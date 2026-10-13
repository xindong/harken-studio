#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "MonitorCaptureHubTestHooks.h"
#include "MonitorDerivedDispatch.h"
#include "MonitorMeterHandlerTestHooks.h"
#include "MonitorRecordPack.h"
#include "Common/TestUtil.h"
#include "monitor_fixture_decode.h"
#include "harken/core/AllocGuard.h"
#include "harken/core/MonitorMeterReduce.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace {

VnMonitorMeasurementRequest MakeMeter(std::uint32_t kind) {
    VnMonitorMeasurementRequest request{};
    request.structSize = sizeof(request);
    request.kind = kind;
    request.channelMode = VN_MONITOR_CHANNEL_PER_CHANNEL;
    request.downsample = 1;
    return request;
}

bool TestGainLookup(const VnMonitorTapDescriptor& tap, float* reductionDb,
                    VnObjectId* ownerStableId,
                    std::uint64_t* ownerInstanceHandle,
                    std::uint64_t* ownerGeneration, void*) {
    if (reductionDb == nullptr || ownerStableId == nullptr ||
        ownerInstanceHandle == nullptr || ownerGeneration == nullptr) {
        return false;
    }
    *reductionDb = 6.0f;
    *ownerStableId = tap.ownerStableId;
    *ownerInstanceHandle = tap.ownerInstanceHandle;
    *ownerGeneration = tap.ownerGeneration;
    return true;
}

} // namespace

TEST(MonitorMeasure, MeterRejectsNonUnityDownsample) {
    VnMonitorMeasurementRequest request =
        MakeMeter(VN_MONITOR_MEASURE_SAMPLE_PEAK);
    request.downsample = 2;
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED, harken::core::ValidateMeterRequest(request));
}

TEST(MonitorMeasure, MeterRejectsIllegalFftSize) {
    VnMonitorMeasurementRequest request =
        MakeMeter(VN_MONITOR_MEASURE_SAMPLE_PEAK);
    request.fftSize = 512;
    EXPECT_EQ(VN_MONITOR_UNSUPPORTED, harken::core::ValidateMeterRequest(request));
}

TEST(MonitorMeasure, MeterTreatsZeroDownsampleAsUnity) {
    VnMonitorMeasurementRequest request =
        MakeMeter(VN_MONITOR_MEASURE_SAMPLE_PEAK);
    request.downsample = 0;
    EXPECT_EQ(VN_OK, harken::core::ValidateMeterRequest(request));
}

TEST(MonitorMeasure, MeterArmedSilenceIsNumericZero) {
    harken::core::MonitorMeterState state{};
    harken::core::ResetMeterState(&state, 48000, 0);
    const float sample = 0.0f;
    const harken::core::MonitorMeterChunkResult item =
        harken::core::ReduceMeterChunk(&state, &sample, 1, 1,
                                      VN_MONITOR_CHANNEL_PER_CHANNEL);
    EXPECT_TRUE(item.numeric);
    EXPECT_FLOAT_EQ(0.0f, item.samplePeak);
}

TEST(MonitorMeasure, MeterNanIsArmedInvalidNotZero) {
    harken::core::MonitorMeterState state{};
    harken::core::ResetMeterState(&state, 48000, 0);
    const float sample = std::numeric_limits<float>::quiet_NaN();
    const harken::core::MonitorMeterChunkResult item =
        harken::core::ReduceMeterChunk(&state, &sample, 1, 1,
                                      VN_MONITOR_CHANNEL_PER_CHANNEL);
    EXPECT_TRUE(item.invalid);
    EXPECT_FALSE(item.numeric);
}

TEST(MonitorMeasure, MeterSamplePeakTracksAbs) {
    harken::core::MonitorMeterState state{};
    harken::core::ResetMeterState(&state, 48000, 0);
    const float samples[2] = {-0.5f, 0.25f};
    const harken::core::MonitorMeterChunkResult item =
        harken::core::ReduceMeterChunk(&state, samples, 2, 1,
                                      VN_MONITOR_CHANNEL_PER_CHANNEL);
    EXPECT_FLOAT_EQ(0.5f, item.samplePeak);
}

