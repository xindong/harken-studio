// Hot reload: the replace verbs — the media/registry verbs under the
// retire-old / new-plays-new migration contract, and the live-logic-map
// verbs (attenuation, binding) that swap in place at a tick boundary; all
// share the bottleneck accounting. Covers: a PCM replace mid-play leaving the playing
// voice on old data to its natural end (golden, bit-equal DC blocks) while a
// new play sounds the new data (golden ratio); the one-retiring-generation-
// per-asset VN_IN_USE bound and its release after the drain; the pack
// whole-generation swap with the present/absent-entry boundary (a removed id
// refuses new plays the moment the replace returns while the old voice keeps
// draining, the handle survives); stream rebind with the old file serving
// existing voices past the ring prefill; attenuation re-curving a live voice
// on the next tick (golden, closed-form rolloff values); container replace
// picking new children with the benign pair-state reset; a parked voice returning on the generation it started
// on; the four snapshot counters and the per-outcome log-ring records; and
// one all-or-nothing refusal per verb proving the old content stays fully in
// force.
#include "Common/ObjectIdTestSupport.h"
#include "Common/StreamBuilder.h"
#include "Common/TestUtil.h"

#include "harken/core/LogRing.h"
#include "harken/core/StableRowStorage.h"
#include "harken/media/PackFormat.h"
#include "harken/media/StreamManager.h"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the repository's .temp directory"
#endif

namespace {

using harkentest::BuildStreamBytes;
using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeAttenuationDesc;
using harkentest::MakeDC;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MakePlayDesc3D;
using harkentest::MonoDesc;
using harkentest::PumpAndCapture;
using harkentest::RenderHandle;
using harkentest::StreamSpec;
using harkentest::Vec3;
using harkentest::WriteFileBytes;

constexpr std::uint32_t kBlock = 256;
constexpr std::uint32_t kRate = 48000;

std::string TempPath(const std::string& name) {
    return std::string(HARKEN_TEMP_DIR "/") + name;
}

VnConfig ReloadConfig(std::uint32_t maxVoices = 8) {
    return MakeNullConfig(maxVoices, kBlock, kRate);
}

VnSnapshot Snap() {
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    return snapshot;
}

void PumpAndUpdate(harken::Engine* engine, std::uint32_t blocks) {
    engine->nullBackend.Pump(blocks);
    ASSERT_EQ(VN_OK, Vn_Update());
}

// PCM registration with a caller-chosen length so a replaced registration is
// distinguishable through the registry view alone.
void RegisterDc(VnSoundId id, float amplitude, std::size_t frames) {
    const std::vector<float> dc = MakeDC(frames, amplitude);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(id, &desc));
}

VnResult ReplaceDc(VnSoundId id, float amplitude, std::size_t frames) {
    const std::vector<float> dc = MakeDC(frames, amplitude);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    return Vn_ReplacePCM(id, &desc);
}

// --- test-local pack builder (the pack_test shapes) ---

struct PackEntrySpec {
    VnSoundId id = harkentest::ObjectId(0);
    std::vector<float> planar; // mono plane
};

std::uint64_t AlignUp64(std::uint64_t value) {
    const std::uint64_t align = harken::kPackBlobAlign;
    return (value + align - 1) / align * align;
}

std::vector<std::uint8_t> BuildPackBytes(
    const std::vector<PackEntrySpec>& specs) {
    const std::uint64_t tocOffset = harken::kPackHeaderBytes;
    const std::uint64_t tocBytes =
        static_cast<std::uint64_t>(specs.size()) * harken::kPackTocEntryBytes;
    std::vector<std::uint64_t> blobOffsets(specs.size());
    std::uint64_t cursor = AlignUp64(tocOffset + tocBytes);
    std::uint64_t fileBytes = tocOffset + tocBytes;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        blobOffsets[i] = cursor;
        const std::uint64_t blobBytes =
            static_cast<std::uint64_t>(specs[i].planar.size()) * sizeof(float);
        fileBytes = cursor + blobBytes;
        cursor = AlignUp64(fileBytes);
    }
    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileBytes), 0);
    std::vector<std::uint8_t> toc(static_cast<std::size_t>(tocBytes), 0);
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const PackEntrySpec& spec = specs[i];
        const std::uint64_t blobBytes =
            static_cast<std::uint64_t>(spec.planar.size()) * sizeof(float);
        std::memcpy(bytes.data() + blobOffsets[i], spec.planar.data(),
                    static_cast<std::size_t>(blobBytes));
        harken::PackTocEntry entry;
        entry.soundId = spec.id;
        entry.sampleRate = kRate;
        entry.channelCount = 1;
        entry.sampleFormat = harken::kPackSampleFormatF32Planar;
        entry.frameCount = spec.planar.size();
        entry.blobOffset = blobOffsets[i];
        entry.blobBytes = blobBytes;
        entry.blobChecksum =
            harken::Fnv1a64(bytes.data() + blobOffsets[i],
                           static_cast<std::size_t>(blobBytes));
        entry.flags = 0;
        entry.loopStartFrame = 0;
        entry.loopEndFrame = 0;
        harken::SerializePackTocEntry(
            entry, toc.data() + i * harken::kPackTocEntryBytes);
    }
    std::memcpy(bytes.data() + tocOffset, toc.data(), toc.size());
    harken::PackHeader header;
    header.formatVersion = harken::kPackFormatVersion;
    header.headerBytes = harken::kPackHeaderBytes;
    header.fileBytes = fileBytes;
    header.entryCount = static_cast<std::uint32_t>(specs.size());
    header.tocOffset = static_cast<std::uint32_t>(tocOffset);
    header.tocChecksum = harken::Fnv1a64(toc.data(), toc.size());
    harken::SerializePackHeader(header, bytes.data());
    return bytes;
}

