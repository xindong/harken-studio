#include "harken/render/NullBackend.h"

namespace harken {

NullBackend::NullBackend(MixerLoop& mixer, const EngineConfig& config)
    : m_mixer(mixer),
      m_blockFrames(config.blockFrames),
      // The null backend presents the resolved device layout's channel count,
      // exactly as the real device does; at the stereo default this is 2 and
      // the output is byte-identical to M4. When the device layout is narrower
    // than the mix, RenderBlock applies the master->device downmix into
      // exactly these channels.
      m_channels(config.deviceChannelCount),
      m_buffer(static_cast<std::size_t>(config.blockFrames) *
                   config.deviceChannelCount,
               0.0f) {}

void NullBackend::Pump(std::uint32_t blockCount) {
    if (m_suspended) {
        return;
    }
    for (std::uint32_t i = 0; i < blockCount; ++i) {
        m_mixer.RenderBlock(m_buffer.data(), m_blockFrames, m_channels);
    }
}

} // namespace harken
