#include "PackPlay.h"
#include "PackCompanion.h"

#include "Engine.h"
#include "PackPlaySelection.h"
#include "PackPlayInitFailure.h"
#include "RealDeviceInitStage.h"
#include "harken/core/AllocGuard.h"

#include <harken/Harken.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <thread>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <process.h>
#else
#include <unistd.h>
#endif

namespace harkenhost {
namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kBlockFrames = 256;
constexpr float kSilenceFloor = 1.0e-4f;

struct Options {
    std::string root;
    std::string actionName;
    std::string language;
    double seconds = 2.0;
    std::uint32_t repeats = 1;
    std::uint32_t startDelayMs = 0;
    std::string captureReadyGate;
    std::uint32_t captureReadyTimeoutMs = 10000;
    std::uint64_t callerSeed = 0;
    VnObjectId variableId{};
    std::vector<float> variableValues;
    bool nullMode = false;
    bool skipRecovery = false;
};

std::string ReadFileText(const std::filesystem::path& path);

bool ParseUnsigned64(const char* text, std::uint64_t* out) {
    if (text == nullptr || out == nullptr) {
        return false;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0' || value == 0) {
        return false;
    }
    *out = static_cast<std::uint64_t>(value);
    return true;
}

bool ParseRepeats(const char* text, std::uint32_t* out) {
    std::uint64_t value = 0;
    if (!ParseUnsigned64(text, &value) || value > 32) {
        return false;
    }
    *out = static_cast<std::uint32_t>(value);
    return true;
}

bool ParseFloatList(const char* text, std::vector<float>* out) {
    if (text == nullptr || out == nullptr || *text == '\0') {
        return false;
    }
    const std::string values = text;
    std::size_t cursor = 0;
    while (cursor < values.size()) {
        const std::size_t comma = values.find(',', cursor);
        const std::string token = values.substr(cursor, comma - cursor);
        char* end = nullptr;
        const float value = std::strtof(token.c_str(), &end);
        if (end == token.c_str() || *end != '\0' || !std::isfinite(value)) {
            return false;
        }
        out->push_back(value);
        if (comma == std::string::npos) {
            break;
        }
        cursor = comma + 1;
        if (cursor == values.size()) {
            return false;
        }
    }
    return !out->empty();
}

bool ParseOptions(int argc, char** argv, Options* out) {
    for (int i = 2; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--null") {
            out->nullMode = true;
        } else if (arg == "--root" && i + 1 < argc) {
            out->root = argv[++i];
        } else if (arg == "--language" && i + 1 < argc) {
            out->language = argv[++i];
            if (!PackCompanionLanguage(out->language)) return false;
        } else if (arg == "--action-name" && i + 1 < argc) {
            out->actionName = argv[++i];
            if (!PackCompanionName(out->actionName) || out->actionName == "Init") return false;
        } else if (arg == "--seconds" && i + 1 < argc) {
            char* end = nullptr;
            const double seconds = std::strtod(argv[++i], &end);
            if (end == argv[i] || *end != '\0' || !(seconds > 0.0) ||
                !std::isfinite(seconds) || seconds > 60.0) {
                return false;
            }
            out->seconds = seconds;
        } else if (arg == "--repeats" && i + 1 < argc) {
            if (!ParseRepeats(argv[++i], &out->repeats)) {
                return false;
            }
        } else if (arg == "--start-delay-ms" && i + 1 < argc) {
            std::uint64_t delay = 0;
            if (!ParseUnsigned64(argv[++i], &delay) || delay > 10000) {
                return false;
            }
            out->startDelayMs = static_cast<std::uint32_t>(delay);
        } else if (arg == "--capture-ready-gate" && i + 1 < argc) {
            out->captureReadyGate = argv[++i];
            if (out->captureReadyGate.empty()) {
                return false;
            }
        } else if (arg == "--capture-ready-timeout-ms" && i + 1 < argc) {
            std::uint64_t timeout = 0;
            if (!ParseUnsigned64(argv[++i], &timeout) || timeout > 60000) {
                return false;
            }
            out->captureReadyTimeoutMs = static_cast<std::uint32_t>(timeout);
        } else if (arg == "--caller-seed" && i + 1 < argc) {
            if (!ParseUnsigned64(argv[++i], &out->callerSeed)) {
                return false;
            }
        } else if (arg == "--variable-id" && i + 1 < argc) {
            if (!harken::ParseObjectId(argv[++i], &out->variableId)) {
                return false;
            }
        } else if (arg == "--variable-values" && i + 1 < argc) {
            if (!ParseFloatList(argv[++i], &out->variableValues)) {
                return false;
            }
        } else if (arg == "--no-recovery") {
            out->skipRecovery = true;
        } else {
            return false;
        }
    }
    const bool variableOptionsValid = out->variableValues.empty()
        ? harken::IsNull(out->variableId)
        : !harken::IsNull(out->variableId) &&
              out->variableValues.size() == out->repeats;
    return !out->root.empty() && !out->actionName.empty() && variableOptionsValid;
}

