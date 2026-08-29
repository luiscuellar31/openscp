// Focused unit tests for security-sensitive libcurl backend helpers.
#include "CurlBackendCommon.hpp"
#include "TestHarness.hpp"
#include "openscp/RemotePath.hpp"
#if OPENSCP_HAS_CURL_FTP
#include "openscp/CurlFtpClient.hpp"
#endif
#if OPENSCP_HAS_CURL_WEBDAV
#include "openscp/CurlWebDavClient.hpp"
#endif

#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>

namespace {

void testRetryAfter(TestContext &test) {
    using openscp::curlcommon::parseRetryAfter;

    test.check(parseRetryAfter("15", 0) == std::optional<std::uint32_t>(15),
               "Retry-After should parse delta seconds");
    test.check(parseRetryAfter(" 120 ", 0) == std::optional<std::uint32_t>(60),
               "Retry-After delta seconds should be capped at 60");

    constexpr std::time_t beforeDate = 1445412450;
    test.check(parseRetryAfter("Wed, 21 Oct 2015 07:28:00 GMT", beforeDate) ==
                   std::optional<std::uint32_t>(30),
               "Retry-After should parse an HTTP date relative to now");
    test.check(
        parseRetryAfter("Wed, 21 Oct 2015 07:28:00 GMT", beforeDate - 120) ==
            std::optional<std::uint32_t>(60),
        "HTTP-date Retry-After should also be capped at 60");
    test.check(
        parseRetryAfter("Wed, 21 Oct 2015 07:28:00 GMT", beforeDate + 60) ==
            std::optional<std::uint32_t>(0),
        "past Retry-After dates should request no additional wait");
    test.check(!parseRetryAfter("not a retry date", beforeDate).has_value(),
               "invalid Retry-After values should be ignored");
}

void testHostValidation(TestContext &test) {
    using openscp::curlcommon::validateUrlHost;

    std::string err;
    test.check(validateUrlHost("files.example.test", "Host", err),
               "DNS hosts should be accepted");
    err.clear();
    test.check(validateUrlHost("[2001:db8::1]", "Host", err),
               "bracketed IPv6 hosts should be accepted");
    err.clear();
    test.check(
        validateUrlHost("2001:db8::1", "Host", err),
        "unbracketed IPv6 hosts should be accepted and normalized later");

    err.clear();
    test.check(!validateUrlHost("trusted.example@127.0.0.1", "Host", err),
               "userinfo delimiters must be rejected in hosts");
    err.clear();
    test.check(!validateUrlHost("example.test/path", "Host", err),
               "path delimiters must be rejected in hosts");
    err.clear();
    test.check(!validateUrlHost("example.test:2121", "Host", err),
               "ports must use the separate port field");
    err.clear();
    test.check(
        !validateUrlHost("example.test\r\nX-Test: injected", "Host", err),
        "control characters must be rejected in hosts");
}

void testClientsRejectAuthorityInjection(TestContext &test) {
#if OPENSCP_HAS_CURL_FTP
    {
        openscp::CurlFtpClient client(openscp::Protocol::Ftp);
        openscp::SessionOptions options;
        options.protocol = openscp::Protocol::Ftp;
        options.host = "trusted.example@127.0.0.1";
        options.port = 21;
        std::string err;
        test.check(!client.connect(options, err),
                   "FTP should reject injected URL authority before I/O");
        test.check(client.lastOperationError().kind ==
                       openscp::RemoteErrorKind::InvalidRequest,
                   "FTP authority rejection should be structured");
    }
#endif
#if OPENSCP_HAS_CURL_WEBDAV
    {
        openscp::CurlWebDavClient client;
        openscp::SessionOptions options;
        options.protocol = openscp::Protocol::WebDav;
        options.host = "trusted.example@127.0.0.1";
        options.port = 443;
        std::string err;
        test.check(!client.connect(options, err),
                   "WebDAV should reject injected URL authority before I/O");
        test.check(client.lastOperationError().kind ==
                       openscp::RemoteErrorKind::InvalidRequest,
                   "WebDAV authority rejection should be structured");
    }
#endif
}

void testFtpCommandRoot(TestContext &test) {
    using openscp::curlcommon::ftpCommandPath;

    test.check(ftpCommandPath("/", "/workspace/file.txt") ==
                   "/workspace/file.txt",
               "FTP commands should preserve paths for a root login");
    test.check(
        ftpCommandPath("/srv/ftp/alice", "/workspace/file.txt") ==
            "/srv/ftp/alice/workspace/file.txt",
        "FTP commands should resolve logical paths below the login root");
    test.check(ftpCommandPath("/srv/ftp/alice/", "/") == "/srv/ftp/alice",
               "FTP logical root should resolve to the PWD login directory");
    test.check(ftpCommandPath("srv\\ftp\\alice", "../release/./file") ==
                   "/srv/ftp/alice/release/file",
               "FTP PWD roots and relative paths should share canonical "
               "normalization");
}

void testRemotePathNormalization(TestContext &test) {
    using openscp::normalizeRemotePath;

    test.check(normalizeRemotePath("") == "/" &&
                   normalizeRemotePath("relative/path") == "/relative/path",
               "remote paths should always be absolute");
    test.check(normalizeRemotePath("//team\\alpha/./tmp/../release") ==
                   "/team/alpha/release",
               "remote paths should normalize separators and dot segments");
    test.check(normalizeRemotePath("../../../../safe") == "/safe",
               "remote paths must not escape above their logical root");
}

void testWebDavPaths(TestContext &test) {
    using openscp::curlcommon::webDavLogicalPath;
    using openscp::curlcommon::webDavServerPath;

    test.check(webDavServerPath("remote.php/dav/./files/alice/",
                                "projects/../release") ==
                   "/remote.php/dav/files/alice/release",
               "WebDAV bases and relative paths should compose canonically");
    test.check(webDavServerPath("/", "/release") == "/release",
               "a root WebDAV base should preserve logical paths");

    std::string logical;
    test.check(webDavLogicalPath("/remote.php/dav/files/alice",
                                 "/remote.php/dav/files/alice/team", logical) &&
                   logical == "/team",
               "WebDAV server paths should map back below their base");
    test.check(!webDavLogicalPath("/remote.php/dav/files/alice",
                                  "/remote.php/dav/files/bob", logical),
               "WebDAV paths outside the configured base should be rejected");
}

void testWebDavCompletionStatuses(TestContext &test) {
    using openscp::curlcommon::isCompletedWebDavGetStatus;
    using openscp::curlcommon::isCompletedWebDavWriteStatus;

    test.check(isCompletedWebDavGetStatus(200),
               "WebDAV GET 200 should be complete");
    test.check(!isCompletedWebDavGetStatus(202),
               "WebDAV GET 202 must not publish a local destination");
    test.check(isCompletedWebDavWriteStatus(200) &&
                   isCompletedWebDavWriteStatus(201) &&
                   isCompletedWebDavWriteStatus(204),
               "completed WebDAV writes should accept 200, 201 and 204");
    test.check(!isCompletedWebDavWriteStatus(202) &&
                   !isCompletedWebDavWriteStatus(206),
               "asynchronous or partial WebDAV writes must not be committed");
}

void testCurlClientState(TestContext &test) {
    openscp::SessionOptions defaults;
    defaults.protocol = openscp::Protocol::Ftp;
    openscp::curlcommon::CurlClientState state(defaults);

    std::shared_ptr<const openscp::SessionOptions> connectedOptions;
    {
        auto operation = state.beginOperation();
        test.check(!operation.disconnecting() &&
                       !operation.interrupted()->load(),
                   "new curl operations should start ready to run");
        state.prepareForConnect();

        auto session = std::make_unique<openscp::curlcommon::CurlEasySession>();
        std::string error;
        test.check(session->initialize(error),
                   std::string("curl session should initialize: ") + error);
        auto mutableOptions =
            std::make_shared<openscp::SessionOptions>(defaults);
        mutableOptions->host = "ftp.example.test";
        connectedOptions = mutableOptions;
        state.commitConnection(connectedOptions, std::move(session),
                               "srv\\ftp\\alice/./");

        const auto snapshot = state.snapshot(operation);
        test.check(snapshot && snapshot.options.get() == connectedOptions.get(),
                   "curl snapshots should share immutable session options");
        test.check(snapshot.commandRoot == "/srv/ftp/alice",
                   "curl state should canonicalize the FTP PWD root");

        state.requestInterrupt();
        test.check(operation.interrupted()->load(),
                   "curl state should publish interruption requests");
    }

    {
        auto nextOperation = state.beginOperation();
        test.check(!nextOperation.interrupted()->load(),
                   "completed curl operations should clear interruption state");
    }
    state.disconnect(defaults);
    test.check(!state.isConnected(),
               "curl disconnect should clear the reusable session");
    {
        auto operation = state.beginOperation();
        test.check(!state.snapshot(operation),
                   "disconnected curl state should not expose a snapshot");
    }
}

void testBoundedStringSink(TestContext &test) {
    std::string output;
    openscp::curlcommon::BoundedStringSink sink{&output, 4};
    char first[] = {'a', 'b', 'c'};
    test.check(openscp::curlcommon::appendStringCallback(
                   first, 1, sizeof(first), &sink) == sizeof(first),
               "bounded response sinks should accept data within the limit");
    char overflow[] = {'d', 'e'};
    test.check(openscp::curlcommon::appendStringCallback(
                   overflow, 1, sizeof(overflow), &sink) == 0,
               "bounded response sinks should stop oversized responses");
    test.check(sink.limitExceeded && output == "abc",
               "oversized response chunks must not be partially appended");
}

} // namespace

int main() {
    TestContext test;
    testRetryAfter(test);
    testHostValidation(test);
    testClientsRejectAuthorityInjection(test);
    testFtpCommandRoot(test);
    testRemotePathNormalization(test);
    testWebDavPaths(test);
    testWebDavCompletionStatuses(test);
    testCurlClientState(test);
    testBoundedStringSink(test);
    if (test.failures != 0) {
        std::cerr << "[FAIL] openscp_curl_backend_common_tests failures="
                  << test.failures << "\n";
        return EXIT_FAILURE;
    }
    std::cout << "[OK] openscp_curl_backend_common_tests\n";
    return EXIT_SUCCESS;
}
