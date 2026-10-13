#pragma once

#include "TimelineTypes.h"

#include <cstdint>
#include <optional>

namespace VNX {

struct CoordinateResult
{
    int64_t value = 0;
    TimelineValidation validation;

    bool ok() const { return validation.ok(); }
    explicit operator bool() const { return ok(); }

    static CoordinateResult success(int64_t value);
    static CoordinateResult failure(TimelineError error, const QString& message);
};

namespace TimelineCoordinates {

inline constexpr int64_t kPpq = 960;
inline constexpr int64_t kNanosecondsPerSecond = 1000000000;

TimelineValidation validate(const PieceTimeSpec& spec);
CoordinateResult ticksToNanoseconds(int64_t ticks, const PieceTimeSpec& spec);
CoordinateResult nanosecondsToTicks(int64_t nanoseconds, const PieceTimeSpec& spec);
CoordinateResult toNanoseconds(int64_t coordinate, const PieceTimeSpec& spec);
CoordinateResult fromNanoseconds(int64_t nanoseconds, const PieceTimeSpec& spec);
std::optional<double> ticksToBeats(int64_t ticks);
CoordinateResult beatsToTicks(double beats);
std::optional<double> toSeconds(int64_t coordinate, const PieceTimeSpec& spec);
CoordinateResult fromSeconds(double seconds, const PieceTimeSpec& spec);
CoordinateResult snap(int64_t coordinate, TimelineDomain domain);
CoordinateResult snap(int64_t coordinate, const PieceTimeSpec& spec);
CoordinateResult snap(int64_t coordinate, int64_t gridSize);
CoordinateResult convert(int64_t coordinate,
                         const PieceTimeSpec& source,
                         const PieceTimeSpec& destination);

}
}
