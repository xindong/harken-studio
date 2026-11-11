#pragma once
#include <harken/ObjectId.h>
#include <cstdint>

namespace harken {
// Logic-side complete-chain values. UINT32_MAX denotes an unoccupied slot;
// the Preview ABI validates nonempty entries before creating this image.
struct PreviewEffectChainEntry {
    VnObjectId effectId{};
    std::uint32_t effectType = UINT32_MAX;
    bool bypass = false;
    float ceilingDb = 0;
    float releaseMs = 0;
};
} // namespace harken
