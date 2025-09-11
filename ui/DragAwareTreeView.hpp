// Draggable QTreeView that intercepts external drags to run post-drag cleanup
#pragma once
#include <QTreeView>
#include <QMetaObject>
#include <QElapsedTimer>
#include <memory>
#include <atomic>

class DragAwareTreeView : public QTreeView {
    Q_OBJECT
public:
    explicit DragAwareTreeView(QWidget* parent = nullptr);
    ~DragAwareTreeView() override;
    // Transfer manager is used to asynchronously stage remote files for drag-out
    void setTransferManager(class TransferManager* mgr) { transferMgr_ = mgr; }
protected:
    void startDrag(Qt::DropActions supportedActions) override;
    void resizeEvent(QResizeEvent* e) override;
    void closeEvent(QCloseEvent* e) override;
private:
    void showKeepMessage(const QString& batchDir);
    void showKeepMessageWithPrefix(const QString& prefix, const QString& batchDir);
    void scheduleAutoCleanup(const QString& batchDir);
    // Remote -> system drag-out: prepare asynchronously using TransferManager
    void startRemoteDragAsync(class RemoteModel* rm);

    // Lightweight overlay while preparing staging
    void showPrepOverlay(const QString& text);
    void hidePrepOverlay();
    void updateOverlayGeometry();
    void cancelCurrentBatch(const QString& reason);
    void logBatchResult(const QString& batchId, int totalItems, int failedItems, const QString& result);

    // Shared helpers
    QString buildStagingRoot() const;

    // State
    class TransferManager* transferMgr_ = nullptr; // not owned
    QWidget* overlay_ = nullptr;        // owned by this (viewport child)
    class QLabel* overlayLabel_ = nullptr; // non-owning (child of overlay_)
    class QProgressBar* overlayProgress_ = nullptr; // non-owning (child of overlay_)
    class QPushButton* overlayCancel_ = nullptr;    // non-owning (child of overlay_)
    class QShortcut* overlayEsc_ = nullptr;         // ESC shortcut while overlay visible
    class QTimer* waitTimer_ = nullptr;             // Wait/Cancel timer

    // Drag state
    bool dragInProgress_ = false;
    QString currentBatchDir_;
    QString currentBatchId_;
    int currentBatchTotal_ = 0;
    QElapsedTimer prepTimer_;
    QElapsedTimer stagingTimer_;
    QMetaObject::Connection stagingConn_;
    QMetaObject::Connection quitConn_;
    std::shared_ptr<std::atomic_bool> enumCancelFlag_;
    quint64 enumSymlinksSkipped_ = 0;
    quint64 enumDenied_ = 0;
    qint64  enumMs_ = -1;
    bool batchLogged_ = false; // ensure single-shot logging per batch
};
