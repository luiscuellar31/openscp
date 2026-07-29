// MainWindow transfer queue UI, remote prescan, and drag-and-drop handling.
#include "MainWindow.hpp"
#include "LocalTreeDiscovery.hpp"
#include "MainWindowSharedUtils.hpp"
#include "SessionController.hpp"
#include "RemoteModel.hpp"
#include "RemoteOperationController.hpp"
#include "TransferQueueDialog.hpp"
#include "UiAlerts.hpp"

#include <QAbstractAnimation>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEasingCurve>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QMimeData>
#include <QParallelAnimationGroup>
#include <QProgressDialog>
#include <QPropertyAnimation>
#include <QScreen>
#include <QStatusBar>
#include <QStringList>
#include <QTimer>
#include <QThreadPool>

#include <algorithm>
#include <atomic>
#include <limits>
#include <memory>

static constexpr int NAME_COL = 0;
static const char *kStagingBatchMime =
    "application/x-openscp-staging-batch";

static bool isLocalUploadPreparationTerminal(
    TransferTask::Status status) {
    switch (status) {
    case TransferTask::Status::Done:
    case TransferTask::Status::Error:
    case TransferTask::Status::Canceled:
    case TransferTask::Status::Skipped:
    case TransferTask::Status::Warning:
        return true;
    case TransferTask::Status::Queued:
    case TransferTask::Status::Running:
    case TransferTask::Status::Paused:
    case TransferTask::Status::WaitingForConnection:
    case TransferTask::Status::RetryWaiting:
        return false;
    }
    return false;
}

static QRect centeredQueueRect(QWidget *dialog, QWidget *mainWindow) {
    if (!dialog)
        return {};

    QRect rect = dialog->geometry();
    if (!rect.isValid())
        rect = QRect(QPoint(0, 0), dialog->size());

    if (mainWindow) {
        QRect anchor = mainWindow->frameGeometry();
        if (!anchor.isValid())
            anchor = mainWindow->geometry();
        if (anchor.isValid())
            rect.moveCenter(anchor.center());
    }

    QScreen *screen = nullptr;
    if (mainWindow)
        screen = mainWindow->screen();
    if (!screen)
        screen = dialog->screen();
    if (!screen)
        screen = QGuiApplication::primaryScreen();
    if (!screen)
        return rect;

    const QRect avail = screen->availableGeometry();
    if (!avail.isValid())
        return rect;

    int boundedX = rect.x();
    int boundedY = rect.y();
    const int minX = avail.left();
    const int minY = avail.top();
    const int maxX = avail.right() - rect.width() + 1;
    const int maxY = avail.bottom() - rect.height() + 1;
    if (maxX >= minX)
        boundedX = qBound(minX, boundedX, maxX);
    if (maxY >= minY)
        boundedY = qBound(minY, boundedY, maxY);
    rect.moveTopLeft(QPoint(boundedX, boundedY));
    return rect;
}

