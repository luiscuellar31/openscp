#pragma once

#include <QMessageBox>

class QString;
class QWidget;

namespace UiAlerts {
void configure(QMessageBox &box,
               Qt::WindowModality modality = Qt::WindowModal);

QMessageBox::StandardButton
show(QWidget *parent, QMessageBox::Icon icon, const QString &title,
     const QString &text,
     QMessageBox::StandardButtons buttons = QMessageBox::Ok,
     QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);

QMessageBox::StandardButton
information(QWidget *parent, const QString &title, const QString &text,
            QMessageBox::StandardButtons buttons = QMessageBox::Ok,
            QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);

QMessageBox::StandardButton
warning(QWidget *parent, const QString &title, const QString &text,
        QMessageBox::StandardButtons buttons = QMessageBox::Ok,
        QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);

QMessageBox::StandardButton
critical(QWidget *parent, const QString &title, const QString &text,
         QMessageBox::StandardButtons buttons = QMessageBox::Ok,
         QMessageBox::StandardButton defaultButton = QMessageBox::NoButton);

QMessageBox::StandardButton
question(QWidget *parent, const QString &title, const QString &text,
         QMessageBox::StandardButtons buttons =
             QMessageBox::Yes | QMessageBox::No,
         QMessageBox::StandardButton defaultButton = QMessageBox::No);
} // namespace UiAlerts
