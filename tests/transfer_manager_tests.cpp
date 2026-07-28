// Transfer queue unit tests without external framework.
#include "TransferManager.hpp"
#include "openscp/MockSftpClient.hpp"

#include <QObject>

#include <atomic>
#include <chrono>
#include <iostream>
#include <memory>
#include <thread>

namespace {

struct TestContext {
    int failures = 0;

    void check(bool condition, const char *message) {
        if (condition)
            return;
        ++failures;
        std::cerr << "[FAIL] " << message << '\n';
    }
};

void testBatchDownloadEnqueue(TestContext &test) {
    TransferManager manager;
    int changeNotifications = 0;
    QObject::connect(&manager, &TransferManager::tasksChanged, &manager,
                     [&changeNotifications] { ++changeNotifications; });

    QVector<QPair<QString, QString>> downloads;
    downloads.reserve(900);
    for (int index = 0; index < 900; ++index) {
        downloads.push_back(
            {QStringLiteral("/remote/file-%1.dat").arg(index),
             QStringLiteral("/local/file-%1.dat").arg(index)});
    }

    const int added = manager.enqueueDownloads(downloads);
    test.check(added == downloads.size(),
               "batch enqueue should report every added download");
    test.check(changeNotifications == 1,
               "batch enqueue should emit one queue change notification");

    const QVector<TransferTask> snapshot = manager.tasksSnapshot();
    test.check(snapshot.size() == downloads.size(),
               "batch enqueue should preserve every download");
    for (int index = 0; index < snapshot.size(); ++index) {
        const TransferTask &task = snapshot[index];
        test.check(task.taskId == static_cast<quint64>(index + 1),
                   "batch tasks should receive stable sequential IDs");
        test.check(task.type == TransferTask::Type::Download,
                   "batch tasks should be downloads");
        test.check(task.src == downloads[index].first,
                   "batch task should preserve its remote source");
        test.check(task.dst == downloads[index].second,
                   "batch task should preserve its local destination");
        test.check(task.status == TransferTask::Status::Queued,
                   "batch task should start queued");
    }
}

void testConcurrencyUpdates(TestContext &test) {
    TransferManager manager;
    int changeNotifications = 0;
    QObject::connect(&manager, &TransferManager::tasksChanged, &manager,
                     [&changeNotifications] { ++changeNotifications; });

    manager.setMaxConcurrent(4);
    test.check(manager.maxConcurrent() == 4,
               "concurrency should update to the requested value");
    test.check(changeNotifications == 1,
               "changing concurrency should refresh queue indicators");

    manager.setMaxConcurrent(4);
    test.check(changeNotifications == 1,
               "setting the same concurrency should not refresh again");

    manager.setMaxConcurrent(0);
    test.check(manager.maxConcurrent() == 1,
               "concurrency should be clamped to at least one");
}

struct ConcurrencyProbe {
    std::atomic<int> active{0};
    std::atomic<int> maximum{0};
};

class ConcurrentMockClient final : public openscp::MockSftpClient {
    public:
    explicit ConcurrentMockClient(std::shared_ptr<ConcurrencyProbe> probe)
        : probe_(std::move(probe)) {}

    bool get(const std::string &, const std::string &, std::string &err,
             std::function<void(std::size_t, std::size_t)> progress,
             std::function<bool()> shouldCancel, bool) override {
        const int activeNow = probe_->active.fetch_add(1) + 1;
        int observedMaximum = probe_->maximum.load();
        while (activeNow > observedMaximum &&
               !probe_->maximum.compare_exchange_weak(observedMaximum,
                                                       activeNow)) {
        }

        if (progress)
            progress(1, 2);
        for (int tick = 0; tick < 30; ++tick) {
            if (shouldCancel && shouldCancel()) {
                probe_->active.fetch_sub(1);
                err = "Canceled";
                return false;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        if (progress)
            progress(2, 2);
        probe_->active.fetch_sub(1);
        err.clear();
        return true;
    }

    std::unique_ptr<openscp::SftpClient>
    newConnectionLike(const openscp::SessionOptions &options,
                      std::string &err) override {
        auto worker = std::make_unique<ConcurrentMockClient>(probe_);
        if (!worker->connect(options, err))
            return nullptr;
        return worker;
    }

    private:
    std::shared_ptr<ConcurrencyProbe> probe_;
};

void testWorkersRunConcurrently(TestContext &test) {
    auto probe = std::make_shared<ConcurrencyProbe>();
    ConcurrentMockClient baseClient(probe);
    openscp::SessionOptions options;
    options.host = "parallel.test";
    options.username = "tester";
    std::string connectError;
    test.check(baseClient.connect(options, connectError),
               "concurrency mock should connect");

    TransferManager manager;
    manager.setSessionOptions(options);
    manager.setClient(&baseClient);
    manager.setMaxConcurrent(4);

    QVector<QPair<QString, QString>> downloads;
    for (int index = 0; index < 8; ++index) {
        downloads.push_back(
            {QStringLiteral("/remote/parallel-%1.dat").arg(index),
             QStringLiteral("/tmp/openscp-transfer-manager-test/file-%1.dat")
                 .arg(index)});
    }
    manager.enqueueDownloads(downloads);

    for (int waitTick = 0;
         waitTick < 200 && probe->maximum.load() < manager.maxConcurrent();
         ++waitTick) {
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    test.check(probe->maximum.load() == 4,
               "four configured workers should transfer concurrently");
}

} // namespace

int main() {
    TestContext test;
    testBatchDownloadEnqueue(test);
    testConcurrencyUpdates(test);
    testWorkersRunConcurrently(test);

    if (test.failures == 0) {
        std::cout << "All transfer manager tests passed\n";
        return 0;
    }
    std::cerr << test.failures << " transfer manager test(s) failed\n";
    return 1;
}
