#include "harken/dsp/Resampler.h"
#include "harken/core/SourcePhase.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cmath>
#include <cstring>
#if !defined(HARKEN_RESAMPLER_FORCE_SCALAR) &&                         \
    (defined(_M_X64) || defined(__x86_64__) ||                        \
     (defined(_M_IX86) && defined(_M_IX86_FP) && _M_IX86_FP >= 2) || \
     defined(__SSE2__))
#define HARKEN_RESAMPLER_HAS_SSE2 1
#include <emmintrin.h>
#else
#define HARKEN_RESAMPLER_HAS_SSE2 0
#endif
#include <limits>

namespace harken {

namespace {

constexpr std::uint32_t kRingMask = kResampleFifoFrames - 1;
static_assert((kResampleFifoFrames & kRingMask) == 0,
              "ring capacity must be a power of two");

constexpr double kMaxDirectKernelScale = 16.0;
constexpr std::uint32_t kMaxSinc8Need = 57;  // ceil(3.5 * 16) + 1
constexpr std::uint32_t kMaxSinc16Need = 121; // ceil(7.5 * 16) + 1
constexpr std::uint32_t kMaxScaledKernelNeed =
    kMaxSinc8Need > kMaxSinc16Need ? kMaxSinc8Need : kMaxSinc16Need;
constexpr std::uint32_t kMaxScaledKernelCoefficients =
    2 * kMaxScaledKernelNeed + 1;
static_assert(kMaxScaledKernelCoefficients <= 256,
              "scaled resampler kernel must fit the bounded stack row");
static_assert(2 * kMaxSinc16Need + kResampleChunkFrames <
                  kResampleFifoFrames,
              "resampler FIFO must hold support plus one feed chunk");

constexpr double kPi = 3.14159265358979323846;
constexpr double kHighRatioCutoffFraction = 0.9;
constexpr double kButterworthQ[kResampleHighRatioFilterSections] = {
    0.5024192861881557, 0.5224986149396889, 0.5669440348163577,
    0.6468217833599901, 0.7881546234512502, 1.0606776859903475,
    1.7224470982383342, 5.1011486186891553,
};

struct HighRatioSectionCoefficients {
    double b0 = 0.0;
    double b1 = 0.0;
    double b2 = 0.0;
    double a1 = 0.0;
    double a2 = 0.0;
};

// The high-ratio path starts above 16, so x <= 0.45*pi/16. The x^7 tan
// expansion has less than 2e-11 absolute error over that interval and avoids
// trigonometry on the audio thread.
double TanSmall(double x) {
    const double x2 = x * x;
    return x * (1.0 + x2 * (1.0 / 3.0 +
                             x2 * (2.0 / 15.0 + x2 * (17.0 / 315.0))));
}

void BuildHighRatioCoefficients(
    double ratio,
    HighRatioSectionCoefficients
        (&coefficients)[kResampleHighRatioFilterSections]) {
    // Put the 16th-order Butterworth corner at 90% of output Nyquist. This
    // leaves a transition band while reaching more than 50 dB rejection by
    // 1.3 times output Nyquist, matching the direct sinc path's stopband edge.
    const double x =
        (0.5 * kHighRatioCutoffFraction * kPi) / ratio;
    const double k = TanSmall(x);
    const double k2 = k * k;
    for (std::uint32_t section = 0;
         section < kResampleHighRatioFilterSections; ++section) {
        const double inverseQ = 1.0 / kButterworthQ[section];
        const double norm = 1.0 / (1.0 + k * inverseQ + k2);
        HighRatioSectionCoefficients& c = coefficients[section];
        c.b0 = k2 * norm;
        c.b1 = 2.0 * c.b0;
        c.b2 = c.b0;
        c.a1 = 2.0 * (k2 - 1.0) * norm;
        c.a2 = (1.0 - k * inverseQ + k2) * norm;
    }
}

struct DownsampleGeometry {
    double scale = 1.0;
    std::uint32_t need = 0;
    bool useSinc16 = false;
};

std::uint32_t CeilPositive(double value) {
    const std::uint32_t truncated = static_cast<std::uint32_t>(value);
    return truncated + (value > static_cast<double>(truncated) ? 1u : 0u);
}

DownsampleGeometry ResolveDownsampleGeometry(ResampleQuality quality,
                                             double ratio) {
    DownsampleGeometry geometry;
    geometry.useSinc16 = quality == ResampleQuality::Sinc16;
    // This direct path is used only through scale 16. Larger ratios take the
    // bounded streaming filter below instead of silently freezing the cutoff.
    geometry.scale = ratio;
    const double activeHalf = geometry.useSinc16 ? 7.5 : 3.5;
    geometry.need = CeilPositive(activeHalf * geometry.scale) + 1;
    return geometry;
}

std::uint32_t HistoryReserve(ResampleQuality quality) {
    return quality == ResampleQuality::Sinc16 ? kMaxSinc16Need
                                               : kMaxSinc8Need;
}

void SeedHighRatioFilter(
    Resampler& resampler, std::uint64_t index,
    const HighRatioSectionCoefficients
        (&coefficients)[kResampleHighRatioFilterSections]) {
    for (std::uint32_t ch = 0; ch < resampler.channelCount; ++ch) {
        const double seed =
            static_cast<double>(resampler.ring[ch][index & kRingMask]);
        resampler.highRatioPrevious[ch] = seed;
        resampler.highRatioCurrent[ch] = seed;
        for (std::uint32_t section = 0;
             section < kResampleHighRatioFilterSections; ++section) {
            const HighRatioSectionCoefficients& c = coefficients[section];
            ResampleFilterSectionState& state =
                resampler.highRatioFilter[ch][section];
            // Steady-state transposed-direct-form state for a constant seed.
            // Starting from the actual render sample avoids a zero-state click
            // when a ramp first crosses into the streaming-filter domain.
            state.z1 = (1.0 - c.b0) * seed;
            state.z2 = (c.b2 - c.a2) * seed;
        }
    }
    resampler.highRatioFilterIndex = index + 1;
    resampler.highRatioFilterActive = true;
}

void ProcessHighRatioInput(
    Resampler& resampler, std::uint64_t index,
    const HighRatioSectionCoefficients
        (&coefficients)[kResampleHighRatioFilterSections]) {
    for (std::uint32_t ch = 0; ch < resampler.channelCount; ++ch) {
        double value =
            static_cast<double>(resampler.ring[ch][index & kRingMask]);
        for (std::uint32_t section = 0;
             section < kResampleHighRatioFilterSections; ++section) {
            const HighRatioSectionCoefficients& c = coefficients[section];
            ResampleFilterSectionState& state =
                resampler.highRatioFilter[ch][section];
            const double filtered = c.b0 * value + state.z1;
            state.z1 = c.b1 * value - c.a1 * filtered + state.z2;
            state.z2 = c.b2 * value - c.a2 * filtered;
            value = filtered;
        }
        resampler.highRatioPrevious[ch] = resampler.highRatioCurrent[ch];
        resampler.highRatioCurrent[ch] = value;
    }
}

bool PrepareHighRatioSample(
    Resampler& resampler, std::uint64_t index,
    const HighRatioSectionCoefficients
        (&coefficients)[kResampleHighRatioFilterSections]) {
    if (!resampler.highRatioFilterActive) {
        const std::uint32_t history = HistoryReserve(resampler.quality);
        const std::uint64_t start = index > history ? index - history : 0;
        if (start >= resampler.writeIndex)
            return false;
        SeedHighRatioFilter(resampler, start, coefficients);
    }

    // One filtered look-ahead sample supports the same fractional read position
    // as the direct interpolators. Filtering advances incrementally across
    // starvation/refill calls, so the FIFO never has to hold the full ratio-wide
    // history at once.
    const std::uint64_t target = index + 1;
    while (resampler.highRatioFilterIndex <= target &&
           resampler.highRatioFilterIndex < resampler.writeIndex) {
        ProcessHighRatioInput(resampler, resampler.highRatioFilterIndex,
                              coefficients);
        ++resampler.highRatioFilterIndex;
    }
    return resampler.highRatioFilterIndex > target;
}

template <int NumTaps>
void BuildScaledCoefficients(const KaiserSincTable<NumTaps>& table,
                             float frac, const DownsampleGeometry& geometry,
                             float* coefficients) {
    const std::uint32_t count = 2 * geometry.need + 1;
    double sum = 0.0;
    for (std::uint32_t i = 0; i < count; ++i) {
        const int offset = static_cast<int>(i) -
                           static_cast<int>(geometry.need);
        const float coordinate = static_cast<float>(
            (static_cast<double>(offset) - static_cast<double>(frac)) /
            geometry.scale);
        const float coefficient = SampleKaiserSincPrototype(table, coordinate);
        coefficients[i] = coefficient;
        sum += static_cast<double>(coefficient);
    }

    const float inverseSum = static_cast<float>(1.0 / sum);
    for (std::uint32_t i = 0; i < count; ++i)
        coefficients[i] *= inverseSum;
}

float ApplyScaledKernel(const float* ring, std::uint64_t index,
                        std::uint32_t need, const float* coefficients) {
    const std::uint32_t count = 2 * need + 1;
    float value = 0.0f;
    for (std::uint32_t i = 0; i < count; ++i) {
        const int offset = static_cast<int>(i) - static_cast<int>(need);
        std::uint64_t sampleIndex = index;
        if (offset < 0) {
            const std::uint64_t distance =
                static_cast<std::uint64_t>(-offset);
            if (distance > index)
                continue; // zero-filled virtual history before absolute zero
            sampleIndex -= distance;
        } else {
            sampleIndex += static_cast<std::uint64_t>(offset);
        }
        value += coefficients[i] * ring[sampleIndex & kRingMask];
    }
    return value;
}

// Frames the kernel reads ahead of the exact integer interpolation center:
// producing at position p requires writeIndex > p + ahead.
std::uint32_t AheadFrames(ResampleQuality q) {
    switch (q) {
        case ResampleQuality::Linear: return 1;
        case ResampleQuality::Hermite: return 2;
        case ResampleQuality::Sinc8: return 4;
        case ResampleQuality::Sinc16: return 8;
    }
    return 8;
}

constexpr std::uint32_t kCandidateRawRingMask =
    kResampleFifoFrames - 1;
constexpr std::uint32_t kCandidateFinalRingMask =
    kMultirateFinalRingFrames - 1;
constexpr std::uint32_t kCandidateFinalHistory =
    kMultirateResidualTaps / 2;
constexpr std::uint32_t kCandidateOverlapFrames =
    kMultirateTransitionOverlapFrames;
// Eleven symmetric side pairs plus the delayed center sample.
constexpr std::uint32_t kCandidateHalfbandMacsPerChannel = 12;
constexpr double kProfessionalMinimumSafeRatio =
    (44100.0 / 48000.0) / 64.0;
constexpr double kProfessionalMaximumSafeRatio =
    kMultirateMaximumEffectiveRatio;
constexpr std::uint64_t kCandidateMaximumTransitionLookahead =
    static_cast<std::uint64_t>(kMultirateFinalRingFrames) *
        (std::uint64_t{1} << kMultirateMaxStages) +
    static_cast<std::uint64_t>(
        kMultirateHalfbandTaps / 2u) *
        ((std::uint64_t{1} << kMultirateMaxStages) - 1u) +
    ((std::uint64_t{1} << kMultirateMaxStages) - 1u);
static_assert((kMultirateFinalRingFrames & kCandidateFinalRingMask) == 0,
              "candidate final ring capacity must be a power of two");

struct CandidatePathCoordinate {
    std::uint64_t index = 0;
    double fraction = 0.0;
    bool valid = false;
};

double CandidatePathScale(const MultirateCandidatePathState& path) {
    return static_cast<double>(std::uint64_t{1} << path.stageCount);
}

CandidatePathCoordinate CandidatePathPosition(
    const MultirateCandidatePathState& path,
    std::uint64_t sourceFrame, double sourceFraction) {
    std::uint64_t distance = 0;
    if (path.sourceOrigin >= 0) {
        const std::uint64_t origin =
            static_cast<std::uint64_t>(path.sourceOrigin);
        if (sourceFrame < origin)
            return {};
        distance = sourceFrame - origin;
    } else {
        const std::uint64_t originMagnitude =
            static_cast<std::uint64_t>(-(path.sourceOrigin + 1)) + 1u;
        if (sourceFrame > UINT64_MAX - originMagnitude)
            return {};
        distance = sourceFrame + originMagnitude;
    }

    const std::uint64_t scale =
        std::uint64_t{1} << path.stageCount;
    CandidatePathCoordinate coordinate;
    coordinate.index = distance / scale;
    coordinate.fraction =
        (static_cast<double>(distance % scale) + sourceFraction) /
        static_cast<double>(scale);
    coordinate.valid = true;
    return coordinate;
}

std::uint32_t CandidateFinalFreeFrames(
    const MultirateResamplerCandidate& candidate,
    const MultirateCandidatePathState& path) {
    const std::uint64_t retainSource =
        candidate.sourceFrame < path.validSourceStart
            ? path.validSourceStart
            : candidate.sourceFrame;
    const CandidatePathCoordinate retainPosition =
        CandidatePathPosition(path, retainSource, 0.0);
    const std::uint64_t retainIndex =
        retainPosition.valid ? retainPosition.index : 0;
    const std::uint64_t oldestNeeded =
        retainIndex > kCandidateFinalHistory
            ? retainIndex - kCandidateFinalHistory
            : 0;
    const std::int64_t occupied =
        static_cast<std::int64_t>(path.finalWriteIndex) -
        static_cast<std::int64_t>(oldestNeeded);
    if (occupied <= 0)
        return kMultirateFinalRingFrames;
    return occupied >=
                   static_cast<std::int64_t>(
                       kMultirateFinalRingFrames)
               ? 0
               : kMultirateFinalRingFrames -
                     static_cast<std::uint32_t>(occupied);
}

void PushCandidateFinal(MultirateResamplerCandidate& candidate,
                        MultirateCandidatePathState& path,
                        const float* samples) {
    const std::uint32_t slot =
        static_cast<std::uint32_t>(path.finalWriteIndex) &
        kCandidateFinalRingMask;
    for (std::uint32_t ch = 0; ch < candidate.channelCount; ++ch)
        path.finalRing[ch][slot] = samples[ch];
    ++path.finalWriteIndex;
}

void InitializeCandidatePath(
    MultirateResamplerCandidate& candidate,
    MultirateCandidatePathState& path,
    std::uint32_t stageCount, std::uint64_t rawStart,
    bool allowZeroHistory) {
    for (MultirateHalfbandStageState& stage : path.stages) {
        stage.sideWriteOffset = 0;
        stage.centerWriteOffset = 0;
        stage.inputFrames = 0;
    }
    for (std::uint32_t ch = 0; ch < candidate.channelCount; ++ch) {
        for (std::uint32_t frame = 0;
             frame < kCandidateFinalHistory; ++frame) {
            path.finalRing[ch][frame] = 0.0f;
        }
    }
    path.stageCount = stageCount;
    path.rawProcessIndex = rawStart;
    path.finalWriteIndex = kCandidateFinalHistory;
    const std::uint64_t scale =
        std::uint64_t{1} << stageCount;
    const std::uint64_t netDelay =
        static_cast<std::uint64_t>(
            kMultirateHalfbandTaps / 2u - 1u) *
        (scale - 1u);
    const std::uint64_t groupDelay =
        static_cast<std::uint64_t>(
            kMultirateHalfbandTaps / 2u) *
        (scale - 1u);
    const std::uint64_t originOffset =
        netDelay +
        static_cast<std::uint64_t>(kCandidateFinalHistory) * scale;
    path.sourceOrigin =
        rawStart >= originOffset
            ? static_cast<std::int64_t>(rawStart - originOffset)
            : -static_cast<std::int64_t>(originOffset - rawStart);
    path.validSourceStart =
        allowZeroHistory
            ? candidate.sourceFrame
            : rawStart + groupDelay;
    path.initialized = true;
}

template <bool Instrumented>
bool ProcessCandidateRawFrameLegacy(
    MultirateResamplerCandidate& candidate,
    MultirateCandidatePathState& path) {
    if (path.rawProcessIndex >= candidate.rawWriteIndex ||
        CandidateFinalFreeFrames(candidate, path) == 0) {
        return false;
    }

    float stageSamples[kMaxChannels] = {};
    for (std::uint32_t ch = 0; ch < candidate.channelCount; ++ch) {
        stageSamples[ch] =
            candidate.rawRing[ch][path.rawProcessIndex &
                                  kCandidateRawRingMask];
    }
    ++path.rawProcessIndex;
    if constexpr (Instrumented)
        ++candidate.counters.rawFramesRead;

    bool outputAvailable = true;
    for (std::uint32_t stageIndex = 0;
         stageIndex < path.stageCount && outputAvailable;
         ++stageIndex) {
        MultirateHalfbandStageState& stage =
            path.stages[stageIndex];
        const bool evenInputPhase =
            (stage.inputFrames & 1u) == 0;
        if (evenInputPhase) {
            for (std::uint32_t ch = 0;
                 ch < candidate.channelCount; ++ch) {
                stage.centerHistory[ch][stage.centerWriteOffset] =
                    stageSamples[ch];
            }
            stage.centerWriteOffset =
                (stage.centerWriteOffset + 1u) %
                kMultirateHalfbandCenterFrames;
            ++stage.inputFrames;
            outputAvailable = false;
            break;
        }

        for (std::uint32_t ch = 0;
             ch < candidate.channelCount; ++ch) {
            stage.sideHistory[ch][stage.sideWriteOffset] =
                stageSamples[ch];
        }
        stage.sideWriteOffset =
            (stage.sideWriteOffset + 1u) %
            kMultirateHalfbandSideFrames;
        ++stage.inputFrames;
        const std::uint32_t latestSide =
            (stage.sideWriteOffset +
             kMultirateHalfbandSideFrames - 1u) %
            kMultirateHalfbandSideFrames;
        const std::uint32_t latestCenter =
            (stage.centerWriteOffset +
             kMultirateHalfbandCenterFrames - 1u) %
            kMultirateHalfbandCenterFrames;
        const std::uint32_t sideFramesWritten =
            stage.inputFrames / 2u;
        const std::uint32_t centerFramesWritten =
            (stage.inputFrames + 1u) / 2u;
        for (std::uint32_t ch = 0; ch < candidate.channelCount; ++ch) {
            float sum = 0.0f;
            for (std::uint32_t tap = 0;
                 tap < kMultirateHalfbandTaps / 2u;
                 tap += 2u) {
                const float coefficient = candidate.tables->halfband[tap];
                if (coefficient == 0.0f)
                    continue;
                const std::uint32_t mirror =
                    kMultirateHalfbandTaps - 1u - tap;
                const std::uint32_t firstIndex =
                    (latestSide + kMultirateHalfbandSideFrames -
                     tap / 2u) %
                    kMultirateHalfbandSideFrames;
                const std::uint32_t secondIndex =
                    (latestSide + kMultirateHalfbandSideFrames -
                    mirror / 2u) %
                    kMultirateHalfbandSideFrames;
                const std::uint32_t firstDistance = tap / 2u;
                const std::uint32_t secondDistance = mirror / 2u;
                const float firstSample =
                    firstDistance < sideFramesWritten
                        ? stage.sideHistory[ch][firstIndex]
                        : 0.0f;
                const float secondSample =
                    secondDistance < sideFramesWritten
                        ? stage.sideHistory[ch][secondIndex]
                        : 0.0f;
                sum += coefficient * (firstSample + secondSample);
            }
            constexpr std::uint32_t kCenterDistance =
                (kMultirateHalfbandTaps / 2u - 1u) / 2u;
            const std::uint32_t centerIndex =
                (latestCenter +
                 kMultirateHalfbandCenterFrames -
                 kCenterDistance) %
                kMultirateHalfbandCenterFrames;
            sum += candidate.tables
                       ->halfband[kMultirateHalfbandTaps / 2u] *
                   (kCenterDistance < centerFramesWritten
                        ? stage.centerHistory[ch][centerIndex]
                        : 0.0f);
            stageSamples[ch] = sum;
        }
        if constexpr (Instrumented) {
            ++candidate.counters.halfbandOutputFrames;
            candidate.counters.halfbandMultiplyAccumulates +=
                static_cast<std::uint64_t>(candidate.channelCount) *
                kCandidateHalfbandMacsPerChannel;
        }
    }

    if (outputAvailable)
        PushCandidateFinal(candidate, path, stageSamples);
    return true;
}

template <bool Instrumented>
bool EnsureCandidateLookaheadLegacy(
    MultirateResamplerCandidate& candidate,
    MultirateCandidatePathState& path,
    std::uint64_t sourceFrame, double sourceFraction) {
    const CandidatePathCoordinate finalPosition =
        CandidatePathPosition(path, sourceFrame, sourceFraction);
    if (!finalPosition.valid)
        return false;
    const std::uint64_t required =
        finalPosition.index +
        kCandidateFinalHistory;
    while (path.finalWriteIndex <= required) {
        if (!ProcessCandidateRawFrameLegacy<Instrumented>(candidate, path))
            return false;
    }
    return true;
}

template <bool Instrumented>
std::uint32_t ProcessHalfbandStageBurst(
    MultirateResamplerCandidate& candidate,
    MultirateHalfbandStageState& stage,
    const float (*input)[kMaxChannels], std::uint32_t inputCount,
    std::uint32_t channelCount,
    float (*output)[kMaxChannels]) {
    std::uint32_t outputCount = 0;
    std::uint32_t timeGroupCount = 0;
    float timeSamples[kMaxChannels]
                    [kMultirateHalfbandSideTapCount][4] = {};
    float timeCenters[kMaxChannels][4] = {};
    std::uint32_t timeOutputStart = 0;

    const auto finishTimeGroup = [&]() {
        if (timeGroupCount == 0)
            return;
        for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
#if HARKEN_RESAMPLER_HAS_SSE2
            if (timeGroupCount == 4u) {
                __m128 acc = _mm_setzero_ps();
                for (std::uint32_t tap = 0;
                     tap < kMultirateHalfbandSideTapCount; ++tap) {
                    const __m128 samples = _mm_set_ps(
                        timeSamples[ch][tap][3],
                        timeSamples[ch][tap][2],
                        timeSamples[ch][tap][1],
                        timeSamples[ch][tap][0]);
                    acc = _mm_add_ps(
                        acc,
                        _mm_mul_ps(
                            _mm_set1_ps(candidate.tables->halfbandSide[tap]
                                            .coefficient),
                            samples));
                }
                const __m128 centers = _mm_set_ps(
                    timeCenters[ch][3], timeCenters[ch][2],
                    timeCenters[ch][1], timeCenters[ch][0]);
                acc = _mm_add_ps(
                    acc,
                    _mm_mul_ps(
                        _mm_set1_ps(candidate.tables->halfband[
                            kMultirateHalfbandTaps / 2u]),
                        centers));
                alignas(16) float lane[4];
                _mm_store_ps(lane, acc);
                for (std::uint32_t group = 0; group < 4u; ++group)
                    output[timeOutputStart + group][ch] = lane[group];
            } else
#endif
            {
                for (std::uint32_t group = 0;
                     group < timeGroupCount; ++group) {
                    float sum = 0.0f;
                    for (std::uint32_t tap = 0;
                         tap < kMultirateHalfbandSideTapCount; ++tap) {
                        sum += candidate.tables->halfbandSide[tap]
                                   .coefficient *
                               timeSamples[ch][tap][group];
                    }
                    sum += candidate.tables->halfband[
                               kMultirateHalfbandTaps / 2u] *
                           timeCenters[ch][group];
                    output[timeOutputStart + group][ch] = sum;
                }
            }
        }
        timeGroupCount = 0;
    };

