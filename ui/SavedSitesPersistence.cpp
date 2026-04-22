// Shared persistence helpers for saved sites (QSettings array: "sites").
#include "SavedSitesPersistence.hpp"

#include <QSet>
#include <QSettings>
#include <QUuid>

#include <cstdint>

namespace {

std::uint16_t defaultProxyPort(openscp::ProxyType type) {
    return openscp::defaultPortForProxyType(type);
}

std::uint16_t defaultJumpPort() { return 22; }

openscp::ScpTransferMode
loadDefaultScpTransferModeFromSettings(const QSettings &settings) {
    return openscp::scpTransferModeFromStorageName(
        settings
            .value("Protocol/scpTransferModeDefault",
                   QString::fromLatin1(openscp::scpTransferModeStorageName(
                       openscp::ScpTransferMode::Auto)))
            .toString()
            .trimmed()
            .toLower()
            .toStdString());
}

QString fallbackNewSiteId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

QString newSiteId(const SavedSitesPersistence::LoadOptions &options) {
    if (options.createNewId) {
        const QString generated = options.createNewId().trimmed();
        if (!generated.isEmpty())
            return generated;
    }
    return fallbackNewSiteId();
}

QString uniqueSiteId(const SavedSitesPersistence::LoadOptions &options,
                     const QSet<QString> &usedIds) {
    // Keep deterministic behavior for custom generators, then fallback to UUID.
    for (int attempt = 0; attempt < 16; ++attempt) {
        const QString candidate = newSiteId(options);
        if (!candidate.isEmpty() && !usedIds.contains(candidate))
            return candidate;
    }
    QString candidate = fallbackNewSiteId();
    while (usedIds.contains(candidate))
        candidate = fallbackNewSiteId();
    return candidate;
}

} // namespace

SavedSitesPersistence::LoadResult
SavedSitesPersistence::loadSites(const LoadOptions &options) {
    LoadResult result;

    QSettings settings("OpenSCP", "OpenSCP");
    const auto defaultScpMode = loadDefaultScpTransferModeFromSettings(settings);
    const bool defaultFtpsVerifyPeer =
        settings.value("Security/ftpsVerifyPeerDefault", true).toBool();
    const QString defaultFtpsCaPath =
        settings
            .value("Security/ftpsCaCertPathDefault", QString())
            .toString()
            .trimmed();

    const int siteCount = settings.beginReadArray("sites");
    QSet<QString> usedIds;
    for (int i = 0; i < siteCount; ++i) {
        settings.setArrayIndex(i);
        SiteEntry site;

        // Repair missing/duplicate IDs on read to keep each site addressable.
        site.id = settings.value("id").toString().trimmed();
        if (site.id.isEmpty() || usedIds.contains(site.id)) {
            site.id = uniqueSiteId(options, usedIds);
            result.needsSave = true;
        }
        usedIds.insert(site.id);

        site.name = settings.value("name").toString();
        if (options.trimSiteNames)
            site.name = site.name.trimmed();

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
                       QString::fromLatin1(openscp::scpTransferModeStorageName(
                           defaultScpMode)))
                .toString()
                .trimmed()
                .toLower()
                .toStdString());
        // Persist legacy entries with explicit mode so later reads are stable.
        if (!hasScpTransferModeKey)
            result.needsSave = true;

        site.opt.host = settings.value("host").toString().toStdString();
        site.opt.port = static_cast<std::uint16_t>(
            settings
                .value("port", static_cast<int>(openscp::defaultPortForProtocol(
                                   site.opt.protocol)))
                .toUInt());

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
        } else if (
            site.opt.protocol == openscp::Protocol::WebDav &&
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

        site.opt.proxy_type = openscp::proxyTypeFromStorageValue(
            settings
                .value("proxyType", static_cast<int>(openscp::ProxyType::None))
                .toInt());
        site.opt.proxy_host =
            settings.value("proxyHost").toString().trimmed().toStdString();
        site.opt.proxy_port = static_cast<std::uint16_t>(
            settings
                .value("proxyPort",
                       static_cast<int>(defaultProxyPort(site.opt.proxy_type)))
                .toUInt());

        const QString proxyUser = settings.value("proxyUser").toString().trimmed();
        if (!proxyUser.isEmpty())
            site.opt.proxy_username = proxyUser.toStdString();

        const QString jumpHost = settings.value("jumpHost").toString().trimmed();
        if (!jumpHost.isEmpty())
            site.opt.jump_host = jumpHost.toStdString();
        site.opt.jump_port = static_cast<std::uint16_t>(
            settings.value("jumpPort", static_cast<int>(defaultJumpPort()))
                .toUInt());

        const QString jumpUser = settings.value("jumpUser").toString().trimmed();
        if (!jumpUser.isEmpty())
            site.opt.jump_username = jumpUser.toStdString();

        const QString jumpKeyPath = settings.value("jumpKeyPath").toString();
        if (!jumpKeyPath.isEmpty())
            site.opt.jump_private_key_path = jumpKeyPath.toStdString();

        const QString knownHostsPath = settings.value("knownHosts").toString();
        if (!knownHostsPath.isEmpty())
            site.opt.known_hosts_path = knownHostsPath.toStdString();

        site.opt.known_hosts_policy = static_cast<openscp::KnownHostsPolicy>(
            settings
                .value("khPolicy",
                       static_cast<int>(openscp::KnownHostsPolicy::Strict))
                .toInt());
        site.opt.transfer_integrity_policy =
            static_cast<openscp::TransferIntegrityPolicy>(
                settings
                    .value("integrityPolicy",
                           static_cast<int>(
                               openscp::TransferIntegrityPolicy::Optional))
                    .toInt());

        site.opt.ftps_verify_peer =
            settings.value("ftpsVerifyPeer", defaultFtpsVerifyPeer).toBool();
        const QString ftpsCaPath =
            settings.value("ftpsCaCertPath", defaultFtpsCaPath)
                .toString()
                .trimmed();
        if (!ftpsCaPath.isEmpty())
            site.opt.ftps_ca_cert_path = ftpsCaPath.toStdString();

        site.opt.webdav_verify_peer =
            settings.value("webdavVerifyPeer", true).toBool();
        const QString webDavCaPath =
            settings
                .value("webdavCaCertPath", QString())
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

        result.sites.push_back(site);
    }
    settings.endArray();

    return result;
}

