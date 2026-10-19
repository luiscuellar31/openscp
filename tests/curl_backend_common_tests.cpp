// Focused unit tests for security-sensitive libcurl backend helpers.
#include "IntegrationTestSupport.hpp"
#include "TestHarness.hpp"
#include "common/UniqueFile.hpp"
#include "curl/CurlBackendCommon.hpp"
#include "openscp/RemotePath.hpp"
#if OPENSCP_HAS_CURL_FTP
#include "curl/CurlFtpClient.hpp"
#endif
#if OPENSCP_HAS_CURL_WEBDAV
#include "curl/CurlWebDavClient.hpp"
#endif

#ifdef _WIN32
#include "curl/WindowsServerTrust_win.hpp"

// wincrypt.h comes before OpenSSL, whose headers undefine the names the two
// share, such as X509_NAME.
// clang-format off
#include <windows.h>
#include <wincrypt.h>
// clang-format on
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#endif

#include <openssl/ec.h>
#include <openssl/evp.h>
#include <openssl/pem.h>
#include <openssl/ssl.h>
#include <openssl/x509.h>
#include <openssl/x509v3.h>

#include <algorithm>
#include <cctype>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <memory>
#include <new>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

// Records whether curl enabled SO_KEEPALIVE, then stops before connecting.
int recordKeepalive(void *userdata, curl_socket_t socket, curlsocktype) {
    int enabled = 0;
#ifdef _WIN32
    int length = sizeof(enabled);
#else
    socklen_t length = sizeof(enabled);
#endif
    if (getsockopt(socket, SOL_SOCKET, SO_KEEPALIVE,
                   reinterpret_cast<char *>(&enabled), &length) == 0) {
        *static_cast<int *>(userdata) = enabled;
    }
    return CURL_SOCKOPT_ERROR;
}

curl_socket_t rejectSocketOpen(void *userdata, curlsocktype,
                               struct curl_sockaddr *) {
    auto *attempts = static_cast<int *>(userdata);
    if (attempts)
        ++*attempts;
    return CURL_SOCKET_BAD;
}

