#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "MonitorCaptureHub.h"
#include "MonitorCaptureHubTestHooks.h"
#include "MonitorRecordPack.h"
#include "Common/TestUtil.h"
#include "monitor_fixture_decode.h"
#include "harken/render/BusTree.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

namespace {

VnMonitorMeasurementRequest MakePeak() {
    VnMonitorMeasurementRequest request{};
    request.structSize = sizeof(request);
    request.kind = VN_MONITOR_MEASURE_SAMPLE_PEAK;
    request.channelMode = VN_MONITOR_CHANNEL_PER_CHANNEL;
    request.downsample = 1;
    return request;
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

std::uint64_t ReadU64(const std::uint8_t* bytes, std::size_t offset) {
    return static_cast<std::uint64_t>(bytes[offset]) |
           (static_cast<std::uint64_t>(bytes[offset + 1]) << 8) |
           (static_cast<std::uint64_t>(bytes[offset + 2]) << 16) |
           (static_cast<std::uint64_t>(bytes[offset + 3]) << 24) |
           (static_cast<std::uint64_t>(bytes[offset + 4]) << 32) |
           (static_cast<std::uint64_t>(bytes[offset + 5]) << 40) |
           (static_cast<std::uint64_t>(bytes[offset + 6]) << 48) |
           (static_cast<std::uint64_t>(bytes[offset + 7]) << 56);
}

VnResult OpenAudioControl(VnMonitorSessionHandle* handle) {
    VnMonitorSessionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.requestedStreamKindMask =
        VN_MONITOR_STREAM_OPTIONAL_AUDIO | VN_MONITOR_STREAM_EVENT;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_MEASUREMENTS |
                                   VN_MONITOR_PERM_CAPTURE_CONTROL |
                                   VN_MONITOR_PERM_RAW_AUDIO |
                                   VN_MONITOR_PERM_OBSERVE_EVENTS;
    desc.consumerQueueBytes = 1u << 20;
    return Vn_MonitorOpenSession(&desc, handle);
}

VnResult ApplyPeakPlan(VnMonitorSessionHandle handle, std::uint64_t leaseGeneration) {
    VnMonitorTapDescriptor tap = MakeMasterTap();
    VnMonitorMeasurementRequest request = MakePeak();
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.taps = &tap;
    plan.measurementCount = 1;
    plan.measurements = &request;
    plan.downsample = 1;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    return Vn_MonitorApplyCapturePlan(handle, leaseGeneration, &plan, &accepted);
}

struct PolledRecord {
    std::uint32_t kind = 0;
    std::uint64_t capturePlanGeneration = 0;
    std::uint64_t clockAnchorGeneration = 0;
    std::uint64_t measurementSegmentGeneration = 0;
    std::uint64_t executeFrame = 0;
    VnObjectId stableId = harkentest::ObjectId(0);
    std::uint32_t validity = 0;
};

std::vector<PolledRecord> PollRecords(VnMonitorSessionHandle handle) {
    std::vector<PolledRecord> out;
    std::vector<unsigned char> buffer(1u << 20, 0);
    VnMonitorPollDesc pollDesc{};
    pollDesc.structSize = sizeof(pollDesc);
    pollDesc.buffer = buffer.data();
    pollDesc.bufferBytes = buffer.size();
    VnMonitorPollResult result{};
    result.structSize = sizeof(result);
    if (Vn_MonitorPoll(handle, &pollDesc, &result) != VN_OK ||
        result.bytesWritten == 0) {
        return out;
    }
    std::uint64_t offset = 0;
    while (offset + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES <= result.bytesWritten) {
        const unsigned char* rec = buffer.data() + offset;
        const std::uint32_t kind = static_cast<std::uint32_t>(rec[0]) |
                                   (static_cast<std::uint32_t>(rec[1]) << 8) |
                                   (static_cast<std::uint32_t>(rec[2]) << 16) |
                                   (static_cast<std::uint32_t>(rec[3]) << 24);
        const std::uint64_t recordBytes = ReadU64(rec, 16);
        PolledRecord item{};
        item.kind = kind;
        item.capturePlanGeneration =
            ReadU64(rec, harken::monitor::kPackedCapturePlanGenerationOffset);
        item.clockAnchorGeneration =
            ReadU64(rec, harken::monitor::kPackedClockAnchorGenerationOffset);
        item.measurementSegmentGeneration =
            ReadU64(rec, harken::monitor::kPackedMeasurementSegmentGenerationOffset);
        item.executeFrame = ReadU64(rec, harken::monitor::kPackedExecuteFrameOffset);
        item.stableId = harkentest::ObjectId(ReadU64(rec, 176));
        out.push_back(item);
        if (recordBytes == 0) {
            break;
        }
        offset += recordBytes;
    }
    return out;
}

} // namespace

TEST(MonitorMeasure, SegmentPackedAudioKeepsOwnPlanGeneration) {
    std::array<float, 8> samples = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
    std::array<std::uint8_t, 2048> packed{};
    const std::uint32_t bytes = harken::monitor::PackAudioChunkRecord(
        packed.data(), static_cast<std::uint32_t>(packed.size()), 11, 7, 19, 21, 5,
        0, 48000, 1, 0, 0, 8, samples.data(), 8, 3);
    ASSERT_NE(0u, bytes);
    EXPECT_EQ(7u, ReadU64(packed.data(),
                          harken::monitor::kPackedCapturePlanGenerationOffset));
    EXPECT_EQ(0u, ReadU64(packed.data(),
                          harken::monitor::kPackedClockAnchorGenerationOffset));
    EXPECT_EQ(3u, ReadU64(packed.data(),
                          harken::monitor::kPackedMeasurementSegmentGenerationOffset));
    EXPECT_EQ(19u, ReadU64(packed.data(), harken::monitor::kPackedExecuteFrameOffset));
    VnMonitorDecodedRecord decoded[1]{};
    std::uint8_t skip[8]{};
    const VnMonitorDecodeResult decodedResult =
        vn_monitor_decode_records(packed.data(), bytes, decoded, 1, skip, sizeof(skip));
    ASSERT_EQ(VN_OK, decodedResult.status);
    EXPECT_EQ(7u, decoded[0].header.capturePlanGeneration);
    EXPECT_EQ(0u, decoded[0].header.clockAnchorGeneration);
    EXPECT_EQ(3u, decoded[0].header.measurementSegmentGeneration);
    EXPECT_EQ(19u, decoded[0].header.executeFrame);
}

TEST(MonitorMeasure, SegmentLayoutChangeIncrementsPackedField) {
    EXPECT_FALSE(harken::monitor::AudioFormatChangesSegment(0, 0, 0, 48000, 1, 0));
    EXPECT_FALSE(harken::monitor::AudioFormatChangesSegment(48000, 1, 0, 48000, 1, 0));
    EXPECT_TRUE(harken::monitor::AudioFormatChangesSegment(48000, 1, 0, 44100, 1, 0));
    EXPECT_TRUE(harken::monitor::AudioFormatChangesSegment(48000, 1, 0, 48000, 2, 0));
    EXPECT_TRUE(harken::monitor::AudioFormatChangesSegment(48000, 2, 0, 48000, 2, 1));
    std::array<float, 8> mono = {0.1f, 0.2f, 0.3f, 0.4f, 0.5f, 0.6f, 0.7f, 0.8f};
    std::array<std::uint8_t, 4096> packed{};
    const std::uint32_t bytes = harken::monitor::PackAudioChunkRecord(
        packed.data(), static_cast<std::uint32_t>(packed.size()), 4, 2, 10, 11, 1, 0,
        44100, 2, 1, 0, 8, mono.data(), 4, 7);
    ASSERT_NE(0u, bytes);
    EXPECT_EQ(7u, ReadU64(packed.data(),
                          harken::monitor::kPackedMeasurementSegmentGenerationOffset));
}

TEST(MonitorMeasure, SegmentSiblingTapFormatDoesNotBumpSharedGeneration) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenAudioControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    lease.structSize = sizeof(lease);
    ASSERT_EQ(VN_OK, Vn_MonitorAcquireCaptureControl(handle, &lease));
    VnMonitorTapDescriptor taps[2]{};
    taps[0] = MakeMasterTap();
    taps[0].selectorMode = VN_MONITOR_TAP_SELECTOR_FOLLOW_SLOT;
    taps[1] = {};
    taps[1].structSize = sizeof(taps[1]);
    taps[1].tapKind = VN_MONITOR_TAP_VOICE_POST_CHAIN;
    taps[1].selectorMode = VN_MONITOR_TAP_SELECTOR_FOLLOW_SLOT;
    taps[1].ownerKind = VN_MONITOR_ENTITY_VOICE;
    taps[1].slotId = 0;
    VnMonitorMeasurementRequest request = MakePeak();
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 2;
    plan.taps = taps;
    plan.measurementCount = 1;
    plan.measurements = &request;
    plan.downsample = 1;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    harken::monitor::StopDrainForTests();
    const std::uint32_t masterIndex = session.Engine()->buses.FindIndex(VN_BUS_MASTER);
    ASSERT_NE(harken::kInvalidBusIndex, masterIndex);
    const std::uint32_t masterGeneration =
        session.Engine()->buses.GenerationAt(masterIndex);
    ASSERT_NE(0u, masterGeneration);
    std::array<float, 4> master = {0.25f, -0.5f, 0.125f, -0.25f};
    harken::monitor::PublishMasterAudioChunk(master.data(), 4, 1, 0, 48000,
                                            masterGeneration, 0);
    float voiceL[4] = {0.1f, 0.2f, 0.3f, 0.4f};
    float voiceR[4] = {0.0f, 0.0f, 0.0f, 0.0f};
    float* planes[2] = {voiceL, voiceR};
    harken::monitor::PublishPlanarAudioChunk(
        VN_MONITOR_TAP_VOICE_POST_CHAIN, 7, 1, 0, planes, 4, 2, 0, 1, 44100, 0);
    harken::monitor::PublishMasterAudioChunk(master.data(), 4, 1, 0, 48000,
                                            masterGeneration, 4);
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();
    std::uint64_t firstMaster = 0;
    std::uint64_t secondMaster = 0;
    std::uint32_t masterChunks = 0;
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        for (const PolledRecord& rec : PollRecords(handle)) {
            if (rec.kind != VN_MONITOR_RECORD_AUDIO_CHUNK) {
                continue;
            }
            ++masterChunks;
            if (firstMaster == 0) {
                firstMaster = rec.measurementSegmentGeneration;
            } else if (masterChunks >= 2 && secondMaster == 0) {
                secondMaster = rec.measurementSegmentGeneration;
            }
        }
        if (secondMaster != 0) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_NE(0u, firstMaster);
    EXPECT_EQ(firstMaster, secondMaster);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, SegmentDropIsNotNumericSilence) {
    VnMonitorDropSummary gap{};
    gap.structSize = sizeof(gap);
    gap.stage = VN_MONITOR_DROP_HUB_FANOUT;
    gap.recordCount = 1;
    gap.byteCount = 32;
    gap.reason = VN_ACTION_TERMINAL_WORLD_REPLACED;
    std::array<std::uint8_t, 512> packed{};
    const std::uint32_t bytes = harken::monitor::PackDropSummaryRecord(
        packed.data(), static_cast<std::uint32_t>(packed.size()), gap, 9);
    ASSERT_NE(0u, bytes);
    VnMonitorDecodedRecord decoded[1]{};
    std::uint8_t skip[8]{};
    const VnMonitorDecodeResult decodedResult =
        vn_monitor_decode_records(packed.data(), bytes, decoded, 1, skip, sizeof(skip));
    ASSERT_EQ(VN_OK, decodedResult.status);
    EXPECT_EQ(VN_MONITOR_RECORD_DROP_SUMMARY, decoded[0].header.recordKind);
    EXPECT_EQ(0u, decoded[0].header.validityFlags & VN_MONITOR_VALID_NUMERIC);
    EXPECT_NE(VN_MONITOR_VALID_NUMERIC, gap.reason);
}

