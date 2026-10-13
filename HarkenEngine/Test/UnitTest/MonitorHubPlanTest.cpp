#include <harken/Harken.h>

#include "MonitorCaptureHubTestHooks.h"
#include "Common/TestUtil.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {

VnMonitorSessionDesc MakeObserveDesc() {
    VnMonitorSessionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.requestedStreamKindMask =
        VN_MONITOR_STREAM_TIME_SERIES | VN_MONITOR_STREAM_EVENT;
    desc.requestedPermissionMask =
        VN_MONITOR_PERM_OBSERVE_MEASUREMENTS | VN_MONITOR_PERM_OBSERVE_EVENTS;
    desc.consumerQueueBytes = 8192;
    return desc;
}

VnMonitorSessionDesc MakeControlDesc() {
    VnMonitorSessionDesc desc = MakeObserveDesc();
    desc.requestedPermissionMask =
        VN_MONITOR_PERM_OBSERVE_MEASUREMENTS | VN_MONITOR_PERM_OBSERVE_EVENTS |
        VN_MONITOR_PERM_CAPTURE_CONTROL;
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

struct PollArgs {
    std::vector<unsigned char> buffer;
    VnMonitorPollDesc desc{};
    VnMonitorPollResult result{};

    explicit PollArgs(std::uint64_t bytes = 4096)
        : buffer(static_cast<std::size_t>(bytes)) {
        desc.structSize = sizeof(desc);
        desc.buffer = buffer.data();
        desc.bufferBytes = buffer.size();
        result.structSize = sizeof(result);
    }
};

std::uint32_t ReadU32(const unsigned char* p) {
    return static_cast<std::uint32_t>(p[0]) |
           (static_cast<std::uint32_t>(p[1]) << 8) |
           (static_cast<std::uint32_t>(p[2]) << 16) |
           (static_cast<std::uint32_t>(p[3]) << 24);
}

std::uint64_t ReadU64(const unsigned char* p) {
    return static_cast<std::uint64_t>(ReadU32(p)) |
           (static_cast<std::uint64_t>(ReadU32(p + 4)) << 32);
}

VnResult Acquire(VnMonitorSessionHandle handle,
                 VnMonitorCaptureControlStatus* status) {
    status->structSize = sizeof(*status);
    return Vn_MonitorAcquireCaptureControl(handle, status);
}

VnResult OpenControl(VnMonitorSessionHandle* handle) {
    VnMonitorSessionDesc desc = MakeControlDesc();
    return Vn_MonitorOpenSession(&desc, handle);
}

VnResult OpenObserve(VnMonitorSessionHandle* handle) {
    VnMonitorSessionDesc desc = MakeObserveDesc();
    return Vn_MonitorOpenSession(&desc, handle);
}

} // namespace

TEST(MonitorHubPlan, OpenWithoutLeaseCannotApply) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenControl(&handle));
    VnMonitorCapturePlan plan = MakeArmedPlan();
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    EXPECT_EQ(VN_MONITOR_STALE_LEASE,
              Vn_MonitorApplyCapturePlan(handle, 0, &plan, &accepted));
    VnMonitorEffectiveCapturePlan effective{};
    effective.structSize = sizeof(effective);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(handle, 0, &effective));
    EXPECT_EQ(1u, effective.capturePlanGeneration);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_PLAN_ACTIVE),
              effective.activationState);
    EXPECT_EQ(0u, effective.accepted.streamKindMask);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubPlan, KeepUntilExplicitStopIsZero) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorCapabilities caps{};
    caps.structSize = sizeof(caps);
    ASSERT_EQ(VN_OK, Vn_MonitorGetCapabilities(&caps));
    EXPECT_EQ(0u, caps.keepPlanAfterLeaseTimeout);
    EXPECT_EQ(VN_MONITOR_DEFAULT_PLAN_SLOTS, caps.resolvedPlanSlots);
    EXPECT_EQ(VN_MONITOR_DEDICATED_STOP_SLOTS, caps.dedicatedStopSlots);
}

