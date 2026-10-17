#include "widgets/common/AppIcon.hpp"

#include "logic/common/AppSettings.hpp"

#include <QSettings>
#include <QSize>

#include <array>

namespace openscpui {
namespace {

constexpr std::array kStyles{AppIconStyle::Standard, AppIconStyle::MacosLight,
                             AppIconStyle::MacosDark};

// The sizes scripts/icons generates; CMakeLists.txt embeds each of them.
constexpr std::array kSizes{16, 24, 32, 48, 64, 128, 256, 512};

} // namespace

AppIconStyle defaultAppIconStyle() {
#ifdef Q_OS_MACOS
    return AppIconStyle::MacosLight;
#else
    return AppIconStyle::Standard;
#endif
}

QString appIconStyleKey(AppIconStyle style) {
    switch (style) {
    case AppIconStyle::Standard:
        return QStringLiteral("standard");
    case AppIconStyle::MacosLight:
        return QStringLiteral("macos-light");
    case AppIconStyle::MacosDark:
        return QStringLiteral("macos-dark");
    }
    return QStringLiteral("standard");
}

AppIconStyle appIconStyleFromKey(const QString &key) {
    for (const AppIconStyle style : kStyles) {
        if (key == appIconStyleKey(style))
            return style;
    }
    return defaultAppIconStyle();
}

AppIconStyle savedAppIconStyle(const QSettings &settings) {
    return appIconStyleFromKey(
        settings.value(settingskeys::kUiAppIconStyle).toString());
}

QIcon appIcon(AppIconStyle style) {
    // Each style's artwork is embedded under its folder's repository path.
    QIcon icon;
    for (const int size : kSizes) {
        icon.addFile(QStringLiteral(":/assets/icons/app/%1/openscp-%2.png")
                         .arg(appIconStyleKey(style), QString::number(size)),
                     QSize(size, size));
    }
    return icon;
}

} // namespace openscpui
