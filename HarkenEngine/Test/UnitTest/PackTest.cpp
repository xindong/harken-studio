// Pack load/unload behavior: bulk registration and removal, loop
// metadata carry-through, the VN_IN_USE refusal while an entry's voice
// plays, generational stale-handle benignity with its counter, capacity
// exhaustion (VN_NO_PACK), and the refusal matrix for corrupted or
// malformed containers — every refusal leaves the registry untouched and
// never crashes.
//
// Packs for these cases are assembled in-memory by a test-local builder
// over the container format helpers and written to .temp/; the corruption
// cases then patch specific bytes. The real compiler tool is exercised by
// the round-trip golden case, not here.
#include "Common/ObjectIdTestSupport.h"
#include "Common/PackFoundationFixture.h"
#include "Common/TestUtil.h"

#include "harken/media/PackFormat.h"
#include "harken/media/PackTable.h"

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the repository's .temp directory"
#endif

namespace {

using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlockFrames = 256;

std::string TempPath(const std::string& name) {
    return std::string(HARKEN_TEMP_DIR "/") + name;
}

void WriteFileBytes(const std::string& path,
                    const std::vector<std::uint8_t>& bytes) {
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    ASSERT_TRUE(out.good()) << "cannot open " << path;
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    ASSERT_TRUE(out.good()) << "cannot write " << path;
}

VnResult LoadPack(const std::string& path, VnPackHandle* outHandle) {
    VnPackDesc desc{};
    desc.structSize = sizeof(VnPackDesc);
    desc.utf8Path = path.c_str();
    return Vn_LoadPack(&desc, outHandle);
}

// --- test-local pack builder ---

struct PackEntrySpec {
    VnSoundId id = harkentest::ObjectId(0);
    std::uint32_t sampleRate = 48000;
    std::uint16_t channels = 1;
    std::vector<float> planar; // channel planes concatenated
    bool loopable = false;
    std::uint64_t loopStart = 0;
    std::uint64_t loopEnd = 0; // 0 = whole file
};

std::uint64_t AlignUp64(std::uint64_t value) {
    const std::uint64_t align = harken::kPackBlobAlign;
    return (value + align - 1) / align * align;
}

// Serializes a well-formed v1 container: header, TOC at offset 36, blobs
// 64-byte aligned in entry order, zeroed padding, valid checksums. The
// tampering tests start from these bytes.
std::vector<std::uint8_t> BuildPackBytes(
    const std::vector<PackEntrySpec>& specs) {
    const std::uint64_t tocOffset = harken::kPackHeaderBytes;
    const std::uint64_t tocBytes =
        static_cast<std::uint64_t>(specs.size()) *
        harken::kPackTocEntryBytes;

    std::vector<std::uint64_t> blobOffsets(specs.size());
    std::uint64_t cursor = AlignUp64(tocOffset + tocBytes);
    std::uint64_t fileBytes = tocOffset + tocBytes;
    for (std::size_t i = 0; i < specs.size(); ++i) {
        blobOffsets[i] = cursor;
        const std::uint64_t blobBytes =
            static_cast<std::uint64_t>(specs[i].planar.size()) *
            sizeof(float);
        fileBytes = cursor + blobBytes;
        cursor = AlignUp64(fileBytes);
    }

    std::vector<std::uint8_t> bytes(static_cast<std::size_t>(fileBytes),
                                    0);
    std::vector<std::uint8_t> toc(static_cast<std::size_t>(tocBytes), 0);
    for (std::size_t i = 0; i < specs.size(); ++i) {
        const PackEntrySpec& spec = specs[i];
        const std::uint64_t blobBytes =
            static_cast<std::uint64_t>(spec.planar.size()) *
            sizeof(float);
        std::memcpy(bytes.data() + blobOffsets[i], spec.planar.data(),
                    static_cast<std::size_t>(blobBytes));

        harken::PackTocEntry entry;
        entry.soundId = spec.id;
        entry.sampleRate = spec.sampleRate;
        entry.channelCount = spec.channels;
        entry.sampleFormat = harken::kPackSampleFormatF32Planar;
        entry.frameCount = spec.planar.size() / spec.channels;
        entry.blobOffset = blobOffsets[i];
        entry.blobBytes = blobBytes;
        entry.blobChecksum =
            harken::Fnv1a64(bytes.data() + blobOffsets[i],
                           static_cast<std::size_t>(blobBytes));
        entry.flags = spec.loopable ? harken::kPackFlagLoopable : 0;
        entry.loopStartFrame = spec.loopable ? spec.loopStart : 0;
        entry.loopEndFrame = spec.loopable ? spec.loopEnd : 0;
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

// Serialized header field offsets, from the container format definition.
constexpr std::size_t kHeaderEntryCountOffset = 20;
constexpr std::size_t kHeaderTocOffsetOffset = 24;
constexpr std::size_t kHeaderTocChecksumOffset = 28;
// Field offsets inside one serialized TOC entry.
constexpr std::size_t kTocEntryBlobOffsetOffset = 24;

// Recomputes the header's TOC checksum after a deliberate TOC edit, so a
// tampering test exercises the validation step it aims at instead of
// tripping the checksum first.
void FixTocChecksum(std::vector<std::uint8_t>* bytes,
                    std::uint32_t entryCount) {
    const std::size_t tocOffset = harken::kPackHeaderBytes;
    const std::size_t tocBytes =
        static_cast<std::size_t>(entryCount) *
        harken::kPackTocEntryBytes;
    harken::StoreLE64(bytes->data() + kHeaderTocChecksumOffset,
                     harken::Fnv1a64(bytes->data() + tocOffset, tocBytes));
}

PackEntrySpec MakeSineEntry(VnSoundId id, std::size_t frames) {
    PackEntrySpec spec;
    spec.id = id;
    spec.planar = harkentest::MakeSine(frames, 1000.0, 48000.0, 0.8f);
    return spec;
}

// --- lifecycle ---

TEST(Pack, LoadRegistersEntriesAndUnloadRemovesThem) {
    EngineSession session(MakeNullConfig(0, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::string path = TempPath("pack_basic.pack");
    for (const auto version : {3u, 4u}) {
        auto bytes = BuildPackBytes({MakeSineEntry(harkentest::ObjectId(11), 64),
                                    MakeSineEntry(harkentest::ObjectId(12), 128)});
        harken::StoreLE32(bytes.data() + 4, version);
        WriteFileBytes(path, bytes);
        VnPackHandle pack = 0;
        ASSERT_EQ(VN_OK, LoadPack(path, &pack)) << version;
        EXPECT_NE(0u, pack);
        EXPECT_EQ(1u, session.Engine()->packs.LoadedCount());
        EXPECT_EQ(2u, session.Engine()->registry.Count());
        EXPECT_TRUE(session.Engine()->registry.Lookup(harkentest::ObjectId(11)).Valid());
        EXPECT_TRUE(session.Engine()->registry.Lookup(harkentest::ObjectId(12)).Valid());
        ASSERT_EQ(VN_OK, Vn_UnloadPack(pack));
        EXPECT_EQ(0u, session.Engine()->packs.LoadedCount());
        EXPECT_EQ(0u, session.Engine()->registry.Count());
        EXPECT_FALSE(session.Engine()->registry.Lookup(harkentest::ObjectId(11)).Valid());
        EXPECT_FALSE(session.Engine()->registry.Lookup(harkentest::ObjectId(12)).Valid());
    }
}

TEST(Pack, LoopMetadataCarriedThroughLoad) {
    EngineSession session(MakeNullConfig(0, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    PackEntrySpec looped = MakeSineEntry(harkentest::ObjectId(21), 256);
    looped.loopable = true;
    looped.loopStart = 10;
    looped.loopEnd = 100;
    const std::string path = TempPath("pack_loopmeta.pack");
    WriteFileBytes(path, BuildPackBytes({looped, MakeSineEntry(harkentest::ObjectId(22), 64)}));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(path, &pack));

    const auto* entries = session.Engine()->packs.Entries(pack);
    ASSERT_NE(nullptr, entries);
    ASSERT_EQ(2u, entries->size());
    EXPECT_EQ(harkentest::ObjectId(21u), (*entries)[0].soundId);
    EXPECT_EQ(harken::kPackFlagLoopable, (*entries)[0].flags);
    EXPECT_EQ(10u, (*entries)[0].loopStartFrame);
    EXPECT_EQ(100u, (*entries)[0].loopEndFrame);
    EXPECT_EQ(256u, (*entries)[0].frameCount);
    EXPECT_EQ(0u, (*entries)[1].flags);

    const harken::PackEntry* bySound =
        session.Engine()->packs.FindEntryBySound(
            harkentest::ObjectId(21), session.Engine()->registry);
    ASSERT_NE(nullptr, bySound);
    EXPECT_EQ(10u, bySound->loopStartFrame);
    EXPECT_EQ(nullptr, session.Engine()->packs.FindEntryBySound(
                           harkentest::ObjectId(999), session.Engine()->registry));
}

TEST(Pack, UnloadRefusedWhileEntryVoicePlays) {
    EngineSession session(MakeNullConfig(0, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    // Two blocks of material so the voice is still live after one pump.
    PackEntrySpec spec = MakeSineEntry(harkentest::ObjectId(31), kBlockFrames * 2);
    HARKENTEST_ASSERT_NON_SILENT(spec.planar);
    const std::string path = TempPath("pack_inuse.pack");
    WriteFileBytes(path, BuildPackBytes({spec}));

    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(path, &pack));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(31));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    session.Engine()->nullBackend.Pump(1);

    EXPECT_EQ(VN_IN_USE, Vn_UnloadPack(pack));
    // The refusal is all-or-nothing: the entry is still registered.
    EXPECT_TRUE(session.Engine()->registry.Lookup(harkentest::ObjectId(31)).Valid());

    // Let the voice finish; the refusal lifts.
    session.Engine()->nullBackend.Pump(2);
    EXPECT_EQ(nullptr, session.Engine()->voices.Resolve(harkentest::RenderHandle(
        session.Engine(), voice)));
    EXPECT_EQ(VN_OK, Vn_UnloadPack(pack));
    EXPECT_FALSE(session.Engine()->registry.Lookup(harkentest::ObjectId(31)).Valid());
}

TEST(Pack, UnloadSkipsEntryReRegisteredByCaller) {
    EngineSession session(MakeNullConfig(0, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::string path = TempPath("pack_reregister.pack");
    WriteFileBytes(path, BuildPackBytes({MakeSineEntry(harkentest::ObjectId(42), kBlockFrames * 2),
                                         MakeSineEntry(harkentest::ObjectId(43), 64)}));
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(path, &pack));

    // The caller takes id 42 over: individual unregister, then their own
    // registration under the same id. From here 42 is not the pack's.
    ASSERT_EQ(VN_OK, Vn_UnregisterPCM(harkentest::ObjectId(42)));
    const std::vector<float> callerSound =
        harkentest::MakeSine(kBlockFrames * 2, 500.0, 48000.0, 0.7f);
    HARKENTEST_ASSERT_NON_SILENT(callerSound);
    const VnPCMDesc callerDesc = MonoDesc(callerSound, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(42), &callerDesc));

    // A live voice on the caller's 42 must NOT gate the pack's unload:
    // the in-use check only weighs entries the pack still owns.
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(42));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    session.Engine()->nullBackend.Pump(1);
    ASSERT_NE(nullptr, session.Engine()->voices.Resolve(harkentest::RenderHandle(
        session.Engine(), voice)));

    EXPECT_EQ(VN_OK, Vn_UnloadPack(pack));
    // The caller's registration survives untouched — with its voice still
    // playing — while the pack-owned entry 43 is gone.
    EXPECT_TRUE(session.Engine()->registry.Lookup(harkentest::ObjectId(42)).Valid());
    EXPECT_FALSE(session.Engine()->registry.Lookup(harkentest::ObjectId(43)).Valid());
    EXPECT_EQ(1u, session.Engine()->registry.Count());
    EXPECT_NE(nullptr, session.Engine()->voices.Resolve(harkentest::RenderHandle(
        session.Engine(), voice)));
    EXPECT_EQ(0u, session.Engine()->packs.LoadedCount());
}

TEST(Pack, UnloadLeavesIdleReRegisteredEntryUntouched) {
    // The discriminating half of the ownership contract: with no voice
    // playing, nothing about liveness can save the caller's registration
    // — only the unregister loop's cookie gate keeps unload's bulk
    // removal off an id the caller took over while the pack was loaded.
    EngineSession session(MakeNullConfig(0, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::string path = TempPath("pack_idletakeover.pack");
    WriteFileBytes(path, BuildPackBytes({MakeSineEntry(harkentest::ObjectId(42), 64),
                                         MakeSineEntry(harkentest::ObjectId(43), 64)}));
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(path, &pack));

    // The caller takes id 42 over with content of their own and never
    // plays it — the entry sits idle through the unload.
    ASSERT_EQ(VN_OK, Vn_UnregisterPCM(harkentest::ObjectId(42)));
    const std::vector<float> callerSound = harkentest::MakeDC(32, 0.5f);
    const VnPCMDesc callerDesc = MonoDesc(callerSound, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(42), &callerDesc));
    const std::uint64_t callerCookie =
        session.Engine()->registry.CookieOf(harkentest::ObjectId(42));
    ASSERT_NE(0u, callerCookie);

    EXPECT_EQ(VN_OK, Vn_UnloadPack(pack));
    // The caller's idle registration survives as the SAME registration
    // (cookie unchanged, not merely some entry under the id); the
    // pack-owned 43 is gone.
    EXPECT_TRUE(session.Engine()->registry.Lookup(harkentest::ObjectId(42)).Valid());
    EXPECT_EQ(callerCookie, session.Engine()->registry.CookieOf(harkentest::ObjectId(42)));
    EXPECT_FALSE(session.Engine()->registry.Lookup(harkentest::ObjectId(43)).Valid());
    EXPECT_EQ(1u, session.Engine()->registry.Count());
    EXPECT_EQ(0u, session.Engine()->packs.LoadedCount());
}

TEST(Pack, FindEntryBySoundStopsServingReRegisteredId) {
    EngineSession session(MakeNullConfig(0, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    PackEntrySpec looped = MakeSineEntry(harkentest::ObjectId(42), 256);
    looped.loopable = true;
    looped.loopStart = 8;
    looped.loopEnd = 200;
    const std::string path = TempPath("pack_metastale.pack");
    WriteFileBytes(path, BuildPackBytes({looped}));
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(path, &pack));

    // While the pack owns the id, its loop metadata serves.
    ASSERT_NE(nullptr, session.Engine()->packs.FindEntryBySound(
                           harkentest::ObjectId(42), session.Engine()->registry));

    // Unregistered: the pack record still exists but no longer describes
    // any live registration — a miss, not stale metadata.
    ASSERT_EQ(VN_OK, Vn_UnregisterPCM(harkentest::ObjectId(42)));
    EXPECT_EQ(nullptr, session.Engine()->packs.FindEntryBySound(
                           harkentest::ObjectId(42), session.Engine()->registry));

    // Re-registered by the caller with content of their own: the pack's
    // loop facts do not apply to it and must stay unserved.
    const std::vector<float> callerSound = harkentest::MakeDC(32, 0.5f);
    const VnPCMDesc callerDesc = MonoDesc(callerSound, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(42), &callerDesc));
    EXPECT_EQ(nullptr, session.Engine()->packs.FindEntryBySound(
                           harkentest::ObjectId(42), session.Engine()->registry));

    EXPECT_EQ(VN_OK, Vn_UnloadPack(pack));
    EXPECT_TRUE(session.Engine()->registry.Lookup(harkentest::ObjectId(42)).Valid());
    EXPECT_EQ(VN_OK, Vn_UnregisterPCM(harkentest::ObjectId(42)));
}

TEST(Pack, StalePackHandleBenignAndCounted) {
    EngineSession session(MakeNullConfig(0, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::string path = TempPath("pack_stale.pack");
    WriteFileBytes(path, BuildPackBytes({MakeSineEntry(harkentest::ObjectId(41), 64)}));

    VnPackHandle first = 0;
    ASSERT_EQ(VN_OK, LoadPack(path, &first));
    ASSERT_EQ(VN_OK, Vn_UnloadPack(first));

    // The handle died with the unload; every further use is a benign
    // no-op with the dedicated counter ticking.
    EXPECT_EQ(VN_STALE_HANDLE, Vn_UnloadPack(first));
    EXPECT_EQ(1u, session.Engine()->packs.StaleHandleCount());
    EXPECT_EQ(VN_STALE_HANDLE, Vn_UnloadPack(0));
    EXPECT_EQ(2u, session.Engine()->packs.StaleHandleCount());

    // Slot reuse mints a new generation: the old handle stays stale while
    // the new one works.
    VnPackHandle second = 0;
    ASSERT_EQ(VN_OK, LoadPack(path, &second));
    EXPECT_NE(first, second);
    EXPECT_EQ(VN_STALE_HANDLE, Vn_UnloadPack(first));
    EXPECT_EQ(3u, session.Engine()->packs.StaleHandleCount());
    EXPECT_EQ(nullptr, session.Engine()->packs.Entries(first));
    EXPECT_EQ(VN_OK, Vn_UnloadPack(second));
}

TEST(Pack, LoadArgumentValidation) {
    EngineSession session(MakeNullConfig(0, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    VnPackHandle pack = 0;
    EXPECT_EQ(VN_INVALID_ARG, Vn_LoadPack(nullptr, &pack));

    VnPackDesc desc{};
    desc.structSize = sizeof(VnPackDesc);
    desc.utf8Path = "irrelevant";
    EXPECT_EQ(VN_INVALID_ARG, Vn_LoadPack(&desc, nullptr));

    desc.structSize = 0;
    EXPECT_EQ(VN_INVALID_ARG, Vn_LoadPack(&desc, &pack));

    desc.structSize = sizeof(VnPackDesc);
    desc.utf8Path = nullptr;
    EXPECT_EQ(VN_INVALID_ARG, Vn_LoadPack(&desc, &pack));

    // A missing file is a bad pack, not a bad argument.
    const std::string missing = TempPath("pack_missing.pack");
    std::filesystem::remove(missing);
    EXPECT_EQ(VN_BAD_PACK, LoadPack(missing, &pack));
}

TEST(Pack, HeaderDispatchRequiresCurrentSchemaAndKnownLayout) {
    EngineSession session(MakeNullConfig(0, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    const auto entry = MakeSineEntry(harkentest::ObjectId(61), 64);
    harkentest::packfoundation::ImageBuilder sectioned(harken::kPackKindContent);
    sectioned.AddMedia(entry.id, entry.planar);
    const std::vector<std::vector<std::uint8_t>> layouts = {
        BuildPackBytes({entry}), sectioned.Build()};
    const auto path = TempPath("pack_header_dispatch.pack");

    for (const auto& valid : layouts) {
        for (const auto version : {HARKEN_PACK_SCHEMA_VERSION - 1,
                                   HARKEN_PACK_SCHEMA_VERSION + 1}) {
            auto bytes = valid;
            harken::StoreLE32(bytes.data() + 4, version);
            WriteFileBytes(path, bytes);
            VnPackHandle pack = 0;
            EXPECT_EQ(VN_BAD_PACK, LoadPack(path, &pack));
            EXPECT_EQ(0u, pack);
            EXPECT_EQ(0u, session.Engine()->packs.LoadedCount());
            EXPECT_EQ(0u, session.Engine()->registry.Count());
        }
        const auto otherLayout =
            harken::LoadLE32(valid.data() + 8) == harken::kPackHeaderBytes
                ? harken::kPackV2HeaderBytes : harken::kPackHeaderBytes;
        for (const auto headerBytes : {0u, otherLayout}) {
            auto bytes = valid;
            harken::StoreLE32(bytes.data() + 8, headerBytes);
            WriteFileBytes(path, bytes);
            VnPackHandle pack = 0;
            EXPECT_EQ(VN_BAD_PACK, LoadPack(path, &pack));
            EXPECT_EQ(0u, pack);
            EXPECT_EQ(0u, session.Engine()->packs.LoadedCount());
            EXPECT_EQ(0u, session.Engine()->registry.Count());
        }
        WriteFileBytes(path, valid);
        VnPackHandle pack = 0;
        ASSERT_EQ(VN_OK, LoadPack(path, &pack));
        EXPECT_TRUE(session.Engine()->registry.Lookup(entry.id).Valid());
        ASSERT_EQ(VN_OK, Vn_UnloadPack(pack));
        EXPECT_EQ(0u, session.Engine()->registry.Count());
    }
}

// --- capacity ---

TEST(Pack, CapacityExhaustionRefusedWithNoPack) {
    EngineSession session(MakeNullConfig(0, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    // One tiny pack file per slot, each with a distinct entry id so the
    // registrations never collide. Capacity is the default-resolved
    // config value (maxPacks 0 in the public struct).
    const std::uint32_t capacity = session.Engine()->config.maxPacks;
    std::vector<VnPackHandle> handles(capacity, 0);
    for (std::uint32_t i = 0; i < capacity; ++i) {
        const std::string path = TempPath(
            "pack_cap_" + std::to_string(i) + ".pack");
        WriteFileBytes(path, BuildPackBytes({MakeSineEntry(harkentest::ObjectId(1000 + i), 64)}));
        ASSERT_EQ(VN_OK, LoadPack(path, &handles[i])) << "pack " << i;
    }
    EXPECT_EQ(capacity, session.Engine()->packs.LoadedCount());

    const std::string overflowPath =
        TempPath("pack_cap_overflow.pack");
    WriteFileBytes(overflowPath, BuildPackBytes({MakeSineEntry(harkentest::ObjectId(2000), 64)}));
    VnPackHandle overflow = 0;
    EXPECT_EQ(VN_NO_PACK, LoadPack(overflowPath, &overflow));
    // The refusal has no side effects: nothing registered, nothing loaded.
    EXPECT_FALSE(session.Engine()->registry.Lookup(harkentest::ObjectId(2000)).Valid());

    // Freeing one slot makes the load succeed.
    ASSERT_EQ(VN_OK, Vn_UnloadPack(handles[0]));
    EXPECT_EQ(VN_OK, LoadPack(overflowPath, &overflow));
    EXPECT_TRUE(session.Engine()->registry.Lookup(harkentest::ObjectId(2000)).Valid());
}

// --- corruption and malformed containers ---
// Every case pre-registers an unrelated sound so "registry untouched"
// is observable as an exact count and a surviving lookup.

class PackCorruption : public ::testing::Test {
protected:
    void SetUp() override {
        m_session = std::make_unique<EngineSession>(
            MakeNullConfig(0, kBlockFrames));
        ASSERT_EQ(VN_OK, m_session->InitResult());
        m_unrelated = harkentest::MakeDC(16, 0.5f);
        const VnPCMDesc desc = MonoDesc(m_unrelated, 48000);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(777), &desc));
    }

    // Loads the given bytes as a pack file and asserts the refusal left
    // the registry exactly as it was.
    void ExpectRefusal(const std::string& name,
                       const std::vector<std::uint8_t>& bytes,
                       VnResult expected) {
        const std::string path = TempPath(name);
        WriteFileBytes(path, bytes);
        VnPackHandle pack = 0;
        EXPECT_EQ(expected, LoadPack(path, &pack)) << name;
        EXPECT_EQ(1u, m_session->Engine()->registry.Count()) << name;
        EXPECT_TRUE(m_session->Engine()->registry.Lookup(harkentest::ObjectId(777)).Valid())
            << name;
        EXPECT_FALSE(m_session->Engine()->registry.Lookup(harkentest::ObjectId(51)).Valid())
            << name;
        EXPECT_FALSE(m_session->Engine()->registry.Lookup(harkentest::ObjectId(52)).Valid())
            << name;
        EXPECT_EQ(0u, m_session->Engine()->packs.LoadedCount()) << name;
    }

    std::unique_ptr<EngineSession> m_session;
    std::vector<float> m_unrelated;
};

std::vector<std::uint8_t> TwoEntryPackBytes() {
    return BuildPackBytes(
        {MakeSineEntry(harkentest::ObjectId(51), 128), MakeSineEntry(harkentest::ObjectId(52), 96)});
}

TEST_F(PackCorruption, FlippedBlobByteRefused) {
    std::vector<std::uint8_t> bytes = TwoEntryPackBytes();
    // First blob starts at the first 64-byte boundary after the TOC.
    const std::size_t blobStart = static_cast<std::size_t>(AlignUp64(
        harken::kPackHeaderBytes +
        2ull * harken::kPackTocEntryBytes));
    bytes[blobStart + 5] ^= 0x01;
    ExpectRefusal("pack_blobflip.pack", bytes, VN_BAD_PACK);
}

TEST_F(PackCorruption, FlippedTocByteRefused) {
    std::vector<std::uint8_t> bytes = TwoEntryPackBytes();
    // A soundId byte inside the second TOC entry; the TOC checksum must
    // catch it.
    bytes[harken::kPackHeaderBytes + harken::kPackTocEntryBytes] ^=
        0x01;
    ExpectRefusal("pack_tocflip.pack", bytes, VN_BAD_PACK);
}

TEST_F(PackCorruption, TruncatedFileRefused) {
    std::vector<std::uint8_t> bytes = TwoEntryPackBytes();
    std::vector<std::uint8_t> shortened(bytes.begin(), bytes.end() - 10);
    ExpectRefusal("pack_truncated.pack", shortened, VN_BAD_PACK);

    std::vector<std::uint8_t> headerless(bytes.begin(),
                                         bytes.begin() + 20);
    ExpectRefusal("pack_headerless.pack", headerless,
                  VN_BAD_PACK);
}

TEST_F(PackCorruption, OutOfBoundsTocOffsetRefused) {
    std::vector<std::uint8_t> bytes = TwoEntryPackBytes();
    // The header carries no checksum of itself, so this exercises the
    // TOC bounds check directly.
    harken::StoreLE32(bytes.data() + kHeaderTocOffsetOffset,
                     static_cast<std::uint32_t>(bytes.size()));
    ExpectRefusal("pack_tocoob.pack", bytes, VN_BAD_PACK);
}

TEST_F(PackCorruption, OutOfBoundsBlobRefused) {
    std::vector<std::uint8_t> bytes = TwoEntryPackBytes();
    // Point the first entry's blob past end-of-file (64-byte aligned so
    // the alignment rule passes) and re-seal the TOC checksum: the blob
    // bounds check itself must refuse.
    const std::uint64_t badOffset = AlignUp64(bytes.size() + 4096);
    harken::StoreLE64(bytes.data() + harken::kPackHeaderBytes +
                         kTocEntryBlobOffsetOffset,
                     badOffset);
    FixTocChecksum(&bytes, 2);
    ExpectRefusal("pack_bloboob.pack", bytes, VN_BAD_PACK);
}

TEST_F(PackCorruption, InconsistentEntryCountRefused) {
    // One more entry than the TOC holds: the declared TOC region shifts
    // and the checksum no longer matches.
    std::vector<std::uint8_t> bytes = TwoEntryPackBytes();
    harken::StoreLE32(bytes.data() + kHeaderEntryCountOffset, 3);
    ExpectRefusal("pack_countplus.pack", bytes, VN_BAD_PACK);

    // An absurd count must be refused by bounds math, not attempted.
    std::vector<std::uint8_t> absurd = TwoEntryPackBytes();
    harken::StoreLE32(absurd.data() + kHeaderEntryCountOffset, 0x10000000u);
    ExpectRefusal("pack_counthuge.pack", absurd, VN_BAD_PACK);

    // Zero entries carries no content; v1 refuses it.
    std::vector<std::uint8_t> empty = TwoEntryPackBytes();
    harken::StoreLE32(empty.data() + kHeaderEntryCountOffset, 0);
    ExpectRefusal("pack_countzero.pack", empty, VN_BAD_PACK);
}

TEST_F(PackCorruption, DuplicateIdInsidePackRefused) {
    std::vector<PackEntrySpec> specs = {MakeSineEntry(harkentest::ObjectId(51), 64),
                                        MakeSineEntry(harkentest::ObjectId(51), 96)};
    ExpectRefusal("pack_dupid.pack", BuildPackBytes(specs),
                  VN_BAD_PACK);
}

TEST_F(PackCorruption, IdCollisionRollsBackWholeLoad) {
    // Entry 51 registers cleanly, then 777 collides with the pre-existing
    // registration: the whole load unwinds, 51 included.
    std::vector<PackEntrySpec> specs = {MakeSineEntry(harkentest::ObjectId(51), 64),
                                        MakeSineEntry(harkentest::ObjectId(777), 96)};
    ExpectRefusal("pack_collision.pack", BuildPackBytes(specs),
                  VN_IN_USE);
}

// --- illegal UTF-8 paths ---
// The public path contract is UTF-8; a byte sequence that is not valid
// UTF-8 must refuse with VN_BAD_PACK rather than escaping the path
// conversion out of the C ABI (never undefined behavior). Each literal
// terminates its hex escape at the quote so the following ASCII is a
// separate token.
constexpr const char* kIllegalUtf8Paths[] = {
    "\xFF\xFE\x80" "pack",   // bytes that are never UTF-8 lead bytes
    "\x80" "orphan",         // an isolated continuation byte, no lead
    "\xC0\xAF" "overlong",   // overlong two-byte encoding of '/'
};

TEST(Pack, IllegalUtf8PathRefusedWithBadPack) {
    EngineSession session(MakeNullConfig(0, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    VnPackHandle pack = 0;
    for (const char* illegal : kIllegalUtf8Paths) {
        EXPECT_EQ(VN_BAD_PACK, LoadPack(illegal, &pack)) << illegal;
    }
    EXPECT_EQ(0u, session.Engine()->packs.LoadedCount());

    // The engine outlived every refusal: a well-formed pack still loads and
    // registers its entry.
    const std::string path = TempPath("pack_after_illegal.pack");
    WriteFileBytes(path, BuildPackBytes({MakeSineEntry(harkentest::ObjectId(61), 64)}));
    EXPECT_EQ(VN_OK, LoadPack(path, &pack));
    EXPECT_TRUE(session.Engine()->registry.Lookup(harkentest::ObjectId(61)).Valid());
}

TEST(Pack, IllegalUtf8ReplacePathRefusedOldPackKept) {
    EngineSession session(MakeNullConfig(0, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::string path = TempPath("pack_replace_base.pack");
    WriteFileBytes(path, BuildPackBytes({MakeSineEntry(harkentest::ObjectId(62), 64)}));
    VnPackHandle pack = 0;
    ASSERT_EQ(VN_OK, LoadPack(path, &pack));

    // An illegal-UTF-8 replacement path is a bad pack with the old pack
    // untouched, never an escaping conversion.
    VnPackDesc desc{};
    desc.structSize = sizeof(VnPackDesc);
    desc.utf8Path = kIllegalUtf8Paths[0];
    EXPECT_EQ(VN_BAD_PACK, Vn_ReplacePack(pack, &desc));

    // The original entry is still registered and the pack still loaded.
    EXPECT_TRUE(session.Engine()->registry.Lookup(harkentest::ObjectId(62)).Valid());
    EXPECT_EQ(1u, session.Engine()->packs.LoadedCount());
}

} // namespace
