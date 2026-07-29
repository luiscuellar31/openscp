#include "TransferQueuePersistence.hpp"

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

#include <algorithm>
#include <limits>
#include <optional>

namespace {

using Policy = TransferConflictPolicy;
using Status = TransferTask::Status;

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

Policy policyFromName(const QString &name) {
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
    return Policy::Ask;
}

QString operationName(TransferOperation operation) {
    return operation == TransferOperation::Move ? QStringLiteral("move")
                                                : QStringLiteral("copy");
}

TransferOperation operationFromName(const QString &name) {
    return name == QStringLiteral("move") ? TransferOperation::Move
                                          : TransferOperation::Copy;
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

TransferPhase phaseFromName(const QString &name) {
    if (name == QStringLiteral("delete-source"))
        return TransferPhase::DeleteSource;
    if (name == QStringLiteral("finished"))
        return TransferPhase::Finished;
    return TransferPhase::Transfer;
}

std::optional<quint64> parseTaskId(const QJsonValue &value) {
    bool parsedSuccessfully = false;
    quint64 parsed = 0;
    if (value.isString()) {
        parsed = value.toString().toULongLong(&parsedSuccessfully);
    } else if (value.isDouble()) {
        const double number = value.toDouble(-1);
        if (number >= 0 &&
            number <= double(std::numeric_limits<qint64>::max())) {
            parsed = static_cast<quint64>(number);
            parsedSuccessfully = true;
        }
    }
    return parsedSuccessfully && parsed != 0 ? std::optional<quint64>(parsed)
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

TransferTask::Type typeFromName(const QString &name) {
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
    return TransferTask::Type::Download;
}

TransferTask deserializeTask(const QJsonObject &object, quint64 taskId,
                             const QString &currentSessionKey) {
    TransferTask task{
        typeFromName(object.value(QStringLiteral("type")).toString())};
    task.taskId = taskId;
    task.batchId =
        parseTaskId(object.value(QStringLiteral("batchId"))).value_or(taskId);
    task.dependsOnTaskId =
        parseTaskId(object.value(QStringLiteral("dependsOnTaskId")))
            .value_or(0);
    task.sessionKey = object.value(QStringLiteral("sessionKey")).toString();
    task.src = object.value(QStringLiteral("source")).toString();
    task.dst = object.value(QStringLiteral("destination")).toString();
    task.resumeHint = object.value(QStringLiteral("resumeHint")).toBool(false);
    task.speedLimitKBps =
        std::max(0, object.value(QStringLiteral("speedLimitKBps")).toInt());
    task.attempts =
        std::clamp(object.value(QStringLiteral("attempts")).toInt(), 0, 3);
    task.maxAttempts = 3;
    task.operation =
        operationFromName(object.value(QStringLiteral("operation")).toString());
    task.conflictPolicy = policyFromName(
        object.value(QStringLiteral("conflictPolicy")).toString());
    task.postAction = task.operation == TransferOperation::Move
                          ? TransferPostAction::DeleteSource
                          : TransferPostAction::None;
    task.phase =
        phaseFromName(object.value(QStringLiteral("phase")).toString());
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

    QJsonParseError parseError;
    const QJsonDocument document =
        QJsonDocument::fromJson(file.readAll(), &parseError);
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

    const QJsonArray tasks = root.value(QStringLiteral("tasks")).toArray();
    result.tasks.reserve(tasks.size());
    for (const QJsonValue &value : tasks) {
        if (!value.isObject())
            continue;
        const QJsonObject object = value.toObject();
        const auto taskId = parseTaskId(object.value(QStringLiteral("id")));
        if (!taskId)
            continue;
        TransferTask task = deserializeTask(object, *taskId, currentSessionKey);
        const bool requiresSource = task.type == TransferTask::Type::Upload ||
                                    task.type == TransferTask::Type::Download;
        if (task.dst.isEmpty() || (requiresSource && task.src.isEmpty()))
            continue;
        result.tasks.push_back(std::move(task));
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
        serialized.append(serializeTask(task));
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
    QJsonObject root;
    root.insert(QStringLiteral("schemaVersion"), 1);
    root.insert(QStringLiteral("tasks"), serialized);
    const QByteArray data = QJsonDocument(root).toJson(QJsonDocument::Compact);
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
