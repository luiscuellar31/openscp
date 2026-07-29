#include "SyncComparisonEngine.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QHash>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <limits>
#include <utility>

namespace {

QString normalizedPattern(QString pattern) {
    pattern = QDir::fromNativeSeparators(pattern.trimmed());
    while (pattern.startsWith(QStringLiteral("./")))
        pattern.remove(0, 2);
    while (pattern.startsWith(QLatin1Char('/')))
        pattern.remove(0, 1);
    while (pattern.contains(QStringLiteral("//")))
        pattern.replace(QStringLiteral("//"), QStringLiteral("/"));
    return pattern;
}

QString escapedGlobBody(const QString &pattern) {
    QString expression;
    expression.reserve(pattern.size() * 2);
    for (qsizetype index = 0; index < pattern.size(); ++index) {
        const QChar current = pattern.at(index);
        if (current == QLatin1Char('*')) {
            const bool isDouble = index + 1 < pattern.size() &&
                                  pattern.at(index + 1) == QLatin1Char('*');
            if (!isDouble) {
                expression += QStringLiteral("[^/]*");
                continue;
            }
            ++index;
            const bool followedBySlash =
                index + 1 < pattern.size() &&
                pattern.at(index + 1) == QLatin1Char('/');
            if (followedBySlash) {
                ++index;
                expression += QStringLiteral("(?:.*/)?");
            } else {
                expression += QStringLiteral(".*");
            }
            continue;
        }
        if (current == QLatin1Char('?')) {
            expression += QStringLiteral("[^/]");
            continue;
        }
        expression += QRegularExpression::escape(QString(current));
    }
    return expression;
}

QRegularExpression compiledGlob(QString pattern) {
    pattern = normalizedPattern(std::move(pattern));
    if (pattern.isEmpty())
        return {};

    const bool containsSlash = pattern.contains(QLatin1Char('/'));
    const bool trailingTree = pattern.endsWith(QStringLiteral("/**"));
    if (trailingTree)
        pattern.chop(3);

    QString expression = escapedGlobBody(pattern);
    if (trailingTree)
        expression += QStringLiteral("(?:/.*)?");
    if (containsSlash) {
        expression.prepend(QLatin1Char('^'));
        expression.append(QLatin1Char('$'));
    } else {
        expression.prepend(QStringLiteral("(?:^|/)"));
        expression.append(QStringLiteral("(?:$|/)"));
    }
    return QRegularExpression(expression,
                              QRegularExpression::DontCaptureOption);
}

class CompiledGlobSet {
    public:
    explicit CompiledGlobSet(const QStringList &patterns) {
        expressions_.reserve(patterns.size());
        for (const QString &pattern : patterns) {
            QRegularExpression expression = compiledGlob(pattern);
            if (expression.isValid() && !expression.pattern().isEmpty())
                expressions_.push_back(std::move(expression));
        }
    }

    [[nodiscard]] bool matches(const QString &relativePath) const {
        return std::any_of(expressions_.cbegin(), expressions_.cend(),
                           [&](const QRegularExpression &expression) {
                               return expression.match(relativePath).hasMatch();
                           });
    }

