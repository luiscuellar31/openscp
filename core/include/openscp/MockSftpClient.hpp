// Simulated SFTP client for UI testing without network.
#pragma once
#include "SftpClient.hpp"
#include <unordered_map>

namespace openscp {

class MockSftpClient : public SftpClient {
    public:
    bool connect(const SessionOptions &opt, std::string &err) override;
    void disconnect() override;
    bool isConnected() const override { return connected_; }

    bool list(const std::string &remote_path, std::vector<FileInfo> &out,
              std::string &err) override;

    // Methods not supported in the mock; return false with a message.
    bool get(const std::string &remote, const std::string &local,
             std::string &err,
             std::function<void(std::size_t, std::size_t)> progress,
             std::function<bool()> shouldCancel, bool resume) override {
        (void)remote;
        (void)local;
        (void)progress;
        (void)shouldCancel;
        // Keep message text as-is (UI/localization). Only comments translated.
        err = "Mock no soporta GET";
        return false;
    }

    bool put(const std::string &local, const std::string &remote,
             std::string &err,
             std::function<void(std::size_t, std::size_t)> progress,
             std::function<bool()> shouldCancel, bool resume) override {
        (void)local;
        (void)remote;
        (void)progress;
        (void)shouldCancel;
        err = "Mock no soporta PUT";
        return false;
    }

    bool exists(const std::string &remote_path, bool &isDir,
                std::string &err) override {
        isDir = false;
        (void)remote_path;
        err = "Mock no soporta exists";
        return false;
    }

    bool stat(const std::string &remote_path, FileInfo &info,
              std::string &err) override {
        (void)remote_path;
        (void)info;
        err = "Mock no soporta stat";
        return false;
    }

    bool mkdir(const std::string &remote_dir, std::string &err,
               unsigned int mode = 0755) override {
        (void)remote_dir;
        (void)mode;
        err = "Mock no soporta mkdir";
        return false;
    }

    bool removeFile(const std::string &remote_path, std::string &err) override {
        (void)remote_path;
        err = "Mock no soporta remove";
        return false;
    }

    bool removeDir(const std::string &remote_dir, std::string &err) override {
        (void)remote_dir;
        err = "Mock no soporta rmdir";
        return false;
    }

    bool rename(const std::string &from, const std::string &to,
                std::string &err, bool overwrite = false) override {
        (void)from;
        (void)to;
        (void)overwrite;
        err = "Mock no soporta rename";
        return false;
    }

    bool chmod(const std::string &remote_path, std::uint32_t mode,
               std::string &err) override {
        (void)remote_path;
        (void)mode;
        err = "Mock no soporta chmod";
        return false;
    }

    bool chown(const std::string &remote_path, std::uint32_t uid,
               std::uint32_t gid, std::string &err) override {
        (void)remote_path;
        (void)uid;
        (void)gid;
        err = "Mock no soporta chown";
        return false;
    }

    bool setTimes(const std::string &remote_path, std::uint64_t atime,
                  std::uint64_t mtime, std::string &err) override;

    std::unique_ptr<SftpClient> newConnectionLike(const SessionOptions &opt,
                                                  std::string &err) override;

    private:
    bool connected_ = false;
    SessionOptions lastOpt_{};

    // Mini simulated "remote FS": path -> list of entries
    std::unordered_map<std::string, std::vector<FileInfo>> fs_ = {
        {"/",
         {
             {"home", true, 0, false, 0, 0, 0, 0},
             {"var", true, 0, false, 0, 0, 0, 0},
             {"readme.txt", false, 1280, true, 0, 0, 0, 0},
         }},
        {"/home",
         {
             {"luis", true, 0, false, 0, 0, 0, 0},
             {"guest", true, 0, false, 0, 0, 0, 0},
             {"notes.md", false, 2048, true, 0, 0, 0, 0},
         }},
        {"/home/luis",
         {
             {"proyectos", true, 0, false, 0, 0, 0, 0},
             {"foto.jpg", false, 34567, true, 0, 0, 0, 0},
         }},
        {"/var",
         {
             {"log", true, 0, false, 0, 0, 0, 0},
         }},
    };
};

} // namespace openscp
