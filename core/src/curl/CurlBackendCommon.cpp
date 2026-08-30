// Shared internal helpers for libcurl-based backends (FTP/WebDAV).
#include "CurlBackendCommon.hpp"

#include "../common/SafeLocalFile.hpp"
#include "openscp/RemotePath.hpp"

#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <limits>
#include <mutex>
#include <string>
#include <unordered_set>
#include <utility>

#ifdef _WIN32
#include <io.h>
#else
#include <unistd.h>
#endif

namespace openscp::curlcommon {
namespace {

std::mutex activeDestinationMutex;
std::unordered_set<std::string> activeDestinations;

} // namespace

bool ensureCurlInitialized(std::string &err) {
    static std::once_flag initFlag;
    static CURLcode initResult = CURLE_OK;
    // libcurl global init is process-wide and must run exactly once.
    std::call_once(initFlag,
                   [] { initResult = curl_global_init(CURL_GLOBAL_DEFAULT); });
    if (initResult != CURLE_OK) {
        err = std::string("libcurl initialization failed: ") +
              curl_easy_strerror(initResult);
        return false;
    }
    return true;
}

CurlEasySession::~CurlEasySession() {
    if (handle_)
        curl_easy_cleanup(handle_);
}

bool CurlEasySession::initialize(std::string &err) {
    if (handle_)
        return true;
    if (!ensureCurlInitialized(err))
        return false;
    handle_ = curl_easy_init();
    if (!handle_) {
        err = "Could not create CURL handle.";
        return false;
    }
    return true;
}

void CurlEasySession::reset() noexcept {
    if (handle_)
        curl_easy_reset(handle_);
}

CurlClientState::Operation::Operation(CurlClientState &owner)
    : owner_(&owner), lock_(owner.operationMutex_) {
    owner_->interrupted_.store(false);
}

CurlClientState::Operation::Operation(Operation &&other) noexcept
    : owner_(std::exchange(other.owner_, nullptr)),
      lock_(std::move(other.lock_)) {
}

CurlClientState::Operation::~Operation() {
    if (owner_)
        owner_->interrupted_.store(false);
}

bool CurlClientState::Operation::disconnecting() const noexcept {
    return owner_ && owner_->disconnecting_.load();
}

std::atomic<bool> *CurlClientState::Operation::interrupted() const noexcept {
    return owner_ ? &owner_->interrupted_ : nullptr;
}

CurlClientState::CurlClientState(SessionOptions defaultOptions)
    : options_(
          std::make_shared<const SessionOptions>(std::move(defaultOptions))) {
}

CurlClientState::~CurlClientState() = default;

CurlClientState::Operation CurlClientState::beginOperation() {
    return Operation(*this);
}

void CurlClientState::prepareForConnect() {
    std::lock_guard<std::mutex> lock(stateMutex_);
    connected_ = false;
    session_.reset();
}

void CurlClientState::commitConnection(
    std::shared_ptr<const SessionOptions> options,
    std::unique_ptr<CurlEasySession> session, std::string commandRoot) {
    std::lock_guard<std::mutex> lock(stateMutex_);
    options_ = std::move(options);
    session_ = std::move(session);
    commandRoot_ = normalizeRemotePath(commandRoot);
    connected_ = options_ && session_ && session_->get();
}

void CurlClientState::disconnect(SessionOptions defaultOptions) {
    disconnecting_.store(true);
    interrupted_.store(true);
    std::lock_guard<std::mutex> operationLock(operationMutex_);
    {
        std::lock_guard<std::mutex> stateLock(stateMutex_);
        session_.reset();
        options_ =
            std::make_shared<const SessionOptions>(std::move(defaultOptions));
        commandRoot_ = "/";
        connected_ = false;
    }
    interrupted_.store(false);
    disconnecting_.store(false);
}

void CurlClientState::requestInterrupt() noexcept {
    interrupted_.store(true);
}

bool CurlClientState::isConnected() const {
    std::lock_guard<std::mutex> lock(stateMutex_);
    return connected_;
}

CurlConnectionSnapshot
CurlClientState::snapshot(const Operation &operation) const {
    if (operation.owner_ != this)
        return {};
    std::lock_guard<std::mutex> lock(stateMutex_);
    if (!connected_)
        return {};
    return {options_, session_.get(), commandRoot_};
}

bool rejectInterrupted(const std::atomic<bool> *interrupted, std::string &err,
                       CURLcode *curlCodeOut) {
    if (!interrupted || !interrupted->load())
        return false;
    if (curlCodeOut)
        *curlCodeOut = CURLE_ABORTED_BY_CALLBACK;
    err = "Interrupted";
    return true;
}

std::string trimAscii(std::string value) {
    auto isWs = [](unsigned char c) { return std::isspace(c) != 0; };
    while (!value.empty() && isWs(static_cast<unsigned char>(value.front())))
        value.erase(value.begin());
    while (!value.empty() && isWs(static_cast<unsigned char>(value.back())))
        value.pop_back();
    return value;
}

std::string toLowerAscii(std::string value) {
    std::transform(value.begin(), value.end(), value.begin(),
                   [](unsigned char c) -> char {
                       return static_cast<char>(std::tolower(c));
                   });
    return value;
}

bool parseUnsignedDec(std::string_view token, std::uint64_t &out) {
    if (token.empty())
        return false;
    std::uint64_t value = 0;
    for (char ch : token) {
        if (ch < '0' || ch > '9')
            return false;
        const std::uint64_t digit = static_cast<std::uint64_t>(ch - '0');
        if (value > (std::numeric_limits<std::uint64_t>::max() - digit) / 10)
            return false;
        value = value * 10 + digit;
    }
    out = value;
    return true;
}

bool validateUrlHost(const std::string &host, const char *fieldLabel,
                     std::string &err) {
    const std::string label =
        (fieldLabel && *fieldLabel) ? fieldLabel : "Remote host";
    if (host.empty()) {
        err = label + " is required.";
        return false;
    }

    const auto forbiddenAuthorityCharacter = [](const unsigned char ch) {
        return ch < 0x20 || ch == 0x7f || std::isspace(ch) != 0 || ch == '/' ||
               ch == '\\' || ch == '@' || ch == '?' || ch == '#';
    };
    if (std::any_of(host.begin(), host.end(), forbiddenAuthorityCharacter)) {
        err = label + " contains a forbidden URL authority character.";
        return false;
    }

    const bool bracketed = host.front() == '[' || host.back() == ']';
    if (bracketed) {
        if (host.size() < 4 || host.front() != '[' || host.back() != ']' ||
            host.find('[', 1) != std::string::npos ||
            host.find(']') != host.size() - 1) {
            err = label + " contains malformed IPv6 brackets.";
            return false;
        }
        const std::string_view literal =
            std::string_view(host).substr(1, host.size() - 2);
        if (literal.find(':') == std::string_view::npos) {
            err = label + " contains brackets around a non-IPv6 host.";
            return false;
        }
        return true;
    }

    if (host.find('[') != std::string::npos ||
        host.find(']') != std::string::npos) {
        err = label + " contains malformed IPv6 brackets.";
        return false;
    }

    const std::size_t firstColon = host.find(':');
    if (firstColon != std::string::npos &&
        host.find(':', firstColon + 1) == std::string::npos) {
        err =
            label + " must not include a port; use the separate port setting.";
        return false;
    }
    if (firstColon == std::string::npos &&
        host.find('%') != std::string::npos) {
        err = label + " contains an invalid percent escape.";
        return false;
    }
    return true;
}

std::string normalizeHostAuthorityForUrl(const std::string &host) {
    if (host.find(':') != std::string::npos &&
        host.find(']') == std::string::npos) {
        return "[" + host + "]";
    }
    return host;
}

bool validateRemotePath(const std::string &path, const char *backendLabel,
                        std::string &err) {
    if (path.find('\0') != std::string::npos ||
        path.find('\r') != std::string::npos ||
        path.find('\n') != std::string::npos) {
        err = std::string(backendLabel ? backendLabel : "Remote") +
              " path contains a forbidden control character.";
        return false;
    }
    return true;
}

std::string ftpCommandPath(const std::string &loginRoot,
                           const std::string &logicalPath) {
    std::string root = normalizeRemotePath(loginRoot);
    std::string logical = normalizeRemotePath(logicalPath);
    if (root == "/")
        return logical;
    if (logical == "/")
        return root;
    return root + logical;
}

std::string webDavServerPath(std::string_view basePath,
                             std::string_view logicalPath) {
    std::string base = normalizeRemotePath(basePath);
    std::string logical = normalizeRemotePath(logicalPath);
    if (base == "/")
        return logical;
    if (logical == "/")
        return base;
    return base + logical;
}

bool webDavLogicalPath(std::string_view basePath, std::string_view serverPath,
                       std::string &logicalPath) {
    const std::string base = normalizeRemotePath(basePath);
    const std::string server = normalizeRemotePath(serverPath);
    if (base == "/") {
        logicalPath = server;
        return true;
    }
    if (server == base) {
        logicalPath = "/";
        return true;
    }
    const std::string prefix = base + "/";
    if (server.rfind(prefix, 0) != 0)
        return false;
    logicalPath =
        normalizeRemotePath(std::string_view(server).substr(base.size()));
    return true;
}

bool isCompletedWebDavGetStatus(long statusCode) {
    return statusCode == 200;
}

bool isCompletedWebDavWriteStatus(long statusCode) {
    return statusCode == 200 || statusCode == 201 || statusCode == 204;
}

std::optional<std::uint32_t> parseRetryAfter(std::string_view value,
                                             std::time_t now) {
    constexpr std::uint64_t maxRetrySeconds = 60;
    const std::string normalized = trimAscii(std::string(value));
    std::uint64_t seconds = 0;
    if (parseUnsignedDec(normalized, seconds)) {
        return static_cast<std::uint32_t>(
            std::min<std::uint64_t>(seconds, maxRetrySeconds));
    }

    const std::time_t retryAt = curl_getdate(normalized.c_str(), nullptr);
    if (retryAt < 0)
        return std::nullopt;
    if (retryAt <= now)
        return std::uint32_t{0};
    const auto delay = static_cast<std::uint64_t>(retryAt - now);
    return static_cast<std::uint32_t>(
        std::min<std::uint64_t>(delay, maxRetrySeconds));
}

std::string encodeUrlPath(const std::string &path) {
    static constexpr char hex[] = "0123456789ABCDEF";
    std::string out;
    out.reserve(path.size());
    for (const char character : path) {
        const auto c = static_cast<unsigned char>(character);
        const bool unreserved = std::isalnum(c) != 0 || c == '-' || c == '.' ||
                                c == '_' || c == '~' || c == '/';
        if (unreserved) {
            out.push_back(static_cast<char>(c));
        } else {
            out.push_back('%');
            out.push_back(hex[(c >> 4) & 0x0f]);
            out.push_back(hex[c & 0x0f]);
        }
    }
    return out;
}

std::string localPartialPath(const std::string &destination) {
    return destination + ".part";
}

std::FILE *openFileForUpload(const std::string &path, std::uint64_t &fileSize,
                             std::string &err) {
    fileSize = 0;
    try {
        fileSize = std::filesystem::file_size(path);
    } catch (...) {
        err = "Could not determine local file size.";
        return nullptr;
    }
    std::FILE *file = std::fopen(path.c_str(), "rb");
    if (!file)
        err = "Could not open local file for reading.";
    return file;
}

bool flushAndSyncFile(std::FILE *file, std::string &err) {
    return localfiles::flushAndSync(file, err);
}

bool atomicReplaceLocalFile(const std::string &partial,
                            const std::string &destination, std::string &err) {
    return localfiles::atomicReplace(partial, destination, err);
}

RemoteError errorFromCurl(CURLcode code, std::string message, long responseCode,
                          bool commitUncertain) {
    RemoteError error;
    error.message = std::move(message);
    error.native_code =
        responseCode > 0 ? responseCode : static_cast<std::int64_t>(code);
    switch (code) {
    case CURLE_OK:
        error.kind = RemoteErrorKind::None;
        break;
    case CURLE_ABORTED_BY_CALLBACK:
        error.kind = RemoteErrorKind::Canceled;
        break;
    case CURLE_OPERATION_TIMEDOUT:
        error.kind = RemoteErrorKind::Timeout;
        error.transient = true;
        break;
    case CURLE_COULDNT_RESOLVE_HOST:
    case CURLE_COULDNT_RESOLVE_PROXY:
    case CURLE_COULDNT_CONNECT:
    case CURLE_RECV_ERROR:
    case CURLE_SEND_ERROR:
    case CURLE_GOT_NOTHING:
        error.kind = RemoteErrorKind::Connection;
        error.transient = true;
        break;
    case CURLE_PARTIAL_FILE:
        error.kind = RemoteErrorKind::RemoteIo;
        error.transient = true;
        break;
    case CURLE_LOGIN_DENIED:
        error.kind = RemoteErrorKind::Authentication;
        break;
    case CURLE_REMOTE_ACCESS_DENIED:
        error.kind = RemoteErrorKind::PermissionDenied;
        break;
    case CURLE_REMOTE_FILE_NOT_FOUND:
        error.kind = RemoteErrorKind::NotFound;
        break;
    case CURLE_REMOTE_DISK_FULL:
        error.kind = RemoteErrorKind::InsufficientSpace;
        break;
    case CURLE_SSL_CONNECT_ERROR:
    case CURLE_SSL_CERTPROBLEM:
    case CURLE_SSL_CIPHER:
    case CURLE_PEER_FAILED_VERIFICATION:
    case CURLE_SSL_CACERT_BADFILE:
    case CURLE_SSL_CRL_BADFILE:
    case CURLE_SSL_ISSUER_ERROR:
    case CURLE_SSL_PINNEDPUBKEYNOTMATCH:
    case CURLE_SSL_INVALIDCERTSTATUS:
#if LIBCURL_VERSION_NUM >= 0x074d00
    case CURLE_SSL_CLIENTCERT:
#endif
        error.kind = RemoteErrorKind::Certificate;
        break;
    case CURLE_WRITE_ERROR:
        if (errno == ENOSPC) {
            error.kind = RemoteErrorKind::InsufficientSpace;
            error.native_code = ENOSPC;
            break;
        }
        error.kind = RemoteErrorKind::LocalIo;
        break;
    case CURLE_READ_ERROR:
        error.kind = RemoteErrorKind::LocalIo;
        break;
    case CURLE_QUOTE_ERROR:
    case CURLE_WEIRD_SERVER_REPLY:
        error.kind = RemoteErrorKind::Protocol;
        break;
    default:
        error.kind = RemoteErrorKind::RemoteIo;
        break;
    }
    error.commit_uncertain =
        commitUncertain && (error.kind == RemoteErrorKind::Connection ||
                            error.kind == RemoteErrorKind::Timeout ||
                            error.kind == RemoteErrorKind::RemoteIo);
    return error;
}

RemoteError errorFromHttpStatus(long statusCode, std::string message,
                                bool commitUncertain,
                                std::optional<std::uint32_t> retryAfter) {
    RemoteError error;
    error.message = std::move(message);
    error.native_code = statusCode;
    error.retry_after_seconds = retryAfter;
    error.commit_uncertain = commitUncertain && statusCode >= 500;
    if (statusCode == 401) {
        error.kind = RemoteErrorKind::Authentication;
    } else if (statusCode == 403) {
        error.kind = RemoteErrorKind::PermissionDenied;
    } else if (statusCode == 404 || statusCode == 410) {
        error.kind = RemoteErrorKind::NotFound;
    } else if (statusCode == 408 || statusCode == 504) {
        error.kind = RemoteErrorKind::Timeout;
        error.transient = true;
    } else if (statusCode == 409 || statusCode == 412 || statusCode == 423) {
        error.kind = RemoteErrorKind::Conflict;
    } else if (statusCode == 429) {
        error.kind = RemoteErrorKind::RateLimited;
        error.transient = true;
    } else if (statusCode == 507) {
        error.kind = RemoteErrorKind::InsufficientSpace;
        error.commit_uncertain = false;
    } else if (statusCode >= 500 && statusCode <= 599) {
        error.kind = RemoteErrorKind::RemoteIo;
        error.transient = true;
    } else {
        error.kind = RemoteErrorKind::Protocol;
    }
    return error;
}

ActiveDestinationLease::ActiveDestinationLease(std::string key)
    : key_(std::move(key)) {
    std::lock_guard<std::mutex> lock(activeDestinationMutex);
    acquired_ = activeDestinations.insert(key_).second;
}

ActiveDestinationLease::~ActiveDestinationLease() {
    if (!acquired_)
        return;
    std::lock_guard<std::mutex> lock(activeDestinationMutex);
    activeDestinations.erase(key_);
}

std::string localDestinationKey(const std::string &path) {
    std::error_code pathError;
    const std::filesystem::path absolutePath =
        std::filesystem::absolute(std::filesystem::path(path), pathError);
    if (pathError)
        return std::string("local:") + path;
    return std::string("local:") + absolutePath.lexically_normal().string();
}

bool configureBaseCurlHandle(CURL *curl, const char *backendLabel,
                             bool acceptCompressedResponses,
                             std::optional<long> responseTimeoutSeconds,
                             std::string &err) {
    if (!curl) {
        err = "Could not create CURL handle.";
        return false;
    }
    if (curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_LIMIT, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_LOW_SPEED_TIME, 60L) != CURLE_OK) {
        err = std::string("Could not configure ") + backendLabel +
              " client timeouts.";
        return false;
    }
    if (responseTimeoutSeconds &&
        curl_easy_setopt(curl, CURLOPT_FTP_RESPONSE_TIMEOUT,
                         *responseTimeoutSeconds) != CURLE_OK) {
        err = std::string("Could not configure ") + backendLabel +
              " client timeouts.";
        return false;
    }
    if (acceptCompressedResponses &&
        curl_easy_setopt(curl, CURLOPT_ACCEPT_ENCODING, "") != CURLE_OK) {
        err = std::string("Could not configure ") + backendLabel +
              " response encoding.";
        return false;
    }
    return true;
}

