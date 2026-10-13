#include "harken/core/MarkerIndex.h"

#include <algorithm>
#include <cstdint>

namespace harken {
namespace {

// PPQ * 4: the tick length of a whole note, from which a bar and a beat divide.
constexpr std::int64_t kTicksPerWholeNote = kClockPulsesPerQuarter * 4; // 3840

// floor(a / b) for a positive divisor, correct for negative a (C++ integer
// division truncates toward zero, which is not floor below the origin).
std::int64_t FloorDiv(std::int64_t a, std::int64_t b) {
    std::int64_t q = a / b;
    if ((a % b) != 0 && a < 0) {
        q -= 1;
    }
    return q;
}

} // namespace

bool MarkerIndex::Reset(PieceTimeType timeType, const ClockKernel* clock,
                     MeterSignature meter, const MarkerPoint* markers,
                     std::size_t markerCount, std::size_t capacity) {
    timeType_ = PieceTimeType::Musical;
    clock_ = nullptr;
    barTicks_ = 0;
    beatTicks_ = 0;
    markerCount_ = 0;
    valid_ = false;
    markers_.clear();

    if (timeType != PieceTimeType::Musical &&
        timeType != PieceTimeType::Absolute) {
        return false;
    }
    if (clock == nullptr || !clock->Valid()) {
        return false;
    }
    if (capacity == 0 || markerCount > capacity) {
        return false;
    }
    if (markers == nullptr && markerCount != 0) {
        return false;
    }

    std::size_t exitCount = 0;
    for (std::size_t i = 0; i < markerCount; ++i) {
        if (markers[i].kind != MarkerKind::Custom &&
            markers[i].kind != MarkerKind::Exit) {
            return false;
        }
        if (markers[i].id == kEntryMarkerId) {
            return false; // the entry sentinel is never a stored marker
        }
        if (markers[i].kind == MarkerKind::Exit) {
            ++exitCount;
            if (exitCount > 1) {
                return false; // at most one exit marker per Piece
            }
        }
        for (std::size_t j = 0; j < i; ++j) {
            if (markers[j].id == markers[i].id) {
                return false; // ids identify a marker, so they must be unique
            }
        }
    }

    if (timeType == PieceTimeType::Musical) {
        if (meter.numerator <= 0 || meter.denominator <= 0) {
            return false;
        }
        // A beat (and therefore a bar) must be a whole number of ticks; every
        // standard meter's denominator divides PPQ*4.
        if (kTicksPerWholeNote % meter.denominator != 0) {
            return false;
        }
        beatTicks_ = kTicksPerWholeNote / meter.denominator;
        barTicks_ = static_cast<std::int64_t>(meter.numerator) * beatTicks_;
    } else {
        beatTicks_ = 0;
        barTicks_ = 0;
    }

    markers_.assign(capacity, MarkerPoint{});
    for (std::size_t i = 0; i < markerCount; ++i) {
        markers_[i] = markers[i];
    }
    std::sort(markers_.begin(), markers_.begin() + static_cast<std::ptrdiff_t>(markerCount),
              [](const MarkerPoint& a, const MarkerPoint& b) {
                  if (a.position != b.position) {
                      return a.position < b.position;
                  }
                  return a.id < b.id;
              });

    timeType_ = timeType;
    clock_ = clock;
    markerCount_ = markerCount;
    valid_ = true;
    return true;
}

SampleFrame MarkerIndex::FrameOfPosition(std::int64_t position) const {
    if (!valid_) {
        return SampleFrame{0};
    }
    if (timeType_ == PieceTimeType::Musical) {
        return clock_->TickToFrame(MusicalTick{position});
    }
    return clock_->TimeToFrame(AbsoluteTime{position});
}

MarkerLookup MarkerIndex::ByMarkerId(MarkerId id) const {
    MarkerLookup result;
    if (!valid_ || id == kEntryMarkerId) {
        return result;
    }
    for (std::size_t i = 0; i < markerCount_; ++i) {
        if (markers_[i].id == id) {
            result.present = true;
            result.kind = markers_[i].kind;
            result.position = markers_[i].position;
            result.frame = FrameOfPosition(markers_[i].position);
            return result;
        }
    }
    return result;
}

MarkerLookup MarkerIndex::ByPosition(std::int64_t position) const {
    MarkerLookup result;
    if (!valid_) {
        return result;
    }
    // Markers are sorted by (position, id); the first match is the earliest-id marker at
    // that position.
    for (std::size_t i = 0; i < markerCount_; ++i) {
        if (markers_[i].position == position) {
            result.present = true;
            result.kind = markers_[i].kind;
            result.position = position;
            result.frame = FrameOfPosition(position);
            return result;
        }
        if (markers_[i].position > position) {
            break; // sorted: no exact match beyond here
        }
    }
    return result;
}

MarkerLookup MarkerIndex::ExitMarker() const {
    MarkerLookup result;
    if (!valid_) {
        return result;
    }
    for (std::size_t i = 0; i < markerCount_; ++i) {
        if (markers_[i].kind == MarkerKind::Exit) {
            result.present = true;
            result.kind = MarkerKind::Exit;
            result.position = markers_[i].position;
            result.frame = FrameOfPosition(markers_[i].position);
            return result;
        }
    }
    return result;
}

bool MarkerIndex::NextGridLine(std::int64_t from, std::int64_t grid,
                            std::int64_t* next) {
    // Smallest representable multiple of grid strictly greater than `from`.
    const std::int64_t q = FloorDiv(from, grid);
    if (q >= INT64_MAX / grid) {
        return false;
    }
    *next = (q + 1) * grid;
    return true;
}

BoundaryCandidate MarkerIndex::NextBoundary(std::int64_t fromPosition,
                                         BoundaryType type) const {
    BoundaryCandidate result;
    if (!valid_) {
        return result;
    }
    switch (type) {
        case BoundaryType::Bar:
        case BoundaryType::Beat: {
            if (timeType_ != PieceTimeType::Musical) {
                return result; // an Absolute Piece has no musical grid
            }
            const std::int64_t grid =
                (type == BoundaryType::Bar) ? barTicks_ : beatTicks_;
            if (grid <= 0) {
                return result;
            }
            if (!NextGridLine(fromPosition, grid, &result.position)) {
                return result;
            }
            result.frame = FrameOfPosition(result.position);
            result.hasCandidate = true;
            return result;
        }
        case BoundaryType::Marker: {
            // The next stored marker strictly after the position; none is a defined
            // "no candidate", not an unbounded search.
            for (std::size_t i = 0; i < markerCount_; ++i) {
                if (markers_[i].position > fromPosition) {
                    result.hasCandidate = true;
                    result.position = markers_[i].position;
                    result.frame = FrameOfPosition(markers_[i].position);
                    return result;
                }
            }
            return result;
        }
    }
    return result;
}

} // namespace harken
