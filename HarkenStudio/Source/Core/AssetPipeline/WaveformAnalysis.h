
#pragma once

#include <cstdint>
#include <cmath>

namespace VNX {

struct LR2Filter
{
    float s1 = 0.0f;
    float s2 = 0.0f;

    float processLP(float x, float alpha)
    {
        s1 += alpha * (x  - s1);
        s2 += alpha * (s1 - s2);
        return s2;
    }

    float processHP(float x, float alpha)
    {
        return x - processLP(x, alpha);
    }

    void reset()
    {
        s1 = 0.0f;
        s2 = 0.0f;
    }
};

struct BandEnergy
{
    float low  = 0.0f;
    float mid  = 0.0f;
    float high = 0.0f;
};

class BandAnalyzer
{
public:
    void prepare(uint32_t sampleRate)
    {
        constexpr float twoPi = 6.283185307f;
        constexpr float cutoffLow  = 300.0f;
        constexpr float cutoffHigh = 3000.0f;

        float sr = static_cast<float>(sampleRate);
        alphaLow_  = 1.0f - std::exp(-twoPi * cutoffLow  / sr);
        alphaHigh_ = 1.0f - std::exp(-twoPi * cutoffHigh / sr);
    }

    void reset()
    {
        lpLow_.reset();
        hpLow_.reset();
        lpHigh_.reset();
        hpHigh_.reset();
    }

    BandEnergy processChunk(const float* samples, int count)
    {
        if (count <= 0) return {};

        float accumLow  = 0.0f;
        float accumMid  = 0.0f;
        float accumHigh = 0.0f;

        for (int i = 0; i < count; ++i) {
            float x = samples[i];

            float low = lpLow_.processLP(x, alphaLow_);
            float rest = hpLow_.processHP(x, alphaLow_);

            float mid  = lpHigh_.processLP(rest, alphaHigh_);
            float high = hpHigh_.processHP(rest, alphaHigh_);

            accumLow  += low  * low;
            accumMid  += mid  * mid;
            accumHigh += high * high;
        }

        float invN = 1.0f / static_cast<float>(count);
        return {
            std::sqrt(accumLow  * invN),
            std::sqrt(accumMid  * invN),
            std::sqrt(accumHigh * invN)
        };
    }

private:
    float alphaLow_  = 0.0f;
    float alphaHigh_ = 0.0f;

    LR2Filter lpLow_;
    LR2Filter hpLow_;

    LR2Filter lpHigh_;
    LR2Filter hpHigh_;
};

}
