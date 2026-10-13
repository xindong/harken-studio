// Scaling benchmark for job-parallel voice rendering (M5 §8.3). Reports
// render throughput for a heavy voice load at renderWorkerCount 0, 1, 2, and 4
// — a reported number, honestly, NOT a pass/fail correctness gate (that is the
// byte-identity determinism test in the unit suite). It lives under
// Runtime/Benchmark/ and builds as its own executable that is deliberately not
// registered with ctest, so the correctness suite stays free of timing.
//
// The one thing it does assert is a safety cross-check: the output of every
// worker count is byte-identical to the serial baseline (an FNV-1a hash of the
// rendered PCM), so a "speedup" that quietly changed the mix is caught. A hash
// mismatch is the only nonzero exit.
#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Engine.h"
#include "Common/RawVoiceTestSupport.h"
#include "harken/dsp/Biquad.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <vector>

#if defined(_WIN32)
#include <windows.h>
#endif

int RunCurveUpdateBenchmark(int argc, char* argv[]);
int RunLimiterReleaseBenchmark(int argc, char* argv[]);
int RunCompressorReleaseBenchmark(int argc, char* argv[]);

namespace {

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;
constexpr std::uint32_t kBlocks = 600;
constexpr std::uint32_t kVoices = 48;
constexpr VnBusId kSendBus = harkentest::ObjectId(7);

std::vector<float> MakeSine(std::size_t frames, double freqHz, float amp) {
    std::vector<float> s(frames);
    for (std::size_t n = 0; n < frames; ++n) {
        s[n] = amp * static_cast<float>(
                         std::sin(2.0 * 3.14159265358979323846 * freqHz *
                                  static_cast<double>(n) /
                                  static_cast<double>(kRate)));
    }
    return s;
}

const VnEffectDesc* AsBase(const void* concrete) {
    return static_cast<const VnEffectDesc*>(concrete);
}

// FNV-1a over the raw bytes of a rendered block — the identity fingerprint.
void HashBytes(std::uint64_t& h, const float* data, std::size_t count) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(data);
    const std::size_t n = count * sizeof(float);
    for (std::size_t i = 0; i < n; ++i) {
        h ^= bytes[i];
        h *= 1099511628211ull;
    }
}

struct Result {
    double wallMs = 0.0;
    std::uint64_t hash = 1469598103934665603ull; // FNV offset basis
    std::uint32_t busyPeakUs = 0;
    bool ok = false;
};

enum class Phase1Load {
    Gain,
    Limiter,
    AggregateTone0,
    AggregateTone3,
    AggregateTone4
};

struct Phase1LoadSpec {
    Phase1Load load;
    const char* name;
    bool gain;
    bool limiter;
    std::uint32_t toneSections;
};

struct Phase1LayoutSpec {
    const char* name;
    std::uint32_t layout;
    std::uint32_t channels;
};

struct Phase1Result {
    bool ok = false;
    const char* error = "unknown";
    double p50Us = 0.0;
    double p95Us = 0.0;
    double p99Us = 0.0;
    double maxUs = 0.0;
    double realtimeRatio = 0.0;
    std::uint32_t deadlineMisses = 0;
    std::uint64_t hash = 1469598103934665603ull;
    std::uint64_t arenaHighWaterBytes = 0;
    std::uint64_t arenaOverflowCount = 0;
    std::uint64_t voiceHighWater = 0;
    std::uint32_t gainPoolHighWater = 0;
    std::uint32_t limiterPoolHighWater = 0;
};

constexpr std::uint32_t kPhase1WarmupBlocks = 32;
constexpr std::uint32_t kPhase1MeasuredBlocks = 256;
constexpr std::uint32_t kPhase1ArenaBytes = 1u << 24;
constexpr VnSoundId kPhase1Sound = harkentest::ObjectId(1);
bool gProcessPinned = false;
bool gProcessPrioritySet = false;
std::uint32_t gPinnedCpuCount = 0;

constexpr std::array<Phase1LoadSpec, 5> kPhase1Loads = {{
    {Phase1Load::Gain, "gain", true, false, 0},
    {Phase1Load::Limiter, "limiter", false, true, 0},
    {Phase1Load::AggregateTone0, "aggregate-tone0", true, true, 0},
    {Phase1Load::AggregateTone3, "aggregate-tone3", true, true, 3},
    {Phase1Load::AggregateTone4, "aggregate-tone4", true, true, 4},
}};

constexpr std::array<Phase1LayoutSpec, 2> kPhase1Layouts = {{
    {"stereo", VN_CHANNEL_LAYOUT_STEREO, 2},
    {"7.1", VN_CHANNEL_LAYOUT_7_1, 8},
}};

constexpr std::array<std::uint32_t, 3> kPhase1VoiceCounts = {1, 16, 64};
constexpr std::array<std::uint32_t, 4> kPhase1WorkerCounts = {0, 1, 2, 4};

double NearestRankPercentile(const std::vector<double>& sorted, double p) {
    if (sorted.empty()) {
        return 0.0;
    }
    const std::size_t rank = static_cast<std::size_t>(
        std::ceil(p * static_cast<double>(sorted.size())));
    return sorted[std::max<std::size_t>(1, rank) - 1];
}

std::uint32_t EffectHighWater(const VnSnapshot& snapshot,
                              std::uint32_t effectType) {
    for (std::uint32_t i = 0; i < snapshot.effectTypeStatCount; ++i) {
        if (snapshot.effectTypeStats[i].effectType == effectType) {
            return snapshot.effectTypeStats[i].highWater;
        }
    }
    return 0;
}

std::vector<float> MakePhase1Signal(std::size_t frames,
                                    std::uint32_t channels) {
    std::vector<float> samples(frames * channels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            const double frequency = 97.0 + 31.0 * channel;
            samples[frame * channels + channel] =
                0.8f * static_cast<float>(
                           std::sin(2.0 * 3.14159265358979323846 * frequency *
                                    static_cast<double>(frame) /
                                    static_cast<double>(kRate)));
        }
    }
    return samples;
}

void ApplyPhase1Tone(harken::Engine& engine,
                     const std::vector<VnVoiceHandle>& handles,
                     std::uint32_t sectionCount) {
    if (sectionCount == 0) {
        return;
    }
    constexpr float q = 0.70710678118654752440f;
    const harken::BiquadCoeffs lowpass =
        harken::MakeLowpass(static_cast<float>(kRate), 8000.0f, q);
    const harken::BiquadCoeffs highpass =
        harken::MakeHighpass(static_cast<float>(kRate), 80.0f, q);
    const harken::BiquadCoeffs lowShelf =
        harken::MakeLowShelf(static_cast<float>(kRate), 200.0f, q, 3.0f);
    const harken::BiquadCoeffs highShelf =
        harken::MakeHighShelf(static_cast<float>(kRate), 6000.0f, q, -3.0f);
    const std::uint32_t lowpassTail =
        harken::EstimateBiquadTailFrames(lowpass);
    const std::uint32_t highpassTail =
        harken::EstimateBiquadTailFrames(highpass);
    const std::uint32_t lowShelfTail =
        harken::EstimateBiquadTailFrames(lowShelf);
    const std::uint32_t highShelfTail =
        harken::EstimateBiquadTailFrames(highShelf);

    for (VnVoiceHandle handle : handles) {
        engine.voices.ApplySetFilter(handle, harken::kFilterKindLowpass, true,
                                     8000.0f, q, lowpass, lowpassTail, 1);
        engine.voices.ApplySetFilter(handle, harken::kFilterKindHighpass, true,
                                     80.0f, q, highpass, highpassTail, 1);
        engine.voices.ApplySetShelf(handle, harken::kShelfKindLow, true, 200.0f,
                                    q, 3.0f, lowShelf, lowShelfTail, 1);
        if (sectionCount == 4) {
            engine.voices.ApplySetShelf(handle, harken::kShelfKindHigh, true,
                                        6000.0f, q, -3.0f, highShelf,
                                        highShelfTail, 1);
        }
    }
}

Phase1Result RunPhase1Case(const Phase1LoadSpec& load,
                           const Phase1LayoutSpec& layout,
                           std::uint32_t voiceCount,
                           std::uint32_t workerCount) {
    Phase1Result result;
    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.deviceMode = VN_DEVICE_NULL;
    config.sampleRate = kRate;
    config.blockFrames = kBlock;
    config.maxVoices = voiceCount;
    config.fxSlotsPerVoice = 2;
    config.arenaBytes = kPhase1ArenaBytes;
    config.commandQueueDepth = 512;
    config.mixChannelLayout = layout.layout;
    config.deviceChannelLayout = layout.layout;
    config.renderWorkerCount = workerCount;
    config.gainEffectPoolCapacity = voiceCount;
    config.limiterEffectPoolCapacity = voiceCount;
    if (Vn_Init(&config) != VN_OK) {
        result.error = "init";
        return result;
    }

    const std::size_t sourceFrames =
        static_cast<std::size_t>(kPhase1WarmupBlocks +
                                 kPhase1MeasuredBlocks + 4) *
        kBlock;
    const std::vector<float> signal =
        MakePhase1Signal(sourceFrames, layout.channels);
    VnPCMDesc pcm{};
    pcm.structSize = sizeof(VnPCMDesc);
    pcm.sampleRate = kRate;
    pcm.channels = layout.channels;
    pcm.layout = VN_PCM_LAYOUT_INTERLEAVED;
    pcm.policy = VN_PCM_POLICY_COPY;
    pcm.frames = sourceFrames;
    pcm.samplesInterleaved = signal.data();
    if (Vn_RegisterPCM(kPhase1Sound, &pcm) != VN_OK) {
        result.error = "register-pcm";
        Vn_Term();
        return result;
    }

    VnGainDesc gain{};
    gain.structSize = sizeof(VnGainDesc);
    gain.gainDb = 3.0f;
    VnLimiterDesc limiter{};
    limiter.structSize = sizeof(VnLimiterDesc);
    limiter.ceilingDb = -6.0f;
    limiter.releaseMs = 100.0f;

    std::vector<VnVoiceHandle> handles;
    handles.reserve(voiceCount);
    for (std::uint32_t voice = 0; voice < voiceCount; ++voice) {
        VnPlayDesc play{};
        play.structSize = sizeof(VnPlayDesc);
        play.soundId = kPhase1Sound;
        play.busId = VN_BUS_MASTER;
        play.volumeDb = -12.0f;
        play.pitchRatio = 1.0f;
        play.loopCount = 1;
        VnVoiceHandle handle = 0;
        if (harkentest::PlayRaw(&play, &handle) != VN_OK) {
            result.error = "play";
            Vn_Term();
            return result;
        }
        handles.push_back(handle);
        std::uint32_t slot = 0;
        if (load.gain &&
            Vn_SetVoiceEffect(handle, slot++, VN_EFFECT_GAIN,
                              AsBase(&gain)) != VN_OK) {
            result.error = "gain-install";
            Vn_Term();
            return result;
        }
        if (load.limiter &&
            Vn_SetVoiceEffect(handle, slot, VN_EFFECT_LIMITER,
                              AsBase(&limiter)) != VN_OK) {
            result.error = "limiter-install";
            Vn_Term();
            return result;
        }
    }
    Vn_Update();

    harken::Engine* engine = harken::detail::ActiveEngine();
    if (engine == nullptr) {
        result.error = "active-engine";
        Vn_Term();
        return result;
    }
    engine->nullBackend.Pump(1);
    ApplyPhase1Tone(*engine, handles, load.toneSections);
    for (std::uint32_t block = 0; block < kPhase1WarmupBlocks; ++block) {
        engine->nullBackend.Pump(1);
    }

    std::vector<double> renderUs;
    renderUs.reserve(kPhase1MeasuredBlocks);
    constexpr double deadlineUs =
        1.0e6 * static_cast<double>(kBlock) / static_cast<double>(kRate);
    double totalUs = 0.0;
    for (std::uint32_t block = 0; block < kPhase1MeasuredBlocks; ++block) {
        const auto begin = std::chrono::steady_clock::now();
        engine->nullBackend.Pump(1);
        const auto end = std::chrono::steady_clock::now();
        const double elapsedUs =
            std::chrono::duration<double, std::micro>(end - begin).count();
        renderUs.push_back(elapsedUs);
        totalUs += elapsedUs;
        if (elapsedUs > deadlineUs) {
            ++result.deadlineMisses;
        }
        HashBytes(result.hash, engine->nullBackend.LastBlock(),
                  static_cast<std::size_t>(kBlock) * layout.channels);
    }

    std::sort(renderUs.begin(), renderUs.end());
    result.p50Us = NearestRankPercentile(renderUs, 0.50);
    result.p95Us = NearestRankPercentile(renderUs, 0.95);
    result.p99Us = NearestRankPercentile(renderUs, 0.99);
    result.maxUs = renderUs.back();
    const double audioSeconds =
        static_cast<double>(kPhase1MeasuredBlocks) * kBlock /
        static_cast<double>(kRate);
    result.realtimeRatio =
        totalUs > 0.0 ? audioSeconds / (totalUs / 1.0e6) : 0.0;

    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    if (Vn_CaptureSnapshot(&snapshot) != VN_OK) {
        result.error = "snapshot";
        Vn_Term();
        return result;
    }
    result.arenaHighWaterBytes = snapshot.arenaHighWaterBytes;
    result.arenaOverflowCount = snapshot.arenaOverflowCount;
    result.voiceHighWater = snapshot.voiceHighWater;
    result.gainPoolHighWater =
        EffectHighWater(snapshot, VN_EFFECT_GAIN);
    result.limiterPoolHighWater =
        EffectHighWater(snapshot, VN_EFFECT_LIMITER);
    result.ok = snapshot.arenaOverflowCount == 0 &&
                snapshot.activeVoiceCount == voiceCount &&
                snapshot.voiceHighWater == voiceCount &&
                snapshot.mixChannelCount == layout.channels &&
                snapshot.deviceChannelCount == layout.channels &&
                snapshot.renderWorkerCount == workerCount &&
                result.gainPoolHighWater ==
                    (load.gain ? voiceCount : 0) &&
                result.limiterPoolHighWater ==
                    (load.limiter ? voiceCount : 0);
    if (!result.ok) {
        result.error = "capacity-observability";
    }
    Vn_Term();
    return result;
}

