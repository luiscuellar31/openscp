static void configure_tcp_keepalive(int s) {
    // Enable TCP keepalive
    int opt = 1;
    (void)::setsockopt(s, SOL_SOCKET, SO_KEEPALIVE, &opt, sizeof(opt));
#ifdef __APPLE__
    int idle = 60;
    (void)::setsockopt(s, IPPROTO_TCP, TCP_KEEPALIVE, &idle, sizeof(idle));
#elif defined(__linux__)
    int idle = 60;
    int intvl = 10;
    int cnt = 3;
    (void)::setsockopt(s, IPPROTO_TCP, TCP_KEEPIDLE, &idle, sizeof(idle));
    (void)::setsockopt(s, IPPROTO_TCP, TCP_KEEPINTVL, &intvl, sizeof(intvl));
    (void)::setsockopt(s, IPPROTO_TCP, TCP_KEEPCNT, &cnt, sizeof(cnt));
#endif
}

static bool set_socket_timeout_ms(int sock, int timeoutMs) {
    if (sock < 0)
        return false;
    if (timeoutMs < 0)
        timeoutMs = 0;
#ifdef _WIN32
    DWORD tvMs = static_cast<DWORD>(timeoutMs);
    const int r1 = ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO,
                                reinterpret_cast<const char *>(&tvMs),
                                sizeof(tvMs));
    const int r2 = ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO,
                                reinterpret_cast<const char *>(&tvMs),
                                sizeof(tvMs));
    return r1 == 0 && r2 == 0;
#else
    struct timeval tv{};
    tv.tv_sec = timeoutMs / 1000;
    tv.tv_usec = (timeoutMs % 1000) * 1000;
    const int r1 = ::setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    const int r2 = ::setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return r1 == 0 && r2 == 0;
#endif
}

