// MainWindow connection/session/security and saved-site persistence logic.
#include "MainWindow.hpp"
#include "ConnectionDialog.hpp"
#include "MainWindowSharedUtils.hpp"
#include "RemoteModel.hpp"
#include "SavedSitesPersistence.hpp"
#include "SecretStore.hpp"
#include "SiteManagerDialog.hpp"
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
#include <QSettings>
#include <QStringList>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTimer>
#include <QtGlobal>
#include <QUuid>

#include <atomic>
#include <cstdio>
#include <memory>
#include <thread>

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
    const bool compareWebDavTls =
        (a.protocol != openscp::Protocol::WebDav) ||
        (a.webdav_scheme == b.webdav_scheme &&
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
           a.ftps_verify_peer == b.ftps_verify_peer &&
           normalizedIdentityKeyPath(a.ftps_ca_cert_path) ==
               normalizedIdentityKeyPath(b.ftps_ca_cert_path) &&
           compareWebDavTls;
}

static QString quickSiteSecretKey(const SiteEntry &entry, const QString &item) {
    if (!entry.siteId.isEmpty())
        return QString("site-id:%1:%2").arg(entry.siteId, item);
    return QString("site:%1:%2").arg(entry.name, item);
}

static QVector<SiteEntry> loadSavedSitesForQuickConnect(bool *needsSave) {
    const SavedSitesPersistence::LoadResult loaded =
        SavedSitesPersistence::loadSites({
            .trimSiteNames = true,
            .createNewId = [] { return newQuickSiteId(); },
        });
    if (needsSave)
        *needsSave = loaded.needsSave;
    return loaded.sites;
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

static QString quickPersistStatusShort(SecretStore::PersistStatus status) {
    switch (status) {
    case SecretStore::PersistStatus::Stored:
        return QObject::tr("stored");
    case SecretStore::PersistStatus::Unavailable:
        return QObject::tr("unavailable");
    case SecretStore::PersistStatus::PermissionDenied:
        return QObject::tr("permission denied");
    case SecretStore::PersistStatus::BackendError:
        return QObject::tr("backend error");
    }
    return QObject::tr("error");
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

bool MainWindow::reconnectActiveRemoteSession(QString *errorOut) {
    if (errorOut)
        errorOut->clear();
    if (!rightIsRemote_ || !sftp_ || !activeSessionOptions_.has_value()) {
        if (errorOut)
            *errorOut = tr("No active remote session to reconnect.");
        return false;
    }
    if (isDisconnecting_ || connectInProgress_) {
        if (errorOut)
            *errorOut = tr("Connection state is changing; reconnect skipped.");
        return false;
    }
    if (remoteSessionReconnectInFlight_.exchange(true)) {
        if (errorOut)
            *errorOut = tr("Reconnect already in progress.");
        return false;
    }

    QString restorePath = QStringLiteral("/");
    if (isScpTransferMode()) {
        restorePath = normalizeRemotePanelPath(
            rightPath_ ? rightPath_->text() : QString());
    } else if (rightRemoteModel_) {
        restorePath = rightRemoteModel_->rootPath();
    }
    statusBar()->showMessage(tr("Remote session became stale. Reconnecting…"),
                             0);

    std::string connErr;
    auto replacement = sftp_->newConnectionLike(*activeSessionOptions_, connErr);
    if (!replacement) {
        remoteSessionReconnectInFlight_.store(false);
        if (errorOut)
            *errorOut = QString::fromStdString(connErr);
        return false;
    }

    sftp_->disconnect();
    sftp_ = std::move(replacement);
    applyRemoteConnectedUI(*activeSessionOptions_);
    if (!sftp_ || (!isScpTransferMode() && !rightRemoteModel_)) {
        remoteSessionReconnectInFlight_.store(false);
        if (errorOut) {
            *errorOut = tr("Reconnected transport, but remote panel restore "
                           "failed.");
        }
        return false;
    }

    if (!restorePath.isEmpty() && restorePath != QStringLiteral("/"))
        setRightRemoteRoot(restorePath);

    if (transferMgr_) {
        transferMgr_->setClient(sftp_.get());
        transferMgr_->setSessionOptions(*activeSessionOptions_);
    }

    startRemoteSessionHealthMonitoring();
    statusBar()->showMessage(tr("Remote session reconnected"), 4000);
    remoteSessionReconnectInFlight_.store(false);
    return true;
}

bool MainWindow::maybeRecoverRemoteSession(const QString &operationLabel,
                                           const QString &rawError) {
    if (!isLikelyRemoteTransportError(rawError))
        return false;

    QString reconnectErr;
    const bool recovered = reconnectActiveRemoteSession(&reconnectErr);
    if (recovered) {
        statusBar()->showMessage(
            tr("Recovered remote session while trying to %1")
                .arg(operationLabel),
            4000);
        return true;
    }
    return false;
}

bool MainWindow::executeCriticalRemoteOperation(
    const QString &operationLabel,
    const std::function<bool(openscp::SftpClient *, std::string &)> &operation,
    std::string &err) {
    err.clear();
    if (!operation) {
        err = "Invalid operation callback";
        return false;
    }
    if (!rightIsRemote_ || !sftp_) {
        err = "No active remote session";
        return false;
    }

    if (operation(sftp_.get(), err))
        return true;

    const QString firstError = QString::fromStdString(err);
    if (!maybeRecoverRemoteSession(operationLabel, firstError)) {
        if (isLikelyRemoteTransportError(firstError) && rightIsRemote_ &&
            sftp_ && !isDisconnecting_) {
            UiAlerts::warning(
                this, tr("Connection lost"),
                tr("The remote session failed while trying to %1.\n"
                   "OpenSCP will disconnect to avoid inconsistent operations.\n%2")
                    .arg(operationLabel,
                         shortRemoteError(firstError, tr("Transport error."))));
            disconnectSftp();
        }
        return false;
    }

    err.clear();
    if (!sftp_) {
        err = "Remote session unavailable after recovery";
        return false;
    }
    return operation(sftp_.get(), err);
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
                        sftp_) {
                        runRemoteSessionHealthCheck(
                            tr("resume (%1s)").arg(inactiveMs / 1000), true);
                    }
                    return;
                }
                lastAppInactiveAtMs_ = nowMs;
            });
}

