#pragma once

#include "Widgets/Common/AmplitudeAxis.h"

#include <algorithm>

namespace VNX {

struct MeterBallistics
{
    float levelDb = kAmplitudeFloorDb;
    float peakDb = kAmplitudeFloorDb;
    float holdSeconds = 0.0f;

    void advance(float inputDb, float seconds)
    {
        constexpr float releaseDbPerSecond = 60.0f;
        constexpr float peakHoldSeconds = 1.0f;
        levelDb = inputDb;
        const float releaseSeconds = std::max(0.0f, seconds - holdSeconds);
        holdSeconds = std::max(0.0f, holdSeconds - seconds);
        const float releasedPeak = peakDb - releaseDbPerSecond * releaseSeconds;
        if (inputDb >= releasedPeak) {
            peakDb = inputDb;
            holdSeconds = peakHoldSeconds;
        } else {
            peakDb = std::max(levelDb, releasedPeak);
        }
    }
};

}
