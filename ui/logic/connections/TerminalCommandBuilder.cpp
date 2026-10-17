#include "logic/connections/TerminalCommandBuilder.hpp"

#include "logic/navigation/RemotePath.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>
#include <QStringList>

#include <algorithm>
#include <optional>
#include <string>

#ifdef Q_OS_WIN
#include <windows.h>
#endif

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

// Whether ssh can take `name` as a host or user name: one that starts with '-'
// would be read as an option, and spaces or `forbidden` characters would
// break out of the command line that ssh builds around it.
bool isSafeSshName(const QString &name, QStringView forbidden) {
    if (name.startsWith(QLatin1Char('-')))
        return false;
    return std::none_of(name.cbegin(), name.cend(), [forbidden](QChar ch) {
        return ch.isSpace() || ch.category() == QChar::Other_Control ||
               forbidden.contains(ch);
    });
}

// ssh expands %-tokens in a ProxyCommand, so a literal % is written twice.
QString proxyCommandLiteral(QString value) {
    return value.replace(QStringLiteral("%"), QStringLiteral("%%"));
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

// ssh parses a -o value like a line of its config file, where spaces separate
// values unless they are inside double quotes. OpenSSH 8.4 and later also
// expand %-tokens in known-hosts file names.
QString sshConfigFileName(const QString &path) {
    QString escaped = path;
    escaped.replace(QStringLiteral("%"), QStringLiteral("%%"));
    escaped.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    escaped.replace(QStringLiteral("\""), QStringLiteral("\\\""));
    return QStringLiteral("\"") + escaped + QStringLiteral("\"");
}

void appendHostKeyArguments(QStringList &arguments,
                            const openscp::SessionOptions &session) {
    const openscp::KnownHostsPolicy policy =
        openscp::normalizeKnownHostsPolicy(session.known_hosts_policy);
    if (policy == openscp::KnownHostsPolicy::Off) {
#ifdef Q_OS_WIN
        const QString nullDevice = QStringLiteral("NUL");
#else
        const QString nullDevice = QStringLiteral("/dev/null");
#endif
        arguments << QStringLiteral("-o")
                  << QStringLiteral("StrictHostKeyChecking=no")
                  << QStringLiteral("-o")
                  << QStringLiteral("UserKnownHostsFile=%1").arg(nullDevice);
        return;
    }

    const QString strictValue = policy == openscp::KnownHostsPolicy::AcceptNew
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
                         .arg(sshConfigFileName(QDir::fromNativeSeparators(
                             QDir::cleanPath(knownHostsPath))));
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
            {proxyCommandLiteral(netcatExecutable), QStringLiteral("-x"),
             proxyCommandLiteral(QStringLiteral("%1:%2").arg(proxyHost).arg(
                 session.proxy_port)),
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
            {proxyCommandLiteral(ncatExecutable), QStringLiteral("--proxy"),
             proxyCommandLiteral(QStringLiteral("%1:%2").arg(proxyHost).arg(
                 session.proxy_port)),
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
#ifdef Q_OS_WIN
    // OpenSCP does not connect through jump hosts on Windows yet, and Windows
    // has no nc or ncat to run as a proxy command.
    if (usesJumpHost || usesProxy) {
        *error = QCoreApplication::translate(
            "MainWindow", "On Windows, Open in terminal cannot use an SSH "
                          "jump host or a proxy yet.");
        return false;
    }
#endif
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
        // With -J, ssh puts the jump host and user unquoted into the shell
        // command it runs to reach the jump host, and splits the value at
        // commas, cuts it at '#' and expands %-tokens in it.
        const QStringView jumpForbidden = u"'\"\\`$;&|<>(){}*?!#~,%";
        if (!isSafeSshName(jumpHost, jumpForbidden) ||
            !isSafeSshName(jumpUser, jumpForbidden)) {
            *error = QCoreApplication::translate(
                "MainWindow", "The SSH jump host or its user name contains "
                              "characters that OpenSSH cannot take safely.");
            return false;
        }

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
                    << proxyCommandLiteral(QDir::fromNativeSeparators(
                           QDir::cleanPath(jumpPrivateKey)))
                    << QStringLiteral("-o")
                    << QStringLiteral("IdentitiesOnly=yes")
                    << QStringLiteral("--") << jumpHost;
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
    if (host->isEmpty() || user->isEmpty()) {
        *error = QCoreApplication::translate(
            "MainWindow", "Session is missing host or username information.");
        return false;
    }
    // A proxy command gets the host inside single quotes, as '%h'.
    if (!isSafeSshName(*host, u"'")) {
        *error = QCoreApplication::translate(
            "MainWindow",
            "The host name contains characters that OpenSSH cannot take "
            "safely.");
        return false;
    }
    return true;
}

// The ssh program and its arguments, to be quoted for the platform's shell.
bool buildSshArguments(const openscp::SessionOptions &session,
                       const QString &remotePath, bool forceInteractiveLogin,
                       const TerminalCommandBuilder::ExecutableLookup &lookup,
                       QStringList *command, QString *error) {
    const QString executable = lookup(QStringLiteral("ssh"));
    if (executable.isEmpty()) {
#ifdef Q_OS_WIN
        *error = QCoreApplication::translate(
            "MainWindow",
            "The OpenSSH client was not found. Add the \"OpenSSH Client\" "
            "optional feature in Windows Settings.");
#else
        *error = QCoreApplication::translate(
            "MainWindow", "OpenSSH client was not found in PATH.");
#endif
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

    // "--" keeps a user name that starts with '-' from being read as an
    // option.
    arguments << QStringLiteral("--")
              << QStringLiteral("%1@%2").arg(user, host);
    // The server's login shell, which may be fish or tcsh, only has to read
    // single quotes; POSIX sh changes directory and starts that shell.
    arguments << QStringLiteral("exec /bin/sh -c %1 sh %2")
                     .arg(shellSingleQuote(QStringLiteral(
                              "cd -- \"$1\" 2>/dev/null || cd /; "
                              "exec \"${SHELL:-/bin/sh}\" -l")),
                          shellSingleQuote(normalizeRemotePath(remotePath)));
    *command = arguments;
    return true;
}

bool buildSftpArguments(const openscp::SessionOptions &session,
                        const QString &remotePath, bool forceInteractiveLogin,
                        const TerminalCommandBuilder::ExecutableLookup &lookup,
                        QStringList *command, QString *error) {
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

    arguments << QStringLiteral("--")
              << QStringLiteral("%1@%2:%3")
                     .arg(user, host, normalizeRemotePath(remotePath));
    *command = arguments;
    return true;
}

QString sftpFallbackMessage() {
    return QCoreApplication::translate(
        "MainWindow",
        "OpenSCP: SSH shell was not available. Falling back to SFTP CLI.");
}

#ifdef Q_OS_WIN
// Windows itself names its system folder, normally C:\Windows\System32; PATH
// and %SystemRoot% could point elsewhere.
QString windowsSystemDirectory() {
    wchar_t buffer[MAX_PATH];
    const UINT length = GetSystemDirectoryW(buffer, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    return QDir::fromNativeSeparators(
        QString::fromWCharArray(buffer, static_cast<qsizetype>(length)));
}

// Quotes an argument for a Windows command line, which every program splits
// itself; this follows the Microsoft C runtime, which the OpenSSH for Windows
// programs use. Characters that Cygwin and MSYS programs would treat as
// quotes or wildcards are quoted too.
QString windowsCommandLineArgument(const QString &argument) {
    const auto isPlain = [](QChar ch) {
        return (ch >= u'a' && ch <= u'z') || (ch >= u'A' && ch <= u'Z') ||
               (ch >= u'0' && ch <= u'9') ||
               QStringView(u"-_./:=@,+%").contains(ch);
    };
    if (!argument.isEmpty() &&
        std::all_of(argument.cbegin(), argument.cend(), isPlain)) {
        return argument;
    }

    QString quoted = QStringLiteral("\"");
    qsizetype backslashes = 0;
    for (const QChar ch : argument) {
        if (ch == u'\\') {
            ++backslashes;
            continue;
        }
        // Backslashes are literal unless a quote follows them.
        quoted += QString(ch == u'"' ? backslashes * 2 + 1 : backslashes,
                          QLatin1Char('\\'));
        quoted += ch;
        backslashes = 0;
    }
    quoted += QString(backslashes * 2, QLatin1Char('\\'));
    quoted += QLatin1Char('"');
    return quoted;
}

// A single-quoted Windows PowerShell string. PowerShell also ends one at the
// typographic single quotes, and reads any of them doubled as one.
QString powerShellSingleQuote(const QString &value) {
    QString quoted = QStringLiteral("'");
    for (const QChar ch : value) {
        quoted += ch;
        if (QStringView(u"'\u2018\u2019\u201A\u201B").contains(ch))
            quoted += ch;
    }
    quoted += QLatin1Char('\'');
    return quoted;
}

// A PowerShell expression that runs a program in its console, waits for it
// and gives its exit code. Start-Process takes the command line as written;
// Windows PowerShell 5.1 would lose the quotes inside arguments passed
// directly.
QString powerShellExitCodeOf(const QStringList &command) {
    QStringList arguments;
    for (qsizetype i = 1; i < command.size(); ++i)
        arguments << windowsCommandLineArgument(command.at(i));
    return QStringLiteral("(Start-Process -FilePath ") +
           powerShellSingleQuote(QDir::toNativeSeparators(command.front())) +
           QStringLiteral(" -ArgumentList ") +
           powerShellSingleQuote(arguments.join(QLatin1Char(' '))) +
           QStringLiteral(" -NoNewWindow -Wait -PassThru).ExitCode");
}

// The script the new console runs: ssh, then sftp if ssh could not connect,
// as on other systems. The window closes with the session, unless it ended
// with an error that the user should be able to read first.
QString powerShellScript(const QStringList &ssh, const QStringList &sftp) {
    QString script = QStringLiteral("$status = ") + powerShellExitCodeOf(ssh);
    if (!sftp.isEmpty()) {
        script += QStringLiteral("; if ($status -eq 255) { Write-Host ") +
                  powerShellSingleQuote(sftpFallbackMessage()) +
                  QStringLiteral("; $status = ") + powerShellExitCodeOf(sftp) +
                  QStringLiteral(" }");
    }
    // Not QString::arg(), which would replace the %1 that PowerShell fills.
    script +=
        QStringLiteral("; if ($status -ne 0) { Write-Host ''; Write-Host (") +
        powerShellSingleQuote(QCoreApplication::translate(
            "MainWindow", "OpenSCP: the session ended with exit code %1.")) +
        QStringLiteral(" -replace '%1', $status); Read-Host ") +
        powerShellSingleQuote(QCoreApplication::translate(
            "MainWindow", "Press Enter to close this window")) +
        QStringLiteral(" | Out-Null }");
    return script;
}
#else
QString withSftpFallback(const QString &sshCommand,
                         const QString &sftpCommand) {
    return QStringLiteral("%1; _openscp_ssh_status=$?; "
                          "if [ \"$_openscp_ssh_status\" -eq 255 ]; then "
                          "printf '%s\\n' %2; %3; fi")
        .arg(sshCommand, shellSingleQuote(sftpFallbackMessage()), sftpCommand);
}
#endif

#ifdef Q_OS_MAC
QString appleScriptStringLiteral(const QString &raw) {
    QString escaped = raw;
    escaped.replace(QStringLiteral("\\"), QStringLiteral("\\\\"));
    escaped.replace(QStringLiteral("\""), QStringLiteral("\\\""));
    return QStringLiteral("\"") + escaped + QStringLiteral("\"");
}
#endif

} // namespace

TerminalCommandBuilder::TerminalCommandBuilder(
    ExecutableLookup executableLookup)
    : executableLookup_(std::move(executableLookup)) {
    if (!executableLookup_) {
        executableLookup_ = [](const QString &name) {
#ifdef Q_OS_WIN
            // Only an .exe: Windows would run ssh.cmd or ssh.bat through
            // cmd.exe, which reads the arguments with other quoting rules.
            const QString executable =
                QStandardPaths::findExecutable(name + QStringLiteral(".exe"));
            if (!executable.isEmpty())
                return executable;
            // The OpenSSH client that Windows installs, if PATH misses it.
            const QString systemDirectory = windowsSystemDirectory();
            const QString bundled = systemDirectory +
                                    QStringLiteral("/OpenSSH/") + name +
                                    QStringLiteral(".exe");
            return !systemDirectory.isEmpty() && QFileInfo(bundled).isFile()
                       ? bundled
                       : QString();
#else
            return QStandardPaths::findExecutable(name);
#endif
        };
    }
}

TerminalCommandResult TerminalCommandBuilder::prepare(
    const openscp::SessionOptions &session, const QString &remotePath,
    bool forceInteractiveLogin, bool enableSftpCliFallback) const {
    TerminalCommandResult result;
    QStringList sshCommand;
    if (!buildSshArguments(session, remotePath, forceInteractiveLogin,
                           executableLookup_, &sshCommand, &result.error)) {
        return result;
    }
    result.title = QStringLiteral("%1@%2 - OpenSCP")
                       .arg(QString::fromStdString(session.username).trimmed(),
                            QString::fromStdString(session.host).trimmed());

    QStringList sftpCommand;
    QString ignoredError;
    if (enableSftpCliFallback &&
        !buildSftpArguments(session, remotePath, forceInteractiveLogin,
                            executableLookup_, &sftpCommand, &ignoredError)) {
        sftpCommand.clear();
    }
    result.hasSftpFallback = !sftpCommand.isEmpty();

#ifdef Q_OS_WIN
    result.command = powerShellScript(sshCommand, sftpCommand);
#else
    result.command = shellJoinQuoted(sshCommand);
    if (result.hasSftpFallback) {
        result.command =
            withSftpFallback(result.command, shellJoinQuoted(sftpCommand));
    }
#endif
    return result;
}

QString TerminalCommandBuilder::posixShellCommand(const QString &shellCommand) {
    return QStringLiteral("/bin/sh -c %1").arg(shellSingleQuote(shellCommand));
}

#ifdef Q_OS_WIN
QStringList TerminalCommandBuilder::powerShellArguments(const QString &script) {
    return {QStringLiteral("-NoProfile"), QStringLiteral("-Command"), script};
}
#endif

bool TerminalCommandBuilder::launch(const TerminalCommandResult &command,
                                    QString *error) const {
    if (error)
        error->clear();
    const QString &shellCommand = command.command;

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
            .arg(appleScriptStringLiteral(posixShellCommand(shellCommand)));
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
#elif defined(Q_OS_WIN)
    const QString systemDirectory = windowsSystemDirectory();
    const QString powerShell =
        systemDirectory +
        QStringLiteral("/WindowsPowerShell/v1.0/powershell.exe");
    if (systemDirectory.isEmpty() || !QFileInfo(powerShell).isFile()) {
        if (error)
            *error = QCoreApplication::translate(
                "MainWindow", "Could not locate Windows PowerShell.");
        return false;
    }

    std::wstring title = command.title.toStdWString();
    QProcess process;
    process.setProgram(QDir::toNativeSeparators(powerShell));
    process.setArguments(powerShellArguments(shellCommand));
    // A process keeps its working folder from being deleted on Windows.
    process.setWorkingDirectory(QDir::homePath());
    process.setCreateProcessArgumentsModifier(
        [&title](QProcess::CreateProcessArguments *arguments) {
            // Qt starts a GUI app's detached processes without a console;
            // ssh needs a console window of its own, with its own input and
            // output, to ask for passwords and draw the remote shell.
            arguments->flags &= ~static_cast<DWORD>(CREATE_NO_WINDOW);
            arguments->flags |= CREATE_NEW_CONSOLE;
            arguments->inheritHandles = false;
            arguments->startupInfo->dwFlags &=
                ~static_cast<DWORD>(STARTF_USESTDHANDLES);
            arguments->startupInfo->hStdInput = nullptr;
            arguments->startupInfo->hStdOutput = nullptr;
            arguments->startupInfo->hStdError = nullptr;
            if (!title.empty())
                arguments->startupInfo->lpTitle = title.data();
        });
    if (!process.startDetached()) {
        if (error)
            *error = QCoreApplication::translate(
                "MainWindow", "Could not open a Windows PowerShell window.");
        return false;
    }
    return true;
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
