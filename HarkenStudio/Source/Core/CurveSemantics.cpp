#include "CurveSemantics.h"

#include <harken_studio_build/Curve.h>

#include <QPointF>

#include <algorithm>
#include <array>

namespace VNX {

namespace {

harkenstudiobuild::CurveDomain runtimeDomain(CurveAxisScale scale)
{
    return scale == CurveAxisScale::LinearValue ? harkenstudiobuild::CurveDomain::LinearValue
        : scale == CurveAxisScale::AmplitudeDb ? harkenstudiobuild::CurveDomain::AmplitudeDb
        : harkenstudiobuild::CurveDomain::MirroredAmplitudeDb;
}

}

double curveAxisCoordinate(CurveAxisScale scale, float value)
{
    return harkenstudiobuild::CurveValueToCoordinate(value, runtimeDomain(scale));
}

float curveAxisValueFromCoordinate(CurveAxisScale scale, double coordinate)
{
    return harkenstudiobuild::CurveValueFromCoordinate(coordinate, runtimeDomain(scale));
}

void curveNormalize(float yMin, float yMax,
                    QVector<CurvePointDef>& points,
                    QVector<CurveHandlePairDef>& handles)
{
    for (auto& p : points)
        p.value = qBound(yMin, p.value, yMax);
    std::sort(points.begin(), points.end(),
        [](const CurvePointDef& a, const CurvePointDef& b) {
            return a.x < b.x;
        });
    const qsizetype segments = points.size() > 1 ? points.size() - 1 : 0;
    if (handles.size() != segments) {
        handles.resize(segments);
        for (qsizetype i = 0; i < segments; ++i)
            handles[i] = {points[i].x, points[i].value,
                          points[i + 1].x, points[i + 1].value};
        return;
    }
    for (qsizetype i = 0; i < segments; ++i) {
        CurveHandlePairDef& h = handles[i];
        h.h1x = qBound(points[i].x, h.h1x, points[i + 1].x);
        h.h2x = qBound(points[i].x, h.h2x, points[i + 1].x);
        h.h1y = qBound(yMin, h.h1y, yMax);
        h.h2y = qBound(yMin, h.h2y, yMax);
    }
}

float curveBezierValueAt(const QVector<CurvePointDef>& points,
                         const QVector<CurveHandlePairDef>& handles,
                         float x, CurveAxisScale scale)
{
    if (points.isEmpty())
        return 0.0f;
    if (points.size() == 1 || x <= points.first().x)
        return points.first().value;
    if (x >= points.last().x)
        return points.last().value;

    int seg = 0;
    while (x >= points.at(seg + 1).x)
        ++seg;
    const CurvePointDef& a = points.at(seg);
    const CurvePointDef& b = points.at(seg + 1);
    const CurveHandlePairDef& h = handles.at(seg);
    harkenstudiobuild::Curve curve;
    curve.points[0] = {a.x, a.value};
    curve.points[1] = {b.x, b.value};
    curve.handles[0] = {h.h1x, h.h1y, h.h2x, h.h2y};
    curve.pointCount = 2;
    return curve.Eval(x, runtimeDomain(scale));
}

float curveVolumeBezierValueAt(const QVector<CurvePointDef>& points,
                               const QVector<CurveHandlePairDef>& handles,
                               float x)
{
    return curveBezierValueAt(points, handles, x, CurveAxisScale::AmplitudeDb);
}

void curveClipX(float minimum, float maximum,
                QVector<CurvePointDef>& points,
                QVector<CurveHandlePairDef>& handles,
                CurveAxisScale scale)
{
    using Segment = std::array<QPointF, 4>;
    const auto split = [](const Segment& s, double t) {
        const auto a = s[0] + t * (s[1] - s[0]);
        const auto b = s[1] + t * (s[2] - s[1]);
        const auto c = s[2] + t * (s[3] - s[2]);
        const auto d = a + t * (b - a);
        const auto e = b + t * (c - b);
        const auto f = d + t * (e - d);
        return std::pair{Segment{s[0], a, d, f}, Segment{f, e, c, s[3]}};
    };
    const auto parameterAt = [&](const Segment& s, double x) {
        double low = 0.0;
        double high = 1.0;
        for (int iteration = 0; iteration < 64; ++iteration) {
            const double middle = (low + high) * 0.5;
            if (split(s, middle).first[3].x() < x) low = middle;
            else high = middle;
        }
        return (low + high) * 0.5;
    };
    QVector<CurvePointDef> clippedPoints;
    QVector<CurveHandlePairDef> clippedHandles;
    const auto append = [&](const CurvePointDef& a, const CurvePointDef& b,
                            const CurveHandlePairDef& h) {
        if (clippedPoints.isEmpty()) clippedPoints.append(a);
        clippedPoints.append(b);
        clippedHandles.append(h);
    };
    const auto appendSegment = [&](const Segment& s, const CurvePointDef& a, const CurvePointDef& b) {
        append(a, b, {std::clamp(static_cast<float>(s[1].x()), a.x, b.x),
                      curveAxisValueFromCoordinate(scale, s[1].y()),
                      std::clamp(static_cast<float>(s[2].x()), a.x, b.x),
                      curveAxisValueFromCoordinate(scale, s[2].y())});
    };
    for (qsizetype index = 0; index + 1 < points.size(); ++index) {
        const auto& a = points[index];
        const auto& b = points[index + 1];
        const auto& h = handles[index];
        const float left = std::max(minimum, a.x);
        const float right = std::min(maximum, b.x);
        if (!(left < right)) continue;
        CurvePointDef start = a;
        CurvePointDef end = b;
        if (left != a.x || right != b.x) {
            Segment s{{{a.x, curveAxisCoordinate(scale, a.value)},
                       {h.h1x, curveAxisCoordinate(scale, h.h1y)},
                       {h.h2x, curveAxisCoordinate(scale, h.h2y)},
                       {b.x, curveAxisCoordinate(scale, b.value)}}};
            if (right != b.x) s = split(s, parameterAt(s, right)).first;
            if (left != a.x) s = split(s, parameterAt(s, left)).second;
            if (left != a.x) start = {left, curveAxisValueFromCoordinate(scale, s[0].y())};
            if (right != b.x) end = {right, curveAxisValueFromCoordinate(scale, s[3].y())};
            const auto outside = [&](double x) {
                const float stored = static_cast<float>(x);
                return stored < left || stored > right;
            };
            if (outside(s[1].x()) || outside(s[2].x())) {
                const double d0 = s[1].x() - s[0].x();
                const double d1 = s[2].x() - s[1].x();
                const double d2 = s[3].x() - s[2].x();
                const auto [first, second] = split(s, (d0 - d1) / (d0 - 2.0 * d1 + d2));
                const CurvePointDef middle{static_cast<float>(first[3].x()),
                    curveAxisValueFromCoordinate(scale, first[3].y())};
                appendSegment(first, start, middle);
                appendSegment(second, middle, end);
            } else appendSegment(s, start, end);
        } else append(start, end, h);
    }
    points = std::move(clippedPoints);
    handles = std::move(clippedHandles);
}

double curveAxisNormalizedY(CurveAxisScale scale, float yMin, float yMax, float value)
{
    if (yMin == yMax)
        return 0.5;
    const double minimum = curveAxisCoordinate(scale, yMin);
    const double maximum = curveAxisCoordinate(scale, yMax);
    return (curveAxisCoordinate(scale, value) - minimum) / (maximum - minimum);
}

float curveAxisValueFromNormalizedY(CurveAxisScale scale, float yMin, float yMax, double normalizedY)
{
    if (normalizedY == 0.0 || yMin == yMax)
        return yMin;
    if (normalizedY == 1.0)
        return yMax;
    const double minimum = curveAxisCoordinate(scale, yMin);
    const double maximum = curveAxisCoordinate(scale, yMax);
    return curveAxisValueFromCoordinate(scale, minimum + normalizedY * (maximum - minimum));
}

}
