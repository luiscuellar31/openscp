#include "LocalTreeDiscovery.hpp"

#include <QDir>
#include <QFileInfo>
#include <QMetaObject>
#include <QSet>

#include <algorithm>
#include <limits>
#include <utility>
#include <vector>

namespace {

bool validRemoteEntryName(const QString &name) {
    if (name.isEmpty() || name == QStringLiteral(".") ||
        name == QStringLiteral("..") || name.contains(QLatin1Char('/')) ||
        name.contains(QLatin1Char('\\'))) {
        return false;
    }
    return std::none_of(name.cbegin(), name.cend(), [](QChar character) {
        const ushort codePoint = character.unicode();
        return codePoint < 0x20u || codePoint == 0x7fu;
    });
}

void addKnownBytes(LocalTreeDiscoveryCounters &counters, quint64 bytes) {
    if (bytes > std::numeric_limits<quint64>::max() - counters.knownBytes) {
        counters.knownBytes = std::numeric_limits<quint64>::max();
    } else {
        counters.knownBytes += bytes;
    }
}

bool exceedsConfirmationLimit(const LocalTreeDiscoveryOptions &options,
                              const LocalTreeDiscoveryCounters &counters) {
    return counters.itemCount > options.confirmationItemLimit ||
           counters.knownBytes > options.confirmationKnownBytesLimit;
}

} // namespace

LocalTreeDiscovery::LocalTreeDiscovery(QObject *parent) : QObject(parent) {
}

LocalTreeDiscovery::~LocalTreeDiscovery() {
    stopWorker();
}

void LocalTreeDiscovery::stopWorker() {
    {
        std::lock_guard lock(controlMutex_);
        cancelRequested_ = true;
        largeTreeDecision_ = LargeTreeDecision::Cancel;
    }
    controlWake_.notify_all();
    if (worker_.joinable()) {
        worker_.request_stop();
        worker_.join();
    }
    running_.store(false);
}

void LocalTreeDiscovery::cancel() {
    {
        std::lock_guard lock(controlMutex_);
        cancelRequested_ = true;
        largeTreeDecision_ = LargeTreeDecision::Cancel;
    }
    if (worker_.joinable())
        worker_.request_stop();
    controlWake_.notify_all();
}

void LocalTreeDiscovery::continueAfterLargeTreeConfirmation() {
    {
        std::lock_guard lock(controlMutex_);
        if (largeTreeDecision_ != LargeTreeDecision::Waiting)
            return;
        largeTreeDecision_ = LargeTreeDecision::Continue;
    }
    controlWake_.notify_all();
}

void LocalTreeDiscovery::setPendingTaskCount(int pendingTaskCount) {
    {
        std::lock_guard lock(controlMutex_);
        pendingTaskCount_ = std::max(0, pendingTaskCount);
        if (!backpressurePaused_ && pendingTaskCount_ >= highWatermark_) {
            backpressurePaused_ = true;
        } else if (backpressurePaused_ && pendingTaskCount_ < lowWatermark_) {
            backpressurePaused_ = false;
        }
    }
    controlWake_.notify_all();
}

bool LocalTreeDiscovery::cancellationRequested(
    std::stop_token stopToken) const {
    if (stopToken.stop_requested())
        return true;
    std::lock_guard lock(controlMutex_);
    return cancelRequested_;
}

bool LocalTreeDiscovery::waitForBackpressure(std::stop_token stopToken) {
    std::unique_lock lock(controlMutex_);
    controlWake_.wait(lock, [&] {
        return cancelRequested_ || stopToken.stop_requested() ||
               !backpressurePaused_;
    });
    return !cancelRequested_ && !stopToken.stop_requested();
}

bool LocalTreeDiscovery::deliverBatch(LocalTreeDiscoveryBatch batch,
                                      quint64 generation,
                                      std::stop_token stopToken) {
    if (batch.entries.isEmpty())
        return !cancellationRequested(stopToken);

    const quint64 sequence = batch.sequence;
    QMetaObject::invokeMethod(
        this,
        [this, generation, batch = std::move(batch), sequence] {
            if (generation_.load() == generation) {
                emit batchReady(batch);
            }
            {
                std::lock_guard lock(controlMutex_);
                deliveredBatchSequence_ =
                    std::max(deliveredBatchSequence_, sequence);
            }
            controlWake_.notify_all();
        },
        Qt::QueuedConnection);

    {
        std::unique_lock lock(controlMutex_);
        controlWake_.wait(lock, [&] {
            return cancelRequested_ || stopToken.stop_requested() ||
                   generation_.load() != generation ||
                   deliveredBatchSequence_ >= sequence;
        });
        if (cancelRequested_ || stopToken.stop_requested() ||
            generation_.load() != generation) {
            return false;
        }
    }
    return waitForBackpressure(stopToken);
}

