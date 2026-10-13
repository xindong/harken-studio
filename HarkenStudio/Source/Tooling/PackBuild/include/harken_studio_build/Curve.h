#ifndef HARKEN_STUDIO_BUILD_CURVE_H
#define HARKEN_STUDIO_BUILD_CURVE_H

#include <cstdint>

namespace harkenstudiobuild {

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
    bool SetPoints(const CurvePoint* pts, const CurveHandlePair* segmentHandles,
                   std::uint32_t count);
    float Eval(float x, CurveDomain domain = CurveDomain::LinearValue) const;
};

}
#endif
