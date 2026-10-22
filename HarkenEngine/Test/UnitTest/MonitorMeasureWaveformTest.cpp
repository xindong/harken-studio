#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "MonitorCaptureHubTestHooks.h"
#include "MonitorDerivedDispatch.h"
#include "MonitorRecordPack.h"
#include "Common/TestUtil.h"
#include "monitor_fixture_decode.h"
#include "harken/core/MonitorWaveformReduce.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <thread>
#include <vector>

namespace {

VnMonitorMeasurementRequest MakeWaveform(std::uint32_t bucketFrames,
                                         std::uint32_t downsample = 1) {
    VnMonitorMeasurementRequest request{};
    request.structSize = sizeof(request);
    request.kind = VN_MONITOR_MEASURE_WAVEFORM;
    request.channelMode = VN_MONITOR_CHANNEL_PER_CHANNEL;
    request.downsample = downsample;
    request.bucketFrames = bucketFrames;
    return request;
}

} // namespace

TEST(MonitorMeasure, WaveformRejectsNonUnityDownsample) {
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED,
              harken::core::ValidateWaveformRequest(MakeWaveform(8, 2)));
}

TEST(MonitorMeasure, WaveformRejectsZeroBucketFrames) {
    EXPECT_EQ(VN_INVALID_ARG, harken::core::ValidateWaveformRequest(MakeWaveform(0)));
}

TEST(MonitorMeasure, WaveformAbsoluteBucketsMatchAcrossBatches) {
    harken::core::MonitorWaveformReducer once{};
    once.Reset(4, VN_MONITOR_CHANNEL_PER_CHANNEL, 0);
    harken::core::MonitorWaveformReducer split{};
    split.Reset(4, VN_MONITOR_CHANNEL_PER_CHANNEL, 0);
    const float samples[8] = {0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f, 0.7f, -0.8f};
    harken::core::MonitorWaveformBucketResult a[4]{};
    harken::core::MonitorWaveformBucketResult b[4]{};
    std::uint32_t ac = 0;
    std::uint32_t bc = 0;
    ASSERT_TRUE(harken::core::ReduceWaveformChunk(&once, samples, 8, 1, 0, a, 4, &ac));
    ASSERT_TRUE(harken::core::ReduceWaveformChunk(&split, samples, 3, 1, 0, b, 4, &bc));
    std::uint32_t extra = 0;
    ASSERT_TRUE(harken::core::ReduceWaveformChunk(&split, samples + 3, 5, 1, 3,
                                                 b + bc, 4 - bc, &extra));
    bc += extra;
    ASSERT_EQ(2u, ac);
    ASSERT_EQ(2u, bc);
    EXPECT_TRUE(a[0].complete);
    EXPECT_TRUE(a[1].complete);
    EXPECT_EQ(a[0].sourceFrameBegin, b[0].sourceFrameBegin);
    EXPECT_EQ(a[0].minValue, b[0].minValue);
    EXPECT_EQ(a[0].maxValue, b[0].maxValue);
    EXPECT_EQ(a[1].minValue, b[1].minValue);
    EXPECT_EQ(a[1].maxValue, b[1].maxValue);
}

TEST(MonitorMeasure, WaveformPartialIsNonNumeric) {
    harken::core::MonitorWaveformReducer reducer{};
    reducer.Reset(8, VN_MONITOR_CHANNEL_PER_CHANNEL, 0);
    const float samples[3] = {0.25f, -0.5f, 0.125f};
    harken::core::MonitorWaveformBucketResult out[2]{};
    std::uint32_t count = 0;
    ASSERT_TRUE(harken::core::ReduceWaveformChunk(&reducer, samples, 3, 1, 0, out, 2,
                                                 &count));
    EXPECT_EQ(0u, count);
    const harken::core::MonitorWaveformBucketResult discarded =
        harken::core::FlushWaveformPartial(&reducer);
    EXPECT_TRUE(discarded.discarded);
    EXPECT_FALSE(discarded.complete);
    EXPECT_EQ(3u, discarded.validSampleCount);
    EXPECT_EQ(0.0f, discarded.minValue);
    EXPECT_EQ(0.0f, discarded.maxValue);
}