TEST(MonitorHubPlan, CaptureControlAndRecordAccepted) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionDesc desc = MakeControlDesc();
    desc.requestedPermissionMask |= VN_MONITOR_PERM_RECORD;
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorSubscriptionStatus status{};
    status.structSize = sizeof(status);
    ASSERT_EQ(VN_OK, Vn_MonitorGetSubscriptionStatus(handle, &status));
    EXPECT_NE(0u, status.acceptedPermissionMask & VN_MONITOR_PERM_CAPTURE_CONTROL);
    EXPECT_NE(0u, status.acceptedPermissionMask & VN_MONITOR_PERM_RECORD);
    EXPECT_EQ(0u, status.unsupportedPermissionMask & VN_MONITOR_PERM_RECORD);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubPlan, ObserveSessionDeniedAcquire) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenObserve(&handle));
    VnMonitorCaptureControlStatus lease{};
    EXPECT_EQ(VN_MONITOR_PERMISSION_DENIED, Acquire(handle, &lease));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubPlan, AcquireReleaseThenAcquireIncrementsGeneration) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenControl(&handle));
    VnMonitorCaptureControlStatus first{};
    ASSERT_EQ(VN_OK, Acquire(handle, &first));
    EXPECT_EQ(1u, first.leaseGeneration);
    EXPECT_EQ(1u, first.held);
    EXPECT_NE(0u, first.principalHash);
    EXPECT_EQ(VN_OK, Vn_MonitorReleaseCaptureControl(handle, first.leaseGeneration));
    VnMonitorCaptureControlStatus after{};
    after.structSize = sizeof(after);
    ASSERT_EQ(VN_OK, Vn_MonitorGetCaptureControlStatus(handle, &after));
    EXPECT_EQ(1u, after.leaseGeneration);
    EXPECT_EQ(0u, after.held);
    VnMonitorCaptureControlStatus second{};
    ASSERT_EQ(VN_OK, Acquire(handle, &second));
    EXPECT_EQ(2u, second.leaseGeneration);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubPlan, OtherSessionCannotUseCurrentLease) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle owner = 0;
    VnMonitorSessionHandle other = 0;
    ASSERT_EQ(VN_OK, OpenControl(&owner));
    ASSERT_EQ(VN_OK, OpenControl(&other));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(owner, &lease));
    VnMonitorCapturePlan plan = MakeArmedPlan();
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    EXPECT_EQ(VN_MONITOR_STALE_LEASE,
              Vn_MonitorApplyCapturePlan(other, lease.leaseGeneration, &plan,
                                         &accepted));
    EXPECT_EQ(VN_IN_USE, Acquire(other, &lease));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(owner));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(other));
}

