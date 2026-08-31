// MainWindow remote-side operations and writeability state.
#include "AppSettings.hpp"
#include "MainWindow.hpp"
#include "MainWindowSharedUtils.hpp"
#include "PermissionsDialog.hpp"
#include "RemoteActionController.hpp"
#include "RemoteModel.hpp"
#include "RemoteOperationController.hpp"
#include "SessionController.hpp"
#include "TerminalCommandBuilder.hpp"
#include "TransferManager.hpp"
#include "UiAlerts.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QScrollBar>
#include <QSet>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTreeView>

namespace {

QString tempDownloadPathFor(const QString &remoteName) {
    QString base =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (base.isEmpty())
        base = QDir::homePath() + "/Downloads";
    QDir().mkpath(base);
    return QDir(base).filePath(remoteName);
}

} // namespace

// Reveal a file in the system file manager (select/highlight when possible),

void MainWindow::goUpRight() {
    if (rightIsRemote_) {
        QString cur = rightRemoteModel_
                          ? rightRemoteModel_->rootPath()
                          : (rightPath_ ? rightPath_->text() : QString());
        cur = normalizeRemotePath(cur);
        if (cur == "/" || cur.isEmpty())
            return;
        if (cur.endsWith('/'))
            cur.chop(1);
        const qsizetype slash = cur.lastIndexOf('/');
        QString parent = (slash <= 0) ? "/" : cur.left(slash);
        setRightRemoteRoot(parent);
    } else {
        QString cur = rightPath_->text();
        QDir currentDir(cur);
        if (!currentDir.cdUp())
            return;
        setRightRoot(currentDir.absolutePath());
        updateDeleteShortcutEnables();
    }
}

void MainWindow::goHomeRight() {
    if (rightIsRemote_) {
        // SFTP does not provide a portable remote HOME query; use root
        // fallback.
        setRightRemoteRoot(QStringLiteral("/"));
    } else {
        setRightRoot(preferredLocalHomePath());
        updateDeleteShortcutEnables();
    }
}

void MainWindow::openRightRemoteTerminal() {
    const auto &options = sessionController_->options();
    if (!rightIsRemote_ || !options.has_value()) {
        UiAlerts::information(
            this, tr("Open in terminal"),
            tr("The right panel must be connected as remote."));
        return;
    }

    const openscp::SessionOptions &sessionOptions = *options;
    const QString remotePath = normalizeRemotePath(
        rightRemoteModel_ ? rightRemoteModel_->rootPath()
                          : (rightPath_ ? rightPath_->text() : QString()));
    openscpui::AppSettings settings;
    const bool forceInteractiveLogin =
        settings
            .value(openscpui::settingskeys::kTerminalForceInteractiveLogin,
                   false)
            .toBool();
    const bool enableSftpCliFallback =
        settings
            .value(openscpui::settingskeys::kTerminalEnableSftpCliFallback,
                   true)
            .toBool();

    const openscpui::TerminalCommandBuilder terminalCommands;
    const openscpui::TerminalCommandResult command =
        terminalCommands.prepare(sessionOptions, remotePath,
                                 forceInteractiveLogin, enableSftpCliFallback);
    if (!command.isValid()) {
        UiAlerts::warning(this, tr("Open in terminal"),
                          tr("Could not prepare the terminal command.\n%1")
                              .arg(command.error.isEmpty()
                                       ? tr("Unknown error.")
                                       : command.error));
        return;
    }

    QString launchError;
    if (!terminalCommands.launch(command.command, &launchError)) {
        UiAlerts::warning(this, tr("Open in terminal"),
                          tr("Could not open a remote terminal.\n%1")
                              .arg(launchError.isEmpty() ? tr("Unknown error.")
                                                         : launchError));
        return;
    }

    const bool hasPrivateKey = sessionOptions.private_key_path.has_value() &&
                               !sessionOptions.private_key_path->empty();
    const bool hasSavedPassword = sessionOptions.password.has_value() &&
                                  !sessionOptions.password->empty() &&
                                  !hasPrivateKey;
    QString statusMessage = tr("Opening remote terminal at %1").arg(remotePath);
    if (forceInteractiveLogin) {
        statusMessage += tr(" (interactive login required)");
    } else if (hasSavedPassword) {
        statusMessage +=
            tr(" (password may be requested by OpenSSH for security)");
    }
    if (command.hasSftpFallback) {
        statusMessage += tr(" (auto-fallback to SFTP CLI enabled)");
    }
    statusBar()->showMessage(statusMessage, 6000);
}

