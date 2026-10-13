// Same-position resolution: given the current ("source") winner and the new
// ("target") winner of a discrete selection, decide whether anything hands off at
// all and, if it does, at what position the target begins so the music does not
// jump.
//
// Three outcomes, in the order they are decided:
//
//  * Continue current. If the new winner resolves to the same playable stable
//    identity and the same program generation as the one already playing, it is
//    not a different source at all: no handoff, no transition, no restart. The
//    caller still applies any selection-state update; this resolver only reports
//    that the incumbent keeps playing. (Stable identity is the resolved identity,
//    not a raw object id -- two winners sharing an object id but differing in
//    program generation, media variant or baked entry are genuinely different and
//    do hand off. The full compatibility key below is what a caller compares when
//    it needs the stronger "may I reuse this instance" test.)
//
//  * Handoff. A genuinely different target takes over at the source's current
//    nominal position, wrapped into the target's own nominal duration. When both
//    sides are Musical the position is kept in ticks and converted through each
//    side's own tempo map, so a handoff between two tempos preserves the musical
//    position. The moment either side is Absolute (or has no clock) the shared
//    unit is entry-relative nanoseconds instead. The target position is
//    signed floor-mod into the half-open interval [0, targetNominalDuration): a
//    position past the end wraps forward, a negative (anacrusis-region) position
//    wraps up, and a position that is an exact multiple of the duration lands on
//    0. It never lands outside the interval and never silently resets to the
//    target's entry.
//
//  * Rejected. A handoff that cannot be done safely is refused before the target
//    is armed rather than guessed at in the callback: the target is not seekable,
//    it has no positive nominal duration, or the two clocks cannot be reconciled
//    (an invalid clock, or a sample-rate difference -- reconciling that is a
//    transport rebuild, not a handoff).
//
// Pure value semantics, integer arithmetic, saturating on int64 overflow, with no
// allocation, threads or I/O -- the same inputs always resolve the same way. Not
// this resolver's job: choosing the winners, applying selection state, or deciding
// what a rejected handoff should do next.
#ifndef HARKEN_CORE_SAME_POSITION_RESOLVER_H
#define HARKEN_CORE_SAME_POSITION_RESOLVER_H

#include "harken/core/ClockKernel.h"
#include "harken/core/TimeCoordinates.h"

#include <cstdint>

namespace harken {

// Everything that has to match for one playing instance to stand in for another.
// Matching only an object id is not enough -- two winners can share an id yet
// differ in generation, resolved media, loop shape, route or envelope -- so the
// key carries all of it and AreReuseCompatible compares the whole thing. All
// positions/durations are entry-relative and in the owning Piece's native unit
// (tick for Musical, nanosecond for Absolute).
struct CompatibilityKey {
    std::uint64_t stableIdentity = 0;   // resolved content identity, not a raw id
    std::uint64_t programGeneration = 0;
    std::uint64_t mediaVariant = 0;     // resolved media variant
    std::uint64_t bakedEntry = 0;       // resolved baked entry
    std::int64_t sourceLoopRegionStart = 0;
    std::int64_t sourceLoopRegionEnd = 0;
    std::uint32_t loopPolicy = 0;
    std::uint32_t loopIteration = 0;
    std::int64_t nominalTimeOrigin = 0; // zero for entry-relative positions
    std::int64_t nominalDuration = 0;   // entry->exit; a target needs this > 0
    std::uint64_t routeTarget = 0;
    std::uint64_t envelopeId = 0;
    bool seekable = false;
    std::uint64_t clockMappingId = 0;   // identity of the effective clock mapping
};

// Whether one instance may be reused for another: every field equal. Object id
// alone is deliberately insufficient -- this is the whole-key comparison the
// reuse contract requires, distinct from the stable-identity+generation test the
// resolver uses to short-circuit to "continue current".
bool AreReuseCompatible(const CompatibilityKey& a, const CompatibilityKey& b);

enum class SamePositionKind : std::uint32_t {
    ContinueCurrent = 0, // incumbent keeps playing; no handoff
    Handoff = 1,         // target takes over at the resolved position
    Rejected = 2         // handoff refused before arming
};

enum class SamePositionReject : std::uint32_t {
    None = 0,
    NotSeekable = 1,
    MissingNominalDuration = 2,     // target duration is not positive
    IncompatibleClockMapping = 3    // invalid clock or differing sample rate
};

struct SamePositionOutcome {
    SamePositionKind kind = SamePositionKind::Rejected;
    SamePositionReject reject = SamePositionReject::None;
    // Meaningful only when kind == Handoff:
    std::int64_t targetPosition = 0; // in [0, targetNominalDuration), common unit
    SampleFrame targetFrame{};       // targetPosition mapped through the target clock
    bool targetPositionIsTicks = false; // common unit was ticks (both Musical)
};

// Resolves one selection handoff. sourceNominalPosition is the source's position
// read at the transition commit point, in the source's native unit. The target's
// nominal duration, seekability and clock mapping are read from `target`. Both
// clocks are borrowed for the call only.
SamePositionOutcome ResolveSamePosition(
    const CompatibilityKey& source, const CompatibilityKey& target,
    PieceTimeType sourceType, const ClockKernel& sourceClock,
    PieceTimeType targetType, const ClockKernel& targetClock,
    std::int64_t sourceNominalPosition);

} // namespace harken

#endif // HARKEN_CORE_SAME_POSITION_RESOLVER_H
