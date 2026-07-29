// Versioned, atomic persistence for non-terminal transfer tasks.
#pragma once

#include "TransferTypes.hpp"

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
    [[nodiscard]] static SaveResult save(const QString &path,
                                         const QVector<TransferTask> &tasks);
};
