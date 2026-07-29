// MainWindow remote-side operations and writeability state.
#include "MainWindow.hpp"
#include "MainWindowSharedUtils.hpp"
#include "PermissionsDialog.hpp"
#include "RemoteModel.hpp"
#include "RemoteOperationController.hpp"
#include "TransferManager.hpp"
#include "UiAlerts.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QInputDialog>
#include <QLineEdit>
#include <QListView>
#include <QMenu>
#include <QMessageBox>
#include <QPointer>
#include <QProcess>
#include <QProgressDialog>
#include <QScrollBar>
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTreeView>

#include <string>

static constexpr int NAME_COL = 0;

static QString tempDownloadPathFor(const QString &remoteName) {
    QString base =
        QStandardPaths::writableLocation(QStandardPaths::DownloadLocation);
    if (base.isEmpty())
        base = QDir::homePath() + "/Downloads";
    QDir().mkpath(base);
    return QDir(base).filePath(remoteName);
}

// Reveal a file in the system file manager (select/highlight when possible),

static bool indicatesRemoteWriteabilityDenied(const QString &raw) {
    const QString lower = raw.trimmed().toLower();
    if (lower.isEmpty())
        return false;
    return lower.contains("permission denied") ||
           lower.contains("read-only") ||
           lower.contains("operation not permitted") ||
           lower.contains("access denied") ||
           lower.contains("sftp protocol error 3");
}

static QString trimOptionalString(const std::optional<std::string> &v) {
    if (!v || v->empty())
        return {};
    return QString::fromStdString(*v).trimmed();
}

static QString shellSingleQuote(const QString &value) {
    QString escaped = value;
    escaped.replace(QStringLiteral("'"), QStringLiteral("'\"'\"'"));
    return QStringLiteral("'") + escaped + QStringLiteral("'");
}

static QString shellJoinQuoted(const QStringList &args) {
    QStringList quoted;
    quoted.reserve(args.size());
    for (const QString &arg : args)
        quoted.push_back(shellSingleQuote(arg));
    return quoted.join(QLatin1Char(' '));
}

static QString defaultKnownHostsPath() {
    const QString home = QDir::homePath();
    if (home.isEmpty())
        return {};
    return QDir(home).filePath(QStringLiteral(".ssh/known_hosts"));
}

static bool buildOpenSshProxyCommand(const openscp::SessionOptions &opt,
                                     QString *proxyCommandOut,
                                     QString *errorOut) {
    if (proxyCommandOut)
        proxyCommandOut->clear();
    if (errorOut)
        errorOut->clear();

    if (!proxyCommandOut)
        return false;

    if (opt.proxy_type == openscp::ProxyType::None) {
        if (errorOut) {
            *errorOut = QCoreApplication::translate(
                "MainWindow", "Proxy command requested without proxy settings.");
        }
        return false;
    }

    const QString proxyHost = QString::fromStdString(opt.proxy_host).trimmed();
    const std::uint16_t proxyPort = opt.proxy_port;
    if (proxyHost.isEmpty() || proxyPort == 0) {
        if (errorOut) {
            *errorOut = QCoreApplication::translate(
                "MainWindow",
                "Proxy host/port is missing for terminal command.");
        }
        return false;
    }

    const QString proxyUser = trimOptionalString(opt.proxy_username);
    const QString proxyPass = trimOptionalString(opt.proxy_password);
    const bool wantsProxyAuth = !proxyUser.isEmpty() || !proxyPass.isEmpty();
    if (wantsProxyAuth && proxyUser.isEmpty()) {
        if (errorOut) {
            *errorOut = QCoreApplication::translate(
                "MainWindow", "Proxy authentication requires a username.");
        }
        return false;
    }

    auto ncatTypeForProxy = [&]() -> QString {
        if (opt.proxy_type == openscp::ProxyType::Socks5)
            return QStringLiteral("socks5");
        if (opt.proxy_type == openscp::ProxyType::HttpConnect)
            return QStringLiteral("http");
        return {};
    };

    if (wantsProxyAuth) {
        const QString ncatExe =
            QStandardPaths::findExecutable(QStringLiteral("ncat"));
        if (ncatExe.isEmpty()) {
            if (errorOut) {
                *errorOut = QCoreApplication::translate(
                    "MainWindow",
                    "Proxy authentication in terminal mode requires 'ncat' "
                    "(with --proxy-auth support).");
            }
            return false;
        }
        const QString ncatProxyType = ncatTypeForProxy();
        if (ncatProxyType.isEmpty()) {
            if (errorOut) {
                *errorOut = QCoreApplication::translate(
                    "MainWindow",
                    "Unsupported proxy type for terminal command.");
            }
            return false;
        }
        QStringList ncatArgs;
        ncatArgs << ncatExe << QStringLiteral("--proxy")
                 << QStringLiteral("%1:%2").arg(proxyHost).arg(proxyPort)
                 << QStringLiteral("--proxy-type") << ncatProxyType
                 << QStringLiteral("--proxy-auth")
                 << QStringLiteral("%1:%2").arg(proxyUser, proxyPass)
                 << QStringLiteral("%h") << QStringLiteral("%p");
        *proxyCommandOut = shellJoinQuoted(ncatArgs);
        return true;
    }

    const QString ncExe = QStandardPaths::findExecutable(QStringLiteral("nc"));
    if (!ncExe.isEmpty()) {
        QStringList ncArgs;
        ncArgs << ncExe << QStringLiteral("-x")
               << QStringLiteral("%1:%2").arg(proxyHost).arg(proxyPort);
        if (opt.proxy_type == openscp::ProxyType::Socks5) {
            ncArgs << QStringLiteral("-X") << QStringLiteral("5");
        } else if (opt.proxy_type == openscp::ProxyType::HttpConnect) {
            ncArgs << QStringLiteral("-X") << QStringLiteral("connect");
        } else {
            if (errorOut) {
                *errorOut = QCoreApplication::translate(
                    "MainWindow",
                    "Unsupported proxy type for terminal command.");
            }
            return false;
        }
        ncArgs << QStringLiteral("%h") << QStringLiteral("%p");
        *proxyCommandOut = shellJoinQuoted(ncArgs);
        return true;
    }

    const QString ncatExe = QStandardPaths::findExecutable(QStringLiteral("ncat"));
    if (!ncatExe.isEmpty()) {
        const QString ncatProxyType = ncatTypeForProxy();
        if (ncatProxyType.isEmpty()) {
            if (errorOut) {
                *errorOut = QCoreApplication::translate(
                    "MainWindow",
                    "Unsupported proxy type for terminal command.");
            }
            return false;
        }
        QStringList ncatArgs;
        ncatArgs << ncatExe << QStringLiteral("--proxy")
                 << QStringLiteral("%1:%2").arg(proxyHost).arg(proxyPort)
                 << QStringLiteral("--proxy-type") << ncatProxyType
                 << QStringLiteral("%h") << QStringLiteral("%p");
        *proxyCommandOut = shellJoinQuoted(ncatArgs);
        return true;
    }

    if (errorOut) {
        *errorOut = QCoreApplication::translate(
            "MainWindow",
            "Could not find a proxy helper for terminal mode (tried: nc, ncat).");
    }
    return false;
}

static bool buildRemoteTerminalSshCommand(const openscp::SessionOptions &opt,
                                          const QString &remotePath,
                                          bool forceInteractiveLogin,
                                          QString *commandOut,
                                          QString *errorOut) {
    if (commandOut)
        commandOut->clear();
    if (errorOut)
        errorOut->clear();

    if (!commandOut)
        return false;

    const QString sshExe = QStandardPaths::findExecutable(QStringLiteral("ssh"));
    if (sshExe.isEmpty()) {
        if (errorOut) {
            *errorOut = QCoreApplication::translate(
                "MainWindow", "OpenSSH client was not found in PATH.");
        }
        return false;
    }

    const QString host = QString::fromStdString(opt.host).trimmed();
    const QString user = QString::fromStdString(opt.username).trimmed();
    if (host.isEmpty() || user.isEmpty()) {
        if (errorOut) {
            *errorOut = QCoreApplication::translate(
                "MainWindow",
                "Session is missing host or username information.");
        }
        return false;
    }

    QStringList args;
    args << sshExe << QStringLiteral("-tt");
    args << QStringLiteral("-p") << QString::number(opt.port);

    if (opt.known_hosts_policy == openscp::KnownHostsPolicy::Off) {
        args << QStringLiteral("-o") << QStringLiteral("StrictHostKeyChecking=no");
        args << QStringLiteral("-o")
             << QStringLiteral("UserKnownHostsFile=/dev/null");
    } else {
        const QString strictValue =
            (opt.known_hosts_policy == openscp::KnownHostsPolicy::AcceptNew)
                ? QStringLiteral("accept-new")
                : QStringLiteral("yes");
        args << QStringLiteral("-o")
             << QStringLiteral("StrictHostKeyChecking=%1").arg(strictValue);

        QString khPath = trimOptionalString(opt.known_hosts_path);
        if (khPath.isEmpty())
            khPath = defaultKnownHostsPath();
        if (!khPath.isEmpty()) {
            const QString normalizedKh =
                QDir::fromNativeSeparators(QDir::cleanPath(khPath));
            args << QStringLiteral("-o")
                 << QStringLiteral("UserKnownHostsFile=%1").arg(normalizedKh);
        }
    }

    if (forceInteractiveLogin) {
        args << QStringLiteral("-o") << QStringLiteral("PubkeyAuthentication=no");
        args << QStringLiteral("-o")
             << QStringLiteral(
                    "PreferredAuthentications=keyboard-interactive,password");
    } else {
        const QString keyPath = trimOptionalString(opt.private_key_path);
        if (!keyPath.isEmpty()) {
            const QString normalizedKey =
                QDir::fromNativeSeparators(QDir::cleanPath(keyPath));
            args << QStringLiteral("-i") << normalizedKey;
            args << QStringLiteral("-o") << QStringLiteral("IdentitiesOnly=yes");
        }
    }

    const QString jumpHost = trimOptionalString(opt.jump_host);
    const bool useJump = !jumpHost.isEmpty();
    const bool useProxy = (opt.proxy_type != openscp::ProxyType::None);
    if (useJump && useProxy) {
        if (errorOut) {
            *errorOut = QCoreApplication::translate(
                "MainWindow",
                "Proxy and SSH jump host cannot be used together in the same "
                "terminal command.");
        }
        return false;
    }

    if (useJump) {
        const QString jumpUser = trimOptionalString(opt.jump_username);
        const QString jumpKeyPath = trimOptionalString(opt.jump_private_key_path);
        const std::uint16_t jumpPort = (opt.jump_port == 0) ? 22 : opt.jump_port;

        if (jumpKeyPath.isEmpty()) {
            QString jumpSpec = jumpHost;
            if (!jumpUser.isEmpty())
                jumpSpec = jumpUser + QStringLiteral("@") + jumpSpec;
            if (jumpPort != 22)
                jumpSpec += QStringLiteral(":") + QString::number(jumpPort);
            args << QStringLiteral("-J") << jumpSpec;
        } else {
            QStringList jumpCmd;
            jumpCmd << QStringLiteral("ssh");
            jumpCmd << QStringLiteral("-W") << QStringLiteral("%h:%p");
            jumpCmd << QStringLiteral("-p") << QString::number(jumpPort);
            if (!jumpUser.isEmpty())
                jumpCmd << QStringLiteral("-l") << jumpUser;
            jumpCmd << QStringLiteral("-i")
                    << QDir::fromNativeSeparators(
                           QDir::cleanPath(jumpKeyPath));
            jumpCmd << QStringLiteral("-o")
                    << QStringLiteral("IdentitiesOnly=yes");
            jumpCmd << jumpHost;
            args << QStringLiteral("-o")
                 << QStringLiteral("ProxyCommand=%1").arg(shellJoinQuoted(jumpCmd));
        }
    } else if (useProxy) {
        QString proxyCommand;
        QString proxyErr;
        if (!buildOpenSshProxyCommand(opt, &proxyCommand, &proxyErr)) {
            if (errorOut) {
                *errorOut = proxyErr.isEmpty()
                                ? QCoreApplication::translate(
                                      "MainWindow",
                                      "Could not build proxy command for "
                                      "terminal mode.")
                                : proxyErr;
            }
            return false;
        }
        args << QStringLiteral("-o")
             << QStringLiteral("ProxyCommand=%1").arg(proxyCommand);
    }

    args << QStringLiteral("%1@%2").arg(user, host);
    const QString remoteInit =
        QStringLiteral(
            "cd -- %1 2>/dev/null || cd /; exec ${SHELL:-/bin/sh} -l")
            .arg(shellSingleQuote(normalizeRemotePath(remotePath)));
    args << remoteInit;

    *commandOut = shellJoinQuoted(args);
    return true;
}

