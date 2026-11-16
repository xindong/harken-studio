#ifndef HARKEN_STUDIO_BUILD_EFFECT_ENCODING_H
#define HARKEN_STUDIO_BUILD_EFFECT_ENCODING_H

#include <harken_pack/PackEffects.h>
#include <harken_studio/AudioValues.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace harkenstudiobuild {

struct PackEffectSchemaRow {
    const char* manifestName;
    std::uint32_t effectType;
    std::uint32_t descriptorVintage;
    std::uint32_t paramCount;
    const HkPackEffectParam* params;
};

inline constexpr PackEffectSchemaRow kPackEffectSchema[] = {
    {"feedbackDelay", HK_PACK_EFFECT_FEEDBACK_DELAY, HK_PACK_EFFECT_DESCRIPTOR_VINTAGE, HK_PACK_DELAY_PARAM_COUNT, hkPackDelayParams},
    {"threeBandTone", HK_PACK_EFFECT_THREE_BAND_TONE, HK_PACK_EFFECT_DESCRIPTOR_VINTAGE, HK_PACK_TONE_PARAM_COUNT, hkPackToneParams},
    {"gain", HK_PACK_EFFECT_GAIN, HK_PACK_EFFECT_DESCRIPTOR_VINTAGE, HK_PACK_GAIN_PARAM_COUNT, hkPackGainParams},
    {"limiter", HK_PACK_EFFECT_LIMITER, HK_PACK_EFFECT_DESCRIPTOR_VINTAGE, HK_PACK_LIMITER_PARAM_COUNT, hkPackLimiterParams},
    {"compressor", HK_PACK_EFFECT_COMPRESSOR, HK_PACK_EFFECT_DESCRIPTOR_VINTAGE, HK_PACK_COMPRESSOR_PARAM_COUNT, hkPackCompressorParams},
};

inline constexpr std::uint32_t kPackEffectSchemaRowCount = HK_PACK_EFFECT_TYPE_COUNT;
inline constexpr const char kPackEffectManifestNameList[] =
    "\"feedbackDelay\", \"threeBandTone\", \"gain\", \"limiter\", \"compressor\"";

static_assert(sizeof(kPackEffectSchema) / sizeof(kPackEffectSchema[0]) == kPackEffectSchemaRowCount);
static_assert(harkenstudio::kEffectFeedbackDelay == HK_PACK_EFFECT_FEEDBACK_DELAY);
static_assert(harkenstudio::kEffectThreeBandTone == HK_PACK_EFFECT_THREE_BAND_TONE);
static_assert(harkenstudio::kEffectGain == HK_PACK_EFFECT_GAIN);
static_assert(harkenstudio::kEffectLimiter == HK_PACK_EFFECT_LIMITER);
static_assert(harkenstudio::kEffectCompressor == HK_PACK_EFFECT_COMPRESSOR);

inline const PackEffectSchemaRow* FindPackEffectSchemaByType(std::uint32_t type) {
    for (const PackEffectSchemaRow& row : kPackEffectSchema) {
        if (row.effectType == type) {
            return &row;
        }
    }
    return nullptr;
}

inline const PackEffectSchemaRow* FindPackEffectSchemaByManifestName(const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    for (const PackEffectSchemaRow& row : kPackEffectSchema) {
        if (std::strcmp(row.manifestName, name) == 0) {
            return &row;
        }
    }
    return nullptr;
}

inline bool PackEffectParamInAuthoredRange(std::uint32_t type, std::uint32_t index, float value) {
    if (!std::isfinite(value)) {
        return false;
    }
    const PackEffectSchemaRow* row = FindPackEffectSchemaByType(type);
    if (row == nullptr || index >= row->paramCount) {
        return false;
    }
    const HkPackEffectParam& param = row->params[index];
    if ((param.rangeFlags & HK_PACK_RANGE_HAS_MINIMUM) != 0 &&
        (value < param.minimum ||
         ((param.rangeFlags & HK_PACK_RANGE_MINIMUM_INCLUSIVE) == 0 && value == param.minimum))) {
        return false;
    }
    if ((param.rangeFlags & HK_PACK_RANGE_HAS_MAXIMUM) != 0 &&
        (value > param.maximum ||
         ((param.rangeFlags & HK_PACK_RANGE_MAXIMUM_INCLUSIVE) == 0 && value == param.maximum))) {
        return false;
    }
    return true;
}

}

#endif
