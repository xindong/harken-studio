// High-ratio decimation contract for the resample stage, driven directly.
//
// The stage must keep producing and eventually report exhausted at every
// contract ratio: a large ratio makes the read head jump past the write
// head, which is normal decimation (the skipped input frames are dropped
// samples), and the ring then has writable space beyond its fixed history
// reserve. The defect under test computes the occupied span in unsigned
// arithmetic, so a read head ahead of the write head underflows
// the subtraction to a huge value, pins FreeFrames at zero, and freezes
// both the feed and Produce with no path back — a permanent voice hang.
//
// Only ratios whose per-frame advance leaves the read head past the write
// head trip the underflow; ratios that divide the fixed feed chunk stay
// phase-locked to the write head (kept below as green guards). Every driver
// here carries an iteration budget: a deadlock surfaces as an assertion
// failure with the live ring state inside a finite budget, never as a hang.
#include "harken/dsp/Resampler.h"
#include "harken/render/StreamParkToken.h"

#include "Common/SignalTestSupport.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <utility>
#include <vector>

namespace {

using harken::kResampleChunkFrames;
using harken::kResampleFifoFrames;
using harken::kResampleHistoryPad;
using harken::kMultirateHalfbandTaps;
using harken::kMultirateMaxStages;
using harken::kMultirateMaximumEffectiveRatio;
using harken::kMultirateResidualBaseRatioRows;
using harken::kMultirateResidualMaximumRatio;
using harken::kMultirateResidualTaps;
using harken::MultirateResampleCounters;
using harken::MultirateCandidatePathState;
using harken::MultirateHalfbandStageState;
using harken::MultirateResamplerCandidate;
using harken::MultirateResampleTables;
using harken::PCMView;
using harken::Resampler;
using harken::ResampleProfile;
using harken::ResampleQuality;
using harken::ResampleTables;
using harken::SelectResampleProfile;

const ResampleTables* Tables() {
    static ResampleTables tables;
    static bool initialized = false;
    if (!initialized) {
        tables.Init();
        initialized = true;
    }
    return &tables;
}

const MultirateResampleTables* CandidateTables() {
    static MultirateResampleTables tables;
    static bool initialized = false;
    if (!initialized) {
        tables.Init();
        initialized = true;
    }
    return &tables;
}

struct CandidateDriveResult {
    std::vector<float> out;
    MultirateResampleCounters counters;
    std::uint32_t activeStages = 0;
    double ratio = 1.0;
    double targetRatio = 1.0;
    std::uint32_t rampRemaining = 0;
    bool budgetExceeded = false;
};

CandidateDriveResult DriveCandidate(
    double initialRatio, double targetRatio, std::uint32_t rampFrames,
    const std::vector<float>& input, std::size_t maxOut,
    std::uint32_t channels = 1,
    std::uint32_t feedFrames = kResampleChunkFrames,
    std::uint32_t produceFrames = kResampleChunkFrames,
    bool topologyTransitionsEnabled = true) {
    PCMView view;
    for (std::uint32_t ch = 0; ch < channels; ++ch)
        view.channels[ch] = input.data();
    view.channelCount = channels;
    view.stride = 1;
    view.sampleRate = 48000;
    view.frames = input.size();

    std::vector<float> ringStorage(
        static_cast<std::size_t>(channels) * kResampleFifoFrames, 0.0f);
    float* rings[harken::kMaxChannels] = {};
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        rings[ch] =
            ringStorage.data() +
            static_cast<std::size_t>(ch) * kResampleFifoFrames;
    }

    MultirateResamplerCandidate candidate;
    candidate.Reset(rings, channels, initialRatio, CandidateTables());
    candidate.SetTopologyTransitionsEnabledForTesting(
        topologyTransitionsEnabled);
    if (targetRatio != initialRatio)
        candidate.UpdateRatio(targetRatio, rampFrames);

    std::vector<std::vector<float>> channelOutput(
        channels, std::vector<float>(maxOut, 0.0f));
    float* destination[harken::kMaxChannels] = {};
    for (std::uint32_t ch = 0; ch < channels; ++ch)
        destination[ch] = channelOutput[ch].data();

    const std::size_t budget =
        8 * (input.size() / std::max(1u, feedFrames) + maxOut + 64);
    std::size_t iterations = 0;
    std::uint64_t cursor = 0;
    std::uint32_t produced = 0;
    while (produced < maxOut && ++iterations <= budget) {
        const std::uint32_t request = static_cast<std::uint32_t>(
            std::min<std::size_t>(produceFrames, maxOut - produced));
        produced +=
            candidate.ProduceInstrumentedForTesting(
                destination, produced, request);
        if (produced >= maxOut)
            break;
        if (cursor < view.frames) {
            const std::uint32_t chunk =
                static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(
                        {feedFrames, view.frames - cursor,
                         candidate.FreeFrames()}));
            if (chunk == 0)
                continue;
            candidate.PushPCM(view, cursor, chunk);
            cursor += chunk;
            if (cursor == view.frames)
                candidate.MarkInputEnd();
        } else {
            break;
        }
    }

    CandidateDriveResult result;
    result.out.assign(channelOutput[0].begin(),
                      channelOutput[0].begin() + produced);
    result.counters = candidate.counters;
    result.activeStages = candidate.activeStages;
    result.ratio = candidate.ratio;
    result.targetRatio = candidate.targetRatio;
    result.rampRemaining = candidate.rampRemaining;
    result.budgetExceeded = iterations > budget;
    return result;
}

CandidateDriveResult DriveCandidate(
    double ratio, const std::vector<float>& input,
    std::size_t maxOut, std::uint32_t channels = 1,
    std::uint32_t feedFrames = kResampleChunkFrames,
    std::uint32_t produceFrames = kResampleChunkFrames,
    bool topologyTransitionsEnabled = true) {
    return DriveCandidate(ratio, ratio, 0, input, maxOut, channels,
                          feedFrames, produceFrames,
                          topologyTransitionsEnabled);
}

CandidateDriveResult RunCandidateTone(
    double initialRatio, double targetRatio, std::uint32_t rampFrames,
    double sourceFrequency, std::size_t outputFrames,
    std::uint32_t channels = 1,
    std::uint32_t feedFrames = kResampleChunkFrames,
    std::uint32_t produceFrames = kResampleChunkFrames,
    bool topologyTransitionsEnabled = true) {
    const double maximumRatio =
        std::max(initialRatio, targetRatio);
    const std::size_t inputFrames =
        static_cast<std::size_t>(
            std::ceil(static_cast<double>(outputFrames + 128) *
                      std::max(1.0, maximumRatio))) +
        4096;
    const std::vector<float> input =
        harkentest::MakeSine(inputFrames, sourceFrequency, 48000.0,
                            0.8f);
    return DriveCandidate(initialRatio, targetRatio, rampFrames, input,
                          outputFrames, channels, feedFrames,
                          produceFrames,
                          topologyTransitionsEnabled);
}

CandidateDriveResult RunCandidateRepeatedRatioCrossings(
    double low, double high,
    bool topologyTransitionsEnabled = true) {
    constexpr std::uint32_t kSegmentFrames = 512;
    constexpr std::uint32_t kRampFrames = 256;
    constexpr std::size_t kSegmentCount = 4;
    constexpr std::size_t kOutputFrames =
        kSegmentFrames * kSegmentCount;
    const std::size_t inputFrames =
        static_cast<std::size_t>(
            std::ceil(static_cast<double>(kOutputFrames + 128) *
                      high)) +
        8192;
    const std::vector<float> input =
        harkentest::MakeSine(inputFrames, 120.0, 48000.0, 0.8f);

    PCMView view;
    view.channels[0] = input.data();
    view.channelCount = 1;
    view.stride = 1;
    view.sampleRate = 48000;
    view.frames = input.size();
    std::vector<float> ring(kResampleFifoFrames, 0.0f);
    float* rings[1] = {ring.data()};
    MultirateResamplerCandidate candidate;
    candidate.Reset(rings, 1, low, CandidateTables());
    candidate.SetTopologyTransitionsEnabledForTesting(
        topologyTransitionsEnabled);

    std::vector<float> output(kOutputFrames, 0.0f);
    float* destination[1] = {output.data()};
    std::uint64_t cursor = 0;
    std::uint32_t produced = 0;
    std::size_t iterations = 0;
    constexpr std::size_t kBudget = 200000;
    for (std::size_t segment = 0; segment < kSegmentCount; ++segment) {
        const double target =
            (segment & 1u) == 0 ? high : low;
        candidate.UpdateRatio(target, kRampFrames);
        const std::uint32_t segmentEnd =
            static_cast<std::uint32_t>(
                (segment + 1u) * kSegmentFrames);
        while (produced < segmentEnd && ++iterations <= kBudget) {
            produced += candidate.ProduceInstrumentedForTesting(
                destination, produced, segmentEnd - produced);
            if (produced >= segmentEnd)
                break;
            const std::uint32_t chunk =
                static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(
                        {kResampleChunkFrames,
                         view.frames - cursor,
                         candidate.FreeFrames()}));
            if (chunk == 0)
                continue;
            candidate.PushPCM(view, cursor, chunk);
            cursor += chunk;
        }
    }

    CandidateDriveResult result;
    output.resize(produced);
    result.out.swap(output);
    result.counters = candidate.counters;
    result.activeStages = candidate.activeStages;
    result.ratio = candidate.ratio;
    result.targetRatio = candidate.targetRatio;
    result.rampRemaining = candidate.rampRemaining;
    result.budgetExceeded = iterations > kBudget;
    return result;
}

CandidateDriveResult RunCandidateRepeatedBoundaryCrossings(
    double boundary, bool topologyTransitionsEnabled = true) {
    return RunCandidateRepeatedRatioCrossings(
        boundary * 0.98, boundary * 1.02,
        topologyTransitionsEnabled);
}

double CandidateProjectedAmplitude(const std::vector<float>& samples,
                                   std::size_t start,
                                   std::size_t frames,
                                   double cyclesPerSample) {
    constexpr double kCandidateTwoPi = 6.28318530717958647692;
    double real = 0.0;
    double imaginary = 0.0;
    for (std::size_t i = 0; i < frames; ++i) {
        const double phase =
            kCandidateTwoPi * cyclesPerSample *
            static_cast<double>(i);
        const double sample =
            static_cast<double>(samples[start + i]);
        real += sample * std::cos(phase);
        imaginary -= sample * std::sin(phase);
    }
    return 2.0 * std::sqrt(real * real + imaginary * imaginary) /
           static_cast<double>(frames);
}

struct CandidateStepPeak {
    double value = 0.0;
    std::size_t frame = 0;
};

CandidateStepPeak CandidateMaximumStep(
    const std::vector<float>& samples,
    std::size_t start = 1,
    std::size_t end = (std::numeric_limits<std::size_t>::max)()) {
    CandidateStepPeak peak;
    const std::size_t boundedEnd =
        std::min(end, samples.size());
    const std::size_t boundedStart =
        std::max<std::size_t>(1u, start);
    for (std::size_t i = boundedStart; i < boundedEnd; ++i) {
        const double step =
            std::abs(static_cast<double>(samples[i]) -
                     static_cast<double>(samples[i - 1u]));
        if (step > peak.value) {
            peak.value = step;
            peak.frame = i;
        }
    }
    return peak;
}

double CandidateMaximumWindowEnergyDifference(
    const std::vector<float>& candidate,
    const std::vector<float>& reference,
    std::size_t start, std::size_t windowFrames) {
    const std::size_t count =
        std::min(candidate.size(), reference.size());
    double maximum = 0.0;
    for (std::size_t windowStart = start;
         windowStart + windowFrames <= count;
         windowStart += windowFrames) {
        double candidateEnergy = 0.0;
        double referenceEnergy = 0.0;
        for (std::size_t i = 0; i < windowFrames; ++i) {
            const double candidateSample =
                candidate[windowStart + i];
            const double referenceSample =
                reference[windowStart + i];
            candidateEnergy += candidateSample * candidateSample;
            referenceEnergy += referenceSample * referenceSample;
        }
        maximum = std::max(
            maximum,
            std::abs(candidateEnergy - referenceEnergy) /
                static_cast<double>(windowFrames));
    }
    return maximum;
}

std::uint32_t CandidateSignificantImpulseClusters(
    const std::vector<float>& samples, float threshold,
    std::size_t quietFrames) {
    std::uint32_t clusters = 0;
    std::size_t quiet = quietFrames;
    for (const float sample : samples) {
        if (std::abs(sample) >= threshold) {
            if (quiet >= quietFrames)
                ++clusters;
            quiet = 0;
        } else if (quiet < quietFrames) {
            ++quiet;
        }
    }
    return clusters;
}

struct DriveResult {
    std::vector<float> out;
    bool exhausted = false;
    bool budgetExceeded = false;
};

