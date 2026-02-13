// Declaration of the main window and its state/actions.
#pragma once
#include <QAction>
#include <QFileSystemModel>
#include <QLineEdit>
#include <QMainWindow>
#include <QPointer>
#include <QTreeView>
#include <QVector>
#include <atomic>
#include <condition_variable>
#include <memory>
#include <optional>
#include <string>

class RemoteModel; // fwd
class QModelIndex; // fwd for slot signatures
class QToolBar;    // fwd
class QMenu;       // fwd
class QEvent;      // fwd for eventFilter
class QDialog;     // fwd
class QLabel;      // fwd
namespace openscp {
class SftpClient;
struct SessionOptions;
} // namespace openscp

class MainWindow : public QMainWindow {
    Q_OBJECT
    public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow();
    // Preference: open Site Manager automatically on disconnect (non‑modal)
    void setOpenSiteManagerOnDisconnect(bool on);
    bool openSiteManagerOnDisconnect() const {
        return m_openSiteManagerOnDisconnect;
    }
    // Preference: open Site Manager automatically on startup (non‑modal)
    void setOpenSiteManagerOnStartup(bool on);
    bool openSiteManagerOnStartup() const { return m_openSiteManagerOnStartup; }

    protected:
    bool eventFilter(QObject *obj, QEvent *ev) override;
    void showEvent(QShowEvent *e) override;

    private slots:
    void chooseLeftDir();
    void chooseRightDir();
    void leftPathEntered();
    void rightPathEntered();
    void copyLeftToRight(); // F5
    void copyRightToLeft(); // remote -> left (no dialog)
    void moveRightToLeft(); // move selection from right panel to left
    void moveLeftToRight(); // F6
    void deleteFromLeft();  // Delete
    void goUpRight();       // Go up one level (right)
    void goUpLeft();        // Go up one level (left)

    void connectSftp();
    void disconnectSftp();
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

    private:
    struct PendingSiteSaveRequest {
        QString siteName;
        bool saveCredentials = false;
    };

    void updateDeleteShortcutEnables();
    void applyPreferences();
    // Remote state (a single active session)
    std::unique_ptr<openscp::SftpClient> sftp_;
    bool rightIsRemote_ = false;

    void setLeftRoot(const QString &path);
    void setRightRoot(const QString &path);       // local
    void setRightRemoteRoot(const QString &path); // remote

    // Models
    QFileSystemModel *leftModel_ = nullptr;
    QFileSystemModel *rightLocalModel_ = nullptr;
    RemoteModel *rightRemoteModel_ = nullptr;

    // Views and path inputs
    QTreeView *leftView_ = nullptr;
    QTreeView *rightView_ = nullptr;

    QLineEdit *leftPath_ = nullptr;
    QLineEdit *rightPath_ = nullptr;

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
    QAction *actUpLeft_ = nullptr;  // back left
    QAction *actUpRight_ = nullptr; // back right

    // Sub-toolbars
    QToolBar *leftPaneBar_ = nullptr;
    QToolBar *rightPaneBar_ = nullptr;
    QMenu *rightContextMenu_ = nullptr;
    QMenu *leftContextMenu_ = nullptr;

    // Transfer queue
    class TransferManager *transferMgr_ = nullptr;
    class TransferQueueDialog *transferDlg_ = nullptr;
    QAction *actShowQueue_ = nullptr;
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

    // Host key confirmation (TOFU) — non‑modal UI with sync wait (no exec())
    bool confirmHostKeyUI(const QString &host, quint16 port,
                          const QString &algorithm, const QString &fingerprint,
                          bool canSave);

    // Explicit non‑modal TOFU dialog API per spec
    void showTOfuDialog(const QString &host, const QString &alg,
                        const QString &fp);
    void onTofuFinished(int r);
    void showOneTimeDialog(const QString &host, const QString &alg,
                           const QString &fp);
    void onOneTimeFinished(int r);
    void showSiteManagerNonModal();
    void maybeOpenSiteManagerAfterModal();
    bool
    confirmInsecureHostPolicyForSession(const openscp::SessionOptions &opt);
    void updateHostPolicyRiskBanner();
    void showTransferQueue();
    void maybeShowTransferQueue();
    void openLocalPathWithPreference(const QString &localPath);
    void applyTransferPreferences();
    static QString defaultDownloadDirFromSettings(const class QSettings &s);

    // Helpers for connecting and wiring up the remote UI
    void startSftpConnect(
        openscp::SessionOptions opt,
        std::optional<PendingSiteSaveRequest> saveRequest = std::nullopt);
    void finalizeSftpConnect(bool okConn, const QString &err,
                             openscp::SftpClient *connectedClient,
                             const openscp::SessionOptions &uiOpt,
                             std::optional<PendingSiteSaveRequest> saveRequest,
                             bool canceledByUser);
    void applyRemoteConnectedUI(const openscp::SessionOptions &opt);
    void maybePersistQuickConnectSite(const openscp::SessionOptions &opt,
                                      const PendingSiteSaveRequest &req,
                                      bool connectionEstablished);
    struct LocalFsPair {
        QString sourcePath;
        QString targetPath;
    };
    void runLocalFsOperation(const QVector<LocalFsPair> &pairs,
                             bool deleteSource, int skippedCount = 0);

    // Writable state of the current remote directory
    bool rightRemoteWritable_ = false;
    // Recompute if the current remote directory is writable (create/remove a
    // temporary folder)
    void updateRemoteWriteability();

    bool firstShow_ = true;

    // User preferences
    bool prefShowHidden_ = false;
    bool prefSingleClick_ = false;
    QString prefOpenBehaviorMode_ = QStringLiteral("ask"); // ask|reveal|open
    bool prefShowQueueOnEnqueue_ = true;
    int prefNoHostVerificationTtlMin_ = 15;
    QMetaObject::Connection leftClickConn_;
    QMetaObject::Connection rightClickConn_;

    // Reentrancy guards and dialog pointers
    bool m_isDisconnecting = false;
    QPointer<class QMessageBox> m_tofuBox;
    QPointer<class QWidget> m_siteManager;
    bool m_openSiteManagerOnDisconnect = true;
    bool m_openSiteManagerOnStartup = true;
    bool m_pendingOpenSiteManager = false;
    bool m_sessionNoHostVerification_ = false;
    QLabel *m_hostPolicyRiskLabel_ = nullptr;
    // Connection progress dialog (non-modal), to avoid blocking TOFU
    QPointer<class QProgressDialog> m_connectProgress_;
    bool m_connectProgressDimmed_ = false;
    bool m_connectInProgress_ = false;
    std::shared_ptr<std::atomic<bool>> m_connectCancelRequested_;
    std::atomic<int> m_localFsJobsInFlight_{0};
    // TOFU wait state
    std::mutex m_tofuMutex_;
    std::condition_variable m_tofuCv_;
    bool m_tofuDecided_ = false;
    bool m_tofuAccepted_ = false;
    bool m_tofuCanSave_ = false;
    QString m_tofuHost_;
    QString m_tofuAlg_;
    QString m_tofuFp_;
};
