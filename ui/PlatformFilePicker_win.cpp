#include "PlatformFilePicker.hpp"
#include "PlatformFilePicker_p.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
// Windows SDK headers must precede Qt headers so NOMINMAX is effective and
// COM/Shell declarations see their foundational Win32 types.
// clang-format off
#include <windows.h>
#include <shobjidl.h>
// clang-format on

#include <QCoreApplication>
#include <QWidget>

#include <cstdint>
#include <utility>

namespace openscpui {
namespace {

template <typename Interface> class ComPtr final {
    public:
    ComPtr() = default;
    ComPtr(const ComPtr &) = delete;
    ComPtr &operator=(const ComPtr &) = delete;

    ~ComPtr() { reset(); }

    [[nodiscard]] Interface *get() const { return value_; }

    [[nodiscard]] Interface **put() {
        reset();
        return &value_;
    }

    [[nodiscard]] Interface *operator->() const { return value_; }

    void reset() {
        if (value_ != nullptr) {
            value_->Release();
            value_ = nullptr;
        }
    }

    private:
    Interface *value_ = nullptr;
};

class ComApartment final {
    public:
    ComApartment()
        : result_(CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED |
                                              COINIT_DISABLE_OLE1DDE)),
          initialized_(SUCCEEDED(result_)) {}

    ComApartment(const ComApartment &) = delete;
    ComApartment &operator=(const ComApartment &) = delete;

    ~ComApartment() {
        if (initialized_) {
            CoUninitialize();
        }
    }

    [[nodiscard]] HRESULT result() const { return result_; }

    private:
    HRESULT result_ = E_FAIL;
    bool initialized_ = false;
};

QString hresultCode(HRESULT result) {
    const auto value = static_cast<std::uint32_t>(result);
    return QStringLiteral("0x%1").arg(value, 8, 16, QLatin1Char('0')).toUpper();
}

PlatformFilePickerResult failedResult(HRESULT result) {
    return {PlatformFilePickerResult::Status::Failed,
            {},
            QCoreApplication::translate(
                "PlatformFilePicker",
                "The native file picker could not be opened (error %1).")
                .arg(hresultCode(result))};
}

HRESULT createShellItem(const QString &path, ComPtr<IShellItem> &item) {
    if (path.isEmpty()) {
        return E_INVALIDARG;
    }
    return SHCreateItemFromParsingName(reinterpret_cast<PCWSTR>(path.utf16()),
                                       nullptr, IID_IShellItem,
                                       reinterpret_cast<void **>(item.put()));
}

HWND ownerWindow(QWidget *parent) {
    if (parent == nullptr) {
        return GetActiveWindow();
    }
    return reinterpret_cast<HWND>(parent->window()->winId());
}

class WindowsPlatformFilePicker final : public PlatformFilePicker {
    public:
    PlatformFilePickerResult
    selectUploadSources(QWidget *parent, const QString &title,
                        const QString &initialDirectory) override {
        const detail::UploadSourceType sourceType =
            detail::chooseUploadSourceType(parent, title);
        if (sourceType == detail::UploadSourceType::Canceled) {
            return {PlatformFilePickerResult::Status::Canceled, {}, {}};
        }

        const ComApartment apartment;
        if (FAILED(apartment.result())) {
            return failedResult(apartment.result());
        }

        ComPtr<IFileOpenDialog> dialog;
        HRESULT result = CoCreateInstance(
            CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
            IID_IFileOpenDialog, reinterpret_cast<void **>(dialog.put()));
        if (FAILED(result)) {
            return failedResult(result);
        }

        DWORD options = 0;
        result = dialog->GetOptions(&options);
        if (FAILED(result)) {
            return failedResult(result);
        }
        options |= FOS_ALLOWMULTISELECT | FOS_FORCEFILESYSTEM |
                   FOS_PATHMUSTEXIST | FOS_NOCHANGEDIR |
                   FOS_NODEREFERENCELINKS | FOS_DONTADDTORECENT;
        if (sourceType == detail::UploadSourceType::Folders) {
            options |= FOS_PICKFOLDERS;
        } else {
            options |= FOS_FILEMUSTEXIST;
        }
        result = dialog->SetOptions(options);
        if (FAILED(result)) {
            return failedResult(result);
        }

        if (!title.isEmpty()) {
            result = dialog->SetTitle(reinterpret_cast<LPCWSTR>(title.utf16()));
            if (FAILED(result)) {
                return failedResult(result);
            }
        }

        ComPtr<IShellItem> initialFolder;
        if (SUCCEEDED(createShellItem(initialDirectory, initialFolder))) {
            static_cast<void>(dialog->SetFolder(initialFolder.get()));
        }

        result = dialog->Show(ownerWindow(parent));
        if (result == HRESULT_FROM_WIN32(ERROR_CANCELLED)) {
            return {PlatformFilePickerResult::Status::Canceled, {}, {}};
        }
        if (FAILED(result)) {
            return failedResult(result);
        }

        ComPtr<IShellItemArray> selectedItems;
        result = dialog->GetResults(selectedItems.put());
        if (FAILED(result)) {
            return failedResult(result);
        }

        DWORD count = 0;
        result = selectedItems->GetCount(&count);
        if (FAILED(result)) {
            return failedResult(result);
        }

        QStringList paths;
        paths.reserve(static_cast<qsizetype>(count));
        for (DWORD index = 0; index < count; ++index) {
            ComPtr<IShellItem> item;
            result = selectedItems->GetItemAt(index, item.put());
            if (FAILED(result)) {
                return failedResult(result);
            }

            PWSTR path = nullptr;
            result = item->GetDisplayName(SIGDN_FILESYSPATH, &path);
            if (FAILED(result)) {
                return failedResult(result);
            }
            if (path != nullptr) {
                paths.push_back(QString::fromWCharArray(path));
                CoTaskMemFree(path);
            }
        }

        if (paths.isEmpty()) {
            return {PlatformFilePickerResult::Status::Canceled, {}, {}};
        }
        return {
            PlatformFilePickerResult::Status::Accepted, std::move(paths), {}};
    }
};

} // namespace

std::unique_ptr<PlatformFilePicker> createPlatformFilePicker() {
    return std::make_unique<WindowsPlatformFilePicker>();
}

} // namespace openscpui
