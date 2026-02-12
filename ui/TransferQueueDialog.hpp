// Dialog to visualize and manage the transfer queue.
#pragma once
#include <QDialog>
#include "TransferManager.hpp"

class QLabel;
class QPushButton;
class QTableView;
class TransferTaskTableModel;

// Dialog to monitor and control the transfer queue.
// Allows pausing/resuming, canceling, and limiting per-task speed.
class TransferQueueDialog : public QDialog {
    Q_OBJECT
public:
    explicit TransferQueueDialog(TransferManager* mgr, QWidget* parent = nullptr);

private slots:
    void refresh();           // refresh table from manager
    void onPause();           // pause the whole queue
    void onResume();          // resume the queue (and paused tasks)
    void onRetry();           // retry failed/canceled
    void onClearDone();       // clear completed
    void onPauseSelected();   // pause selected tasks
    void onResumeSelected();  // resume selected tasks
    void onApplyGlobalSpeed();// apply global limit
    void onLimitSelected();   // limit selected tasks
    void onStopSelected();    // cancel selected tasks
    void onStopAll();         // cancel the whole queue in progress
    void showContextMenu(const QPoint& pos); // context menu on the table

private:
    void updateSummary();

    TransferManager* mgr_;             // source of truth for the queue
    QTableView* table_ = nullptr;      // view of tasks
    TransferTaskTableModel* model_ = nullptr; // task model (id-based updates)
    QLabel* summaryLabel_ = nullptr;   // summary at the bottom
    QPushButton* pauseBtn_ = nullptr;  // global pause
    QPushButton* resumeBtn_ = nullptr; // global resume
    QPushButton* retryBtn_ = nullptr;  // retry
    QPushButton* clearBtn_ = nullptr;  // clear completed
    QPushButton* closeBtn_ = nullptr;  // close dialog
    QPushButton* pauseSelBtn_ = nullptr;   // pause selected
    QPushButton* resumeSelBtn_ = nullptr;  // resume selected
    QPushButton* limitSelBtn_ = nullptr;   // limit selected
    QPushButton* stopSelBtn_ = nullptr;    // cancel selected
    QPushButton* stopAllBtn_ = nullptr;    // cancel all
    class QSpinBox* speedSpin_ = nullptr;  // global limit value
    QPushButton* applySpeedBtn_ = nullptr; // apply global limit
}; 
