// Core unit tests without external framework (run via CTest).
#include "IntegrationTestSupport.hpp"
#include "TestHarness.hpp"
#include "common/RemoteListingLimits.hpp"
#include "common/UniqueFile.hpp"
#include "libssh2/Libssh2ScpClient.hpp"
#include "libssh2/Libssh2SftpClient.hpp"
#include "libssh2/detail/HostKeyBlob.hpp"
#include "libssh2/detail/KnownHostsFile.hpp"
#include "libssh2/detail/Libssh2CipherPreference.hpp"
#include "libssh2/detail/Libssh2ErrorClassifier.hpp"
#include "libssh2/detail/Libssh2InputSafety.hpp"
#include "libssh2/detail/Libssh2TransferIntegrity.hpp"
#include "libssh2/detail/Libssh2WritePacing.hpp"
#include "libssh2/detail/TcpConnect.hpp"
#include "libssh2/detail/UniqueSftpHandle.hpp"
#include "mock/MockSftpClient.hpp"
#include "openscp/ClientFactory.hpp"
#include "openscp/SafeLocalFile.hpp"
#include "openscp/SecureString.hpp"
#if OPENSCP_HAS_CURL_FTP
#include "curl/CurlFtpClient.hpp"
#endif
#if OPENSCP_HAS_CURL_WEBDAV
#include "curl/CurlWebDavClient.hpp"
#endif
#if OPENSCP_HAS_CURL_FTP || OPENSCP_HAS_CURL_WEBDAV
#include "curl/CurlBackendCommon.hpp"
#endif
#ifdef _WIN32
#include "libssh2/detail/SshJumpTunnel_win.hpp"
#endif

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <vector>

#ifndef _WIN32
#include <arpa/inet.h>
#include <fcntl.h>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>
#else
// clang-format off
#include <winsock2.h>
#include <windows.h>
#include <winioctl.h>
#include <shellapi.h>
// clang-format on
#include <io.h>
#endif

namespace openscp {

struct Libssh2SftpClientTestAccess {
    static void attachSocket(Libssh2SftpClient &client, int socket) {
        std::lock_guard lock(client.stateMutex_);
        client.sock_ = socket;
        client.connected_.store(true);
    }
};

} // namespace openscp

namespace {

namespace fs = std::filesystem;

openscp::SessionOptions validOptions() {
    openscp::SessionOptions opt;
    opt.host = "example.test";
    opt.username = "alice";
    return opt;
}

static_assert(std::is_same_v<decltype(openscp::SessionOptions{}.password),
                             std::optional<openscp::SecureString>>,
              "SessionOptions passwords must use SecureString storage");

OPENSCP_TEST(test_connection_cancel_before_creation, t) {
    std::stop_source cancellation;
    cancellation.request_stop();
    auto options = validOptions();
    options.connection_stop_token = cancellation.get_token();
    std::string error;
    t.check(!openscp::CreateConnectedClient(options, error) &&
                error == "Connection canceled by user",
            "a stopped connection must not resolve or create a backend");
}

#ifndef _WIN32
OPENSCP_TEST(test_concurrent_disconnect_unblocks_io_and_preserves_reused_socket,
             t) {
    int pair[2] = {-1, -1};
    t.check(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
            "disconnect fixture should create a local transport");
    if (pair[0] < 0)
        return;
    openscp::Libssh2SftpClient client;
    openscp::Libssh2SftpClientTestAccess::attachSocket(client, pair[0]);
    auto ioLock = client.lockIo();
    std::promise<void> start;
    const auto go = start.get_future().share();
    auto first = std::async(std::launch::async, [&] {
        go.wait();
        client.disconnect();
    });
    auto second = std::async(std::launch::async, [&] {
        go.wait();
        client.disconnect();
    });
    start.set_value();
    pollfd peer{pair[1], POLLIN, 0};
    const bool interrupted = ::poll(&peer, 1, 1000) > 0;
    char byte = 0;
    t.check(interrupted && ::recv(pair[1], &byte, 1, MSG_DONTWAIT) == 0 &&
                !client.isConnected(),
            "disconnect must shut down before waiting for active I/O");
    t.check(first.wait_for(std::chrono::milliseconds(0)) ==
                    std::future_status::timeout &&
                second.wait_for(std::chrono::milliseconds(0)) ==
                    std::future_status::timeout &&
                ::fcntl(pair[0], F_GETFD) >= 0,
            "both disconnects must wait for I/O before closing the descriptor");
    // Interrupt must still be able to acquire the state mutex while teardown
    // waits for I/O; it shares the same descriptor lifetime protection.
    client.interrupt();
    ioLock.unlock();
    t.check(first.wait_for(std::chrono::seconds(1)) ==
                    std::future_status::ready &&
                second.wait_for(std::chrono::seconds(1)) ==
                    std::future_status::ready,
            "concurrent disconnects must complete once I/O is released");
    first.get();
    second.get();
    t.check(::fcntl(pair[0], F_GETFD) == -1 && errno == EBADF,
            "the detached transport must be closed");
    ::close(pair[1]);

    int reused[2] = {-1, -1};
    t.check(::socketpair(AF_UNIX, SOCK_STREAM, 0, reused) == 0,
            "reuse fixture should create another transport");
    if (reused[0] < 0)
        return;
    if (reused[1] == pair[0])
        std::swap(reused[0], reused[1]);
    if (reused[0] != pair[0]) {
        const bool duplicated = ::dup2(reused[0], pair[0]) == pair[0];
        t.check(duplicated,
                "reuse fixture must retain the original descriptor number");
        if (!duplicated) {
            ::close(reused[0]);
            ::close(reused[1]);
            return;
        }
        ::close(reused[0]);
        reused[0] = pair[0];
    }
    client.disconnect();
    client.interrupt();
    t.check(::fcntl(reused[0], F_GETFD) >= 0 &&
                ::recv(reused[0], &byte, 1, MSG_DONTWAIT) == -1 &&
                (errno == EAGAIN || errno == EWOULDBLOCK),
            "later disconnect and interrupt must leave a reused socket open "
            "and live");
    ::close(reused[0]);
    ::close(reused[1]);
}

OPENSCP_TEST(test_tcp_wait_deadline_and_cancellation, t) {
    int pair[2] = {-1, -1};
    t.check(::socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0,
            "TCP wait fixture should create socket descriptors");
    if (pair[0] < 0)
        return;
    const int flags = ::fcntl(pair[0], F_GETFL, 0);
    t.check(::fcntl(pair[0], F_SETFL, flags | O_NONBLOCK) == 0,
            "wait fixture should use a nonblocking descriptor");
    const std::array<char, 4096> bytes{};
    while (::write(pair[0], bytes.data(), bytes.size()) > 0) {
    }
    t.check(errno == EAGAIN || errno == EWOULDBLOCK,
            "filled send buffer should withhold write readiness");
    using Kind = openscp::libssh2detail::TcpWaitOutcome::Kind;
    const auto started = std::chrono::steady_clock::now();
    t.check(openscp::libssh2detail::waitForTcpConnections(
                {pair[0]}, started + std::chrono::milliseconds(40), {})
                        .kind == Kind::TimedOut &&
                errno == ETIMEDOUT,
            "a non-ready socket must respect the shared TCP deadline");
    std::stop_source cancellation;
    auto waiting = std::async(std::launch::async, [&] {
        return openscp::libssh2detail::waitForTcpConnections(
            {pair[0]},
            std::chrono::steady_clock::now() + std::chrono::seconds(2),
            cancellation.get_token());
    });
    t.check(waiting.wait_for(std::chrono::milliseconds(20)) ==
                std::future_status::timeout,
            "the connection fixture should remain pending before cancellation");
    cancellation.request_stop();
    t.check(waiting.wait_for(std::chrono::milliseconds(500)) ==
                    std::future_status::ready &&
                waiting.get().error == "Connection canceled by user",
            "pending TCP cancellation must finish without waiting for timeout");
    ::close(pair[0]);
    ::close(pair[1]);
}

OPENSCP_TEST(test_connection_cancel_while_peer_is_silent, t) {
    std::vector<openscp::Protocol> protocols{openscp::Protocol::Sftp,
                                             openscp::Protocol::Scp};
#if OPENSCP_HAS_CURL_FTP
    protocols.push_back(openscp::Protocol::Ftp);
#endif
#if OPENSCP_HAS_CURL_WEBDAV
    protocols.push_back(openscp::Protocol::WebDav);
#endif
    for (const auto protocol : protocols) {
        const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        const bool listening =
            listener >= 0 &&
            ::bind(listener, reinterpret_cast<sockaddr *>(&address),
                   sizeof(address)) == 0 &&
            ::listen(listener, 1) == 0;
        t.check(listening, "connection fixture should listen on loopback");
        if (!listening) {
            if (listener >= 0)
                ::close(listener);
            continue;
        }
        socklen_t length = sizeof(address);
        t.check(::getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                              &length) == 0,
                "connection fixture should obtain its allocated port");
        auto options = validOptions();
        options.host = "127.0.0.1";
        options.port = ntohs(address.sin_port);
        options.protocol = protocol;
        options.webdav_scheme = openscp::WebDavScheme::Http;
        options.known_hosts_policy = openscp::KnownHostsPolicy::Off;
        std::stop_source cancellation;
        options.connection_stop_token = cancellation.get_token();
        std::string error;
        auto connecting = std::async(std::launch::async, [&] {
            return openscp::CreateConnectedClient(options, error);
        });
        pollfd ready{listener, POLLIN, 0};
        const bool accepted = ::poll(&ready, 1, 2000) > 0;
        t.check(accepted, "the real backend should reach the silent TCP peer");
        const int peer = accepted ? ::accept(listener, nullptr, nullptr) : -1;
        cancellation.request_stop();
        const bool stopped =
            connecting.wait_for(std::chrono::milliseconds(1500)) ==
            std::future_status::ready;
        t.check(stopped,
                "canceling connection creation must interrupt silent peer I/O");
        if (peer >= 0) {
            ::shutdown(peer, SHUT_RDWR);
            ::close(peer);
        }
        ::close(listener);
        t.check(!connecting.get() && error == "Connection canceled by user",
                "canceled connection creation must not return a live client");
    }
}
#endif

#ifdef _WIN32
using SocketLength = int;
#else
using SocketLength = socklen_t;
#endif

// 192.0.2.1, of TEST-NET-1, reaches no host: its packets are dropped, as
// those of a broken IPv6 route are, unless the system has no route at all.
constexpr std::uint32_t kUnreachableAddress = 0xC0000201;

// The TCP endpoint of an IPv4 `address`, in host byte order, and `port`.
openscp::libssh2detail::TcpEndpoint ipv4Endpoint(std::uint32_t address,
                                                 std::uint16_t port) {
    sockaddr_in socketAddress{};
    socketAddress.sin_family = AF_INET;
    socketAddress.sin_addr.s_addr = htonl(address);
    socketAddress.sin_port = htons(port);
    const auto *bytes = reinterpret_cast<const unsigned char *>(&socketAddress);
    return {AF_INET, SOCK_STREAM, IPPROTO_TCP,
            std::vector<unsigned char>(bytes, bytes + sizeof(socketAddress))};
}

// A loopback port the system picks, which accepts connections when listening
// and refuses them otherwise.
struct LoopbackPort {
    openscp::libssh2detail::SocketHandle socket =
        openscp::libssh2detail::kInvalidSocket;
    std::uint16_t port = 0;

    explicit LoopbackPort(bool listening)
        : socket(openscp::libssh2detail::openSocket(AF_INET, SOCK_STREAM,
                                                    IPPROTO_TCP)) {
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
        SocketLength length = sizeof(address);
        if (socket != openscp::libssh2detail::kInvalidSocket &&
            ::bind(socket, reinterpret_cast<sockaddr *>(&address),
                   sizeof(address)) == 0 &&
            ::getsockname(socket, reinterpret_cast<sockaddr *>(&address),
                          &length) == 0 &&
            (!listening || ::listen(socket, 4) == 0)) {
            port = ntohs(address.sin_port);
        }
    }
    ~LoopbackPort() {
        if (socket != openscp::libssh2detail::kInvalidSocket)
            openscp::libssh2detail::closeSocket(socket);
    }
    LoopbackPort(const LoopbackPort &) = delete;
    LoopbackPort &operator=(const LoopbackPort &) = delete;
};

OPENSCP_TEST(test_tcp_connect_goes_on_past_an_address_that_refuses, t) {
    // Windows retries a refused connect for about 2 seconds, as happens with
    // "localhost" when a server listens on 127.0.0.1 but not on ::1.
    std::string error;
    t.check(openscp::libssh2detail::initializeSockets(error),
            "sockets should start: " + error);
    const LoopbackPort refusing(false);
    const LoopbackPort listening(true);
    t.check(refusing.port != 0 && listening.port != 0,
            "the loopback ports should open");
    const auto started = std::chrono::steady_clock::now();
    const auto socket = openscp::libssh2detail::connectToFirstEndpoint(
        {ipv4Endpoint(INADDR_LOOPBACK, refusing.port),
         ipv4Endpoint(INADDR_LOOPBACK, listening.port)},
        started + std::chrono::seconds(20), {}, {}, error);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    t.check(socket != openscp::libssh2detail::kInvalidSocket &&
                elapsed < std::chrono::seconds(1),
            "an address that refuses should not hold back the next one: " +
                error);
    if (socket != openscp::libssh2detail::kInvalidSocket)
        openscp::libssh2detail::closeSocket(socket);
}

OPENSCP_TEST(test_tcp_connect_goes_on_past_an_address_that_never_answers, t) {
    std::string error;
    t.check(openscp::libssh2detail::initializeSockets(error),
            "sockets should start: " + error);
    const LoopbackPort listening(true);
    t.check(listening.port != 0, "the loopback port should listen");
    const auto started = std::chrono::steady_clock::now();
    const auto socket = openscp::libssh2detail::connectToFirstEndpoint(
        {ipv4Endpoint(kUnreachableAddress, listening.port),
         ipv4Endpoint(INADDR_LOOPBACK, listening.port)},
        started + std::chrono::seconds(20), {}, {}, error);
    const auto elapsed = std::chrono::steady_clock::now() - started;
    t.check(socket != openscp::libssh2detail::kInvalidSocket &&
                elapsed < std::chrono::seconds(2),
            "an address that never answers should not hold back the next "
            "one: " +
                error);
    if (socket != openscp::libssh2detail::kInvalidSocket)
        openscp::libssh2detail::closeSocket(socket);
}

