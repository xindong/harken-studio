#include <harken/Harken.h>

#include "MonitorCaptureHubTestHooks.h"
#include "MonitorDerivedDispatch.h"
#include "MonitorRecordPack.h"
#include "Common/TestUtil.h"
#include "monitor_fixture_decode.h"
#include "harken/core/MonitorScopeTrigger.h"

#include <gtest/gtest.h>

#include <array>
#include <chrono>
#include <cstdint>
#include <limits>
#include <thread>
#include <vector>

namespace {

VnMonitorMeasurementRequest MakeScope(float threshold, std::uint32_t edge) {
    VnMonitorMeasurementRequest request{};
    request.structSize = sizeof(request);
    request.kind = VN_MONITOR_MEASURE_SCOPE;
    request.channelMode = VN_MONITOR_CHANNEL_MONO_AVERAGE;
    request.downsample = 1;
    request.overlapNumerator = 2;
    request.overlapDenominator = 2;
    request.triggerThreshold = threshold;
    request.hysteresis = 0.1f;
    request.triggerMode = edge;
    return request;
}

} // namespace

TEST(MonitorMeasure, ScopeOverlayMapsFrozenRequestFields) {
    harken::core::MonitorScopeOverlay overlay{};
    ASSERT_EQ(VN_OK, harken::core::DecodeScopeOverlay(MakeScope(0.5f, 0), &overlay));
    EXPECT_EQ(2u, overlay.preTriggerFrames);
    EXPECT_EQ(2u, overlay.postTriggerFrames);
    EXPECT_EQ(16u, overlay.maxSearchFrames);
    EXPECT_EQ(0u, overlay.edge);
}

TEST(MonitorMeasure, ScopeRisingEqualityIsSampleExact) {
    harken::core::MonitorScopeOverlay overlay{};
    ASSERT_EQ(VN_OK, harken::core::DecodeScopeOverlay(MakeScope(0.5f, 0), &overlay));
    harken::core::MonitorScopeState state{};
    harken::core::ResetScopeState(&state, 0);
    (void)harken::core::FeedScopeSample(&state, overlay, -1.0f, 0);
    (void)harken::core::FeedScopeSample(&state, overlay, -1.0f, 1);
    const harken::core::MonitorScopeResult hit =
        harken::core::FeedScopeSample(&state, overlay, 0.5f, 2);
    EXPECT_TRUE(hit.triggered);
    EXPECT_FALSE(hit.noTrigger);
    EXPECT_EQ(2u, hit.triggerFrame);
}

TEST(MonitorMeasure, ScopeNoTriggerDoesNotUseFrameZero) {
    harken::core::MonitorScopeOverlay overlay{};
    ASSERT_EQ(VN_OK, harken::core::DecodeScopeOverlay(MakeScope(0.9f, 0), &overlay));
    overlay.maxSearchFrames = 4;
    harken::core::MonitorScopeState state{};
    harken::core::ResetScopeState(&state, 0);
    harken::core::MonitorScopeResult last{};
    harken::core::MonitorScopeResult noTrigger{};
    std::uint32_t noTriggerCount = 0;
    for (std::uint64_t frame = 0; frame < 8; ++frame) {
        last = harken::core::FeedScopeSample(&state, overlay, -0.2f, frame);
        if (last.noTrigger) {
            ++noTriggerCount;
            noTrigger = last;
        }
    }
    EXPECT_EQ(1u, noTriggerCount);
    EXPECT_FALSE(noTrigger.triggered);
    EXPECT_EQ(0u, noTrigger.triggerFrame);
    EXPECT_NE(noTrigger.windowBegin, noTrigger.windowEnd);
}

