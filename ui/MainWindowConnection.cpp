// MainWindow connection/session/security and saved-site persistence logic.
#include "MainWindow.hpp"
#include "ConnectionDialog.hpp"
#include "MainWindowSharedUtils.hpp"
#include "RemoteModel.hpp"
#include "RemoteOperationController.hpp"
#include "SavedSitesPersistence.hpp"
#include "SessionController.hpp"
#include "SiteCredentialRepository.hpp"
#include "SiteManagerDialog.hpp"
#include "SyncCoordinator.hpp"
#include "TransferManager.hpp"
#include "UiAlerts.hpp"
#include "openscp/ClientFactory.hpp"
#include "openscp/RuntimeLogging.hpp"

#include <QAbstractButton>
#include <QApplication>
#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QGuiApplication>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QProgressDialog>
#include <QPushButton>
#include <QSettings>
#include <QStringList>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QThreadPool>
#include <QtGlobal>
#include <QUuid>

#include <atomic>
#include <cstdio>
#include <memory>

// Best-effort memory scrubbing helpers for sensitive data
static inline void secureClear(QString &text) {
    const int charCount = text.size();
    for (int charIndex = 0; charIndex < charCount; ++charIndex)
        text[charIndex] = QChar(u'\0');
    text.clear();
}
static inline void secureClear(QByteArray &bytes) {
    if (bytes.isEmpty())
        return;
    volatile char *bytePtr = reinterpret_cast<volatile char *>(bytes.data());
    const int byteCount = bytes.size();
    for (int byteIndex = 0; byteIndex < byteCount; ++byteIndex)
        bytePtr[byteIndex] = 0;
    bytes.clear();
    bytes.squeeze();
}
static QString newQuickSiteId() {
    return QUuid::createUuid().toString(QUuid::WithoutBraces);
}

static QString normalizedIdentityHost(const std::string &host) {
    return QString::fromStdString(host).trimmed().toLower();
}

static QString normalizedIdentityUser(const std::string &user) {
    return QString::fromStdString(user).trimmed();
}

static QString normalizedIdentityProtocol(openscp::Protocol protocol) {
    return QString::fromLatin1(openscp::protocolStorageName(protocol));
}

static openscp::ScpTransferMode normalizedIdentityScpMode(
    const openscp::SessionOptions &opt) {
    if (opt.protocol != openscp::Protocol::Scp)
        return openscp::ScpTransferMode::Auto;
    return opt.scp_transfer_mode;
}

static QString normalizedIdentityProxyHost(const std::string &host) {
    return QString::fromStdString(host).trimmed().toLower();
}

static QString normalizedIdentityProxyUser(
    const std::optional<std::string> &user) {
    if (!user || user->empty())
        return {};
    return QString::fromStdString(*user).trimmed();
}

static QString normalizedIdentityJumpHost(
    const std::optional<std::string> &host) {
    if (!host || host->empty())
        return {};
    return QString::fromStdString(*host).trimmed().toLower();
}

static QString normalizedIdentityJumpUser(
    const std::optional<std::string> &user) {
    if (!user || user->empty())
        return {};
    return QString::fromStdString(*user).trimmed();
}

static QString protocolDisplayLabel(openscp::Protocol protocol) {
    return QString::fromLatin1(openscp::protocolDisplayName(protocol));
}

static QString normalizeRemotePanelPath(const QString &rawPath) {
    return normalizeRemotePath(rawPath);
}

static QString
normalizedIdentityKeyPath(const std::optional<std::string> &keyPath) {
    if (!keyPath || keyPath->empty())
        return {};
    return QDir::cleanPath(
        QDir::fromNativeSeparators(QString::fromStdString(*keyPath).trimmed()));
}

static bool hasConfiguredJumpHost(const openscp::SessionOptions &opt) {
    return opt.jump_host.has_value() && !opt.jump_host->empty();
}

static bool hasTransportSelectionConflict(const openscp::SessionOptions &opt) {
    return opt.proxy_type != openscp::ProxyType::None &&
           hasConfiguredJumpHost(opt);
}

static bool sameSavedSiteIdentity(const openscp::SessionOptions &a,
                                  const openscp::SessionOptions &b) {
    const bool compareFtpsTls =
        (a.protocol != openscp::Protocol::Ftps) ||
        (openscp::normalizeFtpsMode(a.ftps_mode) ==
             openscp::normalizeFtpsMode(b.ftps_mode) &&
         a.ftps_verify_peer == b.ftps_verify_peer &&
         normalizedIdentityKeyPath(a.ftps_ca_cert_path) ==
             normalizedIdentityKeyPath(b.ftps_ca_cert_path));
    const bool compareWebDavTls =
        (a.protocol != openscp::Protocol::WebDav) ||
        (a.webdav_scheme == b.webdav_scheme &&
         openscp::normalizeWebDavBasePath(a.webdav_base_path) ==
             openscp::normalizeWebDavBasePath(b.webdav_base_path) &&
         a.webdav_verify_peer == b.webdav_verify_peer &&
         normalizedIdentityKeyPath(a.webdav_ca_cert_path) ==
             normalizedIdentityKeyPath(b.webdav_ca_cert_path));
    return normalizedIdentityProtocol(a.protocol) ==
               normalizedIdentityProtocol(b.protocol) &&
           normalizedIdentityScpMode(a) == normalizedIdentityScpMode(b) &&
           normalizedIdentityHost(a.host) == normalizedIdentityHost(b.host) &&
           a.port == b.port &&
           normalizedIdentityUser(a.username) ==
               normalizedIdentityUser(b.username) &&
           a.proxy_type == b.proxy_type &&
           normalizedIdentityProxyHost(a.proxy_host) ==
               normalizedIdentityProxyHost(b.proxy_host) &&
           a.proxy_port == b.proxy_port &&
           normalizedIdentityProxyUser(a.proxy_username) ==
               normalizedIdentityProxyUser(b.proxy_username) &&
           normalizedIdentityJumpHost(a.jump_host) ==
               normalizedIdentityJumpHost(b.jump_host) &&
           a.jump_port == b.jump_port &&
           normalizedIdentityJumpUser(a.jump_username) ==
               normalizedIdentityJumpUser(b.jump_username) &&
           normalizedIdentityKeyPath(a.jump_private_key_path) ==
               normalizedIdentityKeyPath(b.jump_private_key_path) &&
           normalizedIdentityKeyPath(a.private_key_path) ==
               normalizedIdentityKeyPath(b.private_key_path) &&
           compareFtpsTls &&
           compareWebDavTls;
}

struct QuickSitesLoadResult {
    QVector<SiteEntry> sites;
    bool needsSave = false;
    SiteCredentialMigrationResult legacyMigration;
};

static QuickSitesLoadResult loadSavedSitesForQuickConnect() {
    const SavedSitesPersistence::LoadResult loaded =
        SavedSitesPersistence::loadSites({
            .trimSiteNames = true,
            .createNewId = [] { return newQuickSiteId(); },
        });
    QuickSitesLoadResult result;
    result.sites = loaded.sites;
    result.needsSave = loaded.needsSave;
    result.legacyMigration =
        SiteCredentialRepository::migrateLegacyPlaintext(loaded);
    return result;
}

static void saveSavedSitesForQuickConnect(const QVector<SiteEntry> &sites) {
    SavedSitesPersistence::saveSites(sites, true);
}

static QString defaultQuickSiteName(const openscp::SessionOptions &opt) {
    const QString user = normalizedIdentityUser(opt.username);
    const QString host = normalizedIdentityHost(opt.host);
    const QString protocol = protocolDisplayLabel(opt.protocol);
    QString out;
    if (!user.isEmpty() && !host.isEmpty())
        out = QString("%1@%2").arg(user, host);
    else if (!host.isEmpty())
        out = host;
    else if (!user.isEmpty())
        out = user;
    else
        out = QObject::tr("New site");
    if (!host.isEmpty() &&
        opt.port != openscp::defaultPortForProtocol(opt.protocol))
        out += QString(":%1").arg(opt.port);
    if (opt.protocol != openscp::Protocol::Sftp)
        out = QString("%1 (%2)").arg(out, protocol);
    return out;
}

static QString ensureUniqueQuickSiteName(const QVector<SiteEntry> &sites,
                                         const QString &preferred) {
    QString base = preferred.trimmed();
    if (base.isEmpty())
        base = QObject::tr("New site");
    auto exists = [&](const QString &candidate) {
        for (const auto &site : sites) {
            if (site.name.compare(candidate, Qt::CaseInsensitive) == 0)
                return true;
        }
        return false;
    };
    if (!exists(base))
        return base;
    for (int suffixNumber = 2; suffixNumber < 10000; ++suffixNumber) {
        const QString candidate =
            QString("%1 (%2)").arg(base).arg(suffixNumber);
        if (!exists(candidate))
            return candidate;
    }
    return base +
           QString(" (%1)").arg(
               QUuid::createUuid().toString(QUuid::WithoutBraces).left(6));
}