void MainWindow::runRemoteDownloadPrescan(
    const QVector<RemoteDownloadSeed> &seeds, int initialSkipped,
    bool dragAndDrop) {
    if (!rightIsRemote_ || !rightRemoteModel_ || !transferMgr_ ||
        !remoteOps_ || !remoteOps_->hasRequestedSession()) {
        UiAlerts::warning(this, tr("Remote"),
                          tr("No active remote session."));
        return;
    }
    if (remoteScanInProgress_.exchange(true)) {
        statusBar()->showMessage(tr("A remote scan is already in progress"),
                                 3000);
        return;
    }
    if (seeds.isEmpty()) {
        remoteScanInProgress_ = false;
        if (initialSkipped > 0) {
            statusBar()->showMessage(
                tr("Nothing queued. Skipped invalid: %1").arg(initialSkipped),
                4000);
        }
        return;
    }

    TransferBatchOptions batchOptions;
    batchOptions.sessionKey = transferMgr_->sessionIdentity();
    batchOptions.conflictPolicy = TransferConflictPolicy::Ask;
    batchOptions.batchId = transferMgr_->createBatch(batchOptions);

    const bool needsDirectoryScan =
        std::any_of(seeds.cbegin(), seeds.cend(),
                    [](const RemoteDownloadSeed &seed) { return seed.isDir; });
    if (!needsDirectoryScan) {
        remoteScanInProgress_ = false;
        QVector<QPair<QString, QString>> queuedPairs;
        queuedPairs.reserve(seeds.size());
        for (const auto &seed : seeds)
            queuedPairs.push_back({seed.remotePath, seed.localPath});
        const int enqueuedCount =
            transferMgr_->enqueueDownloads(queuedPairs, batchOptions);
        QString message =
            dragAndDrop ? tr("Queued: %1 downloads (DND)")
                        : tr("Queued: %1 downloads");
        message = message.arg(enqueuedCount);
        if (initialSkipped > 0) {
            message += QStringLiteral("  |  ") +
                       tr("Skipped invalid: %1").arg(initialSkipped);
        }
        statusBar()->showMessage(message, 6000);
        if (enqueuedCount > 0)
            maybeShowTransferQueue();
        return;
    }

    struct ScanState {
        QHash<RemoteOperationController::JobId, QString> localRoots;
        QSet<RemoteOperationController::JobId> pending;
        TransferBatchOptions batchOptions;
        std::shared_ptr<std::atomic<bool>> cancelRequested;
        bool dragAndDrop = false;
        bool canceled = false;
        bool thresholdPrompted = false;
        bool thresholdPromptActive = false;
        bool backpressurePaused = false;
        int enqueuedDownloads = 0;
        int enqueuedDirectories = 0;
        quint64 skippedInvalid = 0;
        quint64 discoveredItems = 0;
        quint64 knownBytes = 0;
        quint64 unknownSizes = 0;
        quint64 skippedSymlinks = 0;
        quint64 depthLimits = 0;
        quint64 listFailures = 0;
        QString lastError;
        QSet<quint64> pendingTransferTasks;
        QMetaObject::Connection batchConnection;
        QMetaObject::Connection progressConnection;
        QMetaObject::Connection completionConnection;
        QMetaObject::Connection tasksAddedConnection;
        QMetaObject::Connection tasksUpdatedConnection;
        QMetaObject::Connection tasksRemovedConnection;
    };
    auto state = std::make_shared<ScanState>();
    state->batchOptions = batchOptions;
    state->dragAndDrop = dragAndDrop;
    state->skippedInvalid = initialSkipped;
    state->cancelRequested = std::make_shared<std::atomic<bool>>(false);
    remoteScanCancelRequested_ = state->cancelRequested;

    auto *scanProgress = new QProgressDialog(
        tr("Preparing remote download queue..."), tr("Cancel"), 0, 0, this);
    scanProgress->setWindowTitle(tr("Preparing queue"));
    scanProgress->setWindowModality(Qt::NonModal);
    scanProgress->setAutoClose(false);
    scanProgress->setAutoReset(false);
    scanProgress->setMinimumDuration(0);
    remoteScanProgress_ = scanProgress;

    auto finish = std::make_shared<std::function<void()>>();
    *finish = [this, state] {
        if (!state->pending.isEmpty() ||
            state->thresholdPromptActive) {
            return;
        }
        QObject::disconnect(state->batchConnection);
        QObject::disconnect(state->progressConnection);
        QObject::disconnect(state->completionConnection);
        QObject::disconnect(state->tasksAddedConnection);
        QObject::disconnect(state->tasksUpdatedConnection);
        QObject::disconnect(state->tasksRemovedConnection);
        remoteScanInProgress_ = false;
        remoteScanCancelRequested_.reset();
        if (remoteScanProgress_) {
            remoteScanProgress_->hide();
            remoteScanProgress_->deleteLater();
            remoteScanProgress_.clear();
        }

        if (!rightIsRemote_ || !transferMgr_)
            return;
        if (state->canceled) {
            statusBar()->showMessage(tr("Remote scan canceled"), 4000);
            return;
        }

        QString message =
            state->dragAndDrop ? tr("Queued: %1 downloads (DND)")
                               : tr("Queued: %1 downloads");
        message = message.arg(state->enqueuedDownloads);
        if (state->enqueuedDirectories > 0) {
            message += QStringLiteral("  |  ") +
                       tr("Folders queued: %1")
                           .arg(state->enqueuedDirectories);
        }
        if (state->skippedInvalid > 0) {
            message += QStringLiteral("  |  ") +
                       tr("Skipped invalid: %1")
                           .arg(state->skippedInvalid);
        }
        if (state->listFailures > 0) {
            message += QStringLiteral("  |  ") +
                       tr("Folders not listed: %1")
                           .arg(state->listFailures);
        }
        if (state->unknownSizes > 0) {
            message += QStringLiteral("  |  ") +
                       tr("Unknown sizes: %1").arg(state->unknownSizes);
        }
        if (state->skippedSymlinks > 0) {
            message +=
                QStringLiteral("  |  ") +
                tr("Symbolic links skipped: %1")
                    .arg(state->skippedSymlinks);
        }
        if (state->depthLimits > 0) {
            message += QStringLiteral("  |  ") +
                       tr("Depth limits reached: %1")
                           .arg(state->depthLimits);
        }
        if (state->listFailures > 0 && !state->lastError.isEmpty()) {
            message += QStringLiteral("\n") + tr("Last error: ") +
                       state->lastError;
        }
        statusBar()->showMessage(message, 6000);
        if (state->enqueuedDownloads > 0 ||
            state->enqueuedDirectories > 0) {
            maybeShowTransferQueue();
        }
    };

    auto resumeAfterBackpressure =
        std::make_shared<std::function<void()>>();
    *resumeAfterBackpressure = [this, state] {
        if (!state->backpressurePaused ||
            state->cancelRequested->load() ||
            state->pendingTransferTasks.size() >= 1000) {
            return;
        }
        state->backpressurePaused = false;
        if (remoteOps_) {
            const auto scanJobs = state->pending;
            for (const auto jobId : scanJobs)
                remoteOps_->setPaused(jobId, false);
        }
        if (remoteScanProgress_) {
            remoteScanProgress_->setLabelText(
                tr("Scanning remote folders..."));
        }
    };
    state->tasksAddedConnection = connect(
        transferMgr_, &TransferManager::tasksAdded, this,
        [this, state](const QVector<quint64> &taskIds) {
            if (!transferMgr_)
                return;
            const auto tasks = transferMgr_->tasksSnapshot(taskIds);
            for (const auto &task : tasks) {
                if (task.batchId == state->batchOptions.batchId &&
                    !isTransferTaskFinalStatus(task.status)) {
                    state->pendingTransferTasks.insert(task.taskId);
                }
            }
        });
    state->tasksUpdatedConnection = connect(
        transferMgr_, &TransferManager::tasksUpdated, this,
        [this, state, resumeAfterBackpressure](
            const QVector<quint64> &taskIds) {
            if (!transferMgr_)
                return;
            QVector<quint64> relevant;
            relevant.reserve(taskIds.size());
            for (const quint64 taskId : taskIds) {
                if (state->pendingTransferTasks.contains(taskId))
                    relevant.push_back(taskId);
            }
            const auto tasks = transferMgr_->tasksSnapshot(relevant);
            QSet<quint64> observed;
            for (const auto &task : tasks) {
                observed.insert(task.taskId);
                if (isTransferTaskFinalStatus(task.status))
                    state->pendingTransferTasks.remove(task.taskId);
            }
            for (const quint64 taskId : relevant) {
                if (!observed.contains(taskId))
                    state->pendingTransferTasks.remove(taskId);
            }
            (*resumeAfterBackpressure)();
        });
    state->tasksRemovedConnection = connect(
        transferMgr_, &TransferManager::tasksRemoved, this,
        [state, resumeAfterBackpressure](
            const QVector<quint64> &taskIds) {
            for (const quint64 taskId : taskIds)
                state->pendingTransferTasks.remove(taskId);
            (*resumeAfterBackpressure)();
        });

    state->batchConnection = connect(
        remoteOps_, &RemoteOperationController::entriesBatchReady, this,
        [this, state, finish](
            const RemoteOperationController::EntryBatch &batch) {
            if (!state->pending.contains(batch.job.id) ||
                state->cancelRequested->load() || !transferMgr_) {
                return;
            }

            QVector<QPair<QString, QString>> files;
            QStringList directories;
            const QString localRoot = state->localRoots.value(batch.job.id);
            for (const auto &entry : batch.entries) {
                ++state->discoveredItems;
                if (!entry.info.is_dir) {
                    if (entry.info.has_size) {
                        const quint64 available =
                            std::numeric_limits<quint64>::max() -
                            state->knownBytes;
                        state->knownBytes +=
                            std::min(available, entry.info.size);
                    }
                }

                bool valid = !entry.relativePath.isEmpty();
                const QStringList parts =
                    entry.relativePath.split(QLatin1Char('/'),
                                             Qt::SkipEmptyParts);
                for (const QString &part : parts) {
                    QString why;
                    if (!isValidEntryName(part, &why)) {
                        valid = false;
                        break;
                    }
                }
                if (!valid) {
                    ++state->skippedInvalid;
                    continue;
                }

                const QString localPath =
                    QDir(localRoot).filePath(entry.relativePath);
                if (entry.info.is_dir)
                    directories.push_back(localPath);
                else
                    files.push_back({entry.path, localPath});
            }

            constexpr quint64 kLargeTreeItems = 100000;
            constexpr quint64 kLargeTreeBytes =
                quint64(100) * 1024 * 1024 * 1024;
            if (!state->thresholdPrompted &&
                (state->discoveredItems > kLargeTreeItems ||
                 state->knownBytes > kLargeTreeBytes)) {
                state->thresholdPrompted = true;
                state->thresholdPromptActive = true;
                const auto scanJobs = state->pending;
                for (const auto jobId : scanJobs)
                    remoteOps_->setPaused(jobId, true);
                const QString sizeText = QLocale().formattedDataSize(
                    static_cast<qint64>(
                        std::min<quint64>(
                            state->knownBytes,
                            static_cast<quint64>(
                                std::numeric_limits<qint64>::max()))));
                const bool keepGoing =
                    UiAlerts::question(
                        this, tr("Very large transfer"),
                        tr("The scan has found more than the safe review "
                           "threshold:\n\n%1 items\n%2 of known data\n\n"
                           "Continue preparing this batch?")
                            .arg(QLocale().toString(state->discoveredItems),
                                 sizeText),
                        QMessageBox::Yes | QMessageBox::No,
                        QMessageBox::No) == QMessageBox::Yes;
                state->thresholdPromptActive = false;
                if (!keepGoing) {
                    state->canceled = true;
                    state->cancelRequested->store(true);
                    const auto pending = state->pending;
                    for (const auto jobId : pending)
                        remoteOps_->cancel(jobId);
                    transferMgr_->cancelBatch(
                        state->batchOptions.batchId);
                    (*finish)();
                    return;
                }
                for (const auto jobId : scanJobs)
                    remoteOps_->setPaused(jobId, false);
            }

            for (const QString &directory : directories) {
                transferMgr_->enqueueLocalDirectory(
                    directory, state->batchOptions);
                ++state->enqueuedDirectories;
            }
            state->enqueuedDownloads +=
                transferMgr_->enqueueDownloads(files, state->batchOptions);

            if (state->pendingTransferTasks.size() < 2000 ||
                state->backpressurePaused) {
                (*finish)();
                return;
            }

            state->backpressurePaused = true;
            const auto scanJobs = state->pending;
            for (const auto jobId : scanJobs)
                remoteOps_->setPaused(jobId, true);
            if (remoteScanProgress_) {
                remoteScanProgress_->setLabelText(
                    tr("Queue backpressure: waiting for pending tasks to "
                       "drop below 1,000…"));
            }
            (*finish)();
        });
    state->progressConnection = connect(
        remoteOps_, &RemoteOperationController::jobProgress, this,
        [this, state](
            const RemoteOperationController::Progress &progress) {
            if (!state->pending.contains(progress.job.id) ||
                !remoteScanProgress_) {
                return;
            }
            remoteScanProgress_->setLabelText(
                tr("Scanning remote folders... %1 folders, %2 files found")
                    .arg(progress.visitedEntries)
                    .arg(state->enqueuedDownloads));
        });
    state->completionConnection = connect(
        remoteOps_, &RemoteOperationController::jobFinished, this,
        [this, state, finish](
            const RemoteOperationController::Completion &completion) {
            if (!state->pending.remove(completion.result.job.id))
                return;
            state->listFailures += completion.failedEntries;
            state->skippedInvalid += completion.invalidNames;
            state->unknownSizes += completion.unknownSizes;
            state->skippedSymlinks += completion.skippedSymlinks;
            state->depthLimits += completion.depthLimits;
            if (completion.result.outcome ==
                RemoteOperationController::Outcome::Canceled) {
                state->canceled = true;
            } else if (completion.result.outcome !=
                       RemoteOperationController::Outcome::Succeeded) {
                ++state->listFailures;
            } else {
                lastSuccessfulRemoteActivityAtMs_ =
                    QDateTime::currentMSecsSinceEpoch();
            }
            if (!completion.result.error.isEmpty())
                state->lastError = completion.result.error;
            (*finish)();
        });
    connect(scanProgress, &QProgressDialog::canceled, this,
            [this, state] {
                state->canceled = true;
                state->cancelRequested->store(true);
                if (remoteOps_) {
                    const auto pending = state->pending;
                    for (const auto jobId : pending)
                        remoteOps_->cancel(jobId);
                }
                if (transferMgr_)
                    transferMgr_->cancelBatch(state->batchOptions.batchId);
            });

    QVector<QPair<QString, QString>> directFiles;
    for (const auto &seed : seeds) {
        if (!seed.isDir) {
            directFiles.push_back({seed.remotePath, seed.localPath});
            continue;
        }
        transferMgr_->enqueueLocalDirectory(seed.localPath, batchOptions);
        ++state->enqueuedDirectories;
        RemoteOperationController::TraverseRequest request;
        request.rootPath = seed.remotePath;
        request.includeDirectories = true;
        request.traversal.includeHidden = true;
        request.traversal.skipSymlinks = true;
        request.traversal.maxDepth = 32;
        request.traversal.batchSize = 250;
        const auto jobId = remoteOps_->submit(request);
        if (jobId != 0) {
            state->localRoots.insert(jobId, seed.localPath);
            state->pending.insert(jobId);
        } else {
            ++state->listFailures;
        }
    }
    state->enqueuedDownloads +=
        transferMgr_->enqueueDownloads(directFiles, batchOptions);

    if (state->pending.isEmpty())
        (*finish)();
    else
        scanProgress->show();
}

