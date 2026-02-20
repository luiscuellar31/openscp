// Integration tests for real Libssh2SftpClient against a test SFTP server.
// The test is skipped (exit code 77) unless required OPEN_SCP_IT_* env vars
// exist.
#include "openscp/Libssh2SftpClient.hpp"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
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

bool readFile(const fs::path &p, std::string &out) {
    std::ifstream in(p, std::ios::binary);
    if (!in.is_open())
        return false;
    out.assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
    return true;
}

bool parsePort(const std::optional<std::string> &raw, std::uint16_t &out) {
    if (!raw.has_value()) {
        out = 22;
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

bool listContainsName(const std::vector<openscp::FileInfo> &entries,
                      const std::string &name) {
    return std::any_of(entries.begin(), entries.end(),
                       [&name](const openscp::FileInfo &e) {
                           return e.name == name;
                       });
}

} // namespace

int main() {
    const auto host = envValue("OPEN_SCP_IT_SFTP_HOST");
    const auto user = envValue("OPEN_SCP_IT_SFTP_USER");
    const auto pass = envValue("OPEN_SCP_IT_SFTP_PASS");
    const auto keyPath = envValue("OPEN_SCP_IT_SFTP_KEY");
    const auto keyPassphrase = envValue("OPEN_SCP_IT_SFTP_KEY_PASSPHRASE");
    const std::string remoteBase =
        envValue("OPEN_SCP_IT_REMOTE_BASE").value_or("/tmp");

    if (!host.has_value() || !user.has_value() ||
        (!pass.has_value() && !keyPath.has_value())) {
        std::cout << "[SKIP] openscp_sftp_integration_tests requires env vars: "
                  << "OPEN_SCP_IT_SFTP_HOST, OPEN_SCP_IT_SFTP_USER and one "
                     "auth method "
                  << "(OPEN_SCP_IT_SFTP_PASS or OPEN_SCP_IT_SFTP_KEY)\n";
        return kSkipExitCode;
    }
    if (keyPath.has_value() && !fs::exists(*keyPath)) {
        std::cerr << "[FAIL] OPEN_SCP_IT_SFTP_KEY does not exist: " << *keyPath
                  << "\n";
        return EXIT_FAILURE;
    }

    std::uint16_t port = 22;
    if (!parsePort(envValue("OPEN_SCP_IT_SFTP_PORT"), port)) {
        std::cerr << "[FAIL] OPEN_SCP_IT_SFTP_PORT is invalid\n";
        return EXIT_FAILURE;
    }

    TestContext t;
    openscp::SessionOptions opt;
    opt.host = *host;
    opt.port = port;
    opt.username = *user;
    if (pass.has_value())
        opt.password = *pass;
    if (keyPath.has_value()) {
        opt.private_key_path = *keyPath;
        if (keyPassphrase.has_value())
            opt.private_key_passphrase = *keyPassphrase;
    }
    opt.known_hosts_policy = openscp::KnownHostsPolicy::Off;
    opt.transfer_integrity_policy = openscp::TransferIntegrityPolicy::Required;

    const std::string token = uniqueToken();
    const std::string remoteSuiteDir =
        joinRemotePath(remoteBase, "openscp-it-" + token);
    const std::string remoteSrc = joinRemotePath(remoteSuiteDir, "payload.txt");
    const std::string remoteMoved =
        joinRemotePath(remoteSuiteDir, "payload-moved.txt");

    const fs::path localTmpRoot =
        fs::temp_directory_path() / ("openscp-it-" + token);
    std::error_code ec;
    fs::create_directories(localTmpRoot, ec);
    if (ec) {
        std::cerr << "[FAIL] could not create temp dir: " << ec.message()
                  << "\n";
        return EXIT_FAILURE;
    }

    const fs::path localSrc = localTmpRoot / "payload.txt";
    const fs::path localDst = localTmpRoot / "payload-downloaded.txt";
    const std::string payload = "OpenSCP integration payload\nline-2\n";
    {
        std::ofstream out(localSrc, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "[FAIL] could not create source file\n";
            fs::remove_all(localTmpRoot, ec);
            return EXIT_FAILURE;
        }
        out << payload;
    }

    openscp::Libssh2SftpClient client;
    std::string err;

    const bool connected = client.connect(opt, err);
    t.check(connected, std::string("connect should succeed: ") + err);
    if (t.failures == 0) {
        err.clear();
        t.check(client.mkdir(remoteSuiteDir, err, 0755),
                std::string("mkdir remoteSuiteDir should succeed: ") + err);
    }
    if (t.failures == 0) {
        err.clear();
        t.check(client.put(localSrc.string(), remoteSrc, err, {}, {}, false),
                std::string("put should succeed: ") + err);
    }
    if (t.failures == 0) {
        bool isDir = true;
        err.clear();
        const bool ex = client.exists(remoteSrc, isDir, err);
        t.check(ex, std::string("exists(remoteSrc) should be true: ") + err);
        t.check(!isDir, "exists(remoteSrc) should report file");
    }
    if (t.failures == 0) {
        openscp::FileInfo st{};
        err.clear();
        t.check(client.stat(remoteSrc, st, err),
                std::string("stat(remoteSrc) should succeed: ") + err);
        t.check(st.has_size, "stat(remoteSrc) should report size");
        t.check(st.size == payload.size(),
                "remote file size should match payload size");
    }
    if (t.failures == 0) {
        std::vector<openscp::FileInfo> entries;
        err.clear();
        t.check(client.list(remoteSuiteDir, entries, err),
                std::string("list(remoteSuiteDir) should succeed: ") + err);
        t.check(listContainsName(entries, "payload.txt"),
                "list should include payload.txt");
    }
    if (t.failures == 0) {
        err.clear();
        t.check(client.get(remoteSrc, localDst.string(), err, {}, {}, false),
                std::string("get should succeed: ") + err);
        std::string downloaded;
        t.check(readFile(localDst, downloaded),
                "downloaded file should be readable");
        t.check(downloaded == payload,
                "downloaded content should match uploaded payload");
    }
    if (t.failures == 0) {
        err.clear();
        t.check(client.rename(remoteSrc, remoteMoved, err, false),
                std::string("rename should succeed: ") + err);
        bool isDir = false;
        err.clear();
        const bool oldExists = client.exists(remoteSrc, isDir, err);
        t.check(!oldExists, "old path should not exist after rename");
    }
    if (t.failures == 0) {
        err.clear();
        t.check(client.removeFile(remoteMoved, err),
                std::string("removeFile should succeed: ") + err);
        err.clear();
        t.check(client.removeDir(remoteSuiteDir, err),
                std::string("removeDir should succeed: ") + err);
    }

    // Best-effort cleanup regardless of test result.
    std::string cleanupErr;
    (void)client.removeFile(remoteSrc, cleanupErr);
    cleanupErr.clear();
    (void)client.removeFile(remoteMoved, cleanupErr);
    cleanupErr.clear();
    (void)client.removeDir(remoteSuiteDir, cleanupErr);
    client.disconnect();
    fs::remove_all(localTmpRoot, ec);

    if (t.failures != 0) {
        std::cerr << "[FAILURES] " << t.failures << "\n";
        return EXIT_FAILURE;
    }
    std::cout << "[OK] openscp_sftp_integration_tests\n";
    return EXIT_SUCCESS;
}
