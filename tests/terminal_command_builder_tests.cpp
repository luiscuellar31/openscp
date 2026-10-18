#include "TestHarness.hpp"
#include "logic/connections/TerminalCommandBuilder.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QList>
#include <QProcess>
#include <QProcessEnvironment>
#include <QStandardPaths>
#include <QStringList>
#include <QTemporaryDir>

#include <functional>
#include <utility>

namespace {

openscp::SessionOptions session() {
    openscp::SessionOptions options;
    options.host = "files.example";
    options.port = 2222;
    options.username = "alice";
    options.password = "never-place-this-in-command";
    options.known_hosts_policy = openscp::KnownHostsPolicy::Strict;
    options.known_hosts_path = "/tmp/known hosts";
    return options;
}

openscpui::TerminalCommandBuilder builder() {
    return openscpui::TerminalCommandBuilder([](const QString &name) {
        if (name == QStringLiteral("ssh") || name == QStringLiteral("sftp") ||
            name == QStringLiteral("nc")) {
            return QStringLiteral("/test tools/") + name;
        }
        return QString();
    });
}

OPENSCP_TEST(testBuildsSshAndFallbackWithoutSecrets, test) {
    const auto result = builder().prepare(
        session(), QStringLiteral("/team/Quarter's reports"), false, true);
    test.check(result.isValid(), "valid sessions should produce a command");
    test.check(result.hasSftpFallback,
               "the optional SFTP fallback should be included when available");
#ifndef Q_OS_WIN
    // testWindowsScriptPassesExactArguments checks the Windows quoting.
    test.check(result.command.contains(QStringLiteral("'2222'")),
               "the selected port should be passed to OpenSSH");
    test.check(result.command.contains(
                   QStringLiteral("UserKnownHostsFile=\"/tmp/known hosts\"")),
               "the known-hosts file should be preserved");
#endif
    test.check(result.command.contains(QStringLiteral("Quarter")),
               "the remote working directory should be preserved");
    test.check(
        !result.command.contains(QStringLiteral("never-place-this-in-command")),
        "stored passwords must never be copied to terminal arguments");
}

#ifndef Q_OS_WIN
// The POSIX shell quoting; on Windows, testWindowsScriptPassesExactArguments
// runs the command and checks every argument that ssh and sftp receive.
OPENSCP_TEST(testKnownHostsFileReachesSshAsOneName, test) {
    // ssh reads the value of -o like a config line: unquoted spaces would
    // split it into several files, and % starts a token in OpenSSH 8.4+.
    auto options = session();
    options.known_hosts_path = "/tmp/known hosts/100%\"x";
    const auto result =
        builder().prepare(options, QStringLiteral("/"), false, false);
    test.check(result.isValid(), "valid sessions should produce a command");
    test.check(result.command.contains(QStringLiteral(
                   "UserKnownHostsFile=\"/tmp/known hosts/100%%\\\"x\"")),
               "the known-hosts file should be quoted and escaped for ssh");
}

OPENSCP_TEST(testSeparatesDestinationsFromOptions, test) {
    // Without "--", ssh and sftp would read this user name as the option
    // -oProxyCommand=... and run its command.
    auto options = session();
    options.username = "-oProxyCommand=touch pwned";
    const auto result =
        builder().prepare(options, QStringLiteral("/"), false, true);
    test.check(result.isValid() && result.hasSftpFallback,
               "an unusual user name should still produce both commands");
    test.check(result.command.contains(QStringLiteral(
                   "'--' '-oProxyCommand=touch pwned@files.example' 'exec ")),
               "ssh should get its destination after --");
    test.check(result.command.contains(QStringLiteral(
                   "'--' '-oProxyCommand=touch pwned@files.example:/'")),
               "sftp should get its destination after --");
}
#endif

OPENSCP_TEST(testRejectsNamesThatOpenSshWouldMisread, test) {
    const auto prepareWith =
        [](const std::function<void(openscp::SessionOptions &)> &edit) {
            auto options = session();
            edit(options);
            return builder().prepare(options, QStringLiteral("/"), false,
                                     false);
        };
    test.check(!prepareWith([](auto &options) {
                    options.host = "-oProxyCommand=x";
                }).isValid(),
               "a host that starts with '-' should be refused");
    test.check(!prepareWith([](auto &options) {
                    options.host = "files.example';touch pwned;'";
                }).isValid(),
               "a host that would leave the quotes around %h should be "
               "refused");
    test.check(!prepareWith([](auto &options) {
                    options.jump_host = "jump.example;touch pwned";
                }).isValid(),
               "ssh puts the jump host unquoted into a shell command");
    test.check(!prepareWith([](auto &options) {
                    options.jump_host = "-oProxyCommand=x";
                }).isValid(),
               "a jump host that starts with '-' should be refused");
    test.check(!prepareWith([](auto &options) {
                    options.jump_host = "jump.example";
                    options.jump_username = "ops admin";
                }).isValid(),
               "a jump user with a space would split the jump command");
    test.check(!prepareWith([](auto &options) {
                    options.jump_host = "jump.example";
                    options.jump_username = "ops,other";
                }).isValid(),
               "ssh splits a jump specification at commas");

#ifndef Q_OS_WIN
    const auto plainJump = prepareWith([](auto &options) {
        options.jump_host = "jump.example";
        options.jump_username = "ops";
    });
    test.check(plainJump.isValid() && plainJump.command.contains(QStringLiteral(
                                          "'-J' 'ops@jump.example'")),
               "ordinary jump hosts should still be accepted");
#endif
}

#ifndef Q_OS_WIN
OPENSCP_TEST(testProxyCommandsKeepPercentSignsLiteral, test) {
    // ssh expands %-tokens in a ProxyCommand, which then sits inside one
    // more pair of single quotes in the terminal command.
    const auto nested = [](QString text) {
        return text.replace(QStringLiteral("'"), QStringLiteral("'\"'\"'"));
    };

    auto jump = session();
    jump.jump_host = "jump.example";
    jump.jump_username = "ops";
    jump.jump_private_key_path = "/keys/100%";
    const auto jumpResult =
        builder().prepare(jump, QStringLiteral("/"), false, false);
    test.check(jumpResult.isValid() &&
                   jumpResult.command.contains(nested(QStringLiteral(
                       "'-i' '/keys/100%%' '-o' 'IdentitiesOnly=yes' '--' "
                       "'jump.example'"))),
               "the jump command should double % and end its options "
               "before the jump host");

    auto proxy = session();
    proxy.proxy_type = openscp::ProxyType::Socks5;
    proxy.proxy_host = "fe80::1%eth0";
    proxy.proxy_port = 1080;
    const auto proxyResult =
        builder().prepare(proxy, QStringLiteral("/"), false, false);
    test.check(proxyResult.isValid() &&
                   proxyResult.command.contains(
                       nested(QStringLiteral("'fe80::1%%eth0:1080'"))),
               "a proxy address with a zone index should keep its %");

    proxy.proxy_host = "fe80::1%1";
    const auto numberedZoneResult =
        builder().prepare(proxy, QStringLiteral("/"), false, false);
    test.check(numberedZoneResult.isValid() &&
                   numberedZoneResult.command.contains(
                       nested(QStringLiteral("'fe80::1%%1:1080'"))),
               "a numbered zone index should not take the port");
}
#endif

OPENSCP_TEST(testRejectsCredentialedProxy, test) {
    auto options = session();
    options.proxy_type = openscp::ProxyType::Socks5;
    options.proxy_host = "proxy.example";
    options.proxy_port = 1080;
    options.proxy_username = "proxy-user";
    options.proxy_password = "proxy-secret";

    const auto result =
        builder().prepare(options, QStringLiteral("/"), false, false);
    test.check(!result.isValid(),
               "credentialed proxies should be rejected for terminal mode");
    test.check(!result.error.isEmpty(),
               "unsafe proxy settings should produce an actionable error");
    test.check(!result.error.contains(QStringLiteral("proxy-secret")),
               "proxy errors must not expose credentials");
}

OPENSCP_TEST(testRejectsAmbiguousNetworkRoute, test) {
    auto options = session();
    options.proxy_type = openscp::ProxyType::HttpConnect;
    options.proxy_host = "proxy.example";
    options.proxy_port = 8080;
    options.jump_host = "jump.example";

    const auto result =
        builder().prepare(options, QStringLiteral("/"), false, false);
    test.check(!result.isValid(),
               "jump hosts and proxies should not be combined implicitly");
}

OPENSCP_TEST(testInteractiveModeDisablesPublicKeys, test) {
    auto options = session();
    options.private_key_path = "/tmp/private key";
    const auto result =
        builder().prepare(options, QStringLiteral("/"), true, false);
    test.check(result.isValid(),
               "interactive login should still produce a valid command");
    test.check(
        result.command.contains(QStringLiteral("PubkeyAuthentication=no")),
        "interactive mode should explicitly disable public-key authentication");
    test.check(!result.command.contains(QStringLiteral("private key")),
               "interactive mode should not pass the saved private key");
}

#ifdef Q_OS_WIN
struct ScriptRun {
    bool finished = false;
    int exitCode = -1;
    QString output;
    // Each program the script started: its name in capitals, then the
    // arguments it received.
    QList<QStringList> programs;
};

QList<QStringList> readProbeLog(const QString &path) {
    QFile log(path);
    if (!log.open(QIODevice::ReadOnly))
        return {};
    QList<QStringList> programs;
    const QString records = QString::fromUtf8(log.readAll());
    for (const QString &record :
         records.split(QChar(0x1e), Qt::SkipEmptyParts)) {
        programs << record.split(QChar(0x1f));
    }
    return programs;
}

QString describe(const QList<QStringList> &programs) {
    QStringList lines;
    for (const QStringList &program : programs) {
        lines << QStringLiteral("[") + program.join(QStringLiteral("|")) +
                     QStringLiteral("]");
    }
    return lines.join(QLatin1Char(' '));
}

// Copies of the argument probe as ssh.exe and sftp.exe, in a folder whose
// name needs quoting both in PowerShell and on a command line.
struct ProbeTools {
    QTemporaryDir root;
    QString ssh;
    QString sftp;
    QString log;

