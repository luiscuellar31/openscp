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

static QString normalizeStagingName(const QString &value) {
#if defined(Q_OS_MAC)
    return value.normalized(QString::NormalizationForm_C);
#else
    return value;
#endif
}

static QPair<QString, QString> splitNameMultiExt(const QString &fileName) {
    const int firstDot = fileName.indexOf('.', 1);
    if (firstDot <= 0)
        return qMakePair(fileName, QString());
    return qMakePair(fileName.left(firstDot), fileName.mid(firstDot));
}

static QPair<int, quint64> loadStagingConfirmThresholds() {
    QSettings settings("OpenSCP", "OpenSCP");
    int itemThreshold = settings.value("Advanced/stagingConfirmItems", 500).toInt();
    if (itemThreshold < 1)
        itemThreshold = 500;
    int mibThreshold = settings.value("Advanced/stagingConfirmMiB", 1024).toInt();
    if (mibThreshold < 1)
        mibThreshold = 1024;
    return qMakePair(itemThreshold, quint64(mibThreshold) * 1024ull * 1024ull);
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

void DragAwareTreeView::scheduleAutoCleanup(const QString &batchDir,
                                            int initialDelayMs) {
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

    const int firstDelayMs = qBound(0, initialDelayMs, 60000);
    // First attempt with a brief delay to allow OS to finish copying
    QTimer::singleShot(firstDelayMs, this, [this, batchDir, tryDelete]() {
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

QModelIndexList DragAwareTreeView::collectRemoteSelectedRows() const {
    auto *sel = selectionModel();
    QModelIndexList rows = sel ? sel->selectedRows(0) : QModelIndexList{};
    if (!rows.isEmpty() || !sel)
        return rows;

    // Fallback: when selection behavior is not row-based yet, infer unique rows
    // from any selected column to keep drag responsive.
    const auto selected = sel->selectedIndexes();
    QSet<int> seenRows;
    for (const QModelIndex &idx : selected) {
        if (!idx.isValid() || seenRows.contains(idx.row()))
            continue;
        const QModelIndex row0 = idx.sibling(idx.row(), 0);
        if (!row0.isValid())
            continue;
        rows.push_back(row0);
        seenRows.insert(idx.row());
    }
    return rows;
}

bool DragAwareTreeView::buildRemoteDragTargets(
    RemoteModel *rm, const QModelIndexList &rows, const QString &stagingDir,
    QVector<RemoteDragTarget> &targets, RemoteDragBatchStats &stats) {
    targets.clear();
    targets.reserve(rows.size());
    stats = RemoteDragBatchStats{};

    enumCancelFlag_ = std::make_shared<std::atomic_bool>(false);
    enumSymlinksSkipped_ = 0;
    enumDenied_ = 0;
    enumMs_ = -1;
    prepTimer_.restart();

    auto uniquePath = [&](const QString &dir, const QString &name) {
        const QString normalizedName = normalizeStagingName(name);
        const auto parts = splitNameMultiExt(normalizedName);
        const QString baseName = parts.first;
        const QString extSuffix = parts.second;
        QString candidate = QDir(dir).filePath(normalizedName);
        int suffix = 1;
        while (QFileInfo::exists(candidate)) {
            candidate = QDir(dir).filePath(
                QString("%1 (%2)%3").arg(baseName).arg(suffix++).arg(extSuffix));
        }
        return candidate;
    };
    auto joinRemote = [](const QString &base, const QString &name) {
        if (base == "/")
            return QStringLiteral("/") + name;
        return base.endsWith('/') ? base + name : base + "/" + name;
    };

    for (const QModelIndex &idx : rows) {
        if (enumCancelFlag_ && enumCancelFlag_->load(std::memory_order_relaxed))
            return false;
        if (!idx.isValid())
            continue;

        const QString name = rm->nameAt(idx);
        const QString remotePath = joinRemote(rm->rootPath(), name);
        if (rm->isDir(idx)) {
            std::vector<RemoteModel::EnumeratedFile> files;
            RemoteModel::EnumOptions options;
            QSettings settings("OpenSCP", "OpenSCP");
            options.maxDepth = settings.value("Advanced/maxFolderDepth", 32).toInt();
            if (options.maxDepth < 1)
                options.maxDepth = 32;
            options.skipSymlinks = true;
            options.cancel = enumCancelFlag_.get();

            bool partial = false;
            bool someUnknown = false;
            quint64 dirCount = 0;
            quint64 symlinkCount = 0;
            quint64 deniedCount = 0;
            quint64 unknownCountPart = 0;
            rm->enumerateFilesUnderEx(remotePath, files, options, &partial,
                                      &someUnknown, &dirCount, &symlinkCount,
                                      &deniedCount, &unknownCountPart);

            stats.anySizeUnknown = stats.anySizeUnknown || someUnknown;
            stats.totalDirs += dirCount;
            enumSymlinksSkipped_ += symlinkCount;
            enumDenied_ += deniedCount;
            stats.unknownSizeCount += unknownCountPart;

            for (const auto &file : files) {
                if (enumCancelFlag_ &&
                    enumCancelFlag_->load(std::memory_order_relaxed)) {
                    return false;
                }
                const QString localPath =
                    QDir(QDir(stagingDir).filePath(normalizeStagingName(name)))
                        .filePath(normalizeStagingName(file.relativePath));
                QDir().mkpath(QFileInfo(localPath).dir().absolutePath());
                targets.push_back({file.remotePath, localPath});
                if (file.hasSize)
                    stats.totalBytes += file.size;
            }
            stats.totalItems += static_cast<quint64>(files.size());
            continue;
        }

        const QString localPath = uniquePath(stagingDir, name);
        if (rm->hasSize(idx)) {
            stats.totalBytes += rm->sizeAt(idx);
        } else {
            stats.anySizeUnknown = true;
            stats.unknownSizeCount += 1;
        }
        targets.push_back({remotePath, localPath});
        stats.totalItems += 1;
    }

    if (enumCancelFlag_ && enumCancelFlag_->load(std::memory_order_relaxed))
        return false;
    if (targets.isEmpty())
        return false;

    currentBatchTotal_ = targets.size();
    enumMs_ = prepTimer_.elapsed();

    QString bytesText = QLocale().formattedDataSize(
        static_cast<qint64>(stats.totalBytes), 1, QLocale::DataSizeIecFormat);
    if (stats.anySizeUnknown) {
        bytesText = QString("~%1 (%2)")
                        .arg(bytesText)
                        .arg(QLocale().toString(
                            static_cast<qulonglong>(stats.unknownSizeCount)));
    }

    const auto thresholds = loadStagingConfirmThresholds();
    const bool exceedsThreshold =
        stats.totalItems > static_cast<quint64>(thresholds.first) ||
        stats.totalBytes > thresholds.second;

    qInfo(ocEnum) << "enum batch" << currentBatchId_ << "dirs"
                  << QLocale().toString(static_cast<qulonglong>(stats.totalDirs))
                  << "files"
                  << QLocale().toString(static_cast<qulonglong>(stats.totalItems))
                  << "bytes" << bytesText << "enumMs" << enumMs_ << "threshold"
                  << (exceedsThreshold ? "yes" : "no") << "symlinkSkipped"
                  << QLocale().toString(
                         static_cast<qulonglong>(enumSymlinksSkipped_))
                  << "denied"
                  << QLocale().toString(static_cast<qulonglong>(enumDenied_));
    return true;
}

bool DragAwareTreeView::confirmRemoteDragThreshold(
    const RemoteDragBatchStats &stats) {
    const auto thresholds = loadStagingConfirmThresholds();
    const bool tooMany =
        stats.totalItems > static_cast<quint64>(thresholds.first);
    const bool tooBig = stats.totalBytes > thresholds.second;
    if (!tooMany && !tooBig)
        return true;

    auto *mw = qobject_cast<QMainWindow *>(window());
    QWidget *parent =
        mw ? static_cast<QWidget *>(mw) : static_cast<QWidget *>(this);
    QMessageBox box(parent);
    UiAlerts::configure(box);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Confirm staging"));

    QString sizePart;
    if (tooBig) {
        sizePart = stats.anySizeUnknown
                       ? QString(" (%1)")
                             .arg(tr("~%1 (some unknown)")
                                      .arg(QLocale().formattedDataSize(
                                          static_cast<qint64>(stats.totalBytes),
                                          1, QLocale::DataSizeIecFormat)))
                       : QString(" (%1)")
                             .arg(QLocale().formattedDataSize(
                                 static_cast<qint64>(stats.totalBytes), 1,
                                 QLocale::DataSizeIecFormat));
    }
    box.setText(tr("You are about to prepare %1 items%2. Continue?")
                    .arg(QLocale().toString(
                        static_cast<qulonglong>(stats.totalItems)))
                    .arg(sizePart));
    auto *yesButton = box.addButton(tr("Continue"), QMessageBox::AcceptRole);
    box.addButton(tr("Cancel"), QMessageBox::RejectRole);
    box.exec();
    return box.clickedButton() == yesButton;
}

void DragAwareTreeView::enqueueRemoteDragTargets(
    QVector<RemoteDragTarget> &targets) {
    QSet<QString> reserved;
    auto keyFor = [&](const QString &absPath) {
        QString key = normalizeStagingName(QDir::cleanPath(absPath));
#if defined(Q_OS_MAC) || defined(Q_OS_WIN)
        key = key.toLower();
#endif
        return key;
    };
    auto uniqueFullPath = [&](const QString &fullPath) {
        const QFileInfo fileInfo(fullPath);
        const QString dirPath = fileInfo.dir().absolutePath();
        const QString normalizedName = normalizeStagingName(fileInfo.fileName());
        const auto parts = splitNameMultiExt(normalizedName);
        const QString baseName = parts.first;
        const QString extSuffix = parts.second;

        auto buildName = [&](int suffix) {
            if (suffix == 0)
                return normalizedName;
            return QString("%1 (%2)%3").arg(baseName).arg(suffix).arg(extSuffix);
        };

#if defined(Q_OS_MAC) || defined(Q_OS_WIN)
        QSet<QString> lowerExisting;
        QDir directory(dirPath);
        const auto entries =
            directory.entryList(QDir::NoDotAndDotDot | QDir::AllEntries);
        for (const QString &entryName : entries)
            lowerExisting.insert(normalizeStagingName(entryName).toLower());
#endif

        int suffix = 0;
        QString candidateName = buildName(suffix);
        QString candidatePath = QDir(dirPath).filePath(candidateName);
        QString candidateKey = keyFor(candidatePath);
        while (
#if defined(Q_OS_MAC) || defined(Q_OS_WIN)
            lowerExisting.contains(normalizeStagingName(candidateName).toLower()) ||
#endif
            reserved.contains(candidateKey) || QFileInfo::exists(candidatePath)) {
            candidateName = buildName(++suffix);
            candidatePath = QDir(dirPath).filePath(candidateName);
            candidateKey = keyFor(candidatePath);
        }
        reserved.insert(candidateKey);
        return candidatePath;
    };

    for (auto &target : targets) {
        QDir().mkpath(QFileInfo(target.second).dir().absolutePath());
        target.second = uniqueFullPath(target.second);
        transferMgr_->enqueueDownload(target.first, target.second);
    }
    transferMgr_->resumeAll();
    if (overlayProgress_)
        overlayProgress_->setValue(0);
    stagingTimer_.restart();
}

QString DragAwareTreeView::formatRemoteDragMetrics(
    const QString &result, const RemoteDragBatchStats &stats,
    qint64 stagingMs) const {
    return QString("result=%1 enumDirs=%2 files=%3 enumMs=%4 stagingMs=%5 "
                   "symlinkSkipped=%6 denied=%7")
        .arg(result)
        .arg(QLocale().toString(static_cast<qulonglong>(stats.totalDirs)))
        .arg(QLocale().toString(static_cast<qulonglong>(stats.totalItems)))
        .arg(enumMs_)
        .arg(stagingMs)
        .arg(QLocale().toString(static_cast<qulonglong>(enumSymlinksSkipped_)))
        .arg(QLocale().toString(static_cast<qulonglong>(enumDenied_)));
}

void DragAwareTreeView::resetRemoteDragState() {
    if (waitTimer_)
        waitTimer_->stop();
    dragInProgress_ = false;
    currentBatchDir_.clear();
    currentBatchId_.clear();
    currentBatchTotal_ = 0;
    enumCancelFlag_.reset();
    if (quitConn_) {
        QObject::disconnect(quitConn_);
        quitConn_ = QMetaObject::Connection();
    }
}

void DragAwareTreeView::beginRemoteDragMonitoring(
    const QVector<RemoteDragTarget> &targets, const RemoteDragBatchStats &stats) {
    if (overlayCancel_) {
        QObject::disconnect(overlayCancel_, nullptr, this, nullptr);
        QObject::connect(overlayCancel_, &QPushButton::clicked, this, [this] {
            cancelCurrentBatch(QStringLiteral("button"));
        });
    }

    if (!waitTimer_)
        waitTimer_ = new QTimer(this);
    waitTimer_->setSingleShot(true);
    waitTimer_->stop();
    QObject::disconnect(waitTimer_, nullptr, this, nullptr);

    QSettings settings("OpenSCP", "OpenSCP");
    int timeoutMs = settings.value("Advanced/stagingPrepTimeoutMs", 2000).toInt();
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
        box.addButton(tr("Wait"), QMessageBox::AcceptRole);
        auto *cancelButton = box.addButton(tr("Cancel"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == cancelButton)
            cancelCurrentBatch(QStringLiteral("dialog"));
    });
    waitTimer_->start();

    QPointer<DragAwareTreeView> self(this);
    stagingConn_ = QObject::connect(
        transferMgr_, &TransferManager::tasksChanged, this,
        [this, self, targets, stats]() {
            if (!self || !transferMgr_)
                return;

            const auto tasks = transferMgr_->tasksSnapshot();
            const int total = targets.size();
            int done = 0;
            int failed = 0;
            for (const auto &target : targets) {
                for (const auto &task : tasks) {
                    if (task.type != TransferTask::Type::Download ||
                        task.dst != target.second) {
                        continue;
                    }
                    if (task.status == TransferTask::Status::Done)
                        ++done;
                    else if (task.status == TransferTask::Status::Error ||
                             task.status == TransferTask::Status::Canceled) {
                        ++failed;
                    }
                    break;
                }
            }

            if (overlayProgress_) {
                const int progressPct =
                    (total > 0) ? int((done * 100) / total) : 0;
                overlayProgress_->setValue(progressPct);
            }

            if ((done + failed) < total)
                return;

            if (stagingConn_) {
                QObject::disconnect(stagingConn_);
                stagingConn_ = QMetaObject::Connection();
            }
            hidePrepOverlay();

            auto finishBatch = [this, total, stats](const QString &result,
                                                    int failedItems) {
                const qint64 stagingMs =
                    stagingTimer_.isValid() ? stagingTimer_.elapsed() : -1;
                logBatchResult(currentBatchId_, total, failedItems,
                               formatRemoteDragMetrics(result, stats, stagingMs));
                resetRemoteDragState();
            };

            if (failed > 0) {
                const QString prefix =
                    tr("%1 of %2 files failed. Staging at:")
                        .arg(failed)
                        .arg(total);
                showKeepMessageWithPrefix(prefix, currentBatchDir_);
                finishBatch(QStringLiteral("partial-fail"), failed);
                return;
            }

            QList<QUrl> urls;
            urls.reserve(targets.size());
            for (const auto &target : targets)
                urls << QUrl::fromLocalFile(target.second);
            auto *mimeData = new QMimeData();
            mimeData->setUrls(urls);
            mimeData->setData("application/x-openscp-staging-batch",
                              currentBatchDir_.toUtf8());

            auto *drag = new QDrag(self);
            drag->setMimeData(mimeData);
            const Qt::DropAction result = drag->exec(Qt::CopyAction);

            bool droppedInsideThisWindow = false;
            if (const QObject *dropTarget = drag->target()) {
                const QWidget *targetWidget =
                    qobject_cast<const QWidget *>(dropTarget);
                QWidget *const sourceWindow = self->window();
                droppedInsideThisWindow =
                    targetWidget && sourceWindow &&
                    targetWidget->window() == sourceWindow;
            }

            QSettings dragSettings("OpenSCP", "OpenSCP");
            const bool autoClean =
                dragSettings.value("Advanced/autoCleanStaging", true).toBool();
            if (result == Qt::IgnoreAction) {
                showKeepMessage(currentBatchDir_);
                finishBatch(QStringLiteral("canceled"), 0);
                return;
            }
            if (!autoClean) {
                showKeepMessage(currentBatchDir_);
                finishBatch(QStringLiteral("accepted"), 0);
                return;
            }

            // Internal drops may still be copying staged files in the target
            // panel. Give them extra time; the drop target also performs
            // explicit cleanup after local copy/move completion.
            const int cleanupDelayMs = droppedInsideThisWindow ? 10000 : 500;
            scheduleAutoCleanup(currentBatchDir_, cleanupDelayMs);
            finishBatch(QStringLiteral("accepted"), 0);
        });
}

void DragAwareTreeView::startRemoteDragAsync(RemoteModel *rm) {
    if (!rm || !transferMgr_) {
        QTreeView::startDrag(Qt::CopyAction);
        return;
    }
    if (dragInProgress_)
        return;

    dragInProgress_ = true;
    const QModelIndexList rows = collectRemoteSelectedRows();
    if (rows.isEmpty()) {
        resetRemoteDragState();
        return;
    }

    const QString root = buildStagingRoot();
    const QString stamp =
        QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss");
    const QString stagingDir = QDir(root).filePath(stamp);
    currentBatchDir_ = stagingDir;
    currentBatchId_ = stamp;
    batchLogged_ = false;
    QDir().mkpath(stagingDir);

    showPrepOverlay(tr("Preparing files…"));

    QVector<RemoteDragTarget> targets;
    RemoteDragBatchStats stats;
    if (!buildRemoteDragTargets(rm, rows, stagingDir, targets, stats)) {
        hidePrepOverlay();
        resetRemoteDragState();
        return;
    }
    if (!confirmRemoteDragThreshold(stats)) {
        cancelCurrentBatch(QStringLiteral("threshold"));
        return;
    }

    enqueueRemoteDragTargets(targets);
    beginRemoteDragMonitoring(targets, stats);
}

void DragAwareTreeView::cancelCurrentBatch(const QString &reason) {
    if (currentBatchDir_.isEmpty()) {
        hidePrepOverlay();
        resetRemoteDragState();
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
    resetRemoteDragState();
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
