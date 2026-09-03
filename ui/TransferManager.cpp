// Persistent transfer queue with a fixed worker pool and per-slot connections.
#include "TransferManager.hpp"

#include "TimeUtils.hpp"
#include "UiAlerts.hpp"
#include "openscp/RemoteClient.hpp"
#include "openscp/RuntimeLogging.hpp"

#include <QAbstractButton>
#include <QCheckBox>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QLoggingCategory>
#include <QMessageBox>
#include <QMetaObject>
#include <QPointer>
#include <QPushButton>
#include <QRandomGenerator>
#include <QRegularExpression>
#include <QSet>
#include <QStandardPaths>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <cmath>
#include <functional>
#include <limits>
#include <utility>

Q_LOGGING_CATEGORY(ocXfer, "openscp.transfer")

namespace {

using Status = TransferTask::Status;
using Policy = TransferConflictPolicy;
using Clock = std::chrono::steady_clock;

bool canRetry(Status status) {
    return status == Status::Error || status == Status::Canceled ||
           status == Status::Warning;
}

const char *statusName(Status status) {
    switch (status) {
    case Status::Queued:
        return "Queued";
    case Status::Running:
        return "Running";
    case Status::Paused:
        return "Paused";
    case Status::Done:
        return "Done";
    case Status::Error:
        return "Error";
    case Status::Canceled:
        return "Canceled";
    case Status::WaitingForConnection:
        return "WaitingForConnection";
    case Status::RetryWaiting:
        return "RetryWaiting";
    case Status::Skipped:
        return "Skipped";
    case Status::Warning:
        return "Warning";
    }
    return "Unknown";
}

QString errorForUi(const std::string &rawError) {
    QString message = QString::fromStdString(rawError).trimmed();
    if (message.isEmpty())
        return message;

    const QString lower = message.toLower();
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
               "\n" + message;
    }
    return message;
}

QString numberedPath(const QString &path, int suffix) {
    const QFileInfo info(path);
    const QString directory = info.path();
    QString base = info.completeBaseName();
    QString extension = info.completeSuffix();
    if (base.isEmpty())
        base = info.fileName();
    QString name = QStringLiteral("%1 (%2)").arg(base).arg(suffix);
    if (!extension.isEmpty())
        name += QStringLiteral(".") + extension;
    if (directory.isEmpty() || directory == QStringLiteral("."))
        return name;
    return QDir(directory).filePath(name);
}

QString numberedRemotePath(const QString &path, int suffix) {
    const qsizetype slash = path.lastIndexOf('/');
    const QString directory = slash >= 0 ? path.left(slash + 1) : QString();
    const QString filename = slash >= 0 ? path.mid(slash + 1) : path;
    const qsizetype dot = filename.lastIndexOf('.');
    const bool hasExtension = dot > 0;
    const QString base = hasExtension ? filename.left(dot) : filename;
    const QString extension = hasExtension ? filename.mid(dot) : QString();
    return directory +
           QStringLiteral("%1 (%2)%3").arg(base).arg(suffix).arg(extension);
}

} // namespace

struct TransferManager::WorkerSlot {
    explicit WorkerSlot(std::size_t slotIndex) : index(slotIndex) {}

    std::size_t index = 0;
    std::jthread thread;
    std::mutex clientMutex;
    std::shared_ptr<openscp::RemoteClient> client;
    quint64 clientGeneration = 0;
    std::atomic<quint64> activeTaskId{0};
};

TransferManager::TransferManager(QObject *parent) : QObject(parent) {
    qRegisterMetaType<QVector<quint64>>("QVector<quint64>");

    persistenceTimer_ = new QTimer(this);
    persistenceTimer_->setSingleShot(true);
    persistenceTimer_->setInterval(250);
    connect(persistenceTimer_, &QTimer::timeout, this,
            &TransferManager::persistNow);
    workerSlots_.reserve(kWorkerSlots);
    for (int index = 0; index < kWorkerSlots; ++index)
        workerSlots_.push_back(std::make_unique<WorkerSlot>(index));
    for (auto &slot : workerSlots_) {
        WorkerSlot *rawSlot = slot.get();
        rawSlot->thread =
            std::jthread([this, rawSlot](std::stop_token stopToken) {
                workerLoop(rawSlot->index, stopToken);
            });
    }
}

TransferManager::~TransferManager() {
    shuttingDown_.store(true);
    paused_.store(true);

    // Persist a recoverable paused representation before stopping workers.
    persistNow();
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (quint64 taskId : activeTaskIds_)
            pausedTasks_.insert(taskId);
    }
    interruptAllActive();
    for (auto &slot : workerSlots_)
        slot->thread.request_stop();
    workCv_.notify_all();
    retryCv_.notify_all();
    bandwidthLimiter_.wakeAll();

    // Destroying jthread joins it. Do this while every manager member used by a
    // worker is still alive.
    for (auto &slot : workerSlots_) {
        if (slot->thread.joinable())
            slot->thread.join();
        std::shared_ptr<openscp::RemoteClient> cached;
        {
            std::lock_guard<std::mutex> lock(slot->clientMutex);
            cached = std::move(slot->client);
            slot->clientGeneration = 0;
        }
        if (cached)
            cached->disconnect();
    }
    workerSlots_.clear();
}

// Session and queue configuration

void TransferManager::setSessionOptions(const openscp::SessionOptions &opt) {
    {
        std::lock_guard<std::mutex> factoryLock(connFactoryMutex_);
        std::lock_guard<std::mutex> lock(mtx_);
        sessionOpt_ = opt;
        ++sessionGeneration_;
    }
    workCv_.notify_all();
}

void TransferManager::setSessionIdentity(const QString &sessionKey) {
    QVector<quint64> changed;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (currentSessionKey_ == sessionKey)
            return;
        currentSessionKey_ = sessionKey;
        ++sessionGeneration_;
        for (auto &taskNode : queueStore_.nodes()) {
            auto &task = *taskNode;
            if (isTerminalTransferStatus(task.status) ||
                task.sessionKey.isEmpty())
                continue;
            if (task.sessionKey != currentSessionKey_) {
                if (task.status == Status::Queued ||
                    task.status == Status::Paused) {
                    task.status = Status::WaitingForConnection;
                    changed.push_back(task.taskId);
                }
            } else if (task.status == Status::WaitingForConnection) {
                // Restored work never starts merely because a site connected.
                task.status = task.restored ? Status::Paused : Status::Queued;
                changed.push_back(task.taskId);
            }
        }
    }
    if (!changed.isEmpty())
        publishUpdated(changed);
    workCv_.notify_all();
}

QString TransferManager::sessionIdentity() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return currentSessionKey_;
}

void TransferManager::setClient(openscp::RemoteClient *client) {
    {
        std::lock_guard<std::mutex> factoryLock(connFactoryMutex_);
        std::lock_guard<std::mutex> lock(mtx_);
        client_ = client;
        ++sessionGeneration_;
    }
    workCv_.notify_all();
}

void TransferManager::clearClient() {
    QVector<quint64> changed;
    {
        std::lock_guard<std::mutex> factoryLock(connFactoryMutex_);
        std::lock_guard<std::mutex> lock(mtx_);
        client_ = nullptr;
        sessionOpt_.reset();
        ++sessionGeneration_;
        for (auto &taskNode : queueStore_.nodes()) {
            auto &task = *taskNode;
            if (task.status == Status::Running ||
                task.status == Status::RetryWaiting ||
                task.status == Status::Queued) {
                if (activeTaskIds_.count(task.taskId))
                    pausedTasks_.insert(task.taskId);
                task.status = Status::WaitingForConnection;
                task.currentSpeedKBps = 0;
                task.etaSeconds = -1;
                changed.push_back(task.taskId);
            }
        }
    }
    if (!changed.isEmpty())
        publishUpdated(changed);
    interruptAllActive();
    workCv_.notify_all();
    retryCv_.notify_all();
    bandwidthLimiter_.wakeAll();

    {
        std::unique_lock<std::mutex> lock(mtx_);
        idleCv_.wait(lock, [this] {
            return activeTaskIds_.empty() || shuttingDown_.load();
        });
    }

    for (auto &slot : workerSlots_)
        invalidateWorkerClient(*slot);
}

void TransferManager::setMaxConcurrent(int maxConcurrent) {
    const int bounded = std::clamp(maxConcurrent, 1, kWorkerSlots);
    if (maxConcurrent_.exchange(bounded) == bounded)
        return;
    emit queueSettingsChanged();
    schedulePersistence();
    workCv_.notify_all();
}

void TransferManager::setGlobalSpeedLimitKBps(int kbps) {
    const int bounded = std::max(0, kbps);
    if (bandwidthLimiter_.limitKBps() == bounded)
        return;
    bandwidthLimiter_.setLimitKBps(bounded);
    emit queueSettingsChanged();
    schedulePersistence();
}

TransferTask *TransferManager::taskForIdLocked(quint64 taskId) {
    return queueStore_.find(taskId);
}

const TransferTask *TransferManager::taskForIdLocked(quint64 taskId) const {
    return queueStore_.find(taskId);
}

void TransferManager::appendTaskLocked(TransferTask task) {
    if (dependencyFailedLocked(task)) {
        skipForFailedDependencyLocked(task,
                                      QDateTime::currentMSecsSinceEpoch());
    }
    queueStore_.append(std::move(task));
}

quint64
TransferManager::enqueuePreparedTask(TransferTask task,
                                     const TransferBatchOptions &options,
                                     bool inheritBatchConflictPolicy) {
    quint64 taskId = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        task.taskId = nextId_++;
        task.batchId = normalizedBatchIdLocked(options.batchId);
        task.dependsOnTaskId = options.dependsOnTaskId;
        task.sessionKey = options.sessionKey.isEmpty() ? currentSessionKey_
                                                       : options.sessionKey;
        initializeConnectionStatusLocked(task);
        task.queuedAtMs = QDateTime::currentMSecsSinceEpoch();
        conflictCoordinator_.ensureBatchPolicy(task.batchId,
                                               options.conflictPolicy);
        if (inheritBatchConflictPolicy) {
            task.conflictPolicy = conflictCoordinator_.batchPolicy(
                task.batchId, options.conflictPolicy);
        }
        taskId = task.taskId;
        appendTaskLocked(std::move(task));
    }
    publishAdded({taskId});
    schedule();
    return taskId;
}

