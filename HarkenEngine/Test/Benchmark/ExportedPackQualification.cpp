#include <harken/Harken.h>
#include <harken_buildinfo.h>

#include "Engine.h"
#include "harken/core/AllocGuard.h"
#include "harken/core/EngineConfig.h"
#include "harken/media/PackFormat.h"
#include "harken/media/PackFormatV2.h"

#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <psapi.h>
#endif

namespace {

using Clock = std::chrono::steady_clock;

constexpr std::uint32_t kSampleRate = 48000;
constexpr std::uint32_t kBlockFrames = 256;
constexpr char kManifestName[] = "HarkenPackManifest.json";

struct ProcessMemory {
    std::uint64_t privateBytes = 0;
    std::uint64_t workingSetBytes = 0;
    bool available = false;
};

ProcessMemory ReadProcessMemorySample() {
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

std::string ReadText(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    if (!in) {
        return {};
    }
    return std::string((std::istreambuf_iterator<char>(in)),
                       std::istreambuf_iterator<char>());
}

std::uint64_t FileFnv(const std::filesystem::path& path) {
    const std::string bytes = ReadText(path);
    if (bytes.empty()) {
        return 0;
    }
    return harken::Fnv1a64(bytes.data(), bytes.size());
}

std::filesystem::path ExecutablePath() {
#if defined(_WIN32)
    char buffer[MAX_PATH];
    const DWORD n = GetModuleFileNameA(nullptr, buffer, MAX_PATH);
    if (n == 0 || n >= MAX_PATH) {
        return {};
    }
    return std::filesystem::path(buffer);
#else
    return {};
#endif
}

std::string JsonStringField(const std::string& object, const char* key) {
    const std::string needle = std::string("\"") + key + "\":";
    const auto at = object.find(needle);
    if (at == std::string::npos) {
        return {};
    }
    auto cursor = at + needle.size();
    while (cursor < object.size() &&
           (object[cursor] == ' ' || object[cursor] == '\t')) {
        ++cursor;
    }
    if (cursor >= object.size() || object[cursor] != '"') {
        return {};
    }
    ++cursor;
    std::string value;
    while (cursor < object.size() && object[cursor] != '"') {
        if (object[cursor] == '\\' && cursor + 1 < object.size()) {
            ++cursor;
        }
        value.push_back(object[cursor++]);
    }
    return value;
}

std::uint64_t ParseStableId(const std::string& text) {
    char* end = nullptr;
    const unsigned long long value = std::strtoull(text.c_str(), &end, 10);
    if (end == text.c_str() || *end != '\0') {
        return 0;
    }
    return static_cast<std::uint64_t>(value);
}

struct ManifestPack {
    std::string name;
    std::string path;
    std::uint64_t packStableId = 0;
};

std::vector<ManifestPack> ParseManifestPacks(const std::string& json) {
    std::vector<ManifestPack> packs;
    const auto packsAt = json.find("\"packs\"");
    if (packsAt == std::string::npos) {
        return packs;
    }
    auto cursor = json.find('[', packsAt);
    if (cursor == std::string::npos) {
        return packs;
    }
    ++cursor;
    while (cursor < json.size()) {
        const auto objectStart = json.find('{', cursor);
        if (objectStart == std::string::npos) {
            break;
        }
        const auto objectEnd = json.find('}', objectStart);
        if (objectEnd == std::string::npos) {
            break;
        }
        const std::string object =
            json.substr(objectStart, objectEnd - objectStart + 1);
        ManifestPack pack;
        pack.name = JsonStringField(object, "name");
        pack.path = JsonStringField(object, "path");
        pack.packStableId = ParseStableId(JsonStringField(object, "packStableId"));
        if (!pack.name.empty() && pack.packStableId != 0) {
            packs.push_back(pack);
        }
        cursor = objectEnd + 1;
        if (json.find('{', cursor) == std::string::npos) {
            break;
        }
    }
    return packs;
}

const ManifestPack* FindPack(const std::vector<ManifestPack>& packs,
                             const std::string& name) {
    for (const auto& pack : packs) {
        if (pack.name == name) {
            return &pack;
        }
    }
    return nullptr;
}

std::filesystem::path PackFilePath(const std::filesystem::path& root,
                                   const ManifestPack& pack) {
    if (pack.path.empty()) {
        return root / (pack.name + ".pack");
    }
    return root / std::filesystem::path(pack.path).filename();
}

VnResult ActivateExportedSet(const std::string& initPath,
                            std::uint64_t initStableId,
                            const std::string& root,
                            const ManifestPack* content,
                            VnCanonicalPackSetMemberResultV1 (&rows)[2],
                            VnCanonicalPackSetActivationReportV1& report,
                            VnPackHandle previous = 0,
                            std::uint64_t base = 0) {
    const std::string contentPath = content == nullptr ? std::string{} :
        PackFilePath(std::filesystem::path(root), *content).generic_string();
    VnCanonicalPackSetMemberV1 members[2]{};
    for (size_t i = 0; i < 2; ++i) {
        members[i].structSize = sizeof(members[i]);
        members[i].flags = VN_CANONICAL_PACK_SET_MEMBER_ROOT;
        members[i].utf8ContainmentRoot = root.c_str();
        rows[i].structSize = sizeof(rows[i]);
    }
    members[0].packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    members[0].packStableId = initStableId;
    members[0].utf8Path = initPath.c_str();
    members[0].expectedHandle = previous;
    if (content != nullptr) {
        members[1].packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
        members[1].packStableId = content->packStableId;
        members[1].utf8Path = contentPath.c_str();
    }
    VnCanonicalPackSetActivationDescV1 desc{};
    desc.structSize = sizeof(desc);
    desc.expectedCurrentInitHandle = previous;
    desc.baseSetRevision = base;
    desc.setRevision = base + 1;
    desc.memberCount = content == nullptr ? 1u : 2u;
    desc.memberStructSize = sizeof(members[0]);
    desc.members = members;
    desc.outputMemberCapacity = desc.memberCount;
    desc.outputMemberStructSize = sizeof(rows[0]);
    desc.outputMembers = rows;
    report.structSize = sizeof(report);
    return Vn_ActivateCanonicalPackSetV1(&desc, &report);
}

VnResult ReplaceCatalogRoot(VnPackHandle handle, std::uint64_t packStableId,
                            const std::string& nextRoot,
                            const std::string& nextPath) {
    VnPackLoadDesc desc{};
    desc.structSize = sizeof(desc);
    desc.flags = VN_PACK_LOAD_FLAG_ROOT;
    desc.packStableId = packStableId;
    desc.utf8Path = nextPath.c_str();
    desc.utf8ContainmentRoot = nextRoot.c_str();
    return Vn_ReplacePack(handle, reinterpret_cast<const VnPackDesc*>(&desc));
}

bool ParseUnsigned64(const char* text, std::uint64_t* value) {
    if (text == nullptr || value == nullptr) {
        return false;
    }
    char* end = nullptr;
    const unsigned long long parsed = std::strtoull(text, &end, 10);
    if (*text == '\0' || end == nullptr || *end != '\0') {
        return false;
    }
    *value = static_cast<std::uint64_t>(parsed);
    return true;
}

VnConfig MakeQualificationConfig() {
    VnConfig config{};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    config.sampleRate = kSampleRate;
    config.blockFrames = kBlockFrames;
    config.maxPacks = 8;
    config.maxActions = 64;
    return config;
}

VnResult InitQualification(EngineGuard* guard) {
    const VnConfig config = MakeQualificationConfig();
    const VnResult result = Vn_Init(&config);
    if (guard != nullptr) {
        guard->active = result == VN_OK;
    }
    return result;
}

void Pump(harken::Engine* engine) {
    (void)Vn_Update();
    if (engine != nullptr) {
        engine->nullBackend.Pump(1);
    }
}

void DrainFiring(harken::Engine* engine, VnActionFiringHandle firing) {
    if (engine == nullptr) {
        return;
    }
    for (int i = 0; i < 256; ++i) {
        Pump(engine);
        if (engine->playableInstances.LiveCount() == 0 &&
            engine->actionDeferredRecords.LiveCount() == 0 &&
            engine->voices.ActiveCount() == 0) {
            break;
        }
    }
    if (firing != 0) {
        (void)Vn_ReleaseActionFiring(firing);
    }
    for (int i = 0; i < 64; ++i) {
        Pump(engine);
        if (engine->packs.RetiringCount() == 0 &&
            engine->playableInstances.LiveCount() == 0) {
            break;
        }
    }
}

bool DrainRetiring(harken::Engine* engine) {
    if (engine == nullptr) {
        return false;
    }
    for (int i = 0; i < 256; ++i) {
        Pump(engine);
        if (engine->packs.RetiringCount() == 0) {
            return true;
        }
    }
    return engine->packs.RetiringCount() == 0;
}

VnObjectId FirstPlayableId(harken::Engine* engine) {
    if (engine == nullptr) {
        return {};
    }
    const auto* published = engine->packComposition.Published();
    if (published == nullptr) {
        return {};
    }
    for (const auto& entry : published->index) {
        if (entry.identityClass == harken::kInitCatalogIdentityClassPlayable &&
            entry.stableId != VnObjectId{}) {
            return entry.stableId;
        }
    }
    return {};
}

void ReleaseFillers(harken::Engine* engine,
                    std::vector<harken::PackIdentityLookup>* fillers) {
    if (engine == nullptr || fillers == nullptr) {
        return;
    }
    for (auto& extra : *fillers) {
        (void)engine->ReleasePackIdentityLease(extra.lease);
    }
    fillers->clear();
}

bool FillSlice(harken::Engine* engine, VnObjectId stableId,
               harken::PackLeaseSlice slice, std::size_t leaveFree,
               std::vector<harken::PackIdentityLookup>* fillers) {
    if (engine == nullptr || fillers == nullptr || stableId == VnObjectId{}) {
        return false;
    }
    for (;;) {
        harken::PackIdentityLookup extra;
        const VnResult acquired =
            engine->AcquirePackIdentityLease(stableId, slice, &extra);
        if (acquired == VN_BACKPRESSURE) {
            break;
        }
        if (acquired != VN_OK) {
            ReleaseFillers(engine, fillers);
            return false;
        }
        fillers->push_back(extra);
    }
    while (leaveFree > 0 && !fillers->empty()) {
        (void)engine->ReleasePackIdentityLease(fillers->back().lease);
        fillers->pop_back();
        --leaveFree;
    }
    return leaveFree == 0;
}

harken::PackIdentityReadiness PeekReadiness(harken::Engine* engine,
                                           VnObjectId stableId) {
    if (engine == nullptr) {
        return harken::PackIdentityReadiness::Absent;
    }
    harken::PackIdentityLookup peeked;
    const VnResult prepared =
        engine->packComposition.PrepareLookup(stableId, &peeked);
    if (prepared == VN_OK) {
        engine->packComposition.AbandonPreparedLease(peeked.lease);
    }
    return peeked.readiness;
}

std::size_t RemainingInSlice(harken::Engine* engine, VnObjectId stableId,
                             harken::PackLeaseSlice slice) {
    std::vector<harken::PackIdentityLookup> fillers;
    if (!FillSlice(engine, stableId, slice, 0, &fillers)) {
        return 0;
    }
    const std::size_t remaining = fillers.size();
    ReleaseFillers(engine, &fillers);
    return remaining;
}

VnResult PostQualificationAction(VnObjectId actionId, VnActionFiringHandle* firing) {
    VnPostActionDesc post{};
    post.structSize = sizeof(post);
    post.actionId = actionId;
    (void)Vn_CreateEmitter(&post.emitter);
    return Vn_PostAction(&post, firing);
}

struct ExportedPackLoad {
    VnPackHandle initHandle = 0;
    VnPackHandle contentHandle = 0;
    VnResult init = VN_DEVICE_ERROR;
    VnResult content = VN_DEVICE_ERROR;
};

ExportedPackLoad LoadExportedPack(const std::string& initPath, const std::string& root,
                                  const ManifestPack& init,
                                  const ManifestPack& content) {
    ExportedPackLoad loaded;
    VnCanonicalPackSetMemberResultV1 rows[2]{};
    VnCanonicalPackSetActivationReportV1 activation{};
    loaded.init = ActivateExportedSet(initPath, init.packStableId, root,
                                      &content, rows, activation);
    loaded.content = loaded.init;
    loaded.initHandle = rows[0].handle;
    loaded.contentHandle = rows[1].handle;
    return loaded;
}

bool ExportedPackLoadOk(const ExportedPackLoad& loaded) {
    return loaded.init == VN_OK && loaded.initHandle != 0 &&
           loaded.content == VN_OK && loaded.contentHandle != 0;
}

struct LeaseWindow {
    std::size_t capacity = 0;
    std::size_t playableUsed = 0;
    std::size_t sharedUsed = 0;
    std::size_t window = 0;
    bool ok = false;
};

LeaseWindow MeasureLeaseWindow(harken::Engine* engine, VnObjectId actionId,
                               VnObjectId playableId) {
    LeaseWindow measured;
    if (engine == nullptr || actionId == VnObjectId{} || playableId == VnObjectId{}) {
        return measured;
    }
    measured.capacity = harken::PackIdentityLeaseCapacity(engine->config);
    const std::size_t playableFree =
        RemainingInSlice(engine, playableId, harken::PackLeaseSlice::Playable);
    const std::size_t sharedFree = RemainingInSlice(
        engine, actionId, harken::PackLeaseSlice::SharedActionAndDeferred);
    VnActionFiringHandle firing = 0;
    if (PostQualificationAction(actionId, &firing) != VN_OK || firing == 0) {
        return measured;
    }
    for (int i = 0; i < 8; ++i) {
        Pump(engine);
    }
    const std::size_t playableFreeAfter =
        RemainingInSlice(engine, playableId, harken::PackLeaseSlice::Playable);
    const std::size_t sharedFreeAfter = RemainingInSlice(
        engine, actionId, harken::PackLeaseSlice::SharedActionAndDeferred);
    if (playableFreeAfter > playableFree || sharedFreeAfter > sharedFree) {
        DrainFiring(engine, firing);
        return measured;
    }
    measured.playableUsed = playableFree - playableFreeAfter;
    measured.sharedUsed = sharedFree - sharedFreeAfter;
    measured.window = measured.playableUsed + measured.sharedUsed;
    measured.ok = measured.window >= 1;
    DrainFiring(engine, firing);
    return measured;
}

VnResult ObserveMinusOne(const std::string& initPath, const std::string& root,
                         const ManifestPack& init, const ManifestPack& content,
                         VnObjectId actionId, const LeaseWindow& window) {
    EngineGuard guard;
    if (InitQualification(&guard) != VN_OK) {
        return VN_DEVICE_ERROR;
    }
    harken::Engine* engine = harken::detail::ActiveEngine();
    const ExportedPackLoad loaded = LoadExportedPack(initPath, root, init, content);
    if (engine == nullptr || !ExportedPackLoadOk(loaded)) {
        return VN_DEVICE_ERROR;
    }
    const VnObjectId playableId = FirstPlayableId(engine);
    if (playableId == VnObjectId{}) {
        return VN_UNKNOWN_ID;
    }
    std::vector<harken::PackIdentityLookup> playableFillers;
    std::vector<harken::PackIdentityLookup> sharedFillers;
    bool filled = false;
    if (window.playableUsed >= 1) {
        filled = FillSlice(engine, playableId, harken::PackLeaseSlice::Playable,
                           window.playableUsed - 1, &playableFillers);
    } else {
        filled = FillSlice(engine, actionId,
                           harken::PackLeaseSlice::SharedActionAndDeferred,
                           window.sharedUsed - 1, &sharedFillers);
    }
    VnActionFiringHandle firing = 0;
    const VnResult posted =
        filled ? PostQualificationAction(actionId, &firing) : VN_DEVICE_ERROR;
    DrainFiring(engine, firing);
    ReleaseFillers(engine, &playableFillers);
    ReleaseFillers(engine, &sharedFillers);
    return posted;
}

bool ObserveExactAndPlusOne(const std::string& initPath, const std::string& root,
                            const ManifestPack& init,
                            const ManifestPack& content, VnObjectId actionId,
                            const LeaseWindow& window, VnResult* exactResult,
                            VnResult* plusOneResult) {
    if (exactResult == nullptr || plusOneResult == nullptr) {
        return false;
    }
    *exactResult = VN_DEVICE_ERROR;
    *plusOneResult = VN_DEVICE_ERROR;
    EngineGuard guard;
    if (InitQualification(&guard) != VN_OK) {
        return false;
    }
    harken::Engine* engine = harken::detail::ActiveEngine();
    const ExportedPackLoad loaded = LoadExportedPack(initPath, root, init, content);
    if (engine == nullptr || !ExportedPackLoadOk(loaded)) {
        return false;
    }
    const VnObjectId playableId = FirstPlayableId(engine);
    if (playableId == VnObjectId{}) {
        return false;
    }
    std::vector<harken::PackIdentityLookup> playableFillers;
    std::vector<harken::PackIdentityLookup> sharedFillers;
    const bool filledPlayable =
        FillSlice(engine, playableId, harken::PackLeaseSlice::Playable,
                  window.playableUsed, &playableFillers);
    const bool filledShared = FillSlice(
        engine, actionId, harken::PackLeaseSlice::SharedActionAndDeferred,
        window.sharedUsed, &sharedFillers);
    if (!filledPlayable || !filledShared) {
        ReleaseFillers(engine, &playableFillers);
        ReleaseFillers(engine, &sharedFillers);
        return false;
    }
    VnActionFiringHandle firing = 0;
    *exactResult = PostQualificationAction(actionId, &firing);
    for (int i = 0; i < 8; ++i) {
        Pump(engine);
    }
    VnActionFiringHandle extra = 0;
    *plusOneResult = PostQualificationAction(actionId, &extra);
    DrainFiring(engine, extra);
    DrainFiring(engine, firing);
    ReleaseFillers(engine, &playableFillers);
    ReleaseFillers(engine, &sharedFillers);
    return true;
}

} // namespace

int main(int argc, char* argv[]) {
    std::filesystem::path exportedPackRoot;
    std::filesystem::path nextRoot;
    VnObjectId actionId{};
    std::string contentName = "ExportedPackVerticalContainer";
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (arg == "--exported-pack-root" && i + 1 < argc) {
            exportedPackRoot = argv[++i];
        } else if (arg == "--next-root" && i + 1 < argc) {
            nextRoot = argv[++i];
        } else if (arg == "--action" && i + 1 < argc) {
            if (!harken::ParseObjectId(argv[++i], &actionId)) {
                return 2;
            }
        } else if (arg == "--content" && i + 1 < argc) {
            contentName = argv[++i];
        } else {
            std::fprintf(stderr, "Unknown or incomplete argument\n");
            return 2;
        }
    }
    if (exportedPackRoot.empty() || actionId == VnObjectId{}) {
        std::fprintf(stderr, "--exported-pack-root and --action are required\n");
        return 2;
    }

