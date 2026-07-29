#include "RemoteOperationController.hpp"
#include "SyncCoordinator.hpp"
#include "TransferManager.hpp"
#include "openscp/MockSftpClient.hpp"

#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QTemporaryDir>
#include <QTimer>

#include <chrono>
#include <algorithm>
#include <functional>
#include <iostream>
#include <thread>

namespace {

using namespace std::chrono_literals;

struct TestContext {
    int failures = 0;

    void check(bool condition, const char *message) {
        if (condition)
            return;
        ++failures;
        std::cerr << "[FAIL] " << message << '\n';
    }
};

bool waitUntil(const std::function<bool()> &predicate,
               std::chrono::milliseconds timeout = 5000ms) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    while (std::chrono::steady_clock::now() < deadline) {
        QCoreApplication::processEvents();
        if (predicate())
            return true;
        std::this_thread::sleep_for(5ms);
    }
    QCoreApplication::processEvents();
    return predicate();
}

std::unique_ptr<openscp::RemoteClient> connectedMock() {
    auto client = std::make_unique<openscp::MockSftpClient>();
    openscp::SessionOptions options;
    options.host = "sync.test";
    options.username = "tester";
    std::string error;
    if (!client->connect(options, error))
        return {};
    return client;
}

class ChecksumMockClient final : public openscp::MockSftpClient {
  public:
    openscp::ProtocolCapabilities capabilities() const override {
        auto result = openscp::MockSftpClient::capabilities();
        result.can_checksum = true;
        return result;
    }

    bool checksum(
        const std::string &remotePath, const std::string &algorithm,
        std::vector<std::uint8_t> &digest, std::string &error,
        std::function<void(std::size_t, std::size_t)> progress,
        std::function<bool()> shouldCancel) override {
        if (algorithm != "SHA-256") {
            error = "Unsupported checksum algorithm";
            setLastOperationError(openscp::RemoteErrorKind::Unsupported,
                                  error);
            return false;
        }
        if (remotePath == "/slow.bin") {
            for (std::size_t done = 0; done < 250; ++done) {
                if (shouldCancel && shouldCancel()) {
                    error = "Checksum calculation canceled";
                    setLastOperationError(
                        openscp::RemoteErrorKind::Canceled, error);
                    return false;
                }
                if (progress)
                    progress(done + 1, 250);
                std::this_thread::sleep_for(2ms);
            }
        }
        const QByteArray content =
            remotePath == "/different.txt" ? QByteArrayLiteral("remote")
                                            : QByteArrayLiteral("same");
        const QByteArray calculated = QCryptographicHash::hash(
            content, QCryptographicHash::Sha256);
        digest.assign(calculated.cbegin(), calculated.cend());
        error.clear();
        return true;
    }
};

std::unique_ptr<openscp::RemoteClient> connectedChecksumMock() {
    auto client = std::make_unique<ChecksumMockClient>();
    openscp::SessionOptions options;
    options.host = "checksum.test";
    options.username = "tester";
    std::string error;
    if (!client->connect(options, error))
        return {};
    return client;
}

