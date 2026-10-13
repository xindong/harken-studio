// The authoritative conversion between musical/absolute time and sample frames.
//
// Everything downstream -- when a note is scheduled, where a weak-up starts, how a
// transition aligns to a bar -- is expressed as a frame, and every one of those
// frames comes from here. So the rule this kernel follows is: one exact integer
// mapping, no floating point anywhere on the path, and every rounding, sign and
// overflow decision spelled out rather than left to whatever the arithmetic
// happened to do. A drift of a single sample per bar is inaudible for one bar and
// a train wreck after an hour; the only way to be sure it never happens is to
// never accumulate.
//
// Three coordinates, all relative to the piece's entry:
//   MusicalTick   signed int64 ticks, 960 per quarter note (PPQ).
//   AbsoluteTime  signed int64 nanoseconds.
//   SampleFrame   signed int64 sample frames.
// Entry is the origin of all three: tick 0, time 0 and frame 0 are the same
// instant. Anacrusis (a weak-up that must sound *before* the downbeat) lives at
// negative coordinates, which is why the frame result is signed even though the
// engine's authoritative clock, MixFrame, is unsigned. Turning a SampleFrame into
// a MixFrame -- adding the entry's anchor frame and clamping the result at zero --
// is the consumer's step, not this kernel's: the kernel does not know where on
// the global timeline a given piece was placed.
//
// Rounding is floor (toward negative infinity) in every direction, so the
// mapping is a monotonic non-decreasing step function whose behaviour at zero and
// below is the same shape as above it -- no special case flips sign at the origin.
// A tick occupies the half-open frame interval [TickToFrame(t), TickToFrame(t+1)),
// and FrameToTick answers "which tick is sounding at this frame", i.e. the unique
// t whose interval contains the frame. Reset requires at least one frame per tick,
// so TickToFrame is strictly increasing and the non-saturating round trip
// TickToFrame -> FrameToTick recovers the original tick exactly, with no
// tempo-dependent exception. The reverse trip (frame -> tick -> frame) is lossy
// by construction, because many frames sit inside one tick; it returns that
// tick's start frame. Nanoseconds are finer than frames, so the absolute
// directions are mirror images: FrameToTime -> TimeToFrame recovers the frame
// exactly, while TimeToFrame -> FrameToTime returns the frame's start nanosecond.
// TickToTime floors the exact rational musical position directly in nanoseconds;
// it does not pass through a sample frame and therefore preserves sub-frame time.
//
// Overflow is saturating, never wrapping and never undefined: an input whose exact
// converted frame or time position would fall outside the int64 range clamps to
// INT64_MIN or INT64_MAX. Real content never approaches that; the guarantee
// exists so a corrupt or adversarial position degrades to a defined edge value
// instead of a wrapped one that reads as the far past or future.
//
// Accepted domain (Reset returns false otherwise): sample rate in
// [1, 3'000'000] Hz, every tempo segment's microseconds-per-quarter in
// [1, 60'000'000'000], and microsPerQuarter * sampleRateHz >= 960'000'000.
// The joint lower bound guarantees at least one frame per tick, strict monotonic
// tick mapping, and exact tick -> frame -> tick round trips before saturation.
// The upper bounds are wide enough for any musical use and keep internal products
// within a 128-bit intermediate without a third limb.
#ifndef HARKEN_CORE_CLOCK_KERNEL_H
#define HARKEN_CORE_CLOCK_KERNEL_H

#include <cstddef>
#include <cstdint>

