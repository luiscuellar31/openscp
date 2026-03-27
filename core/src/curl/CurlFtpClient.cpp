// FTP backend implementation based on libcurl.
#include "openscp/CurlFtpClient.hpp"

#include <curl/curl.h>

#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>

namespace openscp {
namespace {

bool ensureCurlInitialized(std::string &err) {
    static std::once_flag initFlag;
    static CURLcode initResult = CURLE_OK;
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

bool unsupportedFtpOperation(const char *what, std::string &err) {
    err = std::string("FTP backend does not support ") + what + ".";
    return false;
}

std::string normalizeRemotePath(std::string path) {
    if (path.empty())
        return "/";
    if (path.front() != '/')
        path.insert(path.begin(), '/');
    return path;
}

std::string normalizeFtpHostAuthority(const std::string &host) {
    if (host.find(':') != std::string::npos &&
        host.find(']') == std::string::npos) {
        return "[" + host + "]";
    }
    return host;
}

std::string buildFtpUrl(const SessionOptions &opt,
                        const std::string &remotePath) {
    const std::string host = normalizeFtpHostAuthority(opt.host);
    const std::string path = normalizeRemotePath(remotePath);
    return std::string("ftp://") + host + ":" + std::to_string(opt.port) +
           path;
}

bool shouldFailConnectCode(CURLcode code) {
    switch (code) {
    case CURLE_OK:
    case CURLE_REMOTE_ACCESS_DENIED:
    case CURLE_QUOTE_ERROR:
    case CURLE_FTP_COULDNT_RETR_FILE:
        return false;
    default:
        return true;
    }
}

bool configureCommonCurlHandle(CURL *curl, const SessionOptions &opt,
                               std::string &err) {
    if (!curl) {
        err = "Could not create CURL handle.";
        return false;
    }
    if (curl_easy_setopt(curl, CURLOPT_NOSIGNAL, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_CONNECTTIMEOUT, 15L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_FTP_RESPONSE_TIMEOUT, 30L) != CURLE_OK) {
        err = "Could not configure FTP client timeouts.";
        return false;
    }

    const std::string username =
        opt.username.empty() ? std::string("anonymous") : opt.username;
    const std::string password =
        (opt.username.empty() && (!opt.password || opt.password->empty()))
            ? std::string("anonymous@openscp.local")
            : (opt.password ? *opt.password : std::string());

    if (curl_easy_setopt(curl, CURLOPT_USERNAME, username.c_str()) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_PASSWORD, password.c_str()) != CURLE_OK) {
        err = "Could not set FTP credentials.";
        return false;
    }

    if (opt.proxy_type != ProxyType::None) {
        if (opt.proxy_host.empty() || opt.proxy_port == 0) {
            err = "FTP proxy requires host and port.";
            return false;
        }
        const std::string proxy =
            opt.proxy_host + ":" + std::to_string(opt.proxy_port);
        if (curl_easy_setopt(curl, CURLOPT_PROXY, proxy.c_str()) != CURLE_OK) {
            err = "Could not configure FTP proxy endpoint.";
            return false;
        }

        const long proxyType =
            (opt.proxy_type == ProxyType::Socks5) ? CURLPROXY_SOCKS5_HOSTNAME
                                                  : CURLPROXY_HTTP;
        if (curl_easy_setopt(curl, CURLOPT_PROXYTYPE, proxyType) != CURLE_OK) {
            err = "Could not configure FTP proxy type.";
            return false;
        }

        if (opt.proxy_username && !opt.proxy_username->empty()) {
            if (curl_easy_setopt(curl, CURLOPT_PROXYUSERNAME,
                                 opt.proxy_username->c_str()) != CURLE_OK) {
                err = "Could not configure FTP proxy username.";
                return false;
            }
        }
        if (opt.proxy_password && !opt.proxy_password->empty()) {
            if (curl_easy_setopt(curl, CURLOPT_PROXYPASSWORD,
                                 opt.proxy_password->c_str()) != CURLE_OK) {
                err = "Could not configure FTP proxy password.";
                return false;
            }
        }
    }

    return true;
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

struct ProgressContext {
    std::function<void(std::size_t, std::size_t)> progress;
    std::function<bool()> shouldCancel;
    std::atomic<bool> *interrupted = nullptr;
};

int transferProgressCallback(void *userdata, curl_off_t dltotal, curl_off_t dlnow,
                             curl_off_t ultotal, curl_off_t ulnow) {
    ProgressContext *ctx = static_cast<ProgressContext *>(userdata);
    if (!ctx)
        return 0;
    if (ctx->interrupted && ctx->interrupted->load())
        return 1;
    if (ctx->shouldCancel && ctx->shouldCancel())
        return 1;

    const std::size_t total = (dltotal > 0)
                                  ? static_cast<std::size_t>(dltotal)
                                  : ((ultotal > 0) ? static_cast<std::size_t>(ultotal)
                                                   : 0u);
    const std::size_t done = (dlnow > 0)
                                 ? static_cast<std::size_t>(dlnow)
                                 : ((ulnow > 0) ? static_cast<std::size_t>(ulnow)
                                                : 0u);
    if (ctx->progress)
        ctx->progress(done, total);
    return 0;
}

} // namespace

bool CurlFtpClient::connect(const SessionOptions &opt, std::string &err) {
    err.clear();
    interrupted_.store(false);
    if (opt.host.empty()) {
        err = "Host is required.";
        return false;
    }
    if (opt.protocol != Protocol::Ftp) {
        err = "CurlFtpClient only supports FTP protocol.";
        return false;
    }
    if (opt.jump_host.has_value() && !opt.jump_host->empty()) {
        err = "FTP backend does not support SSH jump host.";
        return false;
    }
    if (!ensureCurlInitialized(err))
        return false;

    SessionOptions normalized = opt;
    normalized.protocol = Protocol::Ftp;
    if (normalized.port == 0)
        normalized.port = defaultPortForProtocol(Protocol::Ftp);

    CURL *curl = curl_easy_init();
    if (!curl) {
        err = "Could not create CURL handle.";
        return false;
    }
    if (!configureCommonCurlHandle(curl, normalized, err)) {
        curl_easy_cleanup(curl);
        return false;
    }

    const std::string url = buildFtpUrl(normalized, "/");
    std::string sink;
    if (curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_DIRLISTONLY, 1L) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION,
                         +[](char *ptr, size_t size, size_t nmemb,
                             void *userdata) -> size_t {
                             if (!userdata)
                                 return 0;
                             std::string *out = static_cast<std::string *>(userdata);
                             out->append(ptr, size * nmemb);
                             return size * nmemb;
                         }) != CURLE_OK ||
        curl_easy_setopt(curl, CURLOPT_WRITEDATA, &sink) != CURLE_OK) {
        err = "Could not configure FTP connection probe.";
        curl_easy_cleanup(curl);
        return false;
    }