void testAsynchronousSnapshots(TestContext &test) {
    QTemporaryDir localRoot;
    test.check(localRoot.isValid(), "local snapshot fixture should initialize");
    QFile localFile(localRoot.filePath("readme.txt"));
    test.check(localFile.open(QIODevice::WriteOnly),
               "local snapshot file should be writable");
    localFile.write("local");
    localFile.close();
    test.check(QDir().mkpath(localRoot.filePath("empty")),
               "local empty directory should be created");

    RemoteOperationController remote;
    test.check(remote.installSession(connectedMock()) > 0,
               "remote controller should accept a connected session");
    TransferManager transfers;
    SyncCoordinator coordinator(&remote, &transfers);

    bool ready = false;
    bool failed = false;
    bool eventLoopAdvanced = false;
    SyncPreparationResult prepared;
    QObject::connect(
        &coordinator, &SyncCoordinator::preparationReady, &coordinator,
        [&](const SyncPreparationResult &result) {
            prepared = result;
            ready = true;
        });
    QObject::connect(&coordinator, &SyncCoordinator::preparationFailed,
                     &coordinator, [&](const QString &) { failed = true; });
    QTimer::singleShot(0, &coordinator,
                       [&] { eventLoopAdvanced = true; });

    coordinator.start(localRoot.path(), QStringLiteral("/"));
    test.check(waitUntil([&] { return ready || failed; }),
               "local and remote snapshots should complete asynchronously");
    test.check(ready && !failed,
               "partial child-list errors should still produce a preview");
    test.check(eventLoopAdvanced,
               "snapshot preparation must not block the Qt event loop");

    const auto contains = [](const QVector<SyncSnapshotEntry> &entries,
                             const QString &path) {
        return std::any_of(
            entries.cbegin(), entries.cend(),
            [&](const SyncSnapshotEntry &entry) {
                return entry.relativePath == path;
            });
    };
    test.check(contains(prepared.localSnapshot, QStringLiteral("readme.txt")) &&
                   contains(prepared.localSnapshot, QStringLiteral("empty")),
               "local files and empty folders should enter the snapshot");
    test.check(contains(prepared.remoteSnapshot, QStringLiteral("readme.txt")),
               "remote controller batches should enter the snapshot");
}

void testPersistentExecutionPlan(TestContext &test) {
    RemoteOperationController remote;
    TransferManager transfers;
    transfers.setSessionIdentity(QStringLiteral("site-id"));
    SyncCoordinator coordinator(&remote, &transfers);

    SyncExecutionPlan plan;
    plan.direction = SyncDirection::LocalToRemote;
    plan.directoriesToCreate = {QStringLiteral("new/folder")};
    plan.copies = {{QStringLiteral("new/folder/file.txt"), 12, false}};
    plan.deletes = {
        {QStringLiteral("old/file.txt"), SyncEntryType::File},
        {QStringLiteral("old"), SyncEntryType::Directory},
    };

    qsizetype taskCount = 0;
    const quint64 batchId = coordinator.enqueuePlan(
        plan, QStringLiteral("/local/root"), QStringLiteral("/remote/root"),
        QStringLiteral("site-id"), &taskCount);
    const auto tasks = transfers.tasksSnapshot();
    test.check(batchId != 0 && taskCount == 4 && tasks.size() == 4,
               "the whole sync plan should become one persistent batch");
    if (tasks.size() != 4)
        return;

    test.check(tasks[0].type ==
                       TransferTask::Type::CreateRemoteDirectory &&
                   tasks[1].type == TransferTask::Type::Upload &&
                   tasks[2].type ==
                       TransferTask::Type::DeleteRemoteFile &&
                   tasks[3].type ==
                       TransferTask::Type::DeleteRemoteDirectory,
               "sync actions should map to queue task types");
    test.check(tasks[0].dst == QStringLiteral("/remote/root/new/folder") &&
                   tasks[1].src ==
                       QStringLiteral("/local/root/new/folder/file.txt"),
               "execution paths should remain confined to their roots");
    test.check(tasks[1].dependsOnTaskId == tasks[0].taskId &&
                   tasks[2].dependsOnTaskId == tasks[1].taskId &&
                   tasks[3].dependsOnTaskId == tasks[2].taskId,
               "persistent dependencies should preserve safe execution order");
    test.check(std::all_of(
                   tasks.cbegin(), tasks.cend(),
                   [batchId](const TransferTask &task) {
                       return task.batchId == batchId &&
                              task.sessionKey == QStringLiteral("site-id");
                   }),
               "every sync task should retain batch and session identity");
}

