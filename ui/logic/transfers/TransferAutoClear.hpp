// The automatic cleanup of finished tasks in the transfer queue.
#pragma once

class QSettings;

// Also the value stored under the queue's auto-clear settings keys.
enum class TransferAutoClearMode {
    Off = 0,
    Completed = 1,
    FailedCanceled = 2,
    Finished = 3,
};

struct TransferAutoClearRule {
    TransferAutoClearMode mode = TransferAutoClearMode::Off;
    int minutes = 15;
};

// The rule saved from the transfer queue window, or the defaults from
// Settings until that window saves one.
TransferAutoClearRule storedTransferAutoClearRule(const QSettings &settings);
