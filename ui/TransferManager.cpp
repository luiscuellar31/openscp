// Queue implementation: schedules concurrent worker transfers with isolated SFTP sessions.
#include "TransferManager.hpp"
#include "openscp/SftpClient.hpp"
#include <QApplication>
#include <QThread>
#include <QMetaObject>
#include <QMessageBox>
#include <QAbstractButton>
#include <QPushButton>
#include <QLocale>
#include <QFileInfo>
#include <QFile>
#include <QFileDevice>
#include <QLoggingCategory>
#include <QDateTime>
#include "TimeUtils.hpp"
#include <QTimeZone>
#include <QDir>
#include <chrono>
#include <thread>
Q_LOGGING_CATEGORY(ocXfer, "openscp.transfer")

TransferManager::TransferManager(QObject* parent) : QObject(parent) {}

TransferManager::~TransferManager() {
    paused_ = true;
    for (auto& kv : workers_) {
        if (kv.second.joinable()) kv.second.join();
    }
    workers_.clear();
}

void TransferManager::setClient(openscp::SftpClient* c) {
    std::lock_guard<std::mutex> lk(mtx_);
    client_ = c;
}

void TransferManager::setSessionOptions(const openscp::SessionOptions& opt) {
    std::lock_guard<std::mutex> lk(mtx_);
    sessionOpt_ = opt;
}

void TransferManager::clearClient() {
    // Signal pause so workers cooperate and finish
    paused_ = true;
    for (auto& kv : workers_) {
        if (kv.second.joinable()) kv.second.join();
    }
    workers_.clear();
    {
        std::lock_guard<std::mutex> lk(mtx_);
        client_ = nullptr;
        sessionOpt_.reset();
    }
    running_ = 0;
}

QVector<TransferTask> TransferManager::tasksSnapshot() const {
    std::lock_guard<std::mutex> lk(mtx_);
    return tasks_;
}

std::unique_ptr<openscp::SftpClient> TransferManager::createWorkerClient(std::string& err) {
    openscp::SftpClient* base = nullptr;
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
        err = "Sin opciones de sesión";
        return nullptr;
    }

    using namespace std::chrono_literals;
    std::string lastErr;
    for (int i = 0; i < 3; ++i) {
        std::unique_ptr<openscp::SftpClient> conn;
        {
            // Creating libssh2 clients/sessions from a single entry point avoids
            // cross-thread initialization hazards and keeps connection setup deterministic.
            std::lock_guard<std::mutex> lk(connFactoryMutex_);
            conn = base->newConnectionLike(*opt, lastErr);
        }
        if (conn) return conn;
        if (i < 2) std::this_thread::sleep_for((1 << i) * 500ms);
    }
    err = lastErr.empty() ? "No se pudo crear conexión de transferencia" : lastErr;
    return nullptr;
}

void TransferManager::enqueueUpload(const QString& local, const QString& remote) {
    TransferTask t{ TransferTask::Type::Upload };
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
    if (!paused_) schedule();
}

void TransferManager::enqueueDownload(const QString& remote, const QString& local) {
    TransferTask t{ TransferTask::Type::Download };
    t.id = nextId_++;
    t.src = remote;
    t.dst = local;
    {
        std::lock_guard<std::mutex> lk(mtx_);
        tasks_.push_back(t);
    }
    emit tasksChanged();
    if (!paused_) schedule();
}

void TransferManager::pauseAll() {
    paused_ = true;
    emit tasksChanged();
}

void TransferManager::resumeAll() {
    bool changed = false;
    if (paused_) { paused_ = false; changed = true; }
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& t : tasks_) {
            if (t.status == TransferTask::Status::Paused) {
                t.status = TransferTask::Status::Queued;
                t.resumeHint = true;
                pausedTasks_.erase(t.id);
                changed = true;
            }
        }
    }
    if (changed) emit tasksChanged();
    processNext();
}

void TransferManager::cancelAll() {
    // Mark all tasks as stopped and request cooperative cancellation
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& t : tasks_) {
            canceledTasks_.insert(t.id);
            if (t.status == TransferTask::Status::Queued || t.status == TransferTask::Status::Running || t.status == TransferTask::Status::Paused) {
                t.status = TransferTask::Status::Canceled;
            }
        }
    }
    emit tasksChanged();
}

