#include <harken/Harken.h>

#include "MonitorCaptureHubTestHooks.h"
#include "MonitorCaptureHub.h"
#include "Common/TestUtil.h"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <thread>
#include <vector>

namespace {

VnMonitorSessionDesc MakeSessionDesc() {
    VnMonitorSessionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_STATE;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_STATE;
    desc.consumerQueueBytes = 4096;
    return desc;
}

struct PollArgs {
    std::array<unsigned char, 64> buffer{};
    VnMonitorPollDesc desc{};
    VnMonitorPollResult result{};

    PollArgs() {
        desc.structSize = sizeof(desc);
        desc.buffer = buffer.data();
        desc.bufferBytes = buffer.size();
        result.structSize = sizeof(result);
    }
};

void WaitPollEntered(std::uint32_t count) {
    const auto start = std::chrono::steady_clock::now();
    while (harken::monitor::PollEnteredForTests() < count &&
           std::chrono::steady_clock::now() - start <
               std::chrono::seconds(2)) {
        std::this_thread::yield();
    }
}

} // namespace

TEST(MonitorHubSession, NoEngineReturnsNotInitialized) {
    VnMonitorCapabilities caps{};
    caps.structSize = sizeof(caps);
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorGetCapabilities(&caps));
    VnMonitorSessionHandle handle = 1;
    VnMonitorSessionDesc desc = MakeSessionDesc();
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorOpenSession(&desc, &handle));
}

TEST(MonitorHubSession, CaptureResourcesStartWithFirstConsumer) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    EXPECT_FALSE(harken::monitor::DrainRunningForTests());
    EXPECT_FALSE(harken::monitor::CaptureResourcesAllocatedForTests());
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FALSE(harken::monitor::CaptureResourcesAllocatedForTests());
    VnMonitorSessionDesc desc = MakeSessionDesc();
    VnMonitorSessionHandle session = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    EXPECT_TRUE(harken::monitor::DrainRunningForTests());
    EXPECT_TRUE(harken::monitor::CaptureResourcesAllocatedForTests());
    EXPECT_EQ(0u, harken::monitor::InternalQueuedCountForTests(0));
    EXPECT_EQ(0u, harken::monitor::InternalQueuedCountForTests(1));
    EXPECT_EQ(0u, harken::monitor::InternalQueuedCountForTests(2));
    ASSERT_EQ(VN_OK, Vn_MonitorCloseSession(session));
    ASSERT_EQ(VN_OK, Vn_Term());
    EXPECT_FALSE(harken::monitor::DrainRunningForTests());
    EXPECT_FALSE(harken::monitor::CaptureResourcesAllocatedForTests());
}

TEST(MonitorHubSession, RemotePermissionsDoNotRestrictLocalConsumer) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionDesc desc = MakeSessionDesc();
    desc.requestedPermissionMask |= VN_MONITOR_PERM_CAPTURE_CONTROL;
    VnMonitorSessionHandle remote = 0;
    VnMonitorSessionHandle local = 0;
    ASSERT_EQ(VN_OK, harken::monitor::OpenRemoteSession(
                         &desc, VN_MONITOR_PERM_OBSERVE_STATE, 42, &remote));
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &local));
    VnMonitorCaptureControlStatus lease{};
    lease.structSize = sizeof(lease);
    EXPECT_EQ(VN_MONITOR_PERMISSION_DENIED,
              Vn_MonitorAcquireCaptureControl(remote, &lease));
    ASSERT_EQ(VN_OK, Vn_MonitorAcquireCaptureControl(local, &lease));
    const std::uint64_t generation = lease.leaseGeneration;
    ASSERT_EQ(VN_OK, Vn_MonitorCloseSession(remote));
    ASSERT_EQ(VN_OK, Vn_MonitorGetCaptureControlStatus(local, &lease));
    EXPECT_EQ(generation, lease.leaseGeneration);
    EXPECT_EQ(1u, lease.held);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(local));
}

