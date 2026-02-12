// Transfer queue manager (sequential) with pause/retry/resume.
#pragma once
#include <QObject>
#include <QString>
#include <QVector>
#include <atomic>
#include <thread>
#include <optional>
#include <mutex>
#include <unordered_map>
#include <unordered_set>
#include "openscp/SftpTypes.hpp"

namespace openscp { class SftpClient; }

// Transfer queue item.
// Represents an upload or download operation with its state and options.
struct TransferTask {
    enum class Type { Upload, Download } type;
    quint64 id = 0;  // stable identifier for cross-thread updates
    QString src;     // local for uploads, remote for downloads
    QString dst;     // remote for uploads, local for downloads
    bool resumeHint = false;    // if true, try to resume on next attempt
    int speedLimitKBps = 0;     // 0 = unlimited; KB/s
    int progress = 0;           // 0..100
    int attempts = 0;
    int maxAttempts = 3;
    // Task state:
    //  - Queued: in queue, pending execution
    //  - Running: in progress
    //  - Paused: paused by the user
    //  - Done: completed successfully
    //  - Error: finished with error
    //  - Canceled: canceled by the user
    enum class Status { Queued, Running, Paused, Done, Error, Canceled } status = Status::Queued;
    QString error;
};

class TransferManager : public QObject {
    Q_OBJECT
public:
    explicit TransferManager(QObject* parent = nullptr);
    ~TransferManager();

    // Inject the SFTP client to use (not owned by the manager)
    void setClient(openscp::SftpClient* c) { client_ = c; }
    void clearClient();
    // Session options for auto-reconnect
    void setSessionOptions(const openscp::SessionOptions& opt) { sessionOpt_ = opt; }
    // Concurrency: maximum number of simultaneous tasks
    void setMaxConcurrent(int n) { if (n < 1) n = 1; maxConcurrent_ = n; }
    int maxConcurrent() const { return maxConcurrent_; }
    // Global speed limit (KB/s). 0 = unlimited
    void setGlobalSpeedLimitKBps(int kbps) { globalSpeedKBps_.store(kbps); }
    int globalSpeedLimitKBps() const { return globalSpeedKBps_.load(); }

    // Pause/Resume per task
    void pauseTask(quint64 id);
    void resumeTask(quint64 id);
    // Cancel a task (transitions to Canceled)
    void cancelTask(quint64 id);
    // Cancel all active or queued tasks
    void cancelAll();
    // Adjust per-task speed limit (KB/s). 0 = unlimited
    void setTaskSpeedLimit(quint64 id, int kbps);

    void enqueueUpload(const QString& local, const QString& remote);
    void enqueueDownload(const QString& remote, const QString& local);

    // Thread-safe copy of the current task list.
    QVector<TransferTask> tasksSnapshot() const;

    // Pause/Resume the whole queue
    void pauseAll();
    void resumeAll();
    void retryFailed();
    void clearCompleted();

signals:
    // Emitted when the task list/state changes (to refresh the UI)
    void tasksChanged();

public slots:
    void processNext(); // process in order; one at a time
    void schedule();    // attempt to launch up to maxConcurrent

private:
    openscp::SftpClient* client_ = nullptr; // not owned by the manager
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
    mutable std::mutex mtx_;   // protects tasks_ and auxiliary sets
    std::mutex sftpMutex_;     // serializes calls to libssh2 (not thread-safe)
    quint64 nextId_ = 1;

    int indexForId(quint64 id) const;
    // Reconnect the client if disconnected (with backoff). Returns true on success.
    bool ensureConnected(std::string& err);
    std::optional<openscp::SessionOptions> sessionOpt_;
};
