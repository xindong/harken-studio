// Voice ledger: the logic-side identity layer between the public voice handle
// a caller holds and the internal render-slot handle a voice occupies. This
// file covers the ledger class in isolation (mint, both-way translation, reap,
// generation advance, release, capacity degradation, the correlation-fill
// predicate) and then the pieces only observable through the C API: public-
// handle stale semantics after reuse, the voicePublicHandles correlation
// array, audio-notification handle translation to public identity (proven under a
// forced public/render divergence so it cannot pass by coincidence), the
// logic-side notification queue's drop-oldest counter, the config vintage-6
// acceptance/refusal/defaults, and snapshot old-size compatibility.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/core/VoiceLedger.h"
#include "harken/core/DecisionTable.h"
#include "harken/core/LogicCommitCoordinator.h"
#include "harken/core/OccurrenceStatePool.h"
#include "harken/core/CompositeCompletionOwnerPool.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <iterator>
#include <type_traits>
#include <utility>
#include <vector>


#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
namespace harken::detail {
struct VoiceLedgerOriginalCompletionTestAccess {
    // These are observation or negative prerequisites, never cap issuers.
    static void SetMarkerMax(VoiceLedger& ledger) {
        ledger.m_lastOriginalCompletionMarker = UINT64_MAX;
    }
    static void SetCount(VoiceLedger& ledger, std::uint32_t slot, std::uint32_t count) {
        ASSERT_NE(0u, ledger.m_entries[slot].originalCompletion.marker);
        ASSERT_NE(0u, ledger.m_entries[slot].originalCompletion.externalCount);
        ASSERT_NE(0u, count);
        ledger.m_entries[slot].originalCompletion.externalCount = count;
    }
};
}
#endif

namespace {

using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlock = 256;

VnVoiceHandle MakeRenderHandle(std::uint32_t slot, std::uint32_t generation) {
    return (static_cast<std::uint64_t>(generation) << 32) | slot;
}

// --- ledger class in isolation ---

bool PrepareLedgerCommitInput(harken::VoiceLedger& ledger, VnVoiceHandle render,
                             harken::VoiceLedger::PreparedCommitInput* input) {
    input->parent = {UINT64_C(0x8000000700000002), UINT64_C(0x0000000700000002)};
    input->renderHandle = render;
    std::uint32_t slot = UINT32_MAX;
    return ledger.PreparePlayableOwned(input->parent, &slot, &input->identity) &&
        slot == input->identity.slot;
}

TEST(VoiceLedger, PreparedProofExcludesRealMutationAndAbortPreservesOriginalCancel) {
    using Proof = harken::VoiceLedger::PreparedCommitProof;
    static_assert(!std::is_copy_constructible_v<Proof>);
    static_assert(!std::is_copy_assignable_v<Proof>);
    static_assert(std::is_nothrow_move_constructible_v<Proof>);
    harken::VoiceLedger ledger(1);
    harken::VoiceLedger::PreparedCommitInput input{};
    ASSERT_TRUE(PrepareLedgerCommitInput(ledger, MakeRenderHandle(0, 7), &input));
    input.completionMutation = {{3, 8}, harken::CompletionOwnerRef::Single({2, 9}, 11)};
    ASSERT_TRUE(ledger.BindCompletionMutation(input.identity.slot, input.completionMutation));
    Proof held;
    ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(input, &held));
    EXPECT_FALSE(ledger.CommitPreparedPlayableOwned(input.identity.slot));
    EXPECT_FALSE(ledger.CancelPreparedPlayableOwned(input.identity.slot));
    ledger.Release(input.identity.slot);
    ledger.BindRender(input.identity.slot, input.renderHandle, 19);
    EXPECT_FALSE(ledger.OwnerForRender(input.renderHandle).live);
    EXPECT_EQ(0u, ledger.AvailableEntryCount());
    EXPECT_FALSE(ledger.SetGenerationForTests(input.identity.slot, 19));
    EXPECT_FALSE(ledger.ClearCompletionMutationExact(input.identity.slot, input.completionMutation));
    auto replacement = input.completionMutation;
    ++replacement.owner.factsRevision;
    EXPECT_FALSE(ledger.ReplaceCompletionMutationExact(input.identity.slot,
        input.completionMutation, replacement));
    EXPECT_FALSE(ledger.BindCompletionMutation(input.identity.slot, replacement));
    harken::CompletionMutationBinding captured{};
    ASSERT_TRUE(ledger.CaptureCompletionMutationForSlot(input.identity.slot, &captured));
    EXPECT_TRUE(harken::SameCompletionMutationBinding(captured, input.completionMutation));
    Proof moved(std::move(held));
    EXPECT_FALSE(held.Valid());
    EXPECT_TRUE(moved.Valid());
    EXPECT_FALSE(ledger.ProvePreparedPlayableCommit(input, &moved));
    moved.Release();
    ASSERT_TRUE(ledger.ClearCompletionMutationExact(input.identity.slot, input.completionMutation));
    ASSERT_TRUE(ledger.CancelPreparedPlayableOwned(input.identity.slot));
    EXPECT_EQ(ledger.Capacity(), ledger.AvailableEntryCount());
}

TEST(VoiceLedger, PreparedIssuanceRejectsSameSlotSameParentReuseAndQuarantinesMaximum) {
    harken::VoiceLedger ledger(1);
    harken::VoiceLedger::PreparedCommitInput first{};
    ASSERT_TRUE(PrepareLedgerCommitInput(ledger, MakeRenderHandle(0, 7), &first));
    ASSERT_TRUE(ledger.CancelPreparedPlayableOwned(first.identity.slot));
    harken::VoiceLedger::PreparedCommitInput second{};
    ASSERT_TRUE(PrepareLedgerCommitInput(ledger, first.renderHandle, &second));
    ASSERT_EQ(first.identity.slot, second.identity.slot);
    ASSERT_NE(first.identity.generation, second.identity.generation);
    harken::VoiceLedger::PreparedCommitProof proof;
    EXPECT_FALSE(ledger.ProvePreparedPlayableCommit(first, &proof));
    ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(second, &proof));
    proof.Release();
    ASSERT_TRUE(ledger.CancelPreparedPlayableOwned(second.identity.slot));
    ASSERT_TRUE(ledger.SetGenerationForTests(0, harken::VoiceLedger::kMaxGeneration));
    harken::VoiceLedger::PreparedCommitInput maximum{};
    ASSERT_TRUE(PrepareLedgerCommitInput(ledger, first.renderHandle, &maximum));
    ASSERT_EQ(harken::VoiceLedger::kMaxGeneration, maximum.identity.generation);
    ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(maximum, &proof));
    proof.Release();
    ASSERT_TRUE(ledger.CancelPreparedPlayableOwned(maximum.identity.slot));
    EXPECT_EQ(1u, ledger.QuarantinedCount());
    EXPECT_EQ(0u, ledger.AvailableEntryCount());
    EXPECT_FALSE(PrepareLedgerCommitInput(ledger, first.renderHandle, &second));
    EXPECT_FALSE(ledger.ProvePreparedPlayableCommit(maximum, &proof));
}

TEST(VoiceLedger, PreparedProofConsumesFrozenBindingOnceAndNormalReleaseAdvancesIdentity) {
    harken::VoiceLedger ledger(1);
    harken::VoiceLedger::PreparedCommitInput input{};
    ASSERT_TRUE(PrepareLedgerCommitInput(ledger, MakeRenderHandle(0, 7), &input));
    input.completionMutation = {{3, 8}, harken::CompletionOwnerRef::Single({2, 9}, 11)};
    ASSERT_TRUE(ledger.BindCompletionMutation(input.identity.slot, input.completionMutation));
    const auto original = input;
    harken::VoiceLedger::PreparedCommitProof proof;
    ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(input, &proof));
    input.renderHandle = MakeRenderHandle(0, 19);
    input.parent = {};
    input.completionMutation = {};
    harken::VoiceLedger::ConsumePreparedPlayableCommit(std::move(proof));
    EXPECT_FALSE(proof.Valid());
    EXPECT_FALSE(ledger.CommitPreparedPlayableOwned(original.identity.slot));
    EXPECT_FALSE(ledger.CancelPreparedPlayableOwned(original.identity.slot));
    const auto owner = ledger.OwnerForRender(original.renderHandle);
    ASSERT_TRUE(owner.live);
    EXPECT_EQ(original.identity.slot, owner.ledgerSlot);
    EXPECT_EQ(original.parent.previewHandle, owner.parent.previewHandle);
    EXPECT_EQ(original.parent.internalToken, owner.parent.internalToken);
    EXPECT_EQ(harken::VoiceLedger::PublicVisibility::PlayableOwnedInternal, owner.visibility);
    EXPECT_FALSE(ledger.OwnerForRender(input.renderHandle).live);
    harken::CompletionMutationBinding captured{};
    ASSERT_TRUE(ledger.CaptureCompletionMutationForRender(original.renderHandle, &captured));
    EXPECT_TRUE(harken::SameCompletionMutationBinding(captured, original.completionMutation));
    EXPECT_FALSE(ledger.ProvePreparedPlayableCommit(original, &proof));
    ledger.Release(original.identity.slot);
    EXPECT_EQ(1u, ledger.AvailableEntryCount());
    EXPECT_FALSE(ledger.OwnerForRender(original.renderHandle).live);
    ASSERT_TRUE(PrepareLedgerCommitInput(ledger, original.renderHandle, &input));
    EXPECT_NE(original.identity.generation, input.identity.generation);
    EXPECT_FALSE(ledger.ProvePreparedPlayableCommit(original, &proof));
    ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(input, &proof));
    proof.Release();
    ASSERT_TRUE(ledger.CancelPreparedPlayableOwned(input.identity.slot));
}

