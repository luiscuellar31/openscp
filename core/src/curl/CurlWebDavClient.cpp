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
#include <mutex>
#include <sstream>
#include <string>
#include <vector>

namespace openscp {
namespace {

struct WebDavResponse {
    long statusCode = 0;
    std::string body;
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

std::string normalizeRemotePath(std::string path) {
    if (path.empty())
        return "/";
    for (char &c : path) {
        if (c == '\\')
            c = '/';
    }
    if (path.front() != '/')
        path.insert(path.begin(), '/');
    while (path.find("//") != std::string::npos)
        path.replace(path.find("//"), 2, "/");
    if (path.size() > 1 && path.back() == '/')
        path.pop_back();
    return path;
}

std::string normalizeRemoteDirPath(std::string path) {
    path = normalizeRemotePath(std::move(path));
    if (path != "/" && !path.empty() && path.back() != '/')
        path.push_back('/');
    return path;
}

bool isUnreservedUriChar(unsigned char c) {
    return std::isalnum(c) != 0 || c == '-' || c == '.' || c == '_' ||
           c == '~' || c == '/';
}

std::string encodePathForUrl(const std::string &path) {
    std::ostringstream out;
    out.setf(std::ios::uppercase);
    for (unsigned char c : path) {
        if (isUnreservedUriChar(c)) {
            out << static_cast<char>(c);
        } else {
            const char hex[] = "0123456789ABCDEF";
            out << '%' << hex[(c >> 4) & 0x0F] << hex[c & 0x0F];
        }
    }
    return out.str();
}

std::string buildWebDavUrl(const SessionOptions &opt,
                           const std::string &remotePath) {
    const std::string host = curlcommon::normalizeHostAuthorityForUrl(opt.host);
    const std::string path = encodePathForUrl(normalizeRemotePath(remotePath));
    return std::string(webDavSchemeStorageName(
               normalizeWebDavScheme(opt.webdav_scheme))) +
           "://" + host + ":" + std::to_string(opt.port) + path;
}

std::string formatHttpFailure(const char *what, long statusCode) {
    std::ostringstream out;
    out << (what ? what : "WebDAV operation")
        << " failed with HTTP status " << statusCode << ".";
    return out.str();
}

bool configureCommonCurlHandle(CURL *curl, const SessionOptions &opt,
                               std::string &err) {
    if (!curl) {
        err = "Could not create CURL handle.";
        return false;
    }
    if (curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_TIMEOUT, 120L) != CURLE_OK ||
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

bool performTextRequest(const SessionOptions &opt, const std::string &method,
                        const std::string &remotePath,
                        const std::string *requestBody,
                        const std::vector<std::string> &headers,
                        WebDavResponse &response, std::string &err) {
    response = WebDavResponse{};
    CURL *curl = curl_easy_init();
    if (!curl) {
        err = "Could not create CURL handle.";
        return false;
    }
    if (!configureCommonCurlHandle(curl, opt, err)) {
        curl_easy_cleanup(curl);
        return false;
    }

    const std::string url = buildWebDavUrl(opt, remotePath);
    struct curl_slist *headerList = nullptr;
    for (const std::string &h : headers)
        headerList = curl_slist_append(headerList, h.c_str());

    const bool configured =
        (curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_CUSTOMREQUEST, method.c_str()) ==
         CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                          curlcommon::appendStringCallback) ==
         CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_WRITEDATA, &response.body) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_HTTPHEADER, headerList) == CURLE_OK);
    if (!configured) {
        err = std::string("Could not configure WebDAV ") + method + " request.";
        curl_slist_free_all(headerList);
        curl_easy_cleanup(curl);
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
            curl_easy_cleanup(curl);
            return false;
        }
    }

    const CURLcode rc = curl_easy_perform(curl);
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &response.statusCode);
    curl_slist_free_all(headerList);
    curl_easy_cleanup(curl);

    if (rc != CURLE_OK) {
        err = std::string("WebDAV ") + method +
              " failed: " + curl_easy_strerror(rc);
        return false;
    }
    return true;
}

