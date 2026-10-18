// libssh2 backend: manages TCP socket, SSH session, and SFTP channel.
// Includes keepalive, known_hosts validation, and resume support.
#include "libssh2/Libssh2SftpClient.hpp"

#include "../common/RemoteListingLimits.hpp"
#include "common/UniqueFile.hpp"
#include "detail/KnownHostsFile.hpp"
#include "detail/Libssh2CipherPreference.hpp"
#include "detail/Libssh2ErrorClassifier.hpp"
#include "detail/Libssh2InputSafety.hpp"
#include "detail/Libssh2TransferIntegrity.hpp"
#include "detail/Libssh2WritePacing.hpp"
#include "detail/TcpConnect.hpp"
#include "detail/UniqueSftpHandle.hpp"
#include "openscp/RuntimeLogging.hpp"
#include "openscp/SafeLocalFile.hpp"

#include <libssh2.h>
#include <libssh2_sftp.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <iterator>
#include <limits>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>
#ifndef _WIN32
#include <fcntl.h>
#include <pwd.h>
#include <signal.h>
#include <spawn.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>
#if defined(__APPLE__)
#include <crt_externs.h>
#define openscp_environ (*_NSGetEnviron())
#else
extern char **environ;
#define openscp_environ (environ)
#endif
#else
// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// clang-format on

#include "common/Win32Text.hpp"
#endif
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <openssl/rand.h>
#include <openssl/sha.h>

#include <array>
#include <cstdarg>
#include <cstdio>
#include <ctime>
#include <sstream>

#ifndef _WIN32
// POSIX sockets
#include <arpa/inet.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace openscp {

