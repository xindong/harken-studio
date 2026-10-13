#include "harken/render/BuiltinEffects.h"

#include "harken/render/generated/harken_effect_registry.g.h"

#include <harken/Harken.h>

#include <algorithm>
#include <cmath>
#include <new>

namespace harken {

// The built-in effects are realized as free functions with external
// linkage — the generated registry (harken_effect_registry.g.h) binds their
// addresses into its rows, so the public entry points below are table lookups
// rather than hand-written type switches. The kernels themselves (process/tail/
// setParam/reset) are referenced by the registry rather than a hand-written
// type switch.

// --- feedback delay ---

void DelayReset(void* state) {
    auto* d = static_cast<DelayEffectState*>(state);
    d->writeIndex = 0;
    d->delaySamples = 1;
    d->feedback = 0.0f;
    d->wet = 0.0f;
    d->dry = 1.0f;
    d->targetWet = 0.0f;
    d->targetDry = 1.0f;
    d->wetIncrement = 0.0f;
    d->dryIncrement = 0.0f;
    d->transitionFramesRemaining = 0;
    d->hasRendered = false;
    if (d->lineBase != nullptr) {
        std::fill(d->lineBase,
                  d->lineBase + static_cast<std::size_t>(d->lineFrames) *
                                    kMaxChannels,
                  0.0f);
    }
}

void DelaySetParam(void* state, std::uint32_t paramIndex, float value) {
    auto* d = static_cast<DelayEffectState*>(state);
    switch (paramIndex) {
    case kDelayParamTimeMs: {
        // Milliseconds -> samples at the engine rate, clamped into the
        // preallocated line. Rounding to nearest keeps the mapping stable
        // across rebuilds; the clamp is a guard (validated values arrive in
        // range).
        double samples = static_cast<double>(value) *
                         static_cast<double>(d->sampleRate) / 1000.0;
        std::uint64_t s = (samples < 1.0)
                              ? 1u
                              : static_cast<std::uint64_t>(samples + 0.5);
        if (d->lineFrames != 0 && s > d->lineFrames) {
            s = d->lineFrames;
        }
        if (s < 1) {
            s = 1;
        }
        d->delaySamples = static_cast<std::uint32_t>(s);
        break;
    }
    case kDelayParamFeedback:
        d->feedback = value;
        break;
    case kDelayParamWet:
        d->wet = value;
        d->targetWet = value;
        d->wetIncrement = 0.0f;
        d->transitionFramesRemaining = 0;
        break;
    case kDelayParamDry:
        d->dry = value;
        d->targetDry = value;
        d->dryIncrement = 0.0f;
        d->transitionFramesRemaining = 0;
        break;
    default:
        break; // unknown index: no-op (validated at the API boundary)
    }
}

void DelayApplyPreparedParam(void* state, std::uint32_t paramIndex,
                             float prepared,
                             std::uint32_t transitionFrames) {
    auto* d = static_cast<DelayEffectState*>(state);
    if (paramIndex != kDelayParamWet && paramIndex != kDelayParamDry) {
        DelaySetParam(state, paramIndex, prepared);
        return;
    }
    if (!d->hasRendered || transitionFrames == 0) {
        DelaySetParam(state, paramIndex, prepared);
        return;
    }
    if (paramIndex == kDelayParamWet) {
        d->targetWet = prepared;
    } else {
        d->targetDry = prepared;
    }
    const float inverseFrames =
        1.0f / static_cast<float>(transitionFrames);
    d->wetIncrement = (d->targetWet - d->wet) * inverseFrames;
    d->dryIncrement = (d->targetDry - d->dry) * inverseFrames;
    d->transitionFramesRemaining = transitionFrames;
}

void DelayProcess(void* state, float* const* channels,
                  std::uint32_t channelCount, std::uint32_t offset,
                  std::uint32_t frames) {
    auto* d = static_cast<DelayEffectState*>(state);
    const std::uint32_t lineFrames = d->lineFrames;
    if (lineFrames == 0) {
        return;
    }
    const std::uint32_t delay = d->delaySamples;
    const float fb = d->feedback;
    std::uint32_t w = d->writeIndex;
    for (std::uint32_t i = 0; i < frames; ++i) {
        if (d->transitionFramesRemaining != 0) {
            if (d->transitionFramesRemaining == 1) {
                d->wet = d->targetWet;
                d->dry = d->targetDry;
                d->transitionFramesRemaining = 0;
            } else {
                d->wet += d->wetIncrement;
                d->dry += d->dryIncrement;
                --d->transitionFramesRemaining;
            }
        }
        // Read index is shared across channels so every channel taps the same
        // delay position; it is computed before the per-channel writes so the
        // just-written frame never feeds itself at delay == lineFrames.
        const std::uint32_t r =
            (w >= delay) ? (w - delay) : (w + lineFrames - delay);
        for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
            float* line = d->lineBase + static_cast<std::size_t>(ch) * lineFrames;
            float* s = channels[ch] + offset;
            const float x = s[i];
            const float delayed = line[r];
            s[i] = d->dry * x + d->wet * delayed;
            // Soft-saturated feedback: an analog-style waveshaper in the loop
            // keeps it bounded even near unity feedback and, being non-linear,
            // makes chain order relative to the linear tone effect audible.
            line[w] = x + fb * std::tanh(delayed);
        }
        w = (w + 1 == lineFrames) ? 0u : w + 1;
    }
    d->writeIndex = w;
    d->hasRendered = d->hasRendered || frames != 0;
}

std::uint32_t DelayTail(const void* state) {
    const auto* d = static_cast<const DelayEffectState*>(state);
    const std::uint32_t delay = d->delaySamples;
    const float fb = d->feedback;
    if (!(fb > 0.0f)) {
        // No feedback: a single delayed copy, one delay period of tail.
        return delay;
    }
    // -60 dB decay of the feedback loop under the linear model fb^n <= 1e-3:
    // n >= ln(1e-3)/ln(fb). The saturating feedback decays at least as fast
    // (|tanh(x)| <= |x|), so this is a conservative upper bound that never
    // truncates the real tail. One extra period covers the initial delay
    // before the first echo emerges.
    const double n =
        std::ceil(std::log(1.0e-3) / std::log(static_cast<double>(fb)));
    double tail = static_cast<double>(delay) * (n + 1.0);
    // Clamp below the infinite sentinel so an extreme feedback can never alias
    // it (the real tail is always finite for feedback < 1).
    const double cap = static_cast<double>(kInfiniteTail - 1u);
    if (tail > cap) {
        tail = cap;
    }
    if (!(tail >= 0.0)) {
        tail = 0.0;
    }
    return static_cast<std::uint32_t>(tail);
}

VnResult DelayValidateParam(std::uint32_t paramIndex, float value,
                            std::uint32_t /*sampleRate*/) {
    // Finiteness is checked once by ValidateEffectParam before dispatch; the
    // delay parameters do not depend on the sample rate.
    switch (paramIndex) {
    case kDelayParamTimeMs:
        return (value > 0.0f && value <= kDelayEffectMaxMs) ? VN_OK
                                                            : VN_INVALID_ARG;
    case kDelayParamFeedback:
        // Strictly below unity: a feedback loop at or above 1 would never
        // decay to the reported finite tail.
        return (value >= 0.0f && value < 1.0f) ? VN_OK : VN_INVALID_ARG;
    case kDelayParamWet:
    case kDelayParamDry:
        return (value >= 0.0f && value <= kEffectMaxLevel) ? VN_OK
                                                           : VN_INVALID_ARG;
    default:
        return VN_INVALID_ARG;
    }
}

