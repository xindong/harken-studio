#include <harken/Harken.h>

#include "MonitorCaptureHubTestHooks.h"
#include "MonitorDerivedDispatch.h"
#include "MonitorRecordPack.h"
#include "Common/TestUtil.h"
#include "monitor_fixture_decode.h"
#include "harken/core/MonitorSpectrumFft.h"
#include "harken/core/LogicAllocCheckpoint.h"

#include <gtest/gtest.h>

#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

namespace {

VnMonitorMeasurementRequest MakeSpectrum(std::uint32_t fftSize = 512) {
    VnMonitorMeasurementRequest request{};
    request.structSize = sizeof(request);
    request.kind = VN_MONITOR_MEASURE_SPECTRUM;
    request.channelMode = VN_MONITOR_CHANNEL_MONO_AVERAGE;
    request.downsample = 1;
    request.fftSize = fftSize;
    request.windowKind = 0;
    request.overlapNumerator = 1;
    request.overlapDenominator = 2;
    return request;
}

} // namespace

TEST(MonitorMeasure, SpectrumRejectsIllegalFftSize) {
    VnMonitorMeasurementRequest request = MakeSpectrum(256);
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED,
              harken::core::ValidateSpectrumRequest(request));
}

TEST(MonitorMeasure, SpectrumRejectsNonUnityDownsample) {
    VnMonitorMeasurementRequest request = MakeSpectrum();
    request.downsample = 2;
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED,
              harken::core::ValidateSpectrumRequest(request));
}

TEST(MonitorMeasure, SpectrumRejectsIllegalOverlap) {
    VnMonitorMeasurementRequest request = MakeSpectrum();
    request.overlapNumerator = 1;
    request.overlapDenominator = 4;
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED,
              harken::core::ValidateSpectrumRequest(request));
}

TEST(MonitorMeasure, SpectrumRejectsNonHannWindowKind) {
    VnMonitorMeasurementRequest request = MakeSpectrum();
    request.windowKind = 1;
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED,
              harken::core::ValidateSpectrumRequest(request));
}

TEST(MonitorMeasure, SpectrumAcceptsHannHalfOverlap) {
    EXPECT_EQ(VN_OK, harken::core::ValidateSpectrumRequest(MakeSpectrum()));
}

TEST(MonitorMeasure, SpectrumHannWindowSumsToHalf) {
    std::vector<float> samples(8, 1.0f);
    harken::core::HannWindow(samples.data(), static_cast<std::uint32_t>(samples.size()));
    double sum = 0.0;
    for (float sample : samples) {
        sum += sample;
    }
    EXPECT_NEAR(4.0, sum, 1.0e-5);
}

TEST(MonitorMeasure, SpectrumBinCenteredToneNearZeroDbfs) {
    constexpr std::uint32_t n = 512;
    std::vector<float> time(n, 0.0f);
    const double k = 8.0;
    for (std::uint32_t i = 0; i < n; ++i) {
        time[i] = static_cast<float>(
            std::sin(2.0 * 3.14159265358979323846 * k * static_cast<double>(i) /
                     static_cast<double>(n)));
    }
    std::vector<float> workTime(n, 0.0f);
    std::vector<float> workFft(n * 2, 0.0f);
    std::vector<float> bins(n / 2 + 1, 0.0f);
    harken::core::AnalyzeHannSpectrum(time.data(), n, workTime.data(), workFft.data(),
                                     bins.data());
    float peak = -1000.0f;
    std::uint32_t peakBin = 0;
    for (std::uint32_t i = 0; i < bins.size(); ++i) {
        if (bins[i] > peak) {
            peak = bins[i];
            peakBin = i;
        }
    }
    EXPECT_NEAR(8.0, static_cast<double>(peakBin), 1.0);
    EXPECT_NEAR(0.0, static_cast<double>(peak), 0.25);
}

TEST(MonitorMeasure, SpectrumWarmupDoesNotEmitZeroDbfs) {
    harken::core::MonitorSpectrumState state{};
    harken::core::ResetSpectrumState(&state, 512, 0);
    std::array<float, 4096> workTime{};
    std::array<float, 8192> workFft{};
    harken::core::MonitorSpectrumWindow window{};
    EXPECT_FALSE(harken::core::PushSpectrumSample(&state, 1.0f, 0, workTime.data(),
                                                 workFft.data(), &window));
    EXPECT_FALSE(window.complete);
}

