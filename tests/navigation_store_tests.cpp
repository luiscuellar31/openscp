#include "TestHarness.hpp"
#include "logic/navigation/NavigationStore.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>

namespace {

OPENSCP_TEST(testScopedFavorites, test) {
    QTemporaryDir directory;
    test.check(directory.isValid(),
               "scoped favorites should have a temporary settings directory");
    if (!directory.isValid())
        return;
    auto store = openscpui::NavigationStore::forIniFile(
        directory.filePath(QStringLiteral("navigation.ini")));
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
#if defined(Q_OS_WIN) || defined(Q_OS_MACOS)
    test.check(store.isFavorite(Location::Local, local.toUpper()),
               "local favorites should match without regard to case");
#else
    // Marking ~/docs used to remove the star from ~/Docs on Linux.
    test.check(!store.isFavorite(Location::Local, local.toUpper()),
               "local favorites that differ in case should stay apart");
    test.check(store.toggleFavorite(Location::Local, local.toUpper()) &&
                   store.isFavorite(Location::Local, local),
               "adding a favorite that differs in case should keep the other");
#endif
}

OPENSCP_TEST(testRecentHistory, test) {
    QTemporaryDir directory;
    test.check(directory.isValid(),
               "recent history should have a temporary settings directory");
    if (!directory.isValid())
        return;
    auto store = openscpui::NavigationStore::forIniFile(
        directory.filePath(QStringLiteral("navigation.ini")));
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

OPENSCP_TEST(testFailedSavesAreReportedOnceUntilOneWorks, test) {
    // A path or favorite that could not be saved was dropped in silence and
    // missing at the next start.
    QTemporaryDir directory;
    test.check(directory.isValid(),
               "save failures should have a temporary settings directory");
    if (!directory.isValid())
        return;
    const QString blocker = directory.filePath(QStringLiteral("blocker"));
    const auto blockFolder = [&blocker] {
        QFile file(blocker);
        return file.open(QIODevice::WriteOnly);
    };
    test.check(blockFolder(), "a file should take the folder's place");
    auto store = openscpui::NavigationStore::forIniFile(
        QDir(blocker).filePath(QStringLiteral("navigation.ini")));
    QStringList errors;
    store.setSaveFailureHandler(
        [&errors](const QString &error) { errors.push_back(error); });
    using Location = openscpui::NavigationStore::Location;

    store.addRecentLocalPath(directory.path());
    (void)store.toggleFavorite(Location::Remote, QStringLiteral("/team"),
                               QStringLiteral("server-a"));
    test.check(errors.size() == 1 && !errors.front().isEmpty(),
               "a failed save should be reported once while saves fail");

    test.check(QFile::remove(blocker) && QDir().mkpath(blocker),
               "the folder should become writable");
    store.addRecentLocalPath(directory.path());
    test.check(errors.size() == 1, "a save that works should report nothing");

    test.check(QDir(blocker).removeRecursively() && blockFolder(),
               "the folder should be blocked again");
    store.addRecentLocalPath(directory.path());
    test.check(errors.size() == 2,
               "a failure after a save that worked should be reported again");
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
    openscp::SessionOptions literal = original;
    literal.webdav_base_path = "/dav/a%20b";
    openscp::SessionOptions literalDecoded;
    test.check(openscpui::NavigationStore::decodeRecentServer(
                   openscpui::NavigationStore::encodeRecentServer(literal),
                   &literalDecoded, nullptr) &&
                   literalDecoded.webdav_base_path == "/dav/a%20b",
               "a folder named with a percent escape should survive the "
               "round trip");
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
    openscp::test::TestHarness harness("navigation store");
    return harness.run(argc, argv);
}
