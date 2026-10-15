// Snapshot capture: Vn_CaptureSnapshot fills a caller POD from the
// audio-side published pinned buffer (voice states, taps, bus meters,
// arena marks) merged with logic-owned counters. Coverage here: the
// error contract, live-voice and completed-voice reporting, capture
// progress across blocks, arena-overflow provenance surfacing through
// both the snapshot counter and the log callback, the poisoned-commit
// drop counter, and a concurrent pump/capture stress that any torn read
// would break.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/core/LogRing.h"
#include "harken/core/SnapshotBuffer.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

namespace {

using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

VnSnapshot MakeSnapshotArg() {
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    return snapshot;
}

std::uint64_t MixSnapshotGeneration(std::uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}

void FillSnapshotPayload(harken::SnapshotPayload& payload,
                         std::uint64_t generation) {
    payload.blockIndex = generation;
    payload.activeVoiceCount = static_cast<std::uint32_t>(
        MixSnapshotGeneration(generation ^ 0xA0761D6478BD642Full));
    for (std::uint32_t voice = 0; voice < VN_SNAPSHOT_MAX_VOICES; ++voice) {
        payload.voices[voice].handle = static_cast<std::uint32_t>(
            MixSnapshotGeneration(generation ^
                                  (0xE7037ED1A0B428DBull * (voice + 1))));
    }
    payload.busCount = static_cast<std::uint32_t>(
        MixSnapshotGeneration(generation ^ 0x1D8E4E27C47D124Full));
    for (std::uint32_t bus = 0; bus < VN_SNAPSHOT_MAX_BUSES; ++bus) {
        payload.busPeaks[bus] = static_cast<float>(
            MixSnapshotGeneration(generation ^
                                  (0x589965CC75374CC3ull * (bus + 1))) &
            0xFFFFu);
        payload.busPeaksPreFader[bus] = static_cast<float>(
            MixSnapshotGeneration(generation ^
                                  (0x8CB92BA72F3D8DD7ull * (bus + 1))) &
            0xFFFFu);
        payload.busPeaksPostFader[bus] = static_cast<float>(
            MixSnapshotGeneration(generation ^
                                  (0xDB4F0B9175AE2165ull * (bus + 1))) &
            0xFFFFu);
        for (std::uint32_t channel = 0; channel < VN_MAX_CHANNELS; ++channel) {
            const auto index = bus * VN_MAX_CHANNELS + channel;
            payload.busChannelPeaks[index] = static_cast<float>(
                MixSnapshotGeneration(generation ^ (0x9E3779B97F4A7C15ull * (index + 1))) & 0xFFFFu);
        }
    }
    payload.arenaHighWaterBytes =
        MixSnapshotGeneration(generation ^ 0xEB44ACCAB455D165ull);
    payload.voiceStarvedCount =
        MixSnapshotGeneration(generation ^ 0xC6BC279692B5C323ull);
    payload.renderWorkerBusyPeakUs = static_cast<std::uint32_t>(
        MixSnapshotGeneration(generation ^ 0xD1B54A32D192ED03ull));
    payload.blockIndexEcho = generation;
}

bool IsSnapshotPayloadConsistent(const harken::SnapshotPayload& payload) {
    const std::uint64_t generation = payload.blockIndex;
    if (payload.blockIndexEcho != generation ||
        payload.activeVoiceCount != static_cast<std::uint32_t>(
                                        MixSnapshotGeneration(
                                            generation ^
                                            0xA0761D6478BD642Full)) ||
        payload.busCount != static_cast<std::uint32_t>(
                                MixSnapshotGeneration(
                                    generation ^ 0x1D8E4E27C47D124Full)) ||
        payload.arenaHighWaterBytes !=
            MixSnapshotGeneration(generation ^ 0xEB44ACCAB455D165ull) ||
        payload.voiceStarvedCount !=
            MixSnapshotGeneration(generation ^ 0xC6BC279692B5C323ull) ||
        payload.renderWorkerBusyPeakUs != static_cast<std::uint32_t>(
                                               MixSnapshotGeneration(
                                                   generation ^
                                                   0xD1B54A32D192ED03ull))) {
        return false;
    }
    for (std::uint32_t voice = 0; voice < VN_SNAPSHOT_MAX_VOICES; ++voice) {
        if (payload.voices[voice].handle != static_cast<std::uint32_t>(
                                                MixSnapshotGeneration(
                                                    generation ^
                                                    (0xE7037ED1A0B428DBull *
                                                     (voice + 1))))) {
            return false;
        }
    }
    for (std::uint32_t bus = 0; bus < VN_SNAPSHOT_MAX_BUSES; ++bus) {
        if (payload.busPeaks[bus] != static_cast<float>(
                                         MixSnapshotGeneration(
                                             generation ^
                                             (0x589965CC75374CC3ull *
                                              (bus + 1))) &
                                         0xFFFFu) ||
            payload.busPeaksPreFader[bus] != static_cast<float>(
                                                   MixSnapshotGeneration(
                                                       generation ^
                                                       (0x8CB92BA72F3D8DD7ull *
                                                        (bus + 1))) &
                                                   0xFFFFu) ||
            payload.busPeaksPostFader[bus] != static_cast<float>(
                                                    MixSnapshotGeneration(
                                                        generation ^
                                                        (0xDB4F0B9175AE2165ull *
                                                         (bus + 1))) &
                                                    0xFFFFu)) {
            return false;
        }
    }
    for (std::uint32_t index = 0; index < VN_SNAPSHOT_MAX_BUSES * VN_MAX_CHANNELS; ++index) {
        if (payload.busChannelPeaks[index] != static_cast<float>(
                MixSnapshotGeneration(generation ^ (0x9E3779B97F4A7C15ull * (index + 1))) & 0xFFFFu))
            return false;
    }
    return true;
}

TEST(Snapshot, ConsistencyCheckerRejectsCorruption) {
    harken::SnapshotPayload good{};
    FillSnapshotPayload(good, 42);
    EXPECT_TRUE(IsSnapshotPayloadConsistent(good));

    harken::SnapshotPayload payloadCorruption = good;
    payloadCorruption.busPeaks[0] += 1.0f;
    EXPECT_FALSE(IsSnapshotPayloadConsistent(payloadCorruption));

    harken::SnapshotPayload tapCorruption = good;
    tapCorruption.busPeaksPreFader[0] += 1.0f;
    EXPECT_FALSE(IsSnapshotPayloadConsistent(tapCorruption));

    harken::SnapshotPayload postFaderCorruption = good;
    postFaderCorruption.busPeaksPostFader[0] += 1.0f;
    EXPECT_FALSE(IsSnapshotPayloadConsistent(postFaderCorruption));

    harken::SnapshotPayload generationCorruption = good;
    ++generationCorruption.blockIndex;
    EXPECT_FALSE(IsSnapshotPayloadConsistent(generationCorruption));
}

void CaptureSnapshotBursts(harken::SnapshotBuffer* buffer,
                           std::atomic<std::uint64_t>* armed,
                           std::atomic<std::uint64_t>* acknowledged,
                           std::atomic<std::uint64_t>* completed,
                           std::atomic<bool>* abort, std::uint64_t rounds,
                           std::uint64_t* probed,
                           std::uint64_t* observedGeneration,
                           std::uint64_t* captured, std::uint64_t* tears) {
    for (std::uint64_t round = 0; round < rounds; ++round) {
        const std::uint64_t generation = round + 1;
        while (armed->load(std::memory_order_acquire) < generation) {
            if (abort->load(std::memory_order_acquire)) {
                return;
            }
            std::this_thread::yield();
        }
        acknowledged->store(generation, std::memory_order_release);
        harken::SnapshotPayload probePayload;
        if (buffer->Capture(probePayload)) {
            ++*probed;
            *observedGeneration += probePayload.blockIndex;
        }
        harken::SnapshotPayload capturedPayload;
        if (buffer->Capture(capturedPayload)) {
            ++*captured;
            if (!IsSnapshotPayloadConsistent(capturedPayload)) {
                ++*tears;
            }
        }
        if (abort->load(std::memory_order_acquire)) {
            return;
        }
        completed->store(generation, std::memory_order_release);
    }
}

TEST(Snapshot, CaptureBeforeInitIsRefused) {
    VnSnapshot snapshot = MakeSnapshotArg();
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_CaptureSnapshot(&snapshot));
}

