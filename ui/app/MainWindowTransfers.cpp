// MainWindow transfer queue UI, remote prescan, and drag-and-drop handling.
#include "app/MainWindow.hpp"
#include "logic/common/AppSettings.hpp"
#include "logic/common/MainWindowSharedUtils.hpp"
#include "logic/common/StagingStore.hpp"
#include "logic/common/UiAlerts.hpp"
#include "logic/connections/SessionController.hpp"
#include "logic/remote/LocalTreeDiscovery.hpp"
#include "logic/remote/RemoteModel.hpp"
#include "logic/remote/RemoteOperationController.hpp"
#include "widgets/dialogs/TransferQueueDialog.hpp"
#include "widgets/files/DragAwareTreeView.hpp"
#include "widgets/navigation/PathNavigationBar.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QFileInfo>
#include <QGuiApplication>
#include <QLocale>
#include <QMimeData>
#include <QPointer>
#include <QProgressDialog>
#include <QScreen>
#include <QStatusBar>
#include <QStringList>
#include <QThreadPool>
#include <QTimer>

#include <algorithm>
#include <atomic>
#include <functional>
#include <limits>
#include <memory>

namespace {

// What is dropped on a panel is copied, uploaded or downloaded afterwards,
// never moved, so the drop reports a copy. Accepting a proposed move would
// let the source, such as Explorer or Finder with Shift held, delete the
// originals while OpenSCP still reads them. A drag that offers no copy is
// refused.
void acceptDropAsCopy(QDropEvent *event) {
    if (!event->possibleActions().testFlag(Qt::CopyAction)) {
        event->ignore();
        return;
    }
    event->setDropAction(Qt::CopyAction);
    event->accept();
}

constexpr char kStagingBatchMime[] = "application/x-openscp-staging-batch";

// Whether a drag carries at least one local file or folder, the only thing a
// panel can copy, upload or download into: text and web links have no path.
bool carriesLocalFiles(const QMimeData *mimeData) {
    if (!mimeData || !mimeData->hasUrls())
        return false;
    const QList<QUrl> urls = mimeData->urls();
    return std::any_of(urls.cbegin(), urls.cend(),
                       [](const QUrl &url) { return url.isLocalFile(); });
}

// Whether a drag brings files that exist only inside the application they
// come from, as Outlook attachments do on Windows: they have no path either.
bool carriesVirtualFiles(const QMimeData *mimeData) {
    return mimeData &&
           (mimeData->hasFormat(QStringLiteral(
                "application/"
                "x-qt-windows-mime;value=\"FileGroupDescriptorW\"")) ||
            mimeData->hasFormat(QStringLiteral(
                "application/"
                "x-qt-windows-mime;value=\"FileGroupDescriptor\"")));
}

QRect centeredQueueRect(QWidget *dialog, QWidget *mainWindow) {
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

} // namespace

void MainWindow::runRemoteDownloadPrescan(
    const QVector<RemoteDownloadSeed> &seeds, int initialSkipped,
    bool moveSources) {
    if (!rightIsRemote_ || !rightRemoteModel_ || !transferMgr_ || !remoteOps_ ||
        !remoteOps_->hasRequestedSession()) {
        UiAlerts::warning(this, tr("Remote"), tr("No active remote session."));
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
    batchOptions.operation =
        moveSources ? TransferOperation::Move : TransferOperation::Copy;
    batchOptions.conflictPolicy = TransferConflictPolicy::Ask;
    batchOptions.batchId = transferMgr_->createBatch(batchOptions);

    const bool needsDirectoryScan =
        std::any_of(seeds.cbegin(), seeds.cend(),
                    [](const RemoteDownloadSeed &seed) { return seed.isDir; });
    if (!needsDirectoryScan && !moveSources) {
        remoteScanInProgress_ = false;
        QVector<QPair<QString, QString>> queuedPairs;
        queuedPairs.reserve(seeds.size());
        for (const auto &seed : seeds)
            queuedPairs.push_back({seed.remotePath, seed.localPath});
        const int enqueuedCount =
            transferMgr_->enqueueDownloads(queuedPairs, batchOptions);
        QString message = tr("Queued: %1 downloads").arg(enqueuedCount);
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
        QString remoteBase;
        QStringList topRemoteDirectories;
        QSet<RemoteOperationController::JobId> cleanupPending;
        bool moveSources = false;
        bool canceled = false;
        bool scanReported = false;
        bool cleanupStarted = false;
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
        QMetaObject::Connection cleanupConnection;
    };
    auto state = std::make_shared<ScanState>();
    state->batchOptions = batchOptions;
    state->moveSources = moveSources;
    state->remoteBase = rightRemoteModel_->rootPath();
    state->skippedInvalid = static_cast<quint64>(std::max(0, initialSkipped));
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
        if (!state->pending.isEmpty() || state->thresholdPromptActive)
            return;

        if (!state->scanReported) {
            state->scanReported = true;
            QObject::disconnect(state->batchConnection);
            QObject::disconnect(state->progressConnection);
            QObject::disconnect(state->completionConnection);
            remoteScanInProgress_ = false;
            remoteScanCancelRequested_.reset();
            if (remoteScanProgress_) {
                remoteScanProgress_->hide();
                remoteScanProgress_->deleteLater();
                remoteScanProgress_.clear();
            }

            if (rightIsRemote_ && transferMgr_) {
                if (state->canceled && !state->moveSources) {
                    statusBar()->showMessage(tr("Remote scan canceled"), 4000);
                } else {
                    QString message = state->moveSources
                                          ? tr("Queued: %1 downloads (move)")
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
                        message +=
                            QStringLiteral("  |  ") +
                            tr("Unknown sizes: %1").arg(state->unknownSizes);
                    }
                    if (state->skippedSymlinks > 0) {
                        message += QStringLiteral("  |  ") +
                                   tr("Symbolic links skipped: %1")
                                       .arg(state->skippedSymlinks);
                    }
                    if (state->depthLimits > 0) {
                        message += QStringLiteral("  |  ") +
                                   tr("Depth limits reached: %1")
                                       .arg(state->depthLimits);
                    }
                    if (state->canceled) {
                        message += QStringLiteral("  |  ") + tr("Canceled");
                    }
                    if (state->listFailures > 0 &&
                        !state->lastError.isEmpty()) {
                        message += QStringLiteral("\n") + tr("Last error: ") +
                                   state->lastError;
                    }
                    statusBar()->showMessage(message, 6000);
                    if (state->enqueuedDownloads > 0 ||
                        state->enqueuedDirectories > 0) {
                        maybeShowTransferQueue();
                    }
                }
            }
        }

        if (!state->moveSources || state->canceled || !transferMgr_ ||
            !remoteOps_) {
            QObject::disconnect(state->tasksAddedConnection);
            QObject::disconnect(state->tasksUpdatedConnection);
            QObject::disconnect(state->tasksRemovedConnection);
            return;
        }
        if (!state->pendingTransferTasks.isEmpty() || state->cleanupStarted)
            return;

        state->cleanupStarted = true;
        QObject::disconnect(state->tasksAddedConnection);
        QObject::disconnect(state->tasksUpdatedConnection);
        QObject::disconnect(state->tasksRemovedConnection);
        if (state->topRemoteDirectories.isEmpty()) {
            if (rightIsRemote_ && rightRemoteModel_)
                requestRemoteListing(state->remoteBase, true);
            return;
        }

        state->cleanupConnection = connect(
            remoteOps_, &RemoteOperationController::mutationCompleted, this,
            [this,
             state](const RemoteOperationController::MutationResult &result) {
                if (!state->cleanupPending.remove(result.result.job.id) ||
                    !state->cleanupPending.isEmpty()) {
                    return;
                }
                QObject::disconnect(state->cleanupConnection);
                if (rightIsRemote_ && rightRemoteModel_)
                    requestRemoteListing(state->remoteBase, true);
            });
        for (const QString &remoteDirectory : state->topRemoteDirectories) {
            RemoteOperationController::DeleteRequest request;
            request.path = remoteDirectory;
            request.kind = RemoteOperationController::DeleteKind::Directory;
            request.recursive = true;
            request.traversal.includeHidden = true;
            request.traversal.skipSymlinks = true;
            request.traversal.maxDepth = 32;
            request.emptyDirectoriesOnly = true;
            const auto jobId = remoteOps_->submit(request);
            if (jobId != 0)
                state->cleanupPending.insert(jobId);
        }
        if (state->cleanupPending.isEmpty()) {
            QObject::disconnect(state->cleanupConnection);
            if (rightIsRemote_ && rightRemoteModel_)
                requestRemoteListing(state->remoteBase, true);
        }
    };