VnResult DelayValidateDesc(const void* desc, std::uint32_t sampleRate) {
    const auto* d = static_cast<const VnFeedbackDelayDesc*>(desc);
    if (d->structSize != sizeof(VnFeedbackDelayDesc)) {
        return VN_INVALID_ARG;
    }
    VnResult r;
    if ((r = ValidateEffectParam(VN_EFFECT_FEEDBACK_DELAY, kDelayParamTimeMs,
                                 d->delayMs, sampleRate)) != VN_OK)
        return r;
    if ((r = ValidateEffectParam(VN_EFFECT_FEEDBACK_DELAY, kDelayParamFeedback,
                                 d->feedback, sampleRate)) != VN_OK)
        return r;
    if ((r = ValidateEffectParam(VN_EFFECT_FEEDBACK_DELAY, kDelayParamWet,
                                 d->wet, sampleRate)) != VN_OK)
        return r;
    if ((r = ValidateEffectParam(VN_EFFECT_FEEDBACK_DELAY, kDelayParamDry,
                                 d->dry, sampleRate)) != VN_OK)
        return r;
    return VN_OK;
}

std::uint32_t DelayParamsFromDesc(const void* desc, float* out) {
    const auto* d = static_cast<const VnFeedbackDelayDesc*>(desc);
    out[kDelayParamTimeMs] = d->delayMs;
    out[kDelayParamFeedback] = d->feedback;
    out[kDelayParamWet] = d->wet;
    out[kDelayParamDry] = d->dry;
    return kDelayParamCount;
}

std::uint32_t DelayExtraFramesPerChannel(const EngineConfig& config) {
    // The per-instance delay line holds a full kDelayEffectMaxSeconds at the
    // engine rate; the pool carves this many frames per channel per instance.
    return static_cast<std::uint32_t>(std::ceil(
        kDelayEffectMaxSeconds *
        static_cast<double>(config.mixRate.SampleRate())));
}

std::uint32_t DelayCapacitySource(const EngineConfig& config) {
    return config.delayEffectPoolCapacity;
}

void DelayInitInstance(void* mem, float* extra,
                       std::uint32_t extraFramesPerChannel,
                       std::uint32_t sampleRate) {
    auto* d = new (mem) DelayEffectState{};
    d->lineBase = extra;
    d->lineFrames = extraFramesPerChannel;
    d->sampleRate = sampleRate;
}

// --- three-band tone ---

namespace {

void ToneRecomputeLowShelf(ToneEffectState* t) {
    t->lowShelf = MakeLowShelf(static_cast<float>(t->sampleRate),
                               t->lowShelfFreqHz, kToneShelfQ,
                               t->lowShelfGainDb);
    t->lowShelfTailFrames = EstimateBiquadTailFrames(t->lowShelf);
}
void ToneRecomputePeak(ToneEffectState* t) {
    t->peak = MakePeaking(static_cast<float>(t->sampleRate), t->peakFreqHz,
                          t->peakQ, t->peakGainDb);
    t->peakTailFrames = EstimateBiquadTailFrames(t->peak);
}
void ToneRecomputeHighShelf(ToneEffectState* t) {
    t->highShelf = MakeHighShelf(static_cast<float>(t->sampleRate),
                                 t->highShelfFreqHz, kToneShelfQ,
                                 t->highShelfGainDb);
    t->highShelfTailFrames = EstimateBiquadTailFrames(t->highShelf);
}

} // namespace

bool PrepareToneSection(std::uint32_t paramIndex, const float* params,
                        std::uint32_t sampleRate, BiquadCoeffs* outCoeffs,
                        std::uint32_t* outTailFrames) {
    if (params == nullptr || outCoeffs == nullptr || outTailFrames == nullptr) {
        return false;
    }
    BiquadCoeffs coeffs;
    switch (paramIndex) {
    case kToneParamLowShelfGainDb:
    case kToneParamLowShelfFreqHz:
        coeffs = MakeLowShelf(static_cast<float>(sampleRate),
                              params[kToneParamLowShelfFreqHz], kToneShelfQ,
                              params[kToneParamLowShelfGainDb]);
        break;
    case kToneParamPeakGainDb:
    case kToneParamPeakFreqHz:
    case kToneParamPeakQ:
        coeffs = MakePeaking(static_cast<float>(sampleRate),
                             params[kToneParamPeakFreqHz],
                             params[kToneParamPeakQ],
                             params[kToneParamPeakGainDb]);
        break;
    case kToneParamHighShelfGainDb:
    case kToneParamHighShelfFreqHz:
        coeffs = MakeHighShelf(static_cast<float>(sampleRate),
                               params[kToneParamHighShelfFreqHz], kToneShelfQ,
                               params[kToneParamHighShelfGainDb]);
        break;
    default:
        return false;
    }
    if (!IsBiquadStable(coeffs)) {
        return false;
    }
    *outCoeffs = coeffs;
    *outTailFrames = EstimateBiquadTailFrames(coeffs);
    return true;
}

void ToneReset(void* state) {
    auto* t = static_cast<ToneEffectState*>(state);
    t->lowShelfGainDb = 0.0f;
    t->lowShelfFreqHz = 200.0f;
    t->peakGainDb = 0.0f;
    t->peakFreqHz = 1000.0f;
    t->peakQ = 1.0f;
    t->highShelfGainDb = 0.0f;
    t->highShelfFreqHz = 6000.0f;
    t->lowShelfEverActive = false;
    t->peakEverActive = false;
    t->highShelfEverActive = false;
    t->lowShelfTransition.Reset();
    t->peakTransition.Reset();
    t->highShelfTransition.Reset();
    t->hasRendered = false;
    for (std::uint32_t ch = 0; ch < kMaxChannels; ++ch) {
        t->lowShelfState[ch].Reset();
        t->peakState[ch].Reset();
        t->highShelfState[ch].Reset();
    }
    ToneRecomputeLowShelf(t);
    ToneRecomputePeak(t);
    ToneRecomputeHighShelf(t);
}

void ToneSetParam(void* state, std::uint32_t paramIndex, float value) {
    auto* t = static_cast<ToneEffectState*>(state);
    switch (paramIndex) {
    case kToneParamLowShelfGainDb:
        t->lowShelfGainDb = value;
        t->lowShelfEverActive = t->lowShelfEverActive || value != 0.0f;
        ToneRecomputeLowShelf(t);
        break;
    case kToneParamLowShelfFreqHz:
        t->lowShelfFreqHz = value;
        ToneRecomputeLowShelf(t);
        break;
    case kToneParamPeakGainDb:
        t->peakGainDb = value;
        t->peakEverActive = t->peakEverActive || value != 0.0f;
        ToneRecomputePeak(t);
        break;
    case kToneParamPeakFreqHz:
        t->peakFreqHz = value;
        ToneRecomputePeak(t);
        break;
    case kToneParamPeakQ:
        t->peakQ = value;
        ToneRecomputePeak(t);
        break;
    case kToneParamHighShelfGainDb:
        t->highShelfGainDb = value;
        t->highShelfEverActive = t->highShelfEverActive || value != 0.0f;
        ToneRecomputeHighShelf(t);
        break;
    case kToneParamHighShelfFreqHz:
        t->highShelfFreqHz = value;
        ToneRecomputeHighShelf(t);
        break;
    default:
        break; // unknown index: no-op (validated at the API boundary)
    }
}

