// Supported protocols, storage conversion, and operation capabilities.
#pragma once

#include "RemotePath.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <string>

#ifndef OPENSCP_HAS_CURL_FTP
#define OPENSCP_HAS_CURL_FTP 1
#endif

#ifndef OPENSCP_HAS_CURL_WEBDAV
#define OPENSCP_HAS_CURL_WEBDAV 0
#endif

namespace openscp {

enum class KnownHostsPolicy { Strict, AcceptNew, Off };
enum class TransferIntegrityPolicy { Off, Optional, Required };
enum class ProxyType { None, Socks5, HttpConnect };
enum class Protocol { Sftp, Scp, Ftp, Ftps, WebDav };
enum class WebDavScheme { Https, Http };
enum class FtpsMode { Auto, ExplicitTls, ImplicitTls };
enum class ScpTransferMode { Auto, ScpOnly };

struct ProtocolCapabilities {
    bool implemented = false;
    bool can_list = false;
    bool can_upload = false;
    bool can_download = false;
    bool can_stat = false;
    bool can_mkdir = false;
    bool can_delete = false;
    bool can_rename = false;
    bool can_resume_download = false;
    bool can_resume_upload = false;
    bool can_read_metadata = false;
    bool can_set_permissions = false;
    bool can_set_ownership = false;
    bool can_set_timestamps = false;
    bool can_checksum = false;

