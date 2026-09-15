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
using openscp::testsupport::kSkipExitCode;
using openscp::testsupport::parseBool;
using openscp::testsupport::parsePort;

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
    openscp::testsupport::ManagedFilesContractHooks hooks;
    hooks.afterDirectoryCreated = [](openscp::RemoteClient &connectedClient,
                                     const std::string &remoteDirectory,
                                     TestContext &test) {
        std::string error;
        const bool created = connectedClient.mkdir(remoteDirectory, error);
        test.check(created,
                   "WebDAV: mkdir should verify an existing collection: " +
                       error);
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
