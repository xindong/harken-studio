// Debug allocation guard: detects system-heap use on threads tagged as
// audio-rendering threads. Debug builds install a CRT allocation hook; a
// tagged thread performing an alloc/realloc increments the hit counter,
// reports a raw stack backtrace, and aborts. Release builds compile to
// no-ops. See spec section 4: the audio thread never touches the system heap.
#ifndef HARKEN_CORE_ALLOC_GUARD_H
#define HARKEN_CORE_ALLOC_GUARD_H

#include <cstdint>

namespace harken::memguard {

// Installs the hook (Debug only). Safe to call repeatedly; reference-counted
// is not needed because the engine is a singleton — Install/Uninstall pair
// with Vn_Init/Vn_Term.
void Install();
void Uninstall();

// True when the Debug hook is live. Always false in Release builds.
bool IsArmed();

// Number of heap events observed on tagged threads since Install.
// The expected steady-state value is zero; any hit is a rule violation.
std::uint64_t HitCount();

// True while the calling thread is inside at least one AudioThreadScope.
// The tag itself is live in every build (only the hook is Debug-only),
// so this is an honest observation surface for the re-entrancy contract.
bool IsAudioTagged();

struct AllocationEvents {
    std::uint64_t allocations = 0;
    std::uint64_t reallocations = 0;
    std::uint64_t allocationBytes = 0;
    std::uint64_t reallocationBytes = 0;
};

struct AllocationObservation {
    bool available = false;
    AllocationEvents logic{};
    AllocationEvents audio{};
};

// A single bounded observer on the calling thread, using the existing hook.
// Counts requested allocation traffic, not retained heap storage. The result
// remains unavailable if the hook was unarmed or a scope was already active.
class AllocationObservationScope {
public:
    explicit AllocationObservationScope(AllocationObservation& result) noexcept;
    ~AllocationObservationScope();
    AllocationObservationScope(const AllocationObservationScope&) = delete;
    AllocationObservationScope& operator=(const AllocationObservationScope&) = delete;
private:
    AllocationObservation* result_ = nullptr;
};

// Tags the calling thread for the lifetime of the scope. Re-entrant: the
// tag is a per-thread depth count, so nesting scopes (a device callback
// wrapping the mixer's per-block scope) keeps the thread tagged until
// the outermost scope exits. The mixer loop enters this scope for every
// rendered block.
class AudioThreadScope {
public:
    AudioThreadScope();
    ~AudioThreadScope();
    AudioThreadScope(const AudioThreadScope&) = delete;
    AudioThreadScope& operator=(const AudioThreadScope&) = delete;
};

} // namespace harken::memguard

#endif // HARKEN_CORE_ALLOC_GUARD_H
