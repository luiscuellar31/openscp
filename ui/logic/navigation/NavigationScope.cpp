#include "logic/navigation/NavigationScope.hpp"

#include <QCryptographicHash>
#include <QStringList>

#include <algorithm>

namespace openscpui {

namespace {

QString hashedScope(const QString &prefix, const QStringList &identity) {
    const QByteArray digest =
        QCryptographicHash::hash(identity.join(QLatin1Char('\n')).toUtf8(),
                                 QCryptographicHash::Sha256)
            .toHex();
    return prefix + QString::fromLatin1(digest.left(32));
}

} // namespace

QString savedSiteNavigationScope(const QString &siteId) {
    const QString normalized = siteId.trimmed();
    if (normalized.isEmpty())
        return {};
    const bool settingsSafe =
        std::all_of(normalized.cbegin(), normalized.cend(), [](QChar ch) {
            return ch.isLetterOrNumber() || ch == QLatin1Char('-') ||
                   ch == QLatin1Char('_');
        });
    if (settingsSafe)
        return QStringLiteral("site-") + normalized;
    // Keep the QSettings hierarchy confined even if a malformed identifier
    // reaches this boundary. Valid UUID-style identifiers remain readable.
    return hashedScope(QStringLiteral("site-"), {normalized});
}

QString remoteEndpointScope(const openscp::SessionOptions &options) {
    QStringList identity{
        QString::fromLatin1(openscp::protocolStorageName(options.protocol)),
        QString::fromStdString(options.host).trimmed().toLower(),
        QString::number(options.port),
        QString::fromStdString(options.username).trimmed(),
    };

    // A WebDAV base path is a namespace boundary: two accounts exposed by the
    // same HTTP endpoint must never share favorites or history accidentally.
    if (options.protocol == openscp::Protocol::WebDav) {
        identity.push_back(QString::fromLatin1(
            openscp::webDavSchemeStorageName(options.webdav_scheme)));
        identity.push_back(QString::fromStdString(
            openscp::normalizeWebDavBasePath(options.webdav_base_path)));
    }
    if (options.protocol == openscp::Protocol::Ftps) {
        openscp::FtpsMode effectiveMode =
            openscp::normalizeFtpsMode(options.ftps_mode);
        if (effectiveMode == openscp::FtpsMode::Auto) {
            effectiveMode = options.port == openscp::defaultPortForProtocol(
                                                openscp::Protocol::Ftps)
                                ? openscp::FtpsMode::ImplicitTls
                                : openscp::FtpsMode::ExplicitTls;
        }
        identity.push_back(
            QString::fromLatin1(openscp::ftpsModeStorageName(effectiveMode)));
    }

    return hashedScope(QStringLiteral("endpoint-"), identity);
}

} // namespace openscpui
