#include "HostKeyPromptCoordinator.hpp"

#include <utility>

namespace openscpui {

void HostKeyPromptCoordinator::setPresentPrompt(PresentPrompt presenter) {
    std::lock_guard<std::mutex> lock(mutex_);
    presenter_ = std::move(presenter);
}

bool HostKeyPromptCoordinator::requestDecision(Prompt prompt) {
    PresentPrompt presenter;
    Prompt promptToPresent;
    std::uint64_t cancellationGeneration = 0;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        cancellationGeneration = cancellationGeneration_;
        stateChanged_.wait(lock, [&] {
            return !active_ ||
                   cancellationGeneration_ != cancellationGeneration;
        });
        if (cancellationGeneration_ != cancellationGeneration)
            return false;

        active_ = true;
        decided_ = false;
        accepted_ = false;
        pending_ = std::move(prompt);
        pending_->requestId = nextRequestId_++;
        promptToPresent = *pending_;
        presenter = presenter_;
    }

    if (presenter) {
        presenter(promptToPresent);
    } else {
        (void)resolve(false);
    }

    bool accepted = false;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        stateChanged_.wait(lock, [&] {
            return decided_ ||
                   cancellationGeneration_ != cancellationGeneration;
        });
        accepted = decided_ && accepted_ &&
                   cancellationGeneration_ == cancellationGeneration;
        pending_.reset();
        active_ = false;
        decided_ = false;
        accepted_ = false;
    }
    stateChanged_.notify_all();
    return accepted;
}

bool HostKeyPromptCoordinator::resolve(bool accepted) {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!active_ || decided_)
            return false;
        accepted_ = accepted;
        decided_ = true;
    }
    stateChanged_.notify_all();
    return true;
}

void HostKeyPromptCoordinator::cancel() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        ++cancellationGeneration_;
        pending_.reset();
    }
    stateChanged_.notify_all();
}

bool HostKeyPromptCoordinator::hasPendingPrompt() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_.has_value();
}

std::optional<HostKeyPromptCoordinator::Prompt>
HostKeyPromptCoordinator::pendingPrompt() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return pending_;
}

} // namespace openscpui
