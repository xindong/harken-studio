// The two real built-in insert effects (M2 spec section 5): a feedback delay
// (proves a nonzero tail and non-commuting chain order) and a three-band tone
// effect (low shelf / peaking / high shelf — proves parameterization). Each is
// realized against the context-free InsertEffect seam: pure function pointers
// over a pooled POD state, no host knowledge, no heap ownership.
//
// Both run on the audio thread under the section-4 rules. Tone parameter
// updates crossfade between prepared sections over a fixed sample span;
// coefficients are prepared on the logic thread and stored by the audio-side
// apply path.
#ifndef HARKEN_RENDER_BUILTIN_EFFECTS_H
#define HARKEN_RENDER_BUILTIN_EFFECTS_H

#include <harken/Harken.h>

#include "harken/core/EngineConfig.h"
#include "harken/dsp/Biquad.h"
#include "harken/render/InsertEffect.h"

#include <cstdint>

namespace harken {

// Longest delay a feedback-delay instance can address. The per-instance delay
// line is preallocated at Init to hold this many frames per channel (all
// kMaxChannels), so the maximum is an engine constant, not a per-call bound —
// a delay-time parameter beyond it is refused (VN_INVALID_ARG). One second
// covers game echo/slap use without an unreasonable preallocation.
inline constexpr double kDelayEffectMaxSeconds = 1.0;
inline constexpr float kDelayEffectMaxMs =
    static_cast<float>(kDelayEffectMaxSeconds * 1000.0);

// Wet/dry level ceiling for the delay's mix parameters: a generous linear-gain
// bound that still rejects absurd or non-finite values.
inline constexpr float kEffectMaxLevel = 16.0f;

// Tone parameter ceilings: gains in dB, peaking Q dimensionless. These bound
// the RBJ derivation to a well-behaved region; frequencies are bounded by the
// engine's Nyquist at validation time.
inline constexpr float kToneMaxGainDb = 48.0f;
inline constexpr float kToneMaxQ = 100.0f;

// Fixed Q for the two shelving sections (Butterworth, 1/sqrt(2)): a maximally
// flat shelf transition. Only the peaking section exposes Q as a parameter.
inline constexpr float kToneShelfQ = 0.70710678f;

// Gain insert-effect dB bounds: a generous studio range that still rejects
// absurd or non-finite values. 0 dB is exact unity and bit-transparent. This is
// the Gain insert effect (a chain stage), distinct from the fixed Voice Gain and
// the Bus Volume fader.
inline constexpr float kGainMinDb = -96.0f;
inline constexpr float kGainMaxDb = 24.0f;

// Foundational Limiter bounds. Both descriptor ranges are closed.
inline constexpr float kLimiterMinCeilingDb = -24.0f;
inline constexpr float kLimiterMaxCeilingDb = 0.0f;
inline constexpr float kLimiterMinReleaseMs = 1.0f;
inline constexpr float kLimiterMaxReleaseMs = 1000.0f;
inline constexpr float kLimiterDefaultReleaseMs = 100.0f;

// Compressor descriptor and mathematical-kernel bounds.
inline constexpr float kCompressorMinThresholdDb = -96.0f;
inline constexpr float kCompressorMaxThresholdDb = 0.0f;
inline constexpr float kCompressorMinRatio = 1.0f;
inline constexpr float kCompressorMaxRatio = 20.0f;
inline constexpr float kCompressorMinAttackMs = 0.1f;
inline constexpr float kCompressorMaxAttackMs = 200.0f;
inline constexpr float kCompressorMinReleaseMs = 5.0f;
inline constexpr float kCompressorMaxReleaseMs = 3000.0f;
inline constexpr float kCompressorMinKneeDb = 0.0f;
inline constexpr float kCompressorMaxKneeDb = 24.0f;
inline constexpr float kCompressorMinMakeupDb = -24.0f;
inline constexpr float kCompressorMaxMakeupDb = 24.0f;

// Feedback-delay parameter indices (per-effect-type private index space,
// 0-based — never the base spec's global parameter-target IDs).
inline constexpr std::uint32_t kDelayParamTimeMs = 0;   // (0, kDelayEffectMaxMs]
inline constexpr std::uint32_t kDelayParamFeedback = 1; // [0, 1)
inline constexpr std::uint32_t kDelayParamWet = 2;      // [0, kEffectMaxLevel]
inline constexpr std::uint32_t kDelayParamDry = 3;      // [0, kEffectMaxLevel]
inline constexpr std::uint32_t kDelayParamCount = 4;

// Three-band tone parameter indices (per-effect-type private index space).
inline constexpr std::uint32_t kToneParamLowShelfGainDb = 0;
inline constexpr std::uint32_t kToneParamLowShelfFreqHz = 1;
inline constexpr std::uint32_t kToneParamPeakGainDb = 2;
inline constexpr std::uint32_t kToneParamPeakFreqHz = 3;
inline constexpr std::uint32_t kToneParamPeakQ = 4;
inline constexpr std::uint32_t kToneParamHighShelfGainDb = 5;
inline constexpr std::uint32_t kToneParamHighShelfFreqHz = 6;
inline constexpr std::uint32_t kToneParamCount = 7;

// Gain parameter index (per-effect-type private index space). The Gain insert
// effect exposes exactly one parameter, its dB gain.
inline constexpr std::uint32_t kGainParamGainDb = 0; // [kGainMinDb, kGainMaxDb]
inline constexpr std::uint32_t kGainParamCount = 1;

// Limiter parameter indices (per-effect-type private index space).
inline constexpr std::uint32_t kLimiterParamCeilingDb = 0;
inline constexpr std::uint32_t kLimiterParamReleaseMs = 1;
inline constexpr std::uint32_t kLimiterParamCount = 2;

// Compressor parameter indices (per-effect-type private index space).
inline constexpr std::uint32_t kCompressorParamThresholdDb = 0;
inline constexpr std::uint32_t kCompressorParamRatio = 1;
inline constexpr std::uint32_t kCompressorParamAttackMs = 2;
inline constexpr std::uint32_t kCompressorParamReleaseMs = 3;
inline constexpr std::uint32_t kCompressorParamKneeDb = 4;
inline constexpr std::uint32_t kCompressorParamMakeupDb = 5;
inline constexpr std::uint32_t kCompressorParamCount = 6;

// The widest per-effect parameter index space, sizing fixed logic-side mirrors
// of a voice slot's committed effect parameters.
inline constexpr std::uint32_t kMaxEffectParamCount = kToneParamCount;

// Feedback-delay instance state. The delay line (lineBase, lineFrames per
// channel across kMaxChannels) is wired to preallocated pool storage at Init;
// reset()/process() never resize it. writeIndex advances once per frame and is
// shared across channels so all channels stay phase-aligned. The feedback path
// is soft-saturated (see the .cpp), which keeps the loop bounded and makes the
// effect genuinely non-linear, so its chain order relative to the linear tone
// effect is observable.
struct DelayEffectState {
    float* lineBase = nullptr;    // kMaxChannels * lineFrames floats (pool)
    std::uint32_t lineFrames = 0; // capacity per channel, set at Init
    std::uint32_t sampleRate = 0; // for delay-time -> sample conversion
    std::uint32_t delaySamples = 1;
    std::uint32_t writeIndex = 0;
    float feedback = 0.0f;
    float wet = 0.0f;
    float dry = 1.0f;
    float targetWet = 0.0f;
    float targetDry = 1.0f;
    float wetIncrement = 0.0f;
    float dryIncrement = 0.0f;
    std::uint32_t transitionFramesRemaining = 0;
    bool hasRendered = false;
};

// Three-band tone instance state: three RBJ biquads in series (low shelf ->
// peaking -> high shelf) with per-channel filter memory. Parameter values are
// held so a single-band update changes only that band's coefficients.
struct ToneEffectState {
    std::uint32_t sampleRate = 0;
    float lowShelfGainDb = 0.0f;
    float lowShelfFreqHz = 200.0f;
    float peakGainDb = 0.0f;
    float peakFreqHz = 1000.0f;
    float peakQ = 1.0f;
    float highShelfGainDb = 0.0f;
    float highShelfFreqHz = 6000.0f;
    BiquadCoeffs lowShelf;
    BiquadCoeffs peak;
    BiquadCoeffs highShelf;
    std::uint32_t lowShelfTailFrames = 0;
    std::uint32_t peakTailFrames = 0;
    std::uint32_t highShelfTailFrames = 0;
    bool lowShelfEverActive = false;
    bool peakEverActive = false;
    bool highShelfEverActive = false;
    BiquadState lowShelfState[kMaxChannels];
    BiquadState peakState[kMaxChannels];
    BiquadState highShelfState[kMaxChannels];
    BiquadTransitionState<kMaxChannels> lowShelfTransition;
    BiquadTransitionState<kMaxChannels> peakTransition;
    BiquadTransitionState<kMaxChannels> highShelfTransition;
    bool hasRendered = false;
};

// Gain instance state: one precomputed linear multiplier. The dB parameter is
// converted to a linear gain at the block boundary where it applies (setParam),
// so process() is a single multiply per sample, mirroring how the delay
// precomputes delaySamples and the tone precomputes biquad coefficients. 0 dB
// converts to exactly 1.0 (pow(10, 0)), which process() treats as a
// bit-transparent fast path (no multiply). Memoryless — no sample history and
// no extra pool storage — so a single linear value is the whole state; the raw
// dB is not retained because unity is exactly representable in the linear
// domain and reset restores it directly.
struct GainState {
    float gainLinear = 1.0f; // 0 dB unity
    float targetGainLinear = 1.0f;
    float gainIncrement = 0.0f;
    std::uint32_t transitionFramesRemaining = 0;
    bool hasRendered = false;
};

// Limiter state. Parameter-domain transcendental conversions are performed on
// the logic thread and stored as linear values. The audio kernel retains only
// the linked gain-reduction state and advances its release recurrence with
// multiply/add/compare operations.
struct LimiterState {
    std::uint32_t sampleRate = 0;
    float ceilingLinear = 1.0f;
    float releaseCoeff = 0.0f;
    float releaseAdd = 1.0f;
    float currentGain = 1.0f;
    float targetCeilingLinear = 1.0f;
    float targetReleaseCoeff = 0.0f;
    float ceilingIncrement = 0.0f;
    float releaseCoeffIncrement = 0.0f;
    std::uint32_t transitionFramesRemaining = 0;
    bool hasRendered = false;
};

struct CompressorParameters {
    float thresholdDb = -18.0f;
    float ratio = 1.0f;
    float attackMs = 10.0f;
    float releaseMs = 100.0f;
    float kneeDb = 6.0f;
    float makeupDb = 0.0f;
};

// Values prepared away from the audio kernel. This slice intentionally owns
// no external detector, route, tap, or Sidechain ABI shape.
struct CompressorPreparedParameters {
    float thresholdDb = -18.0f;
    float reciprocalRatio = 1.0f;
    float attackCoeff = 0.0f;
    float attackAdd = 1.0f;
    float releaseCoeff = 0.0f;
    float releaseAdd = 1.0f;
    float kneeDb = 6.0f;
    float makeupLinear = 1.0f;
    float lowerKneeLinear = 0.0f;
    float upperKneeLinear = 0.0f;
    float hardKneeScale = 1.0f;
};

struct CompressorBlockTelemetry {
    float mainInputPeak = 0.0f;
    float detectorPeak = 0.0f;
    float outputPeak = 0.0f;
    float maxGainReductionDb = 0.0f;
    float endGainReductionDb = 0.0f;
    float blockMinimumLinearGain = 1.0f;
    std::uint64_t detectorNonFiniteCount = 0;
    std::uint64_t detectorSaturationCount = 0;
    bool resetThisBlock = false;
    bool resetBridgeActive = false;
};

struct CompressorState {
    std::uint32_t sampleRate = 0;
    CompressorParameters authoredParameters{};
    CompressorPreparedParameters parameters{};
    CompressorPreparedParameters targetParameters{};
    CompressorPreparedParameters parameterIncrements{};
    std::uint32_t parameterTransitionFramesRemaining = 0;
    float currentGain = 1.0f;
    float lastAppliedWetGain = 1.0f;
    float resetBridgeStartGain = 1.0f;
    std::uint32_t resetBridgeTotalFrames = 0;
    std::uint32_t resetBridgeFramesRemaining = 0;
    bool hasRendered = false;
    bool resetPending = false;
    CompressorBlockTelemetry telemetry{};
};

// Assembles the InsertEffect POD (function pointers + state) for a type. Runs
// on both sides: the logic side builds a temporary to reset/configure a fresh
// instance, and the audio side builds the one it installs into a chain slot at
// the apply boundary. An unknown type yields an empty effect (process ==
// nullptr).
InsertEffect BuildEffect(std::uint32_t type, void* state);

// Number of parameters a type exposes (its private index space size).
std::uint32_t EffectParamCount(std::uint32_t type);

// Validates one parameter value against its type's range. Non-finite values,
// unknown indices, and out-of-range values are VN_INVALID_ARG. sampleRate
// bounds frequency parameters by Nyquist. Shared by the desc validator and the
// runtime Vn_SetVoiceEffectParam path so both enforce identical ranges.
VnResult ValidateEffectParam(std::uint32_t type, std::uint32_t paramIndex,
                             float value, std::uint32_t sampleRate);

// Validates a type descriptor: the concrete struct size for the type, then
// every parameter it carries. desc is the base pointer the caller passed to
// Vn_SetVoiceEffect (its common initial structSize field is read to check the
// vintage). VN_INVALID_ARG on any failure.
VnResult ValidateEffectDesc(std::uint32_t type, const void* desc,
                            std::uint32_t sampleRate);

// Fills out[0, count) with a descriptor's parameter values in the canonical
// per-type index order and returns count (0 for an unknown type). out must hold
// at least kMaxEffectParamCount floats. The single source of the
// descriptor->parameter-index mapping, shared by ApplyEffectDesc and the
// logic-side per-voice effect-config mirror so they never diverge.
std::uint32_t EffectParamsFromDesc(std::uint32_t type, const void* desc,
                                   float* out);

bool PrepareToneSection(std::uint32_t paramIndex, const float* params,
                        std::uint32_t sampleRate, BiquadCoeffs* outCoeffs,
                        std::uint32_t* outTailFrames);
void ToneApplyPreparedParam(void* state, std::uint32_t paramIndex, float value,
                            const BiquadCoeffs& coeffs,
                            std::uint32_t tailFrames);
bool PrepareLimiterParam(std::uint32_t paramIndex, float value,
                         std::uint32_t sampleRate, float* outPrepared);
void DelayApplyPreparedParam(void* state, std::uint32_t paramIndex,
                             float prepared,
                             std::uint32_t transitionFrames);
void GainApplyPreparedParam(void* state, float prepared,
                            std::uint32_t transitionFrames);
void LimiterApplyPreparedParam(void* state, std::uint32_t paramIndex,
                               float prepared);
void LimiterRequestPreparedParam(void* state, std::uint32_t paramIndex,
                                 float prepared,
                                 std::uint32_t transitionFrames);
bool PrepareCompressorParameters(
    const CompressorParameters& parameters, std::uint32_t sampleRate,
    CompressorPreparedParameters* outPrepared);
void CompressorInit(CompressorState* state, std::uint32_t sampleRate);
void CompressorApplyPreparedParameters(
    CompressorState* state,
    const CompressorPreparedParameters& prepared);
std::uint32_t CompressorTransitionFrames(std::uint32_t sampleRate);
void CompressorRequestPreparedParameters(
    CompressorState* state,
    const CompressorPreparedParameters& prepared,
    std::uint32_t transitionFrames);
void CompressorReset(CompressorState* state);
void CompressorRequestReset(CompressorState* state,
                            std::uint32_t bridgeFrames);
void CompressorBeginBlock(CompressorState* state);
void CompressorProcess(CompressorState* state, float* const* channels,
                       std::uint32_t channelCount, std::uint32_t offset,
                       std::uint32_t frames);
void CompressorProcessWithDetector(
    CompressorState* state, float* const* channels,
    std::uint32_t channelCount, const float* detectorMagnitudes,
    std::uint32_t offset, std::uint32_t frames);

// Applies a validated descriptor to a freshly reset instance (logic side, at
// Set time — construction, not an audio-thread parameter update). Sets every
// parameter through the logic-side setParam path, so a desc-built instance and
// a prepared live update use the same coefficient equations for equal values.
void ApplyEffectDesc(std::uint32_t type, const void* desc, void* state);

} // namespace harken

#endif // HARKEN_RENDER_BUILTIN_EFFECTS_H
