#ifndef HARKEN_BUS_TOPOLOGY_BUILDER_HPP
#define HARKEN_BUS_TOPOLOGY_BUILDER_HPP

#include <harken/Harken.h>

#include <cstdint>

namespace harken {

// Allocation-free helpers for C++ hosts, including Unreal adapters. The
// caller owns the row storage until Vn_ApplyBusTopology returns.
class BusTopologyBuilder final {
public:
    static VnBusTopologyBus Bus(VnBusId busId,
                                VnBusId parentId) noexcept {
        VnBusTopologyBus row{};
        row.structSize = sizeof(row);
        row.busId = busId;
        row.parentId = parentId;
        return row;
    }

    static VnBusTopologySend Send(VnBusId fromBusId, VnBusId toBusId,
                                  float levelDb = 0.0f,
                                  std::uint32_t rampMs = 0) noexcept {
        VnBusTopologySend row{};
        row.structSize = sizeof(row);
        row.rampMs = rampMs;
        row.fromBusId = fromBusId;
        row.toBusId = toBusId;
        row.levelDb = levelDb;
        return row;
    }

    static VnBusTopologyDesc Snapshot(
        std::uint64_t ownerId, std::uint64_t baseRevision,
        std::uint64_t revision, const VnBusTopologyBus* buses,
        std::uint32_t busCount, const VnBusTopologySend* sends = nullptr,
        std::uint32_t sendCount = 0) noexcept {
        VnBusTopologyDesc desc{};
        desc.structSize = sizeof(desc);
        desc.ownerId = ownerId;
        desc.baseRevision = baseRevision;
        desc.revision = revision;
        desc.busCount = busCount;
        desc.busStructSize = sizeof(VnBusTopologyBus);
        desc.buses = buses;
        desc.sendCount = sendCount;
        desc.sendStructSize = sizeof(VnBusTopologySend);
        desc.sends = sends;
        return desc;
    }
};

} // namespace harken

#endif // HARKEN_BUS_TOPOLOGY_BUILDER_HPP
