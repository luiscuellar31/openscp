// SCP backend module forwarding to the libssh2 SSH/SFTP implementation.
#include "openscp/Libssh2ScpClient.hpp"
#include <utility>

namespace openscp {

bool Libssh2ScpClient::connect(const SessionOptions &opt, std::string &err) {
    SessionOptions copy = opt;
    copy.protocol = Protocol::Scp;
    return delegate_.connect(copy, err);
}

void Libssh2ScpClient::disconnect() { delegate_.disconnect(); }

void Libssh2ScpClient::interrupt() { delegate_.interrupt(); }

bool Libssh2ScpClient::isConnected() const { return delegate_.isConnected(); }

bool Libssh2ScpClient::list(const std::string &remote_path,
                            std::vector<FileInfo> &out, std::string &err) {
    return delegate_.list(remote_path, out, err);
}

bool Libssh2ScpClient::get(
    const std::string &remote, const std::string &local, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel, bool resume) {
    return delegate_.get(remote, local, err, std::move(progress),
                         std::move(shouldCancel), resume);
}

bool Libssh2ScpClient::put(
    const std::string &local, const std::string &remote, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel, bool resume) {
    return delegate_.put(local, remote, err, std::move(progress),
                         std::move(shouldCancel), resume);
}

bool Libssh2ScpClient::exists(const std::string &remote_path, bool &isDir,
                              std::string &err) {
    return delegate_.exists(remote_path, isDir, err);
}

bool Libssh2ScpClient::stat(const std::string &remote_path, FileInfo &info,
                            std::string &err) {
    return delegate_.stat(remote_path, info, err);
}

bool Libssh2ScpClient::chmod(const std::string &remote_path, std::uint32_t mode,
                             std::string &err) {
    return delegate_.chmod(remote_path, mode, err);
}

bool Libssh2ScpClient::chown(const std::string &remote_path, std::uint32_t uid,
                             std::uint32_t gid, std::string &err) {
    return delegate_.chown(remote_path, uid, gid, err);
}

bool Libssh2ScpClient::setTimes(const std::string &remote_path,
                                std::uint64_t atime, std::uint64_t mtime,
                                std::string &err) {
    return delegate_.setTimes(remote_path, atime, mtime, err);
}

bool Libssh2ScpClient::mkdir(const std::string &remote_dir, std::string &err,
                             unsigned int mode) {
    return delegate_.mkdir(remote_dir, err, mode);
}

bool Libssh2ScpClient::removeFile(const std::string &remote_path,
                                  std::string &err) {
    return delegate_.removeFile(remote_path, err);
}

bool Libssh2ScpClient::removeDir(const std::string &remote_dir,
                                 std::string &err) {
    return delegate_.removeDir(remote_dir, err);
}

bool Libssh2ScpClient::rename(const std::string &from, const std::string &to,
                              std::string &err, bool overwrite) {
    return delegate_.rename(from, to, err, overwrite);
}

std::unique_ptr<SftpClient>
Libssh2ScpClient::newConnectionLike(const SessionOptions &opt,
                                    std::string &err) {
    auto ptr = std::make_unique<Libssh2ScpClient>();
    if (!ptr->connect(opt, err))
        return nullptr;
    return ptr;
}

} // namespace openscp