static void refreshOpenSiteManagerWidget(QPointer<QWidget> siteManager) {
    if (!siteManager)
        return;
    auto *dlg = qobject_cast<SiteManagerDialog *>(siteManager.data());
    if (!dlg)
        return;
    dlg->reloadFromSettings();
}

bool MainWindow::isLikelyRemoteTransportError(const QString &rawError) const {
    const QString lower = rawError.trimmed().toLower();
    if (lower.isEmpty())
        return false;

    // Permission/auth/path problems should not trigger reconnect logic.
    if (lower.contains("permission denied") || lower.contains("read-only") ||
        lower.contains("no such file") || lower.contains("not found") ||
        lower.contains("auth fail") || lower.contains("authentication failed"))
        return false;

    static const QStringList markers = {
        QStringLiteral("socket send"),
        QStringLiteral("socket recv"),
        QStringLiteral("socket error"),
        QStringLiteral("session disconnected"),
        QStringLiteral("channel closed"),
        QStringLiteral("connection lost"),
        QStringLiteral("connection reset"),
        QStringLiteral("connection aborted"),
        QStringLiteral("broken pipe"),
        QStringLiteral("transport endpoint is not connected"),
        QStringLiteral("end of file"),
        QStringLiteral("timeout"),
        QStringLiteral("timed out"),
        QStringLiteral("rc=-7"),  // LIBSSH2_ERROR_SOCKET_SEND
        QStringLiteral("rc=-34"), // LIBSSH2_ERROR_SOCKET_RECV
        QStringLiteral("rc=-37"), // LIBSSH2_ERROR_CHANNEL_CLOSED
        QStringLiteral("rc=-13"), // LIBSSH2_ERROR_SOCKET_DISCONNECT
    };
    for (const QString &marker : markers) {
        if (lower.contains(marker))
            return true;
    }
    return false;
}

void MainWindow::ensureRemoteSessionHealthMonitoring() {
    if (remoteSessionHealthTimer_)
        return;

    remoteSessionHealthTimer_ = new QTimer(this);
    remoteSessionHealthTimer_->setSingleShot(false);
    remoteSessionHealthTimer_->setInterval(remoteSessionHealthIntervalMs_);
    connect(remoteSessionHealthTimer_, &QTimer::timeout, this, [this] {
        runRemoteSessionHealthCheck(tr("periodic"), false);
    });

    auto *guiApp = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
    if (!guiApp)
        return;
    connect(guiApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) {
                const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                if (state == Qt::ApplicationActive) {
                    if (lastAppInactiveAtMs_ <= 0) {
                        lastAppInactiveAtMs_ = 0;
                        return;
                    }
                    const qint64 inactiveMs = nowMs - lastAppInactiveAtMs_;
                    lastAppInactiveAtMs_ = 0;
                    constexpr qint64 kResumeProbeThresholdMs = 60 * 1000;
                    if (inactiveMs >= kResumeProbeThresholdMs && rightIsRemote_ &&
                        sessionController_->client()) {
                        runRemoteSessionHealthCheck(
                            tr("resume (%1s)").arg(inactiveMs / 1000), true);
                    }
                    return;
                }
                lastAppInactiveAtMs_ = nowMs;
            });
}

void MainWindow::startRemoteSessionHealthMonitoring() {
    if (!rightIsRemote_ || !sessionController_->client())
        return;
    ensureRemoteSessionHealthMonitoring();
    if (!remoteSessionHealthTimer_)
        return;
    if (remoteSessionHealthIntervalMs_ < 60000)
        remoteSessionHealthIntervalMs_ = 60000;
    remoteSessionHealthTimer_->setInterval(remoteSessionHealthIntervalMs_);
    remoteSessionHealthProbeInFlight_.store(false);
    lastAppInactiveAtMs_ = 0;
    if (!remoteSessionHealthTimer_->isActive())
        remoteSessionHealthTimer_->start();
}

void MainWindow::stopRemoteSessionHealthMonitoring() {
    if (remoteSessionHealthTimer_)
        remoteSessionHealthTimer_->stop();
    if (remoteOps_ && activeRemoteHealthJob_ != 0)
        remoteOps_->cancel(activeRemoteHealthJob_);
    activeRemoteHealthJob_ = 0;
    activeRemoteHealthReason_.clear();
    activeRemoteHealthForced_ = false;
    remoteSessionHealthProbeInFlight_.store(false);
    lastAppInactiveAtMs_ = 0;
}

void MainWindow::runRemoteSessionHealthCheck(const QString &reason, bool force) {
    if (!rightIsRemote_ || !sessionController_->client() || !remoteOps_ ||
        !remoteOps_->hasRequestedSession() ||
        !sessionController_->options().has_value())
        return;
    if (sessionController_->isDisconnecting() ||
        sessionController_->isConnecting())
        return;
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    constexpr qint64 kRecentRemoteActivityMs = 60 * 1000;
    if (!force && lastSuccessfulRemoteActivityAtMs_ > 0 &&
        nowMs - lastSuccessfulRemoteActivityAtMs_ <
            kRecentRemoteActivityMs) {
        return;
    }
    bool expected = false;
    if (!remoteSessionHealthProbeInFlight_.compare_exchange_strong(expected,
                                                                     true)) {
        return;
    }

    const QString probePath =
        (rightRemoteModel_ && !rightRemoteModel_->rootPath().isEmpty())
            ? rightRemoteModel_->rootPath()
            : QStringLiteral("/");
    RemoteOperationController::HealthCheckRequest request;
    request.path = probePath;
    activeRemoteHealthReason_ = reason;
    activeRemoteHealthForced_ = force;
    activeRemoteHealthJob_ = remoteOps_->submit(request);
    if (activeRemoteHealthJob_ == 0) {
        remoteSessionHealthProbeInFlight_.store(false);
        activeRemoteHealthReason_.clear();
        activeRemoteHealthForced_ = false;
    }
}

void MainWindow::openConnectDialogWithPreset(
    const std::optional<openscp::SessionOptions> &preset) {
    ConnectionDialog dlg(this);
    dlg.setQuickConnectSaveOptionsVisible(true);
    if (preset.has_value())
        dlg.setOptions(*preset);
    if (dlg.exec() != QDialog::Accepted)
        return;
    auto sessionOptions = dlg.options();
    std::optional<PendingSiteSaveRequest> saveRequest = std::nullopt;
    if (dlg.saveSiteRequested()) {
        PendingSiteSaveRequest req;
        req.siteName = dlg.siteName();
        req.initialLocalPath = dlg.initialLocalPath();
        req.initialRemotePath = dlg.initialRemotePath();
        req.saveCredentials = dlg.saveCredentialsRequested();
        req.rememberLastPaths = dlg.rememberLastPaths();
        saveRequest = req;
    }
    // Apply global security preferences also for ad‑hoc connections (Advanced
    // settings)
    {
        QSettings securitySettings("OpenSCP", "OpenSCP");
        sessionOptions.known_hosts_hash_names =
            securitySettings.value("Security/knownHostsHashed", true).toBool();
        sessionOptions.show_fp_hex =
            securitySettings.value("Security/fpHex", false).toBool();
    }
    if (hasTransportSelectionConflict(sessionOptions)) {
        UiAlerts::warning(
            this, tr("Invalid transport configuration"),
            tr("Proxy and SSH jump host cannot be used together in the same "
               "connection.\nChoose only one transport method."));
        statusBar()->showMessage(
            tr("Connection canceled: invalid transport configuration"), 5000);
        return;
    }
#ifdef Q_OS_WIN
    if (hasConfiguredJumpHost(sessionOptions)) {
        UiAlerts::warning(
            this, tr("Unsupported transport"),
            tr("SSH jump host is currently unavailable on Windows."));
        statusBar()->showMessage(
            tr("Connection canceled: SSH jump host is unsupported on Windows"),
            5000);
        return;
    }
#endif
    if (saveRequest.has_value()) {
        maybePersistQuickConnectSite(sessionOptions, *saveRequest, false);
        // Already persisted on request; connection lifecycle no longer needs to
        // do it.
        saveRequest.reset();
    }
    startSftpConnect(sessionOptions, saveRequest);
}

void MainWindow::connectSftp() { openConnectDialogWithPreset(std::nullopt); }

