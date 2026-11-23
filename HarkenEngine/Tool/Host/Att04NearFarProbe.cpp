#include "Att04NearFarProbe.h"
#include "HostPacks.h"

#include "Engine.h"

#include <harken/Harken.h>
#include <harken/HarkenDevelopment.h>
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
    std::string initPack;
    std::string contentPack;
    VnObjectId actionId{};
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
        } else if (std::strcmp(arg, "--init-pack") == 0 && i + 1 < argc) {
            out->initPack = argv[++i];
        } else if (std::strcmp(arg, "--content-pack") == 0 && i + 1 < argc) {
            out->contentPack = argv[++i];
        } else if (std::strcmp(arg, "--action-id") == 0 && i + 1 < argc) {
            if (!harken::ParseObjectId(argv[++i], &out->actionId)) return false;
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
    return sawNull && !out->initPack.empty() && !out->contentPack.empty() &&
           !harken::IsNull(out->actionId) && out->distance > 0.0;
}

std::filesystem::path PathFromUtf8(const std::string& text) {
    return std::filesystem::path(std::u8string_view(
        reinterpret_cast<const char8_t*>(text.data()), text.size()));
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
        "\natt04-nearfar activates an Init and a content Pack in a new "
        "process and posts one Action at --distance meters.\n"
        "  harken_host att04-nearfar --init-pack <file> --content-pack <file> --action-id <guid> "
        "--distance <meters> [--seconds N] --null\n"
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

    HostPack packInit, packContent;
    VnResult captured = CaptureHostPack(PathFromUtf8(options.initPack), nullptr, {}, &packInit);
    if (captured == VN_OK)
        captured = CaptureHostPack(PathFromUtf8(options.contentPack), &packInit, options.actionId,
                                   &packContent);
    if (captured != VN_OK) {
        std::fprintf(stderr, "error: att04-nearfar cannot read or inspect the packs (%d)\n",
                     captured);
        return 3;
    }

    VnPackLoadDesc inspectDesc{};
    inspectDesc.structSize = sizeof(inspectDesc);
    inspectDesc.flags = VN_PACK_LOAD_FLAG_PROVIDER;
    inspectDesc.packStableId = packInit.facts.packStableId;
    inspectDesc.providerUserData = &packInit;
    inspectDesc.provide = HostPack::Provide;
    inspectDesc.release = HostPack::Release;
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
    const VnResult activated = ActivateHostPacks(packInit, packContent, &activationReport);
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
        static_cast<unsigned long long>(packInit.facts.packStableId),
        options.initPack.c_str());
    std::fprintf(stderr, "att04-nearfar opened 1 %llu %s\n",
        static_cast<unsigned long long>(packContent.facts.packStableId),
        options.contentPack.c_str());
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

    VnPostActionDesc post{};
    post.structSize = sizeof(post);
    post.actionId = options.actionId;
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
    const VnResult posted = Vn_PostAction(&post, &firing);
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
