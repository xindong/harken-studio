#include "harken_project_schema/CapacityEstimate.h"

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <optional>
#include <utility>
#include <vector>

namespace harkenprojectschema {
namespace {

constexpr std::int64_t kNanosPerMicro = 1000;
constexpr std::int64_t kMaxI64 = (std::numeric_limits<std::int64_t>::max)();
constexpr std::int64_t kMinI64 = (std::numeric_limits<std::int64_t>::min)();

// floor(a / b) for b > 0, correct when a is negative (a pickup at a negative
// tick). Plain C++ division truncates toward zero, which is not the floor for
// negatives; this keeps the mapping monotone.
std::int64_t FloorDiv(std::int64_t a, std::int64_t b) {
    std::int64_t quotient = a / b;
    const std::int64_t remainder = a % b;
    if (remainder != 0 && ((remainder < 0) != (b < 0))) {
        --quotient;
    }
    return quotient;
}

// A musical tick to nanoseconds under a constant tempo, flooring the result and
// refusing rather than wrapping when the product would overflow int64. This is
// the rejection bound the estimate uses in place of a 128-bit intermediate: it
// is portable across the toolchains this library builds under and turns an
// out-of-range authored coordinate into a deterministic refusal.
//
//   ns = tick * (microsPerQuarter * 1000) / 960
std::optional<std::int64_t> TickToNanosFloor(std::int64_t tick,
                                             std::int64_t microsPerQuarter) {
    if (microsPerQuarter <= 0) {
        return std::nullopt;
    }
    if (microsPerQuarter > kMaxI64 / kNanosPerMicro) {
        return std::nullopt;
    }
    const std::int64_t nsPerQuarter = microsPerQuarter * kNanosPerMicro;
    if (tick == kMinI64) {
        return std::nullopt;  // magnitude not representable for the bound check
    }
    const std::int64_t magnitude = tick < 0 ? -tick : tick;
    if (magnitude > kMaxI64 / nsPerQuarter) {
        return std::nullopt;
    }
    return FloorDiv(tick * nsPerQuarter, kMusicalPpq);
}

// a + b, refusing on signed overflow.
std::optional<std::int64_t> AddNanos(std::int64_t a, std::int64_t b) {
    if (b >= 0) {
        if (a > kMaxI64 - b) {
            return std::nullopt;
        }
    } else if (a < kMinI64 - b) {
        return std::nullopt;
    }
    return a + b;
}

// The peak number of half-open [start, end) intervals that overlap. Ends are
// processed before starts at a shared coordinate, so intervals that merely abut
// do not count as overlapping.
std::uint32_t PeakOverlap(
    const std::vector<std::pair<std::int64_t, std::int64_t>>& intervals) {
    std::vector<std::pair<std::int64_t, int>> events;
    events.reserve(intervals.size() * 2);
    for (const auto& interval : intervals) {
        if (interval.second <= interval.first) {
            continue;  // a zero-length interval overlaps nothing
        }
        events.emplace_back(interval.first, 1);
        events.emplace_back(interval.second, -1);
    }
    std::sort(events.begin(), events.end(),
              [](const std::pair<std::int64_t, int>& a,
                 const std::pair<std::int64_t, int>& b) {
                  if (a.first != b.first) {
                      return a.first < b.first;
                  }
                  return a.second < b.second;  // -1 (end) before +1 (start)
              });
    int running = 0;
    int peak = 0;
    for (const auto& event : events) {
        running += event.second;
        if (running > peak) {
            peak = running;
        }
    }
    return static_cast<std::uint32_t>(peak);
}

struct ScopeAccum {
    bool isPiece = false;
    PieceTimeKind timeKind = PieceTimeKind::Absolute;
    std::int64_t microsPerQuarter = 0;
    std::vector<std::pair<std::int64_t, std::int64_t>> intervals;
    std::uint32_t unknown = 0;
};

}  // namespace

std::optional<CapacityEstimate> EstimateCapacity(const PieceIR& ir) {
    // Which Piece, if any, owns each Track. A Track in a Piece shares that
    // Piece's clock and origin; any other Track is its own scope in absolute ns.
    std::map<StableId, const IrPiece*> ownerOf;
    for (const IrPiece& piece : ir.pieces()) {
        for (const StableId trackId : piece.tracks) {
            ownerOf.emplace(trackId, &piece);
        }
    }

    std::map<StableId, ScopeAccum> scopes;
    for (const IrTrack& track : ir.tracks()) {
        const auto ownerIt = ownerOf.find(track.trackId);
        const bool inPiece = ownerIt != ownerOf.end();
        const StableId scopeId =
            inPiece ? ownerIt->second->pieceId : track.trackId;
        ScopeAccum& scope = scopes[scopeId];
        scope.isPiece = inPiece;
        if (inPiece) {
            const IrPiece& piece = *ownerIt->second;
            scope.timeKind = piece.timeSpec.kind;
            if (piece.timeSpec.kind == PieceTimeKind::Musical) {
                if (piece.timeSpec.tempoMap.size() != 1) {
                    return std::nullopt;  // not the constant-map shape
                }
                scope.microsPerQuarter =
                    piece.timeSpec.tempoMap.front().microsPerQuarter;
            }
        } else {
            scope.timeKind = PieceTimeKind::Absolute;
        }

        for (const IrOccurrence& occ : track.occurrences) {
            std::int64_t startNs = occ.time;  // absolute ns by default
            if (scope.timeKind == PieceTimeKind::Musical) {
                const std::optional<std::int64_t> converted =
                    TickToNanosFloor(occ.time, scope.microsPerQuarter);
                if (!converted.has_value()) {
                    return std::nullopt;
                }
                startNs = *converted;
            }
            if (!occ.hasDuration) {
                ++scope.unknown;
                continue;
            }
            const std::optional<std::int64_t> endNs =
                AddNanos(startNs, occ.durationNs);
            if (!endNs.has_value()) {
                return std::nullopt;
            }
            scope.intervals.emplace_back(startNs, *endNs);
        }
    }

    CapacityEstimate estimate;
    for (const auto& [scopeId, scope] : scopes) {
        if (scope.intervals.empty() && scope.unknown == 0) {
            continue;  // a scope with no occurrences contributes nothing
        }
        ScopeCapacity cap;
        cap.isPiece = scope.isPiece;
        cap.scopeId = scopeId;
        cap.peakActiveOccurrences = PeakOverlap(scope.intervals);
        cap.unknownDurationOccurrences = scope.unknown;
        estimate.activeUpperBound += cap.peakActiveOccurrences;
        estimate.unknownDurationOccurrences += cap.unknownDurationOccurrences;
        estimate.scopes.push_back(cap);
    }
    estimate.activeIsLowerBound = estimate.unknownDurationOccurrences > 0;
    return estimate;
}

}  // namespace harkenprojectschema
