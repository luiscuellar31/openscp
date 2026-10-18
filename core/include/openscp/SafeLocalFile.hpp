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
    // The upper half of a 128-bit Windows file ID, as ReFS uses; 0 elsewhere.
    std::uint64_t inodeHigh = 0;

    bool operator==(const LocalFileIdentity &) const = default;
};

// Paths are UTF-8. Local path operations reject symlinks below user-writable
// directories. Each parent component is opened through a directory
// descriptor, so replacing a parent with a symlink cannot redirect a later
// operation. On Windows the same holds for junctions, symbolic links and
// mount points, and every component must also be a valid Windows name (see
// localNameProblem()).
bool ensureLocalDirectories(const std::string &path, std::string &error);
bool removeLocalPath(const std::string &path, bool directory,
                     std::string &error);
bool localFileIdentity(const std::string &path, LocalFileIdentity &identity,
                       std::string &error);
// Removes a move source only if it still has the given identity; already
// absent sources succeed. POSIX cannot atomically compare identity and
// unlink, so there unchanged sources fail with ENOTSUP for manual cleanup.
// Windows holds the file open against changes while it compares and deletes.
bool removeLocalFileIfUnchanged(const std::string &path,
                                const LocalFileIdentity &identity,
                                std::string &error);
bool setLocalModificationTime(const std::string &path,
                              std::int64_t modifiedSeconds, std::string &error);

// Why `name` cannot be the name of a local file or folder, or an empty string
// when it can. POSIX refuses only "", ".", "..", "/" and NUL. Windows also
// refuses the characters <>:"\|?* and control characters, a final dot or
// space, reserved device names such as CON, NUL or COM1 (with or without an
// extension), names longer than 255 UTF-16 units, and names with the form of
// the short names it makes for long ones, such as REPORT~1.TXT, which can
// open another entry. Local paths may still pass through short names.
std::string localNameProblem(const std::string &name);

// The name the existing entry at `path` has on disk, or an empty string when
// nothing is there. Windows and macOS volumes usually ignore case, so
// "makefile" finds an entry stored as "Makefile"; this returns "Makefile".
// Linux volumes distinguish case, so there the name is the one in `path`.
std::string localEntryName(const std::string &path);

// Opens a user-owned regular file without following a final-component symlink.
// The descriptor is opened with close-on-exec and restricted permissions.
// On POSIX, opening does not wait for a FIFO reader. The opened descriptor is
// validated before modification, and returned regular-file streams block.
// Windows refuses any reparse point and any device, and other users' files
// only on local volumes, which record owners reliably.
std::FILE *openRegularFileForWrite(const std::string &path, WriteMode mode,
                                   std::string &error);

// The permission bits (mode & 0777) of an open local file, which uploads give
// the remote copy as OpenSSH's sftp and scp do. Windows has no such bits, so
// it, and a file that cannot be inspected, get 0644.
std::uint32_t uploadPermissions(std::FILE *file);

// Buffered always flushes userspace buffers. File also syncs file data and
// metadata; FileAndDirectory additionally makes the published directory entry
// durable in atomicReplace().
bool flushAndSync(
    std::FILE *file, std::string &error,
    LocalFileDurability durability = LocalFileDurability::FileAndDirectory);

// Atomically publishes a sibling temporary/partial file and durably syncs the
// parent directory where the platform supports it. Windows has no directory
// sync; it retries for a moment while another program, such as an antivirus
// scanner, holds the destination open.
bool atomicReplace(
    const std::string &temporary, const std::string &destination,
    std::string &error,
    LocalFileDurability durability = LocalFileDurability::FileAndDirectory);

} // namespace openscp::localfiles