TEST(MonitorMeasure, SpectrumPlanRejectsIllegalFftSize) {
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
    VnMonitorMeasurementRequest request = MakeSpectrum(256);
    VnMonitorCapturePlan plan{};
    plan.structSize = sizeof(plan);
    plan.streamKindMask = VN_MONITOR_STREAM_OPTIONAL_AUDIO;
    plan.tapCount = 1;
    plan.taps = &tap;
    plan.measurementCount = 1;
    plan.measurements = &request;
    VnMonitorEffectiveCapturePlan accepted{};
    accepted.structSize = sizeof(accepted);
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED,
              Vn_MonitorValidateCapturePlan(handle, lease.leaseGeneration, &plan,
                                            &accepted));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, SpectrumPlanAcceptsMeasurementRequest) {
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
    VnMonitorMeasurementRequest request = MakeSpectrum();
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
    for (std::size_t failAt = 1; failAt <= 3; ++failAt) {
        harken::detail::ArmLogicAllocFaultForTests(
            failAt, harken::detail::LogicAllocSite::MonitorDerivedStorage);
        const auto failed = Vn_MonitorApplyCapturePlan(
            handle, lease.leaseGeneration, &plan, &accepted);
        harken::detail::DisarmLogicAllocFaultForTests();
        EXPECT_EQ(VN_MONITOR_BUDGET_EXCEEDED, failed);
    }
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                                &accepted));
    EXPECT_EQ(1u, accepted.accepted.measurementCount);
    harken::monitor::StopDrainForTests();
    harken::monitor::AdvanceExecuteFrameForTests(1);
    const auto generation = accepted.capturePlanGeneration;
    harken::detail::ArmLogicAllocFaultForTests(
        1, harken::detail::LogicAllocSite::MonitorDerivedStorage);
    const auto failed = Vn_MonitorApplyCapturePlan(
        handle, lease.leaseGeneration, &plan, &accepted);
    harken::detail::DisarmLogicAllocFaultForTests();
    EXPECT_EQ(VN_MONITOR_BUDGET_EXCEEDED, failed);
    ASSERT_EQ(VN_OK, Vn_MonitorGetEffectiveCapturePlan(handle, 0, &accepted));
    EXPECT_EQ(generation, accepted.capturePlanGeneration);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_MONITOR_PLAN_ACTIVE), accepted.activationState);
    EXPECT_EQ(1u, accepted.accepted.measurementCount);
    ASSERT_EQ(VN_OK, Vn_MonitorApplyCapturePlan(handle, lease.leaseGeneration, &plan,
                                              &accepted));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, SpectrumDispatchPublishesPackedBins) {
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapDescriptorId = 11;
    tap.tapDescriptorGeneration = 3;
    VnMonitorMeasurementRequest request = MakeSpectrum(512);
    std::vector<float> samples(512, 0.0f);
    const double k = 8.0;
    for (std::uint32_t i = 0; i < 512; ++i) {
        samples[i] = static_cast<float>(
            std::sin(2.0 * 3.14159265358979323846 * k * static_cast<double>(i) /
                     512.0));
    }
    std::array<std::uint8_t, 16384> packed{};
    const std::uint32_t packedBytes = harken::monitor::PackAudioChunkRecord(
        packed.data(), static_cast<std::uint32_t>(packed.size()), 42, 7, 19, 11, 3, 0,
        48000, 1, 0, 0, 512, samples.data(), 512);
    ASSERT_NE(0u, packedBytes);

    struct Collected {
        std::vector<std::vector<std::uint8_t>> records;
    } collected;
    harken::monitor::DerivedDrainState derived;
    ASSERT_TRUE(derived.Prepare(&request, 1, 48000));
    derived.Dispatch(
        &request, 1, &tap, 1, packed.data(), packedBytes,
        [](void* raw, const harken::monitor::DerivedEmit& emit) {
            auto* typed = static_cast<Collected*>(raw);
            typed->records.emplace_back(emit.bytes, emit.bytes + emit.size);
            return true;
        },
        &collected);
    ASSERT_EQ(1u, collected.records.size());
    const auto& rec = collected.records[0];
    VnMonitorDecodedRecord decoded[1]{};
    std::uint8_t skip[8]{};
    const VnMonitorDecodeResult decodedResult = vn_monitor_decode_records(
        rec.data(), rec.size(), decoded, 1, skip, sizeof(skip));
    ASSERT_EQ(VN_OK, decodedResult.status);
    ASSERT_EQ(1u, decodedResult.recordsWritten);
    EXPECT_EQ(VN_MONITOR_RECORD_SPECTRUM, decoded[0].header.recordKind);
    EXPECT_EQ(42u, decoded[0].header.sourceSequence);
    EXPECT_EQ(7u, decoded[0].header.capturePlanGeneration);
    EXPECT_EQ(19u, decoded[0].header.executeFrame);
    EXPECT_EQ(48u, decoded[0].payload[0]);
    const auto readU32 = [](const std::uint8_t* p) {
        return static_cast<std::uint32_t>(p[0]) |
               (static_cast<std::uint32_t>(p[1]) << 8) |
               (static_cast<std::uint32_t>(p[2]) << 16) |
               (static_cast<std::uint32_t>(p[3]) << 24);
    };
    EXPECT_EQ(512u, readU32(decoded[0].payload + 4));
    EXPECT_EQ(257u, readU32(decoded[0].payload + 20));
    ASSERT_GE(decoded[0].payloadBytes, 48u + 257u * 4u);
    float peak = -1000.0f;
    std::uint32_t peakBin = 0;
    for (std::uint32_t i = 0; i < 257; ++i) {
        const std::uint32_t bits =
            static_cast<std::uint32_t>(decoded[0].payload[48 + i * 4]) |
            (static_cast<std::uint32_t>(decoded[0].payload[49 + i * 4]) << 8) |
            (static_cast<std::uint32_t>(decoded[0].payload[50 + i * 4]) << 16) |
            (static_cast<std::uint32_t>(decoded[0].payload[51 + i * 4]) << 24);
        float db = 0.0f;
        std::memcpy(&db, &bits, sizeof(db));
        if (db > peak) {
            peak = db;
            peakBin = i;
        }
    }
    EXPECT_NEAR(8.0, static_cast<double>(peakBin), 1.0);
    EXPECT_NEAR(0.0, static_cast<double>(peak), 0.25);
    EXPECT_NE(0u, decoded[0].header.validityFlags & VN_MONITOR_VALID_NUMERIC);
}

