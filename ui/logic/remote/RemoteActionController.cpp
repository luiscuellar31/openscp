#include "logic/remote/RemoteActionController.hpp"

#include "logic/common/MainWindowSharedUtils.hpp"
#include "logic/common/UiAlerts.hpp"
#include "logic/navigation/RemotePath.hpp"
#include "logic/remote/RemoteOperationController.hpp"

#include <QCoreApplication>
#include <QFileInfo>
#include <QMessageBox>
#include <QPointer>
#include <QProgressDialog>
#include <QSet>
#include <QStatusBar>

#include <memory>
#include <utility>

namespace openscpui {

RemoteActionController::RemoteActionController(QObject *parent)
    : QObject(parent) {
}

void RemoteActionController::setContext(Context context) {
    context_ = std::move(context);
}

bool RemoteActionController::isReady() const {
    return context_.operations && context_.operations->hasRequestedSession() &&
           (!context_.isSessionActive || context_.isSessionActive());
}

void RemoteActionController::showStatus(const QString &message,
                                        int timeoutMs) const {
    if (context_.statusBar)
        context_.statusBar->showMessage(message, timeoutMs);
}

void RemoteActionController::reportRemoteActivity() const {
    if (context_.remoteActivitySucceeded)
        context_.remoteActivitySucceeded();
}

void RemoteActionController::refresh(const QString &path) const {
    if (context_.refreshPath)
        context_.refreshPath(path);
}

std::shared_ptr<quint64>
RemoteActionController::watchMutation(const QString &basePath,
                                      const QString &failureMessage,
                                      const QString &successMessage) {
    if (!isReady())
        return {};

    auto jobId = std::make_shared<quint64>(0);
    auto connection = std::make_shared<QMetaObject::Connection>();
    *connection = connect(
        context_.operations, &RemoteOperationController::mutationCompleted,
        this,
        [this, jobId, connection, basePath, failureMessage, successMessage](
            const RemoteOperationController::MutationResult &result) {
            if (result.result.job.id != *jobId)
                return;
            QObject::disconnect(*connection);
            if (context_.isSessionActive && !context_.isSessionActive()) {
                return;
            }
            if (result.result.outcome !=
                RemoteOperationController::Outcome::Succeeded) {
                UiAlerts::critical(
                    context_.dialogParent,
                    QCoreApplication::translate("MainWindow", "Remote"),
                    failureMessage.arg(
                        shortRemoteError(result.result.error,
                                         QCoreApplication::translate(
                                             "MainWindow", "Remote error"))));
                return;
            }
            reportRemoteActivity();
            refresh(basePath);
            if (!successMessage.isEmpty())
                showStatus(successMessage, 4000);
        });
    return jobId;
}

void RemoteActionController::createDirectory(const QString &basePath,
                                             const QString &name) {
    const auto jobId = watchMutation(
        basePath, QCoreApplication::translate(
                      "MainWindow", "Could not create the remote folder.\n%1"));
    if (!jobId)
        return;

    RemoteOperationController::MkdirRequest request;
    request.path = joinRemotePath(basePath, name);
    request.mode = 0755;
    *jobId = context_.operations->submit(request);
    showStatus(
        QCoreApplication::translate("MainWindow", "Creating remote folder…"));
}

void RemoteActionController::createFile(const QString &basePath,
                                        const QString &name) {
    if (!isReady())
        return;
    const QString remotePath = joinRemotePath(basePath, name);

    auto submitCreate = [this, basePath, remotePath](bool overwrite) {
        const auto jobId = watchMutation(
            basePath,
            QCoreApplication::translate(
                "MainWindow", "Could not create the remote file.\n%1"),
            QCoreApplication::translate("MainWindow", "File created: ") +
                remotePath);
        if (!jobId)
            return;
        RemoteOperationController::CreateFileRequest request;
        request.path = remotePath;
        request.overwrite = overwrite;
        *jobId = context_.operations->submit(request);
        showStatus(
            QCoreApplication::translate("MainWindow", "Creating remote file…"));
    };

    auto statJob = std::make_shared<quint64>(0);
    auto statConnection = std::make_shared<QMetaObject::Connection>();
    *statConnection = connect(
        context_.operations, &RemoteOperationController::statCompleted, this,
        [this, statJob, statConnection, submitCreate,
         name](const RemoteOperationController::StatResult &result) {
            if (result.result.job.id != *statJob)
                return;
            QObject::disconnect(*statConnection);
            if (context_.isSessionActive && !context_.isSessionActive()) {
                return;
            }
            if (result.result.outcome !=
                RemoteOperationController::Outcome::Succeeded) {
                UiAlerts::critical(
                    context_.dialogParent,
                    QCoreApplication::translate("MainWindow", "Remote"),
                    QCoreApplication::translate(
                        "MainWindow",
                        "Could not check whether the remote file already "
                        "exists.\n%1")
                        .arg(shortRemoteError(
                            result.result.error,
                            QCoreApplication::translate("MainWindow",
                                                        "Remote error"))));
                return;
            }
            reportRemoteActivity();
            if (result.found &&
                UiAlerts::question(
                    context_.dialogParent,
                    QCoreApplication::translate("MainWindow", "File exists"),
                    QCoreApplication::translate(
                        "MainWindow", "«%1» already exists.\nOverwrite?")
                        .arg(name),
                    QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
                return;
            }
            submitCreate(result.found);
        });
    RemoteOperationController::StatRequest request;
    request.path = remotePath;
    *statJob = context_.operations->submit(request);
    showStatus(
        QCoreApplication::translate("MainWindow", "Checking remote file…"));
}

void RemoteActionController::rename(const QString &basePath,
                                    const QString &oldName,
                                    const QString &newName) {
    const auto jobId = watchMutation(
        basePath, QCoreApplication::translate(
                      "MainWindow", "Could not rename the remote item.\n%1"));
    if (!jobId)
        return;

    RemoteOperationController::RenameRequest request;
    request.from = joinRemotePath(basePath, oldName);
    request.to = joinRemotePath(basePath, newName);
    request.overwrite = false;
    *jobId = context_.operations->submit(request);
    showStatus(
        QCoreApplication::translate("MainWindow", "Renaming remote item…"));
}

void RemoteActionController::remove(const QString &basePath,
                                    const QVector<Entry> &entries) {
    if (!isReady() || entries.isEmpty())
        return;
    if (UiAlerts::warning(
            context_.dialogParent,
            QCoreApplication::translate("MainWindow", "Confirm delete"),
            QCoreApplication::translate(
                "MainWindow",
                "This will permanently delete items on the remote "
                "server.\nContinue?"),
            QMessageBox::Yes | QMessageBox::No) != QMessageBox::Yes) {
        return;
    }

    struct DeleteState {
        QSet<quint64> pending;
        quint64 deletedCount = 0;
        quint64 failedCount = 0;
        int completedRequests = 0;
        bool canceled = false;
        QString lastError;
        QString basePath;
        QPointer<QProgressDialog> progress;
        QMetaObject::Connection mutationConnection;
        QMetaObject::Connection progressConnection;
    };
    auto state = std::make_shared<DeleteState>();
    state->basePath = basePath;
    state->progress = new QProgressDialog(
        QCoreApplication::translate("MainWindow", "Deleting remote items…"),
        QCoreApplication::translate("MainWindow", "Cancel"), 0,
        static_cast<int>(entries.size()), context_.dialogParent);
    state->progress->setWindowTitle(
        QCoreApplication::translate("MainWindow", "Delete"));
    state->progress->setWindowModality(Qt::NonModal);
    state->progress->setMinimumDuration(0);
    state->progress->setAutoClose(false);
    state->progress->show();

    state->mutationConnection = connect(
        context_.operations, &RemoteOperationController::mutationCompleted,
        this,
        [this, state](const RemoteOperationController::MutationResult &result) {
            if (!state->pending.remove(result.result.job.id))
                return;
            ++state->completedRequests;
            state->deletedCount += result.affectedEntries;
            state->failedCount += result.failedEntries;
            if (result.result.outcome !=
                RemoteOperationController::Outcome::Succeeded) {
                if (result.result.outcome ==
                    RemoteOperationController::Outcome::Canceled) {
                    state->canceled = true;
                } else if (result.failedEntries == 0) {
                    ++state->failedCount;
                }
                if (!result.result.error.isEmpty())
                    state->lastError = result.result.error;
            }
            if (state->progress)
                state->progress->setValue(state->completedRequests);
            if (!state->pending.isEmpty())
                return;

            QObject::disconnect(state->mutationConnection);
            QObject::disconnect(state->progressConnection);
            if (state->progress) {
                state->progress->hide();
                state->progress->deleteLater();
            }
            if (context_.isSessionActive && !context_.isSessionActive()) {
                return;
            }

            QString status = QCoreApplication::translate(
                                 "MainWindow", "Deleted OK: %1  |  Failed: %2")
                                 .arg(state->deletedCount)
                                 .arg(state->failedCount);
            if (state->canceled)
                status += QStringLiteral("  |  ") +
                          QCoreApplication::translate("MainWindow", "Canceled");
            if (state->failedCount > 0 && !state->lastError.isEmpty()) {
                status +=
                    QStringLiteral("\n") +
                    QCoreApplication::translate("MainWindow", "Last error: ") +
                    state->lastError;
            }
            showStatus(status, 6000);
            if (state->failedCount == 0 && state->deletedCount > 0) {
                reportRemoteActivity();
            }
            refresh(state->basePath);
        });
    state->progressConnection = connect(
        context_.operations, &RemoteOperationController::jobProgress, this,
        [state](const RemoteOperationController::Progress &progress) {
            if (!state->pending.contains(progress.job.id) || !state->progress) {
                return;
            }
            state->progress->setLabelText(
                QCoreApplication::translate(
                    "MainWindow", "Deleting %1\nRemoved: %2  |  Failed: %3")
                    .arg(progress.currentPath)
                    .arg(progress.affectedEntries)
                    .arg(progress.failedEntries));
        });
    connect(state->progress, &QProgressDialog::canceled, this, [this, state] {
        state->canceled = true;
        if (!context_.operations)
            return;
        const auto pending = state->pending;
        for (quint64 jobId : pending)
            context_.operations->cancel(jobId);
    });

    for (const Entry &entry : entries) {
        RemoteOperationController::DeleteRequest request;
        request.path = joinRemotePath(basePath, entry.name);
        request.kind = entry.isDirectory
                           ? RemoteOperationController::DeleteKind::Directory
                           : RemoteOperationController::DeleteKind::File;
        request.recursive = entry.isDirectory;
        request.traversal.includeHidden = true;
        request.traversal.skipSymlinks = true;
        request.traversal.maxDepth = 32;
        request.traversal.batchSize = 250;
        const quint64 jobId = context_.operations->submit(request);
        if (jobId != 0)
            state->pending.insert(jobId);
    }
    if (!state->pending.isEmpty())
        return;

    QObject::disconnect(state->mutationConnection);
    QObject::disconnect(state->progressConnection);
    state->progress->deleteLater();
    UiAlerts::warning(context_.dialogParent,
                      QCoreApplication::translate("MainWindow", "Remote"),
                      QCoreApplication::translate(
                          "MainWindow", "Could not start remote deletion."));
}

void RemoteActionController::changePermissions(const QString &basePath,
                                               const Entry &entry) {
    if (!isReady() || !context_.requestPermissions)
        return;
    const QString path = joinRemotePath(basePath, entry.name);

    auto statJob = std::make_shared<quint64>(0);
    auto statConnection = std::make_shared<QMetaObject::Connection>();
    *statConnection = connect(
        context_.operations, &RemoteOperationController::statCompleted, this,
        [this, statJob, statConnection, path,
         basePath](const RemoteOperationController::StatResult &result) {
            if (result.result.job.id != *statJob)
                return;
            QObject::disconnect(*statConnection);
            if (context_.isSessionActive && !context_.isSessionActive()) {
                return;
            }
            if (result.result.outcome !=
                    RemoteOperationController::Outcome::Succeeded ||
                !result.found) {
                UiAlerts::warning(
                    context_.dialogParent,
                    QCoreApplication::translate("MainWindow", "Permissions"),
                    QCoreApplication::translate(
                        "MainWindow", "Could not read permissions.\n%1")
                        .arg(shortRemoteError(
                            result.result.error,
                            QCoreApplication::translate(
                                "MainWindow",
                                "Error reading remote information."))));
                return;
            }

            const auto selection = context_.requestPermissions(
                result.info.mode & 0777u, result.info.is_dir);
            if (!selection)
                return;
            const std::uint32_t newMode =
                (result.info.mode & ~0777u) | (selection->mode & 0777u);
            const bool recursive = selection->recursive && result.info.is_dir;

            QPointer<QProgressDialog> progress;
            if (recursive) {
                progress = new QProgressDialog(
                    QCoreApplication::translate("MainWindow",
                                                "Changing remote permissions…"),
                    QCoreApplication::translate("MainWindow", "Cancel"), 0, 0,
                    context_.dialogParent);
                progress->setWindowTitle(
                    QCoreApplication::translate("MainWindow", "Permissions"));
                progress->setWindowModality(Qt::NonModal);
                progress->setMinimumDuration(0);
                progress->setAutoClose(false);
                progress->show();
            }

            auto chmodJob = std::make_shared<quint64>(0);
            auto mutationConnection =
                std::make_shared<QMetaObject::Connection>();
            auto progressConnection =
                std::make_shared<QMetaObject::Connection>();
            *progressConnection = connect(
                context_.operations, &RemoteOperationController::jobProgress,
                this,
                [chmodJob,
                 progress](const RemoteOperationController::Progress &update) {
                    if (update.job.id != *chmodJob || !progress)
                        return;
                    progress->setLabelText(
                        QCoreApplication::translate(
                            "MainWindow",
                            "Changing permissions for %1\nUpdated: %2  |  "
                            "Failed: %3")
                            .arg(update.currentPath)
                            .arg(update.affectedEntries)
                            .arg(update.failedEntries));
                });
            *mutationConnection = connect(
                context_.operations,
                &RemoteOperationController::mutationCompleted, this,
                [this, chmodJob, mutationConnection, progressConnection,
                 progress, path, basePath](
                    const RemoteOperationController::MutationResult &mutation) {
                    if (mutation.result.job.id != *chmodJob)
                        return;
                    QObject::disconnect(*mutationConnection);
                    QObject::disconnect(*progressConnection);
                    if (progress) {
                        progress->hide();
                        progress->deleteLater();
                    }
                    if (context_.isSessionActive &&
                        !context_.isSessionActive()) {
                        return;
                    }
                    if (mutation.result.outcome !=
                        RemoteOperationController::Outcome::Succeeded) {
                        QString item = QFileInfo(path).fileName();
                        if (item.isEmpty())
                            item = path;
                        UiAlerts::critical(
                            context_.dialogParent,
                            QCoreApplication::translate("MainWindow",
                                                        "Permissions"),
                            QCoreApplication::translate(
                                "MainWindow", "Could not apply permissions to "
                                              "\"%1\".\n%2")
                                .arg(item,
                                     shortRemoteError(
                                         mutation.result.error,
                                         QCoreApplication::translate(
                                             "MainWindow",
                                             "Error applying changes."))));
                        return;
                    }
                    reportRemoteActivity();
                    refresh(basePath);
                    showStatus(QCoreApplication::translate(
                                   "MainWindow",
                                   "Permissions updated: %1  |  Failed: %2")
                                   .arg(mutation.affectedEntries)
                                   .arg(mutation.failedEntries),
                               4000);
                });

            RemoteOperationController::ChmodRequest request;
            request.path = path;
            request.mode = newMode;
            request.recursive = recursive;
            request.traversal.includeHidden = true;
            request.traversal.skipSymlinks = true;
            request.traversal.maxDepth = 32;
            request.traversal.batchSize = 250;
            *chmodJob = context_.operations->submit(request);
            if (progress) {
                connect(progress, &QProgressDialog::canceled, this,
                        [this, chmodJob] {
                            if (context_.operations && *chmodJob != 0)
                                context_.operations->cancel(*chmodJob);
                        });
            }
        });

    RemoteOperationController::StatRequest request;
    request.path = path;
    *statJob = context_.operations->submit(request);
    showStatus(QCoreApplication::translate("MainWindow",
                                           "Reading remote permissions…"));
}

RemoteActionController::Availability RemoteActionController::availability(
    const openscp::ProtocolCapabilities &capabilities, bool sessionActive) {
    Availability result;
    if (!sessionActive)
        return result;
    result.canUpload = capabilities.can_upload;
    result.canCreateDirectory = capabilities.can_mkdir;
    result.canCreateFile = capabilities.can_upload;
    result.canRename = capabilities.can_rename;
    result.canDelete = capabilities.can_delete;
    result.canMoveToLocal =
        capabilities.can_download && capabilities.can_delete;
    result.canSetPermissions = capabilities.can_set_permissions;
    result.canMutate = result.canUpload || result.canCreateDirectory ||
                       result.canDelete || result.canRename;
    return result;
}

} // namespace openscpui