PackEntrySpec DcEntry(VnSoundId id, float amplitude, std::size_t frames) {
    PackEntrySpec spec;
    spec.id = id;
    spec.planar = MakeDC(frames, amplitude);
    return spec;
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

harken::CompletionOwnerRef CompletionOwner(std::uint32_t slot,
                                          std::uint64_t factsRevision) {
    return harken::CompletionOwnerRef::Single({slot, 1}, factsRevision);
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
        CompletionOwner(ownerSlot, factsRevision);
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

// --- log sink (drained code records) ---

struct LogSink {
    struct Rec {
        std::uint32_t level;
        std::uint32_t code;
        std::uint64_t arg0;
        VnObjectId objectId;
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
    std::size_t Count(std::uint32_t code, std::uint64_t arg0,
                      VnObjectId objectId) const {
        std::size_t n = 0;
        for (const Rec& r : records) {
            if (r.code == code && r.arg0 == arg0 && r.objectId == objectId) {
                ++n;
            }
        }
        return n;
    }
};
void LogCb(void* userData, std::uint32_t level, std::uint32_t code,
           const char* text) {
    std::uint64_t arg0 = 0;
    VnObjectId objectId{};
    if (code == harken::kLogCodeHotReloadApplied ||
        code == harken::kLogCodeHotReloadRefused) {
        unsigned long long parsedArg0 = 0;
        char parsedId[37]{};
        const char* format =
            code == harken::kLogCodeHotReloadApplied
                ? "hot reload applied: asset class %llu id %36s"
                : "hot reload refused: asset class %llu id %36s";
        if (std::sscanf(text, format, &parsedArg0, parsedId) == 2) {
            arg0 = static_cast<std::uint64_t>(parsedArg0);
            harken::ParseObjectId(parsedId, &objectId);
        }
    }
    static_cast<LogSink*>(userData)->records.push_back(
        {level, code, arg0, objectId, text});
}

// --- PCM: old voice drains old data, new plays sound new data (golden) ---

TEST(HotReload, PCMOldVoiceKeepsOldDataNewPlaySoundsNewData) {
    EngineSession session(ReloadConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const auto pageBytes = harken::detail::StablePageReservation::SystemPageBytes();
    ASSERT_NE(0u, pageBytes);
    const auto largeBlocks = static_cast<std::uint32_t>(
        17 * pageBytes / (kBlock * sizeof(float)) + 1);
    for (const auto oldBlocks : {4u, largeBlocks, 2 * largeBlocks}) {
        SCOPED_TRACE(oldBlocks);
        // Keep heap, float page and compact page generations pinned by a real voice.
        RegisterDc(harkentest::ObjectId(100), 0.25f, oldBlocks * kBlock);
        const harken::PCMView oldGeneration = engine->registry.Lookup(harkentest::ObjectId(100));
        ASSERT_NE(0u, oldGeneration.contentRevision);
        if (oldBlocks == 2 * largeBlocks)
            ASSERT_EQ(harken::PcmSampleFormat::ExactInt16, oldGeneration.sampleFormat);

        VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(100));
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        Capture before;
        PumpAndCapture(engine, 2, before); // blocks 1-2: steady DC
        HARKENTEST_ASSERT_NON_SILENT(before.left);
        const float oldLevel = before.left[kBlock + kBlock / 2];

        // Replace mid-play: the voice keeps consuming the OLD samples.
        ASSERT_EQ(VN_OK, ReplaceDc(harkentest::ObjectId(100), 0.5f, 2 * kBlock));
        EXPECT_EQ(1u, Snap().pinnedGenerationCount); // old generation pinned
        const harken::PCMView currentGeneration = engine->registry.Lookup(harkentest::ObjectId(100));
        EXPECT_NE(oldGeneration.contentRevision,
                  currentGeneration.contentRevision);
        Capture after;
        PumpAndCapture(engine, 1, after); // block 3: still the old data
        EXPECT_EQ(oldLevel, after.left[kBlock / 2]); // DC: bit-equal continuation

        // New resolution sees the new registration immediately.
        EXPECT_EQ(2u * kBlock, engine->registry.Lookup(harkentest::ObjectId(100)).frames);

        // Old voice reaches its natural end, then the generation
        // drains and retires on the update.
        PumpAndUpdate(engine, oldBlocks - 2);
        EXPECT_EQ(0u, engine->voices.ActiveCount());
        EXPECT_EQ(0u, Snap().pinnedGenerationCount);
        EXPECT_EQ(1u, Snap().pinnedGenerationHighWater);

        // A fresh play sounds the NEW data: DC 0.5 doubles the channel value.
        VnVoiceHandle fresh = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &fresh));
        Capture renewed;
        PumpAndCapture(engine, 1, renewed);
        HARKENTEST_ASSERT_NON_SILENT(renewed.left);
        EXPECT_FLOAT_EQ(2.0f * oldLevel, renewed.left[kBlock / 2]);
        PumpAndUpdate(engine, 2);
        ASSERT_EQ(VN_OK, Vn_UnregisterPCM(harkentest::ObjectId(100)));
    }
}

TEST(HotReload, PCMSecondReplaceWhileDrainingRefusedThenAdmitted) {
    EngineSession session(ReloadConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterDc(harkentest::ObjectId(101), 0.25f, 2 * kBlock);
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(101));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    PumpAndUpdate(engine, 1); // voice is sounding

    ASSERT_EQ(VN_OK, ReplaceDc(harkentest::ObjectId(101), 0.5f, kBlock));
    // The old generation still has its voice: a second replace is refused
    // as honest backpressure, leaving the first replacement in force.
    EXPECT_EQ(VN_IN_USE, ReplaceDc(harkentest::ObjectId(101), 0.75f, kBlock));
    EXPECT_EQ(kBlock, engine->registry.Lookup(harkentest::ObjectId(101)).frames);
    const VnSnapshot mid = Snap();
    EXPECT_EQ(1u, mid.hotReloadCount);
    EXPECT_EQ(1u, mid.hotReloadRefusedCount);

    // Drain the old voice; the sweep frees the generation, and the same
    // second replace is admitted.
    PumpAndUpdate(engine, 2);
    EXPECT_EQ(0u, Snap().pinnedGenerationCount);
    EXPECT_EQ(VN_OK, ReplaceDc(harkentest::ObjectId(101), 0.75f, 3 * kBlock));
    EXPECT_EQ(3u * kBlock, engine->registry.Lookup(harkentest::ObjectId(101)).frames);
}

TEST(HotReload, IndividuallyReplacedPcmKeepsOriginalPackBackingUntilVoiceDrains) {
    for (const bool replacePack : {false, true}) {
        SCOPED_TRACE(replacePack);
        EngineSession session(ReloadConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        auto* engine = session.Engine();
        const auto sound = harkentest::ObjectId(105);
        const auto successorSound = harkentest::ObjectId(106);
        const auto path = TempPath("hot_reload_original_backing.pack");
        const auto successorPath = TempPath("hot_reload_successor_backing.pack");
        WriteFileBytes(path, BuildPackBytes({DcEntry(sound, 0.25f, 16 * kBlock)}));
        WriteFileBytes(successorPath,
                       BuildPackBytes({DcEntry(successorSound, 0.75f, kBlock)}));
        VnPackHandle pack = 0;
        ASSERT_EQ(VN_OK, LoadPack(path, &pack));
        const auto originalCookie = engine->registry.CookieOf(sound);
        VnVoiceHandle voice = 0;
        const auto play = MakePlayDesc(sound);
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        Capture before;
        PumpAndCapture(engine, 2, before);
        const auto oldLevel = before.left[kBlock + kBlock / 2];
        ASSERT_GT(oldLevel, 0.0f);

        ASSERT_EQ(VN_OK, ReplaceDc(sound, 0.5f, 4 * kBlock));
        const auto replacementCookie = engine->registry.CookieOf(sound);
        ASSERT_NE(originalCookie, replacementCookie);
        ASSERT_EQ(1u, engine->registry.RetiredCount());
        if (replacePack) {
            ASSERT_EQ(VN_OK, ReplacePack(pack, successorPath));
            EXPECT_EQ(1u, engine->packs.RetiringCount());
            // Do not read potentially freed samples when the retention gate fails.
            if (engine->packs.RetiringCount() != 1u) continue;
        } else {
            EXPECT_EQ(VN_IN_USE, engine->packs.CanUnload(pack, engine->registry));
            const auto result = Vn_UnloadPack(pack);
            EXPECT_EQ(VN_IN_USE, result);
            if (result != VN_IN_USE) continue;
        }
        Capture after;
        PumpAndCapture(engine, 1, after);
        EXPECT_EQ(oldLevel, after.left[kBlock / 2]);
        ASSERT_EQ(VN_OK, Vn_Stop(voice, 0));
        PumpAndUpdate(engine, 2);
        EXPECT_EQ(0u, engine->registry.RetiredCount());
        EXPECT_EQ(0u, engine->packs.RetiringCount());
        ASSERT_EQ(VN_OK, Vn_UnloadPack(pack));
        EXPECT_EQ(0u, engine->packs.LoadedCount());
        EXPECT_EQ(replacementCookie, engine->registry.CookieOf(sound));
        EXPECT_FLOAT_EQ(0.5f, engine->registry.Lookup(sound).ReadSample(0, 0));
        ASSERT_EQ(VN_OK, Vn_UnregisterPCM(sound));
        EXPECT_EQ(0u, engine->registry.Count());
    }
}

TEST(HotReload, PCMRefusalsAreAllOrNothingAndNeverRegister) {
    EngineSession session(ReloadConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterDc(harkentest::ObjectId(102), 0.25f, kBlock);

    // Invalid descriptor: the register path's validation, old content kept.
    std::vector<float> dc = MakeDC(kBlock, 0.5f);
    VnPCMDesc bad = MonoDesc(dc, kRate);
    bad.frames = 0;
    EXPECT_EQ(VN_INVALID_ARG, Vn_ReplacePCM(harkentest::ObjectId(102), &bad));
    EXPECT_EQ(kBlock, engine->registry.Lookup(harkentest::ObjectId(102)).frames);

    // Unknown id: replace never implicitly registers.
    const VnPCMDesc good = MonoDesc(dc, kRate);
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_ReplacePCM(harkentest::ObjectId(999), &good));
    EXPECT_FALSE(engine->registry.Lookup(harkentest::ObjectId(999)).Valid());

}

// --- pack: whole-generation swap, present/absent boundary, handle survival ---

TEST(HotReload, PackSwapIsWholeGenerationWithImmediateNewToc) {
    EngineSession session(ReloadConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string pathA = TempPath("hot_reload_a.pack");
    const std::string pathB = TempPath("hot_reload_b.pack");
    WriteFileBytes(pathA, BuildPackBytes({DcEntry(harkentest::ObjectId(21), 0.25f, 8 * kBlock),
                                          DcEntry(harkentest::ObjectId(22), 0.25f, kBlock)}));
    WriteFileBytes(pathB, BuildPackBytes({DcEntry(harkentest::ObjectId(21), 0.5f, 2 * kBlock),
                                          DcEntry(harkentest::ObjectId(23), 0.5f, kBlock)}));
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(pathA, &pack));

    // A voice on entry 21 spans the swap.
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(21));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    Capture before;
    PumpAndCapture(engine, 2, before);
    HARKENTEST_ASSERT_NON_SILENT(before.left);
    const float oldLevel = before.left[kBlock + kBlock / 2];

    ASSERT_EQ(VN_OK, ReplacePack(pack, pathB));

    // New TOC is live the moment the call returns: the removed id refuses
    // NEW plays immediately (no wrong-sound window), the added id resolves,
    // and the shared id reads the new content facts.
    VnPlayDesc removed = MakePlayDesc(harkentest::ObjectId(22));
    VnVoiceHandle unused = 0;
    EXPECT_EQ(VN_UNKNOWN_ID, harkentest::PlayRaw(&removed, &unused));
    EXPECT_FALSE(engine->registry.Lookup(harkentest::ObjectId(22)).Valid());
    EXPECT_TRUE(engine->registry.Lookup(harkentest::ObjectId(23)).Valid());
    EXPECT_EQ(2u * kBlock, engine->registry.Lookup(harkentest::ObjectId(21)).frames);

    // The old voice keeps draining the retiring generation's data.
    Capture after;
    PumpAndCapture(engine, 1, after);
    EXPECT_EQ(oldLevel, after.left[kBlock / 2]);
    EXPECT_EQ(1u, Snap().pinnedGenerationCount);

    // One retiring generation per pack: a second swap must wait.
    EXPECT_EQ(VN_IN_USE, ReplacePack(pack, pathA));

    // The handle survived the swap: its entry metadata reads the new TOC.
    const std::vector<harken::PackEntry>* entries = engine->packs.Entries(pack);
    ASSERT_NE(nullptr, entries);
    EXPECT_EQ(2u, entries->size());

    // Drain the old voice; the generation retires and a new play of the
    // shared id sounds the NEW data.
    ASSERT_EQ(VN_OK, Vn_Stop(voice, 0));
    PumpAndUpdate(engine, 2);
    EXPECT_EQ(0u, Snap().pinnedGenerationCount);
    VnVoiceHandle fresh = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &fresh));
    Capture renewed;
    PumpAndCapture(engine, 1, renewed);
    HARKENTEST_ASSERT_NON_SILENT(renewed.left);
    EXPECT_FLOAT_EQ(2.0f * oldLevel, renewed.left[kBlock / 2]);

    // With the drain complete the second swap is admitted, and the handle
    // still works for the ordinary unload afterwards.
    EXPECT_EQ(VN_OK, ReplacePack(pack, pathA));
    PumpAndUpdate(engine, 2);
    EXPECT_EQ(VN_OK, Vn_UnloadPack(pack));
    EXPECT_EQ(VN_STALE_HANDLE, ReplacePack(pack, pathA));
}

