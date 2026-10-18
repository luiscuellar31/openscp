// The ssh.exe process that carries an SSH connection through a jump host on
// Windows. OpenSSH for Windows reads and writes the pipes it is given, while
// libssh2 needs a socket, so two threads copy between those pipes and one end
// of a pair of loopback sockets; libssh2 gets the other end. POSIX gives ssh
// one end of a socketpair() directly.
#pragma once

#include "libssh2/detail/PlatformSocket.hpp"

#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace openscp::libssh2detail {

// A Windows command line for program and arguments, quoted as the Microsoft
// C runtime splits it, which OpenSSH for Windows uses. Characters that Cygwin
// and MSYS programs treat as quotes or wildcards are quoted too.
std::wstring windowsCommandLine(const std::wstring &program,
                                const std::vector<std::wstring> &arguments);

class SshJumpTunnel {
    public:
    SshJumpTunnel() = default;
    // Stops ssh.exe and the threads that relay its pipes.
    ~SshJumpTunnel();
    SshJumpTunnel(const SshJumpTunnel &) = delete;
    SshJumpTunnel &operator=(const SshJumpTunnel &) = delete;

    // Runs ssh.exe with the UTF-8 arguments that follow its program name. On
    // success, socket is the end of the relay that libssh2 reads and writes,
    // which the caller closes; closing it ends ssh.exe's input.
    bool start(const std::vector<std::string> &arguments, SocketHandle &socket,
               std::string &error);

    // Whether ssh.exe has exited, waiting up to timeoutMs for it.
    [[nodiscard]] bool exited(unsigned long timeoutMs = 0) const;
    // The last part of what ssh.exe wrote to stderr; all of it once it has
    // exited.
    [[nodiscard]] std::string errorOutput();

    private:
    void copyToProcess();
    void copyFromProcess();
    void collectErrors();
    void stop() noexcept;

    void *process_ = nullptr;
    void *input_ = nullptr;  // ssh.exe's stdin, written by copyToProcess()
    void *output_ = nullptr; // ssh.exe's stdout, read by copyFromProcess()
    void *errors_ = nullptr; // ssh.exe's stderr, read by collectErrors()
    SocketHandle relay_ = kInvalidSocket;
    std::thread toProcess_;
    std::thread fromProcess_;
    std::thread errorReader_;
    std::mutex errorTextMutex_;
    std::string errorText_;
};

} // namespace openscp::libssh2detail
