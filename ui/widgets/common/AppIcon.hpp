// The application icon: the styles a user can choose and their artwork.
#pragma once

#include <QIcon>
#include <QString>

class QSettings;

namespace openscpui {

enum class AppIconStyle {
    Standard,   // the free-standing Tango drawing
    MacosLight, // the drawing on Apple's icon shape, with a light background
    MacosDark,  // the drawing on Apple's icon shape, with a dark background
};

// The style until the user picks one: the light tile on macOS, where
// application icons share Apple's shape, and the standard icon elsewhere.
[[nodiscard]] AppIconStyle defaultAppIconStyle();

// The value settings keep for a style. An unknown value reads as the default.
[[nodiscard]] QString appIconStyleKey(AppIconStyle style);
[[nodiscard]] AppIconStyle appIconStyleFromKey(const QString &key);
[[nodiscard]] AppIconStyle savedAppIconStyle(const QSettings &settings);

// The icon in `style`, at every size its artwork is drawn at.
[[nodiscard]] QIcon appIcon(AppIconStyle style);

} // namespace openscpui
