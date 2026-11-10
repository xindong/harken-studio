#include "Common/StreamFixtureRegistration.h"
#include "Common/PcmStreamFixture.h"
#include "Common/ExternalPcmFixture.h"
// Streamed playback mechanics on the null backend: prebuffer gating and
// the PREPARING snapshot state, stream slot exhaustion and refusal
// ordering, starvation survival with exact notification cooldown spacing and
// automatic recovery, mid-stream integrity abort, ring occupancy
// observability, and the allocation-guard boundary between the streaming
// thread and the audio thread. Every drill is made deterministic through
// the stream manager's test hooks (throttle + synchronous serve) — no
// sleeps, no timing luck.
#include "Common/ObjectIdTestSupport.h"
#include "Common/StreamBuilder.h"
#include "Common/TestUtil.h"

#include "harken/core/LogicAllocCheckpoint.h"
#include "harken/core/ResamplerAdvance.h"
#include "harken/core/SourcePhase.h"
#include "harken/media/StreamFormat.h"
#include "harken/media/StreamManager.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <thread>
#include <vector>

#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the repository's .temp directory"
#endif

namespace {

using harkentest::BuildStreamBytes;
using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::StreamSpec;
using harkentest::WriteFileBytes;

constexpr std::uint32_t kBlockFrames = 256;
constexpr std::uint32_t kRate = 48000;

std::string TempPath(const std::string& name) {
    return std::string(HARKEN_TEMP_DIR "/") + name;
}

struct NotificationLog {
    std::vector<VnNotification> notifications;

    std::vector<VnNotification> OfType(std::uint32_t type) const {
        std::vector<VnNotification> matching;
        for (const VnNotification& notification : notifications) {
            if (notification.type == type) {
                matching.push_back(notification);
            }
        }
        return matching;
    }
};

void RecordNotification(const VnNotification* notification, void* userData) {
    static_cast<NotificationLog*>(userData)->notifications.push_back(*notification);
}

VnConfig MakeStreamConfig(NotificationLog* log, std::uint32_t maxVoices = 0,
                          std::uint32_t ringFrames = 0,
                          std::uint32_t maxStreams = 0,
                          std::uint32_t prebufferMs = 0,
                          std::uint32_t starvationCooldownBlocks = 0) {
    VnConfig config = MakeNullConfig(maxVoices, kBlockFrames, kRate);
    // Stream progress coverage does not require the default nested Action scratch.
    config.maxTriggerDepth = 2;
    config.onNotification = log != nullptr ? &RecordNotification : nullptr;
    config.notificationUserData = log;
    config.streamRingBytes = (ringFrames) * sizeof(float);
    config.maxStreams = maxStreams;
    config.prebufferMs = prebufferMs;
    config.starvationCooldownBlocks = starvationCooldownBlocks;
    return config;
}

// Builds and writes a mono sine .stream file, returning its path.
std::string WriteSineStream(const std::string& name, VnObjectId soundId,
                            std::size_t frames,
                            std::uint32_t chunkFrames = 256) {
    StreamSpec spec;
    spec.soundId = soundId;
    spec.sampleRate = kRate;
    spec.channelCount = 1;
    spec.chunkFrames = chunkFrames;
    const std::vector<float> tone =
        harkentest::MakeSine(frames, 500.0, kRate, 0.5f);
    const std::string path = TempPath(name);
    WriteFileBytes(path, BuildStreamBytes(spec, tone));
    return path;
}

// Pumps one block and returns its stereo peak.
float PumpBlockPeak(harken::Engine* engine) {
    Capture capture;
    harkentest::PumpAndCapture(engine, 1, capture);
    return std::max(harkentest::Peak(capture.left),
                    harkentest::Peak(capture.right));
}

VnSnapshot Snap() {
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    return snapshot;
}

// The snapshot voice row correlated to a public handle. The public handle is
// a ledger handle whose low bits are not the render slot, so the owning row is
// found through the voicePublicHandles correlation array rather than by
// masking the handle.
std::uint32_t RowOf(const VnSnapshot& snapshot, VnVoiceHandle handle) {
    const int row = harkentest::VoiceRowForHandle(snapshot, handle);
    EXPECT_GE(row, 0);
    return row < 0 ? 0u : static_cast<std::uint32_t>(row);
}

std::vector<float> MakeDistinctSignal(std::size_t frames) {
    std::vector<float> s(frames);
    for (std::size_t i = 0; i < frames; ++i) {
        const int centered = static_cast<int>((i * 37u) % 97u) - 48;
        s[i] = 0.0025f * static_cast<float>(centered);
    }
    return s;
}

std::vector<float> MakeLoopConcatenation(const std::vector<float>& source,
                                         std::uint64_t loopStart,
                                         std::uint64_t loopEnd,
                                         std::uint32_t loopCount) {
    std::vector<float> out;
    if (loopCount == 1) {
        return source;
    }
    out.insert(out.end(), source.begin(),
               source.begin() + static_cast<std::ptrdiff_t>(loopStart));
    for (std::uint32_t pass = 0; pass < loopCount; ++pass) {
        out.insert(out.end(),
                   source.begin() + static_cast<std::ptrdiff_t>(loopStart),
                   source.begin() + static_cast<std::ptrdiff_t>(loopEnd));
    }
    out.insert(out.end(),
               source.begin() + static_cast<std::ptrdiff_t>(loopEnd),
               source.end());
    return out;
}

std::uint64_t RoundUpPow2ForTest(std::uint64_t v) {
    std::uint64_t p = 1;
    while (p < v) {
        p <<= 1;
    }
    return p;
}

Capture RenderStreamSound(const std::string& name, VnSoundId soundId,
                          const StreamSpec& specIn,
                          const std::vector<float>& source,
                          std::uint32_t loopCount, std::uint32_t blocks,
                          std::uint32_t ringFrames = 384,
                          std::uint32_t maxServeFrames = 512,
                          NotificationLog* log = nullptr,
                          VnVoiceHandle* outVoice = nullptr) {
    StreamSpec spec = specIn;
    spec.soundId = soundId;
    spec.sampleRate = kRate;
    spec.channelCount = 1;
    const std::string path = TempPath(name);
    WriteFileBytes(path, BuildStreamBytes(spec, source));

    EngineSession session(MakeStreamConfig(log, /*maxVoices=*/0, ringFrames,
                                           /*maxStreams=*/0,
                                           /*prebufferMs=*/1,
                                           /*starvationCooldownBlocks=*/4));
    EXPECT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    engine->streams.SetMaxServeFramesForTest(maxServeFrames);
    EXPECT_EQ(VN_OK, harkentest::RegisterFixtureStream(soundId, path.c_str()));

    VnPlayDesc play = MakePlayDesc(soundId);
    play.loopCount = loopCount;
    VnVoiceHandle voice = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    if (outVoice != nullptr) {
        *outVoice = voice;
    }

    Capture capture;
    for (std::uint32_t block = 0; block < blocks; ++block) {
        engine->streams.SyncServe();
        harkentest::PumpAndCapture(engine, 1, capture);
    }
    return capture;
}

void ExpectStreamLoopMatchesExplicitConcat(
    const char* tag, const std::vector<float>& source,
    std::uint32_t chunkFrames, bool loopable, std::uint64_t loopStart,
    std::uint64_t loopEnd, std::uint32_t loopCount,
    std::uint32_t maxServeFrames = 512) {
    const std::uint64_t effectiveEnd =
        loopable ? (loopEnd == 0 ? source.size() : loopEnd) : source.size();
    const std::uint64_t effectiveStart = loopable ? loopStart : 0;
    const std::vector<float> explicitSource = MakeLoopConcatenation(
        source, effectiveStart, effectiveEnd, loopCount);
    HARKENTEST_ASSERT_NON_SILENT(source);
    HARKENTEST_ASSERT_NON_SILENT(explicitSource);

    StreamSpec loopSpec;
    loopSpec.chunkFrames = chunkFrames;
    loopSpec.loopable = loopable;
    loopSpec.loopStartFrame = loopStart;
    loopSpec.loopEndFrame = loopEnd;
    StreamSpec refSpec;
    refSpec.chunkFrames = chunkFrames;

    const std::uint32_t blocks = static_cast<std::uint32_t>(
        explicitSource.size() / kBlockFrames + 6);
    const Capture looped = RenderStreamSound(
        std::string("stream_loop_") + tag + ".stream", harkentest::ObjectId(7201), loopSpec,
        source, loopCount, blocks, /*ringFrames=*/384, maxServeFrames);
    const Capture reference = RenderStreamSound(
        std::string("stream_loop_ref_") + tag + ".stream", harkentest::ObjectId(7202), refSpec,
        explicitSource, 1, blocks, /*ringFrames=*/384, maxServeFrames);

    HARKENTEST_ASSERT_NON_SILENT(looped.left);
    ASSERT_EQ(reference.left.size(), looped.left.size());
    for (std::size_t i = 0; i < reference.left.size(); ++i) {
        ASSERT_EQ(reference.left[i], looped.left[i])
            << tag << " left sample " << i;
        ASSERT_EQ(reference.right[i], looped.right[i])
            << tag << " right sample " << i;
    }
}

TEST(StreamPlayback, PreparingHoldsSilentUntilPrebufferThreshold) {
    NotificationLog log;
    // Ring 1024 frames; the default 100 ms prebuffer target (4800 frames)
    // clamps to the ring capacity, so the gate needs a full ring.
    EngineSession session(MakeStreamConfig(&log, 0, 1024));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string path =
        WriteSineStream("stream_prepare.stream", harkentest::ObjectId(7101), 48000);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(7101), path.c_str()));

    // Throttled before play: the activation-time serve pushes nothing,
    // so the ring is verifiably empty when the first block renders.
    engine->streams.SetThrottled(true);
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(7101));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));

    // Gated blocks are exactly silent, the voice stays alive, and the
    // snapshot reports PREPARING — not ACTIVE, not INACTIVE.
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(0.0f, PumpBlockPeak(engine)) << "gated block " << i;
    }
    ASSERT_NE(nullptr, engine->voices.Resolve(harkentest::RenderHandle(engine,
        voice)));
    VnSnapshot snapshot = Snap();
    EXPECT_EQ(1u, snapshot.activeVoiceCount);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_VOICE_STATE_PREPARING),
              snapshot.voices[RowOf(snapshot, voice)].state);

    // No starvation while preparing: the gate is the announced fill
    // phase, not a data loss.
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_TRUE(log.OfType(VN_NOTIFY_VOICE_STARVED).empty());

    // Data arrives: one full serve fills the ring past the threshold and
    // the very next block opens the gate and sounds.
    engine->streams.SetThrottled(false);
    engine->streams.SyncServe();
    EXPECT_GT(PumpBlockPeak(engine), harkentest::kSilenceFloor);
    snapshot = Snap();
    EXPECT_EQ(static_cast<std::uint32_t>(VN_VOICE_STATE_ACTIVE),
              snapshot.voices[RowOf(snapshot, voice)].state);

    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(StreamPlayback, SpatialRecipeAndCommitApplyBeforeFirstPrebufferedOutput) {
    for (const bool staged : {false, true}) {
        SCOPED_TRACE(staged);
        EngineSession session(MakeStreamConfig(nullptr, 2, 1024, 1, 1));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto* engine = session.Engine();
        const auto sound = harkentest::ObjectId(7198);
        const auto attenuation = harkentest::ObjectId(7199);
        const auto profile = harkentest::MakeAttenuationDesc(1.0f, 101.0f);
        ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(attenuation, &profile));
        StreamSpec spec;
        spec.soundId = sound;
        spec.sampleRate = kRate;
        spec.channelCount = 1;
        spec.chunkFrames = kBlockFrames;
        const auto path = TempPath("stream_spatial_initial.stream");
        WriteFileBytes(path, BuildStreamBytes(spec, harkentest::MakeDC(4096, 0.25f)));
        ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(sound, path.c_str()));
        if (!staged) {
            harken::PackLogicStore::PreparedClaimToken token;
            ASSERT_EQ(VN_OK, engine->logicPacks.PrepareClaim(1, &token));
            harken::PackLogicRecord content;
            harken::PackLogicImage::VoiceRecipe recipe;
            recipe.header.playableId = sound;
            recipe.spatial.attenuationId = attenuation;
            recipe.spatial.distanceScale = 1.0f;
            content.voiceRecipes.push_back(std::move(recipe));
            engine->logicPacks.CommitPreparedClaim(&token, std::move(content));
        }
        engine->streams.SetThrottled(true);
        auto play = harkentest::MakePlayDesc3D(sound, 51.0f, 0.0f, 0.0f);
        play.reserved0 = staged ? VN_PLAY_FLAG_STAGED_INSTALL : 0u;
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        if (staged) {
            ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(voice, attenuation));
            EXPECT_EQ(0.0f, PumpBlockPeak(engine));
            ASSERT_EQ(VN_OK, Vn_CommitVoiceStart(voice));
        }
        EXPECT_EQ(0.0f, PumpBlockPeak(engine));
        EXPECT_EQ(0.0f, PumpBlockPeak(engine));
        engine->streams.SetThrottled(false);
        engine->streams.SyncServe();
        const auto before = engine->mixer.BlocksRendered();
        Capture first;
        harkentest::PumpAndCapture(engine, 1, first);
        ASSERT_EQ(before + 1, engine->mixer.BlocksRendered());
        ASSERT_EQ(kBlockFrames, first.right.size());
        const auto expected = 0.25f * 0.5f * std::sqrt(2.0f);
        for (std::size_t frame = 0; frame < first.right.size(); ++frame) {
            ASSERT_NEAR(0.0f, first.left[frame], 1e-5f) << frame;
            ASSERT_NEAR(expected, first.right[frame], 1e-5f) << frame;
        }
    }
}

