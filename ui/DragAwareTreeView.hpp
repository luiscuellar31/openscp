// Draggable QTreeView that intercepts external drags to run post-drag cleanup
#pragma once
#include <QElapsedTimer>
#include <QHash>
#include <QMetaObject>
#include <QPair>
#include <QPointer>
#include <QSet>
#include <QStringList>
#include <QTreeView>
#include <QVector>

#include <memory>

class RemoteModel;
class RemoteOperationController;
class TransferManager;
class QFrame;

class DragAwareTreeView : public QTreeView {
    Q_OBJECT
    public:
    explicit DragAwareTreeView(QWidget *parent = nullptr);
    ~DragAwareTreeView() override;
    // Transfer manager is used to asynchronously stage remote files for
    // drag-out
    void setTransferManager(TransferManager *mgr);
    void setRemoteOperationController(RemoteOperationController *controller);

    protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void resizeEvent(QResizeEvent *resizeEventArg) override;
    void closeEvent(QCloseEvent *closeEventArg) override;

    private:
    void showKeepMessage(const QString &batchDir);
    void showKeepMessageWithPrefix(const QString &prefix,
                                   const QString &batchDir);
    void scheduleAutoCleanup(const QString &batchDir, int initialDelayMs = 500);
    // Remote -> system drag-out: prepare asynchronously using TransferManager
    void startRemoteDragAsync(class RemoteModel *remoteModel);
    using RemoteDragTarget = QPair<QString, QString>; // remote, local
    struct RemoteDragBatchStats {
        quint64 totalBytes = 0;
        quint64 totalItems = 0;
        quint64 totalDirs = 0;
        quint64 unknownSizeCount = 0;
        bool anySizeUnknown = false;
    };
    QModelIndexList collectRemoteSelectedRows() const;
    void finishRemoteDragEnumeration();
    bool confirmRemoteDragThreshold(const RemoteDragBatchStats &stats);
    bool enforceRemoteDragThreshold();
    struct RemoteDragStagingState;
    void startRemoteDragStaging(QVector<RemoteDragTarget> targets,
                                QStringList directories, QStringList dragRoots,
                                const RemoteDragBatchStats &stats);
    void
    pumpRemoteDragStaging(const std::shared_ptr<RemoteDragStagingState> &state);
    void reconcileRemoteDragTasks(
        const std::shared_ptr<RemoteDragStagingState> &state,
        const QVector<quint64> &taskIds, bool removed);
    void finishRemoteDragStaging(
        const std::shared_ptr<RemoteDragStagingState> &state);
    QString formatRemoteDragMetrics(const QString &result,
                                    const RemoteDragBatchStats &stats,
                                    qint64 stagingMs) const;
    void resetRemoteDragState();

    // Lightweight overlay while preparing staging
    void showPrepOverlay(const QString &text);
    void hidePrepOverlay();
    void updateOverlayGeometry();
    void cancelCurrentBatch(const QString &reason);
    void logBatchResult(const QString &batchId, int totalItems, int failedItems,
                        const QString &result);

    // Shared helpers
    QString buildStagingRoot() const;

    // State
    QPointer<TransferManager> transferMgr_;         // not owned
    QPointer<RemoteOperationController> remoteOps_; // not owned
    QFrame *overlay_ = nullptr;            // owned by this (viewport child)
    class QLabel *overlayLabel_ = nullptr; // non-owning (child of overlay_)
    class QProgressBar *overlayProgress_ =
        nullptr; // non-owning (child of overlay_)
    class QPushButton *overlayCancel_ =
        nullptr; // non-owning (child of overlay_)
    class QShortcut *overlayEsc_ =
        nullptr;                        // ESC shortcut while overlay visible
    class QTimer *waitTimer_ = nullptr; // Wait/Cancel timer

    // Drag state
    bool dragInProgress_ = false;
    QString currentBatchDir_;
    QString currentBatchId_;
    int currentBatchTotal_ = 0;
    QElapsedTimer prepTimer_;
    QElapsedTimer stagingTimer_;
    QMetaObject::Connection quitConn_;
    QHash<quint64, QString> enumJobLocalRoots_;
    QSet<quint64> enumPendingJobs_;
    QVector<RemoteDragTarget> enumTargets_;
    QStringList enumDirectories_;
    QStringList enumDragRoots_;
    std::shared_ptr<RemoteDragStagingState> stagingState_;
    RemoteDragBatchStats enumStats_;
    QMetaObject::Connection enumBatchConn_;
    QMetaObject::Connection enumProgressConn_;
    QMetaObject::Connection enumFinishedConn_;
    quint64 enumSymlinksSkipped_ = 0;
    quint64 enumDepthLimits_ = 0;
    quint64 enumInvalidNames_ = 0;
    quint64 enumInaccessible_ = 0;
    qint64 enumMs_ = -1;
    bool enumThresholdConfirmed_ = false;
    bool enumThresholdPromptActive_ = false;
    bool batchLogged_ = false; // ensure single-shot logging per batch
};
