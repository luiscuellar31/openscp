// MainWindow local-side filesystem operations and local navigation.
#include "MainWindow.hpp"
#include "RemoteModel.hpp"
#include "TransferManager.hpp"

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
        QDirIterator it(srcPath, QDir::NoDotAndDotDot | QDir::AllEntries,
                        QDirIterator::Subdirectories);
        while (it.hasNext()) {
            it.next();
            const QFileInfo fi = it.fileInfo();
            const QString rel =
                QDir(srcPath).relativeFilePath(fi.absoluteFilePath());
            const QString target = QDir(dstPath).filePath(rel);

            if (fi.isDir()) {
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
                if (!QFile::copy(fi.absoluteFilePath(), target)) {
                    error = QString(QCoreApplication::translate(
                                        "MainWindow", "Failed to copy: %1"))
                                .arg(fi.absoluteFilePath());
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

// Validate simple file/folder names (no paths)
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

static QString joinRemotePath(const QString &base, const QString &name) {
    if (base == "/")
        return "/" + name;
    return base.endsWith('/') ? base + name : base + "/" + name;
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
        leftPath_->setText(path);
        leftView_->setRootIndex(leftModel_->index(path));
        statusBar()->showMessage(tr("Left: ") + path, 3000);
        updateDeleteShortcutEnables();
    } else {
        QMessageBox::warning(this, tr("Invalid path"),
                             tr("Folder does not exist."));
    }
}

// Set the right (local) pane root and update view/status.
void MainWindow::setRightRoot(const QString &path) {
    if (QDir(path).exists()) {
        rightPath_->setText(path);
        rightView_->setRootIndex(rightLocalModel_->index(path)); // <-- here
        statusBar()->showMessage(tr("Right: ") + path, 3000);
        updateDeleteShortcutEnables();
    } else {
        QMessageBox::warning(this, tr("Invalid path"),
                             tr("Folder does not exist."));
    }
}

void MainWindow::runLocalFsOperation(const QVector<LocalFsPair> &pairs,
                                     bool deleteSource, int skippedCount) {
    if (pairs.isEmpty()) {
        QString msg;
        if (deleteSource) {
            msg = QString(tr("Moved OK: %1  |  Failed: %2  |  Skipped: %3"))
                      .arg(0)
                      .arg(0)
                      .arg(skippedCount);
        } else if (skippedCount > 0) {
            msg = QString(tr("Copied: %1  |  Failed: %2  |  Skipped: %3"))
                      .arg(0)
                      .arg(0)
                      .arg(skippedCount);
        } else {
            msg = QString(tr("Copied: %1  |  Failed: %2")).arg(0).arg(0);
        }
        statusBar()->showMessage(msg, 5000);
        return;
    }

    ++m_localFsJobsInFlight_;
    statusBar()->showMessage(
        deleteSource ? tr("Moving selected items...")
                     : tr("Copying selected items..."),
        1500);

    QPointer<MainWindow> self(this);
    std::thread([self, pairs, deleteSource, skippedCount]() {
        int ok = 0;
        int fail = 0;
        QString lastError;

        for (const auto &pair : pairs) {
            QString err;
            if (copyEntryRecursively(pair.sourcePath, pair.targetPath, err)) {
                if (deleteSource) {
                    const QFileInfo srcInfo(pair.sourcePath);
                    const bool removed =
                        srcInfo.isDir()
                            ? QDir(pair.sourcePath).removeRecursively()
                            : QFile::remove(pair.sourcePath);
                    if (removed || !QFileInfo::exists(pair.sourcePath)) {
                        ++ok;
                    } else {
                        ++fail;
                        lastError =
                            QString(QCoreApplication::translate(
                                        "MainWindow",
                                        "Could not delete source: %1"))
                                .arg(pair.sourcePath);
                    }
                } else {
                    ++ok;
                }
            } else {
                ++fail;
                lastError = err;
            }
        }

        QObject *const app = QCoreApplication::instance();
        if (!app)
            return;
        QMetaObject::invokeMethod(
            app, [self, ok, fail, skippedCount, lastError, deleteSource]() {
                if (!self)
                    return;

                --self->m_localFsJobsInFlight_;

                QString msg;
                if (deleteSource) {
                    msg = QString(
                              self->tr("Moved OK: %1  |  Failed: %2  |  "
                                       "Skipped: %3"))
                              .arg(ok)
                              .arg(fail)
                              .arg(skippedCount);
                } else if (skippedCount > 0) {
                    msg = QString(
                              self->tr("Copied: %1  |  Failed: %2  |  "
                                       "Skipped: %3"))
                              .arg(ok)
                              .arg(fail)
                              .arg(skippedCount);
                } else {
                    msg = QString(self->tr("Copied: %1  |  Failed: %2"))
                              .arg(ok)
                              .arg(fail);
                }
                if (fail > 0 && !lastError.isEmpty()) {
                    msg += "\n" + self->tr("Last error: ") + lastError;
                }
                self->statusBar()->showMessage(msg, 6000);

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
        if (!sftp_ || !rightRemoteModel_) {
            QMessageBox::warning(this, tr("SFTP"),
                                 tr("No active SFTP session."));
            return;
        }

        // Selection on the left panel (local source)
        auto sel = leftView_->selectionModel();
        if (!sel) {
            QMessageBox::warning(this, tr("Copy"),
                                 tr("No selection available."));
            return;
        }
        const auto rows = sel->selectedRows(NAME_COL);
        if (rows.isEmpty()) {
            QMessageBox::information(
                this, tr("Copy"), tr("No entries selected in the left panel."));
            return;
        }

        // Always enqueue uploads
        const QString remoteBase = rightRemoteModel_->rootPath();
        int enq = 0;
        for (const QModelIndex &idx : rows) {
            const QFileInfo fi = leftModel_->fileInfo(idx);
            if (fi.isDir()) {
                const QString remoteDirBase =
                    joinRemotePath(remoteBase, fi.fileName());
                QDirIterator it(fi.absoluteFilePath(),
                                QDir::NoDotAndDotDot | QDir::AllEntries,
                                QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    it.next();
                    const QFileInfo sfi = it.fileInfo();
                    if (!sfi.isFile())
                        continue;
                    const QString rel =
                        QDir(fi.absoluteFilePath())
                            .relativeFilePath(sfi.absoluteFilePath());
                    const QString rTarget = joinRemotePath(remoteDirBase, rel);
                    transferMgr_->enqueueUpload(sfi.absoluteFilePath(),
                                                rTarget);
                    ++enq;
                }
            } else {
                const QString rTarget =
                    joinRemotePath(remoteBase, fi.fileName());
                transferMgr_->enqueueUpload(fi.absoluteFilePath(), rTarget);
                ++enq;
            }
        }
        if (enq > 0) {
            statusBar()->showMessage(QString(tr("Queued: %1 uploads")).arg(enq),
                                     4000);
            maybeShowTransferQueue();
        }
        return;
    }

    // ---- LOCAL→LOCAL branch: existing logic as-is ----
    const QString dstDirPath = rightPath_->text();
    QDir dstDir(dstDirPath);
    if (!dstDir.exists()) {
        QMessageBox::warning(this, tr("Invalid destination"),
                             tr("Destination folder does not exist."));
        return;
    }

    auto sel = leftView_->selectionModel();
    if (!sel) {
        QMessageBox::warning(this, tr("Copy"), tr("No selection available."));
        return;
    }
    const auto rows = sel->selectedRows(NAME_COL);
    if (rows.isEmpty()) {
        QMessageBox::information(this, tr("Copy"),
                                 tr("No entries selected in the left panel."));
        return;
    }

    enum class OverwritePolicy { Ask, OverwriteAll, SkipAll };
    OverwritePolicy policy = OverwritePolicy::Ask;

    int skipped = 0;
    QVector<LocalFsPair> pairs;

    for (const QModelIndex &idx : rows) {
        const QFileInfo fi = leftModel_->fileInfo(idx);
        const QString target = dstDir.filePath(fi.fileName());

        if (QFileInfo::exists(target)) {
            if (policy == OverwritePolicy::Ask) {
                auto ret = QMessageBox::question(
                    this, tr("Conflict"),
                    QString(
                        tr("“%1” already exists at destination.\nOverwrite?"))
                        .arg(fi.fileName()),
                    QMessageBox::Yes | QMessageBox::No | QMessageBox::YesToAll |
                        QMessageBox::NoToAll);
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
}

void MainWindow::moveLeftToRight() {
    if (rightIsRemote_) {
        if (!sftp_ || !rightRemoteModel_) {
            QMessageBox::warning(this, tr("SFTP"),
                                 tr("No active SFTP session."));
            return;
        }
        const auto rows = leftView_->selectionModel()->selectedRows(NAME_COL);
        if (rows.isEmpty()) {
            QMessageBox::information(
                this, tr("Move"), tr("No entries selected in the left panel."));
            return;
        }
        if (QMessageBox::question(this, tr("Confirm move"),
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

        auto ensureRemoteDir = [&](const QString &dir) -> bool {
            if (dir.isEmpty())
                return true;
            QString cur = "/";
            const QStringList parts = dir.split('/', Qt::SkipEmptyParts);
            for (const QString &part : parts) {
                const QString next =
                    (cur == "/") ? ("/" + part) : (cur + "/" + part);
                bool isD = false;
                std::string e;
                const bool ex = sftp_->exists(next.toStdString(), isD, e);
                if (!e.empty()) {
                    prepError = QString::fromStdString(e);
                    return false;
                }
                if (!ex) {
                    std::string me;
                    if (!sftp_->mkdir(next.toStdString(), me, 0755)) {
                        prepError = QString::fromStdString(me);
                        return false;
                    }
                }
                cur = next;
            }
            return true;
        };

        for (const QModelIndex &idx : rows) {
            const QFileInfo fi = leftModel_->fileInfo(idx);
            if (fi.isDir()) {
                const QString topLocalDir = fi.absoluteFilePath();
                const QString baseRemoteDir =
                    joinRemotePath(remoteBase, fi.fileName());
                if (!ensureRemoteDir(baseRemoteDir)) {
                    ++skippedPrep;
                    continue;
                }
                const int pairStart = pairs.size();
                bool dirPrepFailed = false;
                int filesInDir = 0;
                QDirIterator it(topLocalDir,
                                QDir::NoDotAndDotDot | QDir::AllEntries,
                                QDirIterator::Subdirectories);
                while (it.hasNext()) {
                    it.next();
                    const QFileInfo sfi = it.fileInfo();
                    const QString rel =
                        QDir(topLocalDir)
                            .relativeFilePath(sfi.absoluteFilePath());
                    const QString rTarget = joinRemotePath(baseRemoteDir, rel);
                    if (sfi.isDir()) {
                        if (!ensureRemoteDir(rTarget)) {
                            dirPrepFailed = true;
                            break;
                        }
                        continue;
                    }
                    if (!sfi.isFile())
                        continue;
                    pairs.push_back(
                        {sfi.absoluteFilePath(), rTarget, topLocalDir});
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
            } else if (fi.isFile()) {
                const QString rTarget =
                    joinRemotePath(remoteBase, fi.fileName());
                pairs.push_back({fi.absoluteFilePath(), rTarget, QString()});
            }
        }

        for (const auto &p : pairs)
            transferMgr_->enqueueUpload(p.localPath, p.remotePath);
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
            QString msg = QString(tr("Could not prepare items to move: %1"))
                              .arg(skippedPrep);
            if (!prepError.isEmpty())
                msg += "\n" + tr("Last error: ") + prepError;
            statusBar()->showMessage(msg, 5000);
        }

        // Local cleanup on successful upload, without blocking UI.
        if (!pairs.isEmpty()) {
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

            for (const auto &p : pairs) {
                state->pendingLocalFiles.insert(p.localPath);
                if (!p.topLocalDir.isEmpty()) {
                    state->fileToTopDir.insert(p.localPath, p.topLocalDir);
                    state->remainingInTopDir[p.topLocalDir] =
                        state->remainingInTopDir.value(p.topLocalDir) + 1;
                }
            }

            auto connPtr = std::make_shared<QMetaObject::Connection>();
            *connPtr = connect(
                transferMgr_, &TransferManager::tasksChanged, this,
                [this, state, remoteBase, connPtr, pairs]() {
                    const auto tasks = transferMgr_->tasksSnapshot();

                    for (const auto &t : tasks) {
                        if (t.type != TransferTask::Type::Upload)
                            continue;
                        const QString local = t.src;
                        if (!state->pendingLocalFiles.contains(local))
                            continue;
                        if (state->processedLocalFiles.contains(local))
                            continue;
                        if (t.status != TransferTask::Status::Done &&
                            t.status != TransferTask::Status::Error &&
                            t.status != TransferTask::Status::Canceled) {
                            continue;
                        }

                        state->processedLocalFiles.insert(local);
                        const QString topDir = state->fileToTopDir.value(local);
                        const bool uploadDone =
                            (t.status == TransferTask::Status::Done);
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
                            if (!t.error.isEmpty())
                                state->lastError = t.error;
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

                    bool allFinal = true;
                    for (const auto &p : pairs) {
                        bool found = false;
                        bool final = false;
                        for (const auto &t : tasks) {
                            if (t.type == TransferTask::Type::Upload &&
                                t.src == p.localPath && t.dst == p.remotePath) {
                                found = true;
                                if (t.status == TransferTask::Status::Done ||
                                    t.status == TransferTask::Status::Error ||
                                    t.status ==
                                        TransferTask::Status::Canceled) {
                                    final = true;
                                }
                                break;
                            }
                        }
                        if (!found || !final) {
                            allFinal = false;
                            break;
                        }
                    }

                    if (allFinal) {
                        QString msg = QString(tr("Moved OK: %1  |  Failed: %2  "
                                                 "|  Skipped: %3"))
                                          .arg(state->movedOk)
                                          .arg(state->failed)
                                          .arg(state->skipped);
                        if (state->failed > 0 && !state->lastError.isEmpty()) {
                            msg += "\n" + tr("Last error: ") + state->lastError;
                        }
                        statusBar()->showMessage(msg, 6000);
                        setLeftRoot(leftPath_->text());
                        QString dummy;
                        if (rightRemoteModel_)
                            rightRemoteModel_->setRootPath(remoteBase, &dummy);
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
        QMessageBox::warning(this, tr("Invalid destination"),
                             tr("Destination folder does not exist."));
        return;
    }
    const auto rows = leftView_->selectionModel()->selectedRows(NAME_COL);
    if (rows.isEmpty()) {
        QMessageBox::information(this, tr("Move"),
                                 tr("No entries selected in the left panel."));
        return;
    }
    if (QMessageBox::question(
            this, tr("Confirm move"),
            tr("This will copy and then delete the source.\nContinue?")) !=
        QMessageBox::Yes)
        return;
    enum class OverwritePolicy { Ask, OverwriteAll, SkipAll };
    OverwritePolicy policy = OverwritePolicy::Ask;
    int skipped = 0;
    QVector<LocalFsPair> pairs;
    for (const QModelIndex &idx : rows) {
        const QFileInfo fi = leftModel_->fileInfo(idx);
        const QString target = dstDir.filePath(fi.fileName());
        if (QFileInfo::exists(target)) {
            if (policy == OverwritePolicy::Ask) {
                const auto ret = QMessageBox::question(
                    this, tr("Conflict"),
                    QString(
                        tr("“%1” already exists at destination.\nOverwrite?"))
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
}

void MainWindow::deleteFromLeft() {
    const auto rows = leftView_->selectionModel()->selectedRows(NAME_COL);
    if (rows.isEmpty()) {
        QMessageBox::information(this, tr("Delete"),
                                 tr("No entries selected in the left panel."));
        return;
    }
    if (QMessageBox::warning(this, tr("Confirm delete"),
                             tr("This will permanently delete the selected "
                                "items in the left panel.\nContinue?"),
                             QMessageBox::Yes | QMessageBox::No) !=
        QMessageBox::Yes)
        return;
    int ok = 0, fail = 0;
    for (const QModelIndex &idx : rows) {
        const QFileInfo fi = leftModel_->fileInfo(idx);
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
}

void MainWindow::goUpLeft() {
    QString cur = leftPath_->text();
    QDir d(cur);
    if (!d.cdUp())
        return;
    setLeftRoot(d.absolutePath());
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
    const QFileInfo fi = leftModel_->fileInfo(idx);
    if (fi.isDir()) {
        setLeftRoot(fi.absoluteFilePath());
    } else if (fi.isFile()) {
        openLocalPathWithPreference(fi.absoluteFilePath());
    }
}

void MainWindow::renameLeftSelected() {
    auto sel = leftView_->selectionModel();
    if (!sel)
        return;
    const auto rows = sel->selectedRows();
    if (rows.size() != 1) {
        QMessageBox::information(this, tr("Rename"),
                                 tr("Select exactly one item."));
        return;
    }
    const QModelIndex idx = rows.first();
    const QFileInfo fi = leftModel_->fileInfo(idx);
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
    setLeftRoot(leftPath_->text());
}

// Create a new directory in the left (local) pane.
void MainWindow::newDirLeft() {
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
    QDir base(leftPath_->text());
    if (!base.mkpath(base.filePath(name))) {
        QMessageBox::critical(this, tr("Local"),
                              tr("Could not create folder."));
        return;
    }
    setLeftRoot(base.absolutePath());
}

// Create a new empty file in the left (local) pane.
void MainWindow::newFileLeft() {
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
    QDir base(leftPath_->text());
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
        QMessageBox::critical(this, tr("Local"), tr("Could not create file."));
        return;
    }
    f.close();
    setLeftRoot(base.absolutePath());
    statusBar()->showMessage(tr("File created: ") + path, 4000);
}

void MainWindow::showLeftContextMenu(const QPoint &pos) {
    if (!leftContextMenu_)
        leftContextMenu_ = new QMenu(this);
    leftContextMenu_->clear();
    // Selection and ability to go up
    bool hasSel = false;
    if (auto sel = leftView_->selectionModel()) {
        hasSel = !sel->selectedRows(NAME_COL).isEmpty();
    }
    QDir d(leftPath_ ? leftPath_->text() : QString());
    bool canGoUp = d.cdUp();

    // Local actions on the left panel
    if (canGoUp && actUpLeft_)
        leftContextMenu_->addAction(actUpLeft_);
    if (!hasSel) {
        if (actNewFileLeft_)
            leftContextMenu_->addAction(actNewFileLeft_);
        if (actNewDirLeft_)
            leftContextMenu_->addAction(actNewDirLeft_);
    } else {
        if (actNewFileLeft_)
            leftContextMenu_->addAction(actNewFileLeft_);
        if (actNewDirLeft_)
            leftContextMenu_->addAction(actNewDirLeft_);
        if (actRenameLeft_)
            leftContextMenu_->addAction(actRenameLeft_);
        leftContextMenu_->addSeparator();
        // Directional labels in the menu, wired to existing actions
        leftContextMenu_->addAction(tr("Copy to right panel"), this,
                                    &MainWindow::copyLeftToRight);
        leftContextMenu_->addAction(tr("Move to right panel"), this,
                                    &MainWindow::moveLeftToRight);
        if (actDelete_)
            leftContextMenu_->addAction(actDelete_);
    }
    leftContextMenu_->popup(leftView_->viewport()->mapToGlobal(pos));
}
