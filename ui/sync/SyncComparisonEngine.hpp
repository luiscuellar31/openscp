// UI-independent one-way synchronization comparison and planning.
#pragma once

#include "sync/SyncTypes.hpp"

class SyncComparisonEngine final {
    public:
    [[nodiscard]] static QVector<SyncComparisonItem>
    compare(const QVector<SyncSnapshotEntry> &localSnapshot,
            const QVector<SyncSnapshotEntry> &remoteSnapshot,
            const SyncComparisonOptions &options = {});

    [[nodiscard]] static SyncExecutionPlan
    makeExecutionPlan(const QVector<SyncComparisonItem> &items,
                      const SyncComparisonOptions &options);

    [[nodiscard]] static QString
    normalizeRelativePath(const QString &relativePath);
    [[nodiscard]] static QStringList parsePatterns(const QString &text);
    [[nodiscard]] static bool globMatches(const QString &relativePath,
                                          const QString &pattern);
};
