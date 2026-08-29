// Value types shared by the transfer queue, persistence, and views.
#pragma once

#include "ConflictCoordinator.hpp"

#include <QString>

enum class TransferOperation { Copy, Move };
enum class TransferPostAction { None, DeleteSource };
enum class TransferPhase { Transfer, DeleteSource, Finished };

struct TransferBatchOptions {
    quint64 batchId = 0;
    QString sessionKey;
    TransferOperation operation = TransferOperation::Copy;
    TransferConflictPolicy conflictPolicy = TransferConflictPolicy::Ask;
    quint64 dependsOnTaskId = 0;
};

struct TransferTask {
    enum class Type {
        Upload,
        Download,
        CreateLocalDirectory,
        CreateRemoteDirectory,
        DeleteLocalFile,
        DeleteLocalDirectory,
        DeleteRemoteFile,
        DeleteRemoteDirectory
    } type = Type::Download;

    quint64 taskId = 0;
    quint64 batchId = 0;
    quint64 dependsOnTaskId = 0;
    QString sessionKey;
    QString src;
    QString dst;
    bool resumeHint = false;
    int speedLimitKBps = 0;
    int progress = 0;
    quint64 bytesDone = 0;
    quint64 bytesTotal = 0;
    double currentSpeedKBps = 0.0;
    int etaSeconds = -1;
    int attempts = 0;
    int maxAttempts = 3;
    qint64 queuedAtMs = 0;
    qint64 startedAtMs = 0;
    qint64 nextRetryAtMs = 0;
    qint64 finishedAtMs = 0;
    TransferOperation operation = TransferOperation::Copy;
    TransferConflictPolicy conflictPolicy = TransferConflictPolicy::Ask;
    TransferPostAction postAction = TransferPostAction::None;
    TransferPhase phase = TransferPhase::Transfer;
    bool restored = false;
    bool commitUncertain = false;

    enum class Status {
        Queued,
        Running,
        Paused,
        Done,
        Error,
        Canceled,
        WaitingForConnection,
        RetryWaiting,
        Skipped,
        Warning
    } status = Status::Queued;
    QString error;
};

[[nodiscard]] constexpr bool
isTerminalTransferStatus(TransferTask::Status status) {
    using Status = TransferTask::Status;
    return status == Status::Done || status == Status::Error ||
           status == Status::Canceled || status == Status::Skipped ||
           status == Status::Warning;
}