TEST(HotReload,
     PackReplaceRefusesPinnedCurrentGenerationBeforeWholeGenerationSwap) {
    EngineSession session(ReloadConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::string pathA = TempPath("hot_reload_guarded_pack_a.pack");
    const std::string pathB = TempPath("hot_reload_guarded_pack_b.pack");
    WriteFileBytes(pathA,
                   BuildPackBytes({DcEntry(harkentest::ObjectId(41), 0.25f, 8 * kBlock)}));
    WriteFileBytes(pathB,
                   BuildPackBytes({DcEntry(harkentest::ObjectId(41), 0.5f, 2 * kBlock)}));
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(pathA, &pack));
    VnVoiceHandle voice = 0;
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(41));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    const VnVoiceHandle render = RenderHandle(engine, voice);
    const harken::PCMView before = engine->registry.Lookup(harkentest::ObjectId(41));
    ASSERT_TRUE(before.Valid());
    harken::CompletionMutationBinding binding{};
    ASSERT_TRUE(PinCompletionOwner(*engine, voice, render, 88, 628,
                                   &binding));
    const std::uint64_t backpressure = engine->commands.BackpressureCount();
    const std::uint64_t applied = engine->hotReloadCount;
    const std::uint64_t refused = engine->hotReloadRefusedCount;

    EXPECT_EQ(VN_BACKPRESSURE, ReplacePack(pack, pathB));

    const harken::PCMView after = engine->registry.Lookup(harkentest::ObjectId(41));
    EXPECT_EQ(before.useCount, after.useCount);
    EXPECT_EQ(before.frames, after.frames);
    EXPECT_FALSE(engine->packs.HasRetiring(pack));
    EXPECT_EQ(backpressure + 1u, engine->commands.BackpressureCount());
    EXPECT_EQ(applied, engine->hotReloadCount);
    EXPECT_EQ(refused + 1u, engine->hotReloadRefusedCount);

    ASSERT_TRUE(engine->ledger.ClearCompletionMutationExact(
        static_cast<std::uint32_t>(voice), binding));
    ASSERT_TRUE(engine->timelineOccurrences.RollbackCompletionOwnerPinExact(
        binding.occurrence, binding.owner));
    EXPECT_EQ(VN_OK, ReplacePack(pack, pathB));
    EXPECT_NE(before.useCount, engine->registry.Lookup(harkentest::ObjectId(41)).useCount);
}

