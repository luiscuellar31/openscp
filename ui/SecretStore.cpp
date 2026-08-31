// SecretStore implementation: Keychain (macOS), DPAPI (Windows), Libsecret
// (Linux), or an explicitly enabled fallback with QSettings.
#define QT_NO_KEYWORDS // error GDBusSignalInfo  **signals macros
#include "SecretStore.hpp"
#if defined(HAVE_LIBSECRET)
#include <libsecret/secret.h>
#endif
#include "AppSettings.hpp"

#include <QByteArray>
#include <QString>
#include <QVariant>

#include <cstdint>
#include <cstdlib>
#include <limits>

#ifdef Q_OS_WIN
#include <wincrypt.h>
#include <windows.h>
#endif

#ifdef __APPLE__
#include <CoreFoundation/CoreFoundation.h>
#include <Security/Security.h>

namespace {

CFStringRef kServiceNameCF() {
    static CFStringRef s = CFSTR("OpenSCP");
    return s;
}

CFStringRef cfAccount(const QString &key) {
    return CFStringCreateWithCharacters(
        kCFAllocatorDefault, reinterpret_cast<const UniChar *>(key.utf16()),
        key.size());
}

SecretStore::PersistResult mapApplePersistStatus(OSStatus st) {
    SecretStore::PersistResult r{};
    if (st == errSecSuccess) {
        r.status = SecretStore::PersistStatus::Stored;
        return r;
    }
    if (st == errSecNotAvailable) {
        r.status = SecretStore::PersistStatus::Unavailable;
    } else if (st == errSecAuthFailed || st == errSecInteractionNotAllowed ||
               st == errSecUserCanceled) {
        r.status = SecretStore::PersistStatus::PermissionDenied;
    } else {
        r.status = SecretStore::PersistStatus::BackendError;
    }
    r.detail = QString("Keychain OSStatus=%1").arg(static_cast<int>(st));
    return r;
}

SecretStore::LoadResult mapAppleLoadFailure(OSStatus status) {
    SecretStore::LoadStatus mapped = SecretStore::LoadStatus::BackendError;
    if (status == errSecItemNotFound) {
        mapped = SecretStore::LoadStatus::Missing;
    } else if (status == errSecNotAvailable) {
        mapped = SecretStore::LoadStatus::Unavailable;
    } else if (status == errSecAuthFailed ||
               status == errSecInteractionNotAllowed ||
               status == errSecUserCanceled) {
        mapped = SecretStore::LoadStatus::PermissionDenied;
    }
    return {
        mapped,
        {},
        QStringLiteral("Keychain OSStatus=%1").arg(static_cast<int>(status))};
}

SecretStore::DeleteResult mapAppleDeleteStatus(OSStatus status) {
    if (status == errSecSuccess)
        return {SecretStore::DeleteStatus::Removed, {}};
    if (status == errSecItemNotFound)
        return {SecretStore::DeleteStatus::Missing, {}};
    SecretStore::DeleteStatus mapped = SecretStore::DeleteStatus::BackendError;
    if (status == errSecNotAvailable) {
        mapped = SecretStore::DeleteStatus::Unavailable;
    } else if (status == errSecAuthFailed ||
               status == errSecInteractionNotAllowed ||
               status == errSecUserCanceled) {
        mapped = SecretStore::DeleteStatus::PermissionDenied;
    }
    return {
        mapped,
        QStringLiteral("Keychain OSStatus=%1").arg(static_cast<int>(status))};
}

} // namespace