static bool buildRemoteSftpCliCommand(const openscp::SessionOptions &opt,
                                      const QString &remotePath,
                                      bool forceInteractiveLogin,
                                      QString *commandOut,
                                      QString *errorOut) {
    if (commandOut)
        commandOut->clear();
    if (errorOut)
        errorOut->clear();

    if (!commandOut)
        return false;

    const QString sftpExe = QStandardPaths::findExecutable(QStringLiteral("sftp"));
    if (sftpExe.isEmpty()) {
        if (errorOut) {
            *errorOut = QCoreApplication::translate(
                "MainWindow", "OpenSSH sftp client was not found in PATH.");
        }
        return false;
    }

    const QString host = QString::fromStdString(opt.host).trimmed();
    const QString user = QString::fromStdString(opt.username).trimmed();
    if (host.isEmpty() || user.isEmpty()) {
        if (errorOut) {
            *errorOut = QCoreApplication::translate(
                "MainWindow",
                "Session is missing host or username information.");
        }
        return false;
    }

    QStringList args;
    args << sftpExe;
    args << QStringLiteral("-P") << QString::number(opt.port);

    if (opt.known_hosts_policy == openscp::KnownHostsPolicy::Off) {
        args << QStringLiteral("-o") << QStringLiteral("StrictHostKeyChecking=no");
        args << QStringLiteral("-o")
             << QStringLiteral("UserKnownHostsFile=/dev/null");
    } else {
        const QString strictValue =
            (opt.known_hosts_policy == openscp::KnownHostsPolicy::AcceptNew)
                ? QStringLiteral("accept-new")
                : QStringLiteral("yes");
        args << QStringLiteral("-o")
             << QStringLiteral("StrictHostKeyChecking=%1").arg(strictValue);

        QString khPath = trimOptionalString(opt.known_hosts_path);
        if (khPath.isEmpty())
            khPath = defaultKnownHostsPath();
        if (!khPath.isEmpty()) {
            const QString normalizedKh =
                QDir::fromNativeSeparators(QDir::cleanPath(khPath));
            args << QStringLiteral("-o")
                 << QStringLiteral("UserKnownHostsFile=%1").arg(normalizedKh);
        }
    }

    if (forceInteractiveLogin) {
        args << QStringLiteral("-o") << QStringLiteral("PubkeyAuthentication=no");
        args << QStringLiteral("-o")
             << QStringLiteral(
                    "PreferredAuthentications=keyboard-interactive,password");
    } else {
        const QString keyPath = trimOptionalString(opt.private_key_path);
        if (!keyPath.isEmpty()) {
            const QString normalizedKey =
                QDir::fromNativeSeparators(QDir::cleanPath(keyPath));
            args << QStringLiteral("-i") << normalizedKey;
            args << QStringLiteral("-o") << QStringLiteral("IdentitiesOnly=yes");
        }
    }

    const QString jumpHost = trimOptionalString(opt.jump_host);
    const bool useJump = !jumpHost.isEmpty();
    const bool useProxy = (opt.proxy_type != openscp::ProxyType::None);
    if (useJump && useProxy) {
        if (errorOut) {
            *errorOut = QCoreApplication::translate(
                "MainWindow",
                "Proxy and SSH jump host cannot be used together in the same "
                "terminal command.");
        }
        return false;
    }

    if (useJump) {
        const QString jumpUser = trimOptionalString(opt.jump_username);
        const QString jumpKeyPath = trimOptionalString(opt.jump_private_key_path);
        const std::uint16_t jumpPort = (opt.jump_port == 0) ? 22 : opt.jump_port;

        if (jumpKeyPath.isEmpty()) {
            QString jumpSpec = jumpHost;
            if (!jumpUser.isEmpty())
                jumpSpec = jumpUser + QStringLiteral("@") + jumpSpec;
            if (jumpPort != 22)
                jumpSpec += QStringLiteral(":") + QString::number(jumpPort);
            args << QStringLiteral("-J") << jumpSpec;
        } else {
            QStringList jumpCmd;
            jumpCmd << QStringLiteral("ssh");
            jumpCmd << QStringLiteral("-W") << QStringLiteral("%h:%p");
            jumpCmd << QStringLiteral("-p") << QString::number(jumpPort);
            if (!jumpUser.isEmpty())
                jumpCmd << QStringLiteral("-l") << jumpUser;
            jumpCmd << QStringLiteral("-i")
                    << QDir::fromNativeSeparators(
                           QDir::cleanPath(jumpKeyPath));
            jumpCmd << QStringLiteral("-o")
                    << QStringLiteral("IdentitiesOnly=yes");
            jumpCmd << jumpHost;
            args << QStringLiteral("-o")
                 << QStringLiteral("ProxyCommand=%1").arg(shellJoinQuoted(jumpCmd));
        }
    } else if (useProxy) {
        QString proxyCommand;
        QString proxyErr;
        if (!buildOpenSshProxyCommand(opt, &proxyCommand, &proxyErr)) {
            if (errorOut) {
                *errorOut = proxyErr.isEmpty()
                                ? QCoreApplication::translate(
                                      "MainWindow",
                                      "Could not build proxy command for "
                                      "terminal mode.")
                                : proxyErr;
            }
            return false;
        }
        args << QStringLiteral("-o")
             << QStringLiteral("ProxyCommand=%1").arg(proxyCommand);
    }

    const QString target =
        QStringLiteral("%1@%2:%3")
            .arg(user, host, normalizeRemotePath(remotePath));
    args << target;
    *commandOut = shellJoinQuoted(args);
    return true;
}

static QString buildSshWithSftpFallbackCommand(const QString &sshCommand,
                                               const QString &sftpCommand) {
    if (sftpCommand.trimmed().isEmpty())
        return sshCommand;

    // OpenSSH returns 255 for transport/session errors (for example PTY denied).
    return QStringLiteral(
               "%1; _openscp_ssh_status=$?; "
               "if [ \"$_openscp_ssh_status\" -eq 255 ]; then "
               "printf '%s\\n' %2; "
               "%3; "
               "fi")
        .arg(sshCommand,
             shellSingleQuote(QCoreApplication::translate(
                 "MainWindow",
                 "OpenSCP: SSH shell was not available. Falling back to SFTP "
                 "CLI.")),
             sftpCommand);
}

static QString appleScriptStringLiteral(const QString &raw) {
    QString escaped = raw;
    escaped.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    escaped.replace(QStringLiteral("\""), QStringLiteral("\\\""));
    return QStringLiteral("\"") + escaped + QStringLiteral("\"");
}

