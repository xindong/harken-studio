
#pragma once

#include <cmath>
#include <algorithm>

namespace VNX {

enum class FreqScaleType { Log, Linear, Mel, Bark, ERB, Musical };

struct FrequencyScale
{
    static float freqToNorm(float freq, float minF, float maxF, FreqScaleType type)
    {
        freq = std::clamp(freq, minF, maxF);
        switch (type)
        {
        case FreqScaleType::Log:
            return std::log2(freq / minF) / std::log2(maxF / minF);
        case FreqScaleType::Linear:
            return (freq - minF) / (maxF - minF);
        case FreqScaleType::Mel:
            return (toMel(freq) - toMel(minF)) / (toMel(maxF) - toMel(minF));
        case FreqScaleType::Bark:
            return (toBark(freq) - toBark(minF)) / (toBark(maxF) - toBark(minF));
        case FreqScaleType::ERB:
            return (toERB(freq) - toERB(minF)) / (toERB(maxF) - toERB(minF));
        case FreqScaleType::Musical:
            return (toSemitone(freq) - toSemitone(minF)) / (toSemitone(maxF) - toSemitone(minF));
        }
        return 0.0f;
    }

    static float normToFreq(float norm, float minF, float maxF, FreqScaleType type)
    {
        norm = std::clamp(norm, 0.0f, 1.0f);
        switch (type)
        {
        case FreqScaleType::Log:
            return minF * std::pow(2.0f, norm * std::log2(maxF / minF));
        case FreqScaleType::Linear:
            return minF + norm * (maxF - minF);
        case FreqScaleType::Mel:
        {
            float a = toMel(minF), b = toMel(maxF);
            return fromMel(a + norm * (b - a));
        }
        case FreqScaleType::Bark:
        {
            float a = toBark(minF), b = toBark(maxF);
            return fromBark(a + norm * (b - a));
        }
        case FreqScaleType::ERB:
        {
            float a = toERB(minF), b = toERB(maxF);
            return fromERB(a + norm * (b - a));
        }
        case FreqScaleType::Musical:
        {
            float a = toSemitone(minF), b = toSemitone(maxF);
            return fromSemitone(a + norm * (b - a));
        }
        }
        return minF;
    }

    struct FreqLabel { float freq; const char* name; };

    static const FreqLabel* getLabels(FreqScaleType type, int& count)
    {
        static constexpr FreqLabel logLabels[] = {
            {20,"20"}, {50,"50"}, {100,"100"}, {200,"200"}, {500,"500"},
            {1000,"1k"}, {2000,"2k"}, {5000,"5k"}, {10000,"10k"}, {20000,"20k"}
        };
        static constexpr FreqLabel linearLabels[] = {
            {0,"0"}, {2000,"2k"}, {4000,"4k"}, {6000,"6k"}, {8000,"8k"},
            {10000,"10k"}, {12000,"12k"}, {14000,"14k"}, {16000,"16k"},
            {18000,"18k"}, {20000,"20k"}
        };
        static constexpr FreqLabel melLabels[] = {
            {20,"20"}, {100,"100"}, {250,"250"}, {500,"500"}, {1000,"1k"},
            {2000,"2k"}, {4000,"4k"}, {8000,"8k"}, {16000,"16k"}
        };
        static constexpr FreqLabel barkLabels[] = {
            {20,"20"}, {100,"100"}, {200,"200"}, {400,"400"}, {640,"640"},
            {1000,"1k"}, {1720,"1.7k"}, {2700,"2.7k"}, {4400,"4.4k"},
            {7700,"7.7k"}, {15500,"15.5k"}
        };
        static constexpr FreqLabel erbLabels[] = {
            {20,"20"}, {80,"80"}, {200,"200"}, {500,"500"}, {1000,"1k"},
            {2000,"2k"}, {4000,"4k"}, {8000,"8k"}, {16000,"16k"}
        };
        static constexpr FreqLabel musicalLabels[] = {
            {27.5f,"A0"}, {55,"A1"}, {110,"A2"}, {220,"A3"}, {440,"A4"},
            {880,"A5"}, {1760,"A6"}, {3520,"A7"}, {7040,"A8"}, {14080,"A9"}
        };

        switch (type)
        {
        case FreqScaleType::Log:     count = 10; return logLabels;
        case FreqScaleType::Linear:  count = 11; return linearLabels;
        case FreqScaleType::Mel:     count = 9;  return melLabels;
        case FreqScaleType::Bark:    count = 11; return barkLabels;
        case FreqScaleType::ERB:     count = 9;  return erbLabels;
        case FreqScaleType::Musical: count = 10; return musicalLabels;
        }
        count = 0;
        return nullptr;
    }