TEST(MonitorHubSession, TypedLocalRawPermissionRequiresBothRequestAndAllowedStream) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    struct PrincipalScope {
        ~PrincipalScope() { harken::monitor::ClearHostPrincipalForTests(); }
    } principal;
    for (unsigned mode = 0; mode != 4; ++mode) {
        SCOPED_TRACE(mode);
        harken::monitor::SetHostPrincipalForTests(true, VN_MONITOR_STREAM_EVENT |
            (mode == 1 ? 0u : VN_MONITOR_STREAM_OPTIONAL_AUDIO));
        VnMonitorSessionDescV2 desc{};
        desc.structSize = sizeof(desc);
        desc.requestedExecutionFactVersion = 1;
        desc.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT |
            (mode == 3 ? 0u : VN_MONITOR_STREAM_OPTIONAL_AUDIO);
        desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS |
            (mode == 2 ? 0u : VN_MONITOR_PERM_RAW_AUDIO);
        desc.requestRawAudio = 1;
        VnMonitorSessionHandle handle = 0;
        ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(reinterpret_cast<const VnMonitorSessionDesc*>(&desc), &handle));
        VnMonitorSubscriptionStatusV2 status{};
        status.structSize = sizeof(status);
        ASSERT_EQ(VN_OK, Vn_MonitorGetSubscriptionStatus(handle,
            reinterpret_cast<VnMonitorSubscriptionStatus*>(&status)));
        EXPECT_EQ(mode == 0 ? VN_MONITOR_PERM_RAW_AUDIO : 0u, status.acceptedPermissionMask & VN_MONITOR_PERM_RAW_AUDIO);
        EXPECT_EQ(mode == 0 ? VN_MONITOR_STREAM_OPTIONAL_AUDIO : 0u,
            status.resolvedStreamKindMask & VN_MONITOR_STREAM_OPTIONAL_AUDIO);
        EXPECT_EQ(0u, status.unsupportedPermissionMask & VN_MONITOR_PERM_RAW_AUDIO);
        EXPECT_EQ(mode == 1 || mode == 3 ? VN_MONITOR_PERM_RAW_AUDIO : 0u,
            status.deniedPermissionMask & VN_MONITOR_PERM_RAW_AUDIO);
        EXPECT_EQ(mode == 0 ? 2u : 3u, status.rawAudioDecision);
        EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
    }
    harken::monitor::ClearHostPrincipalForTests();
    auto legacy = MakeSessionDesc();
    legacy.requestedStreamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    legacy.requestedPermissionMask = VN_MONITOR_PERM_RAW_AUDIO;
    legacy.requestRawAudio = 1;
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&legacy, &handle));
    VnMonitorSubscriptionStatus status{};
    status.structSize = sizeof(status);
    ASSERT_EQ(VN_OK, Vn_MonitorGetSubscriptionStatus(handle, &status));
    EXPECT_EQ(0u, status.acceptedPermissionMask & VN_MONITOR_PERM_RAW_AUDIO);
    EXPECT_EQ(VN_MONITOR_PERM_RAW_AUDIO, status.unsupportedPermissionMask & VN_MONITOR_PERM_RAW_AUDIO);
    EXPECT_EQ(3u, status.rawAudioDecision);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubSession, FailedFirstCaptureAllocationReleasesResourcesAndAllowsRetry) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionDesc desc = MakeSessionDesc();
    VnMonitorSessionHandle session = 0;
    harken::monitor::SetFailCaptureResourcesForTests(true);
    EXPECT_EQ(VN_MONITOR_BUDGET_EXCEEDED, Vn_MonitorOpenSession(&desc, &session));
    EXPECT_EQ(0u, session);
    EXPECT_FALSE(harken::monitor::DrainRunningForTests());
    EXPECT_FALSE(harken::monitor::CaptureResourcesAllocatedForTests());
    EXPECT_EQ(0u, harken::monitor::ProducerRingCapacityForTests());
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &session));
    EXPECT_TRUE(harken::monitor::DrainRunningForTests());
    EXPECT_TRUE(harken::monitor::CaptureResourcesAllocatedForTests());
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

