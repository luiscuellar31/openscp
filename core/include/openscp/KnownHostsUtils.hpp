// known_hosts helper utilities shared by core and UI layers.
#pragma once

#include <cstdint>
#include <string>

namespace openscp {

// Remove a known_hosts entry for host:port and rewrite the file atomically.
// Returns true when the operation completes without fatal errors.
bool RemoveKnownHostEntry(const std::string &khPath, const std::string &host,
                          std::uint16_t port, std::string &err);

} // namespace openscp
