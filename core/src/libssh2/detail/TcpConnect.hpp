#pragma once

#include <chrono>
#include <stop_token>
#include <string>

namespace openscp::libssh2detail {
// Waits for a nonblocking TCP connect, sharing one deadline across addresses.
bool waitForTcpConnection(int socket,
                          std::chrono::steady_clock::time_point deadline,
                          std::stop_token stopToken, std::string &error);
} // namespace openscp::libssh2detail
