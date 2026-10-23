#include "Common/ObjectIdTestSupport.h"
#include "harken/core/CompletionMutationBinding.h"
#include "harken/core/VoiceLedger.h"
#include "Common/TestUtil.h"
#include "harken/param/EffectParamLocator.h"
#include "harken/render/BuiltinEffects.h"

#include <gtest/gtest.h>

namespace {

harken::CompletionOwnerRef Owner(std::uint32_t slot,
                                std::uint32_t generation,
                                std::uint64_t factsRevision) {
    return harken::CompletionOwnerRef::Single(
        {slot, generation}, factsRevision);
}

bool PinMutationOwner(harken::Engine& engine, std::uint32_t ledgerSlot,
                      std::uint32_t renderGeneration,
                      std::uint32_t decisionSlot,
                      std::uint64_t factsRevision,
                      harken::CompletionMutationBinding* out) {
    harken::OccurrenceToken occurrence{};
    harken::OccurrenceKey key{};
    key.clipId = harkentest::ObjectId(decisionSlot);
    if (out == nullptr ||
        !engine.timelineOccurrences.Acquire(key, &occurrence) ||
        !engine.timelineOccurrences.BindCompletionProgress(
            occurrence, renderGeneration, factsRevision)) {
        return false;
    }
    const harken::CompletionOwnerRef owner =
        Owner(decisionSlot, 1, factsRevision);
    if (!engine.timelineOccurrences.TryPinCompletionOwner(occurrence, owner)) {
        return false;
    }
    const harken::CompletionMutationBinding binding{occurrence, owner};
    if (!engine.ledger.BindCompletionMutation(ledgerSlot, binding)) {
        return false;
    }
    *out = binding;
    return true;
}

VnBindingDesc MakeVoiceBinding(std::uint32_t targetId) {
    VnBindingDesc desc{};
    desc.structSize = sizeof(VnBindingDesc);
    desc.variableId = harkentest::ObjectId(77);
    desc.targetScopeKind = VN_TARGET_SCOPE_VOICE;
    desc.targetId = targetId;
    desc.curve.structSize = sizeof(VnCurveDesc);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {0.0f, 0.0f};
    desc.curve.points[1] = {1.0f, 1200.0f};
    desc.curve.handles[0] = harkentest::EndpointHandles(
        desc.curve.points[0], desc.curve.points[1]);
    return desc;
}

VnBindingDesc MakeVoiceEffectBinding(std::uint32_t targetId) {
    VnBindingDesc desc = MakeVoiceBinding(targetId);
    desc.targetScopeKind = VN_TARGET_SCOPE_VOICE_EFFECT_PARAM;
    desc.curve.points[1].y = 6.0f;
    desc.curve.handles[0] = harkentest::EndpointHandles(
        desc.curve.points[0], desc.curve.points[1]);
    return desc;
}

VnBindingAttachDesc MakeVoiceAttach(VnSoundId soundId) {
    VnBindingAttachDesc desc{};
    desc.structSize = sizeof(VnBindingAttachDesc);
    desc.scopeKind = VN_TARGET_SCOPE_VOICE;
    desc.objectId = soundId;
    return desc;
}

TEST(CompletionMutationGuard,
     CanonicalUnboundAllowsAndEveryBoundAuthorityMismatchFailsClosed) {
    const harken::CompletionMutationBinding unbound{};
    EXPECT_EQ(harken::CompletionMutationGuardResult::Allowed,
              harken::EvaluateCompletionMutationBinding(unbound, nullptr));

    const harken::CompletionMutationBinding bound{{3, 5}, Owner(7, 11, 13)};
    const harken::CompletionOwnerRef exact = Owner(7, 11, 13);
    EXPECT_EQ(harken::CompletionMutationGuardResult::OwnerPinned,
              harken::EvaluateCompletionMutationBinding(bound, &exact));
    EXPECT_EQ(harken::CompletionMutationGuardResult::StaleBinding,
              harken::EvaluateCompletionMutationBinding(bound, nullptr));
    const harken::CompletionOwnerRef wrong = Owner(7, 12, 13);
    EXPECT_EQ(harken::CompletionMutationGuardResult::StaleBinding,
              harken::EvaluateCompletionMutationBinding(bound, &wrong));

    harken::CompletionMutationBinding malformed{};
    malformed.occurrence = {3, 5};
    EXPECT_EQ(harken::CompletionMutationGuardResult::StaleBinding,
              harken::EvaluateCompletionMutationBinding(malformed, nullptr));
}

TEST(CompletionMutationGuard,
     LedgerMirrorAndReverseIndexRemainExactAcrossEveryVoiceLifecycle) {
    harken::VoiceLedger ledger(3, 2);
    VnVoiceHandle publicHandle = 0;
    std::uint32_t ledgerSlot = UINT32_MAX;
    ASSERT_TRUE(ledger.AcquirePending(&publicHandle, &ledgerSlot, 128));
    const harken::CompletionMutationBinding binding{{3, 5}, Owner(7, 11, 13)};
    ASSERT_TRUE(ledger.BindCompletionMutation(ledgerSlot, binding));

    harken::CompletionMutationBinding captured{};
    ASSERT_TRUE(ledger.CaptureCompletionMutation(publicHandle, &captured));
    EXPECT_TRUE(harken::SameCompletionMutationBinding(binding, captured));

    const VnVoiceHandle firstRender =
        (static_cast<std::uint64_t>(17) << 32) | 0u;
    ledger.StartPending(ledgerSlot, firstRender);
    ASSERT_TRUE(
        ledger.CaptureCompletionMutationForRender(firstRender, &captured));
    EXPECT_TRUE(harken::SameCompletionMutationBinding(binding, captured));
    EXPECT_FALSE(ledger.CaptureCompletionMutationForRender(
        (static_cast<std::uint64_t>(18) << 32) | 0u, &captured));

    std::uint32_t parkedSlot = UINT32_MAX;
    ASSERT_TRUE(ledger.Park(firstRender, &parkedSlot));
    EXPECT_EQ(ledgerSlot, parkedSlot);
    EXPECT_FALSE(
        ledger.CaptureCompletionMutationForRender(firstRender, &captured));
    ASSERT_TRUE(ledger.CaptureCompletionMutation(publicHandle, &captured));
    EXPECT_TRUE(harken::SameCompletionMutationBinding(binding, captured));

    const VnVoiceHandle returnedRender =
        (static_cast<std::uint64_t>(23) << 32) | 1u;
    ledger.ReturnPark(ledgerSlot, returnedRender);
    ASSERT_TRUE(
        ledger.CaptureCompletionMutationForRender(returnedRender, &captured));
    EXPECT_TRUE(harken::SameCompletionMutationBinding(binding, captured));
    const harken::CompletionMutationBinding mismatch{
        binding.occurrence, Owner(7, 12, 13)};
    EXPECT_FALSE(ledger.ClearCompletionMutationExact(ledgerSlot, mismatch));
    ASSERT_TRUE(ledger.CaptureCompletionMutation(publicHandle, &captured));
    EXPECT_TRUE(harken::SameCompletionMutationBinding(binding, captured));
    EXPECT_TRUE(ledger.ClearCompletionMutationExact(ledgerSlot, binding));
    ASSERT_TRUE(ledger.CaptureCompletionMutation(publicHandle, &captured));
    EXPECT_TRUE(harken::IsCanonicalUnboundCompletionMutation(captured));

    ASSERT_TRUE(ledger.BindCompletionMutation(ledgerSlot, binding));
    ledger.Reap(returnedRender);
    EXPECT_FALSE(ledger.CaptureCompletionMutation(publicHandle, &captured));
    EXPECT_FALSE(
        ledger.CaptureCompletionMutationForRender(returnedRender, &captured));

    VnVoiceHandle reusedHandle = 0;
    std::uint32_t reusedSlot = UINT32_MAX;
    ASSERT_TRUE(ledger.Acquire(&reusedHandle, &reusedSlot));
    EXPECT_EQ(ledgerSlot, reusedSlot);
    ASSERT_TRUE(ledger.CaptureCompletionMutation(reusedHandle, &captured));
    EXPECT_TRUE(harken::IsCanonicalUnboundCompletionMutation(captured));
    ledger.Release(reusedSlot);

    ASSERT_TRUE(ledger.AcquirePending(&reusedHandle, &reusedSlot, 128));
    ASSERT_TRUE(ledger.BindCompletionMutation(reusedSlot, binding));
    ledger.CancelPending(reusedSlot);
    EXPECT_FALSE(ledger.CaptureCompletionMutation(reusedHandle, &captured));

    ASSERT_TRUE(ledger.AcquirePending(&reusedHandle, &reusedSlot, 128));
    ASSERT_TRUE(ledger.BindCompletionMutation(reusedSlot, binding));
    ledger.ParkPending(reusedSlot);
    ledger.EndParked(reusedSlot);
    EXPECT_FALSE(ledger.CaptureCompletionMutation(reusedHandle, &captured));
}

TEST(CompletionMutationGuard,
     PitchRefusesPinnedCompletionOwnerBeforePendingParkedOrLiveMutation) {
    VnConfig config = harkentest::MakeNullConfig(4, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> samples = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    VnVoiceHandle live = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &live));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, live);
    ASSERT_NE(0u, render);

    VnVoiceHandle pending = 0;
    std::uint32_t pendingSlot = UINT32_MAX;
    ASSERT_TRUE(engine->ledger.AcquirePending(&pending, &pendingSlot, 128));
    harken::PendingStartRecord& pendingRecord =
        engine->pendingStarts[pendingSlot];
    pendingRecord.active = true;
    pendingRecord.publicHandle = pending;
    pendingRecord.hasPitch = false;
    pendingRecord.pitchRatio = 0.75;

    VnVoiceHandle parked = 0;
    std::uint32_t parkedSlot = UINT32_MAX;
    ASSERT_TRUE(engine->ledger.AcquirePending(&parked, &parkedSlot, 128));
    engine->ledger.ParkPending(parkedSlot);
    harken::ParkRecord& parkedRecord = engine->parkRecords[parkedSlot];
    parkedRecord.active = true;
    parkedRecord.resolvedPitchCents = 123.0;
    parkedRecord.resolvedPitchRatio = 1.25;
    parkedRecord.instancePitchCents = 15.0;

    harken::CompletionMutationBinding liveBinding{};
    harken::CompletionMutationBinding pendingBinding{};
    harken::CompletionMutationBinding parkedBinding{};
    ASSERT_TRUE(PinMutationOwner(
        *engine, static_cast<std::uint32_t>(live),
        static_cast<std::uint32_t>(render >> 32), 11, 101,
        &liveBinding));
    ASSERT_TRUE(PinMutationOwner(*engine, pendingSlot, 31, 12, 102,
                                 &pendingBinding));
    ASSERT_TRUE(PinMutationOwner(*engine, parkedSlot, 37, 13, 103,
                                 &parkedBinding));

    const std::uint64_t commandHead = engine->commands.PublishedHead();
    const std::uint64_t backpressure = engine->commands.BackpressureCount();
    const float livePitch =
        engine->params.ResolvedValue(render, harken::kTargetVoicePitch);
    const std::size_t parkedDirectCount = parkedRecord.apiDirect.size();

    EXPECT_EQ(VN_BACKPRESSURE, Vn_SetVoicePitch(pending, 2.0f));
    EXPECT_EQ(VN_BACKPRESSURE, Vn_SetVoicePitch(parked, 2.0f));
    EXPECT_EQ(VN_BACKPRESSURE, Vn_SetVoicePitch(live, 2.0f));

    EXPECT_FALSE(pendingRecord.hasPitch);
    EXPECT_DOUBLE_EQ(0.75, pendingRecord.pitchRatio);
    EXPECT_EQ(parkedDirectCount, parkedRecord.apiDirect.size());
    EXPECT_DOUBLE_EQ(123.0, parkedRecord.resolvedPitchCents);
    EXPECT_DOUBLE_EQ(1.25, parkedRecord.resolvedPitchRatio);
    EXPECT_FLOAT_EQ(livePitch,
                    engine->params.ResolvedValue(
                        render, harken::kTargetVoicePitch));
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_EQ(backpressure + 3u, engine->commands.BackpressureCount());
}