TEST(MonitorMeasure, SpectrumTapMismatchDoesNotPublish) {
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapDescriptorId = 11;
    tap.tapDescriptorGeneration = 3;
    VnMonitorMeasurementRequest request = MakeSpectrum(512);
    std::vector<float> samples(512, 0.0f);
    std::array<std::uint8_t, 16384> packed{};
    const std::uint32_t packedBytes = harken::monitor::PackAudioChunkRecord(
        packed.data(), static_cast<std::uint32_t>(packed.size()), 42, 7, 19, 11, 3, 0,
        48000, 1, 0, 0, 512, samples.data(), 512);
    ASSERT_NE(0u, packedBytes);
    VnMonitorTapDescriptor other = tap;
    other.tapDescriptorId = 99;
    struct Collected {
        std::uint32_t count = 0;
    } collected;
    harken::monitor::DerivedDrainState derived;
    ASSERT_TRUE(derived.Prepare(&request, 1, 48000));
    derived.Dispatch(
        &request, 1, &other, 1, packed.data(), packedBytes,
        [](void* raw, const harken::monitor::DerivedEmit&) {
            ++static_cast<Collected*>(raw)->count;
            return true;
        },
        &collected);
    EXPECT_EQ(0u, collected.count);
}

TEST(MonitorMeasure, SpectrumSourceGapClearsOverlap) {
    harken::core::MonitorSpectrumState state{};
    harken::core::ResetSpectrumState(&state, 512, 0);
    std::array<float, 4096> workTime{};
    std::array<float, 8192> workFft{};
    harken::core::MonitorSpectrumWindow window{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        EXPECT_FALSE(harken::core::PushSpectrumSample(
            &state, 1.0f, i, workTime.data(), workFft.data(), &window));
    }
    EXPECT_EQ(256u, state.filled);
    EXPECT_FALSE(harken::core::PushSpectrumSample(&state, 1.0f, 400, workTime.data(),
                                                 workFft.data(), &window));
    EXPECT_EQ(1u, state.filled);
    EXPECT_EQ(400u, state.bufferStartFrame);
}