    private:
    QVector<QRegularExpression> expressions_;
};

bool isHiddenPath(const QString &relativePath) {
    const auto segments =
        relativePath.split(QLatin1Char('/'), Qt::SkipEmptyParts);
    return std::any_of(
        segments.cbegin(), segments.cend(), [](const QString &segment) {
            return segment.size() > 1 ? segment.startsWith(QLatin1Char('.'))
                                      : segment == QStringLiteral(".");
        });
}

QString parentRelativePath(const QString &relativePath) {
    const qsizetype separatorPosition =
        relativePath.lastIndexOf(QLatin1Char('/'));
    return separatorPosition < 0 ? QString()
                                 : relativePath.left(separatorPosition);
}

int pathDepth(const QString &relativePath) {
    return relativePath.isEmpty() ? 0
                                  : relativePath.count(QLatin1Char('/')) + 1;
}

bool isPathInside(const QString &path, const QString &directory) {
    return path.size() > directory.size() && path.startsWith(directory) &&
           path.at(directory.size()) == QLatin1Char('/');
}

bool isNewerBeyondTolerance(qint64 candidate, qint64 baseline,
                            qint64 tolerance) {
    if (candidate <= baseline)
        return false;
    return static_cast<long double>(candidate) -
               static_cast<long double>(baseline) >
           static_cast<long double>(std::max<qint64>(0, tolerance));
}

using EntryIndex = QHash<QString, SyncSnapshotEntry>;

EntryIndex buildEntryIndex(const QVector<SyncSnapshotEntry> &snapshot) {
    EntryIndex index;
    index.reserve(snapshot.size());
    for (const SyncSnapshotEntry &original : snapshot) {
        const QString normalized =
            SyncComparisonEngine::normalizeRelativePath(original.relativePath);
        if (normalized.isEmpty())
            continue;

        SyncSnapshotEntry entry = original;
        entry.relativePath = normalized;
        auto existing = index.find(normalized);
        if (existing == index.end()) {
            index.insert(normalized, std::move(entry));
            continue;
        }
        existing->metadataReliable = false;
        existing->checksumAlgorithm.clear();
        existing->checksum.clear();
        if (existing->type != entry.type)
            existing->type = SyncEntryType::Other;
    }
    return index;
}

std::optional<bool> equalComparableChecksums(const SyncSnapshotEntry &source,
                                             const SyncSnapshotEntry &dest) {
    if (source.checksumAlgorithm.isEmpty() ||
        dest.checksumAlgorithm.isEmpty() || source.checksum.isEmpty() ||
        dest.checksum.isEmpty() ||
        source.checksumAlgorithm.compare(dest.checksumAlgorithm,
                                         Qt::CaseInsensitive) != 0) {
        return std::nullopt;
    }
    return source.checksum == dest.checksum;
}

SyncComparisonItem classifyItem(const QString &relativePath,
                                std::optional<SyncSnapshotEntry> source,
                                std::optional<SyncSnapshotEntry> destination,
                                const SyncComparisonOptions &options) {
    SyncComparisonItem item;
    item.relativePath = relativePath;
    item.source = std::move(source);
    item.destination = std::move(destination);

    if (item.source && !item.destination) {
        if (item.source->type == SyncEntryType::Directory) {
            item.action = SyncAction::CreateDirectory;
            item.reason = QCoreApplication::translate(
                "SyncDialog", "Folder exists only in the source");
        } else if (item.source->type == SyncEntryType::File) {
            item.action = SyncAction::Copy;
            item.reason = QCoreApplication::translate(
                "SyncDialog", "File exists only in the source");
        } else {
            item.action = SyncAction::Unknown;
            item.reason = QCoreApplication::translate(
                "SyncDialog", "This source entry type cannot be copied safely");
        }
        item.selected = isSyncActionable(item.action);
        return item;
    }

    if (!item.source && item.destination) {
        if (!options.mirror) {
            item.action = SyncAction::Keep;
            item.reason = QCoreApplication::translate(
                "SyncDialog", "Only in the destination; mirror is disabled");
        } else if (item.destination->type == SyncEntryType::Directory) {
            item.action = SyncAction::DeleteDirectory;
            item.reason = QCoreApplication::translate(
                "SyncDialog", "Only in the destination; mirror will delete it");
        } else if (item.destination->type == SyncEntryType::File ||
                   item.destination->type == SyncEntryType::SymbolicLink) {
            item.action = SyncAction::DeleteFile;
            item.reason = QCoreApplication::translate(
                "SyncDialog", "Only in the destination; mirror will delete it");
        } else {
            item.action = SyncAction::Unknown;
            item.reason = QCoreApplication::translate(
                "SyncDialog",
                "This destination entry type cannot be deleted safely");
        }
        item.selected = isSyncActionable(item.action);
        return item;
    }

    if (!item.source || !item.destination) {
        item.action = SyncAction::Unknown;
        item.reason = QCoreApplication::translate(
            "SyncDialog", "Snapshot entry is incomplete");
        return item;
    }

    const SyncSnapshotEntry &sourceEntry = *item.source;
    const SyncSnapshotEntry &destinationEntry = *item.destination;
    if (sourceEntry.type != destinationEntry.type) {
        item.action = SyncAction::Conflict;
        item.reason = QCoreApplication::translate(
            "SyncDialog", "Source and destination types are different");
        return item;
    }
    if (!sourceEntry.metadataReliable || !destinationEntry.metadataReliable) {
        item.action = SyncAction::Unknown;
        item.reason = QCoreApplication::translate(
            "SyncDialog", "Metadata is not reliable enough to compare");
        return item;
    }
    if (sourceEntry.type == SyncEntryType::Directory) {
        item.action = SyncAction::Keep;
        item.reason = QCoreApplication::translate(
            "SyncDialog", "Folder exists on both sides");
        return item;
    }
    if (sourceEntry.type != SyncEntryType::File) {
        item.action = SyncAction::Unknown;
        item.reason = QCoreApplication::translate(
            "SyncDialog", "This entry type is not synchronized automatically");
        return item;
    }

    const auto checksumEquality =
        equalComparableChecksums(sourceEntry, destinationEntry);
    if (checksumEquality && *checksumEquality) {
        item.action = SyncAction::Keep;
        item.reason =
            QCoreApplication::translate("SyncDialog", "Checksums match");
        return item;
    }
    if (!sourceEntry.size || !destinationEntry.size ||
        !sourceEntry.modifiedMs || !destinationEntry.modifiedMs) {
        item.action = SyncAction::Unknown;
        item.reason = QCoreApplication::translate(
            "SyncDialog", "Size or modification date is unknown");
        return item;
    }

    const qint64 tolerance = std::max<qint64>(0, options.modifiedToleranceMs);
    if (isNewerBeyondTolerance(*destinationEntry.modifiedMs,
                               *sourceEntry.modifiedMs, tolerance)) {
        item.action = SyncAction::Conflict;
        item.reason = QCoreApplication::translate(
            "SyncDialog", "Destination is newer than the source");
        return item;
    }
    if (*sourceEntry.size != *destinationEntry.size) {
        item.action = SyncAction::Copy;
        item.reason = QCoreApplication::translate("SyncDialog",
                                                  "File sizes are different");
        item.selected = true;
        return item;
    }
    if (isNewerBeyondTolerance(*sourceEntry.modifiedMs,
                               *destinationEntry.modifiedMs, tolerance)) {
        item.action = SyncAction::Copy;
        item.reason = QCoreApplication::translate(
            "SyncDialog", "Source is newer than the destination");
        item.selected = true;
        return item;
    }
    if (checksumEquality && !*checksumEquality) {
        item.action = SyncAction::Conflict;
        item.reason = QCoreApplication::translate(
            "SyncDialog",
            "Checksums differ but timestamps do not identify a newer source");
        return item;
    }

    item.action = SyncAction::Keep;
    item.reason = QCoreApplication::translate(
        "SyncDialog", "Size and modification date match");
    return item;
}

} // namespace

