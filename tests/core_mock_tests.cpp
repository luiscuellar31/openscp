// Core unit tests without external framework (run via CTest).
#include "Libssh2ErrorClassifier.hpp"
#include "Libssh2InputSafety.hpp"
#include "SafeLocalFile.hpp"
#include "TestHarness.hpp"
#include "openscp/ClientFactory.hpp"
#include "openscp/Libssh2ScpClient.hpp"
#include "openscp/Libssh2SftpClient.hpp"
#include "openscp/MockSftpClient.hpp"
#include "openscp/SecureString.hpp"
#include "openscp/UniqueFile.hpp"
#if OPENSCP_HAS_CURL_FTP
#include "openscp/CurlFtpClient.hpp"
#endif
#if OPENSCP_HAS_CURL_WEBDAV
#include "openscp/CurlWebDavClient.hpp"
#endif
#if OPENSCP_HAS_CURL_FTP || OPENSCP_HAS_CURL_WEBDAV
#include "../core/src/curl/CurlBackendCommon.hpp"
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

void test_session_defaults(TestContext &t) {
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
            "default FTPS mode should preserve legacy auto behavior");
    t.check((std::is_same_v<openscp::RemoteClient, openscp::SftpClient>),
            "RemoteClient should remain source-compatible with SftpClient");
    t.check((std::is_same_v<decltype(o.password),
                            std::optional<openscp::SecureString>>),
            "session passwords should use SecureString storage");
}

