// Release-only NullBackend laboratory characterization for the M11 Runtime
// capacity matrix. Wall-clock results describe this binary and host only.
// They are not a supported-voice-count promise or real-device qualification.
#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Engine.h"
#include "Common/RawVoiceTestSupport.h"
#include "UnitTest/AllocationProbe.h"
#include "harken/core/VoiceLedger.h"
#include "harken/media/StreamFormat.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <numeric>
#include <new>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace {

constexpr std::uint32_t kBlockFrames = 256;
constexpr std::uint32_t kCanonicalWarmupBlocksPerRepetition = 64;
constexpr std::uint32_t kCanonicalMeasuredBlocksPerRepetition = 400;
constexpr std::uint32_t kCanonicalRepetitions = 3;
constexpr std::uint32_t kPilotPrimingBlocks = 16;
constexpr std::uint32_t kPilotMeasuredBlocks = 32;
constexpr std::uint32_t kSmokeWarmupBlocksTotal = 8;
constexpr std::uint32_t kSmokeMeasuredBlocksTotal = 16;
constexpr std::uint32_t kArenaBytes = 16u << 20;
constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;
constexpr float kOutputFloor = 1.0e-8f;
constexpr double kM12ScreeningP99Fraction = 0.70;
constexpr const char* kBenchmarkName = "runtime-capacity-matrix";
constexpr const char* kSchema = "m11-null-v2";

struct Options {
    bool smoke = false;
    bool pilot = false;
    std::string scenarioId;
};

struct Environment {
    bool processPrioritySet = false;
    bool processAffinitySet = false;
    bool threadPrioritySet = false;
    bool threadAffinitySet = false;
    int threadPriority = 0;
    bool debuggerAttached = false;
    bool profilerAttached = false;
    std::uint32_t pinnedCpuCount = 0;
    int pinnedCpuIndex = -1;
    std::uint32_t hardwareThreads = 0;
    std::string processorIdentifier = "unknown";
    std::string sourceCommit = "unknown";
    std::string sourceDirty = "unknown";
};

struct RenderCase {
    const char* id = "";
    std::uint32_t renderCapacity = 64;
    std::uint32_t virtualCapacity = 64;
    std::uint32_t sourceRate = 48000;
    std::uint32_t mixRate = 48000;
    std::uint32_t sourceChannels = 1;
    int pitchSemitones = 0;
};

struct RunResult {
    bool setupOk = false;
    const char* error = nullptr;
    std::vector<double> callbackUs;
    std::uint64_t hash = kFnvOffset;
    float peak = 0.0f;
    std::uint64_t nonfiniteSampleCount = 0;
    std::uint32_t warmupBlocksUsed = 0;
    VnSnapshot snapshot{};
};

struct TimingSummary {
    double meanUs = 0.0;
    double p12_5Us = 0.0;
    double p15_625Us = 0.0;
    double p50Us = 0.0;
    double p95Us = 0.0;
    double p99Us = 0.0;
    double maximumUs = 0.0;
    std::uint64_t nominalPeriodExceedCount = 0;
    std::uint32_t warmupBlocks = 0;
};

struct CaseResult {
    bool integrityPassed = false;
    bool deterministic = true;
    const char* error = nullptr;
    TimingSummary timing;
    std::vector<TimingSummary> repetitionTimings;
    std::uint64_t hash = kFnvOffset;
    float peak = 0.0f;
    std::uint64_t nonfiniteSampleCount = 0;
    VnSnapshot snapshot{};
};

struct VirtualRunResult {
    bool setupOk = false;
    const char* error = nullptr;
    std::vector<double> callbackUs;
    std::vector<double> updateUs;
    std::uint64_t hash = kFnvOffset;
    float peak = 0.0f;
    std::uint64_t nonfiniteSampleCount = 0;
    std::uint32_t classifiedLive = 0;
    std::uint32_t classifiedParked = 0;
    std::uint32_t classifiedOther = 0;
    std::uint32_t warmupBlocksUsed = 0;
    VnSnapshot snapshot{};
};

struct VirtualCaseResult {
    bool integrityPassed = false;
    bool deterministic = true;
    const char* error = nullptr;
    TimingSummary callbackTiming;
    TimingSummary updateTiming;
    std::vector<TimingSummary> callbackRepetitions;
    std::vector<TimingSummary> updateRepetitions;
    std::uint64_t hash = kFnvOffset;
    float peak = 0.0f;
    std::uint64_t nonfiniteSampleCount = 0;
    std::uint32_t classifiedLive = 0;
    std::uint32_t classifiedParked = 0;
    std::uint32_t classifiedOther = 0;
    VnSnapshot snapshot{};
};

struct StreamingCase {
    const char* id = "";
    const char* mode = "healthy";
    std::uint32_t renderCapacity = 64;
    std::uint32_t virtualCapacity = 64;
    std::uint32_t streamVoices = 1;
    std::uint32_t pcmVoices = 0;
    std::uint32_t expectedReturnRefused = 0;
    std::uint32_t expectedVoiceCompleted = 0;
};

struct StreamingRunResult {
    bool setupOk = false;
    const char* error = nullptr;
    std::vector<double> callbackUs;
    std::vector<double> serviceUs;
    std::vector<double> updateUs;
    std::uint64_t hash = kFnvOffset;
    float peak = 0.0f;
    std::uint64_t nonfiniteSampleCount = 0;
    std::uint32_t warmupBlocksUsed = 0;
    VnSnapshot snapshot{};
};

struct StreamingCaseResult {
    bool integrityPassed = false;
    bool deterministic = true;
    const char* error = nullptr;
    TimingSummary callbackTiming;
    TimingSummary serviceTiming;
    TimingSummary updateTiming;
    std::vector<TimingSummary> callbackRepetitions;
    std::vector<TimingSummary> serviceRepetitions;
    std::vector<TimingSummary> updateRepetitions;
    std::uint64_t hash = kFnvOffset;
    float peak = 0.0f;
    std::uint64_t nonfiniteSampleCount = 0;
    VnSnapshot snapshot{};
};

class EngineGuard {
public:
    explicit EngineGuard(bool active) noexcept : m_active(active) {}

    ~EngineGuard() {
        if (m_active) {
            Vn_Term();
        }
    }

    EngineGuard(const EngineGuard&) = delete;
    EngineGuard& operator=(const EngineGuard&) = delete;

private:
    bool m_active = false;
};

void DiscardNotification(const VnNotification*, void*) {}

void PrintJsonString(const char* value) {
    std::putchar('"');
    if (value != nullptr) {
        for (const unsigned char c :
             std::string(reinterpret_cast<const char*>(value))) {
            switch (c) {
            case '"':
                std::fputs("\\\"", stdout);
                break;
            case '\\':
                std::fputs("\\\\", stdout);
                break;
            case '\b':
                std::fputs("\\b", stdout);
                break;
            case '\f':
                std::fputs("\\f", stdout);
                break;
            case '\n':
                std::fputs("\\n", stdout);
                break;
            case '\r':
                std::fputs("\\r", stdout);
                break;
            case '\t':
                std::fputs("\\t", stdout);
                break;
            default:
                if (c < 0x20) {
                    std::printf("\\u%04x", static_cast<unsigned>(c));
                } else {
                    std::putchar(static_cast<int>(c));
                }
                break;
            }
        }
    }
    std::putchar('"');
}

std::string EnvironmentValue(const char* name) {
#if defined(_WIN32)
    const DWORD required = GetEnvironmentVariableA(name, nullptr, 0);
    if (required == 0) {
        return "unknown";
    }
    std::string value(required, '\0');
    const DWORD written =
        GetEnvironmentVariableA(name, value.data(), required);
    if (written == 0 || written >= required) {
        return "unknown";
    }
    value.resize(written);
    return value;
#else
    const char* value = std::getenv(name);
    return value != nullptr ? value : "unknown";
#endif
}

Environment ConfigureEnvironment() {
    Environment environment;
    environment.hardwareThreads = std::thread::hardware_concurrency();
    environment.processorIdentifier = EnvironmentValue("PROCESSOR_IDENTIFIER");
    environment.sourceCommit = EnvironmentValue("HARKEN_M11_SOURCE_COMMIT");
    environment.sourceDirty = EnvironmentValue("HARKEN_M11_SOURCE_DIRTY");
#if defined(_WIN32)
    environment.debuggerAttached = IsDebuggerPresent() != 0;
    environment.profilerAttached =
        EnvironmentValue("COR_ENABLE_PROFILING") == "1" ||
        EnvironmentValue("CORECLR_ENABLE_PROFILING") == "1";
    environment.processPrioritySet =
        SetPriorityClass(GetCurrentProcess(), HIGH_PRIORITY_CLASS) != 0 &&
        GetPriorityClass(GetCurrentProcess()) == HIGH_PRIORITY_CLASS;

    DWORD_PTR processMask = 0;
    DWORD_PTR systemMask = 0;
    if (GetProcessAffinityMask(GetCurrentProcess(), &processMask,
                               &systemMask) != 0 &&
        processMask != 0) {
        DWORD_PTR benchmarkMask = 0;
        for (std::uint32_t bit = 1;
             bit < static_cast<std::uint32_t>(sizeof(DWORD_PTR) * 8);
             ++bit) {
            const DWORD_PTR candidate = static_cast<DWORD_PTR>(1) << bit;
            if ((processMask & candidate) != 0) {
                benchmarkMask = candidate;
                environment.pinnedCpuIndex = static_cast<int>(bit);
                break;
            }
        }
        if (benchmarkMask == 0) {
            benchmarkMask = processMask & (~processMask + 1);
            for (std::uint32_t bit = 0;
                 bit < static_cast<std::uint32_t>(sizeof(DWORD_PTR) * 8);
                 ++bit) {
                if (benchmarkMask ==
                    (static_cast<DWORD_PTR>(1) << bit)) {
                    environment.pinnedCpuIndex = static_cast<int>(bit);
                    break;
                }
            }
        }
        environment.pinnedCpuCount = benchmarkMask != 0 ? 1u : 0u;
        environment.processAffinitySet =
            environment.pinnedCpuCount == 1u &&
            SetProcessAffinityMask(GetCurrentProcess(), benchmarkMask) != 0;
        environment.threadAffinitySet =
            environment.processAffinitySet &&
            SetThreadAffinityMask(GetCurrentThread(), benchmarkMask) != 0;
    }
    environment.threadPrioritySet =
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST) != 0;
    environment.threadPriority = GetThreadPriority(GetCurrentThread());
    environment.threadPrioritySet =
        environment.threadPrioritySet &&
        environment.threadPriority == THREAD_PRIORITY_HIGHEST;
#endif
    return environment;
}

const char* CompilerName() {
#if defined(_MSC_VER)
    return "msvc";
#elif defined(__clang__)
    return "clang";
#elif defined(__GNUC__)
    return "gcc";
#else
    return "unknown";
#endif
}

double PitchRatio(int semitones) {
    return std::exp2(static_cast<double>(semitones) / 12.0);
}

std::vector<float> MakeInterleavedSource(std::uint32_t sourceRate,
                                         std::uint32_t channels) {
    constexpr double kPi = 3.14159265358979323846;
    const std::uint32_t frames = sourceRate;
    std::vector<float> samples(static_cast<std::size_t>(frames) * channels);
    for (std::uint32_t frame = 0; frame < frames; ++frame) {
        for (std::uint32_t channel = 0; channel < channels; ++channel) {
            const double frequency = channel == 0 ? 440.0 : 659.255;
            samples[static_cast<std::size_t>(frame) * channels + channel] =
                0.25f * static_cast<float>(
                            std::sin(2.0 * kPi * frequency *
                                     static_cast<double>(frame) /
                                     static_cast<double>(sourceRate)));
        }
    }
    return samples;
}

