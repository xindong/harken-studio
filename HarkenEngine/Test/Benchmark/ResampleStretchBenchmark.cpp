// Worst-case Sinc16 characterization around the direct-kernel/high-ratio
// boundary through the null backend. Each voice-count scan is paired with an
// otherwise identical ratio-1 baseline so fixed render, mix, output-scan, and
// hashing costs stay visible. Timing is reported only; this executable contains
// no performance threshold.
#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Engine.h"
#include "Common/RawVoiceTestSupport.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <vector>

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kBlockFrames = 256;
constexpr std::uint32_t kWarmupBlocks = 64;
constexpr std::uint32_t kMeasureBlocks = 1000;
constexpr std::uint32_t kRepetitions = 5;
constexpr std::uint32_t kSourceFrames = kSampleRate;
constexpr double kBlockBudgetUs =
    1000000.0 * static_cast<double>(kBlockFrames) /
    static_cast<double>(kSampleRate);
constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

struct Scenario {
    const char* name;
    std::uint32_t voices;
};

struct Sample {
    double usPerBlock = 0.0;
    std::uint64_t hash = kFnvOffset;
    std::uint64_t arenaOverflowCount = 0;
    float peak = 0.0f;
    bool ok = false;
    const char* error = nullptr;
};

struct Summary {
    double minimumUs = 0.0;
    double medianUs = 0.0;
    std::uint64_t hash = kFnvOffset;
    std::uint64_t arenaOverflowCount = 0;
    float peak = 0.0f;
};

class EngineGuard {
public:
    explicit EngineGuard(bool active) : m_active(active) {}
    ~EngineGuard() {
        if (m_active) {
            Vn_Term();
        }
    }

    EngineGuard(const EngineGuard&) = delete;
    EngineGuard& operator=(const EngineGuard&) = delete;

private:
    bool m_active;
};

std::vector<float> MakeInterleavedSine(std::uint32_t channels) {
    std::vector<float> samples(static_cast<std::size_t>(kSourceFrames) *
                               channels);
    constexpr double kPi = 3.14159265358979323846;
    for (std::uint32_t frame = 0; frame < kSourceFrames; ++frame) {
        const float value = 0.25f * static_cast<float>(std::sin(
                                         2.0 * kPi * 440.0 *
                                         static_cast<double>(frame) /
                                         static_cast<double>(kSampleRate)));
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            samples[static_cast<std::size_t>(frame) * channels + channel] =
                value;
        }
    }
    return samples;
}

void HashAndScan(const float* samples, std::size_t count, std::uint64_t* hash,
                 float* peak) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(samples);
    for (std::size_t i = 0; i < count * sizeof(float); ++i) {
        *hash ^= bytes[i];
        *hash *= kFnvPrime;
    }
    for (std::size_t i = 0; i < count; ++i) {
        *peak = std::max(*peak, std::fabs(samples[i]));
    }
}

void HashWord(std::uint64_t word, std::uint64_t* hash) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(&word);
    for (std::size_t i = 0; i < sizeof(word); ++i) {
        *hash ^= bytes[i];
        *hash *= kFnvPrime;
    }
}

Sample RunOnce(const Scenario& scenario, double ratio) {
    constexpr std::uint32_t kChannels = 2;
    Sample sample;
    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.deviceMode = VN_DEVICE_NULL;
    config.maxVoices = scenario.voices;
    config.blockFrames = kBlockFrames;
    config.sampleRate = kSampleRate;
    config.mixChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    config.deviceChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    config.renderWorkerCount = 0;
    config.arenaBytes = 1u << 20;

    const VnResult initialized = Vn_Init(&config);
    if (initialized != VN_OK) {
        sample.error = "Vn_Init failed";
        return sample;
    }
    EngineGuard guard(true);
    harken::Engine* engine = harken::detail::ActiveEngine();
    if (engine == nullptr) {
        sample.error = "active engine missing";
        return sample;
    }
    if (engine->nullBackend.BlockFrames() != kBlockFrames ||
        engine->nullBackend.Channels() != 2) {
        sample.error = "null backend geometry mismatch";
        return sample;
    }

    const std::vector<float> source = MakeInterleavedSine(kChannels);
    VnPCMDesc pcm{};
    pcm.structSize = sizeof(VnPCMDesc);
    pcm.sampleRate = kSampleRate;
    pcm.channels = kChannels;
    pcm.layout = VN_PCM_LAYOUT_INTERLEAVED;
    pcm.policy = VN_PCM_POLICY_COPY;
    pcm.frames = kSourceFrames;
    pcm.samplesInterleaved = source.data();
    if (Vn_RegisterPCM(harkentest::ObjectId(1), &pcm) != VN_OK) {
        sample.error = "Vn_RegisterPCM failed";
        return sample;
    }

    for (std::uint32_t voice = 0; voice < scenario.voices; ++voice) {
        VnPlayDesc play{};
        play.structSize = sizeof(VnPlayDesc);
        play.soundId = harkentest::ObjectId(1);
        play.busId = VN_BUS_MASTER;
        play.pitchRatio = static_cast<float>(ratio);
        play.loopCount = 0;
        VnVoiceHandle handle = 0;
        if (harkentest::PlayRaw(&play, &handle) != VN_OK || handle == 0) {
            sample.error = "raw voice start failed";
            return sample;
        }
        harken::VoiceRenderContext* context = engine->voices.Resolve(handle);
        if (context == nullptr) {
            sample.error = "voice context missing";
            return sample;
        }
        context->resampler.quality = harken::ResampleQuality::Sinc16;
    }
    if (Vn_Update() != VN_OK) {
        sample.error = "Vn_Update failed";
        return sample;
    }

    engine->nullBackend.Pump(kWarmupBlocks);
    const auto started = std::chrono::steady_clock::now();
    for (std::uint32_t block = 0; block < kMeasureBlocks; ++block) {
        engine->nullBackend.Pump(1);
        HashAndScan(engine->nullBackend.LastBlock(),
                    static_cast<std::size_t>(kBlockFrames) * 2, &sample.hash,
                    &sample.peak);
    }
    const auto finished = std::chrono::steady_clock::now();
    sample.usPerBlock =
        std::chrono::duration<double, std::micro>(finished - started).count() /
        static_cast<double>(kMeasureBlocks);

    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    if (Vn_CaptureSnapshot(&snapshot) != VN_OK) {
        sample.error = "Vn_CaptureSnapshot failed";
        return sample;
    }
    if (snapshot.activeVoiceCount != scenario.voices) {
        sample.error = "voice ended during measurement";
        return sample;
    }
    sample.arenaOverflowCount = snapshot.arenaOverflowCount;
    if (!(sample.peak > 0.0f)) {
        sample.error = "measured output was silent";
        return sample;
    }
    sample.ok = true;
    return sample;
}

