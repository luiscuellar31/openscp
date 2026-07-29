// FTP/FTPS backend using libcurl for basic file transfers.
#pragma once
#include "RemoteClient.hpp"

#include <atomic>
#include <memory>
#include <mutex>

namespace openscp {
namespace curlcommon {
class CurlEasySession;
}

class CurlFtpClient : public RemoteClient {
    public:
    explicit CurlFtpClient(Protocol protocol = Protocol::Ftp);
    ~CurlFtpClient() override;

    Protocol protocol() const override { return protocol_; }
    ProtocolCapabilities capabilities() const override {
        return capabilitiesForProtocol(protocol_);
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

    std::unique_ptr<RemoteClient> newConnectionLike(const SessionOptions &opt,
                                                     std::string &err) override;

    private:
    Protocol protocol_ = Protocol::Ftp;
    mutable std::mutex stateMutex_;
    // One easy handle is reused serially so libcurl can retain its connection
    // cache. Stored opaquely to keep libcurl out of the public header.
    std::mutex operationMutex_;
    std::unique_ptr<curlcommon::CurlEasySession> easySession_;
    SessionOptions options_{};
    // Absolute server path reported by FTP PWD immediately after login. App
    // paths use "/" as the login directory, so quote commands are translated
    // through this root to match libcurl URL path semantics.
    std::string commandRoot_ = "/";
    bool connected_ = false;
    std::atomic<bool> interrupted_{false};
    std::atomic<bool> disconnecting_{false};
};

} // namespace openscp
