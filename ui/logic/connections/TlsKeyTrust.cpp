#include "logic/connections/TlsKeyTrust.hpp"

#include "logic/common/AppSettings.hpp"

#include <string_view>

namespace openscpui {
namespace {

constexpr std::string_view kPinPrefix = "sha256//";

// Pins are kept per protocol, host and port, like known_hosts entries.
QString pinKey(const openscp::SessionOptions &options) {
    const bool ftps = options.protocol == openscp::Protocol::Ftps;
    std::uint16_t port = options.port;
    if (port == 0) {
        port = ftps ? openscp::defaultPortForProtocol(openscp::Protocol::Ftps)
                    : openscp::defaultPortForWebDavScheme(
                          openscp::WebDavScheme::Https);
    }
    return QStringLiteral("%1/%2/%3:%4")
        .arg(QLatin1String(settingskeys::kTlsKeyPins),
             ftps ? QStringLiteral("ftps") : QStringLiteral("webdav"),
             QString::fromStdString(options.host).trimmed().toLower())
        .arg(port);
}

} // namespace

std::optional<std::string>
trustedTlsKeyPin(const openscp::SessionOptions &options) {
    AppSettings settings;
    const QString pin = settings.value(pinKey(options)).toString();
    if (!pin.startsWith(QLatin1String(kPinPrefix.data(), kPinPrefix.size())))
        return std::nullopt;
    return pin.toStdString();
}

void rememberTlsKeyPin(const openscp::SessionOptions &options,
                       const std::string &pin) {
    AppSettings settings;
    settings.setValue(pinKey(options), QString::fromStdString(pin));
    settings.sync();
}

bool pinServerTlsKey(openscp::SessionOptions &options,
                     const FetchTlsKeyPin &fetch, const ConfirmTlsKey &confirm,
                     std::string &error) {
    std::string pin;
    if (!fetch(options, pin, error))
        return false;
    const std::optional<std::string> &trusted = options.tls_pinned_public_key;
    const bool known = trusted.has_value() && !trusted->empty();
    if ((!known || *trusted != pin) && !confirm(pin, known)) {
        error = "The server's TLS public key was not trusted.";
        return false;
    }
    options.tls_pinned_public_key = pin;
    return true;
}

QString tlsKeyFingerprint(const std::string &pin) {
    std::string_view digest = pin;
    if (digest.starts_with(kPinPrefix))
        digest.remove_prefix(kPinPrefix.size());
    return QStringLiteral("SHA256:") +
           QString::fromUtf8(digest.data(),
                             static_cast<qsizetype>(digest.size()));
}

} // namespace openscpui
