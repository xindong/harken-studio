#include <harken/Harken.h>
#include <harken_buildinfo.h>

#include "Engine.h"
#include "Common/PackFoundationFixture.h"
#include "Common/ObjectIdTestSupport.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;
using harkentest::packfoundation::Fixture;
using namespace harkentest::packfoundation;

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

template <typename Fn>
double MeasureUs(Fn&& fn) {
    const auto start = Clock::now();
    fn();
    return std::chrono::duration<double, std::micro>(Clock::now() - start)
        .count();
}

struct EngineGuard {
    bool active = false;
    ~EngineGuard() {
        if (active) {
            (void)Vn_Term();
        }
    }
};

VnResult ActivateFixture(const std::filesystem::path& root,
                         bool includeContent,
                         VnCanonicalPackSetMemberResultV1 (&rows)[2],
                         VnCanonicalPackSetActivationReportV1& report,
                         VnPackHandle previous = 0,
                         std::uint64_t base = 0) {
    const std::string rootText = root.string();
    const std::string paths[] = {(root / "Init.pack").string(),
                                 (root / "Content.pack").string()};
    VnCanonicalPackSetMemberV1 members[2]{};
    for (size_t i = 0; i < 2; ++i) {
        members[i].structSize = sizeof(members[i]);
        members[i].flags = VN_CANONICAL_PACK_SET_MEMBER_ROOT;
        members[i].utf8Path = paths[i].c_str();
        members[i].utf8ContainmentRoot = rootText.c_str();
        rows[i].structSize = sizeof(rows[i]);
    }
    members[0].packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    members[0].packStableId = kInitPackStableId;
    members[0].expectedHandle = previous;
    members[1].packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
    members[1].packStableId = kResidentPackStableId;
    VnCanonicalPackSetActivationDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.expectedCurrentInitHandle = previous;
    desc.baseSetRevision = base;
    desc.setRevision = base + 1;
    desc.memberCount = includeContent ? 2u : 1u;
    desc.memberStructSize = sizeof(members[0]);
    desc.members = members;
    desc.outputMemberCapacity = desc.memberCount;
    desc.outputMemberStructSize = sizeof(rows[0]);
    desc.outputMembers = rows;
    report.structSize = sizeof(report);
    return Vn_ActivateCanonicalPackSetV1(&desc, &report);
}

VnResult ReplaceContent(const std::filesystem::path& root,
                        VnPackHandle handle) {
    const std::string rootText = root.string();
    const std::string path = (root / "ContentNext.pack").string();
    VnPackLoadDesc desc{};
    desc.structSize = sizeof(desc);
    desc.flags = VN_PACK_LOAD_FLAG_ROOT;
    desc.utf8Path = path.c_str();
    desc.utf8ContainmentRoot = rootText.c_str();
    return Vn_ReplacePack(handle,
                          reinterpret_cast<const VnPackDesc*>(&desc));
}

