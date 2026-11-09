#include "Common/TestUtil.h"
#include "Common/ObjectIdTestSupport.h"
#include "Common/VorbisFixture.h"
#include "harken/media/VorbisDecoder.h"
#include "harken/media/PackMediaRange.h"
#include <chrono>
#include <thread>

namespace {
bool PumpQuantum(harken::Engine& engine) {
    const auto before = engine.mixer.BlocksRendered();
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    do {
        engine.nullBackend.Pump(1);
        if (engine.mixer.BlocksRendered() != before) return engine.mixer.BlocksRendered() == before + 1;
        std::this_thread::yield();
    } while (std::chrono::steady_clock::now() < deadline);
    return false;
}
VnResult Start(harken::Engine& engine, const harken::PCMView& view, double pitch,
               unsigned loops, std::uint64_t first, VnVoiceHandle& voice, bool prepared = false) {
    const auto result = engine.voices.Play(view, -12.0f, pitch, 0, false, 0, 0, 0,
        harken::ResolvePanGains(engine.config.mixChannelLayout, 0.0f),
        view.loop.startFrame, view.LoopEndFrame(), loops,
        harken::ResampleProfile::LegacyCompatibility, &voice, 1, UINT32_MAX, 0, prepared);
    if (result == VN_OK && !prepared)
        engine.voices.ApplyPlay(voice, voice, first, engine.mixer.BlocksRendered());
    return result;
}
void Stop(harken::Engine& engine, VnVoiceHandle voice) {
    engine.voices.ApplyStop(voice, 0, 0, engine.notifications, engine.mixer.BlocksRendered());
    engine.vorbis.states.Collect();
}

TEST(VorbisPlayback, ResidentMatchesPcmThroughRenderPitchLoopSeekAndWorkers) {
    auto fixture = harken::test::MakeVorbisFixture(2, false, 257, 1023);
    for (unsigned workers : {0u, 2u}) {
        auto config = harkentest::MakeNullConfig(4, 64, 48000);
        config.renderWorkerCount = workers;
        harkentest::EngineSession session(config);
        ASSERT_EQ(session.InitResult(), VN_OK);
        auto& engine = *session.Engine();
        harken::VorbisPrepareResult status;
        auto source = harken::VorbisResident::Prepare(engine.vorbis, fixture.media, 4 * 1024 * 1024, status);
        ASSERT_NE(source, nullptr);
        auto encoded = source->View();
        auto pcm = encoded; pcm.encoded = nullptr; pcm.stride = 2;
        pcm.channels[0] = fixture.reference.data(); pcm.channels[1] = fixture.reference.data() + 1;
        for (double pitch : {0.75, 1.0, 1.25, 8.0}) {
            SCOPED_TRACE(::testing::Message() << workers << '/' << pitch);
            std::vector<float> expected;
            for (bool compressed : {false, true}) {
                VnVoiceHandle voices[2]{};
                const auto& view = compressed ? encoded : pcm;
                for (auto& voice : voices) ASSERT_EQ(Start(engine, view, pitch, 3, 149, voice), VN_OK);
                for (unsigned block = 0; block < 96; ++block) {
                    if (block == 48) for (auto voice : voices) engine.voices.CommitSeek(voice, 503);
                    ASSERT_TRUE(PumpQuantum(engine));
                    const auto* output = engine.nullBackend.LastBlock();
                    for (unsigned i = 0; i < 128; ++i) {
                        if (!compressed) expected.push_back(output[i]);
                        else ASSERT_NEAR(output[i], expected[block * 128 + i], 2e-7f) << block << '/' << i;
                    }
                }
                for (auto voice : voices) Stop(engine, voice);
                EXPECT_EQ(engine.vorbis.states.ActiveBytes(), 0u);
            }
            EXPECT_GT(*std::max_element(expected.begin(), expected.end()), 0.001f);
        }
    }
}

TEST(VorbisPlayback, PrefetchAndResidentVoicesStartInTheSameQuantumWithoutIo) {
    class UnopenedLocator final : public harken::StreamByteLocator {
    public:
        mutable std::atomic<unsigned> opens{0};
        std::shared_ptr<const harken::StreamByteSource> Open(harken::StreamReadCancellation) const override {
            ++opens;
            return {};
        }
    };
    const auto fixture = harken::test::MakeVorbisFixture(1, true);
    harken::VorbisMedia media;
    ASSERT_TRUE(harken::ParseVorbisTestMedia(fixture.media.data(), fixture.media.size(), media));
    ASSERT_GT(media.seek.size(), 3u);
    for (const auto workers : {0u, 2u}) {
        std::vector<float> reference;
        auto config = harkentest::MakeNullConfig(4, 64, 48000);
        config.renderWorkerCount = workers; config.maxStreams = 2;
        harkentest::EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        engine.streams.SetThrottled(true);
        harken::PCMView pcm{};
        pcm.channels[0] = fixture.reference.data(); pcm.channelCount = 1;
        pcm.sampleRate = media.sampleRate; pcm.frames = media.frames; pcm.stride = 1;
        const auto cut = media.packetDataOffset + media.seek[3].packetOffset;
        std::vector<std::uint8_t> prefix(fixture.media.begin(), fixture.media.begin() + cut);
        harken::VorbisPrepareResult prepared;
        auto encoded = harken::VorbisStreamMedia::Prepare(engine.vorbis, std::move(prefix),
            fixture.media.size(), 4 * 1024 * 1024, prepared, true);
        ASSERT_NE(nullptr, encoded);
        auto locator = std::make_shared<UnopenedLocator>();
        auto external = std::make_shared<harken::PackExternalStream>();
        external->locator = locator;
        external->fileBytes = harken::vorbis_test_wire::kStreamHeaderBytes + fixture.media.size();
        auto range = std::make_shared<harken::PackMediaRange>();
        range->external = external; range->vorbis = encoded;
        range->blobOffset = harken::vorbis_test_wire::kStreamHeaderBytes;
        range->sampleRate = media.sampleRate; range->channelCount = media.channels;
        range->frameCount = media.frames; range->contentRevision = media.revision;
        harken::StreamManager::StreamFacts facts;
        ASSERT_EQ(VN_OK, engine.streams.ResolvePackSource(range, &facts));
        for (bool streaming : {false, true}) {
            SCOPED_TRACE(::testing::Message() << workers << '/' << streaming);
            VnVoiceHandle resident = 0, second = 0;
            ASSERT_EQ(VN_OK, Start(engine, pcm, 1.0, 1, 149, resident));
            if (!streaming) {
                ASSERT_EQ(VN_OK, Start(engine, pcm, 1.0, 1, 149, second));
            } else {
                harken::StreamManager::ReplacementRequest request;
                request.fileIndex = facts.fileIndex; request.sourceGeneration = facts.sourceGeneration;
                request.contentRevision = facts.contentRevision; request.loopEndFrame = media.frames;
                request.targetFrame = 149;
                harken::StreamManager::PreparedToken token;
                ASSERT_EQ(VN_OK, engine.streams.BeginReplacement(request, &token));
                harken::StreamManager::PreparedStream stream;
                ASSERT_EQ(VN_OK, engine.streams.TransferReplacement(token, &stream));
                ASSERT_TRUE(stream.ring->startPromoted.load());
                ASSERT_EQ(VN_OK, engine.voices.PlayStream(stream.ring, 1, media.sampleRate, 0, -12.0f,
                    1.0, 0, false, 0, 0, 0,
                    harken::ResolvePanGains(engine.config.mixChannelLayout, 0.0f), 149,
                    media.frames, 0, media.frames, 1, harken::ResampleProfile::LegacyCompatibility,
                    &second, 1, UINT32_MAX, 0, nullptr, 0, false, 1, {}, encoded.get()));
                ASSERT_TRUE(engine.voices.BindStreamRingToken(second, stream.ring, stream.ringToken));
                engine.voices.ApplyPlay(second, second, 149, engine.mixer.BlocksRendered());
            }
            for (unsigned block = 0; block < 32; ++block) {
                ASSERT_TRUE(PumpQuantum(engine));
                for (unsigned sample = 0; sample < 128; ++sample) {
                    const auto value = engine.nullBackend.LastBlock()[sample];
                    if (!streaming) reference.push_back(value);
                    else ASSERT_NEAR(reference[block * 128 + sample], value, 2e-7f) << block << '/' << sample;
                }
            }
            Stop(engine, resident); Stop(engine, second);
        }
        EXPECT_EQ(0u, locator->opens.load());
        EXPECT_GT(*std::max_element(reference.begin(), reference.end()), 0.001f);
    }
}

TEST(VorbisPlayback, PreparedCancellationAndRapidStopsRecycleDecoderState) {
    auto config = harkentest::MakeNullConfig(2, 64, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    auto& engine = *session.Engine();
    const auto fixture = harken::test::MakeVorbisFixture(2);
    harken::VorbisPrepareResult status;
    auto source = harken::VorbisResident::Prepare(engine.vorbis, fixture.media, 4 * 1024 * 1024, status);
    ASSERT_NE(source, nullptr);
    const auto view = source->View();
    std::size_t backing = 0;
    for (unsigned i = 0; i < 200; ++i) {
        VnVoiceHandle voice = 0;
        ASSERT_EQ(Start(engine, view, 1.0, 1, i, voice, i % 2 == 0), VN_OK);
        if (i % 2 == 0) ASSERT_TRUE(engine.voices.CancelPreparedVoice(voice));
        else { ASSERT_TRUE(PumpQuantum(engine)); Stop(engine, voice); }
        engine.vorbis.states.Collect();
        EXPECT_EQ(engine.vorbis.states.ActiveBytes(), 0u);
        if (!i) backing = engine.vorbis.states.Bytes();
        EXPECT_EQ(engine.vorbis.states.Bytes(), backing);
    }
}

TEST(VorbisPlayback, DecoderBudgetRefusalPublishesNoVoiceOrResourcePin) {
    auto config = harkentest::MakeNullConfig(2, 64, 48000);
    config.compressedDecoderBytes = 1;
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    auto& engine = *session.Engine();
    EXPECT_EQ(engine.config.compressedDecoderBytes, 1u);
    const auto fixture = harken::test::MakeVorbisFixture(2);
    harken::VorbisPrepareResult status;
    auto source = harken::VorbisResident::Prepare(engine.vorbis, fixture.media, 4 * 1024 * 1024, status);
    ASSERT_NE(source, nullptr);
    auto view = source->View();
    std::atomic<std::uint32_t> pins{0}; view.useCount = &pins;
    VnVoiceHandle voice = 123;
    EXPECT_EQ(Start(engine, view, 1.0, 1, 0, voice), VN_NO_VOICE);
    EXPECT_EQ(voice, 123u); EXPECT_EQ(pins.load(), 0u);
    EXPECT_EQ(engine.voices.ActiveCount(), 0u); EXPECT_EQ(engine.vorbis.states.Bytes(), 0u);
}

TEST(VorbisPlayback, RegistryOwnsCompressedBytesAndRetiresPinnedGenerations) {
    const auto fixture = harken::test::MakeVorbisFixture(2);
    harken::VorbisCodec codec(2 * 1024 * 1024, 0);
    harken::PCMRegistry registry;
    const auto sound = harkentest::ObjectId(712);
    harken::PCMRegistry::PreparedPackSamples samples;
    EXPECT_EQ(registry.PrepareVorbisSamples(sound, codec, fixture.media, 1, &samples), VN_PACK_RESIDENT_CAPACITY);
    EXPECT_FALSE(samples.IsPrepared());
    ASSERT_EQ(registry.PrepareVorbisSamples(sound, codec, fixture.media, 4 * 1024 * 1024, &samples), VN_OK);
    EXPECT_LT(samples.View().encoded->ResidentBytes(), fixture.reference.size() * sizeof(float));
    harken::PCMRegistry::PackReferenceRegistration registration;
    registration.soundId = sound; registration.samples = &samples;
    harken::PCMRegistry::PreparedPackMutation mutation;
    ASSERT_EQ(registry.PreparePackMutation(&registration, 1, 0, &mutation), VN_OK);
    registry.CommitPreparedPackMutation(&mutation);
    auto old = registry.Lookup(sound);
    old.useCount->fetch_add(1);
    auto* decoder = old.encoded->Acquire(); ASSERT_NE(decoder, nullptr);
    harken::PCMRegistry::RetiredGeneration retired;
    ASSERT_TRUE(registry.ExtractRetiring(sound, registry.CookieOf(sound), &retired));
    EXPECT_FALSE(retired.Drained());
    EXPECT_FALSE(registry.Lookup(sound).Valid());
    harken::PCMView output;
    ASSERT_TRUE(decoder->Read(517, 64, 0, output));
    EXPECT_FLOAT_EQ(output.ReadSample(0, 0), fixture.reference[517 * 2]);
    decoder->Retire(); old.useCount->fetch_sub(1);
    EXPECT_TRUE(retired.Drained());
    codec.states.Collect(); EXPECT_EQ(codec.states.ActiveBytes(), 0u);
}

TEST(VorbisPlayback, AudibilityParkReleasesDecoderAndReturnUsesAdvancedPosition) {
    auto config = harkentest::MakeNullConfig(2, 64, 48000);
    config.maxTriggerDepth = 2; config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f; config.maxParkedVoices = 2;
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    auto& engine = *session.Engine();
    const auto sound = harkentest::ObjectId(713);
    const auto fixture = harken::test::MakeVorbisFixture(1);
    harken::PCMRegistry::PreparedPackSamples samples;
    ASSERT_EQ(engine.registry.PrepareVorbisSamples(sound, engine.vorbis, fixture.media,
        4 * 1024 * 1024, &samples), VN_OK);
    harken::PCMRegistry::PackReferenceRegistration registration;
    registration.soundId = sound; registration.samples = &samples;
    harken::PCMRegistry::PreparedPackMutation mutation;
    ASSERT_EQ(engine.registry.PreparePackMutation(&registration, 1, 0, &mutation), VN_OK);
    engine.registry.CommitPreparedPackMutation(&mutation);
    auto play = harkentest::MakePlayDesc(sound);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(harkentest::PlayRaw(&play, &voice), VN_OK);
    ASSERT_TRUE(PumpQuantum(engine));
    EXPECT_GT(engine.vorbis.states.ActiveBytes(), 0u);
    ASSERT_EQ(Vn_SetVoiceVolume(voice, -80.0f, 0), VN_OK);
    for (unsigned i = 0; i < 8; ++i) { ASSERT_EQ(Vn_Update(), VN_OK); ASSERT_TRUE(PumpQuantum(engine)); }
    ASSERT_EQ(engine.ledger.Classify(voice).kind, harken::VoiceLedger::HandleKind::Parked);
    ASSERT_EQ(Vn_Update(), VN_OK);
    EXPECT_EQ(engine.vorbis.states.ActiveBytes(), 0u);
    EXPECT_EQ(Vn_UnregisterPCM(sound), VN_IN_USE);
    for (unsigned i = 0; i < 4; ++i) { ASSERT_TRUE(PumpQuantum(engine)); ASSERT_EQ(Vn_Update(), VN_OK); }
    ASSERT_EQ(Vn_SetVoiceVolume(voice, 0.0f, 0), VN_OK);
    ASSERT_EQ(Vn_Update(), VN_OK); ASSERT_TRUE(PumpQuantum(engine));
    auto* render = engine.voices.Resolve(harkentest::RenderHandle(&engine, voice));
    ASSERT_NE(render, nullptr); EXPECT_NE(render->mediaDecoder, nullptr);
    EXPECT_GT(render->srcCursor, 64u);
    EXPECT_GT(engine.vorbis.states.ActiveBytes(), 0u);
    ASSERT_EQ(Vn_Stop(voice, 0), VN_OK); ASSERT_TRUE(PumpQuantum(engine));
    ASSERT_EQ(Vn_Update(), VN_OK); EXPECT_EQ(engine.vorbis.states.ActiveBytes(), 0u);
    EXPECT_EQ(Vn_UnregisterPCM(sound), VN_OK);
}
TEST(VorbisPlayback, EncodedStorageNeverAliasesRawPcmComparisons) {
    const auto fixture = harken::test::MakeVorbisFixture(1);
    harken::VorbisCodec codec(2 * 1024 * 1024, 0);
    harken::PCMRegistry registry;
    const auto sound = harkentest::ObjectId(714);
    harken::PCMRegistry::PreparedPackSamples encoded;
    ASSERT_EQ(registry.PrepareVorbisSamples(sound, codec, fixture.media,
        4 * 1024 * 1024, &encoded), VN_OK);
    harken::PCMRegistry::PreparedPackSamples identical;
    ASSERT_EQ(registry.PrepareVorbisSamples(sound, codec, fixture.media,
        4 * 1024 * 1024, &identical), VN_OK);
    ASSERT_NE(encoded.View().encoded, identical.View().encoded);
    EXPECT_TRUE(harken::EqualResidentSamples(encoded.View(), identical.View()));
    EXPECT_EQ(harken::MatchMediaContent(encoded.View(), nullptr, identical.View(), nullptr),
              harken::MediaContentMatch::Equal);
    const auto changedFixture = harken::test::MakeVorbisFixture(1, false, 257, 1023);
    harken::PCMRegistry::PreparedPackSamples different;
    ASSERT_EQ(registry.PrepareVorbisSamples(sound, codec, changedFixture.media,
        4 * 1024 * 1024, &different), VN_OK);
    EXPECT_FALSE(harken::EqualResidentSamples(encoded.View(), different.View()));
    VnPCMDesc shape{};
    shape.structSize = sizeof(shape); shape.sampleRate = 48000;
    shape.channels = 1; shape.frames = fixture.reference.size();
    shape.layout = VN_PCM_LAYOUT_PLANAR; shape.policy = VN_PCM_POLICY_COPY;
    harken::PCMRegistry::PackSampleBuilder builder;
    ASSERT_EQ(registry.BeginPackSamples(sound, &shape, &builder, encoded.View().loop, &encoded), VN_OK);
    ASSERT_EQ(builder.Append(fixture.reference.data(), fixture.reference.size()), VN_OK);
    harken::PCMRegistry::PreparedPackSamples pcm;
    ASSERT_EQ(builder.Complete(&pcm), VN_OK);
    EXPECT_EQ(pcm.View().encoded, nullptr);
    EXPECT_FLOAT_EQ(pcm.View().ReadSample(0, 517), fixture.reference[517]);
    EXPECT_FALSE(harken::EqualResidentSamples(encoded.View(), pcm.View()));
    EXPECT_FALSE(harken::EqualResidentSamples(pcm.View(), encoded.View()));
    EXPECT_EQ(harken::MatchMediaContent(encoded.View(), nullptr, pcm.View(), nullptr),
              harken::MediaContentMatch::Different);
    EXPECT_EQ(harken::MatchMediaContent(pcm.View(), nullptr, encoded.View(), nullptr),
              harken::MediaContentMatch::Different);
    harken::PackMediaRange range{};
    EXPECT_EQ(harken::MatchMediaContent(encoded.View(), nullptr, {}, &range),
              harken::MediaContentMatch::Different);
    EXPECT_EQ(harken::MatchMediaContent({}, &range, encoded.View(), nullptr),
              harken::MediaContentMatch::Different);
    harken::PCMRegistry::PackReferenceRegistration registration;
    registration.soundId = sound; registration.samples = &encoded;
    harken::PCMRegistry::PreparedPackMutation first;
    ASSERT_EQ(registry.PreparePackMutation(&registration, 1, 0, &first), VN_OK);
    registry.CommitPreparedPackMutation(&first);
    registration.samples = &pcm;
    harken::PCMRegistry::PreparedPackMutation replacement;
    ASSERT_EQ(registry.PreparePackMutation(&registration, 1, 1, &replacement), VN_OK);
    harken::PCMRegistry::RetiredGeneration retired;
    ASSERT_TRUE(registry.ExtractRetiring(sound, registry.CookieOf(sound), &retired));
    registry.CommitPreparedPackMutation(&replacement);
    EXPECT_EQ(registry.Lookup(sound).encoded, nullptr);
    EXPECT_FLOAT_EQ(registry.Lookup(sound).ReadSample(0, 517), fixture.reference[517]);
}

}
