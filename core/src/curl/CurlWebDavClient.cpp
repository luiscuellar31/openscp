// WebDAV backend implementation based on libcurl and tinyxml2.
#include "curl/CurlWebDavClient.hpp"

#include "CurlBackendCommon.hpp"
#include "CurlListingParser.hpp"
#include "openscp/RemotePath.hpp"

#include <curl/curl.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace openscp {
namespace {

struct WebDavResponse {
    long statusCode = 0;
    std::string body;
    std::optional<std::uint32_t> retryAfterSeconds;
};

struct WebDavTextRequest {
    std::string method;
    std::string remotePath;
    const std::string *body = nullptr;
    std::vector<std::string> headers;
    // The target is a collection, so its URL ends in a slash.
    bool collection = false;
};

SessionOptions defaultWebDavOptions() {
    SessionOptions options;
    options.protocol = Protocol::WebDav;
    options.webdav_scheme = WebDavScheme::Https;
    options.webdav_base_path = "/";
    options.port = defaultPortForWebDavScheme(options.webdav_scheme);
    return options;
}

using curlcommon::ensureCurlInitialized;
using curlcommon::parseUnsignedDec;
using curlcommon::toLowerAscii;
using curlcommon::trimAscii;

size_t captureWebDavHeader(char *ptr, size_t size, size_t nmemb,
                           void *userdata) {
    if (!userdata)
        return 0;
    const size_t total = size * nmemb;
    // An exception must not cross libcurl's C frames; returning a short
    // count aborts the transfer instead.
    try {
        std::string line(ptr, total);
        const std::string lowered = toLowerAscii(line);
        static constexpr std::string_view prefix = "retry-after:";
        if (lowered.rfind(prefix, 0) == 0) {
            const std::string value = trimAscii(line.substr(prefix.size()));
            auto *retryAfter =
                static_cast<std::optional<std::uint32_t> *>(userdata);
            *retryAfter = curlcommon::parseRetryAfter(value);
        }
    } catch (...) {
        return 0;
    }
    return total;
}

std::string normalizeRemoteDirPath(std::string path) {
    path = normalizeRemotePath(path);
    if (path != "/" && !path.empty() && path.back() != '/')
        path.push_back('/');
    return path;
}

std::string buildWebDavUrl(const SessionOptions &opt,
                           const std::string &remotePath,
                           bool collection = false) {
    const std::string host = curlcommon::normalizeHostAuthorityForUrl(opt.host);
    std::string serverPath =
        curlcommon::webDavServerPath(opt.webdav_base_path, remotePath);
    if (collection && serverPath.back() != '/')
        serverPath.push_back('/');
    const std::string path = curlcommon::encodeUrlPath(serverPath);
    return std::string(webDavSchemeStorageName(
               normalizeWebDavScheme(opt.webdav_scheme))) +
           "://" + host + ":" + std::to_string(opt.port) + path;
}

std::string webDavDestinationKey(const SessionOptions &opt,
                                 const std::string &remotePath) {
    return std::string("remote:webdav:") +
           webDavSchemeStorageName(normalizeWebDavScheme(opt.webdav_scheme)) +
           ":" + toLowerAscii(opt.host) + ":" + std::to_string(opt.port) + ":" +
           opt.username + ":" + normalizeWebDavBasePath(opt.webdav_base_path) +
           ":" + normalizeRemotePath(remotePath);
}

std::string formatHttpFailure(const char *what, long statusCode) {
    std::ostringstream out;
    out << (what ? what : "WebDAV operation") << " failed with HTTP status "
        << statusCode << ".";
    return out.str();
}

RemoteError webDavMutationStatusError(
    long statusCode, std::string message,
    std::optional<std::uint32_t> retryAfter = std::nullopt) {
    RemoteError error = curlcommon::errorFromHttpStatus(
        statusCode, std::move(message), true, retryAfter);
    if (statusCode >= 200 && statusCode < 300 &&
        !curlcommon::isCompletedWebDavWriteStatus(statusCode)) {
        error.kind = RemoteErrorKind::RemoteIo;
        error.transient = false;
        error.commit_uncertain = true;
    }
    return error;
}

bool configureCommonCurlHandle(CURL *curl, const SessionOptions &opt,
                               bool basicAuthPinned, std::string &err) {
    if (!curlcommon::configureBaseCurlHandle(curl, "WebDAV", true, std::nullopt,
                                             err))
        return false;
    const WebDavScheme webDavScheme = normalizeWebDavScheme(opt.webdav_scheme);
    const curlcommon::CurlUrlScheme urlScheme =
        webDavScheme == WebDavScheme::Https ? curlcommon::CurlUrlScheme::Https
                                            : curlcommon::CurlUrlScheme::Http;
    if (!curlcommon::configureAllowedProtocol(curl, urlScheme, "WebDAV", err))
        return false;

    if (!opt.username.empty()) {
        // Negotiating (CURLAUTH_ANY) sends every request once without
        // credentials, because curl_easy_reset() forgets the method picked by
        // the previous 401. A connection pinned to Basic sends it up front.
        const unsigned long httpAuth =
            basicAuthPinned ? CURLAUTH_BASIC : CURLAUTH_ANY;
        if (curl_easy_setopt(curl, CURLOPT_USERNAME, opt.username.c_str()) !=
                CURLE_OK ||
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, httpAuth) != CURLE_OK) {
            err = "Could not configure WebDAV authentication username.";
            return false;
        }
    }
    if (opt.password && !opt.password->empty()) {
        if (curl_easy_setopt(curl, CURLOPT_PASSWORD, opt.password->c_str()) !=
            CURLE_OK) {
            err = "Could not configure WebDAV authentication password.";
            return false;
        }
    }