TEST(MonitorMeasure, SpectrumNonFiniteClearsOverlap) {
    harken::core::MonitorSpectrumState state{};
    harken::core::ResetSpectrumState(&state, 512, 0);
    std::array<float, 4096> workTime{};
    std::array<float, 8192> workFft{};
    harken::core::MonitorSpectrumWindow window{};
    for (std::uint32_t i = 0; i < 256; ++i) {
        EXPECT_FALSE(harken::core::PushSpectrumSample(
            &state, 1.0f, i, workTime.data(), workFft.data(), &window));
    }
    EXPECT_FALSE(harken::core::PushSpectrumSample(
        &state, std::numeric_limits<float>::quiet_NaN(), 256, workTime.data(),
        workFft.data(), &window));
    EXPECT_EQ(0u, state.filled);
    EXPECT_FALSE(window.complete);
}

TEST(MonitorMeasure, SpectrumPackedRecordStaysEightByteAligned) {
    EXPECT_EQ(0u, harken::monitor::PackedSpectrumBytes(257) % 8u);
    EXPECT_EQ(0u, harken::monitor::PackedSpectrumBytes(2049) % 8u);
    EXPECT_EQ(2049u, harken::core::SpectrumBinCount(4096));
}

TEST(MonitorMeasure, SpectrumPerChannelUsesIndexZero) {
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapDescriptorId = 11;
    tap.tapDescriptorGeneration = 3;
    VnMonitorMeasurementRequest request = MakeSpectrum(512);
    request.channelMode = VN_MONITOR_CHANNEL_PER_CHANNEL;
    std::vector<float> stereo(1024, 0.0f);
    const double k = 8.0;
    for (std::uint32_t i = 0; i < 512; ++i) {
        stereo[i * 2] = static_cast<float>(
            std::sin(2.0 * 3.14159265358979323846 * k * static_cast<double>(i) /
                     512.0));
        stereo[i * 2 + 1] = 0.0f;
    }
    std::array<std::uint8_t, 16384> packed{};
    const std::uint32_t packedBytes = harken::monitor::PackAudioChunkRecord(
        packed.data(), static_cast<std::uint32_t>(packed.size()), 42, 7, 19, 11, 3, 0,
        48000, 2, 0, 0, 512, stereo.data(), 512);
    ASSERT_NE(0u, packedBytes);
    struct Collected {
        std::uint32_t count = 0;
        std::uint32_t channelMode = 99;
    } collected;
    harken::monitor::DerivedDrainState derived;
    ASSERT_TRUE(derived.Prepare(&request, 1, 48000));
    derived.Dispatch(
        &request, 1, &tap, 1, packed.data(), packedBytes,
        [](void* raw, const harken::monitor::DerivedEmit& emit) {
            auto* typed = static_cast<Collected*>(raw);
            ++typed->count;
            const std::uint8_t* p =
                emit.bytes + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES + 44;
            typed->channelMode = static_cast<std::uint32_t>(p[0]) |
                                 (static_cast<std::uint32_t>(p[1]) << 8) |
                                 (static_cast<std::uint32_t>(p[2]) << 16) |
                                 (static_cast<std::uint32_t>(p[3]) << 24);
            return true;
        },
        &collected);
    EXPECT_EQ(1u, collected.count);
    EXPECT_EQ(VN_MONITOR_CHANNEL_PER_CHANNEL, collected.channelMode);
}