TEST(MonitorMeasure, MeterRmsWarmupUntil300msWindowFills) {
    for (const auto sampleRate : {1000u, 44100u, 48000u}) {
        SCOPED_TRACE(sampleRate);
        harken::core::MonitorMeterState state{};
        harken::core::ResetMeterState(&state, sampleRate, 0);
        const auto windowFrames = sampleRate * 300 / 1000;
        std::vector<float> samples(windowFrames, 1.0f);
        const auto first = harken::core::ReduceMeterChunk(
            &state, samples.data(), windowFrames - 1, 1, VN_MONITOR_CHANNEL_PER_CHANNEL);
        EXPECT_TRUE(first.rmsWarmup);
        EXPECT_FLOAT_EQ(0.0f, first.rmsLinear);
        const auto second = harken::core::ReduceMeterChunk(
            &state, samples.data() + windowFrames - 1, 1, 1, VN_MONITOR_CHANNEL_PER_CHANNEL);
        EXPECT_FALSE(second.rmsWarmup);
        EXPECT_NEAR(1.0f, second.rmsLinear, 1.0e-5f);
    }
}

TEST(MonitorMeasure, MeterRmsAveragesChannelEnergyWithoutPhaseCancellation) {
    std::vector<float> samples(14400 * 2);
    for (std::size_t frame = 0; frame < samples.size() / 2; ++frame) {
        samples[frame * 2] = 0.5f;
        samples[frame * 2 + 1] = -1.0f;
    }
    for (const auto mode : {VN_MONITOR_CHANNEL_PER_CHANNEL, VN_MONITOR_CHANNEL_MONO_AVERAGE}) {
        harken::core::MonitorMeterState state{};
        harken::core::ResetMeterState(&state, 48000, 0);
        const auto measured = harken::core::ReduceMeterChunk(
            &state, samples.data(), 14400, 2, mode);
        ASSERT_FALSE(measured.rmsWarmup);
        EXPECT_NEAR(mode == VN_MONITOR_CHANNEL_PER_CHANNEL ? std::sqrt(0.625f) : 0.25f,
                    measured.rmsLinear, 1.0e-6f);
    }
}

TEST(MonitorMeasure, MomentaryLoudnessMatchesKWeightedReferenceTones) {
    for (const auto sampleRate : {44100u, 48000u, 96000u}) {
        for (const auto frequency : {50.0, 1000.0, 10000.0}) {
            if (frequency != 1000.0 && sampleRate != 48000) {
                continue;
            }
            SCOPED_TRACE(sampleRate);
            SCOPED_TRACE(frequency);
            std::vector<float> samples(sampleRate * 2);
            for (std::size_t frame = 0; frame < samples.size(); ++frame) {
                samples[frame] = static_cast<float>(0.1 * std::sin(
                    2.0 * std::acos(-1.0) * frequency * frame / sampleRate));
            }
            harken::core::MonitorLoudnessState state{};
            harken::core::ResetLoudnessState(&state, sampleRate, VN_CHANNEL_LAYOUT_MONO, 0);
            const auto measured = harken::core::ReduceLoudnessChunk(
                &state, samples.data(), static_cast<std::uint32_t>(samples.size()), 1);
            ASSERT_TRUE(measured.numeric);
            ASSERT_FALSE(measured.loudnessWarmup);
            const double expected = frequency == 50.0 ? -27.635354
                : frequency == 1000.0 ? -23.003596 : -19.659418;
            EXPECT_NEAR(expected, measured.loudnessMomentary, 0.03);
        }
    }
}