bool configureProxy(CURL *curl, const SessionOptions &opt,
                    const char *backendLabel, const char *backendKindLabel,
                    std::string &err) {
    if (opt.proxy_type == ProxyType::None) {
        // An empty explicit proxy disables libcurl's implicit ALL_PROXY,
        // http_proxy and ftp_proxy environment handling.
        if (curl_easy_setopt(curl, CURLOPT_PROXY, "") != CURLE_OK) {
            err = std::string("Could not disable environment proxy use for ") +
                  backendLabel + ".";
            return false;
        }
        return true;
    }

    if (opt.proxy_host.empty() || opt.proxy_port == 0) {
        err = std::string(backendLabel) + " proxy requires host and port.";
        return false;
    }
    if (!validateUrlHost(opt.proxy_host, "Proxy host", err))
        return false;
    const std::string proxy = normalizeHostAuthorityForUrl(opt.proxy_host) +
                              ":" + std::to_string(opt.proxy_port);
    if (curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str()) != CURLE_OK) {
        err = std::string("Could not configure ") + backendLabel +
              " proxy endpoint.";
        return false;
    }

    const ProxyType normalizedProxyType = normalizeProxyType(opt.proxy_type);
    // Map app-level proxy enum to libcurl proxy transport.
    long proxyType = 0;
    switch (normalizedProxyType) {
    case ProxyType::Socks5:
        proxyType = CURLPROXY_SOCKS5_HOSTNAME;
        break;
    case ProxyType::HttpConnect:
        proxyType = CURLPROXY_HTTP;
        break;
    case ProxyType::None:
        err = std::string("Unsupported proxy type for ") + backendKindLabel +
              " backend.";
        return false;
    }
    if (curl_easy_setopt(curl, CURLOPT_PROXYTYPE, proxyType) != CURLE_OK) {
        err =
            std::string("Could not configure ") + backendLabel + " proxy type.";
        return false;
    }
    if (curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL,
                         (normalizedProxyType == ProxyType::HttpConnect)
                             ? 1L
                             : 0L) != CURLE_OK) {
        err = std::string("Could not configure ") + backendLabel +
              " proxy tunnel mode.";
        return false;
    }

    if (opt.proxy_username && !opt.proxy_username->empty()) {
        if (curl_easy_setopt(curl, CURLOPT_PROXYUSERNAME,
                             opt.proxy_username->c_str()) != CURLE_OK) {
            err = std::string("Could not configure ") + backendLabel +
                  " proxy username.";
            return false;
        }
    }
    if (opt.proxy_password && !opt.proxy_password->empty()) {
        if (curl_easy_setopt(curl, CURLOPT_PROXYPASSWORD,
                             opt.proxy_password->c_str()) != CURLE_OK) {
            err = std::string("Could not configure ") + backendLabel +
                  " proxy password.";
            return false;
        }
    }

    return true;
}

