#include "Att04NearFarProbe.h"
#include "PackPlayPath.h"
#include "PackSidecar.h"

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
#include <vector>

namespace harkenhost {
namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kBlockFrames = 256;
constexpr float kSilenceFloor = 1.0e-4f;

struct Options {
    std::string root;
    VnObjectId actionId{};
    std::string contentPackName;
    std::string platformId;
    std::string platformName;
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
        } else if (std::strcmp(arg, "--platform-id") == 0 && i + 1 < argc) {
            out->platformId = argv[++i];
            if (!PackSidecarUuid(out->platformId)) return false;
        } else if (std::strcmp(arg, "--platform-name") == 0 && i + 1 < argc) {
            out->platformName = argv[++i];
            if (!PackSidecarName(out->platformName)) return false;
        } else if (std::strcmp(arg, "--language") == 0 && i + 1 < argc) {
            out->language = argv[++i];
            if (!PackPlayLocatorLegal(out->language) ||
                out->language.find('/') != std::string::npos) {
                return false;
            }
        } else if (std::strcmp(arg, "--action") == 0 && i + 1 < argc) {
            if (!harken::ParseObjectId(argv[++i], &out->actionId)) {
                return false;
            }
        } else if (std::strcmp(arg, "--content") == 0 && i + 1 < argc) {
            out->contentPackName = argv[++i];
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
    return sawNull && !out->root.empty() && !harken::IsNull(out->actionId) &&
           out->distance > 0.0;
}

std::string HostBasePlatform() {
#if defined(_WIN32)
    return "Windows";
#elif defined(__ANDROID__)
    return "Android";
#elif defined(__APPLE__)
    return "macOS";
#elif defined(__linux__)
    return "Linux";
#else
    return {};
#endif
}

std::string Utf8Path(const std::filesystem::path& path) {
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

const PackSidecarEntry* FindPack(const std::vector<PackSidecarEntry>& packs,
                                 const std::string& name) {
    for (const auto& pack : packs) {
        if (pack.name == name) {
            return &pack;
        }
    }
    return nullptr;
}

std::filesystem::path PackFilePath(const std::filesystem::path& root,
                                   const PackSidecarEntry& pack) {
    return root / std::filesystem::path(std::u8string_view(
               reinterpret_cast<const char8_t*>(pack.path.data()),
               pack.path.size()));
}

VnResult ActivatePublicPackSet(const std::string& initPath,
                               const std::string& root,
                               std::uint64_t initPackStableId,
                               const std::vector<const PackSidecarEntry*>& loadOrder,
                               const std::filesystem::path& rootPath,
                               VnCanonicalPackSetActivationReportV1* outReport) {
    std::vector<std::string> memberPaths;
    memberPaths.reserve(loadOrder.size() + 1);
    memberPaths.push_back(initPath);
    for (const auto* pack : loadOrder) {
        memberPaths.push_back(Utf8Path(PackFilePath(rootPath, *pack)));
    }
    std::vector<VnCanonicalPackSetMemberV1> members(memberPaths.size());
    std::vector<VnCanonicalPackSetMemberResultV1> results(members.size());
    for (std::size_t i = 0; i < members.size(); ++i) {
        auto& member = members[i];
        member.structSize = sizeof(member);
        member.flags = VN_CANONICAL_PACK_SET_MEMBER_ROOT;
        member.packKind = i == 0 ? VN_CANONICAL_PACK_SET_MEMBER_INIT
                                 : VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
        member.packStableId =
            i == 0 ? initPackStableId : loadOrder[i - 1]->packStableId;
        member.utf8Path = memberPaths[i].c_str();
        member.utf8ContainmentRoot = root.c_str();
        results[i].structSize = sizeof(results[i]);
    }
    VnCanonicalPackSetActivationDescV1 activation{};
    activation.structSize = sizeof(activation);
    activation.setRevision = 1;
    activation.memberCount = static_cast<std::uint32_t>(members.size());
    activation.memberStructSize = sizeof(members[0]);
    activation.members = members.data();
    activation.outputMemberCapacity = activation.memberCount;
    activation.outputMemberStructSize = sizeof(results[0]);
    activation.outputMembers = results.data();
    VnCanonicalPackSetActivationReportV1 report{};
    report.structSize = sizeof(report);
    const VnResult result = Vn_ActivateCanonicalPackSetV1(&activation, &report);
    if (outReport != nullptr) {
        *outReport = report;
    }
    if (result == VN_OK) {
        for (std::size_t i = 0; i < members.size(); ++i) {
            const char* path = members[i].utf8Path != nullptr ? members[i].utf8Path : "";
            std::fprintf(stderr, "att04-nearfar opened %zu %llu %s\n", i,
                         static_cast<unsigned long long>(members[i].packStableId),
                         path);
        }
    } else {
        std::fprintf(stderr, "error: att04-nearfar members=%u\n",
                     static_cast<unsigned>(members.size()));
        for (std::size_t i = 0; i < members.size(); ++i) {
            std::fprintf(stderr,
                         "error: att04-nearfar member[%zu] kind=%u id=%llu path=%s\n",
                         i, members[i].packKind,
                         static_cast<unsigned long long>(members[i].packStableId),
                         members[i].utf8Path != nullptr ? members[i].utf8Path : "");
        }
    }
    return result;
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
        "  harken_host att04-nearfar --root <gold-dir> --action <stableId> "
        "--distance <meters> [--content <packName>] [--seconds N] --null\n"
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
    std::filesystem::path rootPath;
    std::string sidecarError;
    if (!ResolvePackSidecarRoot(requestedRoot, options.platformId,
                                options.platformName, HostBasePlatform(),
                                &rootPath, &sidecarError, options.language)) {
        std::fprintf(stderr, "error: %s\n", sidecarError.c_str());
        return 1;
    }
    const std::string root = Utf8Path(rootPath);
    PackSidecarPlatform platform;
    std::vector<PackSidecarEntry> packs;
    if (!LoadPackSidecarManifest(rootPath, &platform, &packs, &sidecarError,
                                 options.language)) {
        std::fprintf(stderr, "error: %s\n", sidecarError.c_str());
        return 1;
    }
    const auto initPack =
        std::find_if(packs.begin(), packs.end(),
                     [](const auto& pack) { return pack.isInit; });
    if (initPack == packs.end()) {
        std::fprintf(stderr, "error: manifest is missing Init\n");
        return 1;
    }
    const std::string initPath = Utf8Path(PackFilePath(rootPath, *initPack));
    const PackSidecarEntry* content = nullptr;
    if (!options.contentPackName.empty()) {
        content = FindPack(packs, options.contentPackName);
    } else {
        content = FindPack(packs, "ExportedPackSpatialNearFar");
    }
    if (content == nullptr) {
        std::fprintf(stderr,
                     "error: manifest is missing the requested content pack\n");
        return 1;
    }

    std::vector<const PackSidecarEntry*> loadOrder;
    std::vector<std::uint8_t> visiting(packs.size());
    const auto visit = [&](auto&& self, const PackSidecarEntry& pack) -> bool {
        const auto index = static_cast<std::size_t>(&pack - packs.data());
        if (visiting[index] == 2) return true;
        if (visiting[index] == 1) return false;
        visiting[index] = 1;
        for (const auto& dependency : pack.dependencies) {
            std::uint64_t id = 0;
            if (!PackSidecarU64(dependency, &id)) return false;
            const auto provider = std::find_if(
                packs.begin(), packs.end(),
                [id](const auto& row) { return row.packStableId == id; });
            if (provider == packs.end() || !self(self, *provider)) return false;
        }
        visiting[index] = 2;
        if (!pack.isInit) loadOrder.push_back(&pack);
        return true;
    };
    if (!visit(visit, *content)) {
        std::fprintf(stderr, "error: invalid content pack dependency closure\n");
        return 1;
    }

    std::sort(loadOrder.begin(), loadOrder.end(),
              [](const auto* a, const auto* b) {
                  return a->packStableId < b->packStableId;
              });

    VnPackLoadDesc inspectDesc{};
    inspectDesc.structSize = sizeof(inspectDesc);
    inspectDesc.flags = VN_PACK_LOAD_FLAG_ROOT;
    inspectDesc.utf8Path = initPath.c_str();
    inspectDesc.utf8ContainmentRoot = root.c_str();
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
    config.maxActions = 64;
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
    const VnResult activated = ActivatePublicPackSet(
        initPath, root, inspection.packStableId, loadOrder, rootPath,
        &activationReport);
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
