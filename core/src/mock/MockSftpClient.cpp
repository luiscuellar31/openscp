// Deterministic in-memory remote filesystem used by unit tests.
#include "mock/MockSftpClient.hpp"

#include "openscp/RemotePath.hpp"

#include <algorithm>
#include <mutex>
#include <unordered_map>
#include <utility>
#include <vector>

namespace openscp {
namespace {

std::string parentPath(const std::string &path) {
    if (path == "/")
        return {};
    const std::size_t separator = path.find_last_of('/');
    return separator == 0 ? "/" : path.substr(0, separator);
}

std::string baseName(const std::string &path) {
    if (path == "/")
        return "/";
    return path.substr(path.find_last_of('/') + 1);
}

bool isDescendantOf(const std::string &path, const std::string &directory) {
    return path.size() > directory.size() &&
           path.compare(0, directory.size(), directory) == 0 &&
           path[directory.size()] == '/';
}

FileInfo directoryInfo(const std::string &name, std::uint32_t mode = 0755) {
    FileInfo info;
    info.name = name;
    info.is_dir = true;
    info.mode = mode;
    return info;
}

FileInfo fileInfo(const std::string &name, std::uint64_t size) {
    FileInfo info;
    info.name = name;
    info.size = size;
    info.has_size = true;
    info.mode = 0644;
    return info;
}

} // namespace

struct MockSftpClient::SharedState {
    SharedState() {
        entries.emplace("/", directoryInfo("/"));
        entries.emplace("/home", directoryInfo("home"));
        entries.emplace("/var", directoryInfo("var"));
        entries.emplace("/readme.txt", fileInfo("readme.txt", 1280));
        entries.emplace("/home/luis", directoryInfo("luis"));
        entries.emplace("/home/guest", directoryInfo("guest"));
        entries.emplace("/home/notes.md", fileInfo("notes.md", 2048));
        entries.emplace("/home/luis/proyectos", directoryInfo("proyectos"));
        entries.emplace("/home/luis/foto.jpg", fileInfo("foto.jpg", 34567));
        entries.emplace("/var/log", directoryInfo("log"));
    }

