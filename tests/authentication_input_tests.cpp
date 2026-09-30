#include "QtTestSupport.hpp"
#include "TestHarness.hpp"
#include "widgets/dialogs/AuthenticationInput.hpp"

#include <QApplication>
#include <QInputDialog>

#include <future>

namespace {
using openscp::testsupport::flushUiEvents;

OPENSCP_TEST(testAuthenticationInputAcceptanceAndCancellation, test) {
    QWidget parent;
    parent.show();
    for (const bool accept : {true, false}) {
        std::stop_source cancellation;
        QString answer;
        auto request = std::async(std::launch::async, [&] {
            return openscpui::requestAuthenticationInput(
                &parent, cancellation.get_token(),
                QStringLiteral("Authentication"), QStringLiteral("OTP"),
                QLineEdit::Password, answer);
        });
        const bool presented = openscp::testsupport::waitUntil([&] {
            const auto *dialog = parent.findChild<QInputDialog *>();
            return dialog && dialog->isVisible();
        });
        test.check(presented,
                   "authentication requests should display on the UI thread");
        auto *dialog = parent.findChild<QInputDialog *>();
        if (presented && accept) {
            dialog->setTextValue(QString::fromUtf8("á123"));
            dialog->accept();
        } else {
            cancellation.request_stop();
        }
        const bool completed = openscp::testsupport::waitUntil([&] {
            return request.wait_for(std::chrono::milliseconds(0)) ==
                   std::future_status::ready;
        });
        test.check(
            completed,
            "accepted or canceled authentication should unblock its worker");
        test.check(request.get() == accept &&
                       (accept ? answer == QString::fromUtf8("á123")
                               : answer.isEmpty()),
                   "accepted UTF-8 answers should survive while cancellation "
                   "returns no secret");
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        flushUiEvents();
    }
}

} // namespace

int main(int argc, char **argv) {
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    openscp::test::TestHarness harness("authentication input");
    return harness.run();
}
