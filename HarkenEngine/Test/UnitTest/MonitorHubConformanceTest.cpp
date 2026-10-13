#include "Common/ObjectIdTestSupport.h"
#include "MonitorCaptureHubTestHooks.h"
#include "MonitorRecordPack.h"
#include "Common/TestUtil.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <string>
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
    desc.requestedStreamKindMask =
        VN_MONITOR_STREAM_TIME_SERIES | VN_MONITOR_STREAM_EVENT;
    desc.requestedPermissionMask =
        VN_MONITOR_PERM_OBSERVE_MEASUREMENTS | VN_MONITOR_PERM_OBSERVE_EVENTS |
        VN_MONITOR_PERM_CAPTURE_CONTROL;
    return desc;
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

std::uint32_t NumericReadable(std::uint32_t flags) {
    if ((flags & VN_MONITOR_VALID_NUMERIC) == 0) {
        return 0;
    }
    if ((flags & VN_MONITOR_VALID_ARMED) == 0) {
        return 0;
    }
    if ((flags & VN_MONITOR_VALID_UNSUPPORTED) != 0) {
        return 0;
    }
    if ((flags & VN_MONITOR_VALID_WARMUP) != 0) {
        return 0;
    }
    return 1;
}

struct RecordView {
    std::uint32_t kind = 0;
    std::uint32_t version = 0;
    std::uint32_t flags = 0;
    std::uint32_t validity = 0;
    std::uint32_t valueBits = 0;
    std::uint32_t numericReadable = 0;
    std::uint64_t recordBytes = 0;
    std::uint64_t streamId = 0;
    std::uint64_t sourceSequence = 0;
};

std::vector<RecordView> ParseRecords(const PollArgs& poll) {
    std::vector<RecordView> out;
    const unsigned char* bytes = poll.buffer.data();
    std::uint64_t offset = 0;
    const std::uint64_t total = poll.result.bytesWritten;
    const std::uint32_t envelope = VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES;
    while (offset + envelope <= total) {
        RecordView rec;
        rec.kind = ReadU32(bytes + offset);
        rec.version = ReadU32(bytes + offset + 4);
        rec.flags = ReadU32(bytes + offset + 8);
        rec.recordBytes = ReadU64(bytes + offset + 16);
        rec.streamId = ReadU64(bytes + offset + 24);
        rec.sourceSequence = ReadU64(bytes + offset + 40);
        rec.validity = ReadU32(bytes + offset + 252);
        if (rec.recordBytes < envelope || offset + rec.recordBytes > total) {
            break;
        }
        if (rec.kind == VN_MONITOR_RECORD_TIME_SERIES &&
            rec.recordBytes >= envelope + sizeof(VnMonitorTimeSeriesSample)) {
            VnMonitorTimeSeriesSample sample{};
            std::memcpy(&sample, bytes + offset + envelope, sizeof(sample));
            rec.valueBits = sample.valueBits;
        }
        rec.numericReadable = NumericReadable(rec.validity);
        out.push_back(rec);
        offset += rec.recordBytes;
    }
    return out;
}

struct PackedRecord {
    std::array<std::uint8_t, 4096> bytes{};
    std::uint32_t size = 0;
};

bool PushBatchAndDrain(const PackedRecord* records, std::uint32_t count) {
    harken::monitor::StopDrainForTests();
    bool allPushed = true;
    for (std::uint32_t i = 0; i < count; ++i) {
        if (!harken::monitor::PublishConformancePackedForTests(records[i].bytes.data(),
                                                              records[i].size)) {
            allPushed = false;
        }
    }
    harken::monitor::StartDrainForTests();
    harken::monitor::DrainOnceForTests();
    return allPushed;
}

bool PushAndDrain(const std::uint8_t* packed, std::uint32_t size) {
    PackedRecord rec;
    std::memcpy(rec.bytes.data(), packed, size);
    rec.size = size;
    return PushBatchAndDrain(&rec, 1);
}

std::uint32_t PackValiditySeries(std::uint8_t* out, std::uint32_t capacity,
                                 std::uint32_t validity, std::uint32_t valueBits) {
    return harken::monitor::PackTimeSeriesRecord(
        out, capacity, 1, harken::monitor::kTimeSeriesCallbackCpuStreamId,
        harken::monitor::kTimeSeriesMetricCallbackCpu,
        harken::monitor::kTimeSeriesUnitMicroseconds, 30, valueBits, validity, 2,
        1);
}

