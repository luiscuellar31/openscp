// SftpClient implementation using libssh2 for SSH/SFTP.
// Encapsulates the SSH session, SFTP channel, and TCP socket.
#pragma once
#include "SftpClient.hpp"
#include <string>
#include <vector>

// Forward declarations of libssh2 internal types (with leading underscore)
struct _LIBSSH2_SESSION;
struct _LIBSSH2_SFTP;
struct _LIBSSH2_SFTP_HANDLE;

namespace openscp {

class Libssh2SftpClient : public SftpClient {
    public:
    Libssh2SftpClient();
    ~Libssh2SftpClient() override;

    bool connect(const SessionOptions &opt, std::string &err) override;
    void disconnect() override;
    bool isConnected() const override { return connected_; }

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
    bool connected_ = false;
    int sock_ = -1;
    _LIBSSH2_SESSION *session_ = nullptr; // <- uses internal libssh2 types
    _LIBSSH2_SFTP *sftp_ = nullptr;       // <- same
    TransferIntegrityPolicy transferIntegrityPolicy_ =
        TransferIntegrityPolicy::Optional;

    // TCP connection + SSH handshake and authentication.
    bool tcpConnect(const std::string &host, uint16_t port, std::string &err);
    bool sshHandshakeAuth(const SessionOptions &opt, std::string &err);
};

// Utility: remove a known_hosts entry for host:port and rewrite the file
// atomically.
bool RemoveKnownHostEntry(const std::string &khPath, const std::string &host,
                          std::uint16_t port, std::string &err);

} // namespace openscp