    if (webDavScheme == WebDavScheme::Https) {
        if (!curlcommon::configureTlsPolicy(
                curl, opt.webdav_verify_peer, opt.webdav_ca_cert_path,
                opt.tls_pinned_public_key,
                "Could not enforce the WebDAV TLS 1.2 minimum.",
                "Could not configure WebDAV TLS verification policy.",
                "Could not configure WebDAV TLS CA bundle.", err)) {
            return false;
        }
    }

    return curlcommon::configureProxy(curl, opt, "WebDAV", "WebDAV", err);
}

// Returns true when a request sent with pinned Basic credentials was refused
// while the server offers another method (such as Digest on one collection).
// Negotiation is then restored for the connection so the caller can repeat
// the request once.
bool unpinBasicAuthAfterRejection(CURL *curl, bool &basicAuthPinned,
                                  long statusCode) {
    if (!basicAuthPinned || statusCode != 401)
        return false;
    long available = 0;
    if (curl_easy_getinfo(curl, CURLINFO_HTTPAUTH_AVAIL, &available) !=
            CURLE_OK ||
        (static_cast<unsigned long>(available) & ~CURLAUTH_BASIC) == 0) {
        return false;
    }
    basicAuthPinned = false;
    return true;
}

bool performTextRequestOnce(CURL *curl, const SessionOptions &opt,
                            bool basicAuthPinned,
                            const WebDavTextRequest &request,
                            const std::atomic<bool> *interrupted,
                            WebDavResponse &response, std::string &err,
                            CURLcode *curlCodeOut) {
    // Generic request helper for WebDAV verbs with text/XML payloads.
    response = WebDavResponse{};
    if (curlCodeOut)
        *curlCodeOut = CURLE_OK;
    curl_easy_reset(curl);
    if (!configureCommonCurlHandle(curl, opt, basicAuthPinned, err)) {
        return false;
    }

    const std::string url =
        buildWebDavUrl(opt, request.remotePath, request.collection);
    struct curl_slist *headerList = nullptr;
    for (const std::string &h : request.headers)
        headerList = curl_slist_append(headerList, h.c_str());

    curlcommon::TransferProgressContext cancelContext{
        {},
        [token = opt.connection_stop_token] { return token.stop_requested(); },
        interrupted,
        false};
    curlcommon::BoundedStringSink responseSink{&response.body};
    const bool configured =
        (curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST,
                          request.method.c_str()) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                          curlcommon::appendStringCallback) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_WRITEDATA, &responseSink) ==
         CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION, captureWebDavHeader) ==
         CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_HEADERDATA,
                          &response.retryAfterSeconds) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                          curlcommon::transferProgressCallback) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancelContext) ==
         CURLE_OK);
    if (!configured) {
        err = std::string("Could not configure WebDAV ") + request.method +
              " request.";
        curl_slist_free_all(headerList);
        return false;
    }

    if (request.body) {
        if (curl_easy_setopt(curl, CURLOPT_POSTFIELDS, request.body->c_str()) !=
                CURLE_OK ||
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(request.body->size())) !=
                CURLE_OK) {
            err = std::string("Could not configure WebDAV request body for ") +
                  request.method + ".";
            curl_slist_free_all(headerList);
            return false;
        }
    }

    if (curlcommon::rejectInterrupted(interrupted, err, curlCodeOut)) {
        curl_slist_free_all(headerList);
        return false;
    }
    std::string detail;
    CURLcode rc = curlcommon::performWithErrorDetail(curl, detail);
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
    curl_slist_free_all(headerList);

    const bool refused = curlcommon::refusedResponse(
        responseSink, std::string("WebDAV ") + request.method + " response", rc,
        err);
    if (curlCodeOut)
        *curlCodeOut = rc;
    if (refused)
        return false;
    if (rc != CURLE_OK) {
        err = std::string("WebDAV ") + request.method +
              " failed: " + curlcommon::curlFailureText(rc, detail);
        return false;
    }
    return true;
}

// Apache's mod_dir, with its default DirectorySlash On, redirects a request
// for a collection without the trailing slash to the same URL with it.
bool redirectsToCollectionUrl(CURL *curl, const SessionOptions &opt,
                              const WebDavTextRequest &request,
                              long statusCode) {
    if (request.collection || (statusCode != 301 && statusCode != 302 &&
                               statusCode != 307 && statusCode != 308)) {
        return false;
    }
    char *redirectUrl = nullptr;
    return curl_easy_getinfo(curl, CURLINFO_REDIRECT_URL, &redirectUrl) ==
               CURLE_OK &&
           redirectUrl &&
           curlcommon::isWebDavCollectionRedirect(
               redirectUrl, curlcommon::webDavServerPath(opt.webdav_base_path,
                                                         request.remotePath));
}

bool performTextRequest(CURL *curl, const SessionOptions &opt,
                        bool &basicAuthPinned, const WebDavTextRequest &request,
                        const std::atomic<bool> *interrupted,
                        WebDavResponse &response, std::string &err,
                        CURLcode *curlCodeOut = nullptr) {
    bool ok = performTextRequestOnce(curl, opt, basicAuthPinned, request,
                                     interrupted, response, err, curlCodeOut);
    if (ok && unpinBasicAuthAfterRejection(curl, basicAuthPinned,
                                           response.statusCode)) {
        ok = performTextRequestOnce(curl, opt, basicAuthPinned, request,
                                    interrupted, response, err, curlCodeOut);
    }
    // Redirects are never followed; this one is repeated, once, with the
    // collection URL on the same server.
    if (!ok ||
        !redirectsToCollectionUrl(curl, opt, request, response.statusCode))
        return ok;
    WebDavTextRequest collectionRequest = request;
    collectionRequest.collection = true;
    return performTextRequest(curl, opt, basicAuthPinned, collectionRequest,
                              interrupted, response, err, curlCodeOut);
}

