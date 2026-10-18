// Shared helper utilities for MainWindow split implementation files.
#include "logic/common/MainWindowSharedUtils.hpp"

#include "logic/common/LocalLinks.hpp"
#include "logic/common/UiAlerts.hpp"
#include "openscp/SafeLocalFile.hpp"

#include <QCoreApplication>
#include <QCursor>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QInputDialog>
#include <QLineEdit>
#include <QMenu>
#include <QMessageBox>
#include <QStorageInfo>
#include <QUuid>

#include <filesystem>

namespace {

std::filesystem::path localFilesystemPath(const QString &path) {
#ifdef Q_OS_WIN
    return std::filesystem::path(path.toStdWString());
#else
    return std::filesystem::path(QFile::encodeName(path).constData());
#endif
}

bool localEntryExists(const QString &path) {
    const QFileInfo info(path);
    return info.exists() || isLocalLink(info);
}

bool renameEntry(const QString &from, const QString &to) {
    const QFileInfo info(from);
    if (info.isDir() && !isLocalLink(info))
        return QDir().rename(from, to);
    return QFile::rename(from, to);
}

// Recreates a link with its original target text, so a relative link stays
// relative and the target is never copied.
bool copyLink(const QString &srcPath, const QString &dstPath, QString &error) {
    std::error_code nativeError;
    std::filesystem::copy_symlink(localFilesystemPath(srcPath),
                                  localFilesystemPath(dstPath), nativeError);
    if (!nativeError)
        return true;
    error = QString(QCoreApplication::translate("MainWindow",
                                                "Could not copy link: %1 (%2)"))
                .arg(srcPath, QString::fromStdString(nativeError.message()));
    return false;
}

// True when path is folder or lies inside it, once both are resolved.
bool isSameOrInside(const QString &path, const QString &folder) {
    const QString canonicalPath = QFileInfo(path).canonicalFilePath();
    QString canonicalFolder = QFileInfo(folder).canonicalFilePath();
    if (canonicalPath.isEmpty() || canonicalFolder.isEmpty())
        return false;
    if (!canonicalFolder.endsWith(QLatin1Char('/')))
        canonicalFolder += QLatin1Char('/');
    return (canonicalPath + QLatin1Char('/')).startsWith(canonicalFolder);
}

bool copyCanceled(const std::function<bool()> &shouldCancel, QString &error) {
    if (!shouldCancel || !shouldCancel())
        return false;
    error = QCoreApplication::translate("MainWindow", "Canceled");
    return true;
}

bool copyEntryToEmptyDestination(const QString &srcPath, const QString &dstPath,
                                 QString &error,
                                 const std::function<bool()> &shouldCancel) {
    QFileInfo srcInfo(srcPath);

    // isFile() and isDir() follow links, so check for a link first.
    if (isLocalLink(srcInfo))
        return copyLink(srcPath, dstPath, error);

    // QFile::copy() copies a shortcut itself.
    if (isLocalFile(srcInfo)) {
        QDir().mkpath(QFileInfo(dstPath).dir().absolutePath());
        if (!QFile::copy(srcPath, dstPath)) {
            error = QString(QCoreApplication::translate(
                                "MainWindow", "Could not copy file: %1"))
                        .arg(srcPath);
            return false;
        }
        return true;
    }

    if (isLocalFolder(srcInfo)) {
        // Create destination directory
        if (!QDir().mkpath(dstPath)) {
            error = QString(QCoreApplication::translate(
                                "MainWindow",
                                "Could not create destination folder: %1"))
                        .arg(dstPath);
            return false;
        }
        // Iterate recursively. Without Hidden and System, the iterator skips
        // dotfiles and dangling links that removing the source would delete.
        QDirIterator dirIterator(srcPath,
                                 QDir::NoDotAndDotDot | QDir::AllEntries |
                                     QDir::Hidden | QDir::System,
                                 QDirIterator::Subdirectories);
        while (dirIterator.hasNext()) {
            if (copyCanceled(shouldCancel, error))
                return false;
            dirIterator.next();
            const QFileInfo entryFileInfo = dirIterator.fileInfo();
            const QString relativePath = QDir(srcPath).relativeFilePath(
                entryFileInfo.absoluteFilePath());
            const QString target = QDir(dstPath).filePath(relativePath);

            if (isLocalLink(entryFileInfo)) {
                QDir().mkpath(QFileInfo(target).dir().absolutePath());
                if (!copyLink(entryFileInfo.absoluteFilePath(), target, error))
                    return false;
            } else if (isLocalFolder(entryFileInfo)) {
                if (!QDir().mkpath(target)) {
                    error =
                        QString(
                            QCoreApplication::translate(
                                "MainWindow",
                                "Could not create destination subfolder: %1"))
                            .arg(target);
                    return false;
                }
            } else if (isLocalFile(entryFileInfo)) {
                QDir().mkpath(QFileInfo(target).dir().absolutePath());
                if (!QFile::copy(entryFileInfo.absoluteFilePath(), target)) {
                    error = QString(QCoreApplication::translate(
                                        "MainWindow", "Failed to copy: %1"))
                                .arg(entryFileInfo.absoluteFilePath());
                    return false;
                }
            } else {
                // Reading a FIFO or device would block or never end.
                error =
                    QString(QCoreApplication::translate(
                                "MainWindow", "Cannot copy special file: %1"))
                        .arg(entryFileInfo.absoluteFilePath());
                return false;
            }
        }
        return true;
    }

    error = QCoreApplication::translate(
        "MainWindow", "Source entry is neither file nor folder.");
    return false;
}

} // namespace

