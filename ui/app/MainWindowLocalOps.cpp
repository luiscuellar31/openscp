// MainWindow local-side filesystem operations and local navigation.
#include "app/MainWindow.hpp"
#include "logic/common/MainWindowSharedUtils.hpp"
#include "logic/common/UiAlerts.hpp"
#include "logic/connections/SessionController.hpp"
#include "logic/remote/RemoteModel.hpp"
#include "logic/remote/RemoteOperationController.hpp"
#include "logic/transfers/TransferManager.hpp"
#include "widgets/navigation/PathNavigationBar.hpp"
#include "widgets/platform/PlatformPathActions.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QPushButton>
#include <QStatusBar>
#include <QThreadPool>
#include <QUuid>

#include <algorithm>
#include <functional>
#include <memory>

namespace {

bool entryExists(const QString &path) {
    const QFileInfo info(path);
    return info.exists() || info.isSymLink();
}

bool removeEntry(const QString &path) {
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink())
        return true;
    if (info.isDir() && !info.isSymLink())
        return QDir(path).removeRecursively();
    return QFile::remove(path);
}

bool renameEntry(const QString &from, const QString &to) {
    const QFileInfo info(from);
    if (info.isDir() && !info.isSymLink())
        return QDir().rename(from, to);
    return QFile::rename(from, to);
}

bool copyEntryToEmptyDestination(const QString &srcPath, const QString &dstPath,
                                 QString &error) {
    QFileInfo srcInfo(srcPath);

    if (srcInfo.isFile()) {
        QDir().mkpath(QFileInfo(dstPath).dir().absolutePath());
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
            const QString relativePath = QDir(srcPath).relativeFilePath(
                entryFileInfo.absoluteFilePath());
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
                QDir().mkpath(QFileInfo(target).dir().absolutePath());
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

bool copyEntryRecursively(const QString &srcPath, const QString &dstPath,
                          QString &error) {
    const QString destinationParent = QFileInfo(dstPath).dir().absolutePath();
    if (!QDir().mkpath(destinationParent)) {
        error = QString(QCoreApplication::translate(
                            "MainWindow",
                            "Could not create destination folder: %1"))
                    .arg(destinationParent);
        return false;
    }

    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString stagedPath =
        QDir(destinationParent)
            .filePath(QStringLiteral(".openscp-stage-") + token);
    const QString backupPath =
        QDir(destinationParent)
            .filePath(QStringLiteral(".openscp-backup-") + token);

    if (!copyEntryToEmptyDestination(srcPath, stagedPath, error)) {
        (void)removeEntry(stagedPath);
        return false;
    }

    const bool hadDestination = entryExists(dstPath);
    if (hadDestination && !renameEntry(dstPath, backupPath)) {
        (void)removeEntry(stagedPath);
        error = QString(QCoreApplication::translate(
                            "MainWindow",
                            "Could not preserve existing destination: %1"))
                    .arg(dstPath);
        return false;
    }

    if (!renameEntry(stagedPath, dstPath)) {
        (void)removeEntry(stagedPath);
        const bool restored =
            !hadDestination || renameEntry(backupPath, dstPath);
        error =
            restored
                ? QString(
                      QCoreApplication::translate(
                          "MainWindow", "Could not publish completed copy: %1"))
                      .arg(dstPath)
                : QString(QCoreApplication::translate(
                              "MainWindow",
                              "Could not publish completed copy or restore the "
                              "previous destination. Recovery copy: %1"))
                      .arg(backupPath);
        return false;
    }

    if (hadDestination)
        (void)removeEntry(backupPath);
    return true;
}

QString buildLocalFsSummaryMessage(bool deleteSource, int successCount,
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
    return QString(QCoreApplication::translate("MainWindow",
                                               "Copied: %1  |  Failed: %2"))
        .arg(successCount)
        .arg(failureCount);
}

} // namespace

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

void MainWindow::runLocalFsSelection(const QModelIndexList &rows,
                                     QFileSystemModel *sourceModel,
                                     const QDir &destination,
                                     bool deleteSource) {
    QVector<QFileInfo> sources;
    sources.reserve(rows.size());
    for (const QModelIndex &index : rows)
        sources.push_back(sourceModel->fileInfo(index));

    int skipped = 0;
    const auto selectedPairs = buildLocalDestinationPairsWithOverwritePrompt(
        this, sources, destination, &skipped);
    runLocalFsOperation(toLocalFsPairs(selectedPairs), deleteSource, skipped);
}

void MainWindow::chooseLeftDir() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select left folder"), leftPath_->path());
    if (!dir.isEmpty())
        setLeftRoot(dir);
}

