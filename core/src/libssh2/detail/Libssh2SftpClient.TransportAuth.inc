bool Libssh2SftpClient::sshHandshakeAuth(const SessionOptions &opt,
                                         std::string &err,
                                         bool initializeSftpSubsystem) {
    session_ = libssh2_session_init();
    if (!session_) {
        err = "libssh2_session_init failed";
        return false;
    }

    // Prefer modern algorithms (host keys, ciphers, MACs) and avoid DSA where
    // possible
#ifdef LIBSSH2_METHOD_HOSTKEY
    // Host keys: ensure no ssh-dss, avoid ssh-rsa(SHA-1); prefer modern with
    // safe fallbacks
    (void)libssh2_session_method_pref(
        session_, LIBSSH2_METHOD_HOSTKEY,
        "ssh-ed25519,ecdsa-sha2-nistp256,rsa-sha2-512,rsa-sha2-256,ecdsa-sha2-"
        "nistp384,ecdsa-sha2-nistp521");
#endif
#ifdef LIBSSH2_METHOD_KEX
    (void)libssh2_session_method_pref(
        session_, LIBSSH2_METHOD_KEX,
        "curve25519-sha256,ecdh-sha2-nistp256,diffie-hellman-group14-sha256");
#endif
#ifdef LIBSSH2_METHOD_CRYPT_CS
    (void)libssh2_session_method_pref(
        session_, LIBSSH2_METHOD_CRYPT_CS,
        "chacha20-poly1305@openssh.com,aes256-gcm@openssh.com,aes128-gcm@"
        "openssh.com,aes256-ctr,aes128-ctr");
#endif
#ifdef LIBSSH2_METHOD_CRYPT_SC
    (void)libssh2_session_method_pref(
        session_, LIBSSH2_METHOD_CRYPT_SC,
        "chacha20-poly1305@openssh.com,aes256-gcm@openssh.com,aes128-gcm@"
        "openssh.com,aes256-ctr,aes128-ctr");
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
#ifdef LIBSSH2_SESSION_TIMEOUT
    libssh2_session_set_timeout(session_, 20000); // 20s
#endif
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

        // Effective path
        std::string khPath;
        if (opt.known_hosts_path.has_value()) {
            khPath = *opt.known_hosts_path;
        } else {
#ifndef _WIN32
            std::string home = resolve_posix_home();
            if (!home.empty())
                khPath = home + "/.ssh/known_hosts";
#endif
        }

        bool khLoaded = false;
        if (!khPath.empty()) {
            khLoaded =
                (libssh2_knownhost_readfile(
                     nh, khPath.c_str(), LIBSSH2_KNOWNHOST_FILE_OPENSSH) >= 0);
        }
        if (!khLoaded &&
            opt.known_hosts_policy == openscp::KnownHostsPolicy::Strict) {
            libssh2_knownhost_free(nh);
            err = "known_hosts unavailable or unreadable (strict policy)";
            return false;
        }

        size_t keylen = 0;
        int keytype = 0;
        const char *hostkey = libssh2_session_hostkey(session_, &keylen, &keytype);
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

        int typemask_plain =
            LIBSSH2_KNOWNHOST_TYPE_PLAIN | LIBSSH2_KNOWNHOST_KEYENC_RAW | alg;
        int typemask_hash =
            LIBSSH2_KNOWNHOST_TYPE_SHA1 | LIBSSH2_KNOWNHOST_KEYENC_RAW | alg;
        core_logf(CoreLogLevel::Debug,
                  "HostKey map: keytype=%d algMask=%d display=%s", keytype, alg,
                  algDisplay.c_str());

        struct libssh2_knownhost *host = nullptr;
        int check =
            libssh2_knownhost_checkp(nh, opt.host.c_str(), opt.port, hostkey,
                                     keylen, typemask_plain, &host);
        if (check != LIBSSH2_KNOWNHOST_CHECK_MATCH) {
            check =
                libssh2_knownhost_checkp(nh, opt.host.c_str(), opt.port,
                                         hostkey, keylen, typemask_hash, &host);
        }

        if (check == LIBSSH2_KNOWNHOST_CHECK_MATCH) {
            libssh2_knownhost_free(nh);
        } else if (opt.known_hosts_policy ==
                       openscp::KnownHostsPolicy::AcceptNew &&
                   check == LIBSSH2_KNOWNHOST_CHECK_MISMATCH) {
            // TOFU rejects changed keys: changed host identities must fail hard.
            libssh2_knownhost_free(nh);
            err = "Host key does not match known_hosts (TOFU rejects changed keys)";
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
            const unsigned char *h =
                reinterpret_cast<const unsigned char *>(libssh2_hostkey_hash(
                    session_, LIBSSH2_HOSTKEY_HASH_SHA256));
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
                        std::snprintf(b, sizeof(b), "%02X", (unsigned)h[i]);
                        oss << b;
                    }
                    fpStr = std::string("SHA256:") + oss.str();
                } else {
                    fpStr = std::string("SHA256:") + fpB64;
                }
            }
