#include "libssh2/detail/SshJumpTunnel_win.hpp"

// clang-format off
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
// clang-format on

#include "common/Win32Text.hpp"

#include <algorithm>
#include <array>
#include <cstddef>
#include <string_view>

namespace openscp::libssh2detail {

namespace {

constexpr std::size_t kRelayBufferBytes = 64 * 1024;
constexpr std::size_t kErrorTextLimit = 4096;
// ssh -W exits soon after its input ends; past this, it is stopped, about as
// soon as POSIX sends it SIGKILL.
constexpr DWORD kExitWaitMs = 500;

void closeHandle(void *&handle) noexcept {
    if (handle) {
        ::CloseHandle(handle);
        handle = nullptr;
    }
}

std::string windowsFailure(const char *operation, DWORD code) {
    return std::string(operation) + ": " + win32::errorMessage(code);
}

bool isFile(const std::wstring &path) {
    const DWORD attributes = ::GetFileAttributesW(path.c_str());
    return attributes != INVALID_FILE_ATTRIBUTES &&
           (attributes & FILE_ATTRIBUTE_DIRECTORY) == 0;
}

// "C:\tools" or "\\server\share\tools"; not "", ".", "bin", "\bin" or
// "C:bin", which depend on the current directory or drive.
bool isAbsoluteFolder(std::wstring_view folder) {
    const auto separator = [](wchar_t ch) { return ch == L'\\' || ch == L'/'; };
    if (folder.size() >= 3 && folder[1] == L':' && separator(folder[2]))
        return (folder[0] >= L'A' && folder[0] <= L'Z') ||
               (folder[0] >= L'a' && folder[0] <= L'z');
    return folder.size() >= 2 && separator(folder[0]) && separator(folder[1]);
}

std::wstring environmentPath() {
    const DWORD length = ::GetEnvironmentVariableW(L"PATH", nullptr, 0);
    if (length == 0)
        return {};
    std::wstring path(length, L'\0');
    path.resize(::GetEnvironmentVariableW(L"PATH", path.data(), length));
    return path;
}

// The OpenSSH client that Windows installs as an optional feature.
std::wstring systemOpenSsh() {
    wchar_t system[MAX_PATH];
    const UINT length = ::GetSystemDirectoryW(system, MAX_PATH);
    if (length == 0 || length >= MAX_PATH)
        return {};
    return std::wstring(system, length) + L"\\OpenSSH\\ssh.exe";
}

void appendArgument(std::wstring &commandLine, const std::wstring &argument) {
    const auto plain = [](wchar_t ch) {
        return (ch >= L'a' && ch <= L'z') || (ch >= L'A' && ch <= L'Z') ||
               (ch >= L'0' && ch <= L'9') ||
               std::wstring_view(L"-_./:=@,+%").find(ch) !=
                   std::wstring_view::npos;
    };
    if (!argument.empty() &&
        std::all_of(argument.begin(), argument.end(), plain)) {
        commandLine += argument;
        return;
    }
    commandLine += L'"';
    std::size_t backslashes = 0;
    for (const wchar_t ch : argument) {
        if (ch == L'\\') {
            ++backslashes;
            continue;
        }
        // Backslashes are literal unless a quote follows them.
        commandLine.append(ch == L'"' ? backslashes * 2 + 1 : backslashes,
                           L'\\');
        commandLine += ch;
        backslashes = 0;
    }
    commandLine.append(backslashes * 2, L'\\');
    commandLine += L'"';
}

void disableNagle(SocketHandle socket) {
    const BOOL enabled = TRUE;
    (void)::setsockopt(socket, IPPROTO_TCP, TCP_NODELAY,
                       reinterpret_cast<const char *>(&enabled),
                       sizeof(enabled));
}

// Two connected loopback sockets that no child process inherits. The
// listener takes a single connection and checks that it came from the other
// socket, not from a program that raced to connect first.
bool openRelayPair(SocketHandle &first, SocketHandle &second,
                   std::string &error) {
    first = kInvalidSocket;
    second = kInvalidSocket;
    const SocketHandle listener = openSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == kInvalidSocket) {
        error = "Could not open the SSH jump tunnel's local socket: " +
                socketErrorMessage(lastSocketError());
        return false;
    }
    const BOOL exclusive = TRUE;
    (void)::setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE,
                       reinterpret_cast<const char *>(&exclusive),
                       sizeof(exclusive));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int addressLength = sizeof(address);
    SocketHandle client = kInvalidSocket;
    SocketHandle accepted = kInvalidSocket;
    sockaddr_in local{};
    sockaddr_in peer{};
    int localLength = sizeof(local);
    int peerLength = sizeof(peer);
    bool ok = ::bind(listener, reinterpret_cast<const sockaddr *>(&address),
                     sizeof(address)) == 0 &&
              ::listen(listener, 1) == 0 &&
              ::getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                            &addressLength) == 0;
    if (ok) {
        client = openSocket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
        ok = client != kInvalidSocket &&
             ::connect(client, reinterpret_cast<const sockaddr *>(&address),
                       sizeof(address)) == 0 &&
             ::getsockname(client, reinterpret_cast<sockaddr *>(&local),
                           &localLength) == 0;
    }
    if (ok) {
        accepted = ::accept(listener, reinterpret_cast<sockaddr *>(&peer),
                            &peerLength);
        ok = accepted != kInvalidSocket;
    }
    const int failure = ok ? 0 : lastSocketError();
    closeSocket(listener);
    if (ok && (peer.sin_addr.s_addr != local.sin_addr.s_addr ||
               peer.sin_port != local.sin_port)) {
        error = "Another program connected to the SSH jump tunnel's local "
                "socket.";
        ok = false;
    }
    if (!ok) {
        if (client != kInvalidSocket)
            closeSocket(client);
        if (accepted != kInvalidSocket)
            closeSocket(accepted);
        if (error.empty()) {
            error = "Could not connect the SSH jump tunnel's local sockets: " +
                    socketErrorMessage(failure);
        }
        return false;
    }
    // accept() gives a socket that child processes would inherit.
    (void)::SetHandleInformation(reinterpret_cast<HANDLE>(accepted),
                                 HANDLE_FLAG_INHERIT, 0);
    disableNagle(client);
    disableNagle(accepted);
    first = client;
    second = accepted;
    return true;
}

