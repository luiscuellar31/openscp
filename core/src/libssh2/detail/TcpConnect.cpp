#include "libssh2/detail/TcpConnect.hpp"

#include <algorithm>
#include <cerrno>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <poll.h>
#include <sys/socket.h>
#endif

namespace openscp::libssh2detail {
namespace {

#ifdef _WIN32
constexpr int kConnectionAborted = WSAECONNABORTED;

// Before Windows 10 2004, WSAPoll does not report a connect that fails;
// select() reports it in the exception set.
int waitForConnectEvent(SocketHandle socket, int intervalMs, bool &writable) {
    fd_set writeSet;
    fd_set exceptSet;
    FD_ZERO(&writeSet);
    FD_ZERO(&exceptSet);
    FD_SET(socket, &writeSet);
    FD_SET(socket, &exceptSet);
    timeval timeout{intervalMs / 1000, (intervalMs % 1000) * 1000};
    const int result = ::select(0, nullptr, &writeSet, &exceptSet, &timeout);
    writable = result > 0 && FD_ISSET(socket, &writeSet);
    return result;
}

bool pendingSocketError(SocketHandle socket, int &socketError) {
    int length = sizeof(socketError);
    return ::getsockopt(socket, SOL_SOCKET, SO_ERROR,
                        reinterpret_cast<char *>(&socketError), &length) == 0;
}
#else
constexpr int kConnectionAborted = ECONNABORTED;

int waitForConnectEvent(SocketHandle socket, int intervalMs, bool &writable) {
    pollfd descriptor{socket, POLLOUT, 0};
    const int result = ::poll(&descriptor, 1, intervalMs);
    writable = result > 0 && (descriptor.revents & POLLOUT);
    return result;
}

bool pendingSocketError(SocketHandle socket, int &socketError) {
    socklen_t length = sizeof(socketError);
    return ::getsockopt(socket, SOL_SOCKET, SO_ERROR, &socketError, &length) ==
           0;
}
#endif

} // namespace

bool waitForTcpConnection(SocketHandle socket,
                          std::chrono::steady_clock::time_point deadline,
                          std::stop_token stopToken, std::string &error) {
    for (;;) {
        if (stopToken.stop_requested()) {
            errno = ECANCELED;
            error = "Connection canceled by user";
            return false;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(
                deadline - std::chrono::steady_clock::now());
        if (remaining.count() <= 0) {
            errno = ETIMEDOUT;
            error = "TCP connection timed out";
            return false;
        }
        const int interval = static_cast<int>(
            std::min(remaining.count(), decltype(remaining.count()){50}));
        bool writable = false;
        const int result = waitForConnectEvent(socket, interval, writable);
        if (result < 0) {
            const int waitError = lastSocketError();
            if (isInterruptedSocketError(waitError))
                continue;
            error =
                "TCP connection wait failed: " + socketErrorMessage(waitError);
            return false;
        }
        if (result == 0)
            continue;
        if (stopToken.stop_requested())
            continue;
        int socketError = 0;
        if (!pendingSocketError(socket, socketError)) {
            error = "Could not inspect TCP connection: " +
                    socketErrorMessage(lastSocketError());
            return false;
        }
        if (socketError != 0 || !writable) {
            const int failure =
                socketError != 0 ? socketError : kConnectionAborted;
#ifndef _WIN32
            errno = failure;
#endif
            error = "TCP connection failed: " + socketErrorMessage(failure);
            return false;
        }
        error.clear();
        return true;
    }
}
} // namespace openscp::libssh2detail