bool performDownloadRequest(const SessionOptions &opt, const std::string &remote,
                            std::FILE *localFile,
                            curlcommon::TransferProgressContext &progressContext,
                            std::string &err, long &statusCodeOut,
                            CURLcode &rcOut) {
    statusCodeOut = 0;
    rcOut = CURLE_OK;
    CURL *curl = curl_easy_init();
    if (!curl) {
        err = "Could not create CURL handle.";
        return false;
    }
    if (!configureCommonCurlHandle(curl, opt, err)) {
        curl_easy_cleanup(curl);
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
        (curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                          curlcommon::transferProgressCallback) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressContext) ==
         CURLE_OK);
    if (!configured) {
        err = "Could not configure WebDAV download.";
        curl_easy_cleanup(curl);
        return false;
    }
    const CURLcode rc = curl_easy_perform(curl);
    rcOut = rc;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCodeOut);
    curl_easy_cleanup(curl);
    if (rc != CURLE_OK) {
        err = std::string("WebDAV download failed: ") + curl_easy_strerror(rc);
        return false;
    }
    return true;
}

bool performUploadRequest(const SessionOptions &opt, const std::string &remote,
                          std::FILE *localFile, curl_off_t fileSize,
                          curlcommon::TransferProgressContext &progressContext,
                          std::string &err,
                          long &statusCodeOut, CURLcode &rcOut) {
    statusCodeOut = 0;
    rcOut = CURLE_OK;
    CURL *curl = curl_easy_init();
    if (!curl) {
        err = "Could not create CURL handle.";
        return false;
    }
    if (!configureCommonCurlHandle(curl, opt, err)) {
        curl_easy_cleanup(curl);
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
        (curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                          curlcommon::transferProgressCallback) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressContext) ==
         CURLE_OK);
    if (!configured) {
        err = "Could not configure WebDAV upload.";
        curl_easy_cleanup(curl);
        return false;
    }
    const CURLcode rc = curl_easy_perform(curl);
    rcOut = rc;
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &statusCodeOut);
    curl_easy_cleanup(curl);
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

bool parsePropfindResponse(const std::string &xml,
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
        parsed.path = normalizeRemotePath(
            decodePercent(extractPathFromHref(hrefRaw)));
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

bool performPropfind(const SessionOptions &opt, const std::string &remotePath,
                     int depth, WebDavResponse &response, std::string &err) {
    const std::string body = propfindBody();
    std::vector<std::string> headers = {
        "Depth: " + std::to_string(depth),
        "Content-Type: application/xml; charset=utf-8",
    };
    return performTextRequest(opt, "PROPFIND", remotePath, &body, headers,
                              response, err);
}

bool unsupportedWebDavOperation(const char *what, std::string &err) {
    err = std::string("WebDAV backend does not support ") + what + ".";
    return false;
}

} // namespace

bool CurlWebDavClient::connect(const SessionOptions &opt, std::string &err) {
    err.clear();
    interrupted_.store(false);
    if (opt.host.empty()) {
        err = "Host is required.";
        return false;
    }
    if (opt.protocol != Protocol::WebDav) {
        err = "CurlWebDavClient only supports WebDAV protocol.";
        return false;
    }
    if (opt.jump_host && !opt.jump_host->empty()) {
        err = "WebDAV backend does not support SSH jump host.";
        return false;
    }
    if (!ensureCurlInitialized(err))
        return false;

    SessionOptions normalized = opt;
    normalized.webdav_scheme = normalizeWebDavScheme(normalized.webdav_scheme);
    if (normalized.port == 0)
        normalized.port = defaultPortForWebDavScheme(normalized.webdav_scheme);
    normalized.protocol = Protocol::WebDav;

    WebDavResponse probe;
    if (!performPropfind(normalized, "/", 0, probe, err))
        return false;
    if (!isSuccessStatus(probe.statusCode)) {
        err = formatHttpFailure("WebDAV connect probe", probe.statusCode);
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        options_ = normalized;
        connected_ = true;
    }
    return true;
}

