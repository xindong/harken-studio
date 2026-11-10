#include "Core/CurveSemantics.h"

#include <gtest/gtest.h>

#include <cmath>

namespace {

using VNX::CurveHandlePairDef;
using VNX::CurvePointDef;

constexpr float kDbTolerance = 1e-4f;

double bezier(double p0, double p1, double p2, double p3, double t)
{
    const double u = 1.0 - t;
    return u * u * u * p0 + 3.0 * u * u * t * p1 +
           3.0 * u * t * t * p2 + t * t * t * p3;
}

QVector<CurvePointDef> volumeSpanPoints()
{
    return {{0.0f, 0.0f}, {100.0f, -96.0f}};
}

} // namespace

TEST(AttenuationCurveEval, FlatDerivativeExtremeSolvesInterior)
{
    const QVector<CurvePointDef> points{{0.0f, 0.0f}, {100.0f, 1.0f}};
    const QVector<CurveHandlePairDef> handles{{100.0f, 0.0f, 0.0f, 1.0f}};
    EXPECT_NEAR(VNX::curveBezierValueAt(points, handles, 43.75f), 0.15625f, 1e-6f);
    EXPECT_NEAR(VNX::curveBezierValueAt(points, handles, 50.0f), 0.5f, 1e-5f);
    EXPECT_NEAR(VNX::curveBezierValueAt(points, handles, 56.25f), 0.84375f, 1e-6f);
}

TEST(AttenuationCurveEval, EndpointHandlesProduceLinearInterpolation)
{
    const QVector<CurvePointDef> points{{-2.0f, 4.0f}, {3.0f, -6.0f}};
    const QVector<CurveHandlePairDef> handles{{-2.0f, 4.0f, 3.0f, -6.0f}};
    for (int i = 0; i <= 20; ++i) {
        const float x = -2.0f + 5.0f * static_cast<float>(i) / 20.0f;
        const float expected = 4.0f + (-10.0f) * (x + 2.0f) / 5.0f;
        EXPECT_NEAR(VNX::curveBezierValueAt(points, handles, x), expected,
                    2.0e-6f) << "x=" << x;
    }
}

TEST(CurvePresets, PreserveBoundsMonotonicityAndTimeReflectionAcrossAxes)
{
    constexpr float low = -72.0f;
    constexpr float high = 12.0f;
    const QVector<CurvePointDef> rising{{7.0f, low}, {43.0f, high}};
    const QVector<CurvePointDef> falling{{7.0f, high}, {43.0f, low}};
    const QVector<CurvePointDef> flat{{7.0f, -24.0f}, {43.0f, -24.0f}};
    const QVector<CurvePointDef> unit{{0.0f, 0.0f}, {1.0f, 1.0f}};
    for (const auto scale : {VNX::CurveAxisScale::LinearValue,
                            VNX::CurveAxisScale::AmplitudeDb,
                            VNX::CurveAxisScale::MirroredAmplitudeDb}) {
        for (const auto preset : VNX::kCurvePresets) {
            SCOPED_TRACE(::testing::Message() << "scale=" << static_cast<int>(scale)
                         << " preset=" << static_cast<int>(preset));
            const QVector<CurveHandlePairDef> up{
                VNX::curvePresetHandles(preset, rising[0], rising[1], scale)};
            const QVector<CurveHandlePairDef> down{
                VNX::curvePresetHandles(preset, falling[0], falling[1], scale)};
            const QVector<CurveHandlePairDef> horizontal{
                VNX::curvePresetHandles(preset, flat[0], flat[1], scale)};
            const QVector<CurveHandlePairDef> normalized{
                VNX::curvePresetHandles(preset, unit[0], unit[1])};
            double previous = 0.0;
            for (int sample = 0; sample <= 128; ++sample) {
                const float t = sample / 128.0f;
                const float x = 7.0f + 36.0f * t;
                const float value = VNX::curveBezierValueAt(rising, up, x, scale);
                const double y = VNX::curveAxisNormalizedY(scale, low, high, value);
                EXPECT_GE(y, -1e-6);
                EXPECT_LE(y, 1.0 + 1e-6);
                EXPECT_GE(y + 1e-6, previous);
                EXPECT_NEAR(y, VNX::curveBezierValueAt(unit, normalized, t), 2e-5);
                const float reflected = VNX::curveBezierValueAt(falling, down, 50.0f - x, scale);
                EXPECT_NEAR(y, VNX::curveAxisNormalizedY(scale, low, high, reflected), 2e-5);
                EXPECT_NEAR(VNX::curveBezierValueAt(flat, horizontal, x, scale), -24.0f, 1e-5);
                previous = y;
            }
        }
    }
}

