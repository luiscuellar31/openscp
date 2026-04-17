// Protocol-aware backend factory.
#include "openscp/ClientFactory.hpp"
#if defined(OPENSCP_HAS_CURL_FTP) && OPENSCP_HAS_CURL_FTP
#include "openscp/CurlFtpClient.hpp"
#endif
#if defined(OPENSCP_HAS_CURL_WEBDAV) && OPENSCP_HAS_CURL_WEBDAV
#include "openscp/CurlWebDavClient.hpp"
#endif
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
#if defined(OPENSCP_HAS_CURL_FTP) && OPENSCP_HAS_CURL_FTP
        return std::make_unique<CurlFtpClient>(Protocol::Ftp);
#else
        return nullptr;
#endif
    case Protocol::Ftps:
#if defined(OPENSCP_HAS_CURL_FTP) && OPENSCP_HAS_CURL_FTP
        return std::make_unique<CurlFtpClient>(Protocol::Ftps);
#else
        return nullptr;
#endif
    case Protocol::WebDav:
#if defined(OPENSCP_HAS_CURL_WEBDAV) && OPENSCP_HAS_CURL_WEBDAV
        return std::make_unique<CurlWebDavClient>();
#else
        return nullptr;
#endif
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
