// Release-build insert-effect microbenchmark template. Later built-ins can
// copy this file and replace the state type, effect id, descriptor setup, and
// benchmark name while retaining the block-throughput and aligned pool-memory
// metrics. The timed region contains only process() calls; input restoration
// and detector-state seeding stay outside it.
#include "harken/render/BuiltinEffects.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

bool IsBenchmarkProcessPinned();
bool IsBenchmarkProcessPrioritySet();
std::uint32_t BenchmarkPinnedCpuCount();

namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kBlockFrames = 256;
constexpr std::uint32_t kChannels = 2;
constexpr std::uint32_t kDefaultInstances = 64;
constexpr std::uint32_t kDefaultWarmupBlocks = 200;
constexpr std::uint32_t kDefaultMeasuredBlocks = 2000;
constexpr std::uint32_t kCompressorDefaultInstances = 1;
constexpr std::uint32_t kCompressorDefaultWarmupBlocks = 2000;
constexpr std::uint32_t kCompressorDefaultMeasuredBlocks = 10000;
constexpr float kInput = 0.25f;
constexpr float kSeedGain = 0.25f;
constexpr std::uint64_t kFnvOffsetBasis = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

struct Options {
    std::uint32_t instances = kDefaultInstances;
    std::uint32_t warmupBlocks = kDefaultWarmupBlocks;
    std::uint32_t measuredBlocks = kDefaultMeasuredBlocks;
};

volatile float g_limiterBenchmarkSink = 0.0f;
volatile float g_compressorBenchmarkSink = 0.0f;

void HashFloatBytes(std::uint64_t& hash, const float* samples,
                    std::size_t count) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(samples);
    for (std::size_t i = 0; i < count * sizeof(float); ++i) {
        hash ^= bytes[i];
        hash *= kFnvPrime;
    }
}

void PrintUsage() {
    std::printf(
        "Usage: harken_benchmarks --filter limiter-release "
        "[--instances N] [--warmup N] [--blocks N]\n");
}

bool ParseCount(const char* text, std::uint32_t* out) {
    if (text == nullptr || text[0] == '\0' || out == nullptr) {
        return false;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || parsed == 0 ||
        parsed > std::numeric_limits<std::uint32_t>::max()) {
        return false;
    }
    *out = static_cast<std::uint32_t>(parsed);
    return true;
}

bool ParseOptions(int argc, char* argv[], Options* out) {
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--instances") == 0) {
            if (++i >= argc || !ParseCount(argv[i], &out->instances)) {
                return false;
            }
        } else if (std::strcmp(argv[i], "--warmup") == 0) {
            if (++i >= argc || !ParseCount(argv[i], &out->warmupBlocks)) {
                return false;
            }
        } else if (std::strcmp(argv[i], "--blocks") == 0) {
            if (++i >= argc || !ParseCount(argv[i], &out->measuredBlocks)) {
                return false;
            }
        } else if (std::strcmp(argv[i], "--help") == 0) {
            PrintUsage();
            std::exit(0);
        } else {
            return false;
        }
    }
    return true;
}

constexpr std::size_t AlignedPoolBytes(std::size_t stateBytes) {
    constexpr std::size_t unitBytes = sizeof(std::max_align_t);
    return ((stateBytes + unitBytes - 1) / unitBytes) * unitBytes;
}

} // namespace

int RunLimiterReleaseBenchmark(int argc, char* argv[]) {
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
        PrintUsage();
        return 2;
    }

#ifndef NDEBUG
    std::printf(
        "{\"benchmark\":\"limiter-release\",\"status\":\"skipped\","
        "\"reason\":\"debug-build\",\"sample_rate\":%u,"
        "\"block_frames\":%u,\"channels\":%u,"
        "\"release_ms\":1000.0,\"seed_gain\":%.3f,"
        "\"state_bytes\":%zu,\"pool_bytes_per_instance\":%zu}\n",
        kSampleRate, kBlockFrames, kChannels, kSeedGain,
        sizeof(harken::LimiterState),
        AlignedPoolBytes(sizeof(harken::LimiterState)));
    return 0;