static bool launchShellCommandInSystemTerminal(const QString &shellCommand,
                                               QString *errorOut) {
    if (errorOut)
        errorOut->clear();

#ifdef Q_OS_MAC
    const QString osaExe =
        QStandardPaths::findExecutable(QStringLiteral("osascript"));
    if (osaExe.isEmpty()) {
        if (errorOut) {
            *errorOut = QCoreApplication::translate(
                "MainWindow", "Could not locate osascript.");
        }
        return false;
    }
    const QString line1 = QStringLiteral("tell application \"Terminal\" to activate");
    const QString line2 =
        QStringLiteral("tell application \"Terminal\" to do script %1")
            .arg(appleScriptStringLiteral(shellCommand));
    if (!QProcess::startDetached(osaExe,
                                 {QStringLiteral("-e"), line1,
                                  QStringLiteral("-e"), line2})) {
        if (errorOut) {
            *errorOut = QCoreApplication::translate(
                "MainWindow", "Could not launch Terminal.app.");
        }
        return false;
    }
    return true;
#elif defined(Q_OS_LINUX)
    auto tryLaunch = [&](const QString &program,
                         const QStringList &args) -> bool {
        const QString exe = QStandardPaths::findExecutable(program);
        return !exe.isEmpty() && QProcess::startDetached(exe, args);
    };
    if (tryLaunch(QStringLiteral("x-terminal-emulator"),
                  {QStringLiteral("-e"), QStringLiteral("sh"),
                   QStringLiteral("-lc"), shellCommand})) {
        return true;
    }
    if (tryLaunch(QStringLiteral("gnome-terminal"),
                  {QStringLiteral("--"), QStringLiteral("sh"),
                   QStringLiteral("-lc"), shellCommand})) {
        return true;
    }
    if (tryLaunch(QStringLiteral("konsole"),
                  {QStringLiteral("-e"), QStringLiteral("sh"),
                   QStringLiteral("-lc"), shellCommand})) {
        return true;
    }
    if (tryLaunch(QStringLiteral("xfce4-terminal"),
                  {QStringLiteral("--command"),
                   QStringLiteral("sh -lc %1").arg(shellSingleQuote(shellCommand))})) {
        return true;
    }
    if (tryLaunch(QStringLiteral("xterm"),
                  {QStringLiteral("-e"), QStringLiteral("sh"),
                   QStringLiteral("-lc"), shellCommand})) {
        return true;
    }
    if (tryLaunch(QStringLiteral("alacritty"),
                  {QStringLiteral("-e"), QStringLiteral("sh"),
                   QStringLiteral("-lc"), shellCommand})) {
        return true;
    }
    if (tryLaunch(QStringLiteral("kitty"),
                  {QStringLiteral("sh"), QStringLiteral("-lc"),
                   shellCommand})) {
        return true;
    }

    if (errorOut) {
        *errorOut = QCoreApplication::translate(
            "MainWindow",
            "No compatible terminal emulator was found.");
    }
    return false;
#else
    if (errorOut) {
        *errorOut = QCoreApplication::translate(
            "MainWindow",
            "Open in terminal action is not supported on this platform.");
    }
    return false;
#endif
}

void MainWindow::goUpRight() {
    if (rightIsRemote_) {
        QString cur = rightRemoteModel_ ? rightRemoteModel_->rootPath()
                                        : (rightPath_ ? rightPath_->text()
                                                      : QString());
        cur = normalizeRemotePath(cur);
        if (cur == "/" || cur.isEmpty())
            return;
        if (cur.endsWith('/'))
            cur.chop(1);
        int slash = cur.lastIndexOf('/');
        QString parent = (slash <= 0) ? "/" : cur.left(slash);
        setRightRemoteRoot(parent);
    } else {
        QString cur = rightPath_->text();
        QDir currentDir(cur);
        if (!currentDir.cdUp())
            return;
        setRightRoot(currentDir.absolutePath());
        updateDeleteShortcutEnables();
    }
}

void MainWindow::goHomeRight() {
    if (rightIsRemote_) {
        // SFTP does not provide a portable remote HOME query; use root fallback.
        setRightRemoteRoot(QStringLiteral("/"));
    } else {
        setRightRoot(preferredLocalHomePath());
        updateDeleteShortcutEnables();
    }
}

void MainWindow::openRightRemoteTerminal() {
    if (!rightIsRemote_ || !activeSessionOptions_.has_value()) {
        UiAlerts::information(this, tr("Open in terminal"),
                              tr("The right panel must be connected as remote."));
        return;
    }

    const openscp::SessionOptions &sessionOptions = *activeSessionOptions_;
    const QString remotePath = normalizeRemotePath(
        rightRemoteModel_ ? rightRemoteModel_->rootPath()
                          : (rightPath_ ? rightPath_->text() : QString()));
    QSettings settings("OpenSCP", "OpenSCP");
    const bool forceInteractiveLogin =
        settings.value("Terminal/forceInteractiveLogin", false).toBool();
    const bool enableSftpCliFallback =
        settings.value("Terminal/enableSftpCliFallback", true).toBool();

    QString command;
    QString prepareError;
    if (!buildRemoteTerminalSshCommand(sessionOptions, remotePath,
                                       forceInteractiveLogin,
                                       &command, &prepareError)) {
        UiAlerts::warning(
            this, tr("Open in terminal"),
            tr("Could not prepare the terminal command.\n%1")
                .arg(prepareError.isEmpty() ? tr("Unknown error.")
                                            : prepareError));
        return;
    }

    QString sftpFallbackCommand;
    bool hasSftpFallback = false;
    if (enableSftpCliFallback) {
        hasSftpFallback = buildRemoteSftpCliCommand(
            sessionOptions, remotePath, forceInteractiveLogin,
            &sftpFallbackCommand, nullptr);
        if (!hasSftpFallback)
            sftpFallbackCommand.clear();
    }

    const QString launchCommand = hasSftpFallback
                                      ? buildSshWithSftpFallbackCommand(
                                            command, sftpFallbackCommand)
                                      : command;

    QString launchError;
    if (!launchShellCommandInSystemTerminal(launchCommand, &launchError)) {
        UiAlerts::warning(
            this, tr("Open in terminal"),
            tr("Could not open a remote terminal.\n%1")
                .arg(launchError.isEmpty() ? tr("Unknown error.")
                                           : launchError));
        return;
    }

    const bool hasSavedPassword = sessionOptions.password.has_value() &&
                                  !sessionOptions.password->empty() &&
                                  trimOptionalString(
                                      sessionOptions.private_key_path)
                                      .isEmpty();
    QString statusMessage = tr("Opening remote terminal at %1").arg(remotePath);
    if (forceInteractiveLogin) {
        statusMessage += tr(" (interactive login required)");
    } else if (hasSavedPassword) {
        statusMessage +=
            tr(" (password may be requested by OpenSSH for security)");
    }
    if (hasSftpFallback) {
        statusMessage += tr(" (auto-fallback to SFTP CLI enabled)");
    }
    statusBar()->showMessage(statusMessage, 6000);
}

void MainWindow::setRightRemoteRoot(const QString &path) {
    if (!rightIsRemote_)
        return;
    if (!rightRemoteModel_) {
        const QString normalized = normalizeRemotePath(path);
        rightPath_->setText(normalized);
        addRecentRemotePath(normalized);
        refreshRightBreadcrumbs();
        updateDeleteShortcutEnables();
        statusBar()->showMessage(tr("Remote path: %1").arg(normalized), 3000);
        return;
    }
    requestRemoteListing(path, false);
}

void MainWindow::refreshRightRemotePanel() {
    if (!rightIsRemote_)
        return;
    if (!rightRemoteModel_) {
        statusBar()->showMessage(
            tr("Refresh is not available in transfer-only mode."), 3000);
        return;
    }

    requestRemoteListing(rightRemoteModel_->rootPath(), true);
}

void MainWindow::requestRemoteListing(const QString &path, bool refresh,
                                      bool initialLoad) {
    if (!rightIsRemote_ || !rightRemoteModel_ || !remoteOps_ ||
        !remoteOps_->hasRequestedSession()) {
        statusBar()->showMessage(
            tr("Remote control connection is not available"), 4000);
        return;
    }

    const QString normalized = normalizeRemotePath(path);
    if (activeRemoteListJob_ != 0)
        remoteOps_->cancel(activeRemoteListJob_);

    remoteRefreshSelectionNames_.clear();
    remoteRefreshScrollValue_ = 0;
    if (refresh && rightView_->selectionModel()) {
        const QModelIndexList selected =
            rightView_->selectionModel()->selectedRows(NAME_COL);
        for (const QModelIndex &index : selected) {
            const QString name = rightRemoteModel_->nameAt(index);
            if (!name.isEmpty())
                remoteRefreshSelectionNames_.push_back(name);
        }
        if (rightView_->verticalScrollBar()) {
            remoteRefreshScrollValue_ =
                rightView_->verticalScrollBar()->value();
        }
    }

    requestedRemotePath_ = normalized;
    activeRemoteListIsRefresh_ = refresh;
    activeRemoteListIsInitial_ = initialLoad;
    if (initialLoad && normalized != QStringLiteral("/"))
        initialRemoteFallbackAttempted_ = false;
    if (!refresh)
        rightRemoteModel_->setLoading(normalized);
    rightPath_->setText(normalized);
    refreshRightBreadcrumbs();
    rightView_->setEnabled(false);
    statusBar()->showMessage(
        refresh ? tr("Refreshing remote folder…")
                : tr("Opening remote folder…"),
        0);

    RemoteOperationController::ListRequest request;
    request.path = normalized;
    request.includeHidden = prefShowHidden_;
    activeRemoteListJob_ = remoteOps_->submit(request);
}