TEST(CompletionMutationGuard,
     PitchPreservesUnboundPendingParkedLiveAndStaleSemantics) {
    VnConfig config = harkentest::MakeNullConfig(4, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> samples = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    VnVoiceHandle live = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &live));

    VnVoiceHandle pending = 0;
    std::uint32_t pendingSlot = UINT32_MAX;
    ASSERT_TRUE(engine->ledger.AcquirePending(&pending, &pendingSlot, 128));
    engine->pendingStarts[pendingSlot].active = true;
    engine->pendingStarts[pendingSlot].publicHandle = pending;

    VnVoiceHandle parked = 0;
    std::uint32_t parkedSlot = UINT32_MAX;
    ASSERT_TRUE(engine->ledger.AcquirePending(&parked, &parkedSlot, 128));
    engine->ledger.ParkPending(parkedSlot);
    engine->parkRecords[parkedSlot].active = true;
    EXPECT_EQ(VN_OK, Vn_SetVoicePitch(pending, 1.5f));
    EXPECT_EQ(VN_OK, Vn_SetVoicePitch(parked, 1.5f));
    EXPECT_EQ(VN_OK, Vn_SetVoicePitch(live, 1.5f));
    EXPECT_EQ(VN_STALE_HANDLE,
              Vn_SetVoicePitch(UINT64_C(0x0000007700000077), 1.5f));
}

TEST(CompletionMutationGuard,
     StopRefusesPinnedCompletionOwnerBeforePendingParkedOrLiveMutation) {
    VnConfig config = harkentest::MakeNullConfig(4, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> samples = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    VnVoiceHandle live = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &live));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, live);
    ASSERT_NE(0u, render);

    VnVoiceHandle pending = 0;
    std::uint32_t pendingSlot = UINT32_MAX;
    ASSERT_TRUE(engine->ledger.AcquirePending(&pending, &pendingSlot, 128));
    harken::PendingStartRecord& pendingRecord =
        engine->pendingStarts[pendingSlot];
    pendingRecord.active = true;
    pendingRecord.publicHandle = pending;
    pendingRecord.hasPitch = true;
    pendingRecord.pitchRatio = 0.75;

    VnVoiceHandle parked = 0;
    std::uint32_t parkedSlot = UINT32_MAX;
    ASSERT_TRUE(engine->ledger.AcquirePending(&parked, &parkedSlot, 128));
    engine->ledger.ParkPending(parkedSlot);
    harken::ParkRecord& parkedRecord = engine->parkRecords[parkedSlot];
    parkedRecord.active = true;
    parkedRecord.resolvedPitchCents = 123.0;

    harken::CompletionMutationBinding binding{};
    ASSERT_TRUE(PinMutationOwner(
        *engine, static_cast<std::uint32_t>(live),
        static_cast<std::uint32_t>(render >> 32), 21, 201, &binding));
    ASSERT_TRUE(PinMutationOwner(*engine, pendingSlot, 41, 22, 202,
                                 &binding));
    ASSERT_TRUE(PinMutationOwner(*engine, parkedSlot, 43, 23, 203,
                                 &binding));

    const std::uint64_t commandHead = engine->commands.PublishedHead();
    const std::uint64_t backpressure = engine->commands.BackpressureCount();
    EXPECT_FALSE(engine->ledger.IsTerminating(render));

    EXPECT_EQ(VN_BACKPRESSURE, Vn_Stop(pending, 5));
    EXPECT_EQ(VN_BACKPRESSURE, Vn_Stop(parked, 5));
    EXPECT_EQ(VN_BACKPRESSURE, Vn_Stop(live, 5));

    EXPECT_EQ(harken::VoiceLedger::HandleKind::Pending,
              engine->ledger.Classify(pending).kind);
    EXPECT_TRUE(pendingRecord.active);
    EXPECT_TRUE(pendingRecord.hasPitch);
    EXPECT_DOUBLE_EQ(0.75, pendingRecord.pitchRatio);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(parked).kind);
    EXPECT_TRUE(parkedRecord.active);
    EXPECT_DOUBLE_EQ(123.0, parkedRecord.resolvedPitchCents);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(live).kind);
    EXPECT_FALSE(engine->ledger.IsTerminating(render));
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_EQ(backpressure + 3u, engine->commands.BackpressureCount());
    VnNotification notification{};
    EXPECT_FALSE(engine->logicNotifications.TryPop(notification));
}