LocalRenameResult renameLocalEntry(const QString &sourcePath,
                                   const QString &targetPath, QString *error) {
    std::error_code nativeError;
    std::filesystem::rename(localFilesystemPath(sourcePath),
                            localFilesystemPath(targetPath), nativeError);
    if (!nativeError)
        return LocalRenameResult::Moved;
    if (nativeError == std::errc::cross_device_link)
        return LocalRenameResult::CrossDevice;
    if (error)
        *error = QString::fromStdString(nativeError.message());
    return LocalRenameResult::Failed;
}

#ifdef Q_OS_WIN
namespace {

// Removes a tree one level at a time. symlink_status() reports a junction as
// a junction, not a directory, and remove() deletes a link or junction
// itself, and a read-only file too, so nothing outside the tree is touched.
bool removeNativeTree(const std::filesystem::path &path) {
    std::error_code error;
    const auto status = std::filesystem::symlink_status(path, error);
    if (status.type() == std::filesystem::file_type::not_found)
        return true;
    if (error)
        return false;
    bool removed = true;
    if (status.type() == std::filesystem::file_type::directory) {
        std::filesystem::directory_iterator entry(path, error);
        for (; !error && entry != std::filesystem::directory_iterator();
             entry.increment(error))
            removed = removeNativeTree(entry->path()) && removed;
        if (error)
            return false;
    }
    std::filesystem::remove(path, error);
    return removed && !error;
}

} // namespace
#endif

bool removeLocalEntry(const QString &path) {
#ifdef Q_OS_WIN
    // QDir::removeRecursively() descends into junctions.
    return removeNativeTree(localFilesystemPath(path));
#else
    const QFileInfo info(path);
    if (!info.exists() && !info.isSymLink())
        return true;
    // isDir() follows links, and removeRecursively() on a link to a folder
    // would empty the target.
    if (info.isDir() && !info.isSymLink())
        return QDir(path).removeRecursively();
    return QFile::remove(path);
#endif
}

