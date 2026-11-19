// Null device backend: no OS audio device. Blocks are rendered only when a
// host or test clock pumps them, which makes the full render path runnable
// headless (spec section 16). The output buffer is preallocated at init time;
// pumping performs no allocation.
#ifndef HARKEN_RENDER_NULL_BACKEND_H
#define HARKEN_RENDER_NULL_BACKEND_H

#include "harken/core/EngineConfig.h"
#include "harken/render/MixerLoop.h"

#include <cstdint>
#include <vector>

namespace harken {

class NullBackend {
public:
    NullBackend(MixerLoop& mixer, const EngineConfig& config);

    // Drives the mixer exactly as a device callback would, blockCount times.
    // Pumping is a no-op while the backend is suspended.
    void Pump(std::uint32_t blockCount);
    void SetSuspended(bool suspended) { m_suspended = suspended; }

    // View of the most recently rendered block (frames * channels floats),
    // for host/test inspection.
    const float* LastBlock() const { return m_buffer.data(); }
    std::uint32_t BlockFrames() const { return m_blockFrames; }
    std::uint32_t Channels() const { return m_channels; }

private:
    MixerLoop& m_mixer;
    std::uint32_t m_blockFrames;
    std::uint32_t m_channels;
    std::vector<float> m_buffer; // sized once at construction (logic thread)
    bool m_suspended = false;
};

} // namespace harken

#endif // HARKEN_RENDER_NULL_BACKEND_H