std::vector<std::uint8_t> BuildMonoLoopStream(
    VnSoundId soundId, const std::vector<float>& source,
    std::uint32_t sampleRate) {
    constexpr std::uint32_t kChunkFrames = 256;
    constexpr std::uint16_t kChannels = 1;
    const std::uint64_t frameCount = source.size();
    const std::uint64_t chunkBytes =
        harken::StreamChunkBytes(kChunkFrames, kChannels);
    const std::uint32_t chunkCount =
        harken::StreamChunkCount(frameCount, kChunkFrames);
    const std::uint64_t checksumOffset =
        static_cast<std::uint64_t>(harken::kStreamHeaderBytes) +
        harken::kStreamTocEntryBytes;
    const std::uint64_t checksumBytes =
        static_cast<std::uint64_t>(chunkCount) *
        harken::kStreamChunkChecksumBytes;
    const std::uint64_t payloadOffset =
        (checksumOffset + checksumBytes +
         harken::kStreamPayloadAlign - 1) /
        harken::kStreamPayloadAlign * harken::kStreamPayloadAlign;
    const std::uint64_t payloadBytes =
        static_cast<std::uint64_t>(chunkCount) * chunkBytes;
    std::vector<std::uint8_t> bytes(
        static_cast<std::size_t>(payloadOffset + payloadBytes), 0);

    for (std::uint32_t chunkIndex = 0; chunkIndex < chunkCount;
         ++chunkIndex) {
        const std::uint32_t validFrames =
            harken::StreamChunkValidFrames(
                chunkIndex, kChunkFrames, frameCount);
        std::uint8_t* chunk =
            bytes.data() +
            static_cast<std::size_t>(
                harken::StreamChunkFileOffset(
                    payloadOffset, chunkIndex, chunkBytes));
        std::memcpy(
            chunk,
            source.data() +
                static_cast<std::size_t>(chunkIndex) * kChunkFrames,
            static_cast<std::size_t>(validFrames) * sizeof(float));
        harken::StoreLE64(
            bytes.data() +
                static_cast<std::size_t>(checksumOffset) +
                static_cast<std::size_t>(chunkIndex) *
                    harken::kStreamChunkChecksumBytes,
            harken::Fnv1a64(
                chunk, static_cast<std::size_t>(chunkBytes)));
    }

    harken::StreamTocEntry toc;
    toc.soundId = soundId;
    toc.sampleRate = sampleRate;
    toc.channelCount = kChannels;
    toc.sampleFormat = harken::kStreamSampleFormatF32Planar;
    toc.frameCount = frameCount;
    toc.payloadOffset = payloadOffset;
    toc.payloadBytes = payloadBytes;
    toc.chunkChecksumOffset = checksumOffset;
    toc.chunkCount = chunkCount;
    toc.flags = harken::kStreamFlagLoopable;
    toc.loopStartFrame = 0;
    toc.loopEndFrame = frameCount;
    harken::SerializeStreamTocEntry(
        toc, bytes.data() + harken::kStreamHeaderBytes);

    std::uint64_t tocChecksum =
        harken::Fnv1a64(
            bytes.data() + harken::kStreamHeaderBytes,
            harken::kStreamTocEntryBytes);
    tocChecksum = harken::Fnv1a64(
        bytes.data() +
            static_cast<std::size_t>(checksumOffset),
        static_cast<std::size_t>(checksumBytes), tocChecksum);
    harken::StreamHeader header;
    header.formatVersion = harken::kStreamFormatVersion;
    header.headerBytes = harken::kStreamHeaderBytes;
    header.fileBytes = bytes.size();
    header.entryCount = harken::kStreamEntryCount;
    header.tocOffset = harken::kStreamHeaderBytes;
    header.chunkFrames = kChunkFrames;
    header.tocChecksum = tocChecksum;
    harken::SerializeStreamHeader(header, bytes.data());
    return bytes;
}

std::string WriteBenchmarkStream(VnSoundId soundId) {
    constexpr std::uint32_t kRate = 48000;
    const std::vector<float> source =
        MakeInterleavedSource(kRate, 1);
    const std::vector<std::uint8_t> bytes =
        BuildMonoLoopStream(soundId, source, kRate);
    const std::filesystem::path directory =
        std::filesystem::current_path() / ".temp";
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) {
        return {};
    }
    const std::filesystem::path path =
        directory / "m11-capacity.stream";
    std::ofstream output(
        path, std::ios::binary | std::ios::trunc);
    if (!output.good()) {
        return {};
    }
    output.write(
        reinterpret_cast<const char*>(bytes.data()),
        static_cast<std::streamsize>(bytes.size()));
    if (!output.good()) {
        return {};
    }
    return path.string();
}

void HashAndInspect(const float* samples, std::size_t count,
                    std::uint64_t* hash, float* peak,
                    std::uint64_t* nonfiniteSampleCount) {
    const auto* bytes = reinterpret_cast<const std::uint8_t*>(samples);
    for (std::size_t index = 0; index < count * sizeof(float); ++index) {
        *hash ^= bytes[index];
        *hash *= kFnvPrime;
    }
    for (std::size_t index = 0; index < count; ++index) {
        const float sample = samples[index];
        if (!std::isfinite(sample)) {
            ++*nonfiniteSampleCount;
            continue;
        }
        *peak = std::max(*peak, std::fabs(sample));
    }
}

double Percentile(const std::vector<double>& sorted, double percentile) {
    if (sorted.empty()) {
        return 0.0;
    }
    const std::size_t rank = static_cast<std::size_t>(std::ceil(
        percentile * static_cast<double>(sorted.size())));
    return sorted[std::max<std::size_t>(1, rank) - 1];
}

TimingSummary SummarizeTiming(std::vector<double> samples,
                              double nominalPeriodUs) {
    TimingSummary summary;
    if (samples.empty()) {
        return summary;
    }
    summary.meanUs =
        std::accumulate(samples.begin(), samples.end(), 0.0) /
        static_cast<double>(samples.size());
    summary.nominalPeriodExceedCount =
        static_cast<std::uint64_t>(std::count_if(
            samples.begin(), samples.end(),
            [nominalPeriodUs](double value) { return value >= nominalPeriodUs; }));
    std::sort(samples.begin(), samples.end());
    summary.p12_5Us = Percentile(samples, 0.125);
    summary.p15_625Us = Percentile(samples, 0.15625);
    summary.p50Us = Percentile(samples, 0.50);
    summary.p95Us = Percentile(samples, 0.95);
    summary.p99Us = Percentile(samples, 0.99);
    summary.maximumUs = samples.back();
    return summary;
}

void UseWorstRepetitionP99(
    TimingSummary* aggregate,
    const std::vector<TimingSummary>& repetitions) {
    for (const TimingSummary& repetition : repetitions) {
        aggregate->p99Us = std::max(aggregate->p99Us, repetition.p99Us);
    }
}

bool WarmupWindowsStable(const std::vector<double>& timings,
                         std::size_t firstBegin,
                         std::size_t secondBegin) {
    constexpr std::size_t kWindow = 16;
    if (timings.size() < secondBegin + kWindow) {
        return false;
    }
    std::vector<double> first(
        timings.begin() + firstBegin,
        timings.begin() + firstBegin + kWindow);
    std::vector<double> second(
        timings.begin() + secondBegin,
        timings.begin() + secondBegin + kWindow);
    std::sort(first.begin(), first.end());
    std::sort(second.begin(), second.end());
    const double a = Percentile(first, 0.5);
    const double b = Percentile(second, 0.5);
    return a > 0.0 && std::fabs(b - a) / a <= 0.10;
}

bool SnapshotPassesRenderGate(const VnSnapshot& snapshot,
                              std::uint32_t expectedRenderVoices) {
    return snapshot.activeVoiceCount == expectedRenderVoices &&
           snapshot.parkedVoiceCount == 0 &&
           snapshot.voiceHighWater == expectedRenderVoices &&
           snapshot.voiceCompletedCount == 0 &&
           snapshot.staleHandleCount == 0 &&
           snapshot.noVoiceCount == 0 &&
           snapshot.backpressureCount == 0 &&
           snapshot.notificationDropCount == 0 &&
           snapshot.logicNotificationDropCount == 0 &&
           snapshot.logDropCount == 0 &&
           snapshot.logicLogDropCount == 0 &&
           snapshot.paramDropCount == 0 &&
           snapshot.arenaOverflowCount == 0 &&
           snapshot.voiceStarvedCount == 0 &&
           snapshot.returnRefusedCount == 0 &&
           snapshot.pendingStartCount == 0;
}

RunResult RunRenderOnce(const RenderCase& scenario,
                        std::uint32_t warmupBlocks,
                        std::uint32_t measuredBlocks) {
    RunResult result;
    VnConfig config{};
    config.structSize = sizeof(config);
    config.sampleRate = scenario.mixRate;
    config.blockFrames = kBlockFrames;
    config.maxVoices = scenario.renderCapacity;
    config.maxParkedVoices = scenario.virtualCapacity;
    config.arenaBytes = kArenaBytes;
    config.deviceMode = VN_DEVICE_NULL;
    config.commandQueueDepth = 2048;
    config.notificationQueueDepth = 2048;
    config.mixChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    config.deviceChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    config.renderWorkerCount = 0;

    const VnResult initialized = Vn_Init(&config);
    if (initialized != VN_OK) {
        result.error = "Vn_Init failed";
        return result;
    }
    EngineGuard guard(true);
    harken::Engine* engine = harken::detail::ActiveEngine();
    if (engine == nullptr) {
        result.error = "active engine missing";
        return result;
    }
    if (engine->nullBackend.BlockFrames() != kBlockFrames ||
        engine->nullBackend.Channels() != 2) {
        result.error = "null backend geometry mismatch";
        return result;
    }

    const std::vector<float> source =
        MakeInterleavedSource(scenario.sourceRate, scenario.sourceChannels);
    VnPCMDesc pcm{};
    pcm.structSize = sizeof(pcm);
    pcm.sampleRate = scenario.sourceRate;
    pcm.channels = scenario.sourceChannels;
    pcm.layout = VN_PCM_LAYOUT_INTERLEAVED;
    pcm.policy = VN_PCM_POLICY_REFERENCE;
    pcm.frames = scenario.sourceRate;
    pcm.samplesInterleaved = source.data();
    constexpr VnSoundId kSoundId = harkentest::ObjectId(0x4D313152454E4445ull);
    if (Vn_RegisterPCM(kSoundId, &pcm) != VN_OK) {
        result.error = "Vn_RegisterPCM failed";
        return result;
    }

    for (std::uint32_t voice = 0; voice < scenario.renderCapacity; ++voice) {
        VnPlayDesc play{};
        play.structSize = sizeof(play);
        play.soundId = kSoundId;
        play.busId = VN_BUS_MASTER;
        play.volumeDb = -60.0f;
        play.pitchRatio = static_cast<float>(
            PitchRatio(scenario.pitchSemitones));
        play.loopCount = 0;
        VnVoiceHandle handle = 0;
        if (harkentest::PlayRaw(&play, &handle) != VN_OK || handle == 0) {
            result.error = "raw voice start failed";
            return result;
        }
    }
    if (Vn_Update() != VN_OK) {
        result.error = "Vn_Update failed";
        return result;
    }

    if (warmupBlocks == kCanonicalWarmupBlocksPerRepetition) {
        result.warmupBlocksUsed = 128;
        std::vector<double> warmupTimings;
        warmupTimings.reserve(128);
        for (std::uint32_t block = 0; block < 64; ++block) {
            const auto started = std::chrono::steady_clock::now();
            engine->nullBackend.Pump(1);
            const auto finished = std::chrono::steady_clock::now();
            warmupTimings.push_back(
                std::chrono::duration<double, std::micro>(
                    finished - started).count());
        }
        for (std::uint32_t block = 0; block < 64; ++block) {
            const auto started = std::chrono::steady_clock::now();
            engine->nullBackend.Pump(1);
            const auto finished = std::chrono::steady_clock::now();
            warmupTimings.push_back(
                std::chrono::duration<double, std::micro>(
                    finished - started).count());
        }
        if (!WarmupWindowsStable(warmupTimings, 96, 112)) {
            result.error = "unstable warmup";
            return result;
        }
    } else {
        result.warmupBlocksUsed = warmupBlocks;
        engine->nullBackend.Pump(warmupBlocks);
    }
    result.callbackUs.reserve(measuredBlocks);
    for (std::uint32_t block = 0; block < measuredBlocks; ++block) {
        const auto started = std::chrono::steady_clock::now();
        engine->nullBackend.Pump(1);
        const auto finished = std::chrono::steady_clock::now();
        result.callbackUs.push_back(
            std::chrono::duration<double, std::micro>(finished - started)
                .count());

        HashAndInspect(
            engine->nullBackend.LastBlock(),
            static_cast<std::size_t>(kBlockFrames) * 2, &result.hash,
            &result.peak, &result.nonfiniteSampleCount);
    }

    result.snapshot.structSize = sizeof(result.snapshot);
    if (Vn_CaptureSnapshot(&result.snapshot) != VN_OK) {
        result.error = "Vn_CaptureSnapshot failed";
        return result;
    }
    result.setupOk = true;
    return result;
}

CaseResult MeasureRenderCase(const RenderCase& scenario,
                             std::uint32_t warmupBlocks,
                             std::uint32_t measuredBlocks,
                             std::uint32_t repetitions) {
    CaseResult result;
    std::vector<double> allTimings;
    allTimings.reserve(static_cast<std::size_t>(measuredBlocks) *
                       repetitions);
    bool allRunsPassed = true;
    bool haveReferenceHash = false;
    const double nominalPeriodUs =
        1000000.0 * static_cast<double>(kBlockFrames) /
        static_cast<double>(scenario.mixRate);
    result.repetitionTimings.reserve(repetitions);
    for (std::uint32_t repetition = 0; repetition < repetitions;
         ++repetition) {
        RunResult run =
            RunRenderOnce(scenario, warmupBlocks, measuredBlocks);
        if (!run.setupOk) {
            allRunsPassed = false;
            result.error = run.error;
            continue;
        }
        const bool runGate =
            run.nonfiniteSampleCount == 0 &&
            run.peak > kOutputFloor &&
            SnapshotPassesRenderGate(run.snapshot,
                                     scenario.renderCapacity);
        allRunsPassed = allRunsPassed && runGate;
        if (!runGate && result.error == nullptr) {
            result.error = "render correctness gate failed";
        }
        if (!haveReferenceHash) {
            result.hash = run.hash;
            haveReferenceHash = true;
        } else if (run.hash != result.hash) {
            result.deterministic = false;
            allRunsPassed = false;
            if (result.error == nullptr) {
                result.error = "repetition hash drift";
            }
        }
        allTimings.insert(allTimings.end(), run.callbackUs.begin(),
                          run.callbackUs.end());
        TimingSummary repetitionTiming =
            SummarizeTiming(run.callbackUs, nominalPeriodUs);
        repetitionTiming.warmupBlocks = run.warmupBlocksUsed;
        result.repetitionTimings.push_back(repetitionTiming);
        result.peak = std::max(result.peak, run.peak);
        result.nonfiniteSampleCount += run.nonfiniteSampleCount;
        result.snapshot = run.snapshot;
    }
    result.timing =
        SummarizeTiming(std::move(allTimings), nominalPeriodUs);
    UseWorstRepetitionP99(&result.timing, result.repetitionTimings);
    result.integrityPassed =
        allRunsPassed && haveReferenceHash && result.deterministic;
    return result;
}