TEST(HotReload,
     PackUnloadRefusesPinnedCurrentGenerationBeforeOrdinaryInUseGate) {
    EngineSession session(ReloadConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::string path = TempPath("hot_reload_guarded_unload.pack");
    WriteFileBytes(path,
                   BuildPackBytes({DcEntry(harkentest::ObjectId(42), 0.25f, 8 * kBlock)}));
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(path, &pack));
    VnVoiceHandle voice = 0;
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(42));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    const VnVoiceHandle render = RenderHandle(engine, voice);
    const harken::PCMView before = engine->registry.Lookup(harkentest::ObjectId(42));
    ASSERT_TRUE(before.Valid());
    harken::CompletionMutationBinding binding{};
    ASSERT_TRUE(PinCompletionOwner(*engine, voice, render, 89, 629,
                                   &binding));
    const std::uint64_t backpressure = engine->commands.BackpressureCount();

    EXPECT_EQ(VN_BACKPRESSURE, Vn_UnloadPack(pack));
    EXPECT_EQ(before.useCount, engine->registry.Lookup(harkentest::ObjectId(42)).useCount);
    EXPECT_NE(nullptr, engine->packs.Entries(pack));
    EXPECT_EQ(backpressure + 1u, engine->commands.BackpressureCount());

    ASSERT_TRUE(engine->ledger.ClearCompletionMutationExact(
        static_cast<std::uint32_t>(voice), binding));
    ASSERT_TRUE(engine->timelineOccurrences.RollbackCompletionOwnerPinExact(
        binding.occurrence, binding.owner));
    EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
    EXPECT_EQ(backpressure + 1u, engine->commands.BackpressureCount());
    ASSERT_EQ(VN_OK, Vn_Stop(voice, 0));
    PumpAndUpdate(engine, 2);
    EXPECT_EQ(VN_OK, Vn_UnloadPack(pack));
    EXPECT_FALSE(engine->registry.Lookup(harkentest::ObjectId(42)).Valid());
}