void MainWindow::startLocalUploadDiscovery(
    const QVector<QPair<QString, QString>> &localRemoteRoots,
    bool moveSources, bool dragAndDrop) {
    if (!transferMgr_ || !sessionController_->client() || !rightIsRemote_) {
        UiAlerts::warning(this, tr("Remote"),
                          tr("No active remote session."));
        return;
    }
    if (localRemoteRoots.isEmpty()) {
        statusBar()->showMessage(tr("Nothing to upload."), 4000);
        return;
    }

    TransferBatchOptions batchOptions;
    batchOptions.sessionKey = transferMgr_->sessionIdentity();
    batchOptions.conflictPolicy = TransferConflictPolicy::Ask;
    batchOptions.operation =
        moveSources ? TransferOperation::Move : TransferOperation::Copy;
    batchOptions.batchId = transferMgr_->createBatch(batchOptions);

    auto *discovery = new LocalTreeDiscovery(this);
    activeLocalUploadDiscoveries_.insert(discovery);
    connect(discovery, &QObject::destroyed, this,
            [this, discovery] {
                activeLocalUploadDiscoveries_.remove(discovery);
            });
    struct UploadDiscoveryState {
        QPointer<LocalTreeDiscovery> discovery;
        QPointer<QProgressDialog> progress;
        QVector<QString> destinationRoots;
        TransferBatchOptions batchOptions;
        QHash<QString, quint64> directoryTasks;
        QSet<quint64> directoryTaskIds;
        QSet<quint64> pendingTasks;
        QSet<QString> sourceDirectorySet;
        QStringList sourceDirectories;
        LocalTreeDiscoveryCounters counters;
        int uploadCount = 0;
        int directoryCount = 0;
        bool moveSources = false;
        bool dragAndDrop = false;
        bool canceled = false;
        bool scanFinished = false;
        bool moveCleanupStarted = false;
        QMetaObject::Connection updatedConnection;
        QMetaObject::Connection removedConnection;
    };
    auto state = std::make_shared<UploadDiscoveryState>();
    state->discovery = discovery;
    state->batchOptions = batchOptions;
    state->moveSources = moveSources;
    state->dragAndDrop = dragAndDrop;
    state->destinationRoots.reserve(localRemoteRoots.size());

    LocalTreeDiscoveryOptions options;
    options.roots.reserve(localRemoteRoots.size());
    for (const auto &root : localRemoteRoots) {
        options.roots.push_back({root.first});
        state->destinationRoots.push_back(
            normalizeRemotePath(root.second));
    }

    auto *progress = new QProgressDialog(
        tr("Preparing local upload queue…"), tr("Cancel"), 0, 0, this);
    progress->setWindowTitle(moveSources ? tr("Preparing move")
                                         : tr("Preparing upload"));
    progress->setWindowModality(Qt::NonModal);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setMinimumDuration(0);
    progress->show();
    state->progress = progress;

    auto closeProgress = [state] {
        if (!state->progress)
            return;
        state->progress->hide();
        state->progress->deleteLater();
        state->progress.clear();
    };
    auto disconnectTaskTracking = [state] {
        QObject::disconnect(state->updatedConnection);
        QObject::disconnect(state->removedConnection);
    };
    auto countersSummary = [this](const LocalTreeDiscoveryCounters &counters) {
        QStringList notes;
        if (counters.skippedSymlinks > 0) {
            notes.push_back(
                tr("symlinks skipped: %1")
                    .arg(counters.skippedSymlinks));
        }
        if (counters.depthLimits > 0) {
            notes.push_back(
                tr("depth limits: %1").arg(counters.depthLimits));
        }
        if (counters.inaccessibleEntries > 0) {
            notes.push_back(
                tr("inaccessible: %1")
                    .arg(counters.inaccessibleEntries));
        }
        if (counters.invalidNames > 0) {
            notes.push_back(
                tr("invalid names: %1").arg(counters.invalidNames));
        }
        if (counters.unknownSizes > 0) {
            notes.push_back(
                tr("unknown sizes: %1").arg(counters.unknownSizes));
        }
        return notes.join(QStringLiteral(", "));
    };

    auto finishMoveCleanup =
        std::make_shared<std::function<void()>>();
    *finishMoveCleanup =
        [this, state, disconnectTaskTracking] {
            if (!state->moveSources || !state->scanFinished ||
                state->moveCleanupStarted || !state->pendingTasks.isEmpty() ||
                state->canceled) {
                return;
            }
            state->moveCleanupStarted = true;
            disconnectTaskTracking();

            QStringList directories = state->sourceDirectories;
            std::sort(
                directories.begin(), directories.end(),
                [](const QString &left, const QString &right) {
                    const int leftDepth =
                        QDir::fromNativeSeparators(left)
                            .count(QLatin1Char('/'));
                    const int rightDepth =
                        QDir::fromNativeSeparators(right)
                            .count(QLatin1Char('/'));
                    if (leftDepth != rightDepth)
                        return leftDepth > rightDepth;
                    return left > right;
                });
            if (directories.isEmpty()) {
                setLeftRoot(leftPath_->text());
                return;
            }
            QPointer<MainWindow> safeThis(this);
            QThreadPool::globalInstance()->start(
                [safeThis, directories = std::move(directories)] {
                    for (const QString &directory : directories)
                        (void)QDir().rmdir(directory);
                    if (!safeThis)
                        return;
                    QMetaObject::invokeMethod(
                        safeThis,
                        [safeThis] {
                            if (!safeThis)
                                return;
                            safeThis->setLeftRoot(
                                safeThis->leftPath_->text());
                        },
                        Qt::QueuedConnection);
                });
        };

    auto reconcileTasks =
        [this, state, finishMoveCleanup](
            const QVector<quint64> &taskIds, bool removed) {
            bool changed = false;
            bool directoryFailed = false;
            for (const quint64 taskId : taskIds) {
                if (!state->pendingTasks.contains(taskId))
                    continue;
                const auto task =
                    removed ? std::optional<TransferTask>{}
                            : transferMgr_->taskSnapshot(taskId);
                if (state->directoryTaskIds.contains(taskId)) {
                    if (!task || removed ||
                        task->status == TransferTask::Status::Error ||
                        task->status == TransferTask::Status::Canceled ||
                        task->status == TransferTask::Status::Skipped ||
                        task->status == TransferTask::Status::Warning) {
                        directoryFailed = true;
                    }
                }
                if (removed || !task ||
                    isLocalUploadPreparationTerminal(task->status)) {
                    state->pendingTasks.remove(taskId);
                    changed = true;
                }
            }
            if (changed && state->discovery) {
                state->discovery->setPendingTaskCount(
                    state->pendingTasks.size());
            }
            if (directoryFailed && !state->canceled) {
                state->canceled = true;
                transferMgr_->cancelBatch(state->batchOptions.batchId);
                if (state->discovery)
                    state->discovery->cancel();
            }
            (*finishMoveCleanup)();
        };
    state->updatedConnection =
        connect(transferMgr_, &TransferManager::tasksUpdated, this,
                [reconcileTasks](const QVector<quint64> &taskIds) {
                    reconcileTasks(taskIds, false);
                });
    state->removedConnection =
        connect(transferMgr_, &TransferManager::tasksRemoved, this,
                [reconcileTasks](const QVector<quint64> &taskIds) {
                    reconcileTasks(taskIds, true);
                });

    connect(discovery, &LocalTreeDiscovery::batchReady, this,
            [this, state, reconcileTasks](
                const LocalTreeDiscoveryBatch &batch) {
                if (state->canceled || !transferMgr_ ||
                    !rightIsRemote_) {
                    if (state->discovery)
                        state->discovery->cancel();
                    return;
                }
                state->counters = batch.counters;
                QVector<quint64> newTaskIds;
                newTaskIds.reserve(batch.entries.size());
                for (const auto &entry : batch.entries) {
                    if (entry.rootIndex < 0 ||
                        entry.rootIndex >=
                            state->destinationRoots.size()) {
                        continue;
                    }
                    QString destination =
                        state->destinationRoots[entry.rootIndex];
                    if (!entry.relativePath.isEmpty()) {
                        destination =
                            joinRemotePath(destination,
                                           entry.relativePath);
                    }

                    TransferBatchOptions entryOptions =
                        state->batchOptions;
                    QString parentRelative = entry.relativePath;
                    const int separator =
                        parentRelative.lastIndexOf(QLatin1Char('/'));
                    if (entry.type ==
                        LocalTreeDiscoveryEntry::Type::Directory) {
                        if (entry.relativePath.isEmpty()) {
                            parentRelative.clear();
                        } else if (separator >= 0) {
                            parentRelative =
                                parentRelative.left(separator);
                        } else {
                            parentRelative.clear();
                        }
                    } else if (separator >= 0) {
                        parentRelative =
                            parentRelative.left(separator);
                    } else {
                        parentRelative.clear();
                    }

                    const QString parentKey =
                        QString::number(entry.rootIndex) +
                        QStringLiteral(":") + parentRelative;
                    if (entry.type ==
                            LocalTreeDiscoveryEntry::Type::File &&
                        entry.relativePath.isEmpty()) {
                        entryOptions.dependsOnTaskId = 0;
                    } else {
                        entryOptions.dependsOnTaskId =
                            state->directoryTasks.value(parentKey, 0);
                    }

                    quint64 taskId = 0;
                    if (entry.type ==
                        LocalTreeDiscoveryEntry::Type::Directory) {
                        taskId = transferMgr_->enqueueRemoteDirectory(
                            destination, entryOptions);
                        const QString directoryKey =
                            QString::number(entry.rootIndex) +
                            QStringLiteral(":") + entry.relativePath;
                        state->directoryTasks.insert(directoryKey,
                                                     taskId);
                        if (taskId != 0)
                            state->directoryTaskIds.insert(taskId);
                        ++state->directoryCount;
                        if (!state->sourceDirectorySet.contains(
                                entry.localPath)) {
                            state->sourceDirectorySet.insert(
                                entry.localPath);
                            state->sourceDirectories.push_back(
                                entry.localPath);
                        }
                    } else {
                        taskId = transferMgr_->enqueueUpload(
                            entry.localPath, destination, entryOptions);
                        ++state->uploadCount;
                    }
                    if (taskId != 0) {
                        state->pendingTasks.insert(taskId);
                        newTaskIds.push_back(taskId);
                    }
                }
                // A fast directory task can finish between enqueue() and the
                // insertion above. Reconcile immediately so discovery
                // backpressure and move cleanup cannot remain stuck on a
                // terminal (or already-pruned) task.
                reconcileTasks(newTaskIds, false);
                if (state->discovery) {
                    state->discovery->setPendingTaskCount(
                        state->pendingTasks.size());
                }
            });

    connect(
        discovery, &LocalTreeDiscovery::progressChanged, this,
        [state](const LocalTreeDiscoveryCounters &counters,
                const QString &currentPath) {
            state->counters = counters;
            if (!state->progress)
                return;
            state->progress->setLabelText(
                QCoreApplication::translate(
                    "MainWindow",
                    "Scanning local items: %1\n%2")
                    .arg(counters.itemCount)
                    .arg(QDir::toNativeSeparators(currentPath)));
        });

    connect(
        discovery,
        &LocalTreeDiscovery::largeTreeConfirmationRequired, this,
        [this, state](const LocalTreeDiscoveryCounters &counters) {
            if (state->canceled || !state->discovery)
                return;
            const QString knownSize =
                QLocale().formattedDataSize(qint64(std::min(
                    counters.knownBytes,
                    quint64(std::numeric_limits<qint64>::max()))));
            const auto answer = UiAlerts::question(
                this, tr("Large upload"),
                tr("This upload contains more than 100,000 items or 100 GiB "
                   "of known data.\n\nDiscovered so far: %1 items, %2.\n"
                   "Continue preparing it?")
                    .arg(counters.itemCount)
                    .arg(knownSize));
            if (answer == QMessageBox::Yes) {
                state->discovery
                    ->continueAfterLargeTreeConfirmation();
            } else {
                state->canceled = true;
                transferMgr_->cancelBatch(
                    state->batchOptions.batchId);
                state->discovery->cancel();
            }
        });

    connect(progress, &QProgressDialog::canceled, this,
            [this, state] {
                if (state->canceled)
                    return;
                state->canceled = true;
                transferMgr_->cancelBatch(state->batchOptions.batchId);
                if (state->discovery)
                    state->discovery->cancel();
            });

    connect(
        discovery, &LocalTreeDiscovery::finished, this,
        [this, state, closeProgress, disconnectTaskTracking,
         countersSummary, finishMoveCleanup](
            const LocalTreeDiscoveryCounters &counters) {
            state->scanFinished = true;
            state->counters = counters;
            closeProgress();
            if (state->discovery)
                state->discovery->deleteLater();

            QString message =
                state->dragAndDrop
                    ? tr("Queued: %1 uploads, %2 folders (DND)")
                          .arg(state->uploadCount)
                          .arg(state->directoryCount)
                    : (state->moveSources
                           ? tr("Queued: %1 uploads, %2 folders (move)")
                                 .arg(state->uploadCount)
                                 .arg(state->directoryCount)
                           : tr("Queued: %1 uploads, %2 folders")
                                 .arg(state->uploadCount)
                                 .arg(state->directoryCount));
            const QString notes = countersSummary(counters);
            if (!notes.isEmpty())
                message += QStringLiteral("  |  ") + notes;
            statusBar()->showMessage(message, 7000);
            if (state->uploadCount > 0 ||
                state->directoryCount > 0) {
                maybeShowTransferQueue();
            }
            if (!state->moveSources) {
                disconnectTaskTracking();
            } else {
                (*finishMoveCleanup)();
            }
        });

    auto cancelFinished =
        [this, state, closeProgress, disconnectTaskTracking,
         countersSummary](const LocalTreeDiscoveryCounters &counters,
                          const QString &failure) {
            state->canceled = true;
            transferMgr_->cancelBatch(state->batchOptions.batchId);
            closeProgress();
            disconnectTaskTracking();
            if (state->discovery)
                state->discovery->deleteLater();
            QString message =
                failure.isEmpty()
                    ? tr("Upload preparation canceled.")
                    : tr("Upload preparation failed: %1").arg(failure);
            const QString notes = countersSummary(counters);
            if (!notes.isEmpty())
                message += QStringLiteral("  |  ") + notes;
            statusBar()->showMessage(message, 7000);
        };
    connect(discovery, &LocalTreeDiscovery::canceled, this,
            [cancelFinished](
                const LocalTreeDiscoveryCounters &counters) {
                cancelFinished(counters, {});
            });
    connect(discovery, &LocalTreeDiscovery::failed, this,
            [cancelFinished](
                const QString &message,
                const LocalTreeDiscoveryCounters &counters) {
                cancelFinished(counters, message);
            });

    discovery->start(options);
}