TEST(MonitorHubSession, OpenPollZeroClose) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorCapabilities caps{};
    caps.structSize = sizeof(caps);
    EXPECT_EQ(VN_OK, Vn_MonitorGetCapabilities(&caps));
    EXPECT_EQ((1u << 0) | (1u << 1) | (1u << 2) | (1u << 3) | (1u << 4) |
                  (1u << 5) | (1u << 6) | (1u << 7) | (1u << 8) | (1u << 9) |
                  (1u << 10) | (1u << 11) | (1u << 12) | (1u << 13) |
                  (1u << 14) | (1u << 15) | (1u << 16) | (1u << 17) |
                  (1u << 18),
              caps.monitorImplemented);
    EXPECT_EQ(VN_MONITOR_HARD_PROCESS_SESSION_SLOTS,
              caps.resolvedProcessSessionSlots);
    EXPECT_EQ(VN_MONITOR_HARD_PROCESS_QUEUE_BYTES,
              caps.resolvedProcessQueueBytes);

    VnMonitorCapabilities dirtyCaps = caps;
    dirtyCaps.reserved0 = 1;
    EXPECT_EQ(VN_INVALID_ARG, Vn_MonitorGetCapabilities(&dirtyCaps));

    VnMonitorSessionDesc desc = MakeSessionDesc();
    desc.requestedPermissionMask =
        VN_MONITOR_PERM_OBSERVE_STATE | VN_MONITOR_PERM_RECORD;
    VnMonitorSessionHandle handle = 0;
    EXPECT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    EXPECT_NE(0u, handle);

    VnMonitorSessionDesc dirtyDesc = MakeSessionDesc();
    dirtyDesc.reserved0 = 1;
    VnMonitorSessionHandle dirtyHandle = 0;
    EXPECT_EQ(VN_INVALID_ARG, Vn_MonitorOpenSession(&dirtyDesc, &dirtyHandle));

    VnMonitorSubscriptionStatus status{};
    status.structSize = sizeof(status);
    EXPECT_EQ(VN_OK, Vn_MonitorGetSubscriptionStatus(handle, &status));
    EXPECT_NE(0u, status.monitorSessionEpoch);
    EXPECT_EQ(VN_MONITOR_PERM_OBSERVE_STATE | VN_MONITOR_PERM_RECORD,
              status.acceptedPermissionMask);
    EXPECT_EQ(0u, status.unsupportedPermissionMask);
    EXPECT_EQ(0u, status.deniedPermissionMask);
    EXPECT_EQ(0u, status.rawAudioDecision);
    EXPECT_EQ(0u, status.symbolsDecision);

    PollArgs dirtyPoll;
    dirtyPoll.desc.reserved0 = 1;
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_MonitorPoll(handle, &dirtyPoll.desc, &dirtyPoll.result));

    PollArgs poll;
    EXPECT_EQ(VN_OK, Vn_MonitorPoll(handle, &poll.desc, &poll.result));
    EXPECT_EQ(0u, poll.result.recordsWritten);
    EXPECT_EQ(0u, poll.result.recordsRemaining);
    EXPECT_EQ(0u, poll.result.bytesWritten);

    uint32_t required = 99;
    EXPECT_EQ(VN_OK, Vn_MonitorGetConsumerStatus(handle, nullptr, 0, &required));
    EXPECT_EQ(0u, required);

    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorCloseSession(handle));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION,
              Vn_MonitorPoll(handle, &poll.desc, &poll.result));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION,
              Vn_MonitorGetSubscriptionStatus(handle, &status));
}

TEST(MonitorHubSession, RecordingWrongThreadBeforeInvalidArg) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    EXPECT_EQ(VN_INVALID_ARG, Vn_MonitorStartRecording(0, 0, nullptr));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorStopRecording(0, 0));
    EXPECT_EQ(VN_INVALID_ARG, Vn_MonitorGetRecordingStatus(0, nullptr));
    std::atomic<VnResult> startResult{VN_OK};
    std::atomic<VnResult> stopResult{VN_OK};
    std::atomic<VnResult> statusResult{VN_OK};
    std::thread worker([&]() {
        startResult.store(Vn_MonitorStartRecording(0, 0, nullptr));
        stopResult.store(Vn_MonitorStopRecording(0, 0));
        statusResult.store(Vn_MonitorGetRecordingStatus(0, nullptr));
    });
    worker.join();
    EXPECT_EQ(VN_MONITOR_WRONG_THREAD, startResult.load());
    EXPECT_EQ(VN_MONITOR_WRONG_THREAD, stopResult.load());
    EXPECT_EQ(VN_MONITOR_WRONG_THREAD, statusResult.load());
}

TEST(MonitorHubSession, ConcurrentPollRejected) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionDesc desc = MakeSessionDesc();
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));

    harken::monitor::ResetPollEnteredForTests();
    harken::monitor::SetPollStallForTests(true);
    std::atomic<VnResult> firstResult{VN_OK};
    std::thread first([&]() {
        PollArgs poll;
        firstResult.store(Vn_MonitorPoll(handle, &poll.desc, &poll.result));
    });
    WaitPollEntered(1);
    PollArgs poll;
    EXPECT_EQ(VN_MONITOR_CONCURRENT_POLL,
              Vn_MonitorPoll(handle, &poll.desc, &poll.result));
    harken::monitor::SetPollStallForTests(false);
    first.join();
    EXPECT_EQ(VN_OK, firstResult.load());
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubSession, ControlApiWrongThread) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    std::atomic<VnResult> result{VN_OK};
    std::thread worker([&]() {
        VnMonitorCapabilities caps{};
        caps.structSize = sizeof(caps);
        result.store(Vn_MonitorGetCapabilities(&caps));
    });
    worker.join();
    EXPECT_EQ(VN_MONITOR_WRONG_THREAD, result.load());
}

