// The staged voice start: VnPlayDesc.reserved0's staged-install flag, and
// Vn_CommitVoiceStart.
//
// The defect it exists for is a sequencing one. A voice installed by separate
// calls after it has started is audible in whatever partial state those calls
// had reached when a block came due, because each call publishes on its own.
// The first case below reproduces that on the output buffers: a hard-left pan
// installed after the start is absent for a whole block and then eases in over
// the smoothing window rather than being heard from the first sample.
//
// The staged install answers it by moving publication to one atomic batch.
// Nothing is published before the commit, the commit admits the whole install
// or refuses it, one head release publishes play with every staged value, and
// the drain waits until one quantum can apply the complete batch. These cases
// assert both queue/drain indivisibility and first-block output, plus the flag's
// validation, deterministic capacity refusal, and infallible rollback.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include <cmath>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeDC;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;
using harkentest::Peak;
using harkentest::PumpAndCapture;

constexpr std::uint32_t kBlock = 128;
constexpr std::uint32_t kRate = 48000;
constexpr float kAmp = 0.25f;
constexpr VnSoundId kSound = harkentest::ObjectId(1);
constexpr VnBusId kSendBus = harkentest::ObjectId(2);

VnConfig StagedConfig() {
    VnConfig config = MakeNullConfig(/*maxVoices=*/4, kBlock, kRate);
    config.voiceSendSlots = 2;
    return config;
}

VnGainDesc GainDesc(float gainDb) {
    VnGainDesc gain{};
    gain.structSize = sizeof(VnGainDesc);
    gain.gainDb = gainDb;
    return gain;
}

const VnEffectDesc* AsBase(const void* concrete) {
    return static_cast<const VnEffectDesc*>(concrete);
}

// A play descriptor that opts into the staged install.
VnPlayDesc StagedPlayDesc() {
    VnPlayDesc desc = MakePlayDesc(kSound);
    desc.reserved0 = VN_PLAY_FLAG_STAGED_INSTALL;
    return desc;
}

VnSnapshot Snapshot() {
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    return snapshot;
}

void RegisterSource(const std::vector<float>& samples) {
    const VnPCMDesc desc = MonoDesc(samples, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSound, &desc));
}

// --- the window the staged install closes ---

