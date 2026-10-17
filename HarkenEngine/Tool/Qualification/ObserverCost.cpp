#include <harken_buildinfo.h>

#include HARKEN_RTQ_SOURCE_IDENTITY_HEADER
#include "harken/core/CpuClock.h"
#include "harken/core/RtProbe.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <string>
#include <string_view>
#include <vector>

namespace {

constexpr std::uint32_t kDefaultIterations = 200000;
constexpr std::uint32_t kWarmupRecords = 1000;
constexpr std::uint64_t kStorageLimitBytes = 16ull * 1024ull * 1024ull;

struct Options {
    std::uint32_t iterations = kDefaultIterations;
    std::filesystem::path outputPath;
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
        if (parsed >
            (std::numeric_limits<std::uint32_t>::max)()) {
            return false;
        }
    }
    *value = static_cast<std::uint32_t>(parsed);
    return true;
}

bool ParseOptions(int argc, char** argv, Options* options) {
    if (options == nullptr) {
        return false;
    }
    for (int i = 1; i < argc; ++i) {
        if (i + 1 >= argc) {
            return false;
        }
        const std::string_view name(argv[i]);
        const std::string_view value(argv[++i]);
        if (name == "--output") {
            options->outputPath =
                std::filesystem::path(std::string(value));
        } else if (name == "--iterations") {
            if (!ParseUnsigned(value, &options->iterations)) {
                return false;
            }
        } else {
            return false;
        }
    }
    return !options->outputPath.empty() &&
           options->iterations > kWarmupRecords;
}

double Percentile(const std::vector<double>& sorted, double quantile) {
    if (sorted.empty()) {
        return 0.0;
    }
    const std::size_t index =
        static_cast<std::size_t>(
            std::ceil(quantile * sorted.size())) -
        1;
    return sorted[std::min(index, sorted.size() - 1)];
}

