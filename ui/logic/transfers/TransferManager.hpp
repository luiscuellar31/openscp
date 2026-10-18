// Persistent, concurrent transfer queue manager.
#pragma once

#include "logic/transfers/BandwidthLimiter.hpp"
#include "logic/transfers/TransferAutoClear.hpp"
#include "logic/transfers/TransferExecutor.hpp"
#include "logic/transfers/TransferQueue.hpp"
#include "logic/transfers/TransferQueuePersistence.hpp"
#include "logic/transfers/TransferQueueWriter.hpp"
#include "logic/transfers/TransferTaskControls.hpp"
#include "logic/transfers/TransferTypes.hpp"
#include "openscp/Protocol.hpp"
#include "openscp/RemoteError.hpp"

#include <QObject>
#include <QPair>
#include <QSet>
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
    // retain the connections they open. The token is requested when the
    // session is cleared or replaced, or the manager shuts down, so a
    // connection still being opened gives up instead of making them wait.
    using ConnectionFactory =
        std::function<std::unique_ptr<openscp::RemoteClient>(std::stop_token,
                                                             std::string &)>;

    // `uploadsUsePartialFiles` says whether the session's uploads go through
    // "<destination>.part", which removing tasks with partial data cleans up.
    void setConnectionFactory(ConnectionFactory openConnection,
                              bool uploadsUsePartialFiles = true);
    // Pauses the session's active work until another session is set, waiting
    // for workers to stop, and closes their connections.
    void clearSession();
    // Signals worker threads to stop, wakes condition variables, and waits
    // for workers to join. Safe to call multiple times or during teardown.
    void shutdown();
    bool isShuttingDown() const { return shuttingDown_.load(); }
    void setSessionIdentity(const QString &sessionKey);
    QString sessionIdentity() const;
    // The address of the current session's server, from downloadOriginUrl(),
    // which Windows records on each file downloaded from it (the Mark of the
    // Web).
    void setDownloadOrigin(const QString &originUrl);

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
    // Removes every inactive task among taskIds in one pass over the queue
    // and reports them in a single tasksRemoved signal.
    void removeTasks(const QVector<quint64> &taskIds,
                     bool removePartialData = false);

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
    // Adds the requests under one lock, with one tasksAdded and one wake-up
    // of the workers, and returns their task IDs in order. Requests with a
    // zero batchId share one new batch.
    QVector<quint64> enqueueTasks(const QVector<TransferRequest> &requests);
    quint64 createBatch(const TransferBatchOptions &options = {});
    void cancelBatch(quint64 batchId);

    QVector<TransferTask> tasksSnapshot() const;
    QVector<TransferTask> tasksSnapshot(const QVector<quint64> &taskIds) const;
    std::optional<TransferTask> taskSnapshot(quint64 taskId) const;
    // Finds exact non-terminal work for the current (or unscoped) session
    // without copying the queue snapshot.
    [[nodiscard]] std::optional<quint64>
    activeTaskIdForPaths(TransferTask::Type type, const QString &source,
                         const QString &destination) const;
    [[nodiscard]] QVector<quint64>
    activeTaskIdsForSession(const QString &sessionKey) const;

    void pauseAll();
    void resumeAll();
    void retryFailed();
    void retryTask(quint64 taskId);
    void clearCompleted();
    void clearFailedCanceled();
    void clearFinishedOlderThan(int minutes, bool clearDone,
                                bool clearFailedCanceled);
    // Applies `rule` now and then every minute, so finished tasks leave the
    // queue whether or not its window is open.
    void setAutoClear(const TransferAutoClearRule &rule);

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
    // No worker could open a connection, so dispatch stopped and pending work
    // waits until it is resumed or the session changes.
    void transfersWaitingForConnection(const QString &error);

    public slots:
    void schedule();
    void persistNow();

    private:
    static constexpr int kWorkerSlots = 8;
    // Consecutive failed connections, with no worker connected, after which
    // the queue stops dispatching instead of failing every task in turn.
    static constexpr int kConnectionFailureLimit = 3;
    // Completed, failed and canceled tasks each keep their newest
    // kMaxTerminalHistory entries; pending cleanups and uncertain commits are
    // never pruned. Pruning runs once kTerminalHistoryPruneBatch more tasks
    // have finished, so it walks the queue once per batch, not per task.
    static constexpr int kMaxTerminalHistory = 5000;
    static constexpr int kTerminalHistoryPruneBatch = kMaxTerminalHistory / 10;
    // A save waits for a pause in queue changes, but never longer than the
    // deadline, so a busy queue is still saved regularly.
    static constexpr int kPersistenceDelayMs = 250;
    static constexpr int kPersistenceDeadlineMs = 2000;
    static constexpr int kPersistenceRetryMs = 5000;

    struct WorkerSlot;
    enum class PrecheckOutcome { Continue, Skipped, Canceled, Error };

    ConnectionFactory openConnection_;
    // Stops the connections being opened for the current session; replaced
    // with each new one. Read and written under mtx_.
    std::stop_source connectionStop_;
    QString currentSessionKey_;
    // The current session's server, recorded on Windows as the source of
    // each downloaded file. Read and written under mtx_.
    QString downloadOrigin_;
    // Sessions of this run whose uploads write the destination directly, so
    // a "<destination>.part" on their server is not an upload's.
    QSet<QString> sessionsWithoutUploadPartials_;
    quint64 sessionGeneration_ = 1;

    TransferQueueStore queueStore_;
    struct BatchWorkState {
        std::size_t unfinished = 0;
        std::size_t failed = 0;
    };
    std::unordered_map<quint64, BatchWorkState> batchWorkById_;
    // Relationships are fixed at insertion and protected by mtx_. Keep
    // terminal tasks indexed until removal so retries need no reindexing.
    std::unordered_map<quint64, std::unordered_set<quint64>>
        dependentsByTaskId_;
    std::unordered_map<quint64, std::unordered_set<quint64>> batchWaitersById_;
    // Includes upload sources and local destinations (including .part).
    // Only transfer-phase tasks are indexed. Phase changes and renames update
    // membership; status and active ownership are checked at lookup.
    std::unordered_map<std::string, std::unordered_set<quint64>>
        localPathUsers_;
    quint64 nextId_ = 1;
    quint64 nextBatchId_ = 1;
    int terminalTaskCount_ = 0;
    int consecutiveConnectionFailures_ = 0;
    bool connectionSuspended_ = false;
    int terminalHistoryPruneAt_ =
        kMaxTerminalHistory + kTerminalHistoryPruneBatch;
    TransferScheduler scheduler_;

    std::atomic<bool> paused_{false};
    std::atomic<bool> shuttingDown_{false};
    std::atomic<int> running_{0};
    std::atomic<int> maxConcurrent_{2};
    // A worker without a task for this long closes its connection, so idle
    // slots, and slots above a lowered limit, do not hold server connections.
    // Read and written under mtx_.
    std::chrono::milliseconds idleConnectionTimeout_{std::chrono::seconds(60)};

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
    std::unordered_set<std::string> reservedPaths_;
    std::unordered_map<quint64, std::vector<std::string>> reservationByTask_;
    ConflictCoordinator conflictCoordinator_;

    BandwidthLimiter bandwidthLimiter_;

    QTimer *autoClearTimer_ = nullptr;
    TransferAutoClearRule autoClearRule_;

    QTimer *persistenceTimer_ = nullptr;
    TransferQueueWriter persistenceWriter_;
    QString persistencePath_;
    qint64 persistencePendingSinceMs_ = 0;
    // Fingerprint of the last snapshot handed to the writer; guarded by mtx_.
    std::optional<std::size_t> lastPersistedFingerprint_;
    bool persistenceEnabled_ = false;
    bool persistenceBlocked_ = false;
    mutable std::mutex persistenceMutex_;

    mutable std::mutex perfMtx_;
    quint64 perfCompletedTasks_ = 0;
    qint64 perfLastLogAtMs_ = 0;

    TransferTask *taskForIdLocked(quint64 taskId);
    const TransferTask *taskForIdLocked(quint64 taskId) const;
    void appendTaskLocked(TransferTask task);
    quint64 enqueueRequest(TransferRequest request);
    void rebuildTaskLookupLocked();
    // Removes the inactive tasks shouldRemove selects in one pass, releases
    // what the queue kept for them, and returns them.
    TransferQueueStore::Nodes removeInactiveTasksLocked(
        const std::function<bool(const TransferTask &)> &shouldRemove);
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
    void adjustDependencyIndexesLocked(const TransferTask &task, bool add);
    void adjustBatchWorkLocked(const TransferTask &task, bool add);
    void setTaskStatusLocked(
        TransferTask &task, TransferTask::Status status,
        std::optional<bool> skippedByFailedDependency = std::nullopt);
    // State of the batch's tasks that do not wait for the batch.
    BatchWork batchWorkLocked(quint64 batchId) const;
    std::string destinationKey(const TransferTask &task,
                               bool partial = false) const;
    std::vector<std::string> destinationKeys(const TransferTask &task) const;
    std::string localUploadSourceKey(const TransferTask &task) const;
    std::vector<std::string> localPathUserKeys(const TransferTask &task) const;
    void adjustLocalPathUsersLocked(const TransferTask &task, bool add);
    void setTaskPhaseLocked(TransferTask &task, TransferPhase phase);
    bool hasOtherLocalSourceUserLocked(const TransferTask &task) const;
    bool hasOtherLocalPathUserLocked(const std::string &key,
                                     quint64 taskId) const;
    bool canReserveTaskLocked(const TransferTask &task) const;
    bool reserveTaskPathsLocked(const TransferTask &task);
    void reserveCleanupSourceLocked(const TransferTask &task);
    void releaseTaskPathsLocked(quint64 taskId);
    bool dependencySatisfiedLocked(const TransferTask &task) const;
    std::optional<TransferTask> pickRunnableTaskLocked(std::size_t slotIndex);
    void workerLoop(std::size_t slotIndex, std::stop_token stopToken);
    // connectionFailed is set when no connection could be opened, as opposed
    // to the task or session stopping.
    std::shared_ptr<openscp::RemoteClient>
    workerClient(WorkerSlot &slot, quint64 taskId, quint64 generation,
                 std::string &err, bool &connectionFailed);
    void invalidateWorkerClient(WorkerSlot &slot);
    void handleWorkerConnectionFailure(WorkerSlot &slot, quint64 taskId,
                                       quint64 generation,
                                       const std::string &error);
    void resetConnectionFailuresLocked();
    // Requeues the current session's work that waited for a connection.
    void wakeCurrentSessionLocked(QVector<quint64> &changed);
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
    bool retryLocked(TransferTask &task, qint64 nowMs);
    void retryDependentsLocked(QVector<quint64> &retried, qint64 nowMs);
    void applyAutoClear();
    void removeInactiveTasks(
        const std::function<bool(const TransferTask &)> &shouldRemove);
    // Tasks in justCanceled count toward their limit but stay this time, so a
    // cancel does not remove what it just canceled.
    QVector<quint64>
    pruneTerminalHistoryLocked(const QVector<quint64> &justCanceled = {});
    void recordCompletionMetrics(quint64 taskId, TransferTask::Status status,
                                 quint64 bytesDone, qint64 queueLatencyMs,
                                 qint64 precheckMs, qint64 transferMs);

    void publishAdded(const QVector<quint64> &ids);
    void publishUpdated(const QVector<quint64> &ids);
    // Progress is not part of what the queue saves, so reporting it must not
    // schedule a save.
    void publishProgress(const QVector<quint64> &ids);
    void publishRemoved(const QVector<quint64> &ids);
    void schedulePersistence();
    // Both run on the manager's thread.
    void restartPersistenceTimer();
    void writeQueueSnapshot();
    bool restorePersistenceFile(QString &warning);
    // The tasks the queue would save, or nullopt when it saves nothing.
    // The tasks to save. With `onlyIfChanged`, nothing when what would be
    // saved matches the last snapshot taken this way: most updates only
    // change a task's status or progress, which the file does not keep.
    std::optional<QVector<TransferTask>>
    persistedSnapshot(bool onlyIfChanged = false);

    friend struct TransferManagerTestAccess;
};