// Installing the pan after the start is heard: the voice renders a full block
// centered, and even the block after the pan command is only part way to the
// authored image. This is the shape every property installed on a started voice
// has, and the reason the install has to publish at one point.
TEST(StagedVoiceStart, InstallingPanAfterTheStartMissesTheFirstBlock) {
    const std::vector<float> dc = MakeDC(kRate, kAmp);
    EngineSession session(StagedConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    RegisterSource(dc);

    VnPlayDesc play = MakePlayDesc(kSound);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    Capture firstBlock;
    PumpAndCapture(session.Engine(), 1, firstBlock);
    HARKENTEST_ASSERT_NON_SILENT(firstBlock.left);
    // The authored image is hard left, which mutes the right channel. The first
    // audible block does not have it.
    constexpr float kCenterAmplitude = kAmp;
    EXPECT_NEAR(kCenterAmplitude, firstBlock.left.back(), 1e-6f);
    EXPECT_NEAR(kCenterAmplitude, firstBlock.right.back(), 1e-6f)
        << "the first block should still be centered here; if it is not, this "
           "case no longer reproduces the window it documents";

    ASSERT_EQ(VN_OK, Vn_SetVoicePan(handle, -1.0f));
    Capture afterPan;
    PumpAndCapture(session.Engine(), 1, afterPan);
    // Applied to a started voice the balance eases across the smoothing window
    // (longer than one block at this rate), so the authored image is still not
    // in force a block later.
    EXPECT_GT(Peak(afterPan.right), harkentest::kSilenceFloor)
        << "a pan installed after the start was expected to ease in, not snap";
}

// --- the staged install ---

// Staging publishes nothing: the voice holds its Logical identity, reports as a
// pending start, claims no Render Voice, and is inaudible for as long as the
// caller takes to finish installing.
TEST(StagedVoiceStart, StagingIsSilentAndClaimsNoRenderVoice) {
    const std::vector<float> dc = MakeDC(kRate, kAmp);
    EngineSession session(StagedConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    RegisterSource(dc);

    const VnPlayDesc play = StagedPlayDesc();
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_NE(0u, handle);

    Capture staged;
    PumpAndCapture(session.Engine(), 4, staged);
    EXPECT_LE(Peak(staged.left), harkentest::kSilenceFloor);
    EXPECT_LE(Peak(staged.right), harkentest::kSilenceFloor);

    const VnSnapshot snapshot = Snapshot();
    EXPECT_EQ(0u, snapshot.activeVoiceCount);
    EXPECT_EQ(1u, snapshot.pendingStartCount);

    // Vn_Update services deferred starts. A staged install is not one of them:
    // only its commit may publish it, so an update leaves it staged and silent.
    ASSERT_EQ(VN_OK, Vn_Update());
    Capture afterUpdate;
    PumpAndCapture(session.Engine(), 2, afterUpdate);
    EXPECT_LE(Peak(afterUpdate.left), harkentest::kSilenceFloor);
    EXPECT_EQ(1u, Snapshot().pendingStartCount);
}

// A drain budget one record short leaves the complete install queued and silent;
// once the budget can fit it, the whole dressed voice starts in that block.
TEST(StagedVoiceStart, CommittedInstallWaitsUntilItsWholeBatchFitsTheDrain) {
    const std::vector<float> dc = MakeDC(kRate, kAmp);
    EngineSession session(StagedConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* const engine = session.Engine();
    RegisterSource(dc);

    const VnPlayDesc play = StagedPlayDesc();
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_SetVoicePan(handle, -1.0f));
    ASSERT_EQ(VN_OK, Vn_SetVoiceWidth(handle, 1.0f));
    constexpr std::uint32_t kBatchSize = 3;
    for (std::uint32_t i = 0; i < kBatchSize - 1u; ++i) {
        ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, -1.0f, 0));
    }
    ASSERT_EQ(VN_OK, Vn_CommitVoiceStart(handle));
    const std::uint64_t batchSize = engine->commands.PublishedHead() -
                                    engine->commands.ConsumedCount() -
                                    (kBatchSize - 1u);
    ASSERT_EQ(kBatchSize, batchSize);

    engine->mixer.SetMaxCommandsPerQuantumForTest(2u * kBatchSize - 2u);
    Capture waiting;
    PumpAndCapture(engine, 1, waiting);
    EXPECT_LE(Peak(waiting.left), harkentest::kSilenceFloor);
    EXPECT_EQ(batchSize, engine->commands.PublishedHead() -
                             engine->commands.ConsumedCount());

    engine->mixer.SetMaxCommandsPerQuantumForTest(
        static_cast<std::uint32_t>(batchSize));
    Capture started;
    PumpAndCapture(engine, 1, started);
    HARKENTEST_ASSERT_NON_SILENT(started.left);
    EXPECT_EQ(0u, engine->commands.PublishedHead() -
                      engine->commands.ConsumedCount());
    for (float sample : started.right) {
        EXPECT_EQ(0.0f, sample);
    }
}

TEST(StagedVoiceStart, CommittedInstallPansFromItsFirstSample) {
    const std::vector<float> dc = MakeDC(kRate, kAmp);
    EngineSession session(StagedConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    RegisterSource(dc);

    const VnPlayDesc play = StagedPlayDesc();
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_SetVoicePan(handle, -1.0f));
    ASSERT_EQ(VN_OK, Vn_SetVoiceWidth(handle, 1.0f));
    ASSERT_EQ(VN_OK, Vn_CommitVoiceStart(handle));

    Capture firstBlock;
    PumpAndCapture(session.Engine(), 1, firstBlock);
    HARKENTEST_ASSERT_NON_SILENT(firstBlock.left);
    ASSERT_EQ(static_cast<std::size_t>(kBlock), firstBlock.right.size());
    for (std::size_t i = 0; i < firstBlock.right.size(); ++i) {
        EXPECT_EQ(0.0f, firstBlock.right[i])
            << "the authored pan was not in force at frame " << i
            << " of the first audible block";
        EXPECT_FLOAT_EQ(kAmp * std::sqrt(2.0f), firstBlock.left[i]) << "frame " << i;
    }
    EXPECT_EQ(1u, Snapshot().activeVoiceCount);
    EXPECT_EQ(0u, Snapshot().pendingStartCount);
}