void MainWindow::startRemoteSessionHealthMonitoring() {
    if (!rightIsRemote_ || !sftp_)
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
    remoteSessionHealthProbeInFlight_.store(false);
    remoteSessionReconnectInFlight_.store(false);
    lastAppInactiveAtMs_ = 0;
}

void MainWindow::runRemoteSessionHealthCheck(const QString &reason, bool force) {
    if (!rightIsRemote_ || !sftp_ || !activeSessionOptions_.has_value())
        return;
    if (isDisconnecting_ || connectInProgress_)
        return;
    bool expected = false;
    if (!remoteSessionHealthProbeInFlight_.compare_exchange_strong(expected,
                                                                     true)) {
        return;
    }

    const QString probePath =
        (rightRemoteModel_ && !rightRemoteModel_->rootPath().isEmpty())
            ? rightRemoteModel_->rootPath()
            : QStringLiteral("/");
    std::string healthCheckError;
    bool pathIsDirectory = false;
    const bool probeSucceeded = executeCriticalRemoteOperation(
        tr("validate the remote session"),
        [probePath, &pathIsDirectory](openscp::SftpClient *client,
                                      std::string &opErr) {
            bool exists = false;
            exists =
                client->exists(probePath.toStdString(), pathIsDirectory, opErr);
            if (exists)
                return true;
            return opErr.empty();
        },
        healthCheckError);
    remoteSessionHealthProbeInFlight_.store(false);
    if (probeSucceeded) {
        if (force && !reason.isEmpty()) {
            statusBar()->showMessage(tr("Remote session validated (%1)")
                                         .arg(reason),
                                     2500);
        }
        return;
    }
    if (!rightIsRemote_ || !sftp_ || isDisconnecting_)
        return;

    const QString rawErr = QString::fromStdString(healthCheckError);
    if (!isLikelyRemoteTransportError(rawErr))
        return;

    UiAlerts::warning(
        this, tr("Connection lost"),
        tr("The remote session no longer responds (%1).\n"
           "OpenSCP will disconnect to avoid inconsistent operations.\n%2")
            .arg(reason, shortRemoteError(rawErr, tr("Transport error."))));
    disconnectSftp();
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
        req.saveCredentials = dlg.saveCredentialsRequested();
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
    isDisconnecting_ = true;
    stopRemoteSessionHealthMonitoring();
    const quint64 disconnectSeq = ++disconnectSeq_;
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
    pendingRemoteRefreshFromUpload_ = false;
    seenCompletedUploadTaskIds_.clear();
    seenCompletedTransferNoticeTaskIds_.clear();
    restoreRightHeaderState(false);
    if (QDir(rightPath_->text()).exists()) {
        setRightRoot(rightPath_->text());
    } else {
        setRightRoot(QDir::homePath());
    }
    rightRemoteWritable_ = false;
    remoteWriteabilityCache_.clear();
    ++remoteWriteabilityProbeSeq_;
    activeSessionOptions_.reset();
    sessionNoHostVerification_ = false;
    updateHostPolicyRiskBanner();
    setActionEnabled(actDownloadF7_, false);
    setActionEnabled(actUploadRight_, false);
    setActionEnabled(actRefreshRight_, false);
    setActionEnabled(actOpenTerminalRight_, false);
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
        if (!isDisconnecting_ || disconnectSeq != disconnectSeq_)
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
    std::thread([self, mgr, disconnectSeq]() {
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
                if (disconnectSeq == self->disconnectSeq_ &&
                    self->transferCleanupInProgress_) {
                    self->transferCleanupInProgress_ = false;
                    self->transferCleanupStartedAtMs_ = 0;
                    if (!self->isDisconnecting_ && !self->rightIsRemote_) {
                        if (self->actConnect_)
                            self->actConnect_->setToolTip(
                                self->actConnect_->text());
                        self->statusBar()->showMessage(
                            tr("Background transfer cleanup finished"), 3000);
                    }
                }
                self->completeDisconnectSftp(disconnectSeq, false);
            },
            Qt::QueuedConnection);
    }).detach();
    return true;
}