void MainWindow::setRightRemoteRoot(const QString &path) {
    if (!rightIsRemote_)
        return;
    if (!rightRemoteModel_) {
        const QString normalized = normalizeRemotePath(path);
        rightPath_->setText(normalized);
        addRecentRemotePath(normalized);
        refreshRightBreadcrumbs();
        updateDeleteShortcutEnables();
        statusBar()->showMessage(tr("Remote path: %1").arg(normalized), 3000);
        return;
    }
    requestRemoteListing(path, false);
}

void MainWindow::refreshRightRemotePanel() {
    if (!rightIsRemote_)
        return;
    if (!rightRemoteModel_) {
        statusBar()->showMessage(
            tr("Refresh is not available in transfer-only mode."), 3000);
        return;
    }

    requestRemoteListing(rightRemoteModel_->rootPath(), true);
}

void MainWindow::requestRemoteListing(const QString &path, bool refresh,
                                      bool initialLoad) {
    if (!rightIsRemote_ || !rightRemoteModel_ || !remoteOps_ ||
        !remoteOps_->hasRequestedSession()) {
        statusBar()->showMessage(
            tr("Remote control connection is not available"), 4000);
        return;
    }

    const QString normalized = normalizeRemotePath(path);
    if (activeRemoteListJob_ != 0)
        remoteOps_->cancel(activeRemoteListJob_);

    remoteRefreshSelectionNames_.clear();
    remoteRefreshScrollValue_ = 0;
    if (refresh && rightView_->selectionModel()) {
        const QModelIndexList selected =
            rightView_->selectionModel()->selectedRows(kNameColumn);
        for (const QModelIndex &index : selected) {
            const QString name = rightRemoteModel_->nameAt(index);
            if (!name.isEmpty())
                remoteRefreshSelectionNames_.push_back(name);
        }
        if (rightView_->verticalScrollBar()) {
            remoteRefreshScrollValue_ =
                rightView_->verticalScrollBar()->value();
        }
    }

    requestedRemotePath_ = normalized;
    activeRemoteListIsRefresh_ = refresh;
    activeRemoteListIsInitial_ = initialLoad;
    if (initialLoad && normalized != QStringLiteral("/"))
        initialRemoteFallbackAttempted_ = false;
    if (!refresh)
        rightRemoteModel_->setLoading(normalized);
    rightPath_->setText(normalized);
    refreshRightBreadcrumbs();
    rightView_->setEnabled(false);
    statusBar()->showMessage(refresh ? tr("Refreshing remote folder…")
                                     : tr("Opening remote folder…"),
                             0);

    RemoteOperationController::ListRequest request;
    request.path = normalized;
    request.includeHidden = prefShowHidden_;
    activeRemoteListJob_ = remoteOps_->submit(request);
}

