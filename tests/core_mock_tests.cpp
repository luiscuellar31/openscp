// Core unit tests without external framework (run via CTest).
#include "TestHarness.hpp"
#include "common/RemoteListingLimits.hpp"
#include "common/SafeLocalFile.hpp"
#include "common/UniqueFile.hpp"
#include "libssh2/Libssh2ScpClient.hpp"
#include "libssh2/Libssh2SftpClient.hpp"
#include "libssh2/detail/Libssh2CipherPreference.hpp"
#include "libssh2/detail/Libssh2ErrorClassifier.hpp"
#include "libssh2/detail/Libssh2InputSafety.hpp"
#include "libssh2/detail/Libssh2TransferIntegrity.hpp"
#include "mock/MockSftpClient.hpp"
#include "openscp/ClientFactory.hpp"
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

#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <vector>

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

bool readTextFile(const fs::path &path, std::string &out) {
    std::ifstream in(path, std::ios::binary);
    if (!in.is_open())
        return false;
    out.assign(std::istreambuf_iterator<char>(in),
               std::istreambuf_iterator<char>());
    return in.good() || in.eof();
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
    t.check(promptRequestsUsername(copied.front()),
            "bounded prompt matching should detect username prompts");

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

#ifndef _WIN32
    fs::create_symlink(target, partial, ec);
    t.check(!ec, "symlink fixture should be created");
    std::string symlinkError;
    openscp::UniqueFile unsafe(openscp::localfiles::openRegularFileForWrite(
        partial.string(), openscp::localfiles::WriteMode::Truncate,
        symlinkError));
    t.check(!unsafe,
            "local partial files must reject final-component symlinks");
    std::string preservedContents;
    t.check(readTextFile(target, preservedContents) &&
                preservedContents == "preserve-me",
            "rejecting a partial symlink must preserve its target");
    fs::remove(partial, ec);

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
#endif

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
        t.check(openscp::localfiles::flushAndSync(file.get(), error),
                std::string("safe partial file should sync: ") + error);
        file.reset();
        t.check(openscp::localfiles::atomicReplace(partial.string(),
                                                   target.string(), error),
                std::string("safe partial file should publish atomically: ") +
                    error);
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

    const openscp::RemoteError tlsFailure = openscp::curlcommon::errorFromCurl(
        CURLE_SSL_CONNECT_ERROR, "TLS handshake failed");
    t.check(tlsFailure.kind == openscp::RemoteErrorKind::Certificate &&
                !tlsFailure.transient,
            "TLS verification/handshake failures should not be retried");

    const int previousErrno = errno;
    errno = ENOSPC;
    const openscp::RemoteError localFull = openscp::curlcommon::errorFromCurl(
        CURLE_WRITE_ERROR, "local write failed");
    errno = previousErrno;
    t.check(localFull.kind == openscp::RemoteErrorKind::InsufficientSpace &&
                localFull.native_code == ENOSPC,
            "local ENOSPC during a CURL write should retain its native code");
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
            (void)client.get("/test/file", "/tmp/nonexistent", err, {}, {}, false);
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

#ifdef _WIN32
OPENSCP_TEST(test_libssh2_rejects_jump_on_windows, t) {
    openscp::Libssh2SftpClient c;
    openscp::SessionOptions opt = validOptions();
    opt.jump_host = std::string("bastion.example.test");
    opt.jump_port = 22;

    std::string err;
    const bool ok = c.connect(opt, err);
    t.check(!ok, "connect should fail when jump is configured on Windows");
    t.checkContains(err, "not supported on this platform",
                    "connect should explain jump is unsupported on Windows");
}
#else
OPENSCP_TEST(test_libssh2_jump_host_spawn_posix, t) {
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
#endif

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

    // Offset greater than 2 GiB (3 GiB = 3 * 1024 * 1024 * 1024)
    constexpr std::uint64_t largeOffset = 3ULL * 1024 * 1024 * 1024;
    std::string seekErr;
    const bool seekOk =
        openscp::libssh2detail::seekLocalFile(f, largeOffset, &seekErr);
    t.check(seekOk, "seekLocalFile should succeed beyond 2 GiB boundary");

    // Write 1 byte at 3 GiB to establish file size without allocating 3GB of disk blocks (sparse file)
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
    t.check(measuredSize == largeOffset + 1,
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

} // namespace

int main() {
    openscp::test::TestHarness harness("core");
    return harness.run();
}
