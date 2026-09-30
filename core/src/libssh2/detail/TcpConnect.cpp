#include "libssh2/detail/TcpConnect.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <poll.h>
#include <sys/socket.h>

namespace openscp::libssh2detail {
bool waitForTcpConnection(int socket,
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
        pollfd descriptor{socket, POLLOUT, 0};
        const int result = ::poll(&descriptor, 1, interval);
        if (result < 0) {
            if (errno == EINTR)
                continue;
            error = std::string("TCP connection wait failed: ") +
                    std::strerror(errno);
            return false;
        }
        if (result == 0)
            continue;
        if (stopToken.stop_requested())
            continue;
        int socketError = 0;
        socklen_t length = sizeof(socketError);
        if (::getsockopt(socket, SOL_SOCKET, SO_ERROR, &socketError, &length) !=
            0) {
            error = std::string("Could not inspect TCP connection: ") +
                    std::strerror(errno);
            return false;
        }
        if (socketError != 0 || !(descriptor.revents & POLLOUT)) {
            errno = socketError != 0 ? socketError : ECONNABORTED;
            error =
                std::string("TCP connection failed: ") + std::strerror(errno);
            return false;
        }
        error.clear();
        return true;
    }
}
} // namespace openscp::libssh2detail
