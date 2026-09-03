// Asynchronous, bounded discovery of local files and directories.
#pragma once

#include <QObject>
#include <QString>
#include <QVector>

#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>

struct LocalTreeDiscoveryRoot {
    QString localPath;
};

struct LocalTreeDiscoveryEntry {
    enum class Type { File, Directory };

    int rootIndex = -1;
    QString localPath;
    // Empty for the selected root itself. All separators are '/'.
    QString relativePath;
    Type type = Type::File;
    quint64 size = 0;
};

struct LocalTreeDiscoveryCounters {
    quint64 itemCount = 0;
    quint64 knownBytes = 0;
    quint64 skippedSymlinks = 0;
    quint64 depthLimits = 0;
    quint64 inaccessibleEntries = 0;
    quint64 invalidNames = 0;
    quint64 unknownSizes = 0;
};

struct LocalTreeDiscoveryBatch {
    quint64 sequence = 0;
    QVector<LocalTreeDiscoveryEntry> entries;
    LocalTreeDiscoveryCounters counters;
};

struct LocalTreeDiscoveryOptions {
    QVector<LocalTreeDiscoveryRoot> roots;
    int batchSize = 250;
    int maximumDepth = 32;
    int pendingHighWatermark = 2000;
    int pendingLowWatermark = 1000;
    quint64 confirmationItemLimit = 100'000;
    quint64 confirmationKnownBytesLimit = 100ULL * 1024ULL * 1024ULL * 1024ULL;
};

Q_DECLARE_METATYPE(LocalTreeDiscoveryEntry)
Q_DECLARE_METATYPE(LocalTreeDiscoveryCounters)
Q_DECLARE_METATYPE(LocalTreeDiscoveryBatch)

class LocalTreeDiscovery final : public QObject {
    Q_OBJECT

    public:
    explicit LocalTreeDiscovery(QObject *parent = nullptr);
    ~LocalTreeDiscovery() override;

    void start(const LocalTreeDiscoveryOptions &options);
    void cancel();
    void continueAfterLargeTreeConfirmation();
    void setPendingTaskCount(int pendingTaskCount);
    bool isRunning() const { return running_.load(); }

    signals:
    void batchReady(const LocalTreeDiscoveryBatch &batch);
    void progressChanged(const LocalTreeDiscoveryCounters &counters,
                         const QString &currentPath);
    void
    largeTreeConfirmationRequired(const LocalTreeDiscoveryCounters &counters);
    void finished(const LocalTreeDiscoveryCounters &counters);
    void canceled(const LocalTreeDiscoveryCounters &counters);

    private:
    enum class LargeTreeDecision { Waiting, Continue, Cancel };

    void stopWorker();
    bool cancellationRequested(std::stop_token stopToken) const;
    bool deliverBatch(LocalTreeDiscoveryBatch batch, quint64 generation,
                      std::stop_token stopToken);
    bool waitForBackpressure(std::stop_token stopToken);
    bool
    requestLargeTreeConfirmation(const LocalTreeDiscoveryCounters &counters,
                                 quint64 generation, std::stop_token stopToken);

    std::jthread worker_;
    std::atomic_bool running_{false};
    std::atomic<quint64> generation_{0};

    mutable std::mutex controlMutex_;
    std::condition_variable controlWake_;
    bool cancelRequested_ = false;
    bool backpressurePaused_ = false;
    int pendingTaskCount_ = 0;
    int highWatermark_ = 2000;
    int lowWatermark_ = 1000;
    quint64 deliveredBatchSequence_ = 0;
    LargeTreeDecision largeTreeDecision_ = LargeTreeDecision::Waiting;
};
