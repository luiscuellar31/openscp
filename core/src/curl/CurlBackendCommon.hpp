// Shared internal helpers for libcurl-based backends (FTP/WebDAV).
#pragma once

#include "openscp/SftpTypes.hpp"

#include <curl/curl.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <ctime>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>

namespace openscp::curlcommon {

bool ensureCurlInitialized(std::string &err);

class CurlEasySession final {
    public:
    CurlEasySession() = default;
    ~CurlEasySession();
    CurlEasySession(const CurlEasySession &) = delete;
    CurlEasySession &operator=(const CurlEasySession &) = delete;

    [[nodiscard]] bool initialize(std::string &err);
    void reset() noexcept;
    [[nodiscard]] CURL *get() const noexcept { return handle_; }

    private:
    CURL *handle_ = nullptr;
};

struct CurlConnectionSnapshot {
    std::shared_ptr<const SessionOptions> options;
    CurlEasySession *session = nullptr;
    std::string commandRoot = "/";

    [[nodiscard]] explicit operator bool() const noexcept {
        return options && session && session->get();
    }
};

class CurlClientState final {
    public:
    class Operation final {
        public:
        Operation(const Operation &) = delete;
        Operation &operator=(const Operation &) = delete;
        Operation(Operation &&other) noexcept;
        Operation &operator=(Operation &&) = delete;
        ~Operation();

        [[nodiscard]] bool disconnecting() const noexcept;
        [[nodiscard]] std::atomic<bool> *interrupted() const noexcept;

        private:
        friend class CurlClientState;
        explicit Operation(CurlClientState &owner);

        CurlClientState *owner_ = nullptr;
        std::unique_lock<std::mutex> lock_;
    };

    explicit CurlClientState(SessionOptions defaultOptions);
    ~CurlClientState();
    CurlClientState(const CurlClientState &) = delete;
    CurlClientState &operator=(const CurlClientState &) = delete;

    [[nodiscard]] Operation beginOperation();
    void prepareForConnect();
    void commitConnection(std::shared_ptr<const SessionOptions> options,
                          std::unique_ptr<CurlEasySession> session,
                          std::string commandRoot = "/");
    void disconnect(SessionOptions defaultOptions);
    void requestInterrupt() noexcept;
    [[nodiscard]] bool isConnected() const;
    [[nodiscard]] CurlConnectionSnapshot
    snapshot(const Operation &operation) const;

    private:
    // Lock hierarchy: operationMutex_ is acquired before stateMutex_. A
    // snapshot is valid only while its Operation keeps the outer lock held.
    mutable std::mutex stateMutex_;
    std::mutex operationMutex_;
    std::unique_ptr<CurlEasySession> session_;
    std::shared_ptr<const SessionOptions> options_;
    std::string commandRoot_ = "/";
    bool connected_ = false;
    std::atomic<bool> interrupted_{false};
    std::atomic<bool> disconnecting_{false};
};

bool rejectInterrupted(const std::atomic<bool> *interrupted, std::string &err,
                       CURLcode *curlCodeOut = nullptr);

std::string trimAscii(std::string value);
std::string toLowerAscii(std::string value);
bool parseUnsignedDec(std::string_view token, std::uint64_t &out);
bool validateUrlHost(const std::string &host, const char *fieldLabel,
                     std::string &err);
std::string normalizeHostAuthorityForUrl(const std::string &host);
bool validateRemotePath(const std::string &path, const char *backendLabel,
                        std::string &err);
std::string ftpCommandPath(const std::string &loginRoot,
                           const std::string &logicalPath);
std::string webDavServerPath(std::string_view basePath,
                             std::string_view logicalPath);
bool webDavLogicalPath(std::string_view basePath, std::string_view serverPath,
                       std::string &logicalPath);
bool isCompletedWebDavGetStatus(long statusCode);
bool isCompletedWebDavWriteStatus(long statusCode);
std::optional<std::uint32_t>
parseRetryAfter(std::string_view value, std::time_t now = std::time(nullptr));
std::string encodeUrlPath(const std::string &path);

std::string localPartialPath(const std::string &destination);
bool flushAndSyncFile(std::FILE *file, std::string &err);
bool atomicReplaceLocalFile(const std::string &partial,
                            const std::string &destination, std::string &err);

RemoteError errorFromCurl(CURLcode code, std::string message,
                          long responseCode = 0, bool commitUncertain = false);
RemoteError
errorFromHttpStatus(long statusCode, std::string message,
                    bool commitUncertain = false,
                    std::optional<std::uint32_t> retryAfter = std::nullopt);

// Process-local guard for deterministic .part paths. It prevents independent
// worker clients from writing the same destination concurrently.
class ActiveDestinationLease {
    public:
    explicit ActiveDestinationLease(std::string key);
    ~ActiveDestinationLease();
    ActiveDestinationLease(const ActiveDestinationLease &) = delete;
    ActiveDestinationLease &operator=(const ActiveDestinationLease &) = delete;

    bool acquired() const noexcept { return acquired_; }

    private:
    std::string key_;
    bool acquired_ = false;
};

std::string localDestinationKey(const std::string &path);

bool configureBaseCurlHandle(CURL *curl, const char *backendLabel,
                             bool acceptCompressedResponses,
                             std::optional<long> responseTimeoutSeconds,
                             std::string &err);

bool configureProxy(CURL *curl, const SessionOptions &opt,
                    const char *backendLabel, const char *backendKindLabel,
                    std::string &err);

bool configureTlsVerification(CURL *curl, bool verifyPeer,
                              const std::optional<std::string> &caCertPath,
                              const char *verificationError,
                              const char *caPathError, std::string &err);

inline constexpr std::size_t kMaxMetadataResponseBytes = 64 * 1024 * 1024;

struct BoundedStringSink {
    std::string *output = nullptr;
    std::size_t maxBytes = kMaxMetadataResponseBytes;
    bool limitExceeded = false;
    bool allocationFailed = false;
};

size_t appendStringCallback(char *ptr, size_t size, size_t nmemb,
                            void *userdata);
size_t writeFileCallback(char *ptr, size_t size, size_t nmemb, void *userdata);
size_t readFileCallback(char *ptr, size_t size, size_t nmemb, void *userdata);

struct TransferProgressContext {
    std::function<void(std::size_t, std::size_t)> progressCb;
    std::function<bool()> shouldCancel;
    const std::atomic<bool> *interrupted = nullptr;
    bool preferUploadCounters = false;
};

int transferProgressCallback(void *userdata, curl_off_t dltotal,
                             curl_off_t dlnow, curl_off_t ultotal,
                             curl_off_t ulnow);

} // namespace openscp::curlcommon