void MainWindow::rightItemActivated(const QModelIndex &idx) {
    // Local mode (right panel is local): navigate into directories
    if (!rightIsRemote_) {
        if (!rightLocalModel_)
            return;
        const QFileInfo localFileInfo = rightLocalModel_->fileInfo(idx);
        if (localFileInfo.isDir()) {
            setRightRoot(localFileInfo.absoluteFilePath());
        } else if (localFileInfo.isFile()) {
            openLocalPathWithPreference(localFileInfo.absoluteFilePath());
        }
        return;
    }
    // Remote mode: navigate or download/open file
    if (!rightRemoteModel_)
        return;
    if (rightRemoteModel_->isDir(idx)) {
        const QString name = rightRemoteModel_->nameAt(idx);
        const QString next =
            joinRemotePath(rightRemoteModel_->rootPath(), name);
        setRightRemoteRoot(next);
        return;
    }
    const QString name = rightRemoteModel_->nameAt(idx);
    {
        QString why;
        if (!isValidEntryName(name, &why)) {
            UiAlerts::warning(this, tr("Invalid name"), why);
            return;
        }
    }
    const QString remotePath =
        joinRemotePath(rightRemoteModel_->rootPath(), name);
    const QString localPath = tempDownloadPathFor(name);
    // Avoid duplicates: if there is already an active download with same
    // src/dst, do not enqueue again
    const bool alreadyActive = transferMgr_->hasActiveTaskForSource(
        TransferTask::Type::Download, remotePath);
    if (!alreadyActive) {
        // Enqueue download so it appears in the queue (instead of direct
        // download)
        transferMgr_->enqueueDownload(remotePath, localPath);
        statusBar()->showMessage(QString(tr("Queued: %1 downloads")).arg(1),
                                 3000);
        maybeShowTransferQueue();
    } else {
        // There was already an identical task in the queue; optionally show it
        maybeShowTransferQueue();
        statusBar()->showMessage(tr("Download already queued"), 2000);
    }
    // Open the file when the corresponding task finishes (avoid duplicate
    // listeners)
    static QSet<QString> sOpenListeners;
    const QString key = remotePath + "->" + localPath;
    if (!sOpenListeners.contains(key)) {
        sOpenListeners.insert(key);
        auto connPtr = std::make_shared<QMetaObject::Connection>();
        *connPtr = connect(
            transferMgr_, &TransferManager::tasksUpdated, this,
            [this, remotePath, localPath, key,
             connPtr](const QVector<quint64> &taskIds) {
                const auto tasks = transferMgr_->tasksSnapshot(taskIds);
                for (const auto &task : tasks) {
                    if (task.type == TransferTask::Type::Download &&
                        task.src == remotePath && task.dst == localPath) {
                        if (task.status == TransferTask::Status::Done) {
                            openLocalPathWithPreference(localPath);
                            statusBar()->showMessage(
                                tr("Downloaded: ") + localPath, 5000);
                            QObject::disconnect(*connPtr);
                            sOpenListeners.remove(key);
                        } else if (isTransferTaskFinalStatus(task.status)) {
                            QObject::disconnect(*connPtr);
                            sOpenListeners.remove(key);
                        }
                        break;
                    }
                }
            });
    }
}

