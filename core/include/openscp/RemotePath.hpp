#pragma once

#include <string>
#include <string_view>

namespace openscp {

// Returns an absolute, POSIX-like path confined to the logical remote root.
// Empty segments and "." are removed; ".." can never traverse above "/".
[[nodiscard]] std::string normalizeRemotePath(std::string_view rawPath);

} // namespace openscp
