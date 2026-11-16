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

// A structural edit's suffix must feed each detector from its own branch.
// Blending between every slot teaches downstream dynamics the temporary dry
// signal and can multiply their release histories into a near-silent dip.
// Retained downstream Limiters use adoption-time private state on the old
// branch and their installed instances on the new branch. Only an explicit
// host signal-flow boundary terminates the range; an unchanged slot does not.
template <class BeforeNext, class AfterNext>
inline void ProcessTransitionChainRange(InsertEffect* oldEffects, const float* oldBypass,
    InsertEffect* nextEffects, const float* nextBypass, const float* nextStarts,
    std::uint32_t first, std::uint32_t end, float* const* planes,
    float* const* dry, float* const* alternate, std::uint32_t channels,
    std::uint32_t offset, std::uint32_t frames, std::uint32_t done,
    std::uint32_t total, BeforeNext&& beforeNext, AfterNext&& afterNext) {
    if (frames == 0) return;
    const auto prefix = std::min(frames, total > done ? total - done : 0u);
    for (std::uint32_t ch = 0; ch < channels; ++ch)
        std::memcpy(alternate[ch], planes[ch] + offset, frames * sizeof(float));
    for (std::uint32_t slot = first; slot < end; ++slot) {
        bool ramping = false;
        float mix = oldBypass[slot];
        ProcessChainSlot(oldEffects[slot], ramping, mix, mix,
            planes, dry, channels, offset, prefix);
    }
    for (std::uint32_t slot = first; slot < end; ++slot) {
        bool ramping = nextStarts != nullptr && nextStarts[slot] >= 0.0f
            && nextStarts[slot] != nextBypass[slot];
        float mix = ramping ? nextStarts[slot] : nextBypass[slot];
        beforeNext(slot, alternate);
        ProcessChainSlot(nextEffects[slot], ramping, mix, nextBypass[slot],
            alternate, dry, channels, 0, frames);
        afterNext(slot, alternate);
    }
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        float* output = planes[ch] + offset;
        for (std::uint32_t i = 0; i < prefix; ++i) {
            if (done + i + 1u == total) output[i] = alternate[ch][i];
            else {
                const float alpha = static_cast<float>(done + i + 1u) / total;
                output[i] = output[i] * (1.0f - alpha) + alternate[ch][i] * alpha;
            }
        }
        std::memcpy(output + prefix, alternate[ch] + prefix,
            (frames - prefix) * sizeof(float));
    }
}

} // namespace harken

#endif // HARKEN_RENDER_EFFECT_CHAIN_H
