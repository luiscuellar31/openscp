#pragma once

#include <QSettings>
#include <QString>

namespace openscpui {

namespace settingskeys {

// Stable application-settings schema. Persisted record fields (saved-site
// members, queue JSON and secret identifiers) remain owned by their respective
// serializers; every fixed QSettings path belongs here.
inline constexpr char kSites[] = "sites";
inline constexpr char kSitesDeleteSecretsOnRemove[] =
    "Sites/deleteSecretsOnRemove";

inline constexpr char kUiLanguage[] = "UI/language";
inline constexpr char kUiShowHidden[] = "UI/showHidden";
inline constexpr char kUiShowConnectionOnStart[] = "UI/showConnOnStart";
inline constexpr char kUiOpenSiteManagerOnDisconnect[] =
    "UI/openSiteManagerOnDisconnect";
inline constexpr char kUiSingleClick[] = "UI/singleClick";
inline constexpr char kUiOpenBehaviorMode[] = "UI/openBehaviorMode";
inline constexpr char kUiShowQueueOnEnqueue[] = "UI/showQueueOnEnqueue";
inline constexpr char kUiDefaultDownloadDir[] = "UI/defaultDownloadDir";
inline constexpr char kMainWindowGeometry[] = "UI/mainWindow/geometry";
inline constexpr char kMainWindowState[] = "UI/mainWindow/windowState";
inline constexpr char kMainWindowSplitterState[] =
    "UI/mainWindow/splitterState";
inline constexpr char kMainWindowLeftHeaderState[] =
    "UI/mainWindow/leftHeaderState";
inline constexpr char kMainWindowRightHeaderLocal[] =
    "UI/mainWindow/rightHeaderLocal";
inline constexpr char kMainWindowRightHeaderRemote[] =
    "UI/mainWindow/rightHeaderRemote";
inline constexpr char kTransferQueueGeometry[] = "UI/transferQueue/geometry";
inline constexpr char kTransferQueueHeaderState[] =
    "UI/transferQueue/headerStateV4";
inline constexpr char kTransferQueueFilterMode[] =
    "UI/transferQueue/filterMode";
inline constexpr char kTransferQueueAutoClearMode[] =
    "UI/transferQueue/autoClearMode";
inline constexpr char kTransferQueueAutoClearMinutes[] =
    "UI/transferQueue/autoClearMinutes";

inline constexpr char kDefaultProtocol[] = "Protocol/defaultProtocol";
inline constexpr char kDefaultScpTransferMode[] =
    "Protocol/scpTransferModeDefault";

inline constexpr char kDefaultKnownHostsPolicy[] =
    "Security/defaultKnownHostsPolicy";
inline constexpr char kDefaultTransferIntegrityPolicy[] =
    "Security/defaultTransferIntegrityPolicy";
inline constexpr char kFtpsVerifyPeerDefault[] =
    "Security/ftpsVerifyPeerDefault";
inline constexpr char kFtpsCaCertPathDefault[] =
    "Security/ftpsCaCertPathDefault";
inline constexpr char kWebDavVerifyPeerDefault[] =
    "Security/webdavVerifyPeerDefault";
inline constexpr char kWebDavCaCertPathDefault[] =
    "Security/webdavCaCertPathDefault";
inline constexpr char kKnownHostsHashed[] = "Security/knownHostsHashed";
inline constexpr char kFingerprintHex[] = "Security/fpHex";
inline constexpr char kNoHostVerificationTtlMinutes[] =
    "Security/noHostVerificationTtlMin";
inline constexpr char kEnableInsecureSecretFallback[] =
    "Security/enableInsecureSecretFallback";
inline constexpr char kMacKeychainRestrictive[] =
    "Security/macKeychainRestrictive";

inline constexpr char kTransferMaxConcurrent[] = "Transfer/maxConcurrent";
inline constexpr char kTransferGlobalSpeedKbps[] = "Transfer/globalSpeedKBps";
inline constexpr char kTransferDefaultQueueAutoClearMode[] =
    "Transfer/defaultQueueAutoClearMode";
inline constexpr char kTransferDefaultQueueAutoClearMinutes[] =
    "Transfer/defaultQueueAutoClearMinutes";

inline constexpr char kSessionHealthIntervalSeconds[] =
    "Network/sessionHealthIntervalSec";

inline constexpr char kStagingRoot[] = "Advanced/stagingRoot";
inline constexpr char kStagingPreparationTimeoutMs[] =
    "Advanced/stagingPrepTimeoutMs";
inline constexpr char kStagingConfirmationItems[] =
    "Advanced/stagingConfirmItems";
inline constexpr char kStagingConfirmationMiB[] = "Advanced/stagingConfirmMiB";
inline constexpr char kAutoCleanStaging[] = "Advanced/autoCleanStaging";
inline constexpr char kMaximumFolderDepth[] = "Advanced/maxFolderDepth";
inline constexpr char kStagingRetentionDays[] = "Advanced/stagingRetentionDays";

inline constexpr char kTerminalForceInteractiveLogin[] =
    "Terminal/forceInteractiveLogin";
inline constexpr char kTerminalEnableSftpCliFallback[] =
    "Terminal/enableSftpCliFallback";
inline constexpr char kShortcutOpenTransfers[] = "Shortcuts/openTransfers";
inline constexpr char kShortcutOpenHistory[] = "Shortcuts/openHistory";

inline constexpr char kSyncFilterPresets[] = "SyncDialog/filterPresetsV1";
inline constexpr char kRecentLocalPaths[] = "History/recentLocalPaths";
inline constexpr char kRecentServers[] = "History/recentServers";
inline constexpr char kRemoteScopes[] = "History/remoteScopes";
inline constexpr char kRemoteScopeRecentPathsPattern[] =
    "History/remoteScopes/%1/recentPaths";
inline constexpr char kLocalFavorites[] = "Favorites/localPaths";
inline constexpr char kRemoteScopeFavoritesPattern[] =
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
