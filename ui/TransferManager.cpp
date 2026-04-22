// Queue implementation: schedules concurrent worker transfers with isolated
// SFTP sessions.
#include "TransferManager.hpp"
#include "TimeUtils.hpp"
#include "UiAlerts.hpp"
#include "openscp/RuntimeLogging.hpp"
#include "openscp/SftpClient.hpp"
#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QLocale>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QMetaObject>
#include <QPushButton>
#include <QThread>
#include <QTimeZone>
#include <chrono>
#include <condition_variable>
#include <thread>
#include <vector>
Q_LOGGING_CATEGORY(ocXfer, "openscp.transfer")

static const char *transferStatusName(TransferTask::Status st) {
    switch (st) {
    case TransferTask::Status::Queued:
        return "Queued";
    case TransferTask::Status::Running:
        return "Running";
    case TransferTask::Status::Paused:
        return "Paused";
    case TransferTask::Status::Done:
        return "Done";
    case TransferTask::Status::Error:
        return "Error";
    case TransferTask::Status::Canceled:
        return "Canceled";
    }
    return "Unknown";
}

static QString transferErrorForUi(const std::string &rawError) {
    const QString uiErrorMessage = QString::fromStdString(rawError).trimmed();
    if (uiErrorMessage.isEmpty())
        return uiErrorMessage;

    const QString lower = uiErrorMessage.toLower();
    if (lower.contains("checksum mismatch")) {
        return QCoreApplication::translate(
            "TransferManager",
            "Integrity mismatch detected: local and remote checksums differ. "
            "Transfer was stopped to prevent corrupted data.");
    }
    if (lower.contains("prefix does not match")) {
        return QCoreApplication::translate(
            "TransferManager",
            "Resume integrity mismatch detected between local and remote "
            "partial data. Transfer was stopped.");
    }
    if (lower.contains("could not verify final integrity") ||
        lower.contains("could not validate resume integrity")) {
        return QCoreApplication::translate(
                   "TransferManager",
                   "Integrity verification is required but could not be "
                   "completed. Transfer failed.") +
               "\n" + uiErrorMessage;
    }
    return uiErrorMessage;
}

static int askOverwriteConflictOnUi(QObject *uiContext, const QString &name,
                                    const QString &srcInfo,
                                    const QString &dstInfo,
                                    const std::function<bool()> &shouldAbort =
                                        {}) {
    auto showPrompt = [name, srcInfo, dstInfo]() -> int {
        int choice = 0; // 0 = skip, 1 = overwrite, 2 = resume
        QMessageBox msg(nullptr);
        UiAlerts::configure(msg, Qt::ApplicationModal);
        msg.setWindowTitle(QCoreApplication::translate("TransferManager",
                                                       "Conflict"));
        msg.setText(QCoreApplication::translate(
                        "TransferManager",
                        "«%1» already exists.\nLocal: %2\nRemote: %3")
                        .arg(name, srcInfo, dstInfo));
        QAbstractButton *btResume = msg.addButton(
            QCoreApplication::translate("TransferManager", "Resume"),
            QMessageBox::ActionRole);
        QAbstractButton *btOverwrite = msg.addButton(
            QCoreApplication::translate("TransferManager", "Overwrite"),
            QMessageBox::AcceptRole);
        msg.addButton(QCoreApplication::translate("TransferManager", "Skip"),
                      QMessageBox::RejectRole);
        msg.exec();
        if (msg.clickedButton() == btResume)
            choice = 2;
        else if (msg.clickedButton() == btOverwrite)
            choice = 1;
        else
            choice = 0;
        return choice;
    };

    if (!uiContext || QThread::currentThread() == uiContext->thread()) {
        return showPrompt();
    }

    struct PromptState {
        std::mutex m;
        std::condition_variable cv;
        int choice = 0;
        bool done = false;
        std::atomic<bool> canceled{false};
    };

    auto state = std::make_shared<PromptState>();
    const bool invoked = QMetaObject::invokeMethod(
        uiContext,
        [state, showPrompt]() mutable {
            if (!state || state->canceled.load()) {
                if (state) {
                    std::lock_guard<std::mutex> lock(state->m);
                    state->done = true;
                    state->cv.notify_one();
                }
                return;
            }

            const int localChoice = showPrompt();
            {
                std::lock_guard<std::mutex> lock(state->m);
                state->choice = localChoice;
                state->done = true;
            }
            state->cv.notify_one();
        },
        Qt::QueuedConnection);
    if (!invoked)
        return 0;

    std::unique_lock<std::mutex> stateLock(state->m);
    using namespace std::chrono_literals;
    while (!state->done) {
        if (state->cv.wait_for(stateLock, 50ms) == std::cv_status::timeout) {
            if (shouldAbort && shouldAbort()) {
                state->canceled.store(true);
                return -1;
            }
        }
    }
    return state->choice;
}

template <typename IdSet>
static void eraseMissingIds(IdSet &ids,
                            const std::unordered_set<quint64> &remainingIds) {
    for (auto idIt = ids.begin(); idIt != ids.end();) {
        if (!remainingIds.count(*idIt))
            idIt = ids.erase(idIt);
        else
            ++idIt;
    }
}

static void pruneTrackingSets(const QVector<TransferTask> &tasks,
                              std::unordered_set<quint64> &canceledTasks,
                              std::unordered_set<quint64> &pausedTasks) {
    std::unordered_set<quint64> remainingIds;
    remainingIds.reserve(tasks.size());
    for (const auto &task : tasks)
        remainingIds.insert(task.taskId);
    eraseMissingIds(canceledTasks, remainingIds);
    eraseMissingIds(pausedTasks, remainingIds);
}

template <typename MutexType>
static bool tryLockWithRetries(std::unique_lock<MutexType> &lock,
                               int maxAttempts = 200,
                               int sleepMs = 2) {
    for (int lockAttempt = 0; lockAttempt < maxAttempts; ++lockAttempt) {
        if (lock.try_lock())
            return true;
        std::this_thread::sleep_for(std::chrono::milliseconds(sleepMs));
    }
    return false;
}

TransferManager::TransferManager(QObject *parent) : QObject(parent) {}

TransferManager::~TransferManager() {
    paused_ = true;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    {
        std::lock_guard<std::mutex> tasksLock(mtx_);
        resumeRequestedTasks_.clear();
        for (auto &task : tasks_) {
            canceledTasks_.insert(task.taskId);
            if (task.status == TransferTask::Status::Queued ||
                task.status == TransferTask::Status::Running ||
                task.status == TransferTask::Status::Paused) {
                transitionTaskToCanceled(task, nowMs);
            }
        }
    }
    interruptActiveWorkers();
    std::unordered_map<quint64, std::thread> workersToJoin;
    {
        std::lock_guard<std::mutex> workersLock(workersMutex_);
        workersToJoin.swap(workers_);
    }
    for (auto &workerEntry : workersToJoin) {
        if (workerEntry.second.joinable())
            workerEntry.second.join();
    }
    running_ = 0;
}