bool configureTlsVerification(CURL *curl, bool verifyPeer,
                              const std::optional<std::string> &caCertPath,
                              const char *verificationError,
                              const char *caPathError, std::string &err) {
    const long verifyPeerValue = verifyPeer ? 1L : 0L;
    const long verifyHostValue = verifyPeer ? 2L : 0L;
    if (curl_easy_setopt(curl, CURLOPT_SSL_VERIFYPEER, verifyPeerValue) !=
            CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_SSL_VERIFYHOST, verifyHostValue) !=
            CURLE_OK) {
        err = verificationError;
        return false;
    }
    if (caCertPath && !caCertPath->empty()) {
        if (curl_easy_setopt(curl, CURLOPT_CAINFO, caCertPath->c_str()) !=
            CURLE_OK) {
            err = caPathError;
            return false;
        }
    }
    return true;
}

size_t appendStringCallback(char *ptr, size_t size, size_t nmemb,
                            void *userdata) {
    auto *sink = static_cast<BoundedStringSink *>(userdata);
    if (!sink || !sink->output || !ptr)
        return 0;
    if (size != 0 && nmemb > std::numeric_limits<size_t>::max() / size) {
        sink->limitExceeded = true;
        return 0;
    }
    const size_t total = size * nmemb;
    if (sink->output->size() > sink->maxBytes ||
        total > sink->maxBytes - sink->output->size()) {
        sink->limitExceeded = true;
        return 0;
    }
    try {
        sink->output->append(ptr, total);
    } catch (...) {
        sink->allocationFailed = true;
        return 0;
    }
    return total;
}

