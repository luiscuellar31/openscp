// Shared internal helpers for libcurl-based backends (FTP/WebDAV).
#include "CurlBackendCommon.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <limits>
#include <mutex>
#include <string>

namespace openscp::curlcommon {

bool ensureCurlInitialized(std::string &err) {
    static std::once_flag initFlag;
    static CURLcode initResult = CURLE_OK;
    // libcurl global init is process-wide and must run exactly once.
    std::call_once(initFlag, [] {
        initResult = curl_global_init(CURL_GLOBAL_DEFAULT);
    });
    if (initResult != CURLE_OK) {
        err = std::string("libcurl initialization failed: ") +
              curl_easy_strerror(initResult);
        return false;
    }
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

std::string normalizeHostAuthorityForUrl(const std::string &host) {
    if (host.find(':') != std::string::npos &&
        host.find(']') == std::string::npos) {
        return "[" + host + "]";
    }
    return host;
}

bool configureProxy(CURL *curl, const SessionOptions &opt,
                    const char *backendLabel, const char *backendKindLabel,
                    std::string &err) {
    if (opt.proxy_type == ProxyType::None)
        return true;

    if (opt.proxy_host.empty() || opt.proxy_port == 0) {
        err = std::string(backendLabel) + " proxy requires host and port.";
        return false;
    }
    const std::string proxy = opt.proxy_host + ":" + std::to_string(opt.proxy_port);
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
        err = std::string("Could not configure ") + backendLabel + " proxy type.";
        return false;
    }
    if (curl_easy_setopt(curl, CURLOPT_HTTPPROXYTUNNEL,
                         (normalizedProxyType == ProxyType::HttpConnect) ? 1L
                                                                          : 0L) !=
        CURLE_OK) {
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
    if (!userdata)
        return 0;
    auto *out = static_cast<std::string *>(userdata);
    const size_t total = size * nmemb;
    out->append(ptr, total);
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

int transferProgressCallback(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow) {
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
    const curl_off_t totalRaw =
        preferUpload ? ((ultotal > 0) ? ultotal : dltotal)
                     : ((dltotal > 0) ? dltotal : ultotal);
    const curl_off_t doneRaw =
        preferUpload ? ((ulnow > 0) ? ulnow : dlnow)
                     : ((dlnow > 0) ? dlnow : ulnow);
    const std::size_t total =
        totalRaw > 0 ? static_cast<std::size_t>(totalRaw) : 0u;
    const std::size_t done =
        doneRaw > 0 ? static_cast<std::size_t>(doneRaw) : 0u;
    ctx->progressCb(done, total);
    return 0;
}

} // namespace openscp::curlcommon
