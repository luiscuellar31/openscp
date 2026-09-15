// Protocol-neutral remote filesystem metadata.
#pragma once

#include <cstdint>
#include <string>

namespace openscp {

struct FileInfo {
    std::string name;
    bool is_dir = false;
    std::uint64_t size = 0;
    bool has_size = false;
    std::uint64_t mtime = 0;
    std::uint32_t mode = 0;
    std::uint32_t uid = 0;
    std::uint32_t gid = 0;
};

} // namespace openscp
