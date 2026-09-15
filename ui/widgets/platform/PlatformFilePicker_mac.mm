#include "widgets/platform/PlatformFilePicker.hpp"

#import <AppKit/AppKit.h>

#include <QDir>
#include <QFileInfo>

#include <utility>

namespace openscpui {
namespace {

class MacPlatformFilePicker final : public PlatformFilePicker {
    public:
    PlatformFilePickerResult
    selectUploadSources(QWidget *parent, const QString &title,
                        const QString &initialDirectory) override {
        static_cast<void>(parent);

        @autoreleasepool {
            NSOpenPanel *panel = [NSOpenPanel openPanel];
            [panel setTitle:title.toNSString()];
            [panel setCanChooseFiles:YES];
            [panel setCanChooseDirectories:YES];
            [panel setAllowsMultipleSelection:YES];
            [panel setResolvesAliases:YES];
            [panel setTreatsFilePackagesAsDirectories:NO];

            const QFileInfo initialInfo(initialDirectory);
            const QString directory = initialInfo.isDir()
                                          ? initialInfo.absoluteFilePath()
                                          : QDir::homePath();
            [panel setDirectoryURL:[NSURL fileURLWithPath:directory.toNSString()
                                              isDirectory:YES]];

            if ([panel runModal] != NSModalResponseOK) {
                return {PlatformFilePickerResult::Status::Canceled, {}, {}};
            }

            QStringList selectedPaths;
            for (NSURL *url in [panel URLs]) {
                if ([url isFileURL]) {
                    selectedPaths.push_back(QString::fromNSString([url path]));
                }
            }
            if (selectedPaths.isEmpty()) {
                return {PlatformFilePickerResult::Status::Canceled, {}, {}};
            }
            return {PlatformFilePickerResult::Status::Accepted,
                    std::move(selectedPaths),
                    {}};
        }
    }
};

} // namespace

std::unique_ptr<PlatformFilePicker> createPlatformFilePicker() {
    return std::make_unique<MacPlatformFilePicker>();
}

} // namespace openscpui
