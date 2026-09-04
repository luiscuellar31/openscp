#include "logic/sync/SyncCoordinator.hpp"

#include "logic/navigation/RemotePath.hpp"
#include "logic/remote/RemoteOperationController.hpp"
#include "logic/transfers/TransferManager.hpp"
#include "sync/SyncComparisonEngine.hpp"

#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QMetaObject>
#include <QPointer>
#include <QSet>

#include <algorithm>
#include <chrono>
#include <limits>
#include <utility>
#include <vector>

namespace {

constexpr quint64 kLargeTreeItems = 100'000;
constexpr quint64 kLargeTreeKnownBytes = 100ULL * 1024ULL * 1024ULL * 1024ULL;
constexpr int kMaximumDepth = 32;
constexpr int kBatchSize = 250;

QString joinedLocalPath(const QString &root, const QString &relativePath) {
    const QString relative =
        SyncComparisonEngine::normalizeRelativePath(relativePath);
    return relative.isEmpty() ? QDir::cleanPath(root)
                              : QDir(root).filePath(relative);
}

bool exceedsLargeTreeThreshold(quint64 itemCount, quint64 knownBytes) {
    return itemCount > kLargeTreeItems || knownBytes > kLargeTreeKnownBytes;
}

} // namespace

struct SyncCoordinator::PreparationState {
    quint64 generation = 0;
    QString localRoot;
    QString remoteRoot;
    bool allowLargeTree = false;
    std::shared_ptr<std::atomic_bool> canceled =
        std::make_shared<std::atomic_bool>(false);
    RemoteOperationController::JobId remoteJobId = 0;
    bool localDone = false;
    bool remoteDone = false;
    bool limitExceeded = false;
    bool userCanceled = false;
    bool emitted = false;
    QString fatalError;
    SyncPreparationResult result;
};

struct SyncCoordinator::ChecksumState {
    quint64 generation = 0;
    QString localRoot;
    QString remoteRoot;
    QStringList relativePaths;
    std::shared_ptr<std::atomic_bool> canceled =
        std::make_shared<std::atomic_bool>(false);
    QHash<RemoteOperationController::JobId, QString> pendingRemoteJobs;
    SyncChecksumResult result;
    qsizetype completedChecksums = 0;
    bool localFinished = false;
    bool emitted = false;
};

