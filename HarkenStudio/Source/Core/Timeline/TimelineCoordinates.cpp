#include "TimelineCoordinates.h"

#include <cmath>
#include <limits>

namespace VNX {
namespace {

uint64_t magnitude(int64_t value)
{
    if (value >= 0) return static_cast<uint64_t>(value);
    return static_cast<uint64_t>(-(value + 1)) + 1;
}

struct Wide
{
    uint64_t high = 0;
    uint64_t low = 0;
};

Wide multiplyWide(uint64_t left, uint64_t right)
{
    const uint64_t leftLow = left & 0xffffffffULL;
    const uint64_t leftHigh = left >> 32;
    const uint64_t rightLow = right & 0xffffffffULL;
    const uint64_t rightHigh = right >> 32;
    const uint64_t lowLow = leftLow * rightLow;
    const uint64_t lowHigh = leftLow * rightHigh;
    const uint64_t highLow = leftHigh * rightLow;
    const uint64_t highHigh = leftHigh * rightHigh;
    const uint64_t cross = (lowLow >> 32)
        + (lowHigh & 0xffffffffULL) + (highLow & 0xffffffffULL);
    return {
        highHigh + (lowHigh >> 32) + (highLow >> 32) + (cross >> 32),
        (lowLow & 0xffffffffULL) | (cross << 32)
    };
}

bool divideWide(Wide numerator,
                uint64_t divisor,
                uint64_t* quotient,
                uint64_t* remainder)
{
    if (!quotient || !remainder || divisor == 0 || numerator.high >= divisor)
        return false;
    uint64_t result = 0;
    uint64_t rest = numerator.high;
    for (int bit = 63; bit >= 0; --bit) {
        const bool carry = (rest >> 63) != 0;
        rest = (rest << 1) | ((numerator.low >> bit) & 1ULL);
        if (carry || rest >= divisor) {
            rest -= divisor;
            result |= uint64_t{1} << bit;
        }
    }
    *quotient = result;
    *remainder = rest;
    return true;
}

CoordinateResult roundMultiplyDivide(int64_t value,
                                     uint64_t multiplier,
                                     uint64_t divisor)
{
    if (divisor == 0)
        return CoordinateResult::failure(
            TimelineError::InvalidTimeSpec, QStringLiteral("Division by zero"));

    const bool negative = value < 0;
    const Wide numerator = multiplyWide(magnitude(value), multiplier);
    const uint64_t limit = negative
        ? uint64_t{1} << 63
        : static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    uint64_t result = 0;
    uint64_t remainder = 0;
    if (!divideWide(numerator, divisor, &result, &remainder) || result > limit)
        return CoordinateResult::failure(
            TimelineError::CoordinateOverflow,
            QStringLiteral("Timeline coordinate conversion overflows"));
    if (remainder >= divisor / 2 + divisor % 2) {
        if (result == limit)
            return CoordinateResult::failure(
                TimelineError::CoordinateOverflow,
                QStringLiteral("Timeline coordinate rounding overflows"));
        ++result;
    }

    if (!negative) return CoordinateResult::success(static_cast<int64_t>(result));
    if (result == (uint64_t{1} << 63))
        return CoordinateResult::success(std::numeric_limits<int64_t>::min());
    return CoordinateResult::success(-static_cast<int64_t>(result));
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

TimelineValidation validate(const SegmentTimeSpec& spec)
{
    if (spec.domain == TimelineDomain::Absolute) {
        if (!spec.tempoMap.empty() || !spec.meterMap.empty())
            return TimelineValidation::failure(
                TimelineError::InvalidTimeSpec,
                QStringLiteral("Absolute time must not carry tempo or meter maps"));
        return TimelineValidation::success();
    }
    if (spec.domain != TimelineDomain::Musical)
        return TimelineValidation::failure(
            TimelineError::InvalidTimeSpec,
            QStringLiteral("Unknown timeline domain"));

    if (spec.tempoMap.size() != 1 || spec.tempoMap.front().tick != 0)
        return TimelineValidation::failure(
            TimelineError::InvalidTimeSpec,
            QStringLiteral("Musical time requires one tempo point at tick zero"));
    if (spec.meterMap.size() != 1 || spec.meterMap.front().tick != 0)
        return TimelineValidation::failure(
            TimelineError::InvalidTimeSpec,
            QStringLiteral("Musical time requires one meter point at tick zero"));

    const int64_t tempo = spec.tempoMap.front().microsecondsPerQuarter;
    constexpr uint64_t kMaximumSafeTempo =
        std::numeric_limits<uint64_t>::max() / 1000;
    if (tempo <= 0 || static_cast<uint64_t>(tempo) > kMaximumSafeTempo)
        return TimelineValidation::failure(
            TimelineError::InvalidTimeSpec,
            QStringLiteral("Tempo must be positive and safely convertible"));

    const MeterPoint& meter = spec.meterMap.front();
    if (meter.numerator <= 0 || meter.denominator <= 0)
        return TimelineValidation::failure(
            TimelineError::InvalidTimeSpec,
            QStringLiteral("Meter numerator and denominator must be positive"));
    if ((kPpq * 4) % meter.denominator != 0)
        return TimelineValidation::failure(
            TimelineError::InvalidTimeSpec,
            QStringLiteral("Meter denominator must map to an integer tick beat"));
    return TimelineValidation::success();
}

CoordinateResult ticksToNanoseconds(int64_t ticks, const SegmentTimeSpec& spec)
{
    const TimelineValidation validity = validate(spec);
    if (!validity) return CoordinateResult::failure(validity.error, validity.message);
    if (spec.domain != TimelineDomain::Musical)
        return CoordinateResult::failure(
            TimelineError::InvalidTimeSpec,
            QStringLiteral("Tick conversion requires Musical time"));
    const uint64_t nanosecondsPerQuarter =
        static_cast<uint64_t>(spec.tempoMap.front().microsecondsPerQuarter) * 1000;
    return roundMultiplyDivide(ticks, nanosecondsPerQuarter,
                               static_cast<uint64_t>(kPpq));
}

CoordinateResult nanosecondsToTicks(int64_t nanoseconds, const SegmentTimeSpec& spec)
{
    const TimelineValidation validity = validate(spec);
    if (!validity) return CoordinateResult::failure(validity.error, validity.message);
    if (spec.domain != TimelineDomain::Musical)
        return CoordinateResult::failure(
            TimelineError::InvalidTimeSpec,
            QStringLiteral("Tick conversion requires Musical time"));
    const uint64_t nanosecondsPerQuarter =
        static_cast<uint64_t>(spec.tempoMap.front().microsecondsPerQuarter) * 1000;
    return roundMultiplyDivide(nanoseconds, static_cast<uint64_t>(kPpq),
                               nanosecondsPerQuarter);
}

CoordinateResult toNanoseconds(int64_t coordinate, const SegmentTimeSpec& spec)
{
    const TimelineValidation validity = validate(spec);
    if (!validity) return CoordinateResult::failure(validity.error, validity.message);
    if (spec.domain == TimelineDomain::Absolute)
        return CoordinateResult::success(coordinate);
    return ticksToNanoseconds(coordinate, spec);
}

CoordinateResult fromNanoseconds(int64_t nanoseconds, const SegmentTimeSpec& spec)
{
    const TimelineValidation validity = validate(spec);
    if (!validity) return CoordinateResult::failure(validity.error, validity.message);
    if (spec.domain == TimelineDomain::Absolute)
        return CoordinateResult::success(nanoseconds);
    return nanosecondsToTicks(nanoseconds, spec);
}

std::optional<double> ticksToBeats(int64_t ticks)
{
    return static_cast<double>(ticks) / static_cast<double>(kPpq);
}

CoordinateResult beatsToTicks(double beats)
{
    return roundScaledDouble(beats, kPpq, QStringLiteral("Beats"));
}

std::optional<double> toSeconds(int64_t coordinate, const SegmentTimeSpec& spec)
{
    const CoordinateResult nanoseconds = toNanoseconds(coordinate, spec);
    if (!nanoseconds) return std::nullopt;
    return static_cast<double>(nanoseconds.value)
        / static_cast<double>(kNanosecondsPerSecond);
}

CoordinateResult fromSeconds(double seconds, const SegmentTimeSpec& spec)
{
    const CoordinateResult nanoseconds = roundScaledDouble(
        seconds, kNanosecondsPerSecond, QStringLiteral("Seconds"));
    if (!nanoseconds) return nanoseconds;
    return fromNanoseconds(nanoseconds.value, spec);
}

CoordinateResult snap(int64_t coordinate, TimelineDomain domain)
{
    if (domain != TimelineDomain::Absolute && domain != TimelineDomain::Musical)
        return CoordinateResult::failure(
            TimelineError::InvalidTimeSpec,
            QStringLiteral("Unknown timeline domain"));
    return snap(coordinate,
                domain == TimelineDomain::Musical ? kPpq : kNanosecondsPerSecond);
}

CoordinateResult snap(int64_t coordinate, const SegmentTimeSpec& spec)
{
    const TimelineValidation validity = validate(spec);
    if (!validity) return CoordinateResult::failure(validity.error, validity.message);
    if (spec.domain == TimelineDomain::Absolute)
        return snap(coordinate, kNanosecondsPerSecond);
    return snap(coordinate,
                (kPpq * 4) / spec.meterMap.front().denominator);
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

CoordinateResult convert(int64_t coordinate,
                         const SegmentTimeSpec& source,
                         const SegmentTimeSpec& destination)
{
    const TimelineValidation sourceValidity = validate(source);
    if (!sourceValidity)
        return CoordinateResult::failure(sourceValidity.error, sourceValidity.message);
    const TimelineValidation destinationValidity = validate(destination);
    if (!destinationValidity)
        return CoordinateResult::failure(destinationValidity.error,
                                         destinationValidity.message);
    if (source == destination) return CoordinateResult::success(coordinate);
    const CoordinateResult nanoseconds = toNanoseconds(coordinate, source);
    if (!nanoseconds) return nanoseconds;
    return fromNanoseconds(nanoseconds.value, destination);
}

}
}