void TransferManager::rebuildTaskLookupLocked() {
    queueStore_.rebuildIndex();
    terminalTaskCount_ = 0;
    for (const auto &taskNode : queueStore_.nodes()) {
        const TransferTask &task = *taskNode;
        if (isTerminalTransferStatus(task.status))
            ++terminalTaskCount_;
    }
    scheduler_.normalizeForSize(queueStore_.nodes().size());
}

void TransferManager::forgetBatchPolicyIfUnusedLocked(quint64 batchId) {
    const bool stillUsed =
        std::any_of(queueStore_.nodes().cbegin(), queueStore_.nodes().cend(),
                    [batchId](const auto &taskNode) {
                        return taskNode->batchId == batchId;
                    });
    if (!stillUsed)
        conflictCoordinator_.forgetBatch(batchId);
}

quint64 TransferManager::normalizedBatchIdLocked(quint64 requested) {
    if (requested != 0) {
        nextBatchId_ = std::max(nextBatchId_, requested + 1);
        return requested;
    }
    return nextBatchId_++;
}

void TransferManager::initializeConnectionStatusLocked(
    TransferTask &task) const {
    if (!task.sessionKey.isEmpty() && task.sessionKey != currentSessionKey_) {
        task.status = Status::WaitingForConnection;
    }
}

// Task creation and batch policy

std::string TransferManager::destinationKey(const TransferTask &task) const {
    if (task.type == TransferTask::Type::Upload ||
        task.type == TransferTask::Type::CreateRemoteDirectory ||
        task.type == TransferTask::Type::DeleteRemoteFile ||
        task.type == TransferTask::Type::DeleteRemoteDirectory) {
        return ("remote:" + QDir::cleanPath(task.dst)).toStdString();
    }
    const QString local =
        QDir::cleanPath(QFileInfo(task.dst).absoluteFilePath());
    return ("local:" + local).toStdString();
}

bool TransferManager::reserveDestinationLocked(const TransferTask &task) {
    const std::string key = destinationKey(task);
    if (reservedDestinations_.count(key))
        return false;
    reservedDestinations_.insert(key);
    reservationByTask_[task.taskId] = key;
    return true;
}

void TransferManager::releaseDestinationLocked(quint64 taskId) {
    const auto found = reservationByTask_.find(taskId);
    if (found == reservationByTask_.end())
        return;
    reservedDestinations_.erase(found->second);
    reservationByTask_.erase(found);
}

bool TransferManager::dependencySatisfiedLocked(
    const TransferTask &task) const {
    if (task.dependsOnTaskId == 0)
        return true;
    const TransferTask *dependency = taskForIdLocked(task.dependsOnTaskId);
    if (!dependency) {
        // Terminal prerequisites are intentionally pruned and are not written
        // to the non-terminal persistence file.
        return true;
    }
    const Status status = dependency->status;
    return status == Status::Done || (status == Status::Skipped &&
                                      !dependency->skippedByFailedDependency);
}

bool TransferManager::dependencyFailedLocked(const TransferTask &task) const {
    if (task.dependsOnTaskId == 0)
        return false;
    const TransferTask *dependency = taskForIdLocked(task.dependsOnTaskId);
    if (!dependency)
        return false;
    const Status status = dependency->status;
    return status == Status::Error || status == Status::Canceled ||
           status == Status::Warning || dependency->skippedByFailedDependency;
}

void TransferManager::skipForFailedDependencyLocked(TransferTask &task,
                                                    qint64 now) {
    if (isTerminalTransferStatus(task.status))
        return;
    pausedTasks_.erase(task.taskId);
    canceledTasks_.erase(task.taskId);
    resumeRequestedTasks_.erase(task.taskId);
    task.status = Status::Skipped;
    task.phase = TransferPhase::Finished;
    task.error = QCoreApplication::translate(
        "TransferManager",
        "Skipped because a prerequisite task did not complete successfully.");
    task.currentSpeedKBps = 0;
    task.etaSeconds = -1;
    task.finishedAtMs = now;
    task.skippedByFailedDependency = true;
    ++terminalTaskCount_;
}

quint64 TransferManager::enqueueUpload(const QString &local,
                                       const QString &remote,
                                       const TransferBatchOptions &options) {
    TransferTask task{};
    task.type = TransferTask::Type::Upload;
    task.src = local;
    task.dst = remote;
    task.operation = options.operation;
    task.conflictPolicy = options.conflictPolicy;
    task.postAction = options.operation == TransferOperation::Move
                          ? TransferPostAction::DeleteSource
                          : TransferPostAction::None;
    return enqueuePreparedTask(std::move(task), options, true);
}

quint64 TransferManager::enqueueDownload(const QString &remote,
                                         const QString &local,
                                         const TransferBatchOptions &options) {
    TransferTask task{};
    task.type = TransferTask::Type::Download;
    task.src = remote;
    task.dst = local;
    task.operation = options.operation;
    task.conflictPolicy = options.conflictPolicy;
    task.postAction = options.operation == TransferOperation::Move
                          ? TransferPostAction::DeleteSource
                          : TransferPostAction::None;
    return enqueuePreparedTask(std::move(task), options, true);
}

quint64
TransferManager::enqueueLocalDirectory(const QString &localDirectory,
                                       const TransferBatchOptions &options) {
    TransferTask task{};
    task.type = TransferTask::Type::CreateLocalDirectory;
    task.dst = localDirectory;
    task.operation = TransferOperation::Copy;
    task.conflictPolicy = Policy::Skip;
    return enqueuePreparedTask(std::move(task), options, false);
}

quint64
TransferManager::enqueueRemoteDirectory(const QString &remoteDirectory,
                                        const TransferBatchOptions &options) {
    TransferTask task{};
    task.type = TransferTask::Type::CreateRemoteDirectory;
    task.dst = remoteDirectory;
    task.operation = TransferOperation::Copy;
    task.conflictPolicy = Policy::Skip;
    return enqueuePreparedTask(std::move(task), options, false);
}

quint64 TransferManager::enqueuePathTask(TransferTask::Type type,
                                         const QString &path,
                                         const TransferBatchOptions &options) {
    TransferTask task{};
    task.type = type;
    task.src = path;
    task.dst = path;
    task.operation = TransferOperation::Copy;
    task.conflictPolicy = Policy::Skip;
    return enqueuePreparedTask(std::move(task), options, false);
}

quint64
TransferManager::enqueueLocalDelete(const QString &localPath, bool directory,
                                    const TransferBatchOptions &options) {
    return enqueuePathTask(directory ? TransferTask::Type::DeleteLocalDirectory
                                     : TransferTask::Type::DeleteLocalFile,
                           localPath, options);
}

quint64
TransferManager::enqueueRemoteDelete(const QString &remotePath, bool directory,
                                     const TransferBatchOptions &options) {
    return enqueuePathTask(directory ? TransferTask::Type::DeleteRemoteDirectory
                                     : TransferTask::Type::DeleteRemoteFile,
                           remotePath, options);
}

quint64 TransferManager::createBatch(const TransferBatchOptions &options) {
    std::lock_guard<std::mutex> lock(mtx_);
    const quint64 batchId = normalizedBatchIdLocked(options.batchId);
    conflictCoordinator_.setBatchPolicy(batchId, options.conflictPolicy);
    return batchId;
}

void TransferManager::cancelBatch(quint64 batchId) {
    QVector<quint64> changed;
    QVector<quint64> active;
    QVector<quint64> removed;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (auto &taskNode : queueStore_.nodes()) {
            auto &task = *taskNode;
            if (task.batchId != batchId ||
                isTerminalTransferStatus(task.status))
                continue;
            canceledTasks_.insert(task.taskId);
            pausedTasks_.erase(task.taskId);
            resumeRequestedTasks_.erase(task.taskId);
            if (activeTaskIds_.count(task.taskId))
                active.push_back(task.taskId);
            transitionToCanceled(task, now);
            changed.push_back(task.taskId);
        }
        removed = pruneTerminalHistoryLocked();
    }
    if (!changed.isEmpty())
        publishUpdated(changed);
    if (!removed.isEmpty())
        publishRemoved(removed);
    for (quint64 taskId : active)
        interruptTask(taskId);
}

int TransferManager::enqueueDownloads(
    const QVector<QPair<QString, QString>> &remoteLocalPairs,
    const TransferBatchOptions &options) {
    if (remoteLocalPairs.isEmpty())
        return 0;

    QVector<quint64> ids;
    ids.reserve(remoteLocalPairs.size());
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const quint64 batchId = normalizedBatchIdLocked(options.batchId);
        const QString sessionKey = options.sessionKey.isEmpty()
                                       ? currentSessionKey_
                                       : options.sessionKey;
        const qint64 queuedAt = QDateTime::currentMSecsSinceEpoch();
        conflictCoordinator_.ensureBatchPolicy(batchId, options.conflictPolicy);
        const Policy batchPolicy =
            conflictCoordinator_.batchPolicy(batchId, options.conflictPolicy);
        queueStore_.nodes().reserve(
            queueStore_.nodes().size() +
            static_cast<std::size_t>(remoteLocalPairs.size()));
        for (const auto &pair : remoteLocalPairs) {
            TransferTask task{};
            task.type = TransferTask::Type::Download;
            task.taskId = nextId_++;
            task.batchId = batchId;
            task.dependsOnTaskId = options.dependsOnTaskId;
            task.sessionKey = sessionKey;
            initializeConnectionStatusLocked(task);
            task.src = pair.first;
            task.dst = pair.second;
            task.operation = options.operation;
            task.conflictPolicy = batchPolicy;
            task.postAction = options.operation == TransferOperation::Move
                                  ? TransferPostAction::DeleteSource
                                  : TransferPostAction::None;
            task.queuedAtMs = queuedAt;
            ids.push_back(task.taskId);
            appendTaskLocked(std::move(task));
        }
    }
    publishAdded(ids);
    schedule();
    return static_cast<int>(ids.size());
}

QVector<TransferTask> TransferManager::tasksSnapshot() const {
    QVector<TransferTask> result;
    std::lock_guard<std::mutex> lock(mtx_);
    result.reserve(static_cast<qsizetype>(queueStore_.nodes().size()));
    for (const auto &taskNode : queueStore_.nodes())
        result.push_back(*taskNode);
    return result;
}

