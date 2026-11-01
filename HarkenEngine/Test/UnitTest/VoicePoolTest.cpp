// Voice pool capacity and handle behavior: VN_NO_VOICE on exhaustion,
// generational stale-handle detection with its counter, slot recycling,
// play argument validation, and the Debug allocation guard over a real
// multi-voice render run.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "Engine.h"
#include "VoicePoolReleaseTestAccess.h"
#include "harken/core/CommandQueue.h"
#include "harken/core/CpuClock.h"
#include "harken/render/BuiltinEffects.h"
#include "harken/render/VoicePool.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <barrier>
#include <bit>
#include <chrono>
#include <limits>
#include <thread>
#include <utility>

namespace {

using harkentest::EngineSession;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlockFrames = 256;
constexpr std::uint32_t kSampleRate = 48000;

VnConfig VoicePoolConfig(std::uint32_t maxVoices = 0,
                         std::uint32_t blockFrames = 0,
                         std::uint32_t sampleRate = 0) {
    auto config = harkentest::MakeNullConfig(maxVoices, blockFrames, sampleRate);
    // Voice lifetime coverage does not require the default nested Action scratch.
    config.maxTriggerDepth = 2;
    return config;
}

void ExpectVoiceSendSlotsEqual(const harken::VoiceRenderContext& expected,
                               const harken::VoiceRenderContext& actual) {
    EXPECT_EQ(expected.voiceSendActiveCount, actual.voiceSendActiveCount);
    for (std::uint32_t i = 0; i < harken::kMaxVoiceSendSlots; ++i) {
        const auto& lhs = expected.voiceSends[i];
        const auto& rhs = actual.voiceSends[i];
        EXPECT_EQ(lhs.active, rhs.active) << "send " << i;
        EXPECT_EQ(lhs.targetBusIndex, rhs.targetBusIndex) << "send " << i;
        EXPECT_EQ(lhs.gain.current, rhs.gain.current) << "send " << i;
        EXPECT_EQ(lhs.gain.target, rhs.gain.target) << "send " << i;
        EXPECT_EQ(lhs.gain.started, rhs.gain.started) << "send " << i;
        EXPECT_EQ(lhs.ramping, rhs.ramping) << "send " << i;
        EXPECT_EQ(lhs.rampStartGain, rhs.rampStartGain) << "send " << i;
        EXPECT_EQ(lhs.rampEndGain, rhs.rampEndGain) << "send " << i;
        EXPECT_EQ(lhs.rampBlocksTotal, rhs.rampBlocksTotal) << "send " << i;
        EXPECT_EQ(lhs.rampBlocksDone, rhs.rampBlocksDone) << "send " << i;
    }
}

const VnEffectDesc* AsEffectDesc(const void* concrete) {
    return static_cast<const VnEffectDesc*>(concrete);
}

void IdentityTailProcess(void* /*state*/, float* const* /*channels*/,
                         std::uint32_t /*channelCount*/,
                         std::uint32_t /*offset*/, std::uint32_t /*frames*/) {}

std::uint32_t ConfiguredTailFrames(const void* state) {
    return *static_cast<const std::uint32_t*>(state);
}

void ApplyPreparedFilter(harken::Engine* engine, VnVoiceHandle handle,
                         std::uint32_t filterKind, float cutoffHz,
                         bool enable) {
    constexpr float q = 0.70710678f;
    float frequencyHz = cutoffHz;
    harken::BiquadCoeffs coeffs;
    std::uint32_t tailFrames = 0;
    if (enable) {
        const float maxCutoff = 0.499f * static_cast<float>(kSampleRate);
        if (!(frequencyHz >= 1.0f)) {
            frequencyHz = 1.0f;
        } else if (frequencyHz > maxCutoff) {
            frequencyHz = maxCutoff;
        }
        coeffs = filterKind == harken::kFilterKindLowpass
                     ? harken::MakeLowpass(static_cast<float>(kSampleRate),
                                          frequencyHz, q)
                     : harken::MakeHighpass(static_cast<float>(kSampleRate),
                                           frequencyHz, q);
        tailFrames = harken::EstimateBiquadTailFrames(coeffs);
    }
    engine->voices.ApplySetFilter(
        handle, filterKind, enable, frequencyHz, q, coeffs, tailFrames,
        harken::BiquadSmoothingFrames(kSampleRate));
}

void ApplyPreparedLowShelf(harken::Engine* engine, VnVoiceHandle handle,
                           float gainDb) {
    harken::BiquadCoeffs coeffs;
    std::uint32_t tailFrames = 0;
    if (gainDb != 0.0f) {
        coeffs = harken::MakeLowShelf(static_cast<float>(kSampleRate), 250.0f,
                                     0.70710678f, gainDb);
        tailFrames = harken::EstimateBiquadTailFrames(coeffs);
    }
    engine->voices.ApplySetLowShelf(handle, gainDb, coeffs, tailFrames);
}

void ExpectCoeffsEqual(const harken::BiquadCoeffs& expected,
                       const harken::BiquadCoeffs& actual) {
    EXPECT_EQ(std::bit_cast<std::uint32_t>(expected.b0),
              std::bit_cast<std::uint32_t>(actual.b0));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(expected.b1),
              std::bit_cast<std::uint32_t>(actual.b1));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(expected.b2),
              std::bit_cast<std::uint32_t>(actual.b2));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(expected.a1),
              std::bit_cast<std::uint32_t>(actual.a1));
    EXPECT_EQ(std::bit_cast<std::uint32_t>(expected.a2),
              std::bit_cast<std::uint32_t>(actual.a2));
}

void ExpectPcmResampleProfile(std::uint32_t sourceRate,
                              std::uint32_t mixRate,
                              harken::ResampleProfile expected) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames, mixRate));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> samples = harkentest::MakeDC(4096, 0.25f);
    const VnPCMDesc desc = MonoDesc(samples, sourceRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.loopCount = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    const VnVoiceHandle render =
        harkentest::RenderHandle(session.Engine(), handle);
    const harken::VoiceRenderContext* context =
        session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    EXPECT_EQ(expected, context->resampler.Profile());
    EXPECT_EQ(0u, session.Engine()->voices.ProfessionalStorageBytes());
    EXPECT_EQ(nullptr, context->resampler.professional);
    EXPECT_EQ(expected,
              session.Engine()
                  ->voicePlayFacts[static_cast<std::uint32_t>(render)]
                  .resampleProfile);
}

void ExpectStreamResampleProfile(std::uint32_t sourceRate,
                                 std::uint32_t mixRate,
                                 harken::ResampleProfile expected) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames, mixRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* const engine = session.Engine();

    constexpr std::uint32_t kStreamCapacity = 64;
    std::array<float, kStreamCapacity> storage{};
    float* planes[harken::kMaxChannels] = {};
    planes[0] = storage.data();
    harken::StreamRing ring;
    ring.Wire(planes, kStreamCapacity);
    ring.ResetForUse(1);

    VnVoiceHandle handle = 0;
    const harken::ResolvedPanGains pan =
        harken::ResolvePanGains(engine->config.mixChannelLayout, 0.0f);
    ASSERT_EQ(
        VN_OK,
        engine->voices.PlayStream(
            &ring, 1, sourceRate, 0, 0.0f, 1.0, 0, false, 0.0f, 0.0f,
            0.0f, pan, 0, 4096, 0, 4096, 1,
            harken::SelectResampleProfile(sourceRate, mixRate), &handle));
    const harken::VoiceRenderContext* context =
        engine->voices.Resolve(handle);
    ASSERT_NE(nullptr, context);
    EXPECT_EQ(expected, context->resampler.Profile());
    engine->voices.ApplyStop(handle, 0, 0, engine->notifications, 0);
}

VnResult ClaimObserverVoice(harken::Engine& engine, VnVoiceHandle* out,
                           bool prepared = false,
                           std::uint32_t slot = UINT32_MAX) {
    const auto pcm = engine.registry.Lookup(harkentest::ObjectId(1));
    return engine.voices.Play(pcm, 0.0f, 1.0, 0, false, 0, 0, 0,
        harken::ResolvePanGains(engine.config.mixChannelLayout, 0.0f),
        0, pcm.frames, 1, harken::ResampleProfile::Professional44148,
        out, 1, slot, 0, prepared);
}

struct HeldSource {
    harken::PlayableInstanceHandle leaf = 0;
    std::uint64_t epoch = 0;
    harken::SourceTerminalToken token{};
    harken::VoiceReleaseObserver release{};
};

bool BindHeldSource(harken::Engine& engine, VnVoiceHandle voice, HeldSource* source) {
    source->leaf = engine.playableInstances.Acquire(harken::PlayableKind::Sound, 0);
    source->epoch = engine.playableInstances.Classify(source->leaf).scheduleEpoch;
    if (!engine.playableInstances.AcquireSourceRetirementHold(source->leaf, source->epoch) ||
        !engine.voices.CaptureReleaseObserver(voice, &source->release)) return false;
    source->token = engine.sourceTerminals.Reserve(engine.playableInstances,
        source->leaf, source->epoch, voice, source->release, 0);
    return source->token.Valid() && engine.voices.BindSourceTerminal(voice,
        engine.sourceTerminals.Resolve(source->token), source->token);
}

TEST(VoicePool, StreamParkPendingOwnershipSurvivesPartialAndStaleRelease) {
    EngineSession session(VoicePoolConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto samples = harkentest::MakeDC(64, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto& voices = session.Engine()->voices;
    VnVoiceHandle first = 0;
    VnVoiceHandle second = 0;
    ASSERT_EQ(VN_OK, ClaimObserverVoice(*session.Engine(), &first));
    ASSERT_EQ(VN_OK, ClaimObserverVoice(*session.Engine(), &second));
    EXPECT_FALSE(voices.HasPendingStreamParkMailboxes());
    ASSERT_TRUE(voices.ReserveStreamPark(first, first, 1));
    ASSERT_TRUE(voices.ReserveStreamPark(second, second, 2));
    voices.AbortStreamParkReservation(first, first, 1);
    voices.AbortStreamParkReservation(first, first, 1);
    voices.AbortStreamParkReservation(second, second, 1);
    EXPECT_TRUE(voices.HasPendingStreamParkMailboxes());
    harken::StreamParkOutcome outcome;
    bool cancelled = false;
    EXPECT_FALSE(voices.ConsumeStreamParkOutcomeExact(
        second, second, 2, &outcome, &cancelled));
    EXPECT_TRUE(voices.HasPendingStreamParkMailboxes());
    // A non-stream render publishes the original StaleRender outcome.
    voices.ApplyStop(second, 0, 0, session.Engine()->notifications, 0,
        harken::kStopModeStreamPark, second, 2);
    EXPECT_FALSE(voices.ConsumeStreamParkOutcomeExact(
        second, second, 1, &outcome, &cancelled));
    EXPECT_TRUE(voices.HasPendingStreamParkMailboxes());
    ASSERT_TRUE(voices.ConsumeStreamParkOutcome(
        static_cast<std::uint32_t>(second), &outcome, &cancelled));
    EXPECT_EQ(harken::StreamParkDisposition::StaleRender, outcome.disposition);
    EXPECT_FALSE(voices.HasPendingStreamParkMailboxes());
    EXPECT_FALSE(voices.ConsumeStreamParkOutcome(
        static_cast<std::uint32_t>(second), &outcome, &cancelled));
    ASSERT_TRUE(voices.ReserveStreamPark(first, first, 3));
    voices.DiscardAllStreamParkMailboxes();
    EXPECT_FALSE(voices.HasPendingStreamParkMailboxes());
}

TEST(VoicePool, ReleaseObserverSurvivesNaturalExitAndSlotReuse) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto samples = harkentest::MakeDC(64, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto& engine = *session.Engine();
    VnVoiceHandle first = 0;
    EXPECT_EQ(0u, engine.voices.SlotStorageBytes());
    EXPECT_EQ(0u, engine.voices.SlotCommittedBytes());
    EXPECT_EQ(0u, engine.voices.RenderWorksetStorageBytes());
    EXPECT_EQ(0u, engine.voices.RenderWorksetCommittedBytes());
    EXPECT_EQ(1u, engine.voices.Capacity());
    EXPECT_EQ(1u, engine.voices.AvailableSlotCount());
    EXPECT_EQ(0u, engine.timelineVoiceInstallIdentities.RequestedBytes());
    EXPECT_EQ(0u, engine.voiceOriginPlayable.RequestedBytes());
    EXPECT_EQ(0u, engine.voicePlayFacts.RequestedBytes());
    EXPECT_EQ(0u, engine.playbackPaths.RequestedBytes());
    EXPECT_EQ(0u, engine.effectSlotTypes.RequestedBytes());
    EXPECT_EQ(0u, engine.voiceEffectInstanceRefs.RequestedBytes());
    EXPECT_EQ(0u, engine.effectSlotConfig.RequestedBytes());
    EXPECT_EQ(0u, engine.voiceSendConfig.RequestedBytes());
    EXPECT_EQ(0u, engine.params.VoiceStorageBytes());
    const auto emptyStartBytes = engine.timelineSink.VariableStartStorageBytes();
    engine.voices.SetHighRatioCommitPageLimitForTests(0);
    EXPECT_EQ(VN_NO_VOICE, ClaimObserverVoice(engine, &first));
    EXPECT_EQ(0u, first);
    EXPECT_EQ(0u, engine.voices.ConstructedSlotCount());
    EXPECT_EQ(1u, engine.voices.AvailableSlotCount());
    engine.voices.SetHighRatioCommitPageLimitForTests(SIZE_MAX);
    engine.sourceCursors.SetCommitPageLimitForTests(1, 0);
    EXPECT_EQ(VN_NO_VOICE, ClaimObserverVoice(engine, &first));
    EXPECT_EQ(0u, first);
    EXPECT_EQ(0u, engine.voices.ConstructedSlotCount());
    EXPECT_EQ(1u, engine.voices.AvailableSlotCount());
    engine.sourceCursors.SetCommitPageLimitForTests(1, SIZE_MAX);
    harken::VoiceTimelineSink secondSink(engine.voices, engine.timelineArms);
    harken::VoicePool::SlotStorageRegistration refusedStorage;
    ASSERT_TRUE(engine.voices.AttachSlotStoragePreparation(refusedStorage, nullptr,
        [](void*, std::uint32_t count) noexcept { return count == 0; }));
    EXPECT_EQ(VN_NO_VOICE, ClaimObserverVoice(engine, &first));
    EXPECT_EQ(0u, first);
    EXPECT_EQ(0u, engine.voices.ConstructedSlotCount());
    EXPECT_EQ(1u, engine.voices.AvailableSlotCount());
    refusedStorage.Detach();
    engine.playbackPaths.SetCommitPageLimitForTests(0);
    EXPECT_EQ(VN_NO_VOICE, ClaimObserverVoice(engine, &first));
    EXPECT_EQ(0u, first);
    EXPECT_EQ(0u, engine.voices.ConstructedSlotCount());
    EXPECT_EQ(1u, engine.voices.AvailableSlotCount());
    engine.playbackPaths.SetCommitPageLimitForTests(SIZE_MAX);
    engine.mixer.ArenaForTests().SetCommitPageLimitForTests(0);
    EXPECT_EQ(VN_NO_VOICE, ClaimObserverVoice(engine, &first));
    EXPECT_EQ(0u, first);
    EXPECT_EQ(0u, engine.voices.ConstructedSlotCount());
    EXPECT_EQ(1u, engine.voices.AvailableSlotCount());
    engine.mixer.ArenaForTests().SetCommitPageLimitForTests(SIZE_MAX);
    engine.voices.SetSlotCommitPageLimitForTests(0);
    EXPECT_EQ(VN_NO_VOICE, ClaimObserverVoice(engine, &first));
    EXPECT_EQ(0u, first);
    EXPECT_EQ(0u, engine.voices.ConstructedSlotCount());
    EXPECT_EQ(1u, engine.voices.AvailableSlotCount());
    engine.voices.SetSlotCommitPageLimitForTests(SIZE_MAX);
    engine.voices.SetProfessionalCommitPageLimitForTests(0);
    EXPECT_EQ(VN_NO_VOICE, ClaimObserverVoice(engine, &first));
    EXPECT_EQ(0u, first);
    EXPECT_EQ(0u, engine.voices.ProfessionalStorageBytes());
    EXPECT_EQ(1u, engine.voices.AvailableSlotCount());
    engine.voices.SetProfessionalCommitPageLimitForTests(SIZE_MAX);
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &first));
    EXPECT_GT(engine.params.VoiceStorageBytes(), 0u);
    ASSERT_EQ(1u, engine.effectSlotTypes.size());
    for (const auto type : engine.effectSlotTypes[0]) EXPECT_EQ(harken::kEffectTypeNone, type);
    EXPECT_GT(engine.timelineSink.VariableStartStorageBytes(), emptyStartBytes);
    EXPECT_EQ(engine.timelineSink.VariableStartStorageBytes(), secondSink.VariableStartStorageBytes());
    {
        harken::VoiceTimelineSink lateSink(engine.voices, engine.timelineArms);
        EXPECT_EQ(secondSink.VariableStartStorageBytes(), lateSink.VariableStartStorageBytes());
        EXPECT_FALSE(engine.voices.AttachSlotStoragePreparation(refusedStorage, nullptr,
            [](void*, std::uint32_t count) noexcept { return count == 0; }));
    }
    harken::VoiceReleaseObserver observer{};
    ASSERT_TRUE(engine.voices.CaptureReleaseObserver(first, &observer));
    EXPECT_FALSE(engine.voices.ReleaseObserved(observer));
    engine.voices.ApplyPlay(first, first, 0, 0);
    harken::VoiceReleaseObserver tooLate{};
    EXPECT_FALSE(engine.voices.CaptureReleaseObserver(first, &tooLate));
    engine.nullBackend.Pump(1);
    ASSERT_EQ(nullptr, engine.voices.Resolve(first));
    EXPECT_EQ(0u, engine.registry.Lookup(harkentest::ObjectId(1)).useCount->load());
    EXPECT_TRUE(engine.voices.ReleaseObserved(observer));
    VnVoiceHandle second = 0;
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &second));
    harken::VoiceReleaseObserver next{};
    ASSERT_TRUE(engine.voices.CaptureReleaseObserver(second, &next));
    EXPECT_EQ(observer.slot, next.slot);
    EXPECT_EQ(observer.targetSequence + 1u, next.targetSequence);
    EXPECT_TRUE(engine.voices.ReleaseObserved(observer));
    EXPECT_FALSE(engine.voices.ReleaseObserved(next));
    EXPECT_TRUE(engine.voices.AbortClaimedVoice(second));
    EXPECT_TRUE(engine.voices.ReleaseObserved(next));
}

TEST(VoicePool, SourceTerminalSurvivesSlotReuseAndWaitsCompletedMixBlock) {
    for (const auto workers : {0u, 2u}) {
        SCOPED_TRACE(workers);
        auto config = VoicePoolConfig(1, kBlockFrames);
        config.renderWorkerCount = workers;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        const auto samples = harkentest::MakeDC(64, 0.25f);
        const auto desc = MonoDesc(samples, kSampleRate);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
        auto& engine = *session.Engine();
        std::array<harken::SourceTerminalToken, 2> tokens{};
        std::array<VnVoiceHandle, 2> voices{};
        std::array<harken::PlayableInstanceHandle, 2> leaves{};
        for (std::size_t index = 0; index < tokens.size(); ++index) {
            leaves[index] = engine.playableInstances.Acquire(harken::PlayableKind::Sound, 0);
            ASSERT_NE(0u, leaves[index]);
            const auto epoch = engine.playableInstances.Classify(leaves[index]).scheduleEpoch;
            ASSERT_TRUE(engine.playableInstances.AcquireSourceRetirementHold(leaves[index], epoch));
            ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &voices[index]));
            harken::VoiceReleaseObserver release{};
            ASSERT_TRUE(engine.voices.CaptureReleaseObserver(voices[index], &release));
            tokens[index] = engine.sourceTerminals.Reserve(engine.playableInstances,
                leaves[index], epoch, voices[index], release, 0);
            ASSERT_TRUE(tokens[index].Valid());
            ASSERT_TRUE(engine.voices.BindSourceTerminal(voices[index],
                engine.sourceTerminals.Resolve(tokens[index]), tokens[index]));
            EXPECT_FALSE(engine.voices.BindSourceTerminal(voices[index],
                engine.sourceTerminals.Resolve(tokens[index]), tokens[index]));
            engine.voices.ApplyPlay(voices[index], voices[index], 0, engine.mixer.BlocksRendered());
            engine.nullBackend.Pump(1);
            ASSERT_TRUE(engine.voices.ReleaseObserved(release));
        }
        EXPECT_EQ(static_cast<std::uint32_t>(voices[0]), static_cast<std::uint32_t>(voices[1]));
        EXPECT_NE(voices[0], voices[1]);
        // Neither notification delivery nor polling A before B is required.
        harken::SourceTerminalPayload first{}, second{};
        EXPECT_FALSE(engine.sourceTerminals.Capture(tokens[0], 0, &first));
        EXPECT_FALSE(engine.sourceTerminals.Capture(tokens[1], 1, &second));
        ASSERT_TRUE(engine.sourceTerminals.Capture(tokens[0], 2, &first));
        ASSERT_TRUE(engine.sourceTerminals.Capture(tokens[1], 2, &second));
        EXPECT_EQ(harken::SourceTerminalKind::Natural, first.kind);
        EXPECT_EQ(harken::SourceTerminalKind::Natural, second.kind);
        EXPECT_EQ(64u, first.frame);
        EXPECT_EQ(kBlockFrames + 64u, second.frame);
        for (std::size_t index = 0; index < tokens.size(); ++index) {
            EXPECT_TRUE(engine.playableInstances.HasSourceRetirementHold(leaves[index]));
            EXPECT_TRUE(engine.playableInstances.ReleaseSourceRetirementHold(leaves[index]));
            EXPECT_TRUE(engine.sourceTerminals.Release(tokens[index]));
            EXPECT_FALSE(engine.sourceTerminals.Capture(tokens[index], 2, &first));
        }
    }
}

