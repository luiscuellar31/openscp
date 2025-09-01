#include "openscp/MockSftpClient.hpp"
#include <algorithm>

namespace openscp {

bool MockSftpClient::connect(const SessionOptions& opt, std::string& err) {
  if (opt.host.empty() || opt.username.empty()) {
    err = "Host y usuario son obligatorios";
    return false;
  }
  connected_ = true;
  lastOpt_ = opt;
  return true;
}

void MockSftpClient::disconnect() {
  connected_ = false;
}

bool MockSftpClient::list(const std::string& remote_path,
                          std::vector<FileInfo>& out,
                          std::string& err) {
  if (!connected_) {
    err = "No conectado";
    return false;
  }
  std::string path = remote_path;
  if (path.empty()) path = "/";

  auto it = fs_.find(path);
  if (it == fs_.end()) {
    err = "Ruta remota no encontrada en mock: " + path;
    return false;
  }
  out = it->second;
  std::sort(out.begin(), out.end(), [](const FileInfo& a, const FileInfo& b){
    if (a.is_dir != b.is_dir) return a.is_dir > b.is_dir; // dirs primero
    return a.name < b.name;
  });
  return true;
}

} // namespace openscp
