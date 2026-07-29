// WebDAV backend implementation based on libcurl and tinyxml2.
#include "openscp/CurlWebDavClient.hpp"

#include "CurlBackendCommon.hpp"

#include <curl/curl.h>
#include <tinyxml2.h>

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
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

struct WebDavResource {
    std::string path;
    bool isDir = false;
    bool hasSize = false;
    std::uint64_t size = 0;
    bool hasMtime = false;
    std::uint64_t mtime = 0;
};

using curlcommon::ensureCurlInitialized;
using curlcommon::parseUnsignedDec;
using curlcommon::toLowerAscii;
using curlcommon::trimAscii;

class InterruptResetGuard {
    public:
    explicit InterruptResetGuard(std::atomic<bool> &interrupted)
        : interrupted_(interrupted) {}
    ~InterruptResetGuard() { interrupted_.store(false); }

    private:
    std::atomic<bool> &interrupted_;
};

bool rejectInterruptedBeforePerform(
    const std::atomic<bool> *interrupted, std::string &err,
    CURLcode *curlCodeOut = nullptr) {
    if (!interrupted || !interrupted->load())
        return false;
    if (curlCodeOut)
        *curlCodeOut = CURLE_ABORTED_BY_CALLBACK;
    err = "Interrupted";
    return true;
}

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

std::string normalizeRemotePath(std::string path) {
    // Canonicalize to absolute POSIX-like form used by WebDAV operations.
    for (char &c : path) {
        if (c == '\\')
            c = '/';
    }
    std::vector<std::string> segments;
    std::size_t pos = 0;
    while (pos <= path.size()) {
        const std::size_t slash = path.find('/', pos);
        const std::string segment =
            path.substr(pos, slash == std::string::npos ? std::string::npos
                                                        : slash - pos);
        if (!segment.empty() && segment != ".") {
            if (segment == "..") {
                if (!segments.empty())
                    segments.pop_back();
            } else {
                segments.push_back(segment);
            }
        }
        if (slash == std::string::npos)
            break;
        pos = slash + 1;
    }
    std::string normalized = "/";
    for (std::size_t i = 0; i < segments.size(); ++i) {
        if (i != 0)
            normalized.push_back('/');
        normalized += segments[i];
    }
    return normalized;
}

std::string normalizeRemoteDirPath(std::string path) {
    path = normalizeRemotePath(std::move(path));
    if (path != "/" && !path.empty() && path.back() != '/')
        path.push_back('/');
    return path;
}

std::string serverPathForLogicalPath(const SessionOptions &opt,
                                     const std::string &remotePath) {
    const std::string base = normalizeWebDavBasePath(opt.webdav_base_path);
    const std::string logical = normalizeRemotePath(remotePath);
    if (base == "/")
        return logical;
    if (logical == "/")
        return base;
    return base + logical;
}

bool logicalPathForServerPath(const SessionOptions &opt,
                              const std::string &serverPath,
                              std::string &logicalPath) {
    const std::string base = normalizeWebDavBasePath(opt.webdav_base_path);
    const std::string normalizedServer = normalizeRemotePath(serverPath);
    if (base == "/") {
        logicalPath = normalizedServer;
        return true;
    }
    if (normalizedServer == base) {
        logicalPath = "/";
        return true;
    }
    const std::string prefix = base + "/";
    if (normalizedServer.rfind(prefix, 0) != 0)
        return false;
    logicalPath = normalizeRemotePath(normalizedServer.substr(base.size()));
    return true;
}

std::string buildWebDavUrl(const SessionOptions &opt,
                           const std::string &remotePath) {
    const std::string host = curlcommon::normalizeHostAuthorityForUrl(opt.host);
    const std::string path =
        curlcommon::encodeUrlPath(serverPathForLogicalPath(opt, remotePath));
    return std::string(webDavSchemeStorageName(
               normalizeWebDavScheme(opt.webdav_scheme))) +
           "://" + host + ":" + std::to_string(opt.port) + path;
}

std::string webDavDestinationKey(const SessionOptions &opt,
                                 const std::string &remotePath) {
    return std::string("remote:webdav:") +
           webDavSchemeStorageName(normalizeWebDavScheme(opt.webdav_scheme)) +
           ":" + toLowerAscii(opt.host) + ":" + std::to_string(opt.port) +
           ":" + opt.username + ":" +
           normalizeWebDavBasePath(opt.webdav_base_path) + ":" +
           normalizeRemotePath(remotePath);
}

