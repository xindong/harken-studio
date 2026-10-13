#include <harken/Harken.h>

#include "MonitorCaptureHubTestHooks.h"
#include "Common/TestUtil.h"
#include "harken_wire_ids.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <string>

TEST(MonitorRemote, EndpointDefaultClosedHasNoListen) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorCapabilities caps{};
    caps.structSize = sizeof(caps);
    ASSERT_EQ(VN_OK, Vn_MonitorGetCapabilities(&caps));
    EXPECT_EQ(0u, caps.remoteEnabled);
    EXPECT_EQ(VN_MONITOR_SECURITY_DISABLED, caps.securityProfile);
    EXPECT_TRUE(harken::monitor::MonitorEndpointNameForTests().empty());
    EXPECT_EQ(0, harken::monitor::MonitorEndpointPortForTests());
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED,
              harken::monitor::ConnectMonitorEndpointForTests());
}

TEST(MonitorRemote, EndpointExplicitEnableBindsIndependentName) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, harken::monitor::EnableMonitorEndpointForTests(
                         VN_MONITOR_SECURITY_LOCAL_ACL));
    VnMonitorCapabilities caps{};
    caps.structSize = sizeof(caps);
    ASSERT_EQ(VN_OK, Vn_MonitorGetCapabilities(&caps));
    EXPECT_EQ(1u, caps.remoteEnabled);
    EXPECT_EQ(VN_MONITOR_SECURITY_LOCAL_ACL, caps.securityProfile);
    const std::string name = harken::monitor::MonitorEndpointNameForTests();
    EXPECT_FALSE(name.empty());
    EXPECT_EQ(std::string::npos, name.find("HarkenRemote."));
    EXPECT_EQ(std::string::npos, name.find("harken-remote"));
    EXPECT_EQ(std::string::npos, name.find("HarkenRemote"));
    EXPECT_NE(std::string::npos, name.find("HarkenMonitor."));
    EXPECT_EQ(VN_OK, harken::monitor::ConnectMonitorEndpointForTests());
    harken::monitor::DisableMonitorEndpointForTests();
    ASSERT_EQ(VN_OK, Vn_MonitorGetCapabilities(&caps));
    EXPECT_EQ(0u, caps.remoteEnabled);
    EXPECT_TRUE(harken::monitor::MonitorEndpointNameForTests().empty());
}

TEST(MonitorRemote, EndpointRejectsCommandMirrorMutation) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, harken::monitor::EnableMonitorEndpointForTests(
                         VN_MONITOR_SECURITY_LOCAL_ACL));
    ASSERT_EQ(VN_OK, harken::monitor::ConnectMonitorEndpointForTests());

    VnSnapshot before{};
    before.structSize = sizeof(before);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&before));
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED, harken::monitor::SendMonitorOpcodeForTests(
                                          harkenremote::kOpReservedLegacyDirectPlay));
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED,
              harken::monitor::SendMonitorOpcodeForTests(harkenremote::kOpVn_Stop));
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED, harken::monitor::SendMonitorOpcodeForTests(
                                          harkenremote::kOpVn_SetBusVolume));
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED, harken::monitor::SendMonitorOpcodeForTests(
                                          harkenremote::kOpVn_SetBusOutputVolume));
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED, harken::monitor::SendMonitorOpcodeForTests(
                                          harkenremote::kOpVn_SetVoiceOutputVolume));
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED,
              harken::monitor::SendMonitorOpcodeForTests(harkenremote::kOpVn_StopAll));
    VnSnapshot after{};
    after.structSize = sizeof(after);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&after));
    EXPECT_EQ(before.activeVoiceCount, after.activeVoiceCount);
    EXPECT_EQ(before.busCount, after.busCount);
    harken::monitor::DisableMonitorEndpointForTests();
}

TEST(MonitorRemote, EndpointReconnectMintsNewEpoch) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, harken::monitor::EnableMonitorEndpointForTests(
                         VN_MONITOR_SECURITY_LOCAL_ACL));
    ASSERT_EQ(VN_OK, harken::monitor::ConnectMonitorEndpointForTests());
    const std::uint64_t epoch1 = harken::monitor::MonitorEndpointEpochForTests();
    EXPECT_NE(0u, epoch1);
    harken::monitor::DisconnectMonitorEndpointForTests();
    ASSERT_EQ(VN_OK, harken::monitor::ConnectMonitorEndpointForTests());
    const std::uint64_t epoch2 = harken::monitor::MonitorEndpointEpochForTests();
    EXPECT_NE(epoch1, epoch2);
    EXPECT_EQ(VN_MONITOR_STALE_SESSION,
              harken::monitor::SendMonitorPollWithEpochForTests(epoch1));
    EXPECT_EQ(VN_MONITOR_STALE_LEASE,
              harken::monitor::SendMonitorLeaseWithEpochForTests(epoch1));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION,
              harken::monitor::SendMonitorOpcodeWithEpochForTests(
                  harkenremote::kOpVn_Stop, epoch1));
    harken::monitor::DisableMonitorEndpointForTests();
}

TEST(MonitorRemote, EndpointObserveOpcodeAcceptedMutationRejected) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, harken::monitor::EnableMonitorEndpointForTests(
                         VN_MONITOR_SECURITY_LOCAL_ACL));
    ASSERT_EQ(VN_OK, harken::monitor::ConnectMonitorEndpointForTests());
    EXPECT_EQ(VN_OK, harken::monitor::SendMonitorOpcodeForTests(0));
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED, harken::monitor::SendMonitorOpcodeForTests(
                                          harkenremote::kOpVn_StopAll));
    harken::monitor::DisableMonitorEndpointForTests();
}
