#include "TransferExecutor.hpp"

#include "openscp/RemoteClient.hpp"

#include <QCoreApplication>
#include <QDateTime>
#include <QDir>
#include <QFile>
#include <QFileDevice>
#include <QFileInfo>
#include <QTimeZone>

#include <algorithm>
#include <chrono>
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
        if (!QDir().mkpath(task.dst)) {
            error = translatedError("Could not create local directory");
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
        if (!QFileInfo::exists(task.dst) || QFile::remove(task.dst))
            return true;
        error = translatedError("Could not delete local file");
        return false;
    }
    if (task.type == TransferTask::Type::DeleteLocalDirectory) {
        if (!QFileInfo::exists(task.dst) || QDir().rmdir(task.dst))
            return true;
        error = translatedError(
            "Could not delete local directory (it may not be empty)");
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

    QFile localFile(task.dst);
    const QDateTime timestamp = QDateTime::fromSecsSinceEpoch(
        qint64(remoteInfo.mtime), QTimeZone::utc());
    if (localFile.exists()) {
        (void)localFile.setFileTime(timestamp,
                                    QFileDevice::FileModificationTime);
    }
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
        if (!QFileInfo::exists(task.src) || QFile::remove(task.src))
            return true;
        error = translatedError(
            "Transfer completed, but the local source could not be removed");
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
