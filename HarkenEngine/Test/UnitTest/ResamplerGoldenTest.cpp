// Golden cases: the resample stage (FIFO + interpolation kernels) driven
// directly, per kernel family.
//
// Tolerances:
//   - Unity-ratio identity: linear and Hermite are exact (frac == 0 reduces
//     both kernels to the input sample bit-for-bit; tolerance 0). The sinc
//     families reduce to a normalized near-delta row whose off-center taps
//     carry ~1e-15 numerical residue; tolerance 1e-5.
//   - Known-ratio sine: ratio 0.5 is compared per sample against the analytic
//     sine. Ratio 1.5 uses a coherent tone projection because downsampling
//     deliberately changes the interpolation waveform while preserving the
//     requested frequency and passband amplitude.
//   - Output-length accounting: input N frames at ratio r produces
//     ceil(N / r) frames within one frame, then the stage reports
//     exhausted.
#include "harken/dsp/Resampler.h"

#include "Common/SignalTestSupport.h"

#include <algorithm>
#include <cmath>

namespace {

using harken::kResampleChunkFrames;
using harken::kResampleFifoFrames;
using harken::kResampleHistoryPad;
using harken::PCMView;
using harken::Resampler;
using harken::ResampleQuality;
using harken::ResampleTables;
using harken::SampleKaiserSincPrototype;

const ResampleTables* Tables() {
    static ResampleTables tables;
    static bool initialized = false;
    if (!initialized) {
        tables.Init();
        initialized = true;
    }
    return &tables;
}

void PrimeRatioRampResampler(Resampler* resampler,
                             std::vector<float>* ring) {
    const std::vector<float> input = harkentest::MakeDC(64, 0.25f);
    PCMView view;
    view.channels[0] = input.data();
    view.channelCount = 1;
    view.stride = 1;
    view.sampleRate = 48000;
    view.frames = input.size();

    ring->assign(kResampleFifoFrames, 0.0f);
    float* rings[1] = {ring->data()};
    resampler->Reset(rings, 1, ResampleQuality::Linear, 1.0, Tables());
    resampler->PushPCM(view, 0, static_cast<std::uint32_t>(input.size()));
}

void ProduceOne(Resampler* resampler) {
    float output = 0.0f;
    float* dst[1] = {&output};
    ASSERT_EQ(1u, resampler->Produce(dst, 0, 1));
}

// Drives the stage the same way the cascade does: produce until starved,
// feed a fixed-size chunk (or flush silence), repeat until exhausted.
std::vector<float> RunResampler(ResampleQuality quality, double ratio,
                                const std::vector<float>& input,
                                std::size_t maxOut) {
    PCMView view;
    view.channels[0] = input.data();
    view.channelCount = 1;
    view.stride = 1;
    view.sampleRate = 48000;
    view.frames = input.size();

    std::vector<float> ring(kResampleFifoFrames, 0.0f);
    float* rings[1] = {ring.data()};

    Resampler rs;
    rs.Reset(rings, 1, quality, ratio, Tables());

    std::vector<float> out(maxOut, 0.0f);
    float* dst[1] = {out.data()};

    std::uint64_t cursor = 0;
    std::uint32_t produced = 0;
    while (produced < maxOut) {
        produced += rs.Produce(dst, produced,
                               static_cast<std::uint32_t>(maxOut) - produced);
        if (produced >= maxOut) {
            break;
        }
        if (cursor < view.frames) {
            const std::uint32_t chunk = static_cast<std::uint32_t>(
                std::min<std::uint64_t>({kResampleChunkFrames,
                                         view.frames - cursor,
                                         rs.FreeFrames()}));
            rs.PushPCM(view, cursor, chunk);
            cursor += chunk;
            if (cursor == view.frames) {
                rs.MarkInputEnd();
            }
        } else if (!rs.Exhausted()) {
            rs.PushSilence(std::min(kResampleChunkFrames, rs.FreeFrames()));
        } else {
            break;
        }
    }
    out.resize(produced);
    return out;
}

constexpr double kTwoPi = 6.28318530717958647692;

double FoldCyclesToNyquist(double cyclesPerSample) {
    double wrapped = cyclesPerSample - std::floor(cyclesPerSample);
    if (wrapped > 0.5)
        wrapped = 1.0 - wrapped;
    return std::abs(wrapped);
}

double ProjectedToneAmplitude(const std::vector<float>& samples,
                              std::size_t start, std::size_t frames,
                              double cyclesPerSample) {
    double real = 0.0;
    double imaginary = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        const double phase = kTwoPi * cyclesPerSample *
                             static_cast<double>(i);
        const double sample = static_cast<double>(samples[start + i]);
        real += sample * std::cos(phase);
        imaginary -= sample * std::sin(phase);
    }
    return 2.0 * std::sqrt(real * real + imaginary * imaginary) /
           static_cast<double>(frames);
}

double AmplitudeToDb(double amplitude) {
    constexpr double kAnalysisFloor = 1.0e-15;
    return 20.0 * std::log10(std::max(amplitude, kAnalysisFloor));
}

std::vector<float> RunTone(ResampleQuality quality, double ratio,
                           double sourceFrequency, float amplitude,
                           std::size_t maxOut) {
    const std::size_t inputFrames = static_cast<std::size_t>(
        static_cast<double>(maxOut + kResampleChunkFrames) * ratio) +
        kResampleChunkFrames;
    const std::vector<float> input =
        harkentest::MakeSine(inputFrames, sourceFrequency, 48000.0, amplitude);
    return RunResampler(quality, ratio, input, maxOut);
}