void PrintPhase1Row(const Phase1LoadSpec& load,
                    const Phase1LayoutSpec& layout,
                    std::uint32_t voiceCount,
                    std::uint32_t workerCount,
                    const Phase1Result& result,
                    const char* identity) {
    std::printf(
        "{\"benchmark\":\"phase1\",\"status\":\"measured\","
        "\"load\":\"%s\",\"tone_sections\":%u,\"voices\":%u,"
        "\"layout\":\"%s\",\"channels\":%u,\"workers\":%u,"
        "\"sample_rate\":%u,\"block_frames\":%u,\"warmup_blocks\":%u,"
        "\"measured_blocks\":%u,\"deadline_us\":%.3f,"
        "\"p50_us\":%.3f,\"p95_us\":%.3f,\"p99_us\":%.3f,"
        "\"max_us\":%.3f,\"realtime_x\":%.3f,\"deadline_misses\":%u,"
        "\"arena_high_water_bytes\":%llu,\"arena_overflow_count\":%llu,"
        "\"voice_high_water\":%llu,\"gain_pool_high_water\":%u,"
        "\"limiter_pool_high_water\":%u,\"hash\":\"%016llx\","
        "\"identity\":\"%s\"}\n",
        load.name, load.toneSections, voiceCount, layout.name, layout.channels,
        workerCount, kRate, kBlock, kPhase1WarmupBlocks,
        kPhase1MeasuredBlocks,
        1.0e6 * static_cast<double>(kBlock) / static_cast<double>(kRate),
        result.p50Us, result.p95Us, result.p99Us, result.maxUs,
        result.realtimeRatio, result.deadlineMisses,
        static_cast<unsigned long long>(result.arenaHighWaterBytes),
        static_cast<unsigned long long>(result.arenaOverflowCount),
        static_cast<unsigned long long>(result.voiceHighWater),
        result.gainPoolHighWater, result.limiterPoolHighWater,
        static_cast<unsigned long long>(result.hash), identity);
}

int RunPhase1Benchmark(int argc) {
    if (argc != 0) {
        std::fprintf(stderr,
                     "Usage: harken_benchmarks --filter phase1\n");
        return 2;
    }
#ifndef NDEBUG
    std::printf(
        "{\"benchmark\":\"phase1\",\"status\":\"skipped\","
        "\"reason\":\"debug-build\"}\n");
    return 0;
#else
    std::printf(
        "{\"benchmark\":\"phase1\",\"status\":\"matrix\","
        "\"sample_rate\":%u,\"block_frames\":%u,"
        "\"warmup_blocks\":%u,\"measured_blocks\":%u,"
        "\"voices\":[1,16,64],\"layouts\":[\"stereo\",\"7.1\"],"
        "\"workers\":[0,1,2,4],"
        "\"loads\":[\"gain\",\"limiter\",\"aggregate-tone0\","
        "\"aggregate-tone3\",\"aggregate-tone4\"],"
        "\"percentile_method\":\"nearest-rank\","
        "\"performance_threshold\":\"none\"}\n",
        kRate, kBlock, kPhase1WarmupBlocks, kPhase1MeasuredBlocks);

    bool allValid = true;
    bool allIdentical = true;
    constexpr std::size_t hashCount =
        kPhase1Loads.size() * kPhase1VoiceCounts.size() *
        kPhase1Layouts.size() * kPhase1WorkerCounts.size();
    std::array<std::uint64_t, hashCount> hashes{};
    const auto hashIndex = [](std::size_t loadIndex,
                              std::size_t voiceIndex,
                              std::size_t layoutIndex,
                              std::size_t workerIndex) {
        return (((loadIndex * kPhase1VoiceCounts.size() + voiceIndex) *
                     kPhase1Layouts.size() +
                 layoutIndex) *
                    kPhase1WorkerCounts.size() +
                workerIndex);
    };
    for (std::size_t loadIndex = 0; loadIndex < kPhase1Loads.size();
         ++loadIndex) {
        const Phase1LoadSpec& load = kPhase1Loads[loadIndex];
        for (std::size_t voiceIndex = 0;
             voiceIndex < kPhase1VoiceCounts.size(); ++voiceIndex) {
            const std::uint32_t voiceCount =
                kPhase1VoiceCounts[voiceIndex];
            for (std::size_t layoutIndex = 0;
                 layoutIndex < kPhase1Layouts.size(); ++layoutIndex) {
                const Phase1LayoutSpec& layout =
                    kPhase1Layouts[layoutIndex];
                std::uint64_t serialHash = 0;
                for (std::size_t workerIndex = 0;
                     workerIndex < kPhase1WorkerCounts.size();
                     ++workerIndex) {
                    const std::uint32_t workerCount =
                        kPhase1WorkerCounts[workerIndex];
                    const Phase1Result result =
                        RunPhase1Case(load, layout, voiceCount, workerCount);
                    if (!result.ok) {
                        std::fprintf(
                            stderr,
                            "phase1 setup failed: load=%s voices=%u "
                            "layout=%s workers=%u reason=%s\n",
                            load.name, voiceCount, layout.name, workerCount,
                            result.error);
                        allValid = false;
                        continue;
                    }
                    hashes[hashIndex(loadIndex, voiceIndex, layoutIndex,
                                     workerIndex)] = result.hash;
                    if (workerCount == 0) {
                        serialHash = result.hash;
                    }
                    const bool identical =
                        workerCount == 0 || result.hash == serialHash;
                    allIdentical = allIdentical && identical;
                    PrintPhase1Row(load, layout, voiceCount, workerCount,
                                   result,
                                   workerCount == 0
                                       ? "base"
                                       : (identical ? "identical" : "drift"));
                }
            }
        }
    }
    bool toneWorkloadsDistinct = true;
    constexpr std::size_t tone0 =
        static_cast<std::size_t>(Phase1Load::AggregateTone0);
    constexpr std::size_t tone3 =
        static_cast<std::size_t>(Phase1Load::AggregateTone3);
    constexpr std::size_t tone4 =
        static_cast<std::size_t>(Phase1Load::AggregateTone4);
    for (std::size_t voiceIndex = 0;
         voiceIndex < kPhase1VoiceCounts.size(); ++voiceIndex) {
        for (std::size_t layoutIndex = 0;
             layoutIndex < kPhase1Layouts.size(); ++layoutIndex) {
            for (std::size_t workerIndex = 0;
                 workerIndex < kPhase1WorkerCounts.size(); ++workerIndex) {
                const std::uint64_t hash0 =
                    hashes[hashIndex(tone0, voiceIndex, layoutIndex,
                                     workerIndex)];
                const std::uint64_t hash3 =
                    hashes[hashIndex(tone3, voiceIndex, layoutIndex,
                                     workerIndex)];
                const std::uint64_t hash4 =
                    hashes[hashIndex(tone4, voiceIndex, layoutIndex,
                                     workerIndex)];
                toneWorkloadsDistinct =
                    toneWorkloadsDistinct && hash0 != 0 && hash3 != 0 &&
                    hash4 != 0 && hash0 != hash3 && hash0 != hash4 &&
                    hash3 != hash4;
            }
        }
    }
    std::printf(
        "{\"benchmark\":\"phase1\",\"status\":\"complete\","
        "\"measured_rows\":120,\"worker_identity\":%s,"
        "\"tone_workloads_distinct\":%s}\n",
        allIdentical ? "true" : "false",
        toneWorkloadsDistinct ? "true" : "false");
    if (!allValid || !allIdentical || !toneWorkloadsDistinct) {
        std::fprintf(stderr,
                     "FAIL: invalid capacity evidence, worker drift, or "
                     "indistinguishable Tone workload\n");
        return 1;
    }
    return 0;
#endif
}

enum class CompressorSceneKind {
    VoiceSelf,
    BusRouted,
    Mixed,
    Inactive
};

struct CompressorSceneSpec {
    CompressorSceneKind kind;
    const char* name;
    std::uint32_t voiceCount;
    std::uint32_t voiceCompressorCount;
    std::uint32_t busCompressorCount;
    std::uint32_t routeCount;
    double p99BudgetUs;
};

struct CompressorMatrixOptions {
    const char* caseName = "all";
    std::uint32_t workerCount =
        (std::numeric_limits<std::uint32_t>::max)();
    bool workersSpecified = false;
    bool paired = false;
    std::uint32_t warmupBlocks = 2000;
    std::uint32_t measuredBlocks = 10000;
    std::uint32_t runs = 5;
};

struct CompressorMeasurementHygiene {
    std::uint32_t vintage = 1;
    const char* requested = "unchanged";
    const char* observed = "unobserved";
    bool prioritySet = false;
    int effectiveBasePriority = -1;
    bool gateApplicable = false;
    bool gatePassed = false;
};

CompressorMeasurementHygiene gCompressorMeasurementHygiene;

struct CompressorSceneResult {
    bool ok = false;
    const char* error = "unknown";
    double p50Us = 0.0;
    double p99Us = 0.0;
    double maxUs = 0.0;
    std::uint64_t deadlineMisses = 0;
    std::uint64_t hash = 1469598103934665603ull;
    std::uint64_t nonfiniteOutputSampleCount = 0;
    std::uint64_t initDynamicRouteBytes = 0;
    std::uint64_t initCompressorPoolBytes = 0;
    std::uint64_t arenaHighWaterBytes = 0;
    std::uint64_t arenaOverflowCount = 0;
    std::uint32_t compressorPoolHighWater = 0;
    std::uint32_t dynamicRouteCurrent = 0;
    std::uint32_t dynamicRouteHighWater = 0;
    std::uint32_t dynamicBusTapHighWater = 0;
    std::uint32_t dynamicVoiceTapHighWater = 0;
    std::uint64_t dynamicRouteRefusalCount = 0;
    std::uint64_t dynamicPlanRetireOverflowCount = 0;
    std::uint32_t setupPumpBlocks = 0;
    VnConfig initConfig{};
    std::vector<double> blockDurationsUs;
};

struct DurationStats {
    double medianUs = 0.0;
    double p99Us = 0.0;
    double maxUs = 0.0;
};

constexpr std::array<CompressorSceneSpec, 4> kCompressorScenes = {{
    {CompressorSceneKind::VoiceSelf, "voice-self-64", 64, 64, 0, 0,
     1066.666},
    {CompressorSceneKind::BusRouted, "bus-routed-16x4", 32, 0, 16, 64,
     1333.333},
    {CompressorSceneKind::Mixed, "mixed-worst", 64, 48, 16, 64,
     2666.666},
    {CompressorSceneKind::Inactive, "inactive-route-platform", 64, 0, 0, 0,
     0.0},
}};

constexpr std::array<std::uint32_t, 4> kCompressorWorkerCounts = {0, 1, 2,
                                                                  4};

bool ParsePositiveCount(const char* text, std::uint32_t* out) {
    if (text == nullptr || text[0] == '\0' || out == nullptr) {
        return false;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || parsed == 0 ||
        parsed > (std::numeric_limits<std::uint32_t>::max)()) {
        return false;
    }
    *out = static_cast<std::uint32_t>(parsed);
    return true;
}

void PrintCompressorMatrixUsage() {
    std::printf(
        "Usage: harken_benchmarks --filter compressor-matrix "
        "[--case all|voice-self|bus-routed|mixed|inactive] "
        "[--workers all|0|1|2|4] [--paired] "
        "[--warmup N] [--blocks N] [--runs N]\n");
}

bool ParseCompressorMatrixOptions(int argc, char* argv[],
                                  CompressorMatrixOptions* out) {
    for (int i = 0; i < argc; ++i) {
        if (std::strcmp(argv[i], "--case") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->caseName = argv[i];
        } else if (std::strcmp(argv[i], "--workers") == 0) {
            if (++i >= argc) {
                return false;
            }
            out->workersSpecified = true;
            if (std::strcmp(argv[i], "all") == 0) {
                out->workerCount =
                    (std::numeric_limits<std::uint32_t>::max)();
            } else if (std::strcmp(argv[i], "0") == 0) {
                out->workerCount = 0;
            } else {
                if (!ParsePositiveCount(argv[i], &out->workerCount)) {
                    return false;
                }
            }
            if (out->workerCount !=
                    (std::numeric_limits<std::uint32_t>::max)() &&
                std::find(kCompressorWorkerCounts.begin(),
                          kCompressorWorkerCounts.end(), out->workerCount) ==
                    kCompressorWorkerCounts.end()) {
                return false;
            }
        } else if (std::strcmp(argv[i], "--warmup") == 0) {
            if (++i >= argc ||
                !ParsePositiveCount(argv[i], &out->warmupBlocks)) {
                return false;
            }
        } else if (std::strcmp(argv[i], "--blocks") == 0) {
            if (++i >= argc ||
                !ParsePositiveCount(argv[i], &out->measuredBlocks)) {
                return false;
            }
        } else if (std::strcmp(argv[i], "--runs") == 0) {
            if (++i >= argc || !ParsePositiveCount(argv[i], &out->runs)) {
                return false;
            }
        } else if (std::strcmp(argv[i], "--paired") == 0) {
            out->paired = true;
        } else if (std::strcmp(argv[i], "--help") == 0) {
            PrintCompressorMatrixUsage();
            std::exit(0);
        } else {
            return false;
        }
    }
    return std::strcmp(out->caseName, "all") == 0 ||
           std::strcmp(out->caseName, "voice-self") == 0 ||
           std::strcmp(out->caseName, "bus-routed") == 0 ||
           std::strcmp(out->caseName, "mixed") == 0 ||
           std::strcmp(out->caseName, "inactive") == 0;
}