void TransferManager::setClient(openscp::SftpClient *client) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        client_ = client;
    }
    if (client) {
        // Re-enable queue execution after a disconnect/clearClient cycle.
        paused_ = false;
        // Reset stale running count from a previous session cleanup so new
        // queues can proceed deterministically after reconnect.
        running_.store(0);
        QMetaObject::invokeMethod(this, "schedule", Qt::QueuedConnection);
    }
}

void TransferManager::setSessionOptions(const openscp::SessionOptions &opt) {
    std::lock_guard<std::mutex> lock(mtx_);
    sessionOpt_ = opt;
}

void TransferManager::clearClient() {
    const qint64 startedAtMs = QDateTime::currentMSecsSinceEpoch();
    std::size_t activeWorkers = 0;
    {
        std::lock_guard<std::mutex> lock(activeWorkersMutex_);
        activeWorkers = activeWorkerClients_.size();
    }
    qCInfo(ocXfer) << "clearClient begin"
                   << "activeWorkers=" << activeWorkers
                   << "runningCounter=" << running_.load();
    // Signal pause/cancel so workers cooperate and finish quickly.
    paused_ = true;
    bool changed = false;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    {
        std::lock_guard<std::mutex> tasksLock(mtx_);
        // Clear connection inputs up-front so a delayed clearClient() cannot
        // wipe a newer session set by a subsequent reconnect.
        client_ = nullptr;
        sessionOpt_.reset();
        resumeRequestedTasks_.clear();
        for (auto &task : tasks_) {
            canceledTasks_.insert(task.taskId);
            if (task.status == TransferTask::Status::Queued ||
                task.status == TransferTask::Status::Running ||
                task.status == TransferTask::Status::Paused) {
                transitionTaskToCanceled(task, nowMs);
                changed = true;
            }
        }
    }
    if (changed)
        emit tasksChanged();

    // Nudge active workers so blocking I/O exits quickly on cancellation.
    interruptActiveWorkers();

    std::unordered_map<quint64, std::thread> workersToJoin;
    {
        std::lock_guard<std::mutex> workersLock(workersMutex_);
        workersToJoin.swap(workers_);
    }
    for (auto &workerEntry : workersToJoin) {
        if (workerEntry.second.joinable())
            workerEntry.second.join();
    }
    qCInfo(ocXfer) << "clearClient finished"
                   << "elapsedMs="
                   << (QDateTime::currentMSecsSinceEpoch() - startedAtMs)
                   << "runningCounter=" << running_.load();
}

QVector<TransferTask> TransferManager::tasksSnapshot() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return tasks_;
}

std::unique_ptr<openscp::SftpClient>
TransferManager::createWorkerClient(quint64 taskId, std::string &err) {
    openscp::SftpClient *base = nullptr;
    std::optional<openscp::SessionOptions> opt;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        base = client_;
        opt = sessionOpt_;
    }
    if (!base) {
        err = "No client";
        return nullptr;
    }
    if (!opt.has_value()) {
        err = "Missing session options";
        return nullptr;
    }

    using namespace std::chrono_literals;
    std::string lastErr;
    for (int attempt = 0; attempt < 3; ++attempt) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (paused_.load() || canceledTasks_.count(taskId) > 0 ||
                pausedTasks_.count(taskId) > 0) {
                err = "Transfer queue paused/canceled";
                return nullptr;
            }
        }
        std::unique_ptr<openscp::SftpClient> conn;
        {
            // Creating libssh2 clients/sessions from a single entry point
            // avoids cross-thread initialization hazards and keeps connection
            // setup deterministic.
            std::lock_guard<std::mutex> lock(connFactoryMutex_);
            conn = base->newConnectionLike(*opt, lastErr);
        }
        if (conn)
            return conn;
        if (attempt < 2) {
            {
                std::lock_guard<std::mutex> lock(mtx_);
                if (paused_.load() || canceledTasks_.count(taskId) > 0 ||
                    pausedTasks_.count(taskId) > 0) {
                    err = "Transfer queue paused/canceled";
                    return nullptr;
                }
            }
            std::this_thread::sleep_for((1 << attempt) * 500ms);
        }
    }
    err = lastErr.empty() ? "Could not create transfer connection" : lastErr;
    return nullptr;
}

void TransferManager::enqueueUpload(const QString &local,
                                    const QString &remote) {
    TransferTask task{TransferTask::Type::Upload};
    task.taskId = nextId_++;
    task.src = local;
    task.dst = remote;
    task.queuedAtMs = QDateTime::currentMSecsSinceEpoch();
    {
        // Protect the structure
        // (other functions will access concurrently)
        // mtx_ protects tasks_
        std::lock_guard<std::mutex> lock(mtx_);
        tasks_.push_back(task);
    }
    emit tasksChanged();
    if (!paused_)
        schedule();
}

void TransferManager::enqueueDownload(const QString &remote,
                                      const QString &local) {
    TransferTask task{TransferTask::Type::Download};
    task.taskId = nextId_++;
    task.src = remote;
    task.dst = local;
    task.queuedAtMs = QDateTime::currentMSecsSinceEpoch();
    {
        std::lock_guard<std::mutex> lock(mtx_);
        tasks_.push_back(task);
    }
    emit tasksChanged();
    if (!paused_)
        schedule();
}

void TransferManager::pauseAll() {
    paused_ = true;
    {
        std::lock_guard<std::mutex> tasksLock(mtx_);
        for (auto &task : tasks_) {
            if (task.status == TransferTask::Status::Running) {
                pausedTasks_.insert(task.taskId);
                transitionTaskToPaused(task);
            }
        }
    }
    emit tasksChanged();
    interruptActiveWorkers();
}

void TransferManager::resumeAll() {
    bool changed = false;
    if (paused_) {
        paused_ = false;
        changed = true;
    }
    std::unordered_set<quint64> activeNow;
    {
        std::lock_guard<std::mutex> activeWorkersLock(activeWorkersMutex_);
        activeNow.reserve(activeWorkerClients_.size());
        for (const auto &activeWorker : activeWorkerClients_)
            activeNow.insert(activeWorker.first);
    }
    {
        std::lock_guard<std::mutex> tasksLock(mtx_);
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        for (auto &task : tasks_) {
            if (task.status == TransferTask::Status::Paused) {
                if (activeNow.count(task.taskId) > 0) {
                    // Worker is still unwinding from pause; defer relaunch
                    // until it fully exits.
                    resumeRequestedTasks_.insert(task.taskId);
                } else {
                    transitionTaskToQueued(task, nowMs, true);
                    pausedTasks_.erase(task.taskId);
                    resumeRequestedTasks_.erase(task.taskId);
                    changed = true;
                }
            }
        }
    }
    if (changed)
        emit tasksChanged();
    processNext();
}

