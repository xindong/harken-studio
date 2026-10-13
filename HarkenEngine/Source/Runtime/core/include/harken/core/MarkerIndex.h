// The per-Piece marker index: the immutable table a planner reads to answer "where
// is marker C" and "what is the next bar / beat / marker after here", with every answer
// available both as a musical/absolute position and as the sample frame it maps
// to under the Piece's own clock.
//
// Entry is a sentinel, not a row. Every Piece's entry marker is local position zero
// -- tick 0 for a Musical Piece, nanosecond 0 for an Absolute one -- so it never
// needs storing. ByMarkerId(0) always reports absent. The table holds the custom and
// exit markers, each named by a non-zero MarkerId (zero is the entry sentinel and is
// refused as a stored id), and a Piece has at most one exit marker.
//
// One clock, chosen once. A Musical Piece converts a tick position to a frame
// through the tempo map (ClockKernel::TickToFrame); an Absolute Piece converts a
// nanosecond position through the sample rate alone (ClockKernel::TimeToFrame).
// The index is bound to one ClockKernel and one time type at Reset and does not
// change either afterward, because a Piece's clock is fixed for the life of its
// program.
//
// The grid is arithmetic under a constant, single-row meter: a bar is
// numerator*PPQ*4/denominator ticks and a beat is PPQ*4/denominator ticks, so the
// next bar or beat after a position is a division, not a search. At the end of the
// representable int64 axis there may be no greater grid line. The next *marker* after
// a position may likewise not exist, so each query returns an explicit
// "no candidate" instead of fabricating a position or searching forever. A
// consumer's late policy (drop, refuse, ...) is the consumer's decision; this
// index only reports whether a candidate exists and where.
//
// Pure integer, no floating point, no allocation after Reset, deterministic. Not
// this index's job: deciding what to do when a boundary is missing, evaluating
// variables, or owning the tempo map (it borrows the clock).
#ifndef HARKEN_CORE_MARKER_INDEX_H
#define HARKEN_CORE_MARKER_INDEX_H

#include "harken/core/ClockKernel.h"
#include "harken/core/TimeCoordinates.h"

#include <cstddef>
#include <harken/ObjectId.h>

#include <cstdint>
#include <vector>

namespace harken {

// A marker's stable identifier within its Piece. Zero is reserved for the entry
// sentinel and is never a stored marker.
using MarkerId = VnObjectId;

inline constexpr MarkerId kEntryMarkerId{};

// PieceTimeType (Musical / Absolute) is shared with the same-position resolver;
// see harken/core/TimeCoordinates.h.

// The two kinds of stored marker. Entry is not among them: it is the sentinel at
// position zero and is never stored.
enum class MarkerKind : std::uint8_t {
    Custom = 1,
    Exit = 2
};

// One stored marker: its id, kind, and entry-relative position in the Piece's native
// unit (tick for Musical, nanosecond for Absolute). Positions may be negative:
// authored exit/custom markers are relative to entry and may precede it, so a
// pre-entry (anacrusis-region) marker is carried through as given rather than
// refused.
struct MarkerPoint {
    MarkerId id = kEntryMarkerId;
    MarkerKind kind = MarkerKind::Custom;
    std::int64_t position = 0;
};

// A constant meter. A bar is numerator*PPQ*4/denominator ticks; a beat is
// PPQ*4/denominator ticks. Both must divide to a whole number of ticks (true for
// every standard meter, whose denominator divides PPQ*4), which Reset enforces.
struct MeterSignature {
    std::int32_t numerator = 4;
    std::int32_t denominator = 4;
};

// Which grid a boundary query walks.
enum class BoundaryType : std::uint8_t {
    Bar = 0,
    Beat = 1,
    Marker = 2
};

// The next boundary after a queried position. hasCandidate is false when none
// exists -- a Bar/Beat query on an Absolute Piece, a musical grid at the end of
// the representable axis, or a Marker query with no later marker. When true, position
// is in the Piece's native unit and frame is its mapping.
struct BoundaryCandidate {
    bool hasCandidate = false;
    std::int64_t position = 0;
    SampleFrame frame{};
};

// The result of a marker lookup: present says whether the marker exists; the rest is
// meaningful only when it does.
struct MarkerLookup {
    bool present = false;
    MarkerKind kind = MarkerKind::Custom;
    std::int64_t position = 0;
    SampleFrame frame{};
};

class MarkerIndex {
public:
    MarkerIndex() = default;

    // Binds the index to a Piece's time type, clock and meter, and copies its markers
    // into storage sized for `capacity`. Returns false (leaving the index invalid)
    // when: the clock is null or invalid; capacity is zero or fewer than the marker
    // count; the time type or a marker kind is outside its declared enum; any marker
    // carries the reserved entry id 0; two markers share an id; more than one exit marker
    // is present; or (Musical only) the meter is non-positive or does not divide
    // to whole bar/beat ticks. Copies, so the caller's array need not outlive the
    // index; the clock is borrowed and must outlive it.
    bool Reset(PieceTimeType timeType, const ClockKernel* clock,
               MeterSignature meter, const MarkerPoint* markers,
               std::size_t markerCount, std::size_t capacity);

    bool Valid() const { return valid_; }
    PieceTimeType TimeType() const { return timeType_; }
    std::size_t MarkerCount() const { return markerCount_; }
    std::size_t Capacity() const { return markers_.size(); }

    // The Piece's native position converted to a frame under its clock. Musical
    // positions floor through the tempo map; absolute positions through the
    // sample rate. On an invalid index this is frame 0.
    SampleFrame FrameOfPosition(std::int64_t position) const;

    // Ticks per bar and per beat, or 0 for an Absolute Piece (which has no musical
    // grid). Both are whole numbers by Reset's validation.
    std::int64_t BarTicks() const { return barTicks_; }
    std::int64_t BeatTicks() const { return beatTicks_; }

    // The stored marker with this id, or {present=false} for an unknown id (including
    // the reserved entry id 0). Zero allocation.
    MarkerLookup ByMarkerId(MarkerId id) const;

    // The stored marker exactly at this native position, or {present=false} if none.
    // When several markers share a position (allowed only if they differ in id) the
    // earliest in the sorted order is returned.
    MarkerLookup ByPosition(std::int64_t position) const;

    // The Piece's single exit marker, or {present=false} if it has none.
    MarkerLookup ExitMarker() const;

    // The next boundary strictly after fromPosition. For Bar/Beat this is the next
    // representable grid line on a Musical Piece; an Absolute Piece has no grid,
    // and the final representable musical line has no successor. For Marker it is the
    // next stored marker by position. When none exists, hasCandidate is false.
    BoundaryCandidate NextBoundary(std::int64_t fromPosition,
                                   BoundaryType type) const;

private:
    // Writes the smallest representable multiple of `grid` strictly greater than
    // `from`, or returns false when no such int64 value exists. grid is positive.
    static bool NextGridLine(std::int64_t from, std::int64_t grid,
                             std::int64_t* next);

    PieceTimeType timeType_ = PieceTimeType::Musical;
    const ClockKernel* clock_ = nullptr;
    std::int64_t barTicks_ = 0;
    std::int64_t beatTicks_ = 0;
    std::size_t markerCount_ = 0;
    bool valid_ = false;

    // Markers sorted ascending by (position, id), so a by-position query and the marker
    // grid read deterministically. Sized to capacity at Reset.
    std::vector<MarkerPoint> markers_;
};

} // namespace harken

#endif // HARKEN_CORE_MARKER_INDEX_H
