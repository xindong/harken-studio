#include <harken/Harken.h>

#include "MonitorCaptureHubTestHooks.h"
#include "MonitorIdentityCodec.h"
#include "Common/TestUtil.h"
#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <thread>

namespace {

TEST(MonitorHubSubscription, ExecutionNegotiationIsQueryableWithoutCatalogMode) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionDescV2 desc{};
    desc.structSize = sizeof(desc);
    desc.requestedExecutionFactVersion = 1;
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_EVENT | VN_MONITOR_STREAM_STATE;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_EVENTS | VN_MONITOR_PERM_OBSERVE_STATE;
    VnMonitorSessionHandle session = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(reinterpret_cast<const VnMonitorSessionDesc*>(&desc), &session));
    struct LegacyBuffer {
        VnMonitorSubscriptionStatus value{};
        std::uint64_t canary = 0xa5a512345555ffffull;
    } old;
    old.value.structSize = sizeof(old.value);
    ASSERT_EQ(VN_OK, Vn_MonitorGetSubscriptionStatus(session, &old.value));
    EXPECT_EQ(0xa5a512345555ffffull, old.canary);
    EXPECT_EQ(sizeof(old.value), old.value.structSize);
    VnMonitorSubscriptionStatusV2 status{};
    status.structSize = sizeof(status);
    ASSERT_EQ(VN_OK, Vn_MonitorGetSubscriptionStatus(session,
        reinterpret_cast<VnMonitorSubscriptionStatus*>(&status)));
    EXPECT_EQ(sizeof(status), status.structSize);
    EXPECT_EQ(1u, status.acceptedExecutionFactVersion);
    EXPECT_EQ(old.value.monitorSessionEpoch, status.monitorSessionEpoch);
    EXPECT_EQ(old.value.acceptedPermissionMask, status.acceptedPermissionMask);
    EXPECT_EQ(2u, status.baselineCount);
    EXPECT_EQ(0u, status.reserved0);
    std::array<VnMonitorStreamBaseline, 2> baseline{};
    status.baselineCapacity = 2;
    status.baselines = baseline.data();
    ASSERT_EQ(VN_OK, Vn_MonitorGetSubscriptionStatus(session,
        reinterpret_cast<VnMonitorSubscriptionStatus*>(&status)));
    EXPECT_EQ(baseline.data(), status.baselines);
    EXPECT_EQ(VN_MONITOR_STREAM_STATE, baseline[0].streamKind);
    EXPECT_EQ(harken::monitor::kIdentityDeltaStream, baseline[0].streamId);
    EXPECT_EQ(harken::monitor::kIdentityBaselineStream, baseline[1].streamId);
    EXPECT_NE(0u, baseline[0].firstExpectedSourceSequence);
    EXPECT_NE(0u, baseline[1].firstExpectedSourceSequence);
    status.structSize -= 1;
    EXPECT_EQ(VN_INVALID_ARG, Vn_MonitorGetSubscriptionStatus(session,
        reinterpret_cast<VnMonitorSubscriptionStatus*>(&status)));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(session));
}

VnMonitorSessionDesc MakeDesc(std::uint32_t streamMask, std::uint32_t perm,
                              std::uint64_t queueBytes = 4096) {
    VnMonitorSessionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.requestedStreamKindMask = streamMask;
    desc.requestedPermissionMask = perm;
    desc.consumerQueueBytes = queueBytes;
    return desc;
}

std::uint32_t PackedBytes(std::uint32_t payloadBytes) {
    const std::uint32_t total = VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES + payloadBytes;
    return (total + 7u) & ~7u;
}

std::uint32_t PackedBeginBytes() {
    return PackedBytes(static_cast<std::uint32_t>(sizeof(VnMonitorStateCutBegin)));
}

std::uint32_t PackedEndBytes() {
    return PackedBytes(static_cast<std::uint32_t>(sizeof(VnMonitorStateCutEnd)));
}

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

struct LiveCutRecord {
    std::uint32_t recordKind = 0;
    std::uint64_t sourceSequence = 0;
    std::uint64_t recordBytes = 0;
    std::uint64_t payloadBytes = 0;
    const unsigned char* payload = nullptr;
};