bool isDirectChildPath(const std::string &parentPath, const std::string &path,
                       std::string &childName) {
    childName.clear();
    const std::string parentDir = normalizeRemoteDirPath(parentPath);
    if (path == normalizeRemotePath(parentPath))
        return false;
    if (path.rfind(parentDir, 0) != 0)
        return false;

    std::string tail = path.substr(parentDir.size());
    if (!tail.empty() && tail.back() == '/')
        tail.pop_back();
    if (tail.empty())
        return false;
    if (tail.find('/') != std::string::npos)
        return false;
    childName = tail;
    return true;
}

bool isSuccessStatus(long status) {
    return status >= 200 && status < 300;
}

bool isPathMissingStatus(long status) {
    return status == 404;
}

const std::string &propfindBody() {
    static const std::string body =
        "<?xml version=\"1.0\" encoding=\"utf-8\"?>"
        "<d:propfind xmlns:d=\"DAV:\">"
        "<d:prop><d:resourcetype/><d:getcontentlength/><d:getlastmodified/>"
        "</d:prop></d:propfind>";
    return body;
}

bool performPropfind(CURL *curl, const SessionOptions &opt,
                     bool &basicAuthPinned, const std::string &remotePath,
                     int depth, const std::atomic<bool> *interrupted,
                     WebDavResponse &response, std::string &err,
                     CURLcode *curlCodeOut = nullptr, bool collection = false) {
    // PROPFIND drives both stat(depth=0) and list(depth=1).
    const std::string &body = propfindBody();
    std::vector<std::string> headers = {
        "Depth: " + std::to_string(depth),
        "Content-Type: application/xml; charset=utf-8",
    };
    return performTextRequest(curl, opt, basicAuthPinned,
                              WebDavTextRequest{"PROPFIND", remotePath, &body,
                                                std::move(headers), collection},
                              interrupted, response, err, curlCodeOut);
}

bool unsupportedWebDavOperation(const char *what, std::string &err) {
    err = std::string("WebDAV backend does not support ") + what + ".";
    return false;
}

enum class WebDavResourceKind { Missing, File, Collection };

struct WebDavResourceProbe {
    WebDavResourceKind kind = WebDavResourceKind::Missing;
    std::size_t childCount = 0;
};

// Describes the target of a mutation that the server would otherwise apply to
// a whole collection; depth 1 also counts the collection's children. Returns
// false, with failure set, when the target cannot be identified.
bool probeWebDavResource(CURL *curl, const SessionOptions &opt,
                         bool &basicAuthPinned, const std::string &remotePath,
                         int depth, const std::atomic<bool> *interrupted,
                         WebDavResourceProbe &probe, RemoteError &failure) {
    probe = WebDavResourceProbe{};
    WebDavResponse response;
    CURLcode rc = CURLE_OK;
    std::string err;
    if (!performPropfind(curl, opt, basicAuthPinned, remotePath, depth,
                         interrupted, response, err, &rc)) {
        if (rc == CURLE_ABORTED_BY_CALLBACK && interrupted &&
            interrupted->load())
            err = "Interrupted";
        failure = curlcommon::errorFromCurl(rc, err, response.statusCode);
        return false;
    }
    if (isPathMissingStatus(response.statusCode))
        return true;
    if (!isSuccessStatus(response.statusCode)) {
        failure = curlcommon::errorFromHttpStatus(
            response.statusCode,
            formatHttpFailure("WebDAV PROPFIND", response.statusCode), false,
            response.retryAfterSeconds);
        return false;
    }

    std::vector<curlparser::WebDavResource> resources;
    if (curlparser::parseWebDavPropfindResponse(opt, response.body, resources,
                                                err) !=
        curlparser::ListingParseStatus::Success) {
        failure.kind = RemoteErrorKind::Protocol;
        failure.message = err;
        return false;
    }
    const std::string target = normalizeRemotePath(remotePath);
    auto it = std::find_if(resources.begin(), resources.end(),
                           [&target](const curlparser::WebDavResource &r) {
                               return r.path == target;
                           });
    // As in stat(), a single resource answers for the target.
    if (it == resources.end() && depth == 0 && resources.size() == 1)
        it = resources.begin();
    if (it == resources.end()) {
        failure.kind = RemoteErrorKind::Protocol;
        failure.message = "WebDAV PROPFIND did not describe the target.";
        return false;
    }
    probe.kind =
        it->isDir ? WebDavResourceKind::Collection : WebDavResourceKind::File;
    std::string childName;
    for (const curlparser::WebDavResource &resource : resources) {
        if (isDirectChildPath(target, resource.path, childName))
            ++probe.childCount;
    }
    return true;
}

} // namespace

bool fetchWebDavServerPublicKeyPin(const SessionOptions &opt, std::string &pin,
                                   std::string &err) {
    pin.clear();
    err.clear();
    if (opt.protocol != Protocol::WebDav ||
        normalizeWebDavScheme(opt.webdav_scheme) != WebDavScheme::Https) {
        err = "Only HTTPS WebDAV connections present a TLS certificate.";
        return false;
    }
    if (!curlcommon::validateUrlHost(opt.host, "WebDAV host", err) ||
        !ensureCurlInitialized(err))
        return false;
    SessionOptions probeOptions = opt;
    probeOptions.username.clear();
    probeOptions.password.reset();
    probeOptions.webdav_scheme = WebDavScheme::Https;
    probeOptions.webdav_base_path =
        normalizeWebDavBasePath(probeOptions.webdav_base_path);
    probeOptions.webdav_verify_peer = false;
    probeOptions.tls_pinned_public_key.reset();
    if (probeOptions.port == 0)
        probeOptions.port = defaultPortForWebDavScheme(WebDavScheme::Https);
    curlcommon::CurlEasySession session;
    if (!session.initialize(err) ||
        !configureCommonCurlHandle(session.get(), probeOptions, false, err))
        return false;
    return curlcommon::readServerPublicKeyPin(
        session.get(), buildWebDavUrl(probeOptions, "/"),
        opt.connection_stop_token, pin, err);
}