TEST(HotReload, PackReplaceFailuresLeaveOldPackUntouched) {
    EngineSession session(ReloadConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string pathA = TempPath("hot_reload_c.pack");
    const std::string pathBad = TempPath("hot_reload_bad.pack");
    const std::string pathClash = TempPath("hot_reload_clash.pack");
    WriteFileBytes(pathA, BuildPackBytes({DcEntry(harkentest::ObjectId(31), 0.25f, kBlock)}));
    // Corrupt container: flip one payload byte without fixing its checksum.
    std::vector<std::uint8_t> badBytes =
        BuildPackBytes({DcEntry(harkentest::ObjectId(31), 0.5f, kBlock)});
    badBytes.back() ^= 0x01u;
    WriteFileBytes(pathBad, badBytes);
    // Foreign collision: the new pack carries an id someone else registered.
    WriteFileBytes(pathClash, BuildPackBytes({DcEntry(harkentest::ObjectId(32), 0.5f, kBlock)}));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(pathA, &pack));
    RegisterDc(harkentest::ObjectId(32), 0.75f, 2 * kBlock); // the foreign owner of id 32

    EXPECT_EQ(VN_BAD_PACK, ReplacePack(pack, pathBad));
    EXPECT_EQ(kBlock, engine->registry.Lookup(harkentest::ObjectId(31)).frames); // old entry intact

    EXPECT_EQ(VN_IN_USE, ReplacePack(pack, pathClash));
    EXPECT_EQ(kBlock, engine->registry.Lookup(harkentest::ObjectId(31)).frames);
    EXPECT_EQ(2u * kBlock, engine->registry.Lookup(harkentest::ObjectId(32)).frames); // foreign kept
    EXPECT_EQ(0u, Snap().pinnedGenerationCount); // nothing retired
}

// --- stream: rebind with the old file serving existing voices ---