QString
SyncComparisonEngine::normalizeRelativePath(const QString &relativePath) {
    if (relativePath.contains(QChar(0)))
        return {};

    QString normalized = QDir::fromNativeSeparators(relativePath);
    while (normalized.startsWith(QStringLiteral("./")))
        normalized.remove(0, 2);
    while (normalized.startsWith(QLatin1Char('/')))
        normalized.remove(0, 1);
    normalized = QDir::cleanPath(normalized);
    if (normalized.isEmpty() || normalized == QStringLiteral(".") ||
        normalized == QStringLiteral("..") ||
        normalized.startsWith(QStringLiteral("../"))) {
        return {};
    }
    return normalized;
}

QStringList SyncComparisonEngine::parsePatterns(const QString &text) {
    const QStringList candidates = text.split(
        QRegularExpression(QStringLiteral("[;\\r\\n]+")), Qt::SkipEmptyParts);
    QStringList patterns;
    QSet<QString> seen;
    for (const QString &candidate : candidates) {
        const QString pattern = normalizedPattern(candidate);
        if (pattern.isEmpty() || seen.contains(pattern))
            continue;
        seen.insert(pattern);
        patterns.push_back(pattern);
    }
    return patterns;
}

bool SyncComparisonEngine::globMatches(const QString &relativePath,
                                       const QString &rawPattern) {
    const QString path = normalizeRelativePath(relativePath);
    if (path.isEmpty())
        return false;
    const QRegularExpression regex = compiledGlob(rawPattern);
    return regex.isValid() && !regex.pattern().isEmpty() &&
           regex.match(path).hasMatch();
}

