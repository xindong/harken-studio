// Engine-level v2 pack REPLACE behavior: the staged-revision path —
// whole-revision staging at the
// Vn_ReplacePack call, activation at the fixed Vn_Update phase, per-kind
// table semantics. Covers: the gap golden (every lookup between the call
// returning and the activation tick sees the OLD revision, proven by
// rendered output); the fault-injection golden (a late staging failure
// discards the staged revision with the old one fully serving, and held
// reservations release rather than leak); the per-kind table across one
// content and one resident replace (media generational retire-old /
// new-plays-new, container/Action registry swap, same-identity binding
// live revision, attachment/send/wiring diffs, ONE topology republish);
// the section-5.5 structural-identity rule (a binding whose variable
// changed is remove + add, refused while referenced); resident kinds
// never shrinking; the pending-gap gates (second replace, unload, kind
// flip); gap drift discarding the revision at activation with the named
// log record (including an external caller taking the exact attachment
// triple a staged revision adds); command-queue pressure deferring the
// activation one tick whole; provider protection over removals; the
// live in-place attenuation revise plus a meter wiring moved between
// buses; the net definition-capacity arithmetic (a revision's removals
// fund its adds); the attachment-side net arithmetic (only removals
// that are live AND pack-owned fund adds — an absent removal funds
// nothing); the gap rebirth of an identical attachment triple (caller
// detach + verbatim re-attach) discarding the revision instead of
// stealing the caller's triple; the gap recycle of a container or
// Action swap target (caller unregister + same-id re-register)
// discarding the revision at activation, and refusing VN_IN_USE at the
// call with zero staging footprint once the recycle precedes the
// replace; the revision-wide trigger-cycle and
// nesting screens (a double swap closing a cycle refuses at the call,
// a crossing swap that ends acyclic passes where a stale per-entry
// walk would refuse); the unchanged-entry truth of those walks (an
// unchanged entry resolves through the LIVE graph, so caller drift of
// it joins the screen — a drift-closed cycle refuses at the call,
// acyclic drift rides through and survives activation); the
// landing-name simulation over the final graph (a landing name held
// by a live caller Action refuses VN_IN_USE, a removal-freed name
// funds a same-name addition, a double swap exchanging two names
// activates, and a byte-patched revision whose two added Actions
// share one wire name — a shape no compiler output carries — refuses
// at the call with zero staging footprint); the activation re-screen's
// exception boundary (a
// scratch failure injected after a successful stage discards the
// staged revision whole with the named record, the old revision
// fully serving and the engine reusable); the single ordered
// child-run replay (two swaps
// cannot share one free run; a removal-freed run funds a swap's
// growth); and the v1 media-only
// observable equivalence plus the call-return vs tick activation
// vintage split. Pack files are compiled by the real packc tool from v2
// manifests (subprocess, the pipeline-test precedent); refusals assert
// the old revision through rendering, not internal state.
#include "Common/ObjectIdTestSupport.h"
#include "Common/ResourceCompilerTestUtil.h"
#include "Common/TestUtil.h"

#include "PackLogicStore.h"

#include "harken/core/LogRing.h"
#include "harken/media/PackFormatV2.h"
#include "harken/param/BindingTable.h"
#include "harken/render/BusTree.h"

#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the repository's .temp directory"
#endif

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeDC;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MakePlayDesc3D;
using harkentest::MonoDesc;
using harkentest::PumpAndCapture;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;

std::string TempPath(const std::string& name) {
    return std::string(HARKEN_TEMP_DIR "/") + name;
}


// Minimal IEEE float32 RIFF/WAVE writer (the pipeline test's shape):
// samples travel verbatim, so the pack blob carries the exact floats
// the test synthesized.
void WriteWavF32(const std::string& path,
                 const std::vector<float>& interleaved,
                 std::uint32_t channels, std::uint32_t sampleRate) {
    ASSERT_NE(0u, channels);
    ASSERT_EQ(0u, interleaved.size() % channels);
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good()) << "cannot open " << path;

    const std::uint32_t bytesPerSample = 4;
    const std::uint16_t blockAlign =
        static_cast<std::uint16_t>(channels * bytesPerSample);
    const std::uint32_t dataBytes =
        static_cast<std::uint32_t>(interleaved.size() * bytesPerSample);
    const std::uint32_t byteRate = sampleRate * blockAlign;
    const std::uint32_t fmtSize = 18;
    const std::uint32_t factSize = 4;
    const std::uint32_t riffSize =
        4 + (8 + fmtSize) + (8 + factSize) + (8 + dataBytes);
    const std::uint16_t formatIeeeFloat = 3;
    const std::uint16_t bitsPerSample = 32;
    const std::uint16_t cbSize = 0;
    const std::uint16_t channels16 = static_cast<std::uint16_t>(channels);
    const std::uint32_t sampleFrames =
        static_cast<std::uint32_t>(interleaved.size() / channels);

    auto write = [&](const void* data, std::size_t bytes) {
        out.write(static_cast<const char*>(data),
                  static_cast<std::streamsize>(bytes));
    };
    write("RIFF", 4);
    write(&riffSize, 4);
    write("WAVE", 4);
    write("fmt ", 4);
    write(&fmtSize, 4);
    write(&formatIeeeFloat, 2);
    write(&channels16, 2);
    write(&sampleRate, 4);
    write(&byteRate, 4);
    write(&blockAlign, 2);
    write(&bitsPerSample, 2);
    write(&cbSize, 2);
    write("fact", 4);
    write(&factSize, 4);
    write(&sampleFrames, 4);
    write("data", 4);
    write(&dataBytes, 4);
    write(interleaved.data(), interleaved.size() * sizeof(float));
    ASSERT_TRUE(out.good()) << "cannot write " << path;
}

// Writes the manifest and runs packc; false when the build refused.
[[nodiscard]] bool BuildPackFile(const std::string& name,
                                 const std::string& manifestJson) {
    const std::string manifestPath = TempPath(name + "_manifest.json");
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    {
        std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
        if (!out.good()) {
            return false;
        }
        out.write(manifestJson.data(),
                  static_cast<std::streamsize>(manifestJson.size()));
        if (!out.good()) {
            return false;
        }
    }
    return harkentest::CompileResources({manifestPath}) == 0;
}

[[nodiscard]] bool BuildMediaPackFile(const std::string& name,
                                   const std::string& entriesJson,
                                   const std::string& outputPath) {
    const std::string manifestJson = "{\"manifestVersion\":3,\"output\":\"" +
        outputPath + "\",\"entries\":" + entriesJson + "}";
    const std::string manifestPath = TempPath(name + "_manifest.json");
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    {
        std::ofstream out(manifestPath, std::ios::binary | std::ios::trunc);
        if (!out.good()) {
            return false;
        }
        out.write(manifestJson.data(),
                  static_cast<std::streamsize>(manifestJson.size()));
        if (!out.good()) {
            return false;
        }
    }
    return harkentest::CompileResources({manifestPath}) == 0;
}

std::string V2LogicManifest(const std::string& outputPath,
                            const char* packKind,
                            const std::string& sectionsJson) {
    std::string manifest = "{\n\"manifestVersion\": 3,\n\"output\": \"" +
                           outputPath + "\",\n\"packKind\": \"" +
                           std::string(packKind) + "\"";
    if (!sectionsJson.empty()) {
        manifest += ",\n" + sectionsJson;
    }
    manifest += "\n}\n";
    return manifest;
}

VnResult LoadPack(const std::string& path, VnPackHandle* outHandle) {
    VnPackDesc desc{};
    desc.structSize = sizeof(VnPackDesc);
    desc.utf8Path = path.c_str();
    return Vn_LoadPack(&desc, outHandle);
}

VnResult ReplacePack(VnPackHandle handle, const std::string& path) {
    VnPackDesc desc{};
    desc.structSize = sizeof(VnPackDesc);
    desc.utf8Path = path.c_str();
    return Vn_ReplacePack(handle, &desc);
}

VnSnapshot Snap() {
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    return snapshot;
}

bool PinCompletionOwner(harken::Engine& engine, VnVoiceHandle publicHandle,
                        VnVoiceHandle renderHandle, std::uint32_t ownerSlot,
                        std::uint64_t factsRevision,
                        harken::CompletionMutationBinding* out) {
    harken::OccurrenceToken occurrence{};
    harken::OccurrenceKey key{};
    key.clipId = harkentest::ObjectId(ownerSlot);
    if (out == nullptr ||
        !engine.timelineOccurrences.Acquire(key, &occurrence) ||
        !engine.timelineOccurrences.BindCompletionProgress(
            occurrence, static_cast<std::uint32_t>(renderHandle >> 32),
            factsRevision)) {
        return false;
    }
    const harken::CompletionOwnerRef owner =
        harken::CompletionOwnerRef::Single({ownerSlot, 1}, factsRevision);
    if (!engine.timelineOccurrences.TryPinCompletionOwner(occurrence, owner)) {
        return false;
    }
    const harken::CompletionMutationBinding binding{occurrence, owner};
    if (!engine.ledger.BindCompletionMutation(
            static_cast<std::uint32_t>(publicHandle), binding)) {
        return false;
    }
    *out = binding;
    return true;
}

float BusPeak(harken::Engine* engine, VnBusId busId) {
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    const std::uint32_t index = engine->buses.FindIndex(busId);
    EXPECT_LT(index, static_cast<std::uint32_t>(VN_SNAPSHOT_MAX_BUSES));
    return snapshot.busPeaks[index];
}

float DbToLin(float db) { return std::pow(10.0f, db / 20.0f); }

// Drains every pending logic log record (no log callback is configured,
// so call-time records stay in the ring until popped here). Records
// produced INSIDE Vn_Update are consumed by the update's own drain and
// need the callback sink below instead.
std::vector<harken::LogRecord> DrainLogicLog(harken::Engine* engine) {
    std::vector<harken::LogRecord> records;
    harken::LogRecord record{};
    while (engine->logicLogRing.TryPop(record)) {
        records.push_back(record);
    }
    return records;
}

std::size_t CountCode(const std::vector<harken::LogRecord>& records,
                      std::uint32_t code) {
    std::size_t n = 0;
    for (const harken::LogRecord& record : records) {
        if (record.code == code) {
            ++n;
        }
    }
    return n;
}

const harken::LogRecord* FindCode(const std::vector<harken::LogRecord>& records,
                                 std::uint32_t code) {
    for (const harken::LogRecord& record : records) {
        if (record.code == code) {
            return &record;
        }
    }
    return nullptr;
}

// Callback sink for records that surface during Vn_Update (the
// activation-time discard record): the update drains the logic ring
// itself, so only the configured callback sees them.
struct LogSink {
    struct Rec {
        std::uint32_t level;
        std::uint32_t code;
        std::string text;
    };
    std::vector<Rec> records;
    std::size_t Count(std::uint32_t code) const {
        std::size_t n = 0;
        for (const Rec& r : records) {
            if (r.code == code) {
                ++n;
            }
        }
        return n;
    }
    const Rec* Find(std::uint32_t code) const {
        for (const Rec& r : records) {
            if (r.code == code) {
                return &r;
            }
        }
        return nullptr;
    }
};
void LogCb(void* userData, std::uint32_t level, std::uint32_t code,
           const char* text) {
    static_cast<LogSink*>(userData)->records.push_back({level, code, text});
}

// Clears the staging fault even when an assertion aborts the test body,
// so one failed case never poisons the rest of the suite.
struct StagingFaultGuard {
    explicit StagingFaultGuard(harken::PackStagingFault fault) {
        harken::SetPackStagingFaultForTests(fault);
    }
    ~StagingFaultGuard() {
        harken::SetPackStagingFaultForTests(harken::PackStagingFault::None);
    }
    StagingFaultGuard(const StagingFaultGuard&) = delete;
    StagingFaultGuard& operator=(const StagingFaultGuard&) = delete;
};

void RegisterDc(VnSoundId id, float amplitude, std::size_t frames) {
    const std::vector<float> dc = MakeDC(frames, amplitude);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(id, &desc));
}

VnVoiceHandle PlayOnBus(VnSoundId soundId, VnBusId busId) {
    VnPlayDesc play = MakePlayDesc(soundId);
    play.busId = busId;
    VnVoiceHandle handle = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    return handle;
}

// Both captures must hold the same bit patterns: the refusal left the
// old revision fully in force, observed through rendering.
void ExpectSameCapture(const Capture& a, const Capture& b) {
    ASSERT_EQ(a.left.size(), b.left.size());
    ASSERT_EQ(a.right.size(), b.right.size());
    EXPECT_EQ(0, std::memcmp(a.left.data(), b.left.data(),
                             a.left.size() * sizeof(float)))
        << "left channel diverged from the pre-refusal render";
    EXPECT_EQ(0, std::memcmp(a.right.data(), b.right.data(),
                             a.right.size() * sizeof(float)))
        << "right channel diverged from the pre-refusal render";
}

TEST(PackReplaceStaging, VoiceRecipeSwapsOnlyAtActivationTick) {
    VnConfig config = MakeNullConfig(2, kBlock, kRate);
    config.fxSlotsPerVoice = 1;
    config.gainEffectPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::string wav = TempPath("pr_recipe.wav");
    WriteWavF32(wav, MakeDC(kBlock, 0.5f), 1, kRate);
    const std::string oldPath = TempPath("pr_recipe_old.pack");
    const std::string newPath = TempPath("pr_recipe_new.pack");
    const auto manifest = [&](const std::string& output, float gainDb) {
        return V2LogicManifest(
            output, "content",
            "\"entries\": [{\"path\": \"" + wav +
                "\", \"id\": \"3da91a64-b947-4560-0000-0000000026ad\"}],\n"
                "\"voiceRecipes\": [{\"playable\": \"3da91a64-b947-4560-0000-0000000026ad\", "
                "\"effects\": [{\"slot\": 0, \"type\": \"gain\", "
                "\"descriptorVintage\": 2, \"bypass\": false, "
                "\"parameters\": [" +
                std::to_string(gainDb) +
                "]}], \"bindings\": []}]");
    };
    ASSERT_TRUE(BuildPackFile("pr_recipe_old",
                              manifest(oldPath, -6.0f)));
    ASSERT_TRUE(BuildPackFile("pr_recipe_new",
                              manifest(newPath, -12.0f)));
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(oldPath, &pack));
    const auto gainOf = [&]() {
        const harken::PackLogicImage::VoiceRecipe* recipe =
            engine->logicPacks.FindVoiceRecipe(harkentest::ObjectId(9901));
        EXPECT_NE(nullptr, recipe);
        return recipe == nullptr ? 0.0f : recipe->installs[0].params[0];
    };
    EXPECT_FLOAT_EQ(-6.0f, gainOf());
    ASSERT_EQ(VN_OK, ReplacePack(pack, newPath));
    EXPECT_FLOAT_EQ(-6.0f, gainOf());
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FLOAT_EQ(-12.0f, gainOf());
}

