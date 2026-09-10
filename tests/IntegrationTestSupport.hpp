#pragma once

#include "TestHarness.hpp"
#include "openscp/RemoteClient.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openscp::testsupport {

inline constexpr int kSkipExitCode = 77;

inline std::optional<std::string> envValue(const char *key) {
    const char *raw = std::getenv(key);
    if (!raw || !*raw)
        return std::nullopt;
    return std::string(raw);
}

inline std::optional<std::string> envValueWithFallback(const char *primary,
                                                       const char *fallback) {
    auto value = envValue(primary);
    if (value.has_value())
        return value;
    return envValue(fallback);
}

inline bool parsePort(const std::optional<std::string> &raw, std::uint16_t &out,
                      std::uint16_t fallback) {
    if (!raw.has_value()) {
        out = fallback;
        return true;
    }
    try {
        const int parsed = std::stoi(*raw);
        if (parsed < 1 || parsed > 65535)
            return false;
        out = static_cast<std::uint16_t>(parsed);
        return true;
    } catch (...) {
        return false;
    }
}

inline bool parseBool(const std::optional<std::string> &raw, bool fallback) {
    if (!raw.has_value())
        return fallback;
    const std::string &value = *raw;
    if (value == "1" || value == "true" || value == "TRUE" || value == "yes" ||
        value == "YES") {
        return true;
    }
    if (value == "0" || value == "false" || value == "FALSE" || value == "no" ||
        value == "NO") {
        return false;
    }
    return fallback;
}

inline std::string uniqueToken() {
    static std::atomic<unsigned long long> counter{0};
    const auto now =
        std::chrono::steady_clock::now().time_since_epoch().count();
    return std::to_string(static_cast<long long>(now)) + "_" +
           std::to_string(counter.fetch_add(1));
}

inline std::string joinRemotePath(const std::string &base,
                                  const std::string &name) {
    if (base.empty())
        return std::string("/") + name;
    if (base.back() == '/')
        return base + name;
    return base + "/" + name;
}

inline bool writeFile(const std::filesystem::path &path,
                      const std::string &content) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output.is_open())
        return false;
    output << content;
    return output.good();
}

inline bool readFile(const std::filesystem::path &path, std::string &output) {
    std::ifstream input(path, std::ios::binary);
    if (!input.is_open())
        return false;
    output.assign(std::istreambuf_iterator<char>(input),
                  std::istreambuf_iterator<char>());
    return true;
}

class IntegrationTempDir final {
    public:
    IntegrationTempDir() {
        std::error_code error;
        const auto root = std::filesystem::temp_directory_path(error);
        if (!error) {
            path_ = root / ("openscp_integration_" + uniqueToken());
            valid_ = std::filesystem::create_directory(path_, error);
        }
        if (!valid_) {
            std::cerr << "[FAIL] could not create integration directory: "
                      << (error ? error.message() : "path already exists")
                      << '\n';
        }
    }

    ~IntegrationTempDir() {
        if (!valid_)
            return;
        std::error_code error;
        std::filesystem::remove_all(path_, error);
    }

    IntegrationTempDir(const IntegrationTempDir &) = delete;
    IntegrationTempDir &operator=(const IntegrationTempDir &) = delete;

    bool isValid() const { return valid_; }
    const std::filesystem::path &path() const { return path_; }

    private:
    std::filesystem::path path_;
    bool valid_ = false;
};

struct ManagedFilesContractHooks {
    std::function<void(openscp::RemoteClient &, const std::string &,
                       TestContext &)>
        afterDirectoryCreated;
    std::function<void(openscp::RemoteClient &, const std::string &,
                       TestContext &)>
        afterUpload;
};

