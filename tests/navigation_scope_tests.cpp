#include "NavigationScope.hpp"

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

openscp::SessionOptions endpoint() {
    openscp::SessionOptions options;
    options.protocol = openscp::Protocol::WebDav;
    options.host = "DAV.Example";
    options.port = 443;
    options.username = "alice";
    options.webdav_scheme = openscp::WebDavScheme::Https;
    options.webdav_base_path = "/remote.php/dav/files/alice";
    return options;
}

void testEndpointIsolation(TestContext &test) {
    const auto base = endpoint();
    const QString scope = openscpui::remoteEndpointScope(base);
    test.check(scope.startsWith(QStringLiteral("endpoint-")) &&
                   scope.size() == 41,
               "endpoint scopes should be opaque fixed-length identifiers");

    auto hostCase = base;
    hostCase.host = "dav.example";
    test.check(openscpui::remoteEndpointScope(hostCase) == scope,
               "host casing should not split one endpoint");

    auto normalizedBase = base;
    normalizedBase.webdav_base_path =
        "//remote.php/./dav/files/team/../alice/";
    test.check(openscpui::remoteEndpointScope(normalizedBase) == scope,
               "equivalent WebDAV base paths should share a scope");

    auto otherBase = base;
    otherBase.webdav_base_path = "/remote.php/dav/files/bob";
    test.check(openscpui::remoteEndpointScope(otherBase) != scope,
               "different WebDAV namespaces must isolate navigation state");

    auto otherScheme = base;
    otherScheme.webdav_scheme = openscp::WebDavScheme::Http;
    test.check(openscpui::remoteEndpointScope(otherScheme) != scope,
               "HTTP and HTTPS endpoints must not share navigation state");

    auto otherUser = base;
    otherUser.username = "bob";
    test.check(openscpui::remoteEndpointScope(otherUser) != scope,
               "different remote accounts must isolate navigation state");

    auto withSecrets = base;
    withSecrets.password = "new-password";
    withSecrets.proxy_password = "proxy-password";
    test.check(openscpui::remoteEndpointScope(withSecrets) == scope,
               "secrets must never influence or leak through endpoint scopes");
}

void testSavedSiteScope(TestContext &test) {
    const QString rawId = QStringLiteral(" legacy/site\\id ");
    const QString scope = openscpui::savedSiteNavigationScope(rawId);
    test.check(scope.startsWith(QStringLiteral("site-")) &&
                   scope.size() == 37,
               "saved-site scopes should be opaque fixed-length identifiers");
    test.check(!scope.contains(QStringLiteral("legacy")) &&
                   !scope.contains(QLatin1Char('/')) &&
                   !scope.contains(QLatin1Char('\\')),
               "legacy site IDs must not create nested QSettings paths");
    test.check(openscpui::savedSiteNavigationScope(rawId.trimmed()) == scope,
               "surrounding whitespace should not split a saved-site scope");
    test.check(openscpui::savedSiteNavigationScope(QStringLiteral("other")) !=
                   scope,
               "different saved-site IDs must isolate navigation state");
    test.check(
        openscpui::savedSiteNavigationScope(
            QStringLiteral("4a1a5e20-9985-4f4a-8584-c9e0f690fc7b")) ==
            QStringLiteral("site-4a1a5e20-9985-4f4a-8584-c9e0f690fc7b"),
        "safe UUID scopes should preserve existing queue/session identities");
    test.check(openscpui::savedSiteNavigationScope(QString()).isEmpty(),
               "an empty saved-site ID must not create a shared scope");
}

void testFtpsModeIsolation(TestContext &test) {
    openscp::SessionOptions explicitTls;
    explicitTls.protocol = openscp::Protocol::Ftps;
    explicitTls.host = "ftp.example";
    explicitTls.port = 2121;
    explicitTls.username = "alice";
    explicitTls.ftps_mode = openscp::FtpsMode::ExplicitTls;

    auto implicitTls = explicitTls;
    implicitTls.ftps_mode = openscp::FtpsMode::ImplicitTls;
    test.check(openscpui::remoteEndpointScope(explicitTls) !=
                   openscpui::remoteEndpointScope(implicitTls),
               "different FTPS modes on one custom port must not share "
               "navigation state");

    auto automatic = explicitTls;
    automatic.ftps_mode = openscp::FtpsMode::Auto;
    test.check(openscpui::remoteEndpointScope(automatic) ==
                   openscpui::remoteEndpointScope(explicitTls),
               "automatic FTPS should share a scope with its effective mode");

    automatic.port =
        openscp::defaultPortForProtocol(openscp::Protocol::Ftps);
    implicitTls.port = automatic.port;
    test.check(openscpui::remoteEndpointScope(automatic) ==
                   openscpui::remoteEndpointScope(implicitTls),
               "automatic FTPS on the implicit default port should share the "
               "implicit endpoint scope");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    TestContext test;
    testEndpointIsolation(test);
    testSavedSiteScope(test);
    testFtpsModeIsolation(test);
    if (test.failures == 0)
        std::cout << "All navigation scope tests passed\n";
    return test.failures == 0 ? 0 : 1;
}