// --- staging: the gap golden ---


// --- staging: the fault-injection golden ---


TEST(PackReplaceStaging, FaultedBusDiffReplaceReleasesReservations) {
    VnConfig config = MakeNullConfig(4, kBlock, kRate);
    // One live plan slot plus one free: a leaked plan reservation would
    // turn the second faulted attempt into VN_BACKPRESSURE and make the
    // final clean replace impossible.
    config.topologyPlanPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string v1Path = TempPath("pr_resv_v1.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_resv_v1",
        V2LogicManifest(v1Path, "resident",
                        R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000001069", "parent": "00000000-0000-0000-0000-000000000001" } ])")));
    // The revision adds a send edge, so staging must reserve a plan slot
    // at the call and hold it across the gap.
    const std::string v2Path = TempPath("pr_resv_v2.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_resv_v2",
        V2LogicManifest(
            v2Path, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000001069", "parent": "00000000-0000-0000-0000-000000000001" } ],
"busSends": [ { "from": "3da91a64-b947-4560-0000-000000001069", "to": "00000000-0000-0000-0000-000000000001", "levelDb": -6.0 } ])")));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(v1Path, &pack));
    // Let the render thread adopt the load's plan so its predecessor
    // retires and exactly one pool slot is free from here on.
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    RegisterDc(harkentest::ObjectId(4211), 0.5f, kBlock * 32);
    (void)PlayOnBus(harkentest::ObjectId(4211), harkentest::ObjectId(4201));
    Capture before;
    PumpAndCapture(engine, 1, before);
    HARKENTEST_ASSERT_NON_SILENT(before.left);
    EXPECT_FLOAT_EQ(0.5f, before.left[kBlock / 2]); // tree path only

    // Two consecutive BeforeApply faults: each acquires the single free
    // plan slot and must give it back — a leak would surface as
    // VN_BACKPRESSURE on the second attempt.
    for (int attempt = 0; attempt < 2; ++attempt) {
        StagingFaultGuard guard(harken::PackStagingFault::BeforeApply);
        EXPECT_EQ(VN_BAD_PACK, ReplacePack(pack, v2Path)) << attempt;
        EXPECT_TRUE(engine->logicPacks.pending.empty()) << attempt;
    }

    // Clean staging succeeds on the same slot; the gap still renders the
    // old graph, the activation publishes the send.
    ASSERT_EQ(VN_OK, ReplacePack(pack, v2Path));
    Capture gap;
    PumpAndCapture(engine, 1, gap);
    EXPECT_FLOAT_EQ(0.5f, gap.left[kBlock / 2]);
    const std::uint64_t plansBefore = Snap().topologyPlanCount;
    ASSERT_EQ(VN_OK, Vn_Update());
    Capture after;
    PumpAndCapture(engine, 1, after);
    // Master hears the tree path plus the send copy.
    const float expected = 0.5f + 0.5f * DbToLin(-6.0f);
    EXPECT_NEAR(expected, after.left[kBlock / 2], 1e-4f);
    EXPECT_EQ(plansBefore + 1, Snap().topologyPlanCount);
}

// --- staging: pending-gap gates and the kind-identity gate ---

TEST(PackReplaceStaging, SecondReplaceUnloadAndKindFlipGates) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string residentPath = TempPath("pr_gate_res.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_gate_res",
        V2LogicManifest(residentPath, "resident",
                        R"("buses": [ { "id": "3da91a64-b947-4560-0000-0000000010cd", "parent": "00000000-0000-0000-0000-000000000001" } ])")));
    const std::string wavOld = TempPath("pr_gate_old.wav");
    const std::string wavNew = TempPath("pr_gate_new.wav");
    WriteWavF32(wavOld, MakeDC(kBlock, 0.25f), 1, kRate);
    WriteWavF32(wavNew, MakeDC(kBlock, 0.5f), 1, kRate);
    const std::string conV1 = TempPath("pr_gate_con_v1.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_gate_con_v1",
        V2LogicManifest(conV1, "content",
                        "\"entries\": [ { \"path\": \"" + wavOld +
                            "\", \"id\": \"3da91a64-b947-4560-0000-0000000010d7\" } ]")));
    const std::string conV2 = TempPath("pr_gate_con_v2.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_gate_con_v2",
        V2LogicManifest(conV2, "content",
                        "\"entries\": [ { \"path\": \"" + wavNew +
                            "\", \"id\": \"3da91a64-b947-4560-0000-0000000010d7\" } ]")));

    VnPackHandle resident = 0;
    VnPackHandle content = 0;
    ASSERT_EQ(VN_OK, LoadPack(residentPath, &resident));
    ASSERT_EQ(VN_OK, LoadPack(conV1, &content));

    // The pack kind is identity: a revision of the other kind refuses in
    // both directions, and both packs keep serving untouched.
    const std::uint64_t refusedBefore = Snap().hotReloadRefusedCount;
    EXPECT_EQ(VN_INVALID_ARG, ReplacePack(content, residentPath));
    EXPECT_EQ(VN_INVALID_ARG, ReplacePack(resident, conV1));
    EXPECT_EQ(refusedBefore + 2, Snap().hotReloadRefusedCount);
    EXPECT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(4301), 0.0f, 0));
    EXPECT_TRUE(engine->registry.Lookup(harkentest::ObjectId(4311)).Valid());

    // One staged revision per pack: while the gap is open the second
    // replace and the unload both refuse.
    ASSERT_EQ(VN_OK, ReplacePack(content, conV2));
    EXPECT_EQ(VN_IN_USE, ReplacePack(content, conV2));
    EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(content));

    // The gap still renders the old media.
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(4311));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    Capture gap;
    PumpAndCapture(engine, 1, gap);
    EXPECT_FLOAT_EQ(0.25f, gap.left[kBlock / 2]);

    // After the activation pump both verbs behave normally again.
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    Capture swapped;
    PumpAndCapture(engine, 1, swapped);
    EXPECT_FLOAT_EQ(0.5f, swapped.left[kBlock / 2]);

    ASSERT_EQ(VN_OK, ReplacePack(content, conV1));
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    Capture reverted;
    PumpAndCapture(engine, 1, reverted);
    EXPECT_FLOAT_EQ(0.25f, reverted.left[kBlock / 2]);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update()); // drain the last generation sweep
    EXPECT_EQ(VN_OK, Vn_UnloadPack(content));
    EXPECT_FALSE(engine->registry.Lookup(harkentest::ObjectId(4311)).Valid());
}

// --- staging: gap drift discards at activation ---


TEST(PackReplaceStaging, GapExternalAttachmentCollisionDiscardsRevision) {
    LogSink sink;
    VnConfig config = MakeNullConfig(4, kBlock, kRate);
    config.logCallback = &LogCb;
    config.logUserData = &sink;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // Media M1/M2 and the SAME definition in both revisions; only the
    // attachment moves M1 -> M2, so the diff is remove (B, voice, M1)
    // plus add (B, voice, M2) with the definition riding through
    // unchanged — the added triple is neither a removed twin nor on an
    // unregistered definition, so its admission re-runs at activation.
    const std::string wavA = TempPath("pr_gcol_m1.wav");
    const std::string wavB = TempPath("pr_gcol_m2.wav");
    WriteWavF32(wavA, MakeDC(kBlock, 0.25f), 1, kRate);
    WriteWavF32(wavB, MakeDC(kBlock, 0.5f), 1, kRate);
    const auto manifest = [&](const std::string& out, std::uint64_t object) {
        return V2LogicManifest(
            out, "content",
            "\"entries\": [ { \"path\": \"" + wavA +
                "\", \"id\": \"3da91a64-b947-4560-0000-000000001325\" },\n  { \"path\": \"" + wavB +
                "\", \"id\": \"3da91a64-b947-4560-0000-000000001326\" } ],\n"
                "\"bindings\": [ { \"id\": \"3da91a64-b947-4560-0000-00000000132f\", \"variable\": \"3da91a64-b947-4560-0000-000000001e65\", "
                "\"scope\": \"voice\", \"target\": 0,\n"
                "    \"curve\": { \"points\": [[0.0, 0.0], [1.0, -6.0]],\n"
                "               \"handles\": [[0.0, 0.0, 1.0, -6.0]] } } ],\n"
                "\"attachments\": [ { \"binding\": \"3da91a64-b947-4560-0000-00000000132f\", \"scope\": "
                "\"voice\", \"object\": " +
                harkentest::JsonObjectId(harkentest::ObjectId(object)) + " } ]");
    };
    const std::string v1Path = TempPath("pr_gcol_v1.pack");
    const std::string v2Path = TempPath("pr_gcol_v2.pack");
    ASSERT_TRUE(BuildPackFile("pr_gcol_v1", manifest(v1Path, 4901)));
    ASSERT_TRUE(BuildPackFile("pr_gcol_v2", manifest(v2Path, 4902)));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(v1Path, &pack));
    ASSERT_EQ(VN_OK, ReplacePack(pack, v2Path));

    // The gap: an external caller attaches the EXACT triple the staged
    // revision wants to add.
    VnBindingAttachDesc external{};
    external.structSize = sizeof(VnBindingAttachDesc);
    external.scopeKind = VN_TARGET_SCOPE_VOICE;
    external.objectId = harkentest::ObjectId(4902);
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(4911), &external));

    // Activation re-screens against the drifted state: the duplicate
    // triple refuses admission and the revision discards WHOLE, with
    // the named record surfacing through the sink.
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_TRUE(engine->logicPacks.pending.empty());
    EXPECT_EQ(1u, sink.Count(harken::kLogCodePackRevisionDiscarded));

    // The old revision is fully in force: the pack's own (B, voice, M1)
    // still stands beside the caller's (B, voice, M2), and the old
    // media renders at the old amplitude.
    EXPECT_EQ(2u, engine->bindings.AttachmentCountOf(harkentest::ObjectId(4911)));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(4901));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    Capture kept;
    PumpAndCapture(engine, 1, kept);
    HARKENTEST_ASSERT_NON_SILENT(kept.left);
    EXPECT_FLOAT_EQ(0.25f, kept.left[kBlock / 2]);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update()); // drain the finished voice

    // Ownership never corrupted: the caller's triple did not become
    // pack-owned, so the unload gate sees one live attachment beyond
    // the pack's own and refuses whole.
    EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
    EXPECT_TRUE(engine->registry.Lookup(harkentest::ObjectId(4901)).Valid());
    EXPECT_EQ(1u, engine->packs.LoadedCount());

    // Detaching the foreign triple releases the pack.
    ASSERT_EQ(VN_OK, Vn_DetachBinding(harkentest::ObjectId(4911), &external));
    EXPECT_EQ(VN_OK, Vn_UnloadPack(pack));
    EXPECT_FALSE(engine->registry.Lookup(harkentest::ObjectId(4901)).Valid());
    EXPECT_EQ(nullptr, engine->bindings.Find(harkentest::ObjectId(4911)));
}

// --- staging: queue pressure defers the activation whole ---

TEST(PackReplaceStaging, QueuePressureDefersActivationOneTickWhole) {
    VnConfig config = MakeNullConfig(4, kBlock, kRate);
    config.commandQueueDepth = 2; // rounds to 2; easy to fill
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string v1Path = TempPath("pr_defer_v1.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_defer_v1",
        V2LogicManifest(v1Path, "resident",
                        R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000001195", "parent": "00000000-0000-0000-0000-000000000001" } ])")));
    const std::string v2Path = TempPath("pr_defer_v2.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_defer_v2",
        V2LogicManifest(v2Path, "resident",
                        R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000001195", "parent": "00000000-0000-0000-0000-000000000001" },
           { "id": "3da91a64-b947-4560-0000-000000001196", "parent": "3da91a64-b947-4560-0000-000000001195" } ])")));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(v1Path, &pack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    // Staging needs no command admission — that is the activation's own
    // re-check — so the replace stages against a soon-to-be-full queue.
    ASSERT_EQ(VN_OK, ReplacePack(pack, v2Path));

    // Fill the command queue without pumping (the pressure technique).
    VnResult r = VN_OK;
    int pushed = 0;
    while ((r = Vn_SetBusVolume(VN_BUS_MASTER, -1.0f, 0)) == VN_OK &&
           pushed < 64) {
        ++pushed;
    }
    ASSERT_EQ(VN_BACKPRESSURE, r);

    // First update: the plan publish cannot be admitted, so the WHOLE
    // revision defers one tick and every lookup keeps seeing the old
    // revision — the added bus does not exist yet.
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1u, engine->logicPacks.pending.size());
    EXPECT_EQ(harken::kInvalidBusIndex, engine->buses.FindIndex(harkentest::ObjectId(4502)));

    // One rendered block drains the queue; the next update activates.
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_TRUE(engine->logicPacks.pending.empty());
    EXPECT_NE(harken::kInvalidBusIndex, engine->buses.FindIndex(harkentest::ObjectId(4502)));
    EXPECT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(4502), -1.0f, 0));
}

