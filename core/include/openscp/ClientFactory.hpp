// Backend factory by protocol.
#pragma once
#include "RemoteClient.hpp"

namespace openscp {

std::unique_ptr<RemoteClient> CreateClientForProtocol(Protocol protocol);
std::unique_ptr<RemoteClient> CreateConnectedClient(const SessionOptions &opt,
                                                    std::string &err);

} // namespace openscp
