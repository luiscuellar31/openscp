// Secret storage abstraction for the UI. It uses Keychain on macOS, DPAPI on
// Windows, Secret Service on Linux, or an explicitly enabled fallback when no
// secure platform backend is available.
#pragma once
#include <QString>

// Minimal platform secret-store abstraction.
class SecretStore {
    public:
    enum class PersistStatus {
        Stored,
        Unavailable,
        PermissionDenied,
        BackendError,
        Corrupt
    };

    struct PersistResult {
        PersistStatus status = PersistStatus::Stored;
        QString detail;
        bool isStored() const { return status == PersistStatus::Stored; }
    };

    enum class LoadStatus {
        Loaded,
        Missing,
        Unavailable,
        PermissionDenied,
        BackendError,
        Corrupt
    };

    struct LoadResult {
        LoadStatus status = LoadStatus::Missing;
        QString value;
        QString detail;
        [[nodiscard]] bool isLoaded() const {
            return status == LoadStatus::Loaded;
        }
    };

    enum class DeleteStatus {
        Removed,
        Missing,
        Unavailable,
        PermissionDenied,
        BackendError
    };

    struct DeleteResult {
        DeleteStatus status = DeleteStatus::Missing;
        QString detail;
        [[nodiscard]] bool isRemovedOrMissing() const {
            return status == DeleteStatus::Removed ||
                   status == DeleteStatus::Missing;
        }
    };

    // Store a secret under a logical key (e.g. "site-id:<uuid>:password").
    PersistResult setSecret(const QString &key, const QString &value);

    // Retrieve a secret if present.
    [[nodiscard]] LoadResult getSecret(const QString &key) const;

    // Remove a secret and report whether cleanup actually succeeded.
    [[nodiscard]] DeleteResult removeSecret(const QString &key);

    // Whether the insecure fallback is active. Secure platform backends always
    // return false.
    static bool insecureFallbackActive();
};
