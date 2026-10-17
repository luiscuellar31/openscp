// File system fixtures whose making differs between platforms.
#pragma once

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QProcess>
#include <QString>
#include <QStringList>

#include <filesystem>
#include <system_error>

namespace openscp::testsupport {

inline std::filesystem::path nativeTestPath(const QString &path) {
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(path).constData());
#endif
}

// Makes `link` a link to `target` that local operations must take as a link,
// or returns false when this system cannot, and the case should be skipped.
// POSIX makes a symbolic link. QFile::link() would make a shortcut file on
// Windows, so there a folder gets a junction, which needs no privilege, and
// a file a symbolic link, which needs Developer Mode or an administrator.
inline bool createLocalLink(const QString &target, const QString &link) {
#ifdef Q_OS_WIN
    if (QFileInfo(target).isDir()) {
        QProcess mklink;
        mklink.start(QStringLiteral("cmd.exe"),
                     {QStringLiteral("/c"), QStringLiteral("mklink"),
                      QStringLiteral("/J"), QDir::toNativeSeparators(link),
                      QDir::toNativeSeparators(target)});
        return mklink.waitForFinished(10000) &&
               mklink.exitStatus() == QProcess::NormalExit &&
               mklink.exitCode() == 0;
    }
    std::error_code error;
    std::filesystem::create_symlink(nativeTestPath(target),
                                    nativeTestPath(link), error);
    return !error;
#else
    return QFile::link(target, link);
#endif
}

} // namespace openscp::testsupport