TEST(StreamPlayback, PlaysToEndReleasesSlotAndRecycles) {
    NotificationLog log;
    EngineSession session(MakeStreamConfig(&log));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // Short enough that the whole file prebuffers in one serve (the gate
    // opens through end-of-data, below the frame threshold).
    const std::string path =
        WriteSineStream("stream_toend.stream", harkentest::ObjectId(7102), 2000);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(7102), path.c_str()));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(7102));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));

    auto* ring = engine->streams.RingOf(0);
    ASSERT_TRUE(ring->SourceEnded());
    const auto* originalBytes = ring->bytes;
    const std::vector<std::uint8_t> buffered(originalBytes,
                                       originalBytes + ring->capacity);
    StreamSpec wider;
    wider.soundId = harkentest::ObjectId(7199);
    wider.sampleRate = kRate;
    wider.channelCount = 2;
    wider.chunkFrames = 256;
    const auto widerPath = TempPath("stream_toend_wider.stream");
    WriteFileBytes(widerPath, BuildStreamBytes(wider, {0.25f, -0.25f}));
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(wider.soundId, widerPath.c_str()));
    EXPECT_EQ(originalBytes, ring->bytes);
    EXPECT_TRUE(std::equal(buffered.begin(), buffered.end(), ring->bytes));

    Capture capture;
    harkentest::PumpAndCapture(engine, 2000 / kBlockFrames + 3, capture);
    HARKENTEST_ASSERT_NON_SILENT(capture.left);
    EXPECT_EQ(nullptr, engine->voices.Resolve(harkentest::RenderHandle(engine,
        voice)));

    ASSERT_EQ(VN_OK, Vn_Update());
    const std::vector<VnNotification> ended = log.OfType(VN_NOTIFY_VOICE_ENDED);
    ASSERT_EQ(1u, ended.size());
    EXPECT_EQ(voice, ended[0].voiceHandle);
    EXPECT_TRUE(log.OfType(VN_NOTIFY_VOICE_STARVED).empty());

    // The audio thread retired the slot; one streaming-thread pass
    // detaches it and the slot serves a new play.
    engine->streams.SyncServe();
    VnVoiceHandle second = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &second));
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(StreamPlayback, NoStreamRefusalIsSideEffectFreeAndOrdered) {
    NotificationLog log;
    EngineSession session(MakeStreamConfig(&log, /*maxVoices=*/4,
                                           /*ringFrames=*/0,
                                           /*maxStreams=*/1));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string path =
        WriteSineStream("stream_nostream.stream", harkentest::ObjectId(7103), 48000);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(7103), path.c_str()));
    const std::vector<float> tone =
        harkentest::MakeSine(48000, 500.0, kRate, 0.5f);
    VnPCMDesc pcm = harkentest::MonoDesc(tone, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(7104), &pcm));

    const VnPlayDesc playStream = MakePlayDesc(harkentest::ObjectId(7103));
    const VnPlayDesc playPcm = MakePlayDesc(harkentest::ObjectId(7104));

    VnVoiceHandle streamVoice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&playStream, &streamVoice));

    // The single stream slot is taken; a second streamed play refuses.
    VnVoiceHandle refused = 0;
    EXPECT_EQ(VN_NO_STREAM, harkentest::PlayRaw(&playStream, &refused));

    // Side-effect-free: the refusal consumed no voice slot — all three
    // remaining voices are still claimable.
    VnVoiceHandle pcmVoices[3] = {};
    for (int i = 0; i < 3; ++i) {
        EXPECT_EQ(VN_OK, harkentest::PlayRaw(&playPcm, &pcmVoices[i])) << "voice " << i;
    }

    // Refusal ordering: with the voice pool now exhausted too, the voice
    // refusal is reported, not the stream refusal.
    EXPECT_EQ(VN_NO_VOICE, harkentest::PlayRaw(&playStream, &refused));
    EXPECT_EQ(VN_NO_VOICE, harkentest::PlayRaw(&playPcm, &refused));

    VnSnapshot snapshot = Snap();
    EXPECT_EQ(1u, snapshot.streamHighWater);

    // Release everything; after the audio thread retires the stream slot
    // and the streaming thread detaches it, the slot serves a new play.
    ASSERT_EQ(VN_OK, Vn_Stop(streamVoice, 0));
    for (VnVoiceHandle handle : pcmVoices) {
        ASSERT_EQ(VN_OK, Vn_Stop(handle, 0));
    }
    engine->nullBackend.Pump(2);
    engine->streams.SyncServe();
    VnVoiceHandle recycled = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&playStream, &recycled));
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(StreamPlayback, StarvationSurvivesWithExactCooldownAndRecovers) {
    NotificationLog log;
    // Small ring (two blocks), minimal prebuffer, cooldown 4: the drill
    // renders two sounding blocks from the initial fill, then the
    // throttled ring runs dry while the source stays active.
    constexpr std::uint32_t kRingFrames = 512;
    constexpr std::uint32_t kCooldown = 4;
    EngineSession session(MakeStreamConfig(&log, 0, kRingFrames, 0,
                                           /*prebufferMs=*/1, kCooldown));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string path =
        WriteSineStream("stream_starve.stream", harkentest::ObjectId(7105), 48000);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(7105), path.c_str()));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(7105));
    VnVoiceHandle voice = 0;
    // The activation-time serve fills the ring; throttling immediately
    // after guarantees no refill once pumping starts.
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    engine->streams.SetThrottled(true);

    // 14 blocks against a 512-frame fill: roughly two sounding blocks,
    // then continuous starvation. The exact block where the shortfall
    // begins may shift by one with resampler priming, so the assertions
    // below measure the notification spacing, not the absolute start.
    std::vector<float> blockPeaks;
    for (int i = 0; i < 14; ++i) {
        blockPeaks.push_back(PumpBlockPeak(engine));
    }
    EXPECT_GT(blockPeaks[0], harkentest::kSilenceFloor)
        << "the prebuffered material must sound before the ring dries";
    for (int i = 5; i < 14; ++i) {
        EXPECT_EQ(0.0f, blockPeaks[i])
            << "block " << i << ": starvation renders exact silence";
    }

    // The voice survives the gap — no end, no teardown.
    ASSERT_NE(nullptr, engine->voices.Resolve(harkentest::RenderHandle(engine,
        voice)));
    VnSnapshot snapshot = Snap();
    EXPECT_EQ(static_cast<std::uint32_t>(VN_VOICE_STATE_ACTIVE),
              snapshot.voices[RowOf(snapshot, voice)].state);

    // Starvation notifications land exactly cooldown blocks apart while the
    // shortfall is continuous, and the snapshot counter counts notifications
    // (not silent blocks).
    ASSERT_EQ(VN_OK, Vn_Update());
    const std::vector<VnNotification> starved = log.OfType(VN_NOTIFY_VOICE_STARVED);
    ASSERT_GE(starved.size(), 3u);
    for (std::size_t i = 1; i < starved.size(); ++i) {
        EXPECT_EQ(kCooldown, starved[i].blockTimestamp -
                                 starved[i - 1].blockTimestamp)
            << "notifications " << (i - 1) << " -> " << i;
        EXPECT_EQ(voice, starved[i].voiceHandle);
    }
    EXPECT_EQ(starved.size(), snapshot.voiceStarvedCount);
    EXPECT_TRUE(log.OfType(VN_NOTIFY_VOICE_ENDED).empty());

    // Ring occupancy observability: the high-water saw the initial fill
    // and can never exceed the ring's capacity.
    EXPECT_GT(snapshot.streamRingHighWaterFrames, 0u);
    EXPECT_LE(snapshot.streamRingHighWaterFrames,
              static_cast<std::uint64_t>(kRingFrames));

    // Recovery is automatic: data returns, sound resumes, and no new
    // starvation notifications fire while the ring is fed.
    engine->streams.SetThrottled(false);
    engine->streams.SyncServe();
    const std::size_t starvedBefore = starved.size();
    EXPECT_GT(PumpBlockPeak(engine), harkentest::kSilenceFloor);
    engine->streams.SyncServe();
    EXPECT_GT(PumpBlockPeak(engine), harkentest::kSilenceFloor);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(starvedBefore, log.OfType(VN_NOTIFY_VOICE_STARVED).size());
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(StreamPlayback, CorruptChunkEndsVoiceAfterVerifiedMaterial) {
    NotificationLog log;
    EngineSession session(MakeStreamConfig(&log));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // Registration verifies header, TOC, and the checksum table — not
    // payload chunks. Corrupt a mid-file chunk: the file registers, the
    // streaming thread hits the mismatch as it reads, stops feeding, and
    // the already-verified material drains before the voice ends.
    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(7106);
    spec.sampleRate = kRate;
    spec.channelCount = 1;
    spec.chunkFrames = 256;
    const std::vector<float> tone =
        harkentest::MakeSine(2000, 500.0, kRate, 0.5f);
    std::vector<std::uint8_t> bytes = BuildStreamBytes(spec, tone);
    const harken::StreamTocEntry toc = harken::ParseStreamTocEntry(
        bytes.data() + harken::kStreamHeaderBytes);
    const std::uint64_t chunkBytes =
        harken::StreamChunkBytes(256, 1);
    bytes[static_cast<std::size_t>(harken::StreamChunkFileOffset(
        toc.payloadOffset, 4, chunkBytes))] ^= 0x01;
    const std::string path = TempPath("stream_corrupt_chunk.stream");
    WriteFileBytes(path, bytes);

    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(7106), path.c_str()));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(7106));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));

    // Four verified chunks are 1024 frames; well before 12 blocks the
    // voice must have drained them and ended — not hung, not crashed.
    Capture capture;
    harkentest::PumpAndCapture(engine, 12, capture);
    HARKENTEST_ASSERT_NON_SILENT(capture.left);
    EXPECT_EQ(nullptr, engine->voices.Resolve(harkentest::RenderHandle(engine,
        voice)));

    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1u, log.OfType(VN_NOTIFY_VOICE_ENDED).size());
    // The abort presents as end-of-data, never as starvation.
    EXPECT_TRUE(log.OfType(VN_NOTIFY_VOICE_STARVED).empty());

    // The engine remains healthy for further pumping.
    engine->nullBackend.Pump(2);
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(StreamPlayback, ChecksummedNonFiniteSamplesNeverReachBusFilters) {
    constexpr std::uint32_t kFrames = 1033;
    for (const float invalid : {std::numeric_limits<float>::quiet_NaN(),
                                std::numeric_limits<float>::infinity(),
                                -std::numeric_limits<float>::infinity()}) {
        // Cover an initial chunk, the final partial chunk, and unused padding
        // in the second channel plane. Padding is checksummed but never played.
        for (const std::uint32_t frame : {64u, kFrames - 1u, kFrames}) {
            SCOPED_TRACE(invalid);
            SCOPED_TRACE(frame);
            const bool padding = frame == kFrames;
            NotificationLog log;
            EngineSession session(MakeStreamConfig(&log, 2, 4096, 1, 1));
            ASSERT_EQ(VN_OK, session.InitResult());
            auto* engine = session.Engine();
            ASSERT_EQ(VN_OK, Vn_SetBusFilter(
                VN_BUS_MASTER, 0, 4000.0f, 0.707f, 1, 0));

            StreamSpec spec;
            spec.soundId = harkentest::ObjectId(7201);
            spec.channelCount = 2;
            spec.chunkFrames = 256;
            auto bytes = BuildStreamBytes(spec,
                std::vector<float>(kFrames * spec.channelCount, 0.25f));
            const auto toc = harken::ParseStreamTocEntry(
                bytes.data() + harken::kStreamHeaderBytes);
            const auto chunkBytes = harken::StreamChunkBytes(
                spec.chunkFrames, spec.channelCount);
            const auto chunkIndex = frame / spec.chunkFrames;
            auto* chunk = bytes.data() + harken::StreamChunkFileOffset(
                toc.payloadOffset, chunkIndex, chunkBytes);
            std::memcpy(chunk + (spec.chunkFrames + frame % spec.chunkFrames) *
                                   sizeof(float),
                        &invalid, sizeof(invalid));
            harken::StoreLE64(bytes.data() + toc.chunkChecksumOffset +
                                 chunkIndex * harken::kStreamChunkChecksumBytes,
                             harken::Fnv1a64(chunk, chunkBytes));
            harken::StreamHeader header;
            ASSERT_TRUE(harken::ParseStreamHeader(bytes.data(), &header));
            header.tocChecksum = harken::Fnv1a64(
                bytes.data() + toc.chunkChecksumOffset,
                toc.chunkCount * harken::kStreamChunkChecksumBytes,
                harken::Fnv1a64(bytes.data() + harken::kStreamHeaderBytes,
                               harken::kStreamTocEntryBytes));
            harken::SerializeStreamHeader(header, bytes.data());
            const auto path = TempPath("stream_nonfinite_checksummed.stream");
            WriteFileBytes(path, bytes);
            ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(spec.soundId, path.c_str()));
            const auto play = MakePlayDesc(spec.soundId);
            VnVoiceHandle voice = 0;
            ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
            engine->streams.SyncServe();
            const auto* ring = engine->streams.RingOf(0);
            ASSERT_NE(nullptr, ring);
            EXPECT_EQ(padding ? harken::StreamTerminalReason::NaturalEnd
                              : harken::StreamTerminalReason::IntegrityAbort,
                      ring->TerminalReason());
            EXPECT_EQ(padding ? kFrames : chunkIndex * spec.chunkFrames,
                      ring->BufferedFrames());

            Capture verified;
            harkentest::PumpAndCapture(engine, 12, verified);
            if (frame >= spec.chunkFrames) {
                HARKENTEST_ASSERT_NON_SILENT(verified.left);
                HARKENTEST_ASSERT_NON_SILENT(verified.right);
            }
            ASSERT_EQ(VN_OK, Vn_Update());
            EXPECT_EQ(nullptr, engine->voices.Resolve(
                harkentest::RenderHandle(engine, voice)));
            const auto ended = log.OfType(VN_NOTIFY_VOICE_ENDED);
            ASSERT_EQ(1u, ended.size());
            EXPECT_EQ(voice, ended[0].voiceHandle);
            EXPECT_TRUE(log.OfType(VN_NOTIFY_VOICE_STARVED).empty());
            engine->streams.SyncServe();
            EXPECT_EQ(1u, engine->streams.FreeSlotCount());

            // The same persistent bus filter must still render healthy audio.
            const std::vector<float> healthy(4096, 0.25f);
            const auto pcm = harkentest::MonoDesc(healthy, kRate);
            const auto healthyId = harkentest::ObjectId(7202);
            ASSERT_EQ(VN_OK, Vn_RegisterPCM(healthyId, &pcm));
            const auto next = MakePlayDesc(healthyId);
            ASSERT_EQ(VN_OK, harkentest::PlayRaw(&next, &voice));
            engine->nullBackend.Pump(8);
            Capture resumed;
            harkentest::PumpAndCapture(engine, 1, resumed);
            for (std::size_t i = 0; i < resumed.left.size(); ++i) {
                ASSERT_NEAR(0.25f, resumed.left[i], 1e-5f) << i;
                ASSERT_NEAR(0.25f, resumed.right[i], 1e-5f) << i;
            }
            EXPECT_EQ(0u, harken::memguard::HitCount());
        }
    }
}

