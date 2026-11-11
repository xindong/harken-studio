// Shared insert-effect chain processing: one occupied chain slot rendered in
// place over planar channel data, with the one-block bypass crossfade. This is
// the single definition of the slot-level click-free crossfade discipline, so
// voice and bus chains apply the same wet/dry blend. The effect protocol
// (InsertEffect) is context-free, so this helper knows nothing about its host.
#ifndef HARKEN_RENDER_EFFECT_CHAIN_H
#define HARKEN_RENDER_EFFECT_CHAIN_H

#include "harken/render/InsertEffect.h"
#include "harken/render/BuiltinEffects.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

namespace harken {

// Processes one chain slot in place over planes[ch][offset, offset+frames).
//
// An empty slot (process == nullptr) or a zero-length block is skipped at zero
// cost — bit-identical to a passthrough. A slot that is fully active (settled
// mix 0, not crossfading) processes in place. A bypassed or crossfading slot
// keeps its input as the dry signal, processes to wet, then blends
// dry*mix + wet*(1-mix) with mix ramping linearly across this one block on a
// toggle so the transition never clicks; a completed crossfade settles at its
// target and disarms. dryScratch supplies one plane per channel (each at least
// `frames` long) for the dry copy; it is touched only on the blending path, so
// a host that never blends may pass planes it does not otherwise use.
//
// bypassMix / bypassRamping are updated in place (settled after a completed
// crossfade), matching the per-slot bypass state a chain host stores per slot.
inline void ProcessChainSlot(InsertEffect& fx, bool& bypassRamping,
                             float& bypassMix, float bypassTarget,
                             float* const* planes, float* const* dryScratch,
                             std::uint32_t channels, std::uint32_t offset,
                             std::uint32_t frames,
                             const EffectDynamicInputView* dynamicInput =
                                 nullptr, LimiterState* limiter = nullptr) {
    if (fx.process == nullptr || frames == 0) {
        if (limiter != nullptr) limiter->gainReduction = {};
        return;
    }
    const bool steadyActive = !bypassRamping && bypassMix == 0.0f;
    if (steadyActive) {
        if (dynamicInput != nullptr && fx.processDynamic != nullptr) {
            fx.processDynamic(fx.state, planes, channels, offset, frames,
                              dynamicInput);
        } else {
            fx.process(fx.state, planes, channels, offset, frames);
        }
        return;
    }
    const float span = static_cast<float>(frames);
    const float startMix = bypassMix;
    const float endMix = bypassRamping ? bypassTarget : bypassMix;
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        std::memcpy(dryScratch[ch], planes[ch] + offset,
                    static_cast<std::size_t>(frames) * sizeof(float));
    }
    if (dynamicInput != nullptr && fx.processDynamic != nullptr) {
        fx.processDynamic(fx.state, planes, channels, offset, frames,
                          dynamicInput);
    } else {
        fx.process(fx.state, planes, channels, offset, frames);
    }
    if (limiter != nullptr && limiter->gainReduction.status ==
                                  LimiterGainReductionStatus::Numeric) {
        auto& gr = limiter->gainReduction;
        if (startMix == 1.0f && endMix == 1.0f) {
            gr.minimumGain = gr.endGain = 1.0;
            gr.status = LimiterGainReductionStatus::Bypassed;
        } else {
            gr.status = LimiterGainReductionStatus::BypassTransition;
        }
    }
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        float* wet = planes[ch] + offset;
        const float* d = dryScratch[ch];
        for (std::uint32_t i = 0; i < frames; ++i) {
            const float mix =
                startMix +
                (endMix - startMix) * (static_cast<float>(i) + 1.0f) / span;
            // A zero-weight non-finite lane must not contaminate the selected
            // endpoint. The effect still ran above to advance its state.
            if (mix == 1.0f) {
                wet[i] = d[i];
            } else if (mix != 0.0f) {
                wet[i] = d[i] * mix + wet[i] * (1.0f - mix);
            }
        }
    }
    if (bypassRamping) {
        bypassMix = endMix;
        bypassRamping = false;
    }
}

// Two independent branches consume the same slot input. The host supplies the
// shared sample clock; this helper never advances it. Scratch is host-owned,
// prepared on logic, and reused sequentially by changed slots.
inline void ProcessTransitionChainSlot(InsertEffect& oldEffect, float oldBypass,
    InsertEffect& nextEffect, float nextBypass, float* const* planes,
    float* const* dry, float* const* alternate, std::uint32_t channels,
    std::uint32_t offset, std::uint32_t frames, std::uint32_t done,
    std::uint32_t total) {
    if (frames == 0) return;
    const auto prefix = std::min(frames, total > done ? total - done : 0u);
    if (prefix != 0) {
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            std::memcpy(dry[ch], planes[ch] + offset, prefix * sizeof(float));
            std::memcpy(alternate[ch], dry[ch], prefix * sizeof(float));
        }
        if (oldEffect.process) oldEffect.process(oldEffect.state, planes, channels, offset, prefix);
        if (nextEffect.process) nextEffect.process(nextEffect.state, alternate, channels, 0, prefix);
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            for (std::uint32_t i = 0; i < prefix; ++i) {
                const float input = dry[ch][i];
                const float next = !nextEffect.process || nextBypass == 1.0f ? input :
                    nextBypass == 0.0f ? alternate[ch][i] :
                    input * nextBypass + alternate[ch][i] * (1.0f - nextBypass);
                if (done + i + 1u == total) {
                    planes[ch][offset + i] = next;
                } else {
                    const float previous = !oldEffect.process || oldBypass == 1.0f ? input :
                        oldBypass == 0.0f ? planes[ch][offset + i] :
                        input * oldBypass + planes[ch][offset + i] * (1.0f - oldBypass);
                    const float alpha = static_cast<float>(done + i + 1u) / static_cast<float>(total);
                    planes[ch][offset + i] = previous * (1.0f - alpha) + next * alpha;
                }
            }
        }
    }
    if (prefix != frames) {
        bool ramping = false;
        float mix = nextBypass;
        ProcessChainSlot(nextEffect, ramping, mix, nextBypass, planes, dry,
            channels, offset + prefix, frames - prefix);
    }
}

} // namespace harken

#endif // HARKEN_RENDER_EFFECT_CHAIN_H