TEST(MonitorMeasure, ScopeNonFiniteSampleEmitsNoTriggerOnce) {
    harken::core::MonitorScopeOverlay overlay{};
    ASSERT_EQ(VN_OK, harken::core::DecodeScopeOverlay(MakeScope(0.5f, 0), &overlay));
    harken::core::MonitorScopeState state{};
    harken::core::ResetScopeState(&state, 0);
    std::uint32_t noTriggerCount = 0;
    for (int i = 0; i < 8; ++i) {
        const harken::core::MonitorScopeResult item = harken::core::FeedScopeSample(
            &state, overlay, std::numeric_limits<float>::quiet_NaN(),
            static_cast<std::uint64_t>(i));
        if (item.noTrigger) {
            ++noTriggerCount;
        }
    }
    EXPECT_EQ(1u, noTriggerCount);
}

TEST(MonitorMeasure, ScopeWarmupDoesNotTriggerOnFirstCrossing) {
    harken::core::MonitorScopeOverlay overlay{};
    ASSERT_EQ(VN_OK, harken::core::DecodeScopeOverlay(MakeScope(0.5f, 0), &overlay));
    harken::core::MonitorScopeState state{};
    harken::core::ResetScopeState(&state, 0);
    (void)harken::core::FeedScopeSample(&state, overlay, -1.0f, 0);
    const harken::core::MonitorScopeResult early =
        harken::core::FeedScopeSample(&state, overlay, 0.5f, 1);
    EXPECT_FALSE(early.triggered);
}

TEST(MonitorMeasure, ScopeRejectsMissingPreTriggerOverlay) {
    VnMonitorMeasurementRequest request = MakeScope(0.1f, 0);
    request.overlapNumerator = 0;
    harken::core::MonitorScopeOverlay overlay{};
    EXPECT_EQ(VN_INVALID_ARG, harken::core::DecodeScopeOverlay(request, &overlay));
}

TEST(MonitorMeasure, ScopeRejectsMissingPostTriggerOverlay) {
    VnMonitorMeasurementRequest request = MakeScope(0.1f, 0);
    request.overlapDenominator = 0;
    harken::core::MonitorScopeOverlay overlay{};
    EXPECT_EQ(VN_INVALID_ARG, harken::core::DecodeScopeOverlay(request, &overlay));
}

TEST(MonitorMeasure, ScopeClampsMaxSearchToCapabilityWindow) {
    VnMonitorMeasurementRequest request = MakeScope(0.5f, 0);
    request.cadenceHz = 1;
    harken::core::MonitorScopeOverlay overlay{};
    ASSERT_EQ(VN_OK, harken::core::DecodeScopeOverlay(request, &overlay));
    EXPECT_EQ(4u, overlay.maxSearchFrames);
    request.cadenceHz = 100000;
    ASSERT_EQ(VN_OK, harken::core::DecodeScopeOverlay(request, &overlay));
    EXPECT_EQ(64u, overlay.maxSearchFrames);
}

