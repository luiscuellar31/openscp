// Transfer queue tests without an external test framework.
#include "QtTestSupport.hpp"
#include "TestHarness.hpp"
#include "common/UniqueFile.hpp"
#include "logic/transfers/BandwidthLimiter.hpp"
#include "logic/transfers/ConflictCoordinator.hpp"
#include "logic/transfers/TransferManager.hpp"
#include "logic/transfers/TransferQueuePersistence.hpp"
#include "logic/transfers/TransferQueueWriter.hpp"
#include "logic/transfers/TransferTaskControls.hpp"
#include "mock/MockSftpClient.hpp"
#include "openscp/SafeLocalFile.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>

struct TransferManagerTestAccess {
    static bool reserve(TransferManager &manager, const TransferTask &task) {
        std::lock_guard<std::mutex> lock(manager.mtx_);
        return manager.reserveTaskPathsLocked(task);
    }

    static void release(TransferManager &manager, quint64 taskId) {
        std::lock_guard<std::mutex> lock(manager.mtx_);
        manager.releaseTaskPathsLocked(taskId);
    }

    static bool rename(TransferManager &manager, TransferTask &task,
                       std::string &error) {
        return manager.chooseRenamedDestination(task, {}, error);
    }

    static const TransferTask *taskAddress(TransferManager &manager,
                                           quint64 taskId) {
        std::lock_guard<std::mutex> lock(manager.mtx_);
        return manager.taskForIdLocked(taskId);
    }

    static std::size_t taskVectorCapacity(TransferManager &manager) {
        std::lock_guard<std::mutex> lock(manager.mtx_);
        return manager.queueStore_.capacity();
    }

    static std::chrono::microseconds blockedBatchScan(TransferManager &manager,
                                                      int repetitions) {
        std::lock_guard<std::mutex> lock(manager.mtx_);
        manager.openConnection_ = [](std::string &) {
            return std::unique_ptr<openscp::RemoteClient>{};
        };
        const auto start = std::chrono::steady_clock::now();
        for (int index = 0; index < repetitions; ++index)
            (void)manager.pickRunnableTaskLocked(0);
        const auto duration =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start);
        manager.openConnection_ = {};
        return duration;
    }

    static std::chrono::microseconds
    runnableTailSelection(TransferManager &manager, int repetitions) {
        std::lock_guard<std::mutex> lock(manager.mtx_);
        for (std::size_t index = 0;
             index + 1 < manager.queueStore_.nodes().size(); ++index) {
            manager.setTaskStatusLocked(*manager.queueStore_.nodes()[index],
                                        TransferTask::Status::Paused);
        }
        manager.openConnection_ = [](std::string &) {
            return std::unique_ptr<openscp::RemoteClient>{};
        };
        const auto start = std::chrono::steady_clock::now();
        for (int index = 0; index < repetitions; ++index) {
            const auto selected = manager.pickRunnableTaskLocked(0);
            if (!selected)
                break;
            manager.releaseTaskPathsLocked(selected->taskId);
            manager.activeTaskIds_.erase(selected->taskId);
            manager.taskControls_.finishRunning(selected->taskId);
            manager.running_.fetch_sub(1);
            auto *stored = manager.taskForIdLocked(selected->taskId);
            manager.setTaskStatusLocked(*stored, TransferTask::Status::Queued);
        }
        const auto duration =
            std::chrono::duration_cast<std::chrono::microseconds>(
                std::chrono::steady_clock::now() - start);
        manager.openConnection_ = {};
        return duration;
    }
};