TEST(MonitorMeasure, MomentaryLoudnessPreservesChannelEnergyAndSpeakerWeights) {
    const auto measure = [](std::uint32_t layout, std::uint32_t channels,
                            std::uint32_t activeChannel, bool oppositeStereo) {
        std::vector<float> samples(48000 * channels);
        for (std::size_t frame = 0; frame < 48000; ++frame) {
            const float tone = static_cast<float>(0.1 * std::sin(2.0 * std::acos(-1.0) * frame / 48.0));
            samples[frame * channels + activeChannel] = tone;
            if (oppositeStereo) {
                samples[frame * channels + 1] = -tone;
            }
        }
        harken::core::MonitorLoudnessState state{};
        harken::core::ResetLoudnessState(&state, 48000, layout, 0);
        return harken::core::ReduceLoudnessChunk(&state, samples.data(), 48000, channels).loudnessMomentary;
    };
    const float mono = measure(VN_CHANNEL_LAYOUT_MONO, 1, 0, false);
    EXPECT_NEAR(mono + 10.0 * std::log10(2.0),
                measure(VN_CHANNEL_LAYOUT_STEREO, 2, 0, true), 1.0e-4);
    for (const auto [layout, channels] : {std::pair{VN_CHANNEL_LAYOUT_QUAD, 4u},
                                         std::pair{VN_CHANNEL_LAYOUT_5_1, 6u},
                                         std::pair{VN_CHANNEL_LAYOUT_7_1, 8u}}) {
        EXPECT_NEAR(mono, measure(layout, channels, 0, false), 1.0e-4);
        EXPECT_NEAR(mono + 10.0 * std::log10(1.41),
                    measure(layout, channels, channels - 1, false), 1.0e-4);
        if (channels >= 6) {
            EXPECT_EQ(-std::numeric_limits<float>::infinity(), measure(layout, channels, 3, false));
        }
    }
}

TEST(MonitorMeasure, MomentaryLoudnessIsBlockIndependentAndResetsItsWindow) {
    std::vector<float> samples(24000);
    for (std::size_t frame = 0; frame < samples.size(); ++frame) {
        samples[frame] = static_cast<float>((frame < 12000 ? 0.2 : 0.05)
            * std::sin(2.0 * std::acos(-1.0) * frame / 48.0));
    }
    harken::core::MonitorLoudnessState reference{};
    harken::core::ResetLoudnessState(&reference, 48000, VN_CHANNEL_LAYOUT_MONO, 1);
    const auto expected = harken::core::ReduceLoudnessChunk(&reference, samples.data(), 24000, 1);
    for (const auto blockFrames : {17u, 333u, 4096u}) {
        harken::core::MonitorLoudnessState state{};
        harken::core::ResetLoudnessState(&state, 48000, VN_CHANNEL_LAYOUT_MONO, 1);
        harken::core::MonitorMeterChunkResult actual{};
        for (std::uint32_t offset = 0; offset < 24000; offset += blockFrames) {
            const auto count = std::min(blockFrames, 24000u - offset);
            actual = harken::core::ReduceLoudnessChunk(&state, samples.data() + offset, count, 1);
            EXPECT_EQ(offset + count < 19200, actual.loudnessWarmup);
        }
        EXPECT_FLOAT_EQ(expected.loudnessMomentary, actual.loudnessMomentary);
        harken::core::ResetLoudnessState(&state, 48000, VN_CHANNEL_LAYOUT_MONO, 2);
        std::vector<float> silence(19200);
        EXPECT_TRUE(harken::core::ReduceLoudnessChunk(&state, silence.data(), 19199, 1).loudnessWarmup);
        actual = harken::core::ReduceLoudnessChunk(&state, silence.data(), 1, 1);
        EXPECT_FALSE(actual.loudnessWarmup);
        EXPECT_EQ(-std::numeric_limits<float>::infinity(), actual.loudnessMomentary);
    }
}