// Drives the stage exactly as the cascade's FeedResampler does (produce
// until starved, push a fixed chunk or flush look-ahead silence, repeat)
// but under a hard iteration budget. A deadlock — Produce and the feed
// both wedged at zero while neither exhausted nor filling — is caught by
// the budget and reported with the live ring state, so it can never hang
// the suite. The budget is four times a generous healthy-run estimate
// (one feed per chunk of input plus one produce per output frame, with
// flush headroom), far above any legitimate run yet finite.
DriveResult DriveResampler(ResampleQuality quality, double ratio,
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

    DriveResult result;
    std::uint64_t cursor = 0;
    std::uint32_t produced = 0;
    while (produced < maxOut) {
        if (++iterations > budget) {
            result.budgetExceeded = true;
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
    result.out.swap(out);
    result.exhausted = rs.Exhausted();
    if (result.budgetExceeded) {
        ADD_FAILURE() << "resample stage made no progress within " << budget
                      << " iterations at ratio " << ratio
                      << ": readPos=" << rs.readPos
                      << " writeIndex=" << rs.writeIndex
                      << " FreeFrames=" << rs.FreeFrames()
                      << " produced=" << produced << " cursor=" << cursor;
    }
    return result;
}

struct RatioCase {
    double ratio;
    const char* name;
};

enum class BoundarySignal {
    Sine,
    Sweep,
    Transient,
};

struct BoundarySignalCase {
    BoundarySignal signal;
    const char* name;
};

const BoundarySignalCase kBoundarySignals[] = {
    {BoundarySignal::Sine, "sine"},
    {BoundarySignal::Sweep, "frequency sweep"},
    {BoundarySignal::Transient, "transient"},
};

constexpr double kTwoPi = 6.28318530717958647692;
constexpr double kDirectKernelBoundary = 16.0;

double ReadPositionBeforeOutput(double initialRatio, double targetRatio,
                                std::uint32_t rampFrames,
                                std::uint32_t outputIndex) {
    const double rampStep =
        (targetRatio - initialRatio) / static_cast<double>(rampFrames);
    const std::uint32_t rampedFrames = std::min(outputIndex, rampFrames);
    const double rampedAdvance =
        static_cast<double>(rampedFrames) * initialRatio +
        rampStep * static_cast<double>(rampedFrames) *
            static_cast<double>(rampedFrames + 1u) / 2.0;
    const double targetAdvance =
        static_cast<double>(outputIndex - rampedFrames) * targetRatio;
    return static_cast<double>(kResampleHistoryPad) + rampedAdvance +
           targetAdvance;
}

double InputFrameBeforeOutput(double initialRatio, double targetRatio,
                              std::uint32_t rampFrames,
                              std::uint32_t outputIndex) {
    return ReadPositionBeforeOutput(initialRatio, targetRatio, rampFrames,
                                    outputIndex) -
           static_cast<double>(kResampleHistoryPad);
}

std::uint32_t BoundaryCrossingOutputFrame(double initialRatio,
                                          double targetRatio,
                                          std::uint32_t rampFrames) {
    const double rampStep =
        (targetRatio - initialRatio) / static_cast<double>(rampFrames);
    const bool upward = targetRatio > initialRatio;
    for (std::uint32_t i = 0; i < rampFrames; ++i) {
        const double ratio =
            initialRatio + rampStep * static_cast<double>(i + 1u);
        if ((upward && ratio > kDirectKernelBoundary) ||
            (!upward && ratio <= kDirectKernelBoundary)) {
            return i;
        }
    }
    return rampFrames;
}

std::vector<float> MakeBoundarySignal(BoundarySignal signal,
                                      std::size_t frames,
                                      double centerFrame) {
    std::vector<float> input(frames, 0.0f);
    for (std::size_t i = 0; i < frames; ++i) {
        const double relative = static_cast<double>(i) - centerFrame;
        switch (signal) {
            case BoundarySignal::Sine: {
                constexpr double kFrequency = 300.0;
                input[i] = static_cast<float>(
                    0.5 * std::sin(kTwoPi * kFrequency * relative / 48000.0 +
                                   0.37));
                break;
            }
            case BoundarySignal::Sweep: {
                constexpr double kCenterFrequency = 350.0;
                constexpr double kHzPerSourceFrame = 0.12;
                const double cycles =
                    kCenterFrequency * relative / 48000.0 +
                    0.5 * kHzPerSourceFrame * relative * relative / 48000.0;
                input[i] =
                    static_cast<float>(0.5 * std::sin(kTwoPi * cycles + 0.37));
                break;
            }
            case BoundarySignal::Transient: {
                constexpr double kWidthFrames = 40.0;
                const double normalized = relative / kWidthFrames;
                input[i] =
                    static_cast<float>(0.8 * std::exp(-0.5 * normalized *
                                                     normalized));
                break;
            }
        }
    }
    return input;
}

struct RatioRampRun {
    std::vector<float> out;
    std::vector<double> frameRatios;
};

RatioRampRun RunRatioRamp(double initialRatio, double targetRatio,
                          std::uint32_t rampFrames,
                          const std::vector<float>& input,
                          std::size_t outputFrames) {
    PCMView view;
    view.channels[0] = input.data();
    view.channelCount = 1;
    view.stride = 1;
    view.sampleRate = 48000;
    view.frames = input.size();

    std::vector<float> ring(kResampleFifoFrames, 0.0f);
    float* rings[1] = {ring.data()};
    Resampler rs;
    rs.Reset(rings, 1, ResampleQuality::Sinc16, initialRatio, Tables());
    rs.UpdateRatio(targetRatio, rampFrames);

    RatioRampRun run;
    run.out.reserve(outputFrames);
    run.frameRatios.reserve(outputFrames);
    std::uint64_t cursor = 0;
    const std::size_t budget =
        4 * (input.size() / kResampleChunkFrames + outputFrames + 16);
    std::size_t iterations = 0;
    while (run.out.size() < outputFrames && ++iterations <= budget) {
        float sample = 0.0f;
        float* dst[1] = {&sample};
        if (rs.Produce(dst, 0, 1) == 1u) {
            run.out.push_back(sample);
            run.frameRatios.push_back(rs.ratio);
            continue;
        }

        if (cursor >= view.frames) {
            ADD_FAILURE() << "ratio-ramp input exhausted after "
                          << run.out.size() << " output frames";
            break;
        }
        const std::uint32_t chunk = static_cast<std::uint32_t>(
            std::min<std::uint64_t>({kResampleChunkFrames,
                                     view.frames - cursor,
                                     rs.FreeFrames()}));
        if (chunk == 0u) {
            ADD_FAILURE() << "ratio-ramp driver made no progress at ratio "
                          << rs.ratio << " with readPos=" << rs.readPos
                          << " writeIndex=" << rs.writeIndex;
            break;
        }
        rs.PushPCM(view, cursor, chunk);
        cursor += chunk;
    }
    if (iterations > budget) {
        ADD_FAILURE() << "ratio-ramp driver exceeded its iteration budget";
    }
    return run;
}

std::size_t FindBoundaryFrame(const std::vector<double>& frameRatios,
                              bool upward) {
    for (std::size_t i = 1; i < frameRatios.size(); ++i) {
        if (upward &&
            frameRatios[i - 1u] <= kDirectKernelBoundary &&
            frameRatios[i] > kDirectKernelBoundary) {
            return i;
        }
        if (!upward &&
            frameRatios[i - 1u] > kDirectKernelBoundary &&
            frameRatios[i] <= kDirectKernelBoundary) {
            return i;
        }
    }
    return frameRatios.size();
}

struct TransitionMetrics {
    double maximumStep = 0.0;
    double preEnergy = 0.0;
    double postEnergy = 0.0;
    double energyJump = 0.0;
};

TransitionMetrics MeasureTransition(const std::vector<float>& out,
                                    std::size_t boundaryFrame,
                                    std::size_t energyWindowFrames) {
    TransitionMetrics metrics;
    metrics.maximumStep = std::max(
        std::abs(static_cast<double>(out[boundaryFrame]) -
                 static_cast<double>(out[boundaryFrame - 1u])),
        std::abs(static_cast<double>(out[boundaryFrame + 1u]) -
                 static_cast<double>(out[boundaryFrame])));
    for (std::size_t i = 0; i < energyWindowFrames; ++i) {
        const double pre =
            static_cast<double>(out[boundaryFrame - energyWindowFrames + i]);
        const double post = static_cast<double>(out[boundaryFrame + i]);
        metrics.preEnergy += pre * pre;
        metrics.postEnergy += post * post;
    }
    metrics.preEnergy /= static_cast<double>(energyWindowFrames);
    metrics.postEnergy /= static_cast<double>(energyWindowFrames);
    metrics.energyJump = std::abs(metrics.postEnergy - metrics.preEnergy);
    return metrics;
}

void ExpectFinite(const RatioRampRun& run, const char* runName) {
    for (std::size_t i = 0; i < run.out.size(); ++i) {
        EXPECT_TRUE(std::isfinite(run.out[i]))
            << runName << " produced a non-finite sample at output " << i;
    }
}

struct CrossingDirection {
    double initialRatio;
    double targetRatio;
    double directReferenceInitial;
    double directReferenceTarget;
    double highReferenceInitial;
    double highReferenceTarget;
    bool upward;
    const char* name;
};

void ExpectSixteenCrossingBoundedStep(const CrossingDirection& direction) {
    constexpr std::uint32_t kRampFrames = 96;
    constexpr std::size_t kOutputFrames = 128;
    constexpr std::size_t kInputFrames = 4096;
    constexpr std::size_t kEnergyWindowFrames = 8;
    // The direct and streaming kernels have a known group-delay mismatch at
    // the 16x boundary. Keep the current discontinuity bounded while a
    // future transition-smoothing change tightens this allowance.
    constexpr double kStepMargin = 6.0;
    constexpr double kEnergyMargin = 2.0;
    const std::uint32_t crossingOutputFrame = BoundaryCrossingOutputFrame(
        direction.initialRatio, direction.targetRatio, kRampFrames);
    ASSERT_LT(crossingOutputFrame, kRampFrames);

    for (const BoundarySignalCase& signalCase : kBoundarySignals) {
        SCOPED_TRACE(direction.name);
        SCOPED_TRACE(signalCase.name);

        const std::vector<float> crossingInput = MakeBoundarySignal(
            signalCase.signal, kInputFrames,
            InputFrameBeforeOutput(direction.initialRatio,
                                   direction.targetRatio, kRampFrames,
                                   crossingOutputFrame));
        const std::vector<float> directReferenceInput = MakeBoundarySignal(
            signalCase.signal, kInputFrames,
            InputFrameBeforeOutput(direction.directReferenceInitial,
                                   direction.directReferenceTarget,
                                   kRampFrames, crossingOutputFrame));
        const std::vector<float> highReferenceInput = MakeBoundarySignal(
            signalCase.signal, kInputFrames,
            InputFrameBeforeOutput(direction.highReferenceInitial,
                                   direction.highReferenceTarget, kRampFrames,
                                   crossingOutputFrame));

        const RatioRampRun crossing = RunRatioRamp(
            direction.initialRatio, direction.targetRatio, kRampFrames,
            crossingInput, kOutputFrames);
        const RatioRampRun directReference = RunRatioRamp(
            direction.directReferenceInitial,
            direction.directReferenceTarget, kRampFrames,
            directReferenceInput, kOutputFrames);
        const RatioRampRun highReference = RunRatioRamp(
            direction.highReferenceInitial, direction.highReferenceTarget,
            kRampFrames, highReferenceInput, kOutputFrames);

        ASSERT_EQ(kOutputFrames, crossing.out.size());
        ASSERT_EQ(kOutputFrames, directReference.out.size());
        ASSERT_EQ(kOutputFrames, highReference.out.size());
        ASSERT_EQ(crossing.out.size(), crossing.frameRatios.size());
        ASSERT_EQ(directReference.out.size(),
                  directReference.frameRatios.size());
        ASSERT_EQ(highReference.out.size(),
                  highReference.frameRatios.size());
        ASSERT_TRUE(std::all_of(
            directReference.frameRatios.begin(),
            directReference.frameRatios.end(),
            [](double ratio) { return ratio <= kDirectKernelBoundary; }));
        ASSERT_TRUE(std::all_of(
            highReference.frameRatios.begin(),
            highReference.frameRatios.end(),
            [](double ratio) { return ratio > kDirectKernelBoundary; }));
        ExpectFinite(crossing, "boundary-crossing ramp");
        ExpectFinite(directReference, "direct-kernel reference ramp");
        ExpectFinite(highReference, "streaming-filter reference ramp");

        const std::size_t boundaryFrame =
            FindBoundaryFrame(crossing.frameRatios, direction.upward);
        ASSERT_NE(crossing.frameRatios.size(), boundaryFrame)
            << "ratio ramp did not cross the 16x kernel boundary";
        ASSERT_GE(boundaryFrame, kEnergyWindowFrames);
        ASSERT_LT(boundaryFrame + kEnergyWindowFrames, kOutputFrames);

        const TransitionMetrics crossingMetrics = MeasureTransition(
            crossing.out, boundaryFrame, kEnergyWindowFrames);
        const TransitionMetrics directMetrics = MeasureTransition(
            directReference.out, boundaryFrame, kEnergyWindowFrames);
        const TransitionMetrics highMetrics = MeasureTransition(
            highReference.out, boundaryFrame, kEnergyWindowFrames);

        // Characterize the known, bounded discontinuity caused by switching
        // between kernels with different group delay at the 16x boundary.
        // The six-times allowance is intentionally an observed-current-state
        // bound; tighten it when transition smoothing is implemented.
        const double referenceStep = std::max(
            directMetrics.maximumStep, highMetrics.maximumStep);
        const double stepRounding =
            std::numeric_limits<float>::epsilon() *
            std::max(1.0, referenceStep);
        EXPECT_LE(crossingMetrics.maximumStep,
                  referenceStep * kStepMargin + stepRounding);

        // Compare adjacent eight-frame mean-square windows. One window sample
        // worth of the stable-domain energy is the minimum meaningful
        // reference for a localized transient; the two-times margin admits
        // normal kernel phase differences while rejecting a boundary-only
        // energy burst or collapse.
        const double referenceEnergyScale = std::max(
            {directMetrics.preEnergy, directMetrics.postEnergy,
             highMetrics.preEnergy, highMetrics.postEnergy});
        const double referenceEnergyJump = std::max(
            {directMetrics.energyJump, highMetrics.energyJump,
             referenceEnergyScale /
                 static_cast<double>(kEnergyWindowFrames)});
        const double energyRounding =
            std::numeric_limits<float>::epsilon() *
            std::max(1.0, referenceEnergyScale);
        EXPECT_LE(crossingMetrics.energyJump,
                  referenceEnergyJump * kEnergyMargin + energyRounding);
    }
}

// Decimation ratios whose per-frame advance overshoots the available input:
// the read head lands past the write head and the former FreeFrames
// underflow wedges the stage. The 100:1 case sits above the contract pitch
// ceiling (combined ratio can exceed the caller pitch via the source-rate
// factor), so the contract must hold there too. Registered PCM accepts every
// nonzero uint32 sample rate and an explicit engine rate may be 1 Hz, making
// 64 * UINT32_MAX the reachable combined-ratio ceiling.
constexpr double kMaxCombinedRatio =
    64.0 * static_cast<double>(std::numeric_limits<std::uint32_t>::max());
const RatioCase kDecimationRatios[] = {
    {20.0, "20:1"},   {30.0, "30:1"}, {50.0, "50:1"},
    {60.0, "60:1"},   {100.0, "100:1"},
    {kMaxCombinedRatio, "maximum combined ratio"},
};

// Phase-locked guards: a ratio that divides the fixed feed chunk advances
// the read head to land exactly on the write head every cycle, so the
// underflow cannot arise. They pin that the fix leaves this already-correct
// family untouched.
const RatioCase kPhaseLockedGuards[] = {
    {16.0, "16:1"},
    {64.0, "64:1"},
};

struct CandidateRatePair {
    double sourceRate;
    double mixRate;
};

constexpr CandidateRatePair kCandidateProfessionalRatePairs[] = {
    {44100.0, 44100.0},
    {44100.0, 48000.0},
    {48000.0, 44100.0},
    {48000.0, 48000.0},
};

constexpr double kCandidateMinimumProfessionalRatio =
    (44100.0 / 48000.0) / 16.0;
constexpr double kCandidateMaximumProfessionalRatio =
    (48000.0 / 44100.0) * 16.0;
constexpr double kCandidateMaximumPublicRatio =
    kMultirateMaximumEffectiveRatio;

double CandidateEffectiveRatio(const CandidateRatePair& rates,
                               int semitones) {
    return (rates.sourceRate / rates.mixRate) *
           std::pow(2.0, static_cast<double>(semitones) / 12.0);
}

TEST(MultirateResamplerCandidate, FixedStateAndSharedTablesStayInBudget) {
    EXPECT_GT(sizeof(MultirateResamplerCandidate), 8u * 1024u);
    EXPECT_LE(sizeof(MultirateResamplerCandidate), 32u * 1024u);
    EXPECT_LE(sizeof(harken::ProfessionalResamplerState),
              32u * 1024u);
    EXPECT_LE(sizeof(harken::StreamParkToken),
              harken::kStreamParkTokenBytes);
    EXPECT_LE(sizeof(MultirateResampleTables), 256u * 1024u);
    EXPECT_EQ(5u, kMultirateMaxStages);
    EXPECT_DOUBLE_EQ(
        kCandidateMaximumPublicRatio / 32.0,
        kMultirateResidualMaximumRatio);
    EXPECT_EQ(8u, kMultirateResidualBaseRatioRows);
    EXPECT_EQ(9u, harken::kMultirateResidualRatioRows);
    EXPECT_EQ(43u, kMultirateHalfbandTaps);
    EXPECT_EQ(16u, kMultirateResidualTaps);
}

TEST(MultirateResamplerCandidate,
     RatioGatingKeepsResidualAndStructuralWorkBounded) {
    const double ratios[] = {
        kCandidateMinimumProfessionalRatio,
        0.5, 1.0, 1.999, 2.0, 3.999,
        4.0, 7.999, 8.0, 15.999, 16.0,
        31.999, 32.0, 63.999, 64.0,
        kCandidateMaximumProfessionalRatio,
        kCandidateMaximumPublicRatio,
    };
    for (const double ratio : ratios) {
        SCOPED_TRACE(ratio);
        const CandidateDriveResult result =
            RunCandidateTone(ratio, ratio, 0, 300.0, 64, 8);
        ASSERT_FALSE(result.budgetExceeded);
        ASSERT_EQ(64u, result.out.size());
        EXPECT_EQ(
            MultirateResamplerCandidate::ResolveStageCount(ratio),
            result.activeStages);
        EXPECT_LE(result.activeStages, kMultirateMaxStages);
        EXPECT_EQ(
            result.counters.halfbandMultiplyAccumulates,
            result.counters.halfbandOutputFrames * 8u * 12u);
        EXPECT_EQ(
            result.counters.residualMultiplyAccumulates,
            result.counters.residualOutputFrames * 8u *
                kMultirateResidualTaps);
        for (const float sample : result.out)
            EXPECT_TRUE(std::isfinite(sample));
    }
}

TEST(MultirateResamplerCandidate,
     StructuralWorkImprovesOnCurrentRatioScaledPath) {
    constexpr double kDirectRatios[] = {
        2.0, 4.0, 8.0, 16.0, 32.0, 64.0,
        kCandidateMaximumProfessionalRatio,
        kCandidateMaximumPublicRatio,
    };
    constexpr std::size_t kOutputFrames = 4096;
    for (const double ratio : kDirectRatios) {
        SCOPED_TRACE(ratio);
        const CandidateDriveResult result =
            RunCandidateTone(ratio, ratio, 0, 220.0,
                             kOutputFrames);
        ASSERT_FALSE(result.budgetExceeded);
        ASSERT_EQ(kOutputFrames, result.out.size());
        const std::uint64_t currentNeed =
            static_cast<std::uint64_t>(
                std::ceil(7.5 * ratio)) +
            1u;
        const std::uint64_t currentApplyWork =
            (2u * currentNeed + 1u) * kOutputFrames;
        const std::uint64_t candidateApplyWork =
            result.counters.halfbandMultiplyAccumulates +
            result.counters.residualMultiplyAccumulates;
        EXPECT_LT(candidateApplyWork, currentApplyWork)
            << "candidate no longer improves the current Sinc16 "
               "ratio-scaled apply count at "
            << ratio << "x";
    }
}

TEST(MultirateResamplerCandidate,
     ProfessionalRatePairsCoverEveryAuthoredPitchSemitone) {
    double observedMinimum = std::numeric_limits<double>::max();
    double observedMaximum = 0.0;
    for (const CandidateRatePair& rates :
         kCandidateProfessionalRatePairs) {
        for (int semitones = -48; semitones <= 48; ++semitones) {
            const double effectiveRatio =
                CandidateEffectiveRatio(rates, semitones);
            observedMinimum =
                std::min(observedMinimum, effectiveRatio);
            observedMaximum =
                std::max(observedMaximum, effectiveRatio);
            SCOPED_TRACE(
                ::testing::Message()
                << "source rate " << rates.sourceRate
                << " mix rate " << rates.mixRate
                << " semitones " << semitones
                << " effective ratio " << effectiveRatio);
            EXPECT_GE(effectiveRatio,
                      kCandidateMinimumProfessionalRatio);
            EXPECT_LE(effectiveRatio,
                      kCandidateMaximumProfessionalRatio);
            const CandidateDriveResult result =
                RunCandidateTone(effectiveRatio, effectiveRatio, 0,
                                 220.0, 16);
            ASSERT_FALSE(result.budgetExceeded);
            ASSERT_EQ(16u, result.out.size());
            EXPECT_LE(result.counters.maximumActiveStages, 4u);
            for (const float sample : result.out)
                EXPECT_TRUE(std::isfinite(sample));
        }
    }
    EXPECT_DOUBLE_EQ(kCandidateMinimumProfessionalRatio,
                     (44100.0 / 48000.0) / 16.0);
    EXPECT_DOUBLE_EQ(kCandidateMaximumProfessionalRatio,
                     (48000.0 / 44100.0) * 16.0);
    EXPECT_DOUBLE_EQ(kCandidateMinimumProfessionalRatio,
                     observedMinimum);
    EXPECT_DOUBLE_EQ(kCandidateMaximumProfessionalRatio,
                     observedMaximum);
    EXPECT_EQ(0u,
              MultirateResamplerCandidate::ResolveStageCount(
                  kCandidateMinimumProfessionalRatio));
    EXPECT_EQ(4u,
              MultirateResamplerCandidate::ResolveStageCount(
                  kCandidateMaximumProfessionalRatio));
}

TEST(MultirateResamplerCandidate,
     RateCompensatedPitchProducesUnityEffectiveRatio) {
    for (const CandidateRatePair& rates :
         kCandidateProfessionalRatePairs) {
        SCOPED_TRACE(
            ::testing::Message()
            << rates.sourceRate << " to " << rates.mixRate);
        const double rateCompensatingPitch =
            rates.mixRate / rates.sourceRate;
        const double effectiveRatio =
            (rates.sourceRate / rates.mixRate) *
            rateCompensatingPitch;
        EXPECT_NEAR(1.0, effectiveRatio,
                    std::numeric_limits<double>::epsilon());
        const CandidateDriveResult result =
            RunCandidateTone(effectiveRatio, effectiveRatio, 0,
                             440.0, 128);
        ASSERT_FALSE(result.budgetExceeded);
        ASSERT_EQ(128u, result.out.size());
        EXPECT_EQ(0u, result.activeStages);
        for (const float sample : result.out)
            EXPECT_TRUE(std::isfinite(sample));
    }
}

TEST(MultirateResamplerCandidate,
     FixedResidualAndHalfbandStagesRejectFoldedStopbandTone) {
    constexpr double kRatios[] = {
        2.0, 4.0, 8.0, 16.0, 32.0, 64.0,
        kCandidateMaximumProfessionalRatio,
        kCandidateMaximumPublicRatio,
    };
    constexpr std::size_t kOutputFrames = 2048;
    constexpr std::size_t kAnalysisStart = 512;
    constexpr std::size_t kAnalysisFrames = 1024;
    constexpr double kUnfoldedOutputCycles = 0.65;
    constexpr double kFoldedOutputCycles = 0.35;
    for (const double ratio : kRatios) {
        SCOPED_TRACE(ratio);
        const double sourceFrequency =
            kUnfoldedOutputCycles * 48000.0 / ratio;
        const CandidateDriveResult result =
            RunCandidateTone(ratio, ratio, 0, sourceFrequency,
                             kOutputFrames);
        ASSERT_FALSE(result.budgetExceeded);
        ASSERT_EQ(kOutputFrames, result.out.size());
        const double aliasAmplitude = CandidateProjectedAmplitude(
            result.out, kAnalysisStart, kAnalysisFrames,
            kFoldedOutputCycles);
        EXPECT_LT(aliasAmplitude, 0.8 * 0.001)
            << "half-band stopband has not reached the -60 dB "
               "candidate contract";
    }
}

TEST(MultirateResamplerCandidate,
     FixedResidualAndHalfbandStagesPreserveLowPassbandTone) {
    constexpr double kRatios[] = {
        0.5, 1.0, 1.5, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0,
        kCandidateMaximumProfessionalRatio,
        kCandidateMaximumPublicRatio,
    };
    constexpr std::size_t kOutputFrames = 2048;
    constexpr std::size_t kAnalysisStart = 512;
    constexpr std::size_t kAnalysisFrames = 1024;
    constexpr double kOutputCycles = 0.1;
    for (const double ratio : kRatios) {
        SCOPED_TRACE(ratio);
        const double sourceFrequency =
            kOutputCycles * 48000.0 / ratio;
        const CandidateDriveResult result =
            RunCandidateTone(ratio, ratio, 0, sourceFrequency,
                             kOutputFrames);
        ASSERT_FALSE(result.budgetExceeded);
        ASSERT_EQ(kOutputFrames, result.out.size());
        const double amplitude = CandidateProjectedAmplitude(
            result.out, kAnalysisStart, kAnalysisFrames,
            kOutputCycles);
        EXPECT_GT(amplitude, 0.6);
        EXPECT_LT(amplitude, 0.9);
    }
}

TEST(MultirateResamplerCandidate,
     StaticRatioIsIndependentOfFeedAndOutputPartitioning) {
    constexpr double kRatio = 15.75;
    constexpr std::size_t kOutputFrames = 512;
    const CandidateDriveResult regular =
        RunCandidateTone(kRatio, kRatio, 0, 440.0, kOutputFrames,
                         2, 128, 128);
    const CandidateDriveResult fragmented =
        RunCandidateTone(kRatio, kRatio, 0, 440.0, kOutputFrames,
                         2, 17, 7);
    ASSERT_FALSE(regular.budgetExceeded);
    ASSERT_FALSE(fragmented.budgetExceeded);
    ASSERT_EQ(regular.out.size(), fragmented.out.size());
    for (std::size_t i = 0; i < regular.out.size(); ++i)
        EXPECT_FLOAT_EQ(regular.out[i], fragmented.out[i]);
    EXPECT_EQ(regular.counters.rawFramesRead,
              fragmented.counters.rawFramesRead);
    EXPECT_EQ(regular.counters.halfbandMultiplyAccumulates,
              fragmented.counters.halfbandMultiplyAccumulates);
    EXPECT_EQ(regular.counters.residualMultiplyAccumulates,
              fragmented.counters.residualMultiplyAccumulates);
}

TEST(MultirateResamplerCandidate,
     EveryProfessionalBoundaryIsIndependentOfPartitioning) {
    constexpr double kBoundaries[] = {
        1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0,
    };
    constexpr std::uint32_t kRampFrames = 768;
    constexpr std::size_t kOutputFrames = 1024;
    for (const double boundary : kBoundaries) {
        for (const bool upward : {false, true}) {
            const double low = boundary * 0.98;
            const double high = boundary * 1.02;
            const double initial = upward ? low : high;
            const double target = upward ? high : low;
            SCOPED_TRACE(
                ::testing::Message()
                << (upward ? "upward " : "downward ")
                << boundary);
            const CandidateDriveResult regular =
                RunCandidateTone(initial, target, kRampFrames,
                                 220.0, kOutputFrames,
                                 2, 128, 128);
            const CandidateDriveResult fragmented =
                RunCandidateTone(initial, target, kRampFrames,
                                 220.0, kOutputFrames,
                                 2, 17, 7);
            ASSERT_FALSE(regular.budgetExceeded);
            ASSERT_FALSE(fragmented.budgetExceeded);
            ASSERT_EQ(regular.out.size(), fragmented.out.size());
            for (std::size_t i = 0; i < regular.out.size(); ++i)
                EXPECT_FLOAT_EQ(regular.out[i], fragmented.out[i]);
            EXPECT_EQ(regular.counters.rawFramesRead,
                      fragmented.counters.rawFramesRead);
            EXPECT_EQ(
                regular.counters.halfbandMultiplyAccumulates,
                fragmented.counters.halfbandMultiplyAccumulates);
            EXPECT_EQ(
                regular.counters.residualMultiplyAccumulates,
                fragmented.counters.residualMultiplyAccumulates);
        }
    }
}

TEST(MultirateResamplerCandidate,
     EveryProfessionalBoundaryRampsContinuouslyInBothDirections) {
    constexpr double kBoundaries[] = {
        1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0,
    };
    constexpr std::uint32_t kRampFrames = 512;
    constexpr std::size_t kOutputFrames = 1024;
    for (const double boundary : kBoundaries) {
        for (const bool upward : {false, true}) {
            const double low = boundary * 0.98;
            const double high = boundary * 1.02;
            const double initial = upward ? low : high;
            const double target = upward ? high : low;
            SCOPED_TRACE(
                ::testing::Message()
                << (upward ? "upward " : "downward ")
                << boundary);
            const CandidateDriveResult result =
                RunCandidateTone(initial, target, kRampFrames, 120.0,
                                 kOutputFrames);
            const CandidateDriveResult reference =
                RunCandidateTone(initial, target, kRampFrames, 120.0,
                                 kOutputFrames, 1, 128, 128, false);
            ASSERT_FALSE(result.budgetExceeded);
            ASSERT_FALSE(reference.budgetExceeded);
            ASSERT_EQ(kOutputFrames, result.out.size());
            ASSERT_EQ(result.out.size(), reference.out.size());
            for (const float sample : result.out)
                EXPECT_TRUE(std::isfinite(sample));
            const CandidateStepPeak candidatePeak =
                CandidateMaximumStep(result.out);
            const CandidateStepPeak referencePeak =
                CandidateMaximumStep(reference.out);
            const std::size_t crossingFrame =
                static_cast<std::size_t>(std::ceil(
                    (boundary - initial) /
                    (target - initial) * kRampFrames));
            const std::size_t overlapWindowStart =
                crossingFrame >
                        harken::kMultirateTransitionOverlapFrames +
                            32u
                    ? crossingFrame -
                          harken::kMultirateTransitionOverlapFrames -
                          32u
                    : 1u;
            const std::size_t overlapWindowEnd =
                crossingFrame +
                harken::kMultirateTransitionOverlapFrames + 32u;
            const CandidateStepPeak overlapPeak =
                CandidateMaximumStep(
                    result.out, overlapWindowStart,
                    overlapWindowEnd);
            EXPECT_LT(candidatePeak.value,
                      referencePeak.value + 0.05)
                << "candidate peak frame "
                << candidatePeak.frame;
            EXPECT_LT(overlapPeak.value,
                      referencePeak.value + 0.05)
                << "overlap peak frame "
                << overlapPeak.frame;
            EXPECT_LT(CandidateMaximumWindowEnergyDifference(
                          result.out, reference.out, 256, 128),
                      0.03);

            const bool changesStageCount =
                boundary >= 2.0 && boundary <= 32.0;
            EXPECT_EQ(0u,
                      result.counters.discontinuousStageTransitions);
            EXPECT_EQ(
                MultirateResamplerCandidate::ResolveStageCount(target),
                result.activeStages);
            EXPECT_EQ(changesStageCount ? 1u : 0u,
                      result.counters.stageTransitions);
        }
    }
}

TEST(MultirateResamplerCandidate,
     RepeatedBidirectionalBoundaryCrossingsStayContinuous) {
    constexpr double kBoundaries[] = {
        1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0,
    };
    for (const double boundary : kBoundaries) {
        SCOPED_TRACE(boundary);
        const CandidateDriveResult result =
            RunCandidateRepeatedBoundaryCrossings(boundary);
        const CandidateDriveResult reference =
            RunCandidateRepeatedBoundaryCrossings(boundary, false);
        ASSERT_FALSE(result.budgetExceeded);
        ASSERT_FALSE(reference.budgetExceeded);
        ASSERT_EQ(2048u, result.out.size());
        ASSERT_EQ(result.out.size(), reference.out.size());
        EXPECT_EQ(0u,
                  result.counters.discontinuousStageTransitions);
        EXPECT_EQ(
            boundary >= 2.0 && boundary <= 32.0
                ? 4u
                : 0u,
                  result.counters.stageTransitions);
        EXPECT_EQ(
            MultirateResamplerCandidate::ResolveStageCount(
                boundary * 0.98),
            result.activeStages);
        const CandidateStepPeak candidatePeak =
            CandidateMaximumStep(result.out);
        const CandidateStepPeak referencePeak =
            CandidateMaximumStep(reference.out);
        EXPECT_LT(candidatePeak.value,
                  referencePeak.value + 0.05)
            << "candidate peak frame " << candidatePeak.frame;
        for (std::size_t segment = 0; segment < 4u; ++segment) {
            const std::size_t crossingFrame =
                segment * 512u + 128u;
            const std::size_t windowStart =
                crossingFrame > 160u
                    ? crossingFrame - 160u
                    : 1u;
            const CandidateStepPeak overlapPeak =
                CandidateMaximumStep(
                    result.out, windowStart,
                    crossingFrame + 160u);
            EXPECT_LT(overlapPeak.value,
                      referencePeak.value + 0.05)
                << "overlap peak frame "
                << overlapPeak.frame;
        }
        EXPECT_LT(CandidateMaximumWindowEnergyDifference(
                      result.out, reference.out, 256, 128),
                  0.03);
    }
}

TEST(MultirateResamplerCandidate,
     BoundaryImpulseIsNeitherDuplicatedNorDropped) {
    constexpr double kBoundaries[] = {
        1.0, 2.0, 4.0, 8.0, 16.0, 32.0, 64.0,
    };
    constexpr std::uint32_t kRampFrames = 512;
    constexpr std::size_t kOutputFrames = 1024;
    for (const double boundary : kBoundaries) {
        SCOPED_TRACE(boundary);
        const double low = boundary * 0.98;
        const double high = boundary * 1.02;
        const std::size_t impulseFrame =
            static_cast<std::size_t>(boundary * 256.0);
        const std::size_t inputFrames =
            static_cast<std::size_t>(
                std::ceil(static_cast<double>(kOutputFrames + 128) *
                          high)) +
            8192;
        std::vector<float> impulse(inputFrames, 0.0f);
        ASSERT_LT(impulseFrame, impulse.size());
        impulse[impulseFrame] = 1.0f;
        const CandidateDriveResult result =
            DriveCandidate(low, high, kRampFrames, impulse,
                           kOutputFrames);
        ASSERT_FALSE(result.budgetExceeded);
        ASSERT_EQ(kOutputFrames, result.out.size());
        const float peak =
            *std::max_element(
                result.out.begin(), result.out.end(),
                [](float a, float b) {
                    return std::abs(a) < std::abs(b);
                });
        EXPECT_GT(std::abs(peak), 1.0e-4f)
            << "the authoritative impulse was dropped";
        EXPECT_EQ(1u, CandidateSignificantImpulseClusters(
                          result.out, 1.0e-5f, 64))
            << "the authoritative impulse appeared more than once";
        EXPECT_EQ(0u,
                  result.counters.discontinuousStageTransitions);
    }
}

TEST(MultirateResamplerCandidate,
     StageBoundaryRampsRequireDelayCompensatedOverlap) {
    constexpr double kBoundaries[] = {
        2.0, 4.0, 8.0, 16.0, 32.0,
    };
    for (const double boundary : kBoundaries) {
        const CandidateDriveResult result =
            RunCandidateTone(boundary * 0.98, boundary * 1.02,
                             512, 120.0, 1024);
        EXPECT_EQ(0u, result.counters.discontinuousStageTransitions)
            << "bounded two-path overlap regressed at " << boundary
            << "x";
        EXPECT_EQ(
            MultirateResamplerCandidate::ResolveStageCount(
                boundary * 1.02),
            result.activeStages);
    }
}

TEST(MultirateResamplerCandidate,
     ReadyMultiStageRampDoesNotExtendTheAuthoredRamp) {
    constexpr std::uint32_t kRampFrames = 16384;
    const CandidateDriveResult result =
        RunCandidateTone(1.0, kCandidateMaximumPublicRatio,
                         kRampFrames, 120.0, kRampFrames);
    ASSERT_FALSE(result.budgetExceeded);
    ASSERT_EQ(kRampFrames, result.out.size());
    EXPECT_EQ(0u, result.rampRemaining);
    EXPECT_DOUBLE_EQ(kCandidateMaximumPublicRatio, result.ratio);
    EXPECT_DOUBLE_EQ(kCandidateMaximumPublicRatio,
                     result.targetRatio);
    EXPECT_EQ(kMultirateMaxStages, result.activeStages);
    EXPECT_EQ(kMultirateMaxStages,
              result.counters.stageTransitions);
    EXPECT_EQ(0u,
              result.counters.discontinuousStageTransitions);
}

TEST(MultirateResamplerCandidate,
     FastMultiStageRampFallsBackWithoutSkippingStages) {
    constexpr std::uint32_t kRampFrames = 240;
    constexpr std::size_t kOutputFrames = 2048;
    const CandidateDriveResult result =
        RunCandidateTone(1.0, kCandidateMaximumPublicRatio,
                         kRampFrames, 120.0, kOutputFrames);
    ASSERT_FALSE(result.budgetExceeded);
    ASSERT_EQ(kOutputFrames, result.out.size());
    EXPECT_EQ(0u, result.rampRemaining);
    EXPECT_DOUBLE_EQ(kCandidateMaximumPublicRatio, result.ratio);
    EXPECT_EQ(kMultirateMaxStages, result.activeStages);
    EXPECT_EQ(kMultirateMaxStages,
              result.counters.stageTransitions);
    EXPECT_EQ(0u,
              result.counters.discontinuousStageTransitions);
    for (const float sample : result.out)
        EXPECT_TRUE(std::isfinite(sample));
}

TEST(MultirateResamplerCandidate,
     MultiStageRampIsIndependentOfFeedAndOutputPartitioning) {
    constexpr std::uint32_t kRampFrames = 2048;
    constexpr std::size_t kOutputFrames = 4096;
    const CandidateDriveResult regular =
        RunCandidateTone(1.0, kCandidateMaximumPublicRatio,
                         kRampFrames, 120.0, kOutputFrames,
                         2, 128, 128);
    const CandidateDriveResult fragmented =
        RunCandidateTone(1.0, kCandidateMaximumPublicRatio,
                         kRampFrames, 120.0, kOutputFrames,
                         2, 17, 7);
    ASSERT_FALSE(regular.budgetExceeded);
    ASSERT_FALSE(fragmented.budgetExceeded);
    ASSERT_EQ(regular.out.size(), fragmented.out.size());
    for (std::size_t frame = 0; frame < regular.out.size(); ++frame)
        EXPECT_FLOAT_EQ(regular.out[frame], fragmented.out[frame]);
    EXPECT_EQ(regular.activeStages, fragmented.activeStages);
    EXPECT_DOUBLE_EQ(regular.ratio, fragmented.ratio);
    EXPECT_EQ(regular.rampRemaining, fragmented.rampRemaining);
    EXPECT_EQ(regular.counters.stageTransitions,
              fragmented.counters.stageTransitions);
}

TEST(MultirateResamplerCandidate,
     ReverseRetargetDuringMultiStageMigrationStaysBounded) {
    const CandidateDriveResult result =
        RunCandidateRepeatedRatioCrossings(
            1.0, kCandidateMaximumPublicRatio);
    ASSERT_FALSE(result.budgetExceeded);
    ASSERT_EQ(2048u, result.out.size());
    EXPECT_EQ(0u,
              result.counters.discontinuousStageTransitions);
    EXPECT_GT(result.counters.stageTransitions, 1u);
    for (const float sample : result.out)
        EXPECT_TRUE(std::isfinite(sample));
}

TEST(MultirateResamplerCandidate,
     FortyEightToFortyFourOneHighestPitchRampIsContinuous) {
    const CandidateRatePair rates = {48000.0, 44100.0};
    const double ratioAtPlus46 =
        CandidateEffectiveRatio(rates, 46);
    const double ratioAtPlus48 =
        CandidateEffectiveRatio(rates, 48);
    EXPECT_LT(ratioAtPlus46, 16.0);
    EXPECT_DOUBLE_EQ(kCandidateMaximumProfessionalRatio,
                     ratioAtPlus48);

    for (const bool upward : {false, true}) {
        const double initial =
            upward ? ratioAtPlus46 : ratioAtPlus48;
        const double target =
            upward ? ratioAtPlus48 : ratioAtPlus46;
        SCOPED_TRACE(upward ? "upward" : "downward");
        const CandidateDriveResult result =
            RunCandidateTone(initial, target, 512, 120.0, 1024);
        const CandidateDriveResult reference =
            RunCandidateTone(initial, target, 512, 120.0, 1024,
                             1, 128, 128, false);
        ASSERT_FALSE(result.budgetExceeded);
        ASSERT_FALSE(reference.budgetExceeded);
        ASSERT_EQ(1024u, result.out.size());
        ASSERT_EQ(result.out.size(), reference.out.size());
        EXPECT_EQ(0u,
                  result.counters.discontinuousStageTransitions);
        EXPECT_EQ(1u, result.counters.stageTransitions);
        EXPECT_EQ(
            MultirateResamplerCandidate::ResolveStageCount(target),
            result.activeStages);
        const CandidateStepPeak candidatePeak =
            CandidateMaximumStep(result.out);
        const CandidateStepPeak referencePeak =
            CandidateMaximumStep(reference.out);
        const std::size_t crossingFrame =
            static_cast<std::size_t>(std::ceil(
                (16.0 - initial) /
                (target - initial) * 512.0));
        const std::size_t windowStart =
            crossingFrame > 160u
                ? crossingFrame - 160u
                : 1u;
        const CandidateStepPeak overlapPeak =
            CandidateMaximumStep(
                result.out, windowStart,
                crossingFrame + 160u);
        EXPECT_LT(candidatePeak.value,
                  referencePeak.value + 0.05)
            << "candidate peak frame "
            << candidatePeak.frame;
        EXPECT_LT(overlapPeak.value,
                  referencePeak.value + 0.05)
            << "overlap peak frame "
            << overlapPeak.frame;
        EXPECT_LT(CandidateMaximumWindowEnergyDifference(
                      result.out, reference.out, 256, 128),
                  0.03);
    }
}

TEST(MultirateResamplerCandidate,
     FortyEightToFortyFourOneHighestPitchRepeatedCrossingIsContinuous) {
    const CandidateRatePair rates = {48000.0, 44100.0};
    const double ratioAtPlus46 =
        CandidateEffectiveRatio(rates, 46);
    const double ratioAtPlus48 =
        CandidateEffectiveRatio(rates, 48);
    const CandidateDriveResult result =
        RunCandidateRepeatedRatioCrossings(
            ratioAtPlus46, ratioAtPlus48);
    const CandidateDriveResult reference =
        RunCandidateRepeatedRatioCrossings(
            ratioAtPlus46, ratioAtPlus48, false);
    ASSERT_FALSE(result.budgetExceeded);
    ASSERT_FALSE(reference.budgetExceeded);
    ASSERT_EQ(2048u, result.out.size());
    ASSERT_EQ(result.out.size(), reference.out.size());
    EXPECT_EQ(0u,
              result.counters.discontinuousStageTransitions);
    EXPECT_EQ(4u, result.counters.stageTransitions);
    EXPECT_EQ(
        MultirateResamplerCandidate::ResolveStageCount(
            ratioAtPlus46),
        result.activeStages);
    const CandidateStepPeak candidatePeak =
        CandidateMaximumStep(result.out);
    const CandidateStepPeak referencePeak =
        CandidateMaximumStep(reference.out);
    EXPECT_LT(candidatePeak.value,
              referencePeak.value + 0.05)
        << "candidate peak frame " << candidatePeak.frame;
    for (std::size_t segment = 0; segment < 4u; ++segment) {
        const CandidateStepPeak overlapPeak =
            CandidateMaximumStep(
                result.out, segment * 512u + 1u,
                (segment + 1u) * 512u);
        EXPECT_LT(overlapPeak.value,
                  referencePeak.value + 0.05)
            << "overlap peak frame "
            << overlapPeak.frame;
    }
    EXPECT_LT(CandidateMaximumWindowEnergyDifference(
                  result.out, reference.out, 256, 128),
              0.03);
}

TEST(MultirateResamplerCandidate,
     ReusedTransitionPathDoesNotReadStaleHistory) {
    std::vector<float> input =
        harkentest::MakeSine(1024, 120.0, 48000.0, 0.8f);
    PCMView view;
    view.channels[0] = input.data();
    view.channelCount = 1;
    view.stride = 1;
    view.sampleRate = 48000;
    view.frames = input.size();
    std::vector<float> ring(kResampleFifoFrames, 0.0f);
    float* rings[1] = {ring.data()};
    MultirateResamplerCandidate candidate;
    candidate.Reset(rings, 1, 1.96, CandidateTables());
    MultirateCandidatePathState& reusable =
        candidate.paths[candidate.transitionPath];
    const float poison =
        std::numeric_limits<float>::quiet_NaN();
    for (MultirateHalfbandStageState& stage : reusable.stages) {
        for (float& sample : stage.sideHistory[0])
            sample = poison;
        for (float& sample : stage.centerHistory[0])
            sample = poison;
    }
    for (float& sample : reusable.finalRing[0])
        sample = poison;

    std::uint64_t inputCursor = 0;
    const std::uint32_t initialFeed = candidate.FreeFrames();
    candidate.PushPCM(view, inputCursor, initialFeed);
    inputCursor += initialFeed;
    candidate.UpdateRatio(2.04, 128);
    std::vector<float> output(240, 0.0f);
    float* destination[1] = {output.data()};
    std::uint32_t produced = 0;
    while (produced < output.size()) {
        produced += candidate.ProduceInstrumentedForTesting(
            destination, produced,
            static_cast<std::uint32_t>(
                output.size() - produced));
        if (produced == output.size())
            break;
        const std::uint32_t refill =
            static_cast<std::uint32_t>(
                std::min<std::uint64_t>(
                    candidate.FreeFrames(),
                    view.frames - inputCursor));
        ASSERT_GT(refill, 0u);
        candidate.PushPCM(view, inputCursor, refill);
        inputCursor += refill;
    }
    for (const float sample : output)
        EXPECT_TRUE(std::isfinite(sample));
    EXPECT_EQ(
        MultirateResamplerCandidate::ResolveStageCount(2.04),
        candidate.activeStages);
}

TEST(ResamplerProfessionalFacade,
     DefaultProfileIsLegacyAndExplicitProfessionalStays) {
    constexpr std::uint32_t kSupportedRates[] = {44100u, 48000u};
    for (const std::uint32_t sourceRate : kSupportedRates) {
        for (const std::uint32_t mixRate : kSupportedRates) {
            EXPECT_EQ(ResampleProfile::LegacyCompatibility,
                      SelectResampleProfile(sourceRate, mixRate));
        }
    }
    EXPECT_EQ(ResampleProfile::LegacyCompatibility,
              SelectResampleProfile(96000u, 48000u));
    EXPECT_EQ(ResampleProfile::LegacyCompatibility,
              SelectResampleProfile(48000u, 96000u));

    std::vector<float> ring(kResampleFifoFrames, 0.0f);
    float* rings[1] = {ring.data()};
    MultirateResamplerCandidate resamplerWorkspace;
    Resampler resampler;
    resampler.Reset(
        rings, 1, ResampleQuality::Linear, 1.0, Tables(),
        SelectResampleProfile(44100u, 48000u), CandidateTables(), nullptr);
    EXPECT_EQ(ResampleProfile::LegacyCompatibility,
              resampler.Profile());
    resampler.Reset(
        rings, 1, ResampleQuality::Sinc16, 1.0, Tables(),
        ResampleProfile::Professional44148, CandidateTables(), &resamplerWorkspace);
    EXPECT_EQ(ResampleProfile::Professional44148,
              resampler.Profile());
    resampler.UpdateRatio(
        kCandidateMinimumProfessionalRatio, 32);
    EXPECT_EQ(ResampleProfile::Professional44148,
              resampler.Profile());
    resampler.UpdateRatio(
        kCandidateMaximumProfessionalRatio, 32);
    EXPECT_EQ(ResampleProfile::Professional44148,
              resampler.Profile());
}

TEST(ResamplerProfessionalFacade,
    LinearExactEndpointDoesNotTurnInfinityIntoNan) {
    const float inf = std::numeric_limits<float>::infinity();
    EXPECT_EQ(inf, harken::InterpLinear(inf, inf, 0.0f));
    EXPECT_EQ(-inf, harken::InterpLinear(-inf, -inf, 0.0f));
    EXPECT_FLOAT_EQ(0.25f,
                    harken::InterpLinear(0.25f, 0.75f, 0.0f));

    for (const std::uint32_t bits :
         {0x00000000u, 0x80000000u, 0x7f800000u, 0xff800000u,
          0x7fc12345u, 0x7f812345u}) {
        SCOPED_TRACE(bits);
        std::vector<float> ring(kResampleFifoFrames, 0.0f);
        float* rings[1] = {ring.data()};
        Resampler resampler;
        resampler.Reset(rings, 1, ResampleQuality::Linear, 0.5, Tables());
        for (float& sample : ring)
            std::memcpy(&sample, &bits, sizeof(bits));
        resampler.writeIndex = resampler.readIndex + 16;
        float output[7] = {};
        float* destination[1] = {output + 1};
        ASSERT_EQ(5u, resampler.Produce(destination, 0, 5));
        for (const unsigned frame : {0u, 2u, 4u}) {
            std::uint32_t actual = 0;
            std::memcpy(&actual, &destination[0][frame], sizeof(actual));
            EXPECT_EQ(bits, actual);
        }
        EXPECT_EQ(0.0f, output[0]);
        EXPECT_EQ(0.0f, output[6]);
    }
}

TEST(ResamplerProfessionalFacade,
     LinearOverlappingOutputPreservesSequentialReads) {
    for (const int outputOffset : {-4, 0, 1}) {
        SCOPED_TRACE(outputOffset);
        std::vector<float> grouped(kResampleFifoFrames + 16, -987.25f);
        std::vector<float> sequential(grouped);
        Resampler resamplers[2];
        float* rings[2] = {grouped.data() + 8, sequential.data() + 8};
        for (unsigned side = 0; side < 2; ++side) {
            resamplers[side].Reset(&rings[side], 1,
                                  ResampleQuality::Linear, 0.5, Tables());
            for (unsigned frame = 0; frame < kResampleFifoFrames; ++frame)
                rings[side][frame] = static_cast<float>(frame % 17) * 0.03125f;
            resamplers[side].readIndex = kResampleFifoFrames - 1;
            resamplers[side].readPos = kResampleFifoFrames - 1;
            resamplers[side].writeIndex = kResampleFifoFrames + 15;
        }
        float* groupedOutput[1] = {rings[0] + outputOffset};
        float* sequentialOutput[1] = {rings[1] + outputOffset};
        ASSERT_EQ(5u, resamplers[0].Produce(groupedOutput, 0, 5));
        for (unsigned frame = 0; frame < 5; ++frame)
            ASSERT_EQ(1u, resamplers[1].Produce(sequentialOutput, frame, 1));
        EXPECT_EQ(0, std::memcmp(grouped.data(), sequential.data(),
                                 grouped.size() * sizeof(float)));
        EXPECT_EQ(resamplers[0].readIndex, resamplers[1].readIndex);
        EXPECT_EQ(resamplers[0].phaseHigh, resamplers[1].phaseHigh);
        EXPECT_EQ(resamplers[0].phaseLow, resamplers[1].phaseLow);
        EXPECT_DOUBLE_EQ(resamplers[0].readFraction, resamplers[1].readFraction);
        EXPECT_DOUBLE_EQ(resamplers[0].readPos, resamplers[1].readPos);
    }
}

TEST(ResamplerProfessionalFacade,
     IntegerSourceTimelineRemainsExactBeyondDoubleIntegerRange) {
    constexpr std::uint64_t kLargeFrame =
        std::uint64_t{1} << 54;
    constexpr double kInitialRatio = 1.25;
    constexpr double kTargetRatio = 1.75;
    constexpr std::uint32_t kRampFrames = 4;

    std::vector<float> input(64, 0.25f);
    PCMView view;
    view.channels[0] = input.data();
    view.channelCount = 1;
    view.stride = 1;
    view.sampleRate = 48000;
    view.frames = input.size();
    std::vector<float> ring(kResampleFifoFrames, 0.0f);
    float* rings[1] = {ring.data()};
    MultirateResamplerCandidate resamplerWorkspace;
    Resampler resampler;
    resampler.Reset(
        rings, 1, ResampleQuality::Sinc16, kInitialRatio, Tables(),
        ResampleProfile::Professional44148, CandidateTables(), &resamplerWorkspace);

    MultirateResamplerCandidate& candidate = resamplerWorkspace;
    candidate.sourceFrame += kLargeFrame;
    candidate.rawWriteIndex += kLargeFrame;
    MultirateCandidatePathState& path =
        candidate.paths[candidate.activePath];
    path.rawProcessIndex += kLargeFrame;
    path.sourceOrigin += static_cast<std::int64_t>(kLargeFrame);
    path.validSourceStart += kLargeFrame;

    resampler.PushPCM(view, 0, static_cast<std::uint32_t>(input.size()));
    resampler.UpdateRatio(kTargetRatio, kRampFrames);
    float output[kRampFrames] = {};
    float* destination[1] = {output};
    ASSERT_EQ(kRampFrames,
              resampler.Produce(destination, 0, kRampFrames));
    EXPECT_EQ(kLargeFrame + kResampleHistoryPad + 6u,
              resampler.SourceFrame());
    EXPECT_DOUBLE_EQ(0.25, resampler.SourceFraction());
    EXPECT_EQ(ResampleProfile::Professional44148,
              resampler.Profile());
    resampler.MarkInputEnd();
    EXPECT_FALSE(resampler.Exhausted());
    candidate.rawEndIndex = resampler.SourceFrame();
    EXPECT_TRUE(resampler.Exhausted());
    for (const float sample : output)
        EXPECT_TRUE(std::isfinite(sample));
}

TEST(ResamplerProfessionalFacade,
     ResetSlotReuseClearsProfessionalProfileState) {
    std::vector<float> ring(kResampleFifoFrames, 0.0f);
    float* rings[1] = {ring.data()};
    MultirateResamplerCandidate resamplerWorkspace;
    Resampler resampler;
    resampler.Reset(
        rings, 1, ResampleQuality::Sinc16, 2.0, Tables(),
        ResampleProfile::Professional44148, CandidateTables(), &resamplerWorkspace);
    resampler.UpdateRatio(4.0, 64);
    resampler.MarkInputEnd();
    ASSERT_EQ(ResampleProfile::Professional44148,
              resampler.Profile());
    ASSERT_TRUE(resampler.InputEnded());

    resampler.Reset(rings, 1, ResampleQuality::Linear, 0.75,
                    Tables());
    EXPECT_EQ(ResampleProfile::LegacyCompatibility,
              resampler.Profile());
    EXPECT_FALSE(resampler.InputEnded());
    EXPECT_EQ(kResampleHistoryPad, resampler.SourceFrame());
    EXPECT_DOUBLE_EQ(0.0, resampler.SourceFraction());
    EXPECT_DOUBLE_EQ(0.75, resampler.ratio);
    EXPECT_EQ(0u, resampler.rampRemaining);

    resampler.Reset(
        rings, 1, ResampleQuality::Sinc16, 1.0, Tables(),
        ResampleProfile::Professional44148, CandidateTables(), &resamplerWorkspace);
    EXPECT_EQ(ResampleProfile::Professional44148,
              resampler.Profile());
    EXPECT_FALSE(resampler.InputEnded());
    EXPECT_EQ(kResampleHistoryPad, resampler.SourceFrame());
    EXPECT_DOUBLE_EQ(0.0, resampler.SourceFraction());
    EXPECT_EQ(0u, resamplerWorkspace.rampRemaining);
    EXPECT_EQ(0u, resamplerWorkspace.counters.stageTransitions);
}

TEST(ResamplerProfessionalFacade,
     LegacyParkStateAcceptsProductionCombinedRatioRange) {
    constexpr double kLowCombinedRatio =
        (8000.0 / 48000.0) / 64.0;
    constexpr double kHighCombinedRatio =
        (1000000.0 / 48000.0) * 64.0;
    std::vector<float> ringA(kResampleFifoFrames, 0.0f);
    std::vector<float> ringB(kResampleFifoFrames, 0.0f);
    float* ringsA[1] = {ringA.data()};
    float* ringsB[1] = {ringB.data()};

    for (const double ratio :
         {kLowCombinedRatio, kHighCombinedRatio}) {
        SCOPED_TRACE(ratio);
        Resampler original;
        original.Reset(
            ringsA, 1, ResampleQuality::Linear, ratio, Tables(),
            ResampleProfile::LegacyCompatibility, CandidateTables(), nullptr);
        harken::ResamplerParkState state;
        ASSERT_TRUE(original.CaptureParkState(&state));
        ASSERT_TRUE(Resampler::ValidateParkState(state));

        Resampler restored;
        ASSERT_TRUE(restored.RestoreParkState(
            state, ringsB, 1, Tables(), CandidateTables()));
        EXPECT_DOUBLE_EQ(ratio, restored.CurrentRatio());
        EXPECT_DOUBLE_EQ(ratio, restored.TargetRatio());
        EXPECT_EQ(ResampleProfile::LegacyCompatibility,
                  restored.Profile());
    }

    Resampler ramping;
    ramping.Reset(
        ringsA, 1, ResampleQuality::Linear, kLowCombinedRatio, Tables(),
        ResampleProfile::LegacyCompatibility, CandidateTables(), nullptr);
    ramping.UpdateRatio(kHighCombinedRatio, 64);
    harken::ResamplerParkState rampState;
    ASSERT_TRUE(ramping.CaptureParkState(&rampState));
    Resampler restoredRamp;
    ASSERT_TRUE(restoredRamp.RestoreParkState(
        rampState, ringsB, 1, Tables(), CandidateTables()));
    EXPECT_DOUBLE_EQ(kLowCombinedRatio,
                     restoredRamp.CurrentRatio());
    EXPECT_DOUBLE_EQ(kHighCombinedRatio,
                     restoredRamp.TargetRatio());
    EXPECT_EQ(64u, restoredRamp.RampFramesRemaining());
}

TEST(ResamplerHighRatio, OwnedCopiesKeepIndependentCompleteFilterHistory) {
    std::vector<float> ring(kResampleFifoFrames, 0.0f);
    float* rings[1] = {ring.data()};
    Resampler original;
    original.Reset(rings, 1, ResampleQuality::Sinc16, 32.0, Tables());
    harken::ResamplerParkState state;
    ASSERT_TRUE(original.CaptureParkState(&state));
    state.legacyHighRatioFilter[0][2].z1 = 0.125;
    state.legacyHighRatioFilter[7][3].z2 = 0.5;
    state.legacyHighRatioPrevious[7] = -0.0;
    state.legacyHighRatioCurrent[7] = std::numeric_limits<double>::quiet_NaN();
    ASSERT_TRUE(original.RestoreParkState(state, rings, 1, Tables(), CandidateTables()));

    Resampler copied(original);
    Resampler moved(std::move(original));
    Resampler assigned;
    assigned = copied;
    Resampler moveAssigned;
    moveAssigned = std::move(copied);
    for (const Resampler* source : {&original, &copied}) {
        harken::ResamplerParkState afterMove;
        ASSERT_TRUE(source->CaptureParkState(&afterMove));
        EXPECT_EQ(0, std::memcmp(state.legacyHighRatioFilter, afterMove.legacyHighRatioFilter,
                                  sizeof(state.legacyHighRatioFilter)));
    }
    original.Reset(rings, 1, ResampleQuality::Linear, 1.0, Tables());
    copied.Reset(rings, 1, ResampleQuality::Linear, 1.0, Tables());
    for (const Resampler* destination : {&moved, &assigned, &moveAssigned}) {
        harken::ResamplerParkState afterReset;
        ASSERT_TRUE(destination->CaptureParkState(&afterReset));
        EXPECT_DOUBLE_EQ(state.ratio, destination->CurrentRatio());
        EXPECT_EQ(rings[0], destination->ring[0]);
        EXPECT_EQ(0, std::memcmp(state.legacyHighRatioFilter, afterReset.legacyHighRatioFilter,
                                  sizeof(state.legacyHighRatioFilter)));
        EXPECT_EQ(0, std::memcmp(state.legacyHighRatioPrevious, afterReset.legacyHighRatioPrevious,
                                  sizeof(state.legacyHighRatioPrevious)));
        EXPECT_EQ(0, std::memcmp(state.legacyHighRatioCurrent, afterReset.legacyHighRatioCurrent,
                                  sizeof(state.legacyHighRatioCurrent)));
    }
}

TEST(ResamplerHighRatio, ActiveParkRestoresPreparedColumnsAndPreservesInactiveValues) {
    const auto input = harkentest::MakeSine(1024, 375.0, 48000, 0.25f);
    PCMView pcm;
    pcm.channels[0] = input.data();
    pcm.channelCount = 1;
    pcm.stride = 1;
    pcm.frames = input.size();
    pcm.sampleRate = 48000;
    std::vector<float> ringA(kResampleFifoFrames), ringB(kResampleFifoFrames);
    float* ringsA[1] = {ringA.data()};
    float* ringsB[1] = {ringB.data()};
    Resampler original;
    original.Reset(ringsA, 1, ResampleQuality::Sinc16, 32.0, Tables());
    ASSERT_GE(original.FreeFrames(), 256u);
    original.PushPCM(pcm, 0, 256);
    float warm[7]{};
    float* warmPlanes[1] = {warm};
    ASSERT_EQ(7u, original.Produce(warmPlanes, 0, 7));
    harken::ResamplerParkState state;
    ASSERT_TRUE(original.CaptureParkState(&state));
    ASSERT_TRUE(state.legacyHighRatioFilterActive);

    harken::ResamplerHighRatioChannelState channels[harken::kMaxChannels]{};
    const harken::ResamplerHighRatioColumns narrow{{&channels[0]}};
    harken::ResamplerHighRatioColumns full;
    for (std::uint32_t ch = 0; ch < harken::kMaxChannels; ++ch)
        full.channels[ch] = &channels[ch];
    harken::ResamplerView restored;
    restored.BindHighRatioColumns(narrow, 0, 1);
    ASSERT_TRUE(restored.RestoreParkState(state, ringsB, 1, Tables(), CandidateTables()));
    ASSERT_GE(original.FreeFrames(), 128u);
    ASSERT_GE(restored.FreeFrames(), 128u);
    original.PushPCM(pcm, 256, 128);
    restored.PushPCM(pcm, 256, 128);
    float expected[4]{}, actual[4]{};
    float* expectedPlanes[1] = {expected};
    float* actualPlanes[1] = {actual};
    ASSERT_EQ(4u, original.Produce(expectedPlanes, 0, 4));
    ASSERT_EQ(4u, restored.Produce(actualPlanes, 0, 4));
    EXPECT_EQ(0, std::memcmp(expected, actual, sizeof(expected)));

    state.legacyHighRatioFilter[7][3].z1 = 0.5;
    state.legacyHighRatioPrevious[7] = -0.0;
    state.legacyHighRatioCurrent[7] = std::numeric_limits<double>::quiet_NaN();
    ASSERT_TRUE(Resampler::ValidateParkState(state));
    harken::ResamplerParkState before;
    ASSERT_TRUE(restored.CaptureParkState(&before));
    const auto ringBefore = ringB;
    EXPECT_FALSE(restored.RestoreParkState(state, ringsB, 1, Tables(), CandidateTables()));
    harken::ResamplerParkState after;
    ASSERT_TRUE(restored.CaptureParkState(&after));
    EXPECT_EQ(before.legacyReadIndex, after.legacyReadIndex);
    EXPECT_EQ(before.legacyWriteIndex, after.legacyWriteIndex);
    EXPECT_DOUBLE_EQ(before.legacyReadFraction, after.legacyReadFraction);
    EXPECT_EQ(ringBefore, ringB);
    EXPECT_EQ(0, std::memcmp(before.legacyHighRatioFilter, after.legacyHighRatioFilter,
                              sizeof(before.legacyHighRatioFilter)));
    EXPECT_EQ(0, std::memcmp(before.legacyHighRatioPrevious, after.legacyHighRatioPrevious,
                              sizeof(before.legacyHighRatioPrevious)));
    EXPECT_EQ(0, std::memcmp(before.legacyHighRatioCurrent, after.legacyHighRatioCurrent,
                              sizeof(before.legacyHighRatioCurrent)));

    restored.BindHighRatioColumns(full, 0, harken::kMaxChannels);
    ASSERT_TRUE(restored.RestoreParkState(state, ringsB, 1, Tables(), CandidateTables()));
    ASSERT_TRUE(restored.CaptureParkState(&after));
    EXPECT_EQ(0, std::memcmp(state.legacyHighRatioFilter, after.legacyHighRatioFilter,
                              sizeof(state.legacyHighRatioFilter)));
    EXPECT_EQ(0, std::memcmp(state.legacyHighRatioPrevious, after.legacyHighRatioPrevious,
                              sizeof(state.legacyHighRatioPrevious)));
    EXPECT_EQ(0, std::memcmp(state.legacyHighRatioCurrent, after.legacyHighRatioCurrent,
                              sizeof(state.legacyHighRatioCurrent)));
    restored.Reset(ringsB, 1, ResampleQuality::Linear, 1.0, Tables());
    ASSERT_TRUE(restored.CaptureParkState(&after));
    const harken::ResamplerParkState zero{};
    EXPECT_EQ(0, std::memcmp(zero.legacyHighRatioFilter, after.legacyHighRatioFilter,
                              sizeof(zero.legacyHighRatioFilter)));
    EXPECT_EQ(0, std::memcmp(zero.legacyHighRatioPrevious, after.legacyHighRatioPrevious,
                              sizeof(zero.legacyHighRatioPrevious)));
    EXPECT_EQ(0, std::memcmp(zero.legacyHighRatioCurrent, after.legacyHighRatioCurrent,
                              sizeof(zero.legacyHighRatioCurrent)));
}

TEST(ResamplerProfessionalFacade,
     ProfessionalStateRoundTripsMidFractionAndRamp) {
    std::vector<float> input =
        harkentest::MakeSine(512, 440.0, 48000.0, 0.8f);
    PCMView view;
    view.channels[0] = input.data();
    view.channelCount = 1;
    view.stride = 1;
    view.sampleRate = 48000;
    view.frames = input.size();
    std::vector<float> ringA(kResampleFifoFrames, 0.0f);
    std::vector<float> ringB(kResampleFifoFrames, 0.0f);
    float* ringsA[1] = {ringA.data()};
    float* ringsB[1] = {ringB.data()};
    MultirateResamplerCandidate originalWorkspace;
    Resampler original;
    original.Reset(
        ringsA, 1, ResampleQuality::Sinc16, 1.25, Tables(),
        ResampleProfile::Professional44148, CandidateTables(), &originalWorkspace);
    original.PushPCM(view, 0, 256);
    original.UpdateRatio(1.75, 32);
    float prefix[7] = {};
    float* prefixPlanes[1] = {prefix};
    ASSERT_EQ(7u,
              originalWorkspace.ProduceInstrumentedForTesting(
                  prefixPlanes, 0, 7));
    ASSERT_NE(0.0, original.SourceFraction());
    ASSERT_NE(0u, original.RampFramesRemaining());
    ASSERT_GT(originalWorkspace.counters.rawFramesRead, 0u);

    harken::ProfessionalResamplerState state;
    ASSERT_TRUE(original.CaptureProfessional(&state));
    ASSERT_TRUE(Resampler::ValidateProfessionalState(state));
    MultirateResamplerCandidate restoredWorkspace;
    Resampler restored;
    ASSERT_TRUE(restored.RestoreProfessional(
        state, ringsB, 1, Tables(), CandidateTables(), &restoredWorkspace));
    EXPECT_EQ(0u, restoredWorkspace.counters.rawFramesRead);
    EXPECT_EQ(
        0u,
        restoredWorkspace.counters.halfbandMultiplyAccumulates);
    EXPECT_EQ(
        0u,
        restoredWorkspace.counters.residualMultiplyAccumulates);
    EXPECT_EQ(original.SourceFrame(), restored.SourceFrame());
    EXPECT_DOUBLE_EQ(original.SourceFraction(),
                     restored.SourceFraction());
    EXPECT_DOUBLE_EQ(original.CurrentRatio(),
                     restored.CurrentRatio());
    EXPECT_DOUBLE_EQ(original.TargetRatio(),
                     restored.TargetRatio());
    EXPECT_EQ(original.RampFramesRemaining(),
              restored.RampFramesRemaining());

    float expected[16] = {};
    float actual[16] = {};
    float* expectedPlanes[1] = {expected};
    float* actualPlanes[1] = {actual};
    ASSERT_EQ(16u, original.Produce(expectedPlanes, 0, 16));
    ASSERT_EQ(16u, restored.Produce(actualPlanes, 0, 16));
    for (std::uint32_t i = 0; i < 16; ++i)
        EXPECT_FLOAT_EQ(expected[i], actual[i]);
}

TEST(ResamplerProfessionalFacade,
     MaximumPublicRatioRoundTripsWithoutChangingTheTimeline) {
    std::vector<float> input =
        harkentest::MakeSine(8192, 75.0, 48000.0, 0.8f);
    PCMView view;
    view.channels[0] = input.data();
    view.channelCount = 1;
    view.stride = 1;
    view.sampleRate = 48000;
    view.frames = input.size();
    std::vector<float> ringA(kResampleFifoFrames, 0.0f);
    std::vector<float> ringB(kResampleFifoFrames, 0.0f);
    float* ringsA[1] = {ringA.data()};
    float* ringsB[1] = {ringB.data()};
    MultirateResamplerCandidate originalWorkspace;
    Resampler original;
    original.Reset(
        ringsA, 1, ResampleQuality::Sinc16,
        kCandidateMaximumPublicRatio, Tables(),
        ResampleProfile::Professional44148, CandidateTables(), &originalWorkspace);

    std::uint64_t inputCursor = 0;
    float prefix[32] = {};
    float* prefixPlane[1] = {prefix};
    std::uint32_t prefixProduced = 0;
    while (prefixProduced < 32u) {
        prefixProduced += original.Produce(
            prefixPlane, prefixProduced,
            32u - prefixProduced);
        if (prefixProduced == 32u)
            break;
        const std::uint32_t refill =
            static_cast<std::uint32_t>(
                std::min<std::uint64_t>(
                    original.FreeFrames(),
                    view.frames - inputCursor));
        ASSERT_GT(refill, 0u);
        original.PushPCM(view, inputCursor, refill);
        inputCursor += refill;
    }
    ASSERT_EQ(kMultirateMaxStages,
              originalWorkspace.activeStages);

    harken::ProfessionalResamplerState state;
    ASSERT_TRUE(original.CaptureProfessional(&state));
    ASSERT_TRUE(Resampler::ValidateProfessionalState(state));
    MultirateResamplerCandidate restoredWorkspace;
    Resampler restored;
    ASSERT_TRUE(restored.RestoreProfessional(
        state, ringsB, 1, Tables(), CandidateTables(), &restoredWorkspace));

    float expected[32] = {};
    float actual[32] = {};
    float* expectedPlane[1] = {expected};
    float* actualPlane[1] = {actual};
    std::uint32_t produced = 0;
    while (produced < 32u) {
        const std::uint32_t expectedProduced =
            original.Produce(
                expectedPlane, produced, 32u - produced);
        const std::uint32_t actualProduced =
            restored.Produce(
                actualPlane, produced, 32u - produced);
        ASSERT_EQ(expectedProduced, actualProduced);
        produced += expectedProduced;
        if (produced == 32u)
            break;
        const std::uint32_t refill =
            static_cast<std::uint32_t>(
                std::min<std::uint64_t>(
                    {original.FreeFrames(),
                     restored.FreeFrames(),
                     view.frames - inputCursor}));
        ASSERT_GT(refill, 0u);
        original.PushPCM(view, inputCursor, refill);
        restored.PushPCM(view, inputCursor, refill);
        inputCursor += refill;
    }
    for (std::uint32_t frame = 0; frame < 32u; ++frame)
        EXPECT_FLOAT_EQ(expected[frame], actual[frame]);
    EXPECT_EQ(original.SourceFrame(), restored.SourceFrame());
    EXPECT_DOUBLE_EQ(original.SourceFraction(),
                     restored.SourceFraction());
    EXPECT_DOUBLE_EQ(kCandidateMaximumPublicRatio,
                     restored.CurrentRatio());
}

TEST(ResamplerProfessionalFacade,
     WrappedNearFullStateRestoresEverySupportedChannel) {
    for (const std::uint32_t channels : {1u, 2u, 8u}) {
        SCOPED_TRACE(channels);
        std::vector<std::vector<float>> source(
            channels, std::vector<float>(2048, 0.0f));
        PCMView view;
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            for (std::size_t frame = 0; frame < source[ch].size(); ++frame) {
                const int distinct =
                    static_cast<int>((frame * 29u + ch * 17u) % 127u) - 63;
                source[ch][frame] =
                    static_cast<float>(distinct) * 0.003f;
            }
            view.channels[ch] = source[ch].data();
        }
        view.channelCount = channels;
        view.stride = 1;
        view.sampleRate = 48000;
        view.frames = source[0].size();

        std::vector<float> ringA(
            static_cast<std::size_t>(channels) * kResampleFifoFrames, 0.0f);
        std::vector<float> ringB(ringA.size(), 0.0f);
        float* ringsA[harken::kMaxChannels] = {};
        float* ringsB[harken::kMaxChannels] = {};
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            ringsA[ch] =
                ringA.data() +
                static_cast<std::size_t>(ch) * kResampleFifoFrames;
            ringsB[ch] =
                ringB.data() +
                static_cast<std::size_t>(ch) * kResampleFifoFrames;
        }

        MultirateResamplerCandidate originalWorkspace;
        Resampler original;
        original.Reset(
            ringsA, channels, ResampleQuality::Sinc16, 1.0, Tables(),
            ResampleProfile::Professional44148, CandidateTables(), &originalWorkspace);
        std::uint64_t sourceCursor = 0;
        const std::uint32_t initialFeed = original.FreeFrames();
        ASSERT_EQ(kResampleFifoFrames, initialFeed);
        original.PushPCM(view, sourceCursor, initialFeed);
        sourceCursor += initialFeed;

        std::vector<std::vector<float>> prefix(
            channels, std::vector<float>(192, 0.0f));
        float* prefixPlanes[harken::kMaxChannels] = {};
        for (std::uint32_t ch = 0; ch < channels; ++ch)
            prefixPlanes[ch] = prefix[ch].data();
        ASSERT_EQ(192u, original.Produce(prefixPlanes, 0, 192));

        const std::uint32_t refill = original.FreeFrames();
        ASSERT_GT(refill, 0u);
        original.PushPCM(view, sourceCursor, refill);
        sourceCursor += refill;

        harken::ProfessionalResamplerState state;
        ASSERT_TRUE(original.CaptureProfessional(&state));
        ASSERT_TRUE(Resampler::ValidateProfessionalState(state));
        EXPECT_EQ(kResampleFifoFrames, state.rawFrameCount);
        EXPECT_GT(state.rawWriteIndex, kResampleFifoFrames);
        EXPECT_GT((state.rawStartIndex & (kResampleFifoFrames - 1u)) +
                      state.rawFrameCount,
                  kResampleFifoFrames);
        EXPECT_LE(state.usedBytes, harken::kStreamParkTokenBytes);

        MultirateResamplerCandidate restoredWorkspace;
        Resampler restored;
        ASSERT_TRUE(restored.RestoreProfessional(
            state, ringsB, channels, Tables(), CandidateTables(), &restoredWorkspace));
        std::vector<std::vector<float>> expected(
            channels, std::vector<float>(96, 0.0f));
        std::vector<std::vector<float>> actual(
            channels, std::vector<float>(96, 0.0f));
        float* expectedPlanes[harken::kMaxChannels] = {};
        float* actualPlanes[harken::kMaxChannels] = {};
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            expectedPlanes[ch] = expected[ch].data();
            actualPlanes[ch] = actual[ch].data();
        }
        ASSERT_EQ(96u, original.Produce(expectedPlanes, 0, 96));
        ASSERT_EQ(96u, restored.Produce(actualPlanes, 0, 96));
        for (std::uint32_t ch = 0; ch < channels; ++ch) {
            for (std::uint32_t frame = 0; frame < 96; ++frame) {
                EXPECT_FLOAT_EQ(expected[ch][frame], actual[ch][frame])
                    << "channel " << ch << " frame " << frame;
            }
        }
    }
}

