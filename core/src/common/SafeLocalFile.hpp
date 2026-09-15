#pragma once

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

bool flushAndSync(std::FILE *file, std::string &error);

// Atomically publishes a sibling temporary/partial file and durably syncs the
// parent directory where the platform supports it.
bool atomicReplace(const std::string &temporary, const std::string &destination,
                   std::string &error);

} // namespace openscp::localfiles