namespace {

template <typename Container> class CleanseOnScopeExit {
    public:
    explicit CleanseOnScopeExit(Container &container,
                                bool active = true) noexcept
        : container_(container), active_(active) {}

    ~CleanseOnScopeExit() { triggerNow(); }

    void dismiss() noexcept { active_ = false; }

    void triggerNow() noexcept {
        if (active_ && container_.capacity() > 0) {
            OPENSSL_cleanse(container_.data(),
                            container_.capacity() *
                                sizeof(typename Container::value_type));
            active_ = false;
        }
    }

    CleanseOnScopeExit(const CleanseOnScopeExit &) = delete;
    CleanseOnScopeExit &operator=(const CleanseOnScopeExit &) = delete;

    private:
    Container &container_;
    bool active_;
};

enum class CoreLogLevel : int {
    Off = 0,
    Error = 1,
    Warn = 2,
    Info = 3,
    Debug = 4
};

CoreLogLevel core_log_level() {
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

bool core_log_enabled(CoreLogLevel level) {
    return static_cast<int>(core_log_level()) >= static_cast<int>(level);
}

void core_logf(CoreLogLevel level, const char *fmt, ...) {
    if (!core_log_enabled(level) || !fmt)
        return;
    std::string line = "[OpenSCP] ";
    va_list ap;
    va_start(ap, fmt);
    va_list measure;
    va_copy(measure, ap);
#if defined(__clang__)
#pragma clang diagnostic push
#pragma clang diagnostic ignored "-Wformat-nonliteral"
#endif
    const int length = std::vsnprintf(nullptr, 0, fmt, measure);
    if (length > 0) {
        const std::size_t prefix = line.size();
        line.resize(prefix + static_cast<std::size_t>(length));
        std::vsnprintf(line.data() + prefix,
                       static_cast<std::size_t>(length) + 1, fmt, ap);
    }
#if defined(__clang__)
#pragma clang diagnostic pop
#endif
    va_end(measure);
    va_end(ap);
    line += '\n';
    std::fputs(line.c_str(), stderr);
#ifdef _WIN32
    // OpenSCP has no console on Windows, so stderr shows nothing unless it is
    // redirected; debuggers and DebugView show this, as they show Qt's
    // messages.
    ::OutputDebugStringW(win32::toWide(line).c_str());
#endif
}

// Resolve POSIX home directory robustly (prefer $HOME, fallback to getpwuid)
#ifndef _WIN32
std::string resolve_posix_home() {
    const char *home = std::getenv("HOME");
    if (home && *home)
        return std::string(home);
    struct passwd *pw = ::getpwuid(::getuid());
    if (pw && pw->pw_dir && *pw->pw_dir)
        return std::string(pw->pw_dir);
    return std::string();
}
#else
// The profile folder, where OpenSSH for Windows and Qt's QDir::homePath()
// look for .ssh, as UTF-8.
std::string resolve_windows_profile() {
    const wchar_t *profile = ::_wgetenv(L"USERPROFILE");
    if (profile && *profile)
        return win32::toUtf8(profile);
    return std::string();
}
#endif

#ifdef _WIN32
// Reads a private key file into `out`, sized once so no copy of the key is
// left behind in reallocated memory. Keys take a few kilobytes; a file over
// 1 MiB is not one.
bool read_private_key_file(const std::string &path, std::string &out,
                           std::string &why) {
    out.clear();
    UniqueFile file(openLocalFile(path, "rb"));
    if (!file) {
        why = std::string("could not open the private key file: ") +
              std::strerror(errno);
        return false;
    }
    constexpr std::uint64_t kMaximumKeyBytes = 1024 * 1024;
    std::uint64_t size = 0;
    if (!localFileSize(file.get(), size) || size > kMaximumKeyBytes) {
        why = "the private key file is unreadable or too large";
        return false;
    }
    out.resize(static_cast<std::size_t>(size));
    const std::size_t read = std::fread(out.data(), 1, out.size(), file.get());
    if (read != out.size() || std::ferror(file.get()) != 0) {
        why = "could not read the private key file";
        return false;
    }
    return true;
}
#endif

// The known_hosts file OpenSSH uses for the current user, or an empty string
// when the home folder is unknown.
std::string default_known_hosts_path() {
#ifndef _WIN32
    const std::string home = resolve_posix_home();
    return home.empty() ? std::string() : home + "/.ssh/known_hosts";
#else
    const std::string profile = resolve_windows_profile();
    return profile.empty() ? std::string() : profile + "\\.ssh\\known_hosts";
#endif
}

// Append host key audit log lines to ~/.openscp/openscp.auth (0600).
#ifndef _WIN32
void write_best_effort(int fd, const char *data, std::size_t len) {
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
void auditLogHostKey(const std::string &host, uint16_t port,
                     const std::string &algorithm,
                     const std::string &fingerprint, const char *status) {
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
std::string posix_err(const char *where) {
    return std::string(where) + ": " + std::strerror(errno);
}

// Creates the parent folder with mode 0700 when missing. An existing folder
// keeps its permissions: it may be one the user chose and shares.
bool ensure_parent_dir(const std::string &path, std::string *why) {
    std::string dir = path;
    std::size_t p = dir.find_last_of('/');
    if (p == std::string::npos)
        dir = ".";
    else
        dir.resize(p);
    struct ::stat st {};
    if (::stat(dir.c_str(), &st) != 0 && ::mkdir(dir.c_str(), 0700) != 0) {
        if (why)
            *why = posix_err("mkdir");
        return false;
    }
    return true;
}

bool fsync_parent_dir(const std::string &path, std::string *why) {
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

bool write_all(int fd, const char *data, std::size_t len, std::string *why) {
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
#endif

using Sha256Digest = std::array<unsigned char, SHA256_DIGEST_LENGTH>;

TransferIntegrityPolicy
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

// Protect transfer workers from indefinite kernel-level socket blocking when
// the peer/network disappears without a clean TCP close.
void apply_transfer_socket_timeouts(libssh2detail::SocketHandle sock) {
    if (sock == libssh2detail::kInvalidSocket)
        return;
#ifdef _WIN32
    DWORD tvMs = 15000;
    (void)::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                       reinterpret_cast<const char *>(&tvMs), sizeof(tvMs));
    (void)::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                       reinterpret_cast<const char *>(&tvMs), sizeof(tvMs));
#else
    struct timeval tv {};
    tv.tv_sec = 15;
    tv.tv_usec = 0;
    (void)::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    (void)::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
#endif
}

bool seek_local_file(FILE *f, std::uint64_t off, std::string *why) {
    return openscp::libssh2detail::seekLocalFile(f, off, why);
}

bool get_local_file_size(const std::string &path, std::uint64_t &out,
                         std::string *why) {
    return openscp::libssh2detail::getLocalFileSize(path, out, why);
}

void describe_rename_failure(const char *attempt, int rc, unsigned long sftpErr,
                             std::string *why) {
    if (!why)
        return;
    std::ostringstream oss;
    oss << "sftp_rename_ex failed (attempt=" << attempt << ", rc=" << rc
        << ", sftp_err=" << sftpErr << ")";
    if (sftpErr == LIBSSH2_FX_OP_UNSUPPORTED)
        oss << " [server does not support requested rename mode]";
    else if (sftpErr == LIBSSH2_FX_PERMISSION_DENIED)
        oss << " [permission denied]";
    else if (sftpErr == LIBSSH2_FX_FAILURE)
        oss << " [generic failure]";
    *why = oss.str();
}

int rename_sftp_v3(LIBSSH2_SFTP *sftp, const std::string &from,
                   const std::string &to) {
    return libssh2_sftp_rename_ex(
        sftp, from.c_str(), static_cast<unsigned>(from.size()), to.c_str(),
        static_cast<unsigned>(to.size()), 0);
}

bool is_regular_remote_file(LIBSSH2_SFTP *sftp, const std::string &path) {
    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    return libssh2_sftp_stat_ex(sftp, path.c_str(),
                                static_cast<unsigned>(path.size()),
                                LIBSSH2_SFTP_LSTAT, &attrs) == 0 &&
           (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) != 0 &&
           (attrs.permissions & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFREG;
}

// Older libssh2 releases cannot send posix-rename@openssh.com. Move the old
// regular file aside before the v3 rename, and restore it if that rename fails.
// This fallback is not atomic, but never deletes the old file first.
bool rename_remote_with_backup(LIBSSH2_SFTP *sftp, const std::string &from,
                               const std::string &to, std::string *why) {
    std::array<unsigned char, 12> nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        if (why)
            *why = "Could not generate remote replacement backup name";
        return false;
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string backup = to + ".openscp-backup-";
    for (unsigned char byte : nonce) {
        backup.push_back(hex[byte >> 4]);
        backup.push_back(hex[byte & 0x0f]);
    }
    if (backup.size() > std::numeric_limits<unsigned>::max()) {
        if (why)
            *why = "Remote replacement backup path is too long";
        return false;
    }

    LIBSSH2_SFTP_ATTRIBUTES attrs{};
    const int backupStat = libssh2_sftp_stat_ex(
        sftp, backup.c_str(), static_cast<unsigned>(backup.size()),
        LIBSSH2_SFTP_LSTAT, &attrs);
    if (backupStat == 0 ||
        libssh2_sftp_last_error(sftp) != LIBSSH2_FX_NO_SUCH_FILE) {
        if (why)
            *why = "Could not reserve a remote replacement backup path";
        return false;
    }

    const int backupRc = rename_sftp_v3(sftp, to, backup);
    if (backupRc != 0) {
        describe_rename_failure("backup", backupRc,
                                libssh2_sftp_last_error(sftp), why);
        return false;
    }

    const int replaceRc = rename_sftp_v3(sftp, from, to);
    if (replaceRc != 0) {
        const unsigned long replaceErr = libssh2_sftp_last_error(sftp);
        const int restoreRc = rename_sftp_v3(sftp, backup, to);
        describe_rename_failure("replace", replaceRc, replaceErr, why);
        if (restoreRc != 0) {
            if (why)
                *why += " [old destination remains at " + backup + "]";
        } else if (why) {
            *why += " [old destination restored]";
        }
        return false;
    }

    if (libssh2_sftp_unlink_ex(sftp, backup.c_str(),
                               static_cast<unsigned>(backup.size())) != 0) {
        core_logf(CoreLogLevel::Warn,
                  "SFTP replacement succeeded but old backup remains at %s",
                  backup.c_str());
    }
    return true;
}

// libssh2 negotiates SFTP v3, where RENAME carries no flags, so retrying with
// other flag sets only repeats the same request. A v3 RENAME refuses an
// existing destination on OpenSSH. Prefer the atomic posix-rename@openssh.com
// extension when available; otherwise use a backup and v3 rename.
bool rename_remote(LIBSSH2_SFTP *sftp, const std::string &from,
                   const std::string &to, bool overwrite, std::string *why) {
    if (!sftp) {
        if (why)
            *why = "SFTP handle is null";
        return false;
    }

#ifdef libssh2_sftp_posix_rename
    if (overwrite) {
        const int rc = libssh2_sftp_posix_rename_ex(
            sftp, from.c_str(), from.size(), to.c_str(), to.size());
        if (rc == 0)
            return true;
        const unsigned long sftpErr = libssh2_sftp_last_error(sftp);
        // libssh2 returns LIBSSH2_FX_OP_UNSUPPORTED itself when the server did
        // not advertise the extension.
        const bool unsupported =
            rc == static_cast<int>(LIBSSH2_FX_OP_UNSUPPORTED) ||
            (rc == LIBSSH2_ERROR_SFTP_PROTOCOL &&
             sftpErr == LIBSSH2_FX_OP_UNSUPPORTED);
        if (!unsupported) {
            describe_rename_failure("posix-rename", rc, sftpErr, why);
            return false;
        }
    }
#endif

    const long flags = LIBSSH2_SFTP_RENAME_ATOMIC | LIBSSH2_SFTP_RENAME_NATIVE |
                       (overwrite ? LIBSSH2_SFTP_RENAME_OVERWRITE : 0L);
    const int rc = libssh2_sftp_rename_ex(
        sftp, from.c_str(), static_cast<unsigned>(from.size()), to.c_str(),
        static_cast<unsigned>(to.size()), flags);
    if (rc == 0)
        return true;
    const unsigned long sftpErr = libssh2_sftp_last_error(sftp);
    if (overwrite && from != to &&
        (sftpErr == LIBSSH2_FX_FAILURE ||
         sftpErr == LIBSSH2_FX_FILE_ALREADY_EXISTS) &&
        is_regular_remote_file(sftp, from) &&
        is_regular_remote_file(sftp, to)) {
        return rename_remote_with_backup(sftp, from, to, why);
    }
    describe_rename_failure("rename", rc, sftpErr, why);
    return false;
}

bool transfer_cancel_requested(const std::function<bool()> *shouldCancel) {
    return shouldCancel && *shouldCancel && (*shouldCancel)();
}

bool ensure_sftp_ready(bool connected, LIBSSH2_SFTP *sftp, std::string &err) {
    if (connected && sftp)
        return true;
    err = "Not connected";
    return false;
}

bool fail_if_transfer_canceled(const std::function<bool()> *shouldCancel,
                               std::string &err) {
    if (!transfer_cancel_requested(shouldCancel))
        return false;
    err = "Canceled by user";
    return true;
}

// libssh2 splits SFTP reads and writes into 30000-byte requests and pipelines
// as many as the buffer allows (reads ask for up to four buffers ahead), so on
// high-latency links throughput follows the buffer size. At 2 MiB reads reach
// libssh2's read-ahead cap of LIBSSH2_CHANNEL_WINDOW_DEFAULT * 4. Smaller
// transfers get a buffer no larger than themselves so they do not queue reads
// past their end.
std::size_t sftp_transfer_buffer_size(std::uint64_t bytesToTransfer) {
    constexpr std::uint64_t kMinBufferSize = 64 * 1024;
    constexpr std::uint64_t kMaxBufferSize = 2 * 1024 * 1024;
    return static_cast<std::size_t>(
        std::clamp(bytesToTransfer, kMinBufferSize, kMaxBufferSize));
}

bool hash_local_range(const std::string &path, std::uint64_t offset,
                      std::uint64_t length, Sha256Digest &out, std::string *why,
                      const std::function<bool()> *shouldCancel = nullptr) {
    FILE *f = openLocalFile(path, "rb");
    if (!f) {
        if (why)
            *why = "Could not open local file for hashing";
        return false;
    }
    UniqueFile localFile(f);
    if (!seek_local_file(f, offset, why)) {
        return false;
    }
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        if (why)
            *why = "Could not initialize local hash context";
        return false;
    }
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        if (why)
            *why = "EVP_DigestInit_ex(local) failed";
        EVP_MD_CTX_free(ctx);
        return false;
    }
    std::array<unsigned char, 64 * 1024> buf{};
    std::uint64_t remain = length;
    while (remain > 0) {
        if (transfer_cancel_requested(shouldCancel)) {
            if (why)
                *why = "Canceled by user";
            EVP_MD_CTX_free(ctx);
            return false;
        }
        const std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(remain, buf.size()));
        const std::size_t n = std::fread(buf.data(), 1, want, f);
        if (n == 0) {
            if (why)
                *why = "Insufficient local read while hashing";
            EVP_MD_CTX_free(ctx);
            return false;
        }
        if (EVP_DigestUpdate(ctx, buf.data(), n) != 1) {
            if (why)
                *why = "EVP_DigestUpdate(local) failed";
            EVP_MD_CTX_free(ctx);
            return false;
        }
        remain -= static_cast<std::uint64_t>(n);
    }
    unsigned int outLen = 0;
    if (EVP_DigestFinal_ex(ctx, out.data(), &outLen) != 1 ||
        outLen != SHA256_DIGEST_LENGTH) {
        if (why)
            *why = "EVP_DigestFinal_ex(local) failed";
        EVP_MD_CTX_free(ctx);
        return false;
    }
    EVP_MD_CTX_free(ctx);
    return true;
}

bool hash_local_full(const std::string &path, Sha256Digest &out,
                     std::string *why,
                     const std::function<bool()> *shouldCancel = nullptr) {
    std::uint64_t sz = 0;
    if (!get_local_file_size(path, sz, why))
        return false;
    return hash_local_range(path, 0, sz, out, why, shouldCancel);
}

bool hash_remote_range(LIBSSH2_SFTP *sftp, const std::string &remote,
                       std::uint64_t offset, std::uint64_t length,
                       Sha256Digest &out, std::string *why,
                       const std::function<bool()> *shouldCancel = nullptr) {
    LIBSSH2_SFTP_HANDLE *rh = libssh2_sftp_open_ex(
        sftp, remote.c_str(), static_cast<unsigned>(remote.size()),
        LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
    if (!rh) {
        if (why)
            *why = "Could not open remote file for hashing";
        return false;
    }
    libssh2_sftp_seek64(rh, static_cast<libssh2_uint64_t>(offset));
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        if (why)
            *why = "Could not initialize remote hash context";
        libssh2_sftp_close(rh);
        return false;
    }
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        if (why)
            *why = "EVP_DigestInit_ex(remote) failed";
        EVP_MD_CTX_free(ctx);
        libssh2_sftp_close(rh);
        return false;
    }
    std::array<unsigned char, 64 * 1024> buf{};
    std::uint64_t remain = length;
    while (remain > 0) {
        if (transfer_cancel_requested(shouldCancel)) {
            if (why)
                *why = "Canceled by user";
            EVP_MD_CTX_free(ctx);
            libssh2_sftp_close(rh);
            return false;
        }
        const std::size_t want = static_cast<std::size_t>(
            std::min<std::uint64_t>(remain, buf.size()));
        ssize_t n =
            libssh2_sftp_read(rh, reinterpret_cast<char *>(buf.data()), want);
        if (n <= 0) {
            if (why)
                *why = "Insufficient remote read while hashing";
            EVP_MD_CTX_free(ctx);
            libssh2_sftp_close(rh);
            return false;
        }
        if (EVP_DigestUpdate(ctx, buf.data(), static_cast<std::size_t>(n)) !=
            1) {
            if (why)
                *why = "EVP_DigestUpdate(remote) failed";
            EVP_MD_CTX_free(ctx);
            libssh2_sftp_close(rh);
            return false;
        }
        remain -= static_cast<std::uint64_t>(n);
    }
    unsigned int outLen = 0;
    if (EVP_DigestFinal_ex(ctx, out.data(), &outLen) != 1 ||
        outLen != SHA256_DIGEST_LENGTH) {
        if (why)
            *why = "EVP_DigestFinal_ex(remote) failed";
        EVP_MD_CTX_free(ctx);
        libssh2_sftp_close(rh);
        return false;
    }
    EVP_MD_CTX_free(ctx);
    libssh2_sftp_close(rh);
    return true;
}

bool hash_remote_full(
    LIBSSH2_SFTP *sftp, const std::string &remote, Sha256Digest &out,
    std::string *why, const std::function<bool()> *shouldCancel = nullptr,
    const std::function<void(std::size_t, std::size_t)> *progress = nullptr) {
    LIBSSH2_SFTP_HANDLE *rh = libssh2_sftp_open_ex(
        sftp, remote.c_str(), static_cast<unsigned>(remote.size()),
        LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
    if (!rh) {
        if (why)
            *why = "Could not open remote file for full hashing";
        return false;
    }
    EVP_MD_CTX *ctx = EVP_MD_CTX_new();
    if (!ctx) {
        if (why)
            *why = "Could not initialize full remote hash context";
        libssh2_sftp_close(rh);
        return false;
    }
    if (EVP_DigestInit_ex(ctx, EVP_sha256(), nullptr) != 1) {
        if (why)
            *why = "EVP_DigestInit_ex(remote-full) failed";
        EVP_MD_CTX_free(ctx);
        libssh2_sftp_close(rh);
        return false;
    }
    LIBSSH2_SFTP_ATTRIBUTES attributes{};
    const bool hasTotal = libssh2_sftp_fstat_ex(rh, &attributes, 0) == 0 &&
                          (attributes.flags & LIBSSH2_SFTP_ATTR_SIZE) != 0;
    const std::size_t total =
        hasTotal ? static_cast<std::size_t>(attributes.filesize) : 0;
    std::size_t done = 0;
    if (progress && *progress)
        (*progress)(done, total);
    std::vector<unsigned char> buf(sftp_transfer_buffer_size(
        hasTotal ? total : std::numeric_limits<std::uint64_t>::max()));
    while (true) {
        if (transfer_cancel_requested(shouldCancel)) {
            if (why)
                *why = "Canceled by user";
            EVP_MD_CTX_free(ctx);
            libssh2_sftp_close(rh);
            return false;
        }
        ssize_t n = libssh2_sftp_read(rh, reinterpret_cast<char *>(buf.data()),
                                      buf.size());
        if (n > 0) {
            if (EVP_DigestUpdate(ctx, buf.data(),
                                 static_cast<std::size_t>(n)) != 1) {
                if (why)
                    *why = "EVP_DigestUpdate(remote-full) failed";
                EVP_MD_CTX_free(ctx);
                libssh2_sftp_close(rh);
                return false;
            }
            done += static_cast<std::size_t>(n);
            if (progress && *progress)
                (*progress)(done, total);
            continue;
        }
        if (n == 0)
            break;
        if (why)
            *why = "Remote read failed during full hash";
        EVP_MD_CTX_free(ctx);
        libssh2_sftp_close(rh);
        return false;
    }
    unsigned int outLen = 0;
    if (EVP_DigestFinal_ex(ctx, out.data(), &outLen) != 1 ||
        outLen != SHA256_DIGEST_LENGTH) {
        if (why)
            *why = "EVP_DigestFinal_ex(remote-full) failed";
        EVP_MD_CTX_free(ctx);
        libssh2_sftp_close(rh);
        return false;
    }
    EVP_MD_CTX_free(ctx);
    libssh2_sftp_close(rh);
    return true;
}

bool verify_final_transfer_integrity(
    LIBSSH2_SFTP *sftp, const std::string &localPath,
    const std::string &remotePath, TransferIntegrityPolicy policy,
    const char *transferKind, std::string &err,
    const std::function<bool()> *shouldCancel) {
    if (policy == TransferIntegrityPolicy::Off)
        return true;

    Sha256Digest localDigest{}, remoteDigest{};
    std::string hashErr;
    const bool localOk =
        hash_local_full(localPath, localDigest, &hashErr, shouldCancel);
    const bool remoteOk =
        localOk && hash_remote_full(sftp, remotePath, remoteDigest, &hashErr,
                                    shouldCancel);
    if (fail_if_transfer_canceled(shouldCancel, err))
        return false;
    if (!localOk || !remoteOk) {
        if (policy != TransferIntegrityPolicy::Required)
            return true;
        err = std::string("Could not verify final integrity (") + transferKind +
              "): " + hashErr;
        return false;
    }
    if (localDigest != remoteDigest) {
        err = std::string("Final integrity check failed (") + transferKind +
              "): local/remote checksum mismatch";
        return false;
    }
    return true;
}

bool remote_file_changed(const LIBSSH2_SFTP_ATTRIBUTES &before,
                         const LIBSSH2_SFTP_ATTRIBUTES &after) {
    const auto reportedByBoth = [&](unsigned long flag) {
        return (before.flags & flag) != 0 && (after.flags & flag) != 0;
    };
    return (reportedByBoth(LIBSSH2_SFTP_ATTR_SIZE) &&
            before.filesize != after.filesize) ||
           (reportedByBoth(LIBSSH2_SFTP_ATTR_ACMODTIME) &&
            before.mtime != after.mtime);
}

#ifndef _WIN32
bool persist_text_atomic(const std::string &path, const std::string &content,
                         std::string *why) {
    if (path.empty()) {
        if (why)
            *why = "empty destination path";
        return false;
    }
    if (!ensure_parent_dir(path, why))
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
// The same steps as on POSIX, through SafeLocalFile: a new temporary file
// beside known_hosts, flushed to disk, then renamed over it in one step.
bool persist_text_atomic(const std::string &path, const std::string &content,
                         std::string *why) {
    if (path.empty()) {
        if (why)
            *why = "empty destination path";
        return false;
    }
    std::string error;
    const std::size_t separator = path.find_last_of("/\\");
    if (separator != std::string::npos && separator > 0 &&
        !localfiles::ensureLocalDirectories(path.substr(0, separator), error)) {
        if (why)
            *why = error;
        return false;
    }
    std::array<unsigned char, 6> nonce{};
    if (RAND_bytes(nonce.data(), static_cast<int>(nonce.size())) != 1) {
        if (why)
            *why = "Could not generate a temporary file name";
        return false;
    }
    constexpr char hex[] = "0123456789abcdef";
    std::string tmpPath = path + ".tmp";
    for (unsigned char byte : nonce) {
        tmpPath.push_back(hex[byte >> 4]);
        tmpPath.push_back(hex[byte & 0x0f]);
    }
    UniqueFile file(localfiles::openRegularFileForWrite(
        tmpPath, localfiles::WriteMode::Truncate, error));
    bool written = static_cast<bool>(file);
    if (written) {
        written = std::fwrite(content.data(), 1, content.size(), file.get()) ==
                      content.size() &&
                  localfiles::flushAndSync(file.get(), error);
        if (written && file.close() != 0) {
            written = false;
            error = "Could not close temporary known_hosts file";
        }
    }
    if (!written || !localfiles::atomicReplace(tmpPath, path, error)) {
        file.reset();
        std::string cleanupError;
        (void)localfiles::removeLocalPath(tmpPath, false, cleanupError);
        if (why)
            *why = error;
        return false;
    }
    return true;
}
#endif

// Simple Base64 encoder appending directly to output string
void b64encode_append(const unsigned char *data, std::size_t len,
                      std::string &out) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    out.reserve(out.size() + ((len + 2) / 3) * 4);
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
}

// Simple Base64 encoder (standard, with '=' padding)
std::string b64encode(const unsigned char *data, std::size_t len) {
    std::string out;
    b64encode_append(data, len, out);
    return out;
}

bool b64decode(const std::string &input, std::vector<unsigned char> &out) {
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

bool parse_known_hosts_host_field(const std::string &line,
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

bool token_matches_hashed_host(const std::string &token,
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

bool line_matches_site_host(const std::string &line,
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

// The names known_hosts gives `host` and `port`: "host" or "[host]:port".
std::vector<std::string> known_hosts_tokens(const std::string &host,
                                            std::uint16_t port) {
    if (port == 22) {
        // Support this uncommon notation if present in existing files.
        return {host, "[" + host + "]:22"};
    }
    return {"[" + host + "]:" + std::to_string(port)};
}

// The key types known_hosts records for `host` and `port`, under plain or
// hashed names. Lines with a marker, @cert-authority or @revoked, hold no key
// of the host's own.
std::vector<std::string>
recorded_host_key_types(const std::vector<std::string> &lines,
                        const std::string &host, std::uint16_t port) {
    const std::vector<std::string> tokens = known_hosts_tokens(host, port);
    std::vector<std::string> types;
    for (const std::string &line : lines) {
        const std::size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos || line[first] == '@' ||
            !line_matches_site_host(line, tokens)) {
            continue;
        }
        std::size_t hostStart = 0;
        std::size_t hostEnd = 0;
        if (!parse_known_hosts_host_field(line, hostStart, hostEnd))
            continue;
        const std::size_t typeStart = line.find_first_not_of(" \t", hostEnd);
        if (typeStart == std::string::npos)
            continue;
        const std::size_t typeEnd = line.find_first_of(" \t", typeStart);
        types.push_back(line.substr(typeStart, typeEnd == std::string::npos
                                                   ? std::string::npos
                                                   : typeEnd - typeStart));
    }
    return types;
}

// Global libssh2 initialization (once per process).
std::once_flag g_libssh2_init_once;
int g_libssh2_init_result = LIBSSH2_ERROR_NONE;

int ensure_libssh2_initialized() {
    std::call_once(g_libssh2_init_once,
                   []() { g_libssh2_init_result = libssh2_init(0); });
    return g_libssh2_init_result;
}

// Context for keyboard-interactive: the saved credentials and the optional
// UI callback that answers the rest.
struct KbdIntCtx {
    const char *user;
    const char *pass;
    const KbdIntPromptsCB *cb; // optional: UI callback for prompts
    bool *cancelled;           // explicit user cancellation in UI callback
    bool *rejected;            // malformed, excessive or unallocatable input
    std::string *rejectionError;
    int *rounds; // requests with prompts answered so far
};

// Keyboard-interactive callback: the saved username and password answer the
// prompts that clearly ask for them, and the UI callback the others.
void kbint_password_callback(const char *name, int name_len,
                             const char *instruction, int instruction_len,
                             int num_prompts,
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
            auto &promptView = promptViews[static_cast<std::size_t>(i)];
            // libssh2 1.10 exposed prompt text as char*, while newer releases
            // use unsigned char*. Normalize both public API variants here.
            promptView.text =
                reinterpret_cast<const unsigned char *>(prompts[i].text);
            promptView.length = static_cast<std::size_t>(prompts[i].length);
        }
    }
    if (!libssh2detail::copyKeyboardInteractivePrompts(
            prompts ? promptViews.data() : nullptr, num_prompts, promptTexts,
            promptError)) {
        rejectPrompts(promptError);
        return;
    }

    if (num_prompts > 0 && ctx->rounds)
        ++*ctx->rounds;
    const int round = ctx->rounds ? *ctx->rounds : 1;

    auto scrubAnswers = [](std::vector<std::string> &values) {
        for (std::string &value : values) {
            if (value.capacity() > 0) {
                OPENSSL_cleanse(value.data(), value.capacity());
                value.clear();
            }
        }
    };

    // The saved credentials answer what they clearly match; the user answers
    // the rest, so a one-time code never receives the account password.
    std::vector<std::string> answers;
    std::vector<KbdIntPrompt> userPrompts;
    std::vector<int> userPromptIndexes;
    try {
        answers.resize(static_cast<std::size_t>(num_prompts));
        for (int i = 0; i < num_prompts; ++i) {
            KbdIntPrompt prompt{std::move(promptTexts[static_cast<size_t>(i)]),
                                prompts && prompts[i].echo != 0};
            const auto saved = libssh2detail::savedKbdIntAnswer(prompt, round);
            std::string &answer = answers[static_cast<size_t>(i)];
            if (saved == libssh2detail::KbdIntSavedAnswer::Username &&
                ulen > 0) {
                answer.assign(user, ulen);
            } else if (saved == libssh2detail::KbdIntSavedAnswer::Password &&
                       plen > 0) {
                answer.assign(pass, plen);
            } else {
                userPrompts.push_back(std::move(prompt));
                userPromptIndexes.push_back(i);
            }
        }
    } catch (...) {
        scrubAnswers(answers);
        rejectPrompts("Could not allocate keyboard-interactive answer data.");
        return;
    }

    if (!userPrompts.empty()) {
        if (!ctx->cb || !*(ctx->cb)) {
            scrubAnswers(answers);
            rejectPrompts("The server asked for a one-time code or other "
                          "input that only the main connection can answer.");
            return;
        }
        std::vector<std::string> userAnswers;
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
            result = (*(ctx->cb))(nm, ins, userPrompts, userAnswers);
        } catch (...) {
            scrubAnswers(answers);
            scrubAnswers(userAnswers);
            rejectPrompts(
                "Keyboard-interactive prompt handler failed unexpectedly.");
            return;
        }
        if (result == KbdIntPromptResult::Cancelled) {
            scrubAnswers(answers);
            scrubAnswers(userAnswers);
            if (ctx->cancelled)
                *ctx->cancelled = true;
            for (int i = 0; i < num_prompts; ++i) {
                responses[i].text = nullptr;
                responses[i].length = 0;
            }
            return;
        }
        if (result != KbdIntPromptResult::Handled ||
            userAnswers.size() < userPrompts.size()) {
            scrubAnswers(answers);
            scrubAnswers(userAnswers);
            rejectPrompts("Keyboard-interactive prompts were left unanswered.");
            return;
        }
        for (std::size_t k = 0; k < userPrompts.size(); ++k) {
            answers[static_cast<size_t>(userPromptIndexes[k])].swap(
                userAnswers[k]);
        }
        scrubAnswers(userAnswers);
    }

    std::size_t totalAnswerBytes = 0;
    for (const std::string &answer : answers) {
        if (answer.size() > libssh2detail::kMaxKeyboardInteractiveAnswerBytes ||
            totalAnswerBytes >
                libssh2detail::kMaxKeyboardInteractiveAnswerBytes -
                    answer.size()) {
            scrubAnswers(answers);
            rejectPrompts("Keyboard-interactive answer exceeds the safety "
                          "limit.");
            return;
        }
        totalAnswerBytes += answer.size();
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
            scrubAnswers(answers);
            rejectPrompts(
                "Could not allocate keyboard-interactive answer data.");
            return;
        }
        std::memcpy(buf, a.data(), a.size());
        buf[a.size()] = '\0';
        responses[i].text = buf;
        responses[i].length = static_cast<unsigned int>(a.size());
    }
    // Best-effort: scrub answers after copying into libssh2 buffers
    scrubAnswers(answers);
}

} // namespace