std::vector<float> RunRampedTone(ResampleQuality quality, double initialRatio,
                                 double targetRatio,
                                 std::uint32_t rampFrames,
                                 double sourceFrequency, float amplitude,
                                 std::size_t maxOut) {
    const double maximumRatio = std::max(initialRatio, targetRatio);
    const std::size_t inputFrames = static_cast<std::size_t>(
        static_cast<double>(maxOut + kResampleChunkFrames) * maximumRatio) +
        kResampleChunkFrames;
    const std::vector<float> input =
        harkentest::MakeSine(inputFrames, sourceFrequency, 48000.0, amplitude);

    PCMView view;
    view.channels[0] = input.data();
    view.channelCount = 1;
    view.stride = 1;
    view.sampleRate = 48000;
    view.frames = input.size();

    std::vector<float> ring(kResampleFifoFrames, 0.0f);
    float* rings[1] = {ring.data()};
    Resampler rs;
    rs.Reset(rings, 1, quality, initialRatio, Tables());
    rs.UpdateRatio(targetRatio, rampFrames);

    std::vector<float> out(maxOut, 0.0f);
    float* dst[1] = {out.data()};
    std::uint64_t cursor = 0;
    std::uint32_t produced = 0;
    while (produced < maxOut) {
        produced += rs.Produce(dst, produced,
                               static_cast<std::uint32_t>(maxOut) - produced);
        if (produced >= maxOut)
            break;
        if (cursor < view.frames) {
            const std::uint32_t chunk = static_cast<std::uint32_t>(
                std::min<std::uint64_t>({kResampleChunkFrames,
                                         view.frames - cursor,
                                         rs.FreeFrames()}));
            rs.PushPCM(view, cursor, chunk);
            cursor += chunk;
            if (cursor == view.frames)
                rs.MarkInputEnd();
        } else if (!rs.Exhausted()) {
            rs.PushSilence(std::min(kResampleChunkFrames, rs.FreeFrames()));
        } else {
            break;
        }
    }
    out.resize(produced);
    return out;
}

void PushFramesInChunks(Resampler& rs, const PCMView& view,
                        std::uint64_t& cursor, std::uint32_t frames) {
    while (frames > 0) {
        const std::uint32_t chunk = static_cast<std::uint32_t>(
            std::min<std::uint64_t>({kResampleChunkFrames, frames,
                                     rs.FreeFrames()}));
        ASSERT_GT(chunk, 0u);
        rs.PushPCM(view, cursor, chunk);
        cursor += chunk;
        frames -= chunk;
    }
}

float ReferenceScaledSinc16AtInteger(const std::vector<float>& input,
                                     std::uint64_t absoluteIndex,
                                     double scale) {
    constexpr std::uint32_t kMaxNeed = 121;
    constexpr std::uint32_t kMaxCount = 2 * kMaxNeed + 1;
    const std::uint32_t need =
        static_cast<std::uint32_t>(std::ceil(7.5 * scale)) + 1;
    const std::uint32_t count = 2 * need + 1;
    float coefficients[kMaxCount] = {};
    double sum = 0.0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const int offset =
            static_cast<int>(i) - static_cast<int>(need);
        const float coefficient = SampleKaiserSincPrototype(
            Tables()->sinc16,
            static_cast<float>(static_cast<double>(offset) / scale));
        coefficients[i] = coefficient;
        sum += static_cast<double>(coefficient);
    }
    const float inverseSum = static_cast<float>(1.0 / sum);

    float value = 0.0f;
    for (std::uint32_t i = 0; i < count; ++i) {
        const std::uint64_t sampleIndex = absoluteIndex - need + i;
        if (sampleIndex >= kResampleHistoryPad) {
            const std::uint64_t inputIndex =
                sampleIndex - kResampleHistoryPad;
            if (inputIndex < input.size()) {
                value += coefficients[i] * inverseSum * input[inputIndex];
            }
        }
    }
    return value;
}

struct BoundedRun {
    std::vector<float> out;
    bool exhausted = false;
};

// RunResampler with a hard iteration budget, for the extreme ratios where a
// starved-stage defect would otherwise spin forever. The budget is four
// times a generous healthy-run estimate; exceeding it is reported with the
// live ring state and leaves exhausted false, so a deadlock is an assertion
// failure inside a finite budget rather than a hang.
BoundedRun RunResamplerBounded(ResampleQuality quality, double ratio,
                               const std::vector<float>& input,
                               std::size_t maxOut) {
    PCMView view;
    view.channels[0] = input.data();
    view.channelCount = 1;
    view.stride = 1;
    view.sampleRate = 48000;
    view.frames = input.size();

    std::vector<float> ring(kResampleFifoFrames, 0.0f);
    float* rings[1] = {ring.data()};

    Resampler rs;
    rs.Reset(rings, 1, quality, ratio, Tables());

    std::vector<float> out(maxOut, 0.0f);
    float* dst[1] = {out.data()};

    const std::size_t budget =
        4 * (2 * (input.size() / kResampleChunkFrames + 1) + maxOut + 16);
    std::size_t iterations = 0;
    bool budgetExceeded = false;

    std::uint64_t cursor = 0;
    std::uint32_t produced = 0;
    while (produced < maxOut) {
        if (++iterations > budget) {
            budgetExceeded = true;
            break;
        }
        produced += rs.Produce(dst, produced,
                               static_cast<std::uint32_t>(maxOut) - produced);
        if (produced >= maxOut) {
            break;
        }
        if (cursor < view.frames) {
            const std::uint32_t chunk = static_cast<std::uint32_t>(
                std::min<std::uint64_t>({kResampleChunkFrames,
                                         view.frames - cursor,
                                         rs.FreeFrames()}));
            rs.PushPCM(view, cursor, chunk);
            cursor += chunk;
            if (cursor == view.frames) {
                rs.MarkInputEnd();
            }
        } else if (!rs.Exhausted()) {
            rs.PushSilence(std::min(kResampleChunkFrames, rs.FreeFrames()));
        } else {
            break;
        }
    }
    out.resize(produced);
    if (budgetExceeded) {
        ADD_FAILURE() << "resample stage made no progress within " << budget
                      << " iterations at ratio " << ratio
                      << ": readPos=" << rs.readPos
                      << " writeIndex=" << rs.writeIndex
                      << " FreeFrames=" << rs.FreeFrames()
                      << " produced=" << produced;
    }
    BoundedRun result;
    result.out.swap(out);
    result.exhausted = rs.Exhausted();
    return result;
}