TEST(MonitorMeasure, MeterHoldDecaysAfterOneSecond) {
    harken::core::MonitorMeterState state{};
    harken::core::ResetMeterState(&state, 100, 0);
    const float peak = 1.0f;
    (void)harken::core::ReduceMeterChunk(&state, &peak, 1, 1,
                                        VN_MONITOR_CHANNEL_PER_CHANNEL);
    std::vector<float> silence(100, 0.0f);
    const harken::core::MonitorMeterChunkResult held =
        harken::core::ReduceMeterChunk(&state, silence.data(), 99, 1,
                                      VN_MONITOR_CHANNEL_PER_CHANNEL);
    EXPECT_NEAR(1.0f, held.holdLinear, 1.0e-5f);
    const harken::core::MonitorMeterChunkResult stillHeld =
        harken::core::ReduceMeterChunk(&state, silence.data(), 1, 1,
                                      VN_MONITOR_CHANNEL_PER_CHANNEL);
    EXPECT_NEAR(1.0f, stillHeld.holdLinear, 1.0e-5f);
    const harken::core::MonitorMeterChunkResult decayed =
        harken::core::ReduceMeterChunk(&state, silence.data(), 20, 1,
                                      VN_MONITOR_CHANNEL_PER_CHANNEL);
    EXPECT_LT(decayed.holdLinear, 1.0f);
    EXPECT_GT(decayed.holdLinear, 0.0f);
    EXPECT_NEAR(std::pow(10.0f, -20.0f / 100.0f), decayed.holdLinear, 1.0e-3f);
}

TEST(MonitorMeasure, MeterTruePeakWarmupThenNumeric) {
    harken::core::MonitorMeterState state{};
    harken::core::ResetMeterState(&state, 48000, 0);
    std::vector<float> impulse(16, 0.0f);
    impulse[8] = 1.0f;
    const harken::core::MonitorMeterChunkResult item =
        harken::core::ReduceMeterChunk(&state, impulse.data(), 16, 1,
                                      VN_MONITOR_CHANNEL_PER_CHANNEL);
    EXPECT_TRUE(item.numeric);
    EXPECT_FALSE(item.truePeakWarmup);
    EXPECT_TRUE(item.rmsWarmup);
    EXPECT_GT(item.truePeak, 0.9f);
    EXPECT_NEAR(item.truePeak, 0.972991149f, 0.05f);
}

TEST(MonitorMeasure, MeterSegmentClearsHold) {
    harken::core::MonitorMeterState state{};
    harken::core::ResetMeterState(&state, 48000, 0);
    const float peak = 0.5f;
    (void)harken::core::ReduceMeterChunk(&state, &peak, 1, 1,
                                        VN_MONITOR_CHANNEL_PER_CHANNEL);
    harken::core::NoteMeterSegment(&state, 2);
    EXPECT_FALSE(state.holdReady);
    EXPECT_FLOAT_EQ(0.0f, state.holdLinear);
}

TEST(MonitorMeasure, MeterGainReductionRequiresIdentity) {
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    harken::core::MonitorMeterGainReductionSample missing =
        harken::core::ResolveMeterGainReduction(
            tap,
            [](const VnMonitorTapDescriptor&, void*) {
                harken::core::MonitorMeterGainReductionSample sample{};
                sample.valid = true;
                sample.reductionDb = 3.0f;
                sample.ownerStableId = harkentest::ObjectId(1);
                sample.ownerInstanceHandle = 2;
                sample.ownerGeneration = 3;
                return sample;
            },
            nullptr);
    EXPECT_FALSE(missing.valid);
}