TEST(VoiceLedger, PreparedProofReverseContentionAndMoveAssignmentReleaseRealPrefix) {
    harken::VoiceLedger ledger(2, 2);
    harken::VoiceLedger::PreparedCommitInput first{}, second{};
    ASSERT_TRUE(PrepareLedgerCommitInput(ledger, MakeRenderHandle(0, 7), &first));
    ASSERT_TRUE(PrepareLedgerCommitInput(ledger, first.renderHandle, &second));
    ASSERT_NE(first.identity.slot, second.identity.slot);
    harken::VoiceLedger::PreparedCommitProof firstProof, secondProof;
    ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(first, &firstProof));
    EXPECT_FALSE(ledger.ProvePreparedPlayableCommit(second, &secondProof));
    ledger.BindRender(second.identity.slot, second.renderHandle, 19);
    EXPECT_FALSE(ledger.OwnerForRender(second.renderHandle).live);
    second.renderHandle = MakeRenderHandle(1, 7);
    ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(second, &secondProof));
    firstProof = std::move(secondProof);
    EXPECT_FALSE(secondProof.Valid());
    ASSERT_TRUE(ledger.CancelPreparedPlayableOwned(first.identity.slot));
    EXPECT_FALSE(ledger.CancelPreparedPlayableOwned(second.identity.slot));
    firstProof.Release();
    ASSERT_TRUE(ledger.CancelPreparedPlayableOwned(second.identity.slot));
    EXPECT_EQ(ledger.Capacity(), ledger.AvailableEntryCount());
}

TEST(VoiceLedger, PreparedProofReverseReservationSurvivesOldReapAndBlocksPendingAndParkedBind) {
    harken::VoiceLedger ledger(4, 1);
    VnVoiceHandle oldPublic = 0, pendingPublic = 0, parkedPublic = 0;
    std::uint32_t oldSlot = 0, pendingSlot = 0, parkedSlot = 0;
    ASSERT_TRUE(ledger.Acquire(&oldPublic, &oldSlot));
    const auto oldRender = MakeRenderHandle(0, 3);
    ledger.BindRender(oldSlot, oldRender, 128);
    ASSERT_TRUE(ledger.AcquirePending(&pendingPublic, &pendingSlot, 128));
    ASSERT_TRUE(ledger.AcquirePending(&parkedPublic, &parkedSlot, 128));
    ledger.ParkPending(parkedSlot);
    harken::VoiceLedger::PreparedCommitInput input{};
    ASSERT_TRUE(PrepareLedgerCommitInput(ledger, MakeRenderHandle(0, 7), &input));
    harken::VoiceLedger::PreparedCommitProof proof;
    ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(input, &proof));
    ledger.Reap(oldRender);
    EXPECT_FALSE(ledger.OwnerForRender(oldRender).live);
    ledger.StartPending(pendingSlot, input.renderHandle);
    ledger.ReturnPark(parkedSlot, input.renderHandle);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Pending, ledger.Classify(pendingPublic).kind);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Parked, ledger.Classify(parkedPublic).kind);
    EXPECT_FALSE(ledger.OwnerForRender(input.renderHandle).live);
    harken::VoiceLedger::ConsumePreparedPlayableCommit(std::move(proof));
    ASSERT_TRUE(ledger.OwnerForRender(input.renderHandle).live);
    ledger.Reap(oldRender);
    EXPECT_TRUE(ledger.OwnerForRender(input.renderHandle).live);
    ledger.Release(input.identity.slot);
    ledger.Release(pendingSlot);
    ledger.Release(parkedSlot);
    EXPECT_EQ(ledger.Capacity(), ledger.AvailableEntryCount());
}

TEST(VoiceLedger, PreparedProofRejectsWrongOriginalIdentityAndHiddenLiveRenderOwner) {
    harken::VoiceLedger ledger(3, 2);
    harken::VoiceLedger::PreparedCommitInput input{};
    ASSERT_TRUE(PrepareLedgerCommitInput(ledger, MakeRenderHandle(0, 7), &input));
    input.completionMutation = {{3, 8}, harken::CompletionOwnerRef::Single({2, 9}, 11)};
    ASSERT_TRUE(ledger.BindCompletionMutation(input.identity.slot, input.completionMutation));
    for (unsigned index = 0; index < 12; ++index) {
        SCOPED_TRACE(index);
        auto wrong = input;
        switch (index) {
        case 0: wrong.identity.slot = UINT32_MAX; break;
        case 1: ++wrong.identity.generation; break;
        case 2: ++wrong.parent.previewHandle; break;
        case 3: ++wrong.parent.internalToken; break;
        case 4: wrong.renderHandle = 0; break;
        case 5: wrong.renderHandle = 1; break;
        case 6: wrong.renderHandle = MakeRenderHandle(2, 7); break;
        case 7: ++wrong.completionMutation.occurrence.slot; break;
        case 8: ++wrong.completionMutation.occurrence.generation; break;
        case 9: ++wrong.completionMutation.owner.singleDecision.generation; break;
        case 10: ++wrong.completionMutation.owner.factsRevision; break;
        case 11: wrong.completionMutation = {}; break;
        }
        harken::VoiceLedger::PreparedCommitProof rejected;
        EXPECT_FALSE(ledger.ProvePreparedPlayableCommit(wrong, &rejected));
        EXPECT_FALSE(rejected.Valid());
    }
    VnVoiceHandle firstPublic = 0, secondPublic = 0;
    std::uint32_t firstSlot = 0, secondSlot = 0;
    ASSERT_TRUE(ledger.Acquire(&firstPublic, &firstSlot));
    ASSERT_TRUE(ledger.Acquire(&secondPublic, &secondSlot));
    ledger.BindRender(firstSlot, input.renderHandle, 128);
    ledger.BindRender(secondSlot, MakeRenderHandle(0, 19), 128);
    harken::VoiceLedger::PreparedCommitProof proof;
    EXPECT_FALSE(ledger.ProvePreparedPlayableCommit(input, &proof));
    ledger.Release(firstSlot);
    ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(input, &proof));
    proof.Release();
    ASSERT_TRUE(ledger.CancelPreparedPlayableOwned(input.identity.slot));
    ledger.Release(secondSlot);
    EXPECT_EQ(ledger.Capacity(), ledger.AvailableEntryCount());
}

TEST(VoiceLedger, MintFormsNonZeroHandleAndTranslatesBothWays) {
    harken::VoiceLedger ledger(4);
    VnVoiceHandle pub = 0;
    std::uint32_t slot = 0;
    ASSERT_TRUE(ledger.Acquire(&pub, &slot));
    EXPECT_NE(pub, 0u); // handle 0 is never valid

    // Reserved but not yet bound: there is no render handle to resolve to.
    EXPECT_EQ(0u, ledger.ToRender(pub));

    const VnVoiceHandle render = MakeRenderHandle(2, 1);
    ledger.BindRender(slot, render, 128);
    EXPECT_EQ(render, ledger.ToRender(pub)); // public -> render
    EXPECT_EQ(pub, ledger.ToPublic(render)); // render -> public

    // A handle with the right slot but a wrong generation is stale.
    const VnVoiceHandle wrongGen =
        (static_cast<std::uint64_t>(999) << 32) |
        static_cast<std::uint32_t>(pub);
    EXPECT_EQ(0u, ledger.ToRender(wrongGen));
    EXPECT_EQ(0u, ledger.ToRender(0));
}

TEST(VoiceLedger, ReapMakesHandleStaleAndAdvancesGeneration) {
    harken::VoiceLedger ledger(2);
    VnVoiceHandle pub = 0;
    std::uint32_t slot = 0;
    ASSERT_TRUE(ledger.Acquire(&pub, &slot));
    const VnVoiceHandle render = MakeRenderHandle(0, 5);
    ledger.BindRender(slot, render, 128);

    ledger.Reap(render);
    // The reaped public handle no longer resolves, and the render handle no
    // longer maps to any live entry.
    EXPECT_EQ(0u, ledger.ToRender(pub));
    EXPECT_EQ(0u, ledger.ToPublic(render));

    // Reusing the same (Ended) slot mints a distinct handle: the ledger-slot
    // low bits match but the generation advanced, so the stale handle can
    // never be confused with the fresh one.
    VnVoiceHandle pub2 = 0;
    std::uint32_t slot2 = 0;
    ASSERT_TRUE(ledger.Acquire(&pub2, &slot2));
    EXPECT_EQ(slot, slot2);
    EXPECT_NE(pub, pub2);
    EXPECT_EQ(static_cast<std::uint32_t>(pub),
              static_cast<std::uint32_t>(pub2));
}

TEST(VoiceLedger, ReleaseUndoesAReservationSideEffectFree) {
    harken::VoiceLedger ledger(1);
    VnVoiceHandle pub = 0;
    std::uint32_t slot = 0;
    ASSERT_TRUE(ledger.Acquire(&pub, &slot));
    ledger.Release(slot);
    EXPECT_EQ(0u, ledger.ToRender(pub)); // the discarded handle is stale

    // The single slot is available again and the next acquire gets a fresh
    // handle (advanced generation).
    VnVoiceHandle pub2 = 0;
    std::uint32_t slot2 = 0;
    ASSERT_TRUE(ledger.Acquire(&pub2, &slot2));
    EXPECT_NE(pub, pub2);
}

TEST(VoiceLedger, AcquireFailsWhenEveryEntryIsLive) {
    harken::VoiceLedger ledger(2);
    VnVoiceHandle a = 0;
    VnVoiceHandle b = 0;
    VnVoiceHandle c = 0;
    std::uint32_t sa = 0;
    std::uint32_t sb = 0;
    std::uint32_t sc = 0;
    ASSERT_TRUE(ledger.Acquire(&a, &sa));
    ASSERT_TRUE(ledger.Acquire(&b, &sb));
    // Full: a defined degradation (the caller maps this to VN_NO_VOICE), not
    // a crash.
    EXPECT_FALSE(ledger.Acquire(&c, &sc));
}

TEST(VoiceLedger, PlayableOwnedVoiceKeepsExactParentButHasNoPublicVoice) {
    harken::VoiceLedger ledger(1);
    harken::VoiceLedger::PlayableParentRef parent{};
    parent.previewHandle = UINT64_C(0x8000000100000000);
    parent.internalToken = UINT64_C(0x0000000100000000);
    std::uint32_t slot = UINT32_MAX;
    ASSERT_TRUE(ledger.AcquirePlayableOwned(parent, &slot));
    const VnVoiceHandle render = UINT64_C(0x0000000300000002);
    ledger.BindRender(slot, render, 128);

    EXPECT_EQ(ledger.ToPublic(render), 0u);
    EXPECT_EQ(ledger.PublicHandleForSlot(slot), 0u);
    const harken::VoiceLedger::RenderOwner owner =
        ledger.OwnerForRender(render);
    ASSERT_TRUE(owner.live);
    EXPECT_EQ(owner.visibility,
              harken::VoiceLedger::PublicVisibility::PlayableOwnedInternal);
    EXPECT_EQ(owner.parent.previewHandle, parent.previewHandle);
    EXPECT_EQ(owner.parent.internalToken, parent.internalToken);
    std::uint64_t handles[4] = {1, 1, 1, 1};
    ledger.FillPublicHandles(handles, 4,
                             [](VnVoiceHandle) { return true; });
    EXPECT_TRUE(std::all_of(std::begin(handles), std::end(handles),
                            [](std::uint64_t value) { return value == 0; }));

    ledger.Reap(render);
    EXPECT_FALSE(ledger.OwnerForRender(render).live);
}