TEST(StreamPlayback, CorruptOutroChunkIsReachedOnlyOnFinalLoopPass) {
    NotificationLog log;
    EngineSession session(MakeStreamConfig(&log));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(7109);
    spec.sampleRate = kRate;
    spec.channelCount = 1;
    spec.chunkFrames = 64;
    spec.loopable = true;
    spec.loopStartFrame = 64;
    spec.loopEndFrame = 128;
    const std::vector<float> source = MakeDistinctSignal(320);
    std::vector<std::uint8_t> bytes = BuildStreamBytes(spec, source);
    const harken::StreamTocEntry toc = harken::ParseStreamTocEntry(
        bytes.data() + harken::kStreamHeaderBytes);
    const std::uint64_t chunkBytes =
        harken::StreamChunkBytes(spec.chunkFrames, spec.channelCount);
    bytes[static_cast<std::size_t>(harken::StreamChunkFileOffset(
        toc.payloadOffset, 3, chunkBytes))] ^= 0x01;
    const std::string path = TempPath("stream_corrupt_outro.stream");
    WriteFileBytes(path, bytes);

    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(7109), path.c_str()));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(7109));
    play.loopCount = 3;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));

    Capture capture;
    harkentest::PumpAndCapture(engine, 8, capture);
    HARKENTEST_ASSERT_NON_SILENT(capture.left);
    EXPECT_EQ(nullptr, engine->voices.Resolve(harkentest::RenderHandle(engine,
        voice)));

    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1u, log.OfType(VN_NOTIFY_VOICE_ENDED).size());
    EXPECT_TRUE(log.OfType(VN_NOTIFY_VOICE_STARVED).empty());
    engine->streams.SyncServe();
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1u, log.OfType(VN_NOTIFY_VOICE_ENDED).size());
    EXPECT_EQ(0u, harken::memguard::HitCount());
}



TEST(StreamPlayback, StreamingThreadRunsOutsideAudioGuard) {
    // The streaming thread's whole job is what the audio thread must
    // never do: file reads and staging allocations. A full streamed
    // playback with concurrent pumping proves those never count against
    // the audio-thread guard.
    EngineSession session(MakeNullConfig(0, kBlockFrames, kRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    // The armed check keeps the zero honest in builds where the guard is
    // live — a disarmed guard would make the hit-count assertion vacuous.
    // The guard installs during Init, so the check comes after it, and the
    // gate mirrors the guard's own activation condition.
#if defined(_DEBUG) && defined(_MSC_VER)
    EXPECT_TRUE(harken::memguard::IsArmed());
#endif
    harken::Engine* engine = session.Engine();

    const std::string path =
        WriteSineStream("stream_guard.stream", harkentest::ObjectId(7108), 20000);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(7108), path.c_str()));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(7108));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));

    // Interleave serves with pumping so streaming-thread I/O provably
    // overlaps rendered blocks all the way to the end of the file.
    for (int i = 0; i < 20000 / static_cast<int>(kBlockFrames) + 3; ++i) {
        engine->streams.SyncServe();
        engine->nullBackend.Pump(1);
    }
    EXPECT_EQ(nullptr, engine->voices.Resolve(harkentest::RenderHandle(engine,
        voice)));
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(StreamPlayback, LoopCountOneStreamMatchesSinglePass) {
    const std::vector<float> source = MakeDistinctSignal(333);
    ExpectStreamLoopMatchesExplicitConcat(
        "count_one", source, /*chunkFrames=*/64, /*loopable=*/true,
        /*loopStart=*/70, /*loopEnd=*/190, /*loopCount=*/1);
}

TEST(StreamPlayback, StreamWholeFileLoopSeamIsExactAtChunkBoundary) {
    const std::vector<float> source = MakeDistinctSignal(128);
    ExpectStreamLoopMatchesExplicitConcat(
        "whole_boundary", source, /*chunkFrames=*/64, /*loopable=*/true,
        /*loopStart=*/0, /*loopEnd=*/0, /*loopCount=*/3);
}

TEST(StreamPlayback, StreamSubregionLoopSeamIsExactInsideChunk) {
    const std::vector<float> source = MakeDistinctSignal(300);
    ExpectStreamLoopMatchesExplicitConcat(
        "subregion_middle", source, /*chunkFrames=*/64, /*loopable=*/true,
        /*loopStart=*/70, /*loopEnd=*/190, /*loopCount=*/3,
        /*maxServeFrames=*/320);
}

TEST(StreamPlayback, StreamSubregionLoopSeamIsExactAtChunkBoundary) {
    const std::vector<float> source = MakeDistinctSignal(260);
    ExpectStreamLoopMatchesExplicitConcat(
        "subregion_boundary", source, /*chunkFrames=*/64,
        /*loopable=*/true, /*loopStart=*/32, /*loopEnd=*/192,
        /*loopCount=*/2);
}

TEST(StreamPlayback, NonLoopableStreamLoopCountRepeatsWholeFile) {
    const std::vector<float> source = MakeDistinctSignal(150);
    ExpectStreamLoopMatchesExplicitConcat(
        "nonloopable_whole", source, /*chunkFrames=*/64,
        /*loopable=*/false, /*loopStart=*/0, /*loopEnd=*/0,
        /*loopCount=*/4);
}

TEST(StreamPlayback, InfiniteStreamLoopSoakStaysAliveAndFed) {
    NotificationLog log;
    constexpr std::uint32_t kRequestedRingFrames = 300;
    constexpr std::uint32_t kBlocks = 320;
    EngineSession session(MakeStreamConfig(&log, /*maxVoices=*/0,
                                           kRequestedRingFrames,
                                           /*maxStreams=*/1,
                                           /*prebufferMs=*/1,
                                           /*starvationCooldownBlocks=*/4));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    engine->streams.SetMaxServeFramesForTest(512);

    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(7301);
    spec.sampleRate = kRate;
    spec.channelCount = 1;
    spec.chunkFrames = 48;
    spec.loopable = true;
    spec.loopStartFrame = 0;
    spec.loopEndFrame = 0;
    const std::vector<float> source(96, 0.125f);
    const std::string path = TempPath("stream_infinite_soak.stream");
    WriteFileBytes(path, BuildStreamBytes(spec, source));
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(7301), path.c_str()));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(7301));
    play.loopCount = 0;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));

    for (std::uint32_t block = 0; block < kBlocks; ++block) {
        engine->streams.SyncServe();
        const float peak = PumpBlockPeak(engine);
        ASSERT_EQ(0.125f, peak) << "block " << block;
        ASSERT_NE(nullptr, engine->voices.Resolve(harkentest::RenderHandle(
            engine, voice)))
            << "infinite stream ended at block " << block;
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_TRUE(log.OfType(VN_NOTIFY_VOICE_ENDED).empty());
    EXPECT_TRUE(log.OfType(VN_NOTIFY_VOICE_STARVED).empty());

    VnSnapshot snapshot = Snap();
    EXPECT_EQ(0u, snapshot.voiceStarvedCount);
    EXPECT_LE(snapshot.streamHighWater, 1u);
    EXPECT_LE(snapshot.streamRingHighWaterFrames,
              RoundUpPow2ForTest(kRequestedRingFrames))
        << "stream rings allocate power-of-two capacity";
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(StreamPlayback, HealthyStreamLoopsDoNotStarveOrReprepare) {
    NotificationLog log;
    EngineSession session(MakeStreamConfig(&log, /*maxVoices=*/0,
                                           /*ringFrames=*/384,
                                           /*maxStreams=*/0,
                                           /*prebufferMs=*/1,
                                           /*starvationCooldownBlocks=*/4));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    engine->streams.SetMaxServeFramesForTest(512);

    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(7302);
    spec.sampleRate = kRate;
    spec.channelCount = 1;
    spec.chunkFrames = 64;
    spec.loopable = true;
    spec.loopStartFrame = 50;
    spec.loopEndFrame = 130;
    const std::vector<float> source = MakeDistinctSignal(180);
    const std::string path = TempPath("stream_no_starve_loop.stream");
    WriteFileBytes(path, BuildStreamBytes(spec, source));
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(7302), path.c_str()));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(7302));
    play.loopCount = 40;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));

    bool sawActive = false;
    for (std::uint32_t block = 0; block < 12; ++block) {
        engine->streams.SyncServe();
        EXPECT_GT(PumpBlockPeak(engine), harkentest::kSilenceFloor)
            << "block " << block;
        VnSnapshot snapshot = Snap();
        const std::uint32_t state =
            snapshot.voices[RowOf(snapshot, voice)].state;
        if (state == VN_VOICE_STATE_ACTIVE) {
            sawActive = true;
        }
        if (sawActive) {
            EXPECT_NE(static_cast<std::uint32_t>(VN_VOICE_STATE_PREPARING),
                      state)
                << "prebuffer gate re-entered at block " << block;
        }
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_TRUE(log.OfType(VN_NOTIFY_VOICE_STARVED).empty());
    EXPECT_EQ(0u, Snap().voiceStarvedCount);
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(StreamPlayback, InfiniteStreamLoopImmediateStopDoesNotResurrect) {
    NotificationLog log;
    EngineSession session(MakeStreamConfig(&log, /*maxVoices=*/0,
                                           /*ringFrames=*/384,
                                           /*maxStreams=*/0,
                                           /*prebufferMs=*/1));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string path =
        WriteSineStream("stream_stop_infinite.stream", harkentest::ObjectId(7303), 256, 64);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(7303), path.c_str()));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(7303));
    play.loopCount = 0;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    engine->streams.SyncServe();
    EXPECT_GT(PumpBlockPeak(engine), harkentest::kSilenceFloor);

    ASSERT_EQ(VN_OK, Vn_Stop(voice, 0));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->voices.Resolve(harkentest::RenderHandle(engine,
        voice)));
    EXPECT_EQ(1u, log.OfType(VN_NOTIFY_VOICE_ENDED).size());

    for (int i = 0; i < 4; ++i) {
        engine->streams.SyncServe();
        EXPECT_EQ(0.0f, PumpBlockPeak(engine));
    }
    EXPECT_EQ(nullptr, engine->voices.Resolve(harkentest::RenderHandle(engine,
        voice)));
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(StreamPlayback, InfiniteStreamLoopFadeStopDoesNotResurrect) {
    NotificationLog log;
    EngineSession session(MakeStreamConfig(&log, /*maxVoices=*/0,
                                           /*ringFrames=*/512,
                                           /*maxStreams=*/0,
                                           /*prebufferMs=*/1));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string path =
        WriteSineStream("stream_fadestop_infinite.stream", harkentest::ObjectId(7304), 512, 64);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(7304), path.c_str()));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(7304));
    play.loopCount = 0;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    engine->streams.SyncServe();
    EXPECT_GT(PumpBlockPeak(engine), harkentest::kSilenceFloor);

    ASSERT_EQ(VN_OK, Vn_Stop(voice, 10));
    for (int i = 0; i < 6; ++i) {
        engine->streams.SyncServe();
        engine->nullBackend.Pump(1);
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->voices.Resolve(harkentest::RenderHandle(engine,
        voice)));
    EXPECT_EQ(1u, log.OfType(VN_NOTIFY_VOICE_STOPPED_BY_FADE).size());

    for (int i = 0; i < 4; ++i) {
        engine->streams.SyncServe();
        EXPECT_EQ(0.0f, PumpBlockPeak(engine));
    }
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(StreamPlayback, FiniteStreamLoopImmediateStopDoesNotFinishLater) {
    NotificationLog log;
    EngineSession session(MakeStreamConfig(&log, /*maxVoices=*/0,
                                           /*ringFrames=*/384,
                                           /*maxStreams=*/0,
                                           /*prebufferMs=*/1));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string path =
        WriteSineStream("stream_stop_finite.stream", harkentest::ObjectId(7305), 300, 64);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(7305), path.c_str()));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(7305));
    play.loopCount = 8;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    engine->streams.SyncServe();
    EXPECT_GT(PumpBlockPeak(engine), harkentest::kSilenceFloor);

    ASSERT_EQ(VN_OK, Vn_Stop(voice, 0));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->voices.Resolve(harkentest::RenderHandle(engine,
        voice)));
    EXPECT_EQ(1u, log.OfType(VN_NOTIFY_VOICE_ENDED).size());

    for (int i = 0; i < 6; ++i) {
        engine->streams.SyncServe();
        EXPECT_EQ(0.0f, PumpBlockPeak(engine));
    }
    EXPECT_EQ(1u, log.OfType(VN_NOTIFY_VOICE_ENDED).size());
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

// --- stream voice stealing (a stream incoming needs a stream slot) ---













// --- mid-source start frame (spec §7.6) ---

// Renders `blocks` blocks of a mono stream whose read cursor is seeded to
// startFrame at activation (via the stream manager's test override, since no
// public API requests a stream start offset and streams never park).
Capture RenderStreamAtStart(const std::string& name, VnSoundId soundId,
                            std::uint32_t chunkFrames,
                            const std::vector<float>& source,
                            std::uint64_t startFrame, std::uint32_t blocks) {
    StreamSpec spec;
    spec.soundId = soundId;
    spec.sampleRate = kRate;
    spec.channelCount = 1;
    spec.chunkFrames = chunkFrames;
    const std::string path = TempPath(name);
    WriteFileBytes(path, BuildStreamBytes(spec, source));

    EngineSession session(MakeStreamConfig(nullptr, /*maxVoices=*/0,
                                           /*ringFrames=*/384,
                                           /*maxStreams=*/0,
                                           /*prebufferMs=*/1,
                                           /*starvationCooldownBlocks=*/4));
    EXPECT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    engine->streams.SetMaxServeFramesForTest(512);
    EXPECT_EQ(VN_OK, harkentest::RegisterFixtureStream(soundId, path.c_str()));
    if (startFrame != 0) {
        engine->streams.SetActivationStartFrameForTest(startFrame);
    }
    VnPlayDesc play = MakePlayDesc(soundId);
    play.loopCount = 1;
    VnVoiceHandle voice = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));

    Capture capture;
    for (std::uint32_t block = 0; block < blocks; ++block) {
        engine->streams.SyncServe();
        harkentest::PumpAndCapture(engine, 1, capture);
    }
    return capture;
}

TEST(StreamPlayback, StreamStartFrameSeeksIntoSource) {
    const std::vector<float> source = MakeDistinctSignal(1200);
    constexpr std::uint32_t kChunkFrames = 64;
    constexpr std::uint64_t kOffset = kChunkFrames * 2; // 128, chunk-aligned
    constexpr std::uint32_t kBlocks = 6;

    // A stream seeked to `kOffset` must render exactly what a stream whose file
    // IS source[kOffset:] renders from frame 0 — identical pipeline (unity rate,
    // same prebuffer), so the comparison is sample-exact. This proves the
    // chunk-aligned seek and the first prebuffer fill both begin at the offset.
    const Capture seeked =
        RenderStreamAtStart("stream_startframe_seek.stream", harkentest::ObjectId(9601),
                            kChunkFrames, source, kOffset, kBlocks);
    const std::vector<float> shifted(
        source.begin() + static_cast<std::ptrdiff_t>(kOffset), source.end());
    const Capture reference = RenderStreamAtStart(
        "stream_startframe_ref.stream", harkentest::ObjectId(9602), kChunkFrames, shifted,
        /*startFrame=*/0, kBlocks);

    HARKENTEST_ASSERT_NON_SILENT(seeked.left);
    ASSERT_EQ(reference.left.size(), seeked.left.size());
    for (std::size_t i = 0; i < reference.left.size(); ++i) {
        ASSERT_EQ(reference.left[i], seeked.left[i]) << "left sample " << i;
        ASSERT_EQ(reference.right[i], seeked.right[i]) << "right sample " << i;
    }
}

