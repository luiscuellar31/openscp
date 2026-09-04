// Transfer queue tests without an external test framework.
#include "ConflictCoordinator.hpp"
#include "QtTestSupport.hpp"
#include "TestHarness.hpp"
#include "TransferManager.hpp"
#include "openscp/MockSftpClient.hpp"

#include <QCoreApplication>
#include <QFile>
#include <QFileDevice>
#include <QTemporaryDir>

#include <array>
#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <thread>
#include <vector>

struct TransferManagerTestAccess {
    static const TransferTask *taskAddress(TransferManager &manager,
                                           quint64 taskId) {
        std::lock_guard<std::mutex> lock(manager.mtx_);
        return manager.taskForIdLocked(taskId);
    }

    static std::size_t taskVectorCapacity(TransferManager &manager) {
        std::lock_guard<std::mutex> lock(manager.mtx_);
        return manager.queueStore_.capacity();
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
    test.check(
        manager.hasActiveTaskForSource(TransferTask::Type::Download,
                                       QStringLiteral("/remote/file-0.dat")) &&
            manager.hasActiveTaskForDestination(
                TransferTask::Type::Download,
                QStringLiteral("/local/file-9999.dat")),
        "direct path queries should find active work in large queues");
    const auto exactTaskId = manager.activeTaskIdForPaths(
        TransferTask::Type::Download, QStringLiteral("/remote/file-4999.dat"),
        QStringLiteral("/local/file-4999.dat"));
    test.check(
        exactTaskId == std::optional<quint64>{5000} &&
            !manager
                 .activeTaskIdForPaths(TransferTask::Type::Download,
                                       QStringLiteral("/remote/file-4999.dat"),
                                       QStringLiteral("/local/other.dat"))
                 .has_value(),
        "exact path queries should return the matching active task ID");
    test.check(
        !manager.hasActiveTaskForSource(TransferTask::Type::Upload,
                                        QStringLiteral("/remote/file-0.dat")) &&
            !manager.isBatchTerminal(batchId),
        "direct queries should preserve task type and terminal state");
    const QVector<quint64> activeIds =
        manager.activeTaskIdsForSession(QStringLiteral("site-a"));
    test.check(activeIds.size() == downloads.size() && activeIds.front() == 1 &&
                   activeIds.back() == 10'000,
               "session queries should return only IDs without copying a "
               "large queue");

    manager.cancelBatch(batchId);
    test.check(manager.isBatchTerminal(batchId) &&
                   !manager.hasActiveTaskForDestination(
                       TransferTask::Type::Download,
                       QStringLiteral("/local/file-9999.dat")) &&
                   manager.activeTaskIdsForSession({}).isEmpty(),
               "batch cancellation should become visible without snapshots");
    test.check(!manager.isBatchTerminal(0) &&
                   !manager.isBatchTerminal(batchId + 1000),
               "empty and unknown batches should not report terminal");

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

struct ConcurrencyProbe {
    std::atomic<int> active{0};
    std::atomic<int> maximum{0};
    std::atomic<int> connections{0};
    std::atomic<int> transfers{0};
};

class ConcurrentMockClient : public openscp::MockSftpClient {
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
    newConnectionLike(const openscp::SessionOptions &options,
                      std::string &err) override {
        probe_->connections.fetch_add(1);
        auto worker = std::make_unique<ConcurrentMockClient>(probe_);
        if (!worker->connect(options, err))
            return nullptr;
        return worker;
    }

    private:
    std::shared_ptr<ConcurrencyProbe> probe_;
};

void configureManager(TransferManager &manager,
                      openscp::RemoteClient &baseClient,
                      const openscp::SessionOptions &options) {
    std::string connectError;
    (void)baseClient.connect(options, connectError);
    manager.setSessionOptions(options);
    manager.setSessionIdentity(QStringLiteral("test-session"));
    manager.setClient(&baseClient);
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

class CancelLifecycleClient final : public openscp::MockSftpClient {
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
    newConnectionLike(const openscp::SessionOptions &options,
                      std::string &err) override {
        probe_->connections.fetch_add(1);
        auto worker = std::make_unique<CancelLifecycleClient>(probe_);
        if (!worker->connect(options, err))
            return nullptr;
        return worker;
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

OPENSCP_TEST(testClearClientInvalidatesWorkerConnections, test) {
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

    manager.clearClient();
    const auto task = manager.taskSnapshot(taskId);
    test.check(task &&
                   task->status == TransferTask::Status::WaitingForConnection,
               "clearing the session should leave active work waiting");
    test.check(probe->interrupts.load() >= 1 && probe->disconnects.load() >= 1,
               "clearClient should interrupt and invalidate worker clients");
}

class FinalTransportFailureClient final : public openscp::MockSftpClient {
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
    newConnectionLike(const openscp::SessionOptions &options,
                      std::string &err) override {
        probe_->connections.fetch_add(1);
        auto worker = std::make_unique<FinalTransportFailureClient>(probe_);
        if (!worker->connect(options, err))
            return nullptr;
        return worker;
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

struct RetryProbe {
    std::atomic<int> attempts{0};
};

class RetryMockClient final : public openscp::MockSftpClient {
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
    newConnectionLike(const openscp::SessionOptions &options,
                      std::string &err) override {
        auto worker = std::make_unique<RetryMockClient>(probe_);
        if (!worker->connect(options, err))
            return nullptr;
        return worker;
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

class UnclassifiedErrorMockClient final : public openscp::MockSftpClient {
    public:
    explicit UnclassifiedErrorMockClient(
        std::shared_ptr<std::atomic<int>> attempts)
        : attempts_(std::move(attempts)) {}

    bool get(const std::string &, const std::string &, std::string &err,
             std::function<void(std::size_t, std::size_t)>,
             std::function<bool()>, bool) override {
        attempts_->fetch_add(1);
        err.clear();
        clearLastOperationError();
        return false;
    }

    std::unique_ptr<openscp::RemoteClient>
    newConnectionLike(const openscp::SessionOptions &options,
                      std::string &err) override {
        auto worker = std::make_unique<UnclassifiedErrorMockClient>(attempts_);
        if (!worker->connect(options, err))
            return nullptr;
        return worker;
    }

    private:
    std::shared_ptr<std::atomic<int>> attempts_;
};

OPENSCP_TEST(testUnclassifiedErrorNeverRetries, test) {
    auto attempts = std::make_shared<std::atomic<int>>(0);
    UnclassifiedErrorMockClient baseClient(attempts);
    TransferManager manager;
    configureManager(manager, baseClient, testOptions());
    QTemporaryDir destination;
    auto batch = testBatchOptions();
    manager.enqueueDownload(QStringLiteral("/remote/unclassified.dat"),
                            destination.filePath("unclassified.dat"), batch);

    test.check(waitForStatus(manager, 1, TransferTask::Status::Error),
               "unclassified failures should become errors");
    test.check(attempts->load() == 1,
               "an error without transient evidence must not retry");
}

struct MovePhaseProbe {
    std::atomic<int> downloads{0};
    std::atomic<int> sourceDeletes{0};
};

class MovePhaseMockClient final : public openscp::MockSftpClient {
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
    newConnectionLike(const openscp::SessionOptions &options,
                      std::string &err) override {
        auto worker = std::make_unique<MovePhaseMockClient>(probe_);
        if (!worker->connect(options, err))
            return nullptr;
        return worker;
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

class CommitUncertainMockClient final : public openscp::MockSftpClient {
    public:
    bool get(const std::string &, const std::string &, std::string &err,
             std::function<void(std::size_t, std::size_t)>,
             std::function<bool()>, bool) override {
        err = "Connection lost after final server response";
        setLastOperationError(openscp::RemoteErrorKind::Connection, err, 0,
                              true, true);
        return false;
    }

    std::unique_ptr<openscp::RemoteClient>
    newConnectionLike(const openscp::SessionOptions &options,
                      std::string &err) override {
        auto worker = std::make_unique<CommitUncertainMockClient>();
        if (!worker->connect(options, err))
            return nullptr;
        return worker;
    }
};

OPENSCP_TEST(testCommitUncertainDoesNotRetry, test) {
    CommitUncertainMockClient baseClient;
    const auto options = testOptions();
    TransferManager manager;
    configureManager(manager, baseClient, options);
    QTemporaryDir destination;
    auto batch = testBatchOptions();
    manager.enqueueDownload(QStringLiteral("/remote/uncertain.dat"),
                            destination.filePath("uncertain.dat"), batch);
    test.check(waitForStatus(manager, 1, TransferTask::Status::Warning),
               "commit-uncertain results should become warnings");
    const auto beforeRetry = manager.taskSnapshot(1);
    manager.retryTask(1);
    const auto afterRetry = manager.taskSnapshot(1);
    test.check(beforeRetry && afterRetry && afterRetry->commitUncertain &&
                   afterRetry->attempts == 1 &&
                   afterRetry->status == TransferTask::Status::Warning,
               "commit-uncertain operations must never retry blindly");
}

class PermanentErrorMockClient final : public openscp::MockSftpClient {
    public:
    explicit PermanentErrorMockClient(
        std::shared_ptr<std::atomic<int>> attempts)
        : attempts_(std::move(attempts)) {}

    bool get(const std::string &, const std::string &, std::string &err,
             std::function<void(std::size_t, std::size_t)>,
             std::function<bool()>, bool) override {
        attempts_->fetch_add(1);
        err = "Authentication failed";
        // Even a malformed backend flag must not make this category retryable.
        setLastOperationError(openscp::RemoteErrorKind::Authentication, err, 0,
                              true);
        return false;
    }

    std::unique_ptr<openscp::RemoteClient>
    newConnectionLike(const openscp::SessionOptions &options,
                      std::string &err) override {
        auto worker = std::make_unique<PermanentErrorMockClient>(attempts_);
        if (!worker->connect(options, err))
            return nullptr;
        return worker;
    }

    private:
    std::shared_ptr<std::atomic<int>> attempts_;
};

OPENSCP_TEST(testPermanentStructuredErrorNeverRetries, test) {
    auto attempts = std::make_shared<std::atomic<int>>(0);
    PermanentErrorMockClient baseClient(attempts);
    const auto options = testOptions();
    TransferManager manager;
    configureManager(manager, baseClient, options);
    QTemporaryDir destination;
    auto batch = testBatchOptions();
    manager.enqueueDownload(QStringLiteral("/remote/auth.dat"),
                            destination.filePath("auth.dat"), batch);
    test.check(waitForStatus(manager, 1, TransferTask::Status::Error),
               "permanent structured failures should become errors");
    test.check(attempts->load() == 1,
               "authentication failures must not retry even if marked "
               "transient");
}

class InsufficientSpaceMockClient final : public openscp::MockSftpClient {
    public:
    explicit InsufficientSpaceMockClient(
        std::shared_ptr<std::atomic<int>> attempts)
        : attempts_(std::move(attempts)) {}

    bool get(const std::string &, const std::string &, std::string &err,
             std::function<void(std::size_t, std::size_t)>,
             std::function<bool()>, bool) override {
        attempts_->fetch_add(1);
        err = "Remote filesystem has insufficient space";
        // A backend flag must never turn an out-of-space result transient.
        setLastOperationError(openscp::RemoteErrorKind::InsufficientSpace, err,
                              507, true);
        return false;
    }

    std::unique_ptr<openscp::RemoteClient>
    newConnectionLike(const openscp::SessionOptions &options,
                      std::string &err) override {
        auto worker = std::make_unique<InsufficientSpaceMockClient>(attempts_);
        if (!worker->connect(options, err))
            return nullptr;
        return worker;
    }

    private:
    std::shared_ptr<std::atomic<int>> attempts_;
};

OPENSCP_TEST(testInsufficientSpaceNeverRetries, test) {
    auto attempts = std::make_shared<std::atomic<int>>(0);
    InsufficientSpaceMockClient baseClient(attempts);
    const auto options = testOptions();
    TransferManager manager;
    configureManager(manager, baseClient, options);
    QTemporaryDir destination;
    auto batch = testBatchOptions();
    manager.enqueueDownload(QStringLiteral("/remote/full.dat"),
                            destination.filePath("full.dat"), batch);

    test.check(waitForStatus(manager, 1, TransferTask::Status::Error),
               "insufficient-space failures should become errors");
    test.check(attempts->load() == 1,
               "insufficient-space failures must never retry");
}

struct PermanentKindsProbe {
    std::atomic<int> certificate{0};
    std::atomic<int> permission{0};
    std::atomic<int> integrity{0};
};

class PermanentKindsMockClient final : public openscp::MockSftpClient {
    public:
    explicit PermanentKindsMockClient(
        std::shared_ptr<PermanentKindsProbe> probe)
        : probe_(std::move(probe)) {}

    bool get(const std::string &remote, const std::string &, std::string &err,
             std::function<void(std::size_t, std::size_t)>,
             std::function<bool()>, bool) override {
        openscp::RemoteErrorKind kind = openscp::RemoteErrorKind::Integrity;
        if (remote.find("certificate") != std::string::npos) {
            probe_->certificate.fetch_add(1);
            kind = openscp::RemoteErrorKind::Certificate;
            err = "Certificate verification failed";
        } else if (remote.find("permission") != std::string::npos) {
            probe_->permission.fetch_add(1);
            kind = openscp::RemoteErrorKind::PermissionDenied;
            err = "Permission denied";
        } else {
            probe_->integrity.fetch_add(1);
            err = "Checksum mismatch";
        }
        // Deliberately malformed transient=true verifies category precedence.
        setLastOperationError(kind, err, 0, true);
        return false;
    }

    std::unique_ptr<openscp::RemoteClient>
    newConnectionLike(const openscp::SessionOptions &options,
                      std::string &err) override {
        auto worker = std::make_unique<PermanentKindsMockClient>(probe_);
        if (!worker->connect(options, err))
            return nullptr;
        return worker;
    }

    private:
    std::shared_ptr<PermanentKindsProbe> probe_;
};

OPENSCP_TEST(testOtherPermanentKindsNeverRetry, test) {
    auto probe = std::make_shared<PermanentKindsProbe>();
    PermanentKindsMockClient baseClient(probe);
    TransferManager manager;
    manager.setMaxConcurrent(3);
    configureManager(manager, baseClient, testOptions());
    QTemporaryDir destination;
    auto batch = testBatchOptions();
    manager.enqueueDownloads({{QStringLiteral("/remote/certificate"),
                               destination.filePath("certificate")},
                              {QStringLiteral("/remote/permission"),
                               destination.filePath("permission")},
                              {QStringLiteral("/remote/integrity"),
                               destination.filePath("integrity")}},
                             batch);
    test.check(waitUntil([&] {
                   const auto tasks = manager.tasksSnapshot();
                   return tasks.size() == 3 &&
                          std::all_of(tasks.cbegin(), tasks.cend(),
                                      [](const TransferTask &task) {
                                          return task.status ==
                                                 TransferTask::Status::Error;
                                      });
               }),
               "certificate, permission and integrity failures should stop");
    test.check(probe->certificate.load() == 1 &&
                   probe->permission.load() == 1 &&
                   probe->integrity.load() == 1,
               "permanent structured error kinds must never be retried");
}

OPENSCP_TEST(testFailedDependencySkipsFollowingWork, test) {
    auto attempts = std::make_shared<std::atomic<int>>(0);
    PermanentErrorMockClient baseClient(attempts);
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

OPENSCP_TEST(testDependencySkipsKeepTerminalCounterAndHistoryBounded, test) {
    auto attempts = std::make_shared<std::atomic<int>>(0);
    PermanentErrorMockClient baseClient(attempts);
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
    dependent.reserve(5101);
    for (int index = 0; index < 5101; ++index) {
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
    test.check(attempts->load() == 1,
               "skipped dependents must not execute after their prerequisite");
}

class RateMockClient final : public openscp::MockSftpClient {
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
    newConnectionLike(const openscp::SessionOptions &options,
                      std::string &err) override {
        auto worker = std::make_unique<RateMockClient>();
        if (!worker->connect(options, err))
            return nullptr;
        return worker;
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
    QVector<QPair<QString, QString>> downloads;
    downloads.reserve(5100);
    for (int index = 0; index < 5100; ++index) {
        downloads.push_back({QStringLiteral("/remote/history-%1").arg(index),
                             QStringLiteral("/local/history-%1").arg(index)});
    }
    manager.enqueueDownloads(downloads);
    manager.cancelAll();
    const auto tasks = manager.tasksSnapshot();
    test.check(tasks.size() == 5000,
               "terminal queue history should be bounded to 5000 tasks");
    test.check(tasks.front().taskId == 101 && tasks.back().taskId == 5100,
               "history pruning should retain the newest terminal tasks");
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
    newConnectionLike(const openscp::SessionOptions &options,
                      std::string &err) override {
        auto worker = std::make_unique<RemotePartialCleanupClient>(probe_);
        if (!worker->connect(options, err))
            return nullptr;
        return worker;
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
        "{\"schemaVersion\":2,\"tasks\":[{\"future\":true}]}");
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
    const std::array<QByteArray, 3> invalidDocuments{
        QByteArray(
            R"({"schemaVersion":1,"tasks":[{"id":"1","batchId":"1","type":"download","source":"/a","destination":"/b","operation":"copy","conflictPolicy":"ask","phase":"transfer"},{"id":"1","batchId":"1","type":"download","source":"/c","destination":"/d","operation":"copy","conflictPolicy":"ask","phase":"transfer"}]})"),
        QByteArray(
            R"({"schemaVersion":1,"tasks":[{"id":1.5,"batchId":"1","type":"download","source":"/a","destination":"/b","operation":"copy","conflictPolicy":"ask","phase":"transfer"}]})"),
        QByteArray(
            R"({"schemaVersion":1,"tasks":[{"id":"1","batchId":"1","type":"future-task","source":"/a","destination":"/b","operation":"copy","conflictPolicy":"ask","phase":"transfer"}]})")};

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

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    QCoreApplication::setApplicationName(
        QStringLiteral("openscp-transfer-manager-tests"));
    openscp::test::TestHarness harness("transfer manager");
    return harness.run();
}