TEST(Snapshot, NullAndWrongStructSizeAreRefused) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    EXPECT_EQ(VN_INVALID_ARG, Vn_CaptureSnapshot(nullptr));
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot) - 4;
    EXPECT_EQ(VN_INVALID_ARG, Vn_CaptureSnapshot(&snapshot));
    snapshot.structSize = 0;
    EXPECT_EQ(VN_INVALID_ARG, Vn_CaptureSnapshot(&snapshot));
}

TEST(Snapshot, PreBindingObservabilityVintageDoesNotReceiveTheTail) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    // The multichannel/job-parallel vintage ends where the section-12
    // property/binding observability tail begins; a caller declaring it
    // is served the common prefix and its tail bytes stay untouched.
    VnSnapshot snapshot{};
    snapshot.bindingDefinitionHighWater = 0xDEADBEEFu;
    snapshot.meterLoopCount = 0xDEADBEEFu;
    snapshot.structSize = static_cast<std::uint32_t>(
        offsetof(VnSnapshot, bindingDefinitionHighWater));
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    EXPECT_EQ(0xDEADBEEFu, snapshot.bindingDefinitionHighWater);
    EXPECT_EQ(0xDEADBEEFu, snapshot.meterLoopCount);
    // The current full vintage receives the tail (zeroed on a fresh
    // engine with no binding traffic).
    snapshot.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    EXPECT_EQ(0u, snapshot.bindingDefinitionHighWater);
    EXPECT_EQ(0u, snapshot.meterLoopCount);
}