CurlWebDavClient::CurlWebDavClient()
    : state_(std::make_unique<curlcommon::CurlClientState>(
          defaultWebDavOptions())) {
}

CurlWebDavClient::~CurlWebDavClient() {
    disconnect();
}

bool CurlWebDavClient::connect(const SessionOptions &opt, std::string &err) {
    auto operation = state_->beginOperation();
    clearLastOperationError();
    err.clear();
    if (operation.disconnecting()) {
        err = "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err);
        return false;
    }
    if (!curlcommon::validateUrlHost(opt.host, "WebDAV host", err)) {
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    if (opt.protocol != Protocol::WebDav) {
        err = "CurlWebDavClient only supports WebDAV protocol.";
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    if (opt.jump_host && !opt.jump_host->empty()) {
        err = "WebDAV backend does not support SSH jump host.";
        setLastOperationError(RemoteErrorKind::Unsupported, err);
        return false;
    }
    if (!curlcommon::validateRemotePath(opt.webdav_base_path, "WebDAV base",
                                        err)) {
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    if (!ensureCurlInitialized(err)) {
        setLastOperationError(RemoteErrorKind::LocalIo, err);
        return false;
    }

    SessionOptions normalized = opt;
    normalized.webdav_scheme = normalizeWebDavScheme(normalized.webdav_scheme);
    normalized.webdav_base_path =
        normalizeWebDavBasePath(normalized.webdav_base_path);
    if (normalized.port == 0)
        normalized.port = defaultPortForWebDavScheme(normalized.webdav_scheme);
    normalized.protocol = Protocol::WebDav;

    state_->prepareForConnect();
    auto newEasySession = std::make_unique<curlcommon::CurlEasySession>();
    if (!newEasySession->initialize(err)) {
        setLastOperationError(RemoteErrorKind::LocalIo, err);
        return false;
    }
    CURL *curl = newEasySession->get();
    WebDavResponse probe;
    CURLcode probeCode = CURLE_OK;
    bool probeBasicAuthPinned = false;
    if (!performPropfind(curl, normalized, probeBasicAuthPinned, "/", 0,
                         operation.interrupted(), probe, err, &probeCode,
                         true)) {
        setLastOperationError(
            curlcommon::errorFromCurl(probeCode, err, probe.statusCode));
        return false;
    }
    if (!isSuccessStatus(probe.statusCode)) {
        err = formatHttpFailure("WebDAV connect probe", probe.statusCode);
        setLastOperationError(curlcommon::errorFromHttpStatus(
            probe.statusCode, err, false, probe.retryAfterSeconds));
        return false;
    }

    // The probe negotiated. If its 401 offered only Basic, the connection
    // sends Basic up front from now on; with Digest or stronger methods on
    // offer it keeps negotiating, as libcurl would pick those.
    long authAvailable = 0;
    basicAuthPinned_ =
        curl_easy_getinfo(curl, CURLINFO_HTTPAUTH_AVAIL, &authAvailable) ==
            CURLE_OK &&
        static_cast<unsigned long>(authAvailable) == CURLAUTH_BASIC;
    normalized.connection_stop_token = {};
    state_->commitConnection(
        std::make_shared<const SessionOptions>(std::move(normalized)),
        std::move(newEasySession));
    return true;
}

void CurlWebDavClient::disconnect() {
    state_->disconnect(defaultWebDavOptions());
}

void CurlWebDavClient::interrupt() {
    state_->requestInterrupt();
}

bool CurlWebDavClient::isConnected() const {
    return state_->isConnected();
}

bool CurlWebDavClient::list(const std::string &remote_path,
                            std::vector<FileInfo> &out, std::string &err) {
    clearLastOperationError();
    out.clear();
    auto operation =
        state_->beginConnectedOperation(err, "WebDAV", {remote_path});
    if (!operation) {
        setLastOperationError(operation.failure());
        return false;
    }
    const auto &connection = operation.connection();
    const SessionOptions &opt = *connection.options;

    const std::string basePath = normalizeRemotePath(remote_path);
    WebDavResponse response;
    CURLcode rc = CURLE_OK;
    if (!performPropfind(connection.session->get(), opt, basicAuthPinned_,
                         basePath, 1, operation.interrupted(), response, err,
                         &rc, true)) {
        if (rc == CURLE_ABORTED_BY_CALLBACK && operation.interrupted()->load())
            err = "Interrupted";
        setLastOperationError(
            curlcommon::errorFromCurl(rc, err, response.statusCode));
        return false;
    }
    if (isPathMissingStatus(response.statusCode)) {
        err.clear();
        setLastOperationError(RemoteErrorKind::NotFound,
                              "Remote path was not found.",
                              response.statusCode);
        return false;
    }
    if (!isSuccessStatus(response.statusCode)) {
        err = formatHttpFailure("WebDAV PROPFIND", response.statusCode);
        setLastOperationError(curlcommon::errorFromHttpStatus(
            response.statusCode, err, false, response.retryAfterSeconds));
        return false;
    }

    std::vector<curlparser::WebDavResource> resources;
    if (curlparser::parseWebDavPropfindResponse(opt, response.body, resources,
                                                err) !=
        curlparser::ListingParseStatus::Success) {
        setLastOperationError(RemoteErrorKind::Protocol, err);
        return false;
    }

    // Convert PROPFIND output to immediate children only.
    for (const curlparser::WebDavResource &r : resources) {
        std::string childName;
        if (!isDirectChildPath(basePath, r.path, childName))
            continue;
        FileInfo info{};
        info.name = childName;
        info.is_dir = r.isDir;
        if (r.hasSize) {
            info.has_size = true;
            info.size = r.size;
        }
        if (r.hasMtime)
            info.mtime = r.mtime;
        out.push_back(std::move(info));
    }

    // Case-insensitive order, ties broken by the exact name.
    const auto lowerByte = [](char c) {
        return std::tolower(static_cast<unsigned char>(c));
    };
    std::sort(
        out.begin(), out.end(), [&](const FileInfo &a, const FileInfo &b) {
            const auto [aEnd, bEnd] = std::mismatch(
                a.name.begin(), a.name.end(), b.name.begin(), b.name.end(),
                [&](char x, char y) { return lowerByte(x) == lowerByte(y); });
            if (aEnd != a.name.end() && bEnd != b.name.end())
                return lowerByte(*aEnd) < lowerByte(*bEnd);
            if (aEnd != a.name.end() || bEnd != b.name.end())
                return aEnd == a.name.end();
            return a.name < b.name;
        });
    return true;
}

bool CurlWebDavClient::get(
    const std::string &remote, const std::string &local, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel, bool resume) {
    clearLastOperationError();
    setLastDownloadModifiedTime(std::nullopt);
    if (resume) {
        err = "WebDAV backend does not support resume.";
        setLastOperationError(RemoteErrorKind::Unsupported, err);
        return false;
    }
    const std::string_view preflightError =
        normalizeRemotePath(remote) == "/" || local.empty()
            ? "WebDAV download requires a file path and local destination."
            : "";
    auto operation = state_->beginConnectedOperation(err, "WebDAV", {remote},
                                                     preflightError);
    if (!operation) {
        setLastOperationError(operation.failure());
        return false;
    }
    const auto &connection = operation.connection();
    const SessionOptions &opt = *connection.options;

    std::optional<std::uint32_t> retryAfter;
    const std::string url = buildWebDavUrl(opt, remote);
    // IIS sends a script such as default.aspx as a file, instead of running
    // it, only when asked this way, as Windows' own WebDAV client does.
    const std::unique_ptr<curl_slist, decltype(&curl_slist_free_all)>
        asFileHeader(curl_slist_append(nullptr, "Translate: f"),
                     &curl_slist_free_all);
    RemoteError failure;
    long rejectedStatus = 0;
    const auto download = [&] {
        rejectedStatus = 0;
        return curlcommon::downloadToLocalFile(
            connection.session->get(), local, opt.local_file_durability,
            progress, shouldCancel, operation.interrupted(), "WebDAV download",
            [&](CURL *curl, std::FILE *file,
                curlcommon::TransferProgressContext &progressContext,
                std::string &configurationError) {
                return configureCommonCurlHandle(curl, opt, basicAuthPinned_,
                                                 configurationError) &&
                       curlcommon::configureFileDownload(
                           curl, file, progressContext, configurationError) &&
                       curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) ==
                           CURLE_OK &&
                       curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L) ==
                           CURLE_OK &&
                       // A file arrives byte for byte, even a .gz that the
                       // server marks with Content-Encoding.
                       curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING,
                                        static_cast<char *>(nullptr)) ==
                           CURLE_OK &&
                       asFileHeader &&
                       curl_easy_setopt(curl, CURLOPT_HTTPHEADER,
                                        asFileHeader.get()) == CURLE_OK &&
                       // The time comes from Last-Modified.
                       curl_easy_setopt(curl, CURLOPT_FILETIME, 1L) ==
                           CURLE_OK &&
                       curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
                                        captureWebDavHeader) == CURLE_OK &&
                       curl_easy_setopt(curl, CURLOPT_HEADERDATA,
                                        &retryAfter) == CURLE_OK;
            },
            {},
            [&](long responseCode,
                std::string &responseError) -> std::optional<RemoteError> {
                if (curlcommon::isCompletedWebDavGetStatus(responseCode))
                    return std::nullopt;
                rejectedStatus = responseCode;
                responseError = formatHttpFailure("WebDAV GET", responseCode);
                return curlcommon::errorFromHttpStatus(
                    responseCode, responseError, false, retryAfter);
            },
            failure, err);
    };
    bool downloaded = download();
    if (!downloaded &&
        unpinBasicAuthAfterRejection(connection.session->get(),
                                     basicAuthPinned_, rejectedStatus)) {
        err.clear();
        downloaded = download();
    }
    if (!downloaded) {
        setLastOperationError(failure);
        return false;
    }
    curl_off_t fileTime = -1;
    if (curl_easy_getinfo(connection.session->get(), CURLINFO_FILETIME_T,
                          &fileTime) == CURLE_OK &&
        fileTime > 0) {
        setLastDownloadModifiedTime(static_cast<std::uint64_t>(fileTime));
    }
    return true;
}