void MainWindow::rightItemActivated(const QModelIndex &idx) {
    // Local mode (right panel is local): navigate into directories
    if (!rightIsRemote_) {
        if (!rightLocalModel_)
            return;
        const QFileInfo localFileInfo = rightLocalModel_->fileInfo(idx);
        if (localFileInfo.isDir()) {
            setRightRoot(localFileInfo.absoluteFilePath());
        } else if (localFileInfo.isFile()) {
            openLocalPathWithPreference(localFileInfo.absoluteFilePath());
        }
        return;
    }
    // Remote mode: navigate or download/open file
    if (!rightRemoteModel_)
        return;
    if (rightRemoteModel_->isDir(idx)) {
        const QString name = rightRemoteModel_->nameAt(idx);
        const QString next = joinRemotePath(rightRemoteModel_->rootPath(), name);
        setRightRemoteRoot(next);
        return;
    }
    const QString name = rightRemoteModel_->nameAt(idx);
    {
        QString why;
        if (!isValidEntryName(name, &why)) {
            UiAlerts::warning(this, tr("Invalid name"), why);
            return;
        }
    }
    const QString remotePath =
        joinRemotePath(rightRemoteModel_->rootPath(), name);
    const QString localPath = tempDownloadPathFor(name);
    // Avoid duplicates: if there is already an active download with same
    // src/dst, do not enqueue again
    bool alreadyActive = false;
    {
        const auto tasks = transferMgr_->tasksSnapshot();
        alreadyActive = hasActiveTransferTask(tasks,
                                              TransferTask::Type::Download,
                                              remotePath, localPath);
    }
    if (!alreadyActive) {
        // Enqueue download so it appears in the queue (instead of direct
        // download)
        transferMgr_->enqueueDownload(remotePath, localPath);
        statusBar()->showMessage(QString(tr("Queued: %1 downloads")).arg(1),
                                 3000);
        maybeShowTransferQueue();
    } else {
        // There was already an identical task in the queue; optionally show it
        maybeShowTransferQueue();
        statusBar()->showMessage(tr("Download already queued"), 2000);
    }
    // Open the file when the corresponding task finishes (avoid duplicate
    // listeners)
    static QSet<QString> sOpenListeners;
    const QString key = remotePath + "->" + localPath;
    if (!sOpenListeners.contains(key)) {
        sOpenListeners.insert(key);
        auto connPtr = std::make_shared<QMetaObject::Connection>();
        *connPtr = connect(
            transferMgr_, &TransferManager::tasksUpdated, this,
            [this, remotePath, localPath, key,
             connPtr](const QVector<quint64> &) {
                const auto tasks = transferMgr_->tasksSnapshot();
                for (const auto &task : tasks) {
                    if (task.type == TransferTask::Type::Download &&
                        task.src == remotePath && task.dst == localPath) {
                        if (task.status == TransferTask::Status::Done) {
                            openLocalPathWithPreference(localPath);
                            statusBar()->showMessage(
                                tr("Downloaded: ") + localPath, 5000);
                            QObject::disconnect(*connPtr);
                            sOpenListeners.remove(key);
                        } else if (isTransferTaskFinalStatus(task.status)) {
                            QObject::disconnect(*connPtr);
                            sOpenListeners.remove(key);
                        }
                        break;
                    }
                }
            });
    }
}

void MainWindow::downloadRightToLeft() {
    if (!rightIsRemote_) {
        UiAlerts::information(this, tr("Download"),
                                 tr("The right panel is not remote."));
        return;
    }
    if (!sftp_) {
        UiAlerts::warning(this, tr("Remote"), tr("No active remote session."));
        return;
    }
    const QString picked = QFileDialog::getExistingDirectory(
        this, tr("Select destination folder (local)"),
        downloadDir_.isEmpty() ? QDir::homePath() : downloadDir_);
    if (picked.isEmpty())
        return;
    downloadDir_ = picked;
    QDir dst(downloadDir_);
    if (!dst.exists()) {
        UiAlerts::warning(this, tr("Invalid destination"),
                             tr("Destination folder does not exist."));
        return;
    }
    if (isScpTransferMode()) {
        const QString baseRemote = normalizeRemotePath(
            rightPath_ ? rightPath_->text() : QStringLiteral("/"));
        bool inputAccepted = false;
        QString remoteInput = QInputDialog::getText(
            this, tr("Download"),
            tr("Remote file path (absolute or relative to %1):")
                .arg(baseRemote),
            QLineEdit::Normal, baseRemote, &inputAccepted);
        if (!inputAccepted)
            return;
        remoteInput = remoteInput.trimmed();
        if (remoteInput.isEmpty())
            return;
        QString remotePath = remoteInput;
        if (!remotePath.startsWith('/'))
            remotePath = joinRemotePath(baseRemote, remotePath);
        remotePath = normalizeRemotePath(remotePath);
        const QString name = QFileInfo(remotePath).fileName();
        if (name.isEmpty()) {
            UiAlerts::warning(this, tr("Download"),
                              tr("Enter a valid remote file path."));
            return;
        }
        transferMgr_->enqueueDownload(remotePath, dst.filePath(name));
        statusBar()->showMessage(QString(tr("Queued: %1 downloads")).arg(1),
                                 4000);
        maybeShowTransferQueue();
        const int slash = remotePath.lastIndexOf('/');
        const QString parent = (slash <= 0) ? QStringLiteral("/")
                                            : remotePath.left(slash);
        setRightRemoteRoot(parent);
        return;
    }
    if (!rightRemoteModel_) {
        UiAlerts::warning(this, tr("Remote"), tr("No active remote session."));
        return;
    }
    auto selectionModel = rightView_->selectionModel();
    QModelIndexList rows;
    if (selectionModel)
        rows = selectionModel->selectedRows(NAME_COL);
    if (rows.isEmpty()) {
        // Download everything visible (first level) if there is no selection
        int rowCount = rightRemoteModel_ ? rightRemoteModel_->rowCount() : 0;
        for (int rowIndex = 0; rowIndex < rowCount; ++rowIndex)
            rows << rightRemoteModel_->index(rowIndex, NAME_COL);
        if (rows.isEmpty()) {
            UiAlerts::information(this, tr("Download"),
                                     tr("Nothing to download."));
            return;
        }
    }
    int skippedInvalidCount = 0;
    QVector<RemoteDownloadSeed> seeds;
    seeds.reserve(rows.size());
    const QString remoteBase = rightRemoteModel_->rootPath();
    for (const QModelIndex &idx : rows) {
        const QString name = rightRemoteModel_->nameAt(idx);
        {
            QString why;
            if (!isValidEntryName(name, &why)) {
                ++skippedInvalidCount;
                continue;
            }
        }
        const QString remotePath = joinRemotePath(remoteBase, name);
        const QString localPath = dst.filePath(name);
        seeds.push_back(
            {remotePath, localPath, rightRemoteModel_->isDir(idx)});
    }
    runRemoteDownloadPrescan(seeds, skippedInvalidCount, false);
}

// Copy the selection from the right panel to the left.
// - Remote -> enqueue downloads (non-blocking).
// - Local  -> local-to-local copy (with overwrite policy).
void MainWindow::copyRightToLeft() {
    QDir dst(leftPath_->text());
    if (!dst.exists()) {
        UiAlerts::warning(
            this, tr("Invalid destination"),
            tr("The destination folder (left panel) does not exist."));
        return;
    }
    auto selectionModel = rightView_->selectionModel();
    if (!selectionModel) {
        UiAlerts::warning(this, tr("Copy"), tr("No selection."));
        return;
    }
    const auto rows = selectionModel->selectedRows(NAME_COL);
    if (rows.isEmpty()) {
        UiAlerts::information(this, tr("Copy"), tr("Nothing selected."));
        return;
    }

    if (!rightIsRemote_) {
        // Local -> Local copy (right to left)
        QVector<QFileInfo> sources;
        sources.reserve(rows.size());
        for (const QModelIndex &idx : rows)
            sources.push_back(rightLocalModel_->fileInfo(idx));

        int skipped = 0;
        const QVector<QPair<QString, QString>> selectedPairs =
            buildLocalDestinationPairsWithOverwritePrompt(this, sources, dst,
                                                          &skipped);
        const QVector<LocalFsPair> pairs = toLocalFsPairs(selectedPairs);
        runLocalFsOperation(pairs, false, skipped);
        return;
    }

    // Remote -> Local: enqueue downloads
    if (!sftp_ || !rightRemoteModel_) {
        UiAlerts::warning(this, tr("Remote"), tr("No active remote session."));
        return;
    }
    int skippedInvalidCount = 0;
    QVector<RemoteDownloadSeed> seeds;
    seeds.reserve(rows.size());
    const QString remoteBase = rightRemoteModel_->rootPath();
    for (const QModelIndex &idx : rows) {
        const QString name = rightRemoteModel_->nameAt(idx);
        {
            QString why;
            if (!isValidEntryName(name, &why)) {
                ++skippedInvalidCount;
                continue;
            }
        }
        const QString remotePath = joinRemotePath(remoteBase, name);
        const QString localPath = dst.filePath(name);
        seeds.push_back({remotePath, localPath, rightRemoteModel_->isDir(idx)});
    }
    runRemoteDownloadPrescan(seeds, skippedInvalidCount, false);
}

