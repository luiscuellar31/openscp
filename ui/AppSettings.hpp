#pragma once

#include <QSettings>
#include <QString>

namespace openscpui {

namespace settingskeys {

inline constexpr char Sites[] = "sites";
inline constexpr char EnableInsecureSecretFallback[] =
    "Security/enableInsecureSecretFallback";
inline constexpr char MacKeychainRestrictive[] =
    "Security/macKeychainRestrictive";

} // namespace settingskeys

struct SettingsSyncResult {
    bool ok = false;
    QString error;
};

class AppSettings final : public QSettings {
    public:
    enum class Store { Application, SecretFallback };

    explicit AppSettings(Store store = Store::Application);
    AppSettings(const QString &organization, const QString &application);
    AppSettings(const QString &fileName, Format format);
    ~AppSettings() override;

    AppSettings(const AppSettings &) = delete;
    AppSettings &operator=(const AppSettings &) = delete;

    [[nodiscard]] SettingsSyncResult syncSecure();
    [[nodiscard]] SettingsSyncResult ensureOwnerOnly() const;
};

} // namespace openscpui
