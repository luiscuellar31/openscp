#include "TestHarness.hpp"
#include "logic/navigation/NavigationStore.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QTemporaryDir>

#include <iostream>

namespace {

void testScopedFavorites(TestContext &test, openscpui::NavigationStore &store) {
    using Location = openscpui::NavigationStore::Location;

    test.check(store.toggleFavorite(Location::Remote, QStringLiteral("/team"),
                                    QStringLiteral("server-a")),
               "the first toggle should add a remote favorite");
    test.check(store.isFavorite(Location::Remote, QStringLiteral("/team/"),
                                QStringLiteral("server-a")),
               "equivalent remote paths should match");
    test.check(!store.isFavorite(Location::Remote, QStringLiteral("/team"),
                                 QStringLiteral("server-b")),
               "remote favorites must be isolated by connection scope");
    test.check(!store.toggleFavorite(Location::Remote, QStringLiteral("/team"),
                                     QStringLiteral("server-a")),
               "the second toggle should remove the favorite");

    const QString local = QDir::temp().filePath(QStringLiteral("OpenSCP"));
    test.check(store.toggleFavorite(Location::Local, local),
               "local favorites should not require a remote scope");
    test.check(store.isFavorite(Location::Local, local.toUpper()),
               "local favorite matching should be case insensitive");
}

void testRecentHistory(TestContext &test, openscpui::NavigationStore &store) {
    for (int index = 0; index < 25; ++index) {
        store.addRecentRemotePath(QStringLiteral("server-a"),
                                  QStringLiteral("/entry-%1").arg(index));
    }
    const QStringList recent =
        store.recentRemotePaths(QStringLiteral("server-a"));
    test.check(recent.size() == 20, "recent history should remain bounded");
    test.check(recent.first() == QStringLiteral("/entry-24"),
               "the newest path should appear first");
    test.check(store.recentRemotePaths(QStringLiteral("server-b")).isEmpty(),
               "history should be isolated by connection scope");

    store.clearAllHistory();
    test.check(store.recentRemotePaths(QStringLiteral("server-a")).isEmpty(),
               "clearing history should include inactive remote scopes");
}

OPENSCP_TEST(testServerRoundTripDoesNotPersistSecrets, test) {
    openscp::SessionOptions original;
    original.protocol = openscp::Protocol::WebDav;
    original.host = "DAV.Example";
    original.port = 8443;
    original.username = "alice";
    original.password = "secret";
    original.webdav_scheme = openscp::WebDavScheme::Https;
    original.webdav_base_path = "/dav/alice";

    const QString encoded =
        openscpui::NavigationStore::encodeRecentServer(original);
    test.check(!encoded.contains(QStringLiteral("secret")),
               "recent server entries must not contain credentials");

    openscp::SessionOptions decoded;
    QString label;
    test.check(openscpui::NavigationStore::decodeRecentServer(encoded, &decoded,
                                                              &label),
               "valid recent server entries should decode");
    test.check(decoded.host == "dav.example" && decoded.port == 8443 &&
                   decoded.username == "alice",
               "endpoint identity should survive the round trip");
    test.check(decoded.webdav_base_path == "/dav/alice",
               "the WebDAV namespace should survive the round trip");
    test.check(!decoded.password.has_value(),
               "decoded history must never manufacture credentials");
    test.check(label.contains(QStringLiteral("alice@dav.example:8443")),
               "the user-facing endpoint label should remain descriptive");

    const QString incomplete =
        QStringLiteral("protocol=webdav&host=dav.example&port=443&user=alice");
    test.check(!openscpui::NavigationStore::decodeRecentServer(incomplete),
               "pre-release server entries with missing fields must be "
               "discarded");

    const QString numericFallback =
        QStringLiteral("protocol=sftp&host=ssh.example&port=invalid&user=bob");
    test.check(!openscpui::NavigationStore::decodeRecentServer(numericFallback),
               "invalid current server entries must not invent a default "
               "port");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QTemporaryDir directory;
    if (!directory.isValid()) {
        std::cerr << "[FAIL] could not create temporary settings directory\n";
        return 1;
    }

    auto store = openscpui::NavigationStore::forIniFile(
        directory.filePath(QStringLiteral("navigation.ini")));
    openscp::test::TestHarness harness("navigation store");
    harness.add("scoped favorites", [&store](TestContext &test) {
        testScopedFavorites(test, store);
    });
    harness.add("recent history", [&store](TestContext &test) {
        testRecentHistory(test, store);
    });
    return harness.run();
}