bool ParseUnsigned(const char* text, std::uint32_t* value) {
    if (text == nullptr || value == nullptr) {
        return false;
    }
    char* end = nullptr;
    const unsigned long parsed = std::strtoul(text, &end, 10);
    if (*text == '\0' || end == nullptr || *end != '\0' ||
        parsed > UINT32_MAX) {
        return false;
    }
    *value = static_cast<std::uint32_t>(parsed);
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    std::uint32_t catalogCount = 0;
    std::uint32_t residentCount = 0;
    std::filesystem::path root;
    for (int i = 1; i < argc; ++i) {
        if (std::string(argv[i]) == "--catalog" && i + 1 < argc) {
            if (!ParseUnsigned(argv[++i], &catalogCount)) {
                return 2;
            }
        } else if (std::string(argv[i]) == "--resident" && i + 1 < argc) {
            if (!ParseUnsigned(argv[++i], &residentCount)) {
                return 2;
            }
        } else if (std::string(argv[i]) == "--fixture-root" && i + 1 < argc) {
            root = std::filesystem::path(argv[++i]);
        } else {
            std::fprintf(stderr, "Unknown or incomplete argument\n");
            return 2;
        }
    }
    if (catalogCount == 0 || residentCount == 0 || root.empty()) {
        std::fprintf(stderr,
                     "--catalog, --resident and --fixture-root are required\n");
        return 2;
    }

    Fixture fixture;
    bool fixtureOk = false;
    const double exportUs = MeasureUs([&] {
        fixtureOk = BuildFixture(catalogCount, residentCount, &fixture) &&
                    WriteFixture(root, fixture);
    });
    if (!fixtureOk) {
        std::fprintf(stderr, "fixture export failed\n");
        return 1;
    }

    const std::string initPath = (root / "Init.pack").string();
    VnPackLoadDesc inspectDesc{};
    inspectDesc.structSize = sizeof(inspectDesc);
    inspectDesc.utf8Path = initPath.c_str();
    VnInitPackInspectReport inspection{};
    inspection.structSize = sizeof(inspection);
    VnResult inspectResult = VN_DEVICE_ERROR;
    const double inspectUs = MeasureUs([&] {
        inspectResult = Vn_InspectInitPack(&inspectDesc, &inspection);
    });

    VnConfig config{};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    config.maxPacks = 4;
    config.maxActions = residentCount;
    config.maxCatalogEntries = catalogCount;
    config.maxCatalogBytes = fixture.catalogBytes;
    config.maxResidentDefinitionBytes = fixture.initBodyBytes + fixture.residentBodyBytes;
    config.maxCompositeIndexBytes = 32ull << 20;
    config.maxPackDecodeStagingBytes = 256ull << 20;
    config.maxPackReplacementPeakBytes = 384ull << 20;
    const ProcessMemory baseline = ReadProcessMemory();
    const VnResult initResult = Vn_Init(&config);
    EngineGuard guard{initResult == VN_OK};
    if (inspectResult != VN_OK || initResult != VN_OK) {
        std::fprintf(stderr, "inspect or init failed\n");
        return 1;
    }
    const ProcessMemory afterEngineInit = ReadProcessMemory();

    VnCanonicalPackSetMemberResultV1 initRows[2]{};
    VnCanonicalPackSetActivationReportV1 initial{};
    VnResult loadInitResult = VN_DEVICE_ERROR;
    const double loadInitUs = MeasureUs([&] {
        loadInitResult = ActivateFixture(root, false, initRows, initial);
    });
    const VnPackHandle initHandle = initRows[0].handle;
    const ProcessMemory afterInitPack = ReadProcessMemory();

    VnPostActionDesc post{};
    post.structSize = sizeof(post);
    post.actionId = harkentest::ObjectId(kActionBase + residentCount);
    VnActionFiringHandle unloadedFiring = 0;
    VnResult unloadedPostResult = VN_DEVICE_ERROR;
    const double unloadedPostUs = MeasureUs([&] {
        unloadedPostResult = Vn_PostAction(&post, &unloadedFiring);
    });

    VnCanonicalPackSetMemberResultV1 coldRows[2]{};
    VnCanonicalPackSetActivationReportV1 coldActivation{};
    VnResult coldLoadResult = VN_DEVICE_ERROR;
    const double coldLoadUs = MeasureUs([&] {
        coldLoadResult = ActivateFixture(root, true, coldRows, coldActivation,
                                        initHandle, initial.resultSetRevision);
    });
    const ProcessMemory afterColdContent = ReadProcessMemory();
    const VnResult firstUnloadResult = Vn_UnloadPack(coldRows[1].handle);

    VnCanonicalPackSetMemberResultV1 warmRows[2]{};
    VnCanonicalPackSetActivationReportV1 warmActivation{};
    VnResult warmLoadResult = VN_DEVICE_ERROR;
    const double warmLoadUs = MeasureUs([&] {
        warmLoadResult = ActivateFixture(root, true, warmRows, warmActivation,
                                        initHandle, 0);
    });
    const VnPackHandle contentHandle = warmRows[1].handle;
    harken::Engine* engine = harken::detail::ActiveEngine();
    if (engine == nullptr) {
        return 1;
    }

    constexpr std::uint32_t kLookupIterations = 10000;
    volatile std::uintptr_t lookupSink = 0;
    const double loadedLookupTotalUs = MeasureUs([&] {
        for (std::uint32_t i = 0; i < kLookupIterations; ++i) {
            lookupSink = lookupSink ^ reinterpret_cast<std::uintptr_t>(
                engine->packComposition.FindAction(
                    harkentest::ObjectId(kActionBase + (i % residentCount))).programView);
        }
    });
    const double unloadedLookupTotalUs = MeasureUs([&] {
        const std::uint32_t unloadedCount = catalogCount - residentCount;
        for (std::uint32_t i = 0; i < kLookupIterations; ++i) {
            const auto found = engine->packComposition.Lookup(
                harkentest::ObjectId(kActionBase + residentCount + (i % unloadedCount)));
            lookupSink = lookupSink ^
                         static_cast<std::uintptr_t>(found.result);
        }
    });

    VnResult replaceResult = VN_DEVICE_ERROR;
    const double replaceUs = MeasureUs([&] {
        replaceResult = ReplaceContent(root, contentHandle);
    });
    const ProcessMemory afterReplace = ReadProcessMemory();
    const auto residentFacts = engine->packComposition.Facts();

    post.actionId = harkentest::ObjectId(kActionBase);
    VnActionFiringHandle loadedFiring = 0;
    VnResult loadedPostResult = VN_DEVICE_ERROR;
    const double loadedPostUs = MeasureUs([&] {
        loadedPostResult = Vn_PostAction(&post, &loadedFiring);
    });
    VnResult unloadResult = VN_DEVICE_ERROR;
    const double unloadUs = MeasureUs([&] {
        unloadResult = Vn_UnloadPack(contentHandle);
    });
    const std::size_t retiringBeforeRelease = engine->packs.RetiringCount();
    const ProcessMemory afterUnload = ReadProcessMemory();
    const VnResult releaseResult = Vn_ReleaseActionFiring(loadedFiring);
    const VnResult updateResult = Vn_Update();
    const std::size_t retiringCount = engine->packs.RetiringCount();

    const std::uint64_t residentFootprint =
        fixture.residentBodyBytes + residentFacts.loadedBytes;
    const bool behaviorOk = loadInitResult == VN_OK && initHandle != 0 &&
        unloadedPostResult == VN_CONTENT_NOT_LOADED && unloadedFiring == 0 &&
        coldLoadResult == VN_OK && firstUnloadResult == VN_OK &&
        warmLoadResult == VN_OK && replaceResult == VN_OK &&
        loadedPostResult == VN_OK && loadedFiring != 0 &&
        unloadResult == VN_OK && inspection.catalogEntries == catalogCount &&
        residentFacts.loadedCount == residentCount + 1 &&
        retiringBeforeRelease == 1 && releaseResult == VN_OK &&
        updateResult == VN_OK &&
        retiringCount == 0;

    std::printf(
        "{\"benchmark\":\"pack-foundation-runtime\","
        "\"schema\":\"plan05-f3-runtime-v1\","
        "\"load_timing_contract\":\"canonical-set-activate-v1\","
        "\"load_init_scope\":\"init-only-activation\","
        "\"content_timing_scope\":\"full-set-activation-including-validation\","
        "\"comparable_to_legacy_single_pack_load\":false,"
        "\"status\":\"%s\",\"release_build\":%s,"
        "\"build_commit\":\"%.*s\",\"build_dirty\":%s,"
        "\"build_configuration\":\"%s\","
        "\"catalog_count\":%u,\"resident_count\":%u,"
        "\"catalog_bytes\":%llu,\"resident_body_bytes\":%llu,"
        "\"composite_index_bytes\":%llu,"
        "\"resident_footprint_bytes\":%llu,"
        "\"replacement_formula_bytes\":%llu,"
        "\"export_us\":%.3f,\"inspect_us\":%.3f,"
        "\"load_init_us\":%.3f,\"content_cold_us\":%.3f,"
        "\"content_warm_us\":%.3f,\"replace_us\":%.3f,"
        "\"unload_us\":%.3f,\"lookup_loaded_us\":%.6f,"
        "\"lookup_unloaded_us\":%.6f,"
        "\"post_loaded_us\":%.3f,\"post_unloaded_us\":%.3f,"
        "\"memory_available\":%s,\"baseline_private_bytes\":%llu,"
        "\"engine_init_private_bytes\":%llu,"
        "\"init_pack_private_bytes\":%llu,"
        "\"content_private_bytes\":%llu,"
        "\"replace_private_bytes\":%llu,"
        "\"unload_private_bytes\":%llu,"
        "\"retiring_before_release\":%llu,\"retiring_count\":%llu}\n",
        behaviorOk ? "complete" : "failed",
#if defined(NDEBUG)
        "true",
#else
        "false",
#endif
        static_cast<int>(Harken::BuildInfo::GitCommit.size()),
        Harken::BuildInfo::GitCommit.data(),
        Harken::BuildInfo::GitDirty ? "true" : "false",
        HARKEN_QUALIFICATION_CONFIGURATION,
        catalogCount, residentCount,
        static_cast<unsigned long long>(fixture.catalogBytes),
        static_cast<unsigned long long>(fixture.residentBodyBytes),
        static_cast<unsigned long long>(residentFacts.loadedBytes),
        static_cast<unsigned long long>(residentFootprint),
        static_cast<unsigned long long>(fixture.residentBodyBytes * 2 +
                                        fixture.replacement.size()),
        exportUs, inspectUs, loadInitUs, coldLoadUs, warmLoadUs, replaceUs,
        unloadUs, loadedLookupTotalUs / kLookupIterations,
        unloadedLookupTotalUs / kLookupIterations, loadedPostUs,
        unloadedPostUs, baseline.available ? "true" : "false",
        static_cast<unsigned long long>(baseline.privateBytes),
        static_cast<unsigned long long>(afterEngineInit.privateBytes),
        static_cast<unsigned long long>(afterInitPack.privateBytes),
        static_cast<unsigned long long>(afterColdContent.privateBytes),
        static_cast<unsigned long long>(afterReplace.privateBytes),
        static_cast<unsigned long long>(afterUnload.privateBytes),
        static_cast<unsigned long long>(retiringBeforeRelease),
        static_cast<unsigned long long>(retiringCount));
    std::fflush(stdout);
    return behaviorOk ? 0 : 1;
}
