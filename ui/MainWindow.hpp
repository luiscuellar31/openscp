// Declaration of the main window and its state/actions.
#pragma once
#include "ConnectionStatusCoordinator.hpp"
#include "HostKeyPromptCoordinator.hpp"
#include "NavigationStore.hpp"
#include "SessionHealthMonitor.hpp"
#include "TransferUiController.hpp"
#include "openscp/SessionOptions.hpp"

#include <QAction>
#include <QDir>
#include <QFileSystemModel>
#include <QMainWindow>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTreeView>
#include <QVector>

#include <atomic>
#include <functional>
#include <memory>
#include <optional>
#include <string>

class RemoteModel; // fwd
class RemoteOperationController;
class LocalTreeDiscovery;
class SyncCoordinator;
namespace openscpui {
class FocusTraversalController;
class PaneController;
class PathNavigationBar;
class PlatformFilePicker;
struct PathActionResult;
class RemoteActionController;
class SessionController;
} // namespace openscpui
struct SiteEntry;     // fwd
class QModelIndex;    // fwd for slot signatures
class QToolBar;       // fwd
class QMenu;          // fwd
class QEvent;         // fwd for eventFilter
class QCloseEvent;    // fwd for closeEvent
class QDialog;        // fwd
class QLabel;         // fwd
class QStackedWidget; // fwd
class QTimer;         // fwd
class QSplitter;      // fwd
class QPushButton;    // fwd
class QProgressDialog;
namespace openscp {
class RemoteClient;
struct SessionOptions;
} // namespace openscp

class MainWindow : public QMainWindow {
    Q_OBJECT
    public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
    Q_INVOKABLE void resetMainWindowLayoutToDefaults();

    protected:
    bool eventFilter(QObject *eventSource, QEvent *event) override;
    bool focusNextPrevChild(bool next) override;
    void showEvent(QShowEvent *e) override;
    void closeEvent(QCloseEvent *e) override;

    private slots:
    void chooseLeftDir();
    void chooseRightDir();
    void copyLeftToRight();         // F5
    void copyRightToLeft();         // remote -> left (no dialog)
    void moveRightToLeft();         // move selection from right panel to left
    void moveLeftToRight();         // F6
    void deleteFromLeft();          // Delete
    void goUpRight();               // Go up one level (right)
    void goUpLeft();                // Go up one level (left)
    void goHomeRight();             // Go to home/root (right)
    void goHomeLeft();              // Go to local home (left)
    void openRightRemoteTerminal(); // Open SSH terminal at current right path
    void refreshRightRemotePanel(); // Refresh current remote folder (right)
    void showHistoryMenu();         // Show recent routes/servers
    void showFavoritesDialog();     // Browse and manage saved paths

    void connectRemote();
    void disconnectRemote();
    void completeDisconnectRemote(quint64 disconnectSeq, bool forced);
    void rightItemActivated(const QModelIndex &idx); // double click on remote
    void
    leftItemActivated(const QModelIndex &idx); // double click on local (left)
    void downloadRightToLeft();                // remote -> local
    void uploadViaDialog(); // local -> remote (dialog: files or folder)
    void newDirRight();
    void newFileRight();
    void renameRightSelected();
    void deleteRightSelected();
    void showRightContextMenu(const QPoint &pos);
    void changeRemotePermissions();
    void showLeftContextMenu(const QPoint &pos);
    void newDirLeft();
    void newFileLeft();
    void renameLeftSelected();

    // Application menu
    void showAboutDialog();
    void showSettingsDialog();
    void showSyncDialog();

    private:
    struct PendingSiteSaveRequest {
        QString siteName;
        QString initialLocalPath;
        QString initialRemotePath = QStringLiteral("/");
        bool saveCredentials = false;
        bool rememberLastPaths = false;
    };
    struct SavedSiteContext {
        QString siteId;
        QString initialLocalPath;
        QString initialRemotePath = QStringLiteral("/");
        bool rememberLastPaths = false;
    };

