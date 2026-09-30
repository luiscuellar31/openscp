#include "widgets/dialogs/AuthenticationInput.hpp"

#include "openscp/SecureString.hpp"

#include <QInputDialog>
#include <QPointer>
#include <QThread>
#include <QTimer>

#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>

namespace openscpui {
namespace {
struct InputReply {
    std::mutex mutex;
    std::condition_variable_any changed;
    bool finished = false;
    std::optional<openscp::SecureString> answer;
};
} // namespace

bool requestAuthenticationInput(QWidget *parent, std::stop_token stopToken,
                                const QString &title, const QString &prompt,
                                QLineEdit::EchoMode echoMode, QString &answer) {
    answer.clear();
    if (!parent || stopToken.stop_requested() ||
        QThread::currentThread() == parent->thread())
        return false;
    const QPointer<QWidget> owner(parent);
    const auto reply = std::make_shared<InputReply>();
    const bool queued = QMetaObject::invokeMethod(
        parent,
        [owner, reply, stopToken, title, prompt, echoMode] {
            if (!owner || stopToken.stop_requested())
                return;
            auto *dialog = new QInputDialog(owner);
            dialog->setAttribute(Qt::WA_DeleteOnClose);
            dialog->setWindowTitle(title);
            dialog->setLabelText(prompt);
            dialog->setTextEchoMode(echoMode);
            dialog->setWindowModality(Qt::WindowModal);
            QObject::connect(
                dialog, &QDialog::finished, dialog,
                [dialog, reply, stopToken](int result) {
                    {
                        std::lock_guard lock(reply->mutex);
                        if (result == QDialog::Accepted &&
                            !stopToken.stop_requested()) {
                            QByteArray bytes = dialog->textValue().toUtf8();
                            reply->answer.emplace(std::string_view(
                                bytes.constData(),
                                static_cast<std::size_t>(bytes.size())));
                            volatile char *data = bytes.data();
                            for (qsizetype i = 0; i < bytes.size(); ++i)
                                data[i] = 0;
                        }
                        reply->finished = true;
                    }
                    dialog->setTextValue(QString());
                    reply->changed.notify_all();
                });
            QObject::connect(dialog, &QObject::destroyed, [reply] {
                {
                    std::lock_guard lock(reply->mutex);
                    reply->finished = true;
                }
                reply->changed.notify_all();
            });
            // Only a visible prompt needs a timer. Worker cancellation uses
            // the stop-aware wait and also works while the UI is joining it.
            auto *timer = new QTimer(dialog);
            QObject::connect(timer, &QTimer::timeout, dialog,
                             [dialog, stopToken] {
                                 if (stopToken.stop_requested())
                                     dialog->reject();
                             });
            timer->start(50);
            dialog->open();
        },
        Qt::QueuedConnection);
    if (!queued)
        return false;
    std::unique_lock lock(reply->mutex);
    reply->changed.wait(lock, stopToken, [&] { return reply->finished; });
    if (stopToken.stop_requested() || !reply->answer) {
        reply->answer.reset();
        return false;
    }
    answer = QString::fromUtf8(reply->answer->data(),
                               static_cast<qsizetype>(reply->answer->size()));
    reply->answer.reset();
    return true;
}
} // namespace openscpui