void RenderFreshSourceCoordinates(bool streamed, std::uint32_t workers,
    double pitch, std::uint64_t startFrame, std::uint32_t loops,
    harken::ExactSourcePhase phase, Capture* capture, bool history = false, bool park = false) {
    auto config = MakeStreamConfig(nullptr, 2, history ? 512 : 4096, 2, 1);
    config.blockFrames = 64;
    config.renderWorkerCount = workers;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const auto id = harkentest::ObjectId(9689);
    const auto samples = MakeDistinctSignal(1024);
    const auto pan = harken::ResolvePanGains(engine.config.mixChannelLayout, 0);
    VnVoiceHandle voice = 0;
    harken::StreamManager::PreparedStream prepared;
    harken::StreamManager::StreamFacts facts;
    harken::StreamManager::ReplacementRequest request;
    const auto prepareRing = [&] {
        harken::StreamManager::PreparedToken token;
        ASSERT_EQ(VN_OK, engine.streams.BeginReplacement(request, &token));
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (engine.streams.QueryReplacement(token) == harken::StreamManager::PreparationState::Preparing &&
               std::chrono::steady_clock::now() < deadline)
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ASSERT_EQ(VN_OK, engine.streams.TakeReplacement(token, &prepared));
    };
    std::shared_ptr<const harken::PackMediaRange> range, replacementRange;
    if (streamed) {
        StreamSpec spec;
        spec.soundId = id;
        spec.sampleRate = 44100;
        spec.chunkFrames = 64;
        spec.loopable = true;
        spec.loopStartFrame = 64;
        spec.loopEndFrame = 256;
        const auto path = TempPath("stream_fresh_coordinates.stream");
        const auto bytes = BuildStreamBytes(spec, samples);
        WriteFileBytes(path, bytes);
        range = harkentest::ExternalPcmFixtureRange(bytes,
            [path](auto) { return harken::OpenStreamByteSource(path.c_str()); });
        ASSERT_NE(nullptr, range);
        ASSERT_EQ(VN_OK, engine.streams.ResolvePackSource(range, &facts));
        ASSERT_TRUE(engine.streams.AcquirePendingRef(facts.fileIndex));
        VnResult preparation = VN_OK;
        auto state = harken::StreamManager::PreparationState::Preparing;
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(4);
        while (state == harken::StreamManager::PreparationState::Preparing &&
               std::chrono::steady_clock::now() < deadline) {
            state = engine.streams.PreparePackSource(facts.fileIndex, facts.sourceGeneration,
                engine.vorbis, engine.config.maxPackDecodeStagingBytes, &preparation);
            if (state == harken::StreamManager::PreparationState::Preparing)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        ASSERT_EQ(harken::StreamManager::PreparationState::Ready, state);
        request.fileIndex = facts.fileIndex;
        request.sourceGeneration = facts.sourceGeneration;
        request.contentRevision = facts.contentRevision;
        request.targetFrame = startFrame;
        request.loopStartFrame = 64;
        request.loopEndFrame = 256;
        request.loopCount = loops;
        request.prebufferTargetFrames = 4096;
        ASSERT_NO_FATAL_FAILURE(prepareRing());
        engine.streams.ReleasePendingRef(facts.fileIndex);
        if (park) ASSERT_TRUE(engine.streams.AcquirePendingRef(facts.fileIndex));
        ASSERT_EQ(VN_OK, engine.voices.PlayStream(prepared.ring, 1, 44100, 64,
            0, pitch, 0, false, 0, 0, 0, pan, startFrame, samples.size(),
            64, 256, loops, harken::ResampleProfile::LegacyCompatibility,
            &voice, 1, UINT32_MAX, 0, nullptr, 0, true, 1, phase));
        ASSERT_TRUE(engine.voices.BindStreamRingToken(voice, prepared.ring, prepared.ringToken));
    } else {
        const auto desc = harkentest::MonoDesc(samples, 44100);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(id, &desc));
        auto pcm = engine.registry.Lookup(id);
        pcm.loop = {64, 256};
        ASSERT_EQ(VN_OK, engine.voices.Play(pcm, 0, pitch, 0, false, 0, 0, 0,
            pan, 64, 256, loops, harken::ResampleProfile::LegacyCompatibility,
            &voice, 1, UINT32_MAX, 0, true, 0, 1, true, true, phase));
    }
    if (history) engine.voices.Resolve(voice)->resampler.quality = harken::ResampleQuality::Sinc16;
    ASSERT_TRUE(engine.voices.CommitPreparedVoice(voice));
    engine.voices.ApplyPlay(voice, voice, startFrame, 0);
    for (unsigned block = 0; block < 32; ++block) {
        if (streamed) engine.streams.SyncServe();
        if (history && block == 3)
            engine.voices.Resolve(voice)->resampler.UpdateRatio(pitch * (44100.0 / 48000.0) * 1.1, 256);
        if (park && block == 4) {
            ASSERT_TRUE(engine.voices.ReserveStreamPark(voice, voice, 1,
                harken::ResampleProfile::LegacyCompatibility));
            engine.voices.ApplyStop(voice, 0, 64, engine.notifications, block,
                harken::kStopModeStreamPark, voice, 1);
        }
        harkentest::PumpAndCapture(&engine, 1, *capture);
        if (park && block == 4) {
            harken::StreamParkOutcome outcome;
            bool cancelled = false;
            ASSERT_TRUE(engine.voices.ConsumeStreamParkOutcomeExact(voice, voice, 1, &outcome, &cancelled));
            ASSERT_FALSE(cancelled);
            ASSERT_EQ(harken::StreamParkDisposition::ResumeReady, outcome.disposition);
            const auto* saved = engine.voices.ResolveParkOwnedToken(outcome.token);
            ASSERT_NE(nullptr, saved);
            EXPECT_TRUE(harken::Resampler::ValidateParkState(saved->resampler));
            EXPECT_TRUE(saved->gateOpen);
            EXPECT_EQ(harken::ResampleProfile::LegacyCompatibility, saved->profile);
            EXPECT_LE(saved->activationFrame, saved->feedFrame);
            ASSERT_GT(saved->resampler.legacyWriteIndex, saved->resampler.legacyReadIndex);
            ASSERT_GT(saved->resampler.rampRemaining, 0u);
            ASSERT_NE(0u, saved->resampler.legacyPhaseHigh);
            EXPECT_EQ(harken::ResampleQuality::Sinc16, saved->resampler.quality);
            StreamSpec changed;
            changed.soundId = id;
            changed.sampleRate = 44100;
            const auto replacementPath = TempPath("stream_park_changed_generation.stream");
            const auto replacementBytes = BuildStreamBytes(changed, std::vector<float>(1536, -0.4f));
            WriteFileBytes(replacementPath, replacementBytes);
            replacementRange = harkentest::ExternalPcmFixtureRange(replacementBytes,
                [replacementPath](auto) { return harken::OpenStreamByteSource(replacementPath.c_str()); });
            harken::StreamManager::StreamFacts current;
            ASSERT_EQ(VN_OK, engine.streams.ResolvePackSource(replacementRange, &current));
            range.reset();
            ASSERT_NE(facts.sourceGeneration, current.sourceGeneration);
            EXPECT_NE(facts.frameCount, current.frameCount);
            harken::StreamManager::StreamFacts retained;
            ASSERT_TRUE(engine.streams.FactsForFileIndex(facts.fileIndex, &retained));
            EXPECT_EQ(samples.size(), retained.frameCount);
            EXPECT_EQ(facts.sourceGeneration, retained.sourceGeneration);
            request.targetFrame = saved->feedFrame;
            request.loopCount = saved->feedLoopsRemaining;
            ASSERT_NO_FATAL_FAILURE(prepareRing());
            ASSERT_EQ(VN_OK, engine.voices.PlayStream(prepared.ring, 1, 44100, 64,
                0, saved->resampler.targetRatio / (44100.0 / 48000.0), 0, false, 0, 0, 0, pan,
                saved->feedFrame, samples.size(), 64, 256, saved->feedLoopsRemaining,
                harken::ResampleProfile::LegacyCompatibility, &voice, 1, UINT32_MAX, 0,
                &outcome.token, 0, true));
            ASSERT_TRUE(engine.voices.BindStreamRingToken(voice, prepared.ring, prepared.ringToken));
            engine.streams.ReleasePendingRef(facts.fileIndex);
            engine.voices.ReleaseParkOwnedToken(outcome.token);
            ASSERT_TRUE(engine.voices.CommitPreparedVoice(voice));
            engine.voices.ApplyPlay(voice, voice, 0, block + 1);
            ASSERT_NE(nullptr, engine.voices.Resolve(voice));
            EXPECT_EQ(samples.size(), engine.voices.Resolve(voice)->streamFrameCount);
        }
    }
    HARKENTEST_ASSERT_NON_SILENT(capture->left);
    EXPECT_EQ(loops == 0 ? 1u : 0u, engine.voices.ActiveCount());
    engine.voices.ApplyStop(voice, 0, 0, engine.notifications, 32);
    if (streamed) {
        engine.streams.SyncServe();
        EXPECT_EQ(2u, engine.streams.FreeSlotCount());
    }
}

TEST(StreamPlayback, FreshFractionalCoordinatesMatchResidentAndContinuousLinear) {
    // Same fixed, already-bound ratio and loop data as the timeline crop case.
    // Continuous equivalence is specific to Linear with unchanged downstream state.
    const double pitch = 1.25 * std::pow(2.0, 7.0 / 12.0);
    harken::SourcePhaseStep step;
    ASSERT_TRUE(harken::DecodeSourcePhaseStep(pitch * 44100.0 / 48000.0, &step));
    for (const auto workers : {0u, 2u}) {
        for (const auto initialLoops : {3u, 0u}) {
            Capture continuous;
            ASSERT_NO_FATAL_FAILURE(RenderFreshSourceCoordinates(false, workers, pitch, 0, initialLoops, {}, &continuous));
            for (const auto offset : {0u, 192u, 448u, 704u}) {
                SCOPED_TRACE(workers);
                SCOPED_TRACE(initialLoops);
                SCOPED_TRACE(offset);
                std::uint64_t consumed = 0, start = 0;
                std::uint32_t loops = 0;
                harken::ExactSourcePhase phase;
                ASSERT_TRUE(harken::TryJumpSourcePhase(step, {}, offset, &consumed, &phase));
                harken::MapLoopedSourceFrame(0, initialLoops, consumed, 1024, 64, 256, &start, &loops);
                Capture resident, stream;
                ASSERT_NO_FATAL_FAILURE(RenderFreshSourceCoordinates(false, workers, pitch, start, loops, phase, &resident));
                ASSERT_NO_FATAL_FAILURE(RenderFreshSourceCoordinates(true, workers, pitch, start, loops, phase, &stream));
                ASSERT_EQ(resident.left, stream.left);
                ASSERT_EQ(resident.right, stream.right);
                for (std::size_t i = 0; i + offset < continuous.left.size(); ++i) {
                    ASSERT_EQ(continuous.left[i + offset], stream.left[i]) << i;
                    ASSERT_EQ(continuous.right[i + offset], stream.right[i]) << i;
                }
            }
        }
    }
}

TEST(StreamPlayback, FractionalNeighbourAcrossChunkAndLoopSeamMatchesResident) {
    const harken::ExactSourcePhase half{1ull << 63, 0};
    for (const auto start : {63u, 255u, 777u}) {
        for (const auto loops : {3u, 1u, 0u}) {
            if (start > 256 && loops != 1) continue;
            SCOPED_TRACE(start);
            SCOPED_TRACE(loops);
            Capture resident, stream;
            ASSERT_NO_FATAL_FAILURE(RenderFreshSourceCoordinates(false, 0, 1, start, loops, half, &resident));
            ASSERT_NO_FATAL_FAILURE(RenderFreshSourceCoordinates(true, 0, 1, start, loops, half, &stream));
            ASSERT_EQ(resident.left, stream.left);
            ASSERT_EQ(resident.right, stream.right);
        }
    }
}

TEST(StreamPlayback, ParkedHistoryMatchesContinuousOutputFromPinnedSource) {
    for (const auto workers : {0u, 2u}) {
        for (const auto loops : {3u, 0u}) {
            SCOPED_TRACE(workers);
            SCOPED_TRACE(loops);
            Capture continuous, resumed;
            const harken::ExactSourcePhase half{1ull << 63, 0};
            ASSERT_NO_FATAL_FAILURE(RenderFreshSourceCoordinates(true, workers, 1.2345, 0, loops,
                half, &continuous, true));
            ASSERT_NO_FATAL_FAILURE(RenderFreshSourceCoordinates(true, workers, 1.2345, 0, loops,
                half, &resumed, true, true));
            ASSERT_EQ(continuous.left.size(), resumed.left.size());
            // The park transition itself fades one block. After its captured
            // endpoint, restored history follows the same ongoing ratio ramp.
            for (std::size_t i = 5 * 64; i < continuous.left.size(); ++i) {
                ASSERT_EQ(continuous.left[i], resumed.left[i]) << i;
                ASSERT_EQ(continuous.right[i], resumed.right[i]) << i;
            }
        }
    }
}

VnConfig MakeStreamWaitConfig(NotificationLog* log,
                              std::uint32_t maxVoices = 1,
                              std::uint32_t maxStreams = 1,
                              std::uint32_t maxParked = 1) {
    VnConfig config = MakeStreamConfig(
        log, maxVoices, /*ringFrames=*/512, maxStreams,
        /*prebufferMs=*/1, /*starvationCooldownBlocks=*/2);
    config.maxParkedVoices = maxParked;
    config.audibilityParkEnabled = 1;
    config.voiceStealPolicy = 2;
    return config;
}

VnResult PlayDefault(VnSoundId soundId, VnVoiceHandle* outVoice) {
    VnPlayDesc desc = MakePlayDesc(soundId);
    return harkentest::PlayRaw(&desc, outVoice);
}

