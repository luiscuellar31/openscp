#pragma once

#include <QString>
#include <QStringList>

#include <memory>

class QWidget;

namespace openscpui {

struct PlatformFilePickerResult {
    enum class Status { Accepted, Canceled, Failed };

    Status status = Status::Canceled;
    QStringList paths;
    QString errorMessage;

    [[nodiscard]] bool failed() const noexcept {
        return status == Status::Failed;
    }
};

// Selects local upload sources using the best native experience available on
// the current platform. Callers remain responsible for validating the returned
// paths immediately before using them. This interface must be called from the
// GUI thread.
class PlatformFilePicker {
    public:
    virtual ~PlatformFilePicker() = default;

    [[nodiscard]] virtual PlatformFilePickerResult
    selectUploadSources(QWidget *parent, const QString &title,
                        const QString &initialDirectory) = 0;
};

[[nodiscard]] std::unique_ptr<PlatformFilePicker> createPlatformFilePicker();

} // namespace openscpui