namespace {

using namespace std::chrono_literals;
using openscp::testsupport::waitUntil;

openscp::SessionOptions testOptions() {
    openscp::SessionOptions options;
    options.host = "parallel.test";
    options.username = "tester";
    return options;
}

TransferBatchOptions testBatchOptions() {
    TransferBatchOptions options;
    options.sessionKey = QStringLiteral("test-session");
    options.conflictPolicy = TransferConflictPolicy::Overwrite;
    return options;
}

bool waitForStatus(
    const TransferManager &manager, quint64 taskId, TransferTask::Status status,
    std::chrono::milliseconds timeout = std::chrono::milliseconds(5000)) {
    return waitUntil(
        [&] {
            const auto task = manager.taskSnapshot(taskId);
            return task && task->status == status;
        },
        timeout);
}

template <typename Client, typename... Args>
std::unique_ptr<openscp::RemoteClient>
makeConnectedWorker(const openscp::SessionOptions &options, std::string &error,
                    Args &&...args) {
    auto worker = std::make_unique<Client>(std::forward<Args>(args)...);
    if (!worker->connect(options, error))
        return nullptr;
    return worker;
}

OPENSCP_TEST(testConflictPoliciesAndUnsupportedFallback, test) {
    ConflictCoordinator coordinator;
    std::atomic<int> prompts{0};

    auto resolvePreset = [&](std::uint64_t batchId,
                             TransferConflictPolicy policy,
                             ConflictRequest request) {
        request.batchId = batchId;
        coordinator.setBatchPolicy(batchId, policy);
        return coordinator.resolve(
            request, TransferConflictPolicy::Ask, [&](const ConflictRequest &) {
                prompts.fetch_add(1);
                return ConflictResolution{TransferConflictPolicy::Skip};
            });
    };

    ConflictRequest supported;
    supported.allowResume = true;
    supported.sourceMtime = 105;
    supported.destinationMtime = 100;
    test.check(
        resolvePreset(1, TransferConflictPolicy::Overwrite, supported).policy ==
            TransferConflictPolicy::Overwrite,
        "overwrite policy should remain explicit");
    test.check(
        resolvePreset(2, TransferConflictPolicy::Skip, supported).policy ==
            TransferConflictPolicy::Skip,
        "skip policy should remain explicit");
    test.check(
        resolvePreset(3, TransferConflictPolicy::Rename, supported).policy ==
            TransferConflictPolicy::Rename,
        "rename policy should remain explicit");
    test.check(
        resolvePreset(4, TransferConflictPolicy::Resume, supported).policy ==
            TransferConflictPolicy::Resume,
        "resume policy should be used when the backend supports it");
    test.check(
        resolvePreset(5, TransferConflictPolicy::NewerOnly, supported).policy ==
            TransferConflictPolicy::Overwrite,
        "newer-only should copy a source newer by more than two seconds");
    supported.sourceMtime = 102;
    test.check(
        resolvePreset(6, TransferConflictPolicy::NewerOnly, supported).policy ==
            TransferConflictPolicy::Skip,
        "newer-only should respect the two-second timestamp tolerance");
    test.check(prompts.load() == 0,
               "supported preset policies should not invoke a resolver");

    ConflictRequest noResume;
    noResume.batchId = 7;
    coordinator.setBatchPolicy(noResume.batchId,
                               TransferConflictPolicy::Resume);
    const auto resumeFallback = coordinator.resolve(
        noResume, TransferConflictPolicy::Ask, [&](const ConflictRequest &) {
            prompts.fetch_add(1);
            return ConflictResolution{TransferConflictPolicy::Rename};
        });
    test.check(resumeFallback.policy == TransferConflictPolicy::Rename &&
                   prompts.load() == 1,
               "unsupported resume must fall back to Ask, never overwrite");
    test.check(coordinator.batchPolicy(noResume.batchId) ==
                   TransferConflictPolicy::Ask,
               "an unsupported resume batch policy should be reset to Ask");

    ConflictRequest unknownMetadata;
    unknownMetadata.batchId = 8;
    coordinator.setBatchPolicy(unknownMetadata.batchId,
                               TransferConflictPolicy::NewerOnly);
    const auto newerFallback = coordinator.resolve(
        unknownMetadata, TransferConflictPolicy::Ask,
        [&](const ConflictRequest &) {
            prompts.fetch_add(1);
            return ConflictResolution{TransferConflictPolicy::Skip};
        });
    test.check(newerFallback.policy == TransferConflictPolicy::Skip &&
                   prompts.load() == 2,
               "newer-only without metadata must ask instead of overwriting");
    test.check(coordinator.batchPolicy(unknownMetadata.batchId) ==
                   TransferConflictPolicy::Ask,
               "unsupported newer-only policy should remain Ask");
}

OPENSCP_TEST(testConcurrentConflictsUseOneBatchResolution, test) {
    ConflictCoordinator coordinator;
    constexpr std::uint64_t batchId = 44;
    coordinator.setBatchPolicy(batchId, TransferConflictPolicy::Ask);
    std::atomic<int> resolverCalls{0};
    std::atomic<bool> start{false};
    std::array<ConflictResolution, 4> results;
    std::vector<std::thread> workers;
    workers.reserve(results.size());
    for (std::size_t index = 0; index < results.size(); ++index) {
        workers.emplace_back([&, index] {
            while (!start.load())
                std::this_thread::yield();
            ConflictRequest request;
            request.batchId = batchId;
            request.allowResume = true;
            request.sourceMtime = 200;
            request.destinationMtime = 100;
            results[index] = coordinator.resolve(
                request, TransferConflictPolicy::Ask,
                [&](const ConflictRequest &) {
                    resolverCalls.fetch_add(1);
                    std::this_thread::sleep_for(40ms);
                    return ConflictResolution{TransferConflictPolicy::Skip,
                                              true, false};
                });
        });
    }
    start.store(true);
    for (auto &worker : workers)
        worker.join();

    test.check(resolverCalls.load() == 1,
               "concurrent conflicts in one batch should prompt once");
    test.check(
        std::all_of(results.cbegin(), results.cend(),
                    [](const ConflictResolution &result) {
                        return !result.canceled &&
                               result.policy == TransferConflictPolicy::Skip;
                    }),
        "the one batch decision should resolve every concurrent conflict");
    test.check(coordinator.batchPolicy(batchId) == TransferConflictPolicy::Skip,
               "apply-to-remaining should persist the selected batch policy");
}

OPENSCP_TEST(testBatchDownloadEnqueueAndGranularSignals, test) {
    TransferManager manager;
    int addedNotifications = 0;
    qsizetype addedIds = 0;
    QObject::connect(&manager, &TransferManager::tasksAdded, &manager,
                     [&](const QVector<quint64> &ids) {
                         ++addedNotifications;
                         addedIds += ids.size();
                     });

    QVector<QPair<QString, QString>> downloads;
    downloads.reserve(10'000);
    for (int index = 0; index < 10'000; ++index) {
        downloads.push_back({QStringLiteral("/remote/file-%1.dat").arg(index),
                             QStringLiteral("/local/file-%1.dat").arg(index)});
    }

    const int added = manager.enqueueDownloads(downloads);
    test.check(added == downloads.size(),
               "batch enqueue should report every added download");
    test.check(addedNotifications == 1 && addedIds == downloads.size(),
               "batch enqueue should publish one granular range");

    const QVector<TransferTask> snapshot = manager.tasksSnapshot();
    test.check(snapshot.size() == downloads.size(),
               "batch enqueue should preserve every download");
    test.check(snapshot.front().taskId == 1 && snapshot.back().taskId == 10'000,
               "batch tasks should receive stable sequential IDs");

    const auto selected = manager.tasksSnapshot({1, 5000, 10'000, 100'000});
    test.check(selected.size() == 3,
               "indexed snapshots should return only existing task IDs");
    test.check(selected[1].src == QStringLiteral("/remote/file-4999.dat"),
               "indexed snapshots should preserve requested ID order");

    const quint64 batchId = snapshot.front().batchId;
    const auto exactTaskId = manager.activeTaskIdForPaths(
        TransferTask::Type::Download, QStringLiteral("/remote/file-4999.dat"),
        QStringLiteral("/local/file-4999.dat"));
    test.check(
        exactTaskId == std::optional<quint64>{5000} &&
            !manager
                 .activeTaskIdForPaths(TransferTask::Type::Upload,
                                       QStringLiteral("/remote/file-4999.dat"),
                                       QStringLiteral("/local/file-4999.dat"))
                 .has_value() &&
            !manager
                 .activeTaskIdForPaths(TransferTask::Type::Download,
                                       QStringLiteral("/remote/file-4999.dat"),
                                       QStringLiteral("/local/other.dat"))
                 .has_value(),
        "exact path queries should match both paths and task type");
    const QVector<quint64> activeIds =
        manager.activeTaskIdsForSession(QStringLiteral("site-a"));
    test.check(activeIds.size() == downloads.size() && activeIds.front() == 1 &&
                   activeIds.back() == 10'000,
               "session queries should return only IDs without copying a "
               "large queue");

    manager.cancelBatch(batchId);
    const auto canceledTaskId = manager.activeTaskIdForPaths(
        TransferTask::Type::Download, QStringLiteral("/remote/file-4999.dat"),
        QStringLiteral("/local/file-4999.dat"));
    test.check(manager.activeTaskIdsForSession({}).isEmpty() &&
                   !canceledTaskId.has_value(),
               "batch cancellation should remove active work from queries");

    TransferManager sessionManager;
    sessionManager.setSessionIdentity(QStringLiteral("site-a"));
    sessionManager.enqueueDownload(QStringLiteral("/remote/session.dat"),
                                   QStringLiteral("/local/session.dat"));
    test.check(
        sessionManager.activeTaskIdsForSession(QStringLiteral("site-a")) ==
                QVector<quint64>{1} &&
            sessionManager.activeTaskIdsForSession(QStringLiteral("site-b"))
                .isEmpty(),
        "session queries should exclude work owned by another connection");
}

OPENSCP_TEST(testTaskNodesStayStableAcrossQueueGrowth, test) {
    TransferManager manager;
    manager.pauseAll();
    const quint64 first = manager.enqueueDownload(
        QStringLiteral("/remote/first"), QStringLiteral("/local/first"),
        TransferBatchOptions{});
    const TransferTask *firstAddress =
        TransferManagerTestAccess::taskAddress(manager, first);
    const std::size_t initialCapacity =
        TransferManagerTestAccess::taskVectorCapacity(manager);

    QVector<QPair<QString, QString>> bulk;
    bulk.reserve(10000);
    for (int index = 0; index < 10000; ++index) {
        bulk.push_back({QStringLiteral("/remote/item-%1").arg(index),
                        QStringLiteral("/local/item-%1").arg(index)});
    }
    manager.enqueueDownloads(bulk);

    test.check(TransferManagerTestAccess::taskVectorCapacity(manager) >
                   initialCapacity,
               "the stable task test should force queue vector reallocation");
    test.check(firstAddress != nullptr &&
                   TransferManagerTestAccess::taskAddress(manager, first) ==
                       firstAddress,
               "task node addresses must survive queue vector reallocation");
    const auto firstSnapshot = manager.taskSnapshot(first);
    test.check(firstSnapshot.has_value() &&
                   firstSnapshot->src == QStringLiteral("/remote/first"),
               "O(1) task lookup should remain valid after 10,000 inserts");
}

OPENSCP_TEST(testBlockedBatchSchedulerBenchmark, test) {
    if (!qEnvironmentVariableIsSet("OPENSCP_BENCH_TRANSFERS"))
        return;

    TransferManager manager;
    manager.setSessionIdentity(QStringLiteral("test-session"));
    auto batch = testBatchOptions();
    batch.batchId = manager.createBatch(batch);
    const quint64 prerequisite = manager.enqueueRemoteDirectory(
        QStringLiteral("/remote/prerequisite"), batch);
    manager.pauseTask(prerequisite);
    batch.waitForBatch = true;
    QVector<QPair<QString, QString>> downloads;
    downloads.reserve(5000);
    for (int index = 0; index < 5000; ++index) {
        downloads.push_back({QStringLiteral("/remote/file-%1").arg(index),
                             QStringLiteral("/local/file-%1").arg(index)});
    }
    manager.enqueueDownloads(downloads, batch);
    const auto elapsed =
        TransferManagerTestAccess::blockedBatchScan(manager, 3);
    std::cout << "BENCH blocked_batch_5000_scan_3_us=" << elapsed.count()
              << '\n';
    test.check(elapsed.count() > 0,
               "blocked-batch benchmark should execute scheduler checks");
}

OPENSCP_TEST(testRunnableTailSchedulerBenchmark, test) {
    if (!qEnvironmentVariableIsSet("OPENSCP_BENCH_TRANSFERS"))
        return;

    TransferManager manager;
    manager.setSessionIdentity(QStringLiteral("test-session"));
    QVector<QPair<QString, QString>> downloads;
    downloads.reserve(5000);
    for (int index = 0; index < 5000; ++index) {
        downloads.push_back({QStringLiteral("/remote/file-%1").arg(index),
                             QStringLiteral("/local/file-%1").arg(index)});
    }
    manager.enqueueDownloads(downloads, testBatchOptions());
    const auto elapsed =
        TransferManagerTestAccess::runnableTailSelection(manager, 3);
    std::cout << "BENCH runnable_tail_5000_select_3_us=" << elapsed.count()
              << '\n';
    test.check(elapsed.count() > 0,
               "runnable-tail benchmark should select queued work");
}

OPENSCP_TEST(testConcurrencyUpdates, test) {
    TransferManager manager;
    int queueSettingsNotifications = 0;
    QObject::connect(
        &manager, &TransferManager::queueSettingsChanged, &manager,
        [&queueSettingsNotifications] { ++queueSettingsNotifications; });

    manager.setMaxConcurrent(4);
    test.check(manager.maxConcurrent() == 4,
               "concurrency should update to the requested value");
    test.check(queueSettingsNotifications == 1,
               "changing concurrency should emit settings notification");

    manager.setMaxConcurrent(4);
    test.check(queueSettingsNotifications == 1,
               "setting the same concurrency should not notify again");

    manager.setMaxConcurrent(0);
    test.check(manager.maxConcurrent() == 1,
               "concurrency should be clamped to at least one");
    manager.setMaxConcurrent(100);
    test.check(manager.maxConcurrent() == 8,
               "concurrency should be clamped to the fixed worker pool");
}

OPENSCP_TEST(testRunningTaskSignalsMirrorQueueRequests, test) {
    TransferTaskControls controls;
    controls.pause(7);
    controls.startRunning(7, 64);
    const auto running = controls.runningSignals(7);
    test.check(running && running->stopRequested.load() &&
                   running->speedLimitKBps.load() == 64,
               "a task should start with its pending requests and limit");

    controls.resume(7);
    test.check(!running->stopRequested.load(),
               "resuming should clear the running task's stop request");
    controls.cancel(7);
    controls.resume(7);
    test.check(running->stopRequested.load() && controls.isCanceled(7),
               "resuming must not clear a cancellation");
    controls.forget(7);
    test.check(!running->stopRequested.load() && !controls.isCanceled(7),
               "forgetting should clear every request");

    controls.pause(7);
    test.check(running->stopRequested.load(),
               "pausing a running task should signal its worker");
    controls.resume(7);

    controls.setSpeedLimit(7, 128);
    controls.setSpeedLimit(8, 256);
    test.check(running->speedLimitKBps.load() == 128 &&
                   !controls.runningSignals(8),
               "speed limits should only reach running tasks");
    controls.finishRunning(7);
    test.check(!controls.runningSignals(7),
               "a finished task should no longer have signals");
}

struct ConcurrencyProbe {
    std::atomic<int> active{0};
    std::atomic<int> maximum{0};
    std::atomic<int> connections{0};
    std::atomic<int> transfers{0};
};

class DownloadMockClient : public openscp::MockSftpClient {
    public:
    openscp::ProtocolCapabilities capabilities() const override {
        openscp::ProtocolCapabilities result =
            openscp::MockSftpClient::capabilities();
        result.can_download = true;
        return result;
    }
};

class ConcurrentMockClient : public DownloadMockClient {
    public:
    explicit ConcurrentMockClient(std::shared_ptr<ConcurrencyProbe> probe)
        : probe_(std::move(probe)) {}

    bool get(const std::string &, const std::string &, std::string &err,
             std::function<void(std::size_t, std::size_t)> progress,
             std::function<bool()> shouldCancel, bool) override {
        probe_->transfers.fetch_add(1);
        const int activeNow = probe_->active.fetch_add(1) + 1;
        int observedMaximum = probe_->maximum.load();
        while (activeNow > observedMaximum &&
               !probe_->maximum.compare_exchange_weak(observedMaximum,
                                                      activeNow)) {
        }
        if (progress)
            progress(1, 2);
        for (int tick = 0; tick < 20; ++tick) {
            if (shouldCancel && shouldCancel()) {
                probe_->active.fetch_sub(1);
                err = "Canceled";
                return false;
            }
            std::this_thread::sleep_for(5ms);
        }
        if (progress)
            progress(2, 2);
        probe_->active.fetch_sub(1);
        err.clear();
        return true;
    }

    std::unique_ptr<openscp::RemoteClient>
    openConnection(const openscp::SessionOptions &options, std::string &err) {
        probe_->connections.fetch_add(1);
        return makeConnectedWorker<ConcurrentMockClient>(options, err, probe_);
    }

    private:
    std::shared_ptr<ConcurrencyProbe> probe_;
};

// Each fixture client opens the session's worker connections: new clients of
// its own type that share its probe or scripted state.
template <typename Client>
void configureManager(TransferManager &manager, Client &baseClient,
                      const openscp::SessionOptions &options) {
    std::string connectError;
    (void)baseClient.connect(options, connectError);
    manager.setSessionIdentity(QStringLiteral("test-session"));
    manager.setConnectionFactory([&baseClient, options](std::string &error) {
        return baseClient.openConnection(options, error);
    });
}

OPENSCP_TEST(testPersistentWorkersRunConcurrently, test) {
    auto probe = std::make_shared<ConcurrencyProbe>();
    ConcurrentMockClient baseClient(probe);
    const auto options = testOptions();

    TransferManager manager;
    manager.setMaxConcurrent(4);
    configureManager(manager, baseClient, options);

    QTemporaryDir destination;
    QVector<QPair<QString, QString>> downloads;
    for (int index = 0; index < 12; ++index) {
        downloads.push_back(
            {QStringLiteral("/remote/parallel-%1.dat").arg(index),
             destination.filePath(QStringLiteral("file-%1.dat").arg(index))});
    }
    auto batch = testBatchOptions();
    manager.enqueueDownloads(downloads, batch);

    const bool completed = waitUntil([&] {
        const auto tasks = manager.tasksSnapshot();
        return tasks.size() == downloads.size() &&
               std::all_of(tasks.cbegin(), tasks.cend(),
                           [](const TransferTask &task) {
                               return task.status == TransferTask::Status::Done;
                           });
    });
    test.check(completed, "all concurrent mock transfers should finish");
    test.check(probe->maximum.load() == 4,
               "four configured workers should transfer concurrently");
    test.check(probe->connections.load() <= 4,
               "worker slots should reuse their protocol connections");
    test.check(probe->transfers.load() == downloads.size(),
               "every queued task should run exactly once");
}

OPENSCP_TEST(testSuccessfulWorkerConnectionReuse, test) {
    auto probe = std::make_shared<ConcurrencyProbe>();
    ConcurrentMockClient baseClient(probe);
    TransferManager manager;
    manager.setMaxConcurrent(1);
    configureManager(manager, baseClient, testOptions());
    QTemporaryDir destination;
    auto batch = testBatchOptions();
    manager.enqueueDownloads(
        {{QStringLiteral("/remote/reuse-a"), destination.filePath("reuse-a")},
         {QStringLiteral("/remote/reuse-b"), destination.filePath("reuse-b")},
         {QStringLiteral("/remote/reuse-c"), destination.filePath("reuse-c")}},
        batch);
    test.check(waitUntil([&] {
                   const auto tasks = manager.tasksSnapshot();
                   return tasks.size() == 3 &&
                          std::all_of(tasks.cbegin(), tasks.cend(),
                                      [](const TransferTask &task) {
                                          return task.status ==
                                                 TransferTask::Status::Done;
                                      });
               }),
               "successful sequential transfers should complete");
    test.check(probe->connections.load() == 1,
               "a successful worker should reuse its protocol connection");
}

struct LifecycleProbe {
    std::atomic<int> connections{0};
    std::atomic<int> disconnects{0};
    std::atomic<int> interrupts{0};
    std::atomic<int> gets{0};
};

class CancelLifecycleClient final : public DownloadMockClient {
    public:
    explicit CancelLifecycleClient(std::shared_ptr<LifecycleProbe> probe)
        : probe_(std::move(probe)) {}

    void disconnect() override {
        probe_->disconnects.fetch_add(1);
        MockSftpClient::disconnect();
    }

    void interrupt() override { probe_->interrupts.fetch_add(1); }

    bool get(const std::string &, const std::string &, std::string &err,
             std::function<void(std::size_t, std::size_t)> progress,
             std::function<bool()> shouldCancel, bool) override {
        const int call = probe_->gets.fetch_add(1) + 1;
        if (call == 1) {
            while (!shouldCancel())
                std::this_thread::sleep_for(2ms);
            err = "Canceled";
            setLastOperationError(openscp::RemoteErrorKind::Canceled, err);
            return false;
        }
        clearLastOperationError();
        if (progress)
            progress(1, 1);
        err.clear();
        return true;
    }

    std::unique_ptr<openscp::RemoteClient>
    openConnection(const openscp::SessionOptions &options, std::string &err) {
        probe_->connections.fetch_add(1);
        return makeConnectedWorker<CancelLifecycleClient>(options, err, probe_);
    }

    private:
    std::shared_ptr<LifecycleProbe> probe_;
};

OPENSCP_TEST(testCanceledWorkerInvalidatesItsConnection, test) {
    auto probe = std::make_shared<LifecycleProbe>();
    CancelLifecycleClient baseClient(probe);
    TransferManager manager;
    manager.setMaxConcurrent(1);
    configureManager(manager, baseClient, testOptions());
    QTemporaryDir destination;
    auto batch = testBatchOptions();

    const quint64 canceledId =
        manager.enqueueDownload(QStringLiteral("/remote/cancel"),
                                destination.filePath("cancel"), batch);
    test.check(waitUntil([&] { return probe->gets.load() == 1; }),
               "cancel fixture should start its first transfer");
    manager.cancelTask(canceledId);
    test.check(waitUntil([&] {
                   return probe->disconnects.load() >= 1 &&
                          manager.taskSnapshot(canceledId)->status ==
                              TransferTask::Status::Canceled;
               }),
               "canceling should interrupt and invalidate the worker client");
    test.check(probe->interrupts.load() >= 1,
               "canceling an active task should invoke client interruption");

    const quint64 nextId =
        manager.enqueueDownload(QStringLiteral("/remote/after-cancel"),
                                destination.filePath("after-cancel"), batch);
    test.check(waitForStatus(manager, nextId, TransferTask::Status::Done),
               "the worker should recover after a canceled transfer");
    test.check(probe->connections.load() == 2,
               "the task after cancellation must use a fresh connection");
}

OPENSCP_TEST(testClearSessionInvalidatesWorkerConnections, test) {
    auto probe = std::make_shared<LifecycleProbe>();
    CancelLifecycleClient baseClient(probe);
    TransferManager manager;
    manager.setMaxConcurrent(1);
    configureManager(manager, baseClient, testOptions());
    QTemporaryDir destination;
    auto batch = testBatchOptions();
    const quint64 taskId =
        manager.enqueueDownload(QStringLiteral("/remote/disconnect"),
                                destination.filePath("disconnect"), batch);
    test.check(waitUntil([&] { return probe->gets.load() == 1; }),
               "disconnect fixture should start a worker transfer");

    manager.clearSession();
    const auto task = manager.taskSnapshot(taskId);
    test.check(task &&
                   task->status == TransferTask::Status::WaitingForConnection,
               "clearing the session should leave active work waiting");
    test.check(probe->interrupts.load() >= 1 && probe->disconnects.load() >= 1,
               "clearSession should interrupt and invalidate worker clients");
}

OPENSCP_TEST(testPausingRunningTaskStopsItsTransfer, test) {
    auto probe = std::make_shared<LifecycleProbe>();
    CancelLifecycleClient baseClient(probe);
    TransferManager manager;
    manager.setMaxConcurrent(1);
    configureManager(manager, baseClient, testOptions());
    QTemporaryDir destination;

    const quint64 taskId = manager.enqueueDownload(
        QStringLiteral("/remote/pause"), destination.filePath("pause"),
        testBatchOptions());
    test.check(waitUntil([&] { return probe->gets.load() == 1; }),
               "the transfer to pause should start");
    manager.pauseTask(taskId);
    test.check(waitForStatus(manager, taskId, TransferTask::Status::Paused),
               "pausing a running task should stop its transfer");
    manager.resumeTask(taskId);
    test.check(waitForStatus(manager, taskId, TransferTask::Status::Done),
               "a resumed task should finish");
}

struct SpeedLimitProbe {
    std::atomic_bool firstChunkReported{false};
    std::atomic_bool limitSet{false};
    std::atomic<qint64> limitedChunkMs{-1};
};

class SpeedLimitProbeClient final : public DownloadMockClient {
    public:
    explicit SpeedLimitProbeClient(std::shared_ptr<SpeedLimitProbe> probe)
        : probe_(std::move(probe)) {}

    bool get(const std::string &, const std::string &, std::string &err,
             std::function<void(std::size_t, std::size_t)> progress,
             std::function<bool()>, bool) override {
        constexpr std::size_t kChunk = 256 * 1024;
        progress(kChunk, 2 * kChunk);
        probe_->firstChunkReported.store(true);
        (void)waitUntil([this] { return probe_->limitSet.load(); });
        const auto started = std::chrono::steady_clock::now();
        progress(2 * kChunk, 2 * kChunk);
        probe_->limitedChunkMs.store(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - started)
                .count());
        err.clear();
        return true;
    }

    std::unique_ptr<openscp::RemoteClient>
    openConnection(const openscp::SessionOptions &options, std::string &err) {
        return makeConnectedWorker<SpeedLimitProbeClient>(options, err, probe_);
    }

    private:
    std::shared_ptr<SpeedLimitProbe> probe_;
};

OPENSCP_TEST(testSpeedLimitReachesRunningTask, test) {
    auto probe = std::make_shared<SpeedLimitProbe>();
    SpeedLimitProbeClient baseClient(probe);
    TransferManager manager;
    configureManager(manager, baseClient, testOptions());
    QTemporaryDir destination;

    const quint64 taskId = manager.enqueueDownload(
        QStringLiteral("/remote/limited"), destination.filePath("limited"),
        testBatchOptions());
    test.check(waitUntil([&] { return probe->firstChunkReported.load(); }),
               "the limited transfer should report its first chunk");
    // 256 KiB at 512 KiB/s should hold the next chunk for about half a second.
    manager.setTaskSpeedLimit(taskId, 512);
    probe->limitSet.store(true);
    test.check(waitForStatus(manager, taskId, TransferTask::Status::Done),
               "the limited transfer should finish");
    test.check(probe->limitedChunkMs.load() >= 250,
               "a new speed limit should slow the running transfer");
}

class PartialProgressFailureClient final : public DownloadMockClient {
    public:
    bool get(const std::string &, const std::string &, std::string &err,
             std::function<void(std::size_t, std::size_t)> progress,
             std::function<bool()>, bool) override {
        // The second report comes too soon after the first to be published.
        progress(4096, 65536);
        progress(8192, 65536);
        err = "Authentication failed";
        setLastOperationError(openscp::RemoteErrorKind::Authentication, err);
        return false;
    }

