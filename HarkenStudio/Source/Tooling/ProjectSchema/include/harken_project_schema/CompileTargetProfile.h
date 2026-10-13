#ifndef HARKEN_PROJECT_SCHEMA_COMPILE_TARGET_PROFILE_H
#define HARKEN_PROJECT_SCHEMA_COMPILE_TARGET_PROFILE_H

#include <harken_studio/AudioValues.h>

#include <cstdint>
#include <memory>

namespace harkenprojectschema {

struct CompileTargetProfileV1
{
    std::uint32_t vintage = 1;
    std::uint32_t sampleRate = 48000;
    harkenstudio::ChannelLayout mixLayout = harkenstudio::kChannelLayoutStereo;
    harkenstudio::ChannelLayout deviceLayout = harkenstudio::kChannelLayoutStereo;
    std::uint64_t capabilityMask = 0;
    std::uint64_t fingerprint = 0;
};

inline constexpr std::uint64_t kKnownCapabilityMaskV1 =
    UINT64_C(0x0000001F007F03FF);

inline constexpr std::uint64_t kTargetCapabilityVariableDefinitionsV1 = UINT64_C(1) << 0;
inline constexpr std::uint64_t kTargetCapabilityPlayablePropertiesV1 = UINT64_C(1) << 1;
inline constexpr std::uint64_t kTargetCapabilityRandomDrawV1 = UINT64_C(1) << 2;
inline constexpr std::uint64_t kTargetCapabilityRandomRotationV1 = UINT64_C(1) << 3;
inline constexpr std::uint64_t kTargetCapabilitySequenceV1 = UINT64_C(1) << 4;
inline constexpr std::uint64_t kTargetCapabilityRandomQueueV1 = UINT64_C(1) << 5;
inline constexpr std::uint64_t kTargetCapabilitySequenceQueueV1 = UINT64_C(1) << 6;
inline constexpr std::uint64_t kTargetCapabilityVariableControlV1 = UINT64_C(1) << 7;
inline constexpr std::uint64_t kTargetCapabilityTransitionV1 = UINT64_C(1) << 8;
inline constexpr std::uint64_t kTargetCapabilityActionDefinitionsV1 = UINT64_C(1) << 9;
inline constexpr std::uint64_t kTargetCapabilityBusTopologyV1 = UINT64_C(1) << 16;
inline constexpr std::uint64_t kTargetCapabilityBusSendsV1 = UINT64_C(1) << 17;
inline constexpr std::uint64_t kTargetCapabilityBusFaderV1 = UINT64_C(1) << 18;
inline constexpr std::uint64_t kTargetCapabilityBusEffectsV1 = UINT64_C(1) << 19;
inline constexpr std::uint64_t kTargetCapabilityBusFaderPointV1 = UINT64_C(1) << 20;
inline constexpr std::uint64_t kTargetCapabilityBusOutputLayoutV1 = UINT64_C(1) << 21;
inline constexpr std::uint64_t kTargetCapabilityBusDownmixV1 = UINT64_C(1) << 22;
inline constexpr std::uint64_t kTargetCapabilityFeedbackDelayV1 = UINT64_C(1) << 32;
inline constexpr std::uint64_t kTargetCapabilityThreeBandToneV1 = UINT64_C(1) << 33;
inline constexpr std::uint64_t kTargetCapabilityGainV1 = UINT64_C(1) << 34;
inline constexpr std::uint64_t kTargetCapabilityLimiterV1 = UINT64_C(1) << 35;
inline constexpr std::uint64_t kTargetCapabilityCompressorV1 = UINT64_C(1) << 36;

std::uint64_t compileTargetProfileFingerprintV1(
    const CompileTargetProfileV1& profile) noexcept;

std::shared_ptr<const CompileTargetProfileV1> MakeCompileTargetProfileV1(
    std::uint32_t vintage,
    std::uint32_t sampleRate,
    harkenstudio::ChannelLayout mixLayout,
    harkenstudio::ChannelLayout deviceLayout,
    std::uint64_t capabilityMask) noexcept;

}  // namespace harkenprojectschema

#endif  // HARKEN_PROJECT_SCHEMA_COMPILE_TARGET_PROFILE_H
