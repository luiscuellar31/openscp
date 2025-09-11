// Basic types shared between UI and core for SFTP sessions and metadata.
// Keeping these structures simple and serializable makes them easy to use in the UI.
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <functional>

namespace openscp {

// known_hosts validation policy for the server host key.
enum class KnownHostsPolicy {
    Strict,     // Requires exact match with known_hosts.
    AcceptNew,  // TOFU: accept and save new hosts; reject key changes.
    Off         // No verification (not recommended).
};

struct FileInfo {
    std::string   name;         // base name
    bool          is_dir = false;
    std::uint64_t size  = 0;    // bytes (0 valid if has_size == true)
    bool          has_size = false; // true if size is known (ATTR_SIZE present)
    std::uint64_t mtime = 0;    // epoch (seconds)
    std::uint32_t mode  = 0;    // POSIX bits (permissions/type)
    std::uint32_t uid   = 0;
    std::uint32_t gid   = 0;
};

// Callback to answer keyboard-interactive prompts.
// Must return true and fill "responses" with one entry per prompt if the user provided input.
// If it returns false, the backend uses a heuristic (username/password) as a fallback.
using KbdIntPromptsCB = std::function<bool(const std::string& name,
                                           const std::string& instruction,
                                           const std::vector<std::string>& prompts,
                                           std::vector<std::string>& responses)>;

struct SessionOptions {
    std::string host;
    std::uint16_t port = 22;
    std::string username;

    std::optional<std::string> password;
    std::optional<std::string> private_key_path;
    std::optional<std::string> private_key_passphrase;

    // SSH security
    std::optional<std::string> known_hosts_path; // default: ~/.ssh/known_hosts
    KnownHostsPolicy known_hosts_policy = KnownHostsPolicy::Strict;
    // Whether to hash hostnames when saving to known_hosts (OpenSSH hashed hosts)
    bool known_hosts_hash_names = true;
    // Visual preference: show fingerprint in HEX colon format (UI only)
    bool show_fp_hex = false;

    // Host key confirmation (TOFU) when known_hosts lacks an entry.
    // Return true to accept and save, false to reject.
    std::function<bool(const std::string& host,
                       std::uint16_t port,
                       const std::string& algorithm,
                       const std::string& fingerprint)> hostkey_confirm_cb;

    // Custom handling for keyboard-interactive (e.g., OTP/2FA). Optional.
    KbdIntPromptsCB keyboard_interactive_cb;
};

} // namespace openscp