    for (std::uint32_t inputIndex = 0;
         inputIndex < inputCount; ++inputIndex) {
        const bool evenInputPhase = (stage.inputFrames & 1u) == 0;
        if (evenInputPhase) {
            for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
                stage.centerHistory[ch][stage.centerWriteOffset] =
                    input[inputIndex][ch];
            }
            stage.centerWriteOffset =
                (stage.centerWriteOffset + 1u) %
                kMultirateHalfbandCenterFrames;
            ++stage.inputFrames;
            continue;
        }

        for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
            stage.sideHistory[ch][stage.sideWriteOffset] =
                input[inputIndex][ch];
        }
        stage.sideWriteOffset =
            (stage.sideWriteOffset + 1u) % kMultirateHalfbandSideFrames;
        ++stage.inputFrames;
        const std::uint32_t latestSide =
            (stage.sideWriteOffset + kMultirateHalfbandSideFrames - 1u) %
            kMultirateHalfbandSideFrames;
        const std::uint32_t latestCenter =
            (stage.centerWriteOffset + kMultirateHalfbandCenterFrames - 1u) %
            kMultirateHalfbandCenterFrames;
        const std::uint32_t sideFramesWritten = stage.inputFrames / 2u;
        const std::uint32_t centerFramesWritten =
            (stage.inputFrames + 1u) / 2u;
        constexpr std::uint32_t kCenterDistance =
            (kMultirateHalfbandTaps / 2u - 1u) / 2u;
        const std::uint32_t centerIndex =
            (latestCenter + kMultirateHalfbandCenterFrames -
             kCenterDistance) % kMultirateHalfbandCenterFrames;