TEST(CurvePresets, CurvatureStaysOnTheSameSideOfTheChordInBothDirections)
{
    for (const bool descending : {false, true}) {
        const QVector<CurvePointDef> points{{0.0f, descending ? 1.0f : 0.0f},
                                            {1.0f, descending ? 0.0f : 1.0f}};
        for (const float x : {0.125f, 0.25f, 0.5f, 0.75f, 0.875f}) {
            const auto value = [&](VNX::CurvePreset preset) {
                return VNX::curveBezierValueAt(points,
                    {VNX::curvePresetHandles(preset, points[0], points[1])}, x);
            };
            const float chord = descending ? 1.0f - x : x;
            EXPECT_GT(value(VNX::CurvePreset::Log3), value(VNX::CurvePreset::Convex));
            EXPECT_GT(value(VNX::CurvePreset::Convex), chord);
            EXPECT_NEAR(value(VNX::CurvePreset::Linear), chord, 1e-6);
            EXPECT_LT(value(VNX::CurvePreset::Concave), chord);
            EXPECT_LT(value(VNX::CurvePreset::Exp3), value(VNX::CurvePreset::Concave));
        }
    }
}

TEST(CurvePresets, MirroredSCurveIsTheInverseOfSCurve)
{
    for (const bool descending : {false, true}) {
        const QVector<CurvePointDef> points{{0.0f, descending ? 1.0f : 0.0f},
                                            {1.0f, descending ? 0.0f : 1.0f}};
        const QVector<CurveHandlePairDef> forward{
            VNX::curvePresetHandles(VNX::CurvePreset::SCurve, points[0], points[1])};
        const QVector<CurveHandlePairDef> inverse{
            VNX::curvePresetHandles(VNX::CurvePreset::InverseSCurve, points[0], points[1])};
        for (int sample = 0; sample <= 128; ++sample) {
            const float x = sample / 128.0f;
            const float y = VNX::curveBezierValueAt(points, forward, x);
            const float inverseY = VNX::curveBezierValueAt(points, inverse, x);
            EXPECT_NEAR(VNX::curveBezierValueAt(points, inverse, y), x, 1e-5);
            EXPECT_NEAR(VNX::curveBezierValueAt(points, forward, inverseY), x, 1e-5);
        }
    }
}

TEST(CurveAxisMapping, RoundTripStaysWithinDbTolerance)
{
    using VNX::CurveAxisScale;
    for (float v = -96.0f; v <= 0.0f; v += 0.25f) {
        const double ny = VNX::curveAxisNormalizedY(CurveAxisScale::AmplitudeDb,
                                                    -96.0f, 0.0f, v);
        EXPECT_NEAR(VNX::curveAxisValueFromNormalizedY(CurveAxisScale::AmplitudeDb,
                                                       -96.0f, 0.0f, ny),
                    v, kDbTolerance) << "v=" << v;
    }

    for (float v : {-1200.0f, -300.0f, 0.0f, 450.0f, 1200.0f}) {
        const double ny = VNX::curveAxisNormalizedY(CurveAxisScale::LinearValue,
                                                    -1200.0f, 1200.0f, v);
        EXPECT_NEAR(VNX::curveAxisValueFromNormalizedY(CurveAxisScale::LinearValue,
                                                       -1200.0f, 1200.0f, ny),
                    v, 1e-3f) << "v=" << v;
    }
}

// Bezier affine invariance on the amplitude axis: an endpoint-glued segment
// is the exact straight line on the (x, amplitude) plane, so its normalized
// screen ordinate must be collinear with x — the invariant that keeps the
// default volume curve a screen diagonal under the amplitude axis.
TEST(CurveAxisMapping, EndpointHandleSegmentIsCollinearOnAmplitudeAxis)
{
    using VNX::CurveAxisScale;
    const QVector<CurveHandlePairDef> handles{{0.0f, 0.0f, 100.0f, -96.0f}};
    for (int i = 0; i <= 100; i += 5) {
        const float x = static_cast<float>(i);
        const float db = VNX::curveVolumeBezierValueAt(volumeSpanPoints(),
                                                             handles, x);
        const double ny = VNX::curveAxisNormalizedY(CurveAxisScale::AmplitudeDb,
                                                    -96.0f, 0.0f, db);
        EXPECT_NEAR(ny, 1.0 - static_cast<double>(x) / 100.0, 1e-5) << "x=" << x;
    }
}

TEST(CurveAxisMapping, MirroredAmplitudePreservesNegativeScaleAndGainSymmetry)
{
    constexpr auto scale = VNX::CurveAxisScale::MirroredAmplitudeDb;
    const double zero = VNX::curveAxisNormalizedY(scale, -96.0f, 24.0f, 0.0f);
    for (float value : {1.0f, 3.0f, 6.0f, 12.0f, 18.0f, 24.0f}) {
        const double negative = VNX::curveAxisNormalizedY(scale, -96.0f, 24.0f, -value);
        const double positive = VNX::curveAxisNormalizedY(scale, -96.0f, 24.0f, value);
        EXPECT_NEAR(zero - negative, positive - zero, 1e-12);
        const double attenuation = VNX::curveAxisNormalizedY(
            VNX::CurveAxisScale::AmplitudeDb, -96.0f, 0.0f, -value);
        EXPECT_NEAR(negative / zero, attenuation, 1e-12);
    }
}