SecretStore::PersistResult SecretStore::setSecret(const QString &key,
                                                  const QString &value) {
    if (key.isEmpty()) {
        return {PersistStatus::BackendError,
                QStringLiteral("Secret key is empty")};
    }
    CFStringRef account = cfAccount(key);
    QByteArray dataBytes = value.toUtf8();
    CFDataRef data =
        CFDataCreate(kCFAllocatorDefault,
                     reinterpret_cast<const UInt8 *>(dataBytes.constData()),
                     dataBytes.size());
    if (!account || !data) {
        dataBytes.fill('\0');
        if (data)
            CFRelease(data);
        if (account)
            CFRelease(account);
        return {PersistStatus::BackendError,
                QStringLiteral("Could not build Keychain entry")};
    }

    // Accessibility policy based on user preference (default: less restrictive
    // OFF)
    openscpui::AppSettings settings;
    const bool restrictive =
        settings.value(openscpui::settingskeys::kMacKeychainRestrictive, false)
            .toBool();
    CFTypeRef chosenAttr = restrictive
                               ? kSecAttrAccessibleWhenUnlockedThisDeviceOnly
                               : kSecAttrAccessibleAfterFirstUnlock;

    // Query to locate the existing item
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (!query) {
        dataBytes.fill('\0');
        CFRelease(data);
        CFRelease(account);
        return {PersistStatus::BackendError,
                QStringLiteral("Could not allocate Keychain query")};
    }
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, kServiceNameCF());
    CFDictionarySetValue(query, kSecAttrAccount, account);

    // Attempt to update
    CFMutableDictionaryRef attrs = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (!attrs) {
        dataBytes.fill('\0');
        CFRelease(query);
        CFRelease(data);
        CFRelease(account);
        return {PersistStatus::BackendError,
                QStringLiteral("Could not allocate Keychain attributes")};
    }
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

    if (attrs)
        CFRelease(attrs);
    if (query)
        CFRelease(query);
    if (data)
        CFRelease(data);
    if (account)
        CFRelease(account);
    dataBytes.fill('\0');

    return mapApplePersistStatus(finalStatus);
}

SecretStore::LoadResult SecretStore::getSecret(const QString &key) const {
    if (key.isEmpty())
        return {LoadStatus::BackendError,
                {},
                QStringLiteral("Secret key is empty")};
    CFStringRef account = cfAccount(key);
    if (!account)
        return {LoadStatus::BackendError,
                {},
                QStringLiteral("Could not build Keychain account")};
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (!query) {
        CFRelease(account);
        return {LoadStatus::BackendError,
                {},
                QStringLiteral("Could not allocate Keychain query")};
    }
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, kServiceNameCF());
    CFDictionarySetValue(query, kSecAttrAccount, account);
    CFDictionarySetValue(query, kSecReturnData, kCFBooleanTrue);
    CFDictionarySetValue(query, kSecMatchLimit, kSecMatchLimitOne);

    CFTypeRef result = nullptr;
    OSStatus st = SecItemCopyMatching(query, &result);
    if (query)
        CFRelease(query);
    if (account)
        CFRelease(account);
    if (st != errSecSuccess)
        return mapAppleLoadFailure(st);
    if (!result)
        return {LoadStatus::Corrupt,
                {},
                QStringLiteral("Keychain returned an empty result")};

    if (CFGetTypeID(result) != CFDataGetTypeID()) {
        CFRelease(result);
        return {LoadStatus::Corrupt,
                {},
                QStringLiteral("Keychain item has an unexpected type")};
    }
    CFDataRef data = static_cast<CFDataRef>(result);
    const UInt8 *bytes = CFDataGetBytePtr(data);
    const CFIndex len = CFDataGetLength(data);
    if (len < 0 ||
        len > static_cast<CFIndex>(std::numeric_limits<int>::max()) ||
        (len != 0 && !bytes)) {
        CFRelease(result);
        return {LoadStatus::Corrupt,
                {},
                QStringLiteral("Keychain item contains invalid data")};
    }
    const QString out = QString::fromUtf8(reinterpret_cast<const char *>(bytes),
                                          static_cast<int>(len));
    CFRelease(result);
    return {LoadStatus::Loaded, out, {}};
}

SecretStore::DeleteResult SecretStore::removeSecret(const QString &key) {
    if (key.isEmpty())
        return {DeleteStatus::BackendError,
                QStringLiteral("Secret key is empty")};
    CFStringRef account = cfAccount(key);
    if (!account)
        return {DeleteStatus::BackendError,
                QStringLiteral("Could not build Keychain account")};
    CFMutableDictionaryRef query = CFDictionaryCreateMutable(
        kCFAllocatorDefault, 0, &kCFTypeDictionaryKeyCallBacks,
        &kCFTypeDictionaryValueCallBacks);
    if (!query) {
        CFRelease(account);
        return {DeleteStatus::BackendError,
                QStringLiteral("Could not allocate Keychain query")};
    }
    CFDictionarySetValue(query, kSecClass, kSecClassGenericPassword);
    CFDictionarySetValue(query, kSecAttrService, kServiceNameCF());
    CFDictionarySetValue(query, kSecAttrAccount, account);
    const OSStatus status = SecItemDelete(query);
    if (query)
        CFRelease(query);
    if (account)
        CFRelease(account);
    return mapAppleDeleteStatus(status);
}

bool SecretStore::insecureFallbackActive() {
    return false;
}

#elif defined(Q_OS_WIN)