TEST(Snapshot, BusAudibilityVintageAcceptsAppendedCounterTail) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());

    VnSnapshot older{};
    older.structSize = static_cast<std::uint32_t>(
        offsetof(VnSnapshot, actionEmitterMultichannelFallbackCount));
    older.actionEmitterMultichannelFallbackCount = 0xDEADBEEFu;
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&older));
    EXPECT_EQ(0xDEADBEEFu, older.actionEmitterMultichannelFallbackCount);

    VnSnapshot tapVintage{};
    tapVintage.structSize = static_cast<std::uint32_t>(
        offsetof(VnSnapshot, busPeaksPreFader));
    tapVintage.busPeaksPreFader[0] = 11.0f;
    tapVintage.busPeaksPostFader[0] = 13.0f;
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&tapVintage));
    EXPECT_FLOAT_EQ(tapVintage.busPeaksPreFader[0], 11.0f);
    EXPECT_FLOAT_EQ(tapVintage.busPeaksPostFader[0], 13.0f);

    // The immediately preceding full Bus-meter vintage remains accepted
    // after the Dynamic Route aggregate append. It receives its meter rows
    // while bytes beyond its declared 3416-byte prefix remain untouched.
    VnSnapshot meterVintage{};
    meterVintage.structSize = static_cast<std::uint32_t>(
        offsetof(VnSnapshot, dynamicRouteCurrent));
    meterVintage.busPeaksPreFader[0] = 17.0f;
    meterVintage.busPeaksPostFader[0] = 19.0f;
    meterVintage.dynamicRouteCurrent = 0xDEADBEEFu;
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&meterVintage));
    EXPECT_FLOAT_EQ(meterVintage.busPeaksPreFader[0], 0.0f);
    EXPECT_FLOAT_EQ(meterVintage.busPeaksPostFader[0], 0.0f);
    EXPECT_EQ(0xDEADBEEFu, meterVintage.dynamicRouteCurrent);

    VnSnapshot current = MakeSnapshotArg();
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&current));
    EXPECT_EQ(0u, current.actionEmitterMultichannelFallbackCount);
    EXPECT_FLOAT_EQ(current.busPeaksPreFader[0], 0.0f);
    EXPECT_FLOAT_EQ(current.busPeaksPostFader[0], 0.0f);
}

