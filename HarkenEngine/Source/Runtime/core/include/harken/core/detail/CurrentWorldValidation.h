#ifndef HARKEN_CORE_DETAIL_CURRENT_WORLD_VALIDATION_H
#define HARKEN_CORE_DETAIL_CURRENT_WORLD_VALIDATION_H

#include "harken/core/CurrentWorld.h"
#include <span>

namespace harken::detail {

#if defined(__GNUC__) || defined(__clang__)
#define HARKEN_CORE_DETAIL_HIDDEN __attribute__((visibility("hidden")))
#else
#define HARKEN_CORE_DETAIL_HIDDEN
#endif

// Composite rows retain the contract of their owning partition. This context
// is used only while validating Runtime Pack composition, never as a wire format.
struct CompositeWorldVintagesV1 {
    std::span<const std::uint32_t> segments;
    std::span<const std::uint32_t> actions;
    bool hasEntryScheduling = false;
};

HARKEN_CORE_DETAIL_HIDDEN WorldInstallStatus ValidateCurrentWorldCompositeV1(
    const ContainerWorldInstallViewV1& world,
    const CompositeWorldVintagesV1& vintages) noexcept;

HARKEN_CORE_DETAIL_HIDDEN WorldInstallStatus ValidateCurrentWorldPartitionLocalV1(
    const ContainerWorldInstallViewV1& world) noexcept;

HARKEN_CORE_DETAIL_HIDDEN std::uint64_t ComputeCurrentWorldLocalCapabilityMaskV1(
    const ContainerWorldInstallViewV1& world) noexcept;
HARKEN_CORE_DETAIL_HIDDEN std::uint64_t ComputeCurrentWorldLocalCapabilityMaskV1(
    const HistoricalContainerWorldViewV1& world) noexcept;

#undef HARKEN_CORE_DETAIL_HIDDEN

} // namespace harken::detail

#endif // HARKEN_CORE_DETAIL_CURRENT_WORLD_VALIDATION_H
