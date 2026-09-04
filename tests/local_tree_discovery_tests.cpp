#include "QtTestSupport.hpp"
#include "TestHarness.hpp"
#include "logic/remote/LocalTreeDiscovery.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QElapsedTimer>
#include <QEventLoop>
#include <QFile>
#include <QTemporaryDir>
#include <QThread>
#include <QTimer>

#include <algorithm>
#include <chrono>
#include <functional>

namespace {

bool spinUntil(const std::function<bool()> &predicate, int timeoutMs = 5000) {
    return openscp::testsupport::spinUntil(predicate, timeoutMs);
}

bool writeFile(const QString &path, QByteArray contents = "x") {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly))
        return false;
    return file.write(contents) == contents.size();
}

OPENSCP_TEST(testBatchesEmptyFoldersAndEventLoop, test) {
    QTemporaryDir root;
    test.check(root.isValid(), "temporary discovery root should initialize");
    test.check(QDir().mkpath(root.filePath("empty")),
               "empty directory fixture should initialize");
    test.check(QDir().mkpath(root.filePath("nested/deeper")),
               "nested directory fixture should initialize");
    for (int index = 0; index < 511; ++index) {
        test.check(writeFile(root.filePath(
                       QStringLiteral("nested/file-%1").arg(index))),
                   "batch fixture file should be writable");
    }

    LocalTreeDiscovery discovery;
    LocalTreeDiscoveryOptions options;
    options.roots = {{root.path()}};
    bool eventLoopAdvanced = false;
    bool finished = false;
    QVector<LocalTreeDiscoveryEntry> entries;
    int maximumBatchSize = 0;
    QObject::connect(&discovery, &LocalTreeDiscovery::batchReady, &discovery,
                     [&](const LocalTreeDiscoveryBatch &batch) {
                         maximumBatchSize = std::max(maximumBatchSize,
                                                     int(batch.entries.size()));
                         entries += batch.entries;
                     });
    QObject::connect(
        &discovery, &LocalTreeDiscovery::finished, &discovery,
        [&](const LocalTreeDiscoveryCounters &) { finished = true; });
    QTimer::singleShot(0, &discovery, [&] { eventLoopAdvanced = true; });

    discovery.start(options);
    test.check(spinUntil([&] { return finished; }), "discovery should finish");
    test.check(eventLoopAdvanced, "discovery must not block the Qt event loop");
    test.check(maximumBatchSize <= 250,
               "default discovery batches must contain at most 250 items");
    const auto emptyDirectory = std::find_if(
        entries.cbegin(), entries.cend(),
        [&](const LocalTreeDiscoveryEntry &entry) {
            return entry.type == LocalTreeDiscoveryEntry::Type::Directory &&
                   entry.localPath == root.filePath("empty");
        });
    test.check(emptyDirectory != entries.cend(),
               "empty directories should be explicit discovery entries");
}

OPENSCP_TEST(testCancellation, test) {
    QTemporaryDir root;
    test.check(root.isValid(), "cancellation root should initialize");
    for (int index = 0; index < 100; ++index) {
        test.check(
            writeFile(root.filePath(QStringLiteral("file-%1").arg(index))),
            "cancellation fixture file should be writable");
    }

    LocalTreeDiscovery discovery;
    LocalTreeDiscoveryOptions options;
    options.roots = {{root.path()}};
    options.batchSize = 1;
    bool canceled = false;
    int batches = 0;
    QObject::connect(&discovery, &LocalTreeDiscovery::batchReady, &discovery,
                     [&](const LocalTreeDiscoveryBatch &) {
                         ++batches;
                         if (batches == 1)
                             discovery.cancel();
                     });
    QObject::connect(
        &discovery, &LocalTreeDiscovery::canceled, &discovery,
        [&](const LocalTreeDiscoveryCounters &) { canceled = true; });

    discovery.start(options);
    test.check(spinUntil([&] { return canceled; }),
               "cancel should stop discovery and emit canceled");
    test.check(batches < 101,
               "cancel should prevent the remaining entries from batching");
}

