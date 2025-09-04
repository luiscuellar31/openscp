// Tipos básicos compartidos entre UI y core para sesiones y metadatos SFTP.
// Mantener estas estructuras simples y serializables facilita su uso en UI.
#pragma once
#include <string>
#include <vector>
#include <optional>
#include <cstdint>
#include <functional>

namespace openscp {

// Política de validación de known_hosts para la clave del servidor.
enum class KnownHostsPolicy {
    Strict,     // Requiere coincidencia exacta con known_hosts.
    AcceptNew,  // TOFU: acepta y guarda nuevos hosts; rechaza cambios de clave.
    Off         // Sin verificación (no recomendado).
};

struct FileInfo {
    std::string   name;     // nombre base
    bool          is_dir = false;
    std::uint64_t size  = 0;  // bytes (si aplica)
    std::uint64_t mtime = 0;  // epoch (segundos)
    std::uint32_t mode  = 0;  // bits POSIX (permisos/tipo)
    std::uint32_t uid   = 0;
    std::uint32_t gid   = 0;
};

// Callback para responder a prompts de keyboard-interactive.
// Debe devolver true y llenar "responses" con un elemento por prompt si el usuario completó la info.
// Si devuelve false, el backend usará una heurística (usuario/contraseña) como fallback.
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

    // Seguridad SSH
    std::optional<std::string> known_hosts_path; // por defecto: ~/.ssh/known_hosts
    KnownHostsPolicy known_hosts_policy = KnownHostsPolicy::Strict;

    // Confirmación de huella (TOFU) cuando known_hosts no tiene entrada.
    // Devuelve true para aceptar y guardar, false para rechazar.
    std::function<bool(const std::string& host,
                       std::uint16_t port,
                       const std::string& algorithm,
                       const std::string& fingerprint)> hostkey_confirm_cb;

    // Manejo personalizado de keyboard-interactive (p. ej., OTP/2FA). Opcional.
    KbdIntPromptsCB keyboard_interactive_cb;
};

} // namespace openscp
