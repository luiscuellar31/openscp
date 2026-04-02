// Basic types shared between UI and core for SFTP sessions and metadata.
// Keeping these structures simple and serializable makes them easy to use in
// the UI.
#pragma once
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#ifndef OPENSCP_HAS_CURL_FTP
#define OPENSCP_HAS_CURL_FTP 1
#endif

namespace openscp {

// known_hosts validation policy for the server host key.
enum class KnownHostsPolicy {
    Strict,    // Requires exact match with known_hosts.
    AcceptNew, // TOFU: accept and save new hosts; reject key changes.
    Off        // No verification (not recommended).
};

// Transfer integrity policy.
// - Off: no hash verification.
// - Optional: verify when possible; fail on detected mismatch.
// - Required: verification is mandatory; fail if verification cannot be
// completed.
enum class TransferIntegrityPolicy { Off, Optional, Required };
enum class ProxyType { None, Socks5, HttpConnect };
enum class Protocol { Sftp, Scp, Ftp, Ftps, WebDav };
enum class ScpTransferMode {
    Auto,    // Try native SCP first and fallback to SFTP transfers if needed.
    ScpOnly, // Enforce classic SCP transfers only (no SFTP fallback).
};

inline constexpr bool isValidProxyType(ProxyType type) {
    switch (type) {
    case ProxyType::None:
    case ProxyType::Socks5:
    case ProxyType::HttpConnect:
        return true;
    }
    return false;
}

inline constexpr ProxyType normalizeProxyType(ProxyType type) {
    return isValidProxyType(type) ? type : ProxyType::None;
}

inline ProxyType proxyTypeFromStorageValue(int raw) {
    const auto candidate = static_cast<ProxyType>(raw);
    return normalizeProxyType(candidate);
}

inline constexpr std::uint16_t defaultPortForProxyType(ProxyType type) {
    switch (type) {
    case ProxyType::Socks5:
        return 1080;
    case ProxyType::HttpConnect:
        return 8080;
    case ProxyType::None:
        break;
    }
    return 0;
}

struct ProtocolCapabilities {
    bool implemented = false;
    bool supports_listing = false;
    bool supports_file_transfers = false;
    bool supports_resume = false;
    bool supports_metadata = false;
    bool supports_permissions = false;
    bool supports_ownership = false;
    bool supports_timestamps = false;
    bool supports_proxy = false;
    bool supports_jump_host = false;
    bool supports_known_hosts = false;
    bool supports_transfer_integrity = false;
};

inline constexpr std::uint16_t defaultPortForProtocol(Protocol protocol) {
    switch (protocol) {
    case Protocol::Sftp:
    case Protocol::Scp:
        return 22;
    case Protocol::Ftp:
        return 21;
    case Protocol::Ftps:
        return 990;
    case Protocol::WebDav:
        return 443;
    }
    return 22;
}

inline constexpr const char *protocolStorageName(Protocol protocol) {
    switch (protocol) {
    case Protocol::Sftp:
        return "sftp";
    case Protocol::Scp:
        return "scp";
    case Protocol::Ftp:
        return "ftp";
    case Protocol::Ftps:
        return "ftps";
    case Protocol::WebDav:
        return "webdav";
    }
    return "sftp";
}

inline constexpr const char *protocolDisplayName(Protocol protocol) {
    switch (protocol) {
    case Protocol::Sftp:
        return "SFTP";
    case Protocol::Scp:
        return "SCP";
    case Protocol::Ftp:
        return "FTP";
    case Protocol::Ftps:
        return "FTPS";
    case Protocol::WebDav:
        return "WebDAV";
    }
    return "SFTP";
}

inline Protocol protocolFromStorageName(const std::string &raw) {
    if (raw.empty())
        return Protocol::Sftp;
    std::string normalized = raw;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) -> char {
                       return static_cast<char>(std::tolower(c));
                   });
    if (normalized == "scp")
        return Protocol::Scp;
    if (normalized == "ftp")
        return Protocol::Ftp;
    if (normalized == "ftps")
        return Protocol::Ftps;
    if (normalized == "webdav")
        return Protocol::WebDav;
    return Protocol::Sftp;
}

inline constexpr const char *scpTransferModeStorageName(ScpTransferMode mode) {
    switch (mode) {
    case ScpTransferMode::Auto:
        return "auto";
    case ScpTransferMode::ScpOnly:
        return "scp-only";
    }
    return "auto";
}

