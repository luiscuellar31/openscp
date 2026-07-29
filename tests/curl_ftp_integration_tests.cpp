// Integration tests for CurlFtpClient against a real FTP server.
// Skips with exit code 77 unless required OPENSCP_IT_FTP_* vars exist.
#include "curl_integration_test_support.hpp"
#include "openscp/ClientFactory.hpp"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kSkipExitCode = 77;

struct TestContext {
    int failures = 0;

    void check(bool cond, const std::string &msg) {
        if (!cond) {
            ++failures;
            std::cerr << "[FAIL] " << msg << "\n";
        }
    }
};

std::optional<std::string> envValue(const char *key) {
    const char *raw = std::getenv(key);
    if (!raw || !*raw)
        return std::nullopt;
    return std::string(raw);
}

bool parsePort(const std::optional<std::string> &raw, std::uint16_t &out,
               std::uint16_t fallback) {
    if (!raw.has_value()) {
        out = fallback;
        return true;
    }
    try {
        const int n = std::stoi(*raw);
        if (n < 1 || n > 65535)
            return false;
        out = static_cast<std::uint16_t>(n);
        return true;
    } catch (...) {
        return false;
    }
}

std::string uniqueToken() {
    const auto now =
        std::chrono::steady_clock::now().time_since_epoch().count();
    return std::to_string(static_cast<long long>(now));
}

std::string joinRemotePath(const std::string &base, const std::string &name) {
    if (base.empty())
        return std::string("/") + name;
    if (base.back() == '/')
        return base + name;
    return base + "/" + name;
}

bool writeFile(const fs::path &path, const std::string &content) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out.is_open())
        return false;
    out << content;
    return out.good();
}

bool readFile(const fs::path &path, std::string &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return false;
    out.assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
    return true;
}

} // namespace

