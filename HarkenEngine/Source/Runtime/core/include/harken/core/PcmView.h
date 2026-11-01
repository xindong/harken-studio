// Immutable POD view of registered PCM. This is the cross-module contract
// between media (which owns the data) and render (which reads it): render
// never includes media internals, it only consumes this view (spec section
// 3.1 dependency rule).
//
// Stride counts elements of the stored sample format. Planar storage has
// stride 1; interleaved storage has stride channelCount. Compact storage
// restores the original float bits before entering the render FIFO.
#ifndef HARKEN_CORE_PCM_VIEW_H
#define HARKEN_CORE_PCM_VIEW_H

#include "harken/core/EngineConfig.h"

#include <atomic>
#include <cassert>
#include <cstdint>

namespace harken {

struct PcmLoopRegion {
    std::uint64_t startFrame = 0;
    std::uint64_t endFrame = 0;
};

enum class PcmSampleFormat : std::uint32_t {
    Float32 = 0,
    ExactInt16 = 1
};

struct PCMView {
    const void* channels[kMaxChannels] = {};
    std::uint32_t channelCount = 0;
    std::uint32_t stride = 1;      // stored elements between consecutive frames
    std::uint32_t sampleRate = 0;  // source material rate
    PcmSampleFormat sampleFormat = PcmSampleFormat::Float32;
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
        assert(sampleFormat == PcmSampleFormat::Float32);
        return static_cast<const float*>(channels[channel]) + frame * stride;
    }

    const std::int16_t* Int16SamplePtr(std::uint32_t channel,
                                     std::uint64_t frame) const {
        assert(sampleFormat == PcmSampleFormat::ExactInt16);
        return static_cast<const std::int16_t*>(channels[channel]) + frame * stride;
    }

    float ReadSample(std::uint32_t channel, std::uint64_t frame) const {
        return sampleFormat == PcmSampleFormat::ExactInt16
            ? static_cast<float>(*Int16SamplePtr(channel, frame)) * 0x1p-15f
            : *SamplePtr(channel, frame);
    }
};

} // namespace harken

#endif // HARKEN_CORE_PCM_VIEW_H