TEST(VoicePool, SourceTerminalStopBeforeEqualAfterRealBoundary) {
    for (const auto workers : {0u, 2u}) for (const auto cutoff : {63u, 64u, 65u}) {
        SCOPED_TRACE(workers);
        SCOPED_TRACE(cutoff);
        auto config = VoicePoolConfig(1, kBlockFrames);
        config.renderWorkerCount = workers;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        const auto samples = harkentest::MakeDC(64, 0.25f);
        const auto desc = MonoDesc(samples, kSampleRate);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
        auto& engine = *session.Engine();
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &voice));
        harken::VoiceReleaseObserver release{};
        ASSERT_TRUE(engine.voices.CaptureReleaseObserver(voice, &release));
        const auto leaf = engine.playableInstances.Acquire(harken::PlayableKind::Sound, 0);
        const auto epoch = engine.playableInstances.Classify(leaf).scheduleEpoch;
        ASSERT_TRUE(engine.playableInstances.AcquireSourceRetirementHold(leaf, epoch));
        const auto token = engine.sourceTerminals.Reserve(engine.playableInstances, leaf, epoch, voice, release, 0);
        ASSERT_TRUE(token.Valid());
        ASSERT_TRUE(engine.voices.BindSourceTerminal(voice, engine.sourceTerminals.Resolve(token), token));
        engine.voices.ApplyPlay(voice, voice, 0, 0);
        engine.voices.ApplyStop(voice, 0, cutoff, engine.notifications, 0);
        engine.nullBackend.Pump(1);
        harken::SourceTerminalPayload result{};
        ASSERT_TRUE(engine.sourceTerminals.Capture(token, engine.mixer.BlocksRendered(), &result));
        EXPECT_EQ(cutoff <= 64 ? harken::SourceTerminalKind::Stop : harken::SourceTerminalKind::Natural, result.kind);
        EXPECT_EQ(std::min(64u, cutoff), result.frame);
    }
}

TEST(VoicePool, SourceTerminalCancelAndQuarantineNeverPublishNatural) {
    for (const auto quarantine : {false, true}) {
        EngineSession session(VoicePoolConfig(1, kBlockFrames));
        ASSERT_EQ(VN_OK, session.InitResult());
        const auto samples = harkentest::MakeDC(64, 0.25f);
        const auto desc = MonoDesc(samples, kSampleRate);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
        auto& engine = *session.Engine();
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &voice, !quarantine));
        harken::VoiceReleaseObserver release{};
        ASSERT_TRUE(engine.voices.CaptureReleaseObserver(voice, &release));
        const auto leaf = engine.playableInstances.Acquire(harken::PlayableKind::Sound, 0);
        const auto epoch = engine.playableInstances.Classify(leaf).scheduleEpoch;
        ASSERT_TRUE(engine.playableInstances.AcquireSourceRetirementHold(leaf, epoch));
        const auto token = engine.sourceTerminals.Reserve(engine.playableInstances, leaf, epoch, voice, release, 0);
        ASSERT_TRUE(token.Valid());
        auto* record = engine.sourceTerminals.Resolve(token);
        ASSERT_TRUE(engine.voices.BindSourceTerminal(voice, record, token));
        if (quarantine)
            harken::detail::VoicePoolReleaseTestAccess::Quarantine(engine.voices, engine.notifications);
        else ASSERT_TRUE(engine.voices.CancelPreparedVoice(voice));
        harken::SourceTerminalPayload result{};
        EXPECT_FALSE(engine.sourceTerminals.Capture(token, UINT64_MAX, &result));
        EXPECT_EQ(!quarantine, engine.voices.ReleaseObserved(release));
        EXPECT_EQ(quarantine ? harken::SourceTerminalKind::None : harken::SourceTerminalKind::Cancelled,
            record->bypassResult.load());
    }
}

TEST(VoicePool, SourceTerminalStreamQueuedEndFailureAndStarvationRemainDistinct) {
    for (const auto workers : {0u, 2u}) {
        for (const auto reason : {harken::StreamTerminalReason::None,
                harken::StreamTerminalReason::NaturalEnd,
                harken::StreamTerminalReason::ProducerFailure,
                harken::StreamTerminalReason::IntegrityAbort,
                harken::StreamTerminalReason::Count}) {
            SCOPED_TRACE(workers);
            SCOPED_TRACE(static_cast<int>(reason));
            auto config = VoicePoolConfig(1, kBlockFrames);
            config.renderWorkerCount = workers;
            std::array<float, 2048> storage{};
            storage.fill(0.25f);
            float* planes[harken::kMaxChannels] = {storage.data()};
            harken::StreamRing ring;
            ring.Wire(planes, static_cast<std::uint32_t>(storage.size()));
            ring.ResetForUse(1);
            ring.PublishWrite(1024);
            ring.state.store(harken::StreamSlotState::Active);
            const bool staleGeneration = reason == harken::StreamTerminalReason::Count;
            if (reason != harken::StreamTerminalReason::None)
                ASSERT_TRUE(ring.MarkEndOfData(staleGeneration ? harken::StreamTerminalReason::NaturalEnd : reason));
            EngineSession session(config);
            ASSERT_EQ(VN_OK, session.InitResult());
            auto& engine = *session.Engine();
            VnVoiceHandle voice = 0;
            const auto pan = harken::ResolvePanGains(engine.config.mixChannelLayout, 0.0f);
            ASSERT_EQ(VN_OK, engine.voices.PlayStream(&ring, 1, kSampleRate,
                0, 0.0f, 1.0, 0, false, 0.0f, 0.0f, 0.0f, pan,
                0, 1024, 0, 1024, 1,
                harken::SelectResampleProfile(kSampleRate, kSampleRate), &voice));
            HeldSource source{};
            ASSERT_TRUE(BindHeldSource(engine, voice, &source));
            if (staleGeneration) {
                // Publish a terminal winner for a different ring generation;
                // it cannot establish EOF for this already-bound source.
                const auto next = ring.generation.load() + 1u;
                ring.generation.store(next);
                ring.winnerGeneration.store(next);
            }
            engine.voices.ApplyPlay(voice, voice, 0, 0);
            engine.nullBackend.Pump(1);
            harken::SourceTerminalPayload result{};
            EXPECT_FALSE(engine.sourceTerminals.Capture(source.token, engine.mixer.BlocksRendered(), &result));
            EXPECT_GT(ring.ReadAvailable(), 0u);
            EXPECT_FALSE(engine.voices.ReleaseObserved(source.release));
            for (std::uint32_t block = 0; block < 12; ++block) engine.nullBackend.Pump(1);
            EXPECT_EQ(0u, ring.ReadAvailable());
            const bool completed = engine.sourceTerminals.Capture(source.token,
                engine.mixer.BlocksRendered(), &result);
            if (reason == harken::StreamTerminalReason::None) {
                EXPECT_FALSE(completed);
                EXPECT_FALSE(engine.voices.ReleaseObserved(source.release));
                EXPECT_TRUE(engine.playableInstances.HasSourceRetirementHold(source.leaf));
                engine.voices.ApplyStop(voice, 0, 0, engine.notifications, engine.mixer.BlocksRendered());
            } else {
                ASSERT_TRUE(completed);
                const auto expected = reason == harken::StreamTerminalReason::NaturalEnd
                    ? harken::SourceTerminalKind::Natural
                    : reason == harken::StreamTerminalReason::ProducerFailure
                        ? harken::SourceTerminalKind::ProducerFailure : harken::SourceTerminalKind::IntegrityAbort;
                EXPECT_EQ(expected, result.kind);
                EXPECT_EQ(1024u, result.frame);
                EXPECT_TRUE(engine.voices.ReleaseObserved(source.release));
            }
        }
    }
}

TEST(VoicePool, SourceTerminalNaturalSurvivesLaterDspTailStop) {
    for (const auto workers : {0u, 2u}) {
        auto config = VoicePoolConfig(1, kBlockFrames);
        config.renderWorkerCount = workers;
        config.fxSlotsPerVoice = 1;
        std::uint32_t tailFrames = kBlockFrames * 8;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        const auto samples = harkentest::MakeDC(64, 0.25f);
        const auto desc = MonoDesc(samples, kSampleRate);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
        auto& engine = *session.Engine();
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &voice));
        HeldSource source{};
        ASSERT_TRUE(BindHeldSource(engine, voice, &source));
        auto* context = engine.voices.Resolve(voice);
        ASSERT_NE(nullptr, context);
        context->fxSlots[0].process = IdentityTailProcess;
        context->fxSlots[0].tailFrames = ConfiguredTailFrames;
        context->fxSlots[0].state = &tailFrames;
        engine.voices.ApplyPlay(voice, voice, 0, 0);
        engine.nullBackend.Pump(1);
        harken::SourceTerminalPayload result{};
        ASSERT_TRUE(engine.sourceTerminals.Capture(source.token, engine.mixer.BlocksRendered(), &result));
        EXPECT_EQ(harken::SourceTerminalKind::Natural, result.kind);
        EXPECT_EQ(64u, result.frame);
        EXPECT_FALSE(engine.voices.ReleaseObserved(source.release));
        ASSERT_NE(nullptr, engine.voices.Resolve(voice));
        ASSERT_TRUE(engine.voices.Resolve(voice)->tailStarted);
        const auto publishedSequence = engine.sourceTerminals.Resolve(source.token)->completedSequence.load();
        engine.voices.ApplyStop(voice, 0, 0, engine.notifications, engine.mixer.BlocksRendered());
        engine.nullBackend.Pump(1);
        EXPECT_TRUE(engine.voices.ReleaseObserved(source.release));
        ASSERT_TRUE(engine.sourceTerminals.Capture(source.token, engine.mixer.BlocksRendered(), &result));
        EXPECT_EQ(harken::SourceTerminalKind::Natural, result.kind);
        EXPECT_EQ(64u, result.frame);
        EXPECT_EQ(publishedSequence, engine.sourceTerminals.Resolve(source.token)->completedSequence.load());
        EXPECT_TRUE(engine.playableInstances.HasSourceRetirementHold(source.leaf));
    }
}

TEST(VoicePool, SourceTerminalUnjoinedWorkerRetainsSourceAndLeaf) {
    struct WorkerGate {
        std::thread::id coordinator = std::this_thread::get_id();
        std::atomic<bool> entered{false};
        std::atomic<bool> release{false};
        std::atomic<bool> exited{false};
    } gate;
    auto config = VoicePoolConfig(2, kBlockFrames);
    config.renderWorkerCount = 1;
    config.fxSlotsPerVoice = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const auto samples = harkentest::MakeDC(64, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    struct WorkerJoinGuard {
        WorkerGate& gate;
        harken::VoicePool& voices;
        ~WorkerJoinGuard() {
            gate.release.store(true, std::memory_order_release);
            harken::detail::VoicePoolReleaseTestAccess::JoinWorkers(voices);
        }
    } join{gate, engine.voices};
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    std::array<HeldSource, 2> sources{};
    ASSERT_EQ(1u, engine.voices.RenderWorkerCount());
    for (auto& source : sources) {
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &voice));
        ASSERT_TRUE(BindHeldSource(engine, voice, &source));
        auto* context = engine.voices.Resolve(voice);
        ASSERT_NE(nullptr, context);
        context->fxSlots[0].state = &gate;
        context->fxSlots[0].process = [](void* opaque, float* const*, std::uint32_t,
                                        std::uint32_t, std::uint32_t) {
            auto& state = *static_cast<WorkerGate*>(opaque);
            if (std::this_thread::get_id() == state.coordinator) {
                const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(1);
                while (!state.entered.load(std::memory_order_acquire) &&
                       std::chrono::steady_clock::now() < deadline) std::this_thread::yield();
            } else {
                state.entered.store(true, std::memory_order_release);
                while (!state.release.load(std::memory_order_acquire)) std::this_thread::yield();
                state.exited.store(true, std::memory_order_release);
            }
        };
        engine.voices.ApplyPlay(voice, voice, 0, 0);
    }
    engine.mixer.ArenaForTests().SetCommitPageLimitForTests(0);
    harken::detail::VoicePoolReleaseTestAccess::SetWorkerDeadline(
        engine.voices, harken::CpuClockTicksPerSecond() / 20u);
    const auto completed = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_TRUE(gate.entered.load(std::memory_order_acquire));
    EXPECT_FALSE(gate.exited.load(std::memory_order_acquire));
    const HeldSource* unjoined = nullptr;
    for (const auto& source : sources) {
        if (!engine.voices.ReleaseObserved(source.release)) unjoined = &source;
    }
    ASSERT_NE(nullptr, unjoined);
    harken::SourceTerminalPayload result{};
    EXPECT_FALSE(engine.sourceTerminals.Capture(unjoined->token, UINT64_MAX, &result));
    EXPECT_TRUE(engine.playableInstances.HasSourceRetirementHold(unjoined->leaf));
    EXPECT_TRUE(engine.playableInstances.Classify(unjoined->leaf).live);
    EXPECT_EQ(harken::SourceTerminalKind::None,
        engine.sourceTerminals.Resolve(unjoined->token)->bypassResult.load());
    VnVoiceHandle following = 0;
    EXPECT_EQ(VN_NO_VOICE, ClaimObserverVoice(engine, &following));
    const auto overflows = engine.mixer.Arena().OverflowCount();
    engine.nullBackend.Pump(1);
    EXPECT_EQ(overflows, engine.mixer.Arena().OverflowCount());
    EXPECT_EQ(completed, engine.mixer.BlocksRendered());
    EXPECT_FALSE(gate.exited.load(std::memory_order_acquire));
    gate.release.store(true, std::memory_order_release);
    harken::detail::VoicePoolReleaseTestAccess::JoinWorkers(engine.voices);
    EXPECT_TRUE(gate.exited.load(std::memory_order_acquire));
    EXPECT_FALSE(engine.sourceTerminals.Capture(unjoined->token, UINT64_MAX, &result));
    EXPECT_FALSE(engine.voices.ReleaseObserved(unjoined->release));
    EXPECT_TRUE(engine.playableInstances.HasSourceRetirementHold(unjoined->leaf));
    engine.nullBackend.Pump(1);
    EXPECT_EQ(completed + 1u, engine.mixer.BlocksRendered());
    for (const auto& source : sources) {
        EXPECT_TRUE(engine.voices.ReleaseObserved(source.release));
        ASSERT_TRUE(engine.sourceTerminals.Capture(source.token,
            engine.mixer.BlocksRendered(), &result));
        EXPECT_EQ(harken::SourceTerminalKind::Natural, result.kind);
        EXPECT_EQ(64u, result.frame);
    }
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &following));
    engine.voices.ApplyPlay(following, following, 0, 0);
    engine.nullBackend.Pump(1);
    EXPECT_EQ(overflows, engine.mixer.Arena().OverflowCount());
    EXPECT_EQ(nullptr, engine.voices.Resolve(following));
}

TEST(VoicePool, SourceTerminalCapacityGenerationReuseAndExhaustionStayBounded) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto samples = harkentest::MakeDC(64, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto& engine = *session.Engine();
    ASSERT_EQ(engine.playableInstances.Capacity(), engine.sourceTerminals.Capacity());
    std::vector<HeldSource> sources(engine.sourceTerminals.Capacity());
    for (auto& source : sources) {
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &voice, true));
        ASSERT_TRUE(BindHeldSource(engine, voice, &source));
        EXPECT_FALSE(engine.sourceTerminals.Reserve(engine.playableInstances,
            source.leaf, source.epoch, voice, source.release, 0).Valid());
        ASSERT_TRUE(engine.voices.CancelPreparedVoice(voice));
        EXPECT_TRUE(engine.voices.ReleaseObserved(source.release));
        EXPECT_TRUE(engine.playableInstances.HasSourceRetirementHold(source.leaf));
    }
    EXPECT_EQ(0u, engine.playableInstances.FreeCount());
    EXPECT_EQ(0u, engine.playableInstances.Acquire(harken::PlayableKind::Sound, 0));
    EXPECT_EQ(sources.size(), engine.playableInstances.LiveCount());
    const auto first = sources.front();
    auto release = [&](const HeldSource& source) {
        EXPECT_TRUE(engine.playableInstances.ReleaseSourceRetirementHold(source.leaf));
        EXPECT_TRUE(engine.sourceTerminals.Release(source.token));
        EXPECT_TRUE(engine.playableInstances.AdvanceLifecycle(source.leaf, harken::PlayableLifecycle::Retired));
    };
    release(first);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &voice));
    HeldSource reused{};
    ASSERT_TRUE(BindHeldSource(engine, voice, &reused));
    EXPECT_EQ(first.token.slot, reused.token.slot);
    EXPECT_EQ(first.token.generation + 1u, reused.token.generation);
    EXPECT_NE(first.leaf, reused.leaf);
    EXPECT_FALSE(engine.playableInstances.ReleaseSourceRetirementHold(first.leaf));
    const auto newEpoch = engine.playableInstances.BumpScheduleEpoch(reused.leaf);
    EXPECT_NE(0u, newEpoch);
    EXPECT_NE(reused.epoch, newEpoch);
    EXPECT_EQ(reused.epoch, engine.sourceTerminals.Resolve(reused.token)->acceptedEpoch);
    engine.voices.ApplyPlay(voice, voice, 0, 0);
    engine.nullBackend.Pump(1);
    harken::SourceTerminalPayload result{};
    EXPECT_FALSE(engine.sourceTerminals.Capture(first.token, UINT64_MAX, &result));
    ASSERT_TRUE(engine.sourceTerminals.Capture(reused.token, engine.mixer.BlocksRendered(), &result));
    EXPECT_EQ(harken::SourceTerminalKind::Natural, result.kind);
    EXPECT_TRUE(engine.voices.ReleaseObserved(reused.release));
    EXPECT_TRUE(engine.playableInstances.HasSourceRetirementHold(reused.leaf));
    release(reused);

    engine.sourceTerminals.At(first.token.slot)->generation = UINT32_MAX;
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &voice, true));
    HeldSource exhausted{};
    ASSERT_TRUE(BindHeldSource(engine, voice, &exhausted));
    ASSERT_EQ(UINT32_MAX, exhausted.token.generation);
    ASSERT_TRUE(engine.voices.CancelPreparedVoice(voice));
    release(exhausted);
    EXPECT_TRUE(engine.sourceTerminals.At(first.token.slot)->quarantined);
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &voice, true));
    HeldSource refused{};
    EXPECT_FALSE(BindHeldSource(engine, voice, &refused));
    EXPECT_FALSE(refused.token.Valid());
    EXPECT_FALSE(engine.sourceTerminals.Capture(exhausted.token, UINT64_MAX, &result));
    ASSERT_TRUE(engine.voices.CancelPreparedVoice(voice));
    ASSERT_TRUE(engine.playableInstances.ReleaseSourceRetirementHold(refused.leaf));
    ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(refused.leaf, harken::PlayableLifecycle::Retired));
    for (std::size_t index = 1; index < sources.size(); ++index) release(sources[index]);
    EXPECT_EQ(engine.playableInstances.Capacity(), engine.playableInstances.FreeCount());
    EXPECT_EQ(0u, engine.voices.ActiveCount());
    for (std::uint32_t slot = 0; slot < engine.sourceTerminals.Capacity(); ++slot)
        EXPECT_FALSE(engine.sourceTerminals.At(slot)->occupied);
}

TEST(VoicePool, SourceOwnerCancelFailureRetainsPreparedAndCommittedTree) {
    for (const bool finalOwnerFailure : {false, true}) {
        SCOPED_TRACE(finalOwnerFailure);
        EngineSession session(VoicePoolConfig(1, kBlockFrames));
        ASSERT_EQ(VN_OK, session.InitResult());
        const auto samples = harkentest::MakeDC(64, 0.25f);
        const auto desc = MonoDesc(samples, kSampleRate);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
        auto& engine = *session.Engine();
        const auto freeBefore = engine.playableInstances.FreeCount();
        const auto containersBefore = engine.containerPlayback.FreeCount();
        harken::PlayableTreeExecutor executor(2);
        ASSERT_TRUE(executor.PrepareInstance(engine.playableInstances, harken::PlayableKind::Container));
        const auto root = executor.PreparedInstanceHandle(0);
        ASSERT_TRUE(executor.PrepareContainer(engine.playableInstances,
            engine.containerPlayback, root, executor.PreparedInstanceEpoch(0), harkentest::ObjectId(41), 42, 0, 1));
        ASSERT_TRUE(executor.PrepareInstance(engine.playableInstances, harken::PlayableKind::Sound, root));
        const auto leaf = executor.PreparedInstanceHandle(1);
        const auto epoch = executor.PreparedInstanceEpoch(1);
        ASSERT_TRUE(engine.playableInstances.AcquireSourceRetirementHold(leaf, epoch));
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &voice, true));
        harken::VoiceReleaseObserver release{};
        ASSERT_TRUE(engine.voices.CaptureReleaseObserver(voice, &release));
        const auto token = engine.sourceTerminals.Reserve(engine.playableInstances, leaf, epoch, voice, release, root);
        ASSERT_TRUE(token.Valid());
        ASSERT_TRUE(engine.voices.BindSourceTerminal(voice, engine.sourceTerminals.Resolve(token), token));
        struct Owner {
            harken::Engine* engine;
            VnVoiceHandle voice;
            harken::SourceTerminalToken token;
            std::uint32_t attempts = 0;
        } owner{&engine, voice, token};
        ASSERT_TRUE(executor.AdoptPreparedOwnerToken(&owner,
            [](void*) noexcept { return true; },
            [](void*, std::uint64_t) noexcept { return false; },
            [](void* opaque) noexcept {
                auto& value = *static_cast<Owner*>(opaque);
                if (++value.attempts == 1) return false;
                auto* record = value.engine->sourceTerminals.Resolve(value.token);
                if (record == nullptr || !value.engine->voices.CancelPreparedVoice(value.voice) ||
                    !value.engine->voices.ReleaseObserved(record->release)) return false;
                return value.engine->playableInstances.ReleaseSourceRetirementHold(record->leaf) &&
                    value.engine->sourceTerminals.Release(value.token);
            }));
        if (finalOwnerFailure) EXPECT_FALSE(executor.Commit());
        else EXPECT_FALSE(executor.Cancel(engine.playableInstances));
        EXPECT_EQ(1u, owner.attempts);
        EXPECT_EQ(1u, executor.OwnerTokenCount());
        EXPECT_EQ(2u, executor.InstanceReservationCount());
        EXPECT_EQ(1u, executor.ContainerReservationCount());
        EXPECT_EQ(leaf, executor.PreparedInstanceHandle(1));
        EXPECT_EQ(freeBefore - 2u, engine.playableInstances.FreeCount());
        EXPECT_EQ(containersBefore - 1u, engine.containerPlayback.FreeCount());
        EXPECT_TRUE(engine.playableInstances.HasSourceRetirementHold(leaf));
        EXPECT_FALSE(engine.voices.ReleaseObserved(release));
        ASSERT_NE(nullptr, engine.sourceTerminals.Resolve(token));
        if (finalOwnerFailure) {
            harken::ContainerPlaybackRow row{};
            EXPECT_TRUE(engine.containerPlayback.Capture(root, &row));
            EXPECT_TRUE(engine.playableInstances.Classify(root).live);
            EXPECT_TRUE(engine.playableInstances.Classify(leaf).live);
            EXPECT_TRUE(executor.RollbackCommitted(engine.playableInstances));
        } else EXPECT_TRUE(executor.Cancel(engine.playableInstances));
        EXPECT_EQ(2u, owner.attempts);
        EXPECT_EQ(0u, executor.OwnerTokenCount());
        EXPECT_EQ(0u, executor.InstanceReservationCount());
        EXPECT_EQ(0u, executor.ContainerReservationCount());
        EXPECT_EQ(freeBefore, engine.playableInstances.FreeCount());
        EXPECT_EQ(containersBefore, engine.containerPlayback.FreeCount());
        EXPECT_TRUE(engine.voices.ReleaseObserved(release));
        EXPECT_EQ(nullptr, engine.sourceTerminals.Resolve(token));
        EXPECT_EQ(token.generation + 1u, engine.sourceTerminals.At(token.slot)->generation);
        EXPECT_FALSE(engine.playableInstances.HasSourceRetirementHold(leaf));
    }
}