PackedRecord MakeValiditySeries(std::uint32_t validity, std::uint32_t valueBits) {
    PackedRecord rec;
    rec.size = PackValiditySeries(rec.bytes.data(), rec.bytes.size(), validity,
                                  valueBits);
    return rec;
}

PackedRecord MakeUnknown(std::uint32_t flags,
                         std::uint32_t kind = VN_MONITOR_RECORD_UNKNOWN,
                         std::uint32_t version = 1) {
    PackedRecord rec;
    rec.size = harken::monitor::PackUnknownRecord(
        rec.bytes.data(), rec.bytes.size(), 1, flags, kind, version);
    return rec;
}

VnResult OpenHeadless(VnMonitorSessionHandle* handle,
                      std::uint64_t queueBytes = 65536) {
    VnMonitorSessionDesc desc = MakeObserveDesc();
    desc.consumerQueueBytes = queueBytes;
    return Vn_MonitorOpenSession(&desc, handle);
}

bool HasLiveTimeSeries(const std::vector<RecordView>& recs) {
    for (const RecordView& rec : recs) {
        if (rec.kind == VN_MONITOR_RECORD_TIME_SERIES &&
            rec.streamId >= harken::monitor::kTimeSeriesCallbackCpuStreamId &&
            rec.streamId <= harken::monitor::kTimeSeriesArenaHighWaterStreamId) {
            return true;
        }
    }
    return false;
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

void DrainPoll(VnMonitorSessionHandle handle) {
    for (int i = 0; i < 8; ++i) {
        PollArgs leftover;
        if (Vn_MonitorPoll(handle, &leftover.desc, &leftover.result) != VN_OK ||
            leftover.result.recordsWritten == 0) {
            break;
        }
    }
}

} // namespace

TEST(MonitorHub, ConformanceUnsupportedIsNotNumericZero) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle headless = 0;
    ASSERT_EQ(VN_OK, OpenHeadless(&headless));
    std::uint8_t packed[4096];
    const std::uint32_t size =
        harken::monitor::PackSyntheticTimeSeries(packed, sizeof(packed), 1);
    ASSERT_GT(size, 0u);
    ASSERT_TRUE(PushAndDrain(packed, size));
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(headless, &poll.desc, &poll.result));
    const auto recs = ParseRecords(poll);
    ASSERT_FALSE(recs.empty());
    EXPECT_EQ(VN_MONITOR_RECORD_TIME_SERIES, recs[0].kind);
    EXPECT_EQ(0u, recs[0].numericReadable);
    EXPECT_EQ(0u, recs[0].valueBits);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(headless));
}

TEST(MonitorHub, ConformanceUnarmedZeroIsNotReadable) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle headless = 0;
    ASSERT_EQ(VN_OK, OpenHeadless(&headless));
    std::uint8_t packed[4096];
    const std::uint32_t size = PackValiditySeries(packed, sizeof(packed), 0, 0);
    ASSERT_GT(size, 0u);
    ASSERT_TRUE(PushAndDrain(packed, size));
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(headless, &poll.desc, &poll.result));
    const auto recs = ParseRecords(poll);
    ASSERT_FALSE(recs.empty());
    EXPECT_EQ(0u, recs[0].numericReadable);
    EXPECT_EQ(0u, recs[0].valueBits);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(headless));
}

TEST(MonitorHub, ConformanceArmedInvalidZeroIsNotReadable) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle headless = 0;
    ASSERT_EQ(VN_OK, OpenHeadless(&headless));
    std::uint8_t packed[4096];
    const std::uint32_t size =
        PackValiditySeries(packed, sizeof(packed), VN_MONITOR_VALID_ARMED, 0);
    ASSERT_GT(size, 0u);
    ASSERT_TRUE(PushAndDrain(packed, size));
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(headless, &poll.desc, &poll.result));
    EXPECT_GE(poll.result.recordsWritten, 1u);
    EXPECT_GE(poll.result.bytesWritten, size);
    const auto recs = ParseRecords(poll);
    ASSERT_FALSE(recs.empty());
    EXPECT_EQ(VN_MONITOR_RECORD_TIME_SERIES, recs[0].kind);
    EXPECT_NE(0u, recs[0].validity & VN_MONITOR_VALID_ARMED);
    EXPECT_EQ(0u, recs[0].validity & VN_MONITOR_VALID_NUMERIC);
    EXPECT_EQ(0u, recs[0].numericReadable);
    EXPECT_EQ(0u, recs[0].valueBits);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(headless));
}