bool ReadLiveCutRecord(const unsigned char* bytes, std::uint64_t available,
                       LiveCutRecord* out) {
    if (bytes == nullptr || out == nullptr ||
        available < VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES) {
        return false;
    }
    out->recordKind = ReadU32(bytes);
    out->recordBytes = ReadU64(bytes + 16);
    out->sourceSequence = ReadU64(bytes + 40);
    if (out->recordBytes < VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES ||
        out->recordBytes > available) {
        return false;
    }
    out->payloadBytes = out->recordBytes - VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES;
    out->payload = bytes + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES;
    return true;
}

std::uint64_t HubFanoutDropCount(VnMonitorSessionHandle handle) {
    VnMonitorDropSummary summaries[4]{};
    uint32_t required = 0;
    if (Vn_MonitorGetConsumerStatus(handle, summaries, 4, &required) != VN_OK) {
        return 0;
    }
    for (uint32_t i = 0; i < required; ++i) {
        if (summaries[i].stage == VN_MONITOR_DROP_HUB_FANOUT) {
            return summaries[i].recordCount;
        }
    }
    return 0;
}

struct PollArgs {
    std::array<unsigned char, 8192> buffer{};
    VnMonitorPollDesc desc{};
    VnMonitorPollResult result{};

    PollArgs() {
        desc.structSize = sizeof(desc);
        desc.buffer = buffer.data();
        desc.bufferBytes = buffer.size();
        result.structSize = sizeof(result);
    }
};

} // namespace

TEST(MonitorHubSubscription, OpenWithoutPrincipalOverrideStillSucceeds) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionDesc desc =
        MakeDesc(VN_MONITOR_STREAM_STATE, VN_MONITOR_PERM_OBSERVE_STATE);
    VnMonitorSessionHandle handle = 0;
    EXPECT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    EXPECT_NE(0u, handle);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubSubscription, DeniedPrincipalDoesNotOpen) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::SetHostPrincipalForTests(false, 0);
    VnMonitorSessionDesc desc =
        MakeDesc(VN_MONITOR_STREAM_STATE, VN_MONITOR_PERM_OBSERVE_STATE);
    VnMonitorSessionHandle handle = 1;
    EXPECT_EQ(VN_MONITOR_PERMISSION_DENIED, Vn_MonitorOpenSession(&desc, &handle));
    EXPECT_EQ(1u, handle);
    harken::monitor::ClearHostPrincipalForTests();
}

TEST(MonitorHubSubscription, UnsubscribedKindHasNoFalseGap) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionDesc stateOnly =
        MakeDesc(VN_MONITOR_STREAM_STATE, VN_MONITOR_PERM_OBSERVE_STATE);
    VnMonitorSessionDesc series =
        MakeDesc(VN_MONITOR_STREAM_TIME_SERIES,
                 VN_MONITOR_PERM_OBSERVE_MEASUREMENTS);
    VnMonitorSessionHandle a = 0;
    VnMonitorSessionHandle b = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&stateOnly, &a));
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&series, &b));

    VnMonitorStreamBaseline baselines[2]{};
    VnMonitorSubscriptionStatus status{};
    status.structSize = sizeof(status);
    status.baselines = baselines;
    status.baselineCapacity = 2;
    ASSERT_EQ(VN_OK, Vn_MonitorGetSubscriptionStatus(a, &status));
    EXPECT_EQ(1u, status.baselineCount);
    EXPECT_EQ(2u, baselines[0].streamId);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_STREAM_STATE),
              baselines[0].streamKind);
    EXPECT_EQ(VN_MONITOR_STREAM_STATE, status.resolvedStreamKindMask);
    const std::uint64_t frozenState = baselines[0].firstExpectedSourceSequence;
    EXPECT_NE(0u, frozenState);

    status = {};
    status.structSize = sizeof(status);
    status.baselines = baselines;
    status.baselineCapacity = 2;
    ASSERT_EQ(VN_OK, Vn_MonitorGetSubscriptionStatus(b, &status));
    EXPECT_EQ(1u, status.baselineCount);
    EXPECT_EQ(1u, baselines[0].streamId);
    const std::uint64_t frozenSeries = baselines[0].firstExpectedSourceSequence;
    EXPECT_NE(0u, frozenSeries);

    harken::monitor::StopDrainForTests();
    ASSERT_TRUE(harken::monitor::PublishSyntheticTimeSeriesForTests(1));
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();

    status = {};
    status.structSize = sizeof(status);
    status.baselines = baselines;
    status.baselineCapacity = 2;
    ASSERT_EQ(VN_OK, Vn_MonitorGetSubscriptionStatus(b, &status));
    EXPECT_EQ(frozenSeries, baselines[0].firstExpectedSourceSequence);
    status = {};
    status.structSize = sizeof(status);
    status.baselines = baselines;
    status.baselineCapacity = 2;
    ASSERT_EQ(VN_OK, Vn_MonitorGetSubscriptionStatus(a, &status));
    EXPECT_EQ(frozenState, baselines[0].firstExpectedSourceSequence);

    PollArgs pollA;
    PollArgs pollB;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(a, &pollA.desc, &pollA.result));
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(b, &pollB.desc, &pollB.result));
    EXPECT_EQ(0u, pollA.result.recordsWritten);
    EXPECT_EQ(1u, pollB.result.recordsWritten);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(a));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(b));
}

