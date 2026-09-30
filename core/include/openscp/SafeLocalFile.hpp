#pragma once

#include "openscp/Protocol.hpp"

#include <cstdint>
#include <cstdio>
#include <string>

namespace openscp::localfiles {

enum class WriteMode {
    Truncate,
    Append,
};

struct LocalFileIdentity {
    std::uint64_t device = 0;
    std::uint64_t inode = 0;
    std::uint64_t size = 0;
    std::int64_t modifiedSeconds = 0;
    std::int64_t modifiedNanoseconds = 0;
    std::int64_t changedSeconds = 0;
    std::int64_t changedNanoseconds = 0;

    bool operator==(const LocalFileIdentity &) const = default;
};

// On supported POSIX platforms, local path operations reject symlinks below
// user-writable directories. Each parent component is opened through a
// directory descriptor, so replacing a parent with a symlink cannot redirect
// a later operation.
bool ensureLocalDirectories(const std::string &path, std::string &error);
bool removeLocalPath(const std::string &path, bool directory,
                     std::string &error);
bool localFileIdentity(const std::string &path, LocalFileIdentity &identity,
                       std::string &error);
// Checks a move source without unlinking an existing entry. POSIX cannot
// atomically compare identity and unlink, so unchanged sources fail with
// ENOTSUP for manual cleanup; already absent sources succeed.
bool removeLocalFileIfUnchanged(const std::string &path,
                                const LocalFileIdentity &identity,
                                std::string &error);
bool setLocalModificationTime(const std::string &path,
                              std::int64_t modifiedSeconds, std::string &error);

// Opens a user-owned regular file without following a final-component symlink.
// The descriptor is opened with close-on-exec and restricted permissions.
// On POSIX, opening does not wait for a FIFO reader. The opened descriptor is
// validated before modification, and returned regular-file streams block.
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