// Tear down the current remote session and restore local mode.
quint64 MainWindow::beginDisconnectFlow() {
    stopRemoteSessionHealthMonitoring();
    const quint64 disconnectSeq =
        sessionController_->beginDisconnect();
    transferCleanupInProgress_ = (transferMgr_ != nullptr);
    transferCleanupStartedAtMs_ =
        transferCleanupInProgress_ ? QDateTime::currentMSecsSinceEpoch() : 0;
    saveRightHeaderState(true);
    if (actDisconnect_)
        actDisconnect_->setEnabled(false);
    if (actConnect_) {
        actConnect_->setEnabled(false);
        actConnect_->setToolTip(
            tr("Please wait while active transfers are canceled"));
    }

    if (remoteScanCancelRequested_)
        remoteScanCancelRequested_->store(true);
    if (remoteScanProgress_) {
        remoteScanProgress_->hide();
        remoteScanProgress_->deleteLater();
        remoteScanProgress_.clear();
    }
    remoteScanInProgress_ = false;
    applyDisconnectLocalUiState();
    statusBar()->showMessage(
        tr("Disconnecting… waiting for active transfers to stop"), 0);
    return disconnectSeq;
}

void MainWindow::applyDisconnectLocalUiState() {
    auto setActionEnabled = [](QAction *action, bool enabled) {
        if (action)
            action->setEnabled(enabled);
    };
    if (remoteOps_) {
        if (activeRemoteListJob_ != 0)
            remoteOps_->cancel(activeRemoteListJob_);
        activeRemoteListJob_ = 0;
        remoteOps_->clearSession();
    }
    if (syncCoordinator_)
        syncCoordinator_->cancel();
    cancelLocalUploadDiscoveries();
    if (syncProgress_) {
        syncProgress_->hide();
        syncProgress_->deleteLater();
        syncProgress_.clear();
    }
    // Switch UI immediately to local/local so the app remains usable while
    // transfer workers unwind in the background.
    if (rightRemoteModel_) {
        rightView_->setModel(rightLocalModel_);
        if (rightView_->selectionModel()) {
            connect(rightView_->selectionModel(),
                    &QItemSelectionModel::selectionChanged, this,
                    [this] { updateDeleteShortcutEnables(); });
        }
        delete rightRemoteModel_;
        rightRemoteModel_ = nullptr;
    }
    rightIsRemote_ = false;
    activateScpTransferModeUi(false);
    transferUiController_.reset();
    restoreRightHeaderState(false);
    if (QDir(rightPath_->text()).exists()) {
        setRightRoot(rightPath_->text());
    } else {
        setRightRoot(QDir::homePath());
    }
    rightRemoteMutationsSupported_ = false;
    sessionController_->clearOptions();
    sessionNoHostVerification_ = false;
    updateHostPolicyRiskBanner();
    setActionEnabled(actDownloadF7_, false);
    setActionEnabled(actUploadRight_, false);
    setActionEnabled(actRefreshRight_, false);
    setActionEnabled(actOpenTerminalRight_, false);
    setActionEnabled(actSync_, false);
    // Local mode: re-enable local actions on the right panel
    setActionEnabled(actNewDirRight_, true);
    setActionEnabled(actNewFileRight_, true);
    setActionEnabled(actRenameRight_, true);
    setActionEnabled(actDeleteRight_, true);
    setActionEnabled(actMoveRight_, true);
    setActionEnabled(actMoveRightTb_, true);
    setActionEnabled(actCopyRightTb_, true);
    if (actChooseRight_) {
        actChooseRight_->setIcon(
            QIcon(QLatin1String(":/assets/icons/action-open-folder.svg")));
        actChooseRight_->setEnabled(true);
        actChooseRight_->setToolTip(actChooseRight_->text());
    }
    if (rightView_)
        rightView_->setEnabled(true);
    setWindowTitle(tr("OpenSCP — local/local"));
    updateDeleteShortcutEnables();
}

void MainWindow::scheduleDisconnectWatchdog(quint64 disconnectSeq) {
    constexpr int kDisconnectWatchdogMs = 25000;
    QTimer::singleShot(kDisconnectWatchdogMs, this, [this, disconnectSeq]() {
        if (!sessionController_->isCurrentDisconnect(disconnectSeq))
            return;
        statusBar()->showMessage(
            tr("Disconnect timeout reached; forcing local mode while cleanup "
               "continues"),
            5000);
        completeDisconnectSftp(disconnectSeq, true);
    });
}

bool MainWindow::runDisconnectTransferCleanupAsync(quint64 disconnectSeq) {
    // Stop transfer workers off the UI thread; clearClient() may need to join
    // active workers and can block while they unwind.
    if (!transferMgr_)
        return false;
    QPointer<MainWindow> self(this);
    TransferManager *mgr = transferMgr_;
    QThreadPool::globalInstance()->start([self, mgr, disconnectSeq]() {
        try {
            mgr->clearClient();
        } catch (...) {
            // Best effort: continue UI teardown even if queue cleanup
            // throws unexpectedly.
        }
        QObject *app = QCoreApplication::instance();
        if (!app)
            return;
        QMetaObject::invokeMethod(
            app,
            [self, disconnectSeq]() {
                if (!self)
                    return;
                if (disconnectSeq ==
                        self->sessionController_->disconnectSequence() &&
                    self->transferCleanupInProgress_) {
                    self->transferCleanupInProgress_ = false;
                    self->transferCleanupStartedAtMs_ = 0;
                    if (!self->sessionController_->isDisconnecting() &&
                        !self->rightIsRemote_) {
                        if (self->actConnect_)
                            self->actConnect_->setToolTip(
                                self->actConnect_->text());
                        self->statusBar()->showMessage(
                            tr("Background transfer cleanup finished"), 3000);
                    }
                }
                self->completeDisconnectSftp(disconnectSeq, false);
                if (!self->sessionController_->isDisconnecting() &&
                    !self->transferCleanupInProgress_ &&
                    self->pendingCloseAfterDisconnect_) {
                    self->pendingCloseAfterDisconnect_ = false;
                    QTimer::singleShot(0, self,
                                       [self] {
                                           if (self)
                                               self->close();
                                       });
                }
            },
            Qt::QueuedConnection);
    });
    return true;
}

void MainWindow::disconnectSftp() {
    if (sessionController_->isDisconnecting())
        return;

    if (transferMgr_) {
        const QString sessionKey = transferMgr_->sessionIdentity();
        QVector<quint64> activeTaskIds;
        for (const TransferTask &task : transferMgr_->tasksSnapshot()) {
            const bool belongsToSession =
                sessionKey.isEmpty() || task.sessionKey.isEmpty() ||
                task.sessionKey == sessionKey;
            const bool active =
                task.status == TransferTask::Status::Queued ||
                task.status == TransferTask::Status::Running ||
                task.status == TransferTask::Status::RetryWaiting;
            if (belongsToSession && active)
                activeTaskIds.push_back(task.taskId);
        }
        if (!activeTaskIds.isEmpty()) {
            QMessageBox choice(this);
            UiAlerts::configure(choice);
            choice.setIcon(QMessageBox::Question);
            choice.setWindowTitle(tr("Transfers are still active"));
            choice.setText(
                tr("%1 transfer(s) are still active for this session.")
                    .arg(activeTaskIds.size()));
            choice.setInformativeText(
                tr("Choose whether to keep their progress for the next "
                   "connection, cancel them, or stay connected."));
            auto *pauseButton = choice.addButton(
                tr("Pause and disconnect"), QMessageBox::AcceptRole);
            auto *cancelButton = choice.addButton(
                tr("Cancel and disconnect"), QMessageBox::DestructiveRole);
            auto *stayButton = choice.addButton(
                tr("Stay connected"), QMessageBox::RejectRole);
            choice.setDefaultButton(
                qobject_cast<QPushButton *>(stayButton));
            choice.exec();
            if (choice.clickedButton() == stayButton ||
                !choice.clickedButton()) {
                return;
            }
            if (choice.clickedButton() == cancelButton) {
                for (quint64 taskId : activeTaskIds)
                    transferMgr_->cancelTask(taskId);
            } else if (choice.clickedButton() != pauseButton) {
                return;
            }
        }
    }

    persistActiveSitePaths();
    const quint64 disconnectSeq = beginDisconnectFlow();
    scheduleDisconnectWatchdog(disconnectSeq);

    if (runDisconnectTransferCleanupAsync(disconnectSeq))
        return;

    transferCleanupInProgress_ = false;
    transferCleanupStartedAtMs_ = 0;
    completeDisconnectSftp(disconnectSeq, false);
}