    std::unique_ptr<openscp::RemoteClient>
    openConnection(const openscp::SessionOptions &options, std::string &err) {
        return makeConnectedWorker<PartialProgressFailureClient>(options, err);
    }
};

OPENSCP_TEST(testUnpublishedProgressIsStoredWhenAttemptEnds, test) {
    PartialProgressFailureClient baseClient;
    TransferManager manager;
    configureManager(manager, baseClient, testOptions());
    QTemporaryDir destination;

    const quint64 taskId = manager.enqueueDownload(
        QStringLiteral("/remote/partial"), destination.filePath("partial"),
        testBatchOptions());
    test.check(waitForStatus(manager, taskId, TransferTask::Status::Error),
               "the partial transfer should fail");
    const auto task = manager.taskSnapshot(taskId);
    test.check(task && task->bytesDone == 8192,
               "the last progress report should be stored with the task");
}

OPENSCP_TEST(testWorkersConnectInParallel, test) {
    auto probe = std::make_shared<ConcurrencyProbe>();
    ConcurrentMockClient baseClient(probe);
    const auto options = testOptions();
    std::mutex handshakeMutex;
    std::condition_variable handshakeChanged;
    int handshaking = 0;
    int maximumHandshaking = 0;

    TransferManager manager;
    manager.setMaxConcurrent(2);
    manager.setSessionIdentity(QStringLiteral("test-session"));
    manager.setConnectionFactory([&](std::string &error) {
        {
            // Each handshake waits for a second one to overlap it, which can
            // only happen when workers do not connect one at a time.
            std::unique_lock lock(handshakeMutex);
            ++handshaking;
            maximumHandshaking = std::max(maximumHandshaking, handshaking);
            handshakeChanged.notify_all();
            handshakeChanged.wait_for(lock, 2s,
                                      [&] { return maximumHandshaking >= 2; });
            --handshaking;
        }
        return baseClient.openConnection(options, error);
    });

    QTemporaryDir destination;
    manager.enqueueDownloads(
        {{QStringLiteral("/remote/parallel-a"), destination.filePath("a")},
         {QStringLiteral("/remote/parallel-b"), destination.filePath("b")}},
        testBatchOptions());
    test.check(waitUntil([&] {
                   const auto tasks = manager.tasksSnapshot();
                   return tasks.size() == 2 &&
                          std::all_of(tasks.cbegin(), tasks.cend(),
                                      [](const TransferTask &task) {
                                          return task.status ==
                                                 TransferTask::Status::Done;
                                      });
               }),
               "transfers should finish after their workers connect");
    std::lock_guard lock(handshakeMutex);
    test.check(maximumHandshaking == 2,
               "workers should run their connection handshakes in parallel");
}

OPENSCP_TEST(testConnectionOpenedAfterClearSessionIsDiscarded, test) {
    auto probe = std::make_shared<LifecycleProbe>();
    CancelLifecycleClient baseClient(probe);
    const auto options = testOptions();
    std::mutex gateMutex;
    std::condition_variable gateChanged;
    bool handshakeStarted = false;
    bool handshakeReleased = false;

    TransferManager manager;
    manager.setMaxConcurrent(1);
    manager.setSessionIdentity(QStringLiteral("test-session"));
    manager.setConnectionFactory([&](std::string &error) {
        {
            std::unique_lock lock(gateMutex);
            handshakeStarted = true;
            gateChanged.notify_all();
            gateChanged.wait_for(lock, 5s, [&] { return handshakeReleased; });
        }
        return baseClient.openConnection(options, error);
    });
    QTemporaryDir destination;
    const quint64 taskId = manager.enqueueDownload(
        QStringLiteral("/remote/late"), destination.filePath("late"),
        testBatchOptions());
    {
        std::unique_lock lock(gateMutex);
        test.check(
            gateChanged.wait_for(lock, 5s, [&] { return handshakeStarted; }),
            "the worker should start connecting");
    }

    // clearSession() waits for the connecting worker, so it runs aside.
    std::thread clearing([&manager] { manager.clearSession(); });
    test.check(waitForStatus(manager, taskId,
                             TransferTask::Status::WaitingForConnection),
               "clearing should not wait for a handshake to update tasks");
    {
        std::lock_guard lock(gateMutex);
        handshakeReleased = true;
    }
    gateChanged.notify_all();
    clearing.join();

    test.check(probe->connections.load() == 1 && probe->disconnects.load() >= 1,
               "a connection opened for a cleared session should be closed");
    test.check(probe->gets.load() == 0,
               "a cleared session must not transfer on a late connection");
    const auto task = manager.taskSnapshot(taskId);
    test.check(task &&
                   task->status == TransferTask::Status::WaitingForConnection,
               "the task should keep waiting for a new session");
}

class FinalTransportFailureClient final : public DownloadMockClient {
    public:
    explicit FinalTransportFailureClient(std::shared_ptr<LifecycleProbe> probe)
        : probe_(std::move(probe)) {}

    void disconnect() override {
        probe_->disconnects.fetch_add(1);
        MockSftpClient::disconnect();
    }

    bool get(const std::string &, const std::string &, std::string &err,
             std::function<void(std::size_t, std::size_t)> progress,
             std::function<bool()>, bool) override {
        const int call = probe_->gets.fetch_add(1) + 1;
        if (call == 1) {
            err = "Connection closed";
            setLastOperationError(openscp::RemoteErrorKind::Connection, err, 0,
                                  false);
            return false;
        }
        clearLastOperationError();
        if (progress)
            progress(1, 1);
        err.clear();
        return true;
    }

    std::unique_ptr<openscp::RemoteClient>
    openConnection(const openscp::SessionOptions &options, std::string &err) {
        probe_->connections.fetch_add(1);
        return makeConnectedWorker<FinalTransportFailureClient>(options, err,
                                                                probe_);
    }

    private:
    std::shared_ptr<LifecycleProbe> probe_;
};

OPENSCP_TEST(testFinalTransportErrorInvalidatesConnection, test) {
    auto probe = std::make_shared<LifecycleProbe>();
    FinalTransportFailureClient baseClient(probe);
    TransferManager manager;
    manager.setMaxConcurrent(1);
    configureManager(manager, baseClient, testOptions());
    QTemporaryDir destination;
    auto batch = testBatchOptions();

    const quint64 failedId = manager.enqueueDownload(
        QStringLiteral("/remote/transport-failure"),
        destination.filePath("transport-failure"), batch);
    test.check(waitUntil([&] {
                   const auto task = manager.taskSnapshot(failedId);
                   return task && task->status == TransferTask::Status::Error &&
                          probe->disconnects.load() >= 1;
               }),
               "a final transport failure should invalidate its connection");
    const quint64 nextId = manager.enqueueDownload(
        QStringLiteral("/remote/transport-recovery"),
        destination.filePath("transport-recovery"), batch);
    test.check(waitForStatus(manager, nextId, TransferTask::Status::Done),
               "a later task should recover from a final transport failure");
    test.check(probe->connections.load() == 2,
               "recovery after a transport failure should reconnect");
}

OPENSCP_TEST(testPersistentTaskDependencies, test) {
    auto probe = std::make_shared<ConcurrencyProbe>();
    ConcurrentMockClient baseClient(probe);
    const auto options = testOptions();

    TransferManager manager;
    manager.setMaxConcurrent(4);
    configureManager(manager, baseClient, options);
    QTemporaryDir destination;

    auto batch = testBatchOptions();
    const quint64 first = manager.enqueueDownload(
        QStringLiteral("/remote/ordered-a"),
        destination.filePath(QStringLiteral("ordered-a")), batch);
    batch.dependsOnTaskId = first;
    manager.enqueueDownload(QStringLiteral("/remote/ordered-b"),
                            destination.filePath(QStringLiteral("ordered-b")),
                            batch);

    test.check(waitUntil([&] {
                   const auto tasks = manager.tasksSnapshot();
                   return tasks.size() == 2 &&
                          tasks[0].status == TransferTask::Status::Done &&
                          tasks[1].status == TransferTask::Status::Done;
               }),
               "dependent tasks should run after their prerequisite");
    test.check(probe->maximum.load() == 1,
               "a persisted dependency should prevent concurrent execution");
}

OPENSCP_TEST(testDestinationReservation, test) {
    auto probe = std::make_shared<ConcurrencyProbe>();
    ConcurrentMockClient baseClient(probe);
    const auto options = testOptions();
    TransferManager manager;
    manager.setMaxConcurrent(4);
    configureManager(manager, baseClient, options);

    QTemporaryDir destination;
    const QString sameDestination = destination.filePath("same.dat");
    auto batch = testBatchOptions();
    QVector<QPair<QString, QString>> downloads{
        {QStringLiteral("/remote/a.dat"), sameDestination},
        {QStringLiteral("/remote/b.dat"), sameDestination},
    };
    manager.enqueueDownloads(downloads, batch);
    test.check(waitUntil([&] {
                   return probe->transfers.load() == 2 &&
                          manager.tasksSnapshot()[1].status ==
                              TransferTask::Status::Done;
               }),
               "tasks sharing a destination should both finish");
    test.check(probe->maximum.load() == 1,
               "one destination must never belong to concurrent tasks");
}

OPENSCP_TEST(testWorkersSerializeOverlappingDownloadPaths, test) {
    auto probe = std::make_shared<ConcurrencyProbe>();
    ConcurrentMockClient baseClient(probe);
    TransferManager manager;
    manager.setMaxConcurrent(4);
    configureManager(manager, baseClient, testOptions());
    QTemporaryDir root;
    manager.enqueueDownloads(
        {{QStringLiteral("/remote/a"), root.filePath("file")},
         {QStringLiteral("/remote/b"), root.filePath("file.part")}},
        testBatchOptions());
    test.check(waitUntil([&] {
                   const auto tasks = manager.tasksSnapshot();
                   return tasks.size() == 2 &&
                          std::all_of(tasks.cbegin(), tasks.cend(),
                                      [](const auto &task) {
                                          return task.status ==
                                                 TransferTask::Status::Done;
                                      });
               }),
               "downloads with overlapping publication paths should complete");
    test.check(probe->maximum.load() == 1,
               "workers must serialize final-to-partial collisions");
}

OPENSCP_TEST(testDownloadReservesPublicationPaths, test) {
    QTemporaryDir root;
    TransferManager manager;
    TransferTask first;
    first.taskId = 1;
    first.type = TransferTask::Type::Download;
    first.dst = root.filePath("file");
    TransferTask second = first;
    second.taskId = 2;
    second.dst += QStringLiteral(".part");
    test.check(TransferManagerTestAccess::reserve(manager, first),
               "the first download should reserve its publication paths");
    test.check(!TransferManagerTestAccess::reserve(manager, second),
               "a destination must not overlap an active download's partial");

    TransferTask independent = first;
    independent.taskId = 3;
    independent.dst = root.filePath("independent");
    test.check(TransferManagerTestAccess::reserve(manager, independent),
               "unrelated downloads should still reserve concurrently");
    TransferManagerTestAccess::release(manager, independent.taskId);

    auto publish = [&](const TransferTask &task, const std::string &contents) {
        const std::string destination = task.dst.toStdString();
        const std::string partial = destination + ".part";
        std::string error;
        openscp::UniqueFile file(openscp::localfiles::openRegularFileForWrite(
            partial, openscp::localfiles::WriteMode::Truncate, error));
        if (!file)
            return false;
        if (std::fwrite(contents.data(), 1, contents.size(), file.get()) !=
                contents.size() ||
            !openscp::localfiles::flushAndSync(file.get(), error))
            return false;
        file.reset();
        return openscp::localfiles::atomicReplace(partial, destination, error);
    };
    test.check(publish(first, "first download"),
               "the first download should publish through the real helpers");
    TransferManagerTestAccess::release(manager, first.taskId);
    test.check(TransferManagerTestAccess::reserve(manager, second),
               "the conflicting download should become runnable after release");
    test.check(!TransferManagerTestAccess::reserve(manager, first),
               "partial overlap must also be excluded in the opposite order");
    test.check(publish(second, "second download"),
               "the second download should publish its own contents");
    TransferManagerTestAccess::release(manager, second.taskId);
    QFile firstFile(first.dst);
    QFile secondFile(second.dst);
    test.check(firstFile.open(QIODevice::ReadOnly) &&
                   firstFile.readAll() == "first download" &&
                   secondFile.open(QIODevice::ReadOnly) &&
                   secondFile.readAll() == "second download",
               "successful publications must retain their respective contents");
    test.check(TransferManagerTestAccess::reserve(manager, first),
               "all publication reservations should be released together");
    TransferManagerTestAccess::release(manager, first.taskId);
}

OPENSCP_TEST(testLocalReservationsExcludeCaseAndUnicodeAliases, test) {
    QTemporaryDir root;
    TransferManager manager;
    TransferTask first;
    first.taskId = 1;
    first.type = TransferTask::Type::Download;
    first.dst = root.filePath(QString::fromUtf8("caf\xc3\xa9"));
    test.check(TransferManagerTestAccess::reserve(manager, first),
               "the local destination should be reserved");
    TransferTask alias = first;
    alias.taskId = 2;
    alias.dst = root.filePath(QString::fromUtf8("CAFE\xcc\x81"));
    test.check(!TransferManagerTestAccess::reserve(manager, alias),
               "case and normalization aliases must share a local reservation");
    alias.dst += QStringLiteral(".PART");
    test.check(!TransferManagerTestAccess::reserve(manager, alias),
               "partial paths must use the same local alias normalization");
    alias.type = TransferTask::Type::Upload;
    alias.src = alias.dst;
    alias.dst = QStringLiteral("/remote/other");
    test.check(!TransferManagerTestAccess::reserve(manager, alias),
               "uploads must not read an active download's temporary file");
    alias.type = TransferTask::Type::DeleteLocalFile;
    alias.dst = alias.src;
    test.check(!TransferManagerTestAccess::reserve(manager, alias),
               "local deletion must not remove an active download's temporary");
    TransferManagerTestAccess::release(manager, first.taskId);
}

OPENSCP_TEST(testUploadReservationsIncludePartialAndKeepRemoteCase, test) {
    QTemporaryDir root;
    TransferManager manager;
    TransferTask first;
    first.taskId = 1;
    first.type = TransferTask::Type::Upload;
    first.src = root.filePath("source");
    first.dst = QStringLiteral("/remote/file");
    TransferTask second = first;
    second.taskId = 2;
    second.dst += QStringLiteral(".part");
    test.check(TransferManagerTestAccess::reserve(manager, first) &&
                   !TransferManagerTestAccess::reserve(manager, second),
               "remote uploads must also reserve their partial destination");
    second.dst = QStringLiteral("/remote/FILE");
    test.check(TransferManagerTestAccess::reserve(manager, second),
               "remote reservations must preserve case-sensitive semantics");
    TransferManagerTestAccess::release(manager, first.taskId);
    TransferManagerTestAccess::release(manager, second.taskId);
}

OPENSCP_TEST(testRenameSkipsReservedPartialDestination, test) {
    QTemporaryDir root;
    TransferManager manager;
    TransferTask task;
    task.taskId = 1;
    task.type = TransferTask::Type::Download;
    task.dst = root.filePath("file.dat");
    TransferTask blocker = task;
    blocker.taskId = 2;
    blocker.dst = root.filePath("file (1).dat.part");
    test.check(
        TransferManagerTestAccess::reserve(manager, task) &&
            TransferManagerTestAccess::reserve(manager, blocker),
        "original destination and conflicting rename should be reserved");
    std::string error;
    test.check(TransferManagerTestAccess::rename(manager, task, error) &&
                   task.dst == root.filePath("file (2).dat"),
               "rename must skip candidates whose temporary path is reserved");
    TransferTask probe = task;
    probe.taskId = 3;
    probe.dst += QStringLiteral(".part");
    test.check(!TransferManagerTestAccess::reserve(manager, probe),
               "rename must reserve the new temporary path");
    TransferManagerTestAccess::release(manager, task.taskId);
    TransferManagerTestAccess::release(manager, blocker.taskId);
}

struct RetryProbe {
    std::atomic<int> attempts{0};
};

class RetryMockClient final : public DownloadMockClient {
    public:
    explicit RetryMockClient(std::shared_ptr<RetryProbe> probe)
        : probe_(std::move(probe)) {}