TEST(MonitorMeasure, WaveformIdentityChangeDiscardsPartial) {
    harken::core::MonitorWaveformReducer reducer{};
    reducer.Reset(8, VN_MONITOR_CHANNEL_PER_CHANNEL, 0);
    const float samples[3] = {0.25f, -0.5f, 0.125f};
    harken::core::MonitorWaveformBucketResult out[2]{};
    std::uint32_t count = 0;
    ASSERT_TRUE(harken::core::ReduceWaveformChunk(&reducer, samples, 3, 1, 0, out, 2,
                                                 &count));
    harken::core::MonitorWaveformBucketResult discarded{};
    harken::core::NoteWaveformSegment(&reducer, 1, &discarded);
    EXPECT_TRUE(discarded.discarded);
    EXPECT_EQ(3u, discarded.validSampleCount);
    reducer.Reset(4, VN_MONITOR_CHANNEL_PER_CHANNEL, 1);
    EXPECT_EQ(0u, reducer.filledCount);
}

TEST(MonitorMeasure, WaveformPlanAcceptsMeasurementRequest) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO |
                                   VN_MONITOR_STREAM_EVENT;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_MEASUREMENTS |
                                   VN_MONITOR_PERM_CAPTURE_CONTROL |
                                   VN_MONITOR_PERM_RAW_AUDIO;
    desc.consumerQueueBytes = 65536;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    lease.structSize = sizeof(lease);
    ASSERT_EQ(VN_OK, Vn_MonitorAcquireCaptureControl(handle, &lease));
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapKind = VN_MONITOR_TAP_MASTER_OUTPUT;
    tap.ownerKind = VN_MONITOR_ENTITY_BUS;
    tap.selectorMode = VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE;
    tap.ownerStableId = VN_BUS_MASTER;
    VnMonitorMeasurementRequest request = MakeWaveform(64);
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
    ASSERT_EQ(VN_OK, Vn_MonitorValidateCapturePlan(handle, lease.leaseGeneration,
                                                   &plan, &accepted));
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                &accepted));
    EXPECT_EQ(1u, accepted.accepted.measurementCount);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, WaveformPublishesCompleteBucketOnPoll) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO |
                                   VN_MONITOR_STREAM_EVENT;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_MEASUREMENTS |
                                   VN_MONITOR_PERM_CAPTURE_CONTROL |
                                   VN_MONITOR_PERM_RAW_AUDIO |
                                   VN_MONITOR_PERM_OBSERVE_EVENTS;
    desc.consumerQueueBytes = 1u << 20;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    lease.structSize = sizeof(lease);
    ASSERT_EQ(VN_OK, Vn_MonitorAcquireCaptureControl(handle, &lease));
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapKind = VN_MONITOR_TAP_MASTER_OUTPUT;
    tap.ownerKind = VN_MONITOR_ENTITY_BUS;
    tap.selectorMode = VN_MONITOR_TAP_SELECTOR_EXACT_INSTANCE;
    tap.ownerStableId = VN_BUS_MASTER;
    VnMonitorMeasurementRequest request = MakeWaveform(64);
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
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    const std::vector<float> sine =
        harkentest::MakeSine(256, 750.0, 48000.0, 0.7f);
    const VnPCMDesc pcm = harkentest::MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(21), &pcm));
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(21));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    harkentest::Capture out;
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    bool sawBucket = false;
    const auto start = std::chrono::steady_clock::now();
    while (std::chrono::steady_clock::now() - start < std::chrono::seconds(2)) {
        std::vector<unsigned char> buffer(1u << 20, 0);
        VnMonitorPollDesc pollDesc{};
        pollDesc.structSize = sizeof(pollDesc);
        pollDesc.buffer = buffer.data();
        pollDesc.bufferBytes = buffer.size();
        VnMonitorPollResult result{};
        result.structSize = sizeof(result);
        ASSERT_EQ(VN_OK, Vn_MonitorPoll(handle, &pollDesc, &result));
        std::uint64_t offset = 0;
        while (offset + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES <=
               result.bytesWritten) {
            const unsigned char* rec = buffer.data() + offset;
            const std::uint32_t kind = static_cast<std::uint32_t>(rec[0]) |
                                       (static_cast<std::uint32_t>(rec[1]) << 8) |
                                       (static_cast<std::uint32_t>(rec[2]) << 16) |
                                       (static_cast<std::uint32_t>(rec[3]) << 24);
            const std::uint64_t recordBytes =
                static_cast<std::uint64_t>(rec[16]) |
                (static_cast<std::uint64_t>(rec[17]) << 8) |
                (static_cast<std::uint64_t>(rec[18]) << 16) |
                (static_cast<std::uint64_t>(rec[19]) << 24) |
                (static_cast<std::uint64_t>(rec[20]) << 32) |
                (static_cast<std::uint64_t>(rec[21]) << 40) |
                (static_cast<std::uint64_t>(rec[22]) << 48) |
                (static_cast<std::uint64_t>(rec[23]) << 56);
            if (kind == VN_MONITOR_RECORD_WAVEFORM_BUCKET) {
                sawBucket = true;
                const unsigned char* p =
                    rec + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES;
                EXPECT_EQ(88u, p[0]);
                EXPECT_EQ(64u, p[4]);
                EXPECT_EQ(0u, p[72]);
                const std::uint64_t begin = static_cast<std::uint64_t>(p[24]) |
                                            (static_cast<std::uint64_t>(p[25]) << 8) |
                                            (static_cast<std::uint64_t>(p[26]) << 16) |
                                            (static_cast<std::uint64_t>(p[27]) << 24);
                const std::uint64_t end = static_cast<std::uint64_t>(p[32]) |
                                          (static_cast<std::uint64_t>(p[33]) << 8) |
                                          (static_cast<std::uint64_t>(p[34]) << 16) |
                                          (static_cast<std::uint64_t>(p[35]) << 24);
                EXPECT_EQ(64u, end - begin);
                break;
            }
            if (recordBytes == 0) {
                break;
            }
            offset += recordBytes;
        }
        if (sawBucket) {
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(10));
    }
    EXPECT_TRUE(sawBucket);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, WaveformOverflowDoesNotDropUnwrittenBuckets) {
    harken::core::MonitorWaveformReducer reducer{};
    reducer.Reset(4, VN_MONITOR_CHANNEL_PER_CHANNEL, 0);
    std::vector<float> samples(16);
    for (std::uint32_t i = 0; i < samples.size(); ++i) {
        samples[i] = static_cast<float>(i + 1);
    }
    harken::core::MonitorWaveformBucketResult tight[2]{};
    std::uint32_t tightCount = 0;
    EXPECT_FALSE(harken::core::ReduceWaveformChunk(
        &reducer, samples.data(), 16, 1, 0, tight, 2, &tightCount));
    EXPECT_EQ(2u, tightCount);
    EXPECT_TRUE(tight[0].complete);
    EXPECT_TRUE(tight[1].complete);
    EXPECT_EQ(0u, tight[0].sourceFrameBegin);
    EXPECT_EQ(4u, tight[1].sourceFrameBegin);
    EXPECT_EQ(3u, reducer.filledCount);
    EXPECT_EQ(8u, reducer.filledBegin);

    harken::core::MonitorWaveformBucketResult rest[4]{};
    std::uint32_t restCount = 0;
    ASSERT_TRUE(harken::core::ReduceWaveformChunk(
        &reducer, samples.data() + 11, 5, 1, 11, rest, 4, &restCount));
    ASSERT_EQ(2u, restCount);
    EXPECT_TRUE(rest[0].complete);
    EXPECT_TRUE(rest[1].complete);
    EXPECT_EQ(8u, rest[0].sourceFrameBegin);
    EXPECT_EQ(12u, rest[1].sourceFrameBegin);
    EXPECT_EQ(9.0f, rest[0].minValue);
    EXPECT_EQ(12.0f, rest[0].maxValue);
}

