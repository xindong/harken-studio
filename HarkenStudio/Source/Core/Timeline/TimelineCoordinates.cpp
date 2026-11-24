#include "TimelineCoordinates.h"

#include <cmath>
#include <limits>

namespace harken::studio {
namespace {

uint64_t magnitude(int64_t value)
{
    if (value >= 0) return static_cast<uint64_t>(value);
    return static_cast<uint64_t>(-(value + 1)) + 1;
}

CoordinateResult roundScaledDouble(double value,
                                   int64_t scale,
                                   const QString& unit)
{
    if (!std::isfinite(value))
        return CoordinateResult::failure(
            TimelineError::CoordinateOverflow,
            QStringLiteral("%1 must be finite").arg(unit));
    const double scaled = value * static_cast<double>(scale);
    const double rounded = scaled < 0.0
        ? std::ceil(scaled - 0.5)
        : std::floor(scaled + 0.5);
    const double signedLimit = std::ldexp(1.0, 63);
    if (rounded < -signedLimit || rounded >= signedLimit)
        return CoordinateResult::failure(
            TimelineError::CoordinateOverflow,
            QStringLiteral("%1 exceed the timeline coordinate range").arg(unit));
    return CoordinateResult::success(static_cast<int64_t>(rounded));
}

}

CoordinateResult CoordinateResult::success(int64_t value)
{
    return {value, TimelineValidation::success()};
}

CoordinateResult CoordinateResult::failure(TimelineError error,
                                           const QString& message)
{
    return {0, TimelineValidation::failure(error, message)};
}

namespace TimelineCoordinates {

double toSeconds(int64_t coordinate)
{
    return static_cast<double>(coordinate) / static_cast<double>(kNanosecondsPerSecond);
}

CoordinateResult fromSeconds(double seconds)
{
    return roundScaledDouble(seconds, kNanosecondsPerSecond, QStringLiteral("Seconds"));
}

CoordinateResult snap(int64_t coordinate, int64_t gridSize)
{
    if (gridSize <= 0)
        return CoordinateResult::failure(
            TimelineError::InvalidTimeSpec,
            QStringLiteral("Snap grid must be positive"));
    const int64_t remainder = coordinate % gridSize;
    if (remainder == 0) return CoordinateResult::success(coordinate);
    const uint64_t distance = magnitude(remainder);
    const uint64_t grid = static_cast<uint64_t>(gridSize);
    const bool away = distance >= grid / 2 + grid % 2;
    const int64_t base = coordinate - remainder;
    if (!away) return CoordinateResult::success(base);
    if (coordinate > 0) {
        if (base > std::numeric_limits<int64_t>::max() - gridSize)
            return CoordinateResult::failure(
                TimelineError::CoordinateOverflow,
                QStringLiteral("Snapped coordinate overflows"));
        return CoordinateResult::success(base + gridSize);
    }
    if (base < std::numeric_limits<int64_t>::min() + gridSize)
        return CoordinateResult::failure(
            TimelineError::CoordinateOverflow,
            QStringLiteral("Snapped coordinate overflows"));
    return CoordinateResult::success(base - gridSize);
}

}
}