// Move the selection from the right panel to the left.
// - Remote -> download with progress and delete remotely on success.
// - Local  -> local copy and delete the source.
void MainWindow::moveRightToLeft() {
    auto selectionModel = rightView_->selectionModel();
    if (!selectionModel || selectionModel->selectedRows(NAME_COL).isEmpty()) {
        UiAlerts::information(this, tr("Move"), tr("Nothing selected."));
        return;
    }
    QDir dst(leftPath_->text());
    if (!dst.exists()) {
        UiAlerts::warning(
            this, tr("Invalid destination"),
            tr("The destination folder (left panel) does not exist."));
        return;
    }

    if (!rightIsRemote_) {
        // Local -> Local: move (copy then delete)
        const auto rows = selectionModel->selectedRows(NAME_COL);
        QVector<QFileInfo> sources;
        sources.reserve(rows.size());
        for (const QModelIndex &idx : rows)
            sources.push_back(rightLocalModel_->fileInfo(idx));

        int skipped = 0;
        const QVector<QPair<QString, QString>> selectedPairs =
            buildLocalDestinationPairsWithOverwritePrompt(this, sources, dst,
                                                          &skipped);
        const QVector<LocalFsPair> pairs = toLocalFsPairs(selectedPairs);
        runLocalFsOperation(pairs, true, skipped);
        return;
    }

    // Remote -> Local: discover on the serialized control worker, then enqueue
    // real move tasks. Source deletion is a persisted transfer phase.
    if (!remoteOps_ || !remoteOps_->hasRequestedSession() ||
        !rightRemoteModel_ || !transferMgr_) {
        UiAlerts::warning(this, tr("Remote"), tr("No active remote session."));
        return;
    }
    const auto rows = selectionModel->selectedRows(NAME_COL);
    const QString remoteBase = rightRemoteModel_->rootPath();
    TransferBatchOptions batchOptions;
    batchOptions.sessionKey = transferMgr_->sessionIdentity();
    batchOptions.operation = TransferOperation::Move;
    batchOptions.conflictPolicy = TransferConflictPolicy::Ask;
    batchOptions.batchId = transferMgr_->createBatch(batchOptions);

    struct MovePreparationState {
        QHash<RemoteOperationController::JobId, QString> localRoots;
        QSet<RemoteOperationController::JobId> pending;
        TransferBatchOptions batchOptions;
        QString remoteBase;
        QStringList topRemoteDirectories;
        QSet<RemoteOperationController::JobId> cleanupPending;
        int enqueuedFiles = 0;
        int enqueuedDirectories = 0;
        int skippedInvalid = 0;
        quint64 scanFailures = 0;
        bool canceled = false;
        bool scanFinished = false;
        bool cleanupStarted = false;
        QString lastError;
        QPointer<QProgressDialog> progress;
        QMetaObject::Connection batchConnection;
        QMetaObject::Connection completionConnection;
        QMetaObject::Connection progressConnection;
        QMetaObject::Connection transferConnection;
        QMetaObject::Connection cleanupConnection;
    };
    auto state = std::make_shared<MovePreparationState>();
    state->batchOptions = batchOptions;
    state->remoteBase = remoteBase;

    auto maybeStartCleanup = std::make_shared<std::function<void()>>();
    *maybeStartCleanup = [this, state] {
        if (!state->scanFinished || state->cleanupStarted ||
            state->canceled || !transferMgr_ || !remoteOps_) {
            if (state->scanFinished && state->canceled)
                QObject::disconnect(state->transferConnection);
            return;
        }
        const auto tasks = transferMgr_->tasksSnapshot();
        bool foundBatchTask = false;
        for (const auto &task : tasks) {
            if (task.batchId != state->batchOptions.batchId)
                continue;
            foundBatchTask = true;
            if (!isTransferTaskFinalStatus(task.status))
                return;
        }
        if (!foundBatchTask)
            return;

        state->cleanupStarted = true;
        QObject::disconnect(state->transferConnection);
        if (state->topRemoteDirectories.isEmpty()) {
            if (rightIsRemote_ && rightRemoteModel_)
                requestRemoteListing(state->remoteBase, true);
            return;
        }

        state->cleanupConnection = connect(
            remoteOps_, &RemoteOperationController::mutationCompleted, this,
            [this, state](
                const RemoteOperationController::MutationResult &result) {
                if (!state->cleanupPending.remove(result.result.job.id))
                    return;
                if (!state->cleanupPending.isEmpty())
                    return;
                QObject::disconnect(state->cleanupConnection);
                if (rightIsRemote_ && rightRemoteModel_)
                    requestRemoteListing(state->remoteBase, true);
            });
        for (const QString &remoteDirectory :
             state->topRemoteDirectories) {
            RemoteOperationController::DeleteRequest request;
            request.path = remoteDirectory;
            request.kind =
                RemoteOperationController::DeleteKind::Directory;
            request.recursive = true;
            request.traversal.includeHidden = true;
            request.traversal.skipSymlinks = true;
            request.traversal.maxDepth = 32;
            request.emptyDirectoriesOnly = true;
            const auto jobId = remoteOps_->submit(request);
            if (jobId != 0)
                state->cleanupPending.insert(jobId);
        }
        if (state->cleanupPending.isEmpty()) {
            QObject::disconnect(state->cleanupConnection);
            if (rightIsRemote_ && rightRemoteModel_)
                requestRemoteListing(state->remoteBase, true);
        }
    };
    state->transferConnection = connect(
        transferMgr_, &TransferManager::tasksUpdated, this,
        [maybeStartCleanup](const QVector<quint64> &) {
            (*maybeStartCleanup)();
        });

    state->progress = new QProgressDialog(
        tr("Preparing remote move…"), tr("Cancel"), 0, 0, this);
    state->progress->setWindowTitle(tr("Preparing queue"));
    state->progress->setWindowModality(Qt::NonModal);
    state->progress->setMinimumDuration(0);
    state->progress->setAutoClose(false);

    state->batchConnection = connect(
        remoteOps_, &RemoteOperationController::entriesBatchReady, this,
        [this, state](const RemoteOperationController::EntryBatch &batch) {
            if (!state->pending.contains(batch.job.id) || !transferMgr_)
                return;
            const QString localRoot = state->localRoots.value(batch.job.id);
            for (const auto &entry : batch.entries) {
                bool valid = !entry.relativePath.isEmpty();
                const QStringList parts =
                    entry.relativePath.split(QLatin1Char('/'),
                                             Qt::SkipEmptyParts);
                for (const QString &part : parts) {
                    QString why;
                    if (!isValidEntryName(part, &why)) {
                        valid = false;
                        break;
                    }
                }
                if (!valid) {
                    ++state->skippedInvalid;
                    continue;
                }
                const QString localPath =
                    QDir(localRoot).filePath(entry.relativePath);
                if (entry.info.is_dir) {
                    transferMgr_->enqueueLocalDirectory(
                        localPath, state->batchOptions);
                    ++state->enqueuedDirectories;
                    continue;
                }
                transferMgr_->enqueueDownload(
                    entry.path, localPath, state->batchOptions);
                ++state->enqueuedFiles;
            }
        });
    state->progressConnection = connect(
        remoteOps_, &RemoteOperationController::jobProgress, this,
        [state](const RemoteOperationController::Progress &progress) {
            if (!state->pending.contains(progress.job.id) ||
                !state->progress) {
                return;
            }
            state->progress->setLabelText(
                QCoreApplication::translate(
                    "MainWindow",
                    "Scanning %1\nFound: %2  |  Queued: %3")
                    .arg(progress.currentPath)
                    .arg(progress.visitedEntries)
                    .arg(state->enqueuedFiles));
        });
    state->completionConnection = connect(
        remoteOps_, &RemoteOperationController::jobFinished, this,
        [this, state, maybeStartCleanup](
            const RemoteOperationController::Completion &completion) {
            if (!state->pending.remove(completion.result.job.id))
                return;
            state->scanFailures += completion.failedEntries;
            if (completion.result.outcome ==
                RemoteOperationController::Outcome::Canceled) {
                state->canceled = true;
            } else if (completion.result.outcome !=
                       RemoteOperationController::Outcome::Succeeded) {
                ++state->scanFailures;
            }
            if (!completion.result.error.isEmpty())
                state->lastError = completion.result.error;
            if (!state->pending.isEmpty())
                return;
            state->scanFinished = true;

            QObject::disconnect(state->batchConnection);
            QObject::disconnect(state->completionConnection);
            QObject::disconnect(state->progressConnection);
            if (state->progress) {
                state->progress->hide();
                state->progress->deleteLater();
                state->progress.clear();
            }
            if (!rightIsRemote_)
                return;
            QString statusMessage =
                tr("Queued: %1 downloads (move)")
                    .arg(state->enqueuedFiles);
            if (state->enqueuedDirectories > 0) {
                statusMessage +=
                    QStringLiteral("  |  ") +
                    tr("Folders queued: %1")
                        .arg(state->enqueuedDirectories);
            }
            if (state->skippedInvalid > 0) {
                statusMessage +=
                    QStringLiteral("  |  ") +
                    tr("Skipped invalid: %1").arg(state->skippedInvalid);
            }
            if (state->scanFailures > 0) {
                statusMessage +=
                    QStringLiteral("  |  ") +
                    tr("Folders not listed: %1").arg(state->scanFailures);
            }
            if (state->canceled)
                statusMessage += QStringLiteral("  |  ") + tr("Canceled");
            statusBar()->showMessage(statusMessage, 6000);
            if (state->enqueuedFiles > 0 ||
                state->enqueuedDirectories > 0) {
                maybeShowTransferQueue();
            }
            (*maybeStartCleanup)();
        });
    connect(state->progress, &QProgressDialog::canceled, this,
            [this, state] {
                state->canceled = true;
                if (remoteOps_) {
                    const auto pending = state->pending;
                    for (const auto jobId : pending)
                        remoteOps_->cancel(jobId);
                }
                if (transferMgr_)
                    transferMgr_->cancelBatch(state->batchOptions.batchId);
            });

    for (const QModelIndex &index : rows) {
        const QString name = rightRemoteModel_->nameAt(index);
        QString why;
        if (!isValidEntryName(name, &why)) {
            ++state->skippedInvalid;
            continue;
        }
        const QString remotePath = joinRemotePath(remoteBase, name);
        const QString localPath = dst.filePath(name);
        if (!rightRemoteModel_->isDir(index)) {
            transferMgr_->enqueueDownload(remotePath, localPath,
                                          batchOptions);
            ++state->enqueuedFiles;
            continue;
        }

        transferMgr_->enqueueLocalDirectory(localPath, batchOptions);
        ++state->enqueuedDirectories;
        state->topRemoteDirectories.push_back(remotePath);
        RemoteOperationController::TraverseRequest request;
        request.rootPath = remotePath;
        request.includeDirectories = true;
        request.traversal.includeHidden = true;
        request.traversal.skipSymlinks = true;
        request.traversal.maxDepth = 32;
        request.traversal.batchSize = 250;
        const auto jobId = remoteOps_->submit(request);
        if (jobId != 0) {
            state->localRoots.insert(jobId, localPath);
            state->pending.insert(jobId);
        }
    }
    if (state->pending.isEmpty()) {
        state->scanFinished = true;
        QObject::disconnect(state->batchConnection);
        QObject::disconnect(state->completionConnection);
        QObject::disconnect(state->progressConnection);
        state->progress->deleteLater();
        statusBar()->showMessage(
            tr("Queued: %1 downloads (move)").arg(state->enqueuedFiles),
            4000);
        if (state->enqueuedFiles > 0)
            maybeShowTransferQueue();
        (*maybeStartCleanup)();
    } else {
        state->progress->show();
    }
}