TEST(MonitorMeasure, WaveformDispatchIsolatesMeasurementsAndCopiesEnvelope) {
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapDescriptorId = 11;
    tap.tapDescriptorGeneration = 3;
    VnMonitorMeasurementRequest requests[2] = {MakeWaveform(4), MakeWaveform(8)};
    requests[1].tapIndex = 0;
    std::array<float, 8> samples = {0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f, 0.7f, -0.8f};
    std::array<std::uint8_t, 2048> packed{};
    const std::uint32_t packedBytes = harken::monitor::PackAudioChunkRecord(
        packed.data(), static_cast<std::uint32_t>(packed.size()), 42, 7, 19, 11, 3, 0,
        48000, 1, 0, 0, 8, samples.data(), 8);
    ASSERT_NE(0u, packedBytes);

    struct Collected {
        std::vector<std::vector<std::uint8_t>> records;
    } collected;
    harken::monitor::DerivedDrainState derived;
    ASSERT_TRUE(derived.Prepare(requests, 2, 48000));
    derived.Dispatch(
        requests, 2, &tap, 1, packed.data(), packedBytes,
        [](void* raw, const harken::monitor::DerivedEmit& emit) {
            auto* typed = static_cast<Collected*>(raw);
            typed->records.emplace_back(emit.bytes, emit.bytes + emit.size);
            return true;
        },
        &collected);

    std::uint32_t fourFrameBuckets = 0;
    std::uint32_t eightFrameBuckets = 0;
    for (const auto& rec : collected.records) {
        ASSERT_GE(rec.size(), VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES);
        VnMonitorDecodedRecord decoded[1]{};
        std::uint8_t skip[8]{};
        const VnMonitorDecodeResult decodedResult = vn_monitor_decode_records(
            rec.data(), rec.size(), decoded, 1, skip, sizeof(skip));
        ASSERT_EQ(VN_OK, decodedResult.status);
        ASSERT_EQ(1u, decodedResult.recordsWritten);
        EXPECT_EQ(42u, decoded[0].header.sourceSequence);
        EXPECT_EQ(7u, decoded[0].header.capturePlanGeneration);
        EXPECT_EQ(19u, decoded[0].header.executeFrame);
        ASSERT_GE(decoded[0].payloadBytes, 8u);
        const std::uint32_t bucketFrames =
            static_cast<std::uint32_t>(decoded[0].payload[4]) |
            (static_cast<std::uint32_t>(decoded[0].payload[5]) << 8) |
            (static_cast<std::uint32_t>(decoded[0].payload[6]) << 16) |
            (static_cast<std::uint32_t>(decoded[0].payload[7]) << 24);
        if (bucketFrames == 4) {
            ++fourFrameBuckets;
        } else if (bucketFrames == 8) {
            ++eightFrameBuckets;
        }
    }
    EXPECT_EQ(2u, fourFrameBuckets);
    EXPECT_EQ(1u, eightFrameBuckets);

    VnMonitorTapDescriptor otherTap = tap;
    otherTap.tapDescriptorId = 99;
    otherTap.tapDescriptorGeneration = 1;
    collected.records.clear();
    derived.Dispatch(
        requests, 2, &otherTap, 1, packed.data(), packedBytes,
        [](void* raw, const harken::monitor::DerivedEmit& emit) {
            auto* typed = static_cast<Collected*>(raw);
            typed->records.emplace_back(emit.bytes, emit.bytes + emit.size);
            return true;
        },
        &collected);
    EXPECT_TRUE(collected.records.empty());
}