void TransferManager::cancelAll() {
    // Mark all tasks as stopped and request cooperative cancellation
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    int affected = 0;
    int runningNow = 0;
    {
        std::lock_guard<std::mutex> tasksLock(mtx_);
        resumeRequestedTasks_.clear();
        for (auto &task : tasks_) {
            canceledTasks_.insert(task.taskId);
            if (task.status == TransferTask::Status::Queued ||
                task.status == TransferTask::Status::Running ||
                task.status == TransferTask::Status::Paused) {
                ++affected;
                if (task.status == TransferTask::Status::Running)
                    ++runningNow;
                transitionTaskToCanceled(task, nowMs);
            }
        }
    }
    emit tasksChanged();
    qCInfo(ocXfer) << "cancelAll requested"
                   << "affectedTasks=" << affected
                   << "runningTasks=" << runningNow;
    interruptActiveWorkers();
}

void TransferManager::retryFailed() {
    {
        std::lock_guard<std::mutex> tasksLock(mtx_);
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        for (auto &task : tasks_) {
            if (task.status == TransferTask::Status::Error ||
                task.status == TransferTask::Status::Canceled) {
                resetTaskForRetry(task, nowMs);
                canceledTasks_.erase(task.taskId);
                pausedTasks_.erase(task.taskId);
            }
        }
    }
    emit tasksChanged();
    schedule();
}

void TransferManager::retryTask(quint64 taskId) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        int taskIndex = indexForId(taskId);
        if (taskIndex >= 0 &&
            (tasks_[taskIndex].status == TransferTask::Status::Error ||
             tasks_[taskIndex].status == TransferTask::Status::Canceled)) {
            auto &task = tasks_[taskIndex];
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            resetTaskForRetry(task, nowMs);
            canceledTasks_.erase(task.taskId);
            pausedTasks_.erase(task.taskId);
            changed = true;
        }
    }
    if (changed) {
        emit tasksChanged();
        schedule();
    }
}

void TransferManager::clearCompleted() {
    {
        std::lock_guard<std::mutex> tasksLock(mtx_);
        QVector<TransferTask> next;
        next.reserve(tasks_.size());
        for (const auto &task : tasks_) {
            if (task.status != TransferTask::Status::Done)
                next.push_back(task);
        }
        tasks_.swap(next);
        pruneTrackingSets(tasks_, canceledTasks_, pausedTasks_);
    }
    emit tasksChanged();
}

void TransferManager::clearFailedCanceled() {
    {
        std::lock_guard<std::mutex> tasksLock(mtx_);
        QVector<TransferTask> next;
        next.reserve(tasks_.size());
        for (const auto &task : tasks_) {
            if (task.status != TransferTask::Status::Error &&
                task.status != TransferTask::Status::Canceled)
                next.push_back(task);
        }
        tasks_.swap(next);
        pruneTrackingSets(tasks_, canceledTasks_, pausedTasks_);
    }
    emit tasksChanged();
}

void TransferManager::clearFinishedOlderThan(int minutes, bool clearDone,
                                             bool clearFailedCanceled) {
    if (minutes <= 0 || (!clearDone && !clearFailedCanceled))
        return;
    const qint64 cutoff =
        QDateTime::currentMSecsSinceEpoch() - qint64(minutes) * 60 * 1000;
    bool changed = false;
    {
        std::lock_guard<std::mutex> tasksLock(mtx_);
        QVector<TransferTask> next;
        next.reserve(tasks_.size());
        for (const auto &task : tasks_) {
            const bool isDone = (task.status == TransferTask::Status::Done);
            const bool isFailed =
                (task.status == TransferTask::Status::Error ||
                 task.status == TransferTask::Status::Canceled);
            const bool candidate =
                (clearDone && isDone) || (clearFailedCanceled && isFailed);
            const bool oldEnough =
                (task.finishedAtMs > 0 && task.finishedAtMs <= cutoff);
            if (candidate && oldEnough) {
                changed = true;
                continue;
            }
            next.push_back(task);
        }
        tasks_.swap(next);
        if (changed)
            pruneTrackingSets(tasks_, canceledTasks_, pausedTasks_);
    }
    if (changed)
        emit tasksChanged();
}

void TransferManager::processNext() {
    // Delegate to the concurrent scheduler
    schedule();
}

int TransferManager::nextQueuedTaskIndexLocked() {
    if (tasks_.isEmpty()) {
        schedulingCursor_ = 0;
        return -1;
    }
    if (schedulingCursor_ < 0 || schedulingCursor_ >= tasks_.size())
        schedulingCursor_ = 0;

    const int total = tasks_.size();
    for (int off = 0; off < total; ++off) {
        const int queueIndex = (schedulingCursor_ + off) % total;
        if (tasks_[queueIndex].status == TransferTask::Status::Queued) {
            schedulingCursor_ = (queueIndex + 1) % total;
            return queueIndex;
        }
    }
    return -1;
}

void TransferManager::recordCompletionMetrics(quint64 taskId,
                                              TransferTask::Status status,
                                              quint64 bytesDone,
                                              qint64 queueLatencyMs,
                                              qint64 precheckMs,
                                              qint64 transferMs) {
    if (queueLatencyMs < 0)
        queueLatencyMs = 0;
    if (precheckMs < 0)
        precheckMs = 0;
    if (transferMs < 0)
        transferMs = 0;

    const double throughputKBps =
        (transferMs > 0)
            ? (double(bytesDone) / 1024.0) /
                  (double(transferMs) / 1000.0)
            : 0.0;

    qCInfo(ocXfer) << "task metrics"
                   << "taskId=" << taskId
                   << "status=" << transferStatusName(status)
                   << "queueLatencyMs=" << queueLatencyMs
                   << "precheckMs=" << precheckMs
                   << "transferMs=" << transferMs
                   << "bytesDone=" << bytesDone
                   << "throughputKBps=" << throughputKBps;

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    std::lock_guard<std::mutex> lock(perfMtx_);
    perfCompletedTasks_ += 1;
    perfCompletedBytes_ += bytesDone;
    perfTotalQueueLatencyMs_ += queueLatencyMs;
    perfTotalPrecheckMs_ += precheckMs;
    perfTotalTransferMs_ += transferMs;

    const bool periodic =
        ((nowMs - perfLastLogAtMs_) >= 10000) || (perfCompletedTasks_ % 10 == 0);
    if (!periodic)
        return;

    const double completedTaskCount = double(perfCompletedTasks_);
    const double avgQueueMs = perfTotalQueueLatencyMs_ / completedTaskCount;
    const double avgPrecheckMs = perfTotalPrecheckMs_ / completedTaskCount;
    const double avgTransferMs = perfTotalTransferMs_ / completedTaskCount;
    const double aggThroughputKBps =
        (perfTotalTransferMs_ > 0)
            ? (double(perfCompletedBytes_) / 1024.0) /
                  (double(perfTotalTransferMs_) / 1000.0)
            : 0.0;

    perfLastLogAtMs_ = nowMs;
    qCInfo(ocXfer) << "queue metrics aggregate"
                   << "completedTasks=" << perfCompletedTasks_
                   << "avgQueueLatencyMs=" << avgQueueMs
                   << "avgPrecheckMs=" << avgPrecheckMs
                   << "avgTransferMs=" << avgTransferMs
                   << "aggThroughputKBps=" << aggThroughputKBps
                   << "runningCounter=" << running_.load();
}