TEST(MonitorHub, ConformanceArmedNumericZeroIsSilence) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle headless = 0;
    ASSERT_EQ(VN_OK, OpenHeadless(&headless));
    std::uint8_t packed[4096];
    const std::uint32_t size = PackValiditySeries(
        packed, sizeof(packed),
        VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC, 0);
    ASSERT_GT(size, 0u);
    ASSERT_TRUE(PushAndDrain(packed, size));
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(headless, &poll.desc, &poll.result));
    const auto recs = ParseRecords(poll);
    ASSERT_FALSE(recs.empty());
    EXPECT_EQ(1u, recs[0].numericReadable);
    EXPECT_EQ(0u, recs[0].valueBits);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(headless));
}

TEST(MonitorHub, ConformanceProducerDropIsGapNotZero) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle headless = 0;
    ASSERT_EQ(VN_OK, OpenHeadless(&headless));
    DrainPoll(headless);
    harken::monitor::StopDrainForTests();
    harken::monitor::DrainOnceForTests();
    std::uint8_t packed[4096];
    const std::uint32_t size = PackValiditySeries(
        packed, sizeof(packed),
        VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC, 1);
    ASSERT_GT(size, 0u);
    const std::uint32_t capacity = harken::monitor::ProducerRingCapacityForTests();
    std::uint32_t pushed = 0;
    for (std::uint32_t i = 0; i < capacity + 4; ++i) {
        if (harken::monitor::PublishConformancePackedForTests(packed, size)) {
            ++pushed;
        }
    }
    EXPECT_EQ(capacity, pushed);
    const VnMonitorDropSummary drop = harken::monitor::ProducerDropForTests();
    EXPECT_GE(drop.recordCount, 1u);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_DROP_SOURCE_PRODUCER),
              drop.stage);
    EXPECT_GE(drop.lastSourceSequence, drop.firstSourceSequence);
    harken::monitor::StartDrainForTests();
    harken::monitor::DrainOnceForTests();
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(headless, &poll.desc, &poll.result));
    const auto recs = ParseRecords(poll);
    std::uint32_t seriesCount = 0;
    std::uint64_t maxSeq = 0;
    for (const RecordView& rec : recs) {
        if (rec.kind != VN_MONITOR_RECORD_TIME_SERIES || rec.valueBits != 1) {
            continue;
        }
        ++seriesCount;
        maxSeq = rec.sourceSequence > maxSeq ? rec.sourceSequence : maxSeq;
        EXPECT_NE(0u, rec.numericReadable);
    }
    EXPECT_EQ(pushed, seriesCount);
    EXPECT_LT(maxSeq, drop.firstSourceSequence);
    uint32_t required = 0;
    EXPECT_EQ(VN_MONITOR_BUFFER_TOO_SMALL,
              Vn_MonitorGetConsumerStatus(headless, nullptr, 0, &required));
    EXPECT_GE(required, 1u);
    std::vector<VnMonitorDropSummary> summaries(required);
    ASSERT_EQ(VN_OK, Vn_MonitorGetConsumerStatus(headless, summaries.data(),
                                                 required, &required));
    bool sawProducerDrop = false;
    for (uint32_t i = 0; i < required; ++i) {
        if (summaries[i].stage ==
            static_cast<std::uint32_t>(VN_MONITOR_DROP_SOURCE_PRODUCER)) {
            sawProducerDrop = true;
            EXPECT_GE(summaries[i].recordCount, 1u);
        }
    }
    EXPECT_TRUE(sawProducerDrop);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(headless));
}