void ToneApplyPreparedParam(void* state, std::uint32_t paramIndex, float value,
                            const BiquadCoeffs& coeffs,
                            std::uint32_t tailFrames) {
    auto* t = static_cast<ToneEffectState*>(state);
    const auto apply = [t](BiquadCoeffs& activeCoeffs,
                           std::uint32_t& activeTail,
                           BiquadTransitionState<kMaxChannels>& transition,
                           const BiquadCoeffs& targetCoeffs,
                           std::uint32_t targetTail) {
        if (!t->hasRendered) {
            activeCoeffs = targetCoeffs;
            activeTail = targetTail;
            transition.Reset();
            return;
        }
        RequestBiquadTransition(
            transition, true, activeCoeffs, targetTail, true, targetCoeffs,
            BiquadSmoothingFrames(t->sampleRate));
    };
    switch (paramIndex) {
    case kToneParamLowShelfGainDb:
        t->lowShelfGainDb = value;
        t->lowShelfEverActive = t->lowShelfEverActive || value != 0.0f;
        apply(t->lowShelf, t->lowShelfTailFrames, t->lowShelfTransition,
              coeffs, tailFrames);
        break;
    case kToneParamLowShelfFreqHz:
        t->lowShelfFreqHz = value;
        apply(t->lowShelf, t->lowShelfTailFrames, t->lowShelfTransition,
              coeffs, tailFrames);
        break;
    case kToneParamPeakGainDb:
        t->peakGainDb = value;
        t->peakEverActive = t->peakEverActive || value != 0.0f;
        apply(t->peak, t->peakTailFrames, t->peakTransition, coeffs,
              tailFrames);
        break;
    case kToneParamPeakFreqHz:
        t->peakFreqHz = value;
        apply(t->peak, t->peakTailFrames, t->peakTransition, coeffs,
              tailFrames);
        break;
    case kToneParamPeakQ:
        t->peakQ = value;
        apply(t->peak, t->peakTailFrames, t->peakTransition, coeffs,
              tailFrames);
        break;
    case kToneParamHighShelfGainDb:
        t->highShelfGainDb = value;
        t->highShelfEverActive = t->highShelfEverActive || value != 0.0f;
        apply(t->highShelf, t->highShelfTailFrames,
              t->highShelfTransition, coeffs, tailFrames);
        break;
    case kToneParamHighShelfFreqHz:
        t->highShelfFreqHz = value;
        apply(t->highShelf, t->highShelfTailFrames,
              t->highShelfTransition, coeffs, tailFrames);
        break;
    default:
        break;
    }
}

void ToneProcess(void* state, float* const* channels,
                 std::uint32_t channelCount, std::uint32_t offset,
                 std::uint32_t frames) {
    auto* t = static_cast<ToneEffectState*>(state);
    bool lowEnabled = true;
    bool peakEnabled = true;
    bool highEnabled = true;
    BiquadProcessSmoothed(t->lowShelfTransition, lowEnabled, t->lowShelf,
                          t->lowShelfState, t->lowShelfTailFrames, channels,
                          channelCount, offset, frames);
    BiquadProcessSmoothed(t->peakTransition, peakEnabled, t->peak,
                          t->peakState, t->peakTailFrames, channels,
                          channelCount, offset, frames);
    BiquadProcessSmoothed(t->highShelfTransition, highEnabled, t->highShelf,
                          t->highShelfState, t->highShelfTailFrames, channels,
                          channelCount, offset, frames);
    if (frames > 0) {
        t->hasRendered = true;
    }
}

std::uint32_t ToneTail(const void* state) {
    const auto* t = static_cast<const ToneEffectState*>(state);
    std::uint32_t tail = 0;
    const std::uint32_t lowTail = BiquadTransitionTailFrames(
        t->lowShelfTransition, true, t->lowShelfTailFrames);
    const std::uint32_t peakTail = BiquadTransitionTailFrames(
        t->peakTransition, true, t->peakTailFrames);
    const std::uint32_t highTail = BiquadTransitionTailFrames(
        t->highShelfTransition, true, t->highShelfTailFrames);
    if (t->lowShelfGainDb != 0.0f || t->lowShelfEverActive ||
        t->lowShelfTransition.transitioning) {
        tail = SaturatingTailAdd(tail, lowTail);
    }
    if (t->peakGainDb != 0.0f || t->peakEverActive ||
        t->peakTransition.transitioning) {
        tail = SaturatingTailAdd(tail, peakTail);
    }
    if (t->highShelfGainDb != 0.0f || t->highShelfEverActive ||
        t->highShelfTransition.transitioning) {
        tail = SaturatingTailAdd(tail, highTail);
    }
    return tail;
}

VnResult ToneValidateParam(std::uint32_t paramIndex, float value,
                           std::uint32_t sampleRate) {
    // Finiteness is checked once by ValidateEffectParam before dispatch.
    const float nyquist = 0.5f * static_cast<float>(sampleRate);
    switch (paramIndex) {
    case kToneParamLowShelfGainDb:
    case kToneParamPeakGainDb:
    case kToneParamHighShelfGainDb:
        return (value >= -kToneMaxGainDb && value <= kToneMaxGainDb)
                   ? VN_OK
                   : VN_INVALID_ARG;
    case kToneParamLowShelfFreqHz:
    case kToneParamPeakFreqHz:
    case kToneParamHighShelfFreqHz:
        return (value > 0.0f && value < nyquist) ? VN_OK : VN_INVALID_ARG;
    case kToneParamPeakQ:
        return (value > 0.0f && value <= kToneMaxQ) ? VN_OK : VN_INVALID_ARG;
    default:
        return VN_INVALID_ARG;
    }
}

VnResult ToneValidateDesc(const void* desc, std::uint32_t sampleRate) {
    const auto* t = static_cast<const VnThreeBandToneDesc*>(desc);
    if (t->structSize != sizeof(VnThreeBandToneDesc)) {
        return VN_INVALID_ARG;
    }
    const struct {
        std::uint32_t index;
        float value;
    } params[kToneParamCount] = {
        {kToneParamLowShelfGainDb, t->lowShelfGainDb},
        {kToneParamLowShelfFreqHz, t->lowShelfFreqHz},
        {kToneParamPeakGainDb, t->peakGainDb},
        {kToneParamPeakFreqHz, t->peakFreqHz},
        {kToneParamPeakQ, t->peakQ},
        {kToneParamHighShelfGainDb, t->highShelfGainDb},
        {kToneParamHighShelfFreqHz, t->highShelfFreqHz},
    };
    for (const auto& p : params) {
        const VnResult r = ValidateEffectParam(VN_EFFECT_THREE_BAND_TONE,
                                               p.index, p.value, sampleRate);
        if (r != VN_OK) {
            return r;
        }
    }
    float values[kToneParamCount] = {};
    for (const auto& p : params) {
        values[p.index] = p.value;
    }
    const std::uint32_t sectionParams[] = {
        kToneParamLowShelfGainDb, kToneParamPeakGainDb,
        kToneParamHighShelfGainDb};
    for (const std::uint32_t index : sectionParams) {
        BiquadCoeffs coeffs;
        std::uint32_t tailFrames = 0;
        if (!PrepareToneSection(index, values, sampleRate, &coeffs,
                                &tailFrames)) {
            return VN_INVALID_ARG;
        }
    }
    return VN_OK;
}

std::uint32_t ToneParamsFromDesc(const void* desc, float* out) {
    const auto* t = static_cast<const VnThreeBandToneDesc*>(desc);
    out[kToneParamLowShelfGainDb] = t->lowShelfGainDb;
    out[kToneParamLowShelfFreqHz] = t->lowShelfFreqHz;
    out[kToneParamPeakGainDb] = t->peakGainDb;
    out[kToneParamPeakFreqHz] = t->peakFreqHz;
    out[kToneParamPeakQ] = t->peakQ;
    out[kToneParamHighShelfGainDb] = t->highShelfGainDb;
    out[kToneParamHighShelfFreqHz] = t->highShelfFreqHz;
    return kToneParamCount;
}

