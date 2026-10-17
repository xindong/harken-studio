#include <harken/Harken.h>

#include "MonitorCaptureHubTestHooks.h"
#include "MonitorRecordPack.h"
#include "Common/TestUtil.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>

namespace {

VnConfig MeasurementBudgetConfig() {
    auto config = harkentest::MakeNullConfig();
    // Measurement budgets do not require the default nested Action scratch.
    config.maxTriggerDepth = 2;
    return config;
}

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

VnMonitorSessionDesc MakeAudioControlDesc() {
    VnMonitorSessionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.requestedStreamKindMask =
        VN_MONITOR_STREAM_OPTIONAL_AUDIO | VN_MONITOR_STREAM_EVENT;
    desc.requestedPermissionMask = VN_MONITOR_PERM_OBSERVE_MEASUREMENTS |
                                   VN_MONITOR_PERM_CAPTURE_CONTROL |
                                   VN_MONITOR_PERM_RAW_AUDIO;
    desc.consumerQueueBytes = 65536;
    return desc;
}

VnResult OpenAudioControl(VnMonitorSessionHandle* handle) {
    VnMonitorSessionDesc desc = MakeAudioControlDesc();
    return Vn_MonitorOpenSession(&desc, handle);
}

} // namespace

TEST(MonitorMeasure, BudgetRejectsUserBudgetBelowPredictedCost) {
    harkentest::EngineSession engine(MeasurementBudgetConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenAudioControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    lease.structSize = sizeof(lease);
    ASSERT_EQ(VN_OK, Vn_MonitorAcquireCaptureControl(handle, &lease));
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
    plan.bytesPerSecondBudget = 1;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    EXPECT_EQ(VN_MONITOR_BUDGET_EXCEEDED,
              Vn_MonitorValidateCapturePlan(handle, lease.leaseGeneration, &plan,
                                            &accepted));
    EXPECT_EQ(VN_MONITOR_BUDGET_EXCEEDED,
              Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                         &accepted));
    VnMonitorEffectiveCapturePlan effective{};
    effective.structSize = sizeof(effective);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(handle, 0, &effective));
    EXPECT_EQ(1u, effective.capturePlanGeneration);
    EXPECT_EQ(0u, effective.accepted.measurementCount);
    EXPECT_EQ(0u, effective.accepted.streamKindMask);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, BudgetRejectsTwoTapsBelowScaledDerivedCost) {
    harkentest::EngineSession engine(MeasurementBudgetConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenAudioControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    lease.structSize = sizeof(lease);
    ASSERT_EQ(VN_OK, Vn_MonitorAcquireCaptureControl(handle, &lease));
    std::array<VnMonitorTapDescriptor, 2> taps = {MakeMasterTap(), MakeMasterTap()};
    taps[1].tapKind = VN_MONITOR_TAP_BUS_POST_FADER;
    taps[1].selectorMode = VN_MONITOR_TAP_SELECTOR_FOLLOW_SLOT;
    VnMonitorMeasurementRequest request = MakePeak();
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 2;
    plan.taps = taps.data();
    plan.measurementCount = 1;
    plan.measurements = &request;
    plan.downsample = 1;
    plan.bytesPerSecondBudget = 1;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    EXPECT_EQ(VN_MONITOR_BUDGET_EXCEEDED,
              Vn_MonitorValidateCapturePlan(handle, lease.leaseGeneration, &plan,
                                            &accepted));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, BudgetAccepts512SpectrumAtHopScaledRate) {
    harkentest::EngineSession engine(MeasurementBudgetConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenAudioControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    lease.structSize = sizeof(lease);
    ASSERT_EQ(VN_OK, Vn_MonitorAcquireCaptureControl(handle, &lease));
    VnMonitorTapDescriptor tap = MakeMasterTap();
    VnMonitorMeasurementRequest small = {};
    small.structSize = sizeof(small);
    small.kind = VN_MONITOR_MEASURE_SPECTRUM;
    small.channelMode = VN_MONITOR_CHANNEL_MONO_AVERAGE;
    small.downsample = 1;
    small.fftSize = 512;
    small.windowKind = 0;
    small.overlapNumerator = 1;
    small.overlapDenominator = 2;
    constexpr std::uint64_t kHz = 100ull;
    const std::uint64_t audio =
        static_cast<std::uint64_t>(harken::monitor::PackedAudioChunkBytes(
            VN_MONITOR_DEFAULT_AUDIO_CHUNK_FRAMES,
            harken::monitor::kPackedAudioMaxChannels)) *
        kHz;
    const std::uint64_t windows512 =
        (static_cast<std::uint64_t>(VN_MONITOR_DEFAULT_AUDIO_CHUNK_FRAMES) + 255ull) /
        256ull;
    const std::uint64_t fit512 =
        audio + static_cast<std::uint64_t>(harken::monitor::PackedSpectrumBytes(257)) *
                    windows512 * kHz;
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.taps = &tap;
    plan.measurementCount = 1;
    plan.measurements = &small;
    plan.downsample = 1;
    plan.bytesPerSecondBudget = fit512;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    EXPECT_EQ(VN_OK, Vn_MonitorValidateCapturePlan(handle, lease.leaseGeneration,
                                                   &plan, &accepted));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, BudgetRejectsOverlappedSpectrumWhenBudgetFitsOneWindow) {
    harkentest::EngineSession engine(MeasurementBudgetConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenAudioControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    lease.structSize = sizeof(lease);
    ASSERT_EQ(VN_OK, Vn_MonitorAcquireCaptureControl(handle, &lease));
    VnMonitorTapDescriptor tap = MakeMasterTap();
    VnMonitorMeasurementRequest request = {};
    request.structSize = sizeof(request);
    request.kind = VN_MONITOR_MEASURE_SPECTRUM;
    request.channelMode = VN_MONITOR_CHANNEL_MONO_AVERAGE;
    request.downsample = 1;
    request.fftSize = 512;
    request.windowKind = 0;
    request.overlapNumerator = 1;
    request.overlapDenominator = 2;
    constexpr std::uint64_t kHz = 100ull;
    const std::uint64_t audio =
        static_cast<std::uint64_t>(harken::monitor::PackedAudioChunkBytes(
            VN_MONITOR_DEFAULT_AUDIO_CHUNK_FRAMES,
            harken::monitor::kPackedAudioMaxChannels)) *
        kHz;
    const std::uint64_t fitOneWindow =
        audio + static_cast<std::uint64_t>(harken::monitor::PackedSpectrumBytes(257)) *
                    kHz;
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.taps = &tap;
    plan.measurementCount = 1;
    plan.measurements = &request;
    plan.downsample = 1;
    plan.bytesPerSecondBudget = fitOneWindow;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    EXPECT_EQ(VN_MONITOR_BUDGET_EXCEEDED,
              Vn_MonitorValidateCapturePlan(handle, lease.leaseGeneration, &plan,
                                            &accepted));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, BudgetRejectsOneFrameWaveformWhenBudgetFitsCoarseBuckets) {
    harkentest::EngineSession engine(MeasurementBudgetConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenAudioControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    lease.structSize = sizeof(lease);
    ASSERT_EQ(VN_OK, Vn_MonitorAcquireCaptureControl(handle, &lease));
    VnMonitorTapDescriptor tap = MakeMasterTap();
    VnMonitorMeasurementRequest coarse = {};
    coarse.structSize = sizeof(coarse);
    coarse.kind = VN_MONITOR_MEASURE_WAVEFORM;
    coarse.channelMode = VN_MONITOR_CHANNEL_PER_CHANNEL;
    coarse.downsample = 1;
    coarse.bucketFrames = 64;
    constexpr std::uint64_t kHz = 100ull;
    const std::uint64_t audio =
        static_cast<std::uint64_t>(harken::monitor::PackedAudioChunkBytes(
            VN_MONITOR_DEFAULT_AUDIO_CHUNK_FRAMES,
            harken::monitor::kPackedAudioMaxChannels)) *
        kHz;
    const std::uint64_t buckets =
        (static_cast<std::uint64_t>(VN_MONITOR_DEFAULT_AUDIO_CHUNK_FRAMES) + 63ull) /
        64ull;
    const std::uint64_t fitCoarse =
        audio + static_cast<std::uint64_t>(harken::monitor::PackedWaveformBucketBytes()) *
                    buckets * kHz;
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.taps = &tap;
    plan.measurementCount = 1;
    plan.measurements = &coarse;
    plan.downsample = 1;
    plan.bytesPerSecondBudget = fitCoarse;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    EXPECT_EQ(VN_OK, Vn_MonitorValidateCapturePlan(handle, lease.leaseGeneration,
                                                   &plan, &accepted));
    VnMonitorMeasurementRequest fine = coarse;
    fine.bucketFrames = 1;
    plan.measurements = &fine;
    EXPECT_EQ(VN_MONITOR_BUDGET_EXCEEDED,
              Vn_MonitorValidateCapturePlan(handle, lease.leaseGeneration, &plan,
                                            &accepted));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, BudgetAccepts4096SpectrumWhenBudgetFitsHopScaledRate) {
    harkentest::EngineSession engine(MeasurementBudgetConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenAudioControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    lease.structSize = sizeof(lease);
    ASSERT_EQ(VN_OK, Vn_MonitorAcquireCaptureControl(handle, &lease));
    VnMonitorTapDescriptor tap = MakeMasterTap();
    VnMonitorMeasurementRequest request = {};
    request.structSize = sizeof(request);
    request.kind = VN_MONITOR_MEASURE_SPECTRUM;
    request.channelMode = VN_MONITOR_CHANNEL_MONO_AVERAGE;
    request.downsample = 1;
    request.fftSize = 4096;
    request.windowKind = 0;
    request.overlapNumerator = 1;
    request.overlapDenominator = 2;
    constexpr std::uint64_t kHz = 100ull;
    const std::uint64_t audio =
        static_cast<std::uint64_t>(harken::monitor::PackedAudioChunkBytes(
            VN_MONITOR_DEFAULT_AUDIO_CHUNK_FRAMES,
            harken::monitor::kPackedAudioMaxChannels)) *
        kHz;
    const std::uint64_t hopScaled =
        audio + static_cast<std::uint64_t>(harken::monitor::PackedSpectrumBytes(2049)) *
                    VN_MONITOR_DEFAULT_AUDIO_CHUNK_FRAMES / 2048ull * kHz;
    const std::uint64_t ceilQuantum =
        audio + static_cast<std::uint64_t>(harken::monitor::PackedSpectrumBytes(2049)) *
                    kHz;
    ASSERT_LT(hopScaled, ceilQuantum);
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.taps = &tap;
    plan.measurementCount = 1;
    plan.measurements = &request;
    plan.downsample = 1;
    plan.bytesPerSecondBudget = hopScaled;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    EXPECT_EQ(VN_OK, Vn_MonitorValidateCapturePlan(handle, lease.leaseGeneration,
                                                   &plan, &accepted));
    plan.bytesPerSecondBudget = hopScaled - 1ull;
    EXPECT_EQ(VN_MONITOR_BUDGET_EXCEEDED,
              Vn_MonitorValidateCapturePlan(handle, lease.leaseGeneration, &plan,
                                            &accepted));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, BudgetAcceptsWideWaveformBucketsWhenBudgetFitsFrameRate) {
    harkentest::EngineSession engine(MeasurementBudgetConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenAudioControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    lease.structSize = sizeof(lease);
    ASSERT_EQ(VN_OK, Vn_MonitorAcquireCaptureControl(handle, &lease));
    VnMonitorTapDescriptor tap = MakeMasterTap();
    VnMonitorMeasurementRequest request = {};
    request.structSize = sizeof(request);
    request.kind = VN_MONITOR_MEASURE_WAVEFORM;
    request.channelMode = VN_MONITOR_CHANNEL_PER_CHANNEL;
    request.downsample = 1;
    request.bucketFrames = 4096;
    constexpr std::uint64_t kHz = 100ull;
    const std::uint64_t audio =
        static_cast<std::uint64_t>(harken::monitor::PackedAudioChunkBytes(
            VN_MONITOR_DEFAULT_AUDIO_CHUNK_FRAMES,
            harken::monitor::kPackedAudioMaxChannels)) *
        kHz;
    const std::uint64_t frameRate =
        audio + static_cast<std::uint64_t>(harken::monitor::PackedWaveformBucketBytes()) *
                    VN_MONITOR_DEFAULT_AUDIO_CHUNK_FRAMES / 4096ull * kHz;
    const std::uint64_t ceilQuantum =
        audio + static_cast<std::uint64_t>(harken::monitor::PackedWaveformBucketBytes()) *
                    kHz;
    ASSERT_LT(frameRate, ceilQuantum);
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.taps = &tap;
    plan.measurementCount = 1;
    plan.measurements = &request;
    plan.downsample = 1;
    plan.bytesPerSecondBudget = frameRate;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    EXPECT_EQ(VN_OK, Vn_MonitorValidateCapturePlan(handle, lease.leaseGeneration,
                                                   &plan, &accepted));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, BudgetZeroUsesPredictedAndDoesNotBypassHardCeiling) {
    harkentest::EngineSession engine(MeasurementBudgetConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenAudioControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    lease.structSize = sizeof(lease);
    ASSERT_EQ(VN_OK, Vn_MonitorAcquireCaptureControl(handle, &lease));
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
    plan.bytesPerSecondBudget = 0;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    EXPECT_EQ(VN_OK, Vn_MonitorValidateCapturePlan(handle, lease.leaseGeneration,
                                                   &plan, &accepted));
    EXPECT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                &accepted));
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_PLAN_PENDING),
              accepted.activationState);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, BudgetTimeSeriesOnly1024IsNotDerivedAudio) {
    harkentest::EngineSession engine(MeasurementBudgetConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_TIME_SERIES;
    desc.requestedPermissionMask =
        VN_MONITOR_PERM_OBSERVE_MEASUREMENTS | VN_MONITOR_PERM_CAPTURE_CONTROL;
    desc.consumerQueueBytes = 8192;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    lease.structSize = sizeof(lease);
    ASSERT_EQ(VN_OK, Vn_MonitorAcquireCaptureControl(handle, &lease));
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_TIME_SERIES;
    plan.cadenceHz = 30;
    plan.bytesPerSecondBudget = 1024;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    EXPECT_EQ(VN_OK, Vn_MonitorValidateCapturePlan(handle, lease.leaseGeneration,
                                                   &plan, &accepted));
    EXPECT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                &accepted));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, BudgetPartialKindFailureRollsBackWholePlan) {
    harkentest::EngineSession engine(MeasurementBudgetConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenAudioControl(&handle));
    VnMonitorCaptureControlStatus lease{};
    lease.structSize = sizeof(lease);
    ASSERT_EQ(VN_OK, Vn_MonitorAcquireCaptureControl(handle, &lease));
    VnMonitorTapDescriptor tap = MakeMasterTap();
    VnMonitorMeasurementRequest peak = MakePeak();
    VnMonitorCapturePlan first{};
    first.structSize = sizeof(first);
    first.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    first.tapCount = 1;
    first.taps = &tap;
    first.measurementCount = 1;
    first.measurements = &peak;
    first.downsample = 1;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &first,
                                                &accepted));
    harken::monitor::AdvanceExecuteFrameForTests(1);
    VnMonitorEffectiveCapturePlan live{};
    live.structSize = sizeof(live);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(handle, 0, &live));
    EXPECT_EQ(2u, live.capturePlanGeneration);
    EXPECT_EQ(1u, live.accepted.measurementCount);

    std::array<VnMonitorMeasurementRequest, 2> requests = {MakePeak(), MakePeak()};
    requests[1].kind = VN_MONITOR_MEASURE_LUFS_INTEGRATED;
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.taps = &tap;
    plan.measurementCount = 2;
    plan.measurements = requests.data();
    plan.downsample = 1;
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED,
              Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                         &accepted));
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(handle, 0, &live));
    EXPECT_EQ(2u, live.capturePlanGeneration);
    EXPECT_EQ(1u, live.accepted.measurementCount);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_PLAN_ACTIVE),
              live.activationState);
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, BudgetHardCeilingStillRejects) {
    harkentest::EngineSession engine(MeasurementBudgetConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    VnMonitorSessionHandle handle = 0;
    VnMonitorSessionDesc desc{};
    desc.structSize = sizeof(desc);
    desc.requestedStreamKindMask = VN_MONITOR_STREAM_TIME_SERIES;
    desc.requestedPermissionMask =
        VN_MONITOR_PERM_OBSERVE_MEASUREMENTS | VN_MONITOR_PERM_CAPTURE_CONTROL;
    desc.consumerQueueBytes = 8192;
    ASSERT_EQ(VN_OK, Vn_MonitorOpenSession(&desc, &handle));
    VnMonitorCaptureControlStatus lease{};
    lease.structSize = sizeof(lease);
    ASSERT_EQ(VN_OK, Vn_MonitorAcquireCaptureControl(handle, &lease));
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_TIME_SERIES;
    plan.cadenceHz = 30;
    plan.bytesPerSecondBudget = VN_MONITOR_HARD_CAPTURE_BYTE_RATE + 1ull;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    EXPECT_EQ(VN_MONITOR_BUDGET_EXCEEDED,
              Vn_MonitorValidateCapturePlan(handle, lease.leaseGeneration, &plan,
                                            &accepted));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, FixedRingCapacityIsStableAcrossSessionReopen) {
    harkentest::EngineSession engine(MeasurementBudgetConfig());
    ASSERT_EQ(VN_OK, engine.InitResult());
    EXPECT_EQ(0u, harken::monitor::ProducerRingCapacityForTests());
    VnMonitorSessionHandle handle = 0;
    ASSERT_EQ(VN_OK, OpenAudioControl(&handle));
    const std::uint32_t ring = harken::monitor::ProducerRingCapacityForTests();
    EXPECT_GT(ring, 0u);
    RecordProperty("ring_capacity", static_cast<int>(ring));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
    EXPECT_EQ(ring, harken::monitor::ProducerRingCapacityForTests());
    ASSERT_EQ(VN_OK, OpenAudioControl(&handle));
    EXPECT_EQ(ring, harken::monitor::ProducerRingCapacityForTests());
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, BudgetCapabilityMatchesImplementedLoudnessWindows) {
    struct RateCase {
        std::uint32_t sampleRate;
        std::uint32_t loudnessSupported;
    };
    for (const RateCase rate : {RateCase{1000, 0}, RateCase{3363, 0},
                               RateCase{3364, 1}, RateCase{44100, 1},
                               RateCase{48000, 1}, RateCase{384000, 1}}) {
        SCOPED_TRACE(rate.sampleRate);
        auto config = MeasurementBudgetConfig();
        config.sampleRate = rate.sampleRate;
        harkentest::EngineSession engine(config);
        ASSERT_EQ(VN_OK, engine.InitResult());
        VnMonitorCapabilities caps{};
        caps.structSize = sizeof(caps);
        ASSERT_EQ(VN_OK, Vn_MonitorGetCapabilities(&caps));
        EXPECT_EQ(rate.loudnessSupported, caps.lufsMomentarySupported);
        EXPECT_EQ(0u, caps.lufsShortTermSupported);
        EXPECT_EQ(0u, caps.lufsIntegratedSupported);
        VnMonitorSessionHandle handle = 0;
        ASSERT_EQ(VN_OK, OpenAudioControl(&handle));
        VnMonitorCaptureControlStatus lease{};
        lease.structSize = sizeof(lease);
        ASSERT_EQ(VN_OK, Vn_MonitorAcquireCaptureControl(handle, &lease));
        VnMonitorTapDescriptor tap = MakeMasterTap();
        const std::array kinds = {VN_MONITOR_MEASURE_SAMPLE_PEAK,
                                 VN_MONITOR_MEASURE_RMS,
                                 VN_MONITOR_MEASURE_LUFS_MOMENTARY,
                                 VN_MONITOR_MEASURE_LUFS_SHORT_TERM,
                                 VN_MONITOR_MEASURE_LUFS_INTEGRATED};
        const std::array supported = {1u, 1u, caps.lufsMomentarySupported,
                                     caps.lufsShortTermSupported, caps.lufsIntegratedSupported};
        for (std::size_t i = 0; i < kinds.size(); ++i) {
            SCOPED_TRACE(kinds[i]);
            VnMonitorMeasurementRequest request = MakePeak();
            request.kind = kinds[i];
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
            EXPECT_EQ(supported[i] ? VN_OK : VN_MONITOR_UNSUPPORTED,
                      Vn_MonitorValidateCapturePlan(handle, lease.leaseGeneration, &plan,
                                                   &accepted));
            if (!supported[i]) {
                EXPECT_EQ(VN_MONITOR_UNSUPPORTED,
                          Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                    &accepted));
            }
        }
        EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
    }
}
