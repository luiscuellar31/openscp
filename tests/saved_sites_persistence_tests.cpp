#include "AppSettings.hpp"
#include "SavedSitesPersistence.hpp"
#include "TestHarness.hpp"

#include <QCoreApplication>
#include <QFileInfo>
#include <QSettings>
#include <QTemporaryDir>

#include <iostream>
#include <optional>
#include <string>

namespace {

void writeLegacySites() {
    QSettings settings(QStringLiteral("OpenSCP"), QStringLiteral("OpenSCP"));
    settings.clear();
    settings.beginWriteArray(QStringLiteral("sites"));

    settings.setArrayIndex(0);
    settings.setValue(QStringLiteral("name"), QStringLiteral("Legacy FTPS"));
    settings.setValue(QStringLiteral("protocol"), QStringLiteral("ftps"));
    settings.setValue(QStringLiteral("host"), QStringLiteral("ftp.example"));
    settings.setValue(QStringLiteral("port"), 21);
    settings.setValue(QStringLiteral("user"), QStringLiteral("alice"));

    settings.setArrayIndex(1);
    settings.setValue(QStringLiteral("name"), QStringLiteral("Legacy DAV"));
    settings.setValue(QStringLiteral("protocol"), QStringLiteral("webdav"));
    settings.setValue(QStringLiteral("host"), QStringLiteral("dav.example"));
    settings.setValue(QStringLiteral("port"), 443);
    settings.setValue(QStringLiteral("user"), QStringLiteral("bob"));

    settings.endArray();
    settings.sync();
}

OPENSCP_TEST(testLegacyMigration, test) {
    writeLegacySites();
    int nextId = 0;
    const auto loaded = SavedSitesPersistence::loadSites(
        {.trimSiteNames = true, .createNewId = [&nextId] {
             return QStringLiteral("generated-%1").arg(++nextId);
         }});

    test.check(loaded.needsSave,
               "legacy sites should request a normalized rewrite");
    test.check(loaded.sites.size() == 2,
               "all legacy sites should survive migration");
    if (loaded.sites.size() != 2)
        return;

    const SiteEntry &ftps = loaded.sites[0];
    test.check(ftps.siteId == QStringLiteral("generated-1"),
               "missing site IDs should be generated");
    test.check(ftps.opt.ftps_mode == openscp::FtpsMode::Auto,
               "legacy FTPS sites should migrate to Auto mode");
    test.check(ftps.initialLocalPath.isEmpty(),
               "legacy local roots should keep home/current semantics");
    test.check(ftps.initialRemotePath == QStringLiteral("/"),
               "legacy remote roots should migrate to slash");
    test.check(!ftps.rememberLastPaths,
               "legacy sites should not remember paths by default");

    const SiteEntry &webdav = loaded.sites[1];
    test.check(webdav.siteId == QStringLiteral("generated-2"),
               "each migrated site should receive a distinct ID");
    test.check(webdav.opt.webdav_base_path == "/",
               "legacy WebDAV sites should migrate to a root base path");
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

    const auto saveResult = SavedSitesPersistence::saveSites({site}, true);
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
               "normalized saved sites should not require another migration");
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
    QSettings settings(QStringLiteral("OpenSCP"), QStringLiteral("OpenSCP"));
    settings.clear();
    settings.beginWriteArray(QStringLiteral("sites"));
    for (int siteIndex = 0; siteIndex < 2; ++siteIndex) {
        settings.setArrayIndex(siteIndex);
        settings.setValue(QStringLiteral("id"), QStringLiteral("duplicate-id"));
        settings.setValue(QStringLiteral("name"),
                          QStringLiteral("Site %1").arg(siteIndex));
        settings.setValue(QStringLiteral("initialLocalPath"), QString());
        settings.setValue(QStringLiteral("initialRemotePath"),
                          QStringLiteral("/"));
        settings.setValue(QStringLiteral("rememberLastPaths"), false);
        settings.setValue(QStringLiteral("scpTransferMode"),
                          QStringLiteral("auto"));
        settings.setValue(QStringLiteral("ftpsMode"), QStringLiteral("auto"));
        settings.setValue(QStringLiteral("webdavBasePath"),
                          QStringLiteral("/"));
    }
    settings.endArray();
    settings.sync();

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

OPENSCP_TEST(testLegacySecretsRemainAvailableForSecureMigration, test) {
    QSettings settings(QStringLiteral("OpenSCP"), QStringLiteral("OpenSCP"));
    settings.clear();
    settings.beginWriteArray(QStringLiteral("sites"));
    settings.setArrayIndex(0);
    settings.setValue(QStringLiteral("name"), QStringLiteral("Old site"));
    settings.setValue(QStringLiteral("host"), QStringLiteral("old.example"));
    settings.setValue(QStringLiteral("password"), QStringLiteral("password-1"));
    settings.setValue(QStringLiteral("keyPass"), QStringLiteral("key-pass-1"));
    settings.setValue(QStringLiteral("proxyPass"),
                      QStringLiteral("proxy-pass-1"));
    settings.endArray();
    settings.sync();

    const auto loaded = SavedSitesPersistence::loadSites(
        {.trimSiteNames = false,
         .createNewId = [] { return QStringLiteral("migrated-id"); }});
    test.check(loaded.needsSave,
               "plaintext legacy secrets should request a secure rewrite");
    test.check(loaded.legacySecrets.size() == 3,
               "all supported plaintext legacy secrets should remain available "
               "for secure migration");
    if (loaded.legacySecrets.size() == 3) {
        test.check(
            loaded.legacySecrets[0].siteIndex == 0 &&
                loaded.legacySecrets[0].item == QStringLiteral("password") &&
                loaded.legacySecrets[0].value == QStringLiteral("password-1"),
            "password migration metadata should identify its site and item");
        test.check(loaded.legacySecrets[1].item == QStringLiteral("keypass") &&
                       loaded.legacySecrets[1].value ==
                           QStringLiteral("key-pass-1"),
                   "key passphrase should be captured for secure migration");
        test.check(
            loaded.legacySecrets[2].item == QStringLiteral("proxypass") &&
                loaded.legacySecrets[2].value == QStringLiteral("proxy-pass-1"),
            "proxy password should be captured for secure migration");
    }

    settings.beginReadArray(QStringLiteral("sites"));
    settings.setArrayIndex(0);
    test.check(settings.value(QStringLiteral("password")).toString() ==
                   QStringLiteral("password-1"),
               "loading alone must not destroy a legacy secret before secure "
               "migration succeeds");
    settings.endArray();
}

OPENSCP_TEST(testInitialRemotePathNormalization, test) {
    QSettings settings(QStringLiteral("OpenSCP"), QStringLiteral("OpenSCP"));
    settings.clear();
    settings.beginWriteArray(QStringLiteral("sites"));
    settings.setArrayIndex(0);
    settings.setValue(QStringLiteral("id"), QStringLiteral("path-site"));
    settings.setValue(QStringLiteral("name"), QStringLiteral("Path site"));
    settings.setValue(QStringLiteral("initialLocalPath"), QString());
    settings.setValue(QStringLiteral("initialRemotePath"),
                      QStringLiteral("//projects/./alpha/tmp/../release/"));
    settings.setValue(QStringLiteral("rememberLastPaths"), true);
    settings.setValue(QStringLiteral("scpTransferMode"),
                      QStringLiteral("auto"));
    settings.setValue(QStringLiteral("ftpsMode"), QStringLiteral("auto"));
    settings.setValue(QStringLiteral("webdavBasePath"), QStringLiteral("/"));
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
    SiteEntry site = loaded.sites.front();
    site.initialRemotePath = QStringLiteral("/../../safe/../root");
    const auto saveResult = SavedSitesPersistence::saveSites({site}, true);
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
    QSettings settings(QStringLiteral("OpenSCP"), QStringLiteral("OpenSCP"));
    settings.clear();
    settings.beginWriteArray(QStringLiteral("sites"));
    settings.setArrayIndex(0);
    settings.setValue(QStringLiteral("id"), QStringLiteral("corrupt-site"));
    settings.setValue(QStringLiteral("name"), QStringLiteral("Corrupt site"));
    settings.setValue(QStringLiteral("protocol"), QStringLiteral("sftp"));
    settings.setValue(QStringLiteral("port"), -1);
    settings.setValue(QStringLiteral("proxyType"), 999);
    settings.setValue(QStringLiteral("proxyPort"), 70000);
    settings.setValue(QStringLiteral("jumpPort"), QStringLiteral("invalid"));
    settings.setValue(QStringLiteral("khPolicy"), 999);
    settings.setValue(QStringLiteral("integrityPolicy"), -9);
    settings.setValue(QStringLiteral("initialLocalPath"), QString());
    settings.setValue(QStringLiteral("initialRemotePath"), QStringLiteral("/"));
    settings.setValue(QStringLiteral("rememberLastPaths"), false);
    settings.setValue(QStringLiteral("scpTransferMode"),
                      QStringLiteral("auto"));
    settings.setValue(QStringLiteral("ftpsMode"), QStringLiteral("auto"));
    settings.setValue(QStringLiteral("webdavBasePath"), QStringLiteral("/"));
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

OPENSCP_TEST(testWebDavSecurityDefaultsAreAppliedToLegacySites, test) {
    QSettings settings(QStringLiteral("OpenSCP"), QStringLiteral("OpenSCP"));
    settings.clear();
    settings.setValue(
        QString::fromLatin1(openscpui::settingskeys::kWebDavVerifyPeerDefault),
        false);
    settings.setValue(
        QString::fromLatin1(openscpui::settingskeys::kWebDavCaCertPathDefault),
        QStringLiteral("/configured/ca.pem"));
    settings.beginWriteArray(QStringLiteral("sites"));
    settings.setArrayIndex(0);
    settings.setValue(QStringLiteral("id"), QStringLiteral("legacy-dav"));
    settings.setValue(QStringLiteral("name"), QStringLiteral("Legacy DAV"));
    settings.setValue(QStringLiteral("protocol"), QStringLiteral("webdav"));
    settings.setValue(QStringLiteral("port"), 443);
    settings.setValue(QStringLiteral("initialLocalPath"), QString());
    settings.setValue(QStringLiteral("initialRemotePath"), QStringLiteral("/"));
    settings.setValue(QStringLiteral("rememberLastPaths"), false);
    settings.setValue(QStringLiteral("scpTransferMode"),
                      QStringLiteral("auto"));
    settings.setValue(QStringLiteral("ftpsMode"), QStringLiteral("auto"));
    settings.setValue(QStringLiteral("webdavBasePath"), QStringLiteral("/"));
    settings.endArray();
    settings.sync();

    const auto loaded = SavedSitesPersistence::loadSites();
    test.check(loaded.sites.size() == 1 &&
                   !loaded.sites.front().opt.webdav_verify_peer &&
                   loaded.sites.front().opt.webdav_ca_cert_path ==
                       std::optional<std::string>("/configured/ca.pem"),
               "legacy WebDAV sites should inherit both configured TLS "
               "defaults");
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
