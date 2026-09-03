// Implementation of DragAwareTreeView
#include "DragAwareTreeView.hpp"

#include "AppSettings.hpp"
#include "KeyboardFocusIndicator.hpp"
#include "MainWindowSharedUtils.hpp"
#include "RemoteModel.hpp"
#include "RemoteOperationController.hpp"
#include "TransferManager.hpp"
#include "UiAlerts.hpp"

#include <QApplication>
#include <QCloseEvent>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDrag>
#include <QFileInfo>
#include <QFrame>
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
#include <QShortcut>
#include <QStatusBar>
#include <QTimer>
#include <QUrl>
#include <QUuid>
#include <QVector>

#include <algorithm>
#include <functional>
#include <limits>
#include <memory>

Q_LOGGING_CATEGORY(ocEnum, "openscp.enum")
Q_LOGGING_CATEGORY(ocDrag, "openscp.drag")

namespace {

QString stagingRootFromSettings() {
    openscpui::AppSettings settings;
    QString root =
        settings.value(openscpui::settingskeys::kStagingRoot).toString();
    if (root.isEmpty()) {
        root = QDir::homePath() + "/Downloads/OpenSCP-Dragged";
    }
    return root;
}

QString normalizeStagingName(const QString &value) {
#if defined(Q_OS_MAC)
    return value.normalized(QString::NormalizationForm_C);
#else
    return value;
#endif
}

QPair<QString, QString> splitNameMultiExt(const QString &fileName) {
    const qsizetype firstDot = fileName.indexOf('.', 1);
    if (firstDot <= 0)
        return qMakePair(fileName, QString());
    return qMakePair(fileName.left(firstDot), fileName.mid(firstDot));
}

QPair<int, quint64> loadStagingConfirmThresholds() {
    openscpui::AppSettings settings;
    int itemThreshold =
        settings
            .value(openscpui::settingskeys::kStagingConfirmationItems, 100000)
            .toInt();
    if (itemThreshold < 1)
        itemThreshold = 100000;
    int mibThreshold =
        settings
            .value(openscpui::settingskeys::kStagingConfirmationMiB, 100 * 1024)
            .toInt();
    if (mibThreshold < 1)
        mibThreshold = 100 * 1024;
    return qMakePair(itemThreshold,
                     static_cast<quint64>(mibThreshold) * 1024ull * 1024ull);
}

} // namespace

struct DragAwareTreeView::RemoteDragStagingState {
    QVector<RemoteDragTarget> targets;
    QStringList orderedDirectories;
    QStringList dragRoots;
    QHash<QString, QVector<int>> dragRootIndexes;
    QHash<QString, quint64> directoryTaskIds;
    QHash<QString, int> nextCollisionSuffix;
    QSet<QString> reservedPaths;
    QSet<quint64> allTaskIds;
    QSet<quint64> pendingTaskIds;
    TransferBatchOptions batchOptions;
    RemoteDragBatchStats stats;
    qsizetype nextDirectory = 0;
    qsizetype nextTarget = 0;
    int succeeded = 0;
    int failed = 0;
    bool enqueueComplete = false;
    bool backpressurePaused = false;
    bool pumpScheduled = false;
    bool finished = false;
    QMetaObject::Connection addedConnection;
    QMetaObject::Connection updatedConnection;
    QMetaObject::Connection removedConnection;
};

DragAwareTreeView::DragAwareTreeView(QWidget *parent) : QTreeView(parent) {
    keyboardFocusIndicator_ = new openscpui::KeyboardFocusIndicator(this);
}

void DragAwareTreeView::setTransferManager(TransferManager *mgr) {
    transferMgr_ = mgr;
}

void DragAwareTreeView::setRemoteOperationController(
    RemoteOperationController *controller) {
    remoteOps_ = controller;
}

DragAwareTreeView::~DragAwareTreeView() {
    if (!currentBatchDir_.isEmpty() || (overlay_ && overlay_->isVisible())) {
        cancelCurrentBatch(QStringLiteral("dtor"));
    }
}

void DragAwareTreeView::resizeEvent(QResizeEvent *resizeEventArg) {
    QTreeView::resizeEvent(resizeEventArg);
    updateOverlayGeometry();
}