bool LocalTreeDiscovery::requestLargeTreeConfirmation(
    const LocalTreeDiscoveryCounters &counters, quint64 generation,
    std::stop_token stopToken) {
    {
        std::lock_guard lock(controlMutex_);
        largeTreeDecision_ = LargeTreeDecision::Waiting;
    }
    QMetaObject::invokeMethod(
        this,
        [this, generation, counters] {
            if (generation_.load() == generation)
                emit largeTreeConfirmationRequired(counters);
        },
        Qt::QueuedConnection);

    std::unique_lock lock(controlMutex_);
    controlWake_.wait(lock, [&] {
        return cancelRequested_ || stopToken.stop_requested() ||
               generation_.load() != generation ||
               largeTreeDecision_ != LargeTreeDecision::Waiting;
    });
    return !cancelRequested_ && !stopToken.stop_requested() &&
           generation_.load() == generation &&
           largeTreeDecision_ == LargeTreeDecision::Continue;
}

void LocalTreeDiscovery::start(
    const LocalTreeDiscoveryOptions &requestedOptions) {
    stopWorker();

    LocalTreeDiscoveryOptions options = requestedOptions;
    options.batchSize = std::max(1, options.batchSize);
    options.maximumDepth = std::max(0, options.maximumDepth);
    options.pendingHighWatermark = std::max(1, options.pendingHighWatermark);
    options.pendingLowWatermark = std::clamp(options.pendingLowWatermark, 0,
                                             options.pendingHighWatermark);

    const quint64 generation = generation_.fetch_add(1) + 1;
    {
        std::lock_guard lock(controlMutex_);
        cancelRequested_ = false;
        backpressurePaused_ = false;
        pendingTaskCount_ = 0;
        highWatermark_ = options.pendingHighWatermark;
        lowWatermark_ = options.pendingLowWatermark;
        deliveredBatchSequence_ = 0;
        largeTreeDecision_ = LargeTreeDecision::Waiting;
    }
    running_.store(true);

    worker_ = std::jthread([this, options = std::move(options),
                            generation](std::stop_token stopToken) {
        struct DirectoryNode {
            int rootIndex = -1;
            QString absolutePath;
            QString relativePath;
            int depth = 0;
        };

        LocalTreeDiscoveryCounters counters;
        LocalTreeDiscoveryBatch batch;
        quint64 nextSequence = 1;
        bool confirmationAccepted = false;

        auto postProgress = [&](const QString &path) {
            const auto snapshot = counters;
            QMetaObject::invokeMethod(
                this,
                [this, generation, snapshot, path] {
                    if (generation_.load() == generation)
                        emit progressChanged(snapshot, path);
                },
                Qt::QueuedConnection);
        };
        auto flush = [&]() {
            if (batch.entries.isEmpty())
                return !cancellationRequested(stopToken);
            batch.sequence = nextSequence++;
            batch.counters = counters;
            LocalTreeDiscoveryBatch outgoing;
            std::swap(outgoing, batch);
            return deliverBatch(std::move(outgoing), generation, stopToken);
        };
        auto maybeConfirm = [&]() {
            if (confirmationAccepted ||
                !exceedsConfirmationLimit(options, counters)) {
                return true;
            }
            if (!requestLargeTreeConfirmation(counters, generation,
                                              stopToken)) {
                return false;
            }
            confirmationAccepted = true;
            return true;
        };
        auto appendEntry = [&](LocalTreeDiscoveryEntry entry,
                               const QString &progressPath) {
            batch.entries.push_back(std::move(entry));
            ++counters.itemCount;
            if (!maybeConfirm())
                return false;
            if (batch.entries.size() >= options.batchSize) {
                if (!flush())
                    return false;
                postProgress(progressPath);
            }
            return true;
        };

        QSet<QString> acceptedRoots;
        for (int rootIndex = 0; rootIndex < options.roots.size() &&
                                !cancellationRequested(stopToken);
             ++rootIndex) {
            const QFileInfo requestedInfo(options.roots[rootIndex].localPath);
            const QString absolutePath =
                QDir::cleanPath(requestedInfo.absoluteFilePath());
            const QFileInfo info(absolutePath);

            if (info.isSymLink()) {
                ++counters.skippedSymlinks;
                continue;
            }
            if (!info.exists() || !info.isReadable()) {
                ++counters.inaccessibleEntries;
                continue;
            }
            if (!validRemoteEntryName(info.fileName())) {
                ++counters.invalidNames;
                continue;
            }
            const QString canonical = info.canonicalFilePath().isEmpty()
                                          ? absolutePath
                                          : info.canonicalFilePath();
            bool duplicateOrNested = acceptedRoots.contains(canonical);
            if (!duplicateOrNested) {
                for (const QString &accepted : acceptedRoots) {
                    if (canonical.startsWith(accepted + QDir::separator())) {
                        duplicateOrNested = true;
                        break;
                    }
                }
            }
            if (duplicateOrNested)
                continue;
            acceptedRoots.insert(canonical);

            if (info.isFile()) {
                const qint64 fileSize = info.size();
                LocalTreeDiscoveryEntry entry;
                entry.rootIndex = rootIndex;
                entry.localPath = absolutePath;
                entry.type = LocalTreeDiscoveryEntry::Type::File;
                if (fileSize >= 0) {
                    entry.hasKnownSize = true;
                    entry.size = quint64(fileSize);
                    addKnownBytes(counters, entry.size);
                } else {
                    ++counters.unknownSizes;
                }
                if (!appendEntry(std::move(entry), absolutePath))
                    break;
                continue;
            }
            if (!info.isDir()) {
                ++counters.inaccessibleEntries;
                continue;
            }

            LocalTreeDiscoveryEntry rootEntry;
            rootEntry.rootIndex = rootIndex;
            rootEntry.localPath = absolutePath;
            rootEntry.type = LocalTreeDiscoveryEntry::Type::Directory;
            if (!appendEntry(std::move(rootEntry), absolutePath))
                break;

            std::vector<DirectoryNode> stack{
                {rootIndex, absolutePath, QString(), 0}};
            while (!stack.empty() && !cancellationRequested(stopToken)) {
                DirectoryNode node = std::move(stack.back());
                stack.pop_back();
                if (node.depth >= options.maximumDepth) {
                    QDir depthDirectory(node.absolutePath);
                    if (!depthDirectory
                             .entryList(QDir::AllEntries |
                                            QDir::NoDotAndDotDot |
                                            QDir::Hidden | QDir::System,
                                        QDir::Name)
                             .isEmpty()) {
                        ++counters.depthLimits;
                    }
                    continue;
                }

                const QFileInfo directoryInfo(node.absolutePath);
                if (!directoryInfo.isReadable()) {
                    ++counters.inaccessibleEntries;
                    continue;
                }
                QDir directory(node.absolutePath);
                const QFileInfoList entries = directory.entryInfoList(
                    QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden |
                        QDir::System,
                    QDir::DirsFirst | QDir::Name);
                QVector<DirectoryNode> childDirectories;
                childDirectories.reserve(entries.size());
                for (const QFileInfo &child : entries) {
                    if (cancellationRequested(stopToken))
                        break;
                    if (child.isSymLink()) {
                        ++counters.skippedSymlinks;
                        continue;
                    }
                    if (!validRemoteEntryName(child.fileName())) {
                        ++counters.invalidNames;
                        continue;
                    }
                    if (!child.isReadable()) {
                        ++counters.inaccessibleEntries;
                        continue;
                    }
                    const QString relative = node.relativePath.isEmpty()
                                                 ? child.fileName()
                                                 : node.relativePath +
                                                       QStringLiteral("/") +
                                                       child.fileName();
                    LocalTreeDiscoveryEntry entry;
                    entry.rootIndex = rootIndex;
                    entry.localPath = child.absoluteFilePath();
                    entry.relativePath = relative;
                    if (child.isDir()) {
                        entry.type = LocalTreeDiscoveryEntry::Type::Directory;
                        childDirectories.push_back({rootIndex,
                                                    child.absoluteFilePath(),
                                                    relative, node.depth + 1});
                    } else if (child.isFile()) {
                        entry.type = LocalTreeDiscoveryEntry::Type::File;
                        const qint64 fileSize = child.size();
                        if (fileSize >= 0) {
                            entry.hasKnownSize = true;
                            entry.size = quint64(fileSize);
                            addKnownBytes(counters, entry.size);
                        } else {
                            ++counters.unknownSizes;
                        }
                    } else {
                        ++counters.inaccessibleEntries;
                        continue;
                    }
                    if (!appendEntry(std::move(entry),
                                     child.absoluteFilePath())) {
                        break;
                    }
                }
                for (auto it = childDirectories.crbegin();
                     it != childDirectories.crend(); ++it) {
                    stack.push_back(*it);
                }
            }
        }

        if (!cancellationRequested(stopToken) && !flush())
            cancel();

        const bool wasCanceled = cancellationRequested(stopToken);
        running_.store(false);
        QMetaObject::invokeMethod(
            this,
            [this, generation, counters, wasCanceled] {
                if (generation_.load() != generation)
                    return;
                if (wasCanceled) {
                    emit canceled(counters);
                } else {
                    emit finished(counters);
                }
            },
            Qt::QueuedConnection);
    });
}