std::string formatHttpFailure(const char *what, long statusCode) {
    std::ostringstream out;
    out << (what ? what : "WebDAV operation")
        << " failed with HTTP status " << statusCode << ".";
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
    if (!curl) {
        err = "Could not create CURL handle.";
        return false;
    }
    if (curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "") != CURLE_OK) {
        err = "Could not configure WebDAV client timeouts.";
        return false;
    }

    if (!opt.username.empty()) {
        if (curl_easy_setopt(curl, CURLOPT_USERNAME, opt.username.c_str()) !=
                CURLE_OK ||
            curl_easy_setopt(curl, CURLOPT_HTTPAUTH, CURLAUTH_ANY) != CURLE_OK) {
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
                        const std::string &method,
                        const std::string &remotePath,
                        const std::string *requestBody,
                        const std::vector<std::string> &headers,
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

    const std::string url = buildWebDavUrl(opt, remotePath);
    struct curl_slist *headerList = nullptr;
    for (const std::string &h : headers)
        headerList = curl_slist_append(headerList, h.c_str());

    curlcommon::TransferProgressContext cancelContext{
        {}, {}, interrupted, false};
    const bool configured =
        (curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str()) ==
         CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                          curlcommon::appendStringCallback) ==
         CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
                          captureWebDavHeader) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_HEADERDATA,
                          &response.retryAfterSeconds) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                          curlcommon::transferProgressCallback) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &cancelContext) ==
         CURLE_OK);
    if (!configured) {
        err = std::string("Could not configure WebDAV ") + method + " request.";
        curl_slist_free_all(headerList);
        return false;
    }

    if (requestBody) {
        if (curl_easy_setopt(curl, CURLOPT_POSTFIELDS, requestBody->c_str()) !=
                CURLE_OK ||
            curl_easy_setopt(curl, CURLOPT_POSTFIELDSIZE,
                             static_cast<long>(requestBody->size())) != CURLE_OK) {
            err = std::string("Could not configure WebDAV request body for ") +
                  method + ".";
            curl_slist_free_all(headerList);
            return false;
        }
    }

    if (rejectInterruptedBeforePerform(interrupted, err, curlCodeOut)) {
        curl_slist_free_all(headerList);
        return false;
    }
    const CURLcode rc = curl_easy_perform(curl);
    if (curlCodeOut)
        *curlCodeOut = rc;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
    curl_slist_free_all(headerList);

    if (rc != CURLE_OK) {
        err = std::string("WebDAV ") + method +
              " failed: " + curl_easy_strerror(rc);
        return false;
    }
    return true;
}

bool performDownloadRequest(CURL *curl, const SessionOptions &opt,
                            const std::string &remote,
                            std::FILE *localFile,
                            curlcommon::TransferProgressContext &progressContext,
                            std::string &err, long &statusCodeOut,
                            CURLcode &rcOut,
                            std::optional<std::uint32_t> &retryAfterOut) {
    statusCodeOut = 0;
    rcOut = CURLE_OK;
    retryAfterOut.reset();
    curl_easy_reset(curl);
    if (!configureCommonCurlHandle(curl, opt, err)) {
        return false;
    }
    const std::string url = buildWebDavUrl(opt, remote);
    const bool configured =
        (curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_HTTPGET, 1L) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                          curlcommon::writeFileCallback) ==
         CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_WRITEDATA, localFile) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
                          captureWebDavHeader) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_HEADERDATA, &retryAfterOut) ==
         CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                          curlcommon::transferProgressCallback) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressContext) ==
         CURLE_OK);
    if (!configured) {
        err = "Could not configure WebDAV download.";
        return false;
    }
    if (rejectInterruptedBeforePerform(progressContext.interrupted, err,
                                      &rcOut)) {
        return false;
    }
    const CURLcode rc = curl_easy_perform(curl);
    rcOut = rc;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCodeOut);
    if (progressContext.interrupted &&
        progressContext.interrupted->load()) {
        rcOut = CURLE_ABORTED_BY_CALLBACK;
        err = "Interrupted";
        return false;
    }
    if (rc != CURLE_OK) {
        err = std::string("WebDAV download failed: ") + curl_easy_strerror(rc);
        return false;
    }
    return true;
}