void MainWindow::disconnectSftp() {
    if (isDisconnecting_)
        return;
    const quint64 disconnectSeq = beginDisconnectFlow();
    scheduleDisconnectWatchdog(disconnectSeq);

    if (runDisconnectTransferCleanupAsync(disconnectSeq))
        return;

    transferCleanupInProgress_ = false;
    transferCleanupStartedAtMs_ = 0;
    completeDisconnectSftp(disconnectSeq, false);
}

void MainWindow::completeDisconnectSftp(quint64 disconnectSeq, bool forced) {
    if (!isDisconnecting_ || disconnectSeq != disconnectSeq_)
        return;
    if (sftp_)
        sftp_->disconnect();
    sftp_.reset();
    resetConnectionSessionIndicators();
    if (actConnect_) {
        actConnect_->setEnabled(true);
        actConnect_->setToolTip(actConnect_->text());
    }

    // Per spec: non‑modal Site Manager after disconnect (if enabled), without
    // blocking UI
    isDisconnecting_ = false;
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
            openscp::SessionOptions opt{};
            if (dlg->selectedOptions(opt)) {
                startSftpConnect(opt);
            }
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
    if (opt.known_hosts_policy != openscp::KnownHostsPolicy::Off)
        return true;

    const QString hostKey =
        QString::fromStdString(opt.host).trimmed().toLower();
    const QString allowKey =
        QString("Security/noHostVerificationConfirmedUntilUtc/%1:%2")
            .arg(hostKey)
            .arg((int)opt.port);
    const qint64 now = QDateTime::currentSecsSinceEpoch();
    QSettings settings("OpenSCP", "OpenSCP");
    const qint64 allowedUntil = settings.value(allowKey, 0).toLongLong();
    if (allowedUntil > now)
        return true;

    const auto first = UiAlerts::warning(
        this, tr("Critical security risk"),
        tr("You are about to connect using the \"No verification\" policy.\n"
           "This allows MITM attacks and server impersonation.\n\n"
           "Do you want to continue at your own risk?"),
        QMessageBox::Yes | QMessageBox::Cancel, QMessageBox::Cancel);
    if (first != QMessageBox::Yes)
        return false;

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

    // Temporary exception per host:port to avoid persistent bypasses.
    const int ttlMin = qBound(1, prefNoHostVerificationTtlMin_, 120);
    const qint64 newUntil = now + qint64(ttlMin) * 60;
    settings.setValue(allowKey, newUntil);
    settings.sync();
    const QDateTime expLocal =
        QDateTime::fromSecsSinceEpoch(newUntil).toLocalTime();
    statusBar()->showMessage(
        tr("Temporary \"no verification\" exception active until %1")
            .arg(QLocale().toString(expLocal, QLocale::ShortFormat)),
        8000);
    return true;
}

