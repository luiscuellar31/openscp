// libssh2 backend: manages TCP socket, SSH session, and SFTP channel.
// Includes keepalive, known_hosts validation, and resume support.
#include "openscp/Libssh2SftpClient.hpp"

#include "../common/SafeLocalFile.hpp"
#include "detail/Libssh2ErrorClassifier.hpp"
#include "detail/Libssh2InputSafety.hpp"
#include "openscp/RuntimeLogging.hpp"

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <limits>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
// OpenSSL RNG for hashed known_hosts hostnames fallback.
#include <openssl/rand.h>
#else
#include <windows.h>
#endif
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/sha.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <sstream>

// POSIX sockets
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>

namespace openscp {

enum class CoreLogLevel : int {
    Off = 0,
    Error = 1,
    Warn = 2,
    Info = 3,
    Debug = 4
};

static CoreLogLevel core_log_level() {
    static const CoreLogLevel level = []() {
        const char *raw = std::getenv("OPENSCP_LOG_LEVEL");
        if (!raw || !*raw)
            return CoreLogLevel::Off;
        std::string v(raw);
        std::transform(v.begin(), v.end(), v.begin(),
                       [](unsigned char c) -> char {
                           return static_cast<char>(std::tolower(c));
                       });
        if (v == "off" || v == "none" || v == "0")
            return CoreLogLevel::Off;
        if (v == "error" || v == "err")
            return CoreLogLevel::Error;
        if (v == "warn" || v == "warning")
            return CoreLogLevel::Warn;
        if (v == "debug" || v == "2")
            return CoreLogLevel::Debug;
        if (v == "info" || v == "1")
            return CoreLogLevel::Info;
        return CoreLogLevel::Off;
    }();
    return level;
}

static bool core_sensitive_debug_enabled() {
    static const bool enabled = openscp::sensitiveLoggingEnabled();
    return enabled;
}

static bool core_log_enabled(CoreLogLevel level) {
    return static_cast<int>(core_log_level()) >= static_cast<int>(level);
}

static void core_logf(CoreLogLevel level, const char *fmt, ...) {
    if (!core_log_enabled(level) || !fmt)
        return;
    std::fprintf(stderr, "[OpenSCP] ");
    va_list ap;
    va_start(ap, fmt);
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
    std::vfprintf(stderr, fmt, ap);
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    va_end(ap);
    std::fprintf(stderr, "\n");
}

// Resolve POSIX home directory robustly (prefer $HOME, fallback to getpwuid)
#ifndef _WIN32
static std::string resolve_posix_home() {
    const char *home = std::getenv("HOME");
    if (home && *home)
        return std::string(home);
    struct passwd *pw = ::getpwuid(::getuid());
    if (pw && pw->pw_dir && *pw->pw_dir)
        return std::string(pw->pw_dir);
    return std::string();
}
#endif

// Append host key audit log lines to ~/.openscp/openscp.auth (0600).
#ifndef _WIN32
static void write_best_effort(int fd, const char *data, std::size_t len) {
    while (len > 0) {
        const ssize_t written = ::write(fd, data, len);
        if (written <= 0)
            return;
        data += static_cast<std::size_t>(written);
        len -= static_cast<std::size_t>(written);
    }
}
#endif

// Best-effort.
static void auditLogHostKey(const std::string &host, uint16_t port,
                            const std::string &algorithm,
                            const std::string &fingerprint,
                            const char *status) {
    if (!openscp::sensitiveLoggingEnabled())
        return;
#ifndef _WIN32
    const char *home = std::getenv("HOME");
    if (!home)
        return;
    std::string dir = std::string(home) + "/.openscp";
    struct ::stat st {};
    if (::stat(dir.c_str(), &st) != 0) {
        (void)::mkdir(dir.c_str(), 0700);
    } else {
        (void)::chmod(dir.c_str(), 0700);
    }
    std::string path = dir + "/openscp.auth";
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0)
        return;
    // Simple timestamp (epoch seconds) for now
    char line[1024];
    std::snprintf(line, sizeof(line),
                  "ts=%ld host=%s port=%u alg=\"%s\" fp=\"%s\" status=%s\n",
                  static_cast<long>(time(nullptr)), host.c_str(),
                  static_cast<unsigned>(port), algorithm.c_str(),
                  fingerprint.c_str(), status ? status : "unknown");
    write_best_effort(fd, line, std::strlen(line));
    ::close(fd);
#else
    (void)host;
    (void)port;
    (void)algorithm;
    (void)fingerprint;
    (void)status;
#endif
}