QVector<SyncComparisonItem>
SyncComparisonEngine::compare(const QVector<SyncSnapshotEntry> &localSnapshot,
                              const QVector<SyncSnapshotEntry> &remoteSnapshot,
                              const SyncComparisonOptions &options) {
    const EntryIndex local = buildEntryIndex(localSnapshot);
    const EntryIndex remote = buildEntryIndex(remoteSnapshot);

    QSet<QString> allPaths;
    allPaths.reserve(local.size() + remote.size());
    for (auto iterator = local.cbegin(); iterator != local.cend(); ++iterator)
        allPaths.insert(iterator.key());
    for (auto iterator = remote.cbegin(); iterator != remote.cend();
         ++iterator) {
        allPaths.insert(iterator.key());
    }

    QStringList includes = options.includePatterns;
    if (includes.isEmpty())
        includes.push_back(QStringLiteral("**"));
    const CompiledGlobSet includeGlobs(includes);
    const CompiledGlobSet excludeGlobs(options.excludePatterns);

    QSet<QString> visiblePaths;
    visiblePaths.reserve(allPaths.size());
    for (const QString &path : allPaths) {
        if ((!options.includeHidden && isHiddenPath(path)) ||
            excludeGlobs.matches(path) || !includeGlobs.matches(path)) {
            continue;
        }
        visiblePaths.insert(path);
        QString parent = parentRelativePath(path);
        while (!parent.isEmpty()) {
            if ((!options.includeHidden && isHiddenPath(parent)) ||
                excludeGlobs.matches(parent)) {
                break;
            }
            visiblePaths.insert(parent);
            parent = parentRelativePath(parent);
        }
    }

    QStringList sortedPaths = visiblePaths.values();
    std::sort(sortedPaths.begin(), sortedPaths.end(),
              [](const QString &left, const QString &right) {
                  return left.compare(right, Qt::CaseSensitive) < 0;
              });

    QVector<SyncComparisonItem> result;
    result.reserve(sortedPaths.size());
    for (const QString &path : sortedPaths) {
        const auto localEntry = local.constFind(path);
        const auto remoteEntry = remote.constFind(path);
        if (localEntry == local.cend() && remoteEntry == remote.cend())
            continue;

        std::optional<SyncSnapshotEntry> source;
        std::optional<SyncSnapshotEntry> destination;
        if (options.direction == SyncDirection::LocalToRemote) {
            if (localEntry != local.cend())
                source = *localEntry;
            if (remoteEntry != remote.cend())
                destination = *remoteEntry;
        } else {
            if (remoteEntry != remote.cend())
                source = *remoteEntry;
            if (localEntry != local.cend())
                destination = *localEntry;
        }
        result.push_back(classifyItem(path, std::move(source),
                                      std::move(destination), options));
    }
    return result;
}