bool copyLocalEntry(const QString &srcPath, const QString &dstPath,
                    QString &error, const std::function<bool()> &shouldCancel) {
    if (copyCanceled(shouldCancel, error))
        return false;
    const QString destinationParent = QFileInfo(dstPath).dir().absolutePath();
    if (!QDir().mkpath(destinationParent)) {
        error = QString(QCoreApplication::translate(
                            "MainWindow",
                            "Could not create destination folder: %1"))
                    .arg(destinationParent);
        return false;
    }

    // The staged copy would land inside the tree being copied and grow
    // without end.
    const QFileInfo srcInfo(srcPath);
    if (isLocalFolder(srcInfo) && !isLocalLink(srcInfo) &&
        isSameOrInside(destinationParent, srcPath)) {
        error =
            QString(QCoreApplication::translate(
                        "MainWindow", "Cannot copy a folder into itself: %1"))
                .arg(srcPath);
        return false;
    }

    const QString token = QUuid::createUuid().toString(QUuid::WithoutBraces);
    const QString stagedPath =
        QDir(destinationParent)
            .filePath(QStringLiteral(".openscp-stage-") + token);
    const QString backupPath =
        QDir(destinationParent)
            .filePath(QStringLiteral(".openscp-backup-") + token);

    if (!copyEntryToEmptyDestination(srcPath, stagedPath, error,
                                     shouldCancel) ||
        copyCanceled(shouldCancel, error)) {
        (void)removeLocalEntry(stagedPath);
        return false;
    }

    const bool hadDestination = localEntryExists(dstPath);
    if (hadDestination && !renameEntry(dstPath, backupPath)) {
        (void)removeLocalEntry(stagedPath);
        error = QString(QCoreApplication::translate(
                            "MainWindow",
                            "Could not preserve existing destination: %1"))
                    .arg(dstPath);
        return false;
    }

    if (!renameEntry(stagedPath, dstPath)) {
        (void)removeLocalEntry(stagedPath);
        const bool restored =
            !hadDestination || renameEntry(backupPath, dstPath);
        error =
            restored
                ? QString(
                      QCoreApplication::translate(
                          "MainWindow", "Could not publish completed copy: %1"))
                      .arg(dstPath)
                : QString(QCoreApplication::translate(
                              "MainWindow",
                              "Could not publish completed copy or restore the "
                              "previous destination. Recovery copy: %1"))
                      .arg(backupPath);
        return false;
    }

    if (hadDestination)
        (void)removeLocalEntry(backupPath);
    return true;
}

bool moveLocalEntry(const QString &srcPath, const QString &dstPath,
                    QString &error, const std::function<bool()> &shouldCancel) {
    if (copyCanceled(shouldCancel, error))
        return false;
    if (QDir::cleanPath(QFileInfo(srcPath).absoluteFilePath()) ==
        QDir::cleanPath(QFileInfo(dstPath).absoluteFilePath())) {
        return true;
    }

    const bool hadDestination = localEntryExists(dstPath);
    QString backupPath;
    if (hadDestination) {
        const QString token =
            QUuid::createUuid().toString(QUuid::WithoutBraces);
        backupPath = QDir(QFileInfo(dstPath).dir())
                         .filePath(QStringLiteral(".openscp-backup-") + token);
        QString backupError;
        if (renameLocalEntry(dstPath, backupPath, &backupError) !=
            LocalRenameResult::Moved) {
            error = QString(QCoreApplication::translate(
                                "MainWindow",
                                "Could not preserve existing destination: %1 "
                                "(%2)"))
                        .arg(dstPath, backupError);
            return false;
        }
    }

    QString renameError;
    const LocalRenameResult renameResult =
        renameLocalEntry(srcPath, dstPath, &renameError);
    if (renameResult == LocalRenameResult::Moved) {
        if (hadDestination)
            (void)removeLocalEntry(backupPath);
        return true;
    }

    if (hadDestination &&
        renameLocalEntry(backupPath, dstPath) != LocalRenameResult::Moved) {
        error = QString(QCoreApplication::translate(
                            "MainWindow",
                            "Could not move the source or restore the previous "
                            "destination. Recovery copy: %1"))
                    .arg(backupPath);
        return false;
    }

    if (renameResult != LocalRenameResult::CrossDevice) {
        error = QString(QCoreApplication::translate(
                            "MainWindow", "Could not move entry: %1 (%2)"))
                    .arg(srcPath, renameError);
        return false;
    }

    if (!copyLocalEntry(srcPath, dstPath, error, shouldCancel))
        return false;
    if (removeLocalEntry(srcPath))
        return true;
    error = QString(QCoreApplication::translate("MainWindow",
                                                "Could not delete source: %1"))
                .arg(srcPath);
    return false;
}