#ifndef _WIN32
static std::string posix_err(const char *where) {
    return std::string(where) + ": " + std::strerror(errno);
}

static bool ensure_parent_dir_0700(const std::string &path, std::string *why) {
    std::string dir = path;
    std::size_t p = dir.find_last_of('/');
    if (p == std::string::npos)
        dir = ".";
    else
        dir.resize(p);
    struct ::stat st {};
    if (::stat(dir.c_str(), &st) != 0) {
        if (::mkdir(dir.c_str(), 0700) != 0) {
            if (why)
                *why = posix_err("mkdir");
            return false;
        }
    } else if (::chmod(dir.c_str(), 0700) != 0) {
        if (why)
            *why = posix_err("chmod(dir)");
        return false;
    }
    return true;
}

static bool fsync_parent_dir(const std::string &path, std::string *why) {
    std::string dir = path;
    std::size_t p = dir.find_last_of('/');
    if (p == std::string::npos)
        dir = ".";
    else
        dir.resize(p);
    int dfd = ::open(dir.c_str(), O_RDONLY);
    if (dfd < 0) {
        if (why)
            *why = posix_err("open(parent)");
        return false;
    }
    if (::fsync(dfd) != 0) {
        if (why)
            *why = posix_err("fsync(parent)");
        ::close(dfd);
        return false;
    }
    if (::close(dfd) != 0) {
        if (why)
            *why = posix_err("close(parent)");
        return false;
    }
    return true;
}

static bool write_all(int fd, const char *data, std::size_t len,
                      std::string *why) {
    std::size_t off = 0;
    while (off < len) {
        ssize_t w = ::write(fd, data + off, len - off);
        if (w < 0) {
            if (why)
                *why = posix_err("write");
            return false;
        }
        off += static_cast<std::size_t>(w);
    }
    return true;
}
#else
static std::string win_err(const char *where, DWORD code) {
    std::ostringstream oss;
    oss << where << " (GetLastError=" << static_cast<unsigned long>(code)
        << ")";
    return oss.str();
}
#endif

using Sha256Digest = std::array<unsigned char, SHA256_DIGEST_LENGTH>;

static TransferIntegrityPolicy
integrity_policy_from_env(TransferIntegrityPolicy fallback) {
    const char *raw = std::getenv("OPENSCP_TRANSFER_INTEGRITY");
    if (!raw || !*raw)
        return fallback;
    std::string v(raw);
    std::transform(v.begin(), v.end(), v.begin(), [](unsigned char c) -> char {
        return static_cast<char>(std::tolower(c));
    });
    if (v == "off" || v == "0" || v == "false")
        return TransferIntegrityPolicy::Off;
    if (v == "required" || v == "strict")
        return TransferIntegrityPolicy::Required;
    if (v == "optional" || v == "1" || v == "true")
        return TransferIntegrityPolicy::Optional;
    return fallback;
}

// These implementation fragments are order-sensitive code inclusions, not
// header dependencies. Keep clang-format from regrouping them with headers.
// clang-format off
#include "detail/Libssh2SftpClient.TransferSupport.inc"
// clang-format on

