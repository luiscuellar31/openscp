// MainWindow local-side filesystem operations and local navigation.
#include "MainWindow.hpp"
#include "MainWindowSharedUtils.hpp"
#include "RemoteModel.hpp"
#include "TransferManager.hpp"
#include "UiAlerts.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QProcess>
#include <QPushButton>
#include <QSet>
#include <QStatusBar>
#include <QTemporaryFile>
#include <QTreeView>
#include <QUrl>

#include <memory>
#include <thread>

static constexpr int NAME_COL = 0;

static bool copyEntryRecursively(const QString &srcPath, const QString &dstPath,
                                 QString &error) {
    QFileInfo srcInfo(srcPath);

    if (srcInfo.isFile()) {
        // Ensure destination directory
        QDir().mkpath(QFileInfo(dstPath).dir().absolutePath());
        if (QFile::exists(dstPath))
            QFile::remove(dstPath);
        if (!QFile::copy(srcPath, dstPath)) {
            error = QString(QCoreApplication::translate(
                                "MainWindow", "Could not copy file: %1"))
                        .arg(srcPath);
            return false;
        }
        return true;
    }

    if (srcInfo.isDir()) {
        // Create destination directory
        if (!QDir().mkpath(dstPath)) {
            error = QString(QCoreApplication::translate(
                                "MainWindow",
                                "Could not create destination folder: %1"))
                        .arg(dstPath);
            return false;
        }
        // Iterate recursively
        QDirIterator dirIterator(srcPath,
                                 QDir::NoDotAndDotDot | QDir::AllEntries,
                                 QDirIterator::Subdirectories);
        while (dirIterator.hasNext()) {
            dirIterator.next();
            const QFileInfo entryFileInfo = dirIterator.fileInfo();
            const QString relativePath =
                QDir(srcPath).relativeFilePath(entryFileInfo.absoluteFilePath());
            const QString target = QDir(dstPath).filePath(relativePath);

            if (entryFileInfo.isDir()) {
                if (!QDir().mkpath(target)) {
                    error =
                        QString(
                            QCoreApplication::translate(
                                "MainWindow",
                                "Could not create destination subfolder: %1"))
                            .arg(target);
                    return false;
                }
            } else {
                // Ensure parent directory exists
                QDir().mkpath(QFileInfo(target).dir().absolutePath());
                if (QFile::exists(target))
                    QFile::remove(target);
                if (!QFile::copy(entryFileInfo.absoluteFilePath(), target)) {
                    error = QString(QCoreApplication::translate(
                                        "MainWindow", "Failed to copy: %1"))
                                .arg(entryFileInfo.absoluteFilePath());
                    return false;
                }
            }
        }
        return true;
    }

    error = QCoreApplication::translate(
        "MainWindow", "Source entry is neither file nor folder.");
    return false;
}

static void revealInFolder(const QString &filePath) {
#if defined(Q_OS_MAC)
    // macOS: use 'open -R' to reveal in Finder
    QProcess::startDetached("open", {"-R", filePath});
#elif defined(Q_OS_WIN)
    // Windows: explorer /select,<path>
    QString arg = "/select," + QDir::toNativeSeparators(filePath);
    QProcess::startDetached("explorer", {arg});
#else
    // Linux/others: try to open the containing directory
    const QString dir = QFileInfo(filePath).dir().absolutePath();
    QDesktopServices::openUrl(QUrl::fromLocalFile(dir));
#endif
}

static QString buildLocalFsSummaryMessage(bool deleteSource, int successCount,
                                          int failureCount, int skippedCount) {
    if (deleteSource) {
        return QString(QCoreApplication::translate(
                           "MainWindow",
                           "Moved OK: %1  |  Failed: %2  |  Skipped: %3"))
            .arg(successCount)
            .arg(failureCount)
            .arg(skippedCount);
    }
    if (skippedCount > 0) {
        return QString(QCoreApplication::translate(
                           "MainWindow",
                           "Copied: %1  |  Failed: %2  |  Skipped: %3"))
            .arg(successCount)
            .arg(failureCount)
            .arg(skippedCount);
    }
    return QString(
               QCoreApplication::translate("MainWindow",
                                           "Copied: %1  |  Failed: %2"))
        .arg(successCount)
        .arg(failureCount);
}