bool CurlWebDavClient::put(
    const std::string &local, const std::string &remote, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel, bool resume) {
    clearLastOperationError();
    if (resume) {
        err = "WebDAV backend does not support resume.";
        setLastOperationError(RemoteErrorKind::Unsupported, err);
        return false;
    }
    const std::string_view preflightError =
        normalizeRemotePath(remote) == "/" || local.empty()
            ? "WebDAV upload requires a local file and remote file path."
            : "";
    auto operation = state_->beginConnectedOperation(err, "WebDAV", {remote},
                                                     preflightError);
    if (!operation) {
        setLastOperationError(operation.failure());
        return false;
    }
    const auto &connection = operation.connection();
    const SessionOptions &opt = *connection.options;

    const std::string remotePartial = normalizeRemotePath(remote) + ".part";
    curlcommon::ActiveDestinationLease destinationLease(
        webDavDestinationKey(opt, remote));
    curlcommon::ActiveDestinationLease partialLease(
        webDavDestinationKey(opt, remotePartial));
    if (!destinationLease.acquired() || !partialLease.acquired()) {
        err = "Another transfer is already using this remote destination.";
        setLastOperationError(RemoteErrorKind::Conflict, err);
        return false;
    }

    std::optional<std::uint32_t> retryAfter;
    std::string responseBody;
    curlcommon::BoundedStringSink responseSink{&responseBody};
    const std::string uploadUrl = buildWebDavUrl(opt, remotePartial);
    RemoteError failure;
    long rejectedStatus = 0;
    const auto upload = [&] {
        rejectedStatus = 0;
        responseBody.clear();
        return curlcommon::uploadFromLocalFile(
            connection.session->get(), local, progress, shouldCancel,
            operation.interrupted(), "WebDAV upload",
            [&](CURL *curl, std::FILE *file, curl_off_t fileSize,
                curlcommon::TransferProgressContext &progressContext,
                std::string &configurationError) {
                return configureCommonCurlHandle(curl, opt, basicAuthPinned_,
                                                 configurationError) &&
                       curlcommon::configureFileUpload(curl, file, fileSize,
                                                       progressContext,
                                                       configurationError) &&
                       curl_easy_setopt(curl, CURLOPT_URL, uploadUrl.c_str()) ==
                           CURLE_OK &&
                       curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT") ==
                           CURLE_OK &&
                       curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                                        curlcommon::appendStringCallback) ==
                           CURLE_OK &&
                       curl_easy_setopt(curl, CURLOPT_WRITEDATA,
                                        &responseSink) == CURLE_OK &&
                       curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
                                        captureWebDavHeader) == CURLE_OK &&
                       curl_easy_setopt(curl, CURLOPT_HEADERDATA,
                                        &retryAfter) == CURLE_OK;
            },
            {},
            [&](long responseCode,
                std::string &responseError) -> std::optional<RemoteError> {
                if (curlcommon::isCompletedWebDavWriteStatus(responseCode))
                    return std::nullopt;
                rejectedStatus = responseCode;
                responseError = formatHttpFailure("WebDAV PUT", responseCode);
                return curlcommon::errorFromHttpStatus(
                    responseCode, responseError, false, retryAfter);
            },
            failure, err);
    };
    bool uploaded = upload();
    if (!uploaded &&
        unpinBasicAuthAfterRejection(connection.session->get(),
                                     basicAuthPinned_, rejectedStatus)) {
        err.clear();
        uploaded = upload();
    }
    if (!uploaded) {
        setLastOperationError(failure);
        return false;
    }

    // MOVE with Overwrite: T first deletes whatever is at the destination, a
    // whole collection included (RFC 4918 9.9.3). Replace only a file, and do
    // not overwrite when nothing was there.
    WebDavResourceProbe destinationProbe;
    if (!probeWebDavResource(connection.session->get(), opt, basicAuthPinned_,
                             remote, 0, operation.interrupted(),
                             destinationProbe, failure)) {
        err = failure.message;
        setLastOperationError(failure);
        return false;
    }
    if (destinationProbe.kind == WebDavResourceKind::Collection) {
        err = "WebDAV upload cannot replace a collection.";
        setLastOperationError(RemoteErrorKind::Conflict, err);
        return false;
    }
    const std::string destination = buildWebDavUrl(opt, remote);
    std::vector<std::string> headers = {
        "Destination: " + destination,
        destinationProbe.kind == WebDavResourceKind::File ? "Overwrite: T"
                                                          : "Overwrite: F",
    };
    WebDavResponse moveResponse;
    CURLcode moveCode = CURLE_OK;
    if (!performTextRequest(connection.session->get(), opt, basicAuthPinned_,
                            WebDavTextRequest{"MOVE", remotePartial, nullptr,
                                              std::move(headers)},
                            operation.interrupted(), moveResponse, err,
                            &moveCode)) {
        setLastOperationError(curlcommon::errorFromCurl(
            moveCode, err, moveResponse.statusCode, true));
        return false;
    }
    if (!curlcommon::isCompletedWebDavWriteStatus(moveResponse.statusCode)) {
        err = formatHttpFailure("WebDAV MOVE", moveResponse.statusCode);
        setLastOperationError(webDavMutationStatusError(
            moveResponse.statusCode, err, moveResponse.retryAfterSeconds));
        return false;
    }
    return true;
}