inline ScpTransferMode scpTransferModeFromStorageName(const std::string &raw) {
    if (raw.empty())
        return ScpTransferMode::Auto;
    std::string normalized = raw;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char c) -> char {
                       return static_cast<char>(std::tolower(c));
                   });
    if (normalized == "scp-only")
        return ScpTransferMode::ScpOnly;
    return ScpTransferMode::Auto;
}

inline ProtocolCapabilities capabilitiesForProtocol(Protocol protocol) {
    ProtocolCapabilities caps{};
    switch (protocol) {
    case Protocol::Sftp:
        caps.implemented = true;
        caps.supports_listing = true;
        caps.supports_file_transfers = true;
        caps.supports_resume = true;
        caps.supports_metadata = true;
        caps.supports_permissions = true;
        caps.supports_ownership = true;
        caps.supports_timestamps = true;
        caps.supports_proxy = true;
        caps.supports_jump_host = true;
        caps.supports_known_hosts = true;
        caps.supports_transfer_integrity = true;
        return caps;
    case Protocol::Scp:
        caps.implemented = true;
        caps.supports_file_transfers = true;
        caps.supports_proxy = true;
        caps.supports_jump_host = true;
        caps.supports_known_hosts = true;
        return caps;
    case Protocol::Ftp:
#if OPENSCP_HAS_CURL_FTP
        caps.implemented = true;
        caps.supports_file_transfers = true;
        caps.supports_proxy = true;
#endif
        return caps;
    case Protocol::Ftps:
#if OPENSCP_HAS_CURL_FTP
        caps.implemented = true;
        caps.supports_file_transfers = true;
        caps.supports_proxy = true;
#endif
        return caps;
    case Protocol::WebDav:
        return caps;
    }
    return caps;
}

struct FileInfo {
    std::string name; // base name
    bool is_dir = false;
    std::uint64_t size = 0;  // bytes (0 valid if has_size == true)
    bool has_size = false;   // true if size is known (ATTR_SIZE present)
    std::uint64_t mtime = 0; // epoch (seconds)
    std::uint32_t mode = 0;  // POSIX bits (permissions/type)
    std::uint32_t uid = 0;
    std::uint32_t gid = 0;
};

// Result for keyboard-interactive prompt handling.
// - Handled: callback provided answers in "responses".
// - Unhandled: callback could not answer; backend may use heuristic fallback.
// - Cancelled: user explicitly cancelled; backend must not use fallback.
enum class KbdIntPromptResult { Handled, Unhandled, Cancelled };

// Callback to answer keyboard-interactive prompts.
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

    // SSH security
    std::optional<std::string> known_hosts_path; // default: ~/.ssh/known_hosts
    KnownHostsPolicy known_hosts_policy = KnownHostsPolicy::Strict;
    // Whether to hash hostnames when saving to known_hosts (OpenSSH hashed
    // hosts)
    bool known_hosts_hash_names = true;
    // Visual preference: show fingerprint in HEX colon format (UI only)
    bool show_fp_hex = false;
    // Transfer integrity checks for resume and final content verification.
    TransferIntegrityPolicy transfer_integrity_policy =
        TransferIntegrityPolicy::Optional;

    // FTPS security
    bool ftps_verify_peer = true;
    std::optional<std::string> ftps_ca_cert_path;

    // Optional TCP proxy tunnel for SSH transport.
    ProxyType proxy_type = ProxyType::None;
    std::string proxy_host;
    std::uint16_t proxy_port = 0;
    std::optional<std::string> proxy_username;
    std::optional<std::string> proxy_password;

    // Optional SSH jump host (bastion) tunnel.
    // Current backend implementation uses the system OpenSSH client to build
    // a stdio tunnel (`ssh -W target:port jumpHost`).
    std::optional<std::string> jump_host;
    std::uint16_t jump_port = 22;
    std::optional<std::string> jump_username;
    std::optional<std::string> jump_private_key_path;

    // Host key confirmation (TOFU) when known_hosts lacks an entry.
    // Return true to accept and save, false to reject.
    // canSave: whether the client will be able to persist the host key (false
    // means user must explicitly allow a one‑time connection without saving)
    std::function<bool(const std::string &host, std::uint16_t port,
                       const std::string &algorithm,
                       const std::string &fingerprint, bool canSave)>
        hostkey_confirm_cb;

    // Optional: backend status messages (e.g., persist errors/reasons)
    std::function<void(const std::string &message)> hostkey_status_cb;

    // Custom handling for keyboard-interactive (e.g., OTP/2FA). Optional.
    KbdIntPromptsCB keyboard_interactive_cb;
};

} // namespace openscp