TEST(Snapshot, ReportsActiveVoiceMetersAndProgress) {
    EngineSession session(MakeNullConfig(4, 256, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> sine =
        harkentest::MakeSine(48000, 440.0, 48000.0, 0.5f);
    HARKENTEST_ASSERT_NON_SILENT(sine);
    const VnPCMDesc desc = MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    engine->nullBackend.Pump(4);

    VnSnapshot snapshot = MakeSnapshotArg();
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));

    // The public handle joins to its render-slot row through the
    // voicePublicHandles correlation array (the public handle is a ledger
    // handle whose low bits are not the render slot). The joined row's own
    // handle field is that render slot index.
    const int row = harkentest::VoiceRowForHandle(snapshot, handle);
    ASSERT_GE(row, 0);
    ASSERT_LT(row, static_cast<int>(VN_SNAPSHOT_MAX_VOICES));
    EXPECT_EQ(static_cast<std::uint32_t>(row),
              snapshot.voices[row].handle);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_VOICE_STATE_ACTIVE),
              snapshot.voices[row].state);
    EXPECT_EQ(1u, snapshot.activeVoiceCount);
    EXPECT_EQ(1u, snapshot.busCount);
    EXPECT_GE(snapshot.blockIndex, 3u);
    EXPECT_GT(snapshot.arenaHighWaterBytes, 0u);
    EXPECT_GE(snapshot.voiceHighWater, 1u);

    // Master bus meter carries the live mix in EVERY build from M6 on
    // (spec section 11.7: the bus output scan left the stage-tap gate).
    // Scalar silence guard: a meter below the floor would make this a
    // silent-vs-silent check.
    EXPECT_GT(snapshot.busPeaks[0], harkentest::kSilenceFloor);
    EXPECT_GT(snapshot.busPeaksPreFader[0], harkentest::kSilenceFloor);
    EXPECT_GT(snapshot.busPeaksPostFader[0], harkentest::kSilenceFloor);

    // Progress: a later capture observes a strictly later block.
    engine->nullBackend.Pump(3);
    VnSnapshot later = MakeSnapshotArg();
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&later));
    EXPECT_GT(later.blockIndex, snapshot.blockIndex);
}

TEST(Snapshot, BusChannelPeaksPreserveOutputLevels) {
    for (const std::uint32_t channels : {1u, 2u, 8u}) {
        const auto layout = channels == 1 ? VN_CHANNEL_LAYOUT_MONO
            : channels == 2 ? VN_CHANNEL_LAYOUT_STEREO : VN_CHANNEL_LAYOUT_7_1;
        EngineSession session(harkentest::MakeNullConfigLayout(4, 256, 48000, layout, layout));
        ASSERT_EQ(VN_OK, session.InitResult());
        const auto busId = harkentest::ObjectId(42);
        ASSERT_EQ(VN_OK, Vn_CreateBus(busId, VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, Vn_SetBusVolume(busId, -6.0206f, 0));
        std::vector<float> samples(4096 * channels);
        for (std::size_t index = 0; index < samples.size(); ++index)
            samples[index] = -0.05f * static_cast<float>(index % channels + 1);
        const auto pcm = harkentest::InterleavedDesc(samples, channels, 48000);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
        auto play = MakePlayDesc(harkentest::ObjectId(1));
        play.busId = busId;
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        session.Engine()->nullBackend.Pump(4);
        const auto busIndex = session.Engine()->buses.FindIndex(busId);
        auto snapshot = MakeSnapshotArg();
        ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
        ASSERT_EQ(snapshot.mixChannelCount, channels);
        for (const auto row : {0u, busIndex}) {
            EXPECT_NEAR(snapshot.busPeaks[row], 0.025f * channels, 1e-5f);
            for (std::uint32_t channel = 0; channel < VN_MAX_CHANNELS; ++channel) {
                const float expected = channel < channels ? 0.025f * (channel + 1) : 0.0f;
                EXPECT_NEAR(snapshot.busChannelPeaks[row * VN_MAX_CHANNELS + channel], expected, 1e-5f);
            }
        }
    }
}

TEST(Snapshot, EndedVoiceReportsInactiveAndCompleted) {
    EngineSession session(MakeNullConfig(4, 256, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    // 300 frames end within two blocks; extra pumps cover the release.
    const std::vector<float> sine =
        harkentest::MakeSine(300, 440.0, 48000.0, 0.5f);
    const VnPCMDesc desc = MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    engine->nullBackend.Pump(6);

    VnSnapshot snapshot = MakeSnapshotArg();
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    // The voice ended but Vn_Update has not reaped its ledger entry, so its
    // render slot no longer appears in the correlation array (the row reports
    // a free slot). The ledger still maps the public handle to that render
    // slot, so translate to find the render row and confirm it went INACTIVE.
    const std::uint32_t slot =
        static_cast<std::uint32_t>(harkentest::RenderHandle(engine, handle));
    EXPECT_EQ(static_cast<std::uint32_t>(VN_VOICE_STATE_INACTIVE),
              snapshot.voices[slot].state);
    EXPECT_EQ(0u, snapshot.activeVoiceCount);
    EXPECT_GE(snapshot.voiceCompletedCount, 1u);
}

struct LogCapture {
    std::uint32_t level = 0;
    std::uint32_t code = 0;
    std::string text;
    std::uint32_t calls = 0;
};

void RecordLog(void* userData, std::uint32_t level, std::uint32_t code,
               const char* text) {
    LogCapture* capture = static_cast<LogCapture*>(userData);
    capture->level = level;
    capture->code = code;
    capture->text = text;
    ++capture->calls;
}

TEST(Snapshot, ArenaOverflowSurfacesInCounterAndLogSink) {
    // An arena too small for even one bus accumulator plane overflows on
    // every block; the degradation must surface both as the snapshot
    // counter and as a drained warn record through the config sink.
    VnConfig config = MakeNullConfig(2, 256, 48000);
    config.arenaBytes = 64;
    LogCapture received;
    config.logCallback = &RecordLog;
    config.logUserData = &received;

    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    engine->nullBackend.Pump(3);

    VnSnapshot snapshot = MakeSnapshotArg();
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    EXPECT_GT(snapshot.arenaOverflowCount, 0u);

    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_GT(received.calls, 0u);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_LOG_WARN), received.level);
    EXPECT_EQ(harken::kLogCodeArenaOverflow, received.code);
    EXPECT_FALSE(received.text.empty());
}

TEST(Snapshot, DegradedBlockPublishesHonestSilence) {
    // The arena cannot fit even one bus accumulator plane, so every block
    // degrades before any rendering: the published taps and bus meters
    // must read silent for that block, while the structural facts (the
    // voice is rendering-live, overflow counted) still report.
    VnConfig config = MakeNullConfig(2, 256, 48000);
    config.arenaBytes = 64;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> sine =
        harkentest::MakeSine(48000, 440.0, 48000.0, 0.5f);
    HARKENTEST_ASSERT_NON_SILENT(sine);
    const VnPCMDesc desc = MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    engine->nullBackend.Pump(3);

    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    const int row = harkentest::VoiceRowForHandle(snapshot, handle);
    ASSERT_GE(row, 0);
    const VnVoiceSnapshot& voice = snapshot.voices[row];
    // The play command applied at the block boundary (commands drain
    // before the arena is touched), so the voice is rendering-live and
    // still ACTIVE — it degraded, it did not end.
    EXPECT_EQ(static_cast<std::uint32_t>(VN_VOICE_STATE_ACTIVE),
              voice.state);
    EXPECT_EQ(1u, snapshot.activeVoiceCount);
    EXPECT_GT(snapshot.arenaOverflowCount, 0u);

#if HARKEN_STAGE_TAPS_ACTIVE
    // Honest silence: nothing rendered this block, so measured taps and
    // meters are zero rather than a previous block's values.
    EXPECT_EQ(0.0f, voice.tapPostResample);
    EXPECT_EQ(0.0f, voice.tapPostFx);
    EXPECT_EQ(0.0f, voice.tapPostFilter);
    EXPECT_EQ(0.0f, voice.tapPostGain);
#else
    // Per-voice measurement compiled out: structurally zero.
    EXPECT_EQ(0.0f, voice.tapPostGain);
#endif
    // The bus output meter is live in every build (section 11.7), and a
    // degraded block's honest value is silence in every build too.
    EXPECT_EQ(0.0f, snapshot.busPeaks[0]);
    for (const float peak : snapshot.busChannelPeaks)
        EXPECT_FLOAT_EQ(peak, 0.0f);
}

TEST(Snapshot, ConfigVintagesAcceptedWithAppendedFieldsDefaulted) {
    // Mirrors the exact structSize boundaries ResolveConfig accepts: the
    // pre-callback vintage, the pre-log-sink vintage, and the current
    // full struct. Older vintages must behave as if the appended fields
    // were never set.
    const std::uint32_t vintage1 =
        static_cast<std::uint32_t>(offsetof(VnConfig, onNotification));
    const std::uint32_t vintage2 =
        static_cast<std::uint32_t>(offsetof(VnConfig, logCallback));

    {
        VnConfig config{};
        config.structSize = vintage1;
        config.deviceMode = VN_DEVICE_NULL;
        ASSERT_EQ(VN_OK, Vn_Init(&config));
        EXPECT_EQ(VN_OK, Vn_Term());
    }
    {
        // Vintage 2 predates the log fields: even with sink values
        // present in memory beyond the declared size, the engine must
        // not read them — records drain with no callback.
        LogCapture received;
        VnConfig config = MakeNullConfig(2, 256, 48000);
        config.arenaBytes = 64; // every block emits an overflow record
        config.logCallback = &RecordLog;
        config.logUserData = &received;
        config.structSize = vintage2;
        ASSERT_EQ(VN_OK, Vn_Init(&config));
        harken::detail::ActiveEngine()->nullBackend.Pump(2);
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_EQ(0u, received.calls);
        EXPECT_EQ(VN_OK, Vn_Term());
    }
    {
        // Current vintage: the same setup honors the sink.
        LogCapture received;
        VnConfig config = MakeNullConfig(2, 256, 48000);
        config.arenaBytes = 64;
        config.logCallback = &RecordLog;
        config.logUserData = &received;
        ASSERT_EQ(VN_OK, Vn_Init(&config));
        harken::detail::ActiveEngine()->nullBackend.Pump(2);
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_GT(received.calls, 0u);
        EXPECT_EQ(VN_OK, Vn_Term());
    }
    {
        // A size between the known vintages is not a vintage: refused.
        VnConfig config{};
        config.structSize = vintage1 + 4;
        config.deviceMode = VN_DEVICE_NULL;
        EXPECT_EQ(VN_INVALID_ARG, Vn_Init(&config));
    }
}

TEST(Snapshot, PoisonedCommitDropIsCounted) {
    EngineSession session(MakeNullConfig(2, 256, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  engine, {harkentest::ObjectId(7)}, 0.0f, 1.0f).status);

    const std::vector<float> sine =
        harkentest::MakeSine(48000, 440.0, 48000.0, 0.5f);
    const VnPCMDesc desc = MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    // Two bindings each contribute a near-FLT_MAX dB value to the same
    // volume target; their in-domain accumulation overflows to infinity,
    // which the wire boundary consumes as a poisoned commit.
    harken::Curve curve;
    const harken::CurvePoint pts[] = {{0.0f, 3.0e38f}, {1.0f, 3.0e38f}};
    const harken::CurveHandlePair handles[] = {{pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
    ASSERT_TRUE(curve.SetPoints(pts, handles, 2));
    ASSERT_TRUE(engine->params.BindVoiceTarget(harkentest::ObjectId(7), harkentest::RenderHandle(
        engine, handle), harken::kTargetVoiceVolume, curve));
    ASSERT_TRUE(engine->params.BindVoiceTarget(harkentest::ObjectId(7), harkentest::RenderHandle(
        engine, handle), harken::kTargetVoiceVolume, curve));

    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 0.5f));
    ASSERT_EQ(VN_OK, Vn_Update());
    // VN_OK accepts the write; the held initial command frontier has not run yet.
    ASSERT_GT(engine->variables.PendingCount(), 0u);
    VnSnapshot pending = MakeSnapshotArg();
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&pending));
    EXPECT_EQ(0u, pending.paramDropCount);
    ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(engine));

    VnSnapshot snapshot = MakeSnapshotArg();
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    EXPECT_EQ(1u, snapshot.paramDropCount);
    // The drop is not a commit: no command reached the audio thread, so
    // exactly one of the two counters moved.
    EXPECT_EQ(0u, snapshot.paramCommitCount);
    for (std::uint32_t step = 0; step < 4; ++step) {
        engine->nullBackend.Pump(2);
        ASSERT_EQ(VN_OK, Vn_Update());
        ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
        EXPECT_EQ(1u, snapshot.paramDropCount);
    }
}

TEST(Snapshot, ConcurrentCaptureStaysCoherent) {
    EngineSession session(MakeNullConfig(4, 128, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> sine =
        harkentest::MakeSine(48000 * 8, 440.0, 48000.0, 0.5f);
    const VnPCMDesc desc = MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    // One block renders before the thread starts so the published buffer
    // already carries real state (busCount, initial voice entries); the
    // capture loop then races the pump thread, which stands in for the
    // audio thread. Deterministic-pass by construction: bounded block
    // count, and every assertion holds for any interleaving — the test
    // exists to catch torn reads, which break monotonicity or the range
    // invariants.
    engine->nullBackend.Pump(1);
    constexpr std::uint64_t kBlocks = 1500;
    std::thread pumper([engine] {
        for (std::uint64_t i = 0; i < kBlocks; ++i) {
            engine->nullBackend.Pump(1);
        }
    });

    std::uint64_t lastBlockIndex = 0;
    while (true) {
        VnSnapshot snapshot = MakeSnapshotArg();
        ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
        EXPECT_GE(snapshot.blockIndex, lastBlockIndex);
        lastBlockIndex = snapshot.blockIndex;
        EXPECT_EQ(1u, snapshot.busCount);
        EXPECT_LE(snapshot.activeVoiceCount, 4u);
        for (std::uint32_t v = 0; v < VN_SNAPSHOT_MAX_VOICES; ++v) {
            ASSERT_LE(snapshot.voices[v].state,
                      static_cast<std::uint32_t>(VN_VOICE_STATE_DRAINING));
        }
        if (snapshot.blockIndex >= kBlocks) {
            break;
        }
    }
    pumper.join();

    VnSnapshot final_ = MakeSnapshotArg();
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&final_));
    EXPECT_EQ(kBlocks, final_.blockIndex);
}

