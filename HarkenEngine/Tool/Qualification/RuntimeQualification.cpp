#include <harken/Harken.h>
#include <harken_buildinfo.h>

#include "Engine.h"
#include "HarkenApiInternal.h"
#include HARKEN_RTQ_SOURCE_IDENTITY_HEADER
#include "harken/core/CpuClock.h"
#include "harken/core/RtProbe.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <map>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

constexpr std::uint64_t kDefaultStorageLimitBytes =
    16ull * 1024ull * 1024ull;
constexpr std::uint64_t kDeviceOnlyStorageLimitBytes =
    64ull * 1024ull * 1024ull;
constexpr VnSoundId kQualificationSound = 0x52545155414C0001ull;
#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK)
constexpr bool kDeviceCallbackEnabled = true;
#else
constexpr bool kDeviceCallbackEnabled = false;
#endif
#if defined(HARKEN_RT_PROBE_MIX_PUBLICATION)
constexpr bool kMixPublicationEnabled = true;
#else
constexpr bool kMixPublicationEnabled = false;
#endif
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
constexpr bool kWorkerBarrierEnabled = true;
#else
constexpr bool kWorkerBarrierEnabled = false;
#endif

struct Options {
    bool realDevice = false;
    bool allowShortDiagnostic = false;
    bool audibilityParking = false;
    bool deviceCallbackOnly = false;
    double seconds = 60.0;
    std::uint32_t sampleRate = 48000;
    std::uint32_t blockFrames = 256;
    std::uint32_t minCallbackFrames = 64;
    std::uint32_t voices = 8;
    std::uint32_t virtualVoices = 0;
    std::uint32_t workers = 0;
    std::uint32_t restartCount = 0;
    double pitchCents = 0.0;
    std::filesystem::path outputPath;
};

struct WallClockAnchor {
    std::uint64_t ticksBefore = 0;
    std::uint64_t unixNs = 0;
    std::uint64_t ticksAfter = 0;
};

WallClockAnchor CaptureWallClockAnchor() {
    WallClockAnchor anchor;
    anchor.ticksBefore = harken::CpuClockTicks();
    anchor.unixNs = static_cast<std::uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch())
            .count());
    anchor.ticksAfter = harken::CpuClockTicks();
    return anchor;
}

struct RestartSummary {
    struct Observation {
        std::uint32_t generation = 0;
        bool callbackObserved = false;
        std::uint64_t suspendBeginTicks = 0;
        std::uint64_t resumeEndTicks = 0;
        std::uint64_t callbackBeginTicks = 0;
        std::uint64_t callbackRecoveredTicks = 0;
        std::uint32_t activeVoices = 0;
        std::uint64_t parkedVoices = 0;
        std::uint64_t blockIndex = 0;
        harken::rtprobe::BackendObservation backend{};
    };

    std::uint32_t requested = 0;
    std::uint32_t completed = 0;
    std::uint64_t generationBefore = 0;
    std::uint64_t generationAfter = 0;
    VnResult suspendResult = VN_OK;
    VnResult resumeResult = VN_OK;
    bool initialBackendCaptured = false;
    harken::rtprobe::BackendObservation initialBackend{};
    harken::rtprobe::BackendObservation finalBackend{};
    std::array<Observation, 100> observations{};
};

bool SameBackendIdentity(
    const harken::rtprobe::BackendObservation& first,
    const harken::rtprobe::BackendObservation& second) {
    return first.valid == second.valid &&
           first.backendCode == second.backendCode &&
           first.clientSampleRate == second.clientSampleRate &&
           first.clientChannels == second.clientChannels &&
           first.clientFormat == second.clientFormat &&
           first.internalSampleRate == second.internalSampleRate &&
           first.internalChannels == second.internalChannels &&
           first.internalFormat == second.internalFormat &&
           first.deviceIdHash == second.deviceIdHash &&
           std::strcmp(first.backendName, second.backendName) == 0 &&
           std::strcmp(first.deviceName, second.deviceName) == 0;
}

bool RestartObservationsValid(const Options& options,
                              const RestartSummary& restarts) {
    if (restarts.completed != restarts.requested ||
        restarts.suspendResult != VN_OK ||
        restarts.resumeResult != VN_OK) {
        return false;
    }
    if (restarts.requested == 0) {
        return true;
    }
    if (!restarts.initialBackendCaptured ||
        restarts.generationAfter !=
            restarts.generationBefore + restarts.requested ||
        !SameBackendIdentity(restarts.initialBackend,
                             restarts.finalBackend)) {
        return false;
    }
    for (std::uint32_t index = 0; index < restarts.requested; ++index) {
        const RestartSummary::Observation& observation =
            restarts.observations[index];
        if (observation.generation !=
                restarts.generationBefore + index + 1u ||
            !observation.callbackObserved ||
            observation.suspendBeginTicks == 0 ||
            observation.resumeEndTicks < observation.suspendBeginTicks ||
            observation.callbackBeginTicks <
                observation.suspendBeginTicks ||
            observation.callbackRecoveredTicks <
                observation.callbackBeginTicks ||
            observation.activeVoices != options.voices ||
            observation.parkedVoices != options.virtualVoices ||
            !SameBackendIdentity(restarts.initialBackend,
                                 observation.backend)) {
            return false;
        }
    }
    return true;
}

struct TimingSummary {
    std::uint64_t count = 0;
    std::uint64_t invalidCount = 0;
    std::uint64_t deadlineMisses = 0;
    std::uint32_t minFrames = 0;
    std::uint32_t maxFrames = 0;
    double p50Us = 0.0;
    double p95Us = 0.0;
    double p99Us = 0.0;
    double p999Us = 0.0;
    double maxUs = 0.0;
};

struct WorkerSummary {
    std::uint64_t count = 0;
    std::uint64_t invalidCount = 0;
    std::uint64_t faultCount = 0;
    std::uint64_t serialFallbackCount = 0;
    std::uint64_t incompleteScratchCount = 0;
    std::uint64_t identityMismatchMask = 0;
    std::uint64_t maxYieldIterations = 0;
    double barrierP50Us = 0.0;
    double barrierP99Us = 0.0;
    double barrierMaxUs = 0.0;
    double firstWorkerRunP50Us = 0.0;
    double firstWorkerRunP99Us = 0.0;
};

struct CallbackShapeSummary {
    std::map<std::uint32_t, std::uint64_t> frameCounts;
    std::map<std::uint32_t, std::uint64_t> mixBlockCounts;
    std::map<std::uint32_t, std::uint64_t> stagingReadBefore;
    std::map<std::uint32_t, std::uint64_t> stagingAvailBefore;
    std::map<std::uint32_t, std::uint64_t> stagingReadAfter;
    std::map<std::uint32_t, std::uint64_t> stagingAvailAfter;
};

struct PublicationShapeSummary {
    std::uint64_t count = 0;
    std::uint64_t meterArmedCount = 0;
    std::uint64_t sourceCursorPublishedCount = 0;
};

struct OutputSignalSummary {
    std::uint64_t sampleCount = 0;
    std::uint64_t nonzeroSampleCount = 0;
    std::uint64_t nonFiniteSampleCount = 0;
    double sumSquares = 0.0;
    float peakAbs = 0.0f;
};

struct RawExport {
    std::filesystem::path timelinePath;
    std::filesystem::path workerPath;
};

