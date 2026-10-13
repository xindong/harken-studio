
#pragma once

#include <cmath>

namespace VNX {

inline float smoothTimed(float current, float target, float timeConstantMs, float deltaTimeMs)
{
    if (timeConstantMs <= 0.0f || deltaTimeMs <= 0.0f)
        return target;
    float coeff = 1.0f - std::exp(-deltaTimeMs / timeConstantMs);
    return current + (target - current) * coeff;
}

inline float smoothTimedAsym(float current, float target,
                             float attackMs, float releaseMs, float deltaTimeMs)
{
    float tc = (target > current) ? attackMs : releaseMs;
    return smoothTimed(current, target, tc, deltaTimeMs);
}

}
