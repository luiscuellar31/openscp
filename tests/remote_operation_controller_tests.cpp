// Unit tests for the serialized remote-operation execution lane.
#include "RemoteOperationController.hpp"
#include "TestHarness.hpp"
#include "openscp/SftpClient.hpp"

#include <QCoreApplication>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <iostream>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace {

bool spinUntil(const std::function<bool()> &predicate, int timeoutMs = 3000) {
    QElapsedTimer timer;
    timer.start();
    while (!predicate() && timer.elapsed() < timeoutMs) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
    return predicate();
}

struct FakeState {
    std::mutex mutex;
    std::condition_variable wake;
    std::vector<std::string> calls;
    std::thread::id workerThread;
    std::thread::id disconnectThread;
    std::thread::id destructorThread;
    std::atomic_int activeCalls{0};
    std::atomic_int maximumActiveCalls{0};
    std::atomic_bool interrupted{false};
};

class ActiveCall {
    public:
    ActiveCall(const std::shared_ptr<FakeState> &state, std::string name)
        : state_(state) {
        const int active = state_->activeCalls.fetch_add(1) + 1;
        int maximum = state_->maximumActiveCalls.load();
        while (active > maximum &&
               !state_->maximumActiveCalls.compare_exchange_weak(maximum,
                                                                 active)) {
        }
        std::lock_guard lock(state_->mutex);
        state_->workerThread = std::this_thread::get_id();
        state_->calls.push_back(std::move(name));
    }

    ~ActiveCall() { state_->activeCalls.fetch_sub(1); }

    private:
    std::shared_ptr<FakeState> state_;
};

class ControllerFakeClient final : public openscp::SftpClient {
    public:
    explicit ControllerFakeClient(std::shared_ptr<FakeState> state)
        : state_(std::move(state)) {}

    openscp::ProtocolCapabilities capabilities() const override {
        return openscp::capabilitiesForProtocol(openscp::Protocol::Sftp);
    }

    ~ControllerFakeClient() override {
        std::lock_guard lock(state_->mutex);
        state_->destructorThread = std::this_thread::get_id();
    }

    bool connect(const openscp::SessionOptions &, std::string &err) override {
        connected_.store(true);
        state_->interrupted.store(false);
        err.clear();
        return true;
    }

    void disconnect() override {
        connected_.store(false);
        std::lock_guard lock(state_->mutex);
        state_->disconnectThread = std::this_thread::get_id();
    }

    void interrupt() override {
        state_->interrupted.store(true);
        state_->wake.notify_all();
    }

    bool isConnected() const override { return connected_.load(); }

    bool list(const std::string &remotePath,
              std::vector<openscp::FileInfo> &out, std::string &err) override {
        ActiveCall call(state_, "list:" + remotePath);
        if (remotePath == "/slow") {
            std::unique_lock lock(state_->mutex);
            state_->wake.wait_for(lock, std::chrono::seconds(2), [this] {
                return state_->interrupted.load();
            });
            if (state_->interrupted.load()) {
                err = "Canceled";
                return false;
            }
        }

        if (remotePath == "/") {
            out = {
                {"docs", true, 0, false, 10, 0040755u, 1, 1},
                {"alpha.txt", false, 10, true, 20, 0100644u, 1, 1},
            };
        } else if (remotePath == "/docs") {
            out = {
                {"beta.txt", false, 20, true, 30, 0100644u, 1, 1},
            };
        } else if (remotePath == "/summary") {
            out = {
                {"linked-file", false, 0, false, 0, 0120777u, 1, 1},
                {"deep", true, 0, false, 0, 0040755u, 1, 1},
                {"unknown.bin", false, 0, false, 0, 0100644u, 1, 1},
                {"../escape.txt", false, 1, true, 0, 0100644u, 1, 1},
                {"nested/escape.txt", false, 1, true, 0, 0100644u, 1, 1},
            };
        } else if (remotePath == "/summary/deep") {
            out = {
                {"must-not-be-listed.txt", false, 1, true, 0, 0100644u, 1, 1},
            };
        } else if (remotePath == "/unsafe") {
            out = {
                {"safe.txt", false, 4, true, 0, 0100644u, 1, 1},
                {".", true, 0, false, 0, 0040755u, 1, 1},
                {"..", true, 0, false, 0, 0040755u, 1, 1},
                {"nested/escape.txt", false, 1, true, 0, 0100644u, 1, 1},
                {std::string("bad-") + char(0xff), false, 1, true, 0, 0100644u,
                 1, 1},
            };
        } else {
            out.clear();
        }
        err.clear();
        return true;
    }

