#include "Att04NearFarProbe.h"
#include "PackCompanion.h"

#include "Engine.h"

#include <harken/Harken.h>
#include <harken/ObjectId.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>

namespace harkenhost {
namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kBlockFrames = 256;
constexpr float kSilenceFloor = 1.0e-4f;

struct Options {
    std::string root;
    std::string actionName;
    std::string language;
    double seconds = 0.4;
    double distance = 0.0;
};

bool ParsePositiveFinite(const char* text, double* out) {
    if (text == nullptr || out == nullptr) {
        return false;
    }
    char* end = nullptr;
    const double value = std::strtod(text, &end);
    if (end == text || *end != '\0' || !(value > 0.0) || !std::isfinite(value)) {
        return false;
    }
    *out = value;
    return true;
}

bool ParseOptions(int argc, char** argv, Options* out) {
    bool sawNull = false;
    for (int i = 2; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--null") == 0) {
            sawNull = true;
        } else if (std::strcmp(arg, "--root") == 0 && i + 1 < argc) {
            out->root = argv[++i];
        } else if (std::strcmp(arg, "--language") == 0 && i + 1 < argc) {
            out->language = argv[++i];
            if (!PackCompanionLanguage(out->language)) return false;
        } else if (std::strcmp(arg, "--action-name") == 0 && i + 1 < argc) {
            out->actionName = argv[++i];
            if (!PackCompanionName(out->actionName) || out->actionName == "Init") return false;
        } else if (std::strcmp(arg, "--seconds") == 0 && i + 1 < argc) {
            if (!ParsePositiveFinite(argv[++i], &out->seconds) ||
                out->seconds > 60.0) {
                return false;
            }
        } else if (std::strcmp(arg, "--distance") == 0 && i + 1 < argc) {
            if (!ParsePositiveFinite(argv[++i], &out->distance) ||
                out->distance > 10000.0) {
                return false;
            }
        } else {
            return false;
        }
    }
    return sawNull && !out->root.empty() && !out->actionName.empty() &&
           out->distance > 0.0;
}

std::string Utf8Path(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

float CaptureNullPeak(harken::Engine* engine, double seconds) {
    if (engine == nullptr) {
        return 0.0f;
    }
    float maxPeak = 0.0f;
    const std::uint32_t nullBlocks = std::max(
        2u, static_cast<std::uint32_t>(
                std::ceil(seconds * kSampleRate / kBlockFrames)));
    for (std::uint32_t block = 0; block < nullBlocks; ++block) {
        engine->nullBackend.Pump(1);
        if (Vn_Update() != VN_OK) {
            return 0.0f;
        }
        const float* samples = engine->nullBackend.LastBlock();
        const std::uint32_t sampleCount = engine->nullBackend.BlockFrames() *
                                          engine->nullBackend.Channels();
        for (std::uint32_t sample = 0; sample < sampleCount; ++sample) {
            maxPeak = std::max(maxPeak, std::fabs(samples[sample]));
        }
    }
    return maxPeak;
}

} // namespace

void PrintAtt04NearFarProbeUsage() {
    std::fprintf(
        stderr,
        "\natt04-nearfar activates an official exported pack set in a new "
        "process and posts one Action at --distance meters.\n"
        "  harken_host att04-nearfar --root <platform-dir> --action-name <name> "
        "--distance <meters> [--language NAME] [--seconds N] --null\n"
        "  Legacy manifest/index exports are unsupported; re-export with the matching Studio release.\n"
        "  Null backend only. One process, one distance. Position is set "
        "before Post; peak is read after Post plus at least one more "
        "Vn_Update.\n");
}

