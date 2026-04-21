// Shared internal helpers for libcurl-based backends (FTP/WebDAV).
#pragma once

#include "openscp/SftpTypes.hpp"

#include <curl/curl.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>

namespace openscp::curlcommon {

bool ensureCurlInitialized(std::string &err);

std::string trimAscii(std::string value);
std::string toLowerAscii(std::string value);
bool parseUnsignedDec(std::string_view token, std::uint64_t &out);
std::string normalizeHostAuthorityForUrl(const std::string &host);

bool configureProxy(CURL *curl, const SessionOptions &opt,
                    const char *backendLabel, const char *backendKindLabel,
                    std::string &err);

bool configureTlsVerification(CURL *curl, bool verifyPeer,
                              const std::optional<std::string> &caCertPath,
                              const char *verificationError,
                              const char *caPathError, std::string &err);

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

int transferProgressCallback(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow);

} // namespace openscp::curlcommon