DurationStats ComputeDurationStats(const std::vector<double>& durations) {
    DurationStats stats;
    if (durations.empty()) {
        return stats;
    }
    std::vector<double> sorted = durations;
    std::sort(sorted.begin(), sorted.end());
    stats.medianUs = NearestRankPercentile(sorted, 0.50);
    stats.p99Us = NearestRankPercentile(sorted, 0.99);
    stats.maxUs = sorted.back();
    return stats;
}

bool CompressorCaseSelected(const CompressorSceneSpec& scene,
                            const char* caseName) {
    if (std::strcmp(caseName, "all") == 0) {
        return true;
    }
    switch (scene.kind) {
    case CompressorSceneKind::VoiceSelf:
        return std::strcmp(caseName, "voice-self") == 0;
    case CompressorSceneKind::BusRouted:
        return std::strcmp(caseName, "bus-routed") == 0;
    case CompressorSceneKind::Mixed:
        return std::strcmp(caseName, "mixed") == 0;
    case CompressorSceneKind::Inactive:
        return std::strcmp(caseName, "inactive") == 0;
    }
    return false;
}

bool CompressorWorkerSelected(const CompressorMatrixOptions& options,
                              const CompressorSceneSpec& scene,
                              std::uint32_t workerCount) {
    if (!options.workersSpecified) {
        return scene.kind == CompressorSceneKind::Mixed || workerCount == 0;
    }
    return options.workerCount ==
               (std::numeric_limits<std::uint32_t>::max)() ||
           workerCount == options.workerCount;
}

bool IsCanonicalCompressorGateRun(const CompressorMatrixOptions& options) {
    if (options.warmupBlocks != 2000 ||
        options.measuredBlocks != 10000) {
        return false;
    }
    if (options.paired) {
        const bool pairedCase =
            std::strcmp(options.caseName, "all") == 0 ||
            std::strcmp(options.caseName, "voice-self") == 0 ||
            std::strcmp(options.caseName, "bus-routed") == 0;
        const bool serialWorker =
            !options.workersSpecified || options.workerCount == 0;
        return pairedCase && serialWorker && options.runs == 5;
    }
    return std::strcmp(options.caseName, "mixed") == 0 &&
           options.runs == 1;
}

bool IsCanonicalSerialCompressorRun(
    const CompressorMatrixOptions& options) {
    return IsCanonicalCompressorGateRun(options) &&
           options.workersSpecified && options.workerCount == 0;
}

#if defined(_WIN32)
const char* ThreadPriorityName(int priority) {
    switch (priority) {
    case THREAD_PRIORITY_HIGHEST:
        return "highest";
    case THREAD_PRIORITY_ABOVE_NORMAL:
        return "above-normal";
    case THREAD_PRIORITY_NORMAL:
        return "normal";
    case THREAD_PRIORITY_BELOW_NORMAL:
        return "below-normal";
    case THREAD_PRIORITY_LOWEST:
        return "lowest";
    case THREAD_PRIORITY_IDLE:
        return "idle";
    case THREAD_PRIORITY_ERROR_RETURN:
        return "error";
    default:
        return "unknown";
    }
}

int EffectiveHighProcessBasePriority(int threadPriority) {
    if (!gProcessPrioritySet) {
        return -1;
    }
    switch (threadPriority) {
    case THREAD_PRIORITY_HIGHEST:
        return 15;
    case THREAD_PRIORITY_ABOVE_NORMAL:
        return 14;
    case THREAD_PRIORITY_NORMAL:
        return 13;
    case THREAD_PRIORITY_BELOW_NORMAL:
        return 12;
    case THREAD_PRIORITY_LOWEST:
        return 11;
    case THREAD_PRIORITY_IDLE:
        return 1;
    default:
        return -1;
    }
}
#endif

bool ConfigureCompressorMeasurementHygiene(
    const CompressorMatrixOptions& options) {
    gCompressorMeasurementHygiene = {};
    gCompressorMeasurementHygiene.gateApplicable =
        IsCanonicalSerialCompressorRun(options);
    gCompressorMeasurementHygiene.requested =
        gCompressorMeasurementHygiene.gateApplicable ? "highest"
                                                     : "unchanged";
#if defined(_WIN32)
    if (gCompressorMeasurementHygiene.gateApplicable) {
        gCompressorMeasurementHygiene.prioritySet =
            SetThreadPriority(GetCurrentThread(),
                              THREAD_PRIORITY_HIGHEST) != 0;
    }
    const int observed = GetThreadPriority(GetCurrentThread());
    gCompressorMeasurementHygiene.observed =
        ThreadPriorityName(observed);
    gCompressorMeasurementHygiene.effectiveBasePriority =
        EffectiveHighProcessBasePriority(observed);
    if (gCompressorMeasurementHygiene.gateApplicable) {
        gCompressorMeasurementHygiene.gatePassed =
            gCompressorMeasurementHygiene.prioritySet &&
            observed == THREAD_PRIORITY_HIGHEST &&
            gCompressorMeasurementHygiene.effectiveBasePriority == 15;
    }
#else
    gCompressorMeasurementHygiene.observed = "unsupported";
#endif
    return !gCompressorMeasurementHygiene.gateApplicable ||
           gCompressorMeasurementHygiene.gatePassed;
}

std::array<char, 640> CompressorMeasurementHygieneJson() {
    std::array<char, 640> json{};
    std::snprintf(
        json.data(), json.size(),
        "\"measurement_hygiene_vintage\":%u,"
        "\"callback_thread_priority_requested\":\"%s\","
        "\"callback_thread_priority_observed\":\"%s\","
        "\"callback_thread_priority_set\":%s,"
        "\"callback_thread_effective_base_priority\":%d,"
        "\"time_critical_requested\":false,"
        "\"callback_thread_priority_gate_applicable\":%s,"
        "\"callback_thread_priority_gate_passed\":%s,"
        "\"worker_thread_priority_policy_unchanged\":true,",
        gCompressorMeasurementHygiene.vintage,
        gCompressorMeasurementHygiene.requested,
        gCompressorMeasurementHygiene.observed,
        gCompressorMeasurementHygiene.prioritySet ? "true" : "false",
        gCompressorMeasurementHygiene.effectiveBasePriority,
        gCompressorMeasurementHygiene.gateApplicable ? "true" : "false",
        gCompressorMeasurementHygiene.gatePassed ? "true" : "false");
    return json;
}

std::array<char, 512> CompressorPerformanceContractJson(
    bool scopedCostGateApplicable, bool scopedCostGatePassed,
    std::uint64_t deadlineMisses) {
    std::array<char, 512> json{};
    std::snprintf(
        json.data(), json.size(),
        "\"performance_contract_vintage\":2,"
        "\"performance_gate_scope\":\"finite-cost-regression-only\","
        "\"scoped_cost_gate_applicable\":%s,"
        "\"scoped_cost_gate_passed\":%s,"
        "\"deadline_misses_diagnostic_only\":true,"
        "\"deadline_miss_hard_fail_applicable\":false,"
        "\"deadline_reliability_claimed\":false,"
        "\"deadline_observation_status\":\"%s\","
        "\"scoped_gate_meaning\":"
        "\"finite-cost-regression-only-not-hard-realtime-or-glitch-free\",",
        scopedCostGateApplicable ? "true" : "false",
        scopedCostGatePassed ? "true" : "false",
        deadlineMisses == 0 ? "clear" : "unclassified");
    return json;
}

std::uint32_t CompressorScenarioWorkerCoverageBit(
    const CompressorSceneSpec& scene, std::uint32_t workerCount) {
    std::uint32_t sceneIndex = 0;
    switch (scene.kind) {
        case CompressorSceneKind::VoiceSelf:
            sceneIndex = 0;
            break;
        case CompressorSceneKind::BusRouted:
            sceneIndex = 1;
            break;
        case CompressorSceneKind::Mixed:
            sceneIndex = 2;
            break;
        case CompressorSceneKind::Inactive:
            sceneIndex = 3;
            break;
    }
    for (std::size_t i = 0; i < kCompressorWorkerCounts.size(); ++i) {
        if (kCompressorWorkerCounts[i] == workerCount) {
            const std::uint32_t bitIndex =
                sceneIndex * static_cast<std::uint32_t>(
                                 kCompressorWorkerCounts.size()) +
                static_cast<std::uint32_t>(i);
            return 1u << bitIndex;
        }
    }
    return 0;
}

constexpr std::uint32_t kRequiredPairedCoverageMask = 0x0011u;
constexpr std::uint32_t kRequiredMixedCoverageMask = 0x0f00u;
constexpr std::uint32_t kRequiredInactiveCoverageMask = 1u << 12;

std::uint64_t DynamicRouteInitBytes(const VnConfig& config) {
    const std::uint64_t rows =
        static_cast<std::uint64_t>(config.dynamicRouteCapacity) *
        sizeof(harken::DynamicRoutePlanRow);
    const std::uint64_t pins =
        static_cast<std::uint64_t>(config.dynamicBusTapCapacity +
                                   config.dynamicVoiceTapCapacity) *
        sizeof(std::uint32_t);
    const std::uint64_t plans =
        static_cast<std::uint64_t>(config.dynamicRoutePlanPoolCapacity + 1) *
        (rows + pins);
    const std::uint64_t sampleBytes =
        static_cast<std::uint64_t>(harken::kMaxChannels) * kBlock *
        sizeof(float);
    return plans +
           static_cast<std::uint64_t>(config.dynamicBusTapCapacity) *
               sampleBytes +
           static_cast<std::uint64_t>(config.dynamicVoiceTapCapacity) * 2 *
               sampleBytes +
           static_cast<std::uint64_t>(kBlock) * sizeof(float);
}

std::vector<float> MakeCompressorMatrixSignal(std::size_t frames) {
    std::vector<float> samples(frames * harken::kMaxChannels);
    for (std::size_t frame = 0; frame < frames; ++frame) {
        for (std::uint32_t channel = 0; channel < harken::kMaxChannels;
             ++channel) {
            const double frequency = 83.0 + 29.0 * channel;
            samples[frame * harken::kMaxChannels + channel] =
                0.55f * static_cast<float>(
                            std::sin(2.0 * 3.14159265358979323846 *
                                     frequency * static_cast<double>(frame) /
                                     static_cast<double>(kRate)));
        }
    }
    return samples;
}

VnCompressorDesc MatrixCompressorDesc() {
    VnCompressorDesc compressor{};
    compressor.structSize = sizeof compressor;
    compressor.thresholdDb = -24.0f;
    compressor.ratio = 6.0f;
    compressor.attackMs = 2.0f;
    compressor.releaseMs = 120.0f;
    compressor.kneeDb = 6.0f;
    compressor.makeupDb = 0.0f;
    return compressor;
}

VnEffectDynamicRouteDesc MatrixBusRoute(VnBusId sourceBus) {
    VnEffectDynamicRouteDesc route{};
    route.structSize = sizeof route;
    route.signalForm = VN_DYNAMIC_SIGNAL_RAW_AUDIO;
    route.sourceKind = VN_DYNAMIC_SOURCE_BUS;
    route.tap = VN_DYNAMIC_TAP_BUS_POST_EFFECTS;
    route.sourceId = sourceBus;
    route.levelDb = 0.0f;
    route.expectedEffectType = VN_EFFECT_COMPRESSOR;
    return route;
}

VnEffectDynamicRouteDesc MatrixVoiceRoute(VnVoiceHandle sourceVoice) {
    VnEffectDynamicRouteDesc route{};
    route.structSize = sizeof route;
    route.signalForm = VN_DYNAMIC_SIGNAL_RAW_AUDIO;
    route.sourceKind = VN_DYNAMIC_SOURCE_VOICE;
    route.tap = VN_DYNAMIC_TAP_VOICE_POST_GAIN_PRE_SPATIAL;
    route.sourceId = sourceVoice;
    route.levelDb = 0.0f;
    route.expectedEffectType = VN_EFFECT_COMPRESSOR;
    return route;
}