SyncCoordinator::SyncCoordinator(RemoteOperationController *remoteOperations,
                                 TransferManager *transfers, QObject *parent)
    : QObject(parent), remoteOperations_(remoteOperations),
      transfers_(transfers) {
    Q_ASSERT(remoteOperations_);
    Q_ASSERT(transfers_);

    connect(
        remoteOperations_, &RemoteOperationController::entriesBatchReady, this,
        [this](const RemoteOperationController::EntryBatch &batch) {
            const auto state = state_;
            if (!state || state->generation != generation_ ||
                batch.job.id != state->remoteJobId || state->userCanceled) {
                return;
            }

            for (const auto &entry : batch.entries) {
                const QString relative =
                    SyncComparisonEngine::normalizeRelativePath(
                        entry.relativePath);
                if (relative.isEmpty()) {
                    ++state->result.invalidNames;
                    continue;
                }

                SyncSnapshotEntry snapshot;
                snapshot.relativePath = relative;
                snapshot.type =
                    entry.isSymlink
                        ? SyncEntryType::SymbolicLink
                        : (entry.info.is_dir ? SyncEntryType::Directory
                                             : SyncEntryType::File);
                if (!entry.info.is_dir) {
                    if (entry.info.has_size) {
                        snapshot.size = entry.info.size;
                        if (entry.info.size >
                            std::numeric_limits<quint64>::max() -
                                state->result.knownBytes) {
                            state->result.knownBytes =
                                std::numeric_limits<quint64>::max();
                        } else {
                            state->result.knownBytes += entry.info.size;
                        }
                    }
                }
                if (entry.info.mtime > 0 &&
                    entry.info.mtime <=
                        quint64(std::numeric_limits<qint64>::max() / 1000)) {
                    snapshot.modifiedMs = qint64(entry.info.mtime) * 1000;
                }
                snapshot.metadataReliable = entry.info.is_dir ||
                                            entry.info.has_size ||
                                            entry.info.mtime > 0;
                state->result.remoteSnapshot.push_back(std::move(snapshot));
                ++state->result.itemCount;

                if (!state->allowLargeTree &&
                    exceedsLargeTreeThreshold(state->result.itemCount,
                                              state->result.knownBytes)) {
                    state->limitExceeded = true;
                    state->canceled->store(true);
                    remoteOperations_->cancel(state->remoteJobId);
                    break;
                }
            }
            emit progressChanged(state->result.itemCount,
                                 state->result.knownBytes, state->remoteRoot);
        });

    connect(remoteOperations_, &RemoteOperationController::jobProgress, this,
            [this](const RemoteOperationController::Progress &progress) {
                const auto state = state_;
                if (!state || progress.job.id != state->remoteJobId ||
                    state->generation != generation_) {
                    return;
                }
                emit progressChanged(state->result.itemCount,
                                     state->result.knownBytes,
                                     progress.currentPath);
            });

    connect(remoteOperations_, &RemoteOperationController::jobFinished, this,
            [this](const RemoteOperationController::Completion &completion) {
                const auto state = state_;
                if (!state || completion.result.job.id != state->remoteJobId ||
                    state->generation != generation_) {
                    return;
                }
                state->remoteDone = true;
                state->result.inaccessibleFolders += completion.failedEntries;
                state->result.skippedSymlinks += completion.skippedSymlinks;
                state->result.depthLimits += completion.depthLimits;
                state->result.invalidNames += completion.invalidNames;
                state->result.unknownSizes += completion.unknownSizes;
                if (completion.result.partial) {
                    state->result.warnings.push_back(
                        tr("Some remote folders could not be read; the "
                           "comparison is partial."));
                }
                if (completion.result.outcome ==
                        RemoteOperationController::Outcome::Failed &&
                    !completion.result.partial && !state->limitExceeded) {
                    state->fatalError =
                        completion.result.error.isEmpty()
                            ? tr("Could not scan the remote folder.")
                            : completion.result.error;
                }
                maybeFinish(state);
            });

    connect(remoteOperations_, &RemoteOperationController::checksumCompleted,
            this,
            [this](const RemoteOperationController::ChecksumResult &result) {
                const auto state = checksumState_;
                if (!state || state->generation != checksumGeneration_ ||
                    state->emitted) {
                    return;
                }
                const auto pending =
                    state->pendingRemoteJobs.find(result.result.job.id);
                if (pending == state->pendingRemoteJobs.end())
                    return;

                const QString relativePath = pending.value();
                state->pendingRemoteJobs.erase(pending);
                ++state->completedChecksums;
                if (result.result.outcome ==
                        RemoteOperationController::Outcome::Succeeded &&
                    !result.digest.isEmpty()) {
                    state->result.remoteChecksums.insert(relativePath,
                                                         result.digest);
                } else {
                    const QString error =
                        result.result.error.isEmpty()
                            ? tr("Could not calculate the remote checksum.")
                            : result.result.error;
                    state->result.failures.push_back(
                        tr("%1: %2").arg(relativePath, error));
                }
                emit checksumProgressChanged(
                    state->completedChecksums, state->relativePaths.size() * 2,
                    relativePath, result.processedBytes, result.totalBytes);
                maybeFinishChecksums(state);
            });

    connect(remoteOperations_, &RemoteOperationController::jobProgress, this,
            [this](const RemoteOperationController::Progress &progress) {
                const auto state = checksumState_;
                if (!state || state->generation != checksumGeneration_ ||
                    !state->pendingRemoteJobs.contains(progress.job.id)) {
                    return;
                }
                emit checksumProgressChanged(
                    state->completedChecksums, state->relativePaths.size() * 2,
                    state->pendingRemoteJobs.value(progress.job.id),
                    progress.processedBytes, progress.totalBytes);
            });
}

SyncCoordinator::~SyncCoordinator() {
    stopChecksumState(false);
    stopState(false);
}

bool SyncCoordinator::isPreparing() const {
    return state_ && !state_->emitted;
}

