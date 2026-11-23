#include "logic/common/StagingStore.hpp"

#include "logic/common/AppSettings.hpp"
#include "logic/common/LocalLinks.hpp"
#include "logic/common/MainWindowSharedUtils.hpp"

#include <QDir>
#include <QFileInfo>
#include <QRegularExpression>
#include <QSettings>
#include <QUuid>

namespace openscpui {

namespace {

// OpenSCP creates the default root, named OpenSCP-Dragged. Any other root is a
// folder the user chose, which may hold their own folders named by date.
bool isOwnStagingRoot(const QString &root) {
    return QFileInfo(root).fileName() ==
           QFileInfo(defaultStagingRootPath()).fileName();
}

bool isOlderStagingBatchName(const QString &name) {
    static const QRegularExpression pattern(QStringLiteral("^\\d{8}-\\d{6}$"));
    return pattern.match(name).hasMatch();
}

} // namespace

QString newStagingBatchName() {
    return QDateTime::currentDateTimeUtc().toString(
               QStringLiteral("yyyyMMdd-HHmmss-zzz")) +
           QLatin1Char('-') +
           QUuid::createUuid().toString(QUuid::WithoutBraces).left(8);
}

bool isStagingBatchName(const QString &name) {
    // Releases before the random suffix named batches yyyyMMdd-HHmmss.
    static const QRegularExpression pattern(
        QStringLiteral("^\\d{8}-\\d{6}(?:-\\d{3}-[0-9a-f]{8})?$"));
    return pattern.match(name).hasMatch();
}

QString stagingBatchPathForCleanup(const QSettings &settings,
                                   const QString &path) {
    if (path.isEmpty())
        return {};
    const QFileInfo batch(path);
    QString canonical = batch.canonicalFilePath();
    const QString root =
        QFileInfo(effectiveStagingRootPath(settings)).canonicalFilePath();
    const QString name = QFileInfo(canonical).fileName();
    if (isLocalLink(batch) || canonical.isEmpty() || root.isEmpty() ||
        !QFileInfo(canonical).isDir() ||
        QFileInfo(canonical).absolutePath() != root ||
        !isStagingBatchName(name) ||
        (!isOwnStagingRoot(root) && isOlderStagingBatchName(name))) {
        return {};
    }
    return canonical;
}

void removeStagingBatch(const QString &batchPath) {
    // removeLocalEntry() never descends into a link, Windows junctions
    // included.
    (void)removeLocalEntry(batchPath);
    // QDir(root).rmdir(".") never removes the root on POSIX; its path does.
    const QString root = QFileInfo(batchPath).absolutePath();
    if (isOwnStagingRoot(root) &&
        QDir(root).isEmpty(QDir::NoDotAndDotDot | QDir::AllEntries |
                           QDir::Hidden | QDir::System)) {
        (void)QDir().rmdir(root);
    }
}

QStringList stagingRootsForStartupCleanup(const QSettings &settings) {
    const QString root = effectiveStagingRootPath(settings);
    QStringList roots{root};
#ifdef Q_OS_LINUX
    const QString previousDefault =
        QDir(QDir::homePath())
            .filePath(QStringLiteral("Downloads/OpenSCP-Dragged"));
    if (settings.value(settingskeys::kStagingRoot).toString().isEmpty() &&
        QDir::cleanPath(previousDefault) != QDir::cleanPath(root))
        roots << previousDefault;
#endif
    return roots;
}

int removeStagingBatchesOlderThan(const QString &root,
                                  const QDateTime &cutoff) {
    int removed = 0;
    const bool ownRoot = isOwnStagingRoot(root);
    const auto entries = QDir(root).entryInfoList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::NoSymLinks | QDir::Hidden);
    for (const QFileInfo &entry : entries) {
        if (isLocalLink(entry) || !isStagingBatchName(entry.fileName()) ||
            (!ownRoot && isOlderStagingBatchName(entry.fileName())))
            continue;
        const QDateTime modified = entry.lastModified();
        if (modified.isValid() && modified < cutoff &&
            removeLocalEntry(entry.absoluteFilePath())) {
            ++removed;
        }
    }
    return removed;
}

} // namespace openscpui
