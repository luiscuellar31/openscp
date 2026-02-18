// Queue implementation: schedules concurrent worker transfers with isolated
// SFTP sessions.
#include "TransferManager.hpp"
#include "TimeUtils.hpp"
#include "UiAlerts.hpp"
#include "openscp/SftpClient.hpp"
#include <QAbstractButton>
#include <QApplication>
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

TransferManager::TransferManager(QObject *parent) : QObject(parent) {}

TransferManager::~TransferManager() {
    paused_ = true;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        resumeRequestedTasks_.clear();
        for (auto &t : tasks_) {
            canceledTasks_.insert(t.id);
            if (t.status == TransferTask::Status::Queued ||
                t.status == TransferTask::Status::Running ||
                t.status == TransferTask::Status::Paused) {
                t.status = TransferTask::Status::Canceled;
                t.currentSpeedKBps = 0.0;
                t.etaSeconds = -1;
                t.finishedAtMs = nowMs;
            }
        }
    }
    interruptActiveWorkers();
    std::unordered_map<quint64, std::thread> workersToJoin;
    {
        std::lock_guard<std::mutex> wl(workersMutex_);
        workersToJoin.swap(workers_);
    }
    for (auto &kv : workersToJoin) {
        if (kv.second.joinable())
            kv.second.join();
    }
    running_ = 0;
}

void TransferManager::setClient(openscp::SftpClient *c) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        client_ = c;
    }
    if (c) {
        // Re-enable queue execution after a disconnect/clearClient cycle.
        paused_ = false;
        // Reset stale running count from a previous session cleanup so new
        // queues can proceed deterministically after reconnect.
        running_.store(0);
        QMetaObject::invokeMethod(this, "schedule", Qt::QueuedConnection);
    }
}

void TransferManager::setSessionOptions(const openscp::SessionOptions &opt) {
    std::lock_guard<std::mutex> lk(mtx_);
    sessionOpt_ = opt;
}

void TransferManager::clearClient() {
    const qint64 startedAtMs = QDateTime::currentMSecsSinceEpoch();
    std::size_t activeWorkers = 0;
    {
        std::lock_guard<std::mutex> lk(activeWorkersMutex_);
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
        std::lock_guard<std::mutex> lk(mtx_);
        // Clear connection inputs up-front so a delayed clearClient() cannot
        // wipe a newer session set by a subsequent reconnect.
        client_ = nullptr;
        sessionOpt_.reset();
        resumeRequestedTasks_.clear();
        for (auto &t : tasks_) {
            canceledTasks_.insert(t.id);
            if (t.status == TransferTask::Status::Queued ||
                t.status == TransferTask::Status::Running ||
                t.status == TransferTask::Status::Paused) {
                t.status = TransferTask::Status::Canceled;
                t.currentSpeedKBps = 0.0;
                t.etaSeconds = -1;
                t.finishedAtMs = nowMs;
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
        std::lock_guard<std::mutex> wl(workersMutex_);
        workersToJoin.swap(workers_);
    }
    for (auto &kv : workersToJoin) {
        if (kv.second.joinable())
            kv.second.join();
    }
    qCInfo(ocXfer) << "clearClient finished"
                   << "elapsedMs="
                   << (QDateTime::currentMSecsSinceEpoch() - startedAtMs)
                   << "runningCounter=" << running_.load();
}

QVector<TransferTask> TransferManager::tasksSnapshot() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return tasks_;
}

std::unique_ptr<openscp::SftpClient>
TransferManager::createWorkerClient(quint64 taskId, std::string &err) {
    openscp::SftpClient *base = nullptr;
    std::optional<openscp::SessionOptions> opt;
    {
        std::lock_guard<std::mutex> lk(mtx_);
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
    for (int i = 0; i < 3; ++i) {
        {
            std::lock_guard<std::mutex> lk(mtx_);
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
            std::lock_guard<std::mutex> lk(connFactoryMutex_);
            conn = base->newConnectionLike(*opt, lastErr);
        }
        if (conn)
            return conn;
        if (i < 2) {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                if (paused_.load() || canceledTasks_.count(taskId) > 0 ||
                    pausedTasks_.count(taskId) > 0) {
                    err = "Transfer queue paused/canceled";
                    return nullptr;
                }
            }
            std::this_thread::sleep_for((1 << i) * 500ms);
        }
    }
    err = lastErr.empty() ? "Could not create transfer connection" : lastErr;
    return nullptr;
}

void TransferManager::enqueueUpload(const QString &local,
                                    const QString &remote) {
    TransferTask t{TransferTask::Type::Upload};
    t.id = nextId_++;
    t.src = local;
    t.dst = remote;
    {
        // Protect the structure
        // (other functions will access concurrently)
        // mtx_ protects tasks_
        std::lock_guard<std::mutex> lk(mtx_);
        tasks_.push_back(t);
    }
    emit tasksChanged();
    if (!paused_)
        schedule();
}

void TransferManager::enqueueDownload(const QString &remote,
                                      const QString &local) {
    TransferTask t{TransferTask::Type::Download};
    t.id = nextId_++;
    t.src = remote;
    t.dst = local;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        tasks_.push_back(t);
    }
    emit tasksChanged();
    if (!paused_)
        schedule();
}

