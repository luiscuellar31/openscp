// Transfer queue manager with concurrent worker execution.
#pragma once
#include "openscp/SftpTypes.hpp"
#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <unordered_map>
#include <unordered_set>

namespace openscp {
class SftpClient;
}

// Transfer queue item.
// Represents an upload or download operation with its state and options.
struct TransferTask {
    enum class Type { Upload, Download } type;
    quint64 id = 0;                // stable identifier for cross-thread updates
    QString src;                   // local for uploads, remote for downloads
    QString dst;                   // remote for uploads, local for downloads
    bool resumeHint = false;       // if true, try to resume on next attempt
    int speedLimitKBps = 0;        // 0 = unlimited; KB/s
    int progress = 0;              // 0..100
    quint64 bytesDone = 0;         // transferred bytes so far
    quint64 bytesTotal = 0;        // total bytes if known
    double currentSpeedKBps = 0.0; // measured speed
    int etaSeconds = -1;           // -1 unknown
    int attempts = 0;
    int maxAttempts = 3;
    // Task state:
    //  - Queued: in queue, pending execution
    //  - Running: in progress
    //  - Paused: paused by the user
    //  - Done: completed successfully
    //  - Error: finished with error
    //  - Canceled: canceled by the user
    enum class Status {
        Queued,
        Running,
        Paused,
        Done,
        Error,
        Canceled
    } status = Status::Queued;
    QString error;
    qint64 finishedAtMs = 0; // epoch ms when task entered final state
};

class TransferManager : public QObject {
    Q_OBJECT
    public:
    explicit TransferManager(QObject *parent = nullptr);
    ~TransferManager();

    // Inject the SFTP client to use (not owned by the manager)
    void setClient(openscp::SftpClient *c);
    void clearClient();
    // Session options used to create independent worker connections.
    void setSessionOptions(const openscp::SessionOptions &opt);
    // Concurrency: maximum number of simultaneous tasks
    void setMaxConcurrent(int n) {
        if (n < 1)
            n = 1;
        maxConcurrent_ = n;
    }
    int maxConcurrent() const { return maxConcurrent_; }
    // Global speed limit (KB/s). 0 = unlimited
    void setGlobalSpeedLimitKBps(int kbps) { globalSpeedKBps_.store(kbps); }
    int globalSpeedLimitKBps() const { return globalSpeedKBps_.load(); }
    bool isQueuePaused() const { return paused_.load(); }

    // Pause/Resume per task
    void pauseTask(quint64 id);
    void resumeTask(quint64 id);
    // Cancel a task (transitions to Canceled)
    void cancelTask(quint64 id);
    // Cancel all active or queued tasks
    void cancelAll();
    // Adjust per-task speed limit (KB/s). 0 = unlimited
    void setTaskSpeedLimit(quint64 id, int kbps);

    void enqueueUpload(const QString &local, const QString &remote);
    void enqueueDownload(const QString &remote, const QString &local);

    // Thread-safe copy of the current task list.
    QVector<TransferTask> tasksSnapshot() const;

    // Pause/Resume the whole queue
    void pauseAll();
    void resumeAll();
    void retryFailed();
    void retryTask(quint64 id);
    void clearCompleted();
    void clearFailedCanceled();
    void clearFinishedOlderThan(int minutes, bool clearDone,
                                bool clearFailedCanceled);

    signals:
    // Emitted when the task list/state changes (to refresh the UI)
    void tasksChanged();

    public slots:
    void processNext(); // process in order; one at a time
    void schedule();    // attempt to launch up to maxConcurrent

    private:
    openscp::SftpClient *client_ = nullptr; // not owned by the manager
    QVector<TransferTask> tasks_;
    std::atomic<bool> paused_{false};
    std::atomic<int> running_{0};
    int maxConcurrent_ = 2;
    std::atomic<int> globalSpeedKBps_{0};

    // Worker threads per task
    std::unordered_map<quint64, std::thread> workers_;
    // Auxiliary state: paused/canceled ids for worker cooperation
    std::unordered_set<quint64> pausedTasks_;
    std::unordered_set<quint64> canceledTasks_;
    // Synchronization
    mutable std::mutex
        mtx_; // protects tasks_, client_, options and auxiliary sets
    std::mutex connFactoryMutex_; // serializes creation of worker SFTP clients
    quint64 nextId_ = 1;

    int indexForId(quint64 id) const;
    // Build an isolated SFTP client for one worker task.
    std::unique_ptr<openscp::SftpClient> createWorkerClient(std::string &err);
    std::optional<openscp::SessionOptions> sessionOpt_;
};