    std::mutex mutex;
    std::unordered_map<std::string, FileInfo> entries;
};

MockSftpClient::MockSftpClient() : state_(std::make_shared<SharedState>()) {
}

MockSftpClient::MockSftpClient(std::shared_ptr<SharedState> state)
    : state_(std::move(state)) {
}

MockSftpClient::~MockSftpClient() = default;

ProtocolCapabilities MockSftpClient::capabilities() const {
    ProtocolCapabilities result;
    result.implemented = true;
    result.can_list = true;
    result.can_stat = true;
    result.can_mkdir = true;
    result.can_delete = true;
    result.can_rename = true;
    result.can_read_metadata = true;
    result.can_set_permissions = true;
    result.can_set_ownership = true;
    result.can_set_timestamps = true;
    return result;
}

bool MockSftpClient::fail(RemoteErrorKind kind, const std::string &message,
                          std::string &err, bool transient) {
    err = message;
    setLastOperationError(kind, err, 0, transient);
    return false;
}

void MockSftpClient::succeed(std::string &err) {
    err.clear();
    clearLastOperationError();
}

bool MockSftpClient::requireConnection(std::string &err) {
    if (isConnected())
        return true;
    return fail(RemoteErrorKind::Connection, "Mock client is not connected",
                err, true);
}

bool MockSftpClient::connect(const SessionOptions &opt, std::string &err) {
    if (opt.host.empty() || opt.username.empty()) {
        connected_.store(false);
        return fail(RemoteErrorKind::InvalidRequest,
                    "Host and username are required", err);
    }
    connected_.store(true);
    succeed(err);
    return true;
}

void MockSftpClient::disconnect() {
    connected_.store(false);
}

bool MockSftpClient::list(const std::string &remote_path,
                          std::vector<FileInfo> &out, std::string &err) {
    out.clear();
    if (!requireConnection(err))
        return false;

    const std::string path = normalizeRemotePath(remote_path);
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto directory = state_->entries.find(path);
    if (directory == state_->entries.end()) {
        return fail(RemoteErrorKind::NotFound,
                    "Mock remote path not found: " + path, err);
    }
    if (!directory->second.is_dir) {
        return fail(RemoteErrorKind::InvalidRequest,
                    "Mock remote path is not a directory: " + path, err);
    }

    for (const auto &[entryPath, info] : state_->entries) {
        if (parentPath(entryPath) == path)
            out.push_back(info);
    }
    std::sort(out.begin(), out.end(), [](const FileInfo &a, const FileInfo &b) {
        if (a.is_dir != b.is_dir)
            return a.is_dir > b.is_dir;
        return a.name < b.name;
    });
    succeed(err);
    return true;
}

bool MockSftpClient::get(const std::string &remote, const std::string &local,
                         std::string &err,
                         std::function<void(std::size_t, std::size_t)> progress,
                         std::function<bool()> shouldCancel, bool resume) {
    (void)remote;
    (void)local;
    (void)progress;
    (void)shouldCancel;
    (void)resume;
    return fail(RemoteErrorKind::Unsupported,
                "Mock client does not implement downloads", err);
}

bool MockSftpClient::put(const std::string &local, const std::string &remote,
                         std::string &err,
                         std::function<void(std::size_t, std::size_t)> progress,
                         std::function<bool()> shouldCancel, bool resume) {
    (void)local;
    (void)remote;
    (void)progress;
    (void)shouldCancel;
    (void)resume;
    return fail(RemoteErrorKind::Unsupported,
                "Mock client does not implement uploads", err);
}

bool MockSftpClient::exists(const std::string &remote_path, bool &isDir,
                            std::string &err) {
    isDir = false;
    if (!requireConnection(err))
        return false;

    const std::string path = normalizeRemotePath(remote_path);
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto entry = state_->entries.find(path);
    if (entry == state_->entries.end()) {
        err.clear();
        setLastOperationError(RemoteErrorKind::NotFound,
                              "Mock remote path not found: " + path);
        return false;
    }
    isDir = entry->second.is_dir;
    succeed(err);
    return true;
}

bool MockSftpClient::stat(const std::string &remote_path, FileInfo &info,
                          std::string &err) {
    info = FileInfo{};
    if (!requireConnection(err))
        return false;

    const std::string path = normalizeRemotePath(remote_path);
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto entry = state_->entries.find(path);
    if (entry == state_->entries.end()) {
        err.clear();
        setLastOperationError(RemoteErrorKind::NotFound,
                              "Mock remote path not found: " + path);
        return false;
    }
    info = entry->second;
    succeed(err);
    return true;
}

bool MockSftpClient::chmod(const std::string &remote_path, std::uint32_t mode,
                           std::string &err) {
    if (!requireConnection(err))
        return false;
    const std::string path = normalizeRemotePath(remote_path);
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto entry = state_->entries.find(path);
    if (entry == state_->entries.end())
        return fail(RemoteErrorKind::NotFound,
                    "Mock remote path not found: " + path, err);
    entry->second.mode = mode;
    succeed(err);
    return true;
}

bool MockSftpClient::chown(const std::string &remote_path, std::uint32_t uid,
                           std::uint32_t gid, std::string &err) {
    if (!requireConnection(err))
        return false;
    const std::string path = normalizeRemotePath(remote_path);
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto entry = state_->entries.find(path);
    if (entry == state_->entries.end())
        return fail(RemoteErrorKind::NotFound,
                    "Mock remote path not found: " + path, err);
    entry->second.uid = uid;
    entry->second.gid = gid;
    succeed(err);
    return true;
}

bool MockSftpClient::setTimes(const std::string &remote_path,
                              std::uint64_t atime, std::uint64_t mtime,
                              std::string &err) {
    (void)atime; // FileInfo currently exposes only modification time.
    if (!requireConnection(err))
        return false;
    const std::string path = normalizeRemotePath(remote_path);
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto entry = state_->entries.find(path);
    if (entry == state_->entries.end())
        return fail(RemoteErrorKind::NotFound,
                    "Mock remote path not found: " + path, err);
    entry->second.mtime = mtime;
    succeed(err);
    return true;
}

bool MockSftpClient::mkdir(const std::string &remote_dir, std::string &err,
                           unsigned int mode) {
    if (!requireConnection(err))
        return false;
    const std::string path = normalizeRemotePath(remote_dir);
    if (path == "/")
        return fail(RemoteErrorKind::Conflict,
                    "Mock remote directory already exists: /", err);

    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->entries.contains(path)) {
        return fail(RemoteErrorKind::Conflict,
                    "Mock remote path already exists: " + path, err);
    }
    const std::string parentPathValue = parentPath(path);
    const auto parent = state_->entries.find(parentPathValue);
    if (parent == state_->entries.end()) {
        return fail(RemoteErrorKind::NotFound,
                    "Mock parent directory not found: " + parentPathValue, err);
    }
    if (!parent->second.is_dir) {
        return fail(RemoteErrorKind::InvalidRequest,
                    "Mock parent path is not a directory: " + parentPathValue,
                    err);
    }
    state_->entries.emplace(path, directoryInfo(baseName(path), mode));
    succeed(err);
    return true;
}

