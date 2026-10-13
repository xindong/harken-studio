#ifndef HARKEN_CORE_STREAM_SEEK_HANDOFF_TABLE_H
#define HARKEN_CORE_STREAM_SEEK_HANDOFF_TABLE_H

#include "harken/core/StreamRing.h"

#include <harken/Harken.h>

#include <atomic>
#include <cstdint>
#include <memory>

namespace harken {

inline constexpr std::uint32_t kStreamSeekHandoffSlotBits = 12;
inline constexpr std::uint32_t kMaxStreamSeekHandoffsHard =
    1u << kStreamSeekHandoffSlotBits;
inline constexpr std::uint64_t kMaxStreamSeekHandoffGeneration =
    UINT64_MAX >> kStreamSeekHandoffSlotBits;

enum class StreamSeekHandoffLifecycle : std::uint32_t {
    Free = 0,
    Prepared = 1,
    Applied = 2,
    Cancelled = 3,
    Quarantined = 4,
};

struct StreamSeekHandoffSnapshot {
    StreamRing* replacementRing = nullptr;
    std::uint64_t expectedOldRingToken = 0;
    std::uint64_t replacementRingToken = 0;
    std::uint64_t targetFrame = 0;
    std::uint64_t frameCount = 0;
    std::uint32_t sourceSampleRate = 0;
    std::uint32_t prebufferTargetFrames = 0;
};

// A fixed ownership ledger for replacement stream rings prepared by the logic
// thread and consumed or cancelled by the audio thread. It does not own voice
// lifecycle; it only keeps a prepared ring reachable without putting a pointer
// in CommandRecord.
class StreamSeekHandoffTable {
public:
    bool Reset(std::uint32_t capacity);

    bool Prepare(VnActionSeekTicket ticket, VnVoiceHandle expectedVoice,
                 std::uint64_t expectedOldRingToken,
                 StreamRing* replacementRing,
                 std::uint64_t replacementRingToken,
                 std::uint32_t fileIndex,
                 std::uint64_t contentRevision,
                 std::uint64_t targetFrame,
                 std::uint64_t frameCount,
                 std::uint32_t sourceSampleRate,
                 std::uint32_t prebufferTargetFrames,
                 std::uint64_t* outToken) noexcept;

    bool CanApply(std::uint64_t token, VnActionSeekTicket ticket,
                  VnVoiceHandle expectedVoice,
                  StreamSeekHandoffSnapshot* out) const noexcept;
    bool MarkApplied(std::uint64_t token,
                     VnActionSeekTicket ticket) noexcept;
    bool CancelFromAudio(std::uint64_t token,
                         VnActionSeekTicket ticket) noexcept;
    bool Abandon(std::uint64_t token) noexcept;
    void ReleaseTicket(VnActionSeekTicket ticket) noexcept;
    void CancelAllAfterAudioJoin() noexcept;

    std::uint32_t Capacity() const noexcept { return capacity_; }

private:
    struct Row {
        std::atomic<std::uint32_t> lifecycle{
            static_cast<std::uint32_t>(StreamSeekHandoffLifecycle::Free)};
        std::uint64_t generation = 0;
        VnActionSeekTicket ticket = 0;
        VnVoiceHandle expectedVoice = 0;
        std::uint64_t expectedOldRingToken = 0;
        StreamRing* replacementRing = nullptr;
        std::uint64_t replacementRingToken = 0;
        std::uint32_t fileIndex = 0;
        std::uint64_t contentRevision = 0;
        std::uint64_t targetFrame = 0;
        std::uint64_t frameCount = 0;
        std::uint32_t sourceSampleRate = 0;
        std::uint32_t prebufferTargetFrames = 0;
    };

    static std::uint64_t MakeToken(std::uint32_t slot,
                                   std::uint64_t generation) noexcept;
    static std::uint32_t SlotOf(std::uint64_t token) noexcept;
    static std::uint64_t GenerationOf(std::uint64_t token) noexcept;
    Row* Resolve(std::uint64_t token) noexcept;
    const Row* Resolve(std::uint64_t token) const noexcept;
    static bool ReplacementIsExactActiveGeneration(const Row& row) noexcept;
    static void RetireReplacement(Row& row) noexcept;
    bool Recycle(Row& row) noexcept;

    std::unique_ptr<Row[]> rows_;
    std::uint32_t capacity_ = 0;
};

} // namespace harken

#endif // HARKEN_CORE_STREAM_SEEK_HANDOFF_TABLE_H