size_t writeFileCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    if (!userdata)
        return 0;
    std::FILE *file = static_cast<std::FILE *>(userdata);
    const size_t total = size * nmemb;
    return std::fwrite(ptr, 1, total, file);
}

size_t readFileCallback(char *ptr, size_t size, size_t nmemb, void *userdata) {
    if (!userdata)
        return 0;
    std::FILE *file = static_cast<std::FILE *>(userdata);
    const size_t total = size * nmemb;
    return std::fread(ptr, 1, total, file);
}

bool configureFileDownload(CURL *curl, std::FILE *file,
                           TransferProgressContext &progressContext,
                           std::string &err) {
    const bool configured =
        curl && file &&
        (curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFileCallback) ==
         CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_WRITEDATA, file) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                          transferProgressCallback) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressContext) ==
         CURLE_OK);
    if (!configured)
        err = "Could not configure CURL download transfer.";
    return configured;
}

bool configureFileUpload(CURL *curl, std::FILE *file, curl_off_t fileSize,
                         TransferProgressContext &progressContext,
                         std::string &err) {
    const bool configured =
        curl && file &&
        (curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_READFUNCTION, readFileCallback) ==
         CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_READDATA, file) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE, fileSize) ==
         CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                          transferProgressCallback) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &progressContext) ==
         CURLE_OK);
    if (!configured)
        err = "Could not configure CURL upload transfer.";
    return configured;
}

