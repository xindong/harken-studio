#ifndef HARKEN_RENDER_EFFECT_REGISTRY_H
#define HARKEN_RENDER_EFFECT_REGISTRY_H

#include <harken/Harken.h>
#include <harken_pack/PackEffects.h>

#include "harken/core/EngineConfig.h"
#include "harken/render/BuiltinEffects.h"
#include "harken/render/InsertEffect.h"

#include <cstddef>
#include <cstdint>

namespace harken {

enum class EffectParamCombine : std::uint32_t {
    AddDb = HK_PACK_EFFECT_COMBINE_ADD_DB,
    Multiply = HK_PACK_EFFECT_COMBINE_MULTIPLY,
    AddLogHz = HK_PACK_EFFECT_COMBINE_ADD_LOG_HZ,
    AddLogQ = HK_PACK_EFFECT_COMBINE_ADD_LOG_Q,
    None = HK_PACK_EFFECT_COMBINE_NONE,
    AddLogTime = HK_PACK_EFFECT_COMBINE_ADD_LOG_TIME,
};

enum class EffectParamUnit : std::uint32_t {
    Db = HK_PACK_EFFECT_UNIT_DB,
    Linear = HK_PACK_EFFECT_UNIT_LINEAR,
    Hz = HK_PACK_EFFECT_UNIT_HZ,
    Q = HK_PACK_EFFECT_UNIT_Q,
    Ms = HK_PACK_EFFECT_UNIT_MS,
    Ratio = HK_PACK_EFFECT_UNIT_RATIO,
};

inline constexpr float kEffectParamClampNyquist = -1.0f;

inline constexpr float kEffectParamClampStrictlyPositive = -1.0e30f;

inline constexpr float kEffectParamClampStrictlyBelowOne = -1.0e29f;

static_assert(kEffectParamClampNyquist !=
                  kEffectParamClampStrictlyPositive &&
              kEffectParamClampNyquist !=
                  kEffectParamClampStrictlyBelowOne &&
              kEffectParamClampStrictlyPositive !=
                  kEffectParamClampStrictlyBelowOne);

struct EffectParamMeta {
    const char* name;
    EffectParamUnit unit;
    EffectParamCombine combine;
    float neutral;
    float clampMin;
    float clampMax;
    bool supportsTransition;
    bool dynamicControlSafe;
};

inline constexpr EffectParamMeta MakeEffectParamMeta(const char* name, const HkPackEffectParam& param,
                                                     float clampMin, float clampMax) {
    return {name, static_cast<EffectParamUnit>(param.unit), static_cast<EffectParamCombine>(param.combine),
            param.neutral, clampMin, clampMax, (param.paramFlags & HK_PACK_EFFECT_PARAM_TRANSITION) != 0,
            (param.paramFlags & HK_PACK_EFFECT_PARAM_DYNAMIC_CONTROL) != 0};
}

struct EffectRegistryRow {
    std::uint32_t effectType;
    std::uint32_t paramCount;
    std::uint32_t descriptorSize;
    std::uint32_t descriptorVintage;
    std::uint32_t hostScopeMask;
    std::uint32_t stateSize;
    std::uint32_t stateAlign;
    std::uint32_t dynamicSignalFormsMask;
    std::uint32_t dynamicSourceKindsMask;
    std::uint32_t dynamicBusTapsMask;
    std::uint32_t dynamicVoiceTapsMask;
    std::uint32_t dynamicMaxRawRoutes;
    std::uint32_t dynamicRawBusLatencyBlocks;
    std::uint32_t dynamicRawVoiceLatencyBlocks;
    std::uint32_t dynamicFlags;
    float dynamicRawRouteLevelMinDb;
    float dynamicRawRouteLevelMaxDb;
    float dynamicRawRouteLevelDefaultDb;
    std::uint32_t dynamicRawRouteCombine;
    std::uint32_t dynamicRawChannelReduction;