TEST(MonitorMeasure, MeterDispatchPublishesSamplePeak) {
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapDescriptorId = 11;
    tap.tapDescriptorGeneration = 3;
    VnMonitorMeasurementRequest request =
        MakeMeter(VN_MONITOR_MEASURE_SAMPLE_PEAK);
    std::array<float, 8> samples = {0.1f, -0.2f, 0.3f, -0.4f, 0.5f, -0.6f, 0.7f, -0.8f};
    std::array<std::uint8_t, 2048> packed{};
    const std::uint32_t packedBytes = harken::monitor::PackAudioChunkRecord(
        packed.data(), static_cast<std::uint32_t>(packed.size()), 42, 7, 19, 11, 3, 0,
        48000, 1, 0, 0, 8, samples.data(), 8);
    ASSERT_NE(0u, packedBytes);
    struct Collected {
        std::vector<std::uint32_t> kinds;
        std::vector<std::uint32_t> validity;
        std::vector<std::uint32_t> lufs;
    } collected;
    harken::monitor::DispatchDerivedAudioChunk(
        &request, 1, &tap, 1, packed.data(), packedBytes,
        [](void* raw, const harken::monitor::DerivedEmit& emit) {
            auto* typed = static_cast<Collected*>(raw);
            VnMonitorDecodedRecord decoded[1]{};
            std::uint8_t skip[8]{};
            const VnMonitorDecodeResult decodedResult = vn_monitor_decode_records(
                emit.bytes, emit.size, decoded, 1, skip, sizeof(skip));
            if (decodedResult.status != VN_OK ||
                decodedResult.recordsWritten != 1) {
                return false;
            }
            typed->kinds.push_back(decoded[0].header.recordKind);
            typed->validity.push_back(decoded[0].header.validityFlags);
            if (decoded[0].payloadBytes < 16u) {
                typed->lufs.push_back(0xffffffffu);
                return false;
            }
            typed->lufs.push_back(
                static_cast<std::uint32_t>(decoded[0].payload[12]) |
                (static_cast<std::uint32_t>(decoded[0].payload[13]) << 8) |
                (static_cast<std::uint32_t>(decoded[0].payload[14]) << 16) |
                (static_cast<std::uint32_t>(decoded[0].payload[15]) << 24));
            return true;
        },
        &collected);
    ASSERT_EQ(1u, collected.kinds.size());
    EXPECT_EQ(VN_MONITOR_RECORD_METER, collected.kinds[0]);
    EXPECT_NE(0u, collected.validity[0] & VN_MONITOR_VALID_NUMERIC);
    EXPECT_EQ(0u, collected.lufs[0]);
}

TEST(MonitorMeasure, MeterDispatchTruePeakNumericBeforeRmsWindow) {
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapDescriptorId = 21;
    tap.tapDescriptorGeneration = 5;
    VnMonitorMeasurementRequest request =
        MakeMeter(VN_MONITOR_MEASURE_TRUE_PEAK);
    std::array<float, 16> samples{};
    samples[8] = 1.0f;
    std::array<std::uint8_t, 2048> packed{};
    const std::uint32_t packedBytes = harken::monitor::PackAudioChunkRecord(
        packed.data(), static_cast<std::uint32_t>(packed.size()), 42, 7, 19, 21, 5,
        0, 48000, 1, 0, 0, 16, samples.data(), 16);
    ASSERT_NE(0u, packedBytes);
    struct Collected {
        std::vector<std::uint32_t> validity;
    } collected;
    harken::monitor::DispatchDerivedAudioChunk(
        &request, 1, &tap, 1, packed.data(), packedBytes,
        [](void* raw, const harken::monitor::DerivedEmit& emit) {
            auto* typed = static_cast<Collected*>(raw);
            VnMonitorDecodedRecord decoded[1]{};
            std::uint8_t skip[8]{};
            const VnMonitorDecodeResult decodedResult = vn_monitor_decode_records(
                emit.bytes, emit.size, decoded, 1, skip, sizeof(skip));
            if (decodedResult.status != VN_OK ||
                decodedResult.recordsWritten != 1) {
                return false;
            }
            typed->validity.push_back(decoded[0].header.validityFlags);
            return true;
        },
        &collected);
    ASSERT_EQ(1u, collected.validity.size());
    EXPECT_NE(0u, collected.validity[0] & VN_MONITOR_VALID_NUMERIC);
    EXPECT_EQ(0u, collected.validity[0] & VN_MONITOR_VALID_WARMUP);
}