QVector<TransferTask>
TransferManager::tasksSnapshot(const QVector<quint64> &taskIds) const {
    QVector<TransferTask> result;
    result.reserve(taskIds.size());
    std::lock_guard<std::mutex> lock(mtx_);
    for (quint64 id : taskIds) {
        const TransferTask *task = taskForIdLocked(id);
        if (task)
            result.push_back(*task);
    }
    return result;
}

std::optional<TransferTask>
TransferManager::taskSnapshot(quint64 taskId) const {
    std::lock_guard<std::mutex> lock(mtx_);
    const TransferTask *task = taskForIdLocked(taskId);
    if (!task)
        return std::nullopt;
    return *task;
}

bool TransferManager::hasActiveTaskForSource(TransferTask::Type type,
                                             const QString &source) const {
    std::lock_guard<std::mutex> lock(mtx_);
    return std::any_of(queueStore_.nodes().cbegin(), queueStore_.nodes().cend(),
                       [&](const auto &taskNode) {
                           return taskNode->type == type &&
                                  taskNode->src == source &&
                                  !isTerminalTransferStatus(taskNode->status);
                       });
}

bool TransferManager::hasActiveTaskForDestination(
    TransferTask::Type type, const QString &destination) const {
    std::lock_guard<std::mutex> lock(mtx_);
    return std::any_of(queueStore_.nodes().cbegin(), queueStore_.nodes().cend(),
                       [&](const auto &taskNode) {
                           return taskNode->type == type &&
                                  taskNode->dst == destination &&
                                  !isTerminalTransferStatus(taskNode->status);
                       });
}

std::optional<quint64>
TransferManager::activeTaskIdForPaths(TransferTask::Type type,
                                      const QString &source,
                                      const QString &destination) const {
    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto &taskNode : queueStore_.nodes()) {
        const TransferTask &task = *taskNode;
        const bool belongsToCurrentSession =
            currentSessionKey_.isEmpty() || task.sessionKey.isEmpty() ||
            task.sessionKey == currentSessionKey_;
        if (belongsToCurrentSession && task.type == type &&
            task.src == source && task.dst == destination &&
            !isTerminalTransferStatus(task.status)) {
            return task.taskId;
        }
    }
    return std::nullopt;
}

QVector<quint64>
TransferManager::activeTaskIdsForSession(const QString &sessionKey) const {
    QVector<quint64> result;
    std::lock_guard<std::mutex> lock(mtx_);
    for (const auto &taskNode : queueStore_.nodes()) {
        const auto &task = *taskNode;
        const bool belongsToSession = sessionKey.isEmpty() ||
                                      task.sessionKey.isEmpty() ||
                                      task.sessionKey == sessionKey;
        const bool active = task.status == Status::Queued ||
                            task.status == Status::Running ||
                            task.status == Status::RetryWaiting;
        if (belongsToSession && active)
            result.push_back(task.taskId);
    }
    return result;
}

bool TransferManager::isBatchTerminal(quint64 batchId) const {
    if (batchId == 0)
        return false;
    std::lock_guard<std::mutex> lock(mtx_);
    bool found = false;
    for (const auto &taskNode : queueStore_.nodes()) {
        if (taskNode->batchId != batchId)
            continue;
        found = true;
        if (!isTerminalTransferStatus(taskNode->status))
            return false;
    }
    return found;
}

// Worker scheduling and cancellation

bool TransferManager::hasRunnableTaskLocked(std::size_t slotIndex) {
    if (shuttingDown_.load() || paused_.load() ||
        slotIndex >= static_cast<std::size_t>(maxConcurrent_.load()) ||
        !client_ || !sessionOpt_.has_value() || queueStore_.nodes().empty()) {
        return false;
    }
    for (const auto &taskNode : queueStore_.nodes()) {
        const auto &task = *taskNode;
        if (task.status != Status::Queued)
            continue;
        if (!dependencySatisfiedLocked(task))
            continue;
        if (!task.sessionKey.isEmpty() &&
            task.sessionKey != currentSessionKey_) {
            continue;
        }
        if (!reservedDestinations_.count(destinationKey(task)))
            return true;
    }
    return false;
}

std::optional<TransferTask>
TransferManager::pickRunnableTaskLocked(std::size_t slotIndex) {
    if (!hasRunnableTaskLocked(slotIndex))
        return std::nullopt;
    const auto taskIndex = scheduler_.nextRunnable(
        queueStore_.nodes(), [this](TransferTask &task) {
            if (task.status != Status::Queued ||
                !dependencySatisfiedLocked(task)) {
                return false;
            }
            if (!task.sessionKey.isEmpty() &&
                task.sessionKey != currentSessionKey_) {
                task.status = Status::WaitingForConnection;
                return false;
            }
            return reserveDestinationLocked(task);
        });
    if (!taskIndex)
        return std::nullopt;

    TransferTask &task = *queueStore_.nodes()[*taskIndex];
    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    task.status = Status::Running;
    task.error.clear();
    task.startedAtMs = now;
    task.finishedAtMs = 0;
    task.nextRetryAtMs = 0;
    activeTaskIds_.insert(task.taskId);
    running_.fetch_add(1);
    return task;
}

void TransferManager::workerLoop(std::size_t slotIndex,
                                 std::stop_token stopToken) {
    WorkerSlot &slot = *workerSlots_[slotIndex];
    while (!stopToken.stop_requested() && !shuttingDown_.load()) {
        std::optional<TransferTask> task;
        {
            std::unique_lock<std::mutex> lock(mtx_);
            workCv_.wait(lock, [this, slotIndex, &stopToken] {
                return stopToken.stop_requested() || shuttingDown_.load() ||
                       hasRunnableTaskLocked(slotIndex);
            });
            if (stopToken.stop_requested() || shuttingDown_.load())
                break;
            task = pickRunnableTaskLocked(slotIndex);
        }
        if (!task.has_value())
            continue;
        slot.activeTaskId.store(task->taskId);
        publishUpdated({task->taskId});
        executeTask(slot, std::move(*task), stopToken);
        slot.activeTaskId.store(0);
    }
}

std::shared_ptr<openscp::RemoteClient>
TransferManager::workerClient(WorkerSlot &slot, quint64 taskId,
                              quint64 generation, std::string &err) {
    {
        std::lock_guard<std::mutex> lock(slot.clientMutex);
        if (slot.client && slot.clientGeneration == generation &&
            slot.client->isConnected()) {
            err.clear();
            return slot.client;
        }
    }
    invalidateWorkerClient(slot);

    std::unique_ptr<openscp::RemoteClient> created;
    {
        // This lock also guarantees clearClient() cannot return and allow the
        // raw control client to be destroyed while newConnectionLike uses it.
        std::lock_guard<std::mutex> factoryLock(connFactoryMutex_);
        openscp::RemoteClient *base = nullptr;
        std::optional<openscp::SessionOptions> options;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            if (generation != sessionGeneration_ || paused_.load() ||
                canceledTasks_.count(taskId) || pausedTasks_.count(taskId)) {
                err = QCoreApplication::translate(
                          "TransferManager",
                          "Transfer queue paused or disconnected")
                          .toUtf8()
                          .toStdString();
                return {};
            }
            base = client_;
            options = sessionOpt_;
        }
        if (!base || !options.has_value()) {
            err = QCoreApplication::translate(
                      "TransferManager", "No transfer connection is available")
                      .toUtf8()
                      .toStdString();
            return {};
        }
        created = base->newConnectionLike(*options, err);
    }
    if (!created)
        return {};

    auto shared = std::shared_ptr<openscp::RemoteClient>(std::move(created));
    {
        std::lock_guard<std::mutex> lock(slot.clientMutex);
        slot.client = shared;
        slot.clientGeneration = generation;
    }
    return shared;
}

void TransferManager::invalidateWorkerClient(WorkerSlot &slot) {
    std::shared_ptr<openscp::RemoteClient> stale;
    {
        std::lock_guard<std::mutex> lock(slot.clientMutex);
        stale = std::move(slot.client);
        slot.clientGeneration = 0;
    }
    if (stale)
        stale->disconnect();
}

void TransferManager::interruptTask(quint64 taskId) {
    for (auto &slot : workerSlots_) {
        if (slot->activeTaskId.load() != taskId)
            continue;
        std::shared_ptr<openscp::RemoteClient> active;
        {
            std::lock_guard<std::mutex> lock(slot->clientMutex);
            active = slot->client;
        }
        if (active)
            active->interrupt();
    }
    retryCv_.notify_all();
    bandwidthLimiter_.wakeAll();
    workCv_.notify_all();
}

void TransferManager::interruptAllActive() {
    for (auto &slot : workerSlots_) {
        if (slot->activeTaskId.load() == 0)
            continue;
        std::shared_ptr<openscp::RemoteClient> active;
        {
            std::lock_guard<std::mutex> lock(slot->clientMutex);
            active = slot->client;
        }
        if (active)
            active->interrupt();
    }
    retryCv_.notify_all();
    bandwidthLimiter_.wakeAll();
    workCv_.notify_all();
}

bool TransferManager::shouldCancel(quint64 taskId) const {
    if (shuttingDown_.load() || paused_.load())
        return true;
    std::lock_guard<std::mutex> lock(mtx_);
    return canceledTasks_.count(taskId) || pausedTasks_.count(taskId);
}

// Task state transitions and queue controls

void TransferManager::transitionToQueued(TransferTask &task, qint64 nowMs,
                                         bool resume) {
    task.status = Status::Queued;
    task.resumeHint = resume;
    task.queuedAtMs = nowMs;
    task.startedAtMs = 0;
    task.finishedAtMs = 0;
    task.nextRetryAtMs = 0;
    task.currentSpeedKBps = 0;
    task.etaSeconds = -1;
}

void TransferManager::transitionToPaused(TransferTask &task) {
    task.status = Status::Paused;
    task.currentSpeedKBps = 0;
    task.etaSeconds = -1;
    task.nextRetryAtMs = 0;
    task.finishedAtMs = 0;
}

void TransferManager::transitionToCanceled(TransferTask &task, qint64 nowMs) {
    const bool wasTerminal = isTerminalTransferStatus(task.status);
    task.status = Status::Canceled;
    task.error.clear();
    task.currentSpeedKBps = 0;
    task.etaSeconds = -1;
    task.nextRetryAtMs = 0;
    task.finishedAtMs = nowMs;
    if (!wasTerminal)
        ++terminalTaskCount_;
}

