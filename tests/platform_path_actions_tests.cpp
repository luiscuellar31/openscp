#include "PlatformPathActions.hpp"
#include "PlatformPathActions_p.hpp"
#include "TestHarness.hpp"

#include <QCoreApplication>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <iostream>

namespace {

using openscpui::detail::collapseTargetsToDirectories;
using openscpui::detail::PathActionKind;
using openscpui::detail::planRevealBatch;
using openscpui::detail::resolveRevealTarget;

bool writeFile(const QString &path) {
    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate))
        return false;
    file.write("payload");
    file.close();
    return true;
}

OPENSCP_TEST(testResolvesExistingFileToReveal, test) {
    QTemporaryDir root;
    test.check(root.isValid(), "the fixture directory should be created");
    const QString filePath = QDir(root.path()).filePath("report.txt");
    test.check(writeFile(filePath), "the fixture file should be created");

    const auto target = resolveRevealTarget(filePath);
    test.check(target.kind == PathActionKind::Reveal,
               "an existing file should be revealed, not opened");
    test.check(target.path == QFileInfo(filePath).absoluteFilePath(),
               "the resolved path should be absolute");
}

OPENSCP_TEST(testResolvesExistingDirectoryToOpenFolder, test) {
    QTemporaryDir root;
    test.check(root.isValid(), "the fixture directory should be created");

    const auto target = resolveRevealTarget(root.path());
    test.check(target.kind == PathActionKind::OpenFolder,
               "an existing directory should be opened directly");
    test.check(target.path == QFileInfo(root.path()).absoluteFilePath(),
               "the resolved directory should be absolute");
}

OPENSCP_TEST(testResolvesMissingPathToNearestExistingAncestor, test) {
    QTemporaryDir root;
    test.check(root.isValid(), "the fixture directory should be created");
    const QString missing =
        QDir(root.path()).filePath("absent/deeper/still/missing.bin");

    const auto target = resolveRevealTarget(missing);
    test.check(target.kind == PathActionKind::OpenFolder,
               "a missing destination should fall back to a real ancestor");
    test.check(target.path == QFileInfo(root.path()).absoluteFilePath(),
               "the fallback should stop at the nearest existing ancestor");
}

OPENSCP_TEST(testRejectsEmptyPaths, test) {
    const auto empty = resolveRevealTarget(QString());
    test.check(!empty.isResolved(), "empty input should not resolve");
    test.check(empty.path.isEmpty(), "empty input should not report a path");

    const auto planned = planRevealBatch({QString(), QString()});
    test.check(planned.isEmpty(), "empty input should not produce any target");

    const auto noDestinations = openscpui::PlatformPathActions::revealPaths({});
    test.check(noDestinations.failed() &&
                   !noDestinations.error.trimmed().isEmpty(),
               "an empty reveal batch should report a useful failure");

    const auto emptyDestinations =
        openscpui::PlatformPathActions::revealPaths({QString(), QString()});
    test.check(emptyDestinations.failed() &&
                   !emptyDestinations.error.trimmed().isEmpty(),
               "a reveal batch containing only empty paths should fail");

    test.check(openscpui::PlatformPathActions::revealPath(QString()).failed(),
               "revealing one empty path should fail without dispatching");

    test.check(openscpui::PlatformPathActions::openFile(QString()).failed(),
               "opening an empty file path should fail without dispatching");
    test.check(openscpui::PlatformPathActions::openFolder(QString()).failed(),
               "opening an empty folder path should fail without dispatching");

    QTemporaryDir root;
    test.check(root.isValid(), "the fixture directory should be created");
    test.check(openscpui::PlatformPathActions::openFile(root.path()).failed(),
               "openFile should reject directories without dispatching");
}

#if defined(Q_OS_UNIX)
OPENSCP_TEST(testPreservesWhitespaceInFileNames, test) {
    QTemporaryDir root;
    test.check(root.isValid(), "the fixture directory should be created");
    const QString filePath =
        QDir(root.path()).filePath(QStringLiteral(" report .txt "));
    test.check(writeFile(filePath),
               "a file whose name has surrounding spaces should be created");

    const auto target = resolveRevealTarget(filePath);
    test.check(target.kind == PathActionKind::Reveal,
               "a whitespace-bearing file name should be revealed");
    test.check(target.path == filePath,
               "path actions must preserve significant whitespace");
}