        if (channelCount < 4u) {
            if (timeGroupCount == 0)
                timeOutputStart = outputCount;
            for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
                for (std::uint32_t tap = 0;
                     tap < kMultirateHalfbandSideTapCount; ++tap) {
                    const MultirateHalfbandSideTap& sideTap =
                        candidate.tables->halfbandSide[tap];
                    const std::uint32_t firstIndex =
                        (latestSide + kMultirateHalfbandSideFrames -
                         sideTap.firstDistance) %
                        kMultirateHalfbandSideFrames;
                    const std::uint32_t secondIndex =
                        (latestSide + kMultirateHalfbandSideFrames -
                         sideTap.secondDistance) %
                        kMultirateHalfbandSideFrames;
                    const float firstSample =
                        sideTap.firstDistance < sideFramesWritten
                            ? stage.sideHistory[ch][firstIndex]
                            : 0.0f;
                    const float secondSample =
                        sideTap.secondDistance < sideFramesWritten
                            ? stage.sideHistory[ch][secondIndex]
                            : 0.0f;
                    timeSamples[ch][tap][timeGroupCount] =
                        firstSample + secondSample;
                }
                timeCenters[ch][timeGroupCount] =
                    (kCenterDistance < centerFramesWritten
                         ? stage.centerHistory[ch][centerIndex]
                         : 0.0f);
            }
            ++timeGroupCount;
            ++outputCount;
            if (timeGroupCount == 4u)
                finishTimeGroup();
        } else {
#if HARKEN_RESAMPLER_HAS_SSE2
            for (std::uint32_t ch = 0; ch + 4u <= channelCount;
                 ch += 4u) {
                __m128 acc = _mm_setzero_ps();
                for (std::uint32_t tap = 0;
                     tap < kMultirateHalfbandSideTapCount; ++tap) {
                    const MultirateHalfbandSideTap& sideTap =
                        candidate.tables->halfbandSide[tap];
                    const std::uint32_t firstIndex =
                        (latestSide + kMultirateHalfbandSideFrames -
                         sideTap.firstDistance) %
                        kMultirateHalfbandSideFrames;
                    const std::uint32_t secondIndex =
                        (latestSide + kMultirateHalfbandSideFrames -
                         sideTap.secondDistance) %
                        kMultirateHalfbandSideFrames;
                    const __m128 firstSamples = _mm_set_ps(
                        sideTap.firstDistance < sideFramesWritten
                            ? stage.sideHistory[ch + 3u][firstIndex]
                            : 0.0f,
                        sideTap.firstDistance < sideFramesWritten
                            ? stage.sideHistory[ch + 2u][firstIndex]
                            : 0.0f,
                        sideTap.firstDistance < sideFramesWritten
                            ? stage.sideHistory[ch + 1u][firstIndex]
                            : 0.0f,
                        sideTap.firstDistance < sideFramesWritten
                            ? stage.sideHistory[ch + 0u][firstIndex]
                            : 0.0f);
                    const __m128 secondSamples = _mm_set_ps(
                        sideTap.secondDistance < sideFramesWritten
                            ? stage.sideHistory[ch + 3u][secondIndex]
                            : 0.0f,
                        sideTap.secondDistance < sideFramesWritten
                            ? stage.sideHistory[ch + 2u][secondIndex]
                            : 0.0f,
                        sideTap.secondDistance < sideFramesWritten
                            ? stage.sideHistory[ch + 1u][secondIndex]
                            : 0.0f,
                        sideTap.secondDistance < sideFramesWritten
                            ? stage.sideHistory[ch + 0u][secondIndex]
                            : 0.0f);
                    acc = _mm_add_ps(
                        acc,
                        _mm_mul_ps(
                            _mm_set1_ps(sideTap.coefficient),
                            _mm_add_ps(firstSamples, secondSamples)));
                }
                const __m128 centerSamples = _mm_set_ps(
                    kCenterDistance < centerFramesWritten
                        ? stage.centerHistory[ch + 3u][centerIndex]
                        : 0.0f,
                    kCenterDistance < centerFramesWritten
                        ? stage.centerHistory[ch + 2u][centerIndex]
                        : 0.0f,
                    kCenterDistance < centerFramesWritten
                        ? stage.centerHistory[ch + 1u][centerIndex]
                        : 0.0f,
                    kCenterDistance < centerFramesWritten
                        ? stage.centerHistory[ch + 0u][centerIndex]
                        : 0.0f);
                acc = _mm_add_ps(
                    acc,
                    _mm_mul_ps(
                        _mm_set1_ps(candidate.tables->halfband[
                            kMultirateHalfbandTaps / 2u]),
                        centerSamples));
                alignas(16) float lane[4];
                _mm_store_ps(lane, acc);
                for (std::uint32_t laneIndex = 0; laneIndex < 4u;
                     ++laneIndex)
                    output[outputCount][ch + laneIndex] =
                        lane[laneIndex];
            }
#endif
            const std::uint32_t scalarStart =
#if HARKEN_RESAMPLER_HAS_SSE2
                (channelCount / 4u) * 4u;
#else
                0u;
#endif
            for (std::uint32_t ch = scalarStart;
                 ch < channelCount; ++ch) {
                float sum = 0.0f;
                for (std::uint32_t tap = 0;
                     tap < kMultirateHalfbandSideTapCount; ++tap) {
                    const MultirateHalfbandSideTap& sideTap =
                        candidate.tables->halfbandSide[tap];
                    const std::uint32_t firstIndex =
                        (latestSide + kMultirateHalfbandSideFrames -
                         sideTap.firstDistance) %
                        kMultirateHalfbandSideFrames;
                    const std::uint32_t secondIndex =
                        (latestSide + kMultirateHalfbandSideFrames -
                         sideTap.secondDistance) %
                        kMultirateHalfbandSideFrames;
                    const float firstSample =
                        sideTap.firstDistance < sideFramesWritten
                            ? stage.sideHistory[ch][firstIndex]
                            : 0.0f;
                    const float secondSample =
                        sideTap.secondDistance < sideFramesWritten
                            ? stage.sideHistory[ch][secondIndex]
                            : 0.0f;
                    sum += sideTap.coefficient *
                           (firstSample + secondSample);
                }
                sum += candidate.tables->halfband[
                           kMultirateHalfbandTaps / 2u] *
                       (kCenterDistance < centerFramesWritten
                            ? stage.centerHistory[ch][centerIndex]
                            : 0.0f);
                output[outputCount][ch] = sum;
            }
            ++outputCount;
        }
    }
    if (channelCount < 4u)
        finishTimeGroup();
    return outputCount;
}

template <bool Instrumented>
bool ProcessCandidateRawBurst8(
    MultirateResamplerCandidate& candidate,
    MultirateCandidatePathState& path,
    std::uint32_t logicalRawFrames) {
    if (path.rawProcessIndex + 8u > candidate.rawWriteIndex ||
        CandidateFinalFreeFrames(candidate, path) == 0)
        return false;

    float stageInput[8][kMaxChannels] = {};
    for (std::uint32_t inputIndex = 0; inputIndex < 8u; ++inputIndex) {
        for (std::uint32_t ch = 0; ch < candidate.channelCount; ++ch) {
            stageInput[inputIndex][ch] = candidate.rawRing[ch][
                (path.rawProcessIndex + inputIndex) &
                kCandidateRawRingMask];
        }
    }
    path.rawProcessIndex += 8u;
    if constexpr (Instrumented)
        candidate.counters.rawFramesRead += logicalRawFrames;

    std::uint32_t logicalInputCount = logicalRawFrames;
    std::uint32_t logicalHalfbandOutputs = 0;
    for (std::uint32_t stageIndex = 0;
         stageIndex < path.stageCount && logicalInputCount != 0;
         ++stageIndex) {
        const std::uint32_t logicalOutputCount =
            (logicalInputCount +
             ((path.stages[stageIndex].inputFrames & 1u) != 0u ? 1u : 0u)) /
            2u;
        logicalHalfbandOutputs += logicalOutputCount;
        logicalInputCount = logicalOutputCount;
    }
    if constexpr (Instrumented) {
        candidate.counters.halfbandOutputFrames +=
            logicalHalfbandOutputs;
        candidate.counters.halfbandMultiplyAccumulates +=
            static_cast<std::uint64_t>(logicalHalfbandOutputs) *
            candidate.channelCount * kCandidateHalfbandMacsPerChannel;
    }

    std::uint32_t stageCount = 8u;
    for (std::uint32_t stageIndex = 0;
         stageIndex < path.stageCount && stageCount != 0;
         ++stageIndex) {
        float stageOutput[8][kMaxChannels] = {};
        stageCount = ProcessHalfbandStageBurst<Instrumented>(
            candidate, path.stages[stageIndex], stageInput, stageCount,
            candidate.channelCount, stageOutput);
        for (std::uint32_t frame = 0; frame < stageCount; ++frame) {
            for (std::uint32_t ch = 0; ch < candidate.channelCount; ++ch)
                stageInput[frame][ch] = stageOutput[frame][ch];
        }
    }
    for (std::uint32_t frame = 0; frame < stageCount; ++frame)
        PushCandidateFinal(candidate, path, stageInput[frame]);
    return true;
}

template <bool Instrumented>
bool EnsureCandidateLookahead(
    MultirateResamplerCandidate& candidate,
    MultirateCandidatePathState& path,
    std::uint64_t sourceFrame, double sourceFraction) {
    const CandidatePathCoordinate finalPosition =
        CandidatePathPosition(path, sourceFrame, sourceFraction);
    if (!finalPosition.valid)
        return false;
    const std::uint64_t required =
        finalPosition.index + kCandidateFinalHistory;
    while (path.finalWriteIndex <= required) {
        const std::uint64_t rawRemaining =
            candidate.rawWriteIndex - path.rawProcessIndex;
        const std::uint32_t burstScale =
            std::uint32_t{1} << path.stageCount;
        const std::uint32_t burstOutputs =
            (8u + burstScale - 1u) / burstScale;
        if (rawRemaining >= 8u &&
            CandidateFinalFreeFrames(candidate, path) >= burstOutputs) {
            const std::uint64_t outputsNeeded =
                required + 1u - path.finalWriteIndex;
            std::uint32_t logicalRawFrames =
                static_cast<std::uint32_t>(outputsNeeded);
            for (std::uint32_t stageIndex = 0;
                 stageIndex < path.stageCount && logicalRawFrames != 0;
                 ++stageIndex) {
                const bool startsOdd =
                    (path.stages[stageIndex].inputFrames & 1u) != 0u;
                logicalRawFrames =
                    logicalRawFrames * 2u - (startsOdd ? 1u : 0u);
            }
            if (logicalRawFrames > 8u)
                logicalRawFrames = 8u;
            if (!ProcessCandidateRawBurst8<Instrumented>(
                    candidate, path, logicalRawFrames))
                return false;
            continue;
        }
        if (!ProcessCandidateRawFrameLegacy<Instrumented>(candidate, path)) {
            return false;
        }
    }
    return true;
}

bool CandidatePathReady(
    const MultirateCandidatePathState& path,
    std::uint64_t sourceFrame, double sourceFraction) {
    if (!path.initialized ||
        sourceFrame < path.validSourceStart) {
        return false;
    }
    const CandidatePathCoordinate finalPosition =
        CandidatePathPosition(path, sourceFrame, sourceFraction);
    if (!finalPosition.valid)
        return false;
    const std::uint64_t required =
        finalPosition.index +
        kCandidateFinalHistory;
    return path.finalWriteIndex > required;
}

template <bool Instrumented>
void StartCandidateTransition(
    MultirateResamplerCandidate& candidate,
    std::uint32_t desiredStages) {
    MultirateCandidatePathState& active =
        candidate.paths[candidate.activePath];
    MultirateCandidatePathState& transition =
        candidate.paths[candidate.transitionPath];
    const std::uint32_t adjacentStages =
        desiredStages > active.stageCount
            ? active.stageCount + 1u
            : active.stageCount - 1u;
    std::uint64_t canonicalSource = candidate.sourceFrame;
    if (candidate.sourceFraction > 0.0 &&
        canonicalSource != UINT64_MAX) {
        ++canonicalSource;
    }
    const std::uint32_t maximumStages =
        active.stageCount > adjacentStages
            ? active.stageCount
            : adjacentStages;
    const std::uint64_t futureLead =
        static_cast<std::uint64_t>(
            kMultirateFinalRingFrames) *
        (std::uint64_t{1} << maximumStages);
    std::uint64_t rawStart =
        canonicalSource <= UINT64_MAX - futureLead
            ? canonicalSource + futureLead
            : UINT64_MAX;
    const std::uint64_t targetScale =
        std::uint64_t{1} << adjacentStages;
    if (rawStart != UINT64_MAX &&
        rawStart >= kResampleHistoryPad) {
        const std::uint64_t phaseOffset =
            (rawStart - kResampleHistoryPad) &
            (targetScale - 1u);
        const std::uint64_t alignment =
            (targetScale - phaseOffset) &
            (targetScale - 1u);
        rawStart =
            rawStart <= UINT64_MAX - alignment
                ? rawStart + alignment
                : UINT64_MAX;
    }
    InitializeCandidatePath(candidate, transition, adjacentStages,
                            rawStart, false);
    candidate.transitionOverlapSource =
        transition.validSourceStart;
    candidate.transitionTargetStages = adjacentStages;
    candidate.transitionPreparing = true;
    candidate.transitionOverlapping = false;
    candidate.overlapFrame = 0;
    if constexpr (Instrumented) {
        ++candidate.counters.stageTransitions;
        if (adjacentStages >
            candidate.counters.maximumActiveStages) {
            candidate.counters.maximumActiveStages =
                adjacentStages;
        }
    }
}

void DeactivateCandidatePath(MultirateCandidatePathState& path) {
    path.rawProcessIndex = 0;
    path.finalWriteIndex = 0;
    path.sourceOrigin = 0;
    path.validSourceStart = 0;
    path.stageCount = 0;
    path.initialized = false;
}

void CancelCandidateTransition(
    MultirateResamplerCandidate& candidate) {
    DeactivateCandidatePath(
        candidate.paths[candidate.transitionPath]);
    candidate.transitionPreparing = false;
    candidate.transitionOverlapping = false;
    candidate.overlapFrame = 0;
}