int main() {
    const auto host = envValue("OPENSCP_IT_FTP_HOST");
    const auto user = envValue("OPENSCP_IT_FTP_USER");
    const auto pass = envValue("OPENSCP_IT_FTP_PASS");
    const auto remoteBase = envValue("OPENSCP_IT_FTP_REMOTE_BASE");

    if (!host.has_value() || !remoteBase.has_value()) {
        std::cout << "[SKIP] openscp_ftp_integration_tests requires "
                  << "OPENSCP_IT_FTP_HOST and OPENSCP_IT_FTP_REMOTE_BASE\n";
        return kSkipExitCode;
    }

    std::uint16_t port = 21;
    if (!parsePort(envValue("OPENSCP_IT_FTP_PORT"), port, 21)) {
        std::cerr << "[FAIL] OPENSCP_IT_FTP_PORT is invalid\n";
        return EXIT_FAILURE;
    }

    auto client = openscp::CreateClientForProtocol(openscp::Protocol::Ftp);
    if (!client) {
        std::cerr << "[FAIL] factory did not create FTP backend\n";
        return EXIT_FAILURE;
    }

    openscp::SessionOptions opt;
    opt.protocol = openscp::Protocol::Ftp;
    opt.host = *host;
    opt.port = port;
    opt.username = user.value_or("anonymous");
    if (pass.has_value())
        opt.password = pass;

    openscp::testsupport::ScopedEnvironment proxyEnvironment;
    openscp::testsupport::forceUnreachableEnvironmentProxies(proxyEnvironment);

    std::string err;
    if (!client->connect(opt, err)) {
        std::cerr << "[FAIL] FTP connect failed: " << err << "\n";
        return EXIT_FAILURE;
    }

    TestContext t;
    t.check(client->protocol() == openscp::Protocol::Ftp,
            "FTP client should report FTP protocol");
    const auto caps = client->capabilities();
    t.check(caps.implemented, "FTP should be marked implemented");
    t.check(caps.supports_file_transfers, "FTP should support transfers");
    t.check(caps.supports_listing, "FTP should support remote listing");
    t.check(caps.can_stat && caps.can_mkdir && caps.can_delete &&
                caps.can_rename,
            "FTP should advertise remote CRUD operations");

    const std::string token = uniqueToken();
    const fs::path tempDir = fs::temp_directory_path() / ("openscp_ftp_" + token);
    const fs::path localUpload = tempDir / "upload.txt";
    const fs::path localDownload = tempDir / "download.txt";
    const fs::path canceledDownload = tempDir / "canceled.txt";
    const fs::path boundaryCanceledDownload =
        tempDir / "boundary-canceled.txt";
    fs::create_directories(tempDir);

    const std::string payload =
        "openscp ftp integration payload " + token + "\nline two\n";
    t.check(writeFile(localUpload, payload), "should write local upload file");

    const std::string remoteDir =
        joinRemotePath(*remoteBase, "openscp_ftp_it_" + token);
    const std::string remotePath = joinRemotePath(remoteDir, "upload file.txt");
    const std::string renamedPath =
        joinRemotePath(remoteDir, "renamed file.txt");
    const std::string canceledUploadPath =
        joinRemotePath(remoteDir, "canceled upload.txt");

    err.clear();
    t.check(client->mkdir(remoteDir, err),
            std::string("FTP mkdir should succeed: ") + err);

    bool uploadProgressCalled = false;
    err.clear();
    t.check(client->put(localUpload.string(), remotePath, err,
                        [&](std::size_t done, std::size_t total) {
                            (void)done;
                            (void)total;
                            uploadProgressCalled = true;
                        },
                        {}, false),
            std::string("FTP upload should succeed: ") + err);
    t.check(uploadProgressCalled, "upload progress callback should be called");

    bool pathIsDir = true;
    err.clear();
    t.check(client->exists(remotePath, pathIsDir, err),
            std::string("FTP exists should find uploaded file: ") + err);
    t.check(!pathIsDir, "FTP exists should report uploaded path as a file");

    openscp::FileInfo statInfo{};
    err.clear();
    t.check(client->stat(remotePath, statInfo, err),
            std::string("FTP stat should succeed: ") + err);
    t.check(statInfo.has_size && statInfo.size == payload.size(),
            "FTP stat should report the uploaded file size");

    err.clear();
    t.check(client->rename(remotePath, renamedPath, err, false),
            std::string("FTP rename should succeed: ") + err);

    bool downloadProgressCalled = false;
    err.clear();
    t.check(client->get(renamedPath, localDownload.string(), err,
                        [&](std::size_t done, std::size_t total) {
                            (void)done;
                            (void)total;
                            downloadProgressCalled = true;
                        },
                        {}, false),
            std::string("FTP download should succeed: ") + err);
    t.check(downloadProgressCalled,
            "download progress callback should be called");

    std::string downloaded;
    t.check(readFile(localDownload, downloaded),
            "downloaded file should be readable");
    t.check(downloaded == payload, "downloaded content should match uploaded");

    t.check(writeFile(canceledDownload, "keep existing destination"),
            "should prepare an existing download destination");
    err.clear();
    t.check(!client->get(renamedPath, canceledDownload.string(), err, {},
                         [] { return true; }, false),
            "canceled FTP download should fail");
    std::string preserved;
    t.check(readFile(canceledDownload, preserved) &&
                preserved == "keep existing destination",
            "canceled FTP download must preserve the existing destination");
    t.check(fs::exists(canceledDownload.string() + ".part"),
            "canceled FTP download should retain its partial file");

    t.check(writeFile(boundaryCanceledDownload, "keep boundary destination"),
            "should prepare a boundary-cancel destination");
    std::atomic<bool> cancelFinishedDownload{false};
    err.clear();
    t.check(
        !client->get(
            renamedPath, boundaryCanceledDownload.string(), err,
            [&](std::size_t done, std::size_t total) {
                (void)total;
                if (done >= payload.size())
                    cancelFinishedDownload.store(true);
            },
            [&] { return cancelFinishedDownload.load(); }, false),
        "FTP cancellation at transfer completion should prevent final replace");
    preserved.clear();
    t.check(readFile(boundaryCanceledDownload, preserved) &&
                preserved == "keep boundary destination",
            "late FTP cancellation must preserve the existing destination");
    t.check(fs::exists(boundaryCanceledDownload.string() + ".part"),
            "late-canceled FTP download should retain its partial file");

    std::atomic<bool> cancelFinishedUpload{false};
    err.clear();
    t.check(
        !client->put(
            localUpload.string(), canceledUploadPath, err,
            [&](std::size_t done, std::size_t total) {
                (void)total;
                if (done >= payload.size())
                    cancelFinishedUpload.store(true);
            },
            [&] { return cancelFinishedUpload.load(); }, false),
        "FTP cancellation at upload completion should prevent RNFR/RNTO");
    bool canceledUploadIsDir = false;
    err.clear();
    const bool canceledUploadExists =
        client->exists(canceledUploadPath, canceledUploadIsDir, err);
    t.check(!canceledUploadExists && err.empty(),
            "late-canceled FTP upload must not publish the final destination");
    if (canceledUploadExists) {
        err.clear();
        (void)client->removeFile(canceledUploadPath, err);
    }
    const std::string canceledUploadPartial = canceledUploadPath + ".part";
    bool canceledPartialIsDir = false;
    err.clear();
    const bool canceledPartialExists =
        client->exists(canceledUploadPartial, canceledPartialIsDir, err);
    t.check(canceledPartialExists,
            "late-canceled FTP upload should retain its remote partial");
    if (canceledPartialExists) {
        err.clear();
        t.check(client->removeFile(canceledUploadPartial, err),
                std::string("FTP canceled upload partial cleanup failed: ") +
                    err);
    }

    std::vector<openscp::FileInfo> listing;
    err.clear();
    t.check(client->list(remoteDir, listing, err),
            std::string("FTP listing should succeed: ") + err);
    const std::string remoteFileName = "renamed file.txt";
    const auto listed = std::find_if(
        listing.begin(), listing.end(),
        [&](const openscp::FileInfo &f) { return f.name == remoteFileName; });
    t.check(listed != listing.end(),
            "FTP listing should include the uploaded file");
    t.check(std::none_of(listing.begin(), listing.end(),
                         [](const openscp::FileInfo &f) {
                             return f.name == "upload file.txt.part";
                         }),
            "successful FTP upload should not leave a remote partial file");

    err.clear();
    t.check(client->removeFile(renamedPath, err),
            std::string("FTP file deletion should succeed: ") + err);
    pathIsDir = false;
    err.clear();
    t.check(!client->exists(renamedPath, pathIsDir, err) && err.empty(),
            "deleted FTP file should no longer exist");
    err.clear();
    t.check(client->removeDir(remoteDir, err),
            std::string("FTP directory deletion should succeed: ") + err);

    err.clear();
    t.check(!client->removeFile("/safe\r\nDELE /important", err),
            "FTP must reject command-injection path characters");

    client->disconnect();
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    if (t.failures != 0) {
        std::cerr << "[FAIL] openscp_ftp_integration_tests failures="
                  << t.failures << "\n";
        return EXIT_FAILURE;
    }
    std::cout << "[OK] openscp_ftp_integration_tests\n";
    return EXIT_SUCCESS;
}