VirtualRunResult RunVirtualOnce(std::uint32_t virtualCapacity,
                                std::uint32_t warmupBlocks,
                                std::uint32_t measuredBlocks) {
    constexpr std::uint32_t kRenderCapacity = 64;
    constexpr std::uint32_t kRate = 48000;
    VirtualRunResult result;
    VnConfig config{};
    config.structSize = sizeof(config);
    config.sampleRate = kRate;
    config.blockFrames = kBlockFrames;
    config.maxVoices = kRenderCapacity;
    config.maxParkedVoices = virtualCapacity;
    config.arenaBytes = kArenaBytes;
    config.deviceMode = VN_DEVICE_NULL;
    config.commandQueueDepth = 2048;
    config.notificationQueueDepth = 2048;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -70.0f;
    config.mixChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    config.deviceChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    config.renderWorkerCount = 0;

    if (Vn_Init(&config) != VN_OK) {
        result.error = "Vn_Init failed";
        return result;
    }
    EngineGuard guard(true);
    harken::Engine* engine = harken::detail::ActiveEngine();
    if (engine == nullptr) {
        result.error = "active engine missing";
        return result;
    }

    const std::vector<float> source = MakeInterleavedSource(kRate, 1);
    VnPCMDesc pcm{};
    pcm.structSize = sizeof(pcm);
    pcm.sampleRate = kRate;
    pcm.channels = 1;
    pcm.layout = VN_PCM_LAYOUT_INTERLEAVED;
    pcm.policy = VN_PCM_POLICY_REFERENCE;
    pcm.frames = kRate;
    pcm.samplesInterleaved = source.data();
    constexpr VnSoundId kSoundId = harkentest::ObjectId(0x4D31315649525455ull);
    if (Vn_RegisterPCM(kSoundId, &pcm) != VN_OK) {
        result.error = "Vn_RegisterPCM failed";
        return result;
    }

    std::vector<VnVoiceHandle> virtualHandles;
    virtualHandles.reserve(virtualCapacity);
    std::uint32_t remaining = virtualCapacity;
    while (remaining != 0) {
        const std::uint32_t batch =
            std::min(kRenderCapacity, remaining);
        for (std::uint32_t index = 0; index < batch; ++index) {
            VnPlayDesc play{};
            play.structSize = sizeof(play);
            play.soundId = kSoundId;
            play.busId = VN_BUS_MASTER;
            play.volumeDb = -100.0f;
            play.pitchRatio = 1.0f;
            play.loopCount = 0;
            VnVoiceHandle handle = 0;
            if (harkentest::PlayRaw(&play, &handle) != VN_OK || handle == 0) {
                result.error = "virtual raw voice start failed";
                return result;
            }
            virtualHandles.push_back(handle);
        }
        engine->nullBackend.Pump(1);
        if (Vn_Update() != VN_OK) {
            result.error = "virtual setup Vn_Update failed";
            return result;
        }
        engine->nullBackend.Pump(1);
        remaining -= batch;
    }

    std::vector<VnVoiceHandle> renderHandles;
    renderHandles.reserve(kRenderCapacity);
    for (std::uint32_t index = 0; index < kRenderCapacity; ++index) {
        VnPlayDesc play{};
        play.structSize = sizeof(play);
        play.soundId = kSoundId;
        play.busId = VN_BUS_MASTER;
        play.volumeDb = -60.0f;
        play.pitchRatio = 1.0f;
        play.loopCount = 0;
        VnVoiceHandle handle = 0;
        if (harkentest::PlayRaw(&play, &handle) != VN_OK || handle == 0) {
            result.error = "render raw voice start failed";
            return result;
        }
        renderHandles.push_back(handle);
    }
    if (Vn_Update() != VN_OK) {
        result.error = "render setup Vn_Update failed";
        return result;
    }

    std::vector<double> warmupTimings;
    const std::uint32_t initialWarmup = warmupBlocks;
    result.warmupBlocksUsed = initialWarmup;
    warmupTimings.reserve(
        warmupBlocks == kCanonicalWarmupBlocksPerRepetition ? 128
                                                            : warmupBlocks);
    const auto runWarmupBlock = [&]() {
        const auto started = std::chrono::steady_clock::now();
        engine->nullBackend.Pump(1);
        const auto finished = std::chrono::steady_clock::now();
        warmupTimings.push_back(
            std::chrono::duration<double, std::micro>(finished - started)
                .count());
        return Vn_Update();
    };
    for (std::uint32_t block = 0; block < initialWarmup; ++block) {
        if (runWarmupBlock() != VN_OK) {
            result.error = "warmup Vn_Update failed";
            return result;
        }
    }
    if (warmupBlocks == kCanonicalWarmupBlocksPerRepetition) {
        result.warmupBlocksUsed = 128;
        for (std::uint32_t block = 0; block < 64; ++block) {
            if (runWarmupBlock() != VN_OK) {
                result.error = "extended warmup Vn_Update failed";
                return result;
            }
        }
        if (!WarmupWindowsStable(warmupTimings, 96, 112)) {
            result.error = "unstable warmup";
            return result;
        }
    }

    result.callbackUs.reserve(measuredBlocks);
    result.updateUs.reserve(measuredBlocks);
    for (std::uint32_t block = 0; block < measuredBlocks; ++block) {
        const auto callbackStarted = std::chrono::steady_clock::now();
        engine->nullBackend.Pump(1);
        const auto callbackFinished = std::chrono::steady_clock::now();
        result.callbackUs.push_back(
            std::chrono::duration<double, std::micro>(
                callbackFinished - callbackStarted)
                .count());
        HashAndInspect(
            engine->nullBackend.LastBlock(),
            static_cast<std::size_t>(kBlockFrames) * 2, &result.hash,
            &result.peak, &result.nonfiniteSampleCount);

        const auto updateStarted = std::chrono::steady_clock::now();
        const VnResult updated = Vn_Update();
        const auto updateFinished = std::chrono::steady_clock::now();
        result.updateUs.push_back(
            std::chrono::duration<double, std::micro>(
                updateFinished - updateStarted)
                .count());
        if (updated != VN_OK) {
            result.error = "measured Vn_Update failed";
            return result;
        }
    }

    for (const VnVoiceHandle handle : renderHandles) {
        const harken::VoiceLedger::Resolved resolved =
            engine->ledger.Classify(handle);
        if (resolved.kind == harken::VoiceLedger::HandleKind::Live) {
            ++result.classifiedLive;
        } else {
            ++result.classifiedOther;
        }
    }
    for (const VnVoiceHandle handle : virtualHandles) {
        const harken::VoiceLedger::Resolved resolved =
            engine->ledger.Classify(handle);
        if (resolved.kind == harken::VoiceLedger::HandleKind::Parked) {
            ++result.classifiedParked;
        } else {
            ++result.classifiedOther;
        }
    }

    result.snapshot.structSize = sizeof(result.snapshot);
    if (Vn_CaptureSnapshot(&result.snapshot) != VN_OK) {
        result.error = "Vn_CaptureSnapshot failed";
        return result;
    }
    result.setupOk = true;
    return result;
}

VirtualCaseResult MeasureVirtualCase(std::uint32_t virtualCapacity,
                                     std::uint32_t warmupBlocks,
                                     std::uint32_t measuredBlocks,
                                     std::uint32_t repetitions) {
    constexpr std::uint32_t kRenderCapacity = 64;
    constexpr double kDeadlineUs =
        1000000.0 * static_cast<double>(kBlockFrames) / 48000.0;
    VirtualCaseResult result;
    std::vector<double> callbackTimings;
    std::vector<double> updateTimings;
    callbackTimings.reserve(
        static_cast<std::size_t>(measuredBlocks) * repetitions);
    updateTimings.reserve(
        static_cast<std::size_t>(measuredBlocks) * repetitions);
    bool allRunsPassed = true;
    bool haveReferenceHash = false;
    result.callbackRepetitions.reserve(repetitions);
    result.updateRepetitions.reserve(repetitions);
    for (std::uint32_t repetition = 0; repetition < repetitions;
         ++repetition) {
        VirtualRunResult run = RunVirtualOnce(
            virtualCapacity, warmupBlocks, measuredBlocks);
        if (!run.setupOk) {
            allRunsPassed = false;
            result.error = run.error;
            continue;
        }
        const bool snapshotGate =
            run.snapshot.activeVoiceCount == kRenderCapacity &&
            run.snapshot.parkedVoiceCount == virtualCapacity &&
            run.snapshot.voiceHighWater == kRenderCapacity &&
            run.snapshot.parkedHighWater == virtualCapacity &&
            run.snapshot.voiceParkedCount == virtualCapacity &&
            run.snapshot.pendingStartCount == 0 &&
            run.snapshot.noVoiceCount == 0 &&
            run.snapshot.backpressureCount == 0 &&
            run.snapshot.notificationDropCount == 0 &&
            run.snapshot.logicNotificationDropCount == 0 &&
            run.snapshot.arenaOverflowCount == 0 &&
            run.snapshot.voiceStarvedCount == 0 &&
            run.snapshot.returnRefusedCount == 0 &&
            run.classifiedLive == kRenderCapacity &&
            run.classifiedParked == virtualCapacity &&
            run.classifiedOther == 0 &&
            run.nonfiniteSampleCount == 0 &&
            run.peak > kOutputFloor;
        allRunsPassed = allRunsPassed && snapshotGate;
        if (!snapshotGate && result.error == nullptr) {
            result.error = "virtual/logical correctness gate failed";
        }
        if (!haveReferenceHash) {
            result.hash = run.hash;
            haveReferenceHash = true;
        } else if (run.hash != result.hash) {
            result.deterministic = false;
            allRunsPassed = false;
            if (result.error == nullptr) {
                result.error = "virtual repetition hash drift";
            }
        }
        callbackTimings.insert(callbackTimings.end(),
                               run.callbackUs.begin(),
                               run.callbackUs.end());
        updateTimings.insert(updateTimings.end(), run.updateUs.begin(),
                             run.updateUs.end());
        TimingSummary callbackRepetition =
            SummarizeTiming(run.callbackUs, kDeadlineUs);
        callbackRepetition.warmupBlocks = run.warmupBlocksUsed;
        result.callbackRepetitions.push_back(callbackRepetition);
        TimingSummary updateRepetition =
            SummarizeTiming(run.updateUs, kDeadlineUs);
        updateRepetition.warmupBlocks = run.warmupBlocksUsed;
        result.updateRepetitions.push_back(updateRepetition);
        result.peak = std::max(result.peak, run.peak);
        result.nonfiniteSampleCount += run.nonfiniteSampleCount;
        result.classifiedLive = run.classifiedLive;
        result.classifiedParked = run.classifiedParked;
        result.classifiedOther = run.classifiedOther;
        result.snapshot = run.snapshot;
    }
    result.callbackTiming =
        SummarizeTiming(std::move(callbackTimings), kDeadlineUs);
    result.updateTiming =
        SummarizeTiming(std::move(updateTimings), kDeadlineUs);
    UseWorstRepetitionP99(&result.callbackTiming,
                          result.callbackRepetitions);
    UseWorstRepetitionP99(&result.updateTiming,
                          result.updateRepetitions);
    result.integrityPassed =
        allRunsPassed && haveReferenceHash && result.deterministic;
    return result;
}

