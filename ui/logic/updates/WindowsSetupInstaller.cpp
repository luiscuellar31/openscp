#include "logic/updates/WindowsSetupInstaller.hpp"

#include <QCryptographicHash>
#include <QDir>

#ifdef Q_OS_WIN
// objbase.h and shellapi.h need the types windows.h declares.
// clang-format off
#include <windows.h>
#include <objbase.h>
#include <shellapi.h>
// clang-format on

#include <string>
#endif

namespace openscpui {

QStringList windowsSetupArguments(WindowsSetupScope scope, qint64 pid,
                                  const QString &logPath) {
    // /WAITPID and /RELAUNCH are read by packaging/windows/OpenSCP.iss.
    return {
        QStringLiteral("/VERYSILENT"),
        QStringLiteral("/SUPPRESSMSGBOXES"),
        QStringLiteral("/NORESTART"),
        scope == WindowsSetupScope::AllUsers ? QStringLiteral("/ALLUSERS")
                                             : QStringLiteral("/CURRENTUSER"),
        QStringLiteral("/WAITPID=%1").arg(pid),
        QStringLiteral("/RELAUNCH=1"),
        QStringLiteral("/LOG=") + QDir::toNativeSeparators(logPath),
    };
}

#ifdef Q_OS_WIN
namespace {
// Inno Setup splits its command line as Delphi programs do: double quotes
// group, without escapes. Windows paths cannot contain double quotes.
QString setupCommandLine(const QStringList &arguments) {
    QStringList quoted;
    for (const QString &argument : arguments) {
        quoted << (argument.contains(QLatin1Char(' '))
                       ? QLatin1Char('"') + argument + QLatin1Char('"')
                       : argument);
    }
    return quoted.join(QLatin1Char(' '));
}

struct HandleCloser {
    HANDLE handle;
    ~HandleCloser() { CloseHandle(handle); }
};
} // namespace
#endif

bool startWindowsSetup(const QString &setup, const UpdateArtifact &artifact,
                       const QStringList &arguments, WindowsSetupScope scope,
                       QString &error) {
    error = QStringLiteral("Could not start the update installer. The current "
                           "version is still installed.");
#ifdef Q_OS_WIN
    if (artifact.size <= 0 || artifact.size > kMaximumUpdateSize ||
        artifact.sha256.size() != 32)
        return false;
    const std::wstring path = QDir::toNativeSeparators(setup).toStdWString();
    // Others may read the file, but nobody may change, rename or delete it
    // while this handle is open: the setup that starts is the one checked.
    const HANDLE file =
        CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                    OPEN_EXISTING, FILE_FLAG_SEQUENTIAL_SCAN, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return false;
    const HandleCloser closer{file};
    LARGE_INTEGER size{};
    if (!GetFileSizeEx(file, &size) || size.QuadPart != artifact.size)
        return false;
    QCryptographicHash hash(QCryptographicHash::Sha256);
    QByteArray buffer(256 * 1024, Qt::Uninitialized);
    qint64 total = 0;
    while (true) {
        DWORD count = 0;
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()),
                      &count, nullptr))
            return false;
        if (count == 0)
            break;
        total += count;
        if (total > artifact.size)
            return false;
        hash.addData(QByteArrayView(buffer.constData(), count));
    }
    if (total != artifact.size || hash.result() != artifact.sha256) {
        error = QStringLiteral("The downloaded update does not match its "
                               "signed description. It was not installed.");
        return false;
    }

    const std::wstring parameters = setupCommandLine(arguments).toStdWString();
    if (scope == WindowsSetupScope::AllUsers) {
        // Elevated from the start, so that it opens the file while this
        // handle still guards it.
        const HRESULT com = CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                        COINIT_DISABLE_OLE1DDE);
        SHELLEXECUTEINFOW info{};
        info.cbSize = sizeof(info);
        info.fMask =
            SEE_MASK_NOCLOSEPROCESS | SEE_MASK_NOASYNC | SEE_MASK_FLAG_NO_UI;
        info.lpVerb = L"runas";
        info.lpFile = path.c_str();
        info.lpParameters = parameters.c_str();
        info.nShow = SW_SHOWNORMAL;
        const bool started = ShellExecuteExW(&info);
        const DWORD failure = GetLastError();
        if (SUCCEEDED(com))
            CoUninitialize();
        if (!started) {
            if (failure == ERROR_CANCELLED)
                error = QStringLiteral("The update was not installed because "
                                       "administrator approval was declined.");
            return false;
        }
        if (info.hProcess)
            CloseHandle(info.hProcess);
    } else {
        std::wstring commandLine = L"\"" + path + L"\" " + parameters;
        STARTUPINFOW startup{};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process{};
        if (!CreateProcessW(path.c_str(), commandLine.data(), nullptr, nullptr,
                            FALSE, 0, nullptr, nullptr, &startup, &process))
            return false;
        CloseHandle(process.hThread);
        CloseHandle(process.hProcess);
    }
    error.clear();
    return true;
#else
    Q_UNUSED(setup)
    Q_UNUSED(artifact)
    Q_UNUSED(arguments)
    Q_UNUSED(scope)
    return false;
#endif
}
} // namespace openscpui