void MainWindow::persistActiveSitePaths() {
    if (!activeSavedSiteContext_ ||
        !activeSavedSiteContext_->rememberLastPaths ||
        activeSavedSiteContext_->siteId.trimmed().isEmpty()) {
        return;
    }

    QString localPath;
    if (leftView_ && leftModel_) {
        const QModelIndex rootIndex = leftView_->rootIndex();
        if (rootIndex.isValid())
            localPath = leftModel_->filePath(rootIndex).trimmed();
    }
    if (localPath.isEmpty() && leftPath_)
        localPath = leftPath_->text().trimmed();
    QString remotePath;
    if (rightRemoteModel_)
        remotePath = rightRemoteModel_->rootPath().trimmed();
    if (remotePath.isEmpty() && rightPath_)
        remotePath = rightPath_->text().trimmed();
    if (remotePath.isEmpty())
        remotePath = QStringLiteral("/");

    const auto loaded = SavedSitesPersistence::loadSites();
    const SiteCredentialMigrationResult migration =
        SiteCredentialRepository::migrateLegacyPlaintext(loaded);
    if (!migration.complete) {
        UiAlerts::warning(
            this, tr("Paths not saved"),
            tr("The last paths could not be saved because one or more legacy "
               "credentials could not be moved to the secure backend:\n%1")
                .arg(migration.issues.join(QLatin1Char('\n'))));
        return;
    }
    QVector<SiteEntry> sites = loaded.sites;
    bool updated = false;
    for (SiteEntry &site : sites) {
        if (site.siteId != activeSavedSiteContext_->siteId)
            continue;
        site.initialLocalPath = localPath;
        site.initialRemotePath = remotePath;
        site.rememberLastPaths = true;
        updated = true;
        break;
    }
    if (updated)
        SavedSitesPersistence::saveSites(sites, true);
}

void MainWindow::completeDisconnectSftp(quint64 disconnectSeq, bool forced) {
    if (!sessionController_->isCurrentDisconnect(disconnectSeq))
        return;
    activeSavedSiteContext_.reset();
    pendingSavedSiteContext_.reset();
    sessionController_->disconnectClient();
    resetConnectionSessionIndicators();
    if (actConnect_) {
        actConnect_->setEnabled(true);
        actConnect_->setToolTip(actConnect_->text());
    }

    // Per spec: non‑modal Site Manager after disconnect (if enabled), without
    // blocking UI
    sessionController_->finishDisconnect(disconnectSeq);
    if (forced) {
        statusBar()->showMessage(
            tr("Disconnected (transfer cleanup still finishing in background)"),
            5000);
    } else {
        statusBar()->showMessage(tr("Disconnected"), 3000);
    }
    if (pendingCloseAfterDisconnect_) {
        pendingCloseAfterDisconnect_ = false;
        QTimer::singleShot(0, this, [this] { close(); });
        return;
    }
    if (!QCoreApplication::closingDown() && openSiteManagerOnDisconnect_) {
        QTimer::singleShot(0, this, [this] { showSiteManagerNonModal(); });
    }
}

void MainWindow::setOpenSiteManagerOnDisconnect(bool enabled) {
    if (openSiteManagerOnDisconnect_ == enabled)
        return;
    openSiteManagerOnDisconnect_ = enabled;
    QSettings settings("OpenSCP", "OpenSCP");
    settings.setValue("UI/openSiteManagerOnDisconnect", enabled);
    settings.sync();
}

void MainWindow::showSiteManagerNonModal() {
    if (QApplication::activeModalWidget()) {
        pendingOpenSiteManager_ = true;
        QObject *modal = QApplication::activeModalWidget();
        if (modal)
            connect(modal, &QObject::destroyed, this,
                    &MainWindow::maybeOpenSiteManagerAfterModal,
                    Qt::UniqueConnection);
        return; // don't open underneath a modal
    }
    if (siteManager_) {
        siteManager_->show();
        siteManager_->raise();
        siteManager_->activateWindow();
        return;
    }
    auto *dlg = new SiteManagerDialog(this);
    siteManager_ = dlg;
    dlg->setAttribute(Qt::WA_DeleteOnClose, true);
    connect(dlg, &QObject::destroyed, this, [this] { siteManager_.clear(); });
    connect(dlg, &QDialog::finished, this, [this, dlg](int dialogResult) {
        if (dialogResult == QDialog::Accepted && dlg) {
            SiteEntry site;
            if (dlg->selectedSite(site))
                startSavedSiteConnect(site);
        }
    });
    QTimer::singleShot(0, dlg, [dlg] {
        dlg->show();
        dlg->raise();
        dlg->activateWindow();
    });
}

void MainWindow::setOpenSiteManagerOnStartup(bool enabled) {
    if (openSiteManagerOnStartup_ == enabled)
        return;
    openSiteManagerOnStartup_ = enabled;
    QSettings settings("OpenSCP", "OpenSCP");
    settings.setValue("UI/showConnOnStart", enabled);
    settings.sync();
}

void MainWindow::maybeOpenSiteManagerAfterModal() {
    if (!QApplication::activeModalWidget() && pendingOpenSiteManager_) {
        pendingOpenSiteManager_ = false;
        QTimer::singleShot(0, this, [this] { showSiteManagerNonModal(); });
    }
}

bool MainWindow::confirmInsecureHostPolicyForSession(
    const openscp::SessionOptions &opt) {
    const bool unencryptedFtp = opt.protocol == openscp::Protocol::Ftp;
    const bool unencryptedWebDav =
        opt.protocol == openscp::Protocol::WebDav &&
        opt.webdav_scheme == openscp::WebDavScheme::Http;
    const bool unverifiedTls =
        (opt.protocol == openscp::Protocol::Ftps &&
         !opt.ftps_verify_peer) ||
        (opt.protocol == openscp::Protocol::WebDav &&
         opt.webdav_scheme == openscp::WebDavScheme::Https &&
         !opt.webdav_verify_peer);
    const bool unverifiedSsh =
        openscp::capabilitiesForProtocol(opt.protocol).supports_known_hosts &&
        opt.known_hosts_policy == openscp::KnownHostsPolicy::Off;
    const bool unencrypted = unencryptedFtp || unencryptedWebDav;
    if (!unencrypted && !unverifiedTls && !unverifiedSsh)
        return true;

    const QString hostKey =
        QString::fromStdString(opt.host).trimmed().toLower();
    const QString riskKey =
        unencryptedFtp
            ? QStringLiteral("ftp")
            : (unencryptedWebDav
                   ? QStringLiteral("webdav-http")
                   : (unverifiedTls ? QStringLiteral("tls-unverified")
                                    : QStringLiteral("ssh-unverified")));
    const QString allowKey =
        QString("Security/insecureTransportConfirmedUntilUtc/%1/%2:%3")
            .arg(riskKey, hostKey)
            .arg((int)opt.port);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QSettings settings("OpenSCP", "OpenSCP");
    const qint64 allowedUntil = settings.value(allowKey, 0).toLongLong();
    if (allowedUntil > now)
        return true;

    const QString warningText =
        unencrypted
            ? tr("This connection does not encrypt credentials, file names, "
                 "or file contents. Anyone able to observe the network may "
                 "read or modify them.\n\n"
                 "Continue only for a trusted network or legacy server?")
            : (unverifiedTls
                   ? tr("TLS encryption is enabled, but the server "
                        "certificate will not be verified. This allows "
                        "server impersonation and man-in-the-middle "
                        "attacks.\n\nContinue at your own risk?")
                   : tr("The SSH host key will not be verified. This allows "
                        "server impersonation and man-in-the-middle "
                        "attacks.\n\nContinue at your own risk?"));
    const auto first = UiAlerts::warning(
        this, unencrypted ? tr("Insecure connection")
                          : tr("Critical security risk"),
        warningText, QMessageBox::Yes | QMessageBox::Cancel,
        QMessageBox::Cancel);
    if (first != QMessageBox::Yes)
        return false;

    if (!unencrypted) {
        const QString token = QStringLiteral("UNSAFE");
        bool inputAccepted = false;
        const QString entered =
            QInputDialog::getText(this, tr("Additional confirmation required"),
                                  tr("To confirm, type exactly %1").arg(token),
                                  QLineEdit::Normal, QString(), &inputAccepted)
                .trimmed();
        if (!inputAccepted || entered != token) {
            UiAlerts::information(
                this, tr("Connection canceled"),
                tr("Risk confirmation was not completed correctly."));
            return false;
        }
    }

    // Temporary exception per host:port to avoid persistent bypasses.
    const int ttlMin = qBound(1, prefNoHostVerificationTtlMin_, 120);
    const qint64 newUntil = now + qint64(ttlMin) * 60;
    settings.setValue(allowKey, newUntil);
    settings.sync();
    const QDateTime expLocal =
        QDateTime::fromSecsSinceEpoch(newUntil).toLocalTime();
    statusBar()->showMessage(
        tr("Temporary security exception active until %1")
            .arg(QLocale().toString(expLocal, QLocale::ShortFormat)),
        8000);
    return true;
}