TEST(StagedVoiceStart, SpatialCommitInstallsEveryInitialTargetBeforeRendering) {
    const std::vector<float> dc = MakeDC(kRate, kAmp);
    const auto render = [&](bool staged, Capture* output) {
        EngineSession session(StagedConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        auto* engine = session.Engine();
        ASSERT_NO_FATAL_FAILURE(RegisterSource(dc));
        const auto attenuation = harkentest::ObjectId(71);
        auto profile = harkentest::MakeAttenuationDesc(1.0f, 11.0f);
        profile.airAbsorptionNearHz = profile.airAbsorptionFarHz = 1200.0f;
        profile.proximityRadius = 12.0f;
        profile.proximityMaxBoostDb = 6.0f;
        profile.proximityFrequency = 1234.0f;
        profile.widthCurve.enabled = 1;
        profile.widthCurve.pointCount = 2;
        profile.widthCurve.points[0] = {1.0f, 0.6f};
        profile.widthCurve.points[1] = {11.0f, 0.6f};
        profile.widthCurve.handles[0] = harkentest::EndpointHandles(
            profile.widthCurve.points[0], profile.widthCurve.points[1]);
        profile.highpassCurve.enabled = 1;
        profile.highpassCurve.pointCount = 1;
        profile.highpassCurve.points[0] = {0, 350};
        ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(attenuation, &profile));
        auto play = harkentest::MakePlayDesc3D(kSound, 6.0f, 0.0f, 0.0f, -3.0f);
        play.reserved0 = staged ? VN_PLAY_FLAG_STAGED_INSTALL : 0u;
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(voice, attenuation));
        ASSERT_EQ(VN_OK, Vn_SetVoiceDistanceScale(voice, 2.0f));
        ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(voice, -6.0f, 0));
        ASSERT_EQ(VN_OK, Vn_SetVoicePan(voice, -0.25f));
        if (staged) {
            Capture silent;
            PumpAndCapture(engine, 1, silent);
            EXPECT_LE(Peak(silent.left), harkentest::kSilenceFloor);
            EXPECT_LE(Peak(silent.right), harkentest::kSilenceFloor);
            ASSERT_EQ(VN_OK, Vn_CommitVoiceStart(voice));
        } else {
            ASSERT_EQ(VN_OK, Vn_Update());
        }
        const auto handle = harkentest::RenderHandle(engine, voice);
        EXPECT_NEAR(-9.0f + 20.0f * std::log10(0.8f),
            engine->params.ResolvedValue(handle, harken::kTargetVoiceVolume), 1e-5f);
        EXPECT_FLOAT_EQ(0.75f, engine->params.ResolvedValue(handle, harken::kTargetVoicePan));
        EXPECT_FLOAT_EQ(1200.0f, engine->params.ResolvedValue(handle, harken::kTargetVoiceLowpassCutoff));
        EXPECT_FLOAT_EQ(350.0f, engine->params.ResolvedValue(handle, harken::kTargetVoiceHighpassCutoff));
        EXPECT_FLOAT_EQ(0.6f, engine->params.ResolvedValue(handle, harken::kTargetVoiceWidth));
        EXPECT_GT(engine->params.ResolvedValue(handle, harken::kTargetVoiceLowShelfGainDb), 0.0f);
        EXPECT_FLOAT_EQ(1234.0f, engine->params.ResolvedValue(handle, harken::kTargetVoiceLowShelfFreqHz));
        PumpAndCapture(engine, 1, *output);
        const auto* context = engine->voices.Resolve(handle);
        ASSERT_NE(nullptr, context);
        EXPECT_EQ(0u, context->gain.remainingFrames);
    };
    Capture reference, committed;
    ASSERT_NO_FATAL_FAILURE(render(false, &reference));
    ASSERT_NO_FATAL_FAILURE(render(true, &committed));
    HARKENTEST_ASSERT_NON_SILENT(committed.right);
    ASSERT_EQ(reference.right.size(), committed.right.size());
    for (std::size_t frame = 0; frame < reference.right.size(); ++frame) {
        EXPECT_NEAR(reference.left[frame], committed.left[frame], 1e-6f) << frame;
        EXPECT_NEAR(reference.right[frame], committed.right[frame], 1e-6f) << frame;
    }
}