    bool create() {
        if (!root.isValid())
            return false;
        const QString tools =
            QDir(root.path()).filePath(QStringLiteral("O'Brien\u2019s tools"));
        ssh = QDir(tools).filePath(QStringLiteral("ssh.exe"));
        sftp = QDir(tools).filePath(QStringLiteral("sftp.exe"));
        log = QDir(root.path()).filePath(QStringLiteral("programs.log"));
        const QString probe = QStringLiteral(OPENSCP_ARGV_PROBE);
        return QDir().mkpath(tools) && QFile::copy(probe, ssh) &&
               QFile::copy(probe, sftp);
    }

    openscpui::TerminalCommandBuilder builder() const {
        return openscpui::TerminalCommandBuilder([this](const QString &name) {
            if (name == QStringLiteral("ssh"))
                return ssh;
            return name == QStringLiteral("sftp") ? sftp : QString();
        });
    }

    // Runs a prepared script with the arguments that launch() gives
    // powershell.exe, here without a console of its own.
    ScriptRun run(const QString &script,
                  const QList<std::pair<QString, QString>> &environment) const {
        QProcessEnvironment variables =
            QProcessEnvironment::systemEnvironment();
        variables.insert(QStringLiteral("OPENSCP_ARGV_PROBE_LOG"),
                         QDir::toNativeSeparators(log));
        for (const auto &[name, value] : environment)
            variables.insert(name, value);

        QProcess powerShell;
        powerShell.setProcessEnvironment(variables);
        powerShell.setProcessChannelMode(QProcess::MergedChannels);
        powerShell.start(
            qEnvironmentVariable("SystemRoot") +
                QStringLiteral(
                    "/System32/WindowsPowerShell/v1.0/powershell.exe"),
            openscpui::TerminalCommandBuilder::powerShellArguments(script));
        // With no more input, Read-Host returns at once.
        powerShell.closeWriteChannel();

        ScriptRun result;
        result.finished = powerShell.waitForFinished(60000);
        result.exitCode = powerShell.exitCode();
        result.output = QString::fromLocal8Bit(powerShell.readAll());
        result.programs = readProbeLog(log);
        return result;
    }
};

OPENSCP_TEST(testWindowsScriptPassesExactArguments, test) {
    ProbeTools tools;
    test.check(tools.create(), "the probe tools should be created");

    auto options = session();
    // A backslash before a quote is what the C runtime rules treat specially.
    options.username = "CORP\\al\"i\\\"ce";
    options.known_hosts_path = "C:\\Users\\Jane Doe\\100%\\known_hosts";
    options.private_key_path = "C:\\Users\\Jane Doe\\.ssh\\id_ed25519";
    const QString remotePath =
        QStringLiteral("/srv/it's \"quoted\" $HOME \u2018x\u2019 100%");
    const auto result =
        tools.builder().prepare(options, remotePath, false, true);
    test.check(result.isValid() && result.hasSftpFallback,
               "the script with its SFTP fallback should be built");
    test.check(
        !result.command.contains(QStringLiteral("never-place-this-in-command")),
        "stored passwords must never be copied to the script");

    // ssh exits as it does when it cannot connect, so sftp runs as well.
    const ScriptRun run = tools.run(
        result.command, {{QStringLiteral("OPENSCP_ARGV_PROBE_EXIT_SSH"),
                          QStringLiteral("255")}});
    test.check(run.finished && run.exitCode == 0,
               "the script should finish: " + run.output.toStdString());

    const QStringList sharedOptions{
        QStringLiteral("-o"),
        QStringLiteral("StrictHostKeyChecking=yes"),
        QStringLiteral("-o"),
        QStringLiteral(
            "UserKnownHostsFile=\"C:/Users/Jane Doe/100%%/known_hosts\""),
        QStringLiteral("-i"),
        QStringLiteral("C:/Users/Jane Doe/.ssh/id_ed25519"),
        QStringLiteral("-o"),
        QStringLiteral("IdentitiesOnly=yes"),
        QStringLiteral("--"),
    };
    const QString destination =
        QStringLiteral("CORP\\al\"i\\\"ce@files.example");
    const QList<QStringList> expected{
        QStringList{QStringLiteral("SSH"), QStringLiteral("-tt"),
                    QStringLiteral("-p"), QStringLiteral("2222")} +
            sharedOptions +
            QStringList{
                destination,
                QStringLiteral(
                    "exec /bin/sh -c 'cd -- \"$1\" 2>/dev/null || cd /; "
                    "exec \"${SHELL:-/bin/sh}\" -l' sh "
                    "'/srv/it'\"'\"'s \"quoted\" $HOME \u2018x\u2019 100%'")},
        QStringList{QStringLiteral("SFTP"), QStringLiteral("-P"),
                    QStringLiteral("2222")} +
            sharedOptions +
            QStringList{destination + QStringLiteral(":") + remotePath},
    };
    test.check(run.programs == expected,
               "ssh and then sftp should receive exactly these arguments: " +
                   describe(run.programs).toStdString());
    test.check(run.output.contains(QStringLiteral("Falling back to SFTP CLI")),
               "the script should say that it falls back to SFTP");
    test.check(!run.output.contains(QStringLiteral("exit code")),
               "a session that ends well should not hold the window open");
}

OPENSCP_TEST(testWindowsScriptHoldsTheWindowAfterAnError, test) {
    ProbeTools tools;
    test.check(tools.create(), "the probe tools should be created");
    const auto result =
        tools.builder().prepare(session(), QStringLiteral("/"), false, true);
    test.check(result.isValid(), "the script should be built");

    // Only exit code 255, ssh's own failure, falls back to sftp.
    const ScriptRun run = tools.run(
        result.command,
        {{QStringLiteral("OPENSCP_ARGV_PROBE_EXIT_SSH"), QStringLiteral("3")}});
    test.check(run.finished, "Read-Host should return at the end of input");
    test.check(run.programs.size() == 1 &&
                   run.programs.front().value(0) == QStringLiteral("SSH"),
               "only ssh should run: " + describe(run.programs).toStdString());
    test.check(run.output.contains(QStringLiteral("exit code 3")),
               "the window should show the exit code: " +
                   run.output.toStdString());
    // Read-Host shows its prompt only in a console.
    test.check(result.command.contains(QStringLiteral(
                   "Read-Host 'Press Enter to close this window'")),
               "the window should wait for Enter");
}

OPENSCP_TEST(testWindowsUsesTheNulDeviceAndNoNetworkHelpers, test) {
    auto unchecked = session();
    unchecked.known_hosts_policy = openscp::KnownHostsPolicy::Off;
    const auto withoutHostKeys =
        builder().prepare(unchecked, QStringLiteral("/"), false, false);
    test.check(withoutHostKeys.isValid() &&
                   withoutHostKeys.command.contains(
                       QStringLiteral("UserKnownHostsFile=NUL")),
               "Windows has no /dev/null for ssh to write host keys to");

    auto jump = session();
    jump.jump_host = "jump.example";
    jump.jump_username = "ops";
    const auto jumpResult =
        builder().prepare(jump, QStringLiteral("/"), false, false);
    test.check(jumpResult.isValid() && jumpResult.command.contains(
                                           QStringLiteral("ops@jump.example")),
               "a jump host without its own key should go through -J");

    auto proxy = session();
    proxy.proxy_type = openscp::ProxyType::Socks5;
    proxy.proxy_host = "proxy.example";
    proxy.proxy_port = 1080;
    const auto proxyResult =
        builder().prepare(proxy, QStringLiteral("/"), false, false);
    test.check(!proxyResult.isValid() && !proxyResult.error.isEmpty(),
               "proxies should be refused with a reason on Windows");
}

OPENSCP_TEST(testWindowsJumpKeyGoesThroughAQuotedProxyCommand, test) {
    ProbeTools tools;
    test.check(tools.create(), "the probe tools should be created");

    auto options = session();
    options.jump_host = "jump.example";
    options.jump_username = "ops";
    options.jump_private_key_path = "C:\\keys\\100%";
    const auto result =
        tools.builder().prepare(options, QStringLiteral("/"), false, false);
    test.check(result.isValid(), "the script with a jump host should be built");

    const ScriptRun run = tools.run(result.command, {});
    test.check(run.finished,
               "the script should finish: " + run.output.toStdString());
    // OpenSSH for Windows starts the proxy command with CreateProcessW, so it
    // is a command line for the C runtime, not for a shell.
    const QString proxyCommand =
        QStringLiteral("ProxyCommand=\"") +
        QDir::toNativeSeparators(tools.ssh) +
        QStringLiteral("\" -W %h:%p -p 22 -l ops -i C:/keys/100%% -o "
                       "IdentitiesOnly=yes -- jump.example");
    const QStringList ssh = run.programs.value(0);
    test.check(ssh.value(0) == QStringLiteral("SSH") &&
                   ssh.contains(proxyCommand),
               "ssh should get the jump command quoted for the C runtime: " +
                   describe(run.programs).toStdString());
}
#endif

bool writeScript(const QString &path, const QByteArray &body) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly) || file.write(body) != body.size())
        return false;
    file.close();
    return file.setPermissions(QFileDevice::ReadOwner |
                               QFileDevice::WriteOwner | QFileDevice::ExeOwner);
}