void TransferManager::transitionToError(TransferTask &task,
                                        const std::string &rawError,
                                        qint64 nowMs) {
    const bool wasTerminal = isTerminalTransferStatus(task.status);
    task.status = Status::Error;
    task.error = errorForUi(rawError);
    task.currentSpeedKBps = 0;
    task.etaSeconds = -1;
    task.nextRetryAtMs = 0;
    task.finishedAtMs = nowMs;
    if (!wasTerminal)
        ++terminalTaskCount_;
}

void TransferManager::transitionToDone(TransferTask &task, qint64 nowMs) {
    const bool wasTerminal = isTerminalTransferStatus(task.status);
    task.status = Status::Done;
    task.phase = TransferPhase::Finished;
    task.progress = 100;
    if (task.bytesTotal > 0)
        task.bytesDone = task.bytesTotal;
    task.currentSpeedKBps = 0;
    task.etaSeconds = 0;
    task.nextRetryAtMs = 0;
    task.finishedAtMs = nowMs;
    if (!wasTerminal)
        ++terminalTaskCount_;
}

void TransferManager::resetForRetry(TransferTask &task, qint64 nowMs) {
    if (isTerminalTransferStatus(task.status) && terminalTaskCount_ > 0)
        --terminalTaskCount_;
    task.attempts = 0;
    task.error.clear();
    task.finishedAtMs = 0;
    task.nextRetryAtMs = 0;
    if (task.phase == TransferPhase::Transfer) {
        task.progress = 0;
        task.bytesDone = 0;
        task.bytesTotal = 0;
    }
    transitionToQueued(task, nowMs, task.resumeHint);
}

void TransferManager::pauseAll() {
    paused_.store(true);
    QVector<quint64> changed;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        for (auto &taskNode : queueStore_.nodes()) {
            auto &task = *taskNode;
            if (task.status == Status::Queued ||
                task.status == Status::Running ||
                task.status == Status::RetryWaiting) {
                pausedTasks_.insert(task.taskId);
                transitionToPaused(task);
                changed.push_back(task.taskId);
            }
        }
    }
    if (!changed.isEmpty())
        publishUpdated(changed);
    else
        emit queueSettingsChanged();
    interruptAllActive();
}

void TransferManager::resumeAll() {
    paused_.store(false);
    QVector<quint64> changed;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (auto &taskNode : queueStore_.nodes()) {
            auto &task = *taskNode;
            if (task.status != Status::Paused &&
                task.status != Status::WaitingForConnection) {
                continue;
            }
            if (!task.sessionKey.isEmpty() &&
                task.sessionKey != currentSessionKey_) {
                task.status = Status::WaitingForConnection;
                continue;
            }
            if (activeTaskIds_.count(task.taskId)) {
                resumeRequestedTasks_.insert(task.taskId);
                continue;
            }
            pausedTasks_.erase(task.taskId);
            transitionToQueued(task, now, true);
            changed.push_back(task.taskId);
        }
    }
    if (!changed.isEmpty())
        publishUpdated(changed);
    emit queueSettingsChanged();
    workCv_.notify_all();
}

void TransferManager::pauseTask(quint64 taskId) {
    bool interrupt = false;
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        TransferTask *task = taskForIdLocked(taskId);
        if (task && (task->status == Status::Queued ||
                     task->status == Status::Running ||
                     task->status == Status::RetryWaiting ||
                     task->status == Status::WaitingForConnection)) {
            interrupt = activeTaskIds_.count(taskId);
            pausedTasks_.insert(taskId);
            resumeRequestedTasks_.erase(taskId);
            transitionToPaused(*task);
            changed = true;
        }
    }
    if (changed)
        publishUpdated({taskId});
    if (interrupt)
        interruptTask(taskId);
}

void TransferManager::resumeTask(quint64 taskId) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        TransferTask *storedTask = taskForIdLocked(taskId);
        if (!storedTask)
            return;
        auto &task = *storedTask;
        if (task.status != Status::Paused &&
            task.status != Status::WaitingForConnection) {
            return;
        }
        if (!task.sessionKey.isEmpty() &&
            task.sessionKey != currentSessionKey_) {
            task.status = Status::WaitingForConnection;
            return;
        }
        if (activeTaskIds_.count(taskId)) {
            resumeRequestedTasks_.insert(taskId);
        } else {
            pausedTasks_.erase(taskId);
            transitionToQueued(task, QDateTime::currentMSecsSinceEpoch(), true);
            changed = true;
        }
    }
    if (changed)
        publishUpdated({taskId});
    workCv_.notify_all();
}

void TransferManager::cancelTask(quint64 taskId) {
    bool active = false;
    bool changed = false;
    QVector<quint64> removed;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        TransferTask *task = taskForIdLocked(taskId);
        if (task && !isTerminalTransferStatus(task->status)) {
            active = activeTaskIds_.count(taskId);
            canceledTasks_.insert(taskId);
            pausedTasks_.erase(taskId);
            resumeRequestedTasks_.erase(taskId);
            transitionToCanceled(*task, QDateTime::currentMSecsSinceEpoch());
            changed = true;
            removed = pruneTerminalHistoryLocked();
        }
    }
    if (changed)
        publishUpdated({taskId});
    if (!removed.isEmpty())
        publishRemoved(removed);
    if (active)
        interruptTask(taskId);
}

void TransferManager::cancelAll() {
    QVector<quint64> changed;
    QVector<quint64> removed;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        resumeRequestedTasks_.clear();
        for (auto &taskNode : queueStore_.nodes()) {
            auto &task = *taskNode;
            if (isTerminalTransferStatus(task.status))
                continue;
            canceledTasks_.insert(task.taskId);
            pausedTasks_.erase(task.taskId);
            transitionToCanceled(task, now);
            changed.push_back(task.taskId);
        }
        removed = pruneTerminalHistoryLocked();
    }
    if (!changed.isEmpty())
        publishUpdated(changed);
    if (!removed.isEmpty())
        publishRemoved(removed);
    interruptAllActive();
}

void TransferManager::setTaskSpeedLimit(quint64 taskId, int kbps) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        TransferTask *task = taskForIdLocked(taskId);
        if (task) {
            const int bounded = std::max(0, kbps);
            changed = task->speedLimitKBps != bounded;
            task->speedLimitKBps = bounded;
        }
    }
    if (changed)
        publishUpdated({taskId});
}

void TransferManager::retryFailed() {
    QVector<quint64> changed;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        const qint64 now = QDateTime::currentMSecsSinceEpoch();
        for (auto &taskNode : queueStore_.nodes()) {
            auto &task = *taskNode;
            if (!canRetry(task.status) || task.commitUncertain ||
                activeTaskIds_.count(task.taskId))
                continue;
            canceledTasks_.erase(task.taskId);
            pausedTasks_.erase(task.taskId);
            resetForRetry(task, now);
            changed.push_back(task.taskId);
        }
    }
    if (!changed.isEmpty())
        publishUpdated(changed);
    workCv_.notify_all();
}

void TransferManager::retryTask(quint64 taskId) {
    bool changed = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        TransferTask *task = taskForIdLocked(taskId);
        if (task && canRetry(task->status) && !task->commitUncertain &&
            !activeTaskIds_.count(taskId)) {
            canceledTasks_.erase(taskId);
            pausedTasks_.erase(taskId);
            resetForRetry(*task, QDateTime::currentMSecsSinceEpoch());
            changed = true;
        }
    }
    if (changed) {
        publishUpdated({taskId});
        workCv_.notify_all();
    }
}

void TransferManager::removeTask(quint64 taskId, bool removePartialData) {
    TransferTask removed{};
    removed.type = TransferTask::Type::Download;
    bool didRemove = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        TransferTask *task = taskForIdLocked(taskId);
        if (!task || activeTaskIds_.count(taskId))
            return;
        removed = *task;
        const auto position = std::find_if(
            queueStore_.nodes().begin(), queueStore_.nodes().end(),
            [task](const auto &node) { return node.get() == task; });
        if (position == queueStore_.nodes().end())
            return;
        queueStore_.nodes().erase(position);
        canceledTasks_.erase(taskId);
        pausedTasks_.erase(taskId);
        resumeRequestedTasks_.erase(taskId);
        releaseDestinationLocked(taskId);
        rebuildTaskLookupLocked();
        forgetBatchPolicyIfUnusedLocked(removed.batchId);
        didRemove = true;
    }
    if (!didRemove)
        return;
    if (removePartialData && removed.type == TransferTask::Type::Download)
        QFile::remove(removed.dst + QStringLiteral(".part"));
    publishRemoved({taskId});
    if (removePartialData && removed.type == TransferTask::Type::Upload) {
        // Remote cleanup must stay off the UI thread. Represent it as a normal
        // persistent queue operation so it can wait for the matching session.
        TransferBatchOptions cleanup;
        cleanup.batchId = removed.batchId;
        cleanup.sessionKey = removed.sessionKey;
        cleanup.conflictPolicy = Policy::Skip;
        enqueueRemoteDelete(removed.dst + QStringLiteral(".part"), false,
                            cleanup);
    }
}

void TransferManager::removeInactiveTasks(
    const std::function<bool(const TransferTask &)> &shouldRemove) {
    QVector<quint64> removed;
    QSet<quint64> removedBatches;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        std::vector<std::unique_ptr<TransferTask>> kept;
        kept.reserve(queueStore_.nodes().size());
        for (auto &taskNode : queueStore_.nodes()) {
            const auto &task = *taskNode;
            if (shouldRemove(task) && !activeTaskIds_.count(task.taskId)) {
                removed.push_back(task.taskId);
                removedBatches.insert(task.batchId);
            } else {
                kept.push_back(std::move(taskNode));
            }
        }
        queueStore_.nodes().swap(kept);
        rebuildTaskLookupLocked();
        for (quint64 batchId : removedBatches)
            forgetBatchPolicyIfUnusedLocked(batchId);
    }
    if (!removed.isEmpty())
        publishRemoved(removed);
}

void TransferManager::clearCompleted() {
    removeInactiveTasks([](const TransferTask &task) {
        return task.status == Status::Done || task.status == Status::Skipped;
    });
}

void TransferManager::clearFailedCanceled() {
    removeInactiveTasks([](const TransferTask &task) {
        return task.status == Status::Error ||
               task.status == Status::Canceled ||
               task.status == Status::Warning || task.status == Status::Skipped;
    });
}