int RunAtt04NearFarProbe(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
        PrintAtt04NearFarProbeUsage();
        return 1;
    }

    std::error_code ec;
    const std::filesystem::path requestedRoot = std::filesystem::weakly_canonical(
        std::filesystem::path(std::u8string_view(
            reinterpret_cast<const char8_t*>(options.root.data()),
            options.root.size())),
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
    auto actionPath = diagnostic.jsonPath;
    actionPath.replace_extension(".bytes");

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
    config.deviceMode = VN_DEVICE_NULL;
    config.maxVoices = 16;
    config.maxPacks = 32;
    config.maxActions = std::max(64u, inspection.recommendedMaxActions);
    config.maxCatalogEntries = inspection.recommendedMaxCatalogEntries;
    config.maxCatalogBytes = inspection.recommendedCatalogBytes;
    config.maxResidentDefinitionBytes = inspection.recommendedResidentDefinitionBytes;
    const VnResult inited = Vn_Init(&config);
    if (inited != VN_OK) {
        std::fprintf(stderr, "error: att04-nearfar Vn_Init failed VnResult=%d\n",
                     static_cast<int>(inited));
        return 2;
    }
    harken::Engine* engine = harken::detail::ActiveEngine();
    if (engine == nullptr) {
        std::fprintf(stderr, "error: att04-nearfar ActiveEngine is null\n");
        Vn_Term();
        return 2;
    }

    VnCanonicalPackSetActivationReportV1 activationReport{};
    const VnResult activated = ActivatePackCompanions(companionInit, companionContent, &activationReport);
    if (activated != VN_OK) {
        std::fprintf(stderr,
                     "error: att04-nearfar activate failed VnResult=%d "
                     "members=%u initHandle=%llu\n",
                     static_cast<int>(activated),
                     activationReport.memberCount,
                     static_cast<unsigned long long>(activationReport.initHandle));
        Vn_Term();
        return 3;
    }
    std::fprintf(stderr, "att04-nearfar opened 0 %llu %s\n",
        static_cast<unsigned long long>(companionInit.facts.packStableId),
        Utf8Path(requestedRoot / "Init.bytes").c_str());
    std::fprintf(stderr, "att04-nearfar opened 1 %llu %s\n",
        static_cast<unsigned long long>(companionContent.facts.packStableId),
        Utf8Path(actionPath).c_str());
    std::fprintf(stderr,
                 "att04-nearfar identity activationGeneration=%llu "
                 "busContentGeneration=%llu publishedBusFingerprint=%llu "
                 "compileTargetProfileFingerprint=%llu initStableId=%llu\n",
                 static_cast<unsigned long long>(activationReport.activationGeneration),
                 static_cast<unsigned long long>(activationReport.busContentGeneration),
                 static_cast<unsigned long long>(activationReport.publishedBusFingerprint),
                 static_cast<unsigned long long>(
                     activationReport.compileTargetProfileFingerprint),
                 static_cast<unsigned long long>(inspection.packStableId));

    VnPostActionByNameDesc post{};
    post.structSize = sizeof(post);
    post.utf8Name = options.actionName.c_str();
    harken::StoreObjectId(post.projectGuid, companionInit.facts.projectId);
    if (Vn_CreateEmitter(&post.emitter) != VN_OK) {
        std::fprintf(stderr, "error: att04-nearfar Vn_CreateEmitter failed\n");
        Vn_Term();
        return 3;
    }
    VnVec3 position{};
    position.x = 0.0f;
    position.y = 0.0f;
    position.z = -static_cast<float>(options.distance);
    const VnResult placed = Vn_SetEmitterPosition(post.emitter, position);
    if (placed != VN_OK) {
        std::fprintf(stderr,
                     "error: att04-nearfar SetEmitterPosition failed "
                     "VnResult=%d\n",
                     static_cast<int>(placed));
        Vn_Term();
        return 3;
    }
    if (Vn_Update() != VN_OK) {
        std::fprintf(stderr, "error: att04-nearfar pre-post Vn_Update failed\n");
        Vn_Term();
        return 3;
    }

    VnActionFiringHandle firing = 0;
    const VnResult posted = Vn_PostActionByName(&post, &firing);
    if (posted != VN_OK || firing == 0) {
        std::fprintf(stderr,
                     "error: att04-nearfar post failed VnResult=%d firing=%llu\n",
                     static_cast<int>(posted),
                     static_cast<unsigned long long>(firing));
        Vn_Term();
        return 3;
    }
    // Position is applied on the next Update after SetEmitterPosition. Peak
    // must come from Post plus at least one more Update, not the first
    // unaffecting block.
    const float peak = CaptureNullPeak(engine, options.seconds);
    std::fprintf(stderr,
                 "att04-nearfar distance=%.3f peak=%.6f firing=%llu "
                 "VnResult=%d\n",
                 options.distance, peak,
                 static_cast<unsigned long long>(firing),
                 static_cast<int>(posted));
    if (peak <= kSilenceFloor) {
        std::fprintf(stderr, "error: att04-nearfar peak at silence floor\n");
        Vn_Term();
        return 3;
    }
    Vn_Term();
    return 0;
}

} // namespace harkenhost