    bool get(const std::string &, const std::string &, std::string &err,
             std::function<void(std::size_t, std::size_t)> progress,
             std::function<bool()>, bool) override {
        const int attempt = probe_->attempts.fetch_add(1) + 1;
        if (attempt < 3) {
            err = "Connection reset; Retry-After: 0";
            setLastOperationError(openscp::RemoteErrorKind::Connection, err, 0,
                                  true, false, 0);
            return false;
        }
        clearLastOperationError();
        if (progress)
            progress(8, 8);
        err.clear();
        return true;
    }

    std::unique_ptr<openscp::RemoteClient>
    openConnection(const openscp::SessionOptions &options, std::string &err) {
        return makeConnectedWorker<RetryMockClient>(options, err, probe_);
    }

    private:
    std::shared_ptr<RetryProbe> probe_;
};

OPENSCP_TEST(testTransientRetries, test) {
    auto probe = std::make_shared<RetryProbe>();
    RetryMockClient baseClient(probe);
    const auto options = testOptions();
    TransferManager manager;
    configureManager(manager, baseClient, options);
    QTemporaryDir destination;

    auto batch = testBatchOptions();
    manager.enqueueDownload(QStringLiteral("/remote/retry.dat"),
                            destination.filePath("retry.dat"), batch);

    test.check(waitForStatus(manager, 1, TransferTask::Status::Done),
               "transient failures should retry to completion");
    const auto task = manager.taskSnapshot(1);
    test.check(probe->attempts.load() == 3 && task && task->attempts == 3,
               "retry attempts should include the initial transfer");
}

struct FailureScenario {
    std::string name;
    std::string remotePath;
    openscp::RemoteErrorKind errorKind = openscp::RemoteErrorKind::Unknown;
    std::string message;
    bool hasStructuredError = true;
    bool transient = false;
    bool commitUncertain = false;
    TransferTask::Status expectedStatus = TransferTask::Status::Error;
    int expectedAutomaticAttempts = 1;
    bool rejectManualRetry = false;
};

class FailureDownloadState final {
    public:
    explicit FailureDownloadState(std::vector<FailureScenario> scenarios)
        : scenarios_(std::move(scenarios)) {}

    const FailureScenario *scenarioFor(const std::string &remotePath) const {
        const auto found =
            std::find_if(scenarios_.cbegin(), scenarios_.cend(),
                         [&](const FailureScenario &scenario) {
                             return scenario.remotePath == remotePath;
                         });
        return found == scenarios_.cend() ? nullptr : &*found;
    }

    void recordAttempt(const std::string &remotePath) {
        std::lock_guard<std::mutex> lock(mutex_);
        ++attempts_[remotePath];
    }

    int attemptsFor(const std::string &remotePath) const {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto found = attempts_.find(remotePath);
        return found == attempts_.end() ? 0 : found->second;
    }

    const std::vector<FailureScenario> &scenarios() const { return scenarios_; }

    private:
    const std::vector<FailureScenario> scenarios_;
    mutable std::mutex mutex_;
    std::unordered_map<std::string, int> attempts_;
};

class FailureDownloadClient final : public DownloadMockClient {
    public:
    explicit FailureDownloadClient(std::shared_ptr<FailureDownloadState> state)
        : state_(std::move(state)) {}

    bool get(const std::string &remote, const std::string &, std::string &err,
             std::function<void(std::size_t, std::size_t)>,
             std::function<bool()>, bool) override {
        const FailureScenario *scenario = state_->scenarioFor(remote);
        if (!scenario) {
            err = "Unexpected failure scenario: " + remote;
            setLastOperationError(openscp::RemoteErrorKind::InvalidRequest,
                                  err);
            return false;
        }
        state_->recordAttempt(remote);
        err = scenario->message;
        if (scenario->hasStructuredError) {
            setLastOperationError(scenario->errorKind, err, 0,
                                  scenario->transient,
                                  scenario->commitUncertain);
        } else {
            clearLastOperationError();
        }
        return false;
    }

    std::unique_ptr<openscp::RemoteClient>
    openConnection(const openscp::SessionOptions &options, std::string &err) {
        return makeConnectedWorker<FailureDownloadClient>(options, err, state_);
    }

    private:
    std::shared_ptr<FailureDownloadState> state_;
};

std::shared_ptr<FailureDownloadState>
authenticationFailureState(std::string remotePath) {
    return std::make_shared<FailureDownloadState>(std::vector<FailureScenario>{
        {.name = "authentication",
         .remotePath = std::move(remotePath),
         .errorKind = openscp::RemoteErrorKind::Authentication,
         .message = "Authentication failed",
         .transient = true}});
}

struct MovePhaseProbe {
    std::atomic<int> downloads{0};
    std::atomic<int> sourceDeletes{0};
};

class MovePhaseMockClient final : public DownloadMockClient {
    public:
    explicit MovePhaseMockClient(std::shared_ptr<MovePhaseProbe> probe)
        : probe_(std::move(probe)) {}

    bool get(const std::string &, const std::string &, std::string &err,
             std::function<void(std::size_t, std::size_t)> progress,
             std::function<bool()>, bool) override {
        probe_->downloads.fetch_add(1);
        clearLastOperationError();
        if (progress)
            progress(8, 8);
        err.clear();
        return true;
    }

    bool removeFile(const std::string &, std::string &err) override {
        const int attempt = probe_->sourceDeletes.fetch_add(1) + 1;
        if (attempt == 1) {
            err = "Temporary source cleanup failure";
            setLastOperationError(openscp::RemoteErrorKind::RemoteIo, err, 0,
                                  true, false);
            return false;
        }
        clearLastOperationError();
        err.clear();
        return true;
    }

    std::unique_ptr<openscp::RemoteClient>
    openConnection(const openscp::SessionOptions &options, std::string &err) {
        return makeConnectedWorker<MovePhaseMockClient>(options, err, probe_);
    }

    private:
    std::shared_ptr<MovePhaseProbe> probe_;
};

OPENSCP_TEST(testMoveDeleteSourcePhasePersistsWithoutRetransfer, test) {
    QTemporaryDir root;
    const QString queuePath = root.filePath("transfer-queue-v1.json");
    auto probe = std::make_shared<MovePhaseProbe>();
    {
        MovePhaseMockClient baseClient(probe);
        TransferManager manager;
        test.check(manager.enablePersistence(queuePath),
                   "move-phase persistence fixture should initialize");
        manager.setMaxConcurrent(1);
        configureManager(manager, baseClient, testOptions());
        auto batch = testBatchOptions();
        batch.operation = TransferOperation::Move;
        manager.enqueueDownload(QStringLiteral("/remote/move-source"),
                                root.filePath("move-destination"), batch);
        test.check(waitUntil([&] {
                       const auto task = manager.taskSnapshot(1);
                       return task &&
                              task->status == TransferTask::Status::Warning;
                   }),
                   "a failed move cleanup should become a warning");
        const auto pending = manager.taskSnapshot(1);
        test.check(pending && pending->phase == TransferPhase::DeleteSource &&
                       probe->downloads.load() == 1 &&
                       probe->sourceDeletes.load() == 1,
                   "move cleanup failure should persist after the copy phase");
        manager.persistNow();
    }

    MovePhaseMockClient baseClient(probe);
    TransferManager restored;
    test.check(restored.enablePersistence(queuePath),
               "pending move cleanup should restore");
    const auto pending = restored.taskSnapshot(1);
    test.check(pending && pending->restored &&
                   pending->status == TransferTask::Status::Paused &&
                   pending->phase == TransferPhase::DeleteSource,
               "restored move should remain paused in DeleteSource");
    restored.setMaxConcurrent(1);
    configureManager(restored, baseClient, testOptions());
    restored.resumeTask(1);
    test.check(waitForStatus(restored, 1, TransferTask::Status::Done),
               "retrying a restored move should complete source cleanup");
    test.check(probe->downloads.load() == 1 && probe->sourceDeletes.load() == 2,
               "DeleteSource retry must not repeat the completed transfer");
}

#ifndef _WIN32
struct SourceUploadProbe {
    std::atomic_int uploads{0};
    std::mutex mutex;
    std::condition_variable changed;
    bool holdMove = false;
    bool moveEntered = false;
    bool releaseMove = false;
    bool holdCopy = false;
    bool copyEntered = false;
    bool releaseCopy = false;
    bool replaceMove = false;
    std::unordered_map<std::string, std::string> uploaded;
};

class SourceUploadClient final : public openscp::MockSftpClient {
    public:
    explicit SourceUploadClient(std::shared_ptr<SourceUploadProbe> probe)
        : probe_(std::move(probe)) {}

    bool put(const std::string &local, const std::string &remote,
             std::string &err,
             std::function<void(std::size_t, std::size_t)> progress,
             std::function<bool()>, bool) override {
        ++probe_->uploads;
        QFile input(QString::fromStdString(local));
        if (!input.open(QIODevice::ReadOnly)) {
            err = "Could not read local source";
            return false;
        }
        const QByteArray contents = input.readAll();
        input.close();
        {
            std::unique_lock<std::mutex> lock(probe_->mutex);
            probe_->uploaded[remote] = contents.toStdString();
            if (remote == "/home/demo/move") {
                probe_->moveEntered = true;
                probe_->changed.notify_all();
                if (probe_->holdMove &&
                    !probe_->changed.wait_for(
                        lock, 5s, [&] { return probe_->releaseMove; })) {
                    err = "Timed out waiting for the move fixture";
                    return false;
                }
            } else if (remote == "/home/demo/copy") {
                probe_->copyEntered = true;
                probe_->changed.notify_all();
                if (probe_->holdCopy &&
                    !probe_->changed.wait_for(
                        lock, 5s, [&] { return probe_->releaseCopy; })) {
                    err = "Timed out waiting for the copy fixture";
                    return false;
                }
            }
        }
        if (remote == "/home/demo/move" && probe_->replaceMove) {
            const QString source = QString::fromStdString(local);
            if (!QFile::rename(source, source + QStringLiteral(".original"))) {
                err = "Could not move original fixture file";
                return false;
            }
            QFile replacement(source);
            if (!replacement.open(QIODevice::WriteOnly) ||
                replacement.write("replacement") != 11) {
                err = "Could not write replacement fixture file";
                return false;
            }
        }
        if (progress)
            progress(static_cast<std::size_t>(contents.size()),
                     static_cast<std::size_t>(contents.size()));
        clearLastOperationError();
        err.clear();
        return true;
    }

    std::unique_ptr<openscp::RemoteClient>
    openConnection(const openscp::SessionOptions &options, std::string &err) {
        return makeConnectedWorker<SourceUploadClient>(options, err, probe_);
    }