    void updateDeleteShortcutEnables();
    void initializePanels(const QString &homePath);
    void initializeMainToolbar();
    void initializeMenuBarActions();
    void initializePanelInteractions();
    void initializeRuntimeState();
    void initializeSyncCoordinator();
    QProgressDialog *makeComparisonProgress(const QString &label);
    bool isScpTransferMode() const;
    void activateScpTransferModeUi(bool enabled);
    void showOpenPathDialog(bool rightPane);
    void applyPreferences();
    // Remote state (a single active session)
    RemoteOperationController *remoteOps_ = nullptr;
    SyncCoordinator *syncCoordinator_ = nullptr;
    openscpui::PaneController *paneController_ = nullptr;
    openscpui::RemoteActionController *remoteActionController_ = nullptr;
    openscpui::SessionController *sessionController_ = nullptr;
    bool rightIsRemote_ = false;
    openscpui::NavigationStore navigationStore_;

    void setLeftRoot(const QString &path);
    void setRightRoot(const QString &path);       // local
    void setRightRemoteRoot(const QString &path); // remote
    void requestRemoteListing(const QString &path, bool refresh,
                              bool initialLoad = false);

    // Models
    QFileSystemModel *leftModel_ = nullptr;
    QFileSystemModel *rightLocalModel_ = nullptr;
    RemoteModel *rightRemoteModel_ = nullptr;

    // Views and path inputs
    QTreeView *leftView_ = nullptr;
    QTreeView *rightView_ = nullptr;
    QStackedWidget *rightContentStack_ = nullptr;
    QWidget *scpTransferPanel_ = nullptr;
    QLabel *scpModeHintLabel_ = nullptr;
    QPushButton *scpQuickUploadBtn_ = nullptr;
    QPushButton *scpQuickDownloadBtn_ = nullptr;

    openscpui::PathNavigationBar *leftPath_ = nullptr;
    openscpui::PathNavigationBar *rightPath_ = nullptr;
    QSplitter *mainSplitter_ = nullptr;

    // Actions
    QAction *actChooseLeft_ = nullptr;
    QAction *actChooseRight_ = nullptr;
    QAction *actCopyF5_ = nullptr;
    QAction *actCopyRight_ = nullptr; // Copy from right (remote) panel to left
    QAction *actMoveRight_ = nullptr; // Move from right panel to left
    QAction *actMoveF6_ = nullptr;
    QAction *actDelete_ = nullptr;
    QAction *actConnect_ = nullptr;
    QAction *actDisconnect_ = nullptr;
    QAction *actDownloadF7_ = nullptr;
    QAction *actUploadRight_ = nullptr;
    QAction *actRefreshRight_ = nullptr;
    QAction *actOpenTerminalRight_ = nullptr;
    QAction *actSearchLeft_ = nullptr;
    QAction *actSearchRight_ = nullptr;
    QAction *actFavoriteToggleLeft_ = nullptr;
    QAction *actFavoriteToggleRight_ = nullptr;
    QAction *actNewDirRight_ = nullptr;
    QAction *actNewFileRight_ = nullptr;
    QAction *actRenameRight_ = nullptr;
    QAction *actDeleteRight_ = nullptr; // remote
    QAction *actNewDirLeft_ = nullptr;  // local (left)
    QAction *actNewFileLeft_ = nullptr; // local (left)
    QAction *actRenameLeft_ = nullptr;  // local (left)
    QAction *actCopyRightTb_ = nullptr; // right toolbar: Copy (generic text)
    QAction *actMoveRightTb_ = nullptr; // right toolbar: Move (generic text)

    // Sub-toolbar actions
    QAction *actUpLeft_ = nullptr; // back left
    QAction *actHomeLeft_ = nullptr;
    QAction *actUpRight_ = nullptr; // back right
    QAction *actHomeRight_ = nullptr;

    // Sub-toolbars
    QToolBar *mainToolbar_ = nullptr;
    QToolBar *leftPaneBar_ = nullptr;
    QToolBar *rightPaneBar_ = nullptr;
    QMenu *rightContextMenu_ = nullptr;
    QMenu *leftContextMenu_ = nullptr;

    // Transfer queue
    class TransferManager *transferMgr_ = nullptr;
    class TransferQueueDialog *transferDlg_ = nullptr;
    QAction *actShowQueue_ = nullptr;
    QAction *actShowHistory_ = nullptr;
    QAction *actShowFavorites_ = nullptr;
    QAction *actSync_ = nullptr;
    QAction *actSites_ = nullptr;        // site manager
    QAction *actPrefsToolbar_ = nullptr; // settings button (right toolbar)
    QAction *actAboutToolbar_ = nullptr; // about button (right toolbar)