struct FamilyCase {
    ResampleQuality quality;
    const char* name;
    float identityTolerance;
    float sineTolerance;
};

const FamilyCase kFamilies[] = {
    {ResampleQuality::Linear, "linear", 0.0f, 5e-3f},
    {ResampleQuality::Hermite, "hermite", 0.0f, 1e-3f},
    {ResampleQuality::Sinc8, "sinc8", 1e-5f, 2e-3f},
    {ResampleQuality::Sinc16, "sinc16", 1e-5f, 1e-3f},
};

struct DownsampleSpectrumCase {
    ResampleQuality quality;
    const char* name;
    double maxAliasDbFs;
    double maxPassbandErrorDb;
};

const DownsampleSpectrumCase kDownsampleSpectrumCases[] = {
    {ResampleQuality::Hermite, "hermite", -30.0, 1.5},
    {ResampleQuality::Sinc16, "sinc16", -50.0, 0.5},
};

TEST(ResamplerGolden, UnityRatioIsIdentityForEveryFamily) {
    const std::vector<float> sine =
        harkentest::MakeSine(400, 1000.0, 48000.0, 0.5f);
    HARKENTEST_ASSERT_NON_SILENT(sine);

    for (const FamilyCase& family : kFamilies) {
        SCOPED_TRACE(family.name);
        const std::vector<float> out =
            RunResampler(family.quality, 1.0, sine, sine.size());
        ASSERT_EQ(sine.size(), out.size());
        HARKENTEST_ASSERT_NON_SILENT(out);
        for (std::size_t i = 0; i < sine.size(); ++i) {
            if (family.identityTolerance == 0.0f) {
                ASSERT_EQ(sine[i], out[i]) << "sample " << i;
            } else {
                ASSERT_NEAR(sine[i], out[i], family.identityTolerance)
                    << "sample " << i;
            }
        }
    }
}

TEST(ResamplerGolden, KnownRatioSineMatchesAnalyticFrequency) {
    constexpr double kSourceFreq = 1000.0;
    constexpr double kSampleRate = 48000.0;
    constexpr float kAmplitude = 0.8f;
    const std::vector<float> sine =
        harkentest::MakeSine(2000, kSourceFreq, kSampleRate, kAmplitude);
    HARKENTEST_ASSERT_NON_SILENT(sine);

    const double ratios[] = {0.5, 1.5};
    for (const FamilyCase& family : kFamilies) {
        for (const double ratio : ratios) {
            SCOPED_TRACE(::testing::Message()
                         << family.name << " ratio " << ratio);
            const std::size_t expectFrames =
                static_cast<std::size_t>(sine.size() / ratio);
            const std::vector<float> out = RunResampler(
                family.quality, ratio, sine, expectFrames);
            ASSERT_GE(out.size(), static_cast<std::size_t>(64));
            HARKENTEST_ASSERT_NON_SILENT(out);

            if (ratio <= 1.0) {
                // Output sample n sits at source position n*ratio, i.e. a sine
                // of frequency sourceFreq*ratio at the output rate.
                for (std::size_t n = 16; n + 16 < out.size(); ++n) {
                    const double position = static_cast<double>(n) * ratio;
                    const float expected =
                        kAmplitude *
                        static_cast<float>(
                            std::sin(kTwoPi * kSourceFreq * position /
                                     kSampleRate));
                    ASSERT_NEAR(expected, out[n], family.sineTolerance)
                        << "sample " << n;
                }
            } else {
                constexpr std::size_t kStart = 128;
                constexpr std::size_t kFrames = 1024;
                ASSERT_GE(out.size(), kStart + kFrames);
                ASSERT_GT(kAmplitude, 1.0e-12f);
                const double expectedCycles =
                    kSourceFreq * ratio / kSampleRate;
                const double measured = ProjectedToneAmplitude(
                    out, kStart, kFrames, expectedCycles);
                const double errorDb =
                    AmplitudeToDb(measured / static_cast<double>(kAmplitude));
                const double toleranceDb =
                    family.quality == ResampleQuality::Sinc16 ? 0.5 : 1.5;
                EXPECT_LE(std::abs(errorDb), toleranceDb);
            }
        }
    }
}

