// Protocol-aware backend factory.
#include "openscp/ClientFactory.hpp"
#if defined(OPENSCP_HAS_CURL_FTP) && OPENSCP_HAS_CURL_FTP
#include "curl/CurlFtpClient.hpp"
#endif
#if defined(OPENSCP_HAS_CURL_WEBDAV) && OPENSCP_HAS_CURL_WEBDAV
#include "curl/CurlWebDavClient.hpp"
#endif
#include "libssh2/Libssh2ScpClient.hpp"
#include "libssh2/Libssh2SftpClient.hpp"

namespace openscp {

std::unique_ptr<RemoteClient> CreateClientForProtocol(Protocol protocol) {
    if (protocol == Protocol::Sftp)
        return std::make_unique<Libssh2SftpClient>();
    if (protocol == Protocol::Scp)
        return std::make_unique<Libssh2ScpClient>();
#if defined(OPENSCP_HAS_CURL_FTP) && OPENSCP_HAS_CURL_FTP
    if (protocol == Protocol::Ftp || protocol == Protocol::Ftps)
        return std::make_unique<CurlFtpClient>(protocol);
#endif
#if defined(OPENSCP_HAS_CURL_WEBDAV) && OPENSCP_HAS_CURL_WEBDAV
    if (protocol == Protocol::WebDav)
        return std::make_unique<CurlWebDavClient>();
#endif
    return nullptr;
}

std::unique_ptr<RemoteClient> CreateConnectedClient(const SessionOptions &opt,
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