void MainWindow::updateHostPolicyRiskBanner() {
    const bool show =
        rightIsRemote_ && !activeSecurityWarning_.trimmed().isEmpty();
    if (!show) {
        if (hostPolicyRiskLabel_)
            hostPolicyRiskLabel_->hide();
        return;
    }
    if (!hostPolicyRiskLabel_) {
        hostPolicyRiskLabel_ = new QLabel(this);
        hostPolicyRiskLabel_->setStyleSheet(
            "QLabel { color: #B00020; font-weight: 600; }");
        statusBar()->addPermanentWidget(hostPolicyRiskLabel_);
    }
    hostPolicyRiskLabel_->setText(activeSecurityWarning_);
    hostPolicyRiskLabel_->setToolTip(
        tr("This security exception applies only to the current session."));
    hostPolicyRiskLabel_->show();
}

QString MainWindow::defaultDownloadDirFromSettings(const QSettings &settings) {
    QString fallback =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (fallback.isEmpty())
        fallback = QDir::homePath() + "/Downloads";
    QString configured = QDir::cleanPath(
        settings.value("UI/defaultDownloadDir", fallback).toString().trimmed());
    if (configured.isEmpty())
        configured = fallback;
    return configured;
}

bool MainWindow::confirmHostKeyUI(const QString &host, quint16 port,
                                  const QString &algorithm,
                                  const QString &fingerprint, bool canSave) {
    tofuHost_ = host + ":" + QString::number(port);
    tofuAlg_ = algorithm;
    tofuFp_ = fingerprint;
    tofuCanSave_ = canSave;
    {
        std::unique_lock<std::mutex> tofuLock(tofuMutex_);
        tofuDecided_ = false;
        tofuAccepted_ = false;
    }
    QMetaObject::invokeMethod(
        this,
        [this, host, algorithm, fingerprint] {
            showTOfuDialog(host, algorithm, fingerprint);
        },
        Qt::QueuedConnection);
    std::unique_lock<std::mutex> tofuLock(tofuMutex_);
    tofuCv_.wait(tofuLock, [&] { return tofuDecided_; });
    return tofuAccepted_;
}

// Explicit non‑modal TOFU dialog per spec: open() + finished -> onTofuFinished
void MainWindow::showTOfuDialog(const QString &host, const QString &algorithm,
                                const QString &fingerprint) {
    if (tofuBox_) {
        tofuBox_->raise();
        tofuBox_->activateWindow();
        return;
    }
    // If a connection progress dialog is visible, disable it so it does not
    // capture input
    if (connectProgress_ && connectProgress_->isVisible()) {
        connectProgress_->setEnabled(false);
        connectProgressDimmed_ = true;
        if (openscp::sensitiveLoggingEnabled()) {
            std::fprintf(stderr,
                         "[OpenSCP] TOFU shown; progress paused=true\n");
        }
    } else {
        if (openscp::sensitiveLoggingEnabled()) {
            std::fprintf(stderr,
                         "[OpenSCP] TOFU shown; progress paused=false\n");
        }
    }
    auto *box = new QMessageBox(this);
    UiAlerts::configure(*box);
    tofuBox_ = box;
    box->setAttribute(Qt::WA_DeleteOnClose, true);
    box->setWindowModality(Qt::WindowModal);
    box->setIcon(QMessageBox::Question);
    box->setWindowTitle(tr("Confirm SSH fingerprint"));
    QString text = QString(tr("Connect to %1\nAlgorithm: %2\nFingerprint: "
                              "%3\n\nTrust and save to known_hosts?"))
                       .arg(host)
                       .arg(algorithm)
                       .arg(fingerprint);
    if (!tofuCanSave_) {
        text = QString(tr("Connect to %1\nAlgorithm: %2\nFingerprint: "
                          "%3\n\nFingerprint cannot be saved. Connection "
                          "allowed only this time."))
                   .arg(host)
                   .arg(algorithm)
                   .arg(fingerprint);
    }
    box->setText(text);
    box->addButton(tofuCanSave_ ? tr("Trust") : tr("Connect without saving"),
                   QMessageBox::YesRole);
    box->addButton(tr("Cancel"), QMessageBox::RejectRole);
    connect(box, &QMessageBox::finished, this, &MainWindow::onTofuFinished);
    QTimer::singleShot(0, box, [this, box] {
        box->open();
        box->raise();
        box->activateWindow();
        box->setFocus(Qt::ActiveWindowFocusReason);
    });
}

bool MainWindow::consumeTofuDialogDecision(int result) {
    bool accept = (result == QDialog::Accepted || result == QMessageBox::Yes);
    if (!tofuBox_)
        return accept;
    const auto *clicked = tofuBox_->clickedButton();
    if (clicked) {
        const auto role =
            tofuBox_->buttonRole(const_cast<QAbstractButton *>(clicked));
        accept = (role == QMessageBox::YesRole ||
                  role == QMessageBox::AcceptRole);
    }
    tofuBox_->deleteLater();
    tofuBox_.clear();
    return accept;
}

void MainWindow::publishTofuDecision(bool accept) {
    {
        std::unique_lock<std::mutex> tofuLock(tofuMutex_);
        tofuAccepted_ = accept;
        tofuDecided_ = true;
    }
    tofuCv_.notify_one();
}

void MainWindow::onTofuFinished(int dialogResult) {
    const bool accept = consumeTofuDialogDecision(dialogResult);
    if (!tofuCanSave_ && accept) {
        statusBar()->showMessage(
            tr("Could not save fingerprint; allowing one-time connection"),
            5000);
    } else if (!accept) {
        statusBar()->showMessage(
            tr("Connection cancelled: fingerprint not accepted"), 5000);
    }
    const bool resumedProgress = (connectProgressDimmed_ && connectProgress_);
    if (resumedProgress) {
        connectProgress_->setEnabled(true);
        connectProgressDimmed_ = false;
    }
    if (openscp::sensitiveLoggingEnabled())
        std::fprintf(stderr, "[OpenSCP] TOFU closed; progress resumed=%s\n",
                     resumedProgress ? "true" : "false");
    publishTofuDecision(accept);
}

// Secondary non‑modal dialog for one‑time connection without saving
void MainWindow::showOneTimeDialog(const QString &host,
                                   const QString &algorithm,
                                   const QString &fingerprint) {
    if (tofuBox_) {
        tofuBox_->raise();
        tofuBox_->activateWindow();
        return;
    }
    auto *box = new QMessageBox(this);
    UiAlerts::configure(*box);
    tofuBox_ = box;
    box->setAttribute(Qt::WA_DeleteOnClose, true);
    box->setWindowModality(Qt::WindowModal);
    box->setIcon(QMessageBox::Warning);
    box->setWindowTitle(tr("Additional confirmation"));
    box->setText(
        QString(
            tr("Could not save the fingerprint. Connect only this time without "
               "saving?\n\nHost: %1\nAlgorithm: %2\nFingerprint: %3"))
            .arg(host, algorithm, fingerprint));
    box->addButton(tr("Connect without saving"), QMessageBox::YesRole);
    box->addButton(tr("Cancel"), QMessageBox::RejectRole);
    connect(box, &QMessageBox::finished, this, &MainWindow::onOneTimeFinished);
    QTimer::singleShot(0, box, [box] { box->open(); });
}

void MainWindow::onOneTimeFinished(int dialogResult) {
    const bool accept = consumeTofuDialogDecision(dialogResult);
    if (accept)
        statusBar()->showMessage(
            tr("One-time connection without saving confirmed by user"), 5000);
    else
        statusBar()->showMessage(tr("Connection cancelled after save failure"),
                                 5000);
    publishTofuDecision(accept);
}
bool MainWindow::validateSftpConnectStart(
    const openscp::SessionOptions &opt) {
    if (transferCleanupInProgress_) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const int elapsedSec =
            (transferCleanupStartedAtMs_ > 0)
                ? int((nowMs - transferCleanupStartedAtMs_) / 1000)
                : 0;
        statusBar()->showMessage(
            tr("Please wait: previous transfer cleanup is still running (%1s)")
                .arg(elapsedSec),
            4000);
        return false;
    }
    if (sessionController_->isConnecting()) {
        statusBar()->showMessage(tr("A connection is already in progress"),
                                 3000);
        return false;
    }
    if (rightIsRemote_ || sessionController_->client()) {
        statusBar()->showMessage(tr("An active remote session already exists"),
                                 3000);
        return false;
    }
    const QString protocol = protocolDisplayLabel(opt.protocol);
    const openscp::ProtocolCapabilities caps =
        openscp::capabilitiesForProtocol(opt.protocol);
    auto cancelWithWarning = [this](const QString &message,
                                    const QString &statusMessage) {
        UiAlerts::warning(this, tr("Unsupported transport"), message);
        statusBar()->showMessage(statusMessage, 5000);
        return false;
    };
    if (!caps.implemented || (!caps.can_upload && !caps.can_download)) {
        UiAlerts::information(
            this, tr("Protocol not available"),
            tr("%1 support is not implemented yet.").arg(protocol));
        statusBar()->showMessage(
            tr("Connection canceled: unsupported protocol %1").arg(protocol),
            5000);
        return false;
    }
    if (hasConfiguredJumpHost(opt) && !caps.supports_jump_host) {
        return cancelWithWarning(
            tr("SSH jump host is not available for %1.").arg(protocol),
            tr("Connection canceled: SSH jump host is not supported for %1")
                .arg(protocol));
    }
    if (opt.proxy_type != openscp::ProxyType::None && !caps.supports_proxy) {
        return cancelWithWarning(
            tr("Proxy settings are not available for %1.").arg(protocol),
            tr("Connection canceled: proxy is not supported for %1")
                .arg(protocol));
    }
    if (hasTransportSelectionConflict(opt)) {
        UiAlerts::warning(
            this, tr("Invalid transport configuration"),
            tr("Proxy and SSH jump host cannot be used together in the same "
               "connection.\nEdit the site and keep only one transport."));
        statusBar()->showMessage(
            tr("Connection canceled: invalid transport configuration"), 5000);
        return false;
    }
