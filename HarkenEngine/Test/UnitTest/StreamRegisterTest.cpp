// Vn_RegisterStreamFile / Vn_UnregisterStreamFile validation: the
// streaming thread performs the open and the header/TOC/checksum-table
// verification during the (blocking) call, so every refusal here is
// observable synchronously and must leave no trace behind. Also covers
// the cross-namespace exclusivity with Vn_RegisterPCM — one VnSoundId
// names one asset, resident or streamed, never both.
#include "Common/ObjectIdTestSupport.h"
#include "Common/StreamBuilder.h"
#include "Common/TestUtil.h"

#include "harken/media/StreamFormat.h"
#include "harken/media/StreamManager.h"

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the repository's .temp directory"
#endif

namespace harken::detail {
struct StreamManagerTestAccess {
    static void SetCommitPageLimit(StreamManager& manager, std::size_t pages) {
        for (auto& column : manager.m_ringStorage) column.SetCommitPageLimitForTests(pages);
    }
    static std::size_t CommittedBytes(const StreamManager& manager) {
        std::size_t bytes = 0;
        for (const auto& column : manager.m_ringStorage) bytes += column.CommittedBytes();
        return bytes;
    }
};
} // namespace harken::detail

namespace {

using harkentest::BuildStreamBytes;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::StreamSpec;
using harkentest::WriteFileBytes;

constexpr std::uint32_t kRate = 48000;

struct StreamRequestFaultGuard {
    explicit StreamRequestFaultGuard(harken::StreamRequestFault fault) {
        harken::SetStreamRequestFaultForTests(fault);
    }
    ~StreamRequestFaultGuard() {
        harken::SetStreamRequestFaultForTests(
            harken::StreamRequestFault::None);
    }
    StreamRequestFaultGuard(const StreamRequestFaultGuard&) = delete;
    StreamRequestFaultGuard& operator=(const StreamRequestFaultGuard&) =
        delete;
};

std::string TempPath(const std::string& name) {
    return std::string(HARKEN_TEMP_DIR "/") + name;
}

// Recomputes the header's tocChecksum from the (possibly patched) TOC
// entry and chunk-checksum table, so a test can alter a validated TOC
// field and prove the refusal comes from field validation rather than
// from the checksum tripping first.
void FixTocChecksum(std::vector<std::uint8_t>* bytes) {
    harken::StreamHeader header;
    ASSERT_TRUE(harken::ParseStreamHeader(bytes->data(), &header));
    const harken::StreamTocEntry entry = harken::ParseStreamTocEntry(
        bytes->data() + header.tocOffset);
    std::uint64_t checksum =
        harken::Fnv1a64(bytes->data() + header.tocOffset,
                       harken::kStreamTocEntryBytes);
    checksum = harken::Fnv1a64(
        bytes->data() + static_cast<std::size_t>(entry.chunkChecksumOffset),
        static_cast<std::size_t>(entry.chunkCount) *
            harken::kStreamChunkChecksumBytes,
        checksum);
    header.tocChecksum = checksum;
    harken::SerializeStreamHeader(header, bytes->data());
}

std::vector<std::uint8_t> MakeValidImage(VnObjectId soundId,
                                         std::size_t frames = 1000) {
    StreamSpec spec;
    spec.soundId = soundId;
    spec.sampleRate = kRate;
    spec.channelCount = 1;
    spec.chunkFrames = 256;
    const std::vector<float> tone =
        harkentest::MakeSine(frames, 500.0, kRate, 0.5f);
    return BuildStreamBytes(spec, tone);
}

TEST(StreamRegister, RegisterExposesTocFactsThroughFindFile) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& streams = session.Engine()->streams;
    using RingAccess = harken::detail::StreamManagerTestAccess;
    EXPECT_EQ(0u, RingAccess::CommittedBytes(streams));

    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(7001);
    spec.sampleRate = 44100;
    spec.channelCount = 2;
    spec.chunkFrames = 128;
    spec.loopable = true;
    spec.loopStartFrame = 100;
    spec.loopEndFrame = 700;
    std::vector<float> planar(800 * 2);
    const std::vector<float> tone =
        harkentest::MakeSine(800, 500.0, 44100.0, 0.5f);
    for (std::size_t n = 0; n < 800; ++n) {
        planar[n] = tone[n];
        planar[800 + n] = -tone[n];
    }
    const std::string path = TempPath("stream_reg_facts.stream");
    WriteFileBytes(path, BuildStreamBytes(spec, planar));

