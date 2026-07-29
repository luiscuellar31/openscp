// Domain types shared by one-way synchronization logic and widgets.
#pragma once

#include <QByteArray>
#include <QMetaType>
#include <QString>
#include <QStringList>
#include <QVector>

#include <optional>

enum class SyncDirection {
    LocalToRemote = 0,
    RemoteToLocal = 1,
};

enum class SyncEntryType {
    File = 0,
    Directory = 1,
    SymbolicLink = 2,
    Other = 3,
};

struct SyncSnapshotEntry {
    QString relativePath;
    SyncEntryType type = SyncEntryType::File;
    std::optional<quint64> size;
    std::optional<qint64> modifiedMs;
    bool metadataReliable = true;
    QString checksumAlgorithm;
    QByteArray checksum;
};

enum class SyncAction {
    Keep = 0,
    Copy = 1,
    CreateDirectory = 2,
    DeleteFile = 3,
    DeleteDirectory = 4,
    Conflict = 5,
    Unknown = 6,
};

[[nodiscard]] constexpr bool isSyncActionable(SyncAction action) {
    return action == SyncAction::Copy ||
           action == SyncAction::CreateDirectory ||
           action == SyncAction::DeleteFile ||
           action == SyncAction::DeleteDirectory;
}

struct SyncComparisonOptions {
    SyncDirection direction = SyncDirection::LocalToRemote;
    QStringList includePatterns{QStringLiteral("**")};
    QStringList excludePatterns;
    bool includeHidden = false;
    bool mirror = false;
    qint64 modifiedToleranceMs = 2000;
};

struct SyncComparisonItem {
    QString relativePath;
    std::optional<SyncSnapshotEntry> source;
    std::optional<SyncSnapshotEntry> destination;
    SyncAction action = SyncAction::Keep;
    QString reason;
    bool selected = false;
};

struct SyncCopyOperation {
    QString relativePath;
    std::optional<quint64> size;
    bool overwritesExisting = false;
};

struct SyncDeleteOperation {
    QString relativePath;
    SyncEntryType type = SyncEntryType::File;
};

struct SyncExecutionPlan {
    SyncDirection direction = SyncDirection::LocalToRemote;
    bool mirror = false;
    QVector<QString> directoriesToCreate;
    QVector<SyncCopyOperation> copies;
    QVector<SyncDeleteOperation> deletes;
    quint64 knownCopyBytes = 0;
    qsizetype unknownSizeCopies = 0;
    bool requiresMirrorConfirmation = false;
    QStringList warnings;

    [[nodiscard]] bool empty() const {
        return directoriesToCreate.isEmpty() && copies.isEmpty() &&
               deletes.isEmpty();
    }
};

Q_DECLARE_METATYPE(SyncExecutionPlan)
