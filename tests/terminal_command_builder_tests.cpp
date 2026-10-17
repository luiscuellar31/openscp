#include "TestHarness.hpp"
#include "logic/connections/TerminalCommandBuilder.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QProcess>
#include <QStandardPaths>
#include <QTemporaryDir>

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
