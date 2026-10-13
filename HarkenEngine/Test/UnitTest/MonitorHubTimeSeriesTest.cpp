#include "MonitorCaptureHubTestHooks.h"
#include "MonitorRecordPack.h"
#include "Common/TestUtil.h"
#include "monitor_fixture_decode.h"

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
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_TIME_SERIES;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_MEASUREMENTS;
    desc.consumerQueueBytes = 65536;
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
    plan.bytesPerSecondBudget = 65536;
    return plan;
}

struct PollArgs {
    std::vector<unsigned char> buffer;
    VnMonitorPollDesc desc{};
    VnMonitorPollResult result{};

    explicit PollArgs(std::uint64_t bytes = 65536)
        : buffer(static_cast<std::size_t>(bytes)) {
        desc.structSize = sizeof(desc);
        desc.buffer = buffer.data();
        desc.bufferBytes = buffer.size();
        result.structSize = sizeof(result);
    }
};

VnResult Acquire(VnMonitorSessionHandle handle,
                 VnMonitorCaptureControlStatus* status) {
    status->structSize = sizeof(*status);
    return Vn_MonitorAcquireCaptureControl(handle, status);
}

bool WaitActiveGeneration(VnMonitorSessionHandle handle, std::uint64_t generation) {
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

bool WaitPollRecords(VnMonitorSessionHandle handle, PollArgs* poll,
                     std::uint32_t minimum) {
    *poll = PollArgs();
    std::uint32_t got = 0;
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        PollArgs chunk;
        if (Vn_MonitorPoll(handle, &chunk.desc, &chunk.result) == VN_OK &&
            chunk.result.recordsWritten > 0 && chunk.result.bytesWritten > 0) {
            if (got + chunk.result.bytesWritten > poll->buffer.size()) {
                return false;
            }
            std::memcpy(poll->buffer.data() + got, chunk.buffer.data(),
                        static_cast<std::size_t>(chunk.result.bytesWritten));
            got += static_cast<std::uint32_t>(chunk.result.bytesWritten);
            poll->result.bytesWritten = got;
            poll->result.recordsWritten += chunk.result.recordsWritten;
            if (poll->result.recordsWritten >= minimum) {
                return true;
            }
        }
        std::this_thread::yield();
    }
    return poll->result.recordsWritten >= minimum;
}

bool WaitActivationRecord(VnMonitorSessionHandle handle, PollArgs* poll,
                          std::uint64_t generation, std::uint32_t reason) {
    *poll = PollArgs();
    std::uint32_t got = 0;
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        PollArgs chunk;
        if (Vn_MonitorPoll(handle, &chunk.desc, &chunk.result) == VN_OK &&
            chunk.result.bytesWritten > 0) {
            if (got + chunk.result.bytesWritten > poll->buffer.size()) {
                return false;
            }
            std::memcpy(poll->buffer.data() + got, chunk.buffer.data(),
                        static_cast<std::size_t>(chunk.result.bytesWritten));
            got += static_cast<std::uint32_t>(chunk.result.bytesWritten);
            poll->result.bytesWritten = got;
            poll->result.recordsWritten += chunk.result.recordsWritten;
            const unsigned char* bytes = poll->buffer.data();
            std::uint64_t offset = 0;
            const std::uint32_t envelope = VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES;
            while (offset + envelope <= got) {
                const std::uint32_t recordKind = ReadU32(bytes + offset);
                const std::uint64_t recordBytes = ReadU64(bytes + offset + 16);
                if (recordBytes < envelope || offset + recordBytes > got) {
                    break;
                }
                if (recordKind == VN_MONITOR_RECORD_CAPTURE_PLAN_ACTIVATED) {
                    const std::uint64_t recGeneration = ReadU64(bytes + offset + 80);
                    const std::uint32_t recReason = ReadU32(bytes + offset + 248);
                    if (recGeneration == generation && recReason == reason) {
                        return true;
                    }
                }
                offset += recordBytes;
            }
        }
        std::this_thread::yield();
    }
    return false;
}