TEST(CompletionMutationGuard,
     StopFailsClosedForStaleCompletionMirrorInEveryVoiceState) {
    VnConfig config = harkentest::MakeNullConfig(4, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> samples = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    VnVoiceHandle live = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &live));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, live);

    VnVoiceHandle pending = 0;
    std::uint32_t pendingSlot = UINT32_MAX;
    ASSERT_TRUE(engine->ledger.AcquirePending(&pending, &pendingSlot, 128));
    engine->pendingStarts[pendingSlot].active = true;
    engine->pendingStarts[pendingSlot].publicHandle = pending;
    VnVoiceHandle parked = 0;
    std::uint32_t parkedSlot = UINT32_MAX;
    ASSERT_TRUE(engine->ledger.AcquirePending(&parked, &parkedSlot, 128));
    engine->ledger.ParkPending(parkedSlot);
    engine->parkRecords[parkedSlot].active = true;

    harken::CompletionMutationBinding liveBinding{};
    harken::CompletionMutationBinding pendingBinding{};
    harken::CompletionMutationBinding parkedBinding{};
    ASSERT_TRUE(PinMutationOwner(
        *engine, static_cast<std::uint32_t>(live),
        static_cast<std::uint32_t>(render >> 32), 31, 301,
        &liveBinding));
    ASSERT_TRUE(PinMutationOwner(*engine, pendingSlot, 47, 32, 302,
                                 &pendingBinding));
    ASSERT_TRUE(PinMutationOwner(*engine, parkedSlot, 53, 33, 303,
                                 &parkedBinding));
    ASSERT_TRUE(engine->timelineOccurrences.RollbackCompletionOwnerPinExact(
        liveBinding.occurrence, liveBinding.owner));
    ASSERT_TRUE(engine->timelineOccurrences.RollbackCompletionOwnerPinExact(
        pendingBinding.occurrence, pendingBinding.owner));
    ASSERT_TRUE(engine->timelineOccurrences.RollbackCompletionOwnerPinExact(
        parkedBinding.occurrence, parkedBinding.owner));

    const std::uint64_t commandHead = engine->commands.PublishedHead();
    const std::uint64_t backpressure = engine->commands.BackpressureCount();
    EXPECT_EQ(VN_BACKPRESSURE, Vn_Stop(pending, 0));
    EXPECT_EQ(VN_BACKPRESSURE, Vn_Stop(parked, 0));
    EXPECT_EQ(VN_BACKPRESSURE, Vn_Stop(live, 0));
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Pending,
              engine->ledger.Classify(pending).kind);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(parked).kind);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(live).kind);
    EXPECT_FALSE(engine->ledger.IsTerminating(render));
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_EQ(backpressure + 3u, engine->commands.BackpressureCount());
}

TEST(CompletionMutationGuard,
     StopPreservesUnboundPendingParkedLiveAndStaleSemantics) {
    VnConfig config = harkentest::MakeNullConfig(4, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> samples = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    VnVoiceHandle live = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &live));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, live);

    VnVoiceHandle pending = 0;
    std::uint32_t pendingSlot = UINT32_MAX;
    ASSERT_TRUE(engine->ledger.AcquirePending(&pending, &pendingSlot, 128));
    engine->pendingStarts[pendingSlot].active = true;
    engine->pendingStarts[pendingSlot].publicHandle = pending;
    VnVoiceHandle parked = 0;
    std::uint32_t parkedSlot = UINT32_MAX;
    ASSERT_TRUE(engine->ledger.AcquirePending(&parked, &parkedSlot, 128));
    engine->ledger.ParkPending(parkedSlot);
    engine->parkRecords[parkedSlot].active = true;

    const std::uint64_t commandHead = engine->commands.PublishedHead();
    const std::uint64_t backpressure = engine->commands.BackpressureCount();
    EXPECT_EQ(VN_OK, Vn_Stop(pending, 0));
    EXPECT_EQ(VN_OK, Vn_Stop(parked, 0));
    EXPECT_EQ(VN_OK, Vn_Stop(live, 0));
    EXPECT_EQ(VN_STALE_HANDLE,
              Vn_Stop(UINT64_C(0x0000007700000077), 0));
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Stale,
              engine->ledger.Classify(pending).kind);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Stale,
              engine->ledger.Classify(parked).kind);
    EXPECT_TRUE(engine->ledger.IsTerminating(render));
    EXPECT_EQ(commandHead + 1u, engine->commands.PublishedHead());
    EXPECT_EQ(backpressure, engine->commands.BackpressureCount());
}

TEST(CompletionMutationGuard,
     ApplyPitchBindingRefusesPinnedPendingParkedAndLiveBeforeMutation) {
    VnConfig config = harkentest::MakeNullConfig(4, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const VnBindingDesc bindingDesc =
        MakeVoiceBinding(harken::kTargetVoicePitch);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(90), &bindingDesc));
    const std::vector<float> samples = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    VnVoiceHandle live = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &live));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, live);
    ASSERT_NE(0u, render);

    VnVoiceHandle pending = 0;
    std::uint32_t pendingSlot = UINT32_MAX;
    ASSERT_TRUE(engine->ledger.AcquirePending(&pending, &pendingSlot, 128));
    engine->pendingStarts[pendingSlot].active = true;
    engine->pendingStarts[pendingSlot].publicHandle = pending;

    VnVoiceHandle parked = 0;
    std::uint32_t parkedSlot = UINT32_MAX;
    ASSERT_TRUE(engine->ledger.AcquirePending(&parked, &parkedSlot, 128));
    engine->ledger.ParkPending(parkedSlot);
    engine->parkRecords[parkedSlot].active = true;
    engine->parkRecords[parkedSlot].bindingInstances.reserve(
        engine->config.voiceBindingInstanceSlots);

    harken::CompletionMutationBinding mutation{};
    ASSERT_TRUE(PinMutationOwner(*engine, pendingSlot, 31, 61, 601,
                                 &mutation));
    ASSERT_TRUE(PinMutationOwner(*engine, parkedSlot, 37, 62, 602,
                                 &mutation));
    ASSERT_TRUE(PinMutationOwner(
        *engine, static_cast<std::uint32_t>(live),
        static_cast<std::uint32_t>(render >> 32), 63, 603, &mutation));

    const std::uint64_t commandHead = engine->commands.PublishedHead();
    const std::uint64_t backpressure = engine->commands.BackpressureCount();
    EXPECT_EQ(VN_STALE_HANDLE, Vn_ApplyVoiceBinding(pending, harkentest::ObjectId(90)));
    EXPECT_EQ(VN_BACKPRESSURE, Vn_ApplyVoiceBinding(parked, harkentest::ObjectId(90)));
    EXPECT_EQ(VN_BACKPRESSURE, Vn_ApplyVoiceBinding(live, harkentest::ObjectId(90)));

    EXPECT_EQ(0u, engine->pendingStarts[pendingSlot].pinnedBindingCount);
    EXPECT_TRUE(engine->parkRecords[parkedSlot].bindingInstances.empty());
    EXPECT_EQ(0u, engine->params.VoiceBindingCount(render));
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_EQ(backpressure + 2u, engine->commands.BackpressureCount());
}

