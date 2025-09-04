// Implementación de la cola: gestiona una tarea a la vez con progreso y colisiones.
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
#include <QDateTime>
#include <QDir>
#include <chrono>
#include <thread>

TransferManager::TransferManager(QObject* parent) : QObject(parent) {}

TransferManager::~TransferManager() {
    paused_ = true;
    for (auto& kv : workers_) {
        if (kv.second.joinable()) kv.second.join();
    }
    workers_.clear();
}

void TransferManager::clearClient() {
    // Señalar pausa para que el worker coopere y termine
    paused_ = true;
    for (auto& kv : workers_) {
        if (kv.second.joinable()) kv.second.join();
    }
    workers_.clear();
    client_ = nullptr;
    running_ = 0;
}

void TransferManager::enqueueUpload(const QString& local, const QString& remote) {
    TransferTask t{ TransferTask::Type::Upload };
    t.id = nextId_++;
    t.src = local;
    t.dst = remote;
    {
        // proteger estructura
        // (otras funciones accederán concurrentemente)
        // mtx_ protege tasks_
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
    // Marca todas las tareas como detenidas y solicita cancelación cooperativa
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
    emit tasksChanged();
    schedule();
}

void TransferManager::clearCompleted() {
    std::lock_guard<std::mutex> lk(mtx_);
    QVector<TransferTask> next;
    next.reserve(tasks_.size());
    for (const auto& t : tasks_) {
        if (t.status != TransferTask::Status::Done) next.push_back(t);
    }
    tasks_.swap(next);
    emit tasksChanged();
}

void TransferManager::processNext() {
    // delega en el planificador concurrente
    schedule();
}

void TransferManager::schedule() {
    if (paused_ || !client_) return;

    // Resolución de colisiones previa y lanzamiento hasta maxConcurrent_
    auto askOverwrite = [&](const QString& name, const QString& srcInfo, const QString& dstInfo) -> int {
        QMessageBox msg(nullptr);
        msg.setWindowTitle("Conflicto");
        msg.setText(QString("«%1» ya existe.\nOrigen: %2\nDestino: %3").arg(name, srcInfo, dstInfo));
        QAbstractButton* btResume    = msg.addButton("Reanudar", QMessageBox::ActionRole);
        QAbstractButton* btOverwrite = msg.addButton("Sobrescribir", QMessageBox::AcceptRole);
        QAbstractButton* btSkip      = msg.addButton("Omitir", QMessageBox::RejectRole);
        msg.exec();
        if (msg.clickedButton() == btResume)    return 2;
        if (msg.clickedButton() == btOverwrite) return 1;
        return 0; // omitir
    };

    while (running_.load() < maxConcurrent_) {
        // localizar próxima tarea en cola
        TransferTask t;
        int idx = -1;
        {
            std::lock_guard<std::mutex> lk(mtx_);
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

        // Pre-resolución de colisiones
        if (t.type == TransferTask::Type::Upload) {
            // ¿existe remoto?
            bool isDir = false;
            std::string sErr;
            bool ex = false;
            {
                std::lock_guard<std::mutex> slk(sftpMutex_);
                ex = client_->exists(t.dst.toStdString(), isDir, sErr);
            }
            if (!sErr.empty()) {
                std::lock_guard<std::mutex> lk(mtx_);
                tasks_[idx].status = TransferTask::Status::Error;
                tasks_[idx].error = QString::fromStdString(sErr);
                emit tasksChanged();
                continue;
            }
            if (ex) {
                openscp::FileInfo rinfo{};
                std::string stErr;
                {
                    std::lock_guard<std::mutex> slk(sftpMutex_);
                    client_->stat(t.dst.toStdString(), rinfo, stErr);
                }
                QString srcInfo = QString("%1 bytes, %2")
                    .arg(QFileInfo(t.src).size())
                    .arg(QLocale().toString(QFileInfo(t.src).lastModified(), QLocale::ShortFormat));
                QString dstInfo = QString("%1 bytes, %2")
                    .arg(rinfo.size)
                    .arg(rinfo.mtime ? QLocale().toString(QDateTime::fromSecsSinceEpoch((qint64)rinfo.mtime), QLocale::ShortFormat) : "?");
                int choice = askOverwrite(QFileInfo(t.src).fileName(), srcInfo, dstInfo);
                if (choice == 0) {
                    std::lock_guard<std::mutex> lk(mtx_);
                    tasks_[idx].status = TransferTask::Status::Done;
                    emit tasksChanged();
                    continue;
                }
                resume = (choice == 2);
            }

            // asegurar directorios remotos padre
            auto ensureRemoteDir = [&](const QString& dir) -> bool {
                if (dir.isEmpty()) return true;
                QString cur = "/";
                const QStringList parts = dir.split('/', Qt::SkipEmptyParts);
                for (const QString& part : parts) {
                    QString next = (cur == "/") ? ("/" + part) : (cur + "/" + part);
                    bool isD = false;
                    std::string e;
                    bool exs = false;
                    {
                        std::lock_guard<std::mutex> slk(sftpMutex_);
                        exs = client_->exists(next.toStdString(), isD, e);
                        if (!exs && e.empty()) {
                            std::string me;
                            if (!client_->mkdir(next.toStdString(), me, 0755)) return false;
                        }
                    }
                    cur = next;
                }
                return true;
            };
            QString parentDir = QFileInfo(t.dst).path();
            if (!parentDir.isEmpty()) ensureRemoteDir(parentDir);
        } else {
            // Download: colisión local
            QFileInfo lfi(t.dst);
            if (lfi.exists()) {
                openscp::FileInfo rinfo{};
                std::string stErr;
                {
                    std::lock_guard<std::mutex> slk(sftpMutex_);
                    client_->stat(t.src.toStdString(), rinfo, stErr);
                }
                QString srcInfo = QString("%1 bytes, %2")
                    .arg(rinfo.size)
                    .arg(rinfo.mtime ? QLocale().toString(QDateTime::fromSecsSinceEpoch((qint64)rinfo.mtime), QLocale::ShortFormat) : "?");
                QString dstInfo = QString("%1 bytes, %2")
                    .arg(lfi.size())
                    .arg(QLocale().toString(lfi.lastModified(), QLocale::ShortFormat));
                int choice = askOverwrite(lfi.fileName(), srcInfo, dstInfo);
                if (choice == 0) {
                    std::lock_guard<std::mutex> lk(mtx_);
                    tasks_[idx].status = TransferTask::Status::Done;
                    emit tasksChanged();
                    continue;
                }
                resume = (choice == 2);
            }
            QDir().mkpath(QFileInfo(t.dst).dir().absolutePath());
        }

        // Lanzar worker para ejecutar la transferencia
        running_.fetch_add(1);
        const quint64 taskId = t.id;
        // Limpia cualquier worker previo con el mismo id (no debería ocurrir)
        if (workers_.count(taskId) && workers_[taskId].joinable()) {
            workers_[taskId].join();
        }
        workers_[taskId] = std::thread([this, t, taskId, resume]() mutable {
            // Intentar reconectar si hace falta
            std::string err;
            if (!ensureConnected(err)) {
                {
                    std::lock_guard<std::mutex> lk(mtx_);
                    int i = indexForId(taskId);
                    if (i >= 0) { tasks_[i].status = TransferTask::Status::Error; tasks_[i].error = QString::fromStdString(err); }
                }
                emit tasksChanged();
                running_.fetch_sub(1);
                // Replanificar en el hilo de la GUI
                QMetaObject::invokeMethod(this, "schedule", Qt::QueuedConnection);
                return;
            }

            // Marcar intento
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

            // Control de velocidad (por tarea y global): usar bucket simple
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

                // Throttling: dormir si excedemos el límite efectivo
                int taskLimit = 0; // KB/s (0 = sin límite)
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
                        if (sleepSec > 0.0005) { // evita sleeps ultra cortos
                            std::this_thread::sleep_for(std::chrono::duration<double>(sleepSec));
                        }
                    }
                    lastTick = clock::now();
                    lastDone = done;
                }
            };

            bool ok = false;
            if (t.type == TransferTask::Type::Upload) {
                // Subida local->remoto
                std::string perr;
                {
                    std::lock_guard<std::mutex> slk(sftpMutex_);
                    ok = client_->put(t.src.toStdString(), t.dst.toStdString(), perr, progress, shouldCancel, resume);
                }
                if (!ok && shouldCancel()) {
                    // Pausado o cancelado
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
                // Descarga remoto->local
                std::string gerr;
                {
                    std::lock_guard<std::mutex> slk(sftpMutex_);
                    ok = client_->get(t.src.toStdString(), t.dst.toStdString(), gerr, progress, shouldCancel, resume);
                }
                if (!ok && shouldCancel()) {
                    std::lock_guard<std::mutex> lk(mtx_);
                    int i = indexForId(taskId);
                    if (i >= 0) tasks_[i].status = isCanceled() ? TransferTask::Status::Canceled : TransferTask::Status::Paused;
                } else if (!ok) {
                    std::lock_guard<std::mutex> lk(mtx_);
                    int i = indexForId(taskId);
                    if (i >= 0) { tasks_[i].status = TransferTask::Status::Error; tasks_[i].error = QString::fromStdString(gerr); }
                } else {
                    std::lock_guard<std::mutex> lk(mtx_);
                    int i = indexForId(taskId);
                    if (i >= 0) { tasks_[i].progress = 100; tasks_[i].status = TransferTask::Status::Done; }
                }
            }

            emit tasksChanged();
            running_.fetch_sub(1);
            QMetaObject::invokeMethod(this, "schedule", Qt::QueuedConnection);
        });
    }
}

int TransferManager::indexForId(quint64 id) const {
    for (int i = 0; i < tasks_.size(); ++i)
        if (tasks_[i].id == id) return i;
    return -1;
}

bool TransferManager::ensureConnected(std::string& err) {
    if (!client_) {
        err = "No client";
        return false;
    }
    if (client_->isConnected()) return true;
    if (!sessionOpt_.has_value()) {
        err = "Sin opciones de sesión";
        return false;
    }
    // Intentar reconectar con backoff exponencial
    using namespace std::chrono_literals;
    for (int i = 0; i < 3; ++i) {
        std::this_thread::sleep_for((1 << i) * 500ms);
        if (client_->connect(*sessionOpt_, err)) return true;
    }
    return false;
}

void TransferManager::pauseTask(quint64 id) {
    std::lock_guard<std::mutex> lk(mtx_);
    pausedTasks_.insert(id);
    int i = indexForId(id);
    if (i >= 0) tasks_[i].status = TransferTask::Status::Paused;
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
    std::lock_guard<std::mutex> lk(mtx_);
    int i = indexForId(id);
    if (i >= 0) tasks_[i].speedLimitKBps = kbps;
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