    harken::StreamManager::StreamFacts facts;
    RingAccess::SetCommitPageLimit(streams, 1);
    ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(spec.soundId, path.c_str()));
    EXPECT_EQ(0u, RingAccess::CommittedBytes(streams));
    auto play = harkentest::MakePlayDesc(spec.soundId);
    VnVoiceHandle voice = 0;
    const auto freeSlots = streams.FreeSlotCount();
    EXPECT_EQ(VN_NO_STREAM, harkentest::PlayRaw(&play, &voice));
    EXPECT_EQ(0u, voice);
    EXPECT_EQ(freeSlots, streams.FreeSlotCount());
    EXPECT_EQ(0u, session.Engine()->voices.ActiveCount());
    EXPECT_GT(RingAccess::CommittedBytes(streams), 0u);
    RingAccess::SetCommitPageLimit(streams, SIZE_MAX);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));

    ASSERT_TRUE(session.Engine()->streams.FindFile(harkentest::ObjectId(7001), &facts));
    EXPECT_EQ(44100u, facts.sampleRate);
    EXPECT_EQ(2u, facts.channelCount);
    EXPECT_EQ(800u, facts.frameCount);
    EXPECT_EQ(harken::kStreamFlagLoopable, facts.flags);
    EXPECT_EQ(100u, facts.loopStartFrame);
    EXPECT_EQ(700u, facts.loopEndFrame);
    EXPECT_NE(0u, facts.contentRevision);

    const auto originalRevision = facts.contentRevision;
    spec.channelCount = 3;
    planar.resize(800 * 3, 0.125f);
    const auto widerPath = TempPath("stream_reg_wider.stream");
    WriteFileBytes(widerPath, BuildStreamBytes(spec, planar));
    const auto ringBytes = RingAccess::CommittedBytes(streams);
    RingAccess::SetCommitPageLimit(streams, 0);
    ASSERT_EQ(VN_OK, Vn_ReplaceStreamFile(spec.soundId, widerPath.c_str()));
    ASSERT_TRUE(streams.FindFile(spec.soundId, &facts));
    EXPECT_EQ(3u, facts.channelCount);
    EXPECT_NE(originalRevision, facts.contentRevision);
    EXPECT_EQ(ringBytes, RingAccess::CommittedBytes(streams));
}

TEST(StreamRegister, PendingReferencePreservesWideBackingThroughNarrowStartsAndRollback) {
    auto config = MakeNullConfig(4, 256, kRate);
    config.maxStreams = 4;
    config.streamRingFrames = 1024;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& streams = session.Engine()->streams;
    using RingAccess = harken::detail::StreamManagerTestAccess;
    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(7011);
    spec.sampleRate = kRate;
    spec.channelCount = 2;
    spec.chunkFrames = 256;
    const auto widePath = TempPath("stream_pending_wide.stream");
    WriteFileBytes(widePath, BuildStreamBytes(spec, std::vector<float>(8192, 0.25f)));
    const auto monoPath = TempPath("stream_pending_mono.stream");
    WriteFileBytes(monoPath, MakeValidImage(harkentest::ObjectId(7012), 4096));
    ASSERT_EQ(VN_OK, streams.RegisterFile(spec.soundId, widePath.c_str()));
    ASSERT_EQ(VN_OK, streams.RegisterFile(harkentest::ObjectId(7012), monoPath.c_str()));
    harken::StreamManager::StreamFacts wide{}, mono{};
    ASSERT_TRUE(streams.FindFile(spec.soundId, &wide));
    ASSERT_TRUE(streams.FindFile(harkentest::ObjectId(7012), &mono));
    ASSERT_TRUE(streams.AcquirePendingRef(wide.fileIndex));
    for (std::uint32_t slot = 0; slot < 2; ++slot) {
        ASSERT_TRUE(streams.PrepareSlotWorkingSet(slot, 1));
        ASSERT_EQ(VN_OK, streams.ActivateSlot(slot, mono.fileIndex, 0, 0, 4096, 1));
    }
    const auto bytes = RingAccess::CommittedBytes(streams);
    RingAccess::SetCommitPageLimit(streams, 0);
    for (std::uint32_t retry = 0; retry < 8; ++retry) {
        std::uint32_t slot = UINT32_MAX;
        ASSERT_TRUE(streams.PeekPreparedFreeSlot(2, &slot));
        EXPECT_EQ(2u, slot);
        std::uint64_t token = 0;
        ASSERT_EQ(VN_OK, streams.ActivateSlot(slot, wide.fileIndex, 0, 0, 4096, 1, &token));
        ASSERT_TRUE(streams.RetirePreparedSlot(streams.RingOf(slot), token));
        EXPECT_EQ(bytes, RingAccess::CommittedBytes(streams));
    }
    streams.ReleasePendingRef(wide.fileIndex);
    streams.SyncServe();
    EXPECT_EQ(VN_OK, streams.UnregisterFile(spec.soundId));
}