bool detectTransferCancellation(const TransferProgressContext &progressContext,
                                std::string &err) {
    if (progressContext.shouldCancel && progressContext.shouldCancel()) {
        err = "Canceled by user";
        return true;
    }
    if (progressContext.interrupted && progressContext.interrupted->load()) {
        err = "Interrupted";
        return true;
    }
    return false;
}

CurlTransferResult
performCurlTransfer(CURL *curl, TransferProgressContext &progressContext,
                    std::string_view operationLabel,
                    const CurlTransferConfigurator &configure,
                    std::string &err) {
    CurlTransferResult result;
    if (!curl) {
        result.failure = CurlTransferFailure::Configuration;
        err = "Could not create CURL handle.";
        return result;
    }

    curl_easy_reset(curl);
    if (!configure || !configure(curl, err)) {
        result.failure = CurlTransferFailure::Configuration;
        if (err.empty())
            err = std::string("Could not configure ") +
                  std::string(operationLabel) + ".";
        return result;
    }

    if (detectTransferCancellation(progressContext, err)) {
        result.failure = CurlTransferFailure::Canceled;
        result.curlCode = CURLE_ABORTED_BY_CALLBACK;
        return result;
    }

    result.curlCode = curl_easy_perform(curl);
    (void)curl_easy_getinfo(curl, CURLINFO_RESPONSE_CODE, &result.responseCode);
    if (detectTransferCancellation(progressContext, err)) {
        result.failure = CurlTransferFailure::Canceled;
        result.curlCode = CURLE_ABORTED_BY_CALLBACK;
        return result;
    }
    if (result.curlCode != CURLE_OK) {
        result.failure = CurlTransferFailure::Transport;
        err = std::string(operationLabel) +
              " failed: " + curl_easy_strerror(result.curlCode);
    }
    return result;
}

