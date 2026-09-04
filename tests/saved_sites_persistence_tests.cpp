#include "TestHarness.hpp"
#include "logic/common/AppSettings.hpp"
#include "logic/persistence/SavedSitesPersistence.hpp"

#include <QCoreApplication>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>

#include <iostream>
#include <string>

namespace {

void writeIncompletePreReleaseSites() {
    QSettings settings(QStringLiteral("OpenSCP"), QStringLiteral("OpenSCP"));
    settings.clear();
    settings.beginWriteArray(QStringLiteral("sites"));

    settings.setArrayIndex(0);
    settings.setValue(QStringLiteral("name"), QStringLiteral("Beta FTPS"));
    settings.setValue(QStringLiteral("protocol"), QStringLiteral("ftps"));
    settings.setValue(QStringLiteral("host"), QStringLiteral("ftp.example"));
    settings.setValue(QStringLiteral("port"), 21);
    settings.setValue(QStringLiteral("user"), QStringLiteral("alice"));
    settings.setValue(QStringLiteral("password"), QStringLiteral("obsolete"));

    settings.setArrayIndex(1);
    settings.setValue(QStringLiteral("name"), QStringLiteral("Beta DAV"));
    settings.setValue(QStringLiteral("protocol"), QStringLiteral("webdav"));
    settings.setValue(QStringLiteral("host"), QStringLiteral("dav.example"));
    settings.setValue(QStringLiteral("port"), 443);
    settings.setValue(QStringLiteral("user"), QStringLiteral("bob"));

    settings.endArray();
    settings.sync();
}

OPENSCP_TEST(testIncompletePreReleaseSitesAreDiscarded, test) {
    writeIncompletePreReleaseSites();
    const auto loaded = SavedSitesPersistence::loadSites();

    test.check(loaded.needsSave,
               "incomplete pre-release sites should request removal");
    test.check(loaded.sites.isEmpty(),
               "pre-release site formats must not be migrated into 1.0");

    const auto saveResult = SavedSitesPersistence::saveSites(loaded.sites);
    test.check(saveResult.ok,
               "discarded pre-release sites should be removable");
    QSettings settings(QStringLiteral("OpenSCP"), QStringLiteral("OpenSCP"));
    test.check(settings.beginReadArray(QStringLiteral("sites")) == 0,
               "rewriting must remove obsolete site data and plaintext");
    settings.endArray();
}

OPENSCP_TEST(testRoundTrip, test) {
    SiteEntry site;
    site.siteId = QStringLiteral("stable-site-id");
    site.name = QStringLiteral("Production DAV");
    site.opt.protocol = openscp::Protocol::WebDav;
    site.opt.host = "dav.example";
    site.opt.port = 8443;
    site.opt.username = "deploy";
    site.opt.webdav_base_path = "/teams/alpha";
    site.opt.password = "must-not-be-persisted";
    site.opt.private_key_passphrase = "must-not-be-persisted";
    site.opt.proxy_password = "must-not-be-persisted";
    site.initialLocalPath = QStringLiteral("/tmp/project");
    site.initialRemotePath = QStringLiteral("/releases/current");
    site.rememberLastPaths = true;

    const auto saveResult = SavedSitesPersistence::saveSites({site});
    test.check(saveResult.ok,
               std::string("saved sites should report persistence success: ") +
                   saveResult.error.toStdString());
#ifdef Q_OS_UNIX
    openscpui::AppSettings applicationSettings;
    const auto permissions =
        QFileInfo(applicationSettings.fileName()).permissions();
    constexpr QFileDevice::Permissions nonOwnerPermissions =
        QFileDevice::ReadGroup | QFileDevice::WriteGroup |
        QFileDevice::ExeGroup | QFileDevice::ReadOther |
        QFileDevice::WriteOther | QFileDevice::ExeOther;
    test.check((permissions & nonOwnerPermissions) == 0,
               "saved-site settings should use owner-only permissions");
#endif
    const auto loaded = SavedSitesPersistence::loadSites();
    test.check(!loaded.needsSave,
               "current saved sites should not require a normalized rewrite");
    test.check(loaded.sites.size() == 1,
               "a saved site should round-trip exactly once");
    if (loaded.sites.size() != 1)
        return;

    const SiteEntry &restored = loaded.sites.front();
    test.check(restored.siteId == site.siteId && restored.name == site.name,
               "site identity should survive persistence");
    test.check(restored.initialLocalPath == site.initialLocalPath &&
                   restored.initialRemotePath == site.initialRemotePath &&
                   restored.rememberLastPaths,
               "initial and remembered paths should survive persistence");
    test.check(restored.opt.webdav_base_path == "/teams/alpha",
               "WebDAV base path should survive persistence");
    test.check(!restored.opt.password && !restored.opt.private_key_passphrase &&
                   !restored.opt.proxy_password,
               "saved-site metadata must never round-trip credentials through "
               "QSettings");

    QSettings raw(QStringLiteral("OpenSCP"), QStringLiteral("OpenSCP"));
    raw.beginReadArray(QStringLiteral("sites"));
    raw.setArrayIndex(0);
    test.check(!raw.contains(QStringLiteral("password")) &&
                   !raw.contains(QStringLiteral("keyPass")) &&
                   !raw.contains(QStringLiteral("proxyPass")),
               "saved-site writes must not create plaintext credential keys");
    raw.endArray();
}

OPENSCP_TEST(testDuplicateIdsAreRepaired, test) {
    SiteEntry first;
    first.siteId = QStringLiteral("duplicate-id");
    first.name = QStringLiteral("Site 0");
    SiteEntry second = first;
    second.name = QStringLiteral("Site 1");
    test.check(SavedSitesPersistence::saveSites({first, second}).ok,
               "duplicate-ID fixture should use the current format");

    int generated = 0;
    const auto loaded = SavedSitesPersistence::loadSites(
        {.trimSiteNames = false, .createNewId = [&generated] {
             return QStringLiteral("replacement-%1").arg(++generated);
         }});
    test.check(loaded.needsSave,
               "duplicate saved-site IDs should request a rewrite");
    test.check(loaded.sites.size() == 2 &&
                   loaded.sites[0].siteId == QStringLiteral("duplicate-id") &&
                   loaded.sites[1].siteId == QStringLiteral("replacement-1"),
               "the first stable ID should survive and later duplicates should "
               "receive new identities");
}

OPENSCP_TEST(testInitialRemotePathNormalization, test) {
    SiteEntry site;
    site.siteId = QStringLiteral("path-site");
    site.name = QStringLiteral("Path site");
    test.check(SavedSitesPersistence::saveSites({site}).ok,
               "path fixture should use the current format");
    QSettings settings(QStringLiteral("OpenSCP"), QStringLiteral("OpenSCP"));
    settings.beginWriteArray(QStringLiteral("sites"));
    settings.setArrayIndex(0);
    settings.setValue(QStringLiteral("initialRemotePath"),
                      QStringLiteral("//projects/./alpha/tmp/../release/"));
    settings.endArray();
    settings.sync();

    const auto loaded = SavedSitesPersistence::loadSites();
    test.check(loaded.sites.size() == 1,
               "path normalization fixture should load one site");
    if (loaded.sites.size() == 1) {
        test.check(
            loaded.sites.front().initialRemotePath ==
                QStringLiteral("/projects/alpha/release"),
            "initial remote paths should collapse duplicate separators and dot "
            "segments");
    }
    test.check(loaded.needsSave,
               "non-canonical initial remote paths should request a rewrite");

    if (loaded.sites.isEmpty())
        return;
    SiteEntry normalizedSite = loaded.sites.front();
    normalizedSite.initialRemotePath = QStringLiteral("/../../safe/../root");
    const auto saveResult = SavedSitesPersistence::saveSites({normalizedSite});
    test.check(saveResult.ok,
               std::string("normalized paths should save successfully: ") +
                   saveResult.error.toStdString());
    const auto restored = SavedSitesPersistence::loadSites();
    test.check(restored.sites.size() == 1 &&
                   restored.sites.front().initialRemotePath ==
                       QStringLiteral("/root"),
               "saved initial paths should remain confined to the remote root");
}

OPENSCP_TEST(testCorruptSecurityAndPortValuesAreRepaired, test) {
    SiteEntry site;
    site.siteId = QStringLiteral("corrupt-site");
    site.name = QStringLiteral("Corrupt site");
    test.check(SavedSitesPersistence::saveSites({site}).ok,
               "corruption fixture should use the current format");
    QSettings settings(QStringLiteral("OpenSCP"), QStringLiteral("OpenSCP"));
    settings.beginWriteArray(QStringLiteral("sites"));
    settings.setArrayIndex(0);
    settings.setValue(QStringLiteral("port"), -1);
    settings.setValue(QStringLiteral("proxyType"), 999);
    settings.setValue(QStringLiteral("proxyPort"), 70000);
    settings.setValue(QStringLiteral("jumpPort"), QStringLiteral("invalid"));
    settings.setValue(QStringLiteral("khPolicy"), 999);
    settings.setValue(QStringLiteral("integrityPolicy"), -9);
    settings.endArray();
    settings.sync();

    const auto loaded = SavedSitesPersistence::loadSites();
    test.check(loaded.needsSave,
               "corrupt persisted values should request a repaired rewrite");
    test.check(loaded.sites.size() == 1,
               "a repairable site should remain available");
    if (loaded.sites.size() != 1)
        return;

    const auto &options = loaded.sites.front().opt;
    test.check(options.port == 22 && options.jump_port == 22,
               "invalid endpoint ports should use protocol-safe defaults");
    test.check(options.proxy_type == openscp::ProxyType::None &&
                   options.proxy_port == 0,
               "an invalid proxy configuration should normalize to disabled");
    test.check(options.known_hosts_policy == openscp::KnownHostsPolicy::Strict,
               "corrupt host verification must normalize to Strict");
    test.check(options.transfer_integrity_policy ==
                   openscp::TransferIntegrityPolicy::Optional,
               "corrupt integrity policy should use its documented default");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setOrganizationName(QStringLiteral("OpenSCP-tests"));
    QCoreApplication::setApplicationName(
        QStringLiteral("saved-sites-persistence-tests"));

    QTemporaryDir settingsRoot;
    if (!settingsRoot.isValid()) {
        std::cerr << "[FAIL] could not create isolated settings directory\n";
        return 1;
    }
    QSettings::setDefaultFormat(QSettings::IniFormat);
    QSettings::setPath(QSettings::IniFormat, QSettings::UserScope,
                       settingsRoot.path());

    openscp::test::TestHarness harness("saved-sites persistence");
    const int result = harness.run();
    QSettings(QStringLiteral("OpenSCP"), QStringLiteral("OpenSCP")).clear();
    return result;
}
