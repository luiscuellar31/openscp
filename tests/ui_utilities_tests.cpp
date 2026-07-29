#include "RemotePath.hpp"
#include "UiFormatters.hpp"

#include <QCoreApplication>

#include <iostream>

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

void testRemotePaths(TestContext &test) {
    test.check(normalizeRemotePath(QString()) == QStringLiteral("/"),
               "an empty remote path should normalize to root");
    test.check(normalizeRemotePath(QStringLiteral("folder\\child")) ==
                   QStringLiteral("/folder/child"),
               "remote paths should normalize both separator styles");
    test.check(normalizeRemotePath(QStringLiteral("/one/./two/../three")) ==
                   QStringLiteral("/one/three"),
               "dot segments should be resolved");
    test.check(normalizeRemotePath(QStringLiteral("../../outside")) ==
                   QStringLiteral("/outside"),
               "remote paths should remain confined to the logical root");
    test.check(joinRemotePath(QStringLiteral("/base"), QStringLiteral("a/b")) ==
                   QStringLiteral("/base/a/b"),
               "joining should accept a safe relative path");
    test.check(joinRemotePath(QStringLiteral("/base"),
                              QStringLiteral("../b")) == QStringLiteral("/b"),
               "joining should normalize traversal segments");
}

void testByteFormatting(TestContext &test) {
    test.check(formatByteSize(0) == QStringLiteral("0 B"),
               "zero bytes should use the byte unit");
    test.check(formatByteSize(1536) == QStringLiteral("1.5 KiB"),
               "binary byte sizes should use IEC units");
    test.check(formatTransferRate(1.5) == QStringLiteral("1.5 KiB/s"),
               "transfer rates should reuse byte-size formatting");
    test.check(formatTransferRate(0.0) == QString::fromUtf8("—"),
               "unknown transfer rates should use an em dash");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    TestContext test;
    testRemotePaths(test);
    testByteFormatting(test);
    if (test.failures == 0)
        std::cout << "All UI utility tests passed\n";
    return test.failures == 0 ? 0 : 1;
}