TEST(StreamRegister, ContentRevisionUsesSemanticHeaderAndChunkChecksums) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(7201);
    spec.sampleRate = 44100;
    spec.channelCount = 1;
    spec.chunkFrames = 128;
    std::vector<float> samples =
        harkentest::MakeSine(600, 500.0, 44100.0, 0.5f);

    const std::string firstPath =
        TempPath("stream_content_revision_first.stream");
    WriteFileBytes(firstPath, BuildStreamBytes(spec, samples));
    ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(spec.soundId, firstPath.c_str()));
    harken::StreamManager::StreamFacts first;
    ASSERT_TRUE(session.Engine()->streams.FindFile(spec.soundId, &first));
    ASSERT_NE(0u, first.contentRevision);

    spec.soundId = harkentest::ObjectId(7202);
    const std::string samePath =
        TempPath("stream_content_revision_same.stream");
    WriteFileBytes(samePath, BuildStreamBytes(spec, samples));
    ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(spec.soundId, samePath.c_str()));
    harken::StreamManager::StreamFacts same;
    ASSERT_TRUE(session.Engine()->streams.FindFile(spec.soundId, &same));
    EXPECT_EQ(first.contentRevision, same.contentRevision);

    spec.soundId = harkentest::ObjectId(7203);
    samples[257] += 0.001f;
    const std::string changedPath =
        TempPath("stream_content_revision_changed.stream");
    WriteFileBytes(changedPath, BuildStreamBytes(spec, samples));
    ASSERT_EQ(VN_OK,
              Vn_RegisterStreamFile(spec.soundId, changedPath.c_str()));
    harken::StreamManager::StreamFacts changed;
    ASSERT_TRUE(session.Engine()->streams.FindFile(spec.soundId, &changed));
    EXPECT_NE(first.contentRevision, changed.contentRevision);

    spec.soundId = harkentest::ObjectId(7204);
    spec.chunkFrames = 256;
    const std::string rechunkedPath =
        TempPath("stream_content_revision_rechunked.stream");
    WriteFileBytes(rechunkedPath, BuildStreamBytes(spec, samples));
    ASSERT_EQ(VN_OK,
              Vn_RegisterStreamFile(spec.soundId, rechunkedPath.c_str()));
    harken::StreamManager::StreamFacts rechunked;
    ASSERT_TRUE(session.Engine()->streams.FindFile(spec.soundId, &rechunked));
    EXPECT_NE(changed.contentRevision, rechunked.contentRevision);
}

TEST(StreamRegister, EnqueueFailureReturnsBadFileAndCanRetry) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::string path = TempPath("stream_reg_enqueue_fault.stream");
    WriteFileBytes(path, MakeValidImage(harkentest::ObjectId(7001)));

    {
        StreamRequestFaultGuard fault(harken::StreamRequestFault::Enqueue);
        EXPECT_EQ(VN_BAD_STREAM_FILE,
                  Vn_RegisterStreamFile(harkentest::ObjectId(7001), path.c_str()));
    }

    harken::StreamManager::StreamFacts facts;
    EXPECT_FALSE(session.Engine()->streams.FindFile(harkentest::ObjectId(7001), &facts));
    EXPECT_EQ(VN_OK, Vn_RegisterStreamFile(harkentest::ObjectId(7001), path.c_str()));
    EXPECT_EQ(VN_OK, Vn_UnregisterStreamFile(harkentest::ObjectId(7001)));
}