RemoteError
transferFailureError(const CurlTransferResult &result, std::string message,
                     const CurlTransportErrorMapper &transportErrorMapper) {
    if (result.failure == CurlTransferFailure::Canceled) {
        RemoteError error;
        error.kind = RemoteErrorKind::Canceled;
        error.message = std::move(message);
        error.native_code =
            static_cast<std::int64_t>(CURLE_ABORTED_BY_CALLBACK);
        return error;
    }
    if (result.failure == CurlTransferFailure::Configuration) {
        RemoteError error;
        error.kind = RemoteErrorKind::InvalidRequest;
        error.message = std::move(message);
        return error;
    }
    if (transportErrorMapper) {
        return transportErrorMapper(result.curlCode, result.responseCode,
                                    message);
    }
    return errorFromCurl(result.curlCode, std::move(message),
                         result.responseCode);
}

int transferProgressCallback(void *userdata, curl_off_t dltotal,
                             curl_off_t dlnow, curl_off_t ultotal,
                             curl_off_t ulnow) {
    auto *ctx = static_cast<TransferProgressContext *>(userdata);
    if (!ctx)
        return 0;
    if (ctx->interrupted && ctx->interrupted->load())
        return 1;
    if (ctx->shouldCancel && ctx->shouldCancel())
        return 1;
    if (!ctx->progressCb)
        return 0;

    const bool preferUpload = ctx->preferUploadCounters;
    // Some protocols only report one side reliably; pick preferred counters
    // and fallback to the opposite side when needed.
    const curl_off_t totalRaw = preferUpload
                                    ? ((ultotal > 0) ? ultotal : dltotal)
                                    : ((dltotal > 0) ? dltotal : ultotal);
    const curl_off_t doneRaw = preferUpload ? ((ulnow > 0) ? ulnow : dlnow)
                                            : ((dlnow > 0) ? dlnow : ulnow);
    const std::size_t total =
        totalRaw > 0 ? static_cast<std::size_t>(totalRaw) : 0u;
    const std::size_t done =
        doneRaw > 0 ? static_cast<std::size_t>(doneRaw) : 0u;
    ctx->progressCb(done, total);
    // A progress callback can itself publish the state that makes
    // shouldCancel() true (for example at done == total). Observe that state
    // before libcurl is allowed to report a successful transfer.
    if (ctx->interrupted && ctx->interrupted->load())
        return 1;
    if (ctx->shouldCancel && ctx->shouldCancel())
        return 1;
    return 0;
}

} // namespace openscp::curlcommon
