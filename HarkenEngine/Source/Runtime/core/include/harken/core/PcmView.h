// Immutable POD view of registered PCM. This is the cross-module contract
// between media (which owns the data) and render (which reads it): render
// never includes media internals, it only consumes this view (spec section
// 3.1 dependency rule).
//
// The layout is unified for the reader: channelPointer(ch) + n * stride
// addresses frame n of channel ch, regardless of whether the registered data
// was interleaved (stride = channelCount) or planar (stride = 1). The decode
// stage therefore has a single read path.
#ifndef HARKEN_CORE_PCM_VIEW_H
#define HARKEN_CORE_PCM_VIEW_H

#include "harken/core/EngineConfig.h"

#include <atomic>
#include <cstdint>

namespace harken {

struct PcmLoopRegion {
    std::uint64_t startFrame = 0;
    std::uint64_t endFrame = 0;
};

struct PCMView {
    const float* channels[kMaxChannels] = {};
    std::uint32_t channelCount = 0;
    std::uint32_t stride = 1;      // floats between consecutive frames
    std::uint32_t sampleRate = 0;  // source material rate
    std::uint32_t reserved = 0;
    std::uint64_t frames = 0;
    std::uint64_t contentRevision = 0;
    PcmLoopRegion loop;

    // Live-voice counter owned by the registry entry. A voice increments it
    // at start (logic thread) and decrements it when it ends (render path,
    // atomic — no lock). The registry refuses unregister while non-zero.
    std::atomic<std::uint32_t>* useCount = nullptr;

    bool Valid() const { return channelCount > 0 && frames > 0; }

    std::uint64_t LoopEndFrame() const {
        return loop.endFrame != 0 ? loop.endFrame : frames;
    }

    const float* SamplePtr(std::uint32_t channel, std::uint64_t frame) const {
        return channels[channel] + frame * stride;
    }
};

} // namespace harken

#endif // HARKEN_CORE_PCM_VIEW_H
