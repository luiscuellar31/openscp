// Abstract interface for SFTP operations. Concrete implementations (e.g.,
// libssh2) must follow this API to keep the UI decoupled from the backend.
#pragma once
#include "SftpTypes.hpp"
#include <functional>
#include <memory>

namespace openscp {

class SftpClient {
    public:
    using ProgressCB = std::function<void(double)>; // 0..1 (currently unused)

    virtual ~SftpClient() = default;

    // Connect and disconnect
    virtual bool connect(const SessionOptions &opt, std::string &err) = 0;
    virtual void disconnect() = 0;
    // Best-effort async interruption for active network I/O (used to speed up
    // cancellation paths). Backends that cannot interrupt may keep no-op.
    virtual void interrupt() {}
    virtual bool isConnected() const = 0;

    // Remote directory listing
    virtual bool list(const std::string &remote_path,
                      std::vector<FileInfo> &out, std::string &err) = 0;

    // Download a remote file to local; if resume=true, try to continue a
    // partial download
    virtual bool
    get(const std::string &remote, const std::string &local, std::string &err,
        std::function<void(std::size_t /*done*/, std::size_t /*total*/)>
            progress = {},
        std::function<bool()> shouldCancel = {}, bool resume = false) = 0;

    // Upload a local file to remote; if resume=true, try to continue a partial
    // upload
    virtual bool
    put(const std::string &local, const std::string &remote, std::string &err,
        std::function<void(std::size_t /*done*/, std::size_t /*total*/)>
            progress = {},
        std::function<bool()> shouldCancel = {}, bool resume = false) = 0;

    // Check existence (leave err empty if "does not exist")
    virtual bool exists(const std::string &remote_path, bool &isDir,
                        std::string &err) = 0;

    // Detailed metadata (stat). Returns true if it exists.
    virtual bool stat(const std::string &remote_path, FileInfo &info,
                      std::string &err) = 0;

    // Change permissions (POSIX mode, e.g. 0644)
    virtual bool chmod(const std::string &remote_path, std::uint32_t mode,
                       std::string &err) = 0;

    // Change owner/group (if supported by the server)
    virtual bool chown(const std::string &remote_path, std::uint32_t uid,
                       std::uint32_t gid, std::string &err) = 0;

    // Adjust remote times (atime/mtime) if the server supports it
    virtual bool setTimes(const std::string &remote_path, std::uint64_t atime,
                          std::uint64_t mtime, std::string &err) = 0;

    // Remote file/folder operations
    virtual bool mkdir(const std::string &remote_dir, std::string &err,
                       unsigned int mode = 0755) = 0;

    virtual bool removeFile(const std::string &remote_path,
                            std::string &err) = 0;

    virtual bool removeDir(const std::string &remote_dir, std::string &err) = 0;

    virtual bool rename(const std::string &from, const std::string &to,
                        std::string &err, bool overwrite = false) = 0;

    // Create a new connection of the same type with the given options.
    virtual std::unique_ptr<SftpClient>
    newConnectionLike(const SessionOptions &opt, std::string &err) = 0;
};

} // namespace openscp
