// Versioned, atomic persistence for non-terminal transfer tasks.
#pragma once

#include "logic/transfers/TransferTypes.hpp"

#include <QString>
#include <QVector>

class TransferQueuePersistence final {
    public:
    enum class LoadStatus {
        NotFound,
        Loaded,
        IoError,
        Corrupt,
        UnsupportedSchema,
    };

    struct LoadResult {
        LoadStatus status = LoadStatus::NotFound;
        QVector<TransferTask> tasks;
        QString warning;

        [[nodiscard]] bool succeeded() const {
            return status == LoadStatus::NotFound ||
                   status == LoadStatus::Loaded;
        }
        [[nodiscard]] bool shouldBlockWrites() const {
            return status == LoadStatus::Corrupt ||
                   status == LoadStatus::UnsupportedSchema;
        }
    };

    struct SaveResult {
        bool succeeded = false;
        QString warning;
    };

    [[nodiscard]] static LoadResult load(const QString &path,
                                         const QString &currentSessionKey);
    // Finished tasks are not saved, except an upload whose source cleanup is
    // still pending.
    [[nodiscard]] static bool isPersisted(const TransferTask &task);
    [[nodiscard]] static SaveResult save(const QString &path,
                                         const QVector<TransferTask> &tasks);
};