TEST(ResamplerGolden, UpdateRatioUsesHistoryReservedBeforeRingWrap) {
    constexpr std::uint32_t kInitialInputFrames =
        kResampleFifoFrames - kResampleHistoryPad;
    constexpr std::uint32_t kSteadyOutputFrames = 400;
    constexpr std::uint64_t kUpdatePosition =
        kResampleHistoryPad + kSteadyOutputFrames;
    constexpr std::uint64_t kProbeHistoryDistance = 12;
    constexpr std::uint64_t kOriginalAbsolute =
        kUpdatePosition - kProbeHistoryDistance;
    constexpr std::uint64_t kOverwritingAbsolute =
        kOriginalAbsolute + kResampleFifoFrames;
    std::vector<float> input(1200, 0.0f);
    input[kOriginalAbsolute - kResampleHistoryPad] = 1.0f;
    input[kOverwritingAbsolute - kResampleHistoryPad] = -1.0f;

    PCMView view;
    view.channels[0] = input.data();
    view.channelCount = 1;
    view.stride = 1;
    view.sampleRate = 48000;
    view.frames = input.size();

    std::vector<float> ring(kResampleFifoFrames, 0.0f);
    float* rings[1] = {ring.data()};
    Resampler rs;
    rs.Reset(rings, 1, ResampleQuality::Sinc16, 1.0, Tables());

    std::uint64_t cursor = 0;
    PushFramesInChunks(rs, view, cursor, kInitialInputFrames);
    std::vector<float> steady(kSteadyOutputFrames, 0.0f);
    float* steadyDst[1] = {steady.data()};
    ASSERT_EQ(kSteadyOutputFrames,
              rs.Produce(steadyDst, 0, kSteadyOutputFrames));
    ASSERT_DOUBLE_EQ(static_cast<double>(kUpdatePosition), rs.readPos);
    ASSERT_EQ(kResampleFifoFrames, rs.writeIndex);

    // At p=408, the fixed Sinc16 reserve starts at absolute frame 287.
    // With writeIndex=512, 225 frames occupy the ring and exactly 287 remain
    // writable. A ratio-dependent eight-frame reserve would instead report
    // 400 and let the second impulse overwrite the first through ring wrap.
    constexpr std::uint32_t kExpectedFreeFrames = 287;
    const std::uint32_t refill = rs.FreeFrames();
    EXPECT_EQ(kExpectedFreeFrames, refill);
    PushFramesInChunks(rs, view, cursor, refill);
    EXPECT_EQ(0u, rs.FreeFrames());

    rs.UpdateRatio(8.0, 0);
    float actual = 0.0f;
    float* actualDst[1] = {&actual};
    ASSERT_EQ(1u, rs.Produce(actualDst, 0, 1));
    const float reference =
        ReferenceScaledSinc16AtInteger(input, kUpdatePosition, 8.0);
    ASSERT_GT(std::abs(reference), 1.0e-3f)
        << "history probe is too weak to distinguish an overwritten sample";
    EXPECT_NEAR(reference, actual, 1.0e-6f);
}

TEST(ResamplerGolden, RatioCrossingOneRemainsFiniteAndProgresses) {
    constexpr float kInputAmplitude = 0.5f;
    constexpr std::uint32_t kFramesPerPhase = 64;
    const std::vector<float> input =
        harkentest::MakeSine(4096, 3000.0, 48000.0, kInputAmplitude);
    HARKENTEST_ASSERT_NON_SILENT(input);

    PCMView view;
    view.channels[0] = input.data();
    view.channelCount = 1;
    view.stride = 1;
    view.sampleRate = 48000;
    view.frames = input.size();

    std::vector<float> ring(kResampleFifoFrames, 0.0f);
    float* rings[1] = {ring.data()};
    Resampler rs;
    rs.Reset(rings, 1, ResampleQuality::Hermite, 0.75, Tables());

    constexpr double kRatios[] = {0.75, 1.25, 0.75};
    std::vector<float> out(kFramesPerPhase * 3, 0.0f);
    float* dst[1] = {out.data()};
    std::uint64_t cursor = 0;
    std::uint32_t produced = 0;
    for (const double ratio : kRatios) {
        rs.UpdateRatio(ratio, 0);
        const std::uint32_t target = produced + kFramesPerPhase;
        std::uint32_t attempts = 0;
        while (produced < target && ++attempts <= 16) {
            const std::uint32_t before = produced;
            produced += rs.Produce(dst, produced, target - produced);
            if (produced == before) {
                const std::uint32_t chunk = static_cast<std::uint32_t>(
                    std::min<std::uint64_t>({kResampleChunkFrames,
                                             view.frames - cursor,
                                             rs.FreeFrames()}));
                ASSERT_GT(chunk, 0u);
                rs.PushPCM(view, cursor, chunk);
                cursor += chunk;
            }
        }
        ASSERT_EQ(target, produced) << "no forward progress at ratio " << ratio;
    }

    // The input peak is 0.5. A two-times peak allowance is a conservative
    // bound for interpolation and the per-frame normalized FIR sidelobes; this
    // test asserts numerical containment, not transition inaudibility.
    constexpr float kOutputBound = 2.0f * kInputAmplitude;
    for (const float sample : out) {
        EXPECT_TRUE(std::isfinite(sample));
        EXPECT_LE(std::abs(sample), kOutputBound);
    }
}