bool MockSftpClient::removeFile(const std::string &remote_path,
                                std::string &err) {
    if (!requireConnection(err))
        return false;
    const std::string path = normalizeRemotePath(remote_path);
    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto entry = state_->entries.find(path);
    if (entry == state_->entries.end())
        return fail(RemoteErrorKind::NotFound,
                    "Mock remote file not found: " + path, err);
    if (entry->second.is_dir) {
        return fail(RemoteErrorKind::InvalidRequest,
                    "Mock remote path is a directory: " + path, err);
    }
    state_->entries.erase(entry);
    succeed(err);
    return true;
}

bool MockSftpClient::removeDir(const std::string &remote_dir,
                               std::string &err) {
    if (!requireConnection(err))
        return false;
    const std::string path = normalizeRemotePath(remote_dir);
    if (path == "/")
        return fail(RemoteErrorKind::InvalidRequest,
                    "Mock remote root cannot be removed", err);

    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto entry = state_->entries.find(path);
    if (entry == state_->entries.end())
        return fail(RemoteErrorKind::NotFound,
                    "Mock remote directory not found: " + path, err);
    if (!entry->second.is_dir) {
        return fail(RemoteErrorKind::InvalidRequest,
                    "Mock remote path is not a directory: " + path, err);
    }
    const bool hasChildren =
        std::any_of(state_->entries.cbegin(), state_->entries.cend(),
                    [&path](const auto &candidate) {
                        return isDescendantOf(candidate.first, path);
                    });
    if (hasChildren) {
        return fail(RemoteErrorKind::Conflict,
                    "Mock remote directory is not empty: " + path, err);
    }
    state_->entries.erase(entry);
    succeed(err);
    return true;
}