void SyncCoordinator::stopState(bool emitCanceled) {
    const auto stale = std::move(state_);
    ++generation_;
    if (stale) {
        stale->userCanceled = true;
        stale->canceled->store(true);
        if (remoteOperations_ && stale->remoteJobId != 0)
            remoteOperations_->cancel(stale->remoteJobId);
    }
    if (localWorker_.joinable()) {
        localWorker_.request_stop();
        localWorker_.join();
    }
    if (emitCanceled && stale)
        emit preparationCanceled();
}

void SyncCoordinator::cancel() {
    stopState(true);
}

bool SyncCoordinator::isCalculatingChecksums() const {
    return checksumState_ && !checksumState_->emitted;
}

void SyncCoordinator::stopChecksumState(bool emitCanceled) {
    const auto stale = std::move(checksumState_);
    ++checksumGeneration_;
    if (stale) {
        stale->canceled->store(true);
        for (auto it = stale->pendingRemoteJobs.cbegin();
             it != stale->pendingRemoteJobs.cend(); ++it) {
            if (remoteOperations_)
                remoteOperations_->cancel(it.key());
        }
    }
    if (checksumWorker_.joinable()) {
        checksumWorker_.request_stop();
        checksumWorker_.join();
    }
    if (emitCanceled && stale && !stale->emitted) {
        stale->emitted = true;
        emit checksumCanceled();
    }
}

void SyncCoordinator::cancelChecksums() {
    stopChecksumState(true);
}

