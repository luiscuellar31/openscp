// Backend factory by protocol.
#pragma once
#include "SftpClient.hpp"

namespace openscp {

std::unique_ptr<SftpClient> CreateClientForProtocol(Protocol protocol);
std::unique_ptr<SftpClient> CreateConnectedClient(const SessionOptions &opt,
                                                  std::string &err);

} // namespace openscp