TEST(CompletionMutationGuard,
     ClearPitchBindingRefusesPinnedParkedAndLiveBeforeMutation) {
    VnConfig config = harkentest::MakeNullConfig(4, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const VnBindingDesc bindingDesc =
        MakeVoiceBinding(harken::kTargetVoicePitch);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(91), &bindingDesc));
    const std::vector<float> samples = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    VnVoiceHandle live = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &live));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, live);
    ASSERT_NE(0u, render);

    VnVoiceHandle parked = 0;
    std::uint32_t parkedSlot = UINT32_MAX;
    ASSERT_TRUE(engine->ledger.AcquirePending(&parked, &parkedSlot, 128));
    engine->ledger.ParkPending(parkedSlot);
    engine->parkRecords[parkedSlot].active = true;
    engine->parkRecords[parkedSlot].bindingInstances.reserve(
        engine->config.voiceBindingInstanceSlots);
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(parked, harkentest::ObjectId(91)));
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(live, harkentest::ObjectId(91)));

    harken::CompletionMutationBinding mutation{};
    ASSERT_TRUE(PinMutationOwner(*engine, parkedSlot, 41, 64, 604,
                                 &mutation));
    ASSERT_TRUE(PinMutationOwner(
        *engine, static_cast<std::uint32_t>(live),
        static_cast<std::uint32_t>(render >> 32), 65, 605, &mutation));

    const std::uint64_t commandHead = engine->commands.PublishedHead();
    const std::uint64_t backpressure = engine->commands.BackpressureCount();
    EXPECT_EQ(VN_BACKPRESSURE, Vn_ClearVoiceBinding(parked, harkentest::ObjectId(91)));
    EXPECT_EQ(VN_BACKPRESSURE, Vn_ClearVoiceBinding(live, harkentest::ObjectId(91)));

    ASSERT_EQ(1u,
              engine->parkRecords[parkedSlot].bindingInstances.size());
    EXPECT_EQ(harkentest::ObjectId(91u), engine->parkRecords[parkedSlot]
                       .bindingInstances.front()
                       .definitionId);
    bool liveInstance = false;
    engine->params.ForEachInstance(
        [&](const harken::ParamSystem::InstanceCaptureRow& row) {
            if (row.handle == render && row.definitionId == harkentest::ObjectId(91) &&
                row.viaApply && !row.neutralized) {
                liveInstance = true;
            }
        });
    EXPECT_TRUE(liveInstance);
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_EQ(backpressure + 2u, engine->commands.BackpressureCount());
}

TEST(CompletionMutationGuard,
     BindingGuardPreservesRefusalPriorityAndAllowsNonPitchBindings) {
    VnConfig config = harkentest::MakeNullConfig(2, 256, 48000);
    config.fxSlotsPerVoice = 1;
    config.gainEffectPoolCapacity = 2;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const VnBindingDesc pitch =
        MakeVoiceBinding(harken::kTargetVoicePitch);
    const VnBindingDesc volume =
        MakeVoiceBinding(harken::kTargetVoiceVolume);
    VnBindingDesc bus = MakeVoiceBinding(harken::kBusTargetBusVolume);
    bus.targetScopeKind = VN_TARGET_SCOPE_BUS;
    bus.curve.points[1].y = -6.0f;
    bus.curve.handles[0] = harkentest::EndpointHandles(
        bus.curve.points[0], bus.curve.points[1]);
    const VnBindingDesc effect = MakeVoiceEffectBinding(
        harken::PackEffectParamLocator(VN_EFFECT_GAIN, 0,
                                      harken::kGainParamGainDb));
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(90), &pitch));
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(92), &volume));
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(93), &bus));
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(94), &effect));

    const std::vector<float> samples = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    VnVoiceHandle live = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &live));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, live);
    ASSERT_NE(0u, render);
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(live, harkentest::ObjectId(90)));
    VnGainDesc gain{};
    gain.structSize = sizeof(VnGainDesc);
    gain.gainDb = -3.0f;
    ASSERT_EQ(VN_OK, Vn_SetVoiceEffect(
                         live, 0, VN_EFFECT_GAIN,
                         reinterpret_cast<const VnEffectDesc*>(&gain)));

    harken::CompletionMutationBinding mutation{};
    ASSERT_TRUE(PinMutationOwner(
        *engine, static_cast<std::uint32_t>(live),
        static_cast<std::uint32_t>(render >> 32), 66, 606, &mutation));
    const std::uint64_t backpressure = engine->commands.BackpressureCount();

    EXPECT_EQ(VN_UNKNOWN_ID, Vn_ApplyVoiceBinding(live, harkentest::ObjectId(999)));
    EXPECT_EQ(VN_INVALID_ARG, Vn_ApplyVoiceBinding(live, harkentest::ObjectId(93)));
    EXPECT_EQ(VN_STALE_HANDLE,
              Vn_ApplyVoiceBinding(UINT64_C(0x0000007700000077), harkentest::ObjectId(90)));
    EXPECT_EQ(VN_IN_USE, Vn_ApplyVoiceBinding(live, harkentest::ObjectId(90)));
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_ClearVoiceBinding(live, harkentest::ObjectId(999)));
    EXPECT_EQ(VN_STALE_HANDLE,
              Vn_ClearVoiceBinding(UINT64_C(0x0000007700000077), harkentest::ObjectId(90)));

    EXPECT_EQ(VN_OK, Vn_ApplyVoiceBinding(live, harkentest::ObjectId(92)));
    EXPECT_EQ(VN_OK, Vn_ClearVoiceBinding(live, harkentest::ObjectId(92)));
    EXPECT_EQ(VN_OK, Vn_ApplyVoiceBinding(live, harkentest::ObjectId(94)));
    EXPECT_EQ(VN_OK, Vn_ClearVoiceBinding(live, harkentest::ObjectId(94)));
    EXPECT_EQ(backpressure, engine->commands.BackpressureCount());
}

TEST(CompletionMutationGuard,
     ReplacePitchBindingRefusesAllVoicesAtomicallyAndBooksOneOutcome) {
    VnConfig config = harkentest::MakeNullConfig(4, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const VnBindingDesc original =
        MakeVoiceBinding(harken::kTargetVoicePitch);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(95), &original));
    const std::vector<float> samples = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    VnVoiceHandle live = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &live));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, live);
    ASSERT_NE(0u, render);
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(live, harkentest::ObjectId(95)));

    VnVoiceHandle parked = 0;
    std::uint32_t parkedSlot = UINT32_MAX;
    ASSERT_TRUE(engine->ledger.AcquirePending(&parked, &parkedSlot, 128));
    engine->ledger.ParkPending(parkedSlot);
    engine->parkRecords[parkedSlot].active = true;
    engine->parkRecords[parkedSlot].bindingInstances.reserve(
        engine->config.voiceBindingInstanceSlots);
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(parked, harkentest::ObjectId(95)));

    VnVoiceHandle pending = 0;
    std::uint32_t pendingSlot = UINT32_MAX;
    ASSERT_TRUE(engine->ledger.AcquirePending(&pending, &pendingSlot, 128));
    engine->pendingStarts[pendingSlot].active = true;
    engine->pendingStarts[pendingSlot].publicHandle = pending;
    engine->pendingStarts[pendingSlot].pinnedBindings[0].bindingId = harkentest::ObjectId(95);
    engine->pendingStarts[pendingSlot].pinnedBindingCount = 1;

    harken::CompletionMutationBinding pendingMutation{};
    ASSERT_TRUE(PinMutationOwner(*engine, pendingSlot, 47, 69, 609,
                                 &pendingMutation));

    const harken::BindingTable::Definition* before = engine->bindings.Find(harkentest::ObjectId(95));
    ASSERT_NE(nullptr, before);
    ASSERT_FALSE(before->pendingInstall);
    const std::uint32_t revision = before->revision;
    const float oldCurveEnd = before->curve.points[1].y;
    const std::uint64_t commandHead = engine->commands.PublishedHead();
    const std::uint64_t backpressure = engine->commands.BackpressureCount();
    const std::uint64_t applied = engine->hotReloadCount;
    const std::uint64_t refused = engine->hotReloadRefusedCount;
    VnBindingDesc replacement = original;
    replacement.curve.points[1].y = 600.0f;
    replacement.curve.handles[0] = harkentest::EndpointHandles(
        replacement.curve.points[0], replacement.curve.points[1]);

    VnBindingDesc invalid = replacement;
    invalid.targetId = harken::kTargetVoiceVolume;
    EXPECT_EQ(VN_INVALID_ARG, Vn_ReplaceBinding(harkentest::ObjectId(95), &invalid));
    EXPECT_EQ(backpressure, engine->commands.BackpressureCount());
    EXPECT_EQ(refused + 1u, engine->hotReloadRefusedCount);

    EXPECT_EQ(VN_BACKPRESSURE, Vn_ReplaceBinding(harkentest::ObjectId(95), &replacement));

    ASSERT_TRUE(engine->ledger.ClearCompletionMutationExact(
        pendingSlot, pendingMutation));
    ASSERT_TRUE(engine->timelineOccurrences.RollbackCompletionOwnerPinExact(
        pendingMutation.occurrence, pendingMutation.owner));
    harken::CompletionMutationBinding parkedMutation{};
    ASSERT_TRUE(PinMutationOwner(*engine, parkedSlot, 43, 68, 608,
                                 &parkedMutation));
    EXPECT_EQ(VN_BACKPRESSURE, Vn_ReplaceBinding(harkentest::ObjectId(95), &replacement));

    ASSERT_TRUE(engine->ledger.ClearCompletionMutationExact(
        parkedSlot, parkedMutation));
    ASSERT_TRUE(engine->timelineOccurrences.RollbackCompletionOwnerPinExact(
        parkedMutation.occurrence, parkedMutation.owner));
    harken::CompletionMutationBinding liveMutation{};
    ASSERT_TRUE(PinMutationOwner(
        *engine, static_cast<std::uint32_t>(live),
        static_cast<std::uint32_t>(render >> 32), 67, 607,
        &liveMutation));
    EXPECT_EQ(VN_BACKPRESSURE, Vn_ReplaceBinding(harkentest::ObjectId(95), &replacement));

    const harken::BindingTable::Definition* after = engine->bindings.Find(harkentest::ObjectId(95));
    ASSERT_NE(nullptr, after);
    EXPECT_FALSE(after->pendingInstall);
    EXPECT_EQ(revision, after->revision);
    EXPECT_FLOAT_EQ(oldCurveEnd, after->curve.points[1].y);
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_EQ(backpressure + 3u, engine->commands.BackpressureCount());
    EXPECT_EQ(applied, engine->hotReloadCount);
    EXPECT_EQ(refused + 4u, engine->hotReloadRefusedCount);
}

TEST(CompletionMutationGuard,
     DetachPitchBindingRefusesLiveAndParkedInstancesAtomically) {
    VnConfig config = harkentest::MakeNullConfig(4, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const VnBindingDesc bindingDesc =
        MakeVoiceBinding(harken::kTargetVoicePitch);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(96), &bindingDesc));
    const std::vector<float> samples = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnBindingAttachDesc attachment = MakeVoiceAttach(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(96), &attachment));

    VnVoiceHandle live = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &live));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, live);
    ASSERT_NE(0u, render);

    VnVoiceHandle parked = 0;
    std::uint32_t parkedSlot = UINT32_MAX;
    ASSERT_TRUE(engine->ledger.AcquirePending(&parked, &parkedSlot, 128));
    engine->ledger.ParkPending(parkedSlot);
    harken::ParkRecord& parkedRecord = engine->parkRecords[parkedSlot];
    parkedRecord.active = true;
    parkedRecord.bindingInstances.reserve(
        engine->config.voiceBindingInstanceSlots);
    harken::ParamSystem::InstanceState parkedInstance{};
    parkedInstance.definitionId = harkentest::ObjectId(96);
    parkedInstance.attachmentObjectId = harkentest::ObjectId(1);
    parkedInstance.viaApply = false;
    parkedRecord.bindingInstances.push_back(parkedInstance);

    harken::CompletionMutationBinding mutation{};
    ASSERT_TRUE(PinMutationOwner(*engine, parkedSlot, 53, 71, 611,
                                 &mutation));
    const std::uint64_t commandHead = engine->commands.PublishedHead();
    const std::uint64_t backpressure = engine->commands.BackpressureCount();
    const std::uint64_t worstCase =
        engine->propertyPublisher.WorstCaseCommits();

    VnBindingAttachDesc missing = attachment;
    missing.objectId = harkentest::ObjectId(2);
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_DetachBinding(harkentest::ObjectId(96), &missing));
    EXPECT_EQ(backpressure, engine->commands.BackpressureCount());

    EXPECT_EQ(VN_BACKPRESSURE, Vn_DetachBinding(harkentest::ObjectId(96), &attachment));

    EXPECT_TRUE(engine->bindings.HasAttachment(
        harkentest::ObjectId(96), VN_TARGET_SCOPE_VOICE, harkentest::ObjectId(1)));
    ASSERT_EQ(1u, parkedRecord.bindingInstances.size());
    bool liveInstance = false;
    engine->params.ForEachInstance(
        [&](const harken::ParamSystem::InstanceCaptureRow& row) {
            if (row.handle == render && row.definitionId == harkentest::ObjectId(96) &&
                row.attachmentObjectId == harkentest::ObjectId(1) && !row.viaApply &&
                !row.neutralized) {
                liveInstance = true;
            }
        });
    EXPECT_TRUE(liveInstance);
    EXPECT_EQ(worstCase, engine->propertyPublisher.WorstCaseCommits());
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_EQ(backpressure + 1u, engine->commands.BackpressureCount());
}

TEST(CompletionMutationGuard,
     VoicePitchAttachIsFutureOnlyAndPendingOnlyDetachRemainsAllowed) {
    VnConfig config = harkentest::MakeNullConfig(3, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const VnBindingDesc bindingDesc =
        MakeVoiceBinding(harken::kTargetVoicePitch);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(97), &bindingDesc));
    const std::vector<float> samples = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    VnVoiceHandle existing = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &existing));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, existing);
    harken::CompletionMutationBinding mutation{};
    ASSERT_TRUE(PinMutationOwner(
        *engine, static_cast<std::uint32_t>(existing),
        static_cast<std::uint32_t>(render >> 32), 72, 612, &mutation));
    const std::uint64_t backpressure = engine->commands.BackpressureCount();
    const VnBindingAttachDesc attachment = MakeVoiceAttach(harkentest::ObjectId(1));

    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(97), &attachment));
    EXPECT_EQ(0u, engine->params.VoiceBindingCount(render));

    VnVoiceHandle pending = 0;
    std::uint32_t pendingSlot = UINT32_MAX;
    ASSERT_TRUE(engine->ledger.AcquirePending(&pending, &pendingSlot, 128));
    engine->pendingStarts[pendingSlot].active = true;
    engine->pendingStarts[pendingSlot].publicHandle = pending;
    engine->pendingStarts[pendingSlot].pinnedBindings[0].bindingId = harkentest::ObjectId(97);
    engine->pendingStarts[pendingSlot].pinnedBindings[0].attachmentObjectId = harkentest::ObjectId(1);
    engine->pendingStarts[pendingSlot].pinnedBindingCount = 1;
    ASSERT_TRUE(PinMutationOwner(*engine, pendingSlot, 59, 73, 613,
                                 &mutation));

    EXPECT_EQ(VN_OK, Vn_DetachBinding(harkentest::ObjectId(97), &attachment));
    EXPECT_FALSE(engine->bindings.HasAttachment(
        harkentest::ObjectId(97), VN_TARGET_SCOPE_VOICE, harkentest::ObjectId(1)));
    EXPECT_EQ(1u, engine->pendingStarts[pendingSlot].pinnedBindingCount);
    EXPECT_EQ(harkentest::ObjectId(97u),
              engine->pendingStarts[pendingSlot].pinnedBindings[0].bindingId);
    EXPECT_EQ(backpressure, engine->commands.BackpressureCount());
}

TEST(CompletionMutationGuard,
     ReplaceSkipsInertRowsAndAllowsNonPitchOrFutureOnlyDefinitions) {
    VnConfig config = harkentest::MakeNullConfig(3, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const VnBindingDesc pitch =
        MakeVoiceBinding(harken::kTargetVoicePitch);
    const VnBindingDesc volume =
        MakeVoiceBinding(harken::kTargetVoiceVolume);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(98), &pitch));
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(99), &volume));
    const std::vector<float> samples = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    VnVoiceHandle live = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &live));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, live);
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(live, harkentest::ObjectId(98)));
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(live, harkentest::ObjectId(99)));

    VnBindingDesc replacementPitch = pitch;
    replacementPitch.curve.points[1].y = 600.0f;
    replacementPitch.curve.handles[0] = harkentest::EndpointHandles(
        replacementPitch.curve.points[0],
        replacementPitch.curve.points[1]);
    VnBindingDesc replacementVolume = volume;
    replacementVolume.curve.points[1].y = -3.0f;
    replacementVolume.curve.handles[0] = harkentest::EndpointHandles(
        replacementVolume.curve.points[0],
        replacementVolume.curve.points[1]);
    harken::CompletionMutationBinding mutation{};
    ASSERT_TRUE(PinMutationOwner(
        *engine, static_cast<std::uint32_t>(live),
        static_cast<std::uint32_t>(render >> 32), 74, 614, &mutation));
    const std::uint64_t backpressure = engine->commands.BackpressureCount();
    EXPECT_EQ(VN_OK, Vn_ReplaceBinding(harkentest::ObjectId(99), &replacementVolume));
    EXPECT_EQ(backpressure, engine->commands.BackpressureCount());

    engine->ledger.Reap(render);
    EXPECT_EQ(VN_OK, Vn_ReplaceBinding(harkentest::ObjectId(98), &replacementPitch));

    const VnBindingAttachDesc attachment = MakeVoiceAttach(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(98), &attachment));
    replacementPitch.curve.points[1].y = 300.0f;
    replacementPitch.curve.handles[0] = harkentest::EndpointHandles(
        replacementPitch.curve.points[0],
        replacementPitch.curve.points[1]);
    EXPECT_EQ(VN_OK, Vn_ReplaceBinding(harkentest::ObjectId(98), &replacementPitch));
}

TEST(CompletionMutationGuard,
     AggregatePitchBindingMutationsFailClosedForStaleLiveMirror) {
    VnConfig config = harkentest::MakeNullConfig(2, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const VnBindingDesc pitch =
        MakeVoiceBinding(harken::kTargetVoicePitch);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(100), &pitch));
    const std::vector<float> samples = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const VnBindingAttachDesc attachment = MakeVoiceAttach(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(100), &attachment));
    VnVoiceHandle live = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &live));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, live);

    harken::CompletionMutationBinding mutation{};
    ASSERT_TRUE(PinMutationOwner(
        *engine, static_cast<std::uint32_t>(live),
        static_cast<std::uint32_t>(render >> 32), 75, 615, &mutation));
    ASSERT_TRUE(engine->timelineOccurrences.RollbackCompletionOwnerPinExact(
        mutation.occurrence, mutation.owner));
    VnBindingDesc replacement = pitch;
    replacement.curve.points[1].y = 600.0f;
    replacement.curve.handles[0] = harkentest::EndpointHandles(
        replacement.curve.points[0], replacement.curve.points[1]);
    const std::uint64_t backpressure = engine->commands.BackpressureCount();

    EXPECT_EQ(VN_BACKPRESSURE, Vn_ReplaceBinding(harkentest::ObjectId(100), &replacement));
    EXPECT_EQ(VN_BACKPRESSURE, Vn_DetachBinding(harkentest::ObjectId(100), &attachment));

    const harken::BindingTable::Definition* definition =
        engine->bindings.Find(harkentest::ObjectId(100));
    ASSERT_NE(nullptr, definition);
    EXPECT_FALSE(definition->pendingInstall);
    EXPECT_TRUE(engine->bindings.HasAttachment(
        harkentest::ObjectId(100), VN_TARGET_SCOPE_VOICE, harkentest::ObjectId(1)));
    EXPECT_EQ(backpressure + 2u, engine->commands.BackpressureCount());
}

TEST(CompletionMutationGuard,
     PendingPitchRevisionDefersWhenOwnerPinsAfterReplaceThenInstallsOnce) {
    VnConfig config = harkentest::MakeNullConfig(2, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const VnBindingDesc pitch =
        MakeVoiceBinding(harken::kTargetVoicePitch);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(101), &pitch));
    const std::vector<float> samples = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    VnVoiceHandle live = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &live));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, live);
    ASSERT_EQ(VN_OK, Vn_ApplyVoiceBinding(live, harkentest::ObjectId(101)));
    const harken::BindingTable::Definition* definition =
        engine->bindings.Find(harkentest::ObjectId(101));
    ASSERT_NE(nullptr, definition);
    const std::uint32_t revision = definition->revision;
    const float oldCurveEnd = definition->curve.points[1].y;
    VnBindingDesc replacement = pitch;
    replacement.curve.points[1].y = 600.0f;
    replacement.curve.handles[0] = harkentest::EndpointHandles(
        replacement.curve.points[0], replacement.curve.points[1]);
    ASSERT_EQ(VN_OK, Vn_ReplaceBinding(harkentest::ObjectId(101), &replacement));
    ASSERT_TRUE(engine->bindings.Find(harkentest::ObjectId(101))->pendingInstall);

    harken::CompletionMutationBinding mutation{};
    ASSERT_TRUE(PinMutationOwner(
        *engine, static_cast<std::uint32_t>(live),
        static_cast<std::uint32_t>(render >> 32), 87, 627, &mutation));
    const std::uint64_t backpressure = engine->commands.BackpressureCount();
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, Vn_Update());

    definition = engine->bindings.Find(harkentest::ObjectId(101));
    ASSERT_NE(nullptr, definition);
    EXPECT_TRUE(definition->pendingInstall);
    EXPECT_EQ(revision, definition->revision);
    EXPECT_FLOAT_EQ(oldCurveEnd, definition->curve.points[1].y);
    EXPECT_EQ(backpressure, engine->commands.BackpressureCount());

    ASSERT_TRUE(engine->ledger.ClearCompletionMutationExact(
        static_cast<std::uint32_t>(live), mutation));
    ASSERT_TRUE(engine->timelineOccurrences.RollbackCompletionOwnerPinExact(
        mutation.occurrence, mutation.owner));
    ASSERT_EQ(VN_OK, Vn_Update());
    definition = engine->bindings.Find(harkentest::ObjectId(101));
    ASSERT_NE(nullptr, definition);
    EXPECT_FALSE(definition->pendingInstall);
    EXPECT_EQ(revision + 1u, definition->revision);
    EXPECT_FLOAT_EQ(600.0f, definition->curve.points[1].y);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(revision + 1u, engine->bindings.Find(harkentest::ObjectId(101))->revision);
}

TEST(CompletionMutationGuard,
     UnregisterPcmRefusesPinnedCurrentGenerationBeforeRegistryMutation) {
    VnConfig config = harkentest::MakeNullConfig(2, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> samples = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const harken::PCMView current = engine->registry.Lookup(harkentest::ObjectId(1));
    ASSERT_TRUE(current.Valid());
    ASSERT_NE(nullptr, current.useCount);
    VnVoiceHandle live = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &live));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, live);
    ASSERT_NE(0u, render);
    harken::CompletionMutationBinding mutation{};
    ASSERT_TRUE(PinMutationOwner(
        *engine, static_cast<std::uint32_t>(live),
        static_cast<std::uint32_t>(render >> 32), 76, 616, &mutation));
    const std::uint64_t backpressure = engine->commands.BackpressureCount();

    EXPECT_EQ(VN_UNKNOWN_ID, Vn_UnregisterPCM(harkentest::ObjectId(999)));
    EXPECT_EQ(backpressure, engine->commands.BackpressureCount());
    EXPECT_EQ(VN_BACKPRESSURE, Vn_UnregisterPCM(harkentest::ObjectId(1)));

    const harken::PCMView after = engine->registry.Lookup(harkentest::ObjectId(1));
    EXPECT_TRUE(after.Valid());
    EXPECT_EQ(current.useCount, after.useCount);
    EXPECT_EQ(current.contentRevision, after.contentRevision);
    EXPECT_EQ(1u, engine->registry.Count());
    EXPECT_EQ(backpressure + 1u, engine->commands.BackpressureCount());
}

TEST(CompletionMutationGuard,
     ReplacePcmValidatesThenRefusesPinnedCurrentGenerationExactlyOnce) {
    VnConfig config = harkentest::MakeNullConfig(2, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> samples = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    const harken::PCMView current = engine->registry.Lookup(harkentest::ObjectId(1));
    VnVoiceHandle live = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &live));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, live);
    harken::CompletionMutationBinding mutation{};
    ASSERT_TRUE(PinMutationOwner(
        *engine, static_cast<std::uint32_t>(live),
        static_cast<std::uint32_t>(render >> 32), 77, 617, &mutation));
    const std::uint64_t backpressure = engine->commands.BackpressureCount();
    const std::uint64_t applied = engine->hotReloadCount;
    const std::uint64_t refused = engine->hotReloadRefusedCount;

    VnPCMDesc invalid = pcm;
    invalid.sampleRate = 0;
    EXPECT_EQ(VN_INVALID_ARG, Vn_ReplacePCM(harkentest::ObjectId(1), &invalid));
    EXPECT_EQ(backpressure, engine->commands.BackpressureCount());
    EXPECT_EQ(refused + 1u, engine->hotReloadRefusedCount);

    EXPECT_EQ(VN_BACKPRESSURE, Vn_ReplacePCM(harkentest::ObjectId(1), &pcm));

    const harken::PCMView after = engine->registry.Lookup(harkentest::ObjectId(1));
    EXPECT_EQ(current.useCount, after.useCount);
    EXPECT_EQ(current.contentRevision, after.contentRevision);
    EXPECT_EQ(0u, engine->registry.RetiredCount());
    EXPECT_EQ(backpressure + 1u, engine->commands.BackpressureCount());
    EXPECT_EQ(applied, engine->hotReloadCount);
    EXPECT_EQ(refused + 2u, engine->hotReloadRefusedCount);
}

TEST(CompletionMutationGuard,
     PcmCurrentGenerationGuardIncludesPendingAndParkedLogicalVoices) {
    VnConfig config = harkentest::MakeNullConfig(3, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> samples = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &pcm));
    const harken::PCMView pendingView = engine->registry.Lookup(harkentest::ObjectId(1));
    const harken::PCMView parkedView = engine->registry.Lookup(harkentest::ObjectId(2));

    VnVoiceHandle pending = 0;
    std::uint32_t pendingSlot = UINT32_MAX;
    ASSERT_TRUE(engine->ledger.AcquirePending(&pending, &pendingSlot, 128));
    harken::PendingStartRecord& pendingRecord =
        engine->pendingStarts[pendingSlot];
    pendingRecord.active = true;
    pendingRecord.publicHandle = pending;
    pendingRecord.view = pendingView;
    pendingRecord.heldPcmRef = true;
    pendingView.useCount->fetch_add(1, std::memory_order_relaxed);

    VnVoiceHandle parked = 0;
    std::uint32_t parkedSlot = UINT32_MAX;
    ASSERT_TRUE(engine->ledger.AcquirePending(&parked, &parkedSlot, 128));
    engine->ledger.ParkPending(parkedSlot);
    harken::ParkRecord& parkedRecord = engine->parkRecords[parkedSlot];
    parkedRecord.active = true;
    parkedRecord.facts.valid = true;
    parkedRecord.facts.view = parkedView;
    parkedRecord.heldPcmRef = true;
    parkedView.useCount->fetch_add(1, std::memory_order_relaxed);

    harken::CompletionMutationBinding mutation{};
    ASSERT_TRUE(PinMutationOwner(*engine, pendingSlot, 61, 78, 618,
                                 &mutation));
    ASSERT_TRUE(PinMutationOwner(*engine, parkedSlot, 67, 79, 619,
                                 &mutation));
    ASSERT_TRUE(engine->timelineOccurrences.RollbackCompletionOwnerPinExact(
        mutation.occurrence, mutation.owner));
    const std::uint64_t backpressure = engine->commands.BackpressureCount();

    EXPECT_EQ(VN_BACKPRESSURE, Vn_UnregisterPCM(harkentest::ObjectId(1)));
    EXPECT_EQ(VN_BACKPRESSURE, Vn_ReplacePCM(harkentest::ObjectId(2), &pcm));
    EXPECT_EQ(pendingView.useCount,
              engine->registry.Lookup(harkentest::ObjectId(1)).useCount);
    EXPECT_EQ(parkedView.useCount, engine->registry.Lookup(harkentest::ObjectId(2)).useCount);
    EXPECT_EQ(backpressure + 2u, engine->commands.BackpressureCount());
}

TEST(CompletionMutationGuard,
     PcmGuardIgnoresUnrelatedAndRetiringOldGenerationOwners) {
    VnConfig config = harkentest::MakeNullConfig(2, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> oldSamples = harkentest::MakeDC(48000, 0.25f);
    const std::vector<float> newSamples = harkentest::MakeDC(48000, 0.5f);
    const VnPCMDesc oldPcm = harkentest::MonoDesc(oldSamples, 48000);
    const VnPCMDesc newPcm = harkentest::MonoDesc(newSamples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &oldPcm));
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(2), &oldPcm));
    VnVoiceHandle oldVoice = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &oldVoice));
    const VnVoiceHandle oldRender =
        harkentest::RenderHandle(engine, oldVoice);
    const harken::PCMView oldGeneration = engine->registry.Lookup(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, Vn_ReplacePCM(harkentest::ObjectId(1), &newPcm));
    ASSERT_NE(oldGeneration.useCount, engine->registry.Lookup(harkentest::ObjectId(1)).useCount);

    harken::CompletionMutationBinding mutation{};
    ASSERT_TRUE(PinMutationOwner(
        *engine, static_cast<std::uint32_t>(oldVoice),
        static_cast<std::uint32_t>(oldRender >> 32), 80, 620, &mutation));
    const std::uint64_t backpressure = engine->commands.BackpressureCount();
    EXPECT_EQ(VN_OK, Vn_ReplacePCM(harkentest::ObjectId(2), &newPcm));
    EXPECT_EQ(VN_IN_USE, Vn_ReplacePCM(harkentest::ObjectId(1), &newPcm));
    EXPECT_EQ(backpressure, engine->commands.BackpressureCount());
}

TEST(CompletionMutationGuard,
     StopEmitterRefusesTargetSetAtomicallyWhenPendingOwnerIsPinned) {
    VnConfig config = harkentest::MakeNullConfig(4, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> samples = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    VnEmitterHandle emitter = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&emitter));
    VnPlayDesc play =
        harkentest::MakePlayDesc3D(harkentest::ObjectId(1), 1.0f, 0.0f, 0.0f);
    play.emitterHandle = emitter;
    VnVoiceHandle live = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &live));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, live);
    ASSERT_NE(0u, render);

    VnVoiceHandle pending = 0;
    std::uint32_t pendingSlot = UINT32_MAX;
    ASSERT_TRUE(engine->ledger.AcquirePending(&pending, &pendingSlot, 128));
    engine->pendingStarts[pendingSlot].active = true;
    engine->pendingStarts[pendingSlot].publicHandle = pending;
    engine->pendingStarts[pendingSlot].emitterHandle = emitter;
    VnVoiceHandle parked = 0;
    std::uint32_t parkedSlot = UINT32_MAX;
    ASSERT_TRUE(engine->ledger.AcquirePending(&parked, &parkedSlot, 128));
    engine->ledger.ParkPending(parkedSlot);
    engine->parkRecords[parkedSlot].active = true;
    engine->parkRecords[parkedSlot].spatial.emitterHandle = emitter;

    harken::CompletionMutationBinding mutation{};
    ASSERT_TRUE(PinMutationOwner(*engine, pendingSlot, 71, 81, 621,
                                 &mutation));
    const std::uint64_t commandHead = engine->commands.PublishedHead();
    const std::uint64_t backpressure = engine->commands.BackpressureCount();

    EXPECT_EQ(VN_BACKPRESSURE, Vn_StopEmitter(emitter, 0));

    EXPECT_FALSE(engine->ledger.IsTerminating(render));
    EXPECT_TRUE(engine->pendingStarts[pendingSlot].active);
    EXPECT_TRUE(engine->parkRecords[parkedSlot].active);
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_EQ(backpressure + 1u, engine->commands.BackpressureCount());
}

TEST(CompletionMutationGuard,
     StopAllRefusesEveryTargetBeforeStoppingVoices) {
    VnConfig config = harkentest::MakeNullConfig(4, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> samples = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, voice);
    ASSERT_NE(0u, render);
    ASSERT_NE(0u, voice);

    harken::CompletionMutationBinding mutation{};
    ASSERT_TRUE(PinMutationOwner(
        *engine, static_cast<std::uint32_t>(voice),
        static_cast<std::uint32_t>(render >> 32), 83, 623, &mutation));
    const std::uint64_t commandHead = engine->commands.PublishedHead();
    const std::uint64_t backpressure = engine->commands.BackpressureCount();

    EXPECT_EQ(VN_BACKPRESSURE, Vn_StopAll(0));

    EXPECT_FALSE(engine->ledger.IsTerminating(render));
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_EQ(backpressure + 1u, engine->commands.BackpressureCount());
}

TEST(CompletionMutationGuard,
     VoiceStealProtectsPinnedVictimBeforeClaimantAdmission) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    config.voiceStealPolicy = 2;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> samples = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle victim = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &victim));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, victim);
    ASSERT_NE(0u, render);
    harken::CompletionMutationBinding binding{};
    ASSERT_TRUE(PinMutationOwner(
        *engine, static_cast<std::uint32_t>(victim),
        static_cast<std::uint32_t>(render >> 32), 85, 625, &binding));
    const std::uint64_t commandHead = engine->commands.PublishedHead();
    const std::uint64_t backpressure = engine->commands.BackpressureCount();

    VnVoiceHandle refused = 0;
    EXPECT_EQ(VN_NO_VOICE, harkentest::PlayRaw(&play, &refused));
    EXPECT_EQ(0u, refused);
    EXPECT_FALSE(engine->ledger.IsStolen(render));
    EXPECT_FALSE(engine->ledger.IsTerminating(render));
    EXPECT_EQ(0u, engine->ledger.PendingStartCount());
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_EQ(backpressure, engine->commands.BackpressureCount());

    ASSERT_TRUE(engine->ledger.ClearCompletionMutationExact(
        static_cast<std::uint32_t>(victim), binding));
    ASSERT_TRUE(engine->timelineOccurrences.RollbackCompletionOwnerPinExact(
        binding.occurrence, binding.owner));
    VnVoiceHandle admitted = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &admitted));
    EXPECT_NE(0u, admitted);
    EXPECT_TRUE(engine->ledger.IsStolen(render));
    EXPECT_EQ(1u, engine->ledger.PendingStartCount());
}

TEST(CompletionMutationGuard,
     AutomaticPcmParkRefusesPinnedOwnerBeforeAnyParkMutation) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    config.maxParkedVoices = 1;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> samples = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    VnVoiceHandle voice = 0;
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    const VnVoiceHandle render = harkentest::RenderHandle(engine, voice);
    ASSERT_NE(0u, render);
    engine->nullBackend.Pump(1);

    harken::CompletionMutationBinding binding{};
    ASSERT_TRUE(PinMutationOwner(
        *engine, static_cast<std::uint32_t>(voice),
        static_cast<std::uint32_t>(render >> 32), 41, 401, &binding));
    ASSERT_EQ(VN_OK, Vn_SetVoiceVolume(voice, -80.0f, 0));
    engine->nullBackend.Pump(1);

    const std::uint64_t commandHead = engine->commands.PublishedHead();
    const std::uint64_t backpressure = engine->commands.BackpressureCount();
    ASSERT_EQ(VN_OK, Vn_Update());

    EXPECT_EQ(harken::VoiceLedger::HandleKind::Live,
              engine->ledger.Classify(voice).kind);
    EXPECT_NE(nullptr, engine->voices.Resolve(render));
    EXPECT_EQ(0u, engine->ledger.ParkedCount());
    EXPECT_EQ(0u, engine->voiceParkedCount);
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_EQ(backpressure + 1u, engine->commands.BackpressureCount());
    for (const harken::ParkRecord& rec : engine->parkRecords) {
        EXPECT_FALSE(rec.active);
        EXPECT_TRUE(rec.apiDirect.empty());
        EXPECT_TRUE(rec.bindings.empty());
        EXPECT_TRUE(rec.bindingInstances.empty());
        EXPECT_TRUE(rec.effects.empty());
    }
    harken::CompletionMutationBinding captured{};
    ASSERT_TRUE(engine->ledger.CaptureCompletionMutation(voice, &captured));
    EXPECT_TRUE(harken::SameCompletionMutationBinding(binding, captured));

    ASSERT_TRUE(engine->ledger.ClearCompletionMutationExact(
        static_cast<std::uint32_t>(voice), binding));
    ASSERT_TRUE(engine->timelineOccurrences.RollbackCompletionOwnerPinExact(
        binding.occurrence, binding.owner));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(voice).kind);
    EXPECT_EQ(1u, engine->ledger.ParkedCount());
    EXPECT_EQ(1u, engine->voiceParkedCount);
    EXPECT_EQ(commandHead + 1u, engine->commands.PublishedHead());
    EXPECT_EQ(backpressure + 1u, engine->commands.BackpressureCount());
}

TEST(CompletionMutationGuard,
     DirectVirtualCommitRefusesPinnedPendingBeforeCreatingParkRecord) {
    VnConfig config = harkentest::MakeNullConfig(1, 256, 48000);
    config.maxParkedVoices = 1;
    config.audibilityParkEnabled = 1;
    config.parkThresholdDb = -40.0f;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> samples = harkentest::MakeDC(48000, 0.25f);
    const VnPCMDesc pcm = harkentest::MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &pcm));
    VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1), -80.0f);
    play.reserved0 = VN_PLAY_FLAG_STAGED_INSTALL;
    VnVoiceHandle pending = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &pending));
    const harken::VoiceLedger::Resolved resolved =
        engine->ledger.Classify(pending);
    ASSERT_EQ(harken::VoiceLedger::HandleKind::Pending, resolved.kind);
    ASSERT_TRUE(engine->pendingStarts[resolved.ledgerSlot].stagedInstall);

    harken::CompletionMutationBinding binding{};
    ASSERT_TRUE(PinMutationOwner(*engine, resolved.ledgerSlot, 59, 51, 501,
                                 &binding));
    const std::uint64_t commandHead = engine->commands.PublishedHead();
    const std::uint64_t backpressure = engine->commands.BackpressureCount();

    EXPECT_EQ(VN_BACKPRESSURE, Vn_CommitVoiceStart(pending));

    EXPECT_EQ(harken::VoiceLedger::HandleKind::Pending,
              engine->ledger.Classify(pending).kind);
    EXPECT_TRUE(engine->pendingStarts[resolved.ledgerSlot].active);
    EXPECT_TRUE(engine->pendingStarts[resolved.ledgerSlot].stagedInstall);
    EXPECT_FALSE(engine->parkRecords[resolved.ledgerSlot].active);
    EXPECT_TRUE(engine->parkRecords[resolved.ledgerSlot].apiDirect.empty());
    EXPECT_TRUE(
        engine->parkRecords[resolved.ledgerSlot].bindingInstances.empty());
    EXPECT_EQ(0u, engine->ledger.ParkedCount());
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_EQ(backpressure + 1u, engine->commands.BackpressureCount());
    harken::CompletionMutationBinding captured{};
    ASSERT_TRUE(
        engine->ledger.CaptureCompletionMutation(pending, &captured));
    EXPECT_TRUE(harken::SameCompletionMutationBinding(binding, captured));

    ASSERT_TRUE(engine->ledger.ClearCompletionMutationExact(
        resolved.ledgerSlot, binding));
    ASSERT_TRUE(engine->timelineOccurrences.RollbackCompletionOwnerPinExact(
        binding.occurrence, binding.owner));
    EXPECT_EQ(VN_OK, Vn_CommitVoiceStart(pending));
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Parked,
              engine->ledger.Classify(pending).kind);
    EXPECT_EQ(backpressure + 1u, engine->commands.BackpressureCount());
}

} // namespace
