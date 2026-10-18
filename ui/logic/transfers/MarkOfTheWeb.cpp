#include "logic/transfers/MarkOfTheWeb.hpp"

#include <QUrl>

#ifdef Q_OS_WIN
// clang-format off
#include <windows.h>
#include <urlmon.h>
// clang-format on

#include <QDir>
#include <QHash>

#include <mutex>
#endif

namespace openscpui {

namespace {

QString originScheme(const openscp::SessionOptions &options) {
    switch (options.protocol) {
    case openscp::Protocol::Sftp:
        return QStringLiteral("sftp");
    case openscp::Protocol::Scp:
        return QStringLiteral("scp");
    case openscp::Protocol::Ftp:
        return QStringLiteral("ftp");
    case openscp::Protocol::Ftps:
        return QStringLiteral("ftps");
    case openscp::Protocol::WebDav:
        return openscp::normalizeWebDavScheme(options.webdav_scheme) ==
                       openscp::WebDavScheme::Http
                   ? QStringLiteral("http")
                   : QStringLiteral("https");
    }
    return QStringLiteral("sftp");
}

#ifdef Q_OS_WIN
bool policyKeepsZoneInformation() {
    // "Do not preserve zone information in file attachments", from Group
    // Policy or the user's own setting: 1 means that Windows saves none.
    for (HKEY root : {HKEY_LOCAL_MACHINE, HKEY_CURRENT_USER}) {
        DWORD value = 0;
        DWORD size = sizeof(value);
        if (::RegGetValueW(root,
                           L"Software\\Microsoft\\Windows\\CurrentVersion\\"
                           L"Policies\\Attachments",
                           L"SaveZoneInformation", RRF_RT_REG_DWORD, nullptr,
                           &value, &size) == ERROR_SUCCESS &&
            value == 1)
            return false;
    }
    return true;
}

DWORD securityZone(const QString &originUrl) {
    const HRESULT com = ::CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                                      COINIT_DISABLE_OLE1DDE);
    DWORD zone = URLZONE_INTERNET;
    IInternetSecurityManager *manager = nullptr;
    if (SUCCEEDED(::CoInternetCreateSecurityManager(nullptr, &manager, 0))) {
        DWORD mapped = 0;
        if (SUCCEEDED(manager->MapUrlToZone(
                reinterpret_cast<const wchar_t *>(originUrl.utf16()), &mapped,
                0)))
            zone = mapped;
        manager->Release();
    }
    if (SUCCEEDED(com))
        ::CoUninitialize();
    return zone;
}

// The zone to record for files from originUrl, or 0 for none. Each origin is
// looked up once, rather than for every file.
DWORD zoneToRecord(const QString &originUrl) {
    static std::mutex mutex;
    static QHash<QString, DWORD> zones;
    std::lock_guard<std::mutex> lock(mutex);
    const auto known = zones.constFind(originUrl);
    if (known != zones.cend())
        return *known;
    const DWORD zone = policyKeepsZoneInformation()
                           ? securityZone(originUrl)
                           : static_cast<DWORD>(URLZONE_LOCAL_MACHINE);
    const DWORD recorded = zone >= URLZONE_INTERNET ? zone : 0;
    zones.insert(originUrl, recorded);
    return recorded;
}
#endif

} // namespace

QString downloadOriginUrl(const openscp::SessionOptions &options) {
    QUrl url;
    url.setScheme(originScheme(options));
    url.setHost(QString::fromStdString(options.host));
    if (options.port != 0)
        url.setPort(options.port);
    url.setPath(QStringLiteral("/"));
    // An internationalized host name in its ASCII form, as a URL needs.
    return url.isValid() ? QString::fromLatin1(url.toEncoded()) : QString();
}

void markDownloadedFile(const QString &localPath, const QString &originUrl) {
#ifdef Q_OS_WIN
    if (localPath.isEmpty() || originUrl.isEmpty())
        return;
    const DWORD zone = zoneToRecord(originUrl);
    if (zone == 0)
        return;
    const QByteArray mark = QByteArrayLiteral("[ZoneTransfer]\r\nZoneId=") +
                            QByteArray::number(static_cast<quint32>(zone)) +
                            QByteArrayLiteral("\r\nHostUrl=") +
                            originUrl.toLatin1() + QByteArrayLiteral("\r\n");
    const QString stream = QDir::toNativeSeparators(localPath) +
                           QStringLiteral(":Zone.Identifier");
    // A file system without alternate streams, such as FAT, keeps no mark.
    const HANDLE file = ::CreateFileW(
        reinterpret_cast<const wchar_t *>(stream.utf16()), GENERIC_WRITE, 0,
        nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return;
    DWORD written = 0;
    (void)::WriteFile(file, mark.constData(), static_cast<DWORD>(mark.size()),
                      &written, nullptr);
    ::CloseHandle(file);
#else
    Q_UNUSED(localPath);
    Q_UNUSED(originUrl);
#endif
}

} // namespace openscpui
