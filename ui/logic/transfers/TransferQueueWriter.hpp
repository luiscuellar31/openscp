// Writes transfer queue snapshots to disk on its own thread.
#pragma once

#include "logic/transfers/TransferTypes.hpp"

#include <QString>
#include <QVector>

#include <condition_variable>
#include <functional>
#include <mutex>
#include <optional>
#include <stop_token>
#include <thread>

// The thread that owns the queue hands over snapshots and moves on, so it
// never waits for serialization or the disk. Only the newest state matters:
// a snapshot handed over while another one is being written replaces the one
// waiting its turn.
class TransferQueueWriter final {
    public:
    using WarningHandler = std::function<void(const QString &)>;

    TransferQueueWriter();
    ~TransferQueueWriter();

    TransferQueueWriter(const TransferQueueWriter &) = delete;
    TransferQueueWriter &operator=(const TransferQueueWriter &) = delete;

    // Called from the writer thread when a snapshot could not be saved.
    void setWarningHandler(WarningHandler onWarning);
    // Snapshots are dropped until a path is set.
    void setPath(QString path);

    void save(QVector<TransferTask> tasks);
    // Saves and returns once the snapshot is on disk.
    void saveAndWait(QVector<TransferTask> tasks);
    // Writes what is still pending, stops reporting warnings, and joins.
    // Later snapshots are dropped.
    void shutdown();

    private:
    void run(std::stop_token stopToken);

    mutable std::mutex mutex_;
    std::condition_variable wake_;
    std::condition_variable idle_;
    QString path_;
    WarningHandler onWarning_;
    std::optional<QVector<TransferTask>> pending_;
    bool writing_ = false;
    bool stopped_ = false;
    std::jthread thread_;
};
