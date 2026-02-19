// MainWindow transfer queue UI, remote prescan, and drag-and-drop handling.
#include "MainWindow.hpp"
#include "RemoteModel.hpp"
#include "TransferQueueDialog.hpp"
#include "UiAlerts.hpp"

#include <QAbstractAnimation>
#include <QCoreApplication>
#include <QDir>
#include <QDirIterator>
#include <QDragEnterEvent>
#include <QDragMoveEvent>
#include <QDropEvent>
#include <QEasingCurve>
#include <QFileInfo>
#include <QMimeData>
#include <QParallelAnimationGroup>
#include <QProgressDialog>
#include <QPropertyAnimation>
#include <QStatusBar>

#include <atomic>
#include <memory>
#include <thread>

static constexpr int NAME_COL = 0;

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

void MainWindow::runRemoteDownloadPrescan(
    const QVector<RemoteDownloadSeed> &seeds, int initialSkipped,
    bool dragAndDrop) {
    if (!rightIsRemote_ || !sftp_ || !rightRemoteModel_ || !transferMgr_) {
        UiAlerts::warning(this, tr("SFTP"), tr("No active SFTP session."));
        return;
    }
    if (!m_activeSessionOptions_.has_value()) {
        UiAlerts::warning(this, tr("SFTP"),
                             tr("Missing session options for remote scan."));
        return;
    }
    if (m_remoteScanInProgress_.exchange(true)) {
        statusBar()->showMessage(tr("A remote scan is already in progress"),
                                 3000);
        return;
    }
    if (seeds.isEmpty()) {
        m_remoteScanInProgress_ = false;
        if (initialSkipped > 0) {
            statusBar()->showMessage(
                tr("Nothing queued. Skipped invalid: %1").arg(initialSkipped),
                4000);
        }
        return;
    }

    std::string connErr;
    auto scanClient =
        sftp_->newConnectionLike(*m_activeSessionOptions_, connErr);
    if (!scanClient) {
        m_remoteScanInProgress_ = false;
        UiAlerts::warning(
            this, tr("SFTP"),
            tr("Could not start remote scan.\n%1")
                .arg(QString::fromStdString(connErr)));
        return;
    }

    auto cancelRequested = std::make_shared<std::atomic<bool>>(false);
    m_remoteScanCancelRequested_ = cancelRequested;
    auto *scanProgress = new QProgressDialog(
        tr("Preparing remote download queue..."), tr("Cancel"), 0, 0, this);
    scanProgress->setWindowTitle(tr("Preparing queue"));
    scanProgress->setWindowModality(Qt::NonModal);
    scanProgress->setAutoClose(false);
    scanProgress->setAutoReset(false);
    scanProgress->setMinimumDuration(0);
    connect(scanProgress, &QProgressDialog::canceled, this,
            [cancelRequested] { cancelRequested->store(true); });
    m_remoteScanProgress_ = scanProgress;
    scanProgress->show();

    QPointer<MainWindow> self(this);
    std::thread([self, seeds, initialSkipped, dragAndDrop, cancelRequested,
                 scanClient = std::move(scanClient)]() mutable {
        QVector<QPair<QString, QString>> queuedPairs;
        QVector<QPair<QString, QString>> stack;
        int skipped = initialSkipped;
        int scannedDirs = 0;
        int listFailures = 0;
        QString lastError;

        for (const auto &seed : seeds) {
            if (cancelRequested->load())
                break;
            if (seed.isDir) {
                stack.push_back({seed.remotePath, seed.localPath});
            } else {
                queuedPairs.push_back({seed.remotePath, seed.localPath});
            }
        }

        auto postProgress = [&](int dirs, int files) {
            QObject *app = QCoreApplication::instance();
            if (!app)
                return;
            QMetaObject::invokeMethod(
                app,
                [self, dirs, files]() {
                    if (!self || !self->m_remoteScanProgress_)
                        return;
                    self->m_remoteScanProgress_->setLabelText(
                        QCoreApplication::translate(
                            "MainWindow",
                            "Scanning remote folders... %1 folders, %2 "
                            "files found")
                            .arg(dirs)
                            .arg(files));
                },
                Qt::QueuedConnection);
        };

        while (!stack.isEmpty() && !cancelRequested->load()) {
            const auto pair = stack.back();
            stack.pop_back();
            const QString curR = pair.first;
            const QString curL = pair.second;
            QDir().mkpath(curL);
            ++scannedDirs;
            if ((scannedDirs % 25) == 0)
                postProgress(scannedDirs, queuedPairs.size());

            std::vector<openscp::FileInfo> out;
            std::string lerr;
            if (!scanClient->list(curR.toStdString(), out, lerr)) {
                ++listFailures;
                if (!lerr.empty())
                    lastError = QString::fromStdString(lerr);
                continue;
            }

            for (const auto &e : out) {
                if (cancelRequested->load())
                    break;
                const QString ename = QString::fromStdString(e.name);
                QString why;
                if (!isValidEntryName(ename, &why)) {
                    ++skipped;
                    continue;
                }
                const QString childR =
                    (curR.endsWith('/') ? curR + ename : curR + "/" + ename);
                const QString childL = QDir(curL).filePath(ename);
                if (e.is_dir) {
                    stack.push_back({childR, childL});
                } else {
                    queuedPairs.push_back({childR, childL});
                }
            }
        }

        const bool canceled = cancelRequested->load();
        if (scanClient)
            scanClient->disconnect();

        QObject *app = QCoreApplication::instance();
        if (!app)
            return;
        QMetaObject::invokeMethod(
            app,
            [self, queuedPairs = std::move(queuedPairs), skipped, canceled,
             dragAndDrop, listFailures, lastError]() mutable {
                if (!self)
                    return;

                self->m_remoteScanInProgress_ = false;
                self->m_remoteScanCancelRequested_.reset();
                if (self->m_remoteScanProgress_) {
                    self->m_remoteScanProgress_->hide();
                    self->m_remoteScanProgress_->deleteLater();
                    self->m_remoteScanProgress_.clear();
                }

                if (canceled) {
                    self->statusBar()->showMessage(
                        QCoreApplication::translate("MainWindow",
                                                    "Remote scan canceled"),
                        4000);
                    return;
                }

                if (!self->rightIsRemote_ || !self->sftp_ ||
                    !self->transferMgr_) {
                    self->statusBar()->showMessage(
                        QCoreApplication::translate(
                            "MainWindow",
                            "Remote scan finished, but the session is no "
                            "longer active"),
                        5000);
                    return;
                }

                int enq = 0;
                for (const auto &p : queuedPairs) {
                    self->transferMgr_->enqueueDownload(p.first, p.second);
                    ++enq;
                }

                QString msg =
                    dragAndDrop
                        ? QCoreApplication::translate(
                              "MainWindow", "Queued: %1 downloads (DND)")
                        : QCoreApplication::translate("MainWindow",
                                                      "Queued: %1 downloads");
                msg = msg.arg(enq);
                if (skipped > 0)
                    msg += QString("  |  ") +
                           QCoreApplication::translate(
                               "MainWindow", "Skipped invalid: %1")
                               .arg(skipped);
                if (listFailures > 0)
                    msg += QString("  |  ") +
                           QCoreApplication::translate(
                               "MainWindow", "Folders not listed: %1")
                               .arg(listFailures);
                if (listFailures > 0 && !lastError.isEmpty())
                    msg += "\n" +
                           QCoreApplication::translate("MainWindow",
                                                       "Last error: ") +
                           lastError;
                self->statusBar()->showMessage(msg, 6000);
                if (enq > 0)
                    self->maybeShowTransferQueue();
            },
            Qt::QueuedConnection);
    }).detach();
}