void ExpectRequiredUnknownStops(const std::vector<RecordView>& recs,
                                std::uint32_t expectedUnknownKind,
                                std::uint32_t expectedUnknownVersion) {
    ASSERT_GE(recs.size(), 2u);
    EXPECT_EQ(VN_MONITOR_RECORD_TIME_SERIES, recs[0].kind);
    EXPECT_EQ(1u, recs[0].numericReadable);
    EXPECT_EQ(7u, recs[0].valueBits);
    EXPECT_EQ(expectedUnknownKind, recs[1].kind);
    EXPECT_EQ(expectedUnknownVersion, recs[1].version);
    EXPECT_NE(0u, recs[1].flags & VN_MONITOR_RECORD_FLAG_REQUIRED);
    EXPECT_EQ(recs[1].recordBytes, harken::monitor::PackedControlRecordBytes());
    for (std::size_t i = 2; i < recs.size(); ++i) {
        EXPECT_NE(VN_MONITOR_RECORD_TIME_SERIES, recs[i].kind);
        EXPECT_NE(9u, recs[i].valueBits);
    }
}

TEST(MonitorHub, ConformanceRequiredUnknownStopsStream) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle headless = 0;
    ASSERT_EQ(VN_OK, OpenHeadless(&headless));
    PackedRecord batch[3] = {
        MakeValiditySeries(VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC, 7),
        MakeUnknown(VN_MONITOR_RECORD_FLAG_REQUIRED),
        MakeValiditySeries(VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC, 9),
    };
    ASSERT_TRUE(PushBatchAndDrain(batch, 3));
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(headless, &poll.desc, &poll.result));
    ExpectRequiredUnknownStops(ParseRecords(poll), VN_MONITOR_RECORD_UNKNOWN, 1);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(headless));
}

TEST(MonitorHub, ConformanceRequiredUnknownVersionStopsStream) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle headless = 0;
    ASSERT_EQ(VN_OK, OpenHeadless(&headless));
    PackedRecord batch[3] = {
        MakeValiditySeries(VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC, 7),
        MakeUnknown(VN_MONITOR_RECORD_FLAG_REQUIRED, VN_MONITOR_RECORD_TIME_SERIES,
                    99),
        MakeValiditySeries(VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC, 9),
    };
    ASSERT_TRUE(PushBatchAndDrain(batch, 3));
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(headless, &poll.desc, &poll.result));
    ExpectRequiredUnknownStops(ParseRecords(poll), VN_MONITOR_RECORD_TIME_SERIES,
                               99);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(headless));
}

TEST(MonitorHub, ConformanceRequiredInvalidKindStopsStream) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle headless = 0;
    ASSERT_EQ(VN_OK, OpenHeadless(&headless));
    PackedRecord batch[3] = {
        MakeValiditySeries(VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC, 7),
        MakeUnknown(VN_MONITOR_RECORD_FLAG_REQUIRED, VN_MONITOR_RECORD_INVALID, 1),
        MakeValiditySeries(VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC, 9),
    };
    ASSERT_TRUE(PushBatchAndDrain(batch, 3));
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(headless, &poll.desc, &poll.result));
    ExpectRequiredUnknownStops(ParseRecords(poll), VN_MONITOR_RECORD_INVALID, 1);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(headless));
}

TEST(MonitorHub, ConformanceRequiredAndOptionalUnknownKeepsRequiredPriority) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle headless = 0;
    ASSERT_EQ(VN_OK, OpenHeadless(&headless));
    PackedRecord batch[3] = {
        MakeValiditySeries(VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC, 7),
        MakeUnknown(VN_MONITOR_RECORD_FLAG_REQUIRED |
                    VN_MONITOR_RECORD_FLAG_OPTIONAL),
        MakeValiditySeries(VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC, 9),
    };
    ASSERT_TRUE(PushBatchAndDrain(batch, 3));
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(headless, &poll.desc, &poll.result));
    ExpectRequiredUnknownStops(ParseRecords(poll), VN_MONITOR_RECORD_UNKNOWN, 1);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(headless));
}

