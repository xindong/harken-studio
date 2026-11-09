#include "Common/TestUtil.h"
#include "harken/media/StreamManager.h"
#include "harken/media/VorbisStreamDecoder.h"
#include <vorbis/vorbisfile.h>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <thread>

namespace {
constexpr std::uint64_t kFrames = 144137;
constexpr unsigned kRate = 48000;

struct Source final : harken::StreamByteSource {
    std::vector<std::uint8_t> bytes;
    std::uint64_t base = (1ull << 33) + 37;
    mutable std::atomic<unsigned> reads{0};
    mutable std::atomic<bool> fail{false}, hold{false}, entered{false};
    unsigned shortRead = 997;
    std::uint64_t ByteCount() const noexcept override { return base + bytes.size() + 23; }
    harken::StreamReadResult ReadAt(std::uint64_t at, void* out, std::size_t count,
        harken::StreamReadCancellation cancel) const override {
        ++reads;
        entered.store(true);
        while (hold.load() && !cancel.IsCancelled()) std::this_thread::yield();
        if (cancel.IsCancelled()) return {harken::StreamReadStatus::Cancelled, 0};
        if (fail.load() || at < base || at - base > bytes.size() || count > bytes.size() - (at - base))
            return {harken::StreamReadStatus::IoError, 0};
        const auto n = std::min<std::size_t>(count, shortRead);
        std::memcpy(out, bytes.data() + at - base, n);
        return {harken::StreamReadStatus::Ok, n};
    }
};

std::shared_ptr<Source> Fixture(unsigned channels) {
    auto source = std::make_shared<Source>();
    const auto path = std::filesystem::path(__FILE__).parent_path().parent_path() /
        "Fixtures" / "Vorbis" / ("signal-" + std::to_string(channels) + ".ogg");
    std::ifstream file(path, std::ios::binary);
    source->bytes.assign(std::istreambuf_iterator<char>(file), {});
    EXPECT_FALSE(source->bytes.empty());
    return source;
}

std::vector<float> Linear(const Source& source, unsigned channels) {
    struct Cursor { const Source& source; std::size_t position = 0; } cursor{source};
    ov_callbacks callbacks{};
    callbacks.read_func = [](void* out, std::size_t size, std::size_t n, void* context) -> std::size_t {
        auto& c = *static_cast<Cursor*>(context);
        const auto count = std::min(size * n, c.source.bytes.size() - c.position);
        std::memcpy(out, c.source.bytes.data() + c.position, count);
        c.position += count;
        return size ? count / size : 0;
    };
    // A nonseekable linear decoder is an independent positioning reference.
    OggVorbis_File file{};
    EXPECT_EQ(0, ov_open_callbacks(&cursor, &file, nullptr, 0, callbacks));
    std::vector<float> pcm(static_cast<std::size_t>(kFrames) * channels);
    std::size_t frame = 0;
    static constexpr unsigned map6[]{0, 2, 1, 5, 3, 4};
    static constexpr unsigned map8[]{0, 2, 1, 7, 5, 6, 3, 4};
    for (;;) {
        float** samples = nullptr;
        const long count = ov_read_float(&file, &samples, 2048, nullptr);
        EXPECT_GE(count, 0);
        if (count <= 0) break;
        if (frame + count > kFrames) { ADD_FAILURE(); break; }
        for (unsigned ch = 0; ch < channels; ++ch) {
            const auto from = channels == 6 ? map6[ch] : channels == 8 ? map8[ch] : ch;
            std::copy_n(samples[from], count, pcm.data() + ch * kFrames + frame);
        }
        frame += count;
    }
    EXPECT_EQ(kFrames, frame);
    ov_clear(&file);
    return pcm;
}

std::shared_ptr<harken::PackMediaRange> Range(const std::shared_ptr<Source>& source, unsigned channels) {
    auto range = std::make_shared<harken::PackMediaRange>();
    range->codec = harken::MediaRangeCodec::Vorbis;
    range->encodedBytes = source->bytes.size();
    range->source = source;
    range->blobOffset = source->base;
    range->sampleRate = kRate;
    range->channelCount = channels;
    range->frameCount = kFrames;
    range->chunkFrames = harken::kPackMediaRangeChunkFrames;
    range->contentRevision = 71;
    return range;
}

harken::StreamManager::PreparationState Wait(harken::StreamManager& manager,
    harken::StreamManager::PreparedToken token) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
    auto state = manager.QueryReplacement(token);
    while (state == harken::StreamManager::PreparationState::Preparing &&
           std::chrono::steady_clock::now() < deadline) {
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        state = manager.QueryReplacement(token);
    }
    return state;
}

