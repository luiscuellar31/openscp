// Integration tests for real Libssh2SftpClient against a test SFTP server.
// The test is skipped (exit code 77) unless required OPENSCP_IT_* env vars
// exist.
#include "IntegrationTestSupport.hpp"
#include "TestHarness.hpp"
#include "common/UniqueFile.hpp"
#include "libssh2/Libssh2SftpClient.hpp"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

namespace {

using openscp::testsupport::envValue;
using openscp::testsupport::joinRemotePath;
using openscp::testsupport::kSkipExitCode;
using openscp::testsupport::parsePort;
using openscp::testsupport::readFile;
using openscp::testsupport::uniqueToken;
using openscp::testsupport::writeFile;

// An ED25519 key that no test server holds.
constexpr const char *kFakeEd25519Key =
    "AAAAC3NzaC1lZDI1NTE5AAAAILZlz+tnMZZGpyX4/"
    "qwU9iIfMHkUqPnwGwGZRuQQ3v1d";

// Appended to a shell command, to drop the messages it writes to standard
// error, or everything it writes.
#ifdef _WIN32
constexpr const char *kDiscardErrors = " 2>NUL";
constexpr const char *kDiscardOutput = " >NUL 2>&1";
#else
constexpr const char *kDiscardErrors = " 2>/dev/null";
constexpr const char *kDiscardOutput = " >/dev/null 2>&1";
#endif

std::string knownHostsHostToken(const std::string &host, std::uint16_t port) {
    if (port == 22)
        return host;
    return "[" + host + "]:" + std::to_string(port);
}

// The known_hosts lines ssh-keyscan prints for the server, one per host key,
// or only for keys of `keyType` when given. None when it cannot run.
std::vector<std::string> scanHostKeys(const std::string &host,
                                      std::uint16_t port,
                                      const std::string &keyType = {}) {
    std::string command = "ssh-keyscan -T 5";
    if (!keyType.empty())
        command += " -t " + keyType;
    command += " -p " + std::to_string(port) + " " + host + kDiscardErrors;
#ifdef _WIN32
    openscp::UniqueFile scan(::_popen(command.c_str(), "r"), &::_pclose);
#else
    openscp::UniqueFile scan(::popen(command.c_str(), "r"), &::pclose);
#endif
    std::vector<std::string> keys;
    if (!scan)
        return keys;
    char buffer[4096];
    while (std::fgets(buffer, sizeof(buffer), scan.get())) {
        std::string line(buffer);
        while (!line.empty() && (line.back() == '\n' || line.back() == '\r'))
            line.pop_back();
        if (!line.empty() && line[0] != '#')
            keys.push_back(line);
    }
    return keys;
}

bool parseProxyType(const std::optional<std::string> &raw,
                    openscp::ProxyType &out) {
    if (!raw.has_value() || raw->empty()) {
        out = openscp::ProxyType::None;
        return true;
    }
    std::string v = *raw;
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) {
        return static_cast<char>(std::tolower(c));
    });
    if (v == "none" || v == "direct" || v == "off") {
        out = openscp::ProxyType::None;
        return true;
    }
    if (v == "socks5" || v == "socks") {
        out = openscp::ProxyType::Socks5;
        return true;
    }
    if (v == "http" || v == "http_connect" || v == "http-connect" ||
        v == "connect") {
        out = openscp::ProxyType::HttpConnect;
        return true;
    }
    return false;
}

bool listContainsName(const std::vector<openscp::FileInfo> &entries,
                      const std::string &name) {
    return std::any_of(
        entries.begin(), entries.end(),
        [&name](const openscp::FileInfo &e) { return e.name == name; });
}

bool removeRemoteFileIfExists(openscp::Libssh2SftpClient &client,
                              const std::string &remotePath, std::string &err) {
    bool isDir = false;
    err.clear();
    const bool exists = client.exists(remotePath, isDir, err);
    if (!exists) {
        return err.empty();
    }
    if (isDir) {
        err = "path is a directory";
        return false;
    }
    err.clear();
    return client.removeFile(remotePath, err);
}

#ifndef _WIN32
std::string formatHostPortAuthority(const std::string &host,
                                    std::uint16_t port) {
    if (host.find(':') != std::string::npos &&
        host.find(']') == std::string::npos)
        return "[" + host + "]:" + std::to_string(port);
    return host + ":" + std::to_string(port);
}

std::size_t countJumpTunnelProcesses(const std::string &targetHost,
                                     std::uint16_t targetPort,
                                     const std::string &jumpHost,
                                     std::uint16_t jumpPort) {
    const std::string needleTarget =
        std::string("-W ") + formatHostPortAuthority(targetHost, targetPort);
    const std::string needlePort =
        std::string("-p ") + std::to_string(jumpPort);

    openscp::UniqueFile pipe(::popen("ps -ax -o command=", "r"), &::pclose);
    if (!pipe)
        return 0;

    std::size_t matches = 0;
    char lineBuf[4096];
    while (std::fgets(lineBuf, sizeof(lineBuf), pipe.get())) {
        std::string line(lineBuf);
        if (!line.empty() && line.back() == '\n')
            line.pop_back();
        if (line.find("ssh") == std::string::npos)
            continue;
        if (line.find(needleTarget) == std::string::npos)
            continue;
        if (line.find(needlePort) == std::string::npos)
            continue;
        if (line.find(jumpHost) == std::string::npos)
            continue;
        ++matches;
    }
    return matches;
}

