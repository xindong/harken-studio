#ifndef HARKEN_PARAM_CURVE_H
#define HARKEN_PARAM_CURVE_H

#include <cstdint>

namespace harken {

inline constexpr std::uint32_t kMaxCurvePoints = 8;

enum class CurveDomain { LinearValue, AmplitudeDb, MirroredAmplitudeDb };

double CurveValueToCoordinate(float value, CurveDomain domain);
float CurveValueFromCoordinate(double coordinate, CurveDomain domain);

struct CurvePoint {
    float x = 0.0f;
    float y = 0.0f;
};

struct CurveHandlePair {
    float h1x = 0.0f;
    float h1y = 0.0f;
    float h2x = 0.0f;
    float h2y = 0.0f;
};

struct Curve {
    CurvePoint points[kMaxCurvePoints] = {};
    CurveHandlePair handles[kMaxCurvePoints - 1] = {};
    std::uint32_t pointCount = 0;

    // Installs one to eight finite points and one handle pair per segment.
    // Handle x coordinates must lie within their segment's x interval.
    // Rejected input leaves the existing curve unchanged.
    bool SetPoints(const CurvePoint* pts, const CurveHandlePair* segmentHandles,
                   std::uint32_t count);

    // Evaluates the cubic Bezier curve, clamping outside the authored span.
    float Eval(float x, CurveDomain domain = CurveDomain::LinearValue) const;
};

inline constexpr std::uint32_t kCurveLutEntries = 128;

struct CurveLUT {
    float values[kCurveLutEntries + 1] = {};
    float xMin = 0.0f;
    float xMax = 0.0f;

    // Samples the curve at uniformly spaced x coordinates.
    void Bake(const Curve& curve);

    // Linearly interpolates between baked samples.
    float Eval(float x) const;
};

} // namespace harken

#endif // HARKEN_PARAM_CURVE_H