TEST(HotReload, StreamOldVoiceServesOldFileNewPlaySoundsNewFile) {
    VnConfig config = ReloadConfig(/*maxVoices=*/4);
    config.streamRingFrames = 1024; // small ring: prefill exhausts quickly
    config.maxStreams = 4;
    config.prebufferMs = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string pathX = TempPath("hot_reload_x.stream");
    const std::string pathY = TempPath("hot_reload_y.stream");
    const std::string pathZ = TempPath("hot_reload_z.stream");
    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(7301);
    spec.sampleRate = kRate;
    spec.channelCount = 1;
    spec.chunkFrames = kBlock;
    WriteFileBytes(pathX, BuildStreamBytes(spec, MakeDC(kRate, 0.25f)));
    WriteFileBytes(pathY, BuildStreamBytes(spec, MakeDC(kRate / 2, 0.5f)));
    WriteFileBytes(pathZ, BuildStreamBytes(spec, MakeDC(kRate / 4, 0.5f)));
    ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(harkentest::ObjectId(7301), pathX.c_str()));
    harken::StreamManager::StreamFacts oldFacts;
    ASSERT_TRUE(engine->streams.FindFile(harkentest::ObjectId(7301), &oldFacts));
    ASSERT_NE(0u, oldFacts.contentRevision);

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(7301));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    engine->streams.SyncServe();
    Capture before;
    PumpAndCapture(engine, 2, before);
    HARKENTEST_ASSERT_NON_SILENT(before.left);
    const float oldLevel = before.left[kBlock + kBlock / 2];

    ASSERT_EQ(VN_OK, Vn_ReplaceStreamFile(harkentest::ObjectId(7301), pathY.c_str()));
    // The id resolves to the new file immediately (the new frame count),
    // while the live voice keeps streaming the old one.
    harken::StreamManager::StreamFacts facts;
    ASSERT_TRUE(engine->streams.FindFile(harkentest::ObjectId(7301), &facts));
    EXPECT_EQ(kRate / 2, facts.frameCount);
    EXPECT_NE(oldFacts.contentRevision, facts.contentRevision);
    harken::StreamManager::StreamFacts retiringFacts;
    ASSERT_TRUE(engine->streams.FactsForFileIndex(oldFacts.fileIndex,
                                                  &retiringFacts));
    EXPECT_EQ(oldFacts.contentRevision, retiringFacts.contentRevision);
    EXPECT_EQ(1u, Snap().pinnedGenerationCount);

    // Pump well past the ring prefill (ring holds 4 blocks) so these frames
    // provably came from fresh serves of the OLD file.
    Capture after;
    for (int i = 0; i < 8; ++i) {
        engine->streams.SyncServe();
        PumpAndCapture(engine, 1, after);
    }
    EXPECT_EQ(oldLevel, after.left[7 * kBlock + kBlock / 2]);

    // One retiring generation per asset while the old file drains.
    EXPECT_EQ(VN_IN_USE, Vn_ReplaceStreamFile(harkentest::ObjectId(7301), pathZ.c_str()));

    // Drain: stop the voice, let the slot retire and the old file close.
    ASSERT_EQ(VN_OK, Vn_Stop(voice, 0));
    PumpAndUpdate(engine, 1);
    engine->streams.SyncServe(); // detach pass + drained-file close
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, Snap().pinnedGenerationCount);
    EXPECT_EQ(VN_OK, Vn_ReplaceStreamFile(harkentest::ObjectId(7301), pathZ.c_str()));

    // A fresh play sounds the newest file's data (DC 0.5 doubles the value).
    VnVoiceHandle fresh = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &fresh));
    engine->streams.SyncServe();
    Capture renewed;
    PumpAndCapture(engine, 2, renewed);
    HARKENTEST_ASSERT_NON_SILENT(renewed.left);
    EXPECT_FLOAT_EQ(2.0f * oldLevel, renewed.left[kBlock + kBlock / 2]);
}

TEST(HotReload, StreamReplaceRefusalsLeaveRegistrationUntouched) {
    VnConfig config = ReloadConfig();
    config.maxStreams = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string pathX = TempPath("hot_reload_r.stream");
    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(7302);
    spec.sampleRate = kRate;
    spec.channelCount = 1;
    spec.chunkFrames = kBlock;
    WriteFileBytes(pathX, BuildStreamBytes(spec, MakeDC(kRate, 0.25f)));
    ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(harkentest::ObjectId(7302), pathX.c_str()));

    // A truncated file fails validation; the old registration survives.
    const std::string pathBad = TempPath("hot_reload_bad.stream");
    std::vector<std::uint8_t> bytes =
        BuildStreamBytes(spec, MakeDC(kRate, 0.5f));
    bytes.resize(bytes.size() / 2);
    WriteFileBytes(pathBad, bytes);
    EXPECT_EQ(VN_BAD_STREAM_FILE,
              Vn_ReplaceStreamFile(harkentest::ObjectId(7302), pathBad.c_str()));
    harken::StreamManager::StreamFacts facts;
    ASSERT_TRUE(engine->streams.FindFile(harkentest::ObjectId(7302), &facts));
    EXPECT_EQ(kRate, facts.frameCount);

    // Class discipline + never-register: a PCM id is not a stream.
    RegisterDc(harkentest::ObjectId(103), 0.25f, kBlock);
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_ReplaceStreamFile(harkentest::ObjectId(103), pathX.c_str()));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_ReplaceStreamFile(harkentest::ObjectId(7999), pathX.c_str()));
}

// --- attenuation: live voice re-curves on the next tick (golden) ---