TEST(ResamplerProfessionalFacade,
     PreparingFuturePathRoundTripsAcrossEveryStageBoundary) {
    constexpr double kBoundaries[] = {
        2.0, 4.0, 8.0, 16.0, 32.0,
    };
    const std::vector<float> input =
        harkentest::MakeSine(8192, 120.0, 48000.0, 0.8f);
    PCMView view;
    view.channels[0] = input.data();
    view.channelCount = 1;
    view.stride = 1;
    view.sampleRate = 48000;
    view.frames = input.size();

    for (const double boundary : kBoundaries) {
        SCOPED_TRACE(boundary);
        std::vector<float> ringA(kResampleFifoFrames, 0.0f);
        std::vector<float> ringB(kResampleFifoFrames, 0.0f);
        float* ringsA[1] = {ringA.data()};
        float* ringsB[1] = {ringB.data()};
        MultirateResamplerCandidate originalWorkspace;
        Resampler original;
        original.Reset(
            ringsA, 1, ResampleQuality::Sinc16,
            boundary * 0.98, Tables(),
            ResampleProfile::Professional44148,
            CandidateTables(), &originalWorkspace);
        original.UpdateRatio(boundary * 1.02, 128);
        float unused = 0.0f;
        float* unusedPlane[1] = {&unused};
        EXPECT_EQ(0u, original.Produce(unusedPlane, 0, 1));
        ASSERT_TRUE(originalWorkspace.transitionPreparing);
        const MultirateCandidatePathState& future =
            originalWorkspace.paths[
                originalWorkspace.transitionPath];
        ASSERT_GT(future.rawProcessIndex,
                  originalWorkspace.rawWriteIndex);
        constexpr std::uint64_t kMaximumPreparingLookahead =
            static_cast<std::uint64_t>(
                harken::kMultirateFinalRingFrames) *
                (std::uint64_t{1} <<
                 harken::kMultirateMaxStages) +
            static_cast<std::uint64_t>(
                harken::kMultirateHalfbandTaps / 2u) *
                ((std::uint64_t{1} <<
                  harken::kMultirateMaxStages) -
                 1u) +
            ((std::uint64_t{1} <<
              harken::kMultirateMaxStages) -
             1u);
        EXPECT_LE(future.rawProcessIndex -
                      originalWorkspace.rawWriteIndex,
                  kMaximumPreparingLookahead);

        harken::ProfessionalResamplerState state;
        ASSERT_TRUE(original.CaptureProfessional(&state));
        ASSERT_TRUE(Resampler::ValidateProfessionalState(state));
        MultirateResamplerCandidate restoredWorkspace;
        Resampler restored;
        ASSERT_TRUE(restored.RestoreProfessional(
            state, ringsB, 1, Tables(), CandidateTables(), &restoredWorkspace));
        float expected[16] = {};
        float actual[16] = {};
        float* expectedPlane[1] = {expected};
        float* actualPlane[1] = {actual};
        std::uint64_t inputCursor = 0;
        std::uint32_t produced = 0;
        while (produced < 16u) {
            const std::uint32_t expectedProduced =
                original.Produce(
                    expectedPlane, produced, 16u - produced);
            const std::uint32_t actualProduced =
                restored.Produce(
                    actualPlane, produced, 16u - produced);
            ASSERT_EQ(expectedProduced, actualProduced);
            produced += expectedProduced;
            if (produced == 16u)
                break;
            const std::uint32_t refill =
                static_cast<std::uint32_t>(
                    std::min<std::uint64_t>(
                        {original.FreeFrames(),
                         restored.FreeFrames(),
                         view.frames - inputCursor}));
            ASSERT_GT(refill, 0u);
            original.PushPCM(view, inputCursor, refill);
            restored.PushPCM(view, inputCursor, refill);
            inputCursor += refill;
        }
        for (std::uint32_t frame = 0; frame < 16; ++frame) {
            EXPECT_TRUE(std::isfinite(expected[frame]));
            EXPECT_FLOAT_EQ(expected[frame], actual[frame]);
        }
        EXPECT_EQ(original.SourceFrame(), restored.SourceFrame());
        EXPECT_DOUBLE_EQ(original.SourceFraction(),
                         restored.SourceFraction());
    }
}

