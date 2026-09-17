// Persistent, concurrent transfer queue manager.
#pragma once

#include "logic/transfers/BandwidthLimiter.hpp"
#include "logic/transfers/TransferExecutor.hpp"
#include "logic/transfers/TransferQueue.hpp"
#include "logic/transfers/TransferQueuePersistence.hpp"
#include "logic/transfers/TransferTaskControls.hpp"
#include "logic/transfers/TransferTypes.hpp"
#include "openscp/Protocol.hpp"
#include "openscp/RemoteError.hpp"

#include <QObject>
#include <QPair>
#include <QString>
#include <QVector>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <stop_token>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class QTimer;
struct TransferManagerTestAccess;

namespace openscp {
class RemoteClient;
}

class TransferManager : public QObject {
    Q_OBJECT

    public:
    explicit TransferManager(QObject *parent = nullptr);
    ~TransferManager() override;

    // Opens a connection to the current session, or returns null with an
    // error. Worker slots call it concurrently and without locks held, and
    // retain the connections they open.
    using ConnectionFactory =
        std::function<std::unique_ptr<openscp::RemoteClient>(std::string &)>;

    void setConnectionFactory(ConnectionFactory openConnection);
    // Pauses the session's active work until another session is set, waiting
    // for workers to stop, and closes their connections.
    void clearSession();
    void setSessionIdentity(const QString &sessionKey);
    QString sessionIdentity() const;

    void setMaxConcurrent(int maxConcurrent);
    int maxConcurrent() const { return maxConcurrent_.load(); }
    void setGlobalSpeedLimitKBps(int kbps);
    int globalSpeedLimitKBps() const { return bandwidthLimiter_.limitKBps(); }
    bool isQueuePaused() const { return paused_.load(); }

    void pauseTask(quint64 taskId);
    void resumeTask(quint64 taskId);
    void cancelTask(quint64 taskId);
    void cancelAll();
    void setTaskSpeedLimit(quint64 taskId, int kbps);
    void removeTask(quint64 taskId, bool removePartialData = false);

    // A zero batchId is replaced with a stable generated ID.
    quint64 enqueueUpload(const QString &local, const QString &remote,
                          const TransferBatchOptions &options = {});
    quint64 enqueueDownload(const QString &remote, const QString &local,
                            const TransferBatchOptions &options = {});
    quint64 enqueueLocalDirectory(const QString &localDirectory,
                                  const TransferBatchOptions &options = {});
    quint64 enqueueRemoteDirectory(const QString &remoteDirectory,
                                   const TransferBatchOptions &options = {});
    quint64 enqueueLocalDelete(const QString &localPath, bool directory,
                               const TransferBatchOptions &options = {});
    quint64 enqueueRemoteDelete(const QString &remotePath, bool directory,
                                const TransferBatchOptions &options = {});
    int
    enqueueDownloads(const QVector<QPair<QString, QString>> &remoteLocalPairs,
                     const TransferBatchOptions &options = {});
    quint64 createBatch(const TransferBatchOptions &options = {});
    void cancelBatch(quint64 batchId);

    QVector<TransferTask> tasksSnapshot() const;
    QVector<TransferTask> tasksSnapshot(const QVector<quint64> &taskIds) const;
    std::optional<TransferTask> taskSnapshot(quint64 taskId) const;
    [[nodiscard]] bool hasActiveTaskForSource(TransferTask::Type type,
                                              const QString &source) const;
    [[nodiscard]] bool
    hasActiveTaskForDestination(TransferTask::Type type,
                                const QString &destination) const;
    // Finds exact non-terminal work for the current (or unscoped) session
    // without copying the queue snapshot.
    [[nodiscard]] std::optional<quint64>
    activeTaskIdForPaths(TransferTask::Type type, const QString &source,
                         const QString &destination) const;
    [[nodiscard]] QVector<quint64>
    activeTaskIdsForSession(const QString &sessionKey) const;
    [[nodiscard]] bool isBatchTerminal(quint64 batchId) const;

    void pauseAll();
    void resumeAll();
    void retryFailed();
    void retryTask(quint64 taskId);
    void clearCompleted();
    void clearFailedCanceled();
    void clearFinishedOlderThan(int minutes, bool clearDone,
                                bool clearFailedCanceled);

    // Queue persistence is explicit so tests and embedders do not unexpectedly
    // touch user data. The application should call this once during startup.
    bool enablePersistence(const QString &path = {});

    signals:
    // Granular signals are the preferred hot-path interface.
    void tasksAdded(const QVector<quint64> &taskIds);
    void tasksUpdated(const QVector<quint64> &taskIds);
    void tasksRemoved(const QVector<quint64> &taskIds);
    void queueSettingsChanged();
    void persistenceWarning(const QString &message);

    public slots:
    void schedule();
    void persistNow();

    private:
    static constexpr int kWorkerSlots = 8;
    static constexpr int kMaxTerminalHistory = 5000;

    struct WorkerSlot;
    enum class PrecheckOutcome { Continue, Skipped, Canceled, Error };

    ConnectionFactory openConnection_;
    QString currentSessionKey_;
    quint64 sessionGeneration_ = 1;

    TransferQueueStore queueStore_;
    quint64 nextId_ = 1;
    quint64 nextBatchId_ = 1;
    int terminalTaskCount_ = 0;
    TransferScheduler scheduler_;

    std::atomic<bool> paused_{false};
    std::atomic<bool> shuttingDown_{false};
    std::atomic<int> running_{0};
    std::atomic<int> maxConcurrent_{2};

    // Worker-slot client mutexes and retry, persistence, and performance
    // mutexes are independent of mtx_ and must be released before acquiring
    // it. External client calls, connection handshakes, and Qt signal
    // emissions happen without these locks held.
    mutable std::mutex mtx_;
    std::condition_variable workCv_;
    std::condition_variable idleCv_;
    std::mutex retryMutex_;
    std::condition_variable retryCv_;
    std::vector<std::unique_ptr<WorkerSlot>> workerSlots_;

    // Its running tasks are exactly activeTaskIds_.
    TransferTaskControls taskControls_;
    std::unordered_set<quint64> activeTaskIds_;
    std::unordered_set<quint64> resumeRequestedTasks_;
    std::unordered_set<std::string> reservedDestinations_;
    std::unordered_map<quint64, std::string> reservationByTask_;
    ConflictCoordinator conflictCoordinator_;

    BandwidthLimiter bandwidthLimiter_;

    QTimer *persistenceTimer_ = nullptr;
    QString persistencePath_;
    bool persistenceEnabled_ = false;
    bool persistenceBlocked_ = false;
    mutable std::mutex persistenceMutex_;

    mutable std::mutex perfMtx_;
    quint64 perfCompletedTasks_ = 0;
    qint64 perfLastLogAtMs_ = 0;