TEST(VoiceLedger, MaximumGenerationRetiresIntoQuarantineWithoutSettingBit63) {
    harken::VoiceLedger ledger(1);
    ASSERT_TRUE(ledger.SetGenerationForTests(
        0, harken::VoiceLedger::kMaxGeneration));
    VnVoiceHandle handle = 0;
    std::uint32_t slot = UINT32_MAX;
    ASSERT_TRUE(ledger.Acquire(&handle, &slot));
    EXPECT_EQ(handle & (UINT64_C(1) << 63), 0u);
    ledger.Release(slot);
    EXPECT_EQ(ledger.QuarantinedCount(), 1u);

    VnVoiceHandle next = 0;
    std::uint32_t nextSlot = UINT32_MAX;
    EXPECT_FALSE(ledger.Acquire(&next, &nextSlot));
}

TEST(VoiceLedger, FillPublicHandlesSelectsTheLiveOwnerPerRenderSlot) {
    harken::VoiceLedger ledger(4);
    VnVoiceHandle a = 0;
    VnVoiceHandle b = 0;
    std::uint32_t sa = 0;
    std::uint32_t sb = 0;
    ASSERT_TRUE(ledger.Acquire(&a, &sa));
    ASSERT_TRUE(ledger.Acquire(&b, &sb));
    const VnVoiceHandle rA = MakeRenderHandle(0, 1);
    const VnVoiceHandle rB = MakeRenderHandle(1, 1);
    ledger.BindRender(sa, rA, 128);
    ledger.BindRender(sb, rB, 128);

    std::uint64_t out[4] = {7, 7, 7, 7};
    // Only rA is reported live: rB's render row must stay 0, proving the
    // liveness predicate (not the stored render handle alone) chooses the
    // current owner — the ended-but-unreaped case.
    ledger.FillPublicHandles(out, 4,
                             [rA](VnVoiceHandle h) { return h == rA; });
    EXPECT_EQ(a, out[0]);  // render slot 0, live
    EXPECT_EQ(0u, out[1]); // render slot 1, predicate said dead
    EXPECT_EQ(0u, out[2]);
    EXPECT_EQ(0u, out[3]);
}

// --- public-handle semantics through the C API ---

TEST(VoiceLedger, PublicHandleGoesStaleOnRenderEndAndAfterReap) {
    EngineSession session(MakeNullConfig(2, kBlock, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> shortSound = harkentest::MakeDC(64, 0.5f);
    const VnPCMDesc desc = MonoDesc(shortSound, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    // The 64-frame sound ends inside one block; the render slot frees, but
    // the ledger entry is not reaped until the end notification drains in
    // Vn_Update.
    engine->nullBackend.Pump(1);
    const std::uint64_t staleBefore = engine->voices.StaleHandleCount();
    // Render dead, ledger entry still Live: an op is a benign stale no-op,
    // counted exactly once (the render-liveness check owns the counter).
    EXPECT_EQ(VN_STALE_HANDLE, Vn_SetVoiceVolume(handle, -3.0f, 0));
    EXPECT_EQ(staleBefore + 1, engine->voices.StaleHandleCount());

    // After the pump the end notification is queued; draining it reaps the entry.
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(VN_STALE_HANDLE, Vn_Stop(handle, 0));
    EXPECT_EQ(staleBefore + 2, engine->voices.StaleHandleCount());
}

TEST(VoiceLedger, SnapshotCorrelatesEveryPublicHandleToItsRow) {
    EngineSession session(MakeNullConfig(4, kBlock, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> sine =
        harkentest::MakeSine(48000, 440.0, 48000.0, 0.5f);
    const VnPCMDesc desc = MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handles[3] = {0, 0, 0};
    for (VnVoiceHandle& h : handles) {
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &h));
    }
    engine->nullBackend.Pump(2);

    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));

    // Each returned public handle joins to exactly one render row through the
    // correlation array, the rows are distinct, and each is ACTIVE.
    int rows[3] = {-1, -1, -1};
    for (int i = 0; i < 3; ++i) {
        rows[i] = harkentest::VoiceRowForHandle(snapshot, handles[i]);
        ASSERT_GE(rows[i], 0);
        EXPECT_EQ(static_cast<std::uint32_t>(VN_VOICE_STATE_ACTIVE),
                  snapshot.voices[rows[i]].state);
    }
    EXPECT_NE(rows[0], rows[1]);
    EXPECT_NE(rows[1], rows[2]);
    EXPECT_NE(rows[0], rows[2]);
}

struct NotificationLog {
    std::vector<VnNotification> notifications;
};

void RecordNotification(const VnNotification* notification, void* userData) {
    static_cast<NotificationLog*>(userData)->notifications.push_back(*notification);
}

TEST(VoiceLedger, EndNotificationsCarryThePublicHandleUnderForcedDivergence) {
    // A single render slot, so replaying reuses render slot 0 while the first
    // voice's ledger entry still lingers — the public and render handles of
    // the second voice diverge, so a callback that reports the public handle
    // proves real translation rather than a coincidental equality.
    NotificationLog log;
    VnConfig config = MakeNullConfig(1, kBlock, 48000);
    config.onNotification = &RecordNotification;
    config.notificationUserData = &log;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> shortSound = harkentest::MakeDC(64, 0.5f);
    const VnPCMDesc desc = MonoDesc(shortSound, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle first = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &first));
    // The sound ends in one block; do NOT pump Vn_Update, so the first
    // ledger entry lingers and the render slot is reused below.
    engine->nullBackend.Pump(1);

    VnVoiceHandle second = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &second));
    // Divergence: the second voice's public handle is not its render handle.
    EXPECT_NE(second, harkentest::RenderHandle(engine, second));
    engine->nullBackend.Pump(1); // second voice ends too

    // Both end notifications drain now, each rewritten to its public handle
    // in queue order (first, then second).
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(2u, log.notifications.size());
    EXPECT_EQ(first, log.notifications[0].voiceHandle);
    EXPECT_EQ(second, log.notifications[1].voiceHandle);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_NOTIFY_VOICE_ENDED),
              log.notifications[0].type);
    EXPECT_EQ(static_cast<std::uint32_t>(VN_NOTIFY_VOICE_ENDED),
              log.notifications[1].type);
}

TEST(VoiceLedger, LogicNotificationQueueDropsOldestAndCountsThem) {
    // The logic-side notification queue is wired and drained after the translated
    // audio notifications. No feature emits into it yet, so this drives it directly
    // to prove the drop-oldest counter surfaces in the snapshot.
    NotificationLog log;
    VnConfig config = MakeNullConfig(2, kBlock, 48000);
    config.maxParkedVoices = 2; // small ledger => small logic queue
    config.onNotification = &RecordNotification;
    config.notificationUserData = &log;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::uint32_t cap = engine->logicNotifications.Capacity();
    const std::uint32_t overflow = 3;
    for (std::uint32_t i = 0; i < cap + overflow; ++i) {
        VnNotification notification{};
        notification.type = VN_NOTIFY_DEGRADATION_OCCURRED;
        notification.blockTimestamp = i;
        engine->logicNotifications.Push(notification);
    }

    // Draining delivers the surviving newest notifications and counts the
    // dropped oldest ones. The survivor window is capacity - 1, so the boundary
    // record the producer's next Push would reclaim is dropped alongside the
    // overflow; delivered == cap - 1 and the drop count == overflow + 1 surface
    // together.
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(cap - 1, log.notifications.size());

    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    EXPECT_EQ(static_cast<std::uint64_t>(overflow + 1),
              snapshot.logicNotificationDropCount);
}

TEST(VoiceLedger, DroppedTerminalNotificationStillReclaimsTheLedgerEntry) {
    // Identity reclamation must not hinge on notification delivery, which
    // is advisory and droppable. A requested depth of 1 creates a physical
    // capacity of 2 with a one-record survivor window; paired with a single
    // render slot, this forces an ENDED notification to be overwritten before
    // Vn_Update drains it;
    // the ledger must still reclaim the orphaned entry (via the post-drain
    // sweep) so it never leaks capacity into a false exhaustion.
    VnConfig config = MakeNullConfig(1, kBlock, 48000);
    config.maxParkedVoices = 1; // ledger capacity = maxVoices + maxParkedVoices = 2
    // Requested depth 1 yields a one-record survivor window, so an older
    // notification is overwritten.
    config.notificationQueueDepth = 1;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(2u, engine->ledger.Capacity());

    const std::vector<float> shortSound = harkentest::MakeDC(64, 0.5f);
    const VnPCMDesc desc = MonoDesc(shortSound, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    const VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));

    // Two voices reuse the single render slot without an intervening
    // Vn_Update, so both ledger entries linger Live over a dead render slot;
    // their ENDED notifications collide in the one-record survivor window
    // (the first is dropped).
    VnVoiceHandle first = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &first));
    engine->nullBackend.Pump(1); // first ends, ENDED queued
    VnVoiceHandle second = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &second));
    engine->nullBackend.Pump(1); // second ends, ENDED overwrites the first

    // Both ledger entries are occupied by ended-but-unreaped voices: without
    // reclamation a third play is falsely refused even though a render slot is
    // free — this is the leak the sweep prevents.
    VnVoiceHandle refused = 0;
    EXPECT_EQ(VN_NO_VOICE, harkentest::PlayRaw(&play, &refused));

    ASSERT_EQ(VN_OK, Vn_Update());

    // A terminal notification was provably dropped, so the drain alone could
    // not have reaped both entries — the sweep did.
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    EXPECT_GE(snapshot.notificationDropCount, 1u);

    // Identity reclaimed: neither stale public handle resolves, and a fresh
    // play now succeeds.
    EXPECT_EQ(0u, harkentest::RenderHandle(engine, first));
    EXPECT_EQ(0u, harkentest::RenderHandle(engine, second));
    VnVoiceHandle third = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &third));
}

// --- config vintage 6 ---

