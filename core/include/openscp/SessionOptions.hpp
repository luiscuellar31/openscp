// Connection, authentication, and transport settings for a remote session.
#pragma once

#include "Protocol.hpp"

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

namespace openscp {

enum class KbdIntPromptResult { Handled, Unhandled, Cancelled };

using KbdIntPromptsCB = std::function<KbdIntPromptResult(
    const std::string &name, const std::string &instruction,
    const std::vector<std::string> &prompts,
    std::vector<std::string> &responses)>;

struct SessionOptions {
    Protocol protocol = Protocol::Sftp;
    ScpTransferMode scp_transfer_mode = ScpTransferMode::Auto;
    std::string host;
    std::uint16_t port = defaultPortForProtocol(Protocol::Sftp);
    std::string username;

    std::optional<std::string> password;
    std::optional<std::string> private_key_path;
    std::optional<std::string> private_key_passphrase;

    std::optional<std::string> known_hosts_path;
    KnownHostsPolicy known_hosts_policy = KnownHostsPolicy::Strict;
    bool known_hosts_hash_names = true;
    bool show_fp_hex = false;
    TransferIntegrityPolicy transfer_integrity_policy =
        TransferIntegrityPolicy::Optional;

    FtpsMode ftps_mode = FtpsMode::Auto;
    bool ftps_verify_peer = true;
    std::optional<std::string> ftps_ca_cert_path;

    WebDavScheme webdav_scheme = WebDavScheme::Https;
    std::string webdav_base_path = "/";
    bool webdav_verify_peer = true;
    std::optional<std::string> webdav_ca_cert_path;

    ProxyType proxy_type = ProxyType::None;
    std::string proxy_host;
    std::uint16_t proxy_port = 0;
    std::optional<std::string> proxy_username;
    std::optional<std::string> proxy_password;

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

} // namespace openscp
