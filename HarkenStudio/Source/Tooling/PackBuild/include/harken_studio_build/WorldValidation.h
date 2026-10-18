#ifndef HARKEN_STUDIO_BUILD_DETAIL_CURRENT_WORLD_VALIDATION_H
#define HARKEN_STUDIO_BUILD_DETAIL_CURRENT_WORLD_VALIDATION_H

#include "harken_studio_build/World.h"

namespace harkenstudiobuild::detail {

#if defined(__GNUC__) || defined(__clang__)
#define HARKEN_STUDIO_BUILD_DETAIL_HIDDEN __attribute__((visibility("hidden")))
#else
#define HARKEN_STUDIO_BUILD_DETAIL_HIDDEN
#endif

HARKEN_STUDIO_BUILD_DETAIL_HIDDEN WorldInstallStatus ValidateCurrentWorldPartitionLocalV1(
    const ContainerWorldInstallViewV1& world) noexcept;

HARKEN_STUDIO_BUILD_DETAIL_HIDDEN std::uint64_t ComputeCurrentWorldLocalCapabilityMaskV1(
    const ContainerWorldInstallViewV1& world) noexcept;

#undef HARKEN_STUDIO_BUILD_DETAIL_HIDDEN

} // namespace harkenstudiobuild::detail

#endif // HARKEN_STUDIO_BUILD_DETAIL_CURRENT_WORLD_VALIDATION_H
