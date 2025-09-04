#include "SecretStore.hpp"
#include <QSettings>
#include <QByteArray>
#include <QVariant>
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

void SecretStore::setSecret(const QString& key, const QString& value) {
    CFStringRef account = cfAccount(key);
    QByteArray dataBytes = value.toUtf8();
    CFDataRef data = CFDataCreate(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(dataBytes.constData()),
        dataBytes.size()
    );

    // Query para localizar el item existente
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
    );
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, kServiceNameCF());
    CFDictionarySetValue(query, kSecAttrAccount, account);

    // Intentar actualizar
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0,
        &kCFTypeDictionaryKeyCallBacks, &kCFTypeDictionaryValueCallBacks
    );
    CFDictionarySetValue(attrs, kSecValueData, data);
    OSStatus st = SecItemUpdate(query, attrs);

    if (st == errSecItemNotFound) {
        // Crear nuevo
        CFDictionarySetValue(query, kSecValueData, data);
        // Accesibilidad opcional: después del primer desbloqueo
        CFDictionarySetValue(query, kSecAttrAccessible, kSecAttrAccessibleAfterFirstUnlock);
        (void)SecItemAdd(query, nullptr);
    }

    if (attrs) CFRelease(attrs);
    if (query) CFRelease(query);
    if (data) CFRelease(data);
    if (account) CFRelease(account);
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

#else // non-Apple: fallback inseguro opcional por env var

static bool fallbackEnabledEnv() {
    const char* v = std::getenv("OPEN_SCP_ENABLE_INSECURE_FALLBACK");
    return v && *v == '1';
}

void SecretStore::setSecret(const QString& key, const QString& value) {
#ifdef OPEN_SCP_BUILD_SECURE_ONLY
    Q_UNUSED(key); Q_UNUSED(value);
    return; // deshabilitado por build seguro
#else
    if (!fallbackEnabledEnv()) return;
    QSettings s("OpenSCP", "Secrets");
    s.setValue(key, value);
#endif
}

std::optional<QString> SecretStore::getSecret(const QString& key) const {
#ifdef OPEN_SCP_BUILD_SECURE_ONLY
    Q_UNUSED(key);
    return std::nullopt;
#else
    if (!fallbackEnabledEnv()) return std::nullopt;
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
    if (!fallbackEnabledEnv()) return;
    QSettings s("OpenSCP", "Secrets");
    s.remove(key);
#endif
}

bool SecretStore::insecureFallbackActive() {
#ifdef OPEN_SCP_BUILD_SECURE_ONLY
    return false;
#else
    return fallbackEnabledEnv();
#endif
}

#endif
