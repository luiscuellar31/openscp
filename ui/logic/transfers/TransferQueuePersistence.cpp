#include "logic/transfers/TransferQueuePersistence.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QJsonParseError>
#include <QSaveFile>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>
#include <optional>

namespace {

using Policy = TransferConflictPolicy;
using Status = TransferTask::Status;

constexpr qint64 kMaxPersistenceBytes = 16 * 1024 * 1024;
constexpr qsizetype kMaxPersistedTasks = 100'000;

QString translate(const char *text) {
    return QCoreApplication::translate("TransferManager", text);
}

QString policyName(Policy policy) {
    switch (policy) {
    case Policy::Overwrite:
        return QStringLiteral("overwrite");
    case Policy::Skip:
        return QStringLiteral("skip");
    case Policy::Resume:
        return QStringLiteral("resume");
    case Policy::Rename:
        return QStringLiteral("rename");
    case Policy::NewerOnly:
        return QStringLiteral("newer-only");
    case Policy::Ask:
        return QStringLiteral("ask");
    }
    return QStringLiteral("ask");
}

std::optional<Policy> policyFromName(const QString &name) {
    if (name == QStringLiteral("overwrite"))
        return Policy::Overwrite;
    if (name == QStringLiteral("skip"))
        return Policy::Skip;
    if (name == QStringLiteral("resume"))
        return Policy::Resume;
    if (name == QStringLiteral("rename"))
        return Policy::Rename;
    if (name == QStringLiteral("newer-only"))
        return Policy::NewerOnly;
    if (name == QStringLiteral("ask"))
        return Policy::Ask;
    return std::nullopt;
}

QString operationName(TransferOperation operation) {
    return operation == TransferOperation::Move ? QStringLiteral("move")
                                                : QStringLiteral("copy");
}

std::optional<TransferOperation> operationFromName(const QString &name) {
    if (name == QStringLiteral("copy"))
        return TransferOperation::Copy;
    if (name == QStringLiteral("move"))
        return TransferOperation::Move;
    return std::nullopt;
}

QString phaseName(TransferPhase phase) {
    switch (phase) {
    case TransferPhase::DeleteSource:
        return QStringLiteral("delete-source");
    case TransferPhase::Finished:
        return QStringLiteral("finished");
    case TransferPhase::Transfer:
        return QStringLiteral("transfer");
    }
    return QStringLiteral("transfer");
}

std::optional<TransferPhase> phaseFromName(const QString &name) {
    if (name == QStringLiteral("delete-source"))
        return TransferPhase::DeleteSource;
    if (name == QStringLiteral("finished"))
        return TransferPhase::Finished;
    if (name == QStringLiteral("transfer"))
        return TransferPhase::Transfer;
    return std::nullopt;
}

std::optional<quint64> parseTaskId(const QJsonValue &value) {
    bool parsedSuccessfully = false;
    quint64 parsed = 0;
    if (value.isString()) {
        parsed = value.toString().toULongLong(&parsedSuccessfully);
    } else if (value.isDouble()) {
        const double number = value.toDouble(-1);
        // JSON numbers are IEEE-754 doubles. Accept legacy numeric IDs only
        // while they are positive, integral and exactly representable.
        constexpr double maxExactJsonInteger = 9007199254740991.0;
        if (std::isfinite(number) && number > 0 &&
            number <= maxExactJsonInteger && std::trunc(number) == number) {
            parsed = static_cast<quint64>(number);
            parsedSuccessfully = true;
        }
    }
    constexpr quint64 maxRestorableId =
        static_cast<quint64>((std::numeric_limits<qint64>::max)()) - 1;
    return parsedSuccessfully && parsed != 0 && parsed <= maxRestorableId
               ? std::optional<quint64>(parsed)
               : std::nullopt;
}

QString typeName(TransferTask::Type type) {
    switch (type) {
    case TransferTask::Type::Upload:
        return QStringLiteral("upload");
    case TransferTask::Type::Download:
        return QStringLiteral("download");
    case TransferTask::Type::CreateLocalDirectory:
        return QStringLiteral("local-directory");
    case TransferTask::Type::CreateRemoteDirectory:
        return QStringLiteral("remote-directory");
    case TransferTask::Type::DeleteLocalFile:
        return QStringLiteral("delete-local-file");
    case TransferTask::Type::DeleteLocalDirectory:
        return QStringLiteral("delete-local-directory");
    case TransferTask::Type::DeleteRemoteFile:
        return QStringLiteral("delete-remote-file");
    case TransferTask::Type::DeleteRemoteDirectory:
        return QStringLiteral("delete-remote-directory");
    }
    return QStringLiteral("download");
}

std::optional<TransferTask::Type> typeFromName(const QString &name) {
    if (name == QStringLiteral("upload"))
        return TransferTask::Type::Upload;
    if (name == QStringLiteral("local-directory"))
        return TransferTask::Type::CreateLocalDirectory;
    if (name == QStringLiteral("remote-directory"))
        return TransferTask::Type::CreateRemoteDirectory;
    if (name == QStringLiteral("delete-local-file"))
        return TransferTask::Type::DeleteLocalFile;
    if (name == QStringLiteral("delete-local-directory"))
        return TransferTask::Type::DeleteLocalDirectory;
    if (name == QStringLiteral("delete-remote-file"))
        return TransferTask::Type::DeleteRemoteFile;
    if (name == QStringLiteral("delete-remote-directory"))
        return TransferTask::Type::DeleteRemoteDirectory;
    if (name == QStringLiteral("download"))
        return TransferTask::Type::Download;
    return std::nullopt;
}

std::optional<TransferTask> deserializeTask(const QJsonObject &object,
                                            quint64 taskId,
                                            const QString &currentSessionKey) {
    const auto type =
        typeFromName(object.value(QStringLiteral("type")).toString());
    const QJsonValue operationValue = object.value(QStringLiteral("operation"));
    const auto operation =
        operationValue.isUndefined()
            ? std::optional<TransferOperation>(TransferOperation::Copy)
            : operationFromName(operationValue.toString());
    const QJsonValue conflictPolicyValue =
        object.value(QStringLiteral("conflictPolicy"));
    const auto conflictPolicy =
        conflictPolicyValue.isUndefined()
            ? std::optional<Policy>(Policy::Ask)
            : policyFromName(conflictPolicyValue.toString());
    const QJsonValue phaseValue = object.value(QStringLiteral("phase"));
    const auto phase =
        phaseValue.isUndefined()
            ? std::optional<TransferPhase>(TransferPhase::Transfer)
            : phaseFromName(phaseValue.toString());
    if (!type || !operation || !conflictPolicy || !phase)
        return std::nullopt;

    const QJsonValue batchIdValue = object.value(QStringLiteral("batchId"));
    const auto batchId = batchIdValue.isUndefined()
                             ? std::optional<quint64>(taskId)
                             : parseTaskId(batchIdValue);
    if (!batchId)
        return std::nullopt;

    quint64 dependencyId = 0;
    const QJsonValue dependencyValue =
        object.value(QStringLiteral("dependsOnTaskId"));
    if (!dependencyValue.isUndefined()) {
        const auto parsedDependency = parseTaskId(dependencyValue);
        if (!parsedDependency || *parsedDependency == taskId)
            return std::nullopt;
        dependencyId = *parsedDependency;
    }

    TransferTask task{};
    task.type = *type;
    task.taskId = taskId;
    task.batchId = *batchId;
    task.dependsOnTaskId = dependencyId;
    task.sessionKey = object.value(QStringLiteral("sessionKey")).toString();
    task.src = object.value(QStringLiteral("source")).toString();
    task.dst = object.value(QStringLiteral("destination")).toString();
    task.resumeHint = object.value(QStringLiteral("resumeHint")).toBool(false);
    task.speedLimitKBps =
        std::max(0, object.value(QStringLiteral("speedLimitKBps")).toInt());
    task.attempts =
        std::clamp(object.value(QStringLiteral("attempts")).toInt(), 0, 3);
    task.maxAttempts = 3;
    task.operation = *operation;
    task.conflictPolicy = *conflictPolicy;
    task.postAction = task.operation == TransferOperation::Move
                          ? TransferPostAction::DeleteSource
                          : TransferPostAction::None;
    task.phase = *phase;
    task.commitUncertain =
        object.value(QStringLiteral("commitUncertain")).toBool(false);
    task.queuedAtMs =
        object.value(QStringLiteral("queuedAtMs")).toVariant().toLongLong();
    if (task.queuedAtMs <= 0)
        task.queuedAtMs = QDateTime::currentMSecsSinceEpoch();
    task.restored = true;
    task.status = !currentSessionKey.isEmpty() && !task.sessionKey.isEmpty() &&
                          task.sessionKey != currentSessionKey
                      ? Status::WaitingForConnection
                      : Status::Paused;
    return task;
}

void markCorrupt(TransferQueuePersistence::LoadResult &result) {
    result.status = TransferQueuePersistence::LoadStatus::Corrupt;
    result.tasks.clear();
    result.warning = translate(
        "The saved transfer queue is corrupt and was preserved without "
        "changes.");
}

QJsonObject serializeTask(const TransferTask &task) {
    QJsonObject object;
    object.insert(QStringLiteral("id"), QString::number(task.taskId));
    object.insert(QStringLiteral("batchId"), QString::number(task.batchId));
    if (task.dependsOnTaskId != 0) {
        object.insert(QStringLiteral("dependsOnTaskId"),
                      QString::number(task.dependsOnTaskId));
    }
    object.insert(QStringLiteral("type"), typeName(task.type));
    object.insert(QStringLiteral("sessionKey"), task.sessionKey);
    object.insert(QStringLiteral("source"), task.src);
    object.insert(QStringLiteral("destination"), task.dst);
    object.insert(QStringLiteral("resumeHint"), task.resumeHint);
    object.insert(QStringLiteral("speedLimitKBps"), task.speedLimitKBps);
    object.insert(QStringLiteral("attempts"), task.attempts);
    object.insert(QStringLiteral("maxAttempts"), task.maxAttempts);
    object.insert(QStringLiteral("operation"), operationName(task.operation));
    object.insert(QStringLiteral("conflictPolicy"),
                  policyName(task.conflictPolicy));
    object.insert(QStringLiteral("phase"), phaseName(task.phase));
    object.insert(QStringLiteral("commitUncertain"), task.commitUncertain);
    object.insert(QStringLiteral("queuedAtMs"),
                  QString::number(task.queuedAtMs));
    return object;
}

} // namespace