TEST(ResamplerRatioRamp, HermiteCrossingUnityIsContinuous) {
    constexpr double kInitialRatio = 0.75;
    constexpr double kTargetRatio = 1.25;
    constexpr std::uint32_t kRampFrames = 64;
    constexpr std::uint32_t kOutputFrames = 56;
    constexpr std::uint32_t kFirstScaledFrame = 32;
    constexpr std::uint32_t kReferenceFrames = 16;
    constexpr float kSlopeMargin = 1.05f;
    constexpr float kCurvatureMargin = 2.0f;
    constexpr float kInputAmplitude = 0.5f;
    const std::vector<float> input =
        harkentest::MakeSine(512, 3000.0, 48000.0, kInputAmplitude);
    HARKENTEST_ASSERT_NON_SILENT(input);

    PCMView view;
    view.channels[0] = input.data();
    view.channelCount = 1;
    view.stride = 1;
    view.sampleRate = 48000;
    view.frames = input.size();

    std::vector<float> ring(kResampleFifoFrames, 0.0f);
    float* rings[1] = {ring.data()};
    Resampler rs;
    rs.Reset(rings, 1, ResampleQuality::Hermite, kInitialRatio, Tables());
    std::uint64_t cursor = 0;
    PushFramesInChunks(rs, view, cursor, rs.FreeFrames());
    rs.UpdateRatio(kTargetRatio, kRampFrames);

    std::vector<float> out(kOutputFrames, 0.0f);
    float* dst[1] = {out.data()};
    ASSERT_EQ(kOutputFrames, rs.Produce(dst, 0, kOutputFrames));
    HARKENTEST_ASSERT_NON_SILENT(out);
    for (const float sample : out)
        ASSERT_TRUE(std::isfinite(sample));

    constexpr double kRampStep =
        (kTargetRatio - kInitialRatio) / kRampFrames;
    ASSERT_DOUBLE_EQ(1.0,
                     kInitialRatio + kRampStep * kFirstScaledFrame);
    ASSERT_GT(kInitialRatio + kRampStep * (kFirstScaledFrame + 1u),
              1.0);
    EXPECT_DOUBLE_EQ(kInitialRatio + kRampStep * kOutputFrames, rs.ratio);
    EXPECT_EQ(kRampFrames - kOutputFrames, rs.rampRemaining);

    // Ratio advances before each output: frame 31 is the last native Hermite
    // sample at unity, and frame 32 is the first scaled-kernel sample.
    float preD1Max = 0.0f;
    float preD2Max = 0.0f;
    for (std::uint32_t i = kFirstScaledFrame - kReferenceFrames;
         i < kFirstScaledFrame; ++i) {
        preD1Max =
            std::max(preD1Max, std::fabs(out[i] - out[i - 1u]));
        preD2Max = std::max(
            preD2Max,
            std::fabs(out[i] - 2.0f * out[i - 1u] + out[i - 2u]));
    }
    float postD1Max = 0.0f;
    float postD2Max = 0.0f;
    for (std::uint32_t i = kFirstScaledFrame + 2u;
         i < kFirstScaledFrame + 2u + kReferenceFrames; ++i) {
        postD1Max =
            std::max(postD1Max, std::fabs(out[i] - out[i - 1u]));
        postD2Max = std::max(
            postD2Max,
            std::fabs(out[i] - 2.0f * out[i - 1u] + out[i - 2u]));
    }
    const float referenceD1 = std::max(preD1Max, postD1Max);
    const float referenceD2 = std::max(preD2Max, postD2Max);
    ASSERT_GT(referenceD1, 0.0f);
    ASSERT_GT(referenceD2, 0.0f);

    const float crossingD1 = std::max(
        std::fabs(out[kFirstScaledFrame] - out[kFirstScaledFrame - 1u]),
        std::fabs(out[kFirstScaledFrame + 1u] - out[kFirstScaledFrame]));
    const float crossingD2 = std::max(
        std::fabs(out[kFirstScaledFrame] -
                  2.0f * out[kFirstScaledFrame - 1u] +
                  out[kFirstScaledFrame - 2u]),
        std::fabs(out[kFirstScaledFrame + 1u] -
                  2.0f * out[kFirstScaledFrame] +
                  out[kFirstScaledFrame - 1u]));
    EXPECT_LE(crossingD1, referenceD1 * kSlopeMargin);
    EXPECT_LE(crossingD2, referenceD2 * kCurvatureMargin);
}

TEST(ResamplerGolden, RampedDownsamplingUsesCurrentRatioForAliasRejection) {
    constexpr double kInitialRatio = 0.9;
    constexpr double kTargetRatio = 2.2;
    constexpr std::uint32_t kRampFrames = 480;
    constexpr std::uint32_t kPrefixFrames = 230;
    constexpr std::uint32_t kLateWindowFrames = 64;
    constexpr double kSourceFrequency = 20000.0;
    constexpr float kAmplitude = 0.5f;
    const std::vector<float> input =
        harkentest::MakeSine(4096, kSourceFrequency, 48000.0, kAmplitude);
    HARKENTEST_ASSERT_NON_SILENT(input);

    PCMView view;
    view.channels[0] = input.data();
    view.channelCount = 1;
    view.stride = 1;
    view.sampleRate = 48000;
    view.frames = input.size();

    std::vector<float> ring(kResampleFifoFrames, 0.0f);
    float* rings[1] = {ring.data()};
    Resampler rs;
    rs.Reset(rings, 1, ResampleQuality::Sinc16, kInitialRatio, Tables());

    std::uint64_t cursor = 0;
    PushFramesInChunks(rs, view, cursor, rs.FreeFrames());
    rs.UpdateRatio(kTargetRatio, kRampFrames);

    std::vector<float> out(kRampFrames, 0.0f);
    float* dst[1] = {out.data()};
    ASSERT_EQ(kPrefixFrames, rs.Produce(dst, 0, kPrefixFrames));

    const std::uint32_t refill = rs.FreeFrames();
    ASSERT_GT(refill, 0u);
    PushFramesInChunks(rs, view, cursor, refill);
    const std::uint32_t secondProduced = rs.Produce(
        dst, kPrefixFrames, kRampFrames - kPrefixFrames);
    ASSERT_GE(secondProduced, kLateWindowFrames);

    const std::uint32_t produced = kPrefixFrames + secondProduced;
    const std::uint32_t lateStart = produced - kLateWindowFrames;
    const double rampStep =
        (kTargetRatio - kInitialRatio) / static_cast<double>(kRampFrames);
    const double lateStartRatio =
        kInitialRatio + rampStep * static_cast<double>(lateStart + 1u);
    ASSERT_GE(lateStartRatio, 1.9);
    ASSERT_LT(lateStartRatio, 2.0);
    ASSERT_GT(rs.ratio, 2.0);

    // At these ratios the source is above the current output Nyquist, so any
    // remaining signal is folded alias energy. The window also crosses a
    // scaled-kernel support boundary and pins that change as continuous.
    double squareSum = 0.0;
    float maximumStep = 0.0f;
    for (std::uint32_t i = lateStart; i < produced; ++i) {
        const float sample = out[i];
        ASSERT_TRUE(std::isfinite(sample));
        squareSum += static_cast<double>(sample) * sample;
        if (i > lateStart) {
            maximumStep =
                std::max(maximumStep, std::abs(sample - out[i - 1u]));
        }
    }
    const double aliasAmplitude = std::sqrt(
        2.0 * squareSum / static_cast<double>(kLateWindowFrames));
    EXPECT_LE(AmplitudeToDb(aliasAmplitude), -50.0);
    EXPECT_LE(maximumStep, 0.02f);
}