TEST(VorbisStream, ExactSeekingAndTailMatchLinearAcrossLayoutsAndLargePackOffsets) {
    for (unsigned channels : {1u, 2u, 4u, 6u, 8u}) {
        SCOPED_TRACE(channels);
        const auto source = Fixture(channels);
        const auto reference = Linear(*source, channels);
        // Each canonical input channel has a distinct authored tone. This
        // independently checks the role mapping rather than duplicating it.
        for (unsigned ch = 0; ch < channels; ++ch) {
            double sine = 0, cosine = 0;
            for (unsigned frame = 0; frame < kRate; ++frame) {
                const auto angle = 6.283185307179586 * (213 + 97 * ch) * frame / kRate;
                sine += reference[ch * kFrames + frame] * std::sin(angle);
                cosine += reference[ch * kFrames + frame] * std::cos(angle);
            }
            EXPECT_GT(2 * std::hypot(sine, cosine) / kRate, 0.01) << ch;
        }
        harken::StreamReadWindow window;
        harken::VorbisStreamDecoder decoder(*source, source->base, source->bytes.size(), window);
        ASSERT_TRUE(decoder.Open(kRate, channels, kFrames, {}));
        for (const auto frame : {0ull, 1ull, 127ull, 4095ull, 48713ull, 95019ull, 70001ull, 144000ull, 143001ull}) {
            const unsigned n = static_cast<unsigned>(std::min<std::uint64_t>(4096, kFrames - frame));
            std::vector<float> actual(n * channels);
            ASSERT_TRUE(decoder.Read(frame, n, actual.data(), n, {})) << frame;
            for (unsigned ch = 0; ch < channels; ++ch)
                for (unsigned i = 0; i < n; ++i)
                    ASSERT_NEAR(actual[ch * n + i], reference[ch * kFrames + frame + i], 1e-6f)
                        << "frame=" << frame + i << " channel=" << ch;
            ASSERT_TRUE(decoder.PrepareLoop(frame, {})) << frame;
            for (unsigned pass = 0; pass < 3; ++pass) {
                ASSERT_TRUE(decoder.Read(frame, n, actual.data(), n, {}));
                for (unsigned ch = 0; ch < channels; ++ch)
                    for (unsigned i = 0; i < n; ++i)
                        ASSERT_NEAR(actual[ch * n + i], reference[ch * kFrames + frame + i], 1e-6f);
            }
        }
        EXPECT_LE(window.capacityBytes.load(), 16u * 1024);
    }
}

TEST(VorbisStream, RejectsOversizeBadHeadersMissingTailAndIoWithoutReadingOtherPayloads) {
    for (unsigned mode = 0; mode < 5; ++mode) {
        SCOPED_TRACE(mode);
        auto source = Fixture(2);
        auto length = source->bytes.size();
        if (mode == 0) length = 1ull << 31;
        if (mode == 1) source->bytes[0] ^= 1;
        if (mode == 2) { source->bytes.resize(length - 127); length -= 127; }
        if (mode == 3) source->fail.store(true);
        if (mode == 4) source->bytes.back() ^= 1;
        harken::StreamReadWindow window;
        harken::VorbisStreamDecoder decoder(*source, source->base, length, window);
        EXPECT_FALSE(decoder.Open(kRate, 2, kFrames, {}));
        if (mode == 0) {
            EXPECT_EQ(harken::VorbisStreamDecoder::Failure::Unsupported, decoder.Error());
            EXPECT_EQ(0u, source->reads.load());
        }
        if (mode == 3) EXPECT_EQ(harken::VorbisStreamDecoder::Failure::Io, decoder.Error());
    }
}