static bool socket_send_all(int sock, const unsigned char *data,
                            std::size_t len, const char *stage,
                            std::string &err) {
    // send(2) may write partially; loop until the full buffer is sent.
    std::size_t off = 0;
    while (off < len) {
        const ssize_t w =
            ::send(sock, reinterpret_cast<const char *>(data + off), len - off,
                   0);
        if (w < 0) {
            if (errno == EINTR)
                continue;
            err = std::string(stage) + " send failed: " + std::strerror(errno);
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

static bool socket_recv_exact(int sock, unsigned char *buf, std::size_t len,
                              const char *stage, std::string &err) {
    // recv(2) may return partial data; loop until we fill the requested bytes.
    std::size_t off = 0;
    while (off < len) {
        const ssize_t r =
            ::recv(sock, reinterpret_cast<char *>(buf + off), len - off, 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            err =
                std::string(stage) + " recv failed: " + std::strerror(errno);
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

static bool socket_recv_until(int sock, const std::string &delimiter,
                              std::size_t maxBytes, const char *stage,
                              std::string &out, std::string &err) {
    out.clear();
    if (delimiter.empty()) {
        err = std::string(stage) + " invalid delimiter";
        return false;
    }
    std::array<char, 512> chunk{};
    while (out.find(delimiter) == std::string::npos) {
        const ssize_t r = ::recv(sock, chunk.data(), chunk.size(), 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            err =
                std::string(stage) + " recv failed: " + std::strerror(errno);
            return false;
        }
        if (r == 0) {
            err = std::string(stage) + " recv failed: connection closed";
            return false;
        }
        out.append(chunk.data(), static_cast<std::size_t>(r));
        if (out.size() > maxBytes) {
            err = std::string(stage) + " too large";
            return false;
        }
    }
    return true;
}

static bool connect_tcp_endpoint(const std::string &host, uint16_t port,
                                 int &sockOut, std::string &err) {
    sockOut = -1;
    struct addrinfo hints{};
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char portStr[16];
    snprintf(portStr, sizeof(portStr), "%u", static_cast<unsigned>(port));

    struct addrinfo *res = nullptr;
    int gai = getaddrinfo(host.c_str(), portStr, &hints, &res);
    if (gai != 0) {
        err = std::string("getaddrinfo: ") + gai_strerror(gai);
        return false;
    }

    std::string lastConnectErr;
    // Try all resolved addresses (IPv4/IPv6) until one succeeds.
    for (auto rp = res; rp != nullptr; rp = rp->ai_next) {
        int s = ::socket(rp->ai_family, rp->ai_socktype, rp->ai_protocol);
        if (s == -1)
            continue;
        configure_tcp_keepalive(s);
        if (::connect(s, rp->ai_addr, rp->ai_addrlen) == 0) {
            sockOut = s;
            freeaddrinfo(res);
            return true;
        }
        lastConnectErr = std::strerror(errno);
        ::close(s);
    }
    freeaddrinfo(res);
    err = "Could not connect to host/port";
    if (!lastConnectErr.empty())
        err += ": " + lastConnectErr;
    err += ".";
    return false;
}

static const char *socks5_reply_text(unsigned char rep) {
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

static std::string format_host_port_authority(const std::string &host,
                                              std::uint16_t port) {
    if (host.find(':') != std::string::npos && host.find(']') == std::string::npos)
        return "[" + host + "]:" + std::to_string(port);
    return host + ":" + std::to_string(port);
}

#ifndef _WIN32
static std::string trim_ascii_whitespace(std::string value) {
    auto notSpace = [](unsigned char ch) { return !std::isspace(ch); };
    value.erase(value.begin(),
                std::find_if(value.begin(), value.end(), notSpace));
    value.erase(std::find_if(value.rbegin(), value.rend(), notSpace).base(),
                value.end());
    return value;
}

static std::string read_jump_tunnel_stderr(int fd) {
    if (fd < 0)
        return std::string();

    std::string out;
    out.reserve(256);
    char buf[256];
    while (out.size() < 4096) {
        const std::size_t remaining = 4096 - out.size();
        const ssize_t n = ::read(fd, buf,
                                 remaining < sizeof(buf) ? remaining
                                                         : sizeof(buf));
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

static std::string format_jump_tunnel_exit_error(const char *prefix,
                                                 const std::string &stderrText) {
    std::string out(prefix ? prefix : "SSH jump tunnel failed.");
    if (!stderrText.empty()) {
        if (!out.empty() && out.back() != '.' && out.back() != ':')
            out += ":";
        out += " " + stderrText;
    }
    return out;
}

static bool spawn_ssh_jump_tunnel(const SessionOptions &opt, int &sockOut,
                                  int &pidOut, int &stderrFdOut,
                                  std::string &err) {
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

    const pid_t pid = ::fork();
    if (pid < 0) {
        err = std::string("fork failed: ") + std::strerror(errno);
        ::close(pairfd[0]);
        ::close(pairfd[1]);
        ::close(stderrPipe[0]);
        ::close(stderrPipe[1]);
        return false;
    }

    if (pid == 0) {
        ::close(pairfd[0]);
        ::close(stderrPipe[0]);
        if (::dup2(pairfd[1], STDIN_FILENO) < 0 ||
            ::dup2(pairfd[1], STDOUT_FILENO) < 0) {
            _exit(127);
        }
        if (pairfd[1] != STDIN_FILENO && pairfd[1] != STDOUT_FILENO)
            ::close(pairfd[1]);
        if (::dup2(stderrPipe[1], STDERR_FILENO) < 0) {
            _exit(127);
        }
        if (stderrPipe[1] != STDERR_FILENO)
            ::close(stderrPipe[1]);

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
        args.emplace_back(*opt.jump_host);

        std::vector<char *> argv;
        argv.reserve(args.size() + 1);
        std::transform(args.begin(), args.end(), std::back_inserter(argv),
                       [](const std::string &a) {
                           return const_cast<char *>(a.c_str());
                       });
        argv.push_back(nullptr);
        ::execvp("ssh", argv.data());
        const std::string execErr =
            std::string("exec ssh failed: ") + std::strerror(errno);
        write_best_effort(STDERR_FILENO, execErr.c_str(), execErr.size());
        _exit(127);
    }

    ::close(pairfd[1]);
    ::close(stderrPipe[1]);
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

static void stop_ssh_jump_tunnel(int pid) {
    if (pid <= 0)
        return;

    int status = 0;
    for (int attempt = 0; attempt < 8; ++attempt) {
        const pid_t wr = ::waitpid(static_cast<pid_t>(pid), &status, WNOHANG);
        if (wr == static_cast<pid_t>(pid) ||
            (wr == -1 && errno == ECHILD)) {
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
        if (wr == static_cast<pid_t>(pid) ||
            (wr == -1 && errno == ECHILD)) {
            return;
        }
        if (wr == -1 && errno != EINTR)
            return;
    }
}
#endif

static bool establish_socks5_tunnel(int sock, const SessionOptions &opt,
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
        const std::string pass = haveProxyPass ? *opt.proxy_password : "";
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
        authReq.push_back(0x01);
        authReq.push_back(static_cast<unsigned char>(user.size()));
        authReq.insert(authReq.end(), user.begin(), user.end());
        authReq.push_back(static_cast<unsigned char>(pass.size()));
        authReq.insert(authReq.end(), pass.begin(), pass.end());
        if (!socket_send_all(sock, authReq.data(), authReq.size(),
                             "SOCKS5 auth", err)) {
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
    if (tail > 0 &&
        !socket_recv_exact(sock, sink.data(), sink.size(), "SOCKS5 connect",
                           err)) {
        return false;
    }
    return true;
}

static bool establish_http_connect_tunnel(int sock, const SessionOptions &opt,
                                          std::string &err) {
    if (opt.host.empty()) {
        err = "HTTP CONNECT target host is empty.";
        return false;
    }
    const std::string authority = format_host_port_authority(opt.host, opt.port);
    std::ostringstream req;
    req << "CONNECT " << authority << " HTTP/1.1\r\n";
    req << "Host: " << authority << "\r\n";
    req << "Proxy-Connection: Keep-Alive\r\n";
    req << "User-Agent: OpenSCP\r\n";
    if ((opt.proxy_username && !opt.proxy_username->empty()) ||
        (opt.proxy_password && !opt.proxy_password->empty())) {
        const std::string user =
            opt.proxy_username ? *opt.proxy_username : std::string();
        const std::string pass =
            opt.proxy_password ? *opt.proxy_password : std::string();
        const std::string creds = user + ":" + pass;
        req << "Proxy-Authorization: Basic "
            << b64encode(reinterpret_cast<const unsigned char *>(creds.data()),
                         creds.size())
            << "\r\n";
    }
    req << "\r\n";
    const std::string payload = req.str();
    if (!socket_send_all(
            sock, reinterpret_cast<const unsigned char *>(payload.data()),
            payload.size(), "HTTP CONNECT", err)) {
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
        }
        sock_ = jumpSock;
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

    int socketFd = -1;
    if (!connect_tcp_endpoint(endpointHost, endpointPort, socketFd, err))
        return false;

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
            ::close(socketFd);
            return false;
        }
    }

    sock_ = socketFd;
    return true;
}
