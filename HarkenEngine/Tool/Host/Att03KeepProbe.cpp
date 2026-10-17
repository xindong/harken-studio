#include "Att03KeepProbe.h"

#include "Engine.h"

#include <harken/Harken.h>
#include <harken/ObjectId.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

namespace harkenhost {
namespace {

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kBlockFrames = 256;
constexpr std::uint64_t kInitPackStableId = 11;
constexpr std::uint64_t kContentPackStableId = 22;
constexpr std::uint64_t kObjectIdHigh = 0x3da91a64b9474560ull;

struct Options {
    std::string initPath;
    std::string contentPath;
    std::string badPath;
    std::uint64_t actionLow = 0;
};

bool ParseUnsigned64(const char* text, std::uint64_t* out) {
    if (text == nullptr || out == nullptr) {
        return false;
    }
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text, &end, 10);
    if (end == text || *end != '\0') {
        return false;
    }
    *out = static_cast<std::uint64_t>(value);
    return true;
}

bool ParseOptions(int argc, char** argv, Options* out) {
    for (int i = 2; i < argc; ++i) {
        const char* arg = argv[i];
        if (std::strcmp(arg, "--init") == 0 && i + 1 < argc) {
            out->initPath = argv[++i];
        } else if (std::strcmp(arg, "--content") == 0 && i + 1 < argc) {
            out->contentPath = argv[++i];
        } else if (std::strcmp(arg, "--bad") == 0 && i + 1 < argc) {
            out->badPath = argv[++i];
        } else if (std::strcmp(arg, "--action") == 0 && i + 1 < argc) {
            if (!ParseUnsigned64(argv[++i], &out->actionLow) ||
                out->actionLow == 0) {
                return false;
            }
        } else {
            return false;
        }
    }
    return !out->initPath.empty() && !out->contentPath.empty() &&
           !out->badPath.empty() && out->actionLow != 0;
}

VnResult ActivateTwoMembers(const char* initPath, std::uint64_t initStableId,
                            const char* contentPath,
                            std::uint64_t contentStableId,
                            VnPackHandle expectedInit,
                            VnPackHandle expectedContent,
                            std::uint64_t baseRevision,
                            std::uint64_t setRevision,
                            VnCanonicalPackSetActivationReportV1* outReport,
                            VnCanonicalPackSetMemberResultV1* outRows) {
    VnCanonicalPackSetMemberV1 members[2]{};
    VnCanonicalPackSetMemberResultV1 rows[2]{};
    members[0].structSize = sizeof(members[0]);
    members[0].packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    members[0].packStableId = initStableId;
    members[0].utf8Path = initPath;
    members[0].expectedHandle = expectedInit;
    members[1].structSize = sizeof(members[1]);
    members[1].packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
    members[1].packStableId = contentStableId;
    members[1].utf8Path = contentPath;
    members[1].expectedHandle = expectedContent;
    rows[0].structSize = sizeof(rows[0]);
    rows[1].structSize = sizeof(rows[1]);

    VnCanonicalPackSetActivationDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.expectedCurrentInitHandle = expectedInit;
    desc.baseSetRevision = baseRevision;
    desc.setRevision = setRevision;
    desc.memberCount = 2;
    desc.memberStructSize = sizeof(members[0]);
    desc.members = members;
    desc.outputMemberCapacity = 2;
    desc.outputMemberStructSize = sizeof(rows[0]);
    desc.outputMembers = rows;
    outReport->structSize = sizeof(*outReport);
    const VnResult result = Vn_ActivateCanonicalPackSetV1(&desc, outReport);
    if (outRows != nullptr) {
        outRows[0] = rows[0];
        outRows[1] = rows[1];
    }
    return result;
}

VnPackHandle LiveInitHandle(harken::Engine* engine) {
    if (engine == nullptr) {
        return 0;
    }
    const auto* published = engine->packComposition.Published();
    return published == nullptr ? 0 : published->initHandle;
}

void PrintPublished(const char* tag, VnResult result, harken::Engine* engine) {
    const std::uint32_t loaded =
        engine == nullptr ? 0u : engine->packs.LoadedCount();
    const bool published =
        engine != nullptr && engine->packComposition.Published() != nullptr;
    std::fprintf(stderr,
                 "att03-keep %s VnResult=%d initHandle=%llu LoadedCount=%u "
                 "published=%u\n",
                 tag, static_cast<int>(result),
                 static_cast<unsigned long long>(LiveInitHandle(engine)), loaded,
                 published ? 1u : 0u);
}

} // namespace

