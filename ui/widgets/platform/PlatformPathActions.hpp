#pragma once

#include <QString>
#include <QStringList>

namespace openscpui {

struct PathActionResult {
    bool succeeded = true;
    QString error;

    [[nodiscard]] bool failed() const noexcept { return !succeeded; }
};

class PlatformPathActions {
    public:
    [[nodiscard]] static PathActionResult revealPath(const QString &path);
    [[nodiscard]] static PathActionResult revealPaths(const QStringList &paths);
    [[nodiscard]] static PathActionResult openFolder(const QString &folderPath);
    [[nodiscard]] static PathActionResult openFile(const QString &filePath);
};

} // namespace openscpui
