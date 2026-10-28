#include "harken/core/SamePositionResolver.h"

#include <cstdint>

namespace harken {
namespace {

// a mod m folded into [0, m) for a positive m, correct for a negative a: C++'s %
// takes the sign of the dividend, so a lone % can return a negative remainder,
// which would place the target before its own entry. This is the signed floor-mod
// the contract names. Overflow-free: % never overflows, and the single add stays
// within range because the pre-add value is in (-m, 0].
std::int64_t SignedFloorMod(std::int64_t a, std::int64_t m) {
    std::int64_t r = a % m;
    if (r < 0) {
        r += m;
    }
    return r;
}

} // namespace

bool AreReuseCompatible(const CompatibilityKey& a, const CompatibilityKey& b) {
    return a.stableIdentity == b.stableIdentity &&
           a.programGeneration == b.programGeneration &&
           a.mediaVariant == b.mediaVariant && a.bakedEntry == b.bakedEntry &&
           a.sourceLoopRegionStart == b.sourceLoopRegionStart &&
           a.sourceLoopRegionEnd == b.sourceLoopRegionEnd &&
           a.loopPolicy == b.loopPolicy && a.loopIteration == b.loopIteration &&
           a.nominalTimeOrigin == b.nominalTimeOrigin &&
           a.nominalDuration == b.nominalDuration &&
           a.routeTarget == b.routeTarget && a.envelopeId == b.envelopeId &&
           a.seekable == b.seekable && a.clockMappingId == b.clockMappingId;
}

SamePositionOutcome ResolveSamePosition(
    const CompatibilityKey& source, const CompatibilityKey& target,
    SegmentTimeType sourceType, const ClockKernel& sourceClock,
    SegmentTimeType targetType, const ClockKernel& targetClock,
    std::int64_t sourceNominalPosition) {
    SamePositionOutcome out;

    // Continue current: the new winner is the same playable, at the same program
    // generation. Not a different source, so nothing hands off.
    if (source.stableIdentity == target.stableIdentity &&
        source.programGeneration == target.programGeneration) {
        out.kind = SamePositionKind::ContinueCurrent;
        return out;
    }

    auto rejectWith = [&](SamePositionReject reason) {
        out.kind = SamePositionKind::Rejected;
        out.reject = reason;
        return out;
    };

    if (!target.seekable) {
        return rejectWith(SamePositionReject::NotSeekable);
    }
    if (target.nominalDuration <= 0) {
        return rejectWith(SamePositionReject::MissingNominalDuration);
    }
    // The clocks must be reconcilable: both valid, and on the same sample rate. A
    // sample-rate difference is a transport rebuild, not a handoff. (Two different
    // tempo maps at the same rate ARE reconcilable -- a Musical-to-Musical handoff
    // between tempos is exactly what this supports.)
    if (!sourceClock.Valid() || !targetClock.Valid() ||
        sourceClock.SampleRateHz() != targetClock.SampleRateHz()) {
        return rejectWith(SamePositionReject::IncompatibleClockMapping);
    }

    // Both Musical: keep the position in ticks and convert through each side's own
    // tempo map. Otherwise the shared unit is entry-relative nanoseconds, and a
    // musical side is expressed in nanoseconds through its clock first.
    const bool commonIsTicks =
        (sourceType == SegmentTimeType::Musical &&
         targetType == SegmentTimeType::Musical);

    std::int64_t sourcePos = 0;
    std::int64_t targetDuration = 0;
    if (commonIsTicks) {
        sourcePos = sourceNominalPosition;
        targetDuration = target.nominalDuration;
    } else {
        sourcePos =
            (sourceType == SegmentTimeType::Musical)
                ? sourceClock.TickToTime(MusicalTick{sourceNominalPosition}).value
                : sourceNominalPosition;
        targetDuration =
            (targetType == SegmentTimeType::Musical)
                ? targetClock.TickToTime(MusicalTick{target.nominalDuration}).value
                : target.nominalDuration;
        // Guard the fold divisor against zero. A valid clock keeps frames-per-tick
        // at least one, so a positive musical tick duration always converts to a
        // positive nanosecond span and this cannot fire in practice; it stays as
        // defence against a zero-length fold interval (a division by zero) rather
        // than depending on the clock domain from here.
        if (targetDuration <= 0) {
            return rejectWith(SamePositionReject::MissingNominalDuration);
        }
    }

    const std::int64_t targetPos = SignedFloorMod(sourcePos, targetDuration);
    out.kind = SamePositionKind::Handoff;
    out.targetPosition = targetPos;
    out.targetPositionIsTicks = commonIsTicks;
    out.targetFrame = commonIsTicks
                          ? targetClock.TickToFrame(MusicalTick{targetPos})
                          : targetClock.TimeToFrame(AbsoluteTime{targetPos});
    return out;
}

} // namespace harken
