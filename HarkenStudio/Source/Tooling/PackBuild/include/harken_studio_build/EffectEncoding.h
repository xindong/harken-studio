#ifndef HARKEN_STUDIO_BUILD_GENERATED_PACK_EFFECT_SCHEMA_G_H
#define HARKEN_STUDIO_BUILD_GENERATED_PACK_EFFECT_SCHEMA_G_H

#include <harken_studio/AudioValues.h>

#include <cmath>
#include <cstdint>
#include <cstring>

namespace harkenstudiobuild {

struct PackEffectAuthoredRange {
    float minimum;
    float maximum;
    bool hasMinimum;
    bool hasMaximum;
    bool minimumInclusive;
    bool maximumInclusive;
};

struct PackEffectSchemaRow {
    const char* manifestName;
    std::uint32_t effectType;
    std::uint32_t descriptorVintage;
    std::uint32_t paramCount;
    const PackEffectAuthoredRange* authoredRanges;
};

inline constexpr PackEffectAuthoredRange kPackDelayAuthoredRanges[] = {
    {0.0f, 1000.0f, true, true, false, true},
    {0.0f, 1.0f, true, true, true, false},
    {0.0f, 16.0f, true, true, true, true},
    {0.0f, 16.0f, true, true, true, true},
};
inline constexpr PackEffectAuthoredRange kPackToneAuthoredRanges[] = {
    {-48.0f, 48.0f, true, true, true, true},
    {0.0f, 0.0f, true, false, false, false},
    {-48.0f, 48.0f, true, true, true, true},
    {0.0f, 0.0f, true, false, false, false},
    {0.0f, 100.0f, true, true, false, true},
    {-48.0f, 48.0f, true, true, true, true},
    {0.0f, 0.0f, true, false, false, false},
};
inline constexpr PackEffectAuthoredRange kPackGainAuthoredRanges[] = {
    {-96.0f, 24.0f, true, true, true, true},
};
inline constexpr PackEffectAuthoredRange kPackLimiterAuthoredRanges[] = {
    {-24.0f, 0.0f, true, true, true, true},
    {1.0f, 1000.0f, true, true, true, true},
};
inline constexpr PackEffectAuthoredRange kPackCompressorAuthoredRanges[] = {
    {-96.0f, 0.0f, true, true, true, true},
    {1.0f, 20.0f, true, true, true, true},
    {0.1f, 200.0f, true, true, true, true},
    {5.0f, 3000.0f, true, true, true, true},
    {0.0f, 24.0f, true, true, true, true},
    {-24.0f, 24.0f, true, true, true, true},
};

inline constexpr std::uint32_t kPackEffectSchemaRowCount = 5u;
inline constexpr const char kPackEffectManifestNameList[] = "\"feedbackDelay\", \"threeBandTone\", \"gain\", \"limiter\", \"compressor\"";
inline constexpr PackEffectSchemaRow
    kPackEffectSchema[kPackEffectSchemaRowCount] = {
        {"feedbackDelay", harkenstudio::kEffectFeedbackDelay, 2u, 4u, kPackDelayAuthoredRanges},
        {"threeBandTone", harkenstudio::kEffectThreeBandTone, 2u, 7u, kPackToneAuthoredRanges},
        {"gain", harkenstudio::kEffectGain, 2u, 1u, kPackGainAuthoredRanges},
        {"limiter", harkenstudio::kEffectLimiter, 2u, 2u, kPackLimiterAuthoredRanges},
        {"compressor", harkenstudio::kEffectCompressor, 2u, 6u, kPackCompressorAuthoredRanges},
};

inline const PackEffectSchemaRow*
FindPackEffectSchemaByType(std::uint32_t type) {
    for (std::uint32_t i = 0; i < kPackEffectSchemaRowCount; ++i) {
        if (kPackEffectSchema[i].effectType == type) {
            return &kPackEffectSchema[i];
        }
    }
    return nullptr;
}

inline const PackEffectSchemaRow*
FindPackEffectSchemaByManifestName(const char* name) {
    if (name == nullptr) {
        return nullptr;
    }
    for (std::uint32_t i = 0; i < kPackEffectSchemaRowCount; ++i) {
        if (std::strcmp(kPackEffectSchema[i].manifestName, name) == 0) {
            return &kPackEffectSchema[i];
        }
    }
    return nullptr;
}

inline bool PackEffectParamInAuthoredRange(
    std::uint32_t type, std::uint32_t index, float value) {
    if (!std::isfinite(value)) {
        return false;
    }
    const PackEffectSchemaRow* row = FindPackEffectSchemaByType(type);
    if (row == nullptr || index >= row->paramCount) {
        return false;
    }
    const PackEffectAuthoredRange& range = row->authoredRanges[index];
    if (range.hasMinimum &&
        (value < range.minimum ||
         (!range.minimumInclusive && value == range.minimum))) {
        return false;
    }
    if (range.hasMaximum &&
        (value > range.maximum ||
         (!range.maximumInclusive && value == range.maximum))) {
        return false;
    }
    return true;
}

static_assert(harkenstudio::kEffectFeedbackDelay == 0,
              "effect-type ABI value drift: feedbackDelay");
static_assert(kPackEffectSchema[0].paramCount == 4u,
              "Pack effect parameter-count drift: feedbackDelay");
static_assert(harkenstudio::kEffectThreeBandTone == 1,
              "effect-type ABI value drift: threeBandTone");
static_assert(kPackEffectSchema[1].paramCount == 7u,
              "Pack effect parameter-count drift: threeBandTone");
static_assert(harkenstudio::kEffectGain == 2,
              "effect-type ABI value drift: gain");
static_assert(kPackEffectSchema[2].paramCount == 1u,
              "Pack effect parameter-count drift: gain");
static_assert(harkenstudio::kEffectLimiter == 3,
              "effect-type ABI value drift: limiter");
static_assert(kPackEffectSchema[3].paramCount == 2u,
              "Pack effect parameter-count drift: limiter");
static_assert(harkenstudio::kEffectCompressor == 4,
              "effect-type ABI value drift: compressor");
static_assert(kPackEffectSchema[4].paramCount == 6u,
              "Pack effect parameter-count drift: compressor");

} // namespace harkenstudiobuild

#endif // HARKEN_STUDIO_BUILD_GENERATED_PACK_EFFECT_SCHEMA_G_H