TEST(ResamplerGolden, RatioSixtyFourTracksTheTrueOutputNyquist) {
    constexpr double kRatio = 64.0;
    constexpr double kSourceFrequency = 1200.0;
    constexpr float kAmplitude = 0.5f;
    constexpr std::size_t kDiscard = 256;
    constexpr std::size_t kFrames = 1280;
    constexpr std::size_t kMaxOut = kDiscard + kFrames;
    const double aliasCycles = FoldCyclesToNyquist(
        kSourceFrequency * kRatio / 48000.0);
    ASSERT_DOUBLE_EQ(0.4, aliasCycles);

    // At 64:1 the true source-domain output Nyquist is 375 Hz. The former
    // scale-16 ceiling instead filtered at 1500 Hz and passed this tone into a
    // 19.2 kHz folded alias. Both quality tiers must follow the actual ratio.
    for (const DownsampleSpectrumCase& c : kDownsampleSpectrumCases) {
        SCOPED_TRACE(c.name);
        const std::vector<float> out = RunTone(
            c.quality, kRatio, kSourceFrequency, kAmplitude, kMaxOut);
        ASSERT_EQ(kMaxOut, out.size());
        const double aliasAmplitude = ProjectedToneAmplitude(
            out, kDiscard, kFrames, aliasCycles);
        EXPECT_LE(AmplitudeToDb(aliasAmplitude), c.maxAliasDbFs);
    }
}

TEST(ResamplerGolden, RampPastSixteenTracksTheCurrentCutoff) {
    constexpr double kInitialRatio = 12.0;
    constexpr double kTargetRatio = 32.0;
    constexpr std::uint32_t kRampFrames = 512;
    constexpr double kSourceFrequency = 1350.0;
    constexpr float kAmplitude = 0.5f;
    constexpr std::size_t kLateStart = 704;
    constexpr std::size_t kLateFrames = 320;
    constexpr std::size_t kMaxOut = kLateStart + kLateFrames;
    const double aliasCycles = FoldCyclesToNyquist(
        kSourceFrequency * kTargetRatio / 48000.0);
    ASSERT_NEAR(0.1, aliasCycles, 1.0e-15);

    const std::vector<float> out = RunRampedTone(
        ResampleQuality::Hermite, kInitialRatio, kTargetRatio, kRampFrames,
        kSourceFrequency, kAmplitude, kMaxOut);
    ASSERT_EQ(kMaxOut, out.size());
    const double aliasAmplitude = ProjectedToneAmplitude(
        out, kLateStart, kLateFrames, aliasCycles);
    EXPECT_LE(AmplitudeToDb(aliasAmplitude), -30.0);
}

TEST(ResamplerGolden, RatioTwoSuppressesEighteenKilohertzAlias) {
    constexpr double kRatio = 2.0;
    constexpr double kSourceFrequency = 18000.0;
    constexpr float kAmplitude = 0.5f;
    constexpr std::size_t kDiscard = 512;
    constexpr std::size_t kFrames = 4800;
    constexpr std::size_t kMaxOut = kDiscard + kFrames;
    const double aliasCycles = FoldCyclesToNyquist(
        kSourceFrequency * kRatio / 48000.0);
    ASSERT_DOUBLE_EQ(0.25, aliasCycles);

    // The direct stage observes 0.25 cycles per output frame. At the engine's
    // 48 kHz output rate that is 12 kHz; it is 6 kHz only if the same frames
    // are instead labelled as a 24 kHz sample-rate-conversion output. For the
    // 8-tap prototype, 18 kHz is exactly the included 1.5-times-cutoff boundary.
    for (const DownsampleSpectrumCase& c : kDownsampleSpectrumCases) {
        SCOPED_TRACE(c.name);
        const std::vector<float> out = RunTone(
            c.quality, kRatio, kSourceFrequency, kAmplitude, kMaxOut);
        ASSERT_EQ(kMaxOut, out.size());
        const double aliasAmplitude = ProjectedToneAmplitude(
            out, kDiscard, kFrames, aliasCycles);
        EXPECT_LE(AmplitudeToDb(aliasAmplitude), c.maxAliasDbFs);
    }
}

TEST(ResamplerGolden, RatioTwoPreservesFourKilohertzPassband) {
    constexpr double kRatio = 2.0;
    constexpr double kSourceFrequency = 4000.0;
    constexpr float kAmplitude = 0.5f;
    constexpr std::size_t kDiscard = 512;
    constexpr std::size_t kFrames = 4800;
    constexpr std::size_t kMaxOut = kDiscard + kFrames;
    const double outputCycles =
        kSourceFrequency * kRatio / 48000.0;
    ASSERT_GT(kAmplitude, 1.0e-12f);

    for (const DownsampleSpectrumCase& c : kDownsampleSpectrumCases) {
        SCOPED_TRACE(c.name);
        const std::vector<float> out = RunTone(
            c.quality, kRatio, kSourceFrequency, kAmplitude, kMaxOut);
        ASSERT_EQ(kMaxOut, out.size());
        const double measured = ProjectedToneAmplitude(
            out, kDiscard, kFrames, outputCycles);
        const double errorDb =
            AmplitudeToDb(measured / static_cast<double>(kAmplitude));
        EXPECT_LE(std::abs(errorDb), c.maxPassbandErrorDb);
    }
}

