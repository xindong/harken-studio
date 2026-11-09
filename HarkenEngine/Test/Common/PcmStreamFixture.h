#pragma once

#include "harken/core/StreamRing.h"
#include <vector>

namespace harkentest {

inline void ConsumePcmFrames(harken::StreamRing& ring, std::uint32_t frames) {
    ring.DeliverFrames(frames);
    ring.ConsumeBytes(frames * ring.bytesPerFrame);
}

inline float PeekPcmSample(const harken::StreamRing& ring,
                           std::uint32_t frame, std::uint32_t channel = 0) {
    float sample;
    ring.CopyOut(ring.readIndex.load() + frame * ring.bytesPerFrame + channel * sizeof(float),
                 &sample, sizeof(sample));
    return sample;
}

// Existing planar test signals are staged here, then published through the
// production byte ring. No planar sample storage is visible to the renderer.
struct PcmStreamFixture : harken::StreamRing {
    float* planes[harken::kMaxChannels]{};
    std::uint32_t frameCapacity = 0;
    std::vector<std::uint8_t> storage;

    void Wire(float* const* input, std::uint32_t frames) {
        frameCapacity = frames;
        for (std::uint32_t ch = 0; ch < harken::kMaxChannels; ++ch) planes[ch] = input[ch];
    }
    void ResetForUse(std::uint32_t channels) {
        std::uint32_t size = 1;
        while (size < frameCapacity * channels * sizeof(float)) size <<= 1;
        storage.resize(size);
        harken::StreamRing::Wire(storage.data(), size);
        harken::StreamRing::ResetForUse(channels);
    }
    std::uint32_t WritableFrames() const { return WriteAvailableBytes() / bytesPerFrame; }
    std::uint32_t ContiguousStaging(float** output) {
        const auto position = static_cast<std::uint32_t>(writeFrames.load() % frameCapacity);
        for (std::uint32_t ch = 0; ch < channelCount; ++ch) output[ch] = planes[ch] + position;
        return std::min(WritableFrames(), frameCapacity - position);
    }
    void PublishFrames(std::uint32_t frames) {
        assert(frames <= WritableFrames());
        const auto at = writeIndex.load();
        const auto first = writeFrames.load();
        for (std::uint32_t frame = 0; frame < frames; ++frame)
            for (std::uint32_t ch = 0; ch < channelCount; ++ch)
                CopyIn(at + frame * bytesPerFrame + ch * sizeof(float),
                       &planes[ch][(first + frame) % frameCapacity], sizeof(float));
        PublishBytes(frames * bytesPerFrame, frames);
    }
};

} // namespace harkentest