TEST(MonitorMeasure, MomentaryDispatchPublishesWarmupNumericAndRearmsAfterDiscontinuities) {
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapDescriptorId = 81;
    tap.tapDescriptorGeneration = 9;
    auto request = MakeMeter(VN_MONITOR_MEASURE_LUFS_MOMENTARY);
    ASSERT_EQ(VN_OK, harken::monitor::ValidateDerivedRequest(request, tap));
    struct Reading {
        std::uint32_t validity = 0;
        VnMonitorMeterPayload payload{};
        std::uint32_t count = 0;
    } reading;
    const auto submit = [&](std::uint64_t begin, std::uint64_t segment, std::uint64_t plan,
                            bool invalid = false) {
        std::array<float, 2048> samples{};
        for (std::size_t frame = 0; frame < 1024; ++frame) {
            samples[frame * 2] = static_cast<float>(0.1 * std::sin(
                2.0 * std::acos(-1.0) * (begin + frame) / 48.0));
            samples[frame * 2 + 1] = -samples[frame * 2];
        }
        if (invalid) {
            samples[0] = std::numeric_limits<float>::quiet_NaN();
        }
        std::array<std::uint8_t, 10000> packed{};
        const auto bytes = harken::monitor::PackAudioChunkRecord(
            packed.data(), static_cast<std::uint32_t>(packed.size()), begin + 1, plan,
            begin, 81, 9, 0, 48000, 2, VN_CHANNEL_LAYOUT_STEREO,
            begin, begin + 1024, samples.data(), 1024, segment);
        ASSERT_NE(0u, bytes);
        const auto count = reading.count;
        harken::monitor::DispatchDerivedAudioChunk(
            &request, 1, &tap, 1, packed.data(), bytes,
            [](void* raw, const harken::monitor::DerivedEmit& emit) {
                auto& out = *static_cast<Reading*>(raw);
                VnMonitorDecodedRecord decoded[1]{};
                std::uint8_t skip[8]{};
                const auto result = vn_monitor_decode_records(
                    emit.bytes, emit.size, decoded, 1, skip, sizeof(skip));
                if (result.status != VN_OK || result.recordsWritten != 1 ||
                    decoded[0].payloadBytes != sizeof(out.payload)) {
                    return false;
                }
                out.validity = decoded[0].header.validityFlags;
                std::memcpy(&out.payload, decoded[0].payload, sizeof(out.payload));
                ++out.count;
                return true;
            }, &reading);
        EXPECT_EQ(count + 1, reading.count);
    };
    for (std::uint64_t block = 0; block < 20; ++block) {
        submit(block * 1024, 1, 3);
        EXPECT_NE(0u, reading.validity & (block < 18 ? VN_MONITOR_VALID_WARMUP : VN_MONITOR_VALID_NUMERIC));
    }
    EXPECT_EQ(VN_MONITOR_MEASURE_LUFS_MOMENTARY, reading.payload.algorithmId);
    EXPECT_EQ(1u, reading.payload.lufsSupported);
    EXPECT_NEAR(-19.993296, std::bit_cast<float>(reading.payload.lufsMomentaryBits), 0.03f);
    submit(22 * 1024, 1, 3);
    EXPECT_NE(0u, reading.validity & VN_MONITOR_VALID_WARMUP);
    for (std::uint64_t block = 23; block < 42; ++block) {
        submit(block * 1024, 1, 3);
    }
    EXPECT_NE(0u, reading.validity & VN_MONITOR_VALID_NUMERIC);
    submit(42 * 1024, 2, 3);
    EXPECT_NE(0u, reading.validity & VN_MONITOR_VALID_WARMUP);
    for (std::uint64_t block = 43; block < 62; ++block) {
        submit(block * 1024, 2, 3);
    }
    EXPECT_NE(0u, reading.validity & VN_MONITOR_VALID_NUMERIC);
    submit(62 * 1024, 2, 4);
    EXPECT_NE(0u, reading.validity & VN_MONITOR_VALID_WARMUP);
    for (std::uint64_t block = 63; block < 82; ++block) {
        submit(block * 1024, 2, 4);
    }
    EXPECT_NE(0u, reading.validity & VN_MONITOR_VALID_NUMERIC);
    for (std::uint64_t block = 82; block < 85; ++block) {
        submit(block * 1024, 2, 4, true);
        EXPECT_EQ(VN_MONITOR_VALID_ARMED, reading.validity);
    }
    for (std::uint64_t block = 85; block < 104; ++block) {
        submit(block * 1024, 2, 4);
        EXPECT_EQ(VN_MONITOR_VALID_ARMED | (block < 103 ? VN_MONITOR_VALID_WARMUP : VN_MONITOR_VALID_NUMERIC),
                  reading.validity);
    }
    EXPECT_NEAR(-19.993296, std::bit_cast<float>(reading.payload.lufsMomentaryBits), 0.03f);
}

