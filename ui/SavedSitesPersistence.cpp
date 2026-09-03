// Shared persistence helpers for saved sites (QSettings array: "sites").
#include "SavedSitesPersistence.hpp"

#include "AppSettings.hpp"
#include "RemotePath.hpp"

#include <QSet>
#include <QUuid>

#include <cstdint>
#include <limits>

namespace {

std::uint16_t defaultProxyPort(openscp::ProxyType type) {
    return openscp::defaultPortForProxyType(type);
}

std::uint16_t defaultJumpPort() {
    return 22;
}

std::uint16_t loadPort(QSettings &settings, const char *key,
                       std::uint16_t fallback, bool allowZero,
                       bool &needsSave) {
    const bool wasPersisted = settings.contains(key);
    bool converted = false;
    const qulonglong raw =
        settings.value(key, static_cast<unsigned int>(fallback))
            .toULongLong(&converted);
    const bool valid = converted &&
                       raw <= std::numeric_limits<std::uint16_t>::max() &&
                       (allowZero || raw != 0);
    if (!valid) {
        if (wasPersisted)
            needsSave = true;
        return fallback;
    }
    return static_cast<std::uint16_t>(raw);
}

openscp::ScpTransferMode
loadDefaultScpTransferModeFromSettings(const QSettings &settings) {
    return openscp::scpTransferModeFromStorageName(
        settings
            .value(openscpui::settingskeys::kDefaultScpTransferMode,
                   QString::fromLatin1(openscp::scpTransferModeStorageName(
                       openscp::ScpTransferMode::Auto)))
            .toString()
            .trimmed()
            .toLower()
            .toStdString());
}

QString generatedSiteId(const SavedSitesPersistence::LoadOptions &options) {
    if (options.createNewId) {
        const QString generated = options.createNewId().trimmed();
        if (!generated.isEmpty())
            return generated;
    }
    return SavedSitesPersistence::createSiteId();
}

QString uniqueSiteId(const SavedSitesPersistence::LoadOptions &options,
                     const QSet<QString> &usedIds) {
    // Keep deterministic behavior for custom generators, then fallback to UUID.
    for (int attempt = 0; attempt < 16; ++attempt) {
        const QString candidate = generatedSiteId(options);
        if (!candidate.isEmpty() && !usedIds.contains(candidate))
            return candidate;
    }
    QString candidate = SavedSitesPersistence::createSiteId();
    while (usedIds.contains(candidate))
        candidate = SavedSitesPersistence::createSiteId();
    return candidate;
}

} // namespace

QString SavedSitesPersistence::createSiteId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