OPENSCP_TEST(testCommandsRunUnderCshLoginShells, test) {
    // Terminal on macOS types the command into the user's login shell, and
    // sshd runs the remote part through the server user's login shell; both
    // can be csh-like. Without tcsh there is nothing to run it under.
    const QString tcsh = QStandardPaths::findExecutable(QStringLiteral("tcsh"));
    if (tcsh.isEmpty())
        return;
    QTemporaryDir root;
    test.check(root.isValid(), "the shell test should have a temporary folder");
    if (!root.isValid())
        return;

    const QString target =
        QDir(root.path()).filePath(QStringLiteral("it's $HOME"));
    const QString ssh = QDir(root.path()).filePath(QStringLiteral("ssh"));
    const QString loginShell =
        QDir(root.path()).filePath(QStringLiteral("shell"));
    // The fake ssh hands its last argument to tcsh, as sshd hands the remote
    // command to a tcsh user's login shell; that shell then prints where it
    // started.
    test.check(QDir().mkpath(target) &&
                   writeScript(ssh, "#!/bin/sh\nfor last; do :; done\n"
                                    "exec " +
                                        tcsh.toUtf8() + " -f -c \"$last\"\n") &&
                   writeScript(loginShell, "#!/bin/sh\npwd -P\n"),
               "the shell test fixtures should be created");

    const openscpui::TerminalCommandBuilder fakeTools([&ssh](
                                                          const QString &name) {
        return name == QStringLiteral("ssh") || name == QStringLiteral("sftp")
                   ? ssh
                   : QString();
    });
    const auto result = fakeTools.prepare(session(), target, false, true);
    test.check(result.isValid() && result.hasSftpFallback,
               "the command with its SFTP fallback should be built");

    QProcess localShell;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QStringLiteral("SHELL"), loginShell);
    localShell.setProcessEnvironment(environment);
    localShell.start(
        tcsh,
        {QStringLiteral("-f"), QStringLiteral("-c"),
         openscpui::TerminalCommandBuilder::posixShellCommand(result.command)});
    const bool finished = localShell.waitForFinished(10000);
    const QString output =
        QString::fromUtf8(localShell.readAllStandardOutput()).trimmed();
    test.check(
        finished && output == QDir(target).canonicalPath(),
        "csh-like shells should run the command and open the remote "
        "folder: " +
            output.toStdString() + " " +
            QString::fromUtf8(localShell.readAllStandardError()).toStdString());
}

} // namespace

int main(int argc, char **argv) {
    openscp::test::TestHarness harness("terminal command builder");
    return harness.runWithApplication<QCoreApplication>(argc, argv);
}
