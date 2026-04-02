// Integration tests for CurlFtpClient against a real FTP server.
// Skips with exit code 77 unless required OPENSCP_IT_FTP_* vars exist.
#include "openscp/ClientFactory.hpp"

#include <algorithm>
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

    const std::string token = uniqueToken();
    const fs::path tempDir = fs::temp_directory_path() / ("openscp_ftp_" + token);
    const fs::path localUpload = tempDir / "upload.txt";
    const fs::path localDownload = tempDir / "download.txt";
    fs::create_directories(tempDir);

    const std::string payload =
        "openscp ftp integration payload " + token + "\nline two\n";
    t.check(writeFile(localUpload, payload), "should write local upload file");

    const std::string remotePath =
        joinRemotePath(*remoteBase, "openscp_ftp_it_" + token + ".txt");

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

    bool downloadProgressCalled = false;
    err.clear();
    t.check(client->get(remotePath, localDownload.string(), err,
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

    std::vector<openscp::FileInfo> listing;
    err.clear();
    t.check(client->list(*remoteBase, listing, err),
            std::string("FTP listing should succeed: ") + err);
    const std::string remoteFileName = "openscp_ftp_it_" + token + ".txt";
    const auto listed = std::find_if(
        listing.begin(), listing.end(),
        [&](const openscp::FileInfo &f) { return f.name == remoteFileName; });
    t.check(listed != listing.end(),
            "FTP listing should include the uploaded file");

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
