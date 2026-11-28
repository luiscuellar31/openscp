// How local folder operations tell a link from the entry it leads to.
#pragma once

#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileInfo>

// Whether local operations must take `info` as a link: copying recreates it,
// deleting removes the link itself, and scans never descend into it. Windows
// junctions count, though isSymLink() misses them, and Windows shortcuts
// (.lnk files), which isSymLink() reports, are ordinary files there.
inline bool isLocalLink(const QFileInfo &info) {
#ifdef Q_OS_WIN
    return info.isSymbolicLink() || info.isJunction();
#else
    return info.isSymLink();
#endif
}

// A Windows shortcut is an ordinary file to local operations, as to Explorer,
// but Qt describes the entry it leads to: isDir(), isFile(), size(),
// lastModified() and canonicalFilePath() follow it. These describe the
// shortcut itself, and any other entry as QFileInfo does; only Windows has
// shortcuts.
inline bool isLocalFolder(const QFileInfo &info) {
    return info.isDir() && !info.isShortcut();
}

inline bool isLocalFile(const QFileInfo &info) {
    return info.isFile() || info.isShortcut();
}

// -1 when the size cannot be read.
inline qint64 localFileSize(const QFileInfo &info) {
    if (!info.isShortcut())
        return info.size();
    QFile file(info.absoluteFilePath());
    return file.open(QIODevice::ReadOnly) ? file.size() : -1;
}

// Invalid when the time cannot be read.
inline QDateTime localModificationTime(const QFileInfo &info) {
    if (!info.isShortcut())
        return info.lastModified();
    QFile file(info.absoluteFilePath());
    return file.open(QIODevice::ReadOnly)
               ? file.fileTime(QFileDevice::FileModificationTime)
               : QDateTime();
}

// Empty when the folder that holds the entry cannot be resolved.
inline QString localCanonicalPath(const QFileInfo &info) {
    if (!info.isShortcut())
        return info.canonicalFilePath();
    const QString folder = QFileInfo(info.absolutePath()).canonicalFilePath();
    return folder.isEmpty() ? QString()
                            : QDir(folder).filePath(info.fileName());
}