SavedSitesPersistence::LoadResult
SavedSitesPersistence::loadSites(const LoadOptions &options) {
    LoadResult result;

    openscpui::AppSettings settings;
    const auto defaultScpMode =
        loadDefaultScpTransferModeFromSettings(settings);
    const bool defaultFtpsVerifyPeer =
        settings.value(openscpui::settingskeys::kFtpsVerifyPeerDefault, true)
            .toBool();
    const QString defaultFtpsCaPath =
        settings
            .value(openscpui::settingskeys::kFtpsCaCertPathDefault, QString())
            .toString()
            .trimmed();
    const bool defaultWebDavVerifyPeer =
        settings.value(openscpui::settingskeys::kWebDavVerifyPeerDefault, true)
            .toBool();
    const QString defaultWebDavCaPath =
        settings
            .value(openscpui::settingskeys::kWebDavCaCertPathDefault, QString())
            .toString()
            .trimmed();

    const int siteCount =
        settings.beginReadArray(openscpui::settingskeys::kSites);
    QSet<QString> usedIds;
    for (int siteIndex = 0; siteIndex < siteCount; ++siteIndex) {
        settings.setArrayIndex(siteIndex);
        SiteEntry site;

        // Repair missing/duplicate IDs on read to keep each site addressable.
        site.siteId = settings.value("id").toString().trimmed();
        if (site.siteId.isEmpty() || usedIds.contains(site.siteId)) {
            site.siteId = uniqueSiteId(options, usedIds);
            result.needsSave = true;
        }
        usedIds.insert(site.siteId);

        site.name = settings.value("name").toString();
        if (options.trimSiteNames)
            site.name = site.name.trimmed();

        const bool hasInitialLocalPath = settings.contains("initialLocalPath");
        const bool hasInitialRemotePath =
            settings.contains("initialRemotePath");
        const bool hasRememberLastPaths =
            settings.contains("rememberLastPaths");
        site.initialLocalPath =
            settings.value("initialLocalPath", QString()).toString().trimmed();
        site.initialRemotePath =
            settings.value("initialRemotePath", QStringLiteral("/"))
                .toString()
                .trimmed();
        const QString normalizedInitialRemotePath =
            ::normalizeRemotePath(site.initialRemotePath);
        if (site.initialRemotePath != normalizedInitialRemotePath)
            result.needsSave = true;
        site.initialRemotePath = normalizedInitialRemotePath;
        site.rememberLastPaths =
            settings.value("rememberLastPaths", false).toBool();
        if (!hasInitialLocalPath || !hasInitialRemotePath ||
            !hasRememberLastPaths) {
            result.needsSave = true;
        }

        site.opt.protocol = openscp::protocolFromStorageName(
            settings
                .value("protocol",
                       QString::fromLatin1(openscp::protocolStorageName(
                           openscp::Protocol::Sftp)))
                .toString()
                .trimmed()
                .toLower()
                .toStdString());

        const bool hasScpTransferModeKey = settings.contains("scpTransferMode");
        site.opt.scp_transfer_mode = openscp::scpTransferModeFromStorageName(
            settings
                .value("scpTransferMode",
                       QString::fromLatin1(
                           openscp::scpTransferModeStorageName(defaultScpMode)))
                .toString()
                .trimmed()
                .toLower()
                .toStdString());
        // Persist legacy entries with explicit mode so later reads are stable.
        if (!hasScpTransferModeKey)
            result.needsSave = true;

        site.opt.host = settings.value("host").toString().toStdString();
        site.opt.port =
            loadPort(settings, "port",
                     openscp::defaultPortForProtocol(site.opt.protocol), false,
                     result.needsSave);

        const bool hasWebDavSchemeKey = settings.contains("webdavScheme");
        if (hasWebDavSchemeKey) {
            site.opt.webdav_scheme = openscp::webDavSchemeFromStorageName(
                settings
                    .value("webdavScheme",
                           QString::fromLatin1(openscp::webDavSchemeStorageName(
                               openscp::WebDavScheme::Https)))
                    .toString()
                    .trimmed()
                    .toLower()
                    .toStdString());
        } else if (site.opt.protocol == openscp::Protocol::WebDav &&
                   site.opt.port == openscp::defaultPortForWebDavScheme(
                                        openscp::WebDavScheme::Http)) {
            // Legacy WebDAV entries with port 80 are normalized to HTTP.
            site.opt.webdav_scheme = openscp::WebDavScheme::Http;
            result.needsSave = true;
        }

        site.opt.username = settings.value("user").toString().toStdString();

        const QString keyPath = settings.value("keyPath").toString();
        if (!keyPath.isEmpty())
            site.opt.private_key_path = keyPath.toStdString();

        bool proxyTypeConverted = false;
        const int rawProxyType =
            settings
                .value("proxyType", static_cast<int>(openscp::ProxyType::None))
                .toInt(&proxyTypeConverted);
        const auto storedProxyType =
            static_cast<openscp::ProxyType>(rawProxyType);
        site.opt.proxy_type = openscp::normalizeProxyType(storedProxyType);
        if (settings.contains("proxyType") &&
            (!proxyTypeConverted ||
             !openscp::isValidProxyType(storedProxyType))) {
            result.needsSave = true;
        }
        site.opt.proxy_host =
            settings.value("proxyHost").toString().trimmed().toStdString();
        site.opt.proxy_port = loadPort(
            settings, "proxyPort", defaultProxyPort(site.opt.proxy_type),
            site.opt.proxy_type == openscp::ProxyType::None, result.needsSave);
        if (site.opt.proxy_type == openscp::ProxyType::None &&
            site.opt.proxy_port != 0) {
            site.opt.proxy_port = 0;
            result.needsSave = true;
        }

        const QString proxyUser =
            settings.value("proxyUser").toString().trimmed();
        if (!proxyUser.isEmpty())
            site.opt.proxy_username = proxyUser.toStdString();

        const QString jumpHost =
            settings.value("jumpHost").toString().trimmed();
        if (!jumpHost.isEmpty())
            site.opt.jump_host = jumpHost.toStdString();
        site.opt.jump_port = loadPort(settings, "jumpPort", defaultJumpPort(),
                                      false, result.needsSave);

        const QString jumpUser =
            settings.value("jumpUser").toString().trimmed();
        if (!jumpUser.isEmpty())
            site.opt.jump_username = jumpUser.toStdString();

        const QString jumpKeyPath = settings.value("jumpKeyPath").toString();
        if (!jumpKeyPath.isEmpty())
            site.opt.jump_private_key_path = jumpKeyPath.toStdString();

        const QString knownHostsPath = settings.value("knownHosts").toString();
        if (!knownHostsPath.isEmpty())
            site.opt.known_hosts_path = knownHostsPath.toStdString();

        bool knownHostsPolicyConverted = false;
        const int rawKnownHostsPolicy =
            settings
                .value("khPolicy",
                       static_cast<int>(openscp::KnownHostsPolicy::Strict))
                .toInt(&knownHostsPolicyConverted);
        const auto storedKnownHostsPolicy =
            static_cast<openscp::KnownHostsPolicy>(rawKnownHostsPolicy);
        site.opt.known_hosts_policy =
            openscp::normalizeKnownHostsPolicy(storedKnownHostsPolicy);
        if (settings.contains("khPolicy") &&
            (!knownHostsPolicyConverted ||
             !openscp::isValidKnownHostsPolicy(storedKnownHostsPolicy))) {
            result.needsSave = true;
        }

        bool integrityPolicyConverted = false;
        const int rawIntegrityPolicy =
            settings
                .value("integrityPolicy",
                       static_cast<int>(
                           openscp::TransferIntegrityPolicy::Optional))
                .toInt(&integrityPolicyConverted);
        const auto storedIntegrityPolicy =
            static_cast<openscp::TransferIntegrityPolicy>(rawIntegrityPolicy);
        site.opt.transfer_integrity_policy =
            openscp::normalizeTransferIntegrityPolicy(storedIntegrityPolicy);
        if (settings.contains("integrityPolicy") &&
            (!integrityPolicyConverted ||
             !openscp::isValidTransferIntegrityPolicy(storedIntegrityPolicy))) {
            result.needsSave = true;
        }

        site.opt.ftps_verify_peer =
            settings.value("ftpsVerifyPeer", defaultFtpsVerifyPeer).toBool();
        const bool hasFtpsModeKey = settings.contains("ftpsMode");
        site.opt.ftps_mode = openscp::ftpsModeFromStorageName(
            settings.value("ftpsMode", QStringLiteral("auto"))
                .toString()
                .trimmed()
                .toLower()
                .toStdString());
        if (!hasFtpsModeKey)
            result.needsSave = true;
        const QString ftpsCaPath =
            settings.value("ftpsCaCertPath", defaultFtpsCaPath)
                .toString()
                .trimmed();
        if (!ftpsCaPath.isEmpty())
            site.opt.ftps_ca_cert_path = ftpsCaPath.toStdString();

        site.opt.webdav_verify_peer =
            settings.value("webdavVerifyPeer", defaultWebDavVerifyPeer)
                .toBool();
        const bool hasWebDavBasePathKey = settings.contains("webdavBasePath");
        site.opt.webdav_base_path = openscp::normalizeWebDavBasePath(
            settings.value("webdavBasePath", QStringLiteral("/"))
                .toString()
                .trimmed()
                .toStdString());
        if (!hasWebDavBasePathKey)
            result.needsSave = true;
        const QString webDavCaPath =
            settings.value("webdavCaCertPath", defaultWebDavCaPath)
                .toString()
                .trimmed();
        if (!webDavCaPath.isEmpty())
            site.opt.webdav_ca_cert_path = webDavCaPath.toStdString();

        if (site.opt.protocol == openscp::Protocol::WebDav &&
            site.opt.webdav_scheme == openscp::WebDavScheme::Http) {
            // HTTP mode never uses TLS verification fields.
            site.opt.webdav_verify_peer = false;
            site.opt.webdav_ca_cert_path.reset();
        }

        const auto collectLegacySecret = [&](const char *settingsKey,
                                             const char *secretItem) {
            if (!settings.contains(settingsKey))
                return;
            result.needsSave = true;
            const QString value = settings.value(settingsKey).toString();
            if (!value.isEmpty()) {
                result.legacySecrets.push_back(
                    {siteIndex, QString::fromLatin1(secretItem), value});
            }
        };
        collectLegacySecret("password", "password");
        collectLegacySecret("keyPass", "keypass");
        collectLegacySecret("proxyPass", "proxypass");

        result.sites.push_back(site);
    }
    settings.endArray();

    return result;
}