#else
    std::vector<harken::LimiterState> states(options.instances);
    std::vector<harken::InsertEffect> effects(options.instances);
    const std::size_t samplesPerInstance =
        static_cast<std::size_t>(kChannels) * kBlockFrames;
    std::vector<float> samples(
        static_cast<std::size_t>(options.instances) * samplesPerInstance,
        kInput);

    for (std::uint32_t i = 0; i < options.instances; ++i) {
        states[i].sampleRate = kSampleRate;
        effects[i] = harken::BuildEffect(VN_EFFECT_LIMITER, &states[i]);
        effects[i].setParam(&states[i], harken::kLimiterParamCeilingDb,
                            -12.0f);
        effects[i].setParam(&states[i], harken::kLimiterParamReleaseMs,
                            1000.0f);
        effects[i].reset(&states[i]);
    }

    double totalSeconds = 0.0;
    const std::uint64_t totalOuterBlocks =
        static_cast<std::uint64_t>(options.warmupBlocks) +
        options.measuredBlocks;
    for (std::uint64_t block = 0; block < totalOuterBlocks; ++block) {
        std::fill(samples.begin(), samples.end(), kInput);
        for (harken::LimiterState& state : states) {
            state.currentGain = kSeedGain;
        }
        const auto begin = std::chrono::steady_clock::now();
        for (std::uint32_t i = 0; i < options.instances; ++i) {
            float* channels[kChannels] = {
                samples.data() +
                    static_cast<std::size_t>(i) * samplesPerInstance,
                samples.data() +
                    static_cast<std::size_t>(i) * samplesPerInstance +
                    kBlockFrames};
            effects[i].process(&states[i], channels, kChannels, 0,
                               kBlockFrames);
        }
        const auto end = std::chrono::steady_clock::now();
        if (block >= options.warmupBlocks) {
            totalSeconds +=
                std::chrono::duration<double>(end - begin).count();
        }
    }
    g_limiterBenchmarkSink = samples.back() + states.back().currentGain;

    const std::uint64_t instanceBlocks =
        static_cast<std::uint64_t>(options.measuredBlocks) *
        options.instances;
    const std::uint64_t frames =
        instanceBlocks * static_cast<std::uint64_t>(kBlockFrames);
    const std::uint64_t sampleChannels =
        frames * static_cast<std::uint64_t>(kChannels);
    const double instanceBlocksPerSecond =
        static_cast<double>(instanceBlocks) / totalSeconds;
    const double framesPerSecond =
        static_cast<double>(frames) / totalSeconds;
    const double nsPerSampleChannel =
        totalSeconds * 1.0e9 / static_cast<double>(sampleChannels);

    std::printf(
        "{\"benchmark\":\"limiter-release\",\"status\":\"measured\","
        "\"build\":\"release\",\"sample_rate\":%u,"
        "\"block_frames\":%u,\"channels\":%u,\"instances\":%u,"
        "\"release_ms\":1000.0,\"seed_gain\":%.3f,"
        "\"warmup_blocks\":%u,\"measured_blocks\":%u,"
        "\"elapsed_seconds\":%.9f,\"instance_blocks_per_second\":%.3f,"
        "\"frames_per_second\":%.3f,\"ns_per_sample_channel\":%.6f,"
        "\"state_bytes\":%zu,\"pool_bytes_per_instance\":%zu}\n",
        kSampleRate, kBlockFrames, kChannels, options.instances, kSeedGain,
        options.warmupBlocks, options.measuredBlocks, totalSeconds,
        instanceBlocksPerSecond, framesPerSecond, nsPerSampleChannel,
        sizeof(harken::LimiterState),
        AlignedPoolBytes(sizeof(harken::LimiterState)));
    return 0;
#endif
}

int RunCompressorReleaseBenchmark(int argc, char* argv[]) {
    Options options;
    options.instances = kCompressorDefaultInstances;
    options.warmupBlocks = kCompressorDefaultWarmupBlocks;
    options.measuredBlocks = kCompressorDefaultMeasuredBlocks;
    std::uint32_t channels = 8;
    std::vector<char*> common;
    common.reserve(static_cast<std::size_t>(argc));
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--channels") == 0) {
            if (++i >= argc || !ParseCount(argv[i], &channels) ||
                channels > harken::kMaxChannels) {
                std::printf(
                    "Usage: harken_benchmarks --filter compressor-release "
                    "[--instances N] [--channels 1..8] "
                    "[--warmup N] [--blocks N]\n");
                return 2;
            }
        } else {
            common.push_back(argv[i]);
        }
    }
    if (!ParseOptions(static_cast<int>(common.size()), common.data(),
                      &options)) {
        std::printf(
            "Usage: harken_benchmarks --filter compressor-release "
            "[--instances N] [--channels 1..8] "
            "[--warmup N] [--blocks N]\n");
        return 2;
    }

