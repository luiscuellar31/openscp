// SCP backend module using libssh2 scp_send/scp_recv channels for file
// transfers over SSH.
#include "openscp/Libssh2ScpClient.hpp"
#include <libssh2.h>

#include <chrono>
#include <cstdio>
#include <filesystem>
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

} // namespace

bool Libssh2ScpClient::connect(const SessionOptions &opt, std::string &err) {
    sessionOptions_.reset();
    SessionOptions copy = opt;
    copy.protocol = Protocol::Scp;
    if (!delegate_.connectTransportOnly(copy, err))
        return false;
    sessionOptions_ = copy;
    return true;
}

void Libssh2ScpClient::disconnect() {
    delegate_.disconnect();
    sessionOptions_.reset();
}

void Libssh2ScpClient::interrupt() { delegate_.interrupt(); }

bool Libssh2ScpClient::isConnected() const { return delegate_.isConnected(); }

bool Libssh2ScpClient::list(const std::string &remote_path,
                            std::vector<FileInfo> &out, std::string &err) {
    (void)remote_path;
    out.clear();
    return unsupportedScpOperation("directory listing", err);
}

bool Libssh2ScpClient::get(
    const std::string &remote, const std::string &local, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel, bool resume) {
    if (resume) {
        err = "SCP downloads do not support resume.";
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

    libssh2_struct_stat fileInfo{};
    LIBSSH2_CHANNEL *channel =
        libssh2_scp_recv2(session, remote.c_str(), &fileInfo);
    if (!channel) {
        std::string scpErr = "Could not open remote file for SCP download";
        appendSessionErrorDetail(session, scpErr);
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

    std::FILE *localFile = std::fopen(local.c_str(), "wb");
    if (!localFile) {
        err = "Could not open local file for writing";
        closeScpChannel(channel, false);
        return false;
    }

    const std::size_t total =
        (fileInfo.st_size > 0) ? static_cast<std::size_t>(fileInfo.st_size) : 0;
    std::vector<char> buffer(kScpChunkSize);
    std::size_t done = 0;

    while (true) {
        if (shouldCancel && shouldCancel()) {
            err = "Canceled by user";
            std::fclose(localFile);
            (void)std::remove(local.c_str());
            closeScpChannel(channel, false);
            return false;
        }

        const ssize_t n = libssh2_channel_read(channel, buffer.data(),
                                               static_cast<size_t>(buffer.size()));
        if (n == LIBSSH2_ERROR_EAGAIN) {
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
            continue;
        }
        if (n < 0) {
            std::fclose(localFile);
            (void)std::remove(local.c_str());
            closeScpChannel(channel, false);
            std::string scpErr = "SCP read failed";
            appendSessionErrorDetail(session, scpErr);
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
            err = "Local write failed";
            std::fclose(localFile);
            (void)std::remove(local.c_str());
            closeScpChannel(channel, false);
            return false;
        }

        done += static_cast<std::size_t>(n);
        if (progress && total)
            progress(done, total);
    }

    if (std::fclose(localFile) != 0) {
        err = "Could not finalize local file";
        (void)std::remove(local.c_str());
        closeScpChannel(channel, false);
        return false;
    }
    closeScpChannel(channel, true);
    if (progress && total)
        progress(done, total);
    return true;
}

bool Libssh2ScpClient::put(
    const std::string &local, const std::string &remote, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel, bool resume) {
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

    std::uint64_t total = 0;
    try {
        total = std::filesystem::file_size(local);
    } catch (...) {
        err = "Could not determine local file size";
        return false;
    }

    std::FILE *localFile = std::fopen(local.c_str(), "rb");
    if (!localFile) {
        err = "Could not open local file for reading";
        return false;
    }

    LIBSSH2_CHANNEL *channel = libssh2_scp_send64(
        session, remote.c_str(), 0644, static_cast<libssh2_int64_t>(total), 0,
        0);
    if (!channel) {
        std::string scpErr = "Could not open remote file for SCP upload";
        appendSessionErrorDetail(session, scpErr);
        std::fclose(localFile);
        if (!sftpFallbackEnabled()) {
            scpErr +=
                " (SFTP fallback is disabled by the selected SCP mode)";
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
                err = "Local read failed";
                std::fclose(localFile);
                closeScpChannel(channel, false);
                return false;
            }
            break; // EOF
        }

        char *ptr = buffer.data();
        std::size_t remaining = nread;
        while (remaining > 0) {
            if (shouldCancel && shouldCancel()) {
                err = "Canceled by user";
                std::fclose(localFile);
                closeScpChannel(channel, false);
                return false;
            }
            const ssize_t wr = libssh2_channel_write(channel, ptr, remaining);
            if (wr == LIBSSH2_ERROR_EAGAIN) {
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
                continue;
            }
            if (wr < 0) {
                std::fclose(localFile);
                closeScpChannel(channel, false);
                std::string scpErr = "SCP write failed";
                appendSessionErrorDetail(session, scpErr);
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

    if (std::fclose(localFile) != 0) {
        err = "Could not close local file";
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
    (void)remote_path;
    isDir = false;
    return unsupportedScpOperation("path existence checks", err);
}

bool Libssh2ScpClient::stat(const std::string &remote_path, FileInfo &info,
                            std::string &err) {
    (void)remote_path;
    info = FileInfo{};
    return unsupportedScpOperation("metadata stat", err);
}

bool Libssh2ScpClient::chmod(const std::string &remote_path, std::uint32_t mode,
                             std::string &err) {
    (void)remote_path;
    (void)mode;
    return unsupportedScpOperation("chmod", err);
}

bool Libssh2ScpClient::chown(const std::string &remote_path, std::uint32_t uid,
                             std::uint32_t gid, std::string &err) {
    (void)remote_path;
    (void)uid;
    (void)gid;
    return unsupportedScpOperation("chown", err);
}

bool Libssh2ScpClient::setTimes(const std::string &remote_path,
                                std::uint64_t atime, std::uint64_t mtime,
                                std::string &err) {
    (void)remote_path;
    (void)atime;
    (void)mtime;
    return unsupportedScpOperation("timestamp updates", err);
}

bool Libssh2ScpClient::mkdir(const std::string &remote_dir, std::string &err,
                             unsigned int mode) {
    (void)remote_dir;
    (void)mode;
    return unsupportedScpOperation("mkdir", err);
}

bool Libssh2ScpClient::removeFile(const std::string &remote_path,
                                  std::string &err) {
    (void)remote_path;
    return unsupportedScpOperation("file deletion", err);
}

bool Libssh2ScpClient::removeDir(const std::string &remote_dir,
                                 std::string &err) {
    (void)remote_dir;
    return unsupportedScpOperation("directory deletion", err);
}

bool Libssh2ScpClient::rename(const std::string &from, const std::string &to,
                              std::string &err, bool overwrite) {
    (void)from;
    (void)to;
    (void)overwrite;
    return unsupportedScpOperation("rename", err);
}

std::unique_ptr<SftpClient>
Libssh2ScpClient::newConnectionLike(const SessionOptions &opt,
                                    std::string &err) {
    auto ptr = std::make_unique<Libssh2ScpClient>();
    if (!ptr->connect(opt, err))
        return nullptr;
    return ptr;
}

bool Libssh2ScpClient::transferViaSftpFallbackGet(
    const std::string &remote, const std::string &local, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel) {
    if (!sessionOptions_.has_value()) {
        err = "missing session options";
        return false;
    }
    if (shouldCancel && shouldCancel()) {
        err = "Canceled by user";
        return false;
    }

    SessionOptions sftpOpt = *sessionOptions_;
    sftpOpt.protocol = Protocol::Sftp;
    Libssh2SftpClient sftpFallback;
    if (!sftpFallback.connect(sftpOpt, err))
        return false;
    const bool ok = sftpFallback.get(remote, local, err, std::move(progress),
                                     std::move(shouldCancel), false);
    sftpFallback.disconnect();
    return ok;
}

bool Libssh2ScpClient::transferViaSftpFallbackPut(
    const std::string &local, const std::string &remote, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel) {
    if (!sessionOptions_.has_value()) {
        err = "missing session options";
        return false;
    }
    if (shouldCancel && shouldCancel()) {
        err = "Canceled by user";
        return false;
    }

    SessionOptions sftpOpt = *sessionOptions_;
    sftpOpt.protocol = Protocol::Sftp;
    Libssh2SftpClient sftpFallback;
    if (!sftpFallback.connect(sftpOpt, err))
        return false;
    const bool ok = sftpFallback.put(local, remote, err, std::move(progress),
                                     std::move(shouldCancel), false);
    sftpFallback.disconnect();
    return ok;
}

bool Libssh2ScpClient::sftpFallbackEnabled() const {
    if (!sessionOptions_.has_value())
        return true;
    return sessionOptions_->scp_transfer_mode == ScpTransferMode::Auto;
}

} // namespace openscp
