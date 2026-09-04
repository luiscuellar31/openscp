// Integration tests for CurlWebDavClient against a real WebDAV server.
// Skips with exit code 77 unless required OPENSCP_IT_WEBDAV_* vars exist.
#include "IntegrationTestSupport.hpp"
#include "TestHarness.hpp"
#include "openscp/ClientFactory.hpp"

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace fs = std::filesystem;

namespace {

constexpr int kSkipExitCode = 77;

using openscp::testsupport::envValue;
using openscp::testsupport::joinRemotePath;
using openscp::testsupport::parseBool;
using openscp::testsupport::parsePort;
using openscp::testsupport::readFile;
using openscp::testsupport::uniqueToken;
using openscp::testsupport::writeFile;

openscp::WebDavScheme parseWebDavScheme(const std::optional<std::string> &raw,
                                        std::uint16_t port) {
    if (!raw.has_value()) {
        return (port == openscp::defaultPortForWebDavScheme(
                            openscp::WebDavScheme::Http))
                   ? openscp::WebDavScheme::Http
                   : openscp::WebDavScheme::Https;
    }
    return openscp::webDavSchemeFromStorageName(*raw);
}

} // namespace

int main() {
    const auto host = envValue("OPENSCP_IT_WEBDAV_HOST");
    const auto user = envValue("OPENSCP_IT_WEBDAV_USER");
    const auto pass = envValue("OPENSCP_IT_WEBDAV_PASS");
    const auto remoteBase = envValue("OPENSCP_IT_WEBDAV_REMOTE_BASE");
    const auto schemeRaw = envValue("OPENSCP_IT_WEBDAV_SCHEME");
    const auto basePath = envValue("OPENSCP_IT_WEBDAV_BASE_PATH");
    const auto caCert = envValue("OPENSCP_IT_WEBDAV_CA_CERT");
    const bool verifyPeer =
        parseBool(envValue("OPENSCP_IT_WEBDAV_VERIFY_PEER"), true);

    if (!host.has_value() || !remoteBase.has_value()) {
        std::cout
            << "[SKIP] openscp_webdav_integration_tests requires "
            << "OPENSCP_IT_WEBDAV_HOST and OPENSCP_IT_WEBDAV_REMOTE_BASE\n";
        return kSkipExitCode;
    }

    std::uint16_t port = 443;
    if (!parsePort(envValue("OPENSCP_IT_WEBDAV_PORT"), port, 443)) {
        std::cerr << "[FAIL] OPENSCP_IT_WEBDAV_PORT is invalid\n";
        return EXIT_FAILURE;
    }

    auto client = openscp::CreateClientForProtocol(openscp::Protocol::WebDav);
    if (!client) {
        std::cerr << "[FAIL] factory did not create WebDAV backend\n";
        return EXIT_FAILURE;
    }

    openscp::SessionOptions opt;
    opt.protocol = openscp::Protocol::WebDav;
    opt.host = *host;
    opt.port = port;
    opt.webdav_scheme = parseWebDavScheme(schemeRaw, port);
    opt.webdav_base_path = basePath.value_or("/");
    opt.username = user.value_or("");
    if (pass.has_value())
        opt.password = pass;
    opt.webdav_verify_peer = (opt.webdav_scheme == openscp::WebDavScheme::Https)
                                 ? verifyPeer
                                 : false;
    if (caCert.has_value())
        opt.webdav_ca_cert_path = *caCert;

    openscp::testsupport::ScopedEnvironment proxyEnvironment;
    openscp::testsupport::forceUnreachableEnvironmentProxies(proxyEnvironment);

    std::string err;
    if (!client->connect(opt, err)) {
        std::cerr << "[FAIL] WebDAV connect failed: " << err << "\n";
        return EXIT_FAILURE;
    }

    TestContext t;
    t.check(client->protocol() == openscp::Protocol::WebDav,
            "WebDAV client should report WebDAV protocol");
    const auto caps = client->capabilities();
    t.check(caps.implemented, "WebDAV should be marked implemented");
    t.check(caps.can_upload && caps.can_download,
            "WebDAV should support transfers");
    t.check(caps.can_list, "WebDAV should support remote listing");
    t.check(caps.can_stat && caps.can_mkdir && caps.can_delete &&
                caps.can_rename,
            "WebDAV should advertise remote CRUD operations");

    const std::string token = uniqueToken();
    const fs::path tempDir =
        fs::temp_directory_path() / ("openscp_webdav_" + token);
    const fs::path localUpload = tempDir / "upload.txt";
    const fs::path localDownload = tempDir / "download.txt";
    const fs::path canceledDownload = tempDir / "canceled.txt";
    const fs::path boundaryCanceledDownload = tempDir / "boundary-canceled.txt";
    fs::create_directories(tempDir);

    const std::string payload =
        "openscp webdav integration payload " + token + "\nline two\n";
    t.check(writeFile(localUpload, payload), "should write local upload file");

    const std::string remoteDir =
        joinRemotePath(*remoteBase, "openscp_webdav_it_" + token);
    const std::string remoteFileName = "upload file.txt";
    const std::string renamedFileName = "renamed file.txt";
    const std::string remotePath = joinRemotePath(remoteDir, remoteFileName);
    const std::string renamedPath = joinRemotePath(remoteDir, renamedFileName);
    const std::string canceledUploadPath =
        joinRemotePath(remoteDir, "canceled upload.txt");

    err.clear();
    t.check(client->mkdir(remoteDir, err),
            std::string("WebDAV mkdir should succeed: ") + err);
    err.clear();
    t.check(client->mkdir(remoteDir, err),
            std::string("WebDAV mkdir should verify an existing collection: ") +
                err);

    bool uploadProgressCalled = false;
    err.clear();
    t.check(client->put(
                localUpload.string(), remotePath, err,
                [&](std::size_t, std::size_t) { uploadProgressCalled = true; },
                {}, false),
            std::string("WebDAV upload should succeed: ") + err);
    t.check(uploadProgressCalled, "upload progress callback should be called");

    err.clear();
    t.check(!client->mkdir(remotePath, err),
            "WebDAV MKCOL must not treat 405 on an existing file as success");
    t.check(client->lastOperationError().kind ==
                openscp::RemoteErrorKind::Conflict,
            "WebDAV MKCOL on a file should report a conflict");

    err.clear();
    t.check(client->rename(remotePath, renamedPath, err, false),
            std::string("WebDAV rename should succeed: ") + err);

    bool downloadProgressCalled = false;
    err.clear();
    t.check(
        client->get(
            renamedPath, localDownload.string(), err,
            [&](std::size_t, std::size_t) { downloadProgressCalled = true; },
            {}, false),
        std::string("WebDAV download should succeed: ") + err);
    t.check(downloadProgressCalled,
            "download progress callback should be called");

    std::string downloaded;
    t.check(readFile(localDownload, downloaded),
            "downloaded file should be readable");
    t.check(downloaded == payload, "downloaded content should match uploaded");

    t.check(writeFile(canceledDownload, "keep existing destination"),
            "should prepare existing WebDAV destination");
    err.clear();
    t.check(!client->get(
                renamedPath, canceledDownload.string(), err, {},
                [] { return true; }, false),
            "canceled WebDAV download should fail");
    std::string preserved;
    t.check(readFile(canceledDownload, preserved) &&
                preserved == "keep existing destination",
            "canceled WebDAV download must preserve existing destination");
    t.check(fs::exists(canceledDownload.string() + ".part"),
            "canceled WebDAV download should retain its partial file");

    t.check(writeFile(boundaryCanceledDownload, "keep boundary destination"),
            "should prepare a boundary-cancel WebDAV destination");
    std::atomic<bool> cancelFinishedDownload{false};
    err.clear();
    t.check(!client->get(
                renamedPath, boundaryCanceledDownload.string(), err,
                [&](std::size_t done, std::size_t) {
                    if (done >= payload.size())
                        cancelFinishedDownload.store(true);
                },
                [&] { return cancelFinishedDownload.load(); }, false),
            "WebDAV cancellation at completion should prevent final replace");
    preserved.clear();
    t.check(readFile(boundaryCanceledDownload, preserved) &&
                preserved == "keep boundary destination",
            "late WebDAV cancellation must preserve the existing destination");
    t.check(fs::exists(boundaryCanceledDownload.string() + ".part"),
            "late-canceled WebDAV download should retain its partial file");

    std::atomic<bool> cancelFinishedUpload{false};
    err.clear();
    t.check(!client->put(
                localUpload.string(), canceledUploadPath, err,
                [&](std::size_t done, std::size_t) {
                    if (done >= payload.size())
                        cancelFinishedUpload.store(true);
                },
                [&] { return cancelFinishedUpload.load(); }, false),
            "WebDAV cancellation at upload completion should prevent MOVE");
    bool canceledUploadIsDir = false;
    err.clear();
    const bool canceledUploadExists =
        client->exists(canceledUploadPath, canceledUploadIsDir, err);
    t.check(
        !canceledUploadExists && err.empty(),
        "late-canceled WebDAV upload must not publish the final destination");
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
            "late-canceled WebDAV upload should retain its remote partial");
    if (canceledPartialExists) {
        err.clear();
        t.check(client->removeFile(canceledUploadPartial, err),
                std::string("WebDAV canceled upload partial cleanup failed: ") +
                    err);
    }

    std::vector<openscp::FileInfo> listing;
    err.clear();
    t.check(client->list(remoteDir, listing, err),
            std::string("WebDAV listing should succeed: ") + err);
    const auto listed = std::find_if(
        listing.begin(), listing.end(),
        [&](const openscp::FileInfo &f) { return f.name == renamedFileName; });
    t.check(listed != listing.end(),
            "WebDAV listing should include the uploaded file");

    openscp::FileInfo statInfo{};
    err.clear();
    t.check(client->stat(renamedPath, statInfo, err),
            std::string("WebDAV stat should succeed: ") + err);
    t.check(!statInfo.is_dir, "stat on uploaded file should report file");

    err.clear();
    t.check(client->removeFile(renamedPath, err),
            std::string("WebDAV remove file should succeed: ") + err);

    bool existsIsDir = false;
    err.clear();
    const bool exists = client->exists(renamedPath, existsIsDir, err);
    t.check(!exists, "removed file should not exist anymore");
    err.clear();
    t.check(client->removeDir(remoteDir, err),
            std::string("WebDAV remove directory should succeed: ") + err);

    client->disconnect();
    std::error_code ec;
    fs::remove_all(tempDir, ec);
    if (t.failures != 0) {
        std::cerr << "[FAIL] openscp_webdav_integration_tests failures="
                  << t.failures << "\n";
        return EXIT_FAILURE;
    }
    std::cout << "[OK] openscp_webdav_integration_tests\n";
    return EXIT_SUCCESS;
}