    const std::string root = exportedPackRoot.generic_string();
    const std::string initPath = (exportedPackRoot / "Init.pack").generic_string();
    const std::string manifest = ReadText(exportedPackRoot / kManifestName);
    const auto packs = ParseManifestPacks(manifest);
    const ManifestPack* init = FindPack(packs, "Init");
    const ManifestPack* content = FindPack(packs, contentName);
    if (init == nullptr || content == nullptr) {
        std::fprintf(stderr, "exported pack manifest missing Init or content pack\n");
        return 1;
    }
    const std::filesystem::path contentPath = PackFilePath(exportedPackRoot, *content);
    const std::filesystem::path runnerPath = ExecutablePath();

    VnPackLoadDesc inspectDesc{};
    inspectDesc.structSize = sizeof(inspectDesc);
    inspectDesc.utf8Path = initPath.c_str();
    VnInitPackInspectReport inspection{};
    inspection.structSize = sizeof(inspection);
    VnResult inspectResult = VN_DEVICE_ERROR;
    const double inspectUs = MeasureUs([&] {
        inspectResult = Vn_InspectInitPack(&inspectDesc, &inspection);
    });
    if (inspectResult != VN_OK) {
        std::fprintf(stderr, "inspect failed\n");
        return 1;
    }

    LeaseWindow window;
    {
        EngineGuard guard;
        if (InitQualification(&guard) != VN_OK) {
            std::fprintf(stderr, "window init failed\n");
            return 1;
        }
        harken::Engine* engine = harken::detail::ActiveEngine();
        const ExportedPackLoad loaded = LoadExportedPack(initPath, root, *init, *content);
        if (engine == nullptr || !ExportedPackLoadOk(loaded)) {
            std::fprintf(stderr, "window load failed\n");
            return 1;
        }
        const VnObjectId playableId = FirstPlayableId(engine);
        window = MeasureLeaseWindow(engine, actionId, playableId);
        if (!window.ok) {
            std::fprintf(stderr, "exported pack resident window is empty\n");
            return 1;
        }
    }

