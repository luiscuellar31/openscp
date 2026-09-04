// Core unit tests without external framework (run via CTest).
#include "TestHarness.hpp"
#include "common/RemoteListingLimits.hpp"
#include "common/SafeLocalFile.hpp"
#include "common/UniqueFile.hpp"
#include "libssh2/Libssh2ScpClient.hpp"
#include "libssh2/Libssh2SftpClient.hpp"
#include "libssh2/detail/Libssh2ErrorClassifier.hpp"
#include "libssh2/detail/Libssh2InputSafety.hpp"
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

#include <cerrno>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
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
    t.check((std::is_same_v<decltype(o.password),
                            std::optional<openscp::SecureString>>),
            "session passwords should use SecureString storage");
}

OPENSCP_TEST(test_security_policy_normalization, t) {
    const auto invalidKnownHosts = static_cast<openscp::KnownHostsPolicy>(999);
    const auto invalidIntegrity =
        static_cast<openscp::TransferIntegrityPolicy>(-7);

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

    openscp::SessionOptions options = validOptions();
    options.known_hosts_policy = invalidKnownHosts;
    openscp::Libssh2SftpClient client;
    std::string error;
    t.check(!client.connect(options, error) &&
                client.lastOperationError().kind ==
                    openscp::RemoteErrorKind::InvalidRequest,
            "the SSH trust boundary must reject invalid security policies");

    options.known_hosts_policy = openscp::KnownHostsPolicy::Strict;
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
    fs::remove(target, ec);
    fs::remove_all(target.parent_path(), ec);
}