TEST(CurveAxisMapping, MirroredAmplitudeRoundTripsTheEntireVolumeRange)
{
    constexpr auto scale = VNX::CurveAxisScale::MirroredAmplitudeDb;
    double previous = -1.0;
    for (float value = -96.0f; value <= 24.0f; value += 0.125f) {
        const double normalized = VNX::curveAxisNormalizedY(scale, -96.0f, 24.0f, value);
        EXPECT_GT(normalized, previous);
        EXPECT_GE(normalized, 0.0);
        EXPECT_LE(normalized, 1.0);
        EXPECT_NEAR(VNX::curveAxisValueFromNormalizedY(scale, -96.0f, 24.0f, normalized),
                    value, kDbTolerance);
        previous = normalized;
    }
    EXPECT_FLOAT_EQ(VNX::curveAxisValueFromNormalizedY(scale, -96.0f, 24.0f, 0.0), -96.0f);
    EXPECT_FLOAT_EQ(VNX::curveAxisValueFromNormalizedY(scale, -96.0f, 24.0f, 1.0), 24.0f);
}

TEST(AttenuationCurveEval, PitchLinearDomainMatchesParametricOracle)
{
    const QVector<CurvePointDef> points{{0.0f, -1200.0f}, {100.0f, 1200.0f}};
    const QVector<CurveHandlePairDef> handles{{25.0f, -1200.0f, 75.0f, 1200.0f}};
    for (int i = 1; i < 20; ++i) {
        const double t = static_cast<double>(i) / 20.0;
        const double x = bezier(0.0, 25.0, 75.0, 100.0, t);
        const double expected = bezier(-1200.0, -1200.0, 1200.0, 1200.0, t);
        EXPECT_NEAR(VNX::curveBezierValueAt(points, handles,
                                                  static_cast<float>(x)),
                    expected, 5.0e-3) << "t=" << t;
    }
}

TEST(CurveDomainClipping, RetainsTheFunctionAcrossAxisScalesAndSegmentBoundaries)
{
    const QVector<CurvePointDef> points{{0.0f, -48.0f}, {45.0f, -6.0f}, {100.0f, -24.0f}};
    const QVector<CurveHandlePairDef> handles{
        {45.0f, -80.0f, 0.0f, 18.0f}, {60.0f, 12.0f, 90.0f, -72.0f}};
    for (const auto scale : {VNX::CurveAxisScale::LinearValue,
                            VNX::CurveAxisScale::AmplitudeDb,
                            VNX::CurveAxisScale::MirroredAmplitudeDb}) {
        for (const auto bounds : {std::pair{5.0f, 28.0f}, std::pair{12.0f, 83.0f},
                                  std::pair{45.0f, 100.0f}, std::pair{0.0f, 100.0f}}) {
            auto clippedPoints = points;
            auto clippedHandles = handles;
            VNX::curveClipX(bounds.first, bounds.second, clippedPoints, clippedHandles, scale);
            ASSERT_GE(clippedPoints.size(), 2);
            EXPECT_LE(clippedPoints.size(), VNX::kCurveMaxPoints);
            ASSERT_EQ(clippedHandles.size(), clippedPoints.size() - 1);
            EXPECT_FLOAT_EQ(clippedPoints.front().x, bounds.first);
            EXPECT_FLOAT_EQ(clippedPoints.back().x, bounds.second);
            auto normalizedPoints = clippedPoints;
            auto normalizedHandles = clippedHandles;
            VNX::curveNormalize(-96.0f, 24.0f, normalizedPoints, normalizedHandles);
            EXPECT_EQ(normalizedPoints, clippedPoints);
            EXPECT_EQ(normalizedHandles, clippedHandles);
            for (int sample = 0; sample <= 100; ++sample) {
                const float x = bounds.first + (bounds.second - bounds.first) * sample / 100.0f;
                ASSERT_NEAR(VNX::curveBezierValueAt(clippedPoints, clippedHandles, x, scale),
                            VNX::curveBezierValueAt(points, handles, x, scale), 0.001f)
                    << "scale=" << static_cast<int>(scale) << " x=" << x;
            }
            auto repeatedPoints = clippedPoints;
            auto repeatedHandles = clippedHandles;
            VNX::curveClipX(bounds.first, bounds.second, repeatedPoints, repeatedHandles, scale);
            EXPECT_EQ(repeatedPoints, clippedPoints);
            EXPECT_EQ(repeatedHandles, clippedHandles);
        }
    }
    auto emptyPoints = points;
    auto emptyHandles = handles;
    VNX::curveClipX(110.0f, 150.0f, emptyPoints, emptyHandles);
    EXPECT_TRUE(emptyPoints.isEmpty());
    EXPECT_TRUE(emptyHandles.isEmpty());
}