QString MainWindow::preferredLocalHomePath() const {
    const QString home = QDir::homePath();
    if (!home.isEmpty()) {
        const QString absoluteHome = QDir(home).absolutePath();
        if (QDir(absoluteHome).exists())
            return absoluteHome;
    }
    return QDir::rootPath();
}

QVector<MainWindow::LocalFsPair>
MainWindow::toLocalFsPairs(const QVector<QPair<QString, QString>> &pairs) {
    QVector<LocalFsPair> localFsPairs;
    localFsPairs.reserve(pairs.size());
    for (const auto &pair : pairs)
        localFsPairs.push_back({pair.first, pair.second});
    return localFsPairs;
}

void MainWindow::chooseLeftDir() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select left folder"), leftPath_->text());
    if (!dir.isEmpty())
        setLeftRoot(dir);
}

// Browse and set the right pane root directory (local mode).
void MainWindow::chooseRightDir() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select right folder"), rightPath_->text());
    if (!dir.isEmpty())
        setRightRoot(dir);
}

// Navigate left pane to the path typed by the user.
void MainWindow::leftPathEntered() { setLeftRoot(leftPath_->text()); }

// Navigate right pane (local or remote) to the path typed by the user.
void MainWindow::rightPathEntered() {
    if (rightIsRemote_)
        setRightRemoteRoot(rightPath_->text());
    else
        setRightRoot(rightPath_->text());
}

// Set the left pane root, validating the path and updating view/status.
void MainWindow::setLeftRoot(const QString &path) {
    if (QDir(path).exists()) {
        const QString normalized = QDir(path).absolutePath();
        leftPath_->setText(normalized);
        leftView_->setRootIndex(leftModel_->index(normalized));
        addRecentLocalPath(normalized);
        refreshLeftBreadcrumbs();
        statusBar()->showMessage(tr("Left: ") + normalized, 3000);
        updateDeleteShortcutEnables();
    } else {
        UiAlerts::warning(this, tr("Invalid path"),
                             tr("Folder does not exist."));
    }
}

// Set the right (local) pane root and update view/status.
void MainWindow::setRightRoot(const QString &path) {
    if (QDir(path).exists()) {
        const QString normalized = QDir(path).absolutePath();
        rightPath_->setText(normalized);
        rightView_->setRootIndex(
            rightLocalModel_->index(normalized)); // <-- here
        addRecentLocalPath(normalized);
        refreshRightBreadcrumbs();
        statusBar()->showMessage(tr("Right: ") + normalized, 3000);
        updateDeleteShortcutEnables();
    } else {
        UiAlerts::warning(this, tr("Invalid path"),
                             tr("Folder does not exist."));
    }
}

