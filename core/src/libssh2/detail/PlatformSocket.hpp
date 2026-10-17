// Socket handles, and the socket calls whose names, types or error reporting
// differ between POSIX and Winsock.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace openscp::libssh2detail {

#ifdef _WIN32
// Winsock's SOCKET, without bringing winsock2.h into every user of this
// header. PlatformSocket.cpp checks that the two match.
using SocketHandle = std::uintptr_t;
inline constexpr SocketHandle kInvalidSocket = ~SocketHandle{0};
#else
using SocketHandle = int;
inline constexpr SocketHandle kInvalidSocket = -1;
#endif

// Starts Winsock for the process before its first socket call. POSIX has
// nothing to start.
bool initializeSockets(std::string &error);

// A socket that child processes, such as an SSH jump tunnel, do not inherit.
SocketHandle openSocket(int family, int type, int protocol);
void closeSocket(SocketHandle socket) noexcept;
// Shuts both directions down, which makes a call blocked on the socket in
// another thread return.
void shutdownSocket(SocketHandle socket) noexcept;
// Stops the I/O another thread is waiting for on the socket. Windows needs it
// again after shutdownSocket() for a select() that began after the shutdown,
// which the shutdown alone never wakes; elsewhere it does nothing.
void cancelSocketIo(SocketHandle socket) noexcept;
bool setSocketNonBlocking(SocketHandle socket, bool nonBlocking);

// Each sends or receives once, at most `length` bytes, and returns the count;
// receiving returns 0 when the peer has closed the connection. Both return -1
// and set lastSocketError() on failure. Where the system offers MSG_NOSIGNAL,
// sending on a connection that is shut down raises no SIGPIPE; Windows has no
// such signal.
std::ptrdiff_t sendOnce(SocketHandle socket, const void *data,
                        std::size_t length);
std::ptrdiff_t receiveOnce(SocketHandle socket, void *buffer,
                           std::size_t length);

// The calling thread's last socket error: errno, or WSAGetLastError().
int lastSocketError() noexcept;
std::string socketErrorMessage(int error);
bool isInterruptedSocketError(int error) noexcept;
// What a nonblocking connect() reports while the connection is under way.
bool isConnectInProgressError(int error) noexcept;

} // namespace openscp::libssh2detail