void MainWindow::showTransferQueue() {
    if (!transferDlg_)
        transferDlg_ = new TransferQueueDialog(transferMgr_, this);
    const bool wasVisible = transferDlg_->isVisible();
    if (!wasVisible) {
        // Modeless queue: smooth entrance (fade + slight scale/offset) for
        // better perceived polish.
        transferDlg_->show();
        transferDlg_->raise();
        transferDlg_->activateWindow();

        const QRect endRect = transferDlg_->geometry();
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

bool MainWindow::eventFilter(QObject *obj, QEvent *ev) {
    QObject *const rightViewport = rightView_ ? rightView_->viewport() : nullptr;
    QObject *const leftViewport = leftView_ ? leftView_->viewport() : nullptr;
    if (obj != rightViewport && obj != leftViewport)
        return QMainWindow::eventFilter(obj, ev);

    // Drag-and-drop over the right panel (local or remote)
    if (obj == rightViewport) {
        if (ev->type() == QEvent::DragEnter) {
            auto *de = static_cast<QDragEnterEvent *>(ev);
            if (rightIsRemote_ && !rightRemoteWritable_) {
                de->ignore();
                return true;
            }
            de->acceptProposedAction();
            return true;
        } else if (ev->type() == QEvent::DragMove) {
            auto *dm = static_cast<QDragMoveEvent *>(ev);
            if (rightIsRemote_ && !rightRemoteWritable_) {
                dm->ignore();
                return true;
            }
            dm->acceptProposedAction();
            return true;
        } else if (ev->type() == QEvent::Drop) {
            auto *dd = static_cast<QDropEvent *>(ev);
            const auto urls =
                dd->mimeData() ? dd->mimeData()->urls() : QList<QUrl>{};
            if (urls.isEmpty()) {
                dd->acceptProposedAction();
                return true;
            }
            if (rightIsRemote_) {
                // Block upload if remote is read-only
                if (!rightRemoteWritable_) {
                    statusBar()->showMessage(
                        tr("Remote directory is read-only; cannot upload here"),
                        5000);
                    dd->ignore();
                    return true;
                }
                // Upload to remote
                if (!sftp_ || !rightRemoteModel_) {
                    dd->acceptProposedAction();
                    return true;
                }
                const QString remoteBase = rightRemoteModel_->rootPath();
                int enq = 0;
                for (const QUrl &u : urls) {
                    const QString p = u.toLocalFile();
                    if (p.isEmpty())
                        continue;
                    QFileInfo fi(p);
                    if (fi.isDir()) {
                        QDirIterator it(p,
                                        QDir::NoDotAndDotDot | QDir::AllEntries,
                                        QDirIterator::Subdirectories);
                        while (it.hasNext()) {
                            it.next();
                            if (!it.fileInfo().isFile())
                                continue;
                            const QString rel =
                                QDir(p).relativeFilePath(it.filePath());
                            const QString rTarget =
                                joinRemotePath(remoteBase, rel);
                            transferMgr_->enqueueUpload(it.filePath(), rTarget);
                            ++enq;
                        }
                    } else if (fi.isFile()) {
                        const QString rTarget =
                            joinRemotePath(remoteBase, fi.fileName());
                        transferMgr_->enqueueUpload(fi.absoluteFilePath(),
                                                    rTarget);
                        ++enq;
                    }
                }
                if (enq > 0) {
                    statusBar()->showMessage(
                        QString(tr("Queued: %1 uploads (DND)")).arg(enq), 4000);
                    maybeShowTransferQueue();
                }
                dd->acceptProposedAction();
                return true;
            } else {
                // Local copy to the right panel directory
                QDir dst(rightPath_->text());
                if (!dst.exists()) {
                    dd->acceptProposedAction();
                    return true;
                }
                QVector<LocalFsPair> pairs;
                for (const QUrl &u : urls) {
                    const QString p = u.toLocalFile();
                    if (p.isEmpty())
                        continue;
                    QFileInfo fi(p);
                    const QString target = dst.filePath(fi.fileName());
                    // Avoid copying onto itself if same directory/file
                    if (fi.absoluteFilePath() == target) {
                        continue;
                    }
                    pairs.push_back({fi.absoluteFilePath(), target});
                }
                runLocalFsOperation(pairs, false, 0);
                dd->acceptProposedAction();
                return true;
            }
        }
    }
    // Drag-and-drop over the left panel (local): copy/download
    // Update delete shortcut enablement if selection changes due to DnD or
    // click
    if (obj == leftViewport) {
        if (ev->type() == QEvent::DragEnter) {
            auto *de = static_cast<QDragEnterEvent *>(ev);
            de->acceptProposedAction();
            return true;
        } else if (ev->type() == QEvent::DragMove) {
            auto *dm = static_cast<QDragMoveEvent *>(ev);
            dm->acceptProposedAction();
            return true;
        } else if (ev->type() == QEvent::Drop) {
            auto *dd = static_cast<QDropEvent *>(ev);
            const auto urls =
                dd->mimeData() ? dd->mimeData()->urls() : QList<QUrl>{};
            if (!urls.isEmpty()) {
                // Local copy towards the left panel
                QDir dst(leftPath_->text());
                if (!dst.exists()) {
                    dd->acceptProposedAction();
                    return true;
                }
                QVector<LocalFsPair> pairs;
                for (const QUrl &u : urls) {
                    const QString p = u.toLocalFile();
                    if (p.isEmpty())
                        continue;
                    QFileInfo fi(p);
                    const QString target = dst.filePath(fi.fileName());
                    // Avoid self-drop: same file/folder and same destination
                    if (fi.absoluteFilePath() == target) {
                        continue;
                    }
                    pairs.push_back({fi.absoluteFilePath(), target});
                }
                runLocalFsOperation(pairs, false, 0);
                dd->acceptProposedAction();
                updateDeleteShortcutEnables();
                return true;
            }
            // Download from remote (based on right panel selection)
            if (rightIsRemote_ == true && rightView_ && rightRemoteModel_) {
                auto sel = rightView_->selectionModel();
                if (!sel || sel->selectedRows(NAME_COL).isEmpty()) {
                    dd->acceptProposedAction();
                    return true;
                }
                const auto rows = sel->selectedRows(NAME_COL);
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
                    QString rpath = remoteBase;
                    if (!rpath.endsWith('/'))
                        rpath += '/';
                    rpath += name;
                    const QString lpath = dst.filePath(name);
                    seeds.push_back(
                        {rpath, lpath, rightRemoteModel_->isDir(idx)});
                }
                runRemoteDownloadPrescan(seeds, bad, true);
                dd->acceptProposedAction();
                updateDeleteShortcutEnables();
                return true;
            }
        }
    }
    return QMainWindow::eventFilter(obj, ev);
}