TEST(StagedVoiceStart, MissingCapturedProfileRefusesWithoutPublishingAndCanBeCancelled) {
    const std::vector<float> dc = MakeDC(kRate, kAmp);
    EngineSession session(StagedConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterSource(dc));
    const auto attenuation = harkentest::ObjectId(72);
    const auto profile = harkentest::MakeAttenuationDesc(1.0f, 101.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(attenuation, &profile));
    auto play = harkentest::MakePlayDesc3D(kSound, 0.0f, 0.0f, -51.0f);
    play.reserved0 = VN_PLAY_FLAG_STAGED_INSTALL;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(voice, attenuation));
    auto& pending = engine->pendingStarts[engine->ledger.Classify(voice).ledgerSlot];
    pending.attenuationGeneration = 99;
    const auto head = engine->commands.PublishedHead();
    const auto freeVoices = engine->voices.AvailableSlotCount();
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_CommitVoiceStart(voice));
    EXPECT_EQ(head, engine->commands.PublishedHead());
    EXPECT_EQ(freeVoices, engine->voices.AvailableSlotCount());
    EXPECT_EQ(0u, engine->params.DirtyTargetCount());
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Pending, engine->ledger.Classify(voice).kind);
    Capture silent;
    PumpAndCapture(engine, 1, silent);
    EXPECT_LE(Peak(silent.left), harkentest::kSilenceFloor);
    EXPECT_LE(Peak(silent.right), harkentest::kSilenceFloor);
    ASSERT_EQ(VN_OK, Vn_Stop(voice, 0));
    EXPECT_EQ(0u, Snapshot().pendingStartCount);
    EXPECT_EQ(freeVoices, engine->voices.AvailableSlotCount());
    EXPECT_EQ(0u, engine->spatial.RetainedProfileCount());
}

// The staged effect chain is running on the first audible block: the same source
// committed with a -20 dB Gain step is quieter than without it from block one,
// rather than playing one block at full level first.
TEST(StagedVoiceStart, CommittedInstallRunsItsEffectChainFromItsFirstBlock) {
    const std::vector<float> dc = MakeDC(kRate, kAmp);

    const auto renderFirstBlock = [&dc](bool withGain) {
        Capture out;
        EngineSession session(StagedConfig());
        EXPECT_EQ(VN_OK, session.InitResult());
        const VnPCMDesc desc = MonoDesc(dc, kRate);
        EXPECT_EQ(VN_OK, Vn_RegisterPCM(kSound, &desc));
        const VnPlayDesc play = StagedPlayDesc();
        VnVoiceHandle handle = 0;
        EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        if (withGain) {
            const VnGainDesc gain = GainDesc(-20.0f);
            EXPECT_EQ(VN_OK, Vn_SetVoiceEffect(handle, 0, VN_EFFECT_GAIN,
                                               AsBase(&gain)));
        }
        EXPECT_EQ(VN_OK, Vn_CommitVoiceStart(handle));
        PumpAndCapture(session.Engine(), 1, out);
        return out;
    };

    const Capture plain = renderFirstBlock(false);
    const Capture attenuated = renderFirstBlock(true);
    HARKENTEST_ASSERT_NON_SILENT(plain.left);
    HARKENTEST_ASSERT_NON_SILENT(attenuated.left);
    // -20 dB is a factor of ten; well clear of any tolerance question.
    EXPECT_LT(Peak(attenuated.left), Peak(plain.left) * 0.5f)
        << "the staged effect chain was not running on the first block";
}