CompressorSceneResult RunCompressorScene(const CompressorSceneSpec& scene,
                                         std::uint32_t workerCount,
                                         std::uint32_t warmupBlocks,
                                         std::uint32_t measuredBlocks,
                                         bool featureEnabled = true) {
    CompressorSceneResult result;
    VnConfig config{};
    config.structSize = sizeof config;
    config.deviceMode = VN_DEVICE_NULL;
    config.sampleRate = kRate;
    config.blockFrames = kBlock;
    config.maxVoices = scene.voiceCount;
    config.maxBuses = 33;
    config.fxSlotsPerVoice = 1;
    config.fxSlotsPerBus = 1;
    config.arenaBytes = 1u << 24;
    config.commandQueueDepth = 1024;
    config.mixChannelLayout = VN_CHANNEL_LAYOUT_7_1;
    config.deviceChannelLayout = VN_CHANNEL_LAYOUT_7_1;
    config.renderWorkerCount = workerCount;
    config.compressorEffectPoolCapacity =
        (std::max)(16u, scene.voiceCompressorCount +
                            scene.busCompressorCount);
    config.dynamicRouteCapacity = 64;
    config.dynamicRoutePlanPoolCapacity = 4;
    config.dynamicBusTapCapacity = 8;
    config.dynamicVoiceTapCapacity = 8;
    result.initConfig = config;
    result.initDynamicRouteBytes = DynamicRouteInitBytes(config);
    result.initCompressorPoolBytes =
        static_cast<std::uint64_t>(config.compressorEffectPoolCapacity) *
        ((sizeof(harken::CompressorState) + sizeof(std::max_align_t) - 1) /
         sizeof(std::max_align_t)) *
        sizeof(std::max_align_t);
    if (Vn_Init(&config) != VN_OK) {
        result.error = "init";
        return result;
    }
    const auto finishWithError = [&](const char* error) {
        result.error = error;
        Vn_Term();
        return result;
    };
    harken::Engine* engine = harken::detail::ActiveEngine();
    if (engine == nullptr) {
        return finishWithError("active-engine");
    }

    constexpr std::uint32_t kBusSourceCount = 8;
    constexpr std::uint32_t kBusTargetCount = 16;
    constexpr VnBusId kFirstSourceBus = harkentest::ObjectId(100);
    constexpr VnBusId kFirstTargetBus = harkentest::ObjectId(200);
    const bool routedScene =
        scene.kind == CompressorSceneKind::BusRouted ||
        scene.kind == CompressorSceneKind::Mixed;
    const bool featureRouted = routedScene && featureEnabled;
    if (routedScene) {
        for (std::uint32_t i = 0; i < kBusSourceCount; ++i) {
            if (Vn_CreateBus(kFirstSourceBus + i, VN_BUS_MASTER) != VN_OK) {
                return finishWithError("create-source-bus");
            }
            engine->nullBackend.Pump(1);
            ++result.setupPumpBlocks;
            if (Vn_Update() != VN_OK) {
                return finishWithError("create-source-bus-update");
            }
        }
        for (std::uint32_t i = 0; i < kBusTargetCount; ++i) {
            if (Vn_CreateBus(kFirstTargetBus + i, VN_BUS_MASTER) != VN_OK) {
                return finishWithError("create-target-bus");
            }
            engine->nullBackend.Pump(1);
            ++result.setupPumpBlocks;
            if (Vn_Update() != VN_OK) {
                return finishWithError("create-target-bus-update");
            }
        }
    }

    const std::vector<float> signal =
        MakeCompressorMatrixSignal(static_cast<std::size_t>(kBlock) * 8);
    VnPCMDesc pcm{};
    pcm.structSize = sizeof pcm;
    pcm.sampleRate = kRate;
    pcm.channels = harken::kMaxChannels;
    pcm.layout = VN_PCM_LAYOUT_INTERLEAVED;
    pcm.policy = VN_PCM_POLICY_COPY;
    pcm.frames = signal.size() / harken::kMaxChannels;
    pcm.samplesInterleaved = signal.data();
    constexpr VnSoundId kSound = harkentest::ObjectId(700);
    if (Vn_RegisterPCM(kSound, &pcm) != VN_OK) {
        return finishWithError("register-pcm");
    }

    std::vector<VnVoiceHandle> handles;
    handles.reserve(scene.voiceCount);
    const VnCompressorDesc compressor = MatrixCompressorDesc();
    for (std::uint32_t voice = 0; voice < scene.voiceCount; ++voice) {
        VnPlayDesc play{};
        play.structSize = sizeof play;
        play.soundId = kSound;
        play.busId = VN_BUS_MASTER;
        if (scene.kind == CompressorSceneKind::BusRouted) {
            if (voice < 8) {
                play.busId = kFirstSourceBus + voice;
            } else if (voice >= 16) {
                play.busId = kFirstTargetBus + (voice - 16);
            }
        } else if (scene.kind == CompressorSceneKind::Mixed) {
            play.busId =
                voice < 8 ? kFirstSourceBus + voice
                          : kFirstTargetBus + ((voice - 8) % kBusTargetCount);
        }
        play.volumeDb = -18.0f;
        play.pitchRatio = 1.0f;
        play.loopCount = 0;
        VnVoiceHandle handle = 0;
        if (harkentest::PlayRaw(&play, &handle) != VN_OK) {
            return finishWithError("play");
        }
        handles.push_back(handle);
        if (featureEnabled && voice < scene.voiceCompressorCount &&
            Vn_SetVoiceEffect(handle, 0, VN_EFFECT_COMPRESSOR,
                              AsBase(&compressor)) != VN_OK) {
            return finishWithError("voice-compressor");
        }
    }

    if (featureRouted) {
        for (std::uint32_t bus = 0; bus < kBusTargetCount; ++bus) {
            if (Vn_SetBusEffect(kFirstTargetBus + bus, 0,
                                VN_EFFECT_COMPRESSOR,
                                AsBase(&compressor)) != VN_OK) {
                return finishWithError("bus-compressor");
            }
        }
    }
    if (Vn_Update() != VN_OK) {
        return finishWithError("initial-update");
    }
    engine->nullBackend.Pump(1);
    ++result.setupPumpBlocks;

    if (featureRouted) {
        std::array<VnVoiceHandle, 8> voiceSources{};
        for (std::uint32_t i = 0; i < voiceSources.size(); ++i) {
            voiceSources[i] = handles[8 + i];
        }
        for (std::uint32_t target = 0; target < kBusTargetCount; ++target) {
            const std::array<VnEffectDynamicRouteDesc, 4> routes = {{
                MatrixBusRoute(kFirstSourceBus + (target % 8)),
                MatrixBusRoute(kFirstSourceBus + ((target + 3) % 8)),
                MatrixVoiceRoute(voiceSources[target % 8]),
                MatrixVoiceRoute(voiceSources[(target + 5) % 8]),
            }};
            for (std::uint32_t routeSlot = 0; routeSlot < routes.size();
                 ++routeSlot) {
                if (Vn_SetBusEffectDynamicRoute(
                        kFirstTargetBus + target, 0, routeSlot,
                        &routes[routeSlot]) != VN_OK) {
                    return finishWithError("dynamic-route");
                }
                engine->nullBackend.Pump(1);
                ++result.setupPumpBlocks;
                if (Vn_Update() != VN_OK) {
                    return finishWithError("route-update");
                }
            }
        }
    } else if (routedScene) {
        for (std::uint32_t pad = 0; pad < scene.routeCount; ++pad) {
            engine->nullBackend.Pump(1);
            ++result.setupPumpBlocks;
            if (Vn_Update() != VN_OK) {
                return finishWithError("baseline-phase-pad-update");
            }
        }
    }

    for (std::uint32_t block = 0; block < warmupBlocks; ++block) {
        engine->nullBackend.Pump(1);
    }
    result.blockDurationsUs.reserve(measuredBlocks);
    constexpr double kDeadlineUs =
        1.0e6 * static_cast<double>(kBlock) / static_cast<double>(kRate);
    for (std::uint32_t block = 0; block < measuredBlocks; ++block) {
        const auto begin = std::chrono::steady_clock::now();
        engine->nullBackend.Pump(1);
        const auto end = std::chrono::steady_clock::now();
        const double elapsedUs =
            std::chrono::duration<double, std::micro>(end - begin).count();
        result.blockDurationsUs.push_back(elapsedUs);
        if (elapsedUs >= kDeadlineUs) {
            ++result.deadlineMisses;
        }
        const float* output = engine->nullBackend.LastBlock();
        const std::size_t outputSampleCount =
            static_cast<std::size_t>(kBlock) * harken::kMaxChannels;
        HashBytes(result.hash, output, outputSampleCount);
        result.nonfiniteOutputSampleCount +=
            static_cast<std::uint64_t>(std::count_if(
                output, output + outputSampleCount,
                [](float sample) { return !std::isfinite(sample); }));
    }
    const DurationStats durationStats =
        ComputeDurationStats(result.blockDurationsUs);
    result.p50Us = durationStats.medianUs;
    result.p99Us = durationStats.p99Us;
    result.maxUs = durationStats.maxUs;

    VnSnapshot snapshot{};
    snapshot.structSize = sizeof snapshot;
    if (Vn_CaptureSnapshot(&snapshot) != VN_OK) {
        return finishWithError("snapshot");
    }
    result.arenaHighWaterBytes = snapshot.arenaHighWaterBytes;
    result.arenaOverflowCount = snapshot.arenaOverflowCount;
    result.compressorPoolHighWater =
        EffectHighWater(snapshot, VN_EFFECT_COMPRESSOR);
    result.dynamicRouteCurrent = snapshot.dynamicRouteCurrent;
    result.dynamicRouteHighWater = snapshot.dynamicRouteHighWater;
    result.dynamicBusTapHighWater = snapshot.dynamicBusTapHighWater;
    result.dynamicVoiceTapHighWater = snapshot.dynamicVoiceTapHighWater;
    result.dynamicRouteRefusalCount = snapshot.dynamicRouteRefusalCount;
    result.dynamicPlanRetireOverflowCount =
        snapshot.dynamicPlanRetireOverflowCount;
    const bool capacityObservabilityValid =
        snapshot.activeVoiceCount == scene.voiceCount &&
        snapshot.voiceHighWater == scene.voiceCount &&
        snapshot.renderWorkerCount == workerCount &&
        snapshot.mixChannelCount == harken::kMaxChannels &&
        snapshot.deviceChannelCount == harken::kMaxChannels &&
        result.compressorPoolHighWater ==
            (featureEnabled
                 ? scene.voiceCompressorCount + scene.busCompressorCount
                 : 0u) &&
        result.dynamicRouteCurrent ==
            (featureRouted ? scene.routeCount : 0u) &&
        result.dynamicRouteHighWater ==
            (featureRouted ? scene.routeCount : 0u) &&
        result.dynamicBusTapHighWater == (featureRouted ? 8u : 0u) &&
        result.dynamicVoiceTapHighWater == (featureRouted ? 8u : 0u) &&
        result.arenaOverflowCount == 0 &&
        result.dynamicRouteRefusalCount == 0 &&
        result.dynamicPlanRetireOverflowCount == 0;
    const bool outputFinite = result.nonfiniteOutputSampleCount == 0;
    result.ok = capacityObservabilityValid && outputFinite;
    if (!outputFinite) {
        result.error = "non-finite-output";
    } else if (!capacityObservabilityValid) {
        result.error = "capacity-observability";
    } else {
        result.error = "none";
    }
    Vn_Term();
    return result;
}

void PrintCompressorSceneRow(const CompressorSceneSpec& scene,
                             std::uint32_t workerCount,
                             std::uint32_t warmupBlocks,
                             std::uint32_t measuredBlocks,
                             std::uint32_t run,
                             const CompressorSceneResult& result,
                             const char* identity,
                             bool canonicalGateApplicable) {
    constexpr double kDeadlineUs =
        1.0e6 * static_cast<double>(kBlock) / static_cast<double>(kRate);
    const bool gateApplies = scene.p99BudgetUs > 0.0;
    const bool absoluteDiagnosticPassed =
        gateApplies && result.ok && result.deadlineMisses == 0 &&
        result.p99Us <= scene.p99BudgetUs;
    const bool mixedCostGateApplicable =
        canonicalGateApplicable &&
        scene.kind == CompressorSceneKind::Mixed;
    const bool mixedCostGatePassed =
        mixedCostGateApplicable && result.ok &&
        result.p99Us <= scene.p99BudgetUs && gProcessPinned &&
        gProcessPrioritySet;
    const auto hygieneJson = CompressorMeasurementHygieneJson();
    const auto contractJson = CompressorPerformanceContractJson(
        mixedCostGateApplicable, mixedCostGatePassed,
        result.deadlineMisses);
    std::printf(
        "{\"benchmark\":\"compressor-matrix\",\"status\":\"measured\","
        "%s"
        "%s"
        "\"build\":\"release\",\"scenario\":\"%s\",\"run\":%u,"
        "\"sample_rate\":%u,\"block_frames\":%u,\"channels\":8,"
        "\"voices\":%u,\"voice_compressors\":%u,"
        "\"bus_compressors\":%u,\"unique_taps\":%u,\"routes\":%u,"
        "\"workers\":%u,\"process_pinned\":%s,\"pinned_cpu_count\":%u,"
        "\"priority_set\":%s,"
        "\"requested_process_priority_class\":\"high\","
        "\"warmup_blocks\":%u,"
        "\"measured_blocks\":%u,\"median_us\":%.6f,"
        "\"p99_us\":%.6f,\"max_us\":%.6f,\"deadline_us\":%.3f,"
        "\"deadline_misses\":%llu,\"p99_budget_us\":%.3f,"
        "\"output_finite\":%s,"
        "\"nonfinite_output_sample_count\":%llu,"
        "\"absolute_callback_budget_diagnostic_applicable\":%s,"
        "\"absolute_callback_budget_diagnostic_passed\":%s,"
        "\"mixed_absolute_gate_applicable\":%s,"
        "\"mixed_absolute_gate_passed\":%s,"
        "\"scoped_gate_applicable\":%s,"
        "\"scoped_gate_passed\":%s,"
        "\"percentile_method\":\"nearest-rank\",\"hash\":\"%016llx\","
        "\"worker_identity\":\"%s\","
        "\"init_dynamic_route_bytes\":%llu,"
        "\"init_compressor_pool_bytes\":%llu,"
        "\"arena_high_water_bytes\":%llu,"
        "\"arena_overflow_count\":%llu,"
        "\"compressor_pool_high_water\":%u,"
        "\"dynamic_route_current\":%u,\"dynamic_route_high_water\":%u,"
        "\"dynamic_bus_tap_high_water\":%u,"
        "\"dynamic_voice_tap_high_water\":%u,"
        "\"dynamic_route_refusal_count\":%llu,"
        "\"dynamic_plan_retire_overflow_count\":%llu,"
        "\"setup_valid\":%s,\"setup_error\":\"%s\","
        "\"phase1_comparison\":\"external-paired-run-required\"}\n",
        hygieneJson.data(), contractJson.data(), scene.name, run, kRate,
        kBlock, scene.voiceCount,
        scene.voiceCompressorCount, scene.busCompressorCount,
        scene.routeCount == 0 ? 0u : 16u, scene.routeCount, workerCount,
        gProcessPinned ? "true" : "false", gPinnedCpuCount,
        gProcessPrioritySet ? "true" : "false", warmupBlocks,
        measuredBlocks,
        result.p50Us, result.p99Us, result.maxUs, kDeadlineUs,
        static_cast<unsigned long long>(result.deadlineMisses),
        scene.p99BudgetUs,
        result.nonfiniteOutputSampleCount == 0 ? "true" : "false",
        static_cast<unsigned long long>(
            result.nonfiniteOutputSampleCount),
        gateApplies ? "true" : "false",
        absoluteDiagnosticPassed ? "true" : "false",
        mixedCostGateApplicable ? "true" : "false",
        mixedCostGatePassed ? "true" : "false",
        mixedCostGateApplicable ? "true" : "false",
        mixedCostGatePassed ? "true" : "false",
        static_cast<unsigned long long>(result.hash), identity,
        static_cast<unsigned long long>(result.initDynamicRouteBytes),
        static_cast<unsigned long long>(result.initCompressorPoolBytes),
        static_cast<unsigned long long>(result.arenaHighWaterBytes),
        static_cast<unsigned long long>(result.arenaOverflowCount),
        result.compressorPoolHighWater, result.dynamicRouteCurrent,
        result.dynamicRouteHighWater, result.dynamicBusTapHighWater,
        result.dynamicVoiceTapHighWater,
        static_cast<unsigned long long>(result.dynamicRouteRefusalCount),
        static_cast<unsigned long long>(
            result.dynamicPlanRetireOverflowCount),
        result.ok ? "true" : "false", result.error);
}