    private:
    std::shared_ptr<SourceUploadProbe> probe_;
};

OPENSCP_TEST(testMoveUploadPreservesReplacedLocalSource, test) {
    QTemporaryDir root;
    const QString source = root.filePath("source.txt");
    QFile initial(source);
    test.check(initial.open(QIODevice::WriteOnly) &&
                   initial.write("original") == 8,
               "move fixture should create the original source");
    initial.close();

    auto probe = std::make_shared<SourceUploadProbe>();
    probe->replaceMove = true;
    SourceUploadClient baseClient(probe);
    TransferManager manager;
    manager.setMaxConcurrent(1);
    configureManager(manager, baseClient, testOptions());
    auto batch = testBatchOptions();
    batch.operation = TransferOperation::Move;
    const quint64 id =
        manager.enqueueUpload(source, QStringLiteral("/home/demo/move"), batch);
    test.check(waitForStatus(manager, id, TransferTask::Status::Warning),
               "a replaced source should produce a cleanup warning");
    QFile replacement(source);
    test.check(replacement.open(QIODevice::ReadOnly) &&
                   replacement.readAll() == "replacement",
               "move cleanup must preserve the replacement file");
}

OPENSCP_TEST(testMoveCleanupWithoutIdentityPreservesSource, test) {
    QTemporaryDir root;
    const QString source = root.filePath("source.txt");
    QFile initial(source);
    test.check(initial.open(QIODevice::WriteOnly) &&
                   initial.write("source") == 6,
               "restored cleanup fixture should create the source");
    initial.close();

    TransferTask restored;
    restored.type = TransferTask::Type::Upload;
    restored.src = source;
    restored.phase = TransferPhase::DeleteSource;
    restored.postAction = TransferPostAction::DeleteSource;
    std::string error;
    test.check(!TransferExecutor::runPostAction(restored, nullptr, error) &&
                   error.find("restart") != std::string::npos &&
                   QFileInfo::exists(source),
               "cleanup without a process-local identity must fail closed");
    test.check(QFile::remove(source) &&
                   TransferExecutor::runPostAction(restored, nullptr, error) &&
                   error.empty(),
               "restored cleanup should finish only after manual removal");
}

OPENSCP_TEST(testMoveCleanupWaitsForQueuedSourceReader, test) {
    QTemporaryDir root;
    const QString source = root.filePath("source.txt");
    QFile initial(source);
    test.check(initial.open(QIODevice::WriteOnly) &&
                   initial.write("original") == 8,
               "shared-source fixture should create the source");
    initial.close();

    auto probe = std::make_shared<SourceUploadProbe>();
    probe->holdMove = true;
    SourceUploadClient baseClient(probe);
    TransferManager manager;
    manager.setMaxConcurrent(1);
    configureManager(manager, baseClient, testOptions());
    auto moveBatch = testBatchOptions();
    moveBatch.operation = TransferOperation::Move;
    const quint64 move = manager.enqueueUpload(
        source, QStringLiteral("/home/demo/move"), moveBatch);
    {
        std::unique_lock<std::mutex> lock(probe->mutex);
        test.check(probe->changed.wait_for(lock, 5s,
                                           [&] { return probe->moveEntered; }),
                   "move fixture should enter its upload");
    }
    const quint64 copy = manager.enqueueUpload(
        source, QStringLiteral("/home/demo/copy"), testBatchOptions());
    {
        std::lock_guard<std::mutex> lock(probe->mutex);
        probe->releaseMove = true;
    }
    probe->changed.notify_all();
    test.check(waitForStatus(manager, copy, TransferTask::Status::Done) &&
                   waitForStatus(manager, move, TransferTask::Status::Warning),
               "copy should read the shared source before manual move cleanup");
    {
        std::lock_guard<std::mutex> lock(probe->mutex);
        test.check(probe->uploaded["/home/demo/copy"] == "original",
                   "queued copy should upload the original source bytes");
    }
    test.check(QFileInfo::exists(source),
               "move must preserve its source after the queued copy finishes");
}

OPENSCP_TEST(testMoveCleanupWaitsForRunningSourceReader, test) {
    QTemporaryDir root;
    const QString source = root.filePath("source.txt");
    QFile initial(source);
    test.check(initial.open(QIODevice::WriteOnly) &&
                   initial.write("original") == 8,
               "concurrent fixture should create the source");
    initial.close();

    auto probe = std::make_shared<SourceUploadProbe>();
    probe->holdCopy = true;
    SourceUploadClient baseClient(probe);
    TransferManager manager;
    manager.setMaxConcurrent(2);
    auto moveBatch = testBatchOptions();
    moveBatch.operation = TransferOperation::Move;
    const quint64 move = manager.enqueueUpload(
        source, QStringLiteral("/home/demo/move"), moveBatch);
    const quint64 copy = manager.enqueueUpload(
        source, QStringLiteral("/home/demo/copy"), testBatchOptions());
    configureManager(manager, baseClient, testOptions());
    {
        std::unique_lock<std::mutex> lock(probe->mutex);
        test.check(probe->changed.wait_for(lock, 5s,
                                           [&] { return probe->copyEntered; }),
                   "copy should be reading when move cleanup is considered");
    }
    test.check(waitUntil([&] {
                   const auto task = manager.taskSnapshot(move);
                   return task && task->phase == TransferPhase::DeleteSource;
               }) &&
                   QFileInfo::exists(source),
               "move must keep the source while another worker reads it");
    {
        std::lock_guard<std::mutex> lock(probe->mutex);
        probe->releaseCopy = true;
    }
    probe->changed.notify_all();
    test.check(
        waitForStatus(manager, copy, TransferTask::Status::Done) &&
            waitForStatus(manager, move, TransferTask::Status::Warning) &&
            QFileInfo::exists(source),
        "move should require manual cleanup after the active copy completes");
}

OPENSCP_TEST(testMoveCleanupDoesNotWaitForItsDependentTask, test) {
    QTemporaryDir root;
    const QString source = root.filePath("source.txt");
    QFile initial(source);
    test.check(initial.open(QIODevice::WriteOnly) &&
                   initial.write("original") == 8,
               "dependency fixture should create the source");
    initial.close();

    auto probe = std::make_shared<SourceUploadProbe>();
    SourceUploadClient baseClient(probe);
    TransferManager manager;
    manager.setMaxConcurrent(1);
    auto moveBatch = testBatchOptions();
    moveBatch.operation = TransferOperation::Move;
    const quint64 move = manager.enqueueUpload(
        source, QStringLiteral("/home/demo/move"), moveBatch);
    auto dependent = testBatchOptions();
    dependent.dependsOnTaskId = move;
    const quint64 copy = manager.enqueueUpload(
        source, QStringLiteral("/home/demo/copy"), dependent);
    configureManager(manager, baseClient, testOptions());

    test.check(waitForStatus(manager, move, TransferTask::Status::Warning),
               "move cleanup must not wait for a task depending on that move");
    test.check(waitForStatus(manager, copy, TransferTask::Status::Skipped) &&
                   QFileInfo::exists(source),
               "dependent work must wait for successful cleanup without "
               "deleting the source");
}

OPENSCP_TEST(testMoveUploadManualCleanupDoesNotRetransfer, test) {
    QTemporaryDir root;
    const QString source = root.filePath("source.txt");
    QFile initial(source);
    test.check(initial.open(QIODevice::WriteOnly) &&
                   initial.write("original") == 8,
               "manual cleanup fixture should create the local source");
    initial.close();
    auto probe = std::make_shared<SourceUploadProbe>();
    SourceUploadClient baseClient(probe);
    TransferManager manager;
    manager.setMaxConcurrent(1);
    configureManager(manager, baseClient, testOptions());
    auto batch = testBatchOptions();
    batch.operation = TransferOperation::Move;
    const quint64 move =
        manager.enqueueUpload(source, QStringLiteral("/home/demo/move"), batch);
    test.check(
        waitForStatus(manager, move, TransferTask::Status::Warning) &&
            QFileInfo::exists(source),
        "completed move uploads must retain existing local sources for review");
    const auto pending = manager.taskSnapshot(move);
    test.check(
        pending && pending->phase == TransferPhase::DeleteSource &&
            pending->error.contains(QStringLiteral("manually")),
        "manual cleanup must retain the copy result and explain the warning");
    test.check(QFile::remove(source),
               "manual cleanup fixture should remove the reviewed source");
    manager.retryTask(move);
    test.check(waitForStatus(manager, move, TransferTask::Status::Done),
               "retry after manual removal should complete the existing "
               "cleanup phase");
    std::lock_guard lock(probe->mutex);
    test.check(
        probe->uploaded["/home/demo/move"] == "original" &&
            probe->uploads.load() == 1,
        "cleanup retry must preserve the upload without transferring again");
}
#endif

OPENSCP_TEST(testFailureScenariosDoNotRetry, test) {
    auto state =
        std::make_shared<FailureDownloadState>(std::vector<FailureScenario>{
            {.name = "unclassified",
             .remotePath = "/remote/unclassified.dat",
             .message = {},
             .hasStructuredError = false},
            {.name = "authentication",
             .remotePath = "/remote/auth.dat",
             .errorKind = openscp::RemoteErrorKind::Authentication,
             .message = "Authentication failed",
             .transient = true},
            {.name = "insufficient space",
             .remotePath = "/remote/full.dat",
             .errorKind = openscp::RemoteErrorKind::InsufficientSpace,
             .message = "Remote filesystem has insufficient space",
             .transient = true},
            {.name = "certificate",
             .remotePath = "/remote/certificate",
             .errorKind = openscp::RemoteErrorKind::Certificate,
             .message = "Certificate verification failed",
             .transient = true},
            {.name = "permission denied",
             .remotePath = "/remote/permission",
             .errorKind = openscp::RemoteErrorKind::PermissionDenied,
             .message = "Permission denied",
             .transient = true},
            {.name = "integrity",
             .remotePath = "/remote/integrity",
             .errorKind = openscp::RemoteErrorKind::Integrity,
             .message = "Checksum mismatch",
             .transient = true},
            {.name = "commit uncertain",
             .remotePath = "/remote/uncertain.dat",
             .errorKind = openscp::RemoteErrorKind::Connection,
             .message = "Connection lost after final server response",
             .transient = true,
             .commitUncertain = true,
             .expectedStatus = TransferTask::Status::Warning,
             .rejectManualRetry = true},
        });
    FailureDownloadClient baseClient(state);
    TransferManager manager;
    manager.setMaxConcurrent(static_cast<int>(state->scenarios().size()));
    configureManager(manager, baseClient, testOptions());
    QTemporaryDir destination;
    auto batch = testBatchOptions();
    QVector<QPair<QString, QString>> downloads;
    for (const FailureScenario &scenario : state->scenarios()) {
        downloads.push_back(
            {QString::fromStdString(scenario.remotePath),
             destination.filePath(QString::fromStdString(scenario.name))});
    }
    manager.enqueueDownloads(downloads, batch);

    test.check(
        waitUntil([&] {
            const auto tasks = manager.tasksSnapshot();
            if (tasks.size() != downloads.size())
                return false;
            for (qsizetype index = 0; index < tasks.size(); ++index) {
                if (tasks[index].status !=
                    state->scenarios()[static_cast<std::size_t>(index)]
                        .expectedStatus) {
                    return false;
                }
            }
            return true;
        }),
        "all failure scenarios should reach their expected final status");

    const auto tasks = manager.tasksSnapshot();
    for (std::size_t index = 0; index < state->scenarios().size() &&
                                index < static_cast<std::size_t>(tasks.size());
         ++index) {
        const FailureScenario &scenario = state->scenarios()[index];
        const TransferTask &task = tasks[static_cast<qsizetype>(index)];
        const std::string label = scenario.name + ": ";
        test.check(task.status == scenario.expectedStatus,
                   label + "final status should match");
        test.check(state->attemptsFor(scenario.remotePath) ==
                           scenario.expectedAutomaticAttempts &&
                       task.attempts == scenario.expectedAutomaticAttempts,
                   label + "automatic attempt count should match");
        test.check(task.commitUncertain == scenario.commitUncertain,
                   label + "commit-uncertain state should match");
        if (scenario.rejectManualRetry) {
            manager.retryTask(task.taskId);
            const auto afterRetry = manager.taskSnapshot(task.taskId);
            test.check(afterRetry &&
                           afterRetry->status == scenario.expectedStatus &&
                           afterRetry->attempts ==
                               scenario.expectedAutomaticAttempts &&
                           state->attemptsFor(scenario.remotePath) ==
                               scenario.expectedAutomaticAttempts,
                       label + "manual retry should be rejected");
        }
    }
}

OPENSCP_TEST(testFailedDependencySkipsFollowingWork, test) {
    auto state = authenticationFailureState("/remote/fails");
    FailureDownloadClient baseClient(state);
    const auto options = testOptions();
    TransferManager manager;
    configureManager(manager, baseClient, options);
    QTemporaryDir destination;

    TransferBatchOptions batch;
    batch.sessionKey = QStringLiteral("test-session");
    const quint64 first = manager.enqueueDownload(
        QStringLiteral("/remote/fails"), destination.filePath("fails"), batch);
    test.check(waitForStatus(manager, first, TransferTask::Status::Error),
               "the prerequisite should fail before late dependents arrive");
    batch.dependsOnTaskId = first;
    const quint64 second = manager.enqueueRemoteDelete(
        QStringLiteral("/must-not-delete"), false, batch);
    batch.dependsOnTaskId = second;
    manager.enqueueRemoteDelete(QStringLiteral("/must-not-delete-either"),
                                false, batch);

    test.check(
        waitUntil(
            [&] {
                const auto tasks = manager.tasksSnapshot();
                return tasks.size() == 3 &&
                       tasks[0].status == TransferTask::Status::Error &&
                       tasks[1].status == TransferTask::Status::Skipped &&
                       tasks[1].skippedByFailedDependency &&
                       tasks[2].status == TransferTask::Status::Skipped &&
                       tasks[2].skippedByFailedDependency;
            },
            10'000ms),
        "failed prerequisites should skip late dependency chains");
}

OPENSCP_TEST(testCancelingQueuedPrerequisiteSkipsDependents, test) {
    // Without a connection factory nothing runs, so every task stays queued.
    TransferManager manager;
    manager.setSessionIdentity(QStringLiteral("test-session"));
    TransferBatchOptions batch;
    batch.sessionKey = QStringLiteral("test-session");
    const quint64 first =
        manager.enqueueRemoteDelete(QStringLiteral("/first"), false, batch);
    batch.dependsOnTaskId = first;
    const quint64 second =
        manager.enqueueRemoteDelete(QStringLiteral("/second"), false, batch);
    batch.dependsOnTaskId = second;
    const quint64 third =
        manager.enqueueRemoteDelete(QStringLiteral("/third"), false, batch);
    batch.dependsOnTaskId = 0;
    const quint64 unrelated =
        manager.enqueueRemoteDelete(QStringLiteral("/unrelated"), false, batch);

    manager.cancelTask(first);
    const auto skipped = [&](quint64 taskId) {
        const auto task = manager.taskSnapshot(taskId);
        return task && task->status == TransferTask::Status::Skipped &&
               task->skippedByFailedDependency;
    };
    test.check(skipped(second) && skipped(third),
               "canceling a queued prerequisite should skip its dependents");
    const auto other = manager.taskSnapshot(unrelated);
    test.check(other && other->status == TransferTask::Status::Queued,
               "canceling a task should leave independent work queued");
}

OPENSCP_TEST(testTasksWaitingForBatchRunAfterItSucceeds, test) {
    auto probe = std::make_shared<ConcurrencyProbe>();
    ConcurrentMockClient baseClient(probe);
    TransferManager manager;
    manager.setMaxConcurrent(3);
    configureManager(manager, baseClient, testOptions());
    QTemporaryDir destination;

    TransferBatchOptions batch = testBatchOptions();
    batch.batchId = manager.createBatch(batch);
    const quint64 first = manager.enqueueDownload(
        QStringLiteral("/remote/batch-a"), destination.filePath("a"), batch);
    const quint64 second = manager.enqueueDownload(
        QStringLiteral("/remote/batch-b"), destination.filePath("b"), batch);
    TransferBatchOptions waiting = batch;
    waiting.waitForBatch = true;
    const quint64 last =
        manager.enqueueDownload(QStringLiteral("/remote/batch-last"),
                                destination.filePath("last"), waiting);

    test.check(waitForStatus(manager, last, TransferTask::Status::Done),
               "a task waiting for its batch should run once it succeeds");
    const auto a = manager.taskSnapshot(first);
    const auto b = manager.taskSnapshot(second);
    const auto l = manager.taskSnapshot(last);
    test.check(a && b && l && a->status == TransferTask::Status::Done &&
                   b->status == TransferTask::Status::Done &&
                   l->startedAtMs >= a->finishedAtMs &&
                   l->startedAtMs >= b->finishedAtMs,
               "a task waiting for its batch should start after the rest");
    test.check(probe->maximum.load() == 2,
               "the rest of the batch should still run in parallel");
}

OPENSCP_TEST(testFailedBatchWorkSkipsTasksWaitingForBatch, test) {
    auto state = authenticationFailureState("/remote/fails");
    FailureDownloadClient baseClient(state);
    TransferManager manager;
    configureManager(manager, baseClient, testOptions());
    QTemporaryDir destination;

    TransferBatchOptions batch = testBatchOptions();
    batch.batchId = manager.createBatch(batch);
    const quint64 failing = manager.enqueueDownload(
        QStringLiteral("/remote/fails"), destination.filePath("fails"), batch);
    TransferBatchOptions waiting = batch;
    waiting.waitForBatch = true;
    const quint64 firstDelete = manager.enqueueRemoteDelete(
        QStringLiteral("/must-not-delete"), false, waiting);
    waiting.dependsOnTaskId = firstDelete;
    const quint64 secondDelete = manager.enqueueRemoteDelete(
        QStringLiteral("/must-not-delete-either"), false, waiting);

    const auto skipped = [&](quint64 taskId) {
        const auto task = manager.taskSnapshot(taskId);
        return task && task->status == TransferTask::Status::Skipped &&
               task->skippedByFailedDependency;
    };
    test.check(waitForStatus(manager, failing, TransferTask::Status::Error),
               "the batch work should fail");
    test.check(waitUntil([&] {
                   return skipped(firstDelete) && skipped(secondDelete);
               }),
               "a failure in the batch should skip the tasks waiting for it");

    waiting.dependsOnTaskId = 0;
    const quint64 late =
        manager.enqueueRemoteDelete(QStringLiteral("/late"), false, waiting);
    test.check(skipped(late),
               "a task queued to wait for a failed batch should be skipped");
}

OPENSCP_TEST(testCancelingBatchWorkSkipsTasksWaitingForBatch, test) {
    // Without a connection factory nothing runs, so every task stays queued.
    TransferManager manager;
    manager.setSessionIdentity(QStringLiteral("test-session"));
    const auto skipped = [&](quint64 taskId) {
        const auto task = manager.taskSnapshot(taskId);
        return task && task->status == TransferTask::Status::Skipped &&
               task->skippedByFailedDependency;
    };
    const auto queued = [&](quint64 taskId) {
        const auto task = manager.taskSnapshot(taskId);
        return task && task->status == TransferTask::Status::Queued;
    };

    TransferBatchOptions batch;
    batch.sessionKey = QStringLiteral("test-session");
    batch.batchId = manager.createBatch(batch);
    const quint64 work =
        manager.enqueueRemoteDelete(QStringLiteral("/work"), false, batch);
    batch.waitForBatch = true;
    const quint64 waiter =
        manager.enqueueRemoteDelete(QStringLiteral("/waiter"), false, batch);
    const quint64 otherWaiter = manager.enqueueRemoteDelete(
        QStringLiteral("/other-waiter"), false, batch);

    TransferBatchOptions otherBatch;
    otherBatch.sessionKey = QStringLiteral("test-session");
    otherBatch.batchId = manager.createBatch(otherBatch);
    otherBatch.waitForBatch = true;
    const quint64 canceledWaiter = manager.enqueueRemoteDelete(
        QStringLiteral("/canceled-waiter"), false, otherBatch);
    const quint64 keptWaiter = manager.enqueueRemoteDelete(
        QStringLiteral("/kept-waiter"), false, otherBatch);

    manager.cancelTask(work);
    test.check(skipped(waiter) && skipped(otherWaiter),
               "canceling batch work should skip every task waiting for it");
    manager.cancelTask(canceledWaiter);
    test.check(queued(keptWaiter),
               "tasks waiting for their batch should not wait for each other");
}

OPENSCP_TEST(testBatchWaitersTrackRetryAndRemovedFailure, test) {
    TransferManager manager;
    manager.setSessionIdentity(QStringLiteral("test-session"));
    auto batch = testBatchOptions();
    batch.batchId = manager.createBatch(batch);
    const quint64 work = manager.enqueueRemoteDelete(
        QStringLiteral("/remote/work"), false, batch);

    manager.cancelTask(work);
    manager.retryTask(work);
    batch.waitForBatch = true;
    const quint64 waiting = manager.enqueueRemoteDelete(
        QStringLiteral("/remote/waiting"), false, batch);
    const auto pending = manager.taskSnapshot(waiting);
    test.check(pending && pending->status == TransferTask::Status::Queued,
               "a retried prerequisite should leave batch waiters pending");

    manager.cancelTask(work);
    const auto skipped = manager.taskSnapshot(waiting);
    test.check(skipped && skipped->status == TransferTask::Status::Skipped,
               "a second prerequisite failure should skip its waiter");
    manager.removeTask(work);

    const quint64 late = manager.enqueueRemoteDelete(
        QStringLiteral("/remote/late"), false, batch);
    const auto ready = manager.taskSnapshot(late);
    test.check(ready && ready->status == TransferTask::Status::Queued,
               "removing failed batch work should clear its failure state");

    ConcurrentMockClient baseClient(std::make_shared<ConcurrencyProbe>());
    configureManager(manager, baseClient, testOptions());
    test.check(waitForStatus(manager, late, TransferTask::Status::Done),
               "a waiter should run once no batch work remains");
}

OPENSCP_TEST(testDependencySkipsKeepTerminalCounterAndHistoryBounded, test) {
    auto state = authenticationFailureState("/remote/root-failure");
    FailureDownloadClient baseClient(state);
    TransferManager manager;
    manager.pauseAll();
    manager.setMaxConcurrent(1);
    configureManager(manager, baseClient, testOptions());
    QTemporaryDir destination;

    auto batch = testBatchOptions();
    const quint64 prerequisite =
        manager.enqueueDownload(QStringLiteral("/remote/root-failure"),
                                destination.filePath("root-failure"), batch);
    batch.dependsOnTaskId = prerequisite;
    QVector<QPair<QString, QString>> dependent;
    dependent.reserve(5501);
    for (int index = 0; index < 5501; ++index) {
        dependent.push_back(
            {QStringLiteral("/remote/dependent-%1").arg(index),
             destination.filePath(QStringLiteral("dependent-%1").arg(index))});
    }
    manager.enqueueDownloads(dependent, batch);
    manager.resumeAll();

    test.check(
        waitUntil(
            [&] {
                const auto tasks = manager.tasksSnapshot();
                return tasks.size() == 5000 &&
                       std::all_of(
                           tasks.cbegin(), tasks.cend(),
                           [](const TransferTask &task) {
                               return task.status ==
                                          TransferTask::Status::Skipped ||
                                      task.status ==
                                          TransferTask::Status::Error;
                           });
            },
            8000ms),
        "dependency skips should count once and prune to 5000 terminals");
    test.check(state->attemptsFor("/remote/root-failure") == 1,
               "skipped dependents must not execute after their prerequisite");
}

class RateMockClient final : public DownloadMockClient {
    public:
    bool get(const std::string &, const std::string &, std::string &err,
             std::function<void(std::size_t, std::size_t)> progress,
             std::function<bool()>, bool) override {
        if (progress)
            progress(32 * 1024, 32 * 1024);
        err.clear();
        return true;
    }

    std::unique_ptr<openscp::RemoteClient>
    openConnection(const openscp::SessionOptions &options, std::string &err) {
        return makeConnectedWorker<RateMockClient>(options, err);
    }
};

OPENSCP_TEST(testAggregateRateLimit, test) {
    RateMockClient baseClient;
    const auto options = testOptions();
    TransferManager manager;
    manager.setMaxConcurrent(2);
    manager.setGlobalSpeedLimitKBps(64);
    configureManager(manager, baseClient, options);
    QTemporaryDir destination;
    auto batch = testBatchOptions();

    const auto started = std::chrono::steady_clock::now();
    manager.enqueueDownloads(
        {{QStringLiteral("/remote/rate-a"), destination.filePath("rate-a")},
         {QStringLiteral("/remote/rate-b"), destination.filePath("rate-b")}},
        batch);
    const bool completed = waitUntil(
        [&] {
            const auto tasks = manager.tasksSnapshot();
            return tasks.size() == 2 &&
                   tasks[0].status == TransferTask::Status::Done &&
                   tasks[1].status == TransferTask::Status::Done;
        },
        4000ms);
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    test.check(completed, "rate-limited transfers should complete");
    test.check(elapsed >= 600ms,
               "global limit should cap aggregate, not each worker");
    test.check(elapsed < 3000ms,
               "token bucket should retain a bounded initial burst");
}

OPENSCP_TEST(testBatchCancellationAndDirectoryTasks, test) {
    TransferManager manager;
    TransferBatchOptions batch;
    batch.batchId = manager.createBatch(batch);
    manager.enqueueDownloads(
        {{QStringLiteral("/remote/a"), QStringLiteral("/local/a")},
         {QStringLiteral("/remote/b"), QStringLiteral("/local/b")}},
        batch);
    manager.cancelBatch(batch.batchId);
    const auto canceled = manager.tasksSnapshot();
    test.check(std::all_of(canceled.cbegin(), canceled.cend(),
                           [](const TransferTask &task) {
                               return task.status ==
                                      TransferTask::Status::Canceled;
                           }),
               "cancelBatch should cancel every non-terminal task");

    auto probe = std::make_shared<ConcurrencyProbe>();
    ConcurrentMockClient baseClient(probe);
    const auto options = testOptions();
    TransferManager directoryManager;
    configureManager(directoryManager, baseClient, options);
    QTemporaryDir root;
    const QString emptyDirectory = root.filePath("empty/child");
    TransferBatchOptions directoryBatch;
    directoryBatch.sessionKey = QStringLiteral("test-session");
    directoryManager.enqueueLocalDirectory(emptyDirectory, directoryBatch);
    test.check(waitForStatus(directoryManager, 1, TransferTask::Status::Done) &&
                   QFileInfo(emptyDirectory).isDir(),
               "empty local folders should be explicit queue tasks");
}

OPENSCP_TEST(testLocalDirectoryTaskRejectsSymlinkParent, test) {
#ifndef _WIN32
    auto probe = std::make_shared<ConcurrencyProbe>();
    ConcurrentMockClient baseClient(probe);
    TransferManager manager;
    configureManager(manager, baseClient, testOptions());
    QTemporaryDir selected;
    QTemporaryDir outside;
    test.check(selected.isValid() && outside.isValid(),
               "local directory fixtures should initialize");
    const QString linked = selected.filePath("linked");
    test.check(QFile::link(outside.path(), linked),
               "a parent directory symlink should be created");

    TransferBatchOptions batch;
    batch.sessionKey = QStringLiteral("test-session");
    manager.enqueueLocalDirectory(QDir(linked).filePath("created"), batch);
    test.check(waitForStatus(manager, 1, TransferTask::Status::Error) &&
                   !QFileInfo(outside.filePath("created")).exists(),
               "a local directory task must not create outside its chosen "
               "folder through a symlink");
#endif
}

OPENSCP_TEST(testPersistentDeletionTasks, test) {
    auto probe = std::make_shared<ConcurrencyProbe>();
    ConcurrentMockClient baseClient(probe);
    const auto options = testOptions();
    TransferManager manager;
    manager.setMaxConcurrent(1);
    configureManager(manager, baseClient, options);

    QTemporaryDir root;
    const QString directory = root.filePath("obsolete");
    const QString filePath = QDir(directory).filePath("old.txt");
    test.check(QDir().mkpath(directory),
               "delete fixture directory should exist");
    QFile file(filePath);
    test.check(file.open(QIODevice::WriteOnly),
               "delete fixture file should be writable");
    file.write("obsolete");
    file.close();

    TransferBatchOptions batch;
    batch.sessionKey = QStringLiteral("test-session");
    batch.batchId = manager.createBatch(batch);
    manager.enqueueLocalDelete(filePath, false, batch);
    manager.enqueueLocalDelete(directory, true, batch);

    test.check(waitUntil([&] {
                   const auto tasks = manager.tasksSnapshot();
                   return tasks.size() == 2 &&
                          std::all_of(tasks.cbegin(), tasks.cend(),
                                      [](const TransferTask &task) {
                                          return task.status ==
                                                 TransferTask::Status::Done;
                                      });
               }),
               "postordered local deletion tasks should complete");
    test.check(!QFileInfo::exists(filePath) && !QFileInfo::exists(directory),
               "persistent deletion tasks should remove their targets");
}

OPENSCP_TEST(testTerminalHistoryIsBounded, test) {
    TransferManager manager;
    const auto enqueueFinished = [&manager](int first, int count) {
        QVector<QPair<QString, QString>> downloads;
        downloads.reserve(count);
        for (int index = first; index < first + count; ++index) {
            downloads.push_back(
                {QStringLiteral("/remote/history-%1").arg(index),
                 QStringLiteral("/local/history-%1").arg(index)});
        }
        manager.enqueueDownloads(downloads);
        manager.cancelAll();
    };

    enqueueFinished(0, 5500);
    test.check(manager.tasksSnapshot().size() == 5500,
               "history should wait for a whole prune batch past its bound");
    enqueueFinished(5500, 1);
    const auto tasks = manager.tasksSnapshot();
    test.check(tasks.size() == 5000,
               "terminal queue history should be pruned to 5000 tasks");
    test.check(tasks.front().taskId == 502 && tasks.back().taskId == 5501,
               "history pruning should retain the newest terminal tasks");
    test.check(!manager.taskSnapshot(501) && manager.taskSnapshot(502) &&
                   manager.taskSnapshot(5501),
               "pruning should keep the task index in step with the queue");
    enqueueFinished(5501, 1);
    test.check(manager.tasksSnapshot().size() == 5001,
               "a pruned history should grow again before the next prune");
}

OPENSCP_TEST(testRemovingSelectedTasksReportsThemOnce, test) {
    // Without a connection factory nothing runs, so every task stays queued.
    TransferManager manager;
    manager.setSessionIdentity(QStringLiteral("test-session"));
    TransferBatchOptions batch;
    batch.sessionKey = QStringLiteral("test-session");
    QVector<quint64> ids;
    for (int index = 0; index < 6; ++index) {
        ids.push_back(manager.enqueueRemoteDelete(
            QStringLiteral("/remove-%1").arg(index), false, batch));
    }
    QVector<QVector<quint64>> removedSignals;
    QObject::connect(&manager, &TransferManager::tasksRemoved, &manager,
                     [&](const QVector<quint64> &removedIds) {
                         removedSignals.push_back(removedIds);
                     });

    manager.removeTasks({ids[1], ids[3], ids[4], ids[3], 999'999});
    test.check(removedSignals.size() == 1 &&
                   removedSignals.front() ==
                       QVector<quint64>{ids[1], ids[3], ids[4]},
               "removing a selection should report each task once");
    const auto remaining = manager.tasksSnapshot();
    test.check(remaining.size() == 3 && remaining[0].taskId == ids[0] &&
                   remaining[1].taskId == ids[2] &&
                   remaining[2].taskId == ids[5],
               "removing a selection should keep the rest in order");
    manager.cancelTask(ids[5]);
    const auto canceled = manager.taskSnapshot(ids[5]);
    test.check(!manager.taskSnapshot(ids[3]) && canceled &&
                   canceled->status == TransferTask::Status::Canceled,
               "the task index should follow the removal");
}

OPENSCP_TEST(testRemovingSelectedTasksKeepsRunningWork, test) {
    auto probe = std::make_shared<LifecycleProbe>();
    CancelLifecycleClient baseClient(probe);
    TransferManager manager;
    manager.setMaxConcurrent(1);
    configureManager(manager, baseClient, testOptions());
    QTemporaryDir destination;
    const auto batch = testBatchOptions();

    const quint64 running =
        manager.enqueueDownload(QStringLiteral("/remote/running"),
                                destination.filePath("running"), batch);
    const quint64 queued =
        manager.enqueueDownload(QStringLiteral("/remote/queued"),
                                destination.filePath("queued"), batch);
    test.check(waitUntil([&] { return probe->gets.load() == 1; }),
               "the task to keep should start running");
    manager.removeTasks({running, queued});
    test.check(manager.taskSnapshot(running) && !manager.taskSnapshot(queued),
               "removing a selection must leave running tasks in place");
    manager.cancelTask(running);
    test.check(waitForStatus(manager, running, TransferTask::Status::Canceled),
               "the kept task should still be controllable");
}

OPENSCP_TEST(testRemovingTaskCanDeletePartialData, test) {
    QTemporaryDir root;
    const QString destination = root.filePath("partial.dat");
    QFile partial(destination + QStringLiteral(".part"));
    test.check(partial.open(QIODevice::WriteOnly),
               "partial-data fixture should be writable");
    partial.write("partial");
    partial.close();

    TransferManager manager;
    qsizetype removedSignals = 0;
    QObject::connect(
        &manager, &TransferManager::tasksRemoved, &manager,
        [&](const QVector<quint64> &ids) { removedSignals += ids.size(); });
    manager.enqueueDownload(QStringLiteral("/remote/partial.dat"), destination);
    manager.removeTask(1, true);
    test.check(manager.tasksSnapshot().isEmpty() &&
                   !QFile::exists(destination + QStringLiteral(".part")),
               "removing a task with partial data should delete both");
    test.check(removedSignals == 1,
               "removing a task should emit its granular removal");
}

OPENSCP_TEST(testRemovingTaskPreservesActivePartialData, test) {
    auto probe = std::make_shared<LifecycleProbe>();
    CancelLifecycleClient baseClient(probe);
    TransferManager manager;
    manager.setMaxConcurrent(2);
    configureManager(manager, baseClient, testOptions());
    QTemporaryDir root;
    const QString destination = root.filePath("file");
    const auto batch = testBatchOptions();
    const quint64 active = manager.enqueueDownload(
        QStringLiteral("/remote/active"), destination, batch);
    test.check(waitUntil([&] { return probe->gets.load() == 1; }),
               "the active download should own its partial path");
    QFile partial(destination + QStringLiteral(".part"));
    test.check(partial.open(QIODevice::WriteOnly),
               "the active partial fixture should be writable");
    partial.write("active download");
    partial.close();
    const quint64 queued = manager.enqueueDownload(
        QStringLiteral("/remote/queued"), destination, batch);
    manager.removeTask(queued, true);
    test.check(
        partial.open(QIODevice::ReadOnly) &&
            partial.readAll() == "active download",
        "removing an inactive task must not unlink another task's partial");
    partial.close();
    manager.cancelTask(active);
    test.check(waitForStatus(manager, active, TransferTask::Status::Canceled),
               "the active download should remain cancellable");
    // Canceled is observable before the worker releases its reservations.
    manager.shutdown();
    manager.removeTask(active, true);
    test.check(!QFile::exists(partial.fileName()),
               "partial cleanup should work after the active reservation ends");
}

struct RemotePartialProbe {
    std::mutex mutex;
    std::vector<std::string> removedPaths;
};

class RemotePartialCleanupClient final : public openscp::MockSftpClient {
    public:
    explicit RemotePartialCleanupClient(
        std::shared_ptr<RemotePartialProbe> probe)
        : probe_(std::move(probe)) {}

    bool removeFile(const std::string &remote, std::string &err) override {
        {
            std::lock_guard<std::mutex> lock(probe_->mutex);
            probe_->removedPaths.push_back(remote);
        }
        clearLastOperationError();
        err.clear();
        return true;
    }

    std::unique_ptr<openscp::RemoteClient>
    openConnection(const openscp::SessionOptions &options, std::string &err) {
        return makeConnectedWorker<RemotePartialCleanupClient>(options, err,
                                                               probe_);
    }

    private:
    std::shared_ptr<RemotePartialProbe> probe_;
};

OPENSCP_TEST(testRemovingUploadCanQueueRemotePartialCleanup, test) {
    auto probe = std::make_shared<RemotePartialProbe>();
    TransferManager manager;
    TransferBatchOptions batch;
    batch.sessionKey = QStringLiteral("test-session");
    const quint64 uploadId =
        manager.enqueueUpload(QStringLiteral("/local/upload"),
                              QStringLiteral("/remote/upload"), batch);
    manager.removeTask(uploadId, true);
    const auto queued = manager.tasksSnapshot();
    test.check(
        queued.size() == 1 &&
            queued.front().type == TransferTask::Type::DeleteRemoteFile &&
            queued.front().dst == QStringLiteral("/remote/upload.part"),
        "removing an upload with partial data should queue remote cleanup");

    RemotePartialCleanupClient baseClient(probe);
    manager.setMaxConcurrent(1);
    configureManager(manager, baseClient, testOptions());
    test.check(waitUntil([&] {
                   const auto task = manager.tasksSnapshot();
                   return task.size() == 1 &&
                          task.front().status == TransferTask::Status::Done;
               }),
               "remote partial cleanup should execute through a worker");
    std::lock_guard<std::mutex> lock(probe->mutex);
    test.check(probe->removedPaths.size() == 1 &&
                   probe->removedPaths.front() == "/remote/upload.part",
               "remote cleanup should target the deterministic .part path");
}

struct RemoteLookupProbe {
    std::mutex mutex;
    std::vector<std::string> exists;
    std::vector<std::string> stats;
};

// Records remote lookups. Every directory exists and no upload destination
// does.
class RemoteLookupClient final : public openscp::MockSftpClient {
    public:
    explicit RemoteLookupClient(std::shared_ptr<RemoteLookupProbe> probe)
        : probe_(std::move(probe)) {}

    bool exists(const std::string &remote, bool &isDir,
                std::string &err) override {
        record(probe_->exists, remote);
        isDir = true;
        clearLastOperationError();
        err.clear();
        return true;
    }

    bool stat(const std::string &remote, openscp::FileInfo &info,
              std::string &err) override {
        record(probe_->stats, remote);
        info = openscp::FileInfo{};
        clearLastOperationError();
        err.clear();
        return false;
    }

    bool put(const std::string &, const std::string &, std::string &err,
             std::function<void(std::size_t, std::size_t)>,
             std::function<bool()>, bool) override {
        clearLastOperationError();
        err.clear();
        return true;
    }

    std::unique_ptr<openscp::RemoteClient>
    openConnection(const openscp::SessionOptions &options, std::string &err) {
        return makeConnectedWorker<RemoteLookupClient>(options, err, probe_);
    }

    private:
    void record(std::vector<std::string> &calls, const std::string &remote) {
        std::lock_guard<std::mutex> lock(probe_->mutex);
        calls.push_back(remote);
    }

    std::shared_ptr<RemoteLookupProbe> probe_;
};

OPENSCP_TEST(testUploadPrecheckReusesCompletedParentDirectory, test) {
    auto probe = std::make_shared<RemoteLookupProbe>();
    RemoteLookupClient baseClient(probe);
    TransferManager manager;
    manager.setMaxConcurrent(1);
    configureManager(manager, baseClient, testOptions());

    TransferBatchOptions batch = testBatchOptions();
    const quint64 parent =
        manager.enqueueRemoteDirectory(QStringLiteral("/remote/dir"), batch);
    batch.dependsOnTaskId = parent;
    const quint64 child =
        manager.enqueueUpload(QStringLiteral("/local/child.txt"),
                              QStringLiteral("/remote/dir/child.txt"), batch);
    const quint64 other =
        manager.enqueueRemoteDirectory(QStringLiteral("/remote/other"), batch);
    batch.dependsOnTaskId = other;
    const quint64 unrelated = manager.enqueueUpload(
        QStringLiteral("/local/unrelated.txt"),
        QStringLiteral("/remote/dir/unrelated.txt"), batch);

    test.check(
        waitForStatus(manager, child, TransferTask::Status::Done) &&
            waitForStatus(manager, unrelated, TransferTask::Status::Done),
        "uploads with directory dependencies should complete");

    std::lock_guard<std::mutex> lock(probe->mutex);
    const auto calls = [](const std::vector<std::string> &recorded,
                          const std::string &path) {
        return std::count(recorded.begin(), recorded.end(), path);
    };
    test.check(calls(probe->stats, "/remote/dir/child.txt") == 1 &&
                   calls(probe->exists, "/remote/dir/child.txt") == 0,
               "the upload conflict check should use one stat");
    // Only the upload whose dependency created another directory walks the
    // parent path.
    test.check(calls(probe->exists, "/remote") == 1 &&
                   calls(probe->exists, "/remote/dir") == 2,
               "a completed parent directory task should skip the path walk");
}

TransferTask persistableTask(quint64 taskId, const QString &destination) {
    TransferTask task;
    task.taskId = taskId;
    task.batchId = 1;
    task.type = TransferTask::Type::Download;
    task.sessionKey = QStringLiteral("writer-session");
    task.src = QStringLiteral("/remote/source");
    task.dst = destination;
    task.queuedAtMs = 1;
    task.status = TransferTask::Status::Paused;
    return task;
}

QStringList savedDestinations(const QString &path) {
    QStringList destinations;
    const auto loaded =
        TransferQueuePersistence::load(path, QStringLiteral("writer-session"));
    for (const TransferTask &task : loaded.tasks)
        destinations.push_back(task.dst);
    return destinations;
}

class SlowProgressClient final : public DownloadMockClient {
    public:
    explicit SlowProgressClient(std::shared_ptr<std::atomic_bool> reporting)
        : reporting_(std::move(reporting)) {}

    bool get(const std::string &, const std::string &, std::string &err,
             std::function<void(std::size_t, std::size_t)> progress,
             std::function<bool()> shouldCancel, bool) override {
        for (std::size_t chunk = 1; chunk <= 2000; ++chunk) {
            if (shouldCancel && shouldCancel()) {
                err = "Canceled";
                return false;
            }
            if (progress)
                progress(chunk * 1024, 2000 * 1024);
            reporting_->store(true);
            std::this_thread::sleep_for(5ms);
        }
        err.clear();
        return true;
    }

    std::unique_ptr<openscp::RemoteClient>
    openConnection(const openscp::SessionOptions &options, std::string &err) {
        return makeConnectedWorker<SlowProgressClient>(options, err,
                                                       reporting_);
    }

    private:
    std::shared_ptr<std::atomic_bool> reporting_;
};

OPENSCP_TEST(testReportedProgressDoesNotDelayQueueSaves, test) {
    auto reporting = std::make_shared<std::atomic_bool>(false);
    SlowProgressClient baseClient(reporting);
    QTemporaryDir root;
    const QString queuePath = root.filePath("progress-queue.json");
    TransferManager manager;
    manager.setMaxConcurrent(1);
    test.check(manager.enablePersistence(queuePath),
               "new persistence file should be accepted");
    configureManager(manager, baseClient, testOptions());

    auto batch = testBatchOptions();
    manager.enqueueDownload(QStringLiteral("/remote/progress"),
                            root.filePath("progress.bin"), batch);
    test.check(waitUntil([&] { return reporting->load(); }),
               "the transfer should start reporting progress");
    // Reported progress must not push the save past its usual short wait,
    // which is well under the deadline that would let it through anyway.
    test.check(
        waitUntil([&] { return QFileInfo::exists(queuePath); }, 1200ms) &&
            reporting->load(),
        "a queue change should be saved while progress is reported");
}

OPENSCP_TEST(testBusyQueueIsSavedWithinTheDeadline, test) {
    QTemporaryDir root;
    const QString queuePath = root.filePath("busy-queue.json");
    TransferManager manager;
    manager.setSessionIdentity(QStringLiteral("test-session"));
    test.check(manager.enablePersistence(queuePath),
               "new persistence file should be accepted");
    TransferBatchOptions batch;
    batch.sessionKey = QStringLiteral("test-session");

    // Queue changes every 50 ms keep restarting the save timer; the deadline
    // has to let a save through anyway.
    const auto started = std::chrono::steady_clock::now();
    bool saved = false;
    while (!saved && std::chrono::steady_clock::now() - started < 4000ms) {
        manager.enqueueRemoteDelete(QStringLiteral("/busy"), false, batch);
        saved = waitUntil([&] { return QFileInfo::exists(queuePath); }, 50ms);
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    test.check(saved, "a queue that keeps changing should still be saved");
    test.check(elapsed < 3500ms,
               "the save should not wait much past its deadline");
}

OPENSCP_TEST(testQueueWriterWritesTheNewestSnapshot, test) {
    QTemporaryDir root;
    const QString path = root.filePath("writer.json");
    {
        TransferQueueWriter writer;
        writer.setPath(path);
        writer.saveAndWait({persistableTask(1, QStringLiteral("/first"))});
        test.check(savedDestinations(path) ==
                       QStringList{QStringLiteral("/first")},
                   "saveAndWait should leave the snapshot on disk");

        writer.save({persistableTask(2, QStringLiteral("/queued"))});
        writer.saveAndWait({persistableTask(3, QStringLiteral("/newest"))});
        test.check(savedDestinations(path) ==
                       QStringList{QStringLiteral("/newest")},
                   "the newest snapshot should win over a waiting one");
    }
    test.check(savedDestinations(path) ==
                   QStringList{QStringLiteral("/newest")},
               "closing the writer should leave the last snapshot in place");
}

OPENSCP_TEST(testQueueWriterFinishesPendingWorkOnShutdown, test) {
    QTemporaryDir root;
    const QString path = root.filePath("shutdown.json");
    TransferQueueWriter writer;
    writer.setPath(path);
    writer.save({persistableTask(1, QStringLiteral("/pending"))});
    writer.shutdown();
    test.check(savedDestinations(path) ==
                   QStringList{QStringLiteral("/pending")},
               "a pending snapshot should still be written while stopping");

    writer.save({persistableTask(2, QStringLiteral("/after-shutdown"))});
    writer.saveAndWait({persistableTask(3, QStringLiteral("/also-after"))});
    test.check(savedDestinations(path) ==
                   QStringList{QStringLiteral("/pending")},
               "snapshots handed over after shutdown should be dropped");
}

OPENSCP_TEST(testQueueWriterReportsFailures, test) {
    QTemporaryDir root;
    const QString unwritable = root.filePath("file-as-directory/queue.json");
    QFile blocker(root.filePath("file-as-directory"));
    test.check(blocker.open(QIODevice::WriteOnly),
               "the failure fixture should be writable");
    blocker.close();

    std::mutex warningMutex;
    QString warning;
    TransferQueueWriter writer;
    writer.setWarningHandler([&](const QString &reported) {
        std::lock_guard<std::mutex> lock(warningMutex);
        warning = reported;
    });
    writer.setPath(unwritable);
    writer.saveAndWait({persistableTask(1, QStringLiteral("/nowhere"))});
    test.check(waitUntil([&] {
                   std::lock_guard<std::mutex> lock(warningMutex);
                   return !warning.isEmpty();
               }),
               "a snapshot that cannot be written should report a warning");
}

OPENSCP_TEST(testPausedQueuePersistence, test) {
    QTemporaryDir root;
    const QString queuePath = root.filePath("transfer-queue-v1.json");
    {
        TransferManager manager;
        test.check(manager.enablePersistence(queuePath),
                   "new persistence file should be accepted");
        TransferBatchOptions batch;
        batch.sessionKey = QStringLiteral("saved-site-id");
        batch.operation = TransferOperation::Move;
        manager.enqueueDownload(QStringLiteral("/remote/persist.dat"),
                                root.filePath("persist.dat"), batch);
        manager.persistNow();
    }

    QFile queueFile(queuePath);
    test.check(queueFile.open(QIODevice::ReadOnly),
               "queue persistence should create a readable file");
    const QByteArray stored = queueFile.readAll();
    queueFile.close();
    test.check(stored.contains("\"schemaVersion\":1"),
               "queue persistence should be schema-versioned");
    test.check(!stored.contains("password") && !stored.contains("passphrase"),
               "queue persistence must not contain credentials");
    const auto permissions = QFile::permissions(queuePath);
    test.check(permissions.testFlag(QFileDevice::ReadOwner) &&
                   permissions.testFlag(QFileDevice::WriteOwner) &&
                   !permissions.testFlag(QFileDevice::ReadGroup) &&
                   !permissions.testFlag(QFileDevice::ReadOther),
               "queue persistence should use owner-only permissions");

    TransferManager restored;
    test.check(restored.enablePersistence(queuePath),
               "valid queue persistence should restore");
    const auto tasks = restored.tasksSnapshot();
    test.check(tasks.size() == 1 &&
                   tasks.front().status == TransferTask::Status::Paused &&
                   tasks.front().restored,
               "restored work should always start paused");
    test.check(tasks.front().operation == TransferOperation::Move &&
                   tasks.front().phase == TransferPhase::Transfer,
               "persistent tasks should retain move phase metadata");
}

OPENSCP_TEST(testBatchWaitingPersistence, test) {
    QTemporaryDir root;
    const QString queuePath = root.filePath("transfer-queue-v1.json");
    {
        TransferManager manager;
        test.check(manager.enablePersistence(queuePath),
                   "new persistence file should be accepted");
        TransferBatchOptions batch;
        batch.sessionKey = QStringLiteral("saved-site-id");
        batch.batchId = manager.createBatch(batch);
        manager.enqueueRemoteDelete(QStringLiteral("/remote/work"), false,
                                    batch);
        batch.waitForBatch = true;
        manager.enqueueRemoteDelete(QStringLiteral("/remote/after"), false,
                                    batch);
        manager.persistNow();
    }

    QFile queueFile(queuePath);
    test.check(queueFile.open(QIODevice::ReadOnly),
               "queue persistence should create a readable file");
    const QByteArray stored = queueFile.readAll();
    queueFile.close();
    test.check(stored.contains("\"schemaVersion\":2") &&
                   stored.contains("\"waitsForBatch\":true"),
               "tasks waiting for their batch should need the newer schema");

    TransferManager restored;
    test.check(restored.enablePersistence(queuePath),
               "a queue with tasks waiting for their batch should restore");
    const auto tasks = restored.tasksSnapshot();
    test.check(tasks.size() == 2 && !tasks[0].waitsForBatch &&
                   tasks[1].waitsForBatch,
               "restored tasks should keep waiting for their batch");
    if (tasks.size() != 2)
        return;

    restored.setSessionIdentity(QStringLiteral("saved-site-id"));
    ConcurrentMockClient baseClient(std::make_shared<ConcurrencyProbe>());
    const auto options = testOptions();
    restored.setConnectionFactory([&baseClient, options](std::string &error) {
        return baseClient.openConnection(options, error);
    });
    restored.resumeTask(tasks[1].taskId);
    restored.resumeTask(tasks[0].taskId);
    test.check(
        waitForStatus(restored, tasks[1].taskId, TransferTask::Status::Done),
        "restored batch waiter should run after its prerequisite");
    const auto work = restored.taskSnapshot(tasks[0].taskId);
    const auto waiter = restored.taskSnapshot(tasks[1].taskId);
    test.check(work && waiter && work->status == TransferTask::Status::Done &&
                   waiter->startedAtMs >= work->finishedAtMs,
               "restored batch counts should preserve execution order");
}

OPENSCP_TEST(testDirectoryTaskPersistence, test) {
    QTemporaryDir root;
    const QString queuePath = root.filePath("transfer-queue-v1.json");
    const QString localDirectory = root.filePath("empty/local");
    {
        TransferManager manager;
        test.check(manager.enablePersistence(queuePath),
                   "directory persistence fixture should initialize");
        TransferBatchOptions batch;
        batch.sessionKey = QStringLiteral("saved-site-id");
        manager.enqueueLocalDirectory(localDirectory, batch);
        manager.enqueueRemoteDirectory(QStringLiteral("/empty/remote"), batch);
        manager.persistNow();
    }

    TransferManager restored;
    test.check(restored.enablePersistence(queuePath),
               "directory queue tasks should restore");
    const auto tasks = restored.tasksSnapshot();
    test.check(tasks.size() == 2,
               "both local and remote directory tasks should be persisted");
    if (tasks.size() == 2) {
        test.check(tasks[0].type == TransferTask::Type::CreateLocalDirectory &&
                       tasks[0].dst == localDirectory,
                   "local directory task should round-trip without a source");
        test.check(tasks[1].type == TransferTask::Type::CreateRemoteDirectory &&
                       tasks[1].dst == QStringLiteral("/empty/remote"),
                   "remote directory task should round-trip without a source");
    }
}

OPENSCP_TEST(testDeleteTaskPersistence, test) {
    QTemporaryDir root;
    const QString queuePath = root.filePath("transfer-queue-v1.json");
    {
        TransferManager manager;
        test.check(manager.enablePersistence(queuePath),
                   "delete persistence fixture should initialize");
        TransferBatchOptions batch;
        batch.sessionKey = QStringLiteral("saved-site-id");
        batch.batchId = manager.createBatch(batch);
        const quint64 first = manager.enqueueRemoteDelete(
            QStringLiteral("/obsolete/file"), false, batch);
        batch.dependsOnTaskId = first;
        manager.enqueueRemoteDelete(QStringLiteral("/obsolete"), true, batch);
        manager.persistNow();
    }

    TransferManager restored;
    test.check(restored.enablePersistence(queuePath),
               "delete queue tasks should restore");
    const auto tasks = restored.tasksSnapshot();
    test.check(tasks.size() == 2,
               "both remote deletion task kinds should be persisted");
    if (tasks.size() == 2) {
        test.check(tasks[0].type == TransferTask::Type::DeleteRemoteFile &&
                       tasks[1].type ==
                           TransferTask::Type::DeleteRemoteDirectory,
                   "remote deletion task types should round-trip");
        test.check(tasks[1].dependsOnTaskId == tasks[0].taskId,
                   "ordered deletion dependencies should round-trip");
    }
}

OPENSCP_TEST(testCorruptPersistenceIsPreserved, test) {
    QTemporaryDir root;
    const QString queuePath = root.filePath("transfer-queue-v1.json");
    QFile file(queuePath);
    test.check(file.open(QIODevice::WriteOnly),
               "corrupt queue fixture should be writable");
    const QByteArray corrupt("{ definitely-not-json");
    file.write(corrupt);
    file.close();

    TransferManager manager;
    test.check(!manager.enablePersistence(queuePath),
               "corrupt persistence should fail closed");
    manager.enqueueDownload(QStringLiteral("/remote/new"),
                            root.filePath("new"));
    manager.persistNow();

    test.check(file.open(QIODevice::ReadOnly) && file.readAll() == corrupt,
               "corrupt persistence should never be overwritten");
}

OPENSCP_TEST(testFuturePersistenceIsPreserved, test) {
    QTemporaryDir root;
    const QString queuePath = root.filePath("transfer-queue-v1.json");
    QFile file(queuePath);
    const QByteArray future(
        "{\"schemaVersion\":3,\"tasks\":[{\"future\":true}]}");
    test.check(file.open(QIODevice::WriteOnly),
               "future queue fixture should be writable");
    file.write(future);
    file.close();

    TransferManager manager;
    test.check(!manager.enablePersistence(queuePath),
               "future persistence schema should fail closed");
    manager.enqueueDownload(QStringLiteral("/remote/new"),
                            root.filePath("new"));
    manager.persistNow();
    test.check(file.open(QIODevice::ReadOnly) && file.readAll() == future,
               "future persistence schema should never be overwritten");
}

OPENSCP_TEST(testStructurallyInvalidPersistenceFailsClosed, test) {
    QTemporaryDir root;
    const QString queuePath = root.filePath("transfer-queue-v1.json");
    const std::array<QByteArray, 5> invalidDocuments{
        QByteArray(
            R"({"schemaVersion":1,"tasks":[{"id":"1","batchId":"1","type":"download","source":"/a","destination":"/b","operation":"copy","conflictPolicy":"ask","phase":"transfer"},{"id":"1","batchId":"1","type":"download","source":"/c","destination":"/d","operation":"copy","conflictPolicy":"ask","phase":"transfer"}]})"),
        QByteArray(
            R"({"schemaVersion":1,"tasks":[{"id":1.5,"batchId":"1","type":"download","source":"/a","destination":"/b","operation":"copy","conflictPolicy":"ask","phase":"transfer"}]})"),
        QByteArray(
            R"({"schemaVersion":1,"tasks":[{"id":"1","batchId":"1","type":"future-task","source":"/a","destination":"/b","operation":"copy","conflictPolicy":"ask","phase":"transfer"}]})"),
        QByteArray(
            R"({"schemaVersion":1,"tasks":[{"id":1,"batchId":"1","type":"download","sessionKey":"site","source":"/a","destination":"/b","resumeHint":false,"speedLimitKBps":0,"attempts":0,"maxAttempts":3,"operation":"copy","conflictPolicy":"ask","phase":"transfer","commitUncertain":false,"queuedAtMs":"1"}]})"),
        QByteArray(
            R"({"schemaVersion":1,"tasks":[{"id":"1","batchId":"1","type":"download","sessionKey":"site","source":"/a","destination":"/b","resumeHint":false,"speedLimitKBps":0,"attempts":0,"maxAttempts":3,"conflictPolicy":"ask","phase":"transfer","commitUncertain":false,"queuedAtMs":"1"}]})")};

    for (const QByteArray &invalid : invalidDocuments) {
        QFile file(queuePath);
        test.check(file.open(QIODevice::WriteOnly | QIODevice::Truncate),
                   "invalid queue fixture should be writable");
        file.write(invalid);
        file.close();

        TransferManager manager;
        test.check(!manager.enablePersistence(queuePath),
                   "invalid queue structure should fail closed without "
                   "throwing");
        test.check(manager.tasksSnapshot().isEmpty(),
                   "an invalid queue must not be partially restored");
        manager.enqueueDownload(QStringLiteral("/remote/new"),
                                root.filePath("new"));
        manager.persistNow();
        test.check(file.open(QIODevice::ReadOnly) && file.readAll() == invalid,
                   "invalid queue persistence must remain untouched");
    }
}

OPENSCP_TEST(testPersistenceUsesDebouncedAutomaticSave, test) {
    QTemporaryDir root;
    const QString queuePath = root.filePath("transfer-queue-v1.json");
    TransferManager manager;
    test.check(manager.enablePersistence(queuePath),
               "debounce persistence fixture should initialize");
    manager.enqueueDownload(QStringLiteral("/remote/debounced"),
                            root.filePath("debounced"));
    test.check(!QFileInfo::exists(queuePath),
               "enqueue should not synchronously write the queue file");
    test.check(waitUntil([&] { return QFileInfo::exists(queuePath); }, 1500ms),
               "the 250 ms debounce should automatically persist the queue");
}

OPENSCP_TEST(testShutdownUnblocksConcurrentClearSessionWithoutRace, test) {
    auto probe = std::make_shared<LifecycleProbe>();
    CancelLifecycleClient baseClient(probe);
    auto manager = std::make_unique<TransferManager>();
    manager->setMaxConcurrent(1);
    configureManager(*manager, baseClient, testOptions());
    QTemporaryDir destination;

    const quint64 taskId = manager->enqueueDownload(
        QStringLiteral("/remote/shutdown-race"),
        destination.filePath("shutdown"), testBatchOptions());
    test.check(taskId > 0, "enqueued task ID should be valid");
    test.check(waitUntil([&] { return probe->gets.load() == 1; }),
               "worker transfer should start");

    std::atomic<bool> clearFinished{false};
    std::thread clearing([&] {
        manager->clearSession();
        clearFinished.store(true);
    });

    // Concurrently invoke shutdown while clearSession is in flight
    manager->shutdown();
    test.check(manager->isShuttingDown(), "manager should report shuttingDown");

    // Multiple calls to shutdown must be idempotent and safe
    manager->shutdown();

    clearing.join();
    test.check(clearFinished.load(),
               "clearSession should unblock quickly on shutdown");

    // Destructor of manager should run cleanly after shutdown
    manager.reset();
}

OPENSCP_TEST(testBandwidthLimiterExceptionSafetyAndCleanup, test) {
    BandwidthLimiter limiter;
    limiter.setLimitKBps(32);
    test.check(limiter.queuedWaiters() == 0, "initial queue should be empty");

    // 1. Exception thrown during shouldCancel unwinds stack and cleans up
    // waiter via RAII
    bool exceptionCaught = false;
    try {
        [[maybe_unused]] const bool ignored =
            limiter.acquire(1, 1024 * 1024, [](std::uint64_t) -> bool {
                throw std::runtime_error("Simulated cancel callback exception");
            });
    } catch (const std::runtime_error &) {
        exceptionCaught = true;
    }
    test.check(exceptionCaught, "exception should propagate out of acquire");
    test.check(limiter.queuedWaiters() == 0,
               "waiter should be removed from queue on exception unwind");

    // 2. Cancellation via shouldCancel returning true cleans up waiter
    const bool acquiredCanceled =
        limiter.acquire(2, 1024 * 1024, [](std::uint64_t) { return true; });
    test.check(!acquiredCanceled, "canceled task should return false");
    test.check(limiter.queuedWaiters() == 0,
               "canceled waiter should be cleaned up from queue");

    // 3. Dynamic limit disable unblocks queued waiters cleanly
    std::atomic<bool> workerFinished{false};
    std::thread worker([&] {
        const bool ok =
            limiter.acquire(3, 512 * 1024, [](std::uint64_t) { return false; });
        workerFinished.store(ok);
    });

    test.check(waitUntil([&] { return limiter.queuedWaiters() > 0; }),
               "worker should be queued waiting for rate tokens");

    limiter.setLimitKBps(0);
    worker.join();
    test.check(workerFinished.load(),
               "worker should finish successfully after limit set to 0");
    test.check(limiter.queuedWaiters() == 0,
               "queue should be empty after unblocking");
}

OPENSCP_TEST(testBandwidthLimiterConcurrentFifoFairness, test) {
    BandwidthLimiter limiter;
    limiter.setLimitKBps(64);

    constexpr int kWorkerCount = 4;
    std::atomic<int> completedWorkers{0};
    std::vector<std::thread> workers;
    workers.reserve(kWorkerCount);

    for (int i = 0; i < kWorkerCount; ++i) {
        workers.emplace_back([&, taskId = static_cast<std::uint64_t>(i) + 1] {
            if (limiter.acquire(taskId, 4096,
                                [](std::uint64_t) { return false; })) {
                completedWorkers.fetch_add(1);
            }
        });
    }

    for (auto &w : workers) {
        w.join();
    }

    test.check(completedWorkers.load() == kWorkerCount,
               "all concurrent workers should acquire tokens successfully");
    test.check(limiter.queuedWaiters() == 0,
               "queue should be completely empty after all workers finish");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("openscp-transfer-manager-tests"));
    openscp::test::TestHarness harness("transfer manager");
    return harness.run();
}