TEST(VoicePool, PreparedCommitHoldFreezesBindingsAndAbortRetainsOriginalCancel) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto samples = harkentest::MakeDC(64, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto& engine = *session.Engine();
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &voice, true));
    HeldSource source{};
    ASSERT_TRUE(BindHeldSource(engine, voice, &source));
    harken::CompletionProgressRow row;
    harken::CompletionProgressSnapshot initial{};
    initial.occurrence = {13, 17};
    initial.renderGeneration = static_cast<std::uint32_t>(voice >> 32);
    initial.playbackFactsRevision = 29;
    ASSERT_TRUE(harken::InitializeCompletionProgress(row, initial));
    ASSERT_TRUE(engine.voices.BindCompletionProgress(voice, &row,
        initial.occurrence, initial.playbackFactsRevision));
    harken::VoicePool::PreparedCommitInput input{};
    input.renderHandle = voice;
    input.originalRelease = source.release;
    input.sourceTerminal = engine.sourceTerminals.Resolve(source.token);
    input.sourceTerminalToken = source.token;
    input.completionProgress = &row;
    input.completionOccurrence = initial.occurrence;
    input.completionRenderGeneration = initial.renderGeneration;
    input.playbackFactsRevision = initial.playbackFactsRevision;
    ASSERT_TRUE(engine.voices.CapturePreparedRoutingV1(voice, &input.routing));
    const auto gainBefore = engine.voices.Resolve(voice)->gain.target;
    const auto ratioBefore = engine.voices.Resolve(voice)->resampler.CurrentRatio();
    harken::VoicePool::PreparedCommitProof proof;
    ASSERT_TRUE(engine.voices.ProvePreparedCommit(input, &proof));
    EXPECT_EQ(nullptr, engine.voices.Resolve(voice));
    EXPECT_FALSE(engine.voices.CancelPreparedVoice(voice));
    EXPECT_FALSE(engine.voices.CommitPreparedVoice(voice));
    EXPECT_FALSE(engine.voices.AbortClaimedVoice(voice));
    EXPECT_FALSE(engine.voices.BindSourceTerminal(voice, input.sourceTerminal, source.token));
    EXPECT_FALSE(engine.voices.BindCompletionProgress(voice, &row,
        initial.occurrence, initial.playbackFactsRevision));
    auto migrated = input.routing;
    migrated.busIndex = 5;
    EXPECT_FALSE(engine.voices.CanCommitPreparedRoutingMigrationV1(voice, input.routing, migrated));
    engine.voices.CommitPreparedRoutingMigrationV1(voice, input.routing, migrated);
    engine.voices.ApplySetVolume(voice, -20.0f, 0);
    engine.voices.ApplySetPitch(voice, 2.0, 0);
    engine.voices.ApplySetPosition(voice, 1, 2, 3);
    engine.voices.CommitSeek(voice, 32);
    engine.voices.ApplyPlay(voice, voice, 0, 0);
    engine.voices.ApplyStop(voice, 0, 0, engine.notifications, 0);
    engine.nullBackend.Pump(2);
    for (std::uint32_t i = 0; i < kBlockFrames * engine.config.mixChannelCount; ++i)
        EXPECT_EQ(0.0f, engine.nullBackend.LastBlock()[i]);
    harken::CompletionProgressSnapshot unchanged{};
    ASSERT_TRUE(harken::CaptureCompletionProgressExact(row,
        initial.occurrence, initial.renderGeneration, &unchanged));
    EXPECT_EQ(0u, unchanged.progressRevision);
    EXPECT_EQ(0u, unchanged.completedThroughBlockSequence);
    EXPECT_FALSE(engine.voices.ReleaseObserved(source.release));
    EXPECT_EQ(1u, engine.registry.Lookup(harkentest::ObjectId(1)).useCount->load());
    harken::VoicePool::PreparedRoutingV1 captured{};
    ASSERT_TRUE(engine.voices.CapturePreparedRoutingV1(voice, &captured));
    EXPECT_EQ(input.routing, captured);
    harken::VoicePool::PreparedCommitProof moved(std::move(proof));
    EXPECT_FALSE(proof.Valid());
    EXPECT_TRUE(moved.Valid());
    moved.Release();
    const auto* context = engine.voices.Resolve(voice);
    ASSERT_NE(nullptr, context);
    EXPECT_EQ(gainBefore, context->gain.target);
    EXPECT_EQ(ratioBefore, context->resampler.CurrentRatio());
    EXPECT_EQ(input.sourceTerminal, context->sourceTerminal);
    EXPECT_EQ(&row, context->completionProgress);
    EXPECT_TRUE(engine.voices.CancelPreparedVoice(voice));
    EXPECT_TRUE(engine.voices.ReleaseObserved(source.release));
    EXPECT_EQ(0u, engine.registry.Lookup(harkentest::ObjectId(1)).useCount->load());
    EXPECT_TRUE(engine.playableInstances.ReleaseSourceRetirementHold(source.leaf));
    EXPECT_TRUE(engine.sourceTerminals.Release(source.token));
}

TEST(VoicePool, PreparedCommitConsumePreservesRealNaturalFinalProgressAndRelease) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto samples = harkentest::MakeDC(64, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto& engine = *session.Engine();
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &voice, true));
    HeldSource source{};
    ASSERT_TRUE(BindHeldSource(engine, voice, &source));
    harken::CompletionProgressRow row;
    harken::CompletionProgressSnapshot initial{};
    initial.occurrence = {7, 11};
    initial.renderGeneration = static_cast<std::uint32_t>(voice >> 32);
    initial.playbackFactsRevision = 19;
    ASSERT_TRUE(harken::InitializeCompletionProgress(row, initial));
    ASSERT_TRUE(engine.voices.BindCompletionProgress(voice, &row,
        initial.occurrence, initial.playbackFactsRevision));
    harken::VoicePool::PreparedCommitInput input{};
    input.renderHandle = voice;
    input.originalRelease = source.release;
    input.sourceTerminal = engine.sourceTerminals.Resolve(source.token);
    input.sourceTerminalToken = source.token;
    input.completionProgress = &row;
    input.completionOccurrence = initial.occurrence;
    input.completionRenderGeneration = initial.renderGeneration;
    input.playbackFactsRevision = initial.playbackFactsRevision;
    ASSERT_TRUE(engine.voices.CapturePreparedRoutingV1(voice, &input.routing));
    harken::VoicePool::PreparedCommitProof proof;
    ASSERT_TRUE(engine.voices.ProvePreparedCommit(input, &proof));
    harken::VoicePool::ConsumePreparedCommit(std::move(proof));
    EXPECT_FALSE(proof.Valid());
    EXPECT_FALSE(engine.voices.CommitPreparedVoice(voice));
    EXPECT_FALSE(engine.voices.CancelPreparedVoice(voice));
    EXPECT_FALSE(engine.voices.ReleaseObserved(source.release));
    engine.voices.ApplyPlay(voice, voice, 0, 0);
    engine.nullBackend.Pump(1);
    EXPECT_EQ(nullptr, engine.voices.Resolve(voice));
    EXPECT_TRUE(engine.voices.ReleaseObserved(source.release));
    const float* block = engine.nullBackend.LastBlock();
    EXPECT_GT(*std::max_element(block, block + kBlockFrames * engine.config.mixChannelCount), 0.01f);
    harken::CompletionProgressSnapshot final{};
    ASSERT_TRUE(harken::CaptureCompletionProgressExact(row,
        initial.occurrence, initial.renderGeneration, &final));
    EXPECT_EQ(64u, final.logicalOutputFramesProduced);
    EXPECT_EQ(1u, final.completedThroughBlockSequence);
    EXPECT_EQ(1u, final.progressRevision);
    EXPECT_EQ(1u, final.logicalSourceExhausted);
    harken::SourceTerminalPayload terminal{};
    ASSERT_TRUE(engine.sourceTerminals.Capture(source.token, engine.mixer.BlocksRendered(), &terminal));
    EXPECT_EQ(harken::SourceTerminalKind::Natural, terminal.kind);
    EXPECT_EQ(64u, terminal.frame);
    EXPECT_EQ(0u, engine.registry.Lookup(harkentest::ObjectId(1)).useCount->load());
    EXPECT_TRUE(engine.playableInstances.ReleaseSourceRetirementHold(source.leaf));
    EXPECT_TRUE(engine.sourceTerminals.Release(source.token));
}

TEST(VoicePool, PreparedCommitOriginalObserverRejectsSameHandleCancelReuseAndMaximum) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto samples = harkentest::MakeDC(64, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto& engine = *session.Engine();
    harken::VoicePool::PreparedCommitInput original{};
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &original.renderHandle, true));
    ASSERT_TRUE(engine.voices.CaptureReleaseObserver(original.renderHandle, &original.originalRelease));
    ASSERT_TRUE(engine.voices.CapturePreparedRoutingV1(original.renderHandle, &original.routing));
    ASSERT_TRUE(engine.voices.CancelPreparedVoice(original.renderHandle));
    VnVoiceHandle reused = 0;
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &reused, true));
    ASSERT_EQ(original.renderHandle, reused);
    harken::VoicePool::PreparedCommitProof proof;
    EXPECT_FALSE(engine.voices.ProvePreparedCommit(original, &proof));
    EXPECT_FALSE(proof.Valid());
    auto current = original;
    ASSERT_TRUE(engine.voices.CaptureReleaseObserver(reused, &current.originalRelease));
    EXPECT_EQ(original.originalRelease.targetSequence + 1u, current.originalRelease.targetSequence);
    ASSERT_TRUE(engine.voices.ProvePreparedCommit(current, &proof));
    proof.Release();
    ASSERT_TRUE(engine.voices.CancelPreparedVoice(reused));
    harken::detail::VoicePoolReleaseTestAccess::SetReleaseSequence(engine.voices, UINT64_MAX - 1u);
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &current.renderHandle, true));
    ASSERT_TRUE(engine.voices.CaptureReleaseObserver(current.renderHandle, &current.originalRelease));
    ASSERT_EQ(UINT64_MAX, current.originalRelease.targetSequence);
    ASSERT_TRUE(engine.voices.ProvePreparedCommit(current, &proof));
    proof.Release();
    ASSERT_TRUE(engine.voices.CancelPreparedVoice(current.renderHandle));
    EXPECT_TRUE(engine.voices.ReleaseObserved(current.originalRelease));
    EXPECT_FALSE(engine.voices.ProvePreparedCommit(current, &proof));
    EXPECT_FALSE(engine.voices.HasFreeSlot());
    EXPECT_EQ(VN_NO_VOICE, ClaimObserverVoice(engine, &reused, true));
}

TEST(VoicePool, PreparedCommitWrongMetadataAndMoveAssignmentRetainActualSlots) {
    EngineSession session(VoicePoolConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto samples = harkentest::MakeDC(64, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto& engine = *session.Engine();
    std::array<harken::VoicePool::PreparedCommitInput, 2> inputs{};
    for (auto& input : inputs) {
        ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &input.renderHandle, true));
        ASSERT_TRUE(engine.voices.CaptureReleaseObserver(input.renderHandle, &input.originalRelease));
        ASSERT_TRUE(engine.voices.CapturePreparedRoutingV1(input.renderHandle, &input.routing));
    }
    harken::SourceTerminalRecord otherSource;
    harken::CompletionProgressRow otherProgress;
    harken::StreamRing otherRing;
    for (std::uint32_t wrong = 0; wrong < 12; ++wrong) {
        SCOPED_TRACE(wrong);
        auto bad = inputs[1];
        switch (wrong) {
        case 0: bad.renderHandle ^= std::uint64_t{1} << 32; break;
        case 1: bad.originalRelease.slot = 0; break;
        case 2: ++bad.originalRelease.targetSequence; break;
        case 3: bad.routing.busIndex = 9; break;
        case 4: ++bad.routing.busGeneration; break;
        case 5: bad.routing.sends[0] = {true, 9, 1}; break;
        case 6: bad.sourceTerminalToken = {1, 2}; break;
        case 7: bad.sourceTerminal = &otherSource; bad.sourceTerminalToken = {1, 2}; break;
        case 8: bad.completionOccurrence = {1, 2}; break;
        case 9: bad.completionProgress = &otherProgress; bad.completionOccurrence = {1, 2};
            bad.completionRenderGeneration = static_cast<std::uint32_t>(bad.renderHandle >> 32);
            bad.playbackFactsRevision = 3; break;
        case 10: bad.streamRingToken = 2; break;
        case 11: bad.streamRing = &otherRing; bad.streamRingToken = 2; bad.streamGeneration = 1; break;
        }
        harken::VoicePool::PreparedCommitProof refused;
        EXPECT_FALSE(engine.voices.ProvePreparedCommit(bad, &refused));
        EXPECT_FALSE(refused.Valid());
        EXPECT_NE(nullptr, engine.voices.Resolve(inputs[1].renderHandle));
        EXPECT_FALSE(engine.voices.ReleaseObserved(inputs[1].originalRelease));
    }
    EXPECT_FALSE(engine.voices.ProvePreparedCommit(inputs[0], nullptr));
    harken::VoicePool::PreparedCommitProof first, second;
    ASSERT_TRUE(engine.voices.ProvePreparedCommit(inputs[0], &first));
    EXPECT_FALSE(engine.voices.ProvePreparedCommit(inputs[1], &first));
    EXPECT_FALSE(engine.voices.ProvePreparedCommit(inputs[0], &second));
    ASSERT_TRUE(engine.voices.ProvePreparedCommit(inputs[1], &second));
    first = std::move(second);
    EXPECT_FALSE(second.Valid());
    EXPECT_TRUE(first.Valid());
    EXPECT_TRUE(engine.voices.CancelPreparedVoice(inputs[0].renderHandle));
    EXPECT_FALSE(engine.voices.CancelPreparedVoice(inputs[1].renderHandle));
    first.Release();
    EXPECT_TRUE(engine.voices.CancelPreparedVoice(inputs[1].renderHandle));
    EXPECT_EQ(0u, engine.registry.Lookup(harkentest::ObjectId(1)).useCount->load());
    EXPECT_EQ(2u, engine.voices.AvailableSlotCount());
}

TEST(VoicePool, PreparedCommitStreamRequiresOriginalRingAndAllowsProducerFill) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    std::array<float, 64> storage{};
    float* planes[harken::kMaxChannels] = {};
    planes[0] = storage.data();
    harken::StreamRing ring;
    ring.Wire(planes, 64);
    ring.ResetForUse(1);
    const auto generation = ring.generation.load();
    const auto ringToken = (static_cast<std::uint64_t>(generation) << 32) | 1u;
    harken::VoicePool::PreparedCommitInput input{};
    const auto pan = harken::ResolvePanGains(engine.config.mixChannelLayout, 0.0f);
    ASSERT_EQ(VN_OK, engine.voices.PlayStream(&ring, 1, kSampleRate, 0, 0, 1, 0,
        false, 0, 0, 0, pan, 0, 64, 0, 64, 1,
        harken::ResampleProfile::Professional44148, &input.renderHandle,
        1, UINT32_MAX, 0, nullptr, 0, true));
    ASSERT_TRUE(engine.voices.BindStreamRingToken(input.renderHandle, &ring, ringToken));
    input.streamRing = &ring;
    input.streamRingToken = ringToken;
    input.streamGeneration = generation;
    ASSERT_TRUE(engine.voices.CaptureReleaseObserver(input.renderHandle, &input.originalRelease));
    ASSERT_TRUE(engine.voices.CapturePreparedRoutingV1(input.renderHandle, &input.routing));
    harken::VoicePool::PreparedCommitProof proof;
    auto bad = input;
    ++bad.streamRingToken;
    EXPECT_FALSE(engine.voices.ProvePreparedCommit(bad, &proof));
    bad = input;
    ++bad.streamGeneration;
    EXPECT_FALSE(engine.voices.ProvePreparedCommit(bad, &proof));
    ASSERT_TRUE(engine.voices.ProvePreparedCommit(input, &proof));
    EXPECT_FALSE(engine.voices.BindStreamRingToken(input.renderHandle, &ring, ringToken));
    float* writable[harken::kMaxChannels] = {};
    ASSERT_GE(ring.ContiguousWrite(writable), 16u);
    std::fill_n(writable[0], 16, 0.25f);
    ring.PublishWrite(16);
    std::uint64_t captured = 0;
    ASSERT_TRUE(engine.voices.CaptureStreamRingToken(input.renderHandle, &captured));
    EXPECT_EQ(ringToken, captured);
    EXPECT_FALSE(engine.voices.CancelPreparedVoice(input.renderHandle));
    proof.Release();
    ASSERT_TRUE(engine.voices.ProvePreparedCommit(input, &proof));
    proof.Release();
    EXPECT_TRUE(engine.voices.CancelPreparedVoice(input.renderHandle));
    EXPECT_TRUE(engine.voices.ReleaseObserved(input.originalRelease));
    EXPECT_EQ(harken::StreamSlotState::Retired, ring.state.load());
}

namespace {

bool CaptureOriginalPreparation(harken::Engine& engine, VnVoiceHandle handle,
    harken::VoicePool::PreparedCommitProof* proof) {
    harken::VoicePool::PreparedCommitInput input{};
    input.renderHandle = handle;
    return engine.voices.CaptureReleaseObserver(handle, &input.originalRelease) &&
        engine.voices.CapturePreparedRoutingV1(handle, &input.routing) &&
        engine.voices.ProvePreparedCommit(input, proof);
}

void ExpectFreshOriginalPlayback(harken::Engine& engine, VnVoiceHandle handle) {
    const auto* before = engine.voices.Resolve(handle);
    ASSERT_NE(nullptr, before);
    const auto frame = before->resampler.SourceFrame();
    const auto block = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(block + 1u, engine.mixer.BlocksRendered());
    const auto* after = engine.voices.Resolve(handle);
    ASSERT_NE(nullptr, after);
    EXPECT_EQ(frame + engine.config.blockFrames, after->resampler.SourceFrame());
    const auto* pcm = engine.nullBackend.LastBlock();
    ASSERT_NE(nullptr, pcm);
    const auto count = engine.config.blockFrames * engine.config.mixChannelCount;
    EXPECT_GT(*std::max_element(pcm, pcm + count), 0.01f);
}

bool PrepareOriginalArm(harken::Engine& engine, VnVoiceHandle handle,
    harken::TimelineArmToken* arm) {
    return engine.timelineArms.PreparePendingImmediate(static_cast<std::uint32_t>(handle),
        static_cast<std::uint32_t>(handle >> 32), 7, {}, arm) &&
        engine.timelineArms.CommitPrepared(*arm);
}

void RetireOriginalArm(harken::Engine& engine, harken::TimelineArmToken arm) {
    ASSERT_TRUE(engine.timelineArms.RequestCancel(arm));
    ASSERT_TRUE(engine.timelineArms.Tombstone(arm, engine.mixer.BlocksRendered()));
    const auto block = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(block + 1u, engine.mixer.BlocksRendered());
    EXPECT_TRUE(engine.timelineArms.Reclaim(arm, engine.mixer.BlocksRendered()));
}

} // namespace