// The staged sends are feeding on the first audible block: the send's copy of
// the voice reaches the master through its target bus from block one.
TEST(StagedVoiceStart, CommittedInstallFeedsItsSendsFromItsFirstBlock) {
    const std::vector<float> dc = MakeDC(kRate, kAmp);

    const auto renderFirstBlock = [&dc](bool withSend) {
        Capture out;
        EngineSession session(StagedConfig());
        EXPECT_EQ(VN_OK, session.InitResult());
        EXPECT_EQ(VN_OK, Vn_CreateBus(kSendBus, VN_BUS_MASTER));
        const VnPCMDesc desc = MonoDesc(dc, kRate);
        EXPECT_EQ(VN_OK, Vn_RegisterPCM(kSound, &desc));
        const VnPlayDesc play = StagedPlayDesc();
        VnVoiceHandle handle = 0;
        EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
        if (withSend) {
            EXPECT_EQ(VN_OK, Vn_SetVoiceSend(handle, 0, kSendBus, 0.0f));
        }
        EXPECT_EQ(VN_OK, Vn_CommitVoiceStart(handle));
        PumpAndCapture(session.Engine(), 1, out);
        return out;
    };

    const Capture dry = renderFirstBlock(false);
    const Capture withSend = renderFirstBlock(true);
    HARKENTEST_ASSERT_NON_SILENT(dry.left);
    HARKENTEST_ASSERT_NON_SILENT(withSend.left);
    EXPECT_GT(Peak(withSend.left), Peak(dry.left) * 1.5f)
        << "the staged send was not feeding on the first block";
}

// --- reserved0 validation ---

// The flag-clear play is the ordinary immediate start, unchanged: it is live on
// return rather than pending, is audible on the first block, and still reports a
// full command queue as backpressure — the contract the commit deliberately does
// not share.
TEST(StagedVoiceStart, ClearFlagsKeepTheOrdinaryImmediateStart) {
    const std::vector<float> dc = MakeDC(kRate, kAmp);
    EngineSession session(StagedConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* const engine = session.Engine();
    RegisterSource(dc);

    VnPlayDesc play = MakePlayDesc(kSound);
    play.reserved0 = 0;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    EXPECT_EQ(0u, Snapshot().pendingStartCount)
        << "a flag-clear play must not be staged";

    Capture firstBlock;
    PumpAndCapture(engine, 1, firstBlock);
    HARKENTEST_ASSERT_NON_SILENT(firstBlock.left);
    EXPECT_EQ(1u, Snapshot().activeVoiceCount);

    // Drain the queue to nothing and confirm the immediate path still refuses
    // with backpressure.
    while (engine->commands.FreeSlots() > 0u) {
        ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, -1.0f, 0));
    }
    VnVoiceHandle refused = 0;
    EXPECT_EQ(VN_BACKPRESSURE, harkentest::PlayRaw(&play, &refused));
    EXPECT_EQ(0u, refused);
}