TEST(PackReplaceStaging,
     SameGenerationEffectClaimGatesReviseRemoveAndActivationWhole) {
    VnConfig config = MakeNullConfig(2, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const auto manifest = [](const std::string& output,
                             const char* effectJson) {
        std::string sections =
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-00000000119f", "parent": "00000000-0000-0000-0000-000000000001" } ])";
        if (effectJson != nullptr) {
            sections += ",\n\"busEffects\": [ ";
            sections += effectJson;
            sections += " ]";
        }
        return V2LogicManifest(output, "resident", sections);
    };
    const char* oldEffect =
        R"({ "bus": "00000000-0000-0000-0000-000000000001", "slot": 0, "type": "compressor", "descriptorVintage": 2, "bypass": false, "faderPointGuard": 1, "parameters": [-18.0, 4.0, 10.0, 120.0, 6.0, 0.0] })";
    const char* revisedEffect =
        R"({ "bus": "00000000-0000-0000-0000-000000000001", "slot": 0, "type": "compressor", "descriptorVintage": 2, "bypass": false, "faderPointGuard": 1, "parameters": [-24.0, 4.0, 10.0, 120.0, 6.0, 0.0] })";
    const std::string oldPath = TempPath("pr_claim_old.pack");
    const std::string revisedPath = TempPath("pr_claim_revised.pack");
    const std::string removedPath = TempPath("pr_claim_removed.pack");
    ASSERT_TRUE(BuildPackFile("pr_claim_old",
                              manifest(oldPath, oldEffect)));
    ASSERT_TRUE(BuildPackFile("pr_claim_revised",
                              manifest(revisedPath, revisedEffect)));
    ASSERT_TRUE(BuildPackFile("pr_claim_removed",
                              manifest(removedPath, nullptr)));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(oldPath, &pack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const std::uint32_t master = engine->buses.FindIndex(VN_BUS_MASTER);
    ASSERT_NE(harken::kInvalidBusIndex, master);
    const std::uint32_t generation = engine->buses.GenerationAt(master);
    std::uint32_t revision = 0;
    ASSERT_TRUE(engine->effectParamTargets.ExactTargetRevision(
        harken::EffectParamTargetStore::Scope::Bus, master, generation, 0,
        harken::kCompressorParamMakeupDb, VN_EFFECT_COMPRESSOR, &revision));
    const harken::VariableSliceRef reviseOwner{0, 101};
    ASSERT_TRUE(engine->effectParamTargets.TryClaim(
        harken::EffectParamTargetStore::Scope::Bus, master, generation, 0,
        harken::kCompressorParamMakeupDb, reviseOwner, revision));
    const std::uint64_t commandsBeforeRevise =
        engine->commands.PublishedHead();

    EXPECT_EQ(VN_BACKPRESSURE, ReplacePack(pack, revisedPath));
    EXPECT_EQ(nullptr, engine->logicPacks.PendingFor(pack));
    EXPECT_EQ(commandsBeforeRevise, engine->commands.PublishedHead());
    EXPECT_EQ(0u,
              engine->effectParamTargets.ReservedRecordCountForTests());
    const harken::PackLogicRecord* record =
        engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, record);
    ASSERT_EQ(1u, record->busEffects.size());
    EXPECT_FLOAT_EQ(-18.0f, record->busEffects[0].params[0]);
    EXPECT_FLOAT_EQ(0.0f, record->busEffects[0].params[5]);
    EXPECT_FLOAT_EQ(0.0f,
                    engine->effectParamTargets.LastCommitted(
                        harken::EffectParamTargetStore::Scope::Bus, master, 0,
                        harken::kCompressorParamMakeupDb));

    engine->effectParamTargets.ReleaseClaim(
        harken::EffectParamTargetStore::Scope::Bus, master, generation, 0,
        harken::kCompressorParamMakeupDb, reviseOwner);
    ASSERT_EQ(VN_OK, ReplacePack(pack, revisedPath));
    ASSERT_NE(nullptr, engine->logicPacks.PendingFor(pack));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->logicPacks.PendingFor(pack));
    EXPECT_EQ(0u,
              engine->effectParamTargets.ReservedRecordCountForTests());
    record = engine->logicPacks.RecordFor(pack);
    ASSERT_NE(nullptr, record);
    ASSERT_EQ(1u, record->busEffects.size());
    EXPECT_FLOAT_EQ(-24.0f, record->busEffects[0].params[0]);
    EXPECT_FLOAT_EQ(0.0f, record->busEffects[0].params[5]);

    ASSERT_TRUE(engine->effectParamTargets.ExactTargetRevision(
        harken::EffectParamTargetStore::Scope::Bus, master, generation, 0,
        harken::kCompressorParamMakeupDb, VN_EFFECT_COMPRESSOR, &revision));
    const harken::VariableSliceRef removeOwner{2, 101};
    ASSERT_TRUE(engine->effectParamTargets.TryClaim(
        harken::EffectParamTargetStore::Scope::Bus, master, generation, 0,
        harken::kCompressorParamMakeupDb, removeOwner, revision));
    const std::uint64_t commandsBeforeRemove =
        engine->commands.PublishedHead();
    EXPECT_EQ(VN_BACKPRESSURE, ReplacePack(pack, removedPath));
    EXPECT_EQ(nullptr, engine->logicPacks.PendingFor(pack));
    EXPECT_EQ(commandsBeforeRemove, engine->commands.PublishedHead());
    EXPECT_EQ(VN_EFFECT_COMPRESSOR,
              engine->busEffectSlotTypes[master][0]);

    engine->effectParamTargets.ReleaseClaim(
        harken::EffectParamTargetStore::Scope::Bus, master, generation, 0,
        harken::kCompressorParamMakeupDb, removeOwner);
    ASSERT_EQ(VN_OK, ReplacePack(pack, removedPath));
    ASSERT_NE(nullptr, engine->logicPacks.PendingFor(pack));
    const harken::VariableSliceRef activationOwner{3, 101};
    ASSERT_TRUE(engine->effectParamTargets.TryClaim(
        harken::EffectParamTargetStore::Scope::Bus, master, generation, 0,
        harken::kCompressorParamMakeupDb, activationOwner, revision));
    const std::uint64_t commandsBeforeActivation =
        engine->commands.PublishedHead();
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_NE(nullptr, engine->logicPacks.PendingFor(pack));
    EXPECT_EQ(commandsBeforeActivation, engine->commands.PublishedHead());
    EXPECT_EQ(VN_EFFECT_COMPRESSOR,
              engine->busEffectSlotTypes[master][0]);
    EXPECT_EQ(harken::kCompressorParamCount,
              engine->effectParamTargets.LiveRecordCount());

    engine->effectParamTargets.ReleaseClaim(
        harken::EffectParamTargetStore::Scope::Bus, master, generation, 0,
        harken::kCompressorParamMakeupDb, activationOwner);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->logicPacks.PendingFor(pack));
    EXPECT_EQ(harken::kEffectTypeNone,
              engine->busEffectSlotTypes[master][0]);
    EXPECT_EQ(0u, engine->effectParamTargets.LiveRecordCount());
}

TEST(PackReplaceStaging,
     SameGenerationSurvivingEffectClaimGatesResidentUnloadWhole) {
    VnConfig config = MakeNullConfig(2, kBlock, kRate);
    config.fxSlotsPerBus = 1;
    config.compressorEffectPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::string path = TempPath("pr_claim_unload.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_claim_unload",
        V2LogicManifest(
            path, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-0000000011a9", "parent": "00000000-0000-0000-0000-000000000001" } ],
"busEffects": [ { "bus": "00000000-0000-0000-0000-000000000001", "slot": 0, "type": "compressor", "descriptorVintage": 2, "bypass": false, "faderPointGuard": 1, "parameters": [-18.0, 4.0, 10.0, 120.0, 6.0, 0.0] } ])")));
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(path, &pack));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const std::uint32_t master = engine->buses.FindIndex(VN_BUS_MASTER);
    ASSERT_NE(harken::kInvalidBusIndex, master);
    const std::uint32_t generation = engine->buses.GenerationAt(master);
    std::uint32_t revision = 0;
    ASSERT_TRUE(engine->effectParamTargets.ExactTargetRevision(
        harken::EffectParamTargetStore::Scope::Bus, master, generation, 0,
        harken::kCompressorParamMakeupDb, VN_EFFECT_COMPRESSOR, &revision));
    const harken::VariableSliceRef owner{0, 102};
    ASSERT_TRUE(engine->effectParamTargets.TryClaim(
        harken::EffectParamTargetStore::Scope::Bus, master, generation, 0,
        harken::kCompressorParamMakeupDb, owner, revision));
    const std::uint64_t commandHead = engine->commands.PublishedHead();
    const std::uint32_t loaded = engine->packs.LoadedCount();

    EXPECT_EQ(VN_BACKPRESSURE, Vn_UnloadPack(pack));
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_EQ(loaded, engine->packs.LoadedCount());
    EXPECT_EQ(pack, engine->busEffectOwnerCookies[master][0]);
    EXPECT_EQ(VN_EFFECT_COMPRESSOR,
              engine->busEffectSlotTypes[master][0]);
    EXPECT_EQ(harken::kCompressorParamCount,
              engine->effectParamTargets.LiveRecordCount());

    engine->effectParamTargets.ReleaseClaim(
        harken::EffectParamTargetStore::Scope::Bus, master, generation, 0,
        harken::kCompressorParamMakeupDb, owner);
    ASSERT_EQ(VN_OK, Vn_UnloadPack(pack));
    EXPECT_EQ(loaded - 1u, engine->packs.LoadedCount());
    EXPECT_EQ(0u, engine->busEffectOwnerCookies[master][0]);
    EXPECT_EQ(harken::kEffectTypeNone,
              engine->busEffectSlotTypes[master][0]);
    EXPECT_EQ(0u, engine->effectParamTargets.LiveRecordCount());
}

// --- semantics: the per-kind table, content half ---


// --- semantics: the per-kind table, resident half ---

