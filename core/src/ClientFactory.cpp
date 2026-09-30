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
#if defined(OPENSCP_DEMO_MODE) && OPENSCP_DEMO_MODE
#include "mock/MockSftpClient.hpp"
#endif

namespace openscp {

std::unique_ptr<RemoteClient> CreateClientForProtocol(Protocol protocol) {
#if defined(OPENSCP_DEMO_MODE) && OPENSCP_DEMO_MODE
    // A demo build answers every protocol from the in-memory client: the point
    // is to exercise the interface with no server and no network at all.
    (void)protocol;
    return MockSftpClient::onDemoServer();
#else
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
#endif
}

std::unique_ptr<RemoteClient> CreateConnectedClient(const SessionOptions &opt,
                                                    std::string &err) {
    err.clear();
    if (opt.connection_stop_token.stop_requested()) {
        err = "Connection canceled by user";
        return nullptr;
    }
    auto client = CreateClientForProtocol(opt.protocol);
    if (!client) {
        err = std::string("Protocol not implemented: ") +
              protocolStorageName(opt.protocol);
        return nullptr;
    }
    // Destroy the registration before the client, including failure paths.
    std::stop_callback interruptConnection(
        opt.connection_stop_token,
        [pending = client.get()] { pending->interrupt(); });
    const bool connected = client->connect(opt, err);
    if (opt.connection_stop_token.stop_requested()) {
        err = "Connection canceled by user";
        return nullptr;
    }
    if (!connected)
        return nullptr;
    return client;
}

} // namespace openscp