namespace harken {

// Pulses (ticks) per quarter note. The musical grid's resolution; fixed, because
// a variable PPQ would make two authored positions incomparable without also
// carrying the resolution they were authored at.
inline constexpr std::int64_t kClockPulsesPerQuarter = 960;

// A musical position in ticks, relative to entry. Signed: a weak-up before the
// entry downbeat is a negative tick.
struct MusicalTick {
    std::int64_t value = 0;
};

// An absolute position in nanoseconds, relative to entry. Signed for the same
// reason as MusicalTick.
struct AbsoluteTime {
    std::int64_t value = 0;
};

// A sample-frame position relative to entry. Signed and entry-relative; see the
// file header for why it is not the engine's unsigned MixFrame.
struct SampleFrame {
    std::int64_t value = 0;
};

inline bool operator==(MusicalTick a, MusicalTick b) { return a.value == b.value; }
inline bool operator!=(MusicalTick a, MusicalTick b) { return !(a == b); }
inline bool operator==(AbsoluteTime a, AbsoluteTime b) { return a.value == b.value; }
inline bool operator!=(AbsoluteTime a, AbsoluteTime b) { return !(a == b); }
inline bool operator==(SampleFrame a, SampleFrame b) { return a.value == b.value; }
inline bool operator!=(SampleFrame a, SampleFrame b) { return !(a == b); }
inline bool operator<(SampleFrame a, SampleFrame b) { return a.value < b.value; }

// One entry in a tempo map: from startTick (inclusive) until the next segment's
// startTick (exclusive), the tempo is microsPerQuarter microseconds per quarter
// note. The tempo is stored as integer microseconds, not BPM, because BPM is a
// reciprocal that cannot represent every microsecond value exactly and the whole
// point here is exactness. The initial scope uses a single segment starting at
// tick 0; the type is a map so variable tempo needs no second interface, not
// because the constant case is special-cased away.
struct TempoSegment {
    MusicalTick startTick{};
    std::int64_t microsPerQuarter = 0;
};

// Converts musical and absolute positions to sample frames and back, under one
// tempo map and one sample rate. Pure integer arithmetic: no threads, no I/O, no
// allocation, no floating point, and no per-call state -- the same inputs always
// produce bit-identical outputs, on any toolchain. The kernel is a non-owning
// view over the caller's tempo segments; they must outlive it. It is built for a
// fixed sample rate because a sample-rate change invalidates every frame mapping
// and is handled a level up by raising a transport epoch and rebuilding, not by
// mutating a kernel in place.
class ClockKernel {
public:
    ClockKernel() = default;

    // Binds the kernel to a sample rate and a tempo map. The map must be
    // non-empty, sorted strictly ascending by startTick, start at tick 0, and
    // carry only in-domain tempos (see the file header). Returns true and makes
    // the kernel valid on success; returns false and leaves it invalid otherwise.
    // Non-owning: `segments` is borrowed, not copied.
    bool Reset(std::int64_t sampleRateHz, const TempoSegment* segments,
               std::size_t segmentCount);

    // False before a successful Reset. Every conversion on an invalid kernel maps
    // to frame/tick/time 0, so an unwired kernel degrades to a defined constant
    // rather than reading uninitialized state.
    bool Valid() const { return valid_; }

    std::int64_t SampleRateHz() const { return sampleRateHz_; }
    std::size_t SegmentCount() const { return segmentCount_; }

    // Musical <-> frame. TickToFrame floors; FrameToTick returns the tick whose
    // half-open frame interval contains the frame (see the file header).
    SampleFrame TickToFrame(MusicalTick tick) const;
    MusicalTick FrameToTick(SampleFrame frame) const;

    // Musical -> absolute time. Floors the exact entry-relative rational
    // nanosecond position under the tempo map, carrying fractional nanoseconds
    // across segment boundaries instead of quantizing through a sample frame.
    AbsoluteTime TickToTime(MusicalTick tick) const;

    // Absolute <-> frame. Independent of the tempo map: only the sample rate
    // relates nanoseconds to frames. TimeToFrame floors; FrameToTime returns the
    // earliest nanosecond that lands on the frame (its exact inverse).
    SampleFrame TimeToFrame(AbsoluteTime time) const;
    AbsoluteTime FrameToTime(SampleFrame frame) const;
    bool TryTimeSpanToFrames(AbsoluteTime start, std::int64_t length,
                             std::uint64_t* outFrames) const;
    bool TryTickSpanToFrames(MusicalTick start, std::int64_t length,
                             std::uint64_t* outFrames) const;

private:
    // The entry-relative frame at a segment's startTick, formed by exact integer
    // accumulation of the earlier segments' spans. Not stored: for a
    // single-segment map it is zero, and for a multi-segment map the accumulation
    // is a short exact integer sum with no floating point to drift. `remainder`
    // receives the accumulated numerator modulo the shared tick-frame denominator.
    std::int64_t AnchorFrameForSegment(std::size_t index,
                                       std::uint64_t* remainder) const;
    std::int64_t AnchorTimeForSegment(std::size_t index,
                                      std::uint64_t* remainder) const;
    std::size_t SegmentIndexForTick(std::int64_t tick) const;

    const TempoSegment* segments_ = nullptr;
    std::size_t segmentCount_ = 0;
    std::int64_t sampleRateHz_ = 0;
    bool valid_ = false;
};

} // namespace harken

#endif // HARKEN_CORE_CLOCK_KERNEL_H