void SyncCoordinator::startChecksums(const QString &localRoot,
                                     const QString &remoteRoot,
                                     const QStringList &relativePaths) {
    stopChecksumState(false);

    const QFileInfo localRootInfo(localRoot);
    if (!localRootInfo.exists() || !localRootInfo.isDir()) {
        emit checksumFailed(tr("The local folder is no longer available."));
        return;
    }
    if (!remoteOperations_ || !remoteOperations_->hasRequestedSession()) {
        emit checksumFailed(tr("No remote session is available."));
        return;
    }

    QSet<QString> uniquePaths;
    for (const QString &rawPath : relativePaths) {
        const QString normalized =
            SyncComparisonEngine::normalizeRelativePath(rawPath);
        if (!normalized.isEmpty())
            uniquePaths.insert(normalized);
    }
    QStringList paths = uniquePaths.values();
    std::sort(paths.begin(), paths.end());
    if (paths.isEmpty()) {
        emit checksumFailed(
            tr("Select at least one file that exists on both sides."));
        return;
    }

    auto state = std::make_shared<ChecksumState>();
    state->generation = checksumGeneration_;
    state->localRoot = QDir(localRootInfo.absoluteFilePath()).absolutePath();
    state->remoteRoot = normalizeRemotePath(remoteRoot);
    state->relativePaths = std::move(paths);
    checksumState_ = state;

    QPointer<SyncCoordinator> safeThis(this);
    checksumWorker_ = std::jthread([safeThis,
                                    state](std::stop_token stopToken) {
        QHash<QString, QByteArray> localChecksums;
        QStringList failures;
        QSet<QString> failedPaths;
        qsizetype completedChecksums = 0;
        const QString canonicalRoot =
            QFileInfo(state->localRoot).canonicalFilePath();
        const QString confinedPrefix = canonicalRoot + QDir::separator();
        auto canceled = [&] {
            return stopToken.stop_requested() || state->canceled->load();
        };
        auto postProgress = [safeThis, state](qsizetype completed,
                                              const QString &relativePath,
                                              quint64 processedBytes,
                                              quint64 totalBytes) {
            if (!safeThis)
                return;
            QMetaObject::invokeMethod(
                safeThis,
                [safeThis, state, completed, relativePath, processedBytes,
                 totalBytes] {
                    if (!safeThis || safeThis->checksumState_ != state ||
                        state->generation != safeThis->checksumGeneration_) {
                        return;
                    }
                    emit safeThis->checksumProgressChanged(
                        completed, state->relativePaths.size() * 2,
                        relativePath, processedBytes, totalBytes);
                },
                Qt::QueuedConnection);
        };

        for (const QString &relativePath : state->relativePaths) {
            if (canceled())
                break;

            const QString localPath =
                joinedLocalPath(state->localRoot, relativePath);
            const QFileInfo fileInfo(localPath);
            const QString canonicalFile = fileInfo.canonicalFilePath();
            const bool confined = !canonicalRoot.isEmpty() &&
                                  !canonicalFile.isEmpty() &&
                                  canonicalFile.startsWith(confinedPrefix);
            if (!confined || fileInfo.isSymLink() || !fileInfo.isFile()) {
                failures.push_back(
                    SyncCoordinator::tr(
                        "%1: the local file is unavailable or outside "
                        "the selected root.")
                        .arg(relativePath));
                failedPaths.insert(relativePath);
                completedChecksums += 2;
                postProgress(completedChecksums, relativePath, 0, 0);
                continue;
            }

            QFile file(localPath);
            if (!file.open(QIODevice::ReadOnly)) {
                failures.push_back(
                    SyncCoordinator::tr(
                        "%1: the local file could not be opened.")
                        .arg(relativePath));
                failedPaths.insert(relativePath);
                completedChecksums += 2;
                postProgress(completedChecksums, relativePath, 0, 0);
                continue;
            }

            QCryptographicHash hash(QCryptographicHash::Sha256);
            const quint64 totalBytes =
                fileInfo.size() > 0 ? static_cast<quint64>(fileInfo.size()) : 0;
            quint64 processedBytes = 0;
            auto lastProgress =
                std::chrono::steady_clock::now() - std::chrono::seconds(1);
            bool readFailed = false;
            while (!file.atEnd() && !canceled()) {
                const QByteArray chunk = file.read(256 * 1024);
                if (chunk.isEmpty() && file.error() != QFile::NoError) {
                    readFailed = true;
                    break;
                }
                hash.addData(chunk);
                processedBytes += static_cast<quint64>(chunk.size());
                const auto now = std::chrono::steady_clock::now();
                if (now - lastProgress >= std::chrono::milliseconds(100)) {
                    lastProgress = now;
                    postProgress(completedChecksums, relativePath,
                                 processedBytes, totalBytes);
                }
            }
            file.close();
            if (canceled())
                break;
            if (readFailed) {
                failures.push_back(
                    SyncCoordinator::tr("%1: the local file could not be read.")
                        .arg(relativePath));
                failedPaths.insert(relativePath);
                completedChecksums += 2;
                postProgress(completedChecksums, relativePath, processedBytes,
                             totalBytes);
                continue;
            }

            localChecksums.insert(relativePath, hash.result());
            ++completedChecksums;
            postProgress(completedChecksums, relativePath, processedBytes,
                         totalBytes);
        }

        if (!safeThis)
            return;
        const bool wasCanceled = canceled();
        QMetaObject::invokeMethod(
            safeThis,
            [safeThis, state, localChecksums = std::move(localChecksums),
             failures = std::move(failures),
             failedPaths = std::move(failedPaths), completedChecksums,
             wasCanceled]() mutable {
                if (!safeThis || safeThis->checksumState_ != state ||
                    state->generation != safeThis->checksumGeneration_) {
                    return;
                }
                if (wasCanceled || state->canceled->load()) {
                    safeThis->stopChecksumState(true);
                    return;
                }

                state->result.localChecksums = std::move(localChecksums);
                state->result.failures = std::move(failures);
                state->completedChecksums = completedChecksums;
                state->localFinished = true;

                for (const QString &relativePath : state->relativePaths) {
                    if (failedPaths.contains(relativePath))
                        continue;
                    RemoteOperationController::ChecksumRequest request;
                    request.path =
                        joinRemotePath(state->remoteRoot, relativePath);
                    const auto jobId =
                        safeThis->remoteOperations_->submit(request);
                    if (jobId == 0) {
                        ++state->completedChecksums;
                        state->result.failures.push_back(
                            SyncCoordinator::tr(
                                "%1: the remote checksum could not be "
                                "started.")
                                .arg(relativePath));
                        continue;
                    }
                    state->pendingRemoteJobs.insert(jobId, relativePath);
                }
                emit safeThis->checksumProgressChanged(
                    state->completedChecksums, state->relativePaths.size() * 2,
                    QString(), 0, 0);
                safeThis->maybeFinishChecksums(state);
            },
            Qt::QueuedConnection);
    });
}