TEST(ResamplerGolden,
     RatioOnePointFiveSuppressesTwentyOnePointFiveKilohertzAlias) {
    constexpr double kRatio = 1.5;
    constexpr double kSourceFrequency = 21500.0;
    constexpr float kAmplitude = 0.5f;
    constexpr std::size_t kDiscard = 512;
    constexpr std::size_t kFrames = 4800;
    constexpr std::size_t kMaxOut = kDiscard + kFrames;
    const double aliasCycles = FoldCyclesToNyquist(
        kSourceFrequency * kRatio / 48000.0);
    ASSERT_NEAR(21.0 / 64.0, aliasCycles, 1.0e-15);

    // At R=1.5, 21.5 kHz is at and above the 16-tap guarantee beginning at
    // 1.3 times cutoff. It remains below the 8-tap guarantee at 1.5 times
    // cutoff, so this transition-band point carries no Hermite assertion.
    const std::vector<float> out = RunTone(
        ResampleQuality::Sinc16, kRatio, kSourceFrequency, kAmplitude, kMaxOut);
    ASSERT_EQ(kMaxOut, out.size());
    const double aliasAmplitude = ProjectedToneAmplitude(
        out, kDiscard, kFrames, aliasCycles);
    EXPECT_LE(AmplitudeToDb(aliasAmplitude), -50.0);
}

TEST(ResamplerGolden,
     RatioOnePointSevenFiveHermiteSuppressesGuaranteedStopbandAlias) {
    constexpr double kRatio = 7.0 / 4.0;
    constexpr double kSourceFrequency = 21600.0;
    constexpr float kAmplitude = 0.5f;
    constexpr std::size_t kDiscard = 512;
    constexpr std::size_t kFrames = 4800;
    constexpr std::size_t kMaxOut = kDiscard + kFrames;
    const double aliasCycles = FoldCyclesToNyquist(
        kSourceFrequency * kRatio / 48000.0);
    ASSERT_NEAR(17.0 / 80.0, aliasCycles, 1.0e-15);

    // Source frequency 9/20 is above the included 8-tap stopband boundary
    // 1.5 * (1 / (2R)) = 3/7. Multiplication by R gives 63/80 cycles per
    // output frame, which folds to 17/80 (10.2 kHz at the 48 kHz engine rate).
    // The 4800-frame projection spans an integer 1020 folded cycles.
    const std::vector<float> out = RunTone(
        ResampleQuality::Hermite, kRatio, kSourceFrequency, kAmplitude, kMaxOut);
    ASSERT_EQ(kMaxOut, out.size());
    const double aliasAmplitude = ProjectedToneAmplitude(
        out, kDiscard, kFrames, aliasCycles);
    EXPECT_LE(AmplitudeToDb(aliasAmplitude), -30.0);
}

TEST(ResamplerGolden, Sinc16RatioSixteenFlushesAndExhausts) {
    constexpr std::size_t kInputFrames = 4096;
    constexpr double kRatio = 16.0;
    constexpr std::size_t kProbeOutput = 16;
    constexpr std::uint64_t kProbePosition =
        kResampleHistoryPad + kProbeOutput * 16;
    constexpr std::uint64_t kProbeHistoryDistance = 60;
    std::vector<float> input(kInputFrames, 0.0f);
    input[kProbePosition - kProbeHistoryDistance - kResampleHistoryPad] = 0.5f;
    HARKENTEST_ASSERT_NON_SILENT(input);
    const std::size_t expected =
        static_cast<std::size_t>(std::ceil(kInputFrames / kRatio));
    const BoundedRun result = RunResamplerBounded(
        ResampleQuality::Sinc16, kRatio, input, expected + 64);

    EXPECT_TRUE(result.exhausted)
        << "extreme Sinc16 support did not finish its silence flush";
    EXPECT_LE(result.out.size(), expected + 2);
    EXPECT_GE(result.out.size() + 2, expected);
    ASSERT_GT(result.out.size(), kProbeOutput);
    // Offset -60 is outside the scale-16 8-tap radius of 56, but inside the
    // 16-tap radius of 120. The probe therefore pins that Sinc16 does not
    // change prototype after ratio 8 while the same run exercises EOS flush.
    const float reference =
        ReferenceScaledSinc16AtInteger(input, kProbePosition, kRatio);
    ASSERT_GT(std::abs(reference), 1.0e-4f)
        << "Sinc16-only outer support probe is too weak";
    EXPECT_NEAR(reference, result.out[kProbeOutput], 1.0e-6f);
    for (const float sample : result.out)
        EXPECT_TRUE(std::isfinite(sample));
}