void MainWindow::cancelLocalUploadDiscoveries() {
    const auto active = activeLocalUploadDiscoveries_.values();
    for (LocalTreeDiscovery *discovery : active) {
        if (discovery)
            discovery->cancel();
    }
}

void MainWindow::showTransferQueue() {
    if (!transferDlg_)
        transferDlg_ = new TransferQueueDialog(transferMgr_, this);
    const bool wasVisible = transferDlg_->isVisible();
    if (!wasVisible) {
        const QRect endRect = centeredQueueRect(transferDlg_, this);
        if (endRect.isValid())
            transferDlg_->setGeometry(endRect);
        // Modeless queue: smooth entrance (fade + slight scale/offset) for
        // better perceived polish.
        transferDlg_->show();
        transferDlg_->raise();
        transferDlg_->activateWindow();

        QRect startRect = endRect;
        startRect.setWidth(qMax(220, (endRect.width() * 96) / 100));
        startRect.setHeight(qMax(140, (endRect.height() * 96) / 100));
        startRect.moveCenter(endRect.center() + QPoint(0, 10));

        transferDlg_->setGeometry(startRect);
        transferDlg_->setWindowOpacity(0.0);

        auto *group = new QParallelAnimationGroup(transferDlg_);

        auto *fade =
            new QPropertyAnimation(transferDlg_, "windowOpacity", group);
        fade->setDuration(190);
        fade->setStartValue(0.0);
        fade->setEndValue(1.0);
        fade->setEasingCurve(QEasingCurve::OutCubic);

        auto *grow = new QPropertyAnimation(transferDlg_, "geometry", group);
        grow->setDuration(190);
        grow->setStartValue(startRect);
        grow->setEndValue(endRect);
        grow->setEasingCurve(QEasingCurve::OutCubic);

        connect(group, &QParallelAnimationGroup::finished, transferDlg_,
                [this, endRect] {
                    if (transferDlg_)
                        transferDlg_->setWindowOpacity(1.0);
                    if (transferDlg_)
                        transferDlg_->setGeometry(endRect);
                });
        group->start(QAbstractAnimation::DeleteWhenStopped);
        return;
    }

    transferDlg_->show();
    transferDlg_->raise();
    transferDlg_->activateWindow();
}