void MainWindow::uploadViaDialog() {
    if (!rightIsRemote_ || !sftp_) {
        UiAlerts::information(
            this, tr("Upload"),
            tr("The right panel is not remote or there is no active session."));
        return;
    }
    const bool scpMode = isScpTransferMode();
    if (!scpMode && !rightRemoteModel_) {
        UiAlerts::warning(this, tr("Remote"), tr("No active remote session."));
        return;
    }
    const QString startDir =
        uploadDir_.isEmpty() ? QDir::homePath() : uploadDir_;
    if (scpMode) {
        const QStringList picks = QFileDialog::getOpenFileNames(
            this, tr("Select files to upload"), startDir);
        if (picks.isEmpty())
            return;
        uploadDir_ = QFileInfo(picks.first()).dir().absolutePath();
        const QString remoteBase = normalizeRemotePath(
            rightPath_ ? rightPath_->text() : QStringLiteral("/"));
        int enqueuedCount = 0;
        for (const QString &localPath : picks) {
            const QFileInfo localFileInfo(localPath);
            if (!localFileInfo.isFile())
                continue;
            transferMgr_->enqueueUpload(
                localFileInfo.absoluteFilePath(),
                joinRemotePath(remoteBase, localFileInfo.fileName()));
            ++enqueuedCount;
        }
        if (enqueuedCount > 0) {
            statusBar()->showMessage(
                QString(tr("Queued: %1 uploads")).arg(enqueuedCount), 4000);
            maybeShowTransferQueue();
        }
        return;
    }
    QFileDialog dlg(this, tr("Select files or folders to upload"), startDir);
    dlg.setFileMode(QFileDialog::ExistingFiles);
    dlg.setOption(QFileDialog::DontUseNativeDialog, true);
    dlg.setViewMode(QFileDialog::Detail);
    if (auto *listView = dlg.findChild<QListView *>("listView"))
        listView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    if (auto *treeView = dlg.findChild<QTreeView *>())
        treeView->setSelectionMode(QAbstractItemView::ExtendedSelection);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QStringList picks = dlg.selectedFiles();
    if (picks.isEmpty())
        return;
    uploadDir_ = QFileInfo(picks.first()).dir().absolutePath();
    const QString remoteBase = rightRemoteModel_->rootPath();
    QVector<QPair<QString, QString>> roots;
    roots.reserve(picks.size());
    for (const QString &pickedPath : picks) {
        const QFileInfo selectedPathInfo(pickedPath);
        if (!selectedPathInfo.isDir() && !selectedPathInfo.isFile())
            continue;
        roots.push_back(
            {selectedPathInfo.absoluteFilePath(),
             joinRemotePath(remoteBase,
                            selectedPathInfo.fileName())});
    }
    if (roots.isEmpty()) {
        statusBar()->showMessage(tr("Nothing to upload."), 4000);
        return;
    }
    startLocalUploadDiscovery(roots, false);
}

void MainWindow::newDirRight() {
    QString name;
    if (!promptValidEntryName(this, tr("New folder"), tr("Name:"), {}, name))
        return;
    if (rightIsRemote_) {
        if (!remoteOps_ || !remoteOps_->hasRequestedSession() ||
            !rightRemoteModel_)
            return;
        const QString remoteDirPath =
            joinRemotePath(rightRemoteModel_->rootPath(), name);
        const QString base = rightRemoteModel_->rootPath();
        auto jobId =
            std::make_shared<RemoteOperationController::JobId>(0);
        auto connection = std::make_shared<QMetaObject::Connection>();
        *connection = connect(
            remoteOps_, &RemoteOperationController::mutationCompleted, this,
            [this, jobId, connection, base](
                const RemoteOperationController::MutationResult &result) {
                if (result.result.job.id != *jobId)
                    return;
                QObject::disconnect(*connection);
                if (!rightIsRemote_)
                    return;
                if (result.result.outcome !=
                    RemoteOperationController::Outcome::Succeeded) {
                    invalidateRemoteWriteabilityFromError(result.result.error);
                    UiAlerts::critical(
                        this, tr("Remote"),
                        tr("Could not create the remote folder.\n%1")
                            .arg(shortRemoteError(result.result.error,
                                                  tr("Remote error"))));
                    return;
                }
                lastSuccessfulRemoteActivityAtMs_ =
                    QDateTime::currentMSecsSinceEpoch();
                requestRemoteListing(base, true);
                cacheCurrentRemoteWriteability(true);
            });
        RemoteOperationController::MkdirRequest request;
        request.path = remoteDirPath;
        request.mode = 0755;
        *jobId = remoteOps_->submit(request);
        statusBar()->showMessage(tr("Creating remote folder…"), 0);
    } else {
        QDir base(rightPath_->text());
        if (!base.mkpath(base.filePath(name))) {
            UiAlerts::critical(this, tr("Local"),
                                  tr("Could not create folder."));
            return;
        }
        setRightRoot(base.absolutePath());
    }
}

