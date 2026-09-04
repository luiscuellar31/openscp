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
    std::string line(ptr, total);
    const std::string lowered = toLowerAscii(line);
    static constexpr std::string_view prefix = "retry-after:";
    if (lowered.rfind(prefix, 0) == 0) {
        const std::string value = trimAscii(line.substr(prefix.size()));
        auto *retryAfter =
            static_cast<std::optional<std::uint32_t> *>(userdata);
        *retryAfter = curlcommon::parseRetryAfter(value);
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
                           const std::string &remotePath) {
    const std::string host = curlcommon::normalizeHostAuthorityForUrl(opt.host);
    const std::string path = curlcommon::encodeUrlPath(
        curlcommon::webDavServerPath(opt.webdav_base_path, remotePath));
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
                               std::string &err) {
    if (!curlcommon::configureBaseCurlHandle(curl, "WebDAV", true, std::nullopt,
                                             err))
        return false;

    if (!opt.username.empty()) {
        if (curl_easy_setopt(curl, CURLOPT_USERNAME, opt.username.c_str()) !=
                CURLE_OK ||
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY) !=
                CURLE_OK) {
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

    if (normalizeWebDavScheme(opt.webdav_scheme) == WebDavScheme::Https) {
        if (!curlcommon::configureTlsVerification(
                curl, opt.webdav_verify_peer, opt.webdav_ca_cert_path,
                "Could not configure WebDAV TLS verification policy.",
                "Could not configure WebDAV TLS CA bundle.", err)) {
            return false;
        }
    }

    return curlcommon::configureProxy(curl, opt, "WebDAV", "WebDAV", err);
}

bool performTextRequest(CURL *curl, const SessionOptions &opt,
                        const WebDavTextRequest &request,
                        const std::atomic<bool> *interrupted,
                        WebDavResponse &response, std::string &err,
                        CURLcode *curlCodeOut = nullptr) {
    // Generic request helper for WebDAV verbs with text/XML payloads.
    response = WebDavResponse{};
    if (curlCodeOut)
        *curlCodeOut = CURLE_OK;
    curl_easy_reset(curl);
    if (!configureCommonCurlHandle(curl, opt, err)) {
        return false;
    }

    const std::string url = buildWebDavUrl(opt, request.remotePath);
    struct curl_slist *headerList = nullptr;
    for (const std::string &h : request.headers)
        headerList = curl_slist_append(headerList, h.c_str());

    curlcommon::TransferProgressContext cancelContext{
        {}, {}, interrupted, false};
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
    const CURLcode rc = curl_easy_perform(curl);
    if (curlCodeOut)
        *curlCodeOut = rc;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
    curl_slist_free_all(headerList);

    if (responseSink.limitExceeded || responseSink.allocationFailed) {
        if (curlCodeOut)
            *curlCodeOut = CURLE_WRITE_ERROR;
        err = responseSink.limitExceeded
                  ? std::string("WebDAV ") + request.method +
                        " response exceeded the 64 MiB safety limit."
                  : std::string("WebDAV ") + request.method +
                        " response could not be stored in memory.";
        return false;
    }
    if (rc != CURLE_OK) {
        err = std::string("WebDAV ") + request.method +
              " failed: " + curl_easy_strerror(rc);
        return false;
    }
    return true;
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
                     const std::string &remotePath, int depth,
                     const std::atomic<bool> *interrupted,
                     WebDavResponse &response, std::string &err,
                     CURLcode *curlCodeOut = nullptr) {
    // PROPFIND drives both stat(depth=0) and list(depth=1).
    const std::string &body = propfindBody();
    std::vector<std::string> headers = {
        "Depth: " + std::to_string(depth),
        "Content-Type: application/xml; charset=utf-8",
    };
    return performTextRequest(
        curl, opt,
        WebDavTextRequest{"PROPFIND", remotePath, &body, std::move(headers)},
        interrupted, response, err, curlCodeOut);
}

bool unsupportedWebDavOperation(const char *what, std::string &err) {
    err = std::string("WebDAV backend does not support ") + what + ".";
    return false;
}

} // namespace

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
    if (!performPropfind(curl, normalized, "/", 0, operation.interrupted(),
                         probe, err, &probeCode)) {
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
    if (!performPropfind(connection.session->get(), opt, basePath, 1,
                         operation.interrupted(), response, err, &rc)) {
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

    std::sort(out.begin(), out.end(), [](const FileInfo &a, const FileInfo &b) {
        const std::string al = toLowerAscii(a.name);
        const std::string bl = toLowerAscii(b.name);
        if (al == bl)
            return a.name < b.name;
        return al < bl;
    });
    return true;
}

bool CurlWebDavClient::get(
    const std::string &remote, const std::string &local, std::string &err,
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
    RemoteError failure;
    if (!curlcommon::downloadToLocalFile(
            connection.session->get(), local, std::move(progress),
            std::move(shouldCancel), operation.interrupted(), "WebDAV download",
            [&](CURL *curl, std::FILE *file,
                curlcommon::TransferProgressContext &progressContext,
                std::string &configurationError) {
                return configureCommonCurlHandle(curl, opt,
                                                 configurationError) &&
                       curlcommon::configureFileDownload(
                           curl, file, progressContext, configurationError) &&
                       curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) ==
                           CURLE_OK &&
                       curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L) ==
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
                responseError = formatHttpFailure("WebDAV GET", responseCode);
                return curlcommon::errorFromHttpStatus(
                    responseCode, responseError, false, retryAfter);
            },
            failure, err)) {
        setLastOperationError(failure);
        return false;
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
    if (!curlcommon::uploadFromLocalFile(
            connection.session->get(), local, std::move(progress),
            std::move(shouldCancel), operation.interrupted(), "WebDAV upload",
            [&](CURL *curl, std::FILE *file, curl_off_t fileSize,
                curlcommon::TransferProgressContext &progressContext,
                std::string &configurationError) {
                return configureCommonCurlHandle(curl, opt,
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
                responseError = formatHttpFailure("WebDAV PUT", responseCode);
                return curlcommon::errorFromHttpStatus(
                    responseCode, responseError, false, retryAfter);
            },
            failure, err)) {
        setLastOperationError(failure);
        return false;
    }

    const std::string destination = buildWebDavUrl(opt, remote);
    std::vector<std::string> headers = {
        "Destination: " + destination,
        "Overwrite: T",
    };
    WebDavResponse moveResponse;
    CURLcode moveCode = CURLE_OK;
    if (!performTextRequest(connection.session->get(), opt,
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
    if (!performPropfind(connection.session->get(), opt, target, 0,
                         operation.interrupted(), response, err, &rc)) {
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
    if (!performTextRequest(connection.session->get(), opt,
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
        if (!performPropfind(connection.session->get(), opt, remote_dir, 0,
                             operation.interrupted(), verification, err,
                             &verificationCode)) {
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
    WebDavResponse response;
    CURLcode rc = CURLE_OK;
    if (!performTextRequest(
            connection.session->get(), opt,
            WebDavTextRequest{"DELETE", remote_path, nullptr, {}},
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

bool CurlWebDavClient::removeDir(const std::string &remote_dir,
                                 std::string &err) {
    return removeFile(remote_dir, err);
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
    const std::string destination = buildWebDavUrl(opt, to);
    // RFC 4918 MOVE requires absolute Destination URL.
    std::vector<std::string> headers = {
        "Destination: " + destination,
        std::string("Overwrite: ") + (overwrite ? "T" : "F"),
    };
    WebDavResponse response;
    CURLcode rc = CURLE_OK;
    if (!performTextRequest(
            connection.session->get(), opt,
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

std::unique_ptr<RemoteClient>
CurlWebDavClient::newConnectionLike(const SessionOptions &opt,
                                    std::string &err) {
    auto ptr = std::make_unique<CurlWebDavClient>();
    SessionOptions normalized = opt;
    normalized.protocol = Protocol::WebDav;
    if (!ptr->connect(normalized, err))
        return nullptr;
    return ptr;
}

} // namespace openscp