std::uint32_t ToneExtraFramesPerChannel(const EngineConfig& /*config*/) {
    // The tone effect's whole state fits in ToneEffectState; no extra pool
    // storage per instance.
    return 0;
}

std::uint32_t ToneCapacitySource(const EngineConfig& config) {
    return config.toneEffectPoolCapacity;
}

void ToneInitInstance(void* mem, float* /*extra*/,
                      std::uint32_t /*extraFramesPerChannel*/,
                      std::uint32_t sampleRate) {
    auto* t = new (mem) ToneEffectState{};
    t->sampleRate = sampleRate;
}

// --- gain ---

void GainReset(void* state) {
    auto* g = static_cast<GainState*>(state);
    g->gainLinear = 1.0f; // 0 dB unity
    g->targetGainLinear = 1.0f;
    g->gainIncrement = 0.0f;
    g->transitionFramesRemaining = 0;
    g->hasRendered = false;
}

void GainSetParam(void* state, std::uint32_t paramIndex, float value) {
    auto* g = static_cast<GainState*>(state);
    switch (paramIndex) {
    case kGainParamGainDb:
        // dB -> linear at the block boundary (house convention: 10^(dB/20)), so
        // process() is a single multiply. 0 dB yields exactly 1.0.
        g->gainLinear = std::pow(10.0f, value / 20.0f);
        g->targetGainLinear = g->gainLinear;
        g->gainIncrement = 0.0f;
        g->transitionFramesRemaining = 0;
        break;
    default:
        break; // unknown index: no-op (validated at the API boundary)
    }
}

void GainApplyPreparedParam(void* state, float prepared,
                            std::uint32_t transitionFrames) {
    auto* g = static_cast<GainState*>(state);
    if (!g->hasRendered || transitionFrames == 0) {
        g->gainLinear = prepared;
        g->targetGainLinear = prepared;
        g->gainIncrement = 0.0f;
        g->transitionFramesRemaining = 0;
        return;
    }
    g->targetGainLinear = prepared;
    g->gainIncrement =
        (g->targetGainLinear - g->gainLinear) /
        static_cast<float>(transitionFrames);
    g->transitionFramesRemaining = transitionFrames;
}

void GainProcess(void* state, float* const* channels,
                 std::uint32_t channelCount, std::uint32_t offset,
                 std::uint32_t frames) {
    auto* g = static_cast<GainState*>(state);
    if (g->gainLinear == 1.0f && g->transitionFramesRemaining == 0) {
        // Exact unity (0 dB): bit-transparent neutral fast path. The buffer is
        // left untouched, so the output bytes are identical to a no-effect
        // render — no multiply, no rounding.
        g->hasRendered = g->hasRendered || frames != 0;
        return;
    }
    for (std::uint32_t i = 0; i < frames; ++i) {
        if (g->transitionFramesRemaining != 0) {
            if (g->transitionFramesRemaining == 1) {
                g->gainLinear = g->targetGainLinear;
                g->transitionFramesRemaining = 0;
            } else {
                g->gainLinear += g->gainIncrement;
                --g->transitionFramesRemaining;
            }
        }
        const float gain = g->gainLinear;
        if (gain != 1.0f) {
            for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
                channels[ch][offset + i] *= gain;
            }
        }
    }
    g->hasRendered = g->hasRendered || frames != 0;
}

std::uint32_t GainTail(const void* /*state*/) {
    // Memoryless: the gain has no sample history, so it emits nothing after its
    // input ends regardless of the gain value.
    return 0;
}

VnResult GainValidateParam(std::uint32_t paramIndex, float value,
                           std::uint32_t /*sampleRate*/) {
    // Finiteness is checked once by ValidateEffectParam before dispatch; the
    // gain does not depend on the sample rate.
    switch (paramIndex) {
    case kGainParamGainDb:
        return (value >= kGainMinDb && value <= kGainMaxDb) ? VN_OK
                                                            : VN_INVALID_ARG;
    default:
        return VN_INVALID_ARG;
    }
}

VnResult GainValidateDesc(const void* desc, std::uint32_t sampleRate) {
    const auto* g = static_cast<const VnGainDesc*>(desc);
    if (g->structSize != sizeof(VnGainDesc)) {
        return VN_INVALID_ARG;
    }
    return ValidateEffectParam(VN_EFFECT_GAIN, kGainParamGainDb, g->gainDb,
                               sampleRate);
}

std::uint32_t GainParamsFromDesc(const void* desc, float* out) {
    const auto* g = static_cast<const VnGainDesc*>(desc);
    out[kGainParamGainDb] = g->gainDb;
    return kGainParamCount;
}

std::uint32_t GainExtraFramesPerChannel(const EngineConfig& /*config*/) {
    // The gain's whole state fits in GainState; no extra pool storage per
    // instance.
    return 0;
}

std::uint32_t GainCapacitySource(const EngineConfig& config) {
    return config.gainEffectPoolCapacity;
}

void GainInitInstance(void* mem, float* /*extra*/,
                      std::uint32_t /*extraFramesPerChannel*/,
                      std::uint32_t /*sampleRate*/) {
    new (mem) GainState{};
}

// --- limiter ---

bool PrepareLimiterParam(std::uint32_t paramIndex, float value,
                         std::uint32_t sampleRate, float* outPrepared) {
    if (outPrepared == nullptr || sampleRate == 0) {
        return false;
    }
    switch (paramIndex) {
    case kLimiterParamCeilingDb:
        *outPrepared = std::pow(10.0f, value / 20.0f);
        return true;
    case kLimiterParamReleaseMs: {
        const double releaseSeconds = static_cast<double>(value) * 0.001;
        const double samples =
            releaseSeconds * static_cast<double>(sampleRate);
        *outPrepared =
            static_cast<float>(std::exp(-1.0 / samples));
        return true;
    }
    default:
        return false;
    }
}

void LimiterApplyPreparedParam(void* state, std::uint32_t paramIndex,
                               float prepared) {
    auto* limiter = static_cast<LimiterState*>(state);
    switch (paramIndex) {
    case kLimiterParamCeilingDb:
        limiter->ceilingLinear = prepared;
        limiter->targetCeilingLinear = prepared;
        limiter->ceilingIncrement = 0.0f;
        limiter->transitionFramesRemaining = 0;
        break;
    case kLimiterParamReleaseMs:
        limiter->releaseCoeff = prepared;
        limiter->releaseAdd = 1.0f - prepared;
        limiter->targetReleaseCoeff = prepared;
        limiter->releaseCoeffIncrement = 0.0f;
        limiter->transitionFramesRemaining = 0;
        break;
    default:
        break;
    }
}

void LimiterRequestPreparedParam(void* state, std::uint32_t paramIndex,
                                 float prepared,
                                 std::uint32_t transitionFrames) {
    auto* limiter = static_cast<LimiterState*>(state);
    if (!limiter->hasRendered || transitionFrames == 0) {
        LimiterApplyPreparedParam(state, paramIndex, prepared);
        return;
    }
    if (paramIndex == kLimiterParamCeilingDb) {
        limiter->targetCeilingLinear = prepared;
    } else if (paramIndex == kLimiterParamReleaseMs) {
        limiter->targetReleaseCoeff = prepared;
    } else {
        return;
    }
    const float inverseFrames =
        1.0f / static_cast<float>(transitionFrames);
    limiter->ceilingIncrement =
        (limiter->targetCeilingLinear - limiter->ceilingLinear) *
        inverseFrames;
    limiter->releaseCoeffIncrement =
        (limiter->targetReleaseCoeff - limiter->releaseCoeff) *
        inverseFrames;
    limiter->transitionFramesRemaining = transitionFrames;
}