TEST(PackReplaceSemantics, ResidentReplaceAppliesDiffWithOnePlanPublish) {
    VnConfig config = MakeNullConfig(8, kBlock, kRate);
    config.maxBuses = 16;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const harken::VariableDefinitionV1 definitions[] = {
        harkentest::MakeVariableDefinition(harkentest::ObjectId(7731), harken::VariableKindV1::Step,
                                         0.0f, 1.0f, 0.0f),
        harkentest::MakeVariableDefinition(harkentest::ObjectId(7732), harken::VariableKindV1::Smooth,
                                         0.0f, 1.0f, 0.0f),
        harkentest::MakeVariableDefinition(harkentest::ObjectId(7733), harken::VariableKindV1::Smooth,
                                         0.0f, 1.0f, 0.0f)};
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallDefinitionsWorld(engine, definitions, 3).status);

    // Loaded revision: five buses; two sends into 3004; three binding
    // definitions driven by the installed world, with two attachments on
    // 3001 and one definition (3013) left unattached.
    const std::string v1Path = TempPath("pr_rt_v1.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_rt_v1",
        V2LogicManifest(
            v1Path, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000000bb9", "parent": "00000000-0000-0000-0000-000000000001" },
           { "id": "3da91a64-b947-4560-0000-000000000bba", "parent": "00000000-0000-0000-0000-000000000001" },
           { "id": "3da91a64-b947-4560-0000-000000000bbb", "parent": "00000000-0000-0000-0000-000000000001" },
           { "id": "3da91a64-b947-4560-0000-000000000bbc", "parent": "00000000-0000-0000-0000-000000000001" },
           { "id": "3da91a64-b947-4560-0000-000000000bbd", "parent": "00000000-0000-0000-0000-000000000001" } ],
"busSends": [ { "from": "3da91a64-b947-4560-0000-000000000bbb", "to": "3da91a64-b947-4560-0000-000000000bbc", "levelDb": 0.0 },
              { "from": "3da91a64-b947-4560-0000-000000000bbd", "to": "3da91a64-b947-4560-0000-000000000bbc", "levelDb": -6.0 } ],
"bindings": [ { "id": "3da91a64-b947-4560-0000-000000000bc3", "variable": "3da91a64-b947-4560-0000-000000001e33", "scope": "bus", "target": 0,
                "curve": { "points": [[0.0, 0.0], [1.0, -6.0]],
                           "handles": [[0.0, 0.0, 1.0, -6.0]] } },
              { "id": "3da91a64-b947-4560-0000-000000000bc4", "variable": "3da91a64-b947-4560-0000-000000001e34", "scope": "bus", "target": 0,
                "curve": { "points": [[0.0, 0.0], [1.0, -12.0]],
                           "handles": [[0.0, 0.0, 1.0, -12.0]] } },
              { "id": "3da91a64-b947-4560-0000-000000000bc5", "variable": "3da91a64-b947-4560-0000-000000001e35", "scope": "bus", "target": 0,
                "curve": { "points": [[0.0, 0.0], [1.0, -6.0]],
                           "handles": [[0.0, 0.0, 1.0, -6.0]] } } ],
"attachments": [ { "binding": "3da91a64-b947-4560-0000-000000000bc3", "scope": "bus", "object": "3da91a64-b947-4560-0000-000000000bb9" },
                 { "binding": "3da91a64-b947-4560-0000-000000000bc4", "scope": "bus", "object": "3da91a64-b947-4560-0000-000000000bb9" } ])")));
    // New revision, one diff per class: bus 3006 added (resident kinds
    // may grow); send 3003->3004 removed, 3005->3004 releveled to -12,
    // 3003->3006 added; binding 3011 re-curved in place (same identity);
    // 3012's attachment removed; 3013 attached to 3002.
    const std::string v2Path = TempPath("pr_rt_v2.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_rt_v2",
        V2LogicManifest(
            v2Path, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000000bb9", "parent": "00000000-0000-0000-0000-000000000001" },
           { "id": "3da91a64-b947-4560-0000-000000000bba", "parent": "00000000-0000-0000-0000-000000000001" },
           { "id": "3da91a64-b947-4560-0000-000000000bbb", "parent": "00000000-0000-0000-0000-000000000001" },
           { "id": "3da91a64-b947-4560-0000-000000000bbc", "parent": "00000000-0000-0000-0000-000000000001" },
           { "id": "3da91a64-b947-4560-0000-000000000bbd", "parent": "00000000-0000-0000-0000-000000000001" },
           { "id": "3da91a64-b947-4560-0000-000000000bbe", "parent": "00000000-0000-0000-0000-000000000001" } ],
"busSends": [ { "from": "3da91a64-b947-4560-0000-000000000bbd", "to": "3da91a64-b947-4560-0000-000000000bbc", "levelDb": -12.0 },
              { "from": "3da91a64-b947-4560-0000-000000000bbb", "to": "3da91a64-b947-4560-0000-000000000bbe", "levelDb": 0.0 } ],
"bindings": [ { "id": "3da91a64-b947-4560-0000-000000000bc3", "variable": "3da91a64-b947-4560-0000-000000001e33", "scope": "bus", "target": 0,
                "curve": { "points": [[0.0, 0.0], [1.0, -9.0]],
                           "handles": [[0.0, 0.0, 1.0, -9.0]] } },
              { "id": "3da91a64-b947-4560-0000-000000000bc4", "variable": "3da91a64-b947-4560-0000-000000001e34", "scope": "bus", "target": 0,
                "curve": { "points": [[0.0, 0.0], [1.0, -12.0]],
                           "handles": [[0.0, 0.0, 1.0, -12.0]] } },
              { "id": "3da91a64-b947-4560-0000-000000000bc5", "variable": "3da91a64-b947-4560-0000-000000001e35", "scope": "bus", "target": 0,
                "curve": { "points": [[0.0, 0.0], [1.0, -6.0]],
                           "handles": [[0.0, 0.0, 1.0, -6.0]] } } ],
"attachments": [ { "binding": "3da91a64-b947-4560-0000-000000000bc3", "scope": "bus", "object": "3da91a64-b947-4560-0000-000000000bb9" },
                 { "binding": "3da91a64-b947-4560-0000-000000000bc5", "scope": "bus", "object": "3da91a64-b947-4560-0000-000000000bba" } ])")));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(v1Path, &pack));

    // Long DC sources on every measured bus.
    RegisterDc(harkentest::ObjectId(3021), 0.5f, kRate * 4);
    RegisterDc(harkentest::ObjectId(3022), 0.5f, kRate * 4);
    RegisterDc(harkentest::ObjectId(3023), 0.25f, kRate * 4);
    RegisterDc(harkentest::ObjectId(3024), 0.25f, kRate * 4);
    (void)PlayOnBus(harkentest::ObjectId(3021), harkentest::ObjectId(3001));
    (void)PlayOnBus(harkentest::ObjectId(3022), harkentest::ObjectId(3002));
    (void)PlayOnBus(harkentest::ObjectId(3023), harkentest::ObjectId(3003));
    (void)PlayOnBus(harkentest::ObjectId(3024), harkentest::ObjectId(3005));

    // Drive every binding variable to its curve end and settle.
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7731), 1.0f)); // discrete: integer write
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7732), 1.0f));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7733), 1.0f));
    harkentest::AdvanceVariableWrite(engine);

    // Old-revision facts: both attachments attenuate 3001 (-6 + -12 in dB);
    // 3013 contributes nowhere; 3004 hears both sends; 3006 not a bus.
    EXPECT_NEAR(0.5f * DbToLin(-18.0f), BusPeak(engine, harkentest::ObjectId(3001)), 2e-3f);
    EXPECT_NEAR(0.5f, BusPeak(engine, harkentest::ObjectId(3002)), 2e-3f);
    EXPECT_NEAR(0.25f + 0.25f * DbToLin(-6.0f), BusPeak(engine, harkentest::ObjectId(3004)),
                2e-3f);
    EXPECT_EQ(harken::kInvalidBusIndex, engine->buses.FindIndex(harkentest::ObjectId(3006)));

    const std::uint64_t plansBefore = Snap().topologyPlanCount;
    ASSERT_EQ(VN_OK, ReplacePack(pack, v2Path));

    // The gap: the resident infrastructure is entirely the old revision.
    engine->nullBackend.Pump(1);
    EXPECT_NEAR(0.5f * DbToLin(-18.0f), BusPeak(engine, harkentest::ObjectId(3001)), 2e-3f);
    EXPECT_NEAR(0.25f + 0.25f * DbToLin(-6.0f), BusPeak(engine, harkentest::ObjectId(3004)),
                2e-3f);
    EXPECT_EQ(harken::kInvalidBusIndex, engine->buses.FindIndex(harkentest::ObjectId(3006)));

    // Activation, then two ticks to let every published value land.
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);

    // Same-identity binding: 3011 stayed attached and its contribution
    // moved to the NEW curve's value (-9), while the removed attachment
    // of 3012 neutralized — the bus no longer carries its -12.
    EXPECT_NEAR(0.5f * DbToLin(-9.0f), BusPeak(engine, harkentest::ObjectId(3001)), 2e-3f);
    // Added attachment: 3013 now attenuates 3002 by its -6.
    EXPECT_NEAR(0.5f * DbToLin(-6.0f), BusPeak(engine, harkentest::ObjectId(3002)), 2e-3f);
    // Send diff: the removed 3003 edge is silent on 3004, the releveled
    // 3005 edge snapped to -12.
    EXPECT_NEAR(0.25f * DbToLin(-12.0f), BusPeak(engine, harkentest::ObjectId(3004)), 2e-3f);
    // Added bus and added edge: 3006 exists and hears 3003.
    ASSERT_NE(harken::kInvalidBusIndex, engine->buses.FindIndex(harkentest::ObjectId(3006)));
    EXPECT_NEAR(0.25f, BusPeak(engine, harkentest::ObjectId(3006)), 2e-3f);

    // ONE infrastructure republish covered the whole revision.
    EXPECT_EQ(plansBefore + 1, Snap().topologyPlanCount);
}

TEST(PackReplaceSemantics, BindingIdentityChangeIsRemovePlusAdd) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const harken::VariableDefinitionV1 definitions[] = {
        harkentest::MakeVariableDefinition(harkentest::ObjectId(7751), harken::VariableKindV1::Smooth,
                                         0.0f, 1.0f, 0.0f),
        harkentest::MakeVariableDefinition(harkentest::ObjectId(7752), harken::VariableKindV1::Smooth,
                                         0.0f, 1.0f, 0.0f)};
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallDefinitionsWorld(engine, definitions, 2).status);

    const std::string v1Path = TempPath("pr_bid_v1.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_bid_v1",
        V2LogicManifest(
            v1Path, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-0000000011f9", "parent": "00000000-0000-0000-0000-000000000001" } ],
"bindings": [ { "id": "3da91a64-b947-4560-0000-000000001203", "variable": "3da91a64-b947-4560-0000-000000001e47", "scope": "bus", "target": 0,
                "curve": { "points": [[0.0, 0.0], [1.0, -6.0]],
                           "handles": [[0.0, 0.0, 1.0, -6.0]] } } ],
"attachments": [ { "binding": "3da91a64-b947-4560-0000-000000001203", "scope": "bus", "object": "3da91a64-b947-4560-0000-0000000011f9" } ])")));
    // Same binding id, same curve, DIFFERENT variable: remove + add.
    const std::string v2Path = TempPath("pr_bid_v2.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_bid_v2",
        V2LogicManifest(
            v2Path, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-0000000011f9", "parent": "00000000-0000-0000-0000-000000000001" } ],
"bindings": [ { "id": "3da91a64-b947-4560-0000-000000001203", "variable": "3da91a64-b947-4560-0000-000000001e48", "scope": "bus", "target": 0,
                "curve": { "points": [[0.0, 0.0], [1.0, -6.0]],
                           "handles": [[0.0, 0.0, 1.0, -6.0]] } } ],
"attachments": [ { "binding": "3da91a64-b947-4560-0000-000000001203", "scope": "bus", "object": "3da91a64-b947-4560-0000-0000000011f9" } ])")));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(v1Path, &pack));
    RegisterDc(harkentest::ObjectId(4621), 0.5f, kRate * 4);
    (void)PlayOnBus(harkentest::ObjectId(4621), harkentest::ObjectId(4601));

    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7751), 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    EXPECT_NEAR(0.5f * DbToLin(-6.0f), BusPeak(engine, harkentest::ObjectId(4601)), 2e-3f);

    // No live reference beyond the pack's own attachment (which rides
    // the remove + add): the replace stages.
    ASSERT_EQ(VN_OK, ReplacePack(pack, v2Path));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);

    // The re-added definition reads the NEW variable, which is still at
    // its default: the attenuation is gone even though 7751 still holds 1.
    const harken::BindingTable::Definition* def = engine->bindings.Find(harkentest::ObjectId(4611));
    ASSERT_NE(nullptr, def);
    EXPECT_EQ(harkentest::ObjectId(7752u), def->variableId);
    EXPECT_NEAR(0.5f, BusPeak(engine, harkentest::ObjectId(4601)), 2e-3f);

    // The new variable drives it; the old one no longer does.
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7752), 1.0f));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7751), 0.25f));
    harkentest::AdvanceVariableWrite(engine);
    engine->nullBackend.Pump(2);
    EXPECT_NEAR(0.5f * DbToLin(-6.0f), BusPeak(engine, harkentest::ObjectId(4601)), 2e-3f);
}

TEST(PackReplaceSemantics, BindingIdentityChangeRefusedWhileReferenced) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // Voice-scope half: a live instance on a playing voice blocks the
    // remove + add at the call.
    const std::string wav = TempPath("pr_bref_tone.wav");
    WriteWavF32(wav, MakeDC(kBlock * 8, 0.25f), 1, kRate);
    const auto contentManifest = [&](const std::string& out,
                                     std::uint64_t variable) {
        return V2LogicManifest(
            out, "content",
            "\"entries\": [ { \"path\": \"" + wav +
                "\", \"id\": \"3da91a64-b947-4560-0000-00000000120d\" } ],\n"
                "\"bindings\": [ { \"id\": \"3da91a64-b947-4560-0000-000000001217\", \"variable\": " +
                harkentest::JsonObjectId(harkentest::ObjectId(variable)) +
                ", \"scope\": \"voice\", \"target\": 0,\n"
                "    \"curve\": { \"points\": [[0.0, 0.0], [1.0, -6.0]],\n"
                "               \"handles\": [[0.0, 0.0, 1.0, -6.0]] } } ],\n" +
                std::string(R"("attachments": [ { "binding": "3da91a64-b947-4560-0000-000000001217",
                    "scope": "voice", "object": "3da91a64-b947-4560-0000-00000000120d" } ])"));
    };
    const std::string conV1 = TempPath("pr_bref_v1.pack");
    const std::string conV2 = TempPath("pr_bref_v2.pack");
    ASSERT_TRUE(BuildPackFile("pr_bref_v1", contentManifest(conV1, 7761)));
    ASSERT_TRUE(BuildPackFile("pr_bref_v2", contentManifest(conV2, 7762)));

    VnPackHandle content = 0;
    ASSERT_EQ(VN_OK, LoadPack(conV1, &content));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(4621));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    engine->nullBackend.Pump(1);

    EXPECT_EQ(VN_IN_USE, ReplacePack(content, conV2));
    EXPECT_TRUE(engine->logicPacks.pending.empty());
    const harken::BindingTable::Definition* def = engine->bindings.Find(harkentest::ObjectId(4631));
    ASSERT_NE(nullptr, def);
    EXPECT_EQ(harkentest::ObjectId(7761u), def->variableId); // the old revision untouched

    const VnVoiceHandle stoppedRender = harkentest::RenderHandle(engine, voice);
    // Drain the live reference; the dead render entry deliberately remains
    // in ParamSystem until slot reuse or definition purge.
    ASSERT_EQ(VN_OK, Vn_Stop(voice, 0));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    std::vector<harken::ParamSystem::InstanceState> inertInstances;
    engine->params.CaptureVoiceInstances(stoppedRender, &inertInstances);
    ASSERT_EQ(1u, inertInstances.size());
    EXPECT_EQ(harkentest::ObjectId(4631u), inertInstances[0].definitionId);
    EXPECT_EQ(1u, engine->params.VoiceBindingCount(stoppedRender));
    ASSERT_EQ(VN_OK, ReplacePack(content, conV2));
    ASSERT_EQ(VN_OK, Vn_Update());
    def = engine->bindings.Find(harkentest::ObjectId(4631));
    ASSERT_NE(nullptr, def);
    EXPECT_EQ(harkentest::ObjectId(7762u), def->variableId);
    EXPECT_EQ(0u, engine->params.VoiceBindingCount(stoppedRender));

    // Foreign-attachment half: any attachment beyond the pack's own
    // blocks the identity change, however the definition is scoped.
    const auto residentManifest = [&](const std::string& out,
                                      std::uint64_t variable) {
        return V2LogicManifest(
            out, "resident",
            "\"buses\": [ { \"id\": \"3da91a64-b947-4560-0000-000000001221\", \"parent\": \"00000000-0000-0000-0000-000000000001\" } ],\n"
            "\"bindings\": [ { \"id\": \"3da91a64-b947-4560-0000-00000000122b\", \"variable\": " +
                harkentest::JsonObjectId(harkentest::ObjectId(variable)) +
                ", \"scope\": \"bus\", \"target\": 0,\n"
                "    \"curve\": { \"points\": [[0.0, 0.0], [1.0, -6.0]],\n"
                "               \"handles\": [[0.0, 0.0, 1.0, -6.0]] } } ],\n" +
                std::string(R"("attachments": [ { "binding": "3da91a64-b947-4560-0000-00000000122b",
                    "scope": "bus", "object": "3da91a64-b947-4560-0000-000000001221" } ])"));
    };
    const std::string resV1 = TempPath("pr_bfor_v1.pack");
    const std::string resV2 = TempPath("pr_bfor_v2.pack");
    ASSERT_TRUE(BuildPackFile("pr_bfor_v1", residentManifest(resV1, 7771)));
    ASSERT_TRUE(BuildPackFile("pr_bfor_v2", residentManifest(resV2, 7772)));

    VnPackHandle resident = 0;
    ASSERT_EQ(VN_OK, LoadPack(resV1, &resident));
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(4642), VN_BUS_MASTER));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(VnBindingAttachDesc);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = harkentest::ObjectId(4642);
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(4651), &attach));

    EXPECT_EQ(VN_IN_USE, ReplacePack(resident, resV2));
    def = engine->bindings.Find(harkentest::ObjectId(4651));
    ASSERT_NE(nullptr, def);
    EXPECT_EQ(harkentest::ObjectId(7771u), def->variableId);

    ASSERT_EQ(VN_OK, Vn_DetachBinding(harkentest::ObjectId(4651), &attach));
    ASSERT_EQ(VN_OK, ReplacePack(resident, resV2));
    ASSERT_EQ(VN_OK, Vn_Update());
    def = engine->bindings.Find(harkentest::ObjectId(4651));
    ASSERT_NE(nullptr, def);
    EXPECT_EQ(harkentest::ObjectId(7772u), def->variableId);
}