OPENSCP_TEST(test_tcp_connect_keeps_its_deadline_and_stops_when_asked, t) {
    std::string error;
    t.check(openscp::libssh2detail::initializeSockets(error),
            "sockets should start: " + error);
    const auto started = std::chrono::steady_clock::now();
    const auto timedOut = openscp::libssh2detail::connectToFirstEndpoint(
        {ipv4Endpoint(kUnreachableAddress, 9)},
        started + std::chrono::milliseconds(300), {}, {}, error);
    t.check(timedOut == openscp::libssh2detail::kInvalidSocket &&
                std::chrono::steady_clock::now() - started <
                    std::chrono::seconds(1),
            "connecting should end by its deadline: " + error);

    std::stop_source cancellation;
    std::string cancelError;
    auto connecting = std::async(std::launch::async, [&] {
        return openscp::libssh2detail::connectToFirstEndpoint(
            {ipv4Endpoint(kUnreachableAddress, 9)},
            std::chrono::steady_clock::now() + std::chrono::seconds(10),
            cancellation.get_token(), {}, cancelError);
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    cancellation.request_stop();
    const bool stopped = connecting.wait_for(std::chrono::seconds(1)) ==
                         std::future_status::ready;
    t.check(stopped &&
                connecting.get() == openscp::libssh2detail::kInvalidSocket,
            "connecting should stop soon after a cancellation");
}

fs::path makeTempFilePath(const std::string &tag) {
    const auto now =
        std::chrono::steady_clock::now().time_since_epoch().count();
    const fs::path dir =
        fs::temp_directory_path() /
        ("openscp-tests-" + std::to_string(static_cast<long long>(now)));
    std::error_code ec;
    fs::create_directories(dir, ec);
    return dir / (tag + ".txt");
}

// Makes `link` a link to the folder `target` that local operations must
// refuse: a symbolic link on POSIX, and on Windows a junction, which needs no
// privilege, unlike a symbolic link.
bool createDirectoryLink(const fs::path &target, const fs::path &link) {
#ifdef _WIN32
    const std::wstring command = L"mklink /J \"" + link.wstring() + L"\" \"" +
                                 target.wstring() + L"\" >NUL";
    return _wsystem(command.c_str()) == 0;
#else
    std::error_code ec;
    fs::create_directory_symlink(target, link, ec);
    return !ec;
#endif
}

bool readTextFile(const fs::path &path, std::string &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return false;
    out.assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
    return in.good() || in.eof();
}

OPENSCP_TEST(test_sftp_write_size_follows_the_upload_rate, t) {
    using openscp::libssh2detail::nextSftpWriteSize;
    constexpr std::size_t kKiB = 1024;
    constexpr std::size_t kBuffer = 2 * 1024 * kKiB;
    t.check(nextSftpWriteSize(64 * kKiB, 40 * kKiB, 1.0, kBuffer) == 80 * kKiB,
            "a write should carry two seconds of data at the measured rate");
    t.check(nextSftpWriteSize(32 * kKiB, 100 * 1024 * kKiB, 0.1, kBuffer) ==
                64 * kKiB,
            "a fast link should grow the write size at most twofold");
    t.check(nextSftpWriteSize(1536 * kKiB, 100 * 1024 * kKiB, 0.1, kBuffer) ==
                kBuffer,
            "the write size should stop at the transfer buffer");
    t.check(nextSftpWriteSize(32 * kKiB, kKiB, 10.0, kBuffer) == 32 * kKiB,
            "a very slow link should still send whole libssh2 packets");
    t.check(nextSftpWriteSize(32 * kKiB, 0, 0.0, 16 * kKiB) == 16 * kKiB,
            "a small file's buffer should bound the write size");
}

OPENSCP_TEST(test_session_defaults, t) {
    openscp::SessionOptions o;
    t.check(o.protocol == openscp::Protocol::Sftp,
            "default protocol should be SFTP");
    t.check(o.scp_transfer_mode == openscp::ScpTransferMode::Auto,
            "default SCP transfer mode should be Auto");
    t.check(o.port == openscp::defaultPortForProtocol(openscp::Protocol::Sftp),
            "default port should match the SFTP default");
    t.check(o.known_hosts_policy == openscp::KnownHostsPolicy::Strict,
            "default known_hosts_policy should be Strict");
    t.check(o.known_hosts_hash_names,
            "known_hosts_hash_names should default to true");
    t.check(!o.show_fp_hex, "show_fp_hex should default to false");
    t.check(o.transfer_integrity_policy ==
                openscp::TransferIntegrityPolicy::Optional,
            "transfer_integrity_policy should default to Optional");
    t.check(o.local_file_durability ==
                openscp::LocalFileDurability::FileAndDirectory,
            "local downloads should default to maximum durability");
    t.check(!o.password.has_value(), "password should be empty by default");
    t.check(!o.private_key_path.has_value(),
            "private_key_path should be empty by default");
    t.check(o.webdav_scheme == openscp::WebDavScheme::Https,
            "default WebDAV scheme should be HTTPS");
    t.check(o.webdav_base_path == "/",
            "default WebDAV base path should be root");
    t.check(o.webdav_verify_peer,
            "WebDAV TLS verification should default to enabled");
    t.check(o.ftps_mode == openscp::FtpsMode::Auto,
            "default FTPS mode should use automatic negotiation");
}

OPENSCP_TEST(test_security_policy_normalization, t) {
    const auto invalidKnownHosts = static_cast<openscp::KnownHostsPolicy>(999);
    const auto invalidIntegrity =
        static_cast<openscp::TransferIntegrityPolicy>(-7);
    const auto invalidDurability =
        static_cast<openscp::LocalFileDurability>(999);

    t.check(!openscp::isValidKnownHostsPolicy(invalidKnownHosts) &&
                openscp::normalizeKnownHostsPolicy(invalidKnownHosts) ==
                    openscp::KnownHostsPolicy::Strict,
            "invalid known-hosts policies must normalize to Strict");
    t.check(openscp::knownHostsPolicyFromStorageValue(999) ==
                openscp::KnownHostsPolicy::Strict,
            "corrupt persisted host verification must fail secure");
    t.check(!openscp::isValidTransferIntegrityPolicy(invalidIntegrity) &&
                openscp::normalizeTransferIntegrityPolicy(invalidIntegrity) ==
                    openscp::TransferIntegrityPolicy::Optional,
            "invalid integrity policies must normalize to the documented "
            "default");
    t.check(!openscp::isValidLocalFileDurability(invalidDurability) &&
                openscp::localFileDurabilityFromStorageValue(999) ==
                    openscp::LocalFileDurability::FileAndDirectory,
            "corrupt durability settings must preserve the strongest mode");

    openscp::SessionOptions options = validOptions();
    options.known_hosts_policy = invalidKnownHosts;
    openscp::Libssh2SftpClient client;
    std::string error;
    t.check(!client.connect(options, error) &&
                client.lastOperationError().kind ==
                    openscp::RemoteErrorKind::InvalidRequest,
            "the SSH trust boundary must reject invalid security policies");

    options.known_hosts_policy = openscp::KnownHostsPolicy::Strict;
    options.local_file_durability = invalidDurability;
    error.clear();
    t.check(!client.connect(options, error) &&
                client.lastOperationError().kind ==
                    openscp::RemoteErrorKind::InvalidRequest,
            "the SSH trust boundary must reject invalid durability policies");

    options.local_file_durability =
        openscp::LocalFileDurability::FileAndDirectory;
    options.port = 0;
    error.clear();
    t.check(!client.connect(options, error) &&
                client.lastOperationError().kind ==
                    openscp::RemoteErrorKind::InvalidRequest,
            "the SSH trust boundary must reject invalid endpoint ports");
}

OPENSCP_TEST(test_remote_listing_budget, t) {
    openscp::RemoteListingBudget budget(2, 5);
    t.check(budget.tryConsume(2) && budget.tryConsume(3),
            "listing budget should accept entries exactly at both limits");
    t.check(!budget.tryConsume(0),
            "listing budget should reject entries beyond the count limit");
    t.check(budget.entries() == 2 && budget.nameBytes() == 5,
            "rejected entries must not mutate listing accounting");

    openscp::RemoteListingBudget byteBudget(10, 4);
    t.check(!byteBudget.tryConsume(5) && byteBudget.entries() == 0,
            "listing budget should reject one oversized filename safely");
}

OPENSCP_TEST(test_secure_string_value_semantics, t) {
    openscp::SecureString original("secret");
    openscp::SecureString copy(original);
    original = std::string_view("changed");

    t.check(copy == "secret", "SecureString copies should own their buffers");
    t.check(original == "changed",
            "SecureString assignment should replace the prior value");

    openscp::SecureString moved(std::move(copy));
    t.check(moved == "secret", "SecureString moves should preserve the value");
    moved.clear();
    t.check(moved.empty(), "SecureString clear should release the value");
}

OPENSCP_TEST(test_keyboard_interactive_prompts, t) {
    using openscp::KbdIntPrompt;
    using openscp::KbdIntPromptKind;
    struct Case {
        KbdIntPrompt prompt;
        KbdIntPromptKind kind;
    };
    // Prompts as servers send them: OpenSSH with PAM, RSA SecurID, Duo,
    // pam_oath, Google Authenticator and FreeIPA.
    const std::vector<Case> cases = {
        {{"Password: ", false}, KbdIntPromptKind::Password},
        {{"Password for deploy_user@host: ", false},
         KbdIntPromptKind::Password},
        {{"Enter passphrase for key: ", false}, KbdIntPromptKind::Password},
        {{"Enter PASSCODE:", false}, KbdIntPromptKind::OneTimeCode},
        {{"Passcode or option (1-3): ", true}, KbdIntPromptKind::OneTimeCode},
        {{"One-time password (OATH) for `alice': ", false},
         KbdIntPromptKind::OneTimeCode},
        {{"Verification code: ", false}, KbdIntPromptKind::OneTimeCode},
        {{"Verification code for user alice: ", false},
         KbdIntPromptKind::OneTimeCode},
        {{"Hostname verification code:", false}, KbdIntPromptKind::OneTimeCode},
        {{"First Factor: ", false}, KbdIntPromptKind::Other},
        {{"Username: ", true}, KbdIntPromptKind::Username},
        {{"login: ", true}, KbdIntPromptKind::Username},
        {{"Username: ", false}, KbdIntPromptKind::Other},
        {{"Your account name: ", true}, KbdIntPromptKind::Other},
    };
    for (const Case &entry : cases) {
        t.check(openscp::classifyKbdIntPrompt(entry.prompt) == entry.kind,
                "unexpected kind for the prompt \"" + entry.prompt.text + "\"");
    }

    using openscp::libssh2detail::KbdIntSavedAnswer;
    using openscp::libssh2detail::savedKbdIntAnswer;
    t.check(savedKbdIntAnswer({"Password: ", false}, 1) ==
                    KbdIntSavedAnswer::Password &&
                savedKbdIntAnswer({"Password: ", false}, 2) ==
                    KbdIntSavedAnswer::None,
            "the saved password should answer only the first round");
    t.check(savedKbdIntAnswer({"Username: ", true}, 2) ==
                    KbdIntSavedAnswer::Username &&
                savedKbdIntAnswer({"Enter PASSCODE:", false}, 1) ==
                    KbdIntSavedAnswer::None &&
                savedKbdIntAnswer({"First Factor: ", false}, 1) ==
                    KbdIntSavedAnswer::None,
            "codes and unclear secrets should be left to the user");
}

OPENSCP_TEST(test_libssh2_input_safety, t) {
    using namespace openscp::libssh2detail;

    const unsigned char nonTerminatedPrompt[] = {'U', 's', 'e', 'r'};
    KeyboardInteractivePromptView prompt{};
    prompt.text = nonTerminatedPrompt;
    prompt.length = sizeof(nonTerminatedPrompt);

    std::vector<std::string> copied;
    std::string error;
    t.check(copyKeyboardInteractivePrompts(&prompt, 1, copied, error),
            "bounded keyboard-interactive prompts should be accepted");
    t.check(copied.size() == 1 && copied.front() == "User",
            "keyboard-interactive prompt copies must honor explicit lengths");

    prompt.length = kMaxKeyboardInteractivePromptBytes + 1;
    copied.clear();
    error.clear();
    t.check(!copyKeyboardInteractivePrompts(&prompt, 1, copied, error),
            "oversized keyboard-interactive prompts must be rejected");
    t.check(!copyKeyboardInteractivePrompts(
                &prompt, kMaxKeyboardInteractivePrompts + 1, copied, error),
            "excessive keyboard-interactive prompt counts must be rejected");

    error.clear();
    t.check(validateEndpointHost("files.example.test", "SSH host", error),
            "normal SSH hosts should be accepted");
    error.clear();
    t.check(validateEndpointHost("2001:db8::1", "SSH host", error),
            "unbracketed IPv6 SSH hosts should be accepted");
    error.clear();
    t.check(!validateEndpointHost("example.test\r\nX-Test: injected",
                                  "SSH host", error),
            "SSH hosts must reject HTTP CONNECT header injection");
    error.clear();
    t.check(!validateEndpointHost("example.test:2222", "SSH host", error),
            "SSH hosts must keep ports in the dedicated field");
    error.clear();
    t.check(!validateEndpointHost("-oProxyCommand=true", "Jump host", error),
            "a host must not look like an option of the ssh program");
}

OPENSCP_TEST(test_known_hosts_file_reads_every_line, t) {
    namespace detail = openscp::libssh2detail;
    const fs::path path = makeTempFilePath("known-hosts-read");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << "# comment\n\nalpha k AQID\nbroken\r\nbeta k AQID\r\n"
               "[gamma]:2222 k AQID";
    }
    std::vector<std::string> lines;
    t.check(detail::readKnownHosts(path.string(), lines) ==
                    detail::KnownHostsReadStatus::Read &&
                lines == std::vector<std::string>{"# comment", "",
                                                  "alpha k AQID", "broken",
                                                  "beta k AQID",
                                                  "[gamma]:2222 k AQID"},
            "every line should be read without its line ending");

    t.check(detail::readKnownHosts(path.string() + ".missing", lines) ==
                    detail::KnownHostsReadStatus::Missing &&
                lines.empty(),
            "a missing known_hosts file should be reported as missing");
#ifndef _WIN32
    if (::geteuid() != 0) {
        fs::permissions(path, fs::perms::none);
        t.check(detail::readKnownHosts(path.string(), lines) ==
                    detail::KnownHostsReadStatus::Unreadable,
                "a known_hosts file that cannot be read should be reported");
        fs::permissions(path, fs::perms::owner_read | fs::perms::owner_write);
    }
#endif
    std::error_code ec;
    fs::remove_all(path.parent_path(), ec);
}

OPENSCP_TEST(test_host_key_preference_starts_with_recorded_types, t) {
    namespace detail = openscp::libssh2detail;
    const std::string usual = "ssh-ed25519,ecdsa-sha2-nistp256,rsa-sha2-512,"
                              "rsa-sha2-256,ecdsa-sha2-nistp384,"
                              "ecdsa-sha2-nistp521";
    t.check(detail::hostKeyAlgorithmPreference({}) == usual,
            "an unknown host should get the usual order");
    t.check(detail::hostKeyAlgorithmPreference({"ssh-rsa"}) ==
                "rsa-sha2-512,rsa-sha2-256,ssh-ed25519,ecdsa-sha2-nistp256,"
                "ecdsa-sha2-nistp384,ecdsa-sha2-nistp521",
            "a host known by its RSA key should be asked for RSA first");
    t.check(detail::hostKeyAlgorithmPreference({"ecdsa-sha2-nistp384",
                                                "sk-ssh-ed25519@openssh.com",
                                                "ecdsa-sha2-nistp256"}) ==
                "ecdsa-sha2-nistp256,ecdsa-sha2-nistp384,ssh-ed25519,"
                "rsa-sha2-512,rsa-sha2-256,ecdsa-sha2-nistp521",
            "recorded types should keep the usual order among themselves");
}

OPENSCP_TEST(test_revoked_host_keys_are_found_whatever_hosts_they_name, t) {
    namespace detail = openscp::libssh2detail;
    const unsigned char raw[] = {1, 2, 3};   // AQID
    const unsigned char other[] = {1, 2, 4}; // AQIE
    t.check(detail::hostKeyRevoked({"  @revoked\t*.example k AQID comment"},
                                   raw, sizeof(raw)),
            "a @revoked line should revoke its key whatever hosts it names");
    t.check(!detail::hostKeyRevoked({"h.example k AQID", "# @revoked * k AQID",
                                     "@cert-authority * k AQID"},
                                    raw, sizeof(raw)),
            "only @revoked lines should revoke a key");
    t.check(
        !detail::hostKeyRevoked({"@revoked * k AQID"}, other, sizeof(other)),
        "a @revoked line should revoke only its own key");
    t.check(!detail::hostKeyRevoked({"@revokedx * k AQID", "@revoked * AQID"},
                                    raw, sizeof(raw)),
            "malformed @revoked lines should revoke nothing");
}

OPENSCP_TEST(test_known_hosts_lines_name_their_host, t) {
    namespace detail = openscp::libssh2detail;
    const unsigned char raw[] = {1, 2, 3};
    t.check(detail::knownHostsLine("[h.example]:2222", "ssh-ed25519", raw,
                                   sizeof(raw), false) ==
                "[h.example]:2222 ssh-ed25519 AQID",
            "a plain known_hosts line should name the host and the key");

    const std::string hashed = detail::knownHostsLine(
        "[h.example]:2222", "ssh-ed25519", raw, sizeof(raw), true);
    t.check(hashed.size() == 3 + 28 + 1 + 28 + 17 &&
                hashed.rfind("|1|", 0) == 0 && hashed[31] == '|' &&
                hashed.compare(60, std::string::npos, " ssh-ed25519 AQID") == 0,
            "a hashed known_hosts line should look like ssh-keygen -H's: " +
                hashed);
    t.check(hashed != detail::knownHostsLine("[h.example]:2222", "ssh-ed25519",
                                             raw, sizeof(raw), true),
            "each hashed line should use its own salt");

    // RemoveKnownHostEntry recognizes OpenSSH's hashed names, so it finds the
    // host behind the hash.
    const fs::path path = makeTempFilePath("known-hosts-hashed");
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << hashed << "\nkept k AQID\n";
    }
    std::string error;
    std::string content;
    t.checkWithError(openscp::RemoveKnownHostEntry(path.string(), "h.example",
                                                   2222, error) &&
                         readTextFile(path, content) &&
                         content == "kept k AQID\n",
                     "the hashed name should match its host: ", error);
    std::error_code ec;
    fs::remove_all(path.parent_path(), ec);
}