OPENSCP_TEST(testLargeTreeConfirmationAndDepthCounters, test) {
    QTemporaryDir root;
    test.check(root.isValid(), "limit root should initialize");
    test.check(QDir().mkpath(root.filePath("a/b/c")),
               "depth fixture should initialize");
    test.check(writeFile(root.filePath("one")) &&
                   writeFile(root.filePath("two")) &&
                   writeFile(root.filePath("three")),
               "limit fixture files should initialize");
    const bool madeSymlink =
        QFile::link(root.filePath("one"), root.filePath("one-link"));
    const bool madeInvalidName =
        writeFile(root.filePath(QStringLiteral("invalid\nname")));

    LocalTreeDiscovery discovery;
    LocalTreeDiscoveryOptions options;
    options.roots = {{root.path()}};
    options.batchSize = 2;
    options.confirmationItemLimit = 2;
    options.maximumDepth = 1;
    bool finished = false;
    int confirmations = 0;
    LocalTreeDiscoveryCounters finalCounters;
    QObject::connect(&discovery,
                     &LocalTreeDiscovery::largeTreeConfirmationRequired,
                     &discovery, [&](const LocalTreeDiscoveryCounters &) {
                         ++confirmations;
                         discovery.continueAfterLargeTreeConfirmation();
                     });
    QObject::connect(&discovery, &LocalTreeDiscovery::finished, &discovery,
                     [&](const LocalTreeDiscoveryCounters &counters) {
                         finalCounters = counters;
                         finished = true;
                     });

    discovery.start(options);
    test.check(spinUntil([&] { return finished; }),
               "confirmed large discovery should continue");
    test.check(confirmations == 1,
               "large-tree confirmation should be requested only once");
    test.check(finalCounters.depthLimits > 0,
               "maximum-depth omissions should be counted");
    if (madeSymlink) {
        test.check(finalCounters.skippedSymlinks == 1,
                   "symbolic-link omissions should be counted");
    }
    if (madeInvalidName) {
        test.check(finalCounters.invalidNames == 1,
                   "unsafe local names should be counted");
    }
}

OPENSCP_TEST(testBackpressureHysteresis, test) {
    QTemporaryDir root;
    test.check(root.isValid(), "backpressure root should initialize");
    for (int index = 0; index < 20; ++index) {
        test.check(
            writeFile(root.filePath(QStringLiteral("file-%1").arg(index))),
            "backpressure fixture file should be writable");
    }

    LocalTreeDiscovery discovery;
    LocalTreeDiscoveryOptions options;
    options.roots = {{root.path()}};
    options.batchSize = 2;
    options.pendingHighWatermark = 2;
    options.pendingLowWatermark = 1;
    bool finished = false;
    int batches = 0;
    QObject::connect(&discovery, &LocalTreeDiscovery::batchReady, &discovery,
                     [&](const LocalTreeDiscoveryBatch &) {
                         ++batches;
                         if (batches == 2)
                             discovery.setPendingTaskCount(2);
                     });
    QObject::connect(
        &discovery, &LocalTreeDiscovery::finished, &discovery,
        [&](const LocalTreeDiscoveryCounters &) { finished = true; });

    discovery.start(options);
    test.check(spinUntil([&] { return batches >= 2; }),
               "backpressure fixture should produce initial batches");
    QElapsedTimer pauseTimer;
    pauseTimer.start();
    while (pauseTimer.elapsed() < 100) {
        QCoreApplication::processEvents(QEventLoop::AllEvents, 10);
        QThread::msleep(1);
    }
    test.check(batches == 2 && !finished,
               "reaching the high watermark should pause further discovery");
    discovery.setPendingTaskCount(0);
    test.check(spinUntil([&] { return finished; }),
               "dropping below the low watermark should resume discovery");
    test.check(batches > 2,
               "resumed discovery should deliver remaining batches");
}

} // namespace

int main(int argc, char **argv) {
    openscp::test::TestHarness harness("local tree discovery");
    return harness.runWithApplication<QCoreApplication>(argc, argv);
}
