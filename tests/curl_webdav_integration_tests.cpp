// Integration tests for CurlWebDavClient against a real WebDAV server.
// Skips with exit code 77 unless required OPENSCP_IT_WEBDAV_* vars exist.
#include "IntegrationTestSupport.hpp"
#include "TestHarness.hpp"
#include "openscp/ClientFactory.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

namespace {

using openscp::testsupport::envValue;
using openscp::testsupport::IntegrationTempDir;
using openscp::testsupport::joinRemotePath;
using openscp::testsupport::kSkipExitCode;
using openscp::testsupport::parseBool;
using openscp::testsupport::parsePort;
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

// DELETE and MOVE with Overwrite: T act on a whole collection, so removing,
// uploading over or renaming onto a folder must fail and keep its contents.
void checkCollectionsSurviveMutations(openscp::RemoteClient &client,
                                      const std::string &remoteDirectory,
                                      TestContext &test) {
    IntegrationTempDir localDirectory;
    const auto localPayload = localDirectory.path() / "payload.txt";
    test.check(localDirectory.isValid() && writeFile(localPayload, "payload"),
               "WebDAV: should write the collection test payload");
    const std::string folder = joinRemotePath(remoteDirectory, "protected");
    const std::string child = joinRemotePath(folder, "child.txt");
    const std::string sibling = joinRemotePath(remoteDirectory, "sibling.txt");

    std::string error;
    test.check(
        client.mkdir(folder, error) &&
            client.put(localPayload.string(), child, error, {}, {}, false) &&
            client.put(localPayload.string(), sibling, error, {}, {}, false),
        "WebDAV: should create a folder with a child: " + error);
    const auto failsWith = [&client](bool succeeded,
                                     openscp::RemoteErrorKind kind) {
        return !succeeded && client.lastOperationError().kind == kind;
    };

    error.clear();
    test.check(failsWith(client.removeDir(folder, error),
                         openscp::RemoteErrorKind::Conflict),
               "WebDAV: removeDir should refuse a collection with children");
    error.clear();
    test.check(failsWith(client.removeFile(folder, error),
                         openscp::RemoteErrorKind::InvalidRequest),
               "WebDAV: removeFile should refuse a collection");
    error.clear();
    test.check(failsWith(client.put(localPayload.string(), folder, error, {},
                                    {}, false),
                         openscp::RemoteErrorKind::Conflict),
               "WebDAV: an upload should not replace a collection");
    error.clear();
    (void)client.removeFile(folder + ".part", error);
    error.clear();
    test.check(failsWith(client.rename(sibling, folder, error, true),
                         openscp::RemoteErrorKind::Conflict),
               "WebDAV: an overwriting rename should not replace a "
               "collection");
    bool childIsDirectory = true;
    error.clear();
    test.check(client.exists(child, childIsDirectory, error) &&
                   !childIsDirectory,
               "WebDAV: the refused requests should keep the folder's "
               "contents");

    error.clear();
    test.check(client.removeFile(child, error) &&
                   client.removeDir(folder, error) &&
                   client.removeFile(sibling, error),
               "WebDAV: an emptied collection should be removable: " + error);
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
    openscp::testsupport::ManagedFilesContractHooks hooks;
    hooks.afterDirectoryCreated = [](openscp::RemoteClient &connectedClient,
                                     const std::string &remoteDirectory,
                                     TestContext &test) {
        std::string error;
        const bool created = connectedClient.mkdir(remoteDirectory, error);
        test.check(created,
                   "WebDAV: mkdir should verify an existing collection: " +
                       error);
        checkCollectionsSurviveMutations(connectedClient, remoteDirectory,
                                         test);
    };
    hooks.afterUpload = [](openscp::RemoteClient &connectedClient,
                           const std::string &remotePath, TestContext &test) {
        std::string error;
        test.check(!connectedClient.mkdir(remotePath, error),
                   "WebDAV: MKCOL must not treat an existing file as a "
                   "collection");
        test.check(connectedClient.lastOperationError().kind ==
                       openscp::RemoteErrorKind::Conflict,
                   "WebDAV: MKCOL on a file should report Conflict");
    };
    openscp::testsupport::runManagedFilesContract(
        *client, openscp::Protocol::WebDav, "WebDAV", *remoteBase, t, hooks);
    return openscp::testsupport::finishIntegration(
        "openscp_webdav_integration_tests", t);
}