void MakeAttenuationHole(harken::Spatializer& spatial) {
    auto desc = harkentest::MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(VN_OK, spatial.RegisterProfile(harkentest::ObjectId(7000), &desc));
    const harken::Spatializer::PackProfileInput input{harkentest::ObjectId(7001), &desc};
    harken::Spatializer::PreparedProfileSet prepared;
    ASSERT_EQ(VN_OK, spatial.PreparePackProfiles(&input, 1, 1, &prepared));
    spatial.CommitPreparedSet(&prepared);
    ASSERT_EQ(VN_OK, spatial.RegisterProfile(harkentest::ObjectId(7002), &desc));
    ASSERT_EQ(VN_OK, spatial.PreparePackProfiles(nullptr, 0, 0, &prepared));
    spatial.CommitPreparedSet(&prepared);
    ASSERT_EQ(2u, spatial.ProfileCount());
    ASSERT_EQ(nullptr, spatial.FindProfile(harkentest::ObjectId(7001)));
}

std::string HoleAttenuationsManifest(const std::string& path, unsigned count) {
    std::string rows = "\"attenuations\": [";
    for (unsigned i = 0; i < count; ++i) {
        if (i != 0) rows += ",";
        rows += "{\"id\":" + harkentest::JsonObjectId(harkentest::ObjectId(7101 + i)) +
            ",\"rolloff\":\"linear\",\"minDistance\":1,\"maxDistance\":100,"
            "\"airAbsorptionNearHz\":1000000000,\"airAbsorptionFarHz\":1000000000}";
    }
    rows += "]";
    return V2LogicManifest(path, "resident", rows);
}

TEST(PackReplaceSemantics, AttenuationHolesSupportInitialLoadAndReplacementAdds) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& spatial = session.Engine()->spatial;
    ASSERT_NO_FATAL_FAILURE(MakeAttenuationHole(spatial));
    const auto* direct0 = spatial.FindProfile(harkentest::ObjectId(7000));
    const auto* direct2 = spatial.FindProfile(harkentest::ObjectId(7002));
    ASSERT_NE(nullptr, direct0);
    ASSERT_NE(nullptr, direct2);
    EXPECT_EQ(2, direct2 - direct0);
    const auto path = TempPath("pr_u3_hole_load.pack");
    const auto nextPath = TempPath("pr_u3_hole_replace.pack");
    ASSERT_TRUE(BuildPackFile("pr_u3_hole_load", HoleAttenuationsManifest(path, 2)));
    ASSERT_TRUE(BuildPackFile("pr_u3_hole_replace", HoleAttenuationsManifest(nextPath, 3)));
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(path, &pack));
    ASSERT_NE(nullptr, spatial.FindProfile(harkentest::ObjectId(7101)));
    ASSERT_NE(nullptr, spatial.FindProfile(harkentest::ObjectId(7102)));
    EXPECT_EQ(1, spatial.FindProfile(harkentest::ObjectId(7101)) - direct0);
    EXPECT_EQ(3, spatial.FindProfile(harkentest::ObjectId(7102)) - direct0);
    EXPECT_EQ(4u, spatial.ProfileCount());
    EXPECT_EQ(direct0, spatial.FindProfile(harkentest::ObjectId(7000)));
    EXPECT_EQ(direct2, spatial.FindProfile(harkentest::ObjectId(7002)));
    ASSERT_EQ(VN_OK, ReplacePack(pack, nextPath));
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_NE(nullptr, spatial.FindProfile(harkentest::ObjectId(7103)));
    EXPECT_EQ(4, spatial.FindProfile(harkentest::ObjectId(7103)) - direct0);
    EXPECT_EQ(5u, spatial.ProfileCount());
    EXPECT_EQ(direct0, spatial.FindProfile(harkentest::ObjectId(7000)));
    EXPECT_EQ(direct2, spatial.FindProfile(harkentest::ObjectId(7002)));
    EXPECT_FLOAT_EQ(100.0f, direct0->maxDistance);
    EXPECT_FLOAT_EQ(100.0f, direct2->maxDistance);
}

TEST(PackReplaceSemantics, AttenuationHolePreparedTokensCancelOnLateLoadFailure) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& spatial = session.Engine()->spatial;
    ASSERT_NO_FATAL_FAILURE(MakeAttenuationHole(spatial));
    const auto* direct0 = spatial.FindProfile(harkentest::ObjectId(7000));
    const auto* direct2 = spatial.FindProfile(harkentest::ObjectId(7002));
    const auto path = TempPath("pr_u3_hole_rollback.pack");
    ASSERT_TRUE(BuildPackFile("pr_u3_hole_rollback", HoleAttenuationsManifest(path, 2)));
    for (int attempt = 0; attempt < 2; ++attempt) {
        StagingFaultGuard fault(harken::PackStagingFault::InitialMediaPreparation);
        VnPackHandle refused = 0;
        EXPECT_EQ(VN_DEVICE_ERROR, LoadPack(path, &refused));
        EXPECT_EQ(0u, refused);
        EXPECT_EQ(2u, spatial.ProfileCount());
        EXPECT_EQ(nullptr, spatial.FindProfile(harkentest::ObjectId(7101)));
        EXPECT_EQ(nullptr, spatial.FindProfile(harkentest::ObjectId(7102)));
        EXPECT_EQ(direct0, spatial.FindProfile(harkentest::ObjectId(7000)));
        EXPECT_EQ(direct2, spatial.FindProfile(harkentest::ObjectId(7002)));
    }
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(path, &pack));
    EXPECT_EQ(4u, spatial.ProfileCount());
    ASSERT_NE(nullptr, spatial.FindProfile(harkentest::ObjectId(7101)));
    ASSERT_NE(nullptr, spatial.FindProfile(harkentest::ObjectId(7102)));
    EXPECT_EQ(1, spatial.FindProfile(harkentest::ObjectId(7101)) - direct0);
    EXPECT_EQ(3, spatial.FindProfile(harkentest::ObjectId(7102)) - direct0);
}

// --- semantics: topology is replaceable; one-way resident kinds are not ---

TEST(PackReplaceSemantics,
     ResidentBusTopologyIsSnapshotWhileAttenuationNeverShrinks) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string v1Path = TempPath("pr_shrink_v1.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_shrink_v1",
        V2LogicManifest(
            v1Path, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000000b55", "parent": "00000000-0000-0000-0000-000000000001" },
           { "id": "3da91a64-b947-4560-0000-000000000b56", "parent": "3da91a64-b947-4560-0000-000000000b55" },
           { "id": "3da91a64-b947-4560-0000-000000000b57", "parent": "00000000-0000-0000-0000-000000000001" } ],
"busSends": [ { "from": "3da91a64-b947-4560-0000-000000000b56", "to": "3da91a64-b947-4560-0000-000000000b57", "levelDb": 0.0 } ],
"attenuations": [ { "id": "3da91a64-b947-4560-0000-000000000b5f", "rolloff": "linear",
                    "minDistance": 1.0, "maxDistance": 100.0,
                    "airAbsorptionNearHz": 1000000000.0,
                    "airAbsorptionFarHz": 1000000000.0 } ])")));
    const std::string missingAttenuation = TempPath("pr_shrink_atten.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_shrink_atten",
        V2LogicManifest(
            missingAttenuation, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000000b55", "parent": "00000000-0000-0000-0000-000000000001" },
           { "id": "3da91a64-b947-4560-0000-000000000b56", "parent": "3da91a64-b947-4560-0000-000000000b55" },
           { "id": "3da91a64-b947-4560-0000-000000000b57", "parent": "00000000-0000-0000-0000-000000000001" } ],
"busSends": [ { "from": "3da91a64-b947-4560-0000-000000000b56", "to": "3da91a64-b947-4560-0000-000000000b57", "levelDb": 0.0 } ])")));
    const std::string reparented = TempPath("pr_shrink_reparent.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_shrink_reparent",
        V2LogicManifest(
            reparented, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000000b55", "parent": "00000000-0000-0000-0000-000000000001" },
           { "id": "3da91a64-b947-4560-0000-000000000b56", "parent": "00000000-0000-0000-0000-000000000001" },
           { "id": "3da91a64-b947-4560-0000-000000000b57", "parent": "00000000-0000-0000-0000-000000000001" } ],
"busSends": [ { "from": "3da91a64-b947-4560-0000-000000000b56", "to": "3da91a64-b947-4560-0000-000000000b57", "levelDb": 0.0 } ],
"attenuations": [ { "id": "3da91a64-b947-4560-0000-000000000b5f", "rolloff": "linear",
                    "minDistance": 1.0, "maxDistance": 100.0,
                    "airAbsorptionNearHz": 1000000000.0,
                    "airAbsorptionFarHz": 1000000000.0 } ])")));
    const std::string noSend = TempPath("pr_shrink_send.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_shrink_send",
        V2LogicManifest(
            noSend, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000000b55", "parent": "00000000-0000-0000-0000-000000000001" },
           { "id": "3da91a64-b947-4560-0000-000000000b56", "parent": "3da91a64-b947-4560-0000-000000000b55" },
           { "id": "3da91a64-b947-4560-0000-000000000b57", "parent": "00000000-0000-0000-0000-000000000001" } ],
"attenuations": [ { "id": "3da91a64-b947-4560-0000-000000000b5f", "rolloff": "linear",
                    "minDistance": 1.0, "maxDistance": 100.0,
                    "airAbsorptionNearHz": 1000000000.0,
                    "airAbsorptionFarHz": 1000000000.0 } ])")));
    const std::string missingBus = TempPath("pr_shrink_bus.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_shrink_bus",
        V2LogicManifest(
            missingBus, "resident",
            R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000000b55", "parent": "00000000-0000-0000-0000-000000000001" },
           { "id": "3da91a64-b947-4560-0000-000000000b57", "parent": "00000000-0000-0000-0000-000000000001" } ],
"attenuations": [ { "id": "3da91a64-b947-4560-0000-000000000b5f", "rolloff": "linear",
                    "minDistance": 1.0, "maxDistance": 100.0,
                    "airAbsorptionNearHz": 1000000000.0,
                    "airAbsorptionFarHz": 1000000000.0 } ])")));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(v1Path, &pack));
    RegisterDc(harkentest::ObjectId(2921), 0.5f, kRate * 4);
    (void)PlayOnBus(harkentest::ObjectId(2921), harkentest::ObjectId(2902));
    engine->nullBackend.Pump(1);
    EXPECT_NEAR(0.5f, BusPeak(engine, harkentest::ObjectId(2903)), 2e-3f);

    const std::uint64_t refusedBefore = Snap().hotReloadRefusedCount;
    EXPECT_EQ(VN_INVALID_ARG, ReplacePack(pack, missingAttenuation));
    EXPECT_EQ(refusedBefore + 1, Snap().hotReloadRefusedCount);
    EXPECT_TRUE(engine->logicPacks.pending.empty());
    EXPECT_TRUE(engine->spatial.HasProfile(harkentest::ObjectId(2911)));

    ASSERT_EQ(VN_OK, ReplacePack(pack, reparented));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const std::uint32_t reparentedIndex = engine->buses.FindIndex(harkentest::ObjectId(2902));
    ASSERT_NE(harken::kInvalidBusIndex, reparentedIndex);
    EXPECT_EQ(0u, engine->buses.ParentOf(reparentedIndex));

    ASSERT_EQ(VN_OK, ReplacePack(pack, noSend));
    engine->nullBackend.Pump(1);
    EXPECT_NEAR(0.5f, BusPeak(engine, harkentest::ObjectId(2903)), 2e-3f);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    EXPECT_LT(BusPeak(engine, harkentest::ObjectId(2903)), harkentest::kSilenceFloor);
    EXPECT_NEAR(0.5f, BusPeak(engine, harkentest::ObjectId(2902)), 2e-3f);

    ASSERT_EQ(VN_OK, ReplacePack(pack, missingBus));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::kInvalidBusIndex, engine->buses.FindIndex(harkentest::ObjectId(2902)));
    EXPECT_TRUE(engine->spatial.HasProfile(harkentest::ObjectId(2911)));
}

// --- semantics: provider protection over replace removals ---


// --- semantics: net capacity arithmetic over the diff ---

