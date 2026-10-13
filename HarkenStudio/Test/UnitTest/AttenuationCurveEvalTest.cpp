#include "Core/CurveSemantics.h"

#include <gtest/gtest.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace {

using VNX::CurveHandlePairDef;
using VNX::CurvePointDef;

constexpr float kDbTolerance = 1e-4f;

std::uint32_t floatBits(float value)
{
    static_assert(sizeof(float) == sizeof(std::uint32_t));
    std::uint32_t bits = 0;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

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

TEST(AttenuationCurveEval, VolumeAmplitudeGoldens)
{
    struct Golden {
        const char* name;
        CurveHandlePairDef handles;
        float expectedDb[3];
    };
    const Golden kGoldens[] = {
        {"endpoint line",   {0.0f, 0.0f, 100.0f, -96.0f},  {-2.4987f, -6.0205f, -12.0408f}},
        {"standard bend",   {25.0f, 0.0f, 75.0f, -96.0f},  {-1.7487f, -6.0205f, -14.7810f}},
        {"flat derivative", {100.0f, 0.0f, 0.0f, -96.0f},  {-0.2621f, -6.0204f, -30.5332f}},
    };
    const float xs[] = {25.0f, 50.0f, 75.0f};
    for (const Golden& g : kGoldens) {
        const QVector<CurveHandlePairDef> handles{g.handles};
        for (int i = 0; i < 3; ++i)
            EXPECT_NEAR(VNX::curveVolumeBezierValueAt(volumeSpanPoints(),
                                                            handles, xs[i]),
                        g.expectedDb[i], kDbTolerance)
                << g.name << " x=" << xs[i];
    }
}

TEST(AttenuationCurveEval, EarlyOutsReturnAuthoredValuesBitExact)
{
    const QVector<CurvePointDef> points{{10.0f, 5.0f}, {20.0f, -7.0f}};
    const QVector<CurveHandlePairDef> handles{{17.0f, 40.0f, 12.0f, -50.0f}};
    for (float x : {-5.0f, 0.0f, 10.0f})
        EXPECT_EQ(floatBits(VNX::curveBezierValueAt(points, handles, x)),
                  floatBits(5.0f)) << "x=" << x;
    for (float x : {20.0f, 33.0f, 1000.0f})
        EXPECT_EQ(floatBits(VNX::curveBezierValueAt(points, handles, x)),
                  floatBits(-7.0f)) << "x=" << x;
}

TEST(AttenuationCurveEval, VolumeEarlyOutsPreserveAuthoredEndpoints)
{
    const QVector<CurveHandlePairDef> handles{{0.0f, 0.0f, 100.0f, -96.0f}};
    EXPECT_EQ(floatBits(VNX::curveVolumeBezierValueAt(volumeSpanPoints(),
                                                            handles, 0.0f)),
              floatBits(0.0f));

    const float floorDb = VNX::curveVolumeBezierValueAt(volumeSpanPoints(),
                                                              handles, 100.0f);
    EXPECT_EQ(floatBits(floorDb), floatBits(-96.0f));
}

TEST(AttenuationCurveEval, InteriorKnotBelongsToRightSegment)
{
    const QVector<CurvePointDef> points{
        {0.0f, 0.0f}, {50.0f, -30.0f}, {100.0f, -90.0f}};
    const QVector<CurveHandlePairDef> handles{
        {0.0f, 0.0f, 50.0f, -30.0f},
        {100.0f, -30.0f, 50.0f, -90.0f},
    };
    EXPECT_EQ(floatBits(VNX::curveBezierValueAt(points, handles, 50.0f)),
              floatBits(-30.0f));
    EXPECT_NEAR(VNX::curveBezierValueAt(points, handles, 25.0f), -15.0f, 1e-5f);
    // x=75 is the flat-derivative midpoint (x'(t)=0): near it, x(t)-target
    // drops below one double ulp and the bisection converges with a
    // (ulp/c)^(1/3) systematic offset — inherent to the runtime kernel, so
    // the tolerance is |y'| * 3.3e-6 with 3x headroom. Do not tighten.
    EXPECT_NEAR(VNX::curveBezierValueAt(points, handles, 75.0f), -60.0f, 1e-3f);
}

TEST(AttenuationCurveEval, SinglePointCurveIsConstant)
{
    const QVector<CurvePointDef> points{{30.0f, -12.0f}};
    const QVector<CurveHandlePairDef> handles;
    for (float x : {0.0f, 30.0f, 100.0f})
        EXPECT_EQ(floatBits(VNX::curveBezierValueAt(points, handles, x)),
                  floatBits(-12.0f)) << "x=" << x;
    EXPECT_NEAR(VNX::curveVolumeBezierValueAt(points, handles, 55.0f),
                -12.0f, kDbTolerance);
}

TEST(AttenuationCurveEval, FlatDerivativeExtremeSolvesInterior)
{
    const QVector<CurvePointDef> points{{0.0f, 0.0f}, {100.0f, 1.0f}};
    const QVector<CurveHandlePairDef> handles{{100.0f, 0.0f, 0.0f, 1.0f}};
    EXPECT_NEAR(VNX::curveBezierValueAt(points, handles, 43.75f), 0.15625f, 1e-6f);
    // x=50 sits on the flat-derivative midpoint; same bisection quantization
    // band as above (|y'| * 2.1e-6 with 3x headroom). Do not tighten.
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

    // The bottom of the axis must recover the floor exactly (ny=0 feeds the
    // grid's always-drawn "-96.0" label); the top only needs dB tolerance —
    // aMin + (1 - aMin) is not guaranteed to re-sum to exactly 1.0 in double.
    EXPECT_EQ(floatBits(VNX::curveAxisValueFromNormalizedY(
                  CurveAxisScale::AmplitudeDb, -96.0f, 0.0f, 0.0)),
              floatBits(-96.0f));
    EXPECT_NEAR(VNX::curveAxisValueFromNormalizedY(
                    CurveAxisScale::AmplitudeDb, -96.0f, 0.0f, 1.0),
                0.0f, 1e-6f);

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
    EXPECT_GT(zero, 0.51);
    EXPECT_LT(zero, 0.52);
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