bool MockSftpClient::rename(const std::string &from, const std::string &to,
                            std::string &err, bool overwrite) {
    if (!requireConnection(err))
        return false;
    const std::string source = normalizeRemotePath(from);
    const std::string destination = normalizeRemotePath(to);
    if (source == "/" || destination == "/") {
        return fail(RemoteErrorKind::InvalidRequest,
                    "Mock remote root cannot be renamed", err);
    }

    std::lock_guard<std::mutex> lock(state_->mutex);
    const auto sourceEntry = state_->entries.find(source);
    if (sourceEntry == state_->entries.end())
        return fail(RemoteErrorKind::NotFound,
                    "Mock rename source not found: " + source, err);
    if (source == destination) {
        succeed(err);
        return true;
    }
    if (sourceEntry->second.is_dir && isDescendantOf(destination, source)) {
        return fail(RemoteErrorKind::InvalidRequest,
                    "Mock directory cannot be moved inside itself", err);
    }

    const std::string destinationParentPath = parentPath(destination);
    const auto destinationParent = state_->entries.find(destinationParentPath);
    if (destinationParent == state_->entries.end()) {
        return fail(
            RemoteErrorKind::NotFound,
            "Mock destination parent not found: " + destinationParentPath, err);
    }
    if (!destinationParent->second.is_dir) {
        return fail(RemoteErrorKind::InvalidRequest,
                    "Mock destination parent is not a directory: " +
                        destinationParentPath,
                    err);
    }

    const auto destinationEntry = state_->entries.find(destination);
    if (destinationEntry != state_->entries.end()) {
        if (!overwrite) {
            return fail(
                RemoteErrorKind::Conflict,
                "Mock rename destination already exists: " + destination, err);
        }
        if (destinationEntry->second.is_dir != sourceEntry->second.is_dir) {
            return fail(RemoteErrorKind::Conflict,
                        "Mock rename cannot replace a different entry type",
                        err);
        }
        if (destinationEntry->second.is_dir) {
            const bool destinationHasChildren = std::any_of(
                state_->entries.cbegin(), state_->entries.cend(),
                [&destination](const auto &candidate) {
                    return isDescendantOf(candidate.first, destination);
                });
            if (destinationHasChildren) {
                return fail(RemoteErrorKind::Conflict,
                            "Mock rename destination directory is not empty",
                            err);
            }
        }
    }

    std::vector<std::pair<std::string, FileInfo>> movedEntries;
    for (const auto &[path, info] : state_->entries) {
        if (path != source && !isDescendantOf(path, source))
            continue;
        const std::string movedPath = destination + path.substr(source.size());
        FileInfo movedInfo = info;
        movedInfo.name = baseName(movedPath);
        movedEntries.emplace_back(movedPath, std::move(movedInfo));
    }
    if (destinationEntry != state_->entries.end())
        state_->entries.erase(destinationEntry);
    for (auto entry = state_->entries.begin();
         entry != state_->entries.end();) {
        if (entry->first == source || isDescendantOf(entry->first, source))
            entry = state_->entries.erase(entry);
        else
            ++entry;
    }
    for (auto &[path, info] : movedEntries)
        state_->entries.emplace(std::move(path), std::move(info));
    succeed(err);
    return true;
}

std::unique_ptr<RemoteClient>
MockSftpClient::newConnectionLike(const SessionOptions &opt, std::string &err) {
    auto client = std::unique_ptr<MockSftpClient>(new MockSftpClient(state_));
    if (!client->connect(opt, err)) {
        setLastOperationError(client->lastOperationError());
        return nullptr;
    }
    succeed(err);
    return client;
}

void MockSftpClient::resetFilesystem() {
    std::lock_guard<std::mutex> lock(state_->mutex);
    state_->entries.clear();
    state_->entries.emplace("/", directoryInfo("/"));
}

bool MockSftpClient::addEntry(const std::string &remote_path, FileInfo info) {
    const std::string path = normalizeRemotePath(remote_path);
    if (path == "/")
        return false;

    std::lock_guard<std::mutex> lock(state_->mutex);
    if (state_->entries.contains(path))
        return false;
    const auto parent = state_->entries.find(parentPath(path));
    if (parent == state_->entries.end() || !parent->second.is_dir)
        return false;
    info.name = baseName(path);
    if (info.is_dir) {
        info.size = 0;
        info.has_size = false;
    }
    state_->entries.emplace(path, std::move(info));
    return true;
}

} // namespace openscp