// reserved0 is a bit field with one defined bit. Every other bit is refused
// rather than ignored, and no bit at all is honoured from a vintage that
// predates the commit entry point the flags pair with. Each refusal is
// side-effect-free.
TEST(StagedVoiceStart, UndefinedFlagBitsAreRefusedWithZeroSideEffects) {
    const std::vector<float> dc = MakeDC(kRate, kAmp);
    EngineSession session(StagedConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    RegisterSource(dc);

    const std::uint32_t undefined[] = {
        0x00000002u, 0x00000004u, 0x00000010u, 0x80000000u, 0xFFFFFFFFu,
        VN_PLAY_FLAG_STAGED_INSTALL | 0x00000002u};
    for (const std::uint32_t flags : undefined) {
        VnPlayDesc play = MakePlayDesc(kSound);
        play.reserved0 = flags;
        VnVoiceHandle handle = 0x1234;
        EXPECT_EQ(VN_INVALID_ARG, harkentest::PlayRaw(&play, &handle))
            << "reserved0 = " << flags;
        EXPECT_EQ(0x1234u, handle) << "reserved0 = " << flags;
    }

    // The flags live at an offset every accepted vintage carries, but an older
    // caller has no commit entry point to pair them with, so a set bit there is
    // a mis-set field and not a request.
    VnPlayDesc older = StagedPlayDesc();
    older.structSize =
        static_cast<std::uint32_t>(offsetof(VnPlayDesc, emitterHandle));
    VnVoiceHandle olderHandle = 0x5678;
    EXPECT_EQ(VN_INVALID_ARG, harkentest::PlayRaw(&older, &olderHandle));
    EXPECT_EQ(0x5678u, olderHandle);

    const VnSnapshot snapshot = Snapshot();
    EXPECT_EQ(0u, snapshot.activeVoiceCount);
    EXPECT_EQ(0u, snapshot.pendingStartCount);
    Capture silence;
    PumpAndCapture(session.Engine(), 2, silence);
    EXPECT_LE(Peak(silence.left), harkentest::kSilenceFloor);
}

// --- commit refusals ---

// A commit that cannot get room for the whole install refuses deterministically.
// It is NOT backpressure: that contract promises the identical call succeeds
// after a block drains, and an installer holding a staged voice is not free to
// wait. The staged install is untouched by the refusal — nothing published,
// nothing consumed, still committable once the queue has room.
TEST(StagedVoiceStart, CommitRefusesDeterministicallyWithoutBackpressure) {
    const std::vector<float> dc = MakeDC(kRate, kAmp);
    VnConfig config = StagedConfig();
    config.commandQueueDepth = 16;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* const engine = session.Engine();
    RegisterSource(dc);

    const auto attenuation = harkentest::ObjectId(73);
    const auto profile = harkentest::MakeAttenuationDesc(1.0f, 101.0f);
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(attenuation, &profile));
    auto play = StagedPlayDesc();
    play.spatialized = 1;
    play.positionX = -51.0f;
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    ASSERT_EQ(VN_OK, Vn_BindVoiceAttenuation(handle, attenuation));
    // Staging publishes nothing, so a saturated queue does not obstruct the
    // install itself: every setter below succeeds on an empty-room queue.
    const VnGainDesc gain = GainDesc(-6.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(handle, 0, VN_EFFECT_GAIN,
                                       AsBase(&gain)));
    ASSERT_EQ(VN_OK, Vn_SetVoicePan(handle, -1.0f));
    ASSERT_EQ(VN_OK, Vn_SetVoiceWidth(handle, 0.0f));

    // Play, API pan/width, effect and the six spatial physical commands.
    constexpr auto demand = 4u + harken::kSpatialDrivenTargetsPerVoice;
    while (engine->commands.FreeSlots() > demand - 1u) {
        ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, -1.0f, 0));
    }
    ASSERT_EQ(demand - 1u, engine->commands.FreeSlots());

    const VnResult refused = Vn_CommitVoiceStart(handle);
    EXPECT_NE(VN_BACKPRESSURE, refused)
        << "the commit must not hand a mid-install caller a retry contract";
    EXPECT_EQ(VN_NO_VOICE, refused);
    EXPECT_EQ(demand - 1u, engine->commands.FreeSlots())
        << "a refused commit published nothing";

    const VnSnapshot afterRefusal = Snapshot();
    EXPECT_EQ(0u, afterRefusal.activeVoiceCount);
    EXPECT_EQ(1u, afterRefusal.pendingStartCount)
        << "the refused install must still be staged and cancellable";
    Capture silence;
    PumpAndCapture(engine, 1, silence);
    EXPECT_LE(Peak(silence.left), harkentest::kSilenceFloor);

    // Once the queue has drained, the same install commits and is dressed on its
    // first audible block: the refusal cost it nothing.
    engine->nullBackend.Pump(2);
    ASSERT_GE(engine->commands.FreeSlots(), demand);
    ASSERT_EQ(VN_OK, Vn_CommitVoiceStart(handle));
    Capture committed;
    PumpAndCapture(engine, 1, committed);
    HARKENTEST_ASSERT_NON_SILENT(committed.left);
    EXPECT_LE(Peak(committed.right), harkentest::kSilenceFloor);
}

