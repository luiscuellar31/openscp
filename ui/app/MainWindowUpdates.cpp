#include "app/MainWindow.hpp"
#include "logic/common/AppSettings.hpp"
#include "logic/connections/SessionController.hpp"
#include "logic/transfers/TransferManager.hpp"
#include "logic/updates/MacUpdater.hpp"
#include "logic/updates/UpdateController.hpp"

#include <QDesktopServices>
#include <QMenu>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QThreadPool>
#include <QTimer>

using openscpui::AppSettings;
namespace settingskeys = openscpui::settingskeys;

bool MainWindow::canInstallUpdate() const {
    if (transferCleanupInProgress_ || sessionController_->isConnecting() ||
        sessionController_->isDisconnecting() || remoteScanInProgress_.load() ||
        localFsJobsInFlight_.load() != 0 ||
        !activeLocalUploadDiscoveries_.isEmpty() || activeRemoteListJob_ != 0 ||
        syncProgress_ ||
        QThreadPool::globalInstance()->activeThreadCount() != 0)
        return false;
    for (const auto &task : transferMgr_->tasksSnapshot())
        if (!isTerminalTransferStatus(task.status))
            return false;
    return true;
}

void MainWindow::initializeUpdates() {
    updates_ = new openscpui::UpdateController(this);
    updates_->setCanInstall([this] { return canInstallUpdate(); });
    macUpdater_ = new openscpui::MacUpdater(
        [this] {
            if (!canInstallUpdate())
                return false;
            if (sessionController_->hasSession()) {
                disconnectRemote();
                return false;
            }
            return true;
        },
        [this] {
            transferMgr_->persistNow();
            saveMainWindowUiState();
        },
        this);
    connect(macUpdater_, &openscpui::MacUpdater::finished, this,
            [this] { setEnabled(true); });
    auto *check = appMenu_->addAction(tr("Check for updates…"));
    check->setObjectName(QStringLiteral("checkForUpdatesAction"));
    check->setMenuRole(QAction::NoRole);
    connect(check, &QAction::triggered, updates_,
            [this] { updates_->check(true); });
    auto *automatic =
        appMenu_->addAction(tr("Automatically check for updates"));
    automatic->setObjectName(QStringLiteral("automaticUpdatesAction"));
    automatic->setMenuRole(QAction::NoRole);
    automatic->setCheckable(true);
    automatic->setChecked(
        AppSettings()
            .value(settingskeys::kUpdateAutomaticChecks, false)
            .toBool());
    connect(automatic, &QAction::toggled, updates_, [this](bool enabled) {
        AppSettings().setValue(settingskeys::kUpdateAutomaticChecks, enabled);
        if (enabled)
            updates_->check(false);
    });
    connect(updates_, &openscpui::UpdateController::checked, this,
            [this](bool manual, const QString &error) {
                if (!manual)
                    return;
                if (error.isEmpty())
                    QMessageBox::information(this, tr("Updates"),
                                             tr("OpenSCP is up to date."));
                else
                    QMessageBox::warning(this, tr("Updates"),
                                         tr("Could not check for updates.") +
                                             QLatin1Char('\n') + error);
            });
    connect(updates_, &openscpui::UpdateController::available, this,
            &MainWindow::showAvailableUpdate);
    QTimer::singleShot(3000, updates_, [this] { updates_->check(false); });
    auto *timer = new QTimer(updates_);
    connect(timer, &QTimer::timeout, updates_,
            [this] { updates_->check(false); });
    timer->start(60 * 60 * 1000);
}

void MainWindow::showAvailableUpdate(const openscpui::UpdateRelease &release) {
    using openscpui::UpdateInstallation;
    const auto kind = updates_->installation();
    const bool native =
        kind == UpdateInstallation::MacBundle && macUpdater_->available();
    const bool installable = native || release.artifact.has_value();
    QString text = tr("OpenSCP %1 is available.").arg(release.version);
    if (kind == UpdateInstallation::Flatpak)
        text += QLatin1Char('\n') +
                tr("For repository installations, use your software manager or "
                   "run: flatpak update io.github.luiscuellar31.openscp. For "
                   "standalone bundles, download the new bundle from the "
                   "release page.");
    else if (kind == UpdateInstallation::Snap)
        text +=
            QLatin1Char('\n') +
            tr("For Snap Store installations, Snap manages updates "
               "automatically. To request one, run: sudo snap refresh openscp");
    else if (!installable)
        text +=
            QLatin1Char('\n') +
            tr("Download and install the new version from the release page.");
    QMessageBox prompt(QMessageBox::Information, tr("Updates"), text,
                       QMessageBox::NoButton, this);
    prompt.setTextFormat(Qt::PlainText);
    auto *accept = prompt.addButton(installable ? tr("Download and install…")
                                                : tr("Open release page"),
                                    QMessageBox::AcceptRole);
    auto *skip =
        prompt.addButton(tr("Skip this version"), QMessageBox::RejectRole);
    prompt.addButton(tr("Later"), QMessageBox::RejectRole);
    prompt.exec();
    if (prompt.clickedButton() == skip) {
        AppSettings().setValue(settingskeys::kUpdateSkippedVersion,
                               release.version);
        return;
    }
    if (prompt.clickedButton() != accept)
        return;
    if (!installable) {
        QDesktopServices::openUrl(release.page);
        return;
    }
    if (updates_->busy() || !canInstallUpdate()) {
        QMessageBox::information(
            this, tr("Updates"),
            tr("Finish or cancel pending transfers and file operations before "
               "installing an update."));
        return;
    }
    if (native) {
        // Prevent new work while Sparkle's standard UI downloads and installs.
        // The delegate waits for connection cleanup before allowing relaunch.
        setEnabled(false);
        if (!macUpdater_->check())
            setEnabled(true);
        return;
    }
    auto *progress = new QProgressDialog(tr("Downloading update…"),
                                         tr("Cancel"), 0, 100, this);
    progress->setWindowModality(Qt::ApplicationModal);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    progress->setMinimumDuration(0);
    connect(progress, &QProgressDialog::canceled, updates_,
            &openscpui::UpdateController::cancel);
    connect(updates_, &openscpui::UpdateController::progress, progress,
            [progress](qint64 received, qint64 total) {
                progress->setValue(static_cast<int>(received * 100 / total));
                if (received == total) {
                    progress->setLabelText(
                        MainWindow::tr("Verifying and installing update…"));
                    progress->setCancelButton(nullptr);
                }
            });
    connect(updates_, &openscpui::UpdateController::installed, progress,
            [this, progress](bool success, const QString &error) {
                progress->deleteLater();
                if (success)
                    QMessageBox::information(
                        this, tr("Updates"),
                        tr("Update installed. Close OpenSCP and launch it "
                           "again to use the new version. The previous "
                           "AppImage is saved with the .previous suffix.") +
                            (error.isEmpty() ? QString()
                                             : QLatin1Char('\n') + error));
                else
                    QMessageBox::warning(
                        this, tr("Updates"),
                        tr("The update could not be installed. Your current "
                           "version is still available.") +
                            QLatin1Char('\n') + error);
            });
    transferMgr_->persistNow();
    updates_->install(*release.artifact);
}