TEST(VoiceLedger, ConfigVintageSixAcceptanceRefusalAndDefaults) {
    const std::uint32_t vintage5 =
        static_cast<std::uint32_t>(offsetof(VnConfig, maxActions));

    {
        // Full struct with the playback fields zeroed resolves every default;
        // the ledger capacity is maxVoices + maxParkedVoices.
        VnConfig config = MakeNullConfig(4, kBlock, 48000);
        config.maxParkedVoices = 8;
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        EXPECT_EQ(12u, session.Engine()->ledger.Capacity()); // 4 + 8
    }
    {
        // Playback fields left zero: maxParkedVoices resolves to its default
        // (256), so the ledger is maxVoices + 256.
        VnConfig config = MakeNullConfig(4, kBlock, 48000);
        EngineSession session(config);
        ASSERT_EQ(VN_OK, session.InitResult());
        EXPECT_EQ(260u, session.Engine()->ledger.Capacity()); // 4 + 256
    }
    {
        // The pre-playback vintage is accepted (its appended fields default).
        VnConfig config = MakeNullConfig(4, kBlock, 48000);
        config.structSize = vintage5;
        ASSERT_EQ(VN_OK, Vn_Init(&config));
        EXPECT_EQ(260u, harken::detail::ActiveEngine()->ledger.Capacity());
        EXPECT_EQ(VN_OK, Vn_Term());
    }
    {
        // A size between the playback vintage and the full struct is not a
        // vintage: refused.
        VnConfig config = MakeNullConfig(4, kBlock, 48000);
        config.structSize = vintage5 + 4;
        EXPECT_EQ(VN_INVALID_ARG, Vn_Init(&config));
    }
    {
        // Out-of-range playback fields are refused as invalid config.
        VnConfig config = MakeNullConfig(4, kBlock, 48000);
        config.voiceStealPolicy = 3; // only 0..2 are defined
        EXPECT_EQ(VN_INVALID_CONFIG, Vn_Init(&config));
    }
    {
        VnConfig config = MakeNullConfig(4, kBlock, 48000);
        config.audibilityParkEnabled = 2; // 0/1 only
        EXPECT_EQ(VN_INVALID_CONFIG, Vn_Init(&config));
    }
}

// --- snapshot old-size compatibility ---

TEST(VoiceLedger, SnapshotOldSizeFillsPrefixAndLeavesTailUntouched) {
    EngineSession session(MakeNullConfig(2, kBlock, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    const std::vector<float> sine =
        harkentest::MakeSine(48000, 440.0, 48000.0, 0.5f);
    const VnPCMDesc desc = MonoDesc(sine, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    engine->nullBackend.Pump(2);

    const std::uint32_t oldSize =
        static_cast<std::uint32_t>(offsetof(VnSnapshot, voicePublicHandles));

    // An old-size request fills the common prefix but must not write the
    // appended playback block: pre-seed a sentinel there and confirm it
    // survives the capture.
    VnSnapshot snapshot{};
    snapshot.structSize = oldSize;
    snapshot.voicePublicHandles[0] = 0xD00Dull;
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    EXPECT_EQ(1u, snapshot.activeVoiceCount); // prefix filled
    EXPECT_EQ(0xD00Dull, snapshot.voicePublicHandles[0]); // tail untouched

    // A full-size request does fill the correlation array.
    VnSnapshot full{};
    full.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&full));
    EXPECT_GE(harkentest::VoiceRowForHandle(full, handle), 0);

    // The middle (playback-management) vintage: a struct ending just before the
    // appended logic-log counter. It fills the playback block (so the
    // correlation array is written) but must not write logicLogDropCount —
    // pre-seed a sentinel there and confirm it survives, the same discipline as
    // the old-size prefix check above.
    const std::uint32_t midSize =
        static_cast<std::uint32_t>(offsetof(VnSnapshot, logicLogDropCount));
    VnSnapshot mid{};
    mid.structSize = midSize;
    mid.logicLogDropCount = 0xBEEFu; // sentinel in the not-yet-written tail
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&mid));
    EXPECT_GE(harkentest::VoiceRowForHandle(mid, handle), 0); // playback filled
    EXPECT_EQ(0xBEEFu, mid.logicLogDropCount);               // tail untouched

    // Sizes that match no vintage are refused. oldSize + 8 sits above the
    // pre-playback vintage but below the middle vintage; sizeof - 4 sits between
    // the middle vintage and the full struct — neither is a defined structSize.
    VnSnapshot bad{};
    bad.structSize = oldSize + 8;
    EXPECT_EQ(VN_INVALID_ARG, Vn_CaptureSnapshot(&bad));
    bad.structSize = sizeof(VnSnapshot) - 4;
    EXPECT_EQ(VN_INVALID_ARG, Vn_CaptureSnapshot(&bad));
}



// Only the genuine Ledger proof issues the tested cap. Native Decision,
// Occurrence and parent identities below are fixture inputs, not grants of
// their structural lifetime or physical Source/Current/Reset authority.
struct OriginalLedgerNativeFixture {
    harken::LogicCommitCoordinator coordinator;
    harken::DecisionTable decisions{16, 1};
    harken::PlayableInstanceTable instances{1};
    harken::OccurrenceStatePool occurrences{16};
    harken::PlayableInstanceHandle root = 0;
    std::vector<harken::DecisionToken> decisionTokens;
    std::vector<harken::CompletionMutationBinding> bindings;
    std::vector<std::uint32_t> renderGenerations;

    bool MakeBinding(std::uint32_t renderGeneration, harken::CompletionMutationBinding* out) {
        if (out == nullptr) return false;
        if (root == 0) root = instances.Acquire(harken::PlayableKind::Container, 0);
        if (root == 0) return false;
        harken::DecisionReserveRequest reserve{};
        reserve.ticket.sequence = coordinator.Issue();
        reserve.world = 7;
        reserve.transport = 1;
        reserve.expectedContinuationGeneration = 1;
        reserve.sliceCount = 1;
        harken::DecisionToken decision{};
        if (!decisions.Reserve(reserve, &decision)) return false;
        decisionTokens.push_back(decision);
        harken::OccurrenceToken occurrence{};
        if (!occurrences.Acquire({root, harkentest::ObjectId(73), 0,
            static_cast<std::uint32_t>(bindings.size())}, &occurrence)) return false;
        const auto owner = harken::CompletionOwnerRef::Single(decision, 1);
        if (!occurrences.BindCompletionProgress(occurrence, renderGeneration, 1) ||
            !occurrences.TryPinCompletionOwner(occurrence, owner)) return false;
        *out = {occurrence, owner};
        bindings.push_back(*out);
        renderGenerations.push_back(renderGeneration);
        return true;
    }

    bool Prepare(harken::VoiceLedger& ledger, VnVoiceHandle render,
        harken::VoiceLedger::PreparedCommitInput* out) {
        if (out == nullptr || !MakeBinding(static_cast<std::uint32_t>(render >> 32),
            &out->completionMutation)) return false;
        out->parent = {0, root};
        out->renderHandle = render;
        std::uint32_t slot = UINT32_MAX;
        return ledger.PreparePlayableOwned(out->parent, &slot, &out->identity) &&
            slot == out->identity.slot && ledger.BindCompletionMutation(slot, out->completionMutation);
    }

    void ReleaseAll() {
        for (std::size_t i = 0; i < bindings.size(); ++i) {
            // No audio was submitted for this component-only fixture. Return
            // the actual unpublished owner through its native rollback path.
            EXPECT_TRUE(occurrences.RollbackCompletionOwnerPinExact(bindings[i].occurrence, bindings[i].owner));
            EXPECT_TRUE(occurrences.UnbindCompletionProgress(bindings[i].occurrence, renderGenerations[i]));
            EXPECT_TRUE(occurrences.Advance(bindings[i].occurrence, harken::OccurrenceStage::AwaitingEventFence));
            EXPECT_TRUE(occurrences.ReleaseAfterFence(bindings[i].occurrence));
        }
        bindings.clear();
        renderGenerations.clear();
        for (const auto token : decisionTokens) {
            EXPECT_TRUE(decisions.Cancel(token));
            EXPECT_TRUE(decisions.ReclaimCancelled(token));
        }
        decisionTokens.clear();
        EXPECT_EQ(0u, occurrences.OccupiedCount());
        EXPECT_EQ(0u, decisions.LiveCount());
        if (root != 0) EXPECT_TRUE(instances.AdvanceLifecycle(root, harken::PlayableLifecycle::Retired));
        root = 0;
        EXPECT_EQ(0u, instances.LiveCount());
    }
};

TEST(VoiceLedger, OriginalMutationFactoryRequiresRealHeldSingleAndRejectsForeignDuplicateOutputs) {
    using Original = harken::VoiceLedger::OriginalCompletionMutationOwner;
    static_assert(!std::is_copy_constructible_v<Original>);
    static_assert(!std::is_copy_assignable_v<Original>);
    static_assert(std::is_nothrow_move_constructible_v<Original>);
    harken::VoiceLedger ledger(2, 2), foreign(2, 2);
    OriginalLedgerNativeFixture fixture;
    harken::VoiceLedger::PreparedCommitInput input{};
    ASSERT_TRUE(fixture.Prepare(ledger, MakeRenderHandle(0, 7), &input));
    harken::VoiceLedger::PreparedCommitProof proof, empty;
    Original original, output;
    EXPECT_FALSE(ledger.IssueOriginalCompletionMutationOwner(empty, &output));
    EXPECT_FALSE(output.Valid());
    ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(input, &proof));
    EXPECT_FALSE(foreign.IssueOriginalCompletionMutationOwner(proof, &output));
    EXPECT_FALSE(output.Valid());
    ASSERT_TRUE(ledger.IssueOriginalCompletionMutationOwner(proof, &original));
    EXPECT_FALSE(ledger.IssueOriginalCompletionMutationOwner(proof, &output));
    EXPECT_FALSE(ledger.IssueOriginalCompletionMutationOwner(proof, &original));
    EXPECT_FALSE(ledger.IssueOriginalCompletionMutationOwner(proof, nullptr));
    EXPECT_FALSE(output.Valid());
    EXPECT_TRUE(original.Valid());
    EXPECT_FALSE(ledger.IsCurrentOriginalCompletionMutationOwner(original));
    EXPECT_TRUE(ledger.IsOriginalCompletionMutationOwnerFor(original, input.identity,
        input.parent, input.renderHandle, input.completionMutation));
    EXPECT_FALSE(foreign.IsOriginalCompletionMutationOwnerFor(original, input.identity,
        input.parent, input.renderHandle, input.completionMutation));
    auto wrong = input;
    ++wrong.identity.generation;
    EXPECT_FALSE(ledger.IsOriginalCompletionMutationOwnerFor(original, wrong.identity,
        input.parent, input.renderHandle, input.completionMutation));
    wrong = input;
    ++wrong.parent.internalToken;
    EXPECT_FALSE(ledger.IsOriginalCompletionMutationOwnerFor(original, input.identity,
        wrong.parent, input.renderHandle, input.completionMutation));
    EXPECT_FALSE(ledger.IsOriginalCompletionMutationOwnerFor(original, input.identity,
        input.parent, MakeRenderHandle(0, 9), input.completionMutation));
    harken::VoiceLedger::PreparedCommitInput unbound{};
    unbound.parent = input.parent;
    unbound.renderHandle = MakeRenderHandle(1, 7);
    std::uint32_t unboundSlot = UINT32_MAX;
    ASSERT_TRUE(ledger.PreparePlayableOwned(unbound.parent, &unboundSlot, &unbound.identity));
    ASSERT_EQ(unboundSlot, unbound.identity.slot);
    harken::VoiceLedger::PreparedCommitProof unboundProof;
    ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(unbound, &unboundProof));
    EXPECT_FALSE(ledger.IssueOriginalCompletionMutationOwner(unboundProof, &output));
    EXPECT_FALSE(output.Valid());
    unboundProof.Release();
    ASSERT_TRUE(ledger.CancelPreparedPlayableOwned(unbound.identity.slot));
    proof.Release();
    ASSERT_TRUE(ledger.CancelPreparedPlayableOwned(input.identity.slot));
    EXPECT_TRUE(original.Valid());
    EXPECT_EQ(1u, ledger.AvailableEntryCount());
    original.Release();
    EXPECT_EQ(2u, ledger.AvailableEntryCount());
    fixture.ReleaseAll();
}

