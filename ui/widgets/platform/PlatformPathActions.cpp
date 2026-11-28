#include "widgets/platform/PlatformPathActions.hpp"

#include "logic/common/LocalLinks.hpp"
#include "widgets/platform/PlatformPathActions_p.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHash>
#include <QProcess>
#include <QSet>
#include <QUrl>

#ifdef Q_OS_WIN
// clang-format off
#include <windows.h>
#include <objbase.h>
#include <shlobj.h>
// clang-format on

#include <vector>
#endif

namespace openscpui {

namespace {

QString unavailableError(const QString &path) {
    return QCoreApplication::translate(
               "PlatformPathActions",
               "The destination is no longer available: %1")
        .arg(path);
}

QString missingDestinationError() {
    return QCoreApplication::translate("PlatformPathActions",
                                       "No destination was provided.");
}

QString openFailedError(const QString &path) {
    return QCoreApplication::translate("PlatformPathActions",
                                       "The system could not open %1.")
        .arg(path);
}

#if defined(Q_OS_MAC) || defined(Q_OS_WIN)
QString fileManagerError(const QString &path) {
    return QCoreApplication::translate(
               "PlatformPathActions",
               "Could not open the file manager to show %1.")
        .arg(path);
}
#endif

PathActionResult failure(const QString &error) {
    PathActionResult result;
    result.succeeded = false;
    result.error = error;
    return result;
}

PathActionResult aggregate(const QStringList &errors) {
    if (errors.isEmpty())
        return PathActionResult{};
    if (errors.size() == 1)
        return failure(errors.first());
    return failure(
        QCoreApplication::translate("PlatformPathActions",
                                    "%1 destinations could not be opened.")
            .arg(errors.size()));
}

PathActionResult openLocalUrl(const QString &path) {
    if (QDesktopServices::openUrl(QUrl::fromLocalFile(path)))
        return PathActionResult{};
    return failure(openFailedError(path));
}

#if defined(Q_OS_MAC) || defined(Q_OS_WIN)
QStringList targetsOfKind(const QVector<detail::ResolvedPathTarget> &targets,
                          detail::PathActionKind kind) {
    QStringList paths;
    for (const detail::ResolvedPathTarget &target : targets) {
        if (target.kind == kind)
            paths.push_back(target.path);
    }
    return paths;
}
#endif

QString parentDirectoryOf(const QString &path) {
    return QFileInfo(path).absolutePath();
}

#ifdef Q_OS_WIN
PIDLIST_ABSOLUTE shellItem(const QString &path) {
    const QString native = QDir::toNativeSeparators(path);
    PIDLIST_ABSOLUTE item = nullptr;
    if (FAILED(::SHParseDisplayName(
            reinterpret_cast<const wchar_t *>(native.utf16()), nullptr, &item,
            0, nullptr)))
        return nullptr;
    return item;
}

// Selects the files in one Explorer window for their folder, which reuses a
// window that already shows it, instead of an explorer.exe process per file.
bool selectInExplorer(const QString &folder, const QStringList &files) {
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                      COINIT_DISABLE_OLE1DDE);
    const PIDLIST_ABSOLUTE folderItem = shellItem(folder);
    std::vector<PIDLIST_ABSOLUTE> items;
    std::vector<PCUITEMID_CHILD> children;
    for (const QString &file : files) {
        if (const PIDLIST_ABSOLUTE item = shellItem(file)) {
            items.push_back(item);
            children.push_back(::ILFindLastID(item));
        }
    }
    const bool shown = folderItem && !children.empty() &&
                       SUCCEEDED(::SHOpenFolderAndSelectItems(
                           folderItem, static_cast<UINT>(children.size()),
                           children.data(), 0));
    for (const PIDLIST_ABSOLUTE item : items)
        ::ILFree(item);
    if (folderItem)
        ::ILFree(folderItem);
    if (SUCCEEDED(com))
        ::CoUninitialize();
    return shown;
}
#endif

} // namespace

