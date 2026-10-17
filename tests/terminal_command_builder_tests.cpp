#include "TestHarness.hpp"
#include "logic/connections/TerminalCommandBuilder.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

#include <functional>

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
    test.check(result.command.contains(QStringLiteral("'2222'")),
               "the selected port should be passed to OpenSSH");
    test.check(result.command.contains(
                   QStringLiteral("UserKnownHostsFile=\"/tmp/known hosts\"")),
               "the known-hosts file should be preserved");
    test.check(result.command.contains(QStringLiteral("Quarter")),
               "the remote working directory should be preserved");
    test.check(
        !result.command.contains(QStringLiteral("never-place-this-in-command")),
        "stored passwords must never be copied to terminal arguments");
}

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

    const auto plainJump = prepareWith([](auto &options) {
        options.jump_host = "jump.example";
        options.jump_username = "ops";
    });
    test.check(plainJump.isValid() && plainJump.command.contains(QStringLiteral(
                                          "'-J' 'ops@jump.example'")),
               "ordinary jump hosts should still be accepted");
}

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
}

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
