#pragma once

#include <cstddef>
#include <string>
#include <vector>

namespace openscp::libssh2detail {

enum class KnownHostsReadStatus { Missing, Read, Unreadable };

/// Reads the known_hosts file at `path` into `lines`, without their line
/// endings. Loading them into libssh2 one at a time lets a line it cannot
/// parse be skipped on its own, as OpenSSH does:
/// libssh2_knownhost_readfile() stops at such a line and keeps only the lines
/// before it, so a host listed after one would look new instead of changed.
KnownHostsReadStatus readKnownHosts(const std::string &path,
                                    std::vector<std::string> &lines);

/// An OpenSSH known_hosts line for `hostToken` ("host" or "[host]:port") and
/// the raw host key `key`, with the name hashed as `ssh-keygen -H` does when
/// `hashName` is set. Empty when no random salt could be drawn.
std::string knownHostsLine(const std::string &hostToken,
                           const std::string &keyType, const unsigned char *key,
                           std::size_t keyLength, bool hashName);

/// The host key algorithms to ask the server for, most preferred first: the
/// usual order, with the ones whose key type known_hosts already records for
/// the host moved to the front. OpenSSH does the same, so a host known by one
/// key type does not look new because it also offers another.
std::string
hostKeyAlgorithmPreference(const std::vector<std::string> &recordedKeyTypes);

/// Appends `line` to the known_hosts file at `path`, which is created with
/// mode 0600, and its folder with 0700, when missing. The rest of the file,
/// its permissions, an existing folder's permissions and a symbolic link at
/// `path` stay as they are.
bool appendKnownHostsLine(const std::string &path, const std::string &line,
                          std::string &error);

} // namespace openscp::libssh2detail