bool performUploadRequest(CURL *curl, const SessionOptions &opt,
                          const std::string &remote,
                          std::FILE *localFile, curl_off_t fileSize,
                          curlcommon::TransferProgressContext &progressContext,
                          std::string &err,
                          long &statusCodeOut, CURLcode &rcOut,
                          std::optional<std::uint32_t> &retryAfterOut) {
    statusCodeOut = 0;
    rcOut = CURLE_OK;
    retryAfterOut.reset();
    curl_easy_reset(curl);
    if (!configureCommonCurlHandle(curl, opt, err)) {
        return false;
    }
    const std::string url = buildWebDavUrl(opt, remote);
    const bool configured =
        (curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, "PUT") == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_READFUNCTION,
                          curlcommon::readFileCallback) ==
         CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_READDATA, localFile) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, fileSize) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_HEADERFUNCTION,
                          captureWebDavHeader) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_HEADERDATA, &retryAfterOut) ==
         CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                          curlcommon::transferProgressCallback) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressContext) ==
         CURLE_OK);
    if (!configured) {
        err = "Could not configure WebDAV upload.";
        return false;
    }
    if (rejectInterruptedBeforePerform(progressContext.interrupted, err,
                                      &rcOut)) {
        return false;
    }
    const CURLcode rc = curl_easy_perform(curl);
    rcOut = rc;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCodeOut);
    if (progressContext.interrupted &&
        progressContext.interrupted->load()) {
        rcOut = CURLE_ABORTED_BY_CALLBACK;
        err = "Interrupted";
        return false;
    }
    if (rc != CURLE_OK) {
        err = std::string("WebDAV upload failed: ") + curl_easy_strerror(rc);
        return false;
    }
    return true;
}

const char *xmlLocalName(const char *name) {
    if (!name)
        return "";
    const char *colon = std::strchr(name, ':');
    return colon ? (colon + 1) : name;
}

bool xmlNameEquals(const tinyxml2::XMLElement *elem, const char *local) {
    return elem && std::strcmp(xmlLocalName(elem->Name()), local) == 0;
}

const tinyxml2::XMLElement *firstChildByLocal(const tinyxml2::XMLElement *parent,
                                              const char *local) {
    if (!parent || !local)
        return nullptr;
    for (const tinyxml2::XMLElement *child = parent->FirstChildElement(); child;
         child = child->NextSiblingElement()) {
        if (xmlNameEquals(child, local))
            return child;
    }
    return nullptr;
}

int parseHttpStatusCode(const std::string &statusLine) {
    for (std::size_t i = 0; i + 2 < statusLine.size(); ++i) {
        const unsigned char a = static_cast<unsigned char>(statusLine[i]);
        const unsigned char b = static_cast<unsigned char>(statusLine[i + 1]);
        const unsigned char c = static_cast<unsigned char>(statusLine[i + 2]);
        if (std::isdigit(a) && std::isdigit(b) && std::isdigit(c)) {
            return int((a - '0') * 100 + (b - '0') * 10 + (c - '0'));
        }
    }
    return 0;
}

std::string extractPathFromHref(std::string href) {
    href = trimAscii(std::move(href));
    if (href.empty())
        return "/";
    const std::size_t hashPos = href.find('#');
    if (hashPos != std::string::npos)
        href.erase(hashPos);
    const std::size_t queryPos = href.find('?');
    if (queryPos != std::string::npos)
        href.erase(queryPos);
    const std::size_t schemePos = href.find("://");
    if (schemePos != std::string::npos) {
        const std::size_t pathPos = href.find('/', schemePos + 3);
        if (pathPos == std::string::npos)
            return "/";
        return href.substr(pathPos);
    }
    return href;
}

std::string decodePercent(std::string raw) {
    int decodedLen = 0;
    char *decoded = curl_easy_unescape(nullptr, raw.c_str(),
                                       static_cast<int>(raw.size()), &decodedLen);
    if (!decoded)
        return raw;
    std::string out(decoded, static_cast<std::size_t>(decodedLen));
    curl_free(decoded);
    return out;
}

void parsePropElement(const tinyxml2::XMLElement *prop, WebDavResource &out) {
    if (!prop)
        return;
    // Parse a minimal DAV property set used by listing/stat.
    if (const tinyxml2::XMLElement *resType =
            firstChildByLocal(prop, "resourcetype")) {
        for (const tinyxml2::XMLElement *child = resType->FirstChildElement();
             child; child = child->NextSiblingElement()) {
            if (xmlNameEquals(child, "collection")) {
                out.isDir = true;
                break;
            }
        }
    }
    if (const tinyxml2::XMLElement *lenEl =
            firstChildByLocal(prop, "getcontentlength")) {
        if (const char *text = lenEl->GetText()) {
            std::uint64_t value = 0;
            if (parseUnsignedDec(trimAscii(text), value)) {
                out.hasSize = true;
                out.size = value;
            }
        }
    }
    if (const tinyxml2::XMLElement *mtimeEl =
            firstChildByLocal(prop, "getlastmodified")) {
        if (const char *text = mtimeEl->GetText()) {
            const std::time_t tt = curl_getdate(text, nullptr);
            if (tt >= 0) {
                out.hasMtime = true;
                out.mtime = static_cast<std::uint64_t>(tt);
            }
        }
    }
}