OPENSCP_TEST(testRetryAfter, test) {
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

// Linux takes the first CA bundle it finds; a missing file or a folder with a
// bundle's name must not stop the search.
OPENSCP_TEST(testFirstExistingFileSkipsMissingFilesAndFolders, test) {
    openscp::testsupport::IntegrationTempDir directory;
    if (!directory.isValid()) {
        test.check(false, "the CA bundle test should create its directory");
        return;
    }
    const std::filesystem::path root = directory.path();
    const std::string missing = (root / "missing.pem").string();
    const std::string folder = (root / "folder.pem").string();
    const std::string first = (root / "first.pem").string();
    const std::string second = (root / "second.pem").string();
    std::filesystem::create_directory(folder);
    std::ofstream(first) << "first";
    std::ofstream(second) << "second";

    test.check(openscp::curlcommon::firstExistingFile(
                   {missing, folder, first, second}) == first,
               "the first regular file should be the CA bundle");
    test.check(
        openscp::curlcommon::firstExistingFile({missing, folder}).empty(),
        "without a regular file there should be no CA bundle");
}

OPENSCP_TEST(testTrimAsciiPreservesContentAndWhitespaceRules, test) {
    using openscp::curlcommon::trimAscii;
    test.check(trimAscii("").empty() && trimAscii(" \t\r\n\v\f").empty(),
               "empty and whitespace-only text should trim to empty");
    for (const std::string &padding :
         {std::string(), std::string(" \t\r\n\v\f"),
          std::string(1024 * 1024, ' ')}) {
        const std::string content("a \t b\0\xff", 7);
        std::string padded = padding;
        padded += content;
        padded += padding;
        test.check(trimAscii(padded) == content,
                   "trimming must preserve internal whitespace, nulls and "
                   "high-bit bytes with short or large padding");
    }
    test.check(trimAscii("leading \t\n") == "leading" &&
                   trimAscii(" \ttrailing") == "trailing",
               "one-sided whitespace should also be removed");
    test.check(openscp::curlcommon::parseRetryAfter(
                   std::string(1024 * 1024, ' ') + "15\t", 0) ==
                   std::optional<std::uint32_t>(15),
               "Retry-After should still parse heavily padded numeric text");
}

OPENSCP_TEST(testHostValidation, test) {
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

#ifdef _WIN32
OPENSCP_TEST(testWindowsLibcurlEncodesInternationalizedHosts, test) {
    // vcpkg.json builds libcurl with WinIDN. Without it, a host such as
    // münchen.de reaches the resolver and the certificate check unencoded.
    const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
    test.check(info && (info->features & CURL_VERSION_IDN) != 0,
               "Windows builds should link a libcurl with IDN support");
    CURLU *url = curl_url();
    char *host = nullptr;
    const bool encoded =
        url &&
        curl_url_set(url, CURLUPART_URL, "ftp://m\xc3\xbcnchen.example/", 0) ==
            CURLUE_OK &&
        curl_url_get(url, CURLUPART_HOST, &host, CURLU_PUNYCODE) == CURLUE_OK &&
        host && std::string(host) == "xn--mnchen-3ya.example";
    test.check(encoded, "libcurl should encode an internationalized host name");
    curl_free(host);
    curl_url_cleanup(url);
}
#endif

OPENSCP_TEST(testClientsRejectAuthorityInjection, test) {
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

OPENSCP_TEST(testFtpCommandRoot, test) {
    using openscp::curlcommon::encodeFtpUrlPath;
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
    test.check(encodeFtpUrlPath("/workspace/release notes", false) ==
                   "/workspace/release%20notes",
               "FTP file URLs should encode normalized logical paths");
    test.check(encodeFtpUrlPath("/workspace/release notes/", true) ==
                   "/workspace/release%20notes/",
               "FTP directory URLs must retain their trailing slash");
}

OPENSCP_TEST(testRemotePathNormalization, test) {
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

OPENSCP_TEST(testWebDavPaths, test) {
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

OPENSCP_TEST(testWebDavCollectionRedirects, test) {
    using openscp::curlcommon::isWebDavCollectionRedirect;

    test.check(
        isWebDavCollectionRedirect("https://dav.example/files/team/",
                                   "/files/team") &&
            isWebDavCollectionRedirect("http://127.0.0.1:8080/files/my%20team/",
                                       "/files/my team"),
        "a redirect to the same path plus a slash names the "
        "collection URL, whatever the origin and encoding");
    test.check(!isWebDavCollectionRedirect("https://dav.example/files/team",
                                           "/files/team") &&
                   !isWebDavCollectionRedirect(
                       "https://dav.example/files/team/sub/", "/files/team") &&
                   !isWebDavCollectionRedirect("https://dav.example/login/",
                                               "/files/team") &&
                   !isWebDavCollectionRedirect("https://dav.example/", "/") &&
                   !isWebDavCollectionRedirect("not a url", "/files/team"),
               "other redirects must not be repeated with a slash");
}

OPENSCP_TEST(testWebDavCompletionStatuses, test) {
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

OPENSCP_TEST(testCurlClientState, test) {
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
        test.checkWithError(session->initialize(error),
                            "curl session should initialize: ", error);
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
    {
        std::string error;
        auto operation = state.beginConnectedOperation(error, "FTP", {"/safe"});
        test.check(operation && error.empty() &&
                       operation.connection().options.get() ==
                           connectedOptions.get(),
                   "connected curl operations should expose the shared state");
    }
    state.disconnect(defaults);
    test.check(!state.isConnected(),
               "curl disconnect should clear the reusable session");
    {
        std::string error;
        auto operation = state.beginConnectedOperation(error, "FTP", {"/safe"});
        test.check(!operation && error == "Not connected." &&
                       operation.failure().kind ==
                           openscp::RemoteErrorKind::Connection,
                   "disconnected curl operations should fail structurally");
    }
    {
        std::string error;
        auto operation = state.beginConnectedOperation(
            error, "FTP", {std::string("/unsafe\npath")});
        test.check(!operation && operation.failure().kind ==
                                     openscp::RemoteErrorKind::InvalidRequest,
                   "curl operations should validate paths before state");
    }
    {
        std::string error;
        auto operation = state.beginConnectedOperation(
            error, "FTP", {"/"}, "Refusing a semantic root operation.");
        test.check(!operation &&
                       error == "Refusing a semantic root operation." &&
                       operation.failure().kind ==
                           openscp::RemoteErrorKind::InvalidRequest,
                   "semantic preflight should run before connection state");
    }
}

OPENSCP_TEST(testBoundedStringSink, test) {
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

OPENSCP_TEST(testCurlResponseAndWriteFailures, test) {
    using openscp::RemoteErrorKind;
    using openscp::curlcommon::errorFromCurl;

    std::string output;
    openscp::curlcommon::BoundedStringSink sink{&output, 4};
    char oversized[] = {'a', 'b', 'c', 'd', 'e'};
    (void)openscp::curlcommon::appendStringCallback(oversized, 1,
                                                    sizeof(oversized), &sink);
    CURLcode code = CURLE_OK;
    std::string error;
    test.check(
        openscp::curlcommon::refusedResponse(sink, "Listing", code, error) &&
            errorFromCurl(code, error).kind == RemoteErrorKind::Protocol,
        "an oversized response should be a protocol error");

    // A stream opened for reading refuses writes, as a full disk would.
    const std::filesystem::path readOnlyPath =
        std::filesystem::temp_directory_path() /
        "openscp_curl_write_error_test";
    { std::ofstream(readOnlyPath) << "x"; }
    openscp::UniqueFile readOnly(
        std::fopen(readOnlyPath.string().c_str(), "rb"));
    openscp::curlcommon::TransferProgressContext context;
    context.downloadFile = readOnly.get();
    char data[] = {'x'};
    test.check(
        readOnly &&
            openscp::curlcommon::writeFileCallback(data, 1, 1, &context) == 0 &&
            context.writeError != 0,
        "a failed download write should keep its errno");
    readOnly.reset();
    std::error_code removeError;
    std::filesystem::remove(readOnlyPath, removeError);
}

OPENSCP_TEST(testProgressCallbackContainsExceptions, test) {
    // An exception crossing libcurl's C frames ends the process; the callback
    // has to stop the transfer instead.
    openscp::curlcommon::TransferProgressContext throwingProgress{
        [](std::size_t, std::size_t) { throw std::runtime_error("progress"); },
        {},
        nullptr,
        false};
    openscp::curlcommon::TransferProgressContext throwingCancel{
        {}, []() -> bool { throw std::bad_alloc(); }, nullptr, false};
    for (auto *context : {&throwingProgress, &throwingCancel}) {
        int result = 0;
        bool escaped = false;
        try {
            result = openscp::curlcommon::transferProgressCallback(context, 10,
                                                                   5, 0, 0);
        } catch (...) {
            escaped = true;
        }
        test.check(!escaped && result == 1,
                   "an exception from a transfer callback should stop the "
                   "transfer without leaving the callback");
    }
}

OPENSCP_TEST(testCurlFailuresNameTheirCause, test) {
    openscp::curlcommon::CurlEasySession session;
    std::string error;
    test.checkWithError(session.initialize(error),
                        "curl session should initialize: ", error);
    if (!session.get())
        return;

    // A refused connection: every libcurl version describes it in the error
    // buffer, unlike a socket an open-socket callback rejects (7.81 leaves the
    // buffer empty then).
    openscp::curlcommon::TransferProgressContext progressContext;
    const auto result = openscp::curlcommon::performCurlTransfer(
        session.get(), progressContext, "test transfer",
        [&](CURL *curl, std::string &) {
            return curl_easy_setopt(curl, CURLOPT_PROXY, "") == CURLE_OK &&
                   curl_easy_setopt(curl, CURLOPT_URL, "http://127.0.0.1:1/") ==
                       CURLE_OK;
        },
        error);
    const std::string generic = std::string("test transfer failed: ") +
                                curl_easy_strerror(result.curlCode);
    test.check(result.curlCode != CURLE_OK && error.rfind(generic, 0) == 0 &&
                   error.size() > generic.size(),
               "a curl failure should add libcurl's detail: " + error);
}

OPENSCP_TEST(testCurlConnectionsUseTcpKeepalive, test) {
    openscp::curlcommon::CurlEasySession session;
    std::string error;
    test.checkWithError(session.initialize(error),
                        "curl session should initialize: ", error);
    if (!session.get())
        return;

    int keepalive = -1;
    const bool configured =
        openscp::curlcommon::configureBaseCurlHandle(session.get(), "test",
                                                     false, 30L, error) &&
        curl_easy_setopt(session.get(), CURLOPT_PROXY, "") == CURLE_OK &&
        curl_easy_setopt(session.get(), CURLOPT_URL, "http://127.0.0.1:1/") ==
            CURLE_OK &&
        curl_easy_setopt(session.get(), CURLOPT_SOCKOPTFUNCTION,
                         recordKeepalive) == CURLE_OK &&
        curl_easy_setopt(session.get(), CURLOPT_SOCKOPTDATA, &keepalive) ==
            CURLE_OK;
    test.check(configured, "the keepalive test request should configure");
    if (configured)
        (void)curl_easy_perform(session.get());
    test.check(keepalive > 0,
               "curl connections should enable TCP keepalive, so an idle FTP "
               "control connection survives NAT timeouts");
}

// Closes a socket of the fake proxy below.
void closeTestSocket(curl_socket_t socket) {
#ifdef _WIN32
    (void)::closesocket(socket);
#else
    (void)::close(socket);
#endif
}

// What a request through the fake proxy below led to: the scheme of the
// credentials the proxy received on each connection ("" for none), and the
// request's result.
struct RefusingProxyRun {
    std::vector<std::string> authorizations;
    CURLcode code = CURLE_OK;
    std::string detail;
};

// Runs one request through a fake HTTP proxy that answers each CONNECT with
// 407 and a `scheme` challenge.
RefusingProxyRun runThroughRefusingProxy(openscp::test::TestContext &test,
                                         const std::string &scheme) {
    // On Windows libcurl starts Winsock, which the fake proxy needs too.
    openscp::curlcommon::CurlEasySession session;
    std::string error;
    const bool initialized = session.initialize(error);
    const curl_socket_t listener = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#ifdef _WIN32
    int addressLength = sizeof(address);
#else
    socklen_t addressLength = sizeof(address);
#endif
    const bool listening =
        initialized && listener != CURL_SOCKET_BAD &&
        ::bind(listener, reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) == 0 &&
        ::listen(listener, 4) == 0 &&
        ::getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                      &addressLength) == 0;
    test.check(listening, "the fake proxy should listen: " + error);
    RefusingProxyRun run;
    if (!listening) {
        if (listener != CURL_SOCKET_BAD)
            closeTestSocket(listener);
        return run;
    }
    std::thread proxy([listener, &run, &scheme] {
        for (int request = 0; request < 2; ++request) {
            const curl_socket_t client = ::accept(listener, nullptr, nullptr);
            if (client == CURL_SOCKET_BAD)
                return;
            std::string headers;
            char buffer[1024];
            while (headers.find("\r\n\r\n") == std::string::npos) {
#ifdef _WIN32
                const int n =
                    ::recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
#else
                const ssize_t n = ::recv(client, buffer, sizeof(buffer), 0);
#endif
                if (n <= 0)
                    break;
                headers.append(buffer, static_cast<std::size_t>(n));
            }
            const std::size_t start = headers.find("Proxy-Authorization: ");
            run.authorizations.push_back(
                start == std::string::npos
                    ? std::string()
                    : headers.substr(start + 21, headers.find(' ', start + 21) -
                                                     start - 21));
            const std::string challenge =
                scheme == "Digest"
                    ? "Digest realm=\"test\", nonce=\"abc\", qop=\"auth\""
                    : "Basic realm=\"test\"";
            const std::string reply =
                "HTTP/1.1 407 Proxy Authentication Required\r\n"
                "Proxy-Authenticate: " +
                challenge +
                "\r\nContent-Length: 0\r\nConnection: close\r\n\r\n";
#ifdef _WIN32
            (void)::send(client, reply.data(), static_cast<int>(reply.size()),
                         0);
#else
            (void)::send(client, reply.data(), reply.size(), 0);
#endif
            closeTestSocket(client);
        }
    });

    openscp::SessionOptions options;
    options.proxy_type = openscp::ProxyType::HttpConnect;
    options.proxy_host = "127.0.0.1";
    options.proxy_port = ntohs(address.sin_port);
    options.proxy_username = "proxy-user";
    options.proxy_password = "proxy-secret";
    const bool configured =
        openscp::curlcommon::configureProxy(session.get(), options, "test",
                                            "test", error) &&
        curl_easy_setopt(session.get(), CURLOPT_URL,
                         "http://example.invalid:80/") == CURLE_OK &&
        curl_easy_setopt(session.get(), CURLOPT_TIMEOUT, 10L) == CURLE_OK;
    test.check(configured, "the proxied request should configure: " + error);
    if (configured) {
        run.code = openscp::curlcommon::performWithErrorDetail(session.get(),
                                                               run.detail);
    }
#ifdef _WIN32
    (void)::shutdown(listener, SD_BOTH);
#else
    (void)::shutdown(listener, SHUT_RDWR);
#endif
    closeTestSocket(listener);
    proxy.join();
    return run;
}

OPENSCP_TEST(testHttpProxyAuthenticationIsNegotiated, test) {
    for (const std::string scheme : {"Digest", "Basic"}) {
        const auto authorizations =
            runThroughRefusingProxy(test, scheme).authorizations;
        test.check(!authorizations.empty() && authorizations.front().empty(),
                   "the proxy password must not be sent before the proxy "
                   "asks for it (" +
                       scheme + ")");
        test.check(authorizations.size() == 2 && authorizations[1] == scheme,
                   "curl should answer the proxy's " + scheme + " challenge");
    }
}

OPENSCP_TEST(testProxyThatRefusesTheCredentialsIsAnAuthenticationError, test) {
    // libcurl reports a refused tunnel as a connection that failed, or before
    // 8.20 as a receive error, which would be retried as a network outage.
    const RefusingProxyRun run = runThroughRefusingProxy(test, "Basic");
    const openscp::RemoteError error =
        openscp::curlcommon::errorFromCurl(run.code, run.detail);
    test.check(error.kind == openscp::RemoteErrorKind::Authentication &&
                   !error.transient,
               "a proxy that refuses the credentials should be an "
               "authentication error: " +
                   run.detail);
}

OPENSCP_TEST(testEnvironmentNoProxyKeepsTheSiteProxy, test) {
    // Unless told otherwise, libcurl reads no_proxy and NO_PROXY and connects
    // directly to the hosts they name, even with an explicit proxy, which
    // would expose the address that a site's proxy is there to hide.
    openscp::testsupport::ScopedEnvironment environment;
    environment.set("NO_PROXY", "*");
    environment.set("no_proxy", "*");
    const RefusingProxyRun run = runThroughRefusingProxy(test, "Basic");
    test.check(!run.authorizations.empty(),
               "the site's proxy should carry the request despite NO_PROXY: " +
                   run.detail);
}

#ifdef _WIN32
OPENSCP_TEST(testWindowsLibcurlNegotiatesThroughSspi, test) {
    // vcpkg.json builds libcurl with SSPI, through which Negotiate reaches
    // servers and proxies that ask for Windows authentication.
    const curl_version_info_data *info = curl_version_info(CURLVERSION_NOW);
    test.check(info && (info->features & CURL_VERSION_SSPI) != 0 &&
                   (info->features & CURL_VERSION_SPNEGO) != 0 &&
                   (info->features & CURL_VERSION_KERBEROS5) != 0,
               "Windows builds should link a libcurl with SSPI Negotiate");
}

// A certificate made for the test: an authority, self-signed unless it has
// an issuer, or a server certificate for `usage`, such as serverAuth.
struct TestCertificate {
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key{nullptr,
                                                            &EVP_PKEY_free};
    std::unique_ptr<X509, decltype(&X509_free)> certificate{nullptr,
                                                            &X509_free};
    std::vector<unsigned char> der;
};

TestCertificate makeTestCertificate(const char *commonName,
                                    const TestCertificate *issuer,
                                    bool authority, long validFromSeconds,
                                    long validUntilSeconds,
                                    const char *usage = "serverAuth") {
    static long serial = 1;
    TestCertificate made;
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> keyContext(
        EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), &EVP_PKEY_CTX_free);
    EVP_PKEY *key = nullptr;
    if (!keyContext || EVP_PKEY_keygen_init(keyContext.get()) != 1 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(keyContext.get(),
                                               NID_X9_62_prime256v1) != 1 ||
        EVP_PKEY_keygen(keyContext.get(), &key) != 1)
        return made;
    made.key.reset(key);
    made.certificate.reset(X509_new());
    X509 *certificate = made.certificate.get();
    X509_NAME *name =
        certificate ? X509_get_subject_name(certificate) : nullptr;
    X509 *signer = issuer ? issuer->certificate.get() : certificate;
    if (!name || X509_set_version(certificate, 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate), serial++) != 1 ||
        X509_NAME_add_entry_by_txt(
            name, "CN", MBSTRING_ASC,
            reinterpret_cast<const unsigned char *>(commonName), -1, -1,
            0) != 1 ||
        X509_set_issuer_name(certificate, X509_get_subject_name(signer)) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(certificate), validFromSeconds) ||
        !X509_gmtime_adj(X509_getm_notAfter(certificate), validUntilSeconds) ||
        X509_set_pubkey(certificate, key) != 1)
        return made;
    X509V3_CTX extensions;
    X509V3_set_ctx(&extensions, signer, certificate, nullptr, nullptr, 0);
    const auto add = [&](int nid, const char *value) {
        X509_EXTENSION *extension =
            X509V3_EXT_conf_nid(nullptr, &extensions, nid, value);
        const bool added =
            extension && X509_add_ext(certificate, extension, -1) == 1;
        X509_EXTENSION_free(extension);
        return added;
    };
    const bool extended =
        add(NID_subject_key_identifier, "hash") &&
        (!issuer || add(NID_authority_key_identifier, "keyid:always")) &&
        (authority ? add(NID_basic_constraints, "critical,CA:TRUE") &&
                         add(NID_key_usage, "critical,keyCertSign,cRLSign")
                   : add(NID_basic_constraints, "critical,CA:FALSE") &&
                         add(NID_key_usage, "critical,digitalSignature") &&
                         add(NID_ext_key_usage, usage) &&
                         add(NID_subject_alt_name, "DNS:server.test"));
    EVP_PKEY *signingKey = issuer ? issuer->key.get() : key;
    if (!extended || X509_sign(certificate, signingKey, EVP_sha256()) <= 0)
        return made;
    const int length = i2d_X509(certificate, nullptr);
    if (length > 0) {
        made.der.resize(static_cast<std::size_t>(length));
        unsigned char *end = made.der.data();
        if (i2d_X509(certificate, &end) != length)
            made.der.clear();
    }
    return made;
}

// A chain engine that trusts the root `der` alone, so a test needs no change
// to the user's trusted roots.
class TestChainEngine {
    public:
    explicit TestChainEngine(const std::vector<unsigned char> &der)
        : roots_(::CertOpenStore(CERT_STORE_PROV_MEMORY, 0, 0,
                                 CERT_STORE_CREATE_NEW_FLAG, nullptr)) {
        if (!roots_ ||
            !::CertAddEncodedCertificateToStore(
                roots_, X509_ASN_ENCODING, der.data(),
                static_cast<DWORD>(der.size()), CERT_STORE_ADD_ALWAYS, nullptr))
            return;
        CERT_CHAIN_ENGINE_CONFIG config{};
        config.cbSize = sizeof(config);
        config.hExclusiveRoot = roots_;
        (void)::CertCreateCertificateChainEngine(&config, &engine_);
    }
    ~TestChainEngine() {
        if (engine_)
            ::CertFreeCertificateChainEngine(engine_);
        if (roots_)
            ::CertCloseStore(roots_, 0);
    }
    TestChainEngine(const TestChainEngine &) = delete;
    TestChainEngine &operator=(const TestChainEngine &) = delete;

    [[nodiscard]] void *get() const { return engine_; }

    private:
    HCERTSTORE roots_ = nullptr;
    HCERTCHAINENGINE engine_ = nullptr;
};

OPENSCP_TEST(testWindowsTrustChecksTheServerChain, test) {
    using openscp::curlcommon::windowsServerChainError;
    constexpr long kHour = 3600;
    const TestCertificate root =
        makeTestCertificate("OpenSCP Test Root", nullptr, true, -kHour, kHour);
    const TestCertificate other =
        makeTestCertificate("Other Test Root", nullptr, true, -kHour, kHour);
    const TestCertificate intermediate = makeTestCertificate(
        "OpenSCP Test Intermediate", &root, true, -kHour, kHour);
    const TestCertificate server =
        makeTestCertificate("server.test", &intermediate, false, -kHour, kHour);
    const TestCertificate expired = makeTestCertificate(
        "server.test", &intermediate, false, -2 * kHour, -kHour);
    const TestCertificate client = makeTestCertificate(
        "server.test", &intermediate, false, -kHour, kHour, "clientAuth");
    test.check(!root.der.empty() && !other.der.empty() &&
                   !intermediate.der.empty() && !server.der.empty() &&
                   !expired.der.empty() && !client.der.empty(),
               "the test certificates should be made");
    const TestChainEngine trusting(root.der);
    const TestChainEngine stranger(other.der);
    test.check(trusting.get() && stranger.get(),
               "the test chain engines should be made");

    test.check(windowsServerChainError({server.der, intermediate.der},
                                       trusting.get()) == 0,
               "a server under a trusted root should be trusted");
    test.check(windowsServerChainError({server.der}, trusting.get()) != 0,
               "a server that leaves out its intermediate should be refused");
    test.check(windowsServerChainError({server.der, intermediate.der},
                                       stranger.get()) != 0,
               "a server under another root should be refused");
    test.check(windowsServerChainError({server.der, intermediate.der}) != 0,
               "the user's own trust should not include the test root");
    test.check(windowsServerChainError({expired.der, intermediate.der},
                                       trusting.get()) ==
                   static_cast<unsigned long>(CERT_E_EXPIRED),
               "an expired server certificate should be refused as expired");
    test.check(windowsServerChainError({client.der, intermediate.der},
                                       trusting.get()) ==
                   static_cast<unsigned long>(CERT_E_WRONG_USAGE),
               "a certificate for clients should not serve a server");
}
#endif

// Made for this test with openssl req. The pin below came from `openssl
// x509 -pubkey | openssl pkey -pubin -outform der | openssl dgst -sha256
// -binary | base64`.
constexpr char kTestCertificatePem[] = R"(-----BEGIN CERTIFICATE-----
MIIBmDCCAT+gAwIBAgIUD6mJzHJ4UN+OvMSu6RA2Xf/0gTYwCgYIKoZIzj0EAwIw
FDESMBAGA1UEAwwJbG9jYWxob3N0MB4XDTI2MTAxNjA4MTEzMFoXDTI2MTExNTA4
MTEzMFowFDESMBAGA1UEAwwJbG9jYWxob3N0MFkwEwYHKoZIzj0CAQYIKoZIzj0D
AQcDQgAE2prEIsr6LUO6d2pcv//LBQZAqNsW3tB2RVIDd4hPlPoTgr5eVaOey5DD
Fy9VZiuHvxyKb925otFbgm7npAaIR6NvMG0wHQYDVR0OBBYEFMLzn75ucgUVqzO5
esEeBgZDpsfTMB8GA1UdIwQYMBaAFMLzn75ucgUVqzO5esEeBgZDpsfTMA8GA1Ud
EwEB/wQFMAMBAf8wGgYDVR0RBBMwEYIJbG9jYWxob3N0hwR/AAABMAoGCCqGSM49
BAMCA0cAMEQCIC/4sURVyIq6VEkYhVu0L/PdpF5VoUO0LWhtO6GMtfVOAiBkUXkr
UErBZnu3mBabH5JWnZBPTJjCHWWvq+xN3ukCCg==
-----END CERTIFICATE-----
)";

OPENSCP_TEST(testPublicKeyPinFromPem, test) {
    std::string pin;
    test.check(
        openscp::curlcommon::publicKeyPinFromPem(kTestCertificatePem, pin) &&
            pin == "sha256//2YrJaarPxRSYFIDRKwT33vyJAbXl2/gvXAORgj1y8To=",
        "the pin should be the SHA-256 of the certificate's public key: " +
            pin);
    test.check(
        !openscp::curlcommon::publicKeyPinFromPem("not a certificate", pin) &&
            pin.empty(),
        "text without a certificate should give no pin");
}

#if OPENSCP_HAS_CURL_FTP || OPENSCP_HAS_CURL_WEBDAV
// A server context with a new P-256 key and a self-signed certificate.
struct TlsTestServerIdentity {
    std::unique_ptr<SSL_CTX, decltype(&SSL_CTX_free)> context{nullptr,
                                                              &SSL_CTX_free};
    std::string certificatePem;
};

TlsTestServerIdentity makeTlsTestServerIdentity() {
    TlsTestServerIdentity identity;
    std::unique_ptr<EVP_PKEY_CTX, decltype(&EVP_PKEY_CTX_free)> keyContext(
        EVP_PKEY_CTX_new_id(EVP_PKEY_EC, nullptr), &EVP_PKEY_CTX_free);
    EVP_PKEY *generatedKey = nullptr;
    if (!keyContext || EVP_PKEY_keygen_init(keyContext.get()) != 1 ||
        EVP_PKEY_CTX_set_ec_paramgen_curve_nid(keyContext.get(),
                                               NID_X9_62_prime256v1) != 1 ||
        EVP_PKEY_keygen(keyContext.get(), &generatedKey) != 1)
        return identity;
    std::unique_ptr<EVP_PKEY, decltype(&EVP_PKEY_free)> key(generatedKey,
                                                            &EVP_PKEY_free);
    std::unique_ptr<X509, decltype(&X509_free)> certificate(X509_new(),
                                                            &X509_free);
    if (!certificate)
        return identity;
    X509_NAME *name = X509_get_subject_name(certificate.get());
    const auto *commonName = reinterpret_cast<const unsigned char *>("test");
    std::unique_ptr<BIO, decltype(&BIO_free)> pem(BIO_new(BIO_s_mem()),
                                                  &BIO_free);
    if (X509_set_version(certificate.get(), 2) != 1 ||
        ASN1_INTEGER_set(X509_get_serialNumber(certificate.get()), 1) != 1 ||
        !X509_gmtime_adj(X509_getm_notBefore(certificate.get()), 0) ||
        !X509_gmtime_adj(X509_getm_notAfter(certificate.get()), 3600) ||
        X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, commonName, -1, -1,
                                   0) != 1 ||
        X509_set_issuer_name(certificate.get(), name) != 1 ||
        X509_set_pubkey(certificate.get(), key.get()) != 1 ||
        X509_sign(certificate.get(), key.get(), EVP_sha256()) <= 0 || !pem ||
        PEM_write_bio_X509(pem.get(), certificate.get()) != 1)
        return identity;
    char *pemData = nullptr;
    const long pemLength = BIO_get_mem_data(pem.get(), &pemData);
    identity.context.reset(SSL_CTX_new(TLS_server_method()));
    if (!identity.context ||
        SSL_CTX_use_certificate(identity.context.get(), certificate.get()) !=
            1 ||
        SSL_CTX_use_PrivateKey(identity.context.get(), key.get()) != 1) {
        identity.context.reset();
        return identity;
    }
    identity.certificatePem.assign(pemData,
                                   static_cast<std::size_t>(pemLength));
    return identity;
}

// Serves the first connection on a loopback port with `serve`. Reads on that
// socket give up after two seconds, so a quiet client cannot hang the test.
class LoopbackServer final {
    public:
    explicit LoopbackServer(std::function<void(curl_socket_t socket)> serve) {
#ifdef _WIN32
        WSADATA winsock{};
        winsockStarted_ = ::WSAStartup(MAKEWORD(2, 2), &winsock) == 0;
#else
        // A client that hangs up early, as a refused key makes curl do, must
        // fail the server's next write instead of ending the test process.
        std::signal(SIGPIPE, SIG_IGN);
#endif
        listener_ = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
#ifdef _WIN32
        int addressLength = sizeof(address);
#else
        socklen_t addressLength = sizeof(address);
#endif
        listening_ =
            listener_ != CURL_SOCKET_BAD &&
            ::bind(listener_, reinterpret_cast<sockaddr *>(&address),
                   sizeof(address)) == 0 &&
            ::listen(listener_, 1) == 0 &&
            ::getsockname(listener_, reinterpret_cast<sockaddr *>(&address),
                          &addressLength) == 0;
        port_ = ntohs(address.sin_port);
        if (!listening_)
            return;
        thread_ = std::thread([this, serve = std::move(serve)] {
            const curl_socket_t client = ::accept(listener_, nullptr, nullptr);
            if (client == CURL_SOCKET_BAD)
                return;
#ifdef _WIN32
            const DWORD timeout = 2000;
            (void)::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO,
                               reinterpret_cast<const char *>(&timeout),
                               sizeof(timeout));
#else
            timeval timeout{2, 0};
            (void)::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &timeout,
                               sizeof(timeout));
#endif
            serve(client);
            closeTestSocket(client);
        });
    }
    ~LoopbackServer() {
#ifdef _WIN32
        // Closing the listener ends an accept() still waiting in the thread,
        // which shutting it down does not on Windows.
        if (listener_ != CURL_SOCKET_BAD)
            closeTestSocket(listener_);
        if (thread_.joinable())
            thread_.join();
        if (winsockStarted_)
            ::WSACleanup();
#else
        if (listener_ != CURL_SOCKET_BAD)
            ::shutdown(listener_, SHUT_RDWR);
        if (thread_.joinable())
            thread_.join();
        if (listener_ != CURL_SOCKET_BAD)
            closeTestSocket(listener_);
#endif
    }
    LoopbackServer(const LoopbackServer &) = delete;
    LoopbackServer &operator=(const LoopbackServer &) = delete;

    [[nodiscard]] bool listening() const { return listening_; }
    [[nodiscard]] std::uint16_t port() const { return port_; }

    private:
    curl_socket_t listener_ = CURL_SOCKET_BAD;
    bool listening_ = false;
    std::uint16_t port_ = 0;
    std::thread thread_;