struct SeriesHit {
    bool seen = false;
    std::uint32_t metricId = 0;
    std::uint32_t unitId = 0;
    std::uint32_t cadenceHz = 0;
    std::uint32_t valueBits = 0;
    std::uint32_t validityFlags = 0;
    std::uint64_t sourceSequence = 0;
    std::uint64_t executeFrame = 0;
    std::uint64_t capturePlanGeneration = 0;
    std::uint32_t numericReadable = 0;
};

void CollectSeries(const PollArgs& poll, SeriesHit out[5]) {
    const unsigned char* bytes = poll.buffer.data();
    std::uint64_t offset = 0;
    const std::uint64_t total = poll.result.bytesWritten;
    const std::uint32_t envelope = VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES;
    while (offset + envelope <= total) {
        const std::uint32_t recordKind = ReadU32(bytes + offset);
        const std::uint64_t recordBytes = ReadU64(bytes + offset + 16);
        if (recordBytes < envelope || offset + recordBytes > total) {
            break;
        }
        if (recordKind == VN_MONITOR_RECORD_TIME_SERIES) {
            const std::uint64_t streamId = ReadU64(bytes + offset + 24);
            int slot = -1;
            if (streamId == harken::monitor::kTimeSeriesCallbackCpuStreamId) {
                slot = 0;
            } else if (streamId == harken::monitor::kTimeSeriesStageDrainCpuStreamId) {
                slot = 1;
            } else if (streamId == harken::monitor::kTimeSeriesStageVoicesCpuStreamId) {
                slot = 2;
            } else if (streamId == harken::monitor::kTimeSeriesStageBusesCpuStreamId) {
                slot = 3;
            } else if (streamId == harken::monitor::kTimeSeriesArenaHighWaterStreamId) {
                slot = 4;
            }
            if (slot >= 0 && !out[slot].seen &&
                recordBytes >= envelope + sizeof(VnMonitorTimeSeriesSample)) {
                VnMonitorTimeSeriesSample sample{};
                std::memcpy(&sample, bytes + offset + envelope, sizeof(sample));
                out[slot].seen = true;
                out[slot].metricId = sample.metricId;
                out[slot].unitId = sample.unitId;
                out[slot].cadenceHz = sample.cadenceHz;
                out[slot].valueBits = sample.valueBits;
                out[slot].validityFlags = ReadU32(bytes + offset + 252);
                out[slot].sourceSequence = ReadU64(bytes + offset + 40);
                out[slot].executeFrame = ReadU64(bytes + offset + 128);
                out[slot].capturePlanGeneration = ReadU64(bytes + offset + 80);
                const std::uint32_t flags = out[slot].validityFlags;
                out[slot].numericReadable =
                    ((flags & VN_MONITOR_VALID_NUMERIC) != 0 &&
                     (flags & VN_MONITOR_VALID_ARMED) != 0 &&
                     (flags & VN_MONITOR_VALID_UNSUPPORTED) == 0)
                        ? 1u
                        : 0u;
            }
        }
        offset += recordBytes;
    }
}

bool DrainHeadlessUntilQuiet(VnMonitorSessionHandle handle) {
    const auto start = std::chrono::steady_clock::now();
    int quietStreak = 0;
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        PollArgs leftover;
        if (Vn_MonitorPoll(handle, &leftover.desc, &leftover.result) != VN_OK) {
            return false;
        }
        if (leftover.result.recordsWritten == 0) {
            if (++quietStreak >= 3) {
                return true;
            }
        } else {
            quietStreak = 0;
        }
        std::this_thread::yield();
    }
    return false;
}

} // namespace

