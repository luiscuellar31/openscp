// Backend factory by protocol.
#pragma once
#include "RemoteClient.hpp"

namespace openscp {

std::unique_ptr<RemoteClient> CreateClientForProtocol(Protocol protocol);
std::unique_ptr<RemoteClient> CreateConnectedClient(const SessionOptions &opt,
                                                    std::string &err);

// Protocol-neutral factory spellings for new callers. The original functions
// remain available and return the same aliased interface type.
inline std::unique_ptr<RemoteClient>
CreateRemoteClientForProtocol(Protocol protocol) {
    return CreateClientForProtocol(protocol);
}

inline std::unique_ptr<RemoteClient>
CreateConnectedRemoteClient(const SessionOptions &opt, std::string &err) {
    return CreateConnectedClient(opt, err);
}

} // namespace openscp