Libssh2SftpClient::StructuredErrorScope::StructuredErrorScope(
    Libssh2SftpClient &owner, std::string &error, bool mutation)
    : owner_(owner), error_(error), mutation_(mutation),
      ioLock_(owner.ioMutex_) {
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

namespace {

using libssh2detail::kInvalidSocket;
using libssh2detail::SocketHandle;

// Winsock declares the option value as const char *.
void set_int_socket_option(SocketHandle s, int level, int name, int value) {
    (void)::setsockopt(s, level, name, reinterpret_cast<const char *>(&value),
                       sizeof(value));
}

void configure_tcp_keepalive(SocketHandle s) {
    // Enable TCP keepalive
    set_int_socket_option(s, SOL_SOCKET, SO_KEEPALIVE, 1);
#ifdef __APPLE__
    set_int_socket_option(s, IPPROTO_TCP, TCP_KEEPALIVE, 60);
#elif defined(__linux__) || defined(_WIN32)
    // Windows accepts these options since Windows 10 1709.
    set_int_socket_option(s, IPPROTO_TCP, TCP_KEEPIDLE, 60);
    set_int_socket_option(s, IPPROTO_TCP, TCP_KEEPINTVL, 10);
    set_int_socket_option(s, IPPROTO_TCP, TCP_KEEPCNT, 3);
#endif
}

// libssh2 sends each SSH packet with its own send() and queues SFTP read
// requests back to back, so with Nagle's algorithm every request after the
// first would wait for the server to acknowledge the one before it.
void disable_nagle(SocketHandle s) {
    set_int_socket_option(s, IPPROTO_TCP, TCP_NODELAY, 1);
}

bool set_socket_timeout_ms(SocketHandle sock, int timeoutMs) {
    if (sock == kInvalidSocket)
        return false;
    if (timeoutMs < 0)
        timeoutMs = 0;
#ifdef _WIN32
    DWORD tvMs = static_cast<DWORD>(timeoutMs);
    const int r1 =
        ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                     reinterpret_cast<const char *>(&tvMs), sizeof(tvMs));
    const int r2 =
        ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                     reinterpret_cast<const char *>(&tvMs), sizeof(tvMs));
    return r1 == 0 && r2 == 0;
#else
    struct timeval tv {};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    const int r1 = ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const int r2 = ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return r1 == 0 && r2 == 0;
#endif
}

bool socket_send_all(SocketHandle sock, const unsigned char *data,
                     std::size_t len, const char *stage, std::string &err) {
    // interrupt() shuts the socket down from another thread, and a send after
    // that raises SIGPIPE, which ends the process unless it is suppressed;
    // sendOnce() suppresses it.
    // send(2) may write partially; loop until the full buffer is sent.
    std::size_t off = 0;
    while (off < len) {
        const std::ptrdiff_t w =
            libssh2detail::sendOnce(sock, data + off, len - off);
        if (w < 0) {
            const int sendError = libssh2detail::lastSocketError();
            if (libssh2detail::isInterruptedSocketError(sendError))
                continue;
            err = std::string(stage) + " send failed: " +
                  libssh2detail::socketErrorMessage(sendError);
            return false;
        }
        if (w == 0) {
            err = std::string(stage) + " send failed: connection closed";
            return false;
        }
        off += static_cast<std::size_t>(w);
    }
    return true;
}

bool socket_recv_exact(SocketHandle sock, unsigned char *buf, std::size_t len,
                       const char *stage, std::string &err) {
    // recv(2) may return partial data; loop until we fill the requested bytes.
    std::size_t off = 0;
    while (off < len) {
        const std::ptrdiff_t r =
            libssh2detail::receiveOnce(sock, buf + off, len - off);
        if (r < 0) {
            const int receiveError = libssh2detail::lastSocketError();
            if (libssh2detail::isInterruptedSocketError(receiveError))
                continue;
            err = std::string(stage) + " recv failed: " +
                  libssh2detail::socketErrorMessage(receiveError);
            return false;
        }
        if (r == 0) {
            err = std::string(stage) + " recv failed: connection closed";
            return false;
        }
        off += static_cast<std::size_t>(r);
    }
    return true;
}

// Reads up to the delimiter one byte at a time: what follows it belongs to the
// tunneled protocol, such as the SSH server's banner, and must stay in the
// socket.
bool socket_recv_until(SocketHandle sock, const std::string &delimiter,
                       std::size_t maxBytes, const char *stage,
                       std::string &out, std::string &err) {
    out.clear();
    if (delimiter.empty()) {
        err = std::string(stage) + " invalid delimiter";
        return false;
    }
    while (!out.ends_with(delimiter)) {
        char byte = 0;
        const std::ptrdiff_t r = libssh2detail::receiveOnce(sock, &byte, 1);
        if (r < 0) {
            const int receiveError = libssh2detail::lastSocketError();
            if (libssh2detail::isInterruptedSocketError(receiveError))
                continue;
            err = std::string(stage) + " recv failed: " +
                  libssh2detail::socketErrorMessage(receiveError);
            return false;
        }
        if (r == 0) {
            err = std::string(stage) + " recv failed: connection closed";
            return false;
        }
        out.push_back(byte);
        if (out.size() > maxBytes) {
            err = std::string(stage) + " too large";
            return false;
        }
    }
    return true;
}

bool connect_tcp_endpoint(const std::string &host, uint16_t port,
                          SocketHandle &sockOut, std::string &err,
                          std::stop_token stopToken) {
    sockOut = kInvalidSocket;
    if (stopToken.stop_requested()) {
        err = "Connection canceled by user";
        return false;
    }
    if (!libssh2detail::initializeSockets(err))
        return false;

    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%u", static_cast<unsigned>(port));

#ifdef _WIN32
    // getaddrinfo() reads the name in the system code page, which is UTF-8
    // only from Windows 10 1903 on. GetAddrInfoW takes UTF-16, and encodes an
    // internationalized name itself.
    std::wstring wideHost;
    if (!win32::toWidePath(host, wideHost)) {
        err = "Host name is not valid UTF-8.";
        return false;
    }
    ADDRINFOW hints{};
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    ADDRINFOW *res = nullptr;
    const int gai = ::GetAddrInfoW(
        wideHost.c_str(), win32::toWide(portStr).c_str(), &hints, &res);
    if (gai != 0) {
        // Windows returns a Winsock error code, which has a system message.
        err = "getaddrinfo: " + libssh2detail::socketErrorMessage(gai);
        return false;
    }
    const auto freeAddresses = [](ADDRINFOW *addresses) {
        ::FreeAddrInfoW(addresses);
    };
#else
    struct addrinfo hints {};
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = nullptr;
    const int gai = getaddrinfo(host.c_str(), portStr, &hints, &res);
    if (gai != 0) {
        err = std::string("getaddrinfo: ") + gai_strerror(gai);
        return false;
    }
    const auto freeAddresses = [](addrinfo *addresses) {
        freeaddrinfo(addresses);
    };
#endif

    std::string lastConnectErr;
    // Resolution uses the platform resolver; the TCP budget starts afterward.
    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(20);
    // Try all resolved addresses (IPv4/IPv6) until one succeeds.
    for (auto rp = res; rp != nullptr; rp = rp->ai_next) {
        if (stopToken.stop_requested() ||
            std::chrono::steady_clock::now() >= deadline)
            break;
        const SocketHandle s = libssh2detail::openSocket(
            rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s == kInvalidSocket) {
            lastConnectErr = libssh2detail::socketErrorMessage(
                libssh2detail::lastSocketError());
            continue;
        }
        configure_tcp_keepalive(s);
        disable_nagle(s);
        if (!libssh2detail::setSocketNonBlocking(s, true)) {
            lastConnectErr = libssh2detail::socketErrorMessage(
                libssh2detail::lastSocketError());
            libssh2detail::closeSocket(s);
            continue;
        }
        const int result =
            ::connect(s, rp->ai_addr, static_cast<socklen_t>(rp->ai_addrlen));
        const int connectError =
            result == 0 ? 0 : libssh2detail::lastSocketError();
        bool connected = result == 0;
        if (!connected &&
            libssh2detail::isConnectInProgressError(connectError)) {
            connected = libssh2detail::waitForTcpConnection(
                s, deadline, stopToken, lastConnectErr);
        } else if (!connected) {
            lastConnectErr = libssh2detail::socketErrorMessage(connectError);
        }
        if (connected && !libssh2detail::setSocketNonBlocking(s, false)) {
            connected = false;
            lastConnectErr = libssh2detail::socketErrorMessage(
                libssh2detail::lastSocketError());
        }
        if (connected && !stopToken.stop_requested()) {
            sockOut = s;
            freeAddresses(res);
            return true;
        }
        libssh2detail::closeSocket(s);
    }
    freeAddresses(res);
    if (stopToken.stop_requested()) {
        err = "Connection canceled by user";
        return false;
    }
    if (std::chrono::steady_clock::now() >= deadline) {
        err = "TCP connection timed out";
        return false;
    }
    err = "Could not connect to host/port";
    if (!lastConnectErr.empty())
        err += ": " + lastConnectErr;
    err += ".";
    return false;
}

const char *socks5_reply_text(unsigned char rep) {
    switch (rep) {
    case 0x00:
        return "succeeded";
    case 0x01:
        return "general failure";
    case 0x02:
        return "connection not allowed by ruleset";
    case 0x03:
        return "network unreachable";
    case 0x04:
        return "host unreachable";
    case 0x05:
        return "connection refused";
    case 0x06:
        return "TTL expired";
    case 0x07:
        return "command not supported";
    case 0x08:
        return "address type not supported";
    default:
        break;
    }
    return "unknown error";
}

std::string format_host_port_authority(const std::string &host,
                                       std::uint16_t port) {
    if (host.find(':') != std::string::npos &&
        host.find(']') == std::string::npos)
        return "[" + host + "]:" + std::to_string(port);
    return host + ":" + std::to_string(port);
}

#ifndef _WIN32
std::string trim_ascii_whitespace(std::string value) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(),
                value.end());
    return value;
}

std::string read_jump_tunnel_stderr(int fd) {
    if (fd < 0)
        return std::string();

    std::string out;
    out.reserve(256);
    char buf[256];
    while (out.size() < 4096) {
        const std::size_t remaining = 4096 - out.size();
        const ssize_t n =
            ::read(fd, buf, remaining < sizeof(buf) ? remaining : sizeof(buf));
        if (n > 0) {
            out.append(buf, static_cast<std::size_t>(n));
            continue;
        }
        if (n == 0)
            break;
        if (errno == EINTR)
            continue;
        if (errno == EAGAIN || errno == EWOULDBLOCK)
            break;
        break;
    }
    return trim_ascii_whitespace(out);
}

std::string format_jump_tunnel_exit_error(const char *prefix,
                                          const std::string &stderrText) {
    std::string out(prefix ? prefix : "SSH jump tunnel failed.");
    if (!stderrText.empty()) {
        if (!out.empty() && out.back() != '.' && out.back() != ':')
            out += ":";
        out += " " + stderrText;
    }
    return out;
}

