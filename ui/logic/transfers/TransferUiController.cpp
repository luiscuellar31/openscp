#include "logic/transfers/TransferUiController.hpp"

#include "logic/navigation/RemotePath.hpp"

#include <QCoreApplication>
#include <QFileInfo>

namespace openscpui {

void TransferUiController::initialize(const QVector<TransferTask> &snapshot) {
    reset();
    completedUploadIds_.reserve(snapshot.size());
    notifiedTaskIds_.reserve(snapshot.size());
    for (const TransferTask &task : snapshot) {
        if (task.status != TransferTask::Status::Done)
            continue;
        notifiedTaskIds_.insert(task.taskId);
        if (task.type == TransferTask::Type::Upload)
            completedUploadIds_.insert(task.taskId);
    }
}

TransferUiUpdate TransferUiController::observe(
    const QVector<TransferTask> &upserts, const QVector<quint64> &removedIds,
    bool remotePanelActive, const QString &remoteRoot,
    const std::function<bool(const QString &name)> &isListed) {
    TransferUiUpdate update;
    for (const quint64 taskId : removedIds) {
        completedUploadIds_.remove(taskId);
        notifiedTaskIds_.remove(taskId);
        downloadsToOpen_.remove(taskId);
    }

    int newlyCompleted = 0;
    QString firstCompletionMessage;

    for (const TransferTask &task : upserts) {
        if (downloadsToOpen_.contains(task.taskId) &&
            isTerminalTransferStatus(task.status)) {
            if (task.status == TransferTask::Status::Done &&
                !update.completedDownloadPathsToOpen.contains(task.dst)) {
                update.completedDownloadPathsToOpen.push_back(task.dst);
            }
            downloadsToOpen_.remove(task.taskId);
        }

        if (task.status != TransferTask::Status::Done)
            continue;

        if (task.type == TransferTask::Type::Upload &&
            !completedUploadIds_.contains(task.taskId)) {
            completedUploadIds_.insert(task.taskId);
            const QString below =
                remotePanelActive ? pathBelowRemoteRoot(task.dst, remoteRoot)
                                  : QString();
            const qsizetype slash = below.indexOf(QLatin1Char('/'));
            if (!below.isEmpty() && slash < 0) {
                update.uploadsInRemoteRoot.push_back(
                    ::normalizeRemotePath(task.dst));
            } else if (slash > 0 &&
                       (!isListed || !isListed(below.left(slash)))) {
                update.scheduleRemoteRefresh = true;
            }
        }

        if (notifiedTaskIds_.contains(task.taskId))
            continue;
        notifiedTaskIds_.insert(task.taskId);
        ++newlyCompleted;
        if (newlyCompleted != 1)
            continue;

        const bool upload = task.type == TransferTask::Type::Upload;
        const QString path = upload ? task.src : task.dst;
        QString name = QFileInfo(path).fileName();
        if (name.isEmpty())
            name = path;
        firstCompletionMessage =
            upload ? QCoreApplication::translate("MainWindow",
                                                 "Upload completed: %1")
                         .arg(name)
                   : QCoreApplication::translate("MainWindow",
                                                 "Download completed: %1")
                         .arg(name);
    }

    if (newlyCompleted == 1) {
        update.completionMessage = firstCompletionMessage;
    } else if (newlyCompleted > 1) {
        update.completionMessage =
            QCoreApplication::translate("MainWindow", "%1 transfers completed")
                .arg(newlyCompleted);
    }
    return update;
}

void TransferUiController::openDownloadWhenCompleted(quint64 taskId) {
    if (taskId != 0)
        downloadsToOpen_.insert(taskId);
}

void TransferUiController::reset() {
    completedUploadIds_.clear();
    notifiedTaskIds_.clear();
    downloadsToOpen_.clear();
}

QString TransferUiController::pathBelowRemoteRoot(const QString &candidatePath,
                                                  const QString &rootPath) {
    const QString candidate = ::normalizeRemotePath(candidatePath);
    const QString root = ::normalizeRemotePath(rootPath);
    const QString prefix =
        root == QStringLiteral("/") ? root : root + QStringLiteral("/");
    if (!candidate.startsWith(prefix))
        return {};
    return candidate.mid(prefix.size());
}

} // namespace openscpui
