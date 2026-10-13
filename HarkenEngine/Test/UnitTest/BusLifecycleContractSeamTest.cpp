#include <harken/Harken.h>

#include <cstdint>
#include <type_traits>

static_assert(
    std::is_same_v<decltype(&Vn_SetBusMute),
                   VnResult (*)(VnBusId, uint32_t, uint32_t)>,
    "Vn_SetBusMute signature pinned: (busId, mute01, fadeMs)");
static_assert(
    std::is_same_v<decltype(&Vn_SetBusSolo),
                   VnResult (*)(VnBusId, uint32_t, uint32_t)>,
    "Vn_SetBusSolo signature pinned: (busId, solo01, fadeMs)");
static_assert(std::is_same_v<decltype(VnSnapshot::busMuteMask), uint32_t>,
              "VnSnapshot::busMuteMask is one authored-mute bit per dense bus "
              "index (VN_SNAPSHOT_MAX_BUSES fits uint32_t)");
static_assert(
    std::is_same_v<decltype(VnSnapshot::busSoloEffectiveMask), uint32_t>,
    "VnSnapshot::busSoloEffectiveMask is one post-solo audibility bit per "
    "dense bus index");

static_assert(std::is_same_v<decltype(&Vn_ReparentBus),
                             VnResult (*)(VnBusId, VnBusId)>,
              "Vn_ReparentBus signature pinned: (busId, newParentId)");

static_assert(std::is_same_v<decltype(&Vn_DestroyBus), VnResult (*)(VnBusId)>,
              "Vn_DestroyBus signature pinned: (busId)");
