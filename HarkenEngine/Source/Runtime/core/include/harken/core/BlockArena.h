// Per-block bump arena: the audio thread's only scratch memory source. The
// backing buffer is allocated once on the logic thread; the audio thread
// bumps a pointer and resets it at block end. Exhaustion never allocates and
// never crashes: alloc returns nullptr and an overflow counter increments —
// callers skip the work that needed the scratch (defined degradation).
//
// Two ends, one capacity. The low end serves the bus accumulator planes; the
// high end serves the per-voice cascade scratch that render workers write. They
// grow towards each other and exhaustion is still "they met", so which
// allocation is the first to fail — and therefore which voices a tight arena
// drops — is exactly what it was with one cursor. What the split buys is that a
// worker still writing scratch for a block the mixer has moved past cannot
// reach the next block's bus planes: those live at the other end. Protecting
// them by arithmetic alone would work only while the arithmetic is right.
//
// INVARIANT: every allocation uses one alignment. The two ends round in
// opposite directions (low rounds the base up, high rounds it down), and their
// failure points coincide only while no caller mixes alignments. With mixed
// alignments they diverge — and not benignly: a modelled sweep found the high
// end refusing an allocation the single-cursor arena granted while granting one
// it refused, which in a tight arena means different voices dropped and a
// byte-identical golden that shifts for a reason that looks like the mix
// changed. That is why the general Alloc is private: mixing alignments is not
// something an outside caller can express. Anything that needs a wider
// alignment must change this class, and must re-run the equivalence model the
// arena tests carry.
#ifndef HARKEN_CORE_BLOCK_ARENA_H
#define HARKEN_CORE_BLOCK_ARENA_H

#include <cstddef>
#include <cstdint>
#include <memory>

namespace harken {

class BlockArena {
public:
    explicit BlockArena(std::uint32_t capacityBytes);

    // `count` floats from the low end, zero-initialized (scratch consumers mix
    // and accumulate, so a defined starting value is part of the contract).
    // A zero count is a no-op that returns an aligned non-null pointer without
    // changing usage, high-water, or overflow state.
    float* AllocFloats(std::size_t count);

    // `count` floats from the high end, same contract. Reserved for scratch a
    // render worker may still be writing after its batch was abandoned.
    // A zero count is a no-op that returns an aligned non-null pointer without
    // changing usage, high-water, or overflow state.
    float* AllocFloatsHigh(std::size_t count);

    // Same reservation and accounting, without initializing the contents.
    // Callers must fill or clear the span before exposing it to a reader.
    float* ReserveFloatsHigh(std::size_t count);

    // Rewinds both bump pointers. Called once per rendered block. The
    // descending end rewinds to the retired floor rather than to zero, so a
    // quarantined span is never handed out again.
    void Reset();

    // Retires every descending-end byte handed out since the last Reset, for
    // good. The owner calls this when it has walked away from a block whose
    // writers it can no longer account for: those writers hold pointers into
    // this span and may still be writing through them, so the next block must
    // not be able to allocate any of it.
    //
    // Deliberately coarse. The descending end is a bump allocator and the
    // abandoned block's scratch is interleaved across the whole span it handed
    // out, so the only floor that provably covers every pointer a stranded
    // writer holds is the span's own edge -- not the subset of blocks the owner
    // can still enumerate. The arena shrinks permanently; the caller's own
    // degradation state is what keeps this from happening twice.
    void QuarantineHighEnd();

    std::uint32_t CapacityBytes() const { return m_capacity; }
    // Descending-end bytes permanently retired by QuarantineHighEnd.
    std::uint32_t QuarantinedHighBytes() const { return m_highFloor; }
    // Bytes in use across both ends.
    std::uint32_t UsedBytes() const { return m_lowUsed + m_highUsed; }
    // The most both ends have ever held at the same instant — not the sum of
    // each end's own peak, which would over-report whenever the two peak in
    // different blocks.
    std::uint32_t HighWaterBytes() const { return m_highWater; }
    std::uint64_t OverflowCount() const { return m_overflows; }

    // Internal deterministic failure-injection seam. Tests may temporarily
    // lower the effective capacity between blocks; the backing allocation is
    // never resized and production callers never use this path.
    void SetCapacityForTests(std::uint32_t capacityBytes);
    void RestoreCapacityForTests();

private:
    // Bump-allocates `bytes` aligned to `alignment` (a power of two) from the
    // named end. Returns nullptr when the block does not fit; the overflow
    // counter increments and the arena state is unchanged.
    void* Alloc(std::size_t bytes, std::size_t alignment, bool fromHighEnd);
    void NoteHighWater();

    std::unique_ptr<std::byte[]> m_buffer;
    std::uint32_t m_storageCapacity = 0;
    std::uint32_t m_capacity = 0;
    std::uint32_t m_lowUsed = 0;
    std::uint32_t m_highUsed = 0;
    // Descending-end bytes retired for good. Zero for the whole life of an
    // arena that never quarantines, at which point every allocation path below
    // is bit-identical to one without a floor.
    std::uint32_t m_highFloor = 0;
    std::uint32_t m_highWater = 0;
    std::uint64_t m_overflows = 0;
};

} // namespace harken

#endif // HARKEN_CORE_BLOCK_ARENA_H