    const CURLcode rc = curl_easy_perform(curl);
    curl_easy_cleanup(curl);
    if (shouldFailConnectCode(rc)) {
        err = std::string("FTP connect probe failed: ") +
              curl_easy_strerror(rc);
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        options_ = normalized;
        connected_ = true;
    }
    return true;
}

void CurlFtpClient::disconnect() {
    interrupted_.store(false);
    std::lock_guard<std::mutex> lk(stateMutex_);
    connected_ = false;
    options_ = SessionOptions{};
    options_.protocol = Protocol::Ftp;
    options_.port = defaultPortForProtocol(Protocol::Ftp);
}

void CurlFtpClient::interrupt() { interrupted_.store(true); }

bool CurlFtpClient::isConnected() const {
    std::lock_guard<std::mutex> lk(stateMutex_);
    return connected_;
}

bool CurlFtpClient::list(const std::string &remote_path,
                         std::vector<FileInfo> &out, std::string &err) {
    (void)remote_path;
    out.clear();
    return unsupportedFtpOperation("directory listing", err);
}

bool CurlFtpClient::get(
    const std::string &remote, const std::string &local, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel, bool resume) {
    err.clear();
    if (resume) {
        err = "FTP backend does not support resume.";
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

    CURL *curl = curl_easy_init();
    if (!curl) {
        std::fclose(localFile);
        err = "Could not create CURL handle.";
        return false;
    }
    if (!configureCommonCurlHandle(curl, opt, err)) {
        std::fclose(localFile);
        curl_easy_cleanup(curl);
        return false;
    }

    ProgressContext ctx{progress, shouldCancel, &interrupted_};
    const std::string url = buildFtpUrl(opt, remote);
    const bool configured =
        (curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, writeFileCallback) ==
         CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_WRITEDATA, localFile) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                          transferProgressCallback) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx) == CURLE_OK);
    if (!configured) {
        std::fclose(localFile);
        curl_easy_cleanup(curl);
        err = "Could not configure FTP download.";
        return false;
    }

    const CURLcode rc = curl_easy_perform(curl);
    std::fclose(localFile);
    curl_easy_cleanup(curl);
    if (rc == CURLE_ABORTED_BY_CALLBACK && (shouldCancel && shouldCancel())) {
        err = "Canceled by user";
        return false;
    }
    if (rc == CURLE_ABORTED_BY_CALLBACK && interrupted_.load()) {
        err = "Interrupted";
        return false;
    }
    if (rc != CURLE_OK) {
        err = std::string("FTP download failed: ") + curl_easy_strerror(rc);
        return false;
    }
    return true;
}