void test_secure_string_value_semantics(TestContext &t) {
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

void test_libssh2_input_safety(TestContext &t) {
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

void test_safe_local_partial_files(TestContext &t) {
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

void test_protocol_helpers(TestContext &t) {
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
    t.check(sftpCaps.supports_listing,
            "SFTP capabilities should include listing");
    t.check(sftpCaps.can_upload && sftpCaps.can_download && sftpCaps.can_stat &&
                sftpCaps.can_mkdir && sftpCaps.can_delete &&
                sftpCaps.can_rename,
            "SFTP should advertise fine-grained remote operations");
    t.check(sftpCaps.can_checksum,
            "SFTP should advertise on-demand remote checksums");

    const auto scpCaps =
        openscp::capabilitiesForProtocol(openscp::Protocol::Scp);
    t.check(scpCaps.implemented, "SCP capabilities should be implemented");
    t.check(scpCaps.supports_file_transfers,
            "SCP capabilities should include file transfers");
    t.check(!scpCaps.supports_listing,
            "SCP capabilities should not include listing");
    t.check(!scpCaps.supports_resume,
            "SCP capabilities should not include resume");
    t.check(!scpCaps.supports_permissions,
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
    t.check(webdavCaps.supports_listing,
            "WebDAV capabilities should include listing");
    t.check(webdavCaps.supports_file_transfers,
            "WebDAV capabilities should include file transfers");
    t.check(webdavCaps.supports_metadata,
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
    t.check(!webdavCaps.supports_listing,
            "WebDAV capabilities should not advertise listing when backend is "
            "disabled");
    t.check(!webdavCaps.supports_file_transfers,
            "WebDAV capabilities should not advertise transfers when backend "
            "is disabled");
#endif

    const auto ftpCaps =
        openscp::capabilitiesForProtocol(openscp::Protocol::Ftp);
    const auto ftpsCaps =
        openscp::capabilitiesForProtocol(openscp::Protocol::Ftps);
#if OPENSCP_HAS_CURL_FTP
    t.check(ftpCaps.implemented, "FTP capabilities should be implemented");
    t.check(ftpCaps.supports_file_transfers,
            "FTP capabilities should include file transfers");
    t.check(ftpCaps.supports_listing,
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
    t.check(ftpsCaps.supports_file_transfers,
            "FTPS capabilities should include file transfers");
    t.check(ftpsCaps.supports_listing,
            "FTPS capabilities should include directory listing support");
    t.check(!ftpsCaps.supports_known_hosts,
            "FTPS should use TLS certificates, not SSH known_hosts");
#else
    t.check(!ftpCaps.implemented,
            "FTP capabilities should report not implemented when backend is "
            "disabled");
    t.check(!ftpCaps.supports_file_transfers,
            "FTP capabilities should not advertise transfers when backend is "
            "disabled");
    t.check(!ftpCaps.supports_listing,
            "FTP capabilities should not advertise listing when backend is "
            "disabled");
    t.check(!ftpsCaps.implemented,
            "FTPS capabilities should report not implemented when backend is "
            "disabled");
    t.check(!ftpsCaps.supports_file_transfers,
            "FTPS capabilities should not advertise transfers when backend is "
            "disabled");
    t.check(!ftpsCaps.supports_listing,
            "FTPS capabilities should not advertise listing when backend is "
            "disabled");
#endif
}

#if OPENSCP_HAS_CURL_FTP
void test_curlftp_rejects_unsupported_proxy_type(TestContext &t) {
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

void test_curlftp_rejects_command_injection_paths(TestContext &t) {
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
void test_curl_structured_error_mappings(TestContext &t) {
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
void test_curlwebdav_rejects_control_characters(TestContext &t) {
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

void test_connect_validation(TestContext &t) {
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

void test_disconnect_changes_state(TestContext &t) {
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

void test_list_requires_connection(TestContext &t) {
    openscp::MockSftpClient c;
    std::vector<openscp::FileInfo> out;
    std::string err;
    t.check(!c.list("/", out, err), "list should fail when disconnected");
    t.check(!err.empty(), "list should provide error when disconnected");
}

void test_list_sorting_and_known_path(TestContext &t) {
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

void test_list_root_and_empty_path(TestContext &t) {
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

void test_missing_path_error(TestContext &t) {
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

void test_unsupported_methods_report_error(TestContext &t) {
    openscp::MockSftpClient c;
    std::string err;
    bool isDir = true;
    openscp::FileInfo info;

    const bool ex = c.exists("/x", isDir, err);
    t.check(!ex, "exists should be unsupported in mock");
    t.checkContains(err, "Mock no soporta",
                    "exists should expose unsupported message");
    t.check(!isDir, "exists should reset isDir to false in mock");

    err.clear();
    t.check(!c.stat("/x", info, err), "stat should be unsupported in mock");
    t.checkContains(err, "Mock no soporta",
                    "stat should expose unsupported message");

    err.clear();
    t.check(!c.mkdir("/x", err), "mkdir should be unsupported in mock");
    t.checkContains(err, "Mock no soporta",
                    "mkdir should expose unsupported message");

    err.clear();
    t.check(!c.removeFile("/x", err),
            "removeFile should be unsupported in mock");
    t.checkContains(err, "Mock no soporta",
                    "removeFile should expose unsupported message");

    err.clear();
    t.check(!c.removeDir("/x", err), "removeDir should be unsupported in mock");
    t.checkContains(err, "Mock no soporta",
                    "removeDir should expose unsupported message");

    err.clear();
    t.check(!c.rename("/a", "/b", err, true),
            "rename should be unsupported in mock");
    t.checkContains(err, "Mock no soporta",
                    "rename should expose unsupported message");

    err.clear();
    t.check(!c.chmod("/x", 0644, err), "chmod should be unsupported in mock");
    t.checkContains(err, "Mock no soporta",
                    "chmod should expose unsupported message");

    err.clear();
    t.check(!c.chown("/x", 1000, 1000, err),
            "chown should be unsupported in mock");
    t.checkContains(err, "Mock no soporta",
                    "chown should expose unsupported message");

    err.clear();
    t.check(!c.get("/remote", "/local", err, {}, {}, false),
            "get should be unsupported in mock");
    t.checkContains(err, "Mock no soporta",
                    "get should expose unsupported message");

    err.clear();
    t.check(!c.put("/local", "/remote", err, {}, {}, false),
            "put should be unsupported in mock");
    t.checkContains(err, "Mock no soporta",
                    "put should expose unsupported message");

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

void test_new_connection_like(TestContext &t) {
    openscp::MockSftpClient c;
    auto opt = validOptions();
    std::string err;
    auto conn = c.newConnectionLike(opt, err);
    t.check(static_cast<bool>(conn),
            "newConnectionLike should return a client");
    t.check(conn && conn->isConnected(),
            "newConnectionLike client should be connected");
}

void test_new_connection_like_validation(TestContext &t) {
    openscp::MockSftpClient c;
    openscp::SessionOptions bad;
    bad.host = "";
    bad.username = "alice";
    std::string err;
    auto conn = c.newConnectionLike(bad, err);
    t.check(!conn, "newConnectionLike should fail with invalid options");
    t.check(!err.empty(), "newConnectionLike should report validation errors");
}

void test_client_factory(TestContext &t) {
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

void test_set_times(TestContext &t) {
    openscp::MockSftpClient c;
    std::string err;
    const bool ok = c.setTimes("/home/luis/foto.jpg", 10, 20, err);
    t.check(ok, "setTimes should be supported by mock client");
    t.check(err.empty(), "setTimes should not set an error in mock client");
}

void test_libssh2_rejects_conflicting_proxy_and_jump(TestContext &t) {
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

void test_libssh2_backends_expose_structured_errors(TestContext &t) {
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

void test_shared_libssh2_error_classification(TestContext &t) {
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
void test_libssh2_rejects_jump_on_windows(TestContext &t) {
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

void test_remove_known_hosts_entry_plain_and_hashed(TestContext &t) {
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

void test_remove_known_hosts_entry_non_default_port(TestContext &t) {
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
    harness.add("core behavior", [](TestContext &test) {
        test_session_defaults(test);
        test_secure_string_value_semantics(test);
        test_libssh2_input_safety(test);
        test_safe_local_partial_files(test);
        test_protocol_helpers(test);
        test_connect_validation(test);
        test_disconnect_changes_state(test);
        test_list_requires_connection(test);
        test_list_sorting_and_known_path(test);
        test_list_root_and_empty_path(test);
        test_missing_path_error(test);
        test_unsupported_methods_report_error(test);
        test_new_connection_like(test);
        test_new_connection_like_validation(test);
        test_client_factory(test);
        test_set_times(test);
        test_libssh2_rejects_conflicting_proxy_and_jump(test);
        test_libssh2_backends_expose_structured_errors(test);
        test_shared_libssh2_error_classification(test);
#if OPENSCP_HAS_CURL_FTP
        test_curlftp_rejects_unsupported_proxy_type(test);
        test_curlftp_rejects_command_injection_paths(test);
#endif
#if OPENSCP_HAS_CURL_FTP || OPENSCP_HAS_CURL_WEBDAV
        test_curl_structured_error_mappings(test);
#endif
#if OPENSCP_HAS_CURL_WEBDAV
        test_curlwebdav_rejects_control_characters(test);
#endif
#ifdef _WIN32
        test_libssh2_rejects_jump_on_windows(test);
#endif
        test_remove_known_hosts_entry_plain_and_hashed(test);
        test_remove_known_hosts_entry_non_default_port(test);
    });
    return harness.run();
}