#ifdef _WIN32
    bool winsockStarted_ = false;
#endif
};

// What a client sent after the handshake, until it closed or went quiet.
struct TlsSessionRecord {
    bool handshake = false;
    std::string received;
};

// Completes the server side of the handshake on `socket`, then hands the
// session to `exchange`.
void serveTls(SSL_CTX *context, curl_socket_t socket, TlsSessionRecord &record,
              const std::function<void(SSL *)> &exchange) {
    std::unique_ptr<SSL, decltype(&SSL_free)> ssl(SSL_new(context), &SSL_free);
    // OpenSSL takes a Winsock socket as an int too.
    if (!ssl || SSL_set_fd(ssl.get(), static_cast<int>(socket)) != 1)
        return;
    record.handshake = SSL_accept(ssl.get()) == 1;
    if (record.handshake)
        exchange(ssl.get());
}

std::string readTlsUntilClosed(SSL *ssl) {
    std::string data;
    char buffer[1024];
    int count = 0;
    while ((count = SSL_read(ssl, buffer, sizeof(buffer))) > 0)
        data.append(buffer, static_cast<std::size_t>(count));
    return data;
}
#endif

#if OPENSCP_HAS_CURL_WEBDAV
openscp::SessionOptions unverifiedWebDavOptions(std::uint16_t port) {
    openscp::SessionOptions options;
    options.protocol = openscp::Protocol::WebDav;
    options.webdav_scheme = openscp::WebDavScheme::Https;
    options.webdav_verify_peer = false;
    options.host = "127.0.0.1";
    options.port = port;
    options.username = "user";
    options.password = "secret";
    return options;
}