TEST(VorbisStream, IndependentSlotsLoopAndReplacementFailureKeepOldRing) {
    harken::EngineConfig config;
    config.maxStreams = 4;
    config.streamRingFrames = 8192;
    harken::StreamManager manager(config);
    const auto source = Fixture(2);
    const auto reference = Linear(*source, 2);
    const auto range = Range(source, 2);
    harken::StreamManager::StreamFacts facts;
    ASSERT_EQ(VN_OK, manager.ResolvePackSource(range, &facts));
    harken::StreamManager::ReplacementRequest request;
    request.fileIndex = facts.fileIndex;
    request.sourceGeneration = facts.sourceGeneration;
    request.contentRevision = facts.contentRevision;
    request.targetFrame = 48013;
    request.loopStartFrame = 48013;
    request.loopEndFrame = 53119;
    request.loopCount = 3;
    request.prebufferTargetFrames = 4096;
    harken::StreamManager::PreparedToken tokens[2];
    harken::StreamManager::PreparedStream streams[2];
    ASSERT_EQ(VN_OK, manager.BeginReplacement(request, &tokens[0]));
    request.targetFrame = 79123;
    request.loopCount = 1;
    ASSERT_EQ(VN_OK, manager.BeginReplacement(request, &tokens[1]));
    for (unsigned i = 0; i < 2; ++i) {
        ASSERT_EQ(harken::StreamManager::PreparationState::Ready, Wait(manager, tokens[i]));
        ASSERT_EQ(VN_OK, manager.TakeReplacement(tokens[i], &streams[i]));
        streams[i].ring->startPromoted.store(true, std::memory_order_release);
    }
    std::uint64_t frame0 = 48013, frame1 = 79123;
    unsigned wraps = 0;
    for (unsigned batch = 0; batch < 96; ++batch) {
        manager.SyncServe();
        for (unsigned s = 0; s < 2; ++s) {
            auto& ring = *streams[s].ring;
            const float* planes[harken::kMaxChannels];
            const auto n = std::min(256u, ring.ContiguousRead(planes));
            ASSERT_GT(n, 0u);
            auto& frame = s ? frame1 : frame0;
            for (unsigned i = 0; i < n; ++i) {
                if (s == 0 && frame == 53119 && wraps < 2) { frame = 48013; ++wraps; }
                for (unsigned ch = 0; ch < 2; ++ch)
                    ASSERT_NEAR(planes[ch][i], reference[ch * kFrames + frame], 1e-6f);
                ++frame;
            }
            ring.AdvanceRead(n);
        }
    }
    const auto oldWrite = streams[0].ring->writeIndex.load();
    auto badSource = Fixture(2);
    badSource->fail.store(true);
    const auto badRange = Range(badSource, 2);
    harken::StreamManager::StreamFacts badFacts;
    ASSERT_EQ(VN_OK, manager.ResolvePackSource(badRange, &badFacts));
    request.fileIndex = badFacts.fileIndex;
    request.sourceGeneration = badFacts.sourceGeneration;
    harken::StreamManager::PreparedToken bad;
    ASSERT_EQ(VN_OK, manager.BeginReplacement(request, &bad));
    ASSERT_EQ(harken::StreamManager::PreparationState::Failed, Wait(manager, bad));
    EXPECT_GE(streams[0].ring->writeIndex.load(), oldWrite);
    EXPECT_EQ(harken::StreamSlotState::Active, streams[0].ring->state.load());
    EXPECT_EQ(harken::StreamTerminalReason::None, streams[0].ring->TerminalReason());
    manager.CancelReplacement(bad);
    for (auto& stream : streams) EXPECT_TRUE(manager.RetirePreparedSlot(stream.ring, stream.ringToken));
}

