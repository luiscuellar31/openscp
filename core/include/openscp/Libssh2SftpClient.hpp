#pragma once
#include "SftpClient.hpp"
#include <string>
#include <vector>

// Forward a los tipos INTERNOS (con guion bajo)
struct _LIBSSH2_SESSION;
struct _LIBSSH2_SFTP;
struct _LIBSSH2_SFTP_HANDLE;

namespace openscp {

class Libssh2SftpClient : public SftpClient {
public:
  Libssh2SftpClient();
  ~Libssh2SftpClient() override;

  bool connect(const SessionOptions& opt, std::string& err) override;
  void disconnect() override;
  bool isConnected() const override { return connected_; }

  bool list(const std::string& remote_path,
            std::vector<FileInfo>& out,
            std::string& err) override;

private:
  bool connected_ = false;
  int  sock_ = -1;
  _LIBSSH2_SESSION* session_ = nullptr; // <- usa los tipos internos
  _LIBSSH2_SFTP*    sftp_    = nullptr; // <- idem

  bool tcpConnect(const std::string& host, uint16_t port, std::string& err);
  bool sshHandshakeAuth(const SessionOptions& opt, std::string& err);
};

} // namespace openscp