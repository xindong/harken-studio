#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"
#include "Engine.h"
#include "CommandQueueSubmissionTestAccess.h"
#include "VoicePoolReleaseTestAccess.h"
#include "harken/render/BuiltinEffects.h"
#include "harken/core/PrivateOriginalQueueGroupClockProof.h"
#include "harken/core/PrivateSelectedQueueFutureSpan.h"
#include "harken/core/PrivateSelectedQueueFutureCalendar.h"
#include "harken/core/OriginalImmediateLaneConditionalProjection.h"
#include "harken/core/BlockArena.h"
#include "harken/render/ImmediateLaneTimelineFinalizer.h"
#include <type_traits>

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <memory>
#include <span>
#include <vector>

namespace {
constexpr std::uint32_t kFrames = 64;
constexpr std::uint32_t kRate = 48000;
constexpr auto kSound = harkentest::ObjectId(21);
using Registry = harken::OriginalSourceSubmissionRegistry;

VnConfig SourceBodyConfig() {
    auto config = harkentest::MakeNullConfig(4, kFrames, kRate);
    // Direct firing-table proofs do not dispatch nested Action programs.
    config.maxTriggerDepth = 1;
    // The firing cases retain only a parent and child in each engine.
    config.maxActionFirings = 8;
    config.fxSlotsPerVoice = 1;
    config.gainEffectPoolCapacity = 2;
    return config;
}

float CurrentPeak(const harken::Engine& engine) {
    float result = 0.0f;
    for (std::uint32_t i = 0; i < kFrames * engine.nullBackend.Channels(); ++i)
        result = std::max(result, std::abs(engine.nullBackend.LastBlock()[i]));
    return result;
}

struct SourceBody {
    VnVoiceHandle independent = 0;
    VnVoiceHandle independentPublic = 0;
    harken::VoiceReleaseObserver independentRelease{};
    VnVoiceHandle source = 0;
    harken::VoiceReleaseObserver release{};
    harken::TimelineArmToken arm{};
    harken::EffectInstanceRef incoming{};
    std::uint32_t commandCount = 0;
    std::uint32_t armFreeBefore = 0;
    std::uint64_t acceptedHead = 0;
    float baseline = 0.0f;
};

void StartIndependent(harken::Engine& engine, SourceBody& body,
    const std::vector<float>& samples, bool registerPCM = true) {
    const auto pcm = harkentest::MonoDesc(samples, kRate);
    if (registerPCM) ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSound, &pcm));
    ASSERT_TRUE(engine.registry.Lookup(kSound).Valid());
    auto play = harkentest::MakePlayDesc(kSound);
    play.loopCount = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &body.independentPublic));
    body.independent = harkentest::RenderHandle(&engine, body.independentPublic);
    ASSERT_NE(0u, body.independent);
    ASSERT_TRUE(engine.voices.CaptureReleaseObserver(body.independent, &body.independentRelease));
    const auto before = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    body.baseline = CurrentPeak(engine);
    ASSERT_GT(body.baseline, 0.1f);
}

// Supplementary issuer-level integration: no Lane Start is issued for this Source.
void PublishSourceBody(harken::Engine& engine, SourceBody& body,
    bool withEffect, bool crossTargetEffect,
    Registry::OriginalSourceLifetimePin* original = nullptr,
    bool finalSource = false, bool initialSource = false,
    harken::VoicePool::OriginalSourceBodyLifetimePin* originalBody = nullptr) {
    ASSERT_FALSE(initialSource && withEffect);
    const auto pcm = engine.registry.Lookup(kSound);
    ASSERT_TRUE(pcm.Valid());
    ASSERT_EQ(VN_OK, engine.voices.Play(pcm, 0.0f, 1.0, 0, false, 0, 0, 0,
        harken::ResolvePanGains(engine.config.mixChannelLayout, 0.0f),
        0, pcm.frames, 1, harken::ResampleProfile::Professional44148,
        &body.source, 1, UINT32_MAX, 0, true));
    ASSERT_TRUE(engine.voices.CaptureReleaseObserver(body.source, &body.release));
    harken::VoicePool::PreparedCommitInput physical{};
    physical.renderHandle = body.source;
    physical.originalRelease = body.release;
    ASSERT_TRUE(engine.voices.CapturePreparedRoutingV1(body.source, &physical.routing));
    harken::VoicePool::PreparedCommitProof voiceProof;
    ASSERT_TRUE(engine.voices.ProvePreparedCommit(physical, &voiceProof));
    harken::TimelineArmTable::PreparedCommitInput logical{};
    logical.startFrame = engine.mixer.BlocksRendered() * kFrames;
    logical.armDeadline = logical.startFrame;
    logical.target.subjectId = kSound;
    body.armFreeBefore = engine.timelineArms.FreeCount();
    ASSERT_TRUE(engine.timelineArms.Prepare(static_cast<std::uint32_t>(body.source),
        static_cast<std::uint32_t>(body.source >> 32), logical.startFrame,
        logical.armDeadline, logical.programGeneration, logical.target, &body.arm));
    logical.arm = body.arm;
    harken::TimelineArmTable::PreparedCommitProof armProof;
    ASSERT_TRUE(engine.timelineArms.ProvePreparedCommit(logical, &armProof));
    std::array<harken::EffectPoolBank::PreparedCommitProof, 1> effectProofs;
    const auto effectCount = withEffect ? 1u : 0u;
    if (withEffect) {
        VnGainDesc descriptor{};
        descriptor.structSize = sizeof(descriptor);
        descriptor.gainDb = 0.0f;
        ASSERT_EQ(VN_OK, harken::ValidateEffectDesc(VN_EFFECT_GAIN, &descriptor, kRate));
        body.incoming = engine.effectBank.AcquirePrepared(VN_EFFECT_GAIN);
        ASSERT_EQ(VN_EFFECT_GAIN, body.incoming.type);
        auto* state = engine.effectBank.StateFor(body.incoming.type, body.incoming.poolIndex);
        ASSERT_NE(nullptr, state);
        auto kernel = harken::BuildEffect(body.incoming.type, state);
        ASSERT_NE(nullptr, kernel.reset);
        kernel.reset(state);
        harken::ApplyEffectDesc(VN_EFFECT_GAIN, &descriptor, state);
        ASSERT_TRUE(engine.effectBank.ProvePreparedCommit(body.incoming, {}, &effectProofs[0]));
    }
    std::array<harken::CommandRecord, 3> commands{};
    body.commandCount = initialSource ? 3u : 2u + effectCount;
    auto& header = commands[0];
    header.opcode = harken::kOpcodePlay;
    header.payload.play.handle = body.source;
    header.payload.play.sourceIdentity = body.source;
    header.payload.play.batch.batchVersion = harken::kPlayLedBatchVersion;
    header.payload.play.batch.batchKind = harken::PlayLedBatchKind::TimelineArm;
    header.payload.play.batch.batchCommandCount = body.commandCount;
    header.payload.play.batch.armToken =
        (static_cast<std::uint64_t>(body.arm.generation) << 32) | body.arm.slot;
    commands[1].opcode = harken::kOpcodeSetVoiceVolume;
    commands[1].payload.setVoiceVolume.handle = body.independent;
    commands[1].payload.setVoiceVolume.volumeDb = -6.0205999f;
    if (initialSource) {
        commands[1] = {};
        commands[1].opcode = harken::kOpcodeSetVoicePan;
        auto& pan = commands[1].payload.setVoicePan;
        pan.handle = body.source;
        pan.pan = 0.25f;
        pan.pan2d = 0.25f;
        const auto gains = harken::ResolvePanGains(engine.config.mixChannelLayout, pan.pan);
        pan.gain0 = gains.gain0;
        pan.gain1 = gains.gain1;
        pan.channel0 = gains.channel0;
        pan.channel1 = gains.channel1;
        commands[2].opcode = harken::kOpcodeSetVoiceSpatialScalar;
        commands[2].payload.setVoiceSpatialScalar.handle = body.source;
        commands[2].payload.setVoiceSpatialScalar.kind = harken::kSpatialScalarKindWidth;
        commands[2].payload.setVoiceSpatialScalar.value = 0.4f;
    }
    if (withEffect) {
        commands[2].opcode = harken::kOpcodeSetVoiceEffect;
        auto& payload = commands[2].payload.setVoiceEffect;
        payload.handle = crossTargetEffect ? body.independent : body.source;
        payload.slot = 0;
        payload.effectType = body.incoming.type;
        payload.poolIndex = body.incoming.poolIndex;
        payload.poolGeneration = body.incoming.generation;
    }
    if (finalSource) {
        ASSERT_TRUE(engine.originalSourceSubmissions.CapturePreparedFinalSource(header,
            armProof, voiceProof, std::span(effectProofs.data(), effectCount), original, originalBody));
        EXPECT_FALSE(engine.originalSourceSubmissions.CapturePreparedFinalSource(header,
            armProof, voiceProof, std::span(effectProofs.data(), effectCount)));
        EXPECT_FALSE(engine.originalSourceSubmissions.CapturePreparedSource(header,
            armProof, voiceProof, std::span(effectProofs.data(), effectCount)));
    } else {
        ASSERT_TRUE(engine.originalSourceSubmissions.CapturePreparedSource(header,
            armProof, voiceProof, std::span(effectProofs.data(), effectCount), original, originalBody));
    }
    harken::CommandQueue::BatchReservation reservation{};
    ASSERT_TRUE(initialSource ? engine.commands.ReserveInitialSource(&reservation) :
        engine.commands.ReserveBatch(body.commandCount, &reservation));
    harken::CommandQueue::PreparedBatchProof batch;
    ASSERT_TRUE(engine.commands.ProvePreparedBatch(&reservation, commands.data(), body.commandCount, &batch));
    ASSERT_TRUE(engine.originalSourceSubmissions.BindPreparedChild(batch, 0, body.commandCount));
    if (withEffect) std::move(effectProofs[0]).Consume();
    harken::VoicePool::ConsumePreparedCommit(std::move(voiceProof));
    harken::TimelineArmTable::ConsumePreparedCommit(std::move(armProof));
    engine.originalSourceSubmissions.PublishPreparedChild(header);
    (void)engine.commands.ConsumePreparedBatch(std::move(batch));
    if (withEffect) engine.effectBank.PublishInstalled(body.incoming);
    body.acceptedHead = engine.commands.PublishedHead();
    ASSERT_EQ(Registry::Phase::Published, engine.originalSourceSubmissions.Find(body.arm)->State());
}

void CheckCompletedDisposition(harken::Engine& engine, const SourceBody& body,
    bool cancelled, float factor) {
    const auto before = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    EXPECT_EQ(body.acceptedHead, engine.commands.ConsumedCount());
    const auto* original = engine.originalSourceSubmissions.Find(body.arm);
    ASSERT_NE(nullptr, original);
    EXPECT_EQ(Registry::Phase::Disposed, original->State());
    const auto* row = engine.timelineArms.RowAt(body.arm.slot);
    EXPECT_FALSE(row->originalLifetimePinned.load());
    EXPECT_EQ(0u, row->startedBlockSequence.load());
    EXPECT_EQ(cancelled ? 0u : before + 1u, row->armedCompletionSequence.load());
    EXPECT_EQ(cancelled ? before + 1u : 0u, row->tombstoneAudioSequence.load());
    EXPECT_EQ(cancelled, engine.voices.ReleaseObserved(body.release));
    EXPECT_NEAR(body.baseline * factor, CurrentPeak(engine), 0.00001f);
}

void RecycleExactRetirement(harken::Engine& engine, const SourceBody& body) {
    unsigned exact = 0, others = 0;
    harken::EffectInstanceRef retired{};
    while (engine.effectBank.RetireQueue().TryPop(retired)) {
        if (Registry::SameEffect(retired, body.incoming)) ++exact;
        else ++others;
        engine.effectBank.Recycle(retired);
    }
    EXPECT_EQ(1u, exact);
    EXPECT_EQ(0u, others);
    EXPECT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
    EXPECT_FALSE(engine.effectBank.RetireQueue().TryPop(retired));
}

void RetireSource(harken::Engine& engine, const SourceBody& body, bool cancelled,
    std::uint32_t expectedFree = UINT32_MAX) {
    std::uint64_t stopBlock = 0;
    if (!cancelled) {
        ASSERT_TRUE(engine.timelineArms.RequestCancel(body.arm));
        harken::CommandRecord stop{};
        stop.opcode = harken::kOpcodeStop;
        stop.payload.stop.handle = body.source;
        ASSERT_TRUE(engine.commands.TryPush(stop));
        const auto before = engine.mixer.BlocksRendered();
        stopBlock = before;
        engine.nullBackend.Pump(1);
        ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
        ASSERT_TRUE(engine.voices.ReleaseObserved(body.release));
    }
    ASSERT_TRUE(engine.voices.ReleaseObserved(body.release));
    EXPECT_EQ(nullptr, engine.voices.Resolve(body.source));
    harken::TimelineArmObservation terminal{};
    ASSERT_TRUE(engine.timelineArms.Observe(body.arm, &terminal));
    ASSERT_EQ(harken::ArmState::Tombstoned, terminal.state);
    if (cancelled) {
        ASSERT_EQ(engine.mixer.BlocksRendered(), terminal.tombstoneAudioSequence);
        EXPECT_FALSE(engine.timelineArms.Reclaim(body.arm, engine.mixer.BlocksRendered()));
        const auto before = engine.mixer.BlocksRendered();
        engine.nullBackend.Pump(1);
        ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    } else {
        // ApplyStop already tombstones its installed Arm using the actual block index.
        EXPECT_EQ(stopBlock, terminal.tombstoneAudioSequence);
    }
    ASSERT_GT(engine.mixer.BlocksRendered(), terminal.tombstoneAudioSequence);
    ASSERT_TRUE(engine.timelineArms.Reclaim(body.arm, engine.mixer.BlocksRendered()));
    EXPECT_FALSE(engine.timelineArms.Observe(body.arm, &terminal));
    EXPECT_EQ(expectedFree == UINT32_MAX ? body.armFreeBefore : expectedFree,
        engine.timelineArms.FreeCount());
}

void RetireIndependent(harken::Engine& engine, const SourceBody& body) {
    if (!engine.voices.ReleaseObserved(body.independentRelease)) {
        ASSERT_EQ(VN_OK, Vn_Stop(body.independentPublic, 0));
        const auto before = engine.mixer.BlocksRendered();
        engine.nullBackend.Pump(1);
        ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    }
    ASSERT_TRUE(engine.voices.ReleaseObserved(body.independentRelease));
    EXPECT_EQ(nullptr, engine.voices.Resolve(body.independent));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, engine.ledger.ToRender(body.independentPublic));
    EXPECT_EQ(0u, engine.voices.ActiveCount());
    EXPECT_EQ(0u, engine.registry.Lookup(kSound).useCount->load());
}
}

TEST(OriginalSourceSubmission, InitialEntryReuseKeepsActualBodyReaderAndInstallationAlive) {
    auto config = SourceBodyConfig();
    config.commandQueueDepth = 1;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    const auto pcm = harkentest::MonoDesc(samples, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSound, &pcm));
    EXPECT_EQ(0u, engine.commands.InitialSourceCommittedBytes());
    SourceBody first, second;
    Registry::OriginalSourceLifetimePin source;
    harken::VoicePool::OriginalSourceBodyLifetimePin body;
    ASSERT_NO_FATAL_FAILURE(PublishSourceBody(engine, first, false, false, &source, false, true, &body));
    const auto cut = engine.commands.CaptureBlockInputCut();
    ASSERT_NE(nullptr, engine.commands.InitialSourceAtFront(cut.initialHead));
    EXPECT_EQ(1u, engine.commands.InitialSourceAtFront(cut.initialHead)->range->Count());
    EXPECT_EQ(3u, engine.commands.InitialSourceAtFront(cut.initialHead)->range->FactCount());
    engine.nullBackend.Pump(1);
    ASSERT_EQ(Registry::Phase::Disposed, engine.originalSourceSubmissions.Find(first.arm)->State());
    EXPECT_EQ(1u, engine.timelineArms.RowAt(first.arm.slot)->armedCompletionSequence.load());
    EXPECT_EQ(0.0f, CurrentPeak(engine));
    EXPECT_EQ(1u, engine.commands.InitialSourcesConsumed());
    EXPECT_TRUE(engine.voices.IsOriginalSourceBodyLifetimePinFor(body, first.arm, first.source));
    ASSERT_NO_FATAL_FAILURE(PublishSourceBody(engine, second, false, false, nullptr, false, true));
    ASSERT_NO_FATAL_FAILURE(CheckCompletedDisposition(engine, second, false, 1.0f));
    EXPECT_EQ(2u, engine.commands.InitialSourcesConsumed());
    EXPECT_EQ(0u, engine.commands.ConsumedCount());
    EXPECT_TRUE(engine.voices.IsOriginalSourceBodyLifetimePinFor(body, first.arm, first.source));
    body.Release();
    source.Release();
    ASSERT_NO_FATAL_FAILURE(RetireSource(engine, first, false, first.armFreeBefore - 1u));
    ASSERT_NO_FATAL_FAILURE(RetireSource(engine, second, false, first.armFreeBefore));
}

TEST(OriginalSourceSubmission, InitialBarriersShareRawBudgetAndPreserveFollowingSetter) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    engine.mixer.SetMaxCommandsPerQuantumForTest(2);
    const std::vector<float> samples(kRate, 0.25f);
    const auto pcm = harkentest::MonoDesc(samples, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSound, &pcm));
    SourceBody first, second;
    ASSERT_NO_FATAL_FAILURE(PublishSourceBody(engine, first, false, false, nullptr, false, true));
    const auto setter = [&](float volumeDb) {
        harken::CommandRecord record{};
        record.opcode = harken::kOpcodeSetVoiceVolume;
        record.payload.setVoiceVolume.handle = first.source;
        record.payload.setVoiceVolume.volumeDb = volumeDb;
        return engine.commands.TryPush(record);
    };
    ASSERT_TRUE(setter(-6.0205999f));
    ASSERT_TRUE(setter(-12.0411998f));
    ASSERT_NO_FATAL_FAILURE(PublishSourceBody(engine, second, false, false, nullptr, false, true));
    ASSERT_TRUE(setter(-18.0617997f));
    engine.nullBackend.Pump(1);
    EXPECT_EQ(2u, engine.commands.InitialSourcesConsumed());
    EXPECT_EQ(2u, engine.commands.ConsumedCount());
    EXPECT_EQ(1u, engine.mixer.CommandsDeferredLastBlock());
    ASSERT_NE(nullptr, engine.voices.Resolve(first.source));
    EXPECT_NEAR(0.25f, engine.voices.Resolve(first.source)->gain.current, 0.00001f);
    EXPECT_FALSE(engine.timelineArms.SourcePublicationAccepted(first.arm));
    EXPECT_FALSE(engine.timelineArms.SourcePublicationAccepted(second.arm));
    engine.AcceptSourceCommandPublications();
    EXPECT_FALSE(engine.timelineArms.SourcePublicationAccepted(first.arm));
    engine.nullBackend.Pump(1);
    EXPECT_TRUE(engine.timelineArms.SourcePublicationAccepted(first.arm));
    EXPECT_TRUE(engine.timelineArms.SourcePublicationAccepted(second.arm));
    EXPECT_EQ(3u, engine.commands.ConsumedCount());
    EXPECT_EQ(0u, engine.mixer.CommandsDeferredLastBlock());
    EXPECT_NEAR(0.125f, engine.voices.Resolve(first.source)->gain.current, 0.00001f);
    ASSERT_NO_FATAL_FAILURE(RetireSource(engine, first, false, first.armFreeBefore - 1u));
    ASSERT_NO_FATAL_FAILURE(RetireSource(engine, second, false, first.armFreeBefore));
    SourceBody reused;
    ASSERT_NO_FATAL_FAILURE(PublishSourceBody(engine, reused, false, false));
    ASSERT_NE(nullptr, engine.commands.OriginalRangeAtFront());
    EXPECT_EQ(harken::CommandQueue::OriginalRangeKind::Source,
        engine.commands.OriginalRangeAtFront()->Kind());
    ASSERT_NO_FATAL_FAILURE(CheckCompletedDisposition(engine, reused, false, 1.0f));
    EXPECT_FALSE(engine.timelineArms.SourcePublicationAccepted(reused.arm));
    engine.AcceptSourceCommandPublications();
    EXPECT_FALSE(engine.timelineArms.SourcePublicationAccepted(reused.arm));
    engine.nullBackend.Pump(1);
    EXPECT_TRUE(engine.timelineArms.SourcePublicationAccepted(reused.arm));
    ASSERT_NO_FATAL_FAILURE(RetireSource(engine, reused, false));
}

TEST(OriginalSourceSubmission, ApplyPreservesLegalIndependentTargetSetterAndFreshPcm) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    ASSERT_NO_FATAL_FAILURE(PublishSourceBody(engine, body, false, false));
    ASSERT_NO_FATAL_FAILURE(CheckCompletedDisposition(engine, body, false, 0.5f));
    ASSERT_NO_FATAL_FAILURE(RetireSource(engine, body, false));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
}

TEST(OriginalSourceSubmission, CancelPreservesLegalIndependentTargetSetterAndFreshPcm) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    ASSERT_NO_FATAL_FAILURE(PublishSourceBody(engine, body, false, false));
    ASSERT_TRUE(engine.timelineArms.RequestCancel(body.arm));
    ASSERT_NO_FATAL_FAILURE(CheckCompletedDisposition(engine, body, true, 0.5f));
    ASSERT_NO_FATAL_FAILURE(RetireSource(engine, body, true));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
}

TEST(OriginalSourceSubmission, FinalSourcePublishesOnlyAfterCompleteInstallation) {
    for (const bool finalSource : {false, true}) {
        SCOPED_TRACE(finalSource);
        harkentest::EngineSession session(SourceBodyConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        const std::vector<float> samples(kRate, 0.25f);
        SourceBody body;
        ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
        ASSERT_NO_FATAL_FAILURE(PublishSourceBody(engine, body, true, false, nullptr, finalSource));
        const auto* row = engine.timelineArms.RowAt(body.arm.slot);
        ASSERT_EQ(harken::ArmState::PendingApply, row->state.load());
        EXPECT_EQ(finalSource, row->finalSourcePublicationIssued);
        EXPECT_FALSE(row->finalSourcePublicationConsumed);
        ASSERT_NO_FATAL_FAILURE(CheckCompletedDisposition(engine, body, false, 0.5f));
        EXPECT_EQ(finalSource ? harken::ArmState::Published : harken::ArmState::Armed, row->state.load());
        EXPECT_EQ(finalSource, row->finalSourcePublicationConsumed);
        ASSERT_NE(nullptr, engine.voices.Resolve(body.source));
        EXPECT_TRUE(Registry::SameEffect(body.incoming, engine.voices.Resolve(body.source)->fxSlotEffectRef[0]));
        const auto completed = row->armedCompletionSequence.load();
        engine.nullBackend.Pump(1);
        EXPECT_EQ(completed, row->armedCompletionSequence.load());
        EXPECT_EQ(finalSource ? harken::ArmState::Published : harken::ArmState::Armed, row->state.load());
        EXPECT_EQ(finalSource, row->finalSourcePublicationConsumed);
        ASSERT_NO_FATAL_FAILURE(RetireSource(engine, body, false));
        ASSERT_NO_FATAL_FAILURE(RecycleExactRetirement(engine, body));
        ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
    }
}

TEST(OriginalSourceSubmission, FinalSourceCancelAndCorruptionNeverPublish) {
    for (const bool invalid : {false, true}) {
        SCOPED_TRACE(invalid);
        harkentest::EngineSession session(SourceBodyConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        const std::vector<float> samples(kRate, 0.25f);
        SourceBody body;
        ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
        ASSERT_NO_FATAL_FAILURE(PublishSourceBody(engine, body, true, false, nullptr, true));
        if (invalid) {
            harken::CommandRecord corrupt{};
            ASSERT_TRUE(engine.commands.PeekAt(1, corrupt));
            corrupt.flags = 0x80;
            harken::detail::CommandQueueSubmissionTestAccess::OverwriteAt(engine.commands, 1, corrupt);
        } else ASSERT_TRUE(engine.timelineArms.RequestCancel(body.arm));
        ASSERT_NO_FATAL_FAILURE(CheckCompletedDisposition(engine, body, true, invalid ? 1.0f : 0.5f));
        const auto* row = engine.timelineArms.RowAt(body.arm.slot);
        EXPECT_EQ(harken::ArmState::Tombstoned, row->state.load());
        EXPECT_TRUE(row->finalSourcePublicationIssued);
        EXPECT_FALSE(row->finalSourcePublicationConsumed);
        ASSERT_NO_FATAL_FAILURE(RecycleExactRetirement(engine, body));
        ASSERT_NO_FATAL_FAILURE(RetireSource(engine, body, true));
        ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
    }
}

TEST(OriginalSourceSubmission, FinalSourceBudgetDeferralKeepsWholeInstallationPending) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    engine.mixer.SetMaxCommandsPerQuantumForTest(2);
    harken::CommandRecord prior{};
    prior.opcode = harken::kOpcodeSetVoiceVolume;
    prior.payload.setVoiceVolume.handle = body.independent;
    prior.payload.setVoiceVolume.volumeDb = 0.0f;
    ASSERT_TRUE(engine.commands.TryPush(prior));
    ASSERT_NO_FATAL_FAILURE(PublishSourceBody(engine, body, false, false, nullptr, true));
    const auto* row = engine.timelineArms.RowAt(body.arm.slot);
    const auto before = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    EXPECT_EQ(harken::ArmState::PendingApply, row->state.load());
    EXPECT_EQ(0u, row->armedCompletionSequence.load());
    EXPECT_FALSE(row->finalSourcePublicationConsumed);
    EXPECT_NEAR(body.baseline, CurrentPeak(engine), 0.00001f);
    ASSERT_NO_FATAL_FAILURE(CheckCompletedDisposition(engine, body, false, 0.5f));
    EXPECT_EQ(harken::ArmState::Published, row->state.load());
    ASSERT_NO_FATAL_FAILURE(RetireSource(engine, body, false));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
}

TEST(OriginalSourceSubmission, FinalSourcePhysicalReleaseDeferralNeverPublishesOrAcknowledges) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    ASSERT_NO_FATAL_FAILURE(PublishSourceBody(engine, body, false, false, nullptr, true));
    ASSERT_TRUE(engine.timelineArms.RequestCancel(body.arm));
    struct Held {
        harken::Engine& engine;
        const SourceBody& body;
        std::uint64_t tail;
        std::uint32_t calls = 0;
    } held{engine, body, engine.commands.ConsumedCount()};
    harken::detail::VoicePoolReleaseTestAccess::SetReleaseHeldHook(engine.voices, [](void* opaque) {
        auto& context = *static_cast<Held*>(opaque);
        ++context.calls;
        const auto before = context.engine.mixer.BlocksRendered();
        for (std::uint32_t i = 0; i < 2; ++i) {
            context.engine.nullBackend.Pump(1);
            EXPECT_EQ(before + i + 1u, context.engine.mixer.BlocksRendered());
            EXPECT_EQ(context.tail, context.engine.commands.ConsumedCount());
            EXPECT_FALSE(context.engine.voices.ReleaseObserved(context.body.release));
            const auto* row = context.engine.timelineArms.RowAt(context.body.arm.slot);
            EXPECT_EQ(0u, row->armedCompletionSequence.load());
            EXPECT_EQ(0u, row->tombstoneAudioSequence.load());
            EXPECT_FALSE(row->finalSourcePublicationConsumed);
            EXPECT_NE(harken::ArmState::Published, row->state.load());
        }
    }, &held);
    ASSERT_TRUE(engine.voices.AbortClaimedVoice(body.source));
    ASSERT_EQ(1u, held.calls);
    ASSERT_NO_FATAL_FAILURE(CheckCompletedDisposition(engine, body, true, 0.5f));
    EXPECT_FALSE(engine.timelineArms.RowAt(body.arm.slot)->finalSourcePublicationConsumed);
    ASSERT_NO_FATAL_FAILURE(RetireSource(engine, body, true));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
}

TEST(OriginalSourceSubmission, FinalSourceReuseIssuesOnlyForTheNewActualArmGeneration) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    harken::TimelineArmToken previous{};
    for (const bool finalSource : {true, false, true}) {
        SCOPED_TRACE(finalSource);
        ASSERT_NO_FATAL_FAILURE(PublishSourceBody(engine, body, false, false, nullptr, finalSource));
        if (previous.Valid()) {
            EXPECT_EQ(previous.slot, body.arm.slot);
            EXPECT_LT(previous.generation, body.arm.generation);
            harken::TimelineArmObservation stale{};
            EXPECT_FALSE(engine.timelineArms.Observe(previous, &stale));
        }
        const auto* row = engine.timelineArms.RowAt(body.arm.slot);
        EXPECT_EQ(finalSource, row->finalSourcePublicationIssued);
        EXPECT_FALSE(row->finalSourcePublicationConsumed);
        engine.nullBackend.Pump(1);
        EXPECT_EQ(finalSource ? harken::ArmState::Published : harken::ArmState::Armed, row->state.load());
        EXPECT_EQ(finalSource, row->finalSourcePublicationConsumed);
        previous = body.arm;
        ASSERT_NO_FATAL_FAILURE(RetireSource(engine, body, false));
    }
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
}

TEST(OriginalSourceSubmission, FinalSourcePartialMetadataWaitsForEffectRetirementBeforePublishing) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    VnGainDesc gain{};
    gain.structSize = sizeof(gain);
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(body.independentPublic, 0, VN_EFFECT_GAIN,
        reinterpret_cast<const VnEffectDesc*>(&gain)));
    engine.nullBackend.Pump(1);
    ASSERT_NE(nullptr, engine.voices.Resolve(body.independent));
    const auto oldEffect = engine.voices.Resolve(body.independent)->fxSlotEffectRef[0];
    ASSERT_EQ(VN_EFFECT_GAIN, oldEffect.type);
    Registry::OriginalSourceLifetimePin original;
    ASSERT_NO_FATAL_FAILURE(PublishSourceBody(engine, body, true, true, &original, true));
    const auto tail = engine.commands.ConsumedCount();
    // Fault-inject retire-channel saturation. Placeholders own no effect instance.
    const harken::EffectInstanceRef placeholder{};
    std::uint32_t filled = 0;
    while (engine.effectBank.RetireQueue().TryPush(placeholder)) ++filled;
    ASSERT_GT(filled, 0u);
    const auto before = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    EXPECT_EQ(tail + 2u, engine.commands.ConsumedCount());
    const auto* row = engine.timelineArms.RowAt(body.arm.slot);
    EXPECT_EQ(harken::ArmState::PendingApply, row->state.load());
    EXPECT_EQ(0u, row->armedCompletionSequence.load());
    EXPECT_FALSE(row->finalSourcePublicationConsumed);
    EXPECT_EQ(Registry::Phase::CleanupPending, engine.originalSourceSubmissions.Find(body.arm)->State());
    EXPECT_FALSE(engine.originalSourceSubmissions.IsOriginalInstalledSourceCurrent(original, body.source));
    EXPECT_NEAR(body.baseline * 0.5f, CurrentPeak(engine), 0.00001f);
    harken::EffectInstanceRef returned{};
    ASSERT_TRUE(engine.effectBank.RetireQueue().TryPop(returned));
    ASSERT_TRUE(Registry::SameEffect(placeholder, returned));
    engine.nullBackend.Pump(1);
    EXPECT_EQ(body.acceptedHead, engine.commands.ConsumedCount());
    EXPECT_EQ(harken::ArmState::Published, row->state.load());
    EXPECT_EQ(before + 1u, row->armedCompletionSequence.load());
    EXPECT_TRUE(row->finalSourcePublicationConsumed);
    EXPECT_TRUE(engine.originalSourceSubmissions.IsOriginalInstalledSourceCurrent(original, body.source));
    EXPECT_TRUE(Registry::SameEffect(body.incoming, engine.voices.Resolve(body.independent)->fxSlotEffectRef[0]));
    std::uint32_t placeholders = 0, retired = 0;
    while (engine.effectBank.RetireQueue().TryPop(returned)) {
        if (Registry::SameEffect(placeholder, returned)) ++placeholders;
        else {
            EXPECT_TRUE(Registry::SameEffect(oldEffect, returned));
            ++retired;
            engine.effectBank.Recycle(returned);
        }
    }
    EXPECT_EQ(filled - 1u, placeholders);
    EXPECT_EQ(1u, retired);
    original.Release();
    ASSERT_NO_FATAL_FAILURE(RetireSource(engine, body, false));
    ASSERT_EQ(VN_OK, Vn_Stop(body.independentPublic, 0));
    engine.nullBackend.Pump(1);
    ASSERT_NO_FATAL_FAILURE(RecycleExactRetirement(engine, body));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
}

TEST(OriginalSourceSubmission, MalformedBodyAppliesNoPrefixAndRetiresExactIncomingEffectOnce) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    ASSERT_NO_FATAL_FAILURE(PublishSourceBody(engine, body, true, false));
    harken::CommandRecord corrupted{};
    ASSERT_TRUE(engine.commands.PeekAt(1, corrupted));
    corrupted.flags = 0x80;
    harken::detail::CommandQueueSubmissionTestAccess::OverwriteAt(engine.commands, 1, corrupted);
    const auto violations = engine.mixer.CommandProtocolViolationCount();
    ASSERT_NO_FATAL_FAILURE(CheckCompletedDisposition(engine, body, true, 1.0f));
    EXPECT_EQ(violations + 1u, engine.mixer.CommandProtocolViolationCount());
    ASSERT_NO_FATAL_FAILURE(RecycleExactRetirement(engine, body));
    ASSERT_NO_FATAL_FAILURE(RetireSource(engine, body, true));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
}

TEST(OriginalSourceSubmission, CancelTransfersCrossTargetEffectUntilActualTargetRelease) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    ASSERT_NO_FATAL_FAILURE(PublishSourceBody(engine, body, true, true));
    ASSERT_TRUE(engine.timelineArms.RequestCancel(body.arm));
    ASSERT_NO_FATAL_FAILURE(CheckCompletedDisposition(engine, body, true, 0.5f));
    auto* target = engine.voices.Resolve(body.independent);
    ASSERT_NE(nullptr, target);
    EXPECT_TRUE(Registry::SameEffect(body.incoming, target->fxSlotEffectRef[0]));
    EXPECT_EQ(1u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
    harken::EffectInstanceRef premature{};
    EXPECT_FALSE(engine.effectBank.RetireQueue().TryPop(premature));
    ASSERT_NO_FATAL_FAILURE(RetireSource(engine, body, true));
    ASSERT_EQ(VN_OK, Vn_Stop(body.independentPublic, 0));
    const auto before = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    EXPECT_EQ(nullptr, engine.voices.Resolve(body.independent));
    ASSERT_NO_FATAL_FAILURE(RecycleExactRetirement(engine, body));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
}


TEST(OriginalSourceSubmission, NativeOriginalLoanRetainsInstalledBodyWhilePhysicalVoiceEndsAndReuses) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    Registry foreign(engine.timelineArms, engine.voices, engine.commands, engine.effectBank);
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    Registry::OriginalSourceLifetimePin original, loan;
    ASSERT_NO_FATAL_FAILURE(PublishSourceBody(engine, body, false, false, &original));
    ASSERT_TRUE(original.Valid());
    ASSERT_TRUE(original.Borrow(&loan));
    EXPECT_FALSE(original.Borrow(&loan));
    EXPECT_FALSE(original.Borrow(&original));
    EXPECT_FALSE(original.Borrow(nullptr));
    EXPECT_FALSE(foreign.IsOriginalSourceLifetimePinFor(original, body.arm));
    auto staleArm = body.arm;
    ++staleArm.renderGeneration;
    EXPECT_FALSE(engine.originalSourceSubmissions.IsOriginalSourceLifetimePinFor(original, staleArm));
    EXPECT_FALSE(engine.originalSourceSubmissions.IsOriginalInstalledSourceCurrent(original, body.source));
    original.Release();
    EXPECT_FALSE(original.Valid());
    EXPECT_FALSE(original.Borrow(&original));
    const auto installedBlock = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(installedBlock + 1u, engine.mixer.BlocksRendered());
    EXPECT_EQ(body.acceptedHead, engine.commands.ConsumedCount());
    const auto* native = engine.originalSourceSubmissions.Find(body.arm);
    ASSERT_NE(nullptr, native);
    EXPECT_EQ(Registry::Phase::Disposed, native->State());
    EXPECT_TRUE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
    EXPECT_EQ(installedBlock + 1u, engine.timelineArms.RowAt(body.arm.slot)->armedCompletionSequence.load());
    EXPECT_EQ(0u, engine.timelineArms.RowAt(body.arm.slot)->startedBlockSequence.load());
    EXPECT_FALSE(engine.voices.ReleaseObserved(body.release));
    EXPECT_TRUE(engine.originalSourceSubmissions.IsOriginalInstalledSourceCurrent(loan, body.source));
    EXPECT_FALSE(engine.originalSourceSubmissions.IsOriginalInstalledSourceCurrent(loan, body.independent));
    EXPECT_NEAR(body.baseline * 0.5f, CurrentPeak(engine), 0.00001f);
    Registry::OriginalSourceLifetimePin descendant;
    ASSERT_TRUE(loan.Borrow(&descendant));
    Registry::OriginalSourceLifetimePin moved(std::move(loan));
    EXPECT_FALSE(loan.Valid());
    loan = std::move(moved);
    EXPECT_FALSE(moved.Valid());
    ASSERT_TRUE(engine.timelineArms.RequestCancel(body.arm));
    harken::CommandRecord stop{};
    stop.opcode = harken::kOpcodeStop;
    stop.payload.stop.handle = body.source;
    ASSERT_TRUE(engine.commands.TryPush(stop));
    const auto stopBlock = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(stopBlock + 1u, engine.mixer.BlocksRendered());
    ASSERT_TRUE(engine.voices.ReleaseObserved(body.release));
    EXPECT_EQ(nullptr, engine.voices.Resolve(body.source));
    EXPECT_FALSE(engine.originalSourceSubmissions.IsOriginalInstalledSourceCurrent(loan, body.source));
    EXPECT_TRUE(engine.originalSourceSubmissions.IsOriginalSourceLifetimePinFor(loan, body.arm));
    harken::TimelineArmObservation terminal{};
    ASSERT_TRUE(engine.timelineArms.Observe(body.arm, &terminal));
    EXPECT_EQ(harken::ArmState::Tombstoned, terminal.state);
    EXPECT_EQ(stopBlock, terminal.tombstoneAudioSequence);
    EXPECT_FALSE(engine.timelineArms.Reclaim(body.arm, engine.mixer.BlocksRendered()));
    const auto pcm = engine.registry.Lookup(kSound);
    ASSERT_TRUE(pcm.Valid());
    VnVoiceHandle replacement = 0;
    ASSERT_EQ(VN_OK, engine.voices.Play(pcm, 0.0f, 1.0, 0, false, 0, 0, 0,
        harken::ResolvePanGains(engine.config.mixChannelLayout, 0.0f),
        0, pcm.frames, 1, harken::ResampleProfile::Professional44148,
        &replacement, 1, static_cast<std::uint32_t>(body.source), 0, true));
    EXPECT_EQ(static_cast<std::uint32_t>(body.source), static_cast<std::uint32_t>(replacement));
    EXPECT_NE(body.source, replacement);
    EXPECT_FALSE(engine.originalSourceSubmissions.IsOriginalInstalledSourceCurrent(loan, replacement));
    EXPECT_FALSE(engine.originalSourceSubmissions.IsOriginalInstalledSourceCurrent(loan, body.source));
    ASSERT_TRUE(engine.voices.CancelPreparedVoice(replacement));
    loan.Release();
    EXPECT_TRUE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
    EXPECT_FALSE(engine.timelineArms.Reclaim(body.arm, engine.mixer.BlocksRendered()));
    descendant.Release();
    EXPECT_FALSE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
    EXPECT_FALSE(engine.timelineArms.Reclaim(body.arm, stopBlock));
    ASSERT_TRUE(engine.timelineArms.Reclaim(body.arm, engine.mixer.BlocksRendered()));
    EXPECT_EQ(body.armFreeBefore, engine.timelineArms.FreeCount());
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
}

TEST(OriginalSourceSubmission, NativeOriginalMoveOverwriteAndBothDispositionOrdersReturnActualOwners) {
    for (const bool returnBeforeDisposition : {false, true}) {
        SCOPED_TRACE(returnBeforeDisposition);
        harkentest::EngineSession session(SourceBodyConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        const std::vector<float> samples(kRate, 0.25f);
        SourceBody first;
        ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, first, samples));
        SourceBody second;
        second.independent = first.independent;
        second.independentPublic = first.independentPublic;
        second.independentRelease = first.independentRelease;
        second.baseline = first.baseline;
        Registry::OriginalSourceLifetimePin retained;
        {
            Registry::OriginalSourceLifetimePin firstOriginal, secondOriginal;
            ASSERT_NO_FATAL_FAILURE(PublishSourceBody(engine, first, false, false, &firstOriginal));
            ASSERT_NO_FATAL_FAILURE(PublishSourceBody(engine, second, false, false, &secondOriginal));
            ASSERT_TRUE(firstOriginal.Borrow(&retained));
            if (!returnBeforeDisposition) {
                const auto before = engine.mixer.BlocksRendered();
                engine.nullBackend.Pump(1);
                ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
                EXPECT_TRUE(engine.originalSourceSubmissions.IsOriginalInstalledSourceCurrent(firstOriginal, first.source));
                EXPECT_TRUE(engine.originalSourceSubmissions.IsOriginalInstalledSourceCurrent(secondOriginal, second.source));
            }
            firstOriginal = std::move(secondOriginal);
            EXPECT_FALSE(secondOriginal.Valid());
            EXPECT_FALSE(engine.originalSourceSubmissions.IsOriginalSourceLifetimePinFor(firstOriginal, first.arm));
            EXPECT_TRUE(engine.originalSourceSubmissions.IsOriginalSourceLifetimePinFor(firstOriginal, second.arm));
            EXPECT_TRUE(engine.originalSourceSubmissions.IsOriginalSourceLifetimePinFor(retained, first.arm));
            if (returnBeforeDisposition) retained.Release();
        }
        if (returnBeforeDisposition) {
            const auto before = engine.mixer.BlocksRendered();
            engine.nullBackend.Pump(1);
            ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
        }
        EXPECT_EQ(second.acceptedHead, engine.commands.ConsumedCount());
        EXPECT_FALSE(engine.timelineArms.RowAt(second.arm.slot)->originalLifetimePinned.load());
        EXPECT_EQ(!returnBeforeDisposition, engine.timelineArms.RowAt(first.arm.slot)->originalLifetimePinned.load());
        retained.Release();
        EXPECT_FALSE(engine.timelineArms.RowAt(first.arm.slot)->originalLifetimePinned.load());
        // Each exact tail accounts for the other still-live Arm reservation.
        auto firstTail = first;
        firstTail.armFreeBefore = second.armFreeBefore;
        auto secondTail = second;
        secondTail.armFreeBefore = first.armFreeBefore;
        ASSERT_NO_FATAL_FAILURE(RetireSource(engine, firstTail, false));
        ASSERT_NO_FATAL_FAILURE(RetireSource(engine, secondTail, false));
        EXPECT_EQ(first.armFreeBefore, engine.timelineArms.FreeCount());
        ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, first));
    }
}

TEST(OriginalSourceSubmission, NativeOriginalCancellationAndInvalidBodyRetainOriginWithoutGrantingInstall) {
    for (const bool invalid : {false, true}) {
        SCOPED_TRACE(invalid);
        harkentest::EngineSession session(SourceBodyConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        const std::vector<float> samples(kRate, 0.25f);
        SourceBody body;
        ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
        Registry::OriginalSourceLifetimePin original, loan;
        ASSERT_NO_FATAL_FAILURE(PublishSourceBody(engine, body, false, false, &original));
        ASSERT_TRUE(original.Borrow(&loan));
        if (invalid) {
            harken::CommandRecord corrupt{};
            ASSERT_TRUE(engine.commands.PeekAt(1, corrupt));
            corrupt.flags = 0x80;
            harken::detail::CommandQueueSubmissionTestAccess::OverwriteAt(engine.commands, 1, corrupt);
        } else ASSERT_TRUE(engine.timelineArms.RequestCancel(body.arm));
        const auto before = engine.mixer.BlocksRendered();
        engine.nullBackend.Pump(1);
        ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
        EXPECT_EQ(body.acceptedHead, engine.commands.ConsumedCount());
        EXPECT_EQ(Registry::Phase::Disposed, engine.originalSourceSubmissions.Find(body.arm)->State());
        ASSERT_TRUE(engine.voices.ReleaseObserved(body.release));
        EXPECT_EQ(nullptr, engine.voices.Resolve(body.source));
        EXPECT_TRUE(engine.originalSourceSubmissions.IsOriginalSourceLifetimePinFor(original, body.arm));
        EXPECT_FALSE(engine.originalSourceSubmissions.IsOriginalInstalledSourceCurrent(original, body.source));
        EXPECT_TRUE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
        EXPECT_NEAR(body.baseline * (invalid ? 1.0f : 0.5f), CurrentPeak(engine), 0.00001f);
        EXPECT_FALSE(engine.timelineArms.Reclaim(body.arm, engine.mixer.BlocksRendered()));
        original.Release();
        EXPECT_TRUE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
        loan.Release();
        EXPECT_FALSE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
        ASSERT_NO_FATAL_FAILURE(RetireSource(engine, body, true));
        ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
    }
}


TEST(OriginalSourceSubmission, NativeOriginalQuarantineStalesInstallationWithoutForgingRelease) {
    // This invokes the real native fault exit, not a worker-timeout reproduction.
    const std::vector<float> samples(kRate, 0.25f);
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    Registry::OriginalSourceLifetimePin original, loan;
    ASSERT_NO_FATAL_FAILURE(PublishSourceBody(engine, body, false, false, &original));
    ASSERT_TRUE(original.Borrow(&loan));
    const auto before = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    ASSERT_NE(nullptr, engine.voices.Resolve(body.source));
    ASSERT_TRUE(engine.originalSourceSubmissions.IsOriginalInstalledSourceCurrent(original, body.source));
    EXPECT_EQ(body.acceptedHead, engine.commands.ConsumedCount());
    EXPECT_EQ(Registry::Phase::Disposed, engine.originalSourceSubmissions.Find(body.arm)->State());
    const auto pcmUses = engine.registry.Lookup(kSound).useCount->load();
    ASSERT_EQ(2u, pcmUses);
    harken::detail::VoicePoolReleaseTestAccess::QuarantineOriginalSlot(engine.voices,
        engine.notifications, static_cast<std::uint32_t>(body.source), engine.mixer.BlocksRendered());
    EXPECT_EQ(nullptr, engine.voices.Resolve(body.source));
    EXPECT_NE(static_cast<std::uint32_t>(body.source >> 32),
        harken::detail::VoicePoolReleaseTestAccess::OriginalSlotGeneration(engine.voices,
            static_cast<std::uint32_t>(body.source)));
    EXPECT_FALSE(engine.voices.ReleaseObserved(body.release));
    EXPECT_FALSE(engine.originalSourceSubmissions.IsOriginalInstalledSourceCurrent(original, body.source));
    EXPECT_TRUE(engine.originalSourceSubmissions.IsOriginalSourceLifetimePinFor(original, body.arm));
    EXPECT_TRUE(engine.originalSourceSubmissions.IsOriginalSourceLifetimePinFor(loan, body.arm));
    EXPECT_TRUE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
    EXPECT_EQ(pcmUses, engine.registry.Lookup(kSound).useCount->load());
    EXPECT_EQ(2u, engine.voices.ActiveCount());
    original.Release();
    EXPECT_TRUE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
    loan.Release();
    EXPECT_FALSE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
    EXPECT_FALSE(engine.voices.ReleaseObserved(body.release));
    EXPECT_EQ(pcmUses, engine.registry.Lookup(kSound).useCount->load());
    ASSERT_EQ(VN_OK, Vn_Stop(body.independentPublic, 0));
    const auto stopBlock = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(stopBlock + 1u, engine.mixer.BlocksRendered());
    ASSERT_TRUE(engine.voices.ReleaseObserved(body.independentRelease));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, engine.ledger.ToRender(body.independentPublic));
    // Quarantine deliberately retains the physical writer's resources until join.
    EXPECT_EQ(1u, engine.voices.ActiveCount());
    EXPECT_EQ(1u, engine.registry.Lookup(kSound).useCount->load());
    EXPECT_FALSE(engine.voices.ReleaseObserved(body.release));
}


namespace {
using BodyPin = harken::VoicePool::OriginalSourceBodyLifetimePin;
void PublishOriginalBodyEpisode(harken::Engine& engine, SourceBody& body,
    bool withEffect, bool crossTargetEffect,
    Registry::OriginalSourceLifetimePin* original,
    harken::VoicePool::OriginalSourceBodyLifetimePin* originalBody,
    bool wrongFrozenSourceIdentity = false,
    const harken::VoicePool::OriginalSourceBodyLifetimePin* occupiedBody = nullptr,
    bool pendingStart = false) {
    const auto pcm = engine.registry.Lookup(kSound);
    ASSERT_TRUE(pcm.Valid());
    ASSERT_EQ(VN_OK, engine.voices.Play(pcm, 0.0f, 1.0, 0, false, 0, 0, 0,
        harken::ResolvePanGains(engine.config.mixChannelLayout, 0.0f),
        0, pcm.frames, 1, harken::ResampleProfile::Professional44148,
        &body.source, 1, UINT32_MAX, 0, true));
    ASSERT_TRUE(engine.voices.CaptureReleaseObserver(body.source, &body.release));
    harken::VoicePool::PreparedCommitInput physical{};
    physical.renderHandle = body.source;
    physical.originalRelease = body.release;
    ASSERT_TRUE(engine.voices.CapturePreparedRoutingV1(body.source, &physical.routing));
    harken::VoicePool::PreparedCommitProof voiceProof;
    ASSERT_TRUE(engine.voices.ProvePreparedCommit(physical, &voiceProof));
    harken::TimelineArmTable::PreparedCommitInput logical{};
    logical.startFrame = engine.mixer.BlocksRendered() * kFrames;
    logical.armDeadline = logical.startFrame;
    logical.pendingImmediateStart = pendingStart;
    if (pendingStart) logical.startFrame = logical.armDeadline = 0;
    logical.target.subjectId = kSound;
    body.armFreeBefore = engine.timelineArms.FreeCount();
    ASSERT_TRUE(pendingStart
        ? engine.timelineArms.PreparePendingImmediate(static_cast<std::uint32_t>(body.source),
            static_cast<std::uint32_t>(body.source >> 32), logical.programGeneration, logical.target, &body.arm)
        : engine.timelineArms.Prepare(static_cast<std::uint32_t>(body.source),
            static_cast<std::uint32_t>(body.source >> 32), logical.startFrame,
            logical.armDeadline, logical.programGeneration, logical.target, &body.arm));
    logical.arm = body.arm;
    harken::TimelineArmTable::PreparedCommitProof armProof;
    ASSERT_TRUE(engine.timelineArms.ProvePreparedCommit(logical, &armProof));
    std::array<harken::EffectPoolBank::PreparedCommitProof, 1> effectProofs;
    const auto effectCount = withEffect ? 1u : 0u;
    if (withEffect) {
        VnGainDesc descriptor{};
        descriptor.structSize = sizeof(descriptor);
        descriptor.gainDb = 0.0f;
        ASSERT_EQ(VN_OK, harken::ValidateEffectDesc(VN_EFFECT_GAIN, &descriptor, kRate));
        body.incoming = engine.effectBank.AcquirePrepared(VN_EFFECT_GAIN);
        ASSERT_EQ(VN_EFFECT_GAIN, body.incoming.type);
        auto* state = engine.effectBank.StateFor(body.incoming.type, body.incoming.poolIndex);
        ASSERT_NE(nullptr, state);
        auto kernel = harken::BuildEffect(body.incoming.type, state);
        ASSERT_NE(nullptr, kernel.reset);
        kernel.reset(state);
        harken::ApplyEffectDesc(VN_EFFECT_GAIN, &descriptor, state);
        ASSERT_TRUE(engine.effectBank.ProvePreparedCommit(body.incoming, {}, &effectProofs[0]));
    }
    std::array<harken::CommandRecord, 3> commands{};
    body.commandCount = 2u + effectCount;
    auto& header = commands[0];
    header.opcode = harken::kOpcodePlay;
    header.payload.play.handle = body.source;
    header.payload.play.sourceIdentity = body.source;
    header.payload.play.batch.batchVersion = harken::kPlayLedBatchVersion;
    header.payload.play.batch.batchKind = harken::PlayLedBatchKind::TimelineArm;
    header.payload.play.batch.batchCommandCount = body.commandCount;
    header.payload.play.batch.armToken =
        (static_cast<std::uint64_t>(body.arm.generation) << 32) | body.arm.slot;
    commands[1].opcode = harken::kOpcodeSetVoiceVolume;
    commands[1].payload.setVoiceVolume.handle = body.independent;
    commands[1].payload.setVoiceVolume.volumeDb = -6.0205999f;
    if (withEffect) {
        commands[2].opcode = harken::kOpcodeSetVoiceEffect;
        auto& payload = commands[2].payload.setVoiceEffect;
        payload.handle = crossTargetEffect ? body.independent : body.source;
        payload.slot = 0;
        payload.effectType = body.incoming.type;
        payload.poolIndex = body.incoming.poolIndex;
        payload.poolGeneration = body.incoming.generation;
    }
    const auto acceptedBefore = engine.commands.PublishedHead();
    const auto consumedBefore = engine.commands.ConsumedCount();
    const auto ownersBefore = harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices);
    const auto spans = std::span(effectProofs.data(), effectCount);
    if (occupiedBody != nullptr) {
        harken::VoicePool::OriginalSourceBodyLifetimePin occupied;
        ASSERT_TRUE(occupiedBody->Borrow(&occupied));
        EXPECT_FALSE(engine.originalSourceSubmissions.CapturePreparedSource(header,
            armProof, voiceProof, spans, original, &occupied));
        EXPECT_FALSE(engine.originalSourceSubmissions.CapturePreparedSource(header,
            armProof, voiceProof, spans, nullptr, originalBody));
        auto wrong = header;
        wrong.payload.play.handle ^= UINT64_C(1) << 32;
        EXPECT_FALSE(engine.originalSourceSubmissions.CapturePreparedSource(wrong,
            armProof, voiceProof, spans, original, originalBody));
        EXPECT_FALSE(original->Valid());
        EXPECT_FALSE(originalBody->Valid());
        EXPECT_TRUE(armProof.Valid());
        EXPECT_TRUE(voiceProof.Valid());
        EXPECT_EQ(acceptedBefore, engine.commands.PublishedHead());
        EXPECT_EQ(consumedBefore, engine.commands.ConsumedCount());
        EXPECT_EQ(ownersBefore,
            harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
        EXPECT_FALSE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
        occupied.Release();
    }
    ASSERT_TRUE(engine.originalSourceSubmissions.CapturePreparedSource(header,
        armProof, voiceProof, spans, original, originalBody));
    ASSERT_TRUE(original->Valid());
    ASSERT_TRUE(originalBody->Valid());
    EXPECT_EQ(acceptedBefore, engine.commands.PublishedHead());
    EXPECT_EQ(consumedBefore, engine.commands.ConsumedCount());
    EXPECT_TRUE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
    EXPECT_TRUE(engine.voices.IsOriginalSourceBodyLifetimePinFor(*originalBody, body.arm, body.source));
    EXPECT_FALSE(engine.voices.IsOriginalSourceBodyCurrent(*originalBody));
    // This is a genuine second native batch identity, not wire corruption.
    if (wrongFrozenSourceIdentity) header.payload.play.sourceIdentity = body.independent;
    harken::CommandQueue::BatchReservation reservation{};
    ASSERT_TRUE(engine.commands.ReserveBatch(body.commandCount, &reservation));
    harken::CommandQueue::PreparedBatchProof batch;
    ASSERT_TRUE(engine.commands.ProvePreparedBatch(&reservation, commands.data(), body.commandCount, &batch));
    ASSERT_TRUE(engine.originalSourceSubmissions.BindPreparedChild(batch, 0, body.commandCount));
    if (withEffect) std::move(effectProofs[0]).Consume();
    harken::VoicePool::ConsumePreparedCommit(std::move(voiceProof));
    harken::TimelineArmTable::ConsumePreparedCommit(std::move(armProof));
    engine.originalSourceSubmissions.PublishPreparedChild(header);
    (void)engine.commands.ConsumePreparedBatch(std::move(batch));
    if (withEffect) engine.effectBank.PublishInstalled(body.incoming);
    body.acceptedHead = engine.commands.PublishedHead();
    ASSERT_EQ(Registry::Phase::Published, engine.originalSourceSubmissions.Find(body.arm)->State());
}

void StopOriginalBodyEpisode(harken::Engine& engine, const SourceBody& body) {
    ASSERT_TRUE(engine.timelineArms.RequestCancel(body.arm));
    harken::CommandRecord stop{};
    stop.opcode = harken::kOpcodeStop;
    stop.payload.stop.handle = body.source;
    ASSERT_TRUE(engine.commands.TryPush(stop));
    const auto before = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    ASSERT_TRUE(engine.voices.ReleaseObserved(body.release));
    EXPECT_EQ(nullptr, engine.voices.Resolve(body.source));
    harken::TimelineArmObservation ended{};
    ASSERT_TRUE(engine.timelineArms.Observe(body.arm, &ended));
    EXPECT_EQ(harken::ArmState::Tombstoned, ended.state);
    EXPECT_EQ(before, ended.tombstoneAudioSequence);
}
void ReclaimOriginalBodyArm(harken::Engine& engine, const SourceBody& body) {
    harken::TimelineArmObservation ended{};
    ASSERT_TRUE(engine.timelineArms.Observe(body.arm, &ended));
    ASSERT_EQ(harken::ArmState::Tombstoned, ended.state);
    EXPECT_FALSE(engine.timelineArms.Reclaim(body.arm, ended.tombstoneAudioSequence));
    if (engine.mixer.BlocksRendered() == ended.tombstoneAudioSequence) {
        const auto before = engine.mixer.BlocksRendered();
        engine.nullBackend.Pump(1);
        ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    }
    ASSERT_GT(engine.mixer.BlocksRendered(), ended.tombstoneAudioSequence);
    ASSERT_TRUE(engine.timelineArms.Reclaim(body.arm, engine.mixer.BlocksRendered()));
    EXPECT_FALSE(engine.timelineArms.Observe(body.arm, &ended));
}
VnResult PrepareAtOriginalBodySlot(harken::Engine& engine, const SourceBody& body,
    VnVoiceHandle* out) {
    const auto pcm = engine.registry.Lookup(kSound);
    return engine.voices.Play(pcm, 0.0f, 1.0, 0, false, 0, 0, 0,
        harken::ResolvePanGains(engine.config.mixChannelLayout, 0.0f),
        0, pcm.frames, 1, harken::ResampleProfile::Professional44148,
        out, 1, static_cast<std::uint32_t>(body.source), 0, true);
}
void PumpOriginalBodyInstall(harken::Engine& engine, const SourceBody& body) {
    const auto before = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    EXPECT_EQ(body.acceptedHead, engine.commands.ConsumedCount());
    ASSERT_NE(nullptr, engine.originalSourceSubmissions.Find(body.arm));
    EXPECT_EQ(Registry::Phase::Disposed, engine.originalSourceSubmissions.Find(body.arm)->State());
    EXPECT_EQ(before + 1u, engine.timelineArms.RowAt(body.arm.slot)->armedCompletionSequence.load());
    EXPECT_EQ(0u, engine.timelineArms.RowAt(body.arm.slot)->startedBlockSequence.load());
    EXPECT_FALSE(engine.voices.ReleaseObserved(body.release));
    EXPECT_NEAR(body.baseline * 0.5f, CurrentPeak(engine), 0.00001f);
}
} // namespace

TEST(OriginalSourceSubmission, GenuineBodyFactoryInstallsThenEndsWhileForeignRetainsSameSlotEpisode) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    Registry::OriginalSourceLifetimePin source;
    BodyPin original, foreign;
    ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, body, false, false, &source, &original));
    ASSERT_TRUE(original.Borrow(&foreign));
    EXPECT_FALSE(original.Borrow(&foreign));
    EXPECT_FALSE(original.Borrow(&original));
    EXPECT_FALSE(original.Borrow(nullptr));
    auto bad = body.arm;
    ++bad.renderGeneration;
    EXPECT_FALSE(engine.voices.IsOriginalSourceBodyLifetimePinFor(original, bad, body.source));
    EXPECT_FALSE(engine.voices.IsOriginalSourceBodyLifetimePinFor(original, body.arm, body.independent));
    harken::VoicePool foreignReceiver(engine.config, &engine.tables, &engine.multirateTables,
        &engine.biquadTrig, engine.effectBank, engine.dynamicRoutes, &engine.timelineArms);
    EXPECT_FALSE(foreignReceiver.IsOriginalSourceBodyLifetimePinFor(original, body.arm, body.source));
    EXPECT_FALSE(foreignReceiver.IsOriginalSourceBodyCurrent(original));
    BodyPin moved(std::move(original));
    EXPECT_FALSE(original.Valid());
    original = std::move(moved);
    EXPECT_FALSE(moved.Valid());
    source.Release();
    original.Release();
    EXPECT_TRUE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
    ASSERT_NO_FATAL_FAILURE(PumpOriginalBodyInstall(engine, body));
    EXPECT_TRUE(engine.voices.IsOriginalSourceBodyCurrent(foreign));
    ASSERT_NO_FATAL_FAILURE(StopOriginalBodyEpisode(engine, body));
    EXPECT_FALSE(engine.voices.IsOriginalSourceBodyCurrent(foreign));
    EXPECT_TRUE(engine.voices.IsOriginalSourceBodyLifetimePinFor(foreign, body.arm, body.source));
    EXPECT_FALSE(engine.timelineArms.Reclaim(body.arm, engine.mixer.BlocksRendered()));
    const auto freeBeforeReturn = engine.voices.AvailableSlotCount();
    std::uint32_t generation = 0;
    EXPECT_FALSE(engine.voices.FreeSlotGeneration(static_cast<std::uint32_t>(body.source), &generation));
    VnVoiceHandle replacement = 0;
    EXPECT_EQ(VN_NO_VOICE, PrepareAtOriginalBodySlot(engine, body, &replacement));
    EXPECT_EQ(0u, replacement);
    EXPECT_EQ(1u, engine.registry.Lookup(kSound).useCount->load());
    EXPECT_TRUE(engine.voices.HasFreeSlot());
    EXPECT_EQ(VN_OK, engine.voices.CheckCapacityForPlay());
    // Automatic selection must choose a different usable slot while this old one is held.
    const auto pcm = engine.registry.Lookup(kSound);
    VnVoiceHandle other = 0;
    ASSERT_EQ(VN_OK, engine.voices.Play(pcm, 0.0f, 1.0, 0, false, 0, 0, 0,
        harken::ResolvePanGains(engine.config.mixChannelLayout, 0.0f),
        0, pcm.frames, 1, harken::ResampleProfile::Professional44148, &other, 1, UINT32_MAX, 0, true));
    EXPECT_NE(static_cast<std::uint32_t>(body.source), static_cast<std::uint32_t>(other));
    ASSERT_TRUE(engine.voices.CancelPreparedVoice(other));
    foreign.Release();
    EXPECT_FALSE(foreign.Valid());
    EXPECT_FALSE(foreign.Borrow(&original));
    EXPECT_FALSE(engine.voices.IsOriginalSourceBodyCurrent(foreign));
    EXPECT_FALSE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
    EXPECT_EQ(freeBeforeReturn + 1u, engine.voices.AvailableSlotCount());
    EXPECT_TRUE(engine.voices.FreeSlotGeneration(static_cast<std::uint32_t>(body.source), &generation));
    ASSERT_NO_FATAL_FAILURE(ReclaimOriginalBodyArm(engine, body));
    ASSERT_EQ(VN_OK, PrepareAtOriginalBodySlot(engine, body, &replacement));
    EXPECT_NE(body.source, replacement);
    ASSERT_TRUE(engine.voices.CancelPreparedVoice(replacement));
    EXPECT_EQ(0u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
}

TEST(OriginalSourceSubmission, GenuineBodyAllExternalReturnsKeepActualPendingAndInstalledSelfLoanUntilEnd) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    Registry::OriginalSourceLifetimePin source;
    BodyPin original;
    ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, body, false, false, &source, &original));
    original.Release();
    source.Release();
    ASSERT_NO_FATAL_FAILURE(PumpOriginalBodyInstall(engine, body));
    EXPECT_TRUE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
    EXPECT_EQ(1u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
    ASSERT_NO_FATAL_FAILURE(StopOriginalBodyEpisode(engine, body));
    EXPECT_FALSE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
    EXPECT_EQ(0u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
    ASSERT_NO_FATAL_FAILURE(ReclaimOriginalBodyArm(engine, body));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
}

TEST(OriginalSourceSubmission, GenuineBodyEarlySourceAThenFrozenSourceBRefusesBeforeBeginAndRetires) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    Registry::OriginalSourceLifetimePin source;
    BodyPin original, foreign;
    ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, body, false, false,
        &source, &original, true));
    ASSERT_TRUE(original.Borrow(&foreign));
    const auto before = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    EXPECT_EQ(body.acceptedHead, engine.commands.ConsumedCount());
    EXPECT_EQ(Registry::Phase::Disposed, engine.originalSourceSubmissions.Find(body.arm)->State());
    EXPECT_TRUE(engine.voices.ReleaseObserved(body.release));
    EXPECT_EQ(nullptr, engine.voices.Resolve(body.source));
    EXPECT_FALSE(engine.voices.IsOriginalSourceBodyCurrent(original));
    EXPECT_FALSE(engine.originalSourceSubmissions.IsOriginalInstalledSourceCurrent(source, body.source));
    EXPECT_EQ(0u, engine.timelineArms.RowAt(body.arm.slot)->armedCompletionSequence.load());
    EXPECT_EQ(0u, engine.timelineArms.RowAt(body.arm.slot)->startedBlockSequence.load());
    EXPECT_EQ(before + 1u, engine.timelineArms.RowAt(body.arm.slot)->tombstoneAudioSequence.load());
    // The original independent-target setter is still a valid cancellation metadata tail.
    EXPECT_NEAR(body.baseline * 0.5f, CurrentPeak(engine), 0.00001f);
    source.Release();
    original.Release();
    EXPECT_TRUE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
    VnVoiceHandle replacement = 0;
    EXPECT_EQ(VN_NO_VOICE, PrepareAtOriginalBodySlot(engine, body, &replacement));
    foreign.Release();
    EXPECT_FALSE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
    EXPECT_FALSE(engine.timelineArms.Reclaim(body.arm, engine.mixer.BlocksRendered()));
    const auto strictBefore = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(strictBefore + 1u, engine.mixer.BlocksRendered());
    ASSERT_NO_FATAL_FAILURE(ReclaimOriginalBodyArm(engine, body));
    ASSERT_EQ(VN_OK, PrepareAtOriginalBodySlot(engine, body, &replacement));
    ASSERT_TRUE(engine.voices.CancelPreparedVoice(replacement));
    EXPECT_EQ(0u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
}

TEST(OriginalSourceSubmission, GenuineBodyOccupiedOutputPreflightAndMoveOverwriteRetainExactFamilies) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody first;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, first, samples));
    SourceBody second;
    second.independent = first.independent;
    second.independentPublic = first.independentPublic;
    second.independentRelease = first.independentRelease;
    second.baseline = first.baseline;
    Registry::OriginalSourceLifetimePin firstSource, secondSource;
    BodyPin firstBody, firstForeign, secondBody;
    ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, first, false, false, &firstSource, &firstBody));
    ASSERT_TRUE(firstBody.Borrow(&firstForeign));
    ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, second, false, false,
        &secondSource, &secondBody, false, &firstBody));
    const auto before = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    EXPECT_EQ(second.acceptedHead, engine.commands.ConsumedCount());
    EXPECT_TRUE(engine.voices.IsOriginalSourceBodyCurrent(firstBody));
    EXPECT_TRUE(engine.voices.IsOriginalSourceBodyCurrent(secondBody));
    firstSource.Release();
    secondSource.Release();
    firstBody = std::move(secondBody);
    EXPECT_FALSE(secondBody.Valid());
    EXPECT_TRUE(engine.voices.IsOriginalSourceBodyLifetimePinFor(firstBody, second.arm, second.source));
    EXPECT_FALSE(engine.voices.IsOriginalSourceBodyLifetimePinFor(firstBody, first.arm, first.source));
    EXPECT_TRUE(engine.voices.IsOriginalSourceBodyLifetimePinFor(firstForeign, first.arm, first.source));
    ASSERT_NO_FATAL_FAILURE(StopOriginalBodyEpisode(engine, first));
    ASSERT_NO_FATAL_FAILURE(StopOriginalBodyEpisode(engine, second));
    EXPECT_FALSE(engine.voices.IsOriginalSourceBodyCurrent(firstForeign));
    EXPECT_FALSE(engine.voices.IsOriginalSourceBodyCurrent(firstBody));
    EXPECT_FALSE(engine.timelineArms.Reclaim(first.arm, engine.mixer.BlocksRendered()));
    EXPECT_FALSE(engine.timelineArms.Reclaim(second.arm, engine.mixer.BlocksRendered()));
    firstForeign.Release();
    ASSERT_NO_FATAL_FAILURE(ReclaimOriginalBodyArm(engine, first));
    EXPECT_TRUE(engine.timelineArms.RowAt(second.arm.slot)->originalLifetimePinned.load());
    firstBody.Release();
    ASSERT_NO_FATAL_FAILURE(ReclaimOriginalBodyArm(engine, second));
    EXPECT_EQ(first.armFreeBefore, engine.timelineArms.FreeCount());
    EXPECT_EQ(0u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, first));
}

TEST(OriginalSourceSubmission, GenuineBodyNativeCancellationAndInvalidWireHaveNoInstallAndStrictDispositionTail) {
    for (const bool cancel : {false, true}) {
        SCOPED_TRACE(cancel);
        harkentest::EngineSession session(SourceBodyConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        const std::vector<float> samples(kRate, 0.25f);
        SourceBody body;
        ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
        Registry::OriginalSourceLifetimePin source;
        BodyPin original, foreign;
        ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, body, false, false, &source, &original));
        ASSERT_TRUE(original.Borrow(&foreign));
        if (cancel) ASSERT_TRUE(engine.timelineArms.RequestCancel(body.arm));
        else {
            harken::CommandRecord corrupt{};
            ASSERT_TRUE(engine.commands.PeekAt(1, corrupt));
            corrupt.flags = 0x80;
            harken::detail::CommandQueueSubmissionTestAccess::OverwriteAt(engine.commands, 1, corrupt);
        }
        const auto before = engine.mixer.BlocksRendered();
        engine.nullBackend.Pump(1);
        ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
        EXPECT_EQ(body.acceptedHead, engine.commands.ConsumedCount());
        EXPECT_TRUE(engine.voices.ReleaseObserved(body.release));
        EXPECT_FALSE(engine.voices.IsOriginalSourceBodyCurrent(original));
        EXPECT_EQ(0u, engine.timelineArms.RowAt(body.arm.slot)->armedCompletionSequence.load());
        EXPECT_EQ(before + 1u, engine.timelineArms.RowAt(body.arm.slot)->tombstoneAudioSequence.load());
        EXPECT_NEAR(body.baseline * (cancel ? 0.5f : 1.0f), CurrentPeak(engine), 0.00001f);
        source.Release();
        original.Release();
        EXPECT_TRUE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
        foreign.Release();
        EXPECT_FALSE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
        EXPECT_FALSE(engine.timelineArms.Reclaim(body.arm, engine.mixer.BlocksRendered()));
        const auto strictBefore = engine.mixer.BlocksRendered();
        engine.nullBackend.Pump(1);
        ASSERT_EQ(strictBefore + 1u, engine.mixer.BlocksRendered());
        ASSERT_NO_FATAL_FAILURE(ReclaimOriginalBodyArm(engine, body));
        EXPECT_EQ(0u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
        ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
    }
}

TEST(OriginalSourceSubmission, GenuineBodyQuarantineRetainsSelfLoanUntilActualJoinedDrainWithoutFakeRelease) {
    const std::vector<float> samples(kRate, 0.25f);
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    Registry::OriginalSourceLifetimePin source;
    BodyPin original, foreign;
    ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, body, false, false, &source, &original));
    ASSERT_TRUE(original.Borrow(&foreign));
    ASSERT_NO_FATAL_FAILURE(PumpOriginalBodyInstall(engine, body));
    EXPECT_TRUE(engine.voices.IsOriginalSourceBodyCurrent(original));
    const auto uses = engine.registry.Lookup(kSound).useCount->load();
    harken::detail::VoicePoolReleaseTestAccess::QuarantineOriginalSlot(engine.voices,
        engine.notifications, static_cast<std::uint32_t>(body.source), engine.mixer.BlocksRendered());
    EXPECT_FALSE(engine.voices.IsOriginalSourceBodyCurrent(original));
    EXPECT_FALSE(engine.voices.ReleaseObserved(body.release));
    source.Release();
    original.Release();
    foreign.Release();
    // The self-loan is not completed by Quarantine or by external Return.
    EXPECT_TRUE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
    EXPECT_EQ(1u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
    EXPECT_EQ(uses, engine.registry.Lookup(kSound).useCount->load());
    ASSERT_EQ(VN_OK, Vn_Stop(body.independentPublic, 0));
    const auto before = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    ASSERT_TRUE(engine.voices.ReleaseObserved(body.independentRelease));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(1u, engine.voices.ActiveCount());
    EXPECT_EQ(1u, engine.registry.Lookup(kSound).useCount->load());
    engine.voices.DisableRenderWorkers();
    (void)engine.voices.UnbindRealtimeWorkerDomain();
    engine.voices.ShutdownOriginalSourceBodiesAfterQuiescence();
    EXPECT_FALSE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
    EXPECT_EQ(0u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
    EXPECT_FALSE(engine.voices.ReleaseObserved(body.release));
    EXPECT_EQ(1u, engine.registry.Lookup(kSound).useCount->load());
    EXPECT_EQ(Registry::Phase::Disposed, engine.originalSourceSubmissions.Find(body.arm)->State());
    // Actual drain is idempotent; destructor must not borrow a destroyed registry.
    engine.voices.ShutdownOriginalSourceBodiesAfterQuiescence();
}


TEST(OriginalSourceSubmission, GenuineBodyNeverBoundPreparationUsesBothRealCancelPathsAndReturnsPendingLoan) {
    for (const bool originalCancel : {false, true}) {
        SCOPED_TRACE(originalCancel);
        harkentest::EngineSession session(SourceBodyConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        const std::vector<float> samples(kRate, 0.25f);
        SourceBody body;
        ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
        const auto pcm = engine.registry.Lookup(kSound);
        ASSERT_TRUE(pcm.Valid());
        ASSERT_EQ(VN_OK, engine.voices.Play(pcm, 0.0f, 1.0, 0, false, 0, 0, 0,
            harken::ResolvePanGains(engine.config.mixChannelLayout, 0.0f),
            0, pcm.frames, 1, harken::ResampleProfile::Professional44148,
            &body.source, 1, UINT32_MAX, 0, true));
        ASSERT_TRUE(engine.voices.CaptureReleaseObserver(body.source, &body.release));
        harken::VoicePool::PreparedCommitInput physical{};
        physical.renderHandle = body.source;
        physical.originalRelease = body.release;
        ASSERT_TRUE(engine.voices.CapturePreparedRoutingV1(body.source, &physical.routing));
        harken::VoicePool::PreparedCommitProof voiceProof;
        ASSERT_TRUE(engine.voices.ProvePreparedCommit(physical, &voiceProof));
        harken::TimelineArmTable::PreparedCommitInput logical{};
        logical.startFrame = engine.mixer.BlocksRendered() * kFrames;
        logical.armDeadline = logical.startFrame;
        logical.target.subjectId = kSound;
        body.armFreeBefore = engine.timelineArms.FreeCount();
        ASSERT_TRUE(engine.timelineArms.Prepare(static_cast<std::uint32_t>(body.source),
            static_cast<std::uint32_t>(body.source >> 32), logical.startFrame,
            logical.armDeadline, logical.programGeneration, logical.target, &body.arm));
        logical.arm = body.arm;
        harken::TimelineArmTable::PreparedCommitProof armProof;
        ASSERT_TRUE(engine.timelineArms.ProvePreparedCommit(logical, &armProof));
        harken::CommandRecord header{};
        header.opcode = harken::kOpcodePlay;
        header.payload.play.handle = body.source;
        header.payload.play.sourceIdentity = body.source;
        header.payload.play.batch.batchVersion = harken::kPlayLedBatchVersion;
        header.payload.play.batch.batchKind = harken::PlayLedBatchKind::TimelineArm;
        header.payload.play.batch.batchCommandCount = 1;
        header.payload.play.batch.armToken =
            (static_cast<std::uint64_t>(body.arm.generation) << 32) | body.arm.slot;
        Registry::OriginalSourceLifetimePin source;
        BodyPin original, foreign;
        const auto head = engine.commands.PublishedHead();
        const auto consumed = engine.commands.ConsumedCount();
        ASSERT_TRUE(engine.originalSourceSubmissions.CapturePreparedSource(header,
            armProof, voiceProof, {}, &source, &original));
        ASSERT_TRUE(original.Borrow(&foreign));
        armProof.Release();
        voiceProof.Release();
        EXPECT_FALSE(engine.voices.IsOriginalSourceBodyCurrent(original));
        source.Release();
        original.Release();
        if (originalCancel) ASSERT_TRUE(engine.originalSourceSubmissions.DisposeUnpublishedVoice(body.arm));
        else ASSERT_TRUE(engine.voices.CancelPreparedVoice(body.source));
        ASSERT_TRUE(engine.voices.ReleaseObserved(body.release));
        ASSERT_TRUE(engine.originalSourceSubmissions.WithdrawUnpublished(body.arm));
        EXPECT_EQ(Registry::Phase::Disposed, engine.originalSourceSubmissions.Find(body.arm)->State());
        EXPECT_EQ(head, engine.commands.PublishedHead());
        EXPECT_EQ(consumed, engine.commands.ConsumedCount());
        EXPECT_EQ(0u, engine.timelineArms.RowAt(body.arm.slot)->armedCompletionSequence.load());
        EXPECT_EQ(0u, engine.timelineArms.RowAt(body.arm.slot)->startedBlockSequence.load());
        EXPECT_EQ(0u, engine.timelineArms.RowAt(body.arm.slot)->tombstoneAudioSequence.load());
        EXPECT_TRUE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
        EXPECT_FALSE(engine.timelineArms.CancelPrepared(body.arm));
        VnVoiceHandle replacement = 0;
        EXPECT_EQ(VN_NO_VOICE, PrepareAtOriginalBodySlot(engine, body, &replacement));
        foreign.Release();
        EXPECT_FALSE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
        ASSERT_TRUE(engine.timelineArms.CancelPrepared(body.arm));
        EXPECT_EQ(body.armFreeBefore, engine.timelineArms.FreeCount());
        EXPECT_EQ(0u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
        ASSERT_EQ(VN_OK, PrepareAtOriginalBodySlot(engine, body, &replacement));
        // Prepared cancellation preserves generation; the original marker and
        // release episode still prevent old capability resurrection.
        EXPECT_EQ(body.source, replacement);
        EXPECT_FALSE(engine.voices.IsOriginalSourceBodyCurrent(foreign));
        EXPECT_FALSE(engine.voices.IsOriginalSourceBodyLifetimePinFor(foreign, body.arm, body.source));
        ASSERT_TRUE(engine.voices.CancelPreparedVoice(replacement));
        ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
    }
}


#include "PreparedSingleCurrentOriginBundle.h"
#include <type_traits>

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
namespace harken::detail {
// A scoped negative-only limit. It cannot issue any marker, successful loan,
// physical current body or completion authority. The real native cap predates it.
struct PreparedSingleBundleBorrowLimitTestAccess {
    static bool RefuseLastBorrow(VoiceLedger& table,
        const VoiceLedger::OriginalCompletionMutationOwner& original) {
        const auto identity = original.OriginalIdentity();
        if (!original.Valid() || identity.slot >= table.m_entries.ConstructedCount()) return false;
        auto& entry = table.m_entries[identity.slot];
        auto& native = entry.originalCompletion;
        if (entry.state != VoiceLedger::EntryState::PreparedCommitHeld ||
            native.generation != identity.generation || native.marker == 0 || native.externalCount != 1) return false;
        native.externalCount = UINT32_MAX;
        return true;
    }
    static void RestoreLastBorrow(VoiceLedger& table,
        const VoiceLedger::OriginalCompletionMutationOwner& original) {
        const auto identity = original.OriginalIdentity();
        auto& native = table.m_entries[identity.slot].originalCompletion;
        assert(native.generation == identity.generation && native.marker != 0 && native.externalCount == UINT32_MAX);
        native.externalCount = 1;
    }
};
}
#endif

namespace {
using SingleBundle = harken::PreparedSingleCurrentOriginBundle;
static_assert(!std::is_copy_constructible_v<SingleBundle>);
static_assert(std::is_nothrow_move_constructible_v<SingleBundle>);

struct GenuinePreparedSingleEpisode {
    SourceBody source;
    harken::PlayableInstanceHandle parent = 0;
    harken::PlayableInstanceHandle arranging = 0;
    harken::PreparedPlayableInstance originalParent;
    harken::PreparedPlayableInstance originalChild;
    harken::PreparedPlayableInstance unrelatedParent;
    harken::PlayableInstanceTable::PreparedCommitProof parentProof;
    harken::PlayableInstanceTable::PreparedCommitProof childProof;
    harken::PlayableInstanceTable::PreparedCommitProof unrelatedParentProof;
    harken::PlayableInstanceTable::PreparedParentLinksProof parentLinks{2};
    harken::OccurrenceToken occurrence{};
    harken::CompletionOwnerRef owner{};
    std::uint64_t scheduleEpoch = 0;
    harken::CompletionProgressRow* progress = nullptr;
    harken::VoiceLedger::PreparedCommitInput logical;
    harken::VoicePool::PreparedCommitInput physical;
    harken::PreparedDecisionReservation decisionPreparation;
    harken::DecisionReserveRequest decisionRequest;
    std::array<harken::CommandRecord, 2> commands{};
    harken::CommandQueue::BatchReservation reservation{};
    SingleBundle::SourcePin originalSource;
    SingleBundle::BodyPin originalBody;
    harken::OriginalDecisionLifetimePin originalDecision;
    SingleBundle::OccurrencePin originalOccurrence;
    SingleBundle::LedgerPin originalLedger;
    harken::TimelineArmTable::PreparedCommitProof armProof;
    harken::TimelineArmTable::PreparedCommitInput armInput{};
    harken::VoicePool::PreparedCommitProof voiceProof;
    SingleBundle::OccurrenceProof occurrenceProof;
    SingleBundle::LedgerProof ledgerProof;
    harken::CommandQueue::PreparedBatchProof batch;
    SingleBundle bundle;

    bool Prepare(harken::Engine& engine, bool wrongFrozenSource = false, bool unrelatedLedgerRoot = false,
                 bool pendingQueueSource = false, const GenuinePreparedSingleEpisode* current = nullptr,
                 harken::ParentAudioGroupRef successorGroup = {}, bool freezeHead = true) {
        if (current != nullptr) {
            const auto root = engine.playableInstances.Classify(current->parent);
            if (!pendingQueueSource || !successorGroup.Valid() || !current->bundle.Valid() ||
                !root.live || root.lifecycle != harken::PlayableLifecycle::Active) return false;
            parent = arranging = current->parent;
        } else {
        if (!engine.playableInstances.PrepareAcquire(harken::PlayableKind::Container, 0, &originalParent) ||
            !engine.playableInstances.PrepareAcquire(harken::PlayableKind::Sound, originalParent.Handle(), &originalChild) ||
            !engine.playableInstances.ProvePreparedCommit(originalParent, &parentProof) ||
            !engine.playableInstances.ProvePreparedCommit(originalChild, &childProof)) return false;
        const std::array<harken::PlayableInstanceTable::ParentLinkRequest, 1> children{{{&originalChild, &childProof}}};
        const std::array<harken::PlayableInstanceTable::ParentLinkRequest, 1> anchors{{{&originalParent, &parentProof}}};
        if (!engine.playableInstances.ProvePreparedParentLinks(children, anchors, &parentLinks)) return false;
        parent = originalParent.Handle();
        arranging = originalChild.Handle();
        if (unrelatedLedgerRoot) {
            if (!engine.playableInstances.PrepareAcquire(harken::PlayableKind::Container, 0, &unrelatedParent) ||
                !engine.playableInstances.ProvePreparedCommit(unrelatedParent, &unrelatedParentProof)) return false;
            parent = unrelatedParent.Handle();
        }
        }
        logical.parent = {0, parent};
        std::uint32_t ledgerSlot = UINT32_MAX;
        if (!engine.ledger.PreparePlayableOwned(logical.parent, &ledgerSlot, &logical.identity) ||
            ledgerSlot != logical.identity.slot) return false;
        const auto pcm = engine.registry.Lookup(kSound);
        if (!pcm.Valid() || engine.voices.Play(pcm, 0.0f, 1.0, 0, false, 0, 0, 0,
            harken::ResolvePanGains(engine.config.mixChannelLayout, 0.0f), 0, pcm.frames, 1,
            harken::ResampleProfile::Professional44148, &source.source, 1, UINT32_MAX, 0, true) != VN_OK ||
            !engine.voices.CaptureReleaseObserver(source.source, &source.release)) return false;
        physical.renderHandle = source.source;
        physical.originalRelease = source.release;
        logical.renderHandle = source.source;
        if (!engine.voices.CapturePreparedRoutingV1(source.source, &physical.routing) ||
            !engine.voices.ProvePreparedCommit(physical, &voiceProof)) return false;
        harken::TimelineArmTable::PreparedCommitInput logicalArm{};
        logicalArm.startFrame = pendingQueueSource ? 0 : engine.mixer.BlocksRendered() * kFrames;
        logicalArm.armDeadline = logicalArm.startFrame;
        logicalArm.programGeneration = pendingQueueSource ? 17 : 0;
        logicalArm.pendingImmediateStart = pendingQueueSource;
        logicalArm.parentGroup = successorGroup;
        logicalArm.memberIndex = successorGroup.Valid() ? 0 : UINT16_MAX;
        logicalArm.target.subjectId = kSound;
        source.armFreeBefore = engine.timelineArms.FreeCount();
        if (pendingQueueSource) {
            if (successorGroup.Valid()) {
                if (!engine.timelineArms.PreparePendingImmediateGrouped(static_cast<std::uint32_t>(source.source),
                    static_cast<std::uint32_t>(source.source >> 32), logicalArm.programGeneration,
                    logicalArm.target, successorGroup, 0, &source.arm)) return false;
            } else if (!engine.timelineArms.PreparePendingImmediate(static_cast<std::uint32_t>(source.source),
                static_cast<std::uint32_t>(source.source >> 32), logicalArm.programGeneration,
                logicalArm.target, &source.arm)) return false;
        } else if (!engine.timelineArms.Prepare(static_cast<std::uint32_t>(source.source),
            static_cast<std::uint32_t>(source.source >> 32), logicalArm.startFrame,
            logicalArm.armDeadline, logicalArm.programGeneration, logicalArm.target, &source.arm)) return false;
        logicalArm.arm = source.arm;
        armInput = logicalArm;
        if (!engine.timelineArms.ProvePreparedCommit(logicalArm, &armProof)) return false;
        auto& header = commands[0];
        header.opcode = harken::kOpcodePlay;
        header.payload.play.handle = source.source;
        header.payload.play.sourceIdentity = source.source;
        header.payload.play.batch.batchVersion = harken::kPlayLedBatchVersion;
        header.payload.play.batch.batchKind = harken::PlayLedBatchKind::TimelineArm;
        header.payload.play.batch.batchCommandCount = 1;
        header.payload.play.batch.armToken =
            (static_cast<std::uint64_t>(source.arm.generation) << 32) | source.arm.slot;
        if (!engine.originalSourceSubmissions.CapturePreparedSource(header, armProof,
            voiceProof, std::span<const harken::EffectPoolBank::PreparedCommitProof>{},
            &originalSource, &originalBody)) return false;
        // Only the early temporary Voice proof is released to perform the real
        // final Single bind. Authentic Source/Body factories remain retained.
        voiceProof.Release();
        decisionRequest.world = 7;
        decisionRequest.transport = pendingQueueSource ? engine.timelinePool.Epoch() : 9;
        decisionRequest.expectedContinuationGeneration = 11;
        decisionRequest.sliceCount = 1;
        if (!engine.decisions.PrepareReservation(decisionRequest, &decisionPreparation, &originalDecision) ||
            !engine.timelineOccurrences.PrepareAcquire({arranging, kSound, 0, 0}, &occurrence)) return false;
        scheduleEpoch = engine.timelineOccurrences.ScheduleEpochOf(occurrence);
        owner = harken::CompletionOwnerRef::Single(decisionPreparation.Token(), scheduleEpoch);
        const auto generation = static_cast<std::uint32_t>(source.source >> 32);
        if (!engine.timelineOccurrences.BindCompletionProgress(occurrence, generation, owner.factsRevision)) return false;
        progress = engine.timelineOccurrences.CompletionProgressOf(occurrence);
        if (progress == nullptr || !engine.voices.BindCompletionProgress(source.source, progress,
                occurrence, owner.factsRevision) ||
            !engine.timelineOccurrences.TryPinCompletionOwner(occurrence, owner, &originalOccurrence) ||
            !engine.ledger.BindPreparedSingleCompletionMutation(&logical, {occurrence, owner})) return false;
        physical.completionProgress = progress;
        physical.completionOccurrence = occurrence;
        physical.completionRenderGeneration = generation;
        physical.playbackFactsRevision = owner.factsRevision;
        if (!engine.timelineOccurrences.ProvePreparedSingleCompletionCommit(originalOccurrence,
                owner, progress, generation, scheduleEpoch, &occurrenceProof) ||
            !engine.ledger.ProvePreparedSingleCompletionCommit(logical, &ledgerProof) ||
            !engine.ledger.IssueOriginalCompletionMutationOwner(ledgerProof, &originalLedger) ||
            !engine.voices.ProvePreparedCommit(physical, &voiceProof)) return false;
        if (wrongFrozenSource) header.payload.play.sourceIdentity = source.independentPublic;
        return !freezeHead || FreezeHead(engine);
    }
    bool FreezeHead(harken::Engine& engine, const harken::CommandRecord* control = nullptr) {
        const auto count = control == nullptr ? 1u : 2u;
        if (control != nullptr) commands[1] = *control;
        return engine.commands.ReserveBatch(count, &reservation) &&
            engine.commands.ProvePreparedBatch(&reservation, commands.data(), count, &batch) &&
            engine.originalSourceSubmissions.BindPreparedChild(batch, 0, 1);
    }
    bool Join(harken::Engine& engine, SingleBundle* out) {
        return SingleBundle::Prepare(engine.originalSourceSubmissions, engine.voices,
            engine.timelineArms, engine.decisions, engine.timelineOccurrences, engine.ledger,
            engine.playableInstances, parentLinks, originalSource, originalBody, originalDecision, decisionPreparation,
            originalOccurrence, occurrenceProof, originalLedger, ledgerProof,
            armProof, voiceProof, physical, batch, source.arm, source.source, scheduleEpoch, out);
    }
    bool MatchesPrepared(const SingleBundle& actual) const {
        return actual.MatchesPrepared(parentLinks, decisionPreparation, occurrenceProof, ledgerProof,
            armProof, voiceProof, physical, batch);
    }
    void ReleaseProofs() {
        parentLinks.Release();
        unrelatedParentProof.Release();
        childProof.Release();
        parentProof.Release();
        ledgerProof.Release();
        occurrenceProof.Release();
        voiceProof.Release();
        armProof.Release();
    }
};

void ExpectUnpublishedEpisodePhysicalCancel(harken::Engine& engine, GenuinePreparedSingleEpisode& episode) {
    episode.ReleaseProofs();
    ASSERT_TRUE(engine.originalSourceSubmissions.CancelBoundPreparation(episode.source.arm, episode.batch));
    ASSERT_TRUE(engine.originalSourceSubmissions.DisposeUnpublishedVoice(episode.source.arm));
    ASSERT_TRUE(engine.originalSourceSubmissions.WithdrawUnpublished(episode.source.arm));
    EXPECT_TRUE(engine.voices.ReleaseObserved(episode.source.release));
    EXPECT_EQ(nullptr, engine.voices.Resolve(episode.source.source));
    EXPECT_EQ(0u, engine.timelineArms.RowAt(episode.source.arm.slot)->armedCompletionSequence.load());
    EXPECT_EQ(0u, engine.timelineArms.RowAt(episode.source.arm.slot)->startedBlockSequence.load());
    EXPECT_EQ(0u, engine.timelineArms.RowAt(episode.source.arm.slot)->tombstoneAudioSequence.load());
    ASSERT_TRUE(engine.ledger.CancelPreparedPlayableOwned(episode.logical.identity.slot));
    ASSERT_TRUE(engine.timelineOccurrences.RollbackCompletionOwnerPinExact(episode.occurrence, episode.owner));
    ASSERT_TRUE(engine.timelineOccurrences.UnbindCompletionProgress(episode.occurrence,
        static_cast<std::uint32_t>(episode.source.source >> 32)));
}
void ExpectUnpublishedEpisodeOriginalTail(harken::Engine& engine, GenuinePreparedSingleEpisode& episode) {
    episode.bundle.Release();
    ASSERT_TRUE(engine.timelineOccurrences.CancelPreparedAcquire(episode.occurrence, &episode.originalOccurrence));
    ASSERT_TRUE(engine.decisions.CancelPreparedReservation(&episode.decisionPreparation, &episode.originalDecision));
    episode.originalLedger.Release();
    episode.originalBody.Release();
    episode.originalSource.Release();
    EXPECT_FALSE(engine.timelineArms.RowAt(episode.source.arm.slot)->originalLifetimePinned.load());
    ASSERT_TRUE(engine.timelineArms.CancelPrepared(episode.source.arm));
    EXPECT_EQ(episode.source.armFreeBefore, engine.timelineArms.FreeCount());
    EXPECT_EQ(0u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
    ASSERT_TRUE(engine.playableInstances.CancelPrepared(&episode.originalChild));
    ASSERT_TRUE(engine.playableInstances.CancelPrepared(&episode.originalParent));
    if (episode.unrelatedParent.Valid()) ASSERT_TRUE(engine.playableInstances.CancelPrepared(&episode.unrelatedParent));
    episode.parent = 0;
    episode.arranging = 0;
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, episode.source));
}

TEST(OriginalSourceSubmission, PreparedSingleBundleUsesAllGenuineFactoriesBeforeHeadWithoutRequiringAckOrStart) {
    const std::vector<float> samples(kRate, 0.25f);
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    GenuinePreparedSingleEpisode episode;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, episode.source, samples));
    const auto head = engine.commands.PublishedHead();
    const auto completed = engine.mixer.BlocksRendered();
    ASSERT_TRUE(episode.Prepare(engine));
    ASSERT_NE(episode.parent, episode.arranging);
    ASSERT_EQ(episode.arranging, episode.occurrenceProof.OriginalArrangingInstance());
    ASSERT_TRUE(engine.playableInstances.MatchesPreparedOriginalAncestry(episode.parentLinks,
        episode.arranging, episode.parent));
    ASSERT_TRUE(episode.Join(engine, &episode.bundle));
    EXPECT_EQ(head, engine.commands.PublishedHead());
    EXPECT_EQ(completed, engine.mixer.BlocksRendered());
    EXPECT_TRUE(episode.MatchesPrepared(episode.bundle));
    EXPECT_FALSE(episode.bundle.ObserveInstalled());
    std::uint64_t started = 53;
    EXPECT_FALSE(episode.bundle.CaptureStartedSequence(&started));
    EXPECT_EQ(53u, started);
    EXPECT_EQ(0u, engine.timelineArms.RowAt(episode.source.arm.slot)->armedCompletionSequence.load());
    EXPECT_EQ(0u, engine.timelineArms.RowAt(episode.source.arm.slot)->startedBlockSequence.load());
    SingleBundle foreign;
    ASSERT_TRUE(episode.bundle.Borrow(&foreign));
    EXPECT_TRUE(episode.MatchesPrepared(foreign));
    EXPECT_FALSE(episode.bundle.Borrow(&foreign));
    EXPECT_FALSE(episode.bundle.Borrow(&episode.bundle));
    EXPECT_FALSE(episode.Join(engine, &foreign));
    episode.bundle.Release();
    ASSERT_NO_FATAL_FAILURE(ExpectUnpublishedEpisodePhysicalCancel(engine, episode));
    EXPECT_TRUE(foreign.Valid());
    EXPECT_FALSE(foreign.ObserveInstalled());
    EXPECT_FALSE(engine.timelineOccurrences.CancelPreparedAcquire(episode.occurrence, &episode.originalOccurrence));
    EXPECT_TRUE(episode.originalOccurrence.Valid());
    EXPECT_FALSE(engine.decisions.CancelPreparedReservation(&episode.decisionPreparation, &episode.originalDecision));
    EXPECT_TRUE(episode.decisionPreparation.Valid());
    EXPECT_TRUE(episode.originalDecision.Valid());
    EXPECT_TRUE(engine.timelineArms.RowAt(episode.source.arm.slot)->originalLifetimePinned.load());
    foreign.Release();
    ASSERT_NO_FATAL_FAILURE(ExpectUnpublishedEpisodeOriginalTail(engine, episode));
}

TEST(OriginalSourceSubmission, PreparedSingleBundleObservesFreshActualInstallAckWhileRealStartRemainsAbsent) {
    const std::vector<float> samples(kRate, 0.25f);
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    GenuinePreparedSingleEpisode episode;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, episode.source, samples));
    ASSERT_TRUE(episode.Prepare(engine));
    ASSERT_TRUE(episode.Join(engine, &episode.bundle));
    SingleBundle foreign;
    ASSERT_TRUE(episode.bundle.Borrow(&foreign));
    const auto decision = episode.originalDecision.Token();
    harken::DecisionTable::PreparedTicketCommitProof decisionProof;
    const auto sequence = engine.logicCommits.Issue();
    ASSERT_NE(0u, sequence);
    ASSERT_TRUE(engine.decisions.ProvePreparedReservationCommit(episode.decisionPreparation,
        episode.decisionRequest, sequence, &decisionProof));
    harken::DecisionTable::ConsumePreparedReservationCommit(std::move(decisionProof));
    harken::PlayableInstanceTable::ConsumePreparedCommit(std::move(episode.parentProof));
    harken::PlayableInstanceTable::ConsumePreparedCommit(std::move(episode.childProof));
    harken::PlayableInstanceTable::ConsumePreparedParentLinks(std::move(episode.parentLinks));
    harken::OccurrenceStatePool::ConsumePreparedSingleCompletionCommit(std::move(episode.occurrenceProof));
    harken::VoiceLedger::ConsumePreparedSingleCompletionCommit(std::move(episode.ledgerProof));
    harken::VoicePool::ConsumePreparedCommit(std::move(episode.voiceProof));
    harken::TimelineArmTable::ConsumePreparedCommit(std::move(episode.armProof));
    engine.originalSourceSubmissions.PublishPreparedChild(episode.commands[0]);
    (void)engine.commands.ConsumePreparedBatch(std::move(episode.batch));
    EXPECT_FALSE(episode.bundle.ObserveInstalled());
    const auto before = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    const auto* armRow = engine.timelineArms.RowAt(episode.source.arm.slot);
    ASSERT_EQ(before + 1u, armRow->armedCompletionSequence.load());
    ASSERT_EQ(0u, armRow->startedBlockSequence.load());
    ASSERT_TRUE(episode.bundle.ObserveInstalled());
    std::uint64_t started = 59;
    EXPECT_FALSE(episode.bundle.CaptureStartedSequence(&started));
    EXPECT_EQ(59u, started);
    episode.bundle.Release();
    ASSERT_TRUE(engine.timelineArms.RequestCancel(episode.source.arm));
    harken::CommandRecord stop{};
    stop.opcode = harken::kOpcodeStop;
    stop.payload.stop.handle = episode.source.source;
    ASSERT_TRUE(engine.commands.TryPush(stop));
    const auto stopBefore = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(stopBefore + 1u, engine.mixer.BlocksRendered());
    ASSERT_TRUE(engine.voices.ReleaseObserved(episode.source.release));
    EXPECT_FALSE(foreign.ObserveInstalled());
    EXPECT_TRUE(foreign.Valid());
    engine.ledger.Reap(episode.source.source);
    ASSERT_TRUE(engine.timelineOccurrences.ClearCompletionOwnerExact(episode.occurrence, episode.owner));
    harken::CompletionProgressSnapshot terminal{};
    ASSERT_TRUE(harken::CaptureCompletionProgressExact(*episode.progress, episode.occurrence,
        static_cast<std::uint32_t>(episode.source.source >> 32), &terminal));
    ASSERT_TRUE(engine.timelineOccurrences.UnbindCompletionProgress(episode.occurrence,
        static_cast<std::uint32_t>(episode.source.source >> 32)));
    ASSERT_TRUE(engine.timelineOccurrences.Advance(episode.occurrence, harken::OccurrenceStage::AwaitingEventFence));
    ASSERT_TRUE(engine.decisions.Cancel(decision));
    EXPECT_FALSE(engine.timelineOccurrences.ReleaseAfterFence(episode.occurrence,
        engine.mixer.BlocksRendered(), &episode.originalOccurrence));
    EXPECT_FALSE(engine.decisions.ReclaimCancelled(decision, &episode.originalDecision));
    EXPECT_FALSE(engine.timelineArms.Reclaim(episode.source.arm, engine.mixer.BlocksRendered()));
    foreign.Release();
    EXPECT_FALSE(engine.timelineOccurrences.ReleaseAfterFence(episode.occurrence,
        terminal.completedThroughBlockSequence, &episode.originalOccurrence));
    episode.originalLedger.Release();
    episode.originalBody.Release();
    episode.originalSource.Release();
    const auto afterStop = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(afterStop + 1u, engine.mixer.BlocksRendered());
    ASSERT_GT(engine.mixer.BlocksRendered(), terminal.completedThroughBlockSequence);
    ASSERT_TRUE(engine.timelineOccurrences.ReleaseAfterFence(episode.occurrence,
        engine.mixer.BlocksRendered(), &episode.originalOccurrence));
    ASSERT_TRUE(engine.decisions.ReclaimCancelled(decision, &episode.originalDecision));
    ASSERT_TRUE(engine.timelineArms.Reclaim(episode.source.arm, engine.mixer.BlocksRendered()));
    EXPECT_EQ(episode.source.armFreeBefore, engine.timelineArms.FreeCount());
    EXPECT_EQ(0u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
    EXPECT_FALSE(foreign.Valid());
    ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(episode.arranging, harken::PlayableLifecycle::Retired));
    ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(episode.parent, harken::PlayableLifecycle::Retired));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, episode.source));
}

TEST(OriginalSourceSubmission, PreparedSingleBundleRejectsNativeForeignReceiverAndFullScopeRetargetWithoutLosingOwners) {
    const std::vector<float> samples(kRate, 0.25f);
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    GenuinePreparedSingleEpisode episode;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, episode.source, samples));
    ASSERT_TRUE(episode.Prepare(engine));
    SingleBundle output;
    Registry foreign(engine.timelineArms, engine.voices, engine.commands, engine.effectBank);
    harken::VoicePool foreignVoices(engine.config, &engine.tables, &engine.multirateTables,
        &engine.biquadTrig, engine.effectBank, engine.dynamicRoutes);
    EXPECT_FALSE(SingleBundle::Prepare(engine.originalSourceSubmissions, foreignVoices,
        engine.timelineArms, engine.decisions, engine.timelineOccurrences, engine.ledger,
        engine.playableInstances, episode.parentLinks, episode.originalSource, episode.originalBody, episode.originalDecision, episode.decisionPreparation,
        episode.originalOccurrence, episode.occurrenceProof, episode.originalLedger, episode.ledgerProof,
        episode.armProof, episode.voiceProof, episode.physical, episode.batch, episode.source.arm,
        episode.source.source, episode.scheduleEpoch, &output));
    EXPECT_FALSE(output.Valid());
    EXPECT_FALSE(foreign.MatchesPreparedOriginalSourcePair(episode.originalSource, episode.originalBody,
        episode.armProof, episode.voiceProof, episode.physical, episode.batch,
        episode.source.arm, episode.source.source));
    auto wrong = episode.physical;
    ++wrong.playbackFactsRevision;
    EXPECT_FALSE(engine.originalSourceSubmissions.MatchesPreparedOriginalSourcePair(
        episode.originalSource, episode.originalBody, episode.armProof, episode.voiceProof,
        wrong, episode.batch, episode.source.arm, episode.source.source));
    auto wrongArm = episode.source.arm;
    ++wrongArm.generation;
    EXPECT_FALSE(engine.originalSourceSubmissions.MatchesPreparedOriginalSourcePair(
        episode.originalSource, episode.originalBody, episode.armProof, episode.voiceProof,
        episode.physical, episode.batch, wrongArm, episode.source.source));
    auto wrongLogical = episode.logical;
    wrongLogical.completionMutation = {};
    EXPECT_FALSE(engine.ledger.BindPreparedSingleCompletionMutation(&wrongLogical,
        {episode.occurrence, episode.owner}));
    EXPECT_TRUE(harken::IsCanonicalUnboundCompletionMutation(wrongLogical.completionMutation));
    SingleBundle::LedgerProof unboundProof;
    EXPECT_FALSE(engine.ledger.ProvePreparedSingleCompletionCommit(wrongLogical, &unboundProof));
    harken::VoiceLedger foreignLedger(1, 4);
    EXPECT_FALSE(foreignLedger.MatchesPreparedSingleCompletionCommit(episode.ledgerProof));
    EXPECT_FALSE(foreignLedger.IssueOriginalCompletionMutationOwner(episode.ledgerProof, nullptr));
    ASSERT_TRUE(episode.Join(engine, &episode.bundle));
    SingleBundle moved(std::move(episode.bundle));
    EXPECT_FALSE(episode.bundle.Valid());
    EXPECT_TRUE(moved.Valid());
    EXPECT_TRUE(episode.MatchesPrepared(moved));
    ASSERT_TRUE(moved.Borrow(&output));
    moved = std::move(output);
    EXPECT_FALSE(output.Valid());
    EXPECT_TRUE(moved.Valid());
    moved.Release();
    ASSERT_NO_FATAL_FAILURE(ExpectUnpublishedEpisodePhysicalCancel(engine, episode));
    ASSERT_NO_FATAL_FAILURE(ExpectUnpublishedEpisodeOriginalTail(engine, episode));
}

TEST(OriginalSourceSubmission, PreparedSingleBundleRejectsTwoGenuineButUnrelatedNativeRootScopes) {
    const std::vector<float> samples(kRate, 0.25f);
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    GenuinePreparedSingleEpisode episode;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, episode.source, samples));
    ASSERT_TRUE(episode.Prepare(engine, false, true));
    ASSERT_TRUE(episode.originalParent.Valid());
    ASSERT_TRUE(episode.originalChild.Valid());
    ASSERT_TRUE(episode.unrelatedParent.Valid());
    ASSERT_EQ(episode.originalChild.Parent(), episode.originalParent.Handle());
    ASSERT_EQ(episode.occurrenceProof.OriginalArrangingInstance(), episode.originalChild.Handle());
    ASSERT_EQ(episode.ledgerProof.OriginalInput().parent.internalToken, episode.unrelatedParent.Handle());
    ASSERT_TRUE(engine.playableInstances.MatchesPreparedOriginalAncestry(episode.parentLinks,
        episode.arranging, episode.originalParent.Handle()));
    EXPECT_FALSE(engine.playableInstances.MatchesPreparedOriginalAncestry(episode.parentLinks,
        episode.arranging, episode.unrelatedParent.Handle()));
    harken::PlayableInstanceTable foreignInstances(4);
    EXPECT_FALSE(foreignInstances.MatchesPreparedOriginalAncestry(episode.parentLinks,
        episode.arranging, episode.originalParent.Handle()));
    const auto head = engine.commands.PublishedHead();
    const auto completed = engine.mixer.BlocksRendered();
    EXPECT_FALSE(episode.Join(engine, &episode.bundle));
    EXPECT_FALSE(episode.bundle.Valid());
    EXPECT_EQ(head, engine.commands.PublishedHead());
    EXPECT_EQ(completed, engine.mixer.BlocksRendered());
    EXPECT_TRUE(episode.originalSource.Valid());
    EXPECT_TRUE(episode.originalBody.Valid());
    EXPECT_TRUE(episode.originalDecision.Valid());
    EXPECT_TRUE(episode.originalOccurrence.Valid());
    EXPECT_TRUE(episode.originalLedger.Valid());
    EXPECT_TRUE(episode.parentLinks.Valid());
    ASSERT_NO_FATAL_FAILURE(ExpectUnpublishedEpisodePhysicalCancel(engine, episode));
    ASSERT_NO_FATAL_FAILURE(ExpectUnpublishedEpisodeOriginalTail(engine, episode));
}

TEST(OriginalSourceSubmission, PreparedSingleBundleRejectsActualEarlySourceIdentityVersusLaterNativeFrozenIdentity) {
    const std::vector<float> samples(kRate, 0.25f);
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    GenuinePreparedSingleEpisode episode;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, episode.source, samples));
    ASSERT_TRUE(episode.Prepare(engine, true));
    EXPECT_NE(episode.source.source, episode.source.independentPublic);
    EXPECT_TRUE(engine.originalSourceSubmissions.IsOriginalSourceLifetimePinFor(episode.originalSource, episode.source.arm));
    EXPECT_TRUE(engine.voices.IsOriginalSourceBodyLifetimePinFor(episode.originalBody,
        episode.source.arm, episode.source.source));
    EXPECT_FALSE(episode.Join(engine, &episode.bundle));
    EXPECT_FALSE(episode.bundle.Valid());
    EXPECT_FALSE(engine.originalSourceSubmissions.MatchesPreparedOriginalSourcePair(
        episode.originalSource, episode.originalBody, episode.armProof, episode.voiceProof,
        episode.physical, episode.batch, episode.source.arm, episode.source.independentPublic));
    EXPECT_EQ(0u, engine.timelineArms.RowAt(episode.source.arm.slot)->armedCompletionSequence.load());
    EXPECT_EQ(0u, engine.timelineArms.RowAt(episode.source.arm.slot)->startedBlockSequence.load());
    ASSERT_NO_FATAL_FAILURE(ExpectUnpublishedEpisodePhysicalCancel(engine, episode));
    ASSERT_NO_FATAL_FAILURE(ExpectUnpublishedEpisodeOriginalTail(engine, episode));
}

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
TEST(OriginalSourceSubmission, PreparedSingleBundleLastNativeBorrowRefusalReturnsEveryEarlierGenuinePrefix) {
    const std::vector<float> samples(kRate, 0.25f);
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    GenuinePreparedSingleEpisode episode;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, episode.source, samples));
    ASSERT_TRUE(episode.Prepare(engine));
    using Access = harken::detail::PreparedSingleBundleBorrowLimitTestAccess;
    struct ScopedRefusal {
        harken::VoiceLedger& ledger;
        const SingleBundle::LedgerPin& original;
        bool active;
        ~ScopedRefusal() { if (active) Access::RestoreLastBorrow(ledger, original); }
    } refusal{engine.ledger, episode.originalLedger,
        Access::RefuseLastBorrow(engine.ledger, episode.originalLedger)};
    ASSERT_TRUE(refusal.active);
    EXPECT_FALSE(episode.Join(engine, &episode.bundle));
    EXPECT_FALSE(episode.bundle.Valid());
    Access::RestoreLastBorrow(engine.ledger, episode.originalLedger);
    refusal.active = false;
    // Successful real baseline disposal proves no temporary Source/Body,
    // Decision or Occurrence loan from the failed prefix survived.
    ASSERT_NO_FATAL_FAILURE(ExpectUnpublishedEpisodePhysicalCancel(engine, episode));
    ASSERT_NO_FATAL_FAILURE(ExpectUnpublishedEpisodeOriginalTail(engine, episode));
}
#endif
} // namespace


#include "PlayableInstanceTableTestAccess.h"

namespace {
using PlayableOriginal = harken::PlayableInstanceTable::OriginalLifetimePin;
struct GenuineEngineSingleRootEpisode : GenuinePreparedSingleEpisode {
    bool PrepareSingleRoot(harken::Engine& engine, bool wrongFrozenSource = false, bool unrelatedLedgerRoot = false) {
        if (!engine.playableInstances.PrepareAcquire(harken::PlayableKind::Sound, 0, &originalParent) ||
            !engine.playableInstances.ProvePreparedCommit(originalParent, &parentProof)) return false;
        const std::array<harken::PlayableInstanceTable::ParentLinkRequest, 1> anchors{{{&originalParent, &parentProof}}};
        if (!engine.playableInstances.ProvePreparedParentLinks({}, anchors, &parentLinks)) return false;
        parent = originalParent.Handle();
        arranging = parent;
        if (unrelatedLedgerRoot) {
            if (!engine.playableInstances.PrepareAcquire(harken::PlayableKind::Container, 0, &unrelatedParent) ||
                !engine.playableInstances.ProvePreparedCommit(unrelatedParent, &unrelatedParentProof)) return false;
            parent = unrelatedParent.Handle();
        }
        logical.parent = {0, parent};
        std::uint32_t ledgerSlot = UINT32_MAX;
        if (!engine.ledger.PreparePlayableOwned(logical.parent, &ledgerSlot, &logical.identity) ||
            ledgerSlot != logical.identity.slot) return false;
        const auto pcm = engine.registry.Lookup(kSound);
        if (!pcm.Valid() || engine.voices.Play(pcm, 0.0f, 1.0, 0, false, 0, 0, 0,
            harken::ResolvePanGains(engine.config.mixChannelLayout, 0.0f), 0, pcm.frames, 1,
            harken::ResampleProfile::Professional44148, &source.source, 1, UINT32_MAX, 0, true) != VN_OK ||
            !engine.voices.CaptureReleaseObserver(source.source, &source.release)) return false;
        physical.renderHandle = source.source;
        physical.originalRelease = source.release;
        logical.renderHandle = source.source;
        if (!engine.voices.CapturePreparedRoutingV1(source.source, &physical.routing) ||
            !engine.voices.ProvePreparedCommit(physical, &voiceProof)) return false;
        harken::TimelineArmTable::PreparedCommitInput logicalArm{};
        logicalArm.startFrame = engine.mixer.BlocksRendered() * kFrames;
        logicalArm.armDeadline = logicalArm.startFrame;
        logicalArm.target.subjectId = kSound;
        source.armFreeBefore = engine.timelineArms.FreeCount();
        if (!engine.timelineArms.Prepare(static_cast<std::uint32_t>(source.source),
            static_cast<std::uint32_t>(source.source >> 32), logicalArm.startFrame,
            logicalArm.armDeadline, logicalArm.programGeneration, logicalArm.target, &source.arm)) return false;
        logicalArm.arm = source.arm;
        if (!engine.timelineArms.ProvePreparedCommit(logicalArm, &armProof)) return false;
        auto& header = commands[0];
        header.opcode = harken::kOpcodePlay;
        header.payload.play.handle = source.source;
        header.payload.play.sourceIdentity = source.source;
        header.payload.play.batch.batchVersion = harken::kPlayLedBatchVersion;
        header.payload.play.batch.batchKind = harken::PlayLedBatchKind::TimelineArm;
        header.payload.play.batch.batchCommandCount = 1;
        header.payload.play.batch.armToken =
            (static_cast<std::uint64_t>(source.arm.generation) << 32) | source.arm.slot;
        if (!engine.originalSourceSubmissions.CapturePreparedSource(header, armProof,
            voiceProof, std::span<const harken::EffectPoolBank::PreparedCommitProof>{},
            &originalSource, &originalBody)) return false;
        // Only the early temporary Voice proof is released to perform the real
        // final Single bind. Authentic Source/Body factories remain retained.
        voiceProof.Release();
        decisionRequest.world = 7;
        decisionRequest.transport = 9;
        decisionRequest.expectedContinuationGeneration = 11;
        decisionRequest.sliceCount = 1;
        if (!engine.decisions.PrepareReservation(decisionRequest, &decisionPreparation, &originalDecision) ||
            !engine.timelineOccurrences.PrepareAcquire({arranging, kSound, 0, 0}, &occurrence)) return false;
        scheduleEpoch = engine.timelineOccurrences.ScheduleEpochOf(occurrence);
        owner = harken::CompletionOwnerRef::Single(decisionPreparation.Token(), scheduleEpoch);
        const auto generation = static_cast<std::uint32_t>(source.source >> 32);
        if (!engine.timelineOccurrences.BindCompletionProgress(occurrence, generation, owner.factsRevision)) return false;
        progress = engine.timelineOccurrences.CompletionProgressOf(occurrence);
        if (progress == nullptr || !engine.voices.BindCompletionProgress(source.source, progress,
                occurrence, owner.factsRevision) ||
            !engine.timelineOccurrences.TryPinCompletionOwner(occurrence, owner, &originalOccurrence) ||
            !engine.ledger.BindPreparedSingleCompletionMutation(&logical, {occurrence, owner})) return false;
        physical.completionProgress = progress;
        physical.completionOccurrence = occurrence;
        physical.completionRenderGeneration = generation;
        physical.playbackFactsRevision = owner.factsRevision;
        if (!engine.timelineOccurrences.ProvePreparedSingleCompletionCommit(originalOccurrence,
                owner, progress, generation, scheduleEpoch, &occurrenceProof) ||
            !engine.ledger.ProvePreparedSingleCompletionCommit(logical, &ledgerProof) ||
            !engine.ledger.IssueOriginalCompletionMutationOwner(ledgerProof, &originalLedger) ||
            !engine.voices.ProvePreparedCommit(physical, &voiceProof)) return false;
        if (wrongFrozenSource) header.payload.play.sourceIdentity = source.independentPublic;
        return engine.commands.ReserveBatch(1, &reservation) &&
            engine.commands.ProvePreparedBatch(&reservation, commands.data(), 1, &batch) &&
            engine.originalSourceSubmissions.BindPreparedChild(batch, 0, 1);
    }
};

bool JoinEngineOriginalCurrent(harken::Engine& engine, GenuinePreparedSingleEpisode& episode,
    harken::Engine::OriginalSingleCurrentHolder** original = nullptr) {
    return engine.PrepareOriginalSingleCurrent(episode.parentLinks, episode.originalSource, episode.originalBody,
        episode.originalDecision, episode.decisionPreparation, episode.originalOccurrence, episode.occurrenceProof,
        episode.originalLedger, episode.ledgerProof, episode.armProof, episode.voiceProof, episode.physical,
        episode.batch, episode.source.arm, episode.source.source, episode.scheduleEpoch, original);
}
harken::Engine::OriginalSingleCurrentHolder& CurrentHolder(harken::Engine& engine,
    const GenuinePreparedSingleEpisode& episode) {
    return engine.originalSingleCurrentHolders[static_cast<std::uint32_t>(episode.arranging)];
}

TEST(OriginalSourceSubmission, EngineOriginalPlayableTreeForeignCancelRetainsTheRealPrivateAndAdoptedPrefix) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    harken::PlayableTreeExecutor tree(2);
    harken::PreparedPlayableInstance root;
    harken::PreparedPlayableInstance adopted;
    ASSERT_TRUE(engine.playableInstances.PrepareAcquire(harken::PlayableKind::Container, 0, &root));
    const auto rootHandle = root.Handle();
    ASSERT_TRUE(tree.AdoptPreparedInstance(engine.playableInstances, &root));
    ASSERT_TRUE(tree.PrepareInstance(engine.playableInstances, harken::PlayableKind::Sound, rootHandle));
    ASSERT_TRUE(engine.playableInstances.PrepareAcquire(harken::PlayableKind::Sound, rootHandle, &adopted));
    const auto adoptedHandle = adopted.Handle();
    ASSERT_TRUE(tree.AdoptPreparedInstanceReservation(&adopted));
    ASSERT_TRUE(tree.ProveInstanceCommits(&engine.playableInstances, &root));
    ASSERT_TRUE(tree.ProveParentLinks(engine.playableInstances, &root));
    ASSERT_TRUE(engine.IssueOriginalPlayableTreeLifetimes(tree));
    PlayableOriginal foreign;
    ASSERT_TRUE(engine.originalPlayableLifetimeHolders[static_cast<std::uint32_t>(adoptedHandle)].Borrow(&foreign));
    EXPECT_FALSE(tree.Cancel(engine.playableInstances));
    EXPECT_TRUE(adopted.Valid());
    EXPECT_EQ(adoptedHandle, adopted.Handle());
    EXPECT_TRUE(root.Valid());
    EXPECT_EQ(rootHandle, root.Handle());
    EXPECT_FALSE(tree.ResetForReuse());
    EXPECT_TRUE(engine.originalPlayableLifetimeHolders[static_cast<std::uint32_t>(adoptedHandle)].Valid());
    EXPECT_FALSE(tree.Cancel(engine.playableInstances));
    foreign.Release();
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FALSE(adopted.Valid());
    EXPECT_EQ(adoptedHandle, adopted.Handle());
    EXPECT_FALSE(tree.ResetForReuse());
    // A completed native cancellation still belongs to its retained Tree
    // alias. Exhaust all genuinely free slots without replacing that receipt.
    std::vector<harken::PreparedPlayableInstance> probes(engine.playableInstances.Capacity());
    std::size_t acquired = 0;
    while (acquired < probes.size() && engine.playableInstances.PrepareAcquire(
            harken::PlayableKind::Sound, 0, &probes[acquired])) {
        EXPECT_NE(static_cast<std::uint32_t>(adoptedHandle),
            static_cast<std::uint32_t>(probes[acquired].Handle()));
        ++acquired;
    }
    // The genuine native completion receipt clears the exact retained alias.
    ASSERT_TRUE(tree.Cancel(engine.playableInstances));
    for (std::size_t index = 0; index < acquired; ++index)
        ASSERT_TRUE(engine.playableInstances.CancelPrepared(&probes[index]));
    EXPECT_FALSE(root.Valid());
    EXPECT_FALSE(adopted.Valid());
    EXPECT_FALSE(engine.originalPlayableLifetimeHolders[static_cast<std::uint32_t>(rootHandle)].Valid());
    EXPECT_FALSE(engine.originalPlayableLifetimeHolders[static_cast<std::uint32_t>(adoptedHandle)].Valid());
    ASSERT_TRUE(tree.ResetForReuse());
    EXPECT_EQ(engine.playableInstances.Capacity(), engine.playableInstances.FreeCount());
}

TEST(OriginalSourceSubmission, EngineOriginalPlayableRollbackForeignBusyPreservesTheActualCommittedClosingOwner) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    harken::PlayableTreeExecutor tree(2);
    ASSERT_TRUE(tree.PrepareInstance(engine.playableInstances, harken::PlayableKind::Container));
    const auto root = tree.PreparedInstanceHandle(0);
    ASSERT_TRUE(tree.PrepareInstance(engine.playableInstances, harken::PlayableKind::Sound, root));
    const auto child = tree.PreparedInstanceHandle(1);
    ASSERT_TRUE(tree.ProveInstanceCommits());
    ASSERT_TRUE(tree.ProveParentLinks(engine.playableInstances));
    ASSERT_TRUE(engine.IssueOriginalPlayableTreeLifetimes(tree));
    PlayableOriginal foreign;
    ASSERT_TRUE(engine.originalPlayableLifetimeHolders[static_cast<std::uint32_t>(child)].Borrow(&foreign));
    ASSERT_TRUE(tree.Commit());
    EXPECT_EQ(child, engine.playableInstances.FirstChild(root));
    EXPECT_FALSE(tree.RollbackCommitted(engine.playableInstances));
    EXPECT_TRUE(tree.Committed());
    EXPECT_EQ(2u, tree.InstanceReservationCount());
    EXPECT_TRUE(tree.PreparedInstanceToken(1)->Valid());
    EXPECT_EQ(child, tree.PreparedInstanceToken(1)->Handle());
    EXPECT_FALSE(tree.ResetForReuse());
    EXPECT_FALSE(tree.Cancel(engine.playableInstances));
    foreign.Release();
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_TRUE(tree.RollbackCommitted(engine.playableInstances));
    EXPECT_FALSE(tree.Committed());
    ASSERT_TRUE(tree.ResetForReuse());
    EXPECT_EQ(engine.playableInstances.Capacity(), engine.playableInstances.FreeCount());
}

TEST(OriginalSourceSubmission, EngineOriginalPlayableFailedHeldCommitKeepsActualUnpublishedTokensForRetry) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    harken::PlayableTreeExecutor tree(1);
    ASSERT_TRUE(tree.PrepareInstance(engine.playableInstances, harken::PlayableKind::Sound));
    const auto root = tree.PreparedInstanceHandle(0);
    struct Failure {
        static bool Validate(void* context) noexcept { return context != nullptr; }
        static bool Commit(void*, std::uint64_t) noexcept { return false; }
        static bool Cancel(void*) noexcept { return true; }
    };
    int actualOwner = 0;
    ASSERT_TRUE(tree.AdoptPreparedOwnerToken(&actualOwner, &Failure::Validate, &Failure::Commit, &Failure::Cancel));
    ASSERT_TRUE(tree.ProveInstanceCommits());
    ASSERT_TRUE(tree.ProveParentLinks(engine.playableInstances));
    ASSERT_TRUE(engine.IssueOriginalPlayableTreeLifetimes(tree));
    PlayableOriginal foreign;
    ASSERT_TRUE(engine.originalPlayableLifetimeHolders[static_cast<std::uint32_t>(root)].Borrow(&foreign));
    const auto head = engine.commands.PublishedHead();
    EXPECT_FALSE(tree.Commit());
    EXPECT_EQ(head, engine.commands.PublishedHead());
    EXPECT_EQ(1u, tree.InstanceReservationCount());
    EXPECT_TRUE(tree.PreparedInstanceToken(0)->Valid());
    EXPECT_EQ(root, tree.PreparedInstanceToken(0)->Handle());
    EXPECT_FALSE(tree.ResetForReuse());
    EXPECT_FALSE(tree.RollbackCommitted(engine.playableInstances));
    foreign.Release();
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_TRUE(tree.RollbackCommitted(engine.playableInstances));
    ASSERT_TRUE(tree.ResetForReuse());
    EXPECT_EQ(engine.playableInstances.Capacity(), engine.playableInstances.FreeCount());
}

TEST(OriginalSourceSubmission, EngineOriginalPlayableMoveOverwriteRefusesToDiscardTheForeignHeldDestination) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    harken::PlayableTreeExecutor destination(1), incoming(1);
    ASSERT_TRUE(destination.PrepareInstance(engine.playableInstances, harken::PlayableKind::Sound));
    const auto original = destination.PreparedInstanceHandle(0);
    ASSERT_TRUE(destination.ProveInstanceCommits());
    ASSERT_TRUE(destination.ProveParentLinks(engine.playableInstances));
    ASSERT_TRUE(engine.IssueOriginalPlayableTreeLifetimes(destination));
    PlayableOriginal foreign;
    ASSERT_TRUE(engine.originalPlayableLifetimeHolders[static_cast<std::uint32_t>(original)].Borrow(&foreign));
    destination.ReleaseCommitProofs();
    ASSERT_TRUE(incoming.PrepareInstance(engine.playableInstances, harken::PlayableKind::Sound));
    const auto next = incoming.PreparedInstanceHandle(0);
    destination = std::move(incoming);
    EXPECT_EQ(original, destination.PreparedInstanceHandle(0));
    EXPECT_EQ(next, incoming.PreparedInstanceHandle(0));
    EXPECT_FALSE(destination.ResetForReuse());
    foreign.Release();
    ASSERT_TRUE(destination.Cancel(engine.playableInstances));
    ASSERT_TRUE(incoming.Cancel(engine.playableInstances));
    ASSERT_TRUE(destination.ResetForReuse());
    ASSERT_TRUE(incoming.ResetForReuse());
    EXPECT_EQ(engine.playableInstances.Capacity(), engine.playableInstances.FreeCount());
}

TEST(OriginalSourceSubmission, EngineOriginalSingleCurrentBeforeHeadOwnAndForeignLoansCloseMonotonically) {
    const std::vector<float> samples(kRate, 0.25f);
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    GenuinePreparedSingleEpisode episode;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, episode.source, samples));
    ASSERT_TRUE(episode.Prepare(engine));
    ASSERT_TRUE(engine.IssueOriginalPlayableLifetimes(episode.parentLinks));
    const auto head = engine.commands.PublishedHead();
    const auto completed = engine.mixer.BlocksRendered();
    harken::Engine::OriginalSingleCurrentHolder* issued = nullptr;
    ASSERT_TRUE(JoinEngineOriginalCurrent(engine, episode, &issued));
    ASSERT_NE(nullptr, issued);
    EXPECT_FALSE(JoinEngineOriginalCurrent(engine, episode, &issued));
    auto& holder = *issued;
    ASSERT_TRUE(holder.bundle.HasOriginalPlayableRetention());
    EXPECT_EQ(episode.owner.singleDecision.slot, holder.originalCurrentSingleDecision.slot);
    EXPECT_EQ(episode.owner.singleDecision.generation, holder.originalCurrentSingleDecision.generation);
    EXPECT_EQ(episode.occurrence.slot, holder.originalCurrentOccurrence.slot);
    EXPECT_EQ(episode.occurrence.generation, holder.originalCurrentOccurrence.generation);
    EXPECT_EQ(episode.source.source, holder.originalCurrentSourceIdentity);
    ASSERT_NE(nullptr, holder.bundle.OriginalRootLifetime());
    ASSERT_NE(nullptr, holder.bundle.OriginalArrangingLifetime());
    ASSERT_NE(nullptr, holder.bundle.OriginalSourceLifetime());
    ASSERT_NE(nullptr, holder.bundle.OriginalSourceBodyLifetime());
    EXPECT_EQ(episode.parent, holder.bundle.OriginalRootLifetime()->OriginalHandle());
    EXPECT_EQ(episode.arranging, holder.bundle.OriginalArrangingLifetime()->OriginalHandle());
    harken::DecisionToken laterDecision;
    auto laterRequest = episode.decisionRequest;
    laterRequest.ticket.sequence = engine.logicCommits.Issue();
    ASSERT_NE(0u, laterRequest.ticket.sequence);
    ASSERT_TRUE(engine.decisions.Reserve(laterRequest, &laterDecision));
    EXPECT_NE(laterDecision.slot, holder.originalCurrentSingleDecision.slot);
    EXPECT_EQ(episode.owner.singleDecision.slot, holder.originalCurrentSingleDecision.slot);
    EXPECT_EQ(episode.owner.singleDecision.generation, holder.originalCurrentSingleDecision.generation);
    ASSERT_TRUE(engine.decisions.Cancel(laterDecision));
    ASSERT_TRUE(engine.decisions.ReclaimCancelled(laterDecision));
    ASSERT_TRUE(episode.MatchesPrepared(holder.bundle));
    EXPECT_EQ(head, engine.commands.PublishedHead());
    EXPECT_EQ(completed, engine.mixer.BlocksRendered());
    EXPECT_FALSE(holder.bundle.ObserveInstalled());
    EXPECT_EQ(0u, engine.timelineArms.RowAt(episode.source.arm.slot)->startedBlockSequence.load());
    EXPECT_FALSE(JoinEngineOriginalCurrent(engine, episode));
    SingleBundle foreign;
    ASSERT_TRUE(engine.BorrowOriginalSingleCurrent(holder, &foreign));
    ASSERT_NO_FATAL_FAILURE(ExpectUnpublishedEpisodePhysicalCancel(engine, episode));
    EXPECT_FALSE(engine.playableInstances.CancelPrepared(&episode.originalChild));
    EXPECT_EQ(harken::Engine::OriginalSingleCurrentHolder::State::Ending, holder.state);
    EXPECT_FALSE(holder.bundle.Valid());
    EXPECT_TRUE(foreign.Valid());
    EXPECT_TRUE(foreign.HasOriginalPlayableRetention());
    EXPECT_FALSE(foreign.ObserveInstalled());
    EXPECT_TRUE(episode.originalChild.Valid());
    EXPECT_TRUE(episode.originalParent.Valid());
    EXPECT_FALSE(holder.ResetAfterNativeRetirement(engine));
    const auto originalArranging = holder.originalArranging;
    SingleBundle forbidden;
    EXPECT_FALSE(engine.BorrowOriginalSingleCurrent(holder, &forbidden));
    EXPECT_FALSE(engine.playableInstances.CancelPrepared(&episode.originalChild));
    EXPECT_EQ(harken::Engine::OriginalSingleCurrentHolder::State::Ending, holder.state);
    foreign.Release();
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_NO_FATAL_FAILURE(ExpectUnpublishedEpisodeOriginalTail(engine, episode));
    // A new genuine factory may already have issued the next generation into
    // this fixed slot before the old Ending holder is reset. Full identity,
    // rather than the presence of any new baseline, authenticates retirement.
    GenuinePreparedSingleEpisode next;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, next.source, samples, false));
    ASSERT_TRUE(next.Prepare(engine));
    ASSERT_EQ(static_cast<std::uint32_t>(originalArranging), static_cast<std::uint32_t>(next.arranging));
    ASSERT_TRUE(engine.IssueOriginalPlayableLifetimes(next.parentLinks));
    harken::Engine::OriginalSingleCurrentHolder* nextIssued = nullptr;
    ASSERT_TRUE(JoinEngineOriginalCurrent(engine, next, &nextIssued));
    EXPECT_EQ(&holder, nextIssued);
    EXPECT_NE(originalArranging, holder.originalArranging);
    EXPECT_EQ(next.arranging, holder.originalArranging);
    PlayableOriginal retainedNextChild;
    auto& nextChildBaseline = engine.originalPlayableLifetimeHolders[
        static_cast<std::uint32_t>(next.arranging)];
    ASSERT_TRUE(engine.playableInstances.IsOriginalLifetimePinFor(nextChildBaseline, next.arranging));
    ASSERT_TRUE(nextChildBaseline.Borrow(&retainedNextChild));
    ASSERT_NO_FATAL_FAILURE(ExpectUnpublishedEpisodePhysicalCancel(engine, next));
    EXPECT_FALSE(engine.playableInstances.CancelPrepared(&next.originalChild));
    EXPECT_TRUE(next.originalChild.Valid());
    EXPECT_EQ(harken::Engine::OriginalSingleCurrentHolder::State::Ending, holder.state);
    EXPECT_FALSE(holder.bundle.Valid());
    retainedNextChild.Release();
    ASSERT_NO_FATAL_FAILURE(ExpectUnpublishedEpisodeOriginalTail(engine, next));
    ASSERT_TRUE(holder.ResetAfterNativeRetirement(engine));
    EXPECT_EQ(harken::Engine::OriginalSingleCurrentHolder::State::Empty, holder.state);
}

TEST(OriginalSourceSubmission, EngineOriginalSingleRootDeduplicatesTheOneActualNativePlayableLoan) {
    const std::vector<float> samples(kRate, 0.25f);
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    GenuineEngineSingleRootEpisode episode;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, episode.source, samples));
    ASSERT_TRUE(episode.PrepareSingleRoot(engine));
    ASSERT_EQ(episode.parent, episode.arranging);
    ASSERT_TRUE(engine.IssueOriginalPlayableLifetimes(episode.parentLinks));
    ASSERT_TRUE(JoinEngineOriginalCurrent(engine, episode));
    auto& holder = CurrentHolder(engine, episode);
    using Access = harken::detail::PlayableInstanceTableTestAccess;
    EXPECT_EQ(2u, Access::OriginalLifetimeCount(engine.playableInstances, engine.originalPlayableLifetimeHolders[static_cast<std::uint32_t>(episode.parent)]));
    SingleBundle foreign;
    ASSERT_TRUE(engine.BorrowOriginalSingleCurrent(holder, &foreign));
    EXPECT_EQ(3u, Access::OriginalLifetimeCount(engine.playableInstances, engine.originalPlayableLifetimeHolders[static_cast<std::uint32_t>(episode.parent)]));
    ASSERT_NO_FATAL_FAILURE(ExpectUnpublishedEpisodePhysicalCancel(engine, episode));
    EXPECT_FALSE(engine.playableInstances.CancelPrepared(&episode.originalParent));
    EXPECT_EQ(2u, Access::OriginalLifetimeCount(engine.playableInstances, engine.originalPlayableLifetimeHolders[static_cast<std::uint32_t>(episode.parent)]));
    foreign.Release();
    EXPECT_EQ(1u, Access::OriginalLifetimeCount(engine.playableInstances, engine.originalPlayableLifetimeHolders[static_cast<std::uint32_t>(episode.parent)]));
    ASSERT_TRUE(engine.timelineOccurrences.CancelPreparedAcquire(episode.occurrence, &episode.originalOccurrence));
    ASSERT_TRUE(engine.decisions.CancelPreparedReservation(&episode.decisionPreparation, &episode.originalDecision));
    episode.originalLedger.Release(); episode.originalBody.Release(); episode.originalSource.Release();
    ASSERT_TRUE(engine.timelineArms.CancelPrepared(episode.source.arm));
    ASSERT_TRUE(engine.playableInstances.CancelPrepared(&episode.originalParent));
    ASSERT_TRUE(holder.ResetAfterNativeRetirement(engine));
    EXPECT_FALSE(foreign.Valid());
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, episode.source));
}

TEST(OriginalSourceSubmission, EngineOriginalSingleCurrentForeignReceiverCannotCloseAnotherEnginesNativeFamily) {
    const std::vector<float> samples(kRate, 0.25f);
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    GenuinePreparedSingleEpisode episode;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, episode.source, samples));
    ASSERT_TRUE(episode.Prepare(engine));
    ASSERT_TRUE(engine.IssueOriginalPlayableLifetimes(episode.parentLinks));
    ASSERT_TRUE(JoinEngineOriginalCurrent(engine, episode));
    auto& holder = CurrentHolder(engine, episode);
    harken::PlayableInstanceTable other(engine.playableInstances.Capacity());
    SingleBundle rejected;
    harken::Engine::DisposeOriginalPlayableAssociation(&engine, other, episode.arranging);
    EXPECT_EQ(harken::Engine::OriginalSingleCurrentHolder::State::Prepared, holder.state);
    EXPECT_TRUE(holder.bundle.Valid());
    EXPECT_EQ(nullptr, harken::Engine::ResolveOriginalPlayableBaseline(&engine, other, episode.arranging));
    harken::Engine::OriginalSingleCurrentHolder copiedShape;
    copiedShape.state = holder.state;
    copiedShape.originalTable = &engine.playableInstances;
    copiedShape.originalRoot = holder.originalRoot;
    copiedShape.originalArranging = holder.originalArranging;
    EXPECT_FALSE(engine.BorrowOriginalSingleCurrent(copiedShape, &rejected));
    EXPECT_FALSE(rejected.Valid());
    PlayableOriginal retainedChild;
    auto& originalChildBaseline = engine.originalPlayableLifetimeHolders[
        static_cast<std::uint32_t>(episode.arranging)];
    ASSERT_TRUE(engine.playableInstances.IsOriginalLifetimePinFor(originalChildBaseline, episode.arranging));
    ASSERT_TRUE(originalChildBaseline.Borrow(&retainedChild));
    ASSERT_NO_FATAL_FAILURE(ExpectUnpublishedEpisodePhysicalCancel(engine, episode));
    // The receiver negative control did not request any native disposition.
    // A genuine foreign child loan keeps this first native cancellation Busy
    // after its complete preflight withdraws only Engine's own Current loans.
    EXPECT_FALSE(engine.playableInstances.CancelPrepared(&episode.originalChild));
    EXPECT_TRUE(episode.originalChild.Valid());
    EXPECT_TRUE(retainedChild.Valid());
    EXPECT_EQ(harken::Engine::OriginalSingleCurrentHolder::State::Ending, holder.state);
    EXPECT_FALSE(holder.bundle.Valid());
    retainedChild.Release();
    ASSERT_NO_FATAL_FAILURE(ExpectUnpublishedEpisodeOriginalTail(engine, episode));
    EXPECT_EQ(harken::Engine::OriginalSingleCurrentHolder::State::Ending, holder.state);
    ASSERT_TRUE(holder.ResetAfterNativeRetirement(engine));
}
} // namespace


namespace {
TEST(OriginalSourceSubmission, EngineOriginalSingleCurrentEndPreservesActualSourceHoldTopologyAndStrictPhysicalFences) {
    const std::vector<float> samples(kRate, 0.25f);
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    GenuinePreparedSingleEpisode episode;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, episode.source, samples));
    ASSERT_TRUE(episode.Prepare(engine));
    ASSERT_TRUE(engine.IssueOriginalPlayableLifetimes(episode.parentLinks));
    ASSERT_TRUE(JoinEngineOriginalCurrent(engine, episode));
    auto& holder = CurrentHolder(engine, episode);
    SingleBundle foreign;
    ASSERT_TRUE(engine.BorrowOriginalSingleCurrent(holder, &foreign));
    const auto decision = episode.originalDecision.Token();
    harken::DecisionTable::PreparedTicketCommitProof decisionProof;
    const auto sequence = engine.logicCommits.Issue();
    ASSERT_NE(0u, sequence);
    ASSERT_TRUE(engine.decisions.ProvePreparedReservationCommit(episode.decisionPreparation,
        episode.decisionRequest, sequence, &decisionProof));
    harken::DecisionTable::ConsumePreparedReservationCommit(std::move(decisionProof));
    harken::PlayableInstanceTable::ConsumePreparedCommit(std::move(episode.parentProof));
    harken::PlayableInstanceTable::ConsumePreparedCommit(std::move(episode.childProof));
    harken::PlayableInstanceTable::ConsumePreparedParentLinks(std::move(episode.parentLinks));
    ASSERT_TRUE(engine.playableInstances.AcquireSourceRetirementHold(episode.arranging,
        engine.playableInstances.Classify(episode.arranging).scheduleEpoch));
    harken::OccurrenceStatePool::ConsumePreparedSingleCompletionCommit(std::move(episode.occurrenceProof));
    harken::VoiceLedger::ConsumePreparedSingleCompletionCommit(std::move(episode.ledgerProof));
    harken::VoicePool::ConsumePreparedCommit(std::move(episode.voiceProof));
    harken::TimelineArmTable::ConsumePreparedCommit(std::move(episode.armProof));
    engine.originalSourceSubmissions.PublishPreparedChild(episode.commands[0]);
    (void)engine.commands.ConsumePreparedBatch(std::move(episode.batch));
    const auto before = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    ASSERT_EQ(before + 1u, engine.timelineArms.RowAt(episode.source.arm.slot)->armedCompletionSequence.load());
    ASSERT_EQ(0u, engine.timelineArms.RowAt(episode.source.arm.slot)->startedBlockSequence.load());
    ASSERT_TRUE(holder.bundle.ObserveInstalled());
    ASSERT_TRUE(holder.bundle.HasOriginalPlayableRetention());
    ASSERT_TRUE(engine.timelineArms.RequestCancel(episode.source.arm));
    harken::CommandRecord stop{};
    stop.opcode = harken::kOpcodeStop;
    stop.payload.stop.handle = episode.source.source;
    ASSERT_TRUE(engine.commands.TryPush(stop));
    const auto stopBefore = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(stopBefore + 1u, engine.mixer.BlocksRendered());
    ASSERT_TRUE(engine.voices.ReleaseObserved(episode.source.release));
    EXPECT_FALSE(foreign.ObserveInstalled());
    engine.ledger.Reap(episode.source.source);
    ASSERT_TRUE(engine.timelineOccurrences.ClearCompletionOwnerExact(episode.occurrence, episode.owner));
    harken::CompletionProgressSnapshot terminal{};
    ASSERT_TRUE(harken::CaptureCompletionProgressExact(*episode.progress, episode.occurrence,
        static_cast<std::uint32_t>(episode.source.source >> 32), &terminal));
    ASSERT_TRUE(engine.timelineOccurrences.UnbindCompletionProgress(episode.occurrence,
        static_cast<std::uint32_t>(episode.source.source >> 32)));
    ASSERT_TRUE(engine.timelineOccurrences.Advance(episode.occurrence, harken::OccurrenceStage::AwaitingEventFence));
    ASSERT_TRUE(engine.decisions.Cancel(decision));
    ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(episode.arranging, harken::PlayableLifecycle::LogicalComplete));
    ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(episode.arranging, harken::PlayableLifecycle::AudibleRetiring));
    EXPECT_FALSE(engine.playableInstances.AdvanceLifecycle(episode.parent, harken::PlayableLifecycle::Retired));
    EXPECT_EQ(harken::Engine::OriginalSingleCurrentHolder::State::Prepared, holder.state);
    EXPECT_TRUE(holder.bundle.Valid());
    EXPECT_FALSE(engine.playableInstances.AdvanceLifecycle(episode.arranging, harken::PlayableLifecycle::Retired));
    EXPECT_EQ(harken::Engine::OriginalSingleCurrentHolder::State::Prepared, holder.state);
    EXPECT_TRUE(holder.bundle.Valid());
    ASSERT_TRUE(engine.playableInstances.ReleaseSourceRetirementHold(episode.arranging));
    EXPECT_FALSE(engine.playableInstances.AdvanceLifecycle(episode.arranging, harken::PlayableLifecycle::Retired));
    EXPECT_EQ(harken::Engine::OriginalSingleCurrentHolder::State::Ending, holder.state);
    EXPECT_FALSE(holder.bundle.Valid());
    EXPECT_TRUE(foreign.Valid());
    EXPECT_TRUE(foreign.HasOriginalPlayableRetention());
    EXPECT_EQ(episode.arranging, engine.playableInstances.FirstChild(episode.parent));
    EXPECT_TRUE(engine.playableInstances.Classify(episode.arranging).live);
    EXPECT_FALSE(engine.timelineOccurrences.ReleaseAfterFence(episode.occurrence,
        engine.mixer.BlocksRendered(), &episode.originalOccurrence));
    EXPECT_FALSE(engine.decisions.ReclaimCancelled(decision, &episode.originalDecision));
    foreign.Release();
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FALSE(engine.playableInstances.Classify(episode.arranging).live);
    EXPECT_FALSE(engine.playableInstances.Classify(episode.parent).live);
    EXPECT_FALSE(engine.playableInstances.AdvanceLifecycle(episode.arranging, harken::PlayableLifecycle::Retired));
    EXPECT_FALSE(engine.playableInstances.AdvanceLifecycle(episode.parent, harken::PlayableLifecycle::Retired));
    ASSERT_TRUE(holder.ResetAfterNativeRetirement(engine));
    EXPECT_FALSE(engine.timelineOccurrences.ReleaseAfterFence(episode.occurrence,
        terminal.completedThroughBlockSequence, &episode.originalOccurrence));
    episode.originalLedger.Release(); episode.originalBody.Release(); episode.originalSource.Release();
    const auto afterStop = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(afterStop + 1u, engine.mixer.BlocksRendered());
    ASSERT_GT(engine.mixer.BlocksRendered(), terminal.completedThroughBlockSequence);
    ASSERT_TRUE(engine.timelineOccurrences.ReleaseAfterFence(episode.occurrence,
        engine.mixer.BlocksRendered(), &episode.originalOccurrence));
    ASSERT_TRUE(engine.decisions.ReclaimCancelled(decision, &episode.originalDecision));
    ASSERT_TRUE(engine.timelineArms.Reclaim(episode.source.arm, engine.mixer.BlocksRendered()));
    EXPECT_EQ(episode.source.armFreeBefore, engine.timelineArms.FreeCount());
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, episode.source));
}

TEST(OriginalSourceSubmission, EngineOriginalSingleCurrentActualTermJoinsBeforeReturningOwnAssociationAndBaselines) {
    const std::vector<float> samples(kRate, 0.25f);
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    GenuinePreparedSingleEpisode episode;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, episode.source, samples));
    ASSERT_TRUE(episode.Prepare(engine));
    ASSERT_TRUE(engine.IssueOriginalPlayableLifetimes(episode.parentLinks));
    ASSERT_TRUE(JoinEngineOriginalCurrent(engine, episode));
    auto& holder = CurrentHolder(engine, episode);
    SingleBundle foreign;
    ASSERT_TRUE(engine.BorrowOriginalSingleCurrent(holder, &foreign));
    foreign.Release();
    episode.ReleaseProofs();
    const auto unpublishedHead = engine.commands.PublishedHead();
    // Return the caller's genuine frozen short proof while its Queue is alive.
    // The Engine still owns the prepared Current association and physical Source.
    ASSERT_TRUE(engine.originalSourceSubmissions.CancelBoundPreparation(episode.source.arm, episode.batch));
    EXPECT_FALSE(episode.batch.Valid());
    EXPECT_EQ(unpublishedHead, engine.commands.PublishedHead());
    EXPECT_NE(nullptr, engine.voices.Resolve(episode.source.source));
    EXPECT_FALSE(engine.voices.ReleaseObserved(episode.source.release));
    episode.originalLedger.Release(); episode.originalOccurrence.Release(); episode.originalDecision.Release();
    episode.originalBody.Release(); episode.originalSource.Release();
    ASSERT_TRUE(holder.bundle.Valid());
    ASSERT_TRUE(holder.bundle.HasOriginalPlayableRetention());
    ASSERT_EQ(harken::Engine::OriginalSingleCurrentHolder::State::Prepared, holder.state);
    // Real Engine Term executes callback/worker join, then the association
    // Return and fixed baselines, before native Source/Playable destruction.
    ASSERT_EQ(VN_OK, Vn_Term());
    session.Release();
    EXPECT_EQ(nullptr, harken::detail::ActiveEngine());
}
} // namespace

namespace {

using BoundClock = harken::QueueInstanceTable::OriginalConditionalLaneClock;
using BoundClockState = harken::QueueInstanceTable::OriginalConditionalLaneClockState;
using BoundClockObservation = harken::QueueInstanceTable::OriginalConditionalInitialClockBinding;

// This fixture issues every owner from the actual Engine's native factories.
// It deliberately keeps the Lane private: no Controller receipt, published
// future selection or whole Queue Reset qualification is claimed here.
struct GenuineInitialSingleClockHarness {
    harken::Engine& engine;
    GenuinePreparedSingleEpisode episode;
    harken::PreparedQueueInstanceV1 preparedQueue;
    harken::QueueInstanceToken queue{};
    harken::QueueInstanceTable::OriginalLifetimePin parent;
    harken::PreparedQueueConditionalV1 preparedConditional;
    harken::QueueInstanceTable::OriginalConditionalFutureReservation original;
    harken::ConditionalQueueVersionToken version{};
    BoundClock clock;
    harken::PrivateImmediateLanePlacementProof placement;
    harken::PrivateOriginalQueueGroupClockProof privateClock;
    harken::ImmediateLaneHandle lane{};
    std::array<harken::DecisionToken, 4> decisions{};
    std::array<harken::ArmGroupToken, 4> armGroups{};
    std::array<harken::Plan03GroupSliceInput, 4> inputs{};
    std::array<harken::Plan03GroupSliceIdentity, 4> groups{};
    harken::TimelineEventPayload payload{};
    harken::TimelineProgramView program{};
    harken::ImmediateLaneEvent start{};
    harken::KnownCompletionPredicateV1 originalCompletion{};
    std::uint64_t anchor = UINT64_MAX;
    std::uint64_t installed = 0;
    std::uint64_t logicSequence = 0;
    std::uint64_t terminalSequence = 0;
    std::uint64_t cohort = 17;
    bool sharedEngine = false;
    static constexpr std::uint64_t provisionalAnchor = 100;
    static constexpr std::uint64_t fingerprint = 53;

    explicit GenuineInitialSingleClockHarness(harken::Engine& receiver) : engine(receiver) {}

    const harken::OriginalQueueFutureGroupRegistration& Original(std::uint32_t index) const {
        return engine.originalQueueGroupHolders[groups[index].committedRow];
    }

    void PrepareBeforeHead(std::uint64_t expectedFrames = kFrames) {
        ASSERT_TRUE(episode.Prepare(engine, false, false, true));
        ASSERT_TRUE(episode.Join(engine, &episode.bundle));
        ASSERT_FALSE(episode.bundle.ObserveInstalled());
        logicSequence = engine.logicCommits.Issue();
        ASSERT_NE(0u, logicSequence);
        harken::QueueInstanceCreateRequest create{};
        create.world = episode.decisionRequest.world;
        create.rootInstance = episode.parent;
        create.containerStableId = harkentest::ObjectId(13);
        create.programSemanticFingerprint = fingerprint;
        create.childCount = 2;
        create.initial.version = 1;
        create.resetInitial = create.initial;
        ASSERT_TRUE(engine.PrepareOriginalQueueAcquire(create, &preparedQueue));
        queue = preparedQueue.Token();
        ASSERT_TRUE(engine.originalQueueLifetimeHolders[queue.slot].Borrow(&parent));
        ASSERT_TRUE(engine.queueInstances.CommitPreparedAcquire(&preparedQueue, &queue));

        harken::KnownCompletionFactsV1 facts{};
        const auto pcm = engine.registry.Lookup(kSound);
        ASSERT_TRUE(pcm.Valid());
        facts.occurrence = episode.occurrence;
        facts.renderGeneration = static_cast<std::uint32_t>(episode.source.source >> 32);
        facts.residentExactMedia = true;
        facts.pcmFrameCount = pcm.frames;
        facts.loopEndFrame = pcm.frames;
        facts.loopCount = 1;
        facts.sourceRate = kRate;
        facts.mixRate = engine.config.mixRate.SampleRate();
        facts.pitchFixed = true;
        facts.serialRender = true;
        facts.absoluteLogicalStartFrame = 0;
        facts.playbackFactsRevision = episode.owner.factsRevision;
        facts.program.kind = harken::PlayableKindV1::Sound;
        facts.program.stableId = kSound;
        facts.program.world = episode.decisionRequest.world;
        facts.program.runtimeSemanticFingerprint = fingerprint;
        harken::KnownCompletionPredicateV1 candidate{};
        ASSERT_EQ(harken::CompletionPredictability::KnownFinite,
                  harken::MaterializeKnownCompletion(facts, &candidate));
        ASSERT_TRUE(harken::BindKnownCompletionPredicate(candidate, facts.program,
                                                       episode.owner, &originalCompletion));
        ASSERT_EQ(expectedFrames, originalCompletion.absoluteLogicalCompleteFrame);
        harken::ConditionalQueuePrepareRequest request{};
        request.expectedCurrentVersion = create.initial.version;
        request.rootDecision = episode.originalDecision.Token();
        request.expectedCurrentOccurrence = episode.occurrence;
        request.successorOccurrence = episode.occurrence;
        request.completion = originalCompletion;
        request.completionOwner = episode.owner;
        request.next = create.initial;
        request.next.version = 2;
        request.next.currentChildOrdinal = 1;
        request.next.selectionOrdinal = 1;
        ASSERT_TRUE(engine.queueInstances.PrepareConditional(queue, request,
            &preparedConditional, parent, &original));
        ASSERT_TRUE(engine.queueInstances.CommitPreparedConditional(&preparedConditional, &version));
        decisions[3] = episode.originalDecision.Token();
        for (std::uint32_t index = 0; index < 3; ++index) {
            harken::DecisionReserveRequest decision{};
            decision.ticket.sequence = logicSequence;
            decision.world = create.world;
            decision.transport = engine.timelinePool.Epoch();
            decision.expectedContinuationGeneration = 11;
            decision.sliceCount = 1;
            ASSERT_TRUE(engine.decisions.Reserve(decision, &decisions[index]));
        }
    }

    void ReserveUnboundGroups(std::uint64_t conditionalRelative = kFrames) {
        for (std::uint32_t index = 0; index < groups.size(); ++index) {
            inputs[index].executeFrame = provisionalAnchor + (index < 2 ? 0 : conditionalRelative);
            inputs[index].decision = decisions[index];
            ASSERT_TRUE(engine.ReserveOriginalQueueLaneGroup(cohort, provisionalAnchor,
                inputs[index], engine.timelineArms, &groups[index]));
            ASSERT_TRUE(Original(index).MatchesOriginalScope(cohort, provisionalAnchor,
                inputs[index], engine.timelineArms));
        }
        ASSERT_TRUE(engine.queueInstances.PrepareOriginalConditionalLaneClock(original,
            parent, engine.timelinePool, Original(1), Original(3), &clock));
        BoundClockState state = BoundClockState::Retired;
        ASSERT_TRUE(clock.CaptureState(&state));
        ASSERT_EQ(BoundClockState::Unbound, state);
        EXPECT_EQ(0u, engine.timelineArms.RowAt(episode.source.arm.slot)->armedCompletionSequence.load());
        EXPECT_EQ(0u, engine.timelineArms.RowAt(episode.source.arm.slot)->startedBlockSequence.load());
    }

    void PublishSourceAndObserveFreshAck() {
        harken::DecisionTable::PreparedTicketCommitProof proof;
        const auto sequence = logicSequence;
        ASSERT_NE(0u, sequence);
        ASSERT_TRUE(engine.decisions.ProvePreparedReservationCommit(episode.decisionPreparation,
            episode.decisionRequest, sequence, &proof));
        harken::DecisionTable::ConsumePreparedReservationCommit(std::move(proof));
        harken::PlayableInstanceTable::ConsumePreparedCommit(std::move(episode.parentProof));
        harken::PlayableInstanceTable::ConsumePreparedCommit(std::move(episode.childProof));
        harken::PlayableInstanceTable::ConsumePreparedParentLinks(std::move(episode.parentLinks));
        harken::OccurrenceStatePool::ConsumePreparedSingleCompletionCommit(std::move(episode.occurrenceProof));
        harken::VoiceLedger::ConsumePreparedSingleCompletionCommit(std::move(episode.ledgerProof));
        harken::VoicePool::ConsumePreparedCommit(std::move(episode.voiceProof));
        harken::TimelineArmTable::ConsumePreparedCommit(std::move(episode.armProof));
        const auto before = engine.mixer.BlocksRendered();
        const auto head = engine.commands.PublishedHead();
        ASSERT_EQ(0u, engine.timelineArms.RowAt(episode.source.arm.slot)->armedCompletionSequence.load());
        engine.originalSourceSubmissions.PublishPreparedChild(episode.commands[0]);
        (void)engine.commands.ConsumePreparedBatch(std::move(episode.batch));
        ASSERT_GT(engine.commands.PublishedHead(), head);
        EXPECT_FALSE(episode.bundle.ObserveInstalled());
        engine.nullBackend.Pump(1);
        ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
        harken::TimelineArmObservation observed{};
        ASSERT_TRUE(engine.timelineArms.Observe(episode.source.arm, &observed));
        ASSERT_EQ(before + 1u, observed.armedCompletionSequence);
        ASSERT_GT(observed.armedCompletionSequence, before);
        ASSERT_EQ(0u, observed.startedBlockSequence);
        ASSERT_EQ(0u, observed.logicalCompletionSequence);
        installed = observed.armedCompletionSequence;
        ASSERT_TRUE(episode.bundle.ObserveInstalled());
    }

    void PlaceAndHold(bool initialOwnsQueue = true, bool genuineTicketOrder = true) {
        ASSERT_TRUE(engine.immediateActionLane.Reserve(cohort, {1, 4, 0, 2}, &lane));
        for (std::uint32_t index = 0; index < groups.size(); ++index) {
            ASSERT_TRUE(engine.timelinePool.BindImmediateLaneGroupOwner(groups[index],
                engine.immediateActionLane, cohort, lane.slot, lane.generation));
            ASSERT_TRUE(engine.immediateActionLane.DeclareGroup(lane,
                {groups[index], inputs[index].executeFrame - provisionalAnchor}));
        }
        const auto root = engine.playableInstances.Classify(episode.parent);
        ASSERT_TRUE(root.live);
        ASSERT_EQ(harken::PlayableLifecycle::Active, root.lifecycle);
        harken::FrontierOwnerIdentity owner{};
        owner.child = episode.parent;
        owner.childEpoch = root.scheduleEpoch;
        owner.invocation = episode.parent;
        owner.invocationEpoch = root.scheduleEpoch;
        owner.serviceSlot = 0;
        owner.serviceGeneration = 1;
        payload.kind = harken::TimelineEventKind::Start;
        payload.target.subjectId = kSound;
        program = {17, &payload, 1, harken::TimelineProgramRole::Voice};
        start.event.program = {17, &program};
        start.event.armToken = episode.source.arm;
        start.event.subeventPhase = 20;
        start.event.logicCommitSequence = logicSequence;
        start.event.ownerInstanceHandle = episode.parent;
        start.event.ownerInstanceScheduleEpoch = root.scheduleEpoch;
        start.event.frontier.kind = harken::FrontierOperationKind::Entry;
        start.event.frontier.entry = {decisions[0], 1, owner};
        start.event.frontier.owner = owner;
        start.event.frontier.group = groups[0];
        start.event.frontier.transport = engine.timelinePool.Epoch();
        start.event.frontier.world = episode.decisionRequest.world;
        start.event.frontier.operationEventCount = 1;
        start.event.frontier.entryEventOrdinal = 0;
        ASSERT_TRUE(engine.immediateActionLane.Prepare(lane, &start, 1));
        ASSERT_TRUE(engine.immediateActionLane.TryPlace(lane, 1024, 0, &anchor, &placement));
        ASSERT_EQ(1024u, anchor);
        ASSERT_TRUE(engine.timelinePool.RebaseImmediateLaneGroups(cohort, anchor));
        for (std::uint32_t index = 0; index < groups.size(); ++index) {
            const auto primary = index < 2 ? 0u : 2u;
            harken::FrontierGroupBinding binding{};
            binding.role = index == primary ? harken::FrontierGroupRole::Coordinator :
                harken::FrontierGroupRole::EntryMember;
            binding.entry = {decisions[primary], primary + 1u, owner};
            binding.operation.kind = index == 3 ? harken::FrontierOperationKind::Dependent :
                harken::FrontierOperationKind::Entry;
            binding.operation.owner = owner;
            binding.operation.order.logicCommitSequence = genuineTicketOrder ? logicSequence : logicSequence + 1u;
            binding.operation.order.executeFrame = anchor + inputs[index].executeFrame - provisionalAnchor;
            binding.operation.group = groups[index];
            binding.operation.transport = engine.timelinePool.Epoch();
            binding.operation.world = episode.decisionRequest.world;
            binding.primary = groups[primary];
            if (index == 1) binding.queueInitial = {queue.slot, queue.generation, 1, fingerprint};
            if (index == 3) binding.queueConditional = {queue.slot, queue.generation,
                1, version.version, episode.occurrence, episode.occurrence};
            ASSERT_TRUE(engine.timelinePool.BindPlan03GroupFrontier(groups[index], binding));
            harken::Plan03PageGroupView actual{};
            ASSERT_TRUE(engine.timelinePool.ResolvePlan03GroupSliceProducer(groups[index], &actual));
            ASSERT_NE(nullptr, actual.frontier);
            ASSERT_TRUE(engine.armGroups.Reserve(decisions[index], nullptr, 0, &armGroups[index]));
            harken::DecisionParticipantRef participant{};
            participant.kind = harken::DecisionParticipantKind::QueueInstance;
            participant.slot = queue.slot;
            participant.generation = queue.generation;
            harken::DecisionCommitPayload commit{};
            commit.pages = {static_cast<std::uint32_t>(groups[index].committedRow), groups[index].rowClaimGeneration};
            commit.armGroup = armGroups[index];
            if (index == 3 || (index == 1 && initialOwnsQueue)) {
                commit.participants = &participant;
                commit.participantCount = 1;
            }
            ASSERT_TRUE(engine.decisions.Commit(decisions[index], commit));
            if (index == 1 || index == 3)
                ASSERT_TRUE(engine.decisions.BindFrontierDecision(decisions[index],
                    *actual.frontier, engine.timelinePool));
            if (index == 1)
                ASSERT_TRUE(engine.queueInstances.BindFrontierInitial(queue, *actual.frontier, engine.timelinePool));
            if (index == 3)
                ASSERT_TRUE(engine.queueInstances.BindFrontierConditional(queue, *actual.frontier, engine.timelinePool));
        }
        ASSERT_TRUE(engine.timelineArms.BindPendingImmediateStart(episode.source.arm, anchor, anchor));
        ASSERT_TRUE(engine.immediateActionLane.PinArms(lane, engine.timelineArms));
        ASSERT_TRUE(engine.immediateActionLane.PinGroups(lane, engine.timelinePool,
            engine.decisions, engine.config.maxPlan03GroupHeadersPerPage));
        harken::Plan03PageGroupView initial{};
        ASSERT_TRUE(engine.timelinePool.ResolvePlan03GroupSliceProducer(groups[0], &initial));
        ASSERT_TRUE(engine.timelineArms.BindFrontierAdmission(episode.source.arm, *initial.frontier, *initial.lease));
        ASSERT_TRUE(engine.timelinePool.ProvePrivateOriginalQueueGroupClock(Original(1), Original(3),
            &placement, &privateClock));
        ASSERT_FALSE(placement.Valid());
        ASSERT_TRUE(privateClock.Valid());
    }

    bool Bind() {
        return engine.BindInitialOriginalSingleQueueClock(original, parent, clock,
                                                          privateClock, episode.bundle);
    }

    void EndAndRetireSource() {
        privateClock.Release();
        placement.Release();
        ASSERT_TRUE(engine.queueInstances.RetireConditionalForAbort(queue, version, decisions[3]));
        BoundClockState state = BoundClockState::Unbound;
        ASSERT_TRUE(clock.CaptureState(&state));
        ASSERT_EQ(BoundClockState::Retired, state);
        ASSERT_TRUE(engine.timelineArms.RequestCancel(episode.source.arm));
        harken::CommandRecord stop{};
        stop.opcode = harken::kOpcodeStop;
        stop.payload.stop.handle = episode.source.source;
        ASSERT_TRUE(engine.commands.TryPush(stop));
        const auto before = engine.mixer.BlocksRendered();
        engine.nullBackend.Pump(1);
        ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
        ASSERT_TRUE(engine.voices.ReleaseObserved(episode.source.release));
        EXPECT_EQ(nullptr, engine.voices.Resolve(episode.source.source));
        EXPECT_FALSE(episode.bundle.ObserveInstalled());
        ASSERT_TRUE(engine.immediateActionLane.Cancel(lane));
        ASSERT_TRUE(engine.immediateActionLane.Retire(lane, engine.mixer.BlocksRendered()));
        for (std::uint32_t index = groups.size(); index != 0; --index)
            ASSERT_TRUE(engine.timelinePool.DetachPlan03GroupFrontier(groups[index - 1],
                engine.mixer.BlocksRendered()));
        ASSERT_TRUE(engine.decisions.ReleaseFrontierDecisionBinding(decisions[1]));
        ASSERT_TRUE(engine.decisions.ReleaseFrontierDecisionBinding(decisions[3]));
        engine.ledger.Reap(episode.source.source);
        ASSERT_TRUE(engine.timelineOccurrences.ClearCompletionOwnerExact(episode.occurrence, episode.owner));
        harken::CompletionProgressSnapshot terminal{};
        ASSERT_TRUE(harken::CaptureCompletionProgressExact(*episode.progress, episode.occurrence,
            static_cast<std::uint32_t>(episode.source.source >> 32), &terminal));
        terminalSequence = terminal.completedThroughBlockSequence;
        ASSERT_TRUE(engine.timelineOccurrences.UnbindCompletionProgress(episode.occurrence,
            static_cast<std::uint32_t>(episode.source.source >> 32)));
        ASSERT_TRUE(engine.timelineOccurrences.Advance(episode.occurrence, harken::OccurrenceStage::AwaitingEventFence));
    }

    void ReturnAllAndReclaim() {
        clock.Release();
        original.Release();
        parent.Release();
        ASSERT_TRUE(engine.CancelOriginalQueueLaneGroups(cohort, engine.mixer.BlocksRendered()));
        for (std::uint32_t index = 0; index < groups.size(); ++index)
            EXPECT_FALSE(Original(index).Valid());
        episode.bundle.Release();
        episode.originalLedger.Release();
        episode.originalBody.Release();
        episode.originalSource.Release();
        EXPECT_FALSE(engine.timelineOccurrences.ReleaseAfterFence(episode.occurrence,
            terminalSequence, &episode.originalOccurrence));
        const auto before = engine.mixer.BlocksRendered();
        engine.nullBackend.Pump(1);
        ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
        ASSERT_GT(engine.mixer.BlocksRendered(), terminalSequence);
        ASSERT_TRUE(engine.timelineOccurrences.ReleaseAfterFence(episode.occurrence,
            engine.mixer.BlocksRendered(), &episode.originalOccurrence));
        ASSERT_TRUE(engine.timelineArms.Reclaim(episode.source.arm, engine.mixer.BlocksRendered()));
        if (!sharedEngine) {
            EXPECT_EQ(episode.source.armFreeBefore, engine.timelineArms.FreeCount());
            EXPECT_EQ(0u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
        }
        for (std::uint32_t index = 0; index < decisions.size(); ++index) {
            const auto result = engine.decisions.Invalidate(decisions[index]);
            ASSERT_TRUE(result == harken::DecisionInvalidationDisposition::NewlyInvalidated ||
                        result == harken::DecisionInvalidationDisposition::AlreadyInvalidated);
            ASSERT_TRUE(engine.decisions.Retire(decisions[index], engine.mixer.BlocksRendered()));
            ASSERT_TRUE(engine.decisions.ReclaimRetired(decisions[index],
                index == 3 ? &episode.originalDecision : nullptr));
            ASSERT_TRUE(engine.armGroups.BeginRetire(armGroups[index]));
            ASSERT_TRUE(engine.armGroups.Reclaim(armGroups[index], engine.mixer.BlocksRendered()));
        }
        ASSERT_TRUE(engine.ReleaseOriginalQueue(queue));
        ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(episode.arranging, harken::PlayableLifecycle::Retired));
        ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(episode.parent, harken::PlayableLifecycle::Retired));
        if (!sharedEngine) {
            ASSERT_EQ(VN_OK, engine.registry.UnregisterPCM(kSound));
            EXPECT_EQ(0u, engine.decisions.LiveCount());
        }
    }
};

harken::EngineConfig InitialSingleClockConfig() {
    harken::EngineConfig result{};
    const auto config = SourceBodyConfig();
    EXPECT_EQ(VN_OK, harken::ResolveConfig(&config, &result));
    return result;
}

void RegisterInitialClockPcm(harken::Engine& engine, std::uint32_t frames = kFrames) {
    const std::vector<float> samples(frames, 0.25f);
    const auto pcm = harkentest::MonoDesc(samples, kRate);
    ASSERT_EQ(VN_OK, engine.registry.RegisterPCM(kSound, &pcm));
}

TEST(OriginalSourceSubmission, InitialSingleClockUsesGenuineUnboundBeforeHeadAndFreshAckForExactFirstS) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto& engine = *owned;
    ASSERT_NO_FATAL_FAILURE(RegisterInitialClockPcm(engine));
    GenuineInitialSingleClockHarness state(engine);
    const auto head = engine.commands.PublishedHead();
    ASSERT_NO_FATAL_FAILURE(state.PrepareBeforeHead());
    ASSERT_NO_FATAL_FAILURE(state.ReserveUnboundGroups());
    EXPECT_EQ(head, engine.commands.PublishedHead());
    EXPECT_EQ(0u, engine.mixer.BlocksRendered());
    EXPECT_FALSE(state.Bind());
    BoundClockObservation before{};
    before.anchorFrame = 71;
    before.installedAckSequence = 73;
    EXPECT_FALSE(state.clock.CaptureInitialBinding(&before));
    EXPECT_EQ(71u, before.anchorFrame);
    EXPECT_EQ(73u, before.installedAckSequence);
    ASSERT_NO_FATAL_FAILURE(state.PublishSourceAndObserveFreshAck());
    ASSERT_NO_FATAL_FAILURE(state.PlaceAndHold());
    ASSERT_TRUE(state.Bind());
    BoundClockState clockState = BoundClockState::Unbound;
    ASSERT_TRUE(state.clock.CaptureState(&clockState));
    EXPECT_EQ(BoundClockState::Bound, clockState);
    BoundClockObservation actual{};
    ASSERT_TRUE(state.clock.CaptureInitialBinding(&actual));
    EXPECT_EQ(state.anchor, actual.anchorFrame);
    EXPECT_EQ(state.anchor, actual.initialGroupFrame);
    EXPECT_EQ(state.anchor + kFrames, actual.conditionalGroupFrame);
    EXPECT_EQ(state.anchor, actual.sourceStartFrame);
    EXPECT_EQ(state.installed, actual.installedAckSequence);
    EXPECT_TRUE(Registry::SameToken(state.episode.source.arm, actual.sourceArm));
    EXPECT_EQ(0u, engine.timelineArms.RowAt(actual.sourceArm.slot)->startedBlockSequence.load());
    EXPECT_FALSE(state.Bind());
    ASSERT_NO_FATAL_FAILURE(state.EndAndRetireSource());
    ASSERT_NO_FATAL_FAILURE(state.ReturnAllAndReclaim());
}

TEST(OriginalSourceSubmission, InitialSingleClockRejectsSameNumericScopesFromAnotherActualEngine) {
    const auto config = InitialSingleClockConfig();
    auto firstOwner = std::make_unique<harken::Engine>(config, 0, 2);
    auto secondOwner = std::make_unique<harken::Engine>(config, 0, 2);
    auto& first = *firstOwner;
    auto& second = *secondOwner;
    ASSERT_NO_FATAL_FAILURE(RegisterInitialClockPcm(first));
    ASSERT_NO_FATAL_FAILURE(RegisterInitialClockPcm(second));
    GenuineInitialSingleClockHarness a(first), b(second);
    ASSERT_NO_FATAL_FAILURE(a.PrepareBeforeHead());
    ASSERT_NO_FATAL_FAILURE(b.PrepareBeforeHead());
    ASSERT_NO_FATAL_FAILURE(a.ReserveUnboundGroups());
    ASSERT_NO_FATAL_FAILURE(b.ReserveUnboundGroups());
    ASSERT_NO_FATAL_FAILURE(a.PublishSourceAndObserveFreshAck());
    ASSERT_NO_FATAL_FAILURE(b.PublishSourceAndObserveFreshAck());
    ASSERT_NO_FATAL_FAILURE(a.PlaceAndHold());
    ASSERT_NO_FATAL_FAILURE(b.PlaceAndHold());
    ASSERT_EQ(a.queue.slot, b.queue.slot);
    ASSERT_EQ(a.queue.generation, b.queue.generation);
    ASSERT_EQ(a.episode.occurrence.slot, b.episode.occurrence.slot);
    ASSERT_EQ(a.episode.occurrence.generation, b.episode.occurrence.generation);
    ASSERT_EQ(a.decisions[3].slot, b.decisions[3].slot);
    ASSERT_EQ(a.decisions[3].generation, b.decisions[3].generation);
    ASSERT_TRUE(Registry::SameToken(a.episode.source.arm, b.episode.source.arm));
    EXPECT_FALSE(first.BindInitialOriginalSingleQueueClock(a.original, a.parent,
        a.clock, a.privateClock, b.episode.bundle));
    EXPECT_FALSE(first.BindInitialOriginalSingleQueueClock(b.original, b.parent,
        b.clock, b.privateClock, a.episode.bundle));
    EXPECT_FALSE(first.BindInitialOriginalSingleQueueClock(a.original, a.parent,
        a.clock, b.privateClock, a.episode.bundle));
    EXPECT_FALSE(first.BindInitialOriginalSingleQueueClock(a.original, b.parent,
        a.clock, a.privateClock, a.episode.bundle));
    EXPECT_TRUE(a.privateClock.Valid());
    EXPECT_TRUE(b.privateClock.Valid());
    ASSERT_TRUE(a.Bind());
    ASSERT_TRUE(b.Bind());
    ASSERT_NO_FATAL_FAILURE(a.EndAndRetireSource());
    ASSERT_NO_FATAL_FAILURE(b.EndAndRetireSource());
    ASSERT_NO_FATAL_FAILURE(a.ReturnAllAndReclaim());
    ASSERT_NO_FATAL_FAILURE(b.ReturnAllAndReclaim());
}

TEST(OriginalSourceSubmission, InitialSingleClockRejectsActualOriginalCompletionGeometryWithoutMutatingOutput) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto& engine = *owned;
    ASSERT_NO_FATAL_FAILURE(RegisterInitialClockPcm(engine));
    GenuineInitialSingleClockHarness state(engine);
    ASSERT_NO_FATAL_FAILURE(state.PrepareBeforeHead());
    ASSERT_NO_FATAL_FAILURE(state.ReserveUnboundGroups(kFrames + 1u));
    ASSERT_NO_FATAL_FAILURE(state.PublishSourceAndObserveFreshAck());
    ASSERT_NO_FATAL_FAILURE(state.PlaceAndHold());
    EXPECT_FALSE(state.Bind());
    BoundClockState actualState = BoundClockState::Retired;
    ASSERT_TRUE(state.clock.CaptureState(&actualState));
    EXPECT_EQ(BoundClockState::Unbound, actualState);
    BoundClockObservation sentinel{};
    sentinel.sourceStartFrame = 79;
    sentinel.conditionalGroupFrame = 83;
    EXPECT_FALSE(state.clock.CaptureInitialBinding(&sentinel));
    EXPECT_EQ(79u, sentinel.sourceStartFrame);
    EXPECT_EQ(83u, sentinel.conditionalGroupFrame);
    EXPECT_TRUE(state.privateClock.Valid());
    EXPECT_TRUE(state.episode.bundle.ObserveInstalled());
    ASSERT_NO_FATAL_FAILURE(state.EndAndRetireSource());
    ASSERT_NO_FATAL_FAILURE(state.ReturnAllAndReclaim());
}

TEST(OriginalSourceSubmission, InitialSingleClockRequiresActualC0ExclusiveQueueParticipant) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto& engine = *owned;
    ASSERT_NO_FATAL_FAILURE(RegisterInitialClockPcm(engine));
    GenuineInitialSingleClockHarness state(engine);
    ASSERT_NO_FATAL_FAILURE(state.PrepareBeforeHead());
    ASSERT_NO_FATAL_FAILURE(state.ReserveUnboundGroups());
    ASSERT_NO_FATAL_FAILURE(state.PublishSourceAndObserveFreshAck());
    ASSERT_NO_FATAL_FAILURE(state.PlaceAndHold(false));
    EXPECT_FALSE(state.Bind());
    BoundClockState clockState = BoundClockState::Retired;
    ASSERT_TRUE(state.clock.CaptureState(&clockState));
    EXPECT_EQ(BoundClockState::Unbound, clockState);
    EXPECT_TRUE(state.privateClock.Valid());
    ASSERT_NO_FATAL_FAILURE(state.EndAndRetireSource());
    ASSERT_NO_FATAL_FAILURE(state.ReturnAllAndReclaim());
}

TEST(OriginalSourceSubmission, InitialSingleClockStructuralProofCannotReplaceAnInvalidatedNativeOwnerGate) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto& engine = *owned;
    ASSERT_NO_FATAL_FAILURE(RegisterInitialClockPcm(engine));
    GenuineInitialSingleClockHarness state(engine);
    ASSERT_NO_FATAL_FAILURE(state.PrepareBeforeHead());
    ASSERT_NO_FATAL_FAILURE(state.ReserveUnboundGroups());
    ASSERT_NO_FATAL_FAILURE(state.PublishSourceAndObserveFreshAck());
    ASSERT_NO_FATAL_FAILURE(state.PlaceAndHold());
    harken::Plan03PageGroupView actual{};
    ASSERT_TRUE(engine.timelinePool.ResolvePlan03GroupSliceProducer(state.groups[0], &actual));
    ASSERT_NE(nullptr, actual.frontier);
    ASSERT_TRUE(engine.timelinePool.InvalidateFrontierOwnerOperations(actual.frontier->operation.owner,
        state.decisions[0], harken::AudioSliceInvalidateReason::Cancel));
    EXPECT_TRUE(state.privateClock.Valid());
    EXPECT_FALSE(state.Bind());
    BoundClockState clockState = BoundClockState::Retired;
    ASSERT_TRUE(state.clock.CaptureState(&clockState));
    EXPECT_EQ(BoundClockState::Unbound, clockState);
    ASSERT_NO_FATAL_FAILURE(state.EndAndRetireSource());
    ASSERT_NO_FATAL_FAILURE(state.ReturnAllAndReclaim());
}

TEST(OriginalSourceSubmission, InitialSingleClockMoveBorrowAndRetiredHistoricalBindingKeepActualGroupLoansUntilReturn) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto& engine = *owned;
    ASSERT_NO_FATAL_FAILURE(RegisterInitialClockPcm(engine));
    GenuineInitialSingleClockHarness state(engine);
    ASSERT_NO_FATAL_FAILURE(state.PrepareBeforeHead());
    ASSERT_NO_FATAL_FAILURE(state.ReserveUnboundGroups());
    ASSERT_NO_FATAL_FAILURE(state.PublishSourceAndObserveFreshAck());
    ASSERT_NO_FATAL_FAILURE(state.PlaceAndHold());
    ASSERT_TRUE(state.Bind());
    BoundClock foreign;
    ASSERT_TRUE(state.clock.Borrow(&foreign));
    EXPECT_FALSE(state.clock.Borrow(&foreign));
    EXPECT_FALSE(state.clock.Borrow(&state.clock));
    BoundClock moved(std::move(foreign));
    EXPECT_FALSE(foreign.Valid());
    ASSERT_TRUE(moved.Valid());
    ASSERT_NO_FATAL_FAILURE(state.EndAndRetireSource());
    EXPECT_FALSE(engine.queueInstances.IsCurrentOriginalConditionalFutureReservation(state.original));
    BoundClockObservation historical{};
    ASSERT_TRUE(moved.CaptureInitialBinding(&historical));
    EXPECT_EQ(state.anchor, historical.sourceStartFrame);
    BoundClockState clockState = BoundClockState::Unbound;
    ASSERT_TRUE(moved.CaptureState(&clockState));
    EXPECT_EQ(BoundClockState::Retired, clockState);
    state.clock.Release();
    state.original.Release();
    EXPECT_FALSE(engine.CancelOriginalQueueLaneGroups(state.cohort, engine.mixer.BlocksRendered()));
    EXPECT_TRUE(state.Original(1).Valid());
    moved.Release();
    EXPECT_FALSE(moved.Valid());
    ASSERT_NO_FATAL_FAILURE(state.ReturnAllAndReclaim());
}

} // namespace


namespace {
TEST(OriginalSourceSubmission, InitialSingleClockRejectsNativeBindingOrderDifferentFromActualDecisionTicket) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto& engine = *owned;
    ASSERT_NO_FATAL_FAILURE(RegisterInitialClockPcm(engine));
    GenuineInitialSingleClockHarness state(engine);
    ASSERT_NO_FATAL_FAILURE(state.PrepareBeforeHead());
    ASSERT_NO_FATAL_FAILURE(state.ReserveUnboundGroups());
    ASSERT_NO_FATAL_FAILURE(state.PublishSourceAndObserveFreshAck());
    ASSERT_NO_FATAL_FAILURE(state.PlaceAndHold(true, false));
    harken::DecisionCommittedSnapshot actual{};
    ASSERT_TRUE(engine.decisions.CaptureCommitted(state.decisions[1], &actual));
    ASSERT_EQ(state.logicSequence, actual.ticket.sequence);
    harken::Plan03PageGroupView group{};
    ASSERT_TRUE(engine.timelinePool.ResolvePlan03GroupSliceProducer(state.groups[1], &group));
    ASSERT_NE(nullptr, group.frontier);
    ASSERT_NE(actual.ticket.sequence, group.frontier->operation.order.logicCommitSequence);
    EXPECT_FALSE(state.Bind());
    BoundClockState clockState = BoundClockState::Retired;
    ASSERT_TRUE(state.clock.CaptureState(&clockState));
    EXPECT_EQ(BoundClockState::Unbound, clockState);
    EXPECT_TRUE(state.privateClock.Valid());
    ASSERT_NO_FATAL_FAILURE(state.EndAndRetireSource());
    ASSERT_NO_FATAL_FAILURE(state.ReturnAllAndReclaim());
}
} // namespace


namespace {
TEST(OriginalSourceSubmission, InitialSingleClockQuiescentEngineReturnsNativeConditionalGroupsBeforePoolDestruction) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto& engine = *owned;
    // This genuine logical-boundary fixture tests only owner destruction order.
    // It issues no physical Source, first-S, published Lane or Reset authority.
    const auto root = engine.playableInstances.Acquire(harken::PlayableKind::Container, 0);
    const auto current = engine.playableInstances.Acquire(harken::PlayableKind::Sound, root);
    const auto successor = engine.playableInstances.Acquire(harken::PlayableKind::Sound, root);
    ASSERT_NE(0u, root);
    ASSERT_NE(0u, current);
    ASSERT_NE(0u, successor);
    harken::OccurrenceToken currentOccurrence{}, successorOccurrence{};
    ASSERT_TRUE(engine.timelineOccurrences.Acquire({current, kSound, 0, 0}, &currentOccurrence));
    ASSERT_TRUE(engine.timelineOccurrences.Acquire({successor, kSound, 0, 0}, &successorOccurrence));
    harken::QueueInstanceCreateRequest create{};
    create.world = 7;
    create.rootInstance = root;
    create.containerStableId = harkentest::ObjectId(13);
    create.programSemanticFingerprint = 53;
    create.childCount = 2;
    create.initial.version = 1;
    create.resetInitial = create.initial;
    harken::PreparedQueueInstanceV1 preparedQueue;
    ASSERT_TRUE(engine.PrepareOriginalQueueAcquire(create, &preparedQueue));
    auto queue = preparedQueue.Token();
    harken::QueueInstanceTable::OriginalLifetimePin parent;
    ASSERT_TRUE(engine.originalQueueLifetimeHolders[queue.slot].Borrow(&parent));
    ASSERT_TRUE(engine.queueInstances.CommitPreparedAcquire(&preparedQueue, &queue));
    std::array<harken::DecisionToken, 2> decisions{};
    harken::DecisionReserveRequest request{};
    request.ticket.sequence = engine.logicCommits.Issue();
    request.world = create.world;
    request.transport = engine.timelinePool.Epoch();
    request.expectedContinuationGeneration = 1;
    request.sliceCount = 1;
    for (auto& decision : decisions) ASSERT_TRUE(engine.decisions.Reserve(request, &decision));
    harken::ConditionalQueuePrepareRequest conditional{};
    conditional.expectedCurrentVersion = 1;
    conditional.rootDecision = decisions[1];
    conditional.expectedCurrentOccurrence = currentOccurrence;
    conditional.successorOccurrence = successorOccurrence;
    conditional.completionIsLogicalBoundary = true;
    conditional.next = create.initial;
    conditional.next.version = 2;
    conditional.next.cursor = 1;
    conditional.next.currentChildOrdinal = 1;
    conditional.next.selectionOrdinal = 1;
    harken::PreparedQueueConditionalV1 prepared;
    harken::QueueInstanceTable::OriginalConditionalFutureReservation original;
    ASSERT_TRUE(engine.queueInstances.PrepareConditional(queue, conditional, &prepared, parent, &original));
    harken::ConditionalQueueVersionToken version{};
    ASSERT_TRUE(engine.queueInstances.CommitPreparedConditional(&prepared, &version));
    std::array<harken::Plan03GroupSliceIdentity, 2> groups{};
    for (std::uint32_t index = 0; index < groups.size(); ++index) {
        harken::Plan03GroupSliceInput input{};
        input.executeFrame = 100 + index * kFrames;
        input.decision = decisions[index];
        ASSERT_TRUE(engine.ReserveOriginalQueueLaneGroup(47, 100, input,
            engine.timelineArms, &groups[index]));
    }
    BoundClock clock;
    ASSERT_TRUE(engine.queueInstances.PrepareOriginalConditionalLaneClock(original, parent,
        engine.timelinePool, engine.originalQueueGroupHolders[groups[0].committedRow],
        engine.originalQueueGroupHolders[groups[1].committedRow], &clock));
    BoundClock foreign;
    ASSERT_TRUE(clock.Borrow(&foreign));
    clock.Release();
    original.Release();
    parent.Release();
    ASSERT_TRUE(foreign.Valid());
    EXPECT_FALSE(engine.CancelOriginalQueueLaneGroups(47, 0));
    foreign.Release();
    // Early external drop does not cancel the committed native association.
    harken::QueueInstanceSnapshot native{};
    ASSERT_TRUE(engine.queueInstances.Capture(queue, &native));
    ASSERT_EQ(version.version, native.pendingVersion);
    EXPECT_FALSE(engine.CancelOriginalQueueLaneGroups(47, 0));
    ASSERT_TRUE(engine.originalQueueGroupHolders[groups[0].committedRow].Valid());
    ASSERT_TRUE(engine.originalQueueGroupHolders[groups[1].committedRow].Valid());
    EXPECT_EQ(0u, engine.mixer.BlocksRendered());
    // Actual Engine quiesces callback/workers, native-End returns the two loans,
    // and only then the real Pool owner is destroyed. No foreign count is forced.
    owned.reset();
}
} // namespace

namespace harken::detail {
// This access observes actual native state and borrows an already issued
// range actor. The limit setters establish negative prerequisites only.
struct OriginalSourceMutationActorTestAccess {
    using Registry = OriginalSourceSubmissionRegistry;
    static bool OwnersReturned(const Registry& registry, TimelineArmToken token) {
        const auto* row = registry.Find(token);
        if (row == nullptr) return false;
        const auto lifetime = row->originalLifetime_.load(std::memory_order_acquire);
        return row->State() == Registry::Phase::Disposed &&
            (lifetime & Registry::kPinMask) == 0 && (lifetime & Registry::kOwnersReleased) != 0 &&
            !row->arm_.Valid() && !row->voice_.Valid();
    }
    static Registry::InstalledSourceRetirementRequest RequestRetirement(Registry& registry,
        const OriginalSourceLifetimePin& source, const VoicePool::OriginalSourceBodyLifetimePin& body) {
        return registry.RequestPendingInstalledSourceRetirement(source, body);
    }
    static Registry::InstalledSourceRetirementRequest RequestRetirementReceipt(Registry& registry,
        const OriginalSourceLifetimePin& source, const VoicePool::OriginalSourceBodyLifetimePin& body,
        Registry::PendingInstalledSourceRetirementReceipt* out) {
        return registry.RequestPendingInstalledSourceRetirementWithReceipt(source, body, out);
    }
    static bool ReturnRetirementStorage(Registry& registry,
        Registry::PendingInstalledSourceRetirementReceipt& receipt, TimelineArmToken token) {
        return registry.ReturnPendingInstalledSourceRetirementStorage(receipt, token);
    }
    static bool RetirementSettled(const Registry& registry,
        const Registry::PendingInstalledSourceRetirementReceipt& receipt, TimelineArmToken token) {
        return registry.PendingInstalledSourceRetirementSettled(receipt, token);
    }
    struct Observation {
        std::uint8_t mode = 0;
        std::uint64_t count = 0;
        std::uint64_t epoch = 0;
        std::uint64_t lifetime = 0;
        bool installationStanding = false;
        bool rangeActor = false;
    };
    static Observation Observe(const Registry& registry, TimelineArmToken token) {
        const auto* row = registry.Find(token);
        assert(row != nullptr);
        const auto word = row->originWord_.load(std::memory_order_acquire);
        const auto mode = Registry::OriginState(word);
        const std::uint8_t semanticMode = mode == Registry::OriginMode::Normal ? 1u :
            mode == Registry::OriginMode::ExhaustedNormal ? 2u :
            mode == Registry::OriginMode::Closed ? 3u :
            mode == Registry::OriginMode::Redirect ? 4u :
            mode == Registry::OriginMode::ExhaustedRedirect ? 5u : 0u;
        return {semanticMode,
            Registry::OriginCount(word), Registry::OriginEpoch(word),
            row->originalLifetime_.load(std::memory_order_acquire) & Registry::kPinMask,
            row->originInstallationStanding_.load(std::memory_order_acquire),
            row->rangeActor_.Valid()};
    }
    static void ExhaustNativeSourceEpisodeAfterNext(Registry& registry) {
        registry.nextNativeSourceEpisode_ = UINT64_MAX;
        registry.nativeSourceEpisodeExhausted_ = false;
    }
    static bool BorrowExistingNativeFrontActor(Registry& registry,
        OriginalSourceMutationActor* out) {
        auto* row = registry.BorrowFront();
        // No IssueFrontRangeActor call or reconstruction is permitted here.
        return row != nullptr && row->rangeActor_.Borrow(out);
    }
    static bool Matches(const Registry& registry, const OriginalSourceMutationActor& actor) {
        return registry.MatchesActor(actor);
    }
    static void ExhaustActualEpoch(Registry& registry, TimelineArmToken token) {
        auto* row = registry.Find(token);
        assert(row != nullptr && row->rangeActor_.Valid());
        const auto word = row->originWord_.load(std::memory_order_acquire);
        assert(Registry::OriginState(word) == Registry::OriginMode::Normal);
        row->originWord_.store(Registry::OriginPack(Registry::OriginState(word),
            Registry::OriginCount(word), Registry::kOriginEpochMax), std::memory_order_release);
    }
    static std::uint64_t RefuseLastSourceBorrow(Registry& registry, TimelineArmToken token) {
        auto* row = registry.Find(token);
        assert(row != nullptr && row->rangeActor_.Valid());
        const auto word = row->originalLifetime_.load(std::memory_order_acquire);
        row->originalLifetime_.store((word & ~Registry::kPinMask) |
            (Registry::kPinMask - Registry::kNativeActorReserve), std::memory_order_release);
        return word;
    }
    static void RestoreActualSourceCount(Registry& registry, TimelineArmToken token,
        std::uint64_t prior) {
        auto* row = registry.Find(token);
        assert(row != nullptr);
        row->originalLifetime_.store(prior, std::memory_order_release);
    }
};
bool OriginalSourceOwnersReturnedForTest(const OriginalSourceSubmissionRegistry& registry, TimelineArmToken token) {
    return OriginalSourceMutationActorTestAccess::OwnersReturned(registry, token);
}
} // namespace harken::detail

namespace {
using SourceActor = harken::OriginalSourceMutationActor;
using SourceOriginAccess = harken::detail::OriginalSourceMutationActorTestAccess;
constexpr std::uint8_t kSourceOriginNormal = 1;
constexpr std::uint8_t kSourceOriginExhausted = 2;
constexpr std::uint8_t kSourceOriginClosed = 3;

void HoldActualPendingSource(harken::Engine& engine, const SourceBody& body,
    harken::TimelineArmTable::SourceCommandRowProof& held) {
    const auto* original = engine.originalSourceSubmissions.Find(body.arm);
    ASSERT_NE(nullptr, original);
    ASSERT_EQ(harken::TimelineArmTable::SourceCommandRowStatus::Ready,
        engine.timelineArms.ProvePendingSourceCommand(body.arm, &held, original));
    ASSERT_TRUE(held.Valid());
}
void PumpActualSourceDeferred(harken::Engine& engine, const SourceBody& body) {
    const auto before = engine.mixer.BlocksRendered();
    const auto consumed = engine.commands.ConsumedCount();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    EXPECT_EQ(consumed, engine.commands.ConsumedCount());
    EXPECT_EQ(0u, engine.timelineArms.RowAt(body.arm.slot)->armedCompletionSequence.load());
    EXPECT_EQ(0u, engine.timelineArms.RowAt(body.arm.slot)->startedBlockSequence.load());
    EXPECT_EQ(Registry::Phase::Published, engine.originalSourceSubmissions.Find(body.arm)->State());
    const auto state = SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm);
    EXPECT_TRUE(state.rangeActor);
    EXPECT_GE(state.count, 1u);
    EXPECT_FALSE(engine.voices.ReleaseObserved(body.release));
    // The metadata prefix has not escaped the real row proof reservation.
    EXPECT_NEAR(body.baseline, CurrentPeak(engine), 0.00001f);
}
void ReturnActualOriginTail(harken::Engine& engine, const SourceBody& body,
    Registry::OriginalSourceLifetimePin& source, BodyPin& original) {
    original.Release();
    source.Release();
    const auto state = SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm);
    EXPECT_EQ(0u, state.count);
    EXPECT_EQ(0u, state.lifetime);
    EXPECT_EQ(kSourceOriginClosed, state.mode);
    EXPECT_FALSE(state.installationStanding);
    EXPECT_FALSE(state.rangeActor);
    EXPECT_FALSE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
    ASSERT_NO_FATAL_FAILURE(ReclaimOriginalBodyArm(engine, body));
}
} // namespace

TEST(OriginalSourceSubmission, NormalOriginActualFactoryAndRangeFirstWriteInstallThenPhysicalEndClose) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    Registry::OriginalSourceLifetimePin source;
    BodyPin original;
    SourceActor empty;
    EXPECT_FALSE(empty.Valid());
    EXPECT_FALSE(empty.BeforeFirstWrite());
    EXPECT_FALSE(empty.Borrow(nullptr));
    ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, body, false, false, &source, &original));
    EXPECT_FALSE(SourceOriginAccess::BorrowExistingNativeFrontActor(engine.originalSourceSubmissions, &empty));
    auto state = SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm);
    EXPECT_EQ(kSourceOriginNormal, state.mode);
    EXPECT_EQ(0u, state.count);
    EXPECT_EQ(1u, state.epoch);
    EXPECT_TRUE(state.installationStanding);
    EXPECT_FALSE(state.rangeActor);
    ASSERT_NO_FATAL_FAILURE(PumpOriginalBodyInstall(engine, body));
    state = SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm);
    EXPECT_EQ(kSourceOriginNormal, state.mode);
    EXPECT_EQ(0u, state.count);
    EXPECT_GE(state.epoch, 2u);
    EXPECT_FALSE(state.rangeActor);
    EXPECT_TRUE(engine.originalSourceSubmissions.IsOriginalInstalledSourceCurrent(source, body.source));
    ASSERT_NO_FATAL_FAILURE(StopOriginalBodyEpisode(engine, body));
    original.Release();
    state = SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm);
    EXPECT_EQ(kSourceOriginClosed, state.mode);
    EXPECT_TRUE(source.Valid());
    EXPECT_FALSE(engine.originalSourceSubmissions.IsOriginalInstalledSourceCurrent(source, body.source));
    EXPECT_TRUE(engine.originalSourceSubmissions.IsOriginalSourceLifetimePinFor(source, body.arm));
    ASSERT_NO_FATAL_FAILURE(ReturnActualOriginTail(engine, body, source, original));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
}

TEST(OriginalSourceSubmission, NormalOriginGenuineDeferredTwoCallbacksPreservesActorAndReturnsAfterFreshAck) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    Registry::OriginalSourceLifetimePin source;
    BodyPin original;
    ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, body, false, false, &source, &original));
    harken::TimelineArmTable::SourceCommandRowProof held;
    ASSERT_NO_FATAL_FAILURE(HoldActualPendingSource(engine, body, held));
    const auto before = engine.mixer.BlocksRendered();
    ASSERT_NO_FATAL_FAILURE(PumpActualSourceDeferred(engine, body));
    SourceActor foreign;
    ASSERT_TRUE(SourceOriginAccess::BorrowExistingNativeFrontActor(engine.originalSourceSubmissions, &foreign));
    EXPECT_TRUE(foreign.Valid());
    const auto first = SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm);
    ASSERT_EQ(2u, first.count);
    ASSERT_EQ(2u, first.epoch);
    ASSERT_NO_FATAL_FAILURE(PumpActualSourceDeferred(engine, body));
    const auto second = SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm);
    EXPECT_EQ(first.epoch, second.epoch);
    EXPECT_EQ(first.count, second.count);
    held.Release();
    ASSERT_NO_FATAL_FAILURE(PumpOriginalBodyInstall(engine, body));
    EXPECT_EQ(before + 3u, engine.timelineArms.RowAt(body.arm.slot)->armedCompletionSequence.load());
    EXPECT_EQ(1u, SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm).count);
    EXPECT_FALSE(SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm).rangeActor);
    ASSERT_NO_FATAL_FAILURE(StopOriginalBodyEpisode(engine, body));
    source.Release();
    original.Release();
    EXPECT_TRUE(foreign.Valid());
    EXPECT_TRUE(SourceOriginAccess::Matches(engine.originalSourceSubmissions, foreign));
    EXPECT_TRUE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
    EXPECT_FALSE(engine.timelineArms.Reclaim(body.arm, engine.mixer.BlocksRendered()));
    foreign.Release();
    EXPECT_FALSE(foreign.Valid());
    EXPECT_FALSE(foreign.BeforeFirstWrite());
    ASSERT_NO_FATAL_FAILURE(ReturnActualOriginTail(engine, body, source, original));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
}

TEST(OriginalSourceSubmission, NormalOriginGenuineCancelAndInvalidWireHaveNoInstallAndRetainActorUntilReturn) {
    for (const bool cancel : {false, true}) {
        SCOPED_TRACE(cancel);
        harkentest::EngineSession session(SourceBodyConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        const std::vector<float> samples(kRate, 0.25f);
        SourceBody body;
        ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
        Registry::OriginalSourceLifetimePin source;
        BodyPin original;
        ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, body, false, false, &source, &original));
        harken::TimelineArmTable::SourceCommandRowProof held;
        ASSERT_NO_FATAL_FAILURE(HoldActualPendingSource(engine, body, held));
        ASSERT_NO_FATAL_FAILURE(PumpActualSourceDeferred(engine, body));
        SourceActor retained;
        ASSERT_TRUE(SourceOriginAccess::BorrowExistingNativeFrontActor(engine.originalSourceSubmissions, &retained));
        held.Release();
        const auto violations = engine.mixer.CommandProtocolViolationCount();
        if (cancel) ASSERT_TRUE(engine.timelineArms.RequestCancel(body.arm));
        else {
            harken::CommandRecord changed{};
            ASSERT_TRUE(engine.commands.PeekAt(1, changed));
            changed.flags = 0x80;
            harken::detail::CommandQueueSubmissionTestAccess::OverwriteAt(engine.commands, 1, changed);
        }
        const auto before = engine.mixer.BlocksRendered();
        engine.nullBackend.Pump(1);
        ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
        EXPECT_EQ(body.acceptedHead, engine.commands.ConsumedCount());
        EXPECT_EQ(Registry::Phase::Disposed, engine.originalSourceSubmissions.Find(body.arm)->State());
        EXPECT_EQ(0u, engine.timelineArms.RowAt(body.arm.slot)->armedCompletionSequence.load());
        EXPECT_EQ(0u, engine.timelineArms.RowAt(body.arm.slot)->startedBlockSequence.load());
        EXPECT_EQ(before + 1u, engine.timelineArms.RowAt(body.arm.slot)->tombstoneAudioSequence.load());
        EXPECT_TRUE(engine.voices.ReleaseObserved(body.release));
        EXPECT_FALSE(engine.voices.IsOriginalSourceBodyCurrent(original));
        EXPECT_FALSE(engine.originalSourceSubmissions.IsOriginalInstalledSourceCurrent(source, body.source));
        EXPECT_EQ(violations + (cancel ? 0u : 1u), engine.mixer.CommandProtocolViolationCount());
        EXPECT_NEAR(body.baseline * (cancel ? 0.5f : 1.0f), CurrentPeak(engine), 0.00001f);
        EXPECT_TRUE(SourceOriginAccess::Matches(engine.originalSourceSubmissions, retained));
        source.Release(); original.Release();
        EXPECT_FALSE(engine.timelineArms.Reclaim(body.arm, engine.mixer.BlocksRendered()));
        retained.Release();
        ASSERT_NO_FATAL_FAILURE(ReturnActualOriginTail(engine, body, source, original));
        ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
    }
}

TEST(OriginalSourceSubmission, NormalOriginAllExternalReturnsBeforeInstallDoNotHoldOldNativeProgressOrOwners) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    Registry::OriginalSourceLifetimePin source;
    BodyPin original;
    ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, body, false, false, &source, &original));
    source.Release(); original.Release();
    auto state = SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm);
    EXPECT_EQ(1u, state.lifetime);
    EXPECT_EQ(0u, state.count);
    ASSERT_NO_FATAL_FAILURE(PumpOriginalBodyInstall(engine, body));
    state = SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm);
    EXPECT_EQ(1u, state.lifetime);
    EXPECT_EQ(0u, state.count);
    EXPECT_GE(state.epoch, 2u);
    ASSERT_NO_FATAL_FAILURE(StopOriginalBodyEpisode(engine, body));
    ASSERT_NO_FATAL_FAILURE(ReturnActualOriginTail(engine, body, source, original));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
}

TEST(OriginalSourceSubmission, NormalOriginRealActorMoveOverwriteForeignReceiverAndOccupiedOutReturnExactScopes) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody first;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, first, samples));
    Registry::OriginalSourceLifetimePin firstSource, secondSource;
    BodyPin firstBody, secondBody;
    ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, first, false, false, &firstSource, &firstBody));
    harken::TimelineArmTable::SourceCommandRowProof firstHeld;
    ASSERT_NO_FATAL_FAILURE(HoldActualPendingSource(engine, first, firstHeld));
    ASSERT_NO_FATAL_FAILURE(PumpActualSourceDeferred(engine, first));
    SourceActor firstActor;
    ASSERT_TRUE(SourceOriginAccess::BorrowExistingNativeFrontActor(engine.originalSourceSubmissions, &firstActor));
    Registry foreign(engine.timelineArms, engine.voices, engine.commands, engine.effectBank);
    EXPECT_FALSE(SourceOriginAccess::Matches(foreign, firstActor));
    EXPECT_TRUE(SourceOriginAccess::Matches(engine.originalSourceSubmissions, firstActor));
    EXPECT_FALSE(firstActor.Borrow(&firstActor));
    firstHeld.Release();
    ASSERT_NO_FATAL_FAILURE(PumpOriginalBodyInstall(engine, first));
    SourceBody second;
    second.independent = first.independent;
    second.independentPublic = first.independentPublic;
    second.independentRelease = first.independentRelease;
    second.baseline = CurrentPeak(engine);
    ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, second, false, false, &secondSource, &secondBody));
    harken::TimelineArmTable::SourceCommandRowProof secondHeld;
    ASSERT_NO_FATAL_FAILURE(HoldActualPendingSource(engine, second, secondHeld));
    ASSERT_NO_FATAL_FAILURE(PumpActualSourceDeferred(engine, second));
    SourceActor secondActor;
    ASSERT_TRUE(SourceOriginAccess::BorrowExistingNativeFrontActor(engine.originalSourceSubmissions, &secondActor));
    SourceActor moved(std::move(firstActor));
    EXPECT_FALSE(firstActor.Valid());
    moved = std::move(secondActor);
    EXPECT_FALSE(secondActor.Valid());
    EXPECT_EQ(0u, SourceOriginAccess::Observe(engine.originalSourceSubmissions, first.arm).count);
    EXPECT_EQ(2u, SourceOriginAccess::Observe(engine.originalSourceSubmissions, second.arm).count);
    secondHeld.Release();
    // The same absolute -6 dB setter preserves the original independent level.
    const auto before = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    EXPECT_EQ(before + 1u, engine.timelineArms.RowAt(second.arm.slot)->armedCompletionSequence.load());
    EXPECT_EQ(second.acceptedHead, engine.commands.ConsumedCount());
    moved.Release();
    ASSERT_NO_FATAL_FAILURE(StopOriginalBodyEpisode(engine, first));
    ASSERT_NO_FATAL_FAILURE(StopOriginalBodyEpisode(engine, second));
    ASSERT_NO_FATAL_FAILURE(ReturnActualOriginTail(engine, first, firstSource, firstBody));
    ASSERT_NO_FATAL_FAILURE(ReturnActualOriginTail(engine, second, secondSource, secondBody));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, first));
}

TEST(OriginalSourceSubmission, NormalOriginGenuineBoundedBorrowExhaustionPreservesOldInstallAndEveryRealReturn) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    Registry::OriginalSourceLifetimePin source;
    BodyPin original;
    ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, body, false, false, &source, &original));
    harken::TimelineArmTable::SourceCommandRowProof held;
    ASSERT_NO_FATAL_FAILURE(HoldActualPendingSource(engine, body, held));
    ASSERT_NO_FATAL_FAILURE(PumpActualSourceDeferred(engine, body));
    std::array<SourceActor, 252> loans;
    ASSERT_TRUE(SourceOriginAccess::BorrowExistingNativeFrontActor(engine.originalSourceSubmissions, &loans[0]));
    for (std::size_t i = 1; i < loans.size(); ++i) ASSERT_TRUE(loans[0].Borrow(&loans[i]));
    EXPECT_EQ(253u, SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm).count);
    SourceActor refused;
    EXPECT_FALSE(loans[0].Borrow(&refused));
    EXPECT_FALSE(refused.Valid());
    EXPECT_EQ(kSourceOriginExhausted, SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm).mode);
    EXPECT_EQ(253u, SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm).count);
    held.Release();
    ASSERT_NO_FATAL_FAILURE(PumpOriginalBodyInstall(engine, body));
    EXPECT_EQ(252u, SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm).count);
    ASSERT_TRUE(loans[0].BeforeFirstWrite());
    for (auto& loan : loans) loan.Release();
    EXPECT_EQ(0u, SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm).count);
    ASSERT_NO_FATAL_FAILURE(StopOriginalBodyEpisode(engine, body));
    ASSERT_NO_FATAL_FAILURE(ReturnActualOriginTail(engine, body, source, original));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
}

TEST(OriginalSourceSubmission, NormalOriginTrueActorEpochMaxAndLastSourceBorrowRefusalPreserveAcceptedOld) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    Registry::OriginalSourceLifetimePin source;
    BodyPin original;
    ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, body, false, false, &source, &original));
    harken::TimelineArmTable::SourceCommandRowProof held;
    ASSERT_NO_FATAL_FAILURE(HoldActualPendingSource(engine, body, held));
    ASSERT_NO_FATAL_FAILURE(PumpActualSourceDeferred(engine, body));
    SourceActor actor;
    ASSERT_TRUE(SourceOriginAccess::BorrowExistingNativeFrontActor(engine.originalSourceSubmissions, &actor));
    const auto prior = SourceOriginAccess::RefuseLastSourceBorrow(engine.originalSourceSubmissions, body.arm);
    Registry::OriginalSourceLifetimePin refusedSource;
    SourceActor refusedActor;
    EXPECT_FALSE(source.Borrow(&refusedSource));
    EXPECT_FALSE(actor.Borrow(&refusedActor));
    EXPECT_FALSE(refusedSource.Valid());
    EXPECT_FALSE(refusedActor.Valid());
    SourceOriginAccess::RestoreActualSourceCount(engine.originalSourceSubmissions, body.arm, prior);
    SourceOriginAccess::ExhaustActualEpoch(engine.originalSourceSubmissions, body.arm);
    ASSERT_TRUE(actor.BeforeFirstWrite());
    auto state = SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm);
    EXPECT_EQ(kSourceOriginExhausted, state.mode);
    EXPECT_EQ(UINT64_MAX >> 11, state.epoch);
    ASSERT_TRUE(actor.BeforeFirstWrite());
    EXPECT_EQ(UINT64_MAX >> 11, SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm).epoch);
    held.Release();
    ASSERT_NO_FATAL_FAILURE(PumpOriginalBodyInstall(engine, body));
    EXPECT_EQ(UINT64_MAX >> 11, SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm).epoch);
    ASSERT_NO_FATAL_FAILURE(StopOriginalBodyEpisode(engine, body));
    actor.Release();
    ASSERT_NO_FATAL_FAILURE(ReturnActualOriginTail(engine, body, source, original));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
}

TEST(OriginalSourceSubmission, NormalOriginActualQuiescentTermReturnsDeferredRangeActorBeforeRegistryDestruction) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    Registry::OriginalSourceLifetimePin source;
    BodyPin original;
    ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, body, false, false, &source, &original));
    harken::TimelineArmTable::SourceCommandRowProof held;
    ASSERT_NO_FATAL_FAILURE(HoldActualPendingSource(engine, body, held));
    ASSERT_NO_FATAL_FAILURE(PumpActualSourceDeferred(engine, body));
    ASSERT_NO_FATAL_FAILURE(PumpActualSourceDeferred(engine, body));
    held.Release();
    source.Release(); original.Release();
    const auto beforeTerm = SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm);
    EXPECT_TRUE(beforeTerm.rangeActor);
    EXPECT_EQ(1u, beforeTerm.count);
    EXPECT_EQ(2u, beforeTerm.lifetime);
    EXPECT_EQ(0u, engine.timelineArms.RowAt(body.arm.slot)->armedCompletionSequence.load());
    // Engine stops callback and joins real workers, returns native Source/body
    // actors, disposes the original range, and only then destroys the registry.
    Vn_Term();
    session.Release();
    EXPECT_EQ(nullptr, session.Engine());
}

#include <atomic>
#include <thread>

namespace {
using NativeCallbackAccess = harken::detail::VoicePoolReleaseTestAccess;

void PublishCallbackSingleEpisode(harken::Engine& engine, GenuinePreparedSingleEpisode& episode) {
    harken::DecisionTable::PreparedTicketCommitProof decisionProof;
    const auto sequence = engine.logicCommits.Issue();
    ASSERT_NE(0u, sequence);
    ASSERT_TRUE(engine.decisions.ProvePreparedReservationCommit(episode.decisionPreparation,
        episode.decisionRequest, sequence, &decisionProof));
    harken::DecisionTable::ConsumePreparedReservationCommit(std::move(decisionProof));
    harken::PlayableInstanceTable::ConsumePreparedCommit(std::move(episode.parentProof));
    harken::PlayableInstanceTable::ConsumePreparedCommit(std::move(episode.childProof));
    harken::PlayableInstanceTable::ConsumePreparedParentLinks(std::move(episode.parentLinks));
    harken::OccurrenceStatePool::ConsumePreparedSingleCompletionCommit(std::move(episode.occurrenceProof));
    harken::VoiceLedger::ConsumePreparedSingleCompletionCommit(std::move(episode.ledgerProof));
    harken::VoicePool::ConsumePreparedCommit(std::move(episode.voiceProof));
    harken::TimelineArmTable::ConsumePreparedCommit(std::move(episode.armProof));
    engine.originalSourceSubmissions.PublishPreparedChild(episode.commands[0]);
    (void)engine.commands.ConsumePreparedBatch(std::move(episode.batch));
}
void RetireCallbackSingleEpisode(harken::Engine& engine, GenuinePreparedSingleEpisode& episode) {
    const auto decision = episode.originalDecision.Token();
    episode.bundle.Release();
    ASSERT_NO_FATAL_FAILURE(StopOriginalBodyEpisode(engine, episode.source));
    engine.ledger.Reap(episode.source.source);
    ASSERT_TRUE(engine.timelineOccurrences.ClearCompletionOwnerExact(episode.occurrence, episode.owner));
    harken::CompletionProgressSnapshot terminal{};
    ASSERT_TRUE(harken::CaptureCompletionProgressExact(*episode.progress, episode.occurrence,
        static_cast<std::uint32_t>(episode.source.source >> 32), &terminal));
    ASSERT_TRUE(engine.timelineOccurrences.UnbindCompletionProgress(episode.occurrence,
        static_cast<std::uint32_t>(episode.source.source >> 32)));
    ASSERT_TRUE(engine.timelineOccurrences.Advance(episode.occurrence, harken::OccurrenceStage::AwaitingEventFence));
    ASSERT_TRUE(engine.decisions.Cancel(decision));
    EXPECT_FALSE(engine.timelineOccurrences.ReleaseAfterFence(episode.occurrence,
        terminal.completedThroughBlockSequence, &episode.originalOccurrence));
    episode.originalLedger.Release();
    episode.originalBody.Release();
    episode.originalSource.Release();
    const auto afterStop = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(afterStop + 1u, engine.mixer.BlocksRendered());
    ASSERT_GT(engine.mixer.BlocksRendered(), terminal.completedThroughBlockSequence);
    ASSERT_TRUE(engine.timelineOccurrences.ReleaseAfterFence(episode.occurrence,
        engine.mixer.BlocksRendered(), &episode.originalOccurrence));
    ASSERT_TRUE(engine.decisions.ReclaimCancelled(decision, &episode.originalDecision));
    ASSERT_TRUE(engine.timelineArms.Reclaim(episode.source.arm, engine.mixer.BlocksRendered()));
    EXPECT_EQ(episode.source.armFreeBefore, engine.timelineArms.FreeCount());
    EXPECT_EQ(0u, NativeCallbackAccess::OriginalOwnerCount(engine.voices));
    ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(episode.arranging, harken::PlayableLifecycle::Retired));
    ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(episode.parent, harken::PlayableLifecycle::Retired));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, episode.source));
}
void CheckActualNativeCallbackReturned(const harken::Engine& engine, const SourceBody& body) {
    const auto state = NativeCallbackAccess::ObserveOriginalCallback(engine.voices,
        static_cast<std::uint32_t>(body.source));
    EXPECT_FALSE(state.reader);
    EXPECT_FALSE(state.storageHeld);
    EXPECT_FALSE(state.actor);
}
void CloseManualNativeStopAndReclaim(harken::Engine& engine, const SourceBody& body,
    Registry::OriginalSourceLifetimePin& source, BodyPin& original) {
    original.Release();
    source.Release();
    const auto old = engine.mixer.BlocksRendered();
    // The manual native operation does not publish a Mixer watermark. Only this
    // real subsequent callback can advance the actual completed block fence.
    engine.nullBackend.Pump(1);
    ASSERT_EQ(old + 1u, engine.mixer.BlocksRendered());
    ASSERT_NO_FATAL_FAILURE(ReturnActualOriginTail(engine, body, source, original));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
}
} // namespace

TEST(OriginalSourceSubmission, NativeCallbackTrueFinishHandoffAndRenderingProgressReturnBeforeFreshWatermark) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    GenuinePreparedSingleEpisode episode;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, episode.source, samples));
    ASSERT_TRUE(episode.Prepare(engine));
    ASSERT_TRUE(episode.Join(engine, &episode.bundle));
    const auto baseline = engine.mixer.BlocksRendered();
    harken::CompletionProgressSnapshot before{};
    ASSERT_TRUE(harken::CaptureCompletionProgressExact(*episode.progress, episode.occurrence,
        static_cast<std::uint32_t>(episode.source.source >> 32), &before));
    ASSERT_NO_FATAL_FAILURE(PublishCallbackSingleEpisode(engine, episode));
    engine.nullBackend.Pump(1);
    ASSERT_EQ(baseline + 1u, engine.mixer.BlocksRendered());
    EXPECT_EQ(baseline + 1u, engine.timelineArms.RowAt(episode.source.arm.slot)->armedCompletionSequence.load());
    EXPECT_EQ(0u, engine.timelineArms.RowAt(episode.source.arm.slot)->startedBlockSequence.load());
    EXPECT_TRUE(episode.bundle.ObserveInstalled());
    harken::CompletionProgressSnapshot installed{};
    ASSERT_TRUE(harken::CaptureCompletionProgressExact(*episode.progress, episode.occurrence,
        static_cast<std::uint32_t>(episode.source.source >> 32), &installed));
    EXPECT_GT(installed.progressRevision, before.progressRevision);
    EXPECT_EQ(baseline + 1u, installed.completedThroughBlockSequence);
    ASSERT_NO_FATAL_FAILURE(CheckActualNativeCallbackReturned(engine, episode.source));
    const auto first = SourceOriginAccess::Observe(engine.originalSourceSubmissions, episode.source.arm);
    EXPECT_EQ(0u, first.count);
    // One actual range first write and the actual Finish callback handoff.
    EXPECT_GE(first.epoch, 3u);
    engine.nullBackend.Pump(1);
    ASSERT_EQ(baseline + 2u, engine.mixer.BlocksRendered());
    harken::CompletionProgressSnapshot next{};
    ASSERT_TRUE(harken::CaptureCompletionProgressExact(*episode.progress, episode.occurrence,
        static_cast<std::uint32_t>(episode.source.source >> 32), &next));
    EXPECT_GT(next.progressRevision, installed.progressRevision);
    EXPECT_EQ(baseline + 2u, next.completedThroughBlockSequence);
    const auto second = SourceOriginAccess::Observe(engine.originalSourceSubmissions, episode.source.arm);
    EXPECT_GT(second.epoch, first.epoch);
    EXPECT_EQ(0u, second.count);
    ASSERT_NO_FATAL_FAILURE(CheckActualNativeCallbackReturned(engine, episode.source));
    ASSERT_NO_FATAL_FAILURE(RetireCallbackSingleEpisode(engine, episode));
}

TEST(OriginalSourceSubmission, NativeCallbackRenderingHeldRefusesLogicCancelAndTrueAudioStopBlocksSameBlockReuse) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    Registry::OriginalSourceLifetimePin source;
    BodyPin original;
    ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, body, false, false, &source, &original));
    ASSERT_NO_FATAL_FAILURE(PumpOriginalBodyInstall(engine, body));
    const auto watermark = engine.mixer.BlocksRendered();
    const auto before = SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm);
    engine.voices.BeginAudioBlock();
    const auto held = NativeCallbackAccess::ObserveOriginalCallback(engine.voices,
        static_cast<std::uint32_t>(body.source));
    EXPECT_TRUE(held.callbackHeld);
    EXPECT_FALSE(held.rendering);
    EXPECT_TRUE(held.reader);
    EXPECT_TRUE(held.storageHeld);
    EXPECT_TRUE(held.actor);
    EXPECT_EQ(1u, SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm).count);
    EXPECT_GT(SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm).epoch, before.epoch);
    EXPECT_FALSE(engine.voices.CancelTimelineBinding(static_cast<std::uint32_t>(body.source),
        static_cast<std::uint32_t>(body.source >> 32)));
    EXPECT_FALSE(engine.voices.ReleaseObserved(body.release));
    ASSERT_TRUE(engine.timelineArms.RequestCancel(body.arm));
    original.Release();
    // This is the native audio Stop entry under its own real callback hold.
    engine.voices.ApplyStop(body.source, 0, 0, engine.notifications, watermark);
    ASSERT_TRUE(engine.voices.ReleaseObserved(body.release));
    EXPECT_EQ(nullptr, engine.voices.Resolve(body.source));
    const auto physical = NativeCallbackAccess::ObserveOriginalCallback(engine.voices,
        static_cast<std::uint32_t>(body.source));
    EXPECT_TRUE(physical.free);
    EXPECT_FALSE(physical.callbackHeld);
    EXPECT_TRUE(physical.reader);
    EXPECT_TRUE(physical.actor);
    EXPECT_TRUE(physical.disposed);
    EXPECT_FALSE(physical.loanReturned);
    EXPECT_EQ(0u, physical.bodyCount);
    VnVoiceHandle replacement = 0;
    EXPECT_EQ(VN_NO_VOICE, PrepareAtOriginalBodySlot(engine, body, &replacement));
    EXPECT_EQ(0u, replacement);
    engine.voices.StageCompletionProgressForSilentBlock(watermark);
    (void)engine.voices.PublishCompletionProgress(watermark);
    engine.voices.EndAudioBlock();
    EXPECT_EQ(watermark, engine.mixer.BlocksRendered());
    ASSERT_NO_FATAL_FAILURE(CheckActualNativeCallbackReturned(engine, body));
    const auto returned = NativeCallbackAccess::ObserveOriginalCallback(engine.voices,
        static_cast<std::uint32_t>(body.source));
    EXPECT_TRUE(returned.free);
    EXPECT_TRUE(returned.loanReturned);
    EXPECT_TRUE(returned.returnClaimed);
    EXPECT_EQ(0u, SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm).count);
    EXPECT_EQ(1u, SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm).lifetime);
    ASSERT_EQ(VN_OK, PrepareAtOriginalBodySlot(engine, body, &replacement));
    EXPECT_NE(body.source, replacement);
    ASSERT_TRUE(engine.voices.CancelPreparedVoice(replacement));
    ASSERT_NO_FATAL_FAILURE(CloseManualNativeStopAndReclaim(engine, body, source, original));
}

TEST(OriginalSourceSubmission, NativeTimelineCancelUsesActualCallbackHoldAndReturnsStorageAfterLastUse) {
    for (const bool returnForeignInCallback : {false, true}) {
        SCOPED_TRACE(returnForeignInCallback);
        harkentest::EngineSession session(SourceBodyConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        const std::vector<float> samples(kRate, 0.25f);
        SourceBody body;
        ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
        Registry::OriginalSourceLifetimePin source;
        BodyPin original, foreign;
        ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, body, false, false, &source, &original));
        ASSERT_TRUE(original.Borrow(&foreign));
        ASSERT_NO_FATAL_FAILURE(PumpOriginalBodyInstall(engine, body));
        original.Release();
        ASSERT_TRUE(engine.timelineArms.RequestCancel(body.arm));
        const auto baseline = engine.mixer.BlocksRendered();
        struct Probe {
            harken::Engine* engine;
            const SourceBody* body;
            BodyPin* foreign;
            bool returnForeign;
            bool before = false;
            bool after = false;
        } probe{&engine, &body, &foreign, returnForeignInCallback};
        engine.mixer.SetCurrentSliceFrontierHookForTest(+[](void* context,
            harken::MixerLoop::CurrentSliceHookPhaseForTest phase,
            std::uint64_t, std::size_t, std::uint32_t,
            harken::TimelinePagePrefixStopReason) noexcept {
            auto& probe = *static_cast<Probe*>(context);
            auto& engine = *probe.engine;
            const auto& body = *probe.body;
            const auto slot = static_cast<std::uint32_t>(body.source);
            const auto state = NativeCallbackAccess::ObserveOriginalCallback(engine.voices, slot);
            if (phase == harken::MixerLoop::CurrentSliceHookPhaseForTest::AfterStageBeforeExecute) {
                probe.before = true;
                EXPECT_TRUE(state.callbackHeld);
                EXPECT_TRUE(state.reader);
                EXPECT_TRUE(state.storageHeld);
                EXPECT_TRUE(state.actor);
                EXPECT_FALSE(engine.voices.CancelTimelineBinding(slot,
                    static_cast<std::uint32_t>(body.source >> 32)));
                EXPECT_FALSE(engine.voices.ReleaseObserved(body.release));
                return;
            }
            probe.after = true;
            EXPECT_TRUE(engine.voices.ReleaseObserved(body.release));
            EXPECT_EQ(nullptr, engine.voices.Resolve(body.source));
            EXPECT_TRUE(state.free);
            EXPECT_TRUE(state.reader);
            EXPECT_TRUE(state.storageHeld);
            EXPECT_TRUE(state.actor);
            EXPECT_TRUE(state.disposed);
            EXPECT_FALSE(state.loanReturned);
            harken::TimelineArmObservation arm{};
            EXPECT_TRUE(engine.timelineArms.Observe(body.arm, &arm));
            EXPECT_EQ(harken::ArmState::Tombstoned, arm.state);
            if (probe.returnForeign) probe.foreign->Release();
            VnVoiceHandle refused = 0;
            EXPECT_EQ(VN_NO_VOICE, PrepareAtOriginalBodySlot(engine, body, &refused));
            EXPECT_EQ(0u, refused);
        }, &probe);
        engine.nullBackend.Pump(1);
        engine.mixer.SetCurrentSliceFrontierHookForTest(nullptr, nullptr);
        ASSERT_EQ(baseline + 1u, engine.mixer.BlocksRendered());
        EXPECT_TRUE(probe.before);
        EXPECT_TRUE(probe.after);
        ASSERT_TRUE(engine.voices.ReleaseObserved(body.release));
        EXPECT_NEAR(body.baseline * 0.5f, CurrentPeak(engine), 0.00001f);
        ASSERT_NO_FATAL_FAILURE(CheckActualNativeCallbackReturned(engine, body));
        if (!returnForeignInCallback) {
            VnVoiceHandle refused = 0;
            EXPECT_EQ(VN_NO_VOICE, PrepareAtOriginalBodySlot(engine, body, &refused));
            foreign.Release();
        }
        const auto returned = NativeCallbackAccess::ObserveOriginalCallback(engine.voices,
            static_cast<std::uint32_t>(body.source));
        EXPECT_TRUE(returned.loanReturned);
        VnVoiceHandle replacement = 0;
        ASSERT_EQ(VN_OK, PrepareAtOriginalBodySlot(engine, body, &replacement));
        EXPECT_NE(body.source, replacement);
        ASSERT_TRUE(engine.voices.CancelPreparedVoice(replacement));
        ASSERT_NO_FATAL_FAILURE(CloseManualNativeStopAndReclaim(engine, body, source, original));
    }
}

TEST(OriginalSourceSubmission, NativeCallbackLastForeignBodyReturnAndNativeEndReturnExactSelfLoanOnce) {
    // Three real orders: before physical End, after physical End, and racing
    // native callback last use. No scalar flag or capability is manufactured.
    for (const unsigned order : {0u, 1u, 2u}) {
        SCOPED_TRACE(order);
        harkentest::EngineSession session(SourceBodyConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        const std::vector<float> samples(kRate, 0.25f);
        SourceBody body;
        ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
        Registry::OriginalSourceLifetimePin source;
        BodyPin original, foreign;
        ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, body, false, false, &source, &original));
        ASSERT_TRUE(original.Borrow(&foreign));
        ASSERT_NO_FATAL_FAILURE(PumpOriginalBodyInstall(engine, body));
        original.Release();
        const auto watermark = engine.mixer.BlocksRendered();
        engine.voices.BeginAudioBlock();
        ASSERT_TRUE(engine.timelineArms.RequestCancel(body.arm));
        if (order == 0) foreign.Release();
        engine.voices.ApplyStop(body.source, 0, 0, engine.notifications, watermark);
        ASSERT_TRUE(engine.voices.ReleaseObserved(body.release));
        const auto physical = NativeCallbackAccess::ObserveOriginalCallback(engine.voices,
            static_cast<std::uint32_t>(body.source));
        EXPECT_TRUE(physical.free);
        EXPECT_FALSE(physical.callbackHeld);
        EXPECT_TRUE(physical.disposed);
        EXPECT_TRUE(physical.reader);
        EXPECT_FALSE(physical.loanReturned);
        if (order == 1) foreign.Release();
        engine.voices.StageCompletionProgressForSilentBlock(watermark);
        (void)engine.voices.PublishCompletionProgress(watermark);
        if (order == 2) {
            std::atomic<bool> ready{false}, go{false};
            std::thread lastForeign([&] {
                ready.store(true, std::memory_order_release);
                while (!go.load(std::memory_order_acquire)) std::this_thread::yield();
                foreign.Release();
            });
            while (!ready.load(std::memory_order_acquire)) std::this_thread::yield();
            go.store(true, std::memory_order_release);
            engine.voices.EndAudioBlock();
            lastForeign.join();
        } else engine.voices.EndAudioBlock();
        EXPECT_FALSE(foreign.Valid());
        EXPECT_EQ(watermark, engine.mixer.BlocksRendered());
        ASSERT_NO_FATAL_FAILURE(CheckActualNativeCallbackReturned(engine, body));
        const auto after = NativeCallbackAccess::ObserveOriginalCallback(engine.voices,
            static_cast<std::uint32_t>(body.source));
        EXPECT_TRUE(after.returnClaimed);
        EXPECT_TRUE(after.loanReturned);
        EXPECT_EQ(0u, after.bodyCount);
        EXPECT_FALSE(after.pendingInstallation);
        EXPECT_FALSE(after.installedInstallation);
        // One historical Source pin remains. A duplicate self-loan return
        // would corrupt this exact native count or release its true Arm owner.
        const auto origin = SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm);
        EXPECT_EQ(1u, origin.lifetime);
        EXPECT_EQ(0u, origin.count);
        EXPECT_EQ(kSourceOriginClosed, origin.mode);
        EXPECT_TRUE(source.Valid());
        EXPECT_TRUE(engine.timelineArms.RowAt(body.arm.slot)->originalLifetimePinned.load());
        VnVoiceHandle replacement = 0;
        ASSERT_EQ(VN_OK, PrepareAtOriginalBodySlot(engine, body, &replacement));
        EXPECT_NE(body.source, replacement);
        ASSERT_TRUE(engine.voices.CancelPreparedVoice(replacement));
        ASSERT_NO_FATAL_FAILURE(CloseManualNativeStopAndReclaim(engine, body, source, original));
    }
}

TEST(OriginalSourceSubmission, NativeCallbackActualActorChildrenMoveAndReturnKeepOriginalSourceScope) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    Registry::OriginalSourceLifetimePin source;
    BodyPin original;
    ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, body, false, false, &source, &original));
    ASSERT_NO_FATAL_FAILURE(PumpOriginalBodyInstall(engine, body));
    engine.voices.BeginAudioBlock();
    EXPECT_TRUE(NativeCallbackAccess::ObserveOriginalCallback(engine.voices,
        static_cast<std::uint32_t>(body.source)).callbackHeld);
    EXPECT_TRUE(engine.voices.IsOriginalSourceBodyCurrent(original));
    SourceActor first, second;
    ASSERT_TRUE(NativeCallbackAccess::BorrowActualCallbackActor(engine.voices,
        static_cast<std::uint32_t>(body.source), &first));
    ASSERT_TRUE(first.Borrow(&second));
    EXPECT_FALSE(first.Borrow(&second));
    Registry foreign(engine.timelineArms, engine.voices, engine.commands, engine.effectBank);
    EXPECT_FALSE(SourceOriginAccess::Matches(foreign, first));
    EXPECT_TRUE(SourceOriginAccess::Matches(engine.originalSourceSubmissions, first));
    SourceActor moved(std::move(first));
    EXPECT_FALSE(first.Valid());
    moved = std::move(second);
    EXPECT_FALSE(second.Valid());
    engine.voices.StageCompletionProgressForSilentBlock(engine.mixer.BlocksRendered());
    (void)engine.voices.PublishCompletionProgress(engine.mixer.BlocksRendered());
    engine.voices.EndAudioBlock();
    ASSERT_NO_FATAL_FAILURE(CheckActualNativeCallbackReturned(engine, body));
    EXPECT_TRUE(NativeCallbackAccess::ObserveOriginalCallback(engine.voices,
        static_cast<std::uint32_t>(body.source)).rendering);
    EXPECT_TRUE(engine.voices.IsOriginalSourceBodyCurrent(original));
    EXPECT_EQ(1u, SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm).count);
    moved.Release();
    EXPECT_EQ(0u, SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm).count);
    EXPECT_FALSE(moved.Borrow(&first));
    ASSERT_NO_FATAL_FAILURE(StopOriginalBodyEpisode(engine, body));
    ASSERT_NO_FATAL_FAILURE(ReturnActualOriginTail(engine, body, source, original));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
}

TEST(OriginalSourceSubmission, NativeCallbackRealChildCountExhaustionStillAllowsLaterOldRenderingAndStop) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    Registry::OriginalSourceLifetimePin source;
    BodyPin original;
    ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, body, false, false, &source, &original));
    ASSERT_NO_FATAL_FAILURE(PumpOriginalBodyInstall(engine, body));
    engine.voices.BeginAudioBlock();
    EXPECT_TRUE(NativeCallbackAccess::ObserveOriginalCallback(engine.voices,
        static_cast<std::uint32_t>(body.source)).callbackHeld);
    EXPECT_TRUE(engine.voices.IsOriginalSourceBodyCurrent(original));
    std::array<SourceActor, 252> loans;
    ASSERT_TRUE(NativeCallbackAccess::BorrowActualCallbackActor(engine.voices,
        static_cast<std::uint32_t>(body.source), &loans[0]));
    for (std::size_t i = 1; i < loans.size(); ++i) ASSERT_TRUE(loans[0].Borrow(&loans[i]));
    SourceActor refused;
    EXPECT_FALSE(loans[0].Borrow(&refused));
    EXPECT_FALSE(refused.Valid());
    EXPECT_EQ(kSourceOriginExhausted, SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm).mode);
    engine.voices.StageCompletionProgressForSilentBlock(engine.mixer.BlocksRendered());
    (void)engine.voices.PublishCompletionProgress(engine.mixer.BlocksRendered());
    engine.voices.EndAudioBlock();
    EXPECT_EQ(252u, SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm).count);
    const auto before = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    EXPECT_EQ(252u, SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm).count);
    EXPECT_TRUE(engine.voices.IsOriginalSourceBodyCurrent(original));
    ASSERT_NO_FATAL_FAILURE(CheckActualNativeCallbackReturned(engine, body));
    ASSERT_NO_FATAL_FAILURE(StopOriginalBodyEpisode(engine, body));
    for (auto& loan : loans) loan.Release();
    ASSERT_NO_FATAL_FAILURE(ReturnActualOriginTail(engine, body, source, original));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
}


namespace {
TEST(OriginalSourceSubmission, EngineOriginalSingleEarlyEndUsesActualReleaseAndNativeHoldUntilTheRealFinalTail) {
    const std::vector<float> samples(kRate, 0.25f);
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    GenuinePreparedSingleEpisode episode;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, episode.source, samples));
    ASSERT_TRUE(episode.Prepare(engine));
    ASSERT_TRUE(engine.IssueOriginalPlayableLifetimes(episode.parentLinks));
    ASSERT_TRUE(JoinEngineOriginalCurrent(engine, episode));
    auto& holder = CurrentHolder(engine, episode);
    SingleBundle foreign;
    ASSERT_TRUE(engine.BorrowOriginalSingleCurrent(holder, &foreign));
    const auto decision = episode.originalDecision.Token();
    harken::DecisionTable::PreparedTicketCommitProof decisionProof;
    const auto sequence = engine.logicCommits.Issue();
    ASSERT_NE(0u, sequence);
    ASSERT_TRUE(engine.decisions.ProvePreparedReservationCommit(episode.decisionPreparation,
        episode.decisionRequest, sequence, &decisionProof));
    harken::DecisionTable::ConsumePreparedReservationCommit(std::move(decisionProof));
    harken::PlayableInstanceTable::ConsumePreparedCommit(std::move(episode.parentProof));
    harken::PlayableInstanceTable::ConsumePreparedCommit(std::move(episode.childProof));
    harken::PlayableInstanceTable::ConsumePreparedParentLinks(std::move(episode.parentLinks));
    ASSERT_TRUE(engine.playableInstances.AcquireSourceRetirementHold(episode.arranging,
        engine.playableInstances.Classify(episode.arranging).scheduleEpoch));
    harken::OccurrenceStatePool::ConsumePreparedSingleCompletionCommit(std::move(episode.occurrenceProof));
    harken::VoiceLedger::ConsumePreparedSingleCompletionCommit(std::move(episode.ledgerProof));
    harken::VoicePool::ConsumePreparedCommit(std::move(episode.voiceProof));
    harken::TimelineArmTable::ConsumePreparedCommit(std::move(episode.armProof));
    engine.originalSourceSubmissions.PublishPreparedChild(episode.commands[0]);
    (void)engine.commands.ConsumePreparedBatch(std::move(episode.batch));
    const auto installBefore = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(installBefore + 1u, engine.mixer.BlocksRendered());
    ASSERT_EQ(installBefore + 1u, engine.timelineArms.RowAt(episode.source.arm.slot)->armedCompletionSequence.load());
    ASSERT_TRUE(holder.bundle.ObserveInstalled());
    EXPECT_FALSE(holder.bundle.OriginalPhysicalReleaseObserved());
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::Engine::OriginalSingleCurrentHolder::State::Prepared, holder.state);
    EXPECT_FALSE(holder.retirementHold.Valid());
    ASSERT_TRUE(engine.timelineArms.RequestCancel(episode.source.arm));
    harken::CommandRecord stop{};
    stop.opcode = harken::kOpcodeStop;
    stop.payload.stop.handle = episode.source.source;
    ASSERT_TRUE(engine.commands.TryPush(stop));
    const auto stopBefore = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(stopBefore + 1u, engine.mixer.BlocksRendered());
    ASSERT_TRUE(engine.voices.ReleaseObserved(episode.source.release));
    ASSERT_TRUE(holder.bundle.OriginalPhysicalReleaseObserved());
    EXPECT_FALSE(foreign.ObserveInstalled());
    engine.ledger.Reap(episode.source.source);
    ASSERT_TRUE(engine.timelineOccurrences.ClearCompletionOwnerExact(episode.occurrence, episode.owner));
    harken::CompletionProgressSnapshot terminal{};
    ASSERT_TRUE(harken::CaptureCompletionProgressExact(*episode.progress, episode.occurrence,
        static_cast<std::uint32_t>(episode.source.source >> 32), &terminal));
    ASSERT_TRUE(engine.timelineOccurrences.UnbindCompletionProgress(episode.occurrence,
        static_cast<std::uint32_t>(episode.source.source >> 32)));
    ASSERT_TRUE(engine.timelineOccurrences.Advance(episode.occurrence, harken::OccurrenceStage::AwaitingEventFence));
    ASSERT_TRUE(engine.decisions.Cancel(decision));
    ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(episode.arranging, harken::PlayableLifecycle::LogicalComplete));
    // The actual parent native end request is still rejected by its true child
    // topology. Returning a child hold alone never fabricates a parent end.
    EXPECT_FALSE(engine.playableInstances.AdvanceLifecycle(episode.parent, harken::PlayableLifecycle::Retired));
    EXPECT_EQ(episode.arranging, engine.playableInstances.FirstChild(episode.parent));
    EXPECT_TRUE(engine.playableInstances.Classify(episode.parent).live);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::Engine::OriginalSingleCurrentHolder::State::Prepared, holder.state);
    EXPECT_TRUE(holder.bundle.Valid());
    EXPECT_FALSE(holder.retirementHold.Valid());
    EXPECT_TRUE(engine.playableInstances.HasSourceRetirementHold(episode.arranging));
    ASSERT_TRUE(engine.playableInstances.ReleaseSourceRetirementHold(episode.arranging));
    // This actual normal Update issues the native end hold and returns only
    // the Engine's own association. The independent foreign bundle remains.
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::Engine::OriginalSingleCurrentHolder::State::Ending, holder.state);
    EXPECT_FALSE(holder.bundle.Valid());
    ASSERT_TRUE(holder.retirementHold.Valid());
    EXPECT_EQ(episode.arranging, holder.retirementHold.OriginalHandle());
    EXPECT_TRUE(foreign.Valid());
    EXPECT_TRUE(foreign.HasOriginalPlayableRetention());
    EXPECT_TRUE(engine.playableInstances.Classify(episode.arranging).live);
    EXPECT_EQ(episode.arranging, engine.playableInstances.FirstChild(episode.parent));
    EXPECT_FALSE(engine.timelineOccurrences.ReleaseAfterFence(episode.occurrence,
        engine.mixer.BlocksRendered(), &episode.originalOccurrence));
    EXPECT_FALSE(engine.decisions.ReclaimCancelled(decision, &episode.originalDecision));
    SingleBundle refused;
    EXPECT_FALSE(engine.BorrowOriginalSingleCurrent(holder, &refused));
    // A genuine same-family borrow is not the fixed baseline factory owner.
    auto* rootBaseline = harken::Engine::ResolveOriginalPlayableBaseline(&engine,
        engine.playableInstances, episode.parent);
    ASSERT_NE(nullptr, rootBaseline);
    PlayableOriginal foreignRoot;
    ASSERT_TRUE(rootBaseline->Borrow(&foreignRoot));
    harken::PlayableInstanceTable::OriginalLifetimeRetirementHold rejected;
    EXPECT_FALSE(engine.playableInstances.BeginOriginalLifetimeRetirement(foreignRoot, &rejected));
    foreignRoot.Release();
    foreign.Release();
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_TRUE(holder.retirementHold.Valid());
    EXPECT_TRUE(engine.playableInstances.Classify(episode.arranging).live);
    EXPECT_FALSE(holder.ResetAfterNativeRetirement(engine));
    engine.ReleaseOriginalSingleRetirementHold(episode.parent);
    EXPECT_TRUE(holder.retirementHold.Valid());
    // The true native child-tail owner returns its exact held loan;
    // cached finishing then repeats child/Source/unlink/foreign gates.
    engine.ReleaseOriginalSingleRetirementHold(episode.arranging);
    EXPECT_FALSE(holder.retirementHold.Valid());
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_FALSE(engine.playableInstances.Classify(episode.arranging).live);
    EXPECT_FALSE(engine.playableInstances.Classify(episode.parent).live);
    ASSERT_TRUE(holder.ResetAfterNativeRetirement(engine));
    EXPECT_FALSE(engine.timelineOccurrences.ReleaseAfterFence(episode.occurrence,
        terminal.completedThroughBlockSequence, &episode.originalOccurrence));
    episode.originalLedger.Release();
    episode.originalBody.Release();
    episode.originalSource.Release();
    const auto fenceBefore = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(fenceBefore + 1u, engine.mixer.BlocksRendered());
    ASSERT_GT(engine.mixer.BlocksRendered(), terminal.completedThroughBlockSequence);
    ASSERT_TRUE(engine.timelineOccurrences.ReleaseAfterFence(episode.occurrence,
        engine.mixer.BlocksRendered(), &episode.originalOccurrence));
    ASSERT_TRUE(engine.decisions.ReclaimCancelled(decision, &episode.originalDecision));
    ASSERT_TRUE(engine.timelineArms.Reclaim(episode.source.arm, engine.mixer.BlocksRendered()));
    EXPECT_EQ(episode.source.armFreeBefore, engine.timelineArms.FreeCount());
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, episode.source));
}
} // namespace

namespace {
// Genuine old Source/first-S plus an independently prepared native zero-member
// New C1. This component test does not select New or settle the actual Queue.
struct HiddenBoundNativeFuture {
    GenuineInitialSingleClockHarness old;
    harken::QueueInstanceTable::PreparedCatalogResetScope scope;
    harken::LogicCommitCoordinator::PreparedIssue sequence;
    harken::PreparedDecisionReservation decision, coordinator;
    harken::OriginalDecisionLifetimePin decisionOwner, coordinatorOwner;
    harken::DecisionTable::PreparedTicketCommitProof decisionProof, coordinatorProof;
    harken::DecisionTable::PreparedTicketCommitProof sourceCompletionTicket;
    harken::OccurrenceToken successor{};
    harken::OccurrenceStatePool::OriginalSingleCompletionOwnerPin successorOwner;
    harken::OccurrenceStatePool::PreparedSingleCompletionCommitProof successorProof;
    harken::CompletionOwnerRef completionOwner{};
    std::array<harken::Plan03GroupSliceInput, 2> input{};
    std::array<harken::Plan03GroupSliceIdentity, 2> group{};
    std::array<harken::DecisionToken, 2> token{};
    std::array<harken::ArmGroupToken, 2> armGroup{};
    harken::ImmediateLaneHandle lane{};
    harken::PrivateSelectedQueueFutureSpanPool spans{1};
    harken::PrivateSelectedQueueFutureSpanPool::Owner owner;
    harken::PrivateSelectedQueueFutureSpanPool::NativeConditionalRead read;
    bool retainedCurrent = false;
    std::uint64_t root = 29;
    explicit HiddenBoundNativeFuture(harken::Engine& engine) : old(engine) {}
    const harken::OriginalQueueFutureGroupRegistration& Original(std::uint32_t i) const {
        return old.engine.originalQueueGroupHolders[group[i].committedRow];
    }
    void ReserveOriginalForAudio(harken::OriginalImmediateLaneAttemptLifetimePin* originalLane) {
        auto& engine = old.engine;
        ASSERT_TRUE(engine.immediateActionLane.Reserve(old.cohort, {3, 4, 0, 2}, &old.lane, originalLane));
        for (std::uint32_t index = 0; index < old.groups.size(); ++index) {
            ASSERT_TRUE(engine.timelinePool.BindImmediateLaneGroupOwner(old.groups[index],
                engine.immediateActionLane, old.cohort, old.lane.slot, old.lane.generation));
            ASSERT_TRUE(engine.immediateActionLane.DeclareGroup(old.lane,
                {old.groups[index], old.inputs[index].executeFrame - old.provisionalAnchor}));
        }
    }
    void PlaceOriginalForAudio(harken::OriginalImmediateLaneAttemptLifetimePin* originalLane = nullptr,
        std::uint64_t conditionalRelative = kFrames, std::uint64_t firstAnchor = 1024,
        bool selectedReplacement = false) {
        auto& engine = old.engine;
        if (!old.lane.Valid()) ASSERT_NO_FATAL_FAILURE(ReserveOriginalForAudio(originalLane));
        const auto rootInfo = engine.playableInstances.Classify(old.episode.parent);
        ASSERT_TRUE(rootInfo.live);
        ASSERT_EQ(harken::PlayableLifecycle::Active, rootInfo.lifecycle);
        harken::FrontierOwnerIdentity actualOwner{};
        actualOwner.child = actualOwner.invocation = old.episode.parent;
        actualOwner.childEpoch = actualOwner.invocationEpoch = rootInfo.scheduleEpoch;
        actualOwner.serviceSlot = 0;
        actualOwner.serviceGeneration = 1;
        std::array<const harken::TimelineProgramView*, 2> controller{};
        for (std::uint32_t i = 0; i < controller.size(); ++i) {
            controller[i] = engine.decisions.PrepareFrontierControlProgram(old.decisions[i * 2u], 17);
            ASSERT_NE(nullptr, controller[i]);
            ASSERT_EQ(harken::TimelineProgramRole::ControllerActivation, controller[i]->role);
        }
        old.payload.kind = harken::TimelineEventKind::Start;
        old.payload.target.subjectId = kSound;
        old.program = {17, &old.payload, 1, harken::TimelineProgramRole::Voice};
        old.start = {};
        old.start.event.program = {17, &old.program};
        old.start.event.armToken = old.episode.source.arm;
        old.start.event.subeventPhase = 20;
        old.start.event.logicCommitSequence = old.logicSequence;
        old.start.event.ownerInstanceHandle = old.episode.parent;
        old.start.event.ownerInstanceScheduleEpoch = rootInfo.scheduleEpoch;
        old.start.event.frontier.kind = harken::FrontierOperationKind::Entry;
        old.start.event.frontier.entry = {old.decisions[0], 1, actualOwner};
        old.start.event.frontier.owner = actualOwner;
        old.start.event.frontier.group = old.groups[0];
        old.start.event.frontier.transport = engine.timelinePool.Epoch();
        old.start.event.frontier.world = old.episode.decisionRequest.world;
        old.start.event.frontier.operationEventCount = 1;
        old.start.event.frontier.entryEventOrdinal = 1;
        std::array<harken::ImmediateLaneEvent, 3> events{};
        events[1] = old.start;
        for (std::uint32_t i = 0; i < controller.size(); ++i) {
            auto& entry = events[i == 0 ? 0u : 2u];
            const auto primary = i * 2u;
            entry.relativeFrame = i * conditionalRelative;
            entry.event.program = {controller[i]->generation, controller[i]};
            entry.event.subeventPhase = 10;
            entry.event.logicCommitSequence = old.logicSequence;
            entry.event.ownerInstanceHandle = old.episode.parent;
            entry.event.ownerInstanceScheduleEpoch = rootInfo.scheduleEpoch;
            entry.event.frontier.kind = harken::FrontierOperationKind::Entry;
            entry.event.frontier.entry = {old.decisions[primary], primary + 1u, actualOwner};
            entry.event.frontier.owner = actualOwner;
            entry.event.frontier.group = old.groups[primary];
            entry.event.frontier.transport = engine.timelinePool.Epoch();
            entry.event.frontier.world = old.episode.decisionRequest.world;
            entry.event.frontier.operationEventCount = 1;
            entry.event.frontier.entryEventOrdinal = 0;
        }
        ASSERT_TRUE(engine.immediateActionLane.Prepare(old.lane, events.data(),
            static_cast<std::uint32_t>(events.size())));
        ASSERT_TRUE(engine.immediateActionLane.TryPlace(old.lane, firstAnchor, 0, &old.anchor, &old.placement));
        ASSERT_EQ(firstAnchor, old.anchor);
        ASSERT_TRUE(engine.timelinePool.RebaseImmediateLaneGroups(old.cohort, old.anchor));
        for (std::uint32_t index = 0; index < old.groups.size(); ++index) {
            const auto primary = index < 2u ? 0u : 2u;
            harken::FrontierGroupBinding binding{};
            binding.role = index == primary ? harken::FrontierGroupRole::Coordinator :
                harken::FrontierGroupRole::EntryMember;
            binding.entry = {old.decisions[primary], primary + 1u, actualOwner};
            binding.operation.kind = index == 3u ? harken::FrontierOperationKind::Dependent :
                harken::FrontierOperationKind::Entry;
            binding.operation.owner = actualOwner;
            binding.operation.order.logicCommitSequence = old.logicSequence;
            binding.operation.order.executeFrame = old.anchor + old.inputs[index].executeFrame - old.provisionalAnchor;
            binding.operation.order.subeventPhase = 10;
            binding.operation.group = old.groups[index];
            binding.operation.transport = engine.timelinePool.Epoch();
            binding.operation.world = old.episode.decisionRequest.world;
            binding.primary = old.groups[primary];
            if (index == 1u) binding.queueInitial = {old.queue.slot, old.queue.generation, 1, old.fingerprint};
            if (index == 2u) {
                binding.prerequisite = {old.decisions[0], 1, actualOwner};
                binding.prerequisiteMember = old.groups[1];
            }
            if (index == 3u) binding.queueConditional = {old.queue.slot, old.queue.generation,
                1, old.version.version, old.episode.occurrence, old.episode.occurrence};
            if (index == primary) {
                harken::FrontierActivationDescriptor descriptor{};
                descriptor.entry = binding.entry;
                descriptor.primary = binding.primary;
                descriptor.order = binding.operation.order;
                descriptor.worldGeneration = binding.operation.world;
                descriptor.worldSemanticFingerprint = old.fingerprint;
                descriptor.expectedMemberCount = 1;
                descriptor.expectedEntryEventCount = index == 0u ? 2u : 1u;
                descriptor.programGeneration = controller[index / 2u]->generation;
                ASSERT_TRUE(engine.decisions.PrepareFrontierActivation(old.decisions[index],
                    descriptor, engine.timelinePool));
            }
            ASSERT_TRUE(engine.timelinePool.BindPlan03GroupFrontier(old.groups[index], binding));
            harken::Plan03PageGroupView actual{};
            ASSERT_TRUE(engine.timelinePool.ResolvePlan03GroupSliceProducer(old.groups[index], &actual));
            ASSERT_NE(nullptr, actual.frontier);
            // These actual factories issue empty native groups; no member is invented.
            ASSERT_EQ(0u, actual.memberCount);
            ASSERT_EQ(0u, actual.sourceControlCount);
            ASSERT_TRUE(engine.armGroups.Reserve(old.decisions[index], nullptr, 0, &old.armGroups[index]));
            harken::ArmGroupSnapshot actualArms{};
            ASSERT_TRUE(engine.armGroups.Capture(old.armGroups[index], &actualArms));
            ASSERT_EQ(harken::ArmGroupState::Ready, actualArms.state);
            ASSERT_EQ(0u, actualArms.memberCount);
            ASSERT_EQ(0u, actualArms.ackedCount);
            harken::DecisionParticipantRef participant{};
            if (index == primary) {
                participant.kind = harken::DecisionParticipantKind::Subdecision;
                participant.slot = old.decisions[index + 1u].slot;
                participant.generation = old.decisions[index + 1u].generation;
            } else {
                participant.kind = harken::DecisionParticipantKind::QueueInstance;
                participant.slot = old.queue.slot;
                participant.generation = old.queue.generation;
            }
            harken::DecisionCommitPayload commit{};
            commit.pages = {static_cast<std::uint32_t>(old.groups[index].committedRow),
                old.groups[index].rowClaimGeneration};
            commit.armGroup = old.armGroups[index];
            commit.participants = &participant;
            commit.participantCount = 1;
            ASSERT_TRUE(engine.decisions.Commit(old.decisions[index], commit));
            if (index == 1u || index == 3u)
                ASSERT_TRUE(engine.decisions.BindFrontierDecision(old.decisions[index],
                    *actual.frontier, engine.timelinePool));
            if (index == 1u)
                ASSERT_TRUE(engine.queueInstances.BindFrontierInitial(old.queue, *actual.frontier, engine.timelinePool));
            if (index == 3u) {
                if (selectedReplacement) {
                    EXPECT_FALSE(engine.queueInstances.BindFrontierConditional(old.queue, *actual.frontier, engine.timelinePool));
                    ASSERT_TRUE(engine.BindInitialOriginalQueueConditional(old.original, old.parent, *actual.frontier));
                } else ASSERT_TRUE(engine.queueInstances.BindFrontierConditional(old.queue, *actual.frontier, engine.timelinePool));
            }
        }
        ASSERT_TRUE(engine.timelineArms.BindPendingImmediateStart(old.episode.source.arm, old.anchor, old.anchor));
        ASSERT_TRUE(engine.immediateActionLane.PinArms(old.lane, engine.timelineArms));
        ASSERT_TRUE(engine.immediateActionLane.PinGroups(old.lane, engine.timelinePool,
            engine.decisions, engine.config.maxPlan03GroupHeadersPerPage));
        harken::Plan03PageGroupView initial{};
        ASSERT_TRUE(engine.timelinePool.ResolvePlan03GroupSliceProducer(old.groups[0], &initial));
        ASSERT_TRUE(engine.timelineArms.BindFrontierAdmission(old.episode.source.arm,
            *initial.frontier, *initial.lease));
        ASSERT_TRUE(engine.timelinePool.ProvePrivateOriginalQueueGroupClock(old.Original(1), old.Original(3),
            &old.placement, &old.privateClock));
        ASSERT_FALSE(old.placement.Valid());
        ASSERT_TRUE(old.privateClock.Valid());
    }
    void PublishOriginalForAudio(bool placementReturned = false) {
        auto& engine = old.engine;
        if (placementReturned) {
            ASSERT_FALSE(old.privateClock.Valid());
            ASSERT_FALSE(old.placement.Valid());
        } else {
            ASSERT_TRUE(old.privateClock.ReturnPlacement(&old.placement));
            old.placement.Release();
        }
        ASSERT_TRUE(engine.timelineArms.Publish(old.episode.source.arm));
        for (const auto index : {0u, 2u}) {
            ASSERT_TRUE(engine.decisions.CanPublishFrontierActivation(old.decisions[index]));
            ASSERT_TRUE(engine.decisions.PublishFrontierActivation(old.decisions[index]));
        }
        ASSERT_TRUE(engine.timelinePool.PublishImmediateLaneGroups(old.cohort));
        ASSERT_TRUE(engine.immediateActionLane.Publish(old.lane));
        engine.AcceptSourceCommandPublications();
    }
    void ReconcileOriginalAudioEntries() {
        auto& engine = old.engine;
        const auto completed = engine.mixer.BlocksRendered();
        for (const auto index : {0u, 2u}) {
            const auto* activation = engine.decisions.ResolveFrontierActivation(old.decisions[index]);
            ASSERT_NE(nullptr, activation);
            harken::FrontierActivationReceipt actual{};
            ASSERT_EQ(harken::FrontierReceiptRead::Ready,
                activation->Read(activation->Descriptor().entry, completed, &actual));
            ASSERT_EQ(harken::FrontierReceiptOutcome::Applied, actual.outcome);
            ASSERT_EQ(old.anchor / kFrames + 1u + index / 2u, actual.audioSequence);
        }
        // Real callback advancement supplies the strictly later native reader fence.
        engine.nullBackend.Pump(1);
        ASSERT_EQ(completed + 1u, engine.mixer.BlocksRendered());
        for (const auto index : {0u, 2u}) {
            const auto* activation = engine.decisions.ResolveFrontierActivation(old.decisions[index]);
            ASSERT_NE(nullptr, activation);
            const auto entry = activation->Descriptor().entry;
            harken::FrontierActivationReceipt actual{};
            ASSERT_TRUE(engine.decisions.ReconcileFrontierActivation(old.decisions[index],
                entry, engine.mixer.BlocksRendered(), &actual));
            ASSERT_EQ(harken::FrontierReceiptOutcome::Applied, actual.outcome);
            harken::FrontierActivationSidecarSnapshot tail{};
            ASSERT_TRUE(engine.decisions.CaptureFrontierActivationSidecar(old.decisions[index],
                entry, engine.mixer.BlocksRendered(), &tail));
            EXPECT_FALSE(tail.pagePinHeld);
            EXPECT_FALSE(tail.observerPinHeld);
        }
    }
    void PrepareCandidate(bool retainCurrent = false, const harken::PreparedActionFiring* inherited = nullptr,
        const harken::QueueInstanceTable::PreparedCatalogResetScope* sharedScope = nullptr,
        const harken::LogicCommitCoordinator::PreparedIssue* sharedIssue = nullptr,
        harken::PrivateSelectedQueueFutureDomain* domain = nullptr,
        const harken::OriginalImmediateLaneAttemptLifetimePin* originalLane = nullptr,
        GenuinePreparedSingleEpisode* newSource = nullptr, bool freezeSourceHead = true) {
        retainedCurrent = retainCurrent;
        auto& engine = old.engine;
        harken::QueueResetTargetV1 target{};
        target.world = old.episode.decisionRequest.world;
        target.containerStableId = harkentest::ObjectId(13);
        target.programSemanticFingerprint = old.fingerprint;
        if (sharedScope == nullptr) {
            ASSERT_EQ(harken::QueueResetStatusV1::Ok,
                engine.queueInstances.PrepareCatalogResetScope(target, &scope));
            ASSERT_EQ(1u, scope.MatchedCount());
        }
        const auto& matched = sharedScope == nullptr ? scope : *sharedScope;
        if (inherited == nullptr && sharedIssue == nullptr) ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued,
            engine.logicCommits.PrepareIssue(&sequence));
        const harken::PreparedQueueReplacementOrder order = inherited == nullptr
            ? harken::PreparedQueueReplacementOrder(sharedIssue == nullptr ? sequence : *sharedIssue)
            : harken::PreparedQueueReplacementOrder(*inherited);
        ASSERT_TRUE(order.Valid());
        if (domain != nullptr) {
            ASSERT_NE(nullptr, originalLane);
            harken::OriginalImmediateLaneAttemptLifetimePin foreign;
            harken::PrivateSelectedQueueFutureDomain refused;
            EXPECT_FALSE(engine.timelinePool.PreparePrivateSelectedQueueFutureDomain(
                old.Original(1), old.Original(3), foreign, order, engine.immediateActionLane, &refused));
            ASSERT_TRUE(engine.timelinePool.PreparePrivateSelectedQueueFutureDomain(
                old.Original(1), old.Original(3), *originalLane, order, engine.immediateActionLane, domain));
            root = domain->RootObservation();
        }
        auto request = old.episode.decisionRequest;
        ASSERT_TRUE(engine.decisions.PrepareReservation(request, &decision, &decisionOwner));
        ASSERT_TRUE(engine.decisions.ProvePreparedReservationCommit(decision, request,
            order.Sequence(), &decisionProof));
        ASSERT_TRUE(engine.decisions.PrepareReservation(request, &coordinator, &coordinatorOwner));
        ASSERT_TRUE(engine.decisions.ProvePreparedReservationCommit(coordinator, request,
            order.Sequence(), &coordinatorProof));
        token = {coordinator.Token(), decision.Token()};
        if (newSource != nullptr) {
            ASSERT_FALSE(retainedCurrent);
            ASSERT_TRUE(newSource->Prepare(engine, false, false, true, &old.episode, {token[1]}, freezeSourceHead));
            ASSERT_TRUE(engine.decisions.ProvePreparedReservationCommit(newSource->decisionPreparation,
                newSource->decisionRequest, order.Sequence(), &sourceCompletionTicket));
            successor = newSource->occurrence;
            completionOwner = newSource->owner;
        } else if (!retainedCurrent) {
            ASSERT_TRUE(engine.timelineOccurrences.PrepareAcquire({old.episode.arranging, kSound, 0, 9}, &successor));
            const auto epoch = engine.timelineOccurrences.ScheduleEpochOf(successor);
            completionOwner = harken::CompletionOwnerRef::Single(decision.Token(), epoch);
            const auto generation = static_cast<std::uint32_t>(old.episode.source.source >> 32);
            ASSERT_TRUE(engine.timelineOccurrences.BindCompletionProgress(successor, generation, epoch));
            ASSERT_TRUE(engine.timelineOccurrences.TryPinCompletionOwner(successor, completionOwner, &successorOwner));
            auto* actualProgress = engine.timelineOccurrences.CompletionProgressOf(successor);
            ASSERT_NE(nullptr, actualProgress);
            ASSERT_TRUE(engine.timelineOccurrences.ProvePreparedSingleCompletionCommit(successorOwner,
                completionOwner, actualProgress, generation, epoch, &successorProof));
        }
        for (std::uint32_t i = 0; i < 2; ++i) {
            input[i].decision = token[i];
            input[i].executeFrame = old.provisionalAnchor + kFrames;
            if (i == 1u && newSource != nullptr) {
                input[i].members = &newSource->source.arm;
                input[i].memberCount = 1;
            }
            if (domain == nullptr) {
                ASSERT_TRUE(engine.ReserveOriginalQueueLaneGroup(root, old.provisionalAnchor,
                    input[i], engine.timelineArms, &group[i]));
            } else {
                harken::PreparedPrivateImmediateLaneGroup original;
                ASSERT_TRUE(domain->ReserveGroup(old.provisionalAnchor, input[i], engine.timelineArms,
                    &group[i], &original));
                harken::PrivateImmediateLaneGroupTransferProof transfer;
                ASSERT_TRUE(engine.timelinePool.ProvePrivateImmediateLaneGroupTransfer(original, root,
                    old.provisionalAnchor, input[i], engine.timelineArms, &transfer));
                ASSERT_TRUE(engine.timelinePool.ConsumePrivateImmediateLaneGroupTransfer(std::move(transfer),
                    &engine.originalQueueGroupHolders[group[i].committedRow]));
            }
        }
        if (retainedCurrent) {
            ASSERT_TRUE(spans.PrepareRetainedCurrent(engine.queueInstances, matched, old.parent,
                old.original, old.clock, order, old.episode.originalDecision, old.episode.originalOccurrence,
                engine.decisions, decision, decisionOwner, decisionProof, coordinator, coordinatorOwner,
                coordinatorProof, engine.timelineOccurrences, engine.timelinePool, Original(0), Original(1), &owner, domain));
        } else if (newSource != nullptr) {
            const harken::PrivateSelectedQueueFutureSpanPool::SourceCompletionFactory completion{
                newSource->decisionPreparation, newSource->originalDecision, sourceCompletionTicket};
            ASSERT_TRUE(spans.Prepare(engine.queueInstances, matched, old.parent, old.original, old.clock,
                order, old.episode.originalDecision, old.episode.originalOccurrence,
                engine.decisions, decision, decisionOwner, decisionProof, coordinator, coordinatorOwner, coordinatorProof,
                engine.timelineOccurrences, newSource->originalOccurrence, newSource->occurrenceProof,
                engine.timelinePool, Original(0), Original(1), &owner, domain, &completion));
        } else ASSERT_TRUE(spans.Prepare(engine.queueInstances, matched, old.parent, old.original, old.clock,
            order, old.episode.originalDecision, old.episode.originalOccurrence,
            engine.decisions, decision, decisionOwner, decisionProof, coordinator, coordinatorOwner, coordinatorProof,
            engine.timelineOccurrences, successorOwner, successorProof, engine.timelinePool,
            Original(0), Original(1), &owner, domain));
        ASSERT_TRUE(spans.FreezeUnbound(owner));
        ASSERT_TRUE(owner.BorrowNativeConditional(&read));
    }
    void CancelCandidate() {
        auto& engine = old.engine;
        ASSERT_TRUE(read.Release());
        owner.Release();
        successorProof.Release();
        decisionProof.Release();
        coordinatorProof.Release();
        ASSERT_TRUE(engine.CancelOriginalQueueLaneGroups(root, engine.mixer.BlocksRendered()));
        if (!retainedCurrent) {
            ASSERT_TRUE(engine.timelineOccurrences.RollbackCompletionOwnerPinExact(successor, completionOwner));
            ASSERT_TRUE(engine.timelineOccurrences.UnbindCompletionProgress(successor,
                static_cast<std::uint32_t>(old.episode.source.source >> 32)));
            ASSERT_TRUE(engine.timelineOccurrences.CancelPreparedAcquire(successor, &successorOwner));
        }
        ASSERT_TRUE(engine.decisions.CancelPreparedReservation(&decision, &decisionOwner));
        ASSERT_TRUE(engine.decisions.CancelPreparedReservation(&coordinator, &coordinatorOwner));
        sequence.Release();
        scope.Release();
    }
    void Prepare(bool retainCurrent = false, bool afterOriginalPublication = false) {
        auto& engine = old.engine;
        ASSERT_NO_FATAL_FAILURE(old.PrepareBeforeHead());
        ASSERT_NO_FATAL_FAILURE(old.ReserveUnboundGroups());
        if (!afterOriginalPublication) ASSERT_NO_FATAL_FAILURE(PrepareCandidate(retainCurrent));
        ASSERT_NO_FATAL_FAILURE(old.PublishSourceAndObserveFreshAck());
        ASSERT_NO_FATAL_FAILURE(PlaceOriginalForAudio());
        ASSERT_TRUE(old.Bind());
        if (afterOriginalPublication) {
            ASSERT_NO_FATAL_FAILURE(PublishOriginalForAudio());
            EXPECT_FALSE(old.privateClock.Valid());
            ASSERT_NO_FATAL_FAILURE(PrepareCandidate(retainCurrent));
            harken::QueueInstanceTable::OriginalConditionalLaneClock absent;
            EXPECT_FALSE(spans.BindOriginalFirstS(owner, absent));
            ASSERT_TRUE(spans.BindOriginalFirstS(owner, old.clock));
            EXPECT_FALSE(spans.BindOriginalFirstS(owner, old.clock));
        } else {
            ASSERT_TRUE(spans.BindOriginalFirstS(owner, old.privateClock, engine.timelineArms, engine.decisions));
            EXPECT_FALSE(spans.BindOriginalFirstS(owner, old.privateClock, engine.timelineArms, engine.decisions));
        }
        harken::DecisionTable::ConsumePreparedReservationCommit(std::move(decisionProof));
        harken::DecisionTable::ConsumePreparedReservationCommit(std::move(coordinatorProof));
        if (!retainedCurrent)
            harken::OccurrenceStatePool::ConsumePreparedSingleCompletionCommit(std::move(successorProof));
        const auto reservedSequence = sequence.Sequence();
        engine.logicCommits.ConsumePreparedIssue(std::move(sequence));
        ASSERT_EQ(reservedSequence, engine.logicCommits.Current());
        ASSERT_FALSE(sequence.Valid());
        ASSERT_TRUE(engine.immediateActionLane.Reserve(root, {0, 2, 0, 1}, &lane));
        for (std::uint32_t i = 0; i < 2; ++i) {
            ASSERT_TRUE(engine.timelinePool.BindImmediateLaneGroupOwner(group[i],
                engine.immediateActionLane, root, lane.slot, lane.generation));
            ASSERT_TRUE(engine.immediateActionLane.DeclareGroup(lane, {group[i], kFrames}));
        }
        ASSERT_TRUE(engine.immediateActionLane.Prepare(lane, nullptr, 0));
        std::uint64_t newAnchor = UINT64_MAX;
        ASSERT_TRUE(engine.immediateActionLane.TryPlace(lane, old.anchor, 0, &newAnchor));
        ASSERT_EQ(old.anchor, newAnchor);
        ASSERT_TRUE(engine.timelinePool.RebaseImmediateLaneGroups(root, newAnchor));
        const auto rootInfo = engine.playableInstances.Classify(old.episode.parent);
        ASSERT_TRUE(rootInfo.live);
        harken::FrontierOwnerIdentity actualOwner{};
        actualOwner.child = actualOwner.invocation = old.episode.parent;
        actualOwner.childEpoch = actualOwner.invocationEpoch = rootInfo.scheduleEpoch;
        actualOwner.serviceSlot = 0; actualOwner.serviceGeneration = 1;
        harken::PrivateSelectedQueueFutureSpanPool::ConditionalBacking body;
        ASSERT_TRUE(read.Capture(&body));
        for (std::uint32_t i = 0; i < 2; ++i) {
            harken::FrontierGroupBinding binding{};
            binding.role = i == 0 ? harken::FrontierGroupRole::Coordinator : harken::FrontierGroupRole::EntryMember;
            binding.entry = {token[0], 7, actualOwner};
            binding.operation.kind = i == 0 ? harken::FrontierOperationKind::Entry : harken::FrontierOperationKind::Dependent;
            binding.operation.owner = actualOwner;
            binding.operation.order.logicCommitSequence = body.sequence;
            binding.operation.order.executeFrame = old.anchor + kFrames;
            binding.operation.group = group[i];
            binding.operation.world = old.episode.decisionRequest.world;
            binding.operation.transport = engine.timelinePool.Epoch();
            binding.primary = group[0];
            if (i == 1) binding.queueConditional = {old.queue.slot, old.queue.generation,
                body.currentState.version, body.version.version, body.current, body.successor};
            ASSERT_TRUE(engine.timelinePool.BindPlan03GroupFrontier(group[i], binding));
            ASSERT_TRUE(engine.armGroups.Reserve(token[i], nullptr, 0, &armGroup[i]));
            harken::DecisionParticipantRef participant{};
            participant.kind = harken::DecisionParticipantKind::QueueInstance;
            participant.slot = old.queue.slot; participant.generation = old.queue.generation;
            harken::DecisionCommitPayload commit{};
            commit.pages = {static_cast<std::uint32_t>(group[i].committedRow), group[i].rowClaimGeneration};
            commit.armGroup = armGroup[i]; commit.participants = &participant; commit.participantCount = 1;
            ASSERT_TRUE(engine.decisions.Commit(token[i], commit));
        }
        ASSERT_TRUE(engine.immediateActionLane.PinArms(lane, engine.timelineArms));
        ASSERT_TRUE(engine.immediateActionLane.PinGroups(lane, engine.timelinePool,
            engine.decisions, engine.config.maxPlan03GroupHeadersPerPage));
        ASSERT_TRUE(engine.timelinePool.PublishImmediateLaneGroups(root));
        // Only Old is published to the actual audio Lane. New backing is native
        // readable but has no Catalog selector or public Queue state effect.
        if (!afterOriginalPublication) ASSERT_NO_FATAL_FAILURE(PublishOriginalForAudio());
        const auto wanted = old.anchor / kFrames + 1u;
        ASSERT_GT(wanted, engine.mixer.BlocksRendered());
        engine.nullBackend.Pump(static_cast<std::uint32_t>(wanted - engine.mixer.BlocksRendered()));
        ASSERT_EQ(wanted, engine.mixer.BlocksRendered());
        ASSERT_GT(CurrentPeak(engine), 0.1f);
        const auto* initialActivation = engine.decisions.ResolveFrontierActivation(old.decisions[0]);
        ASSERT_NE(nullptr, initialActivation);
        harken::FrontierActivationReceipt initialReceipt{};
        ASSERT_EQ(harken::FrontierReceiptRead::Ready,
            initialActivation->Read(initialActivation->Descriptor().entry, wanted, &initialReceipt));
        ASSERT_EQ(harken::FrontierReceiptOutcome::Applied, initialReceipt.outcome);
        ASSERT_EQ(wanted, initialReceipt.audioSequence);
        harken::QueueInstanceSnapshot currentQueue{};
        ASSERT_TRUE(engine.queueInstances.Capture(old.queue, &currentQueue));
        ASSERT_EQ(wanted, currentQueue.initialActivationSequence);
        for (const auto index : {0u, 1u}) {
            harken::Plan03PageGroupView admittedGroup{};
            ASSERT_TRUE(engine.timelinePool.ResolvePlan03GroupSliceRt(old.groups[index], &admittedGroup));
            ASSERT_NE(nullptr, admittedGroup.lease);
            ASSERT_EQ(harken::AudioSliceState::SliceApplied, admittedGroup.lease->Capture().state);
        }
        harken::CompletionProgressSnapshot actual;
        ASSERT_TRUE(harken::CaptureCompletionProgressExact(*old.episode.progress, old.episode.occurrence,
            static_cast<std::uint32_t>(old.episode.source.source >> 32), &actual));
        harken::TimelineArmObservation actualSourceArm;
        ASSERT_TRUE(engine.timelineArms.Observe(old.episode.source.arm, &actualSourceArm));
        ASSERT_EQ(kFrames, actual.logicalOutputFramesProduced)
            << "actual source state=" << static_cast<unsigned>(actualSourceArm.state)
            << " started=" << actualSourceArm.startedBlockSequence
            << " completed=" << actual.completedThroughBlockSequence;
        ASSERT_EQ(wanted, actual.completedThroughBlockSequence);
        ASSERT_EQ(wanted, actualSourceArm.startedBlockSequence);
        ASSERT_TRUE(engine.voices.ReleaseObserved(old.episode.source.release));
        harken::TimelineArmObservation observed;
        ASSERT_TRUE(engine.timelineArms.Observe(old.episode.source.arm, &observed));
        ASSERT_EQ(harken::ArmState::Tombstoned, observed.state);
        ASSERT_NE(0u, observed.startedBlockSequence);
    }
    void Cleanup() {
        auto& engine = old.engine;
        ASSERT_TRUE(read.Release());
        owner.Release();
        ASSERT_TRUE(engine.immediateActionLane.Cancel(lane));
        ASSERT_TRUE(engine.immediateActionLane.Retire(lane, engine.mixer.BlocksRendered()));
        for (std::uint32_t i = 2; i != 0; --i)
            ASSERT_TRUE(engine.timelinePool.DetachPlan03GroupFrontier(group[i - 1u], engine.mixer.BlocksRendered()));
        ASSERT_TRUE(engine.CancelOriginalQueueLaneGroups(root, engine.mixer.BlocksRendered()));
        if (!retainedCurrent) {
            ASSERT_TRUE(engine.timelineOccurrences.ClearCompletionOwnerExact(successor, completionOwner));
            ASSERT_TRUE(engine.timelineOccurrences.UnbindCompletionProgress(successor,
                static_cast<std::uint32_t>(old.episode.source.source >> 32)));
            ASSERT_TRUE(engine.timelineOccurrences.Advance(successor, harken::OccurrenceStage::AwaitingEventFence));
            ASSERT_TRUE(engine.timelineOccurrences.ReleaseAfterFence(successor, engine.mixer.BlocksRendered(), &successorOwner));
        }
        for (std::uint32_t i = 0; i < 2; ++i) {
            (void)engine.decisions.Invalidate(token[i]);
            ASSERT_TRUE(engine.decisions.Retire(token[i], engine.mixer.BlocksRendered()));
            ASSERT_TRUE(engine.decisions.ReclaimRetired(token[i], i == 0 ? &coordinatorOwner : &decisionOwner));
            ASSERT_TRUE(engine.armGroups.BeginRetire(armGroup[i]));
            ASSERT_TRUE(engine.armGroups.Reclaim(armGroup[i], engine.mixer.BlocksRendered()));
        }
        scope.Release();
        harken::QueueInstanceSnapshot actualOldTail{};
        ASSERT_TRUE(engine.queueInstances.Capture(old.queue, &actualOldTail));
        ASSERT_EQ(old.version.version, actualOldTail.pendingVersion);
        ASSERT_EQ(harken::ConditionalQueueActivation::Activated, actualOldTail.pendingActivation);
        ASSERT_TRUE(old.original.Valid());
        EXPECT_EQ(old.queue.slot, old.original.Token().slot);
        EXPECT_EQ(old.queue.generation, old.original.Token().generation);
        EXPECT_EQ(old.version.version, old.original.Version().version);
        EXPECT_FALSE(engine.queueInstances.IsCurrentOriginalConditionalFutureReservation(old.original));
        // The actual audio activation ends Waiting, but it does not end the
        // producer-owned original episode or return its two real group loans.
        ASSERT_TRUE(engine.queueInstances.RetireConditionalForAbort(old.queue, old.version, old.decisions[3]));
        ASSERT_TRUE(engine.queueInstances.Capture(old.queue, &actualOldTail));
        EXPECT_EQ(0u, actualOldTail.pendingVersion);
        EXPECT_TRUE(old.original.Valid());
        BoundClockState actualClockTail = BoundClockState::Unbound;
        ASSERT_TRUE(old.clock.CaptureState(&actualClockTail));
        ASSERT_EQ(BoundClockState::Retired, actualClockTail);
        ASSERT_NO_FATAL_FAILURE(ReconcileOriginalAudioEntries());
        ASSERT_TRUE(engine.immediateActionLane.Cancel(old.lane));
        ASSERT_TRUE(engine.immediateActionLane.Retire(old.lane, engine.mixer.BlocksRendered()));
        for (std::uint32_t i = 4; i != 0; --i)
            ASSERT_TRUE(engine.timelinePool.DetachPlan03GroupFrontier(old.groups[i - 1], engine.mixer.BlocksRendered()));
        ASSERT_TRUE(engine.decisions.ReleaseFrontierDecisionBinding(old.decisions[1]));
        ASSERT_TRUE(engine.decisions.ReleaseFrontierDecisionBinding(old.decisions[3]));
        engine.ledger.Reap(old.episode.source.source);
        ASSERT_TRUE(engine.timelineOccurrences.ClearCompletionOwnerExact(old.episode.occurrence, old.episode.owner));
        harken::CompletionProgressSnapshot actual;
        ASSERT_TRUE(harken::CaptureCompletionProgressExact(*old.episode.progress, old.episode.occurrence,
            static_cast<std::uint32_t>(old.episode.source.source >> 32), &actual));
        old.terminalSequence = actual.completedThroughBlockSequence;
        ASSERT_TRUE(engine.timelineOccurrences.UnbindCompletionProgress(old.episode.occurrence,
            static_cast<std::uint32_t>(old.episode.source.source >> 32)));
        ASSERT_TRUE(engine.timelineOccurrences.Advance(old.episode.occurrence, harken::OccurrenceStage::AwaitingEventFence));
        ASSERT_NO_FATAL_FAILURE(old.ReturnAllAndReclaim());
        EXPECT_EQ(0u, engine.voices.ActiveCount());
    }
};

void CheckHiddenFutureRetainedCurrent(bool afterOriginalPublication) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto& engine = *owned;
    ASSERT_NO_FATAL_FAILURE(RegisterInitialClockPcm(engine));
    HiddenBoundNativeFuture future(engine);
    ASSERT_NO_FATAL_FAILURE(future.Prepare(true, afterOriginalPublication));
    EXPECT_FALSE(future.successorOwner.Valid());
    EXPECT_FALSE(future.successorProof.Valid());
    harken::PrivateSelectedQueueFutureSpanPool::Observation selected{};
    ASSERT_TRUE(future.owner.Capture(&selected));
    EXPECT_EQ(future.old.episode.occurrence.slot, selected.newSuccessor.slot);
    EXPECT_EQ(future.old.episode.occurrence.generation, selected.newSuccessor.generation);
    EXPECT_TRUE(harken::SameCompletionOwner(future.old.episode.owner, selected.newSuccessorOwner));
    const auto completed = engine.mixer.BlocksRendered();
    const auto start = completed * kFrames;
    harken::ImmediateLaneTimelineFinalizer finalizer(engine.voices, engine.timelineArms);
    ASSERT_TRUE(finalizer.Attach(engine.immediateActionLane, kFrames));
    (void)finalizer.Capture(completed);
    harken::BlockArena arena(4096);
    harken::NativeTimelineQuantumAdmissionProof admitted;
    ASSERT_TRUE(finalizer.ProveQuantumAdmission(arena, kFrames, &admitted));
    harken::Plan03PageGroupView native;
    ASSERT_TRUE(future.spans.ResolveNativeConditionalGroup(future.read, &native));
    harken::PrivateSelectedQueueFutureSpanPool::NativeConditionalProof proof;
    ASSERT_EQ(harken::QueueConditionalProofStatus::ReadyToActivate,
        future.spans.ProveNativeConditional(future.read, *native.frontier, *native.lease,
            engine.timelineOccurrences, start, start + kFrames, completed, admitted, &proof));
    EXPECT_TRUE(proof.Valid());
    harken::CompletionOwnerRef actualOwner{};
    ASSERT_TRUE(engine.timelineOccurrences.CaptureCompletionOwner(selected.current, &actualOwner));
    EXPECT_TRUE(harken::SameCompletionOwner(future.old.episode.owner, actualOwner));
    proof.Release();
    ASSERT_EQ(harken::AudioSliceInvalidateDisposition::InvalidatedReady,
        native.lease->Invalidate(harken::AudioSliceInvalidateReason::Cancel));
    admitted.Release();
    finalizer.Discard();
    // The hidden candidate remains private; the next real callback runs Old.
    engine.nullBackend.Pump(1);
    ASSERT_NO_FATAL_FAILURE(future.Cleanup());
}

TEST(OriginalSourceSubmission, HiddenFutureRetainedCurrentUsesOriginalClockAndCompletionWithoutNewSource) {
    ASSERT_NO_FATAL_FAILURE(CheckHiddenFutureRetainedCurrent(false));
}

TEST(OriginalSourceSubmission, HiddenFuturePreparedAfterPublicationUsesRetainedOriginalClockAndCompletion) {
    ASSERT_NO_FATAL_FAILURE(CheckHiddenFutureRetainedCurrent(true));
}

TEST(OriginalSourceSubmission, HiddenFutureNativeFirstSReadyRequiresActualScratchAdmission) {
    static_assert(!std::is_copy_constructible_v<harken::NativeTimelineQuantumAdmissionProof>);
    static_assert(!std::is_constructible_v<harken::NativeTimelineQuantumAdmissionProof, bool>);
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto& engine = *owned;
    ASSERT_NO_FATAL_FAILURE(RegisterInitialClockPcm(engine));
    HiddenBoundNativeFuture future(engine);
    ASSERT_NO_FATAL_FAILURE(future.Prepare());
    const auto completed = engine.mixer.BlocksRendered();
    const auto start = completed * kFrames;
    ASSERT_EQ(future.old.anchor + kFrames, start);
    // This is another genuine accepted, installed, unstarted Source. It creates
    // actual timeline scratch pressure at this completed Current boundary.
    SourceBody gated;
    ASSERT_NO_FATAL_FAILURE(PublishSourceBody(engine, gated, false, false));
    engine.mixer.DrainCommandsForTest();
    EXPECT_EQ(completed, engine.mixer.BlocksRendered());
    ASSERT_NE(nullptr, engine.voices.Resolve(gated.source));
    ASSERT_GT(engine.voices.ScratchBytesBeforeTimeline(kFrames), 0u);
    harken::ImmediateLaneTimelineFinalizer finalizer(engine.voices, engine.timelineArms);
    ASSERT_TRUE(finalizer.Attach(engine.immediateActionLane, kFrames));
    (void)finalizer.Capture(completed);
    harken::ImmediateLaneObservation hiddenLane{};
    ASSERT_TRUE(engine.immediateActionLane.Observe(future.lane, &hiddenLane));
    ASSERT_FALSE(hiddenLane.committed);
    harken::Plan03PageGroupView native;
    EXPECT_FALSE(engine.timelinePool.ResolvePlan03GroupSliceRt(future.group[1], &native));
    harken::PrivateSelectedQueueFutureSpanPool foreignSpans(1);
    harken::PrivateSelectedQueueFutureSpanPool::NativeConditionalRead invalidRead;
    native.identity = future.group[0];
    EXPECT_FALSE(foreignSpans.ResolveNativeConditionalGroup(future.read, &native));
    EXPECT_EQ(future.group[0], native.identity);
    EXPECT_FALSE(future.spans.ResolveNativeConditionalGroup(invalidRead, &native));
    EXPECT_EQ(future.group[0], native.identity);
    ASSERT_TRUE(future.spans.ResolveNativeConditionalGroup(future.read, &native));
    ASSERT_EQ(future.group[1], native.identity);
    ASSERT_NE(nullptr, native.frontier);
    ASSERT_NE(nullptr, native.lease);
    harken::BlockArena refusedArena(0), admittedArena(4096);
    harken::NativeTimelineQuantumAdmissionProof admitted;
    EXPECT_FALSE(finalizer.ProveQuantumAdmission(refusedArena, kFrames, &admitted));
    EXPECT_FALSE(admitted.Valid());
    harken::PrivateSelectedQueueFutureSpanPool::NativeConditionalProof proof;
    EXPECT_EQ(harken::QueueConditionalProofStatus::Waiting,
        future.spans.ProveNativeConditional(future.read, *native.frontier, *native.lease,
            engine.timelineOccurrences, start, start + kFrames, completed, admitted, &proof));
    EXPECT_FALSE(proof.Valid());
    EXPECT_EQ(harken::AudioSliceState::Ready, native.lease->Capture().state);
    ASSERT_TRUE(finalizer.ProveQuantumAdmission(admittedArena, kFrames, &admitted));
    ASSERT_TRUE(admitted.Valid());
    EXPECT_TRUE(admitted.MatchesNativeDomain(&engine.voices, &engine.timelineArms, &admittedArena));
    EXPECT_FALSE(admitted.MatchesNativeDomain(&engine.voices, &engine.timelineArms, &refusedArena));
    ASSERT_TRUE(admitted.Matches(&engine.timelineArms, start, start + kFrames, completed));
    EXPECT_FALSE(admitted.Matches(&engine.timelineArms, start, start + kFrames, completed + 1u));
    harken::TimelineArmTable foreign(1);
    EXPECT_FALSE(admitted.Matches(&foreign, start, start + kFrames, completed));
    ASSERT_EQ(harken::QueueConditionalProofStatus::ReadyToActivate,
        future.spans.ProveNativeConditional(future.read, *native.frontier, *native.lease,
            engine.timelineOccurrences, start, start + kFrames, completed, admitted, &proof));
    ASSERT_TRUE(proof.Valid());
    EXPECT_FALSE(future.read.Release());
    // Ending the actual capture invalidates readiness, but it does not return
    // the native row gate or another actor's backing loan.
    finalizer.Discard();
    EXPECT_FALSE(admitted.Valid());
    EXPECT_FALSE(proof.Valid());
    EXPECT_FALSE(future.read.Release());
    EXPECT_EQ(harken::AudioSliceState::Ready, native.lease->Capture().state);
    proof.Release();
    admitted.Release();
    (void)finalizer.Capture(completed);
    ASSERT_TRUE(finalizer.ProveQuantumAdmission(admittedArena, kFrames, &admitted));
    ASSERT_EQ(harken::QueueConditionalProofStatus::ReadyToActivate,
        future.spans.ProveNativeConditional(future.read, *native.frontier, *native.lease,
            engine.timelineOccurrences, start, start + kFrames, completed, admitted, &proof));
    ASSERT_TRUE(proof.Valid());
    harken::QueueInstanceSnapshot oldBefore{};
    ASSERT_TRUE(engine.queueInstances.Capture(future.old.queue, &oldBefore));
    ASSERT_TRUE(native.lease->TryClaim());
    future.spans.ApplyNativeConditional(proof);
    ASSERT_TRUE(native.lease->MarkApplied());
    proof.Release();
    harken::QueueInstanceSnapshot oldAfter{};
    ASSERT_TRUE(engine.queueInstances.Capture(future.old.queue, &oldAfter));
    EXPECT_EQ(oldBefore.logicMirror.version, oldAfter.logicMirror.version);
    EXPECT_EQ(oldBefore.pendingVersion, oldAfter.pendingVersion);
    EXPECT_EQ(oldBefore.pendingActivation, oldAfter.pendingActivation);
    finalizer.Discard();
    EXPECT_FALSE(admitted.Valid());
    admitted.Release();
    // Stop the independent accepted Source by the existing real owner route.
    // This actual callback also completes the ordinary Old C1; no Catalog New
    // selection or public Reset success is inferred from the component Apply.
    ASSERT_NO_FATAL_FAILURE(RetireSource(engine, gated, false));
    ASSERT_NO_FATAL_FAILURE(future.Cleanup());
}

TEST(OriginalSourceSubmission, NativeScratchAdmissionMoveBorrowAndCaptureExitNeverForgeNewQuantum) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto& engine = *owned;
    ASSERT_NO_FATAL_FAILURE(RegisterInitialClockPcm(engine));
    GenuineInitialSingleClockHarness state(engine);
    ASSERT_NO_FATAL_FAILURE(state.PrepareBeforeHead());
    ASSERT_NO_FATAL_FAILURE(state.ReserveUnboundGroups());
    ASSERT_NO_FATAL_FAILURE(state.PublishSourceAndObserveFreshAck());
    ASSERT_NO_FATAL_FAILURE(state.PlaceAndHold());
    ASSERT_TRUE(state.Bind());
    harken::ImmediateLaneTimelineFinalizer finalizer(engine.voices, engine.timelineArms);
    ASSERT_TRUE(finalizer.Attach(engine.immediateActionLane, kFrames));
    harken::BlockArena refusedArena(0), admittedArena(4096);
    harken::NativeTimelineQuantumAdmissionProof proof;
    EXPECT_FALSE(finalizer.ProveQuantumAdmission(admittedArena, kFrames, &proof));
    (void)finalizer.Capture(engine.mixer.BlocksRendered());
    EXPECT_FALSE(finalizer.ProveQuantumAdmission(refusedArena, kFrames, &proof));
    ASSERT_TRUE(finalizer.ProveQuantumAdmission(admittedArena, kFrames, &proof));
    harken::NativeTimelineQuantumAdmissionProof child;
    ASSERT_TRUE(proof.Borrow(&child));
    EXPECT_FALSE(finalizer.Attach(engine.immediateActionLane, kFrames));
    EXPECT_FALSE(finalizer.ProveQuantumAdmission(admittedArena, kFrames, &proof));
    EXPECT_TRUE(proof.Valid());
    harken::NativeTimelineQuantumAdmissionProof moved(std::move(child));
    EXPECT_FALSE(child.Valid());
    EXPECT_TRUE(moved.Valid());
    finalizer.Finish();
    EXPECT_FALSE(proof.Valid());
    EXPECT_FALSE(moved.Valid());
    EXPECT_FALSE(proof.Borrow(&child));
    proof.Release(); moved.Release();
    ASSERT_TRUE(finalizer.Attach(engine.immediateActionLane, kFrames));
    const auto before = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    (void)finalizer.Capture(engine.mixer.BlocksRendered());
    ASSERT_TRUE(finalizer.ProveQuantumAdmission(admittedArena, kFrames, &proof));
    const auto start = engine.mixer.BlocksRendered() * kFrames;
    EXPECT_TRUE(proof.Matches(&engine.timelineArms, start, start + kFrames, engine.mixer.BlocksRendered()));
    finalizer.Discard();
    EXPECT_FALSE(proof.Valid());
    proof.Release();
    ASSERT_NO_FATAL_FAILURE(state.EndAndRetireSource());
    ASSERT_NO_FATAL_FAILURE(state.ReturnAllAndReclaim());
}
} // namespace


namespace {
// Genuine logical-boundary factories exercise Engine ownership only. They do
// not manufacture a physical Current, Source ACK, first-S or published Lane.
struct EngineOriginalConditionalHolderHarness {
    harken::Engine& engine;
    harken::PlayableInstanceHandle root = 0, current = 0, successor = 0;
    harken::OccurrenceToken currentOccurrence{}, successorOccurrence{};
    harken::PreparedQueueInstanceV1 acquired;
    harken::QueueInstanceToken queue{};
    harken::QueueInstanceCreateRequest create{};
    std::array<harken::DecisionToken, 2> decisions{};
    harken::ConditionalQueuePrepareRequest request{};
    harken::PreparedQueueConditionalV1 prepared;
    harken::ConditionalQueueVersionToken version{};
    std::array<harken::Plan03GroupSliceIdentity, 2> groups{};
    static constexpr std::uint64_t cohort = 47;
    bool initializing = false;

    explicit EngineOriginalConditionalHolderHarness(harken::Engine& actual) : engine(actual) {}

    void Acquire(bool beforeCommit = false) {
        initializing = beforeCommit;
        root = engine.playableInstances.Acquire(harken::PlayableKind::Container, 0);
        current = engine.playableInstances.Acquire(harken::PlayableKind::Sound, root);
        successor = engine.playableInstances.Acquire(harken::PlayableKind::Sound, root);
        ASSERT_NE(0u, root);
        ASSERT_NE(0u, current);
        ASSERT_NE(0u, successor);
        ASSERT_TRUE(engine.timelineOccurrences.Acquire({current, kSound, 0, 0}, &currentOccurrence));
        ASSERT_TRUE(engine.timelineOccurrences.Acquire({successor, kSound, 0, 0}, &successorOccurrence));
        create.world = 7;
        create.rootInstance = root;
        create.containerStableId = harkentest::ObjectId(13);
        create.programSemanticFingerprint = 53;
        create.childCount = 2;
        create.initial.version = 1;
        create.resetInitial = create.initial;
        ASSERT_TRUE(engine.PrepareOriginalQueueAcquire(create, &acquired));
        queue = acquired.Token();
        if (!initializing) ASSERT_TRUE(engine.queueInstances.CommitPreparedAcquire(&acquired, &queue));
        harken::DecisionReserveRequest reserve{};
        reserve.ticket.sequence = engine.logicCommits.Issue();
        reserve.world = create.world;
        reserve.transport = engine.timelinePool.Epoch();
        reserve.expectedContinuationGeneration = 1;
        reserve.sliceCount = 1;
        for (auto& decision : decisions) ASSERT_TRUE(engine.decisions.Reserve(reserve, &decision));
        request.expectedCurrentVersion = 1;
        request.rootDecision = decisions[1];
        request.expectedCurrentOccurrence = currentOccurrence;
        request.successorOccurrence = successorOccurrence;
        request.completionIsLogicalBoundary = true;
        request.next = create.initial;
        request.next.version = 2;
        request.next.cursor = 1;
        request.next.currentChildOrdinal = 1;
        request.next.selectionOrdinal = 1;
    }

    void PrepareAndClock() {
        ASSERT_TRUE(engine.PrepareOriginalQueueConditional(queue, request, &prepared));
        for (std::uint32_t index = 0; index < groups.size(); ++index) {
            harken::Plan03GroupSliceInput input{};
            input.executeFrame = 100 + index * kFrames;
            input.decision = decisions[index];
            ASSERT_TRUE(engine.ReserveOriginalQueueLaneGroup(cohort, 100, input,
                engine.timelineArms, &groups[index]));
        }
        ASSERT_TRUE(engine.PrepareOriginalQueueConditionalClock(queue,
            engine.originalQueueGroupHolders[groups[0].committedRow],
            engine.originalQueueGroupHolders[groups[1].committedRow]));
    }

    bool OwnReturned() const {
        const auto& own = engine.originalQueueConditionalHolders[queue.slot];
        return !own.baseline.Valid() && !own.clock.Valid();
    }

    void Cleanup() {
        ASSERT_TRUE(OwnReturned());
        ASSERT_TRUE(engine.CancelOriginalQueueLaneGroups(cohort, 0));
        for (const auto& group : groups)
            if (group.Exact()) EXPECT_FALSE(engine.originalQueueGroupHolders[group.committedRow].Valid());
        if (initializing) ASSERT_TRUE(engine.CancelOriginalQueueAcquire(&acquired));
        else ASSERT_TRUE(engine.ReleaseOriginalQueue(queue));
        for (const auto occurrence : {currentOccurrence, successorOccurrence}) {
            ASSERT_TRUE(engine.timelineOccurrences.Advance(occurrence, harken::OccurrenceStage::AwaitingEventFence));
            ASSERT_TRUE(engine.timelineOccurrences.ReleaseAfterFence(occurrence));
        }
        for (const auto decision : decisions) {
            ASSERT_TRUE(engine.decisions.Cancel(decision));
            ASSERT_TRUE(engine.decisions.ReclaimCancelled(decision));
        }
        ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(current, harken::PlayableLifecycle::Retired));
        ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(successor, harken::PlayableLifecycle::Retired));
        ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(root, harken::PlayableLifecycle::Retired));
        EXPECT_EQ(0u, engine.queueInstances.LiveCount());
        EXPECT_EQ(0u, engine.timelineOccurrences.OccupiedCount());
        EXPECT_EQ(0u, engine.decisions.LiveCount());
        EXPECT_EQ(0u, engine.mixer.BlocksRendered());
    }
};

TEST(OriginalSourceSubmission, EngineConditionalHolderPreparedEndReturnsOnlyOwnCapsAndForeignBlocksNativeCancel) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto& engine = *owned;
    EngineOriginalConditionalHolderHarness state(engine);
    ASSERT_NO_FATAL_FAILURE(state.Acquire(true));
    ASSERT_NO_FATAL_FAILURE(state.PrepareAndClock());
    harken::QueueInstanceTable::OriginalConditionalFutureReservation originalLoan;
    harken::OriginalQueueFutureGroupLifetimePin independentGroupLoan;
    BoundClock clockLoan;
    ASSERT_TRUE(engine.originalQueueGroupHolders[state.groups[1].committedRow].BorrowLifetime(
        &independentGroupLoan));
    ASSERT_TRUE(engine.BorrowOriginalQueueConditional(state.queue, &originalLoan));
    ASSERT_TRUE(engine.BorrowOriginalQueueConditionalClock(state.queue, &clockLoan));
    BoundClockState beforeEnd = BoundClockState::Retired;
    ASSERT_TRUE(clockLoan.CaptureState(&beforeEnd));
    ASSERT_EQ(BoundClockState::Unbound, beforeEnd);
    EXPECT_FALSE(engine.CancelOriginalQueueAcquire(&state.acquired));
    ASSERT_TRUE(state.acquired.Valid());
    ASSERT_TRUE(engine.queueInstances.CancelPreparedConditional(&state.prepared));
    EXPECT_TRUE(state.OwnReturned());
    ASSERT_TRUE(originalLoan.Valid());
    ASSERT_TRUE(clockLoan.Valid());
    BoundClockState observed = BoundClockState::Unbound;
    ASSERT_TRUE(clockLoan.CaptureState(&observed));
    EXPECT_EQ(BoundClockState::Retired, observed);
    EXPECT_FALSE(engine.CancelOriginalQueueAcquire(&state.acquired));
    EXPECT_FALSE(engine.CancelOriginalQueueLaneGroups(state.cohort, 0));
    originalLoan.Release();
    EXPECT_FALSE(engine.CancelOriginalQueueAcquire(&state.acquired));
    clockLoan.Release();
    // Native End cannot force an independently borrowed original group loan.
    EXPECT_TRUE(independentGroupLoan.Valid());
    EXPECT_FALSE(engine.CancelOriginalQueueLaneGroups(state.cohort, 0));
    independentGroupLoan.Release();
    ASSERT_NO_FATAL_FAILURE(state.Cleanup());
}

TEST(OriginalSourceSubmission, EngineConditionalHolderActualSettleEndsAssociationBeforeLastForeignClockReturn) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto& engine = *owned;
    EngineOriginalConditionalHolderHarness state(engine);
    ASSERT_NO_FATAL_FAILURE(state.Acquire());
    ASSERT_NO_FATAL_FAILURE(state.PrepareAndClock());
    BoundClock clockLoan;
    ASSERT_TRUE(engine.BorrowOriginalQueueConditionalClock(state.queue, &clockLoan));
    ASSERT_TRUE(engine.queueInstances.CommitPreparedConditional(&state.prepared, &state.version));
    harken::QueueInstanceSnapshot beforeEnd{};
    ASSERT_TRUE(engine.queueInstances.Capture(state.queue, &beforeEnd));
    ASSERT_EQ(state.version.version, beforeEnd.pendingVersion);
    ASSERT_EQ(state.create.initial.version, beforeEnd.logicMirror.version);
    ASSERT_TRUE(engine.queueInstances.ActivateConditional(state.queue, state.version,
        state.request.rootDecision, state.currentOccurrence));
    EXPECT_FALSE(state.OwnReturned());
    ASSERT_TRUE(engine.queueInstances.SettleActivated(state.queue, state.version));
    ASSERT_TRUE(state.OwnReturned());
    harken::QueueInstanceSnapshot actual{};
    ASSERT_TRUE(engine.queueInstances.Capture(state.queue, &actual));
    EXPECT_EQ(0u, actual.pendingVersion);
    EXPECT_EQ(state.version.version, actual.logicMirror.version);
    BoundClockState observed = BoundClockState::Unbound;
    ASSERT_TRUE(clockLoan.CaptureState(&observed));
    EXPECT_EQ(BoundClockState::Retired, observed);
    EXPECT_FALSE(engine.ReleaseOriginalQueue(state.queue));
    EXPECT_FALSE(engine.CancelOriginalQueueLaneGroups(state.cohort, 0));
    clockLoan.Release();
    ASSERT_NO_FATAL_FAILURE(state.Cleanup());
}

TEST(OriginalSourceSubmission, EngineConditionalHolderInitializingRollbackUsesExactOriginalFamilyAndReissues) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto& engine = *owned;
    EngineOriginalConditionalHolderHarness state(engine);
    ASSERT_NO_FATAL_FAILURE(state.Acquire(true));
    ASSERT_NO_FATAL_FAILURE(state.PrepareAndClock());
    harken::QueueInstanceTable::OriginalConditionalFutureReservation historical;
    ASSERT_TRUE(engine.BorrowOriginalQueueConditional(state.queue, &historical));
    ASSERT_TRUE(engine.queueInstances.CommitPreparedConditional(&state.prepared, &state.version));
    EXPECT_FALSE(engine.queueInstances.RollbackInitializingConditional(state.acquired, state.queue,
        {state.version.version + 1u}, state.request.rootDecision));
    EXPECT_FALSE(state.OwnReturned());
    ASSERT_TRUE(engine.queueInstances.RollbackInitializingConditional(state.acquired, state.queue,
        state.version, state.request.rootDecision));
    ASSERT_TRUE(state.OwnReturned());
    ASSERT_TRUE(historical.Valid());
    ASSERT_TRUE(engine.PrepareOriginalQueueConditional(state.queue, state.request, &state.prepared));
    const auto& fresh = engine.originalQueueConditionalHolders[state.queue.slot].baseline;
    ASSERT_TRUE(fresh.Valid());
    EXPECT_FALSE(engine.queueInstances.IsCurrentOriginalConditionalFutureReservation(historical));
    ASSERT_TRUE(engine.queueInstances.CancelPreparedConditional(&state.prepared));
    EXPECT_TRUE(state.OwnReturned());
    EXPECT_TRUE(historical.Valid());
    EXPECT_FALSE(engine.CancelOriginalQueueAcquire(&state.acquired));
    historical.Release();
    ASSERT_NO_FATAL_FAILURE(state.Cleanup());
}

TEST(OriginalSourceSubmission, EngineConditionalHolderForeignParentAndGroupReceiverRefuseWithoutClearingOwnCaps) {
    auto firstOwned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto secondOwned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto& first = *firstOwned;
    auto& second = *secondOwned;
    EngineOriginalConditionalHolderHarness a(first), b(second);
    ASSERT_NO_FATAL_FAILURE(a.Acquire());
    ASSERT_NO_FATAL_FAILURE(b.Acquire());
    harken::PreparedQueueConditionalV1 foreignPrepared;
    harken::QueueInstanceTable::OriginalConditionalFutureReservation foreignOutput;
    EXPECT_FALSE(first.queueInstances.PrepareConditional(a.queue, a.request, &foreignPrepared,
        second.originalQueueLifetimeHolders[b.queue.slot], &foreignOutput));
    EXPECT_FALSE(foreignPrepared.Valid());
    EXPECT_FALSE(foreignOutput.Valid());
    ASSERT_TRUE(first.PrepareOriginalQueueConditional(a.queue, a.request, &a.prepared));
    // Directly calling the internal callback cannot end a still-active native
    // association even with the genuine baseline's address.
    harken::Engine::EndOriginalQueueConditionalAssociation(&first, first.queueInstances,
        first.originalQueueConditionalHolders[a.queue.slot].baseline);
    EXPECT_TRUE(first.originalQueueConditionalHolders[a.queue.slot].baseline.Valid());
    harken::PreparedQueueConditionalV1 refused;
    EXPECT_FALSE(first.PrepareOriginalQueueConditional(a.queue, a.request, &refused));
    EXPECT_TRUE(a.prepared.Valid());
    EXPECT_FALSE(refused.Valid());
    ASSERT_NO_FATAL_FAILURE(b.PrepareAndClock());
    EXPECT_FALSE(first.PrepareOriginalQueueConditionalClock(a.queue,
        second.originalQueueGroupHolders[b.groups[0].committedRow],
        second.originalQueueGroupHolders[b.groups[1].committedRow]));
    EXPECT_TRUE(first.originalQueueConditionalHolders[a.queue.slot].baseline.Valid());
    EXPECT_FALSE(first.originalQueueConditionalHolders[a.queue.slot].clock.Valid());
    EXPECT_FALSE(second.queueInstances.CancelPreparedConditional(&a.prepared));
    EXPECT_TRUE(a.prepared.Valid());
    ASSERT_TRUE(first.queueInstances.CancelPreparedConditional(&a.prepared));
    EXPECT_TRUE(a.OwnReturned());
    EXPECT_FALSE(b.OwnReturned());
    ASSERT_TRUE(second.queueInstances.CancelPreparedConditional(&b.prepared));
    ASSERT_NO_FATAL_FAILURE(a.Cleanup());
    ASSERT_NO_FATAL_FAILURE(b.Cleanup());
}

TEST(OriginalSourceSubmission, EngineConditionalHolderDefaultDisabledKeepsActualLegacyPrepareAndEnd) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig());
    auto& engine = *owned;
    EngineOriginalConditionalHolderHarness state(engine);
    ASSERT_NO_FATAL_FAILURE(state.Acquire());
    ASSERT_EQ(nullptr, engine.originalQueueConditionalHolders.data());
    EXPECT_FALSE(engine.PrepareOriginalQueueConditional(state.queue, state.request, &state.prepared));
    EXPECT_FALSE(state.prepared.Valid());
    ASSERT_TRUE(engine.queueInstances.PrepareConditional(state.queue, state.request, &state.prepared));
    ASSERT_TRUE(engine.queueInstances.CancelPreparedConditional(&state.prepared));
    ASSERT_TRUE(engine.ReleaseOriginalQueue(state.queue));
    for (const auto occurrence : {state.currentOccurrence, state.successorOccurrence}) {
        ASSERT_TRUE(engine.timelineOccurrences.Advance(occurrence, harken::OccurrenceStage::AwaitingEventFence));
        ASSERT_TRUE(engine.timelineOccurrences.ReleaseAfterFence(occurrence));
    }
    for (const auto decision : state.decisions) {
        ASSERT_TRUE(engine.decisions.Cancel(decision));
        ASSERT_TRUE(engine.decisions.ReclaimCancelled(decision));
    }
    ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(state.current, harken::PlayableLifecycle::Retired));
    ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(state.successor, harken::PlayableLifecycle::Retired));
    ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(state.root, harken::PlayableLifecycle::Retired));
    EXPECT_EQ(0u, engine.queueInstances.LiveCount());
    EXPECT_EQ(0u, engine.decisions.LiveCount());
    EXPECT_EQ(0u, engine.timelineOccurrences.OccupiedCount());
}

TEST(OriginalSourceSubmission, EngineConditionalHolderJoinedTermEndsOwnClockBeforePoolAndQueueParentDestruction) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    EngineOriginalConditionalHolderHarness state(*owned);
    ASSERT_NO_FATAL_FAILURE(state.Acquire());
    ASSERT_NO_FATAL_FAILURE(state.PrepareAndClock());
    BoundClock foreign;
    ASSERT_TRUE(owned->BorrowOriginalQueueConditionalClock(state.queue, &foreign));
    ASSERT_TRUE(owned->queueInstances.CommitPreparedConditional(&state.prepared, &state.version));
    EXPECT_FALSE(owned->CancelOriginalQueueLaneGroups(state.cohort, 0));
    foreign.Release();
    EXPECT_FALSE(state.OwnReturned());
    EXPECT_FALSE(owned->CancelOriginalQueueLaneGroups(state.cohort, 0));
    EXPECT_EQ(0u, owned->mixer.BlocksRendered());
    // Engine joins first. Native End returns only its actual two capabilities;
    // the table then returns native group loans before Pool owner destruction.
    owned.reset();
}
} // namespace

TEST(OriginalSourceSubmission, EngineSingleNativeDecisionBaselineForeignBusyKeepsRealPreparedAndCommittedAliases) {
    (void)Vn_Term();
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const auto baseline = engine.decisions.LiveCount();
    harken::DecisionReserveRequest request{};
    request.world = 7;
    request.transport = 9;
    request.expectedContinuationGeneration = 11;
    request.sliceCount = 1;
    harken::PreparedDecisionReservation prepared;
    ASSERT_TRUE(engine.PrepareOriginalQueueDecisionReservation(request, &prepared));
    const auto token = prepared.Token();
    const auto* original = engine.OriginalSingleDecisionPreparation(prepared);
    ASSERT_NE(nullptr, original);
    harken::OriginalDecisionLifetimePin foreign;
    ASSERT_TRUE(original->Borrow(&foreign));
    harken::DecisionTable other(1, 1);
    EXPECT_EQ(nullptr, harken::Engine::ResolveOriginalSingleDecisionBaseline(&engine, other, token));
    EXPECT_FALSE(engine.decisions.CancelPreparedReservation(&prepared));
    EXPECT_TRUE(prepared.Valid());
    EXPECT_EQ(token.slot, prepared.Token().slot);
    EXPECT_EQ(token.generation, prepared.Token().generation);
    EXPECT_TRUE(engine.decisions.IsCancelled(token));
    EXPECT_FALSE(engine.decisions.MatchesOriginalPreparedDecision(*original, prepared));
    EXPECT_TRUE(engine.decisions.IsOriginalDecisionLifetimePinFor(*original, token));
    EXPECT_TRUE(engine.decisions.MatchesOriginalDecisionFactory(*original, request));
    EXPECT_EQ(baseline + 1u, engine.decisions.LiveCount());
    foreign.Release();
    ASSERT_TRUE(engine.decisions.CancelPreparedReservation(&prepared));
    EXPECT_FALSE(prepared.Valid());
    EXPECT_FALSE(original->Valid());
    EXPECT_EQ(baseline, engine.decisions.LiveCount());

    ASSERT_TRUE(engine.PrepareOriginalQueueDecisionReservation(request, &prepared));
    original = engine.OriginalSingleDecisionPreparation(prepared);
    ASSERT_NE(nullptr, original);
    ASSERT_TRUE(original->Borrow(&foreign));
    harken::DecisionToken committed{};
    ASSERT_TRUE(engine.decisions.CommitPreparedReservation(&prepared, engine.logicCommits.Issue(), &committed));
    ASSERT_TRUE(committed.Valid());
    EXPECT_FALSE(prepared.Valid());
    ASSERT_TRUE(engine.decisions.Cancel(committed));
    EXPECT_FALSE(engine.decisions.ReclaimCancelled(committed));
    EXPECT_TRUE(engine.decisions.Contains(committed));
    EXPECT_TRUE(original->Valid());
    foreign.Release();
    ASSERT_TRUE(engine.decisions.ReclaimCancelled(committed));
    EXPECT_FALSE(original->Valid());
    EXPECT_FALSE(engine.decisions.Contains(committed));
    EXPECT_EQ(baseline, engine.decisions.LiveCount());
}

TEST(OriginalSourceSubmission, EngineSingleNativeOccurrenceResolverConsumesOnlyTrueHeldProofAndForeignKeepsStorage) {
    (void)Vn_Term();
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const auto baseline = engine.timelineOccurrences.OccupiedCount();
    harken::PreparedPlayableInstance arranging;
    ASSERT_TRUE(engine.playableInstances.PrepareAcquire(harken::PlayableKind::Sound, 0, &arranging));
    harken::DecisionReserveRequest request{};
    request.world = 7;
    request.transport = 9;
    request.expectedContinuationGeneration = 11;
    request.sliceCount = 1;
    harken::PreparedDecisionReservation prepared;
    ASSERT_TRUE(engine.PrepareOriginalQueueDecisionReservation(request, &prepared));
    harken::OccurrenceToken occurrence{};
    ASSERT_TRUE(engine.timelineOccurrences.PrepareAcquire({arranging.Handle(), kSound, 0, 0}, &occurrence));
    const auto epoch = engine.timelineOccurrences.ScheduleEpochOf(occurrence);
    const auto owner = harken::CompletionOwnerRef::Single(prepared.Token(), epoch);
    ASSERT_TRUE(engine.timelineOccurrences.BindCompletionProgress(occurrence, 17, owner.factsRevision));
    ASSERT_TRUE(engine.PinOriginalSingleCompletionOwner(occurrence, owner));
    auto& original = engine.originalSingleOccurrenceBaselines[occurrence.slot];
    ASSERT_TRUE(original.Valid());
    harken::OccurrenceStatePool::OriginalSingleCompletionOwnerPin foreign;
    ASSERT_TRUE(original.Borrow(&foreign));
    const auto* progress = engine.timelineOccurrences.CompletionProgressOf(occurrence);
    ASSERT_NE(nullptr, progress);
    harken::OccurrenceStatePool::PreparedSingleCompletionCommitProof held;
    ASSERT_TRUE(engine.timelineOccurrences.ProvePreparedSingleCompletionCommit(original,
        owner, progress, 17, epoch, &held));
    EXPECT_FALSE(engine.timelineOccurrences.CommitPreparedAcquire(occurrence));
    EXPECT_TRUE(held.Valid());
    EXPECT_FALSE(engine.timelineOccurrences.UnbindCompletionProgress(occurrence, 17));
    EXPECT_FALSE(engine.timelineOccurrences.CancelPreparedAcquire(occurrence));
    harken::OccurrenceStatePool other(1);
    EXPECT_EQ(nullptr, harken::Engine::ResolveOriginalSingleOccurrenceBaseline(&engine, other, occurrence));
    EXPECT_EQ(nullptr, harken::Engine::ResolveOriginalSingleOccurrenceCommit(&engine, other, occurrence));
    auto& actualOutput = engine.originalSingleOccurrenceCommitProofs[occurrence.slot];
    actualOutput = std::move(held);
    ASSERT_TRUE(engine.timelineOccurrences.CommitPreparedAcquire(occurrence));
    EXPECT_FALSE(held.Valid());
    EXPECT_FALSE(actualOutput.Valid());
    EXPECT_TRUE(original.Valid());
    EXPECT_TRUE(foreign.Valid());
    ASSERT_TRUE(engine.timelineOccurrences.RollbackCompletionOwnerPinExact(occurrence, owner));
    ASSERT_TRUE(engine.timelineOccurrences.UnbindCompletionProgress(occurrence, 17));
    ASSERT_TRUE(engine.timelineOccurrences.Advance(occurrence, harken::OccurrenceStage::AwaitingEventFence));
    EXPECT_FALSE(engine.timelineOccurrences.ReleaseAfterFence(occurrence, 0));
    EXPECT_EQ(baseline + 1u, engine.timelineOccurrences.OccupiedCount());
    foreign.Release();
    // This unpublished issuer-only row has no audio reader fence to fabricate.
    ASSERT_TRUE(engine.timelineOccurrences.ReleaseAfterFence(occurrence, 0));
    EXPECT_FALSE(original.Valid());
    EXPECT_EQ(baseline, engine.timelineOccurrences.OccupiedCount());
    EXPECT_EQ(nullptr, engine.timelineOccurrences.KeyOf(occurrence));
    ASSERT_TRUE(engine.decisions.CancelPreparedReservation(&prepared));
    ASSERT_TRUE(engine.playableInstances.CancelPrepared(&arranging));
}

#include "harken/core/CpuClock.h"
#include <chrono>

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
namespace {

// Program storage outlives EngineSession. All move-only factory outputs below
// are instead declared after EngineSession and returned before its teardown.
struct RegisteredVariableWorkerProgram {
    harken::TimelineEventPayload start{};
    harken::TimelineProgramView program{};
};

struct RegisteredVariableWorkerEpisode {
    SourceBody body;
    Registry::OriginalSourceLifetimePin source;
    BodyPin originalBody;
    harken::PlayableInstanceHandle leaf = 0;
    harken::VariableControlInstanceToken variable{};
    harken::BankVersionToken bank{};
    harken::DecisionToken decision{};
    harken::ArmGroupToken armGroup{};
    harken::Plan03PageGroupView group{};
    std::uint64_t startFrame = 0;
    std::uint64_t startBlock = 0;
};

VnConfig RegisteredVariableWorkerConfig() {
    auto config = SourceBodyConfig();
    config.renderWorkerCount = 1;
    return config;
}

void PublishRegisteredVariableWorkerSource(harken::Engine& engine,
    RegisteredVariableWorkerProgram& storage,
    RegisteredVariableWorkerEpisode& episode,
    const std::vector<float>& samples) {
    ASSERT_EQ(1u, engine.voices.RenderWorkerCount());
    engine.voices.DisableRenderWorkers();
    ASSERT_TRUE(engine.voices.EnsureRenderWorkersReady());
    ASSERT_TRUE(engine.voices.EnableOfflineRenderWorkers());
    const auto pcm = harkentest::MonoDesc(samples, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSound, &pcm));

    const auto axis = harkentest::ObjectId(0x781);
    const harken::VariableDefinitionV1 definition{
        axis, harken::VariableKindV1::Step, 0.0f, 1.0f, 0.0f};
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallDefinitionsWorld(&engine, &definition, 1,
            UINT64_C(0x5238574F524B4552)).status);
    float value = -1.0f;
    ASSERT_TRUE(engine.variables.CaptureCurrentBank(0, &axis, 1,
        &value, &episode.bank));
    ASSERT_TRUE(episode.bank.Valid());
    ASSERT_FLOAT_EQ(0.0f, value);

    episode.leaf = engine.playableInstances.Acquire(harken::PlayableKind::Sound, 0);
    ASSERT_NE(0u, episode.leaf);
    const auto leaf = engine.playableInstances.Classify(episode.leaf);
    ASSERT_TRUE(leaf.live);
    const auto world = engine.variables.CurrentWorldGeneration();
    ASSERT_NE(0u, world);
    const auto sequence = engine.logicCommits.Issue();
    ASSERT_NE(0u, sequence);
    harken::DecisionReserveRequest decision{};
    decision.ticket.sequence = sequence;
    decision.world = world;
    decision.transport = engine.timelinePool.Epoch();
    decision.expectedContinuationGeneration = leaf.scheduleEpoch;
    decision.sliceCount = 1;
    ASSERT_TRUE(engine.decisions.Reserve(decision, &episode.decision));
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
        engine.variableControlInstances.Acquire({world, episode.leaf,
            harkentest::ObjectId(0x782), UINT64_C(0x52385643534F5552)},
            &episode.variable));
    ASSERT_EQ(harken::VariableControlInstanceStatus::Ok,
        engine.variableControlInstances.PrepareSide(episode.variable,
            harken::VariableControlSide::Discrete, episode.decision, episode.bank));

    const auto resident = engine.registry.Lookup(kSound);
    ASSERT_TRUE(resident.Valid());
    ASSERT_EQ(VN_OK, engine.voices.Play(resident, 0.0f, 1.0, 0,
        false, 0, 0, 0,
        harken::ResolvePanGains(engine.config.mixChannelLayout, 0.0f),
        0, resident.frames, 1, harken::ResampleProfile::Professional44148,
        &episode.body.source, 1, UINT32_MAX, 0, true));
    ASSERT_TRUE(engine.voices.CaptureReleaseObserver(episode.body.source,
        &episode.body.release));
    harken::VoicePool::PreparedCommitInput physical{};
    physical.renderHandle = episode.body.source;
    physical.originalRelease = episode.body.release;
    ASSERT_TRUE(engine.voices.CapturePreparedRoutingV1(episode.body.source,
        &physical.routing));
    // The actual native Variable route remains canonical-unbound. It does not
    // attach a serial Single CompletionProgress to a parallel render voice.
    ASSERT_EQ(nullptr, physical.completionProgress);
    harken::VoicePool::PreparedCommitProof voiceProof;
    ASSERT_TRUE(engine.voices.ProvePreparedCommit(physical, &voiceProof));

    episode.startFrame = (engine.mixer.BlocksRendered() + 2u) * kFrames;
    harken::TimelineArmTable::PreparedCommitInput logical{};
    logical.startFrame = episode.startFrame;
    logical.armDeadline = episode.startFrame;
    logical.programGeneration = episode.leaf;
    logical.target.subjectId = kSound;
    logical.parentGroup = {episode.decision};
    logical.memberIndex = 0;
    episode.body.armFreeBefore = engine.timelineArms.FreeCount();
    ASSERT_TRUE(engine.timelineArms.PrepareGrouped(
        static_cast<std::uint32_t>(episode.body.source),
        static_cast<std::uint32_t>(episode.body.source >> 32),
        logical.startFrame, logical.armDeadline, logical.programGeneration,
        logical.target, logical.parentGroup, logical.memberIndex,
        &episode.body.arm));
    logical.arm = episode.body.arm;
    harken::TimelineArmTable::PreparedCommitProof armProof;
    ASSERT_TRUE(engine.timelineArms.ProvePreparedCommit(logical, &armProof));
    ASSERT_TRUE(engine.armGroups.Reserve(episode.decision, &episode.body.arm,
        1, &episode.armGroup));
    harken::Plan03GroupSliceInput slice{};
    slice.executeFrame = episode.startFrame;
    slice.decision = episode.decision;
    slice.members = &episode.body.arm;
    slice.memberCount = 1;
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
        engine.timelinePool.ReservePlan03GroupSlice(slice, engine.timelineArms));
    ASSERT_EQ(1u, engine.timelinePool.CaptureLivePlan03Groups(&episode.group, 1));
    ASSERT_TRUE(episode.group.identity.Exact());
    ASSERT_EQ(episode.decision.slot, episode.group.identity.decision.slot);
    ASSERT_EQ(episode.decision.generation, episode.group.identity.decision.generation);
    ASSERT_EQ(1u, episode.group.memberCount);
    ASSERT_TRUE(Registry::SameToken(episode.body.arm, episode.group.members[0]));
    ASSERT_NE(nullptr, episode.group.lease);
    ASSERT_EQ(harken::AudioSliceState::Ready, episode.group.lease->Capture().state);

    storage.start.kind = harken::TimelineEventKind::Start;
    storage.start.target = logical.target;
    storage.program = {episode.leaf, &storage.start, 1};
    harken::ScheduledEventInput start{};
    start.executeFrame = episode.startFrame;
    start.logicCommitSequence = sequence;
    start.subeventPhase = 20;
    start.armToken = episode.body.arm;
    harken::TransactionRequest request{};
    request.transactionId = sequence;
    request.transportEpoch = engine.timelinePool.Epoch();
    request.ownerInstanceHandle = episode.leaf;
    request.ownerInstanceScheduleEpoch = leaf.scheduleEpoch;
    request.program = {episode.leaf, &storage.program};
    request.armToken = episode.body.arm;
    request.events = &start;
    request.eventCount = 1;
    harken::PreparedTransactionBatch transaction{};
    ASSERT_EQ(harken::ReserveRefusal::None,
        engine.timelinePool.PrepareTransactionBatch(&request, 1,
            engine.mixer.BlocksRendered() * kFrames, &transaction));
    ASSERT_TRUE(engine.timelinePool.FreezePreparedTransactionBatch(transaction, sequence));
    ASSERT_TRUE(engine.timelinePool.CommitPreparedTransactionBatch(&transaction, sequence));

    const harken::DecisionParticipantRef participant{
        harken::DecisionParticipantKind::Variable, episode.variable.slot,
        episode.variable.generation};
    harken::DecisionCommitPayload commit{};
    // This is the existing legacy Plan03 carrier marker, not a new capability.
    // The genuine group, members and lease above come from the pool factory.
    commit.pages = {0, 1};
    commit.armGroup = episode.armGroup;
    commit.participants = &participant;
    commit.participantCount = 1;
    ASSERT_TRUE(engine.decisions.Commit(episode.decision, commit));

    harken::CommandRecord header{};
    header.opcode = harken::kOpcodePlay;
    header.payload.play.handle = episode.body.source;
    header.payload.play.sourceIdentity = episode.body.source;
    header.payload.play.batch.batchVersion = harken::kPlayLedBatchVersion;
    header.payload.play.batch.batchKind = harken::PlayLedBatchKind::TimelineArm;
    header.payload.play.batch.batchCommandCount = 1;
    header.payload.play.batch.armToken =
        (static_cast<std::uint64_t>(episode.body.arm.generation) << 32) |
        episode.body.arm.slot;
    // Both authentic outputs are requested in this one original factory call,
    // while the matching actual Arm and physical Voice proofs are still Held.
    ASSERT_TRUE(engine.originalSourceSubmissions.CapturePreparedSource(header,
        armProof, voiceProof, {}, &episode.source, &episode.originalBody));
    ASSERT_TRUE(episode.source.Valid());
    ASSERT_TRUE(episode.originalBody.Valid());
    ASSERT_TRUE(engine.voices.IsOriginalSourceBodyLifetimePinFor(
        episode.originalBody, episode.body.arm, episode.body.source));
    harken::CommandQueue::BatchReservation reservation{};
    ASSERT_TRUE(engine.commands.ReserveBatch(1, &reservation));
    harken::CommandQueue::PreparedBatchProof batch;
    ASSERT_TRUE(engine.commands.ProvePreparedBatch(&reservation, &header, 1, &batch));
    ASSERT_TRUE(engine.originalSourceSubmissions.BindPreparedChild(batch, 0, 1));
    harken::VoicePool::ConsumePreparedCommit(std::move(voiceProof));
    harken::TimelineArmTable::ConsumePreparedCommit(std::move(armProof));
    engine.originalSourceSubmissions.PublishPreparedChild(header);
    (void)engine.commands.ConsumePreparedBatch(std::move(batch));
    episode.body.acceptedHead = engine.commands.PublishedHead();
    engine.AcceptSourceCommandPublications();
    ASSERT_EQ(Registry::Phase::Published,
        engine.originalSourceSubmissions.Find(episode.body.arm)->State());
}

void StartRegisteredVariableWorkerSource(harken::Engine& engine,
    RegisteredVariableWorkerEpisode& episode) {
    auto before = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    ASSERT_EQ(before + 1u, engine.timelineArms.RowAt(
        episode.body.arm.slot)->armedCompletionSequence.load());
    ASSERT_EQ(0u, engine.timelineArms.RowAt(
        episode.body.arm.slot)->startedBlockSequence.load());
    ASSERT_TRUE(engine.voices.IsOriginalSourceBodyCurrent(episode.originalBody));
    EXPECT_EQ(0.0f, CurrentPeak(engine));
    ASSERT_EQ(kSourceOriginNormal, SourceOriginAccess::Observe(
        engine.originalSourceSubmissions, episode.body.arm).mode);
    ASSERT_TRUE(SourceOriginAccess::Observe(engine.originalSourceSubmissions,
        episode.body.arm).installationStanding);
    ASSERT_NO_FATAL_FAILURE(CheckActualNativeCallbackReturned(engine, episode.body));

    // Producer group ACK derives only from this original Source's completed
    // native installation; no prefilled receipt or synthetic sequence is used.
    const auto ack = engine.timelineArms.RowAt(
        episode.body.arm.slot)->armedCompletionSequence.load();
    ASSERT_TRUE(engine.armGroups.Acknowledge(episode.armGroup, episode.body.arm, ack));
    ASSERT_TRUE(engine.timelineArms.Publish(episode.body.arm));
    engine.timelinePool.Publish(episode.startFrame + kFrames, nullptr, nullptr,
        &engine.timelineArms);
    harken::ArmGroupSnapshot arms{};
    ASSERT_TRUE(engine.armGroups.Capture(episode.armGroup, &arms));
    ASSERT_EQ(harken::ArmGroupState::Ready, arms.state);
    ASSERT_EQ(1u, arms.ackedCount);
    while (engine.mixer.BlocksRendered() <= episode.startFrame / kFrames) {
        before = engine.mixer.BlocksRendered();
        engine.nullBackend.Pump(1);
        ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    }
    episode.startBlock = episode.startFrame / kFrames + 1u;
    ASSERT_EQ(episode.startBlock, engine.timelineArms.RowAt(
        episode.body.arm.slot)->startedBlockSequence.load());
    ASSERT_EQ(harken::AudioSliceState::SliceApplied,
        episode.group.lease->Capture().state);
    harken::DecisionCommittedSnapshot decision{};
    ASSERT_TRUE(engine.decisions.CaptureCommitted(episode.decision, &decision));
    ASSERT_EQ(harken::AudioGroupState::Admitted, decision.audioState);
    ASSERT_EQ(1u, decision.consumedSlices);
    ASSERT_EQ(episode.startBlock, decision.lastAudioSequence);
    harken::VariableControlInstanceSnapshot variable{};
    ASSERT_TRUE(engine.variableControlInstances.Capture(episode.variable, &variable));
    ASSERT_EQ(harken::VariableControlSideState::Committed, variable.discrete.state);
    ASSERT_EQ(episode.decision.slot, variable.discrete.decision.slot);
    ASSERT_EQ(episode.decision.generation, variable.discrete.decision.generation);
    ASSERT_EQ(episode.bank.slot, variable.discrete.lastApplied.slot);
    ASSERT_EQ(episode.bank.generation, variable.discrete.lastApplied.generation);
    ASSERT_GT(CurrentPeak(engine), 0.1f);
    ASSERT_NO_FATAL_FAILURE(CheckActualNativeCallbackReturned(engine, episode.body));
}

struct RegisteredSourceClaimStall {
    std::atomic<std::uint64_t> generation{0};
    std::atomic<std::uint32_t> job{UINT32_MAX};
    std::atomic<bool> release{false};
    std::atomic<bool> leftClaimGate{false};
    ~RegisteredSourceClaimStall() { release.store(true, std::memory_order_release); }
    static void Coordinator(void* opaque, std::uint64_t generation) noexcept {
        auto& gate = *static_cast<RegisteredSourceClaimStall*>(opaque);
        const auto until = harken::CpuClockTicks() + 2u * harken::CpuClockTicksPerSecond();
        while (gate.generation.load(std::memory_order_acquire) != generation &&
            harken::CpuClockTicks() < until) std::this_thread::yield();
    }
    static void Claimed(void* opaque, std::uint64_t generation,
        std::uint32_t job) noexcept {
        auto& gate = *static_cast<RegisteredSourceClaimStall*>(opaque);
        gate.job.store(job, std::memory_order_relaxed);
        gate.generation.store(generation, std::memory_order_release);
        while (!gate.release.load(std::memory_order_acquire)) std::this_thread::yield();
        gate.leftClaimGate.store(true, std::memory_order_release);
    }
};

// A fatal assertion must not leave the worker waiting while EngineSession joins.
// This guard releases only the workload gate; actual lifecycle still joins it.
struct ReleaseRegisteredSourceClaimBeforeTerm {
    RegisteredSourceClaimStall& gate;
    ~ReleaseRegisteredSourceClaimBeforeTerm() {
        gate.release.store(true, std::memory_order_release);
    }
};

void AbandonRegisteredVariableWorkerSource(harken::Engine& engine,
    RegisteredVariableWorkerEpisode& episode, RegisteredSourceClaimStall& gate) {
    NativeCallbackAccess::SetOriginalSourceWorkerGates(engine.voices,
        &RegisteredSourceClaimStall::Coordinator,
        &RegisteredSourceClaimStall::Claimed, &gate);
    NativeCallbackAccess::SetWorkerDeadline(engine.voices,
        std::max(UINT64_C(1), harken::CpuClockTicksPerSecond() / 20u));
    const auto originBefore = SourceOriginAccess::Observe(
        engine.originalSourceSubmissions, episode.body.arm);
    const auto completed = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(completed + 1u, engine.mixer.BlocksRendered());
    ASSERT_NE(0u, gate.generation.load(std::memory_order_acquire));
    ASSERT_EQ(0u, gate.job.load(std::memory_order_acquire));
    const auto actual = NativeCallbackAccess::ObserveOriginalWorkerJob(engine.voices, 0);
    ASSERT_EQ(gate.generation.load(std::memory_order_acquire), actual.abandonedGeneration);
    ASSERT_EQ(1u, actual.abandonedJobCount);
    ASSERT_EQ(static_cast<std::uint32_t>(episode.body.source), actual.renderSlot);
    ASSERT_EQ(static_cast<std::uint32_t>(episode.body.source >> 32), actual.renderGeneration);
    ASSERT_FALSE(actual.finishedAtAbandonment);
    ASSERT_TRUE(NativeCallbackAccess::IsActuallyQuarantined(engine.voices, actual.renderSlot));
    ASSERT_FALSE(gate.leftClaimGate.load(std::memory_order_acquire));
    EXPECT_NE(static_cast<std::uint32_t>(episode.body.source >> 32),
        NativeCallbackAccess::OriginalSlotGeneration(engine.voices, actual.renderSlot));
    EXPECT_EQ(nullptr, engine.voices.Resolve(episode.body.source));
    EXPECT_FALSE(engine.voices.ReleaseObserved(episode.body.release));
    const auto retained = NativeCallbackAccess::ObserveOriginalCallback(engine.voices,
        actual.renderSlot);
    EXPECT_TRUE(retained.reader);
    EXPECT_TRUE(retained.storageHeld);
    EXPECT_TRUE(retained.actor);
    EXPECT_TRUE(retained.installedInstallation);
    EXPECT_FALSE(retained.loanReturned);
    const auto origin = SourceOriginAccess::Observe(
        engine.originalSourceSubmissions, episode.body.arm);
    EXPECT_EQ(kSourceOriginNormal, origin.mode);
    EXPECT_EQ(1u, origin.count);
    EXPECT_GT(origin.epoch, originBefore.epoch);
    EXPECT_TRUE(origin.installationStanding);
    EXPECT_FALSE(engine.voices.IsOriginalSourceBodyCurrent(episode.originalBody));
    // No PCM, cascade payload, timeline span or pending/installed loan member
    // is read while the claimed worker still owns its native job.
}

void CheckRegisteredQuarantineUntilJoin(bool keepForeignBody) {
    RegisteredVariableWorkerProgram storage;
    RegisteredSourceClaimStall gate;
    harkentest::EngineSession session(RegisteredVariableWorkerConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ReleaseRegisteredSourceClaimBeforeTerm release{gate};
    RegisteredVariableWorkerEpisode episode;
    const std::vector<float> samples(kRate, 0.25f);
    ASSERT_NO_FATAL_FAILURE(PublishRegisteredVariableWorkerSource(engine, storage,
        episode, samples));
    ASSERT_NO_FATAL_FAILURE(StartRegisteredVariableWorkerSource(engine, episode));
    BodyPin foreign;
    if (keepForeignBody) ASSERT_TRUE(episode.originalBody.Borrow(&foreign));
    ASSERT_NO_FATAL_FAILURE(AbandonRegisteredVariableWorkerSource(engine, episode, gate));
    const auto slot = static_cast<std::uint32_t>(episode.body.source);
    episode.originalBody.Release();
    episode.source.Release();
    auto held = NativeCallbackAccess::ObserveOriginalCallback(engine.voices, slot);
    EXPECT_EQ(keepForeignBody ? 1u : 0u, held.bodyCount);
    EXPECT_TRUE(held.reader);
    EXPECT_TRUE(held.installedInstallation);
    EXPECT_FALSE(held.disposed);
    EXPECT_FALSE(held.loanReturned);
    VnVoiceHandle refused = UINT64_C(0x11223344);
    EXPECT_EQ(VN_NO_VOICE, PrepareAtOriginalBodySlot(engine, episode.body, &refused));
    EXPECT_EQ(UINT64_C(0x11223344), refused);

    // Finishing this already-claimed job is not the actual lifecycle join.
    gate.release.store(true, std::memory_order_release);
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!gate.leftClaimGate.load(std::memory_order_acquire) &&
        std::chrono::steady_clock::now() < until) std::this_thread::yield();
    ASSERT_TRUE(gate.leftClaimGate.load(std::memory_order_acquire));
    const auto completed = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(completed + 1u, engine.mixer.BlocksRendered());
    const auto frozen = NativeCallbackAccess::ObserveOriginalWorkerJob(engine.voices, 0);
    EXPECT_FALSE(frozen.finishedAtAbandonment);
    held = NativeCallbackAccess::ObserveOriginalCallback(engine.voices, slot);
    EXPECT_TRUE(held.reader);
    EXPECT_TRUE(held.storageHeld);
    EXPECT_TRUE(held.actor);
    EXPECT_TRUE(held.installedInstallation);
    EXPECT_FALSE(held.loanReturned);
    EXPECT_EQ(1u, SourceOriginAccess::Observe(
        engine.originalSourceSubmissions, episode.body.arm).count);

    // This genuine native entry first calls workers.Stop (join), then marks
    // Body disposition and returns callback last-use storage/actor.
    engine.voices.ShutdownOriginalSourceBodiesAfterQuiescence();
    NativeCallbackAccess::SetOriginalSourceWorkerGates(engine.voices, nullptr, nullptr, nullptr);
    held = NativeCallbackAccess::ObserveOriginalCallback(engine.voices, slot);
    EXPECT_FALSE(held.reader);
    EXPECT_FALSE(held.storageHeld);
    EXPECT_FALSE(held.actor);
    EXPECT_TRUE(held.disposed);
    EXPECT_EQ(0u, SourceOriginAccess::Observe(
        engine.originalSourceSubmissions, episode.body.arm).count);
    if (keepForeignBody) {
        EXPECT_TRUE(foreign.Valid());
        EXPECT_TRUE(held.installedInstallation);
        EXPECT_FALSE(held.loanReturned);
        EXPECT_TRUE(SourceOriginAccess::Observe(
            engine.originalSourceSubmissions, episode.body.arm).installationStanding);
        foreign.Release();
    }
    held = NativeCallbackAccess::ObserveOriginalCallback(engine.voices, slot);
    EXPECT_TRUE(held.loanReturned);
    EXPECT_FALSE(held.pendingInstallation);
    EXPECT_FALSE(held.installedInstallation);
    const auto closed = SourceOriginAccess::Observe(
        engine.originalSourceSubmissions, episode.body.arm);
    EXPECT_EQ(kSourceOriginClosed, closed.mode);
    EXPECT_EQ(0u, closed.count);
    EXPECT_EQ(0u, closed.lifetime);
    EXPECT_FALSE(closed.installationStanding);
    EXPECT_FALSE(engine.timelineArms.RowAt(episode.body.arm.slot)->originalLifetimePinned.load());
    EXPECT_EQ(0u, NativeCallbackAccess::OriginalOwnerCount(engine.voices));
    // Quarantine intentionally does not ReleaseSlot. Thread join and Source
    // loan return do not manufacture a physical release receipt or slot reuse.
    EXPECT_FALSE(engine.voices.ReleaseObserved(episode.body.release));
    EXPECT_TRUE(NativeCallbackAccess::IsActuallyQuarantined(engine.voices, slot));
}

struct JoinRegisteredSourceTermination {
    RegisteredSourceClaimStall& gate;
    std::thread& termination;
    ~JoinRegisteredSourceTermination() {
        gate.release.store(true, std::memory_order_release);
        if (termination.joinable()) termination.join();
    }
};

void CheckRegisteredSourceTermJoinsActualClaim() {
    RegisteredVariableWorkerProgram storage;
    RegisteredSourceClaimStall gate;
    harkentest::EngineSession session(RegisteredVariableWorkerConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ReleaseRegisteredSourceClaimBeforeTerm release{gate};
    RegisteredVariableWorkerEpisode episode;
    const std::vector<float> samples(kRate, 0.25f);
    ASSERT_NO_FATAL_FAILURE(PublishRegisteredVariableWorkerSource(engine, storage,
        episode, samples));
    ASSERT_NO_FATAL_FAILURE(StartRegisteredVariableWorkerSource(engine, episode));
    ASSERT_NO_FATAL_FAILURE(AbandonRegisteredVariableWorkerSource(engine, episode, gate));
    const auto slot = static_cast<std::uint32_t>(episode.body.source);
    episode.originalBody.Release();
    episode.source.Release();
    ASSERT_TRUE(NativeCallbackAccess::OriginalCallbackStorageReader(engine.voices, slot));
    ASSERT_FALSE(gate.release.load(std::memory_order_acquire));
    ASSERT_FALSE(gate.leftClaimGate.load(std::memory_order_acquire));

    // No callback remains in flight. Only the worker that truly claimed this
    // original job is still held, and every external lifetime cap is returned.
    std::atomic<VnResult> result{VN_NOT_INITIALIZED};
    std::atomic<bool> returned{false};
    std::thread termination([&] {
        result.store(Vn_Term(), std::memory_order_relaxed);
        returned.store(true, std::memory_order_release);
    });
    JoinRegisteredSourceTermination joined{gate, termination};
    session.Release();
    const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(2);
    while (!NativeCallbackAccess::OriginalWorkerStopRequested(engine.voices) &&
        std::chrono::steady_clock::now() < until) std::this_thread::yield();
    // The original Engine storage is still live: native Stop must join the
    // actual worker, which cannot leave its claimed job until this gate opens.
    // During Term only atomic state is inspected, never callback loan members.
    ASSERT_TRUE(NativeCallbackAccess::OriginalWorkerStopRequested(engine.voices));
    EXPECT_FALSE(returned.load(std::memory_order_acquire));
    EXPECT_TRUE(NativeCallbackAccess::OriginalCallbackStorageReader(engine.voices, slot));
    EXPECT_FALSE(gate.leftClaimGate.load(std::memory_order_acquire));
    gate.release.store(true, std::memory_order_release);
    termination.join();
    EXPECT_TRUE(gate.leftClaimGate.load(std::memory_order_acquire));
    EXPECT_TRUE(returned.load(std::memory_order_acquire));
    EXPECT_EQ(VN_OK, result.load(std::memory_order_relaxed));
    // Engine destruction has completed. No old pool, record, Arm or Slot is
    // inspected and no physical-release receipt is manufactured after Term.
    EXPECT_EQ(nullptr, harken::detail::ActiveEngine());
}
} // namespace

TEST(OriginalSourceSubmission, RegisteredVariableSourceActualWorkerDeadlineRetainsCallbackUntilJoinedStop) {
    CheckRegisteredQuarantineUntilJoin(false);
}

TEST(OriginalSourceSubmission, RegisteredVariableSourceActualWorkerDeadlineForeignBodyReturnsOnlyAfterJoinedStop) {
    CheckRegisteredQuarantineUntilJoin(true);
}

TEST(OriginalSourceSubmission, RegisteredVariableSourceActualWorkerDeadlineTermWaitsForRealClaimAndReturnsNativeLoans) {
    CheckRegisteredSourceTermJoinsActualClaim();
}
#endif


namespace {
struct InitialConditionalFactoryOutputHarness : EngineOriginalConditionalHolderHarness {
    using EngineOriginalConditionalHolderHarness::EngineOriginalConditionalHolderHarness;
    harken::QueueInstanceTable::OriginalConditionalFutureReservation* issued = nullptr;
    harken::ConditionalQueueVersionToken issuedVersion{};
    std::array<harken::OriginalQueueFutureGroupRegistration*, 2> issuedGroups{};
    void PrepareOriginal() {
        ASSERT_TRUE(engine.PrepareOriginalQueueConditional(queue, request, &prepared, &issued));
        issuedVersion = prepared.Version();
        ASSERT_NE(nullptr, issued);
        EXPECT_EQ(queue.slot, issued->Token().slot);
        EXPECT_EQ(queue.generation, issued->Token().generation);
        EXPECT_EQ(0u, engine.mixer.BlocksRendered());
    }
    void ReserveMembers() {
        for (std::uint32_t index = 0; index < groups.size(); ++index) {
            harken::Plan03GroupSliceInput input{};
            input.executeFrame = 100 + index * kFrames;
            input.decision = decisions[index];
            ASSERT_TRUE(engine.ReserveOriginalQueueLaneGroup(cohort, 100, input,
                engine.timelineArms, &groups[index], nullptr, &issuedGroups[index]));
            ASSERT_NE(nullptr, issuedGroups[index]);
            EXPECT_TRUE(engine.timelinePool.IsOriginalQueueFutureGroupRegistrationFor(
                *issuedGroups[index], groups[index]));
        }
    }
    void Clock() {
        ASSERT_NE(nullptr, issued);
        ASSERT_TRUE(engine.PrepareIssuedOriginalQueueConditionalClock(*issued,
            *issuedGroups[0], *issuedGroups[1]));
    }
    bool Abort() {
        return engine.AbortIssuedOriginalQueueConditional(prepared,
            initializing ? &acquired : nullptr, issued, queue, issuedVersion,
            request.rootDecision, currentOccurrence);
    }
};
}

TEST(OriginalSourceSubmission, InitialConditionalSameCallOriginalOutputsBeforeAckReturnOwnClockBeforeGroupCancel) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto& engine = *owned;
    InitialConditionalFactoryOutputHarness state(engine);
    ASSERT_NO_FATAL_FAILURE(state.Acquire(true));
    ASSERT_NO_FATAL_FAILURE(state.PrepareOriginal());
    ASSERT_NO_FATAL_FAILURE(state.ReserveMembers());
    ASSERT_NO_FATAL_FAILURE(state.Clock());
    BoundClock clock;
    harken::OriginalQueueFutureGroupLifetimePin independent;
    ASSERT_TRUE(state.issuedGroups[1]->BorrowLifetime(&independent));
    ASSERT_TRUE(engine.BorrowOriginalQueueConditionalClock(state.queue, &clock));
    EXPECT_FALSE(engine.CancelOriginalQueueLaneGroups(state.cohort, 0));
    ASSERT_TRUE(state.Abort());
    EXPECT_EQ(nullptr, state.issued);
    EXPECT_TRUE(state.OwnReturned());
    ASSERT_TRUE(clock.Valid());
    EXPECT_FALSE(engine.CancelOriginalQueueLaneGroups(state.cohort, 0));
    clock.Release();
    EXPECT_FALSE(engine.CancelOriginalQueueLaneGroups(state.cohort, 0));
    EXPECT_TRUE(independent.Valid());
    EXPECT_TRUE(state.acquired.Valid());
    independent.Release();
    ASSERT_TRUE(state.Abort());
    ASSERT_NO_FATAL_FAILURE(state.Cleanup());
}

TEST(OriginalSourceSubmission, InitialConditionalWrongIssuedVersionAbortPreservesActualPreparedPrefix) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto& engine = *owned;
    InitialConditionalFactoryOutputHarness state(engine);
    ASSERT_NO_FATAL_FAILURE(state.Acquire(true));
    ASSERT_NO_FATAL_FAILURE(state.PrepareOriginal());
    ASSERT_NO_FATAL_FAILURE(state.ReserveMembers());
    ASSERT_NO_FATAL_FAILURE(state.Clock());
    EXPECT_FALSE(engine.AbortIssuedOriginalQueueConditional(state.prepared, &state.acquired,
        state.issued, state.queue, {state.issuedVersion.version + 1u}, state.request.rootDecision,
        state.currentOccurrence));
    ASSERT_TRUE(state.prepared.Valid());
    ASSERT_NE(nullptr, state.issued);
    EXPECT_FALSE(state.OwnReturned());
    EXPECT_FALSE(engine.CancelOriginalQueueLaneGroups(state.cohort, 0));
    ASSERT_TRUE(state.Abort());
    ASSERT_NO_FATAL_FAILURE(state.Cleanup());
}

TEST(OriginalSourceSubmission, InitialConditionalGenuineForeignGroupClockRefusesWithoutReturningOriginalPrefix) {
    auto firstOwner = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto secondOwner = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto& first = *firstOwner;
    auto& second = *secondOwner;
    InitialConditionalFactoryOutputHarness a(first), b(second);
    ASSERT_NO_FATAL_FAILURE(a.Acquire(true));
    ASSERT_NO_FATAL_FAILURE(b.Acquire(true));
    ASSERT_NO_FATAL_FAILURE(a.PrepareOriginal());
    ASSERT_NO_FATAL_FAILURE(b.PrepareOriginal());
    ASSERT_NO_FATAL_FAILURE(a.ReserveMembers());
    ASSERT_NO_FATAL_FAILURE(b.ReserveMembers());
    EXPECT_FALSE(first.PrepareIssuedOriginalQueueConditionalClock(*a.issued,
        *a.issuedGroups[0], *b.issuedGroups[1]));
    EXPECT_FALSE(second.PrepareIssuedOriginalQueueConditionalClock(*a.issued,
        *b.issuedGroups[0], *b.issuedGroups[1]));
    EXPECT_TRUE(a.prepared.Valid());
    EXPECT_FALSE(first.originalQueueConditionalHolders[a.queue.slot].clock.Valid());
    ASSERT_NO_FATAL_FAILURE(a.Clock());
    ASSERT_NO_FATAL_FAILURE(b.Clock());
    ASSERT_TRUE(a.Abort());
    ASSERT_TRUE(b.Abort());
    ASSERT_NO_FATAL_FAILURE(a.Cleanup());
    ASSERT_NO_FATAL_FAILURE(b.Cleanup());
}

TEST(OriginalSourceSubmission, InitialConditionalCommittedInitializingAbortUsesNativeRollbackBeforeGroupReturn) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto& engine = *owned;
    InitialConditionalFactoryOutputHarness state(engine);
    ASSERT_NO_FATAL_FAILURE(state.Acquire(true));
    ASSERT_NO_FATAL_FAILURE(state.PrepareOriginal());
    ASSERT_NO_FATAL_FAILURE(state.ReserveMembers());
    ASSERT_NO_FATAL_FAILURE(state.Clock());
    ASSERT_TRUE(engine.queueInstances.CommitPreparedConditional(&state.prepared, &state.version));
    ASSERT_EQ(state.issuedVersion.version, state.version.version);
    ASSERT_TRUE(state.Abort());
    ASSERT_TRUE(state.OwnReturned());
    ASSERT_NO_FATAL_FAILURE(state.Cleanup());
}

TEST(OriginalSourceSubmission, InitialConditionalOriginalOutputNormalSettlePreservesFreshMirrorAndForeignLifetime) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto& engine = *owned;
    InitialConditionalFactoryOutputHarness state(engine);
    ASSERT_NO_FATAL_FAILURE(state.Acquire());
    ASSERT_NO_FATAL_FAILURE(state.PrepareOriginal());
    ASSERT_NO_FATAL_FAILURE(state.ReserveMembers());
    ASSERT_NO_FATAL_FAILURE(state.Clock());
    BoundClock foreign;
    ASSERT_TRUE(engine.BorrowOriginalQueueConditionalClock(state.queue, &foreign));
    ASSERT_TRUE(engine.queueInstances.CommitPreparedConditional(&state.prepared, &state.version));
    harken::QueueInstanceSnapshot before{};
    ASSERT_TRUE(engine.queueInstances.Capture(state.queue, &before));
    ASSERT_EQ(state.version.version, before.pendingVersion);
    ASSERT_EQ(1u, before.logicMirror.version);
    ASSERT_TRUE(engine.queueInstances.ActivateConditional(state.queue, state.version,
        state.request.rootDecision, state.currentOccurrence));
    ASSERT_TRUE(engine.queueInstances.SettleActivated(state.queue, state.version));
    harken::QueueInstanceSnapshot after{};
    ASSERT_TRUE(engine.queueInstances.Capture(state.queue, &after));
    EXPECT_EQ(0u, after.pendingVersion);
    EXPECT_EQ(state.version.version, after.logicMirror.version);
    EXPECT_TRUE(state.OwnReturned());
    EXPECT_FALSE(engine.ReleaseOriginalQueue(state.queue));
    foreign.Release();
    ASSERT_NO_FATAL_FAILURE(state.Cleanup());
}

TEST(OriginalSourceSubmission, InitialConditionalSameCallOutputsJoinedTermKeepsNativeReceiversUntilEnd) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    InitialConditionalFactoryOutputHarness state(*owned);
    ASSERT_NO_FATAL_FAILURE(state.Acquire(true));
    ASSERT_NO_FATAL_FAILURE(state.PrepareOriginal());
    ASSERT_NO_FATAL_FAILURE(state.ReserveMembers());
    ASSERT_NO_FATAL_FAILURE(state.Clock());
    BoundClock foreign;
    ASSERT_TRUE(owned->BorrowOriginalQueueConditionalClock(state.queue, &foreign));
    foreign.Release();
    ASSERT_TRUE(owned->queueInstances.CommitPreparedConditional(&state.prepared, &state.version));
    ASSERT_TRUE(owned->queueInstances.CommitPreparedAcquire(&state.acquired, &state.queue));
    ASSERT_FALSE(state.OwnReturned());
    // Only native Engine teardown, with no external loans, disposes own holders.
    owned.reset();
}


// This exercises only actual bound C1 withdrawal. No provisional or New selector
// is issued, and no native state is assigned by the fixture.
struct GenuineBoundC1ProjectionHarness {
    HiddenBoundNativeFuture actual;
    harken::OriginalImmediateLaneAttemptLifetimePin originalLane;
    harken::OriginalImmediateLaneConditionalProjection projection;
    std::uint64_t frames = kFrames;
    bool lateRequestCalled = false;
    explicit GenuineBoundC1ProjectionHarness(harken::Engine& engine) : actual(engine) {}
    auto& Engine() { return actual.old.engine; }
    void Prepare(std::uint32_t pcmFrames = kFrames, bool issue = true,
                 bool prepareUnbound = false, bool beforePlacement = false) {
        frames = pcmFrames;
        ASSERT_NO_FATAL_FAILURE(RegisterInitialClockPcm(Engine(), pcmFrames));
        ASSERT_NO_FATAL_FAILURE(actual.old.PrepareBeforeHead(pcmFrames));
        ASSERT_NO_FATAL_FAILURE(actual.old.ReserveUnboundGroups(pcmFrames));
        if (issue && beforePlacement) {
            ASSERT_NO_FATAL_FAILURE(actual.ReserveOriginalForAudio(&originalLane));
            EXPECT_EQ(0u, Engine().mixer.BlocksRendered());
            EXPECT_FALSE(actual.old.episode.bundle.ObserveInstalled());
            ASSERT_TRUE(harken::OriginalImmediateLaneConditionalProjection::PrepareUnbound(
                Engine().queueInstances, actual.old.parent, actual.old.original, actual.old.clock,
                Engine().timelinePool, actual.old.Original(2), actual.old.Original(3),
                originalLane, &projection));
            EXPECT_FALSE(projection.BindPrepared());
            ASSERT_TRUE(projection.RequestWithdrawal());
        }
        ASSERT_NO_FATAL_FAILURE(actual.old.PublishSourceAndObserveFreshAck());
        ASSERT_NO_FATAL_FAILURE(actual.PlaceOriginalForAudio(&originalLane, pcmFrames));
        // Authentic Unbound clock and placed Lane still cannot issue the Bound capability.
        harken::OriginalImmediateLaneConditionalProjection unboundRefused;
        EXPECT_FALSE(harken::OriginalImmediateLaneConditionalProjection::PrepareBound(
            Engine().queueInstances, actual.old.parent, actual.old.original, actual.old.clock,
            Engine().timelinePool, actual.old.Original(2), actual.old.Original(3),
            originalLane, &unboundRefused));
        EXPECT_FALSE(unboundRefused.Valid());
        EXPECT_TRUE(actual.old.clock.Valid());
        EXPECT_TRUE(originalLane.Valid());
        if (issue && prepareUnbound && !beforePlacement) {
            ASSERT_TRUE(harken::OriginalImmediateLaneConditionalProjection::PrepareUnbound(
                Engine().queueInstances, actual.old.parent, actual.old.original, actual.old.clock,
                Engine().timelinePool, actual.old.Original(2), actual.old.Original(3),
                originalLane, &projection));
            ASSERT_TRUE(projection.Valid());
            EXPECT_FALSE(projection.BindPrepared());
            EXPECT_FALSE(Engine().immediateActionLane.Publish(actual.old.lane));
            harken::OriginalImmediateLaneConditionalProjection borrower;
            ASSERT_TRUE(projection.Borrow(&borrower));
            EXPECT_FALSE(borrower.RequestWithdrawal());
            EXPECT_FALSE(borrower.BindPrepared());
            borrower.Return();
            EXPECT_TRUE(projection.Valid());
            ASSERT_TRUE(projection.RequestWithdrawal());
        }
        ASSERT_TRUE(actual.old.Bind());
        if (!issue) return;
        if (prepareUnbound || beforePlacement) {
            ASSERT_TRUE(projection.BindPrepared());
            EXPECT_FALSE(projection.BindPrepared());
            return;
        }
        ASSERT_TRUE(harken::OriginalImmediateLaneConditionalProjection::PrepareBound(
            Engine().queueInstances, actual.old.parent, actual.old.original, actual.old.clock,
            Engine().timelinePool, actual.old.Original(2), actual.old.Original(3),
            originalLane, &projection));
        ASSERT_TRUE(projection.Valid());
    }
    static void LateRequest(void* context, std::uint32_t index) {
        auto& self = *static_cast<GenuineBoundC1ProjectionHarness*>(context);
        // The C1 controller is the real third immutable event. The shared C1
        // admission CAS has already won before this existing native hook fires.
        if (index != 2u) return;
        self.lateRequestCalled = true;
        EXPECT_TRUE(self.projection.RequestWithdrawal());
    }
    void PumpCurrent() {
        const auto wanted = actual.old.anchor / kFrames + 1u;
        ASSERT_GT(wanted, Engine().mixer.BlocksRendered());
        Engine().nullBackend.Pump(static_cast<std::uint32_t>(wanted - Engine().mixer.BlocksRendered()));
        ASSERT_EQ(wanted, Engine().mixer.BlocksRendered());
        ASSERT_GT(CurrentPeak(Engine()), 0.1f);
        harken::TimelineArmObservation arm{};
        ASSERT_TRUE(Engine().timelineArms.Observe(actual.old.episode.source.arm, &arm));
        ASSERT_EQ(wanted, arm.startedBlockSequence);
        ASSERT_EQ(harken::ArmState::Tombstoned, arm.state);
        ASSERT_TRUE(Engine().voices.ReleaseObserved(actual.old.episode.source.release));
        harken::CompletionProgressSnapshot progress{};
        ASSERT_TRUE(harken::CaptureCompletionProgressExact(*actual.old.episode.progress,
            actual.old.episode.occurrence,
            static_cast<std::uint32_t>(actual.old.episode.source.source >> 32), &progress));
        ASSERT_EQ(frames, progress.logicalOutputFramesProduced);
        ASSERT_EQ(wanted, progress.completedThroughBlockSequence);
        const auto* c0 = Engine().decisions.ResolveFrontierActivation(actual.old.decisions[0]);
        ASSERT_NE(nullptr, c0);
        harken::FrontierActivationReceipt receipt{};
        ASSERT_EQ(harken::FrontierReceiptRead::Ready,
            c0->Read(c0->Descriptor().entry, wanted, &receipt));
        ASSERT_EQ(harken::FrontierReceiptOutcome::Applied, receipt.outcome);
        ASSERT_EQ(wanted, receipt.audioSequence);
        harken::QueueInstanceSnapshot queue{};
        ASSERT_TRUE(Engine().queueInstances.Capture(actual.old.queue, &queue));
        ASSERT_EQ(wanted, queue.initialActivationSequence);
    }
    void PumpC1IfNextBucket() {
        const auto wanted = (actual.old.anchor + frames) / kFrames + 1u;
        if (Engine().mixer.BlocksRendered() < wanted) {
            Engine().nullBackend.Pump(static_cast<std::uint32_t>(wanted - Engine().mixer.BlocksRendered()));
        }
        ASSERT_EQ(wanted, Engine().mixer.BlocksRendered());
    }
    void ReconcileCurrent() {
        const auto* c0 = Engine().decisions.ResolveFrontierActivation(actual.old.decisions[0]);
        ASSERT_NE(nullptr, c0);
        harken::FrontierActivationReceipt receipt{};
        ASSERT_TRUE(Engine().decisions.ReconcileFrontierActivation(actual.old.decisions[0],
            c0->Descriptor().entry, Engine().mixer.BlocksRendered(), &receipt));
        ASSERT_EQ(harken::FrontierReceiptOutcome::Applied, receipt.outcome);
    }
    void Cleanup(bool c1Applied,
        harken::OriginalImmediateLaneConditionalProjection* foreign = nullptr) {
        if (c1Applied) {
            harken::QueueInstanceSnapshot tail{};
            ASSERT_TRUE(Engine().queueInstances.Capture(actual.old.queue, &tail));
            ASSERT_EQ(harken::ConditionalQueueActivation::Activated, tail.pendingActivation);
            ASSERT_TRUE(Engine().queueInstances.RetireConditionalForAbort(actual.old.queue,
                actual.old.version, actual.old.decisions[3]));
            const auto* c1 = Engine().decisions.ResolveFrontierActivation(actual.old.decisions[2]);
            ASSERT_NE(nullptr, c1);
            harken::FrontierActivationReceipt receipt{};
            ASSERT_TRUE(Engine().decisions.ReconcileFrontierActivation(actual.old.decisions[2],
                c1->Descriptor().entry, Engine().mixer.BlocksRendered(), &receipt));
            ASSERT_EQ(harken::FrontierReceiptOutcome::Applied, receipt.outcome);
        }
        ASSERT_NO_FATAL_FAILURE(ReconcileCurrent());
        projection.Return();
        ASSERT_TRUE(Engine().immediateActionLane.Cancel(actual.old.lane));
        if (foreign != nullptr) {
            ASSERT_TRUE(foreign->Valid());
            EXPECT_FALSE(Engine().immediateActionLane.ObserveOriginalUnpublishedStorageOnlyRetention(
                originalLane, actual.old.lane, actual.old.cohort, Engine().mixer.BlocksRendered()));
            EXPECT_FALSE(Engine().immediateActionLane.Retire(actual.old.lane,
                Engine().mixer.BlocksRendered(), &originalLane));
            EXPECT_TRUE(originalLane.Valid());
            foreign->Return();
        }
        ASSERT_TRUE(Engine().immediateActionLane.Retire(actual.old.lane,
            Engine().mixer.BlocksRendered(), &originalLane));
        ASSERT_FALSE(originalLane.Valid());
        for (std::uint32_t i = 4; i != 0; --i)
            ASSERT_TRUE(Engine().timelinePool.DetachPlan03GroupFrontier(actual.old.groups[i - 1],
                Engine().mixer.BlocksRendered()));
        ASSERT_TRUE(Engine().decisions.ReleaseFrontierDecisionBinding(actual.old.decisions[1]));
        ASSERT_TRUE(Engine().decisions.ReleaseFrontierDecisionBinding(actual.old.decisions[3]));
        Engine().ledger.Reap(actual.old.episode.source.source);
        ASSERT_TRUE(Engine().timelineOccurrences.ClearCompletionOwnerExact(actual.old.episode.occurrence,
            actual.old.episode.owner));
        harken::CompletionProgressSnapshot terminal{};
        ASSERT_TRUE(harken::CaptureCompletionProgressExact(*actual.old.episode.progress,
            actual.old.episode.occurrence,
            static_cast<std::uint32_t>(actual.old.episode.source.source >> 32), &terminal));
        actual.old.terminalSequence = terminal.completedThroughBlockSequence;
        ASSERT_TRUE(Engine().timelineOccurrences.UnbindCompletionProgress(actual.old.episode.occurrence,
            static_cast<std::uint32_t>(actual.old.episode.source.source >> 32)));
        ASSERT_TRUE(Engine().timelineOccurrences.Advance(actual.old.episode.occurrence,
            harken::OccurrenceStage::AwaitingEventFence));
        ASSERT_NO_FATAL_FAILURE(actual.old.ReturnAllAndReclaim());
        EXPECT_EQ(0u, Engine().voices.ActiveCount());
        EXPECT_EQ(nullptr, Engine().voices.Resolve(actual.old.episode.source.source));
        harken::TimelineArmObservation stale{};
        EXPECT_FALSE(Engine().timelineArms.Observe(actual.old.episode.source.arm, &stale));
    }
};

enum class ReplacementAdmissionCase {
    BeforeAck, ReadOnlyCapture, NativeWrite, LateAttach, MissedDeadline, Deferred, DeferredCancelled, PublicationHeld
};

void CheckOriginalConditionalReplacementAdmission(ReplacementAdmissionCase mode) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto& engine = *owned;
    GenuineBoundC1ProjectionHarness state(engine);
    auto& future = state.actual;
    ASSERT_NO_FATAL_FAILURE(RegisterInitialClockPcm(engine));
    ASSERT_NO_FATAL_FAILURE(future.old.PrepareBeforeHead());
    ASSERT_NO_FATAL_FAILURE(future.old.ReserveUnboundGroups());
    ASSERT_NO_FATAL_FAILURE(future.ReserveOriginalForAudio(&state.originalLane));
    ASSERT_TRUE(harken::OriginalImmediateLaneConditionalProjection::PrepareUnbound(
        engine.queueInstances, future.old.parent, future.old.original, future.old.clock,
        engine.timelinePool, future.old.Original(2), future.old.Original(3),
        state.originalLane, &state.projection));
    harken::PreparedActionFiring parent, child;
    const bool deferred = mode == ReplacementAdmissionCase::Deferred || mode == ReplacementAdmissionCase::DeferredCancelled;
    harken::LogicCommitSequence inheritedSequence = 0;
    if (deferred) {
        ASSERT_TRUE(engine.actionFirings.PrepareRoot(harkentest::ObjectId(301),
            future.old.episode.decisionRequest.world, 1, &parent));
        inheritedSequence = engine.logicCommits.Issue();
        ASSERT_NE(0u, inheritedSequence);
        ASSERT_TRUE(engine.actionFirings.Commit(&parent, inheritedSequence));
        ASSERT_TRUE(engine.actionFirings.AcquireDeferred(parent.Handle()));
        ASSERT_TRUE(engine.actionFirings.PrepareChild(parent.Handle(), harkentest::ObjectId(302),
            harken::ActionFiringOwnerStamp::DirectWorld(future.old.episode.decisionRequest.world), 1, &child));
    }
    ASSERT_NO_FATAL_FAILURE(future.PrepareCandidate(true, deferred ? &child : nullptr));
    harken::OriginalConditionalReplacementBatch batch(1);
    if (deferred) {
        ASSERT_TRUE(batch.PrepareDeferred(engine.queueInstances, &future.scope, child));
        EXPECT_FALSE(future.sequence.Valid());
        harken::PrivateSelectedQueueFutureSpanPool::Observation selected{};
        ASSERT_TRUE(future.owner.Capture(&selected));
        EXPECT_EQ(inheritedSequence, selected.reservedSequence);
        EXPECT_EQ(inheritedSequence, engine.logicCommits.Current());
    } else ASSERT_TRUE(batch.Prepare(engine.queueInstances, &future.scope, &future.sequence));
    if (mode != ReplacementAdmissionCase::LateAttach && mode != ReplacementAdmissionCase::PublicationHeld) {
        ASSERT_TRUE(batch.Attach(state.projection, future.spans, future.owner));
        EXPECT_FALSE(batch.Attach(state.projection, future.spans, future.owner));
    }
    // The ordinary bundle supplies its actual reservation state. This component
    // selects Old C1 only; it deliberately never publishes or applies New.
    std::uint32_t held = 0;
    harken::ActionCommitBundle action(1);
    const harken::CommitParticipant participant{
        &held,
        [](void* context, std::uint32_t count) noexcept {
            auto& value = *static_cast<std::uint32_t*>(context);
            if (value != 0 || count != 1) return false;
            value = count; return true;
        },
        [](void* context, std::uint32_t count) noexcept {
            auto& value = *static_cast<std::uint32_t*>(context);
            EXPECT_EQ(count, value); value = 0;
        },
        [](void*, std::uint32_t, harken::LogicCommitSequence) noexcept { ADD_FAILURE(); }};
    ASSERT_TRUE(action.AddStep(participant, 1));
    EXPECT_FALSE(batch.TryAccept(action));
    ASSERT_TRUE(action.Reserve());
    if (mode == ReplacementAdmissionCase::DeferredCancelled) {
        ASSERT_TRUE(engine.actionFirings.BumpCancellationEpoch(parent.Handle()));
        EXPECT_EQ(0u, child.InheritedSequence());
        EXPECT_FALSE(batch.TryAccept(action));
    }
    if (mode == ReplacementAdmissionCase::BeforeAck || mode == ReplacementAdmissionCase::Deferred) {
        EXPECT_FALSE(future.old.episode.bundle.ObserveInstalled());
        ASSERT_TRUE(batch.TryAccept(action));
        EXPECT_TRUE(batch.Accepted());
        EXPECT_FALSE(batch.TryAccept(action));
    }
    ASSERT_NO_FATAL_FAILURE(future.old.PublishSourceAndObserveFreshAck());
    ASSERT_NO_FATAL_FAILURE(future.PlaceOriginalForAudio(&state.originalLane));
    ASSERT_TRUE(future.old.Bind());
    ASSERT_TRUE(state.projection.BindPrepared());
    if (mode == ReplacementAdmissionCase::PublicationHeld) {
        ASSERT_TRUE(future.old.privateClock.ReturnPlacement(&future.old.placement));
        future.old.placement.Release();
        harken::PrivateImmediateLanePublicationProof publication;
        ASSERT_TRUE(engine.immediateActionLane.ProvePrivatePublication(future.old.lane, future.old.cohort, &publication));
        EXPECT_FALSE(state.projection.RequestWithdrawal());
        EXPECT_FALSE(batch.Attach(state.projection, future.spans, future.owner));
        EXPECT_FALSE(batch.Accepted());
        publication.Release();
        ASSERT_TRUE(batch.Attach(state.projection, future.spans, future.owner));
        ASSERT_TRUE(engine.immediateActionLane.ProvePrivatePublication(future.old.lane, future.old.cohort, &publication));
        EXPECT_FALSE(batch.TryAccept(action));
        EXPECT_FALSE(batch.Close());
        EXPECT_FALSE(batch.Accepted());
        EXPECT_FALSE(state.projection.RequestWithdrawal());
        EXPECT_FALSE(state.projection.TryRetireNativeConditional(engine.decisions, engine.mixer.BlocksRendered()));
        publication.Release();
        ASSERT_TRUE(batch.TryAccept(action));
        EXPECT_TRUE(batch.Accepted());
        ASSERT_TRUE(state.projection.RequestWithdrawal());
    }
    ASSERT_NO_FATAL_FAILURE(future.PublishOriginalForAudio(mode == ReplacementAdmissionCase::PublicationHeld));
    ASSERT_NO_FATAL_FAILURE(state.PumpCurrent());
    if (mode == ReplacementAdmissionCase::ReadOnlyCapture) {
        harken::ImmediateLaneTimelineFinalizer finalizer(engine.voices, engine.timelineArms);
        ASSERT_TRUE(finalizer.Attach(engine.immediateActionLane, kFrames));
        ASSERT_TRUE(finalizer.Capture(engine.mixer.BlocksRendered()));
        EXPECT_FALSE(batch.TryAccept(action));
        EXPECT_FALSE(batch.Accepted());
        finalizer.Discard();
        ASSERT_TRUE(batch.TryAccept(action));
        EXPECT_TRUE(batch.Accepted());
    }
    if (mode == ReplacementAdmissionCase::MissedDeadline) {
        harken::ImmediateLaneTimelineFinalizer heldCapture(engine.voices, engine.timelineArms);
        ASSERT_TRUE(heldCapture.Attach(engine.immediateActionLane, kFrames));
        const auto capturedSequence = engine.mixer.BlocksRendered();
        ASSERT_TRUE(heldCapture.Capture(capturedSequence));
        // A real outstanding reader prevents this callback from executing C1.
        engine.nullBackend.Pump(1);
        ASSERT_EQ(capturedSequence + 1u, engine.mixer.BlocksRendered());
        EXPECT_FALSE(batch.TryAccept(action));
        heldCapture.Discard();
        // The next actual callback takes the native missed-deadline branch.
        engine.nullBackend.Pump(1);
        ASSERT_EQ(capturedSequence + 2u, engine.mixer.BlocksRendered());
    } else ASSERT_NO_FATAL_FAILURE(state.PumpC1IfNextBucket());
    const bool accepted = mode == ReplacementAdmissionCase::BeforeAck || mode == ReplacementAdmissionCase::ReadOnlyCapture ||
        mode == ReplacementAdmissionCase::Deferred || mode == ReplacementAdmissionCase::PublicationHeld;
    if (!accepted) {
        if (mode == ReplacementAdmissionCase::LateAttach)
            EXPECT_FALSE(batch.Attach(state.projection, future.spans, future.owner));
        EXPECT_FALSE(batch.TryAccept(action));
        EXPECT_FALSE(batch.Accepted());
    }
    action.Abandon();
    EXPECT_EQ(0u, held);
    ASSERT_TRUE(batch.Close());
    ASSERT_NO_FATAL_FAILURE(future.CancelCandidate());
    engine.nullBackend.Pump(1);
    if (accepted || mode == ReplacementAdmissionCase::MissedDeadline) {
        if (!accepted) ASSERT_TRUE(state.projection.RequestWithdrawal());
        ASSERT_TRUE(state.projection.TryRetireNativeConditional(engine.decisions, engine.mixer.BlocksRendered()));
        EXPECT_FALSE(engine.decisions.IsCancelled(future.old.decisions[3]));
    }
    ASSERT_NO_FATAL_FAILURE(state.Cleanup(!accepted && mode != ReplacementAdmissionCase::MissedDeadline));
    if (deferred) {
        EXPECT_EQ(inheritedSequence, engine.logicCommits.Current());
        ASSERT_TRUE(engine.actionFirings.Abandon(&child));
        ASSERT_TRUE(engine.actionFirings.ReleaseDeferred(parent.Handle()));
        ASSERT_TRUE(engine.actionFirings.MarkTerminal(parent.Handle()));
        ASSERT_TRUE(engine.actionFirings.ReleaseRoot(parent.Handle()));
    }
}

TEST(OriginalSourceSubmission, ReplacementBatchAcceptsBeforeAckWithoutMutatingCurrent) {
    ASSERT_NO_FATAL_FAILURE(CheckOriginalConditionalReplacementAdmission(ReplacementAdmissionCase::BeforeAck));
}
TEST(OriginalSourceSubmission, ReplacementBatchWaitsForActualReadOnlyCaptureToEnd) {
    ASSERT_NO_FATAL_FAILURE(CheckOriginalConditionalReplacementAdmission(ReplacementAdmissionCase::ReadOnlyCapture));
}
TEST(OriginalSourceSubmission, ReplacementBatchActualOldNativeWritePermanentlyWins) {
    ASSERT_NO_FATAL_FAILURE(CheckOriginalConditionalReplacementAdmission(ReplacementAdmissionCase::NativeWrite));
}
TEST(OriginalSourceSubmission, ReplacementBatchRefusesAttachmentAfterActualOldNativeWrite) {
    ASSERT_NO_FATAL_FAILURE(CheckOriginalConditionalReplacementAdmission(ReplacementAdmissionCase::LateAttach));
}
TEST(OriginalSourceSubmission, ReplacementBatchActualMissedDeadlinePermanentlyWins) {
    ASSERT_NO_FATAL_FAILURE(CheckOriginalConditionalReplacementAdmission(ReplacementAdmissionCase::MissedDeadline));
}
TEST(OriginalSourceSubmission, ReplacementBatchUsesActualDeferredParentOrderWithoutIssuingAgain) {
    ASSERT_NO_FATAL_FAILURE(CheckOriginalConditionalReplacementAdmission(ReplacementAdmissionCase::Deferred));
}
TEST(OriginalSourceSubmission, ReplacementBatchRefusesDeferredParentCancelledAfterPreparation) {
    ASSERT_NO_FATAL_FAILURE(CheckOriginalConditionalReplacementAdmission(ReplacementAdmissionCase::DeferredCancelled));
}
TEST(OriginalSourceSubmission, BoundC1PublicationHoldPreservesActualReplacementAndWithdrawalRetries) {
    ASSERT_NO_FATAL_FAILURE(CheckOriginalConditionalReplacementAdmission(ReplacementAdmissionCase::PublicationHeld));
}

struct SelectedSourceStop {
    std::optional<harken::AuthoredStopOperation> operation;
    std::optional<harken::AuthoredStopFenceDescriptor> fence;
    harken::TimelineEventPayload fencePayload{};
    harken::TimelineProgramView fenceProgram{};
    harken::TimelineArmTable::PendingAuthoredControlInput pending{};
    harken::TimelineArmTable::PreparedCommitProof commit;
    harken::TimelineArmTable::OriginalControlHeaderRowOwner original;
    harken::ImmediateLaneHandle lane{};

    void Prepare(harken::Engine& engine, HiddenBoundNativeFuture& future,
        GenuinePreparedSingleEpisode& source) {
        ASSERT_TRUE(engine.authoredStopResources.ConstructThrough(1));
        const auto stopFrame = future.old.anchor + kFrames + kFrames / 2u;
        const auto epoch = engine.playableInstances.Classify(source.parent).scheduleEpoch;
        operation.emplace(harken::AuthoredStopOperationIdentity{0, 1, source.parent, epoch, 57, 1, stopFrame});
        auto& resource = engine.authoredStopResources[0];
        resource.runtime = {23, resource.payloads.data(), 0};
        pending = {&*operation, {23, &resource.runtime}, resource.events.data(), 1,
            stopFrame, source.source.arm, {}};
        pending.target.subjectId = kSound;
        // The real Source/Body owners retain identity while the temporary arm
        // proof is returned so the control factory can inspect that same row.
        source.armProof.Release();
        harken::TimelineArmToken rejected;
        EXPECT_FALSE(engine.timelineArms.PreparePendingAuthoredControl(pending, &rejected));
        auto foreign = pending;
        foreign.sourceStart = future.old.episode.source.arm;
        EXPECT_FALSE(future.spans.PrepareSourceAuthoredControl(future.owner, engine.timelineArms, foreign, &rejected));
        ASSERT_TRUE(future.spans.PrepareSourceAuthoredControl(future.owner, engine.timelineArms, pending, &resource.arm));
        ASSERT_TRUE(engine.timelineArms.ProvePreparedCommit(source.armInput, &source.armProof));
        ASSERT_TRUE(engine.timelineArms.ProvePendingAuthoredControlCommit(resource.arm, pending, &commit));
        ASSERT_TRUE(engine.timelineArms.IssueOriginalControlHeaderRowOwner(commit, &original));
        harken::CommandRecord header{};
        header.opcode = harken::kOpcodePlay;
        header.payload.play.batch = {harken::kPlayLedBatchVersion,
            harken::PlayLedBatchKind::TimelineControlArm, 0, 1,
            (static_cast<std::uint64_t>(resource.arm.generation) << 32) | resource.arm.slot};
        header.payload.play.handle = header.payload.play.sourceIdentity = source.source.source;
        ASSERT_TRUE(source.FreezeHead(engine, &header));
        ASSERT_TRUE(engine.commands.IssueOriginalControlHeaderRange(source.batch, 1,
            std::move(original), &resource, &resource.originalControlHeader));
        ASSERT_TRUE(engine.immediateActionLane.Reserve(57, {2, 0, 0, 1}, &lane));
    }

    void BindAndPublish(harken::Engine& engine, HiddenBoundNativeFuture& future,
        GenuinePreparedSingleEpisode& source, harken::LogicCommitSequence sequence) {
        auto& resource = engine.authoredStopResources[0];
        ASSERT_EQ(harken::CommandQueue::OriginalControlDisposition::Settled,
            resource.originalControlHeader.ControlDisposition());
        const auto stopFrame = operation->Identity().stopFrame;
        auto& payload = resource.payloads[0];
        payload.kind = harken::TimelineEventKind::Stop;
        payload.reserved = harken::kTimelineEventTransientControlArm | harken::kTimelineEventAuthoredRootStop;
        payload.target = pending.target;
        payload.authoredStopOperation = operation->Identity();
        payload.envelopeEndFrame = stopFrame;
        auto& event = resource.events[0];
        event.executeFrame = stopFrame;
        event.logicCommitSequence = sequence;
        event.executionOrdinal = 1;
        event.subeventPhase = 50;
        event.armToken = resource.arm;
        resource.runtime.payloadCount = 1;
        const harken::TimelineArmTable::AuthoredControlInput final{&*operation,
            pending.program, &event, 1, future.old.anchor + kFrames, stopFrame,
            static_cast<std::uint32_t>(source.source.source),
            static_cast<std::uint32_t>(source.source.source >> 32), pending.target,
            stopFrame / kFrames * kFrames};
        harken::TimelineArmReaderPin reader;
        ASSERT_TRUE(engine.timelineArms.PinFrontierReader(source.source.arm, &reader));
        harken::TimelineArmTable::PendingAuthoredControlBindingProof binding;
        EXPECT_FALSE(engine.timelineArms.ProvePendingAuthoredControlBinding(resource.arm, reader, final, &binding));
        ASSERT_TRUE(future.spans.ProveSourceAuthoredControlBinding(future.owner, engine.timelineArms, resource.arm, reader, final, &binding));
        engine.timelineArms.ConsumePendingAuthoredControlBinding(std::move(binding));
        ASSERT_TRUE(reader.Release(engine.mixer.BlocksRendered()));
        fencePayload = payload;
        fencePayload.reserved = harken::kTimelineEventAuthoredRootStop;
        fencePayload.target = {};
        fence.emplace(&*operation, operation->Identity(), harken::DecisionToken{}, &fenceProgram, 24);
        fenceProgram = {24, &fencePayload, 1, harken::TimelineProgramRole::AuthoredStopFence, &*fence};
        std::array<harken::ImmediateLaneEvent, 2> events{};
        for (std::uint32_t i = 0; i < events.size(); ++i) {
            auto& item = events[i];
            item.timeDomain = harken::ImmediateLaneTimeDomain::FixedAbsoluteFrame;
            item.fixedFrame = item.event.executeFrame = stopFrame;
            item.event.logicCommitSequence = sequence;
            item.event.executionOrdinal = 1;
            item.event.eventIndex = i;
            item.event.subeventPhase = 50;
            item.event.program = i == 0 ? pending.program : harken::TimelineTransactionProgramRef{24, &fenceProgram};
            if (i == 0) item.event.armToken = resource.arm;
        }
        ASSERT_TRUE(engine.immediateActionLane.Prepare(lane, events.data(), events.size()));
        ASSERT_TRUE(engine.immediateActionLane.PinArms(lane, engine.timelineArms));
        ASSERT_TRUE(engine.immediateActionLane.TryPlaceExactPrivate(lane, future.old.anchor));
        ASSERT_TRUE(engine.timelineArms.Publish(resource.arm));
        ASSERT_TRUE(engine.immediateActionLane.Publish(lane));
    }

    void Reclaim(harken::Engine& engine) {
        auto& resource = engine.authoredStopResources[0];
        ASSERT_NE(0u, operation->AppliedAudioSequence());
        ASSERT_TRUE(engine.immediateActionLane.Retire(lane, engine.mixer.BlocksRendered()));
        ASSERT_EQ(harken::AuthoredStopCancelDisposition::Cancelled, operation->Cancel());
        ASSERT_TRUE(resource.ReturnOriginalControlHeaderAfterDisposition());
        ASSERT_TRUE(engine.timelineArms.Reclaim(resource.arm, engine.mixer.BlocksRendered()));
        resource.arm = {};
    }
};

void CheckAcceptedQueueSelectionKeepsOriginalClock(bool bindBeforeSelection, bool publishSelected = false,
    bool installNewSource = false, bool scheduledStop = false, bool mixedReset = false) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto& engine = *owned;
    GenuineBoundC1ProjectionHarness state(engine);
    auto& future = state.actual;
    GenuinePreparedSingleEpisode newSource;
    harken::TimelineEventPayload newPayload{};
    harken::TimelineProgramView newProgram{};
    SelectedSourceStop stop;
    if (scheduledStop) ASSERT_TRUE(installNewSource);
    if (installNewSource) ASSERT_TRUE(bindBeforeSelection && publishSelected);
    ASSERT_NO_FATAL_FAILURE(RegisterInitialClockPcm(engine));
    ASSERT_NO_FATAL_FAILURE(future.old.PrepareBeforeHead());
    ASSERT_NO_FATAL_FAILURE(future.old.ReserveUnboundGroups());
    ASSERT_NO_FATAL_FAILURE(future.ReserveOriginalForAudio(&state.originalLane));
    ASSERT_TRUE(harken::OriginalImmediateLaneConditionalProjection::PrepareUnbound(
        engine.queueInstances, future.old.parent, future.old.original, future.old.clock,
        engine.timelinePool, future.old.Original(2), future.old.Original(3),
        state.originalLane, &state.projection));
    if (bindBeforeSelection) {
        ASSERT_NO_FATAL_FAILURE(future.old.PublishSourceAndObserveFreshAck());
        ASSERT_NO_FATAL_FAILURE(future.PlaceOriginalForAudio(&state.originalLane));
        ASSERT_TRUE(future.old.Bind());
        ASSERT_TRUE(state.projection.BindPrepared());
        ASSERT_NO_FATAL_FAILURE(future.PublishOriginalForAudio());
    }
    harken::PrivateSelectedQueueFutureDomain selectedDomain;
    harken::PrivateSelectedQueueFutureCalendar selectedCalendar;
    harken::OriginalImmediateLaneAttemptLifetimePin foreignSelectedLane;
    harken::QueueInstanceToken ordinaryQueue{};
    harken::PreparedQueueResetSetV1 ordinaryReset;
    harken::QueueResetTargetV1 resetTarget{};
    resetTarget.world = future.old.episode.decisionRequest.world;
    resetTarget.containerStableId = harkentest::ObjectId(13);
    resetTarget.programSemanticFingerprint = future.old.fingerprint;
    if (mixedReset) {
        harken::QueueInstanceCreateRequest create{};
        create.world = resetTarget.world;
        create.rootInstance = future.old.episode.parent;
        create.containerStableId = resetTarget.containerStableId;
        create.programSemanticFingerprint = resetTarget.programSemanticFingerprint;
        create.childCount = 2;
        create.initial.version = 1;
        create.resetInitial = create.initial;
        ASSERT_TRUE(engine.queueInstances.Acquire(create, &ordinaryQueue));
        ASSERT_EQ(harken::QueueResetStatusV1::Ok,
            engine.queueInstances.PrepareNativeConditionalCatalogResetScope(resetTarget, &future.scope));
        ASSERT_EQ(1u, future.scope.MatchedCount());
    }
    ASSERT_NO_FATAL_FAILURE(future.PrepareCandidate(!installNewSource, nullptr,
        mixedReset ? &future.scope : nullptr, nullptr,
        publishSelected ? &selectedDomain : nullptr, publishSelected ? &state.originalLane : nullptr,
        installNewSource ? &newSource : nullptr, !scheduledStop));
    if (scheduledStop) ASSERT_NO_FATAL_FAILURE(stop.Prepare(engine, future, newSource));
    harken::OriginalConditionalReplacementBatch batch(1);
    ASSERT_TRUE(batch.Prepare(engine.queueInstances, &future.scope, &future.sequence));
    harken::LogicCommitCoordinator::PreparedIssueIdentity acceptedIdentity;
    ASSERT_TRUE(engine.logicCommits.CapturePreparedIssueIdentity(batch.SequenceIssue(), &acceptedIdentity));
    ASSERT_TRUE(batch.Attach(state.projection, future.spans, future.owner));
    // Genuine structural loans alone cannot commit a replacement. Its actual
    // independent Decisions must own the complete private native participants.
    EXPECT_FALSE(batch.PrepareQueueSelections(0));
    harken::DecisionTable::ConsumePreparedReservationCommit(std::move(future.decisionProof));
    harken::DecisionTable::ConsumePreparedReservationCommit(std::move(future.coordinatorProof));
    const auto newCompletionDecision = newSource.decisionPreparation.Token();
    if (installNewSource)
        harken::DecisionTable::ConsumePreparedReservationCommit(std::move(future.sourceCompletionTicket));
    const auto sequence = batch.SequenceIssue().Sequence();
    const auto* controller = engine.decisions.PrepareFrontierControlProgram(future.token[0], 17);
    ASSERT_NE(nullptr, controller);
    const auto rootInfo = engine.playableInstances.Classify(future.old.episode.parent);
    const auto rootEpoch = bindBeforeSelection ? rootInfo.scheduleEpoch
        : future.old.episode.originalParent.ScheduleEpoch();
    ASSERT_NE(0u, rootEpoch);
    if (bindBeforeSelection) ASSERT_TRUE(rootInfo.live);
    else ASSERT_TRUE(future.old.episode.originalParent.Valid());
    harken::FrontierOwnerIdentity currentIdentity{};
    currentIdentity.child = currentIdentity.invocation = future.old.episode.parent;
    currentIdentity.childEpoch = currentIdentity.invocationEpoch = rootEpoch;
    currentIdentity.serviceSlot = 0;
    currentIdentity.serviceGeneration = 1;
    const harken::FrontierEntryReceiptRef selectedEntry{future.token[0], 7, currentIdentity};
    if (publishSelected) {
        // Reserve the actual independent native Lane and its immutable event
        // before the winner. Only real first-S binding happens after Source ACK.
        harken::ImmediateLaneEvent event{};
        event.relativeFrame = kFrames;
        event.event.program = {controller->generation, controller};
        event.event.subeventPhase = 10;
        event.event.logicCommitSequence = sequence;
        event.event.ownerInstanceHandle = currentIdentity.child;
        event.event.ownerInstanceScheduleEpoch = currentIdentity.childEpoch;
        event.event.frontier.kind = harken::FrontierOperationKind::Entry;
        event.event.frontier.entry = selectedEntry;
        event.event.frontier.owner = currentIdentity;
        event.event.frontier.group = future.group[0];
        event.event.frontier.world = future.old.episode.decisionRequest.world;
        event.event.frontier.transport = engine.timelinePool.Epoch();
        event.event.frontier.operationEventCount = 1;
        event.event.frontier.entryEventOrdinal = 0;
        std::array<harken::ImmediateLaneEvent, 2> nativeEvents{event, {}};
        harken::ImmediateLaneMemberManifest member{};
        if (installNewSource) {
            newPayload.kind = harken::TimelineEventKind::Start;
            newPayload.target.subjectId = kSound;
            newPayload.reserved = harken::kTimelineEventTransientStartArm;
            newProgram = {17, &newPayload, 1, harken::TimelineProgramRole::Voice};
            auto& start = nativeEvents[1];
            start = event;
            start.futureGroupedStart = true;
            start.armProgramGeneration = 17;
            start.event.program = {17, &newProgram};
            start.event.armToken = newSource.source.arm;
            start.event.subeventPhase = 20;
            start.event.frontier.kind = harken::FrontierOperationKind::Dependent;
            start.event.frontier.group = future.group[1];
            start.event.frontier.entryEventOrdinal = UINT32_MAX;
            member.group = future.group[1];
            member.arm = newSource.source.arm;
            member.eventOrdinal = 1;
            member.memberIndex = 0;
            member.future = true;
            member.relativeFrame = kFrames;
            member.programGeneration = 17;
            member.target = newPayload.target;
            member.ownerInstance = currentIdentity.child;
            member.ownerEpoch = currentIdentity.childEpoch;
        }
        ASSERT_TRUE(selectedCalendar.Prepare(future.spans, future.owner, selectedDomain,
            nativeEvents.data(), installNewSource ? 2u : 1u, installNewSource ? &member : nullptr,
            engine.timelineArms));
        future.lane = selectedCalendar.HandleObservation();
        EXPECT_NE(future.old.lane.slot, future.lane.slot);
        ASSERT_TRUE(selectedCalendar.BorrowOriginalAttempt(&foreignSelectedLane));
        EXPECT_FALSE(selectedCalendar.PublishSelected());
    }
    for (std::uint32_t i = 0; i < 2; ++i) {
        const bool sourceMember = installNewSource && i == 1u;
        ASSERT_TRUE(engine.armGroups.Reserve(future.token[i], sourceMember ? &newSource.source.arm : nullptr,
            sourceMember ? 1u : 0u, &future.armGroup[i]));
        harken::DecisionParticipantRef participant{};
        participant.kind = i == 0 ? harken::DecisionParticipantKind::Subdecision
                                  : harken::DecisionParticipantKind::QueueInstance;
        participant.slot = i == 0 ? future.token[1].slot : future.old.queue.slot;
        participant.generation = i == 0 ? future.token[1].generation : future.old.queue.generation;
        harken::DecisionCommitPayload payload{};
        payload.pages = {static_cast<std::uint32_t>(future.group[i].committedRow), future.group[i].rowClaimGeneration};
        payload.armGroup = future.armGroup[i];
        payload.participants = &participant;
        payload.participantCount = 1;
        ASSERT_TRUE(engine.decisions.Commit(future.token[i], payload));
    }
    harken::QueueInstanceSnapshot before{};
    ASSERT_TRUE(engine.queueInstances.Capture(future.old.queue, &before));
    const auto resetFrame = bindBeforeSelection ? engine.mixer.BlocksRendered() * kFrames : 0u;
    if (mixedReset) {
        EXPECT_EQ(harken::QueueResetStatusV1::Backpressure,
            engine.queueInstances.PrepareReset(resetTarget, &ordinaryReset, resetFrame));
        ASSERT_EQ(harken::QueueResetStatusV1::Ok,
            engine.queueInstances.PrepareResetRemainder(resetTarget, batch.Scope(), &ordinaryReset, resetFrame));
        EXPECT_EQ(1u, ordinaryReset.MatchedCount());
        harken::PreparedQueueResetSetV1 moved(std::move(ordinaryReset));
        ordinaryReset = std::move(moved);
        EXPECT_EQ(harken::QueueResetStatusV1::InvalidInput,
            engine.queueInstances.CommitPreparedReset(&ordinaryReset, sequence, resetFrame));
        EXPECT_TRUE(ordinaryReset.Valid());
    }
    ASSERT_TRUE(batch.PrepareQueueSelections(resetFrame));
    harken::QueueInstanceSnapshot prepared{};
    ASSERT_TRUE(engine.queueInstances.Capture(future.old.queue, &prepared));
    EXPECT_EQ(before.pendingVersion, prepared.pendingVersion);
    EXPECT_EQ(before.resetPending, prepared.resetPending);
    EXPECT_TRUE(engine.queueInstances.IsCurrentOriginalConditionalFutureReservation(future.old.original));
    std::uint32_t held = 0;
    harken::ActionCommitBundle action(1);
    const harken::CommitParticipant participant{
        &held,
        [](void* context, std::uint32_t count) noexcept {
            *static_cast<std::uint32_t*>(context) = count; return count == 1;
        },
        [](void* context, std::uint32_t) noexcept { *static_cast<std::uint32_t*>(context) = 0; },
        [](void* context, std::uint32_t count, harken::LogicCommitSequence stamp) noexcept {
            EXPECT_NE(0u, stamp);
            EXPECT_EQ(count, *static_cast<std::uint32_t*>(context));
            *static_cast<std::uint32_t*>(context) = 0;
        }};
    ASSERT_TRUE(action.AddStep(participant, 1));
    EXPECT_FALSE(batch.TryAccept(action));
    ASSERT_TRUE(action.Reserve());
    ASSERT_TRUE(batch.TryAccept(action));
    harken::LogicCommitCoordinator foreign;
    harken::LogicCommitCoordinator::PreparedIssue foreignIssue;
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, foreign.PrepareIssue(&foreignIssue));
    auto occupiedReceipt = foreign.ConsumePreparedIssue(std::move(foreignIssue));
    EXPECT_EQ(0u, batch.ConsumeAcceptedOrder(&occupiedReceipt));
    harken::LogicCommitCoordinator::ConsumedIssue acceptedReceipt;
    EXPECT_EQ(sequence, batch.ConsumeAcceptedOrder(&acceptedReceipt));
    EXPECT_TRUE(acceptedReceipt.MatchesOriginalReservation(acceptedIdentity));
    EXPECT_FALSE(occupiedReceipt.MatchesOriginalReservation(acceptedIdentity));
    auto movedReceipt = std::move(acceptedReceipt);
    EXPECT_FALSE(acceptedReceipt.MatchesOriginalReservation(acceptedIdentity));
    EXPECT_TRUE(movedReceipt.MatchesOriginalReservation(acceptedIdentity));
    EXPECT_EQ(sequence, engine.logicCommits.Current());
    if (publishSelected)
        EXPECT_FALSE(future.spans.RollbackUnselectedDecisionCommits(future.owner, selectedDomain));
    if (mixedReset) {
        EXPECT_EQ(harken::QueueResetStatusV1::InvalidInput,
            engine.queueInstances.CommitPreparedReset(&ordinaryReset, sequence + 1u, resetFrame));
        ASSERT_EQ(harken::QueueResetStatusV1::Ok,
            engine.queueInstances.CommitPreparedReset(&ordinaryReset, sequence, resetFrame));
        EXPECT_FALSE(ordinaryReset.Valid());
    }
    ASSERT_TRUE(action.CommitReserved(sequence));
    if (installNewSource) {
        const auto head = engine.commands.PublishedHead();
        const auto beforeAck = engine.mixer.BlocksRendered();
        harken::OccurrenceStatePool::ConsumePreparedSingleCompletionCommit(std::move(newSource.occurrenceProof));
        harken::VoiceLedger::ConsumePreparedSingleCompletionCommit(std::move(newSource.ledgerProof));
        harken::VoicePool::ConsumePreparedCommit(std::move(newSource.voiceProof));
        harken::TimelineArmTable::ConsumePreparedCommit(std::move(newSource.armProof));
        if (scheduledStop) harken::TimelineArmTable::ConsumePreparedCommit(std::move(stop.commit));
        engine.originalSourceSubmissions.PublishPreparedChild(newSource.commands[0]);
        (void)engine.commands.ConsumePreparedBatch(std::move(newSource.batch));
        engine.AcceptSourceCommandPublications();
        ASSERT_EQ(head + (scheduledStop ? 2u : 1u), engine.commands.PublishedHead());
        engine.nullBackend.Pump(1);
        harken::TimelineArmObservation fresh{};
        ASSERT_TRUE(engine.timelineArms.Observe(newSource.source.arm, &fresh));
        ASSERT_EQ(beforeAck + 1u, fresh.armedCompletionSequence);
        ASSERT_EQ(0u, fresh.startedBlockSequence);
        ASSERT_TRUE(engine.timelineArms.BindPendingImmediateStart(newSource.source.arm,
            future.old.anchor + kFrames, future.old.anchor + kFrames));
        ASSERT_TRUE(engine.armGroups.Acknowledge(future.armGroup[1], newSource.source.arm,
            fresh.armedCompletionSequence));
    }
    harken::QueueInstanceSnapshot accepted{};
    ASSERT_TRUE(engine.queueInstances.Capture(future.old.queue, &accepted));
    EXPECT_EQ(before.logicMirror.version, accepted.logicMirror.version);
    EXPECT_EQ(before.logicMirror.cursor, accepted.logicMirror.cursor);
    EXPECT_EQ(resetFrame == 0, accepted.resetPending);
    EXPECT_TRUE(engine.queueInstances.ProvePendingConditionalIdentity(
        future.old.queue, {accepted.pendingVersion}, future.token[1]));
    EXPECT_FALSE(engine.queueInstances.ProvePendingConditionalIdentity(
        future.old.queue, future.old.version, future.old.decisions[3]));
    EXPECT_FALSE(engine.queueInstances.IsCurrentOriginalConditionalFutureReservation(future.old.original));
    harken::CompletionOwnerRef currentOwner{};
    ASSERT_TRUE(engine.timelineOccurrences.CaptureCompletionOwner(future.old.episode.occurrence, &currentOwner));
    EXPECT_TRUE(harken::SameCompletionOwner(currentOwner, future.old.episode.owner));
    if (!bindBeforeSelection) {
        EXPECT_FALSE(future.old.episode.bundle.ObserveInstalled());
        ASSERT_NO_FATAL_FAILURE(future.old.PublishSourceAndObserveFreshAck());
        ASSERT_NO_FATAL_FAILURE(future.PlaceOriginalForAudio(&state.originalLane, kFrames, 1024, true));
        ASSERT_TRUE(future.old.Bind());
        ASSERT_TRUE(state.projection.BindPrepared());
    }
    ASSERT_TRUE(future.spans.BindOriginalFirstS(future.owner, future.old.clock));
    if (!bindBeforeSelection) ASSERT_NO_FATAL_FAILURE(future.PublishOriginalForAudio());
    if (publishSelected) {
        ASSERT_TRUE(selectedCalendar.BindOriginalFirstS(future.old.clock));
        for (std::uint32_t i = 0; i < 2; ++i) {
            harken::FrontierGroupBinding binding{};
            binding.role = i == 0 ? harken::FrontierGroupRole::Coordinator : harken::FrontierGroupRole::EntryMember;
            binding.entry = selectedEntry;
            binding.operation.kind = i == 0 ? harken::FrontierOperationKind::Entry : harken::FrontierOperationKind::Dependent;
            binding.operation.owner = currentIdentity;
            binding.operation.order.executeFrame = future.old.anchor + kFrames;
            binding.operation.order.logicCommitSequence = sequence;
            binding.operation.order.subeventPhase = 10;
            binding.operation.group = future.group[i];
            binding.operation.transport = engine.timelinePool.Epoch();
            binding.operation.world = future.old.episode.decisionRequest.world;
            binding.primary = future.group[0];
            if (i == 0) {
                binding.prerequisite = {future.old.decisions[0], 1, currentIdentity};
                binding.prerequisiteMember = future.old.groups[1];
                harken::FrontierActivationDescriptor descriptor{};
                descriptor.entry = binding.entry;
                descriptor.primary = binding.primary;
                descriptor.order = binding.operation.order;
                descriptor.worldGeneration = binding.operation.world;
                descriptor.worldSemanticFingerprint = future.old.fingerprint;
                descriptor.expectedMemberCount = descriptor.expectedEntryEventCount = 1;
                descriptor.programGeneration = controller->generation;
                ASSERT_TRUE(engine.decisions.PrepareFrontierActivation(future.token[0], descriptor, engine.timelinePool));
            } else binding.queueConditional = {future.old.queue.slot, future.old.queue.generation,
                before.logicMirror.version, accepted.pendingVersion, future.old.episode.occurrence,
                installNewSource ? newSource.occurrence : future.old.episode.occurrence};
            ASSERT_TRUE(engine.timelinePool.BindPlan03GroupFrontier(future.group[i], binding));
            if (i == 1) {
                harken::Plan03PageGroupView actual{};
                ASSERT_TRUE(engine.timelinePool.ResolvePlan03GroupSliceProducer(future.group[i], &actual));
                ASSERT_NE(nullptr, actual.frontier);
                ASSERT_TRUE(engine.queueInstances.BindFrontierConditional(future.old.queue, *actual.frontier, engine.timelinePool));
                ASSERT_TRUE(engine.decisions.BindFrontierDecision(future.token[1], *actual.frontier, engine.timelinePool));
            }
        }
        ASSERT_TRUE(selectedCalendar.PinPrivate(engine.timelineArms, engine.decisions,
            engine.config.maxPlan03GroupHeadersPerPage));
        if (scheduledStop) ASSERT_NO_FATAL_FAILURE(stop.BindAndPublish(engine, future, newSource, sequence));
        if (installNewSource) {
            harken::Plan03PageGroupView actual{};
            ASSERT_TRUE(engine.timelinePool.ResolvePlan03GroupSliceProducer(future.group[1], &actual));
            ASSERT_TRUE(engine.timelineArms.BindFrontierAdmission(newSource.source.arm, *actual.frontier, *actual.lease));
            ASSERT_TRUE(engine.timelineArms.Publish(newSource.source.arm));
        }
        ASSERT_TRUE(selectedCalendar.PublishSelected());
        EXPECT_FALSE(selectedCalendar.RetireSelected(engine.mixer.BlocksRendered()));
    }
    ASSERT_NO_FATAL_FAILURE(state.PumpCurrent());
    engine.queueInstances.ApplyDue(engine.mixer.BlocksRendered() * kFrames);
    if (mixedReset) {
        harken::QueueInstanceSnapshot ordinary{};
        ASSERT_TRUE(engine.queueInstances.Capture(ordinaryQueue, &ordinary));
        EXPECT_TRUE(ordinary.resetPending);
    }
    ASSERT_NO_FATAL_FAILURE(state.PumpC1IfNextBucket());
    if (installNewSource) {
        harken::TimelineArmObservation played{};
        ASSERT_TRUE(engine.timelineArms.Observe(newSource.source.arm, &played));
        EXPECT_EQ(future.old.anchor / kFrames + 2u, played.startedBlockSequence);
        EXPECT_GT(CurrentPeak(engine), 0.1f);
        harken::CompletionProgressSnapshot completed{};
        ASSERT_TRUE(harken::CaptureCompletionProgressExact(*newSource.progress, newSource.occurrence,
            static_cast<std::uint32_t>(newSource.source.source >> 32), &completed));
        EXPECT_EQ(kFrames, completed.logicalOutputFramesProduced);
        EXPECT_EQ(played.startedBlockSequence, completed.completedThroughBlockSequence);
        EXPECT_TRUE(engine.voices.ReleaseObserved(newSource.source.release));
        harken::CompletionOwnerRef actualOwner{};
        ASSERT_TRUE(engine.timelineOccurrences.CaptureCompletionOwner(newSource.occurrence, &actualOwner));
        EXPECT_TRUE(harken::SameCompletionOwner(newSource.owner, actualOwner));
        EXPECT_NE(future.token[1].slot, actualOwner.singleDecision.slot);
        if (scheduledStop) {
            EXPECT_EQ(played.startedBlockSequence, stop.operation->AppliedAudioSequence());
            // A Stop without an authored envelope begins the existing 5 ms
            // declick at its exact sample; the short source then ends naturally.
            constexpr auto declickFrames = kRate / 200u;
            for (std::uint32_t frame = 0; frame < kFrames; ++frame) {
                const auto gain = frame < kFrames / 2u ? 1.0f :
                    static_cast<float>(declickFrames - (frame - kFrames / 2u + 1u)) /
                        static_cast<float>(declickFrames);
                for (std::uint32_t channel = 0; channel < engine.nullBackend.Channels(); ++channel)
                    EXPECT_NEAR(0.25f * gain,
                        engine.nullBackend.LastBlock()[frame * engine.nullBackend.Channels() + channel], 1e-6f);
            }
        }
    }
    engine.nullBackend.Pump(1);
    ASSERT_TRUE(state.projection.TryCompleteNativeWithdrawal(engine.decisions, engine.mixer.BlocksRendered()));
    EXPECT_FALSE(state.projection.TryRetireNativeConditional(engine.decisions, engine.mixer.BlocksRendered()));
    harken::QueueInstanceSnapshot afterOldEnd{};
    ASSERT_TRUE(engine.queueInstances.Capture(future.old.queue, &afterOldEnd));
    if (publishSelected) {
        EXPECT_EQ(harken::ConditionalQueueActivation::Activated, afterOldEnd.pendingActivation);
        harken::FrontierActivationReceipt applied{};
        ASSERT_TRUE(engine.decisions.ReconcileFrontierActivation(future.token[0], selectedEntry,
            engine.mixer.BlocksRendered(), &applied));
        EXPECT_EQ(harken::FrontierReceiptOutcome::Applied, applied.outcome);
        EXPECT_EQ(future.old.anchor / kFrames + 2u, applied.audioSequence);
        ASSERT_TRUE(engine.queueInstances.SettleActivated(future.old.queue, {afterOldEnd.pendingVersion}));
        harken::QueueInstanceSnapshot settled{};
        ASSERT_TRUE(engine.queueInstances.Capture(future.old.queue, &settled));
        EXPECT_EQ(before.logicMirror.version + 1u, settled.logicMirror.version);
        EXPECT_FALSE(settled.resetPending);
        EXPECT_EQ(0u, settled.pendingVersion);
    } else {
        harken::QueueConditionalProof selectedProof{};
        const auto now = engine.mixer.BlocksRendered() * kFrames;
        EXPECT_EQ(harken::QueueConditionalProofStatus::MissedDeadline,
            engine.queueInstances.ProveConditionalForQuantum(future.old.queue, {afterOldEnd.pendingVersion},
                engine.timelineOccurrences, engine.compositeCompletionOwners, now, now + kFrames,
                engine.mixer.BlocksRendered(), false, &selectedProof));
        EXPECT_FALSE(selectedProof.decision.Valid());
        EXPECT_TRUE(engine.queueInstances.ProvePendingConditionalIdentity(
            future.old.queue, {afterOldEnd.pendingVersion}, future.token[1]));
        EXPECT_EQ(harken::ConditionalQueueActivation::Waiting, afterOldEnd.pendingActivation);
        ASSERT_TRUE(engine.queueInstances.RetireConditionalForAbort(future.old.queue,
            {afterOldEnd.pendingVersion}, future.token[1]));
    }
    EXPECT_FALSE(engine.decisions.IsCancelled(future.old.decisions[3]));
    ASSERT_TRUE(state.projection.TryRetireNativeConditional(engine.decisions, engine.mixer.BlocksRendered()));
    action.Abandon();
    EXPECT_EQ(0u, held);
    ASSERT_TRUE(batch.Close());
    ASSERT_TRUE(future.read.Release());
    if (publishSelected) {
        EXPECT_FALSE(selectedCalendar.RetireSelected(engine.mixer.BlocksRendered()));
        EXPECT_TRUE(foreignSelectedLane.Valid());
        foreignSelectedLane.Return();
        ASSERT_TRUE(selectedCalendar.RetireSelected(engine.mixer.BlocksRendered()));
        for (std::uint32_t i = 2; i != 0; --i)
            ASSERT_TRUE(engine.timelinePool.DetachPlan03GroupFrontier(future.group[i - 1], engine.mixer.BlocksRendered()));
        ASSERT_TRUE(engine.decisions.ReleaseFrontierDecisionBinding(future.token[1]));
    }
    future.owner.Release();
    if (publishSelected) {
        ASSERT_TRUE(selectedDomain.CancelGroups(engine.mixer.BlocksRendered(),
            engine.originalQueueGroupHolders.data(), engine.originalQueueGroupHolders.size()));
        ASSERT_TRUE(selectedDomain.Return());
    } else ASSERT_TRUE(engine.CancelOriginalQueueLaneGroups(future.root, engine.mixer.BlocksRendered()));
    for (std::uint32_t i = 0; i < 2; ++i) {
        if (!publishSelected)
            EXPECT_EQ(harken::DecisionInvalidationDisposition::NewlyInvalidated, engine.decisions.Invalidate(future.token[i]));
        ASSERT_TRUE(engine.decisions.Retire(future.token[i], engine.mixer.BlocksRendered()));
        ASSERT_TRUE(engine.decisions.ReclaimRetired(future.token[i], i == 0 ? &future.coordinatorOwner : &future.decisionOwner));
        ASSERT_TRUE(engine.armGroups.BeginRetire(future.armGroup[i]));
        if (installNewSource && i == 1) {
            harken::TimelineArmObservation retired{};
            ASSERT_TRUE(engine.timelineArms.Observe(newSource.source.arm, &retired));
            ASSERT_EQ(harken::ArmState::Tombstoned, retired.state);
            ASSERT_NE(0u, retired.tombstoneAudioSequence);
            ASSERT_LT(retired.tombstoneAudioSequence, engine.mixer.BlocksRendered());
            EXPECT_FALSE(engine.armGroups.Reclaim(future.armGroup[i], engine.mixer.BlocksRendered()));
            ASSERT_TRUE(engine.armGroups.Tombstone(future.armGroup[i], newSource.source.arm,
                retired.tombstoneAudioSequence));
            EXPECT_FALSE(engine.armGroups.Reclaim(future.armGroup[i], retired.tombstoneAudioSequence));
        }
        ASSERT_TRUE(engine.armGroups.Reclaim(future.armGroup[i], engine.mixer.BlocksRendered()));
    }
    if (installNewSource) {
        if (scheduledStop) ASSERT_NO_FATAL_FAILURE(stop.Reclaim(engine));
        engine.ledger.Reap(newSource.source.source);
        ASSERT_TRUE(engine.timelineOccurrences.ClearCompletionOwnerExact(newSource.occurrence, newSource.owner));
        ASSERT_TRUE(engine.timelineOccurrences.UnbindCompletionProgress(newSource.occurrence,
            static_cast<std::uint32_t>(newSource.source.source >> 32)));
        ASSERT_TRUE(engine.timelineOccurrences.Advance(newSource.occurrence, harken::OccurrenceStage::AwaitingEventFence));
        newSource.originalLedger.Release();
        newSource.originalBody.Release();
        newSource.originalSource.Release();
        ASSERT_TRUE(engine.timelineOccurrences.ReleaseAfterFence(newSource.occurrence,
            engine.mixer.BlocksRendered(), &newSource.originalOccurrence));
        ASSERT_TRUE(engine.timelineArms.Reclaim(newSource.source.arm, engine.mixer.BlocksRendered()));
        ASSERT_TRUE(engine.decisions.Cancel(newCompletionDecision));
        ASSERT_TRUE(engine.decisions.ReclaimCancelled(newCompletionDecision, &newSource.originalDecision));
    }
    if (mixedReset) ASSERT_TRUE(engine.queueInstances.Release(ordinaryQueue));
    ASSERT_NO_FATAL_FAILURE(state.Cleanup(false));
}

TEST(OriginalSourceSubmission, AcceptedQueueSelectionBeforeAckKeepsOriginalFirstClockAndCurrent) {
    ASSERT_NO_FATAL_FAILURE(CheckAcceptedQueueSelectionKeepsOriginalClock(false));
}
TEST(OriginalSourceSubmission, AcceptedQueueSelectionAfterPublicationKeepsOriginalFirstClockAndCurrent) {
    ASSERT_NO_FATAL_FAILURE(CheckAcceptedQueueSelectionKeepsOriginalClock(true));
}
TEST(OriginalSourceSubmission, AcceptedQueueSelectionBeforeAckRunsNewNativeC1AndSettlesReset) {
    ASSERT_NO_FATAL_FAILURE(CheckAcceptedQueueSelectionKeepsOriginalClock(false, true));
}
TEST(OriginalSourceSubmission, AcceptedQueueSelectionAfterPublicationRunsNewNativeC1AndSettlesReset) {
    ASSERT_NO_FATAL_FAILURE(CheckAcceptedQueueSelectionKeepsOriginalClock(true, true));
}
TEST(OriginalSourceSubmission, AcceptedQueueSelectionStartsActualNewSourceWithItsOwnCompletionDecision) {
    ASSERT_NO_FATAL_FAILURE(CheckAcceptedQueueSelectionKeepsOriginalClock(true, true, true));
}
TEST(OriginalSourceSubmission, AcceptedQueueSelectionAndPlainStopShareActualSourceHeadAndStopAtTheAuthoredSample) {
    ASSERT_NO_FATAL_FAILURE(CheckAcceptedQueueSelectionKeepsOriginalClock(true, true, true, true));
}
TEST(OriginalSourceSubmission, MixedResetRemainderRequiresActualNativeSelectionBeforeSharedOrderCommit) {
    ASSERT_NO_FATAL_FAILURE(CheckAcceptedQueueSelectionKeepsOriginalClock(true, true, false, false, true));
}
TEST(OriginalSourceSubmission, MixedResetRemainderPreservesOriginalCurrentBeforeSourceAck) {
    ASSERT_NO_FATAL_FAILURE(CheckAcceptedQueueSelectionKeepsOriginalClock(false, true, false, false, true));
}

enum class MatchedReplacementCase { Accepted, IncompleteAttachment, OneOldWrite };

void CheckMatchedConditionalReplacement(MatchedReplacementCase mode) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto& engine = *owned;
    ASSERT_NO_FATAL_FAILURE(RegisterInitialClockPcm(engine));
    GenuineBoundC1ProjectionHarness first(engine), second(engine);
    second.actual.old.cohort = 18;
    second.actual.root = 30;
    const std::array<GenuineBoundC1ProjectionHarness*, 2> states{&first, &second};
    const auto freeArms = engine.timelineArms.FreeCount();
    for (auto* state : states) {
        auto& future = state->actual;
        future.old.sharedEngine = true;
        ASSERT_NO_FATAL_FAILURE(future.old.PrepareBeforeHead());
        ASSERT_NO_FATAL_FAILURE(future.old.ReserveUnboundGroups());
        ASSERT_NO_FATAL_FAILURE(future.ReserveOriginalForAudio(&state->originalLane));
        ASSERT_TRUE(harken::OriginalImmediateLaneConditionalProjection::PrepareUnbound(
            engine.queueInstances, future.old.parent, future.old.original, future.old.clock,
            engine.timelinePool, future.old.Original(2), future.old.Original(3),
            state->originalLane, &state->projection));
        // Each real command reservation is published and acknowledged before
        // preparing the next Source. Neither original Lane is visible yet.
        ASSERT_NO_FATAL_FAILURE(future.old.PublishSourceAndObserveFreshAck());
    }
    for (std::uint32_t i = 0; i < states.size(); ++i) {
        auto& state = *states[i];
        ASSERT_NO_FATAL_FAILURE(state.actual.PlaceOriginalForAudio(&state.originalLane,
            kFrames, 1024u + i * kFrames));
        ASSERT_TRUE(state.actual.old.Bind());
        ASSERT_TRUE(state.projection.BindPrepared());
    }
    harken::QueueInstanceTable::PreparedCatalogResetScope scope;
    harken::QueueResetTargetV1 target{};
    target.world = first.actual.old.episode.decisionRequest.world;
    target.containerStableId = harkentest::ObjectId(13);
    target.programSemanticFingerprint = first.actual.old.fingerprint;
    ASSERT_EQ(harken::QueueResetStatusV1::Ok, engine.queueInstances.PrepareCatalogResetScope(target, &scope));
    ASSERT_EQ(2u, scope.MatchedCount());
    harken::LogicCommitCoordinator::PreparedIssue sequence;
    ASSERT_EQ(harken::LogicCommitCoordinator::IssueStatus::Issued, engine.logicCommits.PrepareIssue(&sequence));
    for (auto* state : states)
        ASSERT_NO_FATAL_FAILURE(state->actual.PrepareCandidate(true, nullptr, &scope, &sequence));
    harken::OriginalConditionalReplacementBatch batch(2);
    ASSERT_TRUE(batch.Prepare(engine.queueInstances, &scope, &sequence));
    ASSERT_TRUE(batch.Attach(first.projection, first.actual.spans, first.actual.owner));
    EXPECT_FALSE(batch.Attach(first.projection, second.actual.spans, second.actual.owner));
    if (mode != MatchedReplacementCase::IncompleteAttachment)
        ASSERT_TRUE(batch.Attach(second.projection, second.actual.spans, second.actual.owner));
    std::uint32_t held = 0;
    harken::ActionCommitBundle action(1);
    const harken::CommitParticipant participant{
        &held,
        [](void* context, std::uint32_t count) noexcept {
            *static_cast<std::uint32_t*>(context) = count; return count == 1;
        },
        [](void* context, std::uint32_t count) noexcept {
            EXPECT_EQ(count, *static_cast<std::uint32_t*>(context));
            *static_cast<std::uint32_t*>(context) = 0;
        },
        [](void*, std::uint32_t, harken::LogicCommitSequence) noexcept { ADD_FAILURE(); }};
    ASSERT_TRUE(action.AddStep(participant, 1));
    ASSERT_TRUE(action.Reserve());
    const bool accepted = mode == MatchedReplacementCase::Accepted;
    if (mode != MatchedReplacementCase::OneOldWrite)
        EXPECT_EQ(accepted, batch.TryAccept(action));
    for (auto* state : states) ASSERT_NO_FATAL_FAILURE(state->actual.PublishOriginalForAudio());
    // At this point A's C1 and B's C0 are due. B's C1 has not executed.
    const auto firstC1 = (first.actual.old.anchor + kFrames) / kFrames + 1u;
    ASSERT_GT(firstC1, engine.mixer.BlocksRendered());
    engine.nullBackend.Pump(static_cast<std::uint32_t>(firstC1 - engine.mixer.BlocksRendered()));
    if (mode == MatchedReplacementCase::OneOldWrite) {
        harken::QueueInstanceSnapshot a{}, b{};
        ASSERT_TRUE(engine.queueInstances.Capture(first.actual.old.queue, &a));
        ASSERT_TRUE(engine.queueInstances.Capture(second.actual.old.queue, &b));
        EXPECT_EQ(harken::ConditionalQueueActivation::Activated, a.pendingActivation);
        EXPECT_EQ(harken::ConditionalQueueActivation::Waiting, b.pendingActivation);
        EXPECT_FALSE(batch.TryAccept(action));
    }
    EXPECT_EQ(accepted, batch.Accepted());
    const auto lastC1 = (second.actual.old.anchor + kFrames) / kFrames + 1u;
    engine.nullBackend.Pump(static_cast<std::uint32_t>(lastC1 - engine.mixer.BlocksRendered()));
    for (auto* state : states) {
        auto& old = state->actual.old;
        harken::QueueInstanceSnapshot queue{};
        ASSERT_TRUE(engine.queueInstances.Capture(old.queue, &queue));
        EXPECT_EQ(accepted ? harken::ConditionalQueueActivation::Waiting : harken::ConditionalQueueActivation::Activated,
            queue.pendingActivation);
        harken::CompletionOwnerRef owner{};
        ASSERT_TRUE(engine.timelineOccurrences.CaptureCompletionOwner(old.episode.occurrence, &owner));
        EXPECT_TRUE(harken::SameCompletionOwner(old.episode.owner, owner));
        harken::CompletionProgressSnapshot progress{};
        ASSERT_TRUE(harken::CaptureCompletionProgressExact(*old.episode.progress, old.episode.occurrence,
            static_cast<std::uint32_t>(old.episode.source.source >> 32), &progress));
        EXPECT_EQ(kFrames, progress.logicalOutputFramesProduced);
        EXPECT_EQ(old.anchor / kFrames + 1u, progress.completedThroughBlockSequence);
        EXPECT_TRUE(engine.voices.ReleaseObserved(old.episode.source.release));
        const auto* c0 = engine.decisions.ResolveFrontierActivation(old.decisions[0]);
        ASSERT_NE(nullptr, c0);
        harken::FrontierActivationReceipt receipt{};
        ASSERT_EQ(harken::FrontierReceiptRead::Ready, c0->Read(c0->Descriptor().entry, lastC1, &receipt));
        EXPECT_EQ(harken::FrontierReceiptOutcome::Applied, receipt.outcome);
    }
    action.Abandon();
    EXPECT_EQ(0u, held);
    ASSERT_TRUE(batch.Close());
    for (auto* state : states) ASSERT_NO_FATAL_FAILURE(state->actual.CancelCandidate());
    engine.nullBackend.Pump(1);
    if (accepted) for (auto* state : states)
        ASSERT_TRUE(state->projection.TryRetireNativeConditional(engine.decisions, engine.mixer.BlocksRendered()));
    for (auto* state : states) ASSERT_NO_FATAL_FAILURE(state->Cleanup(!accepted));
    EXPECT_EQ(freeArms, engine.timelineArms.FreeCount());
    EXPECT_EQ(0u, harken::detail::VoicePoolReleaseTestAccess::OriginalOwnerCount(engine.voices));
    EXPECT_EQ(0u, engine.decisions.LiveCount());
    EXPECT_EQ(0u, engine.armGroups.LiveCount());
    ASSERT_EQ(VN_OK, engine.registry.UnregisterPCM(kSound));
}

TEST(OriginalSourceSubmission, ReplacementBatchSelectsEveryActualMatchedQueueWithOneWinner) {
    ASSERT_NO_FATAL_FAILURE(CheckMatchedConditionalReplacement(MatchedReplacementCase::Accepted));
}
TEST(OriginalSourceSubmission, ReplacementBatchIncompleteMatchingLeavesBothActualFuturesLive) {
    ASSERT_NO_FATAL_FAILURE(CheckMatchedConditionalReplacement(MatchedReplacementCase::IncompleteAttachment));
}
TEST(OriginalSourceSubmission, ReplacementBatchOneActualOldWriteRefusesTheEntireMatchedSet) {
    ASSERT_NO_FATAL_FAILURE(CheckMatchedConditionalReplacement(MatchedReplacementCase::OneOldWrite));
}

void CheckBoundC1RequestPreservesActualCurrent(std::uint32_t frames, bool prepareUnbound = false,
    bool beforePlacement = false) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    GenuineBoundC1ProjectionHarness state(*owned);
    ASSERT_NO_FATAL_FAILURE(state.Prepare(frames, true, prepareUnbound, beforePlacement));
    ASSERT_NO_FATAL_FAILURE(state.actual.PublishOriginalForAudio());
    ASSERT_TRUE(state.projection.RequestWithdrawal());
    ASSERT_NO_FATAL_FAILURE(state.PumpCurrent());
    ASSERT_NO_FATAL_FAILURE(state.PumpC1IfNextBucket());
    const auto equal = owned->mixer.BlocksRendered();
    EXPECT_FALSE(state.projection.TryRetireNativeConditional(owned->decisions, equal));
    harken::QueueInstanceSnapshot before{}, after{};
    ASSERT_TRUE(owned->queueInstances.Capture(state.actual.old.queue, &before));
    EXPECT_EQ(state.actual.old.version.version, before.pendingVersion);
    EXPECT_EQ(harken::ConditionalQueueActivation::Waiting, before.pendingActivation);
    const auto* c0 = owned->decisions.ResolveFrontierActivation(state.actual.old.decisions[0]);
    ASSERT_NE(nullptr, c0);
    const auto c0Entry = c0->Descriptor().entry;
    owned->nullBackend.Pump(1);
    ASSERT_EQ(equal + 1u, owned->mixer.BlocksRendered());
    ASSERT_TRUE(state.projection.TryRetireNativeConditional(owned->decisions, owned->mixer.BlocksRendered()));
    ASSERT_TRUE(state.projection.TryRetireNativeConditional(owned->decisions, owned->mixer.BlocksRendered()));
    const auto* c1 = owned->decisions.ResolveFrontierActivation(state.actual.old.decisions[2]);
    ASSERT_NE(nullptr, c1);
    harken::FrontierActivationReceipt cancelled{};
    ASSERT_EQ(harken::FrontierReceiptRead::Ready,
        c1->Read(c1->Descriptor().entry, owned->mixer.BlocksRendered(), &cancelled));
    EXPECT_EQ(harken::FrontierReceiptOutcome::DefinitivelyNotApplied, cancelled.outcome);
    EXPECT_EQ(harken::FrontierReceiptReason::Cancelled, cancelled.reason);
    EXPECT_EQ(harken::FrontierReceiptOrigin::LogicBeforeAudio, cancelled.origin);
    ASSERT_TRUE(owned->queueInstances.Capture(state.actual.old.queue, &after));
    EXPECT_EQ(before.logicMirror.version, after.logicMirror.version);
    EXPECT_EQ(before.initialActivationSequence, after.initialActivationSequence);
    EXPECT_EQ(0u, after.pendingVersion);
    harken::FrontierActivationReceipt current{};
    ASSERT_EQ(harken::FrontierReceiptRead::Ready,
        c0->Read(c0Entry, owned->mixer.BlocksRendered(), &current));
    EXPECT_EQ(harken::FrontierReceiptOutcome::Applied, current.outcome);
    EXPECT_TRUE(owned->decisions.Contains(state.actual.old.decisions[3]));
    EXPECT_FALSE(owned->decisions.IsCancelled(state.actual.old.decisions[3]));
    ASSERT_NO_FATAL_FAILURE(state.Cleanup(false));
}

TEST(OriginalSourceSubmission, BoundC1WithdrawalPreservesCrossBucketOriginalCurrentAndStrictFence) {
    ASSERT_NO_FATAL_FAILURE(CheckBoundC1RequestPreservesActualCurrent(kFrames));
}
TEST(OriginalSourceSubmission, BoundC1WithdrawalPreservesSameBucketC0SourceAndActualCompletion) {
    ASSERT_NO_FATAL_FAILURE(CheckBoundC1RequestPreservesActualCurrent(17));
}
TEST(OriginalSourceSubmission, UnboundC1WithdrawalBindsOnlyAfterOriginalClockAndPreservesCurrent) {
    ASSERT_NO_FATAL_FAILURE(CheckBoundC1RequestPreservesActualCurrent(kFrames, true));
}
TEST(OriginalSourceSubmission, UnboundC1WithdrawalPreservesSameBucketOriginalSource) {
    ASSERT_NO_FATAL_FAILURE(CheckBoundC1RequestPreservesActualCurrent(17, true));
}
TEST(OriginalSourceSubmission, UnplacedC1WithdrawalBeforeSourceAckPreservesOriginalCurrent) {
    ASSERT_NO_FATAL_FAILURE(CheckBoundC1RequestPreservesActualCurrent(kFrames, true, true));
}
TEST(OriginalSourceSubmission, UnplacedC1WithdrawalBeforeSourceAckPreservesSameBucketCurrent) {
    ASSERT_NO_FATAL_FAILURE(CheckBoundC1RequestPreservesActualCurrent(17, true, true));
}
void CheckAbandonedUnboundC1Withdrawal(bool beforePlacement) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    GenuineBoundC1ProjectionHarness state(*owned);
    ASSERT_NO_FATAL_FAILURE(RegisterInitialClockPcm(*owned, kFrames));
    ASSERT_NO_FATAL_FAILURE(state.actual.old.PrepareBeforeHead(kFrames));
    ASSERT_NO_FATAL_FAILURE(state.actual.old.ReserveUnboundGroups(kFrames));
    if (beforePlacement) {
        ASSERT_NO_FATAL_FAILURE(state.actual.ReserveOriginalForAudio(&state.originalLane));
    } else {
        ASSERT_NO_FATAL_FAILURE(state.actual.old.PublishSourceAndObserveFreshAck());
        ASSERT_NO_FATAL_FAILURE(state.actual.PlaceOriginalForAudio(&state.originalLane, kFrames));
    }
    ASSERT_TRUE(harken::OriginalImmediateLaneConditionalProjection::PrepareUnbound(
        owned->queueInstances, state.actual.old.parent, state.actual.old.original, state.actual.old.clock,
        owned->timelinePool, state.actual.old.Original(2), state.actual.old.Original(3),
        state.originalLane, &state.projection));
    ASSERT_TRUE(state.projection.RequestWithdrawal());
    state.projection.Return();
    EXPECT_FALSE(state.projection.Valid());
    if (beforePlacement) {
        ASSERT_NO_FATAL_FAILURE(state.actual.old.PublishSourceAndObserveFreshAck());
        ASSERT_NO_FATAL_FAILURE(state.actual.PlaceOriginalForAudio(&state.originalLane, kFrames));
    }
    ASSERT_TRUE(state.actual.old.Bind());
    ASSERT_NO_FATAL_FAILURE(state.actual.PublishOriginalForAudio());
    ASSERT_NO_FATAL_FAILURE(state.PumpCurrent());
    ASSERT_NO_FATAL_FAILURE(state.PumpC1IfNextBucket());
    owned->nullBackend.Pump(1);
    ASSERT_NO_FATAL_FAILURE(state.Cleanup(true));
}
TEST(OriginalSourceSubmission, AbandonedUnboundC1WithdrawalRestoresOriginalPublication) {
    ASSERT_NO_FATAL_FAILURE(CheckAbandonedUnboundC1Withdrawal(false));
}
TEST(OriginalSourceSubmission, AbandonedUnplacedC1WithdrawalRestoresOriginalPublication) {
    ASSERT_NO_FATAL_FAILURE(CheckAbandonedUnboundC1Withdrawal(true));
}
TEST(OriginalSourceSubmission, AppliedC1ReturnsOnlyOwnProjectionAfterLaterCompletedCallback) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    GenuineBoundC1ProjectionHarness state(*owned);
    ASSERT_NO_FATAL_FAILURE(state.Prepare());
    ASSERT_NO_FATAL_FAILURE(state.actual.PublishOriginalForAudio());
    ASSERT_NO_FATAL_FAILURE(state.PumpCurrent());
    EXPECT_FALSE(state.projection.ReturnAfterApplied(owned->immediateActionLane,
        state.actual.old.lane, owned->decisions, owned->mixer.BlocksRendered()));
    ASSERT_NO_FATAL_FAILURE(state.PumpC1IfNextBucket());
    const auto applied = owned->mixer.BlocksRendered();
    EXPECT_FALSE(state.projection.ReturnAfterApplied(owned->immediateActionLane,
        state.actual.old.lane, owned->decisions, applied));
    harken::OriginalImmediateLaneConditionalProjection foreign;
    ASSERT_TRUE(state.projection.Borrow(&foreign));
    owned->nullBackend.Pump(1);
    ASSERT_EQ(applied + 1u, owned->mixer.BlocksRendered());
    EXPECT_FALSE(foreign.ReturnAfterApplied(owned->immediateActionLane,
        state.actual.old.lane, owned->decisions, owned->mixer.BlocksRendered()));
    ASSERT_TRUE(state.projection.ReturnAfterApplied(owned->immediateActionLane,
        state.actual.old.lane, owned->decisions, owned->mixer.BlocksRendered()));
    EXPECT_FALSE(state.projection.Valid());
    EXPECT_TRUE(foreign.Valid());
    foreign.Return();
    ASSERT_NO_FATAL_FAILURE(state.Cleanup(true));
}
TEST(OriginalSourceSubmission, BoundC1ActualCaptureWinsLateRequestWithoutCancelledOrForgedEnd) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    GenuineBoundC1ProjectionHarness state(*owned);
    ASSERT_NO_FATAL_FAILURE(state.Prepare());
    ASSERT_NO_FATAL_FAILURE(state.actual.PublishOriginalForAudio());
    ASSERT_NO_FATAL_FAILURE(state.PumpCurrent());
    owned->immediateActionLane.SetEventAdmissionHookForTest(&GenuineBoundC1ProjectionHarness::LateRequest, &state);
    ASSERT_NO_FATAL_FAILURE(state.PumpC1IfNextBucket());
    owned->immediateActionLane.SetEventAdmissionHookForTest(nullptr, nullptr);
    ASSERT_TRUE(state.lateRequestCalled);
    owned->nullBackend.Pump(1);
    EXPECT_FALSE(state.projection.TryRetireNativeConditional(owned->decisions, owned->mixer.BlocksRendered()));
    const auto* c1 = owned->decisions.ResolveFrontierActivation(state.actual.old.decisions[2]);
    ASSERT_NE(nullptr, c1);
    harken::FrontierActivationReceipt receipt{};
    ASSERT_EQ(harken::FrontierReceiptRead::Ready,
        c1->Read(c1->Descriptor().entry, owned->mixer.BlocksRendered(), &receipt));
    EXPECT_EQ(harken::FrontierReceiptOutcome::Applied, receipt.outcome);
    ASSERT_NO_FATAL_FAILURE(state.Cleanup(true));
}
TEST(OriginalSourceSubmission, BoundC1FactoryRejectsGenuineForeignLaneAndCurrentPrimaryWithoutMinting) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto foreign = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    GenuineBoundC1ProjectionHarness state(*owned);
    ASSERT_NO_FATAL_FAILURE(state.Prepare(kFrames, false));
    harken::ImmediateLaneHandle handle{};
    harken::OriginalImmediateLaneAttemptLifetimePin other;
    ASSERT_TRUE(foreign->immediateActionLane.Reserve(state.actual.old.cohort, {3, 4, 0, 2}, &handle, &other));
    EXPECT_EQ(state.actual.old.lane.slot, handle.slot);
    EXPECT_EQ(state.actual.old.lane.generation, handle.generation);
    harken::OriginalImmediateLaneConditionalProjection refused;
    EXPECT_FALSE(harken::OriginalImmediateLaneConditionalProjection::PrepareBound(
        owned->queueInstances, state.actual.old.parent, state.actual.old.original, state.actual.old.clock,
        owned->timelinePool, state.actual.old.Original(2), state.actual.old.Original(3), other, &refused));
    EXPECT_FALSE(refused.Valid());
    EXPECT_FALSE(harken::OriginalImmediateLaneConditionalProjection::PrepareBound(
        owned->queueInstances, state.actual.old.parent, state.actual.old.original, state.actual.old.clock,
        owned->timelinePool, state.actual.old.Original(0), state.actual.old.Original(3), state.originalLane, &refused));
    EXPECT_FALSE(refused.Valid());
    ASSERT_TRUE(harken::OriginalImmediateLaneConditionalProjection::PrepareBound(
        owned->queueInstances, state.actual.old.parent, state.actual.old.original, state.actual.old.clock,
        owned->timelinePool, state.actual.old.Original(2), state.actual.old.Original(3),
        state.originalLane, &state.projection));
    ASSERT_TRUE(foreign->immediateActionLane.Cancel(handle));
    ASSERT_TRUE(foreign->immediateActionLane.Retire(handle, 0, &other));
    ASSERT_NO_FATAL_FAILURE(state.actual.PublishOriginalForAudio());
    ASSERT_TRUE(state.projection.RequestWithdrawal());
    ASSERT_NO_FATAL_FAILURE(state.PumpCurrent());
    ASSERT_NO_FATAL_FAILURE(state.PumpC1IfNextBucket());
    owned->nullBackend.Pump(1);
    ASSERT_TRUE(state.projection.TryRetireNativeConditional(owned->decisions, owned->mixer.BlocksRendered()));
    ASSERT_NO_FATAL_FAILURE(state.Cleanup(false));
}

TEST(OriginalSourceSubmission, BoundC1ForeignProjectionLoanPreservesNativeAttemptUntilActualReturn) {
    static_assert(!std::is_copy_constructible_v<harken::OriginalImmediateLaneConditionalProjection>);
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    GenuineBoundC1ProjectionHarness state(*owned);
    ASSERT_NO_FATAL_FAILURE(state.Prepare());
    harken::OriginalImmediateLaneConditionalProjection borrowed;
    ASSERT_TRUE(state.projection.Borrow(&borrowed));
    harken::OriginalImmediateLaneConditionalProjection foreign(std::move(borrowed));
    EXPECT_FALSE(borrowed.Valid());
    ASSERT_TRUE(foreign.Valid());
    EXPECT_FALSE(owned->immediateActionLane.ObserveOriginalUnpublishedStorageOnlyRetention(
        state.originalLane, state.actual.old.lane, state.actual.old.cohort, owned->mixer.BlocksRendered()));
    EXPECT_FALSE(foreign.RequestWithdrawal());
    EXPECT_FALSE(foreign.TryRetireNativeConditional(owned->decisions, owned->mixer.BlocksRendered()));
    ASSERT_NO_FATAL_FAILURE(state.actual.PublishOriginalForAudio());
    ASSERT_TRUE(state.projection.RequestWithdrawal());
    ASSERT_NO_FATAL_FAILURE(state.PumpCurrent());
    ASSERT_NO_FATAL_FAILURE(state.PumpC1IfNextBucket());
    owned->nullBackend.Pump(1);
    ASSERT_TRUE(state.projection.TryRetireNativeConditional(owned->decisions, owned->mixer.BlocksRendered()));
    EXPECT_TRUE(foreign.Valid());
    ASSERT_NO_FATAL_FAILURE(state.Cleanup(false, &foreign));
    EXPECT_FALSE(foreign.Valid());
}


namespace {
// Genuine factory integration for the bounded C0 geometry observation only.
// It deliberately never publishes the Lane or issues an enduring Current cap.
struct GenuineScopedC0SourceFixture : GenuineInitialSingleClockHarness {
    harken::OriginalQueueFutureGroupLifetimePin primaryOriginal, initialOriginal;
    harken::PlayableInstanceHandle independentAdmissionRoot = 0;
    bool oldConditionalEnded = false;
    explicit GenuineScopedC0SourceFixture(harken::Engine& receiver)
        : GenuineInitialSingleClockHarness(receiver) {}
    void PlaceScopedC0(std::uint64_t sourceRelative = 0, bool independentAdmission = false) {
        ASSERT_TRUE(engine.immediateActionLane.Reserve(cohort, {1, 4, 0, 2}, &lane));
        for (std::uint32_t index = 0; index < groups.size(); ++index) {
            ASSERT_TRUE(engine.timelinePool.BindImmediateLaneGroupOwner(groups[index],
                engine.immediateActionLane, cohort, lane.slot, lane.generation));
            ASSERT_TRUE(engine.immediateActionLane.DeclareGroup(lane,
                {groups[index], inputs[index].executeFrame - provisionalAnchor}));
        }
        const auto root = engine.playableInstances.Classify(episode.parent);
        ASSERT_TRUE(root.live);
        ASSERT_EQ(harken::PlayableLifecycle::Active, root.lifecycle);
        harken::FrontierOwnerIdentity owner{};
        owner.child = episode.parent;
        owner.childEpoch = root.scheduleEpoch;
        owner.invocation = episode.parent;
        owner.invocationEpoch = root.scheduleEpoch;
        owner.serviceSlot = 0;
        owner.serviceGeneration = 1;
        auto queueOwner = owner;
        if (independentAdmission) {
            independentAdmissionRoot = engine.playableInstances.Acquire(harken::PlayableKind::Sound, 0);
            ASSERT_NE(0u, independentAdmissionRoot);
            const auto admission = engine.playableInstances.Classify(independentAdmissionRoot);
            ASSERT_TRUE(admission.live);
            ASSERT_EQ(harken::PlayableLifecycle::Active, admission.lifecycle);
            ASSERT_NE(episode.parent, independentAdmissionRoot);
            owner.child = owner.invocation = independentAdmissionRoot;
            owner.childEpoch = owner.invocationEpoch = admission.scheduleEpoch;
            queueOwner.invocation = independentAdmissionRoot;
            queueOwner.invocationEpoch = admission.scheduleEpoch;
            queueOwner.serviceSlot = 1;
        }
        payload.kind = harken::TimelineEventKind::Start;
        payload.target.subjectId = kSound;
        program = {17, &payload, 1, harken::TimelineProgramRole::Voice};
        start.relativeFrame = sourceRelative;
        // This is the actual pending Source factory's original generation (17
        // in GenuinePreparedSingleEpisode), not a Start/ACK or minted owner.
        start.armProgramGeneration = 17;
        start.event.program = {17, &program};
        start.event.armToken = episode.source.arm;
        start.event.subeventPhase = 20;
        start.event.logicCommitSequence = logicSequence;
        start.event.ownerInstanceHandle = episode.parent;
        start.event.ownerInstanceScheduleEpoch = root.scheduleEpoch;
        start.event.frontier.kind = harken::FrontierOperationKind::Entry;
        start.event.frontier.entry = {decisions[0], 1, owner};
        start.event.frontier.owner = owner;
        start.event.frontier.group = groups[0];
        start.event.frontier.transport = engine.timelinePool.Epoch();
        start.event.frontier.world = episode.decisionRequest.world;
        start.event.frontier.operationEventCount = 1;
        start.event.frontier.entryEventOrdinal = 0;
        ASSERT_TRUE(engine.immediateActionLane.Prepare(lane, &start, 1));
        ASSERT_TRUE(engine.immediateActionLane.TryPlace(lane, 1024, 0, &anchor, &placement));
        ASSERT_EQ(1024u, anchor);
        ASSERT_TRUE(engine.timelinePool.RebaseImmediateLaneGroups(cohort, anchor));
        for (std::uint32_t index = 0; index < groups.size(); ++index) {
            const auto primary = index < 2 ? 0u : 2u;
            harken::FrontierGroupBinding binding{};
            binding.role = index == primary ? harken::FrontierGroupRole::Coordinator :
                harken::FrontierGroupRole::EntryMember;
            const auto& entryOwner = index < 2u ? owner : queueOwner;
            binding.entry = {decisions[primary], primary + 1u, entryOwner};
            binding.operation.kind = index == 3 ? harken::FrontierOperationKind::Dependent :
                harken::FrontierOperationKind::Entry;
            binding.operation.owner = index == 0u ? owner : queueOwner;
            binding.operation.order.logicCommitSequence = logicSequence;
            binding.operation.order.executeFrame = anchor + inputs[index].executeFrame - provisionalAnchor;
            binding.operation.group = groups[index];
            binding.operation.transport = engine.timelinePool.Epoch();
            binding.operation.world = episode.decisionRequest.world;
            binding.primary = groups[primary];
            if (index == 1) binding.queueInitial = {queue.slot, queue.generation, 1, fingerprint};
            if (index == 3) binding.queueConditional = {queue.slot, queue.generation,
                1, version.version, episode.occurrence, episode.occurrence};
            ASSERT_TRUE(engine.timelinePool.BindPlan03GroupFrontier(groups[index], binding));
            harken::Plan03PageGroupView actual{};
            ASSERT_TRUE(engine.timelinePool.ResolvePlan03GroupSliceProducer(groups[index], &actual));
            ASSERT_NE(nullptr, actual.frontier);
            ASSERT_TRUE(engine.armGroups.Reserve(decisions[index], nullptr, 0, &armGroups[index]));
            harken::DecisionParticipantRef participant{};
            participant.kind = harken::DecisionParticipantKind::QueueInstance;
            participant.slot = queue.slot;
            participant.generation = queue.generation;
            harken::DecisionCommitPayload commit{};
            commit.pages = {static_cast<std::uint32_t>(groups[index].committedRow), groups[index].rowClaimGeneration};
            commit.armGroup = armGroups[index];
            if (index == 3 || index == 1) {
                commit.participants = &participant;
                commit.participantCount = 1;
            }
            ASSERT_TRUE(engine.decisions.Commit(decisions[index], commit));
            if (index == 1 || index == 3)
                ASSERT_TRUE(engine.decisions.BindFrontierDecision(decisions[index],
                    *actual.frontier, engine.timelinePool));
            if (index == 1)
                ASSERT_TRUE(engine.queueInstances.BindFrontierInitial(queue, *actual.frontier, engine.timelinePool));
            if (index == 3)
                ASSERT_TRUE(engine.queueInstances.BindFrontierConditional(queue, *actual.frontier, engine.timelinePool));
        }
        ASSERT_TRUE(engine.timelineArms.BindPendingImmediateStart(episode.source.arm, anchor + sourceRelative, anchor + sourceRelative));
        ASSERT_TRUE(engine.immediateActionLane.PinArms(lane, engine.timelineArms));
        ASSERT_TRUE(engine.immediateActionLane.PinGroups(lane, engine.timelinePool,
            engine.decisions, engine.config.maxPlan03GroupHeadersPerPage));
        harken::Plan03PageGroupView initial{};
        ASSERT_TRUE(engine.timelinePool.ResolvePlan03GroupSliceProducer(groups[0], &initial));
        ASSERT_TRUE(engine.timelineArms.BindFrontierAdmission(episode.source.arm, *initial.frontier, *initial.lease));
        ASSERT_TRUE(Original(0).BorrowLifetime(&primaryOriginal));
        ASSERT_TRUE(Original(1).BorrowLifetime(&initialOriginal));
        ASSERT_TRUE(engine.timelinePool.ProvePrivateOriginalQueueGroupClock(Original(0), Original(1),
            &placement, &privateClock));
        ASSERT_FALSE(placement.Valid());
        ASSERT_TRUE(privateClock.Valid());
    }
    bool Capture(std::array<std::uint64_t, 3>* frames,
        const harken::FrontierGroupBinding** primary,
        const harken::FrontierGroupBinding** initial) const noexcept {
        return engine.timelinePool.CapturePrivateOriginalQueueC0Source(privateClock,
            primaryOriginal, initialOriginal, engine.timelineArms, engine.decisions,
            episode.source.arm, frames, primary, initial);
    }
    void ReacquireOriginalPlacement() {
        ASSERT_FALSE(privateClock.Valid());
        ASSERT_FALSE(placement.Valid());
        std::uint64_t chosen = UINT64_MAX;
        ASSERT_TRUE(engine.immediateActionLane.TryPlace(lane, anchor, 0, &chosen, &placement));
        ASSERT_EQ(anchor, chosen);
        ASSERT_TRUE(placement.Valid());
    }
    void EndScopedAndRetireSource() {
        privateClock.Release();
        primaryOriginal.Release();
        initialOriginal.Release();
        placement.Release();
        if (!oldConditionalEnded)
            ASSERT_TRUE(engine.queueInstances.RetireConditionalForAbort(queue, version, decisions[3]));
        else {
            harken::QueueInstanceSnapshot ended{};
            ASSERT_TRUE(engine.queueInstances.Capture(queue, &ended));
            ASSERT_EQ(0u, ended.pendingVersion);
        }
        BoundClockState state = BoundClockState::Unbound;
        ASSERT_TRUE(clock.CaptureState(&state));
        ASSERT_EQ(BoundClockState::Retired, state);
        ASSERT_TRUE(engine.timelineArms.RequestCancel(episode.source.arm));
        harken::CommandRecord stop{};
        stop.opcode = harken::kOpcodeStop;
        stop.payload.stop.handle = episode.source.source;
        ASSERT_TRUE(engine.commands.TryPush(stop));
        const auto before = engine.mixer.BlocksRendered();
        engine.nullBackend.Pump(1);
        ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
        ASSERT_TRUE(engine.voices.ReleaseObserved(episode.source.release));
        EXPECT_EQ(nullptr, engine.voices.Resolve(episode.source.source));
        EXPECT_FALSE(episode.bundle.ObserveInstalled());
        ASSERT_TRUE(engine.immediateActionLane.Cancel(lane));
        ASSERT_TRUE(engine.immediateActionLane.Retire(lane, engine.mixer.BlocksRendered()));
        for (std::uint32_t index = groups.size(); index != 0; --index)
            ASSERT_TRUE(engine.timelinePool.DetachPlan03GroupFrontier(groups[index - 1],
                engine.mixer.BlocksRendered()));
        ASSERT_TRUE(engine.decisions.ReleaseFrontierDecisionBinding(decisions[1]));
        ASSERT_TRUE(engine.decisions.ReleaseFrontierDecisionBinding(decisions[3]));
        engine.ledger.Reap(episode.source.source);
        ASSERT_TRUE(engine.timelineOccurrences.ClearCompletionOwnerExact(episode.occurrence, episode.owner));
        harken::CompletionProgressSnapshot terminal{};
        ASSERT_TRUE(harken::CaptureCompletionProgressExact(*episode.progress, episode.occurrence,
            static_cast<std::uint32_t>(episode.source.source >> 32), &terminal));
        terminalSequence = terminal.completedThroughBlockSequence;
        ASSERT_TRUE(engine.timelineOccurrences.UnbindCompletionProgress(episode.occurrence,
            static_cast<std::uint32_t>(episode.source.source >> 32)));
        ASSERT_TRUE(engine.timelineOccurrences.Advance(episode.occurrence, harken::OccurrenceStage::AwaitingEventFence));
        if (independentAdmissionRoot != 0) {
            ASSERT_TRUE(engine.playableInstances.AdvanceLifecycle(independentAdmissionRoot, harken::PlayableLifecycle::Retired));
            independentAdmissionRoot = 0;
        }
    }
    void Finish() {
        ASSERT_NO_FATAL_FAILURE(EndScopedAndRetireSource());
        ASSERT_NO_FATAL_FAILURE(ReturnAllAndReclaim());
        EXPECT_EQ(0u, engine.voices.ActiveCount());
        EXPECT_EQ(nullptr, engine.voices.Resolve(episode.source.source));
        harken::TimelineArmObservation stale{};
        EXPECT_FALSE(engine.timelineArms.Observe(episode.source.arm, &stale));
    }
};
void PrepareGenuineScopedC0Source(GenuineScopedC0SourceFixture& state,
    std::uint64_t relative = 0, bool independentAdmission = false) {
    ASSERT_NO_FATAL_FAILURE(RegisterInitialClockPcm(state.engine));
    ASSERT_NO_FATAL_FAILURE(state.PrepareBeforeHead());
    ASSERT_NO_FATAL_FAILURE(state.ReserveUnboundGroups());
    ASSERT_NO_FATAL_FAILURE(state.PublishSourceAndObserveFreshAck());
    ASSERT_NO_FATAL_FAILURE(state.PlaceScopedC0(relative, independentAdmission));
}
void ExpectScopedC0SentinelUnchanged(const std::array<std::uint64_t, 3>& frames,
    const harken::FrontierGroupBinding* primary,
    const harken::FrontierGroupBinding* initial) {
    EXPECT_EQ((std::array<std::uint64_t, 3>{71, 73, 79}), frames);
    EXPECT_EQ(nullptr, primary);
    EXPECT_EQ(nullptr, initial);
}
} // namespace

TEST(OriginalSourceSubmission, OriginalC0ScopedGeometryUsesFreshNativeAckAndHonestExclusiveShortProof) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    GenuineScopedC0SourceFixture state(*owned);
    ASSERT_NO_FATAL_FAILURE(PrepareGenuineScopedC0Source(state));
    std::array<std::uint64_t, 3> frames{};
    const harken::FrontierGroupBinding* primary = nullptr;
    const harken::FrontierGroupBinding* initial = nullptr;
    ASSERT_TRUE(state.Capture(&frames, &primary, &initial));
    EXPECT_EQ(state.anchor, frames[0]);
    EXPECT_EQ(state.anchor, frames[1]);
    EXPECT_EQ(state.anchor, frames[2]);
    ASSERT_NE(nullptr, primary);
    ASSERT_NE(nullptr, initial);
    EXPECT_EQ(harken::FrontierGroupRole::Coordinator, primary->role);
    EXPECT_EQ(harken::FrontierGroupRole::EntryMember, initial->role);
    EXPECT_EQ(state.groups[0], primary->operation.group);
    EXPECT_EQ(state.groups[1], initial->operation.group);
    harken::TimelineArmObservation actual{};
    ASSERT_TRUE(owned->timelineArms.Observe(state.episode.source.arm, &actual));
    EXPECT_EQ(state.installed, actual.armedCompletionSequence);
    EXPECT_GT(actual.armedCompletionSequence, 0u);
    EXPECT_EQ(0u, actual.startedBlockSequence);
    std::uint64_t refusedAnchor = 83;
    harken::PrivateImmediateLanePlacementProof refusedPlacement;
    EXPECT_FALSE(owned->immediateActionLane.TryPlace(state.lane, state.anchor, 0,
        &refusedAnchor, &refusedPlacement));
    EXPECT_EQ(83u, refusedAnchor);
    EXPECT_TRUE(state.privateClock.Valid());
    EXPECT_FALSE(refusedPlacement.Valid());
    ASSERT_TRUE(state.privateClock.ReturnPlacement(&state.placement));
    EXPECT_FALSE(state.privateClock.Valid());
    ASSERT_TRUE(state.placement.Valid());
    ASSERT_TRUE(owned->timelinePool.ProvePrivateOriginalQueueGroupClock(state.Original(1),
        state.Original(3), &state.placement, &state.privateClock));
    harken::OriginalQueueFutureGroupLifetimePin actualC1;
    ASSERT_TRUE(state.Original(3).BorrowLifetime(&actualC1));
    std::array<std::uint64_t, 4> jointFrames{83, 89, 97, 101};
    const harken::FrontierGroupBinding* jointInitial = nullptr;
    const harken::FrontierGroupBinding* jointConditional = nullptr;
    ASSERT_TRUE(owned->timelinePool.CapturePrivateOriginalQueueClockInitialSource(
        state.privateClock, state.initialOriginal, actualC1, owned->timelineArms,
        owned->decisions, state.episode.source.arm, &jointFrames,
        &jointInitial, &jointConditional));
    EXPECT_EQ((std::array<std::uint64_t, 4>{state.anchor, state.anchor,
        state.anchor + kFrames, state.anchor}), jointFrames);
    EXPECT_EQ(state.groups[1], jointInitial->operation.group);
    EXPECT_EQ(state.groups[3], jointConditional->operation.group);
    frames = {71, 73, 79};
    primary = initial = nullptr;
    EXPECT_FALSE(state.Capture(&frames, &primary, &initial));
    ASSERT_NO_FATAL_FAILURE(ExpectScopedC0SentinelUnchanged(frames, primary, initial));
    EXPECT_TRUE(state.privateClock.Valid());
    actualC1.Release();
    ASSERT_TRUE(state.privateClock.ReturnPlacement(&state.placement));
    EXPECT_FALSE(state.privateClock.Valid());
    ASSERT_TRUE(state.placement.Valid());
    ASSERT_TRUE(owned->timelinePool.ProvePrivateOriginalQueueGroupClock(state.Original(0),
        state.Original(1), &state.placement, &state.privateClock));
    ASSERT_TRUE(state.Capture(&frames, &primary, &initial));
    ASSERT_NO_FATAL_FAILURE(state.Finish());
}

TEST(OriginalSourceSubmission, OriginalC0ScopedGeometrySurvivesC1CoordinatorInvalidationWithoutEndingOriginalConditional) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    GenuineScopedC0SourceFixture state(*owned);
    ASSERT_NO_FATAL_FAILURE(PrepareGenuineScopedC0Source(state));
    const auto invalidated = owned->decisions.Invalidate(state.decisions[2]);
    ASSERT_EQ(harken::DecisionInvalidationDisposition::NewlyInvalidated, invalidated);
    BoundClockState clockState = BoundClockState::Retired;
    ASSERT_TRUE(state.clock.CaptureState(&clockState));
    ASSERT_EQ(BoundClockState::Unbound, clockState);
    harken::QueueInstanceSnapshot queue{};
    ASSERT_TRUE(owned->queueInstances.Capture(state.queue, &queue));
    ASSERT_EQ(state.version.version, queue.pendingVersion);
    ASSERT_EQ(harken::ConditionalQueueActivation::Waiting, queue.pendingActivation);
    EXPECT_TRUE(state.privateClock.Valid());
    harken::DecisionCommittedSnapshot current{};
    ASSERT_TRUE(owned->decisions.CaptureCommitted(state.episode.originalDecision.Token(), &current));
    EXPECT_EQ(harken::AudioGroupState::Pending, current.audioState);
    EXPECT_FALSE(owned->decisions.IsCancelled(state.episode.originalDecision.Token()));
    EXPECT_TRUE(state.episode.bundle.ObserveInstalled());
    EXPECT_FALSE(owned->voices.ReleaseObserved(state.episode.source.release));
    std::array<std::uint64_t, 3> frames{};
    const harken::FrontierGroupBinding* primary = nullptr;
    const harken::FrontierGroupBinding* initial = nullptr;
    ASSERT_TRUE(state.Capture(&frames, &primary, &initial));
    EXPECT_EQ((std::array<std::uint64_t, 3>{state.anchor, state.anchor, state.anchor}), frames);
    EXPECT_EQ(state.groups[0], primary->operation.group);
    EXPECT_EQ(state.groups[1], initial->operation.group);
    // The C1 Decision is cancelled, while the original record/clock and
    // Pool owner gate remain unchanged. A structural joint proof is still
    // issuable; its native health capture must reject the actual cancellation.
    ASSERT_TRUE(state.privateClock.ReturnPlacement(&state.placement));
    EXPECT_FALSE(state.privateClock.Valid());
    ASSERT_TRUE(state.placement.Valid());
    harken::PrivateOriginalQueueGroupClockProof structuralJoint;
    ASSERT_TRUE(owned->timelinePool.ProvePrivateOriginalQueueGroupClock(
        state.Original(1), state.Original(3), &state.placement, &structuralJoint));
    ASSERT_TRUE(structuralJoint.Valid());
    EXPECT_FALSE(state.placement.Valid());
    harken::OriginalQueueFutureGroupLifetimePin actualC1;
    ASSERT_TRUE(state.Original(3).BorrowLifetime(&actualC1));
    std::array<std::uint64_t, 4> refusedFrames{83, 89, 97, 101};
    const harken::FrontierGroupBinding* refusedInitial = nullptr;
    const harken::FrontierGroupBinding* refusedConditional = nullptr;
    EXPECT_FALSE(owned->timelinePool.CapturePrivateOriginalQueueClockInitialSource(
        structuralJoint, state.initialOriginal, actualC1, owned->timelineArms,
        owned->decisions, state.episode.source.arm, &refusedFrames,
        &refusedInitial, &refusedConditional));
    EXPECT_EQ((std::array<std::uint64_t, 4>{83, 89, 97, 101}), refusedFrames);
    EXPECT_EQ(nullptr, refusedInitial);
    EXPECT_EQ(nullptr, refusedConditional);
    EXPECT_TRUE(structuralJoint.Valid());
    ASSERT_TRUE(structuralJoint.ReturnPlacement(&state.placement));
    EXPECT_FALSE(structuralJoint.Valid());
    ASSERT_TRUE(state.placement.Valid());
    actualC1.Release();
    ASSERT_TRUE(owned->timelinePool.ProvePrivateOriginalQueueGroupClock(
        state.Original(0), state.Original(1), &state.placement, &state.privateClock));
    ASSERT_TRUE(state.Capture(&frames, &primary, &initial));
    ASSERT_NO_FATAL_FAILURE(state.Finish());
}

TEST(OriginalSourceSubmission, OriginalC0ScopedGeometryKeepsActualSourceOnsetDistinctFromEntryAnchor) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    GenuineScopedC0SourceFixture state(*owned);
    constexpr std::uint64_t delay = 17;
    ASSERT_NO_FATAL_FAILURE(PrepareGenuineScopedC0Source(state, delay));
    std::array<std::uint64_t, 3> frames{};
    const harken::FrontierGroupBinding* primary = nullptr;
    const harken::FrontierGroupBinding* initial = nullptr;
    ASSERT_TRUE(state.Capture(&frames, &primary, &initial));
    EXPECT_EQ(state.anchor, frames[0]);
    EXPECT_EQ(state.anchor, frames[1]);
    EXPECT_EQ(state.anchor + delay, frames[2]);
    EXPECT_NE(frames[0], frames[2]);
    const auto* actual = owned->timelineArms.RowAt(state.episode.source.arm.slot);
    ASSERT_NE(nullptr, actual);
    EXPECT_EQ(frames[2], actual->startFrame);
    EXPECT_EQ(0u, actual->startedBlockSequence.load());
    ASSERT_NO_FATAL_FAILURE(state.Finish());
}

TEST(OriginalSourceSubmission, OriginalC0ScopedGeometryPreservesTwoActualRootOwnersAndPhysicalSourceOwner) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    GenuineScopedC0SourceFixture state(*owned);
    ASSERT_NO_FATAL_FAILURE(PrepareGenuineScopedC0Source(state, 0, true));
    std::array<std::uint64_t, 3> frames{};
    const harken::FrontierGroupBinding* primary = nullptr;
    const harken::FrontierGroupBinding* initial = nullptr;
    ASSERT_TRUE(state.Capture(&frames, &primary, &initial));
    EXPECT_EQ(state.independentAdmissionRoot, primary->operation.owner.child);
    EXPECT_EQ(state.episode.parent, initial->operation.owner.child);
    EXPECT_NE(primary->operation.owner.child, initial->operation.owner.child);
    EXPECT_EQ(state.episode.parent, state.start.event.ownerInstanceHandle);
    EXPECT_EQ(primary->operation.owner, state.start.event.frontier.owner);
    EXPECT_EQ(primary->entry, initial->entry);
    EXPECT_EQ(state.episode.source.arm.renderSlot, state.start.event.armToken.renderSlot);
    EXPECT_EQ(state.episode.source.arm.renderGeneration, state.start.event.armToken.renderGeneration);
    harken::OriginalQueueFutureGroupLifetimePin actualWrongOwner;
    ASSERT_TRUE(state.Original(2).BorrowLifetime(&actualWrongOwner));
    frames = {71, 73, 79};
    primary = initial = nullptr;
    // This is another genuine original Coordinator with the real Queue
    // owner, not a fabricated pin or a modified marker/identity.
    EXPECT_FALSE(owned->timelinePool.CapturePrivateOriginalQueueC0Source(
        state.privateClock, actualWrongOwner, state.initialOriginal,
        owned->timelineArms, owned->decisions, state.episode.source.arm,
        &frames, &primary, &initial));
    ASSERT_NO_FATAL_FAILURE(ExpectScopedC0SentinelUnchanged(frames, primary, initial));
    EXPECT_FALSE(owned->timelinePool.CapturePrivateOriginalQueueC0Source(
        state.privateClock, state.initialOriginal, state.primaryOriginal,
        owned->timelineArms, owned->decisions, state.episode.source.arm,
        &frames, &primary, &initial));
    ASSERT_NO_FATAL_FAILURE(ExpectScopedC0SentinelUnchanged(frames, primary, initial));
    actualWrongOwner.Release();
    ASSERT_TRUE(state.Capture(&frames, &primary, &initial));
    ASSERT_NO_FATAL_FAILURE(state.Finish());
}

TEST(OriginalSourceSubmission, OriginalC0ScopedGeometryRefusesOtherRealReceiverAndEveryStaleSourceGenerationWithoutWritingOutputs) {
    auto firstOwner = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto secondOwner = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    GenuineScopedC0SourceFixture a(*firstOwner), b(*secondOwner);
    ASSERT_NO_FATAL_FAILURE(PrepareGenuineScopedC0Source(a));
    ASSERT_NO_FATAL_FAILURE(PrepareGenuineScopedC0Source(b));
    ASSERT_TRUE(Registry::SameToken(a.episode.source.arm, b.episode.source.arm));
    std::array<std::uint64_t, 3> frames{71, 73, 79};
    const harken::FrontierGroupBinding* primary = nullptr;
    const harken::FrontierGroupBinding* initial = nullptr;
    EXPECT_FALSE(firstOwner->timelinePool.CapturePrivateOriginalQueueC0Source(a.privateClock,
        b.primaryOriginal, a.initialOriginal, firstOwner->timelineArms, firstOwner->decisions,
        a.episode.source.arm, &frames, &primary, &initial));
    ASSERT_NO_FATAL_FAILURE(ExpectScopedC0SentinelUnchanged(frames, primary, initial));
    EXPECT_FALSE(firstOwner->timelinePool.CapturePrivateOriginalQueueC0Source(a.privateClock,
        a.primaryOriginal, a.initialOriginal, secondOwner->timelineArms, firstOwner->decisions,
        a.episode.source.arm, &frames, &primary, &initial));
    ASSERT_NO_FATAL_FAILURE(ExpectScopedC0SentinelUnchanged(frames, primary, initial));
    EXPECT_FALSE(firstOwner->timelinePool.CapturePrivateOriginalQueueC0Source(a.privateClock,
        a.primaryOriginal, a.initialOriginal, firstOwner->timelineArms, secondOwner->decisions,
        a.episode.source.arm, &frames, &primary, &initial));
    ASSERT_NO_FATAL_FAILURE(ExpectScopedC0SentinelUnchanged(frames, primary, initial));
    for (std::uint32_t field = 0; field < 3; ++field) {
        auto stale = a.episode.source.arm;
        if (field == 0) ++stale.generation;
        if (field == 1) ++stale.renderGeneration;
        if (field == 2) ++stale.renderSlot;
        EXPECT_FALSE(firstOwner->timelinePool.CapturePrivateOriginalQueueC0Source(a.privateClock,
            a.primaryOriginal, a.initialOriginal, firstOwner->timelineArms, firstOwner->decisions,
            stale, &frames, &primary, &initial));
        ASSERT_NO_FATAL_FAILURE(ExpectScopedC0SentinelUnchanged(frames, primary, initial));
    }
    ASSERT_TRUE(a.Capture(&frames, &primary, &initial));
    ASSERT_NO_FATAL_FAILURE(a.Finish());
    ASSERT_NO_FATAL_FAILURE(b.Finish());
}

TEST(OriginalSourceSubmission, OriginalC0ScopedGeometryRefusesRealCancelledC0WhileOriginalStructuralLoansRemain) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    GenuineScopedC0SourceFixture state(*owned);
    ASSERT_NO_FATAL_FAILURE(PrepareGenuineScopedC0Source(state));
    const auto disposition = owned->decisions.Invalidate(state.decisions[0]);
    ASSERT_EQ(harken::DecisionInvalidationDisposition::NewlyInvalidated, disposition);
    EXPECT_TRUE(state.privateClock.Valid());
    EXPECT_TRUE(state.primaryOriginal.Valid());
    EXPECT_TRUE(state.initialOriginal.Valid());
    std::array<std::uint64_t, 3> frames{71, 73, 79};
    const harken::FrontierGroupBinding* primary = nullptr;
    const harken::FrontierGroupBinding* initial = nullptr;
    EXPECT_FALSE(state.Capture(&frames, &primary, &initial));
    ASSERT_NO_FATAL_FAILURE(ExpectScopedC0SentinelUnchanged(frames, primary, initial));
    EXPECT_EQ(0u, owned->timelineArms.RowAt(state.episode.source.arm.slot)->startedBlockSequence.load());
    ASSERT_NO_FATAL_FAILURE(state.Finish());
}


TEST(OriginalSourceSubmission, OriginalC0ScopedGeometryRefusesWholeConditionalRetirementWhileCurrentSourceRemainsInstalled) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    GenuineScopedC0SourceFixture state(*owned);
    ASSERT_NO_FATAL_FAILURE(PrepareGenuineScopedC0Source(state));
    std::array<std::uint64_t, 3> frames{};
    const harken::FrontierGroupBinding* primary = nullptr;
    const harken::FrontierGroupBinding* initial = nullptr;
    ASSERT_TRUE(state.Capture(&frames, &primary, &initial));
    ASSERT_TRUE(owned->queueInstances.RetireConditionalForAbort(state.queue,
        state.version, state.decisions[3]));
    state.oldConditionalEnded = true;
    BoundClockState clockState = BoundClockState::Unbound;
    ASSERT_TRUE(state.clock.CaptureState(&clockState));
    ASSERT_EQ(BoundClockState::Retired, clockState);
    harken::QueueInstanceSnapshot queue{};
    ASSERT_TRUE(owned->queueInstances.Capture(state.queue, &queue));
    ASSERT_EQ(0u, queue.pendingVersion);
    harken::DecisionCommittedSnapshot current{};
    ASSERT_TRUE(owned->decisions.CaptureCommitted(state.episode.originalDecision.Token(), &current));
    EXPECT_EQ(harken::AudioGroupState::Pending, current.audioState);
    EXPECT_FALSE(owned->decisions.IsCancelled(state.episode.originalDecision.Token()));
    EXPECT_TRUE(state.episode.bundle.ObserveInstalled());
    EXPECT_FALSE(owned->voices.ReleaseObserved(state.episode.source.release));
    harken::TimelineArmObservation source{};
    ASSERT_TRUE(owned->timelineArms.Observe(state.episode.source.arm, &source));
    EXPECT_EQ(state.installed, source.armedCompletionSequence);
    EXPECT_EQ(0u, source.startedBlockSequence);
    EXPECT_TRUE(state.privateClock.Valid());
    EXPECT_TRUE(state.primaryOriginal.Valid());
    EXPECT_TRUE(state.initialOriginal.Valid());
    frames = {71, 73, 79};
    primary = initial = nullptr;
    // The existing ordinary End invalidates the complete owner operation
    // gate. Structural retention cannot grant healthy C0 or a selected End.
    EXPECT_FALSE(state.Capture(&frames, &primary, &initial));
    ASSERT_NO_FATAL_FAILURE(ExpectScopedC0SentinelUnchanged(frames, primary, initial));
    ASSERT_NO_FATAL_FAILURE(state.Finish());
}


namespace {
// This initializes the actual fixed logic owner through its native gate. The
// tests issue only Lane lifetime; no Current, Source or placement is fabricated.
void PrepareEngineOriginalLaneAttempt(harken::Engine& engine,
    harken::EngineImmediateEntryAttempt& attempt, harken::ActionFiringHandle root,
    std::uint64_t generation) {
    ASSERT_TRUE(engine.OwnsImmediateEntryAttemptStorage(attempt));
    ASSERT_FALSE(attempt.active);
    ASSERT_FALSE(attempt.lane.Valid());
    ASSERT_FALSE(attempt.originalLaneLifetime.HandleObservation().Valid());
    ASSERT_EQ(0u, attempt.originalLaneFactoryRoot);
    ASSERT_EQ(0u, attempt.originalLaneFactoryAttemptGeneration);
    ASSERT_TRUE(attempt.gate.Prepare(generation));
    attempt.root = root;
    attempt.generation = generation;
    engine.SetImmediateEntryAttemptActive(attempt, true);
}
void FinishEngineOriginalLaneAttempt(harken::Engine& engine,
    harken::EngineImmediateEntryAttempt& attempt) {
    ASSERT_FALSE(attempt.originalLaneLifetime.HandleObservation().Valid());
    ASSERT_EQ(0u, attempt.originalLaneFactoryRoot);
    ASSERT_EQ(0u, attempt.originalLaneFactoryAttemptGeneration);
    ASSERT_FALSE(engine.RetireOriginalImmediateEntryLane(attempt, 0));
    attempt.lane = {};
    ASSERT_TRUE(attempt.gate.Abort(attempt.generation));
    ASSERT_TRUE(attempt.gate.Retire(attempt.generation));
    engine.SetImmediateEntryAttemptActive(attempt, false);
    attempt.root = 0;
}
} // namespace

TEST(OriginalSourceSubmission, EngineOriginalLaneSameReserveForeignBusyKeepsExactOwnerUntilRealReturnAndReuse) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig());
    auto& engine = *owned;
    ASSERT_GT(engine.immediateEntryAttempts.Capacity(), 0u);
    ASSERT_TRUE(engine.EnsureImmediateEntryAttempt(0));
    auto& attempt = engine.immediateEntryAttempts[0];
    ASSERT_NO_FATAL_FAILURE(PrepareEngineOriginalLaneAttempt(engine, attempt, 1201, 1));
    const harken::ImmediateLaneDemand demand{1, 0, 0, 1};
    ASSERT_TRUE(engine.ReserveOriginalImmediateEntryLane(attempt, demand, true));
    const auto originalHandle = attempt.lane;
    ASSERT_TRUE(engine.immediateActionLane.IsOriginalAttemptLifetimePinFor(
        attempt.originalLaneLifetime, originalHandle, 1201));
    EXPECT_EQ(1201u, attempt.originalLaneFactoryRoot);
    EXPECT_EQ(1u, attempt.originalLaneFactoryAttemptGeneration);
    harken::OriginalImmediateLaneAttemptLifetimePin foreign;
    ASSERT_TRUE(engine.BorrowOriginalImmediateEntryLane(attempt, &foreign));
    EXPECT_FALSE(engine.BorrowOriginalImmediateEntryLane(attempt, &foreign));
    harken::OriginalImmediateLaneAttemptLifetimePin moved(std::move(foreign));
    EXPECT_FALSE(foreign.Valid());
    ASSERT_TRUE(moved.Valid());
    ASSERT_TRUE(engine.immediateActionLane.Cancel(originalHandle));
    EXPECT_FALSE(engine.RetireOriginalImmediateEntryLane(attempt, 0));
    EXPECT_FALSE(engine.RetireOriginalImmediateEntryLane(attempt, 0));
    EXPECT_TRUE(attempt.active);
    EXPECT_EQ(originalHandle.slot, attempt.lane.slot);
    EXPECT_EQ(originalHandle.generation, attempt.lane.generation);
    EXPECT_EQ(1201u, attempt.root);
    EXPECT_EQ(1u, attempt.generation);
    EXPECT_TRUE(attempt.originalLaneLifetime.Valid());
    harken::ImmediateLaneObservation cancelled{};
    ASSERT_TRUE(engine.immediateActionLane.Observe(originalHandle, &cancelled));
    EXPECT_TRUE(cancelled.complete);
    EXPECT_FALSE(cancelled.applied);
    moved.Return();
    EXPECT_FALSE(moved.Valid());
    ASSERT_TRUE(engine.RetireOriginalImmediateEntryLane(attempt, 0));
    EXPECT_FALSE(engine.immediateActionLane.Observe(originalHandle, &cancelled));
    ASSERT_NO_FATAL_FAILURE(FinishEngineOriginalLaneAttempt(engine, attempt));
    ASSERT_NO_FATAL_FAILURE(PrepareEngineOriginalLaneAttempt(engine, attempt, 1202, 2));
    ASSERT_TRUE(engine.ReserveOriginalImmediateEntryLane(attempt, demand, true));
    EXPECT_EQ(originalHandle.slot, attempt.lane.slot);
    EXPECT_NE(originalHandle.generation, attempt.lane.generation);
    EXPECT_FALSE(engine.immediateActionLane.Observe(originalHandle, &cancelled));
    ASSERT_TRUE(engine.immediateActionLane.Cancel(attempt.lane));
    ASSERT_TRUE(engine.RetireOriginalImmediateEntryLane(attempt, 0));
    ASSERT_NO_FATAL_FAILURE(FinishEngineOriginalLaneAttempt(engine, attempt));
}

TEST(OriginalSourceSubmission, EngineOriginalLaneRejectsForeignReceiverAndChangedOriginalRootOrFullAttemptGeneration) {
    const auto config = InitialSingleClockConfig();
    auto firstOwner = std::make_unique<harken::Engine>(config);
    auto secondOwner = std::make_unique<harken::Engine>(config);
    auto& first = *firstOwner;
    auto& second = *secondOwner;
    ASSERT_TRUE(first.EnsureImmediateEntryAttempt(0));
    ASSERT_TRUE(second.EnsureImmediateEntryAttempt(0));
    auto& a = first.immediateEntryAttempts[0];
    auto& b = second.immediateEntryAttempts[0];
    ASSERT_NO_FATAL_FAILURE(PrepareEngineOriginalLaneAttempt(first, a, 1301, 1));
    ASSERT_NO_FATAL_FAILURE(PrepareEngineOriginalLaneAttempt(second, b, 1301, 1));
    const harken::ImmediateLaneDemand demand{1, 0, 0, 1};
    ASSERT_TRUE(first.ReserveOriginalImmediateEntryLane(a, demand, true));
    ASSERT_TRUE(second.ReserveOriginalImmediateEntryLane(b, demand, true));
    ASSERT_EQ(a.lane.slot, b.lane.slot);
    ASSERT_EQ(a.lane.generation, b.lane.generation);
    harken::OriginalImmediateLaneAttemptLifetimePin empty;
    EXPECT_FALSE(first.BorrowOriginalImmediateEntryLane(b, &empty));
    EXPECT_FALSE(first.RetireOriginalImmediateEntryLane(b, 0));
    EXPECT_FALSE(first.immediateActionLane.IsOriginalAttemptLifetimePinFor(
        b.originalLaneLifetime, a.lane, a.root));
    const auto root = a.root;
    const auto generation = a.generation;
    // Negative-only owner corruption cannot create a second issuer. Restore
    // the exact original receipt before the genuine cancellation is retried.
    a.root = root + 1u;
    EXPECT_FALSE(first.BorrowOriginalImmediateEntryLane(a, &empty));
    EXPECT_FALSE(first.RetireOriginalImmediateEntryLane(a, 0));
    a.root = root;
    a.generation = generation + 1u;
    EXPECT_FALSE(first.BorrowOriginalImmediateEntryLane(a, &empty));
    EXPECT_FALSE(first.RetireOriginalImmediateEntryLane(a, 0));
    a.generation = generation;
    ASSERT_TRUE(first.immediateActionLane.IsOriginalAttemptLifetimePinFor(
        a.originalLaneLifetime, a.lane, root));
    EXPECT_TRUE(b.originalLaneLifetime.Valid());
    ASSERT_TRUE(first.immediateActionLane.Cancel(a.lane));
    ASSERT_TRUE(second.immediateActionLane.Cancel(b.lane));
    ASSERT_TRUE(first.RetireOriginalImmediateEntryLane(a, 0));
    ASSERT_TRUE(second.RetireOriginalImmediateEntryLane(b, 0));
    ASSERT_NO_FATAL_FAILURE(FinishEngineOriginalLaneAttempt(first, a));
    ASSERT_NO_FATAL_FAILURE(FinishEngineOriginalLaneAttempt(second, b));
}

TEST(OriginalSourceSubmission, EngineOriginalLaneDefaultNoOutputRetainsLegacyNativeCancelAndRetire) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig());
    auto& engine = *owned;
    ASSERT_TRUE(engine.EnsureImmediateEntryAttempt(0));
    auto& attempt = engine.immediateEntryAttempts[0];
    ASSERT_NO_FATAL_FAILURE(PrepareEngineOriginalLaneAttempt(engine, attempt, 1401, 1));
    ASSERT_TRUE(engine.ReserveOriginalImmediateEntryLane(attempt, 1u));
    EXPECT_FALSE(attempt.originalLaneLifetime.Valid());
    EXPECT_EQ(0u, attempt.originalLaneFactoryRoot);
    EXPECT_EQ(0u, attempt.originalLaneFactoryAttemptGeneration);
    harken::OriginalImmediateLaneAttemptLifetimePin empty;
    EXPECT_FALSE(engine.BorrowOriginalImmediateEntryLane(attempt, &empty));
    ASSERT_TRUE(engine.immediateActionLane.Cancel(attempt.lane));
    ASSERT_TRUE(engine.RetireOriginalImmediateEntryLane(attempt, 0));
    ASSERT_NO_FATAL_FAILURE(FinishEngineOriginalLaneAttempt(engine, attempt));
}

TEST(OriginalSourceSubmission, EngineOriginalLaneJoinedTermReturnsOnlyOwnActualFactoryBaseline) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig());
    auto& engine = *owned;
    ASSERT_TRUE(engine.EnsureImmediateEntryAttempt(0));
    auto& attempt = engine.immediateEntryAttempts[0];
    ASSERT_NO_FATAL_FAILURE(PrepareEngineOriginalLaneAttempt(engine, attempt, 1501, 1));
    ASSERT_TRUE(engine.ReserveOriginalImmediateEntryLane(attempt, 1u, true));
    harken::OriginalImmediateLaneAttemptLifetimePin foreign;
    ASSERT_TRUE(engine.BorrowOriginalImmediateEntryLane(attempt, &foreign));
    EXPECT_FALSE(engine.RetireOriginalImmediateEntryLane(attempt, 0));
    foreign.Return();
    ASSERT_TRUE(attempt.originalLaneLifetime.Valid());
    EXPECT_EQ(0u, engine.mixer.BlocksRendered());
    // The real Engine destructor joins callbacks/workers, returns its fixed
    // original, and lets the native Lane assert that no foreign loans remain.
    owned.reset();
}


#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE) && !defined(HARKEN_RT_QUALIFICATION)
namespace harken::detail {
// This access reads only a refusal while the real native callback owns the
// capture. It cannot mint, cancel, change a field, or return a native loan.
struct OriginalConditionalWithdrawalNegativeTestAccess {
    static bool HasRealCapturedAudioPair(const ImmediateActionLane& lane,
        ImmediateLaneHandle handle, const ScheduledTransactionPool& pool,
        const DecisionTable& decisions, const Plan03GroupSliceIdentity& primary,
        const Plan03GroupSliceIdentity& member) noexcept {
        if (!handle.Valid() || handle.slot >= lane.config_.attempts) return false;
        const auto& actual = lane.attempts_[handle.slot];
        if (actual.generation != handle.generation ||
            !actual.captureActive.load(std::memory_order_acquire) ||
            actual.capturedBucketOrdinal >= actual.bucketCount ||
            !actual.capturedConditionalProjection || !actual.conditionalProjectionBound ||
            actual.pinGroupPool != &pool || actual.conditionalProjectionDecisions != &decisions ||
            actual.conditionalProjectionGroups[0] != primary ||
            actual.conditionalProjectionGroups[1] != member) return false;
        const auto* bucket = lane.BucketBase(handle.slot) + actual.capturedBucketOrdinal;
        if (bucket == nullptr || bucket->attemptGeneration != handle.generation ||
            bucket->groupCount != 4u || bucket->eventCount != 3u) return false;
        std::uint32_t count = 0, primaries = 0, members = 0;
        if (!lane.CapturePublishedGroupCount(handle, &count) || count != 4u) return false;
        for (std::uint32_t index = 0; index < count; ++index) {
            ImmediateLaneGroupRef declaration{};
            if (!lane.CapturePublishedGroupDescriptor(handle, index, &declaration)) return false;
            if (declaration.identity != primary && declaration.identity != member) continue;
            if (declaration.laneSlot != handle.slot ||
                declaration.laneGeneration != handle.generation ||
                declaration.bucketOrdinal != actual.capturedBucketOrdinal ||
                declaration.dueQuantum != bucket->quantum ||
                declaration.groupIndex != index ||
                index < bucket->firstGroup || index - bucket->firstGroup >= bucket->groupCount ||
                !lane.IsGroupAdmitted(handle.slot, bucket, index)) return false;
            if (declaration.identity == primary) {
                if (index != 2u) return false;
                ++primaries;
            } else {
                if (index != 3u) return false;
                ++members;
            }
        }
        return primaries == 1u && members == 1u;
    }
    static bool RefusesCapturedPair(const ImmediateActionLane& lane,
        ImmediateLaneHandle handle, const ScheduledTransactionPool& pool,
        const DecisionTable& decisions, const Plan03GroupSliceIdentity& primary,
        const Plan03GroupSliceIdentity& member) noexcept {
        const ImmediateLaneBucket* bucket = nullptr;
        if (handle.slot < lane.config_.attempts) {
            const auto& actual = lane.attempts_[handle.slot];
            if (actual.captureActive.load(std::memory_order_acquire) &&
                actual.capturedBucketOrdinal < actual.bucketCount)
                bucket = lane.BucketBase(handle.slot) + actual.capturedBucketOrdinal;
        }
        ImmediateLaneGroupRef primaryDeclaration{}, memberDeclaration{};
        std::uint32_t count = 0;
        if (lane.CapturePublishedGroupCount(handle, &count)) {
            for (std::uint32_t index = 0; index < count; ++index) {
                ImmediateLaneGroupRef actual{};
                if (!lane.CapturePublishedGroupDescriptor(handle, index, &actual)) return true;
                if (actual.identity == primary) primaryDeclaration = actual;
                if (actual.identity == member) memberDeclaration = actual;
            }
        }
        return !OriginalImmediateLaneConditionalProjection::CapturedWithdrawalExcludesPair(
            lane, handle, bucket, pool, decisions, primaryDeclaration, memberDeclaration);
    }
};
}
namespace {
struct GenuineSameBucketLateWithdrawalNegative {
    GenuineBoundC1ProjectionHarness state;
    harken::Engine& foreign;
    bool called = false;
    GenuineSameBucketLateWithdrawalNegative(harken::Engine& actual, harken::Engine& other)
        : state(actual), foreign(other) {}
    static void Capture(void* context, std::uint32_t slot) {
        auto& self = *static_cast<GenuineSameBucketLateWithdrawalNegative*>(context);
        if (slot != self.state.actual.old.lane.slot) return;
        self.called = true;
        auto& engine = self.state.Engine();
        const auto handle = self.state.originalLane.HandleObservation();
        const auto primary = self.state.actual.old.groups[2];
        const auto member = self.state.actual.old.groups[3];
        using Access = harken::detail::OriginalConditionalWithdrawalNegativeTestAccess;
        // Independently prove that this is the real complete same-bucket C1
        // capture. Refusal from null or missing descriptors cannot satisfy it.
        EXPECT_TRUE(Access::HasRealCapturedAudioPair(engine.immediateActionLane, handle,
            engine.timelinePool, engine.decisions, primary, member));
        // BeginConditionalProjection has already won its real shared admission
        // CAS before this actual after-capture hook. No request is an exception right.
        EXPECT_TRUE(Access::RefusesCapturedPair(engine.immediateActionLane, handle,
            engine.timelinePool, engine.decisions, primary, member));
        EXPECT_TRUE(self.state.projection.RequestWithdrawal());
        // A late monotonic request cannot turn that existing audio ownership
        // into permission for C0 to ignore the live same-bucket C1 pair.
        EXPECT_TRUE(Access::RefusesCapturedPair(engine.immediateActionLane, handle,
            engine.timelinePool, engine.decisions, primary, member));
        EXPECT_TRUE(Access::RefusesCapturedPair(engine.immediateActionLane, handle,
            self.foreign.timelinePool, engine.decisions, primary, member));
        EXPECT_TRUE(Access::RefusesCapturedPair(engine.immediateActionLane, handle,
            engine.timelinePool, self.foreign.decisions, primary, member));
        auto stale = handle;
        ++stale.generation; // Negative observation only; this does not mint a cap.
        EXPECT_TRUE(Access::RefusesCapturedPair(engine.immediateActionLane, stale,
            engine.timelinePool, engine.decisions, primary, member));
        EXPECT_TRUE(Access::RefusesCapturedPair(engine.immediateActionLane, handle,
            engine.timelinePool, engine.decisions, self.state.actual.old.groups[0], member));
    }
};
TEST(OriginalSourceSubmission, BoundC1SameBucketRealCapturedAudioRefusesLateWithdrawalExceptionAndReturnsNativeTail) {
    auto owned = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    auto foreign = std::make_unique<harken::Engine>(InitialSingleClockConfig(), 0, 2);
    GenuineSameBucketLateWithdrawalNegative actual(*owned, *foreign);
    auto& state = actual.state;
    ASSERT_NO_FATAL_FAILURE(state.Prepare(17));
    ASSERT_NO_FATAL_FAILURE(state.actual.PublishOriginalForAudio());
    const auto wanted = state.actual.old.anchor / kFrames + 1u;
    ASSERT_GT(wanted, owned->mixer.BlocksRendered());
    owned->immediateActionLane.SetCapturedAdmissionHookForTest(
        &GenuineSameBucketLateWithdrawalNegative::Capture, &actual);
    owned->nullBackend.Pump(static_cast<std::uint32_t>(wanted - owned->mixer.BlocksRendered()));
    owned->immediateActionLane.SetCapturedAdmissionHookForTest(nullptr, nullptr);
    ASSERT_EQ(wanted, owned->mixer.BlocksRendered());
    ASSERT_TRUE(actual.called);
    // Producer-only retirement is checked after the actual callback returns.
    // Equal to its actual native last-use sequence must refuse before writes.
    EXPECT_FALSE(state.projection.TryRetireNativeConditional(
        owned->decisions, owned->mixer.BlocksRendered()));
    // This is the existing live joint-bucket refusal, not the withdrawn-pair
    // positive. Preserve the separate unchanged genuine17-frame PCM positive.
    EXPECT_EQ(0.0f, CurrentPeak(*owned));
    harken::TimelineArmObservation source{};
    ASSERT_TRUE(owned->timelineArms.Observe(state.actual.old.episode.source.arm, &source));
    EXPECT_EQ(0u, source.startedBlockSequence);
    harken::CompletionProgressSnapshot progress{};
    ASSERT_TRUE(harken::CaptureCompletionProgressExact(*state.actual.old.episode.progress,
        state.actual.old.episode.occurrence,
        static_cast<std::uint32_t>(state.actual.old.episode.source.source >> 32), &progress));
    EXPECT_EQ(0u, progress.logicalOutputFramesProduced);
    ASSERT_TRUE(state.projection.Valid());
    ASSERT_TRUE(state.originalLane.Valid());
    // An AudioFinal refusal is immutable. Cancel both actual sidecars before
    // advancing the real callback fence; retain the resulting receipts verbatim.
    std::array<harken::FrontierActivationReceipt, 2> savedReceipts{};
    for (const auto index : {0u, 2u}) {
        auto* activation = owned->decisions.ResolveFrontierActivation(state.actual.old.decisions[index]);
        ASSERT_NE(nullptr, activation);
        const auto entry = activation->Descriptor().entry;
        harken::FrontierActivationReceipt before{};
        const auto read = activation->Read(entry, wanted, &before);
        const bool audioFinal = read == harken::FrontierReceiptRead::Ready;
        if (audioFinal) {
            ASSERT_EQ(harken::FrontierReceiptState::AudioFinal, activation->State());
            ASSERT_EQ(harken::FrontierReceiptOutcome::DefinitivelyNotApplied, before.outcome);
            ASSERT_EQ(harken::FrontierReceiptOrigin::Audio, before.origin);
            ASSERT_EQ(wanted, before.audioSequence);
            ASSERT_NE(harken::FrontierReceiptReason::None, before.reason);
            // Joint-bucket refusal can cancel a coordinator without a local identity failure.
            // The exact audio reason must survive the later cancellation below.
        } else {
            ASSERT_EQ(harken::FrontierReceiptRead::InFlight, read);
            ASSERT_EQ(harken::FrontierReceiptState::Pending, activation->State());
        }
        const auto cancelled = activation->Cancel(entry, harken::AudioSliceInvalidateReason::Cancel);
        ASSERT_EQ(audioFinal ? harken::FrontierCancelDisposition::AlreadyCancelled :
            harken::FrontierCancelDisposition::CancelledBeforeAudio, cancelled);
        auto& saved = savedReceipts[index / 2u];
        ASSERT_EQ(harken::FrontierReceiptRead::Ready, activation->Read(entry, wanted, &saved));
        EXPECT_EQ(harken::FrontierReceiptOutcome::DefinitivelyNotApplied, saved.outcome);
        EXPECT_TRUE(saved.entry == entry);
        if (audioFinal) {
            EXPECT_EQ(before.origin, saved.origin);
            EXPECT_EQ(before.reason, saved.reason);
            EXPECT_EQ(before.audioSequence, saved.audioSequence);
            EXPECT_TRUE(before.order == saved.order);
            EXPECT_EQ(before.failingMember, saved.failingMember);
            EXPECT_TRUE(before.stop == saved.stop);
        } else {
            EXPECT_EQ(harken::FrontierReceiptReason::Cancelled, saved.reason);
            EXPECT_EQ(harken::FrontierReceiptOrigin::LogicBeforeAudio, saved.origin);
        }
        // CollectGroup actually read each same-bucket primary at wanted. A Ready
        // receipt does not permit its reader to return at that same watermark.
        harken::FrontierActivationReceipt blocked{};
        ASSERT_FALSE(owned->decisions.ReconcileFrontierActivation(state.actual.old.decisions[index],
            entry, wanted, &blocked));
        harken::FrontierActivationSidecarSnapshot held{};
        ASSERT_TRUE(owned->decisions.CaptureFrontierActivationSidecar(state.actual.old.decisions[index],
            entry, wanted, &held));
        EXPECT_TRUE(held.pagePinHeld);
        EXPECT_TRUE(held.observerPinHeld);
    }
    owned->nullBackend.Pump(1);
    ASSERT_EQ(wanted + 1u, owned->mixer.BlocksRendered());
    for (const auto index : {0u, 2u}) {
        const auto& saved = savedReceipts[index / 2u];
        harken::FrontierActivationReceipt receipt{};
        ASSERT_TRUE(owned->decisions.ReconcileFrontierActivation(state.actual.old.decisions[index],
            saved.entry, owned->mixer.BlocksRendered(), &receipt));
        EXPECT_EQ(saved.outcome, receipt.outcome);
        EXPECT_TRUE(saved.entry == receipt.entry);
        EXPECT_EQ(saved.origin, receipt.origin);
        EXPECT_EQ(saved.reason, receipt.reason);
        EXPECT_EQ(saved.audioSequence, receipt.audioSequence);
        EXPECT_TRUE(saved.order == receipt.order);
        EXPECT_EQ(saved.failingMember, receipt.failingMember);
        EXPECT_TRUE(saved.stop == receipt.stop);
        harken::FrontierActivationSidecarSnapshot returned{};
        ASSERT_TRUE(owned->decisions.CaptureFrontierActivationSidecar(state.actual.old.decisions[index],
            saved.entry, owned->mixer.BlocksRendered(), &returned));
        EXPECT_FALSE(returned.pagePinHeld);
        EXPECT_FALSE(returned.observerPinHeld);
    }
    state.projection.Return();
    state.originalLane.Return();
    ASSERT_NO_FATAL_FAILURE(state.actual.old.EndAndRetireSource());
    ASSERT_NO_FATAL_FAILURE(state.actual.old.ReturnAllAndReclaim());
    EXPECT_EQ(0u, owned->voices.ActiveCount());
    EXPECT_EQ(nullptr, owned->voices.Resolve(state.actual.old.episode.source.source));
    EXPECT_FALSE(owned->timelineArms.Observe(state.actual.old.episode.source.arm, &source));
}
}
#endif

namespace {
struct NativeUnpublishedDispositionSource {
    VnVoiceHandle render = 0;
    harken::VoiceReleaseObserver release{};
    harken::TimelineArmToken arm{};
    Registry::OriginalSourceLifetimePin original;
    harken::VoicePool::OriginalSourceBodyLifetimePin originalBody;
    harken::CommandQueue::BatchReservation reservation{};
    harken::CommandQueue::PreparedBatchDispositionReceipt cancellation;
    harken::CommandQueue::PreparedBatchProof batch;
    std::array<harken::CommandRecord, 3> body{};

    void Prepare(harken::Engine& engine, bool bindRange, bool commit = true,
        bool identifySource = true, bool expectCapture = true,
        harken::ParentAudioGroupRef parent = {}, bool captureBody = false,
        std::uint64_t startFrame = 0, bool pendingImmediate = false,
        bool finalSource = false, Registry* receiver = nullptr, bool initialSource = false) {
        const auto pcm = engine.registry.Lookup(kSound);
        ASSERT_TRUE(pcm.Valid());
        ASSERT_EQ(VN_OK, engine.voices.Play(pcm, 0.0f, 1.0, 0, false, 0, 0, 0,
            harken::ResolvePanGains(engine.config.mixChannelLayout, 0.0f),
            0, pcm.frames, 1, harken::ResampleProfile::Professional44148,
            &render, 1, UINT32_MAX, 0, true));
        ASSERT_TRUE(engine.voices.CaptureReleaseObserver(render, &release));
        harken::VoicePool::PreparedCommitInput physical{};
        physical.renderHandle = render; physical.originalRelease = release;
        ASSERT_TRUE(engine.voices.CapturePreparedRoutingV1(render, &physical.routing));
        harken::VoicePool::PreparedCommitProof voiceProof;
        ASSERT_TRUE(engine.voices.ProvePreparedCommit(physical, &voiceProof));
        harken::TimelineArmTable::PreparedCommitInput logical{};
        logical.target.subjectId = kSound;
        logical.startFrame = logical.armDeadline = startFrame;
        logical.parentGroup = parent;
        logical.memberIndex = parent.Valid() ? 0 : UINT16_MAX;
        logical.pendingImmediateStart = pendingImmediate;
        ASSERT_FALSE(pendingImmediate && parent.Valid());
        if (parent.Valid()) {
            ASSERT_TRUE(engine.timelineArms.PrepareGrouped(static_cast<std::uint32_t>(render),
                static_cast<std::uint32_t>(render >> 32), startFrame, startFrame, 0, logical.target, parent, 0, &arm));
        } else if (pendingImmediate) {
            ASSERT_TRUE(engine.timelineArms.PreparePendingImmediate(static_cast<std::uint32_t>(render),
                static_cast<std::uint32_t>(render >> 32), 0, logical.target, &arm));
        } else {
            ASSERT_TRUE(engine.timelineArms.Prepare(static_cast<std::uint32_t>(render),
                static_cast<std::uint32_t>(render >> 32), startFrame, startFrame, 0, logical.target, &arm));
        }
        logical.arm = arm;
        harken::TimelineArmTable::PreparedCommitProof armProof;
        ASSERT_TRUE(engine.timelineArms.ProvePreparedCommit(logical, &armProof));
        auto& header = body[0];
        header.opcode = harken::kOpcodePlay; header.payload.play.handle = render;
        header.payload.play.sourceIdentity = identifySource ? render : 0;
        header.payload.play.batch.batchVersion = harken::kPlayLedBatchVersion;
        header.payload.play.batch.batchKind = harken::PlayLedBatchKind::TimelineArm;
        const auto count = initialSource ? 3u : 1u;
        header.payload.play.batch.batchCommandCount = count;
        header.payload.play.batch.armToken = (static_cast<std::uint64_t>(arm.generation) << 32) | arm.slot;
        if (initialSource) {
            body[1].opcode = harken::kOpcodeSetVoicePan;
            body[1].payload.setVoicePan.handle = render;
            body[2].opcode = harken::kOpcodeSetVoiceSpatialScalar;
            body[2].payload.setVoiceSpatialScalar.handle = render;
            body[2].payload.setVoiceSpatialScalar.kind = harken::kSpatialScalarKindWidth;
        }
        auto& sources = receiver == nullptr ? engine.originalSourceSubmissions : *receiver;
        const bool captured = finalSource
            ? sources.CapturePreparedFinalSource(header, armProof,
                voiceProof, {}, &original, captureBody ? &originalBody : nullptr)
            : sources.CapturePreparedSource(header, armProof,
                voiceProof, {}, &original, captureBody ? &originalBody : nullptr);
        if (!expectCapture) { ASSERT_FALSE(captured); return; }
        ASSERT_TRUE(captured);
        if (bindRange) {
            ASSERT_TRUE(initialSource ? engine.commands.ReserveInitialSource(&reservation)
                : engine.commands.ReserveBatch(count, &reservation));
            ASSERT_TRUE(engine.commands.ProvePreparedBatch(&reservation, body.data(), count, &batch, &cancellation));
            ASSERT_TRUE(engine.originalSourceSubmissions.BindPreparedChild(batch, 0, count));
        }
        if (commit) {
            harken::VoicePool::ConsumePreparedCommit(std::move(voiceProof));
            harken::TimelineArmTable::ConsumePreparedCommit(std::move(armProof));
        }
    }
    void Dispose(harken::Engine& engine) {
        ASSERT_TRUE(engine.originalSourceSubmissions.CancelBoundPreparation(arm, batch));
        ASSERT_TRUE(engine.originalSourceSubmissions.DisposeUnpublishedVoice(arm));
        ASSERT_TRUE(engine.voices.ReleaseObserved(release));
        ASSERT_TRUE(engine.originalSourceSubmissions.WithdrawUnpublished(arm));
        ASSERT_EQ(Registry::Phase::Disposed, engine.originalSourceSubmissions.Find(arm)->State());
    }
};
void RegisterNativeDispositionPcm() {
    static const std::vector<float> samples(kFrames * 4, 0.25f);
    const auto pcm = harkentest::MonoDesc(samples, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSound, &pcm));
}
}

TEST(OriginalSourceSubmission, FinalSourceUnpublishedAbortReturnsOriginalOwnersWithoutPublication) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterNativeDispositionPcm());
    const auto free = engine.timelineArms.FreeCount();
    NativeUnpublishedDispositionSource source;
    ASSERT_NO_FATAL_FAILURE(source.Prepare(engine, true, false, true, true, {}, true, 0, false, true));
    const auto* row = engine.timelineArms.RowAt(source.arm.slot);
    EXPECT_TRUE(row->finalSourcePublicationIssued);
    EXPECT_FALSE(row->finalSourcePublicationConsumed);
    ASSERT_NO_FATAL_FAILURE(source.Dispose(engine));
    source.original.Release();
    source.originalBody.Release();
    ASSERT_TRUE(engine.timelineArms.CancelPrepared(source.arm));
    EXPECT_EQ(free, engine.timelineArms.FreeCount());
    EXPECT_EQ(0u, engine.commands.PublishedHead());
    EXPECT_EQ(0u, engine.mixer.BlocksRendered());
}

TEST(OriginalSourceSubmission, FinalSourceFactoryRefusesUnboundPendingFrameBeforeTakingOwners) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterNativeDispositionPcm());
    const auto free = engine.timelineArms.FreeCount();
    NativeUnpublishedDispositionSource source;
    ASSERT_NO_FATAL_FAILURE(source.Prepare(engine, false, false, true, false, {}, true, 0, true, true));
    const auto* row = engine.timelineArms.RowAt(source.arm.slot);
    EXPECT_FALSE(row->finalSourcePublicationIssued);
    EXPECT_FALSE(row->originalLifetimePinned.load());
    EXPECT_FALSE(source.original.Valid());
    EXPECT_FALSE(source.originalBody.Valid());
    ASSERT_TRUE(engine.voices.CancelPreparedVoice(source.render));
    ASSERT_TRUE(engine.timelineArms.CancelPrepared(source.arm));
    EXPECT_EQ(free, engine.timelineArms.FreeCount());
    EXPECT_EQ(0u, engine.commands.PublishedHead());
}

TEST(OriginalSourceSubmission, FinalSourceFactoryRefusesGroupedArmBeforeTakingOwners) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterNativeDispositionPcm());
    const harken::VariableDefinitionV1 definition{
        harkentest::ObjectId(0x791), harken::VariableKindV1::Step, 0.0f, 1.0f, 0.0f};
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
        harkentest::InstallDefinitionsWorld(&engine, &definition, 1, UINT64_C(0x4A4F494E54534F55)).status);
    harken::DecisionReserveRequest request{};
    request.ticket.sequence = engine.logicCommits.Issue();
    request.world = engine.variables.CurrentWorldGeneration();
    request.transport = engine.timelinePool.Epoch();
    request.expectedContinuationGeneration = 1;
    request.sliceCount = 1;
    harken::DecisionToken decision{};
    ASSERT_TRUE(engine.decisions.Reserve(request, &decision));
    NativeUnpublishedDispositionSource source;
    ASSERT_NO_FATAL_FAILURE(source.Prepare(engine, false, false, true, false, {decision}, true, 0, false, true));
    const auto* row = engine.timelineArms.RowAt(source.arm.slot);
    EXPECT_FALSE(row->finalSourcePublicationIssued);
    EXPECT_FALSE(row->originalLifetimePinned.load());
    EXPECT_FALSE(source.original.Valid());
    EXPECT_FALSE(source.originalBody.Valid());
    ASSERT_TRUE(engine.voices.CancelPreparedVoice(source.render));
    ASSERT_TRUE(engine.timelineArms.CancelPrepared(source.arm));
    ASSERT_TRUE(engine.decisions.Cancel(decision));
    EXPECT_EQ(0u, engine.commands.PublishedHead());
}

TEST(OriginalSourceSubmission, FinalSourceFactoryRefusesForeignArmTableBeforeTakingOwners) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterNativeDispositionPcm());
    harken::TimelineArmTable foreignArms(4);
    Registry foreign(foreignArms, engine.voices, engine.commands, engine.effectBank);
    NativeUnpublishedDispositionSource source;
    ASSERT_NO_FATAL_FAILURE(source.Prepare(engine, false, false, true, false, {}, true, 0, false, true, &foreign));
    const auto* row = engine.timelineArms.RowAt(source.arm.slot);
    EXPECT_FALSE(row->finalSourcePublicationIssued);
    EXPECT_FALSE(row->originalLifetimePinned.load());
    EXPECT_FALSE(source.original.Valid());
    EXPECT_FALSE(source.originalBody.Valid());
    ASSERT_TRUE(engine.voices.CancelPreparedVoice(source.render));
    ASSERT_TRUE(engine.timelineArms.CancelPrepared(source.arm));
    EXPECT_EQ(0u, engine.commands.PublishedHead());
}

TEST(OriginalSourceSubmission, NativeUnpublishedDispositionRequiresTrueBatchCancelAndPhysicalRelease) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterNativeDispositionPcm());
    NativeUnpublishedDispositionSource source;
    ASSERT_NO_FATAL_FAILURE(source.Prepare(engine, true));
    const auto head = engine.commands.PublishedHead();
    EXPECT_EQ(nullptr, engine.originalSourceSubmissions.OriginalUnpublishedDisposition(source.arm));
    EXPECT_FALSE(engine.originalSourceSubmissions.WithdrawUnpublished(source.arm));
    ASSERT_TRUE(engine.originalSourceSubmissions.CancelBoundPreparation(source.arm, source.batch));
    EXPECT_TRUE(source.cancellation.CancelledUnpublishedFrom(engine.commands));
    EXPECT_EQ(nullptr, engine.originalSourceSubmissions.OriginalUnpublishedDisposition(source.arm));
    EXPECT_FALSE(engine.originalSourceSubmissions.WithdrawUnpublished(source.arm));
    ASSERT_TRUE(engine.originalSourceSubmissions.DisposeUnpublishedVoice(source.arm));
    ASSERT_TRUE(engine.voices.ReleaseObserved(source.release));
    EXPECT_EQ(nullptr, engine.originalSourceSubmissions.OriginalUnpublishedDisposition(source.arm));
    ASSERT_TRUE(engine.originalSourceSubmissions.WithdrawUnpublished(source.arm));
    const auto* actual = engine.originalSourceSubmissions.OriginalUnpublishedDisposition(source.arm);
    ASSERT_NE(nullptr, actual);
    EXPECT_TRUE(actual->Valid()); EXPECT_TRUE(Registry::SameToken(source.arm, actual->Token()));
    ASSERT_TRUE(engine.originalSourceSubmissions.WithdrawUnpublished(source.arm));
    EXPECT_EQ(actual, engine.originalSourceSubmissions.OriginalUnpublishedDisposition(source.arm));
    source.original.Release();
    EXPECT_FALSE(engine.timelineArms.RowAt(source.arm.slot)->originalLifetimePinned.load());
    EXPECT_EQ(head, engine.commands.PublishedHead()); EXPECT_EQ(0u, engine.mixer.BlocksRendered());
    EXPECT_EQ(harken::ArmState::PendingApply, engine.timelineArms.RowAt(source.arm.slot)->state.load());
    EXPECT_EQ(0u, engine.timelineArms.RowAt(source.arm.slot)->armedCompletionSequence.load());
    EXPECT_EQ(0u, engine.timelineArms.RowAt(source.arm.slot)->startedBlockSequence.load());
    EXPECT_EQ(0u, engine.timelineArms.RowAt(source.arm.slot)->tombstoneAudioSequence.load());
}

TEST(OriginalSourceSubmission, NativeUnpublishedDispositionAcceptsTrueNeverBoundPreparationWithoutFreeingArm) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult()); auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterNativeDispositionPcm());
    NativeUnpublishedDispositionSource source;
    ASSERT_NO_FATAL_FAILURE(source.Prepare(engine, false, false));
    ASSERT_NO_FATAL_FAILURE(source.Dispose(engine));
    const auto* actual = engine.originalSourceSubmissions.OriginalUnpublishedDisposition(source.arm);
    ASSERT_NE(nullptr, actual); EXPECT_TRUE(Registry::SameToken(actual->Token(), source.arm));
    EXPECT_TRUE(engine.timelineArms.RowAt(source.arm.slot)->originalLifetimePinned.load());
    EXPECT_FALSE(engine.timelineArms.CancelPrepared(source.arm));
    source.original.Release();
    EXPECT_FALSE(engine.timelineArms.RowAt(source.arm.slot)->originalLifetimePinned.load());
    ASSERT_TRUE(engine.timelineArms.CancelPrepared(source.arm));
    EXPECT_EQ(0u, engine.mixer.BlocksRendered()); EXPECT_EQ(0u, engine.commands.PublishedHead());
}

TEST(OriginalSourceSubmission, UnpublishedStorageWaitsForBothLoansAcrossPreparationAndCommit) {
    for (const auto transport : {0u, 1u, 2u}) {
        const bool bound = transport != 0;
        const bool initialSource = transport == 2;
        for (const bool committed : {false, true}) {
            for (const bool bodyFirst : {false, true}) {
                SCOPED_TRACE(testing::Message() << "transport=" << transport << " committed=" << committed
                    << " bodyFirst=" << bodyFirst);
                harkentest::EngineSession session(SourceBodyConfig());
                ASSERT_EQ(VN_OK, session.InitResult());
                auto& engine = *session.Engine();
                ASSERT_NO_FATAL_FAILURE(RegisterNativeDispositionPcm());
                const auto free = engine.timelineArms.FreeCount();
                NativeUnpublishedDispositionSource source;
                ASSERT_NO_FATAL_FAILURE(source.Prepare(engine, bound, committed, true, true, {}, true,
                    0, false, false, nullptr, initialSource));
                Registry::OriginalSourceLifetimePin sourceLoan;
                harken::VoicePool::OriginalSourceBodyLifetimePin bodyLoan;
                ASSERT_TRUE(source.original.Borrow(&sourceLoan));
                ASSERT_TRUE(source.originalBody.Borrow(&bodyLoan));
                EXPECT_FALSE(engine.originalSourceSubmissions.CanReturnUnpublishedOriginalSourcePairStorage(
                    source.original, source.originalBody));
                ASSERT_NO_FATAL_FAILURE(source.Dispose(engine));
                EXPECT_TRUE(engine.voices.ReleaseObserved(source.release));
                EXPECT_FALSE(engine.originalSourceSubmissions.CanReturnUnpublishedOriginalSourcePairStorage(
                    source.original, source.originalBody));
                if (bodyFirst) bodyLoan.Release();
                else sourceLoan.Release();
                EXPECT_FALSE(engine.originalSourceSubmissions.CanReturnUnpublishedOriginalSourcePairStorage(
                    source.original, source.originalBody));
                if (bodyFirst) sourceLoan.Release();
                else bodyLoan.Release();
                EXPECT_TRUE(engine.originalSourceSubmissions.CanReturnUnpublishedOriginalSourcePairStorage(
                    source.original, source.originalBody));
                source.originalBody.Release();
                source.original.Release();
                const auto* disposition = engine.originalSourceSubmissions.OriginalUnpublishedDisposition(source.arm);
                ASSERT_NE(nullptr, disposition);
                using Retirement = harken::TimelineArmTable::UnparentedUnpublishedSourceRetirement;
                if (committed)
                    EXPECT_EQ(Retirement::Consumed,
                        engine.timelineArms.ConsumeOriginalUnparentedUnpublishedSource(*disposition));
                else
                    EXPECT_TRUE(engine.timelineArms.CancelPrepared(source.arm));
                EXPECT_EQ(free, engine.timelineArms.FreeCount());
                EXPECT_EQ(0u, engine.commands.PublishedHead());
                EXPECT_EQ(0u, engine.mixer.BlocksRendered());
            }
        }
    }
}

TEST(OriginalSourceSubmission, PublishedPairReturnRequiresRealRangeAndExactPhysicalRelease) {
    for (const bool cancelledBeforeInstall : {false, true}) {
        SCOPED_TRACE(cancelledBeforeInstall);
        harkentest::EngineSession session(SourceBodyConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        const std::vector<float> samples(kRate, 0.25f);
        SourceBody first, second;
        ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, first, samples));
        second.independent = first.independent;
        Registry::OriginalSourceLifetimePin source, otherSource, sourceLoan;
        BodyPin original, otherBody, bodyLoan;
        ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, first, false, false, &source, &original));
        ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, second, false, false, &otherSource, &otherBody));
        ASSERT_TRUE(source.Borrow(&sourceLoan));
        ASSERT_TRUE(original.Borrow(&bodyLoan));
        EXPECT_FALSE(engine.originalSourceSubmissions.CanReturnPublishedOriginalSourcePairStorage(
            source, original, first.release));
        if (cancelledBeforeInstall) ASSERT_TRUE(engine.timelineArms.RequestCancel(first.arm));
        engine.nullBackend.Pump(1);
        ASSERT_EQ(Registry::Phase::Disposed, engine.originalSourceSubmissions.Find(first.arm)->State());
        if (!cancelledBeforeInstall) {
            EXPECT_FALSE(engine.originalSourceSubmissions.CanReturnPublishedOriginalSourcePairStorage(
                source, original, first.release));
            ASSERT_NO_FATAL_FAILURE(StopOriginalBodyEpisode(engine, first));
        }
        ASSERT_TRUE(engine.voices.ReleaseObserved(first.release));
        auto wrongRelease = first.release;
        ++wrongRelease.targetSequence;
        EXPECT_FALSE(engine.originalSourceSubmissions.CanReturnPublishedOriginalSourcePairStorage(
            source, original, wrongRelease));
        EXPECT_FALSE(engine.originalSourceSubmissions.CanReturnPublishedOriginalSourcePairStorage(
            source, original, second.release));
        EXPECT_FALSE(engine.originalSourceSubmissions.CanReturnPublishedOriginalSourcePairStorage(
            source, otherBody, first.release));
        EXPECT_FALSE(engine.originalSourceSubmissions.CanReturnPublishedOriginalSourcePairStorage(
            otherSource, original, first.release));
        ASSERT_TRUE(engine.originalSourceSubmissions.CanReturnPublishedOriginalSourcePairStorage(
            source, original, first.release));
        original.Release();
        source.Release();
        EXPECT_TRUE(engine.timelineArms.RowAt(first.arm.slot)->originalLifetimePinned.load());
        ASSERT_TRUE(engine.originalSourceSubmissions.CanReturnPublishedOriginalSourcePairStorage(
            sourceLoan, bodyLoan, first.release));
        bodyLoan.Release();
        sourceLoan.Release();
        EXPECT_FALSE(engine.timelineArms.RowAt(first.arm.slot)->originalLifetimePinned.load());
        ASSERT_NO_FATAL_FAILURE(ReclaimOriginalBodyArm(engine, first));
        ASSERT_NO_FATAL_FAILURE(StopOriginalBodyEpisode(engine, second));
        otherBody.Release();
        otherSource.Release();
        ASSERT_NO_FATAL_FAILURE(ReclaimOriginalBodyArm(engine, second));
        ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, first));
    }
}

TEST(OriginalSourceSubmission, PendingInstalledRetirementRunsOnActualCallbackAndPreservesForeignStorage) {
    using Result = Registry::InstalledSourceRetirementRequest;
    for (const bool returnDuringCallback : {false, true}) {
        SCOPED_TRACE(returnDuringCallback);
        harkentest::EngineSession session(SourceBodyConfig());
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        const std::vector<float> samples(kRate, 0.25f);
        SourceBody body;
        ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
        Registry::OriginalSourceLifetimePin source;
        BodyPin original, foreign;
        ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, body, true, false,
            &source, &original, false, nullptr, true));
        EXPECT_EQ(Result::Refused,
            SourceOriginAccess::RequestRetirement(engine.originalSourceSubmissions, source, original));
        ASSERT_NO_FATAL_FAILURE(PumpOriginalBodyInstall(engine, body));
        ASSERT_TRUE(original.Borrow(&foreign));
        const auto watermark = engine.mixer.BlocksRendered();
        const auto completed = engine.voices.CompletedCount();
        const auto origin = SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm);
        ASSERT_EQ(Result::Requested,
            SourceOriginAccess::RequestRetirement(engine.originalSourceSubmissions, source, original));
        ASSERT_EQ(Result::Requested,
            SourceOriginAccess::RequestRetirement(engine.originalSourceSubmissions, source, foreign));
        EXPECT_EQ(origin.epoch, SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm).epoch);
        EXPECT_FALSE(engine.voices.ReleaseObserved(body.release));
        EXPECT_EQ(completed, engine.voices.CompletedCount());
        EXPECT_FALSE(engine.timelineArms.BindPendingImmediateStart(body.arm,
            (watermark + 2u) * kFrames, (watermark + 2u) * kFrames));

        engine.voices.BeginAudioBlock(watermark);
        ASSERT_TRUE(engine.voices.ReleaseObserved(body.release));
        EXPECT_EQ(completed + 1u, engine.voices.CompletedCount());
        EXPECT_EQ(nullptr, engine.voices.Resolve(body.source));
        EXPECT_FALSE(engine.voices.IsOriginalSourceBodyCurrent(original));
        EXPECT_FALSE(engine.originalSourceSubmissions.IsOriginalInstalledSourceCurrent(source, body.source));
        const auto physical = NativeCallbackAccess::ObserveOriginalCallback(engine.voices,
            static_cast<std::uint32_t>(body.source));
        EXPECT_TRUE(physical.free);
        EXPECT_TRUE(physical.reader);
        EXPECT_TRUE(physical.actor);
        EXPECT_TRUE(physical.disposed);
        EXPECT_FALSE(physical.loanReturned);
        harken::TimelineArmObservation ended{};
        ASSERT_TRUE(engine.timelineArms.Observe(body.arm, &ended));
        EXPECT_EQ(harken::ArmState::Tombstoned, ended.state);
        EXPECT_EQ(watermark, ended.tombstoneAudioSequence);
        EXPECT_EQ(0u, ended.startedBlockSequence);
        EXPECT_EQ(Result::AlreadyReleased,
            SourceOriginAccess::RequestRetirement(engine.originalSourceSubmissions, source, original));
        VnVoiceHandle replacement = 0;
        EXPECT_EQ(VN_NO_VOICE, PrepareAtOriginalBodySlot(engine, body, &replacement));
        if (returnDuringCallback) {
            foreign.Release();
            original.Release();
            EXPECT_EQ(VN_NO_VOICE, PrepareAtOriginalBodySlot(engine, body, &replacement));
        }
        engine.voices.EndAudioBlock();
        ASSERT_NO_FATAL_FAILURE(CheckActualNativeCallbackReturned(engine, body));
        EXPECT_EQ(watermark, engine.mixer.BlocksRendered());
        if (!returnDuringCallback) {
            EXPECT_EQ(VN_NO_VOICE, PrepareAtOriginalBodySlot(engine, body, &replacement));
            original.Release();
            EXPECT_EQ(VN_NO_VOICE, PrepareAtOriginalBodySlot(engine, body, &replacement));
            foreign.Release();
        }
        ASSERT_EQ(VN_OK, PrepareAtOriginalBodySlot(engine, body, &replacement));
        EXPECT_NE(body.source, replacement);
        ASSERT_TRUE(engine.voices.CancelPreparedVoice(replacement));
        ASSERT_NO_FATAL_FAILURE(RecycleExactRetirement(engine, body));
        ASSERT_NO_FATAL_FAILURE(CloseManualNativeStopAndReclaim(engine, body, source, original));
    }
}

TEST(OriginalSourceSubmission, PendingInstalledRetirementUsesFreshMixerCallbackAndExactRelease) {
    using Result = Registry::InstalledSourceRetirementRequest;
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    Registry::OriginalSourceLifetimePin source;
    BodyPin original;
    ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, body, true, false,
        &source, &original, false, nullptr, true));
    ASSERT_NO_FATAL_FAILURE(PumpOriginalBodyInstall(engine, body));
    const auto before = engine.mixer.BlocksRendered();
    const auto completed = engine.voices.CompletedCount();
    const auto origin = SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm);
    ASSERT_EQ(Result::Requested,
        SourceOriginAccess::RequestRetirement(engine.originalSourceSubmissions, source, original));
    EXPECT_FALSE(engine.voices.ReleaseObserved(body.release));
    engine.nullBackend.Pump(1);
    ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    ASSERT_TRUE(engine.voices.ReleaseObserved(body.release));
    EXPECT_EQ(completed + 1u, engine.voices.CompletedCount());
    EXPECT_GT(SourceOriginAccess::Observe(engine.originalSourceSubmissions, body.arm).epoch, origin.epoch);
    ASSERT_NO_FATAL_FAILURE(CheckActualNativeCallbackReturned(engine, body));
    harken::TimelineArmObservation ended{};
    ASSERT_TRUE(engine.timelineArms.Observe(body.arm, &ended));
    EXPECT_EQ(harken::ArmState::Tombstoned, ended.state);
    EXPECT_EQ(before, ended.tombstoneAudioSequence);
    EXPECT_EQ(0u, ended.startedBlockSequence);
    EXPECT_FALSE(engine.timelineArms.Reclaim(body.arm, engine.mixer.BlocksRendered()));
    ASSERT_NO_FATAL_FAILURE(RecycleExactRetirement(engine, body));
    ASSERT_NO_FATAL_FAILURE(ReturnActualOriginTail(engine, body, source, original));
    engine.nullBackend.Pump(1);
    EXPECT_EQ(completed + 1u, engine.voices.CompletedCount());
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
}

TEST(OriginalSourceSubmission, PendingInstalledRetirementSealsFreshTerminalBeforeReturningReusableStorage) {
    using Result = Registry::InstalledSourceRetirementRequest;
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody body;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, body, samples));
    Registry::OriginalSourceLifetimePin source;
    BodyPin original, foreign;
    ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, body, true, false,
        &source, &original, false, nullptr, true));
    ASSERT_NO_FATAL_FAILURE(PumpOriginalBodyInstall(engine, body));
    ASSERT_TRUE(original.Borrow(&foreign));
    Registry::PendingInstalledSourceRetirementReceipt receipt;
    ASSERT_EQ(Result::Requested,
        SourceOriginAccess::RequestRetirementReceipt(engine.originalSourceSubmissions, source, original, &receipt));
    EXPECT_FALSE(receipt.StorageReturned());
    EXPECT_FALSE(SourceOriginAccess::ReturnRetirementStorage(engine.originalSourceSubmissions, receipt, body.arm));
    EXPECT_FALSE(SourceOriginAccess::RetirementSettled(engine.originalSourceSubmissions, receipt, body.arm));
    const auto before = engine.mixer.BlocksRendered();
    engine.nullBackend.Pump(1);
    ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    ASSERT_TRUE(engine.voices.ReleaseObserved(body.release));
    ASSERT_TRUE(SourceOriginAccess::ReturnRetirementStorage(engine.originalSourceSubmissions, receipt, body.arm));
    EXPECT_TRUE(receipt.Valid());
    EXPECT_TRUE(receipt.StorageReturned());
    EXPECT_TRUE(SourceOriginAccess::RetirementSettled(engine.originalSourceSubmissions, receipt, body.arm));
    auto wrong = body.arm;
    ++wrong.renderGeneration;
    EXPECT_FALSE(SourceOriginAccess::RetirementSettled(engine.originalSourceSubmissions, receipt, wrong));
    Registry foreignRegistry(engine.timelineArms, engine.voices, engine.commands, engine.effectBank);
    EXPECT_FALSE(SourceOriginAccess::RetirementSettled(foreignRegistry, receipt, body.arm));
    original.Release();
    source.Release();
    VnVoiceHandle replacement = 0;
    EXPECT_EQ(VN_NO_VOICE, PrepareAtOriginalBodySlot(engine, body, &replacement));
    foreign.Release();
    ASSERT_EQ(VN_OK, PrepareAtOriginalBodySlot(engine, body, &replacement));
    EXPECT_NE(body.source, replacement);
    ASSERT_TRUE(engine.voices.CancelPreparedVoice(replacement));
    ASSERT_TRUE(engine.timelineArms.Reclaim(body.arm, engine.mixer.BlocksRendered()));
    EXPECT_EQ(nullptr, engine.timelineArms.Resolve(body.arm));
    // The certificate survives physical storage reuse without reading the old row.
    Registry::PendingInstalledSourceRetirementReceipt moved(std::move(receipt));
    EXPECT_FALSE(receipt.Valid());
    EXPECT_FALSE(SourceOriginAccess::RetirementSettled(engine.originalSourceSubmissions, receipt, body.arm));
    EXPECT_TRUE(SourceOriginAccess::RetirementSettled(engine.originalSourceSubmissions, moved, body.arm));
    ASSERT_TRUE(SourceOriginAccess::ReturnRetirementStorage(engine.originalSourceSubmissions, moved, body.arm));
    ASSERT_NO_FATAL_FAILURE(RecycleExactRetirement(engine, body));
    SourceBody next;
    next.independent = body.independent;
    next.baseline = body.baseline;
    Registry::OriginalSourceLifetimePin nextSource;
    BodyPin nextBody;
    ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, next, false, false,
        &nextSource, &nextBody, false, nullptr, true));
    ASSERT_EQ(body.arm.slot, next.arm.slot);
    ASSERT_NE(body.arm.generation, next.arm.generation);
    EXPECT_EQ(nullptr, engine.originalSourceSubmissions.Find(body.arm));
    EXPECT_TRUE(SourceOriginAccess::RetirementSettled(engine.originalSourceSubmissions, moved, body.arm));
    EXPECT_FALSE(SourceOriginAccess::RetirementSettled(engine.originalSourceSubmissions, moved, next.arm));
    ASSERT_NO_FATAL_FAILURE(PumpOriginalBodyInstall(engine, next));
    EXPECT_EQ(Result::Refused,
        SourceOriginAccess::RequestRetirementReceipt(engine.originalSourceSubmissions, nextSource, nextBody, &moved));
    EXPECT_TRUE(SourceOriginAccess::RetirementSettled(engine.originalSourceSubmissions, moved, body.arm));
    ASSERT_NO_FATAL_FAILURE(StopOriginalBodyEpisode(engine, next));
    ASSERT_NO_FATAL_FAILURE(ReturnActualOriginTail(engine, next, nextSource, nextBody));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, body));
    moved.Release();
    EXPECT_FALSE(moved.Valid());
    EXPECT_FALSE(SourceOriginAccess::RetirementSettled(engine.originalSourceSubmissions, moved, body.arm));
}

TEST(OriginalSourceSubmission, PendingInstalledRetirementRejectsForeignPairAndBoundStart) {
    using Result = Registry::InstalledSourceRetirementRequest;
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    const std::vector<float> samples(kRate, 0.25f);
    SourceBody first, second;
    ASSERT_NO_FATAL_FAILURE(StartIndependent(engine, first, samples));
    second.independent = first.independent;
    second.baseline = first.baseline;
    Registry::OriginalSourceLifetimePin firstSource, secondSource, emptySource;
    BodyPin firstBody, secondBody, emptyBody;
    ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, first, false, false,
        &firstSource, &firstBody, false, nullptr, true));
    ASSERT_NO_FATAL_FAILURE(PumpOriginalBodyInstall(engine, first));
    ASSERT_NO_FATAL_FAILURE(PublishOriginalBodyEpisode(engine, second, false, false,
        &secondSource, &secondBody, false, nullptr, true));
    ASSERT_NO_FATAL_FAILURE(PumpOriginalBodyInstall(engine, second));
    EXPECT_EQ(Result::Refused,
        SourceOriginAccess::RequestRetirement(engine.originalSourceSubmissions, firstSource, secondBody));
    EXPECT_EQ(Result::Refused,
        SourceOriginAccess::RequestRetirement(engine.originalSourceSubmissions, secondSource, firstBody));
    EXPECT_EQ(Result::Refused,
        SourceOriginAccess::RequestRetirement(engine.originalSourceSubmissions, emptySource, firstBody));
    EXPECT_EQ(Result::Refused,
        SourceOriginAccess::RequestRetirement(engine.originalSourceSubmissions, firstSource, emptyBody));
    const auto frame = (engine.mixer.BlocksRendered() + 2u) * kFrames;
    ASSERT_TRUE(engine.timelineArms.BindPendingImmediateStart(first.arm, frame, frame));
    EXPECT_EQ(Result::Refused,
        SourceOriginAccess::RequestRetirement(engine.originalSourceSubmissions, firstSource, firstBody));
    EXPECT_FALSE(engine.voices.ReleaseObserved(first.release));
    EXPECT_FALSE(engine.voices.ReleaseObserved(second.release));
    ASSERT_NO_FATAL_FAILURE(StopOriginalBodyEpisode(engine, first));
    ASSERT_NO_FATAL_FAILURE(StopOriginalBodyEpisode(engine, second));
    ASSERT_NO_FATAL_FAILURE(ReturnActualOriginTail(engine, first, firstSource, firstBody));
    ASSERT_NO_FATAL_FAILURE(ReturnActualOriginTail(engine, second, secondSource, secondBody));
    ASSERT_NO_FATAL_FAILURE(RetireIndependent(engine, first));
}

TEST(OriginalSourceSubmission, NativeUnpublishedDispositionRetainsRealForeignSourceLoanAfterPhysicalExit) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult()); auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterNativeDispositionPcm());
    NativeUnpublishedDispositionSource source;
    ASSERT_NO_FATAL_FAILURE(source.Prepare(engine, true, false));
    Registry::OriginalSourceLifetimePin foreign;
    ASSERT_TRUE(source.original.Borrow(&foreign));
    ASSERT_NO_FATAL_FAILURE(source.Dispose(engine));
    ASSERT_NE(nullptr, engine.originalSourceSubmissions.OriginalUnpublishedDisposition(source.arm));
    source.original.Release();
    EXPECT_TRUE(foreign.Valid());
    EXPECT_TRUE(engine.timelineArms.RowAt(source.arm.slot)->originalLifetimePinned.load());
    EXPECT_FALSE(engine.timelineArms.CancelPrepared(source.arm));
    foreign.Release();
    EXPECT_FALSE(engine.timelineArms.RowAt(source.arm.slot)->originalLifetimePinned.load());
    ASSERT_TRUE(engine.timelineArms.CancelPrepared(source.arm));
    EXPECT_EQ(0u, engine.mixer.BlocksRendered());
}

TEST(OriginalSourceSubmission, NativeUnpublishedDispositionRejectsPublishedSourceWithoutChangingStrictFence) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult()); auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterNativeDispositionPcm());
    NativeUnpublishedDispositionSource source;
    ASSERT_NO_FATAL_FAILURE(source.Prepare(engine, true));
    engine.originalSourceSubmissions.PublishPreparedChild(source.body[0]);
    (void)engine.commands.ConsumePreparedBatch(std::move(source.batch));
    EXPECT_FALSE(engine.originalSourceSubmissions.DisposeUnpublishedVoice(source.arm));
    EXPECT_FALSE(engine.originalSourceSubmissions.WithdrawUnpublished(source.arm));
    EXPECT_EQ(nullptr, engine.originalSourceSubmissions.OriginalUnpublishedDisposition(source.arm));
    ASSERT_TRUE(engine.timelineArms.RequestCancel(source.arm));
    const auto before = engine.mixer.BlocksRendered(); engine.nullBackend.Pump(1);
    ASSERT_EQ(before + 1u, engine.mixer.BlocksRendered());
    ASSERT_TRUE(engine.voices.ReleaseObserved(source.release)); source.original.Release();
    EXPECT_EQ(nullptr, engine.originalSourceSubmissions.OriginalUnpublishedDisposition(source.arm));
    harken::TimelineArmObservation observed{};
    ASSERT_TRUE(engine.timelineArms.Observe(source.arm, &observed));
    ASSERT_EQ(harken::ArmState::Tombstoned, observed.state);
    EXPECT_FALSE(engine.timelineArms.Reclaim(source.arm, observed.tombstoneAudioSequence));
    const auto completed = engine.mixer.BlocksRendered(); engine.nullBackend.Pump(1);
    ASSERT_EQ(completed + 1u, engine.mixer.BlocksRendered());
    ASSERT_TRUE(engine.timelineArms.Reclaim(source.arm, engine.mixer.BlocksRendered()));
}

TEST(OriginalSourceSubmission, NativeUnpublishedDispositionDoesNotPromoteMissingOrLegacyZeroIdentity) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult()); auto& engine = *session.Engine();
    const harken::TimelineArmToken absent{0, 77, 0, 99};
    ASSERT_TRUE(engine.originalSourceSubmissions.WithdrawUnpublished(absent));
    EXPECT_EQ(nullptr, engine.originalSourceSubmissions.OriginalUnpublishedDisposition(absent));
    ASSERT_NO_FATAL_FAILURE(RegisterNativeDispositionPcm());
    NativeUnpublishedDispositionSource source;
    ASSERT_NO_FATAL_FAILURE(source.Prepare(engine, false, false, false));
    ASSERT_NO_FATAL_FAILURE(source.Dispose(engine)); source.original.Release();
    EXPECT_EQ(nullptr, engine.originalSourceSubmissions.OriginalUnpublishedDisposition(source.arm));
    ASSERT_TRUE(engine.timelineArms.CancelPrepared(source.arm));
}

TEST(OriginalSourceSubmission, NativeUnpublishedDispositionNewFactoryReplacesActualRecordWithNewEpisode) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult()); auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterNativeDispositionPcm());
    NativeUnpublishedDispositionSource oldSource;
    ASSERT_NO_FATAL_FAILURE(oldSource.Prepare(engine, false, false));
    const auto oldEpisode = engine.timelineArms.RowAt(oldSource.arm.slot)->originalSourceEpisode;
    ASSERT_NE(0u, oldEpisode); ASSERT_NO_FATAL_FAILURE(oldSource.Dispose(engine));
    ASSERT_NE(nullptr, engine.originalSourceSubmissions.OriginalUnpublishedDisposition(oldSource.arm));
    oldSource.original.Release(); ASSERT_TRUE(engine.timelineArms.CancelPrepared(oldSource.arm));
    NativeUnpublishedDispositionSource current;
    ASSERT_NO_FATAL_FAILURE(current.Prepare(engine, false, false));
    ASSERT_EQ(oldSource.arm.slot, current.arm.slot);
    ASSERT_NE(oldSource.arm.generation, current.arm.generation);
    EXPECT_NE(oldEpisode, engine.timelineArms.RowAt(current.arm.slot)->originalSourceEpisode);
    EXPECT_EQ(nullptr, engine.originalSourceSubmissions.OriginalUnpublishedDisposition(oldSource.arm));
    EXPECT_EQ(nullptr, engine.originalSourceSubmissions.OriginalUnpublishedDisposition(current.arm));
    ASSERT_NO_FATAL_FAILURE(current.Dispose(engine));
    const auto* actual = engine.originalSourceSubmissions.OriginalUnpublishedDisposition(current.arm);
    ASSERT_NE(nullptr, actual); EXPECT_TRUE(Registry::SameToken(actual->Token(), current.arm));
    current.original.Release(); ASSERT_TRUE(engine.timelineArms.CancelPrepared(current.arm));
}

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
TEST(OriginalSourceSubmission, NativeUnpublishedDispositionLastEpisodeIssuesOnceThenRefusesBeforeOwnershipMutation) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult()); auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterNativeDispositionPcm());
    harken::detail::OriginalSourceMutationActorTestAccess::ExhaustNativeSourceEpisodeAfterNext(
        engine.originalSourceSubmissions);
    NativeUnpublishedDispositionSource last;
    ASSERT_NO_FATAL_FAILURE(last.Prepare(engine, false, false));
    ASSERT_EQ(UINT64_MAX, engine.timelineArms.RowAt(last.arm.slot)->originalSourceEpisode);
    ASSERT_NO_FATAL_FAILURE(last.Dispose(engine));
    ASSERT_NE(nullptr, engine.originalSourceSubmissions.OriginalUnpublishedDisposition(last.arm));
    last.original.Release(); ASSERT_TRUE(engine.timelineArms.CancelPrepared(last.arm));
    const auto* oldRecord = engine.originalSourceSubmissions.Find(last.arm);
    ASSERT_NE(nullptr, oldRecord); ASSERT_EQ(Registry::Phase::Disposed, oldRecord->State());
    NativeUnpublishedDispositionSource refused;
    ASSERT_NO_FATAL_FAILURE(refused.Prepare(engine, false, false, true, false));
    EXPECT_FALSE(refused.original.Valid());
    EXPECT_EQ(nullptr, engine.originalSourceSubmissions.Find(refused.arm));
    EXPECT_EQ(oldRecord, engine.originalSourceSubmissions.Find(last.arm));
    EXPECT_EQ(Registry::Phase::Disposed, oldRecord->State());
    EXPECT_EQ(nullptr, engine.timelineArms.RowAt(refused.arm.slot)->originalSourceSubmission);
    EXPECT_EQ(0u, engine.timelineArms.RowAt(refused.arm.slot)->originalSourceEpisode);
    EXPECT_FALSE(engine.timelineArms.RowAt(refused.arm.slot)->originalLifetimePinned.load());
    EXPECT_EQ(nullptr, engine.originalSourceSubmissions.OriginalUnpublishedDisposition(refused.arm));
    ASSERT_TRUE(engine.voices.CancelPreparedVoice(refused.render));
    ASSERT_TRUE(engine.voices.ReleaseObserved(refused.release));
    ASSERT_TRUE(engine.timelineArms.CancelPrepared(refused.arm));
    EXPECT_EQ(0u, engine.mixer.BlocksRendered()); EXPECT_EQ(0u, engine.commands.PublishedHead());
}
#endif


namespace {
using JointStatus = harken::UnpublishedGroupArmCancellationStatus;
struct NativeJointCancellation {
    NativeUnpublishedDispositionSource source;
    std::uint64_t startFrame = 0;
    harken::DecisionToken decision{};
    std::array<harken::CommittedPlan03GroupSlice, 2> groups;
    std::array<harken::CommittedPlan03GroupSlice*, 2> originals{&groups[0], &groups[1]};
    std::array<harken::Plan03GroupSliceIdentity, 2> identities{};
    std::array<harken::UnpublishedGroupCancellationHold, 2> groupStorage;
    std::array<harken::UnpublishedArmCancellationHold, 1> armStorage;
    harken::UnpublishedGroupArmCancellationProof proof;

    void Prepare(harken::Engine& engine, harken::ScheduledTransactionPool& pool,
        std::uint32_t count = 2, bool bindRange = true, bool captureBody = false) {
        ASSERT_NO_FATAL_FAILURE(RegisterNativeDispositionPcm());
        if (engine.variables.CurrentWorldGeneration() == 0) {
            const harken::VariableDefinitionV1 definition{
                harkentest::ObjectId(0x791), harken::VariableKindV1::Step, 0.0f, 1.0f, 0.0f};
            ASSERT_EQ(harken::WorldInstallStatus::Ok,
                harkentest::InstallDefinitionsWorld(&engine, &definition, 1,
                    UINT64_C(0x4A4F494E54534F55)).status);
        }
        harken::DecisionReserveRequest request{};
        request.ticket.sequence = engine.logicCommits.Issue();
        request.world = engine.variables.CurrentWorldGeneration();
        request.transport = pool.Epoch(); request.expectedContinuationGeneration = 1;
        request.sliceCount = 2;
        ASSERT_TRUE(engine.decisions.Reserve(request, &decision));
        startFrame = pool.PublishCutoffFrame();
        ASSERT_NO_FATAL_FAILURE(source.Prepare(engine, bindRange, true, true, true, {decision}, captureBody, startFrame));
        for (std::uint32_t index = 0; index < count; ++index) {
            harken::Plan03GroupSliceInput input{};
            input.executeFrame = startFrame; input.decision = decision; input.sliceIndex = index;
            input.members = &source.arm; input.memberCount = 1;
            harken::PreparedPlan03GroupSlice prepared;
            ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
                pool.PreparePlan03GroupSlice(input, engine.timelineArms, &prepared));
            ASSERT_TRUE(pool.CommitPreparedPlan03GroupSlice(&prepared, &groups[index]));
        }
        std::array<harken::Plan03PageGroupView, 2> actual{};
        ASSERT_EQ(count, pool.CaptureLivePlan03Groups(actual.data(), actual.size()));
        for (std::uint32_t index = 0; index < count; ++index) {
            ASSERT_TRUE(actual[index].identity.Exact());
            ASSERT_EQ(decision.slot, actual[index].identity.decision.slot);
            ASSERT_TRUE(Registry::SameToken(source.arm, actual[index].members[0]));
            identities[actual[index].identity.sliceIndex] = actual[index].identity;
        }
    }
    JointStatus Begin(harken::Engine& engine, harken::ScheduledTransactionPool& pool, std::size_t count = 2) {
        return pool.BeginUnpublishedGroupArmCancellation(std::span(originals.data(), count),
            engine.timelineArms, groupStorage, armStorage, &proof);
    }
    JointStatus Finish(harken::Engine& engine, harken::ScheduledTransactionPool& pool) {
        const std::array sources{engine.originalSourceSubmissions.OriginalUnpublishedDisposition(source.arm)};
        return pool.FinishUnpublishedGroupArmCancellation(proof, sources);
    }
    void Dispose(harken::Engine& engine) {
        ASSERT_NO_FATAL_FAILURE(source.Dispose(engine));
        source.original.Release(); source.originalBody.Release();
        ASSERT_TRUE(engine.voices.ReleaseObserved(source.release));
    }
};

harken::ScheduledTransactionPool::Config NativeJointForeignConfig() {
    harken::ScheduledTransactionPool::Config config{};
    config.quantumFrames = kFrames; config.scheduleHorizonFrames = kFrames * 32;
    config.publishLeadQuanta = 1; config.maxScheduledTransactions = 4;
    config.maxTransactionSpanFrames = kFrames * 2; config.maxIngressTransactionsPerQuantum = 4;
    config.maxFrameBatchesPerQuantum = 4; config.maxEventsPerQuantum = 8;
    config.maxTransactionEvents = 4; config.maxCurrentSlicesPerQuantum = 4;
    config.maxPlan03GroupHeadersPerPage = 4; config.maxArmsPerGroup = 4;
    config.maxImmediateLaneGroupRows = 2; config.maxImmediateLaneOwnerGates = 4;
    config.maxImmediateLaneDecisionGates = 4;
    return config;
}
}

TEST(OriginalSourceSubmission, NativeJointCancellationConsumesTwoRealSlicesAndOneArmWithoutAudioFence) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult()); auto& engine = *session.Engine();
    const auto free = engine.timelineArms.FreeCount();
    NativeJointCancellation actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare(engine, engine.timelinePool));
    ASSERT_EQ(JointStatus::Held, actual.Begin(engine, engine.timelinePool));
    EXPECT_FALSE(engine.timelinePool.RollbackCommittedPlan03GroupSlice(&actual.groups[0]));
    EXPECT_FALSE(engine.timelinePool.ReleaseCommittedPlan03GroupSliceOwnership(&actual.groups[1]));
    EXPECT_FALSE(engine.timelineArms.RequestCancel(actual.source.arm));
    ASSERT_NO_FATAL_FAILURE(actual.Dispose(engine));
    ASSERT_EQ(JointStatus::Consumed, actual.Finish(engine, engine.timelinePool));
    EXPECT_FALSE(actual.proof.Valid()); EXPECT_TRUE(actual.proof.ConsumedOriginalArm(actual.source.arm));
    EXPECT_FALSE(actual.groups[0].Valid()); EXPECT_FALSE(actual.groups[1].Valid());
    EXPECT_EQ(free, engine.timelineArms.FreeCount());
    std::array<harken::Plan03PageGroupView, 2> remaining{};
    EXPECT_EQ(0u, engine.timelinePool.CaptureLivePlan03Groups(remaining.data(), remaining.size()));
    EXPECT_EQ(0u, engine.mixer.BlocksRendered()); EXPECT_EQ(0u, engine.commands.PublishedHead());
    EXPECT_EQ(0u, engine.timelineArms.RowAt(actual.source.arm.slot)->armedCompletionSequence.load());
    EXPECT_EQ(0u, engine.timelineArms.RowAt(actual.source.arm.slot)->tombstoneAudioSequence.load());
    EXPECT_EQ(nullptr, engine.timelineArms.RowAt(actual.source.arm.slot)->originalSourceSubmission);
    EXPECT_EQ(JointStatus::NotEligible, actual.Finish(engine, engine.timelinePool));
    auto stale = actual.source.arm; ++stale.renderGeneration;
    EXPECT_FALSE(actual.proof.ConsumedOriginalArm(stale));
    ASSERT_TRUE(engine.decisions.Cancel(actual.decision));
}

TEST(OriginalSourceSubmission, NativeJointCancellationBusyRetainsTruePublicationProtectionAcrossUpdateAndBothLoans) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult()); auto& engine = *session.Engine();
    NativeJointCancellation actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare(engine, engine.timelinePool, 2, true, true));
    Registry::OriginalSourceLifetimePin sourceLoan;
    harken::VoicePool::OriginalSourceBodyLifetimePin bodyLoan;
    ASSERT_TRUE(actual.source.original.Borrow(&sourceLoan));
    ASSERT_TRUE(actual.source.originalBody.Borrow(&bodyLoan));
    ASSERT_EQ(JointStatus::Held, actual.Begin(engine, engine.timelinePool));
    ASSERT_NO_FATAL_FAILURE(actual.Dispose(engine));
    EXPECT_EQ(JointStatus::Busy, actual.Finish(engine, engine.timelinePool));
    actual.proof.Release();
    EXPECT_TRUE(actual.groups[0].Held()); EXPECT_TRUE(actual.groups[1].Held());
    ASSERT_EQ(VN_OK, Vn_Update());
    engine.timelinePool.Publish(actual.startFrame + kFrames, nullptr, nullptr, &engine.timelineArms);
    for (const auto& identity : actual.identities) {
        harken::Plan03ExactGroupSnapshot group{};
        ASSERT_TRUE(engine.timelinePool.CaptureExactPlan03Group(identity, &group));
        EXPECT_FALSE(group.page.frozen); EXPECT_TRUE(group.publicationHeld);
        EXPECT_EQ(UINT64_MAX, group.pageStamp); EXPECT_EQ(UINT64_MAX, group.emptyPageStamp);
        EXPECT_EQ(harken::AudioSliceState::Ready, group.effectiveLease.state);
        harken::Plan03GroupReaderPin forbidden;
        EXPECT_FALSE(engine.timelinePool.AcquirePlan03GroupReader(identity, &forbidden));
    }
    EXPECT_EQ(0u, engine.timelinePool.Advance());
    EXPECT_FALSE(engine.timelinePool.CanResetForEpoch(0, engine.timelinePool.Epoch() + 1));
    sourceLoan.Release();
    ASSERT_EQ(JointStatus::Held, actual.Begin(engine, engine.timelinePool));
    EXPECT_EQ(JointStatus::Busy, actual.Finish(engine, engine.timelinePool));
    bodyLoan.Release();
    ASSERT_EQ(JointStatus::Consumed, actual.Finish(engine, engine.timelinePool));
    EXPECT_TRUE(actual.proof.ConsumedOriginalArm(actual.source.arm));
    EXPECT_EQ(0u, engine.mixer.BlocksRendered());
    ASSERT_TRUE(engine.decisions.Cancel(actual.decision));
}

TEST(OriginalSourceSubmission, NativeJointCancellationRequiresEveryActualMemberAndReturnsAllFailedPrefixGates) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult()); auto& engine = *session.Engine();
    NativeJointCancellation actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare(engine, engine.timelinePool));
    EXPECT_EQ(JointStatus::Busy, actual.Begin(engine, engine.timelinePool, 1));
    harken::Plan03ExactGroupSnapshot before{};
    ASSERT_TRUE(engine.timelinePool.CaptureExactPlan03Group(actual.identities[0], &before));
    EXPECT_FALSE(before.publicationHeld); EXPECT_FALSE(actual.groups[0].Held());
    std::array<harken::UnpublishedArmCancellationHold, 0> absent;
    EXPECT_EQ(JointStatus::StorageTooSmall, engine.timelinePool.BeginUnpublishedGroupArmCancellation(
        actual.originals, engine.timelineArms, actual.groupStorage, absent, &actual.proof));
    harken::Plan03GroupReaderPin reader;
    ASSERT_TRUE(engine.timelinePool.AcquirePlan03GroupReader(actual.identities[1], &reader));
    EXPECT_EQ(JointStatus::Busy, actual.Begin(engine, engine.timelinePool));
    ASSERT_TRUE(reader.Release(0));
    auto* row = engine.timelineArms.RowAt(actual.source.arm.slot);
    ASSERT_FALSE(row->reservation.test_and_set());
    EXPECT_EQ(JointStatus::Busy, actual.Begin(engine, engine.timelinePool));
    row->reservation.clear();
    ASSERT_TRUE(engine.timelinePool.CaptureExactPlan03Group(actual.identities[0], &before));
    EXPECT_EQ(harken::AudioSliceState::Ready, before.effectiveLease.state); EXPECT_FALSE(before.publicationHeld);
    ASSERT_EQ(JointStatus::Held, actual.Begin(engine, engine.timelinePool));
    actual.proof.Release();
    ASSERT_TRUE(engine.timelinePool.RollbackCommittedPlan03GroupSlice(&actual.groups[1]));
    ASSERT_TRUE(engine.timelinePool.RollbackCommittedPlan03GroupSlice(&actual.groups[0]));
    ASSERT_NO_FATAL_FAILURE(actual.Dispose(engine));
    ASSERT_TRUE(engine.timelineArms.RequestCancel(actual.source.arm));
    engine.nullBackend.Pump(1);
    ASSERT_EQ(harken::ArmState::Tombstoned, row->state.load());
    const auto nativeRetirement = row->tombstoneAudioSequence.load();
    EXPECT_FALSE(engine.timelineArms.Reclaim(actual.source.arm, nativeRetirement));
    engine.nullBackend.Pump(1);
    ASSERT_GT(engine.mixer.BlocksRendered(), nativeRetirement);
    ASSERT_TRUE(engine.timelineArms.Reclaim(actual.source.arm, engine.mixer.BlocksRendered()));
    ASSERT_TRUE(engine.decisions.Cancel(actual.decision));
}

TEST(OriginalSourceSubmission, NativeJointCancellationForeignPoolObligationPreventsPartialFastExit) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult()); auto& engine = *session.Engine();
    NativeJointCancellation actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare(engine, engine.timelinePool, 1));
    harken::ScheduledTransactionPool foreign;
    ASSERT_TRUE(foreign.Reset(NativeJointForeignConfig(), 0, engine.timelinePool.Epoch()));
    harken::Plan03GroupSliceInput input{};
    input.executeFrame = actual.startFrame; input.decision = actual.decision; input.sliceIndex = 1;
    input.members = &actual.source.arm; input.memberCount = 1;
    harken::PreparedPlan03GroupSlice prepared; harken::CommittedPlan03GroupSlice committed;
    ASSERT_EQ(harken::Plan03GroupReserveStatus::Committed,
        foreign.PreparePlan03GroupSlice(input, engine.timelineArms, &prepared));
    ASSERT_TRUE(foreign.CommitPreparedPlan03GroupSlice(&prepared, &committed));
    EXPECT_EQ(JointStatus::Busy, actual.Begin(engine, engine.timelinePool, 1));
    EXPECT_FALSE(actual.proof.Valid()); EXPECT_TRUE(committed.Valid());
    ASSERT_TRUE(foreign.RollbackCommittedPlan03GroupSlice(&committed));
    ASSERT_EQ(JointStatus::Held, actual.Begin(engine, engine.timelinePool, 1));
    ASSERT_NO_FATAL_FAILURE(actual.Dispose(engine));
    ASSERT_EQ(JointStatus::Consumed, actual.Finish(engine, engine.timelinePool));
    ASSERT_TRUE(engine.decisions.Cancel(actual.decision));
}

TEST(OriginalSourceSubmission, NativeJointCancellationRequiresAuthenticSourceReceiptAndKeepsNeverBoundBranch) {
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult()); auto& engine = *session.Engine();
    NativeJointCancellation actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare(engine, engine.timelinePool, 1, false));
    ASSERT_EQ(JointStatus::Held, actual.Begin(engine, engine.timelinePool, 1));
    harken::TimelineArmTable::OriginalUnpublishedSourceDisposition empty;
    const std::array absent{&empty};
    EXPECT_EQ(JointStatus::Busy, engine.timelinePool.FinishUnpublishedGroupArmCancellation(actual.proof, absent));
    EXPECT_TRUE(actual.groups[0].Valid());
    ASSERT_NO_FATAL_FAILURE(actual.Dispose(engine));
    EXPECT_FALSE(engine.timelineArms.RowAt(actual.source.arm.slot)->originalLifetimePinned.load());
    EXPECT_EQ(JointStatus::Busy, engine.timelinePool.FinishUnpublishedGroupArmCancellation(actual.proof, absent));
    EXPECT_TRUE(actual.groups[0].Valid());
    ASSERT_EQ(JointStatus::Consumed, actual.Finish(engine, engine.timelinePool));
    EXPECT_EQ(0u, engine.commands.PublishedHead()); EXPECT_EQ(0u, engine.mixer.BlocksRendered());
    ASSERT_TRUE(engine.decisions.Cancel(actual.decision));
}

TEST(OriginalSourceSubmission, NativeJointCancellationBothRealReceiverDestructionOrdersReleaseOnlyTemporaryProof) {
    for (bool poolFirst : {true, false}) {
        auto session = std::make_unique<harkentest::EngineSession>(SourceBodyConfig());
        ASSERT_EQ(VN_OK, session->InitResult()); auto& engine = *session->Engine();
        auto pool = std::make_unique<harken::ScheduledTransactionPool>();
        ASSERT_TRUE(pool->Reset(NativeJointForeignConfig(), 0, engine.timelinePool.Epoch()));
        NativeJointCancellation actual;
        ASSERT_NO_FATAL_FAILURE(actual.Prepare(engine, *pool, 1));
        ASSERT_EQ(JointStatus::Held, actual.Begin(engine, *pool, 1));
        ASSERT_NO_FATAL_FAILURE(actual.Dispose(engine));
        ASSERT_TRUE(engine.decisions.Cancel(actual.decision));
        if (poolFirst) {
            pool.reset();
            EXPECT_FALSE(actual.proof.Valid()); EXPECT_FALSE(actual.groups[0].Valid());
            EXPECT_FALSE(actual.proof.ConsumedOriginalArm(actual.source.arm));
            EXPECT_EQ(harken::ArmState::PendingApply, engine.timelineArms.RowAt(actual.source.arm.slot)->state.load());
            session.reset();
        } else {
            session.reset();
            EXPECT_FALSE(actual.proof.Valid()); EXPECT_FALSE(actual.proof.ConsumedOriginalArm(actual.source.arm));
            pool.reset(); EXPECT_FALSE(actual.groups[0].Valid());
        }
        actual.proof.Release();
    }
}

TEST(OriginalSourceSubmission, NativeUnparentedExitAcceptsActualPreparedFactoriesAndBothRangeOriginsOnce) {
    using Status = harken::TimelineArmTable::UnparentedUnpublishedSourceRetirement;
    for (const bool pending : {false, true}) for (const bool bind : {false, true}) {
        SCOPED_TRACE(pending);
        SCOPED_TRACE(bind);
        harkentest::EngineSession session(SourceBodyConfig());
        ASSERT_EQ(VN_OK, session.InitResult()); auto& engine = *session.Engine();
        ASSERT_NO_FATAL_FAILURE(RegisterNativeDispositionPcm());
        const auto free = engine.timelineArms.FreeCount();
        NativeUnpublishedDispositionSource source;
        ASSERT_NO_FATAL_FAILURE(source.Prepare(engine, bind, true, true, true, {}, false, 0, pending));
        ASSERT_NO_FATAL_FAILURE(source.Dispose(engine)); source.original.Release();
        const auto* actual = engine.originalSourceSubmissions.OriginalUnpublishedDisposition(source.arm);
        ASSERT_NE(nullptr, actual);
        auto foreign = std::make_unique<harken::Engine>(engine.config);
        harken::TimelineArmToken foreignArm{};
        harken::TimelineSemanticTarget foreignTarget{}; foreignTarget.subjectId = kSound;
        ASSERT_TRUE(foreign->timelineArms.Prepare(source.arm.renderSlot, source.arm.renderGeneration,
            0, 0, 0, foreignTarget, &foreignArm));
        EXPECT_TRUE(Registry::SameToken(source.arm, foreignArm));
        EXPECT_EQ(Status::NotEligible, foreign->timelineArms.ConsumeOriginalUnparentedUnpublishedSource(*actual));
        ASSERT_TRUE(foreign->timelineArms.CancelPrepared(foreignArm));
        harken::TimelineArmTable::OriginalUnpublishedSourceDisposition empty;
        EXPECT_EQ(Status::NotEligible, engine.timelineArms.ConsumeOriginalUnparentedUnpublishedSource(empty));
        ASSERT_EQ(Status::Consumed, engine.timelineArms.ConsumeOriginalUnparentedUnpublishedSource(*actual));
        EXPECT_EQ(Status::NotEligible, engine.timelineArms.ConsumeOriginalUnparentedUnpublishedSource(*actual));
        EXPECT_EQ(free, engine.timelineArms.FreeCount());
        EXPECT_EQ(0u, engine.mixer.BlocksRendered()); EXPECT_EQ(0u, engine.commands.PublishedHead());
        EXPECT_EQ(nullptr, engine.timelineArms.Resolve(source.arm));
        const auto* row = engine.timelineArms.RowAt(source.arm.slot);
        EXPECT_EQ(harken::ArmState::Free, row->state.load());
        EXPECT_EQ(source.arm.generation + 1u, row->generation);
        EXPECT_EQ(0u, row->armedCompletionSequence.load()); EXPECT_EQ(0u, row->tombstoneAudioSequence.load());
        EXPECT_EQ(nullptr, row->originalSourceSubmission);
        NativeUnpublishedDispositionSource next;
        ASSERT_NO_FATAL_FAILURE(next.Prepare(engine, bind));
        EXPECT_EQ(source.arm.slot, next.arm.slot); EXPECT_NE(source.arm.generation, next.arm.generation);
        ASSERT_NO_FATAL_FAILURE(next.Dispose(engine)); next.original.Release();
        const auto* fresh = engine.originalSourceSubmissions.OriginalUnpublishedDisposition(next.arm);
        ASSERT_NE(nullptr, fresh);
        ASSERT_EQ(Status::Consumed, engine.timelineArms.ConsumeOriginalUnparentedUnpublishedSource(*fresh));
    }
}

TEST(OriginalSourceSubmission, NativeUnparentedExitRequiresBothRealLoansAfterPhysicalExitAcrossUpdate) {
    using Status = harken::TimelineArmTable::UnparentedUnpublishedSourceRetirement;
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult()); auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterNativeDispositionPcm());
    NativeUnpublishedDispositionSource source;
    ASSERT_NO_FATAL_FAILURE(source.Prepare(engine, true, true, true, true, {}, true, 0, true));
    Registry::OriginalSourceLifetimePin sourceLoan;
    harken::VoicePool::OriginalSourceBodyLifetimePin bodyLoan;
    ASSERT_TRUE(source.original.Borrow(&sourceLoan)); ASSERT_TRUE(source.originalBody.Borrow(&bodyLoan));
    ASSERT_NO_FATAL_FAILURE(source.Dispose(engine)); source.original.Release(); source.originalBody.Release();
    const auto* actual = engine.originalSourceSubmissions.OriginalUnpublishedDisposition(source.arm);
    ASSERT_NE(nullptr, actual);
    const auto free = engine.timelineArms.FreeCount();
    EXPECT_EQ(Status::Busy, engine.timelineArms.ConsumeOriginalUnparentedUnpublishedSource(*actual));
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_NE(nullptr, engine.timelineArms.Resolve(source.arm));
    const auto* row = engine.timelineArms.Resolve(source.arm);
    EXPECT_EQ(harken::ArmState::PendingApply, row->state.load());
    EXPECT_TRUE(row->pendingImmediateStart); EXPECT_FALSE(row->startBound);
    EXPECT_EQ(0u, row->armedCompletionSequence.load()); EXPECT_EQ(0u, row->tombstoneAudioSequence.load());
    EXPECT_TRUE(engine.voices.ReleaseObserved(source.release)); EXPECT_EQ(free, engine.timelineArms.FreeCount());
    sourceLoan.Release();
    EXPECT_EQ(Status::Busy, engine.timelineArms.ConsumeOriginalUnparentedUnpublishedSource(*actual));
    bodyLoan.Release();
    ASSERT_EQ(Status::Consumed, engine.timelineArms.ConsumeOriginalUnparentedUnpublishedSource(*actual));
    EXPECT_EQ(free + 1u, engine.timelineArms.FreeCount());
    EXPECT_EQ(0u, engine.mixer.BlocksRendered()); EXPECT_EQ(0u, engine.commands.PublishedHead());
}

TEST(OriginalSourceSubmission, NativeUnparentedExitNeverPromotesGroupedFactoryAfterAllActualGroupsReturn) {
    using Status = harken::TimelineArmTable::UnparentedUnpublishedSourceRetirement;
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult()); auto& engine = *session.Engine();
    NativeJointCancellation actual;
    ASSERT_NO_FATAL_FAILURE(actual.Prepare(engine, engine.timelinePool));
    for (std::size_t index = actual.groups.size(); index-- > 0;)
        ASSERT_TRUE(engine.timelinePool.RollbackCommittedPlan03GroupSlice(&actual.groups[index]));
    ASSERT_NO_FATAL_FAILURE(actual.Dispose(engine));
    const auto* original = engine.originalSourceSubmissions.OriginalUnpublishedDisposition(actual.source.arm);
    ASSERT_NE(nullptr, original);
    EXPECT_EQ(Status::NotEligible, engine.timelineArms.ConsumeOriginalUnparentedUnpublishedSource(*original));
    ASSERT_NE(nullptr, engine.timelineArms.Resolve(actual.source.arm));
    ASSERT_TRUE(engine.timelineArms.RequestCancel(actual.source.arm));
    engine.nullBackend.Pump(1);
    harken::TimelineArmObservation observed{};
    ASSERT_TRUE(engine.timelineArms.Observe(actual.source.arm, &observed));
    ASSERT_EQ(harken::ArmState::Tombstoned, observed.state);
    const auto fence = observed.tombstoneAudioSequence;
    EXPECT_FALSE(engine.timelineArms.Reclaim(actual.source.arm, fence));
    engine.nullBackend.Pump(1);
    ASSERT_GT(engine.mixer.BlocksRendered(), fence);
    ASSERT_TRUE(engine.timelineArms.Reclaim(actual.source.arm, engine.mixer.BlocksRendered()));
    ASSERT_TRUE(engine.decisions.Cancel(actual.decision));
}


TEST(OriginalSourceSubmission, NativeUnparentedExitWaitsForGenuineFrontierReaderReturnWithoutAudioProgress) {
    using Status = harken::TimelineArmTable::UnparentedUnpublishedSourceRetirement;
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult()); auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterNativeDispositionPcm());
    NativeUnpublishedDispositionSource source;
    ASSERT_NO_FATAL_FAILURE(source.Prepare(engine, true, true, true, true, {}, false, 0, true));
    harken::TimelineArmReaderPin reader;
    ASSERT_TRUE(engine.timelineArms.PinFrontierReader(source.arm, &reader));
    ASSERT_NO_FATAL_FAILURE(source.Dispose(engine)); source.original.Release();
    const auto* actual = engine.originalSourceSubmissions.OriginalUnpublishedDisposition(source.arm);
    ASSERT_NE(nullptr, actual);
    const auto free = engine.timelineArms.FreeCount();
    EXPECT_EQ(Status::Busy, engine.timelineArms.ConsumeOriginalUnparentedUnpublishedSource(*actual));
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_TRUE(reader.Valid());
    harken::TimelineArmObservation observed{};
    ASSERT_TRUE(engine.timelineArms.Observe(source.arm, &observed));
    EXPECT_EQ(harken::ArmState::PendingApply, observed.state);
    EXPECT_EQ(free, engine.timelineArms.FreeCount());
    EXPECT_EQ(0u, engine.timelineArms.RowAt(source.arm.slot)->frontierLastReadSequence.load());
    ASSERT_TRUE(reader.Release(engine.mixer.BlocksRendered()));
    ASSERT_EQ(Status::Consumed, engine.timelineArms.ConsumeOriginalUnparentedUnpublishedSource(*actual));
    EXPECT_FALSE(reader.Valid()); EXPECT_EQ(free + 1u, engine.timelineArms.FreeCount());
    EXPECT_FALSE(engine.timelineArms.PinFrontierReader(source.arm, &reader));
    EXPECT_EQ(0u, engine.mixer.BlocksRendered()); EXPECT_EQ(0u, engine.commands.PublishedHead());
}

TEST(OriginalSourceSubmission, NativeUnparentedExitQuarantinesActualLastGenerationAndNextFactoryUsesAnotherRow) {
    using Status = harken::TimelineArmTable::UnparentedUnpublishedSourceRetirement;
    harkentest::EngineSession session(SourceBodyConfig());
    ASSERT_EQ(VN_OK, session.InitResult()); auto& engine = *session.Engine();
    ASSERT_NO_FATAL_FAILURE(RegisterNativeDispositionPcm());
    const auto free = engine.timelineArms.FreeCount();
    ASSERT_GT(free, 1u);
    ASSERT_TRUE(engine.timelineArms.SetFreeGenerationForTests(0, UINT32_MAX));
    auto* finalRow = engine.timelineArms.RowAt(0);
    ASSERT_NE(nullptr, finalRow);
    ASSERT_EQ(harken::ArmState::Free, finalRow->state.load());
    ASSERT_EQ(0u, finalRow->identity.load());
    NativeUnpublishedDispositionSource source;
    ASSERT_NO_FATAL_FAILURE(source.Prepare(engine, true, true, true, true, {}, false, 0, true));
    ASSERT_EQ(0u, source.arm.slot); ASSERT_EQ(UINT32_MAX, source.arm.generation);
    ASSERT_NO_FATAL_FAILURE(source.Dispose(engine)); source.original.Release();
    const auto* actual = engine.originalSourceSubmissions.OriginalUnpublishedDisposition(source.arm);
    ASSERT_NE(nullptr, actual);
    ASSERT_EQ(Status::Consumed, engine.timelineArms.ConsumeOriginalUnparentedUnpublishedSource(*actual));
    EXPECT_EQ(harken::ArmState::Quarantined, finalRow->state.load());
    EXPECT_EQ(UINT32_MAX, finalRow->generation); EXPECT_EQ(0u, finalRow->identity.load());
    EXPECT_EQ(free - 1u, engine.timelineArms.FreeCount());
    EXPECT_EQ(Status::NotEligible, engine.timelineArms.ConsumeOriginalUnparentedUnpublishedSource(*actual));
    EXPECT_EQ(nullptr, finalRow->originalSourceSubmission);
    EXPECT_EQ(0u, finalRow->armedCompletionSequence.load());
    EXPECT_EQ(0u, finalRow->tombstoneAudioSequence.load());
    NativeUnpublishedDispositionSource next;
    ASSERT_NO_FATAL_FAILURE(next.Prepare(engine, false));
    ASSERT_NE(source.arm.slot, next.arm.slot); ASSERT_NE(0u, next.arm.generation);
    ASSERT_NO_FATAL_FAILURE(next.Dispose(engine)); next.original.Release();
    const auto* fresh = engine.originalSourceSubmissions.OriginalUnpublishedDisposition(next.arm);
    ASSERT_NE(nullptr, fresh);
    ASSERT_EQ(Status::Consumed, engine.timelineArms.ConsumeOriginalUnparentedUnpublishedSource(*fresh));
    EXPECT_EQ(free - 1u, engine.timelineArms.FreeCount());
    EXPECT_EQ(harken::ArmState::Quarantined, finalRow->state.load());
    EXPECT_EQ(0u, engine.mixer.BlocksRendered()); EXPECT_EQ(0u, engine.commands.PublishedHead());
}