TEST(Snapshot, DirectConcurrentPublicationNeverTearsPayload) {
    constexpr std::uint64_t kRounds = 1000;
    constexpr std::uint64_t kBurstPublications = 64;
    auto buffer = std::make_unique<harken::SnapshotBuffer>();
    FillSnapshotPayload(buffer->BeginWrite(), 1);
    buffer->EndWrite();

    std::atomic<std::uint64_t> armed{0};
    std::atomic<std::uint64_t> acknowledged{0};
    std::atomic<std::uint64_t> completed{0};
    std::atomic<bool> abort{false};
    std::uint64_t probed = 0;
    std::uint64_t observedGeneration = 0;
    std::uint64_t captured = 0;
    std::uint64_t tears = 0;
    std::thread reader(&CaptureSnapshotBursts, buffer.get(), &armed,
                       &acknowledged, &completed, &abort, kRounds, &probed,
                       &observedGeneration, &captured, &tears);

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::seconds(30);
    bool timedOut = false;
    std::uint64_t generation = 2;
    for (std::uint64_t round = 0; round < kRounds; ++round) {
        FillSnapshotPayload(buffer->BeginWrite(), generation++);
        const std::uint64_t handshake = round + 1;
        armed.store(handshake, std::memory_order_release);
        while (acknowledged.load(std::memory_order_acquire) < handshake) {
            if (std::chrono::steady_clock::now() >= deadline) {
                timedOut = true;
                break;
            }
            std::this_thread::yield();
        }
        if (timedOut) {
            break;
        }
        buffer->EndWrite();
        for (std::uint64_t publication = 0;
             publication < kBurstPublications; ++publication) {
            FillSnapshotPayload(buffer->BeginWrite(), generation++);
            buffer->EndWrite();
        }
        while (completed.load(std::memory_order_acquire) < handshake) {
            if (std::chrono::steady_clock::now() >= deadline) {
                timedOut = true;
                break;
            }
            std::this_thread::yield();
        }
        if (timedOut) {
            break;
        }
    }
    if (timedOut) {
        abort.store(true, std::memory_order_release);
    }
    reader.join();

    ASSERT_FALSE(timedOut) << "publication did not converge within the time bound";
    ASSERT_GT(probed, 0u);
    ASSERT_GT(observedGeneration, 0u);
    ASSERT_GT(captured, 0u);
    EXPECT_EQ(0u, tears);

    harken::SnapshotPayload out{};
    ASSERT_TRUE(buffer->Capture(out));
    EXPECT_EQ(1 + kRounds * (kBurstPublications + 1), out.blockIndex);
    EXPECT_TRUE(IsSnapshotPayloadConsistent(out));
}

} // namespace