TEST(VoicePool, OriginalOwnerRequiresGenuinePreparationAndRejectsDuplicateIssuance) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto samples = harkentest::MakeDC(4096, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto& engine = *session.Engine();
    harken::VoicePool foreign(engine.config, &engine.tables, &engine.multirateTables,
        &engine.biquadTrig, engine.effectBank, engine.dynamicRoutes);
    harken::VoicePool::PreparedCommitProof proof;
    harken::VoicePool::OriginalVoiceOwner owner, other;
    EXPECT_FALSE(engine.voices.IssueOriginalVoiceOwner(proof, &owner));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &handle, true));
    ASSERT_TRUE(CaptureOriginalPreparation(engine, handle, &proof));
    EXPECT_FALSE(foreign.IssueOriginalVoiceOwner(proof, &owner));
    EXPECT_FALSE(engine.voices.IssueOriginalVoiceOwner(proof, nullptr));
    ASSERT_TRUE(engine.voices.IssueOriginalVoiceOwner(proof, &owner));
    EXPECT_TRUE(proof.Valid());
    EXPECT_FALSE(engine.voices.IssueOriginalVoiceOwner(proof, &owner));
    EXPECT_FALSE(engine.voices.IssueOriginalVoiceOwner(proof, &other));
    EXPECT_EQ(harken::VoicePool::OriginalVoiceDisposal::Refused,
        foreign.TryDisposeOriginalTimelineVoice(owner));
    EXPECT_EQ(harken::VoicePool::OriginalVoiceDisposal::Deferred,
        engine.voices.TryDisposeOriginalTimelineVoice(owner));
    EXPECT_EQ(1u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
    owner.Release();
    EXPECT_EQ(0u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
    EXPECT_FALSE(engine.voices.IssueOriginalVoiceOwner(proof, &other));
    proof.Release();
    ASSERT_TRUE(CaptureOriginalPreparation(engine, handle, &proof));
    EXPECT_FALSE(engine.voices.IssueOriginalVoiceOwner(proof, &other));
    proof.Release();
    EXPECT_EQ(1u, engine.registry.Lookup(harkentest::ObjectId(1)).useCount->load());
    EXPECT_TRUE(engine.voices.CancelPreparedVoice(handle));
}

TEST(VoicePool, OriginalOwnerMovesAndDropsWithoutReleasingEitherOriginalVoice) {
    EngineSession session(VoicePoolConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto samples = harkentest::MakeDC(4096, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto& engine = *session.Engine();
    std::array<VnVoiceHandle, 2> handles{};
    std::array<harken::VoicePool::OriginalVoiceOwner, 2> owners;
    for (std::size_t i = 0; i < handles.size(); ++i) {
        ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &handles[i], true));
        harken::VoicePool::PreparedCommitProof proof;
        ASSERT_TRUE(CaptureOriginalPreparation(engine, handles[i], &proof));
        ASSERT_TRUE(engine.voices.IssueOriginalVoiceOwner(proof, &owners[i]));
        harken::VoicePool::ConsumePreparedCommit(std::move(proof));
        EXPECT_FALSE(engine.voices.IssueOriginalVoiceOwner(proof, &owners[i]));
    }
    EXPECT_EQ(2u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
    harken::VoicePool::OriginalVoiceOwner moved(std::move(owners[0]));
    EXPECT_FALSE(owners[0].Valid());
    EXPECT_TRUE(moved.Valid());
    moved = std::move(moved);
    EXPECT_TRUE(moved.Valid());
    moved = std::move(owners[1]);
    EXPECT_FALSE(owners[1].Valid());
    EXPECT_EQ(1u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
    moved.Release();
    moved.Release();
    EXPECT_EQ(0u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
    EXPECT_EQ(2u, engine.registry.Lookup(harkentest::ObjectId(1)).useCount->load());
    for (const auto handle : handles) {
        ASSERT_NE(nullptr, engine.voices.Resolve(handle));
        EXPECT_TRUE(engine.voices.AbortClaimedVoice(handle));
    }
    EXPECT_EQ(0u, engine.registry.Lookup(harkentest::ObjectId(1)).useCount->load());
}

TEST(VoicePool, OriginalOwnerPreparedRollbackDistinguishesRealSameHandleReissue) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto samples = harkentest::MakeDC(4096, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto& engine = *session.Engine();
    VnVoiceHandle original = 0;
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &original, true));
    harken::VoiceReleaseObserver released{};
    ASSERT_TRUE(engine.voices.CaptureReleaseObserver(original, &released));
    harken::VoicePool::PreparedCommitProof proof;
    ASSERT_TRUE(CaptureOriginalPreparation(engine, original, &proof));
    harken::VoicePool::OriginalVoiceOwner oldOwner, newOwner;
    ASSERT_TRUE(engine.voices.IssueOriginalVoiceOwner(proof, &oldOwner));
    proof.Release();
    EXPECT_EQ(harken::VoicePool::OriginalVoiceDisposal::Refused,
        engine.voices.TryDisposeOriginalTimelineVoice(oldOwner));
    ASSERT_TRUE(engine.voices.CancelPreparedVoice(original));
    ASSERT_TRUE(engine.voices.ReleaseObserved(released));
    VnVoiceHandle replacement = 0;
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &replacement, true));
    ASSERT_EQ(original, replacement);
    harken::VoiceReleaseObserver current{};
    ASSERT_TRUE(engine.voices.CaptureReleaseObserver(replacement, &current));
    ASSERT_EQ(released.targetSequence + 1u, current.targetSequence);
    ASSERT_TRUE(CaptureOriginalPreparation(engine, replacement, &proof));
    ASSERT_TRUE(engine.voices.IssueOriginalVoiceOwner(proof, &newOwner));
    EXPECT_EQ(harken::VoicePool::OriginalVoiceDisposal::AlreadyReleased,
        engine.voices.TryDisposeOriginalTimelineVoice(oldOwner));
    EXPECT_FALSE(engine.voices.ReleaseObserved(current));
    harken::VoicePool::ConsumePreparedCommit(std::move(proof));
    engine.voices.ApplyPlay(replacement, replacement, 0, engine.mixer.BlocksRendered());
    EXPECT_EQ(harken::VoicePool::OriginalVoiceDisposal::AlreadyReleased,
        engine.voices.TryDisposeOriginalTimelineVoice(oldOwner));
    EXPECT_EQ(harken::VoicePool::OriginalVoiceDisposal::Refused,
        engine.voices.TryDisposeOriginalTimelineVoice(newOwner));
    ExpectFreshOriginalPlayback(engine, replacement);
    EXPECT_FALSE(engine.voices.ReleaseObserved(current));
    engine.voices.ApplyStop(replacement, 0, 0, engine.notifications, engine.mixer.BlocksRendered());
    EXPECT_EQ(harken::VoicePool::OriginalVoiceDisposal::AlreadyReleased,
        engine.voices.TryDisposeOriginalTimelineVoice(newOwner));
}

TEST(VoicePool, OriginalOwnerClaimedDisposalPublishesExactlyOneActualRelease) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto samples = harkentest::MakeDC(4096, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto& engine = *session.Engine();
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &handle, true));
    HeldSource source{};
    ASSERT_TRUE(BindHeldSource(engine, handle, &source));
    harken::VoicePool::PreparedCommitInput input{};
    input.renderHandle = handle;
    input.originalRelease = source.release;
    input.sourceTerminal = engine.sourceTerminals.Resolve(source.token);
    input.sourceTerminalToken = source.token;
    ASSERT_TRUE(engine.voices.CapturePreparedRoutingV1(handle, &input.routing));
    harken::VoicePool::PreparedCommitProof proof;
    ASSERT_TRUE(engine.voices.ProvePreparedCommit(input, &proof));
    harken::VoicePool::OriginalVoiceOwner owner;
    ASSERT_TRUE(engine.voices.IssueOriginalVoiceOwner(proof, &owner));
    harken::VoicePool::ConsumePreparedCommit(std::move(proof));
    const auto completed = engine.voices.CompletedCount();
    ASSERT_EQ(harken::VoicePool::OriginalVoiceDisposal::Released,
        engine.voices.TryDisposeOriginalTimelineVoice(owner));
    EXPECT_TRUE(engine.voices.ReleaseObserved(source.release));
    EXPECT_EQ(completed + 1u, engine.voices.CompletedCount());
    EXPECT_EQ(0u, engine.registry.Lookup(harkentest::ObjectId(1)).useCount->load());
    EXPECT_EQ(harken::SourceTerminalKind::Cancelled, input.sourceTerminal->bypassResult.load());
    EXPECT_EQ(harken::VoicePool::OriginalVoiceDisposal::AlreadyReleased,
        engine.voices.TryDisposeOriginalTimelineVoice(owner));
    EXPECT_EQ(completed + 1u, engine.voices.CompletedCount());
    EXPECT_TRUE(engine.playableInstances.ReleaseSourceRetirementHold(source.leaf));
    EXPECT_TRUE(engine.sourceTerminals.Release(source.token));
}

TEST(VoicePool, OriginalOwnerNaturalExitCannotDisposeReissuedFreshPlayback) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto samples = harkentest::MakeDC(4096, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto& engine = *session.Engine();
    VnVoiceHandle first = 0;
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &first, true));
    harken::VoicePool::PreparedCommitProof proof;
    ASSERT_TRUE(CaptureOriginalPreparation(engine, first, &proof));
    harken::VoicePool::OriginalVoiceOwner owner;
    ASSERT_TRUE(engine.voices.IssueOriginalVoiceOwner(proof, &owner));
    harken::VoicePool::ConsumePreparedCommit(std::move(proof));
    engine.voices.ApplyPlay(first, first, samples.size() - 64, engine.mixer.BlocksRendered());
    const auto before = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    ASSERT_EQ(nullptr, engine.voices.Resolve(first));
    VnVoiceHandle next = 0;
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &next, true));
    ASSERT_EQ(static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(next));
    ASSERT_NE(first, next);
    ASSERT_TRUE(CaptureOriginalPreparation(engine, next, &proof));
    harken::VoicePool::OriginalVoiceOwner nextOwner;
    ASSERT_TRUE(engine.voices.IssueOriginalVoiceOwner(proof, &nextOwner));
    harken::VoicePool::ConsumePreparedCommit(std::move(proof));
    engine.voices.ApplyPlay(next, next, 0, engine.mixer.BlocksRendered());
    EXPECT_EQ(harken::VoicePool::OriginalVoiceDisposal::AlreadyReleased,
        engine.voices.TryDisposeOriginalTimelineVoice(owner));
    EXPECT_EQ(harken::VoicePool::OriginalVoiceDisposal::Refused,
        engine.voices.TryDisposeOriginalTimelineVoice(nextOwner));
    ExpectFreshOriginalPlayback(engine, next);
    engine.voices.ApplyStop(next, 0, 0, engine.notifications, engine.mixer.BlocksRendered());
}

TEST(VoicePool, OriginalOwnerGatedDisposalDoesNotInventCoreRetirement) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto samples = harkentest::MakeDC(4096, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto& engine = *session.Engine();
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &handle, true));
    harken::VoicePool::PreparedCommitProof proof;
    ASSERT_TRUE(CaptureOriginalPreparation(engine, handle, &proof));
    harken::VoicePool::OriginalVoiceOwner owner;
    ASSERT_TRUE(engine.voices.IssueOriginalVoiceOwner(proof, &owner));
    harken::VoicePool::ConsumePreparedCommit(std::move(proof));
    harken::TimelineArmToken arm{};
    ASSERT_TRUE(PrepareOriginalArm(engine, handle, &arm));
    harken::CommandRecord command{};
    command.opcode = harken::kOpcodePlay;
    command.payload.play.batch = {harken::kPlayLedBatchVersion,
        harken::PlayLedBatchKind::TimelineArm, 0, 1,
        (static_cast<std::uint64_t>(arm.generation) << 32) | arm.slot};
    command.payload.play.handle = handle;
    command.payload.play.sourceIdentity = handle;
    ASSERT_TRUE(engine.commands.TryPush(command));
    const auto block = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(block + 1u, engine.mixer.BlocksRendered());
    ASSERT_NE(nullptr, engine.voices.Resolve(handle));
    ASSERT_TRUE(engine.voices.Resolve(handle)->timelineGated);
    const auto quantum = engine.mixer.BlocksRendered();
    engine.voices.BeginTimelineQuantum(quantum);
    harken::VoiceTimelineReservation reservation{};
    ASSERT_TRUE(engine.voices.ReserveLaneTimelineEvents(static_cast<std::uint32_t>(handle),
        static_cast<std::uint32_t>(handle >> 32), quantum, 1, &reservation));
    harken::VoiceTimelineEvent event{harken::TimelineEventKind::Stop};
    event.frontierProof.state = harken::FrontierEventState::Pending;
    event.frontierProof.quantum = quantum;
    event.frontierProof.renderGeneration = static_cast<std::uint32_t>(handle >> 32);
    ASSERT_TRUE(engine.voices.StageReservedLaneTimelineEvent(reservation, 0, event));
    harken::VoiceTimelineReservationProof eventProof;
    ASSERT_TRUE(engine.voices.ProveTimelineReservation(reservation, &eventProof));
    EXPECT_EQ(harken::VoicePool::OriginalVoiceDisposal::Deferred,
        engine.voices.TryDisposeOriginalTimelineVoice(owner));
    ASSERT_NE(nullptr, engine.voices.Resolve(handle));
    EXPECT_EQ(1u, engine.registry.Lookup(harkentest::ObjectId(1)).useCount->load());
    harken::VoicePool::ReleaseTimelineReservationProof(eventProof);
    ASSERT_TRUE(engine.voices.RejectTimelineReservation(reservation));
    EXPECT_EQ(harken::VoicePool::OriginalVoiceDisposal::Deferred,
        engine.voices.TryDisposeOriginalTimelineVoice(owner));
    engine.voices.FinalizeTimelineQuantum(quantum);
    ASSERT_EQ(harken::VoicePool::OriginalVoiceDisposal::Released,
        engine.voices.TryDisposeOriginalTimelineVoice(owner));
    harken::TimelineArmObservation observation{};
    ASSERT_TRUE(engine.timelineArms.Observe(arm, &observation));
    EXPECT_EQ(harken::ArmState::Armed, observation.state);
    EXPECT_EQ(block + 1u, observation.armedCompletionSequence);
    EXPECT_EQ(0u, observation.startedBlockSequence);
    RetireOriginalArm(engine, arm);
    const auto* pcm = engine.nullBackend.LastBlock();
    ASSERT_NE(nullptr, pcm);
    for (std::uint32_t i = 0; i < kBlockFrames * engine.config.mixChannelCount; ++i)
        EXPECT_FLOAT_EQ(0.0f, pcm[i]);
}

TEST(VoicePool, OriginalOwnerActualSourceInstallationHoldDefersPhysicalDisposal) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto samples = harkentest::MakeDC(4096, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto& engine = *session.Engine();
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &handle, true));
    harken::VoicePool::PreparedCommitProof proof;
    ASSERT_TRUE(CaptureOriginalPreparation(engine, handle, &proof));
    EXPECT_EQ(nullptr, engine.voices.Resolve(handle));
    EXPECT_TRUE(engine.voices.IsAlive(handle));
    EXPECT_EQ(VN_OK, engine.voices.CheckLive(handle));
    harken::VoicePool::OriginalVoiceOwner owner;
    ASSERT_TRUE(engine.voices.IssueOriginalVoiceOwner(proof, &owner));
    harken::VoicePool::ConsumePreparedCommit(std::move(proof));
    harken::TimelineArmToken arm{};
    ASSERT_TRUE(PrepareOriginalArm(engine, handle, &arm));
    harken::TimelineArmTable::SourceCommandRowProof core;
    ASSERT_EQ(harken::TimelineArmTable::SourceCommandRowStatus::Ready,
        engine.timelineArms.ProvePendingSourceCommand(arm, &core));
    harken::VoicePool::SourceCommandInstallProof physical;
    ASSERT_EQ(harken::VoicePool::SourceCommandInstallStatus::Ready,
        engine.voices.ProveSourceCommandInstallation(core, &physical));
    EXPECT_EQ(nullptr, engine.voices.Resolve(handle));
    EXPECT_TRUE(engine.voices.IsAlive(handle));
    EXPECT_EQ(VN_OK, engine.voices.CheckLive(handle));
    EXPECT_EQ(harken::VoicePool::OriginalVoiceDisposal::Deferred,
        engine.voices.TryDisposeOriginalTimelineVoice(owner));
    harken::VoicePool::BeginSourceCommandInstallation(physical, handle, 0,
        engine.mixer.BlocksRendered());
    EXPECT_EQ(nullptr, engine.voices.Resolve(handle));
    EXPECT_TRUE(engine.voices.IsAlive(handle));
    EXPECT_EQ(VN_OK, engine.voices.CheckLive(handle));
    EXPECT_EQ(harken::VoicePool::OriginalVoiceDisposal::Deferred,
        engine.voices.TryDisposeOriginalTimelineVoice(owner));
    harken::TimelineArmTable::AcknowledgeSourceCommand(core, engine.mixer.BlocksRendered() + 1u);
    harken::VoicePool::FinishSourceCommandInstallation(physical);
    core.Release();
    EXPECT_EQ(harken::VoicePool::OriginalVoiceDisposal::Released,
        engine.voices.TryDisposeOriginalTimelineVoice(owner));
    EXPECT_FALSE(engine.voices.IsAlive(handle));
    EXPECT_EQ(VN_STALE_HANDLE, engine.voices.CheckLive(handle));
    RetireOriginalArm(engine, arm);
}

TEST(VoicePool, OriginalOwnerMaximumLastReleaseExhaustsNewIssuance) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto samples = harkentest::MakeDC(4096, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto& engine = *session.Engine();
    harken::detail::VoicePoolReleaseTestAccess::SetReleaseSequence(engine.voices, UINT64_MAX - 1u);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &handle, true));
    harken::VoiceReleaseObserver observer{};
    ASSERT_TRUE(engine.voices.CaptureReleaseObserver(handle, &observer));
    ASSERT_EQ(UINT64_MAX, observer.targetSequence);
    harken::VoicePool::PreparedCommitProof proof;
    ASSERT_TRUE(CaptureOriginalPreparation(engine, handle, &proof));
    harken::VoicePool::OriginalVoiceOwner owner;
    ASSERT_TRUE(engine.voices.IssueOriginalVoiceOwner(proof, &owner));
    harken::VoicePool::ConsumePreparedCommit(std::move(proof));
    ASSERT_EQ(harken::VoicePool::OriginalVoiceDisposal::Released,
        engine.voices.TryDisposeOriginalTimelineVoice(owner));
    EXPECT_TRUE(engine.voices.ReleaseObserved(observer));
    EXPECT_EQ(harken::VoicePool::OriginalVoiceDisposal::AlreadyReleased,
        engine.voices.TryDisposeOriginalTimelineVoice(owner));
    EXPECT_FALSE(engine.voices.HasFreeSlot());
    VnVoiceHandle refused = 0;
    EXPECT_EQ(VN_NO_VOICE, ClaimObserverVoice(engine, &refused, true));
    EXPECT_EQ(0u, refused);
    EXPECT_EQ(0u, engine.registry.Lookup(harkentest::ObjectId(1)).useCount->load());
}

TEST(VoicePool, OriginalOwnerQuarantineRetainsOriginalResourcesAndRefusesDisposal) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto samples = harkentest::MakeDC(4096, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto& engine = *session.Engine();
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &handle, true));
    harken::VoiceReleaseObserver observer{};
    ASSERT_TRUE(engine.voices.CaptureReleaseObserver(handle, &observer));
    harken::VoicePool::PreparedCommitProof proof;
    ASSERT_TRUE(CaptureOriginalPreparation(engine, handle, &proof));
    harken::VoicePool::OriginalVoiceOwner owner;
    ASSERT_TRUE(engine.voices.IssueOriginalVoiceOwner(proof, &owner));
    harken::VoicePool::ConsumePreparedCommit(std::move(proof));
    harken::detail::VoicePoolReleaseTestAccess::Quarantine(engine.voices, engine.notifications);
    EXPECT_EQ(harken::VoicePool::OriginalVoiceDisposal::Refused,
        engine.voices.TryDisposeOriginalTimelineVoice(owner));
    EXPECT_FALSE(engine.voices.ReleaseObserved(observer));
    EXPECT_FALSE(engine.voices.HasFreeSlot());
    EXPECT_EQ(1u, engine.registry.Lookup(harkentest::ObjectId(1)).useCount->load());
}

TEST(VoicePool, ReleaseObserverPreparedCancelPublishesOnce) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto samples = harkentest::MakeDC(64, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto& engine = *session.Engine();
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &voice, true));
    harken::VoiceReleaseObserver observer{};
    ASSERT_TRUE(engine.voices.CaptureReleaseObserver(voice, &observer));
    EXPECT_TRUE(engine.voices.CancelPreparedVoice(voice));
    EXPECT_FALSE(engine.voices.CancelPreparedVoice(voice));
    EXPECT_TRUE(engine.voices.ReleaseObserved(observer));
    EXPECT_EQ(0u, engine.registry.Lookup(harkentest::ObjectId(1)).useCount->load());
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &voice, true));
    harken::VoiceReleaseObserver next{};
    ASSERT_TRUE(engine.voices.CaptureReleaseObserver(voice, &next));
    EXPECT_EQ(observer.targetSequence + 1u, next.targetSequence);
    EXPECT_FALSE(engine.voices.ReleaseObserved(next));
    EXPECT_TRUE(engine.voices.CancelPreparedVoice(voice));
}

TEST(VoicePool, ReleaseObserverQuarantineDoesNotPublishExit) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto samples = harkentest::MakeDC(64, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto& engine = *session.Engine();
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &voice));
    harken::VoiceReleaseObserver observer{};
    ASSERT_TRUE(engine.voices.CaptureReleaseObserver(voice, &observer));
    harken::detail::VoicePoolReleaseTestAccess::Quarantine(
        engine.voices, engine.notifications);
    EXPECT_EQ(nullptr, engine.voices.Resolve(voice));
    EXPECT_EQ(1u, engine.registry.Lookup(harkentest::ObjectId(1)).useCount->load());
    EXPECT_FALSE(engine.voices.ReleaseObserved(observer));
    EXPECT_FALSE(engine.voices.HasFreeSlot());
    EXPECT_EQ(1u, engine.voices.ActiveCount());
}

TEST(VoicePool, ReleaseObserverMaximumPermanentlyExhaustsAdmission) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto samples = harkentest::MakeDC(64, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto& engine = *session.Engine();
    harken::detail::VoicePoolReleaseTestAccess::SetReleaseSequence(
        engine.voices, UINT64_MAX - 1u);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &voice, true));
    harken::VoiceReleaseObserver observer{};
    ASSERT_TRUE(engine.voices.CaptureReleaseObserver(voice, &observer));
    EXPECT_EQ(UINT64_MAX, observer.targetSequence);
    ASSERT_TRUE(engine.voices.CancelPreparedVoice(voice));
    EXPECT_TRUE(engine.voices.ReleaseObserved(observer));
    EXPECT_FALSE(engine.voices.CaptureReleaseObserver(voice, &observer));
    EXPECT_FALSE(engine.voices.HasFreeSlot());
    EXPECT_EQ(0u, engine.voices.AvailableSlotCount());
    EXPECT_EQ(0u, engine.voices.ActiveCount());
    std::uint32_t generation = 0;
    EXPECT_FALSE(engine.voices.FreeSlotGeneration(0, &generation));
    EXPECT_EQ(VN_NO_VOICE, engine.voices.CheckCapacityForPlay());
    EXPECT_EQ(VN_NO_VOICE, ClaimObserverVoice(engine, &voice));
    EXPECT_EQ(VN_NO_VOICE, ClaimObserverVoice(engine, &voice, false, 0));
    harken::StreamRing ring;
    const auto pan = harken::ResolvePanGains(engine.config.mixChannelLayout, 0.0f);
    for (const auto slot : {UINT32_MAX, 0u}) {
        EXPECT_EQ(VN_NO_VOICE, engine.voices.PlayStream(
            &ring, 1, kSampleRate, 0, 0, 1, 0, false, 0, 0, 0, pan,
            0, 64, 0, 64, 1, harken::ResampleProfile::Professional44148,
            &voice, 1, slot));
    }
}