TEST(VoiceLedger, OriginalMutationLoansAllowRealReapAndAllCapacityPathsWaitOnlyForLastReturn) {
    using Original = harken::VoiceLedger::OriginalCompletionMutationOwner;
    harken::VoiceLedger ledger(1);
    OriginalLedgerNativeFixture fixture;
    harken::VoiceLedger::PreparedCommitInput input{};
    ASSERT_TRUE(fixture.Prepare(ledger, MakeRenderHandle(0, 7), &input));
    harken::VoiceLedger::PreparedCommitProof proof;
    ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(input, &proof));
    Original original, loan;
    ASSERT_TRUE(ledger.IssueOriginalCompletionMutationOwner(proof, &original));
    ASSERT_TRUE(original.Borrow(&loan));
    EXPECT_FALSE(original.Borrow(&loan));
    EXPECT_FALSE(original.Borrow(&original));
    EXPECT_FALSE(original.Borrow(nullptr));
    harken::VoiceLedger::ConsumePreparedPlayableCommit(std::move(proof));
    EXPECT_TRUE(ledger.IsCurrentOriginalCompletionMutationOwner(original));
    ledger.Reap(input.renderHandle);
    EXPECT_FALSE(ledger.OwnerForRender(input.renderHandle).live);
    EXPECT_FALSE(ledger.IsCurrentOriginalCompletionMutationOwner(original));
    EXPECT_TRUE(original.Valid());
    EXPECT_TRUE(loan.Valid());
    EXPECT_EQ(input.identity.generation, original.OriginalIdentity().generation);
    EXPECT_TRUE(ledger.IsOriginalCompletionMutationOwnerFor(original, input.identity,
        input.parent, input.renderHandle, input.completionMutation));
    EXPECT_FALSE(ledger.HasFreeEntry());
    EXPECT_EQ(0u, ledger.AvailableEntryCount());
    VnVoiceHandle publicOut = 41;
    std::uint32_t slotOut = 43;
    harken::VoiceLedger::PreparedPlayableIdentity identityOut{47, 53};
    EXPECT_FALSE(ledger.Acquire(&publicOut, &slotOut));
    EXPECT_FALSE(ledger.AcquirePending(&publicOut, &slotOut, 128));
    EXPECT_FALSE(ledger.AcquirePlayableOwned(input.parent, &slotOut));
    EXPECT_FALSE(ledger.PreparePlayableOwned(input.parent, &slotOut, &identityOut));
    EXPECT_EQ(41u, publicOut);
    EXPECT_EQ(43u, slotOut);
    EXPECT_EQ(47u, identityOut.slot);
    EXPECT_EQ(53u, identityOut.generation);
    EXPECT_FALSE(ledger.SetGenerationForTests(input.identity.slot, 19));
    original.Release();
    EXPECT_FALSE(ledger.HasFreeEntry());
    loan.Release();
    EXPECT_TRUE(ledger.HasFreeEntry());
    EXPECT_EQ(1u, ledger.AvailableEntryCount());
    harken::VoiceLedger::PreparedCommitInput next{};
    ASSERT_TRUE(fixture.Prepare(ledger, input.renderHandle, &next));
    EXPECT_EQ(input.identity.slot, next.identity.slot);
    EXPECT_NE(input.identity.generation, next.identity.generation);
    ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(next, &proof));
    Original nextOriginal;
    ASSERT_TRUE(ledger.IssueOriginalCompletionMutationOwner(proof, &nextOriginal));
    EXPECT_FALSE(ledger.IsOriginalCompletionMutationOwnerFor(nextOriginal, input.identity,
        input.parent, input.renderHandle, input.completionMutation));
    proof.Release();
    ASSERT_TRUE(ledger.CancelPreparedPlayableOwned(next.identity.slot));
    nextOriginal.Release();
    EXPECT_EQ(1u, ledger.AvailableEntryCount());
    fixture.ReleaseAll();
}

TEST(VoiceLedger, OriginalMutationPreparedProofReleaseAndEarlyDropsPreserveNativeCommitOrCancel) {
    using Original = harken::VoiceLedger::OriginalCompletionMutationOwner;
    for (const bool commit : {false, true}) {
        SCOPED_TRACE(commit);
        harken::VoiceLedger ledger(1);
        OriginalLedgerNativeFixture fixture;
        harken::VoiceLedger::PreparedCommitInput input{};
        ASSERT_TRUE(fixture.Prepare(ledger, MakeRenderHandle(0, 7), &input));
        harken::VoiceLedger::PreparedCommitProof proof;
        ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(input, &proof));
        Original original;
        ASSERT_TRUE(ledger.IssueOriginalCompletionMutationOwner(proof, &original));
        original.Release();
        EXPECT_FALSE(original.Valid());
        proof.Release();
        EXPECT_EQ(0u, ledger.AvailableEntryCount());
        if (commit) {
            ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(input, &proof));
            Original remint;
            EXPECT_FALSE(ledger.IssueOriginalCompletionMutationOwner(proof, &remint));
            EXPECT_FALSE(remint.Valid());
            harken::VoiceLedger::ConsumePreparedPlayableCommit(std::move(proof));
            EXPECT_TRUE(ledger.OwnerForRender(input.renderHandle).live);
            EXPECT_EQ(0u, ledger.AvailableEntryCount());
            ledger.Reap(input.renderHandle);
        } else ASSERT_TRUE(ledger.CancelPreparedPlayableOwned(input.identity.slot));
        EXPECT_EQ(1u, ledger.AvailableEntryCount());
        fixture.ReleaseAll();
    }
}

TEST(VoiceLedger, OriginalMutationClearAndParkNeverRecreateCurrentFromSameBindingOrNewRender) {
    using Original = harken::VoiceLedger::OriginalCompletionMutationOwner;
    harken::VoiceLedger ledger(1);
    OriginalLedgerNativeFixture fixture;
    harken::VoiceLedger::PreparedCommitInput input{};
    ASSERT_TRUE(fixture.Prepare(ledger, MakeRenderHandle(0, 7), &input));
    harken::VoiceLedger::PreparedCommitProof proof;
    ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(input, &proof));
    Original original;
    ASSERT_TRUE(ledger.IssueOriginalCompletionMutationOwner(proof, &original));
    harken::VoiceLedger::ConsumePreparedPlayableCommit(std::move(proof));
    ASSERT_TRUE(ledger.IsCurrentOriginalCompletionMutationOwner(original));
    ASSERT_TRUE(ledger.ReplaceCompletionMutationExact(input.identity.slot,
        input.completionMutation, input.completionMutation));
    harken::CompletionMutationBinding different{};
    ASSERT_TRUE(fixture.MakeBinding(7, &different));
    EXPECT_FALSE(ledger.ReplaceCompletionMutationExact(input.identity.slot, input.completionMutation, different));
    harken::CompletionMutationBinding observed{};
    ASSERT_TRUE(ledger.CaptureCompletionMutationForRender(input.renderHandle, &observed));
    EXPECT_TRUE(harken::SameCompletionMutationBinding(input.completionMutation, observed));
    ASSERT_TRUE(ledger.ClearCompletionMutationExact(input.identity.slot, input.completionMutation));
    EXPECT_TRUE(original.Valid());
    EXPECT_FALSE(ledger.IsCurrentOriginalCompletionMutationOwner(original));
    Original clearedLoan;
    ASSERT_TRUE(original.Borrow(&clearedLoan));
    EXPECT_TRUE(ledger.IsOriginalCompletionMutationOwnerFor(clearedLoan, input.identity,
        input.parent, input.renderHandle, input.completionMutation));
    EXPECT_FALSE(ledger.BindCompletionMutation(input.identity.slot, different));
    ASSERT_TRUE(ledger.BindCompletionMutation(input.identity.slot, input.completionMutation));
    EXPECT_TRUE(original.Valid());
    EXPECT_TRUE(clearedLoan.Valid());
    EXPECT_FALSE(ledger.IsCurrentOriginalCompletionMutationOwner(original));
    EXPECT_FALSE(ledger.IsCurrentOriginalCompletionMutationOwner(clearedLoan));
    std::uint32_t parked = UINT32_MAX;
    ASSERT_TRUE(ledger.Park(input.renderHandle, &parked));
    EXPECT_EQ(input.identity.slot, parked);
    EXPECT_TRUE(original.Valid());
    EXPECT_TRUE(clearedLoan.Valid());
    EXPECT_FALSE(ledger.IsCurrentOriginalCompletionMutationOwner(clearedLoan));
    const auto returnedRender = MakeRenderHandle(0, 9);
    ledger.ReturnPark(parked, returnedRender);
    EXPECT_TRUE(ledger.OwnerForRender(returnedRender).live);
    EXPECT_FALSE(ledger.IsCurrentOriginalCompletionMutationOwner(original));
    EXPECT_TRUE(ledger.IsOriginalCompletionMutationOwnerFor(original, input.identity,
        input.parent, input.renderHandle, input.completionMutation));
    ASSERT_TRUE(ledger.Park(returnedRender, &parked));
    ledger.EndParked(parked);
    EXPECT_FALSE(ledger.OwnerForRender(returnedRender).live);
    EXPECT_FALSE(ledger.HasFreeEntry());
    EXPECT_TRUE(original.Valid());
    original.Release();
    EXPECT_TRUE(clearedLoan.Valid());
    EXPECT_EQ(0u, ledger.AvailableEntryCount());
    clearedLoan.Release();
    EXPECT_EQ(1u, ledger.AvailableEntryCount());
    fixture.ReleaseAll();
}

