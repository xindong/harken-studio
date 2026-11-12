#ifndef HARKEN_REFERENCE_SLOTS_REFERENCE_SLOTS_H
#define HARKEN_REFERENCE_SLOTS_REFERENCE_SLOTS_H

#include <harken_pack/PackSlots.h>

#include <cstdint>
#include <iterator>

namespace harkenreferenceslots {

inline constexpr std::uint32_t kReferenceSlotTableVintage = HK_PACK_SLOT_TABLE_VINTAGE;

enum class ReferenceSlot : std::uint32_t {
    OutputBus = HK_PACK_SLOT_OUTPUT_BUS,
    Attenuation = HK_PACK_SLOT_ATTENUATION,
};

inline constexpr std::uint32_t kReferenceSlotCount = HK_PACK_SLOT_COUNT;

enum class ResolutionLevel : std::uint32_t {
    ObjectOverride = 1,
    PlacementAncestor = 2,
    OrganizationalAncestor = 3,
    SlotDefault = 4,
};

inline constexpr std::uint32_t kResolutionLevelCount = 4;

struct ReferenceSlotMeta {
    ReferenceSlot slot;
    const char* name;
};

inline constexpr ReferenceSlotMeta kReferenceSlotTable[] = {
    {ReferenceSlot::OutputBus, "outputBus"},
    {ReferenceSlot::Attenuation, "attenuation"},
};

static_assert(std::size(kReferenceSlotTable) ==
                  kReferenceSlotCount);
static_assert(kReferenceSlotTable[0].slot ==
                  ReferenceSlot::OutputBus);
static_assert(kReferenceSlotTable[1].slot ==
                  ReferenceSlot::Attenuation);

static_assert(static_cast<std::uint32_t>(ResolutionLevel::ObjectOverride) <
                  static_cast<std::uint32_t>(ResolutionLevel::PlacementAncestor));
static_assert(static_cast<std::uint32_t>(ResolutionLevel::PlacementAncestor) <
                  static_cast<std::uint32_t>(ResolutionLevel::OrganizationalAncestor));
static_assert(static_cast<std::uint32_t>(ResolutionLevel::OrganizationalAncestor) <
                  static_cast<std::uint32_t>(ResolutionLevel::SlotDefault));
static_assert(static_cast<std::uint32_t>(ResolutionLevel::SlotDefault) ==
                  kResolutionLevelCount);

}

#endif