void TransferManager::transitionTaskToRunning(TransferTask &task, qint64 nowMs) {
    task.status = TransferTask::Status::Running;
    task.progress = 0;
    task.bytesDone = 0;
    task.bytesTotal = 0;
    task.currentSpeedKBps = 0.0;
    task.etaSeconds = -1;
    task.error.clear();
    task.startedAtMs = nowMs;
    task.finishedAtMs = 0;
}

void TransferManager::transitionTaskToQueued(TransferTask &task, qint64 nowMs,
                                             bool resumeHint) {
    task.status = TransferTask::Status::Queued;
    task.resumeHint = resumeHint;
    task.queuedAtMs = nowMs;
    task.startedAtMs = 0;
    task.finishedAtMs = 0;
}

void TransferManager::transitionTaskToPaused(TransferTask &task) {
    task.status = TransferTask::Status::Paused;
    task.error.clear();
    task.currentSpeedKBps = 0.0;
    task.etaSeconds = -1;
    task.finishedAtMs = 0;
}

void TransferManager::transitionTaskToCanceled(TransferTask &task,
                                               qint64 nowMs) {
    task.status = TransferTask::Status::Canceled;
    task.error.clear();
    task.currentSpeedKBps = 0.0;
    task.etaSeconds = -1;
    task.finishedAtMs = nowMs;
}

void TransferManager::transitionTaskToError(TransferTask &task,
                                            const std::string &rawErr,
                                            qint64 nowMs) {
    task.status = TransferTask::Status::Error;
    task.error = transferErrorForUi(rawErr);
    task.currentSpeedKBps = 0.0;
    task.etaSeconds = -1;
    task.finishedAtMs = nowMs;
}

void TransferManager::transitionTaskToDone(TransferTask &task, qint64 nowMs,
                                           bool preserveProgress) {
    if (!preserveProgress) {
        task.progress = 100;
        if (task.bytesTotal > 0)
            task.bytesDone = task.bytesTotal;
    }
    task.status = TransferTask::Status::Done;
    task.currentSpeedKBps = 0.0;
    task.etaSeconds = 0;
    task.finishedAtMs = nowMs;
}

void TransferManager::resetTaskForRetry(TransferTask &task, qint64 nowMs) {
    const bool previousResumeHint = task.resumeHint;
    task.attempts = 0;
    task.progress = 0;
    task.bytesDone = 0;
    task.bytesTotal = 0;
    task.currentSpeedKBps = 0.0;
    task.etaSeconds = -1;
    task.error.clear();
    transitionTaskToQueued(task, nowMs, previousResumeHint);
}

TransferManager::SchedulePickResult TransferManager::pickTaskForSchedule() {
    SchedulePickResult result;
    std::lock_guard<std::mutex> lock(mtx_);
    if (!client_) {
        result.outcome = SchedulePickResult::Outcome::NoClient;
        return result;
    }

    const int queueIndex = nextQueuedTaskIndexLocked();
    if (queueIndex < 0) {
        result.outcome = SchedulePickResult::Outcome::NoQueuedTask;
        return result;
    }

    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    result.task = tasks_[queueIndex];
    transitionTaskToRunning(tasks_[queueIndex], nowMs);
    result.outcome = SchedulePickResult::Outcome::Ready;
    return result;
}

bool TransferManager::validateWorkerLaunch(quint64 taskId) {
    const bool workerActiveNow = isWorkerActive(taskId);
    std::lock_guard<std::mutex> workersLock(workersMutex_);
    auto workerIt = workers_.find(taskId);
    if (workerIt != workers_.end() && workerIt->second.joinable()) {
        if (workerActiveNow) {
            // Do not block UI by joining an active worker; defer this relaunch
            // until the existing worker fully exits.
            std::lock_guard<std::mutex> lock(mtx_);
            const int taskIndex = indexForId(taskId);
            if (taskIndex >= 0) {
                transitionTaskToPaused(tasks_[taskIndex]);
                pausedTasks_.insert(taskId);
                resumeRequestedTasks_.insert(taskId);
            }
            return false;
        }
        qCInfo(ocXfer) << "schedule joining finished worker thread"
                       << "taskId=" << taskId;
        workerIt->second.join();
    }
    return true;
}

void TransferManager::startTaskWorker(const TransferTask &task) {
    std::lock_guard<std::mutex> workersLock(workersMutex_);
    auto &workerSlot = workers_[task.taskId];
    if (workerSlot.joinable()) {
        qCWarning(ocXfer) << "startTaskWorker found stale joinable worker"
                          << "taskId=" << task.taskId;
        workerSlot.join();
    }
    workerSlot = std::thread(&TransferManager::runTaskWorkerPipeline, this, task);
}

void TransferManager::finalizeDeferredRelaunch(quint64 taskId) {
    emit tasksChanged();
    qCInfo(ocXfer) << "schedule deferred relaunch; worker still active"
                   << "taskId=" << taskId;
    decrementRunningCounter();
}

void TransferManager::finalizeWorkerRun(quint64 taskId, qint64 precheckMs,
                                        qint64 transferStartMs) {
    qint64 transferMs = 0;
    if (transferStartMs > 0) {
        transferMs = QDateTime::currentMSecsSinceEpoch() - transferStartMs;
    }

    TransferTask::Status finalStatus = TransferTask::Status::Error;
    quint64 bytesDone = 0;
    qint64 queueLatencyMs = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const int taskIndex = indexForId(taskId);
        if (taskIndex >= 0) {
            finalStatus = tasks_[taskIndex].status;
            bytesDone = tasks_[taskIndex].bytesDone;
            if (tasks_[taskIndex].queuedAtMs > 0 &&
                tasks_[taskIndex].startedAtMs >= tasks_[taskIndex].queuedAtMs) {
                queueLatencyMs = tasks_[taskIndex].startedAtMs -
                                 tasks_[taskIndex].queuedAtMs;
            }
        }
    }
    emit tasksChanged();
    recordCompletionMetrics(taskId, finalStatus, bytesDone, queueLatencyMs,
                            precheckMs, transferMs);

    decrementRunningCounter();
    QMetaObject::invokeMethod(this, "schedule", Qt::QueuedConnection);
}