TEST(ResamplerProfessionalFacade,
     InvalidCapturedIndicesAndTransitionStatesAreRejectedAtomically) {
    std::vector<float> sourceRing(kResampleFifoFrames, 0.0f);
    float* sourceRings[1] = {sourceRing.data()};
    MultirateResamplerCandidate sourceWorkspace;
    Resampler source;
    source.Reset(
        sourceRings, 1, ResampleQuality::Sinc16, 1.0, Tables(),
        ResampleProfile::Professional44148, CandidateTables(), &sourceWorkspace);
    harken::ProfessionalResamplerState valid;
    ASSERT_TRUE(source.CaptureProfessional(&valid));
    ASSERT_TRUE(Resampler::ValidateProfessionalState(valid));

    auto invalid = valid;
    invalid.paths[invalid.activePath].stages[0].sideWriteOffset =
        harken::kMultirateHalfbandSideFrames;
    EXPECT_FALSE(Resampler::ValidateProfessionalState(invalid));

    invalid = valid;
    invalid.paths[invalid.activePath].stages[0].centerWriteOffset =
        harken::kMultirateHalfbandCenterFrames;
    EXPECT_FALSE(Resampler::ValidateProfessionalState(invalid));

    invalid = valid;
    invalid.transitionPreparing = true;
    invalid.transitionOverlapping = true;
    EXPECT_FALSE(Resampler::ValidateProfessionalState(invalid));

    invalid = valid;
    invalid.transitionOverlapping = true;
    invalid.overlapFrame =
        harken::kMultirateTransitionOverlapFrames;
    EXPECT_FALSE(Resampler::ValidateProfessionalState(invalid));

    invalid = valid;
    invalid.inputEnded = true;
    invalid.rawEndIndex = invalid.rawWriteIndex + 1u;
    EXPECT_FALSE(Resampler::ValidateProfessionalState(invalid));

    invalid = valid;
    invalid.rampRemaining = 1;
    invalid.rampStep =
        (std::numeric_limits<double>::max)();
    EXPECT_FALSE(Resampler::ValidateProfessionalState(invalid));

    invalid = valid;
    invalid.ratio = 1.0;
    invalid.targetRatio = 1.0;
    invalid.rampRemaining = 4;
    invalid.rampStep = -0.49;
    EXPECT_FALSE(Resampler::ValidateProfessionalState(invalid));

    invalid = valid;
    invalid.targetRatio = 70.0;
    EXPECT_FALSE(Resampler::ValidateProfessionalState(invalid));

    invalid = valid;
    invalid.paths[invalid.transitionPath].initialized = true;
    EXPECT_FALSE(Resampler::ValidateProfessionalState(invalid));

    invalid = valid;
    invalid.transitionPreparing = true;
    invalid.paths[invalid.transitionPath].initialized = true;
    invalid.paths[invalid.transitionPath].rawProcessIndex = 0;
    EXPECT_FALSE(Resampler::ValidateProfessionalState(invalid));

    invalid = valid;
    invalid.transitionPreparing = true;
    invalid.transitionTargetStages = 1;
    invalid.paths[invalid.transitionPath].initialized = true;
    invalid.paths[invalid.transitionPath].stageCount = 1;
    invalid.paths[invalid.transitionPath].rawProcessIndex =
        UINT64_MAX;
    EXPECT_FALSE(Resampler::ValidateProfessionalState(invalid));

    invalid.paths[invalid.transitionPath].rawProcessIndex =
        invalid.rawWriteIndex + 4096u;
    EXPECT_FALSE(Resampler::ValidateProfessionalState(invalid));

    std::vector<float> destinationRing(
        kResampleFifoFrames, 0.0f);
    float* destinationRings[1] = {destinationRing.data()};
    MultirateResamplerCandidate destinationWorkspace;
    Resampler destination;
    destination.Reset(
        destinationRings, 1, ResampleQuality::Sinc16, 1.5,
        Tables(), ResampleProfile::Professional44148,
        CandidateTables(), &destinationWorkspace);
    destinationRing[17] = 0.375f;
    const std::uint64_t frameBefore = destination.SourceFrame();
    const double ratioBefore = destination.CurrentRatio();
    EXPECT_FALSE(destination.RestoreProfessional(
        valid, destinationRings, 1, Tables(), CandidateTables(), nullptr));
    EXPECT_FALSE(destination.RestoreParkState(
        valid, destinationRings, 1, Tables(), CandidateTables()));
    EXPECT_EQ(&destinationWorkspace, destination.professional);
    EXPECT_EQ(frameBefore, destination.SourceFrame());
    EXPECT_DOUBLE_EQ(ratioBefore, destination.CurrentRatio());
    EXPECT_FLOAT_EQ(0.375f, destinationRing[17]);
    invalid = valid;
    invalid.paths[invalid.activePath].stages[0].sideWriteOffset =
        harken::kMultirateHalfbandSideFrames;
    EXPECT_FALSE(destination.RestoreProfessional(
        invalid, destinationRings, 1, Tables(),
        CandidateTables(), &destinationWorkspace));
    EXPECT_EQ(frameBefore, destination.SourceFrame());
    EXPECT_DOUBLE_EQ(ratioBefore, destination.CurrentRatio());
    EXPECT_FLOAT_EQ(0.375f, destinationRing[17]);

    invalid = valid;
    invalid.rampRemaining = 1;
    invalid.rampStep =
        (std::numeric_limits<double>::max)();
    EXPECT_FALSE(destination.RestoreProfessional(
        invalid, destinationRings, 1, Tables(),
        CandidateTables(), &destinationWorkspace));
    EXPECT_EQ(frameBefore, destination.SourceFrame());
    EXPECT_DOUBLE_EQ(ratioBefore, destination.CurrentRatio());
    EXPECT_FLOAT_EQ(0.375f, destinationRing[17]);

    invalid = valid;
    invalid.ratio = 1.0;
    invalid.targetRatio = 1.0;
    invalid.rampRemaining = 4;
    invalid.rampStep = -0.49;
    EXPECT_FALSE(destination.RestoreProfessional(
        invalid, destinationRings, 1, Tables(),
        CandidateTables(), &destinationWorkspace));
    EXPECT_EQ(frameBefore, destination.SourceFrame());
    EXPECT_DOUBLE_EQ(ratioBefore, destination.CurrentRatio());
    EXPECT_FLOAT_EQ(0.375f, destinationRing[17]);
}