void PrintAtt03KeepProbeUsage() {
    std::fprintf(
        stderr,
        "\natt03-keep activates a legal Init+content set, then a missing or "
        "bad second member, without Terming the published set.\n"
        "  harken_host att03-keep --init <Init.pack> --content <Content.pack> "
        "--bad <missing-or-bad.pack> --action <stableId-low>\n"
        "  Null backend only. Failure must print VnResult and published "
        "still-present (initHandle / LoadedCount).\n");
}

int RunAtt03KeepProbe(int argc, char** argv) {
    Options options;
    if (!ParseOptions(argc, argv, &options)) {
        PrintAtt03KeepProbeUsage();
        return 1;
    }

    VnConfig config{};
    config.structSize = sizeof(config);
    config.sampleRate = kSampleRate;
    config.blockFrames = kBlockFrames;
    config.deviceMode = VN_DEVICE_NULL;
    config.maxVoices = 16;
    config.maxPacks = 8;
    config.maxActions = 64;
    const VnResult inited = Vn_Init(&config);
    if (inited != VN_OK) {
        std::fprintf(stderr, "error: att03-keep Vn_Init failed VnResult=%d\n",
                     static_cast<int>(inited));
        return 2;
    }
    harken::Engine* engine = harken::detail::ActiveEngine();
    if (engine == nullptr) {
        std::fprintf(stderr, "error: att03-keep ActiveEngine is null\n");
        Vn_Term();
        return 2;
    }

    VnCanonicalPackSetActivationReportV1 first{};
    VnCanonicalPackSetMemberResultV1 firstRows[2]{};
    const VnResult legal = ActivateTwoMembers(
        options.initPath.c_str(), kInitPackStableId, options.contentPath.c_str(),
        kContentPackStableId, 0, 0, 0, 1, &first, firstRows);
    PrintPublished("legal", legal, engine);
    if (legal != VN_OK || first.initHandle == 0 ||
        engine->packs.LoadedCount() == 0 ||
        engine->packComposition.Published() == nullptr) {
        std::fprintf(stderr, "error: att03-keep legal activate failed\n");
        Vn_Term();
        return 3;
    }
    const VnPackHandle keptHandle = first.initHandle;
    const std::uint32_t keptLoaded = engine->packs.LoadedCount();
    const auto* keptPublished = engine->packComposition.Published();

    VnCanonicalPackSetActivationReportV1 refused{};
    const VnResult bad = ActivateTwoMembers(
        options.initPath.c_str(), kInitPackStableId, options.badPath.c_str(),
        kContentPackStableId, keptHandle, firstRows[1].handle,
        first.resultSetRevision, first.resultSetRevision + 1, &refused,
        nullptr);
    PrintPublished("refused", bad, engine);
    if (bad == VN_OK) {
        std::fprintf(stderr, "error: att03-keep bad activate unexpectedly OK\n");
        Vn_Term();
        return 3;
    }
    if (engine->packComposition.Published() != keptPublished ||
        LiveInitHandle(engine) != keptHandle ||
        engine->packs.LoadedCount() != keptLoaded ||
        engine->packs.LoadedCount() == 0) {
        std::fprintf(stderr,
                     "error: att03-keep published destroyed after refusal "
                     "VnResult=%d initHandle=%llu LoadedCount=%u published=%u\n",
                     static_cast<int>(bad),
                     static_cast<unsigned long long>(LiveInitHandle(engine)),
                     engine->packs.LoadedCount(),
                     engine->packComposition.Published() != nullptr ? 1u : 0u);
        Vn_Term();
        return 3;
    }

    VnPostActionDesc post{};
    post.structSize = sizeof(post);
    post.actionId = VnObjectId{kObjectIdHigh, options.actionLow};
    VnActionFiringHandle firing = 0;
    const VnResult posted = Vn_PostAction(&post, &firing);
    std::fprintf(stderr,
                 "att03-keep post VnResult=%d firing=%llu initHandle=%llu "
                 "LoadedCount=%u published=%u\n",
                 static_cast<int>(posted),
                 static_cast<unsigned long long>(firing),
                 static_cast<unsigned long long>(LiveInitHandle(engine)),
                 engine->packs.LoadedCount(),
                 engine->packComposition.Published() != nullptr ? 1u : 0u);
    if (posted != VN_OK || firing == 0) {
        std::fprintf(stderr, "error: att03-keep post after refusal failed\n");
        Vn_Term();
        return 3;
    }

    Vn_Term();
    return 0;
}

} // namespace harkenhost
