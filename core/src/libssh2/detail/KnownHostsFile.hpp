#pragma once

#include <cstddef>
#include <functional>
#include <string>

namespace openscp::libssh2detail {

enum class KnownHostsReadStatus { Missing, Read, Unreadable };

struct KnownHostsReadResult {
    KnownHostsReadStatus status = KnownHostsReadStatus::Missing;
    // Lines `addLine` refused. As OpenSSH does, each is skipped on its own.
    int skippedLines = 0;
};

/// Reads the known_hosts file at `path` and hands each line, without its line
/// ending, to `addLine`, which returns false for a line it cannot parse.
/// libssh2_knownhost_readfile() instead stops at the first such line and keeps
/// only the lines before it, so a host listed after one would look new
/// instead of changed.
KnownHostsReadResult
readKnownHosts(const std::string &path,
               const std::function<bool(const std::string &line)> &addLine);
/// An OpenSSH known_hosts line for `hostToken` ("host" or "[host]:port") and
/// the raw host key `key`, with the name hashed as `ssh-keygen -H` does when
/// `hashName` is set. Empty when no random salt could be drawn.
std::string knownHostsLine(const std::string &hostToken,
                           const std::string &keyType, const unsigned char *key,
                           std::size_t keyLength, bool hashName);

/// Appends `line` to the known_hosts file at `path`, which is created with
/// mode 0600, and its folder with 0700, when missing. The rest of the file,
/// its permissions, an existing folder's permissions and a symbolic link at
/// `path` stay as they are.
bool appendKnownHostsLine(const std::string &path, const std::string &line,
                          std::string &error);

} // namespace openscp::libssh2detail