OPENSCP_TEST(testWebDavKeyProbeSendsNoRequest, test) {
    TlsTestServerIdentity identity = makeTlsTestServerIdentity();
    std::string expectedPin;
    test.check(identity.context && openscp::curlcommon::publicKeyPinFromPem(
                                       identity.certificatePem, expectedPin),
               "the test server identity should be created");
    if (!identity.context)
        return;

    TlsSessionRecord probe;
    std::string pin;
    std::string error;
    {
        LoopbackServer server([&](curl_socket_t socket) {
            serveTls(identity.context.get(), socket, probe, [&](SSL *ssl) {
                probe.received = readTlsUntilClosed(ssl);
            });
        });
        test.check(server.listening(), "the TLS server should listen");
        (void)openscp::fetchWebDavServerPublicKeyPin(
            unverifiedWebDavOptions(server.port()), pin, error);
    }
    test.check(pin == expectedPin,
               "the probe should return the server's key pin: " + error);
    test.check(probe.handshake && probe.received.empty(),
               "the probe should send no request after the handshake");

    TlsSessionRecord mismatch;
    openscp::RemoteError mismatchError;
    {
        LoopbackServer server([&](curl_socket_t socket) {
            serveTls(identity.context.get(), socket, mismatch, [&](SSL *ssl) {
                mismatch.received = readTlsUntilClosed(ssl);
            });
        });
        openscp::SessionOptions options =
            unverifiedWebDavOptions(server.port());
        options.tls_pinned_public_key =
            "sha256//AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA=";
        openscp::CurlWebDavClient client;
        test.check(!client.connect(options, error),
                   "a key other than the pinned one should be refused");
        mismatchError = client.lastOperationError();
    }
    test.check(mismatchError.kind == openscp::RemoteErrorKind::Certificate,
               "a key mismatch should be a certificate error: " +
                   mismatchError.message);
    test.check(mismatch.received.empty(),
               "a key mismatch should stop before the request is sent");

    TlsSessionRecord pinned;
    {
        LoopbackServer server([&](curl_socket_t socket) {
            serveTls(identity.context.get(), socket, pinned, [&](SSL *ssl) {
                char buffer[1024];
                while (pinned.received.find("\r\n\r\n") == std::string::npos) {
                    const int count = SSL_read(ssl, buffer, sizeof(buffer));
                    if (count <= 0)
                        return;
                    pinned.received.append(buffer,
                                           static_cast<std::size_t>(count));
                }
                constexpr char kReply[] = "HTTP/1.1 403 Forbidden\r\n"
                                          "Content-Length: 0\r\n"
                                          "Connection: close\r\n\r\n";
                (void)SSL_write(ssl, kReply, sizeof(kReply) - 1);
            });
        });
        openscp::SessionOptions options =
            unverifiedWebDavOptions(server.port());
        options.tls_pinned_public_key = expectedPin;
        openscp::CurlWebDavClient client;
        (void)client.connect(options, error);
    }
    test.check(pinned.received.starts_with("PROPFIND "),
               "the pinned key should let the request through");
}