float ApplyCandidateResidual(const MultirateResampleTables& tables,
                             const float* ring, std::uint64_t index,
                             float fraction, double residualRatio) {
    double ratioPosition = 0.0;
    if (residualRatio <= 2.0) {
        ratioPosition =
            (residualRatio - 1.0) *
            static_cast<double>(
                kMultirateResidualBaseRatioRows - 1u);
    } else {
        ratioPosition =
            static_cast<double>(
                kMultirateResidualBaseRatioRows - 1u) +
            (residualRatio - 2.0) /
                (kMultirateResidualMaximumRatio - 2.0);
    }
    if (ratioPosition < 0.0)
        ratioPosition = 0.0;
    const double maximumRatioPosition =
        static_cast<double>(kMultirateResidualRatioRows - 1u);
    if (ratioPosition > maximumRatioPosition)
        ratioPosition = maximumRatioPosition;
    std::uint32_t ratioRow =
        static_cast<std::uint32_t>(ratioPosition);
    float ratioMix =
        static_cast<float>(ratioPosition -
                           static_cast<double>(ratioRow));
    if (ratioRow >= kMultirateResidualRatioRows - 1u) {
        ratioRow = kMultirateResidualRatioRows - 2u;
        ratioMix = 1.0f;
    }

    const float phasePosition =
        fraction * static_cast<float>(kMultirateResidualPhases);
    std::uint32_t phase =
        static_cast<std::uint32_t>(phasePosition);
    float phaseMix =
        phasePosition - static_cast<float>(phase);
    if (phase >= kMultirateResidualPhases - 1u) {
        phase = kMultirateResidualPhases - 2u;
        phaseMix = 1.0f;
    }

    const std::uint64_t first =
        index - (kMultirateResidualTaps / 2u - 1u);
    float value = 0.0f;
    for (std::uint32_t tap = 0; tap < kMultirateResidualTaps; ++tap) {
        const float row0Phase0 =
            tables.residual[ratioRow][phase][tap];
        const float row0Phase1 =
            tables.residual[ratioRow][phase + 1u][tap];
        const float row1Phase0 =
            tables.residual[ratioRow + 1u][phase][tap];
        const float row1Phase1 =
            tables.residual[ratioRow + 1u][phase + 1u][tap];
        const float row0 =
            row0Phase0 + phaseMix * (row0Phase1 - row0Phase0);
        const float row1 =
            row1Phase0 + phaseMix * (row1Phase1 - row1Phase0);
        const float coefficient =
            row0 + ratioMix * (row1 - row0);
        value += coefficient *
                 ring[(first + tap) & kCandidateFinalRingMask];
    }
    return value;
}

void BuildResidualCoefficients(const MultirateResampleTables& tables,
                               float fraction, double residualRatio,
                               float* coefficients) {
    double ratioPosition = 0.0;
    if (residualRatio <= 2.0) {
        ratioPosition =
            (residualRatio - 1.0) *
            static_cast<double>(kMultirateResidualBaseRatioRows - 1u);
    } else {
        ratioPosition =
            static_cast<double>(kMultirateResidualBaseRatioRows - 1u) +
            (residualRatio - 2.0) /
                (kMultirateResidualMaximumRatio - 2.0);
    }
    if (ratioPosition < 0.0)
        ratioPosition = 0.0;
    const double maximumRatioPosition =
        static_cast<double>(kMultirateResidualRatioRows - 1u);
    if (ratioPosition > maximumRatioPosition)
        ratioPosition = maximumRatioPosition;
    std::uint32_t ratioRow =
        static_cast<std::uint32_t>(ratioPosition);
    float ratioMix =
        static_cast<float>(ratioPosition - static_cast<double>(ratioRow));
    if (ratioRow >= kMultirateResidualRatioRows - 1u) {
        ratioRow = kMultirateResidualRatioRows - 2u;
        ratioMix = 1.0f;
    }

    const float phasePosition =
        fraction * static_cast<float>(kMultirateResidualPhases);
    std::uint32_t phase = static_cast<std::uint32_t>(phasePosition);
    float phaseMix = phasePosition - static_cast<float>(phase);
    if (phase >= kMultirateResidualPhases - 1u) {
        phase = kMultirateResidualPhases - 2u;
        phaseMix = 1.0f;
    }
    for (std::uint32_t tap = 0; tap < kMultirateResidualTaps; ++tap) {
        const float row0Phase0 = tables.residual[ratioRow][phase][tap];
        const float row0Phase1 =
            tables.residual[ratioRow][phase + 1u][tap];
        const float row1Phase0 =
            tables.residual[ratioRow + 1u][phase][tap];
        const float row1Phase1 =
            tables.residual[ratioRow + 1u][phase + 1u][tap];
        const float row0 =
            row0Phase0 + phaseMix * (row0Phase1 - row0Phase0);
        const float row1 =
            row1Phase0 + phaseMix * (row1Phase1 - row1Phase0);
        coefficients[tap] = row0 + ratioMix * (row1 - row0);
    }
}

float DotResidual(const float* ring, std::uint64_t index,
                  const float* coefficients) {
    const std::uint64_t first =
        index - (kMultirateResidualTaps / 2u - 1u);
    float value = 0.0f;
    for (std::uint32_t tap = 0; tap < kMultirateResidualTaps; ++tap) {
        value += coefficients[tap] *
                 ring[(first + tap) & kCandidateFinalRingMask];
    }
    return value;
}

void DotResidual4(float* const* dst, std::uint32_t dstIndex,
                  float* const rings[4], std::uint64_t index,
                  const float* coefficients) {
#if HARKEN_RESAMPLER_HAS_SSE2
    const std::uint64_t first =
        index - (kMultirateResidualTaps / 2u - 1u);
    __m128 acc = _mm_setzero_ps();
    for (std::uint32_t tap = 0; tap < kMultirateResidualTaps; ++tap) {
        const std::uint32_t idx = static_cast<std::uint32_t>(
            (first + tap) & kCandidateFinalRingMask);
        const __m128 samples =
            _mm_set_ps(rings[3][idx], rings[2][idx], rings[1][idx],
                       rings[0][idx]);
        acc = _mm_add_ps(
            acc, _mm_mul_ps(_mm_set1_ps(coefficients[tap]), samples));
    }
    alignas(16) float lane[4];
    _mm_store_ps(lane, acc);
    dst[0][dstIndex] = lane[0];
    dst[1][dstIndex] = lane[1];
    dst[2][dstIndex] = lane[2];
    dst[3][dstIndex] = lane[3];
#else
    for (std::uint32_t ch = 0; ch < 4u; ++ch)
        dst[ch][dstIndex] = DotResidual(rings[ch], index, coefficients);
#endif
}

double CandidateResidualRatio(
    const MultirateCandidatePathState& path,
    double frameRatio) {
    const double residualRatio =
        frameRatio / CandidatePathScale(path);
    return std::clamp(
        residualRatio, 1.0,
        kMultirateResidualMaximumRatio);
}

double CandidateResidualFilterRatio(
    const MultirateCandidatePathState& path,
    double frameRatio) {
    const double residualRatio =
        CandidateResidualRatio(path, frameRatio);
    if (path.stageCount != kMultirateMaxStages ||
        residualRatio < kMultirateResidualTailBlendStart) {
        return residualRatio;
    }
    const double tailMix = std::clamp(
        (residualRatio -
         kMultirateResidualTailBlendStart) /
            (2.0 - kMultirateResidualTailBlendStart),
        0.0, 1.0);
    return 2.0 +
           tailMix *
               (kMultirateResidualMaximumRatio - 2.0);
}

float RenderCandidatePath(
    MultirateResamplerCandidate& candidate,
    MultirateCandidatePathState& path,
    std::uint32_t channel, double frameRatio) {
    const CandidatePathCoordinate finalPosition =
        CandidatePathPosition(path, candidate.sourceFrame,
                              candidate.sourceFraction);
    const std::uint64_t index = finalPosition.index;
    const double fractionDouble = finalPosition.fraction;
    const float fraction =
        static_cast<float>(fractionDouble);
    const double residualRatio =
        CandidateResidualFilterRatio(path, frameRatio);
    if (residualRatio == 1.0 && fractionDouble == 0.0) {
        return path.finalRing[channel]
                             [index & kCandidateFinalRingMask];
    }
    return ApplyCandidateResidual(
        *candidate.tables, path.finalRing[channel], index,
        fraction, residualRatio);
}

bool CandidatePathUsesResidual(
    const MultirateResamplerCandidate& candidate,
    const MultirateCandidatePathState& path,
    double frameRatio) {
    const CandidatePathCoordinate position =
        CandidatePathPosition(path, candidate.sourceFrame,
                              candidate.sourceFraction);
    return CandidateResidualRatio(path, frameRatio) != 1.0 ||
           position.fraction != 0.0;
}

double CandidateTransitionBoundary(
    std::uint32_t activeStages,
    std::uint32_t transitionStages) {
    const std::uint32_t exponent =
        transitionStages > activeStages
            ? transitionStages
            : activeStages;
    return static_cast<double>(std::uint64_t{1} << exponent);
}

} // namespace

ResampleProfile SelectResampleProfile(std::uint32_t sourceRate,
                                      std::uint32_t mixRate) {
    (void)sourceRate;
    (void)mixRate;
    // Runtime default brick is Linear (LegacyCompatibility). Professional
    // remains available to internal callers that explicitly select it and is
    // never inferred from 44.1/48.
    return ResampleProfile::LegacyCompatibility;
}

void MultirateResampleTables::Init() {
    constexpr double kHalfbandBeta = 9.0;
    constexpr double kResidualBeta = 9.0;
    constexpr double kHalfbandCenter =
        static_cast<double>(kMultirateHalfbandTaps - 1u) * 0.5;
    const double halfbandI0Beta = BesselI0(kHalfbandBeta);
    double halfbandSum = 0.0;
    for (std::uint32_t tap = 0; tap < kMultirateHalfbandTaps; ++tap) {
        const int offset =
            static_cast<int>(tap) -
            static_cast<int>(kMultirateHalfbandTaps / 2u);
        double coefficient = 0.0;
        if (offset == 0) {
            coefficient = 0.5;
        } else if ((offset & 1) != 0) {
            const double x = static_cast<double>(offset);
            const double radius = x / kHalfbandCenter;
            const double window =
                std::abs(radius) >= 1.0
                    ? 0.0
                    : BesselI0(kHalfbandBeta *
                               std::sqrt(1.0 - radius * radius)) /
                          halfbandI0Beta;
            coefficient =
                std::sin(0.5 * kPi * x) / (kPi * x) * window;
        }
        halfband[tap] = static_cast<float>(coefficient);
        halfbandSum += coefficient;
    }
    const float inverseHalfbandSum =
        static_cast<float>(1.0 / halfbandSum);
    for (float& coefficient : halfband)
        coefficient *= inverseHalfbandSum;
    std::uint32_t sideTapCount = 0;
    for (std::uint32_t tap = 0;
         tap < kMultirateHalfbandTaps / 2u;
         tap += 2u) {
        const float coefficient = halfband[tap];
        if (coefficient == 0.0f)
            continue;
        const std::uint32_t mirror =
            kMultirateHalfbandTaps - 1u - tap;
        halfbandSide[sideTapCount++] = {
            coefficient, tap / 2u, mirror / 2u};
    }
    for (; sideTapCount < kMultirateHalfbandSideTapCount;
         ++sideTapCount) {
        halfbandSide[sideTapCount] = {};
    }

    constexpr double kResidualCenter =
        static_cast<double>(kMultirateResidualTaps / 2u - 1u);
    constexpr double kResidualHalfWidth =
        static_cast<double>(kMultirateResidualTaps - 1u) * 0.5;
    const double residualI0Beta = BesselI0(kResidualBeta);
    for (std::uint32_t row = 0;
         row < kMultirateResidualRatioRows; ++row) {
        const double rowRatio =
            row < kMultirateResidualBaseRatioRows
                ? 1.0 + static_cast<double>(row) /
                            static_cast<double>(
                                kMultirateResidualBaseRatioRows -
                                1u)
                : kMultirateResidualMaximumRatio;
        const double cutoff =
            row < kMultirateResidualBaseRatioRows
                ? 0.5 / rowRatio
                : kMultirateResidualTailCutoff;
        for (std::uint32_t phase = 0;
             phase < kMultirateResidualPhases; ++phase) {
            const double fraction =
                static_cast<double>(phase) /
                static_cast<double>(kMultirateResidualPhases);
            double sum = 0.0;
            for (std::uint32_t tap = 0;
                 tap < kMultirateResidualTaps; ++tap) {
                const double x =
                    static_cast<double>(tap) - kResidualCenter -
                    fraction;
                const double ideal =
                    std::abs(x) < 1.0e-12
                        ? 2.0 * cutoff
                        : std::sin(2.0 * kPi * cutoff * x) /
                              (kPi * x);
                const double radius = x / kResidualHalfWidth;
                const double window =
                    std::abs(radius) >= 1.0
                        ? 0.0
                        : BesselI0(kResidualBeta *
                                   std::sqrt(1.0 - radius * radius)) /
                              residualI0Beta;
                const double coefficient = ideal * window;
                residual[row][phase][tap] =
                    static_cast<float>(coefficient);
                sum += coefficient;
            }
            const float inverseSum =
                static_cast<float>(1.0 / sum);
            for (float& coefficient : residual[row][phase])
                coefficient *= inverseSum;
        }
    }
}

