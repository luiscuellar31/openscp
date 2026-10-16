// Trust on first use for TLS connections that skip certificate verification:
// the pin of each server's public key is remembered, and a later connection
// refuses any other key unless the user trusts it again.
#pragma once

#include "openscp/SessionOptions.hpp"

#include <QString>

#include <functional>
#include <optional>
#include <string>

namespace openscpui {

// The pin remembered for the connection's server, if any.
std::optional<std::string>
trustedTlsKeyPin(const openscp::SessionOptions &options);
void rememberTlsKeyPin(const openscp::SessionOptions &options,
                       const std::string &pin);

using FetchTlsKeyPin =
    std::function<bool(const openscp::SessionOptions &options, std::string &pin,
                       std::string &error)>;
// Asks whether to trust `pin`; `changed` when another key was trusted before.
using ConfirmTlsKey = std::function<bool(const std::string &pin, bool changed)>;

// Reads the server's key with `fetch` and pins it in options: the remembered
// key, or a new one that `confirm` accepts. Returns false, with the reason in
// error, otherwise.
bool pinServerTlsKey(openscp::SessionOptions &options,
                     const FetchTlsKeyPin &fetch, const ConfirmTlsKey &confirm,
                     std::string &error);

// How a pin is shown: "SHA256:" and the Base64 digest.
QString tlsKeyFingerprint(const std::string &pin);

} // namespace openscpui