TEST(MonitorHubTimeSeries, LivePathPublishesFiveNumericStreams) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle control = 0;
    VnMonitorSessionHandle headless = 0;
    VnMonitorSessionDesc controlDesc = MakeControlDesc();
    VnMonitorSessionDesc observeDesc = MakeObserveDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&controlDesc, &control));
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&observeDesc, &headless));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(control, &lease));
    VnMonitorCapturePlan plan = MakeArmedPlan();
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(control, lease.leaseGeneration,
                                                &plan, &accepted));
    engine.Engine()->nullBackend.Pump(1);
    ASSERT_TRUE(WaitActiveGeneration(control, 2));
    engine.Engine()->nullBackend.Pump(2);
    PollArgs poll;
    ASSERT_TRUE(WaitPollRecords(headless, &poll, 5));
    SeriesHit hits[5]{};
    CollectSeries(poll, hits);
    EXPECT_TRUE(hits[0].seen);
    EXPECT_TRUE(hits[1].seen);
    EXPECT_TRUE(hits[2].seen);
    EXPECT_TRUE(hits[3].seen);
    EXPECT_TRUE(hits[4].seen);
    EXPECT_EQ(harken::monitor::kTimeSeriesMetricCallbackCpu, hits[0].metricId);
    EXPECT_EQ(harken::monitor::kTimeSeriesUnitMicroseconds, hits[0].unitId);
    EXPECT_EQ(30u, hits[0].cadenceHz);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_VALID_ARMED |
                                         VN_MONITOR_VALID_NUMERIC),
              hits[0].validityFlags);
    EXPECT_NE(0u, hits[0].numericReadable);
    EXPECT_EQ(hits[0].sourceSequence, hits[1].sourceSequence);
    EXPECT_EQ(hits[0].executeFrame, hits[4].executeFrame);
    EXPECT_EQ(2u, hits[0].capturePlanGeneration);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(control));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(headless));
}

TEST(MonitorHubTimeSeries, PendingDoesNotPublishLiveSeries) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle control = 0;
    VnMonitorSessionHandle headless = 0;
    VnMonitorSessionDesc controlDesc = MakeControlDesc();
    VnMonitorSessionDesc observeDesc = MakeObserveDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&controlDesc, &control));
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&observeDesc, &headless));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(control, &lease));
    VnMonitorCapturePlan plan = MakeArmedPlan();
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(control, lease.leaseGeneration,
                                                &plan, &accepted));
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(headless, &poll.desc, &poll.result));
    EXPECT_EQ(0u, poll.result.recordsWritten);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(control));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(headless));
}

TEST(MonitorHubTimeSeries, StopThenPumpDoesNotPublishLiveSeries) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle control = 0;
    VnMonitorSessionHandle headless = 0;
    VnMonitorSessionDesc controlDesc = MakeControlDesc();
    controlDesc.requestedStreamKindMask =
        VN_MONITOR_STREAM_TIME_SERIES | VN_MONITOR_STREAM_EVENT;
    controlDesc.requestedPermissionMask =
        VN_MONITOR_PERM_OBSERVE_MEASUREMENTS | VN_MONITOR_PERM_OBSERVE_EVENTS |
        VN_MONITOR_PERM_CAPTURE_CONTROL;
    VnMonitorSessionDesc observeDesc = MakeObserveDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&controlDesc, &control));
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&observeDesc, &headless));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(control, &lease));
    VnMonitorCapturePlan plan = MakeArmedPlan();
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(control, lease.leaseGeneration,
                                                &plan, &accepted));
    engine.Engine()->nullBackend.Pump(1);
    ASSERT_TRUE(WaitActiveGeneration(control, 2));
    VnMonitorEffectiveCapturePlan stopped{};
    stopped.structSize = sizeof(stopped);
    ASSERT_EQ(VN_OK, Vn_MonitorStopCapture(control, lease.leaseGeneration, &stopped));
    engine.Engine()->nullBackend.Pump(1);
    ASSERT_TRUE(WaitActiveGeneration(control, 3));
    PollArgs activation;
    ASSERT_TRUE(WaitActivationRecord(control, &activation, 3, VN_MONITOR_EVENT_STOP));
    harken::monitor::StopDrainForTests();
    harken::monitor::DrainOnceForTests();
    const bool quiet = DrainHeadlessUntilQuiet(headless);
    harken::monitor::StartDrainForTests();
    ASSERT_TRUE(quiet);
    engine.Engine()->nullBackend.Pump(2);
    harken::monitor::DrainOnceForTests();
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(headless, &poll.desc, &poll.result));
    SeriesHit hits[5]{};
    if (poll.result.recordsWritten > 0) {
        CollectSeries(poll, hits);
    }
    EXPECT_FALSE(hits[0].seen);
    EXPECT_FALSE(hits[1].seen);
    EXPECT_FALSE(hits[2].seen);
    EXPECT_FALSE(hits[3].seen);
    EXPECT_FALSE(hits[4].seen);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(control));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(headless));
}