TEST(MonitorMeasure, MeterDispatchSkipsGainReductionWithoutIdentity) {
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapDescriptorId = 11;
    tap.tapDescriptorGeneration = 3;
    VnMonitorMeasurementRequest request =
        MakeMeter(VN_MONITOR_MEASURE_GAIN_REDUCTION);
    std::array<float, 4> samples = {0.1f, 0.2f, 0.3f, 0.4f};
    std::array<std::uint8_t, 2048> packed{};
    const std::uint32_t packedBytes = harken::monitor::PackAudioChunkRecord(
        packed.data(), static_cast<std::uint32_t>(packed.size()), 1, 1, 1, 11, 3, 0,
        48000, 1, 0, 0, 4, samples.data(), 4);
    ASSERT_NE(0u, packedBytes);
    std::uint32_t count = 0;
    harken::monitor::DispatchDerivedAudioChunk(
        &request, 1, &tap, 1, packed.data(), packedBytes,
        [](void* raw, const harken::monitor::DerivedEmit&) {
            ++*static_cast<std::uint32_t*>(raw);
            return true;
        },
        &count);
    EXPECT_EQ(0u, count);
}

TEST(MonitorMeasure, MeterDispatchPublishesGainReductionWithIdentity) {
    VnMonitorTapDescriptor tap{};
    tap.structSize = sizeof(tap);
    tap.tapDescriptorId = 11;
    tap.tapDescriptorGeneration = 3;
    tap.ownerStableId = harkentest::ObjectId(9);
    tap.ownerInstanceHandle = 8;
    tap.ownerGeneration = 7;
    VnMonitorMeasurementRequest request =
        MakeMeter(VN_MONITOR_MEASURE_GAIN_REDUCTION);
    std::array<float, 4> samples = {0.1f, 0.2f, 0.3f, 0.4f};
    std::array<std::uint8_t, 2048> packed{};
    const std::uint32_t packedBytes = harken::monitor::PackAudioChunkRecord(
        packed.data(), static_cast<std::uint32_t>(packed.size()), 1, 1, 1, 11, 3, 0,
        48000, 1, 0, 0, 4, samples.data(), 4);
    ASSERT_NE(0u, packedBytes);
    harken::monitor::meter_test::SetGainReductionLookupForTests(&TestGainLookup,
                                                              nullptr);
    struct Collected {
        std::vector<std::uint32_t> kinds;
    } collected;
    harken::monitor::DispatchDerivedAudioChunk(
        &request, 1, &tap, 1, packed.data(), packedBytes,
        [](void* raw, const harken::monitor::DerivedEmit& emit) {
            auto* typed = static_cast<Collected*>(raw);
            const std::uint32_t kind =
                static_cast<std::uint32_t>(emit.bytes[0]) |
                (static_cast<std::uint32_t>(emit.bytes[1]) << 8) |
                (static_cast<std::uint32_t>(emit.bytes[2]) << 16) |
                (static_cast<std::uint32_t>(emit.bytes[3]) << 24);
            typed->kinds.push_back(kind);
            return true;
        },
        &collected);
    harken::monitor::meter_test::SetGainReductionLookupForTests(nullptr, nullptr);
    ASSERT_EQ(1u, collected.kinds.size());
    EXPECT_EQ(VN_MONITOR_RECORD_GAIN_REDUCTION, collected.kinds[0]);
}

