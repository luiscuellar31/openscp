// SCP backend module using libssh2 scp_send/scp_recv channels for file
// transfers over SSH.
#include "libssh2/Libssh2ScpClient.hpp"

#include "common/UniqueFile.hpp"
#include "detail/Libssh2ErrorClassifier.hpp"
#include "openscp/SafeLocalFile.hpp"

#include <libssh2.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <limits>
#include <thread>
#include <utility>
#include <vector>

namespace openscp {
namespace {

constexpr std::size_t kScpChunkSize = 64 * 1024;

bool unsupportedScpOperation(const char *what, std::string &err) {
    err = std::string("SCP backend does not support ") + what + ".";
    return false;
}

void closeScpChannel(LIBSSH2_CHANNEL *channel, bool graceful) {
    if (!channel)
        return;
    if (graceful) {
        (void)libssh2_channel_send_eof(channel);
        (void)libssh2_channel_wait_eof(channel);
        (void)libssh2_channel_wait_closed(channel);
    }
    (void)libssh2_channel_free(channel);
}

void appendSessionErrorDetail(_LIBSSH2_SESSION *session, std::string &msg) {
    if (!session)
        return;
    char *emsg = nullptr;
    int emlen = 0;
    (void)libssh2_session_last_error(session, &emsg, &emlen, 0);
    if (emsg && emlen > 0) {
        msg += ": ";
        msg.append(emsg, static_cast<std::size_t>(emlen));
        return;
    }
    const long eno = libssh2_session_last_errno(session);
    if (eno != 0) {
        msg += " (libssh2 errno ";
        msg += std::to_string(eno);
        msg += ")";
    }
}

bool readScpByte(LIBSSH2_CHANNEL *channel, char &byte) {
    while (true) {
        const ssize_t n = libssh2_channel_read(channel, &byte, 1);
        if (n == 1)
            return true;
        if (n < 0 && n != LIBSSH2_ERROR_EAGAIN)
            return false;
        if (n == 0 && libssh2_channel_eof(channel))
            return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// Reads an SCP status: 0, or 1 or 2 and a message line. remoteFailure tells
// a reported error from a broken channel.
bool readScpStatus(_LIBSSH2_SESSION *session, LIBSSH2_CHANNEL *channel,
                   const char *missingStatus, std::string &err,
                   bool &remoteFailure) {
    remoteFailure = false;
    char status = 0;
    if (!readScpByte(channel, status)) {
        err = missingStatus;
        appendSessionErrorDetail(session, err);
        return false;
    }
    if (status == 0)
        return true;

    constexpr std::size_t kMaxMessage = 1024;
    std::string message;
    char byte = 0;
    while (message.size() < kMaxMessage && readScpByte(channel, byte) &&
           byte != '\n') {
        if (static_cast<unsigned char>(byte) >= 0x20)
            message.push_back(byte);
    }
    err = "Remote SCP reported an error";
    if (!message.empty())
        err += ": " + message;
    remoteFailure = true;
    return false;
}

bool writeScpOk(_LIBSSH2_SESSION *session, LIBSSH2_CHANNEL *channel,
                const char *failure, std::string &err) {
    const char ok = 0;
    while (true) {
        const ssize_t n = libssh2_channel_write(channel, &ok, 1);
        if (n == 1)
            return true;
        if (n != LIBSSH2_ERROR_EAGAIN) {
            err = failure;
            appendSessionErrorDetail(session, err);
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
}

// After the file data, an SCP source sends one status byte and waits for an
// acknowledgement before it exits. Reading until EOF instead takes that byte
// as data and then blocks until the session timeout.
bool finishScpDownload(_LIBSSH2_SESSION *session, LIBSSH2_CHANNEL *channel,
                       std::string &err, bool &remoteFailure) {
    return readScpStatus(session, channel,
                         "SCP download ended without a final status", err,
                         remoteFailure) &&
           writeScpOk(session, channel,
                      "Could not acknowledge the SCP download", err);
}

// The upload is the source: after the data it sends the status byte and
// waits for the remote scp to answer, which is where a failed write on the
// remote disk shows up. Without that byte the remote scp exits with an error
// even after a good copy.
bool finishScpUpload(_LIBSSH2_SESSION *session, LIBSSH2_CHANNEL *channel,
                     std::string &err, bool &remoteFailure) {
    remoteFailure = false;
    return writeScpOk(session, channel, "Could not finish the SCP upload",
                      err) &&
           readScpStatus(session, channel,
                         "SCP upload ended without a final status", err,
                         remoteFailure);
}

} // namespace

Libssh2ScpClient::StructuredErrorScope::StructuredErrorScope(
    Libssh2ScpClient &owner, std::string &error, bool mutation)
    : owner_(owner), error_(error), mutation_(mutation) {
    error_.clear();
    owner_.clearLastOperationError();
}

Libssh2ScpClient::StructuredErrorScope::~StructuredErrorScope() {
    if (error_.empty() || owner_.lastOperationError())
        return;
    owner_.setLastOperationError(
        owner_.classifyStructuredFailure(error_, mutation_));
}

Libssh2ScpClient::StructuredErrorScope
Libssh2ScpClient::beginStructuredOperation(std::string &err, bool mutation) {
    return StructuredErrorScope(*this, err, mutation);
}

RemoteError
Libssh2ScpClient::classifyStructuredFailure(const std::string &message,
                                            bool mutation) const {
    const RemoteError fallback = delegate_.lastOperationError();
    return libssh2detail::classifyFailure(message, delegate_.sessionHandle(),
                                          nullptr, mutation, &fallback);
}

bool Libssh2ScpClient::connect(const SessionOptions &opt, std::string &err) {
    auto structuredErrorScope = beginStructuredOperation(err);
    if (!delegate_.connectTransportOnly(opt, err))
        return false;
    transferMode_ = opt.scp_transfer_mode;
    localFileDurability_ = opt.local_file_durability;
    return true;
}

void Libssh2ScpClient::disconnect() {
    delegate_.disconnect();
}

void Libssh2ScpClient::interrupt() {
    delegate_.interrupt();
}

bool Libssh2ScpClient::isConnected() const {
    return delegate_.isConnected();
}

bool Libssh2ScpClient::list(const std::string &remote_path,
                            std::vector<FileInfo> &out, std::string &err) {
    auto structuredErrorScope = beginStructuredOperation(err);
    (void)remote_path;
    out.clear();
    return unsupportedScpOperation("directory listing", err);
}

bool Libssh2ScpClient::get(
    const std::string &remote, const std::string &local, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel, bool resume) {
    auto ioLock = delegate_.lockIo();
    auto structuredErrorScope = beginStructuredOperation(err);
    if (resume) {
        err = "SCP downloads do not support resume.";
        return false;
    }
    if (!delegate_.isConnected()) {
        err = "Not connected";
        return false;
    }
    const LocalFileDurability localFileDurability = localFileDurability_;
    _LIBSSH2_SESSION *session = delegate_.sessionHandle();
    if (!session) {
        err = "Not connected";
        return false;
    }

    libssh2_struct_stat fileInfo{};
    LIBSSH2_CHANNEL *channel =
        libssh2_scp_recv2(session, remote.c_str(), &fileInfo);
    if (!channel) {
        std::string scpErr = "Could not open remote file for SCP download";
        appendSessionErrorDetail(session, scpErr);
        // SCP could be blocked by server policy/path rules; try SFTP fallback
        // only when the selected mode allows it.
        if (!sftpFallbackEnabled()) {
            scpErr += " (SFTP fallback is disabled by the selected SCP mode)";
            err = std::move(scpErr);
            return false;
        }

        std::string fallbackErr;
        if (transferViaSftpFallbackGet(remote, local, fallbackErr, progress,
                                       shouldCancel)) {
            return true;
        }
        if (!fallbackErr.empty()) {
            scpErr += " | SFTP fallback failed: ";
            scpErr += fallbackErr;
        }
        err = std::move(scpErr);
        return false;
    }

    if (fileInfo.st_size < 0 || static_cast<std::uintmax_t>(fileInfo.st_size) >
                                    std::numeric_limits<std::size_t>::max()) {
        err = "Remote SCP file size cannot be represented locally";
        closeScpChannel(channel, false);
        setLastOperationError(RemoteErrorKind::Protocol, err);
        return false;
    }
    const std::size_t total = static_cast<std::size_t>(fileInfo.st_size);

    const std::string partial = local + ".part";
    std::string openError;
    std::FILE *localFile = localfiles::openRegularFileForWrite(
        partial, localfiles::WriteMode::Truncate, openError);
    if (!localFile) {
        const int nativeError = errno;
        err = openError.empty()
                  ? "Could not open local partial file for writing"
                  : openError;
        closeScpChannel(channel, false);
        setLastOperationError(RemoteErrorKind::LocalIo, err, nativeError);
        return false;
    }
    UniqueFile localFileOwner(localFile);

    std::vector<char> buffer(kScpChunkSize);
    std::size_t done = 0;

    // Read exactly the advertised size: the source follows the data with a
    // status byte, handled by finishScpDownload().
    while (done < total) {
        if (shouldCancel && shouldCancel()) {
            err = "Canceled by user";
            localFileOwner.reset();
            closeScpChannel(channel, false);
            return false;
        }

        const std::size_t wanted = std::min(buffer.size(), total - done);
        const ssize_t n = libssh2_channel_read(channel, buffer.data(), wanted);
        if (n == LIBSSH2_ERROR_EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (n < 0) {
            // Capture the session error before freeing the channel resets it.
            std::string scpErr = "SCP read failed";
            appendSessionErrorDetail(session, scpErr);
            localFileOwner.reset();
            closeScpChannel(channel, false);
            // Retry through SFTP only when SCP mode is "Auto".
            if (!sftpFallbackEnabled()) {
                scpErr +=
                    " (SFTP fallback is disabled by the selected SCP mode)";
                err = std::move(scpErr);
                return false;
            }
            std::string fallbackErr;
            if (transferViaSftpFallbackGet(remote, local, fallbackErr, progress,
                                           shouldCancel)) {
                return true;
            }
            if (!fallbackErr.empty()) {
                scpErr += " | SFTP fallback failed: ";
                scpErr += fallbackErr;
            }
            err = std::move(scpErr);
            return false;
        }
        if (n == 0) {
            if (libssh2_channel_eof(channel))
                break;
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }

        if (std::fwrite(buffer.data(), 1, static_cast<size_t>(n), localFile) !=
            static_cast<size_t>(n)) {
            const int nativeError = errno;
            err = "Local write failed";
            localFileOwner.reset();
            closeScpChannel(channel, false);
            setLastOperationError(RemoteErrorKind::LocalIo, err, nativeError);
            return false;
        }

        done += static_cast<std::size_t>(n);
        if (progress && total)
            progress(done, total);
    }

    if (done != total) {
        err = "Remote SCP download size did not match advertised metadata";
        localFileOwner.reset();
        closeScpChannel(channel, false);
        setLastOperationError(RemoteErrorKind::Integrity, err);
        return false;
    }
    bool remoteFailure = false;
    if (!finishScpDownload(session, channel, err, remoteFailure)) {
        // Classify while the session still holds the channel's error.
        if (remoteFailure)
            setLastOperationError(RemoteErrorKind::RemoteIo, err);
        else
            setLastOperationError(classifyStructuredFailure(err, false));
        localFileOwner.reset();
        closeScpChannel(channel, false);
        return false;
    }

    std::string syncError;
    if (!localfiles::flushAndSync(localFile, syncError, localFileDurability)) {
        const int nativeError = errno;
        localFileOwner.reset();
        closeScpChannel(channel, false);
        err = syncError.empty() ? "Could not synchronize local partial file"
                                : syncError;
        setLastOperationError(RemoteErrorKind::LocalIo, err, nativeError);
        return false;
    }
    if (localFileOwner.close() != 0) {
        const int nativeError = errno;
        closeScpChannel(channel, false);
        err = "Could not close local partial file";
        setLastOperationError(RemoteErrorKind::LocalIo, err, nativeError);
        return false;
    }
    closeScpChannel(channel, true);
    if (shouldCancel && shouldCancel()) {
        err = "Canceled by user";
        setLastOperationError(RemoteErrorKind::Canceled, err);
        return false;
    }
    std::string replaceError;
    if (!localfiles::atomicReplace(partial, local, replaceError,
                                   localFileDurability)) {
        const int nativeError = errno;
        err = replaceError.empty()
                  ? "Could not atomically finalize local SCP download"
                  : replaceError;
        setLastOperationError(RemoteErrorKind::LocalIo, err, nativeError);
        return false;
    }
    if (progress && total)
        progress(done, total);
    return true;
}

bool Libssh2ScpClient::put(
    const std::string &local, const std::string &remote, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel, bool resume) {
    auto ioLock = delegate_.lockIo();
    auto structuredErrorScope = beginStructuredOperation(err, true);
    if (resume) {
        err = "SCP uploads do not support resume.";
        return false;
    }
    if (!delegate_.isConnected()) {
        err = "Not connected";
        return false;
    }
    _LIBSSH2_SESSION *session = delegate_.sessionHandle();
    if (!session) {
        err = "Not connected";
        return false;
    }

    // Auto mode prioritizes SFTP's temporary upload + atomic rename. Classic
    // SCP writes directly to the final remote path and is only used when the
    // user explicitly selects SCP-only mode.
    if (sftpFallbackEnabled()) {
        return transferViaSftpFallbackPut(
            local, remote, err, std::move(progress), std::move(shouldCancel));
    }

    std::uint64_t total = 0;
    try {
        total = std::filesystem::file_size(local);
    } catch (...) {
        err = "Could not determine local file size";
        setLastOperationError(RemoteErrorKind::LocalIo, err);
        return false;
    }

    std::FILE *localFile = std::fopen(local.c_str(), "rb");
    if (!localFile) {
        err = "Could not open local file for reading";
        setLastOperationError(RemoteErrorKind::LocalIo, err, errno);
        return false;
    }
    UniqueFile localFileOwner(localFile);

    LIBSSH2_CHANNEL *channel = libssh2_scp_send64(
        session, remote.c_str(),
        static_cast<int>(localfiles::uploadPermissions(localFile)),
        static_cast<libssh2_int64_t>(total), 0, 0);
    if (!channel) {
        std::string scpErr = "Could not open remote file for SCP upload";
        appendSessionErrorDetail(session, scpErr);
        localFileOwner.reset();
        // Mirror download behavior: optional fallback to SFTP upload.
        if (!sftpFallbackEnabled()) {
            scpErr += " (SFTP fallback is disabled by the selected SCP mode)";
            err = std::move(scpErr);
            return false;
        }

        std::string fallbackErr;
        if (transferViaSftpFallbackPut(local, remote, fallbackErr, progress,
                                       shouldCancel)) {
            return true;
        }
        if (!fallbackErr.empty()) {
            scpErr += " | SFTP fallback failed: ";
            scpErr += fallbackErr;
        }
        err = std::move(scpErr);
        return false;
    }

    std::vector<char> buffer(kScpChunkSize);
    std::size_t done = 0;
    while (true) {
        const std::size_t nread =
            std::fread(buffer.data(), 1, buffer.size(), localFile);
        if (nread == 0) {
            if (std::ferror(localFile)) {
                const int nativeError = errno;
                err = "Local read failed";
                localFileOwner.reset();
                closeScpChannel(channel, false);
                setLastOperationError(RemoteErrorKind::LocalIo, err,
                                      nativeError);
                return false;
            }
            break; // EOF
        }

        char *ptr = buffer.data();
        std::size_t remaining = nread;
        // libssh2_channel_write may write fewer bytes than requested.
        while (remaining > 0) {
            if (shouldCancel && shouldCancel()) {
                err = "Canceled by user";
                localFileOwner.reset();
                closeScpChannel(channel, false);
                return false;
            }
            const ssize_t wr = libssh2_channel_write(channel, ptr, remaining);
            if (wr == LIBSSH2_ERROR_EAGAIN) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (wr < 0) {
                localFileOwner.reset();
                closeScpChannel(channel, false);
                std::string scpErr = "SCP write failed";
                appendSessionErrorDetail(session, scpErr);
                // Fallback keeps transfer features available when SCP write
                // path is rejected by the server.
                if (!sftpFallbackEnabled()) {
                    scpErr +=
                        " (SFTP fallback is disabled by the selected SCP mode)";
                    err = std::move(scpErr);
                    return false;
                }
                std::string fallbackErr;
                if (transferViaSftpFallbackPut(local, remote, fallbackErr,
                                               progress, shouldCancel)) {
                    return true;
                }
                if (!fallbackErr.empty()) {
                    scpErr += " | SFTP fallback failed: ";
                    scpErr += fallbackErr;
                }
                err = std::move(scpErr);
                return false;
            }
            ptr += wr;
            remaining -= static_cast<std::size_t>(wr);
            done += static_cast<std::size_t>(wr);
            if (progress && total)
                progress(done, static_cast<std::size_t>(total));
        }
    }

    if (localFileOwner.close() != 0) {
        const int nativeError = errno;
        err = "Could not close local file";
        closeScpChannel(channel, false);
        setLastOperationError(RemoteErrorKind::LocalIo, err, nativeError);
        return false;
    }
    bool remoteFailure = false;
    if (!finishScpUpload(session, channel, err, remoteFailure)) {
        // Classify while the session still holds the channel's error.
        if (remoteFailure)
            setLastOperationError(RemoteErrorKind::RemoteIo, err);
        else
            setLastOperationError(classifyStructuredFailure(err, false));
        closeScpChannel(channel, false);
        return false;
    }
    closeScpChannel(channel, true);
    if (progress && total)
        progress(done, static_cast<std::size_t>(total));
    return true;
}

bool Libssh2ScpClient::exists(const std::string &remote_path, bool &isDir,
                              std::string &err) {
    auto structuredErrorScope = beginStructuredOperation(err);
    (void)remote_path;
    isDir = false;
    return unsupportedScpOperation("path existence checks", err);
}

bool Libssh2ScpClient::stat(const std::string &remote_path, FileInfo &info,
                            std::string &err) {
    auto structuredErrorScope = beginStructuredOperation(err);
    (void)remote_path;
    info = FileInfo{};
    return unsupportedScpOperation("metadata stat", err);
}

bool Libssh2ScpClient::chmod(const std::string &remote_path, std::uint32_t mode,
                             std::string &err) {
    auto structuredErrorScope = beginStructuredOperation(err, true);
    (void)remote_path;
    (void)mode;
    return unsupportedScpOperation("chmod", err);
}

bool Libssh2ScpClient::chown(const std::string &remote_path, std::uint32_t uid,
                             std::uint32_t gid, std::string &err) {
    auto structuredErrorScope = beginStructuredOperation(err, true);
    (void)remote_path;
    (void)uid;
    (void)gid;
    return unsupportedScpOperation("chown", err);
}

bool Libssh2ScpClient::setTimes(const std::string &remote_path,
                                std::uint64_t atime, std::uint64_t mtime,
                                std::string &err) {
    auto structuredErrorScope = beginStructuredOperation(err, true);
    (void)remote_path;
    (void)atime;
    (void)mtime;
    return unsupportedScpOperation("timestamp updates", err);
}

bool Libssh2ScpClient::mkdir(const std::string &remote_dir, std::string &err,
                             unsigned int mode) {
    auto structuredErrorScope = beginStructuredOperation(err, true);
    (void)remote_dir;
    (void)mode;
    return unsupportedScpOperation("mkdir", err);
}

bool Libssh2ScpClient::removeFile(const std::string &remote_path,
                                  std::string &err) {
    auto structuredErrorScope = beginStructuredOperation(err, true);
    (void)remote_path;
    return unsupportedScpOperation("file deletion", err);
}

bool Libssh2ScpClient::removeDir(const std::string &remote_dir,
                                 std::string &err) {
    auto structuredErrorScope = beginStructuredOperation(err, true);
    (void)remote_dir;
    return unsupportedScpOperation("directory deletion", err);
}

bool Libssh2ScpClient::rename(const std::string &from, const std::string &to,
                              std::string &err, bool overwrite) {
    auto structuredErrorScope = beginStructuredOperation(err, true);
    (void)from;
    (void)to;
    (void)overwrite;
    return unsupportedScpOperation("rename", err);
}

// The fallback runs SFTP on the session SCP already authenticated, so it
// needs no second login and interrupt() reaches it too.
bool Libssh2ScpClient::transferViaSftpFallbackGet(
    const std::string &remote, const std::string &local, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel) {
    if (shouldCancel && shouldCancel()) {
        err = "Canceled by user";
        return false;
    }
    if (!delegate_.ensureSftpSubsystem(err))
        return false;
    const bool ok = delegate_.get(remote, local, err, std::move(progress),
                                  std::move(shouldCancel), false);
    if (!ok)
        setLastOperationError(delegate_.lastOperationError());
    return ok;
}

bool Libssh2ScpClient::transferViaSftpFallbackPut(
    const std::string &local, const std::string &remote, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel) {
    if (shouldCancel && shouldCancel()) {
        err = "Canceled by user";
        return false;
    }
    if (!delegate_.ensureSftpSubsystem(err))
        return false;
    const bool ok = delegate_.put(local, remote, err, std::move(progress),
                                  std::move(shouldCancel), false);
    if (!ok)
        setLastOperationError(delegate_.lastOperationError());
    return ok;
}

bool Libssh2ScpClient::sftpFallbackEnabled() const {
    return transferMode_ == ScpTransferMode::Auto;
}

} // namespace openscp