StreamingRunResult RunStreamingOnce(
    const StreamingCase& scenario, std::uint32_t warmupBlocks,
    std::uint32_t measuredBlocks) {
    constexpr std::uint32_t kRate = 48000;
    constexpr std::uint32_t kRingFrames = 16384;
    constexpr VnSoundId kStreamSoundId = harkentest::ObjectId(0x4D31315354524541ull);
    constexpr VnSoundId kPcmSoundId = harkentest::ObjectId(0x4D31315350434D31ull);
    StreamingRunResult result;
    const std::string streamPath =
        WriteBenchmarkStream(kStreamSoundId);
    if (streamPath.empty()) {
        result.error = "stream fixture write failed";
        return result;
    }

    VnConfig config{};
    config.structSize = sizeof(config);
    config.sampleRate = kRate;
    config.blockFrames = kBlockFrames;
    config.maxVoices = scenario.renderCapacity;
    config.maxParkedVoices = scenario.virtualCapacity;
    config.arenaBytes = kArenaBytes;
    config.deviceMode = VN_DEVICE_NULL;
    config.commandQueueDepth = 2048;
    config.notificationQueueDepth = 2048;
    config.streamRingFrames = kRingFrames;
    config.maxStreams = 16;
    config.prebufferMs = 1;
    config.mixChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    config.deviceChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    config.renderWorkerCount = 0;
    if (Vn_Init(&config) != VN_OK) {
        result.error = "Vn_Init failed";
        return result;
    }
    EngineGuard guard(true);
    harken::Engine* engine = harken::detail::ActiveEngine();
    if (engine == nullptr) {
        result.error = "active engine missing";
        return result;
    }
    if (Vn_RegisterStreamFile(kStreamSoundId,
                              streamPath.c_str()) != VN_OK) {
        result.error = "Vn_RegisterStreamFile failed";
        return result;
    }

    const std::uint32_t pcmVoices = scenario.pcmVoices;
    std::vector<float> pcmSource;
    if (pcmVoices != 0) {
        pcmSource = MakeInterleavedSource(kRate, 1);
        VnPCMDesc pcm{};
        pcm.structSize = sizeof(pcm);
        pcm.sampleRate = kRate;
        pcm.channels = 1;
        pcm.layout = VN_PCM_LAYOUT_INTERLEAVED;
        pcm.policy = VN_PCM_POLICY_REFERENCE;
        pcm.frames = kRate;
        pcm.samplesInterleaved = pcmSource.data();
        if (Vn_RegisterPCM(kPcmSoundId, &pcm) != VN_OK) {
            result.error = "Vn_RegisterPCM failed";
            return result;
        }
    }

    for (std::uint32_t voice = 0; voice < scenario.streamVoices;
         ++voice) {
        VnPlayDesc play{};
        play.structSize = sizeof(play);
        play.soundId = kStreamSoundId;
        play.busId = VN_BUS_MASTER;
        play.volumeDb = -60.0f;
        play.pitchRatio = 1.0f;
        play.loopCount = 0;
        VnVoiceHandle handle = 0;
        if (harkentest::PlayRaw(&play, &handle) != VN_OK || handle == 0) {
            result.error = "stream raw voice start failed";
            return result;
        }
    }
    for (std::uint32_t voice = 0; voice < pcmVoices; ++voice) {
        VnPlayDesc play{};
        play.structSize = sizeof(play);
        play.soundId = kPcmSoundId;
        play.busId = VN_BUS_MASTER;
        play.volumeDb = -60.0f;
        play.pitchRatio = 1.0f;
        play.loopCount = 0;
        VnVoiceHandle handle = 0;
        if (harkentest::PlayRaw(&play, &handle) != VN_OK || handle == 0) {
            result.error = "PCM raw voice start failed";
            return result;
        }
    }
    if (Vn_Update() != VN_OK) {
        result.error = "initial Vn_Update failed";
        return result;
    }

    std::vector<double> warmupTimings;
    result.warmupBlocksUsed = warmupBlocks;
    warmupTimings.reserve(
        warmupBlocks == kCanonicalWarmupBlocksPerRepetition ? 128
                                                            : warmupBlocks);
    const auto runWarmupBlock = [&]() {
        engine->streams.SyncServe();
        const auto started = std::chrono::steady_clock::now();
        engine->nullBackend.Pump(1);
        const auto finished = std::chrono::steady_clock::now();
        warmupTimings.push_back(
            std::chrono::duration<double, std::micro>(finished - started)
                .count());
        return Vn_Update();
    };
    for (std::uint32_t block = 0; block < warmupBlocks; ++block) {
        if (runWarmupBlock() != VN_OK) {
            result.error = "warmup Vn_Update failed";
            return result;
        }
    }
    if (warmupBlocks == kCanonicalWarmupBlocksPerRepetition) {
        result.warmupBlocksUsed = 128;
        for (std::uint32_t block = 0; block < 64; ++block) {
            if (runWarmupBlock() != VN_OK) {
                result.error = "extended warmup Vn_Update failed";
                return result;
            }
        }
        if (!WarmupWindowsStable(warmupTimings, 96, 112)) {
            result.error = "unstable warmup";
            return result;
        }
    }

    result.callbackUs.reserve(measuredBlocks);
    result.serviceUs.reserve(measuredBlocks);
    result.updateUs.reserve(measuredBlocks);
    for (std::uint32_t block = 0; block < measuredBlocks; ++block) {
        const auto serviceStarted = std::chrono::steady_clock::now();
        engine->streams.SyncServe();
        const auto serviceFinished = std::chrono::steady_clock::now();
        result.serviceUs.push_back(
            std::chrono::duration<double, std::micro>(
                serviceFinished - serviceStarted)
                .count());

        const auto callbackStarted = std::chrono::steady_clock::now();
        engine->nullBackend.Pump(1);
        const auto callbackFinished = std::chrono::steady_clock::now();
        result.callbackUs.push_back(
            std::chrono::duration<double, std::micro>(
                callbackFinished - callbackStarted)
                .count());
        HashAndInspect(
            engine->nullBackend.LastBlock(),
            static_cast<std::size_t>(kBlockFrames) * 2, &result.hash,
            &result.peak, &result.nonfiniteSampleCount);
        const auto updateStarted = std::chrono::steady_clock::now();
        const VnResult updated = Vn_Update();
        const auto updateFinished = std::chrono::steady_clock::now();
        result.updateUs.push_back(
            std::chrono::duration<double, std::micro>(
                updateFinished - updateStarted)
                .count());
        if (updated != VN_OK) {
            result.error = "measured Vn_Update failed";
            return result;
        }
    }

    result.snapshot.structSize = sizeof(result.snapshot);
    if (Vn_CaptureSnapshot(&result.snapshot) != VN_OK) {
        result.error = "Vn_CaptureSnapshot failed";
        return result;
    }
    result.setupOk = true;
    return result;
}

StreamingCaseResult MeasureStreamingCase(
    const StreamingCase& scenario, std::uint32_t warmupBlocks,
    std::uint32_t measuredBlocks, std::uint32_t repetitions) {
    constexpr double kDeadlineUs =
        1000000.0 * static_cast<double>(kBlockFrames) / 48000.0;
    StreamingCaseResult result;
    std::vector<double> callbackTimings;
    std::vector<double> serviceTimings;
    std::vector<double> updateTimings;
    callbackTimings.reserve(
        static_cast<std::size_t>(measuredBlocks) * repetitions);
    serviceTimings.reserve(
        static_cast<std::size_t>(measuredBlocks) * repetitions);
    updateTimings.reserve(
        static_cast<std::size_t>(measuredBlocks) * repetitions);
    bool allRunsPassed = true;
    bool haveReferenceHash = false;
    result.callbackRepetitions.reserve(repetitions);
    result.serviceRepetitions.reserve(repetitions);
    result.updateRepetitions.reserve(repetitions);
    for (std::uint32_t repetition = 0; repetition < repetitions;
         ++repetition) {
        StreamingRunResult run = RunStreamingOnce(
            scenario, warmupBlocks, measuredBlocks);
        if (!run.setupOk) {
            allRunsPassed = false;
            result.error = run.error;
            continue;
        }
        const bool runGate =
            run.snapshot.activeVoiceCount ==
                scenario.streamVoices + scenario.pcmVoices &&
            run.snapshot.parkedVoiceCount == 0 &&
            run.snapshot.voiceHighWater ==
                scenario.streamVoices + scenario.pcmVoices &&
            run.snapshot.streamHighWater ==
                scenario.streamVoices &&
            run.snapshot.streamRingHighWaterFrames > 0 &&
            run.snapshot.streamRingHighWaterFrames <= 16384 &&
            run.snapshot.voiceCompletedCount == 0 &&
            run.snapshot.noVoiceCount == 0 &&
            run.snapshot.backpressureCount == 0 &&
            run.snapshot.notificationDropCount == 0 &&
            run.snapshot.logicNotificationDropCount == 0 &&
            run.snapshot.arenaOverflowCount == 0 &&
            run.snapshot.voiceStarvedCount == 0 &&
            run.snapshot.returnRefusedCount == 0 &&
            run.snapshot.pendingStartCount == 0 &&
            run.nonfiniteSampleCount == 0 &&
            run.peak > kOutputFloor;
        allRunsPassed = allRunsPassed && runGate;
        if (!runGate && result.error == nullptr) {
            result.error = "streaming correctness gate failed";
        }
        if (!haveReferenceHash) {
            result.hash = run.hash;
            haveReferenceHash = true;
        } else if (run.hash != result.hash) {
            result.deterministic = false;
            allRunsPassed = false;
            if (result.error == nullptr) {
                result.error = "streaming repetition hash drift";
            }
        }
        callbackTimings.insert(callbackTimings.end(),
                               run.callbackUs.begin(),
                               run.callbackUs.end());
        serviceTimings.insert(serviceTimings.end(),
                              run.serviceUs.begin(),
                              run.serviceUs.end());
        updateTimings.insert(updateTimings.end(), run.updateUs.begin(),
                             run.updateUs.end());
        TimingSummary callbackRepetition =
            SummarizeTiming(run.callbackUs, kDeadlineUs);
        callbackRepetition.warmupBlocks = run.warmupBlocksUsed;
        result.callbackRepetitions.push_back(callbackRepetition);
        TimingSummary serviceRepetition =
            SummarizeTiming(run.serviceUs, kDeadlineUs);
        serviceRepetition.warmupBlocks = run.warmupBlocksUsed;
        result.serviceRepetitions.push_back(serviceRepetition);
        TimingSummary updateRepetition =
            SummarizeTiming(run.updateUs, kDeadlineUs);
        updateRepetition.warmupBlocks = run.warmupBlocksUsed;
        result.updateRepetitions.push_back(updateRepetition);
        result.peak = std::max(result.peak, run.peak);
        result.nonfiniteSampleCount += run.nonfiniteSampleCount;
        result.snapshot = run.snapshot;
    }
    result.callbackTiming =
        SummarizeTiming(std::move(callbackTimings), kDeadlineUs);
    result.serviceTiming =
        SummarizeTiming(std::move(serviceTimings), kDeadlineUs);
    result.updateTiming =
        SummarizeTiming(std::move(updateTimings), kDeadlineUs);
    UseWorstRepetitionP99(&result.callbackTiming,
                          result.callbackRepetitions);
    UseWorstRepetitionP99(&result.serviceTiming,
                          result.serviceRepetitions);
    UseWorstRepetitionP99(&result.updateTiming,
                          result.updateRepetitions);
    result.integrityPassed =
        allRunsPassed && haveReferenceHash && result.deterministic;
    return result;
}

struct ProcessMemory {
    std::uint64_t privateBytes = 0;
    std::uint64_t workingSetBytes = 0;
    bool available = false;
};

ProcessMemory ReadProcessMemory() {
    ProcessMemory result;
#if defined(_WIN32)
    PROCESS_MEMORY_COUNTERS_EX counters{};
    counters.cb = sizeof(counters);
    if (GetProcessMemoryInfo(
            GetCurrentProcess(),
            reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&counters),
            sizeof(counters)) != 0) {
        result.privateBytes = counters.PrivateUsage;
        result.workingSetBytes = counters.WorkingSetSize;
        result.available = true;
    }
#endif
    return result;
}

struct ResourceEntry {
    std::string id;
    const char* ladder = "scale";
    std::uint32_t renderCapacity = 64;
    std::uint32_t virtualCapacity = 64;
};

std::vector<ResourceEntry> BuildResourceCatalog() {
    constexpr std::array<std::uint32_t, 4> kTiers = {64, 128, 256, 512};
    std::vector<ResourceEntry> result;
    result.reserve(12);
    for (const std::uint32_t tier : kTiers) {
        result.push_back({"resource.render.r" + std::to_string(tier) +
                              ".v64",
                          "render", tier, 64});
    }
    for (const std::uint32_t tier : kTiers) {
        result.push_back({"resource.virtual.r64.v" +
                              std::to_string(tier),
                          "virtual", 64, tier});
    }
    for (const std::uint32_t tier : kTiers) {
        result.push_back({"resource.scale.r" + std::to_string(tier) +
                              ".v" + std::to_string(tier),
                          "scale", tier, tier});
    }
    return result;
}