    std::uint32_t (*extraFramesPerChannel)(const EngineConfig& config);

    std::uint32_t (*capacity)(const EngineConfig& config);

    void (*initInstance)(void* state, float* extra,
                         std::uint32_t extraFramesPerChannel,
                         std::uint32_t sampleRate);

    void (*process)(void* state, float* const* channels,
                    std::uint32_t channelCount, std::uint32_t offset,
                    std::uint32_t frames);
    std::uint32_t (*tailFrames)(const void* state);
    void (*setParam)(void* state, std::uint32_t paramIndex, float value);
    void (*reset)(void* state);

    VnResult (*validateDesc)(const void* desc, std::uint32_t sampleRate);
    VnResult (*validateParam)(std::uint32_t paramIndex, float value,
                              std::uint32_t sampleRate);
    std::uint32_t (*paramsFromDesc)(const void* desc, float* out);

    const char* label;
    const char* capacitySource;

    const EffectParamMeta* params;
};

std::uint32_t DelayExtraFramesPerChannel(const EngineConfig& config);
std::uint32_t DelayCapacitySource(const EngineConfig& config);
void DelayInitInstance(void* state, float* extra, std::uint32_t extraFramesPerChannel, std::uint32_t sampleRate);
void DelayProcess(void* state, float* const* channels, std::uint32_t channelCount, std::uint32_t offset, std::uint32_t frames);
std::uint32_t DelayTail(const void* state);
void DelaySetParam(void* state, std::uint32_t paramIndex, float value);
void DelayReset(void* state);
VnResult DelayValidateDesc(const void* desc, std::uint32_t sampleRate);
VnResult DelayValidateParam(std::uint32_t paramIndex, float value, std::uint32_t sampleRate);
std::uint32_t DelayParamsFromDesc(const void* desc, float* out);
std::uint32_t ToneExtraFramesPerChannel(const EngineConfig& config);
std::uint32_t ToneCapacitySource(const EngineConfig& config);
void ToneInitInstance(void* state, float* extra, std::uint32_t extraFramesPerChannel, std::uint32_t sampleRate);
void ToneProcess(void* state, float* const* channels, std::uint32_t channelCount, std::uint32_t offset, std::uint32_t frames);
std::uint32_t ToneTail(const void* state);
void ToneSetParam(void* state, std::uint32_t paramIndex, float value);
void ToneReset(void* state);
VnResult ToneValidateDesc(const void* desc, std::uint32_t sampleRate);
VnResult ToneValidateParam(std::uint32_t paramIndex, float value, std::uint32_t sampleRate);
std::uint32_t ToneParamsFromDesc(const void* desc, float* out);
std::uint32_t GainExtraFramesPerChannel(const EngineConfig& config);
std::uint32_t GainCapacitySource(const EngineConfig& config);
void GainInitInstance(void* state, float* extra, std::uint32_t extraFramesPerChannel, std::uint32_t sampleRate);
void GainProcess(void* state, float* const* channels, std::uint32_t channelCount, std::uint32_t offset, std::uint32_t frames);
std::uint32_t GainTail(const void* state);
void GainSetParam(void* state, std::uint32_t paramIndex, float value);
void GainReset(void* state);
VnResult GainValidateDesc(const void* desc, std::uint32_t sampleRate);
VnResult GainValidateParam(std::uint32_t paramIndex, float value, std::uint32_t sampleRate);
std::uint32_t GainParamsFromDesc(const void* desc, float* out);
std::uint32_t LimiterExtraFramesPerChannel(const EngineConfig& config);
std::uint32_t LimiterCapacitySource(const EngineConfig& config);
void LimiterInitInstance(void* state, float* extra, std::uint32_t extraFramesPerChannel, std::uint32_t sampleRate);
void LimiterProcess(void* state, float* const* channels, std::uint32_t channelCount, std::uint32_t offset, std::uint32_t frames);
std::uint32_t LimiterTail(const void* state);
void LimiterSetParam(void* state, std::uint32_t paramIndex, float value);
void LimiterReset(void* state);
VnResult LimiterValidateDesc(const void* desc, std::uint32_t sampleRate);
VnResult LimiterValidateParam(std::uint32_t paramIndex, float value, std::uint32_t sampleRate);
std::uint32_t LimiterParamsFromDesc(const void* desc, float* out);
std::uint32_t CompressorExtraFramesPerChannel(const EngineConfig& config);
std::uint32_t CompressorCapacitySource(const EngineConfig& config);
void CompressorInitInstance(void* state, float* extra, std::uint32_t extraFramesPerChannel, std::uint32_t sampleRate);
void CompressorProcess(void* state, float* const* channels, std::uint32_t channelCount, std::uint32_t offset, std::uint32_t frames);
std::uint32_t CompressorTail(const void* state);
void CompressorSetParam(void* state, std::uint32_t paramIndex, float value);
void CompressorReset(void* state);
VnResult CompressorValidateDesc(const void* desc, std::uint32_t sampleRate);
VnResult CompressorValidateParam(std::uint32_t paramIndex, float value, std::uint32_t sampleRate);
std::uint32_t CompressorParamsFromDesc(const void* desc, float* out);

inline constexpr EffectParamMeta kDelayParamMeta[] = {
    MakeEffectParamMeta("delayMs", hkPackDelayParams[0], kEffectParamClampStrictlyPositive, 1000.0f),
    MakeEffectParamMeta("feedback", hkPackDelayParams[1], 0.0f, kEffectParamClampStrictlyBelowOne),
    MakeEffectParamMeta("wet", hkPackDelayParams[2], 0.0f, 16.0f),
    MakeEffectParamMeta("dry", hkPackDelayParams[3], 0.0f, 16.0f),
};
inline constexpr EffectParamMeta kToneParamMeta[] = {
    MakeEffectParamMeta("lowShelfGainDb", hkPackToneParams[0], -48.0f, 48.0f),
    MakeEffectParamMeta("lowShelfFreqHz", hkPackToneParams[1], kEffectParamClampStrictlyPositive, kEffectParamClampNyquist),
    MakeEffectParamMeta("peakGainDb", hkPackToneParams[2], -48.0f, 48.0f),
    MakeEffectParamMeta("peakFreqHz", hkPackToneParams[3], kEffectParamClampStrictlyPositive, kEffectParamClampNyquist),
    MakeEffectParamMeta("peakQ", hkPackToneParams[4], kEffectParamClampStrictlyPositive, 100.0f),
    MakeEffectParamMeta("highShelfGainDb", hkPackToneParams[5], -48.0f, 48.0f),
    MakeEffectParamMeta("highShelfFreqHz", hkPackToneParams[6], kEffectParamClampStrictlyPositive, kEffectParamClampNyquist),
};
inline constexpr EffectParamMeta kGainParamMeta[] = {
    MakeEffectParamMeta("gainDb", hkPackGainParams[0], -96.0f, 24.0f),
};
inline constexpr EffectParamMeta kLimiterParamMeta[] = {
    MakeEffectParamMeta("ceilingDb", hkPackLimiterParams[0], -24.0f, 0.0f),
    MakeEffectParamMeta("releaseMs", hkPackLimiterParams[1], 1.0f, 1000.0f),
};
inline constexpr EffectParamMeta kCompressorParamMeta[] = {
    MakeEffectParamMeta("thresholdDb", hkPackCompressorParams[0], -96.0f, 0.0f),
    MakeEffectParamMeta("ratio", hkPackCompressorParams[1], 1.0f, 20.0f),
    MakeEffectParamMeta("attackMs", hkPackCompressorParams[2], 0.1f, 200.0f),
    MakeEffectParamMeta("releaseMs", hkPackCompressorParams[3], 5.0f, 3000.0f),
    MakeEffectParamMeta("kneeDb", hkPackCompressorParams[4], 0.0f, 24.0f),
    MakeEffectParamMeta("makeupDb", hkPackCompressorParams[5], -24.0f, 24.0f),
};

inline constexpr std::uint32_t kEffectRegistryRowCount = 5;

inline constexpr EffectRegistryRow kEffectRegistry[kEffectRegistryRowCount] = {
    {
        VN_EFFECT_FEEDBACK_DELAY,
        kDelayParamCount,
        static_cast<std::uint32_t>(sizeof(VnFeedbackDelayDesc)),
        HK_PACK_EFFECT_DESCRIPTOR_VINTAGE,
        VN_EFFECT_HOST_SCOPE_VOICE | VN_EFFECT_HOST_SCOPE_BUS,
        static_cast<std::uint32_t>(sizeof(DelayEffectState)),
        static_cast<std::uint32_t>(alignof(DelayEffectState)),
        VN_DYNAMIC_SIGNAL_CONTROL,
        VN_DYNAMIC_SOURCE_VARIABLE,
        0u,
        0u,
        0u,
        0u,
        0u,
        VN_DYNAMIC_CAP_CONTROL_USES_BINDING | VN_DYNAMIC_CAP_PACK_CONTROL,
        0.0f,
        0.0f,
        0.0f,
        0u,
        0u,
        &DelayExtraFramesPerChannel,
        &DelayCapacitySource,
        &DelayInitInstance,
        &DelayProcess,
        &DelayTail,
        &DelaySetParam,
        &DelayReset,
        &DelayValidateDesc,
        &DelayValidateParam,
        &DelayParamsFromDesc,
        "feedback-delay",
        "delayEffectPoolCapacity",
        kDelayParamMeta,
    },
    {
        VN_EFFECT_THREE_BAND_TONE,
        kToneParamCount,
        static_cast<std::uint32_t>(sizeof(VnThreeBandToneDesc)),
        HK_PACK_EFFECT_DESCRIPTOR_VINTAGE,
        VN_EFFECT_HOST_SCOPE_VOICE | VN_EFFECT_HOST_SCOPE_BUS,
        static_cast<std::uint32_t>(sizeof(ToneEffectState)),
        static_cast<std::uint32_t>(alignof(ToneEffectState)),
        VN_DYNAMIC_SIGNAL_CONTROL,
        VN_DYNAMIC_SOURCE_VARIABLE,
        0u,
        0u,
        0u,
        0u,
        0u,
        VN_DYNAMIC_CAP_CONTROL_USES_BINDING | VN_DYNAMIC_CAP_PACK_CONTROL,
        0.0f,
        0.0f,
        0.0f,
        0u,
        0u,
        &ToneExtraFramesPerChannel,
        &ToneCapacitySource,
        &ToneInitInstance,
        &ToneProcess,
        &ToneTail,
        &ToneSetParam,
        &ToneReset,
        &ToneValidateDesc,
        &ToneValidateParam,
        &ToneParamsFromDesc,
        "three-band-tone",
        "toneEffectPoolCapacity",
        kToneParamMeta,
    },
    {
        VN_EFFECT_GAIN,
        kGainParamCount,
        static_cast<std::uint32_t>(sizeof(VnGainDesc)),
        HK_PACK_EFFECT_DESCRIPTOR_VINTAGE,
        VN_EFFECT_HOST_SCOPE_VOICE | VN_EFFECT_HOST_SCOPE_BUS,
        static_cast<std::uint32_t>(sizeof(GainState)),
        static_cast<std::uint32_t>(alignof(GainState)),
        VN_DYNAMIC_SIGNAL_CONTROL,
        VN_DYNAMIC_SOURCE_VARIABLE,
        0u,
        0u,
        0u,
        0u,
        0u,
        VN_DYNAMIC_CAP_CONTROL_USES_BINDING | VN_DYNAMIC_CAP_PACK_CONTROL,
        0.0f,
        0.0f,
        0.0f,
        0u,
        0u,
        &GainExtraFramesPerChannel,
        &GainCapacitySource,
        &GainInitInstance,
        &GainProcess,
        &GainTail,
        &GainSetParam,
        &GainReset,
        &GainValidateDesc,
        &GainValidateParam,
        &GainParamsFromDesc,
        "gain-insert",
        "gainEffectPoolCapacity",
        kGainParamMeta,
    },
    {
        VN_EFFECT_LIMITER,
        kLimiterParamCount,
        static_cast<std::uint32_t>(sizeof(VnLimiterDesc)),
        HK_PACK_EFFECT_DESCRIPTOR_VINTAGE,
        VN_EFFECT_HOST_SCOPE_VOICE | VN_EFFECT_HOST_SCOPE_BUS,
        static_cast<std::uint32_t>(sizeof(LimiterState)),
        static_cast<std::uint32_t>(alignof(LimiterState)),
        VN_DYNAMIC_SIGNAL_CONTROL,
        VN_DYNAMIC_SOURCE_VARIABLE,
        0u,
        0u,
        0u,
        0u,
        0u,
        VN_DYNAMIC_CAP_CONTROL_USES_BINDING | VN_DYNAMIC_CAP_PACK_CONTROL,
        0.0f,
        0.0f,
        0.0f,
        0u,
        0u,
        &LimiterExtraFramesPerChannel,
        &LimiterCapacitySource,
        &LimiterInitInstance,
        &LimiterProcess,
        &LimiterTail,
        &LimiterSetParam,
        &LimiterReset,
        &LimiterValidateDesc,
        &LimiterValidateParam,
        &LimiterParamsFromDesc,
        "limiter",
        "limiterEffectPoolCapacity",
        kLimiterParamMeta,
    },
    {
        VN_EFFECT_COMPRESSOR,
        kCompressorParamCount,
        static_cast<std::uint32_t>(sizeof(VnCompressorDesc)),
        HK_PACK_EFFECT_DESCRIPTOR_VINTAGE,
        VN_EFFECT_HOST_SCOPE_VOICE | VN_EFFECT_HOST_SCOPE_BUS,
        static_cast<std::uint32_t>(sizeof(CompressorState)),
        static_cast<std::uint32_t>(alignof(CompressorState)),
        VN_DYNAMIC_SIGNAL_CONTROL | VN_DYNAMIC_SIGNAL_RAW_AUDIO,
        VN_DYNAMIC_SOURCE_VARIABLE | VN_DYNAMIC_SOURCE_BUS | VN_DYNAMIC_SOURCE_VOICE,
        1u << VN_DYNAMIC_TAP_BUS_POST_EFFECTS | 1u << VN_DYNAMIC_TAP_BUS_PRE_FADER | 1u << VN_DYNAMIC_TAP_BUS_POST_FADER,
        1u << VN_DYNAMIC_TAP_VOICE_POST_GAIN_PRE_SPATIAL,
        4u,
        0u,
        1u,
        VN_DYNAMIC_CAP_CONTROL_USES_BINDING | VN_DYNAMIC_CAP_RAW_EFFECT_LOCAL | VN_DYNAMIC_CAP_PACK_CONTROL | VN_DYNAMIC_CAP_PACK_RAW_BUS | VN_DYNAMIC_CAP_RAW_VOICE_LIVE_ONLY | VN_DYNAMIC_CAP_EFFECT_RESET | VN_DYNAMIC_CAP_RAW_STALE_IS_SILENT | VN_DYNAMIC_CAP_RAW_ATOMIC_PUBLICATION | VN_DYNAMIC_CAP_RAW_REFUSES_CYCLE_ORDER_CAPACITY | VN_DYNAMIC_CAP_RAW_RESET_RETAINS_ROUTE,
        -96.0f,
        24.0f,
        0.0f,
        1u,
        1u,
        &CompressorExtraFramesPerChannel,
        &CompressorCapacitySource,
        &CompressorInitInstance,
        &CompressorProcess,
        &CompressorTail,
        &CompressorSetParam,
        &CompressorReset,
        &CompressorValidateDesc,
        &CompressorValidateParam,
        &CompressorParamsFromDesc,
        "compressor",
        "compressorEffectPoolCapacity",
        kCompressorParamMeta,
    },
};

inline const EffectRegistryRow* FindEffectRow(std::uint32_t type) {
    for (std::uint32_t i = 0; i < kEffectRegistryRowCount; ++i) {
        if (kEffectRegistry[i].effectType == type) {
            return &kEffectRegistry[i];
        }
    }
    return nullptr;
}

inline const EffectParamMeta* FindEffectParamMeta(std::uint32_t type,
                                                  std::uint32_t paramIndex) {
    const EffectRegistryRow* row = FindEffectRow(type);
    if (row == nullptr || paramIndex >= row->paramCount) {
        return nullptr;
    }
    return &row->params[paramIndex];
}

static_assert(kEffectRegistryRowCount == HK_PACK_EFFECT_TYPE_COUNT);
inline constexpr std::uint32_t kMaxEffectTypeStats = 16;
static_assert(kEffectRegistryRowCount <= kMaxEffectTypeStats);
static_assert(VN_EFFECT_FEEDBACK_DELAY == HK_PACK_EFFECT_FEEDBACK_DELAY);
static_assert(kEffectRegistry[0].effectType == 0);
static_assert(kEffectRegistry[0].paramCount == kDelayParamCount);
static_assert(alignof(DelayEffectState) <= alignof(std::max_align_t));
static_assert(sizeof(kDelayParamMeta) / sizeof(EffectParamMeta) == kDelayParamCount);
static_assert(VN_EFFECT_THREE_BAND_TONE == HK_PACK_EFFECT_THREE_BAND_TONE);
static_assert(kEffectRegistry[1].effectType == 1);
static_assert(kEffectRegistry[1].paramCount == kToneParamCount);
static_assert(alignof(ToneEffectState) <= alignof(std::max_align_t));
static_assert(sizeof(kToneParamMeta) / sizeof(EffectParamMeta) == kToneParamCount);
static_assert(VN_EFFECT_GAIN == HK_PACK_EFFECT_GAIN);
static_assert(kEffectRegistry[2].effectType == 2);
static_assert(kEffectRegistry[2].paramCount == kGainParamCount);
static_assert(alignof(GainState) <= alignof(std::max_align_t));
static_assert(sizeof(kGainParamMeta) / sizeof(EffectParamMeta) == kGainParamCount);
static_assert(VN_EFFECT_LIMITER == HK_PACK_EFFECT_LIMITER);
static_assert(kEffectRegistry[3].effectType == 3);
static_assert(kEffectRegistry[3].paramCount == kLimiterParamCount);
static_assert(alignof(LimiterState) <= alignof(std::max_align_t));
static_assert(sizeof(kLimiterParamMeta) / sizeof(EffectParamMeta) == kLimiterParamCount);
static_assert(VN_EFFECT_COMPRESSOR == HK_PACK_EFFECT_COMPRESSOR);
static_assert(kEffectRegistry[4].effectType == 4);
static_assert(kEffectRegistry[4].paramCount == kCompressorParamCount);
static_assert(alignof(CompressorState) <= alignof(std::max_align_t));
static_assert(sizeof(kCompressorParamMeta) / sizeof(EffectParamMeta) == kCompressorParamCount);
static_assert(kDelayParamCount == HK_PACK_DELAY_PARAM_COUNT);
static_assert(kToneParamCount == HK_PACK_TONE_PARAM_COUNT);
static_assert(kGainParamCount == HK_PACK_GAIN_PARAM_COUNT);
static_assert(kLimiterParamCount == HK_PACK_LIMITER_PARAM_COUNT);
static_assert(kCompressorParamCount == HK_PACK_COMPRESSOR_PARAM_COUNT);

}

#endif
