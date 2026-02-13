// MainWindow remote-side operations, SFTP actions, and writeability state.
#include "MainWindow.hpp"
#include "PermissionsDialog.hpp"
#include "RemoteModel.hpp"
#include "TransferManager.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QSet>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryFile>
#include <QTreeView>

#include <functional>
#include <string>
#include <vector>

static constexpr int NAME_COL = 0;

static QString tempDownloadPathFor(const QString &remoteName) {
    QString base =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (base.isEmpty())
        base = QDir::homePath() + "/Downloads";
    QDir().mkpath(base);
    return QDir(base).filePath(remoteName);
}

// Reveal a file in the system file manager (select/highlight when possible),

static bool isValidEntryName(const QString &name, QString *why = nullptr) {
    if (name == "." || name == "..") {
        if (why)
            *why = QCoreApplication::translate(
                "MainWindow", "Invalid name: cannot be '.' or '..'.");
        return false;
    }
    if (name.contains('/') || name.contains('\\')) {
        if (why)
            *why = QCoreApplication::translate(
                "MainWindow",
                "Invalid name: cannot contain separators ('/' or '\\\\').");
        return false;
    }
    for (const QChar &ch : name) {
        ushort u = ch.unicode();
        if (u < 0x20u || u == 0x7Fu) { // ASCII control characters
            if (why)
                *why = QCoreApplication::translate(
                    "MainWindow",
                    "Invalid name: cannot contain control characters.");
            return false;
        }
    }
    return true;
}

static QString shortRemoteError(const QString &raw, const QString &fallback) {
    QString msg = raw.trimmed();
    if (msg.isEmpty())
        return fallback;

    const QString lower = msg.toLower();
    if (lower.contains("permission denied")) {
        return QCoreApplication::translate("MainWindow", "Permission denied.");
    }
    if (lower.contains("read-only")) {
        return QCoreApplication::translate("MainWindow",
                                           "Location is read-only.");
    }
    if (lower.contains("no such file") || lower.contains("not found")) {
        return QCoreApplication::translate("MainWindow",
                                           "File or folder does not exist.");
    }
    if (lower.contains("timed out") || lower.contains("timeout")) {
        return QCoreApplication::translate("MainWindow",
                                           "Connection timed out.");
    }
    if (lower.contains("could not resolve") ||
        lower.contains("name or service not known") ||
        lower.contains("nodename nor servname")) {
        return QCoreApplication::translate(
            "MainWindow", "Could not resolve the server hostname.");
    }
    if (lower.contains("connection refused")) {
        return QCoreApplication::translate("MainWindow",
                                           "Connection refused by the server.");
    }
    if (lower.contains("network is unreachable") ||
        lower.contains("host is unreachable")) {
        return QCoreApplication::translate(
            "MainWindow", "Network unavailable or host unreachable.");
    }
    if (lower.contains("authentication failed") ||
        lower.contains("auth fail")) {
        return QCoreApplication::translate("MainWindow",
                                           "Authentication failed.");
    }

    const int nl = msg.indexOf('\n');
    if (nl > 0)
        msg = msg.left(nl);
    msg = msg.simplified();
    if (msg.size() > 96)
        msg = msg.left(93) + "...";
    return msg;
}

static QString shortRemoteError(const std::string &raw,
                                const QString &fallback) {
    return shortRemoteError(QString::fromStdString(raw), fallback);
}

static bool indicatesRemoteWriteabilityDenied(const QString &raw) {
    const QString lower = raw.trimmed().toLower();
    if (lower.isEmpty())
        return false;
    return lower.contains("permission denied") ||
           lower.contains("read-only") ||
           lower.contains("operation not permitted") ||
           lower.contains("access denied") ||
           lower.contains("sftp protocol error 3");
}

static QString joinRemotePath(const QString &base, const QString &name) {
    if (base == "/")
        return "/" + name;
    return base.endsWith('/') ? base + name : base + "/" + name;
}

void MainWindow::goUpRight() {
    if (rightIsRemote_) {
        if (!rightRemoteModel_)
            return;
        QString cur = rightRemoteModel_->rootPath();
        if (cur == "/" || cur.isEmpty())
            return;
        if (cur.endsWith('/'))
            cur.chop(1);
        int slash = cur.lastIndexOf('/');
        QString parent = (slash <= 0) ? "/" : cur.left(slash);
        setRightRemoteRoot(parent);
    } else {
        QString cur = rightPath_->text();
        QDir d(cur);
        if (!d.cdUp())
            return;
        setRightRoot(d.absolutePath());
        updateDeleteShortcutEnables();
    }
}

void MainWindow::setRightRemoteRoot(const QString &path) {
    if (!rightIsRemote_ || !rightRemoteModel_)
        return;
    QString e;
    if (!rightRemoteModel_->setRootPath(path, &e)) {
        QMessageBox::warning(
            this, tr("Remote error"),
            tr("Could not open the remote folder.\n%1")
                .arg(shortRemoteError(e,
                                      tr("Failed to read remote contents."))));
        return;
    }
}