struct ParkCheckpoint {
    std::uint64_t frame = 0;
    std::uint32_t loopsRemaining = 0;
};

std::uint64_t FiniteLoopLinearAdvance(
    std::uint64_t frame, std::uint32_t loopsRemaining,
    std::uint32_t initialLoops, std::uint64_t loopStart,
    std::uint64_t loopEnd) {
    const std::uint32_t completed = initialLoops - loopsRemaining;
    if (completed == 0) {
        return frame;
    }
    return loopEnd +
           static_cast<std::uint64_t>(completed - 1u) *
               (loopEnd - loopStart) +
           (frame - loopStart);
}

void AdvanceFiniteLoopCoordinate(
    std::uint64_t* frame, std::uint32_t* loopsRemaining,
    std::uint64_t loopStart, std::uint64_t loopEnd) {
    ++*frame;
    if (*frame == loopEnd && *loopsRemaining > 1u) {
        *frame = loopStart;
        --*loopsRemaining;
    }
}

ParkCheckpoint CaptureFiniteParkCheckpoint(std::uint32_t renderedBlocks) {
    EngineSession session(MakeStreamWaitConfig(nullptr));
    EXPECT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(9691);
    spec.sampleRate = kRate;
    spec.channelCount = 1;
    spec.chunkFrames = 128;
    spec.loopable = true;
    spec.loopStartFrame = 512;
    spec.loopEndFrame = 1024;
    const std::string path = TempPath("stream_wait_checkpoint_phase.stream");
    WriteFileBytes(path, BuildStreamBytes(spec, MakeDistinctSignal(4096)));
    EXPECT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(9691), path.c_str()));
    const std::vector<float> pcmData(48000, 0.2f);
    VnPCMDesc pcm = harkentest::MonoDesc(pcmData, kRate);
    EXPECT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(9692), &pcm));

    VnPlayDesc looped = MakePlayDesc(harkentest::ObjectId(9691));
    looped.loopCount = 3;
    VnVoiceHandle stream = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&looped, &stream));
    for (std::uint32_t i = 0; i < renderedBlocks; ++i) {
        engine->streams.SyncServe();
        engine->nullBackend.Pump(1);
    }
    VnVoiceHandle claimant = 0;
    EXPECT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9692), &claimant));
    engine->nullBackend.Pump(1);
    EXPECT_EQ(VN_OK, Vn_Update());
    const harken::VoiceLedger::Resolved waiting =
        engine->ledger.Classify(stream);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Parked, waiting.kind);
    if (waiting.kind != harken::VoiceLedger::HandleKind::Parked) {
        return {};
    }
    const harken::ParkRecord& rec =
        engine->parkRecords[waiting.ledgerSlot];
    return {rec.streamResumeFrame, rec.loopsRemaining};
}

TEST(StreamPlayback, PressureWaitKeepsHandleAndReturnsAtAcknowledgedFrame) {
    NotificationLog log;
    EngineSession session(MakeStreamWaitConfig(&log));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::string path =
        WriteSineStream("stream_wait_resume.stream", harkentest::ObjectId(9701), 48000);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(9701), path.c_str()));
    const std::vector<float> pcmData =
        harkentest::MakeSine(48000, 700.0, kRate, 0.25f);
    VnPCMDesc pcm = harkentest::MonoDesc(pcmData, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(9702), &pcm));

    VnVoiceHandle stream = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9701), &stream));
    engine->streams.SyncServe();
    engine->nullBackend.Pump(3);

    VnVoiceHandle claimant = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9702), &claimant));
    const harken::VoiceLedger::Resolved waiting =
        engine->ledger.Classify(stream);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, waiting.kind);
    ASSERT_EQ(harken::ParkRecord::State::StreamAckPending,
              engine->parkRecords[waiting.ledgerSlot].state);
    EXPECT_EQ(harken::ResampleProfile::LegacyCompatibility,
              engine->parkRecords[waiting.ledgerSlot]
                  .facts.resampleProfile);
    EXPECT_EQ(0u, engine->voiceStolenCount);
    EXPECT_EQ(1u, engine->ledger.ParkedCount());

    engine->nullBackend.Pump(1);
    engine->streams.SyncServe();
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(harken::ParkRecord::State::StreamResumeReady,
              engine->parkRecords[waiting.ledgerSlot].state);
    const std::uint64_t acknowledgedFrame =
        engine->parkRecords[waiting.ledgerSlot].streamResumeFrame;
    EXPECT_GT(acknowledgedFrame, 0u);
    const harken::StreamParkTokenRef resumeToken =
        engine->parkRecords[waiting.ledgerSlot].streamParkToken;
    const harken::StreamParkToken* saved =
        engine->voices.ResolveParkOwnedToken(resumeToken);
    ASSERT_NE(nullptr, saved);
    EXPECT_EQ(harken::ResampleProfile::LegacyCompatibility,
              saved->resampler.profile);
    EXPECT_TRUE(harken::Resampler::ValidateParkState(saved->resampler));
    EXPECT_EQ(0u, saved->activationFrame);
    EXPECT_EQ(acknowledgedFrame, saved->feedFrame);
    EXPECT_TRUE(saved->gateOpen);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(claimant).kind);

    engine->streams.SetThrottled(true);
    ASSERT_EQ(VN_OK, Vn_Stop(claimant, 0));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const harken::VoiceLedger::Resolved returned =
        engine->ledger.Classify(stream);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live, returned.kind);
    harken::VoiceRenderContext* ctx =
        engine->voices.Resolve(returned.renderHandle);
    ASSERT_NE(nullptr, ctx);
    EXPECT_EQ(harken::ResampleProfile::LegacyCompatibility,
              ctx->resampler.Profile());
    EXPECT_EQ(0u, ctx->streamActivationStartFrame);
    EXPECT_TRUE(ctx->streamGateOpen);

    engine->nullBackend.Pump(1);
    const VnSnapshot resumed = Snap();
    EXPECT_EQ(static_cast<std::uint32_t>(VN_VOICE_STATE_ACTIVE),
              resumed.voices[RowOf(resumed, stream)].state);
    EXPECT_TRUE(log.OfType(VN_NOTIFY_VOICE_STOLEN).empty());
    ASSERT_EQ(1u, log.OfType(VN_NOTIFY_VOICE_PARKED).size());
    ASSERT_EQ(1u, log.OfType(VN_NOTIFY_VOICE_RETURNED).size());

    engine->streams.SetThrottled(false);
    engine->streams.SyncServe();
    EXPECT_GT(PumpBlockPeak(engine), harkentest::kSilenceFloor);
}

TEST(StreamPlayback,
     PressureWaitRetainsAttenuationGenerationAfterProfileReplacement) {
    EngineSession session(MakeStreamWaitConfig(nullptr));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const auto streamId = harkentest::ObjectId(9703);
    const auto claimantId = harkentest::ObjectId(9704);
    const auto attenuationId = harkentest::ObjectId(9705);
    const std::string path =
        WriteSineStream("stream_wait_attenuation_generation.stream", streamId,
                        48000);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(streamId, path.c_str()));
    const std::vector<float> pcmData(48000, 0.2f);
    const VnPCMDesc pcm = harkentest::MonoDesc(pcmData, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(claimantId, &pcm));

    const auto oldProfile = harkentest::MakeAttenuationDesc(1.0f, 101.0f);
    const harken::Spatializer::PackProfileInput oldInput{attenuationId,
                                                       &oldProfile};
    harken::Spatializer::PreparedProfileSet first;
    ASSERT_EQ(VN_OK,
              engine->spatial.PreparePackProfiles(&oldInput, 1, 1, &first));
    engine->spatial.CommitPreparedSet(&first);
    ASSERT_EQ(VN_OK, Vn_SetListener(harkentest::Vec3(0.0f, 0.0f, -50.0f),
                                    harkentest::Vec3(0.0f, 0.0f, -1.0f),
                                    harkentest::Vec3(0.0f, 1.0f, 0.0f)));
    const auto play =
        harkentest::MakePlayDesc3D(streamId, 0.0f, 0.0f, -101.0f);
    VnVoiceHandle stream = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &stream));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(stream, attenuationId));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->streams.SyncServe();
    engine->nullBackend.Pump(2);

    const auto nextProfile = harkentest::MakeAttenuationDesc(1.0f, 201.0f);
    const harken::Spatializer::PackProfileInput nextInput{attenuationId,
                                                        &nextProfile};
    harken::Spatializer::PreparedProfileSet replacement;
    ASSERT_EQ(VN_OK, engine->spatial.PreparePackProfiles(
                         &nextInput, 1, 2, &replacement, {{attenuationId, 1}}));
    engine->spatial.CommitPreparedSet(&replacement);
    VnVoiceHandle claimant = 0;
    ASSERT_EQ(VN_OK, PlayDefault(claimantId, &claimant));
    const auto waiting = engine->ledger.Classify(stream);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, waiting.kind);
    const auto& rec = engine->parkRecords[waiting.ledgerSlot];
    EXPECT_EQ(1u, rec.spatial.attenuationGeneration);
    // Remove the original distance term, even though the current curve differs.
    EXPECT_NEAR(0.0f, rec.frozenNonSpatialDb, 1e-4f);

    ASSERT_EQ(VN_OK, Vn_SetListener(harkentest::Vec3(0.0f, 0.0f, -25.0f),
                                    harkentest::Vec3(0.0f, 0.0f, -1.0f),
                                    harkentest::Vec3(0.0f, 1.0f, 0.0f)));
    engine->nullBackend.Pump(1);
    engine->streams.SyncServe();
    ASSERT_EQ(VN_OK, Vn_Update());
    const auto& requests = engine->voiceManager.Requests();
    const auto request = std::find_if(
        requests.begin(), requests.end(),
        [stream](const harken::VoiceManager::RenderRequest& value) {
            return value.publicHandle == stream;
        });
    ASSERT_NE(requests.end(), request);
    EXPECT_EQ(harken::VoiceManager::Residency::RenderRequired,
              request->residency);
    // At distance 76 the retained curve has gain 0.25, the current curve 0.625.
    EXPECT_NEAR(20.0 * std::log10(0.25), request->audibilityDb, 1e-4);
}

TEST(StreamPlayback, PressureWaitUsesAppliedDistanceBeforeUnpublishedGeometryChanges) {
    enum class Change { SwitchReference, MoveReference, ClearReference,
                        DestroyReference, Listener, EmitterScale, VoiceScale, Source };
    for (const auto change : {Change::SwitchReference, Change::MoveReference,
                              Change::ClearReference, Change::DestroyReference,
                              Change::Listener, Change::EmitterScale,
                              Change::VoiceScale, Change::Source}) {
        SCOPED_TRACE(static_cast<int>(change));
        EngineSession session(MakeStreamWaitConfig(nullptr));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto* engine = session.Engine();
        const auto streamId = harkentest::ObjectId(9706);
        const auto claimantId = harkentest::ObjectId(9707);
        const auto attenuationId = harkentest::ObjectId(9708);
        const auto path = WriteSineStream("stream_wait_distance_reference.stream", streamId, 48000);
        ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(streamId, path.c_str()));
        const std::vector<float> pcmData(48000, 0.2f);
        const auto pcm = harkentest::MonoDesc(pcmData, kRate);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(claimantId, &pcm));
        auto profile = harkentest::MakeAttenuationDesc(1, 101);
        profile.coneEnabled = 1;
        profile.coneInnerDeg = 30;
        profile.coneOuterDeg = 60;
        profile.coneOuterGainDb = -12;
        profile.coneOuterLowpassHz = 20000;
        ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(attenuationId, &profile));
        const auto setListener = [](float z) {
            return Vn_SetListener(harkentest::Vec3(0, 0, z), harkentest::Vec3(0, 0, -1),
                                  harkentest::Vec3(0, 1, 0));
        };
        ASSERT_EQ(VN_OK, setListener(-52));
        VnEmitterHandle source = 0, reference = 0, farReference = 0;
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&source));
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&reference));
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&farReference));
        ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(source, harkentest::Vec3(0, 0, -51)));
        ASSERT_EQ(VN_OK, Vn_SetEmitterOrientation(source, harkentest::Vec3(0, 0, 1)));
        ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(reference, harkentest::Vec3(0, 0, -50)));
        if (change != Change::Listener)
            ASSERT_EQ(VN_OK, Vn_SetListenerDistanceReference(reference));
        auto play = harkentest::MakePlayDesc3D(streamId, 0, 0, -51);
        play.emitterHandle = source;
        VnVoiceHandle stream = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &stream));
        ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(stream, attenuationId));
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->streams.SyncServe();
        engine->nullBackend.Pump(2);
        const auto render = harkentest::RenderHandle(engine, stream);
        ASSERT_NEAR(-12, engine->params.ResolvedValue(render, harken::kTargetVoiceVolume), 1e-4f);
        float expectedGain = 0.5f;
        switch (change) {
        case Change::SwitchReference:
            ASSERT_EQ(VN_OK, Vn_SetListenerDistanceReference(farReference));
            break;
        case Change::MoveReference:
            ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(reference, harkentest::Vec3(0, 0, 0)));
            break;
        case Change::ClearReference:
            ASSERT_EQ(VN_OK, setListener(0));
            ASSERT_EQ(VN_OK, Vn_SetListenerDistanceReference(0));
            break;
        case Change::DestroyReference:
            ASSERT_EQ(VN_OK, setListener(0));
            ASSERT_EQ(VN_OK, Vn_DestroyEmitter(reference));
            break;
        case Change::Listener:
            ASSERT_EQ(VN_OK, setListener(0));
            break;
        case Change::EmitterScale:
            ASSERT_EQ(VN_OK, Vn_SetEmitterDistanceScale(source, 0.02f));
            expectedGain = 0.51f;
            break;
        case Change::VoiceScale:
            ASSERT_EQ(VN_OK, Vn_SetVoiceDistanceScale(stream, 0.02f));
            expectedGain = 0.51f;
            break;
        case Change::Source:
            ASSERT_EQ(VN_OK, Vn_SetEmitterPosition(source, harkentest::Vec3(0, 0, -101)));
            break;
        }
        // Steal without another spatial evaluation: freeze the applied cone,
        // then let the waiting request use the new distance term exactly once.
        ASSERT_NEAR(-12, engine->params.ResolvedValue(render, harken::kTargetVoiceVolume), 1e-4f);
        VnVoiceHandle claimant = 0;
        ASSERT_EQ(VN_OK, PlayDefault(claimantId, &claimant));
        const auto waiting = engine->ledger.Classify(stream);
        ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, waiting.kind);
        EXPECT_NEAR(-12, engine->parkRecords[waiting.ledgerSlot].frozenNonSpatialDb, 1e-4f);
        engine->nullBackend.Pump(1);
        engine->streams.SyncServe();
        ASSERT_EQ(VN_OK, Vn_Update());
        const auto& requests = engine->voiceManager.Requests();
        const auto request = std::find_if(requests.begin(), requests.end(),
            [stream](const harken::VoiceManager::RenderRequest& value) {
                return value.publicHandle == stream;
            });
        ASSERT_NE(requests.end(), request);
        EXPECT_NEAR(-12 + 20 * std::log10(expectedGain), request->audibilityDb, 1e-4);
    }
}