#else
            const unsigned char *h =
                reinterpret_cast<const unsigned char *>(libssh2_hostkey_hash(
                    session_, LIBSSH2_HOSTKEY_HASH_SHA1));
            if (h) {
                std::ostringstream oss;
                oss << "SHA1:";
                for (int i = 0; i < 20; ++i) {
                    if (i)
                        oss << ':';
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
            case LIBSSH2_HOSTKEY_TYPE_ED25519:
                keyBits = 256;
                break;
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
                keyBits = (int)keylen * 8;
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
                bool saved = false;
                // Prepare add/typemask
                bool preferHashed = opt.known_hosts_hash_names;
                if (const char *ev = std::getenv("OPENSCP_KNOWNHOSTS_PLAIN")) {
                    if (*ev == '1')
                        preferHashed = false;
                    else if (*ev == '0')
                        preferHashed = true;
                }
                int nameMask = preferHashed ? LIBSSH2_KNOWNHOST_TYPE_SHA1
                                            : LIBSSH2_KNOWNHOST_TYPE_PLAIN;
                int addMask = nameMask | LIBSSH2_KNOWNHOST_KEYENC_RAW | alg;
                // host string with port when non-default
                std::string hostForKnown = opt.host;
                if (opt.port != 22)
                    hostForKnown = std::string("[") + opt.host +
                                   "]:" + std::to_string(opt.port);

                // Add entry
                if (alg != 0 &&
                    libssh2_knownhost_addc(nh, hostForKnown.c_str(), nullptr,
                                           hostkey, (size_t)keylen, nullptr, 0,
                                           addMask, nullptr) == 0) {
                    std::string persistErr;
                    saved = persist_known_hosts_atomic(nh, khPath, &persistErr);
                    if (!saved && opt.hostkey_status_cb) {
                        opt.hostkey_status_cb(
                            std::string("Could not save known_hosts: ") +
                            persistErr);
                    }
                }
                // Manual ED25519 fallback when libssh2 lacks knownhosts alg
                // mask
#ifdef LIBSSH2_HOSTKEY_TYPE_ED25519
                if (!saved && keytype == LIBSSH2_HOSTKEY_TYPE_ED25519) {
                    const auto *hostkeyBytes =
                        reinterpret_cast<const unsigned char *>(hostkey);
                    if (core_sensitive_debug_enabled()) {
                        std::ostringstream preview;
                        const std::size_t n = std::min<std::size_t>(
                            (std::size_t)keylen, (std::size_t)8);
                        for (std::size_t i = 0; i < n; ++i) {
                            if (i)
                                preview << ' ';
                            char b[4];
                            std::snprintf(
                                b, sizeof(b), "%02X",
                                (unsigned)hostkeyBytes[i]);
                            preview << b;
                        }
                        core_logf(CoreLogLevel::Debug,
                                  "Manual known_hosts write for ssh-ed25519 "
                                  "fallback; keylen=%zu key_head=%s",
                                  (size_t)keylen, preview.str().c_str());
                    } else {
                        core_logf(CoreLogLevel::Debug,
                                  "Manual known_hosts write for ssh-ed25519 "
                                  "fallback; key material redacted (set "
                                  "OPENSCP_ENV=dev and "
                                  "OPENSCP_LOG_SENSITIVE=1 to include)");
                    }
                    // Fallback: write OpenSSH line atomically (hashed or plain)
#ifndef _WIN32
                    // Read existing
                    std::string existing;
                    int rfd = ::open(khPath.c_str(), O_RDONLY);
                    if (rfd >= 0) {
                        char buf[4096];
                        ssize_t n;
                        while ((n = ::read(rfd, buf, sizeof(buf))) > 0)
                            existing.append(buf, buf + n);
                        ::close(rfd);
                    }
                    // Build lines
                    std::vector<std::string> lines;
                    {
                        std::string tmp;
                        for (char c : existing) {
                            if (c == '\r')
                                continue;
                            if (c == '\n') {
                                lines.push_back(tmp);
                                tmp.clear();
                            } else
                                tmp.push_back(c);
                        }
                        if (!existing.empty() && existing.back() != '\n')
                            lines.push_back(tmp);
                    }
                    std::string b64 = b64encode(hostkeyBytes, (size_t)keylen);
                    std::string hostToken;
                    if (preferHashed) {
                        hostToken = openssh_hash_hostname(hostForKnown);
                        // note: hashed entries won't be deduplicated (new salt)
                        lines.push_back(hostToken + " ssh-ed25519 " + b64);
                    } else {
                        hostToken = hostForKnown;
                        std::string prefix = hostToken + " ssh-ed25519 ";
                        auto it = std::find_if(
                            lines.begin(), lines.end(),
                            [&prefix](const std::string &ln) {
                                return ln.rfind(prefix, 0) == 0;
                            });
                        if (it != lines.end()) {
                            *it = prefix + b64;
                        } else {
                            lines.push_back(prefix + b64);
                        }
                    }
                    std::string content;
                    content.reserve(lines.size() * 64);
                    for (const std::string &ln : lines) {
                        if (!ln.empty())
                            content.append(ln);
                        content.push_back('\n');
                    }
                    std::string persistErr;
                    saved = persist_text_atomic(khPath, content, &persistErr);
                    if (!saved && opt.hostkey_status_cb) {
                        opt.hostkey_status_cb(
                            std::string("Could not save known_hosts: ") +
                            persistErr);
                    }
#endif
                }
#endif

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
        int rc = libssh2_userauth_publickey_fromfile(
            session_, opt.username.c_str(),
            NULL, // public key path (NULL: derived from the private key)
            opt.private_key_path->c_str(), passphrase);
        if (rc != 0) {
            char *emsgPtr = nullptr;
            int emlen = 0;
            (void)libssh2_session_last_error(session_, &emsgPtr, &emlen, 0);
            const std::string lastErr =
                (emsgPtr && emlen > 0) ? std::string(emsgPtr, (size_t)emlen)
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
                    pwLastErr.assign(emsgPtr, (size_t)emlen);
                pwErrno = libssh2_session_last_errno(session_);
            };
            auto do_kbdint = [&]() {
                bool kbdCancelled = false;
                KbdIntCtx ctx{opt.username.c_str(), opt.password->c_str(),
                              &opt.keyboard_interactive_cb, &kbdCancelled};
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
                char *emsgPtr = nullptr;
                int emlen = 0;
                (void)libssh2_session_last_error(session_, &emsgPtr, &emlen, 0);
                if (emsgPtr && emlen > 0)
                    kbLastErr.assign(emsgPtr, (size_t)emlen);
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
                char *methods =
                    libssh2_userauth_list(session_, opt.username.c_str(),
                                          (unsigned)opt.username.size());
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
                    char *methods =
                        libssh2_userauth_list(session_, opt.username.c_str(),
                                              (unsigned)opt.username.size());
                    authlist = methods ? std::string(methods) : std::string();
                }

                bool authed = false;
                if (hasMethod("publickey")) {
                    LIBSSH2_AGENT *agent = libssh2_agent_init(session_);
                    if (agent && libssh2_agent_connect(agent) == 0) {
                        if (libssh2_agent_list_identities(agent) == 0) {
                            struct libssh2_agent_publickey *identity = nullptr;
                            struct libssh2_agent_publickey *prev = nullptr;
                            int tries = 0;
                            const int kMaxAgentTries = 3; // conservative limit
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

                if (!authed) {
                    // Save libssh2 error message for diagnostics
                    char *emsgPtr = nullptr;
                    int emlen = 0;
                    (void)libssh2_session_last_error(session_, &emsgPtr, &emlen,
                                                     0);
                    std::string lastErr =
                        (emsgPtr && emlen > 0)
                            ? std::string(emsgPtr, (size_t)emlen)
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
            char *methods =
                libssh2_userauth_list(session_, opt.username.c_str(),
                                      (unsigned)opt.username.size());
            authlist = methods ? std::string(methods) : std::string();
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

