// Secret storage abstraction for the UI.
// On macOS uses Keychain; on other OSes an insecure fallback may be enabled via env var.
#pragma once
#include <QString>
#include <optional>

// Minimal secret store abstraction.
// Current implementation: fallback with QSettings (not secure),
// designed to be replaceable by Keychain (macOS) / Secret Service (Linux).
class SecretStore {
public:
    enum class PersistStatus {
        Stored,
        Unavailable,
        PermissionDenied,
        BackendError
    };

    struct PersistResult {
        PersistStatus status = PersistStatus::Stored;
        QString detail;
        bool ok() const { return status == PersistStatus::Stored; }
    };

    // Store a secret under a logical key (e.g. "site:Name:password").
    PersistResult setSecret(const QString& key, const QString& value);

    // Retrieve a secret if present.
    std::optional<QString> getSecret(const QString& key) const;

    // Remove a secret (optional).
    void removeSecret(const QString& key);

    // Whether the insecure fallback is active (only for environments without Keychain/Secret Service).
    // On macOS always returns false. On other OSes depends on build macro and env var.
    static bool insecureFallbackActive();
};