    // Top menu
    QMenu *appMenu_ = nullptr;  // OpenSCP
    QMenu *fileMenu_ = nullptr; // File
    QAction *actAbout_ = nullptr;
    QAction *actPrefs_ = nullptr;
    QAction *actQuit_ = nullptr;

    // Downloads
    QString downloadDir_; // last local folder chosen for downloads
    QString uploadDir_;   // last local folder chosen for uploads
    std::unique_ptr<openscpui::PlatformFilePicker> uploadSourcePicker_;

    void showHostKeyPrompt(
        const openscpui::HostKeyPromptCoordinator::Prompt &prompt);
    void onTofuFinished(int dialogResult);
    bool consumeTofuDialogDecision(int result);
    void showSiteManagerNonModal();
    void maybeOpenSiteManagerAfterModal();
    bool
    confirmInsecureHostPolicyForSession(const openscp::SessionOptions &opt);
    void updateHostPolicyRiskBanner();
    void initializeConnectionSessionIndicators();
    void startConnectionSessionIndicators(const QString &connectionType);
    void resetConnectionSessionIndicators();
    void showTransferQueue();
    void maybeShowTransferQueue();
    void openLocalPathWithPreference(const QString &localPath);
    void reportPathActionResult(const openscpui::PathActionResult &result);
    void openConnectDialogWithPreset(
        const std::optional<openscp::SessionOptions> &preset);
    void addRecentLocalPath(const QString &path);
    void addRecentRemotePath(const QString &path);
    void addRecentServer(const openscp::SessionOptions &opt);
    QString remoteNavigationScope() const;
    void toggleCurrentFavorite(bool rightPane);
    void refreshFavoriteToggleAction(QAction *action,
                                     const QString &currentPath, bool remote);
    void refreshFavoritesActions();
    void applyTransferPreferences();
    static QString
    defaultDownloadDirFromSettings(const class QSettings &settings);

    // Helpers for connecting and wiring up the remote UI
    bool startRemoteConnection(
        openscp::SessionOptions opt,
        std::optional<PendingSiteSaveRequest> saveRequest = std::nullopt);
    void startSavedSiteConnect(const SiteEntry &site);
    void persistActiveSitePaths();
    bool validateConnectionStart(const openscp::SessionOptions &opt);
    void initializeConnectionUiState(
        const std::shared_ptr<std::atomic<bool>> &cancelFlag);
    void configureConnectionCallbacks(openscp::SessionOptions &opt);
    void launchConnectionWorker(
        openscp::SessionOptions opt, const openscp::SessionOptions &uiOpt,
        std::optional<PendingSiteSaveRequest> saveRequest,
        const std::shared_ptr<std::atomic<bool>> &cancelFlag);
    void finalizeConnection(bool connectionOk, const QString &errorText,
                            openscp::RemoteClient *connectedClient,
                            openscp::RemoteClient *remoteControlClient,
                            const openscp::SessionOptions &uiOpt,
                            std::optional<PendingSiteSaveRequest> saveRequest,
                            bool canceledByUser);
    quint64 beginDisconnectFlow();
    void applyDisconnectLocalUiState();
    bool runDisconnectTransferCleanupAsync(quint64 disconnectSeq);
    void scheduleDisconnectWatchdog(quint64 disconnectSeq);
    void applyRemoteConnectedUI(const openscp::SessionOptions &opt);
    void maybePersistQuickConnectSite(const openscp::SessionOptions &opt,
                                      const PendingSiteSaveRequest &req,
                                      bool connectionEstablished);
    struct LocalFsPair {
        QString sourcePath;
        QString targetPath;
    };
    static QVector<LocalFsPair>
    toLocalFsPairs(const QVector<QPair<QString, QString>> &pairs);
    void runLocalFsSelection(const QModelIndexList &rows,
                             QFileSystemModel *sourceModel,
                             const QDir &destination, bool deleteSource);
    void runLocalFsOperation(const QVector<LocalFsPair> &pairs,
                             bool deleteSource, int skippedCount = 0);
    struct RemoteDownloadSeed {
        QString remotePath;
        QString localPath;
        bool isDir = false;
    };
    void runRemoteDownloadPrescan(const QVector<RemoteDownloadSeed> &seeds,
                                  int initialSkipped, bool dragAndDrop,
                                  bool moveSources = false);
    void startLocalUploadDiscovery(
        const QVector<QPair<QString, QString>> &localRemoteRoots,
        bool moveSources, bool dragAndDrop = false);
    void cancelLocalUploadDiscoveries();
    void searchItemsInCurrentFolder(QTreeView *view, const QString &panelLabel);
    void rebuildContextMenu(QMenu *menu,
                            const QVector<QAction *> &entries) const;
    void refreshLeftPathNavigation();
    void refreshRightPathNavigation();
    void restoreMainWindowUiState();
    void saveMainWindowUiState() const;
    void saveRightHeaderState(bool remoteMode) const;
    bool restoreRightHeaderState(bool remoteMode);
    void handleTransferUiUpdate(const QVector<quint64> &upsertIds,
                                const QVector<quint64> &removedIds);
    bool isLikelyRemoteTransportError(const QString &rawError) const;
    QString preferredLocalHomePath() const;

    // Capability-derived mutation state. Actual path permissions are checked by
    // the requested operation without mutating the server as a probe.
    bool rightRemoteMutationsSupported_ = false;
    void updateRemoteMutationCapability();
    void applyRemoteMutationActions();

    bool firstShow_ = true;
    bool restoredWindowGeometry_ = false;
    openscpui::HostKeyPromptCoordinator hostKeyPromptCoordinator_;
    openscpui::SessionHealthMonitor sessionHealthMonitor_;
    openscpui::ConnectionStatusCoordinator connectionStatusCoordinator_;
    openscpui::TransferUiController transferUiController_;
    openscpui::FocusTraversalController *focusTraversalController_ = nullptr;

    // User preferences
    bool prefShowHidden_ = false;
    bool prefSingleClick_ = false;
    QString prefOpenBehaviorMode_ = QStringLiteral("ask"); // ask|reveal|open
    bool prefShowQueueOnEnqueue_ = true;
    int prefNoHostVerificationTtlMin_ = 15;
    QMetaObject::Connection leftClickConn_;
    QMetaObject::Connection rightClickConn_;

    // Reentrancy guards and dialog pointers
    bool transferCleanupInProgress_ = false;
    qint64 transferCleanupStartedAtMs_ = 0;
    QPointer<class QMessageBox> tofuBox_;
    QPointer<class QWidget> siteManager_;
    bool openSiteManagerOnDisconnect_ = true;
    bool openSiteManagerOnStartup_ = true;
    bool pendingOpenSiteManager_ = false;
    bool pendingCloseAfterDisconnect_ = false;
    bool sessionNoHostVerification_ = false;
    QString activeSecurityWarning_;
    QLabel *hostPolicyRiskLabel_ = nullptr;
    QLabel *connectionTypeLabel_ = nullptr;
    QLabel *connectionElapsedLabel_ = nullptr;
    // Connection progress dialog (non-modal), to avoid blocking TOFU
    QPointer<class QProgressDialog> connectProgress_;
    bool connectProgressDimmed_ = false;
    std::atomic<int> localFsJobsInFlight_{0};
    std::optional<SavedSiteContext> pendingSavedSiteContext_;
    std::optional<SavedSiteContext> activeSavedSiteContext_;
    QPointer<class QProgressDialog> remoteScanProgress_;
    QPointer<class QProgressDialog> syncProgress_;
    QSet<LocalTreeDiscovery *> activeLocalUploadDiscoveries_;
    std::shared_ptr<std::atomic<bool>> remoteScanCancelRequested_;
    std::atomic<bool> remoteScanInProgress_{false};
    quint64 activeRemoteListJob_ = 0;
    QString requestedRemotePath_;
    QStringList remoteRefreshSelectionNames_;
    int remoteRefreshScrollValue_ = 0;
    bool activeRemoteListIsRefresh_ = false;
    bool activeRemoteListIsInitial_ = false;
    bool initialRemoteFallbackAttempted_ = false;
};
