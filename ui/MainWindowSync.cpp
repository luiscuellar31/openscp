#include "MainWindow.hpp"
#include "SessionController.hpp"

#include "RemoteModel.hpp"
#include "RemoteOperationController.hpp"
#include "SyncCoordinator.hpp"
#include "SyncDialog.hpp"
#include "TransferManager.hpp"
#include "UiAlerts.hpp"

#include <algorithm>
#include <limits>

#include <QDialog>
#include <QLocale>
#include <QMessageBox>
#include <QProgressDialog>
#include <QStatusBar>

namespace {

void closeSyncProgress(QPointer<QProgressDialog> &progress) {
    if (!progress)
        return;
    progress->hide();
    progress->deleteLater();
    progress.clear();
}

qint64 displayableByteCount(quint64 bytes) {
    return static_cast<qint64>(
        std::min<quint64>(
            bytes, static_cast<quint64>(std::numeric_limits<qint64>::max())));
}

} // namespace

void MainWindow::initializeSyncCoordinator() {
    if (syncCoordinator_ || !remoteOps_ || !transferMgr_)
        return;
    syncCoordinator_ =
        new SyncCoordinator(remoteOps_, transferMgr_, this);

    connect(syncCoordinator_, &SyncCoordinator::progressChanged, this,
            [this](quint64 items, quint64 bytes, const QString &path) {
                if (!syncProgress_)
                    return;
                syncProgress_->setLabelText(
                    tr("Comparing folders… %1 items, %2\n%3")
                        .arg(QLocale().toString(items),
                             QLocale().formattedDataSize(
                                 static_cast<qint64>(std::min<quint64>(
                                     bytes, quint64(
                                                std::numeric_limits<qint64>::
                                                    max())))),
                             path));
            });
    connect(syncCoordinator_, &SyncCoordinator::preparationCanceled, this,
            [this] {
                closeSyncProgress(syncProgress_);
                statusBar()->showMessage(tr("Folder comparison canceled"),
                                         4000);
            });
    connect(syncCoordinator_, &SyncCoordinator::preparationFailed, this,
            [this](const QString &message) {
                closeSyncProgress(syncProgress_);
                UiAlerts::warning(this, tr("Compare folders"), message);
            });
    connect(syncCoordinator_,
            &SyncCoordinator::largeTreeConfirmationRequired, this,
            [this](quint64 items, quint64 bytes) {
                closeSyncProgress(syncProgress_);
                const auto answer = UiAlerts::question(
                    this, tr("Very large comparison"),
                    tr("The two trees contain more than the safe review "
                       "threshold:\n\n%1 items\n%2 of known data\n\n"
                       "Scan them again without this limit?")
                        .arg(QLocale().toString(items),
                             QLocale().formattedDataSize(
                                 static_cast<qint64>(std::min<quint64>(
                                     bytes, quint64(
                                                std::numeric_limits<qint64>::
                                                    max()))))),
                    QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
                if (answer != QMessageBox::Yes) {
                    syncCoordinator_->cancel();
                    return;
                }
                auto *progress = new QProgressDialog(
                    tr("Preparing a large folder comparison…"), tr("Cancel"),
                    0, 0, this);
                progress->setWindowTitle(tr("Compare folders"));
                progress->setWindowModality(Qt::NonModal);
                progress->setMinimumDuration(0);
                progress->setAutoClose(false);
                syncProgress_ = progress;
                connect(progress, &QProgressDialog::canceled,
                        syncCoordinator_, &SyncCoordinator::cancel);
                progress->show();
                syncCoordinator_->continueLargeTree();
            });
    connect(syncCoordinator_, &SyncCoordinator::preparationReady, this,
            [this](const SyncPreparationResult &prepared) {
                closeSyncProgress(syncProgress_);
                if (!rightIsRemote_ || !transferMgr_)
                    return;

                if (!prepared.warnings.isEmpty()) {
                    UiAlerts::warning(
                        this, tr("Partial comparison"),
                        tr("The preview is available with these warnings:\n%1")
                            .arg(prepared.warnings.join(QLatin1Char('\n'))));
                }

                SyncDialog dialog(this);
                const QString localRoot = prepared.localRoot;
                const QString remoteRoot = prepared.remoteRoot;
                QVector<SyncSnapshotEntry> localSnapshot =
                    prepared.localSnapshot;
                QVector<SyncSnapshotEntry> remoteSnapshot =
                    prepared.remoteSnapshot;
                dialog.setRootPaths(localRoot, remoteRoot);
                dialog.setSnapshots(localSnapshot, remoteSnapshot);
                const openscp::ProtocolCapabilities capabilities =
                    sessionController_->options()
                        ? openscp::capabilitiesForProtocol(
                              sessionController_->options()->protocol)
                        : openscp::ProtocolCapabilities{};
                dialog.setChecksumAvailable(capabilities.can_checksum);

                connect(
                    &dialog, &SyncDialog::checksumRequested, &dialog,
                    [this, &dialog, localRoot,
                     remoteRoot](const QStringList &relativePaths) {
                        if (!syncCoordinator_ ||
                            syncCoordinator_->isCalculatingChecksums()) {
                            return;
                        }
                        dialog.setChecksumBusy(true);
                        auto *progress = new QProgressDialog(
                            tr("Calculating SHA-256 checksums…"), tr("Cancel"),
                            0,
                            std::max(
                                1, static_cast<int>(std::min<qsizetype>(
                                       relativePaths.size() * 2,
                                       std::numeric_limits<int>::max()))),
                            &dialog);
                        progress->setWindowTitle(
                            tr("Compare checksums"));
                        progress->setWindowModality(Qt::WindowModal);
                        progress->setMinimumDuration(0);
                        progress->setAutoClose(false);
                        syncProgress_ = progress;
                        connect(progress, &QProgressDialog::canceled,
                                syncCoordinator_,
                                &SyncCoordinator::cancelChecksums);
                        progress->show();
                        syncCoordinator_->startChecksums(
                            localRoot, remoteRoot, relativePaths);
                    });

                connect(
                    syncCoordinator_,
                    &SyncCoordinator::checksumProgressChanged, &dialog,
                    [this](qsizetype completed, qsizetype total,
                           const QString &path, quint64 processedBytes,
                           quint64 totalBytes) {
                        if (!syncProgress_)
                            return;
                        syncProgress_->setMaximum(std::max(
                            1, static_cast<int>(std::min<qsizetype>(
                                   total,
                                   std::numeric_limits<int>::max()))));
                        syncProgress_->setValue(static_cast<int>(
                            std::min<qsizetype>(
                                completed,
                                std::numeric_limits<int>::max())));
                        QString byteProgress;
                        if (processedBytes > 0 || totalBytes > 0) {
                            byteProgress =
                                totalBytes > 0
                                    ? tr("%1 of %2")
                                          .arg(QLocale().formattedDataSize(
                                                   displayableByteCount(
                                                       processedBytes)),
                                               QLocale().formattedDataSize(
                                                   displayableByteCount(
                                                       totalBytes)))
                                    : QLocale().formattedDataSize(
                                          displayableByteCount(
                                              processedBytes));
                        }
                        syncProgress_->setLabelText(
                            tr("Calculating SHA-256 checksums…\n"
                               "%1 of %2 completed\n%3\n%4")
                                .arg(QLocale().toString(
                                         static_cast<qlonglong>(completed)),
                                     QLocale().toString(
                                         static_cast<qlonglong>(total)),
                                     path,
                                     byteProgress));
                    });

                connect(
                    syncCoordinator_, &SyncCoordinator::checksumReady,
                    &dialog,
                    [this, &dialog, &localSnapshot,
                     &remoteSnapshot](const SyncChecksumResult &result) {
                        closeSyncProgress(syncProgress_);
                        for (SyncSnapshotEntry &entry : localSnapshot) {
                            const auto local =
                                result.localChecksums.constFind(
                                    entry.relativePath);
                            const auto remote =
                                result.remoteChecksums.constFind(
                                    entry.relativePath);
                            if (local == result.localChecksums.cend() ||
                                remote ==
                                    result.remoteChecksums.cend()) {
                                continue;
                            }
                            entry.checksumAlgorithm = result.algorithm;
                            entry.checksum = local.value();
                        }
                        for (SyncSnapshotEntry &entry : remoteSnapshot) {
                            const auto local =
                                result.localChecksums.constFind(
                                    entry.relativePath);
                            const auto remote =
                                result.remoteChecksums.constFind(
                                    entry.relativePath);
                            if (local == result.localChecksums.cend() ||
                                remote ==
                                    result.remoteChecksums.cend()) {
                                continue;
                            }
                            entry.checksumAlgorithm = result.algorithm;
                            entry.checksum = remote.value();
                        }
                        dialog.setSnapshots(localSnapshot, remoteSnapshot);
                        dialog.setChecksumBusy(false);
                        if (!result.failures.isEmpty()) {
                            UiAlerts::warning(
                                &dialog, tr("Partial checksum comparison"),
                                tr("Some checksums could not be calculated:\n"
                                   "%1")
                                    .arg(result.failures.join(
                                        QLatin1Char('\n'))));
                        }
                    });
                connect(
                    syncCoordinator_, &SyncCoordinator::checksumFailed,
                    &dialog, [this, &dialog](const QString &message) {
                        closeSyncProgress(syncProgress_);
                        dialog.setChecksumBusy(false);
                        UiAlerts::warning(&dialog, tr("Compare checksums"),
                                          message);
                    });
                connect(
                    syncCoordinator_, &SyncCoordinator::checksumCanceled,
                    &dialog, [this, &dialog] {
                        closeSyncProgress(syncProgress_);
                        dialog.setChecksumBusy(false);
                        statusBar()->showMessage(
                            tr("Checksum comparison canceled"), 4000);
                    });

                if (dialog.exec() != QDialog::Accepted) {
                    if (syncCoordinator_->isCalculatingChecksums())
                        syncCoordinator_->cancelChecksums();
                    closeSyncProgress(syncProgress_);
                    return;
                }

                const SyncExecutionPlan plan = dialog.executionPlan();
                qsizetype taskCount = 0;
                const quint64 batchId = syncCoordinator_->enqueuePlan(
                    plan, prepared.localRoot, prepared.remoteRoot,
                    transferMgr_->sessionIdentity(), &taskCount);
                if (batchId == 0 || taskCount == 0) {
                    statusBar()->showMessage(
                        tr("No synchronization operations were selected"),
                        4000);
                    return;
                }
                statusBar()->showMessage(
                    tr("Queued %1 synchronization operations")
                        .arg(taskCount),
                    6000);
                maybeShowTransferQueue();
            });
}

void MainWindow::showSyncDialog() {
    if (!rightIsRemote_ || !rightRemoteModel_ || !remoteOps_ ||
        !remoteOps_->hasRequestedSession()) {
        UiAlerts::warning(this, tr("Compare folders"),
                          tr("Connect to a browsable remote server first."));
        return;
    }
    if (!syncCoordinator_)
        initializeSyncCoordinator();
    if (!syncCoordinator_)
        return;
    if (syncCoordinator_->isPreparing()) {
        if (syncProgress_) {
            syncProgress_->show();
            syncProgress_->raise();
            syncProgress_->activateWindow();
        }
        return;
    }

    auto *progress = new QProgressDialog(
        tr("Preparing folder comparison…"), tr("Cancel"), 0, 0, this);
    progress->setWindowTitle(tr("Compare folders"));
    progress->setWindowModality(Qt::NonModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    syncProgress_ = progress;
    connect(progress, &QProgressDialog::canceled, syncCoordinator_,
            &SyncCoordinator::cancel);
    progress->show();
    syncCoordinator_->start(leftPath_->text(),
                            rightRemoteModel_->rootPath());
}
