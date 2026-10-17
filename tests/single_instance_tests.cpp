#include "TestHarness.hpp"
#include "logic/common/SingleInstance.hpp"

#include <QCoreApplication>
#include <QDeadlineTimer>
#include <QDir>
#include <QLocalSocket>
#include <QLockFile>
#include <QProcess>
#include <QProcessEnvironment>
#include <QTemporaryDir>

#include <chrono>
#include <functional>

namespace {

using openscpui::SingleInstance;

// With this variable set, the test program acts as a later launch of
// OpenSCP for that data folder and exits with the role it got.
constexpr char kLaterLaunchFolder[] = "OPENSCP_SINGLE_INSTANCE_TEST_FOLDER";

// Lets the primary's local server handle what has arrived, until `done`
// holds or ten seconds pass.
bool processEventsUntil(const std::function<bool()> &done) {
    QDeadlineTimer deadline(10000);
    while (!done() && !deadline.hasExpired())
        QCoreApplication::processEvents(QEventLoop::AllEvents, 50);
    return done();
}

OPENSCP_TEST(testSecondLaunchAsksTheFirstToShowItsWindow, test) {
    QTemporaryDir folder;
    test.check(folder.isValid(), "the test should have a data folder");
    SingleInstance first(folder.path());
    int activations = 0;
    QObject::connect(&first, &SingleInstance::activationRequested,
                     [&activations] { ++activations; });
    test.check(first.start() == SingleInstance::Role::Primary,
               "the first launch should run OpenSCP");

    // A separate process: on Windows the message waits in the pipe until
    // the first launch reads it, which this thread does below.
    QProcess later;
    QProcessEnvironment environment = QProcessEnvironment::systemEnvironment();
    environment.insert(QString::fromLatin1(kLaterLaunchFolder), folder.path());
    later.setProcessEnvironment(environment);
    later.start(QCoreApplication::applicationFilePath(), {});
    test.check(processEventsUntil([&] {
                   return later.state() == QProcess::NotRunning &&
                          activations == 1;
               }),
               "the first launch should be asked to show its window");
    test.check(later.exitStatus() == QProcess::NormalExit &&
                   later.exitCode() ==
                       static_cast<int>(SingleInstance::Role::Forwarded),
               "the second launch should hand over to the first and quit");
}

OPENSCP_TEST(testClosingTheFirstLetsTheNextOneRun, test) {
    QTemporaryDir folder;
    test.check(folder.isValid(), "the test should have a data folder");
    {
        SingleInstance first(folder.path());
        test.check(first.start() == SingleInstance::Role::Primary,
                   "the first launch should run OpenSCP");
    }
    SingleInstance next(folder.path());
    test.check(next.start() == SingleInstance::Role::Primary,
               "closing OpenSCP should release its data folder");
}

OPENSCP_TEST(testOnlyTheActivationMessageCounts, test) {
    QTemporaryDir folder;
    test.check(folder.isValid(), "the test should have a data folder");
    SingleInstance first(folder.path());
    int activations = 0;
    QObject::connect(&first, &SingleInstance::activationRequested,
                     [&activations] { ++activations; });
    test.check(first.start() == SingleInstance::Role::Primary,
               "the first launch should run OpenSCP");

    const QString name = SingleInstance::serverName(folder.path());
    QLocalSocket unknown;
    unknown.connectToServer(name);
    QLocalSocket oversized;
    oversized.connectToServer(name);
    test.check(unknown.waitForConnected(2000) &&
                   oversized.waitForConnected(2000),
               "other programs of the user can reach the local server");
    unknown.write("openscp-quit\n");
    unknown.flush();
    oversized.write(QByteArray(200, 'x'));
    oversized.flush();

    // The server closes both connections without acting on them.
    test.check(processEventsUntil([&] {
                   return unknown.state() == QLocalSocket::UnconnectedState &&
                          oversized.state() == QLocalSocket::UnconnectedState;
               }),
               "the server should close connections it does not understand");
    test.check(activations == 0,
               "only the activation message should show the window");
}

OPENSCP_TEST(testSilentLockHolderIsReported, test) {
    // A process holds the data folder but does not answer, as while it is
    // still starting.
    QTemporaryDir folder;
    test.check(folder.isValid(), "the test should have a data folder");
    QLockFile holder(
        QDir(folder.path()).filePath(QStringLiteral("instance.lock")));
    test.check(holder.tryLock(0), "the test should hold the lock");

    SingleInstance late(folder.path());
    test.check(late.start(std::chrono::milliseconds(300)) ==
                   SingleInstance::Role::Unreachable,
               "a launch that cannot reach the lock holder should say so");
}

} // namespace

int main(int argc, char **argv) {
    const QString laterLaunchFolder = qEnvironmentVariable(kLaterLaunchFolder);
    if (!laterLaunchFolder.isEmpty()) {
        QCoreApplication app(argc, argv);
        SingleInstance later(laterLaunchFolder);
        return static_cast<int>(later.start(std::chrono::seconds(5)));
    }

    openscp::test::TestHarness harness("single instance");
    return harness.runWithApplication<QCoreApplication>(argc, argv);
}