OPENSCP_TEST(test_local_entry_name_is_the_name_stored_on_disk, t) {
    const fs::path stored = makeTempFilePath("Mixed-Case");
    {
        std::ofstream out(stored, std::ios::binary | std::ios::trunc);
        out << "content";
    }
    const fs::path other = stored.parent_path() / "mixed-case.txt";
    const std::string name =
        openscp::localfiles::localEntryName(other.string());
#if defined(_WIN32) || defined(__APPLE__)
    // Their volumes ignore case, so the other spelling finds the file.
    t.check(name == "Mixed-Case.txt",
            "another spelling should give the stored name: " + name);
#else
    t.check(name.empty(), "a Linux volume should not find another spelling");
#endif
    t.check(openscp::localfiles::localEntryName(stored.string()) ==
                    "Mixed-Case.txt" &&
                openscp::localfiles::localEntryName(
                    (stored.parent_path() / "missing.txt").string())
                    .empty(),
            "a stored name should come back as it is, and nothing for a "
            "missing entry");
    std::error_code ec;
    fs::remove_all(stored.parent_path(), ec);
}

OPENSCP_TEST(test_known_hosts_path_reads_a_tilde_as_the_home_folder, t) {
    // The connection dialog suggests ~/.ssh/known_hosts, which OpenSSH reads
    // in the home folder; as it stands, the path names a folder called "~".
    const fs::path home = makeTempFilePath("home").parent_path();
    std::error_code ec;
    fs::create_directories(home / ".ssh", ec);
    {
        std::ofstream out(home / ".ssh" / "known_hosts",
                          std::ios::binary | std::ios::trunc);
        out << "home.example ssh-ed25519 AQID\nkept k AQID\n";
    }
    {
        openscp::testsupport::ScopedEnvironment environment;
#ifdef _WIN32
        environment.set("USERPROFILE", home.string());
#else
        environment.set("HOME", home.string());
#endif
        t.check(openscp::HasKnownHostEntry("~/.ssh/known_hosts", "home.example",
                                           22),
                "a path from ~ should find the entry in the home folder");
        std::string error;
        std::string content;
        t.checkWithError(
            openscp::RemoveKnownHostEntry("~/.ssh/known_hosts", "home.example",
                                          22, error) &&
                readTextFile(home / ".ssh" / "known_hosts", content) &&
                content == "kept k AQID\n",
            "a path from ~ should remove the entry from the home folder: ",
            error);
    }
    fs::remove_all(home, ec);
}

OPENSCP_TEST(test_known_hosts_append_keeps_the_rest_of_the_file, t) {
    namespace detail = openscp::libssh2detail;
    const fs::path folder =
        makeTempFilePath("known-hosts-append").parent_path() / "append-folder";
    std::error_code ec;
    fs::create_directories(folder, ec);
#ifndef _WIN32
    fs::permissions(folder,
                    fs::perms::owner_all | fs::perms::group_read |
                        fs::perms::group_exec | fs::perms::others_read |
                        fs::perms::others_exec,
                    ec);
#endif
    const fs::path path = folder / "known_hosts";
    const std::string original =
        "# keep me\n\nmulti.example,10.0.0.9 ssh-ed25519 AQID";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        out << original;
    }
#ifndef _WIN32
    fs::permissions(path,
                    fs::perms::owner_read | fs::perms::owner_write |
                        fs::perms::group_read | fs::perms::others_read,
                    ec);
#endif
    std::string error;
    t.checkWithError(detail::appendKnownHostsLine(path.string(),
                                                  "new.example k AQID", error),
                     "a line should be appended: ", error);
    std::string content;
    t.check(readTextFile(path, content) &&
                content == original + "\nnew.example k AQID\n",
            "the file should keep its lines and gain the new one: " + content);
#ifndef _WIN32
    struct ::stat fileStat {};
    struct ::stat folderStat {};
    t.check(::stat(path.c_str(), &fileStat) == 0 &&
                (fileStat.st_mode & 0777) == 0644 &&
                ::stat(folder.c_str(), &folderStat) == 0 &&
                (folderStat.st_mode & 0777) == 0755,
            "appending should keep the file's and the folder's permissions");
#endif

    const fs::path link = folder / "linked_known_hosts";
    fs::create_symlink(path, link, ec);
#ifdef _WIN32
    // Only administrators, or Developer Mode, may make symbolic links.
    const bool linked = !ec;
    if (!linked)
        t.skip("appending through a link needs symbolic links");
#else
    const bool linked = true;
#endif
    if (linked) {
        t.check(!ec &&
                    detail::appendKnownHostsLine(link.string(),
                                                 "via.link k AQID", error) &&
                    fs::is_symlink(link) && readTextFile(path, content) &&
                    content.size() >= 16 &&
                    content.compare(content.size() - 16, 16,
                                    "via.link k AQID\n") == 0,
                "a symbolic link should stay a link and its target gain the "
                "line");
    }

    const fs::path created = folder / "new-ssh" / "known_hosts";
    t.checkWithError(
        detail::appendKnownHostsLine(created.string(), "first k AQID", error) &&
            readTextFile(created, content) && content == "first k AQID\n",
        "a missing file and folder should be created: ", error);
#ifndef _WIN32
    t.check(::stat(created.c_str(), &fileStat) == 0 &&
                (fileStat.st_mode & 0777) == 0600 &&
                ::stat(created.parent_path().c_str(), &folderStat) == 0 &&
                (folderStat.st_mode & 0777) == 0700,
            "a missing file and folder should be created private");
#endif
    fs::remove_all(folder.parent_path(), ec);
}

OPENSCP_TEST(test_safe_local_partial_files, t) {
    const fs::path target = makeTempFilePath("safe-target");
    const fs::path partial = target.string() + ".part";
    std::error_code ec;
    fs::create_directories(target.parent_path(), ec);
    {
        std::ofstream output(target, std::ios::binary | std::ios::trunc);
        output << "preserve-me";
    }

    fs::create_symlink(target, partial, ec);
#ifdef _WIN32
    // Only administrators, or Developer Mode, may make symbolic links.
    const bool symlinked = !ec;
    if (!symlinked)
        t.skip("refusing a partial symbolic link needs symbolic links");
#else
    const bool symlinked = true;
    t.check(!ec, "symlink fixture should be created");
#endif
    std::string symlinkError;
    openscp::UniqueFile unsafe;
    std::string preservedContents;
    if (symlinked) {
        unsafe.reset(openscp::localfiles::openRegularFileForWrite(
            partial.string(), openscp::localfiles::WriteMode::Truncate,
            symlinkError));
        t.check(!unsafe,
                "local partial files must reject final-component symlinks");
        t.check(readTextFile(target, preservedContents) &&
                    preservedContents == "preserve-me",
                "rejecting a partial symlink must preserve its target");
        fs::remove(partial, ec);
    }

    // A hard link needs no privilege on Windows either.
    fs::create_hard_link(target, partial, ec);
    t.check(!ec, "hard-link fixture should be created");
    symlinkError.clear();
    unsafe.reset(openscp::localfiles::openRegularFileForWrite(
        partial.string(), openscp::localfiles::WriteMode::Truncate,
        symlinkError));
    t.check(!unsafe, "local partial files must reject additional hard links");
    preservedContents.clear();
    t.check(readTextFile(target, preservedContents) &&
                preservedContents == "preserve-me",
            "rejecting a partial hard link must preserve its target");
    fs::remove(partial, ec);

    std::string error;
    openscp::UniqueFile file(openscp::localfiles::openRegularFileForWrite(
        partial.string(), openscp::localfiles::WriteMode::Truncate, error));
    t.check(static_cast<bool>(file),
            std::string("regular local partial files should open: ") + error);
    if (file) {
        const char payload[] = "replacement";
        t.check(std::fwrite(payload, 1, sizeof(payload) - 1, file.get()) ==
                    sizeof(payload) - 1,
                "safe partial file should be writable");
        t.checkWithError(openscp::localfiles::flushAndSync(file.get(), error),
                         "safe partial file should sync: ", error);
        file.reset();
        t.checkWithError(
            openscp::localfiles::atomicReplace(partial.string(),
                                               target.string(), error),
            "safe partial file should publish atomically: ", error);
    }
    std::string targetContents;
    t.check(readTextFile(target, targetContents) &&
                targetContents == "replacement",
            "atomic replacement should publish complete partial contents");

    error.clear();
    file.reset(openscp::localfiles::openRegularFileForWrite(
        partial.string(), openscp::localfiles::WriteMode::Truncate, error));
    if (file) {
        const char bufferedPayload[] = "buffered";
        t.check(
            std::fwrite(bufferedPayload, 1, sizeof(bufferedPayload) - 1,
                        file.get()) == sizeof(bufferedPayload) - 1 &&
                openscp::localfiles::flushAndSync(
                    file.get(), error, openscp::LocalFileDurability::Buffered),
            "buffered durability should still flush userspace writes");
        file.reset();
        t.check(openscp::localfiles::atomicReplace(
                    partial.string(), target.string(), error,
                    openscp::LocalFileDurability::Buffered),
                "buffered durability should still publish atomically");
    }
    targetContents.clear();
    t.check(readTextFile(target, targetContents) &&
                targetContents == "buffered",
            "buffered durability should publish complete contents");
    fs::remove(target, ec);
    fs::remove_all(target.parent_path(), ec);
}

// Downloads used to stay at the partial file's 0600, and replacing a file took
// its mode away.
OPENSCP_TEST(test_downloads_get_the_umask_or_the_replaced_mode, t) {
#ifdef _WIN32
    t.skip("Windows files have no permission bits.");
#else
    const mode_t mask = ::umask(0);
    ::umask(mask);
    const auto expected = [mask](unsigned mode) {
        return mode & ~static_cast<unsigned>(mask) & 0777u;
    };
    const fs::path target = makeTempFilePath("download-mode");
    const fs::path partial = target.string() + ".part";
    const auto publish = [&](std::optional<std::uint32_t> remoteMode) {
        std::string error;
        openscp::UniqueFile file(openscp::localfiles::openRegularFileForWrite(
            partial.string(), openscp::localfiles::WriteMode::Truncate, error));
        if (!file)
            return false;
        file.reset();
        openscp::localfiles::applyDownloadPermissions(
            partial.string(), target.string(), remoteMode);
        return openscp::localfiles::atomicReplace(partial.string(),
                                                  target.string(), error);
    };
    const auto modeOf = [&target] {
        struct stat metadata {};
        return ::stat(target.c_str(), &metadata) == 0
                   ? static_cast<unsigned>(metadata.st_mode) & 0777u
                   : 07777u;
    };
    std::error_code ec;

    t.check(publish(std::nullopt) && modeOf() == expected(0666u),
            "a new download without a remote mode should get 0666 less the "
            "umask");
    fs::remove(target, ec);
    t.check(publish(0755u) && modeOf() == expected(0755u),
            "a new download should keep the remote mode less the umask");
    fs::remove(target, ec);
    t.check(publish(0444u) && modeOf() == expected(0644u),
            "a new download should stay writable by its owner");
    t.check(::chmod(target.c_str(), 0750) == 0 && publish(0644u) &&
                modeOf() == 0750u,
            "a download that replaces a file should keep that file's mode");
    fs::remove_all(target.parent_path(), ec);
#endif
}

OPENSCP_TEST(test_safe_local_partial_fifos, t) {
#ifdef _WIN32
    t.skip("Windows has no FIFOs among its files.");
#else
    const fs::path partial = makeTempFilePath("fifo").string() + ".part";
    t.check(::mkfifo(partial.c_str(), 0600) == 0,
            "partial FIFO fixture should be created");
    if (!fs::is_fifo(partial))
        return;

    // A regression must fail instead of hanging the test worker indefinitely.
    struct AlarmGuard {
        ~AlarmGuard() { ::alarm(0); }
    } alarmGuard;
    ::alarm(5);
    for (const auto mode : {openscp::localfiles::WriteMode::Truncate,
                            openscp::localfiles::WriteMode::Append}) {
        std::string error;
        openscp::UniqueFile file(openscp::localfiles::openRegularFileForWrite(
            partial.string(), mode, error));
        t.check(!file && !error.empty(),
                "a partial FIFO without a reader must fail without blocking");

        // With a reader, open succeeds; descriptor type validation must reject
        // the FIFO before it can be chmod'ed, truncated, or handed to stdio.
        const int reader =
            ::open(partial.c_str(), O_RDONLY | O_NONBLOCK | O_CLOEXEC);
        t.check(reader >= 0, "a nonblocking FIFO reader should open");
        if (reader < 0)
            continue;
        file.reset(openscp::localfiles::openRegularFileForWrite(
            partial.string(), mode, error));
        t.check(!file && errno == EINVAL && !error.empty(),
                "a partial FIFO with a reader must fail type validation");
        ::close(reader);
        t.check(fs::is_fifo(partial),
                "rejecting a partial FIFO must leave the entry intact");
    }
    std::error_code ec;
    fs::remove_all(partial.parent_path(), ec);
#endif
}