// Only a staged install can be committed. A started voice has nothing left to
// publish, a stale handle names nothing, and a second commit of the same voice
// is the first of those — each refused without touching the voice.
TEST(StagedVoiceStart, CommitRejectsHandlesThatAreNotStagedInstalls) {
    const std::vector<float> dc = MakeDC(kRate, kAmp);
    EngineSession session(StagedConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    RegisterSource(dc);

    EXPECT_EQ(VN_STALE_HANDLE, Vn_CommitVoiceStart(0x1234));

    const VnPlayDesc immediate = MakePlayDesc(kSound);
    VnVoiceHandle live = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&immediate, &live));
    EXPECT_EQ(VN_INVALID_ARG, Vn_CommitVoiceStart(live));
    Capture stillPlaying;
    PumpAndCapture(session.Engine(), 1, stillPlaying);
    HARKENTEST_ASSERT_NON_SILENT(stillPlaying.left);
    ASSERT_EQ(VN_OK, Vn_Stop(live, 0));

    const VnPlayDesc play = StagedPlayDesc();
    VnVoiceHandle staged = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &staged));
    ASSERT_EQ(VN_OK, Vn_CommitVoiceStart(staged));
    EXPECT_EQ(VN_INVALID_ARG, Vn_CommitVoiceStart(staged))
        << "a committed start must not be publishable twice";
}

// --- rollback ---

// Cancelling a staged install cannot fail and leaves nothing behind. It needs no
// command-queue room (nothing was published, so nothing has to be undone on the
// audio side), it leaves no audible voice and no pending start, and it returns
// the effect-pool instance the staged chain held — proven by installing the same
// chain again on a pool with room for exactly one.
TEST(StagedVoiceStart, CancellingAStagedInstallCannotFailAndLeaksNothing) {
    const std::vector<float> dc = MakeDC(kRate, kAmp);
    VnConfig config = StagedConfig();
    config.gainEffectPoolCapacity = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* const engine = session.Engine();
    RegisterSource(dc);

    const VnPlayDesc play = StagedPlayDesc();
    VnVoiceHandle first = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &first));
    const VnGainDesc gain = GainDesc(-6.0f);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(first, 0, VN_EFFECT_GAIN,
                                       AsBase(&gain)));
    ASSERT_EQ(VN_OK, Vn_SetVoicePan(first, -1.0f));

    // Saturate the queue: the rollback must not depend on room it cannot get.
    while (engine->commands.FreeSlots() > 0u) {
        ASSERT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, -1.0f, 0));
    }
    ASSERT_EQ(VN_OK, Vn_Stop(first, 0))
        << "cancelling a staged install must not be refusable";

    const VnSnapshot afterRollback = Snapshot();
    EXPECT_EQ(0u, afterRollback.activeVoiceCount);
    EXPECT_EQ(0u, afterRollback.pendingStartCount);
    Capture silence;
    PumpAndCapture(engine, 2, silence);
    EXPECT_LE(Peak(silence.left), harkentest::kSilenceFloor)
        << "a cancelled install must leave no audible voice";

    // The pool holds one Gain instance. A leak here refuses this install.
    engine->nullBackend.Pump(2);
    ASSERT_GE(engine->commands.FreeSlots(), 3u);
    VnVoiceHandle second = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &second));
    EXPECT_EQ(VN_OK, Vn_SetVoiceEffect(second, 0, VN_EFFECT_GAIN,
                                       AsBase(&gain)))
        << "the cancelled install did not return its effect-pool instance";
    ASSERT_EQ(VN_OK, Vn_CommitVoiceStart(second));
    Capture playing;
    PumpAndCapture(engine, 1, playing);
    HARKENTEST_ASSERT_NON_SILENT(playing.left);
}

} // namespace