TEST(StreamPlayback,
     ParkTokenKeepsFiniteLoopAudibleAndFeedFrontiersOrdered) {
    VnConfig config = MakeStreamWaitConfig(nullptr);
    config.streamRingBytes = (1024) * sizeof(float);
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    engine->streams.SetMaxServeFramesForTest(1024);

    constexpr std::uint32_t kInitialLoops = 4;
    constexpr std::uint64_t kLoopStart = 64;
    constexpr std::uint64_t kLoopEnd = 192;
    const std::vector<float> source = MakeDistinctSignal(2048);
    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(9703);
    spec.sampleRate = kRate;
    spec.channelCount = 1;
    spec.chunkFrames = 64;
    spec.loopable = true;
    spec.loopStartFrame = kLoopStart;
    spec.loopEndFrame = kLoopEnd;
    const std::string path =
        TempPath("stream_wait_frontiers.stream");
    WriteFileBytes(path, BuildStreamBytes(spec, source));
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(9703), path.c_str()));

    const std::vector<float> pcmData(48000, 0.2f);
    VnPCMDesc pcm = harkentest::MonoDesc(pcmData, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(9704), &pcm));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(9703));
    play.loopCount = kInitialLoops;
    VnVoiceHandle stream = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &stream));
    engine->streams.SyncServe();
    engine->nullBackend.Pump(1);
    engine->streams.SyncServe();

    const harken::VoiceLedger::Resolved live =
        engine->ledger.Classify(stream);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live, live.kind);
    harken::VoiceRenderContext* beforePark =
        engine->voices.Resolve(live.renderHandle);
    ASSERT_NE(nullptr, beforePark);
    ASSERT_NE(nullptr, beforePark->streamRing);
    const std::uint64_t producerWrite =
        beforePark->streamRing->writeIndex.load(
            std::memory_order_relaxed);
    const std::uint64_t consumerRead =
        beforePark->streamRing->readIndex.load(
            std::memory_order_relaxed);
    ASSERT_GT(producerWrite, consumerRead);

    VnVoiceHandle claimant = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9704), &claimant));
    engine->nullBackend.Pump(1);
    engine->streams.SyncServe();
    ASSERT_EQ(VN_OK, Vn_Update());
    const harken::VoiceLedger::Resolved waiting =
        engine->ledger.Classify(stream);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, waiting.kind);
    const harken::ParkRecord& record =
        engine->parkRecords[waiting.ledgerSlot];
    ASSERT_EQ(harken::ParkRecord::State::StreamResumeReady,
              record.state);
    const harken::StreamParkToken* token =
        engine->voices.ResolveParkOwnedToken(record.streamParkToken);
    ASSERT_NE(nullptr, token);
    const harken::StreamParkToken saved = *token;

    EXPECT_EQ(0u, saved.activationFrame);
    EXPECT_EQ(kInitialLoops, saved.activationLoopsRemaining);
    EXPECT_LT(saved.audibleLoopsRemaining, kInitialLoops);
    EXPECT_LE(saved.feedLoopsRemaining,
              saved.audibleLoopsRemaining);
    const std::uint64_t audibleAdvance =
        FiniteLoopLinearAdvance(
            saved.audibleFrame, saved.audibleLoopsRemaining,
            kInitialLoops, kLoopStart, kLoopEnd);
    const std::uint64_t feedAdvance =
        FiniteLoopLinearAdvance(
            saved.feedFrame, saved.feedLoopsRemaining,
            kInitialLoops, kLoopStart, kLoopEnd);
    EXPECT_EQ(saved.resampler.legacyReadIndex -
                  harken::kResampleHistoryPad,
              audibleAdvance);
    EXPECT_EQ(saved.resampler.legacyWriteIndex -
                  harken::kResampleHistoryPad,
              feedAdvance);
    EXPECT_GT(feedAdvance, audibleAdvance);
    EXPECT_GT(producerWrite, feedAdvance);

    ASSERT_EQ(VN_OK, Vn_Stop(claimant, 0));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const harken::VoiceLedger::Resolved returned =
        engine->ledger.Classify(stream);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live, returned.kind);
    harken::VoiceRenderContext* restored =
        engine->voices.Resolve(returned.renderHandle);
    ASSERT_NE(nullptr, restored);
    EXPECT_EQ(saved.activationFrame,
              restored->streamActivationStartFrame);
    EXPECT_EQ(saved.activationLoopsRemaining,
              restored->streamActivationLoopsRemaining);
    EXPECT_EQ(saved.feedFrame,
              restored->streamFeedActivationStartFrame);
    EXPECT_EQ(saved.feedLoopsRemaining,
              restored->streamFeedActivationLoopsRemaining);
    EXPECT_EQ(saved.resampler.legacyReadIndex,
              restored->resampler.SourceFrame());
    EXPECT_DOUBLE_EQ(saved.resampler.legacyReadFraction,
                     restored->resampler.SourceFraction());

    engine->streams.SyncServe();
    const std::uint32_t span = restored->streamRing->BufferedFrames();
    ASSERT_GT(span, 0u);
    std::uint64_t expectedFrame = saved.feedFrame;
    std::uint32_t expectedLoops = saved.feedLoopsRemaining;
    const std::uint32_t inspected = std::min(32u, span);
    for (std::uint32_t i = 0; i < inspected; ++i) {
        EXPECT_FLOAT_EQ(source[expectedFrame], harkentest::PeekPcmSample(*restored->streamRing, i))
            << "reread frame " << i;
        AdvanceFiniteLoopCoordinate(
            &expectedFrame, &expectedLoops, kLoopStart, kLoopEnd);
    }

    engine->nullBackend.Pump(1);
    engine->streams.SyncServe();
    VnVoiceHandle secondClaimant = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9704), &secondClaimant));
    engine->nullBackend.Pump(1);
    engine->streams.SyncServe();
    ASSERT_EQ(VN_OK, Vn_Update());
    const harken::VoiceLedger::Resolved waitingAgain =
        engine->ledger.Classify(stream);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked,
              waitingAgain.kind);
    const harken::ParkRecord& secondRecord =
        engine->parkRecords[waitingAgain.ledgerSlot];
    ASSERT_EQ(harken::ParkRecord::State::StreamResumeReady,
              secondRecord.state);
    const harken::StreamParkToken* secondToken =
        engine->voices.ResolveParkOwnedToken(
            secondRecord.streamParkToken);
    ASSERT_NE(nullptr, secondToken);
    const std::uint64_t secondFeedAdvance =
        FiniteLoopLinearAdvance(
            secondToken->feedFrame,
            secondToken->feedLoopsRemaining, kInitialLoops,
            kLoopStart, kLoopEnd);
    ASSERT_GE(secondToken->resampler.legacyWriteIndex,
              saved.resampler.legacyWriteIndex);
    const std::uint64_t rereadFramesConsumed =
        secondToken->resampler.legacyWriteIndex -
        saved.resampler.legacyWriteIndex;
    EXPECT_GT(rereadFramesConsumed, 0u);
    EXPECT_EQ(feedAdvance + rereadFramesConsumed,
              secondFeedAdvance);
    EXPECT_GT(secondFeedAdvance, feedAdvance);
}



TEST(StreamPlayback,
     CancelledParkTokenStaysReservedUntilPublishedOutcomeIsConsumed) {
    EngineSession session(MakeStreamWaitConfig(
        nullptr, /*maxVoices=*/2, /*maxStreams=*/2,
        /*maxParked=*/1));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::string path =
        WriteSineStream("stream_wait_cancelled_token.stream", harkentest::ObjectId(9713), 48000);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(9713), path.c_str()));

    VnVoiceHandle first = 0;
    VnVoiceHandle second = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9713), &first));
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9713), &second));
    engine->streams.SyncServe();
    engine->nullBackend.Pump(1);
    const harken::VoiceLedger::Resolved firstLive =
        engine->ledger.Classify(first);
    const harken::VoiceLedger::Resolved secondLive =
        engine->ledger.Classify(second);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live, firstLive.kind);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live, secondLive.kind);

    constexpr std::uint64_t kFirstSequence = 91;
    constexpr std::uint64_t kSecondSequence = 92;
    EXPECT_FALSE(engine->voices.HasPendingStreamParkMailboxes());
    ASSERT_TRUE(engine->voices.ReserveStreamPark(
        firstLive.renderHandle, first, kFirstSequence,
        harken::ResampleProfile::Professional44148));
    EXPECT_TRUE(engine->voices.HasPendingStreamParkMailboxes());
    ASSERT_EQ(harken::StreamParkCancelResult::Cancelled,
              engine->voices.CancelStreamPark(
                  firstLive.renderHandle, first, kFirstSequence));
    EXPECT_FALSE(engine->voices.ReserveStreamPark(
        secondLive.renderHandle, second, kSecondSequence,
        harken::ResampleProfile::Professional44148));
    EXPECT_TRUE(engine->voices.HasPendingStreamParkMailboxes());

    engine->voices.ApplyStop(
        firstLive.renderHandle, 0, kBlockFrames,
        engine->notifications, 0, harken::kStopModeStreamPark,
        first, kFirstSequence);
    engine->nullBackend.Pump(1);
    EXPECT_EQ(0u, engine->voices.StreamParkCaptureBytesPeak());
    EXPECT_TRUE(engine->voices.HasPendingStreamParkMailboxes());
    EXPECT_FALSE(engine->voices.ReserveStreamPark(
        secondLive.renderHandle, second, kSecondSequence,
        harken::ResampleProfile::Professional44148));

    harken::StreamParkOutcome outcome;
    bool cancelled = false;
    ASSERT_TRUE(engine->voices.ConsumeStreamParkOutcomeExact(
        firstLive.renderHandle, first, kFirstSequence,
        &outcome, &cancelled));
    EXPECT_TRUE(cancelled);
    EXPECT_FALSE(outcome.token.IsValid());
    EXPECT_FALSE(engine->voices.HasPendingStreamParkMailboxes());
    ASSERT_TRUE(engine->voices.ReserveStreamPark(
        secondLive.renderHandle, second, kSecondSequence,
        harken::ResampleProfile::Professional44148));
    engine->voices.AbortStreamParkReservation(
        secondLive.renderHandle, second, kSecondSequence);
    EXPECT_FALSE(engine->voices.HasPendingStreamParkMailboxes());
}

TEST(StreamPlayback, PreparingVictimAcknowledgesActivationOrigin) {
    EngineSession session(MakeStreamWaitConfig(nullptr));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::string path =
        WriteSineStream("stream_wait_preparing.stream", harkentest::ObjectId(9721), 48000);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(9721), path.c_str()));
    const std::vector<float> pcmData(48000, 0.25f);
    VnPCMDesc pcm = harkentest::MonoDesc(pcmData, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(9722), &pcm));

    engine->streams.SetThrottled(true);
    VnVoiceHandle stream = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9721), &stream));
    VnVoiceHandle claimant = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9722), &claimant));
    engine->nullBackend.Pump(1);
    engine->streams.SyncServe();
    ASSERT_EQ(VN_OK, Vn_Update());
    const harken::VoiceLedger::Resolved parked =
        engine->ledger.Classify(stream);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, parked.kind);
    ASSERT_EQ(harken::ParkRecord::State::StreamResumeReady,
              engine->parkRecords[parked.ledgerSlot].state);
    EXPECT_EQ(0u,
              engine->parkRecords[parked.ledgerSlot].streamResumeFrame);
    EXPECT_EQ(0.0,
              engine->parkRecords[parked.ledgerSlot].virtualCursor);
}

TEST(StreamPlayback, StreamResumeFrameAboveDoublePrecisionStaysExact) {
    EngineSession session(MakeStreamWaitConfig(nullptr));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::string path =
        WriteSineStream("stream_wait_uint64_frame.stream", harkentest::ObjectId(9723), 48000);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(9723), path.c_str()));
    const std::vector<float> pcmData(48000, 0.2f);
    VnPCMDesc pcm = harkentest::MonoDesc(pcmData, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(9724), &pcm));

    engine->streams.SetThrottled(true);
    VnVoiceHandle stream = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9723), &stream));
    const harken::VoiceLedger::Resolved live =
        engine->ledger.Classify(stream);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live, live.kind);
    harken::VoiceRenderContext* preparing =
        engine->voices.Resolve(live.renderHandle);
    ASSERT_NE(nullptr, preparing);
    constexpr std::uint64_t kExactFrame = (1ull << 53u) + 1u;
    preparing->streamActivationStartFrame = kExactFrame;
    preparing->streamFrameCount = UINT64_MAX;

    VnVoiceHandle claimant = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9724), &claimant));
    engine->nullBackend.Pump(1);
    engine->streams.SyncServe();
    ASSERT_EQ(VN_OK, Vn_Update());
    const harken::VoiceLedger::Resolved waiting =
        engine->ledger.Classify(stream);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, waiting.kind);
    ASSERT_EQ(harken::ParkRecord::State::StreamResumeReady,
              engine->parkRecords[waiting.ledgerSlot].state);
    EXPECT_EQ(kExactFrame,
              engine->parkRecords[waiting.ledgerSlot].streamResumeFrame);
    EXPECT_EQ(0.0,
              engine->parkRecords[waiting.ledgerSlot].virtualCursor);

    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(claimant).kind);
    ASSERT_EQ(VN_OK, Vn_Stop(claimant, 0));
    engine->nullBackend.Pump(1);
    engine->streams.SyncServe();
    ASSERT_EQ(VN_OK, Vn_Update());
    const harken::VoiceLedger::Resolved returned =
        engine->ledger.Classify(stream);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live, returned.kind);
    const harken::VoiceRenderContext* returnedContext =
        engine->voices.Resolve(returned.renderHandle);
    ASSERT_NE(nullptr, returnedContext);
    EXPECT_EQ(kExactFrame,
              returnedContext->streamActivationStartFrame);
}