TEST(MonitorHubSession, CloseWhilePollInFlightChargesCapabilities) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionDesc desc = MakeSessionDesc();
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));

    VnMonitorCapabilities before{};
    before.structSize = sizeof(before);
    ASSERT_EQ(VN_OK, Vn_MonitorGetCapabilities(&before));
    EXPECT_EQ(VN_MONITOR_HARD_PROCESS_SESSION_SLOTS,
              before.resolvedProcessSessionSlots);
    EXPECT_EQ(VN_MONITOR_HARD_PROCESS_QUEUE_BYTES,
              before.resolvedProcessQueueBytes);

    harken::monitor::ResetPollEnteredForTests();
    harken::monitor::SetPollStallForTests(true);
    std::atomic<VnResult> pollResult{VN_OK};
    std::thread poller([&]() {
        PollArgs poll;
        pollResult.store(Vn_MonitorPoll(handle, &poll.desc, &poll.result));
    });
    WaitPollEntered(1);

    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
    EXPECT_EQ(VN_MONITOR_STALE_SESSION, Vn_MonitorCloseSession(handle));
    PollArgs stale;
    EXPECT_EQ(VN_MONITOR_STALE_SESSION,
              Vn_MonitorPoll(handle, &stale.desc, &stale.result));

    VnMonitorCapabilities during{};
    during.structSize = sizeof(during);
    ASSERT_EQ(VN_OK, Vn_MonitorGetCapabilities(&during));
    EXPECT_EQ(during.hardProcessSessionSlots, VN_MONITOR_HARD_PROCESS_SESSION_SLOTS);
    EXPECT_EQ(during.hardProcessQueueBytes, VN_MONITOR_HARD_PROCESS_QUEUE_BYTES);
    EXPECT_EQ(VN_MONITOR_HARD_PROCESS_SESSION_SLOTS,
              during.resolvedProcessSessionSlots);
    EXPECT_EQ(VN_MONITOR_HARD_PROCESS_QUEUE_BYTES,
              during.resolvedProcessQueueBytes);

    harken::monitor::SetPollStallForTests(false);
    poller.join();
    EXPECT_EQ(VN_OK, pollResult.load());

    VnMonitorCapabilities after{};
    after.structSize = sizeof(after);
    ASSERT_EQ(VN_OK, Vn_MonitorGetCapabilities(&after));
    EXPECT_EQ(after.resolvedProcessSessionSlots, after.hardProcessSessionSlots);
    EXPECT_EQ(after.resolvedProcessQueueBytes, after.hardProcessQueueBytes);
}

TEST(MonitorHubSession, SessionRetirementCap) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionDesc desc = MakeSessionDesc();
    VnMonitorSessionHandle first = 0;
    VnMonitorSessionHandle second = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &first));
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &second));

    harken::monitor::ResetPollEnteredForTests();
    harken::monitor::SetPollStallForTests(true);
    std::vector<std::thread> polls;
    std::atomic<int> finished{0};
    for (VnMonitorSessionHandle handle : {first, second}) {
        polls.emplace_back([handle, &finished]() {
            PollArgs poll;
            (void)Vn_MonitorPoll(handle, &poll.desc, &poll.result);
            finished.fetch_add(1);
        });
    }
    WaitPollEntered(2);

    EXPECT_EQ(VN_OK, Vn_Term());
    PollArgs afterTermPoll;
    const VnResult afterTerm =
        Vn_MonitorPoll(first, &afterTermPoll.desc, &afterTermPoll.result);
    EXPECT_TRUE(afterTerm == VN_NOT_INITIALIZED ||
                afterTerm == VN_MONITOR_STALE_SESSION);

    VnMonitorCapabilities caps{};
    caps.structSize = sizeof(caps);
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_MonitorGetCapabilities(&caps));

    const VnConfig config = harkentest::MakeNullConfig();
    EXPECT_EQ(VN_OK, Vn_Init(&config));
    VnMonitorSessionHandle duringRetired = 0;
    EXPECT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &duringRetired));
    EXPECT_NE(0u, duringRetired);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(duringRetired));
    EXPECT_EQ(VN_OK, Vn_Term());

    harken::monitor::SetPollStallForTests(false);
    for (auto& thread : polls) {
        thread.join();
    }
    EXPECT_EQ(2, finished.load());
    EXPECT_EQ(VN_OK, Vn_Init(&config));
    VnMonitorSessionHandle restored = 0;
    EXPECT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &restored));
    EXPECT_NE(0u, restored);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(restored));
}