TEST(MonitorHubSubscription, DualSubscribeBaselinesStayFrozenAtOpen) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionDesc desc = MakeDesc(
        VN_MONITOR_STREAM_STATE | VN_MONITOR_STREAM_TIME_SERIES,
        VN_MONITOR_PERM_OBSERVE_STATE | VN_MONITOR_PERM_OBSERVE_MEASUREMENTS);
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorStreamBaseline baselines[2]{};
    VnMonitorSubscriptionStatus status{};
    status.structSize = sizeof(status);
    status.baselines = baselines;
    status.baselineCapacity = 2;
    ASSERT_EQ(VN_OK, Vn_MonitorGetSubscriptionStatus(handle, &status));
    ASSERT_EQ(2u, status.baselineCount);
    const std::uint64_t frozenSeries = baselines[0].firstExpectedSourceSequence;
    const std::uint64_t frozenState = baselines[1].firstExpectedSourceSequence;
    EXPECT_EQ(1u, baselines[0].streamId);
    EXPECT_EQ(2u, baselines[1].streamId);

    harken::monitor::StopDrainForTests();
    ASSERT_TRUE(harken::monitor::PublishSyntheticTimeSeriesForTests(1));
    VnMonitorStateCutDesc cutDesc{};
    cutDesc.structSize = sizeof(cutDesc);
    uint64_t cutId = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorRequestStateCut(handle, &cutDesc, &cutId));
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();

    status = {};
    status.structSize = sizeof(status);
    status.baselines = baselines;
    status.baselineCapacity = 2;
    ASSERT_EQ(VN_OK, Vn_MonitorGetSubscriptionStatus(handle, &status));
    EXPECT_EQ(frozenSeries, baselines[0].firstExpectedSourceSequence);
    EXPECT_EQ(frozenState, baselines[1].firstExpectedSourceSequence);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubSubscription, BaselineBufferTooSmallDoesNotTruncate) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionDesc desc =
        MakeDesc(VN_MONITOR_STREAM_TIME_SERIES,
                 VN_MONITOR_PERM_OBSERVE_MEASUREMENTS);
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorSubscriptionStatus status{};
    status.structSize = sizeof(status);
    VnMonitorStreamBaseline one{};
    status.baselines = &one;
    status.baselineCapacity = 0;
    EXPECT_EQ(VN_MONITOR_BUFFER_TOO_SMALL,
              Vn_MonitorGetSubscriptionStatus(handle, &status));
    EXPECT_EQ(1u, status.baselineCount);
    EXPECT_EQ(0u, one.streamId);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubSubscription, CompleteStateCutIsAuthoritative) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const std::uint64_t queueBytes = PackedBeginBytes() + PackedEndBytes() + 512;
    VnMonitorSessionDesc desc =
        MakeDesc(VN_MONITOR_STREAM_STATE, VN_MONITOR_PERM_OBSERVE_STATE, queueBytes);
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    harken::monitor::StopDrainForTests();
    harken::monitor::DrainOnceForTests();
    VnMonitorStateCutDesc cutDesc{};
    cutDesc.structSize = sizeof(cutDesc);
    uint64_t cutId = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorRequestStateCut(handle, &cutDesc, &cutId));
    EXPECT_NE(0u, cutId);
    harken::monitor::DrainOnceForTests();
    VnMonitorStateCutStatus cutStatus{};
    cutStatus.structSize = sizeof(cutStatus);
    ASSERT_EQ(VN_OK, Vn_MonitorGetStateCutStatus(handle, cutId, &cutStatus));
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_STATE_CUT_COMPLETE),
              cutStatus.result);
    EXPECT_EQ(2u, cutStatus.emittedCount);
    EXPECT_NE(0u, cutStatus.checksum);
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(handle, &poll.desc, &poll.result));
    EXPECT_GE(poll.result.recordsWritten, 2u);
    LiveCutRecord recs[8]{};
    std::uint64_t offset = 0;
    std::uint32_t n = 0;
    while (offset < poll.result.bytesWritten && n < 8) {
        ASSERT_TRUE(ReadLiveCutRecord(poll.buffer.data() + offset,
                                      poll.result.bytesWritten - offset, &recs[n]));
        offset += recs[n].recordBytes;
        ++n;
    }
    bool sawBegin = false;
    bool sawEnd = false;
    bool sawUpsert = false;
    const VnMonitorStateCutBegin* begin = nullptr;
    const VnMonitorStateCutEnd* end = nullptr;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (recs[i].recordKind == VN_MONITOR_RECORD_STATE_CUT_BEGIN) {
            sawBegin = true;
            begin = reinterpret_cast<const VnMonitorStateCutBegin*>(recs[i].payload);
        } else if (recs[i].recordKind == VN_MONITOR_RECORD_STATE_CUT_END) {
            sawEnd = true;
            end = reinterpret_cast<const VnMonitorStateCutEnd*>(recs[i].payload);
        } else if (recs[i].recordKind == VN_MONITOR_RECORD_STATE_UPSERT) {
            sawUpsert = true;
        }
    }
    EXPECT_TRUE(sawBegin);
    EXPECT_TRUE(sawEnd);
    EXPECT_FALSE(sawUpsert);
    ASSERT_NE(nullptr, begin);
    ASSERT_NE(nullptr, end);
    EXPECT_EQ(cutId, begin->stateCutId);
    EXPECT_EQ(0u, begin->entityTrueCount[VN_MONITOR_ENTITY_VOICE]);
    EXPECT_EQ(cutId, end->stateCutId);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_STATE_CUT_COMPLETE),
              end->result);
    EXPECT_EQ(cutStatus.checksum, end->checksum);
    harken::monitor::StartDrainForTests();
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubSubscription, DroppedEndIsTruncatedNotDelete) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const std::uint64_t queueBytes = PackedBeginBytes();
    VnMonitorSessionDesc desc =
        MakeDesc(VN_MONITOR_STREAM_STATE, VN_MONITOR_PERM_OBSERVE_STATE, queueBytes);
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    harken::monitor::StopDrainForTests();
    VnMonitorStateCutDesc cutDesc{};
    cutDesc.structSize = sizeof(cutDesc);
    uint64_t cutId = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorRequestStateCut(handle, &cutDesc, &cutId));
    harken::monitor::DrainOnceForTests();
    VnMonitorStateCutStatus cutStatus{};
    cutStatus.structSize = sizeof(cutStatus);
    ASSERT_EQ(VN_OK, Vn_MonitorGetStateCutStatus(handle, cutId, &cutStatus));
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_STATE_CUT_TRUNCATED),
              cutStatus.result);
    EXPECT_EQ(1u, cutStatus.emittedCount);
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(handle, &poll.desc, &poll.result));
    EXPECT_GE(poll.result.recordsWritten, 1u);
    LiveCutRecord recs[4]{};
    std::uint64_t offset = 0;
    std::uint32_t n = 0;
    while (offset < poll.result.bytesWritten && n < 4) {
        ASSERT_TRUE(ReadLiveCutRecord(poll.buffer.data() + offset,
                                      poll.result.bytesWritten - offset, &recs[n]));
        offset += recs[n].recordBytes;
        ++n;
    }
    bool sawEnd = false;
    bool sawBegin = false;
    for (std::uint32_t i = 0; i < n; ++i) {
        if (recs[i].recordKind == VN_MONITOR_RECORD_STATE_CUT_END) sawEnd = true;
        if (recs[i].recordKind == VN_MONITOR_RECORD_STATE_CUT_BEGIN) sawBegin = true;
    }
    EXPECT_TRUE(sawBegin);
    EXPECT_FALSE(sawEnd);
    harken::monitor::StartDrainForTests();
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubSubscription, HintTooSmallAbortsWithoutBegin) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionDesc desc =
        MakeDesc(VN_MONITOR_STREAM_STATE, VN_MONITOR_PERM_OBSERVE_STATE);
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorStateCutDesc cutDesc{};
    cutDesc.structSize = sizeof(cutDesc);
    cutDesc.requestedCapacityHint = 1;
    uint64_t cutId = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorRequestStateCut(handle, &cutDesc, &cutId));
    VnMonitorStateCutStatus cutStatus{};
    cutStatus.structSize = sizeof(cutStatus);
    ASSERT_EQ(VN_OK, Vn_MonitorGetStateCutStatus(handle, cutId, &cutStatus));
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_STATE_CUT_ABORTED),
              cutStatus.result);
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(handle, &poll.desc, &poll.result));
    EXPECT_EQ(0u, poll.result.recordsWritten);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubSubscription, StateCutDoesNotLeakToOtherStateSession) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const std::uint64_t queueBytes =
        PackedBeginBytes() + PackedEndBytes();
    VnMonitorSessionDesc desc =
        MakeDesc(VN_MONITOR_STREAM_STATE, VN_MONITOR_PERM_OBSERVE_STATE, queueBytes);
    VnMonitorSessionHandle a = 0;
    VnMonitorSessionHandle b = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &a));
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &b));
    VnMonitorStateCutDesc cutDesc{};
    cutDesc.structSize = sizeof(cutDesc);
    uint64_t cutId = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorRequestStateCut(a, &cutDesc, &cutId));
    harken::monitor::StopDrainForTests();
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();
    VnMonitorStateCutStatus cutStatus{};
    cutStatus.structSize = sizeof(cutStatus);
    ASSERT_EQ(VN_OK, Vn_MonitorGetStateCutStatus(a, cutId, &cutStatus));
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_STATE_CUT_COMPLETE),
              cutStatus.result);
    EXPECT_EQ(VN_MONITOR_STALE_SESSION,
              Vn_MonitorGetStateCutStatus(b, cutId, &cutStatus));
    PollArgs pollA;
    PollArgs pollB;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(a, &pollA.desc, &pollA.result));
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(b, &pollB.desc, &pollB.result));
    EXPECT_GE(pollA.result.recordsWritten, 2u);
    EXPECT_EQ(0u, pollB.result.recordsWritten);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(a));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(b));
}