void CurlWebDavClient::disconnect() {
    interrupted_.store(false);
    std::lock_guard<std::mutex> lk(stateMutex_);
    connected_ = false;
    options_ = SessionOptions{};
    options_.protocol = Protocol::WebDav;
    options_.webdav_scheme = WebDavScheme::Https;
    options_.port = defaultPortForWebDavScheme(options_.webdav_scheme);
}

void CurlWebDavClient::interrupt() { interrupted_.store(true); }

bool CurlWebDavClient::isConnected() const {
    std::lock_guard<std::mutex> lk(stateMutex_);
    return connected_;
}

bool CurlWebDavClient::list(const std::string &remote_path,
                            std::vector<FileInfo> &out, std::string &err) {
    err.clear();
    out.clear();
    SessionOptions opt;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            return false;
        }
        opt = options_;
    }

    if (!ensureCurlInitialized(err))
        return false;

    const std::string basePath = normalizeRemotePath(remote_path);
    WebDavResponse response;
    if (!performPropfind(opt, basePath, 1, response, err))
        return false;
    if (isPathMissingStatus(response.statusCode)) {
        err.clear();
        return false;
    }
    if (!isSuccessStatus(response.statusCode)) {
        err = formatHttpFailure("WebDAV PROPFIND", response.statusCode);
        return false;
    }

    std::vector<WebDavResource> resources;
    if (!parsePropfindResponse(response.body, resources, err))
        return false;

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
    err.clear();
    if (resume) {
        err = "WebDAV backend does not support resume.";
        return false;
    }
    SessionOptions opt;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            return false;
        }
        opt = options_;
    }
    interrupted_.store(false);
    if (!ensureCurlInitialized(err))
        return false;

    std::FILE *localFile = std::fopen(local.c_str(), "wb");
    if (!localFile) {
        err = "Could not open local file for writing.";
        return false;
    }

    curlcommon::TransferProgressContext progressContext{
        progress, shouldCancel, &interrupted_, true};
    long statusCode = 0;
    CURLcode rc = CURLE_OK;
    const bool ok = performDownloadRequest(opt, remote, localFile,
                                           progressContext, err, statusCode, rc);
    std::fclose(localFile);
    if (!ok) {
        if (rc == CURLE_ABORTED_BY_CALLBACK) {
            if (shouldCancel && shouldCancel())
                err = "Canceled by user";
            else if (interrupted_.load())
                err = "Interrupted";
        }
        return false;
    }
    if (!isSuccessStatus(statusCode)) {
        err = formatHttpFailure("WebDAV GET", statusCode);
        return false;
    }
    return true;
}

bool CurlWebDavClient::put(
    const std::string &local, const std::string &remote, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel, bool resume) {
    err.clear();
    if (resume) {
        err = "WebDAV backend does not support resume.";
        return false;
    }
    SessionOptions opt;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            return false;
        }
        opt = options_;
    }
    interrupted_.store(false);
    if (!ensureCurlInitialized(err))
        return false;

    std::uint64_t total = 0;
    try {
        total = std::filesystem::file_size(local);
    } catch (...) {
        err = "Could not determine local file size.";
        return false;
    }

    std::FILE *localFile = std::fopen(local.c_str(), "rb");
    if (!localFile) {
        err = "Could not open local file for reading.";
        return false;
    }

    curlcommon::TransferProgressContext progressContext{
        progress, shouldCancel, &interrupted_, true};
    long statusCode = 0;
    CURLcode rc = CURLE_OK;
    const bool ok = performUploadRequest(
        opt, remote, localFile, static_cast<curl_off_t>(total),
        progressContext, err,
        statusCode, rc);
    std::fclose(localFile);
    if (!ok) {
        if (rc == CURLE_ABORTED_BY_CALLBACK) {
            if (shouldCancel && shouldCancel())
                err = "Canceled by user";
            else if (interrupted_.load())
                err = "Interrupted";
        }
        return false;
    }
    if (!(statusCode == 200 || statusCode == 201 || statusCode == 204)) {
        err = formatHttpFailure("WebDAV PUT", statusCode);
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
    err.clear();
    info = FileInfo{};
    SessionOptions opt;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            return false;
        }
        opt = options_;
    }

    if (!ensureCurlInitialized(err))
        return false;

    const std::string target = normalizeRemotePath(remote_path);
    WebDavResponse response;
    if (!performPropfind(opt, target, 0, response, err))
        return false;
    if (isPathMissingStatus(response.statusCode)) {
        err.clear();
        return false;
    }
    if (!isSuccessStatus(response.statusCode)) {
        err = formatHttpFailure("WebDAV PROPFIND", response.statusCode);
        return false;
    }

    std::vector<WebDavResource> resources;
    if (!parsePropfindResponse(response.body, resources, err))
        return false;

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
    (void)remote_path;
    (void)mode;
    return unsupportedWebDavOperation("chmod", err);
}