TEST(VorbisStream, CancelBlockedPreparationReclaimsSlotAndSource) {
    harken::EngineConfig config;
    config.maxStreams = 1;
    harken::StreamManager manager(config);
    auto source = Fixture(2);
    auto range = Range(source, 2);
    harken::StreamManager::StreamFacts facts;
    ASSERT_EQ(VN_OK, manager.ResolvePackSource(range, &facts));
    harken::StreamManager::ReplacementRequest request;
    request.fileIndex = facts.fileIndex;
    request.sourceGeneration = facts.sourceGeneration;
    request.contentRevision = facts.contentRevision;
    request.prebufferTargetFrames = 256;
    source->hold.store(true);
    harken::StreamManager::PreparedToken token;
    ASSERT_EQ(VN_OK, manager.BeginReplacement(request, &token));
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
    while (!source->entered.load() && std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
    EXPECT_TRUE(source->entered.load());
    manager.CancelReplacement(token);
    source->hold.store(false);
    while (manager.FreeSlotCount() == 0 && std::chrono::steady_clock::now() < deadline) manager.SyncServe();
    EXPECT_EQ(1u, manager.FreeSlotCount());
    std::weak_ptr<Source> lifetime = source;
    range.reset();
    source.reset();
    while (!lifetime.expired() && std::chrono::steady_clock::now() < deadline) manager.SyncServe();
    EXPECT_TRUE(lifetime.expired());
}

TEST(VorbisStream, SteadyIoAndBadPageAreTerminalFailuresRatherThanNaturalEnd) {
    for (bool ioFailure : {false, true}) {
        harken::EngineConfig config;
        config.maxStreams = 1;
        config.streamRingFrames = 4096;
        harken::StreamManager manager(config);
        auto source = Fixture(8);
        auto range = Range(source, 8);
        harken::StreamManager::StreamFacts facts;
        ASSERT_EQ(VN_OK, manager.ResolvePackSource(range, &facts));
        harken::StreamManager::ReplacementRequest request;
        request.fileIndex = facts.fileIndex;
        request.sourceGeneration = facts.sourceGeneration;
        request.contentRevision = facts.contentRevision;
        request.prebufferTargetFrames = 4096;
        harken::StreamManager::PreparedToken token;
        ASSERT_EQ(VN_OK, manager.BeginReplacement(request, &token));
        ASSERT_EQ(harken::StreamManager::PreparationState::Ready, Wait(manager, token));
        harken::StreamManager::PreparedStream stream;
        ASSERT_EQ(VN_OK, manager.TakeReplacement(token, &stream));
        manager.SetThrottled(true);
        manager.SyncServe();
        if (ioFailure) source->fail.store(true);
        else source->bytes[source->bytes.size() - 2048] ^= 1;
        manager.SetThrottled(false);
        for (unsigned i = 0; i < 64 && stream.ring->TerminalReason() == harken::StreamTerminalReason::None; ++i) {
            stream.ring->AdvanceRead(stream.ring->ReadAvailable());
            manager.SyncServe();
        }
        EXPECT_EQ(ioFailure ? harken::StreamTerminalReason::ProducerFailure :
                              harken::StreamTerminalReason::IntegrityAbort, stream.ring->TerminalReason());
        EXPECT_TRUE(manager.RetirePreparedSlot(stream.ring, stream.ringToken));
    }
}

TEST(VorbisStream, DefaultCapacityConcurrentCursorsRemainIndependent) {
    harken::EngineConfig config;
    harken::StreamManager manager(config);
    const auto source = Fixture(2);
    const auto reference = Linear(*source, 2);
    const auto range = Range(source, 2);
    harken::StreamManager::StreamFacts facts;
    ASSERT_EQ(VN_OK, manager.ResolvePackSource(range, &facts));
    std::vector<harken::StreamManager::PreparedToken> tokens(config.maxStreams);
    std::vector<harken::StreamManager::PreparedStream> streams(config.maxStreams);
    harken::StreamManager::ReplacementRequest request;
    request.fileIndex = facts.fileIndex;
    request.sourceGeneration = facts.sourceGeneration;
    request.contentRevision = facts.contentRevision;
    request.prebufferTargetFrames = 4800;
    request.loopStartFrame = 48013;
    request.loopEndFrame = 53119;
    request.loopCount = 0;
    for (unsigned i = 0; i < config.maxStreams; ++i) {
        request.targetFrame = 48013 + i * 71;
        ASSERT_EQ(VN_OK, manager.BeginReplacement(request, &tokens[i]));
    }
    for (unsigned i = 0; i < config.maxStreams; ++i) {
        ASSERT_EQ(harken::StreamManager::PreparationState::Ready, Wait(manager, tokens[i]));
        ASSERT_EQ(VN_OK, manager.TakeReplacement(tokens[i], &streams[i]));
    }
    std::vector<std::uint64_t> frames(config.maxStreams);
    for (unsigned i = 0; i < config.maxStreams; ++i) frames[i] = 48013 + i * 71;
    for (unsigned block = 0; block < 96; ++block) {
        manager.SyncServe();
        for (unsigned i = 0; i < config.maxStreams; ++i) {
            auto& ring = *streams[i].ring;
            const float* planes[harken::kMaxChannels];
            ASSERT_GE(ring.ContiguousRead(planes), 256u);
            for (unsigned frame = 0; frame < 256; ++frame) {
                if (frames[i] == 53119) frames[i] = 48013;
                for (unsigned ch = 0; ch < 2; ++ch)
                    ASSERT_NEAR(planes[ch][frame], reference[ch * kFrames + frames[i]], 1e-6f);
                ++frames[i];
            }
            ring.AdvanceRead(256);
        }
    }
    for (auto& stream : streams) EXPECT_TRUE(manager.RetirePreparedSlot(stream.ring, stream.ringToken));
}

TEST(VorbisStream, DecoderRingVoiceOutputMatchesPcm) {
    const auto source = Fixture(1);
    const auto pcm = Linear(*source, 1);
    const auto render = [&](bool compressed) {
        auto config = harkentest::MakeNullConfig(2, 256, kRate);
        config.maxTriggerDepth = 2;
        config.maxStreams = 2;
        harkentest::EngineSession session(config);
        EXPECT_EQ(VN_OK, session.InitResult());
        auto& e = *session.Engine();
        VnVoiceHandle voice = 0;
        const auto pan = harken::ResolvePanGains(e.config.mixChannelLayout, 0);
        std::shared_ptr<harken::PackMediaRange> range;
        if (compressed) {
            range = Range(source, 1);
            harken::StreamManager::StreamFacts facts;
            EXPECT_EQ(VN_OK, e.streams.ResolvePackSource(range, &facts));
            harken::StreamManager::ReplacementRequest request;
            request.fileIndex = facts.fileIndex;
            request.sourceGeneration = facts.sourceGeneration;
            request.contentRevision = facts.contentRevision;
            request.prebufferTargetFrames = 4096;
            harken::StreamManager::PreparedToken token;
            EXPECT_EQ(VN_OK, e.streams.BeginReplacement(request, &token));
            EXPECT_EQ(harken::StreamManager::PreparationState::Ready, Wait(e.streams, token));
            harken::StreamManager::PreparedStream stream;
            EXPECT_EQ(VN_OK, e.streams.TakeReplacement(token, &stream));
            EXPECT_EQ(VN_OK, e.voices.PlayStream(stream.ring, 1, kRate, 256,
                0, 1, 0, false, 0, 0, 0, pan, 0, kFrames, 0, kFrames, 1,
                harken::ResampleProfile::LegacyCompatibility, &voice, 1, UINT32_MAX, 0, nullptr, 0, true));
            EXPECT_TRUE(e.voices.BindStreamRingToken(voice, stream.ring, stream.ringToken));
        } else {
            harken::PCMView view;
            view.channels[0] = pcm.data();
            view.channelCount = 1;
            view.sampleRate = kRate;
            view.frames = kFrames;
            EXPECT_EQ(VN_OK, e.voices.Play(view, 0, 1, 0, false, 0, 0, 0, pan, 0, kFrames, 1,
                harken::ResampleProfile::LegacyCompatibility, &voice, 1, UINT32_MAX, 0, true));
        }
        EXPECT_TRUE(e.voices.CommitPreparedVoice(voice));
        e.voices.ApplyPlay(voice, voice, 0, 0);
        harkentest::Capture capture;
        for (unsigned block = 0; block < 570; ++block) {
            if (compressed) e.streams.SyncServe();
            harkentest::PumpAndCapture(&e, 1, capture);
        }
        return capture;
    };
    const auto expected = render(false);
    const auto actual = render(true);
    ASSERT_EQ(expected.left.size(), actual.left.size());
    EXPECT_GT(harkentest::Peak(actual.left), 0.01f);
    for (unsigned i = 0; i < actual.left.size(); ++i) {
        ASSERT_NEAR(actual.left[i], expected.left[i], 1e-6f) << i;
        ASSERT_NEAR(actual.right[i], expected.right[i], 1e-6f) << i;
    }
}
} // namespace
