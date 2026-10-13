// Frozen public ABI and audio-thread command surface of the released built-in
// insert effects. These are compile-time
// value pins: an accidental renumber of an effect type id, a reordered or
// resized descriptor field, a shifted per-effect parameter index, or a moved
// command opcode all become build errors here rather than silent wire/ABI
// breaks. The pins mirror the released contract exactly; nothing derives an
// expected value from the thing under test, so a pin only agrees when the
// value has not moved.
#include <harken/Harken.h>

#include "harken/core/CommandQueue.h"
#include "harken/core/EngineConfig.h"
#include "harken/render/BuiltinEffects.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <type_traits>

namespace {

// --- effect type ids (append-only, never renumbered) ---

static_assert(VN_EFFECT_FEEDBACK_DELAY == 0, "effect type id must not renumber");
static_assert(VN_EFFECT_THREE_BAND_TONE == 1,
              "effect type id must not renumber");
static_assert(VN_EFFECT_GAIN == 2, "effect type id must not renumber");
static_assert(VN_EFFECT_LIMITER == 3, "effect type id must not renumber");
static_assert(VN_EFFECT_COMPRESSOR == 4,
              "compressor effect type id is append-only at 4");
static_assert(VN_EFFECT_PARAMETER_UNIT_RATIO == 5,
              "ratio parameter unit is append-only at 5");

// --- refusal code for an exhausted effect pool ---

static_assert(VN_NO_EFFECT == -309, "public result code must keep its value");
static_assert(VN_NO_DYNAMIC_ROUTE == -322 &&
                  VN_DYNAMIC_ROUTE_CYCLE == -323 &&
                  VN_DYNAMIC_ROUTE_ORDER == -324,
              "dynamic-route result codes are append-only");
static_assert(VN_DYNAMIC_SIGNAL_CONTROL == 1 &&
                  VN_DYNAMIC_SIGNAL_RAW_AUDIO == 2 &&
                  VN_DYNAMIC_SOURCE_VARIABLE == 1 &&
                  VN_DYNAMIC_SOURCE_BUS == 2 &&
                  VN_DYNAMIC_SOURCE_VOICE == 4,
              "dynamic signal and source masks are ABI-fixed");
static_assert(VN_DYNAMIC_TAP_BUS_POST_EFFECTS == 0 &&
                  VN_DYNAMIC_TAP_BUS_PRE_FADER == 1 &&
                  VN_DYNAMIC_TAP_BUS_POST_FADER == 2 &&
                  VN_DYNAMIC_TAP_VOICE_POST_GAIN_PRE_SPATIAL == 3,
              "dynamic tap ordinals are ABI-fixed");
static_assert(VN_DYNAMIC_ROUTE_AUTHORED == 1 &&
                  VN_DYNAMIC_ROUTE_ACTIVE == 2 &&
                  VN_DYNAMIC_ROUTE_STALE == 4 &&
                  VN_DYNAMIC_ROUTE_FIRST_BLOCK == 8 &&
                  VN_DYNAMIC_ROUTE_PACK_OWNED == 16 &&
                  VN_DYNAMIC_ROUTE_PENDING == 32 &&
                  VN_DYNAMIC_ROUTE_SOURCE_SILENT == 64,
              "dynamic route state flags are ABI-fixed");
static_assert(VN_DYNAMIC_ROUTE_INACTIVE_NONE == 0 &&
                  VN_DYNAMIC_ROUTE_INACTIVE_SOURCE_STALE == 1 &&
                  VN_DYNAMIC_ROUTE_INACTIVE_FIRST_BLOCK == 2 &&
                  VN_DYNAMIC_ROUTE_INACTIVE_TAP_GENERATION == 3 &&
                  VN_DYNAMIC_ROUTE_INACTIVE_TARGET_STALE == 4 &&
                  VN_DYNAMIC_ROUTE_INACTIVE_PENDING == 5 &&
                  VN_DYNAMIC_ROUTE_INACTIVE_NO_AUDIO_STATUS == 6,
              "dynamic route inactive reasons are ABI-fixed");
static_assert(VN_DYNAMIC_ROUTE_OWNER_CALLER == 0 &&
                  VN_DYNAMIC_ROUTE_OWNER_PACK == 1 &&
                  VN_DYNAMIC_RAW_COMBINE_NONE == 0 &&
                  VN_DYNAMIC_RAW_COMBINE_MAX_MAGNITUDE == 1 &&
                  VN_DYNAMIC_CHANNEL_REDUCTION_NONE == 0 &&
                  VN_DYNAMIC_CHANNEL_REDUCTION_MAX_ABSOLUTE == 1,
              "dynamic route owner and reduction values are ABI-fixed");
static_assert(VN_DYNAMIC_CAP_CONTROL_USES_BINDING == 1 &&
                  VN_DYNAMIC_CAP_RAW_EFFECT_LOCAL == 2 &&
                  VN_DYNAMIC_CAP_PACK_CONTROL == 4 &&
                  VN_DYNAMIC_CAP_PACK_RAW_BUS == 8 &&
                  VN_DYNAMIC_CAP_RAW_VOICE_LIVE_ONLY == 16 &&
                  VN_DYNAMIC_CAP_EFFECT_RESET == 32 &&
                  VN_DYNAMIC_CAP_RAW_STALE_IS_SILENT == 64 &&
                  VN_DYNAMIC_CAP_RAW_ATOMIC_PUBLICATION == 128 &&
                  VN_DYNAMIC_CAP_RAW_REFUSES_CYCLE_ORDER_CAPACITY == 256 &&
                  VN_DYNAMIC_CAP_RAW_RESET_RETAINS_ROUTE == 512,
              "dynamic capability flags are ABI-fixed");
static_assert(VN_DYNAMIC_PARAM_CONTROL_SAFE == 1 &&
                  VN_DYNAMIC_CADENCE_UPDATE == 0 &&
                  VN_DYNAMIC_CADENCE_AUDIO_FRAME == 1 &&
                  VN_DYNAMIC_SMOOTHING_NONE == 0 &&
                  VN_DYNAMIC_SMOOTHING_TARGET == 1 &&
                  VN_DYNAMIC_SMOOTHING_EFFECT_INTERNAL == 2,
              "dynamic parameter, cadence, and smoothing values are ABI-fixed");
static_assert(VN_COMPRESSOR_RUNTIME_BYPASSED == 1 &&
                  VN_COMPRESSOR_RUNTIME_BYPASS_TRANSITION == 2 &&
                  VN_COMPRESSOR_RUNTIME_PARAM_TRANSITION == 4 &&
                  VN_COMPRESSOR_RUNTIME_EXTERNAL_AUTHORED == 8 &&
                  VN_COMPRESSOR_RUNTIME_EXTERNAL_ACTIVE == 16 &&
                  VN_COMPRESSOR_RUNTIME_RESET_THIS_BLOCK == 32 &&
                  VN_COMPRESSOR_RUNTIME_STALE == 64 &&
                  VN_COMPRESSOR_RUNTIME_RESET_BRIDGE == 128,
              "compressor Runtime state flags are ABI-fixed");

// --- feedback-delay descriptor layout ---

static_assert(sizeof(VnFeedbackDelayDesc) == 40,
              "delay descriptor size is ABI-fixed");
static_assert(offsetof(VnFeedbackDelayDesc, structSize) == 0 &&
                  offsetof(VnFeedbackDelayDesc, reserved0) == 4 &&
                  offsetof(VnFeedbackDelayDesc, delayMs) == 24 &&
                  offsetof(VnFeedbackDelayDesc, feedback) == 28 &&
                  offsetof(VnFeedbackDelayDesc, wet) == 32 &&
                  offsetof(VnFeedbackDelayDesc, dry) == 36,
              "delay descriptor field layout pinned");

// --- three-band-tone descriptor layout ---

static_assert(sizeof(VnThreeBandToneDesc) == 56,
              "tone descriptor size is ABI-fixed");
static_assert(offsetof(VnThreeBandToneDesc, structSize) == 0 &&
                  offsetof(VnThreeBandToneDesc, reserved0) == 4 &&
                  offsetof(VnThreeBandToneDesc, lowShelfGainDb) == 24 &&
                  offsetof(VnThreeBandToneDesc, lowShelfFreqHz) == 28 &&
                  offsetof(VnThreeBandToneDesc, peakGainDb) == 32 &&
                  offsetof(VnThreeBandToneDesc, peakFreqHz) == 36 &&
                  offsetof(VnThreeBandToneDesc, peakQ) == 40 &&
                  offsetof(VnThreeBandToneDesc, highShelfGainDb) == 44 &&
                  offsetof(VnThreeBandToneDesc, highShelfFreqHz) == 48,
              "tone descriptor field layout pinned");

// --- foundational-Limiter descriptor layout ---

static_assert(sizeof(VnLimiterDesc) == 32,
              "limiter descriptor size is ABI-fixed");
static_assert(offsetof(VnLimiterDesc, structSize) == 0 &&
                  offsetof(VnLimiterDesc, reserved0) == 4 &&
                  offsetof(VnLimiterDesc, ceilingDb) == 24 &&
                  offsetof(VnLimiterDesc, releaseMs) == 28,
              "limiter descriptor field layout pinned");

// --- Compressor descriptor layout ---

static_assert(sizeof(VnCompressorDesc) == 48,
              "compressor descriptor size is ABI-fixed");
static_assert(offsetof(VnCompressorDesc, structSize) == 0 &&
                  offsetof(VnCompressorDesc, reserved0) == 4 &&
                  offsetof(VnCompressorDesc, thresholdDb) == 24 &&
                  offsetof(VnCompressorDesc, ratio) == 28 &&
                  offsetof(VnCompressorDesc, attackMs) == 32 &&
                  offsetof(VnCompressorDesc, releaseMs) == 36 &&
                  offsetof(VnCompressorDesc, kneeDb) == 40 &&
                  offsetof(VnCompressorDesc, makeupDb) == 44,
              "compressor descriptor field layout pinned");

// --- Dynamic Modulation public layouts ---

static_assert(sizeof(VnEffectDynamicRouteDesc) == 56 &&
                  alignof(VnEffectDynamicRouteDesc) == 8,
              "dynamic-route descriptor shape is ABI-fixed");
static_assert(offsetof(VnEffectDynamicRouteDesc, structSize) == 0 &&
                  offsetof(VnEffectDynamicRouteDesc, reserved0) == 4 &&
                  offsetof(VnEffectDynamicRouteDesc, signalForm) == 8 &&
                  offsetof(VnEffectDynamicRouteDesc, sourceKind) == 12 &&
                  offsetof(VnEffectDynamicRouteDesc, tap) == 16 &&
                  offsetof(VnEffectDynamicRouteDesc, reserved1) == 20 &&
                  offsetof(VnEffectDynamicRouteDesc, sourceId) == 24 &&
                  offsetof(VnEffectDynamicRouteDesc, levelDb) == 48 &&
                  offsetof(VnEffectDynamicRouteDesc, expectedEffectType) == 52,
              "dynamic-route descriptor offsets are ABI-fixed");

static_assert(sizeof(VnEffectDynamicCapability) == 88 &&
                  alignof(VnEffectDynamicCapability) == 4,
              "dynamic effect capability shape is ABI-fixed");
static_assert(offsetof(VnEffectDynamicCapability, structSize) == 0 &&
                  offsetof(VnEffectDynamicCapability, reserved0) == 4 &&
                  offsetof(VnEffectDynamicCapability, effectType) == 8 &&
                  offsetof(VnEffectDynamicCapability,
                           acceptedSignalFormsMask) == 12 &&
                  offsetof(VnEffectDynamicCapability,
                           configuredVoiceTapCapacity) == 44 &&
                  offsetof(VnEffectDynamicCapability, flags) == 56 &&
                  offsetof(VnEffectDynamicCapability,
                           rawRouteLevelMinDb) == 64 &&
                  offsetof(VnEffectDynamicCapability, reserved1) == 84,
              "dynamic effect capability offsets are ABI-fixed");

static_assert(sizeof(VnEffectDynamicParameterCapability) == 40 &&
                  alignof(VnEffectDynamicParameterCapability) == 4,
              "dynamic parameter capability shape is ABI-fixed");
static_assert(
    offsetof(VnEffectDynamicParameterCapability, structSize) == 0 &&
        offsetof(VnEffectDynamicParameterCapability, reserved0) == 4 &&
        offsetof(VnEffectDynamicParameterCapability, effectType) == 8 &&
        offsetof(VnEffectDynamicParameterCapability, parameterIndex) == 12 &&
        offsetof(VnEffectDynamicParameterCapability,
                 acceptedSignalFormsMask) == 16 &&
        offsetof(VnEffectDynamicParameterCapability, sourceKindsMask) == 20 &&
        offsetof(VnEffectDynamicParameterCapability, updateCadence) == 24 &&
        offsetof(VnEffectDynamicParameterCapability, smoothingOwner) == 28 &&
        offsetof(VnEffectDynamicParameterCapability, smoothingFrames) == 32 &&
        offsetof(VnEffectDynamicParameterCapability, flags) == 36,
    "dynamic parameter capability offsets are ABI-fixed");

static_assert(sizeof(VnEffectDynamicRouteInfo) == 112 &&
                  alignof(VnEffectDynamicRouteInfo) == 8,
              "dynamic route capture shape is ABI-fixed");
static_assert(offsetof(VnEffectDynamicRouteInfo, structSize) == 0 &&
                  offsetof(VnEffectDynamicRouteInfo, reserved0) == 4 &&
                  offsetof(VnEffectDynamicRouteInfo, targetBusId) == 8 &&
                  offsetof(VnEffectDynamicRouteInfo, sourceId) == 24 &&
                  offsetof(VnEffectDynamicRouteInfo, revision) == 48 &&
                  offsetof(VnEffectDynamicRouteInfo, blockEpoch) == 56 &&
                  offsetof(VnEffectDynamicRouteInfo, targetEffectType) == 64 &&
                  offsetof(VnEffectDynamicRouteInfo, effectSlot) == 68 &&
                  offsetof(VnEffectDynamicRouteInfo, routeSlot) == 72 &&
                  offsetof(VnEffectDynamicRouteInfo, signalForm) == 76 &&
                  offsetof(VnEffectDynamicRouteInfo, sourceKind) == 80 &&
                  offsetof(VnEffectDynamicRouteInfo, tap) == 84 &&
                  offsetof(VnEffectDynamicRouteInfo, levelDb) == 88 &&
                  offsetof(VnEffectDynamicRouteInfo, stateFlags) == 92 &&
                  offsetof(VnEffectDynamicRouteInfo, inactiveReason) == 96 &&
                  offsetof(VnEffectDynamicRouteInfo, ownerKind) == 100 &&
                  offsetof(VnEffectDynamicRouteInfo, pendingPublication) == 104 &&
                  offsetof(VnEffectDynamicRouteInfo, reserved1) == 108,
              "dynamic route capture offsets are ABI-fixed");

static_assert(sizeof(VnCompressorRuntimeInfo) == 96 &&
                  alignof(VnCompressorRuntimeInfo) == 8,
              "compressor telemetry shape is ABI-fixed");
static_assert(offsetof(VnCompressorRuntimeInfo, structSize) == 0 &&
                  offsetof(VnCompressorRuntimeInfo, reserved0) == 4 &&
                  offsetof(VnCompressorRuntimeInfo, objectId) == 8 &&
                  offsetof(VnCompressorRuntimeInfo, blockEpoch) == 32 &&
                  offsetof(VnCompressorRuntimeInfo,
                           detectorNonFiniteCount) == 40 &&
                  offsetof(VnCompressorRuntimeInfo,
                           detectorSaturationCount) == 48 &&
                  offsetof(VnCompressorRuntimeInfo, hostScope) == 56 &&
                  offsetof(VnCompressorRuntimeInfo, effectSlot) == 60 &&
                  offsetof(VnCompressorRuntimeInfo, stateFlags) == 64 &&
                  offsetof(VnCompressorRuntimeInfo,
                           transitionFramesRemaining) == 68 &&
                  offsetof(VnCompressorRuntimeInfo, staleRouteCount) == 72 &&
                  offsetof(VnCompressorRuntimeInfo, mainInputPeakDb) == 76 &&
                  offsetof(VnCompressorRuntimeInfo, detectorPeakDb) == 80 &&
                  offsetof(VnCompressorRuntimeInfo, outputPeakDb) == 84 &&
                  offsetof(VnCompressorRuntimeInfo,
                           maxGainReductionDb) == 88 &&
                  offsetof(VnCompressorRuntimeInfo,
                           endGainReductionDb) == 92,
              "compressor telemetry offsets are ABI-fixed");

using SetDynamicRouteSignature =
    VnResult (*)(VnBusId, std::uint32_t, std::uint32_t,
                 const VnEffectDynamicRouteDesc*);
using ClearDynamicRouteSignature =
    VnResult (*)(VnBusId, std::uint32_t, std::uint32_t, VnEffectType);
using CaptureDynamicRouteSignature =
    VnResult (*)(VnEffectDynamicRouteInfo*, std::uint32_t, std::uint32_t*);
using ResetVoiceEffectSignature =
    VnResult (*)(VnVoiceHandle, std::uint32_t, VnEffectType);
using ResetBusEffectSignature =
    VnResult (*)(VnBusId, std::uint32_t, VnEffectType);

static_assert(std::is_same_v<decltype(&Vn_SetBusEffectDynamicRoute),
                             SetDynamicRouteSignature> &&
                  std::is_same_v<decltype(&Vn_ClearBusEffectDynamicRoute),
                                 ClearDynamicRouteSignature> &&
                  std::is_same_v<decltype(&Vn_CaptureEffectDynamicRoutes),
                                 CaptureDynamicRouteSignature>,
              "dynamic route public function signatures are ABI-fixed");
static_assert(std::is_same_v<decltype(&Vn_ResetVoiceEffect),
                             ResetVoiceEffectSignature> &&
                  std::is_same_v<decltype(&Vn_ResetBusEffect),
                                 ResetBusEffectSignature>,
              "effect reset public function signatures are ABI-fixed");

// --- per-effect-type private parameter index spaces ---

static_assert(harken::kDelayParamTimeMs == 0 &&
                  harken::kDelayParamFeedback == 1 &&
                  harken::kDelayParamWet == 2 && harken::kDelayParamDry == 3 &&
                  harken::kDelayParamCount == 4,
              "delay parameter indices are wire-fixed");
static_assert(harken::kToneParamLowShelfGainDb == 0 &&
                  harken::kToneParamLowShelfFreqHz == 1 &&
                  harken::kToneParamPeakGainDb == 2 &&
                  harken::kToneParamPeakFreqHz == 3 &&
                  harken::kToneParamPeakQ == 4 &&
                  harken::kToneParamHighShelfGainDb == 5 &&
                  harken::kToneParamHighShelfFreqHz == 6 &&
                  harken::kToneParamCount == 7,
              "tone parameter indices are wire-fixed");
static_assert(harken::kLimiterParamCeilingDb == 0 &&
                  harken::kLimiterParamReleaseMs == 1 &&
                  harken::kLimiterParamCount == 2,
              "limiter parameter indices are wire-fixed");
static_assert(harken::kCompressorParamThresholdDb == 0 &&
                  harken::kCompressorParamRatio == 1 &&
                  harken::kCompressorParamAttackMs == 2 &&
                  harken::kCompressorParamReleaseMs == 3 &&
                  harken::kCompressorParamKneeDb == 4 &&
                  harken::kCompressorParamMakeupDb == 5 &&
                  harken::kCompressorParamCount == 6,
              "compressor parameter indices are wire-fixed");
static_assert(harken::kMaxEffectParamCount == harken::kToneParamCount,
              "widest parameter index space is the tone effect's");

// --- audio-thread command opcodes for the effect surface ---

static_assert(harken::kOpcodeSetVoiceEffectBypass == 11 &&
                  harken::kOpcodeSetVoiceEffect == 12 &&
                  harken::kOpcodeClearVoiceEffect == 13 &&
                  harken::kOpcodeSetVoiceEffectParam == 14,
              "voice-effect opcodes are frozen");
static_assert(harken::kOpcodeSetBusEffect == 20 &&
                  harken::kOpcodeClearBusEffect == 21 &&
                  harken::kOpcodeSetBusEffectParam == 22 &&
                  harken::kOpcodeSetBusEffectBypass == 23 &&
                  harken::kOpcodeSetBusFaderPoint == 24 &&
                  harken::kOpcodeSetBusFilter == 25,
              "bus-effect, fader-point, and bus-filter opcodes are frozen");

static_assert(harken::kCommandVersion == 7,
              "Action Seek transport vintage is pinned");

// Ceiling of the frozen audio-thread vocabulary. No such pin exists in the
// production headers (enforcement there is a frozen enum plus a Debug bounds
// assert); this fixes the count from the test side so a 27th opcode cannot be
// appended without tripping a build error the reviewer must acknowledge.
static_assert(harken::kMaxKnownOpcode == 26,
              "audio-thread opcode ceiling is frozen at 26");

// --- command record transport shape (mirror of the production assert) ---

static_assert(sizeof(harken::CommandRecord) == 64,
              "command records are fixed 64-byte POD across the queue");
static_assert(std::is_trivially_copyable_v<harken::CommandRecord>,
              "command records cross threads by memcpy semantics");

// --- effect-pool default capacities ---
// Runtime EXPECTs rather than static_asserts: these are the internal
// EngineConfig's default-resolved capacities (a zero in the public VnConfig
// resolves to them), the values that size both pools when a caller leaves the
// config fields unset.
TEST(EffectAbiFreeze, DefaultEffectPoolCapacities) {
    const harken::EngineConfig config;
    EXPECT_EQ(8u, config.delayEffectPoolCapacity);
    EXPECT_EQ(16u, config.toneEffectPoolCapacity);
    EXPECT_EQ(16u, config.gainEffectPoolCapacity);
    EXPECT_EQ(16u, config.limiterEffectPoolCapacity);
    EXPECT_EQ(16u, config.compressorEffectPoolCapacity);
}

} // namespace