    const VnResult minusOneResult =
        ObserveMinusOne(initPath, root, *init, *content, actionId, window);
    VnResult exactResult = VN_DEVICE_ERROR;
    VnResult plusOneResult = VN_DEVICE_ERROR;
    const bool exactObserved = ObserveExactAndPlusOne(
        initPath, root, *init, *content, actionId, window, &exactResult,
        &plusOneResult);
    const bool minusOneOk = minusOneResult == VN_BACKPRESSURE;
    const bool exactOk = exactObserved && exactResult == VN_OK;
    const bool plusOneOk = exactObserved && plusOneResult == VN_BACKPRESSURE;

    const ProcessMemory baseline = ReadProcessMemorySample();
    EngineGuard guard;
    const VnResult initResult = InitQualification(&guard);
    if (initResult != VN_OK) {
        std::fprintf(stderr, "inspect or init failed\n");
        return 1;
    }
    harken::Engine* engine = harken::detail::ActiveEngine();
    if (engine == nullptr) {
        return 1;
    }
    const ProcessMemory afterEngineInit = ReadProcessMemorySample();

    VnCanonicalPackSetMemberResultV1 initRows[2]{};
    VnCanonicalPackSetActivationReportV1 initial{};
    VnResult loadInitResult = VN_DEVICE_ERROR;
    const double loadInitUs = MeasureUs([&] {
        loadInitResult = ActivateExportedSet(initPath, init->packStableId, root,
                                             nullptr, initRows, initial);
    });
    const VnPackHandle initHandle = initRows[0].handle;
    VnCanonicalPackSetMemberResultV1 coldRows[2]{};
    VnCanonicalPackSetActivationReportV1 coldActivation{};
    VnResult coldLoadResult = VN_DEVICE_ERROR;
    const double coldLoadUs = MeasureUs([&] {
        coldLoadResult = ActivateExportedSet(initPath, init->packStableId, root,
            content, coldRows, coldActivation, initHandle, initial.resultSetRevision);
    });
    const ProcessMemory afterCold = ReadProcessMemorySample();
    const VnResult firstUnloadResult = Vn_UnloadPack(coldRows[1].handle);