bool CurlWebDavClient::exists(const std::string &remote_path, bool &isDir,
                              std::string &err) {
    err.clear();
    FileInfo info{};
    const bool ok = stat(remote_path, info, err);
    if (ok) {
        isDir = info.is_dir;
        return true;
    }
    if (err.empty()) {
        isDir = false;
        return false;
    }
    return false;
}

bool CurlWebDavClient::stat(const std::string &remote_path, FileInfo &info,
                            std::string &err) {
    clearLastOperationError();
    info = FileInfo{};
    auto operation =
        state_->beginConnectedOperation(err, "WebDAV", {remote_path});
    if (!operation) {
        setLastOperationError(operation.failure());
        return false;
    }
    const auto &connection = operation.connection();
    const SessionOptions &opt = *connection.options;

    const std::string target = normalizeRemotePath(remote_path);
    WebDavResponse response;
    CURLcode rc = CURLE_OK;
    if (!performPropfind(connection.session->get(), opt, basicAuthPinned_,
                         target, 0, operation.interrupted(), response, err,
                         &rc)) {
        if (rc == CURLE_ABORTED_BY_CALLBACK && operation.interrupted()->load())
            err = "Interrupted";
        setLastOperationError(
            curlcommon::errorFromCurl(rc, err, response.statusCode));
        return false;
    }
    if (isPathMissingStatus(response.statusCode)) {
        err.clear();
        setLastOperationError(RemoteErrorKind::NotFound,
                              "Remote path was not found.",
                              response.statusCode);
        return false;
    }
    if (!isSuccessStatus(response.statusCode)) {
        err = formatHttpFailure("WebDAV PROPFIND", response.statusCode);
        setLastOperationError(curlcommon::errorFromHttpStatus(
            response.statusCode, err, false, response.retryAfterSeconds));
        return false;
    }

    std::vector<curlparser::WebDavResource> resources;
    if (curlparser::parseWebDavPropfindResponse(opt, response.body, resources,
                                                err) !=
        curlparser::ListingParseStatus::Success) {
        setLastOperationError(RemoteErrorKind::Protocol, err);
        return false;
    }

    // Some servers return only one resource; accept it as target fallback.
    auto it = std::find_if(resources.begin(), resources.end(),
                           [&target](const curlparser::WebDavResource &r) {
                               return r.path == target;
                           });
    if (it == resources.end()) {
        if (resources.size() == 1) {
            it = resources.begin();
        } else {
            err.clear();
            return false;
        }
    }

    info.name = (target == "/") ? std::string("/")
                                : target.substr(target.find_last_of('/') + 1);
    info.is_dir = it->isDir;
    if (it->hasSize) {
        info.has_size = true;
        info.size = it->size;
    }
    if (it->hasMtime)
        info.mtime = it->mtime;
    return true;
}

