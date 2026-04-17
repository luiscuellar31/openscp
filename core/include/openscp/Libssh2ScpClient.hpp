// SCP backend module. Uses real libssh2 SCP send/recv channels and reuses the
// authenticated SSH transport setup from Libssh2SftpClient.
#pragma once
#include "Libssh2SftpClient.hpp"
#include <optional>

namespace openscp {

class Libssh2ScpClient : public SftpClient {
    public:
    Libssh2ScpClient() = default;
    ~Libssh2ScpClient() override = default;

    Protocol protocol() const override { return Protocol::Scp; }
    ProtocolCapabilities capabilities() const override {
        return capabilitiesForProtocol(Protocol::Scp);
    }

    bool connect(const SessionOptions &opt, std::string &err) override;
    void disconnect() override;
    void interrupt() override;
    bool isConnected() const override;

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

    std::unique_ptr<SftpClient> newConnectionLike(const SessionOptions &opt,
                                                  std::string &err) override;

    private:
    bool transferViaSftpFallbackGet(
        const std::string &remote, const std::string &local, std::string &err,
        std::function<void(std::size_t, std::size_t)> progress,
        std::function<bool()> shouldCancel);
    bool transferViaSftpFallbackPut(
        const std::string &local, const std::string &remote, std::string &err,
        std::function<void(std::size_t, std::size_t)> progress,
        std::function<bool()> shouldCancel);
    bool sftpFallbackEnabled() const;

    Libssh2SftpClient delegate_;
    std::optional<SessionOptions> sessionOptions_;
};

} // namespace openscp