TEST(MonitorHubPlan, PumpActivatesPendingPlan) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorCapturePlan plan = MakeArmedPlan();
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration,
                                                &plan, &accepted));
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_PLAN_PENDING),
              accepted.activationState);
    VnMonitorEffectiveCapturePlan pending{};
    pending.structSize = sizeof(pending);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(handle, 0, &pending));
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_PLAN_PENDING),
              pending.activationState);
    EXPECT_EQ(2u, pending.capturePlanGeneration);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(handle, 2, &pending));
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_PLAN_PENDING),
              pending.activationState);
    VnMonitorEffectiveCapturePlan stillActive{};
    stillActive.structSize = sizeof(stillActive);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(handle, 1, &stillActive));
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_PLAN_ACTIVE),
              stillActive.activationState);
    EXPECT_EQ(1u, stillActive.capturePlanGeneration);
    EXPECT_EQ(0u, stillActive.accepted.streamKindMask);
    const auto beforePump = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - beforePump <
           std::chrono::milliseconds(50)) {
        ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(handle, 0, &pending));
        EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_PLAN_PENDING),
                  pending.activationState);
        std::this_thread::yield();
    }
    engine.Engine()->nullBackend.Pump(1);
    const auto start = std::chrono::steady_clock::now();
    VnMonitorEffectiveCapturePlan effective{};
    effective.structSize = sizeof(effective);
    while (std::chrono::steady_clock::now() - start <
           std::chrono::seconds(2)) {
        ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(handle, 0, &effective));
        if (effective.activationState ==
                static_cast<std::uint32_t>(VN_MONITOR_PLAN_ACTIVE) &&
            effective.capturePlanGeneration == 2u) {
            break;
        }
        std::this_thread::yield();
    }
    EXPECT_EQ(2u, effective.capturePlanGeneration);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_PLAN_ACTIVE),
              effective.activationState);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubPlan, ApplyThenAdvanceActivatesAndStopEmpties) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorCapturePlan plan = MakeArmedPlan();
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration,
                                                &plan, &accepted));
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_PLAN_PENDING),
              accepted.activationState);
    harken::monitor::AdvanceExecuteFrameForTests(1);
    VnMonitorEffectiveCapturePlan effective{};
    effective.structSize = sizeof(effective);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(handle, 0, &effective));
    EXPECT_EQ(2u, effective.capturePlanGeneration);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_PLAN_ACTIVE),
              effective.activationState);
    EXPECT_EQ(VN_MONITOR_STREAM_TIME_SERIES, effective.accepted.streamKindMask);
    VnMonitorEffectiveCapturePlan stopped{};
    stopped.structSize = sizeof(stopped);
    ASSERT_EQ(VN_OK, Vn_MonitorStopCapture(handle, lease.leaseGeneration, &stopped));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(handle, 0, &effective));
    EXPECT_EQ(3u, effective.capturePlanGeneration);
    EXPECT_EQ(0u, effective.accepted.streamKindMask);
    ASSERT_EQ(VN_OK, Vn_MonitorStopCapture(handle, lease.leaseGeneration, &stopped));
    EXPECT_EQ(3u, stopped.capturePlanGeneration);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubPlan, ApplyFailsKeepsOriginalPlan) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorCapturePlan plan = MakeArmedPlan();
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration,
                                                &plan, &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    VnMonitorCapturePlan bad{};
    bad.structSize = sizeof(bad);
    bad.reserved0 = 1;
    EXPECT_EQ(VN_INVALID_ARG, Vn_MonitorApplyCapturePlan(
                                  handle, lease.leaseGeneration, &bad, &accepted));
    VnMonitorEffectiveCapturePlan effective{};
    effective.structSize = sizeof(effective);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(handle, 0, &effective));
    EXPECT_EQ(2u, effective.capturePlanGeneration);
    EXPECT_EQ(VN_MONITOR_STREAM_TIME_SERIES, effective.accepted.streamKindMask);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubPlan, FullNormalSlotsRejectApplyButStopSucceeds) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    harken::monitor::HoldPlanAckForTests(true);
    VnMonitorCapturePlan plan = MakeArmedPlan();
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration,
                                                &plan, &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    harken::monitor::SetAbandonedPlanWorkerForTests();
    for (std::uint32_t i = 1; i < VN_MONITOR_DEFAULT_PLAN_SLOTS; ++i) {
        ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration,
                                                    &plan, &accepted));
        harken::monitor::AdvanceExecuteFrameForTests(1);
    }
    EXPECT_EQ(VN_MONITOR_PLAN_CAPACITY,
              Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                         &accepted));
    VnMonitorEffectiveCapturePlan before{};
    before.structSize = sizeof(before);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(handle, 0, &before));
    VnMonitorEffectiveCapturePlan stopped{};
    stopped.structSize = sizeof(stopped);
    ASSERT_EQ(VN_OK, Vn_MonitorStopCapture(handle, lease.leaseGeneration, &stopped));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    VnMonitorEffectiveCapturePlan after{};
    after.structSize = sizeof(after);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(handle, 0, &after));
    EXPECT_GT(after.capturePlanGeneration, before.capturePlanGeneration);
    EXPECT_EQ(0u, after.accepted.streamKindMask);
    harken::monitor::HoldPlanAckForTests(false);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubPlan, AbandonedSlotNotReused) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorCapturePlan plan = MakeArmedPlan();
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration,
                                                &plan, &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    harken::monitor::SetAbandonedPlanWorkerForTests();
    harken::monitor::HoldPlanAckForTests(true);
    for (std::uint32_t i = 1; i < VN_MONITOR_DEFAULT_PLAN_SLOTS; ++i) {
        ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration,
                                                    &plan, &accepted));
        harken::monitor::AdvanceExecuteFrameForTests(1);
    }
    EXPECT_EQ(VN_MONITOR_PLAN_CAPACITY,
              Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                         &accepted));
    harken::monitor::HoldPlanAckForTests(false);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration,
                                                &plan, &accepted));
    harken::monitor::HoldPlanAckForTests(true);
    harken::monitor::AdvanceExecuteFrameForTests(1);
    for (std::uint32_t i = 1; i < VN_MONITOR_DEFAULT_PLAN_SLOTS; ++i) {
        const VnResult applied = Vn_MonitorApplyCapturePlan(
            handle, lease.leaseGeneration, &plan, &accepted);
        if (applied != VN_OK) {
            EXPECT_EQ(VN_MONITOR_PLAN_CAPACITY, applied);
            break;
        }
        harken::monitor::AdvanceExecuteFrameForTests(1);
    }
    EXPECT_EQ(VN_MONITOR_PLAN_CAPACITY,
              Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                         &accepted));
    harken::monitor::HoldPlanAckForTests(false);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubPlan, MixedGenerationKeepsOriginalLabels) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorCapturePlan plan = MakeArmedPlan();
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration,
                                                &plan, &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    harken::monitor::StopDrainForTests();
    ASSERT_TRUE(harken::monitor::PublishSyntheticTimeSeriesForTests(1));
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration,
                                                &plan, &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    ASSERT_TRUE(harken::monitor::PublishSyntheticTimeSeriesForTests(1));
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();
    std::vector<std::uint64_t> generations;
    for (int i = 0; i < 8 && generations.size() < 2; ++i) {
        PollArgs poll;
        ASSERT_EQ(VN_OK, Vn_MonitorPoll(handle, &poll.desc, &poll.result));
        if (poll.result.recordsWritten == 0 || poll.result.bytesWritten < 4u) {
            break;
        }
        std::uint64_t offset = 0;
        while (offset + 88u <= poll.result.bytesWritten) {
            const unsigned char* rec = poll.buffer.data() + offset;
            const std::uint32_t kind = ReadU32(rec);
            const std::uint64_t recordBytes = ReadU64(rec + 16);
            if (kind == static_cast<std::uint32_t>(VN_MONITOR_RECORD_TIME_SERIES)) {
                generations.push_back(ReadU64(rec + 80));
            }
            if (recordBytes == 0 || offset + recordBytes > poll.result.bytesWritten) {
                break;
            }
            offset += recordBytes;
        }
    }
    ASSERT_EQ(2u, generations.size());
    EXPECT_EQ(2u, generations[0]);
    EXPECT_EQ(3u, generations[1]);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubPlan, DroppedActivationRecoveredFromEffectivePlan) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    harken::monitor::DropNextActivationRecordForTests(true);
    VnMonitorCapturePlan plan = MakeArmedPlan();
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration,
                                                &plan, &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    VnMonitorEffectiveCapturePlan effective{};
    effective.structSize = sizeof(effective);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(handle, 0, &effective));
    EXPECT_EQ(2u, effective.capturePlanGeneration);
    EXPECT_EQ(VN_MONITOR_STREAM_TIME_SERIES, effective.accepted.streamKindMask);
    VnMonitorDropSummary summaries[4]{};
    uint32_t required = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorGetConsumerStatus(handle, summaries, 4, &required));
    bool sawProducerDrop = false;
    for (uint32_t i = 0; i < required; ++i) {
        if (summaries[i].stage == VN_MONITOR_DROP_SOURCE_PRODUCER &&
            summaries[i].recordCount > 0) {
            sawProducerDrop = true;
        }
    }
    EXPECT_TRUE(sawProducerDrop);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubPlan, ApplyWithTapsRejected) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorCapturePlan plan = MakeArmedPlan();
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration,
                                                &plan, &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    plan.tapCount = 1;
    plan.taps = &tap;
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED,
              Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                         &accepted));
    VnMonitorEffectiveCapturePlan effective{};
    effective.structSize = sizeof(effective);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(handle, 0, &effective));
    EXPECT_EQ(2u, effective.capturePlanGeneration);
    EXPECT_EQ(VN_MONITOR_STREAM_TIME_SERIES, effective.accepted.streamKindMask);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubPlan, StopActivationReasonIsStop) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorCapturePlan plan = MakeArmedPlan();
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration,
                                                &plan, &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    VnMonitorEffectiveCapturePlan stopped{};
    stopped.structSize = sizeof(stopped);
    harken::monitor::StopDrainForTests();
    ASSERT_EQ(VN_OK, Vn_MonitorStopCapture(handle, lease.leaseGeneration, &stopped));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();
    bool sawStop = false;
    std::uint32_t lastKind = 0;
    std::uint32_t lastReason = 0;
    std::uint64_t lastWritten = 0;
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(handle, &poll.desc, &poll.result));
    lastWritten = poll.result.bytesWritten;
    std::uint64_t offset = 0;
    while (offset + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES <=
           poll.result.bytesWritten) {
        const unsigned char* rec = poll.buffer.data() + offset;
        const std::uint32_t kind = ReadU32(rec);
        const std::uint32_t reason = ReadU32(rec + 248);
        const std::uint64_t recordBytes = ReadU64(rec + 16);
        lastKind = kind;
        lastReason = reason;
        if (kind == static_cast<std::uint32_t>(
                        VN_MONITOR_RECORD_CAPTURE_PLAN_ACTIVATED) &&
            reason == static_cast<std::uint32_t>(VN_MONITOR_EVENT_STOP)) {
            sawStop = true;
            break;
        }
        if (recordBytes == 0 ||
            offset + recordBytes > poll.result.bytesWritten) {
            break;
        }
        offset += recordBytes;
    }
    EXPECT_TRUE(sawStop) << "bytes=" << lastWritten << " kind=" << lastKind
                         << " reason=" << lastReason;
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubPlan, ExpiryAfterAdvanceMakesOldLeaseStaleWithoutChangingPlan) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorCapturePlan plan = MakeArmedPlan();
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration,
                                                &plan, &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    harken::monitor::SetLeaseExpiryExecuteFrameForTests(2);
    harken::monitor::AdvanceExecuteFrameForTests(1);
    VnMonitorEffectiveCapturePlan stopped{};
    stopped.structSize = sizeof(stopped);
    EXPECT_EQ(VN_MONITOR_STALE_LEASE,
              Vn_MonitorStopCapture(handle, lease.leaseGeneration, &stopped));
    VnMonitorEffectiveCapturePlan effective{};
    effective.structSize = sizeof(effective);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(handle, 0, &effective));
    EXPECT_EQ(2u, effective.capturePlanGeneration);
    EXPECT_EQ(VN_MONITOR_STREAM_TIME_SERIES, effective.accepted.streamKindMask);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubPlan, PendingStopBlocksNewApplyThenExpiryKeepsStop) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    VnMonitorCapturePlan plan = MakeArmedPlan();
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration,
                                                &plan, &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    VnMonitorEffectiveCapturePlan stopped{};
    stopped.structSize = sizeof(stopped);
    ASSERT_EQ(VN_OK, Vn_MonitorStopCapture(handle, lease.leaseGeneration, &stopped));
    EXPECT_EQ(VN_IN_USE, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration,
                                                    &plan, &accepted));
    harken::monitor::SetLeaseExpiryExecuteFrameForTests(
        harken::monitor::ExecuteFrameForTests());
    harken::monitor::AdvanceExecuteFrameForTests(1);
    VnMonitorEffectiveCapturePlan effective{};
    effective.structSize = sizeof(effective);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(handle, 0, &effective));
    EXPECT_EQ(0u, effective.accepted.streamKindMask);
    EXPECT_EQ(VN_MONITOR_STALE_LEASE,
              Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                         &accepted));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubPlan, PendingApplyDroppedWhenExpiryHitsSameQuantum) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(handle, &lease));
    harken::monitor::SetLeaseExpiryExecuteFrameForTests(1);
    VnMonitorCapturePlan plan = MakeArmedPlan();
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration,
                                                &plan, &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    VnMonitorEffectiveCapturePlan effective{};
    effective.structSize = sizeof(effective);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(handle, 0, &effective));
    EXPECT_EQ(1u, effective.capturePlanGeneration);
    EXPECT_EQ(0u, effective.accepted.streamKindMask);
    EXPECT_EQ(VN_MONITOR_STALE_LEASE,
              Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                         &accepted));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubPlan, ReleasedApplyPendingDoesNotActivateForNewLease) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle first = 0;
    VnMonitorSessionHandle second = 0;
    ASSERT_EQ(VN_OK, OpenControl(&first));
    ASSERT_EQ(VN_OK, OpenControl(&second));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(first, &lease));
    VnMonitorCapturePlan plan = MakeArmedPlan();
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(first, lease.leaseGeneration,
                                                &plan, &accepted));
    ASSERT_EQ(VN_OK, Vn_MonitorReleaseCaptureControl(first, lease.leaseGeneration));
    VnMonitorCaptureControlStatus next{};
    ASSERT_EQ(VN_OK, Acquire(second, &next));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    VnMonitorEffectiveCapturePlan effective{};
    effective.structSize = sizeof(effective);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(second, 0, &effective));
    EXPECT_EQ(1u, effective.capturePlanGeneration);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_PLAN_ACTIVE),
              effective.activationState);
    EXPECT_EQ(0u, effective.accepted.streamKindMask);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(second, next.leaseGeneration,
                                                &plan, &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(second, 0, &effective));
    EXPECT_EQ(VN_MONITOR_STREAM_TIME_SERIES, effective.accepted.streamKindMask);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(first));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(second));
}