void SyncCoordinator::maybeFinishChecksums(
    const std::shared_ptr<ChecksumState> &state) {
    if (!state || checksumState_ != state || state->emitted ||
        !state->localFinished || !state->pendingRemoteJobs.isEmpty()) {
        return;
    }
    state->emitted = true;
    if (state->canceled->load()) {
        emit checksumCanceled();
        return;
    }
    bool hasComparablePair = false;
    for (auto it = state->result.localChecksums.cbegin();
         it != state->result.localChecksums.cend(); ++it) {
        if (state->result.remoteChecksums.contains(it.key())) {
            hasComparablePair = true;
            break;
        }
    }
    if (!hasComparablePair) {
        emit checksumFailed(
            state->result.failures.isEmpty()
                ? tr("No checksums could be calculated.")
                : state->result.failures.join(QLatin1Char('\n')));
        return;
    }
    emit checksumReady(state->result);
}

void SyncCoordinator::start(const QString &localRoot, const QString &remoteRoot,
                            bool allowLargeTree) {
    stopState(false);

    const QFileInfo localInfo(localRoot);
    if (!localInfo.exists() || !localInfo.isDir()) {
        emit preparationFailed(tr("The current local folder is unavailable."));
        return;
    }
    if (!remoteOperations_ || !remoteOperations_->hasRequestedSession()) {
        emit preparationFailed(tr("No remote session is available."));
        return;
    }

    auto state = std::make_shared<PreparationState>();
    state->generation = generation_;
    state->localRoot = QDir(localInfo.absoluteFilePath()).absolutePath();
    state->remoteRoot = normalizeRemotePath(remoteRoot);
    state->allowLargeTree = allowLargeTree;
    state->result.localRoot = state->localRoot;
    state->result.remoteRoot = state->remoteRoot;
    state_ = state;
    retryLocalRoot_ = state->localRoot;
    retryRemoteRoot_ = state->remoteRoot;

    RemoteOperationController::TraverseRequest request;
    request.rootPath = state->remoteRoot;
    request.includeDirectories = true;
    request.traversal.includeHidden = true;
    request.traversal.skipSymlinks = true;
    request.traversal.maxDepth = kMaximumDepth;
    request.traversal.batchSize = kBatchSize;
    state->remoteJobId = remoteOperations_->submit(request);
    if (state->remoteJobId == 0) {
        state_.reset();
        emit preparationFailed(tr("Could not start the remote scan."));
        return;
    }

    QPointer<SyncCoordinator> safeThis(this);
    localWorker_ = std::jthread([safeThis, state](std::stop_token stopToken) {
        struct DirectoryNode {
            QString absolutePath;
            QString relativePath;
            int depth = 0;
        };

        QVector<SyncSnapshotEntry> snapshot;
        std::vector<DirectoryNode> stack{{state->localRoot, QString(), 0}};
        quint64 itemCount = 0;
        quint64 knownBytes = 0;
        quint64 skippedSymlinks = 0;
        quint64 inaccessible = 0;
        quint64 invalidNames = 0;
        quint64 unknownSizes = 0;
        quint64 depthLimits = 0;
        bool limitExceeded = false;

        auto canceled = [&] {
            return stopToken.stop_requested() || state->canceled->load();
        };
        auto postProgress = [&](const QString &path) {
            if (!safeThis)
                return;
            QMetaObject::invokeMethod(
                safeThis,
                [safeThis, state, itemCount, knownBytes, path] {
                    if (!safeThis || safeThis->state_ != state ||
                        state->generation != safeThis->generation_) {
                        return;
                    }
                    emit safeThis->progressChanged(
                        state->result.itemCount + itemCount,
                        state->result.knownBytes + knownBytes, path);
                },
                Qt::QueuedConnection);
        };

        while (!stack.empty() && !canceled()) {
            DirectoryNode node = std::move(stack.back());
            stack.pop_back();
            const QFileInfo directoryInfo(node.absolutePath);
            if (!directoryInfo.isReadable()) {
                ++inaccessible;
                continue;
            }

            QDir directory(node.absolutePath);
            const QFileInfoList entries = directory.entryInfoList(
                QDir::AllEntries | QDir::NoDotAndDotDot | QDir::Hidden |
                    QDir::System,
                QDir::DirsFirst | QDir::Name);
            for (const QFileInfo &info : entries) {
                if (canceled())
                    break;
                const QString relative = node.relativePath.isEmpty()
                                             ? info.fileName()
                                             : node.relativePath +
                                                   QStringLiteral("/") +
                                                   info.fileName();
                const QString normalized =
                    SyncComparisonEngine::normalizeRelativePath(relative);
                if (normalized.isEmpty()) {
                    ++invalidNames;
                    continue;
                }
                if (info.isSymLink()) {
                    ++skippedSymlinks;
                    continue;
                }

                SyncSnapshotEntry entry;
                entry.relativePath = normalized;
                entry.type = info.isDir() ? SyncEntryType::Directory
                                          : SyncEntryType::File;
                if (info.isFile()) {
                    const qint64 size = info.size();
                    if (size >= 0) {
                        entry.size = quint64(size);
                        if (quint64(size) >
                            std::numeric_limits<quint64>::max() - knownBytes) {
                            knownBytes = std::numeric_limits<quint64>::max();
                        } else {
                            knownBytes += quint64(size);
                        }
                    } else {
                        ++unknownSizes;
                    }
                }
                const QDateTime modified = info.lastModified();
                if (modified.isValid())
                    entry.modifiedMs = modified.toMSecsSinceEpoch();
                snapshot.push_back(std::move(entry));
                ++itemCount;

                if (info.isDir()) {
                    if (node.depth + 1 >= kMaximumDepth) {
                        ++depthLimits;
                    } else {
                        stack.push_back({info.absoluteFilePath(), normalized,
                                         node.depth + 1});
                    }
                }
                if (!state->allowLargeTree &&
                    exceedsLargeTreeThreshold(itemCount, knownBytes)) {
                    limitExceeded = true;
                    state->canceled->store(true);
                    break;
                }
                if ((itemCount % kBatchSize) == 0)
                    postProgress(info.absoluteFilePath());
            }
        }

        if (!safeThis)
            return;
        QMetaObject::invokeMethod(
            safeThis,
            [safeThis, state, snapshot = std::move(snapshot), itemCount,
             knownBytes, skippedSymlinks, inaccessible, invalidNames,
             unknownSizes, depthLimits, limitExceeded]() mutable {
                if (!safeThis || safeThis->state_ != state ||
                    state->generation != safeThis->generation_) {
                    return;
                }
                state->result.localSnapshot = std::move(snapshot);
                state->result.itemCount += itemCount;
                if (knownBytes > std::numeric_limits<quint64>::max() -
                                     state->result.knownBytes) {
                    state->result.knownBytes =
                        std::numeric_limits<quint64>::max();
                } else {
                    state->result.knownBytes += knownBytes;
                }
                state->result.skippedSymlinks += skippedSymlinks;
                state->result.inaccessibleFolders += inaccessible;
                state->result.invalidNames += invalidNames;
                state->result.unknownSizes += unknownSizes;
                state->result.depthLimits += depthLimits;
                state->limitExceeded = state->limitExceeded || limitExceeded;
                state->localDone = true;
                if (state->limitExceeded && safeThis->remoteOperations_)
                    safeThis->remoteOperations_->cancel(state->remoteJobId);
                safeThis->maybeFinish(state);
            },
            Qt::QueuedConnection);
    });
}