bool Measure(const Scenario& scenario, double ratio, Summary* summary) {
    std::array<double, kRepetitions> timings{};
    summary->hash = kFnvOffset;
    summary->arenaOverflowCount = 0;
    summary->peak = 0.0f;
    for (std::uint32_t repetition = 0; repetition < kRepetitions;
         ++repetition) {
        const Sample sample = RunOnce(scenario, ratio);
        if (!sample.ok) {
            std::fprintf(stderr, "FAIL %s ratio %.1f repetition %u: %s\n",
                         scenario.name, ratio, repetition + 1,
                         sample.error != nullptr ? sample.error
                                                 : "unknown error");
            return false;
        }
        timings[repetition] = sample.usPerBlock;
        HashWord(sample.hash, &summary->hash);
        summary->arenaOverflowCount =
            std::max(summary->arenaOverflowCount, sample.arenaOverflowCount);
        summary->peak = std::max(summary->peak, sample.peak);
    }
    std::sort(timings.begin(), timings.end());
    summary->minimumUs = timings.front();
    summary->medianUs = timings[kRepetitions / 2];
    return true;
}

void PrintRow(const Scenario& scenario, const char* path, double ratio,
              const Summary& result, double baselineMedianUs) {
    const double budgetPercent = 100.0 * result.medianUs / kBlockBudgetUs;
    const double deltaUs = result.medianUs - baselineMedianUs;
    const double deltaPercent = 100.0 * deltaUs / kBlockBudgetUs;
    std::printf("%-11s %-12s %5.1f %12.3f %12.3f %10.3f %12.3f %11.3f "
                "%016llx %.6f %10llu%s\n",
                scenario.name, path, ratio, result.minimumUs,
                result.medianUs, budgetPercent, deltaUs, deltaPercent,
                static_cast<unsigned long long>(result.hash), result.peak,
                static_cast<unsigned long long>(result.arenaOverflowCount),
                result.arenaOverflowCount != 0
                    ? " INVALID(arena overflow)"
                    : "");
}

} // namespace

int main() {
    constexpr Scenario scenarios[] = {
        {"1 voice", 1},
        {"4 voices", 4},
        {"8 voices", 8},
        {"16 voices", 16},
        {"32 voices", 32},
    };
    constexpr double ratios[] = {
        15.9,
        16.0,
        16.1,
    };

    std::printf("Resample boundary: Sinc16, %u Hz, %u frames, warmup %u, "
                "measure %u, repetitions %u\n",
                kSampleRate, kBlockFrames, kWarmupBlocks, kMeasureBlocks,
                kRepetitions);
#ifdef NDEBUG
    std::printf("build_mode=NDEBUG ");
#else
    std::printf("build_mode=debug ");
#endif
    std::printf("timed_region=Pump+FNV1a+peak_scan budget_us=%.3f\n",
                kBlockBudgetUs);
    std::printf("%-11s %-12s %5s %12s %12s %10s %12s %11s %16s %s %10s\n",
                "scenario", "kernel_path", "ratio", "min_us", "median_us",
                "budget_%", "delta_us", "delta_%", "hash", "peak",
                "arena_ovf");

    bool hasInvalidScenario = false;
    for (const Scenario& scenario : scenarios) {
        Summary baseline;
        if (!Measure(scenario, 1.0, &baseline)) {
            return 2;
        }
        PrintRow(scenario, "baseline", 1.0, baseline, baseline.medianUs);
        hasInvalidScenario =
            hasInvalidScenario || baseline.arenaOverflowCount != 0;
        for (double ratio : ratios) {
            Summary stretched;
            if (!Measure(scenario, ratio, &stretched)) {
                return 2;
            }
            PrintRow(scenario, ratio <= 16.0 ? "direct-sinc" : "streaming-lp",
                     ratio, stretched, baseline.medianUs);
            hasInvalidScenario =
                hasInvalidScenario || stretched.arenaOverflowCount != 0;
        }
    }
    return hasInvalidScenario ? 3 : 0;
}