int RunResourceScenario(const ResourceEntry& scenario) {
    constexpr std::uint32_t kRate = 48000;
    constexpr VnSoundId kSoundId = harkentest::ObjectId(0x4D31315245534F55ull);
    std::vector<float> source = MakeInterleavedSource(kRate, 2);
    std::vector<VnVoiceHandle> handles(
        static_cast<std::size_t>(scenario.renderCapacity) +
        scenario.virtualCapacity);
    const std::uint64_t fixtureBytes =
        static_cast<std::uint64_t>(source.capacity()) * sizeof(float) +
        static_cast<std::uint64_t>(handles.capacity()) *
            sizeof(VnVoiceHandle);
    const ProcessMemory baseline = ReadProcessMemory();

    VnConfig config{};
    config.structSize = sizeof(config);
    config.sampleRate = kRate;
    config.blockFrames = kBlockFrames;
    config.maxVoices = scenario.renderCapacity;
    config.maxParkedVoices = scenario.virtualCapacity;
    config.arenaBytes = kArenaBytes;
    config.deviceMode = VN_DEVICE_NULL;
    config.commandQueueDepth = 2048;
    config.notificationQueueDepth = 2048;
    config.streamRingFrames = 16384;
    config.maxStreams = 16;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -70.0f;
    config.mixChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    config.deviceChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    config.renderWorkerCount = 0;
    if (Vn_Init(&config) != VN_OK) {
        std::fprintf(stderr, "resource Vn_Init failed\n");
        return 1;
    }
    EngineGuard guard(true);
    harken::Engine* engine = harken::detail::ActiveEngine();
    if (engine == nullptr) {
        return 1;
    }
    const ProcessMemory afterInit = ReadProcessMemory();

    VnPCMDesc pcm{};
    pcm.structSize = sizeof(pcm);
    pcm.sampleRate = kRate;
    pcm.channels = 2;
    pcm.layout = VN_PCM_LAYOUT_INTERLEAVED;
    pcm.policy = VN_PCM_POLICY_REFERENCE;
    pcm.frames = kRate;
    pcm.samplesInterleaved = source.data();
    if (Vn_RegisterPCM(kSoundId, &pcm) != VN_OK) {
        return 1;
    }

    std::size_t handleIndex = 0;
    std::uint32_t remaining = scenario.virtualCapacity;
    while (remaining != 0) {
        const std::uint32_t batch =
            std::min(scenario.renderCapacity, remaining);
        for (std::uint32_t index = 0; index < batch; ++index) {
            VnPlayDesc play{};
            play.structSize = sizeof(play);
            play.soundId = kSoundId;
            play.busId = VN_BUS_MASTER;
            play.volumeDb = -100.0f;
            play.pitchRatio = 1.0f;
            play.loopCount = 0;
            if (harkentest::PlayRaw(&play, &handles[handleIndex++]) != VN_OK) {
                return 1;
            }
        }
        engine->nullBackend.Pump(1);
        if (Vn_Update() != VN_OK) {
            return 1;
        }
        engine->nullBackend.Pump(1);
        remaining -= batch;
    }
    for (std::uint32_t index = 0;
         index < scenario.renderCapacity; ++index) {
        VnPlayDesc play{};
        play.structSize = sizeof(play);
        play.soundId = kSoundId;
        play.busId = VN_BUS_MASTER;
        play.volumeDb = -60.0f;
        play.pitchRatio = 1.0f;
        play.loopCount = 0;
        if (harkentest::PlayRaw(&play, &handles[handleIndex++]) != VN_OK) {
            return 1;
        }
    }
    const auto pendingCommands = engine->commands.PublishedHead() -
        engine->commands.ConsumedCount();
    const auto commandBudget = engine->config.maxControlCommandsPerQuantum;
    if (commandBudget == 0) {
        std::fprintf(stderr, "resource command budget is zero\n");
        return 1;
    }
    const auto populateBlocks = std::max<std::uint64_t>(1,
        pendingCommands / commandBudget + (pendingCommands % commandBudget != 0));
    const auto firstPopulateBlock = engine->mixer.BlocksRendered();
    for (std::uint64_t block = 0; block < populateBlocks; ++block) {
        engine->nullBackend.Pump(1);
    }
    const ProcessMemory populated = ReadProcessMemory();
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(snapshot);
    const VnResult snapshotResult = Vn_CaptureSnapshot(&snapshot);
    const bool snapshotOk = snapshotResult == VN_OK &&
        snapshot.blockIndex == firstPopulateBlock + populateBlocks - 1u &&
        engine->commands.PublishedHead() == engine->commands.ConsumedCount() &&
        snapshot.activeVoiceCount == scenario.renderCapacity &&
        snapshot.parkedVoiceCount == scenario.virtualCapacity &&
        snapshot.arenaOverflowCount == 0;
    if (!snapshotOk) {
        const auto commandHead = engine->commands.PublishedHead();
        const auto commandTail = engine->commands.ConsumedCount();
        std::fprintf(stderr,
            "resource snapshot failed: scenario=%s result=%d block=%llu "
            "active=%u parked=%u overflow=%llu commands_pending=%llu "
            "commands_head=%llu commands_tail=%llu\n",
            scenario.id.c_str(), static_cast<int>(snapshotResult),
            static_cast<unsigned long long>(snapshot.blockIndex),
            static_cast<unsigned>(snapshot.activeVoiceCount),
            static_cast<unsigned>(snapshot.parkedVoiceCount),
            static_cast<unsigned long long>(snapshot.arenaOverflowCount),
            static_cast<unsigned long long>(commandHead - commandTail),
            static_cast<unsigned long long>(commandHead),
            static_cast<unsigned long long>(commandTail));
    }
    const bool available = baseline.available && afterInit.available &&
        populated.available;
    std::printf(
        "{\"benchmark\":\"%s\",\"schema\":\"%s\","
        "\"record\":\"resource\",\"scenario_id\":",
        kBenchmarkName, kSchema);
    PrintJsonString(scenario.id.c_str());
    std::printf(
        ",\"ladder\":\"%s\",\"render_capacity\":%u,"
        "\"virtual_capacity\":%u,\"logical_capacity\":%u,"
        "\"fixture_bytes\":%llu,\"memory_available\":%s,"
        "\"baseline_private_bytes\":%llu,"
        "\"after_init_private_bytes\":%llu,"
        "\"populated_private_bytes\":%llu,"
        "\"init_private_delta_bytes\":%lld,"
        "\"populated_private_delta_bytes\":%lld,"
        "\"baseline_working_set_bytes\":%llu,"
        "\"after_init_working_set_bytes\":%llu,"
        "\"populated_working_set_bytes\":%llu,"
        "\"init_working_set_delta_bytes\":%lld,"
        "\"populated_working_set_delta_bytes\":%lld,"
        "\"arena_bytes\":%u,\"command_queue_depth\":2048,"
        "\"notification_queue_depth\":2048,"
        "\"max_streams\":16,\"stream_ring_frames\":16384,"
        "\"max_buses\":%u,\"topology_plan_pool_capacity\":%u,"
        "\"topology_plan_pool_bytes\":%llu,"
        "\"resolved_render_pool_capacity\":%u,"
        "\"resolved_ledger_capacity\":%u,"
        "\"resolved_park_record_capacity\":%llu,"
        "\"fx_slots_per_voice\":%u,\"fx_slots_per_bus\":%u,"
        "\"delay_effect_pool_capacity\":%u,"
        "\"tone_effect_pool_capacity\":%u,"
        "\"gain_effect_pool_capacity\":%u,"
        "\"limiter_effect_pool_capacity\":%u,"
        "\"compressor_effect_pool_capacity\":%u,"
        "\"status\":\"%s\",\"support_claim\":false}\n",
        scenario.ladder, scenario.renderCapacity,
        scenario.virtualCapacity,
        scenario.renderCapacity + scenario.virtualCapacity,
        static_cast<unsigned long long>(fixtureBytes),
        available ? "true" : "false",
        static_cast<unsigned long long>(baseline.privateBytes),
        static_cast<unsigned long long>(afterInit.privateBytes),
        static_cast<unsigned long long>(populated.privateBytes),
        static_cast<long long>(afterInit.privateBytes) -
            static_cast<long long>(baseline.privateBytes),
        static_cast<long long>(populated.privateBytes) -
            static_cast<long long>(baseline.privateBytes),
        static_cast<unsigned long long>(baseline.workingSetBytes),
        static_cast<unsigned long long>(afterInit.workingSetBytes),
        static_cast<unsigned long long>(populated.workingSetBytes),
        static_cast<long long>(afterInit.workingSetBytes) -
            static_cast<long long>(baseline.workingSetBytes),
        static_cast<long long>(populated.workingSetBytes) -
            static_cast<long long>(baseline.workingSetBytes),
        kArenaBytes,
        engine->config.maxBuses,
        engine->config.topologyPlanPoolCapacity,
        static_cast<unsigned long long>(
            engine->config.topologyPlanPoolBytes),
        engine->voices.Capacity(), engine->ledger.Capacity(),
        static_cast<unsigned long long>(engine->parkRecords.size()),
        engine->config.fxSlotsPerVoice,
        engine->config.fxSlotsPerBus,
        engine->config.delayEffectPoolCapacity,
        engine->config.toneEffectPoolCapacity,
        engine->config.gainEffectPoolCapacity,
        engine->config.limiterEffectPoolCapacity,
        engine->config.compressorEffectPoolCapacity,
        available && snapshotOk ? "complete" : "failed");
    return available && snapshotOk ? 0 : 1;
}

enum class WorkloadKind { Render, Virtual, Streaming };

struct CatalogEntry {
    std::string id;
    WorkloadKind kind = WorkloadKind::Render;
    RenderCase render;
    std::uint32_t virtualCapacity = 0;
    StreamingCase streaming;
};

std::vector<CatalogEntry> BuildCatalog() {
    constexpr std::array<std::uint32_t, 4> kTiers = {64, 128, 256, 512};
    constexpr std::array<int, 9> kPitch = {
        -48, -36, -24, -12, 0, 12, 24, 36, 48};
    std::vector<CatalogEntry> catalog;
    catalog.reserve(35);
    const auto addRender = [&catalog](const std::string& id,
                                      std::uint32_t render,
                                      std::uint32_t parked,
                                      std::uint32_t sourceRate,
                                      std::uint32_t mixRate,
                                      std::uint32_t channels,
                                      int pitch) {
        CatalogEntry entry;
        entry.id = id;
        entry.kind = WorkloadKind::Render;
        entry.render.renderCapacity = render;
        entry.render.virtualCapacity = parked;
        entry.render.sourceRate = sourceRate;
        entry.render.mixRate = mixRate;
        entry.render.sourceChannels = channels;
        entry.render.pitchSemitones = pitch;
        catalog.push_back(std::move(entry));
    };
    for (const std::uint32_t rate : {44100u, 48000u}) {
        for (const std::uint32_t tier : kTiers) {
            addRender("render.native." + std::to_string(rate) +
                          ".stereo.p0.r" + std::to_string(tier),
                      tier, 64, rate, rate, 2, 0);
        }
    }
    addRender("render.cross.44100_to_48000.stereo.p0.r64",
              64, 64, 44100, 48000, 2, 0);
    addRender("render.cross.48000_to_44100.stereo.p0.r64",
              64, 64, 48000, 44100, 2, 0);
    for (const int pitch : kPitch) {
        addRender("pitch.native.48000.stereo.p" +
                      std::to_string(pitch) + ".r16",
                  16, 16, 48000, 48000, 2, pitch);
    }
    for (const std::uint32_t tier : kTiers) {
        addRender("pitch_stress.native.48000.stereo.p48.r" +
                      std::to_string(tier),
                  tier, 64, 48000, 48000, 2, 48);
    }
    addRender("render.native.48000.mono.p0.r64",
              64, 64, 48000, 48000, 1, 0);
    addRender("pitch_stress.native.48000.mono.p48.r64",
              64, 64, 48000, 48000, 1, 48);

    for (const std::uint32_t parked : {0u, 64u, 128u, 256u, 512u}) {
        CatalogEntry entry;
        entry.id = "virtual.48000.r64.v" + std::to_string(parked);
        entry.kind = WorkloadKind::Virtual;
        entry.virtualCapacity = parked;
        catalog.push_back(std::move(entry));
    }

    for (const std::uint32_t streams : {1u, 4u, 8u, 16u}) {
        CatalogEntry entry;
        entry.id = "stream.warm.48000.r16.v16.s" +
                   std::to_string(streams);
        entry.kind = WorkloadKind::Streaming;
        entry.streaming.mode = "warm-served";
        entry.streaming.renderCapacity = 16;
        entry.streaming.virtualCapacity = 16;
        entry.streaming.streamVoices = streams;
        entry.streaming.pcmVoices = 0;
        catalog.push_back(std::move(entry));
    }
    CatalogEntry mixed;
    mixed.id = "stream.mixed.48000.r64.v64.s16.pcm48";
    mixed.kind = WorkloadKind::Streaming;
    mixed.streaming.mode = "mixed-warm-served";
    mixed.streaming.renderCapacity = 64;
    mixed.streaming.virtualCapacity = 64;
    mixed.streaming.streamVoices = 16;
    mixed.streaming.pcmVoices = 48;
    catalog.push_back(std::move(mixed));
    return catalog;
}

const char* WorkloadName(WorkloadKind kind) {
    switch (kind) {
    case WorkloadKind::Render:
        return "render";
    case WorkloadKind::Virtual:
        return "virtual";
    case WorkloadKind::Streaming:
        return "streaming";
    }
    return "unknown";
}

bool EnvironmentQualified(const Environment& environment) {
    return environment.processPrioritySet &&
           environment.processAffinitySet &&
           environment.threadPrioritySet &&
           environment.threadAffinitySet &&
           !environment.debuggerAttached &&
           !environment.profilerAttached &&
           environment.pinnedCpuCount == 1;
}

