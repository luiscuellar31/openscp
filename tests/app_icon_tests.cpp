#include "TestHarness.hpp"
#include "logic/common/AppSettings.hpp"
#include "widgets/common/AppIcon.hpp"

#include <QGuiApplication>
#include <QIcon>
#include <QSettings>
#include <QSize>
#include <QTemporaryDir>

namespace {

using openscpui::AppIconStyle;

constexpr AppIconStyle kStyles[] = {
    AppIconStyle::Standard, AppIconStyle::MacosLight, AppIconStyle::MacosDark};

OPENSCP_TEST(testStyleKeysReadBack, test) {
    for (const AppIconStyle style : kStyles) {
        test.check(openscpui::appIconStyleFromKey(
                       openscpui::appIconStyleKey(style)) == style,
                   "each style should read back from the value it saves");
    }
}

OPENSCP_TEST(testUnknownStyleReadsAsDefault, test) {
    for (const QString &key : {QString(), QStringLiteral("macos-blue"),
                               QStringLiteral("Standard")}) {
        test.check(openscpui::appIconStyleFromKey(key) ==
                       openscpui::defaultAppIconStyle(),
                   "a missing or unknown value should read as the default");
    }
}

OPENSCP_TEST(testDefaultStyleFollowsPlatform, test) {
#ifdef Q_OS_MACOS
    test.check(openscpui::defaultAppIconStyle() == AppIconStyle::MacosLight,
               "macOS should start with the light tile");
#else
    test.check(openscpui::defaultAppIconStyle() == AppIconStyle::Standard,
               "other platforms should start with the standard icon");
#endif
}

OPENSCP_TEST(testSavedStyle, test) {
    QTemporaryDir directory;
    test.check(directory.isValid(),
               "the saved style should have a temporary settings directory");
    if (!directory.isValid())
        return;
    QSettings settings(directory.filePath(QStringLiteral("settings.ini")),
                       QSettings::IniFormat);

    test.check(openscpui::savedAppIconStyle(settings) ==
                   openscpui::defaultAppIconStyle(),
               "without a saved choice the default style should apply");
    settings.setValue(openscpui::settingskeys::kUiAppIconStyle,
                      openscpui::appIconStyleKey(AppIconStyle::MacosDark));
    test.check(openscpui::savedAppIconStyle(settings) ==
                   AppIconStyle::MacosDark,
               "the saved choice should apply");
}

OPENSCP_TEST(testEveryStyleHasItsArtwork, test) {
    // QIcon skips a file it cannot read, so a missing size would not be
    // available.
    for (const AppIconStyle style : kStyles) {
        const QList<QSize> sizes = openscpui::appIcon(style).availableSizes();
        for (const int size : {16, 24, 32, 48, 64, 128, 256, 512}) {
            test.check(sizes.contains(QSize(size, size)),
                       "each style should embed its artwork at every size");
        }
    }
}

} // namespace

int main(int argc, char **argv) {
    openscp::test::TestHarness harness("application icon");
    return harness.runWithApplication<QGuiApplication>(argc, argv);
}
