#pragma once

#include "openscp/Protocol.hpp"

#include <cstdio>
#include <string>

namespace openscp::localfiles {

enum class WriteMode {
    Truncate,
    Append,
};

// Opens a user-owned regular file without following a final-component symlink.
// The descriptor is opened with close-on-exec and restricted permissions.
std::FILE *openRegularFileForWrite(const std::string &path, WriteMode mode,
                                   std::string &error);

// Buffered always flushes userspace buffers. File also syncs file data and
// metadata; FileAndDirectory additionally makes the published directory entry
// durable in atomicReplace().
bool flushAndSync(
    std::FILE *file, std::string &error,
    LocalFileDurability durability = LocalFileDurability::FileAndDirectory);

// Atomically publishes a sibling temporary/partial file and durably syncs the
// parent directory where the platform supports it.
bool atomicReplace(
    const std::string &temporary, const std::string &destination,
    std::string &error,
    LocalFileDurability durability = LocalFileDurability::FileAndDirectory);

} // namespace openscp::localfiles