TEST(StreamRegister, ChunkFramesAcceptanceMatchesSharedCeiling) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> oneFrame = {0.25f};
    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(7010);
    spec.sampleRate = kRate;
    spec.channelCount = 1;
    spec.chunkFrames = harken::kMaxStreamChunkFrames;
    const std::string acceptedPath =
        TempPath("stream_reg_chunk_ceiling.stream");
    WriteFileBytes(acceptedPath, BuildStreamBytes(spec, oneFrame));
    EXPECT_EQ(VN_OK,
              Vn_RegisterStreamFile(spec.soundId, acceptedPath.c_str()));

    spec.soundId = harkentest::ObjectId(7011);
    spec.chunkFrames = harken::kMaxStreamChunkFrames + 1;
    const std::string refusedPath =
        TempPath("stream_reg_chunk_over_ceiling.stream");
    WriteFileBytes(refusedPath, BuildStreamBytes(spec, oneFrame));
    EXPECT_EQ(VN_BAD_STREAM_FILE,
              Vn_RegisterStreamFile(spec.soundId, refusedPath.c_str()));
}

TEST(StreamRegister, ArgumentAndLifecycleRefusals) {
    // No engine: shape checks cannot even run.
    EXPECT_EQ(VN_NOT_INITIALIZED,
              Vn_RegisterStreamFile(harkentest::ObjectId(7002), "irrelevant"));
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_UnregisterStreamFile(harkentest::ObjectId(7002)));

    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    EXPECT_EQ(VN_INVALID_ARG, Vn_RegisterStreamFile(harkentest::ObjectId(7002), nullptr));

    // A missing file is a bad stream file, not a bad argument.
    const std::string missing = TempPath("stream_reg_missing.stream");
    std::filesystem::remove(missing);
    EXPECT_EQ(VN_BAD_STREAM_FILE,
              Vn_RegisterStreamFile(harkentest::ObjectId(7002), missing.c_str()));

    // Unregistering an id that names no stream.
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_UnregisterStreamFile(harkentest::ObjectId(7002)));
}

TEST(StreamRegister, CorruptContainersRefusedWithoutSideEffects) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<std::uint8_t> valid = MakeValidImage(harkentest::ObjectId(7003));
    const std::string path = TempPath("stream_reg_corrupt.stream");

    struct Corruption {
        const char* name;
        void (*mutate)(std::vector<std::uint8_t>*);
    };
    const Corruption corruptions[] = {
        {"wrong magic",
         [](std::vector<std::uint8_t>* b) { (*b)[0] = 'X'; }},
        {"unknown format version",
         [](std::vector<std::uint8_t>* b) {
             harken::StoreLE32(b->data() + 4, 999);
         }},
        {"entry count not one",
         [](std::vector<std::uint8_t>* b) {
             harken::StoreLE32(b->data() + 20, 2);
         }},
        {"TOC entry byte flipped (tocChecksum mismatch)",
         [](std::vector<std::uint8_t>* b) {
             (*b)[harken::kStreamHeaderBytes + 16] ^= 0x01;
         }},
        {"chunk-checksum table byte flipped (tocChecksum mismatch)",
         [](std::vector<std::uint8_t>* b) {
             (*b)[harken::kStreamHeaderBytes +
                  harken::kStreamTocEntryBytes] ^= 0x01;
         }},
        {"file truncated mid-payload",
         [](std::vector<std::uint8_t>* b) { b->resize(b->size() / 2); }},
        {"chunk count off by one (validated field, checksum refit)",
         [](std::vector<std::uint8_t>* b) {
             harken::StreamTocEntry entry = harken::ParseStreamTocEntry(
                 b->data() + harken::kStreamHeaderBytes);
             entry.chunkCount += 1;
             harken::SerializeStreamTocEntry(
                 entry, b->data() + harken::kStreamHeaderBytes);
             FixTocChecksum(b);
         }},
        {"loop fields on a non-loopable entry (checksum refit)",
         [](std::vector<std::uint8_t>* b) {
             harken::StreamTocEntry entry = harken::ParseStreamTocEntry(
                 b->data() + harken::kStreamHeaderBytes);
             entry.loopStartFrame = 10;
             entry.loopEndFrame = 20;
             harken::SerializeStreamTocEntry(
                 entry, b->data() + harken::kStreamHeaderBytes);
             FixTocChecksum(b);
         }},
    };

    for (const Corruption& corruption : corruptions) {
        std::vector<std::uint8_t> bytes = valid;
        corruption.mutate(&bytes);
        WriteFileBytes(path, bytes);
        EXPECT_EQ(VN_BAD_STREAM_FILE,
                  Vn_RegisterStreamFile(harkentest::ObjectId(7003), path.c_str()))
            << corruption.name;
        harken::StreamManager::StreamFacts facts;
        EXPECT_FALSE(session.Engine()->streams.FindFile(harkentest::ObjectId(7003), &facts))
            << corruption.name << ": refusal must leave no registration";
    }

    // The untouched image proves the fixture itself is valid — the
    // refusals above measured the corruption, not the builder.
    WriteFileBytes(path, valid);
    EXPECT_EQ(VN_OK, Vn_RegisterStreamFile(harkentest::ObjectId(7003), path.c_str()));
}

