#include "UiAlerts.hpp"

#include <QDialog>

namespace UiAlerts {
namespace {
QMessageBox::StandardButton
resolveDefaultButton(QMessageBox::StandardButtons buttons,
                     QMessageBox::StandardButton requested) {
    if (requested != QMessageBox::NoButton && buttons.testFlag(requested))
        return requested;

    if (buttons.testFlag(QMessageBox::No))
        return QMessageBox::No;
    if (buttons.testFlag(QMessageBox::Cancel))
        return QMessageBox::Cancel;
    if (buttons.testFlag(QMessageBox::Ok))
        return QMessageBox::Ok;
    if (buttons.testFlag(QMessageBox::Yes))
        return QMessageBox::Yes;
    if (buttons.testFlag(QMessageBox::Close))
        return QMessageBox::Close;

    return QMessageBox::NoButton;
}
} // namespace

void configure(QMessageBox &box, Qt::WindowModality modality) {
    box.setTextFormat(Qt::PlainText);
    box.setWindowModality(modality);
}

QMessageBox::StandardButton
show(QWidget *parent, QMessageBox::Icon icon, const QString &title,
     const QString &text, QMessageBox::StandardButtons buttons,
     QMessageBox::StandardButton defaultButton) {
    QMessageBox box(parent);
    configure(box);
    box.setIcon(icon);
    box.setWindowTitle(title);
    box.setText(text);
    box.setStandardButtons(buttons);

    const auto resolvedDefault = resolveDefaultButton(buttons, defaultButton);
    if (resolvedDefault != QMessageBox::NoButton)
        box.setDefaultButton(resolvedDefault);

    const int rc = box.exec();
    return static_cast<QMessageBox::StandardButton>(rc);
}

QMessageBox::StandardButton
information(QWidget *parent, const QString &title, const QString &text,
            QMessageBox::StandardButtons buttons,
            QMessageBox::StandardButton defaultButton) {
    return show(parent, QMessageBox::Information, title, text, buttons,
                defaultButton);
}

QMessageBox::StandardButton
warning(QWidget *parent, const QString &title, const QString &text,
        QMessageBox::StandardButtons buttons,
        QMessageBox::StandardButton defaultButton) {
    return show(parent, QMessageBox::Warning, title, text, buttons,
                defaultButton);
}

QMessageBox::StandardButton
critical(QWidget *parent, const QString &title, const QString &text,
         QMessageBox::StandardButtons buttons,
         QMessageBox::StandardButton defaultButton) {
    return show(parent, QMessageBox::Critical, title, text, buttons,
                defaultButton);
}

QMessageBox::StandardButton
question(QWidget *parent, const QString &title, const QString &text,
         QMessageBox::StandardButtons buttons,
         QMessageBox::StandardButton defaultButton) {
    return show(parent, QMessageBox::Question, title, text, buttons,
                defaultButton);
}
} // namespace UiAlerts
