#include "CurveSemantics.h"

#include <harken_studio_build/Curve.h>

#include <algorithm>

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