void MainWindow::downloadRightToLeft() {
    if (!rightIsRemote_) {
        UiAlerts::information(this, tr("Download"),
                              tr("The right panel is not remote."));
        return;
    }
    if (!sessionController_->client()) {
        UiAlerts::warning(this, tr("Remote"), tr("No active remote session."));
        return;
    }
    const QString picked = QFileDialog::getExistingDirectory(
        this, tr("Select destination folder (local)"),
        downloadDir_.isEmpty() ? QDir::homePath() : downloadDir_);
    if (picked.isEmpty())
        return;
    downloadDir_ = picked;
    QDir dst(downloadDir_);
    if (!dst.exists()) {
        UiAlerts::warning(this, tr("Invalid destination"),
                          tr("Destination folder does not exist."));
        return;
    }
    if (isScpTransferMode()) {
        const QString baseRemote = normalizeRemotePath(
            rightPath_ ? rightPath_->text() : QStringLiteral("/"));
        bool inputAccepted = false;
        QString remoteInput = QInputDialog::getText(
            this, tr("Download"),
            tr("Remote file path (absolute or relative to %1):")
                .arg(baseRemote),
            QLineEdit::Normal, baseRemote, &inputAccepted);
        if (!inputAccepted)
            return;
        remoteInput = remoteInput.trimmed();
        if (remoteInput.isEmpty())
            return;
        QString remotePath = remoteInput;
        if (!remotePath.startsWith('/'))
            remotePath = joinRemotePath(baseRemote, remotePath);
        remotePath = normalizeRemotePath(remotePath);
        const QString name = QFileInfo(remotePath).fileName();
        if (name.isEmpty()) {
            UiAlerts::warning(this, tr("Download"),
                              tr("Enter a valid remote file path."));
            return;
        }
        transferMgr_->enqueueDownload(remotePath, dst.filePath(name));
        statusBar()->showMessage(QString(tr("Queued: %1 downloads")).arg(1),
                                 4000);
        maybeShowTransferQueue();
        const qsizetype slash = remotePath.lastIndexOf('/');
        const QString parent =
            (slash <= 0) ? QStringLiteral("/") : remotePath.left(slash);
        setRightRemoteRoot(parent);
        return;
    }
    if (!rightRemoteModel_) {
        UiAlerts::warning(this, tr("Remote"), tr("No active remote session."));
        return;
    }
    auto selectionModel = rightView_->selectionModel();
    QModelIndexList rows;
    if (selectionModel)
        rows = selectionModel->selectedRows(kNameColumn);
    if (rows.isEmpty()) {
        // Download everything visible (first level) if there is no selection
        int rowCount = rightRemoteModel_ ? rightRemoteModel_->rowCount() : 0;
        for (int rowIndex = 0; rowIndex < rowCount; ++rowIndex)
            rows << rightRemoteModel_->index(rowIndex, kNameColumn);
        if (rows.isEmpty()) {
            UiAlerts::information(this, tr("Download"),
                                  tr("Nothing to download."));
            return;
        }
    }
    int skippedInvalidCount = 0;
    QVector<RemoteDownloadSeed> seeds;
    seeds.reserve(rows.size());
    const QString remoteBase = rightRemoteModel_->rootPath();
    for (const QModelIndex &idx : rows) {
        const QString name = rightRemoteModel_->nameAt(idx);
        {
            QString why;
            if (!isValidEntryName(name, &why)) {
                ++skippedInvalidCount;
                continue;
            }
        }
        const QString remotePath = joinRemotePath(remoteBase, name);
        const QString localPath = dst.filePath(name);
        seeds.push_back({remotePath, localPath, rightRemoteModel_->isDir(idx)});
    }
    runRemoteDownloadPrescan(seeds, skippedInvalidCount, false);
}

// Copy the selection from the right panel to the left.
// - Remote -> enqueue downloads (non-blocking).
// - Local  -> local-to-local copy (with overwrite policy).
void MainWindow::copyRightToLeft() {
    QDir dst(leftPath_->text());
    if (!dst.exists()) {
        UiAlerts::warning(
            this, tr("Invalid destination"),
            tr("The destination folder (left panel) does not exist."));
        return;
    }
    auto selectionModel = rightView_->selectionModel();
    if (!selectionModel) {
        UiAlerts::warning(this, tr("Copy"), tr("No selection."));
        return;
    }
    const auto rows = selectionModel->selectedRows(kNameColumn);
    if (rows.isEmpty()) {
        UiAlerts::information(this, tr("Copy"), tr("Nothing selected."));
        return;
    }

    if (!rightIsRemote_) {
        runLocalFsSelection(rows, rightLocalModel_, dst, false);
        return;
    }

    // Remote -> Local: enqueue downloads
    if (!sessionController_->client() || !rightRemoteModel_) {
        UiAlerts::warning(this, tr("Remote"), tr("No active remote session."));
        return;
    }
    int skippedInvalidCount = 0;
    QVector<RemoteDownloadSeed> seeds;
    seeds.reserve(rows.size());
    const QString remoteBase = rightRemoteModel_->rootPath();
    for (const QModelIndex &idx : rows) {
        const QString name = rightRemoteModel_->nameAt(idx);
        {
            QString why;
            if (!isValidEntryName(name, &why)) {
                ++skippedInvalidCount;
                continue;
            }
        }
        const QString remotePath = joinRemotePath(remoteBase, name);
        const QString localPath = dst.filePath(name);
        seeds.push_back({remotePath, localPath, rightRemoteModel_->isDir(idx)});
    }
    runRemoteDownloadPrescan(seeds, skippedInvalidCount, false);
}

