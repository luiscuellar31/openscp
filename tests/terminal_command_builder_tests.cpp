#include "TerminalCommandBuilder.hpp"

#include <QCoreApplication>

#include <iostream>

namespace {

struct TestContext {
    int failures = 0;

    void check(bool condition, const char *message) {
        if (condition)
            return;
        ++failures;
        std::cerr << "[FAIL] " << message << '\n';
    }
};

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

void testBuildsSshAndFallbackWithoutSecrets(TestContext &test) {
    const auto result = builder().prepare(
        session(), QStringLiteral("/team/Quarter's reports"), false, true);
    test.check(result.isValid(), "valid sessions should produce a command");
    test.check(result.hasSftpFallback,
               "the optional SFTP fallback should be included when available");
    test.check(result.command.contains(QStringLiteral("'2222'")),
               "the selected port should be passed to OpenSSH");
    test.check(result.command.contains(QStringLiteral("known hosts")),
               "the known-hosts file should be preserved");
    test.check(result.command.contains(QStringLiteral("Quarter")),
               "the remote working directory should be preserved");
    test.check(
        !result.command.contains(QStringLiteral("never-place-this-in-command")),
        "stored passwords must never be copied to terminal arguments");
}

void testRejectsCredentialedProxy(TestContext &test) {
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

void testRejectsAmbiguousNetworkRoute(TestContext &test) {
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

void testInteractiveModeDisablesPublicKeys(TestContext &test) {
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

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    TestContext test;
    testBuildsSshAndFallbackWithoutSecrets(test);
    testRejectsCredentialedProxy(test);
    testRejectsAmbiguousNetworkRoute(test);
    testInteractiveModeDisablesPublicKeys(test);
    if (test.failures == 0)
        std::cout << "All terminal command builder tests passed\n";
    return test.failures == 0 ? 0 : 1;
}