TEST(ResamplerProfessionalFacade,
     DefaultProduceDoesNotInstrumentAndMatchesInstrumentedDspState) {
    std::vector<float> input =
        harkentest::MakeSine(256, 330.0, 48000.0, 0.8f);
    PCMView view;
    view.channels[0] = input.data();
    view.channelCount = 1;
    view.stride = 1;
    view.sampleRate = 48000;
    view.frames = input.size();
    std::vector<float> defaultRing(kResampleFifoFrames, 0.0f);
    std::vector<float> instrumentedRing(kResampleFifoFrames, 0.0f);
    float* defaultRings[1] = {defaultRing.data()};
    float* instrumentedRings[1] = {instrumentedRing.data()};
    MultirateResamplerCandidate regularWorkspace;
    Resampler regular;
    MultirateResamplerCandidate instrumentedWorkspace;
    Resampler instrumented;
    regular.Reset(
        defaultRings, 1, ResampleQuality::Sinc16, 3.25, Tables(),
        ResampleProfile::Professional44148, CandidateTables(), &regularWorkspace);
    instrumented.Reset(
        instrumentedRings, 1, ResampleQuality::Sinc16, 3.25,
        Tables(), ResampleProfile::Professional44148,
        CandidateTables(), &instrumentedWorkspace);
    regular.PushPCM(view, 0, 256);
    instrumented.PushPCM(view, 0, 256);
    regular.UpdateRatio(3.75, 16);
    instrumented.UpdateRatio(3.75, 16);

    float regularOutput[16] = {};
    float instrumentedOutput[16] = {};
    float* regularPlanes[1] = {regularOutput};
    float* instrumentedPlanes[1] = {instrumentedOutput};
    ASSERT_EQ(16u, regular.Produce(regularPlanes, 0, 16));
    ASSERT_EQ(
        16u,
        instrumentedWorkspace.ProduceInstrumentedForTesting(
            instrumentedPlanes, 0, 16));
    EXPECT_EQ(0u, regularWorkspace.counters.rawFramesRead);
    EXPECT_EQ(
        0u,
        regularWorkspace.counters.halfbandMultiplyAccumulates);
    EXPECT_EQ(
        0u,
        regularWorkspace.counters.residualMultiplyAccumulates);
    EXPECT_GT(instrumentedWorkspace.counters.rawFramesRead, 0u);
    for (std::uint32_t frame = 0; frame < 16; ++frame)
        EXPECT_FLOAT_EQ(regularOutput[frame],
                        instrumentedOutput[frame]);

    harken::ProfessionalResamplerState regularState;
    harken::ProfessionalResamplerState instrumentedState;
    ASSERT_TRUE(regular.CaptureProfessional(&regularState));
    ASSERT_TRUE(
        instrumented.CaptureProfessional(&instrumentedState));
    EXPECT_EQ(regularState.sourceFrame,
              instrumentedState.sourceFrame);
    EXPECT_DOUBLE_EQ(regularState.sourceFraction,
                     instrumentedState.sourceFraction);
    EXPECT_DOUBLE_EQ(regularState.ratio,
                     instrumentedState.ratio);
    EXPECT_DOUBLE_EQ(regularState.targetRatio,
                     instrumentedState.targetRatio);
    EXPECT_EQ(regularState.rampRemaining,
              instrumentedState.rampRemaining);
    EXPECT_EQ(regularState.activePath,
              instrumentedState.activePath);
    EXPECT_EQ(regularState.transitionPath,
              instrumentedState.transitionPath);
    EXPECT_EQ(regularState.rawStartIndex,
              instrumentedState.rawStartIndex);
    EXPECT_EQ(regularState.rawFrameCount,
              instrumentedState.rawFrameCount);
    EXPECT_EQ(0, std::memcmp(regularState.paths,
                             instrumentedState.paths,
                             sizeof(regularState.paths)));
    EXPECT_EQ(0, std::memcmp(
                     regularState.rawFrames,
                     instrumentedState.rawFrames,
                     sizeof(regularState.rawFrames)));
}