TEST(VoicePool, DefaultPlayUsesLinearBrickForAllRatePairs) {
    for (const std::uint32_t mixRate : {44100u, 48000u}) {
        for (const std::uint32_t sourceRate : {44100u, 48000u}) {
            ExpectPcmResampleProfile(
                sourceRate, mixRate,
                harken::ResampleProfile::LegacyCompatibility);
            ExpectStreamResampleProfile(
                sourceRate, mixRate,
                harken::ResampleProfile::LegacyCompatibility);
        }
    }

    ExpectPcmResampleProfile(
        96000u, 48000u,
        harken::ResampleProfile::LegacyCompatibility);
    ExpectStreamResampleProfile(
        32000u, 44100u,
        harken::ResampleProfile::LegacyCompatibility);
}

TEST(VoicePool, DeferredWeightPreparationFailsBeforePublicationAndKeepsFullOwnerCapacity) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto samples = harkentest::MakeDC(8192, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto& engine = *session.Engine();
    const auto pcm = engine.registry.Lookup(harkentest::ObjectId(1));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, engine.voices.Play(pcm, 0, 1, 0, false, 0, 0, 0,
        harken::ResolvePanGains(engine.config.mixChannelLayout, 0), 0, pcm.frames, 1,
        harken::ResampleProfile::LegacyCompatibility, &voice, 1, UINT32_MAX, 0, true, 0, 1, false));
    auto* context = engine.voices.Resolve(voice);
    ASSERT_NE(nullptr, context);
    EXPECT_TRUE(context->variableWeightControls.empty());
    EXPECT_EQ(0u, engine.voices.VariableWeightStorageBytes());
    const harken::VariableWeightControlBinding first{{101, 7, 0},
        {0, 2, 0.5f, 0.5f, 1, {1, 1}, {1, 1}}};
    engine.voices.SetVariableWeightCommitPageLimitForTests(0);
    EXPECT_FALSE(engine.voices.PrepareVariableWeightControls(voice, &first, 1));
    EXPECT_TRUE(context->variableWeightControls.empty());
    EXPECT_EQ(0u, context->variableWeightControlCount);
    engine.voices.SetVariableWeightCommitPageLimitForTests(SIZE_MAX);
    ASSERT_TRUE(engine.voices.PrepareVariableWeightControls(voice, &first, 1));
    ASSERT_EQ(harken::kSelectionPathNodeCapacity, context->variableWeightControls.size());
    auto* const backing = context->variableWeightControls.data();
    for (std::uint32_t i = 1; i < harken::kSelectionPathNodeCapacity; ++i)
        ASSERT_TRUE(engine.voices.ApplyOwnedVariableWeightSegment(voice, {101u + i, 7, 0}, first.segment));
    ASSERT_TRUE(engine.voices.RemoveOwnedVariableWeightSegment(voice, first.owner, 1));
    EXPECT_FALSE(engine.voices.ApplyOwnedVariableWeightSegment(voice, {1000, 7, 0}, first.segment));
    ASSERT_TRUE(engine.voices.CancelPreparedVoice(voice));
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &voice, true));
    context = engine.voices.Resolve(voice);
    ASSERT_NE(nullptr, context);
    EXPECT_EQ(backing, context->variableWeightControls.data());
    EXPECT_EQ(0u, context->variableWeightControlCount);
    for (const auto& entry : context->variableWeightControls) {
        EXPECT_FALSE(entry.active);
        EXPECT_FALSE(entry.owner.Valid());
    }
    ASSERT_TRUE(engine.voices.CancelPreparedVoice(voice));
}

TEST(VoicePool, TimelineCurvePreparationRefusalKeepsVoiceAdmissionAtomic) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto samples = harkentest::MakeDC(8192, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto& engine = *session.Engine();
    engine.voices.SetTimelineEnvelopeCurveCommitPageLimitForTests(0);
    VnVoiceHandle voice = 0;
    EXPECT_EQ(VN_NO_VOICE, ClaimObserverVoice(engine, &voice, true));
    EXPECT_EQ(0u, voice);
    EXPECT_EQ(0u, engine.voices.ActiveCount());
    EXPECT_TRUE(engine.voices.HasFreeSlot());
    engine.voices.SetTimelineEnvelopeCurveCommitPageLimitForTests(SIZE_MAX);
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &voice, true));
    auto* context = engine.voices.Resolve(voice);
    ASSERT_NE(nullptr, context);
    ASSERT_NE(nullptr, context->timelineEnvelopes[0].curve);
    ASSERT_NE(nullptr, context->timelineEnvelopes[1].curve);
    ASSERT_TRUE(engine.voices.CancelPreparedVoice(voice));
}

TEST(VoicePool, GenericFutureBackingFillsSparseWeightHolesBeforePreparedUse) {
    EngineSession session(VoicePoolConfig(4, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto samples = harkentest::MakeDC(8192, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    const auto wideSamples = harkentest::MakeDC(8192 * harken::kMaxChannels, 0.01f);
    const auto wideDesc = harkentest::InterleavedDesc(wideSamples, harken::kMaxChannels, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &wideDesc));
    auto& engine = *session.Engine();
    VnVoiceHandle high = 0;
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &high, true, 3));
    const harken::VariableWeightControlBinding first{{101, 7, 0},
        {0, 2, 0.5f, 0.5f, 1, {1, 1}, {1, 1}}};
    ASSERT_TRUE(engine.voices.PrepareVariableWeightControls(high, &first, 1));
    const auto* highContext = engine.voices.Resolve(high);
    ASSERT_NE(nullptr, highContext);
    auto* const backing = highContext->variableWeightControls.data();
    auto* const retainedCurve = highContext->timelineEnvelopes[0].curve;
    ASSERT_NE(nullptr, retainedCurve);
    retainedCurve->values[0] = 0.375f;
    harken::StableStoragePreparation held;
    for (std::uint32_t slot = 0; slot < 4; ++slot)
        EXPECT_TRUE(engine.voices.PendingTimelineVariableEvents(slot).empty());
    engine.voices.SetHighRatioCommitPageLimitForTests(0);
    EXPECT_FALSE(engine.voices.PrepareSlotBacking(1, &held));
    EXPECT_FALSE(held.Valid());
    EXPECT_EQ(highContext, engine.voices.Resolve(high));
    EXPECT_EQ(backing, highContext->variableWeightControls.data());
    EXPECT_EQ(first.owner, highContext->variableWeightControls[0].owner);
    engine.voices.SetHighRatioCommitPageLimitForTests(SIZE_MAX);
    engine.voices.SetPendingTimelineVariableEventCommitPageLimitForTests(0);
    EXPECT_FALSE(engine.voices.PrepareSlotBacking(1, &held));
    EXPECT_FALSE(held.Valid());
    EXPECT_EQ(highContext, engine.voices.Resolve(high));
    EXPECT_EQ(backing, highContext->variableWeightControls.data());
    EXPECT_EQ(first.owner, highContext->variableWeightControls[0].owner);
    engine.voices.SetPendingTimelineVariableEventCommitPageLimitForTests(SIZE_MAX);
    engine.voices.SetVariableWeightCommitPageLimitForTests(0);
    EXPECT_FALSE(engine.voices.PrepareSlotBacking(1, &held));
    EXPECT_FALSE(held.Valid());
    engine.voices.SetVariableWeightCommitPageLimitForTests(SIZE_MAX);
    engine.mixer.ArenaForTests().SetCommitPageLimitForTests(0);
    EXPECT_FALSE(engine.voices.PrepareSlotBacking(1, &held));
    EXPECT_FALSE(held.Valid());
    engine.mixer.ArenaForTests().SetCommitPageLimitForTests(SIZE_MAX);
    ASSERT_TRUE(engine.voices.PrepareSlotBacking(1, &held));
    for (std::uint32_t slot = 0; slot < 4; ++slot)
        ASSERT_EQ(harken::kTimelineMaxEventsPerVoiceQuantum,
            engine.voices.PendingTimelineVariableEvents(slot).size());
    auto* const pendingEvents = engine.voices.PendingTimelineVariableEvents(3).data();
    pendingEvents[0].value = 173;
    engine.voices.SetPendingTimelineVariableEventCommitPageLimitForTests(0);
    EXPECT_EQ(backing, highContext->variableWeightControls.data());
    EXPECT_EQ(1u, highContext->variableWeightControlCount);
    EXPECT_EQ(first.owner, highContext->variableWeightControls[0].owner);
    engine.voices.SetVariableWeightCommitPageLimitForTests(0);
    engine.mixer.ArenaForTests().SetCommitPageLimitForTests(0);
    engine.voices.SetHighRatioCommitPageLimitForTests(0);
    {
        auto use = held.Use();
        VnVoiceHandle low = 0;
        const auto pcm = engine.registry.Lookup(harkentest::ObjectId(2));
        ASSERT_EQ(VN_OK, engine.voices.Play(pcm, 0, 1, 0, false, 0, 0, 0,
            harken::ResolvePanGains(engine.config.mixChannelLayout, 0), 0, pcm.frames, 1,
            harken::ResampleProfile::LegacyCompatibility, &low, 1, 0, 0, true));
        ASSERT_EQ(harken::kSelectionPathNodeCapacity,
            engine.voices.Resolve(low)->variableWeightControls.size());
        EXPECT_EQ(harken::kMaxChannels, engine.voices.Resolve(low)->pcm.channelCount);
        ASSERT_NE(nullptr, engine.voices.Resolve(low)->timelineEnvelopes[0].curve);
        ASSERT_NE(nullptr, engine.voices.Resolve(low)->timelineEnvelopes[1].curve);
        EXPECT_EQ(retainedCurve, highContext->timelineEnvelopes[0].curve);
        EXPECT_FLOAT_EQ(0.375f, retainedCurve->values[0]);
        ASSERT_TRUE(engine.voices.PrepareVariableWeightControls(low, &first, 1));
        ASSERT_TRUE(engine.voices.CancelPreparedVoice(low));
        EXPECT_EQ(pendingEvents, engine.voices.PendingTimelineVariableEvents(3).data());
        EXPECT_EQ(173u, pendingEvents[0].value);
    }
    ASSERT_TRUE(engine.voices.CancelPreparedVoice(high));
}

TEST(VoicePool, WiderLowSlotPreservesLiveHighSlotsWithBlendAndSends) {
    constexpr std::uint32_t voiceCount = 16;
    std::array<harkentest::Capture, 2> captures;
    for (std::uint32_t pass = 0; pass < captures.size(); ++pass) {
        SCOPED_TRACE(pass);
        auto config = VoicePoolConfig(voiceCount, kBlockFrames, kSampleRate);
        config.fxSlotsPerVoice = 1;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        const auto mono = harkentest::MakeDC(8192, 0.001f);
        const auto monoDesc = MonoDesc(mono, kSampleRate);
        const auto wide = harkentest::MakeDC(8192 * harken::kMaxChannels, 0.001f);
        const auto wideDesc = harkentest::InterleavedDesc(wide, harken::kMaxChannels, kSampleRate);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &monoDesc));
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &wideDesc));
        if (pass == 0) ASSERT_TRUE(engine.mixer.PrepareVoiceBacking(voiceCount));
        const auto activate = [&](VnVoiceHandle voice) {
            auto* context = engine.voices.Resolve(voice);
            ASSERT_NE(nullptr, context);
            context->fxSlots[0].process = IdentityTailProcess;
            context->fxBypassMix[0] = 0.5f;
            for (std::uint32_t send = 0; send < harken::kMaxVoiceSendSlots; ++send)
                engine.voices.ApplySetVoiceSend(voice, send, 0, 0.0f, 1);
            engine.voices.ApplyPlay(voice, voice, 0, engine.mixer.BlocksRendered());
        };
        std::array<VnVoiceHandle, voiceCount> voices{};
        for (std::uint32_t slot = 0; slot < voiceCount; ++slot) {
            ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &voices[slot], false, slot));
            activate(voices[slot]);
        }
        auto& captured = captures[pass];
        harkentest::PumpAndCapture(&engine, 2, captured);
        engine.voices.ApplyStop(voices[0], 0, 0, engine.notifications, engine.mixer.BlocksRendered());
        harkentest::PumpAndCapture(&engine, 2, captured);
        ASSERT_EQ(nullptr, engine.voices.Resolve(voices[0]));
        const auto pcm = engine.registry.Lookup(harkentest::ObjectId(2));
        const auto claimWide = [&](VnVoiceHandle* out) {
            return engine.voices.Play(pcm, 0, 1, 0, false, 0, 0, 0,
                harken::ResolvePanGains(engine.config.mixChannelLayout, 0), 0, pcm.frames, 1,
                harken::ResampleProfile::LegacyCompatibility, out, 1, 0);
        };
        if (pass != 0) {
            engine.mixer.ArenaForTests().SetCommitPageLimitForTests(0);
            VnVoiceHandle refused = 0;
            EXPECT_EQ(VN_NO_VOICE, claimWide(&refused));
            EXPECT_EQ(0u, refused);
            EXPECT_EQ(1u, engine.voices.AvailableSlotCount());
        }
        harkentest::PumpAndCapture(&engine, 1, captured);
        engine.mixer.ArenaForTests().SetCommitPageLimitForTests(SIZE_MAX);
        ASSERT_EQ(VN_OK, claimWide(&voices[0]));
        activate(voices[0]);
        engine.mixer.ArenaForTests().SetCommitPageLimitForTests(0);
        harkentest::PumpAndCapture(&engine, 3, captured);
        EXPECT_EQ(0u, engine.mixer.Arena().OverflowCount());
        EXPECT_EQ(0u, engine.voices.AvailableSlotCount());
        for (const auto voice : voices) ASSERT_NE(nullptr, engine.voices.Resolve(voice));
        EXPECT_GT(captured.left.back(), 0.01f);
        EXPECT_GT(captured.right.back(), 0.01f);
    }
    EXPECT_EQ(captures[0].left, captures[1].left);
    EXPECT_EQ(captures[0].right, captures[1].right);
}

TEST(VoicePool, ReusedWidthKeepsDirectAudioAtExactArenaBoundaries) {
    std::vector<float> referenceMono, referenceWide;
    for (const auto workers : {0u, 2u}) for (const bool reuse : {false, true}) {
        SCOPED_TRACE(workers);
        SCOPED_TRACE(reuse);
        auto config = VoicePoolConfig(2, kBlockFrames, kSampleRate);
        config.fxSlotsPerVoice = 1;
        config.voiceSendSlots = 1;
        config.renderWorkerCount = workers;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        const auto sentinelPcm = harkentest::MakeDC(8192, 0.03125f);
        const auto monoPcm = harkentest::MakeDC(8192, 0.0625f);
        std::vector<float> widePcm(8192 * harken::kMaxChannels);
        for (std::size_t i = 0; i < widePcm.size(); ++i)
            widePcm[i] = 0.002f * static_cast<float>(1 + i % harken::kMaxChannels);
        const auto sentinelDesc = MonoDesc(sentinelPcm, kSampleRate);
        const auto monoDesc = MonoDesc(monoPcm, kSampleRate);
        const auto wideDesc = harkentest::InterleavedDesc(widePcm, harken::kMaxChannels, kSampleRate);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &sentinelDesc));
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &monoDesc));
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(3), &wideDesc));
        const auto sendBus = harkentest::ObjectId(7);
        ASSERT_EQ(VN_OK, Vn_CreateBus(sendBus, VN_BUS_MASTER));
        ASSERT_EQ(VN_OK, Vn_Update());
        const auto pump = [&] {
            const auto before = engine.mixer.BlocksRendered();
            engine.nullBackend.Pump(1);
            ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
        };
        const auto capture = [&] {
            return std::vector<float>(engine.nullBackend.LastBlock(),
                engine.nullBackend.LastBlock() + 2 * kBlockFrames);
        };
        const auto play = [&](unsigned id, unsigned slot, VnVoiceHandle* out) {
            const auto pcm = engine.registry.Lookup(harkentest::ObjectId(id));
            ASSERT_EQ(VN_OK, engine.voices.Play(pcm, 0, 1, 0, false, 0, 0, 0,
                harken::ResolvePanGains(engine.config.mixChannelLayout, 0), 0, pcm.frames, 1,
                harken::ResampleProfile::LegacyCompatibility, out, 1, slot));
            engine.voices.ApplyPlay(*out, *out, 0, engine.mixer.BlocksRendered());
        };
        const auto stop = [&](VnVoiceHandle handle) {
            engine.voices.ApplyStop(handle, 0, 0, engine.notifications, engine.mixer.BlocksRendered());
            ASSERT_NO_FATAL_FAILURE(pump());
            ASSERT_NO_FATAL_FAILURE(pump());
            ASSERT_EQ(nullptr, engine.voices.Resolve(handle));
        };
        VnVoiceHandle sentinel = 0, probe = 0, oldWide = 0;
        ASSERT_NO_FATAL_FAILURE(play(1, 0, &sentinel));
        ASSERT_NO_FATAL_FAILURE(pump());
        const auto sentinelOnly = capture();
        ASSERT_GT(harkentest::Peak(sentinelOnly.data(), sentinelOnly.size()), 0.01f);
        for (const auto sample : sentinelOnly) ASSERT_NEAR(0.03125f, sample, 0.000001f);
        if (reuse) {
            ASSERT_NO_FATAL_FAILURE(play(3, 1, &oldWide));
            ASSERT_NO_FATAL_FAILURE(pump());
            ASSERT_NO_FATAL_FAILURE(pump());
            ASSERT_NO_FATAL_FAILURE(stop(oldWide));
        }
        ASSERT_NO_FATAL_FAILURE(play(2, 1, &probe));
        if (reuse) EXPECT_NE(oldWide, probe);
        ASSERT_NO_FATAL_FAILURE(pump());
        ASSERT_NO_FATAL_FAILURE(pump());
        const auto direct = capture();
        // Centered mono contributes unity to both stereo channels.
        for (const auto sample : direct) ASSERT_NEAR(0.09375f, sample, 0.000001f);
        if (referenceMono.empty()) referenceMono = direct;
        EXPECT_EQ(referenceMono, direct);
        auto* context = engine.voices.Resolve(probe);
        ASSERT_NE(nullptr, context);
        context->fxSlots[0].process = IdentityTailProcess;
        engine.voices.ApplySetVoiceSend(probe, 0, engine.buses.FindIndex(sendBus), 0.0f, 1);
        ASSERT_EQ(1u, context->voiceSendActiveCount);
        ASSERT_EQ(2u, engine.buses.AudioBusCount());
        ASSERT_EQ(0u, engine.mixer.Arena().QuarantinedHighBytes());
        constexpr auto plane = kBlockFrames * sizeof(float);
        const std::array<unsigned, 3> capacities{7 * plane - sizeof(float),
            8 * plane - sizeof(float), 8 * plane};
        for (unsigned boundary = 0; boundary < capacities.size(); ++boundary) {
            SCOPED_TRACE(boundary);
            context->fxBypassMix[0] = 0.5f;
            const auto frame = context->resampler.SourceFrame();
            const auto fraction = context->resampler.SourceFraction();
            const auto sentinelFrame = engine.voices.Resolve(sentinel)->resampler.SourceFrame();
            const auto overflows = engine.mixer.Arena().OverflowCount();
            engine.mixer.ArenaForTests().SetCapacityForTests(capacities[boundary]);
            const auto block = engine.mixer.BlocksRendered();
            engine.nullBackend.Pump(1);
            engine.mixer.ArenaForTests().RestoreCapacityForTests();
            ASSERT_EQ(block + 1u, engine.mixer.BlocksRendered());
            EXPECT_EQ(overflows + (boundary < 2 ? 1u : 0u), engine.mixer.Arena().OverflowCount());
            EXPECT_EQ(frame + (boundary == 0 ? 0 : kBlockFrames), context->resampler.SourceFrame());
            EXPECT_EQ(fraction, context->resampler.SourceFraction());
            EXPECT_EQ(sentinelFrame + kBlockFrames, engine.voices.Resolve(sentinel)->resampler.SourceFrame());
            const auto output = capture();
            for (std::size_t i = 0; i < output.size(); ++i) {
                const auto expected = boundary == 0 ? sentinelOnly[i]
                    : boundary == 1 ? direct[i] : 2.0f * direct[i] - sentinelOnly[i];
                ASSERT_NEAR(expected, output[i], 0.000001f) << i;
            }
            // Recover from the exact consumed position, with the direct and send paths audible.
            for (unsigned recovery = 0; recovery < 2; ++recovery) {
                const auto beforeFrame = context->resampler.SourceFrame();
                ASSERT_NO_FATAL_FAILURE(pump());
                EXPECT_EQ(beforeFrame + kBlockFrames, context->resampler.SourceFrame());
                EXPECT_EQ(overflows + (boundary < 2 ? 1u : 0u), engine.mixer.Arena().OverflowCount());
                const auto recovered = capture();
                for (std::size_t i = 0; i < recovered.size(); ++i)
                    ASSERT_NEAR(2.0f * direct[i] - sentinelOnly[i], recovered[i], 0.000001f) << i;
            }
        }
        ASSERT_NO_FATAL_FAILURE(stop(probe));
        VnVoiceHandle returnedWide = 0;
        ASSERT_NO_FATAL_FAILURE(play(3, 1, &returnedWide));
        EXPECT_NE(probe, returnedWide);
        ASSERT_NO_FATAL_FAILURE(pump());
        ASSERT_NO_FATAL_FAILURE(pump());
        const auto wide = capture();
        // A multichannel bed maps its front channels by role into this stereo mix.
        for (std::size_t i = 0; i < wide.size(); ++i)
            ASSERT_NEAR(0.03125f + 0.002f * static_cast<float>(1 + i % 2), wide[i], 0.000001f);
        if (referenceWide.empty()) referenceWide = wide;
        EXPECT_EQ(referenceWide, wide);
        VnSnapshot snapshot{};
        snapshot.structSize = sizeof(snapshot);
        ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
        EXPECT_EQ(engine.mixer.BlocksRendered() - 1u, snapshot.blockIndex);
        EXPECT_EQ(workers, snapshot.renderWorkerCount);
        EXPECT_EQ(0u, engine.mixer.CommandProtocolViolationCount());
        ASSERT_NO_FATAL_FAILURE(stop(returnedWide));
        ASSERT_NO_FATAL_FAILURE(stop(sentinel));
        EXPECT_EQ(0.0f, harkentest::Peak(engine.nullBackend.LastBlock(), 2 * kBlockFrames));
    }
}

