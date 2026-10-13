#ifndef HARKEN_STUDIO_BUILD_CURRENT_WORLD_ARTIFACT_H
#define HARKEN_STUDIO_BUILD_CURRENT_WORLD_ARTIFACT_H

#include "harken_studio_build/World.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace harkenstudiobuild {

inline constexpr std::uint32_t kCurrentWorldArtifactHeaderBytes = 64;
inline constexpr std::uint64_t kCurrentWorldArtifactMaxBodyBytes =
    UINT64_C(67108864);

bool EncodeCurrentWorldArtifactV1(
    const ContainerWorldInstallViewV1& world,
    std::vector<std::uint8_t>& out) noexcept;

} // namespace harkenstudiobuild

#endif // HARKEN_STUDIO_BUILD_CURRENT_WORLD_ARTIFACT_H
