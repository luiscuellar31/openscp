#include "libssh2/detail/PlatformSocket.hpp"

#include <algorithm>

#ifdef _WIN32
// clang-format off
#include <winsock2.h>
#include <windows.h>
// clang-format on

#include "common/Win32Text.hpp"

#include <climits>
#include <type_traits>
#else
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/types.h>
#include <unistd.h>
#endif

namespace openscp::libssh2detail {

#ifdef _WIN32

static_assert(std::is_same_v<SocketHandle, SOCKET>);
static_assert(kInvalidSocket == INVALID_SOCKET);

bool initializeSockets(std::string &error) {
    // WSAStartup counts its calls, and the process keeps Winsock until it
    // exits, as libcurl's global initialization does.
    static const int startupError = [] {
        WSADATA data{};
        return ::WSAStartup(MAKEWORD(2, 2), &data);
    }();
    if (startupError != 0) {
        error = "Could not start Windows Sockets: " +
                socketErrorMessage(startupError);
        return false;
    }
    return true;
}

SocketHandle openSocket(int family, int type, int protocol) {
    return ::WSASocketW(family, type, protocol, nullptr, 0,
                        WSA_FLAG_OVERLAPPED | WSA_FLAG_NO_HANDLE_INHERIT);
}

void closeSocket(SocketHandle socket) noexcept {
    (void)::closesocket(socket);
}

void shutdownSocket(SocketHandle socket) noexcept {
    // A local shutdown does not wake a select() or recv() that is already
    // waiting on the socket, as POSIX's does; canceling that I/O does.
    // Afterwards every recv() and send() fails at once with WSAESHUTDOWN.
    (void)::shutdown(socket, SD_BOTH);
    cancelSocketIo(socket);
}

void cancelSocketIo(SocketHandle socket) noexcept {
    (void)::CancelIoEx(reinterpret_cast<HANDLE>(socket), nullptr);
}

bool setSocketNonBlocking(SocketHandle socket, bool nonBlocking) {
    u_long mode = nonBlocking ? 1 : 0;
    return ::ioctlsocket(socket, FIONBIO, &mode) == 0;
}

std::ptrdiff_t sendOnce(SocketHandle socket, const void *data,
                        std::size_t length) {
    const int chunk = static_cast<int>(std::min<std::size_t>(length, INT_MAX));
    return ::send(socket, static_cast<const char *>(data), chunk, 0);
}

std::ptrdiff_t receiveOnce(SocketHandle socket, void *buffer,
                           std::size_t length) {
    const int chunk = static_cast<int>(std::min<std::size_t>(length, INT_MAX));
    return ::recv(socket, static_cast<char *>(buffer), chunk, 0);
}

int lastSocketError() noexcept {
    return ::WSAGetLastError();
}

std::string socketErrorMessage(int error) {
    return win32::errorMessage(static_cast<unsigned long>(error));
}

bool isInterruptedSocketError(int) noexcept {
    // No signal interrupts a Winsock call. WSAEINTR means cancelSocketIo()
    // stopped it, so it must end the call rather than be retried.
    return false;
}

bool isConnectInProgressError(int error) noexcept {
    return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS;
}

#else

bool initializeSockets(std::string &error) {
    error.clear();
    return true;
}

SocketHandle openSocket(int family, int type, int protocol) {
    const int descriptor = ::socket(family, type, protocol);
    if (descriptor < 0)
        return kInvalidSocket;
    if (::fcntl(descriptor, F_SETFD, FD_CLOEXEC) < 0) {
        const int error = errno;
        ::close(descriptor);
        errno = error;
        return kInvalidSocket;
    }
    return descriptor;
}

void closeSocket(SocketHandle socket) noexcept {
    (void)::close(socket);
}

void shutdownSocket(SocketHandle socket) noexcept {
    (void)::shutdown(socket, SHUT_RDWR);
}

void cancelSocketIo(SocketHandle) noexcept {
}

bool setSocketNonBlocking(SocketHandle socket, bool nonBlocking) {
    const int flags = ::fcntl(socket, F_GETFL, 0);
    if (flags < 0)
        return false;
    const int updated = nonBlocking ? flags | O_NONBLOCK : flags & ~O_NONBLOCK;
    return updated == flags || ::fcntl(socket, F_SETFL, updated) == 0;
}

std::ptrdiff_t sendOnce(SocketHandle socket, const void *data,
                        std::size_t length) {
#ifdef MSG_NOSIGNAL
    constexpr int kSendFlags = MSG_NOSIGNAL;
#else
    constexpr int kSendFlags = 0;
#endif
    return ::send(socket, data, length, kSendFlags);
}

std::ptrdiff_t receiveOnce(SocketHandle socket, void *buffer,
                           std::size_t length) {
    return ::recv(socket, buffer, length, 0);
}

int lastSocketError() noexcept {
    return errno;
}

std::string socketErrorMessage(int error) {
    return std::strerror(error);
}

bool isInterruptedSocketError(int error) noexcept {
    return error == EINTR;
}

bool isConnectInProgressError(int error) noexcept {
    return error == EINPROGRESS || error == EINTR;
}

#endif

} // namespace openscp::libssh2detail
