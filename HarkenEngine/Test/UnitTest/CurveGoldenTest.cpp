#include <gtest/gtest.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#include "harken/param/Curve.h"
#include "harken/param/ParamTargetTable.h"

namespace {

using harken::Curve;
using harken::CurveHandlePair;
using harken::CurveLUT;
using harken::CurvePoint;

std::uint32_t FloatBits(float value) {
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

CurveHandlePair Endpoints(const CurvePoint& a, const CurvePoint& b) {
    return {a.x, a.y, b.x, b.y};
}

double Bezier(double p0, double p1, double p2, double p3, double t) {
    const double u = 1.0 - t;
    return u * u * u * p0 + 3.0 * u * u * t * p1 +
           3.0 * u * t * t * p2 + t * t * t * p3;
}

TEST(CurveGolden, EndpointHandlesProduceLinearInterpolation) {
    const CurvePoint points[] = {{-2.0f, 4.0f}, {3.0f, -6.0f}};
    const CurveHandlePair handles[] = {Endpoints(points[0], points[1])};
    Curve curve;
    ASSERT_TRUE(curve.SetPoints(points, handles, 2));
    for (int i = 0; i <= 20; ++i) {
        const float x = -2.0f + 5.0f * static_cast<float>(i) / 20.0f;
        const float expected = 4.0f + (-10.0f) * (x + 2.0f) / 5.0f;
        EXPECT_NEAR(curve.Eval(x), expected, 2.0e-6f);
    }
}

TEST(CurveGolden, VolumeAmplitudeGoldens) {
    const CurvePoint authored[] = {{0.0f, 0.0f}, {100.0f, -96.0f}};
    const auto toAmplitude = [](float db) {
        return static_cast<float>(std::pow(10.0, static_cast<double>(db) /
                                                   20.0));
    };
    const CurvePoint points[] = {{0.0f, toAmplitude(authored[0].y)},
                                 {100.0f, toAmplitude(authored[1].y)}};
    const CurveHandlePair handles[] = {
        {0.0f, points[0].y, 100.0f, points[1].y}};
    Curve curve;
    ASSERT_TRUE(curve.SetPoints(points, handles, 2));
    const float expectedDb[] = {-2.4987f, -6.0205f, -12.0408f};
    const float xs[] = {25.0f, 50.0f, 75.0f};
    for (int i = 0; i < 3; ++i) {
        const float actualDb =
            20.0f * std::log10(std::max(curve.Eval(xs[i]), 1.0e-30f));
        EXPECT_NEAR(actualDb, expectedDb[i], 1.0e-4f);
    }
}

TEST(CurveGolden, BindingDecibelsInterpolateLikeAttenuationWithoutChangingStorage) {
    const CurvePoint points[] = {{0.0f, 0.0f}, {100.0f, -96.0f}};
    const CurveHandlePair handles[] = {Endpoints(points[0], points[1])};
    Curve curve;
    ASSERT_TRUE(curve.SetPoints(points, handles, 2));
    const float xs[] = {25.0f, 50.0f, 75.0f};
    const float expected[] = {-2.4987f, -6.0205f, -12.0408f};
    for (int i = 0; i < 3; ++i) {
        EXPECT_NEAR(curve.Eval(xs[i], harken::BindingCurveDomain(
            harken::kTargetScopeVoice, harken::kTargetVoiceVolume)), expected[i], 1.0e-4f);
        EXPECT_NEAR(curve.Eval(xs[i], harken::BindingCurveDomain(
            harken::kTargetScopeBus, harken::kBusTargetBusVolume)), expected[i], 1.0e-4f);
        EXPECT_NEAR(curve.Eval(xs[i], harken::BindingCurveDomain(
            harken::kTargetScopeBus, harken::kBusTargetBusOutputVolume)), expected[i], 1.0e-4f);
    }
    EXPECT_FLOAT_EQ(curve.Eval(50.0f, harken::BindingCurveDomain(
        harken::kTargetScopeVoice, harken::kTargetVoicePitch)), -48.0f);
    EXPECT_FLOAT_EQ(curve.points[0].y, 0.0f);
    EXPECT_FLOAT_EQ(curve.points[1].y, -96.0f);
    EXPECT_FLOAT_EQ(curve.handles[0].h2y, -96.0f);
}

TEST(CurveGolden, AmplitudeInterpolationSupportsPositiveGainAndBezierHandles) {
    const CurvePoint points[] = {{0.0f, 24.0f}, {1.0f, -96.0f}};
    const CurveHandlePair handles[] = {{0.2f, 12.0f, 0.7f, -6.0f}};
    Curve curve;
    ASSERT_TRUE(curve.SetPoints(points, handles, 2));
    Curve amplitudeCurve;
    CurvePoint amplitudes[2] = {points[0], points[1]};
    CurveHandlePair amplitudeHandles[] = {handles[0]};
    const auto amplitude = [](float db) {
        return static_cast<float>(std::pow(10.0, static_cast<double>(db) / 20.0));
    };
    for (auto& point : amplitudes) point.y = amplitude(point.y);
    amplitudeHandles[0].h1y = amplitude(handles[0].h1y);
    amplitudeHandles[0].h2y = amplitude(handles[0].h2y);
    ASSERT_TRUE(amplitudeCurve.SetPoints(amplitudes, amplitudeHandles, 2));
    for (int i = 0; i <= 100; ++i) {
        const float x = i / 100.0f;
        EXPECT_NEAR(curve.Eval(x, harken::CurveDomain::AmplitudeDb),
            20.0 * std::log10(amplitudeCurve.Eval(x)), 1.0e-4f);
    }
}

TEST(CurveGolden, BindingDecibelsKeepStraightGeometryAcrossZero) {
    const auto coordinate = [](float db) {
        const double amplitude = std::pow(10.0, -std::abs(static_cast<double>(db)) / 20.0);
        return db <= 0.0f ? amplitude : 2.0 - amplitude;
    };
    for (float start : {-6.0f, 0.0f, 6.0f, 24.0f}) {
        const CurvePoint points[] = {{0.0f, start}, {1.0f, -24.0f}};
        const CurveHandlePair handles[] = {Endpoints(points[0], points[1])};
        Curve curve;
        ASSERT_TRUE(curve.SetPoints(points, handles, 2));
        const harken::CurveDomain domains[] = {
            harken::BindingCurveDomain(harken::kTargetScopeVoice, harken::kTargetVoiceVolume),
            harken::BindingCurveDomain(harken::kTargetScopeBus, harken::kBusTargetBusVolume),
            harken::BindingCurveDomain(harken::kTargetScopeBus, harken::kBusTargetBusOutputVolume)};
        for (auto domain : domains) {
            for (int i = 0; i <= 20; ++i) {
                const float x = i / 20.0f;
                const double expected = coordinate(start) * (1.0 - x) + coordinate(-24.0f) * x;
                EXPECT_NEAR(coordinate(curve.Eval(x, domain)), expected, 1e-6);
            }
        }
        if (start == 24.0f)
            EXPECT_NEAR(curve.Eval(0.5f, domains[0]), 0.0f, 1e-6f);
    }
}

TEST(CurveGolden, MirroredBezierHandlesUseTheSameCoordinatesAsTheirAnchors) {
    const CurvePoint points[] = {{0.0f, 24.0f}, {1.0f, -96.0f}};
    const CurveHandlePair handles[] = {{0.2f, 12.0f, 0.7f, -6.0f}};
    Curve curve;
    ASSERT_TRUE(curve.SetPoints(points, handles, 2));
    const double ys[] = {2.0 - std::pow(10.0, -24.0 / 20.0),
                          2.0 - std::pow(10.0, -12.0 / 20.0),
                          std::pow(10.0, -6.0 / 20.0), std::pow(10.0, -96.0 / 20.0)};
    for (int i = 1; i < 20; ++i) {
        const double t = i / 20.0;
        const float x = static_cast<float>(Bezier(0.0, handles[0].h1x, handles[0].h2x, 1.0, t));
        const double y = Bezier(ys[0], ys[1], ys[2], ys[3], t);
        const double expected = y <= 1.0 ? 20.0 * std::log10(y) : -20.0 * std::log10(2.0 - y);
        EXPECT_NEAR(curve.Eval(x, harken::CurveDomain::MirroredAmplitudeDb), expected, 1e-4);
    }
}

TEST(CurveGolden, MirroredDecibelsPreserveFiniteLargeBindingValues) {
    for (float direction : {-1.0f, 1.0f}) {
        const CurvePoint points[] = {{0.0f, 400.0f * direction}, {1.0f, 300.0f * direction}};
        const CurveHandlePair handles[] = {Endpoints(points[0], points[1])};
        Curve curve;
        ASSERT_TRUE(curve.SetPoints(points, handles, 2));
        EXPECT_NEAR(curve.Eval(0.5f, harken::CurveDomain::MirroredAmplitudeDb),
                    direction * 306.0205f, 1e-4f);
    }
}

TEST(CurveGolden, MirroredCrossZeroPreservesFiniteValuesNearBothBoundaries) {
    const harken::CurveDomain domains[] = {
        harken::BindingCurveDomain(harken::kTargetScopeVoice, harken::kTargetVoiceVolume),
        harken::BindingCurveDomain(harken::kTargetScopeBus, harken::kBusTargetBusVolume),
        harken::BindingCurveDomain(harken::kTargetScopeBus, harken::kBusTargetBusOutputVolume)};
    for (float magnitude : {400.0f, 10000.0f, std::numeric_limits<float>::max()}) {
        for (float direction : {-1.0f, 1.0f}) {
            const CurvePoint points[] = {{0, direction * magnitude}, {1, -direction * magnitude}};
            const CurveHandlePair handles[] = {{0.25f, points[0].y, 0.75f, points[0].y}};
            Curve curve;
            ASSERT_TRUE(curve.SetPoints(points, handles, 2));
            for (auto domain : domains) {
                EXPECT_FLOAT_EQ(curve.Eval(0, domain), points[0].y);
                EXPECT_FLOAT_EQ(curve.Eval(1, domain), points[1].y);
                for (float x : {std::nextafter(0.0f, 1.0f), 1e-8f, 0.25f, 0.5f,
                                0.75f, std::nextafter(1.0f, 0.0f)}) {
                    const float value = curve.Eval(x, domain);
                    EXPECT_TRUE(std::isfinite(value)) << magnitude << ", " << direction << ", " << x;
                    EXPECT_LE(std::abs(value), magnitude);
                }
                EXPECT_NEAR(curve.Eval(0.5f, domain), direction * 12.0411998f, 1e-4f);
                if (magnitude == 400.0f)
                    EXPECT_NEAR(curve.Eval(std::nextafter(0.0f, 1.0f), domain), points[0].y, 1e-4f);
            }
        }
    }
}

TEST(CurveGolden, VolumeAmplitudeCurvedGoldens) {
    const auto amplitude = [](float db) {
        return static_cast<float>(std::pow(10.0, static_cast<double>(db) /
                                                   20.0));
    };
    const CurvePoint points[] = {{0.0f, amplitude(0.0f)},
                                 {100.0f, amplitude(-96.0f)}};
    const CurveHandlePair handles[] = {
        {25.0f, amplitude(0.0f), 75.0f, amplitude(-96.0f)}};
    Curve curve;
    ASSERT_TRUE(curve.SetPoints(points, handles, 2));
    const float expectedDb[] = {-1.7487f, -6.0205f, -14.7810f};
    const float xs[] = {25.0f, 50.0f, 75.0f};
    for (int i = 0; i < 3; ++i) {
        const float actualDb =
            20.0f * std::log10(std::max(curve.Eval(xs[i]), 1.0e-30f));
        EXPECT_NEAR(actualDb, expectedDb[i], 1.0e-4f);
    }
}

TEST(CurveGolden, VolumeAmplitudeFlatDerivativeGolden) {
    const auto amplitude = [](float db) {
        return static_cast<float>(std::pow(10.0, static_cast<double>(db) /
                                                   20.0));
    };
    const CurvePoint points[] = {{0.0f, amplitude(0.0f)},
                                 {100.0f, amplitude(-96.0f)}};
    const CurveHandlePair handles[] = {
        {100.0f, points[0].y, 0.0f, points[1].y}};
    Curve curve;
    ASSERT_TRUE(curve.SetPoints(points, handles, 2));
    const float expectedDb[] = {-0.2621f, -6.0204f, -30.5332f};
    const float xs[] = {25.0f, 50.0f, 75.0f};
    for (int i = 0; i < 3; ++i) {
        const float actualDb =
            20.0f * std::log10(std::max(curve.Eval(xs[i]), 1.0e-30f));
        EXPECT_NEAR(actualDb, expectedDb[i], 1.0e-4f);
    }
}

TEST(CurveGolden, RejectsInvalidHandlesWithoutMutatingState) {
    const CurvePoint goodPoints[] = {{0.0f, 1.0f}, {1.0f, 2.0f}};
    const CurveHandlePair goodHandles[] = {Endpoints(goodPoints[0],
                                                     goodPoints[1])};
    Curve curve;
    ASSERT_TRUE(curve.SetPoints(goodPoints, goodHandles, 2));
    const float old = curve.Eval(0.5f);

    const CurveHandlePair outside[] = {{-0.1f, 1.0f, 1.0f, 2.0f}};
    EXPECT_FALSE(curve.SetPoints(goodPoints, outside, 2));
    EXPECT_FLOAT_EQ(curve.Eval(0.5f), old);
    const CurveHandlePair outsideRight[] = {{0.0f, 1.0f, 1.1f, 2.0f}};
    EXPECT_FALSE(curve.SetPoints(goodPoints, outsideRight, 2));
    EXPECT_FLOAT_EQ(curve.Eval(0.5f), old);

    const float nan = std::numeric_limits<float>::quiet_NaN();
    const CurveHandlePair nonFinite[] = {{0.0f, nan, 1.0f, 2.0f}};
    EXPECT_FALSE(curve.SetPoints(goodPoints, nonFinite, 2));
    EXPECT_FLOAT_EQ(curve.Eval(0.5f), old);
}

TEST(CurveGolden, InverseConvergesToTheSpecifiedTolerance) {
    const CurvePoint points[] = {{0.0f, -2.0f}, {3.0f, 7.0f}};
    const CurveHandlePair handles[] = {{2.9f, 20.0f, 0.1f, -15.0f}};
    Curve curve;
    ASSERT_TRUE(curve.SetPoints(points, handles, 2));
    constexpr double expectedToleranceScale = 1.0e-6;
    for (int i = 1; i < 20; ++i) {
        const double t = static_cast<double>(i) / 20.0;
        const double x = Bezier(0.0, 2.9, 0.1, 3.0, t);
        const double expected = Bezier(-2.0, 20.0, -15.0, 7.0, t);
        const double actual = curve.Eval(static_cast<float>(x));
        const double tolerance =
            std::max(expectedToleranceScale * std::max(1.0, 9.0),
                     expectedToleranceScale * std::abs(expected));
        EXPECT_LE(std::abs(actual - expected), tolerance);
    }
}

TEST(CurveGolden, LutGridPointsMatchAnalyticEvaluationExactly) {
    const CurvePoint points[] = {{0.0f, 0.0f}, {1.0f, 1.0f}};
    const CurveHandlePair handles[] = {{1.0f, 0.0f, 0.0f, 1.0f}};
    Curve curve;
    ASSERT_TRUE(curve.SetPoints(points, handles, 2));
    CurveLUT lut;
    lut.Bake(curve);

    const double span = static_cast<double>(lut.xMax) -
                        static_cast<double>(lut.xMin);
    for (std::uint32_t i = 0; i <= harken::kCurveLutEntries; ++i) {
        const double u = static_cast<double>(i) /
                         static_cast<double>(harken::kCurveLutEntries);
        const float x =
            static_cast<float>(static_cast<double>(lut.xMin) + span * u);
        EXPECT_EQ(FloatBits(lut.Eval(x)), FloatBits(curve.Eval(x)));
    }
}

TEST(CurveGolden, LutChordErrorIsTightForBoundedSlopeCurve) {
    const CurvePoint points[] = {{0.0f, 0.0f}, {1.0f, 1.0f}};
    const CurveHandlePair handles[] = {{0.25f, 0.0f, 0.75f, 1.0f}};
    Curve curve;
    ASSERT_TRUE(curve.SetPoints(points, handles, 2));
    CurveLUT lut;
    lut.Bake(curve);

    // x(t) = 3t/4 + 3t^2/4 - t^3/2 has x'(t) >= 3/4, while
    // y(t) = 3t^2 - 2t^3. Therefore
    // |d^2y/dx^2| = |9/2 - 9t| / x'(t)^3 <= 32/3.
    // Linear interpolation on cells of width h = 1/128 is bounded by
    // max|d^2y/dx^2| * h^2 / 8 = 1/12288, about 8.14e-5.
    // The threshold below leaves slightly less than three times that bound.
    float maxChordError = 0.0f;
    for (std::uint32_t i = 0; i < harken::kCurveLutEntries; ++i) {
        const float x0 = static_cast<float>(i) /
                         static_cast<float>(harken::kCurveLutEntries);
        const float x1 = static_cast<float>(i + 1) /
                         static_cast<float>(harken::kCurveLutEntries);
        const float xm = (x0 + x1) * 0.5f;
        maxChordError =
            std::max(maxChordError, std::abs(lut.Eval(xm) - curve.Eval(xm)));
    }
    EXPECT_LT(maxChordError, 2.4e-4f);
}

TEST(CurveGolden, LutExtremeVerticalTangentHasKnownChordErrorBand) {
    const CurvePoint points[] = {{0.0f, 0.0f}, {1.0f, 1.0f}};
    const CurveHandlePair handles[] = {{1.0f, 0.0f, 0.0f, 1.0f}};
    Curve curve;
    ASSERT_TRUE(curve.SetPoints(points, handles, 2));
    CurveLUT lut;
    lut.Bake(curve);

    // With t = 1/2 + delta, x - 1/2 = 4 delta^3 exactly and
    // y - 1/2 = 3 delta/2 - 2 delta^3. The singular leading term is
    // 1.5 * cbrt((x - 0.5) / 4), so the midpoint chord error scales as
    // 0.2775 * h^(1/3), about 0.0551 for h = 1/128. The two-sided band
    // characterizes the known limit of a uniform-x table.
    float maxChordError = 0.0f;
    for (std::uint32_t i = 0; i < harken::kCurveLutEntries; ++i) {
        const float x0 = static_cast<float>(i) /
                         static_cast<float>(harken::kCurveLutEntries);
        const float x1 = static_cast<float>(i + 1) /
                         static_cast<float>(harken::kCurveLutEntries);
        const float xm = (x0 + x1) * 0.5f;
        maxChordError =
            std::max(maxChordError, std::abs(lut.Eval(xm) - curve.Eval(xm)));
    }
    EXPECT_GT(maxChordError, 0.04f);
    EXPECT_LT(maxChordError, 0.07f);
}

} // namespace