void TransferManager::pauseAll() {
    paused_ = true;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto &t : tasks_) {
            if (t.status == TransferTask::Status::Running) {
                pausedTasks_.insert(t.id);
                t.status = TransferTask::Status::Paused;
                t.currentSpeedKBps = 0.0;
                t.etaSeconds = -1;
                t.finishedAtMs = 0;
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
        std::lock_guard<std::mutex> lk(activeWorkersMutex_);
        activeNow.reserve(activeWorkerClients_.size());
        for (const auto &kv : activeWorkerClients_)
            activeNow.insert(kv.first);
    }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto &t : tasks_) {
            if (t.status == TransferTask::Status::Paused) {
                if (activeNow.count(t.id) > 0) {
                    // Worker is still unwinding from pause; defer relaunch
                    // until it fully exits.
                    resumeRequestedTasks_.insert(t.id);
                } else {
                    t.status = TransferTask::Status::Queued;
                    t.resumeHint = true;
                    t.finishedAtMs = 0;
                    pausedTasks_.erase(t.id);
                    resumeRequestedTasks_.erase(t.id);
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
        std::lock_guard<std::mutex> lk(mtx_);
        resumeRequestedTasks_.clear();
        for (auto &t : tasks_) {
            canceledTasks_.insert(t.id);
            if (t.status == TransferTask::Status::Queued ||
                t.status == TransferTask::Status::Running ||
                t.status == TransferTask::Status::Paused) {
                ++affected;
                if (t.status == TransferTask::Status::Running)
                    ++runningNow;
                t.status = TransferTask::Status::Canceled;
                t.currentSpeedKBps = 0.0;
                t.etaSeconds = -1;
                t.finishedAtMs = nowMs;
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
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto &t : tasks_) {
            if (t.status == TransferTask::Status::Error ||
                t.status == TransferTask::Status::Canceled) {
                t.status = TransferTask::Status::Queued;
                t.attempts = 0;
                t.progress = 0;
                t.bytesDone = 0;
                t.bytesTotal = 0;
                t.currentSpeedKBps = 0.0;
                t.etaSeconds = -1;
                t.error.clear();
                t.finishedAtMs = 0;
                canceledTasks_.erase(t.id);
                pausedTasks_.erase(t.id);
            }
        }
    }
    emit tasksChanged();
    schedule();
}

void TransferManager::retryTask(quint64 id) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        int i = indexForId(id);
        if (i >= 0 && (tasks_[i].status == TransferTask::Status::Error ||
                       tasks_[i].status == TransferTask::Status::Canceled)) {
            auto &t = tasks_[i];
            t.status = TransferTask::Status::Queued;
            t.attempts = 0;
            t.progress = 0;
            t.bytesDone = 0;
            t.bytesTotal = 0;
            t.currentSpeedKBps = 0.0;
            t.etaSeconds = -1;
            t.error.clear();
            t.finishedAtMs = 0;
            canceledTasks_.erase(t.id);
            pausedTasks_.erase(t.id);
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
        std::lock_guard<std::mutex> lk(mtx_);
        QVector<TransferTask> next;
        next.reserve(tasks_.size());
        for (const auto &t : tasks_) {
            if (t.status != TransferTask::Status::Done)
                next.push_back(t);
        }
        tasks_.swap(next);
        std::unordered_set<quint64> remainingIds;
        remainingIds.reserve(tasks_.size());
        for (const auto &t : tasks_)
            remainingIds.insert(t.id);
        for (auto it = canceledTasks_.begin(); it != canceledTasks_.end();) {
            if (!remainingIds.count(*it))
                it = canceledTasks_.erase(it);
            else
                ++it;
        }
        for (auto it = pausedTasks_.begin(); it != pausedTasks_.end();) {
            if (!remainingIds.count(*it))
                it = pausedTasks_.erase(it);
            else
                ++it;
        }
    }
    emit tasksChanged();
}

void TransferManager::clearFailedCanceled() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        QVector<TransferTask> next;
        next.reserve(tasks_.size());
        for (const auto &t : tasks_) {
            if (t.status != TransferTask::Status::Error &&
                t.status != TransferTask::Status::Canceled)
                next.push_back(t);
        }
        tasks_.swap(next);
        std::unordered_set<quint64> remainingIds;
        remainingIds.reserve(tasks_.size());
        for (const auto &t : tasks_)
            remainingIds.insert(t.id);
        for (auto it = canceledTasks_.begin(); it != canceledTasks_.end();) {
            if (!remainingIds.count(*it))
                it = canceledTasks_.erase(it);
            else
                ++it;
        }
        for (auto it = pausedTasks_.begin(); it != pausedTasks_.end();) {
            if (!remainingIds.count(*it))
                it = pausedTasks_.erase(it);
            else
                ++it;
        }
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
        std::lock_guard<std::mutex> lk(mtx_);
        QVector<TransferTask> next;
        next.reserve(tasks_.size());
        for (const auto &t : tasks_) {
            const bool isDone = (t.status == TransferTask::Status::Done);
            const bool isFailed = (t.status == TransferTask::Status::Error ||
                                   t.status == TransferTask::Status::Canceled);
            const bool candidate =
                (clearDone && isDone) || (clearFailedCanceled && isFailed);
            const bool oldEnough =
                (t.finishedAtMs > 0 && t.finishedAtMs <= cutoff);
            if (candidate && oldEnough) {
                changed = true;
                continue;
            }
            next.push_back(t);
        }
        tasks_.swap(next);
        if (changed) {
            std::unordered_set<quint64> remainingIds;
            remainingIds.reserve(tasks_.size());
            for (const auto &t : tasks_)
                remainingIds.insert(t.id);
            for (auto it = canceledTasks_.begin();
                 it != canceledTasks_.end();) {
                if (!remainingIds.count(*it))
                    it = canceledTasks_.erase(it);
                else
                    ++it;
            }
            for (auto it = pausedTasks_.begin(); it != pausedTasks_.end();) {
                if (!remainingIds.count(*it))
                    it = pausedTasks_.erase(it);
                else
                    ++it;
            }
        }
    }
    if (changed)
        emit tasksChanged();
}