void MainWindow::runLocalFsOperation(const QVector<LocalFsPair> &pairs,
                                     bool deleteSource, int skippedCount) {
    if (pairs.isEmpty()) {
        statusBar()->showMessage(
            buildLocalFsSummaryMessage(deleteSource, 0, 0, skippedCount), 5000);
        return;
    }

    ++m_localFsJobsInFlight_;
    statusBar()->showMessage(
        deleteSource ? tr("Moving selected items...")
                     : tr("Copying selected items..."),
        1500);

    QPointer<MainWindow> self(this);
    std::thread([self, pairs, deleteSource, skippedCount]() {
        int successCount = 0;
        int failureCount = 0;
        QString lastError;

        for (const auto &pair : pairs) {
            QString copyError;
            if (copyEntryRecursively(pair.sourcePath, pair.targetPath,
                                     copyError)) {
                if (deleteSource) {
                    const QFileInfo srcInfo(pair.sourcePath);
                    const bool removed =
                        srcInfo.isDir()
                            ? QDir(pair.sourcePath).removeRecursively()
                            : QFile::remove(pair.sourcePath);
                    if (removed || !QFileInfo::exists(pair.sourcePath)) {
                        ++successCount;
                    } else {
                        ++failureCount;
                        lastError =
                            QString(QCoreApplication::translate(
                                        "MainWindow",
                                        "Could not delete source: %1"))
                                .arg(pair.sourcePath);
                    }
                } else {
                    ++successCount;
                }
            } else {
                ++failureCount;
                lastError = copyError;
            }
        }

        QObject *const app = QCoreApplication::instance();
        if (!app)
            return;
        QMetaObject::invokeMethod(
            app, [self, successCount, failureCount, skippedCount, lastError,
                  deleteSource]() {
                if (!self)
                    return;

                --self->m_localFsJobsInFlight_;

                QString statusMessage = buildLocalFsSummaryMessage(
                    deleteSource, successCount, failureCount, skippedCount);
                if (failureCount > 0 && !lastError.isEmpty()) {
                    statusMessage +=
                        "\n" +
                        QCoreApplication::translate("MainWindow",
                                                    "Last error: ") +
                        lastError;
                }
                self->statusBar()->showMessage(statusMessage, 6000);

                self->setLeftRoot(self->leftPath_->text());
                if (!self->rightIsRemote_) {
                    self->setRightRoot(self->rightPath_->text());
                }
                self->updateDeleteShortcutEnables();
            },
            Qt::QueuedConnection);
    }).detach();
}

void MainWindow::copyLeftToRight() {
    if (rightIsRemote_) {
        // ---- REMOTE branch: upload files (PUT) to the current remote
        // directory ----
        if (!sftp_) {
            UiAlerts::warning(this, tr("Remote"),
                                 tr("No active remote session."));
            return;
        }
        const bool scpMode = isScpTransferMode();
        if (!scpMode && !rightRemoteModel_) {
            UiAlerts::warning(this, tr("Remote"),
                              tr("No active remote session."));
            return;
        }

        // Selection on the left panel (local source)
        auto selectionModel = leftView_->selectionModel();
        if (!selectionModel) {
            UiAlerts::warning(this, tr("Copy"),
                                 tr("No selection available."));
            return;
        }
        const auto rows = selectionModel->selectedRows(NAME_COL);
        if (rows.isEmpty()) {
            UiAlerts::information(
                this, tr("Copy"), tr("No entries selected in the left panel."));
            return;
        }

        // Always enqueue uploads
        const QString remoteBase =
            scpMode ? normalizeRemotePath(
                          rightPath_ ? rightPath_->text() : QString())
                    : rightRemoteModel_->rootPath();
        int enqueuedCount = 0;
        int skippedDirs = 0;
        for (const QModelIndex &idx : rows) {
            const QFileInfo sourceFileInfo = leftModel_->fileInfo(idx);
            if (sourceFileInfo.isDir()) {
                if (scpMode) {
                    ++skippedDirs;
                    continue;
                }
                const QString remoteDirBase =
                    joinRemotePath(remoteBase, sourceFileInfo.fileName());
                QDirIterator dirIterator(sourceFileInfo.absoluteFilePath(),
                                         QDir::NoDotAndDotDot |
                                             QDir::AllEntries,
                                         QDirIterator::Subdirectories);
                while (dirIterator.hasNext()) {
                    dirIterator.next();
                    const QFileInfo childFileInfo = dirIterator.fileInfo();
                    if (!childFileInfo.isFile())
                        continue;
                    const QString relativePath =
                        QDir(sourceFileInfo.absoluteFilePath())
                            .relativeFilePath(childFileInfo.absoluteFilePath());
                    const QString remoteTargetPath =
                        joinRemotePath(remoteDirBase, relativePath);
                    transferMgr_->enqueueUpload(childFileInfo.absoluteFilePath(),
                                                remoteTargetPath);
                    ++enqueuedCount;
                }
            } else {
                const QString remoteTargetPath =
                    joinRemotePath(remoteBase, sourceFileInfo.fileName());
                transferMgr_->enqueueUpload(sourceFileInfo.absoluteFilePath(),
                                            remoteTargetPath);
                ++enqueuedCount;
            }
        }
        if (enqueuedCount > 0) {
            QString statusMessage =
                QString(tr("Queued: %1 uploads")).arg(enqueuedCount);
            if (skippedDirs > 0) {
                statusMessage +=
                    QStringLiteral("  |  ") +
                    tr("Skipped folders in transfer-only mode: %1")
                        .arg(skippedDirs);
            }
            statusBar()->showMessage(statusMessage, 4000);
            maybeShowTransferQueue();
        } else if (skippedDirs > 0) {
            UiAlerts::information(
                this, tr("Upload"),
                tr("Transfer-only mode currently supports uploading files "
                   "only."));
        }
        return;
    }

    // ---- LOCAL→LOCAL branch: existing logic as-is ----
    const QString dstDirPath = rightPath_->text();
    QDir dstDir(dstDirPath);
    if (!dstDir.exists()) {
        UiAlerts::warning(this, tr("Invalid destination"),
                             tr("Destination folder does not exist."));
        return;
    }

    auto selectionModel = leftView_->selectionModel();
    if (!selectionModel) {
        UiAlerts::warning(this, tr("Copy"), tr("No selection available."));
        return;
    }
    const auto rows = selectionModel->selectedRows(NAME_COL);
    if (rows.isEmpty()) {
        UiAlerts::information(this, tr("Copy"),
                                 tr("No entries selected in the left panel."));
        return;
    }

    QVector<QFileInfo> sources;
    sources.reserve(rows.size());
    for (const QModelIndex &idx : rows)
        sources.push_back(leftModel_->fileInfo(idx));

    int skipped = 0;
    const QVector<QPair<QString, QString>> selectedPairs =
        buildLocalDestinationPairsWithOverwritePrompt(this, sources, dstDir,
                                                      &skipped);
    const QVector<LocalFsPair> pairs = toLocalFsPairs(selectedPairs);
    runLocalFsOperation(pairs, false, skipped);
}