void PrintRepetitions(const std::vector<TimingSummary>& repetitions) {
    std::putchar('[');
    for (std::size_t index = 0; index < repetitions.size(); ++index) {
        if (index != 0) {
            std::putchar(',');
        }
        const TimingSummary& value = repetitions[index];
        std::printf(
            "{\"p12_5_us\":%.3f,\"p15_625_us\":%.3f,"
            "\"p50_us\":%.3f,"
            "\"p95_us\":%.3f,\"p99_us\":%.3f,"
            "\"warmup_blocks\":%u,"
            "\"max_us\":%.3f,\"nominal_period_exceed_count\":%llu}",
            value.p12_5Us, value.p15_625Us, value.p50Us,
            value.p95Us, value.p99Us, value.warmupBlocks,
            value.maximumUs,
            static_cast<unsigned long long>(
                value.nominalPeriodExceedCount));
    }
    std::putchar(']');
}

bool M12ScreeningCandidate(const std::vector<TimingSummary>& repetitions,
                           double nominalPeriodUs,
                           bool integrity,
                           bool deterministic,
                           const Environment& environment) {
    if (!integrity || !deterministic || repetitions.size() != 3 ||
        !EnvironmentQualified(environment)) {
        return false;
    }
    for (const TimingSummary& repetition : repetitions) {
        if (repetition.p99Us > nominalPeriodUs * kM12ScreeningP99Fraction ||
            repetition.maximumUs >= nominalPeriodUs ||
            repetition.nominalPeriodExceedCount != 0) {
            return false;
        }
    }
    return true;
}

void PrintOperation(const char* name, const TimingSummary& aggregate,
                    const std::vector<TimingSummary>& repetitions,
                    bool nominalApplicable, bool pilot,
                    bool integrity, bool deterministic,
                    std::uint32_t measuredBlocksPerRepetition,
                    double nominalPeriodUs,
                    const Environment& environment) {
    const bool candidate = !pilot && nominalApplicable &&
        M12ScreeningCandidate(repetitions, nominalPeriodUs, integrity,
                              deterministic, environment);
    const char* nominalObservation = "not_applicable";
    if (nominalApplicable) {
        nominalObservation = aggregate.nominalPeriodExceedCount == 0
            ? "zero_exceeds"
            : "one_or_more_exceeds";
    }
    std::printf(
        "\"%s\":{\"measurement_status\":\"measured_complete\","
        "\"measurement_integrity_status\":\"%s\","
        "\"determinism_status\":\"%s\","
        "\"nominal_period_observation\":\"%s\","
        "\"m12_screening_status\":\"%s\","
        "\"sample_count\":%llu,\"mean_us\":%.3f,"
        "\"p12_5_us\":%.3f,\"p15_625_us\":%.3f,"
        "\"p50_us\":%.3f,\"p95_us\":%.3f,\"p99_us\":%.3f,"
        "\"max_us\":%.3f,\"nominal_period_exceed_count\":%llu,"
        "\"repetitions\":",
        name, integrity ? "passed" : "failed",
        repetitions.size() == 3
            ? (deterministic ? "passed" : "failed")
            : "not_evaluated",
        nominalObservation,
        nominalApplicable ? (candidate ? "candidate" : "not_candidate")
                          : "not_applicable",
        static_cast<unsigned long long>(repetitions.size()) *
            measuredBlocksPerRepetition,
        aggregate.meanUs, aggregate.p12_5Us, aggregate.p15_625Us,
        aggregate.p50Us,
        aggregate.p95Us,
        aggregate.p99Us, aggregate.maximumUs,
        static_cast<unsigned long long>(
            aggregate.nominalPeriodExceedCount));
    PrintRepetitions(repetitions);
    std::putchar('}');
}

// Opt-in spatial characterization leaves the established capacity catalog intact.
struct SpatialCase {
    std::uint32_t voices = 32;
    std::uint32_t voicesPerEmitter = 1;
    std::string motion;
    bool curves = false;
    bool highpass = false;
};

VnAttenuationDesc SpatialProfile(bool curves, bool alternate = false, bool highpass = false) {
    VnAttenuationDesc desc{};
    desc.structSize = sizeof(desc);
    desc.rolloff = VN_ROLLOFF_LINEAR;
    desc.minDistance = 1.0f;
    desc.maxDistance = 100.0f;
    desc.airAbsorptionNearHz = 18000.0f;
    desc.airAbsorptionFarHz = 2000.0f;
    desc.proximityRadius = 20.0f;
    desc.proximityMaxBoostDb = 3.0f;
    desc.proximityFrequency = 200.0f;
    desc.coneEnabled = 1;
    desc.coneInnerDeg = 30.0f;
    desc.coneOuterDeg = 120.0f;
    desc.coneOuterGainDb = -6.0f;
    desc.coneOuterLowpassHz = 4000.0f;
    const auto fill = [](VnAttenuationCurve& curve, float nearValue, float farValue) {
        curve.enabled = 1;
        curve.pointCount = VN_ATTENUATION_MAX_CURVE_POINTS;
        for (std::uint32_t i = 0; i < curve.pointCount; ++i) {
            const float fraction = static_cast<float>(i) / 7.0f;
            curve.points[i] = {1.0f + 99.0f * fraction,
                nearValue + (farValue - nearValue) * fraction};
            if (i == 0) continue;
            const auto a = curve.points[i - 1];
            const auto b = curve.points[i];
            curve.handles[i - 1] = {a.x + (b.x - a.x) * 0.2f, a.y,
                a.x + (b.x - a.x) * 0.8f, b.y};
        }
    };
    if (highpass) fill(desc.highpassCurve, 0.0f, alternate ? 1800.0f : 1000.0f);
    if (!curves) {
        if (alternate) desc.maxDistance = 90.0f;
        return desc;
    }
    fill(desc.volumeCurve, 0.0f, alternate ? -50.0f : -60.0f);
    fill(desc.lowpassCurve, 18000.0f, 2000.0f);
    fill(desc.widthCurve, 0.8f, 0.1f);
    return desc;
}

struct SpatialRun {
    bool passed = false;
    VnResult failure = VN_OK;
    const char* stage = "setup";
    std::vector<double> publishUs, updateUs, mainUs, renderUs;
    std::vector<double> eventMainUs;
    std::uint64_t hash = kFnvOffset;
    std::uint64_t nonfinite = 0;
    std::uint64_t setterCalls = 0;
    std::size_t mainAllocations = 0, renderAllocations = 0;
    std::size_t mainBytes = 0, renderBytes = 0;
    std::size_t nonEventMainAllocations = 0, nonEventMainBytes = 0;
    std::uint64_t maxBacklog = 0, remainingBacklog = 0;
    std::uint32_t maxDrainBlocks = 0;
    std::uint32_t pendingPeak = 0, residencyEvents = 0, profileChanges = 0;
    bool warmupEvaluated = false;
    bool warmupStable = false;
    VnSnapshot snapshot{};
    float peak = 0.0f;
};