void MainWindow::updateHostPolicyRiskBanner() {
    const bool show = rightIsRemote_ && sessionNoHostVerification_;
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
    hostPolicyRiskLabel_->setText(
        tr("Risk: host key not verified in this session"));
    hostPolicyRiskLabel_->setToolTip(tr(
        "The current session does not validate host key; MITM risk exists."));
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
    if (connectInProgress_) {
        statusBar()->showMessage(tr("A connection is already in progress"),
                                 3000);
        return false;
    }
    if (rightIsRemote_ || sftp_) {
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
    if (!caps.implemented || !caps.supports_file_transfers) {
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
            tr("Connection canceled: no-verification policy not confirmed"),
            5000);
        return false;
    }
    return true;
}

void MainWindow::initializeSftpConnectUiState(
    const std::shared_ptr<std::atomic<bool>> &cancelFlag) {
    connectCancelRequested_ = cancelFlag;
    connectInProgress_ = true;

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
        if (connectCancelRequested_) {
            connectCancelRequested_->store(true);
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
    std::thread([self, opt = std::move(opt), uiOpt, saveRequest,
                 cancelFlag]() mutable {
        bool connectionSucceeded = false;
        bool canceledByUser = false;
        std::string connectionError;
        openscp::SftpClient *connectedClient = nullptr;
        try {
            if (cancelFlag && cancelFlag->load()) {
                canceledByUser = true;
                connectionError = "Connection canceled by user";
            } else {
                auto connectedCandidate =
                    openscp::CreateConnectedClient(opt, connectionError);
                connectionSucceeded = static_cast<bool>(connectedCandidate);
                if (cancelFlag && cancelFlag->load()) {
                    canceledByUser = true;
                    if (connectionSucceeded)
                        connectedCandidate->disconnect();
                    connectionSucceeded = false;
                    if (connectionError.empty())
                        connectionError = "Connection canceled by user";
                }
                if (connectionSucceeded)
                    connectedClient = connectedCandidate.release();
            }
        } catch (const std::exception &ex) {
            connectionError = std::string("Connection exception: ") + ex.what();
            connectionSucceeded = false;
        } catch (...) {
            connectionError = "Unknown connection exception";
            connectionSucceeded = false;
        }

        const QString connectionErrorText =
            QString::fromStdString(connectionError);
        const bool queued = QMetaObject::invokeMethod(
            qApp,
            [self, connectionSucceeded, connectionErrorText, connectedClient,
             uiOpt, saveRequest,
             canceledByUser]() {
                if (!self) {
                    if (connectedClient) {
                        connectedClient->disconnect();
                        delete connectedClient;
                    }
                    return;
                }
                self->finalizeSftpConnect(connectionSucceeded, connectionErrorText,
                                          connectedClient, uiOpt, saveRequest,
                                          canceledByUser);
            },
            Qt::QueuedConnection);
        if (!queued && connectedClient) {
            connectedClient->disconnect();
            delete connectedClient;
        }
    }).detach();
}

void MainWindow::startSftpConnect(
    openscp::SessionOptions opt,
    std::optional<PendingSiteSaveRequest> saveRequest) {
    if (!validateSftpConnectStart(opt))
        return;

    const openscp::SessionOptions uiOpt = opt;
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    initializeSftpConnectUiState(cancelFlag);

    configureSftpConnectCallbacks(opt);
    launchSftpConnectWorker(std::move(opt), uiOpt, std::move(saveRequest),
                            cancelFlag);
}

void MainWindow::finalizeSftpConnect(
    bool connectionOk, const QString &errorText,
    openscp::SftpClient *connectedClient,
    const openscp::SessionOptions &uiOpt,
    std::optional<PendingSiteSaveRequest> saveRequest, bool canceledByUser) {
    std::unique_ptr<openscp::SftpClient> guard(connectedClient);
    if (connectProgress_) {
        connectProgress_->close();
        connectProgress_.clear();
    }
    connectProgressDimmed_ = false;
    connectCancelRequested_.reset();
    connectInProgress_ = false;
    if (actSites_)
        actSites_->setEnabled(true);

    if (!connectionOk) {
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
        (uiOpt.known_hosts_policy == openscp::KnownHostsPolicy::Off);
    sftp_ = std::move(guard);
    applyRemoteConnectedUI(uiOpt);
    if (rightIsRemote_ && saveRequest.has_value()) {
        maybePersistQuickConnectSite(uiOpt, *saveRequest, true);
    }
}

void MainWindow::maybePersistQuickConnectSite(
    const openscp::SessionOptions &opt, const PendingSiteSaveRequest &req,
    bool connectionEstablished) {
    bool regeneratedIds = false;
    QVector<SiteEntry> sites = loadSavedSitesForQuickConnect(&regeneratedIds);

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
        newEntry.opt = opt;
        newEntry.opt.password.reset();
        newEntry.opt.private_key_passphrase.reset();
        newEntry.opt.proxy_password.reset();
        sites.push_back(newEntry);
        matchIndex = sites.size() - 1;
        created = true;
    }

    if (created || regeneratedIds) {
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

    SecretStore store;
    QStringList issues;
    bool anyCredentialStored = false;
    const SiteEntry &target = sites[matchIndex];

    auto storeCredential = [&](const QString &label, const QString &secretKey,
                               const std::optional<std::string> &value) {
        if (!value || value->empty())
            return;
        const auto saveResult = store.setSecret(
            quickSiteSecretKey(target, secretKey),
            QString::fromStdString(*value));
        if (saveResult.isStored())
            anyCredentialStored = true;
        else
            issues << tr("%1: %2")
                          .arg(label, quickPersistStatusShort(saveResult.status));
    };

    storeCredential(tr("Password"), QStringLiteral("password"), opt.password);
    storeCredential(tr("Passphrase"), QStringLiteral("keypass"),
                    opt.private_key_passphrase);
    if (opt.proxy_type != openscp::ProxyType::None) {
        storeCredential(tr("Proxy password"), QStringLiteral("proxypass"),
                        opt.proxy_password);
    } else {
        store.removeSecret(quickSiteSecretKey(target, QStringLiteral("proxypass")));
    }

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
    const bool transferOnlyMode = !caps.supports_listing;

    if (!transferOnlyMode) {
        rightRemoteModel_ = new RemoteModel(sftp_.get(), this);
        rightRemoteModel_->setSessionOptions(opt);
        rightRemoteModel_->setShowHidden(prefShowHidden_);
        connect(rightRemoteModel_, &RemoteModel::rootPathLoaded, this,
                [this](const QString &path, bool loadOk,
                       const QString &error) {
                    if (!rightRemoteModel_)
                        return;
                    if (!loadOk) {
                        UiAlerts::warning(
                            this, tr("Remote error"),
                            tr("Could not open the remote folder.\n%1")
                                .arg(shortRemoteError(
                                    error,
                                    tr("Failed to read remote contents."))));
                        return;
                    }
                    rightPath_->setText(path);
                    addRecentRemotePath(path);
                    refreshRightBreadcrumbs();
                    if (rightIsRemote_) {
                        updateRemoteWriteability();
                        updateDeleteShortcutEnables();
                    }
                });
        QString initialRootLoadError;
        if (!rightRemoteModel_->setRootPath("/", &initialRootLoadError, false)) {
            UiAlerts::critical(
                this, tr("Error listing remote"),
                tr("Could not open the initial remote folder.\n%1")
                    .arg(shortRemoteError(
                        initialRootLoadError,
                        tr("Failed to read remote contents."))));
            sftp_.reset();
            rightView_->setModel(rightLocalModel_);
            activateScpTransferModeUi(false);
            remoteWriteabilityCache_.clear();
            activeSessionOptions_.reset();
            sessionNoHostVerification_ = false;
            rightIsRemote_ = false;
            stopRemoteSessionHealthMonitoring();
            if (transferMgr_)
                transferMgr_->setClient(nullptr);
            updateHostPolicyRiskBanner();
            resetConnectionSessionIndicators();
            delete rightRemoteModel_;
            rightRemoteModel_ = nullptr;
            if (actConnect_)
                actConnect_->setEnabled(true);
            if (actDisconnect_)
                actDisconnect_->setEnabled(false);
            if (actDownloadF7_)
                actDownloadF7_->setEnabled(false);
            if (actUploadRight_)
                actUploadRight_->setEnabled(false);
            if (actRefreshRight_)
                actRefreshRight_->setEnabled(false);
            if (actOpenTerminalRight_)
                actOpenTerminalRight_->setEnabled(false);
            if (actNewDirRight_)
                actNewDirRight_->setEnabled(true);
            if (actNewFileRight_)
                actNewFileRight_->setEnabled(true);
            if (actRenameRight_)
                actRenameRight_->setEnabled(true);
            if (actDeleteRight_)
                actDeleteRight_->setEnabled(true);
            if (actMoveRight_)
                actMoveRight_->setEnabled(true);
            if (actMoveRightTb_)
                actMoveRightTb_->setEnabled(true);
            if (actCopyRightTb_)
                actCopyRightTb_->setEnabled(true);
            if (actSearchRight_)
                actSearchRight_->setEnabled(true);
            if (actChooseRight_) {
                actChooseRight_->setIcon(QIcon(
                    QLatin1String(":/assets/icons/action-open-folder.svg")));
                actChooseRight_->setEnabled(true);
                actChooseRight_->setToolTip(actChooseRight_->text());
            }
            if (QDir(rightPath_->text()).exists()) {
                setRightRoot(rightPath_->text());
            } else {
                setRightRoot(QDir::homePath());
            }
            if (rightView_)
                rightView_->setEnabled(true);
            rightRemoteWritable_ = false;
            applyRemoteWriteabilityActions();
            updateDeleteShortcutEnables();
            setWindowTitle(tr("OpenSCP — local/local"));
            return;
        }
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
        rightPath_->setText(rightRemoteModel_->rootPath());
        rightIsRemote_ = true;
        pendingRemoteRefreshFromUpload_ = false;
        seenCompletedUploadTaskIds_.clear();
        seenCompletedTransferNoticeTaskIds_.clear();
        refreshRightBreadcrumbs();
        activeSessionOptions_ = opt;
        remoteWriteabilityCache_.clear();
        if (transferMgr_) {
            transferMgr_->setClient(sftp_.get());
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
            actRefreshRight_->setEnabled(true);
        if (actOpenTerminalRight_)
            actOpenTerminalRight_->setEnabled(true);
        if (actSearchRight_)
            actSearchRight_->setEnabled(true);
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
        updateRemoteWriteability();
        updateDeleteShortcutEnables();
        return;
    }

    rightView_->setModel(rightLocalModel_);
    rightPath_->setText(QStringLiteral("/"));
    addRecentRemotePath(QStringLiteral("/"));
    rightIsRemote_ = true;
    activateScpTransferModeUi(true);
    pendingRemoteRefreshFromUpload_ = false;
    seenCompletedUploadTaskIds_.clear();
    seenCompletedTransferNoticeTaskIds_.clear();
    refreshRightBreadcrumbs();
    activeSessionOptions_ = opt;
    remoteWriteabilityCache_.clear();
    ++remoteWriteabilityProbeSeq_;
    rightRemoteWritable_ = false;
    if (transferMgr_) {
        transferMgr_->setClient(sftp_.get());
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