TEST(StreamPlayback, StreamConsumedOffsetAboveDoublePrecisionStaysExact) {
    EngineSession session(MakeStreamWaitConfig(nullptr));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::string path =
        WriteSineStream("stream_wait_uint64_consumed.stream", harkentest::ObjectId(9725), 48000);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(9725), path.c_str()));
    const std::vector<float> pcmData(48000, 0.2f);
    VnPCMDesc pcm = harkentest::MonoDesc(pcmData, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(9726), &pcm));

    VnVoiceHandle stream = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9725), &stream));
    engine->streams.SyncServe();
    engine->nullBackend.Pump(2);
    engine->streams.SetThrottled(true);
    const harken::VoiceLedger::Resolved live =
        engine->ledger.Classify(stream);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live, live.kind);
    harken::VoiceRenderContext* render =
        engine->voices.Resolve(live.renderHandle);
    ASSERT_NE(nullptr, render);
    constexpr std::uint64_t kExactOffset = (1ull << 53u) + 1u;
    render->streamActivationStartFrame = 0;
    render->streamFrameCount = UINT64_MAX;
    render->streamActivationLoopsRemaining = 1;
    ASSERT_EQ(harken::ResampleProfile::LegacyCompatibility,
              render->resampler.Profile());
    render->resampler.readIndex =
        harken::kResampleHistoryPad + kExactOffset;
    render->resampler.readFraction = 0.75;
    render->resampler.phaseHigh = UINT64_C(0xc000000000000000);
    render->resampler.phaseLow = 0;

    VnVoiceHandle claimant = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9726), &claimant));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const harken::VoiceLedger::Resolved waiting =
        engine->ledger.Classify(stream);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, waiting.kind);
    ASSERT_EQ(harken::ParkRecord::State::StreamResumeReady,
              engine->parkRecords[waiting.ledgerSlot].state);
    const harken::StreamParkTokenRef resumeToken =
        engine->parkRecords[waiting.ledgerSlot].streamParkToken;
    const harken::StreamParkToken* saved =
        engine->voices.ResolveParkOwnedToken(resumeToken);
    ASSERT_NE(nullptr, saved);
    EXPECT_EQ(harken::ResampleProfile::LegacyCompatibility,
              saved->resampler.profile);
    EXPECT_EQ(harken::kResampleHistoryPad + kExactOffset,
              saved->resampler.legacyReadIndex);
    EXPECT_DOUBLE_EQ(0.75, saved->resampler.legacyReadFraction);

    ASSERT_EQ(VN_OK, Vn_Stop(claimant, 0));
    engine->nullBackend.Pump(1);
    engine->streams.SyncServe();
    ASSERT_EQ(VN_OK, Vn_Update());
    const harken::VoiceLedger::Resolved returned =
        engine->ledger.Classify(stream);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live, returned.kind);
    const harken::VoiceRenderContext* restored =
        engine->voices.Resolve(returned.renderHandle);
    ASSERT_NE(nullptr, restored);
    EXPECT_EQ(harken::kResampleHistoryPad + kExactOffset,
              restored->resampler.SourceFrame());
    EXPECT_DOUBLE_EQ(0.75, restored->resampler.SourceFraction());
}














TEST(StreamPlayback, StopAfterPublishedAckDrainsWinnerBeforeEnding) {
    NotificationLog log;
    EngineSession session(MakeStreamWaitConfig(
        &log, /*maxVoices=*/1, /*maxStreams=*/1, /*maxParked=*/2));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::string path =
        WriteSineStream("stream_wait_published_cancel.stream", harkentest::ObjectId(9767), 48000);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(9767), path.c_str()));
    const std::vector<float> pcmData(48000, 0.2f);
    VnPCMDesc pcm = harkentest::MonoDesc(pcmData, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(9768), &pcm));

    VnVoiceHandle stream = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9767), &stream));
    engine->streams.SyncServe();
    engine->nullBackend.Pump(2);
    engine->streams.SetThrottled(true);
    const harken::VoiceLedger::Resolved live = engine->ledger.Classify(stream);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live, live.kind);
    const VnVoiceHandle parkedRenderHandle = live.renderHandle;
    VnVoiceHandle claimant = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9768), &claimant));
    for (std::uint32_t block = 0;
         block < 4 && engine->voices.Resolve(parkedRenderHandle) != nullptr;
         ++block) {
        engine->nullBackend.Pump(1);
    }
    ASSERT_EQ(nullptr, engine->voices.Resolve(parkedRenderHandle));
    const harken::VoiceLedger::Resolved waiting =
        engine->ledger.Classify(stream);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, waiting.kind);
    const harken::ParkRecord& record =
        engine->parkRecords[waiting.ledgerSlot];
    ASSERT_EQ(
        harken::StreamParkCancelResult::Published,
        engine->voices.CancelStreamPark(
            parkedRenderHandle, stream, record.streamParkRequestSequence));
    ASSERT_EQ(VN_OK, Vn_Stop(stream, 0));
    EXPECT_EQ(VN_STALE_HANDLE, Vn_Stop(stream, 0));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Stale,
              engine->ledger.Classify(stream).kind);
    EXPECT_EQ(1u, log.OfType(VN_NOTIFY_VOICE_PARKED).size());
    EXPECT_TRUE(log.OfType(VN_NOTIFY_VOICE_RETURNED).empty());
    EXPECT_EQ(1u, log.OfType(VN_NOTIFY_VOICE_ENDED).size());
}

TEST(StreamPlayback, PublishedNaturalEndWinsOverLaterFadedStop) {
    NotificationLog log;
    EngineSession session(MakeStreamWaitConfig(&log));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::string path =
        WriteSineStream("stream_wait_published_natural.stream", harkentest::ObjectId(9821), 384,
                        /*chunkFrames=*/128);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(9821), path.c_str()));
    const std::vector<float> pcmData(48000, 0.2f);
    VnPCMDesc pcm = harkentest::MonoDesc(pcmData, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(9822), &pcm));

    VnVoiceHandle stream = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9821), &stream));
    engine->streams.SyncServe();
    engine->nullBackend.Pump(1);
    VnVoiceHandle claimant = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9822), &claimant));
    engine->nullBackend.Pump(1);

    ASSERT_EQ(VN_OK, Vn_Stop(stream, 10));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Stale,
              engine->ledger.Classify(stream).kind);
    EXPECT_EQ(1u, log.OfType(VN_NOTIFY_VOICE_ENDED).size());
    EXPECT_TRUE(
        log.OfType(VN_NOTIFY_VOICE_STOPPED_BY_FADE).empty());
    EXPECT_TRUE(log.OfType(VN_NOTIFY_VOICE_PARKED).empty());
}



TEST(StreamPlayback, FiniteCheckpointCoversIntroSeamFinalAndOutro) {
    const ParkCheckpoint intro = CaptureFiniteParkCheckpoint(0);
    const ParkCheckpoint seam = CaptureFiniteParkCheckpoint(2);
    const ParkCheckpoint finalTraversal =
        CaptureFiniteParkCheckpoint(4);
    const ParkCheckpoint outro = CaptureFiniteParkCheckpoint(8);

    EXPECT_LT(intro.frame, 512u);
    EXPECT_EQ(3u, intro.loopsRemaining);
    EXPECT_GE(seam.frame, 512u);
    EXPECT_LT(seam.frame, 1024u);
    EXPECT_LE(finalTraversal.loopsRemaining, seam.loopsRemaining);
    EXPECT_GE(finalTraversal.frame, 512u);
    EXPECT_EQ(1u, outro.loopsRemaining);
    EXPECT_GE(outro.frame, 1024u);
}

TEST(StreamPlayback, FiniteLoopCheckpointKeepsRemainingPasses) {
    EngineSession session(MakeStreamWaitConfig(nullptr));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(9771);
    spec.sampleRate = kRate;
    spec.channelCount = 1;
    spec.chunkFrames = 128;
    spec.loopable = true;
    spec.loopStartFrame = 256;
    spec.loopEndFrame = 768;
    const std::vector<float> source = MakeDistinctSignal(4096);
    const std::string path = TempPath("stream_wait_finite_loop.stream");
    WriteFileBytes(path, BuildStreamBytes(spec, source));
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(9771), path.c_str()));
    const std::vector<float> pcmData(48000, 0.2f);
    VnPCMDesc pcm = harkentest::MonoDesc(pcmData, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(9772), &pcm));

    VnPlayDesc looped = MakePlayDesc(harkentest::ObjectId(9771));
    looped.loopCount = 4;
    VnVoiceHandle stream = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&looped, &stream));
    for (int i = 0; i < 6; ++i) {
        engine->streams.SyncServe();
        engine->nullBackend.Pump(1);
    }
    VnVoiceHandle claimant = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9772), &claimant));
    engine->nullBackend.Pump(1);
    engine->streams.SyncServe();
    ASSERT_EQ(VN_OK, Vn_Update());
    const harken::VoiceLedger::Resolved parked =
        engine->ledger.Classify(stream);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, parked.kind);
    const harken::ParkRecord& rec =
        engine->parkRecords[parked.ledgerSlot];
    EXPECT_GT(rec.loopsRemaining, 0u);
    EXPECT_LT(rec.loopsRemaining, 4u);
    EXPECT_GE(rec.streamResumeFrame, 256u);
    EXPECT_LT(rec.streamResumeFrame, 4096u);
}

TEST(StreamPlayback, InfiniteLoopCheckpointRemainsInfinite) {
    EngineSession session(MakeStreamWaitConfig(nullptr));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(9773);
    spec.sampleRate = kRate;
    spec.channelCount = 1;
    spec.chunkFrames = 128;
    spec.loopable = true;
    spec.loopStartFrame = 256;
    spec.loopEndFrame = 768;
    const std::string path = TempPath("stream_wait_infinite_loop.stream");
    WriteFileBytes(path, BuildStreamBytes(spec, MakeDistinctSignal(4096)));
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(9773), path.c_str()));
    const std::vector<float> pcmData(48000, 0.2f);
    VnPCMDesc pcm = harkentest::MonoDesc(pcmData, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(9774), &pcm));

    VnPlayDesc looped = MakePlayDesc(harkentest::ObjectId(9773));
    looped.loopCount = 0;
    VnVoiceHandle stream = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&looped, &stream));
    for (int i = 0; i < 7; ++i) {
        engine->streams.SyncServe();
        engine->nullBackend.Pump(1);
    }
    VnVoiceHandle claimant = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9774), &claimant));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const harken::VoiceLedger::Resolved waiting =
        engine->ledger.Classify(stream);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, waiting.kind);
    const harken::ParkRecord& rec =
        engine->parkRecords[waiting.ledgerSlot];
    EXPECT_EQ(0u, rec.loopsRemaining);
    EXPECT_GE(rec.streamResumeFrame, 256u);
    EXPECT_LT(rec.streamResumeFrame, 768u);
}

TEST(StreamPlayback, StarvationAndArenaSilenceBoundParkWithoutAdvancing) {
    EngineSession session(MakeStreamWaitConfig(nullptr));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::string path =
        WriteSineStream("stream_wait_arena_silent.stream", harkentest::ObjectId(9775), 48000);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(9775), path.c_str()));
    const std::vector<float> pcmData(48000, 0.2f);
    VnPCMDesc pcm = harkentest::MonoDesc(pcmData, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(9776), &pcm));

    VnVoiceHandle stream = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9775), &stream));
    engine->streams.SyncServe();
    engine->nullBackend.Pump(2);
    VnVoiceHandle claimant = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9776), &claimant));
    engine->streams.SetThrottled(true);
    engine->mixer.ArenaForTests().SetCapacityForTests(0);
    engine->nullBackend.Pump(1);
    engine->mixer.ArenaForTests().RestoreCapacityForTests();
    ASSERT_EQ(VN_OK, Vn_Update());

    const harken::VoiceLedger::Resolved waiting =
        engine->ledger.Classify(stream);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, waiting.kind);
    ASSERT_EQ(harken::ParkRecord::State::StreamResumeReady,
              engine->parkRecords[waiting.ledgerSlot].state);
    EXPECT_GT(engine->mixer.ArenaForTests().OverflowCount(), 0u);
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(StreamPlayback, DrainingArenaSilenceStillReleasesWithinParkDeadline) {
    EngineSession session(MakeStreamWaitConfig(nullptr));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::string path =
        WriteSineStream("stream_wait_draining_arena.stream", harkentest::ObjectId(9783), 48000);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(9783), path.c_str()));
    const std::vector<float> pcmData(48000, 0.2f);
    VnPCMDesc pcm = harkentest::MonoDesc(pcmData, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(9784), &pcm));

    VnVoiceHandle stream = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9783), &stream));
    engine->streams.SyncServe();
    engine->nullBackend.Pump(2);
    const harken::VoiceLedger::Resolved live =
        engine->ledger.Classify(stream);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live, live.kind);
    harken::VoiceRenderContext* render =
        engine->voices.Resolve(live.renderHandle);
    ASSERT_NE(nullptr, render);
    render->phase = harken::SourcePhase::Draining;
    render->lastResult.phase = harken::SourcePhase::Draining;

    VnVoiceHandle claimant = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9784), &claimant));
    engine->streams.SetThrottled(true);
    engine->mixer.ArenaForTests().SetCapacityForTests(0);
    engine->nullBackend.Pump(1);
    engine->mixer.ArenaForTests().RestoreCapacityForTests();
    ASSERT_EQ(VN_OK, Vn_Update());

    const harken::VoiceLedger::Resolved waiting =
        engine->ledger.Classify(stream);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, waiting.kind);
    EXPECT_EQ(harken::ParkRecord::State::StreamResumeReady,
              engine->parkRecords[waiting.ledgerSlot].state);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(claimant).kind);
}

TEST(StreamPlayback, NaturalEndRacingParkHasOneTerminalWinner) {
    NotificationLog log;
    EngineSession session(MakeStreamWaitConfig(&log));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::string path =
        WriteSineStream("stream_wait_natural_race.stream", harkentest::ObjectId(9785), 384,
                        /*chunkFrames=*/128);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(9785), path.c_str()));
    const std::vector<float> pcmData(48000, 0.2f);
    VnPCMDesc pcm = harkentest::MonoDesc(pcmData, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(9786), &pcm));

    VnVoiceHandle stream = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9785), &stream));
    engine->streams.SyncServe();
    engine->nullBackend.Pump(1);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(stream).kind);
    VnVoiceHandle claimant = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9786), &claimant));
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Stale,
              engine->ledger.Classify(stream).kind);
    EXPECT_TRUE(log.OfType(VN_NOTIFY_VOICE_RETURNED).empty());
    EXPECT_EQ(1u, log.OfType(VN_NOTIFY_VOICE_ENDED).size());
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1u, log.OfType(VN_NOTIFY_VOICE_ENDED).size());
}

