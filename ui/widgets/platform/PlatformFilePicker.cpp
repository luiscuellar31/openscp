#include "widgets/platform/PlatformFilePicker_p.hpp"

#include <QCoreApplication>
#include <QMessageBox>
#include <QPushButton>

namespace openscpui::detail {

UploadSourceType chooseUploadSourceType(QWidget *parent, const QString &title) {
    QMessageBox choice(parent);
    choice.setIcon(QMessageBox::Question);
    choice.setWindowTitle(title);
    choice.setText(QCoreApplication::translate(
        "PlatformFilePicker", "What would you like to upload?"));

    auto *filesButton = choice.addButton(
        QCoreApplication::translate("PlatformFilePicker", "Files…"),
        QMessageBox::AcceptRole);
    auto *folderButton = choice.addButton(
        QCoreApplication::translate("PlatformFilePicker", "Folder…"),
        QMessageBox::ActionRole);
    choice.addButton(QMessageBox::Cancel);
    choice.exec();

    if (choice.clickedButton() == filesButton) {
        return UploadSourceType::Files;
    }
    if (choice.clickedButton() == folderButton) {
        return UploadSourceType::Folders;
    }
    return UploadSourceType::Canceled;
}

} // namespace openscpui::detail
