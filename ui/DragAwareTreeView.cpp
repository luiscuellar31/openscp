// Implementation of DragAwareTreeView
#include "DragAwareTreeView.hpp"
#include "RemoteModel.hpp"
#include "TransferManager.hpp"
#include "UiAlerts.hpp"
#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDesktopServices>
#include <QDir>
#include <QDrag>
#include <QFileInfo>
#include <QKeySequence>
#include <QLabel>
#include <QLocale>
#include <QLoggingCategory>
#include <QMainWindow>
#include <QMessageBox>
#include <QMimeData>
#include <QPointer>
#include <QProgressBar>
#include <QPushButton>
#include <QSet>
#include <QSettings>
#include <QSharedPointer>
#include <QShortcut>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>
#include <QVector>
Q_DECLARE_LOGGING_CATEGORY(ocEnum)

Q_LOGGING_CATEGORY(ocDrag, "openscp.drag")

static QString stagingRootFromSettings() {
    QSettings s("OpenSCP", "OpenSCP");
    QString root = s.value("Advanced/stagingRoot").toString();
    if (root.isEmpty()) {
        root = QDir::homePath() + "/Downloads/OpenSCP-Dragged";
    }
    return root;
}

DragAwareTreeView::DragAwareTreeView(QWidget *parent) : QTreeView(parent) {}

DragAwareTreeView::~DragAwareTreeView() {
    if (!currentBatchDir_.isEmpty() || (overlay_ && overlay_->isVisible())) {
        cancelCurrentBatch(QStringLiteral("dtor"));
    }
}

void DragAwareTreeView::resizeEvent(QResizeEvent *e) {
    QTreeView::resizeEvent(e);
    updateOverlayGeometry();
}

void DragAwareTreeView::startDrag(Qt::DropActions supportedActions) {
    if (dragInProgress_) {
        if (auto *mw = qobject_cast<QMainWindow *>(window())) {
            mw->statusBar()->showMessage(
                tr("Preparation in progress; please wait."), 3000);
        }
        return;
    }
    // If this is a remote model, run asynchronous staging flow
    if (auto *rm = qobject_cast<RemoteModel *>(model())) {
        startRemoteDragAsync(rm);
        return;
    }

    // Fallback to default behavior (local models)
    QModelIndexList indexes = selectedIndexes();
    if (indexes.isEmpty()) {
        QTreeView::startDrag(supportedActions);
        return;
    }
    QMimeData *md = model()->mimeData(indexes);
    if (!md) {
        QTreeView::startDrag(supportedActions);
        return;
    }
    auto drag = new QDrag(this);
    drag->setMimeData(md);
    const auto res = drag->exec(Qt::CopyAction);

    // Custom payload from RemoteModel when dragging out from remote
    const QByteArray k =
        QByteArrayLiteral("application/x-openscp-staging-batch");
    if (!md->hasFormat(k))
        return; // nothing to do
    const QString batchDir = QString::fromUtf8(md->data(k));
    if (batchDir.isEmpty())
        return;

    QSettings s("OpenSCP", "OpenSCP");
    const bool autoClean = s.value("Advanced/autoCleanStaging", true).toBool();

    if (res == Qt::IgnoreAction) {
        // Drag canceled: keep staging and notify with clickable link
        showKeepMessage(batchDir);
        return;
    }

    if (!autoClean) {
        showKeepMessage(batchDir);
        return;
    }

    // Success path: schedule retries to delete batch dir and maybe root
    scheduleAutoCleanup(batchDir);
}

void DragAwareTreeView::showKeepMessage(const QString &batchDir) {
    auto *mw = qobject_cast<QMainWindow *>(window());
    if (!mw)
        return;
    auto *lbl = new QLabel(mw);
    lbl->setTextFormat(Qt::RichText);
    const QString url = QUrl::fromLocalFile(batchDir).toString();
    lbl->setText(
        tr("Staging kept at: <a href=\"%1\">%2</a>").arg(url, batchDir));
    lbl->setOpenExternalLinks(true);
    lbl->setCursor(Qt::PointingHandCursor);
    mw->statusBar()->addPermanentWidget(lbl, 0);
    QTimer::singleShot(10000, lbl, [mw, lbl] {
        mw->statusBar()->removeWidget(lbl);
        lbl->deleteLater();
    });
}

