#pragma once

#include <QSettings>
#include <QString>

namespace openscpui {

namespace settingskeys {

// Stable application-settings schema. Persisted record fields (saved-site
// members, queue JSON and secret identifiers) remain owned by their respective
// serializers; every fixed QSettings path belongs here.
inline constexpr char Sites[] = "sites";
inline constexpr char SitesDeleteSecretsOnRemove[] =
    "Sites/deleteSecretsOnRemove";

inline constexpr char UiLanguage[] = "UI/language";
inline constexpr char UiShowHidden[] = "UI/showHidden";
inline constexpr char UiShowConnectionOnStart[] = "UI/showConnOnStart";
inline constexpr char UiOpenSiteManagerOnDisconnect[] =
    "UI/openSiteManagerOnDisconnect";
inline constexpr char UiSingleClick[] = "UI/singleClick";
inline constexpr char UiOpenBehaviorMode[] = "UI/openBehaviorMode";
inline constexpr char UiOpenRevealInFolder[] = "UI/openRevealInFolder";
inline constexpr char UiOpenBehaviorChosen[] = "UI/openBehaviorChosen";
inline constexpr char UiShowQueueOnEnqueue[] = "UI/showQueueOnEnqueue";
inline constexpr char UiDefaultDownloadDir[] = "UI/defaultDownloadDir";
inline constexpr char MainWindowGeometry[] = "UI/mainWindow/geometry";
inline constexpr char MainWindowState[] = "UI/mainWindow/windowState";
inline constexpr char MainWindowSplitterState[] = "UI/mainWindow/splitterState";
inline constexpr char MainWindowLeftHeaderState[] =
    "UI/mainWindow/leftHeaderState";
inline constexpr char MainWindowRightHeaderLocal[] =
    "UI/mainWindow/rightHeaderLocal";
inline constexpr char MainWindowRightHeaderRemote[] =
    "UI/mainWindow/rightHeaderRemote";
inline constexpr char TransferQueueGeometry[] = "UI/transferQueue/geometry";
inline constexpr char TransferQueueHeaderState[] =
    "UI/transferQueue/headerStateV4";
inline constexpr char TransferQueueFilterMode[] = "UI/transferQueue/filterMode";
inline constexpr char TransferQueueAutoClearMode[] =
    "UI/transferQueue/autoClearMode";
inline constexpr char TransferQueueAutoClearMinutes[] =
    "UI/transferQueue/autoClearMinutes";

inline constexpr char DefaultProtocol[] = "Protocol/defaultProtocol";
inline constexpr char DefaultScpTransferMode[] =
    "Protocol/scpTransferModeDefault";

inline constexpr char DefaultKnownHostsPolicy[] =
    "Security/defaultKnownHostsPolicy";
inline constexpr char DefaultTransferIntegrityPolicy[] =
    "Security/defaultTransferIntegrityPolicy";
inline constexpr char FtpsVerifyPeerDefault[] =
    "Security/ftpsVerifyPeerDefault";
inline constexpr char FtpsCaCertPathDefault[] =
    "Security/ftpsCaCertPathDefault";
inline constexpr char WebDavVerifyPeerDefault[] =
    "Security/webdavVerifyPeerDefault";
inline constexpr char WebDavCaCertPathDefault[] =
    "Security/webdavCaCertPathDefault";
inline constexpr char KnownHostsHashed[] = "Security/knownHostsHashed";
inline constexpr char FingerprintHex[] = "Security/fpHex";
inline constexpr char NoHostVerificationTtlMinutes[] =
    "Security/noHostVerificationTtlMin";
inline constexpr char EnableInsecureSecretFallback[] =
    "Security/enableInsecureSecretFallback";
inline constexpr char MacKeychainRestrictive[] =
    "Security/macKeychainRestrictive";

inline constexpr char TransferMaxConcurrent[] = "Transfer/maxConcurrent";
inline constexpr char TransferGlobalSpeedKbps[] = "Transfer/globalSpeedKBps";
inline constexpr char TransferDefaultQueueAutoClearMode[] =
    "Transfer/defaultQueueAutoClearMode";
inline constexpr char TransferDefaultQueueAutoClearMinutes[] =
    "Transfer/defaultQueueAutoClearMinutes";

inline constexpr char SessionHealthIntervalSeconds[] =
    "Network/sessionHealthIntervalSec";

inline constexpr char StagingRoot[] = "Advanced/stagingRoot";
inline constexpr char StagingPreparationTimeoutMs[] =
    "Advanced/stagingPrepTimeoutMs";
inline constexpr char StagingConfirmationItems[] =
    "Advanced/stagingConfirmItems";
inline constexpr char StagingConfirmationMiB[] = "Advanced/stagingConfirmMiB";
inline constexpr char AutoCleanStaging[] = "Advanced/autoCleanStaging";
inline constexpr char MaximumFolderDepth[] = "Advanced/maxFolderDepth";
inline constexpr char StagingRetentionDays[] = "Advanced/stagingRetentionDays";

inline constexpr char TerminalForceInteractiveLogin[] =
    "Terminal/forceInteractiveLogin";
inline constexpr char TerminalEnableSftpCliFallback[] =
    "Terminal/enableSftpCliFallback";
inline constexpr char ShortcutOpenTransfers[] = "Shortcuts/openTransfers";
inline constexpr char ShortcutOpenHistory[] = "Shortcuts/openHistory";

inline constexpr char SyncFilterPresets[] = "SyncDialog/filterPresetsV1";
inline constexpr char RecentLocalPaths[] = "History/recentLocalPaths";
inline constexpr char LegacyRecentRemotePaths[] = "History/recentRemotePaths";
inline constexpr char RecentServers[] = "History/recentServers";
inline constexpr char RemoteScopes[] = "History/remoteScopes";
inline constexpr char RemoteScopeRecentPathsPattern[] =
    "History/remoteScopes/%1/recentPaths";
inline constexpr char LocalFavorites[] = "Favorites/localPaths";
inline constexpr char RemoteScopeFavoritesPattern[] =
    "Favorites/remoteScopes/%1/paths";

} // namespace settingskeys

struct SettingsSyncResult {
    bool ok = false;
    QString error;
};

class AppSettings final : public QSettings {
    public:
    enum class Store { Application, SecretFallback };

    explicit AppSettings(Store store = Store::Application);
    AppSettings(const QString &organization, const QString &application);
    AppSettings(const QString &fileName, Format format);
    ~AppSettings() override;

    AppSettings(const AppSettings &) = delete;
    AppSettings &operator=(const AppSettings &) = delete;

    [[nodiscard]] SettingsSyncResult syncSecure();
    [[nodiscard]] SettingsSyncResult ensureOwnerOnly() const;
};

} // namespace openscpui
