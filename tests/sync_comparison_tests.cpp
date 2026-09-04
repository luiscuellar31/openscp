// Pure comparison/filter/planning tests.
#include "SyncComparisonEngine.hpp"
#include "TestHarness.hpp"

#include <QCoreApplication>

#include <optional>

namespace {

SyncSnapshotEntry file(const QString &path, std::optional<quint64> size,
                       std::optional<qint64> modifiedMs, bool reliable = true) {
    SyncSnapshotEntry entry;
    entry.relativePath = path;
    entry.type = SyncEntryType::File;
    entry.size = size;
    entry.modifiedMs = modifiedMs;
    entry.metadataReliable = reliable;
    return entry;
}

SyncSnapshotEntry directory(const QString &path) {
    SyncSnapshotEntry entry;
    entry.relativePath = path;
    entry.type = SyncEntryType::Directory;
    return entry;
}

const SyncComparisonItem *findItem(const QVector<SyncComparisonItem> &items,
                                   const QString &path) {
    for (const SyncComparisonItem &item : items) {
        if (item.relativePath == path)
            return &item;
    }
    return nullptr;
}

OPENSCP_TEST(testPathAndGlobHelpers, test) {
    test.check(SyncComparisonEngine::normalizeRelativePath(QStringLiteral(
                   "./src/../main.cpp")) == QStringLiteral("main.cpp"),
               "relative paths should resolve dot segments");
    test.check(SyncComparisonEngine::normalizeRelativePath(
                   QStringLiteral("../../outside"))
                   .isEmpty(),
               "relative paths must not escape their root");
    test.check(SyncComparisonEngine::normalizeRelativePath(QStringLiteral(
                   "/root/file.txt")) == QStringLiteral("root/file.txt"),
               "snapshot paths should be made root-relative");

    struct GlobCase {
        QString path;
        QString pattern;
        bool expected;
        const char *message;
    };
    const GlobCase globCases[] = {
        {QStringLiteral("src/main.cpp"), QStringLiteral("**/*.cpp"), true,
         "double-star should match nested paths"},
        {QStringLiteral("main.cpp"), QStringLiteral("**/*.cpp"), true,
         "double-star slash should also match the root"},
        {QStringLiteral("assets/icons/open.svg"), QStringLiteral("assets/**"),
         true, "trailing double-star should match descendants"},
        {QStringLiteral("assets"), QStringLiteral("assets/**"), true,
         "trailing double-star should match the folder itself"},
        {QStringLiteral("nested/cache/file.bin"), QStringLiteral("cache"), true,
         "a slashless pattern should match a path segment"},
        {QStringLiteral("nested/readme.md"), QStringLiteral("readme.??"), true,
         "question marks should match one non-separator character"},
        {QStringLiteral("nested/readme.txt"), QStringLiteral("*.cpp"), false,
         "single-star should not match unrelated suffixes"},
    };
    for (const GlobCase &globCase : globCases) {
        test.check(SyncComparisonEngine::globMatches(
                       globCase.path, globCase.pattern) == globCase.expected,
                   globCase.message);
    }

    const QStringList parsed = SyncComparisonEngine::parsePatterns(
        QStringLiteral(" **/*.cpp ; assets/**\n**/*.cpp\r\n "));
    test.check(parsed.size() == 2 &&
                   parsed.at(0) == QStringLiteral("**/*.cpp") &&
                   parsed.at(1) == QStringLiteral("assets/**"),
               "pattern parser should support line/semicolon separators and "
               "deduplicate");
}

OPENSCP_TEST(testComparisonRules, test) {
    QVector<SyncSnapshotEntry> local{
        file(QStringLiteral("same.txt"), 10, 10'000),
        file(QStringLiteral("size.txt"), 20, 10'000),
        file(QStringLiteral("source-newer.txt"), 10, 20'000),
        file(QStringLiteral("destination-newer.txt"), 10, 10'000),
        file(QStringLiteral("unknown.txt"), std::nullopt, 10'000),
        file(QStringLiteral("local-only.txt"), 42, 10'000),
        directory(QStringLiteral("empty")),
        directory(QStringLiteral("type-clash")),
        file(QStringLiteral(".hidden.txt"), 1, 10'000),
        file(QStringLiteral("ignored.tmp"), 1, 10'000),
    };
    QVector<SyncSnapshotEntry> remote{
        file(QStringLiteral("same.txt"), 10, 11'500),
        file(QStringLiteral("size.txt"), 10, 10'000),
        file(QStringLiteral("source-newer.txt"), 10, 10'000),
        file(QStringLiteral("destination-newer.txt"), 10, 20'000),
        file(QStringLiteral("unknown.txt"), 10, 10'000),
        file(QStringLiteral("type-clash"), 10, 10'000),
        file(QStringLiteral("remote-only.txt"), 7, 10'000),
        file(QStringLiteral(".hidden.txt"), 1, 10'000),
        file(QStringLiteral("ignored.tmp"), 2, 10'000),
    };

    SyncComparisonOptions options;
    options.excludePatterns = {QStringLiteral("*.tmp")};
    const auto result = SyncComparisonEngine::compare(local, remote, options);

    test.check(findItem(result, QStringLiteral("same.txt"))->action ==
                   SyncAction::Keep,
               "mtime differences inside two seconds should be equal");
    test.check(findItem(result, QStringLiteral("size.txt"))->action ==
                   SyncAction::Copy,
               "different sizes should propose copy");
    test.check(findItem(result, QStringLiteral("source-newer.txt"))->action ==
                   SyncAction::Copy,
               "a newer source should propose copy");
    test.check(
        findItem(result, QStringLiteral("destination-newer.txt"))->action ==
            SyncAction::Conflict,
        "a newer destination must be a conflict");
    test.check(findItem(result, QStringLiteral("unknown.txt"))->action ==
                   SyncAction::Unknown,
               "missing metadata must not overwrite automatically");
    test.check(findItem(result, QStringLiteral("type-clash"))->action ==
                   SyncAction::Conflict,
               "different entry types must be a conflict");
    test.check(findItem(result, QStringLiteral("local-only.txt"))->action ==
                   SyncAction::Copy,
               "source-only files should be copied");
    test.check(findItem(result, QStringLiteral("empty"))->action ==
                   SyncAction::CreateDirectory,
               "source-only empty folders should be explicit operations");
    test.check(findItem(result, QStringLiteral("remote-only.txt"))->action ==
                   SyncAction::Keep,
               "destination extras should be kept by default");
    test.check(findItem(result, QStringLiteral(".hidden.txt")) == nullptr,
               "hidden items should be filtered by default");
    test.check(findItem(result, QStringLiteral("ignored.tmp")) == nullptr,
               "excluded items should be omitted");

    options.mirror = true;
    const auto mirror = SyncComparisonEngine::compare(local, remote, options);
    test.check(findItem(mirror, QStringLiteral("remote-only.txt"))->action ==
                   SyncAction::DeleteFile,
               "mirror should preview deletion of destination extras");

    options.direction = SyncDirection::RemoteToLocal;
    options.mirror = false;
    const auto reverse = SyncComparisonEngine::compare(local, remote, options);
    test.check(findItem(reverse, QStringLiteral("remote-only.txt"))->action ==
                   SyncAction::Copy,
               "reversing direction should make remote-only files sources");
    test.check(findItem(reverse, QStringLiteral("local-only.txt"))->action ==
                   SyncAction::Keep,
               "reversing direction should keep local destination extras");
}

OPENSCP_TEST(testIncludePatternsPreserveParents, test) {
    QVector<SyncSnapshotEntry> local{
        directory(QStringLiteral("src")),
        directory(QStringLiteral("src/nested")),
        file(QStringLiteral("src/nested/main.cpp"), 10, 10'000),
        file(QStringLiteral("src/nested/readme.md"), 10, 10'000),
    };
    SyncComparisonOptions options;
    options.includePatterns = {QStringLiteral("**/*.cpp")};
    const auto result = SyncComparisonEngine::compare(local, {}, options);
    test.check(findItem(result, QStringLiteral("src")) != nullptr &&
                   findItem(result, QStringLiteral("src/nested")) != nullptr,
               "parents of included files should remain visible for mkdir");
    test.check(findItem(result, QStringLiteral("src/nested/main.cpp")) !=
                   nullptr,
               "matching files should be visible");
    test.check(findItem(result, QStringLiteral("src/nested/readme.md")) ==
                   nullptr,
               "non-matching files should be filtered");
}

OPENSCP_TEST(testExecutionPlanOrdering, test) {
    QVector<SyncSnapshotEntry> local{
        directory(QStringLiteral("incoming")),
        file(QStringLiteral("incoming/new.bin"), 128, 20'000),
    };
    QVector<SyncSnapshotEntry> remote{
        directory(QStringLiteral("old")),
        directory(QStringLiteral("old/nested")),
        file(QStringLiteral("old/nested/file.bin"), 64, 10'000),
    };
    SyncComparisonOptions options;
    options.mirror = true;
    const auto comparison =
        SyncComparisonEngine::compare(local, remote, options);
    const SyncExecutionPlan plan =
        SyncComparisonEngine::makeExecutionPlan(comparison, options);

    test.check(plan.directoriesToCreate.size() == 1 &&
                   plan.directoriesToCreate.front() ==
                       QStringLiteral("incoming"),
               "execution plan should create source-only folders");
    test.check(plan.copies.size() == 1 &&
                   plan.copies.front().relativePath ==
                       QStringLiteral("incoming/new.bin") &&
                   plan.knownCopyBytes == 128,
               "execution plan should include selected copies and byte totals");
    test.check(plan.deletes.size() == 3,
               "mirror plan should include every selected destination extra");
    test.check(plan.deletes.at(0).relativePath ==
                       QStringLiteral("old/nested/file.bin") &&
                   plan.deletes.at(1).relativePath ==
                       QStringLiteral("old/nested") &&
                   plan.deletes.at(2).relativePath == QStringLiteral("old"),
               "mirror deletions must be ordered children before parents");
    test.check(plan.requiresMirrorConfirmation,
               "a mirror plan with deletes must require confirmation");

    auto deselected = comparison;
    for (SyncComparisonItem &item : deselected) {
        if (item.action == SyncAction::Copy)
            item.selected = false;
    }
    const auto withoutCopy =
        SyncComparisonEngine::makeExecutionPlan(deselected, options);
    test.check(withoutCopy.copies.isEmpty(),
               "unchecked preview rows must not enter the execution plan");
}

OPENSCP_TEST(testChecksumComparison, test) {
    SyncSnapshotEntry local = file(QStringLiteral("verified.bin"), 10, 20'000);
    local.checksumAlgorithm = QStringLiteral("SHA-256");
    local.checksum = QByteArrayLiteral("same");
    SyncSnapshotEntry remote = file(QStringLiteral("verified.bin"), 99, 1'000);
    remote.checksumAlgorithm = QStringLiteral("sha-256");
    remote.checksum = QByteArrayLiteral("same");

    const auto result = SyncComparisonEngine::compare({local}, {remote});
    test.check(result.size() == 1 && result.front().action == SyncAction::Keep,
               "matching on-demand checksums should be authoritative");

    remote.size = local.size;
    remote.modifiedMs = local.modifiedMs;
    remote.checksum = QByteArrayLiteral("different");
    const auto mismatch = SyncComparisonEngine::compare({local}, {remote});
    test.check(mismatch.size() == 1 &&
                   mismatch.front().action == SyncAction::Conflict,
               "different checksums with equivalent metadata should be a "
               "visible conflict");
}

} // namespace

int main(int argc, char **argv) {
    openscp::test::TestHarness harness("sync comparison");
    return harness.runWithApplication<QCoreApplication>(argc, argv);
}