void TransferManager::retryFailed() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        for (auto& t : tasks_) {
            if (t.status == TransferTask::Status::Error || t.status == TransferTask::Status::Canceled) {
                t.status = TransferTask::Status::Queued;
                t.attempts = 0;
                t.progress = 0;
                t.error.clear();
                canceledTasks_.erase(t.id);
            }
        }
    }
    emit tasksChanged();
    schedule();
}

void TransferManager::clearCompleted() {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        QVector<TransferTask> next;
        next.reserve(tasks_.size());
        for (const auto& t : tasks_) {
            if (t.status != TransferTask::Status::Done) next.push_back(t);
        }
        tasks_.swap(next);
    }
    emit tasksChanged();
}

void TransferManager::processNext() {
    // Delegate to the concurrent scheduler
    schedule();
}

void TransferManager::schedule() {
    if (paused_) return;

    // Pre-resolve collisions on the manager (GUI) thread.
    auto askOverwrite = [&](const QString& name, const QString& srcInfo, const QString& dstInfo) -> int {
        QMessageBox msg(nullptr);
        msg.setWindowTitle(tr("Conflicto"));
        // Clarify which side is local vs remote for better UX
        msg.setText(tr("«%1» ya existe.\nLocal: %2\nRemoto: %3").arg(name, srcInfo, dstInfo));
        QAbstractButton* btResume    = msg.addButton(tr("Reanudar"), QMessageBox::ActionRole);
        QAbstractButton* btOverwrite = msg.addButton(tr("Sobrescribir"), QMessageBox::AcceptRole);
        QAbstractButton* btSkip      = msg.addButton(tr("Omitir"), QMessageBox::RejectRole);
        msg.exec();
        if (msg.clickedButton() == btResume) return 2;
        if (msg.clickedButton() == btOverwrite) return 1;
        return 0; // omitir
    };

    while (running_.load() < maxConcurrent_) {
        // Locate next queued task
        TransferTask t;
        int idx = -1;
        {
            std::lock_guard<std::mutex> lk(mtx_);
            if (!client_) return;
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
                tasks_[idx].error.clear();
            }
        }
        if (idx < 0) break;
        emit tasksChanged();

        bool resume = t.resumeHint;
        std::string preErr;
        auto precheckClient = createWorkerClient(preErr);
        if (!precheckClient) {
            {
                std::lock_guard<std::mutex> lk(mtx_);
                tasks_[idx].status = TransferTask::Status::Error;
                tasks_[idx].error = QString::fromStdString(preErr);
            }
            emit tasksChanged();
            continue;
        }

        if (t.type == TransferTask::Type::Upload) {
            bool isDir = false;
            std::string sErr;
            bool ex = precheckClient->exists(t.dst.toStdString(), isDir, sErr);
            if (!sErr.empty()) {
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    tasks_[idx].status = TransferTask::Status::Error;
                    tasks_[idx].error = QString::fromStdString(sErr);
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
                    .arg(openscpui::localShortTime(QFileInfo(t.src).lastModified()));
                QString dstInfo = QString("%1 bytes, %2")
                    .arg(rinfo.size)
                    .arg(rinfo.mtime ? openscpui::localShortTime((quint64)rinfo.mtime) : QStringLiteral("?"));
                int choice = askOverwrite(QFileInfo(t.src).fileName(), srcInfo, dstInfo);
                if (choice == 0) {
                    {
                        std::lock_guard<std::mutex> lk(mtx_);
                        tasks_[idx].status = TransferTask::Status::Done;
                    }
                    precheckClient->disconnect();
                    emit tasksChanged();
                    continue;
                }
                resume = (choice == 2);
            }

            auto ensureRemoteDir = [&](const QString& dir) -> bool {
                if (dir.isEmpty()) return true;
                QString cur = "/";
                const QStringList parts = dir.split('/', Qt::SkipEmptyParts);
                for (const QString& part : parts) {
                    QString next = (cur == "/") ? ("/" + part) : (cur + "/" + part);
                    bool isD = false;
                    std::string e;
                    bool exs = precheckClient->exists(next.toStdString(), isD, e);
                    if (!exs && e.empty()) {
                        std::string me;
                        if (!precheckClient->mkdir(next.toStdString(), me, 0755)) return false;
                    }
                    cur = next;
                }
                return true;
            };
            QString parentDir = QFileInfo(t.dst).path();
            if (!parentDir.isEmpty()) (void)ensureRemoteDir(parentDir);
        } else {
            QFileInfo lfi(t.dst);
            if (lfi.exists()) {
                openscp::FileInfo rinfo{};
                std::string stErr;
                (void)precheckClient->stat(t.src.toStdString(), rinfo, stErr);
                QString srcInfo = QString("%1 bytes, %2")
                    .arg(rinfo.size)
                    .arg(rinfo.mtime ? openscpui::localShortTime((quint64)rinfo.mtime) : QStringLiteral("?"));
                QString dstInfo = QString("%1 bytes, %2")
                    .arg(lfi.size())
                    .arg(openscpui::localShortTime(lfi.lastModified()));
                int choice = askOverwrite(lfi.fileName(), srcInfo, dstInfo);
                if (choice == 0) {
                    {
                        std::lock_guard<std::mutex> lk(mtx_);
                        tasks_[idx].status = TransferTask::Status::Done;
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
        if (workers_.count(taskId) && workers_[taskId].joinable()) {
            workers_[taskId].join();
        }
        workers_[taskId] = std::thread([this, t, taskId, resume]() mutable {
            auto finalize = [this]() {
                running_.fetch_sub(1);
                QMetaObject::invokeMethod(this, "schedule", Qt::QueuedConnection);
            };

            std::string err;
            auto workerClient = createWorkerClient(err);
            if (!workerClient) {
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    int i = indexForId(taskId);
                    if (i >= 0) {
                        tasks_[i].status = TransferTask::Status::Error;
                        tasks_[i].error = QString::fromStdString(err);
                    }
                }
                emit tasksChanged();
                finalize();
                return;
            }

            // Mark attempt
            {
                std::lock_guard<std::mutex> lk(mtx_);
                int i = indexForId(taskId);
                if (i >= 0) tasks_[i].attempts += 1;
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
                if (paused_.load()) return true;
                if (isCanceled()) return true;
                if (isPausedTask()) return true;
                return false;
            };

            // Speed control (per task and global): simple bucket-based throttling
            using clock = std::chrono::steady_clock;
            static constexpr double KIB = 1024.0;
            std::size_t lastDone = 0;
            auto lastTick = clock::now();
            auto progress = [this, taskId, lastTick, lastDone](std::size_t done, std::size_t total) mutable {
                int pct = (total > 0) ? int((done * 100) / total) : 0;
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    int i = indexForId(taskId);
                    if (i >= 0) tasks_[i].progress = pct;
                }
                emit tasksChanged();

                int taskLimit = 0; // KB/s (0 = unlimited)
                int globalLimit = globalSpeedKBps_.load();
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    int i = indexForId(taskId);
                    if (i >= 0) taskLimit = tasks_[i].speedLimitKBps;
                }
                int effKBps = 0;
                if (taskLimit > 0 && globalLimit > 0) effKBps = std::min(taskLimit, globalLimit);
                else effKBps = (taskLimit > 0 ? taskLimit : (globalLimit > 0 ? globalLimit : 0));
                if (effKBps > 0 && done > lastDone) {
                    const auto now = clock::now();
                    const double deltaBytes = double(done - lastDone);
                    const double expectedSec = deltaBytes / (effKBps * KIB);
                    const double elapsedSec = std::chrono::duration_cast<std::chrono::duration<double>>(now - lastTick).count();
                    if (elapsedSec < expectedSec) {
                        const double sleepSec = expectedSec - elapsedSec;
                        if (sleepSec > 0.0005) {
                            std::this_thread::sleep_for(std::chrono::duration<double>(sleepSec));
                        }
                    }
                    lastTick = clock::now();
                    lastDone = done;
                }
            };

            bool ok = false;
            if (t.type == TransferTask::Type::Upload) {
                std::string perr;
                ok = workerClient->put(t.src.toStdString(), t.dst.toStdString(), perr, progress, shouldCancel, resume);
                if (!ok && shouldCancel()) {
                    std::lock_guard<std::mutex> lk(mtx_);
                    int i = indexForId(taskId);
                    if (i >= 0) tasks_[i].status = isCanceled() ? TransferTask::Status::Canceled : TransferTask::Status::Paused;
                } else if (!ok) {
                    std::lock_guard<std::mutex> lk(mtx_);
                    int i = indexForId(taskId);
                    if (i >= 0) { tasks_[i].status = TransferTask::Status::Error; tasks_[i].error = QString::fromStdString(perr); }
                } else {
                    std::lock_guard<std::mutex> lk(mtx_);
                    int i = indexForId(taskId);
                    if (i >= 0) { tasks_[i].progress = 100; tasks_[i].status = TransferTask::Status::Done; }
                }
            } else {
                std::string gerr;
                ok = workerClient->get(t.src.toStdString(), t.dst.toStdString(), gerr, progress, shouldCancel, resume);
                if (!ok && shouldCancel()) {
                    std::lock_guard<std::mutex> lk(mtx_);
                    int i = indexForId(taskId);
                    if (i >= 0) tasks_[i].status = isCanceled() ? TransferTask::Status::Canceled : TransferTask::Status::Paused;
                } else if (!ok) {
                    std::lock_guard<std::mutex> lk(mtx_);
                    int i = indexForId(taskId);
                    if (i >= 0) { tasks_[i].status = TransferTask::Status::Error; tasks_[i].error = QString::fromStdString(gerr); }
                } else {
                    openscp::FileInfo rinfo{};
                    std::string stErr;
                    (void)workerClient->stat(t.src.toStdString(), rinfo, stErr);
                    if (rinfo.mtime > 0) {
                        QFile f(t.dst);
                        if (f.exists()) {
                            const QDateTime tsUtc = QDateTime::fromSecsSinceEpoch((qint64)rinfo.mtime, QTimeZone::utc());
                            if (!f.setFileTime(tsUtc, QFileDevice::FileModificationTime)) {
                                qWarning(ocXfer) << "Failed to set mtime for" << t.dst << "to" << tsUtc;
                            }
                        }
                    }
                    std::lock_guard<std::mutex> lk(mtx_);
                    int i = indexForId(taskId);
                    if (i >= 0) { tasks_[i].progress = 100; tasks_[i].status = TransferTask::Status::Done; }
                }
            }

            workerClient->disconnect();
            emit tasksChanged();
            finalize();
        });
    }
}