TEST(MonitorMeasure, ScopePlanRejectsMissingPreTrigger) {
    harkentest::EngineSession engine(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_MEASUREMENTS |
                                   VN_MONITOR_PERM_CAPTURE_CONTROL |
                                   VN_MONITOR_PERM_RAW_AUDIO;
    desc.consumerQueueBytes = 4096;
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
    VnMonitorMeasurementRequest request = MakeScope(0.5f, 0);
    request.overlapNumerator = 0;
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.taps = &tap;
    plan.measurementCount = 1;
    plan.measurements = &request;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    EXPECT_EQ(VN_INVALID_ARG, Vn_MonitorValidateCapturePlan(
                                  handle, lease.leaseGeneration, &plan, &accepted));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, ScopePlanAcceptsMeasurementRequest) {
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
    VnMonitorMeasurementRequest request = MakeScope(0.5f, 0);
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

TEST(MonitorMeasure, ScopeDispatchPublishesTriggerAndNoTriggerBytes) {
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapDescriptorId = 11;
    tap.tapDescriptorGeneration = 3;
    VnMonitorMeasurementRequest request = MakeScope(0.5f, 0);
    request.cadenceHz = 4;
    std::array<float, 8> samples = {-1.0f, -1.0f, 0.5f, 0.6f, -1.0f, -1.0f, -1.0f,
                                    -1.0f};
    std::array<std::uint8_t, 4096> packed{};
    const std::uint32_t packedBytes = harken::monitor::PackAudioChunkRecord(
        packed.data(), static_cast<std::uint32_t>(packed.size()), 42, 7, 19, 11, 3, 0,
        48000, 1, 0, 0, 8, samples.data(), 8);
    ASSERT_NE(0u, packedBytes);

    struct Collected {
        std::vector<std::vector<std::uint8_t>> records;
    } collected;
    harken::monitor::DispatchDerivedAudioChunk(
        &request, 1, &tap, 1, packed.data(), packedBytes,
        [](void* raw, const harken::monitor::DerivedEmit& emit) {
            auto* typed = static_cast<Collected*>(raw);
            typed->records.emplace_back(emit.bytes, emit.bytes + emit.size);
            return true;
        },
        &collected);
    ASSERT_FALSE(collected.records.empty());
    bool sawTrigger = false;
    bool sawNoTrigger = false;
    for (const auto& rec : collected.records) {
        VnMonitorDecodedRecord decoded[1]{};
        std::uint8_t skip[8]{};
        const VnMonitorDecodeResult decodedResult = vn_monitor_decode_records(
            rec.data(), rec.size(), decoded, 1, skip, sizeof(skip));
        ASSERT_EQ(VN_OK, decodedResult.status);
        ASSERT_EQ(1u, decodedResult.recordsWritten);
        EXPECT_EQ(VN_MONITOR_RECORD_SCOPE_RESULT, decoded[0].header.recordKind);
        EXPECT_EQ(42u, decoded[0].header.sourceSequence);
        EXPECT_EQ(7u, decoded[0].header.capturePlanGeneration);
        EXPECT_EQ(19u, decoded[0].header.executeFrame);
        ASSERT_GE(decoded[0].payloadBytes, 144u);
        EXPECT_EQ(144u, decoded[0].payload[0]);
        const std::uint32_t noTrigger =
            static_cast<std::uint32_t>(decoded[0].payload[64]);
        const std::uint64_t triggerFrame =
            static_cast<std::uint64_t>(decoded[0].payload[72]) |
            (static_cast<std::uint64_t>(decoded[0].payload[73]) << 8);
        const std::uint64_t windowBegin =
            static_cast<std::uint64_t>(decoded[0].payload[80]) |
            (static_cast<std::uint64_t>(decoded[0].payload[81]) << 8);
        const std::uint64_t windowEnd =
            static_cast<std::uint64_t>(decoded[0].payload[88]) |
            (static_cast<std::uint64_t>(decoded[0].payload[89]) << 8);
        if (noTrigger == 0) {
            sawTrigger = true;
            EXPECT_EQ(2u, triggerFrame);
            EXPECT_NE(windowBegin, windowEnd);
            EXPECT_NE(0u, decoded[0].header.validityFlags & VN_MONITOR_VALID_NUMERIC);
        } else {
            sawNoTrigger = true;
            EXPECT_EQ(0u, triggerFrame);
            EXPECT_NE(windowBegin, windowEnd);
            EXPECT_EQ(0u, decoded[0].header.validityFlags & VN_MONITOR_VALID_NUMERIC);
        }
    }
    EXPECT_TRUE(sawTrigger);
    EXPECT_TRUE(sawNoTrigger);

    VnMonitorTapDescriptor other = tap;
    other.tapDescriptorId = 99;
    collected.records.clear();
    harken::monitor::DispatchDerivedAudioChunk(
        &request, 1, &other, 1, packed.data(), packedBytes,
        [](void* raw, const harken::monitor::DerivedEmit& emit) {
            auto* typed = static_cast<Collected*>(raw);
            typed->records.emplace_back(emit.bytes, emit.bytes + emit.size);
            return true;
        },
        &collected);
    EXPECT_TRUE(collected.records.empty());
}

TEST(MonitorMeasure, ScopePlanGenerationChangeResetsWarmup) {
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapDescriptorId = 11;
    tap.tapDescriptorGeneration = 3;
    VnMonitorMeasurementRequest request = MakeScope(0.5f, 0);
    std::array<float, 1> first = {-1.0f};
    std::array<float, 3> second = {-1.0f, -1.0f, 0.5f};
    std::array<std::uint8_t, 2048> packedOld{};
    std::array<std::uint8_t, 2048> packedNew{};
    const std::uint32_t oldBytes = harken::monitor::PackAudioChunkRecord(
        packedOld.data(), static_cast<std::uint32_t>(packedOld.size()), 10, 7, 1, 11,
        3, 0, 48000, 1, 0, 0, 1, first.data(), 1);
    const std::uint32_t newBytes = harken::monitor::PackAudioChunkRecord(
        packedNew.data(), static_cast<std::uint32_t>(packedNew.size()), 11, 8, 2, 11,
        3, 0, 48000, 1, 0, 0, 3, second.data(), 3);
    ASSERT_NE(0u, oldBytes);
    ASSERT_NE(0u, newBytes);
    struct Collected {
        std::uint32_t triggers = 0;
    } collected;
    auto emit = [](void* raw, const harken::monitor::DerivedEmit& emitRec) {
        auto* typed = static_cast<Collected*>(raw);
        const std::uint32_t noTrigger =
            emitRec.bytes[VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES + 64];
        if (noTrigger == 0) {
            ++typed->triggers;
        }
        return true;
    };
    harken::monitor::DispatchDerivedAudioChunk(&request, 1, &tap, 1, packedOld.data(),
                                              oldBytes, emit, &collected);
    harken::monitor::DispatchDerivedAudioChunk(&request, 1, &tap, 1, packedNew.data(),
                                              newBytes, emit, &collected);
    EXPECT_EQ(1u, collected.triggers);
}

TEST(MonitorMeasure, ScopeTriggerCompletesAcrossChunks) {
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapDescriptorId = 11;
    tap.tapDescriptorGeneration = 3;
    VnMonitorMeasurementRequest request = MakeScope(0.5f, 0);
    std::array<float, 2> first = {-1.0f, -1.0f};
    std::array<float, 1> second = {0.5f};
    std::array<std::uint8_t, 2048> packedFirst{};
    std::array<std::uint8_t, 2048> packedSecond{};
    const std::uint32_t firstBytes = harken::monitor::PackAudioChunkRecord(
        packedFirst.data(), static_cast<std::uint32_t>(packedFirst.size()), 10, 7, 1,
        11, 3, 0, 48000, 1, 0, 0, 2, first.data(), 2);
    const std::uint32_t secondBytes = harken::monitor::PackAudioChunkRecord(
        packedSecond.data(), static_cast<std::uint32_t>(packedSecond.size()), 11, 7, 1,
        11, 3, 0, 48000, 1, 0, 2, 3, second.data(), 1);
    ASSERT_NE(0u, firstBytes);
    ASSERT_NE(0u, secondBytes);
    struct Collected {
        std::uint32_t triggers = 0;
        std::uint64_t triggerFrame = 0;
    } collected;
    auto emit = [](void* raw, const harken::monitor::DerivedEmit& emitRec) {
        auto* typed = static_cast<Collected*>(raw);
        const std::uint8_t* p = emitRec.bytes + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES;
        if (p[64] == 0) {
            ++typed->triggers;
            typed->triggerFrame = static_cast<std::uint64_t>(p[72]);
        }
        return true;
    };
    harken::monitor::DispatchDerivedAudioChunk(&request, 1, &tap, 1, packedFirst.data(),
                                              firstBytes, emit, &collected);
    EXPECT_EQ(0u, collected.triggers);
    harken::monitor::DispatchDerivedAudioChunk(&request, 1, &tap, 1, packedSecond.data(),
                                              secondBytes, emit, &collected);
    EXPECT_EQ(1u, collected.triggers);
    EXPECT_EQ(2u, collected.triggerFrame);
}

TEST(MonitorMeasure, ScopePerChannelUsesRequestedIndex) {
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapDescriptorId = 11;
    tap.tapDescriptorGeneration = 3;
    VnMonitorMeasurementRequest request = MakeScope(0.5f, 0);
    request.channelMode = VN_MONITOR_CHANNEL_PER_CHANNEL;
    request.windowKind = 1;
    const float stereo[6] = {-1.0f, -1.0f, -1.0f, -1.0f, -1.0f, 0.5f};
    std::array<std::uint8_t, 4096> packed{};
    const std::uint32_t packedBytes = harken::monitor::PackAudioChunkRecord(
        packed.data(), static_cast<std::uint32_t>(packed.size()), 42, 7, 19, 11, 3, 0,
        48000, 2, 0, 0, 3, stereo, 3);
    ASSERT_NE(0u, packedBytes);
    struct Collected {
        std::uint32_t triggers = 0;
        std::uint32_t channelIndex = 99;
    } collected;
    harken::monitor::DispatchDerivedAudioChunk(
        &request, 1, &tap, 1, packed.data(), packedBytes,
        [](void* raw, const harken::monitor::DerivedEmit& emit) {
            auto* typed = static_cast<Collected*>(raw);
            const std::uint8_t* p = emit.bytes + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES;
            if (p[64] == 0) {
                ++typed->triggers;
                typed->channelIndex = p[12];
            }
            return true;
        },
        &collected);
    EXPECT_EQ(1u, collected.triggers);
    EXPECT_EQ(1u, collected.channelIndex);
}

TEST(MonitorMeasure, ScopeSourceGapResetsHistoryAndFlagsDiscontinuity) {
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapDescriptorId = 11;
    tap.tapDescriptorGeneration = 3;
    VnMonitorMeasurementRequest request = MakeScope(0.5f, 0);
    std::array<float, 2> first = {-1.0f, -1.0f};
    std::array<float, 1> later = {0.5f};
    std::array<std::uint8_t, 2048> packedFirst{};
    std::array<std::uint8_t, 2048> packedLater{};
    const std::uint32_t firstBytes = harken::monitor::PackAudioChunkRecord(
        packedFirst.data(), static_cast<std::uint32_t>(packedFirst.size()), 10, 7, 1,
        11, 3, 0, 48000, 1, 0, 0, 2, first.data(), 2);
    const std::uint32_t laterBytes = harken::monitor::PackAudioChunkRecord(
        packedLater.data(), static_cast<std::uint32_t>(packedLater.size()), 12, 7, 1,
        11, 3, 0, 48000, 1, 0, 8, 9, later.data(), 1);
    ASSERT_NE(0u, firstBytes);
    ASSERT_NE(0u, laterBytes);
    struct Collected {
        std::uint32_t gaps = 0;
        std::uint32_t triggers = 0;
    } collected;
    auto emit = [](void* raw, const harken::monitor::DerivedEmit& emitRec) {
        auto* typed = static_cast<Collected*>(raw);
        const std::uint8_t* p = emitRec.bytes + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES;
        if (p[68] != 0) {
            ++typed->gaps;
        }
        if (p[64] == 0) {
            ++typed->triggers;
        }
        return true;
    };
    harken::monitor::DispatchDerivedAudioChunk(&request, 1, &tap, 1, packedFirst.data(),
                                              firstBytes, emit, &collected);
    harken::monitor::DispatchDerivedAudioChunk(&request, 1, &tap, 1, packedLater.data(),
                                              laterBytes, emit, &collected);
    EXPECT_EQ(1u, collected.gaps);
    EXPECT_EQ(0u, collected.triggers);
}