void DragAwareTreeView::showKeepMessageWithPrefix(const QString &prefix,
                                                  const QString &batchDir) {
    auto *mw = qobject_cast<QMainWindow *>(window());
    if (!mw)
        return;
    auto *lbl = new QLabel(mw);
    lbl->setTextFormat(Qt::RichText);
    const QString url = QUrl::fromLocalFile(batchDir).toString();
    const QString txt =
        QString("%1 <a href=\"%2\">%3</a>").arg(prefix, url, batchDir);
    lbl->setText(txt);
    lbl->setOpenExternalLinks(true);
    lbl->setCursor(Qt::PointingHandCursor);
    mw->statusBar()->addPermanentWidget(lbl, 0);
    QTimer::singleShot(12000, lbl, [mw, lbl] {
        mw->statusBar()->removeWidget(lbl);
        lbl->deleteLater();
    });
}

void DragAwareTreeView::scheduleAutoCleanup(const QString &batchDir) {
    auto tryDelete = [this](const QString &dir) -> bool {
        QDir d(dir);
        if (!d.exists())
            return true;
        bool ok = d.removeRecursively();
        if (!ok)
            return false;
        // If root empty, remove it
        QString root = stagingRootFromSettings();
        QDir rt(root);
        if (rt.exists() &&
            rt.entryList(QDir::NoDotAndDotDot | QDir::AllEntries).isEmpty()) {
            rt.rmdir(".");
        }
        return true;
    };

    // First attempt with a brief delay to allow OS to finish copying
    QTimer::singleShot(500, this, [this, batchDir, tryDelete]() {
        if (tryDelete(batchDir))
            return;
        // Retry up to 3 times at 1s intervals
        auto retries = new int(3);
        auto timer = new QTimer(this);
        timer->setInterval(1000);
        connect(timer, &QTimer::timeout, this,
                [this, timer, retries, batchDir, tryDelete]() {
                    if (tryDelete(batchDir) || --(*retries) <= 0) {
                        timer->stop();
                        timer->deleteLater();
                        delete retries;
                    }
                });
        timer->start();
    });
}

QString DragAwareTreeView::buildStagingRoot() const {
    QSettings s("OpenSCP", "OpenSCP");
    QString root = s.value("Advanced/stagingRoot").toString();
    if (root.isEmpty())
        root = QDir::homePath() + "/Downloads/OpenSCP-Dragged";
    return root;
}

void DragAwareTreeView::showPrepOverlay(const QString &text) {
    if (!overlay_) {
        overlay_ = new QWidget(viewport());
        overlay_->setAutoFillBackground(true);
        overlay_->setStyleSheet("background: rgba(0,0,0,0.35); border: 1px "
                                "solid rgba(255,255,255,0.25);");
        overlay_->setAccessibleName(QStringLiteral("Preparing files overlay"));
        overlayLabel_ = new QLabel(overlay_);
        overlayLabel_->setStyleSheet("color: white; font-weight: 600;");
        overlayProgress_ = new QProgressBar(overlay_);
        overlayProgress_->setRange(0, 100);
        overlayCancel_ = new QPushButton(tr("Cancel"), overlay_);
        overlayCancel_->setCursor(Qt::PointingHandCursor);
        // ESC shortcut to cancel staging
        overlayEsc_ = new QShortcut(QKeySequence(Qt::Key_Escape), overlay_);
        QObject::connect(overlayEsc_, &QShortcut::activated, this,
                         [this] { cancelCurrentBatch(QStringLiteral("esc")); });
        // Simple geometry without layouts to avoid layout warnings inside
        // viewport
    }
    overlayLabel_->setText(text);
    updateOverlayGeometry();
    overlay_->show();
    // Ensure we cancel batch on app quit
    if (!quitConn_) {
        quitConn_ = QObject::connect(
            qApp, &QCoreApplication::aboutToQuit, this,
            [this] { cancelCurrentBatch(QStringLiteral("quit")); });
    }
}