// Move the selection from the right panel to the left.
// - Remote -> download with progress and delete remotely on success.
// - Local  -> local copy and delete the source.
void MainWindow::moveRightToLeft() {
    auto selectionModel = rightView_->selectionModel();
    if (!selectionModel ||
        selectionModel->selectedRows(kNameColumn).isEmpty()) {
        UiAlerts::information(this, tr("Move"), tr("Nothing selected."));
        return;
    }
    QDir dst(leftPath_->text());
    if (!dst.exists()) {
        UiAlerts::warning(
            this, tr("Invalid destination"),
            tr("The destination folder (left panel) does not exist."));
        return;
    }

    const auto rows = selectionModel->selectedRows(kNameColumn);
    if (!rightIsRemote_) {
        runLocalFsSelection(rows, rightLocalModel_, dst, true);
        return;
    }
    if (!rightRemoteModel_) {
        UiAlerts::warning(this, tr("Remote"), tr("No active remote session."));
        return;
    }

    int skippedInvalidCount = 0;
    QVector<RemoteDownloadSeed> seeds;
    seeds.reserve(rows.size());
    const QString remoteBase = rightRemoteModel_->rootPath();
    for (const QModelIndex &index : rows) {
        const QString name = rightRemoteModel_->nameAt(index);
        QString why;
        if (!isValidEntryName(name, &why)) {
            ++skippedInvalidCount;
            continue;
        }
        seeds.push_back({joinRemotePath(remoteBase, name), dst.filePath(name),
                         rightRemoteModel_->isDir(index)});
    }
    runRemoteDownloadPrescan(seeds, skippedInvalidCount, false, true);
}

void MainWindow::uploadViaDialog() {
    if (!rightIsRemote_ || !sessionController_->client()) {
        UiAlerts::information(
            this, tr("Upload"),
            tr("The right panel is not remote or there is no active session."));
        return;
    }
    const bool scpMode = isScpTransferMode();
    if (!scpMode && !rightRemoteModel_) {
        UiAlerts::warning(this, tr("Remote"), tr("No active remote session."));
        return;
    }
    const QString startDir =
        uploadDir_.isEmpty() ? QDir::homePath() : uploadDir_;
    if (scpMode) {
        const QStringList picks = QFileDialog::getOpenFileNames(
            this, tr("Select files to upload"), startDir);
        if (picks.isEmpty())
            return;
        uploadDir_ = QFileInfo(picks.first()).dir().absolutePath();
        const QString remoteBase = normalizeRemotePath(
            rightPath_ ? rightPath_->text() : QStringLiteral("/"));
        int enqueuedCount = 0;
        for (const QString &localPath : picks) {
            const QFileInfo localFileInfo(localPath);
            if (!localFileInfo.isFile())
                continue;
            transferMgr_->enqueueUpload(
                localFileInfo.absoluteFilePath(),
                joinRemotePath(remoteBase, localFileInfo.fileName()));
            ++enqueuedCount;
        }
        if (enqueuedCount > 0) {
            statusBar()->showMessage(
                QString(tr("Queued: %1 uploads")).arg(enqueuedCount), 4000);
            maybeShowTransferQueue();
        }
        return;
    }
    QFileDialog dlg(this, tr("Select files or folders to upload"), startDir);
    dlg.setFileMode(QFileDialog::ExistingFiles);
    dlg.setOption(QFileDialog::DontUseNativeDialog, true);
    dlg.setViewMode(QFileDialog::Detail);
    if (auto *listView = dlg.findChild<QListView *>("listView"))
        listView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    if (auto *treeView = dlg.findChild<QTreeView *>())
        treeView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QStringList picks = dlg.selectedFiles();
    if (picks.isEmpty())
        return;
    uploadDir_ = QFileInfo(picks.first()).dir().absolutePath();
    const QString remoteBase = rightRemoteModel_->rootPath();
    QVector<QPair<QString, QString>> roots;
    roots.reserve(picks.size());
    for (const QString &pickedPath : picks) {
        const QFileInfo selectedPathInfo(pickedPath);
        if (!selectedPathInfo.isDir() && !selectedPathInfo.isFile())
            continue;
        roots.push_back(
            {selectedPathInfo.absoluteFilePath(),
             joinRemotePath(remoteBase, selectedPathInfo.fileName())});
    }
    if (roots.isEmpty()) {
        statusBar()->showMessage(tr("Nothing to upload."), 4000);
        return;
    }
    startLocalUploadDiscovery(roots, false);
}