TEST(StreamPlayback, IntegrityAbortRacingParkHasOneTerminalWinner) {
    NotificationLog log;
    EngineSession session(MakeStreamWaitConfig(&log));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    StreamSpec spec;
    spec.soundId = harkentest::ObjectId(9777);
    spec.sampleRate = kRate;
    spec.channelCount = 1;
    spec.chunkFrames = 256;
    std::vector<std::uint8_t> bytes =
        BuildStreamBytes(spec, MakeDistinctSignal(4096));
    const harken::StreamTocEntry toc = harken::ParseStreamTocEntry(
        bytes.data() + harken::kStreamHeaderBytes);
    const std::uint64_t chunkBytes =
        harken::StreamChunkBytes(spec.chunkFrames, spec.channelCount);
    bytes[static_cast<std::size_t>(harken::StreamChunkFileOffset(
        toc.payloadOffset, 1, chunkBytes))] ^= 0x01;
    const std::string path = TempPath("stream_wait_integrity_race.stream");
    WriteFileBytes(path, bytes);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(9777), path.c_str()));
    const std::vector<float> pcmData(48000, 0.2f);
    VnPCMDesc pcm = harkentest::MonoDesc(pcmData, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(9778), &pcm));

    VnVoiceHandle stream = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9777), &stream));
    engine->streams.SyncServe();
    VnVoiceHandle claimant = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9778), &claimant));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Stale,
              engine->ledger.Classify(stream).kind);
    EXPECT_TRUE(log.OfType(VN_NOTIFY_VOICE_PARKED).empty());
    EXPECT_TRUE(log.OfType(VN_NOTIFY_VOICE_RETURNED).empty());
    EXPECT_EQ(1u, log.OfType(VN_NOTIFY_VOICE_ENDED).size());
    engine->streams.SyncServe();
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1u, log.OfType(VN_NOTIFY_VOICE_ENDED).size());
}



TEST(StreamPlayback, HotReplacementKeepsParkedGenerationFacts) {
    EngineSession session(MakeStreamWaitConfig(nullptr));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::string oldPath =
        WriteSineStream("stream_wait_hot_old.stream", harkentest::ObjectId(9781), 48000);
    const std::string newPath =
        WriteSineStream("stream_wait_hot_new.stream", harkentest::ObjectId(9781), 12000);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(9781), oldPath.c_str()));
    const std::vector<float> pcmData(48000, 0.2f);
    VnPCMDesc pcm = harkentest::MonoDesc(pcmData, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(9782), &pcm));

    VnVoiceHandle stream = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9781), &stream));
    engine->streams.SyncServe();
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, harkentest::ReplaceFixtureStream(harkentest::ObjectId(9781), newPath.c_str()));
    harken::StreamManager::StreamFacts current;
    ASSERT_TRUE(engine->streams.FindFile(harkentest::ObjectId(9781), &current));
    ASSERT_EQ(12000u, current.frameCount);

    VnVoiceHandle claimant = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9782), &claimant));
    const harken::VoiceLedger::Resolved waiting =
        engine->ledger.Classify(stream);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Parked, waiting.kind);
    EXPECT_EQ(48000u,
              engine->parkRecords[waiting.ledgerSlot]
                  .facts.sourceFrameCount);
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(48000u,
              engine->parkRecords[waiting.ledgerSlot]
                  .facts.sourceFrameCount);
}






TEST(StreamPlayback, ParkCaptureAllocationFailureIsAtomic) {
    EngineSession session(MakeStreamWaitConfig(nullptr));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::string path =
        WriteSineStream("stream_wait_alloc_refusal.stream", harkentest::ObjectId(9783), 48000);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(9783), path.c_str()));
    const std::vector<float> pcmData(48000, 0.2f);
    VnPCMDesc pcm = harkentest::MonoDesc(pcmData, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(9784), &pcm));

    VnVoiceHandle stream = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9783), &stream));
    engine->streams.SyncServe();
    engine->nullBackend.Pump(1);
    const VnVoiceHandle renderBefore = engine->ledger.ToRender(stream);
    const std::uint32_t commandRoom = engine->commands.FreeSlots();
    engine->voices.SetStreamParkCommitPageLimitForTests(1);
    VnVoiceHandle claimant = 0;
    EXPECT_EQ(VN_NO_VOICE, PlayDefault(harkentest::ObjectId(9784), &claimant));
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(stream).kind);
    EXPECT_EQ(renderBefore, engine->ledger.ToRender(stream));
    EXPECT_EQ(0u, engine->ledger.ParkedCount());
    EXPECT_EQ(commandRoom, engine->commands.FreeSlots());
    EXPECT_EQ(0u, engine->voiceStolenCount);
    engine->voices.SetStreamParkCommitPageLimitForTests(SIZE_MAX);
    harken::detail::ArmLogicAllocFaultForTests(
        1, harken::detail::LogicAllocSite::ParkCaptureStorage);
    const VnResult refused = PlayDefault(harkentest::ObjectId(9784), &claimant);
    harken::detail::DisarmLogicAllocFaultForTests();
    EXPECT_EQ(VN_NO_VOICE, refused);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(stream).kind);
    EXPECT_EQ(renderBefore, engine->ledger.ToRender(stream));
    EXPECT_EQ(0u, engine->ledger.ParkedCount());
    EXPECT_EQ(commandRoom, engine->commands.FreeSlots());
    EXPECT_EQ(0u, engine->voiceStolenCount);
    EXPECT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9784), &claimant));
}



TEST(StreamPlayback, NotificationOverflowDoesNotOwnAckEligibility) {
    VnConfig config = MakeStreamWaitConfig(
        nullptr, /*maxVoices=*/3, /*maxStreams=*/1,
        /*maxParked=*/1);
    config.notificationQueueDepth = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::string path =
        WriteSineStream("stream_wait_notify_overflow.stream", harkentest::ObjectId(9794), 48000);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(9794), path.c_str()));
    const std::vector<float> pcmData(48000, 0.2f);
    VnPCMDesc pcm = harkentest::MonoDesc(pcmData, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(9795), &pcm));

    VnVoiceHandle stream = 0;
    VnVoiceHandle endingA = 0;
    VnVoiceHandle endingB = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9794), &stream));
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9795), &endingA));
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9795), &endingB));
    engine->streams.SyncServe();
    engine->nullBackend.Pump(2);
    ASSERT_EQ(VN_OK, Vn_Stop(endingA, 0));
    ASSERT_EQ(VN_OK, Vn_Stop(endingB, 0));
    VnVoiceHandle claimant = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9795), &claimant));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    const harken::VoiceLedger::Resolved waiting =
        engine->ledger.Classify(stream);
    ASSERT_NE(harken::VoiceLedger::HandleKind::Stale, waiting.kind);
    if (waiting.kind == harken::VoiceLedger::HandleKind::Parked) {
        EXPECT_EQ(harken::ParkRecord::State::StreamResumeReady,
                  engine->parkRecords[waiting.ledgerSlot].state);
    } else {
        EXPECT_EQ(harken::VoiceLedger::HandleKind::Live, waiting.kind);
    }
    EXPECT_GT(Snap().notificationDropCount, 0u);
}

TEST(StreamPlayback, SerialAndWorkerParkPublishSameCheckpoint) {
    const auto run = [](std::uint32_t workerCount) {
        VnConfig config = MakeStreamWaitConfig(nullptr);
        config.renderWorkerCount = workerCount;
        EngineSession session(config);
        EXPECT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        const std::string path =
            WriteSineStream("stream_wait_worker.stream", harkentest::ObjectId(9796), 48000);
        EXPECT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(9796), path.c_str()));
        const std::vector<float> pcmData(48000, 0.2f);
        VnPCMDesc pcm = harkentest::MonoDesc(pcmData, kRate);
        EXPECT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(9797), &pcm));
        VnVoiceHandle stream = 0;
        EXPECT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9796), &stream));
        for (int i = 0; i < 3; ++i) {
            engine->streams.SyncServe();
            engine->nullBackend.Pump(1);
        }
        VnVoiceHandle claimant = 0;
        EXPECT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9797), &claimant));
        engine->nullBackend.Pump(1);
        EXPECT_EQ(VN_OK, Vn_Update());
        const harken::VoiceLedger::Resolved waiting =
            engine->ledger.Classify(stream);
        EXPECT_EQ(harken::VoiceLedger::HandleKind::Parked, waiting.kind);
        if (waiting.kind != harken::VoiceLedger::HandleKind::Parked) {
            return std::uint64_t{0};
        }
        EXPECT_EQ(0u, harken::memguard::HitCount());
        return engine->parkRecords[waiting.ledgerSlot].streamResumeFrame;
    };

    EXPECT_EQ(run(0), run(2));
}

TEST(StreamPlayback, TermWithReservedAndPublishedAcksAllowsCleanReinit) {
    {
        EngineSession session(MakeStreamWaitConfig(nullptr));
        ASSERT_EQ(VN_OK, session.InitResult());
        harken::Engine* engine = session.Engine();
        const std::string path =
            WriteSineStream("stream_wait_term_reserved.stream", harkentest::ObjectId(9798), 48000);
        ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(9798), path.c_str()));
        const std::vector<float> pcmData(48000, 0.2f);
        VnPCMDesc pcm = harkentest::MonoDesc(pcmData, kRate);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(9799), &pcm));
        VnVoiceHandle stream = 0;
        VnVoiceHandle claimant = 0;
        ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9798), &stream));
        engine->streams.SyncServe();
        engine->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9799), &claimant));
        engine->nullBackend.Pump(1);
    }
    EngineSession reinitialized(MakeStreamWaitConfig(nullptr));
    EXPECT_EQ(VN_OK, reinitialized.InitResult());
}

TEST(StreamPlayback, IntegrityDiscoveredDuringParkFadeWinsBeforeAck) {
    EngineSession session(MakeStreamWaitConfig(nullptr));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    const std::string path =
        WriteSineStream("stream_wait_integrity_during_fade.stream", harkentest::ObjectId(9801),
                        48000);
    ASSERT_EQ(VN_OK, harkentest::RegisterFixtureStream(harkentest::ObjectId(9801), path.c_str()));

    VnVoiceHandle stream = 0;
    ASSERT_EQ(VN_OK, PlayDefault(harkentest::ObjectId(9801), &stream));
    engine->streams.SyncServe();
    engine->nullBackend.Pump(2);
    engine->streams.SetThrottled(true);
    const harken::VoiceLedger::Resolved live =
        engine->ledger.Classify(stream);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Live, live.kind);
    harken::VoiceRenderContext* render =
        engine->voices.Resolve(live.renderHandle);
    ASSERT_NE(nullptr, render);
    ASSERT_NE(nullptr, render->streamRing);

    constexpr std::uint64_t kRequestSequence = 71;
    ASSERT_TRUE(engine->voices.ReserveStreamPark(
        live.renderHandle, stream, kRequestSequence));
    engine->voices.ApplyStop(
        live.renderHandle, 0, kBlockFrames, engine->notifications, 0,
        harken::kStopModeStreamPark, stream, kRequestSequence);
    ASSERT_TRUE(render->streamRing->MarkEndOfData(
        harken::StreamTerminalReason::IntegrityAbort));
    engine->nullBackend.Pump(1);

    harken::StreamParkOutcome outcome;
    bool cancelled = false;
    ASSERT_TRUE(engine->voices.ConsumeStreamParkOutcomeExact(
        live.renderHandle, stream, kRequestSequence, &outcome, &cancelled));
    EXPECT_FALSE(cancelled);
    EXPECT_EQ(harken::StreamParkDisposition::IntegrityAbort,
              outcome.disposition);
    EXPECT_EQ(nullptr, engine->voices.Resolve(live.renderHandle));
}



TEST(StreamPlayback, StreamTerminalReasonDoesNotCrossGeneration) {
    harkentest::PcmStreamFixture ring;
    float storage[harken::kMaxChannels][16] = {};
    float* planes[harken::kMaxChannels] = {};
    for (std::uint32_t ch = 0; ch < harken::kMaxChannels; ++ch) {
        planes[ch] = storage[ch];
    }
    ring.Wire(planes, 16);
    ring.ResetForUse(1);
    ring.MarkEndOfData(harken::StreamTerminalReason::IntegrityAbort);
    EXPECT_TRUE(ring.SourceEnded());
    EXPECT_EQ(harken::StreamTerminalReason::IntegrityAbort,
              ring.TerminalReason());

    ring.ResetForUse(1);
    EXPECT_FALSE(ring.SourceEnded());
    EXPECT_EQ(harken::StreamTerminalReason::None,
              ring.TerminalReason());
    ring.MarkEndOfData(harken::StreamTerminalReason::NaturalEnd);
    EXPECT_EQ(harken::StreamTerminalReason::NaturalEnd,
              ring.TerminalReason());
}

TEST(StreamPlayback, StreamParkAndProducerTerminalHaveOneAtomicWinner) {
    harkentest::PcmStreamFixture ring;
    float storage[harken::kMaxChannels][16] = {};
    float* planes[harken::kMaxChannels] = {};
    for (std::uint32_t ch = 0; ch < harken::kMaxChannels; ++ch) {
        planes[ch] = storage[ch];
    }
    ring.Wire(planes, 16);

    ring.ResetForUse(1);
    harken::StreamTerminalReason terminal =
        harken::StreamTerminalReason::IntegrityAbort;
    ASSERT_TRUE(ring.TryClaimPark(&terminal));
    EXPECT_EQ(harken::StreamTerminalReason::None, terminal);
    EXPECT_TRUE(
        ring.MarkEndOfData(harken::StreamTerminalReason::NaturalEnd));
    EXPECT_FALSE(ring.TryCommitPark(&terminal));
    EXPECT_EQ(harken::StreamTerminalReason::NaturalEnd, terminal);
    EXPECT_TRUE(ring.SourceEnded());
    EXPECT_EQ(harken::StreamTerminalReason::NaturalEnd,
              ring.TerminalReason());

    ring.ResetForUse(1);
    ASSERT_TRUE(ring.TryClaimPark(&terminal));
    EXPECT_TRUE(ring.TryCommitPark(&terminal));
    EXPECT_EQ(harken::StreamTerminalReason::None, terminal);
    EXPECT_FALSE(
        ring.MarkEndOfData(harken::StreamTerminalReason::NaturalEnd));
    EXPECT_FALSE(ring.SourceEnded());

    ring.ResetForUse(1);
    ASSERT_TRUE(
        ring.MarkEndOfData(harken::StreamTerminalReason::IntegrityAbort));
    terminal = harken::StreamTerminalReason::None;
    EXPECT_FALSE(ring.TryClaimPark(&terminal));
    EXPECT_EQ(harken::StreamTerminalReason::IntegrityAbort, terminal);
    EXPECT_TRUE(ring.SourceEnded());
    EXPECT_EQ(harken::StreamTerminalReason::IntegrityAbort,
              ring.TerminalReason());
}

} // namespace