namespace detail {

ResolvedPathTarget resolveRevealTarget(const QString &requestedPath) {
    ResolvedPathTarget target;
    if (requestedPath.isEmpty())
        return target;

    target.path = requestedPath;

    const QFileInfo requested(requestedPath);
    if (requested.exists() || isLocalLink(requested)) {
        target.kind = requested.isDir() ? PathActionKind::OpenFolder
                                        : PathActionKind::Reveal;
        target.path = requested.absoluteFilePath();
        return target;
    }

    QString candidate = requested.absolutePath();
    while (!candidate.isEmpty()) {
        const QFileInfo ancestor(candidate);
        if (ancestor.isDir()) {
            target.kind = PathActionKind::OpenFolder;
            target.path = ancestor.absoluteFilePath();
            return target;
        }
        const QString parent = ancestor.absolutePath();
        if (parent == candidate)
            break;
        candidate = parent;
    }

    return target;
}

QVector<ResolvedPathTarget> planRevealBatch(const QStringList &requestedPaths) {
    QVector<ResolvedPathTarget> targets;
    QSet<QString> seen;
    for (const QString &requested : requestedPaths) {
        const ResolvedPathTarget target = resolveRevealTarget(requested);
        if (target.path.isEmpty())
            continue;
        const QString key =
            QString::number(static_cast<int>(target.kind)) + target.path;
        if (seen.contains(key))
            continue;
        seen.insert(key);
        targets.push_back(target);
    }

    QSet<QString> revealedParents;
    for (const ResolvedPathTarget &target : targets) {
        if (target.kind == PathActionKind::Reveal)
            revealedParents.insert(parentDirectoryOf(target.path));
    }

    QVector<ResolvedPathTarget> planned;
    planned.reserve(targets.size());
    for (const ResolvedPathTarget &target : targets) {
        if (target.kind == PathActionKind::OpenFolder &&
            revealedParents.contains(target.path)) {
            continue;
        }
        planned.push_back(target);
    }
    return planned;
}

QStringList
collapseTargetsToDirectories(const QVector<ResolvedPathTarget> &targets) {
    QStringList directories;
    QSet<QString> seen;
    for (const ResolvedPathTarget &target : targets) {
        QString directory;
        switch (target.kind) {
        case PathActionKind::Reveal:
            directory = parentDirectoryOf(target.path);
            break;
        case PathActionKind::OpenFolder:
            directory = target.path;
            break;
        case PathActionKind::Unavailable:
            continue;
        }
        if (directory.isEmpty() || seen.contains(directory))
            continue;
        seen.insert(directory);
        directories.push_back(directory);
    }
    return directories;
}

QVector<RevealGroup>
groupRevealedFilesByFolder(const QVector<ResolvedPathTarget> &targets) {
    QVector<RevealGroup> groups;
    QHash<QString, qsizetype> groupOfFolder;
    for (const ResolvedPathTarget &target : targets) {
        if (target.kind != PathActionKind::Reveal)
            continue;
        const QString folder = parentDirectoryOf(target.path);
        auto known = groupOfFolder.constFind(folder);
        if (known == groupOfFolder.cend()) {
            known = groupOfFolder.insert(folder, groups.size());
            groups.push_back({folder, {}});
        }
        groups[*known].files.push_back(target.path);
    }
    return groups;
}

} // namespace detail

PathActionResult PlatformPathActions::revealPath(const QString &path) {
    return revealPaths(QStringList{path});
}

PathActionResult PlatformPathActions::revealPaths(const QStringList &paths) {
    const QVector<detail::ResolvedPathTarget> targets =
        detail::planRevealBatch(paths);
    if (targets.isEmpty())
        return failure(missingDestinationError());

    QStringList errors;
    for (const detail::ResolvedPathTarget &target : targets) {
        if (!target.isResolved())
            errors.push_back(unavailableError(target.path));
    }

#if defined(Q_OS_MAC)
    const QStringList files =
        targetsOfKind(targets, detail::PathActionKind::Reveal);
    if (!files.isEmpty()) {
        QStringList arguments;
        arguments.reserve(files.size() + 1);
        arguments.push_back(QStringLiteral("-R"));
        arguments.append(files);
        if (!QProcess::startDetached(QStringLiteral("open"), arguments))
            errors.push_back(fileManagerError(files.first()));
    }
    for (const QString &folder :
         targetsOfKind(targets, detail::PathActionKind::OpenFolder)) {
        const PathActionResult opened = openLocalUrl(folder);
        if (opened.failed())
            errors.push_back(opened.error);
    }
#elif defined(Q_OS_WIN)
    for (const detail::RevealGroup &group :
         detail::groupRevealedFilesByFolder(targets)) {
        if (!selectInExplorer(group.folder, group.files))
            errors.push_back(fileManagerError(group.files.first()));
    }
    for (const QString &folder :
         targetsOfKind(targets, detail::PathActionKind::OpenFolder)) {
        const PathActionResult opened = openLocalUrl(folder);
        if (opened.failed())
            errors.push_back(opened.error);
    }
#else
    for (const QString &directory :
         detail::collapseTargetsToDirectories(targets)) {
        const PathActionResult opened = openLocalUrl(directory);
        if (opened.failed())
            errors.push_back(opened.error);
    }
#endif

    return aggregate(errors);
}

PathActionResult PlatformPathActions::openFolder(const QString &folderPath) {
    const QFileInfo info(folderPath);
    if (folderPath.isEmpty() || !info.isDir())
        return failure(unavailableError(folderPath));
    return openLocalUrl(info.absoluteFilePath());
}

PathActionResult PlatformPathActions::openFile(const QString &filePath) {
    const QFileInfo info(filePath);
    if (filePath.isEmpty() || info.isDir() ||
        (!info.exists() && !isLocalLink(info))) {
        return failure(unavailableError(filePath));
    }
    return openLocalUrl(info.absoluteFilePath());
}

} // namespace openscpui