void DragAwareTreeView::startDrag(Qt::DropActions supportedActions) {
    if (dragInProgress_) {
        if (auto *mainWindow = qobject_cast<QMainWindow *>(window())) {
            mainWindow->statusBar()->showMessage(
                tr("Preparation in progress; please wait."), 3000);
        }
        return;
    }
    // If this is a remote model, run asynchronous staging flow
    if (auto *remoteModel = qobject_cast<RemoteModel *>(model())) {
        startRemoteDragAsync(remoteModel);
        return;
    }

    // Fallback to default behavior (local models)
    QModelIndexList indexes = selectedIndexes();
    if (indexes.isEmpty()) {
        QTreeView::startDrag(supportedActions);
        return;
    }
    QMimeData *mimeData = model()->mimeData(indexes);
    if (!mimeData) {
        QTreeView::startDrag(supportedActions);
        return;
    }
    auto drag = new QDrag(this);
    drag->setMimeData(mimeData);
    const auto dragResult = drag->exec(Qt::CopyAction);

    // Custom payload from RemoteModel when dragging out from remote
    const QByteArray stagingBatchMimeType =
        QByteArrayLiteral("application/x-openscp-staging-batch");
    if (!mimeData->hasFormat(stagingBatchMimeType))
        return; // nothing to do
    const QString batchDir =
        QString::fromUtf8(mimeData->data(stagingBatchMimeType));
    if (batchDir.isEmpty())
        return;

    openscpui::AppSettings settings;
    const bool autoClean =
        settings.value(openscpui::settingskeys::kAutoCleanStaging, true)
            .toBool();

    if (dragResult == Qt::IgnoreAction) {
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
    auto *mainWindow = qobject_cast<QMainWindow *>(window());
    if (!mainWindow)
        return;
    auto *statusLabel = new QLabel(mainWindow);
    statusLabel->setTextFormat(Qt::RichText);
    const QString url = QUrl::fromLocalFile(batchDir).toString();
    statusLabel->setText(
        tr("Staging kept at: <a href=\"%1\">%2</a>").arg(url, batchDir));
    statusLabel->setOpenExternalLinks(true);
    statusLabel->setCursor(Qt::PointingHandCursor);
    mainWindow->statusBar()->addPermanentWidget(statusLabel, 0);
    QTimer::singleShot(10000, statusLabel, [mainWindow, statusLabel] {
        mainWindow->statusBar()->removeWidget(statusLabel);
        statusLabel->deleteLater();
    });
}

void DragAwareTreeView::showKeepMessageWithPrefix(const QString &prefix,
                                                  const QString &batchDir) {
    auto *mainWindow = qobject_cast<QMainWindow *>(window());
    if (!mainWindow)
        return;
    auto *statusLabel = new QLabel(mainWindow);
    statusLabel->setTextFormat(Qt::RichText);
    const QString url = QUrl::fromLocalFile(batchDir).toString();
    const QString statusText =
        QString("%1 <a href=\"%2\">%3</a>").arg(prefix, url, batchDir);
    statusLabel->setText(statusText);
    statusLabel->setOpenExternalLinks(true);
    statusLabel->setCursor(Qt::PointingHandCursor);
    mainWindow->statusBar()->addPermanentWidget(statusLabel, 0);
    QTimer::singleShot(12000, statusLabel, [mainWindow, statusLabel] {
        mainWindow->statusBar()->removeWidget(statusLabel);
        statusLabel->deleteLater();
    });
}

void DragAwareTreeView::scheduleAutoCleanup(const QString &batchDir,
                                            int initialDelayMs) {
    auto tryDelete = [](const QString &dir) -> bool {
        QDir targetDir(dir);
        if (!targetDir.exists())
            return true;
        bool removed = targetDir.removeRecursively();
        if (!removed)
            return false;
        // If root empty, remove it
        QString root = stagingRootFromSettings();
        QDir rootDir(root);
        if (rootDir.exists() &&
            rootDir.entryList(QDir::NoDotAndDotDot | QDir::AllEntries)
                .isEmpty()) {
            rootDir.rmdir(".");
        }
        return true;
    };

    const int firstDelayMs = qBound(0, initialDelayMs, 60000);
    // First attempt with a brief delay to allow OS to finish copying
    QTimer::singleShot(firstDelayMs, this, [this, batchDir, tryDelete]() {
        if (tryDelete(batchDir))
            return;
        // Retry up to 3 times at 1s intervals
        auto retries = std::make_shared<int>(3);
        auto timer = new QTimer(this);
        timer->setInterval(1000);
        connect(timer, &QTimer::timeout, this,
                [timer, retries, batchDir, tryDelete]() {
                    if (tryDelete(batchDir) || --(*retries) <= 0) {
                        timer->stop();
                        timer->deleteLater();
                    }
                });
        timer->start();
    });
}

QString DragAwareTreeView::buildStagingRoot() const {
    openscpui::AppSettings settings;
    QString root =
        settings.value(openscpui::settingskeys::kStagingRoot).toString();
    if (root.isEmpty())
        root = QDir::homePath() + "/Downloads/OpenSCP-Dragged";
    return root;
}

void DragAwareTreeView::showPrepOverlay(const QString &text) {
    if (!overlay_) {
        overlay_ = new QFrame(viewport());
        overlay_->setFrameShape(QFrame::NoFrame);
        overlay_->setAutoFillBackground(true);
        overlay_->setAccessibleName(tr("Preparing files"));
        overlayLabel_ = new QLabel(overlay_);
        QFont overlayFont = overlayLabel_->font();
        overlayFont.setBold(true);
        overlayLabel_->setFont(overlayFont);
        overlayProgress_ = new QProgressBar(overlay_);
        overlayProgress_->setRange(0, 100);
        overlayProgress_->setAccessibleName(tr("File preparation progress"));
        overlayCancel_ = new QPushButton(tr("Cancel"), overlay_);
        overlayCancel_->setAccessibleDescription(
            tr("Cancel preparing files for drag and drop"));
        overlayCancel_->setCursor(Qt::PointingHandCursor);
        // ESC shortcut to cancel staging
        overlayEsc_ = new QShortcut(QKeySequence(Qt::Key_Escape), overlay_);
        QObject::connect(overlayEsc_, &QShortcut::activated, this,
                         [this] { cancelCurrentBatch(QStringLiteral("esc")); });
        // Simple geometry without layouts to avoid layout warnings inside
        // viewport
    }
    overlayLabel_->setText(text);
    overlayLabel_->setAccessibleName(text);
    overlay_->setAccessibleDescription(text);
    updateOverlayGeometry();
    overlay_->show();
    overlayCancel_->setFocus(Qt::OtherFocusReason);
    // Ensure we cancel batch on app quit
    if (!quitConn_) {
        quitConn_ = QObject::connect(
            qApp, &QCoreApplication::aboutToQuit, this,
            [this] { cancelCurrentBatch(QStringLiteral("quit")); });
    }
}

void DragAwareTreeView::hidePrepOverlay() {
    if (overlay_) {
        overlay_->hide();
        setFocus(Qt::OtherFocusReason);
    }
}

void DragAwareTreeView::updateOverlayGeometry() {
    if (!overlay_)
        return;
    const QRect r = viewport() ? viewport()->rect() : rect();
    overlay_->setGeometry(r.adjusted(r.width() / 6, r.height() / 3,
                                     -r.width() / 6, -r.height() / 3));
    // Place controls vertically: label, progress, cancel
    if (overlayLabel_ && overlayProgress_ && overlayCancel_) {
        int overlayWidth = overlay_->width();
        int offsetX = 16;
        int offsetY = 16;
        overlayLabel_->setGeometry(offsetX, offsetY, overlayWidth - 32, 28);
        offsetY += 34;
        overlayProgress_->setGeometry(offsetX, offsetY, overlayWidth - 32, 22);
        offsetY += 30;
        overlayCancel_->setGeometry(offsetX, offsetY, 120, 28);
    }
}

QModelIndexList DragAwareTreeView::collectRemoteSelectedRows() const {
    auto *selection = selectionModel();
    QModelIndexList rows =
        selection ? selection->selectedRows(0) : QModelIndexList{};
    if (!rows.isEmpty() || !selection)
        return rows;

    // Fallback: when selection behavior is not row-based yet, infer unique rows
    // from any selected column to keep drag responsive.
    const auto selected = selection->selectedIndexes();
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

void DragAwareTreeView::finishRemoteDragEnumeration() {
    if (!enumPendingJobs_.isEmpty() || enumThresholdPromptActive_) {
        return;
    }
    if (enumBatchConn_) {
        QObject::disconnect(enumBatchConn_);
        enumBatchConn_ = QMetaObject::Connection();
    }
    if (enumProgressConn_) {
        QObject::disconnect(enumProgressConn_);
        enumProgressConn_ = QMetaObject::Connection();
    }
    if (enumFinishedConn_) {
        QObject::disconnect(enumFinishedConn_);
        enumFinishedConn_ = QMetaObject::Connection();
    }
    if (!dragInProgress_)
        return;

    enumMs_ = prepTimer_.isValid() ? prepTimer_.elapsed() : -1;
    const quint64 enumeratedItems =
        enumStats_.totalItems + enumStats_.totalDirs;
    currentBatchTotal_ = static_cast<int>(std::min<quint64>(
        enumeratedItems,
        static_cast<quint64>(std::numeric_limits<int>::max())));
    if (enumTargets_.isEmpty() && enumDirectories_.isEmpty()) {
        hidePrepOverlay();
        QDir(currentBatchDir_).removeRecursively();
        logBatchResult(
            currentBatchId_, 0, 0,
            QStringLiteral("result=empty enumMs=%1 symlinkSkipped=%2 "
                           "depthLimited=%3 invalidNames=%4 unknownSizes=%5 "
                           "inaccessible=%6")
                .arg(enumMs_)
                .arg(enumSymlinksSkipped_)
                .arg(enumDepthLimits_)
                .arg(enumInvalidNames_)
                .arg(enumStats_.unknownSizeCount)
                .arg(enumInaccessible_));
        resetRemoteDragState();
        return;
    }

    QString bytesText =
        QLocale().formattedDataSize(static_cast<qint64>(enumStats_.totalBytes),
                                    1, QLocale::DataSizeIecFormat);
    if (enumStats_.anySizeUnknown) {
        bytesText = QStringLiteral("~%1 (%2)")
                        .arg(bytesText)
                        .arg(QLocale().toString(static_cast<qulonglong>(
                            enumStats_.unknownSizeCount)));
    }
    const auto thresholds = loadStagingConfirmThresholds();
    const bool exceedsThreshold =
        enumeratedItems > static_cast<quint64>(thresholds.first) ||
        enumStats_.totalBytes > thresholds.second;
    qInfo(ocEnum)
        << "enum batch" << currentBatchId_ << "dirs"
        << QLocale().toString(static_cast<qulonglong>(enumStats_.totalDirs))
        << "files"
        << QLocale().toString(static_cast<qulonglong>(enumStats_.totalItems))
        << "bytes" << bytesText << "enumMs" << enumMs_ << "threshold"
        << (exceedsThreshold ? "yes" : "no") << "symlinkSkipped"
        << QLocale().toString(static_cast<qulonglong>(enumSymlinksSkipped_))
        << "depthLimited"
        << QLocale().toString(static_cast<qulonglong>(enumDepthLimits_))
        << "invalidNames"
        << QLocale().toString(static_cast<qulonglong>(enumInvalidNames_))
        << "unknownSizes"
        << QLocale().toString(
               static_cast<qulonglong>(enumStats_.unknownSizeCount))
        << "inaccessible"
        << QLocale().toString(static_cast<qulonglong>(enumInaccessible_));

    if (!enumThresholdConfirmed_ && !confirmRemoteDragThreshold(enumStats_)) {
        cancelCurrentBatch(QStringLiteral("threshold"));
        return;
    }
    QVector<RemoteDragTarget> targets = std::move(enumTargets_);
    QStringList directories = std::move(enumDirectories_);
    QStringList dragRoots = std::move(enumDragRoots_);
    const RemoteDragBatchStats stats = enumStats_;
    enumTargets_.clear();
    enumDirectories_.clear();
    enumDragRoots_.clear();
    startRemoteDragStaging(std::move(targets), std::move(directories),
                           std::move(dragRoots), stats);
}

bool DragAwareTreeView::confirmRemoteDragThreshold(
    const RemoteDragBatchStats &stats) {
    const auto thresholds = loadStagingConfirmThresholds();
    const bool tooMany = stats.totalItems + stats.totalDirs >
                         static_cast<quint64>(thresholds.first);
    const bool tooBig = stats.totalBytes > thresholds.second;
    if (!tooMany && !tooBig)
        return true;

    auto *mainWindow = qobject_cast<QMainWindow *>(window());
    QWidget *parent = mainWindow ? static_cast<QWidget *>(mainWindow)
                                 : static_cast<QWidget *>(this);
    QMessageBox box(parent);
    UiAlerts::configure(box);
    box.setIcon(QMessageBox::Question);
    box.setWindowTitle(tr("Confirm staging"));

    QString sizePart;
    if (tooBig) {
        sizePart = stats.anySizeUnknown
                       ? QString(" (%1)").arg(
                             tr("~%1 (some unknown)")
                                 .arg(QLocale().formattedDataSize(
                                     static_cast<qint64>(stats.totalBytes), 1,
                                     QLocale::DataSizeIecFormat)))
                       : QString(" (%1)").arg(QLocale().formattedDataSize(
                             static_cast<qint64>(stats.totalBytes), 1,
                             QLocale::DataSizeIecFormat));
    }
    box.setText(tr("You are about to prepare %1 items%2. Continue?")
                    .arg(QLocale().toString(static_cast<qulonglong>(
                        stats.totalItems + stats.totalDirs)))
                    .arg(sizePart));
    auto *yesButton = box.addButton(tr("Continue"), QMessageBox::AcceptRole);
    box.addButton(tr("Cancel"), QMessageBox::RejectRole);
    box.exec();
    return box.clickedButton() == yesButton;
}

bool DragAwareTreeView::enforceRemoteDragThreshold() {
    if (enumThresholdConfirmed_ || !dragInProgress_)
        return dragInProgress_;
    const auto thresholds = loadStagingConfirmThresholds();
    if (enumStats_.totalItems + enumStats_.totalDirs <=
            static_cast<quint64>(thresholds.first) &&
        enumStats_.totalBytes <= thresholds.second) {
        return true;
    }

    enumThresholdConfirmed_ = true;
    enumThresholdPromptActive_ = true;
    const auto jobs = enumPendingJobs_;
    if (remoteOps_) {
        for (const auto jobId : jobs)
            remoteOps_->setPaused(jobId, true);
    }
    const bool confirmed = confirmRemoteDragThreshold(enumStats_);
    enumThresholdPromptActive_ = false;
    if (!confirmed) {
        cancelCurrentBatch(QStringLiteral("threshold"));
        return false;
    }
    if (!dragInProgress_)
        return false;
    if (remoteOps_) {
        for (const auto jobId : jobs)
            remoteOps_->setPaused(jobId, false);
    }
    return true;
}

void DragAwareTreeView::startRemoteDragStaging(
    QVector<RemoteDragTarget> targets, QStringList directories,
    QStringList dragRoots, const RemoteDragBatchStats &stats) {
    if (!transferMgr_) {
        cancelCurrentBatch(QStringLiteral("queue-unavailable"));
        return;
    }

    auto state = std::make_shared<RemoteDragStagingState>();
    state->targets = std::move(targets);
    state->dragRoots = std::move(dragRoots);
    state->stats = stats;
    state->batchOptions.sessionKey = transferMgr_->sessionIdentity();
    state->batchOptions.conflictPolicy = TransferConflictPolicy::Overwrite;
    state->batchOptions.batchId =
        transferMgr_->createBatch(state->batchOptions);

    auto pathKey = [](const QString &path) {
        QString key = normalizeStagingName(QDir::cleanPath(path));
#if defined(Q_OS_MAC) || defined(Q_OS_WIN)
        key = key.toCaseFolded();
#endif
        return key;
    };
    QSet<QString> seenDirectories;
    for (const QString &directory : directories) {
        const QString normalized = QDir::cleanPath(directory);
        const QString relative =
            QDir(currentBatchDir_).relativeFilePath(normalized);
        const QString key = pathKey(normalized);
        if (relative == QLatin1String("..") ||
            relative.startsWith(QStringLiteral("../")) ||
            seenDirectories.contains(key)) {
            continue;
        }
        seenDirectories.insert(key);
        state->reservedPaths.insert(key);
        state->orderedDirectories.push_back(normalized);
    }
    std::sort(state->orderedDirectories.begin(),
              state->orderedDirectories.end(),
              [](const QString &left, const QString &right) {
                  const qsizetype leftDepth =
                      QDir::fromNativeSeparators(left).count(QLatin1Char('/'));
                  const qsizetype rightDepth =
                      QDir::fromNativeSeparators(right).count(QLatin1Char('/'));
                  if (leftDepth != rightDepth)
                      return leftDepth < rightDepth;
                  return left < right;
              });
    for (int index = 0; index < state->dragRoots.size(); ++index) {
        state->dragRootIndexes[QDir::cleanPath(state->dragRoots.at(index))]
            .push_back(index);
    }

    stagingState_ = state;
    if (overlayProgress_)
        overlayProgress_->setValue(0);
    stagingTimer_.restart();

    state->addedConnection =
        connect(transferMgr_, &TransferManager::tasksAdded, this,
                [this, state](const QVector<quint64> &taskIds) {
                    if (stagingState_ != state || !transferMgr_)
                        return;
                    const auto tasks = transferMgr_->tasksSnapshot(taskIds);
                    for (const auto &task : tasks) {
                        if (task.batchId != state->batchOptions.batchId ||
                            state->allTaskIds.contains(task.taskId)) {
                            continue;
                        }
                        state->allTaskIds.insert(task.taskId);
                        state->pendingTaskIds.insert(task.taskId);
                    }
                });
    state->updatedConnection =
        connect(transferMgr_, &TransferManager::tasksUpdated, this,
                [this, state](const QVector<quint64> &taskIds) {
                    reconcileRemoteDragTasks(state, taskIds, false);
                });
    state->removedConnection =
        connect(transferMgr_, &TransferManager::tasksRemoved, this,
                [this, state](const QVector<quint64> &taskIds) {
                    reconcileRemoteDragTasks(state, taskIds, true);
                });

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
    openscpui::AppSettings settings;
    int timeoutMs =
        settings
            .value(openscpui::settingskeys::kStagingPreparationTimeoutMs, 2000)
            .toInt();
    timeoutMs = qBound(250, timeoutMs, 60000);
    waitTimer_->setInterval(timeoutMs);
    QObject::connect(waitTimer_, &QTimer::timeout, this, [this] {
        auto *mainWindow = qobject_cast<QMainWindow *>(window());
        if (!mainWindow || !dragInProgress_)
            return;
        QMessageBox box(mainWindow);
        UiAlerts::configure(box);
        box.setIcon(QMessageBox::Information);
        box.setWindowTitle(tr("Preparing files…"));
        box.setText(tr("Still preparing files for drag-out. Wait or cancel?"));
        box.addButton(tr("Wait"), QMessageBox::AcceptRole);
        auto *cancelButton =
            box.addButton(tr("Cancel"), QMessageBox::RejectRole);
        box.exec();
        if (box.clickedButton() == cancelButton)
            cancelCurrentBatch(QStringLiteral("dialog"));
    });
    waitTimer_->start();

    QTimer::singleShot(0, this,
                       [this, state] { pumpRemoteDragStaging(state); });
}

void DragAwareTreeView::pumpRemoteDragStaging(
    const std::shared_ptr<RemoteDragStagingState> &state) {
    if (!state || stagingState_ != state || state->finished ||
        !dragInProgress_) {
        return;
    }
    state->pumpScheduled = false;
    if (!transferMgr_) {
        cancelCurrentBatch(QStringLiteral("queue-unavailable"));
        return;
    }
    if (state->backpressurePaused) {
        if (state->pendingTaskIds.size() >= 1000)
            return;
        state->backpressurePaused = false;
    }

    auto pathKey = [](const QString &path) {
        QString key = normalizeStagingName(QDir::cleanPath(path));
#if defined(Q_OS_MAC) || defined(Q_OS_WIN)
        key = key.toCaseFolded();
#endif
        return key;
    };
    auto uniqueFullPath = [&state, &pathKey](const QString &fullPath) {
        const QFileInfo fileInfo(fullPath);
        const QString dirPath = fileInfo.dir().absolutePath();
        const QString normalizedName =
            normalizeStagingName(fileInfo.fileName());
        const auto parts = splitNameMultiExt(normalizedName);
        const QString baseName = parts.first;
        const QString extension = parts.second;
        const QString originalKey = pathKey(fullPath);
        int suffix = state->nextCollisionSuffix.value(originalKey, 0);
        while (true) {
            const QString name = suffix == 0 ? normalizedName
                                             : QStringLiteral("%1 (%2)%3")
                                                   .arg(baseName)
                                                   .arg(suffix)
                                                   .arg(extension);
            const QString candidate = QDir(dirPath).filePath(name);
            const QString key = pathKey(candidate);
            if (!state->reservedPaths.contains(key) &&
                !QFileInfo::exists(candidate)) {
                state->reservedPaths.insert(key);
                state->nextCollisionSuffix.insert(originalKey, suffix + 1);
                return candidate;
            }
            ++suffix;
        }
    };
    auto registerFallback = [this, state](quint64 taskId) {
        if (taskId == 0) {
            ++state->failed;
            return;
        }
        if (!state->allTaskIds.contains(taskId)) {
            state->allTaskIds.insert(taskId);
            state->pendingTaskIds.insert(taskId);
        }
        reconcileRemoteDragTasks(state, {taskId}, false);
    };

    constexpr int kEnqueueBatchSize = 250;
    constexpr int kHighWatermark = 2000;
    int enqueuedThisTurn = 0;
    while (enqueuedThisTurn < kEnqueueBatchSize &&
           state->pendingTaskIds.size() < kHighWatermark &&
           state->nextDirectory < state->orderedDirectories.size()) {
        const QString directory =
            state->orderedDirectories.at(state->nextDirectory++);
        TransferBatchOptions options = state->batchOptions;
        options.dependsOnTaskId = state->directoryTaskIds.value(
            QFileInfo(directory).dir().absolutePath());
        const quint64 taskId =
            transferMgr_->enqueueLocalDirectory(directory, options);
        if (taskId != 0)
            state->directoryTaskIds.insert(directory, taskId);
        registerFallback(taskId);
        ++enqueuedThisTurn;
    }
    while (enqueuedThisTurn < kEnqueueBatchSize &&
           state->pendingTaskIds.size() < kHighWatermark &&
           state->nextDirectory >= state->orderedDirectories.size() &&
           state->nextTarget < state->targets.size()) {
        auto &target = state->targets[state->nextTarget++];
        const QString originalPath = QDir::cleanPath(target.second);
        const QString relative =
            QDir(currentBatchDir_).relativeFilePath(originalPath);
        if (relative == QLatin1String("..") ||
            relative.startsWith(QStringLiteral("../"))) {
            ++state->failed;
            ++enqueuedThisTurn;
            continue;
        }
        target.second = uniqueFullPath(target.second);
        const auto rootIndexes = state->dragRootIndexes.value(originalPath);
        for (const int rootIndex : rootIndexes) {
            if (rootIndex >= 0 && rootIndex < state->dragRoots.size())
                state->dragRoots[rootIndex] = target.second;
        }
        TransferBatchOptions options = state->batchOptions;
        options.dependsOnTaskId = state->directoryTaskIds.value(
            QFileInfo(target.second).dir().absolutePath());
        registerFallback(transferMgr_->enqueueDownload(target.first,
                                                       target.second, options));
        ++enqueuedThisTurn;
    }

    state->enqueueComplete =
        state->nextDirectory >= state->orderedDirectories.size() &&
        state->nextTarget >= state->targets.size();
    if (state->pendingTaskIds.size() >= kHighWatermark)
        state->backpressurePaused = true;

    const int total = static_cast<int>(state->orderedDirectories.size() +
                                       state->targets.size());
    const int completed = state->succeeded + state->failed;
    if (overlayProgress_) {
        overlayProgress_->setValue(
            total > 0 ? static_cast<int>(
                            (static_cast<qint64>(completed) * 100) / total)
                      : 0);
    }
    if (overlayLabel_) {
        overlayLabel_->setText(
            state->backpressurePaused
                ? tr("Queue backpressure: waiting for pending tasks to "
                     "drop below 1,000…")
                : tr("Preparing files…"));
    }

    if (state->enqueueComplete && state->pendingTaskIds.isEmpty()) {
        finishRemoteDragStaging(state);
        return;
    }
    if (!state->enqueueComplete && !state->backpressurePaused &&
        !state->pumpScheduled) {
        state->pumpScheduled = true;
        QTimer::singleShot(0, this,
                           [this, state] { pumpRemoteDragStaging(state); });
    }
}

void DragAwareTreeView::reconcileRemoteDragTasks(
    const std::shared_ptr<RemoteDragStagingState> &state,
    const QVector<quint64> &taskIds, bool removed) {
    if (!state || stagingState_ != state || state->finished || !transferMgr_) {
        return;
    }
    QVector<quint64> relevant;
    relevant.reserve(taskIds.size());
    for (const quint64 taskId : taskIds) {
        if (state->pendingTaskIds.contains(taskId))
            relevant.push_back(taskId);
    }
    if (relevant.isEmpty())
        return;

    if (removed) {
        for (const quint64 taskId : relevant) {
            if (state->pendingTaskIds.remove(taskId))
                ++state->failed;
        }
    } else {
        const auto tasks = transferMgr_->tasksSnapshot(relevant);
        QSet<quint64> observed;
        for (const auto &task : tasks) {
            observed.insert(task.taskId);
            const bool succeeded = task.status == TransferTask::Status::Done;
            const bool failed = task.status == TransferTask::Status::Error ||
                                task.status == TransferTask::Status::Canceled ||
                                task.status == TransferTask::Status::Skipped ||
                                task.status == TransferTask::Status::Warning;
            if (!succeeded && !failed)
                continue;
            if (!state->pendingTaskIds.remove(task.taskId))
                continue;
            if (succeeded)
                ++state->succeeded;
            else
                ++state->failed;
        }
        for (const quint64 taskId : relevant) {
            if (!observed.contains(taskId) &&
                state->pendingTaskIds.remove(taskId)) {
                ++state->failed;
            }
        }
    }

    if (state->enqueueComplete && state->pendingTaskIds.isEmpty()) {
        finishRemoteDragStaging(state);
        return;
    }
    if (!state->enqueueComplete &&
        (!state->backpressurePaused || state->pendingTaskIds.size() < 1000) &&
        !state->pumpScheduled) {
        state->pumpScheduled = true;
        QTimer::singleShot(0, this,
                           [this, state] { pumpRemoteDragStaging(state); });
    }
}

QString
DragAwareTreeView::formatRemoteDragMetrics(const QString &result,
                                           const RemoteDragBatchStats &stats,
                                           qint64 stagingMs) const {
    return QString("result=%1 enumDirs=%2 files=%3 enumMs=%4 stagingMs=%5 "
                   "symlinkSkipped=%6 depthLimited=%7 invalidNames=%8 "
                   "unknownSizes=%9 inaccessible=%10")
        .arg(result)
        .arg(QLocale().toString(static_cast<qulonglong>(stats.totalDirs)))
        .arg(QLocale().toString(static_cast<qulonglong>(stats.totalItems)))
        .arg(enumMs_)
        .arg(stagingMs)
        .arg(QLocale().toString(static_cast<qulonglong>(enumSymlinksSkipped_)))
        .arg(QLocale().toString(static_cast<qulonglong>(enumDepthLimits_)))
        .arg(QLocale().toString(static_cast<qulonglong>(enumInvalidNames_)))
        .arg(
            QLocale().toString(static_cast<qulonglong>(stats.unknownSizeCount)))
        .arg(QLocale().toString(static_cast<qulonglong>(enumInaccessible_)));
}

void DragAwareTreeView::resetRemoteDragState() {
    if (waitTimer_)
        waitTimer_->stop();
    if (stagingState_) {
        stagingState_->finished = true;
        QObject::disconnect(stagingState_->addedConnection);
        QObject::disconnect(stagingState_->updatedConnection);
        QObject::disconnect(stagingState_->removedConnection);
        stagingState_.reset();
    }
    dragInProgress_ = false;
    currentBatchDir_.clear();
    currentBatchId_.clear();
    currentBatchTotal_ = 0;
    enumJobLocalRoots_.clear();
    enumPendingJobs_.clear();
    enumTargets_.clear();
    enumDirectories_.clear();
    enumDragRoots_.clear();
    enumStats_ = RemoteDragBatchStats{};
    enumSymlinksSkipped_ = 0;
    enumDepthLimits_ = 0;
    enumInvalidNames_ = 0;
    enumInaccessible_ = 0;
    enumThresholdConfirmed_ = false;
    enumThresholdPromptActive_ = false;
    if (enumBatchConn_) {
        QObject::disconnect(enumBatchConn_);
        enumBatchConn_ = QMetaObject::Connection();
    }
    if (enumProgressConn_) {
        QObject::disconnect(enumProgressConn_);
        enumProgressConn_ = QMetaObject::Connection();
    }
    if (enumFinishedConn_) {
        QObject::disconnect(enumFinishedConn_);
        enumFinishedConn_ = QMetaObject::Connection();
    }
    if (quitConn_) {
        QObject::disconnect(quitConn_);
        quitConn_ = QMetaObject::Connection();
    }
}

void DragAwareTreeView::finishRemoteDragStaging(
    const std::shared_ptr<RemoteDragStagingState> &state) {
    if (!state || stagingState_ != state || state->finished)
        return;
    state->finished = true;
    QObject::disconnect(state->addedConnection);
    QObject::disconnect(state->updatedConnection);
    QObject::disconnect(state->removedConnection);
    hidePrepOverlay();

    const int total = static_cast<int>(state->orderedDirectories.size() +
                                       state->targets.size());
    const int enumeratedTotal = static_cast<int>(std::min<quint64>(
        state->stats.totalItems + state->stats.totalDirs,
        static_cast<quint64>(std::numeric_limits<int>::max())));
    auto finishBatch = [this, enumeratedTotal, stats = state->stats](
                           const QString &result, int failedItems) {
        const qint64 stagingMs =
            stagingTimer_.isValid() ? stagingTimer_.elapsed() : -1;
        logBatchResult(currentBatchId_, enumeratedTotal, failedItems,
                       formatRemoteDragMetrics(result, stats, stagingMs));
        resetRemoteDragState();
    };

    if (state->failed > 0) {
        const QString prefix =
            tr("%1 of %2 staging operations failed. Files kept at:")
                .arg(state->failed)
                .arg(total);
        showKeepMessageWithPrefix(prefix, currentBatchDir_);
        finishBatch(QStringLiteral("partial-fail"), state->failed);
        return;
    }

    QList<QUrl> urls;
    QSet<QString> seenRoots;
    for (const QString &root : state->dragRoots) {
        const QString normalized = QDir::cleanPath(root);
        if (seenRoots.contains(normalized) || !QFileInfo::exists(normalized)) {
            continue;
        }
        seenRoots.insert(normalized);
        urls << QUrl::fromLocalFile(normalized);
    }
    if (urls.isEmpty()) {
        showKeepMessageWithPrefix(
            tr("No staged drag roots were available. Files kept at:"),
            currentBatchDir_);
        finishBatch(QStringLiteral("missing-roots"), 1);
        return;
    }

    auto *mimeData = new QMimeData();
    mimeData->setUrls(urls);
    mimeData->setData("application/x-openscp-staging-batch",
                      currentBatchDir_.toUtf8());

    auto *drag = new QDrag(this);
    drag->setMimeData(mimeData);
    const Qt::DropAction result = drag->exec(Qt::CopyAction);

    bool droppedInsideThisWindow = false;
    if (const QObject *dropTarget = drag->target()) {
        const QWidget *targetWidget = qobject_cast<const QWidget *>(dropTarget);
        QWidget *const sourceWindow = window();
        droppedInsideThisWindow = targetWidget && sourceWindow &&
                                  targetWidget->window() == sourceWindow;
    }

    openscpui::AppSettings settings;
    const bool autoClean =
        settings.value(openscpui::settingskeys::kAutoCleanStaging, true)
            .toBool();
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

    const int cleanupDelayMs = droppedInsideThisWindow ? 10000 : 500;
    scheduleAutoCleanup(currentBatchDir_, cleanupDelayMs);
    finishBatch(QStringLiteral("accepted"), 0);
}

void DragAwareTreeView::startRemoteDragAsync(RemoteModel *remoteModel) {
    if (!remoteModel || !transferMgr_ || !remoteOps_ ||
        !remoteOps_->hasRequestedSession()) {
        QTreeView::startDrag(Qt::CopyAction);
        return;
    }
    if (dragInProgress_)
        return;

    const QModelIndexList rows = collectRemoteSelectedRows();
    if (rows.isEmpty())
        return;

    dragInProgress_ = true;
    enumTargets_.clear();
    enumJobLocalRoots_.clear();
    enumPendingJobs_.clear();
    enumStats_ = RemoteDragBatchStats{};
    enumDirectories_.clear();
    enumDragRoots_.clear();
    enumSymlinksSkipped_ = 0;
    enumDepthLimits_ = 0;
    enumInvalidNames_ = 0;
    enumInaccessible_ = 0;
    enumMs_ = -1;
    enumThresholdConfirmed_ = false;
    enumThresholdPromptActive_ = false;
    prepTimer_.restart();

    const QString root = buildStagingRoot();
    const QString stamp =
        QDateTime::currentDateTimeUtc().toString("yyyyMMdd-HHmmss-zzz");
    const QString uniqueSuffix =
        QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
    const QString stagingDir =
        QDir(root).filePath(stamp + QLatin1Char('-') + uniqueSuffix);
    currentBatchDir_ = stagingDir;
    currentBatchId_ = QFileInfo(stagingDir).fileName();
    batchLogged_ = false;
    if (!QDir().mkpath(stagingDir)) {
        currentBatchDir_.clear();
        currentBatchId_.clear();
        dragInProgress_ = false;
        if (auto *mainWindow = qobject_cast<QMainWindow *>(window())) {
            mainWindow->statusBar()->showMessage(
                tr("Could not create the drag staging directory."), 5000);
        }
        return;
    }
    showPrepOverlay(tr("Preparing files…"));
    if (overlayCancel_) {
        QObject::disconnect(overlayCancel_, nullptr, this, nullptr);
        QObject::connect(overlayCancel_, &QPushButton::clicked, this, [this] {
            cancelCurrentBatch(QStringLiteral("enumeration-button"));
        });
    }

    enumBatchConn_ = connect(
        remoteOps_, &RemoteOperationController::entriesBatchReady, this,
        [this](const RemoteOperationController::EntryBatch &batch) {
            const auto rootIt = enumJobLocalRoots_.constFind(batch.job.id);
            if (rootIt == enumJobLocalRoots_.cend() || !dragInProgress_)
                return;
            const QString &localRoot = rootIt.value();
            for (const auto &entry : batch.entries) {
                if (entry.info.is_dir) {
                    bool valid = !entry.relativePath.isEmpty();
                    QStringList normalizedParts;
                    const QStringList parts = entry.relativePath.split(
                        QLatin1Char('/'), Qt::SkipEmptyParts);
                    normalizedParts.reserve(parts.size());
                    for (const QString &part : parts) {
                        QString why;
                        if (!isValidEntryName(part, &why)) {
                            valid = false;
                            break;
                        }
                        normalizedParts.push_back(normalizeStagingName(part));
                    }
                    if (!valid) {
                        ++enumInvalidNames_;
                        continue;
                    }
                    enumDirectories_.push_back(QDir(localRoot).filePath(
                        normalizedParts.join(QLatin1Char('/'))));
                    ++enumStats_.totalDirs;
                    continue;
                }
                bool valid = !entry.relativePath.isEmpty();
                QStringList normalizedParts;
                const QStringList parts = entry.relativePath.split(
                    QLatin1Char('/'), Qt::SkipEmptyParts);
                normalizedParts.reserve(parts.size());
                for (const QString &part : parts) {
                    QString why;
                    if (!isValidEntryName(part, &why)) {
                        valid = false;
                        break;
                    }
                    normalizedParts.push_back(normalizeStagingName(part));
                }
                if (!valid) {
                    ++enumInvalidNames_;
                    continue;
                }
                const QString localPath = QDir(localRoot).filePath(
                    normalizedParts.join(QLatin1Char('/')));
                enumTargets_.push_back({entry.path, localPath});
                ++enumStats_.totalItems;
                if (entry.info.has_size) {
                    const quint64 available =
                        std::numeric_limits<quint64>::max() -
                        enumStats_.totalBytes;
                    enumStats_.totalBytes += std::min(
                        available, static_cast<quint64>(entry.info.size));
                }
            }
            if (enforceRemoteDragThreshold() && enumPendingJobs_.isEmpty()) {
                finishRemoteDragEnumeration();
            }
        });
    enumProgressConn_ = connect(
        remoteOps_, &RemoteOperationController::jobProgress, this,
        [this](const RemoteOperationController::Progress &progress) {
            if (!enumPendingJobs_.contains(progress.job.id) || !overlayLabel_) {
                return;
            }
            overlayLabel_->setText(tr("Preparing files…") +
                                   QStringLiteral(" ") +
                                   QLocale().toString(static_cast<qulonglong>(
                                       enumStats_.totalItems)));
        });
    enumFinishedConn_ = connect(
        remoteOps_, &RemoteOperationController::jobFinished, this,
        [this](const RemoteOperationController::Completion &completion) {
            if (!enumPendingJobs_.remove(completion.result.job.id))
                return;
            enumInaccessible_ += completion.failedEntries;
            enumSymlinksSkipped_ += completion.skippedSymlinks;
            enumDepthLimits_ += completion.depthLimits;
            enumInvalidNames_ += completion.invalidNames;
            enumStats_.unknownSizeCount += completion.unknownSizes;
            enumStats_.anySizeUnknown = enumStats_.unknownSizeCount > 0;
            if (completion.result.outcome ==
                    RemoteOperationController::Outcome::Failed &&
                completion.failedEntries == 0) {
                ++enumInaccessible_;
            }
            finishRemoteDragEnumeration();
        });

    const QString remoteRoot = remoteModel->rootPath();
    openscpui::AppSettings settings;
    int maxDepth =
        settings.value(openscpui::settingskeys::kMaximumFolderDepth, 32)
            .toInt();
    if (maxDepth < 1)
        maxDepth = 32;

    for (const QModelIndex &index : rows) {
        if (!index.isValid())
            continue;
        const QString name = remoteModel->nameAt(index);
        QString why;
        if (!isValidEntryName(name, &why)) {
            ++enumInvalidNames_;
            continue;
        }
        const QString remotePath = joinRemotePath(remoteRoot, name);
        const QString localPath =
            QDir(stagingDir).filePath(normalizeStagingName(name));
        if (!remoteModel->isDir(index)) {
            enumTargets_.push_back({remotePath, localPath});
            enumDragRoots_.push_back(localPath);
            ++enumStats_.totalItems;
            if (remoteModel->hasSize(index)) {
                const quint64 available =
                    std::numeric_limits<quint64>::max() - enumStats_.totalBytes;
                enumStats_.totalBytes +=
                    std::min(available, remoteModel->sizeAt(index));
            } else {
                enumStats_.anySizeUnknown = true;
                ++enumStats_.unknownSizeCount;
            }
            continue;
        }

        enumDirectories_.push_back(localPath);
        enumDragRoots_.push_back(localPath);
        ++enumStats_.totalDirs;
        RemoteOperationController::TraverseRequest request;
        request.rootPath = remotePath;
        request.includeDirectories = true;
        request.traversal.includeHidden = remoteModel->showHidden();
        request.traversal.skipSymlinks = true;
        request.traversal.maxDepth = maxDepth;
        request.traversal.batchSize = 250;
        const auto jobId = remoteOps_->submit(request);
        if (jobId == 0) {
            ++enumInaccessible_;
            continue;
        }
        enumJobLocalRoots_.insert(jobId, localPath);
        enumPendingJobs_.insert(jobId);
    }

    if (!enforceRemoteDragThreshold())
        return;
    finishRemoteDragEnumeration();
}

void DragAwareTreeView::cancelCurrentBatch(const QString &reason) {
    if (currentBatchDir_.isEmpty()) {
        hidePrepOverlay();
        resetRemoteDragState();
        return;
    }
    if (stagingState_) {
        stagingState_->finished = true;
        QObject::disconnect(stagingState_->addedConnection);
        QObject::disconnect(stagingState_->updatedConnection);
        QObject::disconnect(stagingState_->removedConnection);
    }
    if (waitTimer_)
        waitTimer_->stop();
    if (remoteOps_) {
        const auto pending = enumPendingJobs_;
        for (const auto jobId : pending)
            remoteOps_->cancel(jobId);
    }
    hidePrepOverlay();
    if (transferMgr_ && stagingState_)
        transferMgr_->cancelBatch(stagingState_->batchOptions.batchId);
    if (reason != QLatin1String("dtor") && reason != QLatin1String("quit")) {
        showKeepMessage(currentBatchDir_);
    }
    // Preserve discovery counters in diagnostics even on manual cancel.
    const qint64 stagingMs =
        stagingTimer_.isValid() ? stagingTimer_.elapsed() : -1;
    logBatchResult(
        currentBatchId_, currentBatchTotal_, 0,
        QString("result=canceled enumMs=%1 stagingMs=%2 symlinkSkipped=%3 "
                "depthLimited=%4 invalidNames=%5 unknownSizes=%6 "
                "inaccessible=%7 (%8)")
            .arg(enumMs_)
            .arg(stagingMs)
            .arg(QLocale().toString(
                static_cast<qulonglong>(enumSymlinksSkipped_)))
            .arg(QLocale().toString(static_cast<qulonglong>(enumDepthLimits_)))
            .arg(QLocale().toString(static_cast<qulonglong>(enumInvalidNames_)))
            .arg(QLocale().toString(
                static_cast<qulonglong>(enumStats_.unknownSizeCount)))
            .arg(QLocale().toString(static_cast<qulonglong>(enumInaccessible_)))
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

void DragAwareTreeView::closeEvent(QCloseEvent *closeEventArg) {
    if (overlay_ && overlay_->isVisible()) {
        cancelCurrentBatch(QStringLiteral("close"));
    }
    QTreeView::closeEvent(closeEventArg);
}