TEST(VoicePool, ProfessionalWorkingSetSurvivesPreflightSeekAndSlotReuse) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto samples = harkentest::MakeDC(8192, 0.25f);
    const auto desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto& engine = *session.Engine();
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &voice));
    engine.voices.ApplyPlay(voice, voice, 0, 0);
    const auto* context = engine.voices.Resolve(voice);
    ASSERT_NE(nullptr, context);
    auto* workspace = context->resampler.professional;
    ASSERT_NE(nullptr, workspace);
    EXPECT_EQ(sizeof(harken::MultirateResamplerCandidate), engine.voices.ProfessionalStorageBytes());
    workspace->UpdateRatio(1.5, 64);
    ASSERT_TRUE(engine.voices.PrepareSlotWorkingSet(0, 1,
        harken::ResampleProfile::Professional44148));
    EXPECT_EQ(64u, workspace->rampRemaining);
    engine.voices.CommitSeek(voice, 512);
    EXPECT_EQ(workspace, context->resampler.professional);
    EXPECT_EQ(64u, context->resampler.RampFramesRemaining());
    EXPECT_DOUBLE_EQ(1.5, context->resampler.TargetRatio());
    EXPECT_EQ(512u, context->srcCursor);
    engine.voices.ApplyStop(voice, 0, 0, engine.notifications, 0);
    const auto pcm = engine.registry.Lookup(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, engine.voices.Play(pcm, 0, 1, 0, false, 0, 0, 0,
        harken::ResolvePanGains(engine.config.mixChannelLayout, 0),
        0, pcm.frames, 1, harken::ResampleProfile::LegacyCompatibility, &voice));
    ASSERT_NE(nullptr, engine.voices.Resolve(voice));
    EXPECT_EQ(nullptr, engine.voices.Resolve(voice)->resampler.professional);
    EXPECT_EQ(sizeof(harken::MultirateResamplerCandidate), engine.voices.ProfessionalStorageBytes());
    engine.voices.ApplyStop(voice, 0, 0, engine.notifications, 0);
}

TEST(VoicePool, ProfessionalStreamSeekAndParkResumeReuseWorkingSet) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    constexpr auto profile = harken::ResampleProfile::Professional44148;
    std::array<float, 4096> storage;
    storage.fill(0.25f);
    float* planes[harken::kMaxChannels] = {storage.data()};
    std::array<harken::StreamRing, 3> rings;
    for (auto& ring : rings) {
        ring.Wire(planes, static_cast<std::uint32_t>(storage.size()));
        ring.ResetForUse(1);
        ring.PublishWrite(static_cast<std::uint32_t>(storage.size()));
        ring.state.store(harken::StreamSlotState::Active);
    }
    const auto pan = harken::ResolvePanGains(engine.config.mixChannelLayout, 0);
    const auto tokenFor = [](const harken::StreamRing& ring, std::uint32_t slot) {
        return (static_cast<std::uint64_t>(ring.generation.load()) << 32u) | slot;
    };
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, engine.voices.PlayStream(&rings[0], 1, kSampleRate,
        kBlockFrames, 0, 1, 0, false, 0, 0, 0, pan,
        0, 65536, 0, 65536, 1, profile, &voice));
    ASSERT_TRUE(engine.voices.BindStreamRingToken(voice, &rings[0], tokenFor(rings[0], 1)));
    engine.voices.ApplyPlay(voice, voice, 0, 0);
    engine.nullBackend.Pump(1);
    auto* context = engine.voices.Resolve(voice);
    ASSERT_NE(nullptr, context);
    auto* workspace = context->resampler.professional;
    ASSERT_NE(nullptr, workspace);
    context->resampler.UpdateRatio(1.5, 64);
    engine.voices.SetProfessionalCommitPageLimitForTests(0);
    const auto committed = engine.voices.RenderWorksetCommittedBytes();

    harken::StreamSeekHandoffSnapshot seek;
    seek.expectedOldRingToken = tokenFor(rings[0], 1);
    seek.replacementRing = &rings[1];
    seek.replacementRingToken = tokenFor(rings[1], 2);
    seek.targetFrame = 512;
    seek.frameCount = 65536;
    seek.sourceSampleRate = kSampleRate;
    seek.prebufferTargetFrames = kBlockFrames;
    std::uint64_t target = 0;
    ASSERT_EQ(VN_OK, engine.voices.CanApplySeek(voice, VN_ACTION_SEEK_ABSOLUTE,
        512, &target, &seek));
    engine.voices.CommitSeek(voice, target, &seek);
    EXPECT_EQ(workspace, context->resampler.professional);
    EXPECT_EQ(64u, context->resampler.RampFramesRemaining());
    EXPECT_DOUBLE_EQ(1.5, context->resampler.TargetRatio());
    EXPECT_EQ(&rings[1], context->streamRing);
    EXPECT_EQ(harken::StreamSlotState::Retired, rings[0].state.load());
    engine.nullBackend.Pump(1);

    ASSERT_TRUE(engine.voices.ReserveStreamPark(voice, voice, 91, profile));
    engine.voices.SetStreamParkCommitPageLimitForTests(0);
    const auto parkedVoice = voice;
    engine.voices.ApplyStop(voice, 0, kBlockFrames, engine.notifications, 2,
        harken::kStopModeStreamPark, voice, 91);
    engine.nullBackend.Pump(1);
    harken::StreamParkOutcome outcome;
    bool cancelled = false;
    ASSERT_TRUE(engine.voices.ConsumeStreamParkOutcomeExact(
        parkedVoice, parkedVoice, 91, &outcome, &cancelled));
    ASSERT_FALSE(cancelled);
    ASSERT_EQ(harken::StreamParkDisposition::ResumeReady, outcome.disposition);
    const auto* saved = engine.voices.ResolveParkOwnedToken(outcome.token);
    ASSERT_NE(nullptr, saved);
    EXPECT_EQ(profile, saved->profile);
    ASSERT_EQ(VN_OK, engine.voices.PlayStream(&rings[2], 1, kSampleRate,
        kBlockFrames, 0, 1.5, 0, false, 0, 0, 0, pan,
        0, 65536, 0, 65536, 1, profile, &voice, 1, UINT32_MAX, 0, &outcome.token));
    context = engine.voices.Resolve(voice);
    ASSERT_NE(nullptr, context);
    EXPECT_EQ(workspace, context->resampler.professional);
    EXPECT_EQ(saved->resampler.sourceFrame, context->resampler.SourceFrame());
    EXPECT_DOUBLE_EQ(saved->resampler.sourceFraction, context->resampler.SourceFraction());
    EXPECT_EQ(committed, engine.voices.RenderWorksetCommittedBytes());
    engine.voices.ReleaseParkOwnedToken(outcome.token);
    engine.voices.ApplyPlay(voice, voice, 0, 3);
    engine.nullBackend.Pump(1);
    const auto* output = engine.nullBackend.LastBlock();
    EXPECT_TRUE(std::any_of(output, output + kBlockFrames * 2,
        [](float sample) { return sample > 0.01f; }));
    engine.voices.ApplyStop(voice, 0, 0, engine.notifications, 4);
}

TEST(VoicePool, ProfileIsImmutableAcrossPitchSetterAndSlotReuse) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* const engine = session.Engine();

    const std::vector<float> samples = harkentest::MakeDC(8192, 0.25f);
    const VnPCMDesc professional = MonoDesc(samples, 44100);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &professional));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.loopCount = 0;
    VnVoiceHandle first = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &first));
    const VnVoiceHandle firstRender = harkentest::RenderHandle(engine, first);
    ASSERT_NE(nullptr, engine->voices.Resolve(firstRender));
    ASSERT_EQ(harken::ResampleProfile::LegacyCompatibility,
              engine->voices.Resolve(firstRender)->resampler.Profile());
    ASSERT_EQ(VN_OK, Vn_SetVoicePitch(first, 16.0f));
    session.Engine()->nullBackend.Pump(1);
    ASSERT_EQ(harken::ResampleProfile::LegacyCompatibility,
              engine->voices.Resolve(firstRender)->resampler.Profile());
    ASSERT_EQ(VN_OK, Vn_Stop(first, 0));
    session.Engine()->nullBackend.Pump(1);

    const VnPCMDesc legacy = MonoDesc(samples, 96000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &legacy));
    play = MakePlayDesc(harkentest::ObjectId(2));
    play.loopCount = 0;
    VnVoiceHandle second = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &second));
    const VnVoiceHandle secondRender = harkentest::RenderHandle(engine, second);
    ASSERT_EQ(firstRender & 0xffffffffu, secondRender & 0xffffffffu);
    ASSERT_NE(nullptr, engine->voices.Resolve(secondRender));
    EXPECT_EQ(harken::ResampleProfile::LegacyCompatibility,
              engine->voices.Resolve(secondRender)->resampler.Profile());
}

TEST(VoicePool, RefusedPlayDoesNotPublishAResampleProfile) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* const engine = session.Engine();

    const std::vector<float> samples = harkentest::MakeDC(4096, 0.25f);
    const VnPCMDesc desc = MonoDesc(samples, 44100);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = harkentest::ObjectId(999);
    VnVoiceHandle handle = 0;
    EXPECT_EQ(VN_UNKNOWN_ID, harkentest::PlayRaw(&play, &handle));
    EXPECT_EQ(0u, engine->voices.ActiveCount());
    EXPECT_TRUE(std::all_of(
        engine->voicePlayFacts.begin(), engine->voicePlayFacts.end(),
        [](const harken::VoicePlayFacts& facts) { return !facts.valid; }));
    EXPECT_TRUE(std::none_of(
        engine->pendingStarts.begin(), engine->pendingStarts.end(),
        [](const harken::PendingStartRecord& rec) { return rec.active; }));
}

TEST(VoicePool, PoolExhaustionRefusesWithNoVoice) {
    EngineSession session(VoicePoolConfig(4, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> sine =
        harkentest::MakeSine(48000, 440.0, 48000.0, 0.5f);
    const VnPCMDesc desc = MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handles[4] = {};
    for (int i = 0; i < 4; ++i) {
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handles[i])) << "voice " << i;
    }

    VnVoiceHandle overflow = 0;
    EXPECT_EQ(VN_NO_VOICE, harkentest::PlayRaw(&play, &overflow));
    EXPECT_EQ(4u, session.Engine()->voices.ActiveCount());
    // The refusal is countable, like the stale-handle counter.
    EXPECT_EQ(1u, session.Engine()->voices.NoVoiceCount());

    // Freeing one slot makes Play succeed again; a successful Play leaves
    // the refusal counter untouched. The queued stop releases the slot when
    // the audio side applies it, so a pump sits between stop and replay.
    ASSERT_EQ(VN_OK, Vn_Stop(handles[0], 0));
    session.Engine()->nullBackend.Pump(1);
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &overflow));
    EXPECT_EQ(1u, session.Engine()->voices.NoVoiceCount());
}

TEST(VoicePool, StaleHandleAfterSlotReuseIsBenignAndCounted) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> shortSound = harkentest::MakeDC(64, 0.5f);
    const VnPCMDesc desc = MonoDesc(shortSound, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle first = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &first));

    // The 64-frame sound finishes inside one block; the render slot frees.
    // The ledger entry for `first` still lingers (its end notification has not been
    // pumped through Vn_Update), so the render slot is reused while the ledger
    // slot is not — the two identities are now decoupled. Resolve() takes the
    // render-slot handle, so translate through the ledger first.
    session.Engine()->nullBackend.Pump(1);
    ASSERT_EQ(nullptr,
              session.Engine()->voices.Resolve(
                  harkentest::RenderHandle(session.Engine(), first)));

    // Reuse the single render slot: same render-slot index, bumped render
    // generation. (Formerly asserted on the public handle's low bits; the
    // public handle is now a ledger handle, so the reuse claim is checked on
    // the internal render-slot handle instead.)
    VnVoiceHandle second = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &second));
    ASSERT_NE(first, second);
    const VnVoiceHandle firstRender =
        harkentest::RenderHandle(session.Engine(), first);
    const VnVoiceHandle secondRender =
        harkentest::RenderHandle(session.Engine(), second);
    EXPECT_EQ(firstRender & 0xffffffffu, secondRender & 0xffffffffu);

    // The stale handle is a benign no-op with a counter; the live voice on
    // the same slot is untouched.
    const std::uint64_t before =
        session.Engine()->voices.StaleHandleCount();
    EXPECT_EQ(VN_STALE_HANDLE, Vn_Stop(first, 0));
    EXPECT_EQ(before + 1, session.Engine()->voices.StaleHandleCount());
    EXPECT_NE(nullptr,
              session.Engine()->voices.Resolve(secondRender));

    EXPECT_EQ(VN_OK, Vn_Stop(second, 0));
}