void LimiterReset(void* state) {
    // Reset only detector history. Parameters survive bypass-to-enable and
    // live reset; pool acquisition reapplies the complete descriptor.
    static_cast<LimiterState*>(state)->currentGain = 1.0f;
}

void LimiterSetParam(void* state, std::uint32_t paramIndex, float value) {
    auto* limiter = static_cast<LimiterState*>(state);
    float prepared = 0.0f;
    if (PrepareLimiterParam(paramIndex, value, limiter->sampleRate, &prepared)) {
        LimiterApplyPreparedParam(state, paramIndex, prepared);
    }
}

void LimiterProcess(void* state, float* const* channels,
                    std::uint32_t channelCount, std::uint32_t offset,
                    std::uint32_t frames) {
    auto* limiter = static_cast<LimiterState*>(state);
    float gain = limiter->currentGain;
    for (std::uint32_t i = 0; i < frames; ++i) {
        if (limiter->transitionFramesRemaining != 0) {
            if (limiter->transitionFramesRemaining == 1) {
                limiter->ceilingLinear =
                    limiter->targetCeilingLinear;
                limiter->releaseCoeff =
                    limiter->targetReleaseCoeff;
                limiter->transitionFramesRemaining = 0;
            } else {
                limiter->ceilingLinear +=
                    limiter->ceilingIncrement;
                limiter->releaseCoeff +=
                    limiter->releaseCoeffIncrement;
                --limiter->transitionFramesRemaining;
            }
            limiter->releaseAdd = 1.0f - limiter->releaseCoeff;
        }
        float linkedPeak = 0.0f;
        for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
            const float sample = channels[ch][offset + i];
            const float magnitude = sample >= 0.0f ? sample : -sample;
            if (magnitude > linkedPeak) {
                linkedPeak = magnitude;
            }
        }

        float nextGain =
            gain < 1.0f
                ? limiter->releaseCoeff * gain + limiter->releaseAdd
                : 1.0f;
        if (nextGain > 1.0f) {
            nextGain = 1.0f;
        }
        if (linkedPeak > limiter->ceilingLinear) {
            float required = limiter->ceilingLinear / linkedPeak;
            // A float divide followed by a multiply can round one ULP above
            // the Ceiling. Correct that rare case with one conservative
            // multiply while retaining one linked gain for every channel.
            if (linkedPeak * required > limiter->ceilingLinear) {
                required *= 0.99999988f;
            }
            if (required < nextGain) {
                nextGain = required;
            }
        }
        gain = nextGain;
        if (gain != 1.0f) {
            for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
                channels[ch][offset + i] *= gain;
            }
        }
    }
    limiter->currentGain = gain;
    limiter->hasRendered = limiter->hasRendered || frames != 0;
}

std::uint32_t LimiterTail(const void* /*state*/) {
    // Release is internal gain-recovery state; no input means no output.
    return 0;
}

VnResult LimiterValidateParam(std::uint32_t paramIndex, float value,
                              std::uint32_t /*sampleRate*/) {
    switch (paramIndex) {
    case kLimiterParamCeilingDb:
        return (value >= kLimiterMinCeilingDb &&
                value <= kLimiterMaxCeilingDb)
                   ? VN_OK
                   : VN_INVALID_ARG;
    case kLimiterParamReleaseMs:
        return (value >= kLimiterMinReleaseMs &&
                value <= kLimiterMaxReleaseMs)
                   ? VN_OK
                   : VN_INVALID_ARG;
    default:
        return VN_INVALID_ARG;
    }
}

VnResult LimiterValidateDesc(const void* desc, std::uint32_t sampleRate) {
    const auto* limiter = static_cast<const VnLimiterDesc*>(desc);
    if (limiter->structSize != sizeof(VnLimiterDesc)) {
        return VN_INVALID_ARG;
    }
    const VnResult ceiling =
        ValidateEffectParam(VN_EFFECT_LIMITER, kLimiterParamCeilingDb,
                            limiter->ceilingDb, sampleRate);
    if (ceiling != VN_OK) {
        return ceiling;
    }
    return ValidateEffectParam(VN_EFFECT_LIMITER, kLimiterParamReleaseMs,
                               limiter->releaseMs, sampleRate);
}

std::uint32_t LimiterParamsFromDesc(const void* desc, float* out) {
    const auto* limiter = static_cast<const VnLimiterDesc*>(desc);
    out[kLimiterParamCeilingDb] = limiter->ceilingDb;
    out[kLimiterParamReleaseMs] = limiter->releaseMs;
    return kLimiterParamCount;
}

std::uint32_t LimiterExtraFramesPerChannel(const EngineConfig& /*config*/) {
    return 0;
}

std::uint32_t LimiterCapacitySource(const EngineConfig& config) {
    return config.limiterEffectPoolCapacity;
}

void LimiterInitInstance(void* mem, float* /*extra*/,
                         std::uint32_t /*extraFramesPerChannel*/,
                         std::uint32_t sampleRate) {
    auto* limiter = new (mem) LimiterState{};
    limiter->sampleRate = sampleRate;
    LimiterSetParam(limiter, kLimiterParamCeilingDb, 0.0f);
    LimiterSetParam(limiter, kLimiterParamReleaseMs,
                    kLimiterDefaultReleaseMs);
}

// --- compressor mathematical kernel ---

bool PrepareCompressorParameters(
    const CompressorParameters& parameters, std::uint32_t sampleRate,
    CompressorPreparedParameters* outPrepared) {
    if (outPrepared == nullptr || sampleRate == 0 ||
        !std::isfinite(parameters.thresholdDb) ||
        !std::isfinite(parameters.ratio) ||
        !std::isfinite(parameters.attackMs) ||
        !std::isfinite(parameters.releaseMs) ||
        !std::isfinite(parameters.kneeDb) ||
        !std::isfinite(parameters.makeupDb) ||
        parameters.thresholdDb < kCompressorMinThresholdDb ||
        parameters.thresholdDb > kCompressorMaxThresholdDb ||
        parameters.ratio < kCompressorMinRatio ||
        parameters.ratio > kCompressorMaxRatio ||
        parameters.attackMs < kCompressorMinAttackMs ||
        parameters.attackMs > kCompressorMaxAttackMs ||
        parameters.releaseMs < kCompressorMinReleaseMs ||
        parameters.releaseMs > kCompressorMaxReleaseMs ||
        parameters.kneeDb < kCompressorMinKneeDb ||
        parameters.kneeDb > kCompressorMaxKneeDb ||
        parameters.makeupDb < kCompressorMinMakeupDb ||
        parameters.makeupDb > kCompressorMaxMakeupDb) {
        return false;
    }

    const auto timeCoeff = [sampleRate](float milliseconds) {
        const double samples =
            static_cast<double>(milliseconds) * 0.001 *
            static_cast<double>(sampleRate);
        return static_cast<float>(std::exp(-1.0 / samples));
    };

    CompressorPreparedParameters prepared{};
    prepared.thresholdDb = parameters.thresholdDb;
    prepared.reciprocalRatio = 1.0f / parameters.ratio;
    prepared.attackCoeff = timeCoeff(parameters.attackMs);
    prepared.attackAdd = 1.0f - prepared.attackCoeff;
    prepared.releaseCoeff = timeCoeff(parameters.releaseMs);
    prepared.releaseAdd = 1.0f - prepared.releaseCoeff;
    prepared.kneeDb = parameters.kneeDb;
    prepared.makeupLinear =
        parameters.makeupDb == 0.0f
            ? 1.0f
            : std::pow(10.0f, parameters.makeupDb / 20.0f);
    const float halfKnee = 0.5f * prepared.kneeDb;
    prepared.lowerKneeLinear =
        std::pow(10.0f,
                 (prepared.thresholdDb - halfKnee) / 20.0f);
    prepared.upperKneeLinear =
        std::pow(10.0f,
                 (prepared.thresholdDb + halfKnee) / 20.0f);
    prepared.hardKneeScale =
        std::pow(10.0f,
                 prepared.thresholdDb *
                     (1.0f - prepared.reciprocalRatio) /
                     20.0f);
    if (!std::isfinite(prepared.lowerKneeLinear) ||
        !std::isfinite(prepared.upperKneeLinear) ||
        !std::isfinite(prepared.hardKneeScale)) {
        return false;
    }
    *outPrepared = prepared;
    return true;
}

