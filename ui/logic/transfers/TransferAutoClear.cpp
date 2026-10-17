#include "logic/transfers/TransferAutoClear.hpp"

#include "logic/common/AppSettings.hpp"

#include <QtGlobal>

namespace {

TransferAutoClearMode validMode(int raw) {
    switch (raw) {
    case static_cast<int>(TransferAutoClearMode::Completed):
        return TransferAutoClearMode::Completed;
    case static_cast<int>(TransferAutoClearMode::FailedCanceled):
        return TransferAutoClearMode::FailedCanceled;
    case static_cast<int>(TransferAutoClearMode::Finished):
        return TransferAutoClearMode::Finished;
    default:
        return TransferAutoClearMode::Off;
    }
}

} // namespace

TransferAutoClearRule storedTransferAutoClearRule(const QSettings &settings) {
    using namespace openscpui::settingskeys;
    const int defaultMode =
        settings
            .value(kTransferDefaultQueueAutoClearMode,
                   static_cast<int>(TransferAutoClearMode::Off))
            .toInt();
    const int defaultMinutes =
        settings.value(kTransferDefaultQueueAutoClearMinutes, 15).toInt();
    TransferAutoClearRule rule;
    rule.mode = validMode(
        settings.value(kTransferQueueAutoClearMode, defaultMode).toInt());
    rule.minutes = qBound(
        1,
        settings.value(kTransferQueueAutoClearMinutes, defaultMinutes).toInt(),
        1440);
    return rule;
}