void TransferManager::processNext() {
    // Delegate to the concurrent scheduler
    schedule();
}

void TransferManager::schedule() {
    if (paused_)
        return;

    // Pre-resolve collisions on the manager (GUI) thread.
    auto askOverwrite = [&](const QString &name, const QString &srcInfo,
                            const QString &dstInfo) -> int {
        QMessageBox msg(nullptr);
        UiAlerts::configure(msg, Qt::ApplicationModal);
        msg.setWindowTitle(tr("Conflict"));
        // Clarify which side is local vs remote for better UX
        msg.setText(tr("«%1» already exists.\\nLocal: %2\\nRemote: %3")
                        .arg(name, srcInfo, dstInfo));
        QAbstractButton *btResume =
            msg.addButton(tr("Resume"), QMessageBox::ActionRole);
        QAbstractButton *btOverwrite =
            msg.addButton(tr("Overwrite"), QMessageBox::AcceptRole);
        QAbstractButton *btSkip =
            msg.addButton(tr("Skip"), QMessageBox::RejectRole);
        msg.exec();
        if (msg.clickedButton() == btResume)
            return 2;
        if (msg.clickedButton() == btOverwrite)
            return 1;
        return 0; // omitir
    };

    while (running_.load() < maxConcurrent_) {
        // Locate next queued task
        TransferTask t;
        int idx = -1;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!client_)
                return;
            for (int i = 0; i < tasks_.size(); ++i) {
                if (tasks_[i].status == TransferTask::Status::Queued) {
                    idx = i;
                    t = tasks_[i];
                    break;
                }
            }
            if (idx >= 0) {
                tasks_[idx].status = TransferTask::Status::Running;
                tasks_[idx].progress = 0;
                tasks_[idx].bytesDone = 0;
                tasks_[idx].bytesTotal = 0;
                tasks_[idx].currentSpeedKBps = 0.0;
                tasks_[idx].etaSeconds = -1;
                tasks_[idx].error.clear();
                tasks_[idx].finishedAtMs = 0;
            }
        }
        if (idx < 0)
            break;
        emit tasksChanged();

        bool resume = t.resumeHint;
        std::string preErr;
        auto precheckClient = createWorkerClient(t.id, preErr);
        if (!precheckClient) {
            const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
            {
                std::lock_guard<std::mutex> lk(mtx_);
                tasks_[idx].status = TransferTask::Status::Error;
                tasks_[idx].error = QString::fromStdString(preErr);
                tasks_[idx].currentSpeedKBps = 0.0;
                tasks_[idx].etaSeconds = -1;
                tasks_[idx].finishedAtMs = nowMs;
            }
            emit tasksChanged();
            continue;
        }

        if (t.type == TransferTask::Type::Upload) {
            bool isDir = false;
            std::string sErr;
            bool ex = precheckClient->exists(t.dst.toStdString(), isDir, sErr);
            if (!sErr.empty()) {
                const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    tasks_[idx].status = TransferTask::Status::Error;
                    tasks_[idx].error = QString::fromStdString(sErr);
                    tasks_[idx].currentSpeedKBps = 0.0;
                    tasks_[idx].etaSeconds = -1;
                    tasks_[idx].finishedAtMs = nowMs;
                }
                precheckClient->disconnect();
                emit tasksChanged();
                continue;
            }
            if (ex) {
                openscp::FileInfo rinfo{};
                std::string stErr;
                (void)precheckClient->stat(t.dst.toStdString(), rinfo, stErr);
                QString srcInfo = QString("%1 bytes, %2")
                                      .arg(QFileInfo(t.src).size())
                                      .arg(openscpui::localShortTime(
                                          QFileInfo(t.src).lastModified()));
                QString dstInfo =
                    QString("%1 bytes, %2")
                        .arg(rinfo.size)
                        .arg(rinfo.mtime ? openscpui::localShortTime(
                                               (quint64)rinfo.mtime)
                                         : QStringLiteral("?"));
                int choice =
                    askOverwrite(QFileInfo(t.src).fileName(), srcInfo, dstInfo);
                if (choice == 0) {
                    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                    {
                        std::lock_guard<std::mutex> lk(mtx_);
                        tasks_[idx].status = TransferTask::Status::Done;
                        tasks_[idx].currentSpeedKBps = 0.0;
                        tasks_[idx].etaSeconds = 0;
                        tasks_[idx].finishedAtMs = nowMs;
                    }
                    precheckClient->disconnect();
                    emit tasksChanged();
                    continue;
                }
                resume = (choice == 2);
            }

            auto ensureRemoteDir = [&](const QString &dir) -> bool {
                if (dir.isEmpty())
                    return true;
                QString cur = "/";
                const QStringList parts = dir.split('/', Qt::SkipEmptyParts);
                for (const QString &part : parts) {
                    QString next =
                        (cur == "/") ? ("/" + part) : (cur + "/" + part);
                    bool isD = false;
                    std::string e;
                    bool exs =
                        precheckClient->exists(next.toStdString(), isD, e);
                    if (!exs && e.empty()) {
                        std::string me;
                        if (!precheckClient->mkdir(next.toStdString(), me,
                                                   0755))
                            return false;
                    }
                    cur = next;
                }
                return true;
            };
            QString parentDir = QFileInfo(t.dst).path();
            if (!parentDir.isEmpty())
                (void)ensureRemoteDir(parentDir);
        } else {
            QFileInfo lfi(t.dst);
            if (lfi.exists()) {
                openscp::FileInfo rinfo{};
                std::string stErr;
                (void)precheckClient->stat(t.src.toStdString(), rinfo, stErr);
                QString srcInfo =
                    QString("%1 bytes, %2")
                        .arg(rinfo.size)
                        .arg(rinfo.mtime ? openscpui::localShortTime(
                                               (quint64)rinfo.mtime)
                                         : QStringLiteral("?"));
                QString dstInfo =
                    QString("%1 bytes, %2")
                        .arg(lfi.size())
                        .arg(openscpui::localShortTime(lfi.lastModified()));
                int choice = askOverwrite(lfi.fileName(), srcInfo, dstInfo);
                if (choice == 0) {
                    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                    {
                        std::lock_guard<std::mutex> lk(mtx_);
                        tasks_[idx].status = TransferTask::Status::Done;
                        tasks_[idx].currentSpeedKBps = 0.0;
                        tasks_[idx].etaSeconds = 0;
                        tasks_[idx].finishedAtMs = nowMs;
                    }
                    precheckClient->disconnect();
                    emit tasksChanged();
                    continue;
                }
                resume = (choice == 2);
            }
            QDir().mkpath(QFileInfo(t.dst).dir().absolutePath());
        }
        precheckClient->disconnect();

        // Launch worker to execute the transfer.
        running_.fetch_add(1);
        const quint64 taskId = t.id;
        const bool workerActiveNow = isWorkerActive(taskId);
        bool deferredRelaunch = false;
        {
            std::lock_guard<std::mutex> wl(workersMutex_);
            auto it = workers_.find(taskId);
            if (it != workers_.end() && it->second.joinable()) {
                if (workerActiveNow) {
                    // Do not block UI by joining an active worker; defer this
                    // relaunch until the existing worker fully exits.
                    {
                        std::lock_guard<std::mutex> lk(mtx_);
                        int i = indexForId(taskId);
                        if (i >= 0) {
                            tasks_[i].status = TransferTask::Status::Paused;
                            tasks_[i].currentSpeedKBps = 0.0;
                            tasks_[i].etaSeconds = -1;
                            tasks_[i].finishedAtMs = 0;
                            pausedTasks_.insert(taskId);
                            resumeRequestedTasks_.insert(taskId);
                        }
                    }
                    deferredRelaunch = true;
                } else {
                    qCInfo(ocXfer) << "schedule joining finished worker thread"
                                   << "taskId=" << taskId;
                    it->second.join();
                }
            }
            if (!deferredRelaunch) {
                workers_[taskId] =
                std::thread([this, t, taskId, resume]() mutable {
                    auto finalize = [this]() {
                        decrementRunningCounter();
                        QMetaObject::invokeMethod(this, "schedule",
                                                  Qt::QueuedConnection);
                    };

                    struct ActiveWorkerGuard {
                        TransferManager *self = nullptr;
                        quint64 id = 0;
                        ~ActiveWorkerGuard() {
                            if (!self)
                                return;
                            std::lock_guard<std::mutex> lk(
                                self->activeWorkersMutex_);
                            const std::size_t prevCount =
                                self->activeWorkerTaskIds_.size();
                            self->activeWorkerClients_.erase(id);
                            self->activeWorkerTaskIds_.erase(id);
                            self->pendingInterruptTasks_.erase(id);
                            qCInfo(ocXfer) << "worker active cleared"
                                           << "taskId=" << id
                                           << "prevActiveCount=" << prevCount
                                           << "activeCount="
                                           << self->activeWorkerTaskIds_.size();
                        }
                    };
                    std::string err;
                    {
                        std::lock_guard<std::mutex> lk(activeWorkersMutex_);
                        activeWorkerTaskIds_.insert(taskId);
                        qCInfo(ocXfer) << "worker active registered"
                                       << "taskId=" << taskId
                                       << "activeCount="
                                       << activeWorkerTaskIds_.size();
                    }
                    auto workerClientUnique = createWorkerClient(taskId, err);
                    if (!workerClientUnique) {
                        {
                            std::lock_guard<std::mutex> lk(activeWorkersMutex_);
                            activeWorkerTaskIds_.erase(taskId);
                            pendingInterruptTasks_.erase(taskId);
                        }
                        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                        {
                            std::lock_guard<std::mutex> lk(mtx_);
                            int i = indexForId(taskId);
                            if (i >= 0) {
                                const bool explicitlyCanceled =
                                    canceledTasks_.count(taskId) > 0;
                                const bool pausedTask =
                                    !explicitlyCanceled &&
                                    (pausedTasks_.count(taskId) > 0 ||
                                     paused_.load());
                                if (explicitlyCanceled || pausedTask) {
                                    tasks_[i].status =
                                        explicitlyCanceled
                                            ? TransferTask::Status::Canceled
                                            : TransferTask::Status::Paused;
                                    tasks_[i].error.clear();
                                    tasks_[i].currentSpeedKBps = 0.0;
                                    tasks_[i].etaSeconds = -1;
                                    tasks_[i].finishedAtMs =
                                        explicitlyCanceled ? nowMs : 0;
                                } else {
                                    tasks_[i].status = TransferTask::Status::Error;
                                    tasks_[i].error = QString::fromStdString(err);
                                    tasks_[i].currentSpeedKBps = 0.0;
                                    tasks_[i].etaSeconds = -1;
                                    tasks_[i].finishedAtMs = nowMs;
                                }
                            }
                        }
                        emit tasksChanged();
                        finalize();
                        return;
                    }
                    std::shared_ptr<openscp::SftpClient> workerClient =
                        std::move(workerClientUnique);
                    std::optional<ActiveWorkerGuard> activeGuard;
                    bool applyDeferredInterrupt = false;
                    {
                        std::lock_guard<std::mutex> lk(activeWorkersMutex_);
                        activeWorkerClients_[taskId] = workerClient;
                        applyDeferredInterrupt =
                            pendingInterruptTasks_.erase(taskId) > 0;
                    }
                    if (applyDeferredInterrupt) {
                        qCInfo(ocXfer) << "Applying deferred interrupt to worker"
                                       << "taskId=" << taskId;
                        workerClient->interrupt();
                    }
                    // Construct in-place to avoid a temporary guard whose
                    // destructor would clear active state immediately.
                    activeGuard.emplace();
                    activeGuard->self = this;
                    activeGuard->id = taskId;

                    // Mark attempt
                    {
                        std::lock_guard<std::mutex> lk(mtx_);
                        int i = indexForId(taskId);
                        if (i >= 0)
                            tasks_[i].attempts += 1;
                    }
                    emit tasksChanged();

                    auto isCanceled = [this, taskId]() -> bool {
                        std::lock_guard<std::mutex> lk(mtx_);
                        return canceledTasks_.count(taskId) > 0;
                    };
                    auto isPausedTask = [this, taskId]() -> bool {
                        std::lock_guard<std::mutex> lk(mtx_);
                        return pausedTasks_.count(taskId) > 0;
                    };
                    auto shouldCancel = [this, isCanceled, isPausedTask]() -> bool {
                        if (paused_.load())
                            return true;
                        if (isCanceled())
                            return true;
                        if (isPausedTask())
                            return true;
                        return false;
                    };

                    // Speed control (per task and global): simple bucket-based
                    // throttling
                    using clock = std::chrono::steady_clock;
                    static constexpr double KIB = 1024.0;
                    std::size_t lastDone = 0;
                    auto lastTick = clock::now();
                    auto progress = [this, taskId, lastTick, lastDone](
                                        std::size_t done,
                                        std::size_t total) mutable {
                        int pct = (total > 0) ? int((done * 100) / total) : 0;
                        const auto now = clock::now();
                        const double elapsedSec =
                            std::chrono::duration_cast<
                                std::chrono::duration<double>>(now - lastTick)
                                .count();
                        const double deltaBytes =
                            (done > lastDone) ? double(done - lastDone) : 0.0;
                        double measuredKBps = 0.0;
                        if (elapsedSec > 0.000001 && deltaBytes > 0.0) {
                            measuredKBps = (deltaBytes / KIB) / elapsedSec;
                        }
                        int etaSec = -1;
                        if (total > done && measuredKBps > 0.0) {
                            etaSec =
                                int((double(total - done) / KIB) / measuredKBps);
                        } else if (total > 0 && done >= total) {
                            etaSec = 0;
                        }
                        {
                            std::lock_guard<std::mutex> lk(mtx_);
                            int i = indexForId(taskId);
                            if (i >= 0) {
                                tasks_[i].progress = pct;
                                tasks_[i].bytesDone = done;
                                tasks_[i].bytesTotal = total;
                                if (measuredKBps > 0.0)
                                    tasks_[i].currentSpeedKBps = measuredKBps;
                                tasks_[i].etaSeconds = etaSec;
                            }
                        }
                        emit tasksChanged();

                        int taskLimit = 0; // KB/s (0 = unlimited)
                        int globalLimit = globalSpeedKBps_.load();
                        {
                            std::lock_guard<std::mutex> lk(mtx_);
                            int i = indexForId(taskId);
                            if (i >= 0)
                                taskLimit = tasks_[i].speedLimitKBps;
                        }
                        int effKBps = 0;
                        if (taskLimit > 0 && globalLimit > 0)
                            effKBps = std::min(taskLimit, globalLimit);
                        else
                            effKBps = (taskLimit > 0
                                           ? taskLimit
                                           : (globalLimit > 0 ? globalLimit : 0));
                        if (effKBps > 0 && done > lastDone) {
                            const auto now2 = clock::now();
                            const double deltaBytes2 = double(done - lastDone);
                            const double expectedSec = deltaBytes2 / (effKBps * KIB);
                            const double elapsedSec2 =
                                std::chrono::duration_cast<
                                    std::chrono::duration<double>>(now2 - lastTick)
                                    .count();
                            if (elapsedSec2 < expectedSec) {
                                const double sleepSec = expectedSec - elapsedSec2;
                                if (sleepSec > 0.0005) {
                                    std::this_thread::sleep_for(
                                        std::chrono::duration<double>(sleepSec));
                                }
                            }
                            lastTick = clock::now();
                            lastDone = done;
                        }
                    };

                    bool ok = false;
                    if (t.type == TransferTask::Type::Upload) {
                        std::string perr;
                        ok = workerClient->put(t.src.toStdString(),
                                               t.dst.toStdString(), perr, progress,
                                               shouldCancel, resume);
                        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                        if (!ok && shouldCancel()) {
                            std::lock_guard<std::mutex> lk(mtx_);
                            int i = indexForId(taskId);
                            if (i >= 0) {
                                // Avoid re-locking mtx_ via isCanceled() while
                                // already holding this mutex (self-deadlock).
                                const bool canceled =
                                    canceledTasks_.count(taskId) > 0;
                                tasks_[i].status = canceled
                                                       ? TransferTask::Status::Canceled
                                                       : TransferTask::Status::Paused;
                                tasks_[i].currentSpeedKBps = 0.0;
                                tasks_[i].etaSeconds = -1;
                                tasks_[i].finishedAtMs = canceled ? nowMs : 0;
                            }
                        } else if (!ok) {
                            std::lock_guard<std::mutex> lk(mtx_);
                            int i = indexForId(taskId);
                            if (i >= 0) {
                                tasks_[i].status = TransferTask::Status::Error;
                                tasks_[i].error = QString::fromStdString(perr);
                                tasks_[i].currentSpeedKBps = 0.0;
                                tasks_[i].etaSeconds = -1;
                                tasks_[i].finishedAtMs = nowMs;
                            }
                        } else {
                            std::lock_guard<std::mutex> lk(mtx_);
                            int i = indexForId(taskId);
                            if (i >= 0) {
                                tasks_[i].progress = 100;
                                if (tasks_[i].bytesTotal > 0)
                                    tasks_[i].bytesDone = tasks_[i].bytesTotal;
                                tasks_[i].status = TransferTask::Status::Done;
                                tasks_[i].currentSpeedKBps = 0.0;
                                tasks_[i].etaSeconds = 0;
                                tasks_[i].finishedAtMs = nowMs;
                            }
                        }
                    } else {
                        std::string gerr;
                        ok = workerClient->get(t.src.toStdString(),
                                               t.dst.toStdString(), gerr, progress,
                                               shouldCancel, resume);
                        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                        if (!ok && shouldCancel()) {
                            std::lock_guard<std::mutex> lk(mtx_);
                            int i = indexForId(taskId);
                            if (i >= 0) {
                                // Avoid re-locking mtx_ via isCanceled() while
                                // already holding this mutex (self-deadlock).
                                const bool canceled =
                                    canceledTasks_.count(taskId) > 0;
                                tasks_[i].status = canceled
                                                       ? TransferTask::Status::Canceled
                                                       : TransferTask::Status::Paused;
                                tasks_[i].currentSpeedKBps = 0.0;
                                tasks_[i].etaSeconds = -1;
                                tasks_[i].finishedAtMs = canceled ? nowMs : 0;
                            }
                        } else if (!ok) {
                            std::lock_guard<std::mutex> lk(mtx_);
                            int i = indexForId(taskId);
                            if (i >= 0) {
                                tasks_[i].status = TransferTask::Status::Error;
                                tasks_[i].error = QString::fromStdString(gerr);
                                tasks_[i].currentSpeedKBps = 0.0;
                                tasks_[i].etaSeconds = -1;
                                tasks_[i].finishedAtMs = nowMs;
                            }
                        } else {
                            openscp::FileInfo rinfo{};
                            std::string stErr;
                            (void)workerClient->stat(t.src.toStdString(), rinfo,
                                                     stErr);
                            if (rinfo.mtime > 0) {
                                QFile f(t.dst);
                                if (f.exists()) {
                                    const QDateTime tsUtc =
                                        QDateTime::fromSecsSinceEpoch(
                                            (qint64)rinfo.mtime,
                                            QTimeZone::utc());
                                    if (!f.setFileTime(
                                            tsUtc,
                                            QFileDevice::FileModificationTime)) {
                                        qWarning(ocXfer)
                                            << "Failed to set mtime for" << t.dst
                                            << "to" << tsUtc;
                                    }
                                }
                            }
                            std::lock_guard<std::mutex> lk(mtx_);
                            int i = indexForId(taskId);
                            if (i >= 0) {
                                tasks_[i].progress = 100;
                                if (tasks_[i].bytesTotal > 0)
                                    tasks_[i].bytesDone = tasks_[i].bytesTotal;
                                tasks_[i].status = TransferTask::Status::Done;
                                tasks_[i].currentSpeedKBps = 0.0;
                                tasks_[i].etaSeconds = 0;
                                tasks_[i].finishedAtMs = nowMs;
                            }
                        }
                    }

                    workerClient->disconnect();
                    {
                        std::lock_guard<std::mutex> lk(mtx_);
                        auto itResume = resumeRequestedTasks_.find(taskId);
                        if (itResume != resumeRequestedTasks_.end()) {
                            int i = indexForId(taskId);
                            if (i >= 0 &&
                                tasks_[i].status == TransferTask::Status::Paused) {
                                tasks_[i].status = TransferTask::Status::Queued;
                                tasks_[i].resumeHint = true;
                                tasks_[i].finishedAtMs = 0;
                                pausedTasks_.erase(taskId);
                            }
                            resumeRequestedTasks_.erase(itResume);
                            qCInfo(ocXfer)
                                << "Deferred resume armed after worker unwind"
                                << "taskId=" << taskId;
                        }
                    }
                    emit tasksChanged();
                    finalize();
                });
            }
        }
        if (deferredRelaunch) {
            emit tasksChanged();
            qCInfo(ocXfer) << "schedule deferred relaunch; worker still active"
                           << "taskId=" << taskId;
            decrementRunningCounter();
            continue;
        }
    }
}