void PrintCompressorPairedSessionRow(
    const CompressorSceneSpec& scene, std::uint32_t workerCount,
    std::uint32_t warmupBlocks, std::uint32_t measuredBlocks,
    std::uint32_t pairIndex, const char* role, bool featureSession,
    const CompressorSceneResult& result,
    bool canonicalCountsApplicable) {
    const bool mixedFeatureGateApplicable =
        canonicalCountsApplicable &&
        scene.kind == CompressorSceneKind::Mixed && featureSession;
    const bool mixedFeatureGatePassed =
        mixedFeatureGateApplicable && result.ok &&
        result.p99Us <= scene.p99BudgetUs &&
        gProcessPinned && gProcessPrioritySet;
    const auto hygieneJson = CompressorMeasurementHygieneJson();
    const auto contractJson = CompressorPerformanceContractJson(
        mixedFeatureGateApplicable, mixedFeatureGatePassed,
        result.deadlineMisses);
    std::printf(
        "{\"benchmark\":\"compressor-matrix-paired\","
        "\"status\":\"session\",\"build\":\"release\","
        "%s"
        "%s"
        "\"scenario\":\"%s\",\"pair\":%u,\"role\":\"%s\","
        "\"feature_session\":%s,\"workers\":%u,"
        "\"process_pinned\":%s,\"pinned_cpu_count\":%u,"
        "\"priority_set\":%s,"
        "\"requested_process_priority_class\":\"high\","
        "\"sample_rate\":%u,\"block_frames\":%u,\"channels\":8,"
        "\"warmup_blocks\":%u,\"measured_blocks\":%u,"
        "\"median_us\":%.6f,\"p99_us\":%.6f,\"max_us\":%.6f,"
        "\"deadline_misses\":%llu,\"hash\":\"%016llx\","
        "\"output_finite\":%s,"
        "\"nonfinite_output_sample_count\":%llu,"
        "\"init_dynamic_route_bytes\":%llu,"
        "\"init_compressor_pool_bytes\":%llu,"
        "\"arena_high_water_bytes\":%llu,"
        "\"arena_overflow_count\":%llu,"
        "\"compressor_pool_high_water\":%u,"
        "\"dynamic_route_current\":%u,"
        "\"dynamic_route_high_water\":%u,"
        "\"dynamic_bus_tap_high_water\":%u,"
        "\"dynamic_voice_tap_high_water\":%u,"
        "\"dynamic_route_refusal_count\":%llu,"
        "\"dynamic_plan_retire_overflow_count\":%llu,"
        "\"setup_pump_blocks\":%u,\"setup_valid\":%s,"
        "\"setup_error\":\"%s\","
        "\"mixed_feature_gate_applicable\":%s,"
        "\"mixed_feature_gate_passed\":%s,"
        "\"scoped_gate_applicable\":%s,"
        "\"scoped_gate_passed\":%s,"
        "\"absolute_callback_role\":\"%s\"}\n",
        hygieneJson.data(), contractJson.data(), scene.name, pairIndex, role,
        featureSession ? "true" : "false",
        workerCount, gProcessPinned ? "true" : "false",
        gPinnedCpuCount, gProcessPrioritySet ? "true" : "false",
        kRate, kBlock, warmupBlocks, measuredBlocks,
        result.p50Us, result.p99Us, result.maxUs,
        static_cast<unsigned long long>(result.deadlineMisses),
        static_cast<unsigned long long>(result.hash),
        result.nonfiniteOutputSampleCount == 0 ? "true" : "false",
        static_cast<unsigned long long>(
            result.nonfiniteOutputSampleCount),
        static_cast<unsigned long long>(result.initDynamicRouteBytes),
        static_cast<unsigned long long>(result.initCompressorPoolBytes),
        static_cast<unsigned long long>(result.arenaHighWaterBytes),
        static_cast<unsigned long long>(result.arenaOverflowCount),
        result.compressorPoolHighWater, result.dynamicRouteCurrent,
        result.dynamicRouteHighWater, result.dynamicBusTapHighWater,
        result.dynamicVoiceTapHighWater,
        static_cast<unsigned long long>(
            result.dynamicRouteRefusalCount),
        static_cast<unsigned long long>(
            result.dynamicPlanRetireOverflowCount),
        result.setupPumpBlocks, result.ok ? "true" : "false",
        result.error,
        mixedFeatureGateApplicable ? "true" : "false",
        mixedFeatureGatePassed ? "true" : "false",
        mixedFeatureGateApplicable ? "true" : "false",
        mixedFeatureGatePassed ? "true" : "false",
        scene.kind == CompressorSceneKind::Mixed && featureSession
            ? "mixed-scoped-gate"
            : "diagnostic");
}

bool PrintCompressorPairedRow(
    const CompressorSceneSpec& scene, std::uint32_t workerCount,
    std::uint32_t warmupBlocks, std::uint32_t measuredBlocks,
    std::uint32_t pairIndex, const CompressorSceneResult& baselineFirst,
    const CompressorSceneResult& featureFirst,
    const CompressorSceneResult& featureSecond,
    const CompressorSceneResult& baselineSecond,
    std::uint64_t* expectedBaselineHash, std::uint64_t* expectedFeatureHash,
    double* outDeltaP99, bool* outScopedGatePassed,
    bool canonicalCountsApplicable) {
    const bool sizesMatch =
        baselineFirst.blockDurationsUs.size() == measuredBlocks &&
        featureFirst.blockDurationsUs.size() == measuredBlocks &&
        featureSecond.blockDurationsUs.size() == measuredBlocks &&
        baselineSecond.blockDurationsUs.size() == measuredBlocks;
    std::vector<double> matchedDeltas;
    std::vector<double> baselineDurations;
    std::vector<double> featureDurations;
    if (sizesMatch) {
        matchedDeltas.reserve(measuredBlocks);
        baselineDurations.reserve(static_cast<std::size_t>(measuredBlocks) *
                                  2);
        featureDurations.reserve(static_cast<std::size_t>(measuredBlocks) *
                                 2);
        for (std::uint32_t block = 0; block < measuredBlocks; ++block) {
            const double baselineMean =
                0.5 * (baselineFirst.blockDurationsUs[block] +
                       baselineSecond.blockDurationsUs[block]);
            const double featureMean =
                0.5 * (featureFirst.blockDurationsUs[block] +
                       featureSecond.blockDurationsUs[block]);
            matchedDeltas.push_back(featureMean - baselineMean);
            baselineDurations.push_back(
                baselineFirst.blockDurationsUs[block]);
            baselineDurations.push_back(
                baselineSecond.blockDurationsUs[block]);
            featureDurations.push_back(featureFirst.blockDurationsUs[block]);
            featureDurations.push_back(featureSecond.blockDurationsUs[block]);
        }
    }
    const DurationStats deltaStats =
        ComputeDurationStats(matchedDeltas);
    const DurationStats baselineStats =
        ComputeDurationStats(baselineDurations);
    const DurationStats featureStats =
        ComputeDurationStats(featureDurations);
    *outDeltaP99 = deltaStats.p99Us;

    if (*expectedBaselineHash == 0) {
        *expectedBaselineHash = baselineFirst.hash;
    }
    if (*expectedFeatureHash == 0) {
        *expectedFeatureHash = featureFirst.hash;
    }
    const bool hashesStable =
        baselineFirst.hash == baselineSecond.hash &&
        featureFirst.hash == featureSecond.hash &&
        baselineFirst.hash == *expectedBaselineHash &&
        featureFirst.hash == *expectedFeatureHash;
    const bool sameInitConfig =
        std::memcmp(&baselineFirst.initConfig, &featureFirst.initConfig,
                    sizeof(VnConfig)) == 0 &&
        std::memcmp(&baselineFirst.initConfig, &featureSecond.initConfig,
                    sizeof(VnConfig)) == 0 &&
        std::memcmp(&baselineFirst.initConfig, &baselineSecond.initConfig,
                    sizeof(VnConfig)) == 0 &&
        baselineFirst.initDynamicRouteBytes ==
            featureFirst.initDynamicRouteBytes &&
        baselineFirst.initDynamicRouteBytes ==
            featureSecond.initDynamicRouteBytes &&
        baselineFirst.initDynamicRouteBytes ==
            baselineSecond.initDynamicRouteBytes &&
        baselineFirst.initCompressorPoolBytes ==
            featureFirst.initCompressorPoolBytes &&
        baselineFirst.initCompressorPoolBytes ==
            featureSecond.initCompressorPoolBytes &&
        baselineFirst.initCompressorPoolBytes ==
            baselineSecond.initCompressorPoolBytes;
    const bool setupPhaseAligned =
        baselineFirst.setupPumpBlocks == featureFirst.setupPumpBlocks &&
        baselineFirst.setupPumpBlocks == featureSecond.setupPumpBlocks &&
        baselineFirst.setupPumpBlocks == baselineSecond.setupPumpBlocks;
    const bool setupValid =
        sizesMatch && baselineFirst.ok && featureFirst.ok &&
        featureSecond.ok && baselineSecond.ok && hashesStable &&
        sameInitConfig && setupPhaseAligned && gProcessPinned &&
        gProcessPrioritySet;
    const std::uint64_t featureMisses =
        featureFirst.deadlineMisses + featureSecond.deadlineMisses;
    const std::uint64_t baselineMisses =
        baselineFirst.deadlineMisses + baselineSecond.deadlineMisses;
    const std::uint64_t nonfiniteOutputSampleCount =
        baselineFirst.nonfiniteOutputSampleCount +
        featureFirst.nonfiniteOutputSampleCount +
        featureSecond.nonfiniteOutputSampleCount +
        baselineSecond.nonfiniteOutputSampleCount;
    const bool allSessionsOutputFinite =
        nonfiniteOutputSampleCount == 0;
    const bool quartetHasZeroDeadlineMisses =
        featureMisses == 0 && baselineMisses == 0;
    const bool deltaWithinBudget =
        deltaStats.p99Us <= scene.p99BudgetUs;
    const bool absoluteWithinBudget =
        featureStats.p99Us <= scene.p99BudgetUs;
    const bool combinedAbsoluteDiagnosticPassed =
        setupValid && absoluteWithinBudget && featureMisses == 0;
    const bool featureFirstAbsolutePassed =
        featureFirst.ok && featureFirst.p99Us <= scene.p99BudgetUs &&
        featureFirst.deadlineMisses == 0;
    const bool featureSecondAbsolutePassed =
        featureSecond.ok && featureSecond.p99Us <= scene.p99BudgetUs &&
        featureSecond.deadlineMisses == 0;
    constexpr double kDeadlineUs =
        1.0e6 * static_cast<double>(kBlock) / static_cast<double>(kRate);
    const bool quartetAbsoluteP99BelowDeadline =
        baselineFirst.p99Us < kDeadlineUs &&
        featureFirst.p99Us < kDeadlineUs &&
        featureSecond.p99Us < kDeadlineUs &&
        baselineSecond.p99Us < kDeadlineUs;
    const bool aggregateDeltaConditionsMet =
        setupValid && deltaWithinBudget &&
        quartetAbsoluteP99BelowDeadline;
    const bool featureFirstCostWithinBudget =
        featureFirst.ok && featureFirst.p99Us <= scene.p99BudgetUs;
    const bool featureSecondCostWithinBudget =
        featureSecond.ok && featureSecond.p99Us <= scene.p99BudgetUs;
    const bool mixedAbsoluteConditionsMet =
        scene.kind == CompressorSceneKind::Mixed && setupValid &&
        featureFirstCostWithinBudget && featureSecondCostWithinBudget;
    const bool aggregateDeltaGatePassed =
        canonicalCountsApplicable && aggregateDeltaConditionsMet;
    const bool mixedAbsolutePassed =
        canonicalCountsApplicable && mixedAbsoluteConditionsMet;
    *outScopedGatePassed =
        (scene.kind == CompressorSceneKind::Mixed
             ? mixedAbsolutePassed
             : aggregateDeltaGatePassed);

    const auto hygieneJson = CompressorMeasurementHygieneJson();
    const auto contractJson = CompressorPerformanceContractJson(
        canonicalCountsApplicable, *outScopedGatePassed,
        featureMisses + baselineMisses);
    std::printf(
        "{\"benchmark\":\"compressor-matrix-paired\","
        "\"status\":\"measured\",\"build\":\"release\","
        "%s"
        "%s"
        "\"scenario\":\"%s\",\"pair\":%u,\"order\":\"B-F-F-B\","
        "\"session_model\":\"fresh-identical-config-equal-warmup\","
        "\"sample_rate\":%u,\"block_frames\":%u,\"channels\":8,"
        "\"workers\":%u,\"process_pinned\":%s,"
        "\"pinned_cpu_count\":%u,\"priority_set\":%s,"
        "\"requested_process_priority_class\":\"high\","
        "\"warmup_blocks_per_session\":%u,"
        "\"measured_blocks_per_session\":%u,"
        "\"matched_delta_blocks\":%u,"
        "\"paired_delta_median_us\":%.6f,"
        "\"paired_delta_p99_us\":%.6f,"
        "\"paired_delta_max_us\":%.6f,"
        "\"paired_delta_budget_us\":%.3f,"
        "\"paired_delta_within_budget\":%s,"
        "\"quartet_zero_deadline_misses\":%s,"
        "\"quartet_absolute_p99_below_deadline\":%s,"
        "\"all_sessions_output_finite\":%s,"
        "\"nonfinite_output_sample_count\":%llu,"
        "\"scoped_gate_applicable\":%s,"
        "\"feature_absolute_median_us\":%.6f,"
        "\"feature_absolute_p99_us\":%.6f,"
        "\"feature_absolute_max_us\":%.6f,"
        "\"feature_absolute_deadline_misses\":%llu,"
        "\"feature_first_absolute_p99_us\":%.6f,"
        "\"feature_first_absolute_max_us\":%.6f,"
        "\"feature_first_absolute_deadline_misses\":%llu,"
        "\"feature_first_absolute_diagnostic_passed\":%s,"
        "\"feature_second_absolute_p99_us\":%.6f,"
        "\"feature_second_absolute_max_us\":%.6f,"
        "\"feature_second_absolute_deadline_misses\":%llu,"
        "\"feature_second_absolute_diagnostic_passed\":%s,"
        "\"baseline_absolute_median_us\":%.6f,"
        "\"baseline_absolute_p99_us\":%.6f,"
        "\"baseline_absolute_max_us\":%.6f,"
        "\"baseline_absolute_deadline_misses\":%llu,"
        "\"baseline_first_absolute_p99_us\":%.6f,"
        "\"baseline_first_absolute_max_us\":%.6f,"
        "\"baseline_first_absolute_deadline_misses\":%llu,"
        "\"baseline_second_absolute_p99_us\":%.6f,"
        "\"baseline_second_absolute_max_us\":%.6f,"
        "\"baseline_second_absolute_deadline_misses\":%llu,"
        "\"combined_absolute_callback_diagnostic_passed\":%s,"
        "\"aggregate_delta_conditions_met\":%s,"
        "\"aggregate_delta_gate_passed\":%s,"
        "\"mixed_absolute_gate_passed\":%s,"
        "\"scoped_gate_basis\":\"%s\","
        "\"scoped_gate_passed\":%s,"
        "\"scoped_gate_status\":\"%s\","
        "\"baseline_hash\":\"%016llx\","
        "\"feature_hash\":\"%016llx\","
        "\"hashes_stable\":%s,\"same_init_config\":%s,"
        "\"setup_phase_aligned\":%s,"
        "\"baseline_first_setup_pump_blocks\":%u,"
        "\"feature_first_setup_pump_blocks\":%u,"
        "\"feature_second_setup_pump_blocks\":%u,"
        "\"baseline_second_setup_pump_blocks\":%u,"
        "\"feature_compressor_pool_high_water\":%u,"
        "\"feature_dynamic_route_high_water\":%u,"
        "\"feature_bus_tap_high_water\":%u,"
        "\"feature_voice_tap_high_water\":%u,"
        "\"setup_valid\":%s,"
        "\"acceptance_note\":\"Vintage 2 gates finite cost only; Decision B "
        "uses paired delta plus per-session absolute deadline bounds for "
        "aggregate Compressor cost, while mixed remains absolute\"}\n",
        hygieneJson.data(), contractJson.data(), scene.name, pairIndex,
        kRate, kBlock,
        workerCount,
        gProcessPinned ? "true" : "false", gPinnedCpuCount,
        gProcessPrioritySet ? "true" : "false", warmupBlocks,
        measuredBlocks, measuredBlocks, deltaStats.medianUs,
        deltaStats.p99Us, deltaStats.maxUs, scene.p99BudgetUs,
        deltaWithinBudget ? "true" : "false",
        quartetHasZeroDeadlineMisses ? "true" : "false",
        quartetAbsoluteP99BelowDeadline ? "true" : "false",
        allSessionsOutputFinite ? "true" : "false",
        static_cast<unsigned long long>(nonfiniteOutputSampleCount),
        canonicalCountsApplicable ? "true" : "false",
        featureStats.medianUs,
        featureStats.p99Us, featureStats.maxUs,
        static_cast<unsigned long long>(featureMisses),
        featureFirst.p99Us, featureFirst.maxUs,
        static_cast<unsigned long long>(featureFirst.deadlineMisses),
        featureFirstAbsolutePassed ? "true" : "false",
        featureSecond.p99Us, featureSecond.maxUs,
        static_cast<unsigned long long>(featureSecond.deadlineMisses),
        featureSecondAbsolutePassed ? "true" : "false",
        baselineStats.medianUs, baselineStats.p99Us, baselineStats.maxUs,
        static_cast<unsigned long long>(baselineMisses),
        baselineFirst.p99Us, baselineFirst.maxUs,
        static_cast<unsigned long long>(baselineFirst.deadlineMisses),
        baselineSecond.p99Us, baselineSecond.maxUs,
        static_cast<unsigned long long>(baselineSecond.deadlineMisses),
        combinedAbsoluteDiagnosticPassed ? "true" : "false",
        aggregateDeltaConditionsMet ? "true" : "false",
        aggregateDeltaGatePassed ? "true" : "false",
        mixedAbsolutePassed ? "true" : "false",
        scene.kind == CompressorSceneKind::Mixed
            ? "per-feature-run-absolute"
            : "matched-block-paired-delta",
        *outScopedGatePassed ? "true" : "false",
        !canonicalCountsApplicable
            ? "not-applicable-smoke"
            : (*outScopedGatePassed ? "passed" : "failed"),
        static_cast<unsigned long long>(baselineFirst.hash),
        static_cast<unsigned long long>(featureFirst.hash),
        hashesStable ? "true" : "false",
        sameInitConfig ? "true" : "false",
        setupPhaseAligned ? "true" : "false",
        baselineFirst.setupPumpBlocks, featureFirst.setupPumpBlocks,
        featureSecond.setupPumpBlocks, baselineSecond.setupPumpBlocks,
        featureFirst.compressorPoolHighWater,
        featureFirst.dynamicRouteHighWater,
        featureFirst.dynamicBusTapHighWater,
        featureFirst.dynamicVoiceTapHighWater,
        setupValid ? "true" : "false");
    return setupValid;
}

