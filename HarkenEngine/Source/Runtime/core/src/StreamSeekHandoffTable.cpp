#include "harken/core/StreamSeekHandoffTable.h"

#include <new>

namespace harken {
namespace {

std::atomic<std::uint64_t> g_nextStreamSeekHandoffGeneration{1};

std::uint64_t NextGeneration() noexcept {
    std::uint64_t current =
        g_nextStreamSeekHandoffGeneration.load(std::memory_order_relaxed);
    while (current <= kMaxStreamSeekHandoffGeneration) {
        if (g_nextStreamSeekHandoffGeneration.compare_exchange_weak(
                current, current + 1u, std::memory_order_relaxed,
                std::memory_order_relaxed)) {
            return current;
        }
    }
    return 0;
}

std::uint32_t Value(StreamSeekHandoffLifecycle state) noexcept {
    return static_cast<std::uint32_t>(state);
}

} // namespace

std::uint64_t StreamSeekHandoffTable::MakeToken(
    std::uint32_t slot, std::uint64_t generation) noexcept {
    return (generation << kStreamSeekHandoffSlotBits) | slot;
}

std::uint32_t StreamSeekHandoffTable::SlotOf(std::uint64_t token) noexcept {
    return static_cast<std::uint32_t>(
        token & (kMaxStreamSeekHandoffsHard - 1u));
}

std::uint64_t StreamSeekHandoffTable::GenerationOf(
    std::uint64_t token) noexcept {
    return token >> kStreamSeekHandoffSlotBits;
}

bool StreamSeekHandoffTable::Reset(std::uint32_t capacity) {
    if (capacity == 0 || capacity > kMaxStreamSeekHandoffsHard) return false;
    std::unique_ptr<Row[]> rows;
    try {
        rows = std::make_unique<Row[]>(capacity);
    } catch (const std::bad_alloc&) {
        return false;
    }
    rows_ = std::move(rows);
    capacity_ = capacity;
    return true;
}

StreamSeekHandoffTable::Row* StreamSeekHandoffTable::Resolve(
    std::uint64_t token) noexcept {
    if (token == 0) return nullptr;
    const std::uint32_t slot = SlotOf(token);
    if (slot >= capacity_) return nullptr;
    Row& row = rows_[slot];
    return row.generation == GenerationOf(token) ? &row : nullptr;
}

const StreamSeekHandoffTable::Row* StreamSeekHandoffTable::Resolve(
    std::uint64_t token) const noexcept {
    return const_cast<StreamSeekHandoffTable*>(this)->Resolve(token);
}

bool StreamSeekHandoffTable::Prepare(
    VnActionSeekTicket ticket, VnVoiceHandle expectedVoice,
    std::uint64_t expectedOldRingToken, StreamRing* replacementRing,
    std::uint64_t replacementRingToken, std::uint32_t fileIndex,
    std::uint64_t contentRevision, std::uint64_t targetFrame,
    std::uint64_t frameCount, std::uint32_t sourceSampleRate,
    std::uint32_t prebufferTargetFrames, std::uint64_t* outToken) noexcept {
    if (ticket == 0 || expectedVoice == 0 || expectedOldRingToken == 0 ||
        replacementRing == nullptr || replacementRingToken == 0 ||
        sourceSampleRate == 0 || frameCount == 0 || targetFrame > frameCount ||
        outToken == nullptr) {
        return false;
    }
    *outToken = 0;
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        Row& row = rows_[slot];
        if (row.lifecycle.load(std::memory_order_relaxed) !=
            Value(StreamSeekHandoffLifecycle::Free)) {
            continue;
        }
        if (row.generation == 0) {
            row.generation = NextGeneration();
            if (row.generation == 0) {
                row.lifecycle.store(
                    Value(StreamSeekHandoffLifecycle::Quarantined),
                    std::memory_order_relaxed);
                continue;
            }
        }
        row.ticket = ticket;
        row.expectedVoice = expectedVoice;
        row.expectedOldRingToken = expectedOldRingToken;
        row.replacementRing = replacementRing;
        row.replacementRingToken = replacementRingToken;
        row.fileIndex = fileIndex;
        row.contentRevision = contentRevision;
        row.targetFrame = targetFrame;
        row.frameCount = frameCount;
        row.sourceSampleRate = sourceSampleRate;
        row.prebufferTargetFrames = prebufferTargetFrames;
        row.lifecycle.store(Value(StreamSeekHandoffLifecycle::Prepared),
                            std::memory_order_release);
        *outToken = MakeToken(slot, row.generation);
        return true;
    }
    return false;
}