// A pipe whose child end ssh.exe inherits; the other end stays private.
bool createPipe(HANDLE &childEnd, void *&ownEnd, bool childReads,
                std::string &error) {
    SECURITY_ATTRIBUTES inheritable{sizeof(inheritable), nullptr, TRUE};
    HANDLE read = nullptr;
    HANDLE write = nullptr;
    if (!::CreatePipe(&read, &write, &inheritable,
                      static_cast<DWORD>(kRelayBufferBytes))) {
        error = windowsFailure("CreatePipe", ::GetLastError());
        return false;
    }
    childEnd = childReads ? read : write;
    ownEnd = childReads ? write : read;
    (void)::SetHandleInformation(ownEnd, HANDLE_FLAG_INHERIT, 0);
    return true;
}

bool startProcess(const std::wstring &program,
                  const std::vector<std::wstring> &arguments,
                  std::array<HANDLE, 3> standardHandles,
                  PROCESS_INFORMATION &started, std::string &error) {
    SIZE_T size = 0;
    (void)::InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
    std::vector<unsigned char> storage(size);
    auto *attributes =
        reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(storage.data());
    if (!::InitializeProcThreadAttributeList(attributes, 1, 0, &size)) {
        error = windowsFailure("InitializeProcThreadAttributeList",
                               ::GetLastError());
        return false;
    }
    // ssh.exe inherits these three pipes, not every handle that OpenSCP
    // happens to have inheritable.
    bool ok =
        ::UpdateProcThreadAttribute(
            attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            standardHandles.data(), standardHandles.size() * sizeof(HANDLE),
            nullptr, nullptr) != FALSE;
    STARTUPINFOEXW startup{};
    startup.StartupInfo.cb = sizeof(startup);
    startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
    startup.StartupInfo.hStdInput = standardHandles[0];
    startup.StartupInfo.hStdOutput = standardHandles[1];
    startup.StartupInfo.hStdError = standardHandles[2];
    startup.lpAttributeList = attributes;
    std::wstring commandLine = windowsCommandLine(program, arguments);
    // The full path keeps CreateProcessW from searching for the program.
    ok = ok && ::CreateProcessW(
                   program.c_str(), commandLine.data(), nullptr, nullptr, TRUE,
                   CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT, nullptr,
                   nullptr, &startup.StartupInfo, &started) != FALSE;
    const DWORD failure = ok ? ERROR_SUCCESS : ::GetLastError();
    ::DeleteProcThreadAttributeList(attributes);
    if (!ok) {
        error = windowsFailure("Could not start ssh.exe", failure);
        return false;
    }
    return true;
}