void MainWindow::moveLeftToRight() {
    if (rightIsRemote_) {
        if (!sftp_ || !rightRemoteModel_) {
            UiAlerts::warning(this, tr("Remote"),
                                 tr("No active remote session."));
            return;
        }
        const auto rows = leftView_->selectionModel()->selectedRows(NAME_COL);
        if (rows.isEmpty()) {
            UiAlerts::information(
                this, tr("Move"), tr("No entries selected in the left panel."));
            return;
        }
        if (UiAlerts::question(this, tr("Confirm move"),
                                  tr("This will upload to the server and "
                                     "delete the local source.\nContinue?")) !=
            QMessageBox::Yes)
            return;
        const QString remoteBase = rightRemoteModel_->rootPath();
        struct UploadPair {
            QString localPath;
            QString remotePath;
            QString topLocalDir; // non-empty only for files that belong to a
                                 // moved directory
        };
        QVector<UploadPair> pairs;
        int skippedPrep = 0;
        int movedEmptyDirs = 0;
        QString prepError;

        auto ensureRemoteDir = [&](const QString &remoteDirPath) -> bool {
            if (remoteDirPath.isEmpty())
                return true;
            QString currentPath = "/";
            const QStringList pathParts =
                remoteDirPath.split('/', Qt::SkipEmptyParts);
            for (const QString &pathPart : pathParts) {
                const QString nextPath = joinRemotePath(currentPath, pathPart);
                bool isDirectory = false;
                std::string existsError;
                const bool exists = sftp_->exists(nextPath.toStdString(),
                                                  isDirectory, existsError);
                if (!existsError.empty()) {
                    prepError = QString::fromStdString(existsError);
                    return false;
                }
                if (!exists) {
                    std::string mkdirError;
                    if (!sftp_->mkdir(nextPath.toStdString(), mkdirError,
                                      0755)) {
                        prepError = QString::fromStdString(mkdirError);
                        return false;
                    }
                }
                currentPath = nextPath;
            }
            return true;
        };

        for (const QModelIndex &idx : rows) {
            const QFileInfo sourceFileInfo = leftModel_->fileInfo(idx);
            if (sourceFileInfo.isDir()) {
                const QString topLocalDir = sourceFileInfo.absoluteFilePath();
                const QString baseRemoteDir =
                    joinRemotePath(remoteBase, sourceFileInfo.fileName());
                if (!ensureRemoteDir(baseRemoteDir)) {
                    ++skippedPrep;
                    continue;
                }
                const int pairStart = pairs.size();
                bool dirPrepFailed = false;
                int filesInDir = 0;
                QDirIterator dirIterator(topLocalDir,
                                         QDir::NoDotAndDotDot |
                                             QDir::AllEntries,
                                         QDirIterator::Subdirectories);
                while (dirIterator.hasNext()) {
                    dirIterator.next();
                    const QFileInfo childFileInfo = dirIterator.fileInfo();
                    const QString relativePath =
                        QDir(topLocalDir)
                            .relativeFilePath(childFileInfo.absoluteFilePath());
                    const QString remoteTargetPath =
                        joinRemotePath(baseRemoteDir, relativePath);
                    if (childFileInfo.isDir()) {
                        if (!ensureRemoteDir(remoteTargetPath)) {
                            dirPrepFailed = true;
                            break;
                        }
                        continue;
                    }
                    if (!childFileInfo.isFile())
                        continue;
                    pairs.push_back(
                        {childFileInfo.absoluteFilePath(), remoteTargetPath,
                         topLocalDir});
                    ++filesInDir;
                }
                if (dirPrepFailed) {
                    pairs.resize(pairStart);
                    ++skippedPrep;
                    continue;
                }
                if (filesInDir == 0) {
                    if (QDir(topLocalDir).removeRecursively())
                        ++movedEmptyDirs;
                    else {
                        ++skippedPrep;
                        prepError =
                            tr("Could not delete source: ") + topLocalDir;
                    }
                }
            } else if (sourceFileInfo.isFile()) {
                const QString remoteTargetPath =
                    joinRemotePath(remoteBase, sourceFileInfo.fileName());
                pairs.push_back(
                    {sourceFileInfo.absoluteFilePath(), remoteTargetPath,
                     QString()});
            }
        }

        for (const auto &pair : pairs)
            transferMgr_->enqueueUpload(pair.localPath, pair.remotePath);
        if (!pairs.isEmpty()) {
            statusBar()->showMessage(
                QString(tr("Queued: %1 uploads (move)")).arg(pairs.size()),
                4000);
            maybeShowTransferQueue();
        } else if (movedEmptyDirs > 0) {
            statusBar()->showMessage(
                QString(tr("Moved OK: %1 (empty folders)")).arg(movedEmptyDirs),
                4000);
        } else if (skippedPrep > 0) {
            QString statusMessage =
                QString(tr("Could not prepare items to move: %1"))
                    .arg(skippedPrep);
            if (!prepError.isEmpty())
                statusMessage += "\n" + tr("Last error: ") + prepError;
            statusBar()->showMessage(statusMessage, 5000);
        }

        // Local cleanup on successful upload, without blocking UI.
        if (!pairs.isEmpty()) {
            QVector<QPair<QString, QString>> transferPairs;
            transferPairs.reserve(pairs.size());
            for (const auto &pair : pairs)
                transferPairs.push_back({pair.localPath, pair.remotePath});

            struct MoveUploadState {
                QSet<QString> pendingLocalFiles;
                QSet<QString> processedLocalFiles;
                QHash<QString, QString> fileToTopDir;
                QHash<QString, int> remainingInTopDir;
                QSet<QString> failedTopDirs;
                int movedOk = 0;
                int failed = 0;
                int skipped = 0;
                QString lastError;
            };
            auto state = std::make_shared<MoveUploadState>();
            state->movedOk = movedEmptyDirs;
            state->skipped = skippedPrep;
            state->lastError = prepError;

            for (const auto &pair : pairs) {
                state->pendingLocalFiles.insert(pair.localPath);
                if (!pair.topLocalDir.isEmpty()) {
                    state->fileToTopDir.insert(pair.localPath, pair.topLocalDir);
                    state->remainingInTopDir[pair.topLocalDir] =
                        state->remainingInTopDir.value(pair.topLocalDir) + 1;
                }
            }

            auto connPtr = std::make_shared<QMetaObject::Connection>();
            *connPtr = connect(
                transferMgr_, &TransferManager::tasksChanged, this,
                [this, state, remoteBase, connPtr, transferPairs]() {
                    const auto tasks = transferMgr_->tasksSnapshot();

                    for (const auto &task : tasks) {
                        if (task.type != TransferTask::Type::Upload)
                            continue;
                        const QString local = task.src;
                        if (!state->pendingLocalFiles.contains(local))
                            continue;
                        if (state->processedLocalFiles.contains(local))
                            continue;
                        if (task.status != TransferTask::Status::Done &&
                            task.status != TransferTask::Status::Error &&
                            task.status != TransferTask::Status::Canceled) {
                            continue;
                        }

                        state->processedLocalFiles.insert(local);
                        const QString topDir = state->fileToTopDir.value(local);
                        const bool uploadDone =
                            (task.status == TransferTask::Status::Done);
                        if (uploadDone) {
                            const bool removed = !QFileInfo::exists(local) ||
                                                 QFile::remove(local);
                            if (removed) {
                                ++state->movedOk;
                            } else {
                                ++state->failed;
                                state->lastError =
                                    tr("Could not delete source: ") + local;
                                if (!topDir.isEmpty())
                                    state->failedTopDirs.insert(topDir);
                            }
                        } else {
                            ++state->failed;
                            if (!task.error.isEmpty())
                                state->lastError = task.error;
                            if (!topDir.isEmpty())
                                state->failedTopDirs.insert(topDir);
                        }

                        if (!topDir.isEmpty()) {
                            const int rem = qMax(
                                0, state->remainingInTopDir.value(topDir) - 1);
                            state->remainingInTopDir[topDir] = rem;
                            if (rem == 0 &&
                                !state->failedTopDirs.contains(topDir) &&
                                QDir(topDir).exists()) {
                                if (!QDir(topDir).removeRecursively()) {
                                    ++state->failed;
                                    state->lastError =
                                        tr("Could not delete source: ") +
                                        topDir;
                                }
                            }
                        }
                    }

                    const bool allFinal = areTransferPairsFinal(
                        tasks, TransferTask::Type::Upload, transferPairs);

                    if (allFinal) {
                QString statusMessage =
                    QString(tr("Moved OK: %1  |  Failed: %2  "
                               "|  Skipped: %3"))
                        .arg(state->movedOk)
                        .arg(state->failed)
                        .arg(state->skipped);
                if (state->failed > 0 && !state->lastError.isEmpty()) {
                    statusMessage += "\n" + tr("Last error: ") + state->lastError;
                }
                statusBar()->showMessage(statusMessage, 6000);
                setLeftRoot(leftPath_->text());
                QString refreshError;
                if (rightRemoteModel_)
                    rightRemoteModel_->setRootPath(remoteBase, &refreshError);
                QObject::disconnect(*connPtr);
            }
        });
        }
        return;
    }

    // ---- Existing LOCAL→LOCAL branch ----
    const QString dstDirPath = rightPath_->text();
    QDir dstDir(dstDirPath);
    if (!dstDir.exists()) {
        UiAlerts::warning(this, tr("Invalid destination"),
                             tr("Destination folder does not exist."));
        return;
    }
    const auto rows = leftView_->selectionModel()->selectedRows(NAME_COL);
    if (rows.isEmpty()) {
        UiAlerts::information(this, tr("Move"),
                                 tr("No entries selected in the left panel."));
        return;
    }
    if (UiAlerts::question(
            this, tr("Confirm move"),
            tr("This will copy and then delete the source.\nContinue?")) !=
        QMessageBox::Yes)
        return;
    QVector<QFileInfo> sources;
    sources.reserve(rows.size());
    for (const QModelIndex &idx : rows)
        sources.push_back(leftModel_->fileInfo(idx));

    int skipped = 0;
    const QVector<QPair<QString, QString>> selectedPairs =
        buildLocalDestinationPairsWithOverwritePrompt(this, sources, dstDir,
                                                      &skipped);
    const QVector<LocalFsPair> pairs = toLocalFsPairs(selectedPairs);
    runLocalFsOperation(pairs, true, skipped);
}