void DragAwareTreeView::hidePrepOverlay() {
    if (overlay_)
        overlay_->hide();
}

void DragAwareTreeView::updateOverlayGeometry() {
    if (!overlay_)
        return;
    const QRect r = viewport() ? viewport()->rect() : rect();
    overlay_->setGeometry(r.adjusted(r.width() / 6, r.height() / 3,
                                     -r.width() / 6, -r.height() / 3));
    // Place controls vertically: label, progress, cancel
    if (overlayLabel_ && overlayProgress_ && overlayCancel_) {
        int w = overlay_->width();
        int x = 16;
        int y = 16;
        overlayLabel_->setGeometry(x, y, w - 32, 28);
        y += 34;
        overlayProgress_->setGeometry(x, y, w - 32, 22);
        y += 30;
        overlayCancel_->setGeometry(x, y, 120, 28);
    }
}

void DragAwareTreeView::startRemoteDragAsync(RemoteModel *rm) {
    if (!rm || !transferMgr_) {
        QTreeView::startDrag(Qt::CopyAction);
        return;
    }
    if (dragInProgress_)
        return;
    dragInProgress_ = true;

    // Collect selected rows (name column only)
    auto sel = selectionModel();
    const auto rows = sel ? sel->selectedRows(0) : QModelIndexList{};
    if (rows.isEmpty())
        return;

    // Compute staging directory
    const QString root = buildStagingRoot();
    const QString stamp =
        QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss");
    const QString staging = QDir(root).filePath(stamp);
    currentBatchDir_ = staging;
    currentBatchId_ = stamp;
    batchLogged_ = false;
    QDir().mkpath(staging);

    // Build unique local path helper (for top-level files; folders preserve
    // structure) Normalize to NFC so names are stable across APFS/HFS+
    // normalization differences (macOS filesystems may store NFD; we normalize
    // for consistent collision checks).
    auto nfc = [](const QString &s) {
#if defined(Q_OS_MAC)
        return s.normalized(QString::NormalizationForm_C);
#else
        return s;
#endif
    };
    // Insert suffix before the first extension to keep multi-extensions intact
    // Example: "archive.tar.gz" -> base "archive" + extSuffix ".tar.gz".
    auto splitNameMultiExt = [](const QString &fname) {
        // Place suffix before the first dot after leading char
        int firstDot = fname.indexOf('.', 1);
        if (firstDot <= 0)
            return qMakePair(fname, QString());
        return qMakePair(
            fname.left(firstDot),
            fname.mid(firstDot)); // base, extSuffix including dot(s)
    };
    auto uniquePath = [&](const QString &dir, const QString &name) {
        const QString nName = nfc(name);
        auto parts = splitNameMultiExt(nName);
        QString baseName = parts.first;
        QString extSuffix = parts.second; // includes leading dot(s) or empty
        QString candidate = QDir(dir).filePath(nName);
        int i = 1;
        while (QFileInfo::exists(candidate)) {
            candidate = QDir(dir).filePath(
                QString("%1 (%2)%3").arg(baseName).arg(i++).arg(extSuffix));
        }
        return candidate;
    };
    auto joinRemote = [](const QString &base, const QString &name) {
        if (base == "/")
            return QStringLiteral("/") + name;
        return base.endsWith('/') ? base + name : base + "/" + name;
    };

    // Show overlay early to cover potentially heavy folder enumeration
    showPrepOverlay(tr("Preparing files…"));

    // Prepare list of downloads (support files and directories)
    struct Pair {
        QString remote;
        QString local;
        quint64 size = 0;
    };
    QVector<Pair> targets;
    targets.reserve(rows.size());
    quint64 totalBytes = 0;
    quint64 totalItems = 0;
    quint64 totalDirs = 0;
    enumCancelFlag_ = std::make_shared<std::atomic_bool>(false);
    enumSymlinksSkipped_ = 0;
    enumDenied_ = 0;
    quint64 unknownSizeCount = 0;
    prepTimer_.restart();
    bool anySizeUnknown = false;
    for (const QModelIndex &idx : rows) {
        if (enumCancelFlag_ &&
            enumCancelFlag_->load(std::memory_order_relaxed)) {
            dragInProgress_ = false;
            return;
        }
        if (!idx.isValid())
            continue;
        const QString name = rm->nameAt(idx);
        const QString rpath = joinRemote(rm->rootPath(), name);
        if (rm->isDir(idx)) {
            // Enumerate directory recursively
            std::vector<RemoteModel::EnumeratedFile> files;
            QString e;
            RemoteModel::EnumOptions opt;
            QSettings s("OpenSCP", "OpenSCP");
            opt.maxDepth = s.value("Advanced/maxFolderDepth", 32).toInt();
            if (opt.maxDepth < 1)
                opt.maxDepth = 32;
            opt.skipSymlinks =
                true; // per requirements: skip symlinks by default
            opt.cancel = enumCancelFlag_.get();
            bool partial = false;
            bool someUnknown = false;
            quint64 dirCount = 0;
            quint64 syms = 0;
            quint64 denied = 0;
            quint64 unkCountPart = 0;
            rm->enumerateFilesUnderEx(rpath, files, opt, &partial, &someUnknown,
                                      &dirCount, &syms, &denied, &unkCountPart);
            if (someUnknown)
                anySizeUnknown = true;
            totalDirs += dirCount;
            enumSymlinksSkipped_ += syms;
            enumDenied_ += denied;
            unknownSizeCount += unkCountPart;
            // Map to local targets under staging/name/relative
            // Map to local targets under staging/name/relative
            for (const auto &f : files) {
                if (enumCancelFlag_ &&
                    enumCancelFlag_->load(std::memory_order_relaxed)) {
                    dragInProgress_ = false;
                    return;
                }
                const QString local = QDir(QDir(staging).filePath(nfc(name)))
                                          .filePath(nfc(f.relativePath));
                QDir().mkpath(QFileInfo(local).dir().absolutePath());
                targets.push_back(
                    {f.remotePath, local, f.hasSize ? f.size : 0});
                if (f.hasSize)
                    totalBytes += f.size; // unknown sizes not counted
            }
            totalItems += (int)files.size();
        } else {
            const QString lpath = uniquePath(staging, name);
            // Pick up size info (if available) from the model for single files
            quint64 sz = 0;
            bool hsz = false;
            if (rm->hasSize(idx)) {
                hsz = true;
                sz = rm->sizeAt(idx);
            }
            if (hsz)
                totalBytes += sz;
            else {
                anySizeUnknown = true;
                unknownSizeCount += 1;
            }
            targets.push_back({rpath, lpath, sz});
            totalItems += 1;
        }
    }
    if (enumCancelFlag_ && enumCancelFlag_->load(std::memory_order_relaxed)) {
        dragInProgress_ = false;
        return;
    }
    if (targets.isEmpty()) {
        dragInProgress_ = false;
        return;
    }
    currentBatchTotal_ = targets.size();

    // Confirmation for very large batches
    QSettings dragSettings("OpenSCP", "OpenSCP");
    int confirmItemsThreshold =
        dragSettings.value("Advanced/stagingConfirmItems", 500).toInt();
    if (confirmItemsThreshold < 1)
        confirmItemsThreshold = 500;
    int confirmMiBThreshold =
        dragSettings.value("Advanced/stagingConfirmMiB", 1024).toInt();
    if (confirmMiBThreshold < 1)
        confirmMiBThreshold = 1024;
    const quint64 confirmBytesThreshold =
        quint64(confirmMiBThreshold) * 1024ull * 1024ull;
    const bool tooMany = totalItems > confirmItemsThreshold;
    const bool tooBig = totalBytes > confirmBytesThreshold;
    const qint64 enumMs = prepTimer_.elapsed();
    enumMs_ = enumMs;
    // Log enumeration summary (openscp.enum)
    QString bytesText = QLocale().formattedDataSize((qint64)totalBytes, 1,
                                                    QLocale::DataSizeIecFormat);
    if (anySizeUnknown)
        bytesText = QString("~%1 (%2)")
                        .arg(bytesText)
                        .arg(QLocale().toString((qulonglong)unknownSizeCount));
    qInfo(ocEnum) << "enum batch" << currentBatchId_ << "dirs"
                  << QLocale().toString((qulonglong)totalDirs) << "files"
                  << QLocale().toString((qulonglong)totalItems) << "bytes"
                  << bytesText << "enumMs" << enumMs_ << "threshold"
                  << ((tooMany || tooBig) ? "yes" : "no") << "symlinkSkipped"
                  << QLocale().toString((qulonglong)enumSymlinksSkipped_)
                  << "denied" << QLocale().toString((qulonglong)enumDenied_);
    if (tooMany || tooBig) {
        auto *mw = qobject_cast<QMainWindow *>(window());
        QWidget *parent =
            mw ? static_cast<QWidget *>(mw) : static_cast<QWidget *>(this);
        QMessageBox box(parent);
        UiAlerts::configure(box);
        box.setIcon(QMessageBox::Question);
        box.setWindowTitle(tr("Confirm staging"));
        QString sizePart;
        if (tooBig) {
            sizePart =
                anySizeUnknown
                    ? QString(" (%1)").arg(tr("~%1 (some unknown)")
                                               .arg(QLocale().formattedDataSize(
                                                   (qint64)totalBytes, 1,
                                                   QLocale::DataSizeIecFormat)))
                    : QString(" (%1)").arg(QLocale().formattedDataSize(
                          (qint64)totalBytes, 1, QLocale::DataSizeIecFormat));
        }
        QString detail = tr("You are about to prepare %1 items%2. Continue?")
                             .arg(QLocale().toString((qulonglong)totalItems))
                             .arg(sizePart);
        box.setText(detail);
        auto *yesBtn = box.addButton(tr("Continue"), QMessageBox::AcceptRole);
        auto *cancelBtn = box.addButton(tr("Cancel"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() != yesBtn) {
            cancelCurrentBatch(QStringLiteral("threshold"));
            return;
        }
    }

    // Resolve collisions before enqueueing and ensure directories exist
    // Per-batch reservation to avoid intra-batch races. O(1) QSet with
    // NFC+case-fold keys on macOS/Windows to mimic case-insensitive semantics.
    QSet<QString> reserved;
    auto keyFor = [&](const QString &abs) {
        QString k = nfc(QDir::cleanPath(abs)); // canonicalized path string
#if defined(Q_OS_MAC) || defined(Q_OS_WIN)
        k = k.toLower(); // case-insensitive match on these platforms
#endif
        return k;
    };
    auto uniqueFullPath = [&](const QString &full) {
        QFileInfo fi(full);
        const QString dir = fi.dir().absolutePath();
        const QString origName = fi.fileName();
        const QString nName = nfc(origName);
        auto parts = splitNameMultiExt(nName);
        QString base = parts.first;
        QString extSuffix = parts.second;

#if defined(Q_OS_MAC) || defined(Q_OS_WIN)
        // On macOS (APFS/HFS+) and Windows (NTFS) user-facing semantics are
        // often case-insensitive. Build a lowercase set so that "File.txt" and
        // "file.txt" collide predictably.
        QDir d(dir);
        const auto entries =
            d.entryList(QDir::NoDotAndDotDot | QDir::AllEntries);
        QSet<QString> lower;
        for (const QString &n : entries)
            lower.insert(nfc(n).toLower());
        auto buildName = [&](int idx) -> QString {
            if (idx == 0)
                return nName;
            return QString("%1 (%2)%3").arg(base).arg(idx).arg(extSuffix);
        };
        int i = 0;
        QString name = buildName(i);
        QString abs = QDir(dir).filePath(name);
        QString key = keyFor(abs);
        while (lower.contains(nfc(name).toLower()) || reserved.contains(key) ||
               QFileInfo::exists(abs)) {
            name = buildName(++i);
            abs = QDir(dir).filePath(name);
            key = keyFor(abs);
        }
        reserved.insert(key);
        return abs;
#else
        auto buildName = [&](int idx) -> QString {
            if (idx == 0)
                return nName;
            return QString("%1 (%2)%3").arg(base).arg(idx).arg(extSuffix);
        };
        int i = 0;
        QString name = buildName(i);
        QString abs = QDir(dir).filePath(name);
        QString key = keyFor(abs);
        while (reserved.contains(key) || QFileInfo::exists(abs)) {
            name = buildName(++i);
            abs = QDir(dir).filePath(name);
            key = keyFor(abs);
        }
        reserved.insert(key);
        return abs;
#endif
    };
    for (auto &p : targets) {
        QDir().mkpath(QFileInfo(p.local).dir().absolutePath());
        p.local = uniqueFullPath(p.local);
        transferMgr_->enqueueDownload(p.remote, p.local);
    }
    transferMgr_->resumeAll();
    overlayProgress_->setValue(0);
    stagingTimer_.restart();

    // Track progress of our batch (identified by local dst prefix = staging)
    QPointer<DragAwareTreeView> self(this);
    // Cancel button handler: cancel only our tasks
    QObject::connect(overlayCancel_, &QPushButton::clicked, this,
                     [this] { cancelCurrentBatch(QStringLiteral("button")); });

    // If preparation takes more than 2s, offer Wait/Cancel dialog once
    if (!waitTimer_)
        waitTimer_ = new QTimer(this);
    waitTimer_->setSingleShot(true);
    QSettings s("OpenSCP", "OpenSCP");
    int timeoutMs = s.value("Advanced/stagingPrepTimeoutMs", 2000).toInt();
    timeoutMs = qBound(250, timeoutMs, 60000);
    waitTimer_->setInterval(timeoutMs);
    QObject::connect(waitTimer_, &QTimer::timeout, this, [this]() {
        auto *mw = qobject_cast<QMainWindow *>(window());
        if (!mw)
            return;
        QMessageBox box(mw);
        UiAlerts::configure(box);
        box.setIcon(QMessageBox::Information);
        box.setWindowTitle(tr("Preparing files…"));
        box.setText(tr("Still preparing files for drag-out. Wait or cancel?"));
        auto *waitBtn = box.addButton(tr("Wait"), QMessageBox::AcceptRole);
        auto *cancelBtn = box.addButton(tr("Cancel"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == cancelBtn) {
            cancelCurrentBatch(QStringLiteral("dialog"));
        }
    });
    waitTimer_->start();

    // Connect to tasksChanged to monitor our batch
    stagingConn_ = QObject::connect(
        transferMgr_, &TransferManager::tasksChanged, this,
        [this, self, targets, totalDirs, totalItems]() mutable {
            if (!self)
                return;
            if (!transferMgr_)
                return;
            int total = targets.size();
            int done = 0;
            int failed = 0;
            const auto tasks = transferMgr_->tasksSnapshot();
            for (const auto &p : targets) {
                bool matched = false;
                for (const auto &t : tasks) {
                    if (t.type == TransferTask::Type::Download &&
                        t.dst == p.local) {
                        matched = true;
                        if (t.status == TransferTask::Status::Done)
                            done++;
                        else if (t.status == TransferTask::Status::Error ||
                                 t.status == TransferTask::Status::Canceled)
                            failed++;
                        break;
                    }
                }
                if (!matched) {
                    // still queued but not yet visible; treat as pending
                }
            }
            int pct = (total > 0) ? int((done * 100) / total) : 0;
            if (overlayProgress_)
                overlayProgress_->setValue(pct);

            if ((done + failed) >= total) {
                // Finished staging
                if (stagingConn_) {
                    QObject::disconnect(stagingConn_);
                    stagingConn_ = QMetaObject::Connection();
                }
                hidePrepOverlay();

                if (failed > 0) {
                    // Keep staging and show clickable status-bar message with
                    // counts
                    QString prefix = tr("%1 of %2 files failed. Staging at:")
                                         .arg(failed)
                                         .arg(total);
                    showKeepMessageWithPrefix(prefix, currentBatchDir_);
                    const qint64 stagingMs =
                        stagingTimer_.isValid() ? stagingTimer_.elapsed() : -1;
                    logBatchResult(
                        currentBatchId_, total, failed,
                        QString("result=partial-fail enumDirs=%1 files=%2 "
                                "bytes=%3 enumMs=%4 stagingMs=%5 "
                                "symlinkSkipped=%6 denied=%7")
                            .arg(QLocale().toString((qulonglong)totalDirs))
                            .arg(QLocale().toString((qulonglong)totalItems))
                            .arg(QLocale().toString((qulonglong)0))
                            .arg(enumMs_)
                            .arg(stagingMs)
                            .arg(QLocale().toString(
                                (qulonglong)enumSymlinksSkipped_))
                            .arg(QLocale().toString((qulonglong)enumDenied_)));
                    dragInProgress_ = false;
                    currentBatchDir_.clear();
                    currentBatchId_.clear();
                    currentBatchTotal_ = 0;
                    return;
                }

                // Build MIME and start the actual drag now that files are ready
                QList<QUrl> urls;
                urls.reserve(targets.size());
                for (const auto &p : targets)
                    urls << QUrl::fromLocalFile(p.local);
                auto *md = new QMimeData();
                md->setUrls(urls);
                md->setData("application/x-openscp-staging-batch",
                            currentBatchDir_.toUtf8());

                auto *drag = new QDrag(self);
                drag->setMimeData(md);
                const auto res = drag->exec(Qt::CopyAction);

                // Post-drag behavior: cleanup or keep
                QSettings s("OpenSCP", "OpenSCP");
                const bool autoClean =
                    s.value("Advanced/autoCleanStaging", true).toBool();
                if (res == Qt::IgnoreAction) {
                    showKeepMessage(currentBatchDir_);
                    const qint64 stagingMs =
                        stagingTimer_.isValid() ? stagingTimer_.elapsed() : -1;
                    logBatchResult(
                        currentBatchId_, total, 0,
                        QString(
                            "result=canceled enumDirs=%1 files=%2 enumMs=%3 "
                            "stagingMs=%4 symlinkSkipped=%5 denied=%6")
                            .arg(QLocale().toString((qulonglong)totalDirs))
                            .arg(QLocale().toString((qulonglong)totalItems))
                            .arg(enumMs_)
                            .arg(stagingMs)
                            .arg(QLocale().toString(
                                (qulonglong)enumSymlinksSkipped_))
                            .arg(QLocale().toString((qulonglong)enumDenied_)));
                    dragInProgress_ = false;
                    currentBatchDir_.clear();
                    currentBatchId_.clear();
                    currentBatchTotal_ = 0;
                    if (quitConn_) {
                        QObject::disconnect(quitConn_);
                        quitConn_ = QMetaObject::Connection();
                    }
                    return;
                }
                if (!autoClean) {
                    showKeepMessage(currentBatchDir_);
                    const qint64 stagingMs =
                        stagingTimer_.isValid() ? stagingTimer_.elapsed() : -1;
                    logBatchResult(
                        currentBatchId_, total, 0,
                        QString(
                            "result=accepted enumDirs=%1 files=%2 enumMs=%3 "
                            "stagingMs=%4 symlinkSkipped=%5 denied=%6")
                            .arg(QLocale().toString((qulonglong)totalDirs))
                            .arg(QLocale().toString((qulonglong)totalItems))
                            .arg(enumMs_)
                            .arg(stagingMs)
                            .arg(QLocale().toString(
                                (qulonglong)enumSymlinksSkipped_))
                            .arg(QLocale().toString((qulonglong)enumDenied_)));
                    dragInProgress_ = false;
                    currentBatchDir_.clear();
                    currentBatchId_.clear();
                    currentBatchTotal_ = 0;
                    if (quitConn_) {
                        QObject::disconnect(quitConn_);
                        quitConn_ = QMetaObject::Connection();
                    }
                    return;
                }
                scheduleAutoCleanup(currentBatchDir_);
                {
                    const qint64 stagingMs =
                        stagingTimer_.isValid() ? stagingTimer_.elapsed() : -1;
                    logBatchResult(
                        currentBatchId_, total, 0,
                        QString(
                            "result=accepted enumDirs=%1 files=%2 enumMs=%3 "
                            "stagingMs=%4 symlinkSkipped=%5 denied=%6")
                            .arg(QLocale().toString((qulonglong)totalDirs))
                            .arg(QLocale().toString((qulonglong)totalItems))
                            .arg(enumMs_)
                            .arg(stagingMs)
                            .arg(QLocale().toString(
                                (qulonglong)enumSymlinksSkipped_))
                            .arg(QLocale().toString((qulonglong)enumDenied_)));
                }
                dragInProgress_ = false;
                currentBatchDir_.clear();
                currentBatchId_.clear();
                currentBatchTotal_ = 0;
                if (quitConn_) {
                    QObject::disconnect(quitConn_);
                    quitConn_ = QMetaObject::Connection();
                }
            }
        });
}

void DragAwareTreeView::cancelCurrentBatch(const QString &reason) {
    if (currentBatchDir_.isEmpty()) {
        hidePrepOverlay();
        dragInProgress_ = false;
        return;
    }
    if (stagingConn_) {
        QObject::disconnect(stagingConn_);
        stagingConn_ = QMetaObject::Connection();
    }
    if (waitTimer_)
        waitTimer_->stop();
    if (enumCancelFlag_)
        enumCancelFlag_->store(true, std::memory_order_relaxed);
    hidePrepOverlay();
    if (transferMgr_) {
        const auto tasks = transferMgr_->tasksSnapshot();
        for (const auto &t : tasks) {
            if (t.type == TransferTask::Type::Download &&
                t.dst.startsWith(currentBatchDir_)) {
                transferMgr_->cancelTask(t.id);
            }
        }
    }
    showKeepMessage(currentBatchDir_);
    // Ensure final outcome includes symlinkSkipped/denied counters even on
    // manual cancel
    const qint64 stagingMs =
        stagingTimer_.isValid() ? stagingTimer_.elapsed() : -1;
    logBatchResult(
        currentBatchId_, currentBatchTotal_, 0,
        QString("result=canceled enumMs=%1 stagingMs=%2 symlinkSkipped=%3 "
                "denied=%4 (%5)")
            .arg(enumMs_)
            .arg(stagingMs)
            .arg(QLocale().toString((qulonglong)enumSymlinksSkipped_))
            .arg(QLocale().toString((qulonglong)enumDenied_))
            .arg(reason));
    dragInProgress_ = false;
    currentBatchDir_.clear();
    currentBatchId_.clear();
    currentBatchTotal_ = 0;
    if (quitConn_) {
        QObject::disconnect(quitConn_);
        quitConn_ = QMetaObject::Connection();
    }
}

void DragAwareTreeView::logBatchResult(const QString &batchId, int totalItems,
                                       int failedItems, const QString &result) {
    if (batchLogged_)
        return;
    batchLogged_ = true;
    qInfo(ocDrag) << "batch" << batchId << "items" << totalItems << "failed"
                  << failedItems << result;
}

void DragAwareTreeView::closeEvent(QCloseEvent *e) {
    if (overlay_ && overlay_->isVisible()) {
        cancelCurrentBatch(QStringLiteral("close"));
    }
    QTreeView::closeEvent(e);
}