OPENSCP_TEST(testResolvesBrokenSymbolicLinkToReveal, test) {
    QTemporaryDir root;
    test.check(root.isValid(), "the fixture directory should be created");
    const QString missingTarget =
        QDir(root.path()).filePath(QStringLiteral("missing-target"));
    const QString linkPath =
        QDir(root.path()).filePath(QStringLiteral("broken-link"));
    test.check(QFile::link(missingTarget, linkPath),
               "the broken symbolic link should be created");
    test.check(QFileInfo(linkPath).isSymLink(),
               "the fixture should remain a symbolic link");

    const auto target = resolveRevealTarget(linkPath);
    test.check(target.kind == PathActionKind::Reveal,
               "a broken symbolic link should be revealable");
    test.check(target.path == linkPath,
               "revealing a link must preserve the link path");
}
#endif

OPENSCP_TEST(testMissingPathSearchIsNotDepthLimited, test) {
    QTemporaryDir root;
    test.check(root.isValid(), "the fixture directory should be created");

    QString missing = root.path();
    for (int index = 0; index < 80; ++index)
        missing = QDir(missing).filePath(QStringLiteral("missing"));
    missing = QDir(missing).filePath(QStringLiteral("payload.bin"));

    const auto target = resolveRevealTarget(missing);
    test.check(target.kind == PathActionKind::OpenFolder,
               "deep missing paths should resolve to an existing ancestor");
    test.check(target.path == QFileInfo(root.path()).absoluteFilePath(),
               "ancestor lookup must continue beyond 64 path components");
}

OPENSCP_TEST(testBatchDeduplicatesRepeatedDestinations, test) {
    QTemporaryDir root;
    test.check(root.isValid(), "the fixture directory should be created");
    const QString filePath = QDir(root.path()).filePath("archive.tar");
    test.check(writeFile(filePath), "the fixture file should be created");

    const auto planned = planRevealBatch({filePath, filePath, filePath});
    test.check(planned.size() == 1,
               "the same destination should only be visited once");
    test.check(planned.first().kind == PathActionKind::Reveal,
               "the deduplicated destination should still be revealed");
}

OPENSCP_TEST(testBatchDropsFoldersAlreadyCoveredByRevealedFiles, test) {
    QTemporaryDir root;
    test.check(root.isValid(), "the fixture directory should be created");
    const QString filePath = QDir(root.path()).filePath("notes.md");
    test.check(writeFile(filePath), "the fixture file should be created");

    const auto planned = planRevealBatch({filePath, root.path()});
    test.check(planned.size() == 1,
               "revealing a file already exposes its parent directory");
    test.check(planned.first().path == QFileInfo(filePath).absoluteFilePath(),
               "the surviving target should be the revealed file");
}

OPENSCP_TEST(testBatchKeepsUnrelatedFolders, test) {
    QTemporaryDir root;
    test.check(root.isValid(), "the fixture directory should be created");
    QDir rootDir(root.path());
    test.check(rootDir.mkpath(QStringLiteral("sibling")),
               "the sibling directory should be created");
    const QString filePath = rootDir.filePath("payload.bin");
    test.check(writeFile(filePath), "the fixture file should be created");
    const QString siblingPath = rootDir.filePath("sibling");

    const auto planned = planRevealBatch({filePath, siblingPath});
    test.check(planned.size() == 2,
               "an unrelated directory should survive the batch plan");
}

OPENSCP_TEST(testCollapseGroupsFilesByParentDirectory, test) {
    QTemporaryDir root;
    test.check(root.isValid(), "the fixture directory should be created");
    QDir rootDir(root.path());
    const QString first = rootDir.filePath("one.txt");
    const QString second = rootDir.filePath("two.txt");
    test.check(writeFile(first) && writeFile(second),
               "both fixture files should be created");

    const auto directories =
        collapseTargetsToDirectories(planRevealBatch({first, second}));
    test.check(directories.size() == 1,
               "two files in one folder should open a single window");
    test.check(directories.first() == QFileInfo(root.path()).absoluteFilePath(),
               "the collapsed target should be the shared parent directory");
}

OPENSCP_TEST(testCollapseSkipsUnresolvedTargets, test) {
    QVector<openscpui::detail::ResolvedPathTarget> targets;
    openscpui::detail::ResolvedPathTarget unresolved;
    unresolved.path = QStringLiteral("/nowhere/at/all");
    targets.push_back(unresolved);

    test.check(collapseTargetsToDirectories(targets).isEmpty(),
               "unresolved targets must not reach the file manager");
}

} // namespace

int main(int argc, char **argv) {
    openscp::test::TestHarness harness("platform path actions");
    return harness.runWithApplication<QCoreApplication>(argc, argv);
}