TEST(PackReplaceSemantics, ReplaceNetCapacityFreesBeforeAdds) {
    LogSink sink;
    VnConfig config = MakeNullConfig(4, kBlock, kRate);
    config.maxBindings = 2;
    config.logCallback = &LogCb;
    config.logUserData = &sink;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const harken::VariableDefinitionV1 definitions[] = {
        harkentest::MakeVariableDefinition(harkentest::ObjectId(7785), harken::VariableKindV1::Smooth,
                                         0.0f, 1.0f, 0.0f),
        harkentest::MakeVariableDefinition(harkentest::ObjectId(7786), harken::VariableKindV1::Smooth,
                                         0.0f, 1.0f, 0.0f),
        harkentest::MakeVariableDefinition(harkentest::ObjectId(7787), harken::VariableKindV1::Smooth,
                                         0.0f, 1.0f, 0.0f)};
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallDefinitionsWorld(engine, definitions, 3).status);

    // The loaded revision fills the two-slot definition pool with 4951
    // and 4952. The growth twin keeps both and adds 4953 (net 3 > 2);
    // the swap twin keeps 4951, removes 4952 and adds 4953 (net 2 <= 2):
    // the removal must fund the add's slot, at the call AND at the
    // activation re-screen.
    const auto def = [](std::uint64_t id, std::uint64_t variable) {
        return "{ \"id\": " + harkentest::JsonObjectId(harkentest::ObjectId(id)) +
               ", \"variable\": " + harkentest::JsonObjectId(harkentest::ObjectId(variable)) +
               ", \"scope\": \"bus\", \"target\": 0,\n"
               "    \"curve\": { \"points\": [[0.0, 0.0], [1.0, -6.0]],\n"
               "               \"handles\": [[0.0, 0.0, 1.0, -6.0]] } }";
    };
    const std::string defB1 = def(4951, 7785);
    const std::string defB2 = def(4952, 7786);
    const std::string defB3 = def(4953, 7787);
    const std::string v1Path = TempPath("pr_net_v1.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_net_v1", V2LogicManifest(v1Path, "resident",
                                     "\"bindings\": [ " + defB1 + ",\n" +
                                         defB2 + " ]")));
    const std::string growthPath = TempPath("pr_net_growth.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_net_growth",
        V2LogicManifest(growthPath, "resident",
                        "\"bindings\": [ " + defB1 + ",\n" + defB2 +
                            ",\n" + defB3 + " ]")));
    const std::string swapPath = TempPath("pr_net_swap.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_net_swap", V2LogicManifest(swapPath, "resident",
                                       "\"bindings\": [ " + defB1 +
                                           ",\n" + defB3 + " ]")));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(v1Path, &pack));
    ASSERT_EQ(2u, engine->bindings.DefinitionCount()); // pool full

    // Net 3 over capacity 2: refused at the CALL with the pool code,
    // nothing staged, nothing landed.
    EXPECT_EQ(VN_NO_BINDING, ReplacePack(pack, growthPath));
    EXPECT_TRUE(engine->logicPacks.pending.empty());
    EXPECT_EQ(nullptr, engine->bindings.Find(harkentest::ObjectId(4953)));
    EXPECT_EQ(2u, engine->bindings.DefinitionCount());

    // Net 2 within capacity 2: the removal of 4952 frees the slot 4953
    // consumes. The replace stages, and the activation re-screen
    // recomputes the same net arithmetic — the revision applies whole
    // instead of discarding.
    ASSERT_EQ(VN_OK, ReplacePack(pack, swapPath));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_TRUE(engine->logicPacks.pending.empty());
    EXPECT_EQ(0u, sink.Count(harken::kLogCodePackRevisionDiscarded));
    EXPECT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(4951)));
    EXPECT_EQ(nullptr, engine->bindings.Find(harkentest::ObjectId(4952)));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_UnregisterBinding(harkentest::ObjectId(4952))); // gone whole
    ASSERT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(4953)));

    // The added definition EVALUATES: attached to the master and driven
    // to its curve end, its attenuation lands in the rendered peak.
    RegisterDc(harkentest::ObjectId(4961), 0.5f, kRate * 4);
    (void)PlayOnBus(harkentest::ObjectId(4961), VN_BUS_MASTER);
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(VnBindingAttachDesc);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = VN_BUS_MASTER;
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(4953), &attach));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7787), 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    EXPECT_NEAR(0.5f * DbToLin(-6.0f), BusPeak(engine, VN_BUS_MASTER),
                2e-3f);
}

// --- semantics: v1 observable equivalence and the vintage split ---

TEST(PackReplaceSemantics, V1MediaEquivalenceAndActivationVintageSplit) {
    // The same three-sound media set — the third loopable over its
    // second block — compiled once as a v1 pack (no packKind, the
    // two-positional command line) and once as a v2 media-only content
    // pack.
    const std::string wavM1a = TempPath("pr_vin_m1a.wav");
    const std::string wavM1b = TempPath("pr_vin_m1b.wav");
    const std::string wavM2 = TempPath("pr_vin_m2.wav");
    const std::string wavLoop = TempPath("pr_vin_loop.wav");
    WriteWavF32(wavM1a, MakeDC(kBlock, 0.25f), 1, kRate);
    WriteWavF32(wavM1b, MakeDC(kBlock, 0.5f), 1, kRate);
    WriteWavF32(wavM2, MakeDC(kBlock, 0.35f), 1, kRate);
    // Intro block at 0.2, loop-region block at 0.4: which block a wrap
    // replays tells whether the region metadata was served.
    std::vector<float> loopPcm = MakeDC(kBlock, 0.2f);
    const std::vector<float> loopTail = MakeDC(kBlock, 0.4f);
    loopPcm.insert(loopPcm.end(), loopTail.begin(), loopTail.end());
    WriteWavF32(wavLoop, loopPcm, 1, kRate);
    const std::string loopEntryTail =
        "\", \"id\": \"3da91a64-b947-4560-0000-0000000012c3\", \"loopStartFrame\": " + std::to_string(kBlock) +
        " }";

    const std::string v1A = TempPath("pr_vin_v1_a.pack");
    const std::string v1B = TempPath("pr_vin_v1_b.pack");
    ASSERT_TRUE(BuildMediaPackFile("pr_vin_v1_a",
                                "[ { \"path\": \"" + wavM1a +
                                    "\", \"id\": \"3da91a64-b947-4560-0000-0000000012c1\" },\n  { \"path\": \"" +
                                    wavM2 + "\", \"id\": \"3da91a64-b947-4560-0000-0000000012c2\" },\n"
                                    "  { \"path\": \"" + wavLoop +
                                    loopEntryTail + " ]\n",
                                v1A));
    ASSERT_TRUE(BuildMediaPackFile("pr_vin_v1_b",
                                "[ { \"path\": \"" + wavM1b +
                                    "\", \"id\": \"3da91a64-b947-4560-0000-0000000012c1\" },\n  { \"path\": \"" +
                                    wavM2 + "\", \"id\": \"3da91a64-b947-4560-0000-0000000012c2\" },\n"
                                    "  { \"path\": \"" + wavLoop +
                                    loopEntryTail + " ]\n",
                                v1B));
    const std::string v2A = TempPath("pr_vin_v2_a.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_vin_v2_a",
        V2LogicManifest(v2A, "content",
                        "\"entries\": [ { \"path\": \"" + wavM1a +
                            "\", \"id\": \"3da91a64-b947-4560-0000-0000000012c1\" },\n  { \"path\": \"" + wavM2 +
                            "\", \"id\": \"3da91a64-b947-4560-0000-0000000012c2\" },\n  { \"path\": \"" +
                            wavLoop + loopEntryTail + " ]")));
    const std::string v2B = TempPath("pr_vin_v2_b.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_vin_v2_b",
        V2LogicManifest(v2B, "content",
                        "\"entries\": [ { \"path\": \"" + wavM1b +
                            "\", \"id\": \"3da91a64-b947-4560-0000-0000000012c1\" },\n  { \"path\": \"" + wavM2 +
                            "\", \"id\": \"3da91a64-b947-4560-0000-0000000012c2\" },\n  { \"path\": \"" +
                            wavLoop + loopEntryTail + " ]")));

    // The identical drive both vintages run: one play per sound, one
    // block each, an update between (one-block media, so every window
    // holds exactly its own voice), then the loopable entry for two
    // passes over its region.
    const auto drive = [](harken::Engine* engine, Capture& out) {
        const VnPlayDesc first = MakePlayDesc(harkentest::ObjectId(4801));
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&first, &voice));
        PumpAndCapture(engine, 1, out);
        ASSERT_EQ(VN_OK, Vn_Update());
        const VnPlayDesc second = MakePlayDesc(harkentest::ObjectId(4802));
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&second, &voice));
        PumpAndCapture(engine, 1, out);
        ASSERT_EQ(VN_OK, Vn_Update());
        // Two passes: intro once, then the region [kBlock, 2*kBlock)
        // twice — the served loop metadata puts the REGION in the third
        // rendered block, where a whole-file wrap would replay the
        // intro.
        VnPlayDesc looped = MakePlayDesc(harkentest::ObjectId(4803));
        looped.loopCount = 2;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&looped, &voice));
        PumpAndCapture(engine, 3, out);
        ASSERT_EQ(VN_OK, Vn_Update());
    };

    // v1 engine: equivalence capture, then the call-return activation
    // pin — a play issued right after Vn_ReplacePack, with NO Vn_Update
    // anywhere in between, already renders the NEW bytes.
    Capture v1Capture;
    Capture v1Pin;
    {
        EngineSession session(MakeNullConfig(4, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        VnPackHandle pack = 0;
        ASSERT_EQ(VN_OK, LoadPack(v1A, &pack));
        drive(engine, v1Capture);

        ASSERT_EQ(VN_OK, ReplacePack(pack, v1B));
        const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(4801));
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        PumpAndCapture(engine, 1, v1Pin);
        HARKENTEST_ASSERT_NON_SILENT(v1Pin.left);
        EXPECT_FLOAT_EQ(0.5f, v1Pin.left[kBlock / 2]);

        // Ownership symmetry, v1 vintage: one settle tick, then the
        // unload removes every id together and frees them for caller
        // registrations.
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_EQ(VN_OK, Vn_UnloadPack(pack));
        const VnSoundId ids[] = {harkentest::ObjectId(4801), harkentest::ObjectId(4802), harkentest::ObjectId(4803)};
        for (const VnSoundId id : ids) {
            const VnPlayDesc gone = MakePlayDesc(id);
            VnVoiceHandle probe = 0;
            EXPECT_EQ(VN_UNKNOWN_ID, harkentest::PlayRaw(&gone, &probe)) << id;
        }
        const std::vector<float> own = MakeDC(64, 0.25f);
        const VnPCMDesc ownDesc = MonoDesc(own, kRate);
        EXPECT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(4801), &ownDesc));
    }

    // v2 engine: the identical drive must render byte-identical output
    // (registry-state equivalence observed through rendering), and the
    // same replace renders OLD until the update pump.
    Capture v2Capture;
    {
        EngineSession session(MakeNullConfig(4, kBlock, kRate));
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        VnPackHandle pack = 0;
        ASSERT_EQ(VN_OK, LoadPack(v2A, &pack));
        drive(engine, v2Capture);

        ASSERT_EQ(VN_OK, ReplacePack(pack, v2B));
        const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(4801));
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        Capture gap;
        PumpAndCapture(engine, 1, gap);
        HARKENTEST_ASSERT_NON_SILENT(gap.left);
        EXPECT_FLOAT_EQ(0.25f, gap.left[kBlock / 2]); // tick activation

        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        Capture after;
        PumpAndCapture(engine, 1, after);
        EXPECT_FLOAT_EQ(0.5f, after.left[kBlock / 2]);

        // Ownership symmetry, v2 vintage: identical facts after the
        // generation sweep drains.
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_EQ(VN_OK, Vn_UnloadPack(pack));
        const VnSoundId ids[] = {harkentest::ObjectId(4801), harkentest::ObjectId(4802), harkentest::ObjectId(4803)};
        for (const VnSoundId id : ids) {
            const VnPlayDesc gone = MakePlayDesc(id);
            VnVoiceHandle probe = 0;
            EXPECT_EQ(VN_UNKNOWN_ID, harkentest::PlayRaw(&gone, &probe)) << id;
        }
        const std::vector<float> own = MakeDC(64, 0.25f);
        const VnPCMDesc ownDesc = MonoDesc(own, kRate);
        EXPECT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(4801), &ownDesc));
    }

    HARKENTEST_ASSERT_NON_SILENT(v1Capture.left);
    ExpectSameCapture(v1Capture, v2Capture);

    // Loop metadata served identically on both vintages: within the
    // loop capture (blocks three to five of the drive) the first block
    // is the intro, the next two the region — a whole-file wrap would
    // put the intro amplitude back in the last block.
    for (const Capture* c : {&v1Capture, &v2Capture}) {
        ASSERT_EQ(static_cast<std::size_t>(5 * kBlock), c->left.size());
        EXPECT_FLOAT_EQ(0.2f, c->left[2 * kBlock + kBlock / 2]);
        EXPECT_FLOAT_EQ(0.4f, c->left[3 * kBlock + kBlock / 2]);
        EXPECT_FLOAT_EQ(0.4f, c->left[4 * kBlock + kBlock / 2]);
    }
}

TEST(PackReplaceCompletionGuard,
     V2MediaReplaceRefusesAtCallAndDefersWhenPinnedDuringGap) {
    const std::string wavA = TempPath("pr_guard_media_a.wav");
    const std::string wavB = TempPath("pr_guard_media_b.wav");
    WriteWavF32(wavA, MakeDC(8 * kBlock, 0.25f), 1, kRate);
    WriteWavF32(wavB, MakeDC(2 * kBlock, 0.5f), 1, kRate);
    const std::string pathA = TempPath("pr_guard_media_a.pack");
    const std::string pathB = TempPath("pr_guard_media_b.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_guard_media_a",
        V2LogicManifest(pathA, "content",
                        "\"entries\": [ { \"path\": \"" + wavA +
                            "\", \"id\": \"3da91a64-b947-4560-0000-000000001325\" } ]")));
    ASSERT_TRUE(BuildPackFile(
        "pr_guard_media_b",
        V2LogicManifest(pathB, "content",
                        "\"entries\": [ { \"path\": \"" + wavB +
                            "\", \"id\": \"3da91a64-b947-4560-0000-000000001325\" } ]")));
    EngineSession session(MakeNullConfig(2, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(pathA, &pack));
    VnVoiceHandle voice = 0;
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(4901));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    VnVoiceHandle render = harkentest::RenderHandle(engine, voice);
    const harken::PCMView oldMedia = engine->registry.Lookup(harkentest::ObjectId(4901));
    harken::CompletionMutationBinding binding{};
    ASSERT_TRUE(PinCompletionOwner(*engine, voice, render, 90, 630,
                                   &binding));
    const std::uint64_t backpressure = engine->commands.BackpressureCount();

    EXPECT_EQ(VN_BACKPRESSURE, ReplacePack(pack, pathB));
    EXPECT_EQ(nullptr, engine->logicPacks.PendingFor(pack));
    EXPECT_EQ(oldMedia.useCount, engine->registry.Lookup(harkentest::ObjectId(4901)).useCount);
    EXPECT_EQ(backpressure + 1u, engine->commands.BackpressureCount());

    ASSERT_TRUE(engine->ledger.ClearCompletionMutationExact(
        static_cast<std::uint32_t>(voice), binding));
    ASSERT_TRUE(engine->timelineOccurrences.RollbackCompletionOwnerPinExact(
        binding.occurrence, binding.owner));
    ASSERT_EQ(VN_OK, ReplacePack(pack, pathB));
    ASSERT_NE(nullptr, engine->logicPacks.PendingFor(pack));
    ASSERT_TRUE(PinCompletionOwner(*engine, voice, render, 91, 631,
                                   &binding));
    const std::uint64_t gapBackpressure =
        engine->commands.BackpressureCount();
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_NE(nullptr, engine->logicPacks.PendingFor(pack));
    EXPECT_EQ(oldMedia.useCount, engine->registry.Lookup(harkentest::ObjectId(4901)).useCount);
    EXPECT_EQ(gapBackpressure, engine->commands.BackpressureCount());

    ASSERT_TRUE(engine->ledger.ClearCompletionMutationExact(
        static_cast<std::uint32_t>(voice), binding));
    ASSERT_TRUE(engine->timelineOccurrences.RollbackCompletionOwnerPinExact(
        binding.occurrence, binding.owner));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->logicPacks.PendingFor(pack));
    EXPECT_NE(oldMedia.useCount, engine->registry.Lookup(harkentest::ObjectId(4901)).useCount);
}