bool TransferManager::shouldCancelWorkerTask(quint64 taskId) {
    if (paused_.load())
        return true;
    std::lock_guard<std::mutex> lock(mtx_);
    return canceledTasks_.count(taskId) > 0 || pausedTasks_.count(taskId) > 0;
}

void TransferManager::markTaskCanceledOrPausedFromWorker(quint64 taskId,
                                                         qint64 nowMs) {
    std::lock_guard<std::mutex> lock(mtx_);
    const int taskIndex = indexForId(taskId);
    if (taskIndex < 0)
        return;
    const bool canceled = canceledTasks_.count(taskId) > 0;
    if (canceled)
        transitionTaskToCanceled(tasks_[taskIndex], nowMs);
    else
        transitionTaskToPaused(tasks_[taskIndex]);
}

void TransferManager::markTaskErrorFromWorker(quint64 taskId,
                                              const std::string &rawErr,
                                              qint64 nowMs) {
    std::lock_guard<std::mutex> lock(mtx_);
    const int taskIndex = indexForId(taskId);
    if (taskIndex < 0)
        return;
    transitionTaskToError(tasks_[taskIndex], rawErr, nowMs);
}

void TransferManager::markTaskDoneFromWorker(quint64 taskId, qint64 nowMs) {
    std::lock_guard<std::mutex> lock(mtx_);
    const int taskIndex = indexForId(taskId);
    if (taskIndex < 0)
        return;
    transitionTaskToDone(tasks_[taskIndex], nowMs);
}

bool TransferManager::runWorkerPrecheckStage(WorkerPipelineContext &ctx) {
    ctx.resume = ctx.task.resumeHint;
    ctx.precheckStartedMs = QDateTime::currentMSecsSinceEpoch();
    ctx.precheckDoneMs = ctx.precheckStartedMs;

    auto finalizeEarly = [this, &ctx]() {
        finalizeWorkerRun(ctx.taskId, ctx.precheckDoneMs - ctx.precheckStartedMs,
                          0);
    };
    auto failPrecheck = [this, &ctx, &finalizeEarly](const std::string &rawErr) {
        ctx.precheckDoneMs = QDateTime::currentMSecsSinceEpoch();
        if (shouldCancelWorkerTask(ctx.taskId))
            markTaskCanceledOrPausedFromWorker(ctx.taskId, ctx.precheckDoneMs);
        else
            markTaskErrorFromWorker(ctx.taskId, rawErr, ctx.precheckDoneMs);
        ctx.workerClient->disconnect();
        finalizeEarly();
    };
    auto skipTransfer = [this, &ctx, &finalizeEarly]() {
        ctx.precheckDoneMs = QDateTime::currentMSecsSinceEpoch();
        {
            std::lock_guard<std::mutex> lock(mtx_);
            const int taskIndex = indexForId(ctx.taskId);
            if (taskIndex >= 0)
                transitionTaskToDone(tasks_[taskIndex], ctx.precheckDoneMs,
                                     /*preserveProgress=*/true);
        }
        ctx.workerClient->disconnect();
        finalizeEarly();
    };

    if (ctx.task.type == TransferTask::Type::Upload) {
        if (ctx.workerCaps.supports_metadata) {
            bool isDir = false;
            std::string existsErr;
            const bool existsRemote = ctx.workerClient->exists(
                ctx.task.dst.toStdString(), isDir, existsErr);
            if (!existsErr.empty()) {
                failPrecheck(existsErr);
                return false;
            }

            if (existsRemote) {
                openscp::FileInfo rinfo{};
                std::string statErr;
                (void)ctx.workerClient->stat(ctx.task.dst.toStdString(), rinfo,
                                             statErr);
                const QString srcInfo =
                    QString("%1 bytes, %2")
                        .arg(QFileInfo(ctx.task.src).size())
                        .arg(openscpui::localShortTime(
                            QFileInfo(ctx.task.src).lastModified()));
                const QString dstInfo =
                    QString("%1 bytes, %2")
                        .arg(rinfo.size)
                        .arg(rinfo.mtime
                                 ? openscpui::localShortTime((quint64)rinfo.mtime)
                                 : QStringLiteral("?"));
                int choice = askOverwriteConflictOnUi(
                    this, QFileInfo(ctx.task.src).fileName(), srcInfo, dstInfo,
                    [this, &ctx]() { return shouldCancelWorkerTask(ctx.taskId); });
                if (choice < 0 || shouldCancelWorkerTask(ctx.taskId)) {
                    ctx.precheckDoneMs = QDateTime::currentMSecsSinceEpoch();
                    markTaskCanceledOrPausedFromWorker(ctx.taskId,
                                                       ctx.precheckDoneMs);
                    ctx.workerClient->disconnect();
                    finalizeEarly();
                    return false;
                }
                if (choice == 0) {
                    skipTransfer();
                    return false;
                }
                if (choice == 2 && !ctx.workerCaps.supports_resume)
                    choice = 1;
                ctx.resume = (choice == 2);
            }
        }

        if (ctx.workerCaps.supports_metadata) {
            auto ensureRemoteDir = [&](const QString &dir,
                                       std::string &ensureErr) -> bool {
                if (dir.isEmpty())
                    return true;
                QString currentPath = "/";
                const QStringList parts = dir.split('/', Qt::SkipEmptyParts);
                for (const QString &part : parts) {
                    const QString nextPath = (currentPath == "/")
                                                 ? ("/" + part)
                                                 : (currentPath + "/" + part);
                    bool isDirectory = false;
                    std::string existsErr;
                    const bool exists = ctx.workerClient->exists(
                        nextPath.toStdString(), isDirectory, existsErr);
                    if (!existsErr.empty()) {
                        ensureErr = existsErr;
                        return false;
                    }
                    if (!exists) {
                        std::string mkdirErr;
                        if (!ctx.workerClient->mkdir(nextPath.toStdString(),
                                                     mkdirErr, 0755)) {
                            ensureErr = mkdirErr.empty()
                                            ? ("Could not create remote "
                                               "directory: " +
                                               nextPath.toStdString())
                                            : mkdirErr;
                            return false;
                        }
                    } else if (!isDirectory) {
                        ensureErr = "Remote path component is not a directory: " +
                                    nextPath.toStdString();
                        return false;
                    }
                    currentPath = nextPath;
                }
                return true;
            };

            const QString parentDir = QFileInfo(ctx.task.dst).path();
            if (!parentDir.isEmpty()) {
                std::string ensureErr;
                if (!ensureRemoteDir(parentDir, ensureErr)) {
                    failPrecheck(ensureErr);
                    return false;
                }
            }
        }
    } else {
        const QFileInfo localDestinationInfo(ctx.task.dst);
        if (localDestinationInfo.exists()) {
            QString srcInfo = QStringLiteral("? bytes, ?");
            if (ctx.workerCaps.supports_metadata) {
                openscp::FileInfo remoteInfo{};
                std::string statErr;
                (void)ctx.workerClient->stat(ctx.task.src.toStdString(),
                                             remoteInfo, statErr);
                srcInfo = QString("%1 bytes, %2")
                              .arg(remoteInfo.size)
                              .arg(remoteInfo.mtime
                                       ? openscpui::localShortTime(
                                             (quint64)remoteInfo.mtime)
                                       : QStringLiteral("?"));
            }
            const QString dstInfo =
                QString("%1 bytes, %2")
                    .arg(localDestinationInfo.size())
                    .arg(openscpui::localShortTime(
                        localDestinationInfo.lastModified()));
            int choice = askOverwriteConflictOnUi(
                this, localDestinationInfo.fileName(), srcInfo, dstInfo,
                [this, &ctx]() { return shouldCancelWorkerTask(ctx.taskId); });
            if (choice < 0 || shouldCancelWorkerTask(ctx.taskId)) {
                ctx.precheckDoneMs = QDateTime::currentMSecsSinceEpoch();
                markTaskCanceledOrPausedFromWorker(ctx.taskId, ctx.precheckDoneMs);
                ctx.workerClient->disconnect();
                finalizeEarly();
                return false;
            }
            if (choice == 0) {
                skipTransfer();
                return false;
            }
            if (choice == 2 && !ctx.workerCaps.supports_resume)
                choice = 1;
            ctx.resume = (choice == 2);
        }
        if (!QDir().mkpath(QFileInfo(ctx.task.dst).dir().absolutePath())) {
            failPrecheck("Could not create local destination directory");
            return false;
        }
    }

    if (ctx.resume && !ctx.workerCaps.supports_resume)
        ctx.resume = false;
    ctx.precheckDoneMs = QDateTime::currentMSecsSinceEpoch();
    return true;
}

