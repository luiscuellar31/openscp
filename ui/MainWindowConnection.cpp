// MainWindow connection/session/security and saved-site persistence logic.
#include "MainWindow.hpp"
#include "ConnectionDialog.hpp"
#include "RemoteModel.hpp"
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
#include <QSet>
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
static inline void secureClear(QString &s) {
    for (int i = 0, n = s.size(); i < n; ++i)
        s[i] = QChar(u'\0');
    s.clear();
}
static inline void secureClear(QByteArray &b) {
    if (b.isEmpty())
        return;
    volatile char *p = reinterpret_cast<volatile char *>(b.data());
    for (int i = 0; i < b.size(); ++i)
        p[i] = 0;
    b.clear();
    b.squeeze();
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

static openscp::ScpTransferMode
loadDefaultScpTransferModeFromSettings(const QSettings &s) {
    return openscp::scpTransferModeFromStorageName(
        s.value("Protocol/scpTransferModeDefault",
                QString::fromLatin1(openscp::scpTransferModeStorageName(
                    openscp::ScpTransferMode::Auto)))
            .toString()
            .trimmed()
            .toLower()
            .toStdString());
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

static std::uint16_t defaultJumpPort() { return 22; }

static std::uint16_t defaultProxyPort(openscp::ProxyType type) {
    return openscp::defaultPortForProxyType(type);
}

static QString protocolDisplayLabel(openscp::Protocol protocol) {
    return QString::fromLatin1(openscp::protocolDisplayName(protocol));
}

static QString normalizeRemotePanelPath(const QString &rawPath) {
    QString normalized = rawPath.trimmed();
    if (normalized.isEmpty())
        normalized = QStringLiteral("/");
    if (!normalized.startsWith('/'))
        normalized.prepend('/');
    while (normalized.contains(QStringLiteral("//")))
        normalized.replace(QStringLiteral("//"), QStringLiteral("/"));
    if (normalized.size() > 1 && normalized.endsWith('/'))
        normalized.chop(1);
    return normalized;
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
               normalizedIdentityKeyPath(b.ftps_ca_cert_path);
}

static QString quickSiteSecretKey(const SiteEntry &e, const QString &item) {
    if (!e.id.isEmpty())
        return QString("site-id:%1:%2").arg(e.id, item);
    return QString("site:%1:%2").arg(e.name, item);
}

static QVector<SiteEntry> loadSavedSitesForQuickConnect(bool *needsSave) {
    QVector<SiteEntry> sites;
    bool shouldSave = false;
    QSettings s("OpenSCP", "OpenSCP");
    const auto defaultScpMode = loadDefaultScpTransferModeFromSettings(s);
    const bool defaultFtpsVerifyPeer =
        s.value("Security/ftpsVerifyPeerDefault", true).toBool();
    const QString defaultFtpsCaPath =
        s.value("Security/ftpsCaCertPathDefault", QString())
            .toString()
            .trimmed();
    const int n = s.beginReadArray("sites");
    QSet<QString> usedIds;
    for (int i = 0; i < n; ++i) {
        s.setArrayIndex(i);
        SiteEntry e;
        e.id = s.value("id").toString().trimmed();
        if (e.id.isEmpty() || usedIds.contains(e.id)) {
            e.id = newQuickSiteId();
            shouldSave = true;
        }
        usedIds.insert(e.id);
        e.name = s.value("name").toString().trimmed();
        e.opt.protocol = openscp::protocolFromStorageName(
            s.value("protocol",
                    QString::fromLatin1(
                        openscp::protocolStorageName(openscp::Protocol::Sftp)))
                .toString()
                .trimmed()
                .toLower()
                .toStdString());
        const bool hasScpTransferModeKey = s.contains("scpTransferMode");
        e.opt.scp_transfer_mode = openscp::scpTransferModeFromStorageName(
            s.value("scpTransferMode",
                    QString::fromLatin1(openscp::scpTransferModeStorageName(
                        defaultScpMode)))
                .toString()
                .trimmed()
                .toLower()
                .toStdString());
        if (!hasScpTransferModeKey)
            shouldSave = true;
        e.opt.host = s.value("host").toString().toStdString();
        e.opt.port = static_cast<std::uint16_t>(
            s.value("port",
                    static_cast<int>(
                        openscp::defaultPortForProtocol(e.opt.protocol)))
                .toUInt());
        e.opt.username = s.value("user").toString().toStdString();
        const QString kp = s.value("keyPath").toString();
        if (!kp.isEmpty())
            e.opt.private_key_path = kp.toStdString();
        e.opt.proxy_type = openscp::proxyTypeFromStorageValue(
            s.value("proxyType", static_cast<int>(openscp::ProxyType::None))
                .toInt());
        e.opt.proxy_host = s.value("proxyHost").toString().trimmed().toStdString();
        e.opt.proxy_port = static_cast<std::uint16_t>(
            s.value("proxyPort", static_cast<int>(defaultProxyPort(e.opt.proxy_type)))
                .toUInt());
        const QString proxyUser = s.value("proxyUser").toString().trimmed();
        if (!proxyUser.isEmpty())
            e.opt.proxy_username = proxyUser.toStdString();
        const QString jumpHost = s.value("jumpHost").toString().trimmed();
        if (!jumpHost.isEmpty())
            e.opt.jump_host = jumpHost.toStdString();
        e.opt.jump_port = static_cast<std::uint16_t>(
            s.value("jumpPort", static_cast<int>(defaultJumpPort())).toUInt());
        const QString jumpUser = s.value("jumpUser").toString().trimmed();
        if (!jumpUser.isEmpty())
            e.opt.jump_username = jumpUser.toStdString();
        const QString jumpKeyPath = s.value("jumpKeyPath").toString();
        if (!jumpKeyPath.isEmpty())
            e.opt.jump_private_key_path = jumpKeyPath.toStdString();
        const QString kh = s.value("knownHosts").toString();
        if (!kh.isEmpty())
            e.opt.known_hosts_path = kh.toStdString();
        e.opt.known_hosts_policy = static_cast<openscp::KnownHostsPolicy>(
            s.value("khPolicy",
                    static_cast<int>(openscp::KnownHostsPolicy::Strict))
                .toInt());
        e.opt.transfer_integrity_policy =
            static_cast<openscp::TransferIntegrityPolicy>(
                s.value("integrityPolicy",
                        static_cast<int>(
                            openscp::TransferIntegrityPolicy::Optional))
                    .toInt());
        e.opt.ftps_verify_peer =
            s.value("ftpsVerifyPeer", defaultFtpsVerifyPeer).toBool();
        const QString ftpsCaPath =
            s.value("ftpsCaCertPath", defaultFtpsCaPath).toString().trimmed();
        if (!ftpsCaPath.isEmpty())
            e.opt.ftps_ca_cert_path = ftpsCaPath.toStdString();
        sites.push_back(e);
    }
    s.endArray();
    if (needsSave)
        *needsSave = shouldSave;
    return sites;
}

static void saveSavedSitesForQuickConnect(const QVector<SiteEntry> &sites) {
    QSettings s("OpenSCP", "OpenSCP");
    s.remove("sites");
    s.beginWriteArray("sites");
    for (int i = 0; i < sites.size(); ++i) {
        s.setArrayIndex(i);
        const SiteEntry &e = sites[i];
        s.setValue("id", e.id);
        s.setValue("name", e.name);
        s.setValue("protocol",
                   QString::fromLatin1(
                       openscp::protocolStorageName(e.opt.protocol)));
        s.setValue("scpTransferMode",
                   QString::fromLatin1(openscp::scpTransferModeStorageName(
                       e.opt.scp_transfer_mode)));
        s.setValue("host", QString::fromStdString(e.opt.host));
        s.setValue("port", static_cast<int>(e.opt.port));
        s.setValue("user", QString::fromStdString(e.opt.username));
        s.setValue("keyPath",
                   e.opt.private_key_path
                       ? QString::fromStdString(*e.opt.private_key_path)
                       : QString());
        s.setValue("proxyType", static_cast<int>(e.opt.proxy_type));
        s.setValue("proxyHost", QString::fromStdString(e.opt.proxy_host));
        s.setValue("proxyPort", static_cast<int>(e.opt.proxy_port));
        s.setValue("proxyUser",
                   e.opt.proxy_username
                       ? QString::fromStdString(*e.opt.proxy_username)
                       : QString());
        s.setValue("jumpHost",
                   e.opt.jump_host ? QString::fromStdString(*e.opt.jump_host)
                                   : QString());
        s.setValue("jumpPort", static_cast<int>(e.opt.jump_port));
        s.setValue("jumpUser",
                   e.opt.jump_username
                       ? QString::fromStdString(*e.opt.jump_username)
                       : QString());
        s.setValue("jumpKeyPath",
                   e.opt.jump_private_key_path
                       ? QString::fromStdString(*e.opt.jump_private_key_path)
                       : QString());
        s.setValue("knownHosts",
                   e.opt.known_hosts_path
                       ? QString::fromStdString(*e.opt.known_hosts_path)
                       : QString());
        s.setValue("khPolicy", static_cast<int>(e.opt.known_hosts_policy));
        s.setValue("integrityPolicy",
                   static_cast<int>(e.opt.transfer_integrity_policy));
        s.setValue("ftpsVerifyPeer", e.opt.ftps_verify_peer);
        s.setValue("ftpsCaCertPath",
                   e.opt.ftps_ca_cert_path
                       ? QString::fromStdString(*e.opt.ftps_ca_cert_path)
                       : QString());
    }
    s.endArray();
    s.sync();
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
        for (const auto &s : sites) {
            if (s.name.compare(candidate, Qt::CaseInsensitive) == 0)
                return true;
        }
        return false;
    };
    if (!exists(base))
        return base;
    for (int i = 2; i < 10000; ++i) {
        const QString candidate = QString("%1 (%2)").arg(base).arg(i);
        if (!exists(candidate))
            return candidate;
    }
    return base +
           QString(" (%1)").arg(
               QUuid::createUuid().toString(QUuid::WithoutBraces).left(6));
}

static QString quickPersistStatusShort(SecretStore::PersistStatus st) {
    switch (st) {
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
    if (!rightIsRemote_ || !sftp_ || !m_activeSessionOptions_.has_value()) {
        if (errorOut)
            *errorOut = tr("No active remote session to reconnect.");
        return false;
    }
    if (m_isDisconnecting || m_connectInProgress_) {
        if (errorOut)
            *errorOut = tr("Connection state is changing; reconnect skipped.");
        return false;
    }
    if (m_remoteSessionReconnectInFlight_.exchange(true)) {
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
    auto replacement = sftp_->newConnectionLike(*m_activeSessionOptions_, connErr);
    if (!replacement) {
        m_remoteSessionReconnectInFlight_.store(false);
        if (errorOut)
            *errorOut = QString::fromStdString(connErr);
        return false;
    }

    sftp_->disconnect();
    sftp_ = std::move(replacement);
    applyRemoteConnectedUI(*m_activeSessionOptions_);
    if (!sftp_ || (!isScpTransferMode() && !rightRemoteModel_)) {
        m_remoteSessionReconnectInFlight_.store(false);
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
        transferMgr_->setSessionOptions(*m_activeSessionOptions_);
    }

    startRemoteSessionHealthMonitoring();
    statusBar()->showMessage(tr("Remote session reconnected"), 4000);
    m_remoteSessionReconnectInFlight_.store(false);
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
            sftp_ && !m_isDisconnecting) {
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
    if (m_remoteSessionHealthTimer_)
        return;

    m_remoteSessionHealthTimer_ = new QTimer(this);
    m_remoteSessionHealthTimer_->setSingleShot(false);
    m_remoteSessionHealthTimer_->setInterval(m_remoteSessionHealthIntervalMs_);
    connect(m_remoteSessionHealthTimer_, &QTimer::timeout, this, [this] {
        runRemoteSessionHealthCheck(tr("periodic"), false);
    });

    auto *guiApp = qobject_cast<QGuiApplication *>(QCoreApplication::instance());
    if (!guiApp)
        return;
    connect(guiApp, &QGuiApplication::applicationStateChanged, this,
            [this](Qt::ApplicationState state) {
                const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                if (state == Qt::ApplicationActive) {
                    if (m_lastAppInactiveAtMs_ <= 0) {
                        m_lastAppInactiveAtMs_ = 0;
                        return;
                    }
                    const qint64 inactiveMs = nowMs - m_lastAppInactiveAtMs_;
                    m_lastAppInactiveAtMs_ = 0;
                    constexpr qint64 kResumeProbeThresholdMs = 60 * 1000;
                    if (inactiveMs >= kResumeProbeThresholdMs && rightIsRemote_ &&
                        sftp_) {
                        runRemoteSessionHealthCheck(
                            tr("resume (%1s)").arg(inactiveMs / 1000), true);
                    }
                    return;
                }
                m_lastAppInactiveAtMs_ = nowMs;
            });
}

void MainWindow::startRemoteSessionHealthMonitoring() {
    if (!rightIsRemote_ || !sftp_)
        return;
    ensureRemoteSessionHealthMonitoring();
    if (!m_remoteSessionHealthTimer_)
        return;
    if (m_remoteSessionHealthIntervalMs_ < 60000)
        m_remoteSessionHealthIntervalMs_ = 60000;
    m_remoteSessionHealthTimer_->setInterval(m_remoteSessionHealthIntervalMs_);
    m_remoteSessionHealthProbeInFlight_.store(false);
    m_lastAppInactiveAtMs_ = 0;
    if (!m_remoteSessionHealthTimer_->isActive())
        m_remoteSessionHealthTimer_->start();
}

void MainWindow::stopRemoteSessionHealthMonitoring() {
    if (m_remoteSessionHealthTimer_)
        m_remoteSessionHealthTimer_->stop();
    m_remoteSessionHealthProbeInFlight_.store(false);
    m_remoteSessionReconnectInFlight_.store(false);
    m_lastAppInactiveAtMs_ = 0;
}

void MainWindow::runRemoteSessionHealthCheck(const QString &reason, bool force) {
    if (!rightIsRemote_ || !sftp_ || !m_activeSessionOptions_.has_value())
        return;
    if (m_isDisconnecting || m_connectInProgress_)
        return;
    bool expected = false;
    if (!m_remoteSessionHealthProbeInFlight_.compare_exchange_strong(expected,
                                                                     true)) {
        return;
    }

    const QString probePath =
        (rightRemoteModel_ && !rightRemoteModel_->rootPath().isEmpty())
            ? rightRemoteModel_->rootPath()
            : QStringLiteral("/");
    std::string err;
    bool isDir = false;
    const bool ok = executeCriticalRemoteOperation(
        tr("validate the remote session"),
        [probePath, &isDir](openscp::SftpClient *client, std::string &opErr) {
            bool exists = false;
            exists = client->exists(probePath.toStdString(), isDir, opErr);
            if (exists)
                return true;
            return opErr.empty();
        },
        err);
    m_remoteSessionHealthProbeInFlight_.store(false);
    if (ok) {
        if (force && !reason.isEmpty()) {
            statusBar()->showMessage(tr("Remote session validated (%1)")
                                         .arg(reason),
                                     2500);
        }
        return;
    }
    if (!rightIsRemote_ || !sftp_ || m_isDisconnecting)
        return;

    const QString rawErr = QString::fromStdString(err);
    if (!isLikelyRemoteTransportError(rawErr))
        return;

    UiAlerts::warning(
        this, tr("Connection lost"),
        tr("The remote session no longer responds (%1).\n"
           "OpenSCP will disconnect to avoid inconsistent operations.\n%2")
            .arg(reason, shortRemoteError(rawErr, tr("Transport error."))));
    disconnectSftp();
}

void MainWindow::connectSftp() {
    ConnectionDialog dlg(this);
    dlg.setQuickConnectSaveOptionsVisible(true);
    if (dlg.exec() != QDialog::Accepted)
        return;
    auto opt = dlg.options();
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
        QSettings s("OpenSCP", "OpenSCP");
        opt.known_hosts_hash_names =
            s.value("Security/knownHostsHashed", true).toBool();
        opt.show_fp_hex = s.value("Security/fpHex", false).toBool();
    }
    if (hasTransportSelectionConflict(opt)) {
        UiAlerts::warning(
            this, tr("Invalid transport configuration"),
            tr("Proxy and SSH jump host cannot be used together in the same "
               "connection.\nChoose only one transport method."));
        statusBar()->showMessage(
            tr("Connection canceled: invalid transport configuration"), 5000);
        return;
    }
#ifdef Q_OS_WIN
    if (hasConfiguredJumpHost(opt)) {
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
        maybePersistQuickConnectSite(opt, *saveRequest, false);
        // Already persisted on request; connection lifecycle no longer needs to
        // do it.
        saveRequest.reset();
    }
    startSftpConnect(opt, saveRequest);
}

// Tear down the current remote session and restore local mode.
void MainWindow::disconnectSftp() {
    if (m_isDisconnecting)
        return;
    m_isDisconnecting = true;
    stopRemoteSessionHealthMonitoring();
    const quint64 disconnectSeq = ++m_disconnectSeq_;
    m_transferCleanupInProgress_ = (transferMgr_ != nullptr);
    m_transferCleanupStartedAtMs_ =
        m_transferCleanupInProgress_ ? QDateTime::currentMSecsSinceEpoch() : 0;
    saveRightHeaderState(true);
    if (actDisconnect_)
        actDisconnect_->setEnabled(false);
    if (actConnect_) {
        actConnect_->setEnabled(false);
        actConnect_->setToolTip(
            tr("Please wait while active transfers are canceled"));
    }

    if (m_remoteScanCancelRequested_)
        m_remoteScanCancelRequested_->store(true);
    if (m_remoteScanProgress_) {
        m_remoteScanProgress_->hide();
        m_remoteScanProgress_->deleteLater();
        m_remoteScanProgress_.clear();
    }
    m_remoteScanInProgress_ = false;

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
    m_pendingRemoteRefreshFromUpload_ = false;
    m_seenCompletedUploadTaskIds_.clear();
    m_seenCompletedTransferNoticeTaskIds_.clear();
    restoreRightHeaderState(false);
    if (QDir(rightPath_->text()).exists()) {
        setRightRoot(rightPath_->text());
    } else {
        setRightRoot(QDir::homePath());
    }
    rightRemoteWritable_ = false;
    m_remoteWriteabilityCache_.clear();
    ++m_remoteWriteabilityProbeSeq_;
    m_activeSessionOptions_.reset();
    m_sessionNoHostVerification_ = false;
    updateHostPolicyRiskBanner();
    if (actDownloadF7_)
        actDownloadF7_->setEnabled(false);
    if (actUploadRight_)
        actUploadRight_->setEnabled(false);
    if (actRefreshRight_)
        actRefreshRight_->setEnabled(false);
    if (actOpenTerminalRight_)
        actOpenTerminalRight_->setEnabled(false);
    // Local mode: re-enable local actions on the right panel
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
    statusBar()->showMessage(
        tr("Disconnecting… waiting for active transfers to stop"), 0);
    constexpr int kDisconnectWatchdogMs = 25000;
    QTimer::singleShot(kDisconnectWatchdogMs, this, [this, disconnectSeq]() {
        if (!m_isDisconnecting || disconnectSeq != m_disconnectSeq_)
            return;
        statusBar()->showMessage(
            tr("Disconnect timeout reached; forcing local mode while cleanup "
               "continues"),
            5000);
        completeDisconnectSftp(disconnectSeq, true);
    });

    // Stop transfer workers off the UI thread; clearClient() may need to join
    // active workers and can block while they unwind.
    if (transferMgr_) {
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
                    if (disconnectSeq == self->m_disconnectSeq_ &&
                        self->m_transferCleanupInProgress_) {
                        self->m_transferCleanupInProgress_ = false;
                        self->m_transferCleanupStartedAtMs_ = 0;
                        if (!self->m_isDisconnecting && !self->rightIsRemote_) {
                            if (self->actConnect_)
                                self->actConnect_->setToolTip(
                                    self->actConnect_->text());
                            self->statusBar()->showMessage(
                                tr("Background transfer cleanup finished"),
                                3000);
                        }
                    }
                    self->completeDisconnectSftp(disconnectSeq, false);
                },
                Qt::QueuedConnection);
        }).detach();
        return;
    }

    m_transferCleanupInProgress_ = false;
    m_transferCleanupStartedAtMs_ = 0;
    completeDisconnectSftp(disconnectSeq, false);
}