TEST(StreamRegister, TocSoundIdMustMatchRegisteredId) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    // The file's TOC names the id it was compiled for; registering it
    // under a different id is a packaging error and must refuse instead
    // of silently serving the wrong content at playback.
    const std::string path = TempPath("stream_reg_id_mismatch.stream");
    WriteFileBytes(path, MakeValidImage(harkentest::ObjectId(7006)));

    EXPECT_EQ(VN_BAD_STREAM_FILE, Vn_RegisterStreamFile(harkentest::ObjectId(7007), path.c_str()));
    harken::StreamManager::StreamFacts facts;
    EXPECT_FALSE(session.Engine()->streams.FindFile(harkentest::ObjectId(7007), &facts));

    // The identical file under its own id registers normally, proving the
    // refusal measured the mismatch and not the fixture.
    EXPECT_EQ(VN_OK, Vn_RegisterStreamFile(harkentest::ObjectId(7006), path.c_str()));
}

TEST(StreamRegister, SoundIdNamespaceIsSharedWithPCM) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::string path = TempPath("stream_reg_shared.stream");
    WriteFileBytes(path, MakeValidImage(harkentest::ObjectId(7004)));

    ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(harkentest::ObjectId(7004), path.c_str()));
    // Same id again as a stream: taken.
    EXPECT_EQ(VN_IN_USE, Vn_RegisterStreamFile(harkentest::ObjectId(7004), path.c_str()));
    // Same id as resident PCM: also taken — one namespace.
    const std::vector<float> tone = harkentest::MakeSine(64, 500.0, kRate,
                                                        0.5f);
    VnPCMDesc desc = harkentest::MonoDesc(tone, kRate);
    EXPECT_EQ(VN_IN_USE, Vn_RegisterPCM(harkentest::ObjectId(7004), &desc));

    // And the mirror image: a PCM-registered id refuses stream
    // registration.
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(7005), &desc));
    EXPECT_EQ(VN_IN_USE, Vn_RegisterStreamFile(harkentest::ObjectId(7005), path.c_str()));

    // Unregistration frees the name for either namespace.
    ASSERT_EQ(VN_OK, Vn_UnregisterStreamFile(harkentest::ObjectId(7004)));
    harken::StreamManager::StreamFacts facts;
    EXPECT_FALSE(session.Engine()->streams.FindFile(harkentest::ObjectId(7004), &facts));
    EXPECT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(7004), &desc));
}

// Illegal UTF-8 in the path is refused end to end, never undefined
// behavior: the streaming thread converts the path during the blocking
// register, so a byte sequence that is not valid UTF-8 must surface as
// VN_BAD_STREAM_FILE rather than escaping the conversion — the thread must
// not terminate and PostAndWait must not hang. Each literal terminates its
// hex escape at the quote so the following ASCII is a separate token.
constexpr const char* kIllegalUtf8Paths[] = {
    "\xFF\xFE\x80" "stream",  // bytes that are never UTF-8 lead bytes
    "\x80" "orphan",          // an isolated continuation byte, no lead
    "\xC0\xAF" "overlong",    // overlong two-byte encoding of '/'
};