void TransferManager::clearFinishedOlderThan(int minutes, bool clearDone,
                                             bool clearFailedCanceled) {
    if (minutes <= 0 || (!clearDone && !clearFailedCanceled))
        return;
    const qint64 cutoff = QDateTime::currentMSecsSinceEpoch() -
                          static_cast<qint64>(minutes) * 60 * 1000;
    removeInactiveTasks([=](const TransferTask &task) {
        const bool selected =
            (clearDone && task.status == Status::Done) ||
            (clearFailedCanceled &&
             (task.status == Status::Error || task.status == Status::Canceled ||
              task.status == Status::Skipped ||
              task.status == Status::Warning));
        return selected && task.finishedAtMs > 0 && task.finishedAtMs <= cutoff;
    });
}

void TransferManager::schedule() {
    if (!paused_.load())
        workCv_.notify_all();
}

// Retry and conflict policy

bool TransferManager::waitForRetry(quint64 taskId, int delayMs,
                                   std::stop_token stopToken) {
    const auto deadline = Clock::now() + std::chrono::milliseconds(delayMs);
    while (Clock::now() < deadline) {
        if (stopToken.stop_requested() || shouldCancel(taskId))
            return false;
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline -
                                                                  Clock::now());
        std::unique_lock<std::mutex> lock(retryMutex_);
        retryCv_.wait_for(lock,
                          std::min(remaining, std::chrono::milliseconds(50)));
    }
    return !stopToken.stop_requested() && !shouldCancel(taskId);
}

bool TransferManager::shouldRetryError(
    const openscp::RemoteError &structuredError, const std::string &rawError,
    int &retryAfterMs) const {
    retryAfterMs = -1;
    if (structuredError.commit_uncertain)
        return false;
    if (structuredError.retry_after_seconds.has_value()) {
        const quint64 delay =
            static_cast<quint64>(*structuredError.retry_after_seconds) * 1000;
        retryAfterMs = static_cast<int>(std::min<quint64>(delay, 60000));
    }

    using ErrorKind = openscp::RemoteErrorKind;
    if (structuredError.kind != ErrorKind::None &&
        structuredError.kind != ErrorKind::Unknown) {
        // These categories are never safe automatic retries even if a backend
        // accidentally marked them transient.
        switch (structuredError.kind) {
        case ErrorKind::Canceled:
        case ErrorKind::InvalidRequest:
        case ErrorKind::Unsupported:
        case ErrorKind::NotFound:
        case ErrorKind::Authentication:
        case ErrorKind::PermissionDenied:
        case ErrorKind::Certificate:
        case ErrorKind::Conflict:
        case ErrorKind::Integrity:
        case ErrorKind::InsufficientSpace:
        case ErrorKind::LocalIo:
            return false;
        case ErrorKind::None:
        case ErrorKind::Unknown:
            break;
        case ErrorKind::Timeout:
        case ErrorKind::Connection:
        case ErrorKind::RateLimited:
        case ErrorKind::RemoteIo:
        case ErrorKind::Protocol:
            return structuredError.transient;
        }
        return false;
    }

    // Compatibility fallback for backends that have not populated structured
    // metadata yet. It deliberately defaults to no retry.
    const QString lower = QString::fromStdString(rawError).trimmed().toLower();
    if (lower.isEmpty())
        return false;

    static const QStringList permanent = {
        QStringLiteral("authentication"),
        QStringLiteral("permission denied"),
        QStringLiteral("access denied"),
        QStringLiteral("certificate"),
        QStringLiteral("host key"),
        QStringLiteral("checksum"),
        QStringLiteral("integrity"),
        QStringLiteral("no space"),
        QStringLiteral("disk full"),
        QStringLiteral("read-only"),
        QStringLiteral("invalid path"),
        QStringLiteral("not a directory"),
        QStringLiteral("is a directory"),
        QStringLiteral("commit uncertain"),
        QStringLiteral("result is uncertain"),
        QStringLiteral("unknown whether"),
    };
    for (const QString &needle : permanent) {
        if (lower.contains(needle))
            return false;
    }

    const QRegularExpression retryAfterPattern(
        QStringLiteral("retry[- ]after\\s*[:=]?\\s*(\\d+)"));
    const auto match = retryAfterPattern.match(lower);
    if (match.hasMatch()) {
        bool ok = false;
        const qint64 seconds = match.captured(1).toLongLong(&ok);
        if (ok)
            retryAfterMs =
                static_cast<int>(std::clamp<qint64>(seconds, 0, 60) * 1000);
    }

    static const QStringList transient = {
        QStringLiteral("timeout"),
        QStringLiteral("timed out"),
        QStringLiteral("temporar"),
        QStringLiteral("try again"),
        QStringLiteral("retry-after"),
        QStringLiteral("connection reset"),
        QStringLiteral("connection refused"),
        QStringLiteral("connection closed"),
        QStringLiteral("could not connect"),
        QStringLiteral("no transfer connection"),
        QStringLiteral("network"),
        QStringLiteral("broken pipe"),
        QStringLiteral("unavailable"),
        QStringLiteral("resolve host"),
        QStringLiteral("eof"),
    };
    for (const QString &needle : transient) {
        if (lower.contains(needle))
            return true;
    }
    return false;
}

bool TransferManager::isTransportFailure(
    const openscp::RemoteError &structuredError,
    const std::string &rawError) const {
    using ErrorKind = openscp::RemoteErrorKind;
    if (structuredError.commit_uncertain)
        return true;
    switch (structuredError.kind) {
    case ErrorKind::Timeout:
    case ErrorKind::Connection:
    case ErrorKind::Protocol:
        return true;
    case ErrorKind::None:
    case ErrorKind::Unknown:
        break;
    case ErrorKind::Canceled:
    case ErrorKind::InvalidRequest:
    case ErrorKind::Unsupported:
    case ErrorKind::NotFound:
    case ErrorKind::Authentication:
    case ErrorKind::PermissionDenied:
    case ErrorKind::Certificate:
    case ErrorKind::RateLimited:
    case ErrorKind::Conflict:
    case ErrorKind::Integrity:
    case ErrorKind::InsufficientSpace:
    case ErrorKind::LocalIo:
    case ErrorKind::RemoteIo:
        return false;
    }

    const QString lower = QString::fromStdString(rawError).trimmed().toLower();
    static const QStringList transportMarkers = {
        QStringLiteral("timeout"),
        QStringLiteral("timed out"),
        QStringLiteral("connection reset"),
        QStringLiteral("connection refused"),
        QStringLiteral("connection closed"),
        QStringLiteral("could not connect"),
        QStringLiteral("no transfer connection"),
        QStringLiteral("broken pipe"),
        QStringLiteral("resolve host"),
        QStringLiteral("network unreachable"),
        QStringLiteral("unexpected eof"),
    };
    return std::any_of(
        transportMarkers.cbegin(), transportMarkers.cend(),
        [&lower](const QString &marker) { return lower.contains(marker); });
}

ConflictResolution TransferManager::resolveConflict(
    TransferTask &task, bool allowResume, const QString &name,
    const QString &sourceInfo, const QString &destinationInfo,
    std::optional<qint64> sourceMtime, std::optional<qint64> destinationMtime) {
    const ConflictRequest request{
        task.batchId,
        allowResume,
        sourceMtime,
        destinationMtime,
    };
    const auto promptResolver = [this, taskId = task.taskId, name, sourceInfo,
                                 destinationInfo](
                                    const ConflictRequest &promptRequest) {
        struct PromptState {
            std::mutex mutex;
            std::condition_variable cv;
            ConflictResolution decision;
            bool complete = false;
            std::atomic<bool> abandoned{false};
        };
        auto state = std::make_shared<PromptState>();
        QPointer<TransferManager> safeThis(this);
        const bool invoked = QMetaObject::invokeMethod(
            this,
            [state, safeThis, name, sourceInfo, destinationInfo,
             promptRequest]() {
                if (!safeThis || state->abandoned.load()) {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->decision.canceled = true;
                    state->complete = true;
                    state->cv.notify_one();
                    return;
                }

                QMessageBox message(nullptr);
                UiAlerts::configure(message, Qt::ApplicationModal);
                message.setWindowTitle(
                    QCoreApplication::translate("TransferManager", "Conflict"));
                message.setText(
                    QCoreApplication::translate(
                        "TransferManager",
                        "«%1» already exists.\nSource: %2\nDestination: %3")
                        .arg(name, sourceInfo, destinationInfo));
                auto *overwrite = message.addButton(
                    QCoreApplication::translate("TransferManager", "Overwrite"),
                    QMessageBox::AcceptRole);
                QAbstractButton *resume = nullptr;
                if (promptRequest.allowResume) {
                    resume = message.addButton(QCoreApplication::translate(
                                                   "TransferManager", "Resume"),
                                               QMessageBox::ActionRole);
                }
                auto *rename = message.addButton(
                    QCoreApplication::translate("TransferManager", "Rename"),
                    QMessageBox::ActionRole);
                QAbstractButton *newer = nullptr;
                if (promptRequest.sourceMtime.has_value() &&
                    promptRequest.destinationMtime.has_value()) {
                    newer = message.addButton(
                        QCoreApplication::translate("TransferManager",
                                                    "Copy if newer"),
                        QMessageBox::ActionRole);
                }
                auto *skip = message.addButton(
                    QCoreApplication::translate("TransferManager", "Skip"),
                    QMessageBox::RejectRole);
                auto *apply = new QCheckBox(
                    QCoreApplication::translate(
                        "TransferManager",
                        "Apply this decision to remaining conflicts"),
                    &message);
                message.setCheckBox(apply);
                message.exec();

                ConflictResolution decision;
                decision.applyToRemaining = apply->isChecked();
                if (message.clickedButton() == overwrite)
                    decision.policy = Policy::Overwrite;
                else if (resume && message.clickedButton() == resume)
                    decision.policy = Policy::Resume;
                else if (message.clickedButton() == rename)
                    decision.policy = Policy::Rename;
                else if (newer && message.clickedButton() == newer)
                    decision.policy = Policy::NewerOnly;
                else if (message.clickedButton() == skip)
                    decision.policy = Policy::Skip;
                else
                    decision.canceled = true;

                {
                    std::lock_guard<std::mutex> lock(state->mutex);
                    state->decision = decision;
                    state->complete = true;
                }
                state->cv.notify_one();
            },
            Qt::QueuedConnection);
        if (!invoked)
            return ConflictResolution{Policy::Skip, false, true};

        std::unique_lock<std::mutex> stateLock(state->mutex);
        while (!state->complete) {
            if (state->cv.wait_for(stateLock, std::chrono::milliseconds(50)) ==
                    std::cv_status::timeout &&
                shouldCancel(taskId)) {
                state->abandoned.store(true);
                return ConflictResolution{Policy::Skip, false, true};
            }
        }
        return state->decision;
    };

    ConflictResolution decision = conflictCoordinator_.resolve(
        request, task.conflictPolicy, promptResolver,
        [this, taskId = task.taskId] { return shouldCancel(taskId); });
    const Policy stored =
        conflictCoordinator_.batchPolicy(task.batchId, task.conflictPolicy);
    task.conflictPolicy = stored;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (decision.applyToRemaining && !decision.canceled) {
            for (auto &queuedNode : queueStore_.nodes()) {
                auto &queued = *queuedNode;
                if (queued.batchId == task.batchId &&
                    !isTerminalTransferStatus(queued.status)) {
                    queued.conflictPolicy = stored;
                }
            }
        } else {
            TransferTask *storedTask = taskForIdLocked(task.taskId);
            if (storedTask)
                storedTask->conflictPolicy = stored;
        }
    }
    return decision;
}