bool CurlWebDavClient::chmod(const std::string &remote_path, std::uint32_t mode,
                             std::string &err) {
    clearLastOperationError();
    (void)remote_path;
    (void)mode;
    const bool ok = unsupportedWebDavOperation("chmod", err);
    setLastOperationError(RemoteErrorKind::Unsupported, err);
    return ok;
}

bool CurlWebDavClient::chown(const std::string &remote_path, std::uint32_t uid,
                             std::uint32_t gid, std::string &err) {
    clearLastOperationError();
    (void)remote_path;
    (void)uid;
    (void)gid;
    const bool ok = unsupportedWebDavOperation("chown", err);
    setLastOperationError(RemoteErrorKind::Unsupported, err);
    return ok;
}

bool CurlWebDavClient::setTimes(const std::string &remote_path,
                                std::uint64_t atime, std::uint64_t mtime,
                                std::string &err) {
    clearLastOperationError();
    (void)remote_path;
    (void)atime;
    (void)mtime;
    const bool ok = unsupportedWebDavOperation("timestamp updates", err);
    setLastOperationError(RemoteErrorKind::Unsupported, err);
    return ok;
}

bool CurlWebDavClient::mkdir(const std::string &remote_dir, std::string &err,
                             unsigned int mode) {
    clearLastOperationError();
    (void)mode;
    auto operation =
        state_->beginConnectedOperation(err, "WebDAV", {remote_dir});
    if (!operation) {
        setLastOperationError(operation.failure());
        return false;
    }
    const auto &connection = operation.connection();
    const SessionOptions &opt = *connection.options;
    WebDavResponse response;
    CURLcode rc = CURLE_OK;
    if (!performTextRequest(connection.session->get(), opt, basicAuthPinned_,
                            WebDavTextRequest{"MKCOL", remote_dir, nullptr, {}},
                            operation.interrupted(), response, err, &rc)) {
        setLastOperationError(
            curlcommon::errorFromCurl(rc, err, response.statusCode, true));
        return false;
    }
    if (curlcommon::isCompletedWebDavWriteStatus(response.statusCode))
        return true;

    // RFC 4918 permits 405 when the resource already exists, but it can also
    // mean MKCOL is unsupported. Only accept it after proving the target is an
    // existing collection.
    if (response.statusCode == 405) {
        WebDavResponse verification;
        CURLcode verificationCode = CURLE_OK;
        if (!performPropfind(connection.session->get(), opt, basicAuthPinned_,
                             remote_dir, 0, operation.interrupted(),
                             verification, err, &verificationCode)) {
            setLastOperationError(curlcommon::errorFromCurl(
                verificationCode, err, verification.statusCode));
            return false;
        }
        if (isSuccessStatus(verification.statusCode)) {
            std::vector<curlparser::WebDavResource> resources;
            if (curlparser::parseWebDavPropfindResponse(opt, verification.body,
                                                        resources, err) !=
                curlparser::ListingParseStatus::Success) {
                setLastOperationError(RemoteErrorKind::Protocol, err);
                return false;
            }
            const std::string target = normalizeRemotePath(remote_dir);
            auto it = std::find_if(
                resources.begin(), resources.end(),
                [&target](const curlparser::WebDavResource &resource) {
                    return resource.path == target;
                });
            if (it != resources.end()) {
                if (it->isDir)
                    return true;
                err = "WebDAV MKCOL target already exists as a file.";
                setLastOperationError(RemoteErrorKind::Conflict, err, 405);
                return false;
            }
        } else if (!isPathMissingStatus(verification.statusCode)) {
            err = formatHttpFailure("WebDAV MKCOL verification",
                                    verification.statusCode);
            setLastOperationError(curlcommon::errorFromHttpStatus(
                verification.statusCode, err, false,
                verification.retryAfterSeconds));
            return false;
        }
    }
    err = formatHttpFailure("WebDAV MKCOL", response.statusCode);
    setLastOperationError(webDavMutationStatusError(
        response.statusCode, err, response.retryAfterSeconds));
    return false;
}