void MainWindow::deleteFromLeft() {
    const auto rows = leftView_->selectionModel()->selectedRows(NAME_COL);
    if (rows.isEmpty()) {
        UiAlerts::information(this, tr("Delete"),
                                 tr("No entries selected in the left panel."));
        return;
    }
    if (UiAlerts::warning(this, tr("Confirm delete"),
                             tr("This will permanently delete the selected "
                                "items in the left panel.\nContinue?"),
                             QMessageBox::Yes | QMessageBox::No) !=
        QMessageBox::Yes)
        return;
    int deletedCount = 0;
    int failedCount = 0;
    for (const QModelIndex &idx : rows) {
        const QFileInfo selectedFileInfo = leftModel_->fileInfo(idx);
        bool removed =
            selectedFileInfo.isDir()
                ? QDir(selectedFileInfo.absoluteFilePath()).removeRecursively()
                : QFile::remove(selectedFileInfo.absoluteFilePath());
        if (removed)
            ++deletedCount;
        else
            ++failedCount;
    }
    statusBar()->showMessage(
        QString(tr("Deleted: %1  |  Failed: %2"))
            .arg(deletedCount)
            .arg(failedCount),
        5000);
}

void MainWindow::goUpLeft() {
    QString currentPath = leftPath_->text();
    QDir currentDir(currentPath);
    if (!currentDir.cdUp())
        return;
    setLeftRoot(currentDir.absolutePath());
    updateDeleteShortcutEnables();
}

