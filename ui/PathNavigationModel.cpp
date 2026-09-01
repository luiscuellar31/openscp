#include "PathNavigationModel.hpp"

#include "RemotePath.hpp"

#include <QDir>
#include <QFileInfo>
#include <QStringList>

namespace openscpui {
namespace {

void appendDescendants(QVector<PathSegment> &segments, QString target,
                       const QStringList &parts, qsizetype firstPart = 0) {
    for (qsizetype index = firstPart; index < parts.size(); ++index) {
        if (!target.endsWith(QLatin1Char('/')))
            target += QLatin1Char('/');
        const qsizetype displayStart = target.size();
        target += parts.at(index);
        segments.push_back(
            {parts.at(index), target, displayStart, target.size()});
    }
}

QVector<PathSegment> buildRemoteSegments(const QString &path) {
    const QString normalized = normalizeRemotePath(path);
    QVector<PathSegment> segments{
        {QStringLiteral("/"), QStringLiteral("/"), 0, 1}};
    appendDescendants(segments, QStringLiteral("/"),
                      normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts));
    return segments;
}

QVector<PathSegment> buildLocalSegments(const QString &path) {
    QString normalized = QDir::fromNativeSeparators(path.trimmed());
    if (normalized.isEmpty())
        normalized = QDir::homePath();
    if (!QFileInfo(normalized).isAbsolute())
        normalized = QDir::current().absoluteFilePath(normalized);
    normalized = QDir::cleanPath(normalized);

    QVector<PathSegment> segments;
#ifdef Q_OS_WIN
    if (normalized.startsWith(QStringLiteral("//"))) {
        const QStringList parts =
            normalized.mid(2).split(QLatin1Char('/'), Qt::SkipEmptyParts);
        if (parts.size() >= 2) {
            const QString shareRoot =
                QStringLiteral("//%1/%2").arg(parts.at(0), parts.at(1));
            segments.push_back({shareRoot, shareRoot, 0, shareRoot.size()});
            appendDescendants(segments, shareRoot, parts, 2);
            return segments;
        }
    }
    if (normalized.size() >= 2 && normalized.at(1) == QLatin1Char(':')) {
        const QString drive = normalized.left(2);
        const QString driveRoot = drive + QLatin1Char('/');
        segments.push_back({drive, driveRoot, 0, drive.size()});
        appendDescendants(
            segments, driveRoot,
            normalized.mid(2).split(QLatin1Char('/'), Qt::SkipEmptyParts));
        return segments;
    }
#endif

    segments.push_back({QStringLiteral("/"), QStringLiteral("/"), 0, 1});
    appendDescendants(segments, QStringLiteral("/"),
                      normalized.split(QLatin1Char('/'), Qt::SkipEmptyParts));
    return segments;
}

} // namespace

QVector<PathSegment> buildPathSegments(const QString &path, PathFlavor flavor) {
    return flavor == PathFlavor::Remote ? buildRemoteSegments(path)
                                        : buildLocalSegments(path);
}

} // namespace openscpui