bool parsePropfindResponse(const SessionOptions &opt, const std::string &xml,
                           std::vector<WebDavResource> &resources,
                           std::string &err) {
    resources.clear();
    tinyxml2::XMLDocument doc;
    const tinyxml2::XMLError parseErr = doc.Parse(xml.c_str(), xml.size());
    if (parseErr != tinyxml2::XML_SUCCESS) {
        std::ostringstream out;
        out << "Could not parse WebDAV PROPFIND response (XML error "
            << static_cast<int>(parseErr) << ").";
        err = out.str();
        return false;
    }
    const tinyxml2::XMLElement *root = doc.RootElement();
    if (!root) {
        err = "WebDAV PROPFIND response is empty.";
        return false;
    }

    std::vector<const tinyxml2::XMLElement *> stack;
    stack.push_back(root);

    // Walk the whole XML tree so namespace/prefix variations are tolerated.
    while (!stack.empty()) {
        const tinyxml2::XMLElement *elem = stack.back();
        stack.pop_back();

        for (const tinyxml2::XMLElement *child = elem->FirstChildElement(); child;
             child = child->NextSiblingElement()) {
            stack.push_back(child);
        }

        if (!xmlNameEquals(elem, "response"))
            continue;

        const tinyxml2::XMLElement *hrefEl = firstChildByLocal(elem, "href");
        const char *hrefTxt = hrefEl ? hrefEl->GetText() : nullptr;
        if (!hrefTxt || !*hrefTxt)
            continue;

        const std::string hrefRaw(hrefTxt);
        WebDavResource parsed;
        const std::string serverPath = normalizeRemotePath(
            decodePercent(extractPathFromHref(hrefRaw)));
        if (!logicalPathForServerPath(opt, serverPath, parsed.path))
            continue;
        if (hrefRaw.back() == '/')
            parsed.isDir = true;

        bool consumedPropStat = false;
        for (const tinyxml2::XMLElement *propStat = elem->FirstChildElement();
             propStat; propStat = propStat->NextSiblingElement()) {
            if (!xmlNameEquals(propStat, "propstat"))
                continue;
            const tinyxml2::XMLElement *statusEl =
                firstChildByLocal(propStat, "status");
            const char *statusText = statusEl ? statusEl->GetText() : nullptr;
            const int statusCode =
                statusText ? parseHttpStatusCode(statusText) : 0;
            if (statusCode < 200 || statusCode >= 300)
                continue;
            const tinyxml2::XMLElement *prop = firstChildByLocal(propStat, "prop");
            parsePropElement(prop, parsed);
            consumedPropStat = true;
        }

        if (!consumedPropStat) {
            const tinyxml2::XMLElement *prop = firstChildByLocal(elem, "prop");
            parsePropElement(prop, parsed);
        }

        auto it = std::find_if(resources.begin(), resources.end(),
                               [&parsed](const WebDavResource &r) {
                                   return r.path == parsed.path;
                               });
        if (it == resources.end()) {
            resources.push_back(parsed);
        } else {
            it->isDir = it->isDir || parsed.isDir;
            if (!it->hasSize && parsed.hasSize) {
                it->hasSize = true;
                it->size = parsed.size;
            }
            if (!it->hasMtime && parsed.hasMtime) {
                it->hasMtime = true;
                it->mtime = parsed.mtime;
            }
        }
    }

    if (resources.empty()) {
        err = "WebDAV PROPFIND response does not contain usable resources.";
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

bool isSuccessStatus(long status) { return status >= 200 && status < 300; }

bool isPathMissingStatus(long status) { return status == 404; }

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
    const std::string body = propfindBody();
    std::vector<std::string> headers = {
        "Depth: " + std::to_string(depth),
        "Content-Type: application/xml; charset=utf-8",
    };
    return performTextRequest(curl, opt, "PROPFIND", remotePath, &body, headers,
                              interrupted, response, err, curlCodeOut);
}

bool unsupportedWebDavOperation(const char *what, std::string &err) {
    err = std::string("WebDAV backend does not support ") + what + ".";
    return false;
}

} // namespace