    TransferTask *taskForIdLocked(quint64 taskId);
    const TransferTask *taskForIdLocked(quint64 taskId) const;
    void appendTaskLocked(TransferTask task);
    quint64 enqueuePreparedTask(TransferTask task,
                                const TransferBatchOptions &options,
                                bool inheritBatchConflictPolicy);
    void rebuildTaskLookupLocked();
    void forgetBatchPolicyIfUnusedLocked(quint64 batchId);
    quint64 normalizedBatchIdLocked(quint64 requested);
    void initializeConnectionStatusLocked(TransferTask &task) const;
    bool dependencyFailedLocked(const TransferTask &task) const;
    void skipForFailedDependencyLocked(TransferTask &task, qint64 now);
    // Skips the queued work that depends, directly or through other skipped
    // tasks, on a task that did not complete successfully, including the
    // tasks waiting for that task's batch.
    QVector<quint64> skipDependentsOfFailedLocked(quint64 failedTaskId,
                                                  qint64 now);
    enum class BatchWork { Unfinished, Failed, Succeeded };
    // State of the batch's tasks that do not wait for the batch.
    BatchWork batchWorkLocked(quint64 batchId) const;
    quint64 enqueuePathTask(TransferTask::Type type, const QString &path,
                            const TransferBatchOptions &options);
    std::string destinationKey(const TransferTask &task) const;
    bool reserveDestinationLocked(const TransferTask &task);
    void releaseDestinationLocked(quint64 taskId);
    bool dependencySatisfiedLocked(const TransferTask &task) const;
    bool hasRunnableTaskLocked(std::size_t slotIndex);
    std::optional<TransferTask> pickRunnableTaskLocked(std::size_t slotIndex);
    void workerLoop(std::size_t slotIndex, std::stop_token stopToken);
    std::shared_ptr<openscp::RemoteClient> workerClient(WorkerSlot &slot,
                                                        quint64 taskId,
                                                        quint64 generation,
                                                        std::string &err);
    void invalidateWorkerClient(WorkerSlot &slot);
    void interruptTask(quint64 taskId);
    void interruptAllActive();
    bool shouldCancel(quint64 taskId) const;
    bool waitForRetry(quint64 taskId, int delayMs, std::stop_token stopToken);

    void executeTask(WorkerSlot &slot, TransferTask task,
                     std::stop_token stopToken);
    PrecheckOutcome
    precheckTask(TransferTask &task,
                 const std::shared_ptr<openscp::RemoteClient> &workerClient,
                 const openscp::ProtocolCapabilities &caps, bool &resume,
                 std::string &err);
    bool runTransferAttempt(
        TransferTask &task,
        const std::shared_ptr<openscp::RemoteClient> &workerClient,
        const std::shared_ptr<const TransferTaskControls::Signals> &taskSignals,
        bool resume, std::string &err);
    bool
    runPostAction(TransferTask &task,
                  const std::shared_ptr<openscp::RemoteClient> &workerClient,
                  std::string &err);
    bool shouldRetryError(const openscp::RemoteError &structuredError,
                          const std::string &rawError, int &retryAfterMs) const;
    bool isTransportFailure(const openscp::RemoteError &structuredError,
                            const std::string &rawError) const;
    ConflictResolution resolveConflict(TransferTask &task, bool allowResume,
                                       const QString &name,
                                       const QString &sourceInfo,
                                       const QString &destinationInfo,
                                       std::optional<qint64> sourceMtime,
                                       std::optional<qint64> destinationMtime);
    bool chooseRenamedDestination(
        TransferTask &task,
        const std::shared_ptr<openscp::RemoteClient> &workerClient,
        std::string &err);

    void updateProgress(quint64 taskId, std::size_t done, std::size_t total,
                        double measuredKBps, int etaSeconds);
    void finishWorkerTask(quint64 taskId, qint64 precheckMs,
                          qint64 transferStartedMs);
    void transitionToQueued(TransferTask &task, qint64 nowMs, bool resume);
    void transitionToPaused(TransferTask &task);
    void transitionToCanceled(TransferTask &task, qint64 nowMs);
    void transitionToError(TransferTask &task, const std::string &rawError,
                           qint64 nowMs);
    void transitionToDone(TransferTask &task, qint64 nowMs);
    void resetForRetry(TransferTask &task, qint64 nowMs);
    void removeInactiveTasks(
        const std::function<bool(const TransferTask &)> &shouldRemove);
    QVector<quint64> pruneTerminalHistoryLocked();
    void recordCompletionMetrics(quint64 taskId, TransferTask::Status status,
                                 quint64 bytesDone, qint64 queueLatencyMs,
                                 qint64 precheckMs, qint64 transferMs);

    void publishAdded(const QVector<quint64> &ids);
    void publishUpdated(const QVector<quint64> &ids);
    void publishRemoved(const QVector<quint64> &ids);
    void schedulePersistence();
    bool restorePersistenceFile(QString &warning);
    bool writePersistenceFile(QString &warning);

    friend struct TransferManagerTestAccess;
};