#ifndef NDEBUG
    std::printf(
        "{\"benchmark\":\"compressor-release\",\"status\":\"skipped\","
        "\"performance_contract_vintage\":2,"
        "\"performance_gate_scope\":\"finite-cost-regression-only\","
        "\"scoped_cost_gate_applicable\":false,"
        "\"scoped_cost_gate_passed\":false,"
        "\"deadline_misses_diagnostic_only\":true,"
        "\"deadline_miss_hard_fail_applicable\":false,"
        "\"deadline_reliability_claimed\":false,"
        "\"deadline_observation_status\":\"clear\","
        "\"runtime_pool_observability_applicable\":false,"
        "\"runtime_arena_observability_applicable\":false,"
        "\"runtime_capacity_observability_applicable\":false,"
        "\"reason\":\"debug-build\",\"sample_rate\":%u,"
        "\"block_frames\":%u,\"channels\":%u,"
        "\"state_bytes\":%zu,\"pool_bytes_per_instance\":%zu}\n",
        kSampleRate, kBlockFrames, channels,
        sizeof(harken::CompressorState),
        AlignedPoolBytes(sizeof(harken::CompressorState)));
    return 0;
#else
    std::vector<harken::CompressorState> states(options.instances);
    std::vector<harken::InsertEffect> effects(options.instances);
    const std::size_t samplesPerInstance =
        static_cast<std::size_t>(channels) * kBlockFrames;
    std::vector<float> samples(
        static_cast<std::size_t>(options.instances) * samplesPerInstance,
        0.5f);
    harken::CompressorParameters parameters{};
    parameters.thresholdDb = -18.0f;
    parameters.ratio = 4.0f;
    parameters.attackMs = 10.0f;
    parameters.releaseMs = 100.0f;
    parameters.kneeDb = 6.0f;
    parameters.makeupDb = 0.0f;
    harken::CompressorPreparedParameters prepared{};
    if (!harken::PrepareCompressorParameters(
            parameters, kSampleRate, &prepared)) {
        return 2;
    }
    for (std::uint32_t i = 0; i < options.instances; ++i) {
        harken::CompressorInit(&states[i], kSampleRate);
        harken::CompressorApplyPreparedParameters(&states[i], prepared);
        effects[i] =
            harken::BuildEffect(VN_EFFECT_COMPRESSOR, &states[i]);
    }

    std::vector<double> measuredUs;
    measuredUs.reserve(options.measuredBlocks);
    std::uint64_t outputHash = kFnvOffsetBasis;
    std::uint64_t nonfiniteOutputSampleCount = 0;
    const std::uint64_t totalOuterBlocks =
        static_cast<std::uint64_t>(options.warmupBlocks) +
        options.measuredBlocks;
    for (std::uint64_t block = 0; block < totalOuterBlocks; ++block) {
        std::fill(samples.begin(), samples.end(), 0.5f);
        const auto begin = std::chrono::steady_clock::now();
        for (std::uint32_t i = 0; i < options.instances; ++i) {
            float* planes[harken::kMaxChannels] = {};
            float* base =
                samples.data() +
                static_cast<std::size_t>(i) * samplesPerInstance;
            for (std::uint32_t ch = 0; ch < channels; ++ch) {
                planes[ch] =
                    base + static_cast<std::size_t>(ch) * kBlockFrames;
            }
            effects[i].process(&states[i], planes, channels, 0,
                               kBlockFrames);
        }
        const auto end = std::chrono::steady_clock::now();
        if (block >= options.warmupBlocks) {
            measuredUs.push_back(
                std::chrono::duration<double, std::micro>(
                    end - begin)
                    .count());
            HashFloatBytes(outputHash, samples.data(), samples.size());
            nonfiniteOutputSampleCount +=
                static_cast<std::uint64_t>(std::count_if(
                    samples.begin(), samples.end(),
                    [](float sample) { return !std::isfinite(sample); }));
        }
    }
    g_compressorBenchmarkSink =
        samples.back() + states.back().currentGain;
    std::sort(measuredUs.begin(), measuredUs.end());
    const std::size_t p99Index =
        measuredUs.empty()
            ? 0
            : (static_cast<std::size_t>(
                   std::ceil(0.99 *
                             static_cast<double>(measuredUs.size()))) -
               1);
    const double median =
        measuredUs[(measuredUs.size() - 1) / 2];
    const double p99 = measuredUs[p99Index];
    const double maximum = measuredUs.back();
    constexpr double kDeadlineUs = 5333.333;
    constexpr double kIsolatedP99BudgetUs = 106.7;
    const std::uint64_t misses = static_cast<std::uint64_t>(
        std::count_if(measuredUs.begin(), measuredUs.end(),
                      [](double us) { return us >= kDeadlineUs; }));
    const bool isolatedCostGateApplies =
        options.instances == 1 && channels == harken::kMaxChannels &&
        options.warmupBlocks == kCompressorDefaultWarmupBlocks &&
        options.measuredBlocks == kCompressorDefaultMeasuredBlocks;
    const bool outputFinite = nonfiniteOutputSampleCount == 0;
    const bool isolatedCostGatePassed =
        isolatedCostGateApplies && p99 <= kIsolatedP99BudgetUs &&
        outputFinite &&
        IsBenchmarkProcessPinned() &&
        IsBenchmarkProcessPrioritySet();
    std::printf(
        "{\"benchmark\":\"compressor-release\","
        "\"status\":\"measured\",\"build\":\"release\","
        "\"performance_contract_vintage\":2,"
        "\"performance_gate_scope\":\"finite-cost-regression-only\","
        "\"scenario\":\"isolated-7.1-self\","
        "\"sample_rate\":%u,\"block_frames\":%u,\"channels\":%u,"
        "\"instances\":%u,\"warmup_blocks\":%u,"
        "\"measured_blocks\":%u,\"median_us\":%.6f,"
        "\"p99_us\":%.6f,\"max_us\":%.6f,"
        "\"deadline_us\":%.3f,\"deadline_misses\":%llu,"
        "\"deadline_misses_diagnostic_only\":true,"
        "\"deadline_miss_hard_fail_applicable\":false,"
        "\"deadline_reliability_claimed\":false,"
        "\"deadline_observation_status\":\"%s\","
        "\"output_hash\":\"%016llx\",\"output_finite\":%s,"
        "\"nonfinite_output_sample_count\":%llu,"
        "\"p99_budget_us\":%.3f,"
        "\"scoped_cost_gate_applicable\":%s,"
        "\"scoped_cost_gate_passed\":%s,"
        "\"scoped_gate_applicable\":%s,"
        "\"scoped_gate_passed\":%s,\"scoped_gate_status\":\"%s\","
        "\"scoped_gate_meaning\":"
        "\"finite-cost-regression-only-not-hard-realtime-or-glitch-free\","
        "\"overall_runtime_gate_passed\":false,"
        "\"overall_runtime_gate_status\":"
        "\"unverified-full-matrix-and-external-evidence-required\","
        "\"missing_runtime_evidence\":[\"voice-worker-0-retained\","
        "\"bus-worker-0-paired\",\"mixed-workers-0-1-2-4\","
        "\"external-phase1-b1-f1-f2-b2\"],"
        "\"inactive_external_gate_selected\":false,"
        "\"inactive_external_gate_verified\":false,"
        "\"inactive_external_gate_status\":\"not-selected\","
        "\"process_pinned\":%s,\"pinned_cpu_count\":%u,"
        "\"priority_set\":%s,"
        "\"requested_process_priority_class\":\"high\","
        "\"percentile_method\":\"nearest-rank\","
        "\"runtime_pool_observability_applicable\":false,"
        "\"runtime_arena_observability_applicable\":false,"
        "\"runtime_capacity_observability_applicable\":false,"
        "\"runtime_observability_reason\":"
        "\"direct-kernel-isolated-no-runtime-engine\","
        "\"state_bytes\":%zu,\"pool_bytes_per_instance\":%zu,"
        "\"init_compressor_pool_bytes\":%zu}\n",
        kSampleRate, kBlockFrames, channels, options.instances,
        options.warmupBlocks, options.measuredBlocks, median, p99,
        maximum, kDeadlineUs,
        static_cast<unsigned long long>(misses),
        misses == 0 ? "clear" : "unclassified",
        static_cast<unsigned long long>(outputHash),
        outputFinite ? "true" : "false",
        static_cast<unsigned long long>(nonfiniteOutputSampleCount),
        kIsolatedP99BudgetUs,
        isolatedCostGateApplies ? "true" : "false",
        isolatedCostGatePassed ? "true" : "false",
        isolatedCostGateApplies ? "true" : "false",
        isolatedCostGatePassed ? "true" : "false",
        !isolatedCostGateApplies
            ? "not-applicable-smoke"
            : (isolatedCostGatePassed ? "passed" : "failed"),
        IsBenchmarkProcessPinned() ? "true" : "false",
        BenchmarkPinnedCpuCount(),
        IsBenchmarkProcessPrioritySet() ? "true" : "false",
        sizeof(harken::CompressorState),
        AlignedPoolBytes(sizeof(harken::CompressorState)),
        static_cast<std::size_t>(options.instances) *
            AlignedPoolBytes(sizeof(harken::CompressorState)));
    return !isolatedCostGateApplies || isolatedCostGatePassed ? 0 : 1;
#endif
}