TEST(VoiceLedger, OriginalMutationActualReleaseSweepParkedEndAndPreparedCancelKeepForeignEpisode) {
    using Original = harken::VoiceLedger::OriginalCompletionMutationOwner;
    for (unsigned exit = 0; exit < 4; ++exit) {
        SCOPED_TRACE(exit);
        harken::VoiceLedger ledger(1);
        OriginalLedgerNativeFixture fixture;
        harken::VoiceLedger::PreparedCommitInput input{};
        ASSERT_TRUE(fixture.Prepare(ledger, MakeRenderHandle(0, 7), &input));
        harken::VoiceLedger::PreparedCommitProof proof;
        ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(input, &proof));
        Original original, foreignLoan;
        ASSERT_TRUE(ledger.IssueOriginalCompletionMutationOwner(proof, &original));
        ASSERT_TRUE(original.Borrow(&foreignLoan));
        if (exit == 3) {
            proof.Release();
            ASSERT_TRUE(ledger.CancelPreparedPlayableOwned(input.identity.slot));
        } else {
            harken::VoiceLedger::ConsumePreparedPlayableCommit(std::move(proof));
            if (exit == 0) ledger.Release(input.identity.slot);
            else if (exit == 1) ledger.SweepEnded([](VnVoiceHandle) { return false; });
            else {
                std::uint32_t parked = UINT32_MAX;
                ASSERT_TRUE(ledger.Park(input.renderHandle, &parked));
                ledger.EndParked(parked);
            }
        }
        EXPECT_FALSE(ledger.OwnerForRender(input.renderHandle).live);
        EXPECT_FALSE(ledger.IsCurrentOriginalCompletionMutationOwner(original));
        EXPECT_TRUE(ledger.IsOriginalCompletionMutationOwnerFor(foreignLoan, input.identity,
            input.parent, input.renderHandle, input.completionMutation));
        EXPECT_EQ(0u, ledger.AvailableEntryCount());
        original.Release();
        EXPECT_EQ(0u, ledger.AvailableEntryCount());
        foreignLoan.Release();
        EXPECT_EQ(1u, ledger.AvailableEntryCount());
        fixture.ReleaseAll();
    }
}

TEST(VoiceLedger, OriginalMutationMoveOverwriteAndDestructionReturnExactPriorReceivers) {
    using Original = harken::VoiceLedger::OriginalCompletionMutationOwner;
    harken::VoiceLedger first(1), second(1);
    OriginalLedgerNativeFixture fixture;
    harken::VoiceLedger::PreparedCommitInput a{}, b{};
    ASSERT_TRUE(fixture.Prepare(first, MakeRenderHandle(0, 7), &a));
    ASSERT_TRUE(fixture.Prepare(second, MakeRenderHandle(0, 7), &b));
    Original retained;
    {
        harken::VoiceLedger::PreparedCommitProof firstProof, secondProof;
        ASSERT_TRUE(first.ProvePreparedPlayableCommit(a, &firstProof));
        ASSERT_TRUE(second.ProvePreparedPlayableCommit(b, &secondProof));
        Original firstOriginal, secondOriginal;
        ASSERT_TRUE(first.IssueOriginalCompletionMutationOwner(firstProof, &firstOriginal));
        ASSERT_TRUE(second.IssueOriginalCompletionMutationOwner(secondProof, &secondOriginal));
        ASSERT_TRUE(firstOriginal.Borrow(&retained));
        harken::VoiceLedger::ConsumePreparedPlayableCommit(std::move(firstProof));
        harken::VoiceLedger::ConsumePreparedPlayableCommit(std::move(secondProof));
        first.Reap(a.renderHandle);
        second.Reap(b.renderHandle);
        firstOriginal = std::move(secondOriginal);
        EXPECT_FALSE(secondOriginal.Valid());
        EXPECT_TRUE(second.IsOriginalCompletionMutationOwnerFor(firstOriginal, b.identity,
            b.parent, b.renderHandle, b.completionMutation));
        EXPECT_FALSE(first.IsOriginalCompletionMutationOwnerFor(firstOriginal, a.identity,
            a.parent, a.renderHandle, a.completionMutation));
        Original moved(std::move(firstOriginal));
        EXPECT_FALSE(firstOriginal.Valid());
        EXPECT_TRUE(moved.Valid());
        EXPECT_EQ(0u, first.AvailableEntryCount());
    }
    EXPECT_EQ(1u, second.AvailableEntryCount());
    EXPECT_EQ(0u, first.AvailableEntryCount());
    retained.Release();
    EXPECT_EQ(1u, first.AvailableEntryCount());
    fixture.ReleaseAll();
}

TEST(VoiceLedger, OriginalMutationForeignRecordDoesNotSuppressRealDirectAndPendingPublicEnds) {
    using Original = harken::VoiceLedger::OriginalCompletionMutationOwner;
    harken::VoiceLedger ledger(2, 2);
    OriginalLedgerNativeFixture fixture;
    harken::VoiceLedger::PreparedCommitInput input{};
    ASSERT_TRUE(fixture.Prepare(ledger, MakeRenderHandle(0, 7), &input));
    harken::VoiceLedger::PreparedCommitProof proof;
    ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(input, &proof));
    Original original;
    ASSERT_TRUE(ledger.IssueOriginalCompletionMutationOwner(proof, &original));
    harken::VoiceLedger::ConsumePreparedPlayableCommit(std::move(proof));
    ledger.Reap(input.renderHandle);
    EXPECT_EQ(1u, ledger.AvailableEntryCount());
    VnVoiceHandle direct = 0;
    std::uint32_t directSlot = UINT32_MAX;
    ASSERT_TRUE(ledger.Acquire(&direct, &directSlot));
    EXPECT_NE(input.identity.slot, directSlot);
    const auto directRender = MakeRenderHandle(1, 9);
    ledger.BindRender(directSlot, directRender, 128);
    EXPECT_EQ(direct, ledger.ToPublic(directRender));
    ledger.SweepEnded([](VnVoiceHandle) { return false; });
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Stale, ledger.Classify(direct).kind);
    EXPECT_EQ(0u, ledger.ToRender(direct));
    EXPECT_EQ(0u, ledger.ToPublic(directRender));
    EXPECT_EQ(1u, ledger.AvailableEntryCount());
    VnVoiceHandle pending = 0;
    ASSERT_TRUE(ledger.AcquirePending(&pending, &directSlot, 128));
    EXPECT_NE(input.identity.slot, directSlot);
    ledger.CancelPending(directSlot);
    EXPECT_EQ(harken::VoiceLedger::HandleKind::Stale, ledger.Classify(pending).kind);
    EXPECT_EQ(0u, ledger.PublicHandleForSlot(directSlot));
    EXPECT_EQ(1u, ledger.AvailableEntryCount());
    EXPECT_TRUE(original.Valid());
    EXPECT_FALSE(ledger.IsCurrentOriginalCompletionMutationOwner(original));
    original.Release();
    EXPECT_EQ(2u, ledger.AvailableEntryCount());
    fixture.ReleaseAll();
}

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
TEST(VoiceLedger, OriginalMutationNativeCountMarkerAndGenerationLimitsAllowTrueOldEnd) {
    using Original = harken::VoiceLedger::OriginalCompletionMutationOwner;
    using Access = harken::detail::VoiceLedgerOriginalCompletionTestAccess;
    OriginalLedgerNativeFixture fixture;
    harken::VoiceLedger ledger(1);
    harken::VoiceLedger::PreparedCommitInput input{};
    ASSERT_TRUE(fixture.Prepare(ledger, MakeRenderHandle(0, 7), &input));
    harken::VoiceLedger::PreparedCommitProof proof;
    ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(input, &proof));
    Original original, output;
    ASSERT_TRUE(ledger.IssueOriginalCompletionMutationOwner(proof, &original));
    Access::SetCount(ledger, input.identity.slot, UINT32_MAX);
    EXPECT_FALSE(original.Borrow(&output));
    EXPECT_FALSE(output.Valid());
    harken::VoiceLedger::ConsumePreparedPlayableCommit(std::move(proof));
    ledger.Reap(input.renderHandle);
    EXPECT_TRUE(original.Valid());
    EXPECT_FALSE(ledger.IsCurrentOriginalCompletionMutationOwner(original));
    EXPECT_EQ(0u, ledger.AvailableEntryCount());
    Access::SetCount(ledger, input.identity.slot, 1u);
    original.Release();
    EXPECT_EQ(1u, ledger.AvailableEntryCount());
    harken::VoiceLedger marker(1);
    harken::VoiceLedger::PreparedCommitInput markerInput{};
    ASSERT_TRUE(fixture.Prepare(marker, MakeRenderHandle(0, 7), &markerInput));
    ASSERT_TRUE(marker.ProvePreparedPlayableCommit(markerInput, &proof));
    Access::SetMarkerMax(marker);
    EXPECT_FALSE(marker.IssueOriginalCompletionMutationOwner(proof, &output));
    EXPECT_FALSE(output.Valid());
    EXPECT_TRUE(proof.Valid());
    harken::VoiceLedger::ConsumePreparedPlayableCommit(std::move(proof));
    marker.Reap(markerInput.renderHandle);
    EXPECT_EQ(1u, marker.AvailableEntryCount());
    harken::VoiceLedger maximum(1);
    ASSERT_TRUE(maximum.SetGenerationForTests(0, harken::VoiceLedger::kMaxGeneration));
    harken::VoiceLedger::PreparedCommitInput maximumInput{};
    ASSERT_TRUE(fixture.Prepare(maximum, MakeRenderHandle(0, 7), &maximumInput));
    ASSERT_TRUE(maximum.ProvePreparedPlayableCommit(maximumInput, &proof));
    ASSERT_TRUE(maximum.IssueOriginalCompletionMutationOwner(proof, &original));
    harken::VoiceLedger::ConsumePreparedPlayableCommit(std::move(proof));
    maximum.Reap(maximumInput.renderHandle);
    EXPECT_EQ(1u, maximum.QuarantinedCount());
    EXPECT_TRUE(original.Valid());
    EXPECT_TRUE(maximum.IsOriginalCompletionMutationOwnerFor(original, maximumInput.identity,
        maximumInput.parent, maximumInput.renderHandle, maximumInput.completionMutation));
    EXPECT_FALSE(maximum.IsCurrentOriginalCompletionMutationOwner(original));
    original.Release();
    EXPECT_EQ(0u, maximum.AvailableEntryCount());
    EXPECT_FALSE(maximum.HasFreeEntry());
    fixture.ReleaseAll();
}
#endif

} // namespace

