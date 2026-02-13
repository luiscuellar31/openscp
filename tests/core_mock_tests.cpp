// Core unit tests without external framework (run via CTest).
#include "openscp/MockSftpClient.hpp"

#include <cstdlib>
#include <iostream>
#include <optional>
#include <string>
#include <vector>

namespace {

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

void test_session_defaults(TestContext &t) {
    openscp::SessionOptions o;
    t.check(o.port == 22, "default port should be 22");
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

void test_set_times(TestContext &t) {
    openscp::MockSftpClient c;
    std::string err;
    const bool ok = c.setTimes("/home/luis/foto.jpg", 10, 20, err);
    t.check(ok, "setTimes should be supported by mock client");
    t.check(err.empty(), "setTimes should not set an error in mock client");
}

} // namespace

int main() {
    TestContext t;
    test_session_defaults(t);
    test_connect_validation(t);
    test_disconnect_changes_state(t);
    test_list_requires_connection(t);
    test_list_sorting_and_known_path(t);
    test_list_root_and_empty_path(t);
    test_missing_path_error(t);
    test_unsupported_methods_report_error(t);
    test_new_connection_like(t);
    test_new_connection_like_validation(t);
    test_set_times(t);

    if (t.failures != 0) {
        std::cerr << "[FAILURES] " << t.failures << "\n";
        return EXIT_FAILURE;
    }
    std::cout << "[OK] openscp_core_tests\n";
    return EXIT_SUCCESS;
}