OPENSCP_TEST(test_protocol_helpers, t) {
    t.check(openscp::protocolFromStorageName("sftp") == openscp::Protocol::Sftp,
            "protocolFromStorageName should parse sftp");
    t.check(openscp::protocolFromStorageName("SCP") == openscp::Protocol::Scp,
            "protocolFromStorageName should parse scp case-insensitively");
    t.check(openscp::protocolFromStorageName("FTPS") == openscp::Protocol::Ftps,
            "protocolFromStorageName should parse ftps case-insensitively");
    t.check(openscp::protocolFromStorageName("unknown") ==
                openscp::Protocol::Sftp,
            "protocolFromStorageName should fallback to sftp");
    t.check(std::string(openscp::protocolStorageName(openscp::Protocol::Scp)) ==
                "scp",
            "protocolStorageName should serialize SCP");
    t.check(std::string(openscp::protocolDisplayName(openscp::Protocol::Scp)) ==
                "SCP",
            "protocolDisplayName should expose SCP label");
    t.check(openscp::scpTransferModeFromStorageName("auto") ==
                openscp::ScpTransferMode::Auto,
            "scpTransferModeFromStorageName should parse auto");
    t.check(openscp::scpTransferModeFromStorageName("SCP-ONLY") ==
                openscp::ScpTransferMode::ScpOnly,
            "scpTransferModeFromStorageName should parse scp-only");
    t.check(std::string(openscp::scpTransferModeStorageName(
                openscp::ScpTransferMode::ScpOnly)) == "scp-only",
            "scpTransferModeStorageName should serialize scp-only");
    t.check(openscp::proxyTypeFromStorageValue(static_cast<int>(
                openscp::ProxyType::Socks5)) == openscp::ProxyType::Socks5,
            "proxyTypeFromStorageValue should parse SOCKS5");
    t.check(openscp::proxyTypeFromStorageValue(
                static_cast<int>(openscp::ProxyType::HttpConnect)) ==
                openscp::ProxyType::HttpConnect,
            "proxyTypeFromStorageValue should parse HTTP CONNECT");
    t.check(openscp::proxyTypeFromStorageValue(999) == openscp::ProxyType::None,
            "proxyTypeFromStorageValue should fallback invalid values to None");
    t.check(openscp::defaultPortForProxyType(openscp::ProxyType::Socks5) ==
                1080,
            "default SOCKS5 proxy port should be 1080");
    t.check(openscp::defaultPortForProxyType(openscp::ProxyType::HttpConnect) ==
                8080,
            "default HTTP CONNECT proxy port should be 8080");
    t.check(openscp::webDavSchemeFromStorageName("http") ==
                openscp::WebDavScheme::Http,
            "webDavSchemeFromStorageName should parse http");
    t.check(
        openscp::webDavSchemeFromStorageName("HTTPS") ==
            openscp::WebDavScheme::Https,
        "webDavSchemeFromStorageName should parse https case-insensitively");
    t.check(std::string(openscp::webDavSchemeStorageName(
                openscp::WebDavScheme::Http)) == "http",
            "webDavSchemeStorageName should serialize http");
    t.check(openscp::defaultPortForWebDavScheme(openscp::WebDavScheme::Http) ==
                80,
            "default HTTP WebDAV port should be 80");
    t.check(openscp::defaultPortForWebDavScheme(openscp::WebDavScheme::Https) ==
                443,
            "default HTTPS WebDAV port should be 443");
    t.check(openscp::ftpsModeFromStorageName("EXPLICIT") ==
                openscp::FtpsMode::ExplicitTls,
            "FTPS mode parser should accept explicit TLS");
    t.check(openscp::ftpsModeFromStorageName("implicit-tls") ==
                openscp::FtpsMode::ImplicitTls,
            "FTPS mode parser should accept implicit TLS");
    t.check(std::string(openscp::ftpsModeStorageName(
                openscp::FtpsMode::ImplicitTls)) == "implicit",
            "FTPS mode serializer should persist implicit mode");
    t.check(
        openscp::normalizeWebDavBasePath("remote.php//dav/./files/alice/") ==
            "/remote.php/dav/files/alice",
        "WebDAV base paths should be canonical absolute paths");
    t.check(openscp::normalizeWebDavBasePath("/dav/root/../files") ==
                "/dav/files",
            "WebDAV base paths should resolve dot segments");

    const auto sftpCaps =
        openscp::capabilitiesForProtocol(openscp::Protocol::Sftp);
    t.check(sftpCaps.implemented, "SFTP capabilities should be implemented");
    t.check(sftpCaps.can_list, "SFTP capabilities should include listing");
    t.check(sftpCaps.can_upload && sftpCaps.can_download && sftpCaps.can_stat &&
                sftpCaps.can_mkdir && sftpCaps.can_delete &&
                sftpCaps.can_rename,
            "SFTP should advertise fine-grained remote operations");
    t.check(sftpCaps.can_checksum,
            "SFTP should advertise on-demand remote checksums");

    const auto scpCaps =
        openscp::capabilitiesForProtocol(openscp::Protocol::Scp);
    t.check(scpCaps.implemented, "SCP capabilities should be implemented");
    t.check(scpCaps.can_upload && scpCaps.can_download,
            "SCP capabilities should include file transfers");
    t.check(!scpCaps.can_list, "SCP capabilities should not include listing");
    t.check(!scpCaps.can_resume_download && !scpCaps.can_resume_upload,
            "SCP capabilities should not include resume");
    t.check(!scpCaps.can_set_permissions,
            "SCP capabilities should not include chmod/chown metadata edits");
    t.check(scpCaps.can_upload && scpCaps.can_download && !scpCaps.can_list,
            "SCP should advertise transfer-only fine-grained capabilities");
    t.check(!scpCaps.can_checksum, "SCP should not advertise remote checksums");
    t.check(scpCaps.supports_known_hosts,
            "SCP capabilities should include known_hosts verification");

    const auto webdavCaps =
        openscp::capabilitiesForProtocol(openscp::Protocol::WebDav);
#if OPENSCP_HAS_CURL_WEBDAV
    t.check(webdavCaps.implemented,
            "WebDAV capabilities should be implemented");
    t.check(webdavCaps.can_list, "WebDAV capabilities should include listing");
    t.check(webdavCaps.can_upload && webdavCaps.can_download,
            "WebDAV capabilities should include file transfers");
    t.check(webdavCaps.can_read_metadata,
            "WebDAV capabilities should include metadata");
    t.check(webdavCaps.supports_proxy,
            "WebDAV capabilities should include proxy support");
    t.check(webdavCaps.can_list && webdavCaps.can_upload &&
                webdavCaps.can_download && webdavCaps.can_stat &&
                webdavCaps.can_mkdir && webdavCaps.can_delete &&
                webdavCaps.can_rename,
            "WebDAV should advertise supported remote operations");
    t.check(!webdavCaps.can_checksum,
            "WebDAV should not advertise remote checksums");
#else
    t.check(!webdavCaps.implemented,
            "WebDAV capabilities should report not implemented");
    t.check(!webdavCaps.can_list,
            "WebDAV capabilities should not advertise listing when backend is "
            "disabled");
    t.check(!webdavCaps.can_upload && !webdavCaps.can_download,
            "WebDAV capabilities should not advertise transfers when backend "
            "is disabled");
#endif

    const auto ftpCaps =
        openscp::capabilitiesForProtocol(openscp::Protocol::Ftp);
    const auto ftpsCaps =
        openscp::capabilitiesForProtocol(openscp::Protocol::Ftps);
#if OPENSCP_HAS_CURL_FTP
    t.check(ftpCaps.implemented, "FTP capabilities should be implemented");
    t.check(ftpCaps.can_upload && ftpCaps.can_download,
            "FTP capabilities should include file transfers");
    t.check(ftpCaps.can_list,
            "FTP capabilities should include directory listing support");
    t.check(!ftpCaps.supports_known_hosts,
            "FTP should not advertise SSH known_hosts verification");
    t.check(ftpCaps.can_list && ftpCaps.can_upload && ftpCaps.can_download &&
                ftpCaps.can_stat && ftpCaps.can_mkdir && ftpCaps.can_delete &&
                ftpCaps.can_rename,
            "FTP should advertise implemented CRUD operations");
    t.check(!ftpCaps.can_checksum && !ftpsCaps.can_checksum,
            "FTP and FTPS should not advertise remote checksums");
    t.check(ftpsCaps.implemented, "FTPS capabilities should be implemented");
    t.check(ftpsCaps.can_upload && ftpsCaps.can_download,
            "FTPS capabilities should include file transfers");
    t.check(ftpsCaps.can_list,
            "FTPS capabilities should include directory listing support");
    t.check(!ftpsCaps.supports_known_hosts,
            "FTPS should use TLS certificates, not SSH known_hosts");
#else
    t.check(!ftpCaps.implemented,
            "FTP capabilities should report not implemented when backend is "
            "disabled");
    t.check(!ftpCaps.can_upload && !ftpCaps.can_download,
            "FTP capabilities should not advertise transfers when backend is "
            "disabled");
    t.check(!ftpCaps.can_list,
            "FTP capabilities should not advertise listing when backend is "
            "disabled");
    t.check(!ftpsCaps.implemented,
            "FTPS capabilities should report not implemented when backend is "
            "disabled");
    t.check(!ftpsCaps.can_upload && !ftpsCaps.can_download,
            "FTPS capabilities should not advertise transfers when backend is "
            "disabled");
    t.check(!ftpsCaps.can_list,
            "FTPS capabilities should not advertise listing when backend is "
            "disabled");
#endif
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

OPENSCP_TEST(test_list_sorting_and_known_path, t) {
    openscp::MockSftpClient c;
    std::string err;
    auto opt = validOptions();
    t.check(c.connect(opt, err), "connect should succeed before list test");

    std::vector<openscp::FileInfo> out;
    t.check(c.list("/home", out, err),
            "list('/home') should succeed in mock FS");
    t.check(out.size() == 3, "list('/home') should return 3 entries");
    if (out.size() == 3) {
        t.check(out[0].is_dir && out[0].name == "guest",
                "first entry should be dir 'guest'");
        t.check(out[1].is_dir && out[1].name == "luis",
                "second entry should be dir 'luis'");
        t.check(!out[2].is_dir && out[2].name == "notes.md",
                "third entry should be file 'notes.md'");
    }
}

OPENSCP_TEST(test_list_root_and_empty_path, t) {
    openscp::MockSftpClient c;
    std::string err;
    auto opt = validOptions();
    t.check(c.connect(opt, err),
            "connect should succeed before root listing test");

    std::vector<openscp::FileInfo> root;
    t.check(c.list("/", root, err), "list('/') should succeed");
    t.check(root.size() == 3, "list('/') should return expected mock entries");
    if (root.size() == 3) {
        t.check(root[0].is_dir && root[0].name == "home",
                "root[0] should be 'home' directory");
        t.check(root[1].is_dir && root[1].name == "var",
                "root[1] should be 'var' directory");
        t.check(!root[2].is_dir && root[2].name == "readme.txt",
                "root[2] should be 'readme.txt' file");
    }

    std::vector<openscp::FileInfo> emptyPath;
    err.clear();
    t.check(c.list("", emptyPath, err), "list('') should be treated as '/'");
    t.check(emptyPath.size() == root.size(),
            "list('') should match root entry count");
}

OPENSCP_TEST(test_missing_path_error, t) {
    openscp::MockSftpClient c;
    std::string err;
    auto opt = validOptions();
    t.check(c.connect(opt, err),
            "connect should succeed before missing path test");

    std::vector<openscp::FileInfo> out;
    err.clear();
    t.check(!c.list("/does-not-exist", out, err),
            "list on missing path should fail");
    t.check(!err.empty(), "missing path should report non-empty error");
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

OPENSCP_TEST(test_new_connection_like, t) {
    openscp::MockSftpClient c;
    auto opt = validOptions();
    std::string err;
    auto conn = c.newConnectionLike(opt, err);
    t.check(static_cast<bool>(conn),
            "newConnectionLike should return a client");
    t.check(conn && conn->isConnected(),
            "newConnectionLike client should be connected");

    t.check(conn && conn->mkdir("/shared", err),
            "worker connection should mutate its simulated server");
    std::vector<openscp::FileInfo> entries;
    t.check(c.connect(opt, err) && c.list("/", entries, err) &&
                std::any_of(entries.cbegin(), entries.cend(),
                            [](const openscp::FileInfo &entry) {
                                return entry.is_dir && entry.name == "shared";
                            }),
            "connections created alike should share simulated server state");
}

OPENSCP_TEST(test_new_connection_like_validation, t) {
    openscp::MockSftpClient c;
    openscp::SessionOptions bad;
    bad.host = "";
    bad.username = "alice";
    std::string err;
    auto conn = c.newConnectionLike(bad, err);
    t.check(!conn, "newConnectionLike should fail with invalid options");
    t.check(!err.empty(), "newConnectionLike should report validation errors");
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

} // namespace

int main() {
    openscp::test::TestHarness harness("core");
    return harness.run();
}