TEST(MonitorMeasure, MeterAggregateRequiresSourceRange) {
    VnMonitorAggregatePayload payload{};
    payload.structSize = 40;
    std::array<std::uint8_t, 512> packed{};
    EXPECT_EQ(0u, harken::monitor::PackAggregateRecord(
                      packed.data(), static_cast<std::uint32_t>(packed.size()), 1,
                      1, 1, payload, VN_MONITOR_VALID_ARMED));
    payload.firstSourceSequence = 4;
    payload.lastSourceSequence = 8;
    EXPECT_NE(0u, harken::monitor::PackAggregateRecord(
                      packed.data(), static_cast<std::uint32_t>(packed.size()), 1,
                      1, 1, payload, VN_MONITOR_VALID_ARMED));
}

TEST(MonitorMeasure, MeterPlanAcceptsSamplePeak) {
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
    VnMonitorMeasurementRequest request =
        MakeMeter(VN_MONITOR_MEASURE_SAMPLE_PEAK);
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

TEST(MonitorMeasure, MeterRtCaptureOnOffPassthroughUnchanged) {
    const std::vector<float> sine = harkentest::MakeSine(256, 750.0, 48000.0, 0.7f);
    std::vector<float> offLeft;
    {
        harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
        ASSERT_EQ(VN_OK, session.InitResult());
        const VnPCMDesc pcm = harkentest::MonoDesc(sine, 48000);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(31), &pcm));
        const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(31));
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        harkentest::Capture out;
        harkentest::PumpAndCapture(session.Engine(), 1, out);
        offLeft = out.left;
    }
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
    VnMonitorMeasurementRequest request =
        MakeMeter(VN_MONITOR_MEASURE_SAMPLE_PEAK);
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
    const VnPCMDesc pcm = harkentest::MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(32), &pcm));
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(32));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    harkentest::Capture onOut;
    harkentest::PumpAndCapture(session.Engine(), 1, onOut);
    ASSERT_EQ(offLeft.size(), onOut.left.size());
    for (std::size_t i = 0; i < offLeft.size(); ++i) {
        ASSERT_EQ(offLeft[i], onOut.left[i]) << "sample " << i;
    }
    VnMonitorEffectiveCapturePlan stopped{};
    stopped.structSize = sizeof(stopped);
    EXPECT_EQ(VN_OK, Vn_MonitorStopCapture(handle, lease.leaseGeneration, &stopped));
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, MeterRtAudioCallbackNoAllocOrLock) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(0, 256));
    ASSERT_EQ(VN_OK, session.InitResult());
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
    VnMonitorMeasurementRequest request = MakeMeter(VN_MONITOR_MEASURE_TRUE_PEAK);
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
    const std::vector<float> sine = harkentest::MakeSine(256, 750.0, 48000.0, 0.7f);
    const VnPCMDesc pcm = harkentest::MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(33), &pcm));
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(33));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    const std::uint64_t before = harken::memguard::HitCount();
    const std::uint32_t mutexBefore = harken::monitor::PublishMutexHitsForTests();
    harkentest::Capture out;
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    EXPECT_EQ(before, harken::memguard::HitCount());
    EXPECT_EQ(mutexBefore, harken::monitor::PublishMutexHitsForTests());
    EXPECT_EQ(VN_OK, Vn_MonitorCloseSession(handle));
}

TEST(MonitorMeasure, MeterRtZeroGenerationIsNotRuntimeVerified) {
    harken::core::MonitorMeterState state{};
    harken::core::ResetMeterState(&state, 48000, 0);
    const float peak = 0.25f;
    (void)harken::core::ReduceMeterChunk(&state, &peak, 1, 1,
                                        VN_MONITOR_CHANNEL_PER_CHANNEL);
    harken::core::NoteMeterSegment(&state, 0);
    EXPECT_EQ(0u, state.segmentGeneration);
    EXPECT_TRUE(state.holdReady);
}