int TransferManager::indexForId(quint64 id) const {
    for (int i = 0; i < tasks_.size(); ++i)
        if (tasks_[i].id == id)
            return i;
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

void TransferManager::interruptActiveWorker(quint64 id) {
    qCInfo(ocXfer) << "interruptActiveWorker requested" << "taskId=" << id;
    std::unique_lock<std::mutex> lk(activeWorkersMutex_, std::defer_lock);
    bool locked = false;
    for (int i = 0; i < 200; ++i) {
        if (lk.try_lock()) {
            locked = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!locked) {
        qWarning(ocXfer)
            << "interruptActiveWorker lock timeout; cooperative cancel only"
            << "taskId=" << id;
        return;
    }
    qCInfo(ocXfer) << "interruptActiveWorker lock acquired" << "taskId=" << id;
    std::shared_ptr<openscp::SftpClient> clientToInterrupt;
    bool deferred = false;
    const bool active = activeWorkerTaskIds_.count(id) > 0;
    qCInfo(ocXfer) << "interruptActiveWorker state"
                   << "taskId=" << id
                   << "active=" << active
                   << "activeCount=" << activeWorkerTaskIds_.size()
                   << "clientsCount=" << activeWorkerClients_.size();
    auto it = activeWorkerClients_.find(id);
    if (it != activeWorkerClients_.end())
        clientToInterrupt = it->second.lock();
    if (!active || !clientToInterrupt) {
        pendingInterruptTasks_.insert(id);
        deferred = true;
        qCInfo(ocXfer) << "interruptActiveWorker pending set"
                       << "taskId=" << id;
    }
    lk.unlock();
    if (clientToInterrupt) {
        qCInfo(ocXfer) << "interruptActiveWorker immediate" << "taskId=" << id;
        clientToInterrupt->interrupt();
    } else if (deferred) {
        qCInfo(ocXfer) << "interruptActiveWorker deferred" << "taskId=" << id;
    }
}

void TransferManager::interruptActiveWorkers() {
    qCInfo(ocXfer) << "interruptActiveWorkers requested";
    std::unique_lock<std::mutex> lk(activeWorkersMutex_, std::defer_lock);
    bool locked = false;
    for (int i = 0; i < 200; ++i) {
        if (lk.try_lock()) {
            locked = true;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    if (!locked) {
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
        auto it = activeWorkerClients_.find(taskId);
        if (it != activeWorkerClients_.end())
            client = it->second.lock();
        if (client) {
            immediate.emplace_back(taskId, std::move(client));
        } else {
            pendingInterruptTasks_.insert(taskId);
            deferred.push_back(taskId);
        }
    }
    lk.unlock();
    for (const auto &entry : immediate) {
        qCInfo(ocXfer) << "interruptActiveWorkers immediate taskId="
                       << entry.first;
        entry.second->interrupt();
    }
    for (quint64 taskId : deferred) {
        qCInfo(ocXfer) << "interruptActiveWorkers deferred taskId=" << taskId;
    }
}

bool TransferManager::isWorkerActive(quint64 id) {
    std::lock_guard<std::mutex> lk(activeWorkersMutex_);
    return activeWorkerTaskIds_.count(id) > 0;
}

void TransferManager::pauseTask(quint64 id) {
    bool changed = false;
    bool shouldInterrupt = false;
    TransferTask::Status previousStatus = TransferTask::Status::Queued;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        int i = indexForId(id);
        if (i >= 0) {
            previousStatus = tasks_[i].status;
            if (tasks_[i].status == TransferTask::Status::Queued ||
                tasks_[i].status == TransferTask::Status::Running) {
                resumeRequestedTasks_.erase(id);
                pausedTasks_.insert(id);
                tasks_[i].status = TransferTask::Status::Paused;
                tasks_[i].currentSpeedKBps = 0.0;
                tasks_[i].etaSeconds = -1;
                tasks_[i].finishedAtMs = 0;
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
                   << "taskId=" << id
                   << "prevStatus=" << transferStatusName(previousStatus);
    if (shouldInterrupt)
        interruptActiveWorker(id);
}

void TransferManager::resumeTask(quint64 id) {
    const bool active = isWorkerActive(id);
    bool changed = false;
    bool queueNow = false;
    bool deferred = false;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        int i = indexForId(id);
        if (i >= 0 && tasks_[i].status == TransferTask::Status::Paused) {
            if (active) {
                // Still finishing pause teardown; relaunch when worker exits.
                resumeRequestedTasks_.insert(id);
                deferred = true;
            } else {
                pausedTasks_.erase(id);
                tasks_[i].status = TransferTask::Status::Queued;
                tasks_[i].resumeHint = true;
                tasks_[i].finishedAtMs = 0;
                resumeRequestedTasks_.erase(id);
                changed = true;
                queueNow = true;
            }
        }
    }
    if (!changed && !deferred)
        return;
    if (deferred) {
        qCInfo(ocXfer) << "Resume deferred; worker still active"
                       << "taskId=" << id;
    } else {
        qCInfo(ocXfer) << "Resume queued immediately" << "taskId=" << id;
    }
    emit tasksChanged();
    if (queueNow)
        schedule();
}

void TransferManager::setTaskSpeedLimit(quint64 id, int kbps) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        int i = indexForId(id);
        if (i >= 0)
            tasks_[i].speedLimitKBps = kbps;
    }
    emit tasksChanged();
}

void TransferManager::cancelTask(quint64 id) {
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    const bool activeWorker = isWorkerActive(id);
    bool found = false;
    bool transitionedToCanceled = false;
    bool shouldInterrupt = false;
    TransferTask::Status previousStatus = TransferTask::Status::Queued;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        int i = indexForId(id);
        if (i >= 0) {
            found = true;
            previousStatus = tasks_[i].status;
            if (tasks_[i].status == TransferTask::Status::Queued ||
                tasks_[i].status == TransferTask::Status::Running ||
                tasks_[i].status == TransferTask::Status::Paused) {
                resumeRequestedTasks_.erase(id);
                canceledTasks_.insert(id);
                pausedTasks_.erase(id);
                tasks_[i].status = TransferTask::Status::Canceled;
                tasks_[i].currentSpeedKBps = 0.0;
                tasks_[i].etaSeconds = -1;
                tasks_[i].finishedAtMs = nowMs;
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
                   << "taskId=" << id
                   << "found=" << found
                   << "prevStatus=" << transferStatusName(previousStatus)
                   << "activeWorker=" << activeWorker
                   << "transitionedToCanceled=" << transitionedToCanceled;
    if (shouldInterrupt)
        interruptActiveWorker(id);
}