bool StreamSeekHandoffTable::ReplacementIsExactActiveGeneration(
    const Row& row) noexcept {
    if (row.replacementRing == nullptr) return false;
    const std::uint32_t expectedGeneration =
        static_cast<std::uint32_t>(row.replacementRingToken >> 32u);
    return expectedGeneration != 0 &&
           row.replacementRing->generation.load(std::memory_order_acquire) ==
               expectedGeneration &&
           row.replacementRing->state.load(std::memory_order_acquire) ==
               StreamSlotState::Active;
}

bool StreamSeekHandoffTable::CanApply(
    std::uint64_t token, VnActionSeekTicket ticket,
    VnVoiceHandle expectedVoice, StreamSeekHandoffSnapshot* out) const noexcept {
    const Row* row = Resolve(token);
    if (row == nullptr || out == nullptr || row->ticket != ticket ||
        row->expectedVoice != expectedVoice ||
        row->lifecycle.load(std::memory_order_acquire) !=
            Value(StreamSeekHandoffLifecycle::Prepared) ||
        !ReplacementIsExactActiveGeneration(*row)) {
        return false;
    }
    *out = {row->replacementRing, row->expectedOldRingToken,
            row->replacementRingToken, row->targetFrame, row->frameCount,
            row->sourceSampleRate, row->prebufferTargetFrames};
    return true;
}

bool StreamSeekHandoffTable::MarkApplied(
    std::uint64_t token, VnActionSeekTicket ticket) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr || row->ticket != ticket) return false;
    std::uint32_t expected = Value(StreamSeekHandoffLifecycle::Prepared);
    return row->lifecycle.compare_exchange_strong(
        expected, Value(StreamSeekHandoffLifecycle::Applied),
        std::memory_order_release, std::memory_order_relaxed);
}

void StreamSeekHandoffTable::RetireReplacement(Row& row) noexcept {
    if (!ReplacementIsExactActiveGeneration(row)) return;
    row.replacementRing->state.store(StreamSlotState::Retired,
                                     std::memory_order_release);
}

bool StreamSeekHandoffTable::CancelFromAudio(
    std::uint64_t token, VnActionSeekTicket ticket) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr || row->ticket != ticket) return false;
    std::uint32_t expected = Value(StreamSeekHandoffLifecycle::Prepared);
    if (!row->lifecycle.compare_exchange_strong(
            expected, Value(StreamSeekHandoffLifecycle::Cancelled),
            std::memory_order_acq_rel, std::memory_order_acquire)) {
        return expected == Value(StreamSeekHandoffLifecycle::Cancelled);
    }
    RetireReplacement(*row);
    return true;
}

bool StreamSeekHandoffTable::Abandon(std::uint64_t token) noexcept {
    Row* row = Resolve(token);
    if (row == nullptr ||
        row->lifecycle.load(std::memory_order_acquire) !=
            Value(StreamSeekHandoffLifecycle::Prepared)) {
        return false;
    }
    RetireReplacement(*row);
    return Recycle(*row);
}

void StreamSeekHandoffTable::ReleaseTicket(
    VnActionSeekTicket ticket) noexcept {
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        Row& row = rows_[slot];
        const auto state = static_cast<StreamSeekHandoffLifecycle>(
            row.lifecycle.load(std::memory_order_acquire));
        if (row.ticket == ticket &&
            (state == StreamSeekHandoffLifecycle::Applied ||
             state == StreamSeekHandoffLifecycle::Cancelled)) {
            (void)Recycle(row);
        }
    }
}

void StreamSeekHandoffTable::CancelAllAfterAudioJoin() noexcept {
    for (std::uint32_t slot = 0; slot < capacity_; ++slot) {
        Row& row = rows_[slot];
        if (row.lifecycle.load(std::memory_order_relaxed) ==
            Value(StreamSeekHandoffLifecycle::Prepared)) {
            RetireReplacement(row);
            row.lifecycle.store(Value(StreamSeekHandoffLifecycle::Cancelled),
                                std::memory_order_release);
        }
    }
}

bool StreamSeekHandoffTable::Recycle(Row& row) noexcept {
    row.ticket = 0;
    row.expectedVoice = 0;
    row.expectedOldRingToken = 0;
    row.replacementRing = nullptr;
    row.replacementRingToken = 0;
    row.fileIndex = 0;
    row.contentRevision = 0;
    row.targetFrame = 0;
    row.frameCount = 0;
    row.sourceSampleRate = 0;
    row.prebufferTargetFrames = 0;
    row.generation = NextGeneration();
    if (row.generation == 0) {
        row.lifecycle.store(Value(StreamSeekHandoffLifecycle::Quarantined),
                            std::memory_order_release);
        return false;
    }
    row.lifecycle.store(Value(StreamSeekHandoffLifecycle::Free),
                        std::memory_order_release);
    return true;
}

} // namespace harken