static bool persist_known_hosts_atomic(LIBSSH2_KNOWNHOSTS *nh,
                                       const std::string &khPath,
                                       std::string *why) {
    if (!nh) {
        if (why)
            *why = "known_hosts not initialized";
        return false;
    }
    if (khPath.empty()) {
        if (why)
            *why = "empty known_hosts path";
        return false;
    }

#ifndef _WIN32
    if (!ensure_parent_dir_0700(khPath, why))
        return false;
    std::string tmp = khPath + ".tmpXXXXXX";
    std::vector<char> tmpl(tmp.begin(), tmp.end());
    tmpl.push_back('\0');
    int fd = ::mkstemp(tmpl.data());
    if (fd < 0) {
        if (why)
            *why = posix_err("mkstemp");
        return false;
    }
    const std::string tmpPath(tmpl.data());
    if (::close(fd) != 0) {
        if (why)
            *why = posix_err("close(tmp)");
        (void)::unlink(tmpPath.c_str());
        return false;
    }

    if (libssh2_knownhost_writefile(nh, tmpPath.c_str(),
                                    LIBSSH2_KNOWNHOST_FILE_OPENSSH) != 0) {
        if (why)
            *why = "libssh2_knownhost_writefile failed";
        (void)::unlink(tmpPath.c_str());
        return false;
    }

    if (::chmod(tmpPath.c_str(), 0600) != 0) {
        if (why)
            *why = posix_err("chmod(tmp)");
        (void)::unlink(tmpPath.c_str());
        return false;
    }

    int fdw = ::open(tmpPath.c_str(), O_WRONLY);
    if (fdw < 0) {
        if (why)
            *why = posix_err("open(tmp)");
        (void)::unlink(tmpPath.c_str());
        return false;
    }
    if (::fchmod(fdw, 0600) != 0) {
        if (why)
            *why = posix_err("fchmod(tmp)");
        ::close(fdw);
        (void)::unlink(tmpPath.c_str());
        return false;
    }
    if (::fsync(fdw) != 0) {
        if (why)
            *why = posix_err("fsync(tmp)");
        ::close(fdw);
        (void)::unlink(tmpPath.c_str());
        return false;
    }
    if (::close(fdw) != 0) {
        if (why)
            *why = posix_err("close(tmp)");
        (void)::unlink(tmpPath.c_str());
        return false;
    }

    if (::rename(tmpPath.c_str(), khPath.c_str()) != 0) {
        if (why)
            *why = posix_err("rename(tmp->known_hosts)");
        (void)::unlink(tmpPath.c_str());
        return false;
    }
    if (!fsync_parent_dir(khPath, why))
        return false;
    return true;
#else
    std::string tmpPath = khPath + ".tmp";
    if (libssh2_knownhost_writefile(nh, tmpPath.c_str(),
                                    LIBSSH2_KNOWNHOST_FILE_OPENSSH) != 0) {
        if (why)
            *why = "libssh2_knownhost_writefile failed";
        return false;
    }
    HANDLE h = CreateFileA(tmpPath.c_str(), GENERIC_READ | GENERIC_WRITE,
                           FILE_SHARE_READ, NULL, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD ec = GetLastError();
        (void)DeleteFileA(tmpPath.c_str());
        if (why)
            *why = win_err("CreateFile(tmp)", ec);
        return false;
    }
    if (!FlushFileBuffers(h)) {
        DWORD ec = GetLastError();
        CloseHandle(h);
        (void)DeleteFileA(tmpPath.c_str());
        if (why)
            *why = win_err("FlushFileBuffers(tmp)", ec);
        return false;
    }
    if (!CloseHandle(h)) {
        DWORD ec = GetLastError();
        (void)DeleteFileA(tmpPath.c_str());
        if (why)
            *why = win_err("CloseHandle(tmp)", ec);
        return false;
    }
    if (!MoveFileExA(tmpPath.c_str(), khPath.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        DWORD ec = GetLastError();
        (void)DeleteFileA(tmpPath.c_str());
        if (why)
            *why = win_err("MoveFileEx(tmp->known_hosts)", ec);
        return false;
    }
    return true;
#endif
}

#ifndef _WIN32
static bool persist_text_atomic(const std::string &path,
                                const std::string &content, std::string *why) {
    if (path.empty()) {
        if (why)
            *why = "empty destination path";
        return false;
    }
    if (!ensure_parent_dir_0700(path, why))
        return false;

    std::string tmp = path + ".tmpXXXXXX";
    std::vector<char> tmpl(tmp.begin(), tmp.end());
    tmpl.push_back('\0');
    int fd = ::mkstemp(tmpl.data());
    if (fd < 0) {
        if (why)
            *why = posix_err("mkstemp");
        return false;
    }
    const std::string tmpPath(tmpl.data());

    if (!write_all(fd, content.data(), content.size(), why)) {
        ::close(fd);
        (void)::unlink(tmpPath.c_str());
        return false;
    }
    if (::fchmod(fd, 0600) != 0) {
        if (why)
            *why = posix_err("fchmod(tmp)");
        ::close(fd);
        (void)::unlink(tmpPath.c_str());
        return false;
    }
    if (::fsync(fd) != 0) {
        if (why)
            *why = posix_err("fsync(tmp)");
        ::close(fd);
        (void)::unlink(tmpPath.c_str());
        return false;
    }
    if (::close(fd) != 0) {
        if (why)
            *why = posix_err("close(tmp)");
        (void)::unlink(tmpPath.c_str());
        return false;
    }
    if (::rename(tmpPath.c_str(), path.c_str()) != 0) {
        if (why)
            *why = posix_err("rename(tmp->known_hosts)");
        (void)::unlink(tmpPath.c_str());
        return false;
    }
    if (!fsync_parent_dir(path, why))
        return false;
    return true;
}
#else
static bool persist_text_atomic(const std::string &path,
                                const std::string &content, std::string *why) {
    if (path.empty()) {
        if (why)
            *why = "empty destination path";
        return false;
    }

    std::string tmpPath = path + ".tmp";
    HANDLE h = CreateFileA(tmpPath.c_str(), GENERIC_WRITE, 0, NULL,
                           CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        if (why)
            *why = win_err("CreateFile(tmp)", GetLastError());
        return false;
    }

    const char *ptr = content.data();
    std::size_t rem = content.size();
    while (rem > 0) {
        const DWORD chunk = rem > static_cast<std::size_t>(0xFFFFFFFFu)
                                ? static_cast<DWORD>(0xFFFFFFFFu)
                                : static_cast<DWORD>(rem);
        DWORD written = 0;
        if (!WriteFile(h, ptr, chunk, &written, NULL) || written != chunk) {
            DWORD ec = GetLastError();
            (void)CloseHandle(h);
            (void)DeleteFileA(tmpPath.c_str());
            if (why)
                *why = win_err("WriteFile(tmp)", ec);
            return false;
        }
        ptr += written;
        rem -= written;
    }

    if (!FlushFileBuffers(h)) {
        DWORD ec = GetLastError();
        (void)CloseHandle(h);
        (void)DeleteFileA(tmpPath.c_str());
        if (why)
            *why = win_err("FlushFileBuffers(tmp)", ec);
        return false;
    }
    if (!CloseHandle(h)) {
        DWORD ec = GetLastError();
        (void)DeleteFileA(tmpPath.c_str());
        if (why)
            *why = win_err("CloseHandle(tmp)", ec);
        return false;
    }
    if (!MoveFileExA(tmpPath.c_str(), path.c_str(),
                     MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        DWORD ec = GetLastError();
        (void)DeleteFileA(tmpPath.c_str());
        if (why)
            *why = win_err("MoveFileEx(tmp->known_hosts)", ec);
        return false;
    }
    return true;
}
#endif

// Simple Base64 encoder (standard, with '=' padding)
static std::string b64encode(const unsigned char *data, std::size_t len) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= len) {
        const unsigned int v = (static_cast<unsigned int>(data[i]) << 16U) |
                               (static_cast<unsigned int>(data[i + 1]) << 8U) |
                               static_cast<unsigned int>(data[i + 2]);
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back(kTable[v & 0x3F]);
        i += 3;
    }
    if (i + 1 == len) {
        const unsigned int v = static_cast<unsigned int>(data[i]) << 16U;
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == len) {
        const unsigned int v = (static_cast<unsigned int>(data[i]) << 16U) |
                               (static_cast<unsigned int>(data[i + 1]) << 8U);
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

static bool b64decode(const std::string &input,
                      std::vector<unsigned char> &out) {
    if (input.empty() || (input.size() % 4) != 0)
        return false;
    out.assign((input.size() / 4) * 3, 0);
    int decoded = EVP_DecodeBlock(
        out.data(), reinterpret_cast<const unsigned char *>(input.data()),
        static_cast<int>(input.size()));
    if (decoded < 0)
        return false;
    int pad = 0;
    if (!input.empty() && input.back() == '=')
        ++pad;
    if (input.size() > 1 && input[input.size() - 2] == '=')
        ++pad;
    decoded -= pad;
    if (decoded < 0)
        return false;
    out.resize(static_cast<std::size_t>(decoded));
    return true;
}

static bool parse_known_hosts_host_field(const std::string &line,
                                         std::size_t &hostStart,
                                         std::size_t &hostEnd) {
    auto isSpace = [](unsigned char c) { return std::isspace(c) != 0; };
    std::size_t i = 0;
    while (i < line.size() && isSpace(static_cast<unsigned char>(line[i])))
        ++i;
    if (i >= line.size() || line[i] == '#')
        return false;

    auto readFieldEnd = [&](std::size_t from) {
        std::size_t p = from;
        while (p < line.size() && !isSpace(static_cast<unsigned char>(line[p])))
            ++p;
        return p;
    };

    hostStart = i;
    hostEnd = readFieldEnd(i);
    if (hostStart >= hostEnd)
        return false;

    if (line[hostStart] == '@') {
        i = hostEnd;
        while (i < line.size() && isSpace(static_cast<unsigned char>(line[i])))
            ++i;
        if (i >= line.size())
            return false;
        hostStart = i;
        hostEnd = readFieldEnd(i);
        if (hostStart >= hostEnd)
            return false;
    }
    return true;
}

static bool token_matches_hashed_host(const std::string &token,
                                      const std::string &hostToken) {
    if (token.size() <= 4 || token[0] != '|' || token[1] != '1' ||
        token[2] != '|')
        return false;
    const std::size_t sep = token.find('|', 3);
    if (sep == std::string::npos || sep == 3 || sep + 1 >= token.size())
        return false;

    std::vector<unsigned char> salt;
    std::vector<unsigned char> expectedMac;
    if (!b64decode(token.substr(3, sep - 3), salt) ||
        !b64decode(token.substr(sep + 1), expectedMac) || salt.empty() ||
        expectedMac.empty()) {
        return false;
    }

    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int macLen = 0;
    if (!HMAC(EVP_sha1(), salt.data(), static_cast<int>(salt.size()),
              reinterpret_cast<const unsigned char *>(hostToken.data()),
              hostToken.size(), mac, &macLen)) {
        return false;
    }
    if (expectedMac.size() != static_cast<std::size_t>(macLen))
        return false;
    return std::equal(expectedMac.begin(), expectedMac.end(), mac);
}

static bool line_matches_site_host(const std::string &line,
                                   const std::vector<std::string> &targets) {
    std::size_t hostStart = 0;
    std::size_t hostEnd = 0;
    if (!parse_known_hosts_host_field(line, hostStart, hostEnd))
        return false;

    std::size_t pos = hostStart;
    while (pos <= hostEnd) {
        std::size_t comma = line.find(',', pos);
        if (comma == std::string::npos || comma > hostEnd)
            comma = hostEnd;
        const std::string token = line.substr(pos, comma - pos);
        if (std::any_of(targets.begin(), targets.end(),
                        [&token](const std::string &target) {
                            return token == target ||
                                   token_matches_hashed_host(token, target);
                        })) {
            return true;
        }
        if (comma == hostEnd)
            break;
        pos = comma + 1;
    }
    return false;
}

// Global libssh2 initialization (once per process).
static std::once_flag g_libssh2_init_once;
static int g_libssh2_init_result = LIBSSH2_ERROR_NONE;

static int ensure_libssh2_initialized() {
    std::call_once(g_libssh2_init_once,
                   []() { g_libssh2_init_result = libssh2_init(0); });
    return g_libssh2_init_result;
}

#ifndef _WIN32
// Compute OpenSSH hashed hostname token: |1|base64(salt)|base64(HMAC_SHA1(salt,
// host))
static std::string openssh_hash_hostname(const std::string &host) {
    unsigned char salt[20];
    if (RAND_bytes(salt, sizeof(salt)) != 1) {
        // fallback: pseudo-random
        for (size_t i = 0; i < sizeof(salt); ++i)
            salt[i] = static_cast<unsigned char>(rand() & 0xFF);
    }
    unsigned char mac[EVP_MAX_MD_SIZE];
    unsigned int maclen = 0;
    HMAC(EVP_sha1(), salt, static_cast<int>(sizeof(salt)),
         reinterpret_cast<const unsigned char *>(host.data()), host.size(), mac,
         &maclen);
    std::string s_salt = b64encode(salt, sizeof(salt));
    std::string s_mac = b64encode(mac, maclen);
    return std::string("|1|") + s_salt + "|" + s_mac;
}
#endif

// Context for keyboard-interactive: respond with username/password based on the
// prompt
struct KbdIntCtx {
    const char *user;
    const char *pass;
    const KbdIntPromptsCB *cb; // optional: UI callback for prompts
    bool *cancelled;           // explicit user cancellation in UI callback
    bool *rejected;            // malformed, excessive or unallocatable input
    std::string *rejectionError;
};

// Keyboard-interactive callback: respond to prompts with username/password
// based on the text
static void
kbint_password_callback(const char *name, int name_len, const char *instruction,
                        int instruction_len, int num_prompts,
                        const LIBSSH2_USERAUTH_KBDINT_PROMPT *prompts,
                        LIBSSH2_USERAUTH_KBDINT_RESPONSE *responses,
                        void **abstract) {
    if (!abstract || !*abstract)
        return;
    const KbdIntCtx *ctx = static_cast<const KbdIntCtx *>(*abstract);
    const char *user = ctx->user;
    const char *pass = ctx->pass;
    const size_t ulen = user ? std::strlen(user) : 0;
    const size_t plen = pass ? std::strlen(pass) : 0;

    auto rejectPrompts = [&](const std::string &message) {
        if (ctx->rejected)
            *ctx->rejected = true;
        if (ctx->rejectionError)
            *ctx->rejectionError = message;
        if (responses && num_prompts > 0) {
            const int safeCount = std::min(
                num_prompts, libssh2detail::kMaxKeyboardInteractivePrompts);
            for (int i = 0; i < safeCount; ++i) {
                responses[i].text = nullptr;
                responses[i].length = 0;
            }
        }
    };
    if (!responses || num_prompts < 0) {
        rejectPrompts("Keyboard-interactive response storage is invalid.");
        return;
    }
    if (ulen > libssh2detail::kMaxKeyboardInteractiveAnswerBytes ||
        plen > libssh2detail::kMaxKeyboardInteractiveAnswerBytes) {
        rejectPrompts(
            "Keyboard-interactive credentials exceed the safety limit.");
        return;
    }
    if (name_len < 0 || instruction_len < 0 ||
        static_cast<std::size_t>(name_len) >
            libssh2detail::kMaxKeyboardInteractivePromptBytes ||
        static_cast<std::size_t>(instruction_len) >
            libssh2detail::kMaxKeyboardInteractivePromptBytes) {
        rejectPrompts("Keyboard-interactive heading exceeds the safety limit.");
        return;
    }

    std::vector<std::string> promptTexts;
    std::string promptError;
    std::array<libssh2detail::KeyboardInteractivePromptView,
               libssh2detail::kMaxKeyboardInteractivePrompts>
        promptViews{};
    if (num_prompts > libssh2detail::kMaxKeyboardInteractivePrompts) {
        rejectPrompts(
            "Keyboard-interactive prompt count exceeds the safety limit.");
        return;
    }
    if (prompts) {
        for (int i = 0; i < num_prompts; ++i) {
            promptViews[static_cast<std::size_t>(i)] = {prompts[i].text,
                                                        prompts[i].length};
        }
    }
    if (!libssh2detail::copyKeyboardInteractivePrompts(
            prompts ? promptViews.data() : nullptr, num_prompts, promptTexts,
            promptError)) {
        rejectPrompts(promptError);
        return;
    }

    // If a UI callback is provided, give it a chance to answer the prompts.
    if (ctx->cb && *(ctx->cb) && num_prompts > 0) {
        std::vector<std::string> answers;
        KbdIntPromptResult result = KbdIntPromptResult::Unhandled;
        try {
            const std::size_t safeNameLength =
                (name && name_len > 0) ? static_cast<std::size_t>(name_len) : 0;
            const std::size_t safeInstructionLength =
                (instruction && instruction_len > 0)
                    ? static_cast<std::size_t>(instruction_len)
                    : 0;
            const std::string nm(name ? name : "", safeNameLength);
            const std::string ins(instruction ? instruction : "",
                                  safeInstructionLength);
            result = (*(ctx->cb))(nm, ins, promptTexts, answers);
        } catch (...) {
            rejectPrompts(
                "Keyboard-interactive prompt handler failed unexpectedly.");
            return;
        }
        auto scrubAnswers = [&answers]() {
            for (std::string &answer : answers) {
                if (!answer.empty()) {
                    volatile char *bytes = answer.data();
                    for (std::size_t i = 0; i < answer.size(); ++i)
                        bytes[i] = 0;
                    answer.clear();
                }
            }
        };
        if (result == KbdIntPromptResult::Handled &&
            static_cast<int>(answers.size()) >= num_prompts) {
            std::size_t totalAnswerBytes = 0;
            for (int i = 0; i < num_prompts; ++i) {
                const std::size_t answerSize =
                    answers[static_cast<size_t>(i)].size();
                if (answerSize >
                        libssh2detail::kMaxKeyboardInteractiveAnswerBytes ||
                    totalAnswerBytes >
                        libssh2detail::kMaxKeyboardInteractiveAnswerBytes -
                            answerSize) {
                    rejectPrompts("Keyboard-interactive answer exceeds the "
                                  "safety limit.");
                    scrubAnswers();
                    return;
                }
                totalAnswerBytes += answerSize;
            }
            for (int i = 0; i < num_prompts; ++i) {
                const std::string &a = answers[static_cast<size_t>(i)];
                if (a.empty()) {
                    responses[i].text = nullptr;
                    responses[i].length = 0;
                    continue;
                }
                char *buf = static_cast<char *>(std::malloc(a.size() + 1));
                if (!buf) {
                    for (int j = 0; j < i; ++j) {
                        std::free(responses[j].text);
                        responses[j].text = nullptr;
                        responses[j].length = 0;
                    }
                    rejectPrompts(
                        "Could not allocate keyboard-interactive answer data.");
                    scrubAnswers();
                    return;
                }
                std::memcpy(buf, a.data(), a.size());
                buf[a.size()] = '\0';
                responses[i].text = buf;
                responses[i].length = static_cast<unsigned int>(a.size());
            }
            // Best-effort: scrub answers after copying into libssh2 buffers
            scrubAnswers();
            return;
        }
        if (result == KbdIntPromptResult::Cancelled) {
            scrubAnswers();
            if (ctx->cancelled)
                *ctx->cancelled = true;
            for (int i = 0; i < num_prompts; ++i) {
                responses[i].text = nullptr;
                responses[i].length = 0;
            }
            return;
        }
        scrubAnswers();
        // If the callback could not answer, fall back to the simple heuristic
    }
    for (int i = 0; i < num_prompts; ++i) {
        // Simple heuristic: if the prompt mentions "user" or "name", send
        // username; otherwise send password
        const bool wantUser = libssh2detail::promptRequestsUsername(
            promptTexts[static_cast<size_t>(i)]);
        const char *ans = wantUser ? user : pass;
        const size_t alen = wantUser ? ulen : plen;
        if (!ans || alen == 0) {
            responses[i].text = nullptr;
            responses[i].length = 0;
            continue;
        }
        char *buf = static_cast<char *>(std::malloc(alen + 1));
        if (!buf) {
            responses[i].text = nullptr;
            responses[i].length = 0;
            continue;
        }
        std::memcpy(buf, ans, alen);
        buf[alen] = '\0';
        responses[i].text = buf;
        responses[i].length = static_cast<unsigned int>(alen);
    }
}

Libssh2SftpClient::StructuredErrorScope::StructuredErrorScope(
    Libssh2SftpClient &owner, std::string &error, bool mutation)
    : owner_(owner), error_(error), mutation_(mutation) {
    error_.clear();
    owner_.clearLastOperationError();
}

Libssh2SftpClient::StructuredErrorScope::~StructuredErrorScope() {
    if (error_.empty() || owner_.lastOperationError())
        return;
    owner_.setLastOperationError(
        owner_.classifyStructuredFailure(error_, mutation_));
}

Libssh2SftpClient::StructuredErrorScope
Libssh2SftpClient::beginStructuredOperation(std::string &err, bool mutation) {
    return StructuredErrorScope(*this, err, mutation);
}

RemoteError
Libssh2SftpClient::classifyStructuredFailure(const std::string &message,
                                             bool mutation) const {
    return libssh2detail::classifyFailure(message, session_, sftp_, mutation);
}

Libssh2SftpClient::Libssh2SftpClient() = default;

Libssh2SftpClient::~Libssh2SftpClient() {
    disconnect();
}

// These fragments depend on helpers declared by earlier fragments.
// clang-format off
#include "detail/Libssh2SftpClient.TransportNet.inc"
#include "detail/Libssh2SftpClient.TransportAuth.inc"
#include "detail/Libssh2SftpClient.TransportLifecycle.inc"

#include "detail/Libssh2SftpClient.FileListing.inc"

#include "detail/Libssh2SftpClient.TransferOps.inc"

#include "detail/Libssh2SftpClient.FileMetadataOps.inc"
// clang-format on