bool ParseUnsigned(std::string_view text, std::uint32_t* value) {
    if (text.empty() || value == nullptr) {
        return false;
    }
    std::uint64_t parsed = 0;
    for (char c : text) {
        if (c < '0' || c > '9') {
            return false;
        }
        parsed = parsed * 10u + static_cast<std::uint32_t>(c - '0');
        if (parsed > std::numeric_limits<std::uint32_t>::max()) {
            return false;
        }
    }
    *value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool ParseSeconds(std::string_view text, double* value) {
    if (text.empty() || value == nullptr) {
        return false;
    }
    try {
        std::size_t consumed = 0;
        const double parsed = std::stod(std::string(text), &consumed);
        if (consumed != text.size() || !std::isfinite(parsed) ||
            parsed <= 0.0) {
            return false;
        }
        *value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

bool ParseFiniteDouble(std::string_view text, double* value) {
    if (text.empty() || value == nullptr) {
        return false;
    }
    try {
        std::size_t consumed = 0;
        const double parsed = std::stod(std::string(text), &consumed);
        if (consumed != text.size() || !std::isfinite(parsed)) {
            return false;
        }
        *value = parsed;
        return true;
    } catch (...) {
        return false;
    }
}

void PrintUsage() {
    std::cerr
        << "usage: harken_rt_qualification --output <report.json> "
           "[--device real|null] [--seconds 60] [--sample-rate 48000] "
           "[--block-frames 256] [--min-callback-frames 64] "
           "[--voices 8] [--virtual-voices 0] [--workers 0] "
           "[--pitch-cents 0] [--restart-count 0] "
           "[--audibility-parking] [--device-callback-only] "
           "[--allow-short-diagnostic]\n";
}

bool ParseOptions(int argc, char** argv, Options* options) {
    if (options == nullptr) {
        return false;
    }
    for (int i = 1; i < argc; ++i) {
        const std::string_view name(argv[i]);
        if (name == "--allow-short-diagnostic") {
            options->allowShortDiagnostic = true;
            continue;
        }
        if (name == "--audibility-parking") {
            options->audibilityParking = true;
            continue;
        }
        if (name == "--device-callback-only") {
            options->deviceCallbackOnly = true;
            continue;
        }
        if (i + 1 >= argc) {
            return false;
        }
        const std::string_view value(argv[++i]);
        if (name == "--output") {
            options->outputPath = std::filesystem::path(value);
        } else if (name == "--device") {
            if (value == "real") {
                options->realDevice = true;
            } else if (value == "null") {
                options->realDevice = false;
            } else {
                return false;
            }
        } else if (name == "--seconds") {
            if (!ParseSeconds(value, &options->seconds)) {
                return false;
            }
        } else if (name == "--sample-rate") {
            if (!ParseUnsigned(value, &options->sampleRate)) {
                return false;
            }
        } else if (name == "--block-frames") {
            if (!ParseUnsigned(value, &options->blockFrames)) {
                return false;
            }
        } else if (name == "--min-callback-frames") {
            if (!ParseUnsigned(value, &options->minCallbackFrames)) {
                return false;
            }
        } else if (name == "--voices") {
            if (!ParseUnsigned(value, &options->voices)) {
                return false;
            }
        } else if (name == "--virtual-voices") {
            if (!ParseUnsigned(value, &options->virtualVoices)) {
                return false;
            }
        } else if (name == "--workers") {
            if (!ParseUnsigned(value, &options->workers)) {
                return false;
            }
        } else if (name == "--restart-count") {
            if (!ParseUnsigned(value, &options->restartCount)) {
                return false;
            }
        } else if (name == "--pitch-cents") {
            if (!ParseFiniteDouble(value, &options->pitchCents)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return !options->outputPath.empty() && options->sampleRate != 0 &&
           options->blockFrames != 0 &&
           options->minCallbackFrames != 0 && options->voices != 0 &&
           options->virtualVoices <= 4096 && options->workers <= 64 &&
           options->restartCount <= 100 &&
           (options->restartCount == 0 || options->realDevice) &&
           options->pitchCents >= -4800.0 &&
           options->pitchCents <= 4800.0;
}

bool CapacityWithHeadroom(long double records,
                          std::uint64_t* capacity) {
    if (capacity == nullptr || !std::isfinite(records) ||
        records < 0.0L) {
        return false;
    }
    const long double rounded = std::ceil(records * 1.1L);
    if (!std::isfinite(rounded) ||
        rounded >
            static_cast<long double>(
                (std::numeric_limits<std::uint64_t>::max)())) {
        return false;
    }
    *capacity = static_cast<std::uint64_t>(rounded);
    return true;
}

double Percentile(const std::vector<double>& sorted, double quantile) {
    if (sorted.empty()) {
        return 0.0;
    }
    const double rank = std::ceil(quantile * sorted.size());
    const std::size_t index =
        static_cast<std::size_t>(std::max(1.0, rank)) - 1;
    return sorted[std::min(index, sorted.size() - 1)];
}

TimingSummary SummarizeTimeline(
    const harken::rtprobe::Sink& sink,
    harken::rtprobe::TimelineKind kind,
    std::uint32_t sampleRate) {
    TimingSummary summary;
    std::vector<double> durations;
    durations.reserve(sink.timelineCount);
    for (std::uint32_t i = 0; i < sink.timelineCount; ++i) {
        const harken::rtprobe::TimelineRecord& record = sink.timeline[i];
        if (record.kind != static_cast<std::uint16_t>(kind)) {
            continue;
        }
        if (sink.measurementStartTicks != 0 &&
            record.beginTicks < sink.measurementStartTicks) {
            continue;
        }
        if (sink.measurementEndTicks != 0 &&
            record.endTicks > sink.measurementEndTicks) {
            continue;
        }
        ++summary.count;
        if (sink.tickRate == 0 || record.endTicks < record.beginTicks ||
            record.frames == 0) {
            ++summary.invalidCount;
            continue;
        }
        const std::uint16_t invalidFlags =
            harken::rtprobe::kTimelineFlagWriterIdentityMismatch |
            harken::rtprobe::kTimelineFlagGenerationOverflow |
            harken::rtprobe::kTimelineFlagMixCountOverflow;
        if ((record.flags & invalidFlags) != 0) {
            ++summary.invalidCount;
            continue;
        }
        if (summary.minFrames == 0 || record.frames < summary.minFrames) {
            summary.minFrames = record.frames;
        }
        summary.maxFrames = std::max(summary.maxFrames, record.frames);
        const std::uint64_t ticks = record.endTicks - record.beginTicks;
        const double durationUs =
            static_cast<double>(ticks) * 1.0e6 /
            static_cast<double>(sink.tickRate);
        durations.push_back(durationUs);
        const long double used =
            static_cast<long double>(ticks) * sampleRate;
        const long double budget =
            static_cast<long double>(sink.tickRate) * record.frames;
        if (used > budget) {
            ++summary.deadlineMisses;
        }
    }
    std::sort(durations.begin(), durations.end());
    summary.p50Us = Percentile(durations, 0.50);
    summary.p95Us = Percentile(durations, 0.95);
    summary.p99Us = Percentile(durations, 0.99);
    summary.p999Us = Percentile(durations, 0.999);
    summary.maxUs = durations.empty() ? 0.0 : durations.back();
    return summary;
}

double ShortestCallbackPeriodUs(const harken::rtprobe::Sink& sink) {
    std::uint64_t shortest = std::numeric_limits<std::uint64_t>::max();
    for (std::uint32_t i = 0; i < sink.timelineCount; ++i) {
        const harken::rtprobe::TimelineRecord& record = sink.timeline[i];
        if (record.kind != static_cast<std::uint16_t>(
                               harken::rtprobe::TimelineKind::DeviceCallback)) {
            continue;
        }
        if ((sink.measurementStartTicks != 0 &&
             record.beginTicks < sink.measurementStartTicks) ||
            (sink.measurementEndTicks != 0 &&
             record.endTicks > sink.measurementEndTicks)) {
            continue;
        }
        if (record.priorArrivalTicks != 0 &&
            record.priorArrivalTicks >= sink.measurementStartTicks &&
            record.beginTicks > record.priorArrivalTicks) {
            shortest = std::min(
                shortest, record.beginTicks - record.priorArrivalTicks);
        }
    }
    if (sink.tickRate == 0 ||
        shortest == std::numeric_limits<std::uint64_t>::max()) {
        return 0.0;
    }
    return static_cast<double>(shortest) * 1.0e6 /
           static_cast<double>(sink.tickRate);
}

double LongestCallbackPeriodUs(const harken::rtprobe::Sink& sink) {
    std::uint64_t longest = 0;
    std::uint64_t firstBegin = 0;
    std::uint64_t lastBegin = 0;
    for (std::uint32_t i = 0; i < sink.timelineCount; ++i) {
        const harken::rtprobe::TimelineRecord& record = sink.timeline[i];
        if (record.kind != static_cast<std::uint16_t>(
                               harken::rtprobe::TimelineKind::DeviceCallback)) {
            continue;
        }
        if ((sink.measurementStartTicks != 0 &&
             record.beginTicks < sink.measurementStartTicks) ||
            (sink.measurementEndTicks != 0 &&
            record.endTicks > sink.measurementEndTicks)) {
            continue;
        }
        if (firstBegin == 0 || record.beginTicks < firstBegin) {
            firstBegin = record.beginTicks;
        }
        lastBegin = std::max(lastBegin, record.beginTicks);
        if (record.priorArrivalTicks != 0 &&
            record.priorArrivalTicks >= sink.measurementStartTicks &&
            record.beginTicks > record.priorArrivalTicks) {
            longest = std::max(
                longest, record.beginTicks - record.priorArrivalTicks);
        }
    }
    if (sink.measurementStartTicks != 0 && firstBegin != 0 &&
        firstBegin > sink.measurementStartTicks) {
        longest = std::max(
            longest, firstBegin - sink.measurementStartTicks);
    }
    if (sink.measurementEndTicks != 0 && lastBegin != 0 &&
        sink.measurementEndTicks > lastBegin) {
        longest = std::max(
            longest, sink.measurementEndTicks - lastBegin);
    }
    if (sink.tickRate == 0 || longest == 0) {
        return 0.0;
    }
    return static_cast<double>(longest) * 1.0e6 /
           static_cast<double>(sink.tickRate);
}

WorkerSummary SummarizeWorkers(const harken::rtprobe::Sink& sink) {
    WorkerSummary summary;
    std::vector<double> barriers;
    std::vector<double> firstWorkerRuns;
    barriers.reserve(sink.workerBarrierCount);
    firstWorkerRuns.reserve(sink.workerBarrierCount);
    for (std::uint32_t i = 0; i < sink.workerBarrierCount; ++i) {
        const harken::rtprobe::WorkerBarrierRecord& record =
            sink.workerBarriers[i];
        summary.identityMismatchMask |= record.identityMismatchMask;
        summary.maxYieldIterations =
            std::max(summary.maxYieldIterations, record.yieldIterations);
        if ((record.flags &
             (harken::rtprobe::kWorkerBarrierFlagWakeFault |
              harken::rtprobe::kWorkerBarrierFlagJobFault)) != 0) {
            ++summary.faultCount;
        }
        if ((record.flags &
             harken::rtprobe::kWorkerBarrierFlagSerialFallback) != 0) {
            ++summary.serialFallbackCount;
        }
        if ((record.flags &
             harken::rtprobe::kWorkerBarrierFlagIncompleteScratch) != 0) {
            ++summary.incompleteScratchCount;
        }
        if ((record.flags &
             harken::rtprobe::
                 kWorkerBarrierFlagCoordinatorIdentityMismatch) != 0) {
            ++summary.invalidCount;
        }
        if (sink.tickRate == 0 ||
            record.publishTicks == 0 ||
            record.barrierDoneTicks == 0 ||
            record.barrierDoneTicks < record.publishTicks ||
            record.coordinatorDoneTicks < record.publishTicks ||
            record.barrierDoneTicks < record.coordinatorDoneTicks ||
            record.jobsCompleted != record.jobCount ||
            record.workersFinished != record.workerCount ||
            (record.workerCount != 0 &&
              (record.firstWorkerRunTicks < record.publishTicks ||
               record.lastWorkerRunTicks <
                   record.firstWorkerRunTicks ||
               record.lastWorkerDoneTicks <
                   record.lastWorkerRunTicks ||
               record.barrierDoneTicks <
                   record.lastWorkerDoneTicks))) {
            ++summary.invalidCount;
            continue;
        }
        if ((sink.measurementStartTicks != 0 &&
             record.publishTicks < sink.measurementStartTicks) ||
            (sink.measurementEndTicks != 0 &&
             record.barrierDoneTicks > sink.measurementEndTicks)) {
            continue;
        }
        ++summary.count;
        barriers.push_back(
            static_cast<double>(record.barrierDoneTicks -
                                record.publishTicks) *
            1.0e6 / static_cast<double>(sink.tickRate));
        if (record.workerCount != 0) {
            firstWorkerRuns.push_back(
                static_cast<double>(record.firstWorkerRunTicks -
                                    record.publishTicks) *
                1.0e6 / static_cast<double>(sink.tickRate));
        }
    }
    std::sort(barriers.begin(), barriers.end());
    std::sort(firstWorkerRuns.begin(), firstWorkerRuns.end());
    summary.barrierP50Us = Percentile(barriers, 0.50);
    summary.barrierP99Us = Percentile(barriers, 0.99);
    summary.barrierMaxUs =
        barriers.empty() ? 0.0 : barriers.back();
    summary.firstWorkerRunP50Us =
        Percentile(firstWorkerRuns, 0.50);
    summary.firstWorkerRunP99Us =
        Percentile(firstWorkerRuns, 0.99);
    return summary;
}

std::uint64_t HashBytes(std::uint64_t hash, const void* data,
                         std::size_t size) {
    const auto* bytes = static_cast<const std::uint8_t*>(data);
    for (std::size_t i = 0; i < size; ++i) {
        hash ^= bytes[i];
        hash *= 1099511628211ull;
    }
    return hash;
}

void AccumulateOutputSignal(OutputSignalSummary* summary,
                            const float* samples,
                            std::size_t sampleCount) {
    if (summary == nullptr || samples == nullptr) {
        return;
    }
    for (std::size_t i = 0; i < sampleCount; ++i) {
        const float sample = samples[i];
        ++summary->sampleCount;
        if (!std::isfinite(sample)) {
            ++summary->nonFiniteSampleCount;
            continue;
        }
        if (sample != 0.0f) {
            ++summary->nonzeroSampleCount;
        }
        const float magnitude = std::abs(sample);
        summary->peakAbs = std::max(summary->peakAbs, magnitude);
        summary->sumSquares +=
            static_cast<double>(sample) * static_cast<double>(sample);
    }
}

void WriteJsonString(std::ostream& output, std::string_view text) {
    output << '"';
    for (const char c : text) {
        switch (c) {
        case '\\':
            output << "\\\\";
            break;
        case '"':
            output << "\\\"";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            output << c;
            break;
        }
    }
    output << '"';
}

void WriteTiming(std::ostream& output, const TimingSummary& summary) {
    output << "{\"count\":" << summary.count
           << ",\"invalidCount\":" << summary.invalidCount
           << ",\"deadlineMisses\":" << summary.deadlineMisses
           << ",\"minFrames\":" << summary.minFrames
           << ",\"maxFrames\":" << summary.maxFrames
           << ",\"p50Us\":" << summary.p50Us
           << ",\"p95Us\":" << summary.p95Us
           << ",\"p99Us\":" << summary.p99Us
           << ",\"p99_9Us\":" << summary.p999Us
           << ",\"maxUs\":" << summary.maxUs << '}';
}

bool InMeasurementWindow(const harken::rtprobe::Sink& sink,
                         std::uint64_t beginTicks,
                         std::uint64_t endTicks) {
    return (sink.measurementStartTicks == 0 ||
            beginTicks >= sink.measurementStartTicks) &&
           (sink.measurementEndTicks == 0 ||
            endTicks <= sink.measurementEndTicks);
}

CallbackShapeSummary SummarizeCallbackShape(
    const harken::rtprobe::Sink& sink) {
    CallbackShapeSummary summary;
    for (std::uint32_t i = 0; i < sink.timelineCount; ++i) {
        const auto& record = sink.timeline[i];
        if (record.kind != static_cast<std::uint16_t>(
                               harken::rtprobe::TimelineKind::
                                   DeviceCallback) ||
            !InMeasurementWindow(
                sink, record.beginTicks, record.endTicks)) {
            continue;
        }
        const auto readBefore =
            static_cast<std::uint32_t>(record.logicalIndex >> 32u);
        const auto availBefore =
            static_cast<std::uint32_t>(record.logicalIndex);
        const auto readAfter =
            static_cast<std::uint32_t>(record.relatedIndex >> 32u);
        const auto availAfter =
            static_cast<std::uint32_t>(record.relatedIndex);
        ++summary.frameCounts[record.frames];
        ++summary.mixBlockCounts[record.aux0];
        ++summary.stagingReadBefore[readBefore];
        ++summary.stagingAvailBefore[availBefore];
        ++summary.stagingReadAfter[readAfter];
        ++summary.stagingAvailAfter[availAfter];
    }
    return summary;
}

PublicationShapeSummary SummarizePublicationShape(
    const harken::rtprobe::Sink& sink) {
    PublicationShapeSummary summary;
    for (std::uint32_t i = 0; i < sink.timelineCount; ++i) {
        const auto& record = sink.timeline[i];
        if (record.kind != static_cast<std::uint16_t>(
                               harken::rtprobe::TimelineKind::
                                   MixPublication) ||
            !InMeasurementWindow(
                sink, record.beginTicks, record.endTicks)) {
            continue;
        }
        ++summary.count;
        if ((record.flags &
             harken::rtprobe::kTimelineFlagMeterArmed) != 0) {
            ++summary.meterArmedCount;
        }
        if ((record.flags &
             harken::rtprobe::kTimelineFlagSourceCursorPublished) != 0) {
            ++summary.sourceCursorPublishedCount;
        }
    }
    return summary;
}

void WriteHistogram(
    std::ostream& output,
    const std::map<std::uint32_t, std::uint64_t>& histogram) {
    output << '{';
    bool first = true;
    for (const auto& [value, count] : histogram) {
        if (!first) {
            output << ',';
        }
        first = false;
        output << '"' << value << "\":" << count;
    }
    output << '}';
}

bool WriteRawRecords(const Options& options,
                     const harken::rtprobe::Sink& sink,
                     RawExport* rawExport) {
    if (rawExport == nullptr) {
        return false;
    }
    rawExport->timelinePath =
        options.outputPath.string() + ".timeline.jsonl";
    rawExport->workerPath =
        options.outputPath.string() + ".workers.jsonl";

    std::ofstream timelineOutput(
        rawExport->timelinePath, std::ios::binary);
    if (!timelineOutput) {
        return false;
    }
    for (std::uint32_t i = 0; i < sink.timelineCount; ++i) {
        const auto& record = sink.timeline[i];
        timelineOutput
            << "{\"sequence\":" << record.sequence
            << ",\"kind\":" << record.kind
            << ",\"beginTicks\":" << record.beginTicks
            << ",\"endTicks\":" << record.endTicks
            << ",\"priorArrivalTicks\":"
            << record.priorArrivalTicks
            << ",\"logicalIndex\":" << record.logicalIndex
            << ",\"relatedIndex\":" << record.relatedIndex
            << ",\"writerToken\":" << record.writerToken
            << ",\"frames\":" << record.frames
            << ",\"channels\":" << record.channels
            << ",\"flags\":" << record.flags
            << ",\"aux0\":" << record.aux0
            << ",\"aux1\":" << record.aux1 << "}\n";
    }
    timelineOutput.close();
    if (!timelineOutput) {
        return false;
    }

    std::ofstream workerOutput(
        rawExport->workerPath, std::ios::binary);
    if (!workerOutput) {
        return false;
    }
    for (std::uint32_t i = 0; i < sink.workerBarrierCount; ++i) {
        const auto& record = sink.workerBarriers[i];
        workerOutput
            << "{\"sequence\":" << record.sequence
            << ",\"epoch\":" << record.epoch
            << ",\"publishTicks\":" << record.publishTicks
            << ",\"coordinatorDoneTicks\":"
            << record.coordinatorDoneTicks
            << ",\"firstWorkerRunTicks\":"
            << record.firstWorkerRunTicks
            << ",\"lastWorkerRunTicks\":"
            << record.lastWorkerRunTicks
            << ",\"lastWorkerDoneTicks\":"
            << record.lastWorkerDoneTicks
            << ",\"barrierDoneTicks\":"
            << record.barrierDoneTicks
            << ",\"coordinatorToken\":"
            << record.coordinatorToken
            << ",\"yieldIterations\":"
            << record.yieldIterations
            << ",\"identityMismatchMask\":"
            << record.identityMismatchMask
            << ",\"jobCount\":" << record.jobCount
            << ",\"jobsCompleted\":" << record.jobsCompleted
            << ",\"workerCount\":" << record.workerCount
            << ",\"workersFinished\":" << record.workersFinished
            << ",\"flags\":" << record.flags << "}\n";
    }
    workerOutput.close();
    return static_cast<bool>(workerOutput);
}

bool WriteReport(const Options& options,
                 const harken::rtprobe::Sink& sink,
                 const WallClockAnchor& measurementStartAnchor,
                 const WallClockAnchor& measurementEndAnchor,
                 std::uint64_t storageBytes,
                 std::uint64_t storageLimitBytes,
                 VnResult initResult,
                 VnResult setupResult,
                 VnResult updateResult,
                 VnResult snapshotResult,
                 VnResult termResult,
                 bool detachResult,
                 std::uint64_t outputHash,
                 const OutputSignalSummary& outputSignal,
                 std::uint64_t pumpedBlocks,
                 const VnSnapshot& snapshot,
                 const RestartSummary& restarts,
                 bool* runAccepted) {
    const TimingSummary callbacks = SummarizeTimeline(
        sink, harken::rtprobe::TimelineKind::DeviceCallback,
        options.sampleRate);
    const TimingSummary publications = SummarizeTimeline(
        sink, harken::rtprobe::TimelineKind::MixPublication,
        options.sampleRate);
    const WorkerSummary workers = SummarizeWorkers(sink);
    const CallbackShapeSummary callbackShape =
        SummarizeCallbackShape(sink);
    const PublicationShapeSummary publicationShape =
        SummarizePublicationShape(sink);
    const double measuredSeconds =
        sink.tickRate != 0 &&
                sink.measurementEndTicks >= sink.measurementStartTicks
            ? static_cast<double>(sink.measurementEndTicks -
                                  sink.measurementStartTicks) /
                  static_cast<double>(sink.tickRate)
            : 0.0;
    const double expectedCallbackPeriodUs =
        sink.backend.clientSampleRate != 0 &&
                sink.backend.internalPeriodFrames != 0
            ? static_cast<double>(sink.backend.internalPeriodFrames) *
                  1.0e6 /
                  static_cast<double>(sink.backend.clientSampleRate)
            : 0.0;
    const std::uint64_t minimumCallbackCount =
        expectedCallbackPeriodUs > 0.0
            ? static_cast<std::uint64_t>(std::floor(
                  measuredSeconds * 1.0e6 /
                  expectedCallbackPeriodUs * 0.90))
            : 0;
    const double longestCallbackPeriodUs =
        LongestCallbackPeriodUs(sink);
    const bool callbackContinuityValid =
        !options.realDevice ||
        (expectedCallbackPeriodUs > 0.0 &&
         callbacks.count >= minimumCallbackCount &&
         longestCallbackPeriodUs > 0.0 &&
         longestCallbackPeriodUs <= expectedCallbackPeriodUs * 4.0);
    const bool durationMet =
        options.seconds >= 60.0 && measuredSeconds >= options.seconds;
    const bool workerEvidenceValid =
        workers.invalidCount == 0 && workers.faultCount == 0 &&
        workers.serialFallbackCount == 0 &&
        workers.incompleteScratchCount == 0 &&
        workers.identityMismatchMask == 0;
    const bool lifecycleValid =
        initResult == VN_OK && setupResult == VN_OK &&
        updateResult == VN_OK && snapshotResult == VN_OK &&
        termResult == VN_OK && detachResult;
    const bool snapshotSemanticValid =
        snapshotResult == VN_OK &&
        snapshot.activeVoiceCount == options.voices &&
        snapshot.parkedVoiceCount == options.virtualVoices &&
        snapshot.voiceHighWater >= options.voices &&
        snapshot.parkedHighWater >= options.virtualVoices &&
        snapshot.arenaOverflowCount == 0 &&
        snapshot.meterRingOverflowCount == 0 &&
        snapshot.noVoiceCount == 0 &&
        snapshot.backpressureCount == 0 &&
        snapshot.notificationDropCount == 0 &&
        snapshot.logicNotificationDropCount == 0 &&
        snapshot.voiceStarvedCount == 0 &&
        snapshot.returnRefusedCount == 0 &&
        snapshot.pendingStartCount == 0 &&
        snapshot.renderWorkerCount == options.workers &&
        snapshot.blockIndex != 0;
    const bool restartValid =
        RestartObservationsValid(options, restarts);
    const bool outputSignalValid =
        options.realDevice ||
        (pumpedBlocks != 0 && outputSignal.sampleCount != 0 &&
         outputSignal.nonzeroSampleCount != 0 &&
         outputSignal.nonFiniteSampleCount == 0 &&
         outputSignal.peakAbs > 0.0f &&
         outputSignal.sumSquares > 0.0);
    const bool semanticValid =
        snapshotSemanticValid && outputSignalValid &&
        publicationShape.count == publications.count &&
        publicationShape.meterArmedCount == 0 &&
        publicationShape.sourceCursorPublishedCount ==
            ((options.audibilityParking || options.virtualVoices != 0)
                 ? publications.count
                 : 0);
    const bool captureValid =
        lifecycleValid && semanticValid &&
        sink.timelineOverflow == 0 &&
        sink.workerBarrierOverflow == 0 &&
        sink.identityMismatchCount == 0 &&
        sink.generationOverflowCount == 0 &&
        sink.workerCoordinatorMismatchCount == 0 &&
        sink.concurrentCallbackCount.load(
            std::memory_order_relaxed) == 0 &&
        callbacks.invalidCount == 0 &&
        publications.invalidCount == 0 && workerEvidenceValid;
    const bool deviceCoverageValid =
        !kDeviceCallbackEnabled || !options.realDevice ||
        (callbacks.count != 0 && sink.backend.valid != 0);
    const bool publicationCoverageValid =
        !kMixPublicationEnabled || options.deviceCallbackOnly ||
        publications.count != 0;
    const bool workerCoverageValid =
        !kWorkerBarrierEnabled || options.deviceCallbackOnly ||
        options.workers == 0 ||
        workers.count != 0;
    const bool coverageValid =
        deviceCoverageValid && publicationCoverageValid &&
        workerCoverageValid;
    const bool deadlineValid =
        callbacks.deadlineMisses == 0 &&
        publications.deadlineMisses == 0;
    const bool qualificationInputValid =
        captureValid && coverageValid && deadlineValid && durationMet &&
        restartValid && callbackContinuityValid;
    const bool diagnosticValid =
        captureValid && coverageValid && deadlineValid && restartValid &&
        callbackContinuityValid;
    if (runAccepted != nullptr) {
        *runAccepted =
            diagnosticValid &&
            (durationMet || options.allowShortDiagnostic);
    }

    std::error_code error;
    const std::filesystem::path parent = options.outputPath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
        if (error) {
            return false;
        }
    }
    RawExport rawExport;
    if (!WriteRawRecords(options, sink, &rawExport)) {
        return false;
    }
    std::ofstream output(options.outputPath, std::ios::binary);
    if (!output) {
        return false;
    }
    output << std::fixed << std::setprecision(6);
    output << "{\n  \"schemaVersion\":1,\n";
    output << "  \"evidenceDisposition\":";
    WriteJsonString(
        output,
        qualificationInputValid
            ? "qualification-input-complete"
            : (diagnosticValid ? "diagnostic-capture-complete"
                               : "capture-invalid"));
    output << ",\n  \"captureIntegrityStatus\":\""
           << (captureValid ? "passed" : "failed") << "\",\n";
    output << "  \"coverageStatus\":\""
           << (coverageValid ? "passed" : "failed") << "\",\n";
    output << "  \"qualificationInputStatus\":\""
           << (qualificationInputValid
                   ? "ready"
                   : (diagnosticValid ? "diagnostic-only" : "invalid"))
           << "\",\n";
    output << "  \"productionQualificationStatus\":\"blocked\",\n";
    output << "  \"xrunStatus\":\"unverified\",\n";
    output << "  \"contractDurationStatus\":\""
           << (durationMet ? "passed" : "failed") << "\",\n";
    output << "  \"allowShortDiagnostic\":"
           << (options.allowShortDiagnostic ? "true" : "false")
           << ",\n";
    output << "  \"build\":{\"buildId\":";
    WriteJsonString(output, Harken::BuildInfo::BuildId);
    output << ",\"gitCommit\":";
    WriteJsonString(output, HARKEN_RTQ_GIT_COMMIT);
    output << ",\"gitDirty\":"
           << (HARKEN_RTQ_GIT_DIRTY ? "true" : "false")
           << ",\"buildInfoGitCommit\":";
    WriteJsonString(output, Harken::BuildInfo::GitCommit);
    output << ",\"buildInfoGitDirty\":"
           << (Harken::BuildInfo::GitDirty ? "true" : "false")
           << ",\"trackedWorktreeClean\":"
           << (HARKEN_RTQ_TRACKED_WORKTREE_CLEAN ? "true" : "false")
           << ",\"untrackedEntriesPresent\":"
           << (HARKEN_RTQ_UNTRACKED_ENTRIES_PRESENT ? "true" : "false")
           << ",\"configuration\":";
    WriteJsonString(output, HARKEN_RTQ_BUILD_CONFIGURATION);
    output << ",\"generator\":";
    WriteJsonString(output, HARKEN_RTQ_CMAKE_GENERATOR);
    output << ",\"compilerId\":";
    WriteJsonString(output, HARKEN_RTQ_COMPILER_ID);
    output << ",\"compilerVersion\":";
    WriteJsonString(output, HARKEN_RTQ_COMPILER_VERSION);
    output << ",\"compilerPath\":";
    WriteJsonString(output, HARKEN_RTQ_COMPILER_PATH);
    output << ",\"generatorToolset\":";
    WriteJsonString(output, HARKEN_RTQ_GENERATOR_TOOLSET);
    output << ",\"vsToolsetVersion\":";
    WriteJsonString(output, HARKEN_RTQ_VS_TOOLSET_VERSION);
    output << ",\"windowsSdk\":";
    WriteJsonString(output, HARKEN_RTQ_WINDOWS_SDK);
#if defined(_MSC_FULL_VER)
    output << ",\"msvcFullVersion\":" << _MSC_FULL_VER;
#endif
    output << "},\n";
    output << "  \"controls\":{\"deviceCallback\":"
#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK)
           << "true"
#else
           << "false"
#endif
           << ",\"mixPublication\":"
#if defined(HARKEN_RT_PROBE_MIX_PUBLICATION)
           << "true"
#else
           << "false"
#endif
           << ",\"workerBarrier\":"
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
           << "true"
#else
           << "false"
#endif
           << "},\n";
    output << "  \"coverage\":{\"deviceCallback\":\""
           << (!kDeviceCallbackEnabled
                   ? "disabled"
                   : (!options.realDevice
                          ? "not-applicable-null-backend"
                          : (deviceCoverageValid ? "passed" : "failed")))
           << "\",\"mixPublication\":\""
           << (!kMixPublicationEnabled
                   ? "disabled"
                   : (options.deviceCallbackOnly
                          ? "disabled-by-profile"
                          : (publicationCoverageValid ? "passed" : "failed")))
           << "\",\"workerBarrier\":\""
           << (!kWorkerBarrierEnabled
                   ? "disabled"
                   : (options.deviceCallbackOnly
                          ? "disabled-by-profile"
                          : (options.workers == 0
                          ? "not-applicable-zero-workers"
                          : (workerCoverageValid ? "passed" : "failed"))))
           << "\"},\n";
    output << "  \"profile\":{\"device\":\""
           << (options.realDevice ? "real" : "null")
           << "\",\"seconds\":" << options.seconds
           << ",\"sampleRate\":" << options.sampleRate
           << ",\"blockFrames\":" << options.blockFrames
           << ",\"minCallbackFrames\":"
           << options.minCallbackFrames
           << ",\"channels\":2,\"voices\":" << options.voices
           << ",\"virtualVoices\":" << options.virtualVoices
           << ",\"workers\":" << options.workers
           << ",\"pitchCents\":" << options.pitchCents
           << ",\"restartCount\":" << options.restartCount
           << ",\"deviceCallbackOnly\":"
           << (options.deviceCallbackOnly ? "true" : "false")
           << ",\"meterVariables\":0,\"audibilityParking\":"
           << ((options.audibilityParking ||
                options.virtualVoices != 0)
                   ? "true"
                   : "false")
           << "},\n";
    output << "  \"measurement\":{\"startTicks\":"
           << sink.measurementStartTicks
           << ",\"endTicks\":" << sink.measurementEndTicks
           << ",\"tickRate\":" << sink.tickRate
           << ",\"startAnchorTicksBefore\":"
           << measurementStartAnchor.ticksBefore
           << ",\"startUnixNs\":" << measurementStartAnchor.unixNs
           << ",\"startAnchorTicksAfter\":"
           << measurementStartAnchor.ticksAfter
           << ",\"endAnchorTicksBefore\":"
           << measurementEndAnchor.ticksBefore
           << ",\"endUnixNs\":" << measurementEndAnchor.unixNs
           << ",\"endAnchorTicksAfter\":"
           << measurementEndAnchor.ticksAfter
           << ",\"wallSeconds\":" << measuredSeconds << "},\n";
    output << "  \"storage\":{\"bytes\":" << storageBytes
           << ",\"limitBytes\":" << storageLimitBytes
           << ",\"definition\":"
              "\"record-and-scratch-payload-plus-owner-objects\""
           << ",\"timelineCapacity\":" << sink.timelineCapacity
           << ",\"timelineCount\":" << sink.timelineCount
           << ",\"timelineOverflow\":" << sink.timelineOverflow
           << ",\"workerBarrierCapacity\":"
           << sink.workerBarrierCapacity
           << ",\"workerBarrierCount\":" << sink.workerBarrierCount
           << ",\"workerBarrierOverflow\":"
           << sink.workerBarrierOverflow << "},\n";
    output << "  \"rawRecords\":{\"format\":\"jsonl\","
              "\"timelinePath\":";
    WriteJsonString(output, rawExport.timelinePath.string());
    output << ",\"timelineCount\":" << sink.timelineCount
           << ",\"workerPath\":";
    WriteJsonString(output, rawExport.workerPath.string());
    output << ",\"workerCount\":" << sink.workerBarrierCount
           << "},\n";
    output << "  \"lifecycle\":{\"init\":" << initResult
           << ",\"setup\":" << setupResult
           << ",\"update\":" << updateResult
           << ",\"snapshot\":" << snapshotResult
           << ",\"term\":" << termResult
           << ",\"detach\":" << (detachResult ? "true" : "false")
           << "},\n";
    output << "  \"restart\":{\"requested\":"
           << restarts.requested
           << ",\"completed\":" << restarts.completed
           << ",\"generationBefore\":"
           << restarts.generationBefore
           << ",\"generationAfter\":"
           << restarts.generationAfter
           << ",\"suspendResult\":" << restarts.suspendResult
           << ",\"resumeResult\":" << restarts.resumeResult
           << ",\"sameBackendIdentity\":"
           << ((restarts.requested == 0 ||
                (restarts.initialBackendCaptured &&
                 SameBackendIdentity(restarts.initialBackend,
                                     restarts.finalBackend)))
                   ? "true"
                   : "false")
           << ",\"initialDeviceName\":";
    WriteJsonString(output, restarts.initialBackend.deviceName);
    output << ",\"finalDeviceName\":";
    WriteJsonString(output, restarts.finalBackend.deviceName);
    output << ",\"initialDeviceIdHash\":"
           << restarts.initialBackend.deviceIdHash
           << ",\"finalDeviceIdHash\":"
           << restarts.finalBackend.deviceIdHash;
    output << ",\"observations\":[";
    for (std::uint32_t index = 0; index < restarts.completed; ++index) {
        if (index != 0) {
            output << ',';
        }
        const RestartSummary::Observation& observation =
            restarts.observations[index];
        output << "{\"generation\":" << observation.generation
               << ",\"callbackObserved\":"
               << (observation.callbackObserved ? "true" : "false")
               << ",\"suspendBeginTicks\":"
               << observation.suspendBeginTicks
               << ",\"resumeEndTicks\":"
               << observation.resumeEndTicks
               << ",\"callbackBeginTicks\":"
               << observation.callbackBeginTicks
               << ",\"callbackRecoveredTicks\":"
               << observation.callbackRecoveredTicks
               << ",\"activeVoiceCount\":"
               << observation.activeVoices
               << ",\"parkedVoiceCount\":"
               << observation.parkedVoices
               << ",\"blockIndex\":" << observation.blockIndex
               << ",\"clientSampleRate\":"
               << observation.backend.clientSampleRate
               << ",\"deviceIdHash\":"
               << observation.backend.deviceIdHash
               << ",\"deviceName\":";
        WriteJsonString(output, observation.backend.deviceName);
        output << '}';
    }
    output << "],\"status\":\""
           << (restartValid ? "passed" : "failed") << "\"},\n";
    output << "  \"identity\":{\"mismatchCount\":"
           << sink.identityMismatchCount
           << ",\"deviceGeneration\":" << sink.deviceGeneration
           << ",\"generationOverflowCount\":"
           << sink.generationOverflowCount
           << ",\"workerCoordinatorMismatchCount\":"
           << sink.workerCoordinatorMismatchCount
           << ",\"concurrentCallbackCount\":"
           << sink.concurrentCallbackCount.load(
                  std::memory_order_relaxed)
           << "},\n";
    output << "  \"backend\":{\"valid\":" << sink.backend.valid
           << ",\"code\":" << sink.backend.backendCode
           << ",\"name\":";
    WriteJsonString(output, sink.backend.backendName);
    output << ",\"deviceName\":";
    WriteJsonString(output, sink.backend.deviceName);
    output << ",\"deviceIdHash\":" << sink.backend.deviceIdHash
           << ",\"clientSampleRate\":" << sink.backend.clientSampleRate
           << ",\"clientChannels\":" << sink.backend.clientChannels
           << ",\"clientFormat\":" << sink.backend.clientFormat
           << ",\"requestedPeriodFrames\":"
           << sink.backend.requestedPeriodFrames
           << ",\"requestedPeriods\":" << sink.backend.requestedPeriods
           << ",\"internalSampleRate\":"
           << sink.backend.internalSampleRate
           << ",\"internalChannels\":" << sink.backend.internalChannels
           << ",\"internalFormat\":" << sink.backend.internalFormat
           << ",\"internalPeriodFrames\":"
           << sink.backend.internalPeriodFrames
           << ",\"internalPeriods\":" << sink.backend.internalPeriods
           << ",\"endpointSampleRate\":"
           << sink.backend.endpointSampleRate
           << ",\"conversionObservation\":"
           << sink.backend.conversionOwner << "},\n";
    output << "  \"timingBoundary\":{"
              "\"deviceCallback\":"
              "\"Runtime DataCallback entry through post-render; excludes "
              "miniaudio/WASAPI pre/post and final probe leave\","
              "\"mixPublication\":"
              "\"before DynamicRoutes EndBlock through snapshot, meter, "
              "source-cursor publication, and block completion\","
              "\"overBudgetAuthority\":"
              "\"internal Runtime interval; not an xrun or audio-glitch "
              "authority\"},\n";
    output << "  \"callbackTiming\":";
    WriteTiming(output, callbacks);
    output << ",\n  \"mixPublicationTiming\":";
    WriteTiming(output, publications);
    output << ",\n  \"workerBarrier\":{\"count\":"
           << workers.count
           << ",\"invalidCount\":" << workers.invalidCount
           << ",\"faultCount\":" << workers.faultCount
           << ",\"serialFallbackCount\":"
           << workers.serialFallbackCount
           << ",\"incompleteScratchCount\":"
           << workers.incompleteScratchCount
           << ",\"identityMismatchMask\":"
           << workers.identityMismatchMask
           << ",\"maxYieldIterations\":"
           << workers.maxYieldIterations
           << ",\"barrierP50Us\":" << workers.barrierP50Us
           << ",\"barrierP99Us\":" << workers.barrierP99Us
           << ",\"barrierMaxUs\":" << workers.barrierMaxUs
           << ",\"firstWorkerRunP50Us\":"
           << workers.firstWorkerRunP50Us
           << ",\"firstWorkerRunP99Us\":"
           << workers.firstWorkerRunP99Us
           << ",\"latencyBoundary\":"
              "\"dispatch-to-RunBatch-entry-not-os-wake\"}";
    output << ",\n  \"shortestObservedCallbackPeriodUs\":"
           << ShortestCallbackPeriodUs(sink)
           << ",\n  \"longestObservedCallbackPeriodUs\":"
           << longestCallbackPeriodUs
           << ",\n  \"callbackContinuity\":{\"status\":\""
           << (callbackContinuityValid ? "passed" : "failed")
           << "\",\"expectedPeriodUs\":"
           << expectedCallbackPeriodUs
           << ",\"minimumCallbackCount\":"
           << minimumCallbackCount
           << ",\"measurementWindowEdgesIncluded\":true},\n";
    output << "  \"callbackShape\":{\"frameCounts\":";
    WriteHistogram(output, callbackShape.frameCounts);
    output << ",\"mixBlockCounts\":";
    WriteHistogram(output, callbackShape.mixBlockCounts);
    output << ",\"stagingReadBefore\":";
    WriteHistogram(output, callbackShape.stagingReadBefore);
    output << ",\"stagingAvailBefore\":";
    WriteHistogram(output, callbackShape.stagingAvailBefore);
    output << ",\"stagingReadAfter\":";
    WriteHistogram(output, callbackShape.stagingReadAfter);
    output << ",\"stagingAvailAfter\":";
    WriteHistogram(output, callbackShape.stagingAvailAfter);
    output << "},\n";
    output << "  \"publicationShape\":{\"count\":"
           << publicationShape.count
           << ",\"meterArmedCount\":"
           << publicationShape.meterArmedCount
           << ",\"sourceCursorPublishedCount\":"
           << publicationShape.sourceCursorPublishedCount
           << "},\n";
    output << "  \"semantic\":{\"outputHashValid\":"
           << (!options.realDevice ? "true" : "false")
           << ",\"outputHash\":";
    if (options.realDevice) {
        output << "null";
    } else {
        output << outputHash;
    }
    output << ",\"outputSignalMetricsValid\":"
           << (!options.realDevice ? "true" : "false")
           << ",\"outputPeakAbs\":";
    if (options.realDevice) {
        output << "null";
    } else {
        output << outputSignal.peakAbs;
    }
    output << ",\"outputRms\":";
    if (options.realDevice) {
        output << "null";
    } else {
        const double rms =
            outputSignal.sampleCount == 0
                ? 0.0
                : std::sqrt(
                      outputSignal.sumSquares /
                      static_cast<double>(outputSignal.sampleCount));
        output << rms;
    }
    output << ",\"outputNonzeroSampleCount\":"
           << outputSignal.nonzeroSampleCount
           << ",\"outputSampleCount\":" << outputSignal.sampleCount
           << ",\"outputNonFiniteSampleCount\":"
           << outputSignal.nonFiniteSampleCount
           << ",\"pumpedBlocks\":" << pumpedBlocks
           << ",\"snapshotBlockIndex\":" << snapshot.blockIndex
           << ",\"activeVoiceCount\":" << snapshot.activeVoiceCount
           << ",\"parkedVoiceCount\":" << snapshot.parkedVoiceCount
           << ",\"voiceHighWater\":" << snapshot.voiceHighWater
           << ",\"parkedHighWater\":" << snapshot.parkedHighWater
           << ",\"arenaOverflowCount\":"
           << snapshot.arenaOverflowCount
           << ",\"noVoiceCount\":" << snapshot.noVoiceCount
           << ",\"backpressureCount\":" << snapshot.backpressureCount
           << ",\"notificationDropCount\":"
           << snapshot.notificationDropCount
           << ",\"logicNotificationDropCount\":"
           << snapshot.logicNotificationDropCount
           << ",\"voiceStarvedCount\":"
           << snapshot.voiceStarvedCount
           << ",\"returnRefusedCount\":"
           << snapshot.returnRefusedCount
           << ",\"pendingStartCount\":"
           << snapshot.pendingStartCount
           << ",\"renderWorkerCount\":" << snapshot.renderWorkerCount
           << ",\"meterRingOverflowCount\":"
           << snapshot.meterRingOverflowCount << "}\n";
    output << "}\n";
    return static_cast<bool>(output);
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
        PrintUsage();
        return 2;
    }

    const double blocksPerSecond =
        static_cast<double>(options.sampleRate) / options.blockFrames;
    const double callbacksPerSecond =
        static_cast<double>(options.sampleRate) /
        options.minCallbackFrames;

    std::uint64_t timelineCapacity64 = 0;
    const long double captureSeconds =
        static_cast<long double>(options.seconds) + 1.0L;
#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK) || \
    defined(HARKEN_RT_PROBE_MIX_PUBLICATION)
    long double timelineRecordsPerSecond = 0.0L;
#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK)
    timelineRecordsPerSecond +=
        static_cast<long double>(callbacksPerSecond);
#endif
#if defined(HARKEN_RT_PROBE_MIX_PUBLICATION)
    if (!options.deviceCallbackOnly) {
        timelineRecordsPerSecond +=
            static_cast<long double>(blocksPerSecond);
    }
#endif
    if (!CapacityWithHeadroom(
            captureSeconds * timelineRecordsPerSecond,
            &timelineCapacity64)) {
        std::cerr << "error: invalid timeline capacity request\n";
        return 2;
    }
#endif
    std::uint64_t workerCapacity64 = 0;
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    if (!options.deviceCallbackOnly && options.workers != 0 &&
        !CapacityWithHeadroom(
            captureSeconds *
                static_cast<long double>(blocksPerSecond),
            &workerCapacity64)) {
        std::cerr << "error: invalid worker capacity request\n";
        return 2;
    }
#endif
    if (timelineCapacity64 >
            std::numeric_limits<std::uint32_t>::max() ||
        workerCapacity64 >
            std::numeric_limits<std::uint32_t>::max()) {
        std::cerr << "error: record capacity exceeds the fixed index width\n";
        return 2;
    }

    const std::uint32_t timelineCapacity =
        static_cast<std::uint32_t>(timelineCapacity64);
    const std::uint32_t workerCapacity =
        static_cast<std::uint32_t>(workerCapacity64);
    constexpr std::uint32_t workerScratchCapacity = 64;
    const std::uint64_t storageBytes =
        timelineCapacity64 * sizeof(harken::rtprobe::TimelineRecord) +
        workerCapacity64 *
            sizeof(harken::rtprobe::WorkerBarrierRecord) +
        sizeof(harken::rtprobe::Sink) +
        sizeof(std::vector<harken::rtprobe::TimelineRecord>) +
        sizeof(std::vector<harken::rtprobe::WorkerBarrierRecord>) +
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
        workerScratchCapacity *
            (sizeof(harken::rtprobe::WorkerScratch) +
             sizeof(std::uint64_t)) +
        sizeof(std::vector<harken::rtprobe::WorkerScratch>) +
        sizeof(std::vector<std::uint64_t>);
#else
        0;
#endif
    const std::uint64_t storageLimitBytes =
        options.deviceCallbackOnly
            ? kDeviceOnlyStorageLimitBytes
            : kDefaultStorageLimitBytes;
    if (storageBytes > storageLimitBytes) {
        std::cerr << "error: required probe storage " << storageBytes
                  << " exceeds the profile limit "
                  << storageLimitBytes << '\n';
        return 2;
    }

    std::vector<harken::rtprobe::TimelineRecord> timeline(
        timelineCapacity);
    std::vector<harken::rtprobe::WorkerBarrierRecord> workerBarriers(
        workerCapacity);
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    std::vector<harken::rtprobe::WorkerScratch> workerScratch(
        workerScratchCapacity);
    std::vector<std::uint64_t> workerTokens(workerScratchCapacity);
#endif
    std::fill(timeline.begin(), timeline.end(),
              harken::rtprobe::TimelineRecord{});
    std::fill(workerBarriers.begin(), workerBarriers.end(),
              harken::rtprobe::WorkerBarrierRecord{});
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    std::fill(workerScratch.begin(), workerScratch.end(),
              harken::rtprobe::WorkerScratch{});
    std::fill(workerTokens.begin(), workerTokens.end(), 0);
#endif

    harken::rtprobe::Sink sink{};
    sink.tickRate = harken::CpuClockTicksPerSecond();
    sink.timeline = timeline.empty() ? nullptr : timeline.data();
    sink.timelineCapacity = timelineCapacity;
    sink.workerBarriers =
        workerBarriers.empty() ? nullptr : workerBarriers.data();
    sink.workerBarrierCapacity = workerCapacity;
    sink.captureMask = harken::rtprobe::kCaptureDeviceCallback;
    if (!options.deviceCallbackOnly) {
        sink.captureMask |= harken::rtprobe::kCaptureMixPublication;
    }
    if (!options.deviceCallbackOnly && options.workers != 0) {
        sink.captureMask |= harken::rtprobe::kCaptureWorkerBarrier;
    }
#if defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    sink.workerScratch = workerScratch.data();
    sink.workerTokens = workerTokens.data();
    sink.workerScratchCapacity = workerScratchCapacity;
#endif

    bool attached = false;
#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK) || \
    defined(HARKEN_RT_PROBE_MIX_PUBLICATION) || \
    defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    attached = harken::rtprobe::Attach(&sink);
    if (!attached) {
        std::cerr << "error: probe sink attachment was refused\n";
        return 3;
    }
#endif

    const std::size_t sourceFrames =
        static_cast<std::size_t>(options.blockFrames) * 4u;
    std::vector<float> source(sourceFrames * 2u);
    std::uint32_t noiseState = 0x9e3779b9u;
    for (std::size_t frame = 0; frame < sourceFrames; ++frame) {
        noiseState = noiseState * 1664525u + 1013904223u;
        const float sample =
            (static_cast<float>((noiseState >> 8u) & 0xffffu) /
                 32767.5f -
             1.0f) *
            0.01f;
        source[frame * 2u] = sample;
        source[frame * 2u + 1u] = -sample;
    }

    VnConfig config{};
    config.structSize = sizeof(config);
    config.deviceMode =
        options.realDevice ? VN_DEVICE_REAL : VN_DEVICE_NULL;
    config.sampleRate = options.sampleRate;
    config.blockFrames = options.blockFrames;
    config.maxVoices = options.voices;
    config.maxParkedVoices = options.virtualVoices;
    config.fxSlotsPerVoice = 1;
    config.arenaBytes = 1u << 20;
    config.commandQueueDepth = 512;
    config.mixChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    config.deviceChannelLayout = VN_CHANNEL_LAYOUT_STEREO;
    config.renderWorkerCount = options.workers;
    config.audibilityParkEnabled =
        (options.audibilityParking || options.virtualVoices != 0) ? 1u
                                                                  : 0u;
    if (options.virtualVoices != 0) {
        config.parkThresholdDb = -70.0f;
    }

    const VnResult initResult = Vn_Init(&config);
    VnResult setupResult = initResult;
    VnResult updateResult = initResult;
    VnResult snapshotResult = initResult;
    VnResult termResult = VN_NOT_INITIALIZED;
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(snapshot);
    std::uint64_t outputHash = 1469598103934665603ull;
    OutputSignalSummary outputSignal;
    std::uint64_t pumpedBlocks = 0;
    WallClockAnchor measurementStartAnchor;
    WallClockAnchor measurementEndAnchor;
    RestartSummary restarts;
    restarts.requested = options.restartCount;
    const float pitchRatio = static_cast<float>(
        std::exp2(options.pitchCents / 1200.0));

    if (initResult == VN_OK) {
        VnPCMDesc pcm{};
        pcm.structSize = sizeof(pcm);
        pcm.sampleRate = options.sampleRate;
        pcm.channels = 2;
        pcm.layout = VN_PCM_LAYOUT_INTERLEAVED;
        pcm.policy = VN_PCM_POLICY_COPY;
        pcm.frames = sourceFrames;
        pcm.samplesInterleaved = source.data();
        setupResult = Vn_RegisterPCM(kQualificationSound, &pcm);
        if (setupResult == VN_OK && options.virtualVoices != 0) {
            std::uint32_t parkedTarget = 0;
            std::uint32_t remaining = options.virtualVoices;
            while (remaining != 0 && setupResult == VN_OK) {
                const std::uint32_t batch =
                    std::min(options.voices, remaining);
                for (std::uint32_t voice = 0; voice < batch; ++voice) {
                    VnPlayDesc play{};
                    play.structSize = sizeof(play);
                    play.soundId = kQualificationSound;
                    play.busId = VN_BUS_MASTER;
                    play.volumeDb = -100.0f;
                    play.pitchRatio = pitchRatio;
                    play.loopCount = 0;
                    VnVoiceHandle handle = 0;
                    setupResult =
                        harken::detail::PlayVoiceRaw(&play, &handle);
                    if (setupResult != VN_OK || handle == 0) {
                        if (setupResult == VN_OK) {
                            setupResult = VN_NO_VOICE;
                        }
                        break;
                    }
                }
                if (setupResult != VN_OK) {
                    break;
                }
                setupResult = Vn_Update();
                parkedTarget += batch;
                const auto parkDeadline =
                    std::chrono::steady_clock::now() +
                    std::chrono::seconds(3);
                while (setupResult == VN_OK &&
                       std::chrono::steady_clock::now() < parkDeadline) {
                    VnSnapshot setupSnapshot{};
                    setupSnapshot.structSize = sizeof(setupSnapshot);
                    setupResult = Vn_Update();
                    if (setupResult == VN_OK &&
                        Vn_CaptureSnapshot(&setupSnapshot) == VN_OK &&
                        setupSnapshot.parkedVoiceCount >= parkedTarget) {
                        break;
                    }
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(2));
                }
                if (setupResult == VN_OK) {
                    VnSnapshot setupSnapshot{};
                    setupSnapshot.structSize = sizeof(setupSnapshot);
                    setupResult = Vn_CaptureSnapshot(&setupSnapshot);
                    if (setupResult == VN_OK &&
                        setupSnapshot.parkedVoiceCount < parkedTarget) {
                        setupResult = VN_NO_VOICE;
                    }
                }
                remaining -= batch;
            }
        }
        if (setupResult == VN_OK) {
            for (std::uint32_t voice = 0; voice < options.voices; ++voice) {
                VnPlayDesc play{};
                play.structSize = sizeof(play);
                play.soundId = kQualificationSound;
                play.busId = VN_BUS_MASTER;
                play.volumeDb = -60.0f;
                play.pitchRatio = pitchRatio;
                play.loopCount = 0;
                VnVoiceHandle handle = 0;
                setupResult = harken::detail::PlayVoiceRaw(&play, &handle);
                if (setupResult != VN_OK) {
                    break;
                }
            }
        }
        if (setupResult == VN_OK) {
            updateResult = Vn_Update();
        }

        if (setupResult == VN_OK && updateResult == VN_OK) {
            if (options.realDevice) {
                const auto readyDeadline =
                    std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(250);
                while (std::chrono::steady_clock::now() <
                       readyDeadline) {
                    updateResult = Vn_Update();
                    if (updateResult != VN_OK) {
                        break;
                    }
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(2));
                }
            } else {
                harken::Engine* engine = harken::detail::ActiveEngine();
                if (engine == nullptr) {
                    updateResult = VN_NOT_INITIALIZED;
                } else {
                    const auto warmupBlockDuration =
                        std::chrono::duration<double>(
                            static_cast<double>(options.blockFrames) /
                            options.sampleRate);
                    for (std::uint32_t block = 0;
                         block < 8 && updateResult == VN_OK; ++block) {
                        engine->nullBackend.Pump(1);
                        updateResult = Vn_Update();
                        std::this_thread::sleep_for(
                            warmupBlockDuration);
                    }
                }
            }
        }

        if (setupResult == VN_OK && updateResult == VN_OK) {
            if (options.realDevice && sink.backend.valid != 0) {
                restarts.initialBackend = sink.backend;
                restarts.initialBackendCaptured = true;
                restarts.generationBefore = sink.deviceGeneration;
            }
            measurementStartAnchor = CaptureWallClockAnchor();
            sink.measurementStartTicks =
                measurementStartAnchor.ticksAfter;
            if (options.realDevice) {
                const auto wallStart = std::chrono::steady_clock::now();
                const auto deadline = wallStart +
                    std::chrono::duration<double>(options.seconds);
                auto nextProgress = wallStart + std::chrono::seconds(1);
                while (std::chrono::steady_clock::now() < deadline) {
                    const auto now = std::chrono::steady_clock::now();
                    while (restarts.completed < restarts.requested) {
                        const double fraction =
                            static_cast<double>(restarts.completed + 1u) /
                            static_cast<double>(restarts.requested + 1u);
                        const auto restartAt =
                            wallStart + std::chrono::duration<double>(
                                            options.seconds * fraction);
                        if (now < restartAt) {
                            break;
                        }
                        VnSnapshot beforeRestart{};
                        beforeRestart.structSize = sizeof(beforeRestart);
                        RestartSummary::Observation& observation =
                            restarts.observations[restarts.completed];
                        observation.suspendBeginTicks =
                            harken::CpuClockTicks();
                        restarts.suspendResult = Vn_Suspend();
                        if (restarts.suspendResult != VN_OK) {
                            updateResult = restarts.suspendResult;
                            break;
                        }
                        if (Vn_CaptureSnapshot(&beforeRestart) != VN_OK) {
                            updateResult = VN_DEVICE_ERROR;
                            break;
                        }
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(20));
                        restarts.resumeResult = Vn_Resume();
                        observation.resumeEndTicks =
                            harken::CpuClockTicks();
                        if (restarts.resumeResult != VN_OK) {
                            updateResult = restarts.resumeResult;
                            break;
                        }
                        observation.generation = sink.deviceGeneration;
                        observation.backend = sink.backend;
                        const auto callbackDeadline =
                            std::chrono::steady_clock::now() +
                            std::chrono::seconds(2);
                        while (std::chrono::steady_clock::now() <
                               callbackDeadline) {
                            updateResult = Vn_Update();
                            if (updateResult != VN_OK) {
                                break;
                            }
                            const std::uint64_t callbackEndTicks =
                                sink.generationFirstCallbackEndTicks.load(
                                    std::memory_order_acquire);
                            if (callbackEndTicks == 0) {
                                std::this_thread::sleep_for(
                                    std::chrono::milliseconds(2));
                                continue;
                            }
                            VnSnapshot afterRestart{};
                            afterRestart.structSize = sizeof(afterRestart);
                            if (Vn_CaptureSnapshot(&afterRestart) == VN_OK &&
                                afterRestart.blockIndex >
                                    beforeRestart.blockIndex) {
                                observation.callbackObserved = true;
                                observation.activeVoices =
                                    afterRestart.activeVoiceCount;
                                observation.parkedVoices =
                                    afterRestart.parkedVoiceCount;
                                observation.blockIndex =
                                    afterRestart.blockIndex;
                                observation.callbackBeginTicks =
                                    sink.generationFirstCallbackBeginTicks.load(
                                        std::memory_order_relaxed);
                                observation.callbackRecoveredTicks =
                                    callbackEndTicks;
                                break;
                            }
                            std::this_thread::sleep_for(
                                std::chrono::milliseconds(2));
                        }
                        if (updateResult != VN_OK ||
                            !observation.callbackObserved ||
                            observation.generation !=
                                restarts.generationBefore +
                                    restarts.completed + 1u ||
                            !SameBackendIdentity(
                                restarts.initialBackend,
                                observation.backend) ||
                            observation.activeVoices != options.voices ||
                            observation.parkedVoices !=
                                options.virtualVoices) {
                            updateResult = VN_DEVICE_ERROR;
                            break;
                        }
                        ++restarts.completed;
                    }
                    if (updateResult != VN_OK) {
                        break;
                    }
                    updateResult = Vn_Update();
                    if (updateResult != VN_OK) {
                        break;
                    }
                    if (now >= nextProgress) {
                        const double elapsed =
                            std::chrono::duration<double>(now - wallStart)
                                .count();
                        std::cout << "progress "
                                  << std::min(elapsed, options.seconds)
                                  << "/" << options.seconds << " seconds"
                                  << std::endl;
                        do {
                            nextProgress += std::chrono::seconds(1);
                        } while (nextProgress <= now);
                    }
                    std::this_thread::sleep_for(
                        std::chrono::milliseconds(2));
                }
            } else {
                harken::Engine* engine = harken::detail::ActiveEngine();
                if (engine == nullptr) {
                    updateResult = VN_NOT_INITIALIZED;
                } else {
                    const auto wallStart =
                        std::chrono::steady_clock::now();
                    const auto wallDeadline =
                        wallStart +
                        std::chrono::duration<double>(options.seconds);
                    const double blockSeconds =
                        static_cast<double>(options.blockFrames) /
                        options.sampleRate;
                    const std::uint64_t blockCount =
                        static_cast<std::uint64_t>(std::ceil(
                            options.seconds * blocksPerSecond));
                    for (std::uint64_t block = 0; block < blockCount;
                         ++block) {
                        const auto blockDeadline =
                            wallStart +
                            std::chrono::duration<double>(
                                block * blockSeconds);
                        std::this_thread::sleep_until(blockDeadline);
                        engine->nullBackend.Pump(1);
                        ++pumpedBlocks;
                        outputHash = HashBytes(
                            outputHash,
                            engine->nullBackend.LastBlock(),
                            static_cast<std::size_t>(options.blockFrames) *
                                 2u * sizeof(float));
                        AccumulateOutputSignal(
                            &outputSignal,
                            engine->nullBackend.LastBlock(),
                            static_cast<std::size_t>(options.blockFrames) *
                                2u);
                        if ((block & 63u) == 63u) {
                            updateResult = Vn_Update();
                            if (updateResult != VN_OK) {
                                break;
                            }
                        }
                    }
                    std::this_thread::sleep_until(wallDeadline);
                }
            }
            sink.measurementEndTicks = harken::CpuClockTicks();
            measurementEndAnchor = CaptureWallClockAnchor();
        }
        snapshotResult = Vn_CaptureSnapshot(&snapshot);
        termResult = Vn_Term();
        restarts.generationAfter = sink.deviceGeneration;
        restarts.finalBackend = sink.backend;
    }

    bool detachResult = !attached;
#if defined(HARKEN_RT_PROBE_DEVICE_CALLBACK) || \
    defined(HARKEN_RT_PROBE_MIX_PUBLICATION) || \
    defined(HARKEN_RT_PROBE_WORKER_BARRIER)
    detachResult = harken::rtprobe::Detach(&sink);
#endif

    bool runAccepted = false;
    if (!WriteReport(options, sink, measurementStartAnchor,
                     measurementEndAnchor, storageBytes, storageLimitBytes,
                     initResult,
                     setupResult, updateResult, snapshotResult,
                     termResult, detachResult, outputHash,
                     outputSignal, pumpedBlocks, snapshot, restarts,
                     &runAccepted)) {
        std::cerr << "error: cannot write report '"
                  << options.outputPath.string() << "'\n";
        return 4;
    }
    return runAccepted ? 0 : 3;
}