bool TransferManager::chooseRenamedDestination(
    TransferTask &task,
    const std::shared_ptr<openscp::RemoteClient> &workerClient,
    std::string &err) {
    for (int suffix = 1; suffix <= 9999; ++suffix) {
        const QString candidate = task.type == TransferTask::Type::Download
                                      ? numberedPath(task.dst, suffix)
                                      : numberedRemotePath(task.dst, suffix);
        if (task.type == TransferTask::Type::Download) {
            if (QFileInfo::exists(candidate))
                continue;
        } else {
            bool isDirectory = false;
            std::string existsError;
            const bool exists = workerClient->exists(candidate.toStdString(),
                                                     isDirectory, existsError);
            if (!existsError.empty()) {
                err = existsError;
                return false;
            }
            if (exists)
                continue;
        }

        std::lock_guard<std::mutex> lock(mtx_);
        TransferTask candidateTask{};
        candidateTask.type = task.type;
        candidateTask.dst = candidate;
        const std::string key = destinationKey(candidateTask);
        if (reservedDestinations_.count(key))
            continue;
        releaseDestinationLocked(task.taskId);
        task.dst = candidate;
        if (!reserveDestinationLocked(task)) {
            err =
                QCoreApplication::translate(
                    "TransferManager", "Could not reserve renamed destination")
                    .toUtf8()
                    .toStdString();
            return false;
        }
        TransferTask *storedTask = taskForIdLocked(task.taskId);
        if (storedTask)
            storedTask->dst = candidate;
        return true;
    }
    err = QCoreApplication::translate(
              "TransferManager", "Could not find an available destination name")
              .toUtf8()
              .toStdString();
    return false;
}

// Transfer execution

TransferManager::PrecheckOutcome TransferManager::precheckTask(
    TransferTask &task,
    const std::shared_ptr<openscp::RemoteClient> &workerClient,
    const openscp::ProtocolCapabilities &caps, bool &resume, std::string &err) {
    resume = task.resumeHint;
    if (task.type == TransferTask::Type::Upload) {
        if (caps.can_stat && caps.can_read_metadata) {
            bool isDirectory = false;
            std::string existsError;
            const bool exists = workerClient->exists(task.dst.toStdString(),
                                                     isDirectory, existsError);
            if (!existsError.empty()) {
                err = existsError;
                return PrecheckOutcome::Error;
            }
            if (exists) {
                openscp::FileInfo remoteInfo{};
                std::string statError;
                const bool hasRemoteInfo = workerClient->stat(
                    task.dst.toStdString(), remoteInfo, statError);
                const QFileInfo localInfo(task.src);
                const QString sourceInfo = QStringLiteral("%1 bytes, %2")
                                               .arg(localInfo.size())
                                               .arg(openscpui::localShortTime(
                                                   localInfo.lastModified()));
                const QString destinationInfo =
                    QStringLiteral("%1 bytes, %2")
                        .arg(remoteInfo.size)
                        .arg(remoteInfo.mtime
                                 ? openscpui::localShortTime(remoteInfo.mtime)
                                 : QStringLiteral("?"));
                const auto decision = resolveConflict(
                    task, caps.can_resume_upload, localInfo.fileName(),
                    sourceInfo, destinationInfo,
                    localInfo.exists()
                        ? std::optional<qint64>(
                              localInfo.lastModified().toSecsSinceEpoch())
                        : std::nullopt,
                    hasRemoteInfo && remoteInfo.mtime
                        ? std::optional<qint64>(remoteInfo.mtime)
                        : std::nullopt);
                if (decision.canceled)
                    return PrecheckOutcome::Canceled;
                if (decision.policy == Policy::Skip)
                    return PrecheckOutcome::Skipped;
                if (decision.policy == Policy::Rename) {
                    if (!chooseRenamedDestination(task, workerClient, err))
                        return PrecheckOutcome::Error;
                    publishUpdated({task.taskId});
                }
                resume = decision.policy == Policy::Resume;
            }

            const QString parent = QFileInfo(task.dst).path();
            if (caps.can_mkdir && !parent.isEmpty() &&
                parent != QStringLiteral(".")) {
                QString current = QStringLiteral("/");
                const QStringList pieces =
                    parent.split('/', Qt::SkipEmptyParts);
                for (const QString &piece : pieces) {
                    const QString next =
                        current == QStringLiteral("/")
                            ? current + piece
                            : current + QStringLiteral("/") + piece;
                    bool componentIsDirectory = false;
                    std::string componentExistsError;
                    const bool componentExists = workerClient->exists(
                        next.toStdString(), componentIsDirectory,
                        componentExistsError);
                    if (!componentExistsError.empty()) {
                        err = componentExistsError;
                        return PrecheckOutcome::Error;
                    }
                    if (!componentExists) {
                        if (!workerClient->mkdir(next.toStdString(), err, 0755))
                            return PrecheckOutcome::Error;
                    } else if (!componentIsDirectory) {
                        err =
                            QCoreApplication::translate(
                                "TransferManager",
                                "Remote path component is not a directory: %1")
                                .arg(next)
                                .toUtf8()
                                .toStdString();
                        return PrecheckOutcome::Error;
                    }
                    current = next;
                }
            }
        }
    } else {
        const QFileInfo localInfo(task.dst);
        if (localInfo.exists()) {
            openscp::FileInfo remoteInfo{};
            std::string statError;
            const bool hasRemoteInfo =
                caps.can_stat && caps.can_read_metadata &&
                workerClient->stat(task.src.toStdString(), remoteInfo,
                                   statError);
            const QString sourceInfo =
                hasRemoteInfo
                    ? QStringLiteral("%1 bytes, %2")
                          .arg(remoteInfo.size)
                          .arg(remoteInfo.mtime
                                   ? openscpui::localShortTime(remoteInfo.mtime)
                                   : QStringLiteral("?"))
                    : QStringLiteral("? bytes, ?");
            const QString destinationInfo =
                QStringLiteral("%1 bytes, %2")
                    .arg(localInfo.size())
                    .arg(openscpui::localShortTime(localInfo.lastModified()));
            const auto decision = resolveConflict(
                task, caps.can_resume_download, localInfo.fileName(),
                sourceInfo, destinationInfo,
                hasRemoteInfo && remoteInfo.mtime
                    ? std::optional<qint64>(remoteInfo.mtime)
                    : std::nullopt,
                std::optional<qint64>(
                    localInfo.lastModified().toSecsSinceEpoch()));
            if (decision.canceled)
                return PrecheckOutcome::Canceled;
            if (decision.policy == Policy::Skip)
                return PrecheckOutcome::Skipped;
            if (decision.policy == Policy::Rename) {
                if (!chooseRenamedDestination(task, workerClient, err))
                    return PrecheckOutcome::Error;
                publishUpdated({task.taskId});
            }
            resume = decision.policy == Policy::Resume;
        }
        if (!QDir().mkpath(QFileInfo(task.dst).dir().absolutePath())) {
            err = QCoreApplication::translate(
                      "TransferManager",
                      "Could not create local destination directory")
                      .toUtf8()
                      .toStdString();
            return PrecheckOutcome::Error;
        }
    }
    const bool canResume = task.type == TransferTask::Type::Upload
                               ? caps.can_resume_upload
                               : caps.can_resume_download;
    if (resume && !canResume)
        resume = false;
    return PrecheckOutcome::Continue;
}

void TransferManager::updateProgress(quint64 taskId, std::size_t done,
                                     std::size_t total, double measuredKBps,
                                     int etaSeconds) {
    std::lock_guard<std::mutex> lock(mtx_);
    TransferTask *task = taskForIdLocked(taskId);
    if (!task)
        return;
    task->progress = total > 0 ? static_cast<int>((done * 100) / total) : 0;
    task->bytesDone = done;
    task->bytesTotal = total;
    if (measuredKBps > 0)
        task->currentSpeedKBps = measuredKBps;
    task->etaSeconds = etaSeconds;
}

bool TransferManager::runTransferAttempt(
    TransferTask &task,
    const std::shared_ptr<openscp::RemoteClient> &workerClient, bool resume,
    std::string &err) {
    TransferExecutor::Callbacks callbacks;
    callbacks.shouldCancel = [this, taskId = task.taskId] {
        return shouldCancel(taskId);
    };
    callbacks.acquireGlobalBandwidth = [this,
                                        taskId = task.taskId](quint64 bytes) {
        return bandwidthLimiter_.acquire(
            taskId, bytes, [this](std::uint64_t candidateTaskId) {
                return shouldCancel(candidateTaskId);
            });
    };
    callbacks.taskSpeedLimitKBps = [this, taskId = task.taskId] {
        std::lock_guard<std::mutex> lock(mtx_);
        const TransferTask *storedTask = taskForIdLocked(taskId);
        return storedTask ? storedTask->speedLimitKBps : 0;
    };
    callbacks.progress =
        [this, taskId = task.taskId](std::size_t done, std::size_t total,
                                     double measuredKBps, int etaSeconds,
                                     bool publishNow) {
            updateProgress(taskId, done, total, measuredKBps, etaSeconds);
            if (publishNow)
                publishUpdated({taskId});
        };
    return TransferExecutor::run(task, workerClient, resume, err, callbacks);
}

