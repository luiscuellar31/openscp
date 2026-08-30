#pragma once

#include <QString>
#include <QtGlobal>

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>

namespace openscpui {

class HostKeyPromptCoordinator final {
    public:
    struct Prompt {
        QString host;
        quint16 port = 0;
        QString algorithm;
        QString fingerprint;
        bool canSave = false;
        quint64 requestId = 0;
    };

    using PresentPrompt = std::function<void(const Prompt &)>;

    void setPresentPrompt(PresentPrompt presenter);
    [[nodiscard]] bool requestDecision(Prompt prompt);
    [[nodiscard]] bool resolve(bool accepted);
    void cancel();

    [[nodiscard]] bool hasPendingPrompt() const;
    [[nodiscard]] std::optional<Prompt> pendingPrompt() const;

    private:
    mutable std::mutex mutex_;
    std::condition_variable stateChanged_;
    PresentPrompt presenter_;
    std::optional<Prompt> pending_;
    quint64 nextRequestId_ = 1;
    std::uint64_t cancellationGeneration_ = 0;
    bool active_ = false;
    bool decided_ = false;
    bool accepted_ = false;
};

} // namespace openscpui
