#pragma once

#include "libssh2/detail/PlatformSocket.hpp"

#include <chrono>
#include <cstddef>
#include <functional>
#include <stop_token>
#include <string>
#include <vector>

namespace openscp::libssh2detail {
// An address that name resolution returned, with a copy of its sockaddr.
struct TcpEndpoint {
    int family = 0;
    int type = 0;
    int protocol = 0;
    std::vector<unsigned char> address;
};

// How a wait for nonblocking TCP connects ended.
struct TcpWaitOutcome {
    enum class Kind { Connected, Failed, TimedOut, Canceled, WaitFailed };
    Kind kind = Kind::TimedOut;
    std::size_t index = 0; // the socket that connected or failed
    std::string error;
};

// Waits until one of the nonblocking connects in `sockets` connects or fails,
// `until` passes, or `stopToken` asks to stop.
TcpWaitOutcome
waitForTcpConnections(const std::vector<SocketHandle> &sockets,
                      std::chrono::steady_clock::time_point until,
                      std::stop_token stopToken);

// Connects to one of `endpoints` as Happy Eyeballs (RFC 8305) does: an
// attempt that has not connected after 250 ms, or that fails, starts the next
// one while the earlier ones go on, families alternate after the first
// address, and the first attempt to connect wins. All attempts share
// `deadline`. `configure` prepares each socket before its connect. Returns
// the connected socket, in blocking mode, or kInvalidSocket and `error`.
SocketHandle connectToFirstEndpoint(
    const std::vector<TcpEndpoint> &endpoints,
    std::chrono::steady_clock::time_point deadline, std::stop_token stopToken,
    const std::function<void(SocketHandle)> &configure, std::string &error);
} // namespace openscp::libssh2detail