bool spawn_ssh_jump_tunnel(const SessionOptions &opt, int &sockOut, int &pidOut,
                           int &stderrFdOut, std::string &err) {
    sockOut = -1;
    pidOut = -1;
    stderrFdOut = -1;
    if (!opt.jump_host || opt.jump_host->empty()) {
        err = "SSH jump host is empty.";
        return false;
    }
    if (opt.jump_port == 0) {
        err = "SSH jump host port is invalid.";
        return false;
    }
    if (opt.host.empty()) {
        err = "SSH target host is empty.";
        return false;
    }

    int pairfd[2] = {-1, -1};
    if (::socketpair(AF_UNIX, SOCK_STREAM, 0, pairfd) != 0) {
        err = std::string("socketpair failed: ") + std::strerror(errno);
        return false;
    }

    int stderrPipe[2] = {-1, -1};
    if (::pipe(stderrPipe) != 0) {
        err = std::string("pipe failed: ") + std::strerror(errno);
        ::close(pairfd[0]);
        ::close(pairfd[1]);
        return false;
    }

    std::vector<std::string> args;
    args.reserve(24);
    args.emplace_back("ssh");
    args.emplace_back("-o");
    args.emplace_back("BatchMode=yes");
    args.emplace_back("-o");
    args.emplace_back("IdentitiesOnly=yes");
    args.emplace_back("-o");
    args.emplace_back("ExitOnForwardFailure=yes");
    args.emplace_back("-o");
    args.emplace_back("ConnectTimeout=20");
    args.emplace_back("-o");
    args.emplace_back("ServerAliveInterval=30");
    args.emplace_back("-o");
    args.emplace_back("ServerAliveCountMax=2");
    if (opt.jump_username && !opt.jump_username->empty()) {
        args.emplace_back("-l");
        args.emplace_back(*opt.jump_username);
    }
    if (opt.jump_private_key_path && !opt.jump_private_key_path->empty()) {
        args.emplace_back("-i");
        args.emplace_back(*opt.jump_private_key_path);
    }
    args.emplace_back("-p");
    args.emplace_back(std::to_string(opt.jump_port));
    args.emplace_back("-W");
    args.emplace_back(format_host_port_authority(opt.host, opt.port));
    // Whatever the destination looks like, it is not an option.
    args.emplace_back("--");
    args.emplace_back(*opt.jump_host);

    std::vector<char *> argv;
    argv.reserve(args.size() + 1);
    for (const auto &a : args) {
        argv.push_back(const_cast<char *>(a.c_str()));
    }
    argv.push_back(nullptr);

    posix_spawn_file_actions_t actions;
    if (const int actionErr = posix_spawn_file_actions_init(&actions);
        actionErr != 0) {
        err = std::string("posix_spawn_file_actions_init failed: ") +
              std::strerror(actionErr);
        ::close(pairfd[0]);
        ::close(pairfd[1]);
        ::close(stderrPipe[0]);
        ::close(stderrPipe[1]);
        return false;
    }

    struct ActionsGuard {
        posix_spawn_file_actions_t *act;
        ~ActionsGuard() { posix_spawn_file_actions_destroy(act); }
    } actionsGuard{&actions};

    posix_spawn_file_actions_adddup2(&actions, pairfd[1], STDIN_FILENO);
    posix_spawn_file_actions_adddup2(&actions, pairfd[1], STDOUT_FILENO);
    posix_spawn_file_actions_adddup2(&actions, stderrPipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&actions, pairfd[0]);
    posix_spawn_file_actions_addclose(&actions, stderrPipe[0]);
    if (pairfd[1] > STDERR_FILENO)
        posix_spawn_file_actions_addclose(&actions, pairfd[1]);
    if (stderrPipe[1] > STDERR_FILENO)
        posix_spawn_file_actions_addclose(&actions, stderrPipe[1]);

    pid_t pid = -1;
    const int spawnRes = posix_spawnp(&pid, "ssh", &actions, nullptr,
                                      argv.data(), openscp_environ);

    ::close(pairfd[1]);
    ::close(stderrPipe[1]);

    if (spawnRes != 0) {
        err = std::string("posix_spawnp failed: ") + std::strerror(spawnRes);
        ::close(pairfd[0]);
        ::close(stderrPipe[0]);
        return false;
    }

    const int fdFlags = ::fcntl(pairfd[0], F_GETFD);
    if (fdFlags >= 0)
        (void)::fcntl(pairfd[0], F_SETFD, fdFlags | FD_CLOEXEC);
    const int stderrFdFlags = ::fcntl(stderrPipe[0], F_GETFD);
    if (stderrFdFlags >= 0)
        (void)::fcntl(stderrPipe[0], F_SETFD, stderrFdFlags | FD_CLOEXEC);
    const int stderrStatusFlags = ::fcntl(stderrPipe[0], F_GETFL);
    if (stderrStatusFlags >= 0)
        (void)::fcntl(stderrPipe[0], F_SETFL, stderrStatusFlags | O_NONBLOCK);

    std::this_thread::sleep_for(std::chrono::milliseconds(40));
    int status = 0;
    const pid_t wr = ::waitpid(pid, &status, WNOHANG);
    if (wr == pid) {
        ::close(pairfd[0]);
        const std::string stderrText = read_jump_tunnel_stderr(stderrPipe[0]);
        ::close(stderrPipe[0]);
        err = format_jump_tunnel_exit_error(
            "Failed to start SSH jump tunnel process.", stderrText);
        return false;
    }

    sockOut = pairfd[0];
    pidOut = static_cast<int>(pid);
    stderrFdOut = stderrPipe[0];
    return true;
}

void stop_ssh_jump_tunnel(int pid) {
    if (pid <= 0)
        return;

    int status = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const pid_t wr = ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
        if (wr == static_cast<pid_t>(pid) || (wr == -1 && errno == ECHILD)) {
            return;
        }
        if (attempt == 0) {
            (void)::kill(static_cast<pid_t>(pid), SIGTERM);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(60));
    }
    (void)::kill(static_cast<pid_t>(pid), SIGKILL);
    while (true) {
        const pid_t wr = ::waitpid(static_cast<pid_t>(pid), &status, 0);
        if (wr == static_cast<pid_t>(pid) || (wr == -1 && errno == ECHILD)) {
            return;
        }
        if (wr == -1 && errno != EINTR)
            return;
    }
}
#endif

bool establish_socks5_tunnel(SocketHandle sock, const SessionOptions &opt,
                             std::string &err) {
    const bool haveProxyUser =
        opt.proxy_username.has_value() && !opt.proxy_username->empty();
    const bool haveProxyPass =
        opt.proxy_password.has_value() && !opt.proxy_password->empty();
    const bool wantProxyAuth = haveProxyUser || haveProxyPass;

    std::vector<unsigned char> greeting;
    if (wantProxyAuth)
        greeting = {0x05, 0x02, 0x00, 0x02};
    else
        greeting = {0x05, 0x01, 0x00};

    if (!socket_send_all(sock, greeting.data(), greeting.size(),
                         "SOCKS5 greeting", err)) {
        return false;
    }

    unsigned char helloResp[2] = {0, 0};
    if (!socket_recv_exact(sock, helloResp, sizeof(helloResp),
                           "SOCKS5 greeting", err)) {
        return false;
    }
    if (helloResp[0] != 0x05) {
        err = "SOCKS5 proxy returned invalid greeting version.";
        return false;
    }
    if (helloResp[1] == 0xFF) {
        err = "SOCKS5 proxy has no compatible authentication method.";
        return false;
    }

    if (helloResp[1] == 0x02) {
        const std::string user = haveProxyUser ? *opt.proxy_username : "";
        const std::string_view pass =
            haveProxyPass ? opt.proxy_password->view() : std::string_view();
        if (user.empty()) {
            err = "SOCKS5 proxy authentication requires username.";
            return false;
        }
        if (user.size() > 255 || pass.size() > 255) {
            err = "SOCKS5 proxy credentials are too long.";
            return false;
        }
        std::vector<unsigned char> authReq;
        authReq.reserve(3 + user.size() + pass.size());
        CleanseOnScopeExit authCleanse(authReq);
        authReq.push_back(0x01);
        authReq.push_back(static_cast<unsigned char>(user.size()));
        authReq.insert(authReq.end(), user.begin(), user.end());
        authReq.push_back(static_cast<unsigned char>(pass.size()));
        authReq.insert(authReq.end(), pass.begin(), pass.end());
        const bool sent = socket_send_all(sock, authReq.data(), authReq.size(),
                                          "SOCKS5 auth", err);
        authCleanse.triggerNow();
        if (!sent) {
            return false;
        }
        unsigned char authResp[2] = {0, 0};
        if (!socket_recv_exact(sock, authResp, sizeof(authResp), "SOCKS5 auth",
                               err)) {
            return false;
        }
        if (authResp[1] != 0x00) {
            err = "SOCKS5 proxy authentication failed.";
            return false;
        }
    } else if (helloResp[1] != 0x00) {
        err = "SOCKS5 proxy returned unsupported auth method.";
        return false;
    }

    if (opt.host.empty()) {
        err = "SOCKS5 target host is empty.";
        return false;
    }
    std::vector<unsigned char> connectReq = {0x05, 0x01, 0x00};
    std::array<unsigned char, 16> ipbuf{};
    if (::inet_pton(AF_INET, opt.host.c_str(), ipbuf.data()) == 1) {
        connectReq.push_back(0x01);
        connectReq.insert(connectReq.end(), ipbuf.begin(), ipbuf.begin() + 4);
    } else if (::inet_pton(AF_INET6, opt.host.c_str(), ipbuf.data()) == 1) {
        connectReq.push_back(0x04);
        connectReq.insert(connectReq.end(), ipbuf.begin(), ipbuf.end());
    } else {
        if (opt.host.size() > 255) {
            err = "SOCKS5 target hostname is too long.";
            return false;
        }
        connectReq.push_back(0x03);
        connectReq.push_back(static_cast<unsigned char>(opt.host.size()));
        connectReq.insert(connectReq.end(), opt.host.begin(), opt.host.end());
    }
    connectReq.push_back(static_cast<unsigned char>((opt.port >> 8) & 0xFF));
    connectReq.push_back(static_cast<unsigned char>(opt.port & 0xFF));

    if (!socket_send_all(sock, connectReq.data(), connectReq.size(),
                         "SOCKS5 connect", err)) {
        return false;
    }

    unsigned char connHead[4] = {0, 0, 0, 0};
    if (!socket_recv_exact(sock, connHead, sizeof(connHead), "SOCKS5 connect",
                           err)) {
        return false;
    }
    if (connHead[0] != 0x05) {
        err = "SOCKS5 proxy returned invalid connect version.";
        return false;
    }
    if (connHead[1] != 0x00) {
        err = std::string("SOCKS5 proxy connect failed: ") +
              socks5_reply_text(connHead[1]) + ".";
        return false;
    }

    std::size_t tail = 0;
    if (connHead[3] == 0x01) {
        tail = 4 + 2;
    } else if (connHead[3] == 0x04) {
        tail = 16 + 2;
    } else if (connHead[3] == 0x03) {
        unsigned char nameLen = 0;
        if (!socket_recv_exact(sock, &nameLen, 1, "SOCKS5 connect", err))
            return false;
        tail = static_cast<std::size_t>(nameLen) + 2;
    } else {
        err = "SOCKS5 proxy returned unsupported bind address type.";
        return false;
    }
    std::vector<unsigned char> sink(tail);
    if (tail > 0 && !socket_recv_exact(sock, sink.data(), sink.size(),
                                       "SOCKS5 connect", err)) {
        return false;
    }
    return true;
}

bool establish_http_connect_tunnel(SocketHandle sock, const SessionOptions &opt,
                                   std::string &err) {
    if (opt.host.empty()) {
        err = "HTTP CONNECT target host is empty.";
        return false;
    }
    const std::string authority =
        format_host_port_authority(opt.host, opt.port);

    const bool hasProxyAuth =
        (opt.proxy_username && !opt.proxy_username->empty()) ||
        (opt.proxy_password && !opt.proxy_password->empty());

    std::string req;
    req.reserve(256 + (hasProxyAuth ? 128 : 0));
    // Automatically wipe any sensitive Authorization header from RAM upon scope
    // exit
    CleanseOnScopeExit reqCleanse(req, hasProxyAuth);

    req += "CONNECT ";
    req += authority;
    req += " HTTP/1.1\r\n";
    req += "Host: ";
    req += authority;
    req += "\r\n";
    req += "Proxy-Connection: Keep-Alive\r\n";
    req += "User-Agent: OpenSCP\r\n";

    if (hasProxyAuth) {
        const std::string user =
            opt.proxy_username ? *opt.proxy_username : std::string();
        std::string creds = user + ":";
        if (opt.proxy_password) {
            creds.append(opt.proxy_password->data(),
                         opt.proxy_password->size());
        }
        CleanseOnScopeExit credsCleanse(creds);

        req += "Proxy-Authorization: Basic ";
        b64encode_append(reinterpret_cast<const unsigned char *>(creds.data()),
                         creds.size(), req);
        req += "\r\n";

        credsCleanse.triggerNow();
    }
    req += "\r\n";

    if (!socket_send_all(sock,
                         reinterpret_cast<const unsigned char *>(req.data()),
                         req.size(), "HTTP CONNECT", err)) {
        return false;
    }

    std::string response;
    if (!socket_recv_until(sock, "\r\n\r\n", 8192, "HTTP CONNECT", response,
                           err)) {
        return false;
    }
    const std::size_t eol = response.find("\r\n");
    const std::string statusLine =
        (eol == std::string::npos) ? response : response.substr(0, eol);
    if (statusLine.rfind("HTTP/", 0) != 0) {
        err = "HTTP proxy returned invalid response to CONNECT.";
        return false;
    }
    std::istringstream iss(statusLine);
    std::string httpVersion;
    int statusCode = 0;
    iss >> httpVersion >> statusCode;
    if (statusCode == 200)
        return true;
    if (statusCode == 407) {
        err = "HTTP proxy authentication required or failed (407).";
        return false;
    }
    if (statusCode > 0) {
        err = "HTTP CONNECT tunnel rejected: " + statusLine;
        return false;
    }
    err = "HTTP proxy returned malformed CONNECT status.";
    return false;
}

} // namespace

bool Libssh2SftpClient::tcpConnect(const SessionOptions &opt,
                                   std::string &err) {
    const bool useJump = opt.jump_host.has_value() && !opt.jump_host->empty();
    const bool useProxy = (opt.proxy_type != ProxyType::None);
    if (useJump && useProxy) {
        err = "Proxy and SSH jump host cannot be used together.";
        return false;
    }

    if (useJump) {
#ifdef _WIN32
        err = "SSH jump host is not supported on this platform.";
        return false;
#else
        int jumpSock = -1;
        int jumpPid = -1;
        int jumpStderrFd = -1;
        if (!spawn_ssh_jump_tunnel(opt, jumpSock, jumpPid, jumpStderrFd, err))
            return false;
        {
            std::lock_guard<std::mutex> lk(stateMutex_);
            jumpProxyPid_ = jumpPid;
            jumpProxyStderrFd_ = jumpStderrFd;
            sock_ = jumpSock;
        }
        if (opt.connection_stop_token.stop_requested()) {
            interrupt();
            err = "Connection canceled by user";
            return false;
        }
        return true;
#endif
    }

    const std::string endpointHost = useProxy ? opt.proxy_host : opt.host;
    const std::uint16_t endpointPort = useProxy ? opt.proxy_port : opt.port;
    if (endpointHost.empty()) {
        err = useProxy ? "Proxy host is empty." : "Host is empty.";
        return false;
    }
    if (endpointPort == 0) {
        err = useProxy ? "Proxy port is invalid." : "Port is invalid.";
        return false;
    }

    SocketHandle socketFd = kInvalidSocket;
    if (!connect_tcp_endpoint(endpointHost, endpointPort, socketFd, err,
                              opt.connection_stop_token))
        return false;
    {
        std::lock_guard<std::mutex> lock(stateMutex_);
        sock_ = socketFd;
    }
    if (opt.connection_stop_token.stop_requested()) {
        interrupt();
        err = "Connection canceled by user";
        return false;
    }

    if (useProxy) {
        constexpr int kProxyHandshakeTimeoutMs = 20000;
        (void)set_socket_timeout_ms(socketFd, kProxyHandshakeTimeoutMs);
        bool ok = false;
        if (opt.proxy_type == ProxyType::Socks5) {
            ok = establish_socks5_tunnel(socketFd, opt, err);
        } else if (opt.proxy_type == ProxyType::HttpConnect) {
            ok = establish_http_connect_tunnel(socketFd, opt, err);
        } else {
            err = "Unsupported proxy type.";
        }
        (void)set_socket_timeout_ms(socketFd, 0);
        if (!ok) {
            return false;
        }
    }

    return true;
}