bool TransferManager::runPostAction(
    TransferTask &task,
    const std::shared_ptr<openscp::RemoteClient> &workerClient,
    std::string &err) {
    return TransferExecutor::runPostAction(task, workerClient, err);
}

void TransferManager::executeTask(WorkerSlot &slot, TransferTask task,
                                  std::stop_token stopToken) {
    const qint64 precheckStarted = QDateTime::currentMSecsSinceEpoch();
    qint64 precheckDone = precheckStarted;
    qint64 transferStarted = 0;
    quint64 generation = 0;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        generation = sessionGeneration_;
    }

    auto finish = [&]() {
        finishWorkerTask(task.taskId, precheckDone - precheckStarted,
                         transferStarted);
    };
    auto markStopped = [&]() {
        // interrupt() leaves backend-specific connection state undefined.
        // Never hand an interrupted client to the next task in this slot.
        invalidateWorkerClient(slot);
        std::lock_guard<std::mutex> lock(mtx_);
        TransferTask *storedTask = taskForIdLocked(task.taskId);
        if (!storedTask)
            return;
        if (canceledTasks_.count(task.taskId))
            transitionToCanceled(*storedTask,
                                 QDateTime::currentMSecsSinceEpoch());
        else if (storedTask->status != Status::WaitingForConnection)
            transitionToPaused(*storedTask);
    };
    auto markError = [&](const std::string &error) {
        std::lock_guard<std::mutex> lock(mtx_);
        TransferTask *storedTask = taskForIdLocked(task.taskId);
        if (storedTask)
            transitionToError(*storedTask, error,
                              QDateTime::currentMSecsSinceEpoch());
    };

    std::shared_ptr<openscp::RemoteClient> client;
    openscp::ProtocolCapabilities caps{};
    bool prechecked = task.phase != TransferPhase::Transfer ||
                      task.type == TransferTask::Type::CreateLocalDirectory ||
                      task.type == TransferTask::Type::CreateRemoteDirectory ||
                      task.type == TransferTask::Type::DeleteLocalFile ||
                      task.type == TransferTask::Type::DeleteLocalDirectory ||
                      task.type == TransferTask::Type::DeleteRemoteFile ||
                      task.type == TransferTask::Type::DeleteRemoteDirectory;
    bool resume = task.resumeHint;

    constexpr int maxAutomaticAttempts = 3;
    task.maxAttempts = maxAutomaticAttempts;
    for (int attempt = std::max(1, task.attempts + 1);
         attempt <= maxAutomaticAttempts; ++attempt) {
        if (stopToken.stop_requested() || shouldCancel(task.taskId)) {
            markStopped();
            publishUpdated({task.taskId});
            finish();
            return;
        }
        bool taskMissing = false;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            TransferTask *storedTask = taskForIdLocked(task.taskId);
            if (!storedTask) {
                // Active tasks cannot normally disappear; leave the lock
                // before finalization in case an embedding removed one.
                taskMissing = true;
            } else {
                storedTask->status = Status::Running;
                storedTask->attempts = attempt;
                storedTask->maxAttempts = maxAutomaticAttempts;
                storedTask->nextRetryAtMs = 0;
                storedTask->error.clear();
                task.attempts = attempt;
            }
        }
        if (taskMissing) {
            finish();
            return;
        }
        publishUpdated({task.taskId});

        std::string error;
        openscp::RemoteError operationError{};
        client = workerClient(slot, task.taskId, generation, error);
        bool failed = !client;
        if (client)
            caps = client->capabilities();

        if (!failed && !prechecked) {
            const PrecheckOutcome outcome =
                precheckTask(task, client, caps, resume, error);
            precheckDone = QDateTime::currentMSecsSinceEpoch();
            if (outcome == PrecheckOutcome::Canceled) {
                markStopped();
                publishUpdated({task.taskId});
                finish();
                return;
            }
            if (outcome == PrecheckOutcome::Skipped) {
                {
                    std::lock_guard<std::mutex> lock(mtx_);
                    TransferTask *storedTask = taskForIdLocked(task.taskId);
                    if (storedTask) {
                        if (!isTerminalTransferStatus(storedTask->status))
                            ++terminalTaskCount_;
                        storedTask->status = Status::Skipped;
                        storedTask->phase = TransferPhase::Finished;
                        storedTask->finishedAtMs =
                            QDateTime::currentMSecsSinceEpoch();
                        storedTask->error.clear();
                    }
                }
                publishUpdated({task.taskId});
                finish();
                return;
            }
            failed = outcome == PrecheckOutcome::Error;
            prechecked = !failed;
            if (failed && client)
                operationError = client->lastOperationError();
        }

        if (!failed && task.phase == TransferPhase::Transfer) {
            transferStarted = QDateTime::currentMSecsSinceEpoch();
            failed = !runTransferAttempt(task, client, resume, error);
            if (failed)
                operationError = client->lastOperationError();
            if (failed &&
                (task.type == TransferTask::Type::DeleteRemoteFile ||
                 task.type == TransferTask::Type::DeleteRemoteDirectory) &&
                operationError.kind == openscp::RemoteErrorKind::None) {
                // A legacy backend cannot prove whether a failed remote
                // mutation reached the server. Never retry it blindly.
                operationError.kind = openscp::RemoteErrorKind::RemoteIo;
                operationError.message = error;
                operationError.commit_uncertain = true;
            }
            if (!failed) {
                task.phase = task.postAction == TransferPostAction::DeleteSource
                                 ? TransferPhase::DeleteSource
                                 : TransferPhase::Finished;
                std::lock_guard<std::mutex> lock(mtx_);
                TransferTask *storedTask = taskForIdLocked(task.taskId);
                if (storedTask)
                    storedTask->phase = task.phase;
            }
        }

        if (!failed && shouldCancel(task.taskId)) {
            markStopped();
            publishUpdated({task.taskId});
            finish();
            return;
        }

        if (!failed && task.phase == TransferPhase::DeleteSource) {
            if (!runPostAction(task, client, error)) {
                operationError = client ? client->lastOperationError()
                                        : openscp::RemoteError{};
                if (isTransportFailure(operationError, error))
                    invalidateWorkerClient(slot);
                {
                    std::lock_guard<std::mutex> lock(mtx_);
                    TransferTask *storedTask = taskForIdLocked(task.taskId);
                    if (storedTask) {
                        if (!isTerminalTransferStatus(storedTask->status))
                            ++terminalTaskCount_;
                        storedTask->status = Status::Warning;
                        storedTask->phase = TransferPhase::DeleteSource;
                        storedTask->commitUncertain =
                            operationError.commit_uncertain;
                        storedTask->error =
                            operationError.commit_uncertain
                                ? QCoreApplication::translate(
                                      "TransferManager",
                                      "The source cleanup result is uncertain. "
                                      "Refresh both locations and reconcile "
                                      "the item manually before retrying.")
                                : errorForUi(error);
                        storedTask->finishedAtMs =
                            QDateTime::currentMSecsSinceEpoch();
                    }
                }
                publishUpdated({task.taskId});
                finish();
                return;
            }
            task.phase = TransferPhase::Finished;
        }

        if (!failed) {
            if (shouldCancel(task.taskId)) {
                markStopped();
                publishUpdated({task.taskId});
                finish();
                return;
            }
            bool stoppedDuringCompletion = false;
            {
                std::lock_guard<std::mutex> lock(mtx_);
                TransferTask *storedTask = taskForIdLocked(task.taskId);
                if (storedTask) {
                    if (canceledTasks_.count(task.taskId)) {
                        transitionToCanceled(
                            *storedTask, QDateTime::currentMSecsSinceEpoch());
                        stoppedDuringCompletion = true;
                    } else if (pausedTasks_.count(task.taskId)) {
                        transitionToPaused(*storedTask);
                        stoppedDuringCompletion = true;
                    } else {
                        transitionToDone(*storedTask,
                                         QDateTime::currentMSecsSinceEpoch());
                    }
                }
            }
            if (stoppedDuringCompletion)
                invalidateWorkerClient(slot);
            publishUpdated({task.taskId});
            finish();
            return;
        }

        if (shouldCancel(task.taskId)) {
            markStopped();
            publishUpdated({task.taskId});
            finish();
            return;
        }

        if (operationError.commit_uncertain) {
            if (isTransportFailure(operationError, error))
                invalidateWorkerClient(slot);
            {
                std::lock_guard<std::mutex> lock(mtx_);
                TransferTask *storedTask = taskForIdLocked(task.taskId);
                if (storedTask) {
                    if (!isTerminalTransferStatus(storedTask->status))
                        ++terminalTaskCount_;
                    storedTask->status = Status::Warning;
                    storedTask->commitUncertain = true;
                    storedTask->error = QCoreApplication::translate(
                        "TransferManager",
                        "The server may have committed this operation, "
                        "but its final result could not be confirmed. "
                        "Refresh the destination and reconcile it "
                        "manually; OpenSCP will not retry automatically.");
                    storedTask->currentSpeedKBps = 0;
                    storedTask->etaSeconds = -1;
                    storedTask->finishedAtMs =
                        QDateTime::currentMSecsSinceEpoch();
                }
            }
            publishUpdated({task.taskId});
            finish();
            return;
        }

        int retryAfterMs = -1;
        const bool retryable =
            attempt < maxAutomaticAttempts &&
            shouldRetryError(operationError, error, retryAfterMs);
        if (!retryable) {
            if (isTransportFailure(operationError, error))
                invalidateWorkerClient(slot);
            const std::string finalError =
                error.empty() ? QCoreApplication::translate("TransferManager",
                                                            "Transfer failed")
                                    .toUtf8()
                                    .toStdString()
                              : error;
            markError(finalError);
            publishUpdated({task.taskId});
            finish();
            return;
        }

        if (isTransportFailure(operationError, error))
            invalidateWorkerClient(slot);
        const int exponentialMs = attempt == 1 ? 1000 : 2000;
        const int backoffMs =
            exponentialMs + (QRandomGenerator::global()->bounded(251));
        const int delayMs =
            retryAfterMs >= 0
                ? std::max(backoffMs, std::min(retryAfterMs, 60000))
                : backoffMs;
        {
            std::lock_guard<std::mutex> lock(mtx_);
            TransferTask *storedTask = taskForIdLocked(task.taskId);
            if (storedTask) {
                storedTask->status = Status::RetryWaiting;
                storedTask->error =
                    QCoreApplication::translate(
                        "TransferManager",
                        "Temporary failure. Retrying automatically…") +
                    "\n" + errorForUi(error);
                storedTask->nextRetryAtMs =
                    QDateTime::currentMSecsSinceEpoch() + delayMs;
            }
        }
        publishUpdated({task.taskId});
        if (!waitForRetry(task.taskId, delayMs, stopToken)) {
            markStopped();
            publishUpdated({task.taskId});
            finish();
            return;
        }
        const bool canResume = task.type == TransferTask::Type::Upload
                                   ? caps.can_resume_upload
                                   : (task.type == TransferTask::Type::Download
                                          ? caps.can_resume_download
                                          : false);
        if (canResume)
            resume = true;
    }

    markError(QCoreApplication::translate(
                  "TransferManager", "Transfer failed after all retry attempts")
                  .toUtf8()
                  .toStdString());
    publishUpdated({task.taskId});
    finish();
}