#ifdef Q_OS_WIN
    if (hasConfiguredJumpHost(opt)) {
        UiAlerts::warning(
            this, tr("Unsupported transport"),
            tr("SSH jump host is currently unavailable on Windows."));
        statusBar()->showMessage(
            tr("Connection canceled: SSH jump host is unsupported on Windows"),
            5000);
        return false;
    }
#endif
    if (!confirmInsecureHostPolicyForSession(opt)) {
        statusBar()->showMessage(
            tr("Connection canceled: security exception not confirmed"),
            5000);
        return false;
    }
    return true;
}

void MainWindow::initializeSftpConnectUiState(
    const std::shared_ptr<std::atomic<bool>> &cancelFlag) {
    if (!sessionController_->beginConnection(cancelFlag))
        return;

    if (actConnect_)
        actConnect_->setEnabled(false);
    if (actSites_)
        actSites_->setEnabled(false);

    auto *progress =
        new QProgressDialog(tr("Connecting…"), tr("Cancel"), 0, 0, this);
    progress->setWindowModality(Qt::NonModal);
    progress->setMinimumDuration(0);
    progress->setAutoClose(false);
    progress->setAutoReset(false);
    connect(progress, &QProgressDialog::canceled, this, [this] {
        if (sessionController_->requestConnectionCancellation()) {
            statusBar()->showMessage(tr("Canceling connection…"), 3000);
        }
    });
    progress->show();
    progress->raise();
    connectProgress_ = progress;
    connectProgressDimmed_ = false;
}

void MainWindow::configureSftpConnectCallbacks(openscp::SessionOptions &opt) {
    QPointer<MainWindow> self(this);
    // Inject host key confirmation (TOFU) via UI
    opt.hostkey_confirm_cb = [self](const std::string &host,
                                    std::uint16_t port,
                                    const std::string &algorithm,
                                    const std::string &fingerprint,
                                    bool canSave) {
        if (!self)
            return false;
        return self->confirmHostKeyUI(
            QString::fromStdString(host), static_cast<quint16>(port),
            QString::fromStdString(algorithm),
            QString::fromStdString(fingerprint), canSave);
    };
    opt.hostkey_status_cb = [self](const std::string &msg) {
        if (!self)
            return;
        const QString statusMessage = QString::fromStdString(msg);
        QMetaObject::invokeMethod(
            self,
            [self, statusMessage] {
                if (self)
                    self->statusBar()->showMessage(statusMessage, 5000);
            },
            Qt::QueuedConnection);
    };

    // Keyboard-interactive callback (OTP/2FA). Prefer auto-filling
    // password/username; request OTP if needed.
    const std::string savedUser = opt.username;
    const std::string savedPass = opt.password ? *opt.password : std::string();
    opt.keyboard_interactive_cb =
        [self, savedUser, savedPass](const std::string &name,
                                     const std::string &instruction,
                                     const std::vector<std::string> &prompts,
                                     std::vector<std::string> &responses)
        -> openscp::KbdIntPromptResult {
        (void)name;
        if (!self)
            return openscp::KbdIntPromptResult::Cancelled;
        responses.clear();
        responses.reserve(prompts.size());
        auto promptForInput = [self](const QString &title,
                                     const QString &promptText,
                                     QLineEdit::EchoMode echoMode,
                                     QString &answer) {
            bool accepted = false;
            QMetaObject::invokeMethod(
                self,
                [&] {
                    if (!self)
                        return;
                    answer = QInputDialog::getText(
                        self, title, promptText, echoMode, QString(), &accepted);
                },
                Qt::BlockingQueuedConnection);
            return accepted;
        };
        auto appendUtf8Response = [&responses](QString &answer) {
            QByteArray bytes = answer.toUtf8();
            responses.emplace_back(bytes.constData(), (size_t)bytes.size());
            secureClear(bytes);
            secureClear(answer);
        };
        const QString instructionSuffix =
            instruction.empty()
                ? QString()
                : QStringLiteral(" — ") + QString::fromStdString(instruction);

        // Resolve each prompt: auto-fill user/pass and ask for OTP/codes if
        // present
        for (const std::string &promptTextUtf8 : prompts) {
            QString promptText = QString::fromStdString(promptTextUtf8);
            QString promptTextLower = promptText.toLower();
            // Username
            if (promptTextLower.contains("user") ||
                promptTextLower.contains("name:")) {
                responses.emplace_back(savedUser);
                continue;
            }
            // Password
            if (promptTextLower.contains("password") ||
                promptTextLower.contains("passphrase") ||
                promptTextLower.contains("passcode")) {
                if (!savedPass.empty()) {
                    responses.emplace_back(savedPass);
                    continue;
                }
                QString answer;
                if (!promptForInput(tr("Password required"), promptText,
                                    QLineEdit::Password, answer)) {
                    return openscp::KbdIntPromptResult::Cancelled;
                }
                appendUtf8Response(answer);
                continue;
            }
            // OTP / Verification code / Token
            QString answer;
            if (promptTextLower.contains("verification") ||
                promptTextLower.contains("verify") ||
                promptTextLower.contains("otp") ||
                promptTextLower.contains("code") ||
                promptTextLower.contains("token")) {
                const QString title =
                    tr("Verification code required") + instructionSuffix;
                if (!promptForInput(title, promptText, QLineEdit::Password,
                                    answer)) {
                    return openscp::KbdIntPromptResult::Cancelled;
                }
                appendUtf8Response(answer);
                continue;
            }
            // Generic case: ask for text (not hidden)
            const QString title = tr("Information required") + instructionSuffix;
            if (!promptForInput(title, promptText, QLineEdit::Normal, answer))
                return openscp::KbdIntPromptResult::Cancelled;
            appendUtf8Response(answer);
        }
        return (responses.size() == prompts.size())
                   ? openscp::KbdIntPromptResult::Handled
                   : openscp::KbdIntPromptResult::Unhandled;
    };
}