OPENSCP_TEST(test_safe_local_regular_write_modes, t) {
    const fs::path partial = makeTempFilePath("regular-modes");
    for (const auto mode : {openscp::localfiles::WriteMode::Truncate,
                            openscp::localfiles::WriteMode::Append}) {
        {
            std::ofstream output(partial, std::ios::binary | std::ios::trunc);
            output << "original";
        }
        std::string error;
        openscp::UniqueFile file(openscp::localfiles::openRegularFileForWrite(
            partial.string(), mode, error));
        t.check(static_cast<bool>(file),
                "regular partial files must open in both write modes");
        if (!file)
            continue;
#ifndef _WIN32
        const int descriptor = ::fileno(file.get());
        const int flags = ::fcntl(descriptor, F_GETFL);
        t.check(flags >= 0 && (flags & O_NONBLOCK) == 0,
                "validated regular file streams must use blocking mode");
        t.check(flags >= 0 &&
                    ((flags & O_APPEND) != 0) ==
                        (mode == openscp::localfiles::WriteMode::Append),
                "blocking-mode restoration must preserve append semantics");
        const int descriptorFlags = ::fcntl(descriptor, F_GETFD);
        t.check(descriptorFlags >= 0 && (descriptorFlags & FD_CLOEXEC) != 0,
                "validated regular files must remain close-on-exec");
#endif
        t.check(std::fwrite("new", 1, 3, file.get()) == 3 &&
                    openscp::localfiles::flushAndSync(file.get(), error),
                "regular partial files must remain writable and syncable");
        file.reset();
        std::string contents;
        const std::string expected =
            mode == openscp::localfiles::WriteMode::Append ? "originalnew"
                                                           : "new";
        t.check(readTextFile(partial, contents) && contents == expected,
                "truncate and resume must preserve their existing behavior");
    }
    std::error_code ec;
    fs::remove_all(partial.parent_path(), ec);
}

OPENSCP_TEST(test_safe_local_source_cleanup_preserves_data, t) {
    const fs::path source = makeTempFilePath("move-source");
    std::ofstream(source, std::ios::binary) << "uploaded-original";
    openscp::localfiles::LocalFileIdentity identity;
    std::string error;
    t.check(openscp::localfiles::localFileIdentity(source.string(), identity,
                                                   error),
            "move cleanup should capture the uploaded source identity");
#ifdef _WIN32
    // Windows holds the source open against changes while it compares and
    // deletes it, so an unchanged source goes.
    t.check(openscp::localfiles::removeLocalFileIfUnchanged(source.string(),
                                                            identity, error) &&
                error.empty() && !fs::exists(source),
            "an unchanged source should be removed");
    std::error_code ec;
    std::ofstream(source, std::ios::binary) << "replacement";
    t.check(!openscp::localfiles::removeLocalFileIfUnchanged(source.string(),
                                                             identity, error) &&
                errno == EAGAIN && !error.empty(),
            "a replacement source must not be accepted for cleanup");
    std::string contents;
    t.check(readTextFile(source, contents) && contents == "replacement",
            "a replacement entry must remain intact");
    fs::remove(source, ec);
#else
    for (int attempt = 0; attempt < 2; ++attempt) {
        t.check(
            !openscp::localfiles::removeLocalFileIfUnchanged(source.string(),
                                                             identity, error) &&
                errno == ENOTSUP && !error.empty(),
            "an unchanged source must require manual removal on every retry");
        std::string contents;
        t.check(readTextFile(source, contents) &&
                    contents == "uploaded-original",
                "cleanup must not unlink or modify even an unchanged source");
    }
    std::error_code ec;
    fs::rename(source, source.string() + ".original", ec);
    t.check(!ec, "replacement fixture should preserve the original inode");
    std::ofstream(source, std::ios::binary) << "replacement";
    t.check(!openscp::localfiles::removeLocalFileIfUnchanged(source.string(),
                                                             identity, error),
            "a replacement source must not be accepted for cleanup");
    std::string contents;
    t.check(readTextFile(source, contents) && contents == "replacement",
            "a replacement entry must remain intact");
    fs::remove(source, ec);
#endif
    t.check(openscp::localfiles::removeLocalFileIfUnchanged(source.string(),
                                                            identity, error) &&
                error.empty(),
            "cleanup should finish once the source has been removed manually");
    fs::remove_all(source.parent_path(), ec);
}

OPENSCP_TEST(test_safe_local_parent_symlinks, t) {
    const fs::path base = makeTempFilePath("parent-symlink").parent_path();
    const fs::path selected = base / "selected";
    const fs::path outside = base / "outside";
    std::error_code ec;
    fs::create_directories(selected, ec);
    fs::create_directories(outside, ec);
    t.check(createDirectoryLink(outside, selected / "linked"),
            "parent symlink fixture should be created");

    std::string error;
    openscp::UniqueFile escaped(openscp::localfiles::openRegularFileForWrite(
        (selected / "linked" / "escape.part").string(),
        openscp::localfiles::WriteMode::Truncate, error));
    t.check(!escaped && !fs::exists(outside / "escape.part"),
            "opening a partial file must reject a parent symlink");
    escaped.reset();

    error.clear();
    t.check(!openscp::localfiles::ensureLocalDirectories(
                (selected / "linked" / "created").string(), error) &&
                !fs::exists(outside / "created"),
            "creating local folders must reject a parent symlink");
    error.clear();
    t.check(openscp::localfiles::ensureLocalDirectories(
                (selected / "created" / "child").string(), error) &&
                fs::is_directory(selected / "created" / "child"),
            "creating local folders should still work without symlinks");

    {
        std::ofstream outsideFile(outside / "keep.txt");
        outsideFile << "preserve";
    }
    error.clear();
    t.check(!openscp::localfiles::removeLocalPath(
                (selected / "linked" / "keep.txt").string(), false, error) &&
                fs::exists(outside / "keep.txt"),
            "removing a local file must reject a parent symlink");
    error.clear();
    t.check(!openscp::localfiles::setLocalModificationTime(
                (selected / "linked" / "keep.txt").string(), 1, error),
            "changing a local timestamp must reject a parent symlink");

    fs::create_directories(selected / "real", ec);
    {
        std::ofstream partial(selected / "real" / "item.part");
        partial << "original";
        std::ofstream outsidePartial(outside / "item.part");
        outsidePartial << "outside";
    }
    fs::rename(selected / "real", selected / "moved", ec);
    t.check(!ec && createDirectoryLink(outside, selected / "real"),
            "swapped parent symlink fixture should be created");
    error.clear();
    t.check(!openscp::localfiles::atomicReplace(
                (selected / "real" / "item.part").string(),
                (selected / "real" / "item").string(), error) &&
                !fs::exists(outside / "item") &&
                fs::exists(outside / "item.part"),
            "publishing a partial file must reject a swapped parent symlink");
    fs::remove_all(base, ec);
}

OPENSCP_TEST(test_protocol_helpers, t) {
    struct ProtocolFromStorageCase {
        const char *input;
        openscp::Protocol expected;
    };
    constexpr std::array protocolFromStorageCases{
        ProtocolFromStorageCase{"sftp", openscp::Protocol::Sftp},
        ProtocolFromStorageCase{"SCP", openscp::Protocol::Scp},
        ProtocolFromStorageCase{"ftp", openscp::Protocol::Ftp},
        ProtocolFromStorageCase{"FTPS", openscp::Protocol::Ftps},
        ProtocolFromStorageCase{"webdav", openscp::Protocol::WebDav},
        ProtocolFromStorageCase{"unknown", openscp::Protocol::Sftp},
    };
    for (const auto &[input, expected] : protocolFromStorageCases) {
        t.check(openscp::protocolFromStorageName(input) == expected,
                std::string("protocolFromStorageName: ") + input);
    }

    struct ProtocolStorageNameCase {
        openscp::Protocol input;
        const char *expected;
        const char *name;
    };
    constexpr std::array protocolStorageNameCases{
        ProtocolStorageNameCase{openscp::Protocol::Sftp, "sftp", "SFTP"},
        ProtocolStorageNameCase{openscp::Protocol::Scp, "scp", "SCP"},
        ProtocolStorageNameCase{openscp::Protocol::Ftp, "ftp", "FTP"},
        ProtocolStorageNameCase{openscp::Protocol::Ftps, "ftps", "FTPS"},
        ProtocolStorageNameCase{openscp::Protocol::WebDav, "webdav", "WebDAV"},
        ProtocolStorageNameCase{static_cast<openscp::Protocol>(999), "sftp",
                                "invalid"},
    };
    for (const auto &[input, expected, name] : protocolStorageNameCases) {
        t.check(std::string(openscp::protocolStorageName(input)) == expected,
                std::string("protocolStorageName: ") + name);
    }

    struct ProtocolDisplayNameCase {
        openscp::Protocol input;
        const char *expected;
        const char *name;
    };
    constexpr std::array protocolDisplayNameCases{
        ProtocolDisplayNameCase{openscp::Protocol::Sftp, "SFTP", "SFTP"},
        ProtocolDisplayNameCase{openscp::Protocol::Scp, "SCP", "SCP"},
        ProtocolDisplayNameCase{openscp::Protocol::Ftp, "FTP", "FTP"},
        ProtocolDisplayNameCase{openscp::Protocol::Ftps, "FTPS", "FTPS"},
        ProtocolDisplayNameCase{openscp::Protocol::WebDav, "WebDAV", "WebDAV"},
        ProtocolDisplayNameCase{static_cast<openscp::Protocol>(999), "SFTP",
                                "invalid"},
    };
    for (const auto &[input, expected, name] : protocolDisplayNameCases) {
        t.check(std::string(openscp::protocolDisplayName(input)) == expected,
                std::string("protocolDisplayName: ") + name);
    }

    struct ProtocolPortCase {
        openscp::Protocol input;
        std::uint16_t expected;
        const char *name;
    };
    constexpr std::array protocolPortCases{
        ProtocolPortCase{openscp::Protocol::Sftp, 22, "SFTP"},
        ProtocolPortCase{openscp::Protocol::Scp, 22, "SCP"},
        ProtocolPortCase{openscp::Protocol::Ftp, 21, "FTP"},
        ProtocolPortCase{openscp::Protocol::Ftps, 990, "FTPS"},
        ProtocolPortCase{openscp::Protocol::WebDav, 443, "WebDAV"},
        ProtocolPortCase{static_cast<openscp::Protocol>(999), 22, "invalid"},
    };
    for (const auto &[input, expected, name] : protocolPortCases) {
        t.check(openscp::defaultPortForProtocol(input) == expected,
                std::string("defaultPortForProtocol: ") + name);
    }

    struct ScpTransferModeFromStorageCase {
        const char *input;
        openscp::ScpTransferMode expected;
    };
    constexpr std::array scpTransferModeFromStorageCases{
        ScpTransferModeFromStorageCase{"auto", openscp::ScpTransferMode::Auto},
        ScpTransferModeFromStorageCase{"SCP-ONLY",
                                       openscp::ScpTransferMode::ScpOnly},
        ScpTransferModeFromStorageCase{"unknown",
                                       openscp::ScpTransferMode::Auto},
    };
    for (const auto &[input, expected] : scpTransferModeFromStorageCases) {
        t.check(openscp::scpTransferModeFromStorageName(input) == expected,
                std::string("scpTransferModeFromStorageName: ") + input);
    }

    struct ScpTransferModeStorageNameCase {
        openscp::ScpTransferMode input;
        const char *expected;
        const char *name;
    };
    constexpr std::array scpTransferModeStorageNameCases{
        ScpTransferModeStorageNameCase{openscp::ScpTransferMode::Auto, "auto",
                                       "Auto"},
        ScpTransferModeStorageNameCase{openscp::ScpTransferMode::ScpOnly,
                                       "scp-only", "ScpOnly"},
        ScpTransferModeStorageNameCase{
            static_cast<openscp::ScpTransferMode>(999), "auto", "invalid"},
    };
    for (const auto &[input, expected, name] :
         scpTransferModeStorageNameCases) {
        t.check(std::string(openscp::scpTransferModeStorageName(input)) ==
                    expected,
                std::string("scpTransferModeStorageName: ") + name);
    }

    struct FtpsModeFromStorageCase {
        const char *input;
        openscp::FtpsMode expected;
    };
    constexpr std::array ftpsModeFromStorageCases{
        FtpsModeFromStorageCase{"auto", openscp::FtpsMode::Auto},
        FtpsModeFromStorageCase{"EXPLICIT", openscp::FtpsMode::ExplicitTls},
        FtpsModeFromStorageCase{"implicit-tls", openscp::FtpsMode::ImplicitTls},
        FtpsModeFromStorageCase{"unknown", openscp::FtpsMode::Auto},
    };
    for (const auto &[input, expected] : ftpsModeFromStorageCases) {
        t.check(openscp::ftpsModeFromStorageName(input) == expected,
                std::string("ftpsModeFromStorageName: ") + input);
    }

    struct FtpsModeStorageNameCase {
        openscp::FtpsMode input;
        const char *expected;
        const char *name;
    };
    constexpr std::array ftpsModeStorageNameCases{
        FtpsModeStorageNameCase{openscp::FtpsMode::Auto, "auto", "Auto"},
        FtpsModeStorageNameCase{openscp::FtpsMode::ExplicitTls, "explicit",
                                "ExplicitTls"},
        FtpsModeStorageNameCase{openscp::FtpsMode::ImplicitTls, "implicit",
                                "ImplicitTls"},
        FtpsModeStorageNameCase{static_cast<openscp::FtpsMode>(999), "auto",
                                "invalid"},
    };
    for (const auto &[input, expected, name] : ftpsModeStorageNameCases) {
        t.check(std::string(openscp::ftpsModeStorageName(input)) == expected,
                std::string("ftpsModeStorageName: ") + name);
    }

    struct WebDavSchemeFromStorageCase {
        const char *input;
        openscp::WebDavScheme expected;
    };
    constexpr std::array webDavSchemeFromStorageCases{
        WebDavSchemeFromStorageCase{"http", openscp::WebDavScheme::Http},
        WebDavSchemeFromStorageCase{"HTTPS", openscp::WebDavScheme::Https},
        WebDavSchemeFromStorageCase{"unknown", openscp::WebDavScheme::Https},
    };
    for (const auto &[input, expected] : webDavSchemeFromStorageCases) {
        t.check(openscp::webDavSchemeFromStorageName(input) == expected,
                std::string("webDavSchemeFromStorageName: ") + input);
    }

    struct WebDavSchemeStorageNameCase {
        openscp::WebDavScheme input;
        const char *expected;
        const char *name;
    };
    constexpr std::array webDavSchemeStorageNameCases{
        WebDavSchemeStorageNameCase{openscp::WebDavScheme::Http, "http",
                                    "HTTP"},
        WebDavSchemeStorageNameCase{openscp::WebDavScheme::Https, "https",
                                    "HTTPS"},
        WebDavSchemeStorageNameCase{static_cast<openscp::WebDavScheme>(999),
                                    "https", "invalid"},
    };
    for (const auto &[input, expected, name] : webDavSchemeStorageNameCases) {
        t.check(std::string(openscp::webDavSchemeStorageName(input)) ==
                    expected,
                std::string("webDavSchemeStorageName: ") + name);
    }

    struct WebDavSchemePortCase {
        openscp::WebDavScheme input;
        std::uint16_t expected;
        const char *name;
    };
    constexpr std::array webDavSchemePortCases{
        WebDavSchemePortCase{openscp::WebDavScheme::Http, 80, "HTTP"},
        WebDavSchemePortCase{openscp::WebDavScheme::Https, 443, "HTTPS"},
        WebDavSchemePortCase{static_cast<openscp::WebDavScheme>(999), 443,
                             "invalid"},
    };
    for (const auto &[input, expected, name] : webDavSchemePortCases) {
        t.check(openscp::defaultPortForWebDavScheme(input) == expected,
                std::string("defaultPortForWebDavScheme: ") + name);
    }

    struct ProxyTypeFromStorageCase {
        int input;
        openscp::ProxyType expected;
        const char *name;
    };
    constexpr std::array proxyTypeFromStorageCases{
        ProxyTypeFromStorageCase{static_cast<int>(openscp::ProxyType::Socks5),
                                 openscp::ProxyType::Socks5, "Socks5"},
        ProxyTypeFromStorageCase{
            static_cast<int>(openscp::ProxyType::HttpConnect),
            openscp::ProxyType::HttpConnect, "HttpConnect"},
        ProxyTypeFromStorageCase{999, openscp::ProxyType::None, "invalid"},
    };
    for (const auto &[input, expected, name] : proxyTypeFromStorageCases) {
        t.check(openscp::proxyTypeFromStorageValue(input) == expected,
                std::string("proxyTypeFromStorageValue: ") + name);
    }

    struct ProxyTypePortCase {
        openscp::ProxyType input;
        std::uint16_t expected;
        const char *name;
    };
    constexpr std::array proxyTypePortCases{
        ProxyTypePortCase{openscp::ProxyType::None, 0, "None"},
        ProxyTypePortCase{openscp::ProxyType::Socks5, 1080, "Socks5"},
        ProxyTypePortCase{openscp::ProxyType::HttpConnect, 8080, "HttpConnect"},
        ProxyTypePortCase{static_cast<openscp::ProxyType>(999), 0, "invalid"},
    };
    for (const auto &[input, expected, name] : proxyTypePortCases) {
        t.check(openscp::defaultPortForProxyType(input) == expected,
                std::string("defaultPortForProxyType: ") + name);
    }
    t.check(
        openscp::normalizeWebDavBasePath("remote.php//dav/./files/alice/") ==
            "/remote.php/dav/files/alice",
        "WebDAV base paths should be canonical absolute paths");
    t.check(openscp::normalizeWebDavBasePath("/dav/root/../files") ==
                "/dav/files",
            "WebDAV base paths should resolve dot segments");
    // A base copied from a URL used to be encoded a second time, so the
    // server got "john%2540example.com" and answered 404.
    t.check(openscp::normalizeWebDavBasePath(
                "/remote.php/dav/files/john%40example.com/My%20Files") ==
                "/remote.php/dav/files/john@example.com/My Files",
            "WebDAV base paths should decode the escapes of a URL");
    t.check(openscp::normalizeWebDavBasePath("/caf%c3%a9") == "/caf\xc3\xa9",
            "WebDAV base paths should decode escapes in either case");
    t.check(openscp::normalizeWebDavBasePath("/100%25/%zz/%4") ==
                "/100%/%zz/%4",
            "WebDAV base paths should keep a '%' that starts no escape");

    [[maybe_unused]] const openscp::ProtocolCapabilities managedFiles{
        .implemented = true,
        .can_list = true,
        .can_upload = true,
        .can_download = true,
        .can_stat = true,
        .can_mkdir = true,
        .can_delete = true,
        .can_rename = true,
        .can_read_metadata = true,
        .supports_proxy = true,
    };
    const openscp::ProtocolCapabilities sftpExpected{
        .implemented = true,
        .can_list = true,
        .can_upload = true,
        .can_download = true,
        .can_stat = true,
        .can_mkdir = true,
        .can_delete = true,
        .can_rename = true,
        .can_resume_download = true,
        .can_resume_upload = true,
        .can_read_metadata = true,
        .can_set_permissions = true,
        .can_set_ownership = true,
        .can_set_timestamps = true,
        .can_checksum = true,
        .supports_proxy = true,
        .supports_jump_host = true,
        .supports_known_hosts = true,
        .supports_transfer_integrity = true,
    };
    const openscp::ProtocolCapabilities scpExpected{
        .implemented = true,
        .can_upload = true,
        .can_download = true,
        .supports_proxy = true,
        .supports_jump_host = true,
        .supports_known_hosts = true,
    };

#if OPENSCP_HAS_CURL_WEBDAV
    const openscp::ProtocolCapabilities webdavExpected = managedFiles;
#else
    const openscp::ProtocolCapabilities webdavExpected{};
#endif
#if OPENSCP_HAS_CURL_FTP
    const openscp::ProtocolCapabilities ftpExpected = managedFiles;
    const openscp::ProtocolCapabilities ftpsExpected = managedFiles;
#else
    const openscp::ProtocolCapabilities ftpExpected{};
    const openscp::ProtocolCapabilities ftpsExpected{};
#endif

    struct ProtocolCapabilitiesCase {
        const char *name;
        openscp::Protocol input;
        openscp::ProtocolCapabilities expected;
    };
    const std::array capabilityCases{
        ProtocolCapabilitiesCase{"SFTP", openscp::Protocol::Sftp, sftpExpected},
        ProtocolCapabilitiesCase{"SCP", openscp::Protocol::Scp, scpExpected},
        ProtocolCapabilitiesCase{"FTP", openscp::Protocol::Ftp, ftpExpected},
        ProtocolCapabilitiesCase{"FTPS", openscp::Protocol::Ftps, ftpsExpected},
        ProtocolCapabilitiesCase{"WebDAV", openscp::Protocol::WebDav,
                                 webdavExpected},
    };
    struct CapabilityField {
        const char *name;
        bool openscp::ProtocolCapabilities::*member;
    };
    constexpr std::array capabilityFields{
        CapabilityField{"implemented",
                        &openscp::ProtocolCapabilities::implemented},
        CapabilityField{"can_list", &openscp::ProtocolCapabilities::can_list},
        CapabilityField{"can_upload",
                        &openscp::ProtocolCapabilities::can_upload},
        CapabilityField{"can_download",
                        &openscp::ProtocolCapabilities::can_download},
        CapabilityField{"can_stat", &openscp::ProtocolCapabilities::can_stat},
        CapabilityField{"can_mkdir", &openscp::ProtocolCapabilities::can_mkdir},
        CapabilityField{"can_delete",
                        &openscp::ProtocolCapabilities::can_delete},
        CapabilityField{"can_rename",
                        &openscp::ProtocolCapabilities::can_rename},
        CapabilityField{"can_resume_download",
                        &openscp::ProtocolCapabilities::can_resume_download},
        CapabilityField{"can_resume_upload",
                        &openscp::ProtocolCapabilities::can_resume_upload},
        CapabilityField{"can_read_metadata",
                        &openscp::ProtocolCapabilities::can_read_metadata},
        CapabilityField{"can_set_permissions",
                        &openscp::ProtocolCapabilities::can_set_permissions},
        CapabilityField{"can_set_ownership",
                        &openscp::ProtocolCapabilities::can_set_ownership},
        CapabilityField{"can_set_timestamps",
                        &openscp::ProtocolCapabilities::can_set_timestamps},
        CapabilityField{"can_checksum",
                        &openscp::ProtocolCapabilities::can_checksum},
        CapabilityField{"supports_proxy",
                        &openscp::ProtocolCapabilities::supports_proxy},
        CapabilityField{"supports_jump_host",
                        &openscp::ProtocolCapabilities::supports_jump_host},
        CapabilityField{"supports_known_hosts",
                        &openscp::ProtocolCapabilities::supports_known_hosts},
        CapabilityField{
            "supports_transfer_integrity",
            &openscp::ProtocolCapabilities::supports_transfer_integrity},
    };
    for (const auto &testCase : capabilityCases) {
        const auto actual = openscp::capabilitiesForProtocol(testCase.input);
        for (const auto &field : capabilityFields) {
            t.check(actual.*field.member == testCase.expected.*field.member,
                    std::string(testCase.name) +
                        " capabilities: " + field.name);
        }
    }
}

