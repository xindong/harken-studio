#include "harken/core/ClockKernel.h"

#include <cstddef>
#include <cstdint>

// The arithmetic here is deliberately built from 64-bit pieces rather than a
// compiler 128-bit type. MSVC has no __int128, so a portable kernel that must
// give bit-identical results under clang-cl, Apple clang and MSVC cannot depend
// on one; a single hand-rolled path means every toolchain runs the same code and
// the property tests prove the same thing everywhere. The cost -- a 128-bit long
// division per conversion -- is irrelevant because this kernel runs on the
// planning thread, never in the audio callback.

namespace harken {
namespace {

// Accepted-domain bounds (mirrored in the header's contract comment). Chosen so
// microsPerQuarter * sampleRate stays within int64 and the products below stay
// within the 128-bit intermediate without needing a third limb.
constexpr std::int64_t kMinSampleRateHz = 1;
constexpr std::int64_t kMaxSampleRateHz = 3'000'000;
constexpr std::int64_t kMinMicrosPerQuarter = 1;
constexpr std::int64_t kMaxMicrosPerQuarter = 60'000'000'000;

// Denominator relating ticks to frames: one tick is micros/(PPQ*1e6) seconds, so
// frames per tick is (micros*sr)/(PPQ*1e6). This is the PPQ*1e6 part.
constexpr std::int64_t kTickFrameDenom =
    kClockPulsesPerQuarter * 1'000'000; // 960 * 1e6 = 9.6e8
constexpr std::int64_t kTickTimeDenom = kClockPulsesPerQuarter;
constexpr std::int64_t kNanosPerMicrosecond = 1'000;
constexpr std::int64_t kNanosPerSecond = 1'000'000'000;

// A 128-bit integer as magnitude plus sign. Sign-magnitude rather than two's
// complement because the only operations needed are multiply, add of a
// nonnegative amount, negate, and floor-divide by a positive divisor -- each a
// couple of lines this way, whereas two's-complement division is the subtle part
// for no benefit. `negative` is false whenever the value is zero.
struct Wide {
    std::uint64_t hi = 0;
    std::uint64_t lo = 0;
    bool negative = false;
};

// |v| as an unsigned magnitude, correct even for INT64_MIN where -v would
// overflow: the two's-complement negation read as unsigned is the magnitude.
std::uint64_t AbsU64(std::int64_t v) {
    return v < 0 ? (~static_cast<std::uint64_t>(v) + 1u)
                 : static_cast<std::uint64_t>(v);
}

// Full 64x64 -> 128 unsigned multiply via 32-bit halves.
Wide MulU64(std::uint64_t a, std::uint64_t b) {
    const std::uint64_t aLo = a & 0xFFFFFFFFull;
    const std::uint64_t aHi = a >> 32;
    const std::uint64_t bLo = b & 0xFFFFFFFFull;
    const std::uint64_t bHi = b >> 32;
    const std::uint64_t ll = aLo * bLo;
    const std::uint64_t lh = aLo * bHi;
    const std::uint64_t hl = aHi * bLo;
    const std::uint64_t hh = aHi * bHi;
    const std::uint64_t cross =
        (ll >> 32) + (lh & 0xFFFFFFFFull) + (hl & 0xFFFFFFFFull);
    Wide r;
    r.lo = (ll & 0xFFFFFFFFull) | (cross << 32);
    r.hi = hh + (lh >> 32) + (hl >> 32) + (cross >> 32);
    return r;
}

// Signed 64x64 -> signed 128 multiply.
Wide MulSigned(std::int64_t a, std::int64_t b) {
    Wide r = MulU64(AbsU64(a), AbsU64(b));
    r.negative = (r.hi != 0 || r.lo != 0) && ((a < 0) != (b < 0));
    return r;
}

// value + add, where add is nonnegative. For a negative value this moves it
// toward zero, which is exactly how the inverse conversion's +(D-1) bias is
// applied to a possibly-negative numerator.
Wide AddSignedNonNeg(Wide v, std::uint64_t add) {
    if (!v.negative) {
        const std::uint64_t lo = v.lo + add;
        v.hi += (lo < v.lo) ? 1u : 0u;
        v.lo = lo;
        return v;
    }
    // v < 0. If |v| <= add the result crosses zero to add - |v| (>= 0).
    if (v.hi == 0 && v.lo <= add) {
        Wide r;
        r.lo = add - v.lo;
        return r; // hi = 0, negative = false
    }
    // |v| > add: shrink the magnitude, keep the sign.
    const std::uint64_t borrow = (v.lo < add) ? 1u : 0u;
    v.lo -= add;
    v.hi -= borrow;
    return v;
}

// floor(value / den) clamped to the int64 range. den must be positive and below
// 2^63 (every divisor the conversions form -- PPQ*1e6, micros*sr, 1e9, the sample
// rate -- is far below that), which keeps the single-word running remainder from
// overflowing when it is shifted. When requested, remainderOut receives the
// Euclidean remainder in [0, den).
std::int64_t FloorDivClamp(Wide value, std::uint64_t den,
                           std::uint64_t* remainderOut = nullptr) {
    std::uint64_t qHi = 0;
    std::uint64_t qLo = 0;
    std::uint64_t rem = 0;
    for (int i = 127; i >= 0; --i) {
        const std::uint64_t bit = (i >= 64) ? ((value.hi >> (i - 64)) & 1u)
                                            : ((value.lo >> i) & 1u);
        rem = (rem << 1) | bit;
        if (rem >= den) {
            rem -= den;
            if (i >= 64) {
                qHi |= (std::uint64_t{1} << (i - 64));
            } else {
                qLo |= (std::uint64_t{1} << i);
            }
        }
    }
    if (remainderOut != nullptr) {
        *remainderOut = (!value.negative || rem == 0) ? rem : den - rem;
    }
    if (!value.negative) {
        // A nonnegative quotient floors by truncation.
        if (qHi != 0 || qLo > static_cast<std::uint64_t>(INT64_MAX)) {
            return INT64_MAX;
        }
        return static_cast<std::int64_t>(qLo);
    }
    // value < 0: floor(-mag/den) = -ceil(mag/den) = -(quotient + (rem != 0)).
    if (qHi != 0) {
        return INT64_MIN;
    }
    std::uint64_t q = qLo;
    if (rem != 0) {
        if (q == UINT64_MAX) {
            return INT64_MIN;
        }
        q += 1;
    }
    // -(2^63) is exactly INT64_MIN; any larger magnitude saturates there too.
    if (q > static_cast<std::uint64_t>(INT64_MAX)) {
        return INT64_MIN;
    }
    return -static_cast<std::int64_t>(q);
}

// Saturating int64 add, subtract and negate. Overflow is detected with the
// two's-complement sign trick and clamped, so a pathological coordinate produces
// a defined edge value rather than a wrapped one.
std::int64_t SatAdd(std::int64_t a, std::int64_t b) {
    const std::uint64_t s = static_cast<std::uint64_t>(a) + static_cast<std::uint64_t>(b);
    const std::int64_t res = static_cast<std::int64_t>(s);
    if (((a ^ res) & (b ^ res)) < 0) {
        return a < 0 ? INT64_MIN : INT64_MAX;
    }
    return res;
}

std::int64_t SatSub(std::int64_t a, std::int64_t b) {
    const std::uint64_t d = static_cast<std::uint64_t>(a) - static_cast<std::uint64_t>(b);
    const std::int64_t res = static_cast<std::int64_t>(d);
    if (((a ^ b) & (a ^ res)) < 0) {
        return a < 0 ? INT64_MIN : INT64_MAX;
    }
    return res;
}

// floor(dtick * (micros*sr) / (PPQ*1e6)): the entry-relative frames spanned by
// dtick ticks at one tempo. dtick may be negative (anacrusis). micros*sr is
// within int64 by the accepted domain, so the only wide value is dtick*(micros*sr).
std::int64_t SpanFrames(std::int64_t sampleRateHz, std::int64_t microsPerQuarter,
                        std::int64_t dtick, std::uint64_t initialRemainder,
                        std::uint64_t* finalRemainder) {
    const std::int64_t rate = microsPerQuarter * sampleRateHz;
    Wide num = MulSigned(dtick, rate);
    num = AddSignedNonNeg(num, initialRemainder);
    return FloorDivClamp(num, static_cast<std::uint64_t>(kTickFrameDenom),
                         finalRemainder);
}

// floor((remainder + dtick*micros*1000) / PPQ): the nanoseconds spanned by
// dtick ticks at one tempo. The largest accepted tempo times 1000 is below 2^46;
// multiplying by an int64 tick magnitude is therefore below 2^109. Adding the
// sub-PPQ remainder still fits comfortably in the 128-bit Wide path.
std::int64_t SpanTime(std::int64_t microsPerQuarter, std::int64_t dtick,
                      std::uint64_t initialRemainder,
                      std::uint64_t* finalRemainder) {
    const std::int64_t nanosPerQuarter =
        microsPerQuarter * kNanosPerMicrosecond;
    Wide num = MulSigned(dtick, nanosPerQuarter);
    num = AddSignedNonNeg(num, initialRemainder);
    return FloorDivClamp(num, static_cast<std::uint64_t>(kTickTimeDenom),
                         finalRemainder);
}

} // namespace

bool ClockKernel::Reset(std::int64_t sampleRateHz, const TempoSegment* segments,
                        std::size_t segmentCount) {
    segments_ = nullptr;
    segmentCount_ = 0;
    sampleRateHz_ = 0;
    valid_ = false;

    if (segments == nullptr || segmentCount == 0) {
        return false;
    }
    if (sampleRateHz < kMinSampleRateHz || sampleRateHz > kMaxSampleRateHz) {
        return false;
    }
    // Entry is the origin: the first segment must begin at tick 0.
    if (segments[0].startTick.value != 0) {
        return false;
    }
    for (std::size_t i = 0; i < segmentCount; ++i) {
        const std::int64_t micros = segments[i].microsPerQuarter;
        if (micros < kMinMicrosPerQuarter || micros > kMaxMicrosPerQuarter) {
            return false;
        }
        if (micros * sampleRateHz < kTickFrameDenom) {
            return false;
        }
        // Strictly ascending: a repeated or out-of-order boundary would make the
        // segment covering a tick ambiguous.
        if (i > 0 &&
            segments[i].startTick.value <= segments[i - 1].startTick.value) {
            return false;
        }
    }

    segments_ = segments;
    segmentCount_ = segmentCount;
    sampleRateHz_ = sampleRateHz;
    valid_ = true;
    return true;
}

std::size_t ClockKernel::SegmentIndexForTick(std::int64_t tick) const {
    // The last segment whose startTick <= tick. A tick before the first boundary
    // (tick < 0) belongs to segment 0, extrapolated backward -- that is the
    // anacrusis region, and segment 0's tempo is the one in force there. Segment
    // count is expected to be tiny, so a linear scan is both clearest and fastest.
    std::size_t index = 0;
    for (std::size_t i = 1; i < segmentCount_; ++i) {
        if (segments_[i].startTick.value <= tick) {
            index = i;
        } else {
            break;
        }
    }
    return index;
}

std::int64_t ClockKernel::AnchorFrameForSegment(
    std::size_t index, std::uint64_t* remainderOut) const {
    // Carry the fractional numerator across every boundary so the anchor is the
    // floor of the exact accumulated rational position, not a sum of floors.
    std::int64_t anchor = 0;
    std::uint64_t remainder = 0;
    for (std::size_t i = 0; i < index; ++i) {
        const std::int64_t dtick = SatSub(segments_[i + 1].startTick.value,
                                          segments_[i].startTick.value);
        std::uint64_t nextRemainder = 0;
        const std::int64_t span =
            SpanFrames(sampleRateHz_, segments_[i].microsPerQuarter, dtick,
                       remainder, &nextRemainder);
        anchor = SatAdd(anchor, span);
        remainder = nextRemainder;
    }
    if (remainderOut != nullptr) {
        *remainderOut = remainder;
    }
    return anchor;
}

std::int64_t ClockKernel::AnchorTimeForSegment(
    std::size_t index, std::uint64_t* remainderOut) const {
    std::int64_t anchor = 0;
    std::uint64_t remainder = 0;
    for (std::size_t i = 0; i < index; ++i) {
        const std::int64_t dtick = SatSub(segments_[i + 1].startTick.value,
                                          segments_[i].startTick.value);
        std::uint64_t nextRemainder = 0;
        const std::int64_t span =
            SpanTime(segments_[i].microsPerQuarter, dtick, remainder,
                     &nextRemainder);
        anchor = SatAdd(anchor, span);
        remainder = nextRemainder;
    }
    if (remainderOut != nullptr) {
        *remainderOut = remainder;
    }
    return anchor;
}

SampleFrame ClockKernel::TickToFrame(MusicalTick tick) const {
    if (!valid_) {
        return SampleFrame{0};
    }
    const std::size_t index = SegmentIndexForTick(tick.value);
    std::uint64_t anchorRemainder = 0;
    const std::int64_t anchor =
        AnchorFrameForSegment(index, &anchorRemainder);
    const std::int64_t dtick =
        SatSub(tick.value, segments_[index].startTick.value);
    const std::int64_t span = SpanFrames(
        sampleRateHz_, segments_[index].microsPerQuarter, dtick,
        anchorRemainder, nullptr);
    return SampleFrame{SatAdd(anchor, span)};
}

MusicalTick ClockKernel::FrameToTick(SampleFrame frame) const {
    if (!valid_) {
        return MusicalTick{0};
    }
    // Find the last segment whose anchor frame is <= the queried frame, carrying
    // the running anchor forward as we go. A frame before the first anchor
    // (frame < 0) stays on segment 0, the anacrusis region.
    std::size_t index = 0;
    std::int64_t anchor = 0;
    std::uint64_t anchorRemainder = 0;
    for (std::size_t i = 1; i < segmentCount_; ++i) {
        const std::int64_t dtick = SatSub(segments_[i].startTick.value,
                                          segments_[i - 1].startTick.value);
        std::uint64_t nextRemainder = 0;
        const std::int64_t span = SpanFrames(
            sampleRateHz_, segments_[i - 1].microsPerQuarter, dtick,
            anchorRemainder, &nextRemainder);
        const std::int64_t nextAnchor = SatAdd(anchor, span);
        if (nextAnchor <= frame.value) {
            anchor = nextAnchor;
            anchorRemainder = nextRemainder;
            index = i;
        } else {
            break;
        }
    }

    const std::int64_t df = SatSub(frame.value, anchor);
    const std::int64_t rate = segments_[index].microsPerQuarter * sampleRateHz_;
    // Largest dtick with floor(dtick * rate / D) <= df, i.e. the tick whose
    // half-open frame interval contains df. The carried anchor remainder shifts
    // the inverse bias: floor((df*D + (D-1-rem)) / rate).
    Wide num = MulSigned(df, kTickFrameDenom);
    num = AddSignedNonNeg(
        num, static_cast<std::uint64_t>(kTickFrameDenom - 1) - anchorRemainder);
    const std::int64_t dtick = FloorDivClamp(num, static_cast<std::uint64_t>(rate));
    return MusicalTick{SatAdd(segments_[index].startTick.value, dtick)};
}

AbsoluteTime ClockKernel::TickToTime(MusicalTick tick) const {
    if (!valid_) {
        return AbsoluteTime{0};
    }
    const std::size_t index = SegmentIndexForTick(tick.value);
    std::uint64_t anchorRemainder = 0;
    const std::int64_t anchor =
        AnchorTimeForSegment(index, &anchorRemainder);
    const std::int64_t dtick =
        SatSub(tick.value, segments_[index].startTick.value);
    const std::int64_t span =
        SpanTime(segments_[index].microsPerQuarter, dtick,
                 anchorRemainder, nullptr);
    return AbsoluteTime{SatAdd(anchor, span)};
}

SampleFrame ClockKernel::TimeToFrame(AbsoluteTime time) const {
    if (!valid_) {
        return SampleFrame{0};
    }
    // floor(ns * sr / 1e9); entry is time 0 = frame 0, so there is no anchor.
    const Wide num = MulSigned(time.value, sampleRateHz_);
    return SampleFrame{
        FloorDivClamp(num, static_cast<std::uint64_t>(kNanosPerSecond))};
}

AbsoluteTime ClockKernel::FrameToTime(SampleFrame frame) const {
    if (!valid_) {
        return AbsoluteTime{0};
    }
    // The earliest nanosecond that lands on the frame is ceil(frame*1e9 / sr),
    // the exact inverse of TimeToFrame's floor. For a positive divisor,
    // ceil(a/den) = floor((a + den - 1)/den), which reuses the floor path and so
    // saturates to the correct int64 extreme (the negate-then-floor form would
    // land one unit short of INT64_MIN).
    Wide num = MulSigned(frame.value, kNanosPerSecond);
    num = AddSignedNonNeg(num, static_cast<std::uint64_t>(sampleRateHz_ - 1));
    return AbsoluteTime{
        FloorDivClamp(num, static_cast<std::uint64_t>(sampleRateHz_))};
}

bool ClockKernel::TryTimeSpanToFrames(AbsoluteTime start,
                                      std::int64_t length,
                                      std::uint64_t* outFrames) const {
    if (!valid_ || outFrames == nullptr || length < 0 ||
        start.value > INT64_MAX - length) {
        return false;
    }
    const SampleFrame first = TimeToFrame(start);
    const SampleFrame last = TimeToFrame(AbsoluteTime{start.value + length});
    if (last.value < first.value ||
        (length != 0 &&
         (first.value == INT64_MIN || first.value == INT64_MAX ||
          last.value == INT64_MIN || last.value == INT64_MAX))) {
        return false;
    }
    *outFrames = static_cast<std::uint64_t>(last.value - first.value);
    return true;
}

bool ClockKernel::TryTickSpanToFrames(MusicalTick start,
                                      std::int64_t length,
                                      std::uint64_t* outFrames) const {
    if (!valid_ || outFrames == nullptr || length < 0 ||
        start.value > INT64_MAX - length) {
        return false;
    }
    const SampleFrame first = TickToFrame(start);
    const SampleFrame last = TickToFrame(MusicalTick{start.value + length});
    if (last.value < first.value ||
        (length != 0 &&
         (first.value == INT64_MIN || first.value == INT64_MAX ||
          last.value == INT64_MIN || last.value == INT64_MAX))) {
        return false;
    }
    *outFrames = static_cast<std::uint64_t>(last.value - first.value);
    return true;
}

} // namespace harken
