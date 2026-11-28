#pragma once

#include "logic/common/AppSettings.hpp"
#include "openscp/SessionOptions.hpp"

#include <QString>
#include <QStringList>

#include <functional>
#include <memory>

namespace openscpui {

class NavigationStore {
    public:
    enum class Location { Local, Remote };

    // Empty names follow the running application, matching AppSettings, so a
    // build that renames itself keeps its history in its own store.
    NavigationStore(QString organization = QString(),
                    QString application = QString());

    static NavigationStore forIniFile(const QString &filePath);

    static QString normalizeLocalPath(const QString &path);
    static QString normalizeRemotePath(const QString &path);
    // Whether two spellings of a path that differ only in case are one place.
    static Qt::CaseSensitivity pathCaseSensitivity(Location location);
    static QString encodeRecentServer(const openscp::SessionOptions &session);
    static bool decodeRecentServer(const QString &encoded,
                                   openscp::SessionOptions *session = nullptr,
                                   QString *label = nullptr);

    void addRecentLocalPath(const QString &path);
    void addRecentRemotePath(const QString &scope, const QString &path);
    void addRecentServer(const openscp::SessionOptions &session);

    QStringList recentLocalPaths() const;
    QStringList recentRemotePaths(const QString &scope) const;
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

    // Called with the error when history or favorites could not be saved,
    // once until a save succeeds again, so that a settings file that cannot
    // be written does not warn on every navigation.
    void setSaveFailureHandler(std::function<void(const QString &)> handler);

    private:
    NavigationStore(QString settingsFile, AppSettings::Format format);

    std::unique_ptr<AppSettings> createSettings() const;
    void finishSave(AppSettings &settings);
    static QString historyKeyForScope(const QString &scope);
    static QString favoritesKey(Location location, const QString &remoteScope);
    static void prependRecent(QStringList &values, const QString &value);

    QString organization_;
    QString application_;
    QString settingsFile_;
    AppSettings::Format settingsFormat_ = AppSettings::NativeFormat;
    std::function<void(const QString &)> onSaveFailure_;
    bool saveFailureReported_ = false;
};

} // namespace openscpui
