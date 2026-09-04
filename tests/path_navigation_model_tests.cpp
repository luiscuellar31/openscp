#include "TestHarness.hpp"
#include "logic/navigation/PathNavigationModel.hpp"

#include <QCoreApplication>
#include <QDir>

namespace {

using openscpui::PathFlavor;

OPENSCP_TEST(testRemoteSegments, test) {
    const auto segments = openscpui::buildPathSegments(
        QStringLiteral("//srv/./releases/../incoming/nightly"),
        PathFlavor::Remote);
    test.check(segments.size() == 4,
               "remote path should include root and every normalized segment");
    test.check(segments.at(0).label == QStringLiteral("/") &&
                   segments.at(0).target == QStringLiteral("/"),
               "remote segments should start at logical root");
    test.check(segments.at(2).label == QStringLiteral("incoming") &&
                   segments.at(2).target == QStringLiteral("/srv/incoming"),
               "remote parent segment should retain its complete target");
    test.check(segments.back().target ==
                   QStringLiteral("/srv/incoming/nightly"),
               "remote current segment should target the normalized path");
    const QString normalized = segments.back().target;
    const auto &incoming = segments.at(2);
    test.check(normalized.mid(incoming.displayStart,
                              incoming.displayEnd - incoming.displayStart) ==
                   incoming.label,
               "remote segment ranges should map to the flat path text");
}

OPENSCP_TEST(testLocalSegments, test) {
    const QString path = QDir::cleanPath(
        QDir::tempPath() + QStringLiteral("/openscp/navigation/deep"));
    const auto segments = openscpui::buildPathSegments(path, PathFlavor::Local);
#ifdef Q_OS_WIN
    test.check(!segments.isEmpty() &&
                   (segments.front().target.endsWith(QStringLiteral(":/")) ||
                    segments.front().target.startsWith(QStringLiteral("//"))),
               "local Windows segments should start at a drive or share root");
#else
    test.check(!segments.isEmpty() &&
                   segments.front().target == QStringLiteral("/"),
               "local POSIX segments should start at filesystem root");
#endif
    test.check(segments.back().label == QStringLiteral("deep") &&
                   segments.back().target == path,
               "local current segment should preserve the complete path");
    test.check(segments.at(segments.size() - 2).target ==
                   QDir::cleanPath(path + QStringLiteral("/..")),
               "local parent segment should navigate exactly one level up");
    const auto &current = segments.back();
    test.check(path.mid(current.displayStart,
                        current.displayEnd - current.displayStart) ==
                   current.label,
               "local segment ranges should map to the flat path text");
}

OPENSCP_TEST(testEmptyLocalPathUsesHome, test) {
    const auto segments =
        openscpui::buildPathSegments(QString(), PathFlavor::Local);
    test.check(!segments.isEmpty() &&
                   segments.back().target == QDir::cleanPath(QDir::homePath()),
               "an empty local path should resolve to the user's home folder");
}

} // namespace

int main(int argc, char **argv) {
    openscp::test::TestHarness harness("path navigation model");
    return harness.runWithApplication<QCoreApplication>(argc, argv);
}
