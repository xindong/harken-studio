// The program-generation registry: which published set of compiled programs is
// current, which superseded sets are still readable, and when a superseded set
// may be destroyed.
//
// Replacement never edits in place. Publishing a new set makes it current and
// leaves the previous one readable, because the audio thread may still be
// holding a reference it took before the swap. A superseded set becomes
// reclaimable only once the audio thread has demonstrably moved past the point
// where the swap happened — that is what the sequence fence records. Freeing on
// the publish itself would pull memory out from under a reader.
//
// The fence comparison is deliberately conservative: a set is reclaimed only
// when the observed audio sequence is strictly greater than the sequence at
// which the set was superseded. Equality is not enough, because a reader that
// is exactly at that sequence may still be inside the block that read the old
// set. The failure direction is therefore always "retained one round too long",
// never "freed while in use".
//
// Capacity is fixed and a full registry refuses before publishing anything, so
// a caller either gets a complete new generation or the state it already had —
// never a half-installed set.
//
// The registry knows nothing about program contents. It holds an opaque payload
// value per generation and manages identity and lifetime around it, which is
// what keeps generation bookkeeping from being reimplemented once per program
// kind.
//
// Control thread only for publish and reclaim. Storage is sized once at
// construction; no operation allocates.
#ifndef HARKEN_CORE_PROGRAM_GENERATION_REGISTRY_H
#define HARKEN_CORE_PROGRAM_GENERATION_REGISTRY_H

#include "harken/core/CheckedIssuer.h"

#include <cstdint>
#include <vector>

namespace harken {

// Monotonic generation id. 0 names no generation and is never published.
using ProgramGeneration = std::uint64_t;

class ProgramGenerationRegistry {
public:
    // Capacity is the number of generations that may be retained at once: the
    // current one plus every superseded one still behind its fence.
    explicit ProgramGenerationRegistry(std::uint32_t capacity);

    std::uint32_t Capacity() const {
        return static_cast<std::uint32_t>(m_entries.size());
    }

    // Publishes payload as the new current generation and returns its id, or 0
    // when the registry is full — checked before anything changes, so a refused
    // publish leaves the previous current generation intact and readable.
    // audioSequenceNow is the audio-side sequence observed at publish time; it
    // becomes the fence the outgoing generation must be seen past.
    ProgramGeneration Publish(std::uint64_t payload,
                              std::uint64_t audioSequenceNow);

    // The current generation, or 0 before anything has been published.
    ProgramGeneration Current() const { return m_current; }

    // Whether a generation is the one new work should bind to.
    bool IsCurrent(ProgramGeneration generation) const {
        return generation != 0 && generation == m_current;
    }

    // Whether a generation may still be read. True for the current generation
    // and for every superseded generation that has not been reclaimed, which is
    // the window in which an already-issued reference stays valid.
    bool IsReadable(ProgramGeneration generation) const;

    // The payload published under a generation. False when the generation is
    // not readable, so a reclaimed id cannot resolve to a stale payload.
    bool PayloadOf(ProgramGeneration generation, std::uint64_t* outPayload) const;

    // Destroys every superseded generation the audio thread has been seen past
    // and returns how many were released. Generations still behind their fence
    // are left alone; this is the only path by which a retained generation
    // goes away.
    std::uint32_t ReclaimSuperseded(std::uint64_t observedAudioSequence);
    void BeginTransportReset(std::uint64_t audioSequenceNow) noexcept;
    void ResetForTransport() noexcept;

    // Generations currently retained, and the most ever retained at once. A
    // rising retained count with a flat reclaim is how fence starvation shows
    // up before the registry fills.
    std::uint32_t RetainedCount() const { return m_retainedCount; }
    std::uint32_t HighWater() const { return m_highWater; }
    bool GenerationExhausted() const { return m_generationExhausted; }
    bool SetNextGenerationForTests(ProgramGeneration generation) {
        if (generation == 0 || m_current != 0 || m_retainedCount != 0) {
            return false;
        }
        m_nextGeneration = generation;
        m_generationExhausted = false;
        return true;
    }

private:
    struct Entry {
        bool used = false;
        bool superseded = false;
        ProgramGeneration generation = 0;
        std::uint64_t payload = 0;
        // Audio sequence at the moment this generation stopped being current.
        // Meaningless while superseded is false.
        std::uint64_t supersededAtAudioSequence = 0;
    };

    const Entry* Find(ProgramGeneration generation) const;

    std::vector<Entry> m_entries;
    ProgramGeneration m_nextGeneration = 1;
    bool m_generationExhausted = false;
    ProgramGeneration m_current = 0;
    std::uint32_t m_retainedCount = 0;
    std::uint32_t m_highWater = 0;
};

} // namespace harken

#endif // HARKEN_CORE_PROGRAM_GENERATION_REGISTRY_H
