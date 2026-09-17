// Integration tests for CurlFtpClient against a real FTP server.
// Skips with exit code 77 unless required OPENSCP_IT_FTP_* vars exist.
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
using openscp::testsupport::parsePort;

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
    err.clear();
    t.check(!client->removeFile("/safe\r\nDELE /important", err),
            "FTP: connected operations must reject command-injection paths");
    openscp::testsupport::ManagedFilesContractHooks hooks;
    hooks.afterUpload = openscp::testsupport::checkFtpEntryLookups;
    openscp::testsupport::runManagedFilesContract(
        *client, openscp::Protocol::Ftp, "FTP", *remoteBase, t, hooks);
    return openscp::testsupport::finishIntegration(
        "openscp_ftp_integration_tests", t);
}