bool PathDepthComparator::operator()(const QString &left,
                                     const QString &right) const {
    const qsizetype leftDepth =
        QDir::fromNativeSeparators(left).count(QLatin1Char('/'));
    const qsizetype rightDepth =
        QDir::fromNativeSeparators(right).count(QLatin1Char('/'));
    if (leftDepth != rightDepth)
        return deepestFirst ? leftDepth > rightDepth : leftDepth < rightDepth;
    return deepestFirst ? left > right : left < right;
}

bool canGoUpLocally(const QString &path) {
#ifdef Q_OS_WIN
    (void)path;
    return true;
#else
    QDir directory(path);
    return directory.cdUp();
#endif
}

QString chooseLocalDrive(QWidget *anchor, const QString &currentPath) {
#ifdef Q_OS_WIN
    QMenu menu(anchor);
    const QString currentRoot = QStorageInfo(currentPath).rootPath();
    for (const QStorageInfo &volume : QStorageInfo::mountedVolumes()) {
        if (!volume.isValid() || !volume.isReady())
            continue;
        const QString root = volume.rootPath();
        const QString drive = QDir::toNativeSeparators(root);
        const QString name = volume.displayName();
        QAction *action =
            menu.addAction(name.isEmpty() || name == root
                               ? drive
                               : QStringLiteral("%1  %2").arg(drive, name));
        action->setData(root);
        action->setCheckable(true);
        action->setChecked(root.compare(currentRoot, Qt::CaseInsensitive) == 0);
    }
    const QPoint position =
        anchor ? anchor->mapToGlobal(QPoint(0, anchor->height()))
               : QCursor::pos();
    const QAction *chosen = menu.exec(position);
    return chosen ? chosen->data().toString() : QString();
#else
    (void)anchor;
    (void)currentPath;
    return QString();
#endif
}

bool isValidEntryName(const QString &name, QString *why) {
    if (name == "." || name == "..") {
        if (why) {
            *why = QCoreApplication::translate(
                "MainWindow", "Invalid name: cannot be '.' or '..'.");
        }
        return false;
    }
    if (name.contains('/') || name.contains('\\')) {
        if (why) {
            *why = QCoreApplication::translate(
                "MainWindow",
                "Invalid name: cannot contain separators ('/' or '\\\\').");
        }
        return false;
    }
    for (const QChar ch : name) {
        const ushort codePoint = ch.unicode();
        if (codePoint < 0x20u || codePoint == 0x7Fu) {
            if (why) {
                *why = QCoreApplication::translate(
                    "MainWindow",
                    "Invalid name: cannot contain control characters.");
            }
            return false;
        }
    }
    return true;
}

bool isValidLocalEntryName(const QString &name, QString *why) {
    if (!isValidEntryName(name, why))
        return false;
    // A name a server accepts can still be impossible here. On Windows ':'
    // opens an alternate data stream, CON or NUL a device, a final dot or
    // space is dropped, and REPORT~1.TXT can be another file's short name, so
    // the file would land elsewhere.
    if (!openscp::localfiles::localNameProblem(name.toStdString()).empty()) {
        if (why) {
            *why = QCoreApplication::translate(
                "MainWindow",
                "Invalid name for this computer: Windows does not allow the "
                "characters < > : \" | ? *, a final dot or space, or device "
                "names such as CON, NUL or COM1, and can read a name such as "
                "REPORT~1.TXT as another file's short name.");
        }
        return false;
    }
    return true;
}

bool promptValidEntryName(QWidget *parent, const QString &dialogTitle,
                          const QString &labelText, const QString &initialValue,
                          QString &nameOut, EntryNameTarget target) {
    bool inputAccepted = false;
    const QString candidateName =
        QInputDialog::getText(parent, dialogTitle, labelText, QLineEdit::Normal,
                              initialValue, &inputAccepted);
    if (!inputAccepted || candidateName.isEmpty())
        return false;

    QString invalidReason;
    const bool valid =
        target == EntryNameTarget::Local
            ? isValidLocalEntryName(candidateName, &invalidReason)
            : isValidEntryName(candidateName, &invalidReason);
    if (!valid) {
        UiAlerts::warning(
            parent, QCoreApplication::translate("MainWindow", "Invalid name"),
            invalidReason);
        return false;
    }
    nameOut = candidateName;
    return true;
}