void MainWindow::goHomeLeft() {
    setLeftRoot(preferredLocalHomePath());
    updateDeleteShortcutEnables();
}

void MainWindow::openLocalPathWithPreference(const QString &localPath) {
    const QString mode = prefOpenBehaviorMode_.trimmed().toLower();
    if (mode == QStringLiteral("reveal")) {
        revealInFolder(localPath);
        return;
    }
    if (mode == QStringLiteral("open")) {
        QDesktopServices::openUrl(QUrl::fromLocalFile(localPath));
        return;
    }

    QMessageBox box(this);
    UiAlerts::configure(box);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Opening preference"));
    box.setText(tr("How do you want to open this file?"));
    QPushButton *btnOpen = box.addButton(tr("Open file"), QMessageBox::NoRole);
    QPushButton *btnReveal =
        box.addButton(tr("Show folder"), QMessageBox::AcceptRole);
    box.setDefaultButton(btnReveal);
    box.exec();
    if (box.clickedButton() == btnOpen)
        QDesktopServices::openUrl(QUrl::fromLocalFile(localPath));
    else
        revealInFolder(localPath);
}

void MainWindow::leftItemActivated(const QModelIndex &idx) {
    if (!leftModel_)
        return;
    const QFileInfo selectedFileInfo = leftModel_->fileInfo(idx);
    if (selectedFileInfo.isDir()) {
        setLeftRoot(selectedFileInfo.absoluteFilePath());
    } else if (selectedFileInfo.isFile()) {
        openLocalPathWithPreference(selectedFileInfo.absoluteFilePath());
    }
}

