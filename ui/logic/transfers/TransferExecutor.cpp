#include "logic/transfers/TransferExecutor.hpp"

#include "openscp/RemoteClient.hpp"
#include "openscp/SafeLocalFile.hpp"

#include <QCoreApplication>
#include <QDateTime>

#include <algorithm>
#include <cerrno>
#include <chrono>
#include <limits>
#include <thread>

namespace {

using Clock = std::chrono::steady_clock;
constexpr double kBytesPerKiB = 1024.0;
constexpr qint64 kUiProgressIntervalMs = 100;

std::string translatedError(const char *message) {
    return QCoreApplication::translate("TransferManager", message)
        .toUtf8()
        .toStdString();
}

bool waitForTaskBandwidth(quint64 bytes, int taskLimitKBps,
                          Clock::time_point &windowStart,
                          const std::function<bool()> &shouldCancel) {
    if (taskLimitKBps <= 0 || bytes == 0)
        return !shouldCancel();

    const double expectedSeconds =
        double(bytes) / (double(taskLimitKBps) * kBytesPerKiB);
    const double elapsedSeconds =
        std::chrono::duration<double>(Clock::now() - windowStart).count();
    double remainingSeconds = expectedSeconds - elapsedSeconds;
    while (remainingSeconds > 0.0005) {
        if (shouldCancel())
            return false;
        const double sliceSeconds = std::min(remainingSeconds, 0.05);
        std::this_thread::sleep_for(
            std::chrono::duration<double>(sliceSeconds));
        remainingSeconds -= sliceSeconds;
    }
    windowStart = Clock::now();
    return !shouldCancel();
}

bool runFilesystemOperation(
    TransferTask &task,
    const std::shared_ptr<openscp::RemoteClient> &remoteClient,
    std::string &error) {
    if (task.type == TransferTask::Type::CreateLocalDirectory) {
        std::string localError;
        if (!openscp::localfiles::ensureLocalDirectories(task.dst.toStdString(),
                                                         localError)) {
            error = translatedError("Could not create local directory") + ": " +
                    localError;
            return false;
        }
        return true;
    }
    if (task.type == TransferTask::Type::CreateRemoteDirectory) {
        bool isDirectory = false;
        std::string existsError;
        const bool exists = remoteClient->exists(task.dst.toStdString(),
                                                 isDirectory, existsError);
        if (!existsError.empty()) {
            error = existsError;
            return false;
        }
        if (exists && !isDirectory) {
            error =
                translatedError("Remote path exists and is not a directory");
            return false;
        }
        return exists ||
               remoteClient->mkdir(task.dst.toStdString(), error, 0755);
    }
    if (task.type == TransferTask::Type::DeleteLocalFile) {
        if (openscp::localfiles::removeLocalPath(task.dst.toStdString(), false,
                                                 error))
            return true;
        error = translatedError("Could not delete local file") + ": " + error;
        return false;
    }
    if (task.type == TransferTask::Type::DeleteLocalDirectory) {
        if (openscp::localfiles::removeLocalPath(task.dst.toStdString(), true,
                                                 error))
            return true;
        error = translatedError(
                    "Could not delete local directory (it may not be empty)") +
                ": " + error;
        return false;
    }
    if (task.type == TransferTask::Type::DeleteRemoteFile ||
        task.type == TransferTask::Type::DeleteRemoteDirectory) {
        const bool deleted =
            task.type == TransferTask::Type::DeleteRemoteDirectory
                ? remoteClient->removeDir(task.dst.toStdString(), error)
                : remoteClient->removeFile(task.dst.toStdString(), error);
        if (deleted)
            return true;
        if (remoteClient->lastOperationError().kind ==
            openscp::RemoteErrorKind::NotFound) {
            error.clear();
            return true;
        }
        return false;
    }
    return false;
}

bool isFilesystemOperation(TransferTask::Type type) {
    return type != TransferTask::Type::Upload &&
           type != TransferTask::Type::Download;
}

void preserveDownloadModificationTime(
    const TransferTask &task,
    const std::shared_ptr<openscp::RemoteClient> &remoteClient) {
    openscp::FileInfo remoteInfo{};
    std::string statError;
    (void)remoteClient->stat(task.src.toStdString(), remoteInfo, statError);
    if (remoteInfo.mtime == 0)
        return;

    if (remoteInfo.mtime >
        static_cast<quint64>(std::numeric_limits<std::int64_t>::max()))
        return;
    std::string ignoredError;
    (void)openscp::localfiles::setLocalModificationTime(
        task.dst.toStdString(), static_cast<std::int64_t>(remoteInfo.mtime),
        ignoredError);
}

} // namespace

