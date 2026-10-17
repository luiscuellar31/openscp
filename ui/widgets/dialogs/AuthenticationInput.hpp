#pragma once

#include <QLineEdit>
#include <QString>

#include <stop_token>

class QWidget;

namespace openscpui {

// Worker-only request. The parent must live until its connection worker joins.
// Cancellation does not require the UI to dispatch the queued presentation.
bool requestAuthenticationInput(QWidget *parent, std::stop_token stopToken,
                                const QString &title, const QString &prompt,
                                QLineEdit::EchoMode echoMode, QString &answer);

} // namespace openscpui