bool CurlFtpClient::put(
    const std::string &local, const std::string &remote, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel, bool resume) {
    err.clear();
    if (resume) {
        err = "FTP backend does not support resume.";
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

    CURL *curl = curl_easy_init();
    if (!curl) {
        std::fclose(localFile);
        err = "Could not create CURL handle.";
        return false;
    }
    if (!configureCommonCurlHandle(curl, opt, err)) {
        std::fclose(localFile);
        curl_easy_cleanup(curl);
        return false;
    }

    ProgressContext ctx{progress, shouldCancel, &interrupted_};
    const std::string url = buildFtpUrl(opt, remote);
    const bool configured =
        (curl_easy_setopt(curl, CURLOPT_URL, url.c_str()) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_UPLOAD, 1L) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_READFUNCTION, readFileCallback) ==
         CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_READDATA, localFile) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_INFILESIZE_LARGE,
                          static_cast<curl_off_t>(total)) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_FTP_CREATE_MISSING_DIRS,
                          CURLFTP_CREATE_DIR_RETRY) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_NOPROGRESS, 0L) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFOFUNCTION,
                          transferProgressCallback) == CURLE_OK) &&
        (curl_easy_setopt(curl, CURLOPT_XFERINFODATA, &ctx) == CURLE_OK);
    if (!configured) {
        std::fclose(localFile);
        curl_easy_cleanup(curl);
        err = "Could not configure FTP upload.";
        return false;
    }

    const CURLcode rc = curl_easy_perform(curl);
    std::fclose(localFile);
    curl_easy_cleanup(curl);
    if (rc == CURLE_ABORTED_BY_CALLBACK && (shouldCancel && shouldCancel())) {
        err = "Canceled by user";
        return false;
    }
    if (rc == CURLE_ABORTED_BY_CALLBACK && interrupted_.load()) {
        err = "Interrupted";
        return false;
    }
    if (rc != CURLE_OK) {
        err = std::string("FTP upload failed: ") + curl_easy_strerror(rc);
        return false;
    }
    return true;
}

bool CurlFtpClient::exists(const std::string &remote_path, bool &isDir,
                           std::string &err) {
    (void)remote_path;
    isDir = false;
    return unsupportedFtpOperation("path existence checks", err);
}

bool CurlFtpClient::stat(const std::string &remote_path, FileInfo &info,
                         std::string &err) {
    (void)remote_path;
    info = FileInfo{};
    return unsupportedFtpOperation("metadata stat", err);
}

bool CurlFtpClient::chmod(const std::string &remote_path, std::uint32_t mode,
                          std::string &err) {
    (void)remote_path;
    (void)mode;
    return unsupportedFtpOperation("chmod", err);
}

bool CurlFtpClient::chown(const std::string &remote_path, std::uint32_t uid,
                          std::uint32_t gid, std::string &err) {
    (void)remote_path;
    (void)uid;
    (void)gid;
    return unsupportedFtpOperation("chown", err);
}

bool CurlFtpClient::setTimes(const std::string &remote_path, std::uint64_t atime,
                             std::uint64_t mtime, std::string &err) {
    (void)remote_path;
    (void)atime;
    (void)mtime;
    return unsupportedFtpOperation("timestamp updates", err);
}

bool CurlFtpClient::mkdir(const std::string &remote_dir, std::string &err,
                          unsigned int mode) {
    (void)remote_dir;
    (void)mode;
    return unsupportedFtpOperation("mkdir", err);
}

bool CurlFtpClient::removeFile(const std::string &remote_path,
                               std::string &err) {
    (void)remote_path;
    return unsupportedFtpOperation("file deletion", err);
}

bool CurlFtpClient::removeDir(const std::string &remote_dir, std::string &err) {
    (void)remote_dir;
    return unsupportedFtpOperation("directory deletion", err);
}

bool CurlFtpClient::rename(const std::string &from, const std::string &to,
                           std::string &err, bool overwrite) {
    (void)from;
    (void)to;
    (void)overwrite;
    return unsupportedFtpOperation("rename", err);
}

std::unique_ptr<SftpClient>
CurlFtpClient::newConnectionLike(const SessionOptions &opt, std::string &err) {
    auto ptr = std::make_unique<CurlFtpClient>();
    if (!ptr->connect(opt, err))
        return nullptr;
    return ptr;
}

} // namespace openscp