namespace {

constexpr char kDpapiPrefix[] = "dpapi-v1:";
constexpr char kDpapiEntropy[] = "OpenSCP/SecretStore/DPAPI/v1";

DATA_BLOB byteArrayBlob(QByteArray &bytes) {
    return {static_cast<DWORD>(bytes.size()),
            reinterpret_cast<BYTE *>(bytes.data())};
}

bool fitsDataBlob(const QByteArray &bytes) {
    return static_cast<std::uint64_t>(bytes.size()) <=
           static_cast<std::uint64_t>((std::numeric_limits<DWORD>::max)());
}

void eraseByteArray(QByteArray &bytes) {
    if (!bytes.isEmpty())
        SecureZeroMemory(bytes.data(), static_cast<SIZE_T>(bytes.size()));
    bytes.clear();
}

QString dpapiError(const char *operation, DWORD errorCode) {
    return QStringLiteral("%1 failed (Windows error %2)")
        .arg(QString::fromLatin1(operation))
        .arg(static_cast<qulonglong>(errorCode));
}

SecretStore::PersistStatus dpapiPersistStatus(DWORD errorCode) {
    return errorCode == ERROR_ACCESS_DENIED
               ? SecretStore::PersistStatus::PermissionDenied
               : SecretStore::PersistStatus::BackendError;
}

SecretStore::LoadStatus dpapiLoadStatus(DWORD errorCode) {
    return errorCode == ERROR_ACCESS_DENIED
               ? SecretStore::LoadStatus::PermissionDenied
               : SecretStore::LoadStatus::BackendError;
}

} // namespace

SecretStore::PersistResult SecretStore::setSecret(const QString &key,
                                                  const QString &value) {
    if (key.isEmpty()) {
        return {PersistStatus::BackendError,
                QStringLiteral("Secret key is empty")};
    }

    QByteArray plaintext = value.toUtf8();
    QByteArray entropy(kDpapiEntropy);
    if (!fitsDataBlob(plaintext) || !fitsDataBlob(entropy)) {
        eraseByteArray(plaintext);
        return {PersistStatus::BackendError,
                QStringLiteral("Secret is too large for Windows DPAPI")};
    }

    DATA_BLOB plaintextBlob = byteArrayBlob(plaintext);
    DATA_BLOB entropyBlob = byteArrayBlob(entropy);
    DATA_BLOB protectedBlob{};
    const BOOL protectedOk = CryptProtectData(
        &plaintextBlob, L"OpenSCP saved credential", &entropyBlob, nullptr,
        nullptr, CRYPTPROTECT_UI_FORBIDDEN, &protectedBlob);
    const DWORD protectError = protectedOk ? ERROR_SUCCESS : GetLastError();
    eraseByteArray(plaintext);
    if (!protectedOk) {
        return {dpapiPersistStatus(protectError),
                dpapiError("CryptProtectData", protectError)};
    }

    QByteArray encoded(kDpapiPrefix);
    encoded += QByteArray(reinterpret_cast<const char *>(protectedBlob.pbData),
                          static_cast<qsizetype>(protectedBlob.cbData))
                   .toBase64();
    if (protectedBlob.pbData) {
        SecureZeroMemory(protectedBlob.pbData, protectedBlob.cbData);
        LocalFree(protectedBlob.pbData);
    }

    openscpui::AppSettings settings(
        openscpui::AppSettings::Store::SecretFallback);
    settings.setValue(key, encoded);
    const auto syncResult = settings.syncSecure();
    eraseByteArray(encoded);
    if (!syncResult.ok)
        return {PersistStatus::BackendError, syncResult.error};
    return {PersistStatus::Stored, {}};
}