    bool get(const std::string &, const std::string &, std::string &err,
             std::function<void(std::size_t, std::size_t)>,
             std::function<bool()>, bool) override {
        err = "Not used";
        return false;
    }

    bool put(const std::string &, const std::string &remotePath,
             std::string &err, std::function<void(std::size_t, std::size_t)>,
             std::function<bool()>, bool) override {
        ActiveCall call(state_, "put:" + remotePath);
        err.clear();
        return true;
    }

    bool exists(const std::string &path, bool &isDirectory,
                std::string &err) override {
        ActiveCall call(state_, "exists:" + path);
        isDirectory = path == "/existing";
        err.clear();
        return isDirectory;
    }

    bool stat(const std::string &path, openscp::FileInfo &info,
              std::string &err) override {
        ActiveCall call(state_, "stat:" + path);
        info = {"stat.txt", false, 5, true, 40, 0100644u, 1, 1};
        err.clear();
        return true;
    }

    bool chmod(const std::string &path, std::uint32_t,
               std::string &err) override {
        ActiveCall call(state_, "chmod:" + path);
        err.clear();
        return true;
    }

    bool chown(const std::string &, std::uint32_t, std::uint32_t,
               std::string &err) override {
        err = "Not used";
        return false;
    }

    bool setTimes(const std::string &, std::uint64_t, std::uint64_t,
                  std::string &err) override {
        err = "Not used";
        return false;
    }

    bool mkdir(const std::string &path, std::string &err,
               unsigned int) override {
        ActiveCall call(state_, "mkdir:" + path);
        err.clear();
        return true;
    }

    bool removeFile(const std::string &path, std::string &err) override {
        ActiveCall call(state_, "removeFile:" + path);
        err.clear();
        return true;
    }

    bool removeDir(const std::string &path, std::string &err) override {
        ActiveCall call(state_, "removeDir:" + path);
        err.clear();
        return true;
    }

    bool rename(const std::string &from, const std::string &to,
                std::string &err, bool) override {
        ActiveCall call(state_, "rename:" + from + ":" + to);
        err.clear();
        return true;
    }

