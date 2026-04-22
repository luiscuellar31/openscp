bool Libssh2SftpClient::connectInternal(const SessionOptions &opt,
                                        std::string &err,
                                        bool initializeSftpSubsystem) {
    if (connected_) {
        err = "Already connected";
        return false;
    }

    transferIntegrityPolicy_ =
        integrity_policy_from_env(opt.transfer_integrity_policy);

    // Defensive: ensure no leftover state from any previous partial attempt.
    disconnect();

    if (!tcpConnect(opt, err))
        return false;
    if (!sshHandshakeAuth(opt, err, initializeSftpSubsystem)) {
        disconnect();
        return false;
    }

    connected_ = true;
    return true;
}

bool Libssh2SftpClient::connect(const SessionOptions &opt, std::string &err) {
    return connectInternal(opt, err, true);
}

bool Libssh2SftpClient::connectTransportOnly(const SessionOptions &opt,
                                             std::string &err) {
    return connectInternal(opt, err, false);
}

void Libssh2SftpClient::disconnect() {
    _LIBSSH2_SFTP *sftp = nullptr;
    _LIBSSH2_SESSION *session = nullptr;
    bool wasConnected = false;
    int sock = -1;
#ifndef _WIN32
    int jumpPid = -1;
    int jumpStderrFd = -1;
#endif
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        wasConnected = connected_;
        connected_ = false;
        sftp = sftp_;
        session = session_;
        sock = sock_;
        sftp_ = nullptr;
        session_ = nullptr;
        sock_ = -1;
#ifndef _WIN32
        jumpPid = jumpProxyPid_;
        jumpProxyPid_ = -1;
        jumpStderrFd = jumpProxyStderrFd_;
        jumpProxyStderrFd_ = -1;
#endif
    }

    // Force the transport down first so libssh2 teardown calls fail fast
    // instead of waiting indefinitely on a peer that no longer responds.
    if (sock != -1) {
#ifdef _WIN32
        (void)::shutdown(sock, SD_BOTH);
#else
        (void)::shutdown(sock, SHUT_RDWR);
#endif
        ::close(sock);
    }
#ifndef _WIN32
    if (jumpStderrFd != -1)
        ::close(jumpStderrFd);
    if (jumpPid > 0)
        stop_ssh_jump_tunnel(jumpPid);
#endif

    if (session) {
        libssh2_session_set_blocking(session, 0);
#ifdef LIBSSH2_SESSION_TIMEOUT
        libssh2_session_set_timeout(session, 2000);
#endif
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
        const pid_t wr = ::waitpid(static_cast<pid_t>(jumpPid), &status, WNOHANG);
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
    int sock = -1;
    {
        std::lock_guard<std::mutex> lk(stateMutex_);
        sock = sock_;
    }
    if (sock == -1)
        return;
#ifdef _WIN32
    (void)::shutdown(sock, SD_BOTH);
#else
    (void)::shutdown(sock, SHUT_RDWR);
#endif
}