TEST(ResamplerProfessionalFacade,
     EightChannelResidualRampAndOverlapRemainBitExact) {
    constexpr std::uint32_t channels = 8;
    constexpr std::size_t inputFrames = 2048;
    constexpr std::uint32_t outputFrames = 192;
    std::vector<std::vector<float>> input(
        channels, std::vector<float>(inputFrames, 0.0f));
    PCMView view;
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        for (std::size_t frame = 0; frame < inputFrames; ++frame) {
            const int value = static_cast<int>(
                (frame * 23u + ch * 41u) % 251u) - 125;
            input[ch][frame] = static_cast<float>(value) * 0.004f;
        }
        view.channels[ch] = input[ch].data();
    }
    view.channelCount = channels;
    view.stride = 1;
    view.sampleRate = 48000;
    view.frames = inputFrames;

    std::vector<float> regularRing(
        static_cast<std::size_t>(channels) * kResampleFifoFrames, 0.0f);
    std::vector<float> instrumentedRing(regularRing.size(), 0.0f);
    float* regularRings[harken::kMaxChannels] = {};
    float* instrumentedRings[harken::kMaxChannels] = {};
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        regularRings[ch] =
            regularRing.data() +
            static_cast<std::size_t>(ch) * kResampleFifoFrames;
        instrumentedRings[ch] =
            instrumentedRing.data() +
            static_cast<std::size_t>(ch) * kResampleFifoFrames;
    }

    MultirateResamplerCandidate regularWorkspace;
    Resampler regular;
    MultirateResamplerCandidate instrumentedWorkspace;
    Resampler instrumented;
    regular.Reset(
        regularRings, channels, ResampleQuality::Sinc16, 1.9, Tables(),
        ResampleProfile::Professional44148, CandidateTables(), &regularWorkspace);
    instrumented.Reset(
        instrumentedRings, channels, ResampleQuality::Sinc16, 1.9,
        Tables(), ResampleProfile::Professional44148, CandidateTables(), &instrumentedWorkspace);
    const std::uint32_t feed = regular.FreeFrames();
    ASSERT_EQ(feed, instrumented.FreeFrames());
    regular.PushPCM(view, 0, feed);
    instrumented.PushPCM(view, 0, feed);
    regular.UpdateRatio(2.1, 64);
    instrumented.UpdateRatio(2.1, 64);

    std::vector<std::vector<float>> regularOutput(
        channels, std::vector<float>(outputFrames, 0.0f));
    std::vector<std::vector<float>> instrumentedOutput(
        channels, std::vector<float>(outputFrames, 0.0f));
    float* regularPlanes[harken::kMaxChannels] = {};
    float* instrumentedPlanes[harken::kMaxChannels] = {};
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        regularPlanes[ch] = regularOutput[ch].data();
        instrumentedPlanes[ch] = instrumentedOutput[ch].data();
    }
    ASSERT_EQ(outputFrames,
              regular.Produce(regularPlanes, 0, outputFrames));
    ASSERT_EQ(outputFrames,
              instrumentedWorkspace.ProducePerChannelResidualForTesting(
                  instrumentedPlanes, 0, outputFrames));
    for (std::uint32_t ch = 0; ch < channels; ++ch) {
        EXPECT_EQ(0, std::memcmp(regularOutput[ch].data(),
                                 instrumentedOutput[ch].data(),
                                 outputFrames * sizeof(float)))
            << "channel " << ch;
    }
}