void MainWindow::newDirRight() {
    QString name;
    if (!promptValidEntryName(this, tr("New folder"), tr("Name:"), {}, name))
        return;
    if (rightIsRemote_) {
        if (!remoteActionController_ || !rightRemoteModel_)
            return;
        remoteActionController_->createDirectory(rightRemoteModel_->rootPath(),
                                                 name);
    } else {
        QDir base(rightPath_->text());
        if (!base.mkpath(base.filePath(name))) {
            UiAlerts::critical(this, tr("Local"),
                               tr("Could not create folder."));
            return;
        }
        setRightRoot(base.absolutePath());
    }
}

// Create a new empty file in the right pane (local only).
void MainWindow::newFileRight() {
    QString name;
    if (!promptValidEntryName(this, tr("New file"), tr("Name:"), {}, name))
        return;
    if (rightIsRemote_) {
        if (!remoteActionController_ || !rightRemoteModel_)
            return;
        remoteActionController_->createFile(rightRemoteModel_->rootPath(),
                                            name);
    } else {
        QDir base(rightPath_->text());
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
        setRightRoot(base.absolutePath());
        statusBar()->showMessage(tr("File created: ") + path, 4000);
    }
}

// Rename the selected entry on the right pane (local or remote).
void MainWindow::renameRightSelected() {
    auto selectionModel = rightView_->selectionModel();
    if (!selectionModel)
        return;
    const auto rows = selectionModel->selectedRows();
    if (rows.size() != 1) {
        UiAlerts::information(this, tr("Rename"),
                              tr("Select exactly one item."));
        return;
    }
    if (rightIsRemote_) {
        if (!remoteActionController_ || !rightRemoteModel_)
            return;
        const QModelIndex selectedIndex = rows.first();
        const QString oldName = rightRemoteModel_->nameAt(selectedIndex);
        bool inputAccepted = false;
        const QString newName =
            QInputDialog::getText(this, tr("Rename"), tr("New name:"),
                                  QLineEdit::Normal, oldName, &inputAccepted);
        if (!inputAccepted || newName.isEmpty() || newName == oldName)
            return;
        QString invalidReason;
        if (!isValidEntryName(newName, &invalidReason)) {
            UiAlerts::warning(this, tr("Invalid name"), invalidReason);
            return;
        }
        remoteActionController_->rename(rightRemoteModel_->rootPath(), oldName,
                                        newName);
    } else {
        const QModelIndex selectedIndex = rows.first();
        const QFileInfo selectedFileInfo =
            rightLocalModel_->fileInfo(selectedIndex);
        bool inputAccepted = false;
        const QString newName = QInputDialog::getText(
            this, tr("Rename"), tr("New name:"), QLineEdit::Normal,
            selectedFileInfo.fileName(), &inputAccepted);
        if (!inputAccepted || newName.isEmpty() ||
            newName == selectedFileInfo.fileName())
            return;
        const QString newPath =
            QDir(selectedFileInfo.absolutePath()).filePath(newName);
        bool renamed =
            QFile::rename(selectedFileInfo.absoluteFilePath(), newPath);
        if (!renamed)
            renamed = QDir(selectedFileInfo.absolutePath())
                          .rename(selectedFileInfo.absoluteFilePath(), newPath);
        if (!renamed) {
            UiAlerts::critical(this, tr("Local"), tr("Could not rename."));
            return;
        }
        setRightRoot(rightPath_->text());
    }
}