QString shortRemoteError(const QString &raw, const QString &fallback) {
    QString message = raw.trimmed();
    if (message.isEmpty()) {
        return fallback;
    }

    const QString lowercase = message.toLower();
    if (lowercase.contains("permission denied")) {
        return QCoreApplication::translate("MainWindow", "Permission denied.");
    }
    if (lowercase.contains("read-only")) {
        return QCoreApplication::translate("MainWindow",
                                           "Location is read-only.");
    }
    if (lowercase.contains("no such file") || lowercase.contains("not found")) {
        return QCoreApplication::translate("MainWindow",
                                           "File or folder does not exist.");
    }
    if (lowercase.contains("timed out") || lowercase.contains("timeout")) {
        return QCoreApplication::translate("MainWindow",
                                           "Connection timed out.");
    }
    if (lowercase.contains("could not resolve") ||
        lowercase.contains("name or service not known") ||
        lowercase.contains("nodename nor servname")) {
        return QCoreApplication::translate(
            "MainWindow", "Could not resolve the server hostname.");
    }
    if (lowercase.contains("connection refused")) {
        return QCoreApplication::translate("MainWindow",
                                           "Connection refused by the server.");
    }
    if (lowercase.contains("network is unreachable") ||
        lowercase.contains("host is unreachable")) {
        return QCoreApplication::translate(
            "MainWindow", "Network unavailable or host unreachable.");
    }
    if (lowercase.contains("authentication failed") ||
        lowercase.contains("auth fail")) {
        return QCoreApplication::translate("MainWindow",
                                           "Authentication failed.");
    }

    const qsizetype newlineIndex = message.indexOf('\n');
    if (newlineIndex > 0) {
        message = message.left(newlineIndex);
    }
    message = message.simplified();
    if (message.size() > 96) {
        message = message.left(93) + "...";
    }
    return message;
}

QVector<QPair<QString, QString>> buildLocalDestinationPairsWithOverwritePrompt(
    QWidget *parent, const QVector<QFileInfo> &sources,
    const QDir &destinationDir, int *skippedCount) {
    enum class OverwritePolicy { Ask, OverwriteAll, SkipAll };
    OverwritePolicy policy = OverwritePolicy::Ask;

    int skipped = 0;
    QVector<QPair<QString, QString>> pairs;
    pairs.reserve(sources.size());

    for (const QFileInfo &sourceInfo : sources) {
        const QString targetPath =
            destinationDir.filePath(sourceInfo.fileName());
        if (QFileInfo::exists(targetPath)) {
            if (policy == OverwritePolicy::Ask) {
                const auto decision = UiAlerts::question(
                    parent,
                    QCoreApplication::translate("MainWindow", "Conflict"),
                    QCoreApplication::translate(
                        "MainWindow",
                        "“%1” already exists at destination.\nOverwrite?")
                        .arg(sourceInfo.fileName()),
                    QMessageBox::Yes | QMessageBox::No | QMessageBox::YesToAll |
                        QMessageBox::NoToAll);
                if (decision == QMessageBox::YesToAll)
                    policy = OverwritePolicy::OverwriteAll;
                else if (decision == QMessageBox::NoToAll)
                    policy = OverwritePolicy::SkipAll;

                if (decision == QMessageBox::No ||
                    policy == OverwritePolicy::SkipAll) {
                    ++skipped;
                    continue;
                }
            } else if (policy == OverwritePolicy::SkipAll) {
                ++skipped;
                continue;
            }
        }
        pairs.push_back({sourceInfo.absoluteFilePath(), targetPath});
    }

    if (skippedCount)
        *skippedCount = skipped;
    return pairs;
}