SecretStore::LoadResult SecretStore::getSecret(const QString &key) const {
    if (key.isEmpty())
        return {LoadStatus::BackendError,
                {},
                QStringLiteral("Secret key is empty")};

    openscpui::AppSettings settings(
        openscpui::AppSettings::Store::SecretFallback);
    if (!settings.contains(key))
        return {LoadStatus::Missing, {}, {}};
    QByteArray stored = settings.value(key).toByteArray();
    if (!stored.startsWith(kDpapiPrefix)) {
        eraseByteArray(stored);
        return {LoadStatus::Corrupt,
                {},
                QStringLiteral("Credential is not DPAPI-encrypted")};
    }

    QByteArray protectedBytes =
        QByteArray::fromBase64(stored.sliced(sizeof(kDpapiPrefix) - 1),
                               QByteArray::AbortOnBase64DecodingErrors);
    eraseByteArray(stored);
    QByteArray entropy(kDpapiEntropy);
    if (protectedBytes.isEmpty() || !fitsDataBlob(protectedBytes) ||
        !fitsDataBlob(entropy)) {
        eraseByteArray(protectedBytes);
        return {LoadStatus::Corrupt,
                {},
                QStringLiteral("Credential has invalid DPAPI encoding")};
    }

    DATA_BLOB protectedBlob = byteArrayBlob(protectedBytes);
    DATA_BLOB entropyBlob = byteArrayBlob(entropy);
    DATA_BLOB plaintextBlob{};
    const BOOL unprotectedOk =
        CryptUnprotectData(&protectedBlob, nullptr, &entropyBlob, nullptr,
                           nullptr, CRYPTPROTECT_UI_FORBIDDEN, &plaintextBlob);
    const DWORD unprotectError = unprotectedOk ? ERROR_SUCCESS : GetLastError();
    eraseByteArray(protectedBytes);
    if (!unprotectedOk) {
        return {dpapiLoadStatus(unprotectError),
                {},
                dpapiError("CryptUnprotectData", unprotectError)};
    }

    const QString secret =
        QString::fromUtf8(reinterpret_cast<const char *>(plaintextBlob.pbData),
                          static_cast<qsizetype>(plaintextBlob.cbData));
    if (plaintextBlob.pbData) {
        SecureZeroMemory(plaintextBlob.pbData, plaintextBlob.cbData);
        LocalFree(plaintextBlob.pbData);
    }
    return {LoadStatus::Loaded, secret, {}};
}

SecretStore::DeleteResult SecretStore::removeSecret(const QString &key) {
    if (key.isEmpty())
        return {DeleteStatus::BackendError,
                QStringLiteral("Secret key is empty")};
    openscpui::AppSettings settings(
        openscpui::AppSettings::Store::SecretFallback);
    if (!settings.contains(key))
        return {DeleteStatus::Missing, {}};
    settings.remove(key);
    const auto syncResult = settings.syncSecure();
    if (!syncResult.ok)
        return {DeleteStatus::BackendError, syncResult.error};
    return {DeleteStatus::Removed, {}};
}

bool SecretStore::insecureFallbackActive() {
    return false;
}

#elif defined(HAVE_LIBSECRET) // Linux with Libsecret/Secret Service

namespace {

const SecretSchema *openscp_schema() {
    static const SecretSchema schema = {
        "openscp.secret",
        SECRET_SCHEMA_NONE,
        {{"key", SECRET_SCHEMA_ATTRIBUTE_STRING}, {NULL}}};
    return &schema;
}

} // namespace

SecretStore::PersistResult SecretStore::setSecret(const QString &key,
                                                  const QString &value) {
    if (key.isEmpty()) {
        return {PersistStatus::BackendError,
                QStringLiteral("Secret key is empty")};
    }
    QByteArray keyUtf8 = key.toUtf8();
    QByteArray secretValueUtf8 = value.toUtf8();
    GError *gerr = nullptr;
    const gboolean ok = secret_password_store_sync(
        openscp_schema(), SECRET_COLLECTION_DEFAULT, "OpenSCP secret",
        secretValueUtf8.constData(), nullptr, &gerr, "key", keyUtf8.constData(),
        nullptr);
    secretValueUtf8.fill('\0');
    if (ok)
        return {PersistStatus::Stored, QString()};
    QString detail = gerr ? QString::fromUtf8(gerr->message)
                          : QStringLiteral("libsecret store failed");
    if (gerr)
        g_error_free(gerr);
    return {PersistStatus::BackendError, detail};
}

SecretStore::LoadResult SecretStore::getSecret(const QString &key) const {
    if (key.isEmpty())
        return {LoadStatus::BackendError,
                {},
                QStringLiteral("Secret key is empty")};
    QByteArray keyUtf8 = key.toUtf8();
    GError *gerr = nullptr;
    gchar *pw = secret_password_lookup_sync(
        openscp_schema(), nullptr, &gerr, "key", keyUtf8.constData(), nullptr);
    if (!pw) {
        if (gerr) {
            const QString detail = QString::fromUtf8(gerr->message);
            g_error_free(gerr);
            return {LoadStatus::BackendError, {}, detail};
        }
        return {LoadStatus::Missing, {}, {}};
    }
    QString out = QString::fromUtf8(pw);
    secret_password_free(pw);
    if (gerr)
        g_error_free(gerr);
    return {LoadStatus::Loaded, out, {}};
}

