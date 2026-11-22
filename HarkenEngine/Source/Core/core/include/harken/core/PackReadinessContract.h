#ifndef HARKEN_CORE_PACK_READINESS_CONTRACT_H
#define HARKEN_CORE_PACK_READINESS_CONTRACT_H

#include <harken/Harken.h>

#include "harken/core/CurrentWorld.h"
#include "harken/abi/PackRevisionIdentity.h"

#include <cstdint>

namespace harken {

enum class PackIdentityReadiness : std::uint32_t {
    Absent = 0,
    KnownUnloaded = 1,
    Active = 2,
};

enum class PackLeaseSlice : std::uint8_t {
    SharedActionAndDeferred = 0,
    Playable = 1,
};

struct PackPartitionLease {
    VnPackHandle handle = 0;
    std::uint64_t packStableId = 0;
    std::uint64_t contentRevision = 0;
    std::uint64_t partitionRevision = 0;
    std::uint64_t resourceRevision = 0;
    PackResourceRevisionToken resourceToken = 0;
    PackResourceClosureToken resourceClosureToken = 0;
    std::uint64_t compositeGeneration = 0;
    std::uint32_t slotGeneration = 0;
    std::uint64_t leaseToken = 0;
};

struct PackIdentityLookup {
    PackIdentityReadiness readiness = PackIdentityReadiness::Absent;
    VnResult result = VN_UNKNOWN_ID;
    PackPartitionLease lease{};
    const ActionProgramViewV1* action = nullptr;
    const void* playableRow = nullptr;
};

} // namespace harken

#endif
