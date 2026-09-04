#include "widgets/platform/PlatformFilePicker.hpp"
#include "widgets/platform/PlatformFilePicker_p.hpp"

#include <QCoreApplication>
#include <QFileDialog>

#include <utility>

namespace openscpui {
namespace {

class QtPlatformFilePicker final : public PlatformFilePicker {
    public:
    PlatformFilePickerResult
    selectUploadSources(QWidget *parent, const QString &title,
                        const QString &initialDirectory) override {
        const detail::UploadSourceType sourceType =
            detail::chooseUploadSourceType(parent, title);
        if (sourceType == detail::UploadSourceType::Files) {
            QStringList paths = QFileDialog::getOpenFileNames(
                parent,
                QCoreApplication::translate("PlatformFilePicker",
                                            "Select files to upload"),
                initialDirectory);
            if (paths.isEmpty()) {
                return {PlatformFilePickerResult::Status::Canceled, {}, {}};
            }
            return {PlatformFilePickerResult::Status::Accepted,
                    std::move(paths),
                    {}};
        }
        if (sourceType == detail::UploadSourceType::Folders) {
            const QString folder = QFileDialog::getExistingDirectory(
                parent,
                QCoreApplication::translate("PlatformFilePicker",
                                            "Select a folder to upload"),
                initialDirectory, QFileDialog::ShowDirsOnly);
            if (folder.isEmpty()) {
                return {PlatformFilePickerResult::Status::Canceled, {}, {}};
            }
            return {PlatformFilePickerResult::Status::Accepted, {folder}, {}};
        }
        return {PlatformFilePickerResult::Status::Canceled, {}, {}};
    }
};

} // namespace

std::unique_ptr<PlatformFilePicker> createPlatformFilePicker() {
    return std::make_unique<QtPlatformFilePicker>();
}

} // namespace openscpui