TEST(MonitorHub, ConformanceRequiredUnknownStopsEvenIfEnqueueFails) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    const std::uint32_t seriesBytes = harken::monitor::PackedTimeSeriesBytes();
    VnMonitorSessionHandle headless = 0;
    ASSERT_EQ(VN_OK, OpenHeadless(&headless, seriesBytes));
    PackedRecord filler =
        MakeValiditySeries(VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC, 1);
    ASSERT_TRUE(PushAndDrain(filler.bytes.data(), filler.size));
    PackedRecord batch[2] = {
        MakeUnknown(VN_MONITOR_RECORD_FLAG_REQUIRED),
        MakeValiditySeries(VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC, 9),
    };
    (void)PushBatchAndDrain(batch, 2);
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(headless, &poll.desc, &poll.result));
    const auto recs = ParseRecords(poll);
    bool sawAfter = false;
    for (const RecordView& rec : recs) {
        if (rec.kind == VN_MONITOR_RECORD_TIME_SERIES && rec.valueBits == 9) {
            sawAfter = true;
        }
    }
    EXPECT_FALSE(sawAfter);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(headless));
}

TEST(MonitorHub, ConformanceOptionalUnknownSkipsAndContinues) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle headless = 0;
    ASSERT_EQ(VN_OK, OpenHeadless(&headless));
    PackedRecord batch[3] = {
        MakeValiditySeries(VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC, 3),
        MakeUnknown(VN_MONITOR_RECORD_FLAG_OPTIONAL),
        MakeValiditySeries(VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC, 5),
    };
    ASSERT_TRUE(PushBatchAndDrain(batch, 3));
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(headless, &poll.desc, &poll.result));
    const auto recs = ParseRecords(poll);
    ASSERT_GE(recs.size(), 3u);
    EXPECT_EQ(VN_MONITOR_RECORD_TIME_SERIES, recs[0].kind);
    EXPECT_EQ(3u, recs[0].valueBits);
    EXPECT_EQ(VN_MONITOR_RECORD_UNKNOWN, recs[1].kind);
    EXPECT_NE(0u, recs[1].flags & VN_MONITOR_RECORD_FLAG_OPTIONAL);
    EXPECT_EQ(harken::monitor::PackedControlRecordBytes(), recs[1].recordBytes);
    EXPECT_GT(recs[1].recordBytes, 0u);
    EXPECT_EQ(VN_MONITOR_RECORD_TIME_SERIES, recs[2].kind);
    EXPECT_EQ(5u, recs[2].valueBits);
    EXPECT_EQ(1u, recs[2].numericReadable);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(headless));
}

struct FanoutHoldGuard {
    FanoutHoldGuard() { harken::monitor::SetFanoutHoldForTests(true); }
    ~FanoutHoldGuard() { harken::monitor::SetFanoutHoldForTests(false); }
};

void WaitFanoutEntered(std::uint32_t count) {
    const auto start = std::chrono::steady_clock::now();
    while (harken::monitor::FanoutEnteredForTests() < count &&
           std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        std::this_thread::yield();
    }
}

TEST(MonitorHub, ConformanceCloseReopenClearsTerminated) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle first = 0;
    ASSERT_EQ(VN_OK, OpenHeadless(&first));
    PackedRecord stopBatch[2] = {
        MakeUnknown(VN_MONITOR_RECORD_FLAG_REQUIRED),
        MakeValiditySeries(VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC, 9),
    };
    ASSERT_TRUE(PushBatchAndDrain(stopBatch, 2));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(first));
    VnMonitorSessionHandle second = 0;
    ASSERT_EQ(VN_OK, OpenHeadless(&second));
    PackedRecord fresh =
        MakeValiditySeries(VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC, 4);
    ASSERT_TRUE(PushAndDrain(fresh.bytes.data(), fresh.size));
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(second, &poll.desc, &poll.result));
    const auto recs = ParseRecords(poll);
    ASSERT_FALSE(recs.empty());
    EXPECT_EQ(VN_MONITOR_RECORD_TIME_SERIES, recs[0].kind);
    EXPECT_EQ(4u, recs[0].valueBits);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(second));
}

