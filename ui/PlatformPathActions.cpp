#include "PlatformPathActions.hpp"

#include "PlatformPathActions_p.hpp"

#include <QCoreApplication>
#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QSet>
#include <QUrl>

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

QString fileManagerError(const QString &path) {
    return QCoreApplication::translate(
               "PlatformPathActions",
               "Could not open the file manager to show %1.")
        .arg(path);
}

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

} // namespace

namespace detail {

ResolvedPathTarget resolveRevealTarget(const QString &requestedPath) {
    ResolvedPathTarget target;
    if (requestedPath.isEmpty())
        return target;

    target.path = requestedPath;

    const QFileInfo requested(requestedPath);
    if (requested.exists() || requested.isSymLink()) {
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
    for (const QString &file :
         targetsOfKind(targets, detail::PathActionKind::Reveal)) {
        QProcess explorer;
        explorer.setProgram(QStringLiteral("explorer.exe"));
        explorer.setNativeArguments(QStringLiteral("/select,\"") +
                                    QDir::toNativeSeparators(file) +
                                    QStringLiteral("\""));
        if (!explorer.startDetached())
            errors.push_back(fileManagerError(file));
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
        (!info.exists() && !info.isSymLink())) {
        return failure(unavailableError(filePath));
    }
    return openLocalUrl(info.absoluteFilePath());
}

} // namespace openscpui
