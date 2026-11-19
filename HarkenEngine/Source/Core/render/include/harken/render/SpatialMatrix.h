#ifndef HARKEN_RENDER_SPATIAL_MATRIX_H
#define HARKEN_RENDER_SPATIAL_MATRIX_H

#include "harken/core/ChannelLayout.h"
#include "harken/dsp/Biquad.h"

namespace harken {

struct SpatialMatrix {
    float gain[2][kMaxChannels] = {};
};

inline void FillSpatialFront(float* gains, std::uint32_t layout, float pan,
                             const BiquadTrigTable& trig) {
    int front[3];
    const auto count = FrontSpeakers(layout, front);
    if (count == 0) return;
    if (count == 1) {
        gains[front[0]] = 1.0f;
        return;
    }
    const float position = (std::clamp(pan, -1.0f, 1.0f) + 1.0f) *
                           0.5f * static_cast<float>(count - 1);
    const auto segment = std::min(static_cast<std::uint32_t>(position), count - 2);
    const float fraction = position - static_cast<float>(segment);
    // The immutable table maps frequency/sample-rate to angle. This ratio
    // selects the quarter circle without evaluating trigonometry on audio.
    const auto point = trig.Lookup(4.0f, fraction);
    gains[front[segment]] = fraction == 1.0f ? 0.0f : static_cast<float>(point.cosine);
    gains[front[segment + 1]] = static_cast<float>(point.sine);
}

inline SpatialMatrix MakeSpatialMatrix(std::uint32_t channels,
                                       std::uint32_t layout, float pan2d,
                                       float pan3d, float width, float mix,
                                       const BiquadTrigTable& trig) {
    SpatialMatrix direct;
    SpatialMatrix spatial;
    const int left = ChannelOfRole(layout, ChannelRole::FL);
    const int right = ChannelOfRole(layout, ChannelRole::FR);
    if (channels == 1) {
        if (left < 0 || right < 0) {
            direct.gain[0][0] = 1.0f;
        } else {
            float pair[kMaxChannels] = {};
            FillSpatialFront(pair, VN_CHANNEL_LAYOUT_STEREO, pan2d, trig);
            direct.gain[0][left] = pair[0];
            direct.gain[0][right] = pair[1];
        }
        FillSpatialFront(spatial.gain[0], layout, pan3d, trig);
        if (width != 0.0f) {
            float center[kMaxChannels] = {};
            FillSpatialFront(center, layout, 0.0f, trig);
            float power = 0.0f;
            for (std::uint32_t ch = 0; ch < kMaxChannels; ++ch) {
                auto& g = spatial.gain[0][ch];
                g = (1.0f - width) * g + width * center[ch];
                power += g * g;
            }
            const float scale = power > 0.0f ? 1.0f / std::sqrt(power) : 0.0f;
            for (float& g : spatial.gain[0]) g *= scale;
        }
        if (left >= 0 && right >= 0) {
            // Anchor mono to unity per side at stereo center, like an identical
            // L/R source at zero width. Keep the equal-power pan shape; mono
            // mix remains unity and stereo source columns retain their law.
            for (float& g : direct.gain[0]) g /= kDownmixMinus3dB;
            for (float& g : spatial.gain[0]) g /= kDownmixMinus3dB;
        }
    } else {
        const float balanceLeft = std::min(1.0f, 1.0f - pan2d);
        const float balanceRight = std::min(1.0f, 1.0f + pan2d);
        if (left < 0 || right < 0) {
            direct.gain[0][0] = balanceLeft * kDownmixMinus3dB;
            direct.gain[1][0] = balanceRight * kDownmixMinus3dB;
        } else {
            direct.gain[0][left] = balanceLeft;
            direct.gain[1][right] = balanceRight;
        }
        FillSpatialFront(spatial.gain[0], layout, (1.0f - width) * pan3d - width, trig);
        FillSpatialFront(spatial.gain[1], layout, (1.0f - width) * pan3d + width, trig);
        float overlap = 0.0f;
        for (std::uint32_t ch = 0; ch < kMaxChannels; ++ch)
            overlap += spatial.gain[0][ch] * spatial.gain[1][ch];
        // Separated channels retain unity; coincident columns share the
        // point-source fold gain. Clamp only lookup-table rounding error.
        const float scale = 1.0f / std::sqrt(1.0f + std::clamp(overlap, 0.0f, 1.0f));
        for (auto& column : spatial.gain)
            for (float& g : column) g *= scale;
    }
    for (std::uint32_t source = 0; source < channels; ++source)
        for (std::uint32_t ch = 0; ch < kMaxChannels; ++ch)
            direct.gain[source][ch] = (1.0f - mix) * direct.gain[source][ch] +
                                      mix * spatial.gain[source][ch];
    return direct;
}

struct SpatialMatrixTransition {
    SpatialMatrix start;
    SpatialMatrix target;
    std::uint32_t frames = 1;
    std::uint32_t progress = 1;
    float inverseSpan = 1.0f;

    bool Settled() const { return progress >= frames; }
    float Mix() const {
        return Settled() || progress + 1 >= frames ? 1.0f
            : static_cast<float>(progress) * inverseSpan;
    }
    float Gain(std::uint32_t source, std::uint32_t ch, float mix) const {
        if (mix >= 1.0f) return target.gain[source][ch];
        if (mix <= 0.0f) return start.gain[source][ch];
        return start.gain[source][ch] +
               (target.gain[source][ch] - start.gain[source][ch]) * mix;
    }
    void Set(const SpatialMatrix& next, std::uint32_t requestedFrames) {
        bool changed = false;
        for (std::uint32_t source = 0; source < 2; ++source)
            for (std::uint32_t ch = 0; ch < kMaxChannels; ++ch)
                changed |= target.gain[source][ch] != next.gain[source][ch];
        if (!changed && requestedFrames > 1) return;
        const float last = Settled() ? 1.0f : progress == 0 ? 0.0f
            : static_cast<float>(progress - 1) * inverseSpan;
        for (std::uint32_t source = 0; source < 2; ++source)
            for (std::uint32_t ch = 0; ch < kMaxChannels; ++ch)
                start.gain[source][ch] = requestedFrames > 1
                    ? Gain(source, ch, last) : next.gain[source][ch];
        target = next;
        frames = std::max(1u, requestedFrames);
        progress = frames == 1 ? 1 : 0;
        inverseSpan = frames == 1 ? 1.0f : 1.0f / static_cast<float>(frames - 1);
    }
    void AdvanceFrame() { if (!Settled()) ++progress; }
};

} // namespace harken
#endif
