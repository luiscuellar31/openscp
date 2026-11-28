#pragma once

#include "openscp/SessionOptions.hpp"

#include <QString>
#include <QStringList>

#include <functional>

namespace openscpui {

struct TerminalCommandResult {
    // What the terminal runs: a POSIX shell command line, or on Windows a
    // Windows PowerShell script.
    QString command;
    // The terminal window's title, where launch() can set it (Windows).
    QString title;
    QString error;
    bool hasSftpFallback = false;

    bool isValid() const { return !command.isEmpty() && error.isEmpty(); }
};

class TerminalCommandBuilder {
    public:
    using ExecutableLookup = std::function<QString(const QString &)>;

    explicit TerminalCommandBuilder(ExecutableLookup executableLookup = {});

    TerminalCommandResult prepare(const openscp::SessionOptions &session,
                                  const QString &remotePath,
                                  bool forceInteractiveLogin,
                                  bool enableSftpCliFallback) const;

    bool launch(const TerminalCommandResult &command,
                QString *error = nullptr) const;

    // The command a terminal types into the user's login shell, which on
    // macOS may be fish, csh or tcsh: POSIX sh runs `shellCommand` instead.
    static QString posixShellCommand(const QString &shellCommand);

#ifdef Q_OS_WIN
    // The powershell.exe arguments with which launch() runs a prepared
    // script in a new console.
    static QStringList powerShellArguments(const QString &script);
#endif

    private:
    ExecutableLookup executableLookup_;
};

} // namespace openscpui
