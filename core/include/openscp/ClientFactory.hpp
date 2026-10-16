// Backend factory by protocol.
#pragma once
#include "RemoteClient.hpp"

namespace openscp {

std::unique_ptr<RemoteClient> CreateClientForProtocol(Protocol protocol);
std::unique_ptr<RemoteClient> CreateConnectedClient(const SessionOptions &opt,
                                                    std::string &err);

// Completes the TLS handshake with an FTPS or HTTPS WebDAV server, without
// verifying its certificate and without sending credentials, and returns the
// pin of its public key in the form tls_pinned_public_key takes.
bool FetchServerPublicKeyPin(const SessionOptions &opt, std::string &pin,
                             std::string &err);

} // namespace openscp
