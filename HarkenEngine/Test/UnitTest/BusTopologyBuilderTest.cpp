#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include <harken/BusTopologyBuilder.hpp>

#include <array>
#include <type_traits>

namespace {

using harkentest::EngineSession;
using harkentest::MakeNullConfig;

TEST(BusTopologyBuilder, PlainCppRowsApplyThroughTheStableCAbi) {
    static_assert(std::is_standard_layout_v<VnBusTopologyBus>);
    static_assert(std::is_standard_layout_v<VnBusTopologySend>);
    static_assert(std::is_standard_layout_v<VnBusTopologyDesc>);

    VnConfig config = MakeNullConfig(2, 256);
    config.maxBuses = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    std::array buses{
        harken::BusTopologyBuilder::Bus(harkentest::ObjectId(8101), VN_BUS_MASTER),
        harken::BusTopologyBuilder::Bus(harkentest::ObjectId(8102), harkentest::ObjectId(8101))};
    std::array sends{
        harken::BusTopologyBuilder::Send(harkentest::ObjectId(8102), VN_BUS_MASTER, -6.0f, 20)};
    const VnBusTopologyDesc topology =
        harken::BusTopologyBuilder::Snapshot(
            0x554E5245414C4255ull, 0, 1, buses.data(),
            static_cast<std::uint32_t>(buses.size()), sends.data(),
            static_cast<std::uint32_t>(sends.size()));

    EXPECT_EQ(sizeof(VnBusTopologyBus), topology.busStructSize);
    EXPECT_EQ(sizeof(VnBusTopologySend), topology.sendStructSize);
    EXPECT_EQ(VN_OK, Vn_ApplyBusTopology(&topology));
    EXPECT_NE(harken::kInvalidBusIndex,
              session.Engine()->buses.FindIndex(harkentest::ObjectId(8102)));
}

} // namespace