bool writeAll(HANDLE pipe, const char *data, std::size_t length) {
    while (length > 0) {
        DWORD written = 0;
        const DWORD chunk = static_cast<DWORD>(
            std::min<std::size_t>(length, kRelayBufferBytes));
        if (!::WriteFile(pipe, data, chunk, &written, nullptr) || written == 0)
            return false;
        data += written;
        length -= written;
    }
    return true;
}

bool sendAll(SocketHandle socket, const char *data, std::size_t length) {
    while (length > 0) {
        const std::ptrdiff_t sent = sendOnce(socket, data, length);
        if (sent <= 0)
            return false;
        data += sent;
        length -= static_cast<std::size_t>(sent);
    }
    return true;
}

} // namespace

std::wstring findSshExecutable(const std::wstring &pathValue,
                               const std::wstring &fallback) {
    std::size_t start = 0;
    while (start <= pathValue.size()) {
        std::size_t end = pathValue.find(L';', start);
        if (end == std::wstring::npos)
            end = pathValue.size();
        std::wstring_view folder(pathValue.data() + start, end - start);
        // cmd.exe accepts quoted entries, as "C:\Program Files\Git\usr\bin".
        if (folder.size() >= 2 && folder.front() == L'"' &&
            folder.back() == L'"')
            folder = folder.substr(1, folder.size() - 2);
        if (isAbsoluteFolder(folder)) {
            std::wstring candidate(folder);
            if (candidate.back() != L'\\' && candidate.back() != L'/')
                candidate += L'\\';
            candidate += L"ssh.exe";
            if (isFile(candidate))
                return candidate;
        }
        start = end + 1;
    }
    return !fallback.empty() && isFile(fallback) ? fallback : std::wstring();
}

std::wstring windowsCommandLine(const std::wstring &program,
                                const std::vector<std::wstring> &arguments) {
    std::wstring commandLine;
    appendArgument(commandLine, program);
    for (const std::wstring &argument : arguments) {
        commandLine += L' ';
        appendArgument(commandLine, argument);
    }
    return commandLine;
}

SshJumpTunnel::~SshJumpTunnel() {
    stop();
}

bool SshJumpTunnel::start(const std::vector<std::string> &arguments,
                          SocketHandle &socket, std::string &error) {
    socket = kInvalidSocket;
    if (!initializeSockets(error))
        return false;
    std::vector<std::wstring> wideArguments;
    wideArguments.reserve(arguments.size());
    for (const std::string &argument : arguments) {
        std::wstring wide;
        if (!win32::toWidePath(argument, wide)) {
            error = "The SSH jump host settings are not valid UTF-8.";
            return false;
        }
        wideArguments.push_back(std::move(wide));
    }
    const std::wstring program =
        findSshExecutable(environmentPath(), systemOpenSsh());
    if (program.empty()) {
        error = "The OpenSSH client was not found. Add the \"OpenSSH Client\" "
                "optional feature of Windows, or put ssh.exe in PATH.";
        return false;
    }

    SocketHandle libssh2End = kInvalidSocket;
    if (!openRelayPair(libssh2End, relay_, error))
        return false;
    std::array<HANDLE, 3> childEnds{};
    PROCESS_INFORMATION started{};
    const bool piped = createPipe(childEnds[0], input_, true, error) &&
                       createPipe(childEnds[1], output_, false, error) &&
                       createPipe(childEnds[2], errors_, false, error);
    const bool running = piped && startProcess(program, wideArguments,
                                               childEnds, started, error);
    for (HANDLE childEnd : childEnds) {
        if (childEnd)
            ::CloseHandle(childEnd);
    }
    if (!running) {
        closeSocket(libssh2End);
        stop();
        return false;
    }
    ::CloseHandle(started.hThread);
    process_ = started.hProcess;

    toProcess_ = std::thread(&SshJumpTunnel::copyToProcess, this);
    fromProcess_ = std::thread(&SshJumpTunnel::copyFromProcess, this);
    errorReader_ = std::thread(&SshJumpTunnel::collectErrors, this);
    socket = libssh2End;
    return true;
}