TEST(ResamplerProfessionalFacade,
     HalfbandBurstRemainsBitExactForStereoAndEightChannelTransitions) {
    constexpr std::size_t inputFrames = 20000;
    constexpr std::uint32_t outputFrames = 192;
    for (const std::uint32_t channels : {2u, 8u}) {
        for (const double initialRatio : {1.9, 15.9}) {
            SCOPED_TRACE(channels);
            SCOPED_TRACE(initialRatio);
            const double targetRatio = initialRatio + 0.2;
            std::vector<std::vector<float>> input(
                channels, std::vector<float>(inputFrames, 0.0f));
            PCMView view;
            for (std::uint32_t ch = 0; ch < channels; ++ch) {
                for (std::size_t frame = 0; frame < inputFrames; ++frame) {
                    const int value = static_cast<int>(
                        (frame * 31u + ch * 47u) % 257u) - 128;
                    input[ch][frame] = static_cast<float>(value) * 0.003f;
                }
                view.channels[ch] = input[ch].data();
            }
            view.channelCount = channels;
            view.stride = 1;
            view.sampleRate = 48000;
            view.frames = inputFrames;

            std::vector<float> regularRing(
                static_cast<std::size_t>(channels) *
                    kResampleFifoFrames,
                0.0f);
            std::vector<float> legacyRing(regularRing.size(), 0.0f);
            float* regularRings[harken::kMaxChannels] = {};
            float* legacyRings[harken::kMaxChannels] = {};
            for (std::uint32_t ch = 0; ch < channels; ++ch) {
                regularRings[ch] =
                    regularRing.data() +
                    static_cast<std::size_t>(ch) * kResampleFifoFrames;
                legacyRings[ch] =
                    legacyRing.data() +
                    static_cast<std::size_t>(ch) * kResampleFifoFrames;
            }

            MultirateResamplerCandidate regularWorkspace;
            Resampler regular;
            MultirateResamplerCandidate legacyWorkspace;
            Resampler legacy;
            regular.Reset(
                regularRings, channels, ResampleQuality::Sinc16,
                initialRatio, Tables(),
                ResampleProfile::Professional44148, CandidateTables(), &regularWorkspace);
            legacy.Reset(
                legacyRings, channels, ResampleQuality::Sinc16,
                initialRatio, Tables(),
                ResampleProfile::Professional44148, CandidateTables(), &legacyWorkspace);
            regular.UpdateRatio(targetRatio, 64);
            legacy.UpdateRatio(targetRatio, 64);

            std::vector<std::vector<float>> regularOutput(
                channels, std::vector<float>(outputFrames, 0.0f));
            std::vector<std::vector<float>> legacyOutput(
                channels, std::vector<float>(outputFrames, 0.0f));
            float* regularPlanes[harken::kMaxChannels] = {};
            float* legacyPlanes[harken::kMaxChannels] = {};
            for (std::uint32_t ch = 0; ch < channels; ++ch) {
                regularPlanes[ch] = regularOutput[ch].data();
                legacyPlanes[ch] = legacyOutput[ch].data();
            }

            std::uint64_t sourceCursor = 0;
            std::uint32_t produced = 0;
            for (std::size_t iteration = 0;
                 iteration < 256u && produced < outputFrames;
                 ++iteration) {
                const std::uint32_t freeFrames = regular.FreeFrames();
                if (sourceCursor < inputFrames && freeFrames != 0) {
                    const std::uint32_t feed = static_cast<std::uint32_t>(
                        std::min<std::uint64_t>(
                            freeFrames, inputFrames - sourceCursor));
                    regular.PushPCM(view, sourceCursor, feed);
                    legacy.PushPCM(view, sourceCursor, feed);
                    sourceCursor += feed;
                }
                const std::uint32_t request = outputFrames - produced;
                const std::uint32_t regularProduced = regular.Produce(
                    regularPlanes, produced, request);
                const std::uint32_t legacyProduced =
                    legacyWorkspace.ProducePerChannelResidualForTesting(
                        legacyPlanes, produced, request);
                ASSERT_EQ(regularProduced, legacyProduced);
                produced += regularProduced;
                ASSERT_TRUE(regularProduced != 0 ||
                            sourceCursor == inputFrames ||
                            freeFrames != 0);
            }
            ASSERT_EQ(outputFrames, produced);
            for (std::uint32_t ch = 0; ch < channels; ++ch) {
                EXPECT_EQ(0, std::memcmp(
                                 regularOutput[ch].data(),
                                 legacyOutput[ch].data(),
                                 outputFrames * sizeof(float)))
                    << "channel " << ch;
            }
        }
    }
}