bool waitForJumpTunnelCountAtMost(const std::string &targetHost,
                                  std::uint16_t targetPort,
                                  const std::string &jumpHost,
                                  std::uint16_t jumpPort,
                                  std::size_t baselineCount,
                                  std::chrono::milliseconds timeout) {
    const auto deadline = std::chrono::steady_clock::now() + timeout;
    for (;;) {
        if (countJumpTunnelProcesses(targetHost, targetPort, jumpHost,
                                     jumpPort) <= baselineCount) {
            return true;
        }
        if (std::chrono::steady_clock::now() >= deadline)
            break;
        std::this_thread::sleep_for(std::chrono::milliseconds(120));
    }
    return countJumpTunnelProcesses(targetHost, targetPort, jumpHost,
                                    jumpPort) <= baselineCount;
}
#endif

} // namespace

int main() {
    const auto host = envValue("OPENSCP_IT_SFTP_HOST");
    const auto user = envValue("OPENSCP_IT_SFTP_USER");
    const auto pass = envValue("OPENSCP_IT_SFTP_PASS");
    const auto keyPath = envValue("OPENSCP_IT_SFTP_KEY");
    const auto keyPassphrase = envValue("OPENSCP_IT_SFTP_KEY_PASSPHRASE");
    const auto proxyTypeRaw = envValue("OPENSCP_IT_PROXY_TYPE");
    const auto proxyHost = envValue("OPENSCP_IT_PROXY_HOST");
    const auto proxyUser = envValue("OPENSCP_IT_PROXY_USER");
    const auto proxyPass = envValue("OPENSCP_IT_PROXY_PASS");
    const auto jumpHost = envValue("OPENSCP_IT_JUMP_HOST");
    const auto jumpUser = envValue("OPENSCP_IT_JUMP_USER");
    const auto jumpKey = envValue("OPENSCP_IT_JUMP_KEY");
    const std::string remoteBase =
        envValue("OPENSCP_IT_REMOTE_BASE").value_or("/tmp");

    if (!host.has_value() || !user.has_value() ||
        (!pass.has_value() && !keyPath.has_value())) {
        std::cout << "[SKIP] openscp_sftp_integration_tests requires env vars: "
                  << "OPENSCP_IT_SFTP_HOST, OPENSCP_IT_SFTP_USER and one "
                     "auth method "
                  << "(OPENSCP_IT_SFTP_PASS or OPENSCP_IT_SFTP_KEY)\n";
        return kSkipExitCode;
    }
    if (keyPath.has_value() && !fs::exists(*keyPath)) {
        std::cerr << "[FAIL] OPENSCP_IT_SFTP_KEY does not exist: " << *keyPath
                  << "\n";
        return EXIT_FAILURE;
    }
    if (jumpKey.has_value() && !fs::exists(*jumpKey)) {
        std::cerr << "[FAIL] OPENSCP_IT_JUMP_KEY does not exist: " << *jumpKey
                  << "\n";
        return EXIT_FAILURE;
    }

    std::uint16_t port = 22;
    if (!parsePort(envValue("OPENSCP_IT_SFTP_PORT"), port, 22)) {
        std::cerr << "[FAIL] OPENSCP_IT_SFTP_PORT is invalid\n";
        return EXIT_FAILURE;
    }

    openscp::ProxyType proxyType = openscp::ProxyType::None;
    if (!parseProxyType(proxyTypeRaw, proxyType)) {
        std::cerr << "[FAIL] OPENSCP_IT_PROXY_TYPE is invalid\n";
        return EXIT_FAILURE;
    }
    std::uint16_t proxyPort = 0;
    if (proxyType != openscp::ProxyType::None) {
        if (!proxyHost.has_value()) {
            std::cerr << "[FAIL] OPENSCP_IT_PROXY_HOST is required when "
                         "OPENSCP_IT_PROXY_TYPE is set\n";
            return EXIT_FAILURE;
        }
        const std::uint16_t defaultProxyPort =
            (proxyType == openscp::ProxyType::Socks5) ? 1080 : 8080;
        if (!parsePort(envValue("OPENSCP_IT_PROXY_PORT"), proxyPort,
                       defaultProxyPort)) {
            std::cerr << "[FAIL] OPENSCP_IT_PROXY_PORT is invalid\n";
            return EXIT_FAILURE;
        }
    }
    std::uint16_t jumpPort = 22;
    if (jumpHost.has_value() &&
        !parsePort(envValue("OPENSCP_IT_JUMP_PORT"), jumpPort, 22)) {
        std::cerr << "[FAIL] OPENSCP_IT_JUMP_PORT is invalid\n";
        return EXIT_FAILURE;
    }
    if (proxyType != openscp::ProxyType::None && jumpHost.has_value()) {
        std::cerr << "[FAIL] OPENSCP_IT_PROXY_TYPE and OPENSCP_IT_JUMP_HOST "
                     "cannot be used together in the same run\n";
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
    opt.proxy_type = proxyType;
    if (proxyType != openscp::ProxyType::None) {
        opt.proxy_host = *proxyHost;
        opt.proxy_port = proxyPort;
        if (proxyUser.has_value())
            opt.proxy_username = *proxyUser;
        if (proxyPass.has_value())
            opt.proxy_password = *proxyPass;
    }
    if (jumpHost.has_value()) {
        opt.jump_host = *jumpHost;
        opt.jump_port = jumpPort;
        if (jumpUser.has_value())
            opt.jump_username = *jumpUser;
        if (jumpKey.has_value())
            opt.jump_private_key_path = *jumpKey;
    }

    const std::string token = uniqueToken();
    const std::string remoteSuiteDir =
        joinRemotePath(remoteBase, "openscp-it-" + token);
    const std::string remoteSrc = joinRemotePath(remoteSuiteDir, "payload.txt");
    const std::string remoteMoved =
        joinRemotePath(remoteSuiteDir, "payload-moved.txt");
    const std::string remoteProtected =
        joinRemotePath(remoteSuiteDir, "must-survive-failed-rename.txt");
    const std::string remoteMissing =
        joinRemotePath(remoteSuiteDir, "missing-rename-source.txt");
    const std::string remoteResumeDownload =
        joinRemotePath(remoteSuiteDir, "resume-download.txt");
    const std::string remoteResumeUpload =
        joinRemotePath(remoteSuiteDir, "resume-upload.txt");
    const std::string remoteResumeUploadSeed =
        joinRemotePath(remoteSuiteDir, "resume-upload-seed.txt");
    const std::string remoteResumeUploadPart = remoteResumeUpload + ".part";
    const std::string remoteLarge = joinRemotePath(remoteSuiteDir, "large.bin");

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
    const fs::path localBad = localTmpRoot / "payload-bad.txt";
    const fs::path localDst = localTmpRoot / "payload-downloaded.txt";
    const fs::path localResumeDst =
        localTmpRoot / "payload-resume-downloaded.txt";
    const std::string payload = "OpenSCP integration payload\nline-2\n";
    const std::string badPayload = "CORRUPTED prefix\nline-x\n";
    {
        std::ofstream out(localSrc, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) {
            std::cerr << "[FAIL] could not create source file\n";
            fs::remove_all(localTmpRoot, ec);
            return EXIT_FAILURE;
        }
        out << payload;
    }
    if (!writeFile(localBad, badPayload)) {
        std::cerr << "[FAIL] could not create bad source file\n";
        fs::remove_all(localTmpRoot, ec);
        return EXIT_FAILURE;
    }

    // Regression: TOFU must reject known_hosts mismatches (changed keys),
    // without allowing callback-based override.
    const fs::path tofuMismatchKnownHosts =
        localTmpRoot / "known_hosts_tofu_mismatch";
    {
        std::ofstream khOut(tofuMismatchKnownHosts,
                            std::ios::binary | std::ios::trunc);
        if (!khOut.is_open()) {
            std::cerr << "[FAIL] could not create TOFU mismatch known_hosts\n";
            fs::remove_all(localTmpRoot, ec);
            return EXIT_FAILURE;
        }
        // libssh2_knownhost_readfile() stopped at a line it could not
        // parse, so the changed key after it looked like a new host.
        khOut << "broken line here\n"
              << knownHostsHostToken(*host, port) << " ssh-ed25519 "
              << kFakeEd25519Key << "\n";
    }
    if (t.failures == 0) {
        openscp::Libssh2SftpClient mismatchClient;
        openscp::SessionOptions mismatchOpt = opt;
        mismatchOpt.known_hosts_policy = openscp::KnownHostsPolicy::AcceptNew;
        mismatchOpt.known_hosts_path = tofuMismatchKnownHosts.string();
        bool confirmCalled = false;
        mismatchOpt.hostkey_confirm_cb =
            [&confirmCalled](const std::string &, std::uint16_t,
                             const std::string &, const std::string &, bool) {
                confirmCalled = true;
                return true;
            };
        std::string mismatchErr;
        const bool mismatchOk =
            mismatchClient.connect(mismatchOpt, mismatchErr);
        t.check(!mismatchOk,
                "TOFU connect should fail when known_hosts entry mismatches");
        t.check(mismatchErr.find("does not match known_hosts") !=
                    std::string::npos,
                std::string("TOFU mismatch should report host-key mismatch: ") +
                    mismatchErr);
        t.check(
            !confirmCalled,
            "TOFU mismatch should fail before invoking confirmation callback");
        mismatchClient.disconnect();
    }

    // TOFU with a host that known_hosts does not list: the user is asked, the
    // key is appended under a hashed name, and the rest of the file stays.
    const fs::path tofuNewKnownHosts = localTmpRoot / "known_hosts_tofu_new";
    const std::string tofuNewOriginal =
        "# keep this comment\n\nother.example ssh-ed25519 "
        "AAAAC3NzaC1lZDI1NTE5AAAAILZlz+tnMZZGpyX4/"
        "qwU9iIfMHkUqPnwGwGZRuQQ3v1d\n";
    t.check(writeFile(tofuNewKnownHosts, tofuNewOriginal),
            "TOFU new-host known_hosts should be written");
    if (t.failures == 0) {
        openscp::SessionOptions newHostOpt = opt;
        newHostOpt.known_hosts_policy = openscp::KnownHostsPolicy::AcceptNew;
        newHostOpt.known_hosts_path = tofuNewKnownHosts.string();
        newHostOpt.known_hosts_hash_names = true;
        bool askedToSave = false;
        newHostOpt.hostkey_confirm_cb =
            [&askedToSave](const std::string &, std::uint16_t,
                           const std::string &, const std::string &,
                           bool canSave) {
                askedToSave = canSave;
                return true;
            };
        openscp::Libssh2SftpClient newHostClient;
        std::string newHostErr;
        t.check(newHostClient.connect(newHostOpt, newHostErr) && askedToSave,
                std::string("TOFU should ask about a new host and connect: ") +
                    newHostErr);
        newHostClient.disconnect();
        std::string saved;
        t.check(readFile(tofuNewKnownHosts, saved) &&
                    saved.rfind(tofuNewOriginal, 0) == 0 &&
                    saved.compare(tofuNewOriginal.size(), 3, "|1|") == 0 &&
                    std::count(saved.begin(), saved.end(), '\n') == 4,
                "the accepted key should be appended as one hashed line: " +
                    saved);

        openscp::SessionOptions strictOpt = opt;
        strictOpt.known_hosts_policy = openscp::KnownHostsPolicy::Strict;
        strictOpt.known_hosts_path = tofuNewKnownHosts.string();
        openscp::Libssh2SftpClient strictClient;
        std::string strictErr;
        t.check(strictClient.connect(strictOpt, strictErr),
                std::string("the saved hashed entry should match the host: ") +
                    strictErr);
        strictClient.disconnect();
    }

    // Strict with a host known_hosts does not list reports an unknown host,
    // not a changed key.
    const fs::path strictUnknownKnownHosts =
        localTmpRoot / "known_hosts_strict_unknown";
    if (t.failures == 0 &&
        writeFile(strictUnknownKnownHosts, tofuNewOriginal)) {
        openscp::SessionOptions strictOpt = opt;
        strictOpt.known_hosts_policy = openscp::KnownHostsPolicy::Strict;
        strictOpt.known_hosts_path = strictUnknownKnownHosts.string();
        openscp::Libssh2SftpClient strictClient;
        std::string strictErr;
        t.check(!strictClient.connect(strictOpt, strictErr) &&
                    strictErr == "Unknown host in known_hosts",
                "Strict should report an unknown host: " + strictErr);
        strictClient.disconnect();
    }

    // A host known_hosts records only by its RSA key is asked for that key
    // first, so it matches instead of looking new because it also offers
    // ED25519. ssh-keyscan reads the RSA key, when the server has one.
    std::string rsaLine;
    for (const std::string &line : scanHostKeys(*host, port, "rsa")) {
        if (line.find(" ssh-rsa ") != std::string::npos)
            rsaLine = line + "\n";
    }
    const fs::path rsaOnlyKnownHosts = localTmpRoot / "known_hosts_rsa_only";
    if (t.failures == 0 && !rsaLine.empty() &&
        writeFile(rsaOnlyKnownHosts,
                  knownHostsHostToken(*host, port) +
                      rsaLine.substr(rsaLine.find(" ssh-rsa ")))) {
        openscp::SessionOptions strictOpt = opt;
        strictOpt.known_hosts_policy = openscp::KnownHostsPolicy::Strict;
        strictOpt.known_hosts_path = rsaOnlyKnownHosts.string();
        openscp::Libssh2SftpClient strictClient;
        std::string strictErr;
        t.check(strictClient.connect(strictOpt, strictErr),
                std::string("a host known by its RSA key should match: ") +
                    strictErr);
        strictClient.disconnect();
    } else if (rsaLine.empty()) {
        std::cout << "[SKIP] no RSA host key from ssh-keyscan\n";
    }

    // known_hosts as OpenSSH leaves it: a comment, another host, a
    // certificate authority for this one, which is no key of its own, and
    // every key ssh-keyscan reads from the server, all names hashed by
    // ssh-keygen -H.
    const std::vector<std::string> scannedKeys = scanHostKeys(*host, port);
    const fs::path opensshKnownHosts = localTmpRoot / "known_hosts_openssh";
    std::string opensshLines =
        std::string("# kept by OpenSSH\nother.example ssh-ed25519 ") +
        kFakeEd25519Key + "\n@cert-authority " +
        knownHostsHostToken(*host, port) + " ssh-ed25519 " + kFakeEd25519Key +
        "\n";
    for (const std::string &key : scannedKeys)
        opensshLines += key + "\n";
    const std::string hashCommand = "ssh-keygen -H -f \"" +
                                    opensshKnownHosts.string() + "\"" +
                                    kDiscardOutput;
    std::string hashedLines;
    if (t.failures == 0 && !scannedKeys.empty() &&
        writeFile(opensshKnownHosts, opensshLines) &&
        std::system(hashCommand.c_str()) == 0 &&
        readFile(opensshKnownHosts, hashedLines) &&
        hashedLines.find("other.example") == std::string::npos) {
        openscp::SessionOptions strictOpt = opt;
        strictOpt.known_hosts_policy = openscp::KnownHostsPolicy::Strict;
        strictOpt.known_hosts_path = opensshKnownHosts.string();
        openscp::Libssh2SftpClient strictClient;
        std::string strictErr;
        t.check(strictClient.connect(strictOpt, strictErr),
                "known_hosts as OpenSSH writes it should match the host: " +
                    strictErr);
        strictClient.disconnect();
    } else if (t.failures == 0) {
        std::cout << "[SKIP] no known_hosts from ssh-keyscan and "
                     "ssh-keygen -H\n";
    }

    // Keys marked @revoked are refused, as OpenSSH refuses them: under
    // Strict even where another line records them for the host, and under
    // AcceptNew without offering to trust the host.
    std::string recordedAndRevoked;
    std::string onlyRevoked;
    for (const std::string &key : scannedKeys) {
        const std::string revoked = "@revoked *" + key.substr(key.find(' '));
        recordedAndRevoked += key + "\n" + revoked + "\n";
        onlyRevoked += revoked + "\n";
    }
    const fs::path revokedKnownHosts = localTmpRoot / "known_hosts_revoked";
    for (const auto policy : {openscp::KnownHostsPolicy::Strict,
                              openscp::KnownHostsPolicy::AcceptNew}) {
        const bool strict = policy == openscp::KnownHostsPolicy::Strict;
        if (t.failures != 0 || scannedKeys.empty() ||
            !writeFile(revokedKnownHosts,
                       strict ? recordedAndRevoked : onlyRevoked)) {
            break;
        }
        openscp::SessionOptions revokedOpt = opt;
        revokedOpt.known_hosts_policy = policy;
        revokedOpt.known_hosts_path = revokedKnownHosts.string();
        bool asked = false;
        revokedOpt.hostkey_confirm_cb =
            [&asked](const std::string &, std::uint16_t, const std::string &,
                     const std::string &, bool) {
                asked = true;
                return true;
            };
        openscp::Libssh2SftpClient revokedClient;
        std::string revokedErr;
        t.check(!revokedClient.connect(revokedOpt, revokedErr) && !asked &&
                    revokedErr.find("revoked") != std::string::npos,
                std::string(strict ? "Strict" : "AcceptNew") +
                    " should refuse a revoked host key: " + revokedErr);
        revokedClient.disconnect();
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
    // A failed overwrite rename must never delete the existing destination.
    if (t.failures == 0) {
        err.clear();
        t.check(
            client.put(localSrc.string(), remoteProtected, err, {}, {}, false),
            std::string("protected destination upload should succeed: ") + err);
    }
    std::vector<std::uint8_t> sourceChecksum;
    if (t.failures == 0) {
        std::vector<std::uint8_t> protectedChecksum;
        std::size_t checksumDone = 0;
        std::size_t checksumTotal = 0;
        err.clear();
        t.check(client.checksum(remoteSrc, "SHA-256", sourceChecksum, err,
                                [&](std::size_t done, std::size_t total) {
                                    checksumDone = done;
                                    checksumTotal = total;
                                }),
                std::string("checksum(remoteSrc) should succeed: ") + err);
        err.clear();
        t.check(
            client.checksum(remoteProtected, "sha256", protectedChecksum, err),
            std::string("checksum(remoteProtected) should succeed: ") + err);
        t.check(sourceChecksum.size() == 32 &&
                    sourceChecksum == protectedChecksum,
                "equal remote files should have equal SHA-256 checksums");
        t.check(checksumDone == payload.size() &&
                    (checksumTotal == 0 || checksumTotal == payload.size()),
                "remote checksum should report byte progress");

        std::vector<std::uint8_t> canceledDigest;
        err.clear();
        t.check(!client.checksum(remoteSrc, "SHA-256", canceledDigest, err, {},
                                 [] { return true; }),
                "a remote checksum should honor cancellation");
        t.check(client.lastOperationError().kind ==
                    openscp::RemoteErrorKind::Canceled,
                "checksum cancellation should expose a structured error");
    }
    if (t.failures == 0) {
        err.clear();
        t.check(!client.rename(remoteMissing, remoteProtected, err, true),
                "rename with a missing source should fail");
        bool isDir = true;
        std::string existsErr;
        t.check(client.exists(remoteProtected, isDir, existsErr) && !isDir,
                std::string("failed overwrite rename must preserve its "
                            "destination: ") +
                    existsErr);
    }
    // An upload over an existing destination replaces it.
    if (t.failures == 0) {
        err.clear();
        const bool overwritten =
            client.put(localBad.string(), remoteProtected, err, {}, {}, false);
        t.check(overwritten,
                std::string("upload over an existing destination should "
                            "succeed: ") +
                    err);
        if (overwritten) {
            const fs::path localOverwritten = localTmpRoot / "overwritten.txt";
            std::string downloaded;
            err.clear();
            const bool downloadedOk = client.get(
                remoteProtected, localOverwritten.string(), err, {}, {}, false);
            t.check(downloadedOk && readFile(localOverwritten, downloaded) &&
                        downloaded == badPayload,
                    std::string("an overwritten destination should hold the "
                                "new content: ") +
                        err);
        }
    }
    if (t.failures == 0) {
        err.clear();
        t.check(!client.get(remoteMissing,
                            (localTmpRoot / "missing.txt").string(), err, {},
                            {}, false) &&
                    client.lastOperationError().kind ==
                        openscp::RemoteErrorKind::NotFound,
                std::string("downloading a missing remote file should report "
                            "NotFound: ") +
                    err);
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

        // Changing only the owner, to the one it already has, must not send
        // group 0, which the server would refuse or apply.
        constexpr auto kUnchanged = static_cast<std::uint32_t>(-1);
        openscp::FileInfo owned{};
        err.clear();
        t.check(client.chown(remoteSrc, st.uid, kUnchanged, err) &&
                    client.chown(remoteSrc, kUnchanged, st.gid, err) &&
                    client.stat(remoteSrc, owned, err) && owned.uid == st.uid &&
                    owned.gid == st.gid,
                std::string("chown with an unchanged id should keep it: ") +
                    err);

        const std::uint32_t originalMode =
            (st.mode & 0777u) != 0 ? (st.mode & 0777u) : 0644u;
        openscp::FileInfo changed{};
        err.clear();
        t.check(client.chmod(remoteSrc, 0640, err) &&
                    client.stat(remoteSrc, changed, err) &&
                    (changed.mode & 0777u) == 0640u &&
                    client.chmod(remoteSrc, originalMode, err),
                std::string("chmod should change the remote permissions: ") +
                    err);
    }
#ifndef _WIN32
    // Uploads keep the local file's permissions, so a private file is not
    // left readable by other users of the server.
    for (const fs::perms permissions :
         {fs::perms::owner_read | fs::perms::owner_write,
          fs::perms::owner_read | fs::perms::owner_write |
              fs::perms::group_read}) {
        if (t.failures != 0)
            break;
        const fs::path localPrivate = localTmpRoot / "permissions.txt";
        const std::string remotePrivate =
            joinRemotePath(remoteSuiteDir, "permissions.txt");
        std::error_code permissionsError;
        openscp::FileInfo uploaded{};
        err.clear();
        const bool written = writeFile(localPrivate, "private");
        fs::permissions(localPrivate, permissions, permissionsError);
        t.check(written && !permissionsError &&
                    client.put(localPrivate.string(), remotePrivate, err, {},
                               {}, false) &&
                    client.stat(remotePrivate, uploaded, err) &&
                    (uploaded.mode & 0777u) ==
                        static_cast<std::uint32_t>(permissions),
                std::string("an upload should keep the local permissions: ") +
                    err);
        (void)client.removeFile(remotePrivate, err);
    }
#endif
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
        openscp::FileInfo remoteInfo{};
        t.check(client.stat(remoteSrc, remoteInfo, err) &&
                    remoteInfo.mtime > 0 &&
                    client.lastDownloadModifiedTime() == remoteInfo.mtime,
                "get should report the file's modification time");
        std::string downloaded;
        t.check(readFile(localDst, downloaded),
                "downloaded file should be readable");
        t.check(downloaded == payload,
                "downloaded content should match uploaded payload");
    }
    // A transfer larger than the 2 MiB SFTP buffer spans several pipelined
    // batches and ends with a short request.
    if (t.failures == 0) {
        std::string largePayload(5 * 1024 * 1024 + 12345, '\0');
        std::uint32_t state = 12345;
        for (char &byte : largePayload) {
            state = state * 1103515245u + 12345u;
            byte = static_cast<char>(state >> 24);
        }
        const fs::path localLarge = localTmpRoot / "large.bin";
        const fs::path localLargeDst = localTmpRoot / "large-downloaded.bin";
        err.clear();
        t.check(writeFile(localLarge, largePayload) &&
                    client.put(localLarge.string(), remoteLarge, err, {}, {},
                               false),
                std::string("large upload should succeed: ") + err);
        std::string downloaded;
        err.clear();
        t.check(client.get(remoteLarge, localLargeDst.string(), err, {}, {},
                           false) &&
                    readFile(localLargeDst, downloaded) &&
                    downloaded == largePayload,
                std::string("large download should match the upload: ") + err);
    }
    // Regression: Required integrity must fail on resume mismatch (download).
    if (t.failures == 0) {
        err.clear();
        t.check(client.put(localSrc.string(), remoteResumeDownload, err, {}, {},
                           false),
                std::string("put(remoteResumeDownload) should succeed: ") +
                    err);
    }
    if (t.failures == 0) {
        const fs::path localPart = localResumeDst.string() + ".part";
        t.check(writeFile(localPart, badPayload),
                "local .part for download mismatch should be writable");
        err.clear();
        const bool ok = client.get(remoteResumeDownload,
                                   localResumeDst.string(), err, {}, {}, true);
        t.check(!ok,
                "get resume with required integrity should fail on mismatch");
        t.check(
            err.find("Integrity check failed in resume (download)") !=
                std::string::npos,
            std::string("download mismatch should report integrity error: ") +
                err);
    }
    // Regression: Required integrity must fail on resume mismatch (upload).
    if (t.failures == 0) {
        err.clear();
        t.check(client.put(localBad.string(), remoteResumeUploadSeed, err, {},
                           {}, false),
                std::string("put(remoteResumeUploadSeed) should succeed: ") +
                    err);
        std::vector<std::uint8_t> differentChecksum;
        err.clear();
        t.check(
            client.checksum(remoteResumeUploadSeed, "SHA-256",
                            differentChecksum, err),
            std::string("checksum(different remote file) should succeed: ") +
                err);
        t.check(differentChecksum.size() == 32 &&
                    differentChecksum != sourceChecksum,
                "different remote files should have different SHA-256 "
                "checksums");
    }
    if (t.failures == 0) {
        err.clear();
        t.check(client.rename(remoteResumeUploadSeed, remoteResumeUploadPart,
                              err, false),
                std::string("rename(seed->.part) should succeed: ") + err);
    }
    if (t.failures == 0) {
        err.clear();
        const bool ok = client.put(localSrc.string(), remoteResumeUpload, err,
                                   {}, {}, true);
        t.check(!ok,
                "put resume with required integrity should fail on mismatch");
        t.check(err.find("Integrity check failed in resume (upload)") !=
                    std::string::npos,
                std::string("upload mismatch should report integrity error: ") +
                    err);
    }
    // Optional integrity verifies whole-file transfers from the streamed bytes
    // instead of reading the remote file again.
    const std::string remoteOptional =
        joinRemotePath(remoteSuiteDir, "optional-integrity.txt");
    const std::string remoteOptionalChanged =
        joinRemotePath(remoteSuiteDir, "optional-source-changed.txt");
    const std::string remoteOptionalChanging =
        joinRemotePath(remoteSuiteDir, "optional-remote-changing.txt");
    const auto hourAgo = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count() -
        3600);
    openscp::SessionOptions optionalOpt = opt;
    optionalOpt.transfer_integrity_policy =
        openscp::TransferIntegrityPolicy::Optional;
    openscp::Libssh2SftpClient optionalClient;
    if (t.failures == 0) {
        err.clear();
        t.check(optionalClient.connect(optionalOpt, err),
                std::string("optional integrity connect should succeed: ") +
                    err);
    }
    if (t.failures == 0) {
        err.clear();
        t.check(optionalClient.put(localSrc.string(), remoteOptional, err, {},
                                   {}, false),
                std::string("optional integrity put should succeed: ") + err);
        const fs::path localOptionalDst =
            localTmpRoot / "optional-downloaded.txt";
        // A just-uploaded file is recent, so this download keeps the full
        // check.
        err.clear();
        t.check(optionalClient.get(remoteOptional, localOptionalDst.string(),
                                   err, {}, {}, false),
                std::string("optional recent-file get should succeed: ") + err);
        std::string downloaded;
        t.check(readFile(localOptionalDst, downloaded) && downloaded == payload,
                "optional recent-file download should match the payload");
        // An older file uses the streamed check.
        err.clear();
        t.check(optionalClient.setTimes(remoteOptional, hourAgo, hourAgo, err),
                std::string("optional setTimes should succeed: ") + err);
        err.clear();
        t.check(optionalClient.get(remoteOptional, localOptionalDst.string(),
                                   err, {}, {}, false),
                std::string("optional older-file get should succeed: ") + err);
        downloaded.clear();
        t.check(readFile(localOptionalDst, downloaded) && downloaded == payload,
                "optional older-file download should match the payload");
    }
    if (t.failures == 0) {
        // A remote file that changes during a streamed download fails with a
        // retryable error and discards the mixed .part. The file is changed
        // through the local filesystem, so this case runs only when the test
        // server shares it.
        err.clear();
        t.check(optionalClient.put(localSrc.string(), remoteOptionalChanging,
                                   err, {}, {}, false),
                std::string("optional changing-file put should succeed: ") +
                    err);
        err.clear();
        t.check(
            optionalClient.setTimes(remoteOptionalChanging, hourAgo, hourAgo,
                                    err),
            std::string("optional changing-file setTimes should succeed: ") +
                err);
        const fs::path sharedRemote(remoteOptionalChanging);
        std::error_code sharedError;
        const bool remoteIsShared =
            fs::exists(sharedRemote, sharedError) &&
            std::ofstream(sharedRemote, std::ios::binary | std::ios::app)
                .is_open();
        if (t.failures == 0 && remoteIsShared) {
            const fs::path localChangingDst =
                localTmpRoot / "optional-remote-changing.txt";
            bool appended = false;
            err.clear();
            const bool ok = optionalClient.get(
                remoteOptionalChanging, localChangingDst.string(), err,
                [&](std::size_t, std::size_t) {
                    if (appended)
                        return;
                    std::ofstream out(sharedRemote,
                                      std::ios::binary | std::ios::app);
                    out << "appended";
                    appended = out.good();
                },
                {}, false);
            const openscp::RemoteError changeError =
                optionalClient.lastOperationError();
            t.check(appended, "remote file should change during the download");
            t.check(
                !ok && changeError.kind == openscp::RemoteErrorKind::RemoteIo &&
                    changeError.transient,
                std::string("a remote change during download should be "
                            "retryable: ") +
                    err);
            t.check(!fs::exists(localChangingDst) &&
                        !fs::exists(localChangingDst.string() + ".part"),
                    "a remote change must discard the download and its .part");
        }
    }
    if (t.failures == 0) {
        const fs::path localChanging = localTmpRoot / "optional-changing.txt";
        t.check(writeFile(localChanging, payload),
                "changing upload source should be writable");
        bool changed = false;
        err.clear();
        const bool ok = optionalClient.put(
            localChanging.string(), remoteOptionalChanged, err,
            [&](std::size_t, std::size_t) {
                if (!changed) {
                    changed = writeFile(localChanging,
                                        std::string(payload.size(), 'x'));
                    // Written within one tick of the file system clock,
                    // about 15 ms on Windows, the edit would keep the
                    // original times and so the file's identity. A real
                    // edit comes later, as this one is made to look.
                    std::error_code timeError;
                    const auto written =
                        fs::last_write_time(localChanging, timeError);
                    if (!timeError)
                        fs::last_write_time(localChanging,
                                            written + std::chrono::seconds(2),
                                            timeError);
                    changed = changed && !timeError;
                }
            },
            {}, false);
        t.check(changed, "upload source should change during the transfer");
        t.check(!ok && err.find("changed during transfer") != std::string::npos,
                std::string("optional upload should reject a source changed "
                            "during the transfer: ") +
                    err);
        bool isDir = false;
        std::string existsErr;
        t.check(!optionalClient.exists(remoteOptionalChanged, isDir, existsErr),
                "a rejected optional upload must not publish its destination");
    }
    if (t.failures == 0) {
        // A resumed download depends on earlier data, so it keeps the full
        // end-to-end check and rejects a stale complete .part.
        const fs::path localStaleDst = localTmpRoot / "optional-stale.txt";
        const fs::path localStalePart = localStaleDst.string() + ".part";
        t.check(writeFile(localStalePart, std::string(payload.size(), 'x')),
                "stale .part should be writable");
        err.clear();
        const bool ok = optionalClient.get(
            remoteOptional, localStaleDst.string(), err, {}, {}, true);
        t.check(!ok && err.find("integrity") != std::string::npos,
                std::string("optional resume should reject a stale .part: ") +
                    err);
        t.check(!fs::exists(localStaleDst),
                "a rejected stale download must not publish its destination");
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
        t.check(removeRemoteFileIfExists(client, remoteProtected, err),
                std::string("remove protected destination should succeed: ") +
                    err);
        err.clear();
        t.check(removeRemoteFileIfExists(client, remoteResumeDownload, err),
                std::string("remove remoteResumeDownload should succeed: ") +
                    err);
        err.clear();
        t.check(removeRemoteFileIfExists(client, remoteResumeUpload, err),
                std::string("remove remoteResumeUpload should succeed: ") +
                    err);
        err.clear();
        t.check(removeRemoteFileIfExists(client, remoteResumeUploadSeed, err),
                std::string("remove remoteResumeUploadSeed should succeed: ") +
                    err);
        err.clear();
        t.check(removeRemoteFileIfExists(client, remoteResumeUploadPart, err),
                std::string("remove remoteResumeUploadPart should succeed: ") +
                    err);
        err.clear();
        t.check(removeRemoteFileIfExists(client, remoteOptional, err),
                std::string("remove remoteOptional should succeed: ") + err);
        err.clear();
        t.check(removeRemoteFileIfExists(client, remoteOptionalChanging, err),
                std::string("remove remoteOptionalChanging should succeed: ") +
                    err);
        err.clear();
        t.check(removeRemoteFileIfExists(client,
                                         remoteOptionalChanged + ".part", err),
                std::string("remove remoteOptionalChanged .part should "
                            "succeed: ") +
                    err);
        err.clear();
        t.check(removeRemoteFileIfExists(client, remoteLarge, err),
                std::string("remove remoteLarge should succeed: ") + err);
        err.clear();
        t.check(client.removeDir(remoteSuiteDir, err),
                std::string("removeDir should succeed: ") + err);
    }

    if (t.failures == 0) {
        // Transfer workers connect and disconnect at the same time: one
        // client's teardown must not touch another's socket, whose number it
        // may have just freed. Through a jump host each client has its own
        // ssh process.
        constexpr int kClients = 4;
        constexpr int kRounds = 3;
        std::mutex errorsMutex;
        std::vector<std::string> concurrentErrors;
        std::vector<std::thread> workers;
        for (int index = 0; index < kClients; ++index) {
            workers.emplace_back([&] {
                for (int round = 0; round < kRounds; ++round) {
                    openscp::Libssh2SftpClient concurrent;
                    std::string concurrentErr;
                    std::vector<openscp::FileInfo> entries;
                    const bool listed =
                        concurrent.connect(opt, concurrentErr) &&
                        concurrent.list(remoteBase, entries, concurrentErr);
                    concurrent.disconnect();
                    if (!listed) {
                        std::lock_guard<std::mutex> lock(errorsMutex);
                        concurrentErrors.push_back(concurrentErr);
                    }
                }
            });
        }
        for (std::thread &worker : workers)
            worker.join();
        t.check(concurrentErrors.empty(),
                "clients that connect and disconnect at once should all "
                "succeed: " +
                    (concurrentErrors.empty() ? std::string()
                                              : concurrentErrors.front()));
    }

    // Best-effort cleanup regardless of test result.
    std::string cleanupErr;
    (void)removeRemoteFileIfExists(client, remoteSrc, cleanupErr);
    cleanupErr.clear();
    (void)removeRemoteFileIfExists(client, remoteMoved, cleanupErr);
    cleanupErr.clear();
    (void)removeRemoteFileIfExists(client, remoteProtected, cleanupErr);
    cleanupErr.clear();
    (void)removeRemoteFileIfExists(client, remoteResumeDownload, cleanupErr);
    cleanupErr.clear();
    (void)removeRemoteFileIfExists(client, remoteResumeUpload, cleanupErr);
    cleanupErr.clear();
    (void)removeRemoteFileIfExists(client, remoteResumeUploadSeed, cleanupErr);
    cleanupErr.clear();
    (void)removeRemoteFileIfExists(client, remoteResumeUploadPart, cleanupErr);
    cleanupErr.clear();
    (void)removeRemoteFileIfExists(client, remoteOptional, cleanupErr);
    cleanupErr.clear();
    (void)removeRemoteFileIfExists(client, remoteOptionalChanging, cleanupErr);
    cleanupErr.clear();
    (void)removeRemoteFileIfExists(client, remoteOptionalChanged + ".part",
                                   cleanupErr);
    cleanupErr.clear();
    (void)removeRemoteFileIfExists(client, remoteLarge, cleanupErr);
    cleanupErr.clear();
    (void)client.removeDir(remoteSuiteDir, cleanupErr);
    optionalClient.disconnect();
    client.disconnect();
    fs::remove_all(localTmpRoot, ec);

#ifndef _WIN32
    // Dedicated lifecycle regression: when jump transport is in use, the
    // helper ssh process must be torn down after disconnect (no leaked
    // tunnels).
    if (jumpHost.has_value() && t.failures == 0) {
        const std::size_t baselineJumpCount =
            countJumpTunnelProcesses(*host, port, *jumpHost, jumpPort);
        openscp::Libssh2SftpClient jumpLifecycleClient;
        std::string jumpLifecycleErr;
        const bool jumpLifecycleConnected =
            jumpLifecycleClient.connect(opt, jumpLifecycleErr);
        t.check(jumpLifecycleConnected,
                std::string("jump lifecycle connect should succeed: ") +
                    jumpLifecycleErr);
        if (jumpLifecycleConnected) {
            jumpLifecycleClient.disconnect();
            const bool drained = waitForJumpTunnelCountAtMost(
                *host, port, *jumpHost, jumpPort, baselineJumpCount,
                std::chrono::milliseconds(4000));
            t.check(
                drained,
                "jump lifecycle should stop tunnel process after disconnect");
        }
    }
#endif

    return openscp::testsupport::finishIntegration(
        "openscp_sftp_integration_tests", t);
}