void SyncCoordinator::continueLargeTree() {
    if (retryLocalRoot_.isEmpty() || retryRemoteRoot_.isEmpty())
        return;
    start(retryLocalRoot_, retryRemoteRoot_, true);
}

void SyncCoordinator::maybeFinish(
    const std::shared_ptr<PreparationState> &state) {
    if (!state || state_ != state || state->emitted || !state->localDone ||
        !state->remoteDone) {
        return;
    }
    if (!state->allowLargeTree &&
        exceedsLargeTreeThreshold(state->result.itemCount,
                                  state->result.knownBytes)) {
        state->limitExceeded = true;
    }
    state->emitted = true;
    if (state->userCanceled) {
        emit preparationCanceled();
        return;
    }
    if (state->limitExceeded && !state->allowLargeTree) {
        emit largeTreeConfirmationRequired(state->result.itemCount,
                                           state->result.knownBytes);
        return;
    }
    if (!state->fatalError.isEmpty()) {
        emit preparationFailed(state->fatalError);
        return;
    }
    if (state->result.skippedSymlinks > 0) {
        state->result.warnings.push_back(
            tr("%1 symbolic links were skipped.")
                .arg(state->result.skippedSymlinks));
    }
    if (state->result.depthLimits > 0) {
        state->result.warnings.push_back(
            tr("%1 folders reached the maximum depth of %2.")
                .arg(state->result.depthLimits)
                .arg(kMaximumDepth));
    }
    if (state->result.inaccessibleFolders > 0) {
        state->result.warnings.push_back(
            tr("%1 folders could not be read.")
                .arg(state->result.inaccessibleFolders));
    }
    if (state->result.invalidNames > 0) {
        state->result.warnings.push_back(
            tr("%1 entries with unsafe names were skipped.")
                .arg(state->result.invalidNames));
    }
    if (state->result.unknownSizes > 0) {
        state->result.warnings.push_back(tr("%1 files have an unknown size.")
                                             .arg(state->result.unknownSizes));
    }
    emit preparationReady(state->result);
}