std::uint64_t CurrentProcessId() {
#if defined(_WIN32)
    return static_cast<std::uint64_t>(_getpid());
#else
    return static_cast<std::uint64_t>(getpid());
#endif
}

bool CaptureReadyGateExists(const std::filesystem::path& path,
                            bool* exists) {
    std::error_code ec;
    *exists = std::filesystem::exists(path, ec);
    return !ec;
}

bool WaitForCaptureReadyGate(const Options& options) {
    if (options.captureReadyGate.empty()) {
        return true;
    }
    const std::filesystem::path gatePath(options.captureReadyGate);
    const std::string expected =
        "process_id=" + std::to_string(CurrentProcessId()) + "\n";
    std::printf("pack-play capture-ready waiting pid=%llu\n",
                static_cast<unsigned long long>(CurrentProcessId()));
    std::fflush(stdout);
    const auto deadline = std::chrono::steady_clock::now() +
        std::chrono::milliseconds(options.captureReadyTimeoutMs);
    while (std::chrono::steady_clock::now() < deadline) {
        bool exists = false;
        if (!CaptureReadyGateExists(gatePath, &exists)) {
            std::fprintf(stderr,
                         "error: cannot inspect capture-ready gate '%s'\n",
                         options.captureReadyGate.c_str());
            return false;
        }
        if (exists) {
            const std::string marker = ReadFileText(gatePath);
            if (marker != expected) {
                std::fprintf(stderr,
                             "error: capture-ready gate PID mismatch\n");
                return false;
            }
            std::printf("pack-play capture-ready pid=%llu\n",
                        static_cast<unsigned long long>(CurrentProcessId()));
            std::fflush(stdout);
            return true;
        }
        if (Vn_Update() != VN_OK) {
            std::fprintf(stderr,
                         "error: Vn_Update failed while waiting for capture\n");
            return false;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    std::fprintf(stderr, "error: capture-ready gate timed out after %u ms\n",
                 options.captureReadyTimeoutMs);
    return false;
}

std::string ReadFileText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

std::string Utf8Path(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

struct RenderCursor {
    bool hasSnapshot = false;
    std::uint64_t snapshotBlockIndex = 0;
};

RenderCursor CaptureRenderCursor(harken::Engine* engine) {
    RenderCursor cursor;
    if (engine == nullptr) {
        return cursor;
    }
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(snapshot);
    if (Vn_CaptureSnapshot(&snapshot) == VN_OK) {
        cursor.hasSnapshot = true;
        cursor.snapshotBlockIndex = snapshot.blockIndex;
    }
    return cursor;
}

class ControlServiceWait {
public:
    explicit ControlServiceWait(bool enabled) {
#if defined(_WIN32)
        if (enabled) {
            // Sleep can exceed the arm lead on Windows' default timer tick.
            timer_ = CreateWaitableTimerExW(
                nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION,
                TIMER_MODIFY_STATE | SYNCHRONIZE);
            ready_ = timer_ != nullptr;
            if (!ready_) {
                std::fprintf(stderr,
                    "error: pack-play control timer creation failed (%lu)\n",
                    GetLastError());
            }
        }
#else
        (void)enabled;
#endif
    }
    ~ControlServiceWait() {
#if defined(_WIN32)
        if (timer_ != nullptr) CloseHandle(timer_);
#endif
    }
    ControlServiceWait(const ControlServiceWait&) = delete;
    ControlServiceWait& operator=(const ControlServiceWait&) = delete;

    bool Ready() const { return ready_; }
    bool Wait() {
#if defined(_WIN32)
        LARGE_INTEGER due{};
        due.QuadPart = -10000;
        if (!SetWaitableTimer(timer_, &due, 0, nullptr, nullptr, FALSE)) {
            std::fprintf(stderr,
                "error: pack-play control timer scheduling failed (%lu)\n",
                GetLastError());
            return false;
        }
        if (WaitForSingleObject(timer_, INFINITE) != WAIT_OBJECT_0) {
            std::fprintf(stderr,
                "error: pack-play control timer wait failed (%lu)\n",
                GetLastError());
            return false;
        }
#else
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
#endif
        return true;
    }

private:
    bool ready_ = true;
#if defined(_WIN32)
    HANDLE timer_ = nullptr;
#endif
};

bool CheckActionFiring(VnActionFiringHandle firing) {
    VnActionFiringInfo info{};
    info.structSize = sizeof(info);
    const VnResult result = Vn_QueryActionFiring(firing, &info);
    if (result != VN_OK) {
        std::fprintf(stderr,
            "error: pack-play firing=%llu query failed result=%d\n",
            static_cast<unsigned long long>(firing), result);
        return false;
    }
    if (info.state == VN_ACTION_FIRING_TERMINAL_RETAINED &&
        info.outcome != VN_ACTION_OUTCOME_SUCCEEDED) {
        std::fprintf(stderr,
            "error: pack-play firing=%llu terminated state=%u outcome=%u reason=%u\n",
            static_cast<unsigned long long>(firing), info.state, info.outcome,
            info.terminalReason);
        for (std::uint32_t index = 0; index < info.directStepCount; ++index) {
            VnActionStepResult step{};
            step.structSize = sizeof(step);
            const VnResult queried = Vn_QueryActionStep(firing, index, &step);
            if (queried != VN_OK) {
                std::fprintf(stderr,
                    "error: pack-play step=%u query failed result=%d\n",
                    index, queried);
                break;
            }
            if (step.state == VN_ACTION_STEP_FAILED) {
                std::fprintf(stderr,
                    "error: pack-play step=%u failed result=%d\n",
                    index, step.result);
                break;
            }
        }
        return false;
    }
    return true;
}

bool WaitForAudiblePeak(harken::Engine* engine, const Options& options,
                        VnActionFiringHandle firing, float* outPeak,
                        const RenderCursor* afterStop) {
    if (engine == nullptr) {
        return false;
    }
    if (!options.nullMode &&
        (afterStop == nullptr || !afterStop->hasSnapshot)) {
        return false;
    }
    ControlServiceWait serviceWait(!options.nullMode);
    if (!serviceWait.Ready()) return false;
    float maxPeak = 0.0f;
    const std::uint32_t nullBlocks = std::max(
        1u, static_cast<std::uint32_t>(
                std::ceil(options.seconds * kSampleRate / kBlockFrames)));
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(
                              static_cast<int>(options.seconds * 1000.0));
    std::uint32_t nullBlock = 0;
    while (options.nullMode ? nullBlock < nullBlocks
                            : std::chrono::steady_clock::now() < deadline) {
        if (options.nullMode) {
            engine->nullBackend.Pump(1);
        }
        const VnResult updated = Vn_Update();
        if (updated != VN_OK) {
            std::fprintf(stderr,
                "error: pack-play Vn_Update failed result=%d\n", updated);
            if (outPeak != nullptr) {
                *outPeak = maxPeak;
            }
            return false;
        }
        if (!CheckActionFiring(firing)) return false;
        if (options.nullMode) {
            const float* block = engine->nullBackend.LastBlock();
            for (std::uint32_t i = 0; i < engine->nullBackend.BlockFrames();
                 ++i) {
                maxPeak = std::max(maxPeak, std::fabs(block[i]));
            }
        } else {
            VnSnapshot snapshot{};
            snapshot.structSize = sizeof(snapshot);
            if (Vn_CaptureSnapshot(&snapshot) == VN_OK &&
                snapshot.busCount != 0) {
                const bool newBlock =
                    snapshot.blockIndex > afterStop->snapshotBlockIndex;
                if (newBlock) {
                    maxPeak = std::max(maxPeak, snapshot.busPeaks[0]);
                }
            }
        }
        ++nullBlock;
        if (maxPeak > kSilenceFloor) {
            if (outPeak != nullptr) {
                *outPeak = maxPeak;
            }
            return true;
        }
        if (!options.nullMode && !serviceWait.Wait()) {
            return false;
        }
    }
    if (outPeak != nullptr) {
        *outPeak = maxPeak;
    }
    return maxPeak > kSilenceFloor;
}

bool ServiceControlBlocks(harken::Engine* engine, const Options& options,
                          std::uint32_t blockCount) {
    for (std::uint32_t block = 0; block < blockCount; ++block) {
        if (options.nullMode) {
            engine->nullBackend.Pump(1);
        }
        if (Vn_Update() != VN_OK) {
            return false;
        }
        if (!options.nullMode) {
            std::this_thread::sleep_for(std::chrono::milliseconds(6));
        }
    }
    return true;
}

bool WaitForPlaybackIdle(harken::Engine* engine, const Options& options) {
    if (engine == nullptr) {
        return false;
    }
    const auto rowsAreFree = [engine]() {
        return std::all_of(
            engine->playableServices.begin(), engine->playableServices.end(),
            [](const harken::EnginePlayableServiceRow& row) {
                return row.phase == harken::EnginePlayableStartPhase::Free &&
                       !row.actionPrepared;
            });
    };
    if (options.nullMode) {
        const std::uint32_t maxBlocks =
            std::max(1u, kSampleRate / kBlockFrames);
        for (std::uint32_t block = 0; block < maxBlocks; ++block) {
            engine->nullBackend.Pump(1);
            if (Vn_Update() != VN_OK) {
                return false;
            }
            float peak = 0.0f;
            const float* samples = engine->nullBackend.LastBlock();
            for (std::uint32_t frame = 0;
                 frame < engine->nullBackend.BlockFrames(); ++frame) {
                peak = std::max(peak, std::fabs(samples[frame]));
            }
            if (rowsAreFree() && peak <= kSilenceFloor) {
                return true;
            }
        }
        return false;
    }

    const RenderCursor baseline = CaptureRenderCursor(engine);
    if (!baseline.hasSnapshot) {
        return false;
    }
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(1500);
    while (std::chrono::steady_clock::now() < deadline) {
        if (Vn_Update() != VN_OK) {
            return false;
        }
        VnSnapshot snapshot{};
        snapshot.structSize = sizeof(snapshot);
        if (Vn_CaptureSnapshot(&snapshot) == VN_OK &&
            snapshot.blockIndex > baseline.snapshotBlockIndex &&
            snapshot.busCount != 0 && snapshot.activeVoiceCount == 0 &&
            snapshot.busPeaks[0] <= kSilenceFloor && rowsAreFree()) {
            return true;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

bool RunTriggerWindow(harken::Engine* engine, const Options& options,
                      VnActionFiringHandle firing,
                      const RenderCursor& baseline, float* outPeak,
                      FiringSelectionCopy* selection,
                      FiringQueueWindowCopy* queueWindow,
                      FiringQueueEndCopy* queueEnd) {
    if (engine == nullptr || (!options.nullMode && !baseline.hasSnapshot)) {
        return false;
    }
    ControlServiceWait serviceWait(!options.nullMode);
    if (!serviceWait.Ready()) return false;
    CaptureFiringSelection(engine, firing, selection);
    ObserveFiringQueueWindow(engine, firing, queueWindow);
    CaptureFiringQueueEnd(engine, firing, queueEnd);
    float maxPeak = 0.0f;
    bool sawFreshBlock = false;
    const std::uint32_t nullBlocks = std::max(
        1u, static_cast<std::uint32_t>(
                std::ceil(options.seconds * kSampleRate / kBlockFrames)));
    const auto deadline = std::chrono::steady_clock::now() +
                          std::chrono::milliseconds(
                              static_cast<int>(options.seconds * 1000.0));
    std::uint32_t nullBlock = 0;
    std::uint64_t heardBlockIndex = baseline.snapshotBlockIndex;
    while (options.nullMode ? nullBlock < nullBlocks
                            : std::chrono::steady_clock::now() < deadline) {
        if (options.nullMode) {
            engine->nullBackend.Pump(1);
        }
        const VnResult updated = Vn_Update();
        if (updated != VN_OK) {
            std::fprintf(stderr,
                "error: pack-play Vn_Update failed result=%d\n", updated);
            return false;
        }
        ObserveFiringQueueWindow(engine, firing, queueWindow);
        CaptureFiringQueueEnd(engine, firing, queueEnd);
        if (!CheckActionFiring(firing)) return false;
        CaptureFiringSelection(engine, firing, selection);
        bool haveNewBlock = false;
        bool blockNonSilent = false;
        if (options.nullMode) {
            const float* block = engine->nullBackend.LastBlock();
            float blockPeak = 0.0f;
            for (std::uint32_t frame = 0;
                 frame < engine->nullBackend.BlockFrames(); ++frame) {
                blockPeak = std::max(blockPeak, std::fabs(block[frame]));
            }
            maxPeak = std::max(maxPeak, blockPeak);
            sawFreshBlock = true;
            haveNewBlock = true;
            blockNonSilent = blockPeak > kSilenceFloor;
        } else {
            VnSnapshot snapshot{};
            snapshot.structSize = sizeof(snapshot);
            if (Vn_CaptureSnapshot(&snapshot) == VN_OK &&
                snapshot.busCount != 0 &&
                snapshot.blockIndex > baseline.snapshotBlockIndex) {
                maxPeak = std::max(maxPeak, snapshot.busPeaks[0]);
                sawFreshBlock = true;
                if (snapshot.blockIndex > heardBlockIndex) {
                    heardBlockIndex = snapshot.blockIndex;
                    haveNewBlock = true;
                    blockNonSilent = snapshot.busPeaks[0] > kSilenceFloor;
                }
            }
        }
        NoteFiringQueueHeardObservation(queueWindow, haveNewBlock, blockNonSilent);
        if (!options.nullMode && !serviceWait.Wait()) return false;
        ++nullBlock;
    }
    if (outPeak != nullptr) {
        *outPeak = maxPeak;
    }
    return sawFreshBlock && maxPeak > kSilenceFloor;
}

}  // namespace

void PrintPackPlayUsage() {
    std::fprintf(stderr,
                 "\npack-play loads a Studio gold pack set through the public "
                 "native sequence and posts one or more Actions.\n"
                 "  harken_host pack-play --root <platform-dir> --action-name <name> "
                 "[--language NAME] [--seconds N] [--repeats N] "
                 "[--start-delay-ms N] "
                 "[--capture-ready-gate <path> "
                 "--capture-ready-timeout-ms N] "
                 "[--caller-seed N] "
                 "[--variable-id ID --variable-values v1,v2] "
                 "[--no-recovery] [--null]\n"
                 "  Reads Init and Action companion pairs from the selected platform directory.\n"
                 "  Legacy manifest/index exports are unsupported; re-export with the matching Studio release.\n"
                 "  --null is smoke only. Default device is VN_DEVICE_REAL.\n");
}

int RunPackPlay(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
        PrintPackPlayUsage();
        return 1;
    }

    if (!options.captureReadyGate.empty()) {
        bool gateExists = false;
        if (!CaptureReadyGateExists(options.captureReadyGate, &gateExists)) {
            std::fprintf(stderr, "error: cannot inspect capture-ready gate\n");
            return 1;
        }
        if (gateExists) {
            std::fprintf(stderr,
                         "error: capture-ready gate must not preexist\n");
            return 1;
        }
        std::printf("pack-play capture-ready armed pid=%llu\n",
                    static_cast<unsigned long long>(CurrentProcessId()));
        std::fflush(stdout);
    }

    std::error_code ec;
    const std::filesystem::path requestedRoot =
        std::filesystem::weakly_canonical(std::filesystem::path(std::u8string_view(reinterpret_cast<const char8_t*>(options.root.data()), options.root.size())),
                                          ec);
    if (ec) {
        std::fprintf(stderr, "error: cannot resolve --root '%s'\n",
                     options.root.c_str());
        return 1;
    }
    PackCompanion companionInit, companionContent;
    PackCompanionDiagnostic diagnostic;
    VnResult captured = CapturePackCompanion(requestedRoot, {}, {}, nullptr, &companionInit, &diagnostic);
    if (captured == VN_OK)
        captured = CapturePackCompanion(requestedRoot, options.actionName, options.language,
            &companionInit, &companionContent, &diagnostic);
    if (captured != VN_OK) {
        auto binaryPath = diagnostic.jsonPath;
        binaryPath.replace_extension(".bytes");
        std::fprintf(stderr, "error: %s companion '%s' (bytes '%s'): %s failed (%d)\n",
            diagnostic.kind, Utf8Path(diagnostic.jsonPath).c_str(), Utf8Path(binaryPath).c_str(),
            diagnostic.stage, captured);
        std::fprintf(stderr, "Re-export with the matching Studio release (required schemaVersion: %u); "
            "select its platform directory. Legacy manifest/index exports are unsupported.\n",
            static_cast<unsigned>(HARKEN_PACK_SCHEMA_VERSION));
        return 3;
    }

    VnPackLoadDesc inspectDesc{};
    inspectDesc.structSize = sizeof(inspectDesc);
    inspectDesc.flags = VN_PACK_LOAD_FLAG_PROVIDER;
    inspectDesc.packStableId = companionInit.facts.packStableId;
    inspectDesc.providerUserData = &companionInit;
    inspectDesc.provide = PackCompanion::Provide;
    inspectDesc.release = PackCompanion::Release;
    VnInitPackInspectReport inspection{};
    inspection.structSize = sizeof(inspection);
    const VnResult inspected = Vn_InspectInitPack(&inspectDesc, &inspection);
    if (inspected != VN_OK) {
        std::fprintf(stderr, "error: Vn_InspectInitPack failed (%d)\n",
                     inspected);
        return 3;
    }

    VnConfig config{};
    config.structSize = sizeof(config);
    config.sampleRate = kSampleRate;
    config.blockFrames = kBlockFrames;
    config.deviceMode = options.nullMode ? VN_DEVICE_NULL : VN_DEVICE_REAL;
    config.maxVoices = 16;
    config.maxPacks = 8;
    config.maxActions = 64;
    config.maxCatalogEntries = inspection.recommendedMaxCatalogEntries;
    config.maxCatalogBytes = inspection.recommendedCatalogBytes;
    config.maxActions = std::max(config.maxActions, inspection.recommendedMaxActions);
    config.maxResidentDefinitionBytes = inspection.recommendedResidentDefinitionBytes;
    const VnResult init = Vn_Init(&config);
    if (init != VN_OK) {
        const harkenhost::PackPlayInitFailure failure =
            harkenhost::ClassifyPackPlayInitFailure(
                options.nullMode, static_cast<int>(init));
        char message[160];
        const char* stage = harken::RealDeviceInitStage();
        if (!harkenhost::FormatPackPlayInitFailure(
                static_cast<int>(init), stage, message, sizeof(message),
                harken::RealDeviceInitStageHasCode(),
                harken::RealDeviceInitStageCode(),
                harken::RealDeviceInitStageHasOpenPlan(),
                harken::RealDeviceInitEnumerateFailed(),
                harken::RealDeviceInitTriedEnumeratedCount(),
                harken::RealDeviceInitOpenedEnumeratedIndex())) {
            std::fprintf(stderr, "error: Vn_Init failed (%d)\n",
                         static_cast<int>(init));
        } else {
            std::fprintf(stderr, "%s", message);
        }
        return failure.exitCode;
    }
    harken::Engine* engine = harken::detail::ActiveEngine();
    if (engine == nullptr) {
        Vn_Term();
        return 3;
    }

    const auto activate = [&]() {
        return ActivatePackCompanions(companionInit, companionContent);
    };
    const VnResult activated = activate();
    if (activated != VN_OK) {
        std::fprintf(stderr, "error: freeze public pack load failed (%d)\n", activated);
        Vn_Term();
        return 3;
    }

    VnPostActionByNameDesc named{};
    named.structSize = sizeof(named);
    named.utf8Name = options.actionName.c_str();
    harken::StoreObjectId(named.projectGuid, companionInit.facts.projectId);
    VnActionResolved resolved{};
    resolved.structSize = sizeof(resolved);
    const auto result = Vn_ResolveActionByName(&named, &resolved);
    if (result != VN_OK) {
        std::fprintf(stderr, "error: companion name resolution failed (%d)\n", result);
        Vn_Term();
        return 3;
    }

    VnPostActionDesc post{};
    post.structSize = sizeof(post);
    post.actionId = resolved.actionId;
    post.callerSeed = options.callerSeed;
    if (Vn_CreateEmitter(&post.emitter) != VN_OK) {
        std::fprintf(stderr, "error: Vn_CreateEmitter failed\n");
        Vn_Term();
        return 3;
    }
    if (options.startDelayMs != 0) {
        std::printf("pack-play start-delay-ms=%u\n", options.startDelayMs);
        std::fflush(stdout);
        const auto delayDeadline = std::chrono::steady_clock::now() +
            std::chrono::milliseconds(options.startDelayMs);
        while (std::chrono::steady_clock::now() < delayDeadline) {
            if (Vn_Update() != VN_OK) {
                std::fprintf(stderr,
                             "error: Vn_Update failed during start delay\n");
                Vn_Term();
                return 3;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
    }
    if (!WaitForCaptureReadyGate(options)) {
        Vn_Term();
        return 3;
    }
    float maxPeak = 0.0f;
    VnActionFiringHandle previousFiring = 0;
    for (std::uint32_t trigger = 0; trigger < options.repeats; ++trigger) {
        if (trigger >= 1 && !options.variableValues.empty()) {
            const VnResult stopped = Vn_StopActionFiring(previousFiring, 0);
            if (stopped != VN_OK) {
                std::fprintf(stderr,
                             "error: pack-play trigger %u/%u stop failed (%d)\n",
                             trigger, options.repeats, static_cast<int>(stopped));
                Vn_Term();
                return 3;
            }
        }
        if (trigger != 0 && !WaitForPlaybackIdle(engine, options)) {
            std::fprintf(stderr,
                         "error: pack-play trigger %u/%u did not retire\n",
                         trigger, options.repeats);
            Vn_Term();
            return 3;
        }
        if (!options.variableValues.empty()) {
            const VnResult set =
                Vn_SetValue(options.variableId, options.variableValues[trigger]);
            if (set != VN_OK || !ServiceControlBlocks(engine, options, 8)) {
                std::fprintf(stderr,
                             "error: pack-play variable update failed (%d)\n",
                             set);
                Vn_Term();
                return 3;
            }
        }
        // The real device advances while packs load; publish that control work
        // before capturing the first-post observation boundary.
        if (Vn_Update() != VN_OK) {
            std::fprintf(stderr, "error: pack-play pre-post update failed\n");
            Vn_Term();
            return 3;
        }
        const RenderCursor baseline = CaptureRenderCursor(engine);
        if (!options.nullMode && !baseline.hasSnapshot) {
            std::fprintf(stderr,
                         "error: pack-play trigger %u/%u has no baseline\n",
                         trigger + 1u, options.repeats);
            Vn_Term();
            return 3;
        }
        VnActionFiringHandle firing = 0;
        const VnResult posted = Vn_PostAction(&post, &firing);
        if (posted != VN_OK || firing == 0) {
            std::fprintf(stderr, "error: Vn_PostAction failed (%d)\n", posted);
            Vn_Term();
            return 3;
        }
        float triggerPeak = 0.0f;
        FiringSelectionCopy selection;
        FiringQueueWindowCopy queueWindow;
        FiringQueueEndCopy queueEnd;
        const bool triggerWindowOk = RunTriggerWindow(
            engine, options, firing, baseline, &triggerPeak, &selection,
            &queueWindow, &queueEnd);
        if (!PrintFiringQueueWindow(queueWindow, trigger + 1u, options.repeats,
                                    firing)) {
            std::fprintf(stderr,
                         "error: pack-play queue-window line failed trigger=%u/%u\n",
                         trigger + 1u, options.repeats);
            Vn_Term();
            return 2;
        }
        if (!PrintFiringQueueHeard(queueWindow, trigger + 1u, options.repeats,
                                   firing)) {
            std::fprintf(stderr,
                         "error: pack-play queue-heard line failed trigger=%u/%u\n",
                         trigger + 1u, options.repeats);
            Vn_Term();
            return 2;
        }
        if (!PrintFiringQueueEnd(queueEnd, trigger + 1u, options.repeats, firing)) {
            std::fprintf(stderr,
                         "error: pack-play queue-end line failed trigger=%u/%u\n",
                         trigger + 1u, options.repeats);
            Vn_Term();
            return 2;
        }
        if (!triggerWindowOk) {
            std::fprintf(stderr,
                         "error: pack-play trigger %u/%u validation failed\n",
                         trigger + 1u, options.repeats);
            Vn_Term();
            return 2;
        }
        maxPeak = std::max(maxPeak, triggerPeak);
        previousFiring = firing;
        if (trigger == 0) {
            std::printf("pack-play first-callback non-silent peak=%.4f\n",
                        triggerPeak);
        }
        std::printf("pack-play trigger %u/%u peak=%.4f firing=%llu\n",
                    trigger + 1u, options.repeats, triggerPeak,
                    static_cast<unsigned long long>(firing));
        if (!PrintCapturedSelectionChild(selection, trigger + 1u,
                                           options.repeats, firing)) {
            std::fprintf(stderr,
                         "error: pack-play selection-child trigger %u/%u firing=%llu unbound\n",
                         trigger + 1u, options.repeats,
                         static_cast<unsigned long long>(firing));
            Vn_Term();
            return 2;
        }
        (void)PrintCapturedSelectionSeed(selection, trigger + 1u,
                                         options.repeats, firing);
    }

    if (!options.nullMode && !options.skipRecovery) {
        const std::uint64_t hits = harken::memguard::HitCount();
        std::printf("pack-play audio-thread alloc hits=%llu\n",
                    static_cast<unsigned long long>(hits));
        if (engine->device.RenderAdmissionCommitted()) {
            std::printf("pack-play render-admission=1\n");
        }
        const VnResult stopped = Vn_Suspend();
        const RenderCursor afterStop = CaptureRenderCursor(engine);
        const VnResult restarted = Vn_Resume();
        bool deviceRestored = false;
        if (stopped == VN_OK && restarted == VN_OK) {
            VnActionFiringHandle second = 0;
            post.emitter = 0;
            float restartPeak = 0.0f;
            if (Vn_Update() == VN_OK &&
                Vn_CreateEmitter(&post.emitter) == VN_OK &&
                Vn_PostAction(&post, &second) == VN_OK && second != 0 &&
                WaitForAudiblePeak(
                    engine, options, second, &restartPeak, &afterStop)) {
                deviceRestored = true;
            }
        }
        if (deviceRestored) {
            std::printf("pack-play stop-restart restored playback\n");
        } else {
            Vn_Term();
            if (Vn_Init(&config) != VN_OK) {
                std::fprintf(stderr, "error: pack-play restart Vn_Init failed\n");
                return 3;
            }
            engine = harken::detail::ActiveEngine();
            if (engine == nullptr) {
                std::fprintf(stderr, "error: pack-play restart engine missing\n");
                Vn_Term();
                return 3;
            }
            const VnResult reactivated = activate();
            if (reactivated != VN_OK) {
                std::fprintf(stderr, "error: pack-play reload after restart failed\n");
                Vn_Term();
                return 3;
            }
            VnActionFiringHandle second = 0;
            post.emitter = 0;
            if (Vn_Update() != VN_OK) {
                std::fprintf(stderr, "error: pack-play reload update failed\n");
                Vn_Term();
                return 3;
            }
            const RenderCursor afterInit = CaptureRenderCursor(engine);
            if (Vn_CreateEmitter(&post.emitter) != VN_OK ||
                Vn_PostAction(&post, &second) != VN_OK || second == 0) {
                std::fprintf(stderr, "error: pack-play restart post failed\n");
                Vn_Term();
                return 3;
            }
            float restartPeak = 0.0f;
            if (!WaitForAudiblePeak(
                    engine, options, second, &restartPeak, &afterInit)) {
                std::fprintf(stderr,
                    "error: pack-play restart validation failed\n");
                Vn_Term();
                return 2;
            }
            std::printf("pack-play term-init restored playback\n");
        }
    }

    std::printf("pack-play peak=%.4f action=%s non-silent\n", maxPeak,
                harken::ObjectIdString(resolved.actionId).c_str());
    Vn_Term();
    return 0;
}

} // namespace harkenhost