inline void runManagedFilesContract(
    openscp::RemoteClient &client, openscp::Protocol expectedProtocol,
    const std::string &label, const std::string &remoteBase, TestContext &test,
    const ManagedFilesContractHooks &hooks = {}) {
    const auto message = [&label](const std::string &text) {
        return label + ": " + text;
    };
    const auto checkOperation = [&](bool success, const std::string &text,
                                    const std::string &error) {
        test.check(success,
                   message(error.empty() ? text : text + ": " + error));
    };
    test.check(client.protocol() == expectedProtocol,
               message("client should report the expected protocol"));
    const auto capabilities = client.capabilities();
    test.check(capabilities.implemented,
               message("backend should be marked implemented"));
    test.check(capabilities.can_upload && capabilities.can_download,
               message("backend should support transfers"));
    test.check(capabilities.can_list,
               message("backend should support remote listing"));
    test.check(capabilities.can_stat && capabilities.can_mkdir &&
                   capabilities.can_delete && capabilities.can_rename,
               message("backend should advertise remote CRUD operations"));

    IntegrationTempDir directory;
    test.check(directory.isValid(),
               message("local integration directory should be available"));
    if (!directory.isValid()) {
        client.disconnect();
        return;
    }

    const std::string token = uniqueToken();
    const auto localUpload = directory.path() / ("upload_" + token + ".txt");
    const auto localDownload =
        directory.path() / ("download_" + token + ".txt");
    const auto canceledDownload =
        directory.path() / ("canceled_" + token + ".txt");
    const auto boundaryCanceledDownload =
        directory.path() / ("boundary_canceled_" + token + ".txt");
    const std::string payload =
        "openscp " + label + " integration payload " + token + "\nline two\n";
    test.check(writeFile(localUpload, payload),
               message("should write the local upload file"));

    const std::string remoteDir =
        joinRemotePath(remoteBase, "openscp_managed_" + token);
    const std::string uploadName = "upload_" + token + ".txt";
    const std::string renamedName = "renamed_" + token + ".txt";
    const std::string remotePath = joinRemotePath(remoteDir, uploadName);
    const std::string renamedPath = joinRemotePath(remoteDir, renamedName);
    const std::string canceledUploadPath =
        joinRemotePath(remoteDir, "canceled_upload_" + token + ".txt");

    std::string error;
    checkOperation(client.mkdir(remoteDir, error), "mkdir should succeed",
                   error);
    if (hooks.afterDirectoryCreated)
        hooks.afterDirectoryCreated(client, remoteDir, test);

    bool uploadProgressCalled = false;
    error.clear();
    checkOperation(
        client.put(
            localUpload.string(), remotePath, error,
            [&](std::size_t, std::size_t) { uploadProgressCalled = true; }, {},
            false),
        "upload should succeed", error);
    test.check(uploadProgressCalled,
               message("upload progress callback should be called"));
    if (hooks.afterUpload)
        hooks.afterUpload(client, remotePath, test);

    bool pathIsDirectory = true;
    error.clear();
    checkOperation(client.exists(remotePath, pathIsDirectory, error),
                   "exists should find the uploaded file", error);
    test.check(!pathIsDirectory,
               message("exists should report the uploaded path as a file"));

    openscp::FileInfo statInfo{};
    error.clear();
    checkOperation(client.stat(remotePath, statInfo, error),
                   "stat should succeed", error);
    test.check(!statInfo.is_dir,
               message("stat should report the uploaded path as a file"));
    test.check(statInfo.has_size && statInfo.size == payload.size(),
               message("stat should report the uploaded file size"));

    error.clear();
    checkOperation(client.rename(remotePath, renamedPath, error, false),
                   "rename should succeed", error);

    bool downloadProgressCalled = false;
    error.clear();
    checkOperation(
        client.get(
            renamedPath, localDownload.string(), error,
            [&](std::size_t, std::size_t) { downloadProgressCalled = true; },
            {}, false),
        "download should succeed", error);
    test.check(downloadProgressCalled,
               message("download progress callback should be called"));
    std::string downloaded;
    test.check(readFile(localDownload, downloaded),
               message("downloaded file should be readable"));
    test.check(downloaded == payload,
               message("downloaded content should match the upload"));

    test.check(writeFile(canceledDownload, "keep existing destination"),
               message("should prepare an existing download destination"));
    error.clear();
    test.check(!client.get(
                   renamedPath, canceledDownload.string(), error, {},
                   [] { return true; }, false),
               message("an immediately canceled download should fail"));
    std::string preserved;
    test.check(readFile(canceledDownload, preserved) &&
                   preserved == "keep existing destination",
               message("an immediately canceled download must preserve the "
                       "destination"));
    test.check(std::filesystem::exists(canceledDownload.string() + ".part"),
               message("an immediately canceled download should retain its "
                       "partial file"));

    test.check(writeFile(boundaryCanceledDownload, "keep boundary destination"),
               message("should prepare a late-cancel destination"));
    std::atomic<bool> cancelFinishedDownload{false};
    error.clear();
    test.check(
        !client.get(
            renamedPath, boundaryCanceledDownload.string(), error,
            [&](std::size_t done, std::size_t) {
                if (done >= payload.size())
                    cancelFinishedDownload.store(true);
            },
            [&] { return cancelFinishedDownload.load(); }, false),
        message("cancellation at download completion should prevent the final "
                "replace"));
    preserved.clear();
    test.check(readFile(boundaryCanceledDownload, preserved) &&
                   preserved == "keep boundary destination",
               message("a late-canceled download must preserve the "
                       "destination"));
    test.check(
        std::filesystem::exists(boundaryCanceledDownload.string() + ".part"),
        message("a late-canceled download should retain its partial file"));

    std::atomic<bool> cancelFinishedUpload{false};
    error.clear();
    test.check(
        !client.put(
            localUpload.string(), canceledUploadPath, error,
            [&](std::size_t done, std::size_t) {
                if (done >= payload.size())
                    cancelFinishedUpload.store(true);
            },
            [&] { return cancelFinishedUpload.load(); }, false),
        message("cancellation at upload completion should prevent publishing"));
    bool canceledUploadIsDirectory = false;
    error.clear();
    const bool canceledUploadExists =
        client.exists(canceledUploadPath, canceledUploadIsDirectory, error);
    test.check(!canceledUploadExists && error.empty(),
               message("a late-canceled upload must not publish its final "
                       "destination"));
    if (canceledUploadExists) {
        error.clear();
        (void)client.removeFile(canceledUploadPath, error);
    }
    const std::string canceledUploadPartial = canceledUploadPath + ".part";
    bool canceledPartialIsDirectory = false;
    error.clear();
    const bool canceledPartialExists =
        client.exists(canceledUploadPartial, canceledPartialIsDirectory, error);
    test.check(canceledPartialExists,
               message("a late-canceled upload should retain its remote "
                       "partial file"));
    if (canceledPartialExists) {
        error.clear();
        checkOperation(client.removeFile(canceledUploadPartial, error),
                       "remote partial cleanup should succeed", error);
    }

    std::vector<openscp::FileInfo> listing;
    error.clear();
    checkOperation(client.list(remoteDir, listing, error),
                   "listing should succeed", error);
    test.check(std::any_of(listing.begin(), listing.end(),
                           [&](const openscp::FileInfo &file) {
                               return file.name == renamedName;
                           }),
               message("listing should include the renamed file"));
    test.check(std::none_of(listing.begin(), listing.end(),
                            [&](const openscp::FileInfo &file) {
                                return file.name == uploadName + ".part";
                            }),
               message("a successful upload should not leave a remote "
                       "partial file"));

    error.clear();
    checkOperation(client.removeFile(renamedPath, error),
                   "file deletion should succeed", error);
    pathIsDirectory = false;
    error.clear();
    test.check(!client.exists(renamedPath, pathIsDirectory, error) &&
                   error.empty(),
               message("the deleted file should not exist or report an "
                       "error"));
    error.clear();
    checkOperation(client.removeDir(remoteDir, error),
                   "directory deletion should succeed", error);
    client.disconnect();
}