void CompressorInit(CompressorState* state, std::uint32_t sampleRate) {
    if (state == nullptr) {
        return;
    }
    *state = CompressorState{};
    state->sampleRate = sampleRate;
    CompressorParameters defaults{};
    state->authoredParameters = defaults;
    CompressorPreparedParameters prepared{};
    if (PrepareCompressorParameters(defaults, sampleRate, &prepared)) {
        CompressorApplyPreparedParameters(state, prepared);
    }
    state->lastAppliedWetGain = state->parameters.makeupLinear;
}

void CompressorApplyPreparedParameters(
    CompressorState* state,
    const CompressorPreparedParameters& prepared) {
    if (state == nullptr) {
        return;
    }
    state->parameters = prepared;
    state->parameters.attackAdd = 1.0f - state->parameters.attackCoeff;
    state->parameters.releaseAdd = 1.0f - state->parameters.releaseCoeff;
    state->targetParameters = state->parameters;
    state->parameterIncrements = CompressorPreparedParameters{};
    state->parameterIncrements.thresholdDb = 0.0f;
    state->parameterIncrements.reciprocalRatio = 0.0f;
    state->parameterIncrements.attackCoeff = 0.0f;
    state->parameterIncrements.attackAdd = 0.0f;
    state->parameterIncrements.releaseCoeff = 0.0f;
    state->parameterIncrements.releaseAdd = 0.0f;
    state->parameterIncrements.kneeDb = 0.0f;
    state->parameterIncrements.makeupLinear = 0.0f;
    state->parameterIncrements.lowerKneeLinear = 0.0f;
    state->parameterIncrements.upperKneeLinear = 0.0f;
    state->parameterIncrements.hardKneeScale = 0.0f;
    state->parameterTransitionFramesRemaining = 0;
}

std::uint32_t CompressorTransitionFrames(std::uint32_t sampleRate) {
    const std::uint64_t rounded =
        (static_cast<std::uint64_t>(sampleRate) * 10u + 500u) / 1000u;
    if (rounded == 0) {
        return 1;
    }
    constexpr std::uint64_t kMax =
        static_cast<std::uint64_t>((std::numeric_limits<std::uint32_t>::max)());
    return static_cast<std::uint32_t>(rounded > kMax ? kMax : rounded);
}

void CompressorRequestPreparedParameters(
    CompressorState* state,
    const CompressorPreparedParameters& prepared,
    std::uint32_t transitionFrames) {
    if (state == nullptr) {
        return;
    }
    if (!state->hasRendered || transitionFrames == 0) {
        CompressorApplyPreparedParameters(state, prepared);
        return;
    }

    state->targetParameters = prepared;
    state->targetParameters.attackAdd =
        1.0f - state->targetParameters.attackCoeff;
    state->targetParameters.releaseAdd =
        1.0f - state->targetParameters.releaseCoeff;
    const float inverseFrames = 1.0f / static_cast<float>(transitionFrames);
    state->parameterIncrements.thresholdDb =
        (state->targetParameters.thresholdDb - state->parameters.thresholdDb) *
        inverseFrames;
    state->parameterIncrements.reciprocalRatio =
        (state->targetParameters.reciprocalRatio -
         state->parameters.reciprocalRatio) *
        inverseFrames;
    state->parameterIncrements.attackCoeff =
        (state->targetParameters.attackCoeff - state->parameters.attackCoeff) *
        inverseFrames;
    state->parameterIncrements.releaseCoeff =
        (state->targetParameters.releaseCoeff -
         state->parameters.releaseCoeff) *
        inverseFrames;
    state->parameterIncrements.kneeDb =
        (state->targetParameters.kneeDb - state->parameters.kneeDb) *
        inverseFrames;
    state->parameterIncrements.makeupLinear =
        (state->targetParameters.makeupLinear -
         state->parameters.makeupLinear) *
        inverseFrames;
    state->parameterIncrements.attackAdd = 0.0f;
    state->parameterIncrements.releaseAdd = 0.0f;
    state->parameterIncrements.lowerKneeLinear = 0.0f;
    state->parameterIncrements.upperKneeLinear = 0.0f;
    state->parameterIncrements.hardKneeScale = 0.0f;
    state->parameterTransitionFramesRemaining = transitionFrames;
}

void CompressorRequestReset(CompressorState* state,
                            std::uint32_t bridgeFrames) {
    if (state == nullptr) {
        return;
    }
    state->resetBridgeStartGain = state->lastAppliedWetGain;
    state->resetBridgeTotalFrames = bridgeFrames == 0 ? 1u : bridgeFrames;
    state->resetBridgeFramesRemaining = state->resetBridgeTotalFrames;
    state->currentGain = 1.0f;
    state->parameters = state->targetParameters;
    state->parameters.attackAdd = 1.0f - state->parameters.attackCoeff;
    state->parameters.releaseAdd = 1.0f - state->parameters.releaseCoeff;
    state->parameterTransitionFramesRemaining = 0;
    state->resetPending = true;
    state->telemetry.resetBridgeActive = true;
}

void CompressorReset(CompressorState* state) {
    if (state == nullptr) {
        return;
    }
    if (!state->hasRendered) {
        state->currentGain = 1.0f;
        state->parameters = state->targetParameters;
        state->parameters.attackAdd =
            1.0f - state->parameters.attackCoeff;
        state->parameters.releaseAdd =
            1.0f - state->parameters.releaseCoeff;
        state->parameterTransitionFramesRemaining = 0;
        state->lastAppliedWetGain = state->parameters.makeupLinear;
        state->resetBridgeStartGain = state->lastAppliedWetGain;
        state->resetBridgeTotalFrames = 0;
        state->resetBridgeFramesRemaining = 0;
        state->resetPending = false;
        state->telemetry = CompressorBlockTelemetry{};
        return;
    }
    CompressorRequestReset(state,
                           CompressorTransitionFrames(state->sampleRate));
}

void CompressorBeginBlock(CompressorState* state) {
    if (state == nullptr) {
        return;
    }
    state->telemetry.mainInputPeak = 0.0f;
    state->telemetry.detectorPeak = 0.0f;
    state->telemetry.outputPeak = 0.0f;
    state->telemetry.maxGainReductionDb = 0.0f;
    state->telemetry.blockMinimumLinearGain = 1.0f;
    state->telemetry.endGainReductionDb =
        state->currentGain < 1.0f
            ? -20.0f * std::log10(state->currentGain)
            : 0.0f;
    state->telemetry.detectorNonFiniteCount = 0;
    state->telemetry.detectorSaturationCount = 0;
    state->telemetry.resetThisBlock = state->resetPending;
    state->resetPending = false;
    state->telemetry.resetBridgeActive = false;
}

