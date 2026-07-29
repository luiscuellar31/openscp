// Data-model tests for remote listings. The model must remain independent of
// network clients and expose stable Qt model semantics while controller jobs
// run elsewhere.
#include "RemoteModel.hpp"

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

struct TestContext {
    int failures = 0;

    void check(bool condition, const char *message) {
        if (condition)
            return;
        ++failures;
        std::cerr << "[FAIL] " << message << '\n';
    }
};

openscp::FileInfo entry(std::string name, bool directory, std::uint64_t size,
                        bool hasSize, std::uint64_t mtime,
                        std::uint32_t mode) {
    openscp::FileInfo info;
    info.name = std::move(name);
    info.is_dir = directory;
    info.size = size;
    info.has_size = hasSize;
    info.mtime = mtime;
    info.mode = mode;
    return info;
}

void testLoadingAndReplacement(TestContext &test) {
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

void testFilteringSortingAndMimeData(TestContext &test) {
    RemoteModel model;
    model.setShowHidden(true);
    model.setEntries(
        QStringLiteral("/"),
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

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    TestContext test;
    testLoadingAndReplacement(test);
    testFilteringSortingAndMimeData(test);
    if (test.failures == 0)
        std::cout << "All RemoteModel tests passed\n";
    return test.failures == 0 ? 0 : 1;
}
