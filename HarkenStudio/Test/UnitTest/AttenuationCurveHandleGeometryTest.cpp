#include "Widgets/CurveEditor/CurveCanvas.h"

#include <gtest/gtest.h>

namespace {

constexpr double kTolerance = 1e-9;

const QRectF kBox(0.0, 0.0, 100.0, 100.0);

double cross(const QPointF& a, const QPointF& b)
{
    return a.x() * b.y() - a.y() * b.x();
}

QPointF bezierPoint(const QPointF& p0, const QPointF& p1, const QPointF& p2,
                    const QPointF& p3, double t)
{
    const double u = 1.0 - t;
    const double b0 = u * u * u;
    const double b1 = 3.0 * u * u * t;
    const double b2 = 3.0 * u * t * t;
    const double b3 = t * t * t;
    return QPointF(b0 * p0.x() + b1 * p1.x() + b2 * p2.x() + b3 * p3.x(),
                   b0 * p0.y() + b1 * p1.y() + b2 * p2.y() + b3 * p3.y());
}

constexpr double kBodyDeltaTs[] = {0.05, 0.3, 0.5, 0.7, 0.95};

} // namespace

TEST(AttenuationCurveHandleGeometry, RayReachesRequestedLengthInsideBox)
{
    const QPointF anchor(50.0, 50.0);
    const QPointF result = VNX::curveHandleRayIntoRect(anchor, QPointF(3.0, 4.0), 10.0, kBox);
    EXPECT_NEAR(result.x(), 56.0, kTolerance);
    EXPECT_NEAR(result.y(), 58.0, kTolerance);
}

TEST(AttenuationCurveHandleGeometry, RayStopsOnBoundaryKeepingDirection)
{
    const QPointF anchor(50.0, 50.0);
    const QPointF direction(2.0, 1.0);
    const QPointF result = VNX::curveHandleRayIntoRect(anchor, direction, 1000.0, kBox);
    EXPECT_NEAR(result.x(), 100.0, kTolerance);
    EXPECT_NEAR(result.y(), 75.0, kTolerance);
    EXPECT_NEAR(cross(result - anchor, direction), 0.0, kTolerance);
    EXPECT_TRUE(kBox.contains(result));
}

TEST(AttenuationCurveHandleGeometry, MirrorAlignedForOppositeHandles)
{
    const QPointF anchor(10.0, -4.0);
    EXPECT_TRUE(VNX::curveHandlesMirrorAligned(anchor,
                                               anchor + QPointF(12.0, 0.0),
                                               anchor + QPointF(-30.0, 0.0)));
}

TEST(AttenuationCurveHandleGeometry, NotAlignedAtRightAngle)
{
    const QPointF anchor(0.0, 0.0);
    EXPECT_FALSE(VNX::curveHandlesMirrorAligned(anchor, QPointF(10.0, 0.0),
                                                QPointF(0.0, 10.0)));
}

TEST(AttenuationCurveHandleGeometry, BodyDeltaMovesCurvePointExactly)
{
    const QPointF p0(12.0, 240.0);
    const QPointF p1(80.0, 40.0);
    const QPointF p2(210.0, 310.0);
    const QPointF p3(340.0, 120.0);
    const QPointF delta(7.5, -12.25);
    for (double t : kBodyDeltaTs) {
        QPointF d1;
        QPointF d2;
        VNX::curveDistributeBodyDelta(t, delta, &d1, &d2);
        const QPointF before = bezierPoint(p0, p1, p2, p3, t);
        const QPointF after = bezierPoint(p0, p1 + d1, p2 + d2, p3, t);
        EXPECT_NEAR(after.x() - before.x(), delta.x(), kTolerance) << "t=" << t;
        EXPECT_NEAR(after.y() - before.y(), delta.y(), kTolerance) << "t=" << t;
    }
}
