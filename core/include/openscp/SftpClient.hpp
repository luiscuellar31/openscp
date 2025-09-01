#pragma once
#include "SftpTypes.hpp"
#include <functional>

namespace openscp {

class SftpClient {
public:
  using ProgressCB = std::function<void(double)>; // 0..1

  virtual ~SftpClient() = default;

  virtual bool connect(const SessionOptions& opt, std::string& err) = 0;
  virtual void disconnect() = 0;
  virtual bool isConnected() const = 0;

  virtual bool list(const std::string& remote_path,
                    std::vector<FileInfo>& out,
                    std::string& err) = 0;

  // Futuro: los implementaremos cuando metamos libssh2
  virtual bool get(const std::string& remote_file,
                   const std::string& local_file,
                   ProgressCB /*progress*/,
                   std::string& err) { err = "Not implemented"; return false; }

  virtual bool put(const std::string& local_file,
                   const std::string& remote_dir,
                   ProgressCB /*progress*/,
                   std::string& err) { err = "Not implemented"; return false; }
};

} // namespace openscp