TEST(VoicePool, ZeroAndGarbageHandlesAreStale) {
    EngineSession session(VoicePoolConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    EXPECT_EQ(VN_STALE_HANDLE, Vn_Stop(0, 0));
    // Slot index far beyond the pool.
    EXPECT_EQ(VN_STALE_HANDLE,
              Vn_Stop((1ull << 32) | 0xFFFFFFF0u, 0));
    EXPECT_EQ(2u, session.Engine()->voices.StaleHandleCount());
}

TEST(VoicePool, OversizedFadeStopSaturatesWithoutEndingVoice) {
    constexpr std::uint32_t kSampleRate = 48000;
    constexpr std::uint32_t kFadeMs = 89478486u;
    constexpr std::uint64_t kExactFrames =
        (static_cast<std::uint64_t>(kFadeMs) * kSampleRate + 999u) / 1000u;
    // The exact ceiling is 2^32 + 32 frames, so narrowing must saturate
    // instead of wrapping the fade to 32 frames.
    static_assert(kExactFrames == (std::uint64_t{1} << 32) + 32u);

    EngineSession session(
        VoicePoolConfig(1, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> dc = harkentest::MakeDC(64, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.loopCount = 0; // infinite
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_Stop(handle, kFadeMs));

    session.Engine()->nullBackend.Pump(1);
    auto* const context = session.Engine()->voices.Resolve(
        harkentest::RenderHandle(session.Engine(), handle));
    ASSERT_NE(nullptr, context);
    EXPECT_TRUE(context->fading);
    EXPECT_EQ(std::numeric_limits<std::uint32_t>::max(),
              context->fadeTotalFrames);
    EXPECT_EQ(kBlockFrames, context->fadeDoneFrames);
}

TEST(VoicePool, PlayValidatesArgumentsAndIdentity) {
    EngineSession session(VoicePoolConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> sound = harkentest::MakeDC(64, 0.5f);
    const VnPCMDesc desc = MonoDesc(sound, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    VnVoiceHandle handle = 0;
    EXPECT_EQ(VN_INVALID_ARG, harkentest::PlayRaw(nullptr, &handle));

    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    EXPECT_EQ(VN_INVALID_ARG, harkentest::PlayRaw(&play, nullptr));

    play.structSize = 0;
    EXPECT_EQ(VN_INVALID_ARG, harkentest::PlayRaw(&play, &handle));

    play = MakePlayDesc(harkentest::ObjectId(1));
    play.pitchRatio = -1.0f;
    EXPECT_EQ(VN_INVALID_ARG, harkentest::PlayRaw(&play, &handle));

    // NaN compares false against any range bound and must be caught by the
    // explicit finiteness check, for both float fields.
    play = MakePlayDesc(harkentest::ObjectId(1));
    play.pitchRatio = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(VN_INVALID_ARG, harkentest::PlayRaw(&play, &handle));

    play = MakePlayDesc(harkentest::ObjectId(1));
    play.pitchRatio = std::numeric_limits<float>::infinity();
    EXPECT_EQ(VN_INVALID_ARG, harkentest::PlayRaw(&play, &handle));

    play = MakePlayDesc(harkentest::ObjectId(1));
    play.volumeDb = std::numeric_limits<float>::quiet_NaN();
    EXPECT_EQ(VN_INVALID_ARG, harkentest::PlayRaw(&play, &handle));

    play = MakePlayDesc(harkentest::ObjectId(1));
    play.pitchRatio = 0.001f;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    const harken::VoiceRenderContext* context =
        session.Engine()->voices.Resolve(
            harkentest::RenderHandle(session.Engine(), handle));
    ASSERT_NE(nullptr, context);
    EXPECT_DOUBLE_EQ(1.0 / 64.0,
                     context->resampler.CurrentRatio());
    EXPECT_DOUBLE_EQ(1.0 / 64.0,
                     context->resampler.TargetRatio());

    play = MakePlayDesc(harkentest::ObjectId(99)); // never registered
    EXPECT_EQ(VN_UNKNOWN_ID, harkentest::PlayRaw(&play, &handle));

    play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = harkentest::ObjectId(42); // only the master bus exists
    EXPECT_EQ(VN_UNKNOWN_ID, harkentest::PlayRaw(&play, &handle));
}

TEST(VoicePool, BusTombstonesSkipClaimedAndMigrateRendering) {
    EngineSession session(VoicePoolConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));

    harken::Engine* const engine = session.Engine();
    const std::uint32_t busIndex = engine->buses.FindIndex(harkentest::ObjectId(10));
    ASSERT_EQ(1u, busIndex);

    std::array<float, kBlockFrames> samples{};
    samples.fill(0.25f);
    harken::PCMView view;
    view.channels[0] = samples.data();
    view.channelCount = 1;
    view.stride = 1;
    view.sampleRate = kSampleRate;
    view.frames = samples.size();
    const harken::ResolvedPanGains pan{};

    VnVoiceHandle renderingHandle = 0;
    ASSERT_EQ(VN_OK,
              engine->voices.Play(view, 0.0f, 1.0, busIndex, false, 0.0f,
                                  0.0f, 0.0f, pan, 0, view.frames, 0,
                                  harken::ResampleProfile::LegacyCompatibility,
                                  &renderingHandle));
    engine->voices.ApplyPlay(renderingHandle, renderingHandle, 0, 0);
    auto* const rendering = engine->voices.Resolve(renderingHandle);
    ASSERT_NE(nullptr, rendering);
    ASSERT_EQ(busIndex, rendering->busIndex);

    VnVoiceHandle claimedHandle = 0;
    ASSERT_EQ(VN_OK,
              engine->voices.Play(view, 0.0f, 1.0, busIndex, false, 0.0f,
                                  0.0f, 0.0f, pan, 0, view.frames, 0,
                                  harken::ResampleProfile::LegacyCompatibility,
                                  &claimedHandle));
    auto* const claimed = engine->voices.Resolve(claimedHandle);
    ASSERT_NE(nullptr, claimed);
    const harken::VoiceRenderContext claimedBefore = *claimed;
    ASSERT_EQ(busIndex, claimedBefore.busIndex);
    ASSERT_EQ(0u, claimedBefore.voiceSendActiveCount);

    const std::array<std::uint8_t, 2> alive = {1, 0};
    const std::array<std::uint32_t, 2> parents = {0, 0};
    engine->voices.ApplyBusTombstones(alive.data(), parents.data(),
                                      static_cast<std::uint32_t>(alive.size()));

    // A Claimed context is still being assembled by the logic thread. It must
    // remain byte-for-value unchanged until the play publication is applied.
    EXPECT_EQ(busIndex, claimed->busIndex);
    ExpectVoiceSendSlotsEqual(claimedBefore, *claimed);

    // A Rendering context is audio-thread-owned, so the same tombstone moves
    // its dry bus to the nearest alive ancestor.
    EXPECT_EQ(0u, rendering->busIndex);

    engine->voices.ApplyStop(renderingHandle, 0, 0, engine->notifications, 0);
    engine->voices.ApplyStop(claimedHandle, 0, 0, engine->notifications, 0);
    EXPECT_EQ(0u, engine->voices.ActiveCount());
}

TEST(VoicePool, LogicCancellationAndAudioCompletionReclaimEveryEffect) {
    auto config = VoicePoolConfig(2, kBlockFrames, kSampleRate);
    config.fxSlotsPerVoice = 8;
    config.gainEffectPoolCapacity = 16;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const auto samples = harkentest::MakeDC(4096, 0.125f);
    const auto pcm = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const auto completed = engine.voices.CompletedCount();
    for (unsigned iteration = 0; iteration < 256; ++iteration) {
        SCOPED_TRACE(iteration);
        VnVoiceHandle gated = 0, playing = 0;
        ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &gated, true));
        harken::VoicePool::PreparedCommitProof proof;
        ASSERT_TRUE(CaptureOriginalPreparation(engine, gated, &proof));
        harken::VoicePool::ConsumePreparedCommit(std::move(proof));
        harken::TimelineArmToken arm{};
        ASSERT_TRUE(PrepareOriginalArm(engine, gated, &arm));
        harken::CommandRecord command{};
        command.opcode = harken::kOpcodePlay;
        command.payload.play.batch = {harken::kPlayLedBatchVersion,
            harken::PlayLedBatchKind::TimelineArm, 0, 1,
            (static_cast<std::uint64_t>(arm.generation) << 32) | arm.slot};
        command.payload.play.handle = gated;
        command.payload.play.sourceIdentity = gated;
        ASSERT_TRUE(engine.commands.TryPush(command));
        engine.nullBackend.Pump(1);
        ASSERT_TRUE(engine.voices.Resolve(gated)->timelineGated);
        ASSERT_EQ(VN_OK, ClaimObserverVoice(engine, &playing));
        engine.voices.ApplyPlay(playing, playing, 0, 0);
        for (auto voice : {gated, playing}) {
            for (std::uint32_t slot = 0; slot < 8; ++slot) {
                const auto ref = engine.effectBank.Acquire(VN_EFFECT_GAIN);
                ASSERT_EQ(VN_EFFECT_GAIN, ref.type);
                engine.voices.ApplySetVoiceEffect(voice, slot, ref.type,
                    ref.poolIndex, ref.generation);
            }
        }
        ASSERT_EQ(16u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
        std::barrier ready(2);
        std::thread audio([&] {
            ready.arrive_and_wait();
            engine.voices.ApplyStop(playing, 0, 0, engine.notifications, 0);
        });
        ready.arrive_and_wait();
        const bool cancelled = engine.voices.CancelTimelineBinding(
            static_cast<std::uint32_t>(gated), static_cast<std::uint32_t>(gated >> 32));
        audio.join();
        ASSERT_TRUE(cancelled);
        engine.effectBank.DrainRetire();
        ASSERT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
        EXPECT_EQ(0u, engine.voices.ActiveCount());
        EXPECT_EQ(0u, engine.registry.Lookup(harkentest::ObjectId(1)).useCount->load());
        EXPECT_EQ(completed + (iteration + 1u) * 2u, engine.voices.CompletedCount());
        RetireOriginalArm(engine, arm);
        VnNotification notification{};
        while (engine.notifications.TryPop(notification)) {}
    }
}

TEST(VoicePool, BusTombstonesInterleaveWithPlayAndRelease) {
    constexpr std::uint32_t kQueueCapacity = 8;
    constexpr std::uint32_t kIterations = 1024;
    constexpr std::uint32_t kStreamCapacity = 64;

    EngineSession session(VoicePoolConfig(kQueueCapacity, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* const engine = session.Engine();

    std::array<float, kBlockFrames> samples{};
    samples.fill(0.125f);
    harken::PCMView view;
    view.channels[0] = samples.data();
    view.channelCount = 1;
    view.stride = 1;
    view.sampleRate = kSampleRate;
    view.frames = samples.size();
    const harken::ResolvedPanGains pan{};

    struct PendingVoice {
        PendingVoice() {
            float* planes[harken::kMaxChannels] = {};
            planes[0] = streamStorage.data();
            stream.Wire(planes, kStreamCapacity);
        }

        std::array<float, kStreamCapacity> streamStorage{};
        harken::StreamRing stream;
        VnVoiceHandle handle = 0;
        std::atomic<bool> ready{false};
        std::atomic<bool> free{true};
    };

    std::array<PendingVoice, kQueueCapacity> pending;
    std::atomic<bool> producerDone{false};
    std::atomic<std::uint32_t> playFailures{0};
    std::atomic<std::uint32_t> streamPlays{0};

    // Thread A models the logic side: it claims and fully builds Play/PlayStream
    // contexts, then publishes only the handle through the bounded handoff.
    std::thread logicThread([&] {
        for (std::uint32_t i = 0; i < kIterations; ++i) {
            PendingVoice& slot = pending[i % kQueueCapacity];
            while (!slot.free.load(std::memory_order_acquire)) {
                std::this_thread::yield();
            }
            slot.free.store(false, std::memory_order_relaxed);

            VnResult result = VN_OK;
            if ((i & 1u) == 0) {
                result = engine->voices.Play(
                    view, 0.0f, 1.0, 1, false, 0.0f, 0.0f, 0.0f, pan, 0,
                    view.frames, 0,
                    harken::ResampleProfile::LegacyCompatibility,
                    &slot.handle);
            } else {
                slot.stream.ResetForUse(1);
                slot.stream.state.store(harken::StreamSlotState::Active,
                                        std::memory_order_release);
                result = engine->voices.PlayStream(
                    &slot.stream, 1, kSampleRate, 0, 0.0f, 1.0, 1, false, 0.0f,
                    0.0f, 0.0f, pan, 0, 0, 0, 0, 1,
                    harken::ResampleProfile::LegacyCompatibility,
                    &slot.handle);
                if (result == VN_OK) {
                    streamPlays.fetch_add(1, std::memory_order_relaxed);
                }
            }

            if (result != VN_OK) {
                playFailures.fetch_add(1, std::memory_order_relaxed);
                slot.free.store(true, std::memory_order_release);
                continue;
            }
            slot.ready.store(true, std::memory_order_release);
        }
        producerDone.store(true, std::memory_order_release);
    });

    // Thread B models the audio side: it alone applies play/stop and repeatedly
    // republishes alternating topology tombstone tables while logic claims slots.
    std::thread audioThread([&] {
        std::uint32_t tombstoneTick = 0;
        for (;;) {
            bool observed = false;
            for (PendingVoice& slot : pending) {
                if (!slot.ready.load(std::memory_order_acquire)) {
                    continue;
                }
                observed = true;
                engine->voices.ApplyPlay(
                    slot.handle, slot.handle, 0, tombstoneTick);
                engine->voices.ApplyStop(slot.handle, 0, 0,
                                         engine->notifications, tombstoneTick);
                slot.ready.store(false, std::memory_order_relaxed);
                slot.free.store(true, std::memory_order_release);
                ++tombstoneTick;
            }

            const std::array<std::uint8_t, 2> alive = {
                1, static_cast<std::uint8_t>((tombstoneTick & 1u) == 0u ? 0 : 1)};
            const std::array<std::uint32_t, 2> parents = {0, 0};
            engine->voices.ApplyBusTombstones(
                alive.data(), parents.data(),
                static_cast<std::uint32_t>(alive.size()));

            if (producerDone.load(std::memory_order_acquire)) {
                bool pendingReady = false;
                for (const PendingVoice& slot : pending) {
                    pendingReady |= slot.ready.load(std::memory_order_acquire);
                }
                if (!pendingReady) {
                    break;
                }
            }
            if (!observed) {
                std::this_thread::yield();
            }
        }
    });

    logicThread.join();
    audioThread.join();

    EXPECT_EQ(0u, playFailures.load(std::memory_order_relaxed));
    EXPECT_GT(streamPlays.load(std::memory_order_relaxed), 0u);
    EXPECT_EQ(0u, engine->voices.ActiveCount());
}

TEST(VoicePool, DebugGuardStaysCleanAcrossMultiVoiceRender) {
    EngineSession session(VoicePoolConfig(8, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> sine =
        harkentest::MakeSine(kBlockFrames * 3, 330.0, 48000.0, 0.25f);
    HARKENTEST_ASSERT_NON_SILENT(sine);
    const VnPCMDesc desc = MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    for (int i = 0; i < 8; ++i) {
        VnVoiceHandle handle = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    }

    // Render the full voice lifetime, including end-of-source release, with
    // the allocation guard armed: the audio path must stay heap-silent.
    session.Engine()->nullBackend.Pump(4);
    EXPECT_EQ(0u, session.Engine()->voices.ActiveCount());
    EXPECT_EQ(8u, session.Engine()->voices.CompletedCount());

#ifdef _DEBUG
    EXPECT_TRUE(harken::memguard::IsArmed());
#endif
    EXPECT_EQ(0u, harken::memguard::HitCount());
}

TEST(VoicePool, PreparedFilterAndShelfStoreBitExactDesignAndTail) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> dc = harkentest::MakeDC(kSampleRate, 0.25f);
    const VnPCMDesc pcm = MonoDesc(dc, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1);

    const VnVoiceHandle render =
        harkentest::RenderHandle(session.Engine(), handle);
    harken::VoiceRenderContext* context =
        session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);

    constexpr float cutoff = 1200.0f;
    const harken::BiquadCoeffs lowpass = harken::MakeLowpass(
        static_cast<float>(kSampleRate), cutoff, 0.70710678f);
    const std::uint32_t lowpassTail =
        harken::EstimateBiquadTailFrames(lowpass);
    session.Engine()->voices.ApplySetFilter(
        render, harken::kFilterKindLowpass, true, cutoff, 0.70710678f,
        lowpass, lowpassTail, harken::BiquadSmoothingFrames(kSampleRate));
    EXPECT_TRUE(context->lpfTransition.transitioning);

    constexpr float shelfGain = 6.0f;
    const harken::BiquadCoeffs shelf = harken::MakeLowShelf(
        static_cast<float>(kSampleRate), 250.0f, 0.70710678f, shelfGain);
    const std::uint32_t shelfTail = harken::EstimateBiquadTailFrames(shelf);
    session.Engine()->voices.ApplySetLowShelf(render, shelfGain, shelf,
                                               shelfTail);
    EXPECT_TRUE(context->shelfTransition.transitioning);

    session.Engine()->nullBackend.Pump(3);
    context = session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    EXPECT_TRUE(context->lpfEnabled);
    ExpectCoeffsEqual(lowpass, context->lpfCoeffs);
    EXPECT_EQ(lowpassTail, context->lpfTailFrames);
    EXPECT_TRUE(context->shelfEnabled);
    ExpectCoeffsEqual(shelf, context->shelfCoeffs);
    EXPECT_EQ(shelfTail, context->shelfTailFrames);

    const harken::BiquadCoeffs ignored{0.5f, 0.25f, 0.125f, 0.0625f,
                                      0.03125f};
    session.Engine()->voices.ApplySetLowShelf(render, 0.0f, ignored, 999u);
    EXPECT_TRUE(context->shelfTransition.transitioning);
    session.Engine()->nullBackend.Pump(3);
    context = session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    EXPECT_FALSE(context->shelfEnabled);
    ExpectCoeffsEqual(shelf, context->shelfCoeffs);
    EXPECT_EQ(shelfTail, context->shelfTailFrames);

    // A zero-ramp enable on a live high shelf is a smoothed enable crossfade,
    // not a snap: the prepared design and tail are captured bit-exact as the
    // transition target, and the section reads enabled only once the fade lands.
    const harken::BiquadCoeffs highShelf = harken::MakeHighShelf(
        static_cast<float>(kSampleRate), 6000.0f, 0.70710678f, -6.0f);
    const std::uint32_t highShelfTail =
        harken::EstimateBiquadTailFrames(highShelf);
    session.Engine()->voices.ApplySetShelf(
        render, harken::kShelfKindHigh, true, 6000.0f, 0.70710678f, -6.0f,
        highShelf, highShelfTail, 0);
    ASSERT_TRUE(context->highShelfTransition.transitioning);
    EXPECT_TRUE(context->highShelfTransition.targetEnabled);
    EXPECT_FALSE(context->highShelfEnabled);
    ExpectCoeffsEqual(highShelf, context->highShelfTransition.targetCoeffs);
    EXPECT_EQ(highShelfTail, context->highShelfTransition.targetTailFrames);
    session.Engine()->nullBackend.Pump(3);
    context = session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    ASSERT_TRUE(context->highShelfEnabled);
    ExpectCoeffsEqual(highShelf, context->highShelfCoeffs);
    EXPECT_EQ(highShelfTail, context->highShelfTailFrames);

    // Retuning a live section carries filter memory: the control-rate apply
    // records the new design as the transition target and must not touch live
    // state — the render crossfade, not the apply, migrates the section.
    context->highShelfState[0] = {1.0f, -1.0f};
    const harken::BiquadCoeffs retunedHighShelf = harken::MakeHighShelf(
        static_cast<float>(kSampleRate), 9000.0f, 2.0f, -3.0f);
    const std::uint32_t retunedHighShelfTail =
        harken::EstimateBiquadTailFrames(retunedHighShelf);
    session.Engine()->voices.ApplySetShelf(
        render, harken::kShelfKindHigh, true, 9000.0f, 2.0f, -3.0f,
        retunedHighShelf, retunedHighShelfTail, 0);
    EXPECT_TRUE(context->highShelfTransition.transitioning);
    ExpectCoeffsEqual(retunedHighShelf,
                      context->highShelfTransition.targetCoeffs);
    EXPECT_EQ(retunedHighShelfTail,
              context->highShelfTransition.targetTailFrames);
    EXPECT_FLOAT_EQ(1.0f, context->highShelfState[0].z1);
    EXPECT_FLOAT_EQ(-1.0f, context->highShelfState[0].z2);

    // Disabling preserves live memory for the tail render: the apply targets a
    // dry section and leaves the live state untouched by the control-rate call.
    session.Engine()->nullBackend.Pump(3);
    context = session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    ASSERT_TRUE(context->highShelfEnabled);
    ExpectCoeffsEqual(retunedHighShelf, context->highShelfCoeffs);
    const harken::BiquadState liveBeforeDisable = context->highShelfState[0];
    session.Engine()->voices.ApplySetShelf(
        render, harken::kShelfKindHigh, false, 9000.0f, 2.0f, 0.0f,
        harken::BiquadCoeffs{}, 0, 0);
    EXPECT_TRUE(context->highShelfTransition.transitioning);
    EXPECT_FALSE(context->highShelfTransition.targetEnabled);
    EXPECT_FLOAT_EQ(liveBeforeDisable.z1, context->highShelfState[0].z1);
    EXPECT_FLOAT_EQ(liveBeforeDisable.z2, context->highShelfState[0].z2);

    // A disable followed by a re-enable seeds the section fresh: the enable
    // transition's target state is derived from the running DC input by the seed
    // kernel, not carried over as old dirty memory and not left as literal zero.
    session.Engine()->nullBackend.Pump(3);
    context = session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    ASSERT_FALSE(context->highShelfEnabled);
    session.Engine()->voices.ApplySetShelf(
        render, harken::kShelfKindHigh, true, 9000.0f, 2.0f, -3.0f,
        retunedHighShelf, retunedHighShelfTail, 0);
    ASSERT_TRUE(context->highShelfTransition.transitioning);
    EXPECT_TRUE(context->highShelfTransition.targetEnabled);
    harken::BiquadState expectedSeed;
    harken::SeedBiquadStateForDc(expectedSeed, retunedHighShelf,
                                context->highShelfTransition.lastInput[0]);
    EXPECT_FLOAT_EQ(expectedSeed.z1,
                    context->highShelfTransition.targetState[0].z1);
    EXPECT_FLOAT_EQ(expectedSeed.z2,
                    context->highShelfTransition.targetState[0].z2);
}

// A disable crossfade keeps the section enabled until it completes. An enable
// arriving inside that window must re-arm the enabled state, not be dropped:
// otherwise the disable fade finishes and the section stays bypassed forever.
TEST(VoicePool, LowpassReEnableInsideDisableFadeSurvives) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    constexpr float fs = static_cast<float>(kSampleRate);

    const std::vector<float> dc = harkentest::MakeDC(kSampleRate, 0.25f);
    const VnPCMDesc pcm = MonoDesc(dc, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1); // clear firstBlock

    const VnVoiceHandle render =
        harkentest::RenderHandle(session.Engine(), handle);
    harken::VoiceRenderContext* context =
        session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);

    const harken::BiquadCoeffs lpA =
        harken::MakeLowpass(fs, 1200.0f, 0.70710678f);
    session.Engine()->voices.ApplySetFilter(
        render, harken::kFilterKindLowpass, true, 1200.0f, 0.70710678f, lpA,
        harken::EstimateBiquadTailFrames(lpA), 1024);
    session.Engine()->nullBackend.Pump(8); // settle the enable crossfade
    context = session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    ASSERT_TRUE(context->lpfEnabled);
    ASSERT_FALSE(context->lpfTransition.transitioning);

    // Arm a disable crossfade and advance partway so it is still in flight.
    session.Engine()->voices.ApplySetFilter(
        render, harken::kFilterKindLowpass, false, 1200.0f, 0.70710678f,
        harken::BiquadCoeffs{}, 0, 2048);
    ASSERT_TRUE(context->lpfTransition.transitioning);
    session.Engine()->nullBackend.Pump(1);
    context = session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    ASSERT_TRUE(context->lpfTransition.transitioning);
    ASSERT_TRUE(context->lpfEnabled); // still enabled until the fade completes

    // Re-enable inside the fade window with a new design.
    const harken::BiquadCoeffs lpB =
        harken::MakeLowpass(fs, 4000.0f, 0.70710678f);
    session.Engine()->voices.ApplySetFilter(
        render, harken::kFilterKindLowpass, true, 4000.0f, 0.70710678f, lpB,
        harken::EstimateBiquadTailFrames(lpB), 2048);

    harkentest::Capture cap;
    harkentest::PumpAndCapture(session.Engine(), 24, cap);
    context = session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    EXPECT_TRUE(context->lpfEnabled);
    EXPECT_FALSE(context->lpfTransition.transitioning);
    ExpectCoeffsEqual(lpB, context->lpfCoeffs);
    EXPECT_GT(harkentest::Peak(cap.left), harkentest::kSilenceFloor);
}

TEST(VoicePool, LowShelfReEnableInsideDisableFadeSurvives) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    constexpr float fs = static_cast<float>(kSampleRate);

    const std::vector<float> dc = harkentest::MakeDC(kSampleRate, 0.25f);
    const VnPCMDesc pcm = MonoDesc(dc, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1); // clear firstBlock

    const VnVoiceHandle render =
        harkentest::RenderHandle(session.Engine(), handle);
    harken::VoiceRenderContext* context =
        session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);

    const harken::BiquadCoeffs shA =
        harken::MakeLowShelf(fs, 250.0f, 0.70710678f, 6.0f);
    session.Engine()->voices.ApplySetShelf(
        render, harken::kShelfKindLow, true, 250.0f, 0.70710678f, 6.0f, shA,
        harken::EstimateBiquadTailFrames(shA), 1024);
    session.Engine()->nullBackend.Pump(8);
    context = session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    ASSERT_TRUE(context->shelfEnabled);
    ASSERT_FALSE(context->shelfTransition.transitioning);

    session.Engine()->voices.ApplySetShelf(
        render, harken::kShelfKindLow, false, 250.0f, 0.70710678f, 0.0f,
        harken::BiquadCoeffs{}, 0, 2048);
    ASSERT_TRUE(context->shelfTransition.transitioning);
    session.Engine()->nullBackend.Pump(1);
    context = session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    ASSERT_TRUE(context->shelfTransition.transitioning);
    ASSERT_TRUE(context->shelfEnabled);

    const harken::BiquadCoeffs shB =
        harken::MakeLowShelf(fs, 500.0f, 0.70710678f, -4.0f);
    session.Engine()->voices.ApplySetShelf(
        render, harken::kShelfKindLow, true, 500.0f, 0.70710678f, -4.0f, shB,
        harken::EstimateBiquadTailFrames(shB), 2048);

    harkentest::Capture cap;
    harkentest::PumpAndCapture(session.Engine(), 24, cap);
    context = session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    EXPECT_TRUE(context->shelfEnabled);
    EXPECT_FALSE(context->shelfTransition.transitioning);
    ExpectCoeffsEqual(shB, context->shelfCoeffs);
    EXPECT_GT(harkentest::Peak(cap.left), harkentest::kSilenceFloor);
}

// A retune arriving while a crossfade is live must coalesce as the pending
// request, never overwrite the in-flight target mid-crossfade (whose target
// state was already seeded for the earlier design) and never open a parallel
// parameter glide against the transition.
TEST(VoicePool, LowpassRetuneDuringCrossfadeCoalescesWithoutClobber) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    constexpr float fs = static_cast<float>(kSampleRate);

    const std::vector<float> dc = harkentest::MakeDC(kSampleRate, 0.25f);
    const VnPCMDesc pcm = MonoDesc(dc, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1);

    const VnVoiceHandle render =
        harkentest::RenderHandle(session.Engine(), handle);
    harken::VoiceRenderContext* context =
        session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);

    const harken::BiquadCoeffs lpA =
        harken::MakeLowpass(fs, 800.0f, 0.70710678f);
    session.Engine()->voices.ApplySetFilter(
        render, harken::kFilterKindLowpass, true, 800.0f, 0.70710678f, lpA,
        harken::EstimateBiquadTailFrames(lpA), 1024);
    session.Engine()->nullBackend.Pump(8);
    context = session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    ASSERT_TRUE(context->lpfEnabled);
    ASSERT_FALSE(context->lpfTransition.transitioning);

    // A zero-ramp retune on a live section is now a smoothed crossfade, not a
    // hard swap: it puts a transition in flight with B as the target.
    const harken::BiquadCoeffs lpB =
        harken::MakeLowpass(fs, 1600.0f, 0.70710678f);
    session.Engine()->voices.ApplySetFilter(
        render, harken::kFilterKindLowpass, true, 1600.0f, 0.70710678f, lpB,
        harken::EstimateBiquadTailFrames(lpB), 0);
    ASSERT_TRUE(context->lpfTransition.transitioning);
    ASSERT_TRUE(context->lpfTransition.targetEnabled);
    ExpectCoeffsEqual(lpB, context->lpfTransition.targetCoeffs);

    // A ramped retune to C arriving mid-crossfade must not clobber the live
    // target; it coalesces as pending-latest, and no glide runs in parallel.
    const harken::BiquadCoeffs lpC =
        harken::MakeLowpass(fs, 3200.0f, 0.70710678f);
    session.Engine()->voices.ApplySetFilter(
        render, harken::kFilterKindLowpass, true, 3200.0f, 0.70710678f, lpC,
        harken::EstimateBiquadTailFrames(lpC), 2048);
    EXPECT_TRUE(context->lpfTransition.transitioning);
    ExpectCoeffsEqual(lpB, context->lpfTransition.targetCoeffs); // still B
    EXPECT_TRUE(context->lpfTransition.pendingValid);
    ExpectCoeffsEqual(lpC, context->lpfTransition.pendingCoeffs);
    EXPECT_FALSE(context->lpfGlide.active);

    session.Engine()->nullBackend.Pump(24);
    context = session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    EXPECT_TRUE(context->lpfEnabled);
    EXPECT_FALSE(context->lpfTransition.transitioning);
    ExpectCoeffsEqual(lpC, context->lpfCoeffs);
}