bool TransferExecutor::run(
    TransferTask &task,
    const std::shared_ptr<openscp::RemoteClient> &remoteClient, bool resume,
    std::string &error, const Callbacks &callbacks) {
    if (isFilesystemOperation(task.type)) {
        const bool succeeded =
            runFilesystemOperation(task, remoteClient, error);
        if (succeeded && callbacks.progress)
            callbacks.progress(1, 1, 0, 0, true);
        return succeeded;
    }

    std::size_t previousBytes = 0;
    auto previousTick = Clock::now();
    auto taskWindowStart = previousTick;
    qint64 lastNotificationMs = 0;
    const auto shouldCancel = [&callbacks] {
        return callbacks.shouldCancel && callbacks.shouldCancel();
    };
    const auto progress = [&callbacks, &previousBytes, &previousTick,
                           &taskWindowStart, &lastNotificationMs,
                           &shouldCancel](std::size_t completedBytes,
                                          std::size_t totalBytes) {
        const quint64 delta = completedBytes >= previousBytes
                                  ? completedBytes - previousBytes
                                  : completedBytes;
        if (delta > 0 && callbacks.acquireGlobalBandwidth &&
            !callbacks.acquireGlobalBandwidth(delta)) {
            return;
        }
        const int taskLimit =
            callbacks.taskSpeedLimitKBps ? callbacks.taskSpeedLimitKBps() : 0;
        if (!waitForTaskBandwidth(delta, taskLimit, taskWindowStart,
                                  shouldCancel)) {
            return;
        }

        const auto now = Clock::now();
        const double elapsedSeconds =
            std::chrono::duration<double>(now - previousTick).count();
        const double speedKBps =
            elapsedSeconds > 0.000001 && delta > 0
                ? (double(delta) / kBytesPerKiB) / elapsedSeconds
                : 0.0;
        int etaSeconds = -1;
        if (totalBytes > completedBytes && speedKBps > 0) {
            etaSeconds =
                int((double(totalBytes - completedBytes) / kBytesPerKiB) /
                    speedKBps);
        } else if (totalBytes > 0 && completedBytes >= totalBytes) {
            etaSeconds = 0;
        }

        const qint64 nowMs = QDateTime::currentMSecsSinceEpoch();
        const bool publishNow =
            nowMs - lastNotificationMs >= kUiProgressIntervalMs ||
            (totalBytes > 0 && completedBytes >= totalBytes);
        if (publishNow)
            lastNotificationMs = nowMs;
        if (callbacks.progress) {
            callbacks.progress(completedBytes, totalBytes, speedKBps,
                               etaSeconds, publishNow);
        }
        previousBytes = completedBytes;
        previousTick = Clock::now();
    };

    if (task.type == TransferTask::Type::Upload &&
        task.postAction == TransferPostAction::DeleteSource) {
        openscp::localfiles::LocalFileIdentity identity;
        if (!openscp::localfiles::localFileIdentity(task.src.toStdString(),
                                                    identity, error))
            return false;
        task.localSourceIdentity = identity;
    }

    const bool succeeded =
        task.type == TransferTask::Type::Upload
            ? remoteClient->put(task.src.toStdString(), task.dst.toStdString(),
                                error, progress, shouldCancel, resume)
            : remoteClient->get(task.src.toStdString(), task.dst.toStdString(),
                                error, progress, shouldCancel, resume);
    if (succeeded && task.type == TransferTask::Type::Download)
        preserveDownloadModificationTime(task, remoteClient);
    return succeeded;
}

bool TransferExecutor::runPostAction(
    TransferTask &task,
    const std::shared_ptr<openscp::RemoteClient> &remoteClient,
    std::string &error) {
    if (task.postAction != TransferPostAction::DeleteSource)
        return true;
    if (task.type == TransferTask::Type::Upload) {
        // This helper never unlinks an existing entry. An absent source can
        // finish manual cleanup even after its process-local identity is lost.
        if (openscp::localfiles::removeLocalFileIfUnchanged(
                task.src.toStdString(),
                task.localSourceIdentity.value_or(
                    openscp::localfiles::LocalFileIdentity{}),
                error))
            return true;
        if (!task.localSourceIdentity) {
            error = QCoreApplication::translate(
                        "TransferManager",
                        "The completed upload cannot safely remove its local "
                        "source after a restart. Review the source manually.")
                        .toUtf8()
                        .toStdString();
            return false;
        }
        if (errno == ENOTSUP) {
            error = QCoreApplication::translate(
                        "TransferManager",
                        "The upload completed, but automatic local source "
                        "removal cannot be done safely. Review the source and "
                        "remove it manually, then retry the task to finish "
                        "cleanup.")
                        .toUtf8()
                        .toStdString();
            return false;
        }
        if (error ==
            "Local source changed after transfer; it was not removed.") {
            error = QCoreApplication::translate(
                        "TransferManager",
                        "The local source changed after upload and was not "
                        "removed. Review it manually.")
                        .toUtf8()
                        .toStdString();
            return false;
        }
        error = translatedError(
                    "Transfer completed, but the local source could not be "
                    "removed") +
                ": " + error;
        return false;
    }
    if (remoteClient &&
        remoteClient->removeFile(task.src.toStdString(), error)) {
        return true;
    }
    if (remoteClient && remoteClient->lastOperationError().kind ==
                            openscp::RemoteErrorKind::NotFound) {
        error.clear();
        return true;
    }
    if (error.empty()) {
        error = translatedError(
            "Transfer completed, but the remote source could not be removed");
    }
    return false;
}
