// Thread-safe, batch-scoped conflict policy coordination.
#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <unordered_map>

enum class TransferConflictPolicy {
    Ask,
    Overwrite,
    Skip,
    Resume,
    Rename,
    NewerOnly
};

struct ConflictRequest {
    std::uint64_t batchId = 0;
    bool allowResume = false;
    std::optional<std::int64_t> sourceMtime;
    std::optional<std::int64_t> destinationMtime;
};

struct ConflictResolution {
    TransferConflictPolicy policy = TransferConflictPolicy::Skip;
    bool applyToRemaining = false;
    bool canceled = false;
};

// Serializes conflict resolution within each batch. A decision marked
// "apply to remaining" is stored before the next waiter is released, so
// concurrent conflicts in that batch require only one user interaction.
class ConflictCoordinator {
    public:
    using Resolver = std::function<ConflictResolution(const ConflictRequest &)>;
    using CancelCheck = std::function<bool()>;

    void ensureBatchPolicy(std::uint64_t batchId,
                           TransferConflictPolicy policy) {
        const auto state = stateFor(batchId);
        std::lock_guard<std::timed_mutex> lock(state->mutex);
        if (!state->configured) {
            state->policy = policy;
            state->configured = true;
        }
    }

    void setBatchPolicy(std::uint64_t batchId, TransferConflictPolicy policy) {
        const auto state = stateFor(batchId);
        std::lock_guard<std::timed_mutex> lock(state->mutex);
        state->policy = policy;
        state->configured = true;
    }

    TransferConflictPolicy batchPolicy(
        std::uint64_t batchId,
        TransferConflictPolicy fallback = TransferConflictPolicy::Ask) const {
        const auto state = findState(batchId);
        if (!state)
            return fallback;
        std::lock_guard<std::timed_mutex> lock(state->mutex);
        return state->configured ? state->policy : fallback;
    }

    ConflictResolution resolve(const ConflictRequest &request,
                               TransferConflictPolicy fallbackPolicy,
                               const Resolver &resolver,
                               const CancelCheck &shouldCancel = {}) {
        const auto state = stateFor(request.batchId);
        std::unique_lock<std::timed_mutex> lock(state->mutex, std::defer_lock);
        while (!lock.try_lock_for(std::chrono::milliseconds(50))) {
            if (shouldCancel && shouldCancel())
                return canceledResolution();
        }
        if (shouldCancel && shouldCancel())
            return canceledResolution();

        TransferConflictPolicy policy =
            state->configured ? state->policy : fallbackPolicy;
        state->configured = true;
        state->policy = policy;

        if (!isSupported(policy, request)) {
            // An old/persisted policy can become unsupported when a backend
            // changes. Falling back to Ask prevents a silent overwrite.
            state->policy = TransferConflictPolicy::Ask;
            policy = TransferConflictPolicy::Ask;
        }
        if (policy != TransferConflictPolicy::Ask)
            return evaluate(policy, request);

        // A UI resolver only exposes supported choices. The small retry loop
        // also protects embedders and tests that accidentally return a choice
        // unavailable for this request.
        for (int resolutionAttempt = 0; resolutionAttempt < 4;
             ++resolutionAttempt) {
            if (shouldCancel && shouldCancel())
                return canceledResolution();
            ConflictResolution decision = resolver(request);
            if (decision.canceled)
                return decision;
            if (!isSupported(decision.policy, request) ||
                decision.policy == TransferConflictPolicy::Ask) {
                state->policy = TransferConflictPolicy::Ask;
                continue;
            }

            const TransferConflictPolicy selected = decision.policy;
            if (decision.applyToRemaining) {
                state->policy = selected;
                state->configured = true;
            }
            ConflictResolution evaluated = evaluate(selected, request);
            evaluated.applyToRemaining = decision.applyToRemaining;
            return evaluated;
        }
        return canceledResolution();
    }

    void forgetBatch(std::uint64_t batchId) {
        std::lock_guard<std::mutex> lock(statesMutex_);
        states_.erase(batchId);
    }

    private:
    struct BatchState {
        mutable std::timed_mutex mutex;
        TransferConflictPolicy policy = TransferConflictPolicy::Ask;
        bool configured = false;
    };

    static bool isSupported(TransferConflictPolicy policy,
                            const ConflictRequest &request) {
        if (policy == TransferConflictPolicy::Resume)
            return request.allowResume;
        if (policy == TransferConflictPolicy::NewerOnly) {
            return request.sourceMtime.has_value() &&
                   request.destinationMtime.has_value();
        }
        return true;
    }

    static ConflictResolution evaluate(TransferConflictPolicy policy,
                                       const ConflictRequest &request) {
        if (policy == TransferConflictPolicy::NewerOnly) {
            // Two seconds avoids copies caused only by filesystem/protocol
            // timestamp precision.
            policy = *request.sourceMtime > (*request.destinationMtime + 2)
                         ? TransferConflictPolicy::Overwrite
                         : TransferConflictPolicy::Skip;
        }
        return {policy, false, false};
    }

    static ConflictResolution canceledResolution() {
        return {TransferConflictPolicy::Skip, false, true};
    }

    std::shared_ptr<BatchState> stateFor(std::uint64_t batchId) const {
        std::lock_guard<std::mutex> lock(statesMutex_);
        auto &state = states_[batchId];
        if (!state)
            state = std::make_shared<BatchState>();
        return state;
    }

    std::shared_ptr<BatchState> findState(std::uint64_t batchId) const {
        std::lock_guard<std::mutex> lock(statesMutex_);
        const auto found = states_.find(batchId);
        return found == states_.end() ? nullptr : found->second;
    }

    mutable std::mutex statesMutex_;
    mutable std::unordered_map<std::uint64_t, std::shared_ptr<BatchState>>
        states_;
};