SavedSitesPersistence::SaveResult
SavedSitesPersistence::saveSites(const QVector<SiteEntry> &sites,
                                 bool syncToDisk) {
    openscpui::AppSettings settings;
    settings.remove(openscpui::settingskeys::kSites);
    settings.beginWriteArray(openscpui::settingskeys::kSites);
    for (int siteIndex = 0; siteIndex < sites.size(); ++siteIndex) {
        settings.setArrayIndex(siteIndex);
        const SiteEntry &site = sites[siteIndex];
        settings.setValue("id", site.siteId);
        settings.setValue("name", site.name);
        settings.setValue("initialLocalPath", site.initialLocalPath);
        settings.setValue("initialRemotePath",
                          ::normalizeRemotePath(site.initialRemotePath));
        settings.setValue("rememberLastPaths", site.rememberLastPaths);
        settings.setValue("protocol",
                          QString::fromLatin1(
                              openscp::protocolStorageName(site.opt.protocol)));
        settings.setValue(
            "scpTransferMode",
            QString::fromLatin1(openscp::scpTransferModeStorageName(
                site.opt.scp_transfer_mode)));
        settings.setValue("host", QString::fromStdString(site.opt.host));
        settings.setValue(
            "port", static_cast<int>(site.opt.port != 0
                                         ? site.opt.port
                                         : openscp::defaultPortForProtocol(
                                               site.opt.protocol)));
        settings.setValue("webdavScheme",
                          QString::fromLatin1(openscp::webDavSchemeStorageName(
                              site.opt.webdav_scheme)));
        settings.setValue("user", QString::fromStdString(site.opt.username));
        settings.setValue(
            "keyPath", site.opt.private_key_path
                           ? QString::fromStdString(*site.opt.private_key_path)
                           : QString());
        const openscp::ProxyType proxyType =
            openscp::normalizeProxyType(site.opt.proxy_type);
        settings.setValue("proxyType", static_cast<int>(proxyType));
        settings.setValue("proxyHost",
                          QString::fromStdString(site.opt.proxy_host));
        settings.setValue(
            "proxyPort",
            static_cast<int>(
                proxyType == openscp::ProxyType::None
                    ? 0
                    : (site.opt.proxy_port != 0
                           ? site.opt.proxy_port
                           : openscp::defaultPortForProxyType(proxyType))));
        settings.setValue("proxyUser",
                          site.opt.proxy_username
                              ? QString::fromStdString(*site.opt.proxy_username)
                              : QString());
        settings.setValue("jumpHost",
                          site.opt.jump_host
                              ? QString::fromStdString(*site.opt.jump_host)
                              : QString());
        settings.setValue("jumpPort",
                          static_cast<int>(site.opt.jump_port != 0
                                               ? site.opt.jump_port
                                               : defaultJumpPort()));
        settings.setValue("jumpUser",
                          site.opt.jump_username
                              ? QString::fromStdString(*site.opt.jump_username)
                              : QString());
        settings.setValue(
            "jumpKeyPath",
            site.opt.jump_private_key_path
                ? QString::fromStdString(*site.opt.jump_private_key_path)
                : QString());
        settings.setValue("knownHosts", site.opt.known_hosts_path
                                            ? QString::fromStdString(
                                                  *site.opt.known_hosts_path)
                                            : QString());
        settings.setValue("khPolicy",
                          static_cast<int>(openscp::normalizeKnownHostsPolicy(
                              site.opt.known_hosts_policy)));
        settings.setValue(
            "integrityPolicy",
            static_cast<int>(openscp::normalizeTransferIntegrityPolicy(
                site.opt.transfer_integrity_policy)));
        settings.setValue("ftpsVerifyPeer", site.opt.ftps_verify_peer);
        settings.setValue(
            "ftpsMode", QString::fromLatin1(
                            openscp::ftpsModeStorageName(site.opt.ftps_mode)));
        settings.setValue(
            "ftpsCaCertPath",
            site.opt.ftps_ca_cert_path
                ? QString::fromStdString(*site.opt.ftps_ca_cert_path)
                : QString());
        settings.setValue("webdavVerifyPeer", site.opt.webdav_verify_peer);
        settings.setValue(
            "webdavBasePath",
            QString::fromStdString(
                openscp::normalizeWebDavBasePath(site.opt.webdav_base_path)));
        settings.setValue(
            "webdavCaCertPath",
            site.opt.webdav_ca_cert_path
                ? QString::fromStdString(*site.opt.webdav_ca_cert_path)
                : QString());
    }
    settings.endArray();
    // A SaveResult can only be trustworthy after QSettings has flushed its
    // backend. Keep the parameter for source compatibility while making both
    // call modes report actual persistence failures.
    (void)syncToDisk;
    const auto result = settings.syncSecure();
    return {result.ok, result.error};
}
