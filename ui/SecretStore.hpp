#pragma once
#include <QString>
#include <optional>

// Abstracción mínima de almacén de secretos.
// Implementación actual: fallback con QSettings (no seguro),
// diseñada para poder migrar a Keychain (macOS) / Secret Service (Linux).
class SecretStore {
public:
    // Guarda un secreto asociado a una clave lógica (p.ej. "site:Nombre:password").
    void setSecret(const QString& key, const QString& value);

    // Recupera un secreto si existe.
    std::optional<QString> getSecret(const QString& key) const;

    // Elimina un secreto (opcional).
    void removeSecret(const QString& key);

    // Indica si está activo el fallback inseguro (solo para entornos sin Keychain/Secret Service).
    // En macOS siempre devuelve false. En otros SO depende del macro de build y de la variable de entorno.
    static bool insecureFallbackActive();
};