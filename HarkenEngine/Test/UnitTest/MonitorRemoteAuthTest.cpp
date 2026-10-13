#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "MonitorCaptureHubTestHooks.h"
#include "Common/TestUtil.h"
#include "monitor_remote_codec.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#endif

namespace {

VnMonitorSessionDesc MakeObserveDesc() {
    VnMonitorSessionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_TIME_SERIES;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_MEASUREMENTS;
    desc.consumerQueueBytes = 8192;
    return desc;
}

VnMonitorSessionDesc MakeControlDesc() {
    VnMonitorSessionDesc desc = MakeObserveDesc();
    desc.requestedPermissionMask =
        VN_MONITOR_PERM_OBSERVE_MEASUREMENTS | VN_MONITOR_PERM_CAPTURE_CONTROL;
    return desc;
}

VnMonitorCapturePlan MakeArmedPlan() {
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_TIME_SERIES;
    plan.cadenceHz = 30;
    plan.bytesPerSecondBudget = 1024;
    return plan;
}

VnMonitorSessionDesc MakeAudioControlDesc() {
    VnMonitorSessionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_MEASUREMENTS |
                                   VN_MONITOR_PERM_CAPTURE_CONTROL |
                                   VN_MONITOR_PERM_RAW_AUDIO;
    desc.consumerQueueBytes = 65536;
    return desc;
}

VnMonitorTapDescriptor MakeMasterTap() {
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapKind = VN_MONITOR_TAP_MASTER_OUTPUT;
    tap.ownerKind = VN_MONITOR_ENTITY_BUS;
    tap.selectorMode = VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE;
    tap.ownerStableId = VN_BUS_MASTER;
    return tap;
}

VnMonitorCapturePlan MakeAudioArmedPlan(const VnMonitorTapDescriptor* tap) {
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.taps = tap;
    plan.downsample = 1;
    return plan;
}

bool WaitActiveGeneration(VnMonitorSessionHandle handle,
                          std::uint64_t generation) {
    const auto start = std::chrono::steady_clock::now();
    VnMonitorEffectiveCapturePlan effective{};
    effective.structSize = sizeof(effective);
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        if (Vn_MonitorGetEffectiveCapturePlan(handle, 0, &effective) == VN_OK &&
            effective.activationState ==
                static_cast<std::uint32_t>(VN_MONITOR_PLAN_ACTIVE) &&
            effective.capturePlanGeneration == generation) {
            return true;
        }
        std::this_thread::yield();
    }
    return false;
}

VnMonitorDropSummary RemoteTransportDrop() {
    return harken::monitor::InternalDropForTests(1);
}

} // namespace

TEST(MonitorRemote, UnimplementedLanProfileDoesNotAdvertiseAnEndpoint) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED,
              harken::monitor::EnableMonitorEndpointForTests(VN_MONITOR_SECURITY_LAN_MTLS));
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED, harken::monitor::ConnectMonitorEndpointForTests());
    VnMonitorCapabilities capabilities{};
    capabilities.structSize = sizeof(capabilities);
    ASSERT_EQ(VN_OK, Vn_MonitorGetCapabilities(&capabilities));
    EXPECT_EQ(0u, capabilities.remoteEnabled);
    EXPECT_EQ(VN_MONITOR_SECURITY_DISABLED, capabilities.securityProfile);
    EXPECT_TRUE(harken::monitor::MonitorEndpointNameForTests().empty());
}

TEST(MonitorRemote, AuthLocalAclCreatesRejectRemotePipe) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, harken::monitor::EnableMonitorEndpointForTests(
                         VN_MONITOR_SECURITY_LOCAL_ACL));
    const std::string name = harken::monitor::MonitorEndpointNameForTests();
    ASSERT_FALSE(name.empty());
    EXPECT_EQ(std::string::npos, name.find("HarkenRemote."));
    EXPECT_NE(std::string::npos, name.find("\\\\.\\pipe\\HarkenMonitor."));
    EXPECT_TRUE(harken::monitor::MonitorEndpointRejectsRemoteClientsForTests());
    ASSERT_EQ(VN_OK, harken::monitor::ConnectMonitorEndpointForTests());
    EXPECT_NE(0u, harken::monitor::MonitorEndpointLocalAclSidHashForTests());
    harken::monitor::DisableMonitorEndpointForTests();
}

TEST(MonitorRemote, AuthLocalAclIgnoresSelfReportedPrincipal) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, harken::monitor::EnableMonitorEndpointForTests(
                         VN_MONITOR_SECURITY_LOCAL_ACL));
    ASSERT_EQ(VN_OK, harken::monitor::ConnectMonitorEndpointForTests());
    const std::uint64_t osHash =
        harken::monitor::MonitorEndpointLocalAclSidHashForTests();
    ASSERT_NE(0u, osHash);
    EXPECT_EQ(VN_MONITOR_PERMISSION_DENIED,
              harken::monitor::OfferSelfReportedPrincipalForTests(
                  0xFFFFFFFFFFFFFFFFull,
                  VN_MONITOR_PERM_OBSERVE_STATE | VN_MONITOR_PERM_OBSERVE_EVENTS |
                      VN_MONITOR_PERM_OBSERVE_MEASUREMENTS |
                      VN_MONITOR_PERM_CAPTURE_CONTROL | VN_MONITOR_PERM_RECORD |
                      VN_MONITOR_PERM_RAW_AUDIO | VN_MONITOR_PERM_SYMBOLS));
    EXPECT_EQ(osHash, harken::monitor::MonitorEndpointLocalAclSidHashForTests());
    EXPECT_EQ(0u, harken::monitor::MonitorEndpointGrantedPermissionMaskForTests());
    harken::monitor::DisableMonitorEndpointForTests();
}

TEST(MonitorRemote, AuthHandshakeFailureLeavesNoHalfGrant) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, harken::monitor::EnableMonitorEndpointForTests(
                         VN_MONITOR_SECURITY_LOCAL_ACL));
    EXPECT_EQ(VN_MONITOR_INCOMPATIBLE,
              harken::monitor::FailMonitorHandshakeForTests());
    EXPECT_EQ(0u, harken::monitor::MonitorEndpointEpochForTests());
    EXPECT_EQ(0u, harken::monitor::MonitorEndpointGrantedPermissionMaskForTests());
    EXPECT_EQ(0u, harken::monitor::InternalQueuedCountForTests(1));
    harken::monitor::DisableMonitorEndpointForTests();
}

TEST(MonitorRemote, AuthObserverCannotApplyCapturePlan) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, harken::monitor::EnableMonitorEndpointForTests(
                         VN_MONITOR_SECURITY_LOCAL_ACL));
    ASSERT_EQ(VN_OK, harken::monitor::ConnectMonitorEndpointForTests());
    ASSERT_EQ(VN_OK, harken::monitor::GrantMonitorPermissionsForTests(
                         VN_MONITOR_PERM_OBSERVE_MEASUREMENTS));
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeObserveDesc();
    ASSERT_EQ(VN_OK, harken::monitor::OpenMonitorEndpointSessionForTests(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    lease.structSize = sizeof(lease);
    EXPECT_EQ(VN_MONITOR_PERMISSION_DENIED,
              Vn_MonitorAcquireCaptureControl(handle, &lease));
    VnMonitorEffectiveCapturePlan before{};
    before.structSize = sizeof(before);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(handle, 0, &before));
    VnMonitorCapturePlan plan = MakeArmedPlan();
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    EXPECT_EQ(VN_MONITOR_PERMISSION_DENIED,
              Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                         &accepted));
    VnMonitorEffectiveCapturePlan after{};
    after.structSize = sizeof(after);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(handle, 0, &after));
    EXPECT_EQ(before.capturePlanGeneration, after.capturePlanGeneration);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
    harken::monitor::DisableMonitorEndpointForTests();
}

TEST(MonitorRemote, AuthRawAudioNeedsPermissionAndPlan) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, harken::monitor::EnableMonitorEndpointForTests(
                         VN_MONITOR_SECURITY_LOCAL_ACL));
    ASSERT_EQ(VN_OK, harken::monitor::ConnectMonitorEndpointForTests());
    ASSERT_EQ(VN_OK, harken::monitor::GrantMonitorPermissionsForTests(
                         VN_MONITOR_PERM_OBSERVE_MEASUREMENTS |
                         VN_MONITOR_PERM_RAW_AUDIO));
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeObserveDesc();
    desc.requestRawAudio = 1;
    desc.requestedPermissionMask =
        VN_MONITOR_PERM_OBSERVE_MEASUREMENTS | VN_MONITOR_PERM_RAW_AUDIO;
    ASSERT_EQ(VN_OK, harken::monitor::OpenMonitorEndpointSessionForTests(&desc, &handle));
    VnMonitorSubscriptionStatus status{};
    status.structSize = sizeof(status);
    ASSERT_EQ(VN_OK, Vn_MonitorGetSubscriptionStatus(handle, &status));
    EXPECT_NE(0u, status.acceptedPermissionMask & VN_MONITOR_PERM_RAW_AUDIO);
    EXPECT_NE(1u, status.rawAudioDecision);
    EXPECT_FALSE(harken::monitor::MonitorEndpointEmitsRawAudioForTests());
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
    harken::monitor::DisableMonitorEndpointForTests();
}

TEST(MonitorRemote, AuthDisconnectBumpsLeaseWithoutStoppingRecording) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, harken::monitor::EnableMonitorEndpointForTests(
                         VN_MONITOR_SECURITY_LOCAL_ACL));
    ASSERT_EQ(VN_OK, harken::monitor::ConnectMonitorEndpointForTests());
    ASSERT_EQ(VN_OK, harken::monitor::GrantMonitorPermissionsForTests(
                         VN_MONITOR_PERM_OBSERVE_MEASUREMENTS |
                         VN_MONITOR_PERM_CAPTURE_CONTROL));
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc = MakeControlDesc();
    ASSERT_EQ(VN_OK, harken::monitor::OpenMonitorEndpointSessionForTests(&desc, &handle));
    VnMonitorCaptureControlStatus before{};
    before.structSize = sizeof(before);
    ASSERT_EQ(VN_OK, Vn_MonitorAcquireCaptureControl(handle, &before));
    const std::uint64_t generation = before.leaseGeneration;
    harken::monitor::DisconnectMonitorEndpointForTests();
    VnMonitorCaptureControlStatus after{};
    after.structSize = sizeof(after);
    ASSERT_EQ(VN_OK, Vn_MonitorGetCaptureControlStatus(handle, &after));
    EXPECT_GT(after.leaseGeneration, generation);
    EXPECT_EQ(4u, after.lastChangeReason);
    VnMonitorCapturePlan plan = MakeArmedPlan();
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    EXPECT_NE(VN_OK, Vn_MonitorApplyCapturePlan(handle, generation, &plan,
                                                &accepted));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
    harken::monitor::DisableMonitorEndpointForTests();
}

TEST(MonitorRemote, AuthTwoClientsDoNotStealRecords) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, harken::monitor::EnableMonitorEndpointForTests(
                         VN_MONITOR_SECURITY_LOCAL_ACL));
    ASSERT_EQ(VN_OK, harken::monitor::ConnectMonitorEndpointForTests());
    harken::monitor::StopDrainForTests();
    harken::monitor::EnableInternalConsumersForTests((1u << 1) | (1u << 2));
    ASSERT_TRUE(harken::monitor::PublishSyntheticTimeSeriesForTests(1));
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();
    const std::uint32_t a = harken::monitor::InternalQueuedCountForTests(1);
    const std::uint32_t b = harken::monitor::InternalQueuedCountForTests(2);
    EXPECT_EQ(a, b);
    EXPECT_GE(a, 1u);
    harken::monitor::DisableMonitorEndpointForTests();
}

TEST(MonitorRemote, AuthLanMtlsModelPackedHandshake) {
    std::uint8_t handshake[VN_MONITOR_REMOTE_HANDSHAKE_PACKED_BYTES]{};
    handshake[0] = 120;
    handshake[4] = static_cast<std::uint8_t>(VN_MONITOR_SECURITY_LAN_MTLS);
    handshake[16] = 11;
    handshake[24] = 22;
    handshake[32] = 33;
    handshake[40] = 44;
    handshake[48] = static_cast<std::uint8_t>(VN_MONITOR_PERM_OBSERVE_STATE);
    handshake[56] = 1;
    handshake[88] = 2;
    std::uint8_t spki[VN_MONITOR_REMOTE_SPKI_SHA256_BYTES]{};
    spki[1] = 1;
    const VnMonitorRemoteDecodeResult decoded = vn_monitor_decode_remote_handshake(
        handshake, sizeof(handshake), VN_MONITOR_SECURITY_LAN_MTLS, 11, 22, 33, 44,
        VN_MONITOR_PERM_OBSERVE_STATE, spki, sizeof(spki));
    ASSERT_EQ(VN_OK, decoded.status);
    EXPECT_EQ(VN_MONITOR_SECURITY_LAN_MTLS, decoded.handshake.securityProfile);
    EXPECT_EQ(1u, decoded.principal.lanMtlsSpkiSha256[1]);
    EXPECT_EQ(0u, decoded.principal.localAclSid);
}

TEST(MonitorRemote, AuthNoAudioChangeWhenRemoteStalls) {
    for (int trial = 0; trial < 3; ++trial) {
        harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
        ASSERT_EQ(VN_OK, session.InitResult());
        const std::vector<float> oneBlock =
            harkentest::MakeSine(256, 750.0, 48000.0, 0.7f);
        std::vector<float> sine;
        sine.reserve(oneBlock.size() * 3u);
        for (int block = 0; block < 3; ++block) {
            sine.insert(sine.end(), oneBlock.begin(), oneBlock.end());
        }
        const VnPCMDesc pcm = harkentest::MonoDesc(sine, 48000);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(static_cast<VnSoundId>(harkentest::ObjectId(10 + trial)), &pcm));
        const VnPlayDesc play =
            harkentest::MakePlayDesc(static_cast<VnSoundId>(harkentest::ObjectId(10 + trial)));
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));

        harkentest::Capture absent;
        harkentest::PumpAndCapture(session.Engine(), 1, absent);
        ASSERT_EQ(oneBlock.size(), absent.left.size());
        ASSERT_EQ(oneBlock.size(), absent.right.size());
        ASSERT_NE(0.0f, absent.left[16]);
        for (std::size_t i = 0; i < oneBlock.size(); ++i) {
            const float expected = oneBlock[i];
            ASSERT_EQ(expected, absent.left[i]) << "absent left sample " << i;
            ASSERT_EQ(expected, absent.right[i]) << "absent right sample " << i;
        }

        ASSERT_EQ(VN_OK, harken::monitor::EnableMonitorEndpointForTests(
                             VN_MONITOR_SECURITY_LOCAL_ACL));
        ASSERT_EQ(VN_OK, harken::monitor::ConnectMonitorEndpointForTests());
        ASSERT_EQ(VN_OK, harken::monitor::GrantMonitorPermissionsForTests(
                             VN_MONITOR_PERM_OBSERVE_MEASUREMENTS |
                             VN_MONITOR_PERM_CAPTURE_CONTROL |
                             VN_MONITOR_PERM_RAW_AUDIO));
        VnMonitorSessionHandle control = 0;
        VnMonitorSessionDesc controlDesc = MakeAudioControlDesc();
        ASSERT_EQ(VN_OK, harken::monitor::OpenMonitorEndpointSessionForTests(&controlDesc, &control));
        VnMonitorCaptureControlStatus lease{};
        lease.structSize = sizeof(lease);
        ASSERT_EQ(VN_OK, Vn_MonitorAcquireCaptureControl(control, &lease));
        const VnMonitorTapDescriptor tap = MakeMasterTap();
        VnMonitorCapturePlan plan = MakeAudioArmedPlan(&tap);
        VnMonitorEffectiveCapturePlan accepted{};
        accepted.structSize = sizeof(accepted);
        ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(
                             control, lease.leaseGeneration, &plan, &accepted));
        ASSERT_EQ(1u, accepted.accepted.tapCount);
        ASSERT_NE(0u, accepted.accepted.streamKindMask &
                          VN_MONITOR_STREAM_OPTIONAL_AUDIO);
        harkentest::Capture idle;
        harkentest::PumpAndCapture(session.Engine(), 1, idle);
        ASSERT_TRUE(
            WaitActiveGeneration(control, accepted.capturePlanGeneration));
        ASSERT_EQ(absent.left.size(), idle.left.size());
        ASSERT_EQ(absent.right.size(), idle.right.size());
        for (std::size_t i = 0; i < absent.left.size(); ++i) {
            ASSERT_EQ(absent.left[i], idle.left[i]) << "idle sample " << i;
            ASSERT_EQ(absent.right[i], idle.right[i]) << "idle right sample " << i;
        }

        harken::monitor::StopDrainForTests();
        harken::monitor::EnableInternalConsumersForTests((1u << 1) | (1u << 2));
        const VnMonitorDropSummary seeded = RemoteTransportDrop();
        VnMonitorDropSummary filled = seeded;
        const auto start = std::chrono::steady_clock::now();
        while (filled.lastSourceSequence <= seeded.lastSourceSequence &&
               std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
            // Keep the single producer and consumer sequential while filling
            // the deliberately stalled remote transport queue.
            (void)harken::monitor::PublishSyntheticTimeSeriesForTests(1);
            harken::monitor::DrainOnceForTests();
            filled = RemoteTransportDrop();
        }
        ASSERT_EQ(VN_MONITOR_DROP_REMOTE_TRANSPORT, filled.stage);
        ASSERT_GT(filled.lastSourceSequence, seeded.lastSourceSequence)
            << "stalled remote did not publish a fresh REMOTE_TRANSPORT drop";

        harkentest::Capture stalled;
        harkentest::PumpAndCapture(session.Engine(), 1, stalled);
        harken::monitor::DrainOnceForTests();
        const VnMonitorDropSummary afterStalledPump = RemoteTransportDrop();
        harken::monitor::StartDrainForTests();
        ASSERT_EQ(VN_MONITOR_DROP_REMOTE_TRANSPORT, afterStalledPump.stage);
        EXPECT_GT(afterStalledPump.lastSourceSequence,
                  filled.lastSourceSequence)
            << "the armed audible pump did not publish a fresh remote drop";
        ASSERT_EQ(absent.left.size(), stalled.left.size());
        ASSERT_EQ(absent.right.size(), stalled.right.size());
        for (std::size_t i = 0; i < absent.left.size(); ++i) {
            ASSERT_EQ(absent.left[i], stalled.left[i]) << "stalled sample " << i;
            ASSERT_EQ(absent.right[i], stalled.right[i]) << "stalled right sample " << i;
        }
        EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(control));
        harken::monitor::DisableMonitorEndpointForTests();
    }
}

#if defined(_WIN32)
TEST(MonitorRemote, AuthLocalAclRuntimePipeConnects) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, harken::monitor::EnableMonitorEndpointForTests(
                         VN_MONITOR_SECURITY_LOCAL_ACL));
    const std::string name = harken::monitor::MonitorEndpointNameForTests();
    HANDLE pipe = INVALID_HANDLE_VALUE;
    for (int attempt = 0; attempt < 50 && pipe == INVALID_HANDLE_VALUE; ++attempt) {
        pipe = CreateFileA(name.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                           OPEN_EXISTING, 0, nullptr);
        if (pipe == INVALID_HANDLE_VALUE) {
            Sleep(10);
        }
    }
    ASSERT_NE(INVALID_HANDLE_VALUE, pipe) << GetLastError();
    CloseHandle(pipe);
    harken::monitor::DisableMonitorEndpointForTests();
}

TEST(MonitorRemote, AuthLocalAclReconnectsAfterPendingCancel) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, harken::monitor::EnableMonitorEndpointForTests(
                         VN_MONITOR_SECURITY_LOCAL_ACL));
    harken::monitor::DisableMonitorEndpointForTests();
    ASSERT_EQ(VN_OK, harken::monitor::EnableMonitorEndpointForTests(
                         VN_MONITOR_SECURITY_LOCAL_ACL));
    ASSERT_EQ(VN_OK, harken::monitor::ConnectMonitorEndpointForTests());
    EXPECT_NE(0u, harken::monitor::MonitorEndpointEpochForTests());
    harken::monitor::DisableMonitorEndpointForTests();
}
#endif