TEST(MonitorMeasure, SegmentRecorderRemoteStayModelOnly) {
    VnMonitorCapabilities caps{};
    caps.structSize = sizeof(caps);
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    ASSERT_EQ(VN_OK, Vn_MonitorGetCapabilities(&caps));
    EXPECT_EQ(0u, caps.remoteEnabled);
    RecordProperty("recorder_remote", "model_only");
}

TEST(MonitorMeasure, SegmentMixedGenerationKeepsG1Bytes) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenAudioControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    lease.structSize = sizeof(lease);
    ASSERT_EQ(VN_OK, Vn_MonitorAcquireCaptureControl(handle, &lease));
    ASSERT_EQ(VN_OK, ApplyPeakPlan(handle, lease.leaseGeneration));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    harken::monitor::StopDrainForTests();
    std::array<float, 4> samples = {0.25f, -0.5f, 0.125f, -0.25f};
    std::array<std::uint8_t, 8192> g1{};
    const std::uint32_t g1Bytes = harken::monitor::PackAudioChunkRecord(
        g1.data(), static_cast<std::uint32_t>(g1.size()), 4, 2, 10,
        VN_MONITOR_TAP_MASTER_OUTPUT, 1, 0, 48000, 1, 0, 0, 4, samples.data(), 4, 1);
    ASSERT_NE(0u, g1Bytes);
    ASSERT_TRUE(harken::monitor::PublishConformancePackedForTests(g1.data(), g1Bytes));
    ASSERT_EQ(VN_OK, ApplyPeakPlan(handle, lease.leaseGeneration));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();
    bool sawG1 = false;
    bool sawMeterFromG1 = false;
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        for (const PolledRecord& rec : PollRecords(handle)) {
            if (rec.kind == VN_MONITOR_RECORD_AUDIO_CHUNK &&
                rec.capturePlanGeneration == 2u) {
                sawG1 = true;
            }
            if (rec.kind == VN_MONITOR_RECORD_METER &&
                rec.capturePlanGeneration == 2u) {
                sawMeterFromG1 = true;
            }
        }
        if (sawG1) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(sawG1);
    EXPECT_TRUE(sawMeterFromG1);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, SegmentStopDoesNotFanoutRetiredAudio) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenAudioControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    lease.structSize = sizeof(lease);
    ASSERT_EQ(VN_OK, Vn_MonitorAcquireCaptureControl(handle, &lease));
    ASSERT_EQ(VN_OK, ApplyPeakPlan(handle, lease.leaseGeneration));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    harken::monitor::StopDrainForTests();
    VnMonitorEffectiveCapturePlan stopped{};
    stopped.structSize = sizeof(stopped);
    ASSERT_EQ(VN_OK, Vn_MonitorStopCapture(handle, lease.leaseGeneration, &stopped));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    std::array<float, 4> samples = {0.25f, -0.5f, 0.125f, -0.25f};
    std::array<std::uint8_t, 8192> packed{};
    const std::uint32_t bytes = harken::monitor::PackAudioChunkRecord(
        packed.data(), static_cast<std::uint32_t>(packed.size()), 9, 2, 10, 11, 1, 0,
        48000, 1, 0, 0, 4, samples.data(), 4, 1);
    ASSERT_NE(0u, bytes);
    ASSERT_TRUE(harken::monitor::PublishConformancePackedForTests(packed.data(), bytes));
    harken::monitor::DrainOnceForTests();
    harken::monitor::StartDrainForTests();
    bool sawAudio = false;
    bool sawMeter = false;
    for (int i = 0; i < 8; ++i) {
        for (const PolledRecord& rec : PollRecords(handle)) {
            if (rec.kind == VN_MONITOR_RECORD_AUDIO_CHUNK) {
                sawAudio = true;
            }
            if (rec.kind == VN_MONITOR_RECORD_METER) {
                sawMeter = true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_FALSE(sawAudio);
    EXPECT_FALSE(sawMeter);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}