int RunCompressorPairedBenchmark(const CompressorMatrixOptions& options) {
    if (std::strcmp(options.caseName, "mixed") == 0 ||
        std::strcmp(options.caseName, "inactive") == 0) {
        std::fprintf(
            stderr,
            "--paired accepts only voice-self or bus-routed; mixed and "
            "inactive require their non-paired or external workflows\n");
        return 2;
    }
    if (options.workersSpecified && options.workerCount != 0) {
        std::fprintf(
            stderr,
            "--paired canonical coverage accepts only --workers 0\n");
        return 2;
    }
    const bool canonicalCountsApplicable =
        IsCanonicalCompressorGateRun(options);
    const bool caseAll = std::strcmp(options.caseName, "all") == 0;
    const auto hygieneJson = CompressorMeasurementHygieneJson();
    const auto metadataContractJson =
        CompressorPerformanceContractJson(false, false, 0);
    std::uint32_t expectedScopedCoverageMask = 0;
    std::uint32_t expectedScopedRows = 0;
    for (const CompressorSceneSpec& scene : kCompressorScenes) {
        if (scene.kind == CompressorSceneKind::Mixed ||
            scene.kind == CompressorSceneKind::Inactive ||
            !CompressorCaseSelected(scene, options.caseName)) {
            continue;
        }
        for (std::uint32_t workerCount : kCompressorWorkerCounts) {
            if (!CompressorWorkerSelected(options, scene, workerCount)) {
                continue;
            }
            expectedScopedCoverageMask |=
                CompressorScenarioWorkerCoverageBit(scene, workerCount);
            ++expectedScopedRows;
        }
    }
    std::printf(
        "{\"benchmark\":\"compressor-matrix-paired\","
        "\"status\":\"matrix\",\"order\":\"B-F-F-B\","
        "%s"
        "%s"
        "\"pairs\":%u,\"warmup_blocks_per_session\":%u,"
        "\"measured_blocks_per_session\":%u,"
        "\"process_pinned\":%s,\"pinned_cpu_count\":%u,"
        "\"priority_set\":%s,"
        "\"requested_process_priority_class\":\"high\","
        "\"canonical_counts\":%s,"
        "\"expected_scoped_scenario_worker_mask\":\"%08x\","
        "\"expected_scoped_rows\":%u,"
        "\"delta_method\":\"per-block feature-mean minus "
        "baseline-mean\",\"percentile_method\":\"nearest-rank\"}\n",
        hygieneJson.data(), metadataContractJson.data(), options.runs,
        options.warmupBlocks,
        options.measuredBlocks,
        gProcessPinned ? "true" : "false", gPinnedCpuCount,
        gProcessPrioritySet ? "true" : "false",
        canonicalCountsApplicable ? "true" : "false",
        expectedScopedCoverageMask, expectedScopedRows);
    bool allValid = gProcessPinned && gProcessPrioritySet;
    bool allScopedGatesPassed = true;
    std::uint64_t allDeadlineMisses = 0;
    std::uint64_t allNonfiniteOutputSampleCount = 0;
    std::uint32_t observedScopedCoverageMask = 0;
    std::uint32_t observedScopedRows = 0;
    for (const CompressorSceneSpec& scene : kCompressorScenes) {
        if (scene.kind == CompressorSceneKind::Mixed ||
            scene.kind == CompressorSceneKind::Inactive ||
            !CompressorCaseSelected(scene, options.caseName)) {
            continue;
        }
        for (std::uint32_t workerCount : kCompressorWorkerCounts) {
            if (!CompressorWorkerSelected(options, scene, workerCount)) {
                continue;
            }
            std::uint64_t expectedBaselineHash = 0;
            std::uint64_t expectedFeatureHash = 0;
            bool workerValid = true;
            bool workerGatePassed = true;
            std::uint64_t workerDeadlineMisses = 0;
            std::uint64_t workerNonfiniteOutputSampleCount = 0;
            observedScopedCoverageMask |=
                CompressorScenarioWorkerCoverageBit(scene, workerCount);
            ++observedScopedRows;
            std::vector<double> pairDeltaP99;
            pairDeltaP99.reserve(options.runs);
            for (std::uint32_t pair = 1; pair <= options.runs; ++pair) {
                const CompressorSceneResult baselineFirst =
                    RunCompressorScene(scene, workerCount,
                                       options.warmupBlocks,
                                       options.measuredBlocks, false);
                const CompressorSceneResult featureFirst =
                    RunCompressorScene(scene, workerCount,
                                       options.warmupBlocks,
                                       options.measuredBlocks, true);
                const CompressorSceneResult featureSecond =
                    RunCompressorScene(scene, workerCount,
                                       options.warmupBlocks,
                                       options.measuredBlocks, true);
                const CompressorSceneResult baselineSecond =
                    RunCompressorScene(scene, workerCount,
                                       options.warmupBlocks,
                                       options.measuredBlocks, false);
                const std::uint64_t pairDeadlineMisses =
                    baselineFirst.deadlineMisses +
                    featureFirst.deadlineMisses +
                    featureSecond.deadlineMisses +
                    baselineSecond.deadlineMisses;
                workerDeadlineMisses += pairDeadlineMisses;
                allDeadlineMisses += pairDeadlineMisses;
                const std::uint64_t pairNonfiniteOutputSampleCount =
                    baselineFirst.nonfiniteOutputSampleCount +
                    featureFirst.nonfiniteOutputSampleCount +
                    featureSecond.nonfiniteOutputSampleCount +
                    baselineSecond.nonfiniteOutputSampleCount;
                workerNonfiniteOutputSampleCount +=
                    pairNonfiniteOutputSampleCount;
                allNonfiniteOutputSampleCount +=
                    pairNonfiniteOutputSampleCount;
                PrintCompressorPairedSessionRow(
                    scene, workerCount, options.warmupBlocks,
                    options.measuredBlocks, pair, "B1", false,
                    baselineFirst, canonicalCountsApplicable);
                PrintCompressorPairedSessionRow(
                    scene, workerCount, options.warmupBlocks,
                    options.measuredBlocks, pair, "F1", true,
                    featureFirst, canonicalCountsApplicable);
                PrintCompressorPairedSessionRow(
                    scene, workerCount, options.warmupBlocks,
                    options.measuredBlocks, pair, "F2", true,
                    featureSecond, canonicalCountsApplicable);
                PrintCompressorPairedSessionRow(
                    scene, workerCount, options.warmupBlocks,
                    options.measuredBlocks, pair, "B2", false,
                    baselineSecond, canonicalCountsApplicable);
                double deltaP99 = 0.0;
                bool scopedGatePassed = false;
                const bool valid = PrintCompressorPairedRow(
                    scene, workerCount, options.warmupBlocks,
                    options.measuredBlocks, pair, baselineFirst,
                    featureFirst, featureSecond, baselineSecond,
                    &expectedBaselineHash, &expectedFeatureHash,
                    &deltaP99, &scopedGatePassed,
                    canonicalCountsApplicable);
                allValid = allValid && valid;
                workerValid = workerValid && valid;
                if (canonicalCountsApplicable) {
                    allScopedGatesPassed =
                        allScopedGatesPassed && scopedGatePassed;
                    workerGatePassed =
                        workerGatePassed && scopedGatePassed;
                }
                pairDeltaP99.push_back(deltaP99);
            }
            const DurationStats pairSummary =
                ComputeDurationStats(pairDeltaP99);
            const bool finalWorkerGatePassed =
                canonicalCountsApplicable && workerValid &&
                workerGatePassed &&
                (scene.kind == CompressorSceneKind::Mixed ||
                 pairSummary.maxUs <= scene.p99BudgetUs);
            const auto workerContractJson =
                CompressorPerformanceContractJson(
                    canonicalCountsApplicable, finalWorkerGatePassed,
                    workerDeadlineMisses);
            std::printf(
                "{\"benchmark\":\"compressor-matrix-paired\","
                "\"status\":\"aggregate\",\"scenario\":\"%s\","
                "%s"
                "%s"
                "\"workers\":%u,\"pairs\":%u,"
                "\"median_pair_delta_p99_us\":%.6f,"
                "\"max_pair_delta_p99_us\":%.6f,"
                "\"all_sessions_output_finite\":%s,"
                "\"nonfinite_output_sample_count\":%llu,"
                "\"process_pinned\":%s,\"pinned_cpu_count\":%u,"
                "\"priority_set\":%s,"
                "\"requested_process_priority_class\":\"high\","
                "\"scoped_gate_applicable\":%s,\"setup_valid\":%s,"
                "\"scoped_gate_passed\":%s,"
                "\"scoped_gate_status\":\"%s\"}\n",
                scene.name, hygieneJson.data(), workerContractJson.data(),
                workerCount, options.runs,
                pairSummary.medianUs, pairSummary.maxUs,
                workerNonfiniteOutputSampleCount == 0 ? "true" : "false",
                static_cast<unsigned long long>(
                    workerNonfiniteOutputSampleCount),
                gProcessPinned ? "true" : "false", gPinnedCpuCount,
                gProcessPrioritySet ? "true" : "false",
                canonicalCountsApplicable ? "true" : "false",
                workerValid ? "true" : "false",
                finalWorkerGatePassed ? "true" : "false",
                !canonicalCountsApplicable
                    ? "not-applicable-smoke"
                    : (finalWorkerGatePassed ? "passed" : "failed"));
            if (canonicalCountsApplicable) {
                allScopedGatesPassed =
                    allScopedGatesPassed && finalWorkerGatePassed;
            }
        }
    }
    const bool scopedCoverageComplete =
        expectedScopedRows != 0 &&
        observedScopedRows == expectedScopedRows &&
        observedScopedCoverageMask == expectedScopedCoverageMask;
    const bool requiredPairedCoverageComplete =
        (observedScopedCoverageMask & kRequiredPairedCoverageMask) ==
        kRequiredPairedCoverageMask;
    const bool scopedGateApplicable =
        canonicalCountsApplicable && scopedCoverageComplete &&
        (!caseAll || requiredPairedCoverageComplete);
    const bool scopedGatePassed =
        scopedGateApplicable && allValid && allScopedGatesPassed;
    const auto contractJson = CompressorPerformanceContractJson(
        scopedGateApplicable, scopedGatePassed, allDeadlineMisses);
    std::printf(
        "{\"benchmark\":\"compressor-matrix-paired\","
        "\"status\":\"complete\",\"setup_valid\":%s,"
        "%s"
        "%s"
        "\"process_pinned\":%s,\"pinned_cpu_count\":%u,"
        "\"priority_set\":%s,"
        "\"requested_process_priority_class\":\"high\","
        "\"canonical_counts\":%s,"
        "\"expected_scoped_scenario_worker_mask\":\"%08x\","
        "\"observed_scoped_scenario_worker_mask\":\"%08x\","
        "\"expected_scoped_rows\":%u,\"observed_scoped_rows\":%u,"
        "\"scoped_coverage_complete\":%s,"
        "\"all_sessions_output_finite\":%s,"
        "\"nonfinite_output_sample_count\":%llu,"
        "\"required_paired_scenario_worker_mask\":\"%08x\","
        "\"required_paired_coverage_complete\":%s,"
        "\"scoped_gate_applicable\":%s,"
        "\"scoped_gate_passed\":%s,"
        "\"scoped_gate_status\":\"%s\","
        "\"inactive_external_gate_selected\":false,"
        "\"inactive_external_gate_verified\":false,"
        "\"inactive_external_gate_status\":\"not-selected\","
        "\"overall_runtime_gate_passed\":false,"
        "\"overall_runtime_gate_status\":"
        "\"unverified-full-matrix-and-external-evidence-required\","
        "\"missing_runtime_evidence\":[\"isolated\","
        "\"mixed-workers-0-1-2-4\","
        "\"external-phase1-b1-f1-f2-b2\"]}\n",
        allValid ? "true" : "false", hygieneJson.data(),
        contractJson.data(),
        gProcessPinned ? "true" : "false", gPinnedCpuCount,
        gProcessPrioritySet ? "true" : "false",
        canonicalCountsApplicable ? "true" : "false",
        expectedScopedCoverageMask, observedScopedCoverageMask,
        expectedScopedRows, observedScopedRows,
        scopedCoverageComplete ? "true" : "false",
        allNonfiniteOutputSampleCount == 0 ? "true" : "false",
        static_cast<unsigned long long>(
            allNonfiniteOutputSampleCount),
        kRequiredPairedCoverageMask,
        requiredPairedCoverageComplete ? "true" : "false",
        scopedGateApplicable ? "true" : "false",
        scopedGatePassed ? "true" : "false",
        !canonicalCountsApplicable
            ? "not-applicable-smoke"
            : (!scopedCoverageComplete
                   ? "failed-incomplete-scenario-worker-coverage"
                   : (caseAll && !requiredPairedCoverageComplete
                          ? "failed-incomplete-required-paired-coverage"
                          : (scopedGatePassed ? "passed"
                                              : "failed"))));
    const bool scopedExecutionPassed =
        allValid && scopedCoverageComplete &&
        (!canonicalCountsApplicable || allScopedGatesPassed);
    return scopedExecutionPassed && !caseAll ? 0 : 1;
}

