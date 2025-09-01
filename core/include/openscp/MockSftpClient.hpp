#pragma once
#include "SftpClient.hpp"
#include <unordered_map>

namespace openscp {

class MockSftpClient : public SftpClient {
public:
  bool connect(const SessionOptions& opt, std::string& err) override;
  void disconnect() override;
  bool isConnected() const override { return connected_; }

  bool list(const std::string& remote_path,
            std::vector<FileInfo>& out,
            std::string& err) override;

private:
  bool connected_ = false;
  SessionOptions lastOpt_{};

  // Mini “FS remoto” simulado
  // path -> vector de entradas
  std::unordered_map<std::string, std::vector<FileInfo>> fs_ = {
    { "/", {
        {"home", true, 0, 0},
        {"var",  true, 0, 0},
        {"readme.txt", false, 1280, 0},
      }
    },
    { "/home", {
        {"luis", true, 0, 0},
        {"guest", true, 0, 0},
        {"notes.md", false, 2048, 0},
      }
    },
    { "/home/luis", {
        {"proyectos", true, 0, 0},
        {"foto.jpg", false, 34567, 0},
      }
    },
    { "/var", {
        {"log", true, 0, 0},
      }
    },
  };
};

} // namespace openscp