std::uint32_t MultirateResamplerCandidate::ResolveStageCount(
    double effectiveRatio) {
    std::uint32_t stageCount = 0;
    while (effectiveRatio >= 2.0 &&
           stageCount < kMultirateMaxStages) {
        effectiveRatio *= 0.5;
        ++stageCount;
    }
    return stageCount;
}

void MultirateResamplerCandidate::Reset(
    float* const* ringStorage, std::uint32_t channels,
    double inRatio, const MultirateResampleTables* t) {
    channelCount = channels;
    ratio = inRatio;
    targetRatio = inRatio;
    rampStep = 0.0;
    rampRemaining = 0;
    tables = t;
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        rawRing[ch] = ringStorage[ch];
        std::memset(rawRing[ch], 0,
                    kResampleFifoFrames * sizeof(float));
    }
    rawWriteIndex = kResampleHistoryPad;
    rawEndIndex = 0;
    inputEnded = false;
    std::memset(paths, 0, sizeof(paths));
    sourceFrame = kResampleHistoryPad;
    sourceFraction = 0.0;
    transitionOverlapSource = 0;
    activePath = 0;
    transitionPath = 1;
    transitionTargetStages = 0;
    overlapFrame = 0;
    transitionPreparing = false;
    transitionOverlapping = false;
    topologyTransitionsEnabled = true;
    counters = {};
    activeStages = ResolveStageCount(inRatio);
    InitializeCandidatePath(*this, paths[activePath], activeStages,
                            kResampleHistoryPad, true);
}

std::uint32_t MultirateResamplerCandidate::FreeFrames() const {
    std::uint64_t oldestProcessIndex =
        paths[activePath].rawProcessIndex;
    if ((transitionPreparing || transitionOverlapping) &&
        paths[transitionPath].initialized &&
        paths[transitionPath].rawProcessIndex <= rawWriteIndex &&
        paths[transitionPath].rawProcessIndex <
            oldestProcessIndex) {
        oldestProcessIndex =
            paths[transitionPath].rawProcessIndex;
    }
    const std::uint64_t occupied =
        rawWriteIndex - oldestProcessIndex;
    return occupied >= kResampleFifoFrames
               ? 0
               : kResampleFifoFrames -
                     static_cast<std::uint32_t>(occupied);
}

void MultirateResamplerCandidate::PushPCM(
    const PCMView& pcm, std::uint64_t startFrame,
    std::uint32_t frames) {
    for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
        const float* source = pcm.SamplePtr(ch, startFrame);
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            rawRing[ch][(rawWriteIndex + frame) &
                        kCandidateRawRingMask] =
                source[frame * pcm.stride];
        }
    }
    rawWriteIndex += frames;
}

void MultirateResamplerCandidate::PushSilence(
    std::uint32_t frames) {
    for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
        for (std::uint32_t frame = 0; frame < frames; ++frame) {
            rawRing[ch][(rawWriteIndex + frame) &
                        kCandidateRawRingMask] = 0.0f;
        }
    }
    rawWriteIndex += frames;
}

void MultirateResamplerCandidate::MarkInputEnd() {
    rawEndIndex = rawWriteIndex;
    inputEnded = true;
}

bool MultirateResamplerCandidate::Exhausted() const {
    return inputEnded &&
           sourceFrame >= rawEndIndex;
}

void MultirateResamplerCandidate::UpdateRatio(
    double newRatio, std::uint32_t rampFrames) {
    if (newRatio == targetRatio)
        return;
    targetRatio = newRatio;
    if (rampFrames == 0 || newRatio == ratio) {
        ratio = newRatio;
        rampStep = 0.0;
        rampRemaining = 0;
        return;
    }
    rampStep =
        (targetRatio - ratio) / static_cast<double>(rampFrames);
    rampRemaining = rampFrames;
}

template <bool Instrumented>
std::uint32_t MultirateResamplerCandidate::ProduceImpl(
    float* const* dst, std::uint32_t dstOffset,
    std::uint32_t maxFrames) {
    if constexpr (Instrumented) {
        if (activeStages > counters.maximumActiveStages)
            counters.maximumActiveStages = activeStages;
    }
    std::uint32_t produced = 0;
    while (produced < maxFrames) {
        if (inputEnded && sourceFrame >= rawEndIndex)
            break;
        double frameRatio = ratio;
        if (rampRemaining != 0) {
            frameRatio += rampStep;
            if (rampRemaining == 1)
                frameRatio = targetRatio;
        }

        const std::uint32_t desiredStages =
            ResolveStageCount(targetRatio);
        if (topologyTransitionsEnabled &&
            !transitionPreparing && !transitionOverlapping &&
            desiredStages != activeStages) {
            StartCandidateTransition<Instrumented>(*this,
                                                   desiredStages);
        } else if (transitionPreparing) {
            if (desiredStages == activeStages) {
                CancelCandidateTransition(*this);
            } else {
                const std::uint32_t adjacentStages =
                    desiredStages > activeStages
                        ? activeStages + 1u
                        : activeStages - 1u;
                if (adjacentStages != transitionTargetStages) {
                    CancelCandidateTransition(*this);
                    StartCandidateTransition<Instrumented>(
                        *this, desiredStages);
                }
            }
        }

        const double proposedFrameRatio = frameRatio;
        bool holdRampAtBoundary = false;
        double transitionBoundary = 0.0;
        if (transitionPreparing || transitionOverlapping) {
            transitionBoundary =
                CandidateTransitionBoundary(
                    activeStages, transitionTargetStages);
        }

        MultirateCandidatePathState& active =
            paths[activePath];
        if (transitionPreparing || transitionOverlapping) {
            MultirateCandidatePathState& transition =
                paths[transitionPath];
            const std::uint64_t prepareSource =
                transitionPreparing &&
                        sourceFrame < transitionOverlapSource
                    ? transitionOverlapSource
                    : sourceFrame;
            EnsureCandidateLookahead<Instrumented>(
                *this, transition, prepareSource, 0.0);
            const std::uint64_t transitionPumpLimit =
                active.rawProcessIndex < rawWriteIndex
                    ? active.rawProcessIndex
                    : rawWriteIndex;
            while (transition.rawProcessIndex <
                       transitionPumpLimit &&
                   CandidateFinalFreeFrames(
                       *this, transition) != 0) {
                if (!ProcessCandidateRawFrameLegacy<Instrumented>(
                        *this, transition)) {
                    break;
                }
            }
        }

        if (!EnsureCandidateLookahead<Instrumented>(
                *this, active, sourceFrame, sourceFraction))
            break;

        if (transitionPreparing) {
            MultirateCandidatePathState& transition =
                paths[transitionPath];
            const bool upward =
                transitionTargetStages > activeStages;
            bool shouldStartOverlap = false;
            if (upward) {
                if (ratio >= transitionBoundary ||
                    proposedFrameRatio >= transitionBoundary) {
                    shouldStartOverlap = true;
                } else if (rampRemaining != 0 &&
                           rampStep > 0.0) {
                    const double framesToBoundary =
                        std::ceil(
                            (transitionBoundary - ratio) /
                            rampStep);
                    shouldStartOverlap =
                        framesToBoundary <=
                        static_cast<double>(
                            kCandidateOverlapFrames);
                }
            } else {
                shouldStartOverlap =
                    ratio <= transitionBoundary ||
                    proposedFrameRatio <= transitionBoundary;
            }
            if (shouldStartOverlap) {
                const bool overlapPointReached =
                    sourceFrame >= transitionOverlapSource;
                if (overlapPointReached) {
                    if (!CandidatePathReady(
                            transition, sourceFrame,
                            sourceFraction)) {
                        if (!inputEnded && FreeFrames() != 0)
                            break;
                    } else {
                        transitionPreparing = false;
                        transitionOverlapping = true;
                        overlapFrame = 0;
                    }
                }
            }
        }

        if (transitionPreparing || transitionOverlapping) {
            const bool upward =
                transitionTargetStages > activeStages;
            if (transitionPreparing) {
                const bool crossedBoundary =
                    upward
                        ? proposedFrameRatio >=
                              transitionBoundary
                        : proposedFrameRatio <=
                              transitionBoundary;
                holdRampAtBoundary = crossedBoundary;
            } else if (upward) {
                const bool oldPathStillAudible =
                    overlapFrame + 1u <
                    kCandidateOverlapFrames;
                holdRampAtBoundary =
                    oldPathStillAudible &&
                    proposedFrameRatio >
                        transitionBoundary;
            }
            if (holdRampAtBoundary)
                frameRatio = transitionBoundary;
        }

        MultirateCandidatePathState* transition = nullptr;
        if (transitionOverlapping) {
            transition = &paths[transitionPath];
            if (!EnsureCandidateLookahead<Instrumented>(
                    *this, *transition, sourceFrame,
                    sourceFraction)) {
                break;
            }
        }

        if constexpr (Instrumented) {
            const bool activeUsesResidual =
                CandidatePathUsesResidual(*this, active, frameRatio);
            const bool transitionUsesResidual =
                transition != nullptr &&
                CandidatePathUsesResidual(*this, *transition,
                                          frameRatio);
            const std::uint32_t residualPaths =
                (activeUsesResidual ? 1u : 0u) +
                (transitionUsesResidual ? 1u : 0u);
            counters.residualOutputFrames += residualPaths;
            counters.residualMultiplyAccumulates +=
                static_cast<std::uint64_t>(residualPaths) *
                channelCount * kMultirateResidualTaps;
        }

        float transitionMix = 0.0f;
        if (transition != nullptr) {
            const float x =
                static_cast<float>(overlapFrame + 1u) /
                static_cast<float>(kCandidateOverlapFrames);
            transitionMix =
                x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
        }
        const CandidatePathCoordinate activePos =
            CandidatePathPosition(active, sourceFrame, sourceFraction);
        const double activeResidual =
            CandidateResidualFilterRatio(active, frameRatio);
        const bool activeCopy =
            activeResidual == 1.0 && activePos.fraction == 0.0;
        float activeCoefficients[kMultirateResidualTaps];
        if (!activeCopy) {
            BuildResidualCoefficients(
                *tables, static_cast<float>(activePos.fraction),
                activeResidual, activeCoefficients);
        }
        float transitionCoefficients[kMultirateResidualTaps];
        CandidatePathCoordinate transitionPos{};
        bool transitionCopy = true;
        if (transition != nullptr) {
            transitionPos = CandidatePathPosition(
                *transition, sourceFrame, sourceFraction);
            const double transitionResidual =
                CandidateResidualFilterRatio(*transition, frameRatio);
            transitionCopy =
                transitionResidual == 1.0 &&
                transitionPos.fraction == 0.0;
            if (!transitionCopy) {
                BuildResidualCoefficients(
                    *tables, static_cast<float>(transitionPos.fraction),
                    transitionResidual, transitionCoefficients);
            }
        }
        const std::uint32_t outIndex = dstOffset + produced;
        if (transition == nullptr && activeCopy) {
            const std::uint32_t copyIndex = static_cast<std::uint32_t>(
                activePos.index & kCandidateFinalRingMask);
            for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
                dst[ch][outIndex] = active.finalRing[ch][copyIndex];
            }
        } else if (transition == nullptr) {
            std::uint32_t ch = 0;
            for (; ch + 4u <= channelCount; ch += 4u) {
                float* rings[4] = {
                    active.finalRing[ch + 0], active.finalRing[ch + 1],
                    active.finalRing[ch + 2], active.finalRing[ch + 3]};
                float* outputs[4] = {
                    dst[ch + 0], dst[ch + 1], dst[ch + 2], dst[ch + 3]};
                DotResidual4(outputs, outIndex, rings, activePos.index,
                             activeCoefficients);
            }
            for (; ch < channelCount; ++ch) {
                dst[ch][outIndex] =
                    DotResidual(active.finalRing[ch], activePos.index,
                                activeCoefficients);
            }
        } else {
            for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
                const float activeValue =
                    activeCopy
                        ? active.finalRing[ch][activePos.index &
                                               kCandidateFinalRingMask]
                        : DotResidual(active.finalRing[ch], activePos.index,
                                      activeCoefficients);
                const float transitionValue =
                    transitionCopy
                        ? transition->finalRing[ch]
                              [transitionPos.index &
                               kCandidateFinalRingMask]
                        : DotResidual(transition->finalRing[ch],
                                      transitionPos.index,
                                      transitionCoefficients);
                dst[ch][outIndex] =
                    activeValue +
                    transitionMix * (transitionValue - activeValue);
            }
        }
        if (holdRampAtBoundary) {
            ratio = frameRatio;
        } else if (rampRemaining != 0) {
            ratio = frameRatio;
            --rampRemaining;
        }
        const double advanced = sourceFraction + frameRatio;
        const std::uint64_t whole =
            static_cast<std::uint64_t>(advanced);
        if (sourceFrame > UINT64_MAX - whole) {
            sourceFrame = UINT64_MAX;
            sourceFraction = 0.0;
        } else {
            sourceFrame += whole;
            sourceFraction =
                advanced - static_cast<double>(whole);
        }
        ++produced;

        if (transitionOverlapping) {
            ++overlapFrame;
            if (overlapFrame >= kCandidateOverlapFrames) {
                const std::uint32_t oldActivePath = activePath;
                activePath = transitionPath;
                transitionPath = oldActivePath;
                activeStages = paths[activePath].stageCount;
                DeactivateCandidatePath(paths[transitionPath]);
                transitionOverlapping = false;
                transitionPreparing = false;
                overlapFrame = 0;
                if (rampRemaining == 0)
                    ratio = targetRatio;
            }
        }
    }
    if constexpr (Instrumented) {
        const std::uint64_t logicalRawFrames =
            sourceFrame > kResampleHistoryPad
                ? sourceFrame - kResampleHistoryPad
                : 0;
        counters.rawFramesRead = logicalRawFrames;
        counters.halfbandOutputFrames =
            counters.residualOutputFrames * activeStages;
        counters.halfbandMultiplyAccumulates =
            counters.halfbandOutputFrames * channelCount *
            kCandidateHalfbandMacsPerChannel;
    }
    return produced;
}

