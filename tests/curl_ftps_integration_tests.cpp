// Integration tests for CurlFtpClient against a real FTPS server.
// Skips with exit code 77 unless required OPENSCP_IT_FTPS_* vars exist.
#include "IntegrationTestSupport.hpp"
#include "TestHarness.hpp"
#include "openscp/ClientFactory.hpp"

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <string>

namespace {

using openscp::testsupport::envValue;
using openscp::testsupport::kSkipExitCode;
using openscp::testsupport::parseBool;
using openscp::testsupport::parsePort;

} // namespace

int main() {
    const auto host = envValue("OPENSCP_IT_FTPS_HOST");
    const auto user = envValue("OPENSCP_IT_FTPS_USER");
    const auto pass = envValue("OPENSCP_IT_FTPS_PASS");
    const auto remoteBase = envValue("OPENSCP_IT_FTPS_REMOTE_BASE");
    const auto caCert = envValue("OPENSCP_IT_FTPS_CA_CERT");
    const auto modeRaw = envValue("OPENSCP_IT_FTPS_MODE");
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
    opt.ftps_mode = modeRaw ? openscp::ftpsModeFromStorageName(*modeRaw)
                            : openscp::FtpsMode::Auto;
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
    openscp::testsupport::runManagedFilesContract(
        *client, openscp::Protocol::Ftps, "FTPS", *remoteBase, t);
    return openscp::testsupport::finishIntegration(
        "openscp_ftps_integration_tests", t);
}