// These Ledger-only cases use actual native parent, Composite and Occurrence
// factories. A render key is a reverse-row preparation input, not a claim of
// physical PCM, Source/Current authority, C0, or complete Queue association.
struct OriginalCompositeLedgerNativeFixture {
    struct NativeBinding {
        harken::CompositeCompletionOwnerToken composite{};
        harken::OccurrenceToken aggregate{};
        std::array<harken::OccurrenceToken, 2> leaves{};
        std::uint32_t renderGeneration = 0;
    };
    harken::PlayableInstanceTable instances{1};
    harken::OccurrenceStatePool occurrences{24};
    harken::CompositeCompletionOwnerPool composites{8, 2};
    harken::PlayableInstanceHandle root = 0;
    std::vector<NativeBinding> originals;

    bool Prepare(harken::VoiceLedger& ledger, VnVoiceHandle render,
        harken::VoiceLedger::PreparedCommitInput* out, bool preparedCompositeBind = false) {
        if (out == nullptr || render == 0 || static_cast<std::uint32_t>(render >> 32) == 0) return false;
        if (root == 0) root = instances.Acquire(harken::PlayableKind::Container, 0);
        if (root == 0) return false;
        NativeBinding native{};
        native.renderGeneration = static_cast<std::uint32_t>(render >> 32);
        if (!occurrences.Acquire({root, harkentest::ObjectId(83), 0, 0}, &native.aggregate)) return false;
        std::array<harken::KnownScheduleLeafV1, 2> known{};
        for (std::uint32_t i = 0; i < known.size(); ++i) {
            if (!occurrences.Acquire({root, harkentest::ObjectId(89 + i), 0, i}, &native.leaves[i]) ||
                !occurrences.BindCompletionProgress(native.leaves[i], native.renderGeneration, 1)) return false;
            auto& leaf = known[i];
            leaf.stableLeafOrdinal = i + 1u;
            leaf.authoredOffset.domain = harken::AuthoredTimeDomainV1::SourceFrames;
            leaf.authoredOffset.numerator = i * 10u;
            leaf.authoredOffset.denominator = 1;
            leaf.occurrence = native.leaves[i];
            leaf.renderGeneration = native.renderGeneration;
            leaf.absoluteLeafStartFrame = 100u + i * 10u;
            auto& predicate = leaf.predicate;
            predicate.occurrence = leaf.occurrence;
            predicate.renderGeneration = leaf.renderGeneration;
            predicate.absoluteLogicalStartFrame = leaf.absoluteLeafStartFrame;
            predicate.absoluteLogicalCompleteFrame = leaf.absoluteLeafStartFrame + 32u;
            predicate.totalLogicalOutputFrames = 32;
            predicate.playbackFactsRevision = 1;
            predicate.program.kind = harken::PlayableKindV1::Sound;
            predicate.program.stableId = harkentest::ObjectId(97 + i);
            predicate.program.world = 7;
            predicate.program.runtimeSemanticFingerprint = 11;
        }
        if (composites.PrepareAcquire(native.aggregate, 1, known.data(), known.size(),
                occurrences, &native.composite) != harken::CompositeCompletionAcquireStatus::Ok ||
            composites.CommitPrepared(native.composite, occurrences) != harken::CompositeCompletionAcquireStatus::Ok)
            return false;
        harken::CompletionOwnerRef actual{};
        if (!occurrences.CaptureCompletionOwner(native.leaves[0], &actual) ||
            actual.kind != harken::CompletionOwnerKind::Composite) return false;
        originals.push_back(native);
        out->parent = {0, root};
        out->renderHandle = render;
        const harken::CompletionMutationBinding binding{native.leaves[0], actual};
        std::uint32_t slot = UINT32_MAX;
        if (!ledger.PreparePlayableOwned(out->parent, &slot, &out->identity) || slot != out->identity.slot) return false;
        if (preparedCompositeBind) return ledger.BindPreparedCompositeCompletionMutation(out, binding);
        out->completionMutation = binding;
        return ledger.BindCompletionMutation(slot, binding);
    }

    void ReleaseAll() {
        for (const auto& native : originals) {
            EXPECT_TRUE(composites.RollbackBeforePublication(native.composite, occurrences));
            for (const auto leaf : native.leaves) {
                EXPECT_TRUE(occurrences.UnbindCompletionProgress(leaf, native.renderGeneration));
                EXPECT_TRUE(occurrences.Advance(leaf, harken::OccurrenceStage::AwaitingEventFence));
                EXPECT_TRUE(occurrences.ReleaseAfterFence(leaf));
            }
            EXPECT_TRUE(occurrences.Advance(native.aggregate, harken::OccurrenceStage::AwaitingEventFence));
            EXPECT_TRUE(occurrences.ReleaseAfterFence(native.aggregate));
        }
        originals.clear();
        EXPECT_EQ(0u, occurrences.OccupiedCount());
        EXPECT_EQ(0u, composites.OccupiedCount());
        if (root != 0) EXPECT_TRUE(instances.AdvanceLifecycle(root, harken::PlayableLifecycle::Retired));
        root = 0;
        EXPECT_EQ(0u, instances.LiveCount());
    }
};

TEST(VoiceLedger, OriginalCompositeMutationRequiresActualHeldFactoryAndCannotIssueSingleOutput) {
    using Original = harken::VoiceLedger::OriginalCompositeCompletionMutationOwner;
    static_assert(!std::is_copy_constructible_v<Original> && !std::is_copy_assignable_v<Original>);
    static_assert(std::is_nothrow_move_constructible_v<Original>);
    static_assert(!std::is_convertible_v<Original, harken::VoiceLedger::OriginalCompletionMutationOwner>);
    harken::VoiceLedger ledger(2, 2), foreignReceiver(2, 2);
    OriginalCompositeLedgerNativeFixture fixture;
    harken::VoiceLedger::PreparedCommitInput input{};
    ASSERT_TRUE(fixture.Prepare(ledger, MakeRenderHandle(0, 7), &input));
    harken::VoiceLedger::PreparedCommitProof proof, empty;
    Original original, refused;
    EXPECT_FALSE(ledger.IssueOriginalCompositeCompletionMutationOwner(empty, &refused));
    EXPECT_FALSE(refused.Valid());
    ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(input, &proof));
    EXPECT_FALSE(foreignReceiver.IssueOriginalCompositeCompletionMutationOwner(proof, &refused));
    harken::VoiceLedger::OriginalCompletionMutationOwner refusedSingle;
    EXPECT_FALSE(ledger.IssueOriginalCompletionMutationOwner(proof, &refusedSingle));
    EXPECT_FALSE(refusedSingle.Valid());
    ASSERT_TRUE(ledger.IssueOriginalCompositeCompletionMutationOwner(proof, &original));
    EXPECT_FALSE(ledger.IssueOriginalCompositeCompletionMutationOwner(proof, &refused));
    EXPECT_FALSE(ledger.IssueOriginalCompositeCompletionMutationOwner(proof, &original));
    EXPECT_FALSE(ledger.IssueOriginalCompositeCompletionMutationOwner(proof, nullptr));
    EXPECT_FALSE(refused.Valid());
    EXPECT_FALSE(ledger.IsCurrentOriginalCompositeCompletionMutationOwner(original));
    EXPECT_TRUE(ledger.IsOriginalCompositeCompletionMutationOwnerFor(original, input.identity,
        input.parent, input.renderHandle, input.completionMutation));
    EXPECT_FALSE(foreignReceiver.IsOriginalCompositeCompletionMutationOwnerFor(original, input.identity,
        input.parent, input.renderHandle, input.completionMutation));
    auto wrong = input.identity;
    ++wrong.generation;
    EXPECT_FALSE(ledger.IsOriginalCompositeCompletionMutationOwnerFor(original, wrong,
        input.parent, input.renderHandle, input.completionMutation));
    harken::VoiceLedger::PreparedSingleCompletionCommitProof refusedSingleProof;
    EXPECT_FALSE(ledger.ProvePreparedSingleCompletionCommit(input, &refusedSingleProof));
    EXPECT_FALSE(refusedSingleProof.Valid());
    harken::VoiceLedger::ConsumePreparedPlayableCommit(std::move(proof));
    EXPECT_TRUE(ledger.IsCurrentOriginalCompositeCompletionMutationOwner(original));
    ledger.Reap(input.renderHandle);
    EXPECT_FALSE(ledger.IsCurrentOriginalCompositeCompletionMutationOwner(original));
    EXPECT_TRUE(original.Valid());
    original.Release();
    EXPECT_EQ(2u, ledger.AvailableEntryCount());
    fixture.ReleaseAll();
}

