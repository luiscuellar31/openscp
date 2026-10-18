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

using Clock = std::chrono::steady_clock;

// RFC 8305's Connection Attempt Delay.
constexpr auto kAttemptDelay = std::chrono::milliseconds(250);
// Attempts pending at once; select() on Windows takes at most 64 sockets.
constexpr std::size_t kMaxPendingAttempts = 16;

#ifdef _WIN32
constexpr int kConnectionAborted = WSAECONNABORTED;

// Before Windows 10 2004, WSAPoll does not report a connect that fails;
// select() reports it in the exception set.
int waitForConnectEvents(const std::vector<SocketHandle> &sockets,
                         int intervalMs, std::vector<char> &finished,
                         std::vector<char> &writable) {
    fd_set writeSet;
    fd_set exceptSet;
    FD_ZERO(&writeSet);
    FD_ZERO(&exceptSet);
    for (const SocketHandle socket : sockets) {
        FD_SET(socket, &writeSet);
        FD_SET(socket, &exceptSet);
    }
    timeval timeout{intervalMs / 1000, (intervalMs % 1000) * 1000};
    const int result = ::select(0, nullptr, &writeSet, &exceptSet, &timeout);
    for (std::size_t index = 0; index < sockets.size(); ++index) {
        writable[index] = result > 0 && FD_ISSET(sockets[index], &writeSet);
        finished[index] = writable[index] ||
                          (result > 0 && FD_ISSET(sockets[index], &exceptSet));
    }
    return result;
}

bool pendingSocketError(SocketHandle socket, int &socketError) {
    int length = sizeof(socketError);
    return ::getsockopt(socket, SOL_SOCKET, SO_ERROR,
                        reinterpret_cast<char *>(&socketError), &length) == 0;
}

int connectSocket(SocketHandle socket, const TcpEndpoint &endpoint) {
    return ::connect(
        socket, reinterpret_cast<const sockaddr *>(endpoint.address.data()),
        static_cast<int>(endpoint.address.size()));
}
#else
constexpr int kConnectionAborted = ECONNABORTED;

int waitForConnectEvents(const std::vector<SocketHandle> &sockets,
                         int intervalMs, std::vector<char> &finished,
                         std::vector<char> &writable) {
    std::vector<pollfd> descriptors;
    descriptors.reserve(sockets.size());
    for (const SocketHandle socket : sockets)
        descriptors.push_back(pollfd{socket, POLLOUT, 0});
    const int result =
        ::poll(descriptors.data(), static_cast<nfds_t>(descriptors.size()),
               intervalMs);
    for (std::size_t index = 0; index < sockets.size(); ++index) {
        const short events = descriptors[index].revents;
        writable[index] = result > 0 && (events & POLLOUT) != 0;
        finished[index] =
            result > 0 && (events & (POLLOUT | POLLERR | POLLHUP | POLLNVAL));
    }
    return result;
}

bool pendingSocketError(SocketHandle socket, int &socketError) {
    socklen_t length = sizeof(socketError);
    return ::getsockopt(socket, SOL_SOCKET, SO_ERROR, &socketError, &length) ==
           0;
}

int connectSocket(SocketHandle socket, const TcpEndpoint &endpoint) {
    return ::connect(
        socket, reinterpret_cast<const sockaddr *>(endpoint.address.data()),
        static_cast<socklen_t>(endpoint.address.size()));
}
#endif

// The endpoints in the order to try them. Families alternate after the first
// address, as RFC 8305 asks, so that a family whose addresses all drop
// packets delays the others by one attempt, not by one per address.
std::vector<std::size_t>
attemptOrder(const std::vector<TcpEndpoint> &endpoints) {
    if (endpoints.empty())
        return {};
    std::vector<std::size_t> firstFamily;
    std::vector<std::size_t> otherFamilies;
    for (std::size_t index = 0; index < endpoints.size(); ++index) {
        (endpoints[index].family == endpoints.front().family ? firstFamily
                                                             : otherFamilies)
            .push_back(index);
    }
    std::vector<std::size_t> order;
    order.reserve(endpoints.size());
    for (std::size_t rank = 0;
         rank < std::max(firstFamily.size(), otherFamilies.size()); ++rank) {
        if (rank < firstFamily.size())
            order.push_back(firstFamily[rank]);
        if (rank < otherFamilies.size())
            order.push_back(otherFamilies[rank]);
    }
    return order;
}

} // namespace