// Reads one HTTP request: its header block, then the body that Content-Length
// announces. Empty once the client closes or goes quiet.
std::string readHttpRequest(curl_socket_t socket) {
    std::string request;
    char character = 0;
    while (request.find("\r\n\r\n") == std::string::npos) {
        if (::recv(socket, &character, 1, 0) != 1)
            return {};
        request.push_back(character);
    }
    std::string lower = request;
    std::transform(
        lower.begin(), lower.end(), lower.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    const std::size_t field = lower.find("\r\ncontent-length:");
    const std::size_t length =
        field == std::string::npos
            ? 0
            : static_cast<std::size_t>(
                  std::strtoul(request.c_str() + field + 17, nullptr, 10));
    for (std::size_t i = 0; i < length; ++i) {
        if (::recv(socket, &character, 1, 0) != 1)
            return {};
        request.push_back(character);
    }
    return request;
}

void sendHttpReply(curl_socket_t socket, const std::string &reply) {
#ifdef _WIN32
    (void)::send(socket, reply.data(), static_cast<int>(reply.size()), 0);
#else
    (void)::send(socket, reply.data(), reply.size(), 0);
#endif
}

// Apache serves a .gz with "Content-Encoding: x-gzip"; decoding it saved the
// archive's contents under the archive's name.
OPENSCP_TEST(testWebDavDownloadsKeepTheBytesTheServerSent, test) {
    const std::string compressed("\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff"
                                 "\xcb\x48\xcd\xc9\xc9\xe7\x02\x00\x20\x30"
                                 "\x3a\x36\x06\x00\x00\x00",
                                 26); // "hello\n" in gzip
    const std::string multistatus =
        "<?xml version=\"1.0\"?><d:multistatus xmlns:d=\"DAV:\"><d:response>"
        "<d:href>/</d:href><d:propstat><d:prop><d:resourcetype>"
        "<d:collection/></d:resourcetype></d:prop>"
        "<d:status>HTTP/1.1 200 OK</d:status></d:propstat></d:response>"
        "</d:multistatus>";
    openscp::testsupport::IntegrationTempDir directory;
    if (!directory.isValid()) {
        test.check(false, "the download test should create its directory");
        return;
    }
    const std::filesystem::path local = directory.path() / "archive.tar.gz";
    std::string getRequest;
    std::string error;
    bool downloaded = false;
    {
        LoopbackServer server([&](curl_socket_t socket) {
            for (;;) {
                const std::string request = readHttpRequest(socket);
                if (request.empty())
                    return;
                if (request.starts_with("GET ")) {
                    getRequest = request;
                    sendHttpReply(socket,
                                  "HTTP/1.1 200 OK\r\n"
                                  "Content-Encoding: x-gzip\r\n"
                                  "Content-Length: " +
                                      std::to_string(compressed.size()) +
                                      "\r\n\r\n" + compressed);
                    return;
                }
                sendHttpReply(socket, "HTTP/1.1 207 Multi-Status\r\n"
                                      "Content-Type: application/xml\r\n"
                                      "Content-Length: " +
                                          std::to_string(multistatus.size()) +
                                          "\r\n\r\n" + multistatus);
            }
        });
        test.check(server.listening(), "the WebDAV server should listen");
        openscp::SessionOptions options;
        options.protocol = openscp::Protocol::WebDav;
        options.webdav_scheme = openscp::WebDavScheme::Http;
        options.host = "127.0.0.1";
        options.port = server.port();
        options.username = "user";
        options.password = "secret";
        openscp::CurlWebDavClient client;
        test.checkWithError(client.connect(options, error),
                            "the WebDAV client should connect: ", error);
        downloaded =
            client.get("/archive.tar.gz", local.string(), error, {}, {}, false);
    }
    test.checkWithError(downloaded, "the download should succeed: ", error);
    std::ifstream saved(local, std::ios::binary);
    const std::string bytes((std::istreambuf_iterator<char>(saved)),
                            std::istreambuf_iterator<char>());
    test.check(bytes == compressed,
               "a download should keep the bytes the server sent");
    std::string lowerGet = getRequest;
    std::transform(
        lowerGet.begin(), lowerGet.end(), lowerGet.begin(),
        [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    test.check(!getRequest.empty() &&
                   lowerGet.find("accept-encoding") == std::string::npos,
               "a download should not ask for compression");
}

OPENSCP_TEST(testVerifiedWebDavRefusesAnUntrustedCertificate, test) {
    // Without a CA file, curl checks the trusted roots of the system, which
    // on Windows come from its certificate store, not from OpenSSL.
    TlsTestServerIdentity identity = makeTlsTestServerIdentity();
    test.check(static_cast<bool>(identity.context),
               "the test server identity should be created");
    if (!identity.context)
        return;
    TlsSessionRecord session;
    openscp::RemoteError failure;
    std::string error;
    {
        LoopbackServer server([&](curl_socket_t socket) {
            serveTls(identity.context.get(), socket, session, [&](SSL *ssl) {
                session.received = readTlsUntilClosed(ssl);
            });
        });
        test.check(server.listening(), "the TLS server should listen");
        openscp::SessionOptions options =
            unverifiedWebDavOptions(server.port());
        options.webdav_verify_peer = true;
        openscp::CurlWebDavClient client;
        test.check(!client.connect(options, error),
                   "a self-signed certificate should be refused");
        failure = client.lastOperationError();
    }
    test.check(failure.kind == openscp::RemoteErrorKind::Certificate,
               "an untrusted certificate should be a certificate error: " +
                   failure.message);
    test.check(session.received.empty(),
               "no request should reach a server that failed verification");
}
#endif

#if OPENSCP_HAS_CURL_FTP
std::string readPlainLine(curl_socket_t socket) {
    std::string line;
    char character = 0;
    while (::recv(socket, &character, 1, 0) == 1 && character != '\n')
        line.push_back(character);
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    return line;
}

std::string readTlsLine(SSL *ssl) {
    std::string line;
    char character = 0;
    while (SSL_read(ssl, &character, 1) == 1 && character != '\n')
        line.push_back(character);
    if (!line.empty() && line.back() == '\r')
        line.pop_back();
    return line;
}

OPENSCP_TEST(testFtpsKeyProbeNeverSendsAPassword, test) {
    TlsTestServerIdentity identity = makeTlsTestServerIdentity();
    std::string expectedPin;
    test.check(identity.context && openscp::curlcommon::publicKeyPinFromPem(
                                       identity.certificatePem, expectedPin),
               "the test server identity should be created");
    if (!identity.context)
        return;

    TlsSessionRecord session;
    std::vector<std::string> commands;
    std::string pin;
    std::string error;
    {
        LoopbackServer server([&](curl_socket_t socket) {
            constexpr char kGreeting[] = "220 test\r\n";
            (void)::send(socket, kGreeting, sizeof(kGreeting) - 1, 0);
            if (readPlainLine(socket) != "AUTH TLS")
                return;
            constexpr char kAuthReply[] = "234 go ahead\r\n";
            (void)::send(socket, kAuthReply, sizeof(kAuthReply) - 1, 0);
            serveTls(identity.context.get(), socket, session, [&](SSL *ssl) {
                for (std::string line = readTlsLine(ssl); !line.empty();
                     line = readTlsLine(ssl)) {
                    commands.push_back(line);
                    const std::string reply = line.starts_with("USER ")
                                                  ? "331 password\r\n"
                                                  : "530 no\r\n";
                    (void)SSL_write(ssl, reply.data(),
                                    static_cast<int>(reply.size()));
                }
            });
        });
        test.check(server.listening(), "the FTPS server should listen");
        openscp::SessionOptions options;
        options.protocol = openscp::Protocol::Ftps;
        options.ftps_mode = openscp::FtpsMode::ExplicitTls;
        options.ftps_verify_peer = false;
        options.host = "127.0.0.1";
        options.port = server.port();
        options.username = "user";
        options.password = "secret";
        (void)openscp::fetchFtpsServerPublicKeyPin(options, pin, error);
    }
    test.check(pin == expectedPin,
               "the probe should return the server's key pin: " + error);
    test.check(session.handshake && !commands.empty() &&
                   commands.front() == "USER anonymous",
               "the probe should log in anonymously, not as the user");
    test.check(std::none_of(commands.begin(), commands.end(),
                            [](const std::string &command) {
                                return command.starts_with("PASS");
                            }),
               "the probe should stop before sending a password");
}

OPENSCP_TEST(testVerifiedFtpsRefusesAnUntrustedCertificate, test) {
    // As for WebDAV, the trusted roots are the system's, from its
    // certificate store on Windows.
    TlsTestServerIdentity identity = makeTlsTestServerIdentity();
    test.check(static_cast<bool>(identity.context),
               "the test server identity should be created");
    if (!identity.context)
        return;
    TlsSessionRecord session;
    std::vector<std::string> commands;
    openscp::RemoteError failure;
    std::string error;
    {
        LoopbackServer server([&](curl_socket_t socket) {
            constexpr char kGreeting[] = "220 test\r\n";
            (void)::send(socket, kGreeting, sizeof(kGreeting) - 1, 0);
            if (readPlainLine(socket) != "AUTH TLS")
                return;
            constexpr char kAuthReply[] = "234 go ahead\r\n";
            (void)::send(socket, kAuthReply, sizeof(kAuthReply) - 1, 0);
            serveTls(identity.context.get(), socket, session, [&](SSL *ssl) {
                for (std::string line = readTlsLine(ssl); !line.empty();
                     line = readTlsLine(ssl))
                    commands.push_back(line);
            });
        });
        test.check(server.listening(), "the FTPS server should listen");
        openscp::SessionOptions options;
        options.protocol = openscp::Protocol::Ftps;
        options.ftps_mode = openscp::FtpsMode::ExplicitTls;
        options.ftps_verify_peer = true;
        options.host = "127.0.0.1";
        options.port = server.port();
        options.username = "user";
        options.password = "secret";
        openscp::CurlFtpClient client(openscp::Protocol::Ftps);
        test.check(!client.connect(options, error),
                   "a self-signed certificate should be refused");
        failure = client.lastOperationError();
    }
    test.check(failure.kind == openscp::RemoteErrorKind::Certificate,
               "an untrusted certificate should be a certificate error: " +
                   failure.message);
    test.check(commands.empty(),
               "no command, and so no password, should follow a failed "
               "verification");
}
#endif

OPENSCP_TEST(testCurlTransportPolicy, test) {
    using openscp::curlcommon::configureAllowedProtocol;
    using openscp::curlcommon::configureTlsPolicy;
    using openscp::curlcommon::CurlUrlScheme;

    openscp::curlcommon::CurlEasySession session;
    std::string error;
    test.checkWithError(session.initialize(error),
                        "curl session should initialize: ", error);
    if (!session.get())
        return;

    struct AllowedCase {
        CurlUrlScheme scheme;
        const char *url;
        bool tls;
    };
    constexpr AllowedCase allowedCases[] = {
        {CurlUrlScheme::Ftp, "ftp://127.0.0.1:1/", false},
        {CurlUrlScheme::Ftps, "ftps://127.0.0.1:1/", true},
        {CurlUrlScheme::Http, "http://127.0.0.1:1/", false},
        {CurlUrlScheme::Https, "https://127.0.0.1:1/", true},
    };

    for (const AllowedCase &allowed : allowedCases) {
        session.reset();
        error.clear();
        const bool protocolConfigured = configureAllowedProtocol(
            session.get(), allowed.scheme, "test", error);
        test.check(protocolConfigured,
                   std::string("allowed protocol should configure: ") + error);
        if (allowed.tls) {
            const std::optional<std::string> caPath = "openscp-test-ca.pem";
            const std::optional<std::string> pin =
                "sha256//2YrJaarPxRSYFIDRKwT33vyJAbXl2/gvXAORgj1y8To=";
            const bool tlsConfigured =
                configureTlsPolicy(session.get(), true, caPath, pin,
                                   "Could not enforce test TLS minimum.",
                                   "Could not configure test TLS verification.",
                                   "Could not configure test CA path.", error);
            test.check(tlsConfigured,
                       std::string("TLS 1.2 policy should configure: ") +
                           error);
        }

        int socketAttempts = 0;
        const bool requestConfigured =
            curl_easy_setopt(session.get(), CURLOPT_PROXY, "") == CURLE_OK &&
            curl_easy_setopt(session.get(), CURLOPT_URL, allowed.url) ==
                CURLE_OK &&
            curl_easy_setopt(session.get(), CURLOPT_OPENSOCKETFUNCTION,
                             rejectSocketOpen) == CURLE_OK &&
            curl_easy_setopt(session.get(), CURLOPT_OPENSOCKETDATA,
                             &socketAttempts) == CURLE_OK;
        test.check(requestConfigured,
                   "transport policy test request should configure");
        if (!protocolConfigured || !requestConfigured)
            continue;
        const CURLcode result = curl_easy_perform(session.get());
        test.check(result != CURLE_UNSUPPORTED_PROTOCOL && socketAttempts > 0,
                   "the selected URL scheme should reach socket setup");
    }

    session.reset();
    error.clear();
    const bool restricted = configureAllowedProtocol(
        session.get(), CurlUrlScheme::Https, "test", error);
    int blockedSocketAttempts = 0;
    const bool blockedRequestConfigured =
        curl_easy_setopt(session.get(), CURLOPT_PROXY, "") == CURLE_OK &&
        curl_easy_setopt(session.get(), CURLOPT_URL, "ftp://127.0.0.1:1/") ==
            CURLE_OK &&
        curl_easy_setopt(session.get(), CURLOPT_OPENSOCKETFUNCTION,
                         rejectSocketOpen) == CURLE_OK &&
        curl_easy_setopt(session.get(), CURLOPT_OPENSOCKETDATA,
                         &blockedSocketAttempts) == CURLE_OK;
    test.check(restricted && blockedRequestConfigured,
               "blocked protocol test request should configure");
    if (restricted && blockedRequestConfigured) {
        const CURLcode result = curl_easy_perform(session.get());
        test.check(result == CURLE_UNSUPPORTED_PROTOCOL &&
                       blockedSocketAttempts == 0,
                   "a disallowed URL scheme must fail before socket setup");
    }
}

OPENSCP_TEST(testCurlTransferLifecycle, test) {
    std::uint64_t missingSize = 42;
    std::string missingError;
    test.check(openscp::curlcommon::openFileForUpload(
                   "openscp-definitely-missing-upload-file", missingSize,
                   missingError) == nullptr &&
                   missingSize == 0 && !missingError.empty(),
               "upload preparation should reject missing local files");

    openscp::curlcommon::CurlEasySession session;
    std::string error;
    test.checkWithError(session.initialize(error),
                        "curl session should initialize: ", error);
    if (!session.get())
        return;

    openscp::curlcommon::TransferProgressContext progressContext;
    const auto configurationFailure = openscp::curlcommon::performCurlTransfer(
        session.get(), progressContext, "test transfer",
        [](CURL *, std::string &) { return false; }, error);
    test.check(configurationFailure.failure ==
                   openscp::curlcommon::CurlTransferFailure::Configuration,
               "the common transfer lifecycle should classify setup failures");
    test.check(!error.empty(),
               "setup failures should always produce a diagnostic");

    error.clear();
    progressContext.shouldCancel = [] { return true; };
    const auto canceled = openscp::curlcommon::performCurlTransfer(
        session.get(), progressContext, "test transfer",
        [](CURL *curl, std::string &) {
            return curl_easy_setopt(curl, CURLOPT_URL,
                                    "https://example.invalid/") == CURLE_OK;
        },
        error);
    test.check(canceled.failure ==
                       openscp::curlcommon::CurlTransferFailure::Canceled &&
                   canceled.curlCode == CURLE_ABORTED_BY_CALLBACK,
               "cancellation should be observed before network I/O");
    test.check(error == "Canceled by user",
               "user cancellation should retain a stable diagnostic");
    const openscp::RemoteError canceledError =
        openscp::curlcommon::transferFailureError(canceled, error);
    test.check(canceledError.kind == openscp::RemoteErrorKind::Canceled &&
                   canceledError.native_code == CURLE_ABORTED_BY_CALLBACK,
               "common transfer failures should preserve cancellation data");

    const openscp::RemoteError configurationError =
        openscp::curlcommon::transferFailureError(configurationFailure,
                                                  "invalid setup");
    test.check(configurationError.kind ==
                       openscp::RemoteErrorKind::InvalidRequest &&
                   configurationError.message == "invalid setup",
               "common transfer failures should classify invalid setup");

    bool cancelAtBoundary = false;
    openscp::curlcommon::TransferProgressContext boundaryProgress{
        [&](std::size_t done, std::size_t total) {
            cancelAtBoundary = total > 0 && done >= total;
        },
        [&] { return cancelAtBoundary; }, nullptr, true};
    test.check(openscp::curlcommon::transferProgressCallback(
                   &boundaryProgress, 0, 0, 16, 16) == 0 &&
                   boundaryProgress.payloadComplete && cancelAtBoundary,
               "boundary cancellation should let the remote partial commit");
    error.clear();
    test.check(openscp::curlcommon::detectTransferCancellation(boundaryProgress,
                                                               error) &&
                   error == "Canceled by user",
               "boundary cancellation should still prevent final publish");

    openscp::curlcommon::TransferProgressContext incompleteProgress{
        {}, [] { return true; }, nullptr, true};
    test.check(openscp::curlcommon::transferProgressCallback(
                   &incompleteProgress, 0, 0, 16, 8) == 1 &&
                   !incompleteProgress.payloadComplete,
               "incomplete transfers should abort immediately on cancel");

    openscp::UniqueFile rewindable(std::tmpfile());
    test.check(static_cast<bool>(rewindable),
               "the upload rewind test should create a temporary file");
    if (rewindable) {
        constexpr char payload[] = "rewindable";
        test.check(std::fwrite(payload, 1, sizeof(payload), rewindable.get()) ==
                       sizeof(payload),
                   "the upload rewind test should seed its file");
        test.check(openscp::curlcommon::seekFileCallback(
                       rewindable.get(), 0, SEEK_SET) == CURL_SEEKFUNC_OK,
                   "curl uploads should rewind after authentication retries");
        char firstByte = '\0';
        test.check(std::fread(&firstByte, 1, 1, rewindable.get()) == 1 &&
                       firstByte == payload[0],
                   "rewound uploads should restart at the requested offset");
    }
}

} // namespace

int main() {
    openscp::test::TestHarness harness("curl backend common");
    return harness.run();
}
