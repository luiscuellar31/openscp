// SecretStore implementation: Keychain (macOS), Libsecret (Linux), or optional fallback with QSettings.
#include "SecretStore.hpp"
#include <QSettings>
#include <QByteArray>
#include <QVariant>
#include <QString>
#include <cstdlib>

#ifdef __APPLE__
#include <Security/Security.h>
#include <CoreFoundation/CoreFoundation.h>

static CFStringRef kServiceNameCF() {
    static CFStringRef s = CFSTR("OpenSCP");
    return s;
}

static CFStringRef cfAccount(const QString& key) {
    return CFStringCreateWithCharacters(
        kCFAllocatorDefault,
        reinterpret_cast<const UniChar*>(key.utf16()),
        key.size()
    );
}

static SecretStore::PersistResult mapApplePersistStatus(OSStatus st) {
    SecretStore::PersistResult r{};
    if (st == errSecSuccess) {
        r.status = SecretStore::PersistStatus::Stored;
        return r;
    }
    if (st == errSecNotAvailable) {
        r.status = SecretStore::PersistStatus::Unavailable;
    } else if (st == errSecAuthFailed || st == errSecInteractionNotAllowed || st == errSecUserCanceled) {
        r.status = SecretStore::PersistStatus::PermissionDenied;
    } else {
        r.status = SecretStore::PersistStatus::BackendError;
    }
    r.detail = QString("Keychain OSStatus=%1").arg((int)st);
    return r;
}

SecretStore::PersistResult SecretStore::setSecret(const QString& key, const QString& value) {
    if (key.isEmpty()) {
        return { PersistStatus::BackendError, QStringLiteral("Secret key vacío") };
    }
    CFStringRef account = cfAccount(key);
    QByteArray dataBytes = value.toUtf8();
    CFDataRef data = CFDataCreate(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(dataBytes.constData()),
        dataBytes.size()
    );
    if (!account || !data) {
        if (data) CFRelease(data);
        if (account) CFRelease(account);
        return { PersistStatus::BackendError, QStringLiteral("No se pudo construir entrada para Keychain") };
    }

    // Accessibility policy based on user preference (default: less restrictive OFF)
    QSettings s("OpenSCP", "OpenSCP");
    const bool restrictive = s.value("Security/macKeychainRestrictive", false).toBool();
    CFTypeRef chosenAttr = restrictive ? kSecAttrAccessibleWhenUnlockedThisDeviceOnly
                                       : kSecAttrAccessibleAfterFirstUnlock;

    // Query to locate the existing item
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
    );
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, kServiceNameCF());
    CFDictionarySetValue(query, kSecAttrAccount, account);

    // Attempt to update
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
    );
    CFDictionarySetValue(attrs, kSecValueData, data);
    CFDictionarySetValue(attrs, kSecAttrAccessible, chosenAttr);
    OSStatus st = SecItemUpdate(query, attrs);

    OSStatus finalStatus = st;
    if (st == errSecItemNotFound) {
        // Create new item
        CFDictionarySetValue(query, kSecValueData, data);
        // Accessibility according to preference
        CFDictionarySetValue(query, kSecAttrAccessible, chosenAttr);
        finalStatus = SecItemAdd(query, nullptr);
    }

    if (attrs) CFRelease(attrs);
    if (query) CFRelease(query);
    if (data) CFRelease(data);
    if (account) CFRelease(account);

    return mapApplePersistStatus(finalStatus);
}

std::optional<QString> SecretStore::getSecret(const QString& key) const {
    CFStringRef account = cfAccount(key);
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
    );
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, kServiceNameCF());
    CFDictionarySetValue(query, kSecAttrAccount, account);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef result = nullptr;
    OSStatus st = SecItemCopyMatching(query, &result);
    if (query) CFRelease(query);
    if (account) CFRelease(account);
    if (st != errSecSuccess || !result) return std::nullopt;

    CFDataRef data = (CFDataRef)result;
    QString out;
    if (CFGetTypeID(data) == CFDataGetTypeID()) {
        const UInt8* bytes = CFDataGetBytePtr(data);
        CFIndex len = CFDataGetLength(data);
        out = QString::fromUtf8(reinterpret_cast<const char*>(bytes), (int)len);
    }
    CFRelease(result);
    return out.isEmpty() ? std::nullopt : std::optional<QString>(out);
}

void SecretStore::removeSecret(const QString& key) {
    CFStringRef account = cfAccount(key);
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
    );
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, kServiceNameCF());
    CFDictionarySetValue(query, kSecAttrAccount, account);
    (void)SecItemDelete(query);
    if (query) CFRelease(query);
    if (account) CFRelease(account);
}

