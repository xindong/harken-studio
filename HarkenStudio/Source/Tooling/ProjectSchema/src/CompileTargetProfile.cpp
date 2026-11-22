#include "harken_project_schema/CompileTargetProfile.h"

#include <harken_studio_build/BusFingerprint.h>

#include <array>

namespace harken::studio::project_schema {
namespace {

std::uint32_t channelCount(harken::studio::types::ChannelLayout layout) noexcept
{
    switch (layout) {
    case harken::studio::types::kChannelLayoutMono: return 1;
    case harken::studio::types::kChannelLayoutStereo: return 2;
    case harken::studio::types::kChannelLayoutQuad: return 4;
    case harken::studio::types::kChannelLayout5_1: return 6;
    case harken::studio::types::kChannelLayout7_1: return 8;
    default: return 0;
    }
}

void append32(std::array<std::uint8_t, 24>& bytes,
              std::size_t& offset, std::uint32_t value) noexcept
{
    for (unsigned i = 0; i < 4; ++i)
        bytes[offset++] = static_cast<std::uint8_t>(value >> (i * 8));
}

void append64(std::array<std::uint8_t, 24>& bytes,
              std::size_t& offset, std::uint64_t value) noexcept
{
    for (unsigned i = 0; i < 8; ++i)
        bytes[offset++] = static_cast<std::uint8_t>(value >> (i * 8));
}

}  // namespace

std::uint64_t compileTargetProfileFingerprintV1(
    const CompileTargetProfileV1& profile) noexcept
{
    std::array<std::uint8_t, 24> bytes{};
    std::size_t offset = 0;
    append32(bytes, offset, profile.vintage);
    append32(bytes, offset, profile.sampleRate);
    append32(bytes, offset, profile.mixLayout);
    append32(bytes, offset, profile.deviceLayout);
    append64(bytes, offset, profile.capabilityMask);
    return harken::studio::build::busfingerprint::HashBytes(bytes.data(), bytes.size());
}

std::shared_ptr<const CompileTargetProfileV1> MakeCompileTargetProfileV1(
    std::uint32_t vintage,
    std::uint32_t sampleRate,
    harken::studio::types::ChannelLayout mixLayout,
    harken::studio::types::ChannelLayout deviceLayout,
    std::uint64_t capabilityMask) noexcept
{
    if (vintage != 1 || sampleRate == 0 || sampleRate > 384000
        || channelCount(mixLayout) == 0 || channelCount(deviceLayout) == 0
        || channelCount(deviceLayout) > channelCount(mixLayout)
        || (capabilityMask & ~kKnownCapabilityMaskV1) != 0)
        return {};
    auto profile = std::make_shared<CompileTargetProfileV1>();
    profile->vintage = vintage;
    profile->sampleRate = sampleRate;
    profile->mixLayout = mixLayout;
    profile->deviceLayout = deviceLayout;
    profile->capabilityMask = capabilityMask;
    profile->fingerprint = compileTargetProfileFingerprintV1(*profile);
    return profile;
}

}  // namespace harken::studio::project_schema