bool CurlWebDavClient::removeFile(const std::string &remote_path,
                                  std::string &err) {
    return removeResource(remote_path, false, err);
}

bool CurlWebDavClient::removeDir(const std::string &remote_dir,
                                 std::string &err) {
    return removeResource(remote_dir, true, err);
}

bool CurlWebDavClient::removeResource(const std::string &remote_path,
                                      bool collection, std::string &err) {
    clearLastOperationError();
    const bool targetsRoot = normalizeRemotePath(remote_path) == "/";
    auto operation = state_->beginConnectedOperation(
        err, "WebDAV", {remote_path},
        targetsRoot ? "Refusing to delete the WebDAV base collection." : "");
    if (!operation) {
        setLastOperationError(operation.failure());
        return false;
    }
    const auto &connection = operation.connection();
    const SessionOptions &opt = *connection.options;

    WebDavResourceProbe probe;
    RemoteError failure;
    if (!probeWebDavResource(connection.session->get(), opt, basicAuthPinned_,
                             remote_path, collection ? 1 : 0,
                             operation.interrupted(), probe, failure)) {
        err = failure.message;
        setLastOperationError(failure);
        return false;
    }
    if (probe.kind == WebDavResourceKind::Missing) {
        err = formatHttpFailure("WebDAV PROPFIND", 404);
        setLastOperationError(curlcommon::errorFromHttpStatus(404, err, false));
        return false;
    }
    if (!collection && probe.kind == WebDavResourceKind::Collection) {
        err = "WebDAV path is a collection, not a file.";
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    if (collection && probe.kind == WebDavResourceKind::File) {
        err = "WebDAV path is a file, not a collection.";
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    if (collection && probe.childCount > 0) {
        err = "WebDAV collection is not empty.";
        setLastOperationError(RemoteErrorKind::Conflict, err);
        return false;
    }

    WebDavResponse response;
    CURLcode rc = CURLE_OK;
    if (!performTextRequest(
            connection.session->get(), opt, basicAuthPinned_,
            WebDavTextRequest{"DELETE", remote_path, nullptr, {}, collection},
            operation.interrupted(), response, err, &rc)) {
        setLastOperationError(
            curlcommon::errorFromCurl(rc, err, response.statusCode, true));
        return false;
    }
    if (response.statusCode == 200 || response.statusCode == 204)
        return true;
    err = formatHttpFailure("WebDAV DELETE", response.statusCode);
    setLastOperationError(webDavMutationStatusError(
        response.statusCode, err, response.retryAfterSeconds));
    return false;
}

bool CurlWebDavClient::rename(const std::string &from, const std::string &to,
                              std::string &err, bool overwrite) {
    clearLastOperationError();
    const bool targetsRoot =
        normalizeRemotePath(from) == "/" || normalizeRemotePath(to) == "/";
    auto operation = state_->beginConnectedOperation(
        err, "WebDAV", {from, to},
        targetsRoot ? "Refusing to rename the WebDAV base collection." : "");
    if (!operation) {
        setLastOperationError(operation.failure());
        return false;
    }
    const auto &connection = operation.connection();
    const SessionOptions &opt = *connection.options;

    // Overwrite: T would let the server delete a whole collection at the
    // destination, so only a file may be replaced, and only by a file.
    bool replaceDestination = false;
    if (overwrite) {
        WebDavResourceProbe destinationProbe;
        WebDavResourceProbe sourceProbe;
        RemoteError failure;
        if (!probeWebDavResource(
                connection.session->get(), opt, basicAuthPinned_, to, 0,
                operation.interrupted(), destinationProbe, failure) ||
            (destinationProbe.kind == WebDavResourceKind::File &&
             !probeWebDavResource(
                 connection.session->get(), opt, basicAuthPinned_, from, 0,
                 operation.interrupted(), sourceProbe, failure))) {
            err = failure.message;
            setLastOperationError(failure);
            return false;
        }
        if (destinationProbe.kind == WebDavResourceKind::Collection ||
            sourceProbe.kind == WebDavResourceKind::Collection) {
            err = "WebDAV rename can only replace a file with a file.";
            setLastOperationError(RemoteErrorKind::Conflict, err);
            return false;
        }
        replaceDestination = destinationProbe.kind == WebDavResourceKind::File;
    }
    const std::string destination = buildWebDavUrl(opt, to);
    // RFC 4918 MOVE requires absolute Destination URL.
    std::vector<std::string> headers = {
        "Destination: " + destination,
        std::string("Overwrite: ") + (replaceDestination ? "T" : "F"),
    };
    WebDavResponse response;
    CURLcode rc = CURLE_OK;
    if (!performTextRequest(
            connection.session->get(), opt, basicAuthPinned_,
            WebDavTextRequest{"MOVE", from, nullptr, std::move(headers)},
            operation.interrupted(), response, err, &rc)) {
        setLastOperationError(
            curlcommon::errorFromCurl(rc, err, response.statusCode, true));
        return false;
    }
    if (response.statusCode == 200 || response.statusCode == 201 ||
        response.statusCode == 204) {
        return true;
    }
    err = formatHttpFailure("WebDAV MOVE", response.statusCode);
    setLastOperationError(webDavMutationStatusError(
        response.statusCode, err, response.retryAfterSeconds));
    return false;
}

} // namespace openscp