void MainWindow::renameLeftSelected() {
    auto selectionModel = leftView_->selectionModel();
    if (!selectionModel)
        return;
    const auto rows = selectionModel->selectedRows();
    if (rows.size() != 1) {
        UiAlerts::information(this, tr("Rename"),
                                 tr("Select exactly one item."));
        return;
    }
    const QModelIndex selectedIndex = rows.first();
    const QFileInfo selectedFileInfo = leftModel_->fileInfo(selectedIndex);
    bool inputAccepted = false;
    const QString newName =
        QInputDialog::getText(this, tr("Rename"), tr("New name:"),
                              QLineEdit::Normal, selectedFileInfo.fileName(),
                              &inputAccepted);
    if (!inputAccepted || newName.isEmpty() ||
        newName == selectedFileInfo.fileName())
        return;
    const QString newPath = QDir(selectedFileInfo.absolutePath()).filePath(newName);
    bool renamed = QFile::rename(selectedFileInfo.absoluteFilePath(), newPath);
    if (!renamed)
        renamed = QDir(selectedFileInfo.absolutePath())
                      .rename(selectedFileInfo.absoluteFilePath(), newPath);
    if (!renamed) {
        UiAlerts::critical(this, tr("Local"), tr("Could not rename."));
        return;
    }
    setLeftRoot(leftPath_->text());
}