TcpWaitOutcome waitForTcpConnections(const std::vector<SocketHandle> &sockets,
                                     Clock::time_point until,
                                     std::stop_token stopToken) {
    TcpWaitOutcome outcome;
    std::vector<char> finished(sockets.size());
    std::vector<char> writable(sockets.size());
    for (;;) {
        if (stopToken.stop_requested()) {
            errno = ECANCELED;
            outcome.kind = TcpWaitOutcome::Kind::Canceled;
            outcome.error = "Connection canceled by user";
            return outcome;
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(until -
                                                                  Clock::now());
        if (remaining.count() <= 0) {
            errno = ETIMEDOUT;
            outcome.kind = TcpWaitOutcome::Kind::TimedOut;
            outcome.error = "TCP connection timed out";
            return outcome;
        }
        const int interval = static_cast<int>(
            std::min(remaining.count(), decltype(remaining.count()){50}));
        const int result =
            waitForConnectEvents(sockets, interval, finished, writable);
        if (result < 0) {
            const int waitError = lastSocketError();
            if (isInterruptedSocketError(waitError))
                continue;
            outcome.kind = TcpWaitOutcome::Kind::WaitFailed;
            outcome.error =
                "TCP connection wait failed: " + socketErrorMessage(waitError);
            return outcome;
        }
        if (result == 0)
            continue;
        if (stopToken.stop_requested())
            continue;
        const auto event = std::find(finished.begin(), finished.end(), 1);
        if (event == finished.end())
            continue;
        outcome.index = static_cast<std::size_t>(event - finished.begin());
        int socketError = 0;
        if (!pendingSocketError(sockets[outcome.index], socketError)) {
            outcome.kind = TcpWaitOutcome::Kind::Failed;
            outcome.error = "Could not inspect TCP connection: " +
                            socketErrorMessage(lastSocketError());
            return outcome;
        }
        if (socketError != 0 || !writable[outcome.index]) {
            const int failure =
                socketError != 0 ? socketError : kConnectionAborted;
#ifndef _WIN32
            errno = failure;
#endif
            outcome.kind = TcpWaitOutcome::Kind::Failed;
            outcome.error =
                "TCP connection failed: " + socketErrorMessage(failure);
            return outcome;
        }
        outcome.kind = TcpWaitOutcome::Kind::Connected;
        outcome.error.clear();
        return outcome;
    }
}

SocketHandle
connectToFirstEndpoint(const std::vector<TcpEndpoint> &endpoints,
                       Clock::time_point deadline, std::stop_token stopToken,
                       const std::function<void(SocketHandle)> &configure,
                       std::string &error) {
    const std::vector<std::size_t> order = attemptOrder(endpoints);
    std::size_t next = 0;
    std::vector<SocketHandle> pending;
    auto nextStart = Clock::now();
    SocketHandle connected = kInvalidSocket;
    std::string lastError;
    while (connected == kInvalidSocket) {
        const auto now = Clock::now();
        if (stopToken.stop_requested()) {
            lastError = "Connection canceled by user";
            break;
        }
        if (now >= deadline) {
            lastError = "TCP connection timed out";
            break;
        }
        // The next attempt starts when it is due, and at once when nothing
        // is pending or the last attempt failed.
        if (next < order.size() && pending.size() < kMaxPendingAttempts &&
            (pending.empty() || now >= nextStart)) {
            const TcpEndpoint &endpoint = endpoints[order[next++]];
            nextStart = now;
            const SocketHandle socket =
                openSocket(endpoint.family, endpoint.type, endpoint.protocol);
            if (socket == kInvalidSocket) {
                lastError = socketErrorMessage(lastSocketError());
                continue;
            }
            if (configure)
                configure(socket);
            if (!setSocketNonBlocking(socket, true)) {
                lastError = socketErrorMessage(lastSocketError());
                closeSocket(socket);
                continue;
            }
            const int result = connectSocket(socket, endpoint);
            const int connectError = result == 0 ? 0 : lastSocketError();
            if (result == 0) {
                connected = socket;
            } else if (isConnectInProgressError(connectError)) {
                pending.push_back(socket);
                nextStart = now + kAttemptDelay;
            } else {
                lastError = socketErrorMessage(connectError);
                closeSocket(socket);
            }
            continue;
        }
        if (pending.empty())
            break;

        auto until = deadline;
        if (next < order.size() && pending.size() < kMaxPendingAttempts)
            until = std::min(until, nextStart);
        const TcpWaitOutcome outcome =
            waitForTcpConnections(pending, until, stopToken);
        switch (outcome.kind) {
        case TcpWaitOutcome::Kind::Connected:
            connected = pending[outcome.index];
            pending.erase(pending.begin() +
                          static_cast<std::ptrdiff_t>(outcome.index));
            break;
        case TcpWaitOutcome::Kind::Failed:
            lastError = outcome.error;
            closeSocket(pending[outcome.index]);
            pending.erase(pending.begin() +
                          static_cast<std::ptrdiff_t>(outcome.index));
            nextStart = Clock::now();
            break;
        case TcpWaitOutcome::Kind::TimedOut:
            // Time for the next attempt, or the deadline, checked above.
            break;
        case TcpWaitOutcome::Kind::Canceled:
        case TcpWaitOutcome::Kind::WaitFailed:
            for (const SocketHandle socket : pending)
                closeSocket(socket);
            error = outcome.error;
            return kInvalidSocket;
        }
    }
    for (const SocketHandle socket : pending)
        closeSocket(socket);
    if (connected == kInvalidSocket) {
        error = lastError;
        return kInvalidSocket;
    }
    if (!setSocketNonBlocking(connected, false)) {
        error = socketErrorMessage(lastSocketError());
        closeSocket(connected);
        return kInvalidSocket;
    }
    error.clear();
    return connected;
}
} // namespace openscp::libssh2detail