TEST(MonitorHubPlan, NewLeaseCannotApplyWhileStopPendingThenCanAfterEmpty) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle first = 0;
    VnMonitorSessionHandle second = 0;
    ASSERT_EQ(VN_OK, OpenControl(&first));
    ASSERT_EQ(VN_OK, OpenControl(&second));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(first, &lease));
    VnMonitorCapturePlan plan = MakeArmedPlan();
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(first, lease.leaseGeneration,
                                                &plan, &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    VnMonitorEffectiveCapturePlan stopped{};
    stopped.structSize = sizeof(stopped);
    ASSERT_EQ(VN_OK, Vn_MonitorStopCapture(first, lease.leaseGeneration, &stopped));
    ASSERT_EQ(VN_OK, Vn_MonitorReleaseCaptureControl(first, lease.leaseGeneration));
    VnMonitorCaptureControlStatus next{};
    ASSERT_EQ(VN_OK, Acquire(second, &next));
    EXPECT_EQ(VN_IN_USE, Vn_MonitorApplyCapturePlan(second, next.leaseGeneration,
                                                    &plan, &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    VnMonitorEffectiveCapturePlan effective{};
    effective.structSize = sizeof(effective);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(second, 0, &effective));
    EXPECT_EQ(0u, effective.accepted.streamKindMask);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(second, next.leaseGeneration,
                                                &plan, &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(second, 0, &effective));
    EXPECT_EQ(VN_MONITOR_STREAM_TIME_SERIES, effective.accepted.streamKindMask);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(first));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(second));
}

TEST(MonitorHubPlan, WrongThreadIsFirstFailure) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenControl(&handle));
    std::atomic<VnResult> result{VN_OK};
    std::thread worker([&]() {
        VnMonitorCaptureControlStatus lease{};
        lease.structSize = sizeof(lease);
        result.store(Vn_MonitorAcquireCaptureControl(handle, &lease));
    });
    worker.join();
    EXPECT_EQ(VN_MONITOR_WRONG_THREAD, result.load());
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}
