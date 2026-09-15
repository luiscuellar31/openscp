// Test-only in-memory SFTP client without network access.
#pragma once

#include "openscp/RemoteClient.hpp"

#include <atomic>
#include <memory>

namespace openscp {

class MockSftpClient : public RemoteClient {
    public:
    MockSftpClient();
    ~MockSftpClient() override;

    ProtocolCapabilities capabilities() const override;

    bool connect(const SessionOptions &opt, std::string &err) override;
    void disconnect() override;
    bool isConnected() const override { return connected_.load(); }

    bool list(const std::string &remote_path, std::vector<FileInfo> &out,
              std::string &err) override;

    bool get(const std::string &remote, const std::string &local,
             std::string &err,
             std::function<void(std::size_t, std::size_t)> progress,
             std::function<bool()> shouldCancel, bool resume) override;

    bool put(const std::string &local, const std::string &remote,
             std::string &err,
             std::function<void(std::size_t, std::size_t)> progress,
             std::function<bool()> shouldCancel, bool resume) override;

    bool exists(const std::string &remote_path, bool &isDir,
                std::string &err) override;
    bool stat(const std::string &remote_path, FileInfo &info,
              std::string &err) override;
    bool chmod(const std::string &remote_path, std::uint32_t mode,
               std::string &err) override;
    bool chown(const std::string &remote_path, std::uint32_t uid,
               std::uint32_t gid, std::string &err) override;
    bool setTimes(const std::string &remote_path, std::uint64_t atime,
                  std::uint64_t mtime, std::string &err) override;
    bool mkdir(const std::string &remote_dir, std::string &err,
               unsigned int mode = 0755) override;
    bool removeFile(const std::string &remote_path, std::string &err) override;
    bool removeDir(const std::string &remote_dir, std::string &err) override;
    bool rename(const std::string &from, const std::string &to,
                std::string &err, bool overwrite = false) override;

    std::unique_ptr<RemoteClient> newConnectionLike(const SessionOptions &opt,
                                                    std::string &err) override;

    // Test fixture helpers. resetFilesystem() keeps an empty root directory;
    // addEntry() derives the entry name from remote_path and requires its
    // parent directory to exist.
    void resetFilesystem();
    bool addEntry(const std::string &remote_path, FileInfo info);

    private:
    struct SharedState;

    explicit MockSftpClient(std::shared_ptr<SharedState> state);

    bool requireConnection(std::string &err);
    bool fail(RemoteErrorKind kind, const std::string &message,
              std::string &err, bool transient = false);
    void succeed(std::string &err);

    std::atomic_bool connected_{false};
    std::shared_ptr<SharedState> state_;
};

} // namespace openscp