SpatialRun RunSpatialOnce(const SpatialCase& scenario, std::uint32_t warmup,
                          std::uint32_t ticks) {
    SpatialRun run;
    VnConfig config{};
    config.structSize = sizeof(config);
    config.sampleRate = 48000;
    config.blockFrames = kBlockFrames;
    config.maxVoices = scenario.voices;
    config.maxEmitters = scenario.voices / scenario.voicesPerEmitter;
    config.maxParkedVoices = 16;
    config.arenaBytes = kArenaBytes;
    config.deviceMode = VN_DEVICE_NULL;
    config.commandQueueDepth = 16384;
    config.maxControlCommandsPerQuantum = 8192;
    config.notificationQueueDepth = 2048;
    config.mixChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    config.deviceChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    config.renderWorkerCount = 0;
    const bool residency = scenario.motion == "residency";
    if (residency) {
        config.voiceStealPolicy = 2;
        config.audibilityParkEnabled = 1;
        config.parkThresholdDb = -90.0f;
    }
    run.failure = Vn_Init(&config);
    if (run.failure != VN_OK) return run;
    EngineGuard guard(true);
    auto* engine = harken::detail::ActiveEngine();
    const auto source = MakeInterleavedSource(48000, 1);
    VnPCMDesc pcm{};
    pcm.structSize = sizeof(pcm);
    pcm.sampleRate = 48000;
    pcm.channels = 1;
    pcm.layout = VN_PCM_LAYOUT_INTERLEAVED;
    pcm.policy = VN_PCM_POLICY_REFERENCE;
    pcm.frames = source.size();
    pcm.samplesInterleaved = source.data();
    const auto sound = harkentest::ObjectId(0x5350415449414c01ull);
    const auto profile = harkentest::ObjectId(0x5350415449414c02ull);
    auto desc = SpatialProfile(scenario.curves, false, scenario.highpass);
    if ((run.failure = Vn_RegisterPCM(sound, &pcm)) != VN_OK ||
        (run.failure = Vn_RegisterAttenuation(profile, &desc)) != VN_OK) return run;
    std::vector<VnEmitterHandle> emitters(config.maxEmitters);
    std::vector<VnVoiceHandle> voices(scenario.voices);
    std::array<VnVoiceHandle, 8> parked{};
    for (std::uint32_t i = 0; i < emitters.size(); ++i) {
        if ((run.failure = Vn_CreateEmitter(&emitters[i])) != VN_OK ||
            (run.failure = Vn_SetEmitterPosition(emitters[i],
                {static_cast<float>(i % 9u) - 4.0f, 0.0f, -40.0f})) != VN_OK ||
            (run.failure = Vn_SetEmitterOrientation(emitters[i], {0, 0, 1})) != VN_OK)
            return run;
    }
    if (residency) {
        run.stage = "parked setup";
        for (auto& voice : parked) {
            VnPlayDesc play{};
            play.structSize = sizeof(play);
            play.soundId = sound;
            play.busId = VN_BUS_MASTER;
            play.volumeDb = -100.0f;
            play.pitchRatio = 1.0f;
            play.spatialized = 1;
            play.emitterHandle = emitters.front();
            play.priority = 128;
            if ((run.failure = harkentest::PlayRaw(&play, &voice)) != VN_OK ||
                (run.failure = Vn_BindVoiceAttenuation(voice, profile)) != VN_OK) return run;
        }
        engine->nullBackend.Pump(1);
        if ((run.failure = Vn_Update()) != VN_OK) return run;
        engine->nullBackend.Pump(1);
        if ((run.failure = Vn_Update()) != VN_OK) return run;
        engine->nullBackend.Pump(1);
        for (auto voice : parked) {
            if (engine->ledger.Classify(voice).kind != harken::VoiceLedger::HandleKind::Parked) {
                run.failure = VN_IN_USE;
                return run;
            }
        }
    }
    for (std::uint32_t i = 0; i < voices.size(); ++i) {
        VnPlayDesc play{};
        play.structSize = sizeof(play);
        play.soundId = sound;
        play.busId = VN_BUS_MASTER;
        play.volumeDb = -60.0f;
        play.pitchRatio = 1.0f;
        play.spatialized = 1;
        play.emitterHandle = emitters[i / scenario.voicesPerEmitter];
        play.priority = 128;
        if ((run.failure = harkentest::PlayRaw(&play, &voices[i])) != VN_OK ||
            (run.failure = Vn_BindVoiceAttenuation(voices[i], profile)) != VN_OK)
            return run;
    }
    if ((run.failure = Vn_Update()) != VN_OK) return run;
    engine->nullBackend.Pump(1);
    run.publishUs.reserve(ticks);
    run.updateUs.reserve(ticks);
    run.mainUs.reserve(ticks);
    run.renderUs.reserve(static_cast<std::size_t>(ticks) * 4);
    run.eventMainUs.reserve(ticks);
    std::vector<double> warmupMain, warmupRender;
    warmupMain.reserve(warmup);
    warmupRender.reserve(warmup);
    std::uint32_t audioFrames = 0;
    bool alternateProfile = false;
    for (std::uint32_t tick = 0; tick < warmup + ticks; ++tick) {
        const bool measure = tick >= warmup;
        const bool residencyEvent = residency && (tick == warmup || tick % 60u == 0);
        const bool replaceEvent = scenario.motion == "replace" && (tick == warmup || tick % 60u == 0);
        const bool transition = residencyEvent || engine->ledger.PendingStartCount() != 0 ||
            replaceEvent;
        allocation_probe::count = 0;
        allocation_probe::bytes = 0;
        allocation_probe::enabled = measure;
        const auto start = std::chrono::steady_clock::now();
        const float offset = static_cast<float>(tick % 120u) * 0.05f;
        if (scenario.motion == "listener") {
            run.failure = Vn_SetListener({offset, 0, 0}, {0, 0, -1}, {0, 1, 0});
            if (measure) ++run.setterCalls;
        } else if (scenario.motion == "all" || scenario.motion == "ten" || residency) {
            const std::uint32_t moving = scenario.motion == "all" || residency
                ? static_cast<std::uint32_t>(emitters.size())
                : std::max(1u, static_cast<std::uint32_t>((emitters.size() + 9u) / 10u));
            for (std::uint32_t i = 0; i < moving && run.failure == VN_OK; ++i) {
                run.failure = Vn_SetEmitterPosition(emitters[i],
                    {static_cast<float>(i % 9u) - 4.0f + offset, 0.0f, -40.0f});
                if (run.failure == VN_OK)
                    run.failure = Vn_SetEmitterOrientation(emitters[i], {offset * 0.1f, 0, 1});
                if (measure) run.setterCalls += 2;
            }
        } else if (replaceEvent) {
            alternateProfile = !alternateProfile;
            desc = SpatialProfile(scenario.curves, alternateProfile, scenario.highpass);
            run.failure = Vn_ReplaceAttenuation(profile, &desc);
            if (measure && run.failure == VN_OK) ++run.profileChanges;
        }
        if (run.failure == VN_OK && residencyEvent) {
            alternateProfile = !alternateProfile;
            desc = SpatialProfile(scenario.curves, alternateProfile, scenario.highpass);
            run.failure = Vn_ReplaceAttenuation(profile, &desc);
            if (measure && run.failure == VN_OK) ++run.profileChanges;
            const auto retired = emitters.front();
            if (run.failure == VN_OK) run.failure = Vn_DestroyEmitter(retired);
            if (run.failure == VN_OK) run.failure = Vn_CreateEmitter(&emitters.front());
            if (run.failure == VN_OK && emitters.front() == retired) run.failure = VN_STALE_HANDLE;
            if (run.failure == VN_OK)
                run.failure = Vn_SetEmitterPosition(emitters.front(), {offset, 0, -40});
            if (run.failure == VN_OK)
                run.failure = Vn_SetEmitterOrientation(emitters.front(), {0, 0, 1});
            for (std::uint32_t i = 0; i < 8 && run.failure == VN_OK; ++i) {
                VnPlayDesc play{};
                play.structSize = sizeof(play);
                play.soundId = sound;
                play.busId = VN_BUS_MASTER;
                play.volumeDb = -60.0f;
                play.pitchRatio = 1.0f;
                play.spatialized = 1;
                play.emitterHandle = emitters.front();
                play.priority = 128;
                VnVoiceHandle voice = 0;
                run.failure = harkentest::PlayRaw(&play, &voice);
                if (run.failure == VN_OK) run.failure = Vn_BindVoiceAttenuation(voice, profile);
            }
            if (measure) {
                ++run.residencyEvents;
                run.setterCalls += 2;
                run.pendingPeak = std::max(run.pendingPeak, engine->ledger.PendingStartCount());
            }
        }
        const auto published = std::chrono::steady_clock::now();
        if (run.failure == VN_OK) run.failure = Vn_Update();
        const auto updated = std::chrono::steady_clock::now();
        allocation_probe::enabled = false;
        if (measure) {
            run.mainAllocations += allocation_probe::count;
            run.mainBytes += allocation_probe::bytes;
            if (!transition) {
                run.nonEventMainAllocations += allocation_probe::count;
                run.nonEventMainBytes += allocation_probe::bytes;
            }
        }
        run.stage = "publish/update";
        if (run.failure != VN_OK) return run;
        if (measure) {
            run.publishUs.push_back(std::chrono::duration<double, std::micro>(published - start).count());
            run.updateUs.push_back(std::chrono::duration<double, std::micro>(updated - published).count());
            run.mainUs.push_back(std::chrono::duration<double, std::micro>(updated - start).count());
            if (residencyEvent) run.eventMainUs.push_back(run.mainUs.back());
        } else {
            warmupMain.push_back(std::chrono::duration<double, std::micro>(updated - start).count());
        }
        const auto targetHead = engine->commands.PublishedHead();
        if (measure) run.maxBacklog = std::max(run.maxBacklog, targetHead - engine->commands.ConsumedCount());
        std::uint32_t tickBlocks = 0;
        std::uint32_t drainedAt = targetHead == engine->commands.ConsumedCount() ? 0u : UINT32_MAX;
        double warmupRenderTotal = 0.0;
        // A 60 Hz logic cadence produces exactly 800 audio frames per tick.
        audioFrames += 800;
        while (audioFrames >= kBlockFrames) {
            audioFrames -= kBlockFrames;
            const auto before = engine->mixer.BlocksRendered();
            allocation_probe::count = 0;
            allocation_probe::bytes = 0;
            allocation_probe::enabled = measure;
            const auto renderStart = std::chrono::steady_clock::now();
            engine->nullBackend.Pump(1);
            const auto renderEnd = std::chrono::steady_clock::now();
            allocation_probe::enabled = false;
            if (measure) {
                run.renderAllocations += allocation_probe::count;
                run.renderBytes += allocation_probe::bytes;
            }
            ++tickBlocks;
            if (drainedAt == UINT32_MAX && engine->commands.ConsumedCount() >= targetHead)
                drainedAt = tickBlocks;
            warmupRenderTotal += std::chrono::duration<double, std::micro>(renderEnd - renderStart).count();
            run.stage = "fresh render block";
            if (engine->mixer.BlocksRendered() != before + 1u) {
                run.failure = VN_IN_USE;
                return run;
            }
            if (measure) {
                run.renderUs.push_back(std::chrono::duration<double, std::micro>(renderEnd - renderStart).count());
                HashAndInspect(engine->nullBackend.LastBlock(), kBlockFrames * 2,
                    &run.hash, &run.peak, &run.nonfinite);
            }
        }
        if (!measure) warmupRender.push_back(warmupRenderTotal / tickBlocks);
        else run.maxDrainBlocks = std::max(run.maxDrainBlocks, drainedAt);
    }
    run.remainingBacklog = engine->commands.PublishedHead() - engine->commands.ConsumedCount();
    run.warmupEvaluated = warmup >= 128;
    run.warmupStable = run.warmupEvaluated &&
        (WarmupWindowsStable(warmupMain, 96, 112) && WarmupWindowsStable(warmupRender, 96, 112));
    run.stage = "live voice identities";
    const auto* retained = residency ? parked.data() : voices.data();
    const auto retainedCount = residency ? parked.size() : voices.size();
    const auto expected = residency ? harken::VoiceLedger::HandleKind::Parked : harken::VoiceLedger::HandleKind::Live;
    for (std::size_t index = 0; index < retainedCount; ++index) {
        if (engine->ledger.Classify(retained[index]).kind != expected) {
            run.failure = VN_STALE_HANDLE;
            return run;
        }
    }
    run.stage = "snapshot";
    run.snapshot.structSize = sizeof(run.snapshot);
    run.failure = Vn_CaptureSnapshot(&run.snapshot);
    if (run.failure != VN_OK) return run;
    run.passed = run.peak > kOutputFloor && run.nonfinite == 0 &&
        run.snapshot.blockIndex + 1 == engine->mixer.BlocksRendered() &&
        run.snapshot.activeVoiceCount == scenario.voices &&
        run.snapshot.arenaOverflowCount == 0 &&
        run.snapshot.paramPublicationDeferredCount == 0 &&
        run.remainingBacklog == 0 && run.maxDrainBlocks <= 1 &&
        (!residency || (run.pendingPeak == 8 && run.snapshot.parkedVoiceCount == 8 &&
                       engine->ledger.PendingStartCount() == 0));
    run.stage = "complete";
    return run;
}

int RunSpatial(const SpatialCase& scenario, bool smoke, bool once) {
#if !defined(NDEBUG)
    std::fprintf(stderr, "Optimized build required\n");
    return 4;
#else
    const auto environment = ConfigureEnvironment();
    allocation_probe::count = 0;
    allocation_probe::bytes = 0;
    allocation_probe::enabled = true;
    void* control = ::operator new(8);
    allocation_probe::enabled = false;
    const bool allocationProbeAlive = allocation_probe::count == 1 && allocation_probe::bytes == 8;
    ::operator delete(control);
    if (!allocationProbeAlive) {
        std::fprintf(stderr, "Allocation probe positive control failed\n");
        return 1;
    }
    const std::uint32_t repetitions = smoke || once ? 1u : 5u;
    const std::uint32_t ticks = smoke ? 16u : 400u;
    const std::uint32_t warmup = smoke ? 8u : 128u;
    bool passed = true;
    std::uint64_t reference = 0;
    for (std::uint32_t repetition = 0; repetition < repetitions; ++repetition) {
        auto run = RunSpatialOnce(scenario, warmup, ticks);
        if (repetition == 0) reference = run.hash;
        const bool repetitionIntegrity = run.passed && run.hash == reference;
        passed = passed && repetitionIntegrity;
        std::printf("{\"benchmark\":\"spatial-update\",\"schema\":\"spatial-update-v1\","
            "\"mode\":\"%s\",\"repetition\":%u,\"voices\":%u,\"voices_per_emitter\":%u,"
            "\"motion\":\"%s\",\"curves\":%s,\"distance_highpass\":%s,\"logic_hz\":60,\"sample_rate\":48000,"
            "\"queue_depth\":16384,\"control_commands_per_block\":8192,\"render_workers\":0,"
            "\"block_frames\":256,\"warmup_ticks\":%u,\"measured_ticks\":%u,"
            "\"environment_qualified\":%s,\"pinned_cpu\":%d,\"source_commit\":",
            smoke ? "smoke" : "formal", repetition, scenario.voices, scenario.voicesPerEmitter,
            scenario.motion.c_str(), scenario.curves ? "true" : "false",
            scenario.highpass ? "true" : "false", warmup, ticks,
            EnvironmentQualified(environment) ? "true" : "false", environment.pinnedCpuIndex);
        PrintJsonString(environment.sourceCommit.c_str());
        std::fputs(",\"source_dirty\":", stdout);
        PrintJsonString(environment.sourceDirty.c_str());
        const auto print = [](const char* name, const std::vector<double>& samples, double budget) {
            const auto stats = SummarizeTiming(samples, budget);
            std::printf(",\"%s\":{\"samples\":%llu,\"p50_us\":%.3f,\"p95_us\":%.3f,"
                "\"p99_us\":%.3f,\"max_us\":%.3f,\"budget_us\":%.3f,\"exceeds\":%llu}",
                name, static_cast<unsigned long long>(samples.size()), stats.p50Us, stats.p95Us,
                stats.p99Us, stats.maximumUs, budget,
                static_cast<unsigned long long>(stats.nominalPeriodExceedCount));
        };
        print("publish", run.publishUs, 1000.0);
        print("update", run.updateUs, 1000.0);
        print("main", run.mainUs, 1000.0);
        print("residency_event_main", run.eventMainUs, 1000.0);
        print("render", run.renderUs, 1000000.0 * kBlockFrames / 48000.0);
        std::printf(",\"main_new_allocations\":%llu,\"render_new_allocations\":%llu,"
            "\"main_new_bytes\":%llu,\"render_new_bytes\":%llu,\"probe_control_passed\":true,"
            "\"non_event_main_new_allocations\":%llu,\"non_event_main_new_bytes\":%llu,"
            "\"max_backlog\":%llu,\"remaining_backlog\":%llu,\"max_drain_blocks\":%u,"
            "\"publisher_deferrals\":%llu,\"warmup_stable\":%s,\"live_voices\":%u,"
            "\"warmup_evaluated\":%s,\"timing_qualified\":%s,"
            "\"parked_voices\":%llu,\"pending_peak\":%u,\"residency_events\":%u,\"profile_changes\":%u,"
            "\"allocation_scope\":\"current_thread_cpp_new_family\",\"setter_calls\":%llu,"
            "\"output_hash\":\"%016llx\",\"peak\":%.9g,\"nonfinite\":%llu,"
            "\"integrity_passed\":%s,\"result\":%d,\"stage\":\"%s\","
            "\"support_claim\":false,\"real_device_qualification\":false}\n",
            static_cast<unsigned long long>(run.mainAllocations),
            static_cast<unsigned long long>(run.renderAllocations),
            static_cast<unsigned long long>(run.mainBytes),
            static_cast<unsigned long long>(run.renderBytes),
            static_cast<unsigned long long>(run.nonEventMainAllocations),
            static_cast<unsigned long long>(run.nonEventMainBytes),
            static_cast<unsigned long long>(run.maxBacklog),
            static_cast<unsigned long long>(run.remainingBacklog), run.maxDrainBlocks,
            static_cast<unsigned long long>(run.snapshot.paramPublicationDeferredCount),
            run.warmupStable ? "true" : "false", run.snapshot.activeVoiceCount,
            run.warmupEvaluated ? "true" : "false",
            !smoke && run.warmupStable && repetitionIntegrity && EnvironmentQualified(environment) &&
                environment.sourceCommit != "unknown" && environment.sourceDirty == "0" ? "true" : "false",
            static_cast<unsigned long long>(run.snapshot.parkedVoiceCount), run.pendingPeak,
            run.residencyEvents, run.profileChanges,
            static_cast<unsigned long long>(run.setterCalls),
            static_cast<unsigned long long>(run.hash), run.peak,
            static_cast<unsigned long long>(run.nonfinite),
            repetitionIntegrity ? "true" : "false", run.failure, run.stage);
        std::fflush(stdout);
        if (!run.passed) break;
    }
    return passed ? 0 : 1;
#endif
}

} // namespace

