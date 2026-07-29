#include "TransferUiController.hpp"

#include "RemotePath.hpp"

#include <QCoreApplication>
#include <QFileInfo>

namespace openscpui {

TransferUiUpdate
TransferUiController::observe(const QVector<TransferTask> &tasks,
                              bool remotePanelActive,
                              const QString &remoteRoot) {
    QSet<quint64> activeTaskIds;
    QSet<quint64> activeUploadIds;
    activeTaskIds.reserve(tasks.size());
    activeUploadIds.reserve(tasks.size());

    bool newUploadInCurrentRoot = false;
    int newlyCompleted = 0;
    QString firstCompletionMessage;

    for (const TransferTask &task : tasks) {
        activeTaskIds.insert(task.taskId);
        if (task.type == TransferTask::Type::Upload)
            activeUploadIds.insert(task.taskId);
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

    completedUploadIds_.removeIf(
        [&](quint64 taskId) { return !activeUploadIds.contains(taskId); });
    notifiedTaskIds_.removeIf(
        [&](quint64 taskId) { return !activeTaskIds.contains(taskId); });

    TransferUiUpdate update;
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

void TransferUiController::completeScheduledRefresh() {
    refreshScheduled_ = false;
}

void TransferUiController::reset() {
    refreshScheduled_ = false;
    completedUploadIds_.clear();
    notifiedTaskIds_.clear();
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
