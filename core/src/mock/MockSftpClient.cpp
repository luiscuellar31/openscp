// Mock implementation: maintains a map of predefined paths for listing.
#include "openscp/MockSftpClient.hpp"
#include <algorithm>

namespace openscp {

bool MockSftpClient::connect(const SessionOptions& opt, std::string& err) {
    if (opt.host.empty() || opt.username.empty()) {
        err = "Host and username are required";
        return false;
    }
    connected_ = true;
    lastOpt_ = opt;
    return true;
}

bool MockSftpClient::setTimes(const std::string& remote_path,
                              std::uint64_t atime,
                              std::uint64_t mtime,
                              std::string& err) {
    (void)remote_path; (void)atime; (void)mtime; err.clear(); return true;
}

std::unique_ptr<SftpClient> MockSftpClient::newConnectionLike(const SessionOptions& opt,
                                                              std::string& err) {
    auto p = std::make_unique<MockSftpClient>();
    if (!p->connect(opt, err)) return nullptr;
    return p;
}

void MockSftpClient::disconnect() {
    connected_ = false;
}

bool MockSftpClient::list(const std::string& remote_path,
                          std::vector<FileInfo>& out,
                          std::string& err) {
    if (!connected_) {
        err = "Not connected";
        return false;
    }
    std::string path = remote_path;
    if (path.empty()) path = "/";

    auto it = fs_.find(path);
    if (it == fs_.end()) {
        err = "Mock remote path not found: " + path;
        return false;
    }
    out = it->second;
    std::sort(out.begin(), out.end(), [](const FileInfo& a, const FileInfo& b) {
        if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir; // directories first
        return a.name < b.name;
    });
    return true;
}

} // namespace openscp