void WriteJsonString(std::ostream& output, std::string_view text) {
    output << '"';
    for (char c : text) {
        if (c == '\\' || c == '"') {
            output << '\\';
        }
        output << c;
    }
    output << '"';
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
        std::cerr
            << "usage: harken_rt_observer_cost --output <report.json> "
               "[--iterations 200000]\n";
        return 2;
    }

    const std::uint64_t storageBytes =
        static_cast<std::uint64_t>(options.iterations) *
            sizeof(harken::rtprobe::TimelineRecord) +
        sizeof(harken::rtprobe::WorkerBarrierRecord) +
        sizeof(harken::rtprobe::WorkerScratch) +
        sizeof(std::uint64_t) +
        sizeof(harken::rtprobe::Sink) +
        sizeof(std::vector<harken::rtprobe::TimelineRecord>);
    if (storageBytes > kStorageLimitBytes) {
        std::cerr << "error: observer record storage exceeds 16 MiB\n";
        return 2;
    }

    std::vector<harken::rtprobe::TimelineRecord> records(
        options.iterations);
    std::fill(records.begin(), records.end(),
              harken::rtprobe::TimelineRecord{});

    harken::rtprobe::Sink sink{};
    harken::rtprobe::WorkerBarrierRecord workerBarrier{};
    harken::rtprobe::WorkerScratch workerScratch{};
    std::uint64_t workerToken = 0;
    sink.timeline = records.data();
    sink.timelineCapacity = options.iterations;
    sink.workerBarriers = &workerBarrier;
    sink.workerBarrierCapacity = 1;
    sink.workerScratch = &workerScratch;
    sink.workerTokens = &workerToken;
    sink.workerScratchCapacity = 1;
    if (!harken::rtprobe::Attach(&sink)) {
        std::cerr << "error: probe sink attachment was refused\n";
        return 3;
    }

    harken::rtprobe::BeginDeviceStartAttempt();
    sink.measurementStartTicks = harken::CpuClockTicks();
    for (std::uint32_t i = 0; i < options.iterations; ++i) {
        const std::uint64_t begin = harken::CpuClockTicks();
        if (!harken::rtprobe::EnterDeviceCallback()) {
            break;
        }
        const bool generationFirst =
            harken::rtprobe::RecordGenerationCallbackBegin(begin);
        harken::rtprobe::TimelineRecord* record =
            harken::rtprobe::ReserveTimeline(
                harken::rtprobe::TimelineKind::DeviceCallback);
        if (record == nullptr) {
            harken::rtprobe::LeaveDeviceCallback();
            break;
        }
        harken::rtprobe::ValidateCoordinator(*record);
        harken::rtprobe::RecordCallbackArrival(*record, begin);
        record->frames = 1;
        record->channels = 2;
        record->logicalIndex = i;
        record->relatedIndex =
            harken::rtprobe::PackStaging(i, options.iterations - i);
        record->aux0 = 1;
        record->flags |= harken::rtprobe::kTimelineFlagNone;
        record->beginTicks = begin;
        const std::uint64_t end = harken::CpuClockTicks();
        record->endTicks = end;
        harken::rtprobe::RecordGenerationCallbackEnd(
            generationFirst, end);
        harken::rtprobe::LeaveDeviceCallback();
    }
    sink.measurementEndTicks = harken::CpuClockTicks();
    const bool detached = harken::rtprobe::Detach(&sink);

    std::vector<double> costs;
    if (sink.tickRate != 0 && sink.timelineCount > kWarmupRecords) {
        costs.reserve(sink.timelineCount - kWarmupRecords);
        for (std::uint32_t i = kWarmupRecords;
             i < sink.timelineCount; ++i) {
            const auto& record = records[i];
            if (record.endTicks >= record.beginTicks) {
                costs.push_back(
                    static_cast<double>(record.endTicks -
                                        record.beginTicks) *
                    1.0e6 / static_cast<double>(sink.tickRate));
            }
        }
    }
    std::sort(costs.begin(), costs.end());
    const double p50 = Percentile(costs, 0.50);
    const double p99 = Percentile(costs, 0.99);
    const double p999 = Percentile(costs, 0.999);
    const double maximum = costs.empty() ? 0.0 : costs.back();

    std::error_code error;
    const auto parent = options.outputPath.parent_path();
    if (!parent.empty()) {
        std::filesystem::create_directories(parent, error);
    }
    if (error) {
        std::cerr << "error: cannot create report directory\n";
        return 4;
    }
    std::ofstream output(options.outputPath, std::ios::binary);
    if (!output) {
        std::cerr << "error: cannot open report\n";
        return 4;
    }
    output << std::fixed << std::setprecision(6);
    output << "{\"schemaVersion\":1,\"scope\":"
              "\"qualification-process-thread-observer-primitive\",";
    output << "\"claimBoundary\":"
              "\"QPC-plus-callback-ownership-and-fixed-slot-cost-"
              "not-total-shipping-delta\",";
    output << "\"build\":{\"buildId\":";
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
           << ",\"configuration\":\""
#if defined(NDEBUG)
           << "Release"
#else
           << "Debug"
#endif
           << "\"";
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
    output << "},\"iterations\":" << options.iterations
           << ",\"warmupRecords\":" << kWarmupRecords
           << ",\"measuredRecords\":" << costs.size()
           << ",\"storageBytes\":" << storageBytes
           << ",\"storageLimitBytes\":" << kStorageLimitBytes
           << ",\"timelineCount\":" << sink.timelineCount
           << ",\"timelineOverflow\":" << sink.timelineOverflow
           << ",\"identityMismatchCount\":"
           << sink.identityMismatchCount
           << ",\"concurrentCallbackCount\":"
           << sink.concurrentCallbackCount.load(
                  std::memory_order_relaxed)
           << ",\"detached\":" << (detached ? "true" : "false")
           << ",\"p50Us\":" << p50
           << ",\"p99Us\":" << p99
           << ",\"p99_9Us\":" << p999
           << ",\"maxUs\":" << maximum << "}\n";
    if (!output) {
        return 4;
    }
    return detached && sink.timelineOverflow == 0 &&
                   sink.identityMismatchCount == 0 &&
                   sink.concurrentCallbackCount.load(
                       std::memory_order_relaxed) == 0 &&
                   costs.size() ==
                       options.iterations - kWarmupRecords
               ? 0
               : 3;
}