#if OPENSCP_HAS_CURL_FTP
OPENSCP_TEST(test_curlftp_rejects_unsupported_proxy_type, t) {
    openscp::CurlFtpClient client(openscp::Protocol::Ftp);
    openscp::SessionOptions opt;
    opt.protocol = openscp::Protocol::Ftp;
    opt.host = "127.0.0.1";
    opt.port = openscp::defaultPortForProtocol(openscp::Protocol::Ftp);
    opt.username = "alice";
    opt.proxy_type = static_cast<openscp::ProxyType>(999);
    opt.proxy_host = "127.0.0.1";
    opt.proxy_port = 8080;

    std::string err;
    const bool ok = client.connect(opt, err);
    t.check(!ok, "FTP connect should reject unsupported proxy enum values");
    t.checkContains(err, "Unsupported proxy type",
                    "FTP connect should explain unsupported proxy enum values");
    const openscp::RemoteError detail = client.lastOperationError();
    t.check(detail.kind == openscp::RemoteErrorKind::InvalidRequest,
            "FTP validation failure should expose a structured error");
}

#ifndef _WIN32
// A local port with nothing listening, as behind a firewall that blocks FTP
// data connections.
std::uint16_t closedLocalPort() {
    const int probe = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    const bool bound =
        probe >= 0 &&
        ::bind(probe, reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) == 0 &&
        ::getsockname(probe, reinterpret_cast<sockaddr *>(&address), &length) ==
            0;
    if (probe >= 0)
        ::close(probe);
    return bound ? ntohs(address.sin_port) : 0;
}

OPENSCP_TEST(test_curlftp_listing_skips_list_after_a_data_failure, t) {
    // An FTP control server whose passive data port refuses connections.
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    socklen_t length = sizeof(address);
    const std::uint16_t dataPort = closedLocalPort();
    const bool listening =
        listener >= 0 && dataPort != 0 &&
        ::bind(listener, reinterpret_cast<sockaddr *>(&address),
               sizeof(address)) == 0 &&
        ::listen(listener, 4) == 0 &&
        ::getsockname(listener, reinterpret_cast<sockaddr *>(&address),
                      &length) == 0;
    t.check(listening, "the fake FTP server should listen");
    if (!listening) {
        if (listener >= 0)
            ::close(listener);
        return;
    }
    std::atomic<int> dataCommands{0};
    std::thread server([listener, dataPort, &dataCommands] {
        for (;;) {
            const int client = ::accept(listener, nullptr, nullptr);
            if (client < 0)
                return;
            const auto reply = [client](const std::string &line) {
                const std::string text = line + "\r\n";
                (void)::send(client, text.data(), text.size(), 0);
            };
            reply("220 fake");
            std::string pending;
            char buffer[512];
            for (;;) {
                const std::size_t end = pending.find("\r\n");
                if (end == std::string::npos) {
                    const ssize_t n = ::recv(client, buffer, sizeof(buffer), 0);
                    if (n <= 0)
                        break;
                    pending.append(buffer, static_cast<std::size_t>(n));
                    continue;
                }
                const std::string command = pending.substr(0, end);
                pending.erase(0, end + 2);
                const std::string verb = command.substr(0, command.find(' '));
                if (verb == "USER")
                    reply("331 password");
                else if (verb == "PASS")
                    reply("230 logged in");
                else if (verb == "PWD")
                    reply("257 \"/\" is the current directory");
                else if (verb == "CWD")
                    reply("250 ok");
                else if (verb == "TYPE")
                    reply("200 ok");
                else if (verb == "EPSV") {
                    ++dataCommands;
                    reply("229 Entering Extended Passive Mode (|||" +
                          std::to_string(dataPort) + "|)");
                } else if (verb == "PASV") {
                    ++dataCommands;
                    reply("227 Entering Passive Mode (127,0,0,1," +
                          std::to_string(dataPort / 256) + "," +
                          std::to_string(dataPort % 256) + ")");
                } else if (verb == "QUIT") {
                    reply("221 bye");
                    break;
                } else {
                    reply("502 not implemented");
                }
            }
            ::close(client);
        }
    });

    openscp::CurlFtpClient client(openscp::Protocol::Ftp);
    openscp::SessionOptions opt;
    opt.protocol = openscp::Protocol::Ftp;
    opt.host = "127.0.0.1";
    opt.port = ntohs(address.sin_port);
    opt.username = "alice";
    opt.password = openscp::SecureString("secret");
    std::string err;
    std::vector<openscp::FileInfo> entries;
    const bool connected = client.connect(opt, err);
    t.check(connected, "the fake FTP server should accept the login: " + err);
    const int before = dataCommands.load();
    const bool listed = connected && client.list("/", entries, err);
    // libcurl tries PASV itself after EPSV fails, so one listing command
    // takes two; retrying with LIST took two more.
    t.check(!listed && dataCommands.load() - before == 2,
            "a listing whose data connection fails should not retry with "
            "LIST: " +
                std::to_string(dataCommands.load() - before) +
                " data command(s), " + err);
    client.disconnect();
    ::shutdown(listener, SHUT_RDWR);
    ::close(listener);
    server.join();
}
#endif

OPENSCP_TEST(test_curlftp_rejects_command_injection_paths, t) {
    openscp::CurlFtpClient client(openscp::Protocol::Ftp);
    std::vector<openscp::FileInfo> entries;
    std::string err;
    const bool ok = client.list("/safe\r\nDELE /important", entries, err);
    t.check(!ok, "FTP should reject CRLF in a remote path");
    t.checkContains(err, "forbidden control character",
                    "FTP path validation should explain the rejection");
    t.check(client.lastOperationError().kind ==
                openscp::RemoteErrorKind::InvalidRequest,
            "FTP path injection rejection should be structured");
}
#endif

#if OPENSCP_HAS_CURL_FTP || OPENSCP_HAS_CURL_WEBDAV
OPENSCP_TEST(test_curl_structured_error_mappings, t) {
    const openscp::RemoteError diskFull = openscp::curlcommon::errorFromCurl(
        CURLE_REMOTE_DISK_FULL, "remote disk full");
    t.check(diskFull.kind == openscp::RemoteErrorKind::InsufficientSpace &&
                !diskFull.transient,
            "CURLE_REMOTE_DISK_FULL should be a permanent space error");

    const openscp::RemoteError httpFull =
        openscp::curlcommon::errorFromHttpStatus(
            507, "WebDAV destination has insufficient storage", true);
    t.check(httpFull.kind == openscp::RemoteErrorKind::InsufficientSpace &&
                !httpFull.transient && !httpFull.commit_uncertain,
            "HTTP 507 should be a permanent, commit-certain space error");

    const openscp::RemoteError handshakeFailure =
        openscp::curlcommon::errorFromCurl(CURLE_SSL_CONNECT_ERROR,
                                           "TLS handshake failed");
    t.check(handshakeFailure.kind == openscp::RemoteErrorKind::Connection &&
                handshakeFailure.transient,
            "a broken TLS handshake should be a transient connection error");
    const openscp::RemoteError verificationFailure =
        openscp::curlcommon::errorFromCurl(CURLE_PEER_FAILED_VERIFICATION,
                                           "certificate rejected");
    t.check(verificationFailure.kind == openscp::RemoteErrorKind::Certificate &&
                !verificationFailure.transient,
            "TLS verification failures should not be retried");

    // Downloads keep the errno of the write that failed; libcurl makes more
    // calls before it returns, so a later errno says nothing.
    const int previousErrno = errno;
    errno = ENOSPC;
    const openscp::RemoteError localWrite = openscp::curlcommon::errorFromCurl(
        CURLE_WRITE_ERROR, "local write failed");
    errno = previousErrno;
    t.check(localWrite.kind == openscp::RemoteErrorKind::LocalIo,
            "a CURL write error should not read an unrelated errno");
}
#endif

#if OPENSCP_HAS_CURL_WEBDAV
OPENSCP_TEST(test_curlwebdav_rejects_control_characters, t) {
    openscp::CurlWebDavClient client;
    std::vector<openscp::FileInfo> entries;
    std::string err;
    const bool ok = client.list("/safe\nInjected: header", entries, err);
    t.check(!ok, "WebDAV should reject control characters in remote paths");
    t.checkContains(err, "forbidden control character",
                    "WebDAV path validation should explain the rejection");
    t.check(client.lastOperationError().kind ==
                openscp::RemoteErrorKind::InvalidRequest,
            "WebDAV path rejection should expose a structured error");
}
#endif