void TransferManager::runWorkerTransferStage(WorkerPipelineContext &ctx) {
    using clock = std::chrono::steady_clock;
    static constexpr double KIB = 1024.0;
    std::size_t lastDone = 0;
    auto lastTick = clock::now();
    auto progress = [this, &ctx, lastTick, lastDone](std::size_t done,
                                                      std::size_t total) mutable {
        int pct = (total > 0) ? int((done * 100) / total) : 0;
        const auto now = clock::now();
        const double elapsedSec =
            std::chrono::duration_cast<std::chrono::duration<double>>(now -
                                                                       lastTick)
                .count();
        const double deltaBytes =
            (done > lastDone) ? double(done - lastDone) : 0.0;
        double measuredKBps = 0.0;
        if (elapsedSec > 0.000001 && deltaBytes > 0.0)
            measuredKBps = (deltaBytes / KIB) / elapsedSec;
        int etaSec = -1;
        if (total > done && measuredKBps > 0.0)
            etaSec = int((double(total - done) / KIB) / measuredKBps);
        else if (total > 0 && done >= total)
            etaSec = 0;

        {
            std::lock_guard<std::mutex> lock(mtx_);
            const int taskIndex = indexForId(ctx.taskId);
            if (taskIndex >= 0) {
                tasks_[taskIndex].progress = pct;
                tasks_[taskIndex].bytesDone = done;
                tasks_[taskIndex].bytesTotal = total;
                if (measuredKBps > 0.0)
                    tasks_[taskIndex].currentSpeedKBps = measuredKBps;
                tasks_[taskIndex].etaSeconds = etaSec;
            }
        }
        emit tasksChanged();

        int taskLimit = 0; // KB/s (0 = unlimited)
        const int globalLimit = globalSpeedKBps_.load();
        {
            std::lock_guard<std::mutex> lock(mtx_);
            const int taskIndex = indexForId(ctx.taskId);
            if (taskIndex >= 0)
                taskLimit = tasks_[taskIndex].speedLimitKBps;
        }
        int effectiveKBps = 0;
        if (taskLimit > 0 && globalLimit > 0)
            effectiveKBps = std::min(taskLimit, globalLimit);
        else
            effectiveKBps =
                (taskLimit > 0 ? taskLimit : (globalLimit > 0 ? globalLimit : 0));
        if (effectiveKBps > 0 && done > lastDone) {
            const auto now2 = clock::now();
            const double deltaBytes2 = double(done - lastDone);
            const double expectedSec = deltaBytes2 / (effectiveKBps * KIB);
            const double elapsedSec2 =
                std::chrono::duration_cast<std::chrono::duration<double>>(now2 -
                                                                           lastTick)
                    .count();
            if (elapsedSec2 < expectedSec) {
                const double sleepSec = expectedSec - elapsedSec2;
                if (sleepSec > 0.0005)
                    std::this_thread::sleep_for(
                        std::chrono::duration<double>(sleepSec));
            }
            lastTick = clock::now();
            lastDone = done;
        }
    };

    ctx.transferStartedMs = QDateTime::currentMSecsSinceEpoch();
    if (ctx.task.type == TransferTask::Type::Upload) {
        std::string putError;
        const bool uploadSucceeded =
            ctx.workerClient->put(ctx.task.src.toStdString(),
                                  ctx.task.dst.toStdString(), putError, progress,
                                  [this, &ctx]() {
                                      return shouldCancelWorkerTask(ctx.taskId);
                                  },
                                  ctx.resume);
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (!uploadSucceeded && shouldCancelWorkerTask(ctx.taskId))
            markTaskCanceledOrPausedFromWorker(ctx.taskId, nowMs);
        else if (!uploadSucceeded)
            markTaskErrorFromWorker(ctx.taskId, putError, nowMs);
        else
            markTaskDoneFromWorker(ctx.taskId, nowMs);
        return;
    }

    std::string getError;
    const bool downloadSucceeded =
        ctx.workerClient->get(ctx.task.src.toStdString(),
                              ctx.task.dst.toStdString(), getError, progress,
                              [this, &ctx]() {
                                  return shouldCancelWorkerTask(ctx.taskId);
                              },
                              ctx.resume);
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    if (!downloadSucceeded && shouldCancelWorkerTask(ctx.taskId)) {
        markTaskCanceledOrPausedFromWorker(ctx.taskId, nowMs);
        return;
    }
    if (!downloadSucceeded) {
        markTaskErrorFromWorker(ctx.taskId, getError, nowMs);
        return;
    }

    openscp::FileInfo remoteInfo{};
    std::string statErr;
    (void)ctx.workerClient->stat(ctx.task.src.toStdString(), remoteInfo, statErr);
    if (remoteInfo.mtime > 0) {
        QFile localFile(ctx.task.dst);
        if (localFile.exists()) {
            const QDateTime tsUtc = QDateTime::fromSecsSinceEpoch(
                (qint64)remoteInfo.mtime, QTimeZone::utc());
            if (!localFile.setFileTime(tsUtc,
                                       QFileDevice::FileModificationTime)) {
                if (openscp::sensitiveLoggingEnabled()) {
                    qWarning(ocXfer) << "Failed to set mtime for" << ctx.task.dst
                                     << "to" << tsUtc;
                } else {
                    qWarning(ocXfer) << "Failed to set local file mtime";
                }
            }
        }
    }
    markTaskDoneFromWorker(ctx.taskId, nowMs);
}

