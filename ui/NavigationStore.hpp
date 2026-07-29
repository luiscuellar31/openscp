#pragma once

#include "openscp/SessionOptions.hpp"

#include <QSettings>
#include <QString>
#include <QStringList>

#include <memory>

namespace openscpui {

class NavigationStore {
    public:
    enum class Location { Local, Remote };

    NavigationStore(QString organization = QStringLiteral("OpenSCP"),
                    QString application = QStringLiteral("OpenSCP"));

    static NavigationStore forIniFile(const QString &filePath);

    static QString normalizeLocalPath(const QString &path);
    static QString normalizeRemotePath(const QString &path);
    static QString encodeRecentServer(const openscp::SessionOptions &session);
    static bool decodeRecentServer(const QString &encoded,
                                   openscp::SessionOptions *session = nullptr,
                                   QString *label = nullptr);

    void addRecentLocalPath(const QString &path);
    void addRecentRemotePath(const QString &scope, const QString &path);
    void addRecentServer(const openscp::SessionOptions &session);

    QStringList recentLocalPaths() const;
    QStringList recentRemotePaths(const QString &scope) const;
    QStringList legacyRemotePaths() const;
    QStringList recentServers() const;

    QStringList favorites(Location location,
                          const QString &remoteScope = {}) const;
    bool isFavorite(Location location, const QString &path,
                    const QString &remoteScope = {}) const;

    // Returns true when the path was added and false when it was removed or
    // could not be normalized.
    bool toggleFavorite(Location location, const QString &path,
                        const QString &remoteScope = {});
    void clearFavorites(Location location, const QString &remoteScope = {});
    void clearAllHistory();

    private:
    NavigationStore(QString settingsFile, QSettings::Format format);

    std::unique_ptr<QSettings> createSettings() const;
    static QString historyKeyForScope(const QString &scope);
    static QString favoritesKey(Location location, const QString &remoteScope);
    static void prependRecent(QStringList &values, const QString &value);

    QString organization_;
    QString application_;
    QString settingsFile_;
    QSettings::Format settingsFormat_ = QSettings::NativeFormat;
};

} // namespace openscpui