CurlWebDavClient::~CurlWebDavClient() { disconnect(); }

bool CurlWebDavClient::connect(const SessionOptions &opt, std::string &err) {
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    InterruptResetGuard interruptReset(interrupted_);
    clearLastOperationError();
    err.clear();
    interrupted_.store(false);
    if (disconnecting_.load()) {
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
    if (!curlcommon::validateRemotePath(opt.webdav_base_path,
                                        "WebDAV base", err)) {
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

    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        connected_ = false;
    }
    if (easyHandle_) {
        curl_easy_cleanup(static_cast<CURL *>(easyHandle_));
        easyHandle_ = nullptr;
    }
    CURL *curl = curl_easy_init();
    if (!curl) {
        err = "Could not create CURL handle.";
        setLastOperationError(RemoteErrorKind::LocalIo, err);
        return false;
    }
    WebDavResponse probe;
    CURLcode probeCode = CURLE_OK;
    if (!performPropfind(curl, normalized, "/", 0, &interrupted_, probe, err,
                         &probeCode)) {
        setLastOperationError(
            curlcommon::errorFromCurl(probeCode, err, probe.statusCode));
        curl_easy_cleanup(curl);
        return false;
    }
    if (!isSuccessStatus(probe.statusCode)) {
        err = formatHttpFailure("WebDAV connect probe", probe.statusCode);
        setLastOperationError(curlcommon::errorFromHttpStatus(
            probe.statusCode, err, false, probe.retryAfterSeconds));
        curl_easy_cleanup(curl);
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        options_ = normalized;
        connected_ = true;
    }
    easyHandle_ = curl;
    return true;
}

void CurlWebDavClient::disconnect() {
    disconnecting_.store(true);
    interrupted_.store(true);
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    if (easyHandle_) {
        curl_easy_cleanup(static_cast<CURL *>(easyHandle_));
        easyHandle_ = nullptr;
    }
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        connected_ = false;
        options_ = SessionOptions{};
        options_.protocol = Protocol::WebDav;
        options_.webdav_scheme = WebDavScheme::Https;
        options_.webdav_base_path = "/";
        options_.port = defaultPortForWebDavScheme(options_.webdav_scheme);
    }
    interrupted_.store(false);
    disconnecting_.store(false);
}

void CurlWebDavClient::interrupt() { interrupted_.store(true); }

bool CurlWebDavClient::isConnected() const {
    std::lock_guard<std::mutex> lk(stateMutex_);
    return connected_;
}