void MainWindow::rightItemActivated(const QModelIndex &idx) {
    // Local mode (right panel is local): navigate into directories
    if (!rightIsRemote_) {
        if (!rightLocalModel_)
            return;
        const QFileInfo fi = rightLocalModel_->fileInfo(idx);
        if (fi.isDir()) {
            setRightRoot(fi.absoluteFilePath());
        } else if (fi.isFile()) {
            openLocalPathWithPreference(fi.absoluteFilePath());
        }
        return;
    }
    // Remote mode: navigate or download/open file
    if (!rightRemoteModel_)
        return;
    if (rightRemoteModel_->isDir(idx)) {
        const QString name = rightRemoteModel_->nameAt(idx);
        QString next = rightRemoteModel_->rootPath();
        if (!next.endsWith('/'))
            next += '/';
        next += name;
        setRightRemoteRoot(next);
        return;
    }
    const QString name = rightRemoteModel_->nameAt(idx);
    {
        QString why;
        if (!isValidEntryName(name, &why)) {
            QMessageBox::warning(this, tr("Invalid name"), why);
            return;
        }
    }
    QString remotePath = rightRemoteModel_->rootPath();
    if (!remotePath.endsWith('/'))
        remotePath += '/';
    remotePath += name;
    const QString localPath = tempDownloadPathFor(name);
    // Avoid duplicates: if there is already an active download with same
    // src/dst, do not enqueue again
    bool alreadyActive = false;
    {
        const auto tasks = transferMgr_->tasksSnapshot();
        for (const auto &t : tasks) {
            if (t.type == TransferTask::Type::Download && t.src == remotePath &&
                t.dst == localPath) {
                if (t.status == TransferTask::Status::Queued ||
                    t.status == TransferTask::Status::Running ||
                    t.status == TransferTask::Status::Paused) {
                    alreadyActive = true;
                    break;
                }
            }
        }
    }
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
            transferMgr_, &TransferManager::tasksChanged, this,
            [this, remotePath, localPath, key, connPtr]() {
                const auto tasks = transferMgr_->tasksSnapshot();
                for (const auto &t : tasks) {
                    if (t.type == TransferTask::Type::Download &&
                        t.src == remotePath && t.dst == localPath) {
                        if (t.status == TransferTask::Status::Done) {
                            openLocalPathWithPreference(localPath);
                            statusBar()->showMessage(
                                tr("Downloaded: ") + localPath, 5000);
                            QObject::disconnect(*connPtr);
                            sOpenListeners.remove(key);
                        } else if (t.status == TransferTask::Status::Error ||
                                   t.status == TransferTask::Status::Canceled) {
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
        QMessageBox::information(this, tr("Download"),
                                 tr("The right panel is not remote."));
        return;
    }
    if (!sftp_ || !rightRemoteModel_) {
        QMessageBox::warning(this, tr("SFTP"), tr("No active SFTP session."));
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
        QMessageBox::warning(this, tr("Invalid destination"),
                             tr("Destination folder does not exist."));
        return;
    }
    auto sel = rightView_->selectionModel();
    QModelIndexList rows;
    if (sel)
        rows = sel->selectedRows(NAME_COL);
    if (rows.isEmpty()) {
        // Download everything visible (first level) if there is no selection
        int rc = rightRemoteModel_ ? rightRemoteModel_->rowCount() : 0;
        for (int r = 0; r < rc; ++r)
            rows << rightRemoteModel_->index(r, NAME_COL);
        if (rows.isEmpty()) {
            QMessageBox::information(this, tr("Download"),
                                     tr("Nothing to download."));
            return;
        }
    }
    int enq = 0;
    int bad = 0;
    const QString remoteBase = rightRemoteModel_->rootPath();
    for (const QModelIndex &idx : rows) {
        const QString name = rightRemoteModel_->nameAt(idx);
        {
            QString why;
            if (!isValidEntryName(name, &why)) {
                ++bad;
                continue;
            }
        }
        QString rpath = remoteBase;
        if (!rpath.endsWith('/'))
            rpath += '/';
        rpath += name;
        const QString lpath = dst.filePath(name);
        if (rightRemoteModel_->isDir(idx)) {
            QVector<QPair<QString, QString>> stack;
            stack.push_back({rpath, lpath});
            while (!stack.isEmpty()) {
                auto pair = stack.back();
                stack.pop_back();
                const QString curR = pair.first;
                const QString curL = pair.second;
                QDir().mkpath(curL);
                std::vector<openscp::FileInfo> out;
                std::string lerr;
                if (!sftp_->list(curR.toStdString(), out, lerr))
                    continue;
                for (const auto &e : out) {
                    const QString ename = QString::fromStdString(e.name);
                    QString why;
                    if (!isValidEntryName(ename, &why)) {
                        ++bad;
                        continue;
                    }
                    const QString childR =
                        (curR.endsWith('/') ? curR + ename
                                            : curR + "/" + ename);
                    const QString childL = QDir(curL).filePath(ename);
                    if (e.is_dir)
                        stack.push_back({childR, childL});
                    else {
                        transferMgr_->enqueueDownload(childR, childL);
                        ++enq;
                    }
                }
            }
        } else {
            transferMgr_->enqueueDownload(rpath, lpath);
            ++enq;
        }
    }
    if (enq > 0) {
        QString msg = QString(tr("Queued: %1 downloads")).arg(enq);
        if (bad > 0)
            msg += QString("  |  ") + tr("Skipped invalid: %1").arg(bad);
        statusBar()->showMessage(msg, 4000);
        maybeShowTransferQueue();
    }
}

// Copy the selection from the right panel to the left.
// - Remote -> enqueue downloads (non-blocking).
// - Local  -> local-to-local copy (with overwrite policy).
void MainWindow::copyRightToLeft() {
    QDir dst(leftPath_->text());
    if (!dst.exists()) {
        QMessageBox::warning(
            this, tr("Invalid destination"),
            tr("The destination folder (left panel) does not exist."));
        return;
    }
    auto sel = rightView_->selectionModel();
    if (!sel) {
        QMessageBox::warning(this, tr("Copy"), tr("No selection."));
        return;
    }
    const auto rows = sel->selectedRows(NAME_COL);
    if (rows.isEmpty()) {
        QMessageBox::information(this, tr("Copy"), tr("Nothing selected."));
        return;
    }

    if (!rightIsRemote_) {
        // Local -> Local copy (right to left)
        enum class OverwritePolicy { Ask, OverwriteAll, SkipAll };
        OverwritePolicy policy = OverwritePolicy::Ask;
        int skipped = 0;
        QVector<LocalFsPair> pairs;
        for (const QModelIndex &idx : rows) {
            const QFileInfo fi = rightLocalModel_->fileInfo(idx);
            const QString target = dst.filePath(fi.fileName());
            if (QFileInfo::exists(target)) {
                if (policy == OverwritePolicy::Ask) {
                    auto ret = QMessageBox::question(
                        this, tr("Conflict"),
                        QString(tr("“%1” already exists at "
                                   "destination.\nOverwrite?"))
                            .arg(fi.fileName()),
                        QMessageBox::Yes | QMessageBox::No |
                            QMessageBox::YesToAll | QMessageBox::NoToAll);
                    if (ret == QMessageBox::YesToAll)
                        policy = OverwritePolicy::OverwriteAll;
                    else if (ret == QMessageBox::NoToAll)
                        policy = OverwritePolicy::SkipAll;
                    if (ret == QMessageBox::No ||
                        policy == OverwritePolicy::SkipAll) {
                        ++skipped;
                        continue;
                    }
                }
                QFileInfo tfi(target);
                if (tfi.isDir())
                    QDir(target).removeRecursively();
                else
                    QFile::remove(target);
            }
            pairs.push_back({fi.absoluteFilePath(), target});
        }
        runLocalFsOperation(pairs, false, skipped);
        return;
    }

    // Remote -> Local: enqueue downloads
    if (!sftp_ || !rightRemoteModel_) {
        QMessageBox::warning(this, tr("SFTP"), tr("No active SFTP session."));
        return;
    }
    int bad = 0;
    QVector<RemoteDownloadSeed> seeds;
    seeds.reserve(rows.size());
    const QString remoteBase = rightRemoteModel_->rootPath();
    for (const QModelIndex &idx : rows) {
        const QString name = rightRemoteModel_->nameAt(idx);
        {
            QString why;
            if (!isValidEntryName(name, &why)) {
                ++bad;
                continue;
            }
        }
        QString rpath = remoteBase;
        if (!rpath.endsWith('/'))
            rpath += '/';
        rpath += name;
        const QString lpath = dst.filePath(name);
        seeds.push_back({rpath, lpath, rightRemoteModel_->isDir(idx)});
    }
    runRemoteDownloadPrescan(seeds, bad, false);
}

// Move the selection from the right panel to the left.
// - Remote -> download with progress and delete remotely on success.
// - Local  -> local copy and delete the source.
void MainWindow::moveRightToLeft() {
    auto sel = rightView_->selectionModel();
    if (!sel || sel->selectedRows(NAME_COL).isEmpty()) {
        QMessageBox::information(this, tr("Move"), tr("Nothing selected."));
        return;
    }
    QDir dst(leftPath_->text());
    if (!dst.exists()) {
        QMessageBox::warning(
            this, tr("Invalid destination"),
            tr("The destination folder (left panel) does not exist."));
        return;
    }

    if (!rightIsRemote_) {
        // Local -> Local: move (copy then delete)
        enum class OverwritePolicy { Ask, OverwriteAll, SkipAll };
        OverwritePolicy policy = OverwritePolicy::Ask;
        int skipped = 0;
        QVector<LocalFsPair> pairs;
        const auto rows = sel->selectedRows(NAME_COL);
        for (const QModelIndex &idx : rows) {
            const QFileInfo fi = rightLocalModel_->fileInfo(idx);
            const QString target = dst.filePath(fi.fileName());
            if (QFileInfo::exists(target)) {
                if (policy == OverwritePolicy::Ask) {
                    auto ret = QMessageBox::question(
                        this, tr("Conflict"),
                        QString(tr("“%1” already exists at "
                                   "destination.\nOverwrite?"))
                            .arg(fi.fileName()),
                        QMessageBox::Yes | QMessageBox::No |
                            QMessageBox::YesToAll | QMessageBox::NoToAll);
                    if (ret == QMessageBox::YesToAll)
                        policy = OverwritePolicy::OverwriteAll;
                    else if (ret == QMessageBox::NoToAll)
                        policy = OverwritePolicy::SkipAll;
                    if (ret == QMessageBox::No ||
                        policy == OverwritePolicy::SkipAll) {
                        ++skipped;
                        continue;
                    }
                }
                QFileInfo tfi(target);
                if (tfi.isDir())
                    QDir(target).removeRecursively();
                else
                    QFile::remove(target);
            }
            pairs.push_back({fi.absoluteFilePath(), target});
        }
        runLocalFsOperation(pairs, true, skipped);
        return;
    }

    // Remote -> Local: enqueue downloads and delete remote on completion
    if (!sftp_ || !rightRemoteModel_) {
        QMessageBox::warning(this, tr("SFTP"), tr("No active SFTP session."));
        return;
    }
    const auto rows = sel->selectedRows(NAME_COL);
    const QString remoteBase = rightRemoteModel_->rootPath();
    QVector<QPair<QString, QString>> pairs; // (remote, local) files to download
    int bad = 0;
    struct TopSel {
        QString rpath;
        bool isDir;
    };
    QVector<TopSel> top;
    int enq = 0;
    for (const QModelIndex &idx : rows) {
        const QString name = rightRemoteModel_->nameAt(idx);
        {
            QString why;
            if (!isValidEntryName(name, &why)) {
                ++bad;
                continue;
            }
        }
        QString rpath = remoteBase;
        if (!rpath.endsWith('/'))
            rpath += '/';
        rpath += name;
        const QString lpath = dst.filePath(name);
        const bool isDir = rightRemoteModel_->isDir(idx);
        top.push_back({rpath, isDir});
        if (isDir) {
            QVector<QPair<QString, QString>> stack;
            stack.push_back({rpath, lpath});
            while (!stack.isEmpty()) {
                auto pair = stack.back();
                stack.pop_back();
                const QString curR = pair.first;
                const QString curL = pair.second;
                QDir().mkpath(curL);
                std::vector<openscp::FileInfo> out;
                std::string lerr;
                if (!sftp_->list(curR.toStdString(), out, lerr))
                    continue;
                for (const auto &e : out) {
                    const QString ename = QString::fromStdString(e.name);
                    QString why;
                    if (!isValidEntryName(ename, &why)) {
                        ++bad;
                        continue;
                    }
                    const QString childR =
                        (curR.endsWith('/') ? curR + ename
                                            : curR + "/" + ename);
                    const QString childL = QDir(curL).filePath(ename);
                    if (e.is_dir) {
                        stack.push_back({childR, childL});
                    } else {
                        QDir().mkpath(QFileInfo(childL).dir().absolutePath());
                        pairs.push_back({childR, childL});
                    }
                }
            }
        } else {
            QDir().mkpath(QFileInfo(lpath).dir().absolutePath());
            pairs.push_back({rpath, lpath});
        }
    }
    for (const auto &p : pairs) {
        transferMgr_->enqueueDownload(p.first, p.second);
        ++enq;
    }
    if (enq > 0) {
        QString msg = QString(tr("Queued: %1 downloads (move)")).arg(enq);
        if (bad > 0)
            msg += QString("  |  ") + tr("Skipped invalid: %1").arg(bad);
        statusBar()->showMessage(msg, 4000);
        maybeShowTransferQueue();
    }
    // Per-item deletion: as each download finishes OK, delete that remote file;
    // when a folder has no pending files left, delete the folder.
    if (enq > 0) {
        struct MoveState {
            QSet<QString> filesPending;   // remote files pending deletion
            QSet<QString> filesProcessed; // remote files already processed
                                          // (avoid duplicates)
            QHash<QString, QString>
                fileToTopDir; // remote file -> top dir rpath
            QHash<QString, int> remainingInTopDir; // top dir -> count of
                                                   // pending successful files
            QSet<QString> topDirs; // rpaths of top entries that are directories
            QSet<QString> deletedDirs; // top dirs already deleted
        };
        auto state = std::make_shared<MoveState>();
        // Initialize top dir mapping and counters
        for (const auto &tsel : top)
            if (tsel.isDir) {
                state->topDirs.insert(tsel.rpath);
                state->remainingInTopDir.insert(tsel.rpath, 0);
            }
        for (const auto &pr : pairs) {
            state->filesPending.insert(pr.first);
            // Locate containing top directory
            QString foundTop;
            for (const auto &tsel : top) {
                if (!tsel.isDir)
                    continue;
                const QString prefix =
                    tsel.rpath.endsWith('/') ? tsel.rpath : (tsel.rpath + '/');
                if (pr.first == tsel.rpath || pr.first.startsWith(prefix)) {
                    foundTop = tsel.rpath;
                    break;
                }
            }
            if (!foundTop.isEmpty()) {
                state->fileToTopDir.insert(pr.first, foundTop);
                state->remainingInTopDir[foundTop] =
                    state->remainingInTopDir.value(foundTop) + 1;
            }
        }
        // If there are directories with 0 files, try to delete them only if
        // empty
        for (auto it = state->remainingInTopDir.begin();
             it != state->remainingInTopDir.end(); ++it) {
            if (it.value() == 0) {
                std::vector<openscp::FileInfo> out;
                std::string lerr;
                if (sftp_ && sftp_->list(it.key().toStdString(), out, lerr) &&
                    out.empty()) {
                    std::string derr;
                    if (sftp_->removeDir(it.key().toStdString(), derr)) {
                        state->deletedDirs.insert(it.key());
                    }
                }
            }
        }
        auto connPtr = std::make_shared<QMetaObject::Connection>();
        *connPtr = connect(
            transferMgr_, &TransferManager::tasksChanged, this,
            [this, state, remoteBase, connPtr, pairs]() {
                const auto tasks = transferMgr_->tasksSnapshot();
                // 1) For each successfully completed task, delete the
                // corresponding remote file (once)
                for (const auto &t : tasks) {
                    if (t.type != TransferTask::Type::Download)
                        continue;
                    if (t.status != TransferTask::Status::Done)
                        continue;
                    const QString r = t.src;
                    if (!state->filesPending.contains(r))
                        continue; // does not belong to this move or already
                                  // deleted
                    if (state->filesProcessed.contains(r))
                        continue;
                    // Try to delete the remote file
                    std::string ferr;
                    bool okDel =
                        sftp_ && sftp_->removeFile(r.toStdString(), ferr);
                    state->filesProcessed.insert(r);
                    if (okDel) {
                        state->filesPending.remove(r);
                        // Decrement counter for the top directory it belongs to
                        const QString topDir = state->fileToTopDir.value(r);
                        if (!topDir.isEmpty()) {
                            int rem =
                                state->remainingInTopDir.value(topDir) - 1;
                            state->remainingInTopDir[topDir] = rem;
                            if (rem == 0 &&
                                !state->deletedDirs.contains(topDir)) {
                                // All files under this top dir were moved:
                                // delete folder only if empty
                                std::vector<openscp::FileInfo> out;
                                std::string lerr;
                                if (sftp_ &&
                                    sftp_->list(topDir.toStdString(), out,
                                                lerr) &&
                                    out.empty()) {
                                    std::string derr;
                                    if (sftp_->removeDir(topDir.toStdString(),
                                                         derr)) {
                                        state->deletedDirs.insert(topDir);
                                    }
                                }
                            }
                        }
                    } else {
                        // Not deleted: keep it out to avoid endless retries;
                        // could retry if desired
                        state->filesPending.remove(r);
                    }
                }

                // 2) Disconnect when all related tasks have reached a final
                // state
                bool allFinal = true;
                for (const auto &pr : pairs) {
                    bool found = false, final = false;
                    for (const auto &t : tasks) {
                        if (t.type == TransferTask::Type::Download &&
                            t.src == pr.first && t.dst == pr.second) {
                            found = true;
                            if (t.status == TransferTask::Status::Done ||
                                t.status == TransferTask::Status::Error ||
                                t.status == TransferTask::Status::Canceled)
                                final = true;
                            break;
                        }
                    }
                    if (!found || !final) {
                        allFinal = false;
                        break;
                    }
                }
                if (allFinal) {
                    // Refrescar vista remota una vez al final
                    QString dummy;
                    if (rightRemoteModel_)
                        rightRemoteModel_->setRootPath(remoteBase, &dummy);
                    QObject::disconnect(*connPtr);
                }
            });
    }
}


void MainWindow::uploadViaDialog() {
    if (!rightIsRemote_ || !sftp_ || !rightRemoteModel_) {
        QMessageBox::information(
            this, tr("Upload"),
            tr("The right panel is not remote or there is no active session."));
        return;
    }
    const QString startDir =
        uploadDir_.isEmpty() ? QDir::homePath() : uploadDir_;
    QFileDialog dlg(this, tr("Select files or folders to upload"), startDir);
    dlg.setFileMode(QFileDialog::ExistingFiles);
    dlg.setOption(QFileDialog::DontUseNativeDialog, true);
    dlg.setViewMode(QFileDialog::Detail);
    if (auto *lv = dlg.findChild<QListView *>("listView"))
        lv->setSelectionMode(QAbstractItemView::ExtendedSelection);
    if (auto *tv = dlg.findChild<QTreeView *>())
        tv->setSelectionMode(QAbstractItemView::ExtendedSelection);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QStringList picks = dlg.selectedFiles();
    if (picks.isEmpty())
        return;
    uploadDir_ = QFileInfo(picks.first()).dir().absolutePath();
    QStringList files;
    for (const QString &p : picks) {
        QFileInfo fi(p);
        if (fi.isDir()) {
            QDirIterator it(p, QDir::NoDotAndDotDot | QDir::AllEntries,
                            QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                if (it.fileInfo().isFile())
                    files << it.filePath();
            }
        } else if (fi.isFile()) {
            files << fi.absoluteFilePath();
        }
    }
    if (files.isEmpty()) {
        statusBar()->showMessage(tr("Nothing to upload."), 4000);
        return;
    }
    int enq = 0;
    const QString remoteBase = rightRemoteModel_->rootPath();
    for (const QString &localPath : files) {
        const QFileInfo fi(localPath);
        QString relBase = fi.path().startsWith(uploadDir_)
                              ? fi.path().mid(uploadDir_.size()).trimmed()
                              : QString();
        if (relBase.startsWith('/'))
            relBase.remove(0, 1);
        QString targetDir = relBase.isEmpty()
                                ? remoteBase
                                : joinRemotePath(remoteBase, relBase);
        if (!targetDir.isEmpty() && targetDir != remoteBase) {
            bool isDir = false;
            std::string se;
            bool ex = sftp_->exists(targetDir.toStdString(), isDir, se);
            if (!ex && se.empty()) {
                std::string me;
                sftp_->mkdir(targetDir.toStdString(), me, 0755);
            }
        }
        const QString rTarget = joinRemotePath(targetDir, fi.fileName());
        transferMgr_->enqueueUpload(localPath, rTarget);
        ++enq;
    }
    if (enq > 0) {
        statusBar()->showMessage(QString(tr("Queued: %1 uploads")).arg(enq),
                                 4000);
        maybeShowTransferQueue();
    }
}

void MainWindow::newDirRight() {
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("New folder"), tr("Name:"), QLineEdit::Normal, {}, &ok);
    if (!ok || name.isEmpty())
        return;
    QString why;
    if (!isValidEntryName(name, &why)) {
        QMessageBox::warning(this, tr("Invalid name"), why);
        return;
    }
    if (rightIsRemote_) {
        if (!sftp_ || !rightRemoteModel_)
            return;
        const QString path =
            joinRemotePath(rightRemoteModel_->rootPath(), name);
        std::string err;
        if (!sftp_->mkdir(path.toStdString(), err, 0755)) {
            invalidateRemoteWriteabilityFromError(QString::fromStdString(err));
            QMessageBox::critical(
                this, tr("SFTP"),
                tr("Could not create the remote folder.\n%1")
                    .arg(shortRemoteError(err, tr("Remote error"))));
            return;
        }
        QString dummy;
        rightRemoteModel_->setRootPath(rightRemoteModel_->rootPath(), &dummy);
        cacheCurrentRemoteWriteability(true);
    } else {
        QDir base(rightPath_->text());
        if (!base.mkpath(base.filePath(name))) {
            QMessageBox::critical(this, tr("Local"),
                                  tr("Could not create folder."));
            return;
        }
        setRightRoot(base.absolutePath());
    }
}

// Create a new empty file in the right pane (local only).
void MainWindow::newFileRight() {
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("New file"), tr("Name:"), QLineEdit::Normal, {}, &ok);
    if (!ok || name.isEmpty())
        return;
    QString why;
    if (!isValidEntryName(name, &why)) {
        QMessageBox::warning(this, tr("Invalid name"), why);
        return;
    }
    if (rightIsRemote_) {
        if (!sftp_ || !rightRemoteModel_)
            return;
        const QString remotePath =
            joinRemotePath(rightRemoteModel_->rootPath(), name);
        bool isDir = false;
        std::string e;
        bool exists = sftp_->exists(remotePath.toStdString(), isDir, e);
        if (exists) {
            if (QMessageBox::question(
                    this, tr("File exists"),
                    tr("«%1» already exists.\\nOverwrite?").arg(name),
                    QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
                return;
        } else if (!e.empty()) {
            QMessageBox::critical(
                this, tr("SFTP"),
                tr("Could not check whether the remote file already "
                   "exists.\n%1")
                    .arg(shortRemoteError(e, tr("Remote error"))));
            return;
        }

        QTemporaryFile tmp;
        if (!tmp.open()) {
            QMessageBox::critical(this, tr("Temporary"),
                                  tr("Could not create a temporary file."));
            return;
        }
        tmp.close();
        std::string err;
        bool okPut = sftp_->put(tmp.fileName().toStdString(),
                                remotePath.toStdString(), err);
        if (!okPut) {
            invalidateRemoteWriteabilityFromError(QString::fromStdString(err));
            QMessageBox::critical(
                this, tr("SFTP"),
                tr("Could not upload the temporary file to the server.\n%1")
                    .arg(shortRemoteError(err, tr("Remote error"))));
            return;
        }
        QString dummy;
        rightRemoteModel_->setRootPath(rightRemoteModel_->rootPath(), &dummy);
        cacheCurrentRemoteWriteability(true);
        statusBar()->showMessage(tr("File created: ") + remotePath, 4000);
    } else {
        QDir base(rightPath_->text());
        const QString path = base.filePath(name);
        if (QFileInfo::exists(path)) {
            if (QMessageBox::question(
                    this, tr("File exists"),
                    tr("«%1» already exists.\\nOverwrite?").arg(name),
                    QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
                return;
        }
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            QMessageBox::critical(this, tr("Local"),
                                  tr("Could not create file."));
            return;
        }
        f.close();
        setRightRoot(base.absolutePath());
        statusBar()->showMessage(tr("File created: ") + path, 4000);
    }
}

// Rename the selected entry on the right pane (local or remote).
void MainWindow::renameRightSelected() {
    auto sel = rightView_->selectionModel();
    if (!sel)
        return;
    const auto rows = sel->selectedRows();
    if (rows.size() != 1) {
        QMessageBox::information(this, tr("Rename"),
                                 tr("Select exactly one item."));
        return;
    }
    if (rightIsRemote_) {
        if (!sftp_ || !rightRemoteModel_)
            return;
        const QModelIndex idx = rows.first();
        const QString oldName = rightRemoteModel_->nameAt(idx);
        bool ok = false;
        const QString newName =
            QInputDialog::getText(this, tr("Rename"), tr("New name:"),
                                  QLineEdit::Normal, oldName, &ok);
        if (!ok || newName.isEmpty() || newName == oldName)
            return;
        const QString base = rightRemoteModel_->rootPath();
        const QString from = joinRemotePath(base, oldName);
        const QString to = joinRemotePath(base, newName);
        std::string err;
        if (!sftp_->rename(from.toStdString(), to.toStdString(), err, false)) {
            invalidateRemoteWriteabilityFromError(QString::fromStdString(err));
            QMessageBox::critical(
                this, tr("SFTP"),
                tr("Could not rename the remote item.\n%1")
                    .arg(shortRemoteError(err, tr("Remote error"))));
            return;
        }
        QString dummy;
        rightRemoteModel_->setRootPath(base, &dummy);
        cacheCurrentRemoteWriteability(true);
    } else {
        const QModelIndex idx = rows.first();
        const QFileInfo fi = rightLocalModel_->fileInfo(idx);
        bool ok = false;
        const QString newName =
            QInputDialog::getText(this, tr("Rename"), tr("New name:"),
                                  QLineEdit::Normal, fi.fileName(), &ok);
        if (!ok || newName.isEmpty() || newName == fi.fileName())
            return;
        const QString newPath = QDir(fi.absolutePath()).filePath(newName);
        bool renamed = QFile::rename(fi.absoluteFilePath(), newPath);
        if (!renamed)
            renamed =
                QDir(fi.absolutePath()).rename(fi.absoluteFilePath(), newPath);
        if (!renamed) {
            QMessageBox::critical(this, tr("Local"), tr("Could not rename."));
            return;
        }
        setRightRoot(rightPath_->text());
    }
}

void MainWindow::deleteRightSelected() {
    auto sel = rightView_->selectionModel();
    if (!sel)
        return;
    const auto rows = sel->selectedRows();
    if (rows.isEmpty()) {
        QMessageBox::information(this, tr("Delete"), tr("Nothing selected."));
        return;
    }
    if (rightIsRemote_) {
        if (!sftp_ || !rightRemoteModel_)
            return;
        if (QMessageBox::warning(this, tr("Confirm delete"),
                                 tr("This will permanently delete items on the "
                                    "remote server.\\nContinue?"),
                                 QMessageBox::Yes | QMessageBox::No) !=
            QMessageBox::Yes)
            return;
        int ok = 0, fail = 0;
        QString lastErr;
        const QString base = rightRemoteModel_->rootPath();
        std::function<bool(const QString &)> delRec = [&](const QString &p) {
            // Determine if path is a directory or a file using stat/exists
            bool isDir = false;
            std::string xerr;
            if (!sftp_->exists(p.toStdString(), isDir, xerr)) {
                // If it doesn't exist, treat as success
                if (xerr.empty())
                    return true;
                lastErr = QString::fromStdString(xerr);
                return false;
            }
            if (!isDir) {
                std::string ferr;
                if (!sftp_->removeFile(p.toStdString(), ferr)) {
                    lastErr = QString::fromStdString(ferr);
                    return false;
                }
                return true;
            }
            // Directory: list and remove children first (depth-first)
            std::vector<openscp::FileInfo> out;
            std::string lerr;
            if (!sftp_->list(p.toStdString(), out, lerr)) {
                lastErr = QString::fromStdString(lerr);
                return false;
            }
            for (const auto &e : out) {
                const QString child =
                    joinRemotePath(p, QString::fromStdString(e.name));
                if (!delRec(child))
                    return false;
            }
            std::string derr;
            if (!sftp_->removeDir(p.toStdString(), derr)) {
                lastErr = QString::fromStdString(derr);
                return false;
            }
            return true;
        };
        for (const QModelIndex &idx : rows) {
            const QString name = rightRemoteModel_->nameAt(idx);
            const QString path = joinRemotePath(base, name);
            if (delRec(path))
                ++ok;
            else
                ++fail;
        }
        QString msg =
            QString(tr("Deleted OK: %1  |  Failed: %2")).arg(ok).arg(fail);
        if (fail > 0 && !lastErr.isEmpty())
            msg += "\n" + tr("Last error: ") + lastErr;
        statusBar()->showMessage(msg, 6000);
        if (fail > 0)
            invalidateRemoteWriteabilityFromError(lastErr);
        if (fail == 0 && ok > 0)
            cacheCurrentRemoteWriteability(true);
        QString dummy;
        rightRemoteModel_->setRootPath(base, &dummy);
    } else {
        if (QMessageBox::warning(
                this, tr("Confirm delete"),
                tr("This will permanently delete on local disk.\nContinue?"),
                QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
            return;
        int ok = 0, fail = 0;
        for (const QModelIndex &idx : rows) {
            const QFileInfo fi = rightLocalModel_->fileInfo(idx);
            bool removed = fi.isDir()
                               ? QDir(fi.absoluteFilePath()).removeRecursively()
                               : QFile::remove(fi.absoluteFilePath());
            if (removed)
                ++ok;
            else
                ++fail;
        }
        statusBar()->showMessage(
            QString(tr("Deleted: %1  |  Failed: %2")).arg(ok).arg(fail), 5000);
        setRightRoot(rightPath_->text());
    }
}

// Show context menu for the right pane based on current state.
void MainWindow::showRightContextMenu(const QPoint &pos) {
    if (!rightContextMenu_)
        rightContextMenu_ = new QMenu(this);
    rightContextMenu_->clear();

    // Selection state and ability to go up
    bool hasSel = false;
    if (auto sel = rightView_->selectionModel()) {
        hasSel = !sel->selectedRows(NAME_COL).isEmpty();
    }
    // Is there a parent directory?
    bool canGoUp = false;
    if (rightIsRemote_) {
        QString cur =
            rightRemoteModel_ ? rightRemoteModel_->rootPath() : QString();
        if (cur.endsWith('/'))
            cur.chop(1);
        canGoUp = (!cur.isEmpty() && cur != "/");
    } else {
        QDir d(rightPath_ ? rightPath_->text() : QString());
        canGoUp = d.cdUp();
    }

    if (rightIsRemote_) {
        // Up option (if applicable)
        if (canGoUp && actUpRight_)
            rightContextMenu_->addAction(actUpRight_);

        // Always show "Download" on remote, regardless of selection
        if (actDownloadF7_)
            rightContextMenu_->addAction(actDownloadF7_);

        if (!hasSel) {
            // No selection: creation and navigation
            if (rightRemoteWritable_) {
                if (actNewFileRight_)
                    rightContextMenu_->addAction(actNewFileRight_);
                if (actNewDirRight_)
                    rightContextMenu_->addAction(actNewDirRight_);
            }
        } else {
            // With selection on remote
            if (actCopyRight_)
                rightContextMenu_->addAction(actCopyRight_);
            if (rightRemoteWritable_) {
                rightContextMenu_->addSeparator();
                if (actUploadRight_)
                    rightContextMenu_->addAction(actUploadRight_);
                if (actNewFileRight_)
                    rightContextMenu_->addAction(actNewFileRight_);
                if (actNewDirRight_)
                    rightContextMenu_->addAction(actNewDirRight_);
                if (actRenameRight_)
                    rightContextMenu_->addAction(actRenameRight_);
                if (actDeleteRight_)
                    rightContextMenu_->addAction(actDeleteRight_);
                if (actMoveRight_)
                    rightContextMenu_->addAction(actMoveRight_);
                rightContextMenu_->addSeparator();
                rightContextMenu_->addAction(
                    tr("Change permissions…"), this,
                    &MainWindow::changeRemotePermissions);
            }
        }
    } else {
        // Local: Up option if applicable
        if (canGoUp && actUpRight_)
            rightContextMenu_->addAction(actUpRight_);
        if (!hasSel) {
            // No selection: creation
            if (actNewFileRight_)
                rightContextMenu_->addAction(actNewFileRight_);
            if (actNewDirRight_)
                rightContextMenu_->addAction(actNewDirRight_);
        } else {
            // With selection: local operations + copy/move from left
            if (actNewFileRight_)
                rightContextMenu_->addAction(actNewFileRight_);
            if (actNewDirRight_)
                rightContextMenu_->addAction(actNewDirRight_);
            if (actRenameRight_)
                rightContextMenu_->addAction(actRenameRight_);
            if (actDeleteRight_)
                rightContextMenu_->addAction(actDeleteRight_);
            rightContextMenu_->addSeparator();
            // Copy/move the selection from the right panel to the left
            if (actCopyRight_)
                rightContextMenu_->addAction(actCopyRight_);
            if (actMoveRight_)
                rightContextMenu_->addAction(actMoveRight_);
        }
    }
    rightContextMenu_->popup(rightView_->viewport()->mapToGlobal(pos));
}

void MainWindow::changeRemotePermissions() {
    if (!rightIsRemote_ || !sftp_ || !rightRemoteModel_)
        return;
    auto sel = rightView_->selectionModel();
    if (!sel)
        return;
    const auto rows = sel->selectedRows();
    if (rows.size() != 1) {
        QMessageBox::information(this, tr("Permissions"),
                                 tr("Select only one item."));
        return;
    }
    const QModelIndex idx = rows.first();
    const QString name = rightRemoteModel_->nameAt(idx);
    const QString base = rightRemoteModel_->rootPath();
    const QString path = joinRemotePath(base, name);
    openscp::FileInfo st{};
    std::string err;
    if (!sftp_->stat(path.toStdString(), st, err)) {
        QMessageBox::warning(
            this, tr("Permissions"),
            tr("Could not read permissions.\\n%1")
                .arg(shortRemoteError(
                    err, tr("Error reading remote information."))));
        return;
    }
    PermissionsDialog dlg(this);
    dlg.setMode(st.mode & 0777);
    if (dlg.exec() != QDialog::Accepted)
        return;
    unsigned int newMode = (st.mode & ~0777u) | (dlg.mode() & 0777u);
    auto applyOne = [&](const QString &p) -> bool {
        std::string cerrs;
        if (!sftp_->chmod(p.toStdString(), newMode, cerrs)) {
            invalidateRemoteWriteabilityFromError(
                QString::fromStdString(cerrs));
            const QString item =
                QFileInfo(p).fileName().isEmpty() ? p : QFileInfo(p).fileName();
            QMessageBox::critical(
                this, tr("Permissions"),
                tr("Could not apply permissions to \"%1\".\\n%2")
                    .arg(item, shortRemoteError(
                                   cerrs, tr("Error applying changes."))));
            return false;
        }
        return true;
    };
    bool ok = true;
    if (dlg.recursive() && st.is_dir) {
        QVector<QString> stack;
        stack.push_back(path);
        while (!stack.isEmpty() && ok) {
            const QString cur = stack.back();
            stack.pop_back();
            if (!applyOne(cur)) {
                ok = false;
                break;
            }
            std::vector<openscp::FileInfo> out;
            std::string lerr;
            if (!sftp_->list(cur.toStdString(), out, lerr))
                continue;
            for (const auto &e : out) {
                const QString child =
                    joinRemotePath(cur, QString::fromStdString(e.name));
                if (e.is_dir)
                    stack.push_back(child);
                else {
                    if (!applyOne(child)) {
                        ok = false;
                        break;
                    }
                }
            }
        }
    } else {
        ok = applyOne(path);
    }
    if (!ok)
        return;
    QString dummy;
    rightRemoteModel_->setRootPath(base, &dummy);
    cacheCurrentRemoteWriteability(true);
    statusBar()->showMessage(tr("Permissions updated"), 3000);
}

void MainWindow::applyRemoteWriteabilityActions() {
    if (actUploadRight_)
        actUploadRight_->setEnabled(rightRemoteWritable_);
    if (actNewDirRight_)
        actNewDirRight_->setEnabled(rightRemoteWritable_);
    if (actNewFileRight_)
        actNewFileRight_->setEnabled(rightRemoteWritable_);
    if (actRenameRight_)
        actRenameRight_->setEnabled(rightRemoteWritable_);
    if (actDeleteRight_)
        actDeleteRight_->setEnabled(rightRemoteWritable_);
    if (actMoveRight_)
        actMoveRight_->setEnabled(rightRemoteWritable_);
    if (actMoveRightTb_)
        actMoveRightTb_->setEnabled(rightRemoteWritable_);
    updateDeleteShortcutEnables();
}

void MainWindow::cacheCurrentRemoteWriteability(bool writable) {
    if (!rightIsRemote_ || !rightRemoteModel_) {
        rightRemoteWritable_ = false;
        m_remoteWriteabilityCache_.clear();
        applyRemoteWriteabilityActions();
        return;
    }
    const QString base = rightRemoteModel_->rootPath();
    rightRemoteWritable_ = writable;
    m_remoteWriteabilityCache_.insert(
        base, RemoteWriteabilityCacheEntry{writable,
                                           QDateTime::currentMSecsSinceEpoch()});
    if (m_remoteWriteabilityCache_.size() > 256)
        m_remoteWriteabilityCache_.clear();
    applyRemoteWriteabilityActions();
}

void MainWindow::invalidateRemoteWriteabilityFromError(
    const QString &rawError) {
    if (!rightIsRemote_ || !rightRemoteModel_)
        return;
    if (!indicatesRemoteWriteabilityDenied(rawError))
        return;
    cacheCurrentRemoteWriteability(false);
}

// Check if the current remote directory is writable and update enables.
void MainWindow::updateRemoteWriteability() {
    if (!rightIsRemote_ || !sftp_ || !rightRemoteModel_) {
        rightRemoteWritable_ = false;
        m_remoteWriteabilityCache_.clear();
        applyRemoteWriteabilityActions();
        return;
    }

    const QString base = rightRemoteModel_->rootPath();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    auto it = m_remoteWriteabilityCache_.constFind(base);
    if (it != m_remoteWriteabilityCache_.cend()) {
        if ((nowMs - it->checkedAtMs) <= m_remoteWriteabilityTtlMs_) {
            rightRemoteWritable_ = it->writable;
            applyRemoteWriteabilityActions();
            return;
        }
    }

    // Active probe only when cache is stale/missing.
    const QString testName =
        ".openscp-write-test-" + QString::number(nowMs);
    const QString testPath =
        base.endsWith('/') ? base + testName : base + "/" + testName;
    std::string err;
    const bool created = sftp_->mkdir(testPath.toStdString(), err, 0755);
    if (created) {
        std::string derr;
        sftp_->removeDir(testPath.toStdString(), derr);
        rightRemoteWritable_ = true;
    } else {
        rightRemoteWritable_ = false;
    }
    m_remoteWriteabilityCache_.insert(
        base, RemoteWriteabilityCacheEntry{rightRemoteWritable_, nowMs});
    if (m_remoteWriteabilityCache_.size() > 256)
        m_remoteWriteabilityCache_.clear();
    applyRemoteWriteabilityActions();
}