static void AdvanceCompressorParameters(CompressorState* state) {
    if (state->parameterTransitionFramesRemaining == 0) {
        return;
    }
    if (state->parameterTransitionFramesRemaining == 1) {
        state->parameters = state->targetParameters;
        state->parameterTransitionFramesRemaining = 0;
    } else {
        state->parameters.thresholdDb +=
            state->parameterIncrements.thresholdDb;
        state->parameters.reciprocalRatio +=
            state->parameterIncrements.reciprocalRatio;
        state->parameters.attackCoeff +=
            state->parameterIncrements.attackCoeff;
        state->parameters.releaseCoeff +=
            state->parameterIncrements.releaseCoeff;
        state->parameters.kneeDb += state->parameterIncrements.kneeDb;
        state->parameters.makeupLinear +=
            state->parameterIncrements.makeupLinear;
        --state->parameterTransitionFramesRemaining;
    }
    state->parameters.attackAdd = 1.0f - state->parameters.attackCoeff;
    state->parameters.releaseAdd = 1.0f - state->parameters.releaseCoeff;
}

static float CompressorLegacyTargetGain(
    const CompressorPreparedParameters& p, float detectorMagnitude) {
    float targetGain = 1.0f;
    if (detectorMagnitude > 0.0f && p.reciprocalRatio < 1.0f) {
        const float inputDb = 20.0f * std::log10(detectorMagnitude);
        const float halfKnee = 0.5f * p.kneeDb;
        float outputDb = inputDb;
        if (p.kneeDb > 0.0f &&
            inputDb >= p.thresholdDb - halfKnee &&
            inputDb <= p.thresholdDb + halfKnee) {
            const float distance = inputDb - p.thresholdDb + halfKnee;
            outputDb += (p.reciprocalRatio - 1.0f) * distance * distance /
                        (2.0f * p.kneeDb);
        } else if (inputDb > p.thresholdDb + halfKnee) {
            outputDb = p.thresholdDb +
                       (inputDb - p.thresholdDb) * p.reciprocalRatio;
        }
        const float reductionDb = outputDb - inputDb;
        if (reductionDb < 0.0f) {
            targetGain = std::pow(10.0f, reductionDb / 20.0f);
        }
    }
    return targetGain;
}

static float CompressorTargetGain(
    const CompressorPreparedParameters& p, float detectorMagnitude,
    bool steadyParameters) {
    if (p.reciprocalRatio >= 1.0f) {
        return 1.0f;
    }
    if (!(detectorMagnitude > 0.0f)) {
        return 1.0f;
    }
    if (!steadyParameters || !std::isfinite(detectorMagnitude)) {
        return CompressorLegacyTargetGain(p, detectorMagnitude);
    }
    if (detectorMagnitude < p.lowerKneeLinear) {
        return 1.0f;
    }
    if (detectorMagnitude > p.upperKneeLinear) {
        const float targetGain =
            p.hardKneeScale *
            std::pow(detectorMagnitude,
                     p.reciprocalRatio - 1.0f);
        return std::clamp(targetGain, 0.0f, 1.0f);
    }
    return CompressorLegacyTargetGain(p, detectorMagnitude);
}

void CompressorProcessWithDetector(
    CompressorState* state, float* const* channels,
    std::uint32_t channelCount, const float* detectorMagnitudes,
    std::uint32_t offset, std::uint32_t frames) {
    if (state == nullptr || channels == nullptr || channelCount == 0) {
        return;
    }

    float gain = state->currentGain;
    for (std::uint32_t i = 0; i < frames; ++i) {
        const bool steadyParameters =
            state->parameterTransitionFramesRemaining == 0;
        AdvanceCompressorParameters(state);
        const CompressorPreparedParameters& p = state->parameters;
        float linkedPeak = 0.0f;
        for (std::uint32_t channel = 0; channel < channelCount; ++channel) {
            const float sample = channels[channel][offset + i];
            const float magnitude = sample >= 0.0f ? sample : -sample;
            if (std::isfinite(magnitude) &&
                magnitude > state->telemetry.mainInputPeak) {
                state->telemetry.mainInputPeak = magnitude;
            }
            if (detectorMagnitudes == nullptr) {
                if (!std::isfinite(magnitude)) {
                    ++state->telemetry.detectorNonFiniteCount;
                } else if (magnitude > linkedPeak) {
                    linkedPeak = magnitude;
                }
            }
        }

        if (detectorMagnitudes != nullptr) {
            const float detector = detectorMagnitudes[offset + i];
            if (std::isfinite(detector)) {
                linkedPeak = detector >= 0.0f ? detector : -detector;
            } else {
                linkedPeak = 0.0f;
                ++state->telemetry.detectorNonFiniteCount;
            }
        }
        if (linkedPeak > state->telemetry.detectorPeak) {
            state->telemetry.detectorPeak = linkedPeak;
        }

        const float targetGain =
            CompressorTargetGain(p, linkedPeak, steadyParameters);

        if (targetGain < gain) {
            gain = p.attackCoeff * gain + p.attackAdd * targetGain;
        } else {
            gain = p.releaseCoeff * gain + p.releaseAdd * targetGain;
        }
        if (gain > 1.0f) {
            gain = 1.0f;
        } else if (gain < 0.0f) {
            gain = 0.0f;
        }
        if (gain < state->telemetry.blockMinimumLinearGain) {
            state->telemetry.blockMinimumLinearGain = gain;
        }

        const float freshWetGain = gain * p.makeupLinear;
        float appliedGain = freshWetGain;
        if (state->resetBridgeFramesRemaining != 0) {
            const std::uint32_t bridgeFrame =
                state->resetBridgeTotalFrames -
                state->resetBridgeFramesRemaining;
            const float weight =
                state->resetBridgeTotalFrames <= 1
                    ? 1.0f
                    : static_cast<float>(bridgeFrame) /
                          static_cast<float>(
                              state->resetBridgeTotalFrames - 1u);
            appliedGain =
                state->resetBridgeStartGain * (1.0f - weight) +
                freshWetGain * weight;
            --state->resetBridgeFramesRemaining;
            state->telemetry.resetBridgeActive = true;
        }
        for (std::uint32_t channel = 0; channel < channelCount; ++channel) {
            float& output = channels[channel][offset + i];
            if (appliedGain != 1.0f) {
                output *= appliedGain;
            }
            const float magnitude = output >= 0.0f ? output : -output;
            if (std::isfinite(magnitude) &&
                magnitude > state->telemetry.outputPeak) {
                state->telemetry.outputPeak = magnitude;
            }
        }
        state->lastAppliedWetGain = appliedGain;
    }
    state->currentGain = gain;
    if (frames != 0) {
        state->telemetry.maxGainReductionDb =
            state->telemetry.blockMinimumLinearGain < 1.0f
                ? -20.0f *
                      std::log10(
                          state->telemetry.blockMinimumLinearGain)
                : 0.0f;
        state->telemetry.endGainReductionDb =
            gain < 1.0f ? -20.0f * std::log10(gain) : 0.0f;
    }
    state->hasRendered = state->hasRendered || frames != 0;
}

void CompressorProcess(CompressorState* state, float* const* channels,
                       std::uint32_t channelCount, std::uint32_t offset,
                       std::uint32_t frames) {
    CompressorProcessWithDetector(state, channels, channelCount, nullptr,
                                  offset, frames);
}

