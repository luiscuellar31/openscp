// MainWindow remote-side operations and writeability state.
#include "MainWindow.hpp"
#include "MainWindowSharedUtils.hpp"
#include "PermissionsDialog.hpp"
#include "RemoteWalker.hpp"
#include "RemoteModel.hpp"
#include "TransferManager.hpp"
#include "UiAlerts.hpp"
#include "openscp/ClientFactory.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
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
#include <QSet>
#include <QSettings>
#include <QStandardPaths>
#include <QStatusBar>
#include <QTemporaryFile>
#include <QTreeView>

#include <functional>
#include <limits>
#include <string>
#include <thread>
#include <vector>

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
        QDir d(cur);
        if (!d.cdUp())
            return;
        setRightRoot(d.absolutePath());
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
    if (!rightIsRemote_ || !m_activeSessionOptions_.has_value()) {
        UiAlerts::information(this, tr("Open in terminal"),
                              tr("The right panel must be connected as remote."));
        return;
    }

    const openscp::SessionOptions &opt = *m_activeSessionOptions_;
    const QString remotePath = normalizeRemotePath(
        rightRemoteModel_ ? rightRemoteModel_->rootPath()
                          : (rightPath_ ? rightPath_->text() : QString()));
    QSettings s("OpenSCP", "OpenSCP");
    const bool forceInteractiveLogin =
        s.value("Terminal/forceInteractiveLogin", false).toBool();
    const bool enableSftpCliFallback =
        s.value("Terminal/enableSftpCliFallback", true).toBool();

    QString command;
    QString prepareError;
    if (!buildRemoteTerminalSshCommand(opt, remotePath, forceInteractiveLogin,
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
        hasSftpFallback =
            buildRemoteSftpCliCommand(opt, remotePath, forceInteractiveLogin,
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

    const bool hasSavedPassword = opt.password.has_value() &&
                                  !opt.password->empty() &&
                                  trimOptionalString(opt.private_key_path)
                                      .isEmpty();
    QString statusMsg = tr("Opening remote terminal at %1").arg(remotePath);
    if (forceInteractiveLogin) {
        statusMsg += tr(" (interactive login required)");
    } else if (hasSavedPassword) {
        statusMsg += tr(" (password may be requested by OpenSSH for security)");
    }
    if (hasSftpFallback) {
        statusMsg += tr(" (auto-fallback to SFTP CLI enabled)");
    }
    statusBar()->showMessage(statusMsg, 6000);
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
    QString e;
    if (!rightRemoteModel_->setRootPath(path, &e)) {
        UiAlerts::warning(
            this, tr("Remote error"),
            tr("Could not open the remote folder.\n%1")
                .arg(shortRemoteError(e,
                                      tr("Failed to read remote contents."))));
        return;
    }
}

void MainWindow::refreshRightRemotePanel() {
    if (!rightIsRemote_)
        return;
    if (!rightRemoteModel_) {
        statusBar()->showMessage(
            tr("Refresh is not available in transfer-only mode."), 3000);
        return;
    }

    QString e;
    if (!rightRemoteModel_->setRootPath(rightRemoteModel_->rootPath(), &e,
                                        true)) {
        UiAlerts::warning(
            this, tr("Remote error"),
            tr("Could not refresh the remote folder.\n%1")
                .arg(shortRemoteError(e,
                                      tr("Failed to read remote contents."))));
        return;
    }
}

void MainWindow::rightItemActivated(const QModelIndex &idx) {
    // Local mode (right panel is local): navigate into directories
    if (!rightIsRemote_) {
        if (!rightLocalModel_)
            return;
        const QFileInfo fi = rightLocalModel_->fileInfo(idx);
        if (fi.isDir()) {
            setRightRoot(fi.absoluteFilePath());
        } else if (fi.isFile()) {
            openLocalPathWithPreference(fi.absoluteFilePath());
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
            transferMgr_, &TransferManager::tasksChanged, this,
            [this, remotePath, localPath, key, connPtr]() {
                const auto tasks = transferMgr_->tasksSnapshot();
                for (const auto &t : tasks) {
                    if (t.type == TransferTask::Type::Download &&
                        t.src == remotePath && t.dst == localPath) {
                        if (t.status == TransferTask::Status::Done) {
                            openLocalPathWithPreference(localPath);
                            statusBar()->showMessage(
                                tr("Downloaded: ") + localPath, 5000);
                            QObject::disconnect(*connPtr);
                            sOpenListeners.remove(key);
                        } else if (isTransferTaskFinalStatus(t.status)) {
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
        bool ok = false;
        QString remoteInput = QInputDialog::getText(
            this, tr("Download"),
            tr("Remote file path (absolute or relative to %1):")
                .arg(baseRemote),
            QLineEdit::Normal, baseRemote, &ok);
        if (!ok)
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
    auto sel = rightView_->selectionModel();
    QModelIndexList rows;
    if (sel)
        rows = sel->selectedRows(NAME_COL);
    if (rows.isEmpty()) {
        // Download everything visible (first level) if there is no selection
        int rc = rightRemoteModel_ ? rightRemoteModel_->rowCount() : 0;
        for (int r = 0; r < rc; ++r)
            rows << rightRemoteModel_->index(r, NAME_COL);
        if (rows.isEmpty()) {
            UiAlerts::information(this, tr("Download"),
                                     tr("Nothing to download."));
            return;
        }
    }
    int enq = 0;
    int bad = 0;
    const QString remoteBase = rightRemoteModel_->rootPath();
    for (const QModelIndex &idx : rows) {
        const QString name = rightRemoteModel_->nameAt(idx);
        {
            QString why;
            if (!isValidEntryName(name, &why)) {
                ++bad;
                continue;
            }
        }
        const QString rpath = joinRemotePath(remoteBase, name);
        const QString lpath = dst.filePath(name);
        if (rightRemoteModel_->isDir(idx)) {
            QDir().mkpath(lpath);
            RemoteWalker::Options walkOptions;
            walkOptions.includeHidden = true;
            walkOptions.skipSymlinks = false;
            walkOptions.sanitizeRelativePath = false;
            walkOptions.maxDepth = std::numeric_limits<int>::max();
            walkOptions.validateName = [](const QString &entryName, QString *why) {
                return isValidEntryName(entryName, why);
            };
            walkOptions.onDirectoryEnter = [lpath](const QString &remotePath,
                                                   const QString &relativePath,
                                                   int depth) {
                Q_UNUSED(remotePath);
                Q_UNUSED(depth);
                const QString localDir = relativePath.isEmpty()
                                             ? lpath
                                             : QDir(lpath).filePath(relativePath);
                QDir().mkpath(localDir);
            };
            RemoteWalker::Stats walkStats;
            (void)RemoteWalker::walk(
                sftp_.get(), rpath, walkOptions,
                [&](const RemoteWalker::Entry &entry) {
                    const QString childLocal =
                        QDir(lpath).filePath(entry.relativePath);
                    transferMgr_->enqueueDownload(entry.remotePath, childLocal);
                    ++enq;
                },
                &walkStats);
            bad += static_cast<int>(walkStats.skippedInvalidNameCount);
        } else {
            transferMgr_->enqueueDownload(rpath, lpath);
            ++enq;
        }
    }
    if (enq > 0) {
        QString msg = QString(tr("Queued: %1 downloads")).arg(enq);
        if (bad > 0)
            msg += QString("  |  ") + tr("Skipped invalid: %1").arg(bad);
        statusBar()->showMessage(msg, 4000);
        maybeShowTransferQueue();
    }
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
    auto sel = rightView_->selectionModel();
    if (!sel) {
        UiAlerts::warning(this, tr("Copy"), tr("No selection."));
        return;
    }
    const auto rows = sel->selectedRows(NAME_COL);
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
        QVector<LocalFsPair> pairs;
        pairs.reserve(selectedPairs.size());
        for (const auto &pair : selectedPairs)
            pairs.push_back({pair.first, pair.second});
        runLocalFsOperation(pairs, false, skipped);
        return;
    }

    // Remote -> Local: enqueue downloads
    if (!sftp_ || !rightRemoteModel_) {
        UiAlerts::warning(this, tr("Remote"), tr("No active remote session."));
        return;
    }
    int bad = 0;
    QVector<RemoteDownloadSeed> seeds;
    seeds.reserve(rows.size());
    const QString remoteBase = rightRemoteModel_->rootPath();
    for (const QModelIndex &idx : rows) {
        const QString name = rightRemoteModel_->nameAt(idx);
        {
            QString why;
            if (!isValidEntryName(name, &why)) {
                ++bad;
                continue;
            }
        }
        const QString rpath = joinRemotePath(remoteBase, name);
        const QString lpath = dst.filePath(name);
        seeds.push_back({rpath, lpath, rightRemoteModel_->isDir(idx)});
    }
    runRemoteDownloadPrescan(seeds, bad, false);
}

// Move the selection from the right panel to the left.
// - Remote -> download with progress and delete remotely on success.
// - Local  -> local copy and delete the source.
void MainWindow::moveRightToLeft() {
    auto sel = rightView_->selectionModel();
    if (!sel || sel->selectedRows(NAME_COL).isEmpty()) {
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
        const auto rows = sel->selectedRows(NAME_COL);
        QVector<QFileInfo> sources;
        sources.reserve(rows.size());
        for (const QModelIndex &idx : rows)
            sources.push_back(rightLocalModel_->fileInfo(idx));

        int skipped = 0;
        const QVector<QPair<QString, QString>> selectedPairs =
            buildLocalDestinationPairsWithOverwritePrompt(this, sources, dst,
                                                          &skipped);
        QVector<LocalFsPair> pairs;
        pairs.reserve(selectedPairs.size());
        for (const auto &pair : selectedPairs)
            pairs.push_back({pair.first, pair.second});
        runLocalFsOperation(pairs, true, skipped);
        return;
    }

    // Remote -> Local: enqueue downloads and delete remote on completion
    if (!sftp_ || !rightRemoteModel_) {
        UiAlerts::warning(this, tr("Remote"), tr("No active remote session."));
        return;
    }
    const auto rows = sel->selectedRows(NAME_COL);
    const QString remoteBase = rightRemoteModel_->rootPath();
    QVector<QPair<QString, QString>> pairs; // (remote, local) files to download
    int bad = 0;
    struct TopSel {
        QString rpath;
        bool isDir;
    };
    QVector<TopSel> top;
    int enq = 0;
    for (const QModelIndex &idx : rows) {
        const QString name = rightRemoteModel_->nameAt(idx);
        {
            QString why;
            if (!isValidEntryName(name, &why)) {
                ++bad;
                continue;
            }
        }
        const QString rpath = joinRemotePath(remoteBase, name);
        const QString lpath = dst.filePath(name);
        const bool isDir = rightRemoteModel_->isDir(idx);
        top.push_back({rpath, isDir});
        if (isDir) {
            QDir().mkpath(lpath);
            RemoteWalker::Options walkOptions;
            walkOptions.includeHidden = true;
            walkOptions.skipSymlinks = false;
            walkOptions.sanitizeRelativePath = false;
            walkOptions.maxDepth = std::numeric_limits<int>::max();
            walkOptions.validateName = [](const QString &entryName, QString *why) {
                return isValidEntryName(entryName, why);
            };
            walkOptions.onDirectoryEnter = [lpath](const QString &remotePath,
                                                   const QString &relativePath,
                                                   int depth) {
                Q_UNUSED(remotePath);
                Q_UNUSED(depth);
                const QString localDir = relativePath.isEmpty()
                                             ? lpath
                                             : QDir(lpath).filePath(relativePath);
                QDir().mkpath(localDir);
            };
            RemoteWalker::Stats walkStats;
            (void)RemoteWalker::walk(
                sftp_.get(), rpath, walkOptions,
                [&](const RemoteWalker::Entry &entry) {
                    const QString childLocal =
                        QDir(lpath).filePath(entry.relativePath);
                    QDir().mkpath(QFileInfo(childLocal).dir().absolutePath());
                    pairs.push_back({entry.remotePath, childLocal});
                },
                &walkStats);
            bad += static_cast<int>(walkStats.skippedInvalidNameCount);
        } else {
            QDir().mkpath(QFileInfo(lpath).dir().absolutePath());
            pairs.push_back({rpath, lpath});
        }
    }
    for (const auto &p : pairs) {
        transferMgr_->enqueueDownload(p.first, p.second);
        ++enq;
    }
    if (enq > 0) {
        QString msg = QString(tr("Queued: %1 downloads (move)")).arg(enq);
        if (bad > 0)
            msg += QString("  |  ") + tr("Skipped invalid: %1").arg(bad);
        statusBar()->showMessage(msg, 4000);
        maybeShowTransferQueue();
    }
    // Per-item deletion: as each download finishes OK, delete that remote file;
    // when a folder has no pending files left, delete the folder.
    if (enq > 0) {
        struct MoveState {
            QSet<QString> filesPending;   // remote files pending deletion
            QSet<QString> filesProcessed; // remote files already processed
                                          // (avoid duplicates)
            QHash<QString, QString>
                fileToTopDir; // remote file -> top dir rpath
            QHash<QString, int> remainingInTopDir; // top dir -> count of
                                                   // pending successful files
            QSet<QString> topDirs; // rpaths of top entries that are directories
            QSet<QString> deletedDirs; // top dirs already deleted
        };
        auto state = std::make_shared<MoveState>();
        // Initialize top dir mapping and counters
        for (const auto &tsel : top)
            if (tsel.isDir) {
                state->topDirs.insert(tsel.rpath);
                state->remainingInTopDir.insert(tsel.rpath, 0);
            }
        for (const auto &pr : pairs) {
            state->filesPending.insert(pr.first);
            // Locate containing top directory
            QString foundTop;
            for (const auto &tsel : top) {
                if (!tsel.isDir)
                    continue;
                const QString prefix =
                    tsel.rpath.endsWith('/') ? tsel.rpath : (tsel.rpath + '/');
                if (pr.first == tsel.rpath || pr.first.startsWith(prefix)) {
                    foundTop = tsel.rpath;
                    break;
                }
            }
            if (!foundTop.isEmpty()) {
                state->fileToTopDir.insert(pr.first, foundTop);
                state->remainingInTopDir[foundTop] =
                    state->remainingInTopDir.value(foundTop) + 1;
            }
        }
        // If there are directories with 0 files, try to delete them only if
        // empty
        for (auto it = state->remainingInTopDir.begin();
             it != state->remainingInTopDir.end(); ++it) {
            if (it.value() == 0) {
                std::vector<openscp::FileInfo> out;
                std::string lerr;
                if (sftp_ && sftp_->list(it.key().toStdString(), out, lerr) &&
                    out.empty()) {
                    std::string derr;
                    if (sftp_->removeDir(it.key().toStdString(), derr)) {
                        state->deletedDirs.insert(it.key());
                    }
                }
            }
        }
        auto connPtr = std::make_shared<QMetaObject::Connection>();
        *connPtr = connect(
            transferMgr_, &TransferManager::tasksChanged, this,
            [this, state, remoteBase, connPtr, pairs]() {
                const auto tasks = transferMgr_->tasksSnapshot();
                // 1) For each successfully completed task, delete the
                // corresponding remote file (once)
                for (const auto &t : tasks) {
                    if (t.type != TransferTask::Type::Download)
                        continue;
                    if (t.status != TransferTask::Status::Done)
                        continue;
                    const QString r = t.src;
                    if (!state->filesPending.contains(r))
                        continue; // does not belong to this move or already
                                  // deleted
                    if (state->filesProcessed.contains(r))
                        continue;
                    // Try to delete the remote file
                    std::string ferr;
                    bool okDel =
                        sftp_ && sftp_->removeFile(r.toStdString(), ferr);
                    state->filesProcessed.insert(r);
                    if (okDel) {
                        state->filesPending.remove(r);
                        // Decrement counter for the top directory it belongs to
                        const QString topDir = state->fileToTopDir.value(r);
                        if (!topDir.isEmpty()) {
                            int rem =
                                state->remainingInTopDir.value(topDir) - 1;
                            state->remainingInTopDir[topDir] = rem;
                            if (rem == 0 &&
                                !state->deletedDirs.contains(topDir)) {
                                // All files under this top dir were moved:
                                // delete folder only if empty
                                std::vector<openscp::FileInfo> out;
                                std::string lerr;
                                if (sftp_ &&
                                    sftp_->list(topDir.toStdString(), out,
                                                lerr) &&
                                    out.empty()) {
                                    std::string derr;
                                    if (sftp_->removeDir(topDir.toStdString(),
                                                         derr)) {
                                        state->deletedDirs.insert(topDir);
                                    }
                                }
                            }
                        }
                    } else {
                        // Not deleted: keep it out to avoid endless retries;
                        // could retry if desired
                        state->filesPending.remove(r);
                    }
                }

                // 2) Disconnect when all related tasks have reached a final
                // state
                const bool allFinal = areTransferPairsFinal(
                    tasks, TransferTask::Type::Download, pairs);
                if (allFinal) {
                    // Refrescar vista remota una vez al final
                    QString dummy;
                    if (rightRemoteModel_)
                        rightRemoteModel_->setRootPath(remoteBase, &dummy);
                    QObject::disconnect(*connPtr);
                }
            });
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
        int enq = 0;
        for (const QString &localPath : picks) {
            const QFileInfo fi(localPath);
            if (!fi.isFile())
                continue;
            transferMgr_->enqueueUpload(
                fi.absoluteFilePath(), joinRemotePath(remoteBase, fi.fileName()));
            ++enq;
        }
        if (enq > 0) {
            statusBar()->showMessage(QString(tr("Queued: %1 uploads")).arg(enq),
                                     4000);
            maybeShowTransferQueue();
        }
        return;
    }
    QFileDialog dlg(this, tr("Select files or folders to upload"), startDir);
    dlg.setFileMode(QFileDialog::ExistingFiles);
    dlg.setOption(QFileDialog::DontUseNativeDialog, true);
    dlg.setViewMode(QFileDialog::Detail);
    if (auto *lv = dlg.findChild<QListView *>("listView"))
        lv->setSelectionMode(QAbstractItemView::ExtendedSelection);
    if (auto *tv = dlg.findChild<QTreeView *>())
        tv->setSelectionMode(QAbstractItemView::ExtendedSelection);
    if (dlg.exec() != QDialog::Accepted)
        return;
    const QStringList picks = dlg.selectedFiles();
    if (picks.isEmpty())
        return;
    uploadDir_ = QFileInfo(picks.first()).dir().absolutePath();
    QStringList files;
    for (const QString &p : picks) {
        QFileInfo fi(p);
        if (fi.isDir()) {
            QDirIterator it(p, QDir::NoDotAndDotDot | QDir::AllEntries,
                            QDirIterator::Subdirectories);
            while (it.hasNext()) {
                it.next();
                if (it.fileInfo().isFile())
                    files << it.filePath();
            }
        } else if (fi.isFile()) {
            files << fi.absoluteFilePath();
        }
    }
    if (files.isEmpty()) {
        statusBar()->showMessage(tr("Nothing to upload."), 4000);
        return;
    }
    int enq = 0;
    const QString remoteBase = rightRemoteModel_->rootPath();
    for (const QString &localPath : files) {
        const QFileInfo fi(localPath);
        QString relBase = fi.path().startsWith(uploadDir_)
                              ? fi.path().mid(uploadDir_.size()).trimmed()
                              : QString();
        if (relBase.startsWith('/'))
            relBase.remove(0, 1);
        QString targetDir = relBase.isEmpty()
                                ? remoteBase
                                : joinRemotePath(remoteBase, relBase);
        if (!targetDir.isEmpty() && targetDir != remoteBase) {
            bool isDir = false;
            std::string se;
            bool ex = sftp_->exists(targetDir.toStdString(), isDir, se);
            if (!ex && se.empty()) {
                std::string me;
                sftp_->mkdir(targetDir.toStdString(), me, 0755);
            }
        }
        const QString rTarget = joinRemotePath(targetDir, fi.fileName());
        transferMgr_->enqueueUpload(localPath, rTarget);
        ++enq;
    }
    if (enq > 0) {
        statusBar()->showMessage(QString(tr("Queued: %1 uploads")).arg(enq),
                                 4000);
        maybeShowTransferQueue();
    }
}

void MainWindow::newDirRight() {
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("New folder"), tr("Name:"), QLineEdit::Normal, {}, &ok);
    if (!ok || name.isEmpty())
        return;
    QString why;
    if (!isValidEntryName(name, &why)) {
        UiAlerts::warning(this, tr("Invalid name"), why);
        return;
    }
    if (rightIsRemote_) {
        if (!sftp_ || !rightRemoteModel_)
            return;
        const QString path =
            joinRemotePath(rightRemoteModel_->rootPath(), name);
        std::string err;
        const bool okMkdir = executeCriticalRemoteOperation(
            tr("create a remote folder"),
            [path](openscp::SftpClient *client, std::string &opErr) {
                return client->mkdir(path.toStdString(), opErr, 0755);
            },
            err);
        if (!okMkdir) {
            invalidateRemoteWriteabilityFromError(QString::fromStdString(err));
            UiAlerts::critical(
                this, tr("Remote"),
                tr("Could not create the remote folder.\n%1")
                    .arg(shortRemoteError(err, tr("Remote error"))));
            return;
        }
        QString dummy;
        rightRemoteModel_->setRootPath(rightRemoteModel_->rootPath(), &dummy);
        cacheCurrentRemoteWriteability(true);
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
    bool ok = false;
    const QString name = QInputDialog::getText(
        this, tr("New file"), tr("Name:"), QLineEdit::Normal, {}, &ok);
    if (!ok || name.isEmpty())
        return;
    QString why;
    if (!isValidEntryName(name, &why)) {
        UiAlerts::warning(this, tr("Invalid name"), why);
        return;
    }
    if (rightIsRemote_) {
        if (!sftp_ || !rightRemoteModel_)
            return;
        const QString remotePath =
            joinRemotePath(rightRemoteModel_->rootPath(), name);
        bool isDir = false;
        bool exists = false;
        std::string e;
        const bool existsCheckOk = executeCriticalRemoteOperation(
            tr("check remote item existence"),
            [remotePath, &isDir,
             &exists](openscp::SftpClient *client, std::string &opErr) {
                exists = client->exists(remotePath.toStdString(), isDir, opErr);
                if (exists)
                    return true;
                return opErr.empty();
            },
            e);
        if (!existsCheckOk) {
            UiAlerts::critical(
                this, tr("Remote"),
                tr("Could not check whether the remote file already "
                   "exists.\n%1")
                    .arg(shortRemoteError(e, tr("Remote error"))));
            return;
        }
        if (exists) {
            if (UiAlerts::question(
                    this, tr("File exists"),
                    tr("«%1» already exists.\nOverwrite?").arg(name),
                    QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
                return;
        }

        QTemporaryFile tmp;
        if (!tmp.open()) {
            UiAlerts::critical(this, tr("Temporary"),
                                  tr("Could not create a temporary file."));
            return;
        }
        tmp.close();
        std::string err;
        const bool okPut = executeCriticalRemoteOperation(
            tr("create a remote file"),
            [tmpPath = tmp.fileName(),
             remotePath](openscp::SftpClient *client, std::string &opErr) {
                return client->put(tmpPath.toStdString(), remotePath.toStdString(),
                                   opErr);
            },
            err);
        if (!okPut) {
            invalidateRemoteWriteabilityFromError(QString::fromStdString(err));
            UiAlerts::critical(
                this, tr("Remote"),
                tr("Could not upload the temporary file to the server.\n%1")
                    .arg(shortRemoteError(err, tr("Remote error"))));
            return;
        }
        QString dummy;
        rightRemoteModel_->setRootPath(rightRemoteModel_->rootPath(), &dummy);
        cacheCurrentRemoteWriteability(true);
        statusBar()->showMessage(tr("File created: ") + remotePath, 4000);
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
        QFile f(path);
        if (!f.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
            UiAlerts::critical(this, tr("Local"),
                                  tr("Could not create file."));
            return;
        }
        f.close();
        setRightRoot(base.absolutePath());
        statusBar()->showMessage(tr("File created: ") + path, 4000);
    }
}

// Rename the selected entry on the right pane (local or remote).
void MainWindow::renameRightSelected() {
    auto sel = rightView_->selectionModel();
    if (!sel)
        return;
    const auto rows = sel->selectedRows();
    if (rows.size() != 1) {
        UiAlerts::information(this, tr("Rename"),
                                 tr("Select exactly one item."));
        return;
    }
    if (rightIsRemote_) {
        if (!sftp_ || !rightRemoteModel_)
            return;
        const QModelIndex idx = rows.first();
        const QString oldName = rightRemoteModel_->nameAt(idx);
        bool ok = false;
        const QString newName =
            QInputDialog::getText(this, tr("Rename"), tr("New name:"),
                                  QLineEdit::Normal, oldName, &ok);
        if (!ok || newName.isEmpty() || newName == oldName)
            return;
        const QString base = rightRemoteModel_->rootPath();
        const QString from = joinRemotePath(base, oldName);
        const QString to = joinRemotePath(base, newName);
        std::string err;
        const bool okRename = executeCriticalRemoteOperation(
            tr("rename a remote item"),
            [from, to](openscp::SftpClient *client, std::string &opErr) {
                return client->rename(from.toStdString(), to.toStdString(),
                                      opErr, false);
            },
            err);
        if (!okRename) {
            invalidateRemoteWriteabilityFromError(QString::fromStdString(err));
            UiAlerts::critical(
                this, tr("Remote"),
                tr("Could not rename the remote item.\n%1")
                    .arg(shortRemoteError(err, tr("Remote error"))));
            return;
        }
        QString dummy;
        rightRemoteModel_->setRootPath(base, &dummy);
        cacheCurrentRemoteWriteability(true);
    } else {
        const QModelIndex idx = rows.first();
        const QFileInfo fi = rightLocalModel_->fileInfo(idx);
        bool ok = false;
        const QString newName =
            QInputDialog::getText(this, tr("Rename"), tr("New name:"),
                                  QLineEdit::Normal, fi.fileName(), &ok);
        if (!ok || newName.isEmpty() || newName == fi.fileName())
            return;
        const QString newPath = QDir(fi.absolutePath()).filePath(newName);
        bool renamed = QFile::rename(fi.absoluteFilePath(), newPath);
        if (!renamed)
            renamed =
                QDir(fi.absolutePath()).rename(fi.absoluteFilePath(), newPath);
        if (!renamed) {
            UiAlerts::critical(this, tr("Local"), tr("Could not rename."));
            return;
        }
        setRightRoot(rightPath_->text());
    }
}

void MainWindow::deleteRightSelected() {
    auto sel = rightView_->selectionModel();
    if (!sel)
        return;
    const auto rows = sel->selectedRows();
    if (rows.isEmpty()) {
        UiAlerts::information(this, tr("Delete"), tr("Nothing selected."));
        return;
    }
    if (rightIsRemote_) {
        if (!sftp_ || !rightRemoteModel_)
            return;
        if (UiAlerts::warning(this, tr("Confirm delete"),
                                 tr("This will permanently delete items on the "
                                    "remote server.\nContinue?"),
                                 QMessageBox::Yes | QMessageBox::No) !=
            QMessageBox::Yes)
            return;
        int ok = 0, fail = 0;
        QString lastErr;
        const QString base = rightRemoteModel_->rootPath();
        QStringList names;
        names.reserve(rows.size());
        for (const QModelIndex &idx : rows)
            names.push_back(rightRemoteModel_->nameAt(idx));
        std::function<bool(const QString &)> delRec = [&](const QString &p) {
            // Determine if path is a directory or a file using stat/exists
            bool isDir = false;
            bool exists = false;
            std::string xerr;
            const bool existsOk = executeCriticalRemoteOperation(
                tr("delete remote items"),
                [p, &isDir,
                 &exists](openscp::SftpClient *client, std::string &opErr) {
                    exists = client->exists(p.toStdString(), isDir, opErr);
                    if (exists)
                        return true;
                    // Not found is not an error in delete flow.
                    return opErr.empty();
                },
                xerr);
            if (!existsOk) {
                lastErr = QString::fromStdString(xerr);
                return false;
            }
            if (!exists)
                return true;
            if (!isDir) {
                std::string ferr;
                const bool removed = executeCriticalRemoteOperation(
                    tr("delete remote items"),
                    [p](openscp::SftpClient *client, std::string &opErr) {
                        return client->removeFile(p.toStdString(), opErr);
                    },
                    ferr);
                if (!removed) {
                    lastErr = QString::fromStdString(ferr);
                    return false;
                }
                return true;
            }
            // Directory: list and remove children first (depth-first)
            std::vector<openscp::FileInfo> out;
            std::string lerr;
            const bool listed = executeCriticalRemoteOperation(
                tr("delete remote items"),
                [p, &out](openscp::SftpClient *client, std::string &opErr) {
                    out.clear();
                    return client->list(p.toStdString(), out, opErr);
                },
                lerr);
            if (!listed) {
                lastErr = QString::fromStdString(lerr);
                return false;
            }
            for (const auto &e : out) {
                const QString child =
                    joinRemotePath(p, QString::fromStdString(e.name));
                if (!delRec(child))
                    return false;
            }
            std::string derr;
            const bool removedDir = executeCriticalRemoteOperation(
                tr("delete remote items"),
                [p](openscp::SftpClient *client, std::string &opErr) {
                    return client->removeDir(p.toStdString(), opErr);
                },
                derr);
            if (!removedDir) {
                lastErr = QString::fromStdString(derr);
                return false;
            }
            return true;
        };
        for (const QString &name : names) {
            const QString path = joinRemotePath(base, name);
            if (delRec(path))
                ++ok;
            else
                ++fail;
        }
        QString msg =
            QString(tr("Deleted OK: %1  |  Failed: %2")).arg(ok).arg(fail);
        if (fail > 0 && !lastErr.isEmpty())
            msg += "\n" + tr("Last error: ") + lastErr;
        statusBar()->showMessage(msg, 6000);
        if (fail > 0)
            invalidateRemoteWriteabilityFromError(lastErr);
        if (fail == 0 && ok > 0)
            cacheCurrentRemoteWriteability(true);
        QString dummy;
        rightRemoteModel_->setRootPath(base, &dummy);
    } else {
        if (UiAlerts::warning(
                this, tr("Confirm delete"),
                tr("This will permanently delete on local disk.\nContinue?"),
                QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes)
            return;
        int ok = 0, fail = 0;
        for (const QModelIndex &idx : rows) {
            const QFileInfo fi = rightLocalModel_->fileInfo(idx);
            bool removed = fi.isDir()
                               ? QDir(fi.absoluteFilePath()).removeRecursively()
                               : QFile::remove(fi.absoluteFilePath());
            if (removed)
                ++ok;
            else
                ++fail;
        }
        statusBar()->showMessage(
            QString(tr("Deleted: %1  |  Failed: %2")).arg(ok).arg(fail), 5000);
        setRightRoot(rightPath_->text());
    }
}

// Show context menu for the right pane based on current state.
void MainWindow::showRightContextMenu(const QPoint &pos) {
    if (!rightContextMenu_)
        rightContextMenu_ = new QMenu(this);

    // Selection state and ability to go up
    bool hasSel = false;
    if (auto sel = rightView_->selectionModel()) {
        hasSel = !sel->selectedRows(NAME_COL).isEmpty();
    }
    // Is there a parent directory?
    bool canGoUp = false;
    if (rightIsRemote_) {
        const QString cur = normalizeRemotePath(
            rightRemoteModel_ ? rightRemoteModel_->rootPath()
                              : (rightPath_ ? rightPath_->text() : QString()));
        canGoUp = (!cur.isEmpty() && cur != "/");
    } else {
        QDir d(rightPath_ ? rightPath_->text() : QString());
        canGoUp = d.cdUp();
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
            m_activeSessionOptions_.has_value() &&
            openscp::capabilitiesForProtocol(m_activeSessionOptions_->protocol)
                .supports_permissions;
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
    if (!rightIsRemote_ || !sftp_ || !rightRemoteModel_)
        return;
    if (!m_activeSessionOptions_.has_value() ||
        !openscp::capabilitiesForProtocol(m_activeSessionOptions_->protocol)
             .supports_permissions) {
        UiAlerts::information(
            this, tr("Permissions"),
            tr("Permissions are not supported for the active protocol."));
        return;
    }
    auto sel = rightView_->selectionModel();
    if (!sel)
        return;
    const auto rows = sel->selectedRows();
    if (rows.size() != 1) {
        UiAlerts::information(this, tr("Permissions"),
                                 tr("Select only one item."));
        return;
    }
    const QModelIndex idx = rows.first();
    const QString name = rightRemoteModel_->nameAt(idx);
    const QString base = rightRemoteModel_->rootPath();
    const QString path = joinRemotePath(base, name);
    openscp::FileInfo st{};
    std::string err;
    const bool statOk = executeCriticalRemoteOperation(
        tr("read remote permissions"),
        [path, &st](openscp::SftpClient *client, std::string &opErr) {
            return client->stat(path.toStdString(), st, opErr);
        },
        err);
    if (!statOk) {
        UiAlerts::warning(
            this, tr("Permissions"),
            tr("Could not read permissions.\n%1")
                .arg(shortRemoteError(
                    err, tr("Error reading remote information."))));
        return;
    }
    PermissionsDialog dlg(this);
    dlg.setMode(st.mode & 0777);
    if (dlg.exec() != QDialog::Accepted)
        return;
    unsigned int newMode = (st.mode & ~0777u) | (dlg.mode() & 0777u);
    auto applyOne = [&](const QString &p) -> bool {
        std::string cerrs;
        const bool chmodOk = executeCriticalRemoteOperation(
            tr("change remote permissions"),
            [p, newMode](openscp::SftpClient *client, std::string &opErr) {
                return client->chmod(p.toStdString(), newMode, opErr);
            },
            cerrs);
        if (!chmodOk) {
            invalidateRemoteWriteabilityFromError(
                QString::fromStdString(cerrs));
            const QString item =
                QFileInfo(p).fileName().isEmpty() ? p : QFileInfo(p).fileName();
            UiAlerts::critical(
                this, tr("Permissions"),
                tr("Could not apply permissions to \"%1\".\n%2")
                    .arg(item, shortRemoteError(
                                   cerrs, tr("Error applying changes."))));
            return false;
        }
        return true;
    };
    bool ok = true;
    if (dlg.recursive() && st.is_dir) {
        QVector<QString> stack;
        stack.push_back(path);
        while (!stack.isEmpty() && ok) {
            const QString cur = stack.back();
            stack.pop_back();
            if (!applyOne(cur)) {
                ok = false;
                break;
            }
            std::vector<openscp::FileInfo> out;
            std::string lerr;
            const bool listed = executeCriticalRemoteOperation(
                tr("read remote permissions"),
                [cur, &out](openscp::SftpClient *client, std::string &opErr) {
                    out.clear();
                    return client->list(cur.toStdString(), out, opErr);
                },
                lerr);
            if (!listed)
                continue;
            for (const auto &e : out) {
                const QString child =
                    joinRemotePath(cur, QString::fromStdString(e.name));
                if (e.is_dir)
                    stack.push_back(child);
                else {
                    if (!applyOne(child)) {
                        ok = false;
                        break;
                    }
                }
            }
        }
    } else {
        ok = applyOne(path);
    }
    if (!ok)
        return;
    QString dummy;
    rightRemoteModel_->setRootPath(base, &dummy);
    cacheCurrentRemoteWriteability(true);
    statusBar()->showMessage(tr("Permissions updated"), 3000);
}

void MainWindow::applyRemoteWriteabilityActions() {
    if (actUploadRight_)
        actUploadRight_->setEnabled(rightRemoteWritable_);
    if (actNewDirRight_)
        actNewDirRight_->setEnabled(rightRemoteWritable_);
    if (actNewFileRight_)
        actNewFileRight_->setEnabled(rightRemoteWritable_);
    if (actRenameRight_)
        actRenameRight_->setEnabled(rightRemoteWritable_);
    if (actDeleteRight_)
        actDeleteRight_->setEnabled(rightRemoteWritable_);
    if (actMoveRight_)
        actMoveRight_->setEnabled(rightRemoteWritable_);
    if (actMoveRightTb_)
        actMoveRightTb_->setEnabled(rightRemoteWritable_);
    updateDeleteShortcutEnables();
}

void MainWindow::cacheCurrentRemoteWriteability(bool writable) {
    if (!rightIsRemote_ || !rightRemoteModel_) {
        rightRemoteWritable_ = false;
        m_remoteWriteabilityCache_.clear();
        applyRemoteWriteabilityActions();
        return;
    }
    const QString base = rightRemoteModel_->rootPath();
    rightRemoteWritable_ = writable;
    m_remoteWriteabilityCache_.insert(
        base, RemoteWriteabilityCacheEntry{writable,
                                           QDateTime::currentMSecsSinceEpoch()});
    if (m_remoteWriteabilityCache_.size() > 256)
        m_remoteWriteabilityCache_.clear();
    applyRemoteWriteabilityActions();
}

void MainWindow::invalidateRemoteWriteabilityFromError(
    const QString &rawError) {
    if (!rightIsRemote_ || !rightRemoteModel_)
        return;
    if (!indicatesRemoteWriteabilityDenied(rawError))
        return;
    cacheCurrentRemoteWriteability(false);
}

// Check if the current remote directory is writable and update enables.
void MainWindow::updateRemoteWriteability() {
    if (!rightIsRemote_ || !sftp_ || !rightRemoteModel_) {
        ++m_remoteWriteabilityProbeSeq_;
        rightRemoteWritable_ = false;
        m_remoteWriteabilityCache_.clear();
        applyRemoteWriteabilityActions();
        return;
    }

    const QString base = rightRemoteModel_->rootPath();
    const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
    auto it = m_remoteWriteabilityCache_.constFind(base);
    if (it != m_remoteWriteabilityCache_.cend()) {
        if ((nowMs - it->checkedAtMs) <= m_remoteWriteabilityTtlMs_) {
            rightRemoteWritable_ = it->writable;
            applyRemoteWriteabilityActions();
            return;
        }
    }

    if (!m_activeSessionOptions_.has_value()) {
        // Without session options we cannot probe off-thread safely.
        applyRemoteWriteabilityActions();
        return;
    }

    // Keep UI responsive: optimistic state while background probe runs.
    rightRemoteWritable_ =
        (it != m_remoteWriteabilityCache_.cend()) ? it->writable : true;
    applyRemoteWriteabilityActions();

    const quint64 reqId = ++m_remoteWriteabilityProbeSeq_;
    const openscp::SessionOptions opt = *m_activeSessionOptions_;
    QPointer<MainWindow> self(this);
    std::thread([self, reqId, base, opt]() mutable {
        bool probeFinished = false;
        bool writable = false;

        std::string connErr;
        auto probe = openscp::CreateConnectedClient(opt, connErr);
        if (probe) {
            probeFinished = true;
            const qint64 ts = QDateTime::currentMSecsSinceEpoch();
            const QString testName =
                ".openscp-write-test-" + QString::number(ts);
            const QString testPath = joinRemotePath(base, testName);
            std::string err;
            const bool created =
                probe->mkdir(testPath.toStdString(), err, 0755);
            if (created) {
                std::string derr;
                (void)probe->removeDir(testPath.toStdString(), derr);
                writable = true;
            } else {
                writable = false;
            }
            probe->disconnect();
        }

        QObject *app = QCoreApplication::instance();
        if (!app)
            return;
        QMetaObject::invokeMethod(
            app,
            [self, reqId, base, probeFinished, writable]() {
                if (!self || !probeFinished)
                    return;
                if (reqId != self->m_remoteWriteabilityProbeSeq_.load())
                    return;
                if (!self->rightIsRemote_ || !self->rightRemoteModel_)
                    return;
                if (self->rightRemoteModel_->rootPath() != base)
                    return;

                const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
                self->rightRemoteWritable_ = writable;
                self->m_remoteWriteabilityCache_.insert(
                    base, RemoteWriteabilityCacheEntry{writable, nowMs});
                if (self->m_remoteWriteabilityCache_.size() > 256)
                    self->m_remoteWriteabilityCache_.clear();
                self->applyRemoteWriteabilityActions();
            },
            Qt::QueuedConnection);
    }).detach();
}