void testOnDemandChecksums(TestContext &test) {
    QTemporaryDir localRoot;
    test.check(localRoot.isValid(),
               "checksum fixture should initialize");
    const auto writeFile = [&](const QString &name,
                               const QByteArray &content) {
        QFile file(localRoot.filePath(name));
        if (!file.open(QIODevice::WriteOnly))
            return false;
        return file.write(content) == content.size();
    };
    test.check(writeFile(QStringLiteral("same.txt"),
                         QByteArrayLiteral("same")) &&
                   writeFile(QStringLiteral("different.txt"),
                             QByteArrayLiteral("local")),
               "checksum fixture files should be writable");

    RemoteOperationController remote;
    remote.installSession(connectedChecksumMock());
    TransferManager transfers;
    SyncCoordinator coordinator(&remote, &transfers);
    bool ready = false;
    bool failed = false;
    bool eventLoopAdvanced = false;
    SyncChecksumResult result;
    QObject::connect(
        &coordinator, &SyncCoordinator::checksumReady, &coordinator,
        [&](const SyncChecksumResult &value) {
            result = value;
            ready = true;
        });
    QObject::connect(&coordinator, &SyncCoordinator::checksumFailed,
                     &coordinator, [&](const QString &) { failed = true; });
    QTimer::singleShot(0, &coordinator,
                       [&] { eventLoopAdvanced = true; });

    coordinator.startChecksums(
        localRoot.path(), QStringLiteral("/"),
        {QStringLiteral("same.txt"), QStringLiteral("different.txt")});
    test.check(waitUntil([&] { return ready || failed; }),
               "local and remote checksums should complete asynchronously");
    test.check(ready && !failed,
               "supported SFTP checksums should produce a result");
    test.check(eventLoopAdvanced,
               "local checksum calculation must not block the event loop");
    test.check(
        result.localChecksums.value(QStringLiteral("same.txt")) ==
            result.remoteChecksums.value(QStringLiteral("same.txt")),
        "identical files should produce identical SHA-256 values");
    test.check(
        result.localChecksums.value(QStringLiteral("different.txt")) !=
            result.remoteChecksums.value(QStringLiteral("different.txt")),
        "different files should produce different SHA-256 values");
}

void testChecksumCancellation(TestContext &test) {
    QTemporaryDir localRoot;
    QFile slowFile(localRoot.filePath(QStringLiteral("slow.bin")));
    test.check(slowFile.open(QIODevice::WriteOnly),
               "checksum cancellation fixture should open");
    slowFile.write("same");
    slowFile.close();

    RemoteOperationController remote;
    remote.installSession(connectedChecksumMock());
    TransferManager transfers;
    SyncCoordinator coordinator(&remote, &transfers);
    bool remotePhaseStarted = false;
    bool canceled = false;
    QObject::connect(
        &coordinator, &SyncCoordinator::checksumProgressChanged,
        &coordinator,
        [&](qsizetype completed, qsizetype, const QString &path, quint64,
            quint64 totalBytes) {
            if (completed >= 1 && path == QStringLiteral("slow.bin") &&
                totalBytes == 250) {
                remotePhaseStarted = true;
            }
        });
    QObject::connect(&coordinator, &SyncCoordinator::checksumCanceled,
                     &coordinator, [&] { canceled = true; });

    coordinator.startChecksums(localRoot.path(), QStringLiteral("/"),
                               {QStringLiteral("slow.bin")});
    test.check(waitUntil([&] { return remotePhaseStarted; }),
               "checksum cancellation should reach the remote phase");
    coordinator.cancelChecksums();
    test.check(waitUntil([&] { return canceled; }),
               "checksum cancellation should complete cooperatively");
    test.check(!coordinator.isCalculatingChecksums(),
               "a canceled checksum run should release its state");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication app(argc, argv);
    TestContext test;
    testAsynchronousSnapshots(test);
    testPersistentExecutionPlan(test);
    testOnDemandChecksums(test);
    testChecksumCancellation(test);

    if (test.failures == 0) {
        std::cout << "All sync coordinator tests passed\n";
        return 0;
    }
    std::cerr << test.failures << " sync coordinator test(s) failed\n";
    return 1;
}
