// Integration tests for real Libssh2ScpClient against an SSH server with SCP.
// The test is skipped (exit code 77) unless required OPENSCP_IT_* env vars
// exist.
#include "IntegrationTestSupport.hpp"
#include "TestHarness.hpp"
#include "libssh2/Libssh2ScpClient.hpp"

#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

using openscp::testsupport::envValue;
using openscp::testsupport::envValueWithFallback;
using openscp::testsupport::joinRemotePath;
using openscp::testsupport::kSkipExitCode;
using openscp::testsupport::parsePort;
using openscp::testsupport::readFile;
using openscp::testsupport::uniqueToken;
using openscp::testsupport::writeFile;

} // namespace

int main() {
    const auto host =
        envValueWithFallback("OPENSCP_IT_SCP_HOST", "OPENSCP_IT_SFTP_HOST");
    const auto user =
        envValueWithFallback("OPENSCP_IT_SCP_USER", "OPENSCP_IT_SFTP_USER");
    const auto pass =
        envValueWithFallback("OPENSCP_IT_SCP_PASS", "OPENSCP_IT_SFTP_PASS");
    const auto keyPath =
        envValueWithFallback("OPENSCP_IT_SCP_KEY", "OPENSCP_IT_SFTP_KEY");
    const auto keyPassphrase = envValueWithFallback(
        "OPENSCP_IT_SCP_KEY_PASSPHRASE", "OPENSCP_IT_SFTP_KEY_PASSPHRASE");
    const std::string remoteBase =
        envValue("OPENSCP_IT_SCP_REMOTE_BASE")
            .value_or(envValue("OPENSCP_IT_REMOTE_BASE").value_or("/tmp"));

    if (!host.has_value() || !user.has_value() ||
        (!pass.has_value() && !keyPath.has_value())) {
        std::cout << "[SKIP] openscp_scp_integration_tests requires env vars: "
                  << "OPENSCP_IT_SCP_HOST, OPENSCP_IT_SCP_USER and one auth "
                     "method "
                  << "(OPENSCP_IT_SCP_PASS or OPENSCP_IT_SCP_KEY). It also "
                     "accepts OPENSCP_IT_SFTP_* fallbacks.\n";
        return kSkipExitCode;
    }
    if (keyPath.has_value() && !fs::exists(*keyPath)) {
        std::cerr << "[FAIL] SCP private key does not exist: " << *keyPath
                  << "\n";
        return EXIT_FAILURE;
    }

    std::uint16_t port = 22;
    if (!parsePort(
            envValueWithFallback("OPENSCP_IT_SCP_PORT", "OPENSCP_IT_SFTP_PORT"),
            port, 22)) {
        std::cerr << "[FAIL] SCP port is invalid\n";
        return EXIT_FAILURE;
    }

    openscp::SessionOptions opt;
    opt.protocol = openscp::Protocol::Scp;
    opt.host = *host;
    opt.port = port;
    opt.username = *user;
    opt.known_hosts_policy = openscp::KnownHostsPolicy::Off;
    if (pass.has_value())
        opt.password = pass;
    if (keyPath.has_value())
        opt.private_key_path = keyPath;
    if (keyPassphrase.has_value())
        opt.private_key_passphrase = keyPassphrase;

    openscp::Libssh2ScpClient client;
    std::string err;
    if (!client.connect(opt, err)) {
        std::cerr << "[FAIL] SCP connect failed: " << err << "\n";
        return EXIT_FAILURE;
    }

    TestContext t;
    t.check(client.protocol() == openscp::Protocol::Scp,
            "client should report SCP protocol");
    const auto caps = client.capabilities();
    t.check(caps.implemented, "SCP should be marked implemented");
    t.check(caps.can_upload && caps.can_download,
            "SCP should support file transfers");
    t.check(!caps.can_list, "SCP should not support listing");
    t.check(!caps.can_resume_download && !caps.can_resume_upload,
            "SCP should not support resume");

    const std::string token = uniqueToken();
    const fs::path tempDir =
        fs::temp_directory_path() / ("openscp_scp_" + token);
    const fs::path localUpload = tempDir / "upload.txt";
    const fs::path localDownload = tempDir / "download.txt";
    fs::create_directories(tempDir);

    const std::string payload =
        "openscp scp integration payload " + token + "\nline two\n";
    t.check(writeFile(localUpload, payload), "should write local upload file");

    const std::string remotePath =
        joinRemotePath(remoteBase, "openscp_scp_it_" + token + ".txt");

    bool uploadProgressCalled = false;
    err.clear();
    t.check(client.put(
                localUpload.string(), remotePath, err,
                [&](std::size_t, std::size_t) { uploadProgressCalled = true; },
                {}, false),
            std::string("SCP upload should succeed: ") + err);
    t.check(uploadProgressCalled, "upload progress callback should be called");

    // Waiting for the channel to close instead of acknowledging SCP's final
    // status byte stalls each download until the session timeout.
    constexpr auto kTimeoutStall = std::chrono::seconds(10);
    bool downloadProgressCalled = false;
    err.clear();
    auto started = std::chrono::steady_clock::now();
    t.check(
        client.get(
            remotePath, localDownload.string(), err,
            [&](std::size_t, std::size_t) { downloadProgressCalled = true; },
            {}, false),
        std::string("SCP download should succeed: ") + err);
    t.check(std::chrono::steady_clock::now() - started < kTimeoutStall,
            "SCP download should not wait for the session timeout");
    t.check(downloadProgressCalled,
            "download progress callback should be called");

    std::string downloaded;
    t.check(readFile(localDownload, downloaded),
            "downloaded file should be readable");
    t.check(downloaded == payload, "downloaded content should match uploaded");

    // SCP-only mode has no SFTP fallback to hide a broken download.
    openscp::SessionOptions scpOnlyOptions = opt;
    scpOnlyOptions.scp_transfer_mode = openscp::ScpTransferMode::ScpOnly;
    openscp::Libssh2ScpClient scpOnlyClient;
    err.clear();
    t.check(scpOnlyClient.connect(scpOnlyOptions, err),
            std::string("SCP-only connect should succeed: ") + err);
    const fs::path scpOnlyDownload = tempDir / "scp_only_download.txt";
    for (int attempt = 0; attempt < 2; ++attempt) {
        err.clear();
        downloaded.clear();
        started = std::chrono::steady_clock::now();
        t.check(scpOnlyClient.get(remotePath, scpOnlyDownload.string(), err, {},
                                  {}, false),
                std::string("SCP-only download should succeed on a reused "
                            "session: ") +
                    err);
        t.check(std::chrono::steady_clock::now() - started < kTimeoutStall,
                "SCP-only download should not wait for the session timeout");
        t.check(readFile(scpOnlyDownload, downloaded) && downloaded == payload,
                "SCP-only download should match the upload");
        t.check(!fs::exists(scpOnlyDownload.string() + ".part"),
                "SCP-only download should not leave a partial file");
    }

    // The upload waits for the remote scp to confirm the write, which is
    // where it reports a full disk.
    const std::string scpOnlyRemotePath =
        joinRemotePath(remoteBase, "openscp_scp_only_it_" + token + ".txt");
    err.clear();
    t.check(scpOnlyClient.put(localUpload.string(), scpOnlyRemotePath, err, {},
                              {}, false),
            std::string("SCP-only upload should succeed: ") + err);
    err.clear();
    downloaded.clear();
    t.check(scpOnlyClient.get(scpOnlyRemotePath, scpOnlyDownload.string(), err,
                              {}, {}, false) &&
                readFile(scpOnlyDownload, downloaded) && downloaded == payload,
            std::string("SCP-only upload should round-trip: ") + err);
#ifndef _WIN32
    // SCP creates the file with the local permissions too; SFTP reads them.
    {
        const fs::path localPrivate = tempDir / "private.txt";
        const std::string remotePrivate =
            joinRemotePath(remoteBase, "openscp_scp_private_" + token + ".txt");
        std::error_code permissionsError;
        const bool written = writeFile(localPrivate, "private");
        fs::permissions(localPrivate,
                        fs::perms::owner_read | fs::perms::owner_write,
                        permissionsError);
        openscp::Libssh2SftpClient inspector;
        openscp::SessionOptions inspectorOptions = opt;
        inspectorOptions.protocol = openscp::Protocol::Sftp;
        openscp::FileInfo uploaded{};
        err.clear();
        t.check(written && !permissionsError &&
                    scpOnlyClient.put(localPrivate.string(), remotePrivate, err,
                                      {}, {}, false) &&
                    inspector.connect(inspectorOptions, err) &&
                    inspector.stat(remotePrivate, uploaded, err) &&
                    (uploaded.mode & 0777u) == 0600u,
                std::string("an SCP upload should keep the local "
                            "permissions: ") +
                    err);
        (void)inspector.removeFile(remotePrivate, err);
        inspector.disconnect();
    }
#endif
    scpOnlyClient.disconnect();

    const std::string preservedDestination = "preserve existing destination";
    t.check(writeFile(localDownload, preservedDestination),
            "should prepare an existing SCP download destination");
    err.clear();
    t.check(!client.get(
                remotePath, localDownload.string(), err, {},
                [] { return true; }, false),
            "canceled SCP download should not report success");
    downloaded.clear();
    t.check(readFile(localDownload, downloaded) &&
                downloaded == preservedDestination,
            "canceled SCP download must preserve the existing destination");

    std::vector<openscp::FileInfo> listing;
    err.clear();
    t.check(!client.list(remoteBase, listing, err),
            "SCP listing should report unsupported");
    t.check(!err.empty(), "SCP listing unsupported should set error message");

    err.clear();
    t.check(!client.put(localUpload.string(), remotePath, err, {}, {}, true),
            "SCP upload resume should be rejected");
    t.check(err.find("resume") != std::string::npos,
            "SCP upload resume error should mention resume");

    err.clear();
    t.check(!client.get(remotePath, localDownload.string(), err, {}, {}, true),
            "SCP download resume should be rejected");
    t.check(err.find("resume") != std::string::npos,
            "SCP download resume error should mention resume");

    // Auto mode uploads through SFTP on the session SCP opened, so
    // interrupting the client stops the upload; a separate fallback
    // connection used to carry on until the file was complete.
    {
        const fs::path largeUpload = tempDir / "large.bin";
        openscp::Libssh2ScpClient interrupted;
        std::string interruptErr;
        const bool ready =
            writeFile(largeUpload, std::string(16 * 1024 * 1024, 'x')) &&
            interrupted.connect(opt, interruptErr);
        t.check(ready, "the interrupted upload should start: " + interruptErr);
        bool interruptRequested = false;
        const bool uploaded =
            ready &&
            interrupted.put(
                largeUpload.string(),
                joinRemotePath(remoteBase,
                               "openscp_scp_it_interrupted_" + token + ".bin"),
                interruptErr,
                [&](std::size_t done, std::size_t) {
                    if (done > 0 && !interruptRequested) {
                        interruptRequested = true;
                        interrupted.interrupt();
                    }
                },
                {}, false);
        t.check(ready && interruptRequested && !uploaded,
                "interrupting an Auto-mode upload should stop it");
        interrupted.disconnect();
    }

    client.disconnect();
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    return openscp::testsupport::finishIntegration(
        "openscp_scp_integration_tests", t);
}