int main(int argc, char* argv[]) {
    if (argc >= 2 && std::strcmp(argv[1], "--spatial") == 0) {
        if (argc < 6 || argc > 8) return 2;
        SpatialCase scenario;
        bool smoke = false;
        bool once = false;
        for (int i = 6; i < argc; ++i) {
            if (std::strcmp(argv[i], "--highpass") == 0 && !scenario.highpass) {
                scenario.highpass = true;
            } else if (std::strcmp(argv[i], "--smoke") == 0 && !smoke && !once) {
                smoke = true;
            } else if (std::strcmp(argv[i], "--once") == 0 && !smoke && !once) {
                once = true;
            } else {
                return 2;
            }
        }
        const auto parse = [](const char* text, std::uint32_t* out) {
            char* end = nullptr;
            const auto value = std::strtoul(text, &end, 10);
            if (text == end || *end != '\0' || value == 0 || value > 512) return false;
            *out = static_cast<std::uint32_t>(value);
            return true;
        };
        if (!parse(argv[2], &scenario.voices) || !parse(argv[3], &scenario.voicesPerEmitter) ||
            (scenario.voicesPerEmitter != 1 && scenario.voicesPerEmitter != 4) ||
            scenario.voices % scenario.voicesPerEmitter != 0) return 2;
        scenario.motion = argv[4];
        if (scenario.motion != "static" && scenario.motion != "ten" &&
            scenario.motion != "all" && scenario.motion != "listener" &&
            scenario.motion != "replace" && scenario.motion != "residency") return 2;
        if (scenario.motion == "residency" && scenario.voices < 32) return 2;
        if (std::strcmp(argv[5], "builtin") != 0 && std::strcmp(argv[5], "curves") != 0) return 2;
        scenario.curves = std::strcmp(argv[5], "curves") == 0;
        return RunSpatial(scenario, smoke, once);
    }
    bool list = false;
    bool listResources = false;
    std::string resourceId;
    Options options;
    for (int index = 1; index < argc; ++index) {
        if (std::strcmp(argv[index], "--list") == 0) {
            list = true;
        } else if (std::strcmp(argv[index], "--list-resources") == 0) {
            listResources = true;
        } else if (std::strcmp(argv[index], "--resource") == 0 &&
                   index + 1 < argc) {
            resourceId = argv[++index];
        } else if (std::strcmp(argv[index], "--pilot") == 0) {
            options.pilot = true;
        } else if (std::strcmp(argv[index], "--smoke") == 0) {
            options.smoke = true;
        } else if (std::strcmp(argv[index], "--scenario") == 0 &&
                   index + 1 < argc) {
            options.scenarioId = argv[++index];
        } else if (std::strcmp(argv[index], "--help") == 0) {
            std::printf(
                "Usage: harken_runtime_capacity_benchmark "
                "--list | --list-resources | --resource <id> | "
                "--scenario <id> [--pilot|--smoke] | "
                "--spatial <voices:1..512> <voices-per-emitter:1|4> "
                "<static|ten|all|listener|replace|residency> <builtin|curves> [--highpass] [--smoke|--once]\n");
            return 0;
        } else {
            std::fprintf(stderr, "Unknown or incomplete argument\n");
            return 2;
        }
    }

    const std::vector<CatalogEntry> catalog = BuildCatalog();
    const std::vector<ResourceEntry> resources = BuildResourceCatalog();
    if (list) {
        for (const CatalogEntry& entry : catalog) {
            std::uint32_t render = 0;
            std::uint32_t parked = 0;
            std::uint32_t sourceRate = 0;
            std::uint32_t mixRate = 48000;
            std::uint32_t channels = 0;
            int pitch = 0;
            std::uint32_t streams = 0;
            std::uint32_t pcm = 0;
            if (entry.kind == WorkloadKind::Render) {
                render = entry.render.renderCapacity;
                parked = entry.render.virtualCapacity;
                sourceRate = entry.render.sourceRate;
                mixRate = entry.render.mixRate;
                channels = entry.render.sourceChannels;
                pitch = entry.render.pitchSemitones;
            } else if (entry.kind == WorkloadKind::Virtual) {
                render = 64;
                parked = entry.virtualCapacity;
                sourceRate = 48000;
                channels = 1;
            } else {
                render = entry.streaming.renderCapacity;
                parked = entry.streaming.virtualCapacity;
                sourceRate = 48000;
                channels = 1;
                streams = entry.streaming.streamVoices;
                pcm = entry.streaming.pcmVoices;
            }
            std::printf(
                "{\"benchmark\":\"%s\",\"schema\":\"%s\","
                "\"record\":\"catalog\",\"scenario_id\":",
                kBenchmarkName, kSchema);
            PrintJsonString(entry.id.c_str());
            std::printf(
                ",\"kind\":\"%s\",\"render_capacity\":%u,"
                "\"virtual_capacity\":%u,\"logical_capacity\":%u,"
                "\"source_rate\":%u,\"mix_rate\":%u,"
                "\"channels\":%u,\"pitch_semitones\":%d,"
                "\"stream_voices\":%u,\"pcm_voices\":%u,"
                "\"block_frames\":%u,\"render_worker_count\":0,"
                "\"mix_channels\":2,\"device_channels\":2,"
                "\"device_rate\":%u,\"arena_bytes\":%u,"
                "\"command_queue_depth\":2048,"
                "\"notification_queue_depth\":2048,"
                "\"max_streams\":16,\"stream_ring_frames\":16384,"
                "\"effect_count\":0,\"send_count\":0,"
                "\"spatialized\":false,"
                "\"fixture_profile\":\"deterministic_generated_v1\"}\n",
                WorkloadName(entry.kind), render, parked, render + parked,
                sourceRate, mixRate, channels, pitch, streams, pcm,
                kBlockFrames, mixRate, kArenaBytes);
        }
        std::printf(
            "{\"benchmark\":\"%s\",\"schema\":\"%s\","
            "\"record\":\"catalog_summary\",\"scenario_count\":%u}\n",
            kBenchmarkName, kSchema,
            static_cast<unsigned>(catalog.size()));
        return catalog.size() == 35 ? 0 : 3;
    }

    if (listResources) {
        for (const ResourceEntry& entry : resources) {
            std::printf(
                "{\"benchmark\":\"%s\",\"schema\":\"%s\","
                "\"record\":\"resource_catalog\",\"scenario_id\":",
                kBenchmarkName, kSchema);
            PrintJsonString(entry.id.c_str());
            std::printf(
                ",\"ladder\":\"%s\",\"render_capacity\":%u,"
                "\"virtual_capacity\":%u,\"logical_capacity\":%u}\n",
                entry.ladder, entry.renderCapacity, entry.virtualCapacity,
                entry.renderCapacity + entry.virtualCapacity);
        }
        std::printf(
            "{\"benchmark\":\"%s\",\"schema\":\"%s\","
            "\"record\":\"resource_catalog_summary\","
            "\"scenario_count\":%u}\n",
            kBenchmarkName, kSchema,
            static_cast<unsigned>(resources.size()));
        return resources.size() == 12 ? 0 : 3;
    }

    if (!resourceId.empty()) {
        const auto resource = std::find_if(
            resources.begin(), resources.end(),
            [&resourceId](const ResourceEntry& entry) {
                return entry.id == resourceId;
            });
        if (resource == resources.end()) {
            std::fprintf(stderr, "Unknown resource id\n");
            return 2;
        }
        return RunResourceScenario(*resource);
    }

    if (options.scenarioId.empty()) {
        std::fprintf(stderr, "--scenario is required unless --list is used\n");
        return 2;
    }
    const auto found = std::find_if(
        catalog.begin(), catalog.end(),
        [&options](const CatalogEntry& entry) {
            return entry.id == options.scenarioId;
        });
    if (found == catalog.end()) {
        std::fprintf(stderr, "Unknown scenario id\n");
        return 2;
    }
#if !defined(NDEBUG)
    std::fprintf(stderr, "Release build required\n");
    return 4;
#else
    const Environment environment = ConfigureEnvironment();
    const std::uint32_t repetitions =
        options.pilot || options.smoke ? 1u : kCanonicalRepetitions;
    const std::uint32_t warmupBlocks =
        options.pilot ? kPilotPrimingBlocks
                      : (options.smoke ? kSmokeWarmupBlocksTotal
                                       : kCanonicalWarmupBlocksPerRepetition);
    const std::uint32_t measuredBlocks =
        options.pilot ? kPilotMeasuredBlocks
                      : (options.smoke ? kSmokeMeasuredBlocksTotal
                                       : kCanonicalMeasuredBlocksPerRepetition);
    const char* mode =
        options.pilot ? "pilot" : (options.smoke ? "smoke" : "formal");

    bool passed = false;
    bool deterministic = false;
    const char* error = nullptr;
    std::printf(
        "{\"benchmark\":\"%s\",\"schema\":\"%s\","
        "\"record\":\"scenario\",\"scenario_id\":",
        kBenchmarkName, kSchema);
    PrintJsonString(found->id.c_str());
    std::printf(
        ",\"mode\":\"%s\",\"kind\":\"%s\","
        "\"environment_status\":\"%s\","
        "\"process_priority_high\":%s,"
        "\"process_affinity_single_cpu\":%s,"
        "\"thread_priority_highest\":%s,"
        "\"thread_affinity_single_cpu\":%s,"
        "\"debugger_attached\":%s,\"profiler_attached\":%s,"
        "\"hardware_threads\":%u,\"pinned_cpu_index\":%d,"
        "\"processor_identifier\":",
        mode, WorkloadName(found->kind),
        EnvironmentQualified(environment) ? "qualified" : "unqualified",
        environment.processPrioritySet ? "true" : "false",
        environment.processAffinitySet ? "true" : "false",
        environment.threadPrioritySet ? "true" : "false",
        environment.threadAffinitySet ? "true" : "false",
        environment.debuggerAttached ? "true" : "false",
        environment.profilerAttached ? "true" : "false",
        environment.hardwareThreads, environment.pinnedCpuIndex);
    PrintJsonString(environment.processorIdentifier.c_str());
    std::printf(
        ",\"compiler\":\"%s\",\"source_commit\":",
        CompilerName());
    PrintJsonString(environment.sourceCommit.c_str());
    std::fputs(",\"source_dirty\":", stdout);
    PrintJsonString(environment.sourceDirty.c_str());
    std::fputs(
        ",\"functional_contract_status\":\"separate_suite\","
        "\"measurements\":{",
        stdout);

    if (found->kind == WorkloadKind::Render) {
        const CaseResult result = MeasureRenderCase(
            found->render, warmupBlocks, measuredBlocks, repetitions);
        const double nominalPeriodUs =
            1000000.0 * static_cast<double>(kBlockFrames) /
            static_cast<double>(found->render.mixRate);
        PrintOperation("pump", result.timing, result.repetitionTimings,
                       true, options.pilot || options.smoke,
                       result.integrityPassed, result.deterministic,
                       measuredBlocks, nominalPeriodUs, environment);
        passed = result.integrityPassed;
        deterministic = result.deterministic;
        error = result.error;
    } else if (found->kind == WorkloadKind::Virtual) {
        const VirtualCaseResult result = MeasureVirtualCase(
            found->virtualCapacity, warmupBlocks, measuredBlocks,
            repetitions);
        constexpr double kNominalPeriodUs =
            1000000.0 * static_cast<double>(kBlockFrames) / 48000.0;
        PrintOperation("pump", result.callbackTiming,
                       result.callbackRepetitions, true,
                       options.pilot || options.smoke,
                       result.integrityPassed, result.deterministic,
                       measuredBlocks, kNominalPeriodUs, environment);
        std::putchar(',');
        PrintOperation("update", result.updateTiming,
                       result.updateRepetitions, false,
                       options.pilot || options.smoke,
                       result.integrityPassed, result.deterministic,
                       measuredBlocks, kNominalPeriodUs, environment);
        passed = result.integrityPassed;
        deterministic = result.deterministic;
        error = result.error;
    } else {
        const StreamingCaseResult result = MeasureStreamingCase(
            found->streaming, warmupBlocks, measuredBlocks, repetitions);
        constexpr double kNominalPeriodUs =
            1000000.0 * static_cast<double>(kBlockFrames) / 48000.0;
        PrintOperation("pump", result.callbackTiming,
                       result.callbackRepetitions, true,
                       options.pilot || options.smoke,
                       result.integrityPassed, result.deterministic,
                       measuredBlocks, kNominalPeriodUs, environment);
        std::putchar(',');
        PrintOperation("sync_serve", result.serviceTiming,
                       result.serviceRepetitions, false,
                       options.pilot || options.smoke,
                       result.integrityPassed, result.deterministic,
                       measuredBlocks, kNominalPeriodUs, environment);
        std::putchar(',');
        PrintOperation("update", result.updateTiming,
                       result.updateRepetitions, false,
                       options.pilot || options.smoke,
                       result.integrityPassed, result.deterministic,
                       measuredBlocks, kNominalPeriodUs, environment);
        passed = result.integrityPassed;
        deterministic = result.deterministic;
        error = result.error;
    }
    std::printf(
        "},\"status\":\"%s\",\"deterministic\":%s,"
        "\"support_claim\":false,\"real_device_qualification\":false,"
        "\"error\":",
        passed ? "complete" : "failed",
        deterministic ? "true" : "false");
    PrintJsonString(error != nullptr ? error : "");
    std::fputs("}\n", stdout);
    std::fflush(stdout);
    return passed ? 0 : 1;
#endif
}