int TransferManager::indexForId(quint64 id) const {
    for (int i = 0; i < tasks_.size(); ++i)
        if (tasks_[i].id == id) return i;
    return -1;
}

void TransferManager::pauseTask(quint64 id) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        pausedTasks_.insert(id);
        int i = indexForId(id);
        if (i >= 0) tasks_[i].status = TransferTask::Status::Paused;
    }
    emit tasksChanged();
}

void TransferManager::resumeTask(quint64 id) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        pausedTasks_.erase(id);
        int i = indexForId(id);
        if (i >= 0 && tasks_[i].status == TransferTask::Status::Paused) {
            tasks_[i].status = TransferTask::Status::Queued;
            tasks_[i].resumeHint = true;
        }
    }
    emit tasksChanged();
    schedule();
}

void TransferManager::setTaskSpeedLimit(quint64 id, int kbps) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        int i = indexForId(id);
        if (i >= 0) tasks_[i].speedLimitKBps = kbps;
    }
    emit tasksChanged();
}

void TransferManager::cancelTask(quint64 id) {
    {
        std::lock_guard<std::mutex> lk(mtx_);
        canceledTasks_.insert(id);
        int i = indexForId(id);
        if (i >= 0) {
            if (tasks_[i].status == TransferTask::Status::Queued || tasks_[i].status == TransferTask::Status::Running || tasks_[i].status == TransferTask::Status::Paused) {
                tasks_[i].status = TransferTask::Status::Canceled;
            }
        }
    }
    emit tasksChanged();
}
