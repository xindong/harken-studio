#pragma once

#include <QVector>

#include <cstdint>

namespace VNX {

// The system-wide unified curve form (blueprint D1): a point list plus one
// bezier handle pair per segment. Attenuation dimensions, binding mapping
// curves, and grid edge fades all carry this one shape; a two-point curve is
// the natural single-segment degenerate, never a second form.

// Runtime curves carry at most 8 control points (harkenstudiobuild::kMaxCurvePoints,
// HarkenEngine/Source/Runtime/param/include/harken_studio_build/Curve.h); authoring
// write paths refuse to grow a curve past this so export never has to trim.
inline constexpr int kCurveMaxPoints = 8;

struct CurvePointDef
{
    float x     = 0.0f;
    float value = 0.0f;
};

// One segment's bezier control-handle pair in absolute coordinates, mirroring
// the runtime contract struct (VnCurveHandlePair): handles[i] belongs to the
// segment points[i] -> points[i+1], h1 to the left point, h2 to the right.
struct CurveHandlePairDef
{
    float h1x = 0.0f;
    float h1y = 0.0f;
    float h2x = 0.0f;
    float h2y = 0.0f;
};

// Single normalization authority for authored curves, shared by JSON load,
// action parsing, and the canvas. Values clamp into [yMin, yMax] (handle y
// participates: the Bezier hull covers all four y control values), points
// sort by x, and handles must pair segments (size == points - 1) — a missing
// or mispaired set is rebuilt as endpoint-glued straight lines, a paired set
// clamps its x into the owning segment span. Idempotent.
void curveNormalize(float yMin, float yMax,
                    QVector<CurvePointDef>& points,
                    QVector<CurveHandlePairDef>& handles);

enum class CurveAxisScale : uint8_t
{
    LinearValue,
    AmplitudeDb,
    MirroredAmplitudeDb,
};

double curveAxisCoordinate(CurveAxisScale scale, float value);
float curveAxisValueFromCoordinate(CurveAxisScale scale, double coordinate);

float curveBezierValueAt(const QVector<CurvePointDef>& points,
                         const QVector<CurveHandlePairDef>& handles,
                         float x,
                         CurveAxisScale scale = CurveAxisScale::LinearValue);

float curveVolumeBezierValueAt(const QVector<CurvePointDef>& points,
                               const QVector<CurveHandlePairDef>& handles,
                               float x);

double curveAxisNormalizedY(CurveAxisScale scale, float yMin, float yMax, float value);
float curveAxisValueFromNormalizedY(CurveAxisScale scale, float yMin, float yMax, double normalizedY);

}