void MainWindow::deleteRightSelected() {
    auto *selectionModel = rightView_->selectionModel();
    if (!selectionModel)
        return;
    const auto rows = selectionModel->selectedRows();
    if (rows.isEmpty()) {
        UiAlerts::information(this, tr("Delete"), tr("Nothing selected."));
        return;
    }
    if (rightIsRemote_) {
        if (!remoteActionController_ || !rightRemoteModel_)
            return;
        QVector<openscpui::RemoteActionController::Entry> entries;
        entries.reserve(rows.size());
        for (const QModelIndex &index : rows) {
            entries.push_back({rightRemoteModel_->nameAt(index),
                               rightRemoteModel_->isDir(index)});
        }
        remoteActionController_->remove(rightRemoteModel_->rootPath(), entries);
        return;
    }

    if (UiAlerts::warning(
            this, tr("Confirm delete"),
            tr("This will permanently delete on local disk.\nContinue?"),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }
    int deletedCount = 0;
    int failedCount = 0;
    for (const QModelIndex &index : rows) {
        const QFileInfo selectedFileInfo = rightLocalModel_->fileInfo(index);
        const bool removed =
            selectedFileInfo.isDir()
                ? QDir(selectedFileInfo.absoluteFilePath()).removeRecursively()
                : QFile::remove(selectedFileInfo.absoluteFilePath());
        removed ? ++deletedCount : ++failedCount;
    }
    statusBar()->showMessage(
        tr("Deleted: %1  |  Failed: %2").arg(deletedCount).arg(failedCount),
        5000);
    setRightRoot(rightPath_->text());
}

// Show context menu for the right pane based on current state.
void MainWindow::showRightContextMenu(const QPoint &pos) {
    if (!rightContextMenu_)
        rightContextMenu_ = new QMenu(this);

    // Selection state and ability to go up
    bool hasSel = false;
    if (auto selectionModel = rightView_->selectionModel()) {
        hasSel = !selectionModel->selectedRows(kNameColumn).isEmpty();
    }
    // Is there a parent directory?
    bool canGoUp = false;
    if (rightIsRemote_) {
        const QString cur = normalizeRemotePath(
            rightRemoteModel_ ? rightRemoteModel_->rootPath()
                              : (rightPath_ ? rightPath_->text() : QString()));
        canGoUp = (!cur.isEmpty() && cur != "/");
    } else {
        QDir currentDir(rightPath_ ? rightPath_->text() : QString());
        canGoUp = currentDir.cdUp();
    }

    if (isScpTransferMode()) {
        QVector<QAction *> entries;
        if (canGoUp)
            entries.push_back(actUpRight_);
        entries.push_back(actUploadRight_);
        entries.push_back(actDownloadF7_);
        entries.push_back(actOpenTerminalRight_);
        rebuildContextMenu(rightContextMenu_, entries);
        rightContextMenu_->popup(rightView_->viewport()->mapToGlobal(pos));
        return;
    }

    QVector<QAction *> entries;
    if (rightIsRemote_) {
        const auto &options = sessionController_->options();
        const bool supportsRemotePermissions =
            options.has_value() &&
            openscp::capabilitiesForProtocol(options->protocol)
                .can_set_permissions;
        // Up option (if applicable)
        if (canGoUp)
            entries.push_back(actUpRight_);

        // Always show "Download" on remote, regardless of selection
        entries.push_back(actDownloadF7_);

        if (!hasSel) {
            // No selection: creation and navigation
            if (rightRemoteMutationsSupported_) {
                entries.push_back(actNewFileRight_);
                entries.push_back(actNewDirRight_);
            }
        } else {
            // With selection on remote
            entries.push_back(actCopyRight_);
            if (rightRemoteMutationsSupported_) {
                entries.push_back(nullptr);
                entries.push_back(actUploadRight_);
                entries.push_back(actNewFileRight_);
                entries.push_back(actNewDirRight_);
                entries.push_back(actRenameRight_);
                entries.push_back(actDeleteRight_);
                entries.push_back(actMoveRight_);
                if (supportsRemotePermissions) {
                    entries.push_back(nullptr);
                    auto *changePerms = new QAction(tr("Change permissions…"),
                                                    rightContextMenu_);
                    connect(changePerms, &QAction::triggered, this,
                            &MainWindow::changeRemotePermissions);
                    entries.push_back(changePerms);
                }
            }
        }
    } else {
        // Local: Up option if applicable
        if (canGoUp)
            entries.push_back(actUpRight_);
        if (!hasSel) {
            // No selection: creation
            entries.push_back(actNewFileRight_);
            entries.push_back(actNewDirRight_);
        } else {
            // With selection: local operations + copy/move from left
            entries.push_back(actNewFileRight_);
            entries.push_back(actNewDirRight_);
            entries.push_back(actRenameRight_);
            entries.push_back(actDeleteRight_);
            entries.push_back(nullptr);
            // Copy/move the selection from the right panel to the left
            entries.push_back(actCopyRight_);
            entries.push_back(actMoveRight_);
        }
    }
    rebuildContextMenu(rightContextMenu_, entries);
    rightContextMenu_->popup(rightView_->viewport()->mapToGlobal(pos));
}

void MainWindow::changeRemotePermissions() {
    if (!rightIsRemote_ || !remoteActionController_ || !rightRemoteModel_) {
        return;
    }
    const auto &options = sessionController_->options();
    const auto capabilities =
        options.has_value()
            ? openscp::capabilitiesForProtocol(options->protocol)
            : openscp::ProtocolCapabilities{};
    if (!capabilities.can_set_permissions) {
        UiAlerts::information(
            this, tr("Permissions"),
            tr("Permissions are not supported for the active protocol."));
        return;
    }
    auto *selectionModel = rightView_->selectionModel();
    if (!selectionModel)
        return;
    const auto rows = selectionModel->selectedRows();
    if (rows.size() != 1) {
        UiAlerts::information(this, tr("Permissions"),
                              tr("Select only one item."));
        return;
    }
    const QModelIndex index = rows.first();
    remoteActionController_->changePermissions(
        rightRemoteModel_->rootPath(),
        {rightRemoteModel_->nameAt(index), rightRemoteModel_->isDir(index)});
}

void MainWindow::applyRemoteMutationActions() {
    const openscp::ProtocolCapabilities caps =
        sessionController_->client()
            ? sessionController_->client()->capabilities()
            : openscp::ProtocolCapabilities{};
    const auto availability = openscpui::RemoteActionController::availability(
        caps, rightRemoteMutationsSupported_);
    if (actUploadRight_)
        actUploadRight_->setEnabled(availability.canUpload);
    if (actNewDirRight_)
        actNewDirRight_->setEnabled(availability.canCreateDirectory);
    if (actNewFileRight_)
        actNewFileRight_->setEnabled(availability.canCreateFile);
    if (actRenameRight_)
        actRenameRight_->setEnabled(availability.canRename);
    if (actDeleteRight_)
        actDeleteRight_->setEnabled(availability.canDelete);
    if (actMoveRight_)
        actMoveRight_->setEnabled(availability.canMoveToLocal);
    if (actMoveRightTb_)
        actMoveRightTb_->setEnabled(availability.canMoveToLocal);
    updateDeleteShortcutEnables();
}

// Enable operations from protocol capabilities. Permissions are deliberately
// checked by the real operation; probing with a temporary directory mutates
// the server and can be both slow and misleading.
void MainWindow::updateRemoteMutationCapability() {
    if (!rightIsRemote_ || !sessionController_->client() ||
        !rightRemoteModel_) {
        rightRemoteMutationsSupported_ = false;
        applyRemoteMutationActions();
        return;
    }
    const openscp::ProtocolCapabilities caps =
        sessionController_->client()->capabilities();
    rightRemoteMutationsSupported_ =
        openscpui::RemoteActionController::availability(caps).canMutate;
    applyRemoteMutationActions();
}