TEST(MonitorMeasure, SpectrumCompletesAcrossChunks) {
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapDescriptorId = 11;
    tap.tapDescriptorGeneration = 3;
    VnMonitorMeasurementRequest request = MakeSpectrum(512);
    std::vector<float> samples(512, 0.0f);
    const double k = 8.0;
    for (std::uint32_t i = 0; i < 512; ++i) {
        samples[i] = static_cast<float>(
            std::sin(2.0 * 3.14159265358979323846 * k * static_cast<double>(i) /
                     512.0));
    }
    std::array<std::uint8_t, 16384> first{};
    std::array<std::uint8_t, 16384> second{};
    const std::uint32_t firstBytes = harken::monitor::PackAudioChunkRecord(
        first.data(), static_cast<std::uint32_t>(first.size()), 10, 7, 1, 11, 3, 0,
        48000, 1, 0, 0, 256, samples.data(), 256);
    const std::uint32_t secondBytes = harken::monitor::PackAudioChunkRecord(
        second.data(), static_cast<std::uint32_t>(second.size()), 11, 7, 1, 11, 3, 0,
        48000, 1, 0, 256, 512, samples.data() + 256, 256);
    ASSERT_NE(0u, firstBytes);
    ASSERT_NE(0u, secondBytes);
    struct Collected {
        std::uint32_t count = 0;
    } collected;
    auto emit = [](void* raw, const harken::monitor::DerivedEmit&) {
        ++static_cast<Collected*>(raw)->count;
        return true;
    };
    harken::monitor::DerivedDrainState derived;
    ASSERT_TRUE(derived.Prepare(&request, 1, 48000));
    derived.Dispatch(&request, 1, &tap, 1, first.data(),
                                              firstBytes, emit, &collected);
    EXPECT_EQ(0u, collected.count);
    derived.Dispatch(&request, 1, &tap, 1, second.data(),
                                              secondBytes, emit, &collected);
    EXPECT_EQ(1u, collected.count);
}

TEST(MonitorMeasure, SpectrumStalePlanChunkDoesNotReuseOverlap) {
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapDescriptorId = 11;
    tap.tapDescriptorGeneration = 3;
    VnMonitorMeasurementRequest newRequest = MakeSpectrum(1024);
    std::vector<float> leftover(1024, 1.0f);
    std::vector<float> matching(1024, 0.0f);
    std::array<std::uint8_t, 32768> packedOld{};
    std::array<std::uint8_t, 32768> packedNew{};
    const std::uint32_t oldBytes = harken::monitor::PackAudioChunkRecord(
        packedOld.data(), static_cast<std::uint32_t>(packedOld.size()), 10, 7, 1, 11,
        3, 0, 48000, 1, 0, 0, 1024, leftover.data(), 1024);
    const std::uint32_t newBytes = harken::monitor::PackAudioChunkRecord(
        packedNew.data(), static_cast<std::uint32_t>(packedNew.size()), 11, 8, 2, 11,
        3, 0, 48000, 1, 0, 0, 1024, matching.data(), 1024);
    ASSERT_NE(0u, oldBytes);
    ASSERT_NE(0u, newBytes);
    struct Collected {
        std::uint32_t count = 0;
        std::uint32_t fftSize = 0;
        std::uint64_t planGeneration = 0;
    } collected;
    auto emit = [](void* raw, const harken::monitor::DerivedEmit& emitRec) {
        auto* typed = static_cast<Collected*>(raw);
        ++typed->count;
        const std::uint8_t* p = emitRec.bytes + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES;
        typed->fftSize = static_cast<std::uint32_t>(p[4]) |
                         (static_cast<std::uint32_t>(p[5]) << 8) |
                         (static_cast<std::uint32_t>(p[6]) << 16) |
                         (static_cast<std::uint32_t>(p[7]) << 24);
        typed->planGeneration =
            static_cast<std::uint64_t>(emitRec.bytes[80]) |
            (static_cast<std::uint64_t>(emitRec.bytes[81]) << 8) |
            (static_cast<std::uint64_t>(emitRec.bytes[82]) << 16) |
            (static_cast<std::uint64_t>(emitRec.bytes[83]) << 24) |
            (static_cast<std::uint64_t>(emitRec.bytes[84]) << 32) |
            (static_cast<std::uint64_t>(emitRec.bytes[85]) << 40) |
            (static_cast<std::uint64_t>(emitRec.bytes[86]) << 48) |
            (static_cast<std::uint64_t>(emitRec.bytes[87]) << 56);
        return true;
    };
    harken::monitor::DerivedDrainState derived;
    ASSERT_TRUE(derived.Prepare(&newRequest, 1, 48000));
    derived.Dispatch(&newRequest, 1, &tap, 1, packedOld.data(),
                                              oldBytes, emit, &collected, 8);
    EXPECT_EQ(0u, collected.count);
    derived.Dispatch(&newRequest, 1, &tap, 1, packedNew.data(),
                                              newBytes, emit, &collected, 8);
    EXPECT_EQ(1u, collected.count);
    EXPECT_EQ(1024u, collected.fftSize);
    EXPECT_EQ(8u, collected.planGeneration);
    derived.Dispatch(&newRequest, 1, &tap, 1, packedOld.data(),
                                              oldBytes, emit, &collected, 8);
    EXPECT_EQ(1u, collected.count);
    EXPECT_EQ(8u, collected.planGeneration);
}