TransferQueuePersistence::LoadResult
TransferQueuePersistence::load(const QString &path,
                               const QString &currentSessionKey) {
    LoadResult result;
    QFile file(path);
    if (!file.exists())
        return result;
    if (!file.open(QIODevice::ReadOnly)) {
        result.status = LoadStatus::IoError;
        result.warning =
            translate("The saved transfer queue could not be read: %1")
                .arg(file.errorString());
        return result;
    }

    if (file.size() > kMaxPersistenceBytes) {
        markCorrupt(result);
        return result;
    }
    const QByteArray serialized = file.read(kMaxPersistenceBytes + 1);
    if (serialized.size() > kMaxPersistenceBytes) {
        markCorrupt(result);
        return result;
    }
    if (file.error() != QFileDevice::NoError) {
        result.status = LoadStatus::IoError;
        result.warning =
            translate("The saved transfer queue could not be read: %1")
                .arg(file.errorString());
        return result;
    }

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(serialized, &parseError);
    if (parseError.error != QJsonParseError::NoError || !document.isObject()) {
        result.status = LoadStatus::Corrupt;
        result.warning = translate(
            "The saved transfer queue is corrupt and was preserved without "
            "changes.");
        return result;
    }
    const QJsonObject root = document.object();
    if (root.value(QStringLiteral("schemaVersion")).toInt(-1) != 1) {
        result.status = LoadStatus::UnsupportedSchema;
        result.warning = translate(
            "The saved transfer queue uses a newer format and was preserved "
            "without changes.");
        return result;
    }

    const QJsonValue tasksValue = root.value(QStringLiteral("tasks"));
    if (!tasksValue.isArray()) {
        markCorrupt(result);
        return result;
    }
    const QJsonArray tasks = tasksValue.toArray();
    if (tasks.size() > kMaxPersistedTasks) {
        markCorrupt(result);
        return result;
    }
    result.tasks.reserve(tasks.size());
    QSet<quint64> taskIds;
    for (const auto &value : tasks) {
        if (!value.isObject()) {
            markCorrupt(result);
            return result;
        }
        const QJsonObject object = value.toObject();
        const auto taskId = parseTaskId(object.value(QStringLiteral("id")));
        if (!taskId || taskIds.contains(*taskId)) {
            markCorrupt(result);
            return result;
        }
        auto task = deserializeTask(object, *taskId, currentSessionKey);
        if (!task) {
            markCorrupt(result);
            return result;
        }
        const bool requiresSource = task->type == TransferTask::Type::Upload ||
                                    task->type == TransferTask::Type::Download;
        if (task->dst.isEmpty() || (requiresSource && task->src.isEmpty())) {
            markCorrupt(result);
            return result;
        }
        taskIds.insert(*taskId);
        result.tasks.push_back(std::move(*task));
    }
    result.status = LoadStatus::Loaded;
    return result;
}