TEST(VoiceLedger, OriginalCompositeMutationPreservesRealNativeReapAndLastReturnHigherGenerationReuse) {
    using Original = harken::VoiceLedger::OriginalCompositeCompletionMutationOwner;
    harken::VoiceLedger ledger(1);
    OriginalCompositeLedgerNativeFixture fixture;
    harken::VoiceLedger::PreparedCommitInput input{};
    ASSERT_TRUE(fixture.Prepare(ledger, MakeRenderHandle(0, 7), &input));
    harken::VoiceLedger::PreparedCommitProof proof;
    ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(input, &proof));
    Original original, loan, last;
    ASSERT_TRUE(ledger.IssueOriginalCompositeCompletionMutationOwner(proof, &original));
    ASSERT_TRUE(original.Borrow(&loan));
    EXPECT_FALSE(original.Borrow(&loan));
    EXPECT_FALSE(original.Borrow(&original));
    EXPECT_FALSE(original.Borrow(nullptr));
    harken::VoiceLedger::ConsumePreparedPlayableCommit(std::move(proof));
    ASSERT_TRUE(ledger.IsCurrentOriginalCompositeCompletionMutationOwner(original));
    ledger.Reap(input.renderHandle);
    EXPECT_FALSE(ledger.OwnerForRender(input.renderHandle).live);
    EXPECT_FALSE(ledger.IsCurrentOriginalCompositeCompletionMutationOwner(loan));
    EXPECT_TRUE(ledger.IsOriginalCompositeCompletionMutationOwnerFor(loan, input.identity,
        input.parent, input.renderHandle, input.completionMutation));
    EXPECT_EQ(0u, ledger.AvailableEntryCount());
    VnVoiceHandle untouched = 41;
    std::uint32_t slot = 43;
    EXPECT_FALSE(ledger.Acquire(&untouched, &slot));
    EXPECT_EQ(41u, untouched);
    EXPECT_EQ(43u, slot);
    ASSERT_TRUE(loan.Borrow(&last));
    original.Release();
    loan.Release();
    EXPECT_EQ(0u, ledger.AvailableEntryCount());
    last.Release();
    EXPECT_EQ(1u, ledger.AvailableEntryCount());
    harken::VoiceLedger::PreparedCommitInput next{};
    ASSERT_TRUE(fixture.Prepare(ledger, input.renderHandle, &next));
    EXPECT_EQ(input.identity.slot, next.identity.slot);
    EXPECT_GT(next.identity.generation, input.identity.generation);
    ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(next, &proof));
    Original nextOriginal;
    ASSERT_TRUE(ledger.IssueOriginalCompositeCompletionMutationOwner(proof, &nextOriginal));
    EXPECT_FALSE(ledger.IsOriginalCompositeCompletionMutationOwnerFor(nextOriginal, input.identity,
        input.parent, input.renderHandle, input.completionMutation));
    proof.Release();
    ASSERT_TRUE(ledger.CancelPreparedPlayableOwned(next.identity.slot));
    EXPECT_TRUE(nextOriginal.Valid());
    nextOriginal.Release();
    EXPECT_EQ(1u, ledger.AvailableEntryCount());
    fixture.ReleaseAll();
}

TEST(VoiceLedger, OriginalCompositeMutationMovesPreserveActualPreparedCommitAndCancellation) {
    using Original = harken::VoiceLedger::OriginalCompositeCompletionMutationOwner;
    for (const bool commit : {false, true}) {
        SCOPED_TRACE(commit);
        harken::VoiceLedger ledger(1);
        OriginalCompositeLedgerNativeFixture fixture;
        harken::VoiceLedger::PreparedCommitInput input{};
        ASSERT_TRUE(fixture.Prepare(ledger, MakeRenderHandle(0, 7), &input));
        harken::VoiceLedger::PreparedCommitProof proof;
        ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(input, &proof));
        Original original, borrowed;
        ASSERT_TRUE(ledger.IssueOriginalCompositeCompletionMutationOwner(proof, &original));
        ASSERT_TRUE(original.Borrow(&borrowed));
        Original moved(std::move(original));
        EXPECT_FALSE(original.Valid());
        original = std::move(borrowed);
        EXPECT_FALSE(borrowed.Valid());
        moved.Release();
        EXPECT_TRUE(original.Valid());
        if (commit) {
            harken::VoiceLedger::ConsumePreparedPlayableCommit(std::move(proof));
            EXPECT_TRUE(ledger.IsCurrentOriginalCompositeCompletionMutationOwner(original));
            ASSERT_TRUE(ledger.ClearCompletionMutationExact(input.identity.slot, input.completionMutation));
            EXPECT_FALSE(ledger.IsCurrentOriginalCompositeCompletionMutationOwner(original));
            ledger.Reap(input.renderHandle);
        } else {
            proof.Release();
            ASSERT_TRUE(ledger.CancelPreparedPlayableOwned(input.identity.slot));
        }
        EXPECT_TRUE(ledger.IsOriginalCompositeCompletionMutationOwnerFor(original, input.identity,
            input.parent, input.renderHandle, input.completionMutation));
        EXPECT_EQ(0u, ledger.AvailableEntryCount());
        original.Release();
        EXPECT_EQ(1u, ledger.AvailableEntryCount());
        fixture.ReleaseAll();
    }
}

#if defined(HARKEN_TEST_TIMELINE_CAPACITY_OVERRIDE)
TEST(VoiceLedger, OriginalCompositeMutationMarkerAndBorrowExhaustionPreserveHeldNativeAndOutputs) {
    using Original = harken::VoiceLedger::OriginalCompositeCompletionMutationOwner;
    using Access = harken::detail::VoiceLedgerOriginalCompletionTestAccess;
    harken::VoiceLedger ledger(1), exhausted(1);
    OriginalCompositeLedgerNativeFixture fixture;
    harken::VoiceLedger::PreparedCommitInput input{};
    ASSERT_TRUE(fixture.Prepare(ledger, MakeRenderHandle(0, 7), &input));
    harken::VoiceLedger::PreparedCommitProof proof;
    ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(input, &proof));
    Original original, refused;
    ASSERT_TRUE(ledger.IssueOriginalCompositeCompletionMutationOwner(proof, &original));
    Access::SetCount(ledger, input.identity.slot, UINT32_MAX);
    EXPECT_FALSE(original.Borrow(&refused));
    EXPECT_FALSE(refused.Valid());
    Access::SetCount(ledger, input.identity.slot, 1);
    harken::VoiceLedger::ConsumePreparedPlayableCommit(std::move(proof));
    EXPECT_TRUE(ledger.IsCurrentOriginalCompositeCompletionMutationOwner(original));
    ledger.Reap(input.renderHandle);
    original.Release();
    EXPECT_EQ(1u, ledger.AvailableEntryCount());
    harken::VoiceLedger::PreparedCommitInput blocked{};
    ASSERT_TRUE(fixture.Prepare(exhausted, MakeRenderHandle(0, 7), &blocked));
    ASSERT_TRUE(exhausted.ProvePreparedPlayableCommit(blocked, &proof));
    Access::SetMarkerMax(exhausted);
    EXPECT_FALSE(exhausted.IssueOriginalCompositeCompletionMutationOwner(proof, &refused));
    EXPECT_FALSE(refused.Valid());
    EXPECT_TRUE(proof.Valid());
    harken::VoiceLedger::ConsumePreparedPlayableCommit(std::move(proof));
    EXPECT_TRUE(exhausted.OwnerForRender(blocked.renderHandle).live);
    exhausted.Reap(blocked.renderHandle);
    EXPECT_EQ(1u, exhausted.AvailableEntryCount());
    fixture.ReleaseAll();
}
#endif

TEST(VoiceLedger, PreparedCompositeFactoryInputKeepsGenuineHeldCommitAndNativeEndSeparate) {
    for (const bool commit : {false, true}) {
        OriginalCompositeLedgerNativeFixture fixture;
        harken::VoiceLedger ledger(1, 1), foreign(1, 1);
        harken::VoiceLedger::PreparedCommitInput input{};
        ASSERT_TRUE(fixture.Prepare(ledger, MakeRenderHandle(0, 31), &input, true));
        harken::VoiceLedger::PreparedSingleCompletionCommitProof single;
        EXPECT_FALSE(ledger.ProvePreparedSingleCompletionCommit(input, &single));
        harken::VoiceLedger::PreparedCommitProof proof;
        ASSERT_TRUE(ledger.ProvePreparedPlayableCommit(input, &proof));
        ASSERT_TRUE(ledger.MatchesPreparedPlayableCommit(proof, input));
        EXPECT_FALSE(foreign.MatchesPreparedPlayableCommit(proof, input));
        auto wrong = input;
        ++wrong.identity.generation;
        EXPECT_FALSE(ledger.MatchesPreparedPlayableCommit(proof, wrong));
        wrong = input;
        ++wrong.priority;
        EXPECT_FALSE(ledger.MatchesPreparedPlayableCommit(proof, wrong));
        wrong = input;
        wrong.completionMutation = {};
        EXPECT_FALSE(ledger.MatchesPreparedPlayableCommit(proof, wrong));
        harken::VoiceLedger::OriginalCompositeCompletionMutationOwner original, loan;
        ASSERT_TRUE(ledger.IssueOriginalCompositeCompletionMutationOwner(proof, &original));
        ASSERT_TRUE(original.Borrow(&loan));
        EXPECT_FALSE(ledger.HasOriginalCompositeCompletionNativeEnd(original));
        EXPECT_FALSE(foreign.HasOriginalCompositeCompletionNativeEnd(original));
        if (commit) {
            harken::VoiceLedger::ConsumePreparedPlayableCommit(std::move(proof));
            EXPECT_TRUE(ledger.IsCurrentOriginalCompositeCompletionMutationOwner(original));
            ASSERT_TRUE(ledger.ClearCompletionMutationExact(input.identity.slot, input.completionMutation));
            EXPECT_FALSE(ledger.IsCurrentOriginalCompositeCompletionMutationOwner(original));
            EXPECT_FALSE(ledger.HasOriginalCompositeCompletionNativeEnd(original));
            ledger.Reap(input.renderHandle);
        } else {
            proof.Release();
            EXPECT_FALSE(ledger.MatchesPreparedPlayableCommit(proof, input));
            EXPECT_FALSE(ledger.HasOriginalCompositeCompletionNativeEnd(original));
            ASSERT_TRUE(ledger.ClearCompletionMutationExact(input.identity.slot, input.completionMutation));
            EXPECT_FALSE(ledger.HasOriginalCompositeCompletionNativeEnd(original));
            ASSERT_TRUE(ledger.CancelPreparedPlayableOwned(input.identity.slot));
        }
        ASSERT_TRUE(ledger.HasOriginalCompositeCompletionNativeEnd(original));
        ASSERT_TRUE(ledger.HasOriginalCompositeCompletionNativeEnd(loan));
        EXPECT_EQ(0u, ledger.AvailableEntryCount());
        original.Release();
        EXPECT_EQ(0u, ledger.AvailableEntryCount());
        ASSERT_TRUE(ledger.IsOriginalCompositeCompletionMutationOwnerFor(loan, input.identity,
            input.parent, input.renderHandle, input.completionMutation));
        loan.Release();
        EXPECT_EQ(1u, ledger.AvailableEntryCount());
        fixture.ReleaseAll();
    }
}