TEST(MonitorMeasure, WaveformPlanGenerationChangeDiscardsPartial) {
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapDescriptorId = 11;
    tap.tapDescriptorGeneration = 3;
    VnMonitorMeasurementRequest request = MakeWaveform(8);
    std::array<float, 3> first = {0.1f, -0.2f, 0.3f};
    std::array<float, 8> second = {0.4f, -0.5f, 0.6f, -0.7f, 0.8f, -0.9f, 1.0f, -1.1f};
    std::array<std::uint8_t, 2048> packedOld{};
    std::array<std::uint8_t, 2048> packedNew{};
    const std::uint32_t oldBytes = harken::monitor::PackAudioChunkRecord(
        packedOld.data(), static_cast<std::uint32_t>(packedOld.size()), 10, 7, 1, 11,
        3, 0, 48000, 1, 0, 0, 3, first.data(), 3);
    const std::uint32_t newBytes = harken::monitor::PackAudioChunkRecord(
        packedNew.data(), static_cast<std::uint32_t>(packedNew.size()), 11, 8, 2, 11,
        3, 0, 48000, 1, 0, 0, 8, second.data(), 8);
    ASSERT_NE(0u, oldBytes);
    ASSERT_NE(0u, newBytes);

    struct Collected {
        std::vector<std::uint32_t> kinds;
        std::vector<std::uint32_t> partials;
    } collected;
    auto emit = [](void* raw, const harken::monitor::DerivedEmit& emitRec) {
        auto* typed = static_cast<Collected*>(raw);
        const std::uint32_t kind =
            static_cast<std::uint32_t>(emitRec.bytes[0]) |
            (static_cast<std::uint32_t>(emitRec.bytes[1]) << 8) |
            (static_cast<std::uint32_t>(emitRec.bytes[2]) << 16) |
            (static_cast<std::uint32_t>(emitRec.bytes[3]) << 24);
        typed->kinds.push_back(kind);
        const std::uint8_t* p = emitRec.bytes + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES;
        typed->partials.push_back(p[72]);
        return true;
    };
    harken::monitor::DerivedDrainState derived;
    ASSERT_TRUE(derived.Prepare(&request, 1, 48000));
    derived.Dispatch(&request, 1, &tap, 1, packedOld.data(),
                                              oldBytes, emit, &collected);
    EXPECT_TRUE(collected.kinds.empty());
    harken::monitor::DerivedDrainState replacement;
    ASSERT_TRUE(replacement.Prepare(&request, 1, 48000, &derived));
    derived = {};
    replacement.Dispatch(&request, 1, &tap, 1, packedNew.data(),
                         newBytes, emit, &collected);
    ASSERT_GE(collected.kinds.size(), 2u);
    EXPECT_EQ(VN_MONITOR_RECORD_PARTIAL_BUCKET_DISCARDED, collected.kinds[0]);
    EXPECT_EQ(1u, collected.partials[0]);
    EXPECT_EQ(VN_MONITOR_RECORD_WAVEFORM_BUCKET, collected.kinds[1]);
    EXPECT_EQ(0u, collected.partials[1]);
}

TEST(MonitorMeasure, WaveformBucketKeepsCrossChunkSequenceRange) {
    harken::core::MonitorWaveformReducer reducer{};
    reducer.Reset(8, VN_MONITOR_CHANNEL_PER_CHANNEL, 0);
    const float first[3] = {0.1f, -0.2f, 0.3f};
    const float second[5] = {0.4f, -0.5f, 0.6f, -0.7f, 0.8f};
    harken::core::MonitorWaveformBucketResult out[2]{};
    std::uint32_t count = 0;
    ASSERT_TRUE(harken::core::ReduceWaveformChunk(&reducer, first, 3, 1, 0, out, 2,
                                                 &count, 10));
    EXPECT_EQ(0u, count);
    ASSERT_TRUE(harken::core::ReduceWaveformChunk(&reducer, second, 5, 1, 3, out, 2,
                                                 &count, 11));
    ASSERT_EQ(1u, count);
    EXPECT_TRUE(out[0].complete);
    EXPECT_EQ(10u, out[0].sourceSequenceFirst);
    EXPECT_EQ(11u, out[0].sourceSequenceLast);
}