bool CurlWebDavClient::chown(const std::string &remote_path, std::uint32_t uid,
                             std::uint32_t gid, std::string &err) {
    (void)remote_path;
    (void)uid;
    (void)gid;
    return unsupportedWebDavOperation("chown", err);
}

bool CurlWebDavClient::setTimes(const std::string &remote_path,
                                std::uint64_t atime, std::uint64_t mtime,
                                std::string &err) {
    (void)remote_path;
    (void)atime;
    (void)mtime;
    return unsupportedWebDavOperation("timestamp updates", err);
}

bool CurlWebDavClient::mkdir(const std::string &remote_dir, std::string &err,
                             unsigned int mode) {
    (void)mode;
    err.clear();
    SessionOptions opt;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            return false;
        }
        opt = options_;
    }
    WebDavResponse response;
    if (!performTextRequest(opt, "MKCOL", remote_dir, nullptr, {}, response, err))
        return false;
    if (response.statusCode == 200 || response.statusCode == 201 ||
        response.statusCode == 204 || response.statusCode == 405) {
        return true;
    }
    err = formatHttpFailure("WebDAV MKCOL", response.statusCode);
    return false;
}

bool CurlWebDavClient::removeFile(const std::string &remote_path,
                                  std::string &err) {
    err.clear();
    SessionOptions opt;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            return false;
        }
        opt = options_;
    }
    WebDavResponse response;
    if (!performTextRequest(opt, "DELETE", remote_path, nullptr, {}, response, err))
        return false;
    if (response.statusCode == 200 || response.statusCode == 204)
        return true;
    err = formatHttpFailure("WebDAV DELETE", response.statusCode);
    return false;
}

bool CurlWebDavClient::removeDir(const std::string &remote_dir,
                                 std::string &err) {
    return removeFile(remote_dir, err);
}

bool CurlWebDavClient::rename(const std::string &from, const std::string &to,
                              std::string &err, bool overwrite) {
    err.clear();
    SessionOptions opt;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (!connected_) {
            err = "Not connected.";
            return false;
        }
        opt = options_;
    }
    const std::string destination = buildWebDavUrl(opt, to);
    std::vector<std::string> headers = {
        "Destination: " + destination,
        std::string("Overwrite: ") + (overwrite ? "T" : "F"),
    };
    WebDavResponse response;
    if (!performTextRequest(opt, "MOVE", from, nullptr, headers, response, err))
        return false;
    if (response.statusCode == 200 || response.statusCode == 201 ||
        response.statusCode == 204) {
        return true;
    }
    err = formatHttpFailure("WebDAV MOVE", response.statusCode);
    return false;
}

std::unique_ptr<SftpClient>
CurlWebDavClient::newConnectionLike(const SessionOptions &opt, std::string &err) {
    auto ptr = std::make_unique<CurlWebDavClient>();
    SessionOptions normalized = opt;
    normalized.protocol = Protocol::WebDav;
    if (!ptr->connect(normalized, err))
        return nullptr;
    return ptr;
}

} // namespace openscp
