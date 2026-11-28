#include "QtTestSupport.hpp"
#include "TestHarness.hpp"
#include "logic/common/UiAlerts.hpp"
#include "widgets/dialogs/AuthenticationInput.hpp"

#include <QApplication>
#include <QInputDialog>
#include <QLabel>
#include <QProgressDialog>
#include <QTextDocumentFragment>

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

OPENSCP_TEST(testServerPromptShowsAsText, test) {
    // The prompt was laid out as markup, so a server could style it as one
    // of OpenSCP's own messages.
    QWidget parent;
    parent.show();
    std::stop_source cancellation;
    QString answer;
    const QString prompt = QStringLiteral("<b>OpenSCP</b> needs your password");
    auto request = std::async(std::launch::async, [&] {
        return openscpui::requestAuthenticationInput(
            &parent, cancellation.get_token(), QStringLiteral("Authentication"),
            prompt, QLineEdit::Password, answer);
    });
    const bool presented = openscp::testsupport::waitUntil([&] {
        const auto *dialog = parent.findChild<QInputDialog *>();
        return dialog && dialog->isVisible();
    });
    const auto *dialog = parent.findChild<QInputDialog *>();
    const QString shown =
        dialog
            ? QTextDocumentFragment::fromHtml(dialog->labelText()).toPlainText()
            : QString();
    test.check(presented && shown == prompt,
               "the server's prompt should show as the text it is: " +
                   shown.toStdString());
    cancellation.request_stop();
    (void)openscp::testsupport::waitUntil([&] {
        return request.wait_for(std::chrono::milliseconds(0)) ==
               std::future_status::ready;
    });
    (void)request.get();
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    flushUiEvents();
}

OPENSCP_TEST(testProgressLabelShowsPathsAsText, test) {
    // The delete, permission and search progress name the remote path they
    // are at, which a label read as markup when it began with a tag.
    QProgressDialog dialog(QStringLiteral("Deleting…"),
                           QStringLiteral("Cancel"), 0, 0);
    UiAlerts::showLabelAsText(dialog);
    const QString text = QStringLiteral("Deleting <b>x\nRemoved: 0");
    dialog.setLabelText(text);
    const auto *label = dialog.findChild<QLabel *>();
    test.check(label && label->textFormat() == Qt::PlainText &&
                   label->text() == text && dialog.labelText() == text,
               "the progress label should show a path as written");
}

} // namespace

int main(int argc, char **argv) {
    QApplication application(argc, argv);
    application.setQuitOnLastWindowClosed(false);
    openscp::test::TestHarness harness("authentication input");
    return harness.run(argc, argv);
}
