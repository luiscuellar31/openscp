#include "logic/transfers/TransferQueueWriter.hpp"

#include "logic/transfers/TransferQueuePersistence.hpp"

#include <utility>

TransferQueueWriter::TransferQueueWriter()
    : thread_([this](std::stop_token stopToken) { run(stopToken); }) {
}

TransferQueueWriter::~TransferQueueWriter() {
    shutdown();
}

void TransferQueueWriter::setWarningHandler(WarningHandler onWarning) {
    std::lock_guard<std::mutex> lock(mutex_);
    onWarning_ = std::move(onWarning);
}

void TransferQueueWriter::setPath(QString path) {
    std::lock_guard<std::mutex> lock(mutex_);
    path_ = std::move(path);
}

void TransferQueueWriter::save(QVector<TransferTask> tasks) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_)
            return;
        pending_ = std::move(tasks);
    }
    wake_.notify_one();
}

void TransferQueueWriter::saveAndWait(QVector<TransferTask> tasks) {
    std::unique_lock<std::mutex> lock(mutex_);
    if (stopped_)
        return;
    pending_ = std::move(tasks);
    wake_.notify_one();
    idle_.wait(lock, [this] { return !pending_.has_value() && !writing_; });
}

void TransferQueueWriter::shutdown() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopped_)
            return;
        stopped_ = true;
        // A warning raised while the owner is going away has nowhere to go.
        onWarning_ = {};
    }
    thread_.request_stop();
    wake_.notify_all();
    if (thread_.joinable())
        thread_.join();
}

void TransferQueueWriter::run(std::stop_token stopToken) {
    while (true) {
        QVector<TransferTask> tasks;
        QString path;
        WarningHandler onWarning;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            wake_.wait(lock, [this, &stopToken] {
                return pending_.has_value() || stopToken.stop_requested();
            });
            // Whatever is pending is written even while stopping, so the last
            // state of the queue is never lost.
            if (!pending_.has_value())
                return;
            tasks = std::move(*pending_);
            pending_.reset();
            path = path_;
            onWarning = onWarning_;
            writing_ = true;
        }

        TransferQueuePersistence::SaveResult result;
        if (path.isEmpty())
            result.succeeded = true;
        else
            result = TransferQueuePersistence::save(path, tasks);

        {
            std::lock_guard<std::mutex> lock(mutex_);
            writing_ = false;
        }
        idle_.notify_all();
        if (!result.succeeded && !result.warning.isEmpty() && onWarning)
            onWarning(result.warning);
    }
}