quint64 SyncCoordinator::enqueuePlan(const SyncExecutionPlan &plan,
                                     const QString &localRoot,
                                     const QString &remoteRoot,
                                     const QString &sessionKey,
                                     qsizetype *taskCountOut) {
    if (taskCountOut)
        *taskCountOut = 0;
    if (!transfers_ || plan.empty())
        return 0;

    TransferBatchOptions options;
    options.sessionKey = sessionKey;
    options.conflictPolicy = TransferConflictPolicy::Ask;
    options.operation = TransferOperation::Copy;
    options.batchId = transfers_->createBatch(options);

    qsizetype taskCount = 0;
    quint64 prerequisite = 0;
    auto prepareDependency = [&] { options.dependsOnTaskId = prerequisite; };
    auto record = [&](quint64 taskId) {
        if (taskId == 0)
            return;
        prerequisite = taskId;
        ++taskCount;
    };

    for (const QString &rawRelative : plan.directoriesToCreate) {
        const QString relative =
            SyncComparisonEngine::normalizeRelativePath(rawRelative);
        if (relative.isEmpty())
            continue;
        prepareDependency();
        if (plan.direction == SyncDirection::LocalToRemote) {
            record(transfers_->enqueueRemoteDirectory(
                joinRemotePath(remoteRoot, relative), options));
        } else {
            record(transfers_->enqueueLocalDirectory(
                joinedLocalPath(localRoot, relative), options));
        }
    }

    for (const SyncCopyOperation &copy : plan.copies) {
        const QString relative =
            SyncComparisonEngine::normalizeRelativePath(copy.relativePath);
        if (relative.isEmpty())
            continue;
        prepareDependency();
        if (plan.direction == SyncDirection::LocalToRemote) {
            record(transfers_->enqueueUpload(
                joinedLocalPath(localRoot, relative),
                joinRemotePath(remoteRoot, relative), options));
        } else {
            record(transfers_->enqueueDownload(
                joinRemotePath(remoteRoot, relative),
                joinedLocalPath(localRoot, relative), options));
        }
    }

    // SyncComparisonEngine already orders deletes deepest-first. Chaining the
    // persistent tasks preserves that order even with multiple worker slots.
    for (const SyncDeleteOperation &deletion : plan.deletes) {
        const QString relative =
            SyncComparisonEngine::normalizeRelativePath(deletion.relativePath);
        if (relative.isEmpty())
            continue;
        const bool directory = deletion.type == SyncEntryType::Directory;
        prepareDependency();
        if (plan.direction == SyncDirection::LocalToRemote) {
            record(transfers_->enqueueRemoteDelete(
                joinRemotePath(remoteRoot, relative), directory, options));
        } else {
            record(transfers_->enqueueLocalDelete(
                joinedLocalPath(localRoot, relative), directory, options));
        }
    }

    if (taskCountOut)
        *taskCountOut = taskCount;
    return taskCount > 0 ? options.batchId : 0;
}