TEST(MonitorHubSubscription, RequestDoesNotBlockLogicThreadOnFanoutHold) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const std::uint64_t queueBytes = PackedBeginBytes() + PackedEndBytes() + 512;
    VnMonitorSessionDesc desc =
        MakeDesc(VN_MONITOR_STREAM_STATE, VN_MONITOR_PERM_OBSERVE_STATE, queueBytes);
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    harken::monitor::ResetFanoutEnteredForTests();
    harken::monitor::SetFanoutHoldForTests(true);
    VnMonitorStateCutDesc cutDesc{};
    cutDesc.structSize = sizeof(cutDesc);
    uint64_t cutId = 0;
    const auto requestStart = std::chrono::steady_clock::now();
    ASSERT_EQ(VN_OK, Vn_MonitorRequestStateCut(handle, &cutDesc, &cutId));
    const auto elapsed = std::chrono::steady_clock::now() - requestStart;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
              500);
    VnMonitorStateCutStatus cutStatus{};
    cutStatus.structSize = sizeof(cutStatus);
    EXPECT_EQ(VN_MONITOR_STALE_SESSION,
              Vn_MonitorGetStateCutStatus(handle, cutId, &cutStatus));
    harken::monitor::SetFanoutHoldForTests(false);
    const auto waitStart = std::chrono::steady_clock::now();
    while (harken::monitor::FanoutEnteredForTests() == 0 &&
           std::chrono::steady_clock::now() - waitStart < std::chrono::seconds(2)) {
        std::this_thread::yield();
    }
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubSubscription, StateCutFenceIgnoresAudioBacklog) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::StopDrainForTests();
    const std::uint32_t capacity = harken::monitor::ProducerRingCapacityForTests();
    ASSERT_GT(capacity, 0u);
    ASSERT_TRUE(harken::monitor::PublishSyntheticTimeSeriesForTests(capacity));
    harken::monitor::StartDrainForTests();
    const std::uint64_t queueBytes =
        PackedBeginBytes() + PackedEndBytes();
    VnMonitorSessionDesc desc =
        MakeDesc(VN_MONITOR_STREAM_STATE, VN_MONITOR_PERM_OBSERVE_STATE, queueBytes);
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorStateCutDesc cutDesc{};
    cutDesc.structSize = sizeof(cutDesc);
    uint64_t cutId = 0;
    const auto requestStart = std::chrono::steady_clock::now();
    ASSERT_EQ(VN_OK, Vn_MonitorRequestStateCut(handle, &cutDesc, &cutId));
    const auto elapsed = std::chrono::steady_clock::now() - requestStart;
    EXPECT_LT(std::chrono::duration_cast<std::chrono::milliseconds>(elapsed).count(),
              500);
    harken::monitor::DrainOnceForTests();
    VnMonitorStateCutStatus cutStatus{};
    cutStatus.structSize = sizeof(cutStatus);
    ASSERT_EQ(VN_OK, Vn_MonitorGetStateCutStatus(handle, cutId, &cutStatus));
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_STATE_CUT_COMPLETE),
              cutStatus.result);
    EXPECT_EQ(2u, cutStatus.emittedCount);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubSubscription, StateCutUsesLogicRingNotAudioProducer) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    harken::monitor::StopDrainForTests();
    const std::uint32_t capacity = harken::monitor::ProducerRingCapacityForTests();
    ASSERT_GT(capacity, 0u);
    ASSERT_TRUE(harken::monitor::PublishSyntheticTimeSeriesForTests(capacity));
    EXPECT_FALSE(harken::monitor::PublishSyntheticTimeSeriesForTests(1));
    const std::uint64_t queueBytes =
        PackedBeginBytes() + PackedEndBytes();
    VnMonitorSessionDesc desc =
        MakeDesc(VN_MONITOR_STREAM_STATE, VN_MONITOR_PERM_OBSERVE_STATE, queueBytes);
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorStateCutDesc cutDesc{};
    cutDesc.structSize = sizeof(cutDesc);
    uint64_t cutId = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorRequestStateCut(handle, &cutDesc, &cutId));
    harken::monitor::DrainOnceForTests();
    VnMonitorStateCutStatus cutStatus{};
    cutStatus.structSize = sizeof(cutStatus);
    ASSERT_EQ(VN_OK, Vn_MonitorGetStateCutStatus(handle, cutId, &cutStatus));
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_STATE_CUT_COMPLETE),
              cutStatus.result);
    EXPECT_EQ(2u, cutStatus.emittedCount);
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(handle, &poll.desc, &poll.result));
    EXPECT_GE(poll.result.recordsWritten, 2u);
    harken::monitor::StartDrainForTests();
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorHubSubscription, EpochBoundaryRequiresResync) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionDesc desc =
        MakeDesc(VN_MONITOR_STREAM_TIME_SERIES,
                 VN_MONITOR_PERM_OBSERVE_MEASUREMENTS);
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    ASSERT_TRUE(harken::monitor::PublishEpochBoundaryForTests());
    PollArgs poll;
    EXPECT_EQ(VN_MONITOR_RESYNC_REQUIRED,
              Vn_MonitorPoll(handle, &poll.desc, &poll.result));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
    VnMonitorSessionHandle next = 0;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &next));
    EXPECT_NE(handle, next);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(next));
}
