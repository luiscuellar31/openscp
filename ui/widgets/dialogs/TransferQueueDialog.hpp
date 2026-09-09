// Dialog to visualize and manage the transfer queue.
#pragma once
#include "logic/transfers/TransferManager.hpp"

#include <QDialog>
#include <QPointer>
#include <QRect>

class QLabel;
class QPushButton;
class QTableView;
class QButtonGroup;
class QToolButton;
class QComboBox;
class QAction;
class QParallelAnimationGroup;
class TransferTaskTableModel;
class TransferTaskFilterProxyModel;

// Dialog to monitor and control the transfer queue.
// Allows pausing/resuming, canceling, and limiting per-task speed.
class TransferQueueDialog : public QDialog {
    Q_OBJECT
    public:
    explicit TransferQueueDialog(TransferManager *mgr,
                                 QWidget *parent = nullptr);
    void presentAnimated();

    public slots:
    void reject() override;

    private slots:
    void refresh();            // refresh table from manager
    void onPause();            // pause the whole queue
    void onResume();           // resume the queue (and paused tasks)
    void onRetry();            // retry failed/canceled
    void onClearDone();        // clear completed
    void onPauseSelected();    // pause selected tasks
    void onResumeSelected();   // resume selected tasks
    void onApplyGlobalSpeed(); // apply global limit
    void onLimitSelected();    // limit selected tasks
    void onStopSelected();     // cancel selected tasks
    void onStopAll();          // cancel the whole queue in progress
    void onFilterChanged(int filterId);
    void onRetrySelected();
    void onShowDestinationFolder();
    void onCopySourcePath();
    void onCopyDestinationPath();
    void onClearFinished();
    void onClearFailedCanceled();
    void onAutoClearChanged();
    void onOpenQueueOptions();
    void onTasksAdded(const QVector<quint64> &taskIds);
    void onTasksUpdated(const QVector<quint64> &taskIds);
    void onTasksRemoved(const QVector<quint64> &taskIds);
    void onQueueSettingsChanged();
    void showContextMenu(const QPoint &pos); // context menu on the table

    private:
    enum FilterMode {
        FilterAll = 0,
        FilterActive = 1,
        FilterErrors = 2,
        FilterCompleted = 3,
        FilterCanceled = 4
    };
    enum AutoClearMode {
        AutoClearOff = 0,
        AutoClearCompleted = 1,
        AutoClearFailedCanceled = 2,
        AutoClearFinished = 3
    };
    enum class WindowTransition { None, Showing, Hiding };

    void updateSummary();
    QVector<quint64> selectedTaskIds() const;
    void loadUiState();
    void saveUiState() const;
    void maybeAutoClear(const QVector<TransferTask> &snapshot);
    void buildQueueOptionsDialog();
    void startWindowTransition(WindowTransition transition);
    void stopWindowTransition();

    TransferManager *mgr_;        // source of truth for the queue
    QTableView *table_ = nullptr; // view of tasks
    TransferTaskTableModel *model_ =
        nullptr; // task model (taskId-based updates)
    TransferTaskFilterProxyModel *proxy_ = nullptr; // status filter proxy
    QLabel *badgeTotal_ = nullptr;
    QLabel *badgeActive_ = nullptr;
    QLabel *badgeRunning_ = nullptr;
    QLabel *badgePaused_ = nullptr;
    QLabel *badgeErrors_ = nullptr;
    QLabel *badgeCompleted_ = nullptr;
    QLabel *badgeCanceled_ = nullptr;
    QLabel *badgeParallel_ = nullptr;
    QLabel *badgeLimit_ = nullptr;
    QLabel *emptyStateLabel_ = nullptr;
    QPushButton *pauseBtn_ = nullptr;     // global pause
    QPushButton *resumeBtn_ = nullptr;    // global resume
    QPushButton *retryBtn_ = nullptr;     // retry
    QPushButton *closeBtn_ = nullptr;     // close dialog
    QPushButton *pauseSelBtn_ = nullptr;  // pause selected
    QPushButton *resumeSelBtn_ = nullptr; // resume selected
    QPushButton *limitSelBtn_ = nullptr;  // limit selected
    QPushButton *stopSelBtn_ = nullptr;   // cancel selected
    QPushButton *stopAllBtn_ = nullptr;   // cancel all
    QPushButton *clearMenuBtn_ = nullptr;
    QAction *clearCompletedAction_ = nullptr;
    QAction *clearFailedAction_ = nullptr;
    QDialog *queueOptionsDialog_ = nullptr;
    class QSpinBox *speedSpin_ = nullptr; // global limit value
    QButtonGroup *filterGroup_ = nullptr;
    QToolButton *filterAllBtn_ = nullptr;
    QToolButton *filterActiveBtn_ = nullptr;
    QToolButton *filterErrorsBtn_ = nullptr;
    QToolButton *filterCompletedBtn_ = nullptr;
    QToolButton *filterCanceledBtn_ = nullptr;
    QComboBox *autoClearModeCombo_ = nullptr;
    class QSpinBox *autoClearMinutesSpin_ = nullptr;
    bool suppressAutoClearSignal_ = false;
    QPointer<QParallelAnimationGroup> windowTransitionAnimation_;
    QRect restingGeometry_;
    WindowTransition windowTransition_ = WindowTransition::None;
};
