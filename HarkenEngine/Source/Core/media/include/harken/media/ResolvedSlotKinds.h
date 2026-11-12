#ifndef HARKEN_MEDIA_RESOLVED_SLOT_KINDS_H
#define HARKEN_MEDIA_RESOLVED_SLOT_KINDS_H

#include <harken_pack/PackSlots.h>

#include <cstdint>
#include <iterator>

namespace harken {

inline constexpr std::uint32_t kResolvedSlotTableVintage = HK_PACK_SLOT_TABLE_VINTAGE;

enum class ResolvedSlotKind : std::uint32_t {
    OutputBus = HK_PACK_SLOT_OUTPUT_BUS,
    Attenuation = HK_PACK_SLOT_ATTENUATION,
};

inline constexpr std::uint32_t kResolvedSlotKindCount = HK_PACK_SLOT_COUNT;

enum class ResolvedSlotTargetKind : std::uint32_t {
    Attenuation = HK_PACK_SLOT_TARGET_ATTENUATION,
    Bus = HK_PACK_SLOT_TARGET_BUS,
};

struct ResolvedSlotKindRow {
    ResolvedSlotKind kind;
    ResolvedSlotTargetKind targetKind;
    const char* name;
};

inline constexpr ResolvedSlotKindRow kResolvedSlotKindTable[] = {
    {ResolvedSlotKind::OutputBus, static_cast<ResolvedSlotTargetKind>(HK_PACK_SLOT_OUTPUT_BUS_TARGET), "outputBus"},
    {ResolvedSlotKind::Attenuation, static_cast<ResolvedSlotTargetKind>(HK_PACK_SLOT_ATTENUATION_TARGET), "attenuation"},
};

static_assert(std::size(kResolvedSlotKindTable) == kResolvedSlotKindCount);

inline const ResolvedSlotKindRow* FindResolvedSlotKind(ResolvedSlotKind kind) {
    for (const ResolvedSlotKindRow& row : kResolvedSlotKindTable) {
        if (row.kind == kind) {
            return &row;
        }
    }
    return nullptr;
}

}

#endif