TEST(PackReplaceCompletionGuard,
     V2UnloadRefusesPinnedCurrentMediaBeforeAnyPackMutation) {
    const std::string wav = TempPath("pr_guard_unload_media.wav");
    WriteWavF32(wav, MakeDC(8 * kBlock, 0.25f), 1, kRate);
    const std::string path = TempPath("pr_guard_unload_media.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_guard_unload_media",
        V2LogicManifest(path, "content",
                        "\"entries\": [ { \"path\": \"" + wav +
                            "\", \"id\": \"3da91a64-b947-4560-0000-00000000132f\" } ]")));
    EngineSession session(MakeNullConfig(2, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(path, &pack));
    VnVoiceHandle voice = 0;
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(4911));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, voice);
    const harken::PCMView media = engine->registry.Lookup(harkentest::ObjectId(4911));
    ASSERT_NE(nullptr, media.useCount);
    harken::CompletionMutationBinding binding{};
    ASSERT_TRUE(PinCompletionOwner(*engine, voice, render, 96, 636,
                                   &binding));
    const std::uint64_t backpressure = engine->commands.BackpressureCount();
    const std::uint32_t loaded = engine->packs.LoadedCount();

    EXPECT_EQ(VN_BACKPRESSURE, Vn_UnloadPack(pack));
    EXPECT_EQ(backpressure + 1u, engine->commands.BackpressureCount());
    EXPECT_EQ(loaded, engine->packs.LoadedCount());
    EXPECT_NE(nullptr, engine->packs.Entries(pack));
    EXPECT_NE(nullptr, engine->logicPacks.RecordFor(pack));
    EXPECT_EQ(media.useCount, engine->registry.Lookup(harkentest::ObjectId(4911)).useCount);

    ASSERT_TRUE(engine->ledger.ClearCompletionMutationExact(
        static_cast<std::uint32_t>(voice), binding));
    ASSERT_TRUE(engine->timelineOccurrences.RollbackCompletionOwnerPinExact(
        binding.occurrence, binding.owner));
    EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
    EXPECT_EQ(backpressure + 1u, engine->commands.BackpressureCount());

    ASSERT_EQ(VN_OK, Vn_Stop(voice, 0));
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, Vn_UnloadPack(pack));
    EXPECT_EQ(loaded - 1u, engine->packs.LoadedCount());
    EXPECT_EQ(nullptr, engine->logicPacks.RecordFor(pack));
    EXPECT_EQ(nullptr, engine->registry.Lookup(harkentest::ObjectId(4911)).useCount);
}

TEST(PackReplaceCompletionGuard,
     V2UnloadRefusesPinnedPitchInstanceBeforeAnyPackMutation) {
    const std::string path = TempPath("pr_guard_unload_pitch.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_guard_unload_pitch",
        V2LogicManifest(
            path, "content",
            R"("bindings": [ { "id": "3da91a64-b947-4560-0000-000000001331", "variable": "3da91a64-b947-4560-0000-000000001ee9",
"scope": "voice", "target": 1,
"curve": { "points": [[0.0, 0.0], [1.0, 300.0]],
"handles": [[0.0, 0.0, 1.0, 300.0]] } } ],
"attachments": [ { "binding": "3da91a64-b947-4560-0000-000000001331", "scope": "voice",
"object": "3da91a64-b947-4560-0000-000000001330" } ])")));
    EngineSession session(MakeNullConfig(2, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterDc(harkentest::ObjectId(4912), 0.25f, 8 * kBlock);
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(path, &pack));
    ASSERT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(4913)));
    ASSERT_TRUE(engine->bindings.HasOwnedAttachment(
        harkentest::ObjectId(4913), VN_TARGET_SCOPE_VOICE, harkentest::ObjectId(4912), pack));
    VnVoiceHandle voice = 0;
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(4912));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, voice);
    harken::CompletionMutationBinding binding{};
    ASSERT_TRUE(PinCompletionOwner(*engine, voice, render, 97, 637,
                                   &binding));
    const std::uint64_t backpressure = engine->commands.BackpressureCount();
    const std::uint32_t loaded = engine->packs.LoadedCount();

    EXPECT_EQ(VN_BACKPRESSURE, Vn_UnloadPack(pack));
    EXPECT_EQ(backpressure + 1u, engine->commands.BackpressureCount());
    EXPECT_EQ(loaded, engine->packs.LoadedCount());
    EXPECT_NE(nullptr, engine->logicPacks.RecordFor(pack));
    EXPECT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(4913)));
    EXPECT_TRUE(engine->bindings.HasOwnedAttachment(
        harkentest::ObjectId(4913), VN_TARGET_SCOPE_VOICE, harkentest::ObjectId(4912), pack));

    ASSERT_TRUE(engine->ledger.ClearCompletionMutationExact(
        static_cast<std::uint32_t>(voice), binding));
    ASSERT_TRUE(engine->timelineOccurrences.RollbackCompletionOwnerPinExact(
        binding.occurrence, binding.owner));
    EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
    EXPECT_EQ(backpressure + 1u, engine->commands.BackpressureCount());

    ASSERT_EQ(VN_OK, Vn_Stop(voice, 0));
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, Vn_UnloadPack(pack));
    EXPECT_EQ(loaded - 1u, engine->packs.LoadedCount());
    EXPECT_EQ(nullptr, engine->logicPacks.RecordFor(pack));
    EXPECT_EQ(nullptr, engine->bindings.Find(harkentest::ObjectId(4913)));
    EXPECT_FALSE(engine->bindings.HasAttachment(
        harkentest::ObjectId(4913), VN_TARGET_SCOPE_VOICE, harkentest::ObjectId(4912)));
}

TEST(PackReplaceCompletionGuard,
     V2PitchReviseDetachAndDefinitionRemovalGuardExactCurrentVoice) {
    const std::string wav = TempPath("pr_guard_pitch.wav");
    WriteWavF32(wav, MakeDC(8 * kBlock, 0.25f), 1, kRate);
    const auto manifest = [&](const std::string& output, const char* curveEnd,
                              bool attachment, bool definition) {
        std::string sections =
            "\"entries\": [ { \"path\": \"" + wav +
            "\", \"id\": \"3da91a64-b947-4560-0000-000000001339\" } ]";
        if (definition) {
            sections +=
                ",\n\"bindings\": [ { \"id\": \"3da91a64-b947-4560-0000-00000000133a\", \"variable\": "
                "\"3da91a64-b947-4560-0000-000000001ef2\", \"scope\": \"voice\", \"target\": 1,\n"
                "  \"curve\": { \"points\": [[0.0, 0.0], [1.0, " +
                std::string(curveEnd) +
                "]], \"handles\": [[0.0, 0.0, 1.0, " + curveEnd +
                "]] } } ]";
        }
        if (attachment) {
            sections +=
                ",\n\"attachments\": [ { \"binding\": \"3da91a64-b947-4560-0000-00000000133a\", "
                "\"scope\": \"voice\", \"object\": \"3da91a64-b947-4560-0000-000000001339\" } ]";
        }
        return V2LogicManifest(output, "content", sections);
    };
    const std::string pathA = TempPath("pr_guard_pitch_a.pack");
    const std::string pathB = TempPath("pr_guard_pitch_b.pack");
    const std::string pathC = TempPath("pr_guard_pitch_c.pack");
    const std::string pathD = TempPath("pr_guard_pitch_d.pack");
    ASSERT_TRUE(BuildPackFile("pr_guard_pitch_a",
                              manifest(pathA, "300.0", true, true)));
    ASSERT_TRUE(BuildPackFile("pr_guard_pitch_b",
                              manifest(pathB, "600.0", true, true)));
    ASSERT_TRUE(BuildPackFile("pr_guard_pitch_c",
                              manifest(pathC, "600.0", false, true)));
    ASSERT_TRUE(BuildPackFile("pr_guard_pitch_d",
                              manifest(pathD, "0.0", false, false)));
    EngineSession session(MakeNullConfig(2, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(pathA, &pack));
    VnVoiceHandle voice = 0;
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(4921));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    VnVoiceHandle render = harkentest::RenderHandle(engine, voice);
    ASSERT_NE(0u, render);
    harken::CompletionMutationBinding binding{};
    ASSERT_TRUE(PinCompletionOwner(*engine, voice, render, 92, 632,
                                   &binding));

    EXPECT_EQ(VN_BACKPRESSURE, ReplacePack(pack, pathB));
    EXPECT_EQ(nullptr, engine->logicPacks.PendingFor(pack));
    EXPECT_EQ(1u, engine->bindings.Find(harkentest::ObjectId(4922))->revision);
    ASSERT_TRUE(engine->ledger.ClearCompletionMutationExact(
        static_cast<std::uint32_t>(voice), binding));
    ASSERT_TRUE(engine->timelineOccurrences.RollbackCompletionOwnerPinExact(
        binding.occurrence, binding.owner));
    ASSERT_EQ(VN_OK, ReplacePack(pack, pathB));
    ASSERT_TRUE(PinCompletionOwner(*engine, voice, render, 93, 633,
                                   &binding));
    const std::uint64_t gapBackpressure =
        engine->commands.BackpressureCount();
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_NE(nullptr, engine->logicPacks.PendingFor(pack));
    EXPECT_EQ(1u, engine->bindings.Find(harkentest::ObjectId(4922))->revision);
    EXPECT_EQ(gapBackpressure, engine->commands.BackpressureCount());
    ASSERT_TRUE(engine->ledger.ClearCompletionMutationExact(
        static_cast<std::uint32_t>(voice), binding));
    ASSERT_TRUE(engine->timelineOccurrences.RollbackCompletionOwnerPinExact(
        binding.occurrence, binding.owner));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->logicPacks.PendingFor(pack));
    EXPECT_EQ(2u, engine->bindings.Find(harkentest::ObjectId(4922))->revision);

    ASSERT_EQ(VN_OK, Vn_Stop(voice, 0));
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    render = harkentest::RenderHandle(engine, voice);
    ASSERT_NE(0u, render);

    ASSERT_TRUE(PinCompletionOwner(*engine, voice, render, 94, 634,
                                   &binding));
    EXPECT_EQ(VN_BACKPRESSURE, ReplacePack(pack, pathC));
    EXPECT_TRUE(engine->bindings.HasAttachment(
        harkentest::ObjectId(4922), VN_TARGET_SCOPE_VOICE, harkentest::ObjectId(4921)));
    ASSERT_TRUE(engine->ledger.ClearCompletionMutationExact(
        static_cast<std::uint32_t>(voice), binding));
    ASSERT_TRUE(engine->timelineOccurrences.RollbackCompletionOwnerPinExact(
        binding.occurrence, binding.owner));
    ASSERT_EQ(VN_OK, ReplacePack(pack, pathC));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FALSE(engine->bindings.HasAttachment(
        harkentest::ObjectId(4922), VN_TARGET_SCOPE_VOICE, harkentest::ObjectId(4921)));

    ASSERT_EQ(VN_OK, Vn_Stop(voice, 0));
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    render = harkentest::RenderHandle(engine, voice);
    ASSERT_NE(0u, render);
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(voice, harkentest::ObjectId(4922)));
    ASSERT_TRUE(PinCompletionOwner(*engine, voice, render, 95, 635,
                                   &binding));
    EXPECT_EQ(VN_BACKPRESSURE, ReplacePack(pack, pathD));
    EXPECT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(4922)));
    ASSERT_TRUE(engine->ledger.ClearCompletionMutationExact(
        static_cast<std::uint32_t>(voice), binding));
    ASSERT_TRUE(engine->timelineOccurrences.RollbackCompletionOwnerPinExact(
        binding.occurrence, binding.owner));
    EXPECT_EQ(VN_IN_USE, ReplacePack(pack, pathD));
    ASSERT_EQ(VN_OK, Vn_ClearVoiceBinding(voice, harkentest::ObjectId(4922)));
    ASSERT_EQ(VN_OK, ReplacePack(pack, pathD));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->bindings.Find(harkentest::ObjectId(4922)));
}

// --- staging: attachment ownership rebirth during the gap ---

