// Connection, authentication, and transport settings for a remote session.
#pragma once

#include "KeyboardInteractive.hpp"
#include "Protocol.hpp"
#include "SecureString.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <stop_token>
#include <string>
#include <vector>

namespace openscp {

struct SessionOptions {
    // Runtime-only cancellation for this connection attempt; never persisted.
    std::stop_token connection_stop_token;
    Protocol protocol = Protocol::Sftp;
    ScpTransferMode scp_transfer_mode = ScpTransferMode::Auto;
    std::string host;
    std::uint16_t port = defaultPortForProtocol(Protocol::Sftp);
    std::string username;

    std::optional<SecureString> password;
    std::optional<std::string> private_key_path;
    std::optional<SecureString> private_key_passphrase;

    std::optional<std::string> known_hosts_path;
    KnownHostsPolicy known_hosts_policy = KnownHostsPolicy::Strict;
    bool known_hosts_hash_names = true;
    bool show_fp_hex = false;
    TransferIntegrityPolicy transfer_integrity_policy =
        TransferIntegrityPolicy::Optional;
    LocalFileDurability local_file_durability =
        LocalFileDurability::FileAndDirectory;

    FtpsMode ftps_mode = FtpsMode::Auto;
    bool ftps_verify_peer = true;
    std::optional<std::string> ftps_ca_cert_path;

    WebDavScheme webdav_scheme = WebDavScheme::Https;
    std::string webdav_base_path = "/";
    bool webdav_verify_peer = true;
    std::optional<std::string> webdav_ca_cert_path;

    // Pin of the server's TLS public key: "sha256//" and the Base64 SHA-256
    // of its SubjectPublicKeyInfo. FTPS and HTTPS WebDAV refuse any other
    // key, also when certificate verification is off.
    std::optional<std::string> tls_pinned_public_key;

    ProxyType proxy_type = ProxyType::None;
    std::string proxy_host;
    std::uint16_t proxy_port = 0;
    std::optional<std::string> proxy_username;
    std::optional<SecureString> proxy_password;

    std::optional<std::string> jump_host;
    std::uint16_t jump_port = 22;
    std::optional<std::string> jump_username;
    std::optional<std::string> jump_private_key_path;

    std::function<bool(const std::string &host, std::uint16_t port,
                       const std::string &algorithm,
                       const std::string &fingerprint, bool canSave)>
        hostkey_confirm_cb;
    std::function<void(const std::string &message)> hostkey_status_cb;
    KbdIntPromptsCB keyboard_interactive_cb;
};

// Whether the connection uses TLS without verifying the server certificate.
inline bool skipsTlsCertificateVerification(const SessionOptions &opt) {
    if (opt.protocol == Protocol::Ftps)
        return !opt.ftps_verify_peer;
    return opt.protocol == Protocol::WebDav &&
           normalizeWebDavScheme(opt.webdav_scheme) == WebDavScheme::Https &&
           !opt.webdav_verify_peer;
}

} // namespace openscp