void MainWindow::launchSftpConnectWorker(
    openscp::SessionOptions opt, const openscp::SessionOptions &uiOpt,
    std::optional<PendingSiteSaveRequest> saveRequest,
    const std::shared_ptr<std::atomic<bool>> &cancelFlag) {
    QPointer<MainWindow> self(this);
    QThreadPool::globalInstance()->start(
        [self, opt = std::move(opt), uiOpt, saveRequest,
         cancelFlag]() mutable {
        bool connectionSucceeded = false;
        bool canceledByUser = false;
        std::string connectionError;
        std::unique_ptr<openscp::RemoteClient> connectedOwner;
        std::unique_ptr<openscp::RemoteClient> remoteControlOwner;
        try {
            if (cancelFlag && cancelFlag->load()) {
                canceledByUser = true;
                connectionError = "Connection canceled by user";
            } else {
                connectedOwner =
                    openscp::CreateConnectedClient(opt, connectionError);
                connectionSucceeded = static_cast<bool>(connectedOwner);
                if (cancelFlag && cancelFlag->load()) {
                    canceledByUser = true;
                    if (connectionSucceeded)
                        connectedOwner->disconnect();
                    connectionSucceeded = false;
                    if (connectionError.empty())
                        connectionError = "Connection canceled by user";
                }
                if (connectionSucceeded) {
                    if (openscp::capabilitiesForProtocol(uiOpt.protocol)
                            .can_list) {
                        std::string controlError;
                        remoteControlOwner = connectedOwner->newConnectionLike(
                            opt, controlError);
                        if (!remoteControlOwner) {
                            connectionSucceeded = false;
                            connectionError =
                                controlError.empty()
                                    ? "Could not create the remote control "
                                      "connection"
                                    : controlError;
                            connectedOwner->disconnect();
                            connectedOwner.reset();
                        }
                    }
                }
            }
        } catch (const std::exception &ex) {
            connectionError = std::string("Connection exception: ") + ex.what();
            connectionSucceeded = false;
        } catch (...) {
            connectionError = "Unknown connection exception";
            connectionSucceeded = false;
        }

        if (!connectionSucceeded) {
            if (remoteControlOwner)
                remoteControlOwner->disconnect();
            remoteControlOwner.reset();
            if (connectedOwner)
                connectedOwner->disconnect();
            connectedOwner.reset();
        }
        const QString connectionErrorText =
            QString::fromStdString(connectionError);
        openscp::RemoteClient *connectedClient = connectedOwner.get();
        openscp::RemoteClient *remoteControlClient = remoteControlOwner.get();
        const bool queued = QMetaObject::invokeMethod(
            qApp,
            [self, connectionSucceeded, connectionErrorText, connectedClient,
             remoteControlClient, uiOpt, saveRequest,
             canceledByUser]() {
                if (!self) {
                    if (connectedClient) {
                        connectedClient->disconnect();
                        delete connectedClient;
                    }
                    if (remoteControlClient) {
                        remoteControlClient->disconnect();
                        delete remoteControlClient;
                    }
                    return;
                }
                self->finalizeSftpConnect(connectionSucceeded, connectionErrorText,
                                          connectedClient, remoteControlClient,
                                          uiOpt, saveRequest, canceledByUser);
            },
            Qt::QueuedConnection);
        if (queued) {
            (void)connectedOwner.release();
            (void)remoteControlOwner.release();
        }
        });
}

void MainWindow::startSavedSiteConnect(const SiteEntry &site) {
    SavedSiteContext context;
    context.siteId = site.siteId;
    context.initialLocalPath = site.initialLocalPath;
    context.initialRemotePath = site.initialRemotePath.trimmed().isEmpty()
                                    ? QStringLiteral("/")
                                    : site.initialRemotePath;
    context.rememberLastPaths = site.rememberLastPaths;
    pendingSavedSiteContext_ = std::move(context);
    if (!startSftpConnect(site.opt))
        pendingSavedSiteContext_.reset();
}

bool MainWindow::startSftpConnect(
    openscp::SessionOptions opt,
    std::optional<PendingSiteSaveRequest> saveRequest) {
    if (!validateSftpConnectStart(opt))
        return false;

    const openscp::SessionOptions uiOpt = opt;
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    initializeSftpConnectUiState(cancelFlag);

    if (openscp::capabilitiesForProtocol(opt.protocol).supports_known_hosts)
        configureSftpConnectCallbacks(opt);
    launchSftpConnectWorker(std::move(opt), uiOpt, std::move(saveRequest),
                            cancelFlag);
    return true;
}

void MainWindow::finalizeSftpConnect(
    bool connectionOk, const QString &errorText,
    openscp::RemoteClient *connectedClient,
    openscp::RemoteClient *remoteControlClient,
    const openscp::SessionOptions &uiOpt,
    std::optional<PendingSiteSaveRequest> saveRequest, bool canceledByUser) {
    std::unique_ptr<openscp::RemoteClient> guard(connectedClient);
    std::unique_ptr<openscp::RemoteClient> controlGuard(remoteControlClient);
    if (connectProgress_) {
        connectProgress_->close();
        connectProgress_.clear();
    }
    connectProgressDimmed_ = false;
    sessionController_->finishConnection();
    if (actSites_)
        actSites_->setEnabled(true);

    if (!connectionOk) {
        pendingSavedSiteContext_.reset();
        if (actConnect_ && !rightIsRemote_)
            actConnect_->setEnabled(true);
        if (canceledByUser)
            statusBar()->showMessage(tr("Connection canceled"), 4000);
        else {
            UiAlerts::critical(
                this, tr("Connection error"),
                tr("Could not connect to the server.\n%1")
                    .arg(shortRemoteError(
                        errorText, tr("Check host, port, and credentials."))));
        }
        return;
    }

    sessionNoHostVerification_ =
        openscp::capabilitiesForProtocol(uiOpt.protocol).supports_known_hosts &&
        (uiOpt.known_hosts_policy == openscp::KnownHostsPolicy::Off);
    if (uiOpt.protocol == openscp::Protocol::Ftp) {
        activeSecurityWarning_ =
            tr("Insecure: FTP traffic is not encrypted");
    } else if (uiOpt.protocol == openscp::Protocol::WebDav &&
               uiOpt.webdav_scheme == openscp::WebDavScheme::Http) {
        activeSecurityWarning_ =
            tr("Insecure: WebDAV HTTP traffic is not encrypted");
    } else if ((uiOpt.protocol == openscp::Protocol::Ftps &&
                !uiOpt.ftps_verify_peer) ||
               (uiOpt.protocol == openscp::Protocol::WebDav &&
                uiOpt.webdav_scheme == openscp::WebDavScheme::Https &&
                !uiOpt.webdav_verify_peer)) {
        activeSecurityWarning_ =
            tr("Risk: TLS certificate is not verified");
    } else if (sessionNoHostVerification_) {
        activeSecurityWarning_ =
            tr("Risk: SSH host key is not verified");
    } else {
        activeSecurityWarning_.clear();
    }
    sessionController_->installClient(std::move(guard));
    if (remoteOps_) {
        if (controlGuard)
            remoteOps_->installSession(std::move(controlGuard));
        else
            remoteOps_->clearSession();
    }
    if (pendingSavedSiteContext_) {
        activeSavedSiteContext_ = std::move(pendingSavedSiteContext_);
        pendingSavedSiteContext_.reset();
    } else {
        activeSavedSiteContext_.reset();
    }
    applyRemoteConnectedUI(uiOpt);
    if (rightIsRemote_ && activeSavedSiteContext_) {
        const QString initialLocal =
            activeSavedSiteContext_->initialLocalPath.trimmed();
        if (!initialLocal.isEmpty() && QDir(initialLocal).exists())
            setLeftRoot(initialLocal);
    } else {
        activeSavedSiteContext_.reset();
    }
    if (rightIsRemote_ && saveRequest.has_value()) {
        maybePersistQuickConnectSite(uiOpt, *saveRequest, true);
    }
}

void MainWindow::maybePersistQuickConnectSite(
    const openscp::SessionOptions &opt, const PendingSiteSaveRequest &req,
    bool connectionEstablished) {
    QuickSitesLoadResult loadedSites = loadSavedSitesForQuickConnect();
    QVector<SiteEntry> sites = std::move(loadedSites.sites);

    int matchIndex = -1;
    for (int siteIndex = 0; siteIndex < sites.size(); ++siteIndex) {
        if (sameSavedSiteIdentity(sites[siteIndex].opt, opt)) {
            matchIndex = siteIndex;
            break;
        }
    }

    bool created = false;
    if (matchIndex < 0) {
        SiteEntry newEntry;
        newEntry.siteId = newQuickSiteId();
        newEntry.name = ensureUniqueQuickSiteName(
            sites, req.siteName.trimmed().isEmpty() ? defaultQuickSiteName(opt)
                                                    : req.siteName.trimmed());
        newEntry.initialLocalPath = req.initialLocalPath;
        newEntry.initialRemotePath = req.initialRemotePath;
        newEntry.rememberLastPaths = req.rememberLastPaths;
        newEntry.opt = opt;
        newEntry.opt.password.reset();
        newEntry.opt.private_key_passphrase.reset();
        newEntry.opt.proxy_password.reset();
        sites.push_back(newEntry);
        matchIndex = sites.size() - 1;
        created = true;
    }

    if ((created || loadedSites.needsSave) &&
        !loadedSites.legacyMigration.complete) {
        UiAlerts::warning(
            this, tr("Site not saved"),
            tr("OpenSCP connected, but it did not rewrite saved sites because "
               "one or more legacy credentials could not be moved to the "
               "secure backend:\n%1")
                .arg(loadedSites.legacyMigration.issues.join(
                    QLatin1Char('\n'))));
        statusBar()->showMessage(
            connectionEstablished ? tr("Connected. Site was not saved.")
                                  : tr("Site was not saved."),
            6000);
        return;
    }

    if (created || loadedSites.needsSave) {
        saveSavedSitesForQuickConnect(sites);
        refreshOpenSiteManagerWidget(siteManager_);
    }

    if (!req.saveCredentials) {
        const QString statusMessage =
            connectionEstablished
                ? (created ? tr("Connected. Site saved.")
                           : tr("Connected. Site already exists."))
                : (created ? tr("Site saved.") : tr("Site already exists."));
        statusBar()->showMessage(statusMessage, 5000);
        return;
    }

    const SiteEntry &target = sites[matchIndex];
    SiteCredentialRepository credentials;
    const SiteCredentialOperationResult saveResult =
        credentials.save(target, opt);
    const QStringList issues = saveResult.issueMessages();
    const bool anyCredentialStored = saveResult.anyCredentialHandled;

    if (!issues.isEmpty()) {
        UiAlerts::warning(this, tr("Saved sites"),
                             tr("The site was saved, but some credentials "
                                "could not be saved:\n%1")
                                 .arg(issues.join("\n")));
    }

    const QString statusMessage =
        connectionEstablished
            ? (created
                   ? (anyCredentialStored
                          ? tr("Connected. Site and credentials saved.")
                          : tr("Connected. Site saved."))
                   : (anyCredentialStored
                          ? tr("Connected. Credentials updated.")
                          : tr("Connected. Site already exists.")))
            : (created ? (anyCredentialStored ? tr("Site and credentials saved.")
                                              : tr("Site saved."))
                       : (anyCredentialStored ? tr("Credentials updated.")
                                              : tr("Site already exists.")));
    statusBar()->showMessage(statusMessage, 5000);
}

