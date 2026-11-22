#pragma once

#include "AssetTypes.h"

#include <vector>

namespace harken::studio {

class AssetConverter
{
public:
    static bool resample(const std::vector<float>& input,
                         int inputSampleRate, int outputSampleRate,
                         int channels, std::vector<float>& output);
    static void convertToMono(std::vector<float>& data, int channels);
    static bool processSamples(std::vector<float>& samples, int& sampleRate, int& channels,
                               uint32_t targetSampleRateHz, LoopInfo& loop,
                               QString* error = nullptr);
    static bool writeFloatWav(const QString& path,
                              const std::vector<float>& interleaved,
                              uint16_t channels, uint32_t sampleRate);
    static bool writeFloatWavCache(const QString& path,
                                   const std::vector<float>& interleaved,
                                   uint16_t channels, uint32_t sampleRate,
                                   QString* contentHash = nullptr);

private:
    static LoopInfo remapLoopInfo(const LoopInfo& sourceLoop,
                                  uint64_t sourceFramesAfterTrim,
                                  uint32_t sourceSampleRate,
                                  uint32_t outputSampleRate,
                                  uint64_t outputFrames);
};

}
