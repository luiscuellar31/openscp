#include "SessionController.hpp"
#include "TestHarness.hpp"

#include <QCoreApplication>

#include <iostream>

namespace {

void testConnectionCancellation(TestContext &test,
                                openscpui::SessionController &session) {
    auto canceled = std::make_shared<std::atomic<bool>>(false);
    test.check(session.beginConnection(canceled),
               "an idle controller should begin a connection");
    test.check(session.isConnecting(), "connection state should be observable");
    test.check(!session.beginConnection(canceled),
               "a second concurrent connection should be rejected");
    test.check(session.requestConnectionCancellation() && canceled->load(),
               "cancellation should reach the worker's shared flag");
    session.finishConnection();
    test.check(!session.isConnecting() &&
                   !session.requestConnectionCancellation(),
               "finishing should clear connection and cancellation state");
}

void testDisconnectGenerations(TestContext &test,
                               openscpui::SessionController &session) {
    const quint64 first = session.beginDisconnect();
    test.check(session.isCurrentDisconnect(first),
               "the new disconnect generation should be current");
    test.check(session.beginDisconnect() == first,
               "reentrant disconnect requests should share one generation");
    session.finishDisconnect(first);

    const quint64 second = session.beginDisconnect();
    test.check(second > first,
               "a later disconnect should receive a new generation");
    session.finishDisconnect(first);
    test.check(session.isCurrentDisconnect(second),
               "a stale completion must not finish the current disconnect");
    session.finishDisconnect(second);
    test.check(!session.isDisconnecting(),
               "the matching completion should finish the disconnect");
}

void testOptionsLifecycle(TestContext &test,
                          openscpui::SessionController &session) {
    openscp::SessionOptions options;
    options.protocol = openscp::Protocol::WebDav;
    options.host = "dav.example";
    options.webdav_base_path = "/dav/alice";
    session.setOptions(options);
    const auto &activeOptions = session.options();
    test.check(activeOptions.has_value() &&
                   activeOptions->host == "dav.example",
               "active endpoint options should be owned by the session");
    session.clearOptions();
    test.check(!session.options().has_value(),
               "clearing a session should also clear endpoint metadata");
}

} // namespace

int main(int argc, char **argv) {
    QCoreApplication application(argc, argv);
    openscpui::SessionController session;
    TestContext test;
    testConnectionCancellation(test, session);
    testDisconnectGenerations(test, session);
    testOptionsLifecycle(test, session);
    if (test.failures == 0)
        std::cout << "All session controller tests passed\n";
    return test.failures == 0 ? 0 : 1;
}
