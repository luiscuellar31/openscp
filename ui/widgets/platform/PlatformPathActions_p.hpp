#pragma once

#include <QString>
#include <QStringList>
#include <QVector>

namespace openscpui::detail {

enum class PathActionKind { Unavailable, Reveal, OpenFolder };

struct ResolvedPathTarget {
    PathActionKind kind = PathActionKind::Unavailable;
    QString path;

    [[nodiscard]] bool isResolved() const noexcept {
        return kind != PathActionKind::Unavailable;
    }
};

[[nodiscard]] ResolvedPathTarget
resolveRevealTarget(const QString &requestedPath);

[[nodiscard]] QVector<ResolvedPathTarget>
planRevealBatch(const QStringList &requestedPaths);

[[nodiscard]] QStringList
collapseTargetsToDirectories(const QVector<ResolvedPathTarget> &targets);

struct RevealGroup {
    QString folder;
    QStringList files;
};

// The files to reveal, grouped by folder in the order each folder first
// appears.
[[nodiscard]] QVector<RevealGroup>
groupRevealedFilesByFolder(const QVector<ResolvedPathTarget> &targets);

} // namespace openscpui::detail