void MainWindow::maybeShowTransferQueue() {
    if (prefShowQueueOnEnqueue_)
        showTransferQueue();
}

bool MainWindow::eventFilter(QObject *eventSource, QEvent *event) {
    QObject *const rightViewport = rightView_ ? rightView_->viewport() : nullptr;
    QObject *const leftViewport = leftView_ ? leftView_->viewport() : nullptr;
    if (eventSource != rightViewport && eventSource != leftViewport)
        return QMainWindow::eventFilter(eventSource, event);

    auto extractStagingBatchDir = [](const QMimeData *md) -> QString {
        if (!md || !md->hasFormat(kStagingBatchMime))
            return QString();
        const QByteArray raw = md->data(kStagingBatchMime);
        return QString::fromUtf8(raw).trimmed();
    };

    auto scheduleStagingCleanupAfterLocalJobs = [this](const QString &batchDir) {
        if (batchDir.isEmpty())
            return;
        auto *timer = new QTimer(this);
        timer->setInterval(350);
        timer->setSingleShot(false);
        auto attempts = std::make_shared<int>(0);
        connect(timer, &QTimer::timeout, this, [this, timer, attempts, batchDir] {
            ++(*attempts);
            const bool giveUp = (*attempts >= 300); // ~105s max wait
            if (!giveUp && localFsJobsInFlight_.load() > 0)
                return;

            timer->stop();
            timer->deleteLater();
            QDir batch(batchDir);
            if (batch.exists())
                (void)batch.removeRecursively();

            // If the staging root is now empty, remove it as well.
            const QString rootPath = QFileInfo(batchDir).absolutePath();
            QDir root(rootPath);
            if (root.exists() &&
                root.entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty()) {
                (void)root.rmdir(".");
            }
        });
        timer->start();
    };

    // Drag-and-drop over the right panel (local or remote)
    if (eventSource == rightViewport) {
        if (event->type() == QEvent::DragEnter) {
            auto *dragEnterEvent = static_cast<QDragEnterEvent *>(event);
            if (rightIsRemote_ && !rightRemoteMutationsSupported_) {
                dragEnterEvent->ignore();
                return true;
            }
            dragEnterEvent->acceptProposedAction();
            return true;
        } else if (event->type() == QEvent::DragMove) {
            auto *dragMoveEvent = static_cast<QDragMoveEvent *>(event);
            if (rightIsRemote_ && !rightRemoteMutationsSupported_) {
                dragMoveEvent->ignore();
                return true;
            }
            dragMoveEvent->acceptProposedAction();
            return true;
        } else if (event->type() == QEvent::Drop) {
            auto *dropEvent = static_cast<QDropEvent *>(event);
            const QString stagingBatchDir =
                extractStagingBatchDir(dropEvent->mimeData());
            const auto urls =
                dropEvent->mimeData() ? dropEvent->mimeData()->urls()
                                      : QList<QUrl>{};
            if (urls.isEmpty()) {
                dropEvent->acceptProposedAction();
                return true;
            }
            if (rightIsRemote_) {
                if (!stagingBatchDir.isEmpty()) {
                    statusBar()->showMessage(
                        tr("Drop ignored: remote-origin drag cannot be dropped "
                           "back into the same remote panel"),
                        5000);
                    dropEvent->ignore();
                    return true;
                }
                // Block upload if remote is read-only
                if (!rightRemoteMutationsSupported_) {
                    statusBar()->showMessage(
                        tr("Remote directory is read-only; cannot upload here"),
                        5000);
                    dropEvent->ignore();
                    return true;
                }
                // Upload to remote
                if (!sessionController_->client() || !rightRemoteModel_) {
                    dropEvent->acceptProposedAction();
                    return true;
                }
                const QString remoteBase = rightRemoteModel_->rootPath();
                QVector<QPair<QString, QString>> roots;
                roots.reserve(urls.size());
                for (const QUrl &url : urls) {
                    const QString localPath = url.toLocalFile();
                    if (localPath.isEmpty())
                        continue;
                    const QFileInfo localFileInfo(localPath);
                    if (!localFileInfo.isDir() &&
                        !localFileInfo.isFile()) {
                        continue;
                    }
                    roots.push_back(
                        {localFileInfo.absoluteFilePath(),
                         joinRemotePath(remoteBase,
                                        localFileInfo.fileName())});
                }
                if (!roots.isEmpty())
                    startLocalUploadDiscovery(roots, false, true);
                dropEvent->acceptProposedAction();
                return true;
            } else {
                // Local copy to the right panel directory
                QDir dst(rightPath_->text());
                if (!dst.exists()) {
                    dropEvent->acceptProposedAction();
                    return true;
                }
                QVector<LocalFsPair> pairs;
                for (const QUrl &url : urls) {
                    const QString localPath = url.toLocalFile();
                    if (localPath.isEmpty())
                        continue;
                    QFileInfo localFileInfo(localPath);
                    const QString target =
                        dst.filePath(localFileInfo.fileName());
                    // Avoid copying onto itself if same directory/file
                    if (localFileInfo.absoluteFilePath() == target) {
                        continue;
                    }
                    pairs.push_back({localFileInfo.absoluteFilePath(), target});
                }
                runLocalFsOperation(pairs, false, 0);
                dropEvent->acceptProposedAction();
                return true;
            }
        }
    }
    // Drag-and-drop over the left panel (local): copy/download
    // Update delete shortcut enablement if selection changes due to DnD or
    // click
    if (eventSource == leftViewport) {
        if (event->type() == QEvent::DragEnter) {
            auto *dragEnterEvent = static_cast<QDragEnterEvent *>(event);
            dragEnterEvent->acceptProposedAction();
            return true;
        } else if (event->type() == QEvent::DragMove) {
            auto *dragMoveEvent = static_cast<QDragMoveEvent *>(event);
            dragMoveEvent->acceptProposedAction();
            return true;
        } else if (event->type() == QEvent::Drop) {
            auto *dropEvent = static_cast<QDropEvent *>(event);
            const QString stagingBatchDir =
                extractStagingBatchDir(dropEvent->mimeData());
            const auto urls =
                dropEvent->mimeData() ? dropEvent->mimeData()->urls()
                                      : QList<QUrl>{};
            if (!urls.isEmpty()) {
                // Local copy towards the left panel
                QDir dst(leftPath_->text());
                if (!dst.exists()) {
                    dropEvent->acceptProposedAction();
                    return true;
                }
                QVector<LocalFsPair> pairs;
                for (const QUrl &url : urls) {
                    const QString localPath = url.toLocalFile();
                    if (localPath.isEmpty())
                        continue;
                    QFileInfo localFileInfo(localPath);
                    const QString target =
                        dst.filePath(localFileInfo.fileName());
                    // Avoid self-drop: same file/folder and same destination
                    if (localFileInfo.absoluteFilePath() == target) {
                        continue;
                    }
                    pairs.push_back({localFileInfo.absoluteFilePath(), target});
                }
                runLocalFsOperation(pairs, false, 0);
                if (!stagingBatchDir.isEmpty())
                    scheduleStagingCleanupAfterLocalJobs(stagingBatchDir);
                dropEvent->acceptProposedAction();
                updateDeleteShortcutEnables();
                return true;
            }
            // Download from remote (based on right panel selection)
            if (rightIsRemote_ == true && rightView_ && rightRemoteModel_) {
                auto selectionModel = rightView_->selectionModel();
                if (!selectionModel ||
                    selectionModel->selectedRows(NAME_COL).isEmpty()) {
                    dropEvent->acceptProposedAction();
                    return true;
                }
                const auto rows = selectionModel->selectedRows(NAME_COL);
                int bad = 0;
                QVector<RemoteDownloadSeed> seeds;
                seeds.reserve(rows.size());
                const QString remoteBase = rightRemoteModel_->rootPath();
                QDir dst(leftPath_->text());
                for (const QModelIndex &idx : rows) {
                    const QString name = rightRemoteModel_->nameAt(idx);
                    {
                        QString why;
                        if (!isValidEntryName(name, &why)) {
                            ++bad;
                            continue;
                        }
                    }
                    const QString rpath = joinRemotePath(remoteBase, name);
                    const QString lpath = dst.filePath(name);
                    seeds.push_back(
                        {rpath, lpath, rightRemoteModel_->isDir(idx)});
                }
                runRemoteDownloadPrescan(seeds, bad, true);
                dropEvent->acceptProposedAction();
                updateDeleteShortcutEnables();
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(eventSource, event);
}
