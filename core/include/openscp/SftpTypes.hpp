#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstdint>

namespace openscp {

struct FileInfo {
  std::string name;
  bool        is_dir = false;
  std::uint64_t size  = 0;
  std::uint64_t mtime = 0; // epoch segundos (aprox)
};

struct SessionOptions {
  std::string host;
  std::uint16_t port = 22;
  std::string username;

  std::optional<std::string> password;
  std::optional<std::string> private_key_path;
  std::optional<std::string> private_key_passphrase;
};

} // namespace openscp
