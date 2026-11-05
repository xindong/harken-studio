#pragma once

#include "harken/core/QueueRuntime.h"

namespace harken {

class OriginalConditionalReplacementBatch;

// The original Page future retains its own receiver until both audio and an
// attached Action replacement release it. Preparation never suppresses Old.
class QueuePageConditionalReceiver {
public:
    QueuePageConditionalReceiver() = default;
    QueuePageConditionalReceiver(const QueuePageConditionalReceiver&) = delete;
    QueuePageConditionalReceiver& operator=(const QueuePageConditionalReceiver&) = delete;
    ~QueuePageConditionalReceiver();
    bool Initialize(QueueInstanceTable& queues,
        const QueueInstanceTable::OriginalLifetimePin& original, QueueInstanceToken queue,
        ConditionalQueueVersionToken conditional, DecisionToken decision) noexcept;
    bool MatchesConditional(const QueueInstanceTable& queues, QueueInstanceToken queue,
        ConditionalQueueVersionToken conditional, DecisionToken decision) const noexcept;
    bool PrepareReplacementCheck(std::uint16_t slices, std::uint64_t completed) noexcept {
        slices_ = slices;
        completed_ = completed;
        return Replaceable();
    }
    bool Replaceable() const noexcept;
    bool CanRetire() const noexcept;
    bool BeginAudio(const FrontierGroupBinding& binding) const noexcept;
    void BeforeAudioWrite() const noexcept;
    void EndAudio() const noexcept;
private:
    friend class OriginalConditionalReplacementBatch;
    static constexpr std::uint64_t kChanging = 1, kAttached = 2, kReading = 4,
        kWritten = 8, kWithdrawn = 16;
    QueueInstanceTable* queues_ = nullptr;
    QueueInstanceTable::OriginalLifetimePin original_;
    QueueInstanceToken queue_{};
    ConditionalQueueVersionToken conditional_{};
    DecisionToken decision_{};
    mutable std::atomic<std::uint64_t> admission_{0};
    OriginalConditionalReplacementBatch* replacement_ = nullptr;
    std::uint64_t replacementGeneration_ = 0;
    std::uint64_t completed_ = 0;
    std::uint16_t slices_ = 0;
    mutable bool audioRead_ = false;
    mutable bool replacementRead_ = false;
};

} // namespace harken