TEST(MonitorHub, ConformanceStaleFanoutDoesNotTerminateReopenedSession) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle first = 0;
    ASSERT_EQ(VN_OK, OpenHeadless(&first));
    harken::monitor::StopDrainForTests();
    harken::monitor::ResetFanoutEnteredForTests();
    PackedRecord unknown = MakeUnknown(VN_MONITOR_RECORD_FLAG_REQUIRED);
    FanoutHoldGuard hold;
    harken::monitor::StartDrainForTests();
    ASSERT_TRUE(harken::monitor::PublishConformancePackedForTests(
        unknown.bytes.data(), unknown.size));
    WaitFanoutEntered(1);
    ASSERT_GE(harken::monitor::FanoutEnteredForTests(), 1u);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(first));
    VnMonitorSessionHandle second = 0;
    ASSERT_EQ(VN_OK, OpenHeadless(&second));
    harken::monitor::SetFanoutHoldForTests(false);
    const auto waitStart = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - waitStart <
           std::chrono::milliseconds(200)) {
        std::this_thread::yield();
    }
    PackedRecord fresh =
        MakeValiditySeries(VN_MONITOR_VALID_ARMED | VN_MONITOR_VALID_NUMERIC, 4);
    ASSERT_TRUE(PushAndDrain(fresh.bytes.data(), fresh.size));
    PollArgs poll;
    ASSERT_EQ(VN_OK, Vn_MonitorPoll(second, &poll.desc, &poll.result));
    const auto recs = ParseRecords(poll);
    bool sawFresh = false;
    for (const RecordView& rec : recs) {
        if (rec.kind == VN_MONITOR_RECORD_TIME_SERIES && rec.valueBits == 4) {
            sawFresh = true;
        }
    }
    EXPECT_TRUE(sawFresh);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(second));
}

TEST(MonitorHub, ConformanceCaptureOffThenOnKeepsAudio) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> sine =
        harkentest::MakeSine(256, 750.0, 48000.0, 0.7f);
    const VnPCMDesc pcm = harkentest::MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    auto expectSine = [&](const harkentest::Capture& out) {
        ASSERT_EQ(sine.size(), out.left.size());
        ASSERT_EQ(sine.size(), out.right.size());
        for (std::size_t i = 0; i < sine.size(); ++i) {
            const float expected = sine[i];
            ASSERT_EQ(expected, out.left[i]) << "left sample " << i;
            ASSERT_EQ(expected, out.right[i]) << "right sample " << i;
        }
    };
    VnMonitorSessionHandle control = 0;
    VnMonitorSessionHandle headless = 0;
    VnMonitorSessionDesc controlDesc = MakeControlDesc();
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&controlDesc, &control));
    ASSERT_EQ(VN_OK, OpenHeadless(&headless));
    {
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        harkentest::Capture off;
        harkentest::PumpAndCapture(session.Engine(), 1, off);
        expectSine(off);
        PollArgs offPoll;
        ASSERT_EQ(VN_OK, Vn_MonitorPoll(headless, &offPoll.desc, &offPoll.result));
        EXPECT_FALSE(HasLiveTimeSeries(ParseRecords(offPoll)));
    }
    VnMonitorCaptureControlStatus lease{};
    lease.structSize = sizeof(lease);
    ASSERT_EQ(VN_OK, Vn_MonitorAcquireCaptureControl(control, &lease));
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_TIME_SERIES;
    plan.cadenceHz = 30;
    plan.bytesPerSecondBudget = 65536;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(control, lease.leaseGeneration,
                                                &plan, &accepted));
    session.Engine()->nullBackend.Pump(1);
    ASSERT_TRUE(WaitActiveGeneration(control, 2));
    {
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        harkentest::Capture on;
        harkentest::PumpAndCapture(session.Engine(), 1, on);
        expectSine(on);
        session.Engine()->nullBackend.Pump(2);
        PollArgs onPoll;
        const auto start = std::chrono::steady_clock::now();
        bool sawLive = false;
        while (std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
            ASSERT_EQ(VN_OK, Vn_MonitorPoll(headless, &onPoll.desc, &onPoll.result));
            if (HasLiveTimeSeries(ParseRecords(onPoll))) {
                sawLive = true;
                break;
            }
            std::this_thread::yield();
        }
        EXPECT_TRUE(sawLive);
    }
    VnMonitorEffectiveCapturePlan stopped{};
    stopped.structSize = sizeof(stopped);
    ASSERT_EQ(VN_OK, Vn_MonitorStopCapture(control, lease.leaseGeneration, &stopped));
    session.Engine()->nullBackend.Pump(1);
    ASSERT_TRUE(WaitActiveGeneration(control, 3));
    {
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        harkentest::Capture after;
        harkentest::PumpAndCapture(session.Engine(), 1, after);
        expectSine(after);
    }
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(control));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(headless));
}