bool Libssh2SftpClient::sshHandshakeAuth(const SessionOptions &opt,
                                         std::string &err,
                                         bool initializeSftpSubsystem) {
    session_ = libssh2_session_init();
    if (!session_) {
        err = "libssh2_session_init failed";
        return false;
    }

    // known_hosts is read before the handshake so that the host key types it
    // already records for this host are asked for first.
    std::string khPath;
    std::vector<std::string> khLines;
    auto khRead = libssh2detail::KnownHostsReadStatus::Missing;
    if (opt.known_hosts_policy != openscp::KnownHostsPolicy::Off) {
        if (opt.known_hosts_path.has_value()) {
            khPath = *opt.known_hosts_path;
        } else {
            khPath = default_known_hosts_path();
        }
        if (!khPath.empty()) {
            khRead = libssh2detail::readKnownHosts(khPath, khLines);
        }
    }

    // Prefer modern algorithms (host keys, ciphers, MACs) and avoid DSA where
    // possible
#ifdef LIBSSH2_METHOD_HOSTKEY
    // Host keys: ensure no ssh-dss, avoid ssh-rsa(SHA-1); prefer modern with
    // safe fallbacks, after the types known_hosts records for this host.
    const std::string hostKeyPreference =
        libssh2detail::hostKeyAlgorithmPreference(
            recorded_host_key_types(khLines, opt.host, opt.port));
    (void)libssh2_session_method_pref(session_, LIBSSH2_METHOD_HOSTKEY,
                                      hostKeyPreference.c_str());
#endif
#ifdef LIBSSH2_METHOD_KEX
    (void)libssh2_session_method_pref(
        session_, LIBSSH2_METHOD_KEX,
        "curve25519-sha256,ecdh-sha2-nistp256,diffie-hellman-group14-sha256");
#endif
    // The preferred AEAD depends on whether this CPU accelerates AES-GCM.
    static const char *const cipherPreference =
        libssh2detail::sshCipherPreference(
            libssh2detail::cpuHasAesGcmInstructions());
#ifdef LIBSSH2_METHOD_CRYPT_CS
    (void)libssh2_session_method_pref(session_, LIBSSH2_METHOD_CRYPT_CS,
                                      cipherPreference);
#endif
#ifdef LIBSSH2_METHOD_CRYPT_SC
    (void)libssh2_session_method_pref(session_, LIBSSH2_METHOD_CRYPT_SC,
                                      cipherPreference);
#endif
#ifdef LIBSSH2_METHOD_MAC_CS
    (void)libssh2_session_method_pref(session_, LIBSSH2_METHOD_MAC_CS,
                                      "hmac-sha2-512,hmac-sha2-256");
#endif
#ifdef LIBSSH2_METHOD_MAC_SC
    (void)libssh2_session_method_pref(session_, LIBSSH2_METHOD_MAC_SC,
                                      "hmac-sha2-512,hmac-sha2-256");
#endif

    // Ensure blocking mode and bounded waits before handshake/auth.
    libssh2_session_set_blocking(session_, 1);
    libssh2_session_set_timeout(session_, 20000); // 20s
    if (libssh2_session_handshake(session_, sock_) != 0) {
#ifndef _WIN32
        if (opt.jump_host.has_value() && !opt.jump_host->empty() &&
            describeJumpTunnelFailure(err)) {
            return false;
        }
#endif
        err = "SSH handshake failed";
        return false;
    }

    // SSH keepalive: request libssh2 to send messages every 30s if the peer
    // allows it
    libssh2_keepalive_config(session_, 1, 30);

    // Host key verification according to known_hosts policy
    if (opt.known_hosts_policy == openscp::KnownHostsPolicy::Off) {
        // No verification: do not consult known_hosts and do not fail on
        // mismatch
    } else {
        LIBSSH2_KNOWNHOSTS *nh = libssh2_knownhost_init(session_);
        if (!nh) {
            err = "Could not initialize known_hosts";
            return false;
        }

        // Each line is loaded on its own: a line libssh2 cannot parse is
        // skipped, as OpenSSH does, instead of ending the load.
        int skippedLines = 0;
        for (const std::string &line : khLines) {
            // libssh2_knownhost_readfile() passes the newline too.
            const std::string withNewline = line + "\n";
            if (libssh2_knownhost_readline(nh, withNewline.data(),
                                           withNewline.size(),
                                           LIBSSH2_KNOWNHOST_FILE_OPENSSH) < 0)
                ++skippedLines;
        }
        if (skippedLines > 0) {
            core_logf(CoreLogLevel::Info,
                      "known_hosts: skipped %d line(s) libssh2 cannot parse",
                      skippedLines);
        }
        if (khRead != libssh2detail::KnownHostsReadStatus::Read &&
            opt.known_hosts_policy == openscp::KnownHostsPolicy::Strict) {
            libssh2_knownhost_free(nh);
            err = "known_hosts unavailable or unreadable (strict policy)";
            return false;
        }
        // Without the whole file a changed key would look like a new host.
        if (khRead == libssh2detail::KnownHostsReadStatus::Unreadable &&
            opt.known_hosts_policy == openscp::KnownHostsPolicy::AcceptNew) {
            libssh2_knownhost_free(nh);
            err = "known_hosts exists but could not be read, so the host key "
                  "cannot be verified";
            if (opt.hostkey_status_cb) {
                opt.hostkey_status_cb(err);
            }
            return false;
        }

        size_t keylen = 0;
        int keytype = 0;
        const char *hostkey =
            libssh2_session_hostkey(session_, &keylen, &keytype);
        if (!hostkey || keylen == 0) {
            libssh2_knownhost_free(nh);
            err = "Could not get host key";
            return false;
        }

        int alg = 0;
        std::string algDisplay = "unknown";
        switch (keytype) {
        case LIBSSH2_HOSTKEY_TYPE_RSA:
            alg = LIBSSH2_KNOWNHOST_KEY_SSHRSA;
            algDisplay = "ssh-rsa";
            break;
        case LIBSSH2_HOSTKEY_TYPE_DSS:
            // Reject DSA host keys as too weak
            libssh2_knownhost_free(nh);
            err = "Host key DSA no permitido";
            return false;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
            algDisplay = "ecdsa-sha2-nistp256";
#ifdef LIBSSH2_KNOWNHOST_KEY_ECDSA_256
            alg = LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
#endif
            break;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
            algDisplay = "ecdsa-sha2-nistp384";
#ifdef LIBSSH2_KNOWNHOST_KEY_ECDSA_384
            alg = LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
#endif
            break;
        case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
            algDisplay = "ecdsa-sha2-nistp521";
#ifdef LIBSSH2_KNOWNHOST_KEY_ECDSA_521
            alg = LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
#endif
            break;
        case LIBSSH2_HOSTKEY_TYPE_ED25519:
            algDisplay = "ssh-ed25519";
#ifdef LIBSSH2_KNOWNHOST_KEY_ED25519
            alg = LIBSSH2_KNOWNHOST_KEY_ED25519;
#endif
            break;
        default:
            alg = 0;
            break;
        }

        int typemask_plain =
            LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | alg;
        core_logf(CoreLogLevel::Debug,
                  "HostKey map: keytype=%d algMask=%d display=%s", keytype, alg,
                  algDisplay.c_str());

        // TYPE_PLAIN names the host being checked; libssh2 compares it with
        // plain and hashed (|1|...) entries alike. A second check with
        // TYPE_SHA1 would always report a mismatch, since libssh2 cannot take
        // a SHA1 name as input, and hide the real "not found".
        struct libssh2_knownhost *host = nullptr;
        int check =
            libssh2_knownhost_checkp(nh, opt.host.c_str(), opt.port, hostkey,
                                     keylen, typemask_plain, &host);

        if (check == LIBSSH2_KNOWNHOST_CHECK_MATCH) {
            libssh2_knownhost_free(nh);
        } else if (opt.known_hosts_policy ==
                       openscp::KnownHostsPolicy::AcceptNew &&
                   check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) {
            // TOFU rejects changed keys: changed host identities must fail
            // hard.
            libssh2_knownhost_free(nh);
            err = "Host key does not match known_hosts (TOFU rejects changed "
                  "keys)";
            if (opt.hostkey_status_cb) {
                opt.hostkey_status_cb(err);
            }
            return false;
        } else if (opt.known_hosts_policy ==
                       openscp::KnownHostsPolicy::AcceptNew &&
                   check == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND) {
            // TOFU: for unknown hosts ask the user for confirmation.
            std::string algName;
            switch (keytype) {
            case LIBSSH2_HOSTKEY_TYPE_RSA:
                algName = "RSA";
                break;
            case LIBSSH2_HOSTKEY_TYPE_DSS:
                algName = "DSA";
                break;
            case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
                algName = "ECDSA-256";
                break;
            case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
                algName = "ECDSA-384";
                break;
            case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
                algName = "ECDSA-521";
                break;
            case LIBSSH2_HOSTKEY_TYPE_ED25519:
                algName = "ED25519";
                break;
            default:
                algName = "DESCONOCIDO";
                break;
            }
            // Obtain SHA256 fingerprint if available (Base64 by default);
            // optional hex for compat
            std::string fpStr;
#ifdef LIBSSH2_HOSTKEY_HASH_SHA256
            const unsigned char *h = reinterpret_cast<const unsigned char *>(
                libssh2_hostkey_hash(session_, LIBSSH2_HOSTKEY_HASH_SHA256));
            if (h) {
                std::string fpB64 = b64encode(h, 32);
                // Strip padding '=' to match OpenSSH presentation
                while (!fpB64.empty() && fpB64.back() == '=')
                    fpB64.pop_back();
                bool hexOnly = (std::getenv("OPENSCP_FP_HEX_ONLY") &&
                                *std::getenv("OPENSCP_FP_HEX_ONLY") == '1');
                if (!hexOnly)
                    hexOnly = opt.show_fp_hex;
                if (hexOnly) {
                    std::ostringstream oss;
                    for (int i = 0; i < 32; ++i) {
                        if (i)
                            oss << ':';
                        char b[4];
                        std::snprintf(b, sizeof(b), "%02X",
                                      static_cast<unsigned>(h[i]));
                        oss << b;
                    }
                    fpStr = std::string("SHA256:") + oss.str();
                } else {
                    fpStr = std::string("SHA256:") + fpB64;
                }
            }
#else
            const unsigned char *h = reinterpret_cast<const unsigned char *>(
                libssh2_hostkey_hash(session_, LIBSSH2_HOSTKEY_HASH_SHA1));
            if (h) {
                std::ostringstream oss;
                oss << "SHA1:";
                for (int i = 0; i < 20; ++i) {
                    if (i)
                        oss << ':';
                    char b[4];
                    std::snprintf(b, sizeof(b), "%02X",
                                  static_cast<unsigned>(h[i]));
                    oss << b;
                }
                fpStr = oss.str();
            }
#endif
            // Bits (approx): length of raw hostkey bytes
            int keyBits = 0;
            switch (keytype) {
            case LIBSSH2_HOSTKEY_TYPE_ED25519:
            case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
                keyBits = 256;
                break;
            case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
                keyBits = 384;
                break;
            case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
                keyBits = 521;
                break;
            default:
                keyBits = static_cast<int>(keylen) * 8;
                break;
            }
            std::ostringstream algWithBits;
            algWithBits << algDisplay << " (" << keyBits << "-bit)";

            bool canSaveInitial =
                !khPath.empty() && ((alg != 0)
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
                                    || (keytype == LIBSSH2_HOSTKEY_TYPE_ED25519)
#endif
                                   );
            bool confirmed = false;
            if (opt.hostkey_confirm_cb) {
                confirmed = opt.hostkey_confirm_cb(opt.host, opt.port,
                                                   algWithBits.str(), fpStr,
                                                   canSaveInitial);
            }
            if (!confirmed) {
                libssh2_knownhost_free(nh);
                auditLogHostKey(opt.host, opt.port, algWithBits.str(), fpStr,
                                "rejected");
                err = "Unknown host: fingerprint not confirmed by user";
                return false;
            }

            // If saving is not possible (unsupported alg or no kh path): allow
            // only if user explicitly accepted
            if (!canSaveInitial) {
                libssh2_knownhost_free(nh);
                core_logf(CoreLogLevel::Info,
                          "Saving hostkey skipped: no khPath or unsupported "
                          "algorithm");
                if (opt.hostkey_status_cb) {
                    std::string why = khPath.empty()
                                          ? "known_hosts path is not set"
                                          : "Host key algorithm lacks "
                                            "known_hosts support in libssh2";
                    opt.hostkey_status_cb(
                        std::string("Fingerprint cannot be saved: ") + why);
                }
                auditLogHostKey(opt.host, opt.port, algWithBits.str(), fpStr,
                                "skipped");
                // Continue without persisting
            } else {
                bool preferHashed = opt.known_hosts_hash_names;
                if (const char *ev = std::getenv("OPENSCP_KNOWNHOSTS_PLAIN")) {
                    if (*ev == '1')
                        preferHashed = false;
                    else if (*ev == '0')
                        preferHashed = true;
                }
                // host string with port when non-default
                std::string hostForKnown = opt.host;
                if (opt.port != 22)
                    hostForKnown = std::string("[") + opt.host +
                                   "]:" + std::to_string(opt.port);

                // Only the new line is appended: the rest of the file keeps
                // its comments, its lines and its permissions.
                const std::string line = libssh2detail::knownHostsLine(
                    hostForKnown, algDisplay,
                    reinterpret_cast<const unsigned char *>(hostkey), keylen,
                    preferHashed);
                std::string persistErr = "could not hash the host name";
                const bool saved =
                    !line.empty() && libssh2detail::appendKnownHostsLine(
                                         khPath, line, persistErr);
                if (!saved && opt.hostkey_status_cb) {
                    opt.hostkey_status_cb(
                        std::string("Could not save known_hosts: ") +
                        persistErr);
                }

                libssh2_knownhost_free(nh);
                if (saved) {
                    auditLogHostKey(opt.host, opt.port, algWithBits.str(),
                                    fpStr, "saved");
                } else {
                    // Saving failed: require explicit confirmation to proceed
                    // without saving
                    bool proceed = (opt.hostkey_confirm_cb &&
                                    opt.hostkey_confirm_cb(
                                        opt.host, opt.port, algWithBits.str(),
                                        fpStr, /*canSave*/ false));
                    if (!proceed) {
                        auditLogHostKey(opt.host, opt.port, algWithBits.str(),
                                        fpStr, "save_failed");
                        err = "Could not save fingerprint in known_hosts";
                        return false;
                    }
                    auditLogHostKey(opt.host, opt.port, algWithBits.str(),
                                    fpStr, "skipped");
                }
            }
        } else if (opt.known_hosts_policy ==
                       openscp::KnownHostsPolicy::AcceptNew &&
                   check == LIBSSH2_KNOWNHOST_CHECK_FAILURE) {
            libssh2_knownhost_free(nh);
            err = "known_hosts validation failed (TOFU policy)";
            if (opt.hostkey_status_cb) {
                opt.hostkey_status_cb(err);
            }
            return false;
        } else {
            libssh2_knownhost_free(nh);
            // Policy handling for non-match cases
            // - Strict: fail on mismatch or notfound
            // - AcceptNew (TOFU): already handled NOTFOUND/MISMATCH above with
            // user confirmation
            if (opt.known_hosts_policy == openscp::KnownHostsPolicy::Strict) {
                err = (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH)
                          ? "Host key does not match known_hosts"
                          : "Unknown host in known_hosts";
                return false;
            }
        }
    }

    // Authentication: prefer the method explicitly provided by the user.
    // 1) If a private key is specified: use it first.
    // 2) If a password is specified: try password first; if connection remains
    // open but fails, query methods and try kbd-int. 3) If no credentials: try
    // ssh-agent (if the server allows); otherwise, error.
    if (opt.private_key_path.has_value()) {
        const char *passphrase = opt.private_key_passphrase
                                     ? opt.private_key_passphrase->c_str()
                                     : nullptr;
#ifdef _WIN32
        // libssh2 opens key files with the narrow C API, which on Windows
        // reads the path in the system code page, so a key under a profile
        // such as C:\Users\José would not open. The key is read here instead.
        std::string keyData;
        CleanseOnScopeExit<std::string> keyCleanse(keyData);
        std::string readError;
        if (!read_private_key_file(*opt.private_key_path, keyData, readError)) {
            err = "Key authentication failed — " + readError;
            return false;
        }
        int rc = libssh2_userauth_publickey_frommemory(
            session_, opt.username.c_str(), opt.username.size(), nullptr, 0,
            keyData.data(), keyData.size(), passphrase);
#else
        int rc = libssh2_userauth_publickey_fromfile(
            session_, opt.username.c_str(),
            NULL, // public key path (NULL: derived from the private key)
            opt.private_key_path->c_str(), passphrase);
#endif
        if (rc != 0) {
            char *emsgPtr = nullptr;
            int emlen = 0;
            (void)libssh2_session_last_error(session_, &emsgPtr, &emlen, 0);
            const std::string lastErr =
                (emsgPtr && emlen > 0)
                    ? std::string(emsgPtr, static_cast<size_t>(emlen))
                    : std::string();
            const long lastErrno = libssh2_session_last_errno(session_);
            err = std::string("Key authentication failed") +
                  (lastErr.empty() ? std::string()
                                   : (std::string(" — ") + lastErr)) +
                  " [rc=" + std::to_string(rc) +
                  ", errno=" + std::to_string(lastErrno) + "]";
            return false;
        }
    } else {
        // Keep the methods list ONLY when necessary.
        std::string authlist;
        auto hasMethod = [&](const char *m) {
            return !authlist.empty() && authlist.find(m) != std::string::npos;
        };
        auto tryAgentAuth = [&]() {
            bool authed = false;
            if (hasMethod("publickey")) {
                LIBSSH2_AGENT *agent = libssh2_agent_init(session_);
                if (agent && libssh2_agent_connect(agent) == 0) {
                    if (libssh2_agent_list_identities(agent) == 0) {
                        struct libssh2_agent_publickey *identity = nullptr;
                        struct libssh2_agent_publickey *prev = nullptr;
                        int tries = 0;
                        const int kMaxAgentTries = 3;
                        while (libssh2_agent_get_identity(agent, &identity,
                                                          prev) == 0 &&
                               tries < kMaxAgentTries) {
                            prev = identity;
                            ++tries;
                            int arc = -1;
                            for (;;) {
                                arc = libssh2_agent_userauth(
                                    agent, opt.username.c_str(), identity);
                                if (arc != LIBSSH2_ERROR_EAGAIN)
                                    break;
                                std::this_thread::sleep_for(
                                    std::chrono::milliseconds(50));
                            }
                            if (arc == 0) {
                                authed = true;
                                break;
                            }
                        }
                    }
                }
                if (agent) {
                    libssh2_agent_disconnect(agent);
                    libssh2_agent_free(agent);
                }
            }
            return authed;
        };

        // If the user provided a password: try it first to avoid exhausting
        // attempts with 'none' or agent.
        if (opt.password.has_value()) {
            int rc_pw = -1;
            int rc_kbd = -1;
            std::string pwLastErr;
            long pwErrno = 0;
            std::string kbLastErr;
            long kbErrno = 0;

            auto do_password = [&]() {
                for (;;) {
                    rc_pw = libssh2_userauth_password(
                        session_, opt.username.c_str(), opt.password->c_str());
                    if (rc_pw != LIBSSH2_ERROR_EAGAIN)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                char *emsgPtr = nullptr;
                int emlen = 0;
                (void)libssh2_session_last_error(session_, &emsgPtr, &emlen, 0);
                if (emsgPtr && emlen > 0)
                    pwLastErr.assign(emsgPtr, static_cast<size_t>(emlen));
                pwErrno = libssh2_session_last_errno(session_);
            };
            auto do_kbdint = [&]() {
                bool kbdCancelled = false;
                bool kbdRejected = false;
                std::string kbdRejectionError;
                int kbdRounds = 0;
                KbdIntCtx ctx{opt.username.c_str(),
                              opt.password->c_str(),
                              &opt.keyboard_interactive_cb,
                              &kbdCancelled,
                              &kbdRejected,
                              &kbdRejectionError,
                              &kbdRounds};
                void **abs = libssh2_session_abstract(session_);
                if (abs)
                    *abs = &ctx;
                for (;;) {
                    rc_kbd = libssh2_userauth_keyboard_interactive(
                        session_, opt.username.c_str(),
                        kbint_password_callback);
                    if (rc_kbd != LIBSSH2_ERROR_EAGAIN)
                        break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                if (abs)
                    *abs = nullptr;
                if (kbdCancelled) {
                    err =
                        "Keyboard-interactive authentication canceled by user";
                    rc_kbd = -1;
                    return false;
                }
                if (kbdRejected) {
                    err = kbdRejectionError.empty()
                              ? "Keyboard-interactive authentication input was "
                                "rejected."
                              : kbdRejectionError;
                    rc_kbd = -1;
                    return false;
                }
                char *emsgPtr = nullptr;
                int emlen = 0;
                (void)libssh2_session_last_error(session_, &emsgPtr, &emlen, 0);
                if (emsgPtr && emlen > 0)
                    kbLastErr.assign(emsgPtr, static_cast<size_t>(emlen));
                kbErrno = libssh2_session_last_errno(session_);
                return true;
            };

            // Attempt password directly (without prior userauth_list).
            do_password();

            // If the server closed after the password attempt, stop: the rest
            // would cascade-fail.
            if (rc_pw == LIBSSH2_ERROR_SOCKET_DISCONNECT ||
                rc_pw == LIBSSH2_ERROR_SOCKET_SEND ||
                rc_pw == LIBSSH2_ERROR_SOCKET_RECV) {
                err = "Server closed the connection after password attempt";
                return false;
            }

            // If password failed but the session is still alive, NOW query
            // methods and try keyboard-interactive if applicable.
            if (rc_pw != 0) {
                char *methods = libssh2_userauth_list(
                    session_, opt.username.c_str(),
                    static_cast<unsigned>(opt.username.size()));
                authlist = methods ? std::string(methods) : std::string();
                if (hasMethod("keyboard-interactive")) {
                    if (!do_kbdint())
                        return false;
                }
            }

            if (rc_pw != 0 && rc_kbd != 0) {
                // As a last resort, try ssh-agent if the server allows (limit
                // identities). We need authlist to know if publickey is
                // allowed.
                if (authlist.empty()) {
                    char *methods = libssh2_userauth_list(
                        session_, opt.username.c_str(),
                        static_cast<unsigned>(opt.username.size()));
                    authlist = methods ? std::string(methods) : std::string();
                }

                const bool authed = tryAgentAuth();

                if (!authed) {
                    // Save libssh2 error message for diagnostics
                    char *emsgPtr = nullptr;
                    int emlen = 0;
                    (void)libssh2_session_last_error(session_, &emsgPtr, &emlen,
                                                     0);
                    std::string lastErr =
                        (emsgPtr && emlen > 0)
                            ? std::string(emsgPtr, static_cast<size_t>(emlen))
                            : std::string();
                    long lastErrno = libssh2_session_last_errno(session_);
                    // Include return codes for diagnostics
                    err =
                        std::string("Password/kbdint authentication failed") +
                        (authlist.empty()
                             ? std::string()
                             : (std::string(" (methods: ") + authlist + ")")) +
                        (lastErr.empty() ? std::string()
                                         : (std::string(" — ") + lastErr)) +
                        (std::string(" [rc_pw=") + std::to_string(rc_pw) +
                         ", rc_kbd=" + std::to_string(rc_kbd) +
                         ", errno=" + std::to_string(lastErrno) + "]") +
                        (pwLastErr.empty()
                             ? std::string()
                             : (std::string(" {pw='") + pwLastErr +
                                "' errno=" + std::to_string(pwErrno) + "}")) +
                        (kbLastErr.empty()
                             ? std::string()
                             : (std::string(" {kbd='") + kbLastErr +
                                "' errno=" + std::to_string(kbErrno) + "}"));
                    return false;
                }
            }
        } else {
            // Without a password: query methods and then try ssh-agent if the
            // server allows.
            char *methods = libssh2_userauth_list(
                session_, opt.username.c_str(),
                static_cast<unsigned>(opt.username.size()));
            authlist = methods ? std::string(methods) : std::string();
            const bool authed = tryAgentAuth();
            if (!authed) {
                err = "Sin credenciales: clave/agent/password no disponibles";
                return false;
            }
        }
    }

    if (initializeSftpSubsystem) {
        // Initialize SFTP only for backends that require directory/metadata
        // operations. SCP may reuse this transport without SFTP.
        sftp_ = libssh2_sftp_init(session_);
        if (!sftp_) {
            err = "Could not initialize SFTP";
            return false;
        }
    }

    return true;
}

bool Libssh2SftpClient::connectInternal(const SessionOptions &opt,
                                        std::string &err,
                                        bool initializeSftpSubsystem) {
    if (opt.connection_stop_token.stop_requested()) {
        err = "Connection canceled by user";
        setLastOperationError(RemoteErrorKind::Canceled, err);
        return false;
    }
    if (!isValidKnownHostsPolicy(opt.known_hosts_policy) ||
        !isValidTransferIntegrityPolicy(opt.transfer_integrity_policy) ||
        !isValidLocalFileDurability(opt.local_file_durability)) {
        err = "Invalid SSH security policy.";
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    if (!isValidProxyType(opt.proxy_type) || opt.port == 0 ||
        (opt.proxy_type != ProxyType::None && opt.proxy_port == 0) ||
        (opt.jump_host && opt.jump_port == 0)) {
        err = "Invalid SSH endpoint or proxy port configuration.";
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    const int initResult = ensure_libssh2_initialized();
    if (initResult != LIBSSH2_ERROR_NONE) {
        err = "Could not initialize libssh2 (error " +
              std::to_string(initResult) + ").";
        setLastOperationError(RemoteErrorKind::Unknown, err, initResult);
        return false;
    }
    if (connected_) {
        err = "Already connected";
        return false;
    }

    transferIntegrityPolicy_ =
        integrity_policy_from_env(opt.transfer_integrity_policy);
    localFileDurability_ = opt.local_file_durability;

    // Defensive: ensure no leftover state from any previous partial attempt.
    disconnect();

    std::stop_callback interruptConnection(opt.connection_stop_token,
                                           [this] { interrupt(); });

    if (!libssh2detail::validateEndpointHost(opt.host, "SSH host", err)) {
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    if (opt.proxy_type != ProxyType::None &&
        !libssh2detail::validateEndpointHost(opt.proxy_host, "Proxy host",
                                             err)) {
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    if (opt.jump_host && !libssh2detail::validateEndpointHost(
                             *opt.jump_host, "Jump host", err)) {
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }

    if (!tcpConnect(opt, err)) {
        const RemoteError failure = classifyStructuredFailure(err, false);
        disconnect();
        setLastOperationError(failure);
        return false;
    }
    if (!sshHandshakeAuth(opt, err, initializeSftpSubsystem)) {
        const RemoteError structuredFailure =
            classifyStructuredFailure(err, false);
        disconnect();
        setLastOperationError(structuredFailure);
        return false;
    }

    if (opt.connection_stop_token.stop_requested()) {
        disconnect();
        err = "Connection canceled by user";
        setLastOperationError(RemoteErrorKind::Canceled, err);
        return false;
    }
    connected_ = true;
    return true;
}

bool Libssh2SftpClient::connect(const SessionOptions &opt, std::string &err) {
    auto structuredErrorScope = beginStructuredOperation(err);
    return connectInternal(opt, err, true);
}

bool Libssh2SftpClient::connectTransportOnly(const SessionOptions &opt,
                                             std::string &err) {
    auto structuredErrorScope = beginStructuredOperation(err);
    return connectInternal(opt, err, false);
}

bool Libssh2SftpClient::ensureSftpSubsystem(std::string &err) {
    auto ioLock = lockIo();
    err.clear();
    if (!connected_ || !session_) {
        err = "Not connected";
        return false;
    }
    if (sftp_)
        return true;
    _LIBSSH2_SFTP *sftp = libssh2_sftp_init(session_);
    if (!sftp) {
        err = "Could not initialize SFTP";
        return false;
    }
    std::lock_guard<std::mutex> lk(stateMutex_);
    sftp_ = sftp;
    return true;
}

void Libssh2SftpClient::disconnect() {
    bool wasConnected = false;
    SocketHandle sock = kInvalidSocket;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        wasConnected = connected_;
        connected_ = false;
        // Keep the descriptor attached until shutdown finishes, as interrupt()
        // does. Another disconnect must not detach, close and reuse it first.
        // Shut down before waiting for I/O so blocking operations can finish.
        if (sock_ != kInvalidSocket)
            libssh2detail::shutdownSocket(sock_);
    }

    // Wait for any in-flight I/O operation to finish using session_ and sftp_.
    std::unique_lock<std::recursive_timed_mutex> ioLock(ioMutex_,
                                                        std::defer_lock);
#ifdef _WIN32
    // libssh2 may enter select() after the shutdown, which on Windows only a
    // new cancellation ends, so cancel again until the operation lets go.
    // Waiting on the lock between cancellations takes it as soon as an
    // operation ends; sleeping and then trying it missed that moment while
    // another thread started one operation after another.
    while (!ioLock.try_lock()) {
        {
            std::lock_guard<std::mutex> lk(stateMutex_);
            if (sock_ != kInvalidSocket)
                libssh2detail::cancelSocketIo(sock_);
        }
        if (ioLock.try_lock_for(std::chrono::milliseconds(20)))
            break;
    }
#else
    ioLock.lock();
#endif

    _LIBSSH2_SFTP *sftp = nullptr;
    _LIBSSH2_SESSION *session = nullptr;
#ifndef _WIN32
    int jumpPid = -1;
    int jumpStderrFd = -1;
#endif
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        sftp = sftp_;
        session = session_;
        sock = sock_;
        sftp_ = nullptr;
        session_ = nullptr;
        sock_ = kInvalidSocket;
#ifndef _WIN32
        jumpPid = jumpProxyPid_;
        jumpProxyPid_ = -1;
        jumpStderrFd = jumpProxyStderrFd_;
        jumpProxyStderrFd_ = -1;
#endif
    }

    // libssh2 keeps the socket's number and still reads, writes and changes
    // its flags while shutting down, so the session goes before the socket:
    // once closed, that number may already belong to another connection. The
    // socket is shut down, so these calls fail at once.
    if (session) {
        libssh2_session_set_blocking(session, 0);
        libssh2_session_set_timeout(session, 2000);
    }

    if (sftp) {
        (void)libssh2_sftp_shutdown(sftp);
    }
    if (session) {
        // Only send SSH disconnect message for fully established sessions.
        if (wasConnected) {
            (void)libssh2_session_disconnect(session, "bye");
        }
        libssh2_session_free(session);
    }

    if (sock != kInvalidSocket) {
        libssh2detail::closeSocket(sock);
    }
#ifndef _WIN32
    if (jumpStderrFd != -1)
        ::close(jumpStderrFd);
    if (jumpPid > 0)
        stop_ssh_jump_tunnel(jumpPid);
#endif
}

#ifndef _WIN32
bool Libssh2SftpClient::describeJumpTunnelFailure(std::string &err) {
    int jumpPid = -1;
    int jumpStderrFd = -1;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        jumpPid = jumpProxyPid_;
        jumpStderrFd = jumpProxyStderrFd_;
    }

    if (jumpPid <= 0 && jumpStderrFd < 0)
        return false;

    bool exited = false;
    // Poll child state without blocking; handshake path remains responsive.
    if (jumpPid > 0) {
        int status = 0;
        const pid_t wr =
            ::waitpid(static_cast<pid_t>(jumpPid), &status, WNOHANG);
        if (wr == static_cast<pid_t>(jumpPid) ||
            (wr == -1 && errno == ECHILD)) {
            exited = true;
        }
    }

    const std::string stderrText = read_jump_tunnel_stderr(jumpStderrFd);
    if (!exited && stderrText.empty())
        return false;

    if (exited) {
        std::lock_guard<std::mutex> lk(stateMutex_);
        if (jumpProxyPid_ == jumpPid)
            jumpProxyPid_ = -1;
        if (jumpProxyStderrFd_ == jumpStderrFd) {
            if (jumpProxyStderrFd_ != -1)
                ::close(jumpProxyStderrFd_);
            jumpProxyStderrFd_ = -1;
        }
    }

    err = format_jump_tunnel_exit_error(
        exited ? "SSH jump tunnel failed before the SSH handshake."
               : "SSH jump tunnel reported an error before the SSH handshake.",
        stderrText);
    return true;
}
#endif

void Libssh2SftpClient::interrupt() {
    // Keep the descriptor attached until shutdown finishes. Disconnect may
    // otherwise close it and another thread may reuse that descriptor number.
    std::lock_guard<std::mutex> lk(stateMutex_);
    if (sock_ == kInvalidSocket)
        return;
    // libssh2 cannot abandon a blocking call, so the socket is shut down and
    // the session cannot be used again.
    connected_.store(false);
    libssh2detail::shutdownSocket(sock_);
}

bool Libssh2SftpClient::list(const std::string &remote_path,
                             std::vector<FileInfo> &out, std::string &err) {
    auto structuredErrorScope = beginStructuredOperation(err);
    out.clear();
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;

    std::string path = remote_path.empty() ? "/" : remote_path;
    if (rejectOversizedPath(path, err))
        return false;

    LIBSSH2_SFTP_HANDLE *dir = libssh2_sftp_open_ex(
        sftp_, path.c_str(), static_cast<unsigned int>(path.size()), 0, 0,
        LIBSSH2_SFTP_OPENDIR);
    if (!dir) {
        err = "sftp_opendir failed for: " + path;
        return false;
    }

    out.reserve(64);
    RemoteListingBudget listingBudget;

    char filename[1024];
    char longentry[1024];
    LIBSSH2_SFTP_ATTRIBUTES attrs;

    while (true) {
        memset(&attrs, 0, sizeof(attrs));
        int rc = libssh2_sftp_readdir_ex(dir, filename, sizeof(filename),
                                         longentry, sizeof(longentry), &attrs);
        if (rc > 0) {
            if (static_cast<std::size_t>(rc) > sizeof(filename)) {
                err = "SFTP directory entry name exceeds buffer limit.";
                (void)libssh2_sftp_closedir(dir);
                out.clear();
                setLastOperationError(RemoteErrorKind::Protocol, err);
                return false;
            }
            const std::size_t nameLen = static_cast<std::size_t>(rc);
            if (std::memchr(filename, '\0', nameLen) != nullptr) {
                err = "SFTP directory entry name contains embedded null byte.";
                (void)libssh2_sftp_closedir(dir);
                out.clear();
                setLastOperationError(RemoteErrorKind::Protocol, err);
                return false;
            }
            // rc = name length
            FileInfo fi{};
            fi.name.assign(filename, nameLen);
            fi.is_dir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                            ? ((attrs.permissions & LIBSSH2_SFTP_S_IFMT) ==
                               LIBSSH2_SFTP_S_IFDIR)
                            : false;
            if (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) {
                fi.size = attrs.filesize;
                fi.has_size = true;
            }
            if (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
                fi.mtime = attrs.mtime;
            if (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                fi.mode = static_cast<std::uint32_t>(attrs.permissions);
            if (attrs.flags & LIBSSH2_SFTP_ATTR_UIDGID) {
                fi.uid = static_cast<std::uint32_t>(attrs.uid);
                fi.gid = static_cast<std::uint32_t>(attrs.gid);
            }
            if (fi.name == "." || fi.name == "..")
                continue;
            if (!listingBudget.tryConsume(fi.name.size())) {
                err = "SFTP directory listing exceeded the safety limit.";
                (void)libssh2_sftp_closedir(dir);
                out.clear();
                setLastOperationError(RemoteErrorKind::Protocol, err);
                return false;
            }
            out.push_back(std::move(fi));
        } else if (rc == 0) {
            // end of directory
            break;
        } else {
            err = "sftp_readdir_ex failed";
            (void)libssh2_sftp_closedir(dir);
            out.clear();
            return false;
        }
    }

    (void)libssh2_sftp_closedir(dir);
    return true;
}

bool Libssh2SftpClient::checksum(
    const std::string &remote_path, const std::string &algorithm,
    std::vector<std::uint8_t> &digest, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel) {
    auto structuredErrorScope = beginStructuredOperation(err);
    digest.clear();
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;

    std::string normalizedAlgorithm = algorithm;
    std::transform(normalizedAlgorithm.begin(), normalizedAlgorithm.end(),
                   normalizedAlgorithm.begin(), [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    if (normalizedAlgorithm != "sha-256" && normalizedAlgorithm != "sha256") {
        err = "Unsupported checksum algorithm: " + algorithm;
        setLastOperationError(RemoteErrorKind::Unsupported, err);
        return false;
    }
    if (remote_path.empty() || remote_path.find('\0') != std::string::npos) {
        err = "Remote checksum path is invalid";
        setLastOperationError(RemoteErrorKind::InvalidRequest, err);
        return false;
    }
    if (shouldCancel && shouldCancel()) {
        err = "Checksum calculation canceled";
        setLastOperationError(RemoteErrorKind::Canceled, err);
        return false;
    }

    apply_transfer_socket_timeouts(sock_);
    Sha256Digest calculated{};
    std::string hashError;
    if (!hash_remote_full(sftp_, remote_path, calculated, &hashError,
                          &shouldCancel, &progress)) {
        err = (shouldCancel && shouldCancel())
                  ? "Checksum calculation canceled"
                  : (hashError.empty() ? "Could not calculate remote checksum"
                                       : std::move(hashError));
        if (shouldCancel && shouldCancel())
            setLastOperationError(RemoteErrorKind::Canceled, err);
        return false;
    }
    if (shouldCancel && shouldCancel()) {
        err = "Checksum calculation canceled";
        setLastOperationError(RemoteErrorKind::Canceled, err);
        return false;
    }

    digest.assign(calculated.cbegin(), calculated.cend());
    return true;
}

// Download a remote file to local. Reports progress and supports cooperative
// cancellation.
bool Libssh2SftpClient::get(
    const std::string &remote, const std::string &local, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel, bool resume) {
    auto structuredErrorScope = beginStructuredOperation(err);
    setLastDownloadModifiedTime(std::nullopt);
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;

    apply_transfer_socket_timeouts(sock_);

    const TransferIntegrityPolicy policy = transferIntegrityPolicy_;
    const std::string localPart = local + ".part";

    // Open first and read the size from the handle: this saves the round trip
    // of a separate stat, and the attributes describe the file actually read.
    libssh2detail::UniqueSftpHandle rh(libssh2_sftp_open_ex(
        sftp_, remote.c_str(), static_cast<unsigned>(remote.size()),
        LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE));
    if (!rh) {
        err = "Could not open remote file for reading";
        return false;
    }
    LIBSSH2_SFTP_ATTRIBUTES st{};
    if (libssh2_sftp_fstat_ex(rh.get(), &st, 0) != 0) {
        err = "Could not stat remote path";
        const RemoteError structuredFailure =
            classifyStructuredFailure(err, false);
        setLastOperationError(structuredFailure);
        return false;
    }
    const bool hasTotal = (st.flags & LIBSSH2_SFTP_ATTR_SIZE) != 0;
    std::size_t total = hasTotal ? static_cast<std::size_t>(st.filesize) : 0;

    std::size_t offset = 0;
    if (resume) {
        std::uint64_t partSize = 0;
        std::string sizeErr;
        if (get_local_file_size(localPart, partSize, &sizeErr)) {
            offset = static_cast<std::size_t>(partSize);
        }
        if (offset > 0 && hasTotal && offset > total) {
            if (policy == TransferIntegrityPolicy::Required) {
                err = "Invalid resume: local .part is larger than remote file";
                return false;
            }
            offset = 0; // fallback: restart from scratch
        }
        if (offset > 0 && hasTotal && offset < total &&
            policy != TransferIntegrityPolicy::Off) {
            const std::uint64_t window =
                std::min<std::uint64_t>(offset, 64 * 1024);
            const std::uint64_t start =
                static_cast<std::uint64_t>(offset) - window;
            Sha256Digest lh{}, rhh{};
            std::string hErr;
            const bool okLocal = hash_local_range(localPart, start, window, lh,
                                                  &hErr, &shouldCancel);
            const bool okRemote =
                okLocal && hash_remote_range(sftp_, remote, start, window, rhh,
                                             &hErr, &shouldCancel);
            if (shouldCancel && shouldCancel()) {
                err = "Canceled by user";
                return false;
            }
            if (!okLocal || !okRemote) {
                if (policy == TransferIntegrityPolicy::Required) {
                    err = std::string("Could not validate resume integrity "
                                      "(download): ") +
                          hErr;
                    return false;
                }
                offset = 0; // optional: restart
            } else if (lh != rhh) {
                if (policy == TransferIntegrityPolicy::Required) {
                    err = "Integrity check failed in resume (download): local "
                          "prefix does not match remote";
                    return false;
                }
                offset = 0; // optional: restart
            }
        }
    }

    if (offset > 0) {
        libssh2_sftp_seek64(rh.get(), static_cast<libssh2_uint64_t>(offset));
    }

    // Open local .part for writing
    std::string openError;
    UniqueFile localFile(localfiles::openRegularFileForWrite(
        localPart,
        offset > 0 ? localfiles::WriteMode::Append
                   : localfiles::WriteMode::Truncate,
        openError));
    if (!localFile) {
        const int nativeError = errno;
        err = openError.empty()
                  ? "Could not open local file (.part) for writing"
                  : openError;
        setLastOperationError(RemoteErrorKind::LocalIo, err, nativeError);
        return false;
    }

    // Resume validation leaves offset <= total whenever the size is known.
    std::vector<char> buf(sftp_transfer_buffer_size(
        hasTotal ? total - offset : std::numeric_limits<std::uint64_t>::max()));
    std::size_t done = offset;

    // Under Optional, SSH already protects the bytes in transit, so a
    // whole-file download only checks that the remote file kept its size and
    // time; reading back what was just written would add little. A resumed
    // download also depends on earlier data, and a recently modified file may
    // change without new metadata, so both keep the full check.
    const std::optional<std::uint64_t> remoteModified =
        (st.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) != 0
            ? std::optional<std::uint64_t>(st.mtime)
            : std::nullopt;
    const std::int64_t nowSeconds =
        std::chrono::duration_cast<std::chrono::seconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count();
    const bool metadataCheckSuffices =
        policy == TransferIntegrityPolicy::Optional && offset == 0 &&
        !libssh2detail::remoteFileMayBeChanging(remoteModified, nowSeconds);

    while (true) {
        if (shouldCancel && shouldCancel()) {
            err = "Canceled by user";
            localFile.reset();
            return false;
        }
        ssize_t n = libssh2_sftp_read(rh.get(), buf.data(),
                                      static_cast<size_t>(buf.size()));
        if (n > 0) {
            if (std::fwrite(buf.data(), 1, static_cast<size_t>(n),
                            localFile.get()) != static_cast<size_t>(n)) {
                const int nativeError = errno;
                err = "Local write failed";
                localFile.reset();
                setLastOperationError(RemoteErrorKind::LocalIo, err,
                                      nativeError);
                return false;
            }
            done = done + static_cast<std::size_t>(n);
            if (progress && total)
                progress(done, total);
        } else if (n == 0) {
            break; // EOF
        } else {
            const bool canceledNow = (shouldCancel && shouldCancel());
            err = canceledNow ? "Canceled by user" : "Remote read failed";
            const RemoteError structuredFailure =
                classifyStructuredFailure(err, false);
            localFile.reset();
            setLastOperationError(structuredFailure);
            return false;
        }
    }

    if (metadataCheckSuffices) {
        LIBSSH2_SFTP_ATTRIBUTES after{};
        const bool remoteChanged =
            (hasTotal && done != total) ||
            (libssh2_sftp_fstat_ex(rh.get(), &after, 0) == 0 &&
             remote_file_changed(st, after));
        if (remoteChanged) {
            // The .part mixes versions of the file, so a retry must start
            // over. The change is usually brief, so the queue may retry.
            err = "Remote file changed during download (size or modification "
                  "time differs)";
            localFile.reset();
            std::string cleanupError;
            (void)localfiles::removeLocalPath(localPart, false, cleanupError);
            setLastOperationError(RemoteErrorKind::RemoteIo, err, 0, true);
            return false;
        }
    }

    std::string syncErr;
    if (!localfiles::flushAndSync(localFile.get(), syncErr,
                                  localFileDurability_)) {
        const int nativeError = errno;
        localFile.reset();
        err = std::string("Could not sync local file (.part): ") + syncErr;
        setLastOperationError(RemoteErrorKind::LocalIo, err, nativeError);
        return false;
    }
    localFile.reset();
    rh.close();

    if (fail_if_transfer_canceled(&shouldCancel, err))
        return false;

    if (!metadataCheckSuffices &&
        !verify_final_transfer_integrity(sftp_, localPart, remote, policy,
                                         "download", err, &shouldCancel)) {
        return false;
    }

    if (fail_if_transfer_canceled(&shouldCancel, err))
        return false;

    std::string replaceErr;
    if (!localfiles::atomicReplace(localPart, local, replaceErr,
                                   localFileDurability_)) {
        err = std::string("Could not finalize atomic download: ") + replaceErr;
        return false;
    }
    if ((st.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) != 0)
        setLastDownloadModifiedTime(st.mtime);
    return true;
}

// Upload a local file to remote (create/truncate). Reports progress and
// supports cancellation.
bool Libssh2SftpClient::put(
    const std::string &local, const std::string &remote, std::string &err,
    std::function<void(std::size_t, std::size_t)> progress,
    std::function<bool()> shouldCancel, bool resume) {
    auto structuredErrorScope = beginStructuredOperation(err);
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;

    apply_transfer_socket_timeouts(sock_);

    const TransferIntegrityPolicy policy = transferIntegrityPolicy_;
    const std::string remotePart = remote + ".part";

    // Open local for reading
    UniqueFile localFile(openLocalFile(local, "rb"));
    if (!localFile) {
        err = "Could not open local file for reading";
        setLastOperationError(RemoteErrorKind::LocalIo, err, errno);
        return false;
    }

    // Local size
    std::uint64_t localSize = 0;
    std::string sizeErr;
    if (!get_local_file_size(local, localSize, &sizeErr)) {
        err = sizeErr.empty() ? "Could not determine local file size" : sizeErr;
        setLastOperationError(RemoteErrorKind::LocalIo, err, errno);
        return false;
    }
    const std::size_t total = static_cast<std::size_t>(localSize);

    // Resume against remote .part; finalize with a remote rename.
    std::uint64_t startOffset = 0;
    if (resume) {
        LIBSSH2_SFTP_ATTRIBUTES stR{};
        if (libssh2_sftp_stat_ex(sftp_, remotePart.c_str(),
                                 static_cast<unsigned>(remotePart.size()),
                                 LIBSSH2_SFTP_STAT, &stR) == 0) {
            if (stR.flags & LIBSSH2_SFTP_ATTR_SIZE)
                startOffset = static_cast<std::uint64_t>(stR.filesize);
        }
    }

    if (startOffset > 0 && startOffset > localSize) {
        if (policy == TransferIntegrityPolicy::Required) {
            localFile.reset();
            err = "Invalid resume: remote .part is larger than local file";
            return false;
        }
        startOffset = 0;
    }

    if (startOffset > 0 && startOffset < localSize &&
        policy != TransferIntegrityPolicy::Off) {
        const std::uint64_t window =
            std::min<std::uint64_t>(startOffset, 64 * 1024);
        const std::uint64_t start = startOffset - window;
        Sha256Digest lsum{}, rsum{};
        std::string herr;
        const bool lok =
            hash_local_range(local, start, window, lsum, &herr, &shouldCancel);
        const bool rok =
            lok && hash_remote_range(sftp_, remotePart, start, window, rsum,
                                     &herr, &shouldCancel);
        if (shouldCancel && shouldCancel()) {
            localFile.reset();
            err = "Canceled by user";
            return false;
        }
        if (!lok || !rok) {
            if (policy == TransferIntegrityPolicy::Required) {
                localFile.reset();
                err = std::string(
                          "Could not validate resume integrity (upload): ") +
                      herr;
                return false;
            }
            startOffset = 0;
        } else if (lsum != rsum) {
            if (policy == TransferIntegrityPolicy::Required) {
                localFile.reset();
                err = "Integrity check failed in resume (upload): local/remote "
                      "prefix does not match";
                return false;
            }
            startOffset = 0;
        }
    }

    // The .part stays private while it is written; the local file's own
    // permissions are applied just before it replaces the destination.
    const std::uint32_t permissions =
        localfiles::uploadPermissions(localFile.get());
    unsigned long flags = LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT |
                          ((startOffset > 0) ? 0 : LIBSSH2_FXF_TRUNC);
    libssh2detail::UniqueSftpHandle wh(libssh2_sftp_open_ex(
        sftp_, remotePart.c_str(), static_cast<unsigned>(remotePart.size()),
        flags, 0600, LIBSSH2_SFTP_OPENFILE));
    if (!wh) {
        localFile.reset();
        err = "Could not open remote (.part) for writing";
        return false;
    }

    std::size_t done = 0;

    // If resuming, advance local and remote
    if (resume && startOffset > 0 && startOffset < localSize) {
        libssh2_sftp_seek64(wh.get(),
                            static_cast<libssh2_uint64_t>(startOffset));
        std::string seekErr;
        if (!seek_local_file(localFile.get(), startOffset, &seekErr)) {
            const int nativeError = errno;
            err = seekErr.empty() ? "Could not seek local file" : seekErr;
            localFile.reset();
            setLastOperationError(RemoteErrorKind::LocalIo, err, nativeError);
            return false;
        }
        done = static_cast<std::size_t>(startOffset);
    }
    std::vector<char> buf(sftp_transfer_buffer_size(total - done));

    // Under Optional, SSH already protects the bytes in transit, so a
    // whole-file upload only checks that the source did not change while it
    // was read, by its identity rather than by reading it again. A resumed
    // upload also depends on earlier data, so it keeps the full check.
    const bool identityCheckSuffices =
        policy == TransferIntegrityPolicy::Optional && done == 0;
    localfiles::LocalFileIdentity sourceBefore{};
    std::string identityError;
    const bool sourceIdentified =
        identityCheckSuffices &&
        localfiles::localFileIdentity(local, sourceBefore, identityError);

    std::size_t writeSize =
        std::min(libssh2detail::kMinSftpWriteSize, buf.size());
    const auto writeStart = std::chrono::steady_clock::now();
    std::uint64_t writtenNow = 0;
    while (true) {
        size_t n = std::fread(buf.data(), 1, buf.size(), localFile.get());
        if (n > 0) {
            char *p = buf.data();
            size_t remain = n;
            while (remain > 0) {
                if (shouldCancel && shouldCancel()) {
                    err = "Canceled by user";
                    localFile.reset();
                    return false;
                }
                ssize_t w = libssh2_sftp_write(wh.get(), p,
                                               std::min(remain, writeSize));
                if (w < 0) {
                    const bool canceledNow = (shouldCancel && shouldCancel());
                    err = canceledNow ? "Canceled by user"
                                      : "Remote write failed";
                    const RemoteError structuredFailure =
                        classifyStructuredFailure(err, false);
                    localFile.reset();
                    setLastOperationError(structuredFailure);
                    return false;
                }
                remain = remain - static_cast<size_t>(w);
                p = p + w;
                done = done + static_cast<size_t>(w);
                writtenNow += static_cast<std::uint64_t>(w);
                writeSize = libssh2detail::nextSftpWriteSize(
                    writeSize, writtenNow,
                    std::chrono::duration<double>(
                        std::chrono::steady_clock::now() - writeStart)
                        .count(),
                    buf.size());
                if (progress && total)
                    progress(done, total);
            }
        } else {
            if (std::ferror(localFile.get())) {
                const int nativeError = errno;
                err = "Local read failed";
                localFile.reset();
                setLastOperationError(RemoteErrorKind::LocalIo, err,
                                      nativeError);
                return false;
            }
            break; // EOF
        }
    }

    if (identityCheckSuffices) {
        LIBSSH2_SFTP_ATTRIBUTES written{};
        const bool sizeChanged =
            done != total ||
            (libssh2_sftp_fstat_ex(wh.get(), &written, 0) == 0 &&
             (written.flags & LIBSSH2_SFTP_ATTR_SIZE) != 0 &&
             written.filesize != done);
        if (sizeChanged) {
            err = "Final integrity check failed (upload): local or remote "
                  "size changed during transfer";
            localFile.reset();
            return false;
        }
    }

    // Some servers report a failed write only when the handle closes, as NFS
    // or a quota can; the .part must not replace the destination then.
    if (wh.close() != 0) {
        const bool canceledNow = (shouldCancel && shouldCancel());
        err = canceledNow ? "Canceled by user"
                          : "Could not close remote (.part) after writing";
        const RemoteError structuredFailure =
            classifyStructuredFailure(err, false);
        localFile.reset();
        setLastOperationError(structuredFailure);
        return false;
    }
    localFile.reset();

    if (fail_if_transfer_canceled(&shouldCancel, err))
        return false;

    if (identityCheckSuffices) {
        // A source that cannot be identified is not checked, as Optional
        // does with a checksum it cannot compute.
        localfiles::LocalFileIdentity sourceAfter{};
        if (sourceIdentified &&
            localfiles::localFileIdentity(local, sourceAfter, identityError) &&
            sourceAfter != sourceBefore) {
            err = "Final integrity check failed (upload): the local file "
                  "changed during transfer";
            return false;
        }
    } else if (!verify_final_transfer_integrity(sftp_, local, remotePart,
                                                policy, "upload", err,
                                                &shouldCancel)) {
        return false;
    }

    if (fail_if_transfer_canceled(&shouldCancel, err))
        return false;

    // A server that refuses this leaves the file private, which is safer
    // than failing an upload that otherwise succeeded.
    LIBSSH2_SFTP_ATTRIBUTES mode{};
    mode.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
    mode.permissions = permissions;
    (void)libssh2_sftp_stat_ex(sftp_, remotePart.c_str(),
                               static_cast<unsigned>(remotePart.size()),
                               LIBSSH2_SFTP_SETSTAT, &mode);

    std::string rnErr;
    if (!rename_remote(sftp_, remotePart, remote, true, &rnErr)) {
        err =
            std::string("Could not finalize upload (.part -> destination): ") +
            rnErr;
        return false;
    }

    return true;
}

// Lightweight existence check using sftp_stat.
bool Libssh2SftpClient::exists(const std::string &remote_path, bool &isDir,
                               std::string &err) {
    auto structuredErrorScope = beginStructuredOperation(err);
    isDir = false;
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;

    LIBSSH2_SFTP_ATTRIBUTES st{};
    int rc = libssh2_sftp_stat_ex(sftp_, remote_path.c_str(),
                                  static_cast<unsigned>(remote_path.size()),
                                  LIBSSH2_SFTP_STAT, &st);

    if (rc == 0) {
        if (st.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
            isDir = ((st.permissions & LIBSSH2_SFTP_S_IFMT) ==
                     LIBSSH2_SFTP_S_IFDIR);
        }
        return true; // exists
    }

    unsigned long sftp_err = libssh2_sftp_last_error(sftp_);
    if (sftp_err == LIBSSH2_FX_NO_SUCH_FILE ||
        sftp_err == LIBSSH2_FX_NO_SUCH_PATH) {
        err.clear();
        return false; // does not exist
    }

    err = "remote stat failed";
    return false;
}

// Detailed remote metadata (stat-like). Returns false if the path does not
// exist.
bool Libssh2SftpClient::stat(const std::string &remote_path, FileInfo &info,
                             std::string &err) {
    auto structuredErrorScope = beginStructuredOperation(err);
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;

    LIBSSH2_SFTP_ATTRIBUTES st{};
    int rc = libssh2_sftp_stat_ex(sftp_, remote_path.c_str(),
                                  static_cast<unsigned>(remote_path.size()),
                                  LIBSSH2_SFTP_STAT, &st);
    if (rc != 0) {
        unsigned long sftp_err = libssh2_sftp_last_error(sftp_);
        if (sftp_err == LIBSSH2_FX_NO_SUCH_FILE ||
            sftp_err == LIBSSH2_FX_NO_SUCH_PATH) {
            err.clear();
            return false; // missing path
        }
        err = "remote stat failed";
        return false;
    }
    info.name.clear();
    info.is_dir =
        (st.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
            ? ((st.permissions & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFDIR)
            : false;
    if (st.flags & LIBSSH2_SFTP_ATTR_SIZE) {
        info.size = static_cast<std::uint64_t>(st.filesize);
        info.has_size = true;
    } else {
        info.size = 0;
        info.has_size = false;
    }
    info.mtime = (st.flags & LIBSSH2_SFTP_ATTR_ACMODTIME)
                     ? static_cast<std::uint64_t>(st.mtime)
                     : 0;
    info.mode = (st.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                    ? static_cast<std::uint32_t>(st.permissions)
                    : 0;
    if (st.flags & LIBSSH2_SFTP_ATTR_UIDGID) {
        info.uid = static_cast<std::uint32_t>(st.uid);
        info.gid = static_cast<std::uint32_t>(st.gid);
    }
    return true;
}

bool Libssh2SftpClient::chmod(const std::string &remote_path,
                              std::uint32_t mode, std::string &err) {
    auto structuredErrorScope = beginStructuredOperation(err, true);
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;
    // libssh2 exposes chmod through SETSTAT.
    LIBSSH2_SFTP_ATTRIBUTES a{};
    a.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
    a.permissions = mode;
    int rc = libssh2_sftp_stat_ex(sftp_, remote_path.c_str(),
                                  static_cast<unsigned>(remote_path.size()),
                                  LIBSSH2_SFTP_SETSTAT, &a);
    if (rc != 0) {
        err = "Remote chmod failed";
        return false;
    }
    return true;
}

bool Libssh2SftpClient::setTimes(const std::string &remote_path,
                                 std::uint64_t atime, std::uint64_t mtime,
                                 std::string &err) {
    auto structuredErrorScope = beginStructuredOperation(err, true);
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;
    LIBSSH2_SFTP_ATTRIBUTES a{};
    a.flags = LIBSSH2_SFTP_ATTR_ACMODTIME;
    a.atime = static_cast<unsigned long>(atime);
    a.mtime = static_cast<unsigned long>(mtime);
    int rc = libssh2_sftp_stat_ex(sftp_, remote_path.c_str(),
                                  static_cast<unsigned>(remote_path.size()),
                                  LIBSSH2_SFTP_SETSTAT, &a);
    if (rc != 0) {
        err = "Remote setTimes failed";
        return false;
    }
    return true;
}

bool Libssh2SftpClient::chown(const std::string &remote_path, std::uint32_t uid,
                              std::uint32_t gid, std::string &err) {
    auto structuredErrorScope = beginStructuredOperation(err, true);
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;
    constexpr auto kUnchanged = static_cast<std::uint32_t>(-1);
    if (uid == kUnchanged && gid == kUnchanged) {
        // Nothing requested to change.
        err.clear();
        return true;
    }
    // SFTP v3 sets the owner and the group together, so the one that must
    // not change is read first; sending 0 for it would give it to root.
    if (uid == kUnchanged || gid == kUnchanged) {
        LIBSSH2_SFTP_ATTRIBUTES current{};
        if (libssh2_sftp_stat_ex(sftp_, remote_path.c_str(),
                                 static_cast<unsigned>(remote_path.size()),
                                 LIBSSH2_SFTP_STAT, &current) != 0 ||
            (current.flags & LIBSSH2_SFTP_ATTR_UIDGID) == 0) {
            err = "Could not read the current owner and group for remote chown";
            return false;
        }
        if (uid == kUnchanged)
            uid = static_cast<std::uint32_t>(current.uid);
        if (gid == kUnchanged)
            gid = static_cast<std::uint32_t>(current.gid);
    }
    LIBSSH2_SFTP_ATTRIBUTES a{};
    a.flags = LIBSSH2_SFTP_ATTR_UIDGID;
    a.uid = uid;
    a.gid = gid;
    int rc = libssh2_sftp_stat_ex(sftp_, remote_path.c_str(),
                                  static_cast<unsigned>(remote_path.size()),
                                  LIBSSH2_SFTP_SETSTAT, &a);
    if (rc != 0) {
        err = "Remote chown failed";
        return false;
    }
    return true;
}

bool Libssh2SftpClient::mkdir(const std::string &remote_dir, std::string &err,
                              unsigned int mode) {
    auto structuredErrorScope = beginStructuredOperation(err, true);
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;
    if (rejectOversizedPath(remote_dir, err))
        return false;
    int rc = libssh2_sftp_mkdir_ex(sftp_, remote_dir.c_str(),
                                   static_cast<unsigned int>(remote_dir.size()),
                                   mode);
    if (rc != 0) {
        err = "sftp_mkdir failed";
        return false;
    }
    return true;
}

bool Libssh2SftpClient::removeFile(const std::string &remote_path,
                                   std::string &err) {
    auto structuredErrorScope = beginStructuredOperation(err, true);
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;
    int rc =
        libssh2_sftp_unlink_ex(sftp_, remote_path.c_str(),
                               static_cast<unsigned int>(remote_path.size()));
    if (rc != 0) {
        err = "sftp_unlink failed";
        return false;
    }
    return true;
}

bool Libssh2SftpClient::removeDir(const std::string &remote_dir,
                                  std::string &err) {
    auto structuredErrorScope = beginStructuredOperation(err, true);
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;
    if (rejectOversizedPath(remote_dir, err))
        return false;
    int rc =
        libssh2_sftp_rmdir_ex(sftp_, remote_dir.c_str(),
                              static_cast<unsigned int>(remote_dir.size()));
    if (rc != 0) {
        err = "sftp_rmdir failed (directory not empty?)";
        return false;
    }
    return true;
}

bool Libssh2SftpClient::rejectOversizedPath(const std::string &path,
                                            std::string &err) {
    if (path.size() <= std::numeric_limits<unsigned int>::max())
        return false;
    err = "Remote directory path exceeds the libssh2 size limit";
    setLastOperationError(RemoteErrorKind::InvalidRequest, err);
    return true;
}

bool Libssh2SftpClient::rename(const std::string &from, const std::string &to,
                               std::string &err, bool overwrite) {
    auto structuredErrorScope = beginStructuredOperation(err, true);
    if (!ensure_sftp_ready(connected_, sftp_, err))
        return false;
    if (!rename_remote(sftp_, from, to, overwrite, &err)) {
        if (err.empty())
            err = "sftp_rename_ex failed";
        return false;
    }
    return true;
}

bool HasKnownHostEntry(const std::string &khPath, const std::string &host,
                       std::uint16_t port) {
    if (khPath.empty() || host.empty())
        return false;
    std::vector<std::string> lines;
    if (libssh2detail::readKnownHosts(khPath, lines) !=
        libssh2detail::KnownHostsReadStatus::Read) {
        return false;
    }
    const std::vector<std::string> hostTokens = known_hosts_tokens(host, port);
    return std::any_of(lines.begin(), lines.end(),
                       [&hostTokens](const std::string &line) {
                           return line_matches_site_host(line, hostTokens);
                       });
}

bool RemoveKnownHostEntry(const std::string &khPath, const std::string &host,
                          std::uint16_t port, std::string &err) {
    err.clear();
    if (khPath.empty()) {
        err = "known_hosts path is empty";
        return false;
    }
    if (host.empty()) {
        err = "host is empty";
        return false;
    }

    // The file is closed again before it is replaced below, which Windows
    // refuses while it is open.
    std::string content;
    {
        UniqueFile in(openLocalFile(khPath, "rb"));
        if (!in) {
            err = "Could not open known_hosts";
            return false;
        }
        char buffer[8192];
        std::size_t count = 0;
        while ((count = std::fread(buffer, 1, sizeof(buffer), in.get())) > 0)
            content.append(buffer, count);
        if (std::ferror(in.get()) != 0) {
            err = "Could not read known_hosts";
            return false;
        }
    }

    const std::vector<std::string> hostTokens = known_hosts_tokens(host, port);

    const bool hadTrailingLf = !content.empty() && content.back() == '\n';
    std::istringstream inLines(content);
    std::vector<std::string> keptLines;
    std::string line;
    bool removedAny = false;

    while (std::getline(inLines, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        if (line_matches_site_host(line, hostTokens)) {
            removedAny = true;
            continue;
        }
        keptLines.push_back(line);
    }

    if (!removedAny)
        return true;

    std::string rewritten;
    for (std::size_t i = 0; i < keptLines.size(); ++i) {
        if (i > 0)
            rewritten.push_back('\n');
        rewritten += keptLines[i];
    }
    if (hadTrailingLf && !keptLines.empty())
        rewritten.push_back('\n');

    std::string persistErr;
    if (!persist_text_atomic(khPath, rewritten, &persistErr)) {
        err = std::string("Could not write known_hosts: ") + persistErr;
        return false;
    }
    return true;
}
} // namespace openscp