TEST(HotReload, AttenuationReplaceRecurvesLiveVoiceNextTick) {
    EngineSession session(ReloadConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterDc(harkentest::ObjectId(104), 0.5f, 8 * kBlock);
    // Linear rolloff over [1, 101]: at d = 51 the gain is exactly 0.5.
    const VnAttenuationDesc oldProfile =
        MakeAttenuationDesc(1.0f, 101.0f, VN_ROLLOFF_LINEAR);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(9), &oldProfile));

    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(104), 0.0f, 0.0f, -51.0f);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(voice, harkentest::ObjectId(9)));
    ASSERT_EQ(VN_OK, Vn_Update());
    Capture before;
    PumpAndCapture(engine, 2, before);
    HARKENTEST_ASSERT_NON_SILENT(before.left);
    EXPECT_NEAR(0.5f * 0.5f, before.left[kBlock + kBlock / 2],
                1e-4f);

    // Replace with linear rolloff over [1, 201]: at d = 51 the gain becomes
    // 1 - 50/200 = 0.75. The live voice re-evaluates on the next update —
    // no rebind, the parameter system's normal dirty path.
    const VnAttenuationDesc newProfile =
        MakeAttenuationDesc(1.0f, 201.0f, VN_ROLLOFF_LINEAR);
    ASSERT_EQ(VN_OK, Vn_ReplaceAttenuation(harkentest::ObjectId(9), &newProfile));
    ASSERT_EQ(VN_OK, Vn_Update());
    Capture after;
    PumpAndCapture(engine, 2, after);
    // The 10 ms spatial-volume ramp rounds up to two 256-frame blocks at
    // 48 kHz, and its final sample lands exactly on the replacement target.
    EXPECT_NEAR(0.5f * 0.75f, after.left[2 * kBlock - 1],
                1e-4f);

    // All-or-nothing: invalid descriptors change nothing; the voice keeps
    // the last valid curves. Unknown id never registers.
    VnAttenuationDesc emptySpan = newProfile;
    emptySpan.minDistance = 5.0f;
    emptySpan.maxDistance = 5.0f;
    EXPECT_EQ(VN_INVALID_ARG, Vn_ReplaceAttenuation(harkentest::ObjectId(9), &emptySpan));
    ASSERT_EQ(VN_OK, Vn_Update());
    Capture keptAfterEmptySpan;
    PumpAndCapture(engine, 1, keptAfterEmptySpan);
    EXPECT_NEAR(0.5f * 0.75f,
                keptAfterEmptySpan.left[kBlock / 2], 1e-4f);

    VnAttenuationDesc bad = newProfile;
    bad.volumeCurve.enabled = 1;
    bad.volumeCurve.pointCount = 2;
    bad.volumeCurve.points[0] = {1.0f, 0.0f};
    bad.volumeCurve.points[1] = {201.0f, -12.0f};
    bad.volumeCurve.handles[0] = harkentest::EndpointHandles(
        bad.volumeCurve.points[0], bad.volumeCurve.points[1]);
    bad.volumeCurve.handles[0].h1y = -97.0f;
    EXPECT_EQ(VN_INVALID_ARG, Vn_ReplaceAttenuation(harkentest::ObjectId(9), &bad));
    ASSERT_EQ(VN_OK, Vn_Update());
    Capture keptAfterInvalidHandle;
    PumpAndCapture(engine, 1, keptAfterInvalidHandle);
    EXPECT_NEAR(0.5f * 0.75f,
                keptAfterInvalidHandle.left[kBlock / 2], 1e-4f);
    const VnAttenuationDesc fresh = MakeAttenuationDesc(1.0f, 101.0f);
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_ReplaceAttenuation(harkentest::ObjectId(77), &fresh));
    EXPECT_FALSE(engine->spatial.HasProfile(harkentest::ObjectId(77)));
}

// --- container: new children next resolution, benign pair reset ---


// --- parked voice returns on the generation it started on ---

TEST(HotReload, ParkedVoiceReturnsOnItsOriginalGeneration) {
    VnConfig config = ReloadConfig(/*maxVoices=*/4);
    config.maxParkedVoices = 4;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    RegisterDc(harkentest::ObjectId(105), 0.25f, kRate);
    const VnAttenuationDesc far = MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(harkentest::ObjectId(9), &far));

    // Park: voice at (100,0,0), listener at the origin -> distance 100 ->
    // the linear-rolloff floor, far below the threshold.
    VnPlayDesc play = MakePlayDesc3D(harkentest::ObjectId(105), 100.0f, 0.0f, 0.0f);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(voice, harkentest::ObjectId(9)));
    ASSERT_EQ(VN_OK, Vn_SetListener(Vec3(0.0f, 0.0f, 0.0f),
                                    Vec3(0.0f, 0.0f, -1.0f),
                                    Vec3(0.0f, 1.0f, 0.0f)));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(1u, Snap().parkedVoiceCount);

    // Replace while parked: the park's pin makes the old generation retire
    // rather than vanish.
    ASSERT_EQ(VN_OK, ReplaceDc(harkentest::ObjectId(105), 0.5f, kRate));
    EXPECT_EQ(1u, Snap().pinnedGenerationCount);

    // Return: listener moves next to the voice (distance 1 -> full gain).
    ASSERT_EQ(VN_OK, Vn_SetListener(Vec3(99.0f, 0.0f, 0.0f),
                                    Vec3(0.0f, 0.0f, -1.0f),
                                    Vec3(0.0f, 1.0f, 0.0f)));
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(0u, Snap().parkedVoiceCount);

    // The first returned block sounds the OLD samples at full gain, hard right.
    // Center-unity normalization scales old PCM by sqrt(2); replacement PCM
    // would be twice this level.
    Capture captured;
    PumpAndCapture(engine, 1, captured);
    HARKENTEST_ASSERT_NON_SILENT(captured.right);
    const float expected = 0.25f * std::sqrt(2.0f);
    for (std::uint32_t frame = 0; frame < kBlock; ++frame) {
        EXPECT_NEAR(0.0f, captured.left[frame], 1e-6f) << frame;
        EXPECT_NEAR(expected, captured.right[frame], 1e-3f) << frame;
    }

    // A fresh play resolves the new generation.
    ASSERT_EQ(VN_OK, Vn_Stop(voice, 0));
    PumpAndUpdate(engine, 1);
    VnPlayDesc fresh2d = MakePlayDesc(harkentest::ObjectId(105));
    VnVoiceHandle fresh = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&fresh2d, &fresh));
    Capture renewed;
    PumpAndCapture(engine, 1, renewed);
    // A 2D mono play passes through at unity: the channel value IS the new
    // amplitude.
    EXPECT_NEAR(0.5f, renewed.left[kBlock / 2], 1e-4f);
}