TEST(ResamplerProfessionalFacade,
     StreamParkTokenIsBoundedAndPointerFree) {
    EXPECT_LE(sizeof(harken::StreamParkToken),
              harken::kStreamParkTokenBytes);
    EXPECT_TRUE(
        std::is_trivially_copyable_v<harken::StreamParkToken>);
}

TEST(ResamplerHighRatio, DecimationRatiosProduceThenExhaust) {
    constexpr std::size_t kFrames = 10000;
    const std::vector<float> sine =
        harkentest::MakeSine(kFrames, 1000.0, 48000.0, 0.8f);
    HARKENTEST_ASSERT_NON_SILENT(sine);

    for (const RatioCase& c : kDecimationRatios) {
        SCOPED_TRACE(c.name);
        const std::size_t expected =
            static_cast<std::size_t>(std::ceil(kFrames / c.ratio));
        const DriveResult res =
            DriveResampler(ResampleQuality::Hermite, c.ratio, sine,
                           expected + 256);
        EXPECT_TRUE(res.exhausted)
            << "stage never reported exhausted at ratio " << c.ratio;
        EXPECT_LE(res.out.size(),
                  expected + 2) << "over-produced at ratio " << c.ratio;
        EXPECT_GE(res.out.size() + 2, expected)
            << "under-produced at ratio " << c.ratio;
    }
}

TEST(ResamplerHighRatio, PhaseLockedDivisorsProduceThenExhaust) {
    constexpr std::size_t kFrames = 10000;
    const std::vector<float> sine =
        harkentest::MakeSine(kFrames, 1000.0, 48000.0, 0.8f);
    HARKENTEST_ASSERT_NON_SILENT(sine);

    for (const RatioCase& c : kPhaseLockedGuards) {
        SCOPED_TRACE(c.name);
        const std::size_t expected =
            static_cast<std::size_t>(std::ceil(kFrames / c.ratio));
        const DriveResult res =
            DriveResampler(ResampleQuality::Hermite, c.ratio, sine,
                           expected + 256);
        EXPECT_TRUE(res.exhausted)
            << "stage never reported exhausted at ratio " << c.ratio;
        EXPECT_LE(res.out.size(), expected + 2)
            << "over-produced at ratio " << c.ratio;
        EXPECT_GE(res.out.size() + 2, expected)
            << "under-produced at ratio " << c.ratio;
    }
}

// Once the read head has jumped past the write head, FreeFrames must retain
// the fixed history reserve while still reporting writable space, and a fresh
// chunk must let Produce resume. The former unsigned underflow reported zero
// free frames, so the feed could push nothing and Produce never restarted.
TEST(ResamplerHighRatio, FreeFramesRecoversAfterReadHeadPassesWriteHead) {
    constexpr double kRatio = 60.0;
    const std::vector<float> sine =
        harkentest::MakeSine(10000, 1000.0, 48000.0, 0.8f);
    HARKENTEST_ASSERT_NON_SILENT(sine);

    std::vector<float> ring(kResampleFifoFrames, 0.0f);
    float* rings[1] = {ring.data()};

    PCMView view;
    view.channels[0] = sine.data();
    view.channelCount = 1;
    view.stride = 1;
    view.sampleRate = 48000;
    view.frames = sine.size();

    Resampler rs;
    rs.Reset(rings, 1, ResampleQuality::Hermite, kRatio, Tables());

    std::vector<float> out(512, 0.0f);
    float* dst[1] = {out.data()};

    // Hermite downsampling reserves 57 history frames. With 118 real frames,
    // writeIndex is 126: outputs at p=8 and p=68 both have their required
    // look-ahead, then two 60-frame advances land the read head at p=128,
    // strictly past the write head. This is the state the underflow mishandles.
    constexpr std::uint32_t kPrimeFrames = 118;
    static_assert(kPrimeFrames <= kResampleChunkFrames,
                  "recovery probe must fit one source chunk");
    const std::uint32_t primed = static_cast<std::uint32_t>(
        std::min<std::uint64_t>({kPrimeFrames, view.frames,
                                 rs.FreeFrames()}));
    ASSERT_EQ(kPrimeFrames, primed);
    rs.PushPCM(view, 0, primed);
    std::uint64_t cursor = primed;
    const std::uint32_t firstProduced = rs.Produce(dst, 0, 512);
    ASSERT_EQ(2u, firstProduced) << "priming phase changed its derived geometry";
    ASSERT_DOUBLE_EQ(128.0, rs.readPos);
    ASSERT_EQ(126u, rs.writeIndex);
    ASSERT_GT(rs.readPos, static_cast<double>(rs.writeIndex))
        << "read head did not pass the write head; ratio too low to test recovery";

    // The fixed 57-frame reserve starts at absolute frame 71, leaving 55 live
    // frames through writeIndex 126 and therefore 457 writable FIFO frames.
    // The unsigned-underflow defect reports zero here.
    EXPECT_EQ(kResampleFifoFrames - 55, rs.FreeFrames())
        << "FreeFrames reported no writable space with the read head ahead "
           "of the write head (readPos=" << rs.readPos
        << " writeIndex=" << rs.writeIndex << ")";

    // Feeding against that free space and producing must yield output: the
    // write head catches back up past the read head's look-ahead window.
    const std::uint32_t chunk = static_cast<std::uint32_t>(
        std::min<std::uint64_t>({kResampleChunkFrames, view.frames - cursor,
                                 rs.FreeFrames()}));
    rs.PushPCM(view, cursor, chunk);
    const std::uint32_t afterFeed = rs.Produce(dst, 0, 512);
    EXPECT_GT(afterFeed, 0u)
        << "Produce stayed starved after feeding a chunk past the read head "
           "(FreeFrames=" << rs.FreeFrames() << " readPos=" << rs.readPos
        << " writeIndex=" << rs.writeIndex << ")";
}

// Low ratios never overtake the available input, so they exhaust with a
// proportional output count in both the current and fixed stage — a guard that the fix
// does not disturb the normal operating range.
TEST(ResamplerHighRatio, LowRatiosKeepProportionalOutput) {
    constexpr std::size_t kFrames = 1000;
    const std::vector<float> sine =
        harkentest::MakeSine(kFrames, 1000.0, 48000.0, 0.5f);
    HARKENTEST_ASSERT_NON_SILENT(sine);

    const RatioCase kLowRatios[] = {
        {0.5, "0.5:1"},
        {1.7, "1.7:1"},
        {2.0, "2:1"},
    };
    for (const RatioCase& c : kLowRatios) {
        SCOPED_TRACE(c.name);
        const std::size_t expected =
            static_cast<std::size_t>(std::ceil(kFrames / c.ratio));
        const DriveResult res =
            DriveResampler(ResampleQuality::Hermite, c.ratio, sine,
                           expected + 256);
        EXPECT_TRUE(res.exhausted)
            << "stage never reported exhausted at ratio " << c.ratio;
        EXPECT_LE(res.out.size(), expected + 2)
            << "over-produced at ratio " << c.ratio;
        EXPECT_GE(res.out.size() + 2, expected)
            << "under-produced at ratio " << c.ratio;
    }
}

TEST(ResamplerHighRatio, UpwardRatioRampCrossesSixteenWithBoundedStep) {
    const CrossingDirection direction = {
        15.5, 16.5,
        14.5, 15.5,
        16.5, 17.5,
        true, "upward 15.5x to 16.5x",
    };
    ExpectSixteenCrossingBoundedStep(direction);
}

TEST(ResamplerHighRatio, DownwardRatioRampCrossesSixteenWithBoundedStep) {
    const CrossingDirection direction = {
        16.5, 15.5,
        15.5, 14.5,
        17.5, 16.5,
        false, "downward 16.5x to 15.5x",
    };
    ExpectSixteenCrossingBoundedStep(direction);
}

} // namespace