    static const char* scaleName(FreqScaleType type)
    {
        switch (type)
        {
        case FreqScaleType::Log:     return "Logarithmic";
        case FreqScaleType::Linear:  return "Linear";
        case FreqScaleType::Mel:     return "Mel";
        case FreqScaleType::Bark:    return "Bark";
        case FreqScaleType::ERB:     return "ERB";
        case FreqScaleType::Musical: return "Musical";
        }
        return "Log";
    }

    static const char* scaleShortName(FreqScaleType type)
    {
        switch (type)
        {
        case FreqScaleType::Log:     return "Log";
        case FreqScaleType::Linear:  return "Lin";
        case FreqScaleType::Mel:     return "Mel";
        case FreqScaleType::Bark:    return "Bark";
        case FreqScaleType::ERB:     return "ERB";
        case FreqScaleType::Musical: return "Note";
        }
        return "Log";
    }

    static constexpr int kScaleCount = 6;

    static FreqScaleType scaleFromIndex(int i)
    {
        constexpr FreqScaleType all[] = {
            FreqScaleType::Log, FreqScaleType::Linear, FreqScaleType::Mel,
            FreqScaleType::Bark, FreqScaleType::ERB, FreqScaleType::Musical
        };
        return (i >= 0 && i < kScaleCount) ? all[i] : FreqScaleType::Log;
    }

    static const char* freqToNoteName(float freqHz, char* buf, int bufSize)
    {
        if (freqHz <= 0.0f) { buf[0] = '\0'; return buf; }
        static const char* noteNames[] = {"C","C#","D","D#","E","F","F#","G","G#","A","A#","B"};
        float semitone = 12.0f * std::log2(freqHz / 440.0f) + 69.0f;
        int rounded = static_cast<int>(std::round(semitone));
        int noteIdx = ((rounded % 12) + 12) % 12;
        int octave = (rounded / 12) - 1;
        std::snprintf(buf, bufSize, "%s%d", noteNames[noteIdx], octave);
        return buf;
    }

    static const char* freqToString(float freqHz, char* buf, int bufSize)
    {
        if (freqHz >= 1000.0f)
            std::snprintf(buf, bufSize, "%.2f kHz", freqHz / 1000.0f);
        else
            std::snprintf(buf, bufSize, "%.1f Hz", freqHz);
        return buf;
    }

private:
    static float toMel(float f)        { return 2595.0f * std::log10(1.0f + f / 700.0f); }
    static float fromMel(float m)      { return 700.0f * (std::pow(10.0f, m / 2595.0f) - 1.0f); }
    static float toBark(float f)       { float x = f / 1000.0f; return 13.0f * std::atan(0.76f * x) + 3.5f * std::atan(x * x / 56.25f); }
    static float fromBark(float z)
    {
        float f = 100.0f;
        for (int i = 0; i < 20; ++i)
        {
            float err = toBark(f) - z;
            float deriv = (toBark(f + 1.0f) - toBark(f - 1.0f)) * 0.5f;
            if (std::abs(deriv) < 1e-10f) break;
            f -= err / deriv;
            f = std::max(f, 1.0f);
        }
        return f;
    }
    static float toERB(float f)        { return 21.4f * std::log10(4.37f * f / 1000.0f + 1.0f); }
    static float fromERB(float e)      { return 1000.0f * (std::pow(10.0f, e / 21.4f) - 1.0f) / 4.37f; }
    static float toSemitone(float f)   { return 12.0f * std::log2(std::max(f, 1.0f) / 440.0f); }
    static float fromSemitone(float s) { return 440.0f * std::pow(2.0f, s / 12.0f); }
};

}