void MainWindow::completeDisconnectSftp(quint64 disconnectSeq, bool forced) {
    if (!m_isDisconnecting || disconnectSeq != m_disconnectSeq_)
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
    m_isDisconnecting = false;
    if (forced) {
        statusBar()->showMessage(
            tr("Disconnected (transfer cleanup still finishing in background)"),
            5000);
    } else {
        statusBar()->showMessage(tr("Disconnected"), 3000);
    }
    if (m_pendingCloseAfterDisconnect_) {
        m_pendingCloseAfterDisconnect_ = false;
        QTimer::singleShot(0, this, [this] { close(); });
        return;
    }
    if (!QCoreApplication::closingDown() && m_openSiteManagerOnDisconnect) {
        QTimer::singleShot(0, this, [this] { showSiteManagerNonModal(); });
    }
}

void MainWindow::setOpenSiteManagerOnDisconnect(bool on) {
    if (m_openSiteManagerOnDisconnect == on)
        return;
    m_openSiteManagerOnDisconnect = on;
    QSettings s("OpenSCP", "OpenSCP");
    s.setValue("UI/openSiteManagerOnDisconnect", on);
    s.sync();
}

void MainWindow::showSiteManagerNonModal() {
    if (QApplication::activeModalWidget()) {
        m_pendingOpenSiteManager = true;
        QObject *modal = QApplication::activeModalWidget();
        if (modal)
            connect(modal, &QObject::destroyed, this,
                    &MainWindow::maybeOpenSiteManagerAfterModal,
                    Qt::UniqueConnection);
        return; // don't open underneath a modal
    }
    if (m_siteManager) {
        m_siteManager->show();
        m_siteManager->raise();
        m_siteManager->activateWindow();
        return;
    }
    auto *dlg = new SiteManagerDialog(this);
    m_siteManager = dlg;
    dlg->setAttribute(Qt::WA_DeleteOnClose, true);
    connect(dlg, &QObject::destroyed, this, [this] { m_siteManager.clear(); });
    connect(dlg, &QDialog::finished, this, [this, dlg](int res) {
        if (res == QDialog::Accepted && dlg) {
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

void MainWindow::setOpenSiteManagerOnStartup(bool on) {
    if (m_openSiteManagerOnStartup == on)
        return;
    m_openSiteManagerOnStartup = on;
    QSettings s("OpenSCP", "OpenSCP");
    s.setValue("UI/showConnOnStart", on);
    s.sync();
}

void MainWindow::maybeOpenSiteManagerAfterModal() {
    if (!QApplication::activeModalWidget() && m_pendingOpenSiteManager) {
        m_pendingOpenSiteManager = false;
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
    QSettings s("OpenSCP", "OpenSCP");
    const qint64 allowedUntil = s.value(allowKey, 0).toLongLong();
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
    bool ok = false;
    const QString entered =
        QInputDialog::getText(this, tr("Additional confirmation required"),
                              tr("To confirm, type exactly %1").arg(token),
                              QLineEdit::Normal, QString(), &ok)
            .trimmed();
    if (!ok || entered != token) {
        UiAlerts::information(
            this, tr("Connection canceled"),
            tr("Risk confirmation was not completed correctly."));
        return false;
    }

    // Temporary exception per host:port to avoid persistent bypasses.
    const int ttlMin = qBound(1, prefNoHostVerificationTtlMin_, 120);
    const qint64 newUntil = now + qint64(ttlMin) * 60;
    s.setValue(allowKey, newUntil);
    s.sync();
    const QDateTime expLocal =
        QDateTime::fromSecsSinceEpoch(newUntil).toLocalTime();
    statusBar()->showMessage(
        tr("Temporary \"no verification\" exception active until %1")
            .arg(QLocale().toString(expLocal, QLocale::ShortFormat)),
        8000);
    return true;
}

void MainWindow::updateHostPolicyRiskBanner() {
    const bool show = rightIsRemote_ && m_sessionNoHostVerification_;
    if (!show) {
        if (m_hostPolicyRiskLabel_)
            m_hostPolicyRiskLabel_->hide();
        return;
    }
    if (!m_hostPolicyRiskLabel_) {
        m_hostPolicyRiskLabel_ = new QLabel(this);
        m_hostPolicyRiskLabel_->setStyleSheet(
            "QLabel { color: #B00020; font-weight: 600; }");
        statusBar()->addPermanentWidget(m_hostPolicyRiskLabel_);
    }
    m_hostPolicyRiskLabel_->setText(
        tr("Risk: host key not verified in this session"));
    m_hostPolicyRiskLabel_->setToolTip(tr(
        "The current session does not validate host key; MITM risk exists."));
    m_hostPolicyRiskLabel_->show();
}

QString MainWindow::defaultDownloadDirFromSettings(const QSettings &s) {
    QString fallback =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (fallback.isEmpty())
        fallback = QDir::homePath() + "/Downloads";
    QString configured = QDir::cleanPath(
        s.value("UI/defaultDownloadDir", fallback).toString().trimmed());
    if (configured.isEmpty())
        configured = fallback;
    return configured;
}

bool MainWindow::confirmHostKeyUI(const QString &host, quint16 port,
                                  const QString &algorithm,
                                  const QString &fingerprint, bool canSave) {
    m_tofuHost_ = host + ":" + QString::number(port);
    m_tofuAlg_ = algorithm;
    m_tofuFp_ = fingerprint;
    m_tofuCanSave_ = canSave;
    {
        std::unique_lock<std::mutex> lk(m_tofuMutex_);
        m_tofuDecided_ = false;
        m_tofuAccepted_ = false;
    }
    QMetaObject::invokeMethod(
        this,
        [this, host, algorithm, fingerprint] {
            showTOfuDialog(host, algorithm, fingerprint);
        },
        Qt::QueuedConnection);
    std::unique_lock<std::mutex> lk(m_tofuMutex_);
    m_tofuCv_.wait(lk, [&] { return m_tofuDecided_; });
    return m_tofuAccepted_;
}

// Explicit non‑modal TOFU dialog per spec: open() + finished -> onTofuFinished
void MainWindow::showTOfuDialog(const QString &host, const QString &alg,
                                const QString &fp) {
    if (m_tofuBox) {
        m_tofuBox->raise();
        m_tofuBox->activateWindow();
        return;
    }
    // If a connection progress dialog is visible, disable it so it does not
    // capture input
    if (m_connectProgress_ && m_connectProgress_->isVisible()) {
        m_connectProgress_->setEnabled(false);
        m_connectProgressDimmed_ = true;
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
    m_tofuBox = box;
    box->setAttribute(Qt::WA_DeleteOnClose, true);
    box->setWindowModality(Qt::WindowModal);
    box->setIcon(QMessageBox::Question);
    box->setWindowTitle(tr("Confirm SSH fingerprint"));
    QString text = QString(tr("Connect to %1\nAlgorithm: %2\nFingerprint: "
                              "%3\n\nTrust and save to known_hosts?"))
                       .arg(host)
                       .arg(alg)
                       .arg(fp);
    if (!m_tofuCanSave_) {
        text = QString(tr("Connect to %1\nAlgorithm: %2\nFingerprint: "
                          "%3\n\nFingerprint cannot be saved. Connection "
                          "allowed only this time."))
                   .arg(host)
                   .arg(alg)
                   .arg(fp);
    }
    box->setText(text);
    box->addButton(m_tofuCanSave_ ? tr("Trust") : tr("Connect without saving"),
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

void MainWindow::onTofuFinished(int r) {
    bool accept = (r == QDialog::Accepted || r == QMessageBox::Yes);
    if (m_tofuBox) {
        const auto *clicked = m_tofuBox->clickedButton();
        if (clicked) {
            auto role = m_tofuBox->buttonRole((QAbstractButton *)clicked);
            accept = (role == QMessageBox::YesRole ||
                      role == QMessageBox::AcceptRole);
        }
        m_tofuBox->deleteLater();
        m_tofuBox.clear();
    }
    if (!m_tofuCanSave_ && accept) {
        statusBar()->showMessage(
            tr("Could not save fingerprint; allowing one-time connection"),
            5000);
    } else if (!accept) {
        statusBar()->showMessage(
            tr("Connection cancelled: fingerprint not accepted"), 5000);
    }
    // Re-enable progress if it was dimmed
    if (m_connectProgressDimmed_ && m_connectProgress_) {
        m_connectProgress_->setEnabled(true);
        m_connectProgressDimmed_ = false;
        if (openscp::sensitiveLoggingEnabled()) {
            std::fprintf(stderr,
                         "[OpenSCP] TOFU closed; progress resumed=true\n");
        }
    } else {
        if (openscp::sensitiveLoggingEnabled()) {
            std::fprintf(stderr,
                         "[OpenSCP] TOFU closed; progress resumed=false\n");
        }
    }
    {
        std::unique_lock<std::mutex> lk(m_tofuMutex_);
        m_tofuAccepted_ = accept;
        m_tofuDecided_ = true;
    }
    m_tofuCv_.notify_one();
}

// Secondary non‑modal dialog for one‑time connection without saving
void MainWindow::showOneTimeDialog(const QString &host, const QString &alg,
                                   const QString &fp) {
    if (m_tofuBox) {
        m_tofuBox->raise();
        m_tofuBox->activateWindow();
        return;
    }
    auto *box = new QMessageBox(this);
    UiAlerts::configure(*box);
    m_tofuBox = box;
    box->setAttribute(Qt::WA_DeleteOnClose, true);
    box->setWindowModality(Qt::WindowModal);
    box->setIcon(QMessageBox::Warning);
    box->setWindowTitle(tr("Additional confirmation"));
    box->setText(
        QString(
            tr("Could not save the fingerprint. Connect only this time without "
               "saving?\n\nHost: %1\nAlgorithm: %2\nFingerprint: %3"))
            .arg(host, alg, fp));
    box->addButton(tr("Connect without saving"), QMessageBox::YesRole);
    box->addButton(tr("Cancel"), QMessageBox::RejectRole);
    connect(box, &QMessageBox::finished, this, &MainWindow::onOneTimeFinished);
    QTimer::singleShot(0, box, [box] { box->open(); });
}

void MainWindow::onOneTimeFinished(int r) {
    bool accept = (r == QDialog::Accepted || r == QMessageBox::Yes);
    if (m_tofuBox) {
        const auto *clicked = m_tofuBox->clickedButton();
        if (clicked) {
            auto role = m_tofuBox->buttonRole((QAbstractButton *)clicked);
            accept = (role == QMessageBox::YesRole ||
                      role == QMessageBox::AcceptRole);
        }
        m_tofuBox->deleteLater();
        m_tofuBox.clear();
    }
    if (accept)
        statusBar()->showMessage(
            tr("One-time connection without saving confirmed by user"), 5000);
    else
        statusBar()->showMessage(tr("Connection cancelled after save failure"),
                                 5000);
    {
        std::unique_lock<std::mutex> lk(m_tofuMutex_);
        m_tofuAccepted_ = accept;
        m_tofuDecided_ = true;
    }
    m_tofuCv_.notify_one();
}
void MainWindow::startSftpConnect(
    openscp::SessionOptions opt,
    std::optional<PendingSiteSaveRequest> saveRequest) {
    if (m_transferCleanupInProgress_) {
        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const int elapsedSec =
            (m_transferCleanupStartedAtMs_ > 0)
                ? int((nowMs - m_transferCleanupStartedAtMs_) / 1000)
                : 0;
        statusBar()->showMessage(
            tr("Please wait: previous transfer cleanup is still running (%1s)")
                .arg(elapsedSec),
            4000);
        return;
    }
    if (m_connectInProgress_) {
        statusBar()->showMessage(tr("A connection is already in progress"),
                                 3000);
        return;
    }
    if (rightIsRemote_ || sftp_) {
        statusBar()->showMessage(tr("An active remote session already exists"),
                                 3000);
        return;
    }
    const openscp::ProtocolCapabilities caps =
        openscp::capabilitiesForProtocol(opt.protocol);
    if (!caps.implemented || !caps.supports_file_transfers) {
        const QString protocol = protocolDisplayLabel(opt.protocol);
        UiAlerts::information(
            this, tr("Protocol not available"),
            tr("%1 support is not implemented yet.").arg(protocol));
        statusBar()->showMessage(
            tr("Connection canceled: unsupported protocol %1").arg(protocol),
            5000);
        return;
    }
    if (hasConfiguredJumpHost(opt) && !caps.supports_jump_host) {
        UiAlerts::warning(
            this, tr("Unsupported transport"),
            tr("SSH jump host is not available for %1.")
                .arg(protocolDisplayLabel(opt.protocol)));
        statusBar()->showMessage(
            tr("Connection canceled: SSH jump host is not supported for %1")
                .arg(protocolDisplayLabel(opt.protocol)),
            5000);
        return;
    }
    if (opt.proxy_type != openscp::ProxyType::None && !caps.supports_proxy) {
        UiAlerts::warning(
            this, tr("Unsupported transport"),
            tr("Proxy settings are not available for %1.")
                .arg(protocolDisplayLabel(opt.protocol)));
        statusBar()->showMessage(
            tr("Connection canceled: proxy is not supported for %1")
                .arg(protocolDisplayLabel(opt.protocol)),
            5000);
        return;
    }
    if (hasTransportSelectionConflict(opt)) {
        UiAlerts::warning(
            this, tr("Invalid transport configuration"),
            tr("Proxy and SSH jump host cannot be used together in the same "
               "connection.\nEdit the site and keep only one transport."));
        statusBar()->showMessage(
            tr("Connection canceled: invalid transport configuration"), 5000);
        return;
    }
#ifdef Q_OS_WIN
    if (hasConfiguredJumpHost(opt)) {
        UiAlerts::warning(
            this, tr("Unsupported transport"),
            tr("SSH jump host is currently unavailable on Windows."));
        statusBar()->showMessage(
            tr("Connection canceled: SSH jump host is unsupported on Windows"),
            5000);
        return;
    }
#endif
    if (!confirmInsecureHostPolicyForSession(opt)) {
        statusBar()->showMessage(
            tr("Connection canceled: no-verification policy not confirmed"),
            5000);
        return;
    }

    const openscp::SessionOptions uiOpt = opt;
    QPointer<MainWindow> self(this);
    auto cancelFlag = std::make_shared<std::atomic<bool>>(false);
    m_connectCancelRequested_ = cancelFlag;
    m_connectInProgress_ = true;

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
        if (m_connectCancelRequested_) {
            m_connectCancelRequested_->store(true);
            statusBar()->showMessage(tr("Canceling connection…"), 3000);
        }
    });
    progress->show();
    progress->raise();
    m_connectProgress_ = progress;
    m_connectProgressDimmed_ = false;

    // Inject host key confirmation (TOFU) via UI
    opt.hostkey_confirm_cb = [self](const std::string &h, std::uint16_t p,
                                    const std::string &alg,
                                    const std::string &fp, bool canSave) {
        if (!self)
            return false;
        return self->confirmHostKeyUI(QString::fromStdString(h), (quint16)p,
                                      QString::fromStdString(alg),
                                      QString::fromStdString(fp), canSave);
    };
    opt.hostkey_status_cb = [self](const std::string &msg) {
        if (!self)
            return;
        const QString q = QString::fromStdString(msg);
        QMetaObject::invokeMethod(
            self,
            [self, q] {
                if (self)
                    self->statusBar()->showMessage(q, 5000);
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
        // Resolve each prompt: auto-fill user/pass and ask for OTP/codes if
        // present
        for (const std::string &p : prompts) {
            QString qprompt = QString::fromStdString(p);
            QString lower = qprompt.toLower();
            // Username
            if (lower.contains("user") || lower.contains("name:")) {
                responses.emplace_back(savedUser);
                continue;
            }
            // Password
            if (lower.contains("password") || lower.contains("passphrase") ||
                lower.contains("passcode")) {
                if (!savedPass.empty()) {
                    responses.emplace_back(savedPass);
                    continue;
                }
                // Ask for password if we did not have it
                QString ans;
                bool ok = false;
                QMetaObject::invokeMethod(
                    self,
                    [&] {
                        if (!self)
                            return;
                        ans = QInputDialog::getText(
                            self, tr("Password required"), qprompt,
                            QLineEdit::Password, QString(), &ok);
                    },
                    Qt::BlockingQueuedConnection);
                if (!ok)
                    return openscp::KbdIntPromptResult::Cancelled;
                {
                    QByteArray bytes = ans.toUtf8();
                    responses.emplace_back(bytes.constData(),
                                           (size_t)bytes.size());
                    secureClear(bytes);
                }
                secureClear(ans);
                continue;
            }
            // OTP / Verification code / Token
            if (lower.contains("verification") || lower.contains("verify") ||
                lower.contains("otp") || lower.contains("code") ||
                lower.contains("token")) {
                QString title = tr("Verification code required");
                if (!instruction.empty())
                    title += " — " + QString::fromStdString(instruction);
                QString ans;
                bool ok = false;
                QMetaObject::invokeMethod(
                    self,
                    [&] {
                        if (!self)
                            return;
                        ans = QInputDialog::getText(self, title, qprompt,
                                                    QLineEdit::Password,
                                                    QString(), &ok);
                    },
                    Qt::BlockingQueuedConnection);
                if (!ok)
                    return openscp::KbdIntPromptResult::Cancelled;
                {
                    QByteArray bytes = ans.toUtf8();
                    responses.emplace_back(bytes.constData(),
                                           (size_t)bytes.size());
                    secureClear(bytes);
                }
                secureClear(ans);
                continue;
            }
            // Generic case: ask for text (not hidden)
            QString title = tr("Information required");
            if (!instruction.empty())
                title += " — " + QString::fromStdString(instruction);
            QString ans;
            bool ok = false;
            QMetaObject::invokeMethod(
                self,
                [&] {
                    if (!self)
                        return;
                    ans = QInputDialog::getText(self, title, qprompt,
                                                QLineEdit::Normal, QString(),
                                                &ok);
                },
                Qt::BlockingQueuedConnection);
            if (!ok)
                return openscp::KbdIntPromptResult::Cancelled;
            {
                QByteArray bytes = ans.toUtf8();
                responses.emplace_back(bytes.constData(), (size_t)bytes.size());
                secureClear(bytes);
            }
            secureClear(ans);
        }
        return (responses.size() == prompts.size())
                   ? openscp::KbdIntPromptResult::Handled
                   : openscp::KbdIntPromptResult::Unhandled;
    };

    std::thread([self, opt = std::move(opt), uiOpt, saveRequest,
                 cancelFlag]() mutable {
        bool okConn = false;
        bool canceledByUser = false;
        std::string err;
        openscp::SftpClient *connectedClient = nullptr;
        try {
            if (cancelFlag && cancelFlag->load()) {
                canceledByUser = true;
                err = "Connection canceled by user";
            } else {
                auto tmp = openscp::CreateConnectedClient(opt, err);
                okConn = static_cast<bool>(tmp);
                if (cancelFlag && cancelFlag->load()) {
                    canceledByUser = true;
                    if (okConn)
                        tmp->disconnect();
                    okConn = false;
                    if (err.empty())
                        err = "Connection canceled by user";
                }
                if (okConn)
                    connectedClient = tmp.release();
            }
        } catch (const std::exception &ex) {
            err = std::string("Connection exception: ") + ex.what();
            okConn = false;
        } catch (...) {
            err = "Unknown connection exception";
            okConn = false;
        }

        const QString qerr = QString::fromStdString(err);
        const bool queued = QMetaObject::invokeMethod(
            qApp,
            [self, okConn, qerr, connectedClient, uiOpt, saveRequest,
             canceledByUser]() {
                if (!self) {
                    if (connectedClient) {
                        connectedClient->disconnect();
                        delete connectedClient;
                    }
                    return;
                }
                self->finalizeSftpConnect(okConn, qerr, connectedClient, uiOpt,
                                          saveRequest, canceledByUser);
            },
            Qt::QueuedConnection);
        if (!queued && connectedClient) {
            connectedClient->disconnect();
            delete connectedClient;
        }
    }).detach();
}

void MainWindow::finalizeSftpConnect(
    bool okConn, const QString &err, openscp::SftpClient *connectedClient,
    const openscp::SessionOptions &uiOpt,
    std::optional<PendingSiteSaveRequest> saveRequest, bool canceledByUser) {
    std::unique_ptr<openscp::SftpClient> guard(connectedClient);
    if (m_connectProgress_) {
        m_connectProgress_->close();
        m_connectProgress_.clear();
    }
    m_connectProgressDimmed_ = false;
    m_connectCancelRequested_.reset();
    m_connectInProgress_ = false;
    if (actSites_)
        actSites_->setEnabled(true);

    if (!okConn) {
        if (actConnect_ && !rightIsRemote_)
            actConnect_->setEnabled(true);
        if (canceledByUser)
            statusBar()->showMessage(tr("Connection canceled"), 4000);
        else {
            UiAlerts::critical(
                this, tr("Connection error"),
                tr("Could not connect to the server.\n%1")
                    .arg(shortRemoteError(
                        err, tr("Check host, port, and credentials."))));
        }
        return;
    }

    m_sessionNoHostVerification_ =
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
    for (int i = 0; i < sites.size(); ++i) {
        if (sameSavedSiteIdentity(sites[i].opt, opt)) {
            matchIndex = i;
            break;
        }
    }

    bool created = false;
    if (matchIndex < 0) {
        SiteEntry e;
        e.id = newQuickSiteId();
        e.name = ensureUniqueQuickSiteName(
            sites, req.siteName.trimmed().isEmpty() ? defaultQuickSiteName(opt)
                                                    : req.siteName.trimmed());
        e.opt = opt;
        e.opt.password.reset();
        e.opt.private_key_passphrase.reset();
        e.opt.proxy_password.reset();
        sites.push_back(e);
        matchIndex = sites.size() - 1;
        created = true;
    }

    if (created || regeneratedIds) {
        saveSavedSitesForQuickConnect(sites);
        refreshOpenSiteManagerWidget(m_siteManager);
    }

    if (!req.saveCredentials) {
        if (connectionEstablished) {
            if (created)
                statusBar()->showMessage(tr("Connected. Site saved."), 5000);
            else
                statusBar()->showMessage(tr("Connected. Site already exists."),
                                         5000);
        } else {
            if (created)
                statusBar()->showMessage(tr("Site saved."), 5000);
            else
                statusBar()->showMessage(tr("Site already exists."), 5000);
        }
        return;
    }

    SecretStore store;
    QStringList issues;
    bool anyCredentialStored = false;
    const SiteEntry &target = sites[matchIndex];

    if (opt.password && !opt.password->empty()) {
        const auto r = store.setSecret(
            quickSiteSecretKey(target, QStringLiteral("password")),
            QString::fromStdString(*opt.password));
        if (r.ok())
            anyCredentialStored = true;
        else
            issues << tr("Password: %1").arg(quickPersistStatusShort(r.status));
    }
    if (opt.private_key_passphrase && !opt.private_key_passphrase->empty()) {
        const auto r = store.setSecret(
            quickSiteSecretKey(target, QStringLiteral("keypass")),
            QString::fromStdString(*opt.private_key_passphrase));
        if (r.ok())
            anyCredentialStored = true;
        else
            issues
                << tr("Passphrase: %1").arg(quickPersistStatusShort(r.status));
    }
    if (opt.proxy_type != openscp::ProxyType::None && opt.proxy_password &&
        !opt.proxy_password->empty()) {
        const auto r = store.setSecret(
            quickSiteSecretKey(target, QStringLiteral("proxypass")),
            QString::fromStdString(*opt.proxy_password));
        if (r.ok())
            anyCredentialStored = true;
        else
            issues << tr("Proxy password: %1")
                          .arg(quickPersistStatusShort(r.status));
    } else if (opt.proxy_type == openscp::ProxyType::None) {
        store.removeSecret(quickSiteSecretKey(target, QStringLiteral("proxypass")));
    }

    if (!issues.isEmpty()) {
        UiAlerts::warning(this, tr("Saved sites"),
                             tr("The site was saved, but some credentials "
                                "could not be saved:\n%1")
                                 .arg(issues.join("\n")));
    }

    if (connectionEstablished) {
        if (created && anyCredentialStored)
            statusBar()->showMessage(
                tr("Connected. Site and credentials saved."), 5000);
        else if (created)
            statusBar()->showMessage(tr("Connected. Site saved."), 5000);
        else if (anyCredentialStored)
            statusBar()->showMessage(tr("Connected. Credentials updated."),
                                     5000);
        else
            statusBar()->showMessage(tr("Connected. Site already exists."),
                                     5000);
    } else {
        if (created && anyCredentialStored)
            statusBar()->showMessage(tr("Site and credentials saved."), 5000);
        else if (created)
            statusBar()->showMessage(tr("Site saved."), 5000);
        else if (anyCredentialStored)
            statusBar()->showMessage(tr("Credentials updated."), 5000);
        else
            statusBar()->showMessage(tr("Site already exists."), 5000);
    }
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
                [this](const QString &path, bool ok, const QString &error) {
                    if (!rightRemoteModel_)
                        return;
                    if (!ok) {
                        UiAlerts::warning(
                            this, tr("Remote error"),
                            tr("Could not open the remote folder.\n%1")
                                .arg(shortRemoteError(
                                    error,
                                    tr("Failed to read remote contents."))));
                        return;
                    }
                    rightPath_->setText(path);
                    refreshRightBreadcrumbs();
                    if (rightIsRemote_) {
                        updateRemoteWriteability();
                        updateDeleteShortcutEnables();
                    }
                });
        QString e;
        if (!rightRemoteModel_->setRootPath("/", &e, false)) {
            UiAlerts::critical(
                this, tr("Error listing remote"),
                tr("Could not open the initial remote folder.\n%1")
                    .arg(shortRemoteError(
                        e, tr("Failed to read remote contents."))));
            sftp_.reset();
            rightView_->setModel(rightLocalModel_);
            activateScpTransferModeUi(false);
            m_remoteWriteabilityCache_.clear();
            m_activeSessionOptions_.reset();
            m_sessionNoHostVerification_ = false;
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
        m_pendingRemoteRefreshFromUpload_ = false;
        m_seenCompletedUploadTaskIds_.clear();
        m_seenCompletedTransferNoticeTaskIds_.clear();
        refreshRightBreadcrumbs();
        m_activeSessionOptions_ = opt;
        m_remoteWriteabilityCache_.clear();
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
        setWindowTitle(tr("OpenSCP — local/remote (%1)").arg(activeProtocol));
        updateHostPolicyRiskBanner();
        startRemoteSessionHealthMonitoring();
        updateRemoteWriteability();
        updateDeleteShortcutEnables();
        return;
    }

    rightView_->setModel(rightLocalModel_);
    rightPath_->setText(QStringLiteral("/"));
    rightIsRemote_ = true;
    activateScpTransferModeUi(true);
    m_pendingRemoteRefreshFromUpload_ = false;
    m_seenCompletedUploadTaskIds_.clear();
    m_seenCompletedTransferNoticeTaskIds_.clear();
    refreshRightBreadcrumbs();
    m_activeSessionOptions_ = opt;
    m_remoteWriteabilityCache_.clear();
    ++m_remoteWriteabilityProbeSeq_;
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
    setWindowTitle(tr("OpenSCP — local/remote (%1)").arg(activeProtocol));
    updateHostPolicyRiskBanner();
    startRemoteSessionHealthMonitoring();
    updateDeleteShortcutEnables();
}