OPENSCP_TEST(test_connect_validation, t) {
    openscp::MockSftpClient c;
    std::string err;
    openscp::SessionOptions opt;
    opt.host = "";
    opt.username = "user";
    t.check(!c.connect(opt, err), "connect should fail when host is empty");

    err.clear();
    opt.host = "example.test";
    opt.username.clear();
    t.check(!c.connect(opt, err), "connect should fail when username is empty");

    err.clear();
    opt.username = "alice";
    t.check(c.connect(opt, err), "connect should succeed with host+username");
    t.check(c.isConnected(),
            "client should report connected after successful connect");
}

OPENSCP_TEST(test_disconnect_changes_state, t) {
    openscp::MockSftpClient c;
    std::string err;
    auto opt = validOptions();
    t.check(c.connect(opt, err),
            "connect should succeed before disconnect test");
    c.disconnect();
    t.check(!c.isConnected(), "disconnect should flip isConnected to false");

    std::vector<openscp::FileInfo> out;
    err.clear();
    t.check(!c.list("/", out, err), "list should fail after disconnect");
}

OPENSCP_TEST(test_list_requires_connection, t) {
    openscp::MockSftpClient c;
    std::vector<openscp::FileInfo> out;
    std::string err;
    t.check(!c.list("/", out, err), "list should fail when disconnected");
    t.check(!err.empty(), "list should provide error when disconnected");
    t.check(c.lastOperationError().kind == openscp::RemoteErrorKind::Connection,
            "disconnected mock operations should expose a connection error");
}

OPENSCP_TEST(test_mock_listings, t) {
    struct ExpectedListingEntry {
        const char *name;
        bool isDirectory;
    };
    struct ListingScenario {
        const char *name;
        const char *path;
        bool succeeds;
        std::vector<ExpectedListingEntry> expected;
    };
    const std::array scenarios{
        ListingScenario{"root",
                        "/",
                        true,
                        {{"home", true}, {"var", true}, {"readme.txt", false}}},
        ListingScenario{"empty path aliases root",
                        "",
                        true,
                        {{"home", true}, {"var", true}, {"readme.txt", false}}},
        ListingScenario{"home sorts directories before files",
                        "/home",
                        true,
                        {{"demo", true}, {"guest", true}, {"notes.md", false}}},
        ListingScenario{"missing path", "/does-not-exist", false, {}},
    };

    for (const ListingScenario &scenario : scenarios) {
        openscp::MockSftpClient client;
        std::string error;
        t.check(client.connect(validOptions(), error),
                std::string(scenario.name) + ": connect should succeed");
        std::vector<openscp::FileInfo> actual;
        error.clear();
        const bool listed = client.list(scenario.path, actual, error);
        t.check(listed == scenario.succeeds,
                std::string(scenario.name) + ": listing result should match");
        if (!scenario.succeeds) {
            t.check(!error.empty() && client.lastOperationError().kind ==
                                          openscp::RemoteErrorKind::NotFound,
                    std::string(scenario.name) +
                        ": missing listing should expose a structured error");
            continue;
        }
        t.check(actual.size() == scenario.expected.size(),
                std::string(scenario.name) + ": entry count should match");
        for (std::size_t index = 0;
             index < actual.size() && index < scenario.expected.size();
             ++index) {
            t.check(actual[index].name == scenario.expected[index].name &&
                        actual[index].is_dir ==
                            scenario.expected[index].isDirectory,
                    std::string(scenario.name) + ": entry " +
                        std::to_string(index) + " should be " +
                        scenario.expected[index].name);
        }
    }
}

OPENSCP_TEST(test_mock_capabilities_match_implemented_operations, t) {
    openscp::MockSftpClient c;
    const openscp::ProtocolCapabilities caps = c.capabilities();

    t.check(caps.implemented && caps.can_list && caps.can_stat &&
                caps.can_mkdir && caps.can_delete && caps.can_rename &&
                caps.can_read_metadata && caps.can_set_permissions &&
                caps.can_set_ownership && caps.can_set_timestamps,
            "mock should advertise its in-memory filesystem operations");
    t.check(!caps.can_upload && !caps.can_download && !caps.can_resume_upload &&
                !caps.can_resume_download && !caps.can_checksum,
            "mock should not advertise unimplemented transfer operations");
    t.check(!caps.supports_proxy && !caps.supports_jump_host &&
                !caps.supports_known_hosts && !caps.supports_transfer_integrity,
            "mock should not advertise real transport features");
}

OPENSCP_TEST(test_mock_transfer_methods_report_structured_errors, t) {
    openscp::MockSftpClient c;
    std::string err;

    auto expectUnsupported = [&](const std::string &operation, auto &&invoke) {
        err.clear();
        t.check(!invoke(), operation + " should be unsupported in mock");
        t.checkContains(err, "does not implement",
                        operation + " should expose unsupported message");
        t.check(c.lastOperationError().kind ==
                    openscp::RemoteErrorKind::Unsupported,
                operation + " should expose a structured unsupported error");
    };

    expectUnsupported(
        "get", [&] { return c.get("/remote", "/local", err, {}, {}, false); });
    expectUnsupported(
        "put", [&] { return c.put("/local", "/remote", err, {}, {}, false); });

    std::vector<std::uint8_t> digest{1, 2, 3};
    err.clear();
    t.check(!c.checksum("/remote", "SHA-256", digest, err),
            "the compatible default checksum implementation should fail");
    t.check(digest.empty(),
            "an unsupported checksum must clear the output digest");
    t.check(c.lastOperationError().kind ==
                openscp::RemoteErrorKind::Unsupported,
            "an unsupported checksum should expose a structured error");
    t.check(!c.capabilities().can_checksum,
            "a mock without hashing must not advertise checksum support");
}

OPENSCP_TEST(test_mock_filesystem_crud_and_metadata, t) {
    openscp::MockSftpClient c;
    c.resetFilesystem();

    openscp::FileInfo directory;
    directory.is_dir = true;
    directory.mode = 0750;
    t.check(c.addEntry("/workspace", directory),
            "fixture API should add a directory below the mock root");

    openscp::FileInfo file;
    file.size = 42;
    file.has_size = true;
    file.mtime = 100;
    file.mode = 0640;
    file.uid = 10;
    file.gid = 20;
    t.check(c.addEntry("/workspace/report.txt", file),
            "fixture API should add a file with metadata");
    t.check(!c.addEntry("/missing/file.txt", file),
            "fixture API should reject entries with a missing parent");

    std::string err;
    t.check(c.connect(validOptions(), err),
            "connect should succeed before mock filesystem operations");

    bool isDirectory = true;
    t.check(c.exists("/workspace/report.txt", isDirectory, err) &&
                !isDirectory && err.empty(),
            "exists should find configured files without an error");

    openscp::FileInfo info;
    t.check(c.stat("/workspace/report.txt", info, err) &&
                info.name == "report.txt" && info.size == 42 && info.has_size &&
                info.mtime == 100 && info.mode == 0640 && info.uid == 10 &&
                info.gid == 20,
            "stat should return configured file metadata");

    t.check(c.chmod("/workspace/report.txt", 0600, err) &&
                c.chown("/workspace/report.txt", 1000, 1001, err) &&
                c.setTimes("/workspace/report.txt", 200, 300, err),
            "metadata mutations should succeed for existing entries");
    t.check(c.stat("/workspace/report.txt", info, err) && info.mode == 0600 &&
                info.uid == 1000 && info.gid == 1001 && info.mtime == 300,
            "metadata mutations should persist in the mock filesystem");
    constexpr auto kUnchanged = static_cast<std::uint32_t>(-1);
    t.check(c.chown("/workspace/report.txt", 1002, kUnchanged, err) &&
                c.chown("/workspace/report.txt", kUnchanged, 1003, err) &&
                c.stat("/workspace/report.txt", info, err) &&
                info.uid == 1002 && info.gid == 1003,
            "chown with an unchanged id should keep that id");

    t.check(c.mkdir("/workspace/archive", err, 0700),
            "mkdir should create a directory in the mock filesystem");
    t.check(
        c.rename("/workspace/report.txt", "/workspace/archive/report.txt", err),
        "rename should move an entry in the mock filesystem");
    t.check(c.rename("/workspace/archive", "/workspace/finished", err),
            "rename should move directories together with their children");
    t.check(c.stat("/workspace/finished/report.txt", info, err) &&
                info.size == 42,
            "renamed directory children should keep their metadata");
    t.check(c.removeFile("/workspace/finished/report.txt", err),
            "removeFile should remove a file from the mock filesystem");
    t.check(c.removeDir("/workspace/finished", err),
            "removeDir should remove an empty directory");

    isDirectory = true;
    err = "stale";
    t.check(!c.exists("/workspace/finished", isDirectory, err) &&
                !isDirectory && err.empty(),
            "a missing entry should be a normal negative exists result");
    t.check(c.lastOperationError().kind == openscp::RemoteErrorKind::NotFound,
            "a missing entry should retain structured not-found metadata");
}

OPENSCP_TEST(test_mock_rejects_invalid_mutations, t) {
    openscp::MockSftpClient c;
    std::string err;
    t.check(c.connect(validOptions(), err),
            "connect should succeed before invalid mutation checks");

    t.check(!c.removeDir("/home", err),
            "removeDir should reject a non-empty directory");
    t.check(c.lastOperationError().kind == openscp::RemoteErrorKind::Conflict,
            "non-empty directory removal should expose a conflict");

    err.clear();
    t.check(!c.rename("/home/notes.md", "/readme.txt", err),
            "rename should not replace an entry unless requested");
    t.check(c.lastOperationError().kind == openscp::RemoteErrorKind::Conflict,
            "an occupied rename destination should expose a conflict");
    t.check(c.rename("/home/notes.md", "/readme.txt", err, true),
            "rename should replace a same-type destination when requested");
    openscp::FileInfo replaced;
    t.check(c.stat("/readme.txt", replaced, err) && replaced.size == 2048,
            "overwrite rename should preserve the source metadata");

    err.clear();
    t.check(!c.removeFile("/home", err),
            "removeFile should reject directory targets");
    t.check(c.lastOperationError().kind ==
                openscp::RemoteErrorKind::InvalidRequest,
            "a wrong entry type should expose an invalid request");
}

OPENSCP_TEST(test_demo_server_clients_share_state, t) {
    auto opt = validOptions();
    std::string err;
    auto first = openscp::MockSftpClient::onDemoServer();
    auto second = openscp::MockSftpClient::onDemoServer();
    t.check(first->connect(opt, err) && second->connect(opt, err),
            "demo server clients should connect");
    t.check(first->mkdir("/demo-shared", err),
            "a demo server client should mutate the demo filesystem");
    std::vector<openscp::FileInfo> entries;
    t.check(second->list("/", entries, err) &&
                std::any_of(entries.cbegin(), entries.cend(),
                            [](const openscp::FileInfo &entry) {
                                return entry.is_dir &&
                                       entry.name == "demo-shared";
                            }),
            "separately created demo connections should see the same files");
    t.check(second->removeDir("/demo-shared", err),
            "the demo filesystem should be restored for later tests");
}

OPENSCP_TEST(test_connected_client_validation, t) {
    openscp::SessionOptions bad;
    bad.host = "";
    bad.username = "alice";
    std::string err;
    auto conn = openscp::CreateConnectedClient(bad, err);
    t.check(!conn, "CreateConnectedClient should fail with invalid options");
    t.check(!err.empty(),
            "CreateConnectedClient should report validation errors");
}

OPENSCP_TEST(test_ssh_cipher_preference_follows_aes_instructions, t) {
    const auto sortedCiphers = [](const std::string &preference) {
        std::vector<std::string> ciphers;
        std::size_t start = 0;
        while (start <= preference.size()) {
            const std::size_t comma = preference.find(',', start);
            const std::size_t end =
                comma == std::string::npos ? preference.size() : comma;
            ciphers.push_back(preference.substr(start, end - start));
            start = end + 1;
        }
        std::sort(ciphers.begin(), ciphers.end());
        return ciphers;
    };
    const std::string withAes =
        openscp::libssh2detail::sshCipherPreference(true);
    const std::string withoutAes =
        openscp::libssh2detail::sshCipherPreference(false);
    t.check(withAes.rfind("aes256-gcm@openssh.com,", 0) == 0,
            "CPUs with AES-GCM instructions should prefer AES-GCM");
    t.check(withoutAes.rfind("chacha20-poly1305@openssh.com,", 0) == 0,
            "CPUs without AES-GCM instructions should prefer chacha20");
    t.check(sortedCiphers(withAes) == sortedCiphers(withoutAes) &&
                sortedCiphers(withAes).size() == 5,
            "both cipher orders should offer the same ciphers");
#if defined(__APPLE__) && defined(__aarch64__)
    t.check(openscp::libssh2detail::cpuHasAesGcmInstructions(),
            "Apple silicon should report AES-GCM instructions");
#endif
}

OPENSCP_TEST(test_client_factory, t) {
    auto sftp = openscp::CreateClientForProtocol(openscp::Protocol::Sftp);
    t.check(static_cast<bool>(sftp),
            "factory should create SFTP backend instance");
    if (sftp) {
        t.check(sftp->protocol() == openscp::Protocol::Sftp,
                "SFTP backend should report SFTP protocol");
    }

    auto scp = openscp::CreateClientForProtocol(openscp::Protocol::Scp);
    t.check(static_cast<bool>(scp),
            "factory should create SCP backend instance");
    if (scp) {
        t.check(scp->protocol() == openscp::Protocol::Scp,
                "SCP backend should report SCP protocol");
    }

    auto ftp = openscp::CreateClientForProtocol(openscp::Protocol::Ftp);
    auto ftps = openscp::CreateClientForProtocol(openscp::Protocol::Ftps);
    auto webdav = openscp::CreateClientForProtocol(openscp::Protocol::WebDav);
#if OPENSCP_HAS_CURL_FTP
    t.check(static_cast<bool>(ftp),
            "factory should create FTP backend instance");
    if (ftp) {
        t.check(ftp->protocol() == openscp::Protocol::Ftp,
                "FTP backend should report FTP protocol");
    }
    t.check(static_cast<bool>(ftps),
            "factory should create FTPS backend instance");
    if (ftps) {
        t.check(ftps->protocol() == openscp::Protocol::Ftps,
                "FTPS backend should report FTPS protocol");
    }
#else
    t.check(!ftp,
            "factory should return null for FTP when backend is disabled");
    t.check(!ftps,
            "factory should return null for FTPS when backend is disabled");
#endif
#if OPENSCP_HAS_CURL_WEBDAV
    t.check(static_cast<bool>(webdav),
            "factory should create WebDAV backend instance");
    if (webdav) {
        t.check(webdav->protocol() == openscp::Protocol::WebDav,
                "WebDAV backend should report WebDAV protocol");
    }
#else
    t.check(!webdav,
            "factory should return null for WebDAV when backend is disabled");
#endif
}

OPENSCP_TEST(test_libssh2_rejects_conflicting_proxy_and_jump, t) {
    openscp::Libssh2SftpClient c;
    openscp::SessionOptions opt = validOptions();
    opt.proxy_type = openscp::ProxyType::Socks5;
    opt.proxy_host = "127.0.0.1";
    opt.proxy_port = 1080;
    opt.jump_host = std::string("bastion.example.test");
    opt.jump_port = 22;

    std::string err;
    const bool ok = c.connect(opt, err);
    t.check(!ok, "connect should fail when proxy and jump are both configured");
    t.checkContains(err, "Proxy and SSH jump host cannot be used together",
                    "connect should explain proxy/jump mutual exclusion");
    t.check(c.lastOperationError().kind ==
                openscp::RemoteErrorKind::InvalidRequest,
            "libssh2 validation failures should expose structured metadata");
}