SecretStore::DeleteResult SecretStore::removeSecret(const QString &key) {
    if (key.isEmpty())
        return {DeleteStatus::BackendError,
                QStringLiteral("Secret key is empty")};
    QByteArray keyUtf8 = key.toUtf8();
    GError *gerr = nullptr;
    const gboolean removed = secret_password_clear_sync(
        openscp_schema(), nullptr, &gerr, "key", keyUtf8.constData(), nullptr);
    if (gerr) {
        const QString detail = QString::fromUtf8(gerr->message);
        g_error_free(gerr);
        return {DeleteStatus::BackendError, detail};
    }
    return {removed ? DeleteStatus::Removed : DeleteStatus::Missing, {}};
}

bool SecretStore::insecureFallbackActive() {
    return false; // Using Libsecret (secure)
}

#else // non-Apple and without Libsecret: optional insecure fallback controlled
      // by env var or settings

#ifndef OPENSCP_BUILD_SECURE_ONLY
namespace {

bool fallbackEnabledEnv() {
    const char *v = std::getenv("OPENSCP_ENABLE_INSECURE_FALLBACK");
    return v && *v == '1';
}

bool fallbackEnabledConfigured() {
    openscpui::AppSettings settings;
    return settings
        .value(openscpui::settingskeys::kEnableInsecureSecretFallback, false)
        .toBool();
}

bool fallbackEnabled() {
    return fallbackEnabledEnv() || fallbackEnabledConfigured();
}

} // namespace
#endif

SecretStore::PersistResult SecretStore::setSecret(const QString &key,
                                                  const QString &value) {
#ifdef OPENSCP_BUILD_SECURE_ONLY
    Q_UNUSED(key);
    Q_UNUSED(value);
    return {
        PersistStatus::Unavailable,
        QStringLiteral(
            "Secure-only build: no secure backend available in this platform")};
#else
    if (key.isEmpty())
        return {PersistStatus::BackendError,
                QStringLiteral("Secret key is empty")};
    if (!fallbackEnabled()) {
        return {PersistStatus::Unavailable,
                QStringLiteral("Insecure fallback disabled by configuration")};
    }
    openscpui::AppSettings settings(
        openscpui::AppSettings::Store::SecretFallback);
    settings.setValue(key, value);
    const auto syncResult = settings.syncSecure();
    if (!syncResult.ok)
        return {PersistStatus::BackendError, syncResult.error};
    return {PersistStatus::Stored, QString()};
#endif
}

SecretStore::LoadResult SecretStore::getSecret(const QString &key) const {
#ifdef OPENSCP_BUILD_SECURE_ONLY
    Q_UNUSED(key);
    return {
        LoadStatus::Unavailable,
        {},
        QStringLiteral("Secure-only build: no secure backend available in this "
                       "platform")};
#else
    if (!fallbackEnabled())
        return {LoadStatus::Unavailable,
                {},
                QStringLiteral("Insecure fallback disabled by configuration")};
    if (key.isEmpty())
        return {LoadStatus::BackendError,
                {},
                QStringLiteral("Secret key is empty")};
    openscpui::AppSettings settings(
        openscpui::AppSettings::Store::SecretFallback);
    QVariant storedValue = settings.value(key);
    if (!storedValue.isValid())
        return {LoadStatus::Missing, {}, {}};
    return {LoadStatus::Loaded, storedValue.toString(), {}};
#endif
}

SecretStore::DeleteResult SecretStore::removeSecret(const QString &key) {
#ifdef OPENSCP_BUILD_SECURE_ONLY
    Q_UNUSED(key);
    return {
        DeleteStatus::Unavailable,
        QStringLiteral("Secure-only build: no secure backend available in this "
                       "platform")};
#else
    if (!fallbackEnabled())
        return {DeleteStatus::Unavailable,
                QStringLiteral("Insecure fallback disabled by configuration")};
    if (key.isEmpty())
        return {DeleteStatus::BackendError,
                QStringLiteral("Secret key is empty")};
    openscpui::AppSettings settings(
        openscpui::AppSettings::Store::SecretFallback);
    if (!settings.contains(key))
        return {DeleteStatus::Missing, {}};
    settings.remove(key);
    const auto syncResult = settings.syncSecure();
    if (!syncResult.ok)
        return {DeleteStatus::BackendError, syncResult.error};
    return {DeleteStatus::Removed, {}};
#endif
}

bool SecretStore::insecureFallbackActive() {
#ifdef OPENSCP_BUILD_SECURE_ONLY
    return false;
#else
    return fallbackEnabled();
#endif
}

#endif
