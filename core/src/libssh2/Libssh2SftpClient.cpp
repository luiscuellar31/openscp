// libssh2 backend: manages TCP socket, SSH session, and SFTP channel.
// Includes keepalive, known_hosts validation, and resume support.
#include "openscp/Libssh2SftpClient.hpp"
#include <libssh2.h>
#include <libssh2_sftp.h>

#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <cerrno>
#ifndef _WIN32
#include <sys/stat.h>
#include <unistd.h>
#include <fcntl.h>
#include <pwd.h>
#include <sys/types.h>
// OpenSSL for HMAC-SHA1 (hashed known_hosts names) and RNG
#include <openssl/hmac.h>
#include <openssl/evp.h>
#include <openssl/rand.h>
#else
#include <windows.h>
#endif
#include <cstdio>
#include <sstream>
#include <array>
#include <ctime>

// POSIX sockets
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

namespace openscp {

// Resolve POSIX home directory robustly (prefer $HOME, fallback to getpwuid)
#ifndef _WIN32
static std::string resolve_posix_home() {
    const char* home = std::getenv("HOME");
    if (home && *home) return std::string(home);
    struct passwd* pw = ::getpwuid(::getuid());
    if (pw && pw->pw_dir && *pw->pw_dir) return std::string(pw->pw_dir);
    return std::string();
}
#endif

// Append host key audit log lines to ~/.openscp/openscp.auth (0600). Best-effort.
static void auditLogHostKey(const std::string& host,
                            uint16_t port,
                            const std::string& algorithm,
                            const std::string& fingerprint,
                            const char* status) {
#ifndef _WIN32
    const char* home = std::getenv("HOME");
    if (!home) return;
    std::string dir = std::string(home) + "/.openscp";
    struct ::stat st{};
    if (::stat(dir.c_str(), &st) != 0) {
        (void)::mkdir(dir.c_str(), 0700);
    } else {
        (void)::chmod(dir.c_str(), 0700);
    }
    std::string path = dir + "/openscp.auth";
    int fd = ::open(path.c_str(), O_WRONLY | O_CREAT | O_APPEND, 0600);
    if (fd < 0) return;
    // Simple timestamp (epoch seconds) for now
    char line[1024];
    std::snprintf(line, sizeof(line),
                  "ts=%ld host=%s port=%u alg=\"%s\" fp=\"%s\" status=%s\n",
                  (long)time(nullptr), host.c_str(), (unsigned)port,
                  algorithm.c_str(), fingerprint.c_str(), status ? status : "unknown");
    (void)::write(fd, line, (unsigned)std::strlen(line));
    ::close(fd);
#else
    (void)host; (void)port; (void)algorithm; (void)fingerprint; (void)status;
#endif
}

#ifndef _WIN32
static std::string posix_err(const char* where) {
    return std::string(where) + ": " + std::strerror(errno);
}

static bool ensure_parent_dir_0700(const std::string& path, std::string* why) {
    std::string dir = path;
    std::size_t p = dir.find_last_of('/');
    if (p == std::string::npos) dir = ".";
    else dir.resize(p);
    struct ::stat st{};
    if (::stat(dir.c_str(), &st) != 0) {
        if (::mkdir(dir.c_str(), 0700) != 0) {
            if (why) *why = posix_err("mkdir");
            return false;
        }
    } else if (::chmod(dir.c_str(), 0700) != 0) {
        if (why) *why = posix_err("chmod(dir)");
        return false;
    }
    return true;
}

static bool fsync_parent_dir(const std::string& path, std::string* why) {
    std::string dir = path;
    std::size_t p = dir.find_last_of('/');
    if (p == std::string::npos) dir = ".";
    else dir.resize(p);
    int dfd = ::open(dir.c_str(), O_RDONLY);
    if (dfd < 0) {
        if (why) *why = posix_err("open(parent)");
        return false;
    }
    if (::fsync(dfd) != 0) {
        if (why) *why = posix_err("fsync(parent)");
        ::close(dfd);
        return false;
    }
    if (::close(dfd) != 0) {
        if (why) *why = posix_err("close(parent)");
        return false;
    }
    return true;
}

static bool write_all(int fd, const char* data, std::size_t len, std::string* why) {
    std::size_t off = 0;
    while (off < len) {
        ssize_t w = ::write(fd, data + off, len - off);
        if (w < 0) {
            if (why) *why = posix_err("write");
            return false;
        }
        off += (std::size_t)w;
    }
    return true;
}
#else
static std::string win_err(const char* where, DWORD code) {
    std::ostringstream oss;
    oss << where << " (GetLastError=" << (unsigned long)code << ")";
    return oss.str();
}
#endif

static bool persist_known_hosts_atomic(LIBSSH2_KNOWNHOSTS* nh,
                                       const std::string& khPath,
                                       std::string* why) {
    if (!nh) {
        if (why) *why = "known_hosts no inicializado";
        return false;
    }
    if (khPath.empty()) {
        if (why) *why = "ruta known_hosts vacía";
        return false;
    }

#ifndef _WIN32
    if (!ensure_parent_dir_0700(khPath, why)) return false;
    std::string tmp = khPath + ".tmpXXXXXX";
    std::vector<char> tmpl(tmp.begin(), tmp.end());
    tmpl.push_back('\0');
    int fd = ::mkstemp(tmpl.data());
    if (fd < 0) {
        if (why) *why = posix_err("mkstemp");
        return false;
    }
    const std::string tmpPath(tmpl.data());
    if (::close(fd) != 0) {
        if (why) *why = posix_err("close(tmp)");
        (void)::unlink(tmpPath.c_str());
        return false;
    }

    if (libssh2_knownhost_writefile(nh, tmpPath.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH) != 0) {
        if (why) *why = "libssh2_knownhost_writefile falló";
        (void)::unlink(tmpPath.c_str());
        return false;
    }

    if (::chmod(tmpPath.c_str(), 0600) != 0) {
        if (why) *why = posix_err("chmod(tmp)");
        (void)::unlink(tmpPath.c_str());
        return false;
    }

    int fdw = ::open(tmpPath.c_str(), O_WRONLY);
    if (fdw < 0) {
        if (why) *why = posix_err("open(tmp)");
        (void)::unlink(tmpPath.c_str());
        return false;
    }
    if (::fchmod(fdw, 0600) != 0) {
        if (why) *why = posix_err("fchmod(tmp)");
        ::close(fdw);
        (void)::unlink(tmpPath.c_str());
        return false;
    }
    if (::fsync(fdw) != 0) {
        if (why) *why = posix_err("fsync(tmp)");
        ::close(fdw);
        (void)::unlink(tmpPath.c_str());
        return false;
    }
    if (::close(fdw) != 0) {
        if (why) *why = posix_err("close(tmp)");
        (void)::unlink(tmpPath.c_str());
        return false;
    }

    if (::rename(tmpPath.c_str(), khPath.c_str()) != 0) {
        if (why) *why = posix_err("rename(tmp->known_hosts)");
        (void)::unlink(tmpPath.c_str());
        return false;
    }
    if (!fsync_parent_dir(khPath, why)) return false;
    return true;
#else
    std::string tmpPath = khPath + ".tmp";
    if (libssh2_knownhost_writefile(nh, tmpPath.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH) != 0) {
        if (why) *why = "libssh2_knownhost_writefile falló";
        return false;
    }
    HANDLE h = CreateFileA(tmpPath.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ,
                           NULL, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, NULL);
    if (h == INVALID_HANDLE_VALUE) {
        DWORD ec = GetLastError();
        (void)DeleteFileA(tmpPath.c_str());
        if (why) *why = win_err("CreateFile(tmp)", ec);
        return false;
    }
    if (!FlushFileBuffers(h)) {
        DWORD ec = GetLastError();
        CloseHandle(h);
        (void)DeleteFileA(tmpPath.c_str());
        if (why) *why = win_err("FlushFileBuffers(tmp)", ec);
        return false;
    }
    if (!CloseHandle(h)) {
        DWORD ec = GetLastError();
        (void)DeleteFileA(tmpPath.c_str());
        if (why) *why = win_err("CloseHandle(tmp)", ec);
        return false;
    }
    if (!MoveFileExA(tmpPath.c_str(), khPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_COPY_ALLOWED)) {
        DWORD ec = GetLastError();
        (void)DeleteFileA(tmpPath.c_str());
        if (why) *why = win_err("MoveFileEx(tmp->known_hosts)", ec);
        return false;
    }
    return true;
#endif
}

#ifndef _WIN32
static bool persist_text_atomic(const std::string& path,
                                const std::string& content,
                                std::string* why) {
    if (path.empty()) {
        if (why) *why = "ruta destino vacía";
        return false;
    }
    if (!ensure_parent_dir_0700(path, why)) return false;

    std::string tmp = path + ".tmpXXXXXX";
    std::vector<char> tmpl(tmp.begin(), tmp.end());
    tmpl.push_back('\0');
    int fd = ::mkstemp(tmpl.data());
    if (fd < 0) {
        if (why) *why = posix_err("mkstemp");
        return false;
    }
    const std::string tmpPath(tmpl.data());

    if (!write_all(fd, content.data(), content.size(), why)) {
        ::close(fd);
        (void)::unlink(tmpPath.c_str());
        return false;
    }
    if (::fchmod(fd, 0600) != 0) {
        if (why) *why = posix_err("fchmod(tmp)");
        ::close(fd);
        (void)::unlink(tmpPath.c_str());
        return false;
    }
    if (::fsync(fd) != 0) {
        if (why) *why = posix_err("fsync(tmp)");
        ::close(fd);
        (void)::unlink(tmpPath.c_str());
        return false;
    }
    if (::close(fd) != 0) {
        if (why) *why = posix_err("close(tmp)");
        (void)::unlink(tmpPath.c_str());
        return false;
    }
    if (::rename(tmpPath.c_str(), path.c_str()) != 0) {
        if (why) *why = posix_err("rename(tmp->known_hosts)");
        (void)::unlink(tmpPath.c_str());
        return false;
    }
    if (!fsync_parent_dir(path, why)) return false;
    return true;
}
#endif

// Simple Base64 encoder (standard, with '=' padding)
static std::string b64encode(const unsigned char* data, std::size_t len) {
    static constexpr char kTable[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((len + 2) / 3) * 4);
    std::size_t i = 0;
    while (i + 3 <= len) {
        unsigned int v = (data[i] << 16) | (data[i+1] << 8) | data[i+2];
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back(kTable[v & 0x3F]);
        i += 3;
    }
    if (i + 1 == len) {
        unsigned int v = (data[i] << 16);
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back('=');
        out.push_back('=');
    } else if (i + 2 == len) {
        unsigned int v = (data[i] << 16) | (data[i+1] << 8);
        out.push_back(kTable[(v >> 18) & 0x3F]);
        out.push_back(kTable[(v >> 12) & 0x3F]);
        out.push_back(kTable[(v >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

// Preference: write hashed hostnames to known_hosts (OpenSSH style) unless disabled via env
static bool useHashedKnownHosts() {
    const char* v = std::getenv("OPEN_SCP_KNOWNHOSTS_PLAIN");
    // If env var is set to '1', force PLAIN; otherwise prefer hashed
    return !(v && *v == '1');
}

// Global libssh2 initialization (once per process)
static bool g_libssh2_inited = false;

#ifndef _WIN32
// Compute OpenSSH hashed hostname token: |1|base64(salt)|base64(HMAC_SHA1(salt, host))
static std::string openssh_hash_hostname(const std::string& host) {
    unsigned char salt[20];
    if (RAND_bytes(salt, sizeof(salt)) != 1) {
        // fallback: pseudo-random
        for (size_t i = 0; i < sizeof(salt); ++i) salt[i] = (unsigned char)(rand() & 0xFF);
    }
    unsigned char mac[EVP_MAX_MD_SIZE]; unsigned int maclen = 0;
    HMAC(EVP_sha1(), salt, (int)sizeof(salt),
         reinterpret_cast<const unsigned char*>(host.data()), (int)host.size(),
         mac, &maclen);
    std::string s_salt = b64encode(salt, sizeof(salt));
    std::string s_mac  = b64encode(mac, maclen);
    return std::string("|1|") + s_salt + "|" + s_mac;
}
#endif

// Context for keyboard-interactive: respond with username/password based on the prompt
struct KbdIntCtx {
    const char* user;
    const char* pass;
    const KbdIntPromptsCB* cb; // optional: UI callback for prompts
};

// Keyboard-interactive callback: respond to prompts with username/password based on the text
static void kbint_password_callback(const char* name, int name_len,
                                    const char* instruction, int instruction_len,
                                    int num_prompts,
                                    const LIBSSH2_USERAUTH_KBDINT_PROMPT* prompts,
                                    LIBSSH2_USERAUTH_KBDINT_RESPONSE* responses,
                                    void** abstract) {
    if (!abstract || !*abstract) return;
    const KbdIntCtx* ctx = static_cast<const KbdIntCtx*>(*abstract);
    const char* user = ctx ? ctx->user : nullptr;
    const char* pass = ctx ? ctx->pass : nullptr;
    const size_t ulen = user ? std::strlen(user) : 0;
    const size_t plen = pass ? std::strlen(pass) : 0;

    // If a UI callback is provided, give it a chance to answer the prompts.
    if (ctx && ctx->cb && *(ctx->cb) && num_prompts > 0) {
        std::vector<std::string> ptxts;
        ptxts.reserve((size_t)num_prompts);
        for (int i = 0; i < num_prompts; ++i) {
            const char* pt = (prompts && prompts[i].text) ? reinterpret_cast<const char*>(prompts[i].text) : "";
            ptxts.emplace_back(pt);
        }
        std::vector<std::string> answers;
        std::string nm = (name && name_len > 0) ? std::string(name, (size_t)name_len) : std::string();
        std::string ins = (instruction && instruction_len > 0) ? std::string(instruction, (size_t)instruction_len) : std::string();
        bool ok = (*(ctx->cb))(nm, ins, ptxts, answers);
        if (ok && (int)answers.size() >= num_prompts) {
            for (int i = 0; i < num_prompts; ++i) {
                const std::string& a = answers[(size_t)i];
                if (a.empty()) {
                    responses[i].text = nullptr;
                    responses[i].length = 0;
                    continue;
                }
                char* buf = static_cast<char*>(std::malloc(a.size() + 1));
                if (!buf) {
                    responses[i].text = nullptr;
                    responses[i].length = 0;
                    continue;
                }
                std::memcpy(buf, a.data(), a.size());
                buf[a.size()] = '\0';
                responses[i].text = buf;
                responses[i].length = (unsigned int)a.size();
            }
            // Best-effort: scrub answers after copying into libssh2 buffers
            for (std::string& a : answers) {
                if (!a.empty()) {
                    volatile char* p = a.data();
                    for (size_t i = 0; i < a.size(); ++i) p[i] = 0;
                    a.clear();
                    a.shrink_to_fit();
                }
            }
            return;
        }
        // If the callback could not answer, fall back to the simple heuristic
    }
    for (int i = 0; i < num_prompts; ++i) {
        const char* prompt = (prompts && prompts[i].text) ? reinterpret_cast<const char*>(prompts[i].text) : "";
        bool wantUser = false;
        // Simple heuristic: if the prompt mentions "user" or "name", send username; otherwise send password
        for (const char* p = prompt; *p; ++p) {
            char c = *p;
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            // search simple lowercase sequences: "user" or "name" in the prompt
            if (c == 'u' && p[1] == 's' && p[2] == 'e' && p[3] == 'r') {
                wantUser = true;
                break;
            }
            if (c == 'n' && p[1] == 'a' && p[2] == 'm' && p[3] == 'e') {
                wantUser = true;
                break;
            }
        }
        const char* ans = wantUser ? user : pass;
        const size_t alen = wantUser ? ulen : plen;
        if (!ans || alen == 0) {
            responses[i].text = nullptr;
            responses[i].length = 0;
            continue;
        }
        char* buf = static_cast<char*>(std::malloc(alen + 1));
        if (!buf) {
            responses[i].text = nullptr;
            responses[i].length = 0;
            continue;
        }
        std::memcpy(buf, ans, alen);
        buf[alen] = '\0';
        responses[i].text = buf;
        responses[i].length = (unsigned int)alen;
    }
}

Libssh2SftpClient::Libssh2SftpClient() {
    if (!g_libssh2_inited) {
        int rc = libssh2_init(0);
        (void)rc;
        g_libssh2_inited = true;
    }
}

Libssh2SftpClient::~Libssh2SftpClient() {
    disconnect();
}

bool Libssh2SftpClient::tcpConnect(const std::string& host, uint16_t port, std::string& err) {
    struct addrinfo hints{};
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%u", static_cast<unsigned>(port));

    struct addrinfo* res = nullptr;
    int gai = getaddrinfo(host.c_str(), portStr, &hints, &res);
    if (gai != 0) {
        err = std::string("getaddrinfo: ") + gai_strerror(gai);
        return false;
    }

    int s = -1;
    for (auto rp = res; rp != nullptr; rp = rp->ai_next) {
        s = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s == -1) continue;
        // Note: avoid setting SO_RCVTIMEO/SO_SNDTIMEO during authentication because
        // it may interfere with userauth on some servers/libc. Rely on
        // libssh2_session_set_timeout (when available) to manage timeouts.
        // Enable TCP keepalive
        int opt = 1;
        ::setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
#ifdef __APPLE__
        int idle = 60;
        ::setsockopt(s, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
#elif defined(__linux__)
        int idle = 60, intvl = 10, cnt = 3;
        ::setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
        ::setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
        ::setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
        if (::connect(s, rp->ai_addr, rp->ai_addrlen) == 0) {
            // connected
            sock_ = s;
            freeaddrinfo(res);
            return true;
        }
        ::close(s);
        s = -1;
    }
    freeaddrinfo(res);
    err = "No se pudo conectar al host/puerto.";
    return false;
}

bool Libssh2SftpClient::sshHandshakeAuth(const SessionOptions& opt, std::string& err) {
    session_ = libssh2_session_init();
    if (!session_) {
        err = "libssh2_session_init falló";
        return false;
    }

    // Prefer modern algorithms (host keys, ciphers, MACs) and avoid DSA where possible
#ifdef LIBSSH2_METHOD_HOSTKEY
    // Host keys: ensure no ssh-dss, avoid ssh-rsa(SHA-1); prefer modern with safe fallbacks
    (void)libssh2_session_method_pref(session_, LIBSSH2_METHOD_HOSTKEY,
        "ssh-ed25519,ecdsa-sha2-nistp256,rsa-sha2-512,rsa-sha2-256,ecdsa-sha2-nistp384,ecdsa-sha2-nistp521");
#endif
#ifdef LIBSSH2_METHOD_KEX
    (void)libssh2_session_method_pref(session_, LIBSSH2_METHOD_KEX,
        "curve25519-sha256,ecdh-sha2-nistp256,diffie-hellman-group14-sha256");
#endif
#ifdef LIBSSH2_METHOD_CRYPT_CS
    (void)libssh2_session_method_pref(session_, LIBSSH2_METHOD_CRYPT_CS,
        "chacha20-poly1305@openssh.com,aes256-gcm@openssh.com,aes128-gcm@openssh.com,aes256-ctr,aes128-ctr");
#endif
#ifdef LIBSSH2_METHOD_CRYPT_SC
    (void)libssh2_session_method_pref(session_, LIBSSH2_METHOD_CRYPT_SC,
        "chacha20-poly1305@openssh.com,aes256-gcm@openssh.com,aes128-gcm@openssh.com,aes256-ctr,aes128-ctr");
#endif
#ifdef LIBSSH2_METHOD_MAC_CS
    (void)libssh2_session_method_pref(session_, LIBSSH2_METHOD_MAC_CS,
        "hmac-sha2-512,hmac-sha2-256");
#endif
#ifdef LIBSSH2_METHOD_MAC_SC
    (void)libssh2_session_method_pref(session_, LIBSSH2_METHOD_MAC_SC,
        "hmac-sha2-512,hmac-sha2-256");
#endif

    // Handshake
    if (libssh2_session_handshake(session_, sock_) != 0) {
        err = "SSH handshake falló";
        return false;
    }

    // Ensure blocking mode and a reasonable timeout to avoid EAGAIN during auth
    libssh2_session_set_blocking(session_, 1);
#ifdef LIBSSH2_SESSION_TIMEOUT
    libssh2_session_set_timeout(session_, 20000); // 20s
#endif

    // SSH keepalive: request libssh2 to send messages every 30s if the peer allows it
    libssh2_keepalive_config(session_, 1, 30);

    // Host key verification according to known_hosts policy
    if (opt.known_hosts_policy == openscp::KnownHostsPolicy::Off) {
        // No verification: do not consult known_hosts and do not fail on mismatch
    } else {
        LIBSSH2_KNOWNHOSTS* nh = libssh2_knownhost_init(session_);
        if (!nh) {
            err = "No se pudo inicializar known_hosts";
            return false;
        }

        // Effective path
        std::string khPath;
        if (opt.known_hosts_path.has_value()) {
            khPath = *opt.known_hosts_path;
        } else {
#ifndef _WIN32
            std::string home = resolve_posix_home();
            if (!home.empty()) khPath = home + "/.ssh/known_hosts";
#endif
        }

        bool khLoaded = false;
        if (!khPath.empty()) {
            khLoaded = (libssh2_knownhost_readfile(nh, khPath.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH) >= 0);
        }
        if (!khLoaded && opt.known_hosts_policy == openscp::KnownHostsPolicy::Strict) {
            libssh2_knownhost_free(nh);
            err = "known_hosts no disponible o ilegible (política estricta)";
            return false;
        }

        size_t keylen = 0;
        int keytype = 0;
        const char* hostkey = (const char*)libssh2_session_hostkey(session_, &keylen, &keytype);
        if (!hostkey || keylen == 0) {
            libssh2_knownhost_free(nh);
            err = "No se pudo obtener host key";
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
#ifdef LIBSSH2_KNOWNHOST_KEY_ECDSA_256
                alg = LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
                algDisplay = "ecdsa-sha2-nistp256";
                break;
#else
                alg = 0;
                break;
#endif
            case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
#ifdef LIBSSH2_KNOWNHOST_KEY_ECDSA_384
                alg = LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
                algDisplay = "ecdsa-sha2-nistp384";
                break;
#else
                alg = 0;
                break;
#endif
            case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
#ifdef LIBSSH2_KNOWNHOST_KEY_ECDSA_521
                alg = LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
                algDisplay = "ecdsa-sha2-nistp521";
                break;
#else
                alg = 0;
                break;
#endif
            case LIBSSH2_HOSTKEY_TYPE_ED25519:
#ifdef LIBSSH2_KNOWNHOST_KEY_ED25519
                alg = LIBSSH2_KNOWNHOST_KEY_ED25519;
                algDisplay = "ssh-ed25519";
                break;
#else
                alg = 0;
                break;
#endif
            default:
                alg = 0;
                break;
        }

        int typemask_plain = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | alg;
        int typemask_hash = LIBSSH2_KNOWNHOST_TYPE_SHA1 | LIBSSH2_KNOWNHOST_KEYENC_RAW | alg;
        std::fprintf(stderr, "[OpenSCP] HostKey map: keytype=%d algMask=%d display=%s\n", keytype, alg, algDisplay.c_str());

        struct libssh2_knownhost* host = nullptr;
        int check = libssh2_knownhost_checkp(nh, opt.host.c_str(), opt.port,
                                             hostkey, keylen, typemask_plain, &host);
        if (check != LIBSSH2_KNOWNHOST_CHECK_MATCH) {
            check = libssh2_knownhost_checkp(nh, opt.host.c_str(), opt.port,
                                             hostkey, keylen, typemask_hash, &host);
        }

        if (check == LIBSSH2_KNOWNHOST_CHECK_MATCH) {
            libssh2_knownhost_free(nh);
        } else if (opt.known_hosts_policy == openscp::KnownHostsPolicy::AcceptNew &&
                   (check == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND || check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH)) {
            // TOFU: ask the user for confirmation if a callback is available
            std::string algName;
            switch (keytype) {
                case LIBSSH2_HOSTKEY_TYPE_RSA: algName = "RSA"; break;
                case LIBSSH2_HOSTKEY_TYPE_DSS: algName = "DSA"; break;
                case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: algName = "ECDSA-256"; break;
                case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: algName = "ECDSA-384"; break;
                case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: algName = "ECDSA-521"; break;
                case LIBSSH2_HOSTKEY_TYPE_ED25519: algName = "ED25519"; break;
                default: algName = "DESCONOCIDO"; break;
            }
            // Obtain SHA256 fingerprint if available (Base64 by default); optional hex for compat
            std::string fpStr;
            std::string fpB64;
#ifdef LIBSSH2_HOSTKEY_HASH_SHA256
            const unsigned char* h = (const unsigned char*)libssh2_hostkey_hash(session_, LIBSSH2_HOSTKEY_HASH_SHA256);
            if (h) {
                fpB64 = b64encode(h, 32);
                // Strip padding '=' to match OpenSSH presentation
                while (!fpB64.empty() && fpB64.back() == '=') fpB64.pop_back();
                bool hexOnly = (std::getenv("OPEN_SCP_FP_HEX_ONLY") && *std::getenv("OPEN_SCP_FP_HEX_ONLY") == '1');
                if (!hexOnly) hexOnly = opt.show_fp_hex;
                if (hexOnly) {
                    std::ostringstream oss;
                    for (int i = 0; i < 32; ++i) {
                        if (i) oss << ':';
                        char b[4];
                        std::snprintf(b, sizeof(b), "%02X", (unsigned)h[i]);
                        oss << b;
                    }
                    fpStr = std::string("SHA256:") + oss.str();
                } else {
                    fpStr = std::string("SHA256:") + fpB64;
                }
            }
#else
            const unsigned char* h = (const unsigned char*)libssh2_hostkey_hash(session_, LIBSSH2_HOSTKEY_HASH_SHA1);
            if (h) {
                std::ostringstream oss;
                oss << "SHA1:";
                for (int i = 0; i < 20; ++i) {
                    if (i) oss << ':';
                    char b[4];
                    std::snprintf(b, sizeof(b), "%02X", (unsigned)h[i]);
                    oss << b;
                }
                fpStr = oss.str();
            }
#endif
            // Bits (approx): length of raw hostkey bytes
            int keyBits = 0;
            switch (keytype) {
                case LIBSSH2_HOSTKEY_TYPE_ED25519: keyBits = 256; break;
                case LIBSSH2_HOSTKEY_TYPE_ECDSA_256: keyBits = 256; break;
                case LIBSSH2_HOSTKEY_TYPE_ECDSA_384: keyBits = 384; break;
                case LIBSSH2_HOSTKEY_TYPE_ECDSA_521: keyBits = 521; break;
                default: keyBits = (int)keylen * 8; break;
            }
            std::ostringstream algWithBits;
            algWithBits << algDisplay << " (" << keyBits << "-bit)";

            bool canSaveInitial = !khPath.empty() && ((alg != 0)
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
                || (keytype == LIBSSH2_HOSTKEY_TYPE_ED25519)
#endif
            );
            bool confirmed = false;
            if (opt.hostkey_confirm_cb) {
                confirmed = opt.hostkey_confirm_cb(opt.host, opt.port, algWithBits.str(), fpStr, canSaveInitial);
            }
            if (!confirmed) {
                libssh2_knownhost_free(nh);
                auditLogHostKey(opt.host, opt.port, algWithBits.str(), fpStr, "rejected");
                err = "Host desconocido: huella no confirmada por el usuario";
                return false;
            }

            // If saving is not possible (unsupported alg or no kh path): allow only if user explicitly accepted
            if (!canSaveInitial) {
                libssh2_knownhost_free(nh);
                std::fprintf(stderr, "[OpenSCP] Saving hostkey skipped: no khPath or unsupported alg.\n");
                if (opt.hostkey_status_cb) {
                    std::string why = khPath.empty() ? "Ruta known_hosts no definida" : "Algoritmo de hostkey sin soporte de known_hosts en libssh2";
                    opt.hostkey_status_cb(std::string("No se podrá guardar la huella: ") + why);
                }
                auditLogHostKey(opt.host, opt.port, algWithBits.str(), fpStr, "skipped");
                // Continue without persisting
            } else {
                bool saved = false;
                // Prepare add/typemask
                bool preferHashed = opt.known_hosts_hash_names;
                if (const char* ev = std::getenv("OPEN_SCP_KNOWNHOSTS_PLAIN")) {
                    if (*ev == '1') preferHashed = false; else if (*ev == '0') preferHashed = true;
                }
                int nameMask = preferHashed ? LIBSSH2_KNOWNHOST_TYPE_SHA1
                                            : LIBSSH2_KNOWNHOST_TYPE_PLAIN;
                int addMask = nameMask | LIBSSH2_KNOWNHOST_KEYENC_RAW | alg;
                // host string with port when non-default
                std::string hostForKnown = opt.host;
                if (opt.port != 22) hostForKnown = std::string("[") + opt.host + "]:" + std::to_string(opt.port);

                // Add entry
                if (alg != 0 && libssh2_knownhost_addc(nh, hostForKnown.c_str(), nullptr,
                                           hostkey, (size_t)keylen,
                                           nullptr, 0, addMask, nullptr) == 0) {
                    std::string persistErr;
                    saved = persist_known_hosts_atomic(nh, khPath, &persistErr);
                    if (!saved && opt.hostkey_status_cb) {
                        opt.hostkey_status_cb(std::string("No se pudo guardar known_hosts: ") + persistErr);
                    }
                }
                // Manual ED25519 fallback when libssh2 lacks knownhosts alg mask
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
                if (!saved && keytype == LIBSSH2_HOSTKEY_TYPE_ED25519) {
                    std::fprintf(stderr, "[OpenSCP] Manual known_hosts write for ssh-ed25519 (libssh2 KH mask unavailable). keylen=%zu\n", (size_t)keylen);
                    std::fprintf(stderr, "[OpenSCP] key head: %02X %02X %02X %02X %02X %02X %02X %02X (base64 len=%zu)\n",
                                  (unsigned)((const unsigned char*)hostkey)[0], (unsigned)((const unsigned char*)hostkey)[1],
                                  (unsigned)((const unsigned char*)hostkey)[2], (unsigned)((const unsigned char*)hostkey)[3],
                                  (unsigned)((const unsigned char*)hostkey)[4], (unsigned)((const unsigned char*)hostkey)[5],
                                  (unsigned)((const unsigned char*)hostkey)[6], (unsigned)((const unsigned char*)hostkey)[7],
                                  b64encode((const unsigned char*)hostkey, (size_t)keylen).size());
                    // Fallback: write OpenSSH line atomically (hashed or plain)
#ifndef _WIN32
                    // Read existing
                    std::string existing;
                    int rfd = ::open(khPath.c_str(), O_RDONLY);
                    if (rfd >= 0) { char buf[4096]; ssize_t n; while ((n=::read(rfd,buf,sizeof(buf)))>0) existing.append(buf,buf+n); ::close(rfd);}                    
                    // Build lines
                    std::vector<std::string> lines; { std::string tmp; for(char c:existing){ if(c=='\r') continue; if(c=='\n'){ lines.push_back(tmp); tmp.clear(); } else tmp.push_back(c);} if(!existing.empty() && existing.back()!='\n') lines.push_back(tmp); }
                    std::string b64 = b64encode((const unsigned char*)hostkey, (size_t)keylen);
                    std::string hostToken;
                    if (preferHashed) {
                        hostToken = openssh_hash_hostname(hostForKnown);
                        // note: hashed entries won't be deduplicated (new salt)
                        lines.push_back(hostToken + " ssh-ed25519 " + b64);
                    } else {
                        hostToken = hostForKnown;
                        std::string prefix = hostToken + " ssh-ed25519 ";
                        bool replaced=false; for(std::string& ln:lines){ if(ln.rfind(prefix,0)==0){ ln=prefix+b64; replaced=true; break; } }
                        if(!replaced) lines.push_back(prefix+b64);
                    }
                    std::string content;
                    content.reserve(lines.size() * 64);
                    for (const std::string& ln : lines) {
                        if (!ln.empty()) content.append(ln);
                        content.push_back('\n');
                    }
                    std::string persistErr;
                    saved = persist_text_atomic(khPath, content, &persistErr);
                    if (!saved && opt.hostkey_status_cb) {
                        opt.hostkey_status_cb(std::string("No se pudo guardar known_hosts: ") + persistErr);
                    }
#endif
                }
#endif

                libssh2_knownhost_free(nh);
                if (saved) {
                    auditLogHostKey(opt.host, opt.port, algWithBits.str(), fpStr, "saved");
                } else {
                    // Saving failed: require explicit confirmation to proceed without saving
                    bool proceed = (opt.hostkey_confirm_cb && opt.hostkey_confirm_cb(opt.host, opt.port, algWithBits.str(), fpStr, /*canSave*/false));
                    if (!proceed) {
                        auditLogHostKey(opt.host, opt.port, algWithBits.str(), fpStr, "save_failed");
                        err = "No se pudo guardar la huella en known_hosts";
                        return false;
                    }
                    auditLogHostKey(opt.host, opt.port, algWithBits.str(), fpStr, "skipped");
                }
            }
        } else {
            libssh2_knownhost_free(nh);
            // Policy handling for non-match cases
            // - Strict: fail on mismatch or notfound
            // - AcceptNew (TOFU): already handled NOTFOUND/MISMATCH above with user confirmation
            if (opt.known_hosts_policy == openscp::KnownHostsPolicy::Strict) {
                err = (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH)
                          ? "Host key no coincide con known_hosts"
                          : "Host desconocido en known_hosts";
                return false;
            }
        }
    }

    // Authentication: prefer the method explicitly provided by the user.
    // 1) If a private key is specified: use it first.
    // 2) If a password is specified: try password first; if connection remains open but fails, query methods and try kbd-int.
    // 3) If no credentials: try ssh-agent (if the server allows); otherwise, error.
    if (opt.private_key_path.has_value()) {
        const char* passphrase = opt.private_key_passphrase ? opt.private_key_passphrase->c_str() : nullptr;
        int rc = libssh2_userauth_publickey_fromfile(session_,
                                                     opt.username.c_str(),
                                                     NULL,  // public key path (NULL: derived from the private key)
                                                     opt.private_key_path->c_str(),
                                                     passphrase);
        if (rc != 0) {
            err = "Auth por clave falló";
            return false;
        }
    } else {
        // Keep the methods list ONLY when necessary.
        std::string authlist;
        auto hasMethod = [&](const char* m) { return !authlist.empty() && authlist.find(m) != std::string::npos; };

        // If the user provided a password: try it first to avoid exhausting attempts with 'none' or agent.
        if (opt.password.has_value()) {
            int rc_pw = -1;
            int rc_kbd = -1;
            std::string pwLastErr;
            long pwErrno = 0;
            std::string kbLastErr;
            long kbErrno = 0;

            auto do_password = [&]() {
                for (;;) {
                    rc_pw = libssh2_userauth_password(session_, opt.username.c_str(), opt.password->c_str());
                    if (rc_pw != LIBSSH2_ERROR_EAGAIN) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                char* emsgPtr = nullptr;
                int emlen = 0;
                (void)libssh2_session_last_error(session_, &emsgPtr, &emlen, 0);
                if (emsgPtr && emlen > 0) pwLastErr.assign(emsgPtr, (size_t)emlen);
                pwErrno = libssh2_session_last_errno(session_);
            };
            auto do_kbdint = [&]() {
                KbdIntCtx ctx{opt.username.c_str(), opt.password->c_str(), &opt.keyboard_interactive_cb};
                void** abs = libssh2_session_abstract(session_);
                if (abs) *abs = &ctx;
                for (;;) {
                    rc_kbd = libssh2_userauth_keyboard_interactive(session_, opt.username.c_str(), kbint_password_callback);
                    if (rc_kbd != LIBSSH2_ERROR_EAGAIN) break;
                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
                }
                if (abs) *abs = nullptr;
                char* emsgPtr = nullptr;
                int emlen = 0;
                (void)libssh2_session_last_error(session_, &emsgPtr, &emlen, 0);
                if (emsgPtr && emlen > 0) kbLastErr.assign(emsgPtr, (size_t)emlen);
                kbErrno = libssh2_session_last_errno(session_);
            };

            // Attempt password directly (without prior userauth_list).
            do_password();

            // If the server closed after the password attempt, stop: the rest would cascade-fail.
            if (rc_pw == LIBSSH2_ERROR_SOCKET_DISCONNECT ||
                rc_pw == LIBSSH2_ERROR_SOCKET_SEND ||
                rc_pw == LIBSSH2_ERROR_SOCKET_RECV) {
                err = "El servidor cerró la conexión tras el intento de password";
                return false;
            }

            // If password failed but the session is still alive, NOW query methods and try keyboard-interactive if applicable.
            if (rc_pw != 0) {
                char* methods = libssh2_userauth_list(session_, opt.username.c_str(), (unsigned)opt.username.size());
                authlist = methods ? std::string(methods) : std::string();
                if (hasMethod("keyboard-interactive")) {
                    do_kbdint();
                }
            }

            if (rc_pw != 0 && rc_kbd != 0) {
                // As a last resort, try ssh-agent if the server allows (limit identities).
                // We need authlist to know if publickey is allowed.
                if (authlist.empty()) {
                    char* methods = libssh2_userauth_list(session_, opt.username.c_str(), (unsigned)opt.username.size());
                    authlist = methods ? std::string(methods) : std::string();
                }

                bool authed = false;
                if (hasMethod("publickey")) {
                    LIBSSH2_AGENT* agent = libssh2_agent_init(session_);
                    if (agent && libssh2_agent_connect(agent) == 0) {
                        if (libssh2_agent_list_identities(agent) == 0) {
                            struct libssh2_agent_publickey* identity = nullptr;
                            struct libssh2_agent_publickey* prev = nullptr;
                            int tries = 0;
                            const int kMaxAgentTries = 3; // conservative limit
                            while (libssh2_agent_get_identity(agent, &identity, prev) == 0 && tries < kMaxAgentTries) {
                                prev = identity;
                                ++tries;
                                int arc = -1;
                                for (;;) {
                                    arc = libssh2_agent_userauth(agent, opt.username.c_str(), identity);
                                    if (arc != LIBSSH2_ERROR_EAGAIN) break;
                                    std::this_thread::sleep_for(std::chrono::milliseconds(50));
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

                if (!authed) {
                    // Save libssh2 error message for diagnostics
                    char* emsgPtr = nullptr;
                    int emlen = 0;
                    (void)libssh2_session_last_error(session_, &emsgPtr, &emlen, 0);
                    std::string lastErr = (emsgPtr && emlen > 0) ? std::string(emsgPtr, (size_t)emlen) : std::string();
                    long lastErrno = libssh2_session_last_errno(session_);
                    // Include return codes for diagnostics
                    err = std::string("Auth por password/kbdint falló") +
                          (authlist.empty() ? std::string() : (std::string(" (métodos: ") + authlist + ")")) +
                          (lastErr.empty() ? std::string() : (std::string(" — ") + lastErr)) +
                          (std::string(" [rc_pw=") + std::to_string(rc_pw) + ", rc_kbd=" + std::to_string(rc_kbd) + ", errno=" + std::to_string(lastErrno) + "]") +
                          (pwLastErr.empty() ? std::string() : (std::string(" {pw='") + pwLastErr + "' errno=" + std::to_string(pwErrno) + "}")) +
                          (kbLastErr.empty() ? std::string() : (std::string(" {kbd='") + kbLastErr + "' errno=" + std::to_string(kbErrno) + "}"));
                    return false;
                }
            }
        } else {
            // Without a password: query methods and then try ssh-agent if the server allows.
            if (authlist.empty()) {
                char* methods = libssh2_userauth_list(session_, opt.username.c_str(), (unsigned)opt.username.size());
                authlist = methods ? std::string(methods) : std::string();
            }
            bool authed = false;
            if (hasMethod("publickey")) {
                LIBSSH2_AGENT* agent = libssh2_agent_init(session_);
                if (agent && libssh2_agent_connect(agent) == 0) {
                    if (libssh2_agent_list_identities(agent) == 0) {
                        struct libssh2_agent_publickey* identity = nullptr;
                        struct libssh2_agent_publickey* prev = nullptr;
                        int tries = 0;
                        const int kMaxAgentTries = 3;
                        while (libssh2_agent_get_identity(agent, &identity, prev) == 0 && tries < kMaxAgentTries) {
                            prev = identity;
                            ++tries;
                            int arc = -1;
                            for (;;) {
                                arc = libssh2_agent_userauth(agent, opt.username.c_str(), identity);
                                if (arc != LIBSSH2_ERROR_EAGAIN) break;
                                std::this_thread::sleep_for(std::chrono::milliseconds(50));
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
            if (!authed) {
                err = "Sin credenciales: clave/agent/password no disponibles";
                return false;
            }
        }
    }

    // Inicializar SFTP
    sftp_ = libssh2_sftp_init(session_);
    if (!sftp_) {
        err = "No se pudo inicializar SFTP";
        return false;
    }

    return true;
}

bool Libssh2SftpClient::connect(const SessionOptions& opt, std::string& err) {
    if (connected_) {
        err = "Ya conectado";
        return false;
    }
    if (!tcpConnect(opt.host, opt.port, err)) return false;
    if (!sshHandshakeAuth(opt, err)) return false;

    connected_ = true;
    return true;
}

void Libssh2SftpClient::disconnect() {
    if (sftp_) {
        libssh2_sftp_shutdown(sftp_);
        sftp_ = nullptr;
    }
    if (session_) {
        libssh2_session_disconnect(session_, "bye");
        libssh2_session_free(session_);
        session_ = nullptr;
    }
    if (sock_ != -1) {
        ::close(sock_);
        sock_ = -1;
    }
    connected_ = false;
}

bool Libssh2SftpClient::list(const std::string& remote_path,
                             std::vector<FileInfo>& out,
                             std::string& err) {
    if (!connected_ || !sftp_) {
        err = "No conectado";
        return false;
    }

    std::string path = remote_path.empty() ? "/" : remote_path;

    LIBSSH2_SFTP_HANDLE* dir = libssh2_sftp_opendir(sftp_, path.c_str());
    if (!dir) {
        err = "sftp_opendir falló para: " + path;
        return false;
    }

    out.clear();
    out.reserve(64);

    char filename[512];
    char longentry[1024];
    LIBSSH2_SFTP_ATTRIBUTES attrs;

    while (true) {
        memset(&attrs, 0, sizeof(attrs));
        int rc = libssh2_sftp_readdir_ex(dir,
                                         filename, sizeof(filename),
                                         longentry, sizeof(longentry),
                                         &attrs);
        if (rc > 0) {
            // rc = name length
            FileInfo fi{};
            fi.name = std::string(filename, rc);
            fi.is_dir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                            ? ((attrs.permissions & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFDIR)
                            : false;
            if (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) { fi.size = attrs.filesize; fi.has_size = true; }
            if (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) fi.mtime = attrs.mtime;
            if (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) fi.mode = attrs.permissions;
            if (attrs.flags & LIBSSH2_SFTP_ATTR_UIDGID) {
                fi.uid = attrs.uid;
                fi.gid = attrs.gid;
            }
            if (fi.name == "." || fi.name == "..") continue;
            out.push_back(std::move(fi));
        } else if (rc == 0) {
            // end of directory
            break;
        } else {
            err = "sftp_readdir_ex falló";
            libssh2_sftp_closedir(dir);
            return false;
        }
    }

    libssh2_sftp_closedir(dir);
    return true;
}

// Download a remote file to local. Reports progress and supports cooperative cancellation.
bool Libssh2SftpClient::get(const std::string& remote,
                            const std::string& local,
                            std::string& err,
                            std::function<void(std::size_t, std::size_t)> progress,
                            std::function<bool()> shouldCancel,
                            bool resume) {
    if (!connected_ || !sftp_) {
        err = "No conectado";
        return false;
    }

    // Remote size (for progress)
    LIBSSH2_SFTP_ATTRIBUTES st{};
    if (libssh2_sftp_stat_ex(sftp_, remote.c_str(), (unsigned)remote.size(),
                             LIBSSH2_SFTP_STAT, &st) != 0) {
        err = "No se pudo obtener stat remoto";
        return false;
    }
    std::size_t total = (st.flags & LIBSSH2_SFTP_ATTR_SIZE) ? (std::size_t)st.filesize : 0;

    // Open remote for reading
    LIBSSH2_SFTP_HANDLE* rh = libssh2_sftp_open_ex(
        sftp_, remote.c_str(), (unsigned)remote.size(),
        LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
    if (!rh) {
        err = "No se pudo abrir remoto para lectura";
        return false;
    }

    // Open local for writing, with optional resume
    FILE* lf = nullptr;
    std::size_t offset = 0;
    if (resume) {
        lf = ::fopen(local.c_str(), "ab");
        if (lf) {
            long cur = std::ftell(lf);
            if (cur > 0) offset = (std::size_t)cur;
            if (offset > 0 && offset < total) {
                // position remote at the existing offset
                libssh2_sftp_seek64(rh, (libssh2_uint64_t)offset);
            }
        }
    }
    if (!lf) lf = ::fopen(local.c_str(), "wb");
    if (!lf) {
        libssh2_sftp_close(rh);
        err = "No se pudo abrir archivo local para escribir";
        return false;
    }

    const std::size_t CHUNK = 64 * 1024;
    std::vector<char> buf(CHUNK);
    std::size_t done = offset;

    while (true) {
        if (shouldCancel && shouldCancel()) {
            err = "Cancelado por usuario";
            std::fclose(lf);
            libssh2_sftp_close(rh);
            return false;
        }
        ssize_t n = libssh2_sftp_read(rh, buf.data(), (size_t)buf.size());
        if (n > 0) {
            if (std::fwrite(buf.data(), 1, (size_t)n, lf) != (size_t)n) {
                err = "Escritura local falló";
                std::fclose(lf);
                libssh2_sftp_close(rh);
                return false;
            }
            done = done + (std::size_t)n;
            if (progress && total) progress(done, total);
        } else if (n == 0) {
            break; // EOF
        } else {
            err = "Lectura remota falló";
            std::fclose(lf);
            libssh2_sftp_close(rh);
            return false;
        }
    }

    std::fclose(lf);
    libssh2_sftp_close(rh);
    return true;
}

// Upload a local file to remote (create/truncate). Reports progress and supports cancellation.
bool Libssh2SftpClient::put(const std::string& local,
                            const std::string& remote,
                            std::string& err,
                            std::function<void(std::size_t, std::size_t)> progress,
                            std::function<bool()> shouldCancel,
                            bool resume) {
    if (!connected_ || !sftp_) {
        err = "No conectado";
        return false;
    }

    // Open local for reading
    FILE* lf = ::fopen(local.c_str(), "rb");
    if (!lf) {
        err = "No se pudo abrir archivo local para lectura";
        return false;
    }

    // Local size
    std::fseek(lf, 0, SEEK_END);
    long fsz = std::ftell(lf);
    std::fseek(lf, 0, SEEK_SET);
    std::size_t total = fsz > 0 ? (std::size_t)fsz : 0;

    // Open remote for writing (create, optionally resume without truncation)
    long startOffset = 0;
    if (resume) {
        // Query remote size if it exists
        LIBSSH2_SFTP_ATTRIBUTES stR{};
        if (libssh2_sftp_stat_ex(sftp_, remote.c_str(), (unsigned)remote.size(), LIBSSH2_SFTP_STAT, &stR) == 0) {
            if (stR.flags & LIBSSH2_SFTP_ATTR_SIZE) startOffset = (long)stR.filesize;
        }
    }
    unsigned long flags = LIBSSH2_FXF_WRITE | LIBSSH2_FXF_CREAT | (resume ? 0 : LIBSSH2_FXF_TRUNC);
    LIBSSH2_SFTP_HANDLE* wh = libssh2_sftp_open_ex(
        sftp_, remote.c_str(), (unsigned)remote.size(),
        flags,
        0644, LIBSSH2_SFTP_OPENFILE);
    if (!wh) {
        std::fclose(lf);
        err = "No se pudo abrir remoto para escritura";
        return false;
    }

    const std::size_t CHUNK = 64 * 1024;
    std::vector<char> buf(CHUNK);
    std::size_t done = 0;

    // If resuming, advance local and remote
    if (resume && startOffset > 0 && (std::size_t)startOffset < total) {
        libssh2_sftp_seek64(wh, (libssh2_uint64_t)startOffset);
        if (std::fseek(lf, startOffset, SEEK_SET) != 0) {
            err = "No se pudo posicionar archivo local";
            libssh2_sftp_close(wh);
            std::fclose(lf);
            return false;
        }
        done = (std::size_t)startOffset;
    }

    while (true) {
        size_t n = std::fread(buf.data(), 1, buf.size(), lf);
        if (n > 0) {
            char* p = buf.data();
            size_t remain = n;
            while (remain > 0) {
                if (shouldCancel && shouldCancel()) {
                    err = "Cancelado por usuario";
                    libssh2_sftp_close(wh);
                    std::fclose(lf);
                    return false;
                }
                ssize_t w = libssh2_sftp_write(wh, p, remain);
                if (w < 0) {
                    err = "Escritura remota falló";
                    libssh2_sftp_close(wh);
                    std::fclose(lf);
                    return false;
                }
                remain = remain - (size_t)w;
                p = p + w;
                done = done + (size_t)w;
                if (progress && total) progress(done, total);
            }
        } else {
            if (std::ferror(lf)) {
                err = "Lectura local falló";
                libssh2_sftp_close(wh);
                std::fclose(lf);
                return false;
            }
            break; // EOF
        }
    }

    libssh2_sftp_close(wh);
    std::fclose(lf);
    return true;
}

// Lightweight existence check using sftp_stat.
bool Libssh2SftpClient::exists(const std::string& remote_path,
                               bool& isDir,
                               std::string& err) {
    isDir = false;
    if (!connected_ || !sftp_) {
        err = "No conectado";
        return false;
    }

    LIBSSH2_SFTP_ATTRIBUTES st{};
    int rc = libssh2_sftp_stat_ex(
        sftp_, remote_path.c_str(), (unsigned)remote_path.size(),
        LIBSSH2_SFTP_STAT, &st);

    if (rc == 0) {
        if (st.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) {
            isDir = ((st.permissions & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFDIR);
        }
        return true; // exists
    }

    unsigned long sftp_err = libssh2_sftp_last_error(sftp_);
    if (sftp_err == LIBSSH2_FX_NO_SUCH_FILE || sftp_err == LIBSSH2_FX_FAILURE) {
        err.clear();
        return false; // does not exist
    }

    err = "stat remoto falló";
    return false;
}

// Detailed remote metadata (stat-like). Returns false if the path does not exist.
bool Libssh2SftpClient::stat(const std::string& remote_path,
                             FileInfo& info,
                             std::string& err) {
    if (!connected_ || !sftp_) {
        err = "No conectado";
        return false;
    }

    LIBSSH2_SFTP_ATTRIBUTES st{};
    int rc = libssh2_sftp_stat_ex(
        sftp_, remote_path.c_str(), (unsigned)remote_path.size(),
        LIBSSH2_SFTP_STAT, &st);
    if (rc != 0) {
        unsigned long sftp_err = libssh2_sftp_last_error(sftp_);
        if (sftp_err == LIBSSH2_FX_NO_SUCH_FILE || sftp_err == LIBSSH2_FX_FAILURE) {
            err.clear();
            return false; // no existe
        }
        err = "stat remoto falló";
        return false;
    }
    info.name.clear();
    info.is_dir = (st.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                      ? ((st.permissions & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFDIR)
                      : false;
    if (st.flags & LIBSSH2_SFTP_ATTR_SIZE) { info.size = (std::uint64_t)st.filesize; info.has_size = true; } else { info.size = 0; info.has_size = false; }
    info.mtime = (st.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) ? (std::uint64_t)st.mtime : 0;
    info.mode = (st.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) ? st.permissions : 0;
    if (st.flags & LIBSSH2_SFTP_ATTR_UIDGID) {
        info.uid = st.uid;
        info.gid = st.gid;
    }
    return true;
}

bool Libssh2SftpClient::chmod(const std::string& remote_path,
                              std::uint32_t mode,
                              std::string& err) {
    if (!connected_ || !sftp_) {
        err = "No conectado";
        return false;
    }
#ifdef LIBSSH2_SFTP
    // For compatibility, via SETSTAT
    LIBSSH2_SFTP_ATTRIBUTES a{};
    a.flags = LIBSSH2_SFTP_ATTR_PERMISSIONS;
    a.permissions = mode;
    int rc = libssh2_sftp_stat_ex(sftp_, remote_path.c_str(), (unsigned)remote_path.size(), LIBSSH2_SFTP_SETSTAT, &a);
    if (rc != 0) {
        err = "chmod remoto falló";
        return false;
    }
    return true;
#else
    (void)remote_path;
    (void)mode;
    err = "chmod no soportado";
    return false;
#endif
}

bool Libssh2SftpClient::setTimes(const std::string& remote_path,
                                 std::uint64_t atime,
                                 std::uint64_t mtime,
                                 std::string& err) {
    if (!connected_ || !sftp_) {
        err = "No conectado";
        return false;
    }
    LIBSSH2_SFTP_ATTRIBUTES a{};
    a.flags = LIBSSH2_SFTP_ATTR_ACMODTIME;
    a.atime = (unsigned long)atime;
    a.mtime = (unsigned long)mtime;
    int rc = libssh2_sftp_stat_ex(sftp_, remote_path.c_str(), (unsigned)remote_path.size(), LIBSSH2_SFTP_SETSTAT, &a);
    if (rc != 0) {
        err = "setTimes remoto falló";
        return false;
    }
    return true;
}

bool Libssh2SftpClient::chown(const std::string& remote_path,
                              std::uint32_t uid,
                              std::uint32_t gid,
                              std::string& err) {
    if (!connected_ || !sftp_) {
        err = "No conectado";
        return false;
    }
    LIBSSH2_SFTP_ATTRIBUTES a{};
    a.flags = 0;
    if (uid != (std::uint32_t)-1) {
        a.flags |= LIBSSH2_SFTP_ATTR_UIDGID;
        a.uid = uid;
    }
    if (gid != (std::uint32_t)-1) {
        a.flags |= LIBSSH2_SFTP_ATTR_UIDGID;
        a.gid = gid;
    }
    if (a.flags == 0) {
        err.clear();
        return true;
    }
    int rc = libssh2_sftp_stat_ex(sftp_, remote_path.c_str(), (unsigned)remote_path.size(), LIBSSH2_SFTP_SETSTAT, &a);
    if (rc != 0) {
        err = "chown remoto falló";
        return false;
    }
    return true;
}

bool Libssh2SftpClient::mkdir(const std::string& remote_dir,
                              std::string& err,
                              unsigned int mode) {
    if (!connected_ || !sftp_) {
        err = "No conectado";
        return false;
    }
    int rc = libssh2_sftp_mkdir(sftp_, remote_dir.c_str(), mode);
    if (rc != 0) {
        err = "sftp_mkdir falló";
        return false;
    }
    return true;
}

bool Libssh2SftpClient::removeFile(const std::string& remote_path,
                                   std::string& err) {
    if (!connected_ || !sftp_) {
        err = "No conectado";
        return false;
    }
    int rc = libssh2_sftp_unlink(sftp_, remote_path.c_str());
    if (rc != 0) {
        err = "sftp_unlink falló";
        return false;
    }
    return true;
}

bool Libssh2SftpClient::removeDir(const std::string& remote_dir,
                                  std::string& err) {
    if (!connected_ || !sftp_) {
        err = "No conectado";
        return false;
    }
    int rc = libssh2_sftp_rmdir(sftp_, remote_dir.c_str());
    if (rc != 0) {
        err = "sftp_rmdir falló (¿directorio no vacío?)";
        return false;
    }
    return true;
}

bool Libssh2SftpClient::rename(const std::string& from,
                               const std::string& to,
                               std::string& err,
                               bool overwrite) {
    if (!connected_ || !sftp_) {
        err = "No conectado";
        return false;
    }
    long flags = LIBSSH2_SFTP_RENAME_ATOMIC | LIBSSH2_SFTP_RENAME_NATIVE;
    if (overwrite) flags |= LIBSSH2_SFTP_RENAME_OVERWRITE;
    int rc = libssh2_sftp_rename_ex(
        sftp_,
        from.c_str(), (unsigned)from.size(),
        to.c_str(), (unsigned)to.size(),
        flags);
    if (rc != 0) {
        err = "sftp_rename_ex falló";
        return false;
    }
    return true;
}

bool RemoveKnownHostEntry(const std::string& khPath, const std::string& host, std::uint16_t port, std::string& err) {
    err.clear();
    LIBSSH2_SESSION* s = libssh2_session_init();
    if (!s) { err = "No se pudo inicializar libssh2"; return false; }
    LIBSSH2_KNOWNHOSTS* nh = libssh2_knownhost_init(s);
    if (!nh) { libssh2_session_free(s); err = "No se pudo inicializar known_hosts"; return false; }
    // load existing file if present
    (void)libssh2_knownhost_readfile(nh, khPath.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH);
    struct libssh2_knownhost* kh = nullptr;
    int typemaskPlain = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW;
    int typemaskHash  = LIBSSH2_KNOWNHOST_TYPE_SHA1  | LIBSSH2_KNOWNHOST_KEYENC_RAW;
    // Delete all matches (plain or hashed)
    while (true) {
        int rc = libssh2_knownhost_checkp(nh, host.c_str(), port, nullptr, 0, typemaskPlain, &kh);
        if (rc == LIBSSH2_KNOWNHOST_CHECK_MATCH && kh) {
            libssh2_knownhost_del(nh, kh);
            continue;
        }
        rc = libssh2_knownhost_checkp(nh, host.c_str(), port, nullptr, 0, typemaskHash, &kh);
        if (rc == LIBSSH2_KNOWNHOST_CHECK_MATCH && kh) {
            libssh2_knownhost_del(nh, kh);
            continue;
        }
        break;
    }
    std::string persistErr;
    if (!persist_known_hosts_atomic(nh, khPath, &persistErr)) {
        libssh2_knownhost_free(nh);
        libssh2_session_free(s);
        err = std::string("No se pudo escribir known_hosts: ") + persistErr;
        return false;
    }
    libssh2_knownhost_free(nh);
    libssh2_session_free(s);
    return true;
}

std::unique_ptr<SftpClient> Libssh2SftpClient::newConnectionLike(const SessionOptions& opt,
                                                                 std::string& err) {
    auto ptr = std::make_unique<Libssh2SftpClient>();
    if (!ptr->connect(opt, err)) return nullptr;
    return ptr;
}
} // namespace openscp