TransferQueuePersistence::SaveResult
TransferQueuePersistence::save(const QString &path,
                               const QVector<TransferTask> &tasks) {
    SaveResult result;
    QJsonArray serialized;
    for (const TransferTask &task : tasks) {
        const bool cleanupPending = task.status == Status::Warning &&
                                    task.phase == TransferPhase::DeleteSource;
        if (isTerminalTransferStatus(task.status) && !cleanupPending)
            continue;
        if (serialized.size() >= kMaxPersistedTasks) {
            result.warning = translate(
                "The transfer queue exceeds the persistence safety limit.");
            return result;
        }
        serialized.append(serializeTask(task));
    }

    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), 1);
    root.insert(QStringLiteral("tasks"), serialized);
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Compact);
    if (data.size() > kMaxPersistenceBytes) {
        result.warning = translate(
            "The transfer queue exceeds the persistence safety limit.");
        return result;
    }

    const QFileInfo fileInfo(path);
    if (!QDir().mkpath(fileInfo.dir().absolutePath())) {
        result.warning = translate(
            "The transfer queue storage directory could not be created.");
        return result;
    }
    QSaveFile saveFile(path);
    saveFile.setDirectWriteFallback(false);
    if (!saveFile.open(QIODevice::WriteOnly)) {
        result.warning = translate("The transfer queue could not be saved: %1")
                             .arg(saveFile.errorString());
        return result;
    }
    saveFile.setPermissions(QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    if (saveFile.write(data) != data.size() || !saveFile.commit()) {
        result.warning = translate("The transfer queue could not be saved: %1")
                             .arg(saveFile.errorString());
        return result;
    }
    QFile::setPermissions(path,
                          QFileDevice::ReadOwner | QFileDevice::WriteOwner);
    result.succeeded = true;
    return result;
}