int RunCompressorMatrixBenchmark(int argc, char* argv[]) {
    CompressorMatrixOptions options;
    if (!ParseCompressorMatrixOptions(argc, argv, &options)) {
        PrintCompressorMatrixUsage();
        return 2;
    }
#ifndef NDEBUG
    const auto hygieneJson = CompressorMeasurementHygieneJson();
    const auto contractJson =
        CompressorPerformanceContractJson(false, false, 0);
    std::printf(
        "{\"benchmark\":\"compressor-matrix\",\"status\":\"skipped\","
        "%s"
        "%s"
        "\"reason\":\"debug-build\","
        "\"overall_runtime_gate_passed\":false,"
        "\"overall_runtime_gate_status\":"
        "\"unverified-release-evidence-required\"}\n",
        hygieneJson.data(), contractJson.data());
    return std::strcmp(options.caseName, "all") == 0 ? 1 : 0;
#else
    if (!ConfigureCompressorMeasurementHygiene(options)) {
        const auto hygieneJson = CompressorMeasurementHygieneJson();
        const auto contractJson =
            CompressorPerformanceContractJson(false, false, 0);
        std::printf(
            "{\"benchmark\":\"compressor-matrix\","
            "\"status\":\"error\",%s"
            "%s"
            "\"reason\":\"callback-thread-priority-gate-failed\","
            "\"overall_runtime_gate_passed\":false,"
            "\"overall_runtime_gate_status\":"
            "\"invalid-measurement-hygiene\"}\n",
            hygieneJson.data(), contractJson.data());
        return 3;
    }
    if (options.paired) {
        return RunCompressorPairedBenchmark(options);
    }
    const bool canonicalCountsApplicable =
        IsCanonicalCompressorGateRun(options);
    const bool caseAll = std::strcmp(options.caseName, "all") == 0;
    const auto hygieneJson = CompressorMeasurementHygieneJson();
    const auto metadataContractJson =
        CompressorPerformanceContractJson(false, false, 0);
    std::uint32_t expectedScopedCoverageMask = 0;
    std::uint32_t expectedScopedRows = 0;
    for (const CompressorSceneSpec& scene : kCompressorScenes) {
        if (!CompressorCaseSelected(scene, options.caseName)) {
            continue;
        }
        for (std::uint32_t workerCount : kCompressorWorkerCounts) {
            if (!CompressorWorkerSelected(options, scene, workerCount)) {
                continue;
            }
            expectedScopedCoverageMask |=
                CompressorScenarioWorkerCoverageBit(scene, workerCount);
            ++expectedScopedRows;
        }
    }
    std::printf(
        "{\"benchmark\":\"compressor-matrix\",\"status\":\"matrix\","
        "%s"
        "%s"
        "\"sample_rate\":%u,\"block_frames\":%u,\"channels\":8,"
        "\"warmup_blocks\":%u,\"measured_blocks\":%u,\"runs\":%u,"
        "\"workers\":[0,1,2,4],"
        "\"default_worker_policy\":\"mixed-all-others-serial\","
        "\"scenarios\":[\"voice-self-64\",\"bus-routed-16x4\","
        "\"mixed-worst\",\"inactive-route-platform\"],"
        "\"process_pinned\":%s,\"pinned_cpu_count\":%u,"
        "\"priority_set\":%s,"
        "\"requested_process_priority_class\":\"high\","
        "\"canonical_counts\":%s,"
        "\"expected_scoped_scenario_worker_mask\":\"%08x\","
        "\"expected_scoped_rows\":%u,"
        "\"inactive_overhead_gate\":"
        "\"requires-alternating-phase1-binary-runs\"}\n",
        hygieneJson.data(), metadataContractJson.data(), kRate, kBlock,
        options.warmupBlocks,
        options.measuredBlocks,
        options.runs, gProcessPinned ? "true" : "false",
        gPinnedCpuCount,
        gProcessPrioritySet ? "true" : "false",
        canonicalCountsApplicable ? "true" : "false",
        expectedScopedCoverageMask, expectedScopedRows);
    bool allValid = gProcessPinned && gProcessPrioritySet;
    bool allIdentical = true;
    bool mixedSelected = false;
    bool pairedAggregateSelected = false;
    bool mixedRunsPassed = true;
    bool inactiveExternalGateSelected = false;
    std::uint64_t allDeadlineMisses = 0;
    std::uint64_t allNonfiniteOutputSampleCount = 0;
    std::uint32_t observedScopedCoverageMask = 0;
    std::uint32_t observedScopedRows = 0;
    for (const CompressorSceneSpec& scene : kCompressorScenes) {
        if (!CompressorCaseSelected(scene, options.caseName)) {
            continue;
        }
        mixedSelected =
            mixedSelected || scene.kind == CompressorSceneKind::Mixed;
        pairedAggregateSelected =
            pairedAggregateSelected ||
            scene.kind == CompressorSceneKind::VoiceSelf ||
            scene.kind == CompressorSceneKind::BusRouted;
        inactiveExternalGateSelected =
            inactiveExternalGateSelected ||
            scene.kind == CompressorSceneKind::Inactive;
        std::uint64_t serialHash = 0;
        for (std::uint32_t run = 1; run <= options.runs; ++run) {
            for (std::uint32_t workerCount : kCompressorWorkerCounts) {
                if (!CompressorWorkerSelected(options, scene,
                                              workerCount)) {
                    continue;
                }
                if (run == 1) {
                    observedScopedCoverageMask |=
                        CompressorScenarioWorkerCoverageBit(
                            scene, workerCount);
                    ++observedScopedRows;
                }
                const CompressorSceneResult result =
                    RunCompressorScene(scene, workerCount,
                                       options.warmupBlocks,
                                       options.measuredBlocks);
                allValid = allValid && result.ok;
                allDeadlineMisses += result.deadlineMisses;
                allNonfiniteOutputSampleCount +=
                    result.nonfiniteOutputSampleCount;
                if (scene.kind == CompressorSceneKind::Mixed) {
                    const bool runPassed =
                        result.ok &&
                        result.p99Us <= scene.p99BudgetUs;
                    mixedRunsPassed = mixedRunsPassed && runPassed;
                }
                if (workerCount == 0) {
                    if (serialHash == 0) {
                        serialHash = result.hash;
                    } else {
                        allIdentical =
                            allIdentical && result.hash == serialHash;
                    }
                }
                const bool identityComparable = serialHash != 0;
                const bool identical =
                    !identityComparable || result.hash == serialHash;
                allIdentical = allIdentical && identical;
                PrintCompressorSceneRow(
                    scene, workerCount, options.warmupBlocks,
                    options.measuredBlocks, run, result,
                    !identityComparable
                        ? "unavailable"
                        : (workerCount == 0
                               ? "serial-base"
                               : (identical ? "identical" : "drift")),
                    canonicalCountsApplicable);
            }
        }
    }
    const bool scopedCoverageComplete =
        expectedScopedRows != 0 &&
        observedScopedRows == expectedScopedRows &&
        observedScopedCoverageMask == expectedScopedCoverageMask;
    const bool requiredMixedCoverageComplete =
        (observedScopedCoverageMask & kRequiredMixedCoverageMask) ==
        kRequiredMixedCoverageMask;
    const bool requiredInactiveCoverageComplete =
        (observedScopedCoverageMask & kRequiredInactiveCoverageMask) ==
        kRequiredInactiveCoverageMask;
    const bool requiredFullScenarioWorkerCoverageComplete =
        requiredMixedCoverageComplete &&
        requiredInactiveCoverageComplete;
    const bool scopedGateApplicable =
        canonicalCountsApplicable && scopedCoverageComplete &&
        mixedSelected && !pairedAggregateSelected &&
        !inactiveExternalGateSelected;
    const bool scopedGatePassed =
        scopedGateApplicable && allValid && allIdentical &&
        mixedRunsPassed;
    const auto contractJson = CompressorPerformanceContractJson(
        scopedGateApplicable, scopedGatePassed, allDeadlineMisses);
    std::printf(
        "{\"benchmark\":\"compressor-matrix\",\"status\":\"complete\","
        "\"setup_valid\":%s,\"worker_identity\":%s,"
        "%s"
        "%s"
        "\"process_pinned\":%s,\"pinned_cpu_count\":%u,"
        "\"priority_set\":%s,"
        "\"requested_process_priority_class\":\"high\","
        "\"canonical_counts\":%s,\"mixed_selected\":%s,"
        "\"paired_aggregate_selected\":%s,"
        "\"expected_scoped_scenario_worker_mask\":\"%08x\","
        "\"observed_scoped_scenario_worker_mask\":\"%08x\","
        "\"expected_scoped_rows\":%u,\"observed_scoped_rows\":%u,"
        "\"scoped_coverage_complete\":%s,"
        "\"output_finite\":%s,"
        "\"nonfinite_output_sample_count\":%llu,"
        "\"required_mixed_scenario_worker_mask\":\"%08x\","
        "\"required_mixed_coverage_complete\":%s,"
        "\"required_inactive_scenario_worker_mask\":\"%08x\","
        "\"required_inactive_coverage_complete\":%s,"
        "\"required_full_scenario_worker_coverage_complete\":%s,"
        "\"scoped_gate_applicable\":%s,"
        "\"scoped_gate_passed\":%s,"
        "\"mixed_per_run_absolute_gates_passed\":%s,"
        "\"inactive_external_gate_selected\":%s,"
        "\"inactive_external_gate_verified\":false,"
        "\"inactive_external_gate_status\":\"%s\","
        "\"scoped_gate_status\":\"%s\","
        "\"overall_runtime_gate_passed\":false,"
        "\"overall_runtime_gate_status\":"
        "\"unverified-full-matrix-and-external-evidence-required\","
        "\"missing_runtime_evidence\":[\"isolated\","
        "\"bus-worker-0-paired\","
        "\"external-phase1-b1-f1-f2-b2\"],"
        "\"phase1_comparison\":\"run --filter phase1 on alternating "
        "baseline and feature binaries\"}\n",
        allValid ? "true" : "false",
        allIdentical ? "true" : "false",
        hygieneJson.data(),
        contractJson.data(),
        gProcessPinned ? "true" : "false", gPinnedCpuCount,
        gProcessPrioritySet ? "true" : "false",
        canonicalCountsApplicable ? "true" : "false",
        mixedSelected ? "true" : "false",
        pairedAggregateSelected ? "true" : "false",
        expectedScopedCoverageMask, observedScopedCoverageMask,
        expectedScopedRows, observedScopedRows,
        scopedCoverageComplete ? "true" : "false",
        allNonfiniteOutputSampleCount == 0 ? "true" : "false",
        static_cast<unsigned long long>(
            allNonfiniteOutputSampleCount),
        kRequiredMixedCoverageMask,
        requiredMixedCoverageComplete ? "true" : "false",
        kRequiredInactiveCoverageMask,
        requiredInactiveCoverageComplete ? "true" : "false",
        requiredFullScenarioWorkerCoverageComplete ? "true" : "false",
        scopedGateApplicable ? "true" : "false",
        scopedGatePassed ? "true" : "false",
        canonicalCountsApplicable && mixedRunsPassed ? "true" : "false",
        inactiveExternalGateSelected ? "true" : "false",
        inactiveExternalGateSelected ? "unverified" : "not-selected",
        !canonicalCountsApplicable
            ? "not-applicable-smoke"
            : (!scopedCoverageComplete
                   ? "failed-incomplete-scenario-worker-coverage"
                   : (pairedAggregateSelected
                          ? "not-evaluated-use-paired-mode"
                          : (inactiveExternalGateSelected
                                 ? "unverified-external-old-binary-required"
                                 : (scopedGatePassed ? "passed"
                                                     : "failed")))));
    const bool scopedExecutionValid =
        allValid && allIdentical && scopedCoverageComplete;
    if (caseAll || inactiveExternalGateSelected) {
        return 1;
    }
    if (canonicalCountsApplicable) {
        return scopedGatePassed ? 0 : 1;
    }
    return scopedExecutionValid ? 0 : 1;
#endif
}