    VnCanonicalPackSetMemberResultV1 warmRows[2]{};
    VnCanonicalPackSetActivationReportV1 warmActivation{};
    VnResult warmLoadResult = VN_DEVICE_ERROR;
    const double warmLoadUs = MeasureUs([&] {
        warmLoadResult = ActivateExportedSet(initPath, init->packStableId, root,
            content, warmRows, warmActivation, initHandle, 0);
    });
    VnPackHandle contentHandle = warmRows[1].handle;

    const auto loadedReadiness = PeekReadiness(engine, actionId);
    VnActionFiringHandle firing = 0;
    VnResult postResult = VN_DEVICE_ERROR;
    const double postUs = MeasureUs([&] {
        postResult = PostQualificationAction(actionId, &firing);
    });
    engine->nullBackend.Pump(8);
    for (int i = 0; i < 8; ++i) {
        Pump(engine);
    }
    const std::uint64_t audioHits = harken::memguard::HitCount();
    const ProcessMemory beforeWarmPost = ReadProcessMemorySample();
    VnActionFiringHandle warmFiring = 0;
    const VnResult warmPost = PostQualificationAction(actionId, &warmFiring);
    const ProcessMemory afterWarmPost = ReadProcessMemorySample();

    VnResult replaceResult = VN_OK;
    double replaceUs = 0.0;
    if (!nextRoot.empty()) {
        replaceResult = VN_BAD_PACK;
        const std::string nextRootText = nextRoot.generic_string();
        const auto nextManifest =
            ParseManifestPacks(ReadText(nextRoot / kManifestName));
        const ManifestPack* nextContent = FindPack(nextManifest, contentName);
        if (nextContent == nullptr) {
            std::fprintf(stderr, "successor manifest missing content pack\n");
        } else {
            std::filesystem::path nextFile = nextRoot / (contentName + ".pack");
            if (!nextContent->path.empty()) {
                const std::filesystem::path recorded(nextContent->path);
                nextFile = nextRoot / recorded.filename();
            }
            const std::string nextPath = nextFile.generic_string();
            replaceUs = MeasureUs([&] {
                replaceResult = ReplaceCatalogRoot(
                    contentHandle, nextContent->packStableId, nextRootText,
                    nextPath);
            });
        }
    }