// --- counters and log records ---

TEST(HotReload, CountersAndLogRecordsBookEveryOutcome) {
    LogSink sink;
    VnConfig config = ReloadConfig();
    config.logCallback = &LogCb;
    config.logUserData = &sink;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    RegisterDc(harkentest::ObjectId(106), 0.25f, kBlock);
    ASSERT_EQ(VN_OK, ReplaceDc(harkentest::ObjectId(106), 0.5f, kBlock));        // applied
    EXPECT_EQ(VN_UNKNOWN_ID, ReplaceDc(harkentest::ObjectId(998), 0.5f, kBlock)); // refused

    const VnSnapshot snapshot = Snap();
    EXPECT_EQ(1u, snapshot.hotReloadCount);
    EXPECT_EQ(1u, snapshot.hotReloadRefusedCount);
    EXPECT_EQ(0u, snapshot.pinnedGenerationCount); // nothing was playing
    EXPECT_EQ(0u, snapshot.pinnedGenerationHighWater);

    ASSERT_EQ(VN_OK, Vn_Update()); // drain the logic log ring to the sink
    EXPECT_EQ(1u, sink.Count(harken::kLogCodeHotReloadApplied));
    EXPECT_EQ(1u, sink.Count(harken::kLogCodeHotReloadRefused));
}

// --- binding: the live-logic-map verb shares the bottleneck accounting ---

TEST(HotReload, BindingReplaceBooksEveryOutcome) {
    LogSink sink;
    VnConfig config = ReloadConfig();
    config.logCallback = &LogCb;
    config.logUserData = &sink;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    VnBindingDesc desc{};
    desc.structSize = sizeof(VnBindingDesc);
    desc.variableId = harkentest::ObjectId(7);
    desc.targetScopeKind = VN_TARGET_SCOPE_VOICE;
    desc.targetId = 0; // voice volume
    desc.curve.structSize = sizeof(VnCurveDesc);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {0.0f, 0.0f};
    desc.curve.points[1] = {1.0f, -6.0f};
    desc.curve.handles[0] = harkentest::EndpointHandles(
        desc.curve.points[0], desc.curve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(300), &desc));

    // Applied: a legal re-curve. Refused twice: an unknown id and a
    // non-canonical fixed-capacity handle tail. Every outcome path books,
    // like the six sibling verbs.
    VnBindingDesc recurve = desc;
    recurve.curve.points[1].y = -24.0f;
    recurve.curve.handles[0] = harkentest::EndpointHandles(
        recurve.curve.points[0], recurve.curve.points[1]);
    EXPECT_EQ(VN_OK, Vn_ReplaceBinding(harkentest::ObjectId(300), &recurve));
    const harken::BindingTable::Definition* live = engine->bindings.Find(harkentest::ObjectId(300));
    ASSERT_NE(nullptr, live);
    EXPECT_FLOAT_EQ(-3.0f, live->curve.Eval(0.5f));
    ASSERT_EQ(VN_OK, Vn_Update());
    live = engine->bindings.Find(harkentest::ObjectId(300));
    ASSERT_NE(nullptr, live);
    EXPECT_FLOAT_EQ(-12.0f, live->curve.Eval(0.5f));

    EXPECT_EQ(VN_UNKNOWN_ID, Vn_ReplaceBinding(harkentest::ObjectId(999), &desc));
    VnBindingDesc badTail = desc;
    badTail.curve.handles[1].h1x = -0.0f;
    EXPECT_EQ(VN_INVALID_ARG, Vn_ReplaceBinding(harkentest::ObjectId(300), &badTail));
    ASSERT_EQ(VN_OK, Vn_Update());
    live = engine->bindings.Find(harkentest::ObjectId(300));
    ASSERT_NE(nullptr, live);
    EXPECT_FLOAT_EQ(-12.0f, live->curve.Eval(0.5f));

    const VnSnapshot snapshot = Snap();
    EXPECT_EQ(1u, snapshot.hotReloadCount);
    EXPECT_EQ(2u, snapshot.hotReloadRefusedCount);

    // Vn_Update drains the logic ring into the callback sink. Filter the
    // drained records by the encoded hot-reload class and asset id.
    EXPECT_EQ(1u, sink.Count(harken::kLogCodeHotReloadApplied,
                             harken::kHotReloadClassBinding, harkentest::ObjectId(300u)));
    EXPECT_EQ(1u, sink.Count(harken::kLogCodeHotReloadRefused,
                             harken::kHotReloadClassBinding, harkentest::ObjectId(300u)));
    EXPECT_EQ(1u, sink.Count(harken::kLogCodeHotReloadRefused,
                             harken::kHotReloadClassBinding, harkentest::ObjectId(999u)));
}

} // namespace
