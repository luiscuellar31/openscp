#include "TerminalCommandBuilder.hpp"

#include "RemotePath.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>

#include <optional>
#include <string>

namespace openscpui {

namespace {

QString trimOptionalString(const std::optional<std::string> &value) {
    if (!value || value->empty())
        return {};
    return QString::fromStdString(*value).trimmed();
}

QString shellSingleQuote(const QString &value) {
    QString escaped = value;
    escaped.replace(QStringLiteral("'"), QStringLiteral("'\"'\"'"));
    return QStringLiteral("'") + escaped + QStringLiteral("'");
}

QString shellJoinQuoted(const QStringList &arguments) {
    QStringList quoted;
    quoted.reserve(arguments.size());
    for (const QString &argument : arguments)
        quoted.push_back(shellSingleQuote(argument));
    return quoted.join(QLatin1Char(' '));
}

QString defaultKnownHostsPath() {
    const QString home = QDir::homePath();
    if (home.isEmpty())
        return {};
    return QDir(home).filePath(QStringLiteral(".ssh/known_hosts"));
}

void appendHostKeyArguments(QStringList &arguments,
                            const openscp::SessionOptions &session) {
    if (session.known_hosts_policy == openscp::KnownHostsPolicy::Off) {
        arguments << QStringLiteral("-o")
                  << QStringLiteral("StrictHostKeyChecking=no")
                  << QStringLiteral("-o")
                  << QStringLiteral("UserKnownHostsFile=/dev/null");
        return;
    }

    const QString strictValue =
        session.known_hosts_policy == openscp::KnownHostsPolicy::AcceptNew
            ? QStringLiteral("accept-new")
            : QStringLiteral("yes");
    arguments << QStringLiteral("-o")
              << QStringLiteral("StrictHostKeyChecking=%1").arg(strictValue);

    QString knownHostsPath = trimOptionalString(session.known_hosts_path);
    if (knownHostsPath.isEmpty())
        knownHostsPath = defaultKnownHostsPath();
    if (!knownHostsPath.isEmpty()) {
        arguments << QStringLiteral("-o")
                  << QStringLiteral("UserKnownHostsFile=%1")
                         .arg(QDir::fromNativeSeparators(
                             QDir::cleanPath(knownHostsPath)));
    }
}

void appendAuthenticationArguments(QStringList &arguments,
                                   const openscp::SessionOptions &session,
                                   bool forceInteractiveLogin) {
    if (forceInteractiveLogin) {
        arguments
            << QStringLiteral("-o") << QStringLiteral("PubkeyAuthentication=no")
            << QStringLiteral("-o")
            << QStringLiteral(
                   "PreferredAuthentications=keyboard-interactive,password");
        return;
    }

    const QString privateKeyPath = trimOptionalString(session.private_key_path);
    if (!privateKeyPath.isEmpty()) {
        arguments << QStringLiteral("-i")
                  << QDir::fromNativeSeparators(QDir::cleanPath(privateKeyPath))
                  << QStringLiteral("-o")
                  << QStringLiteral("IdentitiesOnly=yes");
    }
}

bool buildProxyCommand(const openscp::SessionOptions &session,
                       const TerminalCommandBuilder::ExecutableLookup &lookup,
                       QString *command, QString *error) {
    command->clear();
    if (session.proxy_type == openscp::ProxyType::None) {
        *error = QCoreApplication::translate(
            "MainWindow", "Proxy command requested without proxy settings.");
        return false;
    }

    const QString proxyHost =
        QString::fromStdString(session.proxy_host).trimmed();
    if (proxyHost.isEmpty() || session.proxy_port == 0) {
        *error = QCoreApplication::translate(
            "MainWindow", "Proxy host/port is missing for terminal command.");
        return false;
    }

    const bool hasProxyCredentials =
        (session.proxy_username && !session.proxy_username->empty()) ||
        (session.proxy_password && !session.proxy_password->empty());
    if (hasProxyCredentials) {
        *error = QCoreApplication::translate(
            "MainWindow",
            "Open in terminal is unavailable for authenticated proxies "
            "because terminal command arguments could expose the proxy "
            "password.");
        return false;
    }

    const QString netcatExecutable = lookup(QStringLiteral("nc"));
    if (!netcatExecutable.isEmpty()) {
        QString proxyProtocol;
        if (session.proxy_type == openscp::ProxyType::Socks5)
            proxyProtocol = QStringLiteral("5");
        else if (session.proxy_type == openscp::ProxyType::HttpConnect)
            proxyProtocol = QStringLiteral("connect");
        if (proxyProtocol.isEmpty()) {
            *error = QCoreApplication::translate(
                "MainWindow", "Unsupported proxy type for terminal command.");
            return false;
        }

        *command = shellJoinQuoted(
            {netcatExecutable, QStringLiteral("-x"),
             QStringLiteral("%1:%2").arg(proxyHost).arg(session.proxy_port),
             QStringLiteral("-X"), proxyProtocol, QStringLiteral("%h"),
             QStringLiteral("%p")});
        return true;
    }

    const QString ncatExecutable = lookup(QStringLiteral("ncat"));
    if (!ncatExecutable.isEmpty()) {
        QString proxyProtocol;
        if (session.proxy_type == openscp::ProxyType::Socks5)
            proxyProtocol = QStringLiteral("socks5");
        else if (session.proxy_type == openscp::ProxyType::HttpConnect)
            proxyProtocol = QStringLiteral("http");
        if (proxyProtocol.isEmpty()) {
            *error = QCoreApplication::translate(
                "MainWindow", "Unsupported proxy type for terminal command.");
            return false;
        }

        *command = shellJoinQuoted(
            {ncatExecutable, QStringLiteral("--proxy"),
             QStringLiteral("%1:%2").arg(proxyHost).arg(session.proxy_port),
             QStringLiteral("--proxy-type"), proxyProtocol,
             QStringLiteral("%h"), QStringLiteral("%p")});
        return true;
    }

    *error = QCoreApplication::translate(
        "MainWindow",
        "Could not find a proxy helper for terminal mode (tried: nc, ncat).");
    return false;
}

bool appendJumpOrProxyArguments(
    QStringList &arguments, const openscp::SessionOptions &session,
    const TerminalCommandBuilder::ExecutableLookup &lookup, QString *error) {
    const QString jumpHost = trimOptionalString(session.jump_host);
    const bool usesJumpHost = !jumpHost.isEmpty();
    const bool usesProxy = session.proxy_type != openscp::ProxyType::None;
    if (usesJumpHost && usesProxy) {
        *error = QCoreApplication::translate(
            "MainWindow",
            "Proxy and SSH jump host cannot be used together in the same "
            "terminal command.");
        return false;
    }

    if (usesJumpHost) {
        const QString jumpUser = trimOptionalString(session.jump_username);
        const QString jumpPrivateKey =
            trimOptionalString(session.jump_private_key_path);
        const std::uint16_t jumpPort =
            session.jump_port == 0 ? 22 : session.jump_port;

        if (jumpPrivateKey.isEmpty()) {
            QString jumpSpecification = jumpHost;
            if (!jumpUser.isEmpty()) {
                jumpSpecification =
                    jumpUser + QStringLiteral("@") + jumpSpecification;
            }
            if (jumpPort != 22) {
                jumpSpecification +=
                    QStringLiteral(":") + QString::number(jumpPort);
            }
            arguments << QStringLiteral("-J") << jumpSpecification;
            return true;
        }

        QStringList jumpCommand{
            QStringLiteral("ssh"),     QStringLiteral("-W"),
            QStringLiteral("%h:%p"),   QStringLiteral("-p"),
            QString::number(jumpPort),
        };
        if (!jumpUser.isEmpty())
            jumpCommand << QStringLiteral("-l") << jumpUser;
        jumpCommand << QStringLiteral("-i")
                    << QDir::fromNativeSeparators(
                           QDir::cleanPath(jumpPrivateKey))
                    << QStringLiteral("-o")
                    << QStringLiteral("IdentitiesOnly=yes") << jumpHost;
        arguments << QStringLiteral("-o")
                  << QStringLiteral("ProxyCommand=%1")
                         .arg(shellJoinQuoted(jumpCommand));
        return true;
    }

    if (!usesProxy)
        return true;

    QString proxyCommand;
    if (!buildProxyCommand(session, lookup, &proxyCommand, error))
        return false;
    arguments << QStringLiteral("-o")
              << QStringLiteral("ProxyCommand=%1").arg(proxyCommand);
    return true;
}

bool validateSession(const openscp::SessionOptions &session, QString *host,
                     QString *user, QString *error) {
    *host = QString::fromStdString(session.host).trimmed();
    *user = QString::fromStdString(session.username).trimmed();
    if (!host->isEmpty() && !user->isEmpty())
        return true;
    *error = QCoreApplication::translate(
        "MainWindow", "Session is missing host or username information.");
    return false;
}

bool buildSshCommand(const openscp::SessionOptions &session,
                     const QString &remotePath, bool forceInteractiveLogin,
                     const TerminalCommandBuilder::ExecutableLookup &lookup,
                     QString *command, QString *error) {
    const QString executable = lookup(QStringLiteral("ssh"));
    if (executable.isEmpty()) {
        *error = QCoreApplication::translate(
            "MainWindow", "OpenSSH client was not found in PATH.");
        return false;
    }

    QString host;
    QString user;
    if (!validateSession(session, &host, &user, error))
        return false;

    QStringList arguments{executable, QStringLiteral("-tt"),
                          QStringLiteral("-p"), QString::number(session.port)};
    appendHostKeyArguments(arguments, session);
    appendAuthenticationArguments(arguments, session, forceInteractiveLogin);
    if (!appendJumpOrProxyArguments(arguments, session, lookup, error))
        return false;

    arguments << QStringLiteral("%1@%2").arg(user, host);
    arguments << QStringLiteral("cd -- %1 2>/dev/null || cd /; "
                                "exec ${SHELL:-/bin/sh} -l")
                     .arg(shellSingleQuote(normalizeRemotePath(remotePath)));
    *command = shellJoinQuoted(arguments);
    return true;
}

bool buildSftpCommand(const openscp::SessionOptions &session,
                      const QString &remotePath, bool forceInteractiveLogin,
                      const TerminalCommandBuilder::ExecutableLookup &lookup,
                      QString *command, QString *error) {
    const QString executable = lookup(QStringLiteral("sftp"));
    if (executable.isEmpty()) {
        *error = QCoreApplication::translate(
            "MainWindow", "OpenSSH sftp client was not found in PATH.");
        return false;
    }

    QString host;
    QString user;
    if (!validateSession(session, &host, &user, error))
        return false;

    QStringList arguments{executable, QStringLiteral("-P"),
                          QString::number(session.port)};
    appendHostKeyArguments(arguments, session);
    appendAuthenticationArguments(arguments, session, forceInteractiveLogin);
    if (!appendJumpOrProxyArguments(arguments, session, lookup, error))
        return false;

    arguments << QStringLiteral("%1@%2:%3")
                     .arg(user, host, normalizeRemotePath(remotePath));
    *command = shellJoinQuoted(arguments);
    return true;
}

QString withSftpFallback(const QString &sshCommand,
                         const QString &sftpCommand) {
    return QStringLiteral("%1; _openscp_ssh_status=$?; "
                          "if [ \"$_openscp_ssh_status\" -eq 255 ]; then "
                          "printf '%s\\n' %2; %3; fi")
        .arg(sshCommand,
             shellSingleQuote(QCoreApplication::translate(
                 "MainWindow",
                 "OpenSCP: SSH shell was not available. Falling back to SFTP "
                 "CLI.")),
             sftpCommand);
}

QString appleScriptStringLiteral(const QString &raw) {
    QString escaped = raw;
    escaped.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    escaped.replace(QStringLiteral("\""), QStringLiteral("\\\""));
    return QStringLiteral("\"") + escaped + QStringLiteral("\"");
}

} // namespace

TerminalCommandBuilder::TerminalCommandBuilder(
    ExecutableLookup executableLookup)
    : executableLookup_(std::move(executableLookup)) {
    if (!executableLookup_) {
        executableLookup_ = [](const QString &name) {
            return QStandardPaths::findExecutable(name);
        };
    }
}

TerminalCommandResult TerminalCommandBuilder::prepare(
    const openscp::SessionOptions &session, const QString &remotePath,
    bool forceInteractiveLogin, bool enableSftpCliFallback) const {
    TerminalCommandResult result;
    if (!buildSshCommand(session, remotePath, forceInteractiveLogin,
                         executableLookup_, &result.command, &result.error)) {
        return result;
    }

    if (!enableSftpCliFallback)
        return result;

    QString sftpCommand;
    QString ignoredError;
    if (buildSftpCommand(session, remotePath, forceInteractiveLogin,
                         executableLookup_, &sftpCommand, &ignoredError)) {
        result.command = withSftpFallback(result.command, sftpCommand);
        result.hasSftpFallback = true;
    }
    return result;
}

bool TerminalCommandBuilder::launch(const QString &shellCommand,
                                    QString *error) const {
    if (error)
        error->clear();

#ifdef Q_OS_MAC
    const QString osascript = executableLookup_(QStringLiteral("osascript"));
    if (osascript.isEmpty()) {
        if (error)
            *error = QCoreApplication::translate("MainWindow",
                                                 "Could not locate osascript.");
        return false;
    }
    const QString activate =
        QStringLiteral("tell application \"Terminal\" to activate");
    const QString run =
        QStringLiteral("tell application \"Terminal\" to do script %1")
            .arg(appleScriptStringLiteral(shellCommand));
    if (!QProcess::startDetached(osascript, {QStringLiteral("-e"), activate,
                                             QStringLiteral("-e"), run})) {
        if (error)
            *error = QCoreApplication::translate(
                "MainWindow", "Could not launch Terminal.app.");
        return false;
    }
    return true;
#elif defined(Q_OS_LINUX)
    const auto tryLaunch = [&](const QString &program,
                               const QStringList &arguments) {
        const QString executable = executableLookup_(program);
        return !executable.isEmpty() &&
               QProcess::startDetached(executable, arguments);
    };
    if (tryLaunch(QStringLiteral("x-terminal-emulator"),
                  {QStringLiteral("-e"), QStringLiteral("sh"),
                   QStringLiteral("-lc"), shellCommand}) ||
        tryLaunch(QStringLiteral("gnome-terminal"),
                  {QStringLiteral("--"), QStringLiteral("sh"),
                   QStringLiteral("-lc"), shellCommand}) ||
        tryLaunch(QStringLiteral("konsole"),
                  {QStringLiteral("-e"), QStringLiteral("sh"),
                   QStringLiteral("-lc"), shellCommand}) ||
        tryLaunch(QStringLiteral("xfce4-terminal"),
                  {QStringLiteral("--command"),
                   QStringLiteral("sh -lc %1")
                       .arg(shellSingleQuote(shellCommand))}) ||
        tryLaunch(QStringLiteral("xterm"),
                  {QStringLiteral("-e"), QStringLiteral("sh"),
                   QStringLiteral("-lc"), shellCommand}) ||
        tryLaunch(QStringLiteral("alacritty"),
                  {QStringLiteral("-e"), QStringLiteral("sh"),
                   QStringLiteral("-lc"), shellCommand}) ||
        tryLaunch(
            QStringLiteral("kitty"),
            {QStringLiteral("sh"), QStringLiteral("-lc"), shellCommand})) {
        return true;
    }
    if (error)
        *error = QCoreApplication::translate(
            "MainWindow", "No compatible terminal emulator was found.");
    return false;
#else
    Q_UNUSED(shellCommand)
    if (error) {
        *error = QCoreApplication::translate(
            "MainWindow",
            "Open in terminal action is not supported on this platform.");
    }
    return false;
#endif
}

} // namespace openscpui