// Switch UI into remote mode and wire models/actions for the right pane.
void MainWindow::applyRemoteConnectedUI(const openscp::SessionOptions &opt) {
    saveRightHeaderState(rightIsRemote_);
    if (rightRemoteModel_) {
        rightView_->setModel(rightLocalModel_);
        delete rightRemoteModel_;
        rightRemoteModel_ = nullptr;
    }

    const openscp::ProtocolCapabilities caps =
        openscp::capabilitiesForProtocol(opt.protocol);
    const bool transferOnlyMode = !caps.can_list;
    // Establish navigation identity before the initial listing emits history
    // updates or builds scoped favorite menus.
    sessionController_->setOptions(opt);

    if (!transferOnlyMode) {
        rightRemoteModel_ = new RemoteModel(this);
        rightRemoteModel_->setShowHidden(prefShowHidden_);
        connect(rightRemoteModel_, &RemoteModel::rootPathLoaded, this,
                [this](const QString &path, bool loadOk,
                       const QString &error) {
                    Q_UNUSED(error);
                    if (!rightRemoteModel_)
                        return;
                    if (!loadOk)
                        return;
                    rightPath_->setText(path);
                    addRecentRemotePath(path);
                    refreshRightBreadcrumbs();
                    if (rightIsRemote_) {
                        updateRemoteMutationCapability();
                        updateDeleteShortcutEnables();
                    }
                });
        activateScpTransferModeUi(false);
        rightView_->setModel(rightRemoteModel_);
        if (rightView_->selectionModel()) {
            connect(rightView_->selectionModel(),
                    &QItemSelectionModel::selectionChanged, this,
                    [this] { updateDeleteShortcutEnables(); });
        }
        rightView_->header()->setStretchLastSection(false);
        if (!restoreRightHeaderState(true)) {
            rightView_->setColumnWidth(0, 300);
            rightView_->setColumnWidth(1, 120);
            rightView_->setColumnWidth(2, 180);
            rightView_->setColumnWidth(3, 120);
        }
        rightView_->setSortingEnabled(true);
        rightView_->sortByColumn(0, Qt::AscendingOrder);
        const QString initialRemotePath =
            activeSavedSiteContext_
                ? activeSavedSiteContext_->initialRemotePath
                : QStringLiteral("/");
        rightPath_->setText(initialRemotePath.trimmed().isEmpty()
                                ? QStringLiteral("/")
                                : initialRemotePath);
        rightIsRemote_ = true;
        sessionController_->setOptions(opt);
        transferUiController_.reset();
        if (transferMgr_) {
            transferMgr_->setSessionIdentity(remoteNavigationScope());
            transferMgr_->setClient(sessionController_->client());
            transferMgr_->setSessionOptions(opt);
        }
        requestRemoteListing(rightPath_->text(), false, true);
        if (actConnect_)
            actConnect_->setEnabled(false);
        if (actDisconnect_)
            actDisconnect_->setEnabled(true);
        if (actDownloadF7_)
            actDownloadF7_->setEnabled(true);
        if (actUploadRight_)
            actUploadRight_->setEnabled(true);
        if (actRefreshRight_)
            actRefreshRight_->setEnabled(true);
        if (actOpenTerminalRight_)
            actOpenTerminalRight_->setEnabled(true);
        if (actSearchRight_)
            actSearchRight_->setEnabled(true);
        if (actSync_)
            actSync_->setEnabled(caps.can_list && caps.can_upload &&
                                 caps.can_download);
        if (actNewDirRight_)
            actNewDirRight_->setEnabled(true);
        if (actNewFileRight_)
            actNewFileRight_->setEnabled(true);
        if (actRenameRight_)
            actRenameRight_->setEnabled(true);
        if (actDeleteRight_)
            actDeleteRight_->setEnabled(true);
        if (actChooseRight_) {
            actChooseRight_->setIcon(QIcon(
                QLatin1String(":/assets/icons/action-open-folder-remote.svg")));
            // Opening the system file explorer on a remote host is not
            // supported cross‑platform. Disable this action in remote mode to
            // avoid confusion.
            actChooseRight_->setEnabled(false);
            actChooseRight_->setToolTip(tr("Not available in remote mode"));
        }
        const QString activeProtocol = protocolDisplayLabel(opt.protocol);
        startConnectionSessionIndicators(activeProtocol);
        statusBar()->showMessage(
            tr("Connected (%1) to %2")
                .arg(activeProtocol, QString::fromStdString(opt.host)),
            4000);
        addRecentServer(opt);
        setWindowTitle(tr("OpenSCP — local/remote (%1)").arg(activeProtocol));
        updateHostPolicyRiskBanner();
        startRemoteSessionHealthMonitoring();
        updateRemoteMutationCapability();
        updateDeleteShortcutEnables();
        return;
    }

    rightView_->setModel(rightLocalModel_);
    rightPath_->setText(QStringLiteral("/"));
    rightIsRemote_ = true;
    sessionController_->setOptions(opt);
    addRecentRemotePath(QStringLiteral("/"));
    activateScpTransferModeUi(true);
    transferUiController_.reset();
    refreshRightBreadcrumbs();
    rightRemoteMutationsSupported_ = false;
    if (transferMgr_) {
        transferMgr_->setSessionIdentity(remoteNavigationScope());
        transferMgr_->setClient(sessionController_->client());
        transferMgr_->setSessionOptions(opt);
    }
    if (actConnect_)
        actConnect_->setEnabled(false);
    if (actDisconnect_)
        actDisconnect_->setEnabled(true);
    if (actDownloadF7_)
        actDownloadF7_->setEnabled(true);
    if (actUploadRight_)
        actUploadRight_->setEnabled(true);
    if (actRefreshRight_)
        actRefreshRight_->setEnabled(false);
    if (actOpenTerminalRight_)
        actOpenTerminalRight_->setEnabled(true);
    if (actSearchRight_)
        actSearchRight_->setEnabled(false);
    if (actSync_)
        actSync_->setEnabled(false);
    if (actNewDirRight_)
        actNewDirRight_->setEnabled(false);
    if (actNewFileRight_)
        actNewFileRight_->setEnabled(false);
    if (actRenameRight_)
        actRenameRight_->setEnabled(false);
    if (actDeleteRight_)
        actDeleteRight_->setEnabled(false);
    if (actMoveRight_)
        actMoveRight_->setEnabled(false);
    if (actMoveRightTb_)
        actMoveRightTb_->setEnabled(false);
    if (actCopyRightTb_)
        actCopyRightTb_->setEnabled(false);
    if (actChooseRight_) {
        actChooseRight_->setIcon(
            QIcon(QLatin1String(":/assets/icons/action-open-folder-remote.svg")));
        actChooseRight_->setEnabled(false);
        actChooseRight_->setToolTip(tr("Not available in remote mode"));
    }
    const QString activeProtocol = protocolDisplayLabel(opt.protocol);
    startConnectionSessionIndicators(activeProtocol);
    statusBar()->showMessage(
        tr("Connected (%1) to %2")
            .arg(activeProtocol, QString::fromStdString(opt.host)),
        4000);
    addRecentServer(opt);
    setWindowTitle(tr("OpenSCP — local/remote (%1)").arg(activeProtocol));
    updateHostPolicyRiskBanner();
    startRemoteSessionHealthMonitoring();
    updateDeleteShortcutEnables();
}