void TransferManager::runWorkerPostStage(WorkerPipelineContext &ctx) {
    ctx.workerClient->disconnect();
    std::lock_guard<std::mutex> lock(mtx_);
    auto itResume = resumeRequestedTasks_.find(ctx.taskId);
    if (itResume == resumeRequestedTasks_.end())
        return;

    const int taskIndex = indexForId(ctx.taskId);
    if (taskIndex >= 0 && tasks_[taskIndex].status == TransferTask::Status::Paused) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        transitionTaskToQueued(tasks_[taskIndex], nowMs, true);
        pausedTasks_.erase(ctx.taskId);
    }
    resumeRequestedTasks_.erase(itResume);
    qCInfo(ocXfer) << "Deferred resume armed after worker unwind"
                   << "taskId=" << ctx.taskId;
}

void TransferManager::runTaskWorkerPipeline(TransferTask task) {
    WorkerPipelineContext ctx;
    ctx.task = std::move(task);
    ctx.taskId = ctx.task.taskId;

    struct ActiveWorkerGuard {
        TransferManager *self = nullptr;
        quint64 taskId = 0;
        ~ActiveWorkerGuard() {
            if (!self)
                return;
            std::lock_guard<std::mutex> lock(self->activeWorkersMutex_);
            const std::size_t prevCount = self->activeWorkerTaskIds_.size();
            self->activeWorkerClients_.erase(taskId);
            self->activeWorkerTaskIds_.erase(taskId);
            self->pendingInterruptTasks_.erase(taskId);
            qCInfo(ocXfer) << "worker active cleared"
                           << "taskId=" << taskId
                           << "prevActiveCount=" << prevCount
                           << "activeCount="
                           << self->activeWorkerTaskIds_.size();
        }
    };

    std::string createError;
    {
        std::lock_guard<std::mutex> lock(activeWorkersMutex_);
        activeWorkerTaskIds_.insert(ctx.taskId);
        qCInfo(ocXfer) << "worker active registered"
                       << "taskId=" << ctx.taskId
                       << "activeCount=" << activeWorkerTaskIds_.size();
    }

    auto workerClientUnique = createWorkerClient(ctx.taskId, createError);
    if (!workerClientUnique) {
        {
            std::lock_guard<std::mutex> lock(activeWorkersMutex_);
            activeWorkerTaskIds_.erase(ctx.taskId);
            pendingInterruptTasks_.erase(ctx.taskId);
        }
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        if (shouldCancelWorkerTask(ctx.taskId))
            markTaskCanceledOrPausedFromWorker(ctx.taskId, nowMs);
        else
            markTaskErrorFromWorker(ctx.taskId, createError, nowMs);
        finalizeWorkerRun(ctx.taskId, 0, 0);
        return;
    }
    ctx.workerClient = std::move(workerClientUnique);

    std::optional<ActiveWorkerGuard> activeGuard;
    bool applyDeferredInterrupt = false;
    {
        std::lock_guard<std::mutex> lock(activeWorkersMutex_);
        activeWorkerClients_[ctx.taskId] = ctx.workerClient;
        applyDeferredInterrupt = pendingInterruptTasks_.erase(ctx.taskId) > 0;
    }
    if (applyDeferredInterrupt) {
        qCInfo(ocXfer) << "Applying deferred interrupt to worker"
                       << "taskId=" << ctx.taskId;
        ctx.workerClient->interrupt();
    }

    // Construct in-place to avoid a temporary guard whose destructor would
    // clear active state immediately.
    activeGuard.emplace();
    activeGuard->self = this;
    activeGuard->taskId = ctx.taskId;
    ctx.workerCaps = ctx.workerClient->capabilities();

    {
        std::lock_guard<std::mutex> lock(mtx_);
        const int taskIndex = indexForId(ctx.taskId);
        if (taskIndex >= 0)
            tasks_[taskIndex].attempts += 1;
    }
    emit tasksChanged();

    if (!runWorkerPrecheckStage(ctx))
        return;
    runWorkerTransferStage(ctx);
    runWorkerPostStage(ctx);
    finalizeWorkerRun(ctx.taskId, ctx.precheckDoneMs - ctx.precheckStartedMs,
                      ctx.transferStartedMs);
}

void TransferManager::schedule() {
    if (paused_)
        return;

    while (running_.load() < maxConcurrent_) {
        // Stage 1: pick the next queued task and mark it as running.
        const SchedulePickResult picked = pickTaskForSchedule();
        if (picked.outcome == SchedulePickResult::Outcome::NoClient)
            return;
        if (picked.outcome == SchedulePickResult::Outcome::NoQueuedTask)
            break;

        // Stage 2: publish the running transition and reserve a worker slot.
        emit tasksChanged();
        running_.fetch_add(1);

        const quint64 taskId = picked.task.taskId;
        // Stage 3: validate relaunch constraints for the selected task.
        if (!validateWorkerLaunch(taskId)) {
            // Stage 4 (deferred path): finalize this scheduling attempt.
            finalizeDeferredRelaunch(taskId);
            continue;
        }
        // Stage 4 (run path): launch the worker pipeline.
        startTaskWorker(picked.task);
    }
}

int TransferManager::indexForId(quint64 taskId) const {
    for (int taskIndex = 0; taskIndex < tasks_.size(); ++taskIndex)
        if (tasks_[taskIndex].taskId == taskId)
            return taskIndex;
    return -1;
}

void TransferManager::decrementRunningCounter() {
    int current = running_.load();
    while (current > 0 &&
           !running_.compare_exchange_weak(current, current - 1)) {
    }
    if (current <= 0)
        running_.store(0);
}