QVector<quint64> TransferManager::pruneTerminalHistoryLocked() {
    if (terminalTaskCount_ <= kMaxTerminalHistory)
        return {};

    int toRemove = terminalTaskCount_ - kMaxTerminalHistory;
    QVector<quint64> removed;
    QSet<quint64> removedBatches;
    std::vector<std::unique_ptr<TransferTask>> kept;
    kept.reserve(queueStore_.nodes().size() -
                 static_cast<std::size_t>(toRemove));
    for (auto &taskNode : queueStore_.nodes()) {
        const auto &task = *taskNode;
        if (toRemove > 0 && isTerminalTransferStatus(task.status) &&
            !activeTaskIds_.count(task.taskId)) {
            removed.push_back(task.taskId);
            removedBatches.insert(task.batchId);
            --toRemove;
        } else {
            kept.push_back(std::move(taskNode));
        }
    }
    queueStore_.nodes().swap(kept);
    rebuildTaskLookupLocked();
    for (quint64 batchId : removedBatches)
        forgetBatchPolicyIfUnusedLocked(batchId);
    return removed;
}

void TransferManager::finishWorkerTask(quint64 taskId, qint64 precheckMs,
                                       qint64 transferStartedMs) {
    Status finalStatus = Status::Error;
    quint64 bytesDone = 0;
    qint64 queueLatency = 0;
    QVector<quint64> removed;
    QVector<quint64> dependencySkipped;
    bool requeued = false;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        TransferTask *storedTask = taskForIdLocked(taskId);
        if (storedTask) {
            auto &task = *storedTask;
            finalStatus = task.status;
            bytesDone = task.bytesDone;
            if (task.queuedAtMs > 0 && task.startedAtMs >= task.queuedAtMs) {
                queueLatency = task.startedAtMs - task.queuedAtMs;
            }
            if (resumeRequestedTasks_.erase(taskId) &&
                task.status == Status::Paused) {
                pausedTasks_.erase(taskId);
                transitionToQueued(task, QDateTime::currentMSecsSinceEpoch(),
                                   true);
                requeued = true;
            }
        }
        activeTaskIds_.erase(taskId);
        releaseDestinationLocked(taskId);
        if (finalStatus == Status::Error || finalStatus == Status::Canceled ||
            finalStatus == Status::Warning) {
            QSet<quint64> failedPrerequisites{taskId};
            bool foundDependent = true;
            const qint64 now = QDateTime::currentMSecsSinceEpoch();
            while (foundDependent) {
                foundDependent = false;
                for (auto &candidateNode : queueStore_.nodes()) {
                    auto &candidate = *candidateNode;
                    if (isTerminalTransferStatus(candidate.status) ||
                        !failedPrerequisites.contains(
                            candidate.dependsOnTaskId)) {
                        continue;
                    }
                    skipForFailedDependencyLocked(candidate, now);
                    dependencySkipped.push_back(candidate.taskId);
                    failedPrerequisites.insert(candidate.taskId);
                    foundDependent = true;
                }
            }
        }
        int running = running_.load();
        while (running > 0 &&
               !running_.compare_exchange_weak(running, running - 1)) {
        }
        removed = pruneTerminalHistoryLocked();
    }
    idleCv_.notify_all();
    workCv_.notify_all();
    if (requeued)
        publishUpdated({taskId});
    if (!dependencySkipped.isEmpty())
        publishUpdated(dependencySkipped);
    if (!removed.isEmpty())
        publishRemoved(removed);

    const qint64 transferMs =
        transferStartedMs > 0
            ? QDateTime::currentMSecsSinceEpoch() - transferStartedMs
            : 0;
    recordCompletionMetrics(taskId, finalStatus, bytesDone, queueLatency,
                            precheckMs, transferMs);
}

void TransferManager::recordCompletionMetrics(quint64 taskId, Status status,
                                              quint64 bytesDone,
                                              qint64 queueLatencyMs,
                                              qint64 precheckMs,
                                              qint64 transferMs) {
    queueLatencyMs = std::max<qint64>(0, queueLatencyMs);
    precheckMs = std::max<qint64>(0, precheckMs);
    transferMs = std::max<qint64>(0, transferMs);
    qCInfo(ocXfer) << "task metrics"
                   << "taskId=" << taskId << "status=" << statusName(status)
                   << "queueLatencyMs=" << queueLatencyMs
                   << "precheckMs=" << precheckMs << "transferMs=" << transferMs
                   << "bytesDone=" << bytesDone;

    const qint64 now = QDateTime::currentMSecsSinceEpoch();
    std::lock_guard<std::mutex> lock(perfMtx_);
    ++perfCompletedTasks_;
    perfCompletedBytes_ += bytesDone;
    perfTotalQueueLatencyMs_ += queueLatencyMs;
    perfTotalPrecheckMs_ += precheckMs;
    perfTotalTransferMs_ += transferMs;
    if (now - perfLastLogAtMs_ < 10000 && perfCompletedTasks_ % 10 != 0) {
        return;
    }
    perfLastLogAtMs_ = now;
    qCInfo(ocXfer) << "queue metrics aggregate"
                   << "completedTasks=" << perfCompletedTasks_
                   << "runningCounter=" << running_.load();
}

// Notifications and persistence

void TransferManager::publishAdded(const QVector<quint64> &ids) {
    if (ids.isEmpty())
        return;
    emit tasksAdded(ids);
    schedulePersistence();
}

void TransferManager::publishUpdated(const QVector<quint64> &ids) {
    if (ids.isEmpty())
        return;
    emit tasksUpdated(ids);
    schedulePersistence();
}

void TransferManager::publishRemoved(const QVector<quint64> &ids) {
    if (ids.isEmpty())
        return;
    emit tasksRemoved(ids);
    schedulePersistence();
}

void TransferManager::schedulePersistence() {
    {
        std::lock_guard<std::mutex> lock(persistenceMutex_);
        if (!persistenceEnabled_ || persistenceBlocked_ || !persistenceTimer_) {
            return;
        }
    }
    if (QThread::currentThread() == thread()) {
        persistenceTimer_->start();
        return;
    }
    QMetaObject::invokeMethod(
        this,
        [this] {
            std::lock_guard<std::mutex> lock(persistenceMutex_);
            if (persistenceEnabled_ && !persistenceBlocked_ &&
                persistenceTimer_) {
                persistenceTimer_->start();
            }
        },
        Qt::QueuedConnection);
}

bool TransferManager::enablePersistence(const QString &path) {
    QString resolved = path;
    if (resolved.isEmpty()) {
        const QString dataDir =
            QStandardPaths::writableLocation(QStandardPaths::AppDataLocation);
        if (dataDir.isEmpty()) {
            emit persistenceWarning(
                tr("The transfer queue storage directory is unavailable."));
            return false;
        }
        resolved =
            QDir(dataDir).filePath(QStringLiteral("transfer-queue-v1.json"));
    }
    {
        std::lock_guard<std::mutex> lock(persistenceMutex_);
        persistencePath_ = QDir::cleanPath(resolved);
        persistenceEnabled_ = true;
        persistenceBlocked_ = false;
    }

    QString warning;
    const bool restored = restorePersistenceFile(warning);
    if (!warning.isEmpty())
        emit persistenceWarning(warning);
    return restored;
}

bool TransferManager::restorePersistenceFile(QString &warning) {
    QString path;
    {
        std::lock_guard<std::mutex> lock(persistenceMutex_);
        path = persistencePath_;
    }
    auto result = TransferQueuePersistence::load(path, sessionIdentity());
    if (result.shouldBlockWrites()) {
        std::lock_guard<std::mutex> lock(persistenceMutex_);
        persistenceBlocked_ = true;
    }
    warning = result.warning;
    if (!result.succeeded())
        return false;

    QVector<quint64> ids;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        if (!queueStore_.nodes().empty()) {
            warning = tr("The saved transfer queue was not restored because "
                         "the current queue is not empty.");
            return false;
        }
        queueStore_.nodes().reserve(
            static_cast<std::size_t>(result.tasks.size()));
        for (auto &task : result.tasks)
            appendTaskLocked(std::move(task));
        for (const auto &taskNode : queueStore_.nodes()) {
            const TransferTask &task = *taskNode;
            ids.push_back(task.taskId);
            nextId_ = std::max(nextId_, task.taskId + 1);
            nextBatchId_ = std::max(nextBatchId_, task.batchId + 1);
            conflictCoordinator_.ensureBatchPolicy(task.batchId,
                                                   task.conflictPolicy);
            pausedTasks_.insert(task.taskId);
        }
        rebuildTaskLookupLocked();
    }
    if (!ids.isEmpty())
        publishAdded(ids);
    return true;
}

bool TransferManager::writePersistenceFile(QString &warning) {
    QString path;
    {
        std::lock_guard<std::mutex> lock(persistenceMutex_);
        if (!persistenceEnabled_ || persistenceBlocked_)
            return true;
        path = persistencePath_;
    }

    QVector<TransferTask> snapshot;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        snapshot.reserve(static_cast<qsizetype>(queueStore_.nodes().size()));
        for (const auto &taskNode : queueStore_.nodes())
            snapshot.push_back(*taskNode);
    }
    const auto result = TransferQueuePersistence::save(path, snapshot);
    warning = result.warning;
    return result.succeeded;
}

void TransferManager::persistNow() {
    QString warning;
    if (!writePersistenceFile(warning) && !warning.isEmpty())
        emit persistenceWarning(warning);
}