void SavedSitesPersistence::saveSites(const QVector<SiteEntry> &sites,
                                      bool syncToDisk) {
    QSettings settings("OpenSCP", "OpenSCP");
    settings.remove("sites");
    settings.beginWriteArray("sites");
    for (int i = 0; i < sites.size(); ++i) {
        settings.setArrayIndex(i);
        const SiteEntry &site = sites[i];
        settings.setValue("id", site.id);
        settings.setValue("name", site.name);
        settings.setValue(
            "protocol",
            QString::fromLatin1(openscp::protocolStorageName(site.opt.protocol)));
        settings.setValue(
            "scpTransferMode",
            QString::fromLatin1(
                openscp::scpTransferModeStorageName(site.opt.scp_transfer_mode)));
        settings.setValue("host", QString::fromStdString(site.opt.host));
        settings.setValue("port", static_cast<int>(site.opt.port));
        settings.setValue(
            "webdavScheme",
            QString::fromLatin1(
                openscp::webDavSchemeStorageName(site.opt.webdav_scheme)));
        settings.setValue("user", QString::fromStdString(site.opt.username));
        settings.setValue(
            "keyPath",
            site.opt.private_key_path
                ? QString::fromStdString(*site.opt.private_key_path)
                : QString());
        settings.setValue("proxyType", static_cast<int>(site.opt.proxy_type));
        settings.setValue("proxyHost", QString::fromStdString(site.opt.proxy_host));
        settings.setValue("proxyPort", static_cast<int>(site.opt.proxy_port));
        settings.setValue(
            "proxyUser",
            site.opt.proxy_username
                ? QString::fromStdString(*site.opt.proxy_username)
                : QString());
        settings.setValue(
            "jumpHost", site.opt.jump_host ? QString::fromStdString(*site.opt.jump_host)
                                           : QString());
        settings.setValue("jumpPort", static_cast<int>(site.opt.jump_port));
        settings.setValue(
            "jumpUser",
            site.opt.jump_username
                ? QString::fromStdString(*site.opt.jump_username)
                : QString());
        settings.setValue(
            "jumpKeyPath",
            site.opt.jump_private_key_path
                ? QString::fromStdString(*site.opt.jump_private_key_path)
                : QString());
        settings.setValue(
            "knownHosts",
            site.opt.known_hosts_path
                ? QString::fromStdString(*site.opt.known_hosts_path)
                : QString());
        settings.setValue("khPolicy",
                          static_cast<int>(site.opt.known_hosts_policy));
        settings.setValue("integrityPolicy",
                          static_cast<int>(site.opt.transfer_integrity_policy));
        settings.setValue("ftpsVerifyPeer", site.opt.ftps_verify_peer);
        settings.setValue(
            "ftpsCaCertPath",
            site.opt.ftps_ca_cert_path
                ? QString::fromStdString(*site.opt.ftps_ca_cert_path)
                : QString());
        settings.setValue("webdavVerifyPeer", site.opt.webdav_verify_peer);
        settings.setValue(
            "webdavCaCertPath",
            site.opt.webdav_ca_cert_path
                ? QString::fromStdString(*site.opt.webdav_ca_cert_path)
                : QString());
    }
    settings.endArray();
    if (syncToDisk)
        settings.sync();
}