TEST(PackReplaceStaging, GapIdenticalReattachRebirthDiscardsRevision) {
    LogSink sink;
    VnConfig config = MakeNullConfig(4, kBlock, kRate);
    config.logCallback = &LogCb;
    config.logUserData = &sink;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // One media, one voice-scope definition on it, one attachment. The
    // new revision changes the definition's structural identity (a new
    // variable), so the diff is remove + add for the definition, and it
    // drags the byte-identically restated attachment into a remove + add
    // pair with it.
    const std::string wav = TempPath("pr_reb_m1.wav");
    WriteWavF32(wav, MakeDC(kBlock, 0.25f), 1, kRate);
    const auto manifest = [&](const std::string& out, std::uint64_t variable) {
        return V2LogicManifest(
            out, "content",
            "\"entries\": [ { \"path\": \"" + wav +
                "\", \"id\": \"3da91a64-b947-4560-0000-000000001389\" } ],\n"
                "\"bindings\": [ { \"id\": \"3da91a64-b947-4560-0000-000000001393\", \"variable\": " +
                harkentest::JsonObjectId(harkentest::ObjectId(variable)) +
                ", \"scope\": \"voice\", \"target\": 0,\n"
                "    \"curve\": { \"points\": [[0.0, 0.0], [1.0, -6.0]],\n"
                "               \"handles\": [[0.0, 0.0, 1.0, -6.0]] } } ],\n" +
                std::string(R"("attachments": [ { "binding": "3da91a64-b947-4560-0000-000000001393",
                    "scope": "voice", "object": "3da91a64-b947-4560-0000-000000001389" } ])"));
    };
    const std::string v1Path = TempPath("pr_reb_v1.pack");
    const std::string v2Path = TempPath("pr_reb_v2.pack");
    ASSERT_TRUE(BuildPackFile("pr_reb_v1", manifest(v1Path, 7801)));
    ASSERT_TRUE(BuildPackFile("pr_reb_v2", manifest(v2Path, 7802)));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(v1Path, &pack));

    // At the call the triple is still the pack's own, so the identity
    // change screens clean and the revision stages.
    ASSERT_EQ(VN_OK, ReplacePack(pack, v2Path));
    EXPECT_EQ(1u, engine->logicPacks.pending.size());

    // The gap: the caller detaches the pack's triple, then re-attaches
    // the IDENTICAL triple — a caller-owned rebirth that existence and
    // count read exactly like the pack's original.
    VnBindingAttachDesc triple{};
    triple.structSize = sizeof(VnBindingAttachDesc);
    triple.scopeKind = VN_TARGET_SCOPE_VOICE;
    triple.objectId = harkentest::ObjectId(5001);
    ASSERT_EQ(VN_OK, Vn_DetachBinding(harkentest::ObjectId(5011), &triple));
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(5011), &triple));

    // Activation: the removal's target is now a FOREIGN attachment on
    // the definition the revision must remove, so the re-screen discards
    // the revision whole — never steals the caller's triple.
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_TRUE(engine->logicPacks.pending.empty());
    EXPECT_EQ(1u, sink.Count(harken::kLogCodePackRevisionDiscarded));

    // The old definition is fully in force with its old identity, and
    // the caller's attachment is intact.
    const harken::BindingTable::Definition* def = engine->bindings.Find(harkentest::ObjectId(5011));
    ASSERT_NE(nullptr, def);
    EXPECT_EQ(harkentest::ObjectId(7801u), def->variableId);
    EXPECT_EQ(1u, engine->bindings.AttachmentCountOf(harkentest::ObjectId(5011)));

    // Still caller-owned: the unload gate sees a foreign reference and
    // refuses while the reborn triple is attached; releasing it admits
    // the unload.
    EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
    EXPECT_EQ(1u, engine->packs.LoadedCount());
    ASSERT_EQ(VN_OK, Vn_DetachBinding(harkentest::ObjectId(5011), &triple));
    EXPECT_EQ(VN_OK, Vn_UnloadPack(pack));
}

// --- staging: gap recycle of a registry-entry swap target ---


TEST(PackReplaceSemantics, LiveOwnedAttachmentRemovalFundsAdditionOnFullPool) {
    LogSink sink;
    VnConfig config = MakeNullConfig(4, kBlock, kRate);
    config.maxBindingAttachments = 2;
    config.logCallback = &LogCb;
    config.logUserData = &sink;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // Three media, one voice-scope definition. The loaded revision
    // attaches it to M1; the new revision drops that attachment and adds
    // one on M3 (same definition, different object).
    const std::string wavM1 = TempPath("pr_natt_m1.wav");
    const std::string wavM2 = TempPath("pr_natt_m2.wav");
    const std::string wavM3 = TempPath("pr_natt_m3.wav");
    WriteWavF32(wavM1, MakeDC(kBlock, 0.25f), 1, kRate);
    WriteWavF32(wavM2, MakeDC(kBlock, 0.25f), 1, kRate);
    WriteWavF32(wavM3, MakeDC(kBlock, 0.25f), 1, kRate);
    const auto manifest = [&](const std::string& out, std::uint64_t object) {
        return V2LogicManifest(
            out, "content",
            "\"entries\": [ { \"path\": \"" + wavM1 +
                "\", \"id\": \"3da91a64-b947-4560-0000-0000000015eb\" },\n  { \"path\": \"" + wavM2 +
                "\", \"id\": \"3da91a64-b947-4560-0000-0000000015ec\" },\n  { \"path\": \"" + wavM3 +
                "\", \"id\": \"3da91a64-b947-4560-0000-0000000015ed\" } ],\n" +
                std::string(
                    R"("bindings": [ { "id": "3da91a64-b947-4560-0000-0000000015f5", "variable": "3da91a64-b947-4560-0000-000000001e7b",
                "scope": "voice", "target": 0,
                "curve": { "points": [[0.0, 0.0], [1.0, -6.0]],
                           "handles": [[0.0, 0.0, 1.0, -6.0]] } } ],
)") + "\"attachments\": [ { \"binding\": \"3da91a64-b947-4560-0000-0000000015f5\", \"scope\": \"voice\", "
                "\"object\": " +
                harkentest::JsonObjectId(harkentest::ObjectId(object)) + " } ]");
    };
    const std::string v1Path = TempPath("pr_natt_v1.pack");
    const std::string v2Path = TempPath("pr_natt_v2.pack");
    ASSERT_TRUE(BuildPackFile("pr_natt_v1", manifest(v1Path, 5611)));
    ASSERT_TRUE(BuildPackFile("pr_natt_v2", manifest(v2Path, 5613)));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(v1Path, &pack));

    // The caller fills the last pool slot: two attachments live, the
    // two-slot pool is exactly full.
    VnBindingAttachDesc callerTriple{};
    callerTriple.structSize = sizeof(VnBindingAttachDesc);
    callerTriple.scopeKind = VN_TARGET_SCOPE_VOICE;
    callerTriple.objectId = harkentest::ObjectId(5612);
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(5621), &callerTriple));
    ASSERT_EQ(2u, engine->bindings.AttachmentCount());

    // The revision's removal is live AND pack-owned, so it funds the
    // addition: net 2 within capacity 2, staged at the call.
    ASSERT_EQ(VN_OK, ReplacePack(pack, v2Path));

    // The activation applies whole — no discard, no apply defect: the
    // pack's M1 triple detached and the M3 triple attached in its slot.
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_TRUE(engine->logicPacks.pending.empty());
    EXPECT_EQ(0u, sink.Count(harken::kLogCodePackRevisionDiscarded));
    EXPECT_EQ(0u, sink.Count(harken::kLogCodePackApplyDefect));
    EXPECT_FALSE(
        engine->bindings.HasAttachment(harkentest::ObjectId(5621), VN_TARGET_SCOPE_VOICE, harkentest::ObjectId(5611)));
    EXPECT_TRUE(
        engine->bindings.HasAttachment(harkentest::ObjectId(5621), VN_TARGET_SCOPE_VOICE, harkentest::ObjectId(5613)));
    EXPECT_TRUE(
        engine->bindings.HasAttachment(harkentest::ObjectId(5621), VN_TARGET_SCOPE_VOICE, harkentest::ObjectId(5612)));
    EXPECT_EQ(2u, engine->bindings.AttachmentCountOf(harkentest::ObjectId(5621)));
}

TEST(PackReplaceSemantics, AbsentAttachmentRemovalFundsNothing) {
    LogSink sink;
    VnConfig config = MakeNullConfig(4, kBlock, kRate);
    config.maxBindingAttachments = 2;
    config.logCallback = &LogCb;
    config.logUserData = &sink;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // Same base shape as the positive twin, but the revision drops the
    // pack's M1 attachment and adds TWO new ones (M3 and M4).
    const std::string wavM1 = TempPath("pr_nabs_m1.wav");
    const std::string wavM2 = TempPath("pr_nabs_m2.wav");
    const std::string wavM3 = TempPath("pr_nabs_m3.wav");
    const std::string wavM4 = TempPath("pr_nabs_m4.wav");
    WriteWavF32(wavM1, MakeDC(kBlock, 0.25f), 1, kRate);
    WriteWavF32(wavM2, MakeDC(kBlock, 0.25f), 1, kRate);
    WriteWavF32(wavM3, MakeDC(kBlock, 0.25f), 1, kRate);
    WriteWavF32(wavM4, MakeDC(kBlock, 0.25f), 1, kRate);
    const auto manifest = [&](const std::string& out,
                              const std::string& attachments) {
        return V2LogicManifest(
            out, "content",
            "\"entries\": [ { \"path\": \"" + wavM1 +
                "\", \"id\": \"3da91a64-b947-4560-0000-00000000164f\" },\n  { \"path\": \"" + wavM2 +
                "\", \"id\": \"3da91a64-b947-4560-0000-000000001650\" },\n  { \"path\": \"" + wavM3 +
                "\", \"id\": \"3da91a64-b947-4560-0000-000000001651\" },\n  { \"path\": \"" + wavM4 +
                "\", \"id\": \"3da91a64-b947-4560-0000-000000001652\" } ],\n" +
                std::string(
                    R"("bindings": [ { "id": "3da91a64-b947-4560-0000-000000001659", "variable": "3da91a64-b947-4560-0000-000000001e7c",
                "scope": "voice", "target": 0,
                "curve": { "points": [[0.0, 0.0], [1.0, -6.0]],
                           "handles": [[0.0, 0.0, 1.0, -6.0]] } } ],
)") + "\"attachments\": [ " + attachments + " ]");
    };
    const std::string v1Path = TempPath("pr_nabs_v1.pack");
    const std::string v2Path = TempPath("pr_nabs_v2.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_nabs_v1",
        manifest(v1Path,
                 R"({ "binding": "3da91a64-b947-4560-0000-000000001659", "scope": "voice", "object": "3da91a64-b947-4560-0000-00000000164f" })")));
    ASSERT_TRUE(BuildPackFile(
        "pr_nabs_v2",
        manifest(v2Path,
                 R"({ "binding": "3da91a64-b947-4560-0000-000000001659", "scope": "voice", "object": "3da91a64-b947-4560-0000-000000001651" },
                 { "binding": "3da91a64-b947-4560-0000-000000001659", "scope": "voice", "object": "3da91a64-b947-4560-0000-000000001652" })")));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(v1Path, &pack));

    // The caller fills the pool, then detaches the pack's own triple:
    // one slot is genuinely free, but the recorded removal target is
    // ABSENT — the net arithmetic must not credit the phantom.
    VnBindingAttachDesc callerTriple{};
    callerTriple.structSize = sizeof(VnBindingAttachDesc);
    callerTriple.scopeKind = VN_TARGET_SCOPE_VOICE;
    callerTriple.objectId = harkentest::ObjectId(5712);
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(5721), &callerTriple));
    VnBindingAttachDesc packTriple{};
    packTriple.structSize = sizeof(VnBindingAttachDesc);
    packTriple.scopeKind = VN_TARGET_SCOPE_VOICE;
    packTriple.objectId = harkentest::ObjectId(5711);
    ASSERT_EQ(VN_OK, Vn_DetachBinding(harkentest::ObjectId(5721), &packTriple));
    ASSERT_EQ(1u, engine->bindings.AttachmentCount());

    // Two adds need two slots and only one is free: refused at the call
    // with the pool code, nothing staged.
    const std::uint64_t refusedBefore = Snap().hotReloadRefusedCount;
    EXPECT_EQ(VN_NO_BINDING, ReplacePack(pack, v2Path));
    EXPECT_EQ(refusedBefore + 1, Snap().hotReloadRefusedCount);
    EXPECT_TRUE(engine->logicPacks.pending.empty());

    // Zero staging footprint: the next update discards nothing and the
    // old state is unchanged — the caller's triple alone, the pack's
    // triple still detached, no added triple anywhere.
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, sink.Count(harken::kLogCodePackRevisionDiscarded));
    const harken::BindingTable::Definition* def = engine->bindings.Find(harkentest::ObjectId(5721));
    ASSERT_NE(nullptr, def);
    EXPECT_EQ(harkentest::ObjectId(7804u), def->variableId);
    EXPECT_EQ(1u, engine->bindings.AttachmentCountOf(harkentest::ObjectId(5721)));
    EXPECT_TRUE(
        engine->bindings.HasAttachment(harkentest::ObjectId(5721), VN_TARGET_SCOPE_VOICE, harkentest::ObjectId(5712)));
    EXPECT_FALSE(
        engine->bindings.HasAttachment(harkentest::ObjectId(5721), VN_TARGET_SCOPE_VOICE, harkentest::ObjectId(5711)));
    EXPECT_FALSE(
        engine->bindings.HasAttachment(harkentest::ObjectId(5721), VN_TARGET_SCOPE_VOICE, harkentest::ObjectId(5713)));
    EXPECT_FALSE(
        engine->bindings.HasAttachment(harkentest::ObjectId(5721), VN_TARGET_SCOPE_VOICE, harkentest::ObjectId(5714)));
}

// --- semantics: revision-wide trigger-cycle and nesting screens ---


TEST(PackReplace, DirectDestroyCannotInvalidatePackDependency) {
    EngineSession session(MakeNullConfig(4, kBlock, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::string basePath = TempPath("pr_destroy_base.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_destroy_base",
        V2LogicManifest(basePath, "resident",
                        R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000000bb9", "parent": "00000000-0000-0000-0000-000000000001" } ])")));
    VnPackHandle base = 0;
    ASSERT_EQ(VN_OK, LoadPack(basePath, &base));

    const std::string depPath = TempPath("pr_destroy_dep.pack");
    ASSERT_TRUE(BuildPackFile(
        "pr_destroy_dep",
        V2LogicManifest(depPath, "resident",
                        R"("buses": [ { "id": "3da91a64-b947-4560-0000-000000000bba", "parent": "3da91a64-b947-4560-0000-000000000bb9" } ])")));

    ASSERT_EQ(VN_IN_USE, Vn_DestroyBus(harkentest::ObjectId(3001)));
    VnPackHandle dep = 0;
    EXPECT_EQ(VN_OK, LoadPack(depPath, &dep));
}

} // namespace
