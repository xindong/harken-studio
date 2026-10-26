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