SyncExecutionPlan SyncComparisonEngine::makeExecutionPlan(
    const QVector<SyncComparisonItem> &items,
    const SyncComparisonOptions &options) {
    SyncExecutionPlan plan;
    plan.direction = options.direction;
    plan.mirror = options.mirror;

    QSet<QString> destinationDirectories;
    QSet<QString> directoriesToCreate;
    QSet<QString> copiedPaths;
    QSet<QString> deletedPaths;
    for (const SyncComparisonItem &item : items) {
        if (item.destination &&
            item.destination->type == SyncEntryType::Directory) {
            destinationDirectories.insert(item.relativePath);
        }
    }

    for (const SyncComparisonItem &item : items) {
        if (!item.selected || !isSyncActionable(item.action))
            continue;

        const QString path = normalizeRelativePath(item.relativePath);
        if (path.isEmpty()) {
            plan.warnings.push_back(QCoreApplication::translate(
                "SyncDialog",
                "An unsafe relative path was omitted from the plan"));
            continue;
        }
        if (item.action == SyncAction::CreateDirectory) {
            directoriesToCreate.insert(path);
            QString parent = parentRelativePath(path);
            while (!parent.isEmpty()) {
                if (!destinationDirectories.contains(parent))
                    directoriesToCreate.insert(parent);
                parent = parentRelativePath(parent);
            }
            continue;
        }
        if (item.action == SyncAction::Copy) {
            if (copiedPaths.contains(path))
                continue;
            copiedPaths.insert(path);
            SyncCopyOperation copy;
            copy.relativePath = path;
            if (item.source)
                copy.size = item.source->size;
            copy.overwritesExisting = item.destination.has_value();
            plan.copies.push_back(std::move(copy));

            QString parent = parentRelativePath(path);
            while (!parent.isEmpty()) {
                if (!destinationDirectories.contains(parent))
                    directoriesToCreate.insert(parent);
                parent = parentRelativePath(parent);
            }
            continue;
        }
        if (item.action == SyncAction::DeleteFile ||
            item.action == SyncAction::DeleteDirectory) {
            if (deletedPaths.contains(path))
                continue;
            deletedPaths.insert(path);
            plan.deletes.push_back(
                {path, item.action == SyncAction::DeleteDirectory
                           ? SyncEntryType::Directory
                           : (item.destination ? item.destination->type
                                               : SyncEntryType::File)});
        }
    }

    QVector<SyncDeleteOperation> safeDeletes;
    safeDeletes.reserve(plan.deletes.size());
    for (const SyncDeleteOperation &deletion : std::as_const(plan.deletes)) {
        bool conflictsWithIncoming = false;
        if (deletion.type == SyncEntryType::Directory) {
            for (const SyncCopyOperation &copy : std::as_const(plan.copies)) {
                if (isPathInside(copy.relativePath, deletion.relativePath)) {
                    conflictsWithIncoming = true;
                    break;
                }
            }
            if (!conflictsWithIncoming) {
                for (const QString &directory :
                     std::as_const(directoriesToCreate)) {
                    if (directory == deletion.relativePath ||
                        isPathInside(directory, deletion.relativePath)) {
                        conflictsWithIncoming = true;
                        break;
                    }
                }
            }
        }
        if (conflictsWithIncoming) {
            plan.warnings.push_back(
                QCoreApplication::translate("SyncDialog",
                                            "A mirror deletion that contains "
                                            "incoming items was omitted: ") +
                deletion.relativePath);
        } else {
            safeDeletes.push_back(deletion);
        }
    }
    plan.deletes = std::move(safeDeletes);

    plan.directoriesToCreate = directoriesToCreate.values();
    std::sort(plan.directoriesToCreate.begin(), plan.directoriesToCreate.end(),
              [](const QString &left, const QString &right) {
                  const int leftDepth = pathDepth(left);
                  const int rightDepth = pathDepth(right);
                  return leftDepth != rightDepth ? leftDepth < rightDepth
                                                 : left < right;
              });
    std::sort(
        plan.copies.begin(), plan.copies.end(),
        [](const SyncCopyOperation &left, const SyncCopyOperation &right) {
            return left.relativePath < right.relativePath;
        });
    std::sort(
        plan.deletes.begin(), plan.deletes.end(),
        [](const SyncDeleteOperation &left, const SyncDeleteOperation &right) {
            const int leftDepth = pathDepth(left.relativePath);
            const int rightDepth = pathDepth(right.relativePath);
            if (leftDepth != rightDepth)
                return leftDepth > rightDepth;
            if (left.type != right.type)
                return left.type != SyncEntryType::Directory;
            return left.relativePath < right.relativePath;
        });

    for (const SyncCopyOperation &copy : std::as_const(plan.copies)) {
        if (!copy.size) {
            ++plan.unknownSizeCopies;
        } else if (*copy.size >
                   std::numeric_limits<quint64>::max() - plan.knownCopyBytes) {
            plan.knownCopyBytes = std::numeric_limits<quint64>::max();
        } else {
            plan.knownCopyBytes += *copy.size;
        }
    }
    plan.requiresMirrorConfirmation = plan.mirror && !plan.deletes.isEmpty();
    return plan;
}