void CompressorSetParam(void* state, std::uint32_t paramIndex, float value) {
    auto* compressor = static_cast<CompressorState*>(state);
    CompressorParameters next = compressor->authoredParameters;
    switch (paramIndex) {
    case kCompressorParamThresholdDb:
        next.thresholdDb = value;
        break;
    case kCompressorParamRatio:
        next.ratio = value;
        break;
    case kCompressorParamAttackMs:
        next.attackMs = value;
        break;
    case kCompressorParamReleaseMs:
        next.releaseMs = value;
        break;
    case kCompressorParamKneeDb:
        next.kneeDb = value;
        break;
    case kCompressorParamMakeupDb:
        next.makeupDb = value;
        break;
    default:
        return;
    }
    CompressorPreparedParameters prepared{};
    if (PrepareCompressorParameters(next, compressor->sampleRate, &prepared)) {
        compressor->authoredParameters = next;
        CompressorApplyPreparedParameters(compressor, prepared);
    }
}

void CompressorProcess(void* state, float* const* channels,
                       std::uint32_t channelCount, std::uint32_t offset,
                       std::uint32_t frames) {
    CompressorProcess(static_cast<CompressorState*>(state), channels,
                      channelCount, offset, frames);
}

void CompressorProcessDynamic(void* state, float* const* channels,
                              std::uint32_t channelCount,
                              std::uint32_t offset, std::uint32_t frames,
                              const EffectDynamicInputView* input) {
    const float* detector =
        input != nullptr &&
                input->signalForm == VN_DYNAMIC_SIGNAL_RAW_AUDIO
            ? input->detectorMagnitude
            : nullptr;
    CompressorProcessWithDetector(static_cast<CompressorState*>(state),
                                  channels, channelCount, detector, offset,
                                  frames);
}

std::uint32_t CompressorTail(const void* /*state*/) {
    return 0;
}

void CompressorReset(void* state) {
    CompressorReset(static_cast<CompressorState*>(state));
}

VnResult CompressorValidateParam(std::uint32_t paramIndex, float value,
                                 std::uint32_t /*sampleRate*/) {
    switch (paramIndex) {
    case kCompressorParamThresholdDb:
        return value >= kCompressorMinThresholdDb &&
                       value <= kCompressorMaxThresholdDb
                   ? VN_OK
                   : VN_INVALID_ARG;
    case kCompressorParamRatio:
        return value >= kCompressorMinRatio && value <= kCompressorMaxRatio
                   ? VN_OK
                   : VN_INVALID_ARG;
    case kCompressorParamAttackMs:
        return value >= kCompressorMinAttackMs &&
                       value <= kCompressorMaxAttackMs
                   ? VN_OK
                   : VN_INVALID_ARG;
    case kCompressorParamReleaseMs:
        return value >= kCompressorMinReleaseMs &&
                       value <= kCompressorMaxReleaseMs
                   ? VN_OK
                   : VN_INVALID_ARG;
    case kCompressorParamKneeDb:
        return value >= kCompressorMinKneeDb && value <= kCompressorMaxKneeDb
                   ? VN_OK
                   : VN_INVALID_ARG;
    case kCompressorParamMakeupDb:
        return value >= kCompressorMinMakeupDb &&
                       value <= kCompressorMaxMakeupDb
                   ? VN_OK
                   : VN_INVALID_ARG;
    default:
        return VN_INVALID_ARG;
    }
}

VnResult CompressorValidateDesc(const void* desc, std::uint32_t sampleRate) {
    const auto* compressor = static_cast<const VnCompressorDesc*>(desc);
    if (compressor->structSize != sizeof(VnCompressorDesc) ||
        compressor->reserved0 != 0) {
        return VN_INVALID_ARG;
    }
    const float values[kCompressorParamCount] = {
        compressor->thresholdDb, compressor->ratio, compressor->attackMs,
        compressor->releaseMs, compressor->kneeDb, compressor->makeupDb};
    for (std::uint32_t i = 0; i < kCompressorParamCount; ++i) {
        const VnResult result =
            ValidateEffectParam(VN_EFFECT_COMPRESSOR, i, values[i], sampleRate);
        if (result != VN_OK) {
            return result;
        }
    }
    return VN_OK;
}

std::uint32_t CompressorParamsFromDesc(const void* desc, float* out) {
    const auto* compressor = static_cast<const VnCompressorDesc*>(desc);
    out[kCompressorParamThresholdDb] = compressor->thresholdDb;
    out[kCompressorParamRatio] = compressor->ratio;
    out[kCompressorParamAttackMs] = compressor->attackMs;
    out[kCompressorParamReleaseMs] = compressor->releaseMs;
    out[kCompressorParamKneeDb] = compressor->kneeDb;
    out[kCompressorParamMakeupDb] = compressor->makeupDb;
    return kCompressorParamCount;
}

std::uint32_t CompressorExtraFramesPerChannel(
    const EngineConfig& /*config*/) {
    return 0;
}

std::uint32_t CompressorCapacitySource(const EngineConfig& config) {
    return config.compressorEffectPoolCapacity;
}

void CompressorInitInstance(void* mem, float* /*extra*/,
                            std::uint32_t /*extraFramesPerChannel*/,
                            std::uint32_t sampleRate) {
    auto* compressor = new (mem) CompressorState{};
    CompressorInit(compressor, sampleRate);
}

// --- registry-driven dispatch ---

InsertEffect BuildEffect(std::uint32_t type, void* state) {
    InsertEffect fx;
    const EffectRegistryRow* row = FindEffectRow(type);
    if (row == nullptr) {
        return fx; // unknown type: empty effect (process == nullptr)
    }
    fx.process = row->process;
    if (type == VN_EFFECT_COMPRESSOR) {
        fx.processDynamic = &CompressorProcessDynamic;
    }
    fx.tailFrames = row->tailFrames;
    fx.setParam = row->setParam;
    fx.reset = row->reset;
    fx.state = state;
    return fx;
}

std::uint32_t EffectParamCount(std::uint32_t type) {
    const EffectRegistryRow* row = FindEffectRow(type);
    return row != nullptr ? row->paramCount : 0;
}

VnResult ValidateEffectParam(std::uint32_t type, std::uint32_t paramIndex,
                             float value, std::uint32_t sampleRate) {
    if (!std::isfinite(value)) {
        return VN_INVALID_ARG;
    }
    const EffectRegistryRow* row = FindEffectRow(type);
    if (row == nullptr) {
        return VN_INVALID_ARG;
    }
    return row->validateParam(paramIndex, value, sampleRate);
}

VnResult ValidateEffectDesc(std::uint32_t type, const void* desc,
                            std::uint32_t sampleRate) {
    if (desc == nullptr) {
        return VN_INVALID_ARG;
    }
    const EffectRegistryRow* row = FindEffectRow(type);
    if (row == nullptr) {
        return VN_INVALID_ARG;
    }
    return row->validateDesc(desc, sampleRate);
}

std::uint32_t EffectParamsFromDesc(std::uint32_t type, const void* desc,
                                   float* out) {
    const EffectRegistryRow* row = FindEffectRow(type);
    return row != nullptr ? row->paramsFromDesc(desc, out) : 0;
}

void ApplyEffectDesc(std::uint32_t type, const void* desc, void* state) {
    // Configure through the logic-side setParam path. The descriptor->index
    // mapping lives once in EffectParamsFromDesc; a contiguous 0..count-1 index
    // space lets the values apply positionally.
    InsertEffect fx = BuildEffect(type, state);
    if (fx.setParam == nullptr) {
        return;
    }
    float params[kMaxEffectParamCount] = {};
    const std::uint32_t count = EffectParamsFromDesc(type, desc, params);
    for (std::uint32_t i = 0; i < count; ++i) {
        fx.setParam(state, i, params[i]);
    }
}

} // namespace harken
