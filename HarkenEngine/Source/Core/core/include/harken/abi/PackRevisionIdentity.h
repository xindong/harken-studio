#ifndef HARKEN_ABI_PACK_REVISION_IDENTITY_H
#define HARKEN_ABI_PACK_REVISION_IDENTITY_H

#include <harken/Harken.h>

#include <cstdint>

namespace harken {

struct PackRevisionKey {
    VnPackHandle handle = 0;
    std::uint64_t packStableId = 0;
    std::uint32_t slotGeneration = 0;
    std::uint64_t contentRevision = 0;
    std::uint64_t partitionRevision = 0;
    std::uint64_t resourceRevision = 0;

    friend bool operator==(const PackRevisionKey&,
                           const PackRevisionKey&) = default;
};

using PackResourceRevisionToken = std::uint64_t;
using PackResourceClosureToken = std::uint64_t;

} // namespace harken

#endif
