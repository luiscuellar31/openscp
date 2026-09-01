// Pure helpers used by the path navigation widget and its unit tests.
#pragma once

#include <QString>
#include <QVector>

namespace openscpui {

enum class PathFlavor { Local, Remote };

struct PathSegment {
    QString label;
    QString target;
    qsizetype displayStart = 0;
    qsizetype displayEnd = 0;
};

[[nodiscard]] QVector<PathSegment> buildPathSegments(const QString &path,
                                                     PathFlavor flavor);

} // namespace openscpui