TEST(VoicePool, PreparedToneUpdatePreservesStateAndRejectsStaleType) {
    VnConfig config = VoicePoolConfig(1, kBlockFrames, kSampleRate);
    config.fxSlotsPerVoice = 1;
    config.toneEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> dc = harkentest::MakeDC(kSampleRate, 0.25f);
    const VnPCMDesc pcm = MonoDesc(dc, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    VnThreeBandToneDesc desc{};
    desc.structSize = sizeof(VnThreeBandToneDesc);
    desc.lowShelfGainDb = 3.0f;
    desc.lowShelfFreqHz = 180.0f;
    desc.peakGainDb = 5.0f;
    desc.peakFreqHz = 1200.0f;
    desc.peakQ = 1.25f;
    desc.highShelfGainDb = -2.0f;
    desc.highShelfFreqHz = 7000.0f;
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(
                         handle, 0, VN_EFFECT_THREE_BAND_TONE,
                         AsEffectDesc(&desc)));
    session.Engine()->nullBackend.Pump(1);

    const VnVoiceHandle render =
        harkentest::RenderHandle(session.Engine(), handle);
    harken::VoiceRenderContext* context =
        session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    ASSERT_EQ(VN_EFFECT_THREE_BAND_TONE,
              context->fxSlotEffectRef[0].type);
    auto* tone =
        static_cast<harken::ToneEffectState*>(context->fxSlots[0].state);
    ASSERT_NE(nullptr, tone);

    tone->lowShelfState[0] = {1.0f, -1.0f};
    tone->peakState[0] = {2.0f, -2.0f};
    tone->highShelfState[0] = {3.0f, -3.0f};
    const harken::BiquadCoeffs lowBefore = tone->lowShelf;
    const harken::BiquadCoeffs peakBefore = tone->peak;
    const harken::BiquadCoeffs highBefore = tone->highShelf;
    const std::uint32_t lowTailBefore = tone->lowShelfTailFrames;
    const std::uint32_t peakTailBefore = tone->peakTailFrames;
    const std::uint32_t highTailBefore = tone->highShelfTailFrames;

    constexpr float newPeakFreq = 1750.0f;
    const harken::BiquadCoeffs prepared{0.25f, -0.5f, 0.75f, -0.125f,
                                       0.0625f};
    constexpr std::uint32_t preparedTail = 4321;

    session.Engine()->voices.ApplySetVoiceEffectParam(
        render, 0, harken::kToneParamPeakFreqHz, newPeakFreq,
        VN_EFFECT_FEEDBACK_DELAY, prepared, preparedTail);
    EXPECT_FLOAT_EQ(desc.peakFreqHz, tone->peakFreqHz);
    ExpectCoeffsEqual(peakBefore, tone->peak);
    EXPECT_EQ(peakTailBefore, tone->peakTailFrames);

    session.Engine()->voices.ApplySetVoiceEffectParam(
        render, 0, harken::kToneParamPeakFreqHz, newPeakFreq,
        VN_EFFECT_THREE_BAND_TONE, prepared, preparedTail);
    EXPECT_FLOAT_EQ(newPeakFreq, tone->peakFreqHz);
    EXPECT_TRUE(tone->peakTransition.transitioning);
    ExpectCoeffsEqual(peakBefore, tone->peak);
    EXPECT_EQ(peakTailBefore, tone->peakTailFrames);
    ExpectCoeffsEqual(lowBefore, tone->lowShelf);
    ExpectCoeffsEqual(highBefore, tone->highShelf);
    EXPECT_EQ(lowTailBefore, tone->lowShelfTailFrames);
    EXPECT_EQ(highTailBefore, tone->highShelfTailFrames);
    EXPECT_FLOAT_EQ(1.0f, tone->lowShelfState[0].z1);
    EXPECT_FLOAT_EQ(-1.0f, tone->lowShelfState[0].z2);
    EXPECT_FLOAT_EQ(2.0f, tone->peakState[0].z1);
    EXPECT_FLOAT_EQ(-2.0f, tone->peakState[0].z2);
    EXPECT_FLOAT_EQ(3.0f, tone->highShelfState[0].z1);
    EXPECT_FLOAT_EQ(-3.0f, tone->highShelfState[0].z2);
    session.Engine()->nullBackend.Pump(3);
    EXPECT_FALSE(tone->peakTransition.transitioning);
    ExpectCoeffsEqual(prepared, tone->peak);
    EXPECT_EQ(preparedTail, tone->peakTailFrames);
}

TEST(VoicePoolTail, LiveFilterUpdateDuringDrainRestartsElapsedModel) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> impulse = harkentest::MakeImpulse(300);
    const VnPCMDesc desc = MonoDesc(impulse, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1);

    const VnVoiceHandle renderHandle =
        harkentest::RenderHandle(session.Engine(), handle);
    auto* context = session.Engine()->voices.Resolve(renderHandle);
    ASSERT_NE(nullptr, context);
    ApplyPreparedFilter(session.Engine(), renderHandle,
                        harken::kFilterKindLowpass, 100.0f, true);
    ASSERT_TRUE(context->lpfTransition.transitioning);

    session.Engine()->nullBackend.Pump(2);
    context = session.Engine()->voices.Resolve(renderHandle);
    ASSERT_NE(nullptr, context);
    ASSERT_FALSE(context->lpfTransition.transitioning);
    ASSERT_GT(context->lpfTailFrames, 0u);
    ASSERT_NE(harken::kInfiniteTail, context->lpfTailFrames);
    ASSERT_TRUE(context->tailStarted);
    ASSERT_GT(context->tailElapsed, kBlockFrames);
    ASSERT_LT(context->tailElapsed, context->lpfTailFrames);

    ApplyPreparedFilter(session.Engine(), renderHandle,
                        harken::kFilterKindLowpass, 1.0f, true);
    // A live retune of an already-enabled section is carried by the
    // parameter-domain glide, not the coefficient crossfade, so the in-flight
    // signal is glide.active; the crossfade flag stays clear on this path.
    ASSERT_TRUE(context->lpfGlide.active);
    ASSERT_EQ(harken::kInfiniteTail,
              context->lpfTransition.targetTailFrames);
    EXPECT_FALSE(context->tailRearmSuffixWasInfinite);

    session.Engine()->nullBackend.Pump(1);
    context = session.Engine()->voices.Resolve(renderHandle);
    ASSERT_NE(nullptr, context);
    EXPECT_TRUE(context->tailStarted);
    EXPECT_EQ(harken::kInfiniteTail, context->tailRemaining);
    EXPECT_EQ(kBlockFrames, context->tailElapsed);
}

TEST(VoicePoolTail, LiveShelfUpdateDuringDrainRestartsElapsedModel) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> impulse = harkentest::MakeImpulse(300);
    const VnPCMDesc desc = MonoDesc(impulse, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1);

    const VnVoiceHandle renderHandle =
        harkentest::RenderHandle(session.Engine(), handle);
    auto* context = session.Engine()->voices.Resolve(renderHandle);
    ASSERT_NE(nullptr, context);
    ApplyPreparedLowShelf(session.Engine(), renderHandle, 6.0f);
    ASSERT_TRUE(context->shelfTransition.transitioning);

    session.Engine()->nullBackend.Pump(1);
    context = session.Engine()->voices.Resolve(renderHandle);
    ASSERT_NE(nullptr, context);
    ASSERT_TRUE(context->tailStarted);
    const std::uint32_t elapsedBefore = context->tailElapsed;
    ASSERT_GT(elapsedBefore, 0u);

    ApplyPreparedLowShelf(session.Engine(), renderHandle, 3.0f);
    // At this deterministic timing the enable crossfade (480 frames) is
    // still in flight after one 256-frame block, so the retune coalesces
    // into the active transition (pending-latest) rather than starting a
    // parameter glide; the in-flight signal here is the transition itself.
    ASSERT_TRUE(context->shelfTransition.transitioning);
    ASSERT_GT(context->shelfTransition.targetTailFrames, kBlockFrames);
    EXPECT_EQ(elapsedBefore, context->tailElapsed);

    session.Engine()->nullBackend.Pump(1);
    context = session.Engine()->voices.Resolve(renderHandle);
    ASSERT_NE(nullptr, context);
    EXPECT_TRUE(context->tailStarted);
    EXPECT_LE(context->tailElapsed, kBlockFrames);
}

TEST(VoicePoolTail, LiveEffectParamUpdateDuringDrainRestartsElapsedModel) {
    VnConfig config = VoicePoolConfig(1, kBlockFrames, kSampleRate);
    config.fxSlotsPerVoice = 1;
    config.delayEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> impulse = harkentest::MakeImpulse(300);
    const VnPCMDesc desc = MonoDesc(impulse, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    VnFeedbackDelayDesc delay{};
    delay.structSize = sizeof(VnFeedbackDelayDesc);
    delay.delayMs = 100.0f;
    delay.feedback = 0.9f;
    delay.wet = 1.0f;
    delay.dry = 1.0f;
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, 0,
                                       VN_EFFECT_FEEDBACK_DELAY,
                                       AsEffectDesc(&delay)));

    session.Engine()->nullBackend.Pump(30);
    const VnVoiceHandle renderHandle =
        harkentest::RenderHandle(session.Engine(), handle);
    auto* context = session.Engine()->voices.Resolve(renderHandle);
    ASSERT_NE(nullptr, context);
    ASSERT_TRUE(context->tailStarted);
    ASSERT_GT(context->tailElapsed, 4800u);

    ASSERT_EQ(VN_OK, Vn_SetVoiceEffectParam(
                         handle, 0, harken::kDelayParamFeedback, 0.0f));
    session.Engine()->nullBackend.Pump(1);
    context = session.Engine()->voices.Resolve(renderHandle);
    ASSERT_NE(nullptr, context);
    EXPECT_TRUE(context->tailStarted);
    EXPECT_LE(context->tailElapsed, kBlockFrames);
}

TEST(VoicePoolTail, DownstreamFilterRearmPreservesUpstreamElapsed) {
    VnConfig config = VoicePoolConfig(1, kBlockFrames, kSampleRate);
    config.fxSlotsPerVoice = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> impulse = harkentest::MakeImpulse(300);
    const VnPCMDesc desc = MonoDesc(impulse, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1);

    const VnVoiceHandle renderHandle =
        harkentest::RenderHandle(session.Engine(), handle);
    auto* context = session.Engine()->voices.Resolve(renderHandle);
    ASSERT_NE(nullptr, context);
    std::uint32_t upstreamTail = 5000;
    context->fxSlots[0].process = IdentityTailProcess;
    context->fxSlots[0].tailFrames = ConfiguredTailFrames;
    context->fxSlots[0].state = &upstreamTail;
    ApplyPreparedFilter(session.Engine(), renderHandle,
                        harken::kFilterKindLowpass, 1.0f, true);

    session.Engine()->nullBackend.Pump(5);
    context = session.Engine()->voices.Resolve(renderHandle);
    ASSERT_NE(nullptr, context);
    ASSERT_TRUE(context->tailStarted);
    const std::uint32_t elapsedBefore = context->tailElapsed;
    ASSERT_GT(elapsedBefore, kBlockFrames);
    ASSERT_LT(elapsedBefore, upstreamTail);

    ApplyPreparedFilter(session.Engine(), renderHandle,
                        harken::kFilterKindLowpass, 100.0f, true);
    // The retune drives the parameter glide, which carries the new suffix; the
    // section's target tail is still published for the tail aggregate.
    ASSERT_TRUE(context->lpfGlide.active);
    const std::uint32_t suffixNew =
        context->lpfTransition.targetTailFrames;
    ASSERT_GT(suffixNew, 0u);
    ASSERT_NE(harken::kInfiniteTail, suffixNew);
    ASSERT_EQ(harken::kInfiniteTail, context->lpfTailFrames);
    EXPECT_EQ(elapsedBefore, context->tailElapsed);

    session.Engine()->nullBackend.Pump(1);
    context = session.Engine()->voices.Resolve(renderHandle);
    ASSERT_NE(nullptr, context);
    EXPECT_TRUE(context->lpfGlide.active);
    EXPECT_EQ(harken::kInfiniteTail, context->tailRemaining);
    EXPECT_EQ(elapsedBefore + kBlockFrames, context->tailElapsed);

    session.Engine()->nullBackend.Pump(2);
    context = session.Engine()->voices.Resolve(renderHandle);
    ASSERT_NE(nullptr, context);
    ASSERT_FALSE(context->lpfGlide.active);
    EXPECT_EQ(suffixNew, context->lpfTailFrames);
    const std::uint32_t elapsedAfter = elapsedBefore + 3u * kBlockFrames;
    const std::uint32_t total =
        harken::SaturatingTailAdd(upstreamTail, suffixNew);
    ASSERT_GT(total, elapsedAfter);
    EXPECT_EQ(elapsedAfter, context->tailElapsed);
    EXPECT_EQ(total - elapsedAfter, context->tailRemaining);
}

// A suffix that only becomes infinite because of THIS rearm's own update is a
// genuinely new event: tailElapsed must restart, unlike the already-infinite
// case above. Deterministic control over the tail value (a synthetic effect
// slot, not a real filter's decay estimate) isolates the flag's finite ->
// infinite edge from any question of what a specific biquad design's tail
// estimate happens to be.
TEST(VoicePoolTail, EffectTailBecomingInfiniteMidDrainTriggersLegalReset) {
    VnConfig config = VoicePoolConfig(1, kBlockFrames, kSampleRate);
    config.fxSlotsPerVoice = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> impulse = harkentest::MakeImpulse(300);
    const VnPCMDesc desc = MonoDesc(impulse, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1);

    const VnVoiceHandle renderHandle =
        harkentest::RenderHandle(session.Engine(), handle);
    auto* context = session.Engine()->voices.Resolve(renderHandle);
    ASSERT_NE(nullptr, context);
    std::uint32_t effectTail = 5000;
    context->fxSlots[0].process = IdentityTailProcess;
    context->fxSlots[0].tailFrames = ConfiguredTailFrames;
    context->fxSlots[0].state = &effectTail;

    session.Engine()->nullBackend.Pump(2);
    context = session.Engine()->voices.Resolve(renderHandle);
    ASSERT_NE(nullptr, context);
    ASSERT_TRUE(context->tailStarted);
    ASSERT_GT(context->tailElapsed, kBlockFrames);
    ASSERT_LT(context->tailElapsed, effectTail);
    ASSERT_NE(harken::kInfiniteTail, context->tailRemaining)
        << "the fixture must start finite to exercise a genuine transition "
           "into infinite, not the already-infinite case";

    // Queue the rearm BEFORE mutating the effect's tail, matching the
    // production call order (VoicePool::ApplySetFilter/ApplySetLowShelf/
    // ApplySetVoiceEffectParam all queue before applying their own update),
    // so the snapshot correctly observes the still-finite pre-update value.
    harken::QueueVoiceTailSuffixRearm(*context, 0);
    EXPECT_FALSE(context->tailRearmSuffixWasInfinite)
        << "the suffix was finite immediately before this rearm";
    effectTail = harken::kInfiniteTail;

    session.Engine()->nullBackend.Pump(1);
    context = session.Engine()->voices.Resolve(renderHandle);
    ASSERT_NE(nullptr, context);
    EXPECT_EQ(harken::kInfiniteTail, context->tailRemaining);
    EXPECT_EQ(kBlockFrames, context->tailElapsed)
        << "a suffix that just became infinite must restart tailElapsed";

    harken::QueueVoiceTailSuffixRearm(*context, 0);
    EXPECT_TRUE(context->tailRearmSuffixWasInfinite);
    effectTail = 5000;

    session.Engine()->nullBackend.Pump(1);
    context = session.Engine()->voices.Resolve(renderHandle);
    ASSERT_NE(nullptr, context);
    EXPECT_TRUE(context->tailStarted);
    const std::uint32_t expectedElapsed = kBlockFrames;
    ASSERT_GT(effectTail, expectedElapsed);
    EXPECT_EQ(expectedElapsed, context->tailElapsed);
    EXPECT_EQ(effectTail - expectedElapsed, context->tailRemaining);
}

// Mirrors DownstreamFilterRearmPreservesUpstreamElapsed's filter-only half
// without the upstream effect slot: the lowpass's own tail is already
// infinite (an extremely low cutoff's -60 dB decay time is unrepresentable),
// and retargeting it to a much shorter cutoff must not restart tailElapsed,
// because the suffix was already unbounded before this specific rearm.
TEST(VoicePoolTail, InfiniteFilterTailRearmToShorterTargetPreservesElapsed) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> impulse = harkentest::MakeImpulse(300);
    const VnPCMDesc desc = MonoDesc(impulse, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1);

    const VnVoiceHandle renderHandle =
        harkentest::RenderHandle(session.Engine(), handle);
    auto* context = session.Engine()->voices.Resolve(renderHandle);
    ASSERT_NE(nullptr, context);
    ApplyPreparedFilter(session.Engine(), renderHandle,
                        harken::kFilterKindLowpass, 1.0f, true);

    session.Engine()->nullBackend.Pump(5);
    context = session.Engine()->voices.Resolve(renderHandle);
    ASSERT_NE(nullptr, context);
    ASSERT_TRUE(context->tailStarted);
    ASSERT_EQ(harken::kInfiniteTail, context->tailRemaining)
        << "this fixture's extremely low cutoff must already read as an "
           "infinite tail before the rearm, or it no longer exercises the "
           "already-infinite case this test targets";
    const std::uint32_t elapsedBefore = context->tailElapsed;
    ASSERT_GT(elapsedBefore, 0u);

    // Retarget to a much shorter-tailed cutoff. The suffix is still infinite
    // immediately after this call (the old coefficients remain the active/
    // source side of the crossfade until it completes), but that infinity
    // predates this rearm, so elapsed must keep accumulating, not restart.
    ApplyPreparedFilter(session.Engine(), renderHandle,
                        harken::kFilterKindLowpass, 100.0f, true);
    EXPECT_TRUE(context->tailRearmSuffixWasInfinite)
        << "the suffix was already infinite immediately before this rearm";

    session.Engine()->nullBackend.Pump(1);
    context = session.Engine()->voices.Resolve(renderHandle);
    ASSERT_NE(nullptr, context);
    EXPECT_EQ(elapsedBefore + kBlockFrames, context->tailElapsed)
        << "a suffix that was already infinite must not restart tailElapsed";
}

TEST(VoicePoolTail, RepeatedDownstreamRearmsCompleteWithinSuffixBound) {
    VnConfig config = VoicePoolConfig(1, kBlockFrames, kSampleRate);
    config.fxSlotsPerVoice = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> impulse = harkentest::MakeImpulse(300);
    const VnPCMDesc desc = MonoDesc(impulse, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1);

    const VnVoiceHandle renderHandle =
        harkentest::RenderHandle(session.Engine(), handle);
    auto* context = session.Engine()->voices.Resolve(renderHandle);
    ASSERT_NE(nullptr, context);
    std::uint32_t upstreamTail = 4096;
    context->fxSlots[0].process = IdentityTailProcess;
    context->fxSlots[0].tailFrames = ConfiguredTailFrames;
    context->fxSlots[0].state = &upstreamTail;
    ApplyPreparedFilter(session.Engine(), renderHandle,
                        harken::kFilterKindLowpass, 1.0f, true);
    session.Engine()->nullBackend.Pump(1);

    for (std::uint32_t update = 0; update < 20; ++update) {
        const float cutoff = (update & 1u) == 0 ? 90.0f : 100.0f;
        ApplyPreparedFilter(session.Engine(), renderHandle,
                            harken::kFilterKindLowpass, cutoff, true);
        session.Engine()->nullBackend.Pump(1);
        context = session.Engine()->voices.Resolve(renderHandle);
        ASSERT_NE(nullptr, context) << "update " << update;
    }

    // Updates have stopped. Each rearm restarts the parameter glide, so after
    // the burst the section's active tail is still an in-flight control-rate
    // estimate. Let the glide settle (bounded: one glide span) so the section
    // adopts its final retarget target, then bound completion by that settled
    // suffix. The drain never releases while the glide runs, so the voice must
    // survive the settle. The glide restarting every rearm is exactly what
    // must not wedge or prematurely release the voice.
    for (std::uint32_t block = 0; context != nullptr && context->lpfGlide.active;
         ++block) {
        ASSERT_LT(block, 8u) << "glide failed to settle after updates ceased";
        session.Engine()->nullBackend.Pump(1);
        context = session.Engine()->voices.Resolve(renderHandle);
    }
    ASSERT_NE(nullptr, context);
    ASSERT_FALSE(context->lpfGlide.active);

    const std::uint32_t suffix = context->lpfTailFrames;
    ASSERT_GT(suffix, kBlockFrames);
    EXPECT_GE(context->tailElapsed, upstreamTail);
    const std::uint32_t suffixBlocks =
        (suffix + kBlockFrames - 1u) / kBlockFrames;
    for (std::uint32_t block = 0; block < suffixBlocks; ++block) {
        session.Engine()->nullBackend.Pump(1);
        if (session.Engine()->voices.Resolve(renderHandle) == nullptr) {
            break;
        }
    }
    EXPECT_EQ(nullptr, session.Engine()->voices.Resolve(renderHandle));
}

TEST(MixerPitch, BindingRampIsNotSnap) {
    EngineSession session(VoicePoolConfig(1, kBlockFrames, kSampleRate));
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::vector<float> samples = harkentest::MakeDC(4096, 0.25f);
    const VnPCMDesc desc = MonoDesc(samples, kSampleRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_Update());
    session.Engine()->nullBackend.Pump(1);
    const VnVoiceHandle render =
        harkentest::RenderHandle(session.Engine(), handle);
    harken::VoiceRenderContext* context =
        session.Engine()->voices.Resolve(render);
    ASSERT_NE(nullptr, context);
    session.Engine()->voices.ApplySetPitch(render, 2.0, 50);
    const std::uint32_t expectedFrames =
        (50u * kSampleRate + 999u) / 1000u;
    EXPECT_EQ(expectedFrames, context->resampler.RampFramesRemaining());
    EXPECT_NE(2.0 * context->rateScale, context->resampler.CurrentRatio());
    session.Engine()->voices.ApplySetPitch(render, 0.5, 0);
    EXPECT_EQ(0u, context->resampler.RampFramesRemaining());
    EXPECT_DOUBLE_EQ(0.5 * context->rateScale,
                     context->resampler.CurrentRatio());
}

} // namespace
