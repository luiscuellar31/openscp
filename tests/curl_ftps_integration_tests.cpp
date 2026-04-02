// Integration tests for CurlFtpClient against a real FTPS server.
// Skips with exit code 77 unless required OPENSCP_IT_FTPS_* vars exist.
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

bool parseBool(const std::optional<std::string> &raw, bool fallback) {
    if (!raw.has_value())
        return fallback;
    const std::string v = *raw;
    if (v == "1" || v == "true" || v == "TRUE" || v == "yes" || v == "YES")
        return true;
    if (v == "0" || v == "false" || v == "FALSE" || v == "no" || v == "NO")
        return false;
    return fallback;
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
    const auto host = envValue("OPENSCP_IT_FTPS_HOST");
    const auto user = envValue("OPENSCP_IT_FTPS_USER");
    const auto pass = envValue("OPENSCP_IT_FTPS_PASS");
    const auto remoteBase = envValue("OPENSCP_IT_FTPS_REMOTE_BASE");
    const auto caCert = envValue("OPENSCP_IT_FTPS_CA_CERT");
    const bool verifyPeer =
        parseBool(envValue("OPENSCP_IT_FTPS_VERIFY_PEER"), true);

    if (!host.has_value() || !remoteBase.has_value()) {
        std::cout << "[SKIP] openscp_ftps_integration_tests requires "
                  << "OPENSCP_IT_FTPS_HOST and OPENSCP_IT_FTPS_REMOTE_BASE\n";
        return kSkipExitCode;
    }

    std::uint16_t port = 990;
    if (!parsePort(envValue("OPENSCP_IT_FTPS_PORT"), port, 990)) {
        std::cerr << "[FAIL] OPENSCP_IT_FTPS_PORT is invalid\n";
        return EXIT_FAILURE;
    }

    auto client = openscp::CreateClientForProtocol(openscp::Protocol::Ftps);
    if (!client) {
        std::cerr << "[FAIL] factory did not create FTPS backend\n";
        return EXIT_FAILURE;
    }

    openscp::SessionOptions opt;
    opt.protocol = openscp::Protocol::Ftps;
    opt.host = *host;
    opt.port = port;
    opt.username = user.value_or("anonymous");
    if (pass.has_value())
        opt.password = pass;
    opt.ftps_verify_peer = verifyPeer;
    if (caCert.has_value())
        opt.ftps_ca_cert_path = *caCert;

    std::string err;
    if (!client->connect(opt, err)) {
        std::cerr << "[FAIL] FTPS connect failed: " << err << "\n";
        return EXIT_FAILURE;
    }

    TestContext t;
    t.check(client->protocol() == openscp::Protocol::Ftps,
            "FTPS client should report FTPS protocol");
    const auto caps = client->capabilities();
    t.check(caps.implemented, "FTPS should be marked implemented");
    t.check(caps.supports_file_transfers, "FTPS should support transfers");
    t.check(caps.supports_listing, "FTPS should support remote listing");

    const std::string token = uniqueToken();
    const fs::path tempDir =
        fs::temp_directory_path() / ("openscp_ftps_" + token);
    const fs::path localUpload = tempDir / "upload.txt";
    const fs::path localDownload = tempDir / "download.txt";
    fs::create_directories(tempDir);

    const std::string payload =
        "openscp ftps integration payload " + token + "\nline two\n";
    t.check(writeFile(localUpload, payload), "should write local upload file");

    const std::string remotePath =
        joinRemotePath(*remoteBase, "openscp_ftps_it_" + token + ".txt");

    bool uploadProgressCalled = false;
    err.clear();
    t.check(client->put(localUpload.string(), remotePath, err,
                        [&](std::size_t done, std::size_t total) {
                            (void)done;
                            (void)total;
                            uploadProgressCalled = true;
                        },
                        {}, false),
            std::string("FTPS upload should succeed: ") + err);
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
            std::string("FTPS download should succeed: ") + err);
    t.check(downloadProgressCalled,
            "download progress callback should be called");

    std::string downloaded;
    t.check(readFile(localDownload, downloaded),
            "downloaded file should be readable");
    t.check(downloaded == payload, "downloaded content should match uploaded");

    std::vector<openscp::FileInfo> listing;
    err.clear();
    t.check(client->list(*remoteBase, listing, err),
            std::string("FTPS listing should succeed: ") + err);
    const std::string remoteFileName = "openscp_ftps_it_" + token + ".txt";
    const auto listed = std::find_if(
        listing.begin(), listing.end(),
        [&](const openscp::FileInfo &f) { return f.name == remoteFileName; });
    t.check(listed != listing.end(),
            "FTPS listing should include the uploaded file");

    client->disconnect();
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    if (t.failures != 0) {
        std::cerr << "[FAIL] openscp_ftps_integration_tests failures="
                  << t.failures << "\n";
        return EXIT_FAILURE;
    }
    std::cout << "[OK] openscp_ftps_integration_tests\n";
    return EXIT_SUCCESS;
}