// Create a new empty file in the right pane (local only).
void MainWindow::newFileRight() {
    QString name;
    if (!promptValidEntryName(this, tr("New file"), tr("Name:"), {}, name))
        return;
    if (rightIsRemote_) {
        if (!remoteOps_ || !remoteOps_->hasRequestedSession() ||
            !rightRemoteModel_)
            return;
        const QString remotePath =
            joinRemotePath(rightRemoteModel_->rootPath(), name);
        const QString base = rightRemoteModel_->rootPath();
        auto submitCreate = [this, remotePath, base](bool overwrite) {
            auto createJob =
                std::make_shared<RemoteOperationController::JobId>(0);
            auto createConnection =
                std::make_shared<QMetaObject::Connection>();
            *createConnection = connect(
                remoteOps_,
                &RemoteOperationController::mutationCompleted, this,
                [this, createJob, createConnection, remotePath, base](
                    const RemoteOperationController::MutationResult &result) {
                    if (result.result.job.id != *createJob)
                        return;
                    QObject::disconnect(*createConnection);
                    if (!rightIsRemote_)
                        return;
                    if (result.result.outcome !=
                        RemoteOperationController::Outcome::Succeeded) {
                        invalidateRemoteWriteabilityFromError(
                            result.result.error);
                        UiAlerts::critical(
                            this, tr("Remote"),
                            tr("Could not create the remote file.\n%1")
                                .arg(shortRemoteError(result.result.error,
                                                      tr("Remote error"))));
                        return;
                    }
                    lastSuccessfulRemoteActivityAtMs_ =
                        QDateTime::currentMSecsSinceEpoch();
                    requestRemoteListing(base, true);
                    cacheCurrentRemoteWriteability(true);
                    statusBar()->showMessage(
                        tr("File created: ") + remotePath, 4000);
                });
            RemoteOperationController::CreateFileRequest createRequest;
            createRequest.path = remotePath;
            createRequest.overwrite = overwrite;
            *createJob = remoteOps_->submit(createRequest);
            statusBar()->showMessage(tr("Creating remote file…"), 0);
        };

        auto statJob =
            std::make_shared<RemoteOperationController::JobId>(0);
        auto statConnection = std::make_shared<QMetaObject::Connection>();
        *statConnection = connect(
            remoteOps_, &RemoteOperationController::statCompleted, this,
            [this, statJob, statConnection, submitCreate, name](
                const RemoteOperationController::StatResult &result) {
                if (result.result.job.id != *statJob)
                    return;
                QObject::disconnect(*statConnection);
                if (!rightIsRemote_)
                    return;
                if (result.result.outcome !=
                    RemoteOperationController::Outcome::Succeeded) {
                    UiAlerts::critical(
                        this, tr("Remote"),
                        tr("Could not check whether the remote file already "
                           "exists.\n%1")
                            .arg(shortRemoteError(result.result.error,
                                                  tr("Remote error"))));
                    return;
                }
                lastSuccessfulRemoteActivityAtMs_ =
                    QDateTime::currentMSecsSinceEpoch();
                if (result.found &&
                    UiAlerts::question(
                        this, tr("File exists"),
                        tr("«%1» already exists.\nOverwrite?").arg(name),
                        QMessageBox::Yes | QMessageBox::No) !=
                        QMessageBox::Yes) {
                    return;
                }
                submitCreate(result.found);
            });
        RemoteOperationController::StatRequest statRequest;
        statRequest.path = remotePath;
        *statJob = remoteOps_->submit(statRequest);
        statusBar()->showMessage(tr("Checking remote file…"), 0);
    } else {
        QDir base(rightPath_->text());
        const QString path = base.filePath(name);
        if (QFileInfo::exists(path)) {
            if (UiAlerts::question(
                    this, tr("File exists"),
                    tr("«%1» already exists.\nOverwrite?").arg(name),
                    QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
                return;
        }
        QFile newFile(path);
        if (!newFile.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            UiAlerts::critical(this, tr("Local"),
                                  tr("Could not create file."));
            return;
        }
        newFile.close();
        setRightRoot(base.absolutePath());
        statusBar()->showMessage(tr("File created: ") + path, 4000);
    }
}

// Rename the selected entry on the right pane (local or remote).
void MainWindow::renameRightSelected() {
    auto selectionModel = rightView_->selectionModel();
    if (!selectionModel)
        return;
    const auto rows = selectionModel->selectedRows();
    if (rows.size() != 1) {
        UiAlerts::information(this, tr("Rename"),
                                 tr("Select exactly one item."));
        return;
    }
    if (rightIsRemote_) {
        if (!remoteOps_ || !remoteOps_->hasRequestedSession() ||
            !rightRemoteModel_)
            return;
        const QModelIndex selectedIndex = rows.first();
        const QString oldName = rightRemoteModel_->nameAt(selectedIndex);
        bool inputAccepted = false;
        const QString newName =
            QInputDialog::getText(this, tr("Rename"), tr("New name:"),
                                  QLineEdit::Normal, oldName, &inputAccepted);
        if (!inputAccepted || newName.isEmpty() || newName == oldName)
            return;
        QString invalidReason;
        if (!isValidEntryName(newName, &invalidReason)) {
            UiAlerts::warning(this, tr("Invalid name"), invalidReason);
            return;
        }
        const QString base = rightRemoteModel_->rootPath();
        const QString sourcePath = joinRemotePath(base, oldName);
        const QString targetPath = joinRemotePath(base, newName);
        auto jobId =
            std::make_shared<RemoteOperationController::JobId>(0);
        auto connection = std::make_shared<QMetaObject::Connection>();
        *connection = connect(
            remoteOps_, &RemoteOperationController::mutationCompleted, this,
            [this, jobId, connection, base](
                const RemoteOperationController::MutationResult &result) {
                if (result.result.job.id != *jobId)
                    return;
                QObject::disconnect(*connection);
                if (!rightIsRemote_)
                    return;
                if (result.result.outcome !=
                    RemoteOperationController::Outcome::Succeeded) {
                    invalidateRemoteWriteabilityFromError(result.result.error);
                    UiAlerts::critical(
                        this, tr("Remote"),
                        tr("Could not rename the remote item.\n%1")
                            .arg(shortRemoteError(result.result.error,
                                                  tr("Remote error"))));
                    return;
                }
                lastSuccessfulRemoteActivityAtMs_ =
                    QDateTime::currentMSecsSinceEpoch();
                requestRemoteListing(base, true);
                cacheCurrentRemoteWriteability(true);
            });
        RemoteOperationController::RenameRequest request;
        request.from = sourcePath;
        request.to = targetPath;
        request.overwrite = false;
        *jobId = remoteOps_->submit(request);
        statusBar()->showMessage(tr("Renaming remote item…"), 0);
    } else {
        const QModelIndex selectedIndex = rows.first();
        const QFileInfo selectedFileInfo = rightLocalModel_->fileInfo(selectedIndex);
        bool inputAccepted = false;
        const QString newName =
            QInputDialog::getText(this, tr("Rename"), tr("New name:"),
                                  QLineEdit::Normal,
                                  selectedFileInfo.fileName(), &inputAccepted);
        if (!inputAccepted || newName.isEmpty() ||
            newName == selectedFileInfo.fileName())
            return;
        const QString newPath =
            QDir(selectedFileInfo.absolutePath()).filePath(newName);
        bool renamed = QFile::rename(selectedFileInfo.absoluteFilePath(), newPath);
        if (!renamed)
            renamed = QDir(selectedFileInfo.absolutePath())
                          .rename(selectedFileInfo.absoluteFilePath(), newPath);
        if (!renamed) {
            UiAlerts::critical(this, tr("Local"), tr("Could not rename."));
            return;
        }
        setRightRoot(rightPath_->text());
    }
}

void MainWindow::deleteRightSelected() {
    auto selectionModel = rightView_->selectionModel();
    if (!selectionModel)
        return;
    const auto rows = selectionModel->selectedRows();
    if (rows.isEmpty()) {
        UiAlerts::information(this, tr("Delete"), tr("Nothing selected."));
        return;
    }
    if (rightIsRemote_) {
        if (!remoteOps_ || !remoteOps_->hasRequestedSession() ||
            !rightRemoteModel_)
            return;
        if (UiAlerts::warning(this, tr("Confirm delete"),
                                 tr("This will permanently delete items on the "
                                    "remote server.\nContinue?"),
                                 QMessageBox::Yes | QMessageBox::No) !=
            QMessageBox::Yes)
            return;
        const QString base = rightRemoteModel_->rootPath();
        struct DeleteState {
            QSet<RemoteOperationController::JobId> pending;
            quint64 deletedCount = 0;
            quint64 failedCount = 0;
            int completedRequests = 0;
            bool canceled = false;
            QString lastError;
            QString base;
            QPointer<QProgressDialog> progress;
            QMetaObject::Connection mutationConnection;
            QMetaObject::Connection progressConnection;
        };
        auto state = std::make_shared<DeleteState>();
        state->base = base;
        state->progress = new QProgressDialog(
            tr("Deleting remote items…"), tr("Cancel"), 0, rows.size(), this);
        state->progress->setWindowTitle(tr("Delete"));
        state->progress->setWindowModality(Qt::NonModal);
        state->progress->setMinimumDuration(0);
        state->progress->setAutoClose(false);
        state->progress->show();

        state->mutationConnection = connect(
            remoteOps_, &RemoteOperationController::mutationCompleted, this,
            [this, state](
                const RemoteOperationController::MutationResult &result) {
                if (!state->pending.remove(result.result.job.id))
                    return;
                ++state->completedRequests;
                state->deletedCount += result.affectedEntries;
                state->failedCount += result.failedEntries;
                if (result.result.outcome !=
                    RemoteOperationController::Outcome::Succeeded) {
                    if (result.result.outcome ==
                        RemoteOperationController::Outcome::Canceled) {
                        state->canceled = true;
                    } else if (result.failedEntries == 0) {
                        ++state->failedCount;
                    }
                    if (!result.result.error.isEmpty())
                        state->lastError = result.result.error;
                }
                if (state->progress)
                    state->progress->setValue(state->completedRequests);
                if (!state->pending.isEmpty())
                    return;

                QObject::disconnect(state->mutationConnection);
                QObject::disconnect(state->progressConnection);
                if (state->progress) {
                    state->progress->hide();
                    state->progress->deleteLater();
                    state->progress.clear();
                }
                if (!rightIsRemote_)
                    return;
                QString statusMessage =
                    tr("Deleted OK: %1  |  Failed: %2")
                        .arg(state->deletedCount)
                        .arg(state->failedCount);
                if (state->canceled)
                    statusMessage += QStringLiteral("  |  ") + tr("Canceled");
                if (state->failedCount > 0 &&
                    !state->lastError.isEmpty()) {
                    statusMessage +=
                        QStringLiteral("\n") + tr("Last error: ") +
                        state->lastError;
                }
                statusBar()->showMessage(statusMessage, 6000);
                if (state->failedCount > 0)
                    invalidateRemoteWriteabilityFromError(state->lastError);
                if (state->failedCount == 0 &&
                    state->deletedCount > 0) {
                    cacheCurrentRemoteWriteability(true);
                    lastSuccessfulRemoteActivityAtMs_ =
                        QDateTime::currentMSecsSinceEpoch();
                }
                requestRemoteListing(state->base, true);
            });
        state->progressConnection = connect(
            remoteOps_, &RemoteOperationController::jobProgress, this,
            [state](const RemoteOperationController::Progress &progress) {
                if (!state->pending.contains(progress.job.id) ||
                    !state->progress) {
                    return;
                }
                state->progress->setLabelText(
                    QCoreApplication::translate(
                        "MainWindow",
                        "Deleting %1\nRemoved: %2  |  Failed: %3")
                        .arg(progress.currentPath)
                        .arg(progress.affectedEntries)
                        .arg(progress.failedEntries));
            });
        connect(state->progress, &QProgressDialog::canceled, this,
                [this, state] {
                    state->canceled = true;
                    if (!remoteOps_)
                        return;
                    const auto pending = state->pending;
                    for (const auto jobId : pending)
                        remoteOps_->cancel(jobId);
                });

        for (const QModelIndex &index : rows) {
            RemoteOperationController::DeleteRequest request;
            request.path =
                joinRemotePath(base, rightRemoteModel_->nameAt(index));
            request.kind =
                rightRemoteModel_->isDir(index)
                    ? RemoteOperationController::DeleteKind::Directory
                    : RemoteOperationController::DeleteKind::File;
            request.recursive =
                request.kind ==
                RemoteOperationController::DeleteKind::Directory;
            request.traversal.includeHidden = true;
            request.traversal.skipSymlinks = true;
            request.traversal.maxDepth = 32;
            request.traversal.batchSize = 250;
            const auto jobId = remoteOps_->submit(request);
            if (jobId != 0)
                state->pending.insert(jobId);
        }
        if (state->pending.isEmpty()) {
            QObject::disconnect(state->mutationConnection);
            QObject::disconnect(state->progressConnection);
            state->progress->deleteLater();
            UiAlerts::warning(this, tr("Remote"),
                              tr("Could not start remote deletion."));
        }
    } else {
        if (UiAlerts::warning(
                this, tr("Confirm delete"),
                tr("This will permanently delete on local disk.\nContinue?"),
                QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
            return;
        int deletedCount = 0;
        int failedCount = 0;
        for (const QModelIndex &idx : rows) {
            const QFileInfo selectedFileInfo = rightLocalModel_->fileInfo(idx);
            bool removed =
                selectedFileInfo.isDir()
                    ? QDir(selectedFileInfo.absoluteFilePath()).removeRecursively()
                    : QFile::remove(selectedFileInfo.absoluteFilePath());
            if (removed)
                ++deletedCount;
            else
                ++failedCount;
        }
        statusBar()->showMessage(
            QString(tr("Deleted: %1  |  Failed: %2"))
                .arg(deletedCount)
                .arg(failedCount),
            5000);
        setRightRoot(rightPath_->text());
    }
}

// Show context menu for the right pane based on current state.
void MainWindow::showRightContextMenu(const QPoint &pos) {
    if (!rightContextMenu_)
        rightContextMenu_ = new QMenu(this);

    // Selection state and ability to go up
    bool hasSel = false;
    if (auto selectionModel = rightView_->selectionModel()) {
        hasSel = !selectionModel->selectedRows(NAME_COL).isEmpty();
    }
    // Is there a parent directory?
    bool canGoUp = false;
    if (rightIsRemote_) {
        const QString cur = normalizeRemotePath(
            rightRemoteModel_ ? rightRemoteModel_->rootPath()
                              : (rightPath_ ? rightPath_->text() : QString()));
        canGoUp = (!cur.isEmpty() && cur != "/");
    } else {
        QDir currentDir(rightPath_ ? rightPath_->text() : QString());
        canGoUp = currentDir.cdUp();
    }

    if (isScpTransferMode()) {
        QVector<QAction *> entries;
        if (canGoUp)
            entries.push_back(actUpRight_);
        entries.push_back(actUploadRight_);
        entries.push_back(actDownloadF7_);
        entries.push_back(actOpenTerminalRight_);
        rebuildContextMenu(rightContextMenu_, entries);
        rightContextMenu_->popup(rightView_->viewport()->mapToGlobal(pos));
        return;
    }

    QVector<QAction *> entries;
    if (rightIsRemote_) {
        const bool supportsRemotePermissions =
            activeSessionOptions_.has_value() &&
            openscp::capabilitiesForProtocol(activeSessionOptions_->protocol)
                .can_set_permissions;
        // Up option (if applicable)
        if (canGoUp)
            entries.push_back(actUpRight_);

        // Always show "Download" on remote, regardless of selection
        entries.push_back(actDownloadF7_);

        if (!hasSel) {
            // No selection: creation and navigation
            if (rightRemoteWritable_) {
                entries.push_back(actNewFileRight_);
                entries.push_back(actNewDirRight_);
            }
        } else {
            // With selection on remote
            entries.push_back(actCopyRight_);
            if (rightRemoteWritable_) {
                entries.push_back(nullptr);
                entries.push_back(actUploadRight_);
                entries.push_back(actNewFileRight_);
                entries.push_back(actNewDirRight_);
                entries.push_back(actRenameRight_);
                entries.push_back(actDeleteRight_);
                entries.push_back(actMoveRight_);
                if (supportsRemotePermissions) {
                    entries.push_back(nullptr);
                    auto *changePerms = new QAction(tr("Change permissions…"),
                                                    rightContextMenu_);
                    connect(changePerms, &QAction::triggered, this,
                            &MainWindow::changeRemotePermissions);
                    entries.push_back(changePerms);
                }
            }
        }
    } else {
        // Local: Up option if applicable
        if (canGoUp)
            entries.push_back(actUpRight_);
        if (!hasSel) {
            // No selection: creation
            entries.push_back(actNewFileRight_);
            entries.push_back(actNewDirRight_);
        } else {
            // With selection: local operations + copy/move from left
            entries.push_back(actNewFileRight_);
            entries.push_back(actNewDirRight_);
            entries.push_back(actRenameRight_);
            entries.push_back(actDeleteRight_);
            entries.push_back(nullptr);
            // Copy/move the selection from the right panel to the left
            entries.push_back(actCopyRight_);
            entries.push_back(actMoveRight_);
        }
    }
    rebuildContextMenu(rightContextMenu_, entries);
    rightContextMenu_->popup(rightView_->viewport()->mapToGlobal(pos));
}

void MainWindow::changeRemotePermissions() {
    if (!rightIsRemote_ || !remoteOps_ ||
        !remoteOps_->hasRequestedSession() || !rightRemoteModel_)
        return;
    if (!activeSessionOptions_.has_value() ||
        !openscp::capabilitiesForProtocol(activeSessionOptions_->protocol)
             .can_set_permissions) {
        UiAlerts::information(
            this, tr("Permissions"),
            tr("Permissions are not supported for the active protocol."));
        return;
    }
    auto selectionModel = rightView_->selectionModel();
    if (!selectionModel)
        return;
    const auto rows = selectionModel->selectedRows();
    if (rows.size() != 1) {
        UiAlerts::information(this, tr("Permissions"),
                                 tr("Select only one item."));
        return;
    }
    const QModelIndex selectedIndex = rows.first();
    const QString name = rightRemoteModel_->nameAt(selectedIndex);
    const QString base = rightRemoteModel_->rootPath();
    const QString path = joinRemotePath(base, name);
    auto statJob = std::make_shared<RemoteOperationController::JobId>(0);
    auto statConnection = std::make_shared<QMetaObject::Connection>();
    *statConnection = connect(
        remoteOps_, &RemoteOperationController::statCompleted, this,
        [this, statJob, statConnection, path, base](
            const RemoteOperationController::StatResult &result) {
            if (result.result.job.id != *statJob)
                return;
            QObject::disconnect(*statConnection);
            if (!rightIsRemote_)
                return;
            if (result.result.outcome !=
                    RemoteOperationController::Outcome::Succeeded ||
                !result.found) {
                UiAlerts::warning(
                    this, tr("Permissions"),
                    tr("Could not read permissions.\n%1")
                        .arg(shortRemoteError(
                            result.result.error,
                            tr("Error reading remote information."))));
                return;
            }

            PermissionsDialog dialog(this);
            dialog.setMode(result.info.mode & 0777);
            if (dialog.exec() != QDialog::Accepted)
                return;

            const unsigned int newMode =
                (result.info.mode & ~0777u) | (dialog.mode() & 0777u);
            const bool recursive = dialog.recursive() && result.info.is_dir;
            auto progress = QPointer<QProgressDialog>();
            if (recursive) {
                progress = new QProgressDialog(
                    tr("Changing remote permissions…"), tr("Cancel"), 0, 0,
                    this);
                progress->setWindowTitle(tr("Permissions"));
                progress->setWindowModality(Qt::NonModal);
                progress->setMinimumDuration(0);
                progress->setAutoClose(false);
                progress->show();
            }

            auto chmodJob =
                std::make_shared<RemoteOperationController::JobId>(0);
            auto mutationConnection =
                std::make_shared<QMetaObject::Connection>();
            auto progressConnection =
                std::make_shared<QMetaObject::Connection>();
            *progressConnection = connect(
                remoteOps_, &RemoteOperationController::jobProgress, this,
                [chmodJob, progress](
                    const RemoteOperationController::Progress &update) {
                    if (update.job.id != *chmodJob || !progress)
                        return;
                    progress->setLabelText(
                        QCoreApplication::translate(
                            "MainWindow",
                            "Changing permissions for %1\nUpdated: %2  |  "
                            "Failed: %3")
                            .arg(update.currentPath)
                            .arg(update.affectedEntries)
                            .arg(update.failedEntries));
                });
            *mutationConnection = connect(
                remoteOps_,
                &RemoteOperationController::mutationCompleted, this,
                [this, chmodJob, mutationConnection, progressConnection,
                 progress, path, base](
                    const RemoteOperationController::MutationResult
                        &mutation) {
                    if (mutation.result.job.id != *chmodJob)
                        return;
                    QObject::disconnect(*mutationConnection);
                    QObject::disconnect(*progressConnection);
                    if (progress) {
                        progress->hide();
                        progress->deleteLater();
                    }
                    if (!rightIsRemote_)
                        return;
                    if (mutation.result.outcome !=
                        RemoteOperationController::Outcome::Succeeded) {
                        invalidateRemoteWriteabilityFromError(
                            mutation.result.error);
                        const QString item =
                            QFileInfo(path).fileName().isEmpty()
                                ? path
                                : QFileInfo(path).fileName();
                        UiAlerts::critical(
                            this, tr("Permissions"),
                            tr("Could not apply permissions to \"%1\".\n%2")
                                .arg(item,
                                     shortRemoteError(
                                         mutation.result.error,
                                         tr("Error applying changes."))));
                        return;
                    }
                    lastSuccessfulRemoteActivityAtMs_ =
                        QDateTime::currentMSecsSinceEpoch();
                    requestRemoteListing(base, true);
                    cacheCurrentRemoteWriteability(true);
                    statusBar()->showMessage(
                        tr("Permissions updated: %1  |  Failed: %2")
                            .arg(mutation.affectedEntries)
                            .arg(mutation.failedEntries),
                        4000);
                });

            RemoteOperationController::ChmodRequest request;
            request.path = path;
            request.mode = newMode;
            request.recursive = recursive;
            request.traversal.includeHidden = true;
            request.traversal.skipSymlinks = true;
            request.traversal.maxDepth = 32;
            request.traversal.batchSize = 250;
            *chmodJob = remoteOps_->submit(request);
            if (progress) {
                connect(progress, &QProgressDialog::canceled, this,
                        [this, chmodJob] {
                            if (remoteOps_ && *chmodJob != 0)
                                remoteOps_->cancel(*chmodJob);
                        });
            }
        });
    RemoteOperationController::StatRequest request;
    request.path = path;
    *statJob = remoteOps_->submit(request);
    statusBar()->showMessage(tr("Reading remote permissions…"), 0);
}

void MainWindow::applyRemoteWriteabilityActions() {
    const openscp::ProtocolCapabilities caps =
        sftp_ ? sftp_->capabilities() : openscp::ProtocolCapabilities{};
    if (actUploadRight_)
        actUploadRight_->setEnabled(rightRemoteWritable_ && caps.can_upload);
    if (actNewDirRight_)
        actNewDirRight_->setEnabled(rightRemoteWritable_ && caps.can_mkdir);
    if (actNewFileRight_)
        actNewFileRight_->setEnabled(rightRemoteWritable_ && caps.can_upload);
    if (actRenameRight_)
        actRenameRight_->setEnabled(rightRemoteWritable_ && caps.can_rename);
    if (actDeleteRight_)
        actDeleteRight_->setEnabled(rightRemoteWritable_ && caps.can_delete);
    if (actMoveRight_)
        actMoveRight_->setEnabled(rightRemoteWritable_ && caps.can_download &&
                                  caps.can_delete);
    if (actMoveRightTb_)
        actMoveRightTb_->setEnabled(rightRemoteWritable_ &&
                                    caps.can_download && caps.can_delete);
    updateDeleteShortcutEnables();
}

void MainWindow::cacheCurrentRemoteWriteability(bool writable) {
    if (!rightIsRemote_ || !rightRemoteModel_) {
        rightRemoteWritable_ = false;
        remoteWriteabilityCache_.clear();
        applyRemoteWriteabilityActions();
        return;
    }
    const QString base = rightRemoteModel_->rootPath();
    rightRemoteWritable_ = writable;
    remoteWriteabilityCache_.insert(
        base, RemoteWriteabilityCacheEntry{writable,
                                           QDateTime::currentMSecsSinceEpoch()});
    if (remoteWriteabilityCache_.size() > 256)
        remoteWriteabilityCache_.clear();
    applyRemoteWriteabilityActions();
}

void MainWindow::invalidateRemoteWriteabilityFromError(
    const QString &rawError) {
    if (!rightIsRemote_ || !rightRemoteModel_)
        return;
    if (!indicatesRemoteWriteabilityDenied(rawError))
        return;
    // Permissions are path- and operation-specific. Keep capability-based
    // actions available and let the requested operation report its own denial.
    updateRemoteWriteability();
}

// Enable operations from protocol capabilities. Permissions are deliberately
// checked by the real operation; probing with a temporary directory mutates
// the server and can be both slow and misleading.
void MainWindow::updateRemoteWriteability() {
    if (!rightIsRemote_ || !sftp_ || !rightRemoteModel_) {
        ++remoteWriteabilityProbeSeq_;
        rightRemoteWritable_ = false;
        remoteWriteabilityCache_.clear();
        applyRemoteWriteabilityActions();
        return;
    }
    const openscp::ProtocolCapabilities caps = sftp_->capabilities();
    rightRemoteWritable_ =
        caps.can_upload || caps.can_mkdir || caps.can_delete || caps.can_rename;
    applyRemoteWriteabilityActions();
}