    // Deprecated aggregate flags retained for source compatibility.
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

inline constexpr bool isValidKnownHostsPolicy(KnownHostsPolicy policy) {
    return policy == KnownHostsPolicy::Strict ||
           policy == KnownHostsPolicy::AcceptNew ||
           policy == KnownHostsPolicy::Off;
}

// Invalid persisted values must never weaken host-key verification.
inline constexpr KnownHostsPolicy
normalizeKnownHostsPolicy(KnownHostsPolicy policy) {
    return isValidKnownHostsPolicy(policy) ? policy : KnownHostsPolicy::Strict;
}

inline KnownHostsPolicy knownHostsPolicyFromStorageValue(int raw) {
    return normalizeKnownHostsPolicy(static_cast<KnownHostsPolicy>(raw));
}

inline constexpr bool
isValidTransferIntegrityPolicy(TransferIntegrityPolicy policy) {
    return policy == TransferIntegrityPolicy::Off ||
           policy == TransferIntegrityPolicy::Optional ||
           policy == TransferIntegrityPolicy::Required;
}

inline constexpr TransferIntegrityPolicy
normalizeTransferIntegrityPolicy(TransferIntegrityPolicy policy) {
    return isValidTransferIntegrityPolicy(policy)
               ? policy
               : TransferIntegrityPolicy::Optional;
}

inline TransferIntegrityPolicy
transferIntegrityPolicyFromStorageValue(int raw) {
    return normalizeTransferIntegrityPolicy(
        static_cast<TransferIntegrityPolicy>(raw));
}

inline constexpr bool isValidProxyType(ProxyType type) {
    return type == ProxyType::None || type == ProxyType::Socks5 ||
           type == ProxyType::HttpConnect;
}

inline constexpr ProxyType normalizeProxyType(ProxyType type) {
    return isValidProxyType(type) ? type : ProxyType::None;
}

inline constexpr bool isValidWebDavScheme(WebDavScheme scheme) {
    return scheme == WebDavScheme::Https || scheme == WebDavScheme::Http;
}

inline constexpr WebDavScheme normalizeWebDavScheme(WebDavScheme scheme) {
    return isValidWebDavScheme(scheme) ? scheme : WebDavScheme::Https;
}

inline constexpr bool isValidFtpsMode(FtpsMode mode) {
    return mode == FtpsMode::Auto || mode == FtpsMode::ExplicitTls ||
           mode == FtpsMode::ImplicitTls;
}

inline constexpr FtpsMode normalizeFtpsMode(FtpsMode mode) {
    return isValidFtpsMode(mode) ? mode : FtpsMode::Auto;
}

inline ProxyType proxyTypeFromStorageValue(int raw) {
    return normalizeProxyType(static_cast<ProxyType>(raw));
}

inline constexpr std::uint16_t defaultPortForProxyType(ProxyType type) {
    switch (type) {
    case ProxyType::Socks5:
        return 1080;
    case ProxyType::HttpConnect:
        return 8080;
    case ProxyType::None:
        return 0;
    }
    return 0;
}

inline constexpr std::uint16_t defaultPortForWebDavScheme(WebDavScheme scheme) {
    return normalizeWebDavScheme(scheme) == WebDavScheme::Http ? 80 : 443;
}

inline constexpr const char *webDavSchemeStorageName(WebDavScheme scheme) {
    return normalizeWebDavScheme(scheme) == WebDavScheme::Http ? "http"
                                                               : "https";
}

inline WebDavScheme webDavSchemeFromStorageName(const std::string &raw) {
    std::string normalized = raw;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return normalized == "http" ? WebDavScheme::Http : WebDavScheme::Https;
}

inline constexpr const char *ftpsModeStorageName(FtpsMode mode) {
    switch (normalizeFtpsMode(mode)) {
    case FtpsMode::ExplicitTls:
        return "explicit";
    case FtpsMode::ImplicitTls:
        return "implicit";
    case FtpsMode::Auto:
        return "auto";
    }
    return "auto";
}

inline FtpsMode ftpsModeFromStorageName(const std::string &raw) {
    std::string normalized = raw;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    if (normalized == "explicit" || normalized == "explicit-tls")
        return FtpsMode::ExplicitTls;
    if (normalized == "implicit" || normalized == "implicit-tls")
        return FtpsMode::ImplicitTls;
    return FtpsMode::Auto;
}

inline std::string normalizeWebDavBasePath(std::string raw) {
    return normalizeRemotePath(raw);
}

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
    std::string normalized = raw;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
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
    return mode == ScpTransferMode::ScpOnly ? "scp-only" : "auto";
}

inline ScpTransferMode scpTransferModeFromStorageName(const std::string &raw) {
    std::string normalized = raw;
    std::transform(normalized.begin(), normalized.end(), normalized.begin(),
                   [](unsigned char character) {
                       return static_cast<char>(std::tolower(character));
                   });
    return normalized == "scp-only" ? ScpTransferMode::ScpOnly
                                    : ScpTransferMode::Auto;
}

inline ProtocolCapabilities capabilitiesForProtocol(Protocol protocol) {
    ProtocolCapabilities capabilities;
    bool managedFilesSupported = protocol == Protocol::Sftp;
#if OPENSCP_HAS_CURL_FTP
    managedFilesSupported = managedFilesSupported ||
                            protocol == Protocol::Ftp ||
                            protocol == Protocol::Ftps;
#endif
#if OPENSCP_HAS_CURL_WEBDAV
    managedFilesSupported =
        managedFilesSupported || protocol == Protocol::WebDav;
#endif
    const auto enableManagedFiles = [&capabilities] {
        capabilities.implemented = true;
        capabilities.can_list = true;
        capabilities.can_upload = true;
        capabilities.can_download = true;
        capabilities.can_stat = true;
        capabilities.can_mkdir = true;
        capabilities.can_delete = true;
        capabilities.can_rename = true;
        capabilities.can_read_metadata = true;
        capabilities.supports_listing = true;
        capabilities.supports_file_transfers = true;
        capabilities.supports_metadata = true;
        capabilities.supports_proxy = true;
    };

    switch (protocol) {
    case Protocol::Sftp:
        capabilities.can_resume_download = true;
        capabilities.can_resume_upload = true;
        capabilities.can_set_permissions = true;
        capabilities.can_set_ownership = true;
        capabilities.can_set_timestamps = true;
        capabilities.can_checksum = true;
        capabilities.supports_resume = true;
        capabilities.supports_permissions = true;
        capabilities.supports_ownership = true;
        capabilities.supports_timestamps = true;
        capabilities.supports_jump_host = true;
        capabilities.supports_known_hosts = true;
        capabilities.supports_transfer_integrity = true;
        break;
    case Protocol::Scp:
        capabilities.implemented = true;
        capabilities.can_upload = true;
        capabilities.can_download = true;
        capabilities.supports_file_transfers = true;
        capabilities.supports_proxy = true;
        capabilities.supports_jump_host = true;
        capabilities.supports_known_hosts = true;
        break;
    case Protocol::Ftp:
    case Protocol::Ftps:
    case Protocol::WebDav:
        break;
    }
    if (managedFilesSupported)
        enableManagedFiles();
    return capabilities;
}

} // namespace openscp