    const VnResult refuseWhileLive =
        Vn_UnloadPackEx(contentHandle, VN_PACK_UNLOAD_REFUSE_IF_USED);
    DrainFiring(engine, firing);
    DrainFiring(engine, warmFiring);
    const VnResult refuseAfterDrain =
        Vn_UnloadPackEx(contentHandle, VN_PACK_UNLOAD_REFUSE_IF_USED);
    const VnResult refuseResult = refuseWhileLive;
    VnResult unloadResult = VN_DEVICE_ERROR;
    const double unloadUs = MeasureUs([&] {
        unloadResult = (refuseAfterDrain == VN_OK)
                           ? VN_OK
                           : Vn_UnloadPack(contentHandle);
        if (refuseAfterDrain == VN_OK) {
            contentHandle = 0;
        }
    });
    const bool retiringOk = DrainRetiring(engine);
    const std::size_t retiringCount = engine->packs.RetiringCount();
    const auto unloadedReadiness = PeekReadiness(engine, actionId);
    const ProcessMemory afterUnload = ReadProcessMemorySample();

    const harken::EngineConfig resolved = engine->config;
    const std::size_t leaseCapacity = harken::PackIdentityLeaseCapacity(resolved);
    std::fprintf(stderr,
                 "qual codes: init=%d cold=%d firstUnload=%d warm=%d "
                 "post=%d/%llu warmPost=%d refuse=%d replace=%d unload=%d "
                 "loaded=%u unloaded=%u retiring=%llu minus=%d exact=%d plus=%d "
                 "window=%llu\n",
                 loadInitResult, coldLoadResult,
                 firstUnloadResult, warmLoadResult, postResult,
                 static_cast<unsigned long long>(firing), warmPost, refuseResult,
                 replaceResult, unloadResult,
                 static_cast<unsigned>(loadedReadiness),
                 static_cast<unsigned>(unloadedReadiness),
                 static_cast<unsigned long long>(retiringCount), minusOneResult,
                 exactResult, plusOneResult,
                 static_cast<unsigned long long>(window.window));