inline int finishIntegration(std::string_view suiteName,
                             const TestContext &test) {
    if (test.failures != 0) {
        std::cerr << "[FAIL] " << suiteName << " failures=" << test.failures
                  << '\n';
        return EXIT_FAILURE;
    }
    std::cout << "[OK] " << suiteName << '\n';
    return EXIT_SUCCESS;
}

class ScopedEnvironment {
    public:
    ScopedEnvironment() = default;
    ~ScopedEnvironment() {
        for (auto it = previous_.rbegin(); it != previous_.rend(); ++it)
            restore(it->first, it->second);
    }

    ScopedEnvironment(const ScopedEnvironment &) = delete;
    ScopedEnvironment &operator=(const ScopedEnvironment &) = delete;

    void set(const std::string &name, const std::string &value) {
        const char *old = std::getenv(name.c_str());
        previous_.emplace_back(name, old ? std::optional<std::string>(old)
                                         : std::nullopt);
#ifdef _WIN32
        (void)_putenv_s(name.c_str(), value.c_str());
#else
        (void)::setenv(name.c_str(), value.c_str(), 1);
#endif
    }

    private:
    static void restore(const std::string &name,
                        const std::optional<std::string> &value) {
#ifdef _WIN32
        (void)_putenv_s(name.c_str(), value ? value->c_str() : "");
#else
        if (value)
            (void)::setenv(name.c_str(), value->c_str(), 1);
        else
            (void)::unsetenv(name.c_str());
#endif
    }

    std::vector<std::pair<std::string, std::optional<std::string>>> previous_;
};

inline void forceUnreachableEnvironmentProxies(ScopedEnvironment &environment) {
    static constexpr const char *proxyVariables[] = {
        "ALL_PROXY",  "all_proxy",  "FTP_PROXY",   "ftp_proxy",
        "HTTP_PROXY", "http_proxy", "HTTPS_PROXY", "https_proxy",
    };
    for (const char *name : proxyVariables)
        environment.set(name, "http://127.0.0.1:1");
    environment.set("NO_PROXY", "");
    environment.set("no_proxy", "");
}

} // namespace openscp::testsupport