std::uint32_t MultirateResamplerCandidate::Produce(
    float* const* dst, std::uint32_t dstOffset,
    std::uint32_t maxFrames) {
    return ProduceImpl<false>(dst, dstOffset, maxFrames);
}

std::uint32_t
MultirateResamplerCandidate::ProduceInstrumentedForTesting(
    float* const* dst, std::uint32_t dstOffset,
    std::uint32_t maxFrames) {
    return ProduceImpl<true>(dst, dstOffset, maxFrames);
}

std::uint32_t
MultirateResamplerCandidate::ProducePerChannelResidualForTesting(
    float* const* dst, std::uint32_t dstOffset,
    std::uint32_t maxFrames) {
    std::uint32_t produced = 0;
    while (produced < maxFrames) {
        if (inputEnded && sourceFrame >= rawEndIndex)
            break;
        double frameRatio = ratio;
        if (rampRemaining != 0) {
            frameRatio += rampStep;
            if (rampRemaining == 1)
                frameRatio = targetRatio;
        }

        const std::uint32_t desiredStages =
            ResolveStageCount(targetRatio);
        if (topologyTransitionsEnabled &&
            !transitionPreparing && !transitionOverlapping &&
            desiredStages != activeStages) {
            StartCandidateTransition<false>(*this, desiredStages);
        } else if (transitionPreparing) {
            if (desiredStages == activeStages) {
                CancelCandidateTransition(*this);
            } else {
                const std::uint32_t adjacentStages =
                    desiredStages > activeStages
                        ? activeStages + 1u
                        : activeStages - 1u;
                if (adjacentStages != transitionTargetStages) {
                    CancelCandidateTransition(*this);
                    StartCandidateTransition<false>(*this, desiredStages);
                }
            }
        }

        const double proposedFrameRatio = frameRatio;
        bool holdRampAtBoundary = false;
        double transitionBoundary = 0.0;
        if (transitionPreparing || transitionOverlapping) {
            transitionBoundary = CandidateTransitionBoundary(
                activeStages, transitionTargetStages);
        }

        MultirateCandidatePathState& active = paths[activePath];
        if (transitionPreparing || transitionOverlapping) {
            MultirateCandidatePathState& transition =
                paths[transitionPath];
            const std::uint64_t prepareSource =
                transitionPreparing && sourceFrame < transitionOverlapSource
                    ? transitionOverlapSource
                    : sourceFrame;
            EnsureCandidateLookaheadLegacy<false>(
                *this, transition, prepareSource, 0.0);
            const std::uint64_t transitionPumpLimit =
                active.rawProcessIndex < rawWriteIndex
                    ? active.rawProcessIndex
                    : rawWriteIndex;
            while (transition.rawProcessIndex < transitionPumpLimit &&
                   CandidateFinalFreeFrames(*this, transition) != 0) {
                if (!ProcessCandidateRawFrameLegacy<false>(*this, transition))
                    break;
            }
        }

        if (!EnsureCandidateLookaheadLegacy<false>(
                *this, active, sourceFrame, sourceFraction))
            break;

        if (transitionPreparing) {
            MultirateCandidatePathState& transition =
                paths[transitionPath];
            const bool upward = transitionTargetStages > activeStages;
            bool shouldStartOverlap = false;
            if (upward) {
                if (ratio >= transitionBoundary ||
                    proposedFrameRatio >= transitionBoundary) {
                    shouldStartOverlap = true;
                } else if (rampRemaining != 0 && rampStep > 0.0) {
                    const double framesToBoundary = std::ceil(
                        (transitionBoundary - ratio) / rampStep);
                    shouldStartOverlap =
                        framesToBoundary <=
                        static_cast<double>(kCandidateOverlapFrames);
                }
            } else {
                shouldStartOverlap =
                    ratio <= transitionBoundary ||
                    proposedFrameRatio <= transitionBoundary;
            }
            if (shouldStartOverlap) {
                const bool overlapPointReached =
                    sourceFrame >= transitionOverlapSource;
                if (overlapPointReached) {
                    if (!CandidatePathReady(
                            transition, sourceFrame, sourceFraction)) {
                        if (!inputEnded && FreeFrames() != 0)
                            break;
                    } else {
                        transitionPreparing = false;
                        transitionOverlapping = true;
                        overlapFrame = 0;
                    }
                }
            }
        }

        if (transitionPreparing || transitionOverlapping) {
            const bool upward = transitionTargetStages > activeStages;
            if (transitionPreparing) {
                const bool crossedBoundary =
                    upward ? proposedFrameRatio >= transitionBoundary
                           : proposedFrameRatio <= transitionBoundary;
                holdRampAtBoundary = crossedBoundary;
            } else if (upward) {
                const bool oldPathStillAudible =
                    overlapFrame + 1u < kCandidateOverlapFrames;
                holdRampAtBoundary =
                    oldPathStillAudible &&
                    proposedFrameRatio > transitionBoundary;
            }
            if (holdRampAtBoundary)
                frameRatio = transitionBoundary;
        }

        MultirateCandidatePathState* transition = nullptr;
        if (transitionOverlapping) {
            transition = &paths[transitionPath];
            if (!EnsureCandidateLookaheadLegacy<false>(
                    *this, *transition, sourceFrame, sourceFraction))
                break;
        }

        float transitionMix = 0.0f;
        if (transition != nullptr) {
            const float x =
                static_cast<float>(overlapFrame + 1u) /
                static_cast<float>(kCandidateOverlapFrames);
            transitionMix =
                x * x * x * (x * (x * 6.0f - 15.0f) + 10.0f);
        }
        for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
            const float activeValue =
                RenderCandidatePath(*this, active, ch, frameRatio);
            float value = activeValue;
            if (transition != nullptr) {
                const float transitionValue =
                    RenderCandidatePath(*this, *transition, ch, frameRatio);
                value = activeValue +
                        transitionMix * (transitionValue - activeValue);
            }
            dst[ch][dstOffset + produced] = value;
        }

        if (holdRampAtBoundary) {
            ratio = frameRatio;
        } else if (rampRemaining != 0) {
            ratio = frameRatio;
            --rampRemaining;
        }
        const double advanced = sourceFraction + frameRatio;
        const std::uint64_t whole =
            static_cast<std::uint64_t>(advanced);
        if (sourceFrame > UINT64_MAX - whole) {
            sourceFrame = UINT64_MAX;
            sourceFraction = 0.0;
        } else {
            sourceFrame += whole;
            sourceFraction =
                advanced - static_cast<double>(whole);
        }
        ++produced;

        if (transitionOverlapping) {
            ++overlapFrame;
            if (overlapFrame >= kCandidateOverlapFrames) {
                const std::uint32_t oldActivePath = activePath;
                activePath = transitionPath;
                transitionPath = oldActivePath;
                activeStages = paths[activePath].stageCount;
                DeactivateCandidatePath(paths[transitionPath]);
                transitionOverlapping = false;
                transitionPreparing = false;
                overlapFrame = 0;
                if (rampRemaining == 0)
                    ratio = targetRatio;
            }
        }
    }
    return produced;
}

void Resampler::Reset(float* const* ringStorage, std::uint32_t channels,
                      ResampleQuality q, double inRatio,
                      const ResampleTables* t) {
    resampleProfile = ResampleProfile::LegacyCompatibility;
    professional = nullptr;
    channelCount = channels;
    quality = q;
    ratio = inRatio;
    targetRatio = inRatio;
    rampStep = 0.0;
    rampRemaining = 0;
    tables = t;
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        ring[ch] = ringStorage[ch];
        std::memset(ring[ch], 0, kResampleFifoFrames * sizeof(float));
    }
    // The pad occupies absolute indices [0, kResampleHistoryPad); the first
    // real input frame lands at index kResampleHistoryPad, where the read
    // head starts.
    writeIndex = kResampleHistoryPad;
    readIndex = kResampleHistoryPad;
    readFraction = 0.0;
    phaseHigh = 0;
    phaseLow = 0;
    readPos = static_cast<double>(kResampleHistoryPad);
    inputEndIndex = 0;
    inputEnded = false;
    std::memset(highRatioFilter, 0, sizeof(highRatioFilter));
    std::memset(highRatioPrevious, 0, sizeof(highRatioPrevious));
    std::memset(highRatioCurrent, 0, sizeof(highRatioCurrent));
    highRatioFilterIndex = kResampleHistoryPad;
    highRatioFilterActive = false;
}

void Resampler::Reset(
    float* const* ringStorage, std::uint32_t channels,
    ResampleQuality q, double inRatio, const ResampleTables* t,
    ResampleProfile profile,
    const MultirateResampleTables* multirateTables,
    MultirateResamplerCandidate* workspace) {
    assert(profile != ResampleProfile::Professional44148 || workspace != nullptr);
    Reset(ringStorage, channels, q, inRatio, t);
    if (profile == ResampleProfile::Professional44148) {
        professional = workspace;
        professional->Reset(ringStorage, channels, inRatio,
                           multirateTables);
        resampleProfile = ResampleProfile::Professional44148;
    }
}

std::uint32_t Resampler::FreeFrames() const {
    if (resampleProfile == ResampleProfile::Professional44148)
        return professional->FreeFrames();

    // Keep the quality tier's widest possible history even at ratio <= 1, so
    // a later UpdateRatio cannot widen into samples already overwritten. Near
    // the beginning, absolute zero bounds that reserve and earlier samples are
    // virtual silence.
    const std::uint32_t historyNeed = HistoryReserve(quality);
    const std::uint64_t currentReadIndex = readIndex;
    std::uint64_t oldestNeeded =
        currentReadIndex > historyNeed
            ? currentReadIndex - historyNeed
            : 0;
    // The high-ratio filter may be consuming a ratio-wide interval across many
    // refill calls. Its next raw frame is older than the direct-kernel reserve
    // until it catches up, so that frame also constrains ring overwrite.
    if (highRatioFilterActive && highRatioFilterIndex < oldestNeeded)
        oldestNeeded = highRatioFilterIndex;
    // Occupied span must be signed: at a high decimation ratio the read head
    // can leap past writeIndex in a single output step (the skipped input
    // frames are normal dropped samples), so oldestNeeded may exceed
    // writeIndex. A non-positive span means the read head has overtaken the
    // write head — nothing written is still behind the read head, so the whole
    // ring is free to accept input and the stage recovers as fresh input
    // catches the write head back up past the look-ahead window. Computing this
    // difference in unsigned arithmetic wraps to a huge value, pins FreeFrames
    // at 0, and wedges the feed and Produce forever (starvation deadlock).
    const std::int64_t occupied = static_cast<std::int64_t>(writeIndex) -
                                  static_cast<std::int64_t>(oldestNeeded);
    if (occupied <= 0) {
        return kResampleFifoFrames;
    }
    return (occupied >= static_cast<std::int64_t>(kResampleFifoFrames))
               ? 0
               : kResampleFifoFrames - static_cast<std::uint32_t>(occupied);
}

void Resampler::PushPCM(const PCMView& pcm, std::uint64_t startFrame,
                        std::uint32_t frames) {
    if (resampleProfile == ResampleProfile::Professional44148) {
        professional->PushPCM(pcm, startFrame, frames);
        return;
    }

    if (frames == 0)
        return;
    const std::uint32_t offset = writeIndex & kRingMask;
    const std::uint32_t first =
        std::min(frames, kResampleFifoFrames - offset);
    for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
        const float* src = pcm.SamplePtr(ch, startFrame);
        float* dst = ring[ch];
        if (pcm.stride == 1 && frames <= kResampleFifoFrames) {
            std::memcpy(dst + offset, src, first * sizeof(float));
            std::memcpy(dst, src + first, (frames - first) * sizeof(float));
        } else {
            for (std::uint32_t i = 0; i < frames; ++i) {
                dst[(writeIndex + i) & kRingMask] = src[i * pcm.stride];
            }
        }
    }
    writeIndex += frames;
}

void Resampler::PushSilence(std::uint32_t frames) {
    if (resampleProfile == ResampleProfile::Professional44148) {
        professional->PushSilence(frames);
        return;
    }

    const std::uint32_t offset = writeIndex & kRingMask;
    const std::uint32_t first =
        std::min(frames, kResampleFifoFrames - offset);
    for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
        float* dst = ring[ch];
        if (frames <= kResampleFifoFrames) {
            std::memset(dst + offset, 0, first * sizeof(float));
            std::memset(dst, 0, (frames - first) * sizeof(float));
        } else {
            for (std::uint32_t i = 0; i < frames; ++i) {
                dst[(writeIndex + i) & kRingMask] = 0.0f;
            }
        }
    }
    writeIndex += frames;
}