TEST(ResamplerGolden, OutputLengthAccountsForRatioThenExhausts) {
    const std::vector<float> sine =
        harkentest::MakeSine(300, 1000.0, 48000.0, 0.5f);
    HARKENTEST_ASSERT_NON_SILENT(sine);

    // Ask for far more output than the input can produce; the harness
    // stops when the stage reports exhausted.
    const std::vector<float> half =
        RunResampler(ResampleQuality::Hermite, 0.5, sine, 2000);
    EXPECT_NEAR(600.0, static_cast<double>(half.size()), 1.0);

    const std::vector<float> up =
        RunResampler(ResampleQuality::Hermite, 1.5, sine, 2000);
    EXPECT_NEAR(200.0, static_cast<double>(up.size()), 1.0);

    // Extreme upsample (1/64): the read head advances a fraction of a frame
    // per output, so it never overtakes the write head — a 128-frame source
    // yields 128 * 64 output frames and exhausts. Guards the lower ratio end.
    const std::vector<float> tiny =
        harkentest::MakeSine(128, 1000.0, 48000.0, 0.5f);
    HARKENTEST_ASSERT_NON_SILENT(tiny);
    const BoundedRun over =
        RunResamplerBounded(ResampleQuality::Hermite, 1.0 / 64.0, tiny, 8192 + 256);
    EXPECT_TRUE(over.exhausted);
    EXPECT_NEAR(8192.0, static_cast<double>(over.out.size()), 1.0);

    // Extreme decimation (64:1 divides the feed chunk, 60:1 does not). Both
    // must decimate the 300-frame source to ceil(N / ratio) frames and
    // exhaust. 60:1 drives the read head more than the history pad past the
    // write head — where the FreeFrames subtraction underflows — while 64:1
    // stays phase-locked to the write head; it is pinned here against future
    // regression.
    const BoundedRun down60 =
        RunResamplerBounded(ResampleQuality::Hermite, 60.0, sine, 32);
    EXPECT_TRUE(down60.exhausted);
    EXPECT_NEAR(5.0, static_cast<double>(down60.out.size()), 1.0);

    const BoundedRun down64 =
        RunResamplerBounded(ResampleQuality::Hermite, 64.0, sine, 32);
    EXPECT_TRUE(down64.exhausted);
    EXPECT_NEAR(5.0, static_cast<double>(down64.out.size()), 1.0);
}

TEST(ResamplerRatioRamp, ReachesTargetAfterExactProducedFrameCount) {
    Resampler resampler;
    std::vector<float> ring;
    PrimeRatioRampResampler(&resampler, &ring);

    resampler.UpdateRatio(2.0, 4);
    EXPECT_DOUBLE_EQ(1.0, resampler.ratio);
    EXPECT_DOUBLE_EQ(2.0, resampler.targetRatio);
    EXPECT_DOUBLE_EQ(0.25, resampler.rampStep);
    EXPECT_EQ(4u, resampler.rampRemaining);

    ProduceOne(&resampler);
    EXPECT_DOUBLE_EQ(1.25, resampler.ratio);
    EXPECT_DOUBLE_EQ(9.25, resampler.readPos);
    EXPECT_EQ(3u, resampler.rampRemaining);

    ProduceOne(&resampler);
    EXPECT_DOUBLE_EQ(1.5, resampler.ratio);
    EXPECT_EQ(2u, resampler.rampRemaining);
    ProduceOne(&resampler);
    EXPECT_DOUBLE_EQ(1.75, resampler.ratio);
    EXPECT_EQ(1u, resampler.rampRemaining);
    ProduceOne(&resampler);
    EXPECT_DOUBLE_EQ(2.0, resampler.ratio);
    EXPECT_EQ(0u, resampler.rampRemaining);
}

TEST(ResamplerRatioRamp, SameIdleTargetIsExactNoOp) {
    Resampler resampler;
    std::vector<float> ring;
    PrimeRatioRampResampler(&resampler, &ring);

    const double readPos = resampler.readPos;
    const std::uint64_t writeIndex = resampler.writeIndex;
    resampler.UpdateRatio(1.0, 480);

    EXPECT_DOUBLE_EQ(1.0, resampler.ratio);
    EXPECT_DOUBLE_EQ(1.0, resampler.targetRatio);
    EXPECT_DOUBLE_EQ(0.0, resampler.rampStep);
    EXPECT_EQ(0u, resampler.rampRemaining);
    EXPECT_DOUBLE_EQ(readPos, resampler.readPos);
    EXPECT_EQ(writeIndex, resampler.writeIndex);
}

TEST(ResamplerRatioRamp, ResetSnapsAndClearsRamp) {
    Resampler resampler;
    std::vector<float> ring;
    PrimeRatioRampResampler(&resampler, &ring);
    resampler.UpdateRatio(2.0, 4);
    ProduceOne(&resampler);

    float* rings[1] = {ring.data()};
    resampler.Reset(rings, 1, ResampleQuality::Linear, 0.75, Tables());

    EXPECT_DOUBLE_EQ(0.75, resampler.ratio);
    EXPECT_DOUBLE_EQ(0.75, resampler.targetRatio);
    EXPECT_DOUBLE_EQ(0.0, resampler.rampStep);
    EXPECT_EQ(0u, resampler.rampRemaining);
    EXPECT_DOUBLE_EQ(static_cast<double>(harken::kResampleHistoryPad),
                     resampler.readPos);
}

TEST(ResamplerRatioRamp, RetargetRestartsFromInstantaneousRatio) {
    Resampler resampler;
    std::vector<float> ring;
    PrimeRatioRampResampler(&resampler, &ring);
    resampler.UpdateRatio(2.0, 4);
    ProduceOne(&resampler);
    ASSERT_DOUBLE_EQ(1.25, resampler.ratio);

    resampler.UpdateRatio(0.25, 2);
    EXPECT_DOUBLE_EQ(0.25, resampler.targetRatio);
    EXPECT_DOUBLE_EQ(-0.5, resampler.rampStep);
    EXPECT_EQ(2u, resampler.rampRemaining);

    ProduceOne(&resampler);
    EXPECT_DOUBLE_EQ(0.75, resampler.ratio);
    EXPECT_EQ(1u, resampler.rampRemaining);
    ProduceOne(&resampler);
    EXPECT_DOUBLE_EQ(0.25, resampler.ratio);
    EXPECT_EQ(0u, resampler.rampRemaining);
}

} // namespace