// Create a new directory in the left (local) pane.
void MainWindow::newDirLeft() {
    QString name;
    if (!promptValidEntryName(this, tr("New folder"), tr("Name:"), {}, name))
        return;
    QDir base(leftPath_->text());
    if (!base.mkpath(base.filePath(name))) {
        UiAlerts::critical(this, tr("Local"),
                              tr("Could not create folder."));
        return;
    }
    setLeftRoot(base.absolutePath());
}

// Create a new empty file in the left (local) pane.
void MainWindow::newFileLeft() {
    QString name;
    if (!promptValidEntryName(this, tr("New file"), tr("Name:"), {}, name))
        return;
    QDir base(leftPath_->text());
    const QString path = base.filePath(name);
    if (QFileInfo::exists(path)) {
        if (UiAlerts::question(
                this, tr("File exists"),
                tr("«%1» already exists.\nOverwrite?").arg(name),
                QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
            return;
    }
    QFile newFile(path);
    if (!newFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        UiAlerts::critical(this, tr("Local"), tr("Could not create file."));
        return;
    }
    newFile.close();
    setLeftRoot(base.absolutePath());
    statusBar()->showMessage(tr("File created: ") + path, 4000);
}

void MainWindow::showLeftContextMenu(const QPoint &pos) {
    if (!leftContextMenu_)
        leftContextMenu_ = new QMenu(this);
    // Selection and ability to go up
    bool hasSel = false;
    if (auto selectionModel = leftView_->selectionModel()) {
        hasSel = !selectionModel->selectedRows(NAME_COL).isEmpty();
    }
    QDir currentDir(leftPath_ ? leftPath_->text() : QString());
    bool canGoUp = currentDir.cdUp();

    QAction *copyToRight = nullptr;
    QAction *moveToRight = nullptr;
    if (hasSel) {
        copyToRight = new QAction(tr("Copy to right panel"), leftContextMenu_);
        connect(copyToRight, &QAction::triggered, this,
                &MainWindow::copyLeftToRight);
        moveToRight = new QAction(tr("Move to right panel"), leftContextMenu_);
        connect(moveToRight, &QAction::triggered, this,
                &MainWindow::moveLeftToRight);
    }

    QVector<QAction *> entries;
    if (canGoUp)
        entries.push_back(actUpLeft_);
    entries.push_back(actNewFileLeft_);
    entries.push_back(actNewDirLeft_);
    if (hasSel) {
        entries.push_back(actRenameLeft_);
        entries.push_back(nullptr);
        entries.push_back(copyToRight);
        entries.push_back(moveToRight);
        entries.push_back(actDelete_);
    }
    rebuildContextMenu(leftContextMenu_, entries);

    leftContextMenu_->popup(leftView_->viewport()->mapToGlobal(pos));
}