void Resampler::MarkInputEnd() {
    if (resampleProfile == ResampleProfile::Professional44148) {
        professional->MarkInputEnd();
        return;
    }
    inputEndIndex = writeIndex;
    inputEnded = true;
}

bool Resampler::Exhausted() const {
    return resampleProfile == ResampleProfile::Professional44148
               ? professional->Exhausted()
               : inputEnded && readIndex >= inputEndIndex;
}

void Resampler::UpdateRatio(double newRatio, std::uint32_t rampFrames) {
    if (resampleProfile == ResampleProfile::Professional44148) {
        professional->UpdateRatio(newRatio, rampFrames);
        return;
    }

    if (newRatio == targetRatio) {
        return;
    }

    targetRatio = newRatio;
    if (rampFrames == 0 || newRatio == ratio) {
        ratio = newRatio;
        rampStep = 0.0;
        rampRemaining = 0;
        return;
    }

    rampStep = (targetRatio - ratio) / static_cast<double>(rampFrames);
    rampRemaining = rampFrames;
}

bool Resampler::BorrowLinearUnity(std::uint32_t frames,
                                  ResamplerUnitySpan& out) noexcept {
    if (frames == 0 ||
        resampleProfile != ResampleProfile::LegacyCompatibility ||
        quality != ResampleQuality::Linear || ratio != 1.0 ||
        targetRatio != 1.0 || rampRemaining != 0 || phaseHigh != 0 || phaseLow != 0 ||
        inputEnded || highRatioFilterActive || readIndex >= writeIndex ||
        writeIndex - readIndex > kResampleFifoFrames ||
        frames >= writeIndex - readIndex || frames > UINT64_MAX - readIndex) {
        return false;
    }
    const std::uint32_t offset = readIndex & kRingMask;
    out = {offset, std::min(frames, kResampleFifoFrames - offset), frames};
    readIndex += frames;
    readFraction = 0.0;
    readPos = static_cast<double>(readIndex);
    return true;
}

std::uint32_t Resampler::Produce(float* const* dst, std::uint32_t dstOffset,
                                 std::uint32_t maxFrames) {
    if (resampleProfile == ResampleProfile::Professional44148) {
        return professional->Produce(dst, dstOffset, maxFrames);
    }

    if (resampleProfile == ResampleProfile::LegacyCompatibility &&
        quality == ResampleQuality::Linear && ratio == 1.0 &&
        targetRatio == 1.0 && rampRemaining == 0 && phaseHigh == 0 && phaseLow == 0 &&
        readIndex < writeIndex &&
        writeIndex - readIndex <= kResampleFifoFrames &&
        maxFrames <= UINT64_MAX - readIndex &&
        maxFrames <= UINT32_MAX - dstOffset) {
        std::uint64_t available = writeIndex - readIndex - 1;
        if (inputEnded) {
            available = readIndex < inputEndIndex
                            ? std::min(available, inputEndIndex - readIndex)
                            : 0;
        }
        const std::uint32_t frames = static_cast<std::uint32_t>(
            std::min<std::uint64_t>(available, maxFrames));
        if (frames == 0)
            return 0;
        const std::uint32_t offset = readIndex & kRingMask;
        const std::uint32_t first =
            std::min(frames, kResampleFifoFrames - offset);
        for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
            std::memcpy(dst[ch] + dstOffset, ring[ch] + offset,
                        first * sizeof(float));
            std::memcpy(dst[ch] + dstOffset + first, ring[ch],
                        (frames - first) * sizeof(float));
        }
        highRatioFilterActive = false;
        readIndex += frames;
        readFraction = 0.0;
        readPos = static_cast<double>(readIndex);
        return frames;
    }

    // Linear/Hermite keep a bounded interpolator at every ratio. Pitch-up
    // must not switch to scaled sinc or the high-ratio streaming filter;
    // those paths ignore the selected interpolator and dominate MixerLoop.
    const bool directInterp = quality == ResampleQuality::Linear;

    float scaledCoefficients[kMaxScaledKernelCoefficients] = {};
    HighRatioSectionCoefficients
        highRatioCoefficients[kResampleHighRatioFilterSections] = {};
    bool downsampling = !directInterp && ratio > 1.0;
    bool highRatio = !directInterp && ratio > kMaxDirectKernelScale;
    DownsampleGeometry geometry;
    if (downsampling && !highRatio)
        geometry = ResolveDownsampleGeometry(quality, ratio);
    if (highRatio)
        BuildHighRatioCoefficients(ratio, highRatioCoefficients);
    std::uint32_t ahead =
        downsampling && !highRatio ? geometry.need : AheadFrames(quality);

    std::uint32_t produced = 0;
    ExactSourcePhase exactPhase{phaseHigh, phaseLow};
    SourcePhaseStep phaseStep{};
    bool exactStep = DecodeSourcePhaseStep(ratio, &phaseStep);
    while (produced < maxFrames) {
        if (inputEnded && readIndex >= inputEndIndex) {
            break; // every real input frame consumed
        }
        double frameRatio = ratio;
        if (rampRemaining != 0) {
            frameRatio += rampStep;
            if (rampRemaining == 1)
                frameRatio = targetRatio;
            downsampling = !directInterp && frameRatio > 1.0;
            highRatio = !directInterp && frameRatio > kMaxDirectKernelScale;
            if (downsampling && !highRatio)
                geometry = ResolveDownsampleGeometry(quality, frameRatio);
            if (highRatio)
                BuildHighRatioCoefficients(frameRatio,
                                           highRatioCoefficients);
            ahead = downsampling && !highRatio ? geometry.need
                                                : AheadFrames(quality);
        }
        const std::uint64_t idx = readIndex;
        if (highRatio) {
            if (!PrepareHighRatioSample(*this, idx,
                                        highRatioCoefficients)) {
                break; // raw input was filtered, but the output is not ready
            }
        } else {
            highRatioFilterActive = false;
            if (idx + ahead >= writeIndex) {
                break; // starved: kernel look-ahead not yet in the ring
            }
        }
        if (rampRemaining != 0) {
            ratio = frameRatio;
            --rampRemaining;
            exactStep = DecodeSourcePhaseStep(ratio, &phaseStep);
        }
        const float frac = static_cast<float>(readFraction);

        if (downsampling && !highRatio) {
            if (geometry.useSinc16) {
                BuildScaledCoefficients(tables->sinc16, frac, geometry,
                                        scaledCoefficients);
            } else {
                BuildScaledCoefficients(tables->sinc8, frac, geometry,
                                        scaledCoefficients);
            }
        }

        for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
            const float* r = ring[ch];
            float value = 0.0f;
            if (highRatio) {
                const double filtered =
                    highRatioPrevious[ch] +
                    (highRatioCurrent[ch] - highRatioPrevious[ch]) *
                        static_cast<double>(frac);
                value = static_cast<float>(filtered);
            } else if (downsampling) {
                value = ApplyScaledKernel(r, idx, geometry.need,
                                          scaledCoefficients);
            } else {
                switch (quality) {
                    case ResampleQuality::Linear:
                        value = InterpLinear(r[idx & kRingMask],
                                             r[(idx + 1) & kRingMask], frac);
                        break;
                    case ResampleQuality::Hermite:
                        value = InterpHermite(r[(idx - 1) & kRingMask],
                                              r[idx & kRingMask],
                                              r[(idx + 1) & kRingMask],
                                              r[(idx + 2) & kRingMask], frac);
                        break;
                    case ResampleQuality::Sinc8: {
                        float window[8];
                        const std::uint64_t first =
                            idx - KaiserSincTable<8>::kTapCenter;
                        for (int t = 0; t < 8; ++t) {
                            window[t] = r[(first + t) & kRingMask];
                        }
                        value = InterpPolyphase(tables->sinc8, window, frac);
                        break;
                    }
                    case ResampleQuality::Sinc16: {
                        float window[16];
                        const std::uint64_t first =
                            idx - KaiserSincTable<16>::kTapCenter;
                        for (int t = 0; t < 16; ++t) {
                            window[t] = r[(first + t) & kRingMask];
                        }
                        value = InterpPolyphase(tables->sinc16, window, frac);
                        break;
                    }
                }
            }
            dst[ch][dstOffset + produced] = value;
        }

        std::uint64_t whole = 0;
        if (exactStep) {
            whole = StepSourcePhase(phaseStep, &exactPhase);
            readFraction = SourcePhaseFraction(exactPhase);
        } else {
            // Retain the standalone DSP's extreme-rate behavior outside the
            // finite, representable playback timing domain.
            const double advanced = readFraction + ratio;
            whole = static_cast<std::uint64_t>(advanced);
            readFraction = advanced - static_cast<double>(whole);
            SourcePhaseStep imported{};
            exactPhase = {};
            if (readFraction != 0.0 && DecodeSourcePhaseStep(readFraction, &imported))
                exactPhase = imported.fraction;
        }
        if (readIndex > UINT64_MAX - whole) {
            readIndex = UINT64_MAX;
            readFraction = 0.0;
            exactPhase = {};
        } else {
            readIndex += whole;
        }
        readPos = static_cast<double>(readIndex) + readFraction;
        ++produced;
    }
    phaseHigh = exactPhase.high;
    phaseLow = exactPhase.low;
    return produced;
}

bool Resampler::CaptureProfessional(
    ProfessionalResamplerState* out) const {
    if (out == nullptr ||
        resampleProfile != ResampleProfile::Professional44148 ||
        professional == nullptr ||
        professional->channelCount == 0 ||
        professional->channelCount > kMaxChannels) {
        return false;
    }
    *out = {};
    std::uint64_t oldest =
        professional->paths[professional->activePath].rawProcessIndex;
    if ((professional->transitionPreparing ||
         professional->transitionOverlapping) &&
        professional->paths[professional->transitionPath].initialized) {
        oldest = std::min(
            oldest,
            professional->paths[professional->transitionPath].rawProcessIndex);
    }
    if (oldest > professional->rawWriteIndex ||
        professional->rawWriteIndex - oldest > kResampleFifoFrames) {
        return false;
    }

    out->formatVersion =
        ProfessionalResamplerState::kFormatVersion;
    out->profile = ResampleProfile::Professional44148;
    out->quality = quality;
    out->channelCount = professional->channelCount;
    out->activeStages = professional->activeStages;
    out->ratio = professional->ratio;
    out->targetRatio = professional->targetRatio;
    out->rampStep = professional->rampStep;
    out->rampRemaining = professional->rampRemaining;
    out->rawWriteIndex = professional->rawWriteIndex;
    out->rawEndIndex = professional->rawEndIndex;
    out->inputEnded = professional->inputEnded;
    std::memcpy(out->paths, professional->paths,
                sizeof(out->paths));
    out->sourceFrame = professional->sourceFrame;
    out->sourceFraction = professional->sourceFraction;
    out->transitionOverlapSource =
        professional->transitionOverlapSource;
    out->activePath = professional->activePath;
    out->transitionPath = professional->transitionPath;
    out->transitionTargetStages =
        professional->transitionTargetStages;
    out->overlapFrame = professional->overlapFrame;
    out->transitionPreparing = professional->transitionPreparing;
    out->transitionOverlapping = professional->transitionOverlapping;
    out->topologyTransitionsEnabled =
        professional->topologyTransitionsEnabled;
    out->rawStartIndex = oldest;
    out->rawFrameCount = static_cast<std::uint32_t>(
        professional->rawWriteIndex - oldest);
    for (std::uint32_t ch = 0; ch < out->channelCount; ++ch) {
        for (std::uint32_t frame = 0;
             frame < out->rawFrameCount; ++frame) {
            out->rawFrames[ch][frame] =
                professional->rawRing[ch][
                    (oldest + frame) & kCandidateRawRingMask];
        }
    }
    out->usedBytes = static_cast<std::uint32_t>(
        sizeof(ProfessionalResamplerState) -
        sizeof(out->rawFrames) +
        static_cast<std::size_t>(out->channelCount) *
            out->rawFrameCount * sizeof(float));
    return true;
}