    const bool behaviorOk = loadInitResult == VN_OK && initHandle != 0 &&
        coldLoadResult == VN_OK && firstUnloadResult == VN_OK &&
        warmLoadResult == VN_OK && postResult == VN_OK && firing != 0 &&
        warmPost == VN_OK && refuseResult == VN_IN_USE &&
        unloadResult == VN_OK && replaceResult == VN_OK &&
        loadedReadiness == harken::PackIdentityReadiness::Active &&
        unloadedReadiness == harken::PackIdentityReadiness::Absent &&
        audioHits == 0 && minusOneOk && exactOk && plusOneOk && retiringOk &&
        retiringCount == 0 && window.ok && leaseCapacity == window.capacity;

    std::printf(
        "{\"benchmark\":\"exported-pack-runtime\","
        "\"schema\":\"exported-pack-runtime-qualification-v1\","
        "\"load_timing_contract\":\"canonical-set-activate-v1\","
        "\"load_init_scope\":\"init-only-activation\","
        "\"content_timing_scope\":\"full-set-activation-including-validation\","
        "\"comparable_to_legacy_single_pack_load\":false,"
        "\"status\":\"%s\",\"release_build\":%s,"
        "\"source_sha\":\"%.*s\",\"build_commit\":\"%.*s\",\"build_dirty\":%s,"
        "\"binary_sha\":\"%016llx\",\"runner_sha\":\"%016llx\","
        "\"build_configuration\":\"%s\","
        "\"exported_pack_init_hash\":\"%016llx\","
        "\"exported_pack_content_hash\":\"%016llx\","
        "\"inspect_us\":%.3f,\"load_init_us\":%.3f,"
        "\"content_cold_us\":%.3f,\"content_warm_us\":%.3f,"
        "\"replace_us\":%.3f,\"unload_us\":%.3f,\"post_us\":%.3f,"
        "\"lease_capacity\":%llu,\"exported_pack_window\":%llu,"
        "\"max_action_firings\":%u,\"max_deferred_intents\":%u,"
        "\"max_playable_instances\":%u,"
        "\"minus_one_result\":%d,\"exact_result\":%d,\"plus_one_result\":%d,"
        "\"minus_one_ok\":%s,\"exact_ok\":%s,\"plus_one_ok\":%s,"
        "\"retiring_count\":%llu,"
        "\"audio_alloc_hits\":%llu,"
        "\"warm_post_private_delta\":%lld,"
        "\"memory_available\":%s,\"baseline_private_bytes\":%llu,"
        "\"engine_init_private_bytes\":%llu,"
        "\"content_private_bytes\":%llu,"
        "\"unload_private_bytes\":%llu}\n",
        behaviorOk ? "complete" : "failed",
#if defined(NDEBUG)
        "true",
#else
        "false",
#endif
        static_cast<int>(Harken::BuildInfo::GitCommit.size()),
        Harken::BuildInfo::GitCommit.data(),
        static_cast<int>(Harken::BuildInfo::GitCommit.size()),
        Harken::BuildInfo::GitCommit.data(),
        Harken::BuildInfo::GitDirty ? "true" : "false",
        static_cast<unsigned long long>(FileFnv(runnerPath)),
        static_cast<unsigned long long>(FileFnv(runnerPath)),
        HARKEN_QUALIFICATION_CONFIGURATION,
        static_cast<unsigned long long>(FileFnv(exportedPackRoot / "Init.pack")),
        static_cast<unsigned long long>(FileFnv(contentPath)),
        inspectUs, loadInitUs, coldLoadUs, warmLoadUs, replaceUs, unloadUs,
        postUs, static_cast<unsigned long long>(leaseCapacity),
        static_cast<unsigned long long>(window.window),
        resolved.maxActionFirings, resolved.maxDeferredIntents,
        resolved.maxPlayableInstances, minusOneResult, exactResult,
        plusOneResult, minusOneOk ? "true" : "false",
        exactOk ? "true" : "false", plusOneOk ? "true" : "false",
        static_cast<unsigned long long>(retiringCount),
        static_cast<unsigned long long>(audioHits),
        static_cast<long long>(afterWarmPost.privateBytes) -
            static_cast<long long>(beforeWarmPost.privateBytes),
        baseline.available ? "true" : "false",
        static_cast<unsigned long long>(baseline.privateBytes),
        static_cast<unsigned long long>(afterEngineInit.privateBytes),
        static_cast<unsigned long long>(afterCold.privateBytes),
        static_cast<unsigned long long>(afterUnload.privateBytes));
    std::fflush(stdout);
    return behaviorOk ? 0 : 1;
}