void TransferManager::interruptActiveWorker(quint64 taskId) {
    qCInfo(ocXfer) << "interruptActiveWorker requested" << "taskId=" << taskId;
    std::unique_lock<std::mutex> activeWorkersLock(activeWorkersMutex_,
                                                   std::defer_lock);
    if (!tryLockWithRetries(activeWorkersLock)) {
        qWarning(ocXfer)
            << "interruptActiveWorker lock timeout; cooperative cancel only"
            << "taskId=" << taskId;
        return;
    }
    qCInfo(ocXfer) << "interruptActiveWorker lock acquired"
                   << "taskId=" << taskId;
    std::shared_ptr<openscp::SftpClient> clientToInterrupt;
    bool deferred = false;
    const bool active = activeWorkerTaskIds_.count(taskId) > 0;
    qCInfo(ocXfer) << "interruptActiveWorker state"
                   << "taskId=" << taskId
                   << "active=" << active
                   << "activeCount=" << activeWorkerTaskIds_.size()
                   << "clientsCount=" << activeWorkerClients_.size();
    auto clientIt = activeWorkerClients_.find(taskId);
    if (clientIt != activeWorkerClients_.end())
        clientToInterrupt = clientIt->second.lock();
    if (!active || !clientToInterrupt) {
        pendingInterruptTasks_.insert(taskId);
        deferred = true;
        qCInfo(ocXfer) << "interruptActiveWorker pending set"
                       << "taskId=" << taskId;
    }
    activeWorkersLock.unlock();
    if (clientToInterrupt) {
        qCInfo(ocXfer) << "interruptActiveWorker immediate"
                       << "taskId=" << taskId;
        clientToInterrupt->interrupt();
    } else if (deferred) {
        qCInfo(ocXfer) << "interruptActiveWorker deferred"
                       << "taskId=" << taskId;
    }
}

void TransferManager::interruptActiveWorkers() {
    qCInfo(ocXfer) << "interruptActiveWorkers requested";
    std::unique_lock<std::mutex> activeWorkersLock(activeWorkersMutex_,
                                                   std::defer_lock);
    if (!tryLockWithRetries(activeWorkersLock)) {
        qWarning(ocXfer)
            << "interruptActiveWorkers lock timeout; cooperative cancel only";
        return;
    }
    qCInfo(ocXfer) << "interruptActiveWorkers lock acquired";
    std::vector<std::pair<quint64, std::shared_ptr<openscp::SftpClient>>>
        immediate;
    std::vector<quint64> deferred;
    qCInfo(ocXfer) << "interruptActiveWorkers"
                   << "activeCount=" << activeWorkerTaskIds_.size();
    immediate.reserve(activeWorkerTaskIds_.size());
    deferred.reserve(activeWorkerTaskIds_.size());
    for (quint64 taskId : activeWorkerTaskIds_) {
        std::shared_ptr<openscp::SftpClient> client;
        auto clientIt = activeWorkerClients_.find(taskId);
        if (clientIt != activeWorkerClients_.end())
            client = clientIt->second.lock();
        if (client) {
            immediate.emplace_back(taskId, std::move(client));
        } else {
            pendingInterruptTasks_.insert(taskId);
            deferred.push_back(taskId);
        }
    }
    activeWorkersLock.unlock();
    for (const auto &entry : immediate) {
        qCInfo(ocXfer) << "interruptActiveWorkers immediate taskId="
                       << entry.first;
        entry.second->interrupt();
    }
    for (quint64 taskId : deferred) {
        qCInfo(ocXfer) << "interruptActiveWorkers deferred taskId=" << taskId;
    }
}

bool TransferManager::isWorkerActive(quint64 taskId) {
    std::lock_guard<std::mutex> lock(activeWorkersMutex_);
    return activeWorkerTaskIds_.count(taskId) > 0;
}

void TransferManager::pauseTask(quint64 taskId) {
    bool changed = false;
    bool shouldInterrupt = false;
    TransferTask::Status previousStatus = TransferTask::Status::Queued;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        int taskIndex = indexForId(taskId);
        if (taskIndex >= 0) {
            previousStatus = tasks_[taskIndex].status;
            if (tasks_[taskIndex].status == TransferTask::Status::Queued ||
                tasks_[taskIndex].status == TransferTask::Status::Running) {
                resumeRequestedTasks_.erase(taskId);
                pausedTasks_.insert(taskId);
                transitionTaskToPaused(tasks_[taskIndex]);
                changed = true;
                shouldInterrupt =
                    (previousStatus == TransferTask::Status::Running);
            }
        }
    }
    if (!changed)
        return;
    emit tasksChanged();
    qCInfo(ocXfer) << "Pause requested"
                   << "taskId=" << taskId
                   << "prevStatus=" << transferStatusName(previousStatus);
    if (shouldInterrupt)
        interruptActiveWorker(taskId);
}

void TransferManager::resumeTask(quint64 taskId) {
    const bool active = isWorkerActive(taskId);
    bool changed = false;
    bool queueNow = false;
    bool deferred = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        int taskIndex = indexForId(taskId);
        if (taskIndex >= 0 &&
            tasks_[taskIndex].status == TransferTask::Status::Paused) {
            if (active) {
                // Still finishing pause teardown; relaunch when worker exits.
                resumeRequestedTasks_.insert(taskId);
                deferred = true;
            } else {
                const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                pausedTasks_.erase(taskId);
                transitionTaskToQueued(tasks_[taskIndex], nowMs, true);
                resumeRequestedTasks_.erase(taskId);
                changed = true;
                queueNow = true;
            }
        }
    }
    if (!changed && !deferred)
        return;
    if (deferred) {
        qCInfo(ocXfer) << "Resume deferred; worker still active"
                       << "taskId=" << taskId;
    } else {
        qCInfo(ocXfer) << "Resume queued immediately" << "taskId=" << taskId;
    }
    emit tasksChanged();
    if (queueNow)
        schedule();
}

void TransferManager::setTaskSpeedLimit(quint64 taskId, int kbps) {
    {
        std::lock_guard<std::mutex> lock(mtx_);
        int taskIndex = indexForId(taskId);
        if (taskIndex >= 0)
            tasks_[taskIndex].speedLimitKBps = kbps;
    }
    emit tasksChanged();
}

void TransferManager::cancelTask(quint64 taskId) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const bool activeWorker = isWorkerActive(taskId);
    bool found = false;
    bool transitionedToCanceled = false;
    bool shouldInterrupt = false;
    TransferTask::Status previousStatus = TransferTask::Status::Queued;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        int taskIndex = indexForId(taskId);
        if (taskIndex >= 0) {
            found = true;
            previousStatus = tasks_[taskIndex].status;
            if (tasks_[taskIndex].status == TransferTask::Status::Queued ||
                tasks_[taskIndex].status == TransferTask::Status::Running ||
                tasks_[taskIndex].status == TransferTask::Status::Paused) {
                resumeRequestedTasks_.erase(taskId);
                canceledTasks_.insert(taskId);
                pausedTasks_.erase(taskId);
                transitionTaskToCanceled(tasks_[taskIndex], nowMs);
                transitionedToCanceled = true;
                shouldInterrupt =
                    (previousStatus == TransferTask::Status::Running ||
                     previousStatus == TransferTask::Status::Paused);
            }
        }
    }
    if (transitionedToCanceled)
        emit tasksChanged();
    qCInfo(ocXfer) << "cancelTask requested"
                   << "taskId=" << taskId
                   << "found=" << found
                   << "prevStatus=" << transferStatusName(previousStatus)
                   << "activeWorker=" << activeWorker
                   << "transitionedToCanceled=" << transitionedToCanceled;
    if (shouldInterrupt)
        interruptActiveWorker(taskId);
}