bool Resampler::ValidateProfessionalState(
    const ProfessionalResamplerState& state) {
    if (state.formatVersion !=
            ProfessionalResamplerState::kFormatVersion ||
        state.profile != ResampleProfile::Professional44148 ||
        state.quality > ResampleQuality::Sinc16 ||
        state.channelCount == 0 ||
        state.channelCount > kMaxChannels ||
        state.activePath > 1u ||
        state.transitionPath > 1u ||
        state.activePath == state.transitionPath ||
        state.activeStages > kMultirateMaxStages ||
        state.transitionTargetStages > kMultirateMaxStages ||
        state.sourceFraction < 0.0 ||
        state.sourceFraction >= 1.0 ||
        !std::isfinite(state.sourceFraction) ||
        !std::isfinite(state.ratio) ||
        !std::isfinite(state.targetRatio) ||
        !std::isfinite(state.rampStep) ||
        state.ratio <= 0.0 ||
        state.targetRatio <= 0.0 ||
        state.ratio < kProfessionalMinimumSafeRatio ||
        state.ratio > kProfessionalMaximumSafeRatio ||
        state.targetRatio < kProfessionalMinimumSafeRatio ||
        state.targetRatio > kProfessionalMaximumSafeRatio ||
        state.rawFrameCount > kResampleFifoFrames ||
        (state.inputEnded &&
         state.rawEndIndex > state.rawWriteIndex) ||
        (state.transitionPreparing &&
         state.transitionOverlapping) ||
        state.overlapFrame >= kCandidateOverlapFrames ||
        (!state.transitionOverlapping &&
         state.overlapFrame != 0)) {
        return false;
    }
    if (state.rampRemaining != 0) {
        const double nextRatio = state.ratio + state.rampStep;
        if (!std::isfinite(nextRatio) ||
            nextRatio < kProfessionalMinimumSafeRatio ||
            nextRatio > kProfessionalMaximumSafeRatio) {
            return false;
        }
        if (state.rampRemaining > 1u) {
            const long double linearEnd =
                static_cast<long double>(state.ratio) +
                static_cast<long double>(state.rampStep) *
                    static_cast<long double>(
                        state.rampRemaining - 1u);
            if (!std::isfinite(linearEnd) ||
                linearEnd <
                    static_cast<long double>(
                        kProfessionalMinimumSafeRatio) ||
                linearEnd >
                    static_cast<long double>(
                        kProfessionalMaximumSafeRatio)) {
                return false;
            }
        }
    }
    const std::size_t expectedUsedBytes =
        sizeof(ProfessionalResamplerState) -
        sizeof(state.rawFrames) +
        static_cast<std::size_t>(state.channelCount) *
            state.rawFrameCount * sizeof(float);
    if (state.rawStartIndex > state.rawWriteIndex ||
        state.rawWriteIndex - state.rawStartIndex !=
            state.rawFrameCount ||
        state.usedBytes != expectedUsedBytes ||
        expectedUsedBytes > sizeof(ProfessionalResamplerState)) {
        return false;
    }
    for (std::uint32_t index = 0; index < 2u; ++index) {
        const MultirateCandidatePathState& path =
            state.paths[index];
        const bool futurePreparingPath =
            state.transitionPreparing &&
            index == state.transitionPath;
        if (path.stageCount > kMultirateMaxStages ||
            (path.rawProcessIndex > state.rawWriteIndex &&
             (!futurePreparingPath ||
              path.rawProcessIndex - state.rawWriteIndex >
                  kCandidateMaximumTransitionLookahead)) ||
            (path.initialized &&
             path.rawProcessIndex <= state.rawWriteIndex &&
             path.rawProcessIndex < state.rawStartIndex)) {
            return false;
        }
        for (const MultirateHalfbandStageState& stage :
             path.stages) {
            if (stage.sideWriteOffset >=
                    kMultirateHalfbandSideFrames ||
                stage.centerWriteOffset >=
                    kMultirateHalfbandCenterFrames) {
                return false;
            }
        }
    }
    const MultirateCandidatePathState& active =
        state.paths[state.activePath];
    const MultirateCandidatePathState& transition =
        state.paths[state.transitionPath];
    if (!active.initialized ||
        active.stageCount != state.activeStages) {
        return false;
    }
    const bool adjacentTransition =
        state.transitionTargetStages + 1u ==
            state.activeStages ||
        state.activeStages + 1u ==
            state.transitionTargetStages;
    if (state.transitionPreparing) {
        return transition.initialized &&
               adjacentTransition &&
               transition.stageCount ==
                   state.transitionTargetStages;
    }
    if (state.transitionOverlapping) {
        return transition.initialized &&
               adjacentTransition &&
               transition.stageCount ==
                   state.transitionTargetStages &&
               transition.rawProcessIndex <= state.rawWriteIndex;
    }
    return !transition.initialized;
}

bool Resampler::RestoreProfessional(
    const ProfessionalResamplerState& state,
    float* const* ringStorage, std::uint32_t channels,
    const ResampleTables* legacyTables,
    const MultirateResampleTables* multirateTables,
    MultirateResamplerCandidate* workspace) {
    if (workspace == nullptr || !ValidateProfessionalState(state) ||
        ringStorage == nullptr ||
        multirateTables == nullptr ||
        channels != state.channelCount) {
        return false;
    }
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        if (ringStorage[ch] == nullptr)
            return false;
    }
    Reset(ringStorage, channels, state.quality,
          state.ratio, legacyTables,
          ResampleProfile::Professional44148, multirateTables, workspace);
    professional->activeStages = state.activeStages;
    professional->ratio = state.ratio;
    professional->targetRatio = state.targetRatio;
    professional->rampStep = state.rampStep;
    professional->rampRemaining = state.rampRemaining;
    professional->rawWriteIndex = state.rawWriteIndex;
    professional->rawEndIndex = state.rawEndIndex;
    professional->inputEnded = state.inputEnded;
    std::memcpy(professional->paths, state.paths,
                sizeof(professional->paths));
    professional->sourceFrame = state.sourceFrame;
    professional->sourceFraction = state.sourceFraction;
    professional->transitionOverlapSource =
        state.transitionOverlapSource;
    professional->activePath = state.activePath;
    professional->transitionPath = state.transitionPath;
    professional->transitionTargetStages =
        state.transitionTargetStages;
    professional->overlapFrame = state.overlapFrame;
    professional->transitionPreparing = state.transitionPreparing;
    professional->transitionOverlapping = state.transitionOverlapping;
    professional->topologyTransitionsEnabled =
        state.topologyTransitionsEnabled;
    professional->counters = {};
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        std::memset(ringStorage[ch], 0,
                    kResampleFifoFrames * sizeof(float));
        for (std::uint32_t frame = 0;
             frame < state.rawFrameCount; ++frame) {
            ringStorage[ch][
                (state.rawStartIndex + frame) &
                kCandidateRawRingMask] =
                state.rawFrames[ch][frame];
        }
        professional->rawRing[ch] = ringStorage[ch];
    }
    professional->tables = multirateTables;
    return true;
}

bool Resampler::CaptureParkState(ResamplerParkState* out) const {
    if (resampleProfile == ResampleProfile::Professional44148)
        return CaptureProfessional(out);
    if (out == nullptr || channelCount == 0 || channelCount > kMaxChannels)
        return false;

    *out = {};
    out->formatVersion = ResamplerParkState::kFormatVersion;
    out->profile = ResampleProfile::LegacyCompatibility;
    out->quality = quality;
    out->channelCount = channelCount;
    out->ratio = ratio;
    out->targetRatio = targetRatio;
    out->rampStep = rampStep;
    out->rampRemaining = rampRemaining;
    out->legacyReadIndex = readIndex;
    out->legacyReadFraction = readFraction;
    out->legacyPhaseHigh = phaseHigh;
    out->legacyPhaseLow = phaseLow;
    out->legacyWriteIndex = writeIndex;
    out->legacyInputEndIndex = inputEndIndex;
    out->legacyInputEnded = inputEnded;
    std::memcpy(out->legacyHighRatioFilter, highRatioFilter,
                sizeof(highRatioFilter));
    std::memcpy(out->legacyHighRatioPrevious, highRatioPrevious,
                sizeof(highRatioPrevious));
    std::memcpy(out->legacyHighRatioCurrent, highRatioCurrent,
                sizeof(highRatioCurrent));
    out->legacyHighRatioFilterIndex = highRatioFilterIndex;
    out->legacyHighRatioFilterActive = highRatioFilterActive;

    const std::uint64_t oldest =
        writeIndex > kResampleFifoFrames
            ? writeIndex - kResampleFifoFrames
            : 0;
    if (highRatioFilterActive && highRatioFilterIndex < oldest)
        return false;
    out->rawStartIndex = oldest;
    out->rawFrameCount = static_cast<std::uint32_t>(writeIndex - oldest);
    for (std::uint32_t ch = 0; ch < channelCount; ++ch) {
        for (std::uint32_t frame = 0; frame < out->rawFrameCount; ++frame) {
            out->rawFrames[ch][frame] =
                ring[ch][(oldest + frame) & kRingMask];
        }
    }
    out->usedBytes = static_cast<std::uint32_t>(
        sizeof(ResamplerParkState) - sizeof(out->rawFrames) +
        static_cast<std::size_t>(channelCount) *
            out->rawFrameCount * sizeof(float));
    return ValidateParkState(*out);
}

bool Resampler::ValidateParkState(const ResamplerParkState& state) {
    if (state.profile == ResampleProfile::Professional44148)
        return ValidateProfessionalState(state);
    if (state.formatVersion != ResamplerParkState::kFormatVersion ||
        state.profile != ResampleProfile::LegacyCompatibility ||
        state.quality > ResampleQuality::Sinc16 ||
        state.channelCount == 0 || state.channelCount > kMaxChannels ||
        !std::isfinite(state.ratio) ||
        !std::isfinite(state.targetRatio) ||
        !std::isfinite(state.rampStep) ||
        !std::isfinite(state.legacyReadFraction) ||
        state.ratio <= 0.0 ||
        state.targetRatio <= 0.0 ||
        state.legacyReadFraction < 0.0 ||
        state.legacyReadFraction >= 1.0 ||
        state.legacyReadFraction != SourcePhaseFraction(
            {state.legacyPhaseHigh, state.legacyPhaseLow}) ||
        state.rawFrameCount > kResampleFifoFrames ||
        state.rawStartIndex > state.legacyWriteIndex ||
        state.legacyWriteIndex - state.rawStartIndex !=
            state.rawFrameCount ||
        (state.legacyInputEnded &&
         state.legacyInputEndIndex > state.legacyWriteIndex) ||
        (state.legacyHighRatioFilterActive &&
         (state.legacyHighRatioFilterIndex < state.rawStartIndex ||
          state.legacyHighRatioFilterIndex > state.legacyWriteIndex))) {
        return false;
    }
    if (state.rampRemaining != 0) {
        const double nextRatio = state.ratio + state.rampStep;
        const long double linearEnd =
            static_cast<long double>(state.ratio) +
            static_cast<long double>(state.rampStep) *
                static_cast<long double>(state.rampRemaining - 1u);
        if (!std::isfinite(nextRatio) || nextRatio <= 0.0 ||
            !std::isfinite(linearEnd) || linearEnd <= 0.0) {
            return false;
        }
    }
    const std::size_t expectedUsedBytes =
        sizeof(ResamplerParkState) - sizeof(state.rawFrames) +
        static_cast<std::size_t>(state.channelCount) *
            state.rawFrameCount * sizeof(float);
    if (state.usedBytes != expectedUsedBytes ||
        expectedUsedBytes > sizeof(ResamplerParkState)) {
        return false;
    }
    for (std::uint32_t ch = 0; ch < state.channelCount; ++ch) {
        if (!std::isfinite(state.legacyHighRatioPrevious[ch]) ||
            !std::isfinite(state.legacyHighRatioCurrent[ch])) {
            return false;
        }
        for (const ResampleFilterSectionState& section :
             state.legacyHighRatioFilter[ch]) {
            if (!std::isfinite(section.z1) || !std::isfinite(section.z2))
                return false;
        }
    }
    return true;
}

bool Resampler::RestoreParkState(
    const ResamplerParkState& state,
    float* const* ringStorage, std::uint32_t channels,
    const ResampleTables* legacyTables,
    const MultirateResampleTables* multirateTables,
    MultirateResamplerCandidate* workspace) {
    if (state.profile == ResampleProfile::Professional44148) {
        return RestoreProfessional(state, ringStorage, channels,
                                   legacyTables, multirateTables, workspace);
    }
    if (!ValidateParkState(state) || ringStorage == nullptr ||
        legacyTables == nullptr || channels != state.channelCount) {
        return false;
    }
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        if (ringStorage[ch] == nullptr)
            return false;
    }

    Reset(ringStorage, channels, state.quality, state.ratio, legacyTables,
          ResampleProfile::LegacyCompatibility, multirateTables, nullptr);
    ratio = state.ratio;
    targetRatio = state.targetRatio;
    rampStep = state.rampStep;
    rampRemaining = state.rampRemaining;
    readIndex = state.legacyReadIndex;
    readFraction = state.legacyReadFraction;
    phaseHigh = state.legacyPhaseHigh;
    phaseLow = state.legacyPhaseLow;
    readPos = static_cast<double>(readIndex) + readFraction;
    writeIndex = state.legacyWriteIndex;
    inputEndIndex = state.legacyInputEndIndex;
    inputEnded = state.legacyInputEnded;
    std::memcpy(highRatioFilter, state.legacyHighRatioFilter,
                sizeof(highRatioFilter));
    std::memcpy(highRatioPrevious, state.legacyHighRatioPrevious,
                sizeof(highRatioPrevious));
    std::memcpy(highRatioCurrent, state.legacyHighRatioCurrent,
                sizeof(highRatioCurrent));
    highRatioFilterIndex = state.legacyHighRatioFilterIndex;
    highRatioFilterActive = state.legacyHighRatioFilterActive;
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        std::memset(ringStorage[ch], 0,
                    kResampleFifoFrames * sizeof(float));
        for (std::uint32_t frame = 0;
             frame < state.rawFrameCount; ++frame) {
            ringStorage[ch][
                (state.rawStartIndex + frame) & kRingMask] =
                state.rawFrames[ch][frame];
        }
    }
    return true;
}

} // namespace harken