OPENSCP_TEST(test_libssh2_backends_expose_structured_errors, t) {
    openscp::Libssh2SftpClient sftp;
    std::vector<openscp::FileInfo> entries;
    std::string err;
    t.check(!sftp.list("/", entries, err),
            "SFTP listing should fail while disconnected");
    const openscp::RemoteError sftpError = sftp.lastOperationError();
    t.check(sftpError.kind == openscp::RemoteErrorKind::Connection &&
                sftpError.transient,
            "disconnected SFTP operations should expose a transient "
            "connection error");

    openscp::Libssh2ScpClient scp;
    err.clear();
    t.check(!scp.list("/", entries, err),
            "SCP should reject unsupported directory listing");
    t.check(scp.lastOperationError().kind ==
                openscp::RemoteErrorKind::Unsupported,
            "unsupported SCP operations should expose structured metadata");

    err.clear();
    t.check(!scp.get("/remote", "/local", err, {}, {}, false),
            "SCP download should fail while disconnected");
    const openscp::RemoteError scpError = scp.lastOperationError();
    t.check(scpError.kind == openscp::RemoteErrorKind::Connection &&
                scpError.transient,
            "disconnected SCP transfers should expose a transient connection "
            "error");
}

OPENSCP_TEST(test_libssh2_disconnect_is_thread_safe_and_idempotent, t) {
    openscp::Libssh2SftpClient client;
    std::vector<std::thread> threads;
    threads.reserve(8);
    for (int i = 0; i < 8; ++i) {
        threads.emplace_back([&client] {
            client.interrupt();
            client.disconnect();
        });
    }
    for (auto &th : threads) {
        th.join();
    }
    t.check(!client.isConnected(), "client should be disconnected");
}

OPENSCP_TEST(test_libssh2_concurrent_disconnect_and_operations_safe, t) {
    openscp::Libssh2SftpClient client;
    std::atomic<bool> stop{false};

    std::thread worker([&client, &stop] {
        std::vector<openscp::FileInfo> entries;
        std::string err;
        openscp::FileInfo info;
        while (!stop.load()) {
            (void)client.list("/test", entries, err);
            (void)client.stat("/test/file", info, err);
            (void)client.get("/test/file", "/tmp/nonexistent", err, {}, {},
                             false);
        }
    });

    std::thread disconnector([&client, &stop] {
        for (int i = 0; i < 100; ++i) {
            client.interrupt();
            client.disconnect();
            std::this_thread::yield();
        }
        stop.store(true);
    });

    disconnector.join();
    worker.join();
    t.check(!client.isConnected(), "client should remain disconnected");
}

OPENSCP_TEST(test_shared_libssh2_error_classification, t) {
    const openscp::RemoteError authentication =
        openscp::libssh2detail::classifyFailure(
            "Private key authentication failed", nullptr);
    t.check(authentication.kind == openscp::RemoteErrorKind::Authentication,
            "shared libssh2 errors should classify authentication failures");

    const openscp::RemoteError connection =
        openscp::libssh2detail::classifyFailure(
            "Connection closed while finalizing upload", nullptr, nullptr,
            true);
    t.check(connection.kind == openscp::RemoteErrorKind::Connection &&
                connection.transient && connection.commit_uncertain,
            "shared libssh2 errors should mark uncertain mutations");

    const openscp::RemoteError local = openscp::libssh2detail::classifyFailure(
        "Could not finalize local .part file", nullptr);
    t.check(local.kind == openscp::RemoteErrorKind::LocalIo ||
                local.kind == openscp::RemoteErrorKind::InsufficientSpace,
            "shared libssh2 errors should classify local file failures");

    for (const char *path :
         {"/srv/backups/checksums", "/home/ana/orders-canceled",
          "/etc/password-policies", "/data/fingerprints", "/srv/connections"}) {
        const openscp::RemoteError listing =
            openscp::libssh2detail::classifyFailure(
                std::string("sftp_opendir failed for: ") + path, nullptr);
        t.check(listing.kind == openscp::RemoteErrorKind::RemoteIo,
                std::string("a word in a path should not classify the "
                            "failure: ") +
                    path);
    }
    const openscp::RemoteError serverReason =
        openscp::libssh2detail::classifyFailure(
            "Could not open remote file for SCP download: scp: "
            "/etc/password-policies/list: Permission denied",
            nullptr);
    t.check(serverReason.kind == openscp::RemoteErrorKind::PermissionDenied,
            "the server's reason should still classify the failure");
}

OPENSCP_TEST(test_libssh2_recent_remote_modification, t) {
    using openscp::libssh2detail::kRecentRemoteModificationSeconds;
    using openscp::libssh2detail::remoteFileMayBeChanging;
    constexpr std::int64_t now = 1'800'000'000;
    const auto windowStart =
        static_cast<std::uint64_t>(now - kRecentRemoteModificationSeconds);

    t.check(!remoteFileMayBeChanging(windowStart - 1, now),
            "a file modified before the window should use the streamed check");
    t.check(remoteFileMayBeChanging(windowStart, now),
            "a file modified at the window start should use the full check");
    t.check(remoteFileMayBeChanging(static_cast<std::uint64_t>(now) + 60, now),
            "a server clock ahead of the local clock should use the full "
            "check");
    t.check(remoteFileMayBeChanging(std::nullopt, now),
            "an unknown modification time should use the full check");
    t.check(
        remoteFileMayBeChanging(std::numeric_limits<std::uint64_t>::max(), now),
        "an out-of-range modification time should use the full check");
}

OPENSCP_TEST(test_libssh2_jump_host_spawn, t) {
    openscp::Libssh2SftpClient c;
    openscp::SessionOptions opt = validOptions();
    opt.jump_host = std::string("127.0.0.1");
    opt.jump_port = 65534;
    opt.host = "127.0.0.1";
    opt.port = 22;

    std::string err;
    const bool ok = c.connect(opt, err);
    t.check(!ok, "connect should fail when jump bastion cannot connect");
    t.check(!err.empty(), "connect error should not be empty");
    t.check(!c.isConnected(), "client should remain disconnected");
}

#ifdef _WIN32
OPENSCP_TEST(test_windows_local_files_open_past_max_path, t) {
    // Downloads make folders and files relative to their parents, so they
    // reach paths past MAX_PATH even where the system does not enable long
    // paths; uploads and integrity checks then open them by the whole path.
    const fs::path base = makeTempFilePath("long-path").parent_path();
    fs::path folder = base;
    while (folder.native().size() <= MAX_PATH)
        folder /= std::string(40, 'd');
    const fs::path file = folder / "file.txt";
    std::string error;
    t.checkWithError(
        openscp::localfiles::ensureLocalDirectories(folder.string(), error),
        "the long folders should be made: ", error);
    openscp::UniqueFile written(openscp::localfiles::openRegularFileForWrite(
        file.string(), openscp::localfiles::WriteMode::Truncate, error));
    t.checkWithError(written && std::fputs("long", written.get()) >= 0 &&
                         written.close() == 0,
                     "the long file should be written: ", error);

    const auto opens = [](const std::string &path) {
        return static_cast<bool>(
            openscp::UniqueFile(openscp::openLocalFile(path, "rb")));
    };
    t.check(opens(file.string()),
            "a file past MAX_PATH should open for reading");
    // Windows turns "/" into "\" and drops "." names, but not in the form
    // with the \\?\ prefix that reaches past MAX_PATH.
    t.check(opens((folder / "." / "file.txt").generic_string()),
            "a path past MAX_PATH with \"/\" and \".\" should open too");
    std::uint64_t size = 0;
    std::string why;
    const bool sized =
        openscp::libssh2detail::getLocalFileSize(file.string(), size, &why);
    t.checkWithError(sized && size == 4,
                     "a file past MAX_PATH should have its size: ", why);

    // A relative path still starts at the current directory.
    wchar_t previous[MAX_PATH];
    const DWORD previousLength = ::GetCurrentDirectoryW(MAX_PATH, previous);
    t.check(::SetCurrentDirectoryW(base.c_str()) != 0,
            "the test should enter its folder");
    t.check(opens(file.lexically_relative(base).string()),
            "a relative path past MAX_PATH should open from the current "
            "directory");
    if (previousLength > 0 && previousLength < MAX_PATH)
        ::SetCurrentDirectoryW(previous);

    std::error_code ec;
    fs::remove_all(L"\\\\?\\" + base.wstring(), ec);
}

OPENSCP_TEST(test_windows_short_names_are_refused_only_as_names, t) {
    const fs::path base = makeTempFilePath("short-names").parent_path();
    const fs::path archive = base / "Documents Archive";
    std::error_code ec;
    fs::create_directories(archive, ec);
    wchar_t shortPath[MAX_PATH];
    const DWORD length =
        ::GetShortPathNameW(archive.c_str(), shortPath, MAX_PATH);
    const fs::path alias =
        length > 0 && length < MAX_PATH ? fs::path(shortPath) : fs::path();
    if (alias.empty() || alias.filename() == archive.filename()) {
        t.skip("the temporary folder's volume makes no short names");
        fs::remove_all(base, ec);
        return;
    }
    // A remote entry with that name would land in "Documents Archive".
    t.check(!openscp::localfiles::localNameProblem(alias.filename().string())
                 .empty(),
            "a name equal to another entry's short name should be refused");
    // A path the user gives may still reach a folder by its short name.
    std::string error;
    const bool followed = openscp::localfiles::ensureLocalDirectories(
        (alias / "inside").string(), error);
    t.checkWithError(
        followed, "a short name in a whole path should be followed: ", error);
    t.check(fs::is_directory(archive / "inside", ec),
            "the short name should reach the long-named folder");
    fs::remove_all(base, ec);
}

OPENSCP_TEST(test_windows_links_are_trusted_where_users_cannot_add, t) {
    // ProgramData belongs to the system, but any user may add entries to it,
    // so a link there may come from another account. A link in C:\Users,
    // which users cannot change, is still followed; making one there needs an
    // administrator, so this test cannot.
    std::error_code ec;
    const char *programData = std::getenv("ProgramData");
    if (!programData) {
        t.skip("no ProgramData folder");
        return;
    }
    const fs::path target = makeTempFilePath("trust-target").parent_path();
    const fs::path link =
        fs::path(programData) /
        ("openscp-trust-test-" + std::to_string(::GetCurrentProcessId()));
    if (!createDirectoryLink(target, link)) {
        t.skip("a junction could not be made in ProgramData");
        fs::remove_all(target, ec);
        return;
    }
    std::string error;
    const bool followed = openscp::localfiles::ensureLocalDirectories(
        (link / "inside").string(), error);
    ::RemoveDirectoryW(link.c_str());
    fs::remove_all(target, ec);
    t.check(!followed && error.find("untrusted") != std::string::npos,
            "a link where any user may add entries should be refused: " +
                error);
}

OPENSCP_TEST(test_windows_jump_ssh_comes_from_absolute_path_folders, t) {
    namespace detail = openscp::libssh2detail;
    const fs::path base = makeTempFilePath("ssh-search").parent_path();
    const fs::path current = base / "current";
    const fs::path tools = base / "tools";
    const fs::path empty = base / "empty";
    std::error_code ec;
    for (const fs::path &folder : {current, tools, empty})
        fs::create_directories(folder, ec);
    for (const fs::path &folder : {current, tools})
        std::ofstream(folder / "ssh.exe", std::ios::binary) << "MZ";
    const std::wstring missing = (base / "missing" / "ssh.exe").wstring();

    // SearchPathW resolved "", "." and other relative entries against the
    // current directory, where an ssh.exe would then run.
    wchar_t previous[MAX_PATH];
    const DWORD previousLength = ::GetCurrentDirectoryW(MAX_PATH, previous);
    t.check(::SetCurrentDirectoryW(current.c_str()) != 0,
            "the test should enter the folder that holds the decoy");
    t.check(detail::findSshExecutable(L";.;relative;;C:relative;\\relative;" +
                                          empty.wstring(),
                                      missing)
                .empty(),
            "empty and relative PATH entries must not reach the current "
            "directory");
    t.check(detail::findSshExecutable(empty.wstring() + L";\"" +
                                          tools.wstring() + L"\";" +
                                          current.wstring(),
                                      missing) == (tools / "ssh.exe").wstring(),
            "the first absolute folder with ssh.exe should win, quoted or "
            "not");
    if (previousLength > 0 && previousLength < MAX_PATH)
        ::SetCurrentDirectoryW(previous);

    const std::wstring fallback = (tools / "ssh.exe").wstring();
    t.check(detail::findSshExecutable(empty.wstring(), fallback) == fallback,
            "without one in PATH, the fallback should be used");
    t.check(detail::findSshExecutable(empty.wstring(), missing).empty(),
            "a missing fallback should give no program");
    fs::remove_all(base, ec);
}

OPENSCP_TEST(test_windows_jump_relay_carries_both_directions, t) {
    namespace detail = openscp::libssh2detail;
    // The argument probe stands in for ssh.exe, first in PATH, and echoes
    // what it reads, as ssh -W relays a connection.
    const fs::path folder =
        makeTempFilePath("jump-relay").parent_path() / "probe";
    std::error_code ec;
    fs::create_directories(folder, ec);
    fs::copy_file(OPENSCP_ARGV_PROBE, folder / "ssh.exe", ec);
    t.check(!ec, "the probe should stand in for ssh.exe");
    std::wstring previousPath(32767, L'\0');
    previousPath.resize(::GetEnvironmentVariableW(
        L"PATH", previousPath.data(), static_cast<DWORD>(previousPath.size())));
    ::SetEnvironmentVariableW(L"PATH",
                              (folder.wstring() + L";" + previousPath).c_str());
    ::SetEnvironmentVariableW(L"OPENSCP_ARGV_PROBE_ECHO", L"relay ended");
    {
        detail::SshJumpTunnel tunnel;
        detail::SocketHandle socket = detail::kInvalidSocket;
        std::string error;
        const bool started = tunnel.start(
            {"-W", "target.example:22", "jump.example"}, socket, error);
        t.checkWithError(started, "the relay should start: ", error);
        if (started) {
            const std::string sent = "SSH-2.0-relay\r\n";
            t.check(detail::sendOnce(socket, sent.data(), sent.size()) ==
                        static_cast<std::ptrdiff_t>(sent.size()),
                    "libssh2's bytes should reach ssh.exe");
            std::string received;
            char buffer[64];
            while (received.size() < sent.size()) {
                const std::ptrdiff_t count =
                    detail::receiveOnce(socket, buffer, sizeof(buffer));
                if (count <= 0)
                    break;
                received.append(buffer, static_cast<std::size_t>(count));
            }
            t.check(received == sent,
                    "ssh.exe's output should come back through the relay");
            // As when libssh2 closes its end, ssh.exe's input ends.
            detail::shutdownSocket(socket);
            t.check(tunnel.exited(5000),
                    "ssh.exe should exit once its input ends");
            t.check(tunnel.errorOutput().find("relay ended") !=
                        std::string::npos,
                    "ssh.exe's error output should be kept");
            detail::closeSocket(socket);
        }
    }
    ::SetEnvironmentVariableW(L"PATH", previousPath.c_str());
    ::SetEnvironmentVariableW(L"OPENSCP_ARGV_PROBE_ECHO", nullptr);
    fs::remove_all(folder.parent_path(), ec);
}

