// Core unit tests without external framework (run via CTest).
#include "openscp/ClientFactory.hpp"
#include "openscp/Libssh2SftpClient.hpp"
#include "openscp/MockSftpClient.hpp"

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <string>
#include <vector>

namespace {

namespace fs = std::filesystem;

struct TestContext {
    int failures = 0;

    void check(bool cond, const std::string &msg) {
        if (!cond) {
            ++failures;
            std::cerr << "[FAIL] " << msg << "\n";
        }
    }

    void checkContains(const std::string &haystack, const std::string &needle,
                       const std::string &msg) {
        check(haystack.find(needle) != std::string::npos, msg);
    }
};

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
}

void test_protocol_helpers(TestContext &t) {
    t.check(openscp::protocolFromStorageName("sftp") ==
                openscp::Protocol::Sftp,
            "protocolFromStorageName should parse sftp");
    t.check(openscp::protocolFromStorageName("SCP") == openscp::Protocol::Scp,
            "protocolFromStorageName should parse scp case-insensitively");
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
    t.check(
        std::string(openscp::scpTransferModeStorageName(
            openscp::ScpTransferMode::ScpOnly)) == "scp-only",
        "scpTransferModeStorageName should serialize scp-only");

    const auto sftpCaps =
        openscp::capabilitiesForProtocol(openscp::Protocol::Sftp);
    t.check(sftpCaps.implemented, "SFTP capabilities should be implemented");
    t.check(sftpCaps.supports_listing,
            "SFTP capabilities should include listing");

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
    t.check(scpCaps.supports_known_hosts,
            "SCP capabilities should include known_hosts verification");

    const auto webdavCaps =
        openscp::capabilitiesForProtocol(openscp::Protocol::WebDav);
    t.check(!webdavCaps.implemented,
            "WebDAV capabilities should report not implemented");

    const auto ftpCaps = openscp::capabilitiesForProtocol(openscp::Protocol::Ftp);
    t.check(ftpCaps.implemented, "FTP capabilities should be implemented");
    t.check(ftpCaps.supports_file_transfers,
            "FTP capabilities should include file transfers");
    t.check(!ftpCaps.supports_listing,
            "FTP capabilities should currently run in transfer-only mode");
}

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
    t.check(static_cast<bool>(ftp),
            "factory should create FTP backend instance");
    if (ftp) {
        t.check(ftp->protocol() == openscp::Protocol::Ftp,
                "FTP backend should report FTP protocol");
    }
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
    t.checkContains(
        err, "Proxy and SSH jump host cannot be used together",
        "connect should explain proxy/jump mutual exclusion");
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
    const bool ok =
        openscp::RemoveKnownHostEntry(khPath.string(), "example.com", 2222,
                                      err);
    t.check(ok, std::string("port-specific removal should succeed: ") + err);

    std::string content;
    t.check(readTextFile(khPath, content),
            "updated known_hosts port fixture should be readable");
    t.check(content.find("[example.com]:2222 ssh-ed25519") ==
                std::string::npos,
            "port-specific known_hosts entry should be removed");
    t.check(content.find("example.com ssh-ed25519") != std::string::npos,
            "default-port known_hosts entry should remain");

    std::error_code ec;
    fs::remove(khPath, ec);
    fs::remove_all(khPath.parent_path(), ec);
}

} // namespace

int main() {
    TestContext t;
    test_session_defaults(t);
    test_protocol_helpers(t);
    test_connect_validation(t);
    test_disconnect_changes_state(t);
    test_list_requires_connection(t);
    test_list_sorting_and_known_path(t);
    test_list_root_and_empty_path(t);
    test_missing_path_error(t);
    test_unsupported_methods_report_error(t);
    test_new_connection_like(t);
    test_new_connection_like_validation(t);
    test_client_factory(t);
    test_set_times(t);
    test_libssh2_rejects_conflicting_proxy_and_jump(t);
#ifdef _WIN32
    test_libssh2_rejects_jump_on_windows(t);
#endif
    test_remove_known_hosts_entry_plain_and_hashed(t);
    test_remove_known_hosts_entry_non_default_port(t);

    if (t.failures != 0) {
        std::cerr << "[FAILURES] " << t.failures << "\n";
        return EXIT_FAILURE;
    }
    std::cout << "[OK] openscp_core_tests\n";
    return EXIT_SUCCESS;
}