bool SecretStore::insecureFallbackActive() {
    return false;
}

#elif defined(HAVE_LIBSECRET) // Linux with Libsecret/Secret Service

#include <libsecret/secret.h>

static const SecretSchema* openscp_schema() {
    static const SecretSchema schema = {
        "openscp.secret", SECRET_SCHEMA_NONE,
        {
            { "key", SECRET_SCHEMA_ATTRIBUTE_STRING },
            { NULL, 0 }
        }
    };
    return &schema;
}

SecretStore::PersistResult SecretStore::setSecret(const QString& key, const QString& value) {
    if (key.isEmpty()) {
        return { PersistStatus::BackendError, QStringLiteral("Secret key vacío") };
    }
    QByteArray k = key.toUtf8();
    QByteArray v = value.toUtf8();
    GError* gerr = nullptr;
    const gboolean ok = secret_password_store_sync(openscp_schema(), SECRET_COLLECTION_DEFAULT,
                                                   "OpenSCP secret", v.constData(), nullptr,
                                                   &gerr,
                                                   "key", k.constData(), nullptr);
    if (ok) return { PersistStatus::Stored, QString() };
    QString detail = gerr ? QString::fromUtf8(gerr->message) : QStringLiteral("libsecret store failed");
    if (gerr) g_error_free(gerr);
    return { PersistStatus::BackendError, detail };
}

std::optional<QString> SecretStore::getSecret(const QString& key) const {
    QByteArray k = key.toUtf8();
    gchar* pw = secret_password_lookup_sync(openscp_schema(), nullptr,
                                            "key", k.constData(), nullptr);
    if (!pw) return std::nullopt;
    QString out = QString::fromUtf8(pw);
    secret_password_free(pw);
    return out.isEmpty() ? std::nullopt : std::optional<QString>(out);
}

void SecretStore::removeSecret(const QString& key) {
    QByteArray k = key.toUtf8();
    (void)secret_password_clear_sync(openscp_schema(), nullptr,
                                     "key", k.constData(), nullptr);
}

bool SecretStore::insecureFallbackActive() {
    return false; // Using Libsecret (secure)
}

#else // non-Apple and without Libsecret: optional insecure fallback controlled by env var or settings

static bool fallbackEnabledEnv() {
    const char* v = std::getenv("OPEN_SCP_ENABLE_INSECURE_FALLBACK");
    return v && *v == '1';
}

static bool fallbackEnabledConfigured() {
    QSettings s("OpenSCP", "OpenSCP");
    return s.value("Security/enableInsecureSecretFallback", false).toBool();
}

static bool fallbackEnabled() {
    return fallbackEnabledEnv() || fallbackEnabledConfigured();
}

SecretStore::PersistResult SecretStore::setSecret(const QString& key, const QString& value) {
#ifdef OPEN_SCP_BUILD_SECURE_ONLY
    Q_UNUSED(key); Q_UNUSED(value);
    return { PersistStatus::Unavailable, QStringLiteral("Secure-only build: no secure backend available in this platform") };
#else
    if (key.isEmpty()) return { PersistStatus::BackendError, QStringLiteral("Secret key vacío") };
    if (!fallbackEnabled()) {
        return { PersistStatus::Unavailable, QStringLiteral("Fallback inseguro desactivado por configuración") };
    }
    QSettings s("OpenSCP", "Secrets");
    s.setValue(key, value);
    s.sync();
    if (s.status() != QSettings::NoError) {
        return { PersistStatus::BackendError, QStringLiteral("QSettings no pudo persistir el secreto") };
    }
    return { PersistStatus::Stored, QString() };
#endif
}

std::optional<QString> SecretStore::getSecret(const QString& key) const {
#ifdef OPEN_SCP_BUILD_SECURE_ONLY
    Q_UNUSED(key);
    return std::nullopt;
#else
    if (!fallbackEnabled()) return std::nullopt;
    QSettings s("OpenSCP", "Secrets");
    QVariant v = s.value(key);
    if (!v.isValid()) return std::nullopt;
    return v.toString();
#endif
}

void SecretStore::removeSecret(const QString& key) {
#ifdef OPEN_SCP_BUILD_SECURE_ONLY
    Q_UNUSED(key);
    return;
#else
    if (!fallbackEnabled()) return;
    QSettings s("OpenSCP", "Secrets");
    s.remove(key);
#endif
}

bool SecretStore::insecureFallbackActive() {
#ifdef OPEN_SCP_BUILD_SECURE_ONLY
    return false;
#else
    return fallbackEnabled();
#endif
}

#endif