OPENSCP_TEST(test_windows_jump_command_line_round_trips, t) {
    const std::vector<std::wstring> arguments = {
        L"-l",
        L"jos\u00e9 luis",
        L"-i",
        L"C:\\Users\\Jos\u00e9 Luis\\.ssh\\id_ed25519",
        L"-W",
        L"[2001:db8::1]:22",
        L"--",
        L"bastion.example.test",
        L"",
        L"ends with a backslash\\",
        L"quote\"inside",
        L"backslashes\\\\ before a quote\\\"",
        L"*?[]{}'~"};
    const std::wstring program = L"C:\\Windows\\System32\\OpenSSH\\ssh.exe";
    const std::wstring commandLine =
        openscp::libssh2detail::windowsCommandLine(program, arguments);
    int count = 0;
    LPWSTR *parsed = ::CommandLineToArgvW(commandLine.c_str(), &count);
    const bool programKept = parsed && count > 0 && program == parsed[0];
    std::vector<std::wstring> readBack;
    for (int index = 1; parsed && index < count; ++index)
        readBack.emplace_back(parsed[index]);
    if (parsed)
        ::LocalFree(parsed);
    t.check(programKept, "the program should stay the first argument");
    t.check(readBack == arguments,
            "the C runtime should read every argument back as given");
}
#endif

// An "ssh-rsa" blob with exponent 65537 and a modulus of `modulusBytes`
// bytes whose first byte is `top`, as an SSH server sends it.
std::vector<unsigned char> rsaHostKeyBlob(std::size_t modulusBytes,
                                          unsigned char top) {
    std::vector<unsigned char> blob;
    const auto appendField = [&blob](const std::vector<unsigned char> &data) {
        const auto size = static_cast<std::uint32_t>(data.size());
        for (const int shift : {24, 16, 8, 0})
            blob.push_back(static_cast<unsigned char>((size >> shift) & 0xffu));
        blob.insert(blob.end(), data.begin(), data.end());
    };
    appendField({'s', 's', 'h', '-', 'r', 's', 'a'});
    appendField({0x01, 0x00, 0x01});
    std::vector<unsigned char> modulus(modulusBytes, 0xa5);
    modulus.front() = top;
    if (top & 0x80u)
        modulus.insert(modulus.begin(), 0x00);
    appendField(modulus);
    return blob;
}

// The new-host prompt used to show 2048-bit RSA keys as 2232-bit, the size of
// the whole blob.
OPENSCP_TEST(test_rsa_host_key_bits_come_from_the_modulus, t) {
    using openscp::libssh2detail::rsaModulusBits;
    for (const std::size_t bits : {2048u, 3072u, 4096u}) {
        const auto blob = rsaHostKeyBlob(bits / 8, 0xc3);
        t.check(rsaModulusBits(blob.data(), blob.size()) ==
                    static_cast<int>(bits),
                "an RSA host key should show its modulus size: " +
                    std::to_string(bits));
    }
    const auto shortTop = rsaHostKeyBlob(256, 0x7f);
    t.check(rsaModulusBits(shortTop.data(), shortTop.size()) == 2047,
            "a modulus without its top bit should count one bit less");
    const auto truncated = rsaHostKeyBlob(256, 0xc3);
    t.check(rsaModulusBits(truncated.data(), truncated.size() - 1) == 0 &&
                rsaModulusBits(nullptr, 0) == 0,
            "a cut or missing blob should have no RSA size");
    const std::vector<unsigned char> ed25519 = {
        0, 0, 0, 11, 's', 's', 'h', '-', 'e', 'd', '2', '5', '5', '1', '9'};
    t.check(rsaModulusBits(ed25519.data(), ed25519.size()) == 0,
            "a key of another type should have no RSA size");
}

OPENSCP_TEST(test_remove_known_hosts_entry_plain_and_hashed, t) {
    const std::string key =
        "AAAAC3NzaC1lZDI1NTE5AAAAILZlz+tnMZZGpyX4/qwU9iIfMHkUqPnwGwGZRuQQ3v1d";
    const fs::path khPath = makeTempFilePath("openscp-knownhosts-cleanup");
    {
        std::ofstream out(khPath, std::ios::binary | std::ios::trunc);
        t.check(out.is_open(), "known_hosts fixture should be writable");
        if (!out.is_open())
            return;
        out << "example.com ssh-ed25519 " << key << "\n";
        out << "|1|ONUTBfXmPZryon7OlPHra65ZfXs=|lFM22IlwQQfIf9tvjwmXgUKqebE= "
               "ssh-ed25519 "
            << key << "\n";
        out << "other.example ssh-ed25519 " << key << "\n";
    }

    std::string err;
    const bool ok =
        openscp::RemoveKnownHostEntry(khPath.string(), "example.com", 22, err);
    t.check(ok, std::string("RemoveKnownHostEntry should succeed: ") + err);

    std::string content;
    t.check(readTextFile(khPath, content),
            "updated known_hosts fixture should be readable");
    t.check(content.find("example.com ssh-ed25519") == std::string::npos,
            "plain example.com entry should be removed");
    t.check(content.find("|1|ONUTBfXmPZryon7OlPHra65ZfXs=") ==
                std::string::npos,
            "hashed example.com entry should be removed");
    t.check(content.find("other.example ssh-ed25519") != std::string::npos,
            "unrelated known_hosts entry should be preserved");

    std::error_code ec;
    fs::remove(khPath, ec);
    fs::remove_all(khPath.parent_path(), ec);
}

// Deleting a site and its saved key used to delete the revocation of that key,
// so trust on first use could then offer the revoked key as a new host.
OPENSCP_TEST(test_remove_known_hosts_entry_keeps_marker_lines, t) {
    const std::string key =
        "AAAAC3NzaC1lZDI1NTE5AAAAILZlz+tnMZZGpyX4/qwU9iIfMHkUqPnwGwGZRuQQ3v1d";
    const fs::path khPath = makeTempFilePath("openscp-knownhosts-markers");
    {
        std::ofstream out(khPath, std::ios::binary | std::ios::trunc);
        t.check(out.is_open(), "known_hosts marker fixture should be writable");
        if (!out.is_open())
            return;
        out << "@revoked example.com ssh-ed25519 " << key << "\n";
        out << "@cert-authority example.com ssh-ed25519 " << key << "\n";
    }
    t.check(!openscp::HasKnownHostEntry(khPath.string(), "example.com", 22),
            "marker lines should not count as a saved key of the host");

    {
        std::ofstream out(khPath, std::ios::binary | std::ios::app);
        out << "example.com ssh-ed25519 " << key << "\n";
    }
    std::string err;
    t.checkWithError(
        openscp::RemoveKnownHostEntry(khPath.string(), "example.com", 22, err),
        "removing the saved key should succeed: ", err);
    std::string content;
    t.check(readTextFile(khPath, content),
            "updated known_hosts marker fixture should be readable");
    t.check(content.find("\nexample.com ssh-ed25519") == std::string::npos,
            "the saved key of the host should be removed");
    t.check(content.find("@revoked example.com") != std::string::npos &&
                content.find("@cert-authority example.com") !=
                    std::string::npos,
            "removing a saved key should keep revocations and CAs");

    std::error_code ec;
    fs::remove(khPath, ec);
    fs::remove_all(khPath.parent_path(), ec);
}

OPENSCP_TEST(test_remove_known_hosts_entry_non_default_port, t) {
    const std::string key =
        "AAAAC3NzaC1lZDI1NTE5AAAAILZlz+tnMZZGpyX4/qwU9iIfMHkUqPnwGwGZRuQQ3v1d";
    const fs::path khPath = makeTempFilePath("openscp-knownhosts-port");
    {
        std::ofstream out(khPath, std::ios::binary | std::ios::trunc);
        t.check(out.is_open(), "known_hosts port fixture should be writable");
        if (!out.is_open())
            return;
        out << "[example.com]:2222 ssh-ed25519 " << key << "\n";
        out << "example.com ssh-ed25519 " << key << "\n";
    }

    std::string err;
    const bool ok = openscp::RemoveKnownHostEntry(khPath.string(),
                                                  "example.com", 2222, err);
    t.check(ok, std::string("port-specific removal should succeed: ") + err);

    std::string content;
    t.check(readTextFile(khPath, content),
            "updated known_hosts port fixture should be readable");
    t.check(content.find("[example.com]:2222 ssh-ed25519") == std::string::npos,
            "port-specific known_hosts entry should be removed");
    t.check(content.find("example.com ssh-ed25519") != std::string::npos,
            "default-port known_hosts entry should remain");

    std::error_code ec;
    fs::remove(khPath, ec);
    fs::remove_all(khPath.parent_path(), ec);
}

OPENSCP_TEST(test_local_file_64bit_seek_and_size_large_offsets, t) {
    const fs::path tempPath = makeTempFilePath("openscp-large-seek-test");
    std::FILE *f = std::fopen(tempPath.string().c_str(), "w+b");
    t.check(f != nullptr, "temporary file should open for write");
    if (!f)
        return;
#ifdef _WIN32
    // NTFS writes zeros up to the offset unless the file is sparse.
    DWORD returned = 0;
    // NOLINTNEXTLINE(performance-no-int-to-ptr): the CRT gives an integer.
    (void)DeviceIoControl(reinterpret_cast<HANDLE>(_get_osfhandle(_fileno(f))),
                          FSCTL_SET_SPARSE, nullptr, 0, nullptr, 0, &returned,
                          nullptr);
#endif

    // Offset greater than 2 GiB (3 GiB = 3 * 1024 * 1024 * 1024)
    constexpr std::uint64_t largeOffset = 3ULL * 1024 * 1024 * 1024;
    std::string seekErr;
    const bool seekOk =
        openscp::libssh2detail::seekLocalFile(f, largeOffset, &seekErr);
    t.check(seekOk, "seekLocalFile should succeed beyond 2 GiB boundary");

    // Write 1 byte at 3 GiB to establish file size without allocating 3GB of
    // disk blocks (sparse file)
    const char byte = 'X';
    const size_t written = std::fwrite(&byte, 1, 1, f);
    t.check(written == 1, "fwrite at large offset should succeed");
    std::fflush(f);
    std::fclose(f);

    std::uint64_t measuredSize = 0;
    std::string sizeErr;
    const bool sizeOk = openscp::libssh2detail::getLocalFileSize(
        tempPath.string(), measuredSize, &sizeErr);
    t.check(sizeOk, "getLocalFileSize should succeed for files > 2 GiB");
    t.check(
        measuredSize == largeOffset + 1,
        "measured size should accurately reflect 64-bit file size (> 2 GiB)");

    std::error_code ec;
    fs::remove(tempPath, ec);
    fs::remove_all(tempPath.parent_path(), ec);
}

OPENSCP_TEST(test_unique_file_self_reset_and_move_prevent_double_close, t) {
    static int closeCallCount = 0;
    closeCallCount = 0;

    auto testCloser = [](std::FILE *fp) -> int {
        ++closeCallCount;
        return std::fclose(fp);
    };

    const fs::path tempPath = makeTempFilePath("openscp-uniquefile-test");
    std::FILE *rawFile = std::fopen(tempPath.string().c_str(), "w+b");
    t.check(rawFile != nullptr, "file should open for UniqueFile test");
    if (!rawFile)
        return;

    {
        openscp::UniqueFile uf(rawFile, testCloser);
        t.check(uf.get() == rawFile, "UniqueFile should manage raw file");

        // Self-reset: must be a no-op and NOT close or double-free the handle
        uf.reset(uf.get());
        t.check(closeCallCount == 0, "self-reset must not invoke closer");
        t.check(uf.get() == rawFile, "self-reset must preserve file handle");

        // Another self-reset with closer
        uf.reset(uf.get(), testCloser);
        t.check(closeCallCount == 0,
                "self-reset with closer must not invoke closer");
        t.check(uf.get() == rawFile, "file handle must remain valid");
    }

    // Now uf went out of scope: closer should have been called exactly once
    t.check(closeCallCount == 1,
            "closer should be called exactly once upon UniqueFile destruction");

    std::error_code ec;
    fs::remove(tempPath, ec);
    fs::remove_all(tempPath.parent_path(), ec);
}

OPENSCP_TEST(test_unique_sftp_handle_lifecycle_and_leak_prevention, t) {
    static int closeCallCount = 0;
    static LIBSSH2_SFTP_HANDLE *lastClosedHandle = nullptr;
    closeCallCount = 0;
    lastClosedHandle = nullptr;

    auto testCloser = [](LIBSSH2_SFTP_HANDLE *h) -> int {
        ++closeCallCount;
        lastClosedHandle = h;
        return 0;
    };

    int dummy1Storage = 0;
    int dummy2Storage = 0;
    auto *dummy1 = reinterpret_cast<LIBSSH2_SFTP_HANDLE *>(&dummy1Storage);
    auto *dummy2 = reinterpret_cast<LIBSSH2_SFTP_HANDLE *>(&dummy2Storage);

    // 1. RAII destruction closes the handle
    {
        openscp::libssh2detail::UniqueSftpHandle h(dummy1, testCloser);
        t.check(static_cast<bool>(h),
                "handle should evaluate to true when set");
        t.check(h.get() == dummy1, "handle get() should return raw handle");
    }
    t.check(closeCallCount == 1, "destruction must close remote handle");
    t.check(lastClosedHandle == dummy1, "closed handle should match dummy1");

    // 2. Manual close() closes immediately and destructor is a no-op
    {
        openscp::libssh2detail::UniqueSftpHandle h(dummy1, testCloser);
        t.check(h.close() == 0, "close() should return closer result");
        t.check(closeCallCount == 2, "explicit close() should invoke closer");
        t.check(!h, "closed handle should evaluate to false");
        t.check(h.get() == nullptr, "closed handle get() should be null");
    }
    t.check(closeCallCount == 2,
            "destructor after close() must not double close");

    // 3. Self-reset must be a no-op and NOT close
    {
        openscp::libssh2detail::UniqueSftpHandle h(dummy1, testCloser);
        h.reset(dummy1);
        t.check(closeCallCount == 2, "self-reset must not invoke closer");
        t.check(h.get() == dummy1,
                "handle should remain unchanged after self-reset");
    }
    t.check(closeCallCount == 3,
            "destruction after self-reset should close once");

    // 4. Move construction transfers ownership without closing
    {
        openscp::libssh2detail::UniqueSftpHandle h1(dummy1, testCloser);
        openscp::libssh2detail::UniqueSftpHandle h2(std::move(h1));
        // NOLINTNEXTLINE(bugprone-use-after-move): source is null by contract.
        t.check(h1.get() == nullptr, "moved-from handle must be null");
        t.check(h2.get() == dummy1, "moved-to handle must hold dummy1");
        t.check(closeCallCount == 3, "move constructor must not invoke closer");
    }
    t.check(closeCallCount == 4,
            "destruction of moved-to handle should close once");

    // 5. Move assignment closes destination's existing handle and transfers
    {
        openscp::libssh2detail::UniqueSftpHandle h1(dummy1, testCloser);
        openscp::libssh2detail::UniqueSftpHandle h2(dummy2, testCloser);
        h2 = std::move(h1);
        t.check(closeCallCount == 5,
                "move assignment must close old destination handle");
        t.check(lastClosedHandle == dummy2,
                "closed old destination handle should be dummy2");
        t.check(h2.get() == dummy1, "moved-to handle should now hold dummy1");
        // NOLINTNEXTLINE(bugprone-use-after-move): source is null by contract.
        t.check(h1.get() == nullptr, "moved-from handle should now be null");
    }
    t.check(closeCallCount == 6, "destruction of h2 should close dummy1");

    // 6. release() abandons ownership without invoking closer
    {
        openscp::libssh2detail::UniqueSftpHandle h(dummy1, testCloser);
        LIBSSH2_SFTP_HANDLE *rel = h.release();
        t.check(rel == dummy1, "release() must return raw handle");
        t.check(h.get() == nullptr, "handle after release must be null");
    }
    t.check(closeCallCount == 6,
            "destructor after release() must not invoke closer");
}

} // namespace

int main() {
    openscp::test::TestHarness harness("core");
    return harness.run();
}
