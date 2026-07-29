#include "NavigationStore.hpp"

#include "RemotePath.hpp"

#include <QDir>
#include <QFileInfo>
#include <QUrl>
#include <QUrlQuery>

#include <algorithm>
#include <utility>

namespace openscpui {

namespace {

constexpr int kMaximumRecentEntries = 20;
constexpr auto kRecentLocalPathsKey = "History/recentLocalPaths";
constexpr auto kLegacyRemotePathsKey = "History/recentRemotePaths";
constexpr auto kRecentServersKey = "History/recentServers";
constexpr auto kLocalFavoritesKey = "Favorites/localPaths";

} // namespace

NavigationStore::NavigationStore(QString organization, QString application)
    : organization_(std::move(organization)),
      application_(std::move(application)) {
}

NavigationStore::NavigationStore(QString settingsFile, QSettings::Format format)
    : settingsFile_(std::move(settingsFile)), settingsFormat_(format) {
}

NavigationStore NavigationStore::forIniFile(const QString &filePath) {
    return NavigationStore(filePath, QSettings::IniFormat);
}

QString NavigationStore::normalizeLocalPath(const QString &path) {
    QString normalized = QDir::fromNativeSeparators(path.trimmed());
    if (normalized.isEmpty())
        return {};
    normalized = QDir::cleanPath(normalized);
    if (!QFileInfo(normalized).isAbsolute())
        normalized = QDir::current().absoluteFilePath(normalized);
    return QDir(normalized).absolutePath();
}

QString NavigationStore::normalizeRemotePath(const QString &path) {
    return ::normalizeRemotePath(path);
}

QString
NavigationStore::encodeRecentServer(const openscp::SessionOptions &session) {
    QUrlQuery query;
    query.addQueryItem(
        QStringLiteral("protocol"),
        QString::fromLatin1(openscp::protocolStorageName(session.protocol)));
    query.addQueryItem(
        QStringLiteral("host"),
        QString::fromStdString(session.host).trimmed().toLower());
    query.addQueryItem(QStringLiteral("port"), QString::number(session.port));
    query.addQueryItem(QStringLiteral("user"),
                       QString::fromStdString(session.username).trimmed());

    if (session.protocol == openscp::Protocol::Ftps) {
        query.addQueryItem(QStringLiteral("ftpsMode"),
                           QString::fromLatin1(openscp::ftpsModeStorageName(
                               openscp::normalizeFtpsMode(session.ftps_mode))));
    }
    if (session.protocol == openscp::Protocol::WebDav) {
        query.addQueryItem(
            QStringLiteral("webdavScheme"),
            QString::fromLatin1(openscp::webDavSchemeStorageName(
                openscp::normalizeWebDavScheme(session.webdav_scheme))));
        query.addQueryItem(
            QStringLiteral("webdavBasePath"),
            QString::fromStdString(
                openscp::normalizeWebDavBasePath(session.webdav_base_path)));
    }
    return query.toString(QUrl::FullyEncoded);
}

bool NavigationStore::decodeRecentServer(const QString &encoded,
                                         openscp::SessionOptions *session,
                                         QString *label) {
    const QUrlQuery query(encoded);
    const QString host =
        query.queryItemValue(QStringLiteral("host")).trimmed().toLower();
    if (host.isEmpty())
        return false;

    const openscp::Protocol protocol = openscp::protocolFromStorageName(
        query.queryItemValue(QStringLiteral("protocol"))
            .trimmed()
            .toLower()
            .toStdString());
    bool validPort = false;
    int port = query.queryItemValue(QStringLiteral("port"))
                   .trimmed()
                   .toInt(&validPort);
    if (!validPort || port <= 0 || port > 65535) {
        port = static_cast<int>(openscp::defaultPortForProtocol(protocol));
    }
    const QString user = query.queryItemValue(QStringLiteral("user")).trimmed();

    if (session) {
        openscp::SessionOptions decoded;
        decoded.protocol = protocol;
        decoded.host = host.toStdString();
        decoded.port = static_cast<std::uint16_t>(port);
        decoded.username = user.toStdString();
        if (protocol == openscp::Protocol::Ftps) {
            decoded.ftps_mode = openscp::ftpsModeFromStorageName(
                query.queryItemValue(QStringLiteral("ftpsMode"))
                    .trimmed()
                    .toLower()
                    .toStdString());
        }
        if (protocol == openscp::Protocol::WebDav) {
            const QString storedScheme =
                query.queryItemValue(QStringLiteral("webdavScheme"))
                    .trimmed()
                    .toLower();
            if (!storedScheme.isEmpty()) {
                decoded.webdav_scheme = openscp::webDavSchemeFromStorageName(
                    storedScheme.toStdString());
            } else if (decoded.port == openscp::defaultPortForWebDavScheme(
                                           openscp::WebDavScheme::Http)) {
                decoded.webdav_scheme = openscp::WebDavScheme::Http;
            }
            if (decoded.webdav_scheme == openscp::WebDavScheme::Http) {
                decoded.webdav_verify_peer = false;
                decoded.webdav_ca_cert_path.reset();
            }
            decoded.webdav_base_path = openscp::normalizeWebDavBasePath(
                query.queryItemValue(QStringLiteral("webdavBasePath"))
                    .trimmed()
                    .toStdString());
        }
        *session = std::move(decoded);
    }

    if (label) {
        QString endpoint = host;
        if (static_cast<std::uint16_t>(port) !=
            openscp::defaultPortForProtocol(protocol)) {
            endpoint += QStringLiteral(":%1").arg(port);
        }
        if (!user.isEmpty())
            endpoint = QStringLiteral("%1@%2").arg(user, endpoint);
        *label = QStringLiteral("%1  %2").arg(
            QString::fromLatin1(openscp::protocolDisplayName(protocol))
                .toUpper(),
            endpoint);
    }
    return true;
}

void NavigationStore::addRecentLocalPath(const QString &path) {
    const QString normalized = normalizeLocalPath(path);
    if (normalized.isEmpty())
        return;
    auto settings = createSettings();
    QStringList values = settings->value(kRecentLocalPathsKey).toStringList();
    prependRecent(values, normalized);
    settings->setValue(kRecentLocalPathsKey, values);
}

void NavigationStore::addRecentRemotePath(const QString &scope,
                                          const QString &path) {
    const QString key = historyKeyForScope(scope);
    const QString normalized = normalizeRemotePath(path);
    if (key.isEmpty() || normalized.isEmpty())
        return;
    auto settings = createSettings();
    QStringList values = settings->value(key).toStringList();
    prependRecent(values, normalized);
    settings->setValue(key, values);
}

void NavigationStore::addRecentServer(const openscp::SessionOptions &session) {
    if (QString::fromStdString(session.host).trimmed().isEmpty())
        return;
    const QString encoded = encodeRecentServer(session);
    auto settings = createSettings();
    QStringList values = settings->value(kRecentServersKey).toStringList();
    prependRecent(values, encoded);
    settings->setValue(kRecentServersKey, values);
}

QStringList NavigationStore::recentLocalPaths() const {
    return createSettings()->value(kRecentLocalPathsKey).toStringList();
}

QStringList NavigationStore::recentRemotePaths(const QString &scope) const {
    const QString key = historyKeyForScope(scope);
    return key.isEmpty() ? QStringList()
                         : createSettings()->value(key).toStringList();
}

QStringList NavigationStore::legacyRemotePaths() const {
    return createSettings()->value(kLegacyRemotePathsKey).toStringList();
}

QStringList NavigationStore::recentServers() const {
    return createSettings()->value(kRecentServersKey).toStringList();
}

QStringList NavigationStore::favorites(Location location,
                                       const QString &remoteScope) const {
    const QString key = favoritesKey(location, remoteScope);
    return key.isEmpty() ? QStringList()
                         : createSettings()->value(key).toStringList();
}

bool NavigationStore::isFavorite(Location location, const QString &path,
                                 const QString &remoteScope) const {
    const QString normalized = location == Location::Local
                                   ? normalizeLocalPath(path)
                                   : normalizeRemotePath(path);
    if (normalized.isEmpty())
        return false;
    const Qt::CaseSensitivity sensitivity =
        location == Location::Local ? Qt::CaseInsensitive : Qt::CaseSensitive;
    const QStringList values = favorites(location, remoteScope);
    return std::any_of(values.cbegin(), values.cend(),
                       [&](const QString &value) {
                           return value.compare(normalized, sensitivity) == 0;
                       });
}

bool NavigationStore::toggleFavorite(Location location, const QString &path,
                                     const QString &remoteScope) {
    const QString key = favoritesKey(location, remoteScope);
    const QString normalized = location == Location::Local
                                   ? normalizeLocalPath(path)
                                   : normalizeRemotePath(path);
    if (key.isEmpty() || normalized.isEmpty())
        return false;

    auto settings = createSettings();
    QStringList values = settings->value(key).toStringList();
    const Qt::CaseSensitivity sensitivity =
        location == Location::Local ? Qt::CaseInsensitive : Qt::CaseSensitive;
    bool removed = false;
    values.removeIf([&](const QString &value) {
        const bool matches = value.compare(normalized, sensitivity) == 0;
        removed = removed || matches;
        return matches;
    });
    if (!removed)
        values.prepend(normalized);
    settings->setValue(key, values);
    return !removed;
}

void NavigationStore::clearFavorites(Location location,
                                     const QString &remoteScope) {
    const QString key = favoritesKey(location, remoteScope);
    if (!key.isEmpty())
        createSettings()->remove(key);
}

void NavigationStore::clearAllHistory() {
    auto settings = createSettings();
    settings->remove(kRecentLocalPathsKey);
    settings->remove(QStringLiteral("History/remoteScopes"));
    settings->remove(kLegacyRemotePathsKey);
    settings->remove(kRecentServersKey);
}

std::unique_ptr<QSettings> NavigationStore::createSettings() const {
    if (!settingsFile_.isEmpty()) {
        return std::make_unique<QSettings>(settingsFile_, settingsFormat_);
    }
    return std::make_unique<QSettings>(organization_, application_);
}

QString NavigationStore::historyKeyForScope(const QString &scope) {
    const QString normalized = scope.trimmed();
    return normalized.isEmpty()
               ? QString()
               : QStringLiteral("History/remoteScopes/%1/recentPaths")
                     .arg(normalized);
}

QString NavigationStore::favoritesKey(Location location,
                                      const QString &remoteScope) {
    if (location == Location::Local)
        return QString::fromLatin1(kLocalFavoritesKey);
    const QString normalized = remoteScope.trimmed();
    return normalized.isEmpty()
               ? QString()
               : QStringLiteral("Favorites/remoteScopes/%1/paths")
                     .arg(normalized);
}

void NavigationStore::prependRecent(QStringList &values, const QString &value) {
    const QString trimmed = value.trimmed();
    if (trimmed.isEmpty())
        return;
    values.removeAll(trimmed);
    values.prepend(trimmed);
    while (values.size() > kMaximumRecentEntries)
        values.removeLast();
}

} // namespace openscpui