    auto resumeAfterBackpressure = std::make_shared<std::function<void()>>();
    *resumeAfterBackpressure = [this, state] {
        if (!state->backpressurePaused || state->cancelRequested->load() ||
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
            remoteScanProgress_->setLabelText(tr("Scanning remote folders..."));
        }
    };
    state->tasksAddedConnection =
        connect(transferMgr_, &TransferManager::tasksAdded, this,
                [this, state](const QVector<quint64> &taskIds) {
                    if (!transferMgr_)
                        return;
                    const auto tasks = transferMgr_->tasksSnapshot(taskIds);
                    for (const auto &task : tasks) {
                        if (task.batchId == state->batchOptions.batchId &&
                            !isTerminalTransferStatus(task.status)) {
                            state->pendingTransferTasks.insert(task.taskId);
                        }
                    }
                });
    state->tasksUpdatedConnection =
        connect(transferMgr_, &TransferManager::tasksUpdated, this,
                [this, state, finish,
                 resumeAfterBackpressure](const QVector<quint64> &taskIds) {
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
                        if (isTerminalTransferStatus(task.status))
                            state->pendingTransferTasks.remove(task.taskId);
                    }
                    for (const quint64 taskId : relevant) {
                        if (!observed.contains(taskId))
                            state->pendingTransferTasks.remove(taskId);
                    }
                    (*resumeAfterBackpressure)();
                    (*finish)();
                });
    state->tasksRemovedConnection =
        connect(transferMgr_, &TransferManager::tasksRemoved, this,
                [state, finish,
                 resumeAfterBackpressure](const QVector<quint64> &taskIds) {
                    for (const quint64 taskId : taskIds)
                        state->pendingTransferTasks.remove(taskId);
                    (*resumeAfterBackpressure)();
                    (*finish)();
                });

    state->batchConnection = connect(
        remoteOps_, &RemoteOperationController::entriesBatchReady, this,
        [this, state,
         finish](const RemoteOperationController::EntryBatch &batch) {
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
                        state->knownBytes += std::min(
                            available, static_cast<quint64>(entry.info.size));
                    }
                }

                bool valid = !entry.relativePath.isEmpty();
                const QStringList parts = entry.relativePath.split(
                    QLatin1Char('/'), Qt::SkipEmptyParts);
                for (const QString &part : parts) {
                    QString why;
                    if (!isValidLocalEntryName(part, &why)) {
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
            if (!state->moveSources && !state->thresholdPrompted &&
                (state->discoveredItems > kLargeTreeItems ||
                 state->knownBytes > kLargeTreeBytes)) {
                state->thresholdPrompted = true;
                state->thresholdPromptActive = true;
                const auto scanJobs = state->pending;
                for (const auto jobId : scanJobs)
                    remoteOps_->setPaused(jobId, true);
                const QString sizeText = QLocale().formattedDataSize(
                    static_cast<qint64>(std::min<quint64>(
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
                    transferMgr_->cancelBatch(state->batchOptions.batchId);
                    (*finish)();
                    return;
                }
                for (const auto jobId : scanJobs)
                    remoteOps_->setPaused(jobId, false);
            }

            QVector<TransferRequest> requests;
            requests.reserve(directories.size());
            for (const QString &directory : directories) {
                requests.push_back({TransferTask::Type::CreateLocalDirectory,
                                    {},
                                    directory,
                                    state->batchOptions,
                                    -1});
            }
            state->enqueuedDirectories +=
                static_cast<int>(transferMgr_->enqueueTasks(requests).size());
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
        [this, state](const RemoteOperationController::Progress &progress) {
            if (!state->pending.contains(progress.job.id) ||
                !remoteScanProgress_) {
                return;
            }
            remoteScanProgress_->setLabelText(
                tr("Scanning remote folders... %1 folders, %2 files found")
                    .arg(progress.visitedEntries)
                    .arg(state->enqueuedDownloads));
        });
    state->completionConnection =
        connect(remoteOps_, &RemoteOperationController::jobFinished, this,
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
                        sessionHealthMonitor_.recordActivity();
                    }
                    if (!completion.result.error.isEmpty())
                        state->lastError = completion.result.error;
                    (*finish)();
                });
    connect(scanProgress, &QProgressDialog::canceled, this, [this, state] {
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
        if (moveSources)
            state->topRemoteDirectories.push_back(seed.remotePath);
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
    const QVector<QPair<QString, QString>> &localRemoteRoots, bool moveSources,
    bool dragAndDrop) {
    if (!transferMgr_ || !sessionController_->hasSession() || !rightIsRemote_) {
        UiAlerts::warning(this, tr("Remote"), tr("No active remote session."));
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
    connect(discovery, &QObject::destroyed, this, [this, discovery] {
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
        state->destinationRoots.push_back(normalizeRemotePath(root.second));
    }

    auto *progress = new QProgressDialog(tr("Preparing local upload queue…"),
                                         tr("Cancel"), 0, 0, this);
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
    auto countersSummary = [](const LocalTreeDiscoveryCounters &counters) {
        QStringList notes;
        if (counters.skippedSymlinks > 0) {
            notes.push_back(
                tr("symlinks skipped: %1").arg(counters.skippedSymlinks));
        }
        if (counters.depthLimits > 0) {
            notes.push_back(tr("depth limits: %1").arg(counters.depthLimits));
        }
        if (counters.inaccessibleEntries > 0) {
            notes.push_back(
                tr("inaccessible: %1").arg(counters.inaccessibleEntries));
        }
        if (counters.invalidNames > 0) {
            notes.push_back(tr("invalid names: %1").arg(counters.invalidNames));
        }
        if (counters.unknownSizes > 0) {
            notes.push_back(tr("unknown sizes: %1").arg(counters.unknownSizes));
        }
        return notes.join(QStringLiteral(", "));
    };

    auto finishMoveCleanup = std::make_shared<std::function<void()>>();
    *finishMoveCleanup = [this, state, disconnectTaskTracking] {
        if (!state->moveSources || !state->scanFinished ||
            state->moveCleanupStarted || !state->pendingTasks.isEmpty() ||
            state->canceled) {
            return;
        }
        state->moveCleanupStarted = true;
        disconnectTaskTracking();

        QStringList directories = state->sourceDirectories;
        std::sort(directories.begin(), directories.end(),
                  PathDepthComparator{.deepestFirst = true});
        if (directories.isEmpty()) {
            setLeftRoot(leftPath_->path());
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
                        safeThis->setLeftRoot(safeThis->leftPath_->path());
                    },
                    Qt::QueuedConnection);
            });
    };

    auto reconcileTasks = [this, state, finishMoveCleanup](
                              const QVector<quint64> &taskIds, bool removed) {
        bool changed = false;
        bool directoryFailed = false;
        for (const quint64 taskId : taskIds) {
            if (!state->pendingTasks.contains(taskId))
                continue;
            const auto task = removed ? std::optional<TransferTask>{}
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
            if (removed || !task || isTerminalTransferStatus(task->status)) {
                state->pendingTasks.remove(taskId);
                changed = true;
            }
        }
        if (changed && state->discovery) {
            state->discovery->setPendingTaskCount(
                static_cast<int>(state->pendingTasks.size()));
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

    connect(
        discovery, &LocalTreeDiscovery::batchReady, this,
        [this, state, reconcileTasks](const LocalTreeDiscoveryBatch &batch) {
            if (state->canceled || !transferMgr_ || !rightIsRemote_) {
                if (state->discovery)
                    state->discovery->cancel();
                return;
            }
            state->counters = batch.counters;
            // The batch is queued in one call. A directory created by an
            // earlier batch is a task; one created by this batch is a
            // request.
            QVector<TransferRequest> requests;
            requests.reserve(batch.entries.size());
            QHash<QString, qsizetype> directoryRequests;
            QVector<QString> requestDirectoryKeys;
            requestDirectoryKeys.reserve(batch.entries.size());
            for (const auto &entry : batch.entries) {
                if (entry.rootIndex < 0 ||
                    entry.rootIndex >= state->destinationRoots.size()) {
                    continue;
                }
                QString destination = state->destinationRoots[entry.rootIndex];
                if (!entry.relativePath.isEmpty()) {
                    destination =
                        joinRemotePath(destination, entry.relativePath);
                }

                TransferBatchOptions entryOptions = state->batchOptions;
                QString parentRelative = entry.relativePath;
                const qsizetype separator =
                    parentRelative.lastIndexOf(QLatin1Char('/'));
                if (separator >= 0) {
                    parentRelative = parentRelative.left(separator);
                } else {
                    parentRelative.clear();
                }

                const QString parentKey = QString::number(entry.rootIndex) +
                                          QStringLiteral(":") + parentRelative;
                qsizetype parentRequest = -1;
                if (entry.type == LocalTreeDiscoveryEntry::Type::File &&
                    entry.relativePath.isEmpty()) {
                    entryOptions.dependsOnTaskId = 0;
                } else {
                    parentRequest = directoryRequests.value(parentKey, -1);
                    entryOptions.dependsOnTaskId =
                        state->directoryTasks.value(parentKey, 0);
                }

                if (entry.type == LocalTreeDiscoveryEntry::Type::Directory) {
                    const QString directoryKey =
                        QString::number(entry.rootIndex) + QStringLiteral(":") +
                        entry.relativePath;
                    directoryRequests.insert(directoryKey, requests.size());
                    requestDirectoryKeys.push_back(directoryKey);
                    requests.push_back(
                        {TransferTask::Type::CreateRemoteDirectory,
                         {},
                         destination,
                         entryOptions,
                         parentRequest});
                    ++state->directoryCount;
                    if (!state->sourceDirectorySet.contains(entry.localPath)) {
                        state->sourceDirectorySet.insert(entry.localPath);
                        state->sourceDirectories.push_back(entry.localPath);
                    }
                } else {
                    requestDirectoryKeys.push_back({});
                    requests.push_back({TransferTask::Type::Upload,
                                        entry.localPath, destination,
                                        entryOptions, parentRequest});
                    ++state->uploadCount;
                }
            }
            const QVector<quint64> newTaskIds =
                transferMgr_->enqueueTasks(requests);
            for (qsizetype i = 0; i < newTaskIds.size(); ++i) {
                const quint64 taskId = newTaskIds[i];
                if (!requestDirectoryKeys[i].isEmpty()) {
                    state->directoryTasks.insert(requestDirectoryKeys[i],
                                                 taskId);
                    state->directoryTaskIds.insert(taskId);
                }
                state->pendingTasks.insert(taskId);
            }
            // A fast directory task can finish between enqueue() and the
            // insertion above. Reconcile immediately so discovery
            // backpressure and move cleanup cannot remain stuck on a
            // terminal (or already-pruned) task.
            reconcileTasks(newTaskIds, false);
            if (state->discovery) {
                state->discovery->setPendingTaskCount(
                    static_cast<int>(state->pendingTasks.size()));
            }
        });

    connect(discovery, &LocalTreeDiscovery::progressChanged, this,
            [state](const LocalTreeDiscoveryCounters &counters,
                    const QString &currentPath) {
                state->counters = counters;
                if (!state->progress)
                    return;
                state->progress->setLabelText(
                    QCoreApplication::translate("MainWindow",
                                                "Scanning local items: %1\n%2")
                        .arg(counters.itemCount)
                        .arg(QDir::toNativeSeparators(currentPath)));
            });

    connect(
        discovery, &LocalTreeDiscovery::largeTreeConfirmationRequired, this,
        [this, state](const LocalTreeDiscoveryCounters &counters) {
            if (state->canceled || !state->discovery)
                return;
            const QString knownSize = QLocale().formattedDataSize(
                qint64(std::min(counters.knownBytes,
                                quint64(std::numeric_limits<qint64>::max()))));
            const auto answer = UiAlerts::question(
                this, tr("Large upload"),
                tr("This upload contains more than 100,000 items or 100 GiB "
                   "of known data.\n\nDiscovered so far: %1 items, %2.\n"
                   "Continue preparing it?")
                    .arg(counters.itemCount)
                    .arg(knownSize));
            if (answer == QMessageBox::Yes) {
                state->discovery->continueAfterLargeTreeConfirmation();
            } else {
                state->canceled = true;
                transferMgr_->cancelBatch(state->batchOptions.batchId);
                state->discovery->cancel();
            }
        });

    connect(progress, &QProgressDialog::canceled, this, [this, state] {
        if (state->canceled)
            return;
        state->canceled = true;
        transferMgr_->cancelBatch(state->batchOptions.batchId);
        if (state->discovery)
            state->discovery->cancel();
    });

    connect(discovery, &LocalTreeDiscovery::finished, this,
            [this, state, closeProgress, disconnectTaskTracking,
             countersSummary,
             finishMoveCleanup](const LocalTreeDiscoveryCounters &counters) {
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
                if (state->uploadCount > 0 || state->directoryCount > 0) {
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
         countersSummary](const LocalTreeDiscoveryCounters &counters) {
            state->canceled = true;
            transferMgr_->cancelBatch(state->batchOptions.batchId);
            closeProgress();
            disconnectTaskTracking();
            if (state->discovery)
                state->discovery->deleteLater();
            QString message = tr("Upload preparation canceled.");
            const QString notes = countersSummary(counters);
            if (!notes.isEmpty())
                message += QStringLiteral("  |  ") + notes;
            statusBar()->showMessage(message, 7000);
        };
    connect(discovery, &LocalTreeDiscovery::canceled, this, cancelFinished);

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
    if (!transferDlg_->isVisible()) {
        const QRect centeredRect = centeredQueueRect(transferDlg_, this);
        if (centeredRect.isValid())
            transferDlg_->setGeometry(centeredRect);
    }

    transferDlg_->presentAnimated();
}

void MainWindow::maybeShowTransferQueue() {
    if (prefShowQueueOnEnqueue_)
        showTransferQueue();
}

bool MainWindow::eventFilter(QObject *eventSource, QEvent *event) {
    QObject *const rightViewport =
        rightView_ ? rightView_->viewport() : nullptr;
    QObject *const leftViewport = leftView_ ? leftView_->viewport() : nullptr;
    if (eventSource != rightViewport && eventSource != leftViewport)
        return QMainWindow::eventFilter(eventSource, event);

    auto extractStagingBatchDir = [](const QMimeData *md) -> QString {
        if (!md || !md->hasFormat(kStagingBatchMime))
            return QString();
        const QByteArray raw = md->data(kStagingBatchMime);
        return QString::fromUtf8(raw).trimmed();
    };

    // A drop without local files once downloaded the remote selection to the
    // left panel; the remote panel's own drag brings the staged files.
    if (event->type() == QEvent::DragEnter ||
        event->type() == QEvent::DragMove || event->type() == QEvent::Drop) {
        auto *dropEvent = static_cast<QDropEvent *>(event);
        if (!carriesLocalFiles(dropEvent->mimeData())) {
            if (event->type() == QEvent::DragEnter &&
                carriesVirtualFiles(dropEvent->mimeData())) {
                statusBar()->showMessage(
                    tr("Files that exist only inside another application, such "
                       "as e-mail attachments, cannot be dropped here. Save "
                       "them to a folder first."),
                    6000);
            }
            dropEvent->ignore();
            return true;
        }
    }

    // Drag-and-drop over the right panel (local or remote)
    if (eventSource == rightViewport) {
        if (event->type() == QEvent::DragEnter) {
            auto *dragEnterEvent = static_cast<QDragEnterEvent *>(event);
            if (rightIsRemote_ && !rightRemoteMutationsSupported_) {
                dragEnterEvent->ignore();
                return true;
            }
            acceptDropAsCopy(dragEnterEvent);
            return true;
        } else if (event->type() == QEvent::DragMove) {
            auto *dragMoveEvent = static_cast<QDragMoveEvent *>(event);
            if (rightIsRemote_ && !rightRemoteMutationsSupported_) {
                dragMoveEvent->ignore();
                return true;
            }
            acceptDropAsCopy(dragMoveEvent);
            return true;
        } else if (event->type() == QEvent::Drop) {
            auto *dropEvent = static_cast<QDropEvent *>(event);
            const QString stagingBatchDir =
                extractStagingBatchDir(dropEvent->mimeData());
            const auto urls = dropEvent->mimeData()->urls();
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
                if (!sessionController_->hasSession() || !rightRemoteModel_) {
                    acceptDropAsCopy(dropEvent);
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
                    if (!localFileInfo.isDir() && !localFileInfo.isFile()) {
                        continue;
                    }
                    roots.push_back(
                        {localFileInfo.absoluteFilePath(),
                         joinRemotePath(remoteBase, localFileInfo.fileName())});
                }
                if (!roots.isEmpty())
                    startLocalUploadDiscovery(roots, false, true);
                acceptDropAsCopy(dropEvent);
                return true;
            } else {
                // Local copy to the right panel directory
                QDir dst(rightPath_->path());
                if (!dst.exists()) {
                    acceptDropAsCopy(dropEvent);
                    return true;
                }
                QVector<QFileInfo> sources;
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
                    sources.push_back(localFileInfo);
                }
                // As F5 does, ask before replacing an entry, once the drop is
                // over: a dialog during it would hold the application the
                // files come from.
                QTimer::singleShot(0, this, [this, sources, dst] {
                    int skipped = 0;
                    const auto pairs =
                        buildLocalDestinationPairsWithOverwritePrompt(
                            this, sources, dst, &skipped);
                    runLocalFsOperation(toLocalFsPairs(pairs), false, skipped);
                });
                acceptDropAsCopy(dropEvent);
                return true;
            }
        }
    }
    // Drag-and-drop over the left panel (local): copy
    if (eventSource == leftViewport) {
        if (event->type() == QEvent::DragEnter) {
            auto *dragEnterEvent = static_cast<QDragEnterEvent *>(event);
            acceptDropAsCopy(dragEnterEvent);
            return true;
        } else if (event->type() == QEvent::DragMove) {
            auto *dragMoveEvent = static_cast<QDragMoveEvent *>(event);
            acceptDropAsCopy(dragMoveEvent);
            return true;
        } else if (event->type() == QEvent::Drop) {
            auto *dropEvent = static_cast<QDropEvent *>(event);
            // Only the remote panel's own drag carries a staging batch to
            // delete once copied, and only a batch folder in the staging
            // root; any other drop is a plain copy.
            openscpui::AppSettings settings;
            QString stagingBatchDir;
            if (dropEvent->source() == rightView_ &&
                settings.value(openscpui::settingskeys::kAutoCleanStaging, true)
                    .toBool()) {
                stagingBatchDir = openscpui::stagingBatchPathForCleanup(
                    settings, extractStagingBatchDir(dropEvent->mimeData()));
            }
            const auto urls = dropEvent->mimeData()->urls();
            // Local copy towards the left panel
            QDir dst(leftPath_->path());
            if (!dst.exists()) {
                acceptDropAsCopy(dropEvent);
                return true;
            }
            QVector<QFileInfo> sources;
            for (const QUrl &url : urls) {
                const QString localPath = url.toLocalFile();
                if (localPath.isEmpty())
                    continue;
                QFileInfo localFileInfo(localPath);
                const QString target = dst.filePath(localFileInfo.fileName());
                // Avoid self-drop: same file/folder and same destination
                if (localFileInfo.absoluteFilePath() == target) {
                    continue;
                }
                sources.push_back(localFileInfo);
            }
            std::function<void(bool)> removeBatch;
            if (!stagingBatchDir.isEmpty()) {
                // The batch goes once this copy ends; if part of it
                // failed, its files stay where the user can find them.
                QPointer<QTreeView> view(rightView_);
                removeBatch = [stagingBatchDir, view](bool allSucceeded) {
                    if (!allSucceeded) {
                        if (auto *dragView =
                                qobject_cast<DragAwareTreeView *>(view))
                            dragView->showKeepMessage(stagingBatchDir);
                        return;
                    }
                    QThreadPool::globalInstance()->start([stagingBatchDir] {
                        openscpui::removeStagingBatch(stagingBatchDir);
                    });
                };
            }
            // As on the right panel, ask once the drop is over.
            QTimer::singleShot(0, this, [this, sources, dst, removeBatch] {
                int skipped = 0;
                const auto pairs =
                    buildLocalDestinationPairsWithOverwritePrompt(
                        this, sources, dst, &skipped);
                runLocalFsOperation(toLocalFsPairs(pairs), false, skipped,
                                    removeBatch);
            });
            acceptDropAsCopy(dropEvent);
            updateDeleteShortcutEnables();
            return true;
        }
    }
    return QMainWindow::eventFilter(eventSource, event);
}
