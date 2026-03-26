// Protocol-aware backend factory.
#include "openscp/ClientFactory.hpp"
#include "openscp/Libssh2ScpClient.hpp"
#include "openscp/Libssh2SftpClient.hpp"

namespace openscp {

std::unique_ptr<SftpClient> CreateClientForProtocol(Protocol protocol) {
    switch (protocol) {
    case Protocol::Sftp:
        return std::make_unique<Libssh2SftpClient>();
    case Protocol::Scp:
        return std::make_unique<Libssh2ScpClient>();
    case Protocol::Ftp:
    case Protocol::Ftps:
    case Protocol::WebDav:
        return nullptr;
    }
    return nullptr;
}

std::unique_ptr<SftpClient> CreateConnectedClient(const SessionOptions &opt,
                                                  std::string &err) {
    err.clear();
    auto client = CreateClientForProtocol(opt.protocol);
    if (!client) {
        err = std::string("Protocol not implemented: ") +
              protocolStorageName(opt.protocol);
        return nullptr;
    }
    if (!client->connect(opt, err))
        return nullptr;
    return client;
}

} // namespace openscp