// Browse and set the right pane root directory (local mode).
void MainWindow::chooseRightDir() {
    const QString dir = QFileDialog::getExistingDirectory(
        this, tr("Select right folder"), rightPath_->path());
    if (!dir.isEmpty())
        setRightRoot(dir);
}

// Set the left pane root, validating the path and updating view/status.
void MainWindow::setLeftRoot(const QString &path) {
    if (QDir(path).exists()) {
        const QString normalized = QDir(path).absolutePath();
        leftPath_->setPath(normalized);
        leftView_->setRootIndex(leftModel_->index(normalized));
        addRecentLocalPath(normalized);
        refreshLeftPathNavigation();
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
        rightPath_->setPath(normalized);
        rightView_->setRootIndex(
            rightLocalModel_->index(normalized)); // <-- here
        addRecentLocalPath(normalized);
        refreshRightPathNavigation();
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

    ++localFsJobsInFlight_;
    statusBar()->showMessage(deleteSource ? tr("Moving selected items...")
                                          : tr("Copying selected items..."),
                             1500);

    QPointer<MainWindow> self(this);
    QThreadPool::globalInstance()->start([self, pairs, deleteSource,
                                          skippedCount]() {
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
                        lastError = QString(QCoreApplication::translate(
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
            app,
            [self, successCount, failureCount, skippedCount, lastError,
             deleteSource]() {
                if (!self)
                    return;

                --self->localFsJobsInFlight_;

                QString statusMessage = buildLocalFsSummaryMessage(
                    deleteSource, successCount, failureCount, skippedCount);
                if (failureCount > 0 && !lastError.isEmpty()) {
                    statusMessage += "\n" +
                                     QCoreApplication::translate(
                                         "MainWindow", "Last error: ") +
                                     lastError;
                }
                self->statusBar()->showMessage(statusMessage, 6000);

                self->setLeftRoot(self->leftPath_->path());
                if (!self->rightIsRemote_) {
                    self->setRightRoot(self->rightPath_->path());
                }
                self->updateDeleteShortcutEnables();
            },
            Qt::QueuedConnection);
    });
}

void MainWindow::copyLeftToRight() {
    if (rightIsRemote_) {
        // ---- REMOTE branch: upload files (PUT) to the current remote
        // directory ----
        if (!sessionController_->client()) {
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
            UiAlerts::warning(this, tr("Copy"), tr("No selection available."));
            return;
        }
        const auto rows = selectionModel->selectedRows(kNameColumn);
        if (rows.isEmpty()) {
            UiAlerts::information(this, tr("Copy"),
                                  tr("No entries selected in the left panel."));
            return;
        }

        const QString remoteBase =
            scpMode ? normalizeRemotePath(rightPath_ ? rightPath_->path()
                                                     : QString())
                    : rightRemoteModel_->rootPath();
        QVector<QPair<QString, QString>> roots;
        roots.reserve(rows.size());
        int skippedDirs = 0;
        for (const QModelIndex &idx : rows) {
            const QFileInfo sourceFileInfo = leftModel_->fileInfo(idx);
            if (sourceFileInfo.isDir()) {
                if (scpMode) {
                    ++skippedDirs;
                    continue;
                }
            } else if (!sourceFileInfo.isFile()) {
                continue;
            }
            roots.push_back(
                {sourceFileInfo.absoluteFilePath(),
                 joinRemotePath(remoteBase, sourceFileInfo.fileName())});
        }
        if (!roots.isEmpty())
            startLocalUploadDiscovery(roots, false);
        if (roots.isEmpty() && skippedDirs > 0) {
            UiAlerts::information(
                this, tr("Upload"),
                tr("Transfer-only mode currently supports uploading files "
                   "only."));
        } else if (skippedDirs > 0) {
            statusBar()->showMessage(
                tr("Preparing uploads; folders skipped in transfer-only "
                   "mode: %1")
                    .arg(skippedDirs),
                5000);
        }
        return;
    }

    // ---- LOCAL→LOCAL branch: existing logic as-is ----
    const QString dstDirPath = rightPath_->path();
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
    const auto rows = selectionModel->selectedRows(kNameColumn);
    if (rows.isEmpty()) {
        UiAlerts::information(this, tr("Copy"),
                              tr("No entries selected in the left panel."));
        return;
    }

    runLocalFsSelection(rows, leftModel_, dstDir, false);
}

void MainWindow::moveLeftToRight() {
    if (rightIsRemote_) {
        if (!rightRemoteModel_ || !transferMgr_ ||
            !sessionController_->client()) {
            UiAlerts::warning(this, tr("Remote"),
                              tr("No active remote session."));
            return;
        }
        auto *selection = leftView_->selectionModel();
        const auto rows = selection ? selection->selectedRows(kNameColumn)
                                    : QModelIndexList{};
        if (rows.isEmpty()) {
            UiAlerts::information(this, tr("Move"),
                                  tr("No entries selected in the left panel."));
            return;
        }
        if (UiAlerts::question(this, tr("Confirm move"),
                               tr("This will upload to the server and "
                                  "delete the local source.\nContinue?")) !=
            QMessageBox::Yes) {
            return;
        }

        QVector<QPair<QString, QString>> roots;
        roots.reserve(rows.size());
        const QString remoteBase = rightRemoteModel_->rootPath();
        for (const QModelIndex &index : rows) {
            const QFileInfo source = leftModel_->fileInfo(index);
            if (!source.isFile() && !source.isDir())
                continue;
            roots.push_back({source.absoluteFilePath(),
                             joinRemotePath(remoteBase, source.fileName())});
        }
        startLocalUploadDiscovery(roots, true);
        return;
    }

    // ---- Existing LOCAL→LOCAL branch ----
    const QString dstDirPath = rightPath_->path();
    QDir dstDir(dstDirPath);
    if (!dstDir.exists()) {
        UiAlerts::warning(this, tr("Invalid destination"),
                          tr("Destination folder does not exist."));
        return;
    }
    const auto rows = leftView_->selectionModel()->selectedRows(kNameColumn);
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
    runLocalFsSelection(rows, leftModel_, dstDir, true);
}

void MainWindow::deleteFromLeft() {
    const auto rows = leftView_->selectionModel()->selectedRows(kNameColumn);
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
    statusBar()->showMessage(QString(tr("Deleted: %1  |  Failed: %2"))
                                 .arg(deletedCount)
                                 .arg(failedCount),
                             5000);
}

void MainWindow::goUpLeft() {
    QString currentPath = leftPath_->path();
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

void MainWindow::reportPathActionResult(
    const openscpui::PathActionResult &result) {
    if (result.failed())
        UiAlerts::warning(this, tr("Open location"), result.error);
}

void MainWindow::openLocalPathWithPreference(const QString &localPath) {
    if (localPath.isEmpty())
        return;

    const QString mode = prefOpenBehaviorMode_.trimmed().toLower();
    if (mode == QStringLiteral("reveal")) {
        reportPathActionResult(
            openscpui::PlatformPathActions::revealPath(localPath));
        return;
    }
    if (mode == QStringLiteral("open")) {
        reportPathActionResult(
            openscpui::PlatformPathActions::openFile(localPath));
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
    if (box.clickedButton() == btnOpen) {
        reportPathActionResult(
            openscpui::PlatformPathActions::openFile(localPath));
        return;
    }
    reportPathActionResult(
        openscpui::PlatformPathActions::revealPath(localPath));
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
    const QString newName = QInputDialog::getText(
        this, tr("Rename"), tr("New name:"), QLineEdit::Normal,
        selectedFileInfo.fileName(), &inputAccepted);
    if (!inputAccepted || newName.isEmpty() ||
        newName == selectedFileInfo.fileName())
        return;
    const QString newPath =
        QDir(selectedFileInfo.absolutePath()).filePath(newName);
    bool renamed = QFile::rename(selectedFileInfo.absoluteFilePath(), newPath);
    if (!renamed)
        renamed = QDir(selectedFileInfo.absolutePath())
                      .rename(selectedFileInfo.absoluteFilePath(), newPath);
    if (!renamed) {
        UiAlerts::critical(this, tr("Local"), tr("Could not rename."));
        return;
    }
    setLeftRoot(leftPath_->path());
}

// Create a new directory in the left (local) pane.
void MainWindow::newDirLeft() {
    QString name;
    if (!promptValidEntryName(this, tr("New folder"), tr("Name:"), {}, name))
        return;
    QDir base(leftPath_->path());
    if (!base.mkpath(base.filePath(name))) {
        UiAlerts::critical(this, tr("Local"), tr("Could not create folder."));
        return;
    }
    setLeftRoot(base.absolutePath());
}

// Create a new empty file in the left (local) pane.
void MainWindow::newFileLeft() {
    QString name;
    if (!promptValidEntryName(this, tr("New file"), tr("Name:"), {}, name))
        return;
    QDir base(leftPath_->path());
    const QString path = base.filePath(name);
    if (QFileInfo::exists(path)) {
        if (UiAlerts::question(this, tr("File exists"),
                               tr("«%1» already exists.\nOverwrite?").arg(name),
                               QMessageBox::Yes | QMessageBox::No) !=
            QMessageBox::Yes)
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
        hasSel = !selectionModel->selectedRows(kNameColumn).isEmpty();
    }
    QDir currentDir(leftPath_ ? leftPath_->path() : QString());
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
