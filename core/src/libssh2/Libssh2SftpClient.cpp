// Backend libssh2: gestiona socket TCP, sesión SSH y canal SFTP.
// Incluye keepalive, validación de known_hosts y soporte de reanudación.
#include "openscp/Libssh2SftpClient.hpp"
#include <libssh2.h>
#include <libssh2_sftp.h>

#include <cstring>
#include <string>
#include <vector>
#include <chrono>
#include <thread>
#include <cstdlib>
#include <cstdio>
#include <sstream>

// POSIX sockets
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <unistd.h>
#include <netinet/in.h>
#include <netinet/tcp.h>

namespace openscp {

// Inicialización global de libssh2 (una vez por proceso)
static bool g_libssh2_inited = false;

// Contexto para kbd-interactive: podemos responder usuario y contraseña según el prompt
struct KbdIntCtx {
    const char* user;
    const char* pass;
    const KbdIntPromptsCB* cb; // opcional: callback UI para prompts
};

// Callback keyboard-interactive: responde a prompts con user/password según el texto
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

    // Si hay callback UI, ofrecerle la oportunidad de responder los prompts.
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
            return;
        }
        // si el callback no pudo responder, caemos al heurístico simple
    }
    for (int i = 0; i < num_prompts; ++i) {
        const char* prompt = (prompts && prompts[i].text) ? reinterpret_cast<const char*>(prompts[i].text) : "";
        bool wantUser = false;
        // Heurística simple: si el prompt menciona "user" o "name", enviar usuario; si no, contraseña
        for (const char* p = prompt; *p; ++p) {
            char c = *p;
            if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a');
            // buscar secuencias simples en minúsculas
            // "user" o "name" en el prompt
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
        // Nota: evitamos fijar SO_RCVTIMEO/SO_SNDTIMEO durante autenticación porque
        // puede interferir con userauth en algunos servidores/libc. Confiamos en
        // libssh2_session_set_timeout (si está disponible) para gestionar timeouts.
        // Activar keepalive de TCP
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
            // conectado
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

    // Handshake
    if (libssh2_session_handshake(session_, sock_) != 0) {
        err = "SSH handshake falló";
        return false;
    }

    // Asegurar modo bloqueante y timeout razonable para evitar EAGAIN en auth
    libssh2_session_set_blocking(session_, 1);
#ifdef LIBSSH2_SESSION_TIMEOUT
    libssh2_session_set_timeout(session_, 20000); // 20s
#endif

    // Keepalive SSH: pedir que libssh2 envíe mensajes cada 30s si el peer lo permite
    libssh2_keepalive_config(session_, 1, 30);

    // Verificación de host key según política de known_hosts
    if (opt.known_hosts_policy == openscp::KnownHostsPolicy::Off) {
        // Sin verificación: no consultamos known_hosts ni fallamos por mismatch
    } else {
        LIBSSH2_KNOWNHOSTS* nh = libssh2_knownhost_init(session_);
        if (!nh) {
            err = "No se pudo inicializar known_hosts";
            return false;
        }

        // Ruta efectiva
        std::string khPath;
        if (opt.known_hosts_path.has_value()) {
            khPath = *opt.known_hosts_path;
        } else {
            const char* home = std::getenv("HOME");
            if (home) khPath = std::string(home) + "/.ssh/known_hosts";
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
        switch (keytype) {
            case LIBSSH2_HOSTKEY_TYPE_RSA:
                alg = LIBSSH2_KNOWNHOST_KEY_SSHRSA;
                break;
            case LIBSSH2_HOSTKEY_TYPE_DSS:
                alg = LIBSSH2_KNOWNHOST_KEY_SSHDSS;
                break;
            case LIBSSH2_HOSTKEY_TYPE_ECDSA_256:
#ifdef LIBSSH2_KNOWNHOST_KEY_ECDSA_256
                alg = LIBSSH2_KNOWNHOST_KEY_ECDSA_256;
                break;
#else
                alg = 0;
                break;
#endif
            case LIBSSH2_HOSTKEY_TYPE_ECDSA_384:
#ifdef LIBSSH2_KNOWNHOST_KEY_ECDSA_384
                alg = LIBSSH2_KNOWNHOST_KEY_ECDSA_384;
                break;
#else
                alg = 0;
                break;
#endif
            case LIBSSH2_HOSTKEY_TYPE_ECDSA_521:
#ifdef LIBSSH2_KNOWNHOST_KEY_ECDSA_521
                alg = LIBSSH2_KNOWNHOST_KEY_ECDSA_521;
                break;
#else
                alg = 0;
                break;
#endif
            case LIBSSH2_HOSTKEY_TYPE_ED25519:
#ifdef LIBSSH2_KNOWNHOST_KEY_ED25519
                alg = LIBSSH2_KNOWNHOST_KEY_ED25519;
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

        struct libssh2_knownhost* host = nullptr;
        int check = libssh2_knownhost_checkp(nh, opt.host.c_str(), opt.port,
                                             hostkey, keylen, typemask_plain, &host);
        if (check != LIBSSH2_KNOWNHOST_CHECK_MATCH) {
            check = libssh2_knownhost_checkp(nh, opt.host.c_str(), opt.port,
                                             hostkey, keylen, typemask_hash, &host);
        }

        if (check == LIBSSH2_KNOWNHOST_CHECK_MATCH) {
            libssh2_knownhost_free(nh);
        } else if (opt.known_hosts_policy == openscp::KnownHostsPolicy::AcceptNew && check == LIBSSH2_KNOWNHOST_CHECK_NOTFOUND) {
            // TOFU: pedir confirmación al usuario si hay callback
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
            // Obtener huella SHA256 si está disponible
            std::string fpStr;
#ifdef LIBSSH2_HOSTKEY_HASH_SHA256
            const unsigned char* h = (const unsigned char*)libssh2_hostkey_hash(session_, LIBSSH2_HOSTKEY_HASH_SHA256);
            if (h) {
                std::ostringstream oss;
                oss << "SHA256:";
                for (int i = 0; i < 32; ++i) {
                    if (i) oss << ':';
                    char b[4];
                    std::snprintf(b, sizeof(b), "%02X", (unsigned)h[i]);
                    oss << b;
                }
                fpStr = oss.str();
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
            bool confirmed = false;
            if (opt.hostkey_confirm_cb) {
                confirmed = opt.hostkey_confirm_cb(opt.host, opt.port, algName, fpStr);
            }
            if (!confirmed) {
                libssh2_knownhost_free(nh);
                err = "Host desconocido: huella no confirmada por el usuario";
                return false;
            }
            if (khPath.empty()) {
                libssh2_knownhost_free(nh);
                err = "Ruta known_hosts no definida";
                return false;
            }
            int addMask = LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | alg;
            int addrc = libssh2_knownhost_addc(nh, opt.host.c_str(), nullptr,
                                               hostkey, (size_t)keylen,
                                               nullptr, 0, addMask, nullptr);
            if (addrc != 0 || libssh2_knownhost_writefile(nh, khPath.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH) != 0) {
                libssh2_knownhost_free(nh);
                err = "No se pudo agregar/escribir host en known_hosts";
                return false;
            }
            libssh2_knownhost_free(nh);
        } else {
            libssh2_knownhost_free(nh);
            // Solo Strict debe fallar por mismatch o notfound; AcceptNew ya manejó NOTFOUND arriba
            if (opt.known_hosts_policy == openscp::KnownHostsPolicy::Strict) {
                err = (check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH)
                          ? "Host key no coincide con known_hosts"
                          : "Host desconocido en known_hosts";
                return false;
            }
        }
    }

    // Autenticación: preferir el método explícito del usuario.
    // 1) Si se especificó clave privada: usarla primero.
    // 2) Si se especificó contraseña: probar password primero; si NO desconecta y falla, consultar métodos y probar kbdint.
    // 3) En ausencia de credenciales: intentar ssh-agent (si el servidor lo permite); de lo contrario, error.
    if (opt.private_key_path.has_value()) {
        const char* passphrase = opt.private_key_passphrase ? opt.private_key_passphrase->c_str() : nullptr;
        int rc = libssh2_userauth_publickey_fromfile(session_,
                                                     opt.username.c_str(),
                                                     NULL,  // public key path (NULL: se deriva de la privada)
                                                     opt.private_key_path->c_str(),
                                                     passphrase);
        if (rc != 0) {
            err = "Auth por clave falló";
            return false;
        }
    } else {
        // Mantener lista de métodos SOLO cuando sea necesaria.
        std::string authlist;
        auto hasMethod = [&](const char* m) { return !authlist.empty() && authlist.find(m) != std::string::npos; };

        // Si el usuario proporcionó password: intentarlo primero para evitar agotar intentos con 'none' o agent.
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

            // ---- Cambio clave: intentar password directamente (sin userauth_list previo).
            do_password();

            // Si el servidor cerró tras password, no seguimos: el resto fallará en cascada.
            if (rc_pw == LIBSSH2_ERROR_SOCKET_DISCONNECT ||
                rc_pw == LIBSSH2_ERROR_SOCKET_SEND ||
                rc_pw == LIBSSH2_ERROR_SOCKET_RECV) {
                err = "El servidor cerró la conexión tras el intento de password";
                return false;
            }

            // Si password no fue exitoso pero la sesión sigue viva, AHORA consultamos métodos y probamos kbd-int si procede.
            if (rc_pw != 0) {
                char* methods = libssh2_userauth_list(session_, opt.username.c_str(), (unsigned)opt.username.size());
                authlist = methods ? std::string(methods) : std::string();
                if (hasMethod("keyboard-interactive")) {
                    do_kbdint();
                }
            }

            if (rc_pw != 0 && rc_kbd != 0) {
                // Como último recurso, intentar ssh-agent si el servidor lo permite (limitar identidades).
                // Necesitamos authlist para saber si permite publickey.
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
                            const int kMaxAgentTries = 3; // limite conservador
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
                    // Guardar mensaje de error de libssh2 para diagnóstico
                    char* emsgPtr = nullptr;
                    int emlen = 0;
                    (void)libssh2_session_last_error(session_, &emsgPtr, &emlen, 0);
                    std::string lastErr = (emsgPtr && emlen > 0) ? std::string(emsgPtr, (size_t)emlen) : std::string();
                    long lastErrno = libssh2_session_last_errno(session_);
                    // Incluir códigos de retorno para diagnóstico
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
            // Sin password: consultar métodos y luego intentar ssh-agent si el servidor lo permite.
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
            // rc = nombre_len
            FileInfo fi{};
            fi.name = std::string(filename, rc);
            fi.is_dir = (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS)
                            ? ((attrs.permissions & LIBSSH2_SFTP_S_IFMT) == LIBSSH2_SFTP_S_IFDIR)
                            : false;
            if (attrs.flags & LIBSSH2_SFTP_ATTR_SIZE) fi.size = attrs.filesize;
            if (attrs.flags & LIBSSH2_SFTP_ATTR_ACMODTIME) fi.mtime = attrs.mtime;
            if (attrs.flags & LIBSSH2_SFTP_ATTR_PERMISSIONS) fi.mode = attrs.permissions;
            if (attrs.flags & LIBSSH2_SFTP_ATTR_UIDGID) {
                fi.uid = attrs.uid;
                fi.gid = attrs.gid;
            }
            if (fi.name == "." || fi.name == "..") continue;
            out.push_back(std::move(fi));
        } else if (rc == 0) {
            // fin de directorio
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

// Descarga archivo remoto a local. Reporta progreso e integra cancelación cooperativa.
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

    // Tamaño remoto (para progreso)
    LIBSSH2_SFTP_ATTRIBUTES st{};
    if (libssh2_sftp_stat_ex(sftp_, remote.c_str(), (unsigned)remote.size(),
                             LIBSSH2_SFTP_STAT, &st) != 0) {
        err = "No se pudo obtener stat remoto";
        return false;
    }
    std::size_t total = (st.flags & LIBSSH2_SFTP_ATTR_SIZE) ? (std::size_t)st.filesize : 0;

    // Abrir remoto para lectura
    LIBSSH2_SFTP_HANDLE* rh = libssh2_sftp_open_ex(
        sftp_, remote.c_str(), (unsigned)remote.size(),
        LIBSSH2_FXF_READ, 0, LIBSSH2_SFTP_OPENFILE);
    if (!rh) {
        err = "No se pudo abrir remoto para lectura";
        return false;
    }

    // Abrir local para escritura, con opción de reanudar
    FILE* lf = nullptr;
    std::size_t offset = 0;
    if (resume) {
        lf = ::fopen(local.c_str(), "ab");
        if (lf) {
            long cur = std::ftell(lf);
            if (cur > 0) offset = (std::size_t)cur;
            if (offset > 0 && offset < total) {
                // posicionar remoto al offset existente
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

// Sube archivo local a remoto (crear/truncar). Reporta progreso y cancelación.
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

    // Abrir local para lectura
    FILE* lf = ::fopen(local.c_str(), "rb");
    if (!lf) {
        err = "No se pudo abrir archivo local para lectura";
        return false;
    }

    // Tamaño local
    std::fseek(lf, 0, SEEK_END);
    long fsz = std::ftell(lf);
    std::fseek(lf, 0, SEEK_SET);
    std::size_t total = fsz > 0 ? (std::size_t)fsz : 0;

    // Abrir remoto para escritura (crea, y opcionalmente reanuda sin truncar)
    long startOffset = 0;
    if (resume) {
        // Consultar tamaño remoto si existe
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

    // Si reanudamos, avanzar local y remoto
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

// Comprobación ligera de existencia usando sftp_stat.
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
        return true; // existe
    }

    unsigned long sftp_err = libssh2_sftp_last_error(sftp_);
    if (sftp_err == LIBSSH2_FX_NO_SUCH_FILE || sftp_err == LIBSSH2_FX_FAILURE) {
        err.clear();
        return false; // no existe
    }

    err = "stat remoto falló";
    return false;
}

// Metadatos remotos detallados (tipo stat). Devuelve false si la ruta no existe.
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
    info.size = (st.flags & LIBSSH2_SFTP_ATTR_SIZE) ? (std::uint64_t)st.filesize : 0;
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
    // Por compatibilidad, mediante SETSTAT
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

std::unique_ptr<SftpClient> Libssh2SftpClient::newConnectionLike(const SessionOptions& opt,
                                                                 std::string& err) {
    auto ptr = std::make_unique<Libssh2SftpClient>();
    if (!ptr->connect(opt, err)) return nullptr;
    return ptr;
}

} // namespace openscp