bool CurlWebDavClient::list(const std::string &remote_path,
                            std::vector<FileInfo> &out, std::string &err) {
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    InterruptResetGuard interruptReset(interrupted_);
    clearLastOperationError();
    err.clear();
    out.clear();
    interrupted_.store(false);
    if (disconnecting_.load()) {
        err = "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err);
        return false;
    }
    if (!curlcommon::validateRemotePath(remote_path, "WebDAV", err)) {
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    SessionOptions opt;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            setLastOperationError(RemoteErrorKind::Connection, err);
            return false;
        }
        opt = options_;
    }

    if (!ensureCurlInitialized(err)) {
        setLastOperationError(RemoteErrorKind::LocalIo, err);
        return false;
    }

    const std::string basePath = normalizeRemotePath(remote_path);
    WebDavResponse response;
    CURLcode rc = CURLE_OK;
    if (!performPropfind(static_cast<CURL *>(easyHandle_), opt, basePath, 1,
                         &interrupted_, response, err, &rc)) {
        if (rc == CURLE_ABORTED_BY_CALLBACK && interrupted_.load())
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

    std::vector<WebDavResource> resources;
    if (!parsePropfindResponse(opt, response.body, resources, err)) {
        setLastOperationError(RemoteErrorKind::Protocol, err);
        return false;
    }

    // Convert PROPFIND output to immediate children only.
    for (const WebDavResource &r : resources) {
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

    std::sort(out.begin(), out.end(),
              [](const FileInfo &a, const FileInfo &b) {
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
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    InterruptResetGuard interruptReset(interrupted_);
    clearLastOperationError();
    err.clear();
    interrupted_.store(false);
    if (disconnecting_.load()) {
        err = "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err);
        return false;
    }
    if (resume) {
        err = "WebDAV backend does not support resume.";
        setLastOperationError(RemoteErrorKind::Unsupported, err);
        return false;
    }
    if (!curlcommon::validateRemotePath(remote, "WebDAV", err)) {
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    if (normalizeRemotePath(remote) == "/" || local.empty()) {
        err = "WebDAV download requires a file path and local destination.";
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    SessionOptions opt;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            setLastOperationError(RemoteErrorKind::Connection, err);
            return false;
        }
        opt = options_;
    }
    if (!ensureCurlInitialized(err)) {
        setLastOperationError(RemoteErrorKind::LocalIo, err);
        return false;
    }

    const std::string partial = curlcommon::localPartialPath(local);
    curlcommon::ActiveDestinationLease destinationLease(
        curlcommon::localDestinationKey(local));
    curlcommon::ActiveDestinationLease partialLease(
        curlcommon::localDestinationKey(partial));
    if (!destinationLease.acquired() || !partialLease.acquired()) {
        err = "Another transfer is already using this local destination.";
        setLastOperationError(RemoteErrorKind::Conflict, err);
        return false;
    }
    std::FILE *localFile = std::fopen(partial.c_str(), "wb");
    if (!localFile) {
        err = "Could not open local partial file for writing.";
        setLastOperationError(RemoteErrorKind::LocalIo, err, errno);
        return false;
    }

    curlcommon::TransferProgressContext progressContext{
        progress, shouldCancel, &interrupted_, false};
    const bool canceledBeforeTransfer = shouldCancel && shouldCancel();
    if (canceledBeforeTransfer || interrupted_.load()) {
        std::fclose(localFile);
        err = canceledBeforeTransfer ? "Canceled by user" : "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err,
                              static_cast<std::int64_t>(
                                  CURLE_ABORTED_BY_CALLBACK));
        return false;
    }
    long statusCode = 0;
    CURLcode rc = CURLE_OK;
    std::optional<std::uint32_t> retryAfter;
    const bool ok = performDownloadRequest(
        static_cast<CURL *>(easyHandle_), opt, remote, localFile,
        progressContext, err, statusCode, rc, retryAfter);
    const bool userCanceled = shouldCancel && shouldCancel();
    if (!ok || userCanceled || interrupted_.load()) {
        std::fclose(localFile);
        if (userCanceled || interrupted_.load()) {
            err = userCanceled ? "Canceled by user" : "Interrupted";
            setLastOperationError(RemoteErrorKind::Canceled, err,
                                  static_cast<std::int64_t>(
                                      CURLE_ABORTED_BY_CALLBACK));
        } else {
            setLastOperationError(
                curlcommon::errorFromCurl(rc, err, statusCode));
        }
        return false;
    }
    if (!curlcommon::isCompletedWebDavGetStatus(statusCode)) {
        std::fclose(localFile);
        err = formatHttpFailure("WebDAV GET", statusCode);
        setLastOperationError(
            curlcommon::errorFromHttpStatus(statusCode, err, false,
                                            retryAfter));
        return false;
    }
    if (!curlcommon::flushAndSyncFile(localFile, err)) {
        std::fclose(localFile);
        setLastOperationError(RemoteErrorKind::LocalIo, err, errno);
        return false;
    }
    if (std::fclose(localFile) != 0) {
        err = "Could not close local partial file after download.";
        setLastOperationError(RemoteErrorKind::LocalIo, err, errno);
        return false;
    }
    const bool canceledBeforeCommit = shouldCancel && shouldCancel();
    if (canceledBeforeCommit || interrupted_.load()) {
        err = canceledBeforeCommit ? "Canceled by user" : "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err,
                              static_cast<std::int64_t>(
                                  CURLE_ABORTED_BY_CALLBACK));
        return false;
    }
    if (!curlcommon::atomicReplaceLocalFile(partial, local, err)) {
        setLastOperationError(RemoteErrorKind::LocalIo, err, errno);
        return false;
    }
    return true;
}

bool CurlWebDavClient::put(
    const std::string &local, const std::string &remote, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel, bool resume) {
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    InterruptResetGuard interruptReset(interrupted_);
    clearLastOperationError();
    err.clear();
    interrupted_.store(false);
    if (disconnecting_.load()) {
        err = "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err);
        return false;
    }
    if (resume) {
        err = "WebDAV backend does not support resume.";
        setLastOperationError(RemoteErrorKind::Unsupported, err);
        return false;
    }
    if (!curlcommon::validateRemotePath(remote, "WebDAV", err)) {
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    if (normalizeRemotePath(remote) == "/" || local.empty()) {
        err = "WebDAV upload requires a local file and remote file path.";
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    SessionOptions opt;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            setLastOperationError(RemoteErrorKind::Connection, err);
            return false;
        }
        opt = options_;
    }
    if (!ensureCurlInitialized(err)) {
        setLastOperationError(RemoteErrorKind::LocalIo, err);
        return false;
    }

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

    std::uint64_t total = 0;
    try {
        total = std::filesystem::file_size(local);
    } catch (...) {
        err = "Could not determine local file size.";
        setLastOperationError(RemoteErrorKind::LocalIo, err);
        return false;
    }

    std::FILE *localFile = std::fopen(local.c_str(), "rb");
    if (!localFile) {
        err = "Could not open local file for reading.";
        setLastOperationError(RemoteErrorKind::LocalIo, err, errno);
        return false;
    }

    curlcommon::TransferProgressContext progressContext{
        progress, shouldCancel, &interrupted_, true};
    const bool canceledBeforeTransfer = shouldCancel && shouldCancel();
    if (canceledBeforeTransfer || interrupted_.load()) {
        std::fclose(localFile);
        err = canceledBeforeTransfer ? "Canceled by user" : "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err,
                              static_cast<std::int64_t>(
                                  CURLE_ABORTED_BY_CALLBACK));
        return false;
    }
    long statusCode = 0;
    CURLcode rc = CURLE_OK;
    std::optional<std::uint32_t> retryAfter;
    const bool ok = performUploadRequest(
        static_cast<CURL *>(easyHandle_), opt, remotePartial, localFile,
        static_cast<curl_off_t>(total),
        progressContext, err,
        statusCode, rc, retryAfter);
    std::fclose(localFile);
    const bool userCanceled = shouldCancel && shouldCancel();
    if (!ok || userCanceled || interrupted_.load()) {
        if (userCanceled || interrupted_.load()) {
            err = userCanceled ? "Canceled by user" : "Interrupted";
            setLastOperationError(RemoteErrorKind::Canceled, err,
                                  static_cast<std::int64_t>(
                                      CURLE_ABORTED_BY_CALLBACK));
        } else {
            setLastOperationError(
                curlcommon::errorFromCurl(rc, err, statusCode));
        }
        return false;
    }
    if (!curlcommon::isCompletedWebDavWriteStatus(statusCode)) {
        err = formatHttpFailure("WebDAV PUT", statusCode);
        setLastOperationError(
            curlcommon::errorFromHttpStatus(statusCode, err, false,
                                            retryAfter));
        return false;
    }

    const bool canceledBeforeCommit = shouldCancel && shouldCancel();
    if (canceledBeforeCommit || interrupted_.load()) {
        err = canceledBeforeCommit ? "Canceled by user" : "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err,
                              static_cast<std::int64_t>(
                                  CURLE_ABORTED_BY_CALLBACK));
        return false;
    }

    const std::string destination = buildWebDavUrl(opt, remote);
    std::vector<std::string> headers = {
        "Destination: " + destination,
        "Overwrite: T",
    };
    WebDavResponse moveResponse;
    CURLcode moveCode = CURLE_OK;
    if (!performTextRequest(static_cast<CURL *>(easyHandle_), opt, "MOVE",
                            remotePartial, nullptr, headers, &interrupted_,
                            moveResponse, err, &moveCode)) {
        setLastOperationError(curlcommon::errorFromCurl(
            moveCode, err, moveResponse.statusCode, true));
        return false;
    }
    if (!curlcommon::isCompletedWebDavWriteStatus(
            moveResponse.statusCode)) {
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
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    InterruptResetGuard interruptReset(interrupted_);
    clearLastOperationError();
    err.clear();
    info = FileInfo{};
    interrupted_.store(false);
    if (disconnecting_.load()) {
        err = "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err);
        return false;
    }
    if (!curlcommon::validateRemotePath(remote_path, "WebDAV", err)) {
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    SessionOptions opt;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            setLastOperationError(RemoteErrorKind::Connection, err);
            return false;
        }
        opt = options_;
    }

    if (!ensureCurlInitialized(err)) {
        setLastOperationError(RemoteErrorKind::LocalIo, err);
        return false;
    }

    const std::string target = normalizeRemotePath(remote_path);
    WebDavResponse response;
    CURLcode rc = CURLE_OK;
    if (!performPropfind(static_cast<CURL *>(easyHandle_), opt, target, 0,
                         &interrupted_, response, err, &rc)) {
        if (rc == CURLE_ABORTED_BY_CALLBACK && interrupted_.load())
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

    std::vector<WebDavResource> resources;
    if (!parsePropfindResponse(opt, response.body, resources, err)) {
        setLastOperationError(RemoteErrorKind::Protocol, err);
        return false;
    }

    // Some servers return only one resource; accept it as target fallback.
    auto it = std::find_if(resources.begin(), resources.end(),
                           [&target](const WebDavResource &r) {
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

    info.name = (target == "/")
                    ? std::string("/")
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
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    InterruptResetGuard interruptReset(interrupted_);
    clearLastOperationError();
    (void)mode;
    err.clear();
    interrupted_.store(false);
    if (disconnecting_.load()) {
        err = "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err);
        return false;
    }
    if (!curlcommon::validateRemotePath(remote_dir, "WebDAV", err)) {
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    SessionOptions opt;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            setLastOperationError(RemoteErrorKind::Connection, err);
            return false;
        }
        opt = options_;
    }
    WebDavResponse response;
    CURLcode rc = CURLE_OK;
    if (!performTextRequest(static_cast<CURL *>(easyHandle_), opt, "MKCOL",
                            remote_dir, nullptr, {}, &interrupted_, response,
                            err, &rc)) {
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
        if (!performPropfind(static_cast<CURL *>(easyHandle_), opt, remote_dir,
                             0, &interrupted_, verification, err,
                             &verificationCode)) {
            setLastOperationError(curlcommon::errorFromCurl(
                verificationCode, err, verification.statusCode));
            return false;
        }
        if (isSuccessStatus(verification.statusCode)) {
            std::vector<WebDavResource> resources;
            if (!parsePropfindResponse(opt, verification.body, resources,
                                       err)) {
                setLastOperationError(RemoteErrorKind::Protocol, err);
                return false;
            }
            const std::string target = normalizeRemotePath(remote_dir);
            auto it = std::find_if(
                resources.begin(), resources.end(),
                [&target](const WebDavResource &resource) {
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
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    InterruptResetGuard interruptReset(interrupted_);
    clearLastOperationError();
    err.clear();
    interrupted_.store(false);
    if (disconnecting_.load()) {
        err = "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err);
        return false;
    }
    if (!curlcommon::validateRemotePath(remote_path, "WebDAV", err)) {
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    if (normalizeRemotePath(remote_path) == "/") {
        err = "Refusing to delete the WebDAV base collection.";
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    SessionOptions opt;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            setLastOperationError(RemoteErrorKind::Connection, err);
            return false;
        }
        opt = options_;
    }
    WebDavResponse response;
    CURLcode rc = CURLE_OK;
    if (!performTextRequest(static_cast<CURL *>(easyHandle_), opt, "DELETE",
                            remote_path, nullptr, {}, &interrupted_, response,
                            err, &rc)) {
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
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    InterruptResetGuard interruptReset(interrupted_);
    clearLastOperationError();
    err.clear();
    interrupted_.store(false);
    if (disconnecting_.load()) {
        err = "Interrupted";
        setLastOperationError(RemoteErrorKind::Canceled, err);
        return false;
    }
    if (!curlcommon::validateRemotePath(from, "WebDAV", err) ||
        !curlcommon::validateRemotePath(to, "WebDAV", err)) {
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    if (normalizeRemotePath(from) == "/" ||
        normalizeRemotePath(to) == "/") {
        err = "Refusing to rename the WebDAV base collection.";
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    SessionOptions opt;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            setLastOperationError(RemoteErrorKind::Connection, err);
            return false;
        }
        opt = options_;
    }
    const std::string destination = buildWebDavUrl(opt, to);
    // RFC 4918 MOVE requires absolute Destination URL.
    std::vector<std::string> headers = {
        "Destination: " + destination,
        std::string("Overwrite: ") + (overwrite ? "T" : "F"),
    };
    WebDavResponse response;
    CURLcode rc = CURLE_OK;
    if (!performTextRequest(static_cast<CURL *>(easyHandle_), opt, "MOVE", from,
                            nullptr, headers, &interrupted_, response, err,
                            &rc)) {
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
CurlWebDavClient::newConnectionLike(const SessionOptions &opt, std::string &err) {
    auto ptr = std::make_unique<CurlWebDavClient>();
    SessionOptions normalized = opt;
    normalized.protocol = Protocol::WebDav;
    if (!ptr->connect(normalized, err))
        return nullptr;
    return ptr;
}

} // namespace openscp