TEST(StreamRegister, IllegalUtf8PathRefusedAndStreamThreadSurvives) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    for (const char* illegal : kIllegalUtf8Paths) {
        EXPECT_EQ(VN_BAD_STREAM_FILE,
                  Vn_RegisterStreamFile(harkentest::ObjectId(7101), illegal)) << illegal;
        harken::StreamManager::StreamFacts facts;
        EXPECT_FALSE(session.Engine()->streams.FindFile(harkentest::ObjectId(7101), &facts))
            << illegal << ": refusal must leave no registration";
    }

    // Proof the streaming thread outlived every refusal and PostAndWait was
    // signalled each time: a well-formed file still registers on the same
    // session and the update pump completes.
    const std::string path = TempPath("stream_reg_after_illegal.stream");
    WriteFileBytes(path, MakeValidImage(harkentest::ObjectId(7101)));
    EXPECT_EQ(VN_OK, Vn_RegisterStreamFile(harkentest::ObjectId(7101), path.c_str()));
    EXPECT_EQ(VN_OK, Vn_Update());
}

TEST(StreamRegister, IllegalUtf8ReplacePathRefusedOldFileKept) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::string path = TempPath("stream_reg_replace_base.stream");
    WriteFileBytes(path, MakeValidImage(harkentest::ObjectId(7102)));
    ASSERT_EQ(VN_OK, Vn_RegisterStreamFile(harkentest::ObjectId(7102), path.c_str()));

    // An illegal-UTF-8 replacement path is a bad stream file with the old
    // registration untouched, never an escaping conversion.
    EXPECT_EQ(VN_BAD_STREAM_FILE,
              Vn_ReplaceStreamFile(harkentest::ObjectId(7102), kIllegalUtf8Paths[0]));

    // The original file is still registered and serving its own facts.
    harken::StreamManager::StreamFacts facts;
    ASSERT_TRUE(session.Engine()->streams.FindFile(harkentest::ObjectId(7102), &facts));
    EXPECT_EQ(1000u, facts.frameCount);
    EXPECT_EQ(kRate, facts.sampleRate);
}

// A well-formed multibyte UTF-8 path registers end to end. The UTF-8 funnel
// wrapped around the streaming thread's open must accept legal non-ASCII, not
// reject everything past 7-bit — the mirror of the refusals above, proving
// they measured ill-formed bytes rather than the mere presence of multibyte
// ones. The filename carries Chinese plus an emoji. The file is created
// through a path decoded from the same UTF-8 bytes the API is handed, so the
// test's write and the engine's UTF-8 -> native transcode resolve to one file;
// a narrow WriteFileBytes would decode those bytes in the ANSI codepage on
// Windows and name a different file. The hex escapes seal at the quote so the
// ASCII tail is a separate token.
TEST(StreamRegister, LegalUtf8PathRegistersEndToEnd) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    // The filename contains two CJK code points and a musical-note emoji.
    const std::string utf8Path = TempPath(
        "stream_reg_" "\xE6\xB5\x8B\xE8\xAF\x95" "\xF0\x9F\x8E\xB5" ".stream");
    const std::filesystem::path filePath(
        reinterpret_cast<const char8_t*>(utf8Path.c_str()));

    std::filesystem::create_directories(filePath.parent_path());
    {
        const std::vector<std::uint8_t> image = MakeValidImage(harkentest::ObjectId(7103));
        std::ofstream out(filePath, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out.good()) << "cannot create UTF-8-named stream file";
        out.write(reinterpret_cast<const char*>(image.data()),
                  static_cast<std::streamsize>(image.size()));
        ASSERT_TRUE(out.good());
    }

    EXPECT_EQ(VN_OK, Vn_RegisterStreamFile(harkentest::ObjectId(7103), utf8Path.c_str()));
    harken::StreamManager::StreamFacts facts;
    EXPECT_TRUE(session.Engine()->streams.FindFile(harkentest::ObjectId(7103), &facts));

    // Leave nothing behind: drop the registration, then the file.
    EXPECT_EQ(VN_OK, Vn_UnregisterStreamFile(harkentest::ObjectId(7103)));
    std::error_code ec;
    std::filesystem::remove(filePath, ec);
}

} // namespace