// One heavy scene rendered at the given worker count. Same content and call
// order every time, so worker count is the only variable.
Result RunScene(std::uint32_t workerCount) {
    Result result;

    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.deviceMode = VN_DEVICE_NULL;
    config.maxVoices = 64;
    config.blockFrames = kBlock;
    config.sampleRate = kRate;
    config.fxSlotsPerVoice = 2;
    config.voiceSendSlots = 2;
    config.mixChannelLayout = VN_CHANNEL_LAYOUT_5_1;
    config.deviceChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    config.renderWorkerCount = workerCount;
    config.arenaBytes = 1u << 21;

    if (Vn_Init(&config) != VN_OK) {
        return result;
    }
    harken::Engine* engine = harken::detail::ActiveEngine();

    Vn_CreateBus(kSendBus, VN_BUS_MASTER);

    // A long single-pass source (well over blocks * blockFrames even at the
    // slowest pitch) so every voice sounds through the whole run without loop
    // semantics entering the measurement.
    const std::size_t kSourceFrames =
        static_cast<std::size_t>(kBlocks) * kBlock * 2 + kRate;
    for (std::uint32_t i = 0; i < 8; ++i) {
        const std::vector<float> sine =
            MakeSine(kSourceFrames, 110.0 + 40.0 * static_cast<double>(i),
                     0.2f);
        VnPCMDesc desc{};
        desc.structSize = sizeof(VnPCMDesc);
        desc.sampleRate = kRate;
        desc.channels = 1;
        desc.layout = VN_PCM_LAYOUT_INTERLEAVED;
        desc.policy = VN_PCM_POLICY_COPY;
        desc.frames = sine.size();
        desc.samplesInterleaved = sine.data();
        Vn_RegisterPCM(static_cast<VnSoundId>(1 + i), &desc);
    }

    for (std::uint32_t v = 0; v < kVoices; ++v) {
        const VnSoundId id = static_cast<VnSoundId>(1 + (v % 8));
        VnPlayDesc play{};
        play.structSize = sizeof(VnPlayDesc);
        play.soundId = id;
        play.busId = VN_BUS_MASTER;
        play.volumeDb = -12.0f;
        play.pitchRatio = 0.75f + 0.01f * static_cast<float>(v % 16);
        play.loopCount = 1; // single pass; the source outlasts the whole run
        play.spatialized = 1;
        play.positionX = -1.0f + 0.05f * static_cast<float>(v % 40);
        play.positionZ = -3.0f;
        VnVoiceHandle h = 0;
        if (harkentest::PlayRaw(&play, &h) != VN_OK) {
            continue;
        }
        // An effect on every fourth voice (cascade work), a send on every
        // sixth (fold scatter work).
        if ((v % 4) == 0) {
            VnFeedbackDelayDesc d{};
            d.structSize = sizeof(VnFeedbackDelayDesc);
            d.delayMs = 15.0f;
            d.feedback = 0.35f;
            d.wet = 0.5f;
            d.dry = 1.0f;
            Vn_SetVoiceEffect(h, 0, VN_EFFECT_FEEDBACK_DELAY, AsBase(&d));
        }
        if ((v % 6) == 0) {
            Vn_SetVoiceSend(h, 0, kSendBus, -6.0f);
        }
    }
    Vn_Update();

    const std::uint32_t frames = engine->nullBackend.BlockFrames();
    const std::uint32_t channels = engine->nullBackend.Channels();

    const auto t0 = std::chrono::steady_clock::now();
    for (std::uint32_t b = 0; b < kBlocks; ++b) {
        engine->nullBackend.Pump(1);
        HashBytes(result.hash, engine->nullBackend.LastBlock(),
                  static_cast<std::size_t>(frames) * channels);
    }
    const auto t1 = std::chrono::steady_clock::now();
    result.wallMs =
        std::chrono::duration<double, std::milli>(t1 - t0).count();

    VnSnapshot snap{};
    snap.structSize = sizeof(VnSnapshot);
    if (Vn_CaptureSnapshot(&snap) == VN_OK) {
        result.busyPeakUs = snap.renderWorkerBusyPeakUs;
    }
    result.ok = true;
    Vn_Term();
    return result;
}

bool PinProcessToBenchmarkCpus() {
#if defined(_WIN32)
    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask,
                               &systemMask) == 0 ||
        processMask == 0) {
        return false;
    }
    DWORD_PTR benchmarkMask = 0;
    gPinnedCpuCount = 0;
    // Pin one benchmark process to a stable set that still accommodates the
    // callback thread and the maximum four render workers. CPU 0 is reserved
    // for common Windows system activity and is excluded from timing runs.
    constexpr std::uint32_t kRequiredCpuCount = 5;
    for (std::uint32_t bit = 1;
         bit < static_cast<std::uint32_t>(sizeof(DWORD_PTR) * 8) &&
         gPinnedCpuCount < kRequiredCpuCount;
         ++bit) {
        const DWORD_PTR candidate = static_cast<DWORD_PTR>(1) << bit;
        if ((processMask & candidate) != 0) {
            benchmarkMask |= candidate;
            ++gPinnedCpuCount;
        }
    }
    return gPinnedCpuCount == kRequiredCpuCount &&
           SetProcessAffinityMask(GetCurrentProcess(), benchmarkMask) != 0;
#else
    return false;
#endif
}

bool SetBenchmarkProcessPriority() {
#if defined(_WIN32)
    const HANDLE process = GetCurrentProcess();
    return SetPriorityClass(process, HIGH_PRIORITY_CLASS) != 0 &&
           GetPriorityClass(process) == HIGH_PRIORITY_CLASS;
#else
    return false;
#endif
}

} // namespace

bool IsBenchmarkProcessPinned() {
    return gProcessPinned;
}

bool IsBenchmarkProcessPrioritySet() {
    return gProcessPrioritySet;
}

std::uint32_t BenchmarkPinnedCpuCount() {
    return gPinnedCpuCount;
}

int main(int argc, char* argv[]) {
    // Set the process class before Vn_Init creates any render workers. Threads
    // created later run in this process priority class and inherit its process
    // affinity constraints.
    gProcessPrioritySet = SetBenchmarkProcessPriority();
    gProcessPinned = PinProcessToBenchmarkCpus();
    if (argc > 1) {
        if (std::strcmp(argv[1], "--help") == 0) {
            std::printf(
                "Usage: harken_benchmarks [--filter "
                "render-worker|curve-update|limiter-release|"
                "compressor-release|compressor-matrix|phase1 "
                "[options]]\n");
            return 0;
        }
        if (argc < 3 || std::strcmp(argv[1], "--filter") != 0) {
            std::fprintf(stderr, "Unknown benchmark arguments; use --help\n");
            return 2;
        }
        if (std::strcmp(argv[2], "curve-update") == 0) {
            return RunCurveUpdateBenchmark(argc - 3, argv + 3);
        }
        if (std::strcmp(argv[2], "limiter-release") == 0) {
            return RunLimiterReleaseBenchmark(argc - 3, argv + 3);
        }
        if (std::strcmp(argv[2], "compressor-release") == 0) {
            return RunCompressorReleaseBenchmark(argc - 3, argv + 3);
        }
        if (std::strcmp(argv[2], "compressor-matrix") == 0) {
            return RunCompressorMatrixBenchmark(argc - 3, argv + 3);
        }
        if (std::strcmp(argv[2], "phase1") == 0) {
            return RunPhase1Benchmark(argc - 3);
        }
        if (std::strcmp(argv[2], "render-worker") != 0 || argc != 3) {
            std::fprintf(stderr, "Unknown benchmark filter; use --help\n");
            return 2;
        }
    }

    const std::uint32_t counts[] = {0, 1, 2, 4};

    std::printf("Job-parallel render scaling: %u voices, %u blocks, "
                "5.1 mix -> stereo device, %u-frame blocks @ %u Hz\n",
                kVoices, kBlocks, kBlock, kRate);
    const double audioSeconds =
        static_cast<double>(kBlocks) * kBlock / static_cast<double>(kRate);
    std::printf("%-8s %12s %12s %12s %14s %10s\n", "workers", "wall_ms",
                "blocks/s", "realtime_x", "busyPeak_us", "identity");

    Result serial;
    bool allIdentical = true;
    for (std::uint32_t c : counts) {
        const Result r = RunScene(c);
        if (!r.ok) {
            std::printf("%-8u  <init failed>\n", c);
            return 2;
        }
        if (c == 0) {
            serial = r;
        }
        const bool identical = (r.hash == serial.hash);
        allIdentical = allIdentical && identical;
        const double blocksPerSec =
            r.wallMs > 0.0 ? kBlocks / (r.wallMs / 1000.0) : 0.0;
        const double realtimeX =
            r.wallMs > 0.0 ? audioSeconds / (r.wallMs / 1000.0) : 0.0;
        std::printf("%-8u %12.2f %12.0f %12.1f %14u %10s\n", c, r.wallMs,
                    blocksPerSec, realtimeX, r.busyPeakUs,
                    (c == 0) ? "(base)" : (identical ? "identical" : "DRIFT"));
    }

    if (!allIdentical) {
        std::printf("FAIL: a worker count drifted from the serial baseline\n");
        return 1;
    }
    std::printf("All worker counts byte-identical to serial.\n");
    return 0;
}
