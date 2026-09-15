// Abstract, protocol-neutral interface for remote operations.
#pragma once

#include "Protocol.hpp"
#include "RemoteError.hpp"
#include "RemoteFileInfo.hpp"
#include "SessionOptions.hpp"

#include <cerrno>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace openscp {

class RemoteClient {
    public:
    virtual ~RemoteClient() = default;

    virtual Protocol protocol() const { return Protocol::Sftp; }
    virtual ProtocolCapabilities capabilities() const {
        return capabilitiesForProtocol(protocol());
    }

    RemoteError lastOperationError() const {
        std::lock_guard<std::mutex> lk(lastErrorMutex_);
        return lastError_;
    }

    virtual bool connect(const SessionOptions &opt, std::string &err) = 0;
    virtual void disconnect() = 0;
    virtual void interrupt() {}
    virtual bool isConnected() const = 0;

    virtual bool list(const std::string &remote_path,
                      std::vector<FileInfo> &out, std::string &err) = 0;

    virtual bool
    get(const std::string &remote, const std::string &local, std::string &err,
        std::function<void(std::size_t /*done*/, std::size_t /*total*/)>
            progress = {},
        std::function<bool()> shouldCancel = {}, bool resume = false) = 0;

    virtual bool
    put(const std::string &local, const std::string &remote, std::string &err,
        std::function<void(std::size_t /*done*/, std::size_t /*total*/)>
            progress = {},
        std::function<bool()> shouldCancel = {}, bool resume = false) = 0;

    virtual bool exists(const std::string &remote_path, bool &isDir,
                        std::string &err) = 0;

    virtual bool stat(const std::string &remote_path, FileInfo &info,
                      std::string &err) = 0;

    virtual bool chmod(const std::string &remote_path, std::uint32_t mode,
                       std::string &err) = 0;

    virtual bool chown(const std::string &remote_path, std::uint32_t uid,
                       std::uint32_t gid, std::string &err) = 0;

    virtual bool setTimes(const std::string &remote_path, std::uint64_t atime,
                          std::uint64_t mtime, std::string &err) = 0;

    virtual bool mkdir(const std::string &remote_dir, std::string &err,
                       unsigned int mode = 0755) = 0;

    virtual bool removeFile(const std::string &remote_path,
                            std::string &err) = 0;

    virtual bool removeDir(const std::string &remote_dir, std::string &err) = 0;

    virtual bool rename(const std::string &from, const std::string &to,
                        std::string &err, bool overwrite = false) = 0;

    // Calculates a digest without downloading the remote file. Backends must
    // only advertise can_checksum when they implement this operation. The
    // default lets derived clients omit unsupported checksum behavior and
    // fails safely with a structured Unsupported error.
    virtual bool
    checksum(const std::string &remote_path, const std::string &algorithm,
             std::vector<std::uint8_t> &digest, std::string &err,
             std::function<void(std::size_t /*done*/, std::size_t /*total*/)>
                 progress = {},
             std::function<bool()> shouldCancel = {}) {
        (void)remote_path;
        (void)algorithm;
        (void)progress;
        digest.clear();
        if (shouldCancel && shouldCancel()) {
            err = "Checksum calculation canceled";
            setLastOperationError(RemoteErrorKind::Canceled, err);
            return false;
        }
        err = "Remote checksums are not supported by this protocol";
        setLastOperationError(RemoteErrorKind::Unsupported, err);
        return false;
    }

    virtual std::unique_ptr<RemoteClient>
    newConnectionLike(const SessionOptions &opt, std::string &err) = 0;

    protected:
    void clearLastOperationError() {
        std::lock_guard<std::mutex> lk(lastErrorMutex_);
        lastError_ = RemoteError{};
    }

    void setLastOperationError(RemoteError error) {
        std::lock_guard<std::mutex> lk(lastErrorMutex_);
        lastError_ = std::move(error);
    }

    void setLastOperationError(
        RemoteErrorKind kind, std::string message, std::int64_t nativeCode = 0,
        bool transient = false, bool commitUncertain = false,
        std::optional<std::uint32_t> retryAfter = std::nullopt) {
        RemoteError error;
        error.kind = (kind == RemoteErrorKind::LocalIo && nativeCode == ENOSPC)
                         ? RemoteErrorKind::InsufficientSpace
                         : kind;
        error.message = std::move(message);
        error.native_code = nativeCode;
        error.retry_after_seconds = retryAfter;
        error.transient = transient;
        error.commit_uncertain = commitUncertain;
        setLastOperationError(std::move(error));
    }

    private:
    mutable std::mutex lastErrorMutex_;
    RemoteError lastError_{};
};

} // namespace openscp