    bool checksum(const std::string &remotePath, const std::string &algorithm,
                  std::vector<std::uint8_t> &digest, std::string &err,
                  std::function<void(std::size_t, std::size_t)> progress,
                  std::function<bool()> shouldCancel) override {
        ActiveCall call(state_, "checksum:" + remotePath);
        if (algorithm != "SHA-256") {
            err = "Unsupported checksum algorithm";
            setLastOperationError(openscp::RemoteErrorKind::Unsupported, err);
            return false;
        }
        const std::size_t total = remotePath == "/checksum-slow.bin" ? 200 : 4;
        for (std::size_t done = 0; done < total; ++done) {
            if ((shouldCancel && shouldCancel()) ||
                state_->interrupted.load()) {
                err = "Checksum calculation canceled";
                setLastOperationError(openscp::RemoteErrorKind::Canceled, err);
                return false;
            }
            if (progress)
                progress(done + 1, total);
            if (remotePath == "/checksum-slow.bin")
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        digest = remotePath == "/different.bin"
                     ? std::vector<std::uint8_t>{9, 8, 7, 6}
                     : std::vector<std::uint8_t>{1, 2, 3, 4};
        err.clear();
        return true;
    }

    std::unique_ptr<openscp::SftpClient>
    newConnectionLike(const openscp::SessionOptions &options,
                      std::string &err) override {
        auto client = std::make_unique<ControllerFakeClient>(state_);
        if (!client->connect(options, err))
            return nullptr;
        return client;
    }

    private:
    std::shared_ptr<FakeState> state_;
    std::atomic_bool connected_{false};
};

std::unique_ptr<ControllerFakeClient>
makeConnectedClient(const std::shared_ptr<FakeState> &state) {
    auto client = std::make_unique<ControllerFakeClient>(state);
    openscp::SessionOptions options;
    options.host = "controller.test";
    std::string error;
    if (!client->connect(options, error))
        return nullptr;
    return client;
}

void testTypedOperationsAreSerialized(TestContext &test) {
    RemoteOperationController controller;
    const auto state = std::make_shared<FakeState>();
    int readySignals = 0;
    int finishedSignals = 0;
    int successfulSignals = 0;
    int searchMatches = 0;
    bool checksumDelivered = false;
    bool checksumProgressDelivered = false;
    bool callbacksOnUiThread = true;
    const QThread *uiThread = QThread::currentThread();

    QObject::connect(
        &controller, &RemoteOperationController::sessionChanged, &controller,
        [&](const RemoteOperationController::SessionState &session) {
            callbacksOnUiThread &= QThread::currentThread() == uiThread;
            if (session.available)
                ++readySignals;
        });
    QObject::connect(
        &controller, &RemoteOperationController::entriesBatchReady, &controller,
        [&](const RemoteOperationController::EntryBatch &batch) {
            callbacksOnUiThread &= QThread::currentThread() == uiThread;
            if (batch.job.kind == RemoteOperationController::JobKind::Search)
                searchMatches += batch.entries.size();
        });
    QObject::connect(
        &controller, &RemoteOperationController::checksumCompleted, &controller,
        [&](const RemoteOperationController::ChecksumResult &result) {
            callbacksOnUiThread &= QThread::currentThread() == uiThread;
            if (result.result.outcome ==
                    RemoteOperationController::Outcome::Succeeded &&
                result.path == QStringLiteral("/checksum.bin") &&
                result.digest ==
                    QByteArray::fromRawData("\x01\x02\x03\x04", 4)) {
                checksumDelivered = true;
            }
        });
    QObject::connect(
        &controller, &RemoteOperationController::jobProgress, &controller,
        [&](const RemoteOperationController::Progress &progress) {
            callbacksOnUiThread &= QThread::currentThread() == uiThread;
            if (progress.job.kind ==
                    RemoteOperationController::JobKind::Checksum &&
                progress.processedBytes > 0) {
                checksumProgressDelivered = true;
            }
        });
    QObject::connect(
        &controller, &RemoteOperationController::jobFinished, &controller,
        [&](const RemoteOperationController::Completion &completion) {
            callbacksOnUiThread &= QThread::currentThread() == uiThread;
            ++finishedSignals;
            if (completion.result.outcome ==
                RemoteOperationController::Outcome::Succeeded) {
                ++successfulSignals;
            }
        });

    controller.installSession(makeConnectedClient(state));
    test.check(spinUntil([&] { return readySignals == 1; }),
               "installed sessions should become ready asynchronously");

    controller.submit(RemoteOperationController::ListRequest{"/", true});
    controller.submit(RemoteOperationController::StatRequest{"/alpha.txt"});
    controller.submit(
        RemoteOperationController::MkdirRequest{"/created", 0755});
    controller.submit(
        RemoteOperationController::MkdirRequest{"/parent/child", 0755, true});
    controller.submit(
        RemoteOperationController::CreateFileRequest{"/empty.txt", true});
    controller.submit(RemoteOperationController::RenameRequest{
        "/created", "/renamed", false});
    controller.submit(RemoteOperationController::DeleteRequest{
        "/old.txt", RemoteOperationController::DeleteKind::File, false, {}});
    RemoteOperationController::DeleteRequest emptyDirectoryCleanup{
        "/", RemoteOperationController::DeleteKind::Directory, true, {}};
    emptyDirectoryCleanup.emptyDirectoriesOnly = true;
    controller.submit(emptyDirectoryCleanup);
    controller.submit(
        RemoteOperationController::ChmodRequest{"/alpha.txt", 0600, false, {}});
    controller.submit(
        RemoteOperationController::HealthCheckRequest{"/alpha.txt"});
    RemoteOperationController::SearchRequest search;
    search.rootPath = "/";
    search.query = ".txt";
    search.traversal.batchSize = 1;
    controller.submit(search);
    RemoteOperationController::TraverseRequest traversal;
    traversal.rootPath = "/";
    traversal.traversal.batchSize = 2;
    controller.submit(traversal);
    controller.submit(
        RemoteOperationController::ChecksumRequest{"/checksum.bin", "SHA-256"});

    test.check(spinUntil([&] { return finishedSignals == 13; }),
               "every typed request should deliver a completion");
    test.check(successfulSignals == 13,
               "all successful fake operations should report success");
    test.check(searchMatches == 2,
               "recursive search should stream both matching files");
    test.check(state->maximumActiveCalls.load() == 1,
               "remote calls should execute on one serialized lane");
    test.check(callbacksOnUiThread,
               "all controller results should be delivered on the Qt thread");
    test.check(checksumDelivered && checksumProgressDelivered,
               "typed checksum jobs should return digest and progress");
    {
        std::lock_guard lock(state->mutex);
        const bool cleanupDeletedFile =
            std::find(state->calls.cbegin(), state->calls.cend(),
                      "removeFile:/alpha.txt") != state->calls.cend() ||
            std::find(state->calls.cbegin(), state->calls.cend(),
                      "removeFile:/docs/beta.txt") != state->calls.cend();
        test.check(!cleanupDeletedFile,
                   "empty-directory cleanup must never delete remote files");
    }
}

void testChecksumCancellationKeepsEventLoopResponsive(TestContext &test) {
    RemoteOperationController controller;
    const auto state = std::make_shared<FakeState>();
    RemoteOperationController::JobId checksumJob = 0;
    bool started = false;
    bool canceled = false;
    bool typedCanceled = false;
    bool eventLoopAdvanced = false;

    QObject::connect(&controller, &RemoteOperationController::jobStarted,
                     &controller,
                     [&](const RemoteOperationController::JobKey &job) {
                         if (job.id == checksumJob)
                             started = true;
                     });
    QObject::connect(
        &controller, &RemoteOperationController::checksumCompleted, &controller,
        [&](const RemoteOperationController::ChecksumResult &result) {
            if (result.result.job.id == checksumJob) {
                typedCanceled = result.result.outcome ==
                                RemoteOperationController::Outcome::Canceled;
            }
        });
    QObject::connect(
        &controller, &RemoteOperationController::jobFinished, &controller,
        [&](const RemoteOperationController::Completion &completion) {
            if (completion.result.job.id == checksumJob) {
                canceled = completion.result.outcome ==
                           RemoteOperationController::Outcome::Canceled;
            }
        });

    controller.installSession(makeConnectedClient(state));
    test.check(spinUntil([&] { return controller.hasRequestedSession(); }),
               "checksum cancellation session should be requested");
    checksumJob = controller.submit(RemoteOperationController::ChecksumRequest{
        QStringLiteral("/checksum-slow.bin"), QStringLiteral("SHA-256")});
    QTimer::singleShot(0, &controller, [&] { eventLoopAdvanced = true; });
    test.check(spinUntil([&] { return started && eventLoopAdvanced; }),
               "remote checksum must leave the Qt event loop responsive");
    test.check(controller.cancel(checksumJob),
               "an active checksum should accept cancellation");
    test.check(spinUntil([&] { return canceled && typedCanceled; }),
               "checksum cancellation should reach typed and generic results");
    test.check(state->interrupted.load(),
               "checksum cancellation should interrupt blocking remote I/O");
}

void testListRejectsUnsafeNames(TestContext &test) {
    RemoteOperationController controller;
    const auto state = std::make_shared<FakeState>();
    RemoteOperationController::JobId listJob = 0;
    std::optional<RemoteOperationController::ListResult> listResult;
    std::optional<RemoteOperationController::Completion> completion;

    QObject::connect(&controller, &RemoteOperationController::listCompleted,
                     &controller,
                     [&](const RemoteOperationController::ListResult &result) {
                         if (result.result.job.id == listJob)
                             listResult = result;
                     });
    QObject::connect(&controller, &RemoteOperationController::jobFinished,
                     &controller,
                     [&](const RemoteOperationController::Completion &result) {
                         if (result.result.job.id == listJob)
                             completion = result;
                     });

    controller.installSession(makeConnectedClient(state));
    listJob = controller.submit(
        RemoteOperationController::ListRequest{"/unsafe", true});
    test.check(spinUntil([&] { return listResult && completion; }),
               "unsafe-name list fixture should complete");
    test.check(listResult && listResult->entries.size() == 1 &&
                   listResult->entries.front().relativePath ==
                       QStringLiteral("safe.txt"),
               "normal listings must filter unsafe and invalid UTF-8 names");
    test.check(completion && completion->invalidNames == 4,
               "normal listings should report every rejected name");
}

void testRecursiveMutationsRejectUnsafeNames(TestContext &test) {
    RemoteOperationController controller;
    const auto state = std::make_shared<FakeState>();
    RemoteOperationController::JobId deleteJob = 0;
    RemoteOperationController::JobId chmodJob = 0;
    std::optional<RemoteOperationController::Completion> deleteCompletion;
    std::optional<RemoteOperationController::Completion> chmodCompletion;

    QObject::connect(
        &controller, &RemoteOperationController::jobFinished, &controller,
        [&](const RemoteOperationController::Completion &completion) {
            if (completion.result.job.id == deleteJob)
                deleteCompletion = completion;
            if (completion.result.job.id == chmodJob)
                chmodCompletion = completion;
        });

    controller.installSession(makeConnectedClient(state));
    RemoteOperationController::DeleteRequest deletion;
    deletion.path = QStringLiteral("/summary");
    deletion.kind = RemoteOperationController::DeleteKind::Directory;
    deletion.recursive = true;
    deletion.traversal.skipSymlinks = true;
    deleteJob = controller.submit(deletion);

    RemoteOperationController::ChmodRequest chmod;
    chmod.path = QStringLiteral("/summary");
    chmod.mode = 0600;
    chmod.recursive = true;
    chmod.traversal.skipSymlinks = true;
    chmodJob = controller.submit(chmod);

    test.check(spinUntil([&] { return deleteCompletion && chmodCompletion; }),
               "recursive mutation safety fixtures should complete");
    const auto checkCounters =
        [&](const std::optional<RemoteOperationController::Completion>
                &completion,
            const char *context) {
            test.check(completion.has_value(), context);
            if (!completion)
                return;
            test.check(completion->invalidNames == 2,
                       "recursive mutations should count unsafe names");
            test.check(completion->skippedSymlinks == 1,
                       "recursive mutations should count skipped symlinks");
            test.check(completion->result.outcome ==
                               RemoteOperationController::Outcome::Failed &&
                           completion->result.partial,
                       "skipping unsafe mutation targets should report a "
                       "partial failure");
        };
    checkCounters(deleteCompletion,
                  "recursive delete completion should be available");
    checkCounters(chmodCompletion,
                  "recursive chmod completion should be available");

    std::lock_guard lock(state->mutex);
    test.check(
        std::none_of(state->calls.cbegin(), state->calls.cend(),
                     [](const std::string &call) {
                         return call.find("../") != std::string::npos ||
                                call.find("nested/escape") !=
                                    std::string::npos ||
                                call.find("linked-file") != std::string::npos;
                     }),
        "recursive mutations must never target unsafe names or skipped links");
}

void testCancellationAndGenerationReplacement(TestContext &test) {
    RemoteOperationController controller;
    const auto firstState = std::make_shared<FakeState>();
    const auto secondState = std::make_shared<FakeState>();
    RemoteOperationController::JobId slowJob = 0;
    RemoteOperationController::JobId queuedJob = 0;
    bool slowStarted = false;
    bool slowCanceled = false;
    bool queuedSuperseded = false;
    int readySignals = 0;

    QObject::connect(
        &controller, &RemoteOperationController::sessionChanged, &controller,
        [&](const RemoteOperationController::SessionState &session) {
            if (session.available)
                ++readySignals;
        });
    QObject::connect(&controller, &RemoteOperationController::jobStarted,
                     &controller,
                     [&](const RemoteOperationController::JobKey &job) {
                         if (job.id == slowJob)
                             slowStarted = true;
                     });
    QObject::connect(
        &controller, &RemoteOperationController::jobFinished, &controller,
        [&](const RemoteOperationController::Completion &completion) {
            if (completion.result.job.id == slowJob) {
                slowCanceled = completion.result.outcome ==
                               RemoteOperationController::Outcome::Canceled;
            }
            if (completion.result.job.id == queuedJob) {
                queuedSuperseded =
                    completion.result.outcome ==
                    RemoteOperationController::Outcome::Superseded;
            }
        });

    controller.installSession(makeConnectedClient(firstState));
    test.check(spinUntil([&] { return readySignals == 1; }),
               "first session should be installed");

    slowJob = controller.submit(
        RemoteOperationController::ListRequest{"/slow", true});
    queuedJob =
        controller.submit(RemoteOperationController::StatRequest{"/queued"});
    test.check(spinUntil([&] { return slowStarted; }),
               "slow request should start on the worker");

    controller.installSession(makeConnectedClient(secondState));
    spinUntil(
        [&] { return slowCanceled && queuedSuperseded && readySignals == 2; });
    test.check(slowCanceled, "session replacement should cancel active work");
    test.check(queuedSuperseded,
               "session replacement should supersede queued work");
    test.check(readySignals == 2, "replacement session should become ready");
    test.check(firstState->interrupted.load(),
               "replacing a session should interrupt active network I/O");
    test.check(firstState->destructorThread == firstState->workerThread,
               "the serialized lane should destroy the replaced client");
    test.check(firstState->destructorThread != std::this_thread::get_id(),
               "a replaced client should not be destroyed on the UI thread");
}

void testExplicitCancellationAndShutdown(TestContext &test) {
    const auto state = std::make_shared<FakeState>();
    RemoteOperationController::JobId slowJob = 0;
    bool started = false;
    bool canceled = false;
    {
        RemoteOperationController controller;
        QObject::connect(&controller, &RemoteOperationController::jobStarted,
                         &controller,
                         [&](const RemoteOperationController::JobKey &job) {
                             if (job.id == slowJob)
                                 started = true;
                         });
        QObject::connect(
            &controller, &RemoteOperationController::jobFinished, &controller,
            [&](const RemoteOperationController::Completion &completion) {
                if (completion.result.job.id == slowJob) {
                    canceled = completion.result.outcome ==
                               RemoteOperationController::Outcome::Canceled;
                }
            });

        controller.installSession(makeConnectedClient(state));
        test.check(spinUntil([&] { return controller.hasRequestedSession(); }),
                   "controller should expose the requested session state");
        slowJob = controller.submit(
            RemoteOperationController::ListRequest{"/slow", true});
        test.check(spinUntil([&] { return started; }),
                   "cancel test operation should start");
        test.check(controller.cancel(slowJob),
                   "cancel should find an active job");
        test.check(spinUntil([&] { return canceled; }),
                   "an interrupted list should report cancellation");
        controller.shutdown();
        controller.shutdown();
    }
    test.check(state->disconnectThread == state->workerThread,
               "shutdown should disconnect on the serialized lane");
    test.check(state->destructorThread == state->workerThread,
               "shutdown should destroy the client on the serialized lane");
}

void testTraversalPauseProvidesBackpressure(TestContext &test) {
    RemoteOperationController controller;
    const auto state = std::make_shared<FakeState>();
    RemoteOperationController::JobId slowJob = 0;
    RemoteOperationController::JobId traversalJob = 0;
    bool slowStarted = false;
    bool slowFinished = false;
    bool traversalFinished = false;

    QObject::connect(&controller, &RemoteOperationController::jobStarted,
                     &controller,
                     [&](const RemoteOperationController::JobKey &job) {
                         if (job.id == slowJob)
                             slowStarted = true;
                     });
    QObject::connect(
        &controller, &RemoteOperationController::jobFinished, &controller,
        [&](const RemoteOperationController::Completion &completion) {
            if (completion.result.job.id == slowJob)
                slowFinished = true;
            if (completion.result.job.id == traversalJob)
                traversalFinished = true;
        });

    controller.installSession(makeConnectedClient(state));
    test.check(spinUntil([&] { return controller.hasRequestedSession(); }),
               "pause test session should be requested");
    slowJob = controller.submit(
        RemoteOperationController::ListRequest{"/slow", true});
    test.check(spinUntil([&] { return slowStarted; }),
               "pause test blocker should start");

    RemoteOperationController::TraverseRequest traversal;
    traversal.rootPath = "/";
    traversal.traversal.batchSize = 1;
    traversalJob = controller.submit(traversal);
    test.check(controller.setPaused(traversalJob, true),
               "queued traversal should accept backpressure pause");
    controller.cancel(slowJob);
    test.check(spinUntil([&] { return slowFinished; }),
               "blocker should finish after cancellation");

    QElapsedTimer pauseWindow;
    pauseWindow.start();
    while (pauseWindow.elapsed() < 75)
        QCoreApplication::processEvents(QEventLoop::AllEvents, 5);
    {
        std::lock_guard lock(state->mutex);
        const bool listedRoot =
            std::find(state->calls.cbegin(), state->calls.cend(), "list:/") !=
            state->calls.cend();
        test.check(!listedRoot,
                   "paused traversal must not issue another network listing");
    }

    test.check(controller.setPaused(traversalJob, false),
               "paused traversal should resume");
    test.check(spinUntil([&] { return traversalFinished; }),
               "resumed traversal should finish");
}

void testHealthChecksYieldToUserWork(TestContext &test) {
    RemoteOperationController controller;
    const auto state = std::make_shared<FakeState>();
    RemoteOperationController::JobId blocker = 0;
    RemoteOperationController::JobId health = 0;
    RemoteOperationController::JobId userStat = 0;
    bool blockerStarted = false;
    bool healthFinished = false;
    bool statFinished = false;

    QObject::connect(&controller, &RemoteOperationController::jobStarted,
                     &controller,
                     [&](const RemoteOperationController::JobKey &job) {
                         if (job.id == blocker)
                             blockerStarted = true;
                     });
    QObject::connect(
        &controller, &RemoteOperationController::jobFinished, &controller,
        [&](const RemoteOperationController::Completion &completion) {
            if (completion.result.job.id == health)
                healthFinished = true;
            if (completion.result.job.id == userStat)
                statFinished = true;
        });

    controller.installSession(makeConnectedClient(state));
    test.check(spinUntil([&] { return controller.hasRequestedSession(); }),
               "priority test session should be requested");
    blocker = controller.submit(
        RemoteOperationController::ListRequest{"/slow", true});
    test.check(spinUntil([&] { return blockerStarted; }),
               "priority test blocker should start");

    health = controller.submit(
        RemoteOperationController::HealthCheckRequest{"/probe"});
    userStat =
        controller.submit(RemoteOperationController::StatRequest{"/priority"});
    controller.cancel(blocker);
    test.check(spinUntil([&] { return healthFinished && statFinished; }),
               "queued health and user jobs should both finish");

    std::lock_guard lock(state->mutex);
    const auto statPosition =
        std::find(state->calls.cbegin(), state->calls.cend(), "stat:/priority");
    const auto healthPosition =
        std::find(state->calls.cbegin(), state->calls.cend(), "stat:/probe");
    test.check(statPosition != state->calls.cend() &&
                   healthPosition != state->calls.cend() &&
                   statPosition < healthPosition,
               "queued health checks must yield to later user operations");
}

void testDiscoverySummaryCountersAndConfinement(TestContext &test) {
    RemoteOperationController controller;
    const auto state = std::make_shared<FakeState>();
    std::optional<RemoteOperationController::Completion> traversalCompletion;
    std::optional<RemoteOperationController::Completion> searchCompletion;
    QVector<QString> emittedPaths;
    RemoteOperationController::JobId traversalJob = 0;
    RemoteOperationController::JobId searchJob = 0;

    QObject::connect(
        &controller, &RemoteOperationController::entriesBatchReady, &controller,
        [&](const RemoteOperationController::EntryBatch &batch) {
            if (batch.job.id != traversalJob && batch.job.id != searchJob)
                return;
            for (const auto &entry : batch.entries)
                emittedPaths.push_back(entry.relativePath);
        });
    QObject::connect(
        &controller, &RemoteOperationController::jobFinished, &controller,
        [&](const RemoteOperationController::Completion &completion) {
            if (completion.result.job.id == traversalJob)
                traversalCompletion = completion;
            if (completion.result.job.id == searchJob)
                searchCompletion = completion;
        });

    controller.installSession(makeConnectedClient(state));

    RemoteOperationController::TraverseRequest traversal;
    traversal.rootPath = QStringLiteral("/summary");
    traversal.traversal.maxDepth = 1;
    traversal.traversal.skipSymlinks = true;
    traversalJob = controller.submit(traversal);

    RemoteOperationController::SearchRequest search;
    search.rootPath = QStringLiteral("/summary");
    search.query = QStringLiteral("unknown");
    search.traversal.maxDepth = 1;
    search.traversal.skipSymlinks = true;
    searchJob = controller.submit(search);

    test.check(
        spinUntil([&] { return traversalCompletion && searchCompletion; }),
        "traverse and search should report discovery summaries");

    const auto checkSummary =
        [&](const std::optional<RemoteOperationController::Completion>
                &completion,
            quint64 expectedMatches, const char *context) {
            test.check(completion.has_value(), context);
            if (!completion)
                return;
            test.check(completion->result.outcome ==
                           RemoteOperationController::Outcome::Succeeded,
                       "counter fixture discovery should succeed");
            test.check(completion->visitedEntries == 3,
                       "discovery should count valid visible entries");
            test.check(completion->matchedEntries == expectedMatches,
                       "discovery should preserve match counts");
            test.check(completion->skippedSymlinks == 1,
                       "discovery should count skipped symbolic links");
            test.check(completion->depthLimits == 1,
                       "discovery should count directories capped by depth");
            test.check(completion->invalidNames == 2,
                       "discovery should count unsafe remote names");
            test.check(completion->unknownSizes == 1,
                       "discovery should count files with unknown sizes");
        };
    checkSummary(traversalCompletion, 2,
                 "traversal completion should be available");
    checkSummary(searchCompletion, 1, "search completion should be available");

    test.check(
        std::none_of(emittedPaths.cbegin(), emittedPaths.cend(),
                     [](const QString &path) {
                         return path.contains(QStringLiteral("..")) ||
                                path.contains(QStringLiteral("nested/escape"));
                     }),
        "unsafe remote names must never escape into streamed relative paths");
    {
        std::lock_guard lock(state->mutex);
        test.check(std::find(state->calls.cbegin(), state->calls.cend(),
                             "list:/summary/deep") == state->calls.cend(),
                   "depth-limited directories must not issue a child listing");
    }
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    TestContext test;
    testTypedOperationsAreSerialized(test);
    testCancellationAndGenerationReplacement(test);
    testExplicitCancellationAndShutdown(test);
    testChecksumCancellationKeepsEventLoopResponsive(test);
    testListRejectsUnsafeNames(test);
    testRecursiveMutationsRejectUnsafeNames(test);
    testTraversalPauseProvidesBackpressure(test);
    testHealthChecksYieldToUserWork(test);
    testDiscoverySummaryCountersAndConfinement(test);

    if (test.failures == 0) {
        std::cout << "All remote operation controller tests passed\n";
        return 0;
    }
    std::cerr << test.failures
              << " remote operation controller test(s) failed\n";
    return 1;
}