bool SshJumpTunnel::exited(unsigned long timeoutMs) const {
    return process_ &&
           ::WaitForSingleObject(process_, timeoutMs) == WAIT_OBJECT_0;
}

std::string SshJumpTunnel::errorOutput() {
    // Once ssh.exe has exited, the rest of its stderr is moments away.
    if (exited() && errorReader_.joinable())
        (void)::WaitForSingleObject(errorReader_.native_handle(), 500);
    std::lock_guard<std::mutex> lock(errorTextMutex_);
    return errorText_;
}

void SshJumpTunnel::copyToProcess() {
    std::vector<char> buffer(kRelayBufferBytes);
    for (;;) {
        const std::ptrdiff_t received =
            receiveOnce(relay_, buffer.data(), buffer.size());
        if (received <= 0 || !writeAll(input_, buffer.data(),
                                       static_cast<std::size_t>(received)))
            break;
    }
    // ssh -W closes its connection, and then exits, at the end of its input.
    closeHandle(input_);
}

void SshJumpTunnel::copyFromProcess() {
    std::vector<char> buffer(kRelayBufferBytes);
    for (;;) {
        DWORD read = 0;
        if (!::ReadFile(output_, buffer.data(),
                        static_cast<DWORD>(buffer.size()), &read, nullptr))
            break;
        if (read > 0 && !sendAll(relay_, buffer.data(), read))
            break;
    }
    // libssh2 then reads the end of the connection.
    (void)::shutdown(relay_, SD_SEND);
}

void SshJumpTunnel::collectErrors() {
    std::array<char, 512> buffer{};
    for (;;) {
        DWORD read = 0;
        if (!::ReadFile(errors_, buffer.data(),
                        static_cast<DWORD>(buffer.size()), &read, nullptr))
            break;
        std::lock_guard<std::mutex> lock(errorTextMutex_);
        errorText_.append(buffer.data(), read);
        // The last lines tell why ssh.exe stopped.
        if (errorText_.size() > kErrorTextLimit)
            errorText_.erase(0, errorText_.size() - kErrorTextLimit);
    }
}

void SshJumpTunnel::stop() noexcept {
    // Shutting the relay down ends ssh.exe's input, which ends ssh -W.
    if (relay_ != kInvalidSocket)
        shutdownSocket(relay_);
    if (process_ &&
        ::WaitForSingleObject(process_, kExitWaitMs) == WAIT_TIMEOUT)
        ::TerminateProcess(process_, 1);
    // With ssh.exe gone its pipes end, but a program it started may still
    // hold one, so the threads' pending I/O is canceled until each returns.
    for (std::thread *thread : {&toProcess_, &fromProcess_, &errorReader_}) {
        if (!thread->joinable())
            continue;
        const HANDLE handle = thread->native_handle();
        while (::WaitForSingleObject(handle, 50) == WAIT_TIMEOUT)
            (void)::CancelSynchronousIo(handle);
        thread->join();
    }
    closeHandle(input_);
    closeHandle(output_);
    closeHandle(errors_);
    closeHandle(process_);
    if (relay_ != kInvalidSocket) {
        closeSocket(relay_);
        relay_ = kInvalidSocket;
    }
}

} // namespace openscp::libssh2detail
