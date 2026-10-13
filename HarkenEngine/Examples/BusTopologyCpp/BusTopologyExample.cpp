#include <harken/BusTopologyBuilder.hpp>

#include <array>

VnResult ApplyGameBusTopology(std::uint64_t baseRevision,
                              std::uint64_t revision) {
    std::array buses{
        harken::BusTopologyBuilder::Bus(100, VN_BUS_MASTER),
        harken::BusTopologyBuilder::Bus(101, 100)};
    std::array sends{
        harken::BusTopologyBuilder::Send(101, VN_BUS_MASTER, -6.0f, 20)};
    const VnBusTopologyDesc topology =
        harken::BusTopologyBuilder::Snapshot(
            0x47414D45544F504Full, baseRevision, revision, buses.data(),
            static_cast<std::uint32_t>(buses.size()), sends.data(),
            static_cast<std::uint32_t>(sends.size()));
    return Vn_ApplyBusTopology(&topology);
}