TEST(MonitorMeasure, SpectrumSameShapeStalePlanChunkIsSkipped) {
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapDescriptorId = 11;
    tap.tapDescriptorGeneration = 3;
    VnMonitorMeasurementRequest request = MakeSpectrum(512);
    std::vector<float> matching(512, 0.0f);
    std::vector<float> leftover(512, 1.0f);
    std::array<std::uint8_t, 16384> packedNew{};
    std::array<std::uint8_t, 16384> packedOld{};
    const std::uint32_t newBytes = harken::monitor::PackAudioChunkRecord(
        packedNew.data(), static_cast<std::uint32_t>(packedNew.size()), 11, 8, 2, 11,
        3, 0, 48000, 1, 0, 0, 512, matching.data(), 512);
    const std::uint32_t oldBytes = harken::monitor::PackAudioChunkRecord(
        packedOld.data(), static_cast<std::uint32_t>(packedOld.size()), 10, 7, 1, 11,
        3, 0, 48000, 1, 0, 0, 512, leftover.data(), 512);
    ASSERT_NE(0u, newBytes);
    ASSERT_NE(0u, oldBytes);
    struct Collected {
        std::uint32_t count = 0;
        std::uint32_t fftSize = 0;
        std::uint64_t planGeneration = 0;
    } collected;
    auto emit = [](void* raw, const harken::monitor::DerivedEmit& emitRec) {
        auto* typed = static_cast<Collected*>(raw);
        ++typed->count;
        const std::uint8_t* p = emitRec.bytes + VN_MONITOR_RECORD_ENVELOPE_PACKED_BYTES;
        typed->fftSize = static_cast<std::uint32_t>(p[4]) |
                         (static_cast<std::uint32_t>(p[5]) << 8) |
                         (static_cast<std::uint32_t>(p[6]) << 16) |
                         (static_cast<std::uint32_t>(p[7]) << 24);
        typed->planGeneration =
            static_cast<std::uint64_t>(emitRec.bytes[80]) |
            (static_cast<std::uint64_t>(emitRec.bytes[81]) << 8) |
            (static_cast<std::uint64_t>(emitRec.bytes[82]) << 16) |
            (static_cast<std::uint64_t>(emitRec.bytes[83]) << 24) |
            (static_cast<std::uint64_t>(emitRec.bytes[84]) << 32) |
            (static_cast<std::uint64_t>(emitRec.bytes[85]) << 40) |
            (static_cast<std::uint64_t>(emitRec.bytes[86]) << 48) |
            (static_cast<std::uint64_t>(emitRec.bytes[87]) << 56);
        return true;
    };
    harken::monitor::DerivedDrainState derived;
    ASSERT_TRUE(derived.Prepare(&request, 1, 48000));
    derived.Dispatch(&request, 1, &tap, 1, packedOld.data(),
                                              oldBytes, emit, &collected, 8);
    EXPECT_EQ(0u, collected.count);
    derived.Dispatch(&request, 1, &tap, 1, packedNew.data(),
                                              newBytes, emit, &collected, 8);
    EXPECT_EQ(1u, collected.count);
    EXPECT_EQ(512u, collected.fftSize);
    EXPECT_EQ(8u, collected.planGeneration);
    derived.Dispatch(&request, 1, &tap, 1, packedOld.data(),
                                              oldBytes, emit, &collected, 8);
    EXPECT_EQ(1u, collected.count);
    EXPECT_EQ(8u, collected.planGeneration);
}
