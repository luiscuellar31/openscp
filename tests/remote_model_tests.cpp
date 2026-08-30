// Data-model tests for remote listings. The model must remain independent of
// network clients and expose stable Qt model semantics while controller jobs
// run elsewhere.
#include "RemoteModel.hpp"
#include "TestHarness.hpp"

#include <QCoreApplication>
#include <QMimeData>
#include <QUrl>

#include <cstdint>
#include <iostream>
#include <memory>
#include <string>
#include <utility>
#include <vector>

namespace {

openscp::FileInfo entry(std::string name, bool directory, std::uint64_t size,
                        bool hasSize, std::uint64_t mtime, std::uint32_t mode) {
    openscp::FileInfo info;
    info.name = std::move(name);
    info.is_dir = directory;
    info.size = size;
    info.has_size = hasSize;
    info.mtime = mtime;
    info.mode = mode;
    return info;
}

OPENSCP_TEST(testLoadingAndReplacement, test) {
    RemoteModel model;
    int loadSignals = 0;
    QString loadedPath;
    QObject::connect(
        &model, &RemoteModel::rootPathLoaded,
        [&](const QString &path, bool ok, const QString &error) {
            ++loadSignals;
            loadedPath = path;
            test.check(ok && error.isEmpty(),
                       "successful data replacement should report success");
        });

    model.setLoading(QStringLiteral("//incoming/./nightly"));
    test.check(model.isLoading() && model.rowCount() == 1,
               "loading state should expose one placeholder row");
    test.check(model.data(model.index(0, 0), Qt::DisplayRole).toString() ==
                   QStringLiteral("Loading…"),
               "loading state should expose a readable placeholder");
    test.check(model.flags(model.index(0, 0)) == Qt::ItemIsEnabled,
               "loading placeholder must not be selectable or draggable");

    const std::vector<openscp::FileInfo> entries{
        entry("zeta.txt", false, 4096, true, 1'700'000'000, 0100644),
        entry("folder", true, 0, false, 1'700'000'001, 0040755),
        entry(".hidden", false, 1, true, 1'700'000'002, 0100600),
        entry("unknown.bin", false, 0, false, 0, 0100644),
    };
    model.setEntries(QStringLiteral("//incoming/./nightly"), entries);

    test.check(!model.isLoading() && model.rowCount() == 3,
               "entry replacement should clear loading and filter hidden rows");
    test.check(model.rootPath() == QStringLiteral("/incoming/nightly") &&
                   loadedPath == model.rootPath() && loadSignals == 1,
               "entry replacement should normalize and announce its root");
    test.check(model.nameAt(model.index(0, 0)) == QStringLiteral("folder") &&
                   model.isDir(model.index(0, 0)),
               "directories should sort before files");
    test.check(model.data(model.index(0, 3), Qt::DisplayRole).toString() ==
                   QStringLiteral("drwxr-xr-x"),
               "permission text should preserve the remote mode");
    test.check(model.nameAt(model.index(1, 0)) ==
                       QStringLiteral("unknown.bin") &&
                   !model.hasSize(model.index(1, 0)),
               "unknown file sizes should remain distinguishable from zero");
    test.check(model.data(model.index(1, 1), Qt::DisplayRole).toString() ==
                   QString::fromUtf8("—"),
               "unknown sizes should have an explicit display value");
    test.check(model.flags(model.index(2, 0)).testFlag(Qt::ItemIsDragEnabled),
               "loaded entries should be draggable");
}

OPENSCP_TEST(testFilteringSortingAndMimeData, test) {
    RemoteModel model;
    model.setShowHidden(true);
    model.setEntries(QStringLiteral("/"),
                     {entry("small", false, 1, true, 0, 0100644),
                      entry("large", false, 50, true, 0, 0100644),
                      entry(".config", false, 10, true, 0, 0100600),
                      entry("dir", true, 0, false, 0, 0040755)});

    test.check(model.rowCount() == 4,
               "show-hidden should retain dot-prefixed entries");
    model.sort(1, Qt::DescendingOrder);
    test.check(model.isDir(model.index(0, 0)),
               "directories should remain grouped first for every sort");
    test.check(model.nameAt(model.index(1, 0)) == QStringLiteral("large") &&
                   model.sizeAt(model.index(1, 0)) == 50,
               "size sorting should order files without losing metadata");

    std::unique_ptr<QMimeData> mime(
        model.mimeData({model.index(1, 0), model.index(1, 1)}));
    test.check(mime != nullptr && mime->urls().isEmpty(),
               "the data-only model must not synchronously stage drag files");
    test.check(model.mimeTypes() ==
                   QStringList{QStringLiteral("text/uri-list")},
               "remote drags should advertise native file URLs");
}

OPENSCP_TEST(testLargeListingsReusePrecomputedDisplayData, test) {
    RemoteModel model;
    std::vector<openscp::FileInfo> entries;
    entries.reserve(10'000);
    for (int index = 0; index < 10'000; ++index) {
        entries.push_back(entry("file-" + std::to_string(index) + ".dat", false,
                                static_cast<std::uint64_t>(index) + 1, true,
                                1'700'000'000, 0100640));
    }
    model.setEntries(QStringLiteral("/large"), entries);

    test.check(model.rowCount() == 10'000,
               "large remote listings should retain every visible entry");
    for (int row = 0; row < model.rowCount(); row += 97) {
        test.check(!model.data(model.index(row, 1), Qt::DisplayRole)
                        .toString()
                        .isEmpty(),
                   "large listing sizes should remain formatted");
        test.check(
            model.data(model.index(row, 3), Qt::DisplayRole).toString() ==
                QStringLiteral("-rw-r-----"),
            "large listings should reuse precomputed permissions");
    }
}

} // namespace

int main(int argc, char **argv) {
    openscp::test::TestHarness harness("RemoteModel");
    return harness.runWithApplication<QCoreApplication>(argc, argv);
}
