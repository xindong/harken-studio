#pragma once

#include "TimelineTypes.h"

#include <cstdint>

namespace harken::studio {

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

inline constexpr int64_t kNanosecondsPerSecond = 1000000000;

double toSeconds(int64_t coordinate);
CoordinateResult fromSeconds(double seconds);
CoordinateResult snap(int64_t coordinate, int64_t gridSize);

}
}