TEST(MonitorHubTimeSeries, KeepUntilExplicitStopContinuesAfterLeaseRelease) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle control = 0;
    VnMonitorSessionHandle headless = 0;
    VnMonitorSessionDesc controlDesc = MakeControlDesc();
    VnMonitorSessionDesc observeDesc = MakeObserveDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&controlDesc, &control));
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&observeDesc, &headless));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(control, &lease));
    VnMonitorCapturePlan plan = MakeArmedPlan();
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(control, lease.leaseGeneration,
                                                &plan, &accepted));
    engine.Engine()->nullBackend.Pump(1);
    ASSERT_TRUE(WaitActiveGeneration(control, 2));
    ASSERT_EQ(VN_OK, Vn_MonitorReleaseCaptureControl(control, lease.leaseGeneration));
    engine.Engine()->nullBackend.Pump(2);
    PollArgs poll;
    ASSERT_TRUE(WaitPollRecords(headless, &poll, 5));
    SeriesHit hits[5]{};
    CollectSeries(poll, hits);
    EXPECT_TRUE(hits[0].seen);
    EXPECT_EQ(2u, hits[0].capturePlanGeneration);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(control));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(headless));
}

TEST(MonitorHubTimeSeries, CadenceFieldIsPlanValueWithoutDownsample) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle control = 0;
    VnMonitorSessionHandle headless = 0;
    VnMonitorSessionDesc controlDesc = MakeControlDesc();
    VnMonitorSessionDesc observeDesc = MakeObserveDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&controlDesc, &control));
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&observeDesc, &headless));
    VnMonitorCaptureControlStatus lease{};
    ASSERT_EQ(VN_OK, Acquire(control, &lease));
    VnMonitorCapturePlan plan = MakeArmedPlan();
    plan.cadenceHz = 30;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(control, lease.leaseGeneration,
                                                &plan, &accepted));
    engine.Engine()->nullBackend.Pump(1);
    ASSERT_TRUE(WaitActiveGeneration(control, 2));
    engine.Engine()->nullBackend.Pump(3);
    PollArgs poll;
    ASSERT_TRUE(WaitPollRecords(headless, &poll, 5));
    SeriesHit hits[5]{};
    CollectSeries(poll, hits);
    EXPECT_TRUE(hits[0].seen);
    EXPECT_EQ(30u, hits[0].cadenceHz);
    EXPECT_EQ(hits[0].sourceSequence, hits[4].sourceSequence);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(control));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(headless));
}

TEST(MonitorHubTimeSeries, SyntheticBytesStayUnsupportedNegativeFixture) {
    std::uint8_t packed[4096];
    const std::uint32_t size =
        harken::monitor::PackSyntheticTimeSeries(packed, sizeof(packed), 7);
    ASSERT_GT(size, 0u);
    VnMonitorDecodedRecord decoded[1]{};
    uint8_t skip[4096]{};
    const VnMonitorDecodeResult result =
        vn_monitor_decode_records(packed, size, decoded, 1, skip, sizeof(skip));
    ASSERT_EQ(VN_OK, result.status);
    ASSERT_EQ(1u, result.recordsWritten);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_VALID_UNSUPPORTED),
              decoded[0].header.validityFlags);
    EXPECT_EQ(0u, decoded[0].numericReadable);
}
