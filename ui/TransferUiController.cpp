#include "TransferUiController.hpp"

#include "RemotePath.hpp"

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
    bool remotePanelActive, const QString &remoteRoot) {
    TransferUiUpdate update;
    for (const quint64 taskId : removedIds) {
        completedUploadIds_.remove(taskId);
        notifiedTaskIds_.remove(taskId);
        downloadsToOpen_.remove(taskId);
    }

    bool newUploadInCurrentRoot = false;
    int newlyCompleted = 0;
    QString firstCompletionMessage;

    for (const TransferTask &task : upserts) {
        auto pendingOpen = downloadsToOpen_.find(task.taskId);
        if (pendingOpen != downloadsToOpen_.end() &&
            isTerminalTransferStatus(task.status)) {
            if (task.status == TransferTask::Status::Done &&
                !update.completedDownloadPathsToOpen.contains(
                    pendingOpen.value())) {
                update.completedDownloadPathsToOpen.push_back(
                    pendingOpen.value());
            }
            downloadsToOpen_.erase(pendingOpen);
        }

        if (task.status != TransferTask::Status::Done)
            continue;

        if (task.type == TransferTask::Type::Upload &&
            !completedUploadIds_.contains(task.taskId)) {
            completedUploadIds_.insert(task.taskId);
            if (remotePanelActive &&
                pathIsInsideRemoteRoot(task.dst, remoteRoot)) {
                newUploadInCurrentRoot = true;
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

    if (newUploadInCurrentRoot && remotePanelActive && !refreshScheduled_) {
        refreshScheduled_ = true;
        update.scheduleRemoteRefresh = true;
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

void TransferUiController::openDownloadWhenCompleted(quint64 taskId,
                                                     const QString &localPath) {
    if (taskId == 0 || localPath.isEmpty())
        return;
    downloadsToOpen_.insert(taskId, localPath);
}

void TransferUiController::completeScheduledRefresh() {
    refreshScheduled_ = false;
}

void TransferUiController::reset() {
    refreshScheduled_ = false;
    completedUploadIds_.clear();
    notifiedTaskIds_.clear();
    downloadsToOpen_.clear();
}

bool TransferUiController::pathIsInsideRemoteRoot(const QString &candidatePath,
                                                  const QString &rootPath) {
    const QString candidate = ::normalizeRemotePath(candidatePath);
    const QString root = ::normalizeRemotePath(rootPath);
    if (root == QStringLiteral("/"))
        return candidate.startsWith(QLatin1Char('/'));
    return candidate == root ||
           candidate.startsWith(root + QStringLiteral("/"));
}

} // namespace openscpui
