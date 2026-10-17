#include "Common/ObjectIdTestSupport.h"
#include "harken/core/ActionFiringTable.h"

#include <gtest/gtest.h>
#include <type_traits>

namespace {

harken::PackPartitionLease PackAnchor() {
    return {UINT64_C(0x0000000d00000003), 90, 12, 34, 56,
            78, 91, 92, 13, 17};
}

} // namespace

TEST(ActionFiringTable, PreparedRootStaysHiddenUntilCommittedAndReleased) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(1));

    harken::PreparedActionFiring prepared{};
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(41), 7, 1, &prepared));
    ASSERT_TRUE(table.SetPreparedProgramFingerprint(prepared, 101));
    ASSERT_TRUE(prepared.Valid());
    EXPECT_EQ(0u, table.LiveCount());

    harken::ActionFiringSnapshot snapshot{};
    EXPECT_FALSE(table.Capture(prepared.Handle(), &snapshot));

    harken::ActionFiringStepResult result{};
    result.authoredEventIndex = 0;
    result.executionOrdinal = 0;
    ASSERT_TRUE(table.SetPreparedStep(prepared, 0, result));
    ASSERT_TRUE(table.Commit(&prepared, 99));

    ASSERT_TRUE(table.Capture(prepared.Handle(), &snapshot));
    EXPECT_EQ(harken::ActionFiringState::Active, snapshot.state);
    EXPECT_EQ(harkentest::ObjectId(41u), snapshot.actionId);
    EXPECT_EQ(101u, snapshot.programSemanticFingerprint);
    EXPECT_EQ(7u, snapshot.worldGeneration);
    EXPECT_EQ(99u, snapshot.acceptedLogicSequence);
    EXPECT_EQ(1u, table.LiveCount());
    EXPECT_EQ(1u, table.HighWater());

    ASSERT_TRUE(table.MarkTerminal(prepared.Handle()));
    ASSERT_TRUE(table.Capture(prepared.Handle(), &snapshot));
    EXPECT_EQ(harken::ActionFiringState::TerminalRetained, snapshot.state);
    EXPECT_EQ(101u, snapshot.programSemanticFingerprint);
    EXPECT_EQ(1u, table.LiveCount());

    harken::ActionFiringStepResult retained{};
    ASSERT_TRUE(table.CaptureStep(prepared.Handle(), 0, &retained));
    EXPECT_EQ(0u, retained.authoredEventIndex);
    EXPECT_EQ(0u, retained.executionOrdinal);

    ASSERT_TRUE(table.ReleaseRoot(prepared.Handle()));
    EXPECT_FALSE(table.Capture(prepared.Handle(), &snapshot));
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(1u, table.FreeCount());
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(41), 8, 0, &prepared));
    ASSERT_TRUE(table.Commit(&prepared, 100));
    ASSERT_TRUE(table.Capture(prepared.Handle(), &snapshot));
    EXPECT_EQ(0u, snapshot.programSemanticFingerprint);
}

TEST(ActionFiringTable, ExactCapacityAndOneMoreRefusalHaveNoVisibleEffect) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(2));

    harken::PreparedActionFiring first{};
    harken::PreparedActionFiring second{};
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(1), 1, 1, &first));
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(2), 1, 1, &second));
    EXPECT_EQ(0u, table.FreeCount());
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(0u, table.HighWater());

    harken::PreparedActionFiring refused{};
    EXPECT_FALSE(table.PrepareRoot(harkentest::ObjectId(3), 1, 1, &refused));
    EXPECT_FALSE(refused.Valid());
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(0u, table.HighWater());

    ASSERT_TRUE(table.Abandon(&second));
    EXPECT_EQ(1u, table.FreeCount());
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(0u, table.HighWater());

    harken::PreparedActionFiring replacement{};
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(4), 1, 1, &replacement));
    EXPECT_NE(second.Handle(), replacement.Handle());
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(0u, table.HighWater());
}

TEST(ActionFiringTable, InheritedOrderRequiresActualPreparedChildAndLiveUncancelledParent) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(3));
    harken::PreparedActionFiring root, child, unrelated;
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(1), 7, 1, &root));
    EXPECT_EQ(0u, root.InheritedSequence());
    ASSERT_TRUE(table.Commit(&root, 99));
    ASSERT_TRUE(table.PrepareChild(root.Handle(), harkentest::ObjectId(2),
        harken::ActionFiringOwnerStamp::DirectWorld(7), 1, &child));
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(3), 7, 1, &unrelated));
    EXPECT_EQ(99u, child.InheritedSequence());
    EXPECT_EQ(0u, unrelated.InheritedSequence());
    const auto retained = child;
    ASSERT_TRUE(table.BumpCancellationEpoch(root.Handle()));
    EXPECT_EQ(0u, child.InheritedSequence());
    EXPECT_EQ(0u, retained.InheritedSequence());
    ASSERT_TRUE(table.Abandon(&child));
    EXPECT_EQ(0u, child.InheritedSequence());
    EXPECT_EQ(0u, retained.InheritedSequence());
    ASSERT_TRUE(table.PrepareChild(root.Handle(), harkentest::ObjectId(4),
        harken::ActionFiringOwnerStamp::DirectWorld(7), 1, &child));
    EXPECT_EQ(99u, child.InheritedSequence());
    EXPECT_EQ(0u, retained.InheritedSequence());
    ASSERT_TRUE(table.CommitPreparedChild(&child));
    EXPECT_EQ(0u, child.InheritedSequence());
    ASSERT_TRUE(table.Abandon(&unrelated));
}

TEST(ActionFiringTable, PreparedChildCannotCommitAfterItsOriginalParentIsCancelled) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(2));
    harken::PreparedActionFiring root, child;
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(1), 7, 1, &root));
    ASSERT_TRUE(table.Commit(&root, 99));
    ASSERT_TRUE(table.PrepareChild(root.Handle(), harkentest::ObjectId(2),
        harken::ActionFiringOwnerStamp::DirectWorld(7), 1, &child));
    ASSERT_TRUE(table.BumpCancellationEpoch(root.Handle()));
    EXPECT_FALSE(table.CommitPreparedChild(&child));
    EXPECT_TRUE(child.Valid());
    EXPECT_EQ(1u, table.LiveCount());
    ASSERT_TRUE(table.Abandon(&child));
}

TEST(ActionFiringTable, ChildActivationReceiptMatchesActualPreparedIdentityAndTransfersByMove) {
    static_assert(!std::is_copy_constructible_v<harken::ActionFiringTable::ChildActivationReceipt>);
    static_assert(!std::is_copy_assignable_v<harken::ActionFiringTable::ChildActivationReceipt>);
    harken::ActionFiringTable table, foreign;
    ASSERT_TRUE(table.Reset(2));
    ASSERT_TRUE(foreign.Reset(2));
    harken::PreparedActionFiring root, child;
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(1), 7, 1, &root));
    harken::ActionFiringTable::PreparedChildIdentity identity;
    EXPECT_FALSE(table.CapturePreparedChildIdentity(root, 1, &identity));
    ASSERT_TRUE(table.Commit(&root, 99));
    ASSERT_TRUE(table.PrepareChild(root.Handle(), harkentest::ObjectId(2),
        harken::ActionFiringOwnerStamp::DirectWorld(7), 1, &child));
    EXPECT_FALSE(table.CapturePreparedChildIdentity(child, 0, &identity));
    EXPECT_FALSE(table.CapturePreparedChildIdentity(child, 2, &identity));
    EXPECT_FALSE(foreign.CapturePreparedChildIdentity(child, 1, &identity));
    ASSERT_TRUE(table.CapturePreparedChildIdentity(child, 1, &identity));
    EXPECT_EQ(child.Handle(), identity.Child());
    EXPECT_EQ(root.Handle(), identity.Parent());
    EXPECT_EQ(99u, identity.Sequence());
    EXPECT_TRUE(table.IsPreparedChildActive(identity));
    EXPECT_FALSE(foreign.IsPreparedChildActive(identity));
    harken::ActionFiringTable::ChildActivationReceipt receipt;
    EXPECT_FALSE(table.IsChildActivationCurrent(receipt));
    EXPECT_FALSE(receipt.MatchesOriginalPreparation(identity));
    ASSERT_TRUE(table.CommitPreparedChild(&child, &receipt));
    EXPECT_FALSE(table.IsPreparedChildActive(identity));
    EXPECT_TRUE(receipt.MatchesOriginalPreparation(identity));
    EXPECT_TRUE(table.IsChildActivationCurrent(receipt));
    auto moved = std::move(receipt);
    EXPECT_EQ(0u, receipt.Sequence());
    EXPECT_FALSE(receipt.MatchesOriginalPreparation(identity));
    EXPECT_FALSE(table.IsChildActivationCurrent(receipt));
    EXPECT_EQ(99u, moved.Sequence());
    EXPECT_TRUE(table.IsChildActivationCurrent(moved));
    EXPECT_FALSE(foreign.IsChildActivationCurrent(moved));
    ASSERT_TRUE(table.BumpCancellationEpoch(root.Handle()));
    EXPECT_FALSE(table.IsChildActivationCurrent(moved));
}

TEST(ActionFiringTable, SameSequenceSiblingAndOccupiedReceiptCannotAuthorizeAnotherChild) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(3));
    harken::PreparedActionFiring root, first, second;
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(1), 7, 1, &root));
    ASSERT_TRUE(table.Commit(&root, 99));
    ASSERT_TRUE(table.PrepareChild(root.Handle(), harkentest::ObjectId(2),
        harken::ActionFiringOwnerStamp::DirectWorld(7), 1, &first));
    ASSERT_TRUE(table.PrepareChild(root.Handle(), harkentest::ObjectId(3),
        harken::ActionFiringOwnerStamp::DirectWorld(7), 1, &second));
    harken::ActionFiringTable::PreparedChildIdentity firstIdentity, secondIdentity;
    ASSERT_TRUE(table.CapturePreparedChildIdentity(first, 1, &firstIdentity));
    ASSERT_TRUE(table.CapturePreparedChildIdentity(second, 1, &secondIdentity));
    EXPECT_EQ(firstIdentity.Sequence(), secondIdentity.Sequence());
    EXPECT_FALSE(firstIdentity.SamePreparation(secondIdentity));
    harken::ActionFiringTable::ChildActivationReceipt firstReceipt, secondReceipt;
    ASSERT_TRUE(table.CommitPreparedChild(&first, &firstReceipt));
    EXPECT_FALSE(table.CommitPreparedChild(&second, &firstReceipt));
    EXPECT_TRUE(second.Valid());
    EXPECT_FALSE(firstReceipt.MatchesOriginalPreparation(secondIdentity));
    ASSERT_TRUE(table.CommitPreparedChild(&second, &secondReceipt));
    EXPECT_TRUE(secondReceipt.MatchesOriginalPreparation(secondIdentity));
    EXPECT_FALSE(secondReceipt.MatchesOriginalPreparation(firstIdentity));
}

TEST(ActionFiringTable, AbandonedPreparedIdentityCannotAuthorizeReusedSlotOrLaterActivation) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(2));
    harken::PreparedActionFiring root, child;
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(1), 7, 1, &root));
    ASSERT_TRUE(table.Commit(&root, 99));
    ASSERT_TRUE(table.PrepareChild(root.Handle(), harkentest::ObjectId(2),
        harken::ActionFiringOwnerStamp::DirectWorld(7), 1, &child));
    harken::ActionFiringTable::PreparedChildIdentity oldIdentity, replacementIdentity;
    ASSERT_TRUE(table.CapturePreparedChildIdentity(child, 1, &oldIdentity));
    ASSERT_TRUE(table.Abandon(&child));
    EXPECT_FALSE(table.IsPreparedChildActive(oldIdentity));
    ASSERT_TRUE(table.PrepareChild(root.Handle(), harkentest::ObjectId(2),
        harken::ActionFiringOwnerStamp::DirectWorld(7), 1, &child));
    ASSERT_TRUE(table.CapturePreparedChildIdentity(child, 1, &replacementIdentity));
    EXPECT_NE(oldIdentity.Child(), replacementIdentity.Child());
    EXPECT_EQ(oldIdentity.Sequence(), replacementIdentity.Sequence());
    EXPECT_FALSE(table.IsPreparedChildActive(oldIdentity));
    harken::ActionFiringTable::ChildActivationReceipt receipt;
    ASSERT_TRUE(table.CommitPreparedChild(&child, &receipt));
    EXPECT_FALSE(receipt.MatchesOriginalPreparation(oldIdentity));
    EXPECT_TRUE(receipt.MatchesOriginalPreparation(replacementIdentity));
}

TEST(ActionFiringTable, NewChildCannotRecaptureAnOldDeferredCancellationEpoch) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(2));
    harken::PreparedActionFiring root, child;
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(1), 7, 1, &root));
    ASSERT_TRUE(table.Commit(&root, 99));
    const auto originalEpoch = table.CancellationEpoch(root.Handle());
    ASSERT_TRUE(table.BumpCancellationEpoch(root.Handle()));
    ASSERT_TRUE(table.PrepareChild(root.Handle(), harkentest::ObjectId(2),
        harken::ActionFiringOwnerStamp::DirectWorld(7), 1, &child));
    harken::ActionFiringTable::PreparedChildIdentity identity;
    EXPECT_FALSE(table.CapturePreparedChildIdentity(child, originalEpoch, &identity));
    ASSERT_TRUE(table.CapturePreparedChildIdentity(child, table.CancellationEpoch(root.Handle()), &identity));
    ASSERT_TRUE(table.Abandon(&child));
    EXPECT_FALSE(table.IsPreparedChildActive(identity));
}

TEST(ActionFiringTable, ActiveIntermediateParentCannotHideRootCancellationFromPreparedChild) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(3));
    harken::PreparedActionFiring root, parent, child;
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(1), 7, 1, &root));
    ASSERT_TRUE(table.PrepareChild(root, harkentest::ObjectId(2), 7, 1, &parent));
    ASSERT_TRUE(table.Commit(&root, 99));
    ASSERT_TRUE(table.PrepareChild(parent.Handle(), harkentest::ObjectId(3),
        harken::ActionFiringOwnerStamp::DirectWorld(7), 1, &child));
    ASSERT_TRUE(table.BumpCancellationEpoch(root.Handle()));
    harken::ActionFiringSnapshot activeParent{};
    ASSERT_TRUE(table.Capture(parent.Handle(), &activeParent));
    ASSERT_EQ(harken::ActionFiringState::Active, activeParent.state);
    EXPECT_EQ(0u, child.InheritedSequence());
    EXPECT_FALSE(table.CommitPreparedChild(&child));
    EXPECT_TRUE(child.Valid());
    EXPECT_EQ(2u, table.LiveCount());
    ASSERT_TRUE(table.Abandon(&child));
}

TEST(ActionFiringTable, GenerationExhaustionQuarantinesWithoutRevivingAHandle) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(1));
    ASSERT_TRUE(table.SetFreeGenerationForTests(
        0, harken::kMaxActionFiringGeneration));

    harken::PreparedActionFiring prepared{};
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(1), 1, 0, &prepared));
    const harken::ActionFiringHandle maximumHandle = prepared.Handle();
    ASSERT_TRUE(table.Commit(&prepared, 1));
    harken::ActionFiringSnapshot terminal{};
    ASSERT_TRUE(table.Capture(maximumHandle, &terminal));
    ASSERT_EQ(harken::ActionFiringState::TerminalRetained, terminal.state);
    ASSERT_TRUE(table.ReleaseRoot(maximumHandle));

    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(0u, table.FreeCount());
    EXPECT_EQ(1u, table.QuarantinedCount());
    harken::ActionFiringSnapshot snapshot{};
    EXPECT_FALSE(table.Capture(maximumHandle, &snapshot));

    harken::PreparedActionFiring refused{};
    EXPECT_FALSE(table.PrepareRoot(harkentest::ObjectId(2), 1, 0, &refused));
    EXPECT_FALSE(refused.Valid());
}

TEST(ActionFiringTable, TreeDeferredCountIncludesRootAndDescendants) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(2));

    harken::PreparedActionFiring root{};
    harken::PreparedActionFiring child{};
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(1), 1, 1, &root));
    ASSERT_TRUE(table.PrepareChild(
        root, harkentest::ObjectId(2), 1, 1, &child));
    const auto rootHandle = root.Handle();
    const auto childHandle = child.Handle();

    harken::ActionFiringStepResult active{};
    active.state = harken::ActionStepState::Active;
    active.opcode =
        static_cast<std::uint32_t>(harken::ActionOpcodeV1::Play);
    ASSERT_TRUE(table.SetPreparedStep(root, 0, active));
    ASSERT_TRUE(table.SetPreparedStep(child, 0, active));
    ASSERT_TRUE(table.CommitTree(&root, 1));

    std::uint32_t count = 99;
    ASSERT_TRUE(table.TreeOwnedDeferredCount(rootHandle, &count));
    EXPECT_EQ(0u, count);
    ASSERT_TRUE(table.AcquireDeferred(rootHandle));
    ASSERT_TRUE(table.AcquireDeferred(childHandle));
    ASSERT_TRUE(table.TreeOwnedDeferredCount(rootHandle, &count));
    EXPECT_EQ(2u, count);
    EXPECT_FALSE(table.TreeOwnedDeferredCount(childHandle, &count));
}

TEST(ActionFiringTable, ResetDoesNotReviveAHandleFromAnEarlierSession) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(1));

    harken::PreparedActionFiring first{};
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(1), 1, 0, &first));
    const harken::ActionFiringHandle oldHandle = first.Handle();
    ASSERT_TRUE(table.Commit(&first, 1));

    ASSERT_TRUE(table.Reset(1));
    harken::PreparedActionFiring second{};
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(2), 2, 0, &second));
    const harken::ActionFiringHandle newHandle = second.Handle();
    ASSERT_TRUE(table.Commit(&second, 2));

    EXPECT_NE(oldHandle, newHandle);
    harken::ActionFiringSnapshot snapshot{};
    EXPECT_FALSE(table.Capture(oldHandle, &snapshot));
    ASSERT_TRUE(table.Capture(newHandle, &snapshot));
    EXPECT_EQ(harkentest::ObjectId(2), snapshot.actionId);
}

TEST(ActionFiringTable, RootOwnsAndReleasesItsCommittedChildTreePostOrder) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(3));

    harken::PreparedActionFiring root{};
    harken::PreparedActionFiring child{};
    harken::PreparedActionFiring grandchild{};
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(10), 4, 1, &root));
    ASSERT_TRUE(table.PrepareChild(root, harkentest::ObjectId(20), 4, 1, &child));
    ASSERT_TRUE(table.PrepareChild(child, harkentest::ObjectId(30), 4, 0, &grandchild));
    const auto rootHandle = root.Handle();
    const auto childHandle = child.Handle();
    const auto grandchildHandle = grandchild.Handle();

    harken::ActionFiringStepResult rootPost{};
    rootPost.state = harken::ActionStepState::Active;
    rootPost.opcode =
        static_cast<std::uint32_t>(harken::ActionOpcodeV1::PostAction);
    rootPost.childFiring = childHandle;
    rootPost.authoredEventIndex = 0;
    ASSERT_TRUE(table.SetPreparedStep(root, 0, rootPost));
    harken::ActionFiringStepResult childPost = rootPost;
    childPost.childFiring = grandchildHandle;
    ASSERT_TRUE(table.SetPreparedStep(child, 0, childPost));

    ASSERT_TRUE(table.CommitTree(&root, 55));
    EXPECT_FALSE(root.Valid());
    EXPECT_FALSE(child.Valid());
    EXPECT_FALSE(grandchild.Valid());
    EXPECT_EQ(3u, table.LiveCount());

    harken::ActionFiringSnapshot rootSnapshot{};
    harken::ActionFiringSnapshot childSnapshot{};
    ASSERT_TRUE(table.Capture(rootHandle, &rootSnapshot));
    ASSERT_TRUE(table.Capture(childHandle, &childSnapshot));
    EXPECT_EQ(0u, rootSnapshot.parent);
    EXPECT_EQ(1u, rootSnapshot.childFiringCount);
    EXPECT_EQ(rootHandle, childSnapshot.parent);
    EXPECT_EQ(1u, childSnapshot.childFiringCount);
    EXPECT_EQ(55u, childSnapshot.acceptedLogicSequence);
    EXPECT_EQ(harken::ActionFiringState::TerminalRetained,
              rootSnapshot.state);
    EXPECT_EQ(harken::ActionFiringState::TerminalRetained,
              childSnapshot.state);
    harken::ActionFiringSnapshot grandchildSnapshot{};
    ASSERT_TRUE(table.Capture(grandchildHandle, &grandchildSnapshot));
    EXPECT_EQ(harken::ActionFiringState::TerminalRetained,
              grandchildSnapshot.state);

    EXPECT_FALSE(table.ReleaseRoot(childHandle));
    ASSERT_TRUE(table.ReleaseRoot(rootHandle));
    EXPECT_FALSE(table.Capture(rootHandle, &rootSnapshot));
    EXPECT_FALSE(table.Capture(childHandle, &childSnapshot));
    EXPECT_FALSE(table.Capture(grandchildHandle, &grandchildSnapshot));
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(3u, table.FreeCount());
}

TEST(ActionFiringTable, CommittedParentCanAttachAPreparedChildLater) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(2));

    harken::PreparedActionFiring root{};
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(10), 4, 1, &root));
    harken::ActionFiringStepResult pending{};
    pending.state = harken::ActionStepState::AwaitingMaterialization;
    pending.opcode =
        static_cast<std::uint32_t>(harken::ActionOpcodeV1::PostAction);
    pending.authoredEventIndex = 0;
    ASSERT_TRUE(table.SetPreparedStep(root, 0, pending));
    const auto rootHandle = root.Handle();
    ASSERT_TRUE(table.Commit(&root, 7));
    ASSERT_TRUE(table.AcquireDeferred(rootHandle));

    harken::PreparedActionFiring child{};
    ASSERT_TRUE(table.PrepareChild(
        rootHandle, harkentest::ObjectId(20), harken::ActionFiringOwnerStamp::DirectWorld(4), 0,
        &child));
    const auto childHandle = child.Handle();
    ASSERT_TRUE(table.CommitPreparedChild(&child));
    harken::ActionFiringStepResult attached = pending;
    attached.state = harken::ActionStepState::Succeeded;
    attached.childFiring = childHandle;
    ASSERT_TRUE(table.TransitionStep(
        rootHandle, 0, harken::ActionStepState::AwaitingMaterialization,
        attached));
    ASSERT_TRUE(table.ReleaseDeferred(rootHandle));

    harken::ActionFiringSnapshot rootSnapshot{};
    harken::ActionFiringSnapshot childSnapshot{};
    ASSERT_TRUE(table.Capture(rootHandle, &rootSnapshot));
    ASSERT_TRUE(table.Capture(childHandle, &childSnapshot));
    EXPECT_EQ(rootHandle, childSnapshot.parent);
    EXPECT_EQ(1u, rootSnapshot.childFiringCount);
    EXPECT_EQ(harken::ActionFiringState::TerminalRetained, rootSnapshot.state);
    EXPECT_EQ(harken::ActionFiringState::TerminalRetained, childSnapshot.state);
}

TEST(ActionFiringTable, AbandoningPreparedChildOfLiveParentRestoresParent) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(2));

    harken::PreparedActionFiring root{};
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(10), 4, 1, &root));
    harken::ActionFiringStepResult pending{};
    pending.state = harken::ActionStepState::AwaitingMaterialization;
    pending.opcode =
        static_cast<std::uint32_t>(harken::ActionOpcodeV1::PostAction);
    pending.authoredEventIndex = 0;
    ASSERT_TRUE(table.SetPreparedStep(root, 0, pending));
    const auto rootHandle = root.Handle();
    ASSERT_TRUE(table.Commit(&root, 7));
    ASSERT_TRUE(table.AcquireDeferred(rootHandle));

    harken::PreparedActionFiring child{};
    ASSERT_TRUE(table.PrepareChild(
        rootHandle, harkentest::ObjectId(20), harken::ActionFiringOwnerStamp::DirectWorld(4), 0,
        &child));
    const auto childHandle = child.Handle();
    ASSERT_TRUE(table.Abandon(&child));
    EXPECT_FALSE(child.Valid());

    harken::ActionFiringSnapshot rootSnapshot{};
    ASSERT_TRUE(table.Capture(rootHandle, &rootSnapshot));
    EXPECT_EQ(0u, rootSnapshot.childFiringCount);
    EXPECT_EQ(harken::ActionFiringState::Active, rootSnapshot.state);
    EXPECT_EQ(1u, table.LiveCount());
    EXPECT_EQ(1u, table.FreeCount());
    harken::ActionFiringSnapshot childSnapshot{};
    EXPECT_FALSE(table.Capture(childHandle, &childSnapshot));
    ASSERT_TRUE(table.ReleaseDeferred(rootHandle));
}

TEST(ActionFiringTable, CommitPreparedChildActivatesThePreparedSubtree) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(3));

    harken::PreparedActionFiring root{};
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(10), 4, 1, &root));
    harken::ActionFiringStepResult pending{};
    pending.state = harken::ActionStepState::AwaitingMaterialization;
    pending.opcode =
        static_cast<std::uint32_t>(harken::ActionOpcodeV1::PostAction);
    pending.authoredEventIndex = 0;
    ASSERT_TRUE(table.SetPreparedStep(root, 0, pending));
    const auto rootHandle = root.Handle();
    ASSERT_TRUE(table.Commit(&root, 7));
    ASSERT_TRUE(table.AcquireDeferred(rootHandle));

    harken::PreparedActionFiring child{};
    harken::PreparedActionFiring grandchild{};
    ASSERT_TRUE(table.PrepareChild(
        rootHandle, harkentest::ObjectId(20), harken::ActionFiringOwnerStamp::DirectWorld(4), 1,
        &child));
    ASSERT_TRUE(table.PrepareChild(
        child, harkentest::ObjectId(30), harken::ActionFiringOwnerStamp::DirectWorld(4), 0,
        &grandchild));
    harken::ActionFiringStepResult childStep{};
    childStep.state = harken::ActionStepState::Succeeded;
    childStep.opcode =
        static_cast<std::uint32_t>(harken::ActionOpcodeV1::PostAction);
    childStep.authoredEventIndex = 0;
    childStep.childFiring = grandchild.Handle();
    ASSERT_TRUE(table.SetPreparedStep(child, 0, childStep));
    const auto childHandle = child.Handle();
    const auto grandchildHandle = grandchild.Handle();
    ASSERT_TRUE(table.CommitPreparedChild(&child));

    harken::ActionFiringSnapshot childSnapshot{};
    harken::ActionFiringSnapshot grandchildSnapshot{};
    ASSERT_TRUE(table.Capture(childHandle, &childSnapshot));
    ASSERT_TRUE(table.Capture(grandchildHandle, &grandchildSnapshot));
    EXPECT_EQ(rootHandle, childSnapshot.parent);
    EXPECT_EQ(childHandle, grandchildSnapshot.parent);
    EXPECT_EQ(1u, childSnapshot.childFiringCount);
    EXPECT_EQ(harken::ActionFiringState::Active, childSnapshot.state);
    EXPECT_EQ(harken::ActionFiringState::Active, grandchildSnapshot.state);

    harken::ActionFiringStepResult attached = pending;
    attached.state = harken::ActionStepState::Succeeded;
    attached.childFiring = childHandle;
    ASSERT_TRUE(table.TransitionStep(
        rootHandle, 0, harken::ActionStepState::AwaitingMaterialization,
        attached));
    ASSERT_TRUE(table.ReleaseDeferred(rootHandle));
    harken::ActionFiringSnapshot rootSnapshot{};
    ASSERT_TRUE(table.Capture(rootHandle, &rootSnapshot));
    EXPECT_EQ(harken::ActionFiringState::TerminalRetained, rootSnapshot.state);
}

TEST(ActionFiringTable, AbandoningPreparedRootReleasesItsWholeHiddenTree) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(3));

    harken::PreparedActionFiring root{};
    harken::PreparedActionFiring child{};
    harken::PreparedActionFiring grandchild{};
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(10), 4, 1, &root));
    ASSERT_TRUE(table.PrepareChild(root, harkentest::ObjectId(20), 4, 1, &child));
    ASSERT_TRUE(table.PrepareChild(child, harkentest::ObjectId(30), 4, 0, &grandchild));
    const auto childHandle = child.Handle();
    const auto grandchildHandle = grandchild.Handle();

    ASSERT_TRUE(table.Abandon(&root));
    EXPECT_FALSE(root.Valid());
    EXPECT_FALSE(child.Valid());
    EXPECT_FALSE(grandchild.Valid());
    EXPECT_EQ(3u, table.FreeCount());
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(0u, table.HighWater());

    harken::ActionFiringSnapshot snapshot{};
    EXPECT_FALSE(table.Capture(childHandle, &snapshot));
    EXPECT_FALSE(table.Capture(grandchildHandle, &snapshot));
}

TEST(ActionFiringTable, ChildSuccessCompletesParentPostActionAndRoot) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(2));

    harken::PreparedActionFiring root{};
    harken::PreparedActionFiring child{};
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(10), 4, 1, &root));
    ASSERT_TRUE(table.PrepareChild(root, harkentest::ObjectId(20), 4, 1, &child));
    const auto rootHandle = root.Handle();
    const auto childHandle = child.Handle();

    harken::ActionFiringStepResult post{};
    post.state = harken::ActionStepState::Active;
    post.result = 0;
    post.opcode = static_cast<std::uint32_t>(harken::ActionOpcodeV1::PostAction);
    post.childFiring = childHandle;
    post.authoredEventIndex = 0;
    post.executionOrdinal = 0;
    ASSERT_TRUE(table.SetPreparedStep(root, 0, post));

    harken::ActionFiringStepResult setValue{};
    setValue.state = harken::ActionStepState::Succeeded;
    setValue.result = 0;
    setValue.opcode =
        static_cast<std::uint32_t>(harken::ActionOpcodeV1::SetVariable);
    setValue.authoredEventIndex = 0;
    setValue.executionOrdinal = 1;
    ASSERT_TRUE(table.SetPreparedStep(child, 0, setValue));

    ASSERT_TRUE(table.CommitTree(&root, 55));

    harken::ActionFiringSnapshot rootSnapshot{};
    harken::ActionFiringSnapshot childSnapshot{};
    ASSERT_TRUE(table.Capture(rootHandle, &rootSnapshot));
    ASSERT_TRUE(table.Capture(childHandle, &childSnapshot));
    EXPECT_EQ(harken::ActionFiringState::TerminalRetained,
              childSnapshot.state);
    EXPECT_EQ(harken::ActionFiringState::TerminalRetained,
              rootSnapshot.state);
    EXPECT_EQ(harken::ActionFiringOutcome::Succeeded,
              rootSnapshot.outcome);
    EXPECT_EQ(harken::ActionTerminalReason::Natural,
              rootSnapshot.terminalReason);

    harken::ActionFiringStepResult completedPost{};
    ASSERT_TRUE(table.CaptureStep(rootHandle, 0, &completedPost));
    EXPECT_EQ(harken::ActionStepState::Succeeded, completedPost.state);
    EXPECT_EQ(0, completedPost.result);
    EXPECT_EQ(childHandle, completedPost.childFiring);
    ASSERT_TRUE(table.ReleaseRoot(rootHandle));
}

TEST(ActionFiringTable, ChildFailurePropagatesResultReasonAndPartialOutcome) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(2));

    harken::PreparedActionFiring root{};
    harken::PreparedActionFiring child{};
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(10), 4, 2, &root));
    ASSERT_TRUE(table.PrepareChild(root, harkentest::ObjectId(20), 4, 1, &child));
    const auto rootHandle = root.Handle();
    const auto childHandle = child.Handle();

    harken::ActionFiringStepResult post{};
    post.state = harken::ActionStepState::Active;
    post.opcode =
        static_cast<std::uint32_t>(harken::ActionOpcodeV1::PostAction);
    post.childFiring = childHandle;
    post.authoredEventIndex = 0;
    ASSERT_TRUE(table.SetPreparedStep(root, 0, post));
    harken::ActionFiringStepResult succeeded{};
    succeeded.state = harken::ActionStepState::Succeeded;
    succeeded.opcode =
        static_cast<std::uint32_t>(harken::ActionOpcodeV1::SetVariable);
    succeeded.authoredEventIndex = 1;
    ASSERT_TRUE(table.SetPreparedStep(root, 1, succeeded));
    harken::ActionFiringStepResult active{};
    active.state = harken::ActionStepState::Active;
    active.opcode =
        static_cast<std::uint32_t>(harken::ActionOpcodeV1::SetVariable);
    active.authoredEventIndex = 0;
    ASSERT_TRUE(table.SetPreparedStep(child, 0, active));
    ASSERT_TRUE(table.CommitTree(&root, 55));

    harken::ActionFiringStepResult failed = active;
    failed.state = harken::ActionStepState::Failed;
    failed.result = -306;
    ASSERT_TRUE(table.UpdateStep(childHandle, 0, failed));
    ASSERT_TRUE(table.MarkTerminal(
        childHandle, harken::ActionFiringOutcome::Failed,
        harken::ActionTerminalReason::Capacity));

    harken::ActionFiringSnapshot rootSnapshot{};
    ASSERT_TRUE(table.Capture(rootHandle, &rootSnapshot));
    EXPECT_EQ(harken::ActionFiringState::TerminalRetained,
              rootSnapshot.state);
    EXPECT_EQ(harken::ActionFiringOutcome::SucceededWithStepFailure,
              rootSnapshot.outcome);
    EXPECT_EQ(harken::ActionTerminalReason::Capacity,
              rootSnapshot.terminalReason);
    harken::ActionFiringStepResult completedPost{};
    ASSERT_TRUE(table.CaptureStep(rootHandle, 0, &completedPost));
    EXPECT_EQ(harken::ActionStepState::Failed, completedPost.state);
    EXPECT_EQ(-306, completedPost.result);
    ASSERT_TRUE(table.ReleaseRoot(rootHandle));
}

TEST(ActionFiringTable, GrandchildFailurePropagatesThroughEveryAncestor) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(3));

    harken::PreparedActionFiring root{};
    harken::PreparedActionFiring child{};
    harken::PreparedActionFiring grandchild{};
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(10), 4, 1, &root));
    ASSERT_TRUE(table.PrepareChild(root, harkentest::ObjectId(20), 4, 1, &child));
    ASSERT_TRUE(table.PrepareChild(child, harkentest::ObjectId(30), 4, 1, &grandchild));
    const auto rootHandle = root.Handle();
    const auto childHandle = child.Handle();
    const auto grandchildHandle = grandchild.Handle();

    harken::ActionFiringStepResult rootPost{};
    rootPost.state = harken::ActionStepState::Active;
    rootPost.opcode =
        static_cast<std::uint32_t>(harken::ActionOpcodeV1::PostAction);
    rootPost.childFiring = childHandle;
    ASSERT_TRUE(table.SetPreparedStep(root, 0, rootPost));

    harken::ActionFiringStepResult childPost = rootPost;
    childPost.childFiring = grandchildHandle;
    ASSERT_TRUE(table.SetPreparedStep(child, 0, childPost));

    harken::ActionFiringStepResult active{};
    active.state = harken::ActionStepState::Active;
    active.opcode =
        static_cast<std::uint32_t>(harken::ActionOpcodeV1::SetVariable);
    ASSERT_TRUE(table.SetPreparedStep(grandchild, 0, active));
    ASSERT_TRUE(table.CommitTree(&root, 55));

    harken::ActionFiringStepResult failed = active;
    failed.state = harken::ActionStepState::Failed;
    failed.result = -306;
    failed.terminalReason = harken::ActionTerminalReason::Capacity;
    ASSERT_TRUE(table.UpdateStep(grandchildHandle, 0, failed));
    ASSERT_TRUE(table.MarkTerminal(
        grandchildHandle, harken::ActionFiringOutcome::Failed,
        harken::ActionTerminalReason::Capacity));

    harken::ActionFiringSnapshot rootSnapshot{};
    harken::ActionFiringSnapshot childSnapshot{};
    harken::ActionFiringSnapshot grandchildSnapshot{};
    ASSERT_TRUE(table.Capture(rootHandle, &rootSnapshot));
    ASSERT_TRUE(table.Capture(childHandle, &childSnapshot));
    ASSERT_TRUE(table.Capture(grandchildHandle, &grandchildSnapshot));
    EXPECT_EQ(harken::ActionFiringState::TerminalRetained,
              rootSnapshot.state);
    EXPECT_EQ(harken::ActionFiringState::TerminalRetained,
              childSnapshot.state);
    EXPECT_EQ(harken::ActionFiringState::TerminalRetained,
              grandchildSnapshot.state);
    EXPECT_EQ(harken::ActionFiringOutcome::Failed, rootSnapshot.outcome);
    EXPECT_EQ(harken::ActionFiringOutcome::Failed, childSnapshot.outcome);
    EXPECT_EQ(harken::ActionTerminalReason::Capacity,
              rootSnapshot.terminalReason);

    harken::ActionFiringStepResult rootCompletedPost{};
    harken::ActionFiringStepResult childCompletedPost{};
    ASSERT_TRUE(table.CaptureStep(rootHandle, 0, &rootCompletedPost));
    ASSERT_TRUE(table.CaptureStep(childHandle, 0, &childCompletedPost));
    EXPECT_EQ(harken::ActionStepState::Failed, rootCompletedPost.state);
    EXPECT_EQ(harken::ActionStepState::Failed, childCompletedPost.state);
    EXPECT_EQ(-306, rootCompletedPost.result);
    EXPECT_EQ(-306, childCompletedPost.result);
    ASSERT_TRUE(table.ReleaseRoot(rootHandle));
}

TEST(ActionFiringTable, PlayableFailureRetainsExactResultAndReason) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(1));

    harken::PreparedActionFiring root{};
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(10), 4, 1, &root));
    const auto rootHandle = root.Handle();

    harken::ActionFiringStepResult play{};
    play.state = harken::ActionStepState::Active;
    play.opcode = static_cast<std::uint32_t>(harken::ActionOpcodeV1::Play);
    play.executionOrdinal = 7;
    play.producedPlayableInstance = 99;
    ASSERT_TRUE(table.SetPreparedStep(root, 0, play));
    ASSERT_TRUE(table.CommitTree(&root, 55));
    ASSERT_TRUE(table.SetLivePlayableCount(rootHandle, 1));

    ASSERT_TRUE(table.CompletePlayable(
        rootHandle, 7, -328, harken::ActionTerminalReason::RuntimeError));

    harken::ActionFiringSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(rootHandle, &snapshot));
    EXPECT_EQ(harken::ActionFiringState::TerminalRetained, snapshot.state);
    EXPECT_EQ(harken::ActionFiringOutcome::Failed, snapshot.outcome);
    EXPECT_EQ(harken::ActionTerminalReason::RuntimeError,
              snapshot.terminalReason);
    harken::ActionFiringStepResult completed{};
    ASSERT_TRUE(table.CaptureStep(rootHandle, 0, &completed));
    EXPECT_EQ(harken::ActionStepState::Failed, completed.state);
    EXPECT_EQ(-328, completed.result);
    EXPECT_EQ(harken::ActionTerminalReason::RuntimeError,
              completed.terminalReason);
    ASSERT_TRUE(table.ReleaseRoot(rootHandle));
}

TEST(ActionFiringTable, RootCancellationPropagatesThroughChildTree) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(2));

    harken::PreparedActionFiring root{};
    harken::PreparedActionFiring child{};
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(10), 4, 1, &root));
    ASSERT_TRUE(table.PrepareChild(root, harkentest::ObjectId(20), 4, 1, &child));
    const auto rootHandle = root.Handle();
    const auto childHandle = child.Handle();

    harken::ActionFiringStepResult post{};
    post.state = harken::ActionStepState::Active;
    post.opcode =
        static_cast<std::uint32_t>(harken::ActionOpcodeV1::PostAction);
    post.childFiring = childHandle;
    post.authoredEventIndex = 0;
    ASSERT_TRUE(table.SetPreparedStep(root, 0, post));
    harken::ActionFiringStepResult childStep{};
    childStep.state = harken::ActionStepState::Active;
    childStep.opcode =
        static_cast<std::uint32_t>(harken::ActionOpcodeV1::Play);
    childStep.authoredEventIndex = 0;
    ASSERT_TRUE(table.SetPreparedStep(child, 0, childStep));
    ASSERT_TRUE(table.CommitTree(&root, 55));

    ASSERT_TRUE(table.MarkStopping(
        rootHandle, harken::ActionTerminalReason::StopRequested));

    harken::ActionFiringSnapshot rootSnapshot{};
    harken::ActionFiringSnapshot childSnapshot{};
    ASSERT_TRUE(table.Capture(rootHandle, &rootSnapshot));
    ASSERT_TRUE(table.Capture(childHandle, &childSnapshot));
    EXPECT_EQ(harken::ActionFiringState::TerminalRetained,
              rootSnapshot.state);
    EXPECT_EQ(harken::ActionFiringState::TerminalRetained,
              childSnapshot.state);
    EXPECT_EQ(harken::ActionFiringOutcome::Cancelled,
              rootSnapshot.outcome);
    EXPECT_EQ(harken::ActionTerminalReason::StopRequested,
              rootSnapshot.terminalReason);
    harken::ActionFiringStepResult cancelledPost{};
    ASSERT_TRUE(table.CaptureStep(rootHandle, 0, &cancelledPost));
    EXPECT_EQ(harken::ActionStepState::Cancelled, cancelledPost.state);
    ASSERT_TRUE(table.ReleaseRoot(rootHandle));
}

TEST(ActionFiringTable, PackOwnerAndCancellationEpochAreCapturedExactly) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(1));

    const auto owner = harken::ActionFiringOwnerStamp::Pack(PackAnchor());
    harken::PreparedActionFiring prepared{};
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(41), owner, 1, &prepared));
    harken::ActionFiringStepResult step{};
    step.authoredEventIndex = 0;
    step.state = harken::ActionStepState::Active;
    ASSERT_TRUE(table.SetPreparedStep(prepared, 0, step));
    const auto handle = prepared.Handle();
    ASSERT_TRUE(table.Commit(&prepared, 99));

    harken::ActionFiringSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(handle, &snapshot));
    EXPECT_EQ(snapshot.owner, owner);
    EXPECT_EQ(snapshot.worldGeneration, owner.pack.compositeGeneration);
    EXPECT_EQ(snapshot.cancellationEpoch, 1u);
    EXPECT_EQ(snapshot.ownedDeferredCount, 0u);

    std::uint64_t bumped = 0;
    ASSERT_TRUE(table.BumpCancellationEpoch(handle, &bumped));
    EXPECT_EQ(bumped, 2u);
    EXPECT_EQ(table.CancellationEpoch(handle), 2u);
    ASSERT_TRUE(table.Capture(handle, &snapshot));
    EXPECT_EQ(snapshot.cancellationEpoch, 2u);

    ASSERT_TRUE(table.MarkStopping(handle,
                                   harken::ActionTerminalReason::StopRequested));
    ASSERT_TRUE(table.Capture(handle, &snapshot));
    EXPECT_EQ(snapshot.cancellationEpoch, 3u);
    EXPECT_EQ(snapshot.state, harken::ActionFiringState::TerminalRetained);
    EXPECT_TRUE(table.MarkStopping(
        handle, harken::ActionTerminalReason::StopRequested));
    EXPECT_EQ(table.CancellationEpoch(handle), 3u);
    ASSERT_TRUE(table.ReleaseRoot(handle));
}

TEST(ActionFiringTable, DeferredOwnershipBlocksTerminalFinalizationAndRelease) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(1));

    harken::PreparedActionFiring prepared{};
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(41), 7, 1, &prepared));
    harken::ActionFiringStepResult step{};
    step.authoredEventIndex = 0;
    step.state = harken::ActionStepState::Active;
    ASSERT_TRUE(table.SetPreparedStep(prepared, 0, step));
    const auto handle = prepared.Handle();
    ASSERT_TRUE(table.Commit(&prepared, 99));
    ASSERT_TRUE(table.AcquireDeferred(handle));
    EXPECT_EQ(table.OwnedDeferredCount(handle), 1u);

    step.state = harken::ActionStepState::Succeeded;
    ASSERT_TRUE(table.UpdateStep(handle, 0, step));

    harken::ActionFiringSnapshot snapshot{};
    ASSERT_TRUE(table.Capture(handle, &snapshot));
    EXPECT_EQ(snapshot.state, harken::ActionFiringState::Active);
    EXPECT_EQ(snapshot.ownedDeferredCount, 1u);
    EXPECT_FALSE(table.MarkTerminal(handle));
    EXPECT_FALSE(table.ReleaseRoot(handle));

    ASSERT_TRUE(table.ReleaseDeferred(handle));
    EXPECT_EQ(table.OwnedDeferredCount(handle), 0u);
    ASSERT_TRUE(table.Capture(handle, &snapshot));
    EXPECT_EQ(snapshot.state, harken::ActionFiringState::TerminalRetained);
    ASSERT_TRUE(table.ReleaseRoot(handle));
}

TEST(ActionFiringTable, RequestAndFirstControlCauseSurviveTerminalRetentionAndClearOnReuse) {
    harken::ActionFiringTable table;
    ASSERT_TRUE(table.Reset(2));
    harken::MonitorRequestSource requests(91);
    const auto play = requests.Begin(harken::MonitorRequestOrigin::NativeAPI, 1);
    harken::PreparedActionFiring root, child;
    const harken::MonitorEmitterOrigin origin{41, {91, 5, 3}, 7,
        harken::MonitorEmitterKind::ExplicitStateOnly, harken::MonitorHostOwnership::UnityEmitter};
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(1), 7, 1, &root, play, origin));
    ASSERT_TRUE(table.PrepareChild(root, harkentest::ObjectId(2), 7, 1, &child));
    harken::ActionFiringStepResult post{};
    post.state = harken::ActionStepState::Active;
    post.opcode = static_cast<std::uint32_t>(harken::ActionOpcodeV1::PostAction);
    post.childFiring = child.Handle();
    ASSERT_TRUE(table.SetPreparedStep(root, 0, post));
    ASSERT_TRUE(table.CommitTree(&root, 99));
    const harken::MonitorControlCause stop{requests.Begin(harken::MonitorRequestOrigin::NativeAPI, 6)};
    ASSERT_TRUE(table.MarkTreeStopping(root.Handle(), harken::ActionTerminalReason::StopRequested, stop));
    const harken::MonitorControlCause repeated{requests.Begin(harken::MonitorRequestOrigin::NativeAPI, 6)};
    ASSERT_TRUE(table.MarkTreeStopping(child.Handle(), harken::ActionTerminalReason::StopRequested, repeated));
    for (const auto handle : {root.Handle(), child.Handle()}) {
        harken::ActionFiringSnapshot identity{};
        ASSERT_TRUE(table.Capture(handle, &identity));
        EXPECT_EQ(identity.playbackRequest.key, play.key);
        EXPECT_EQ(identity.emitterOrigin.host, origin.host);
        EXPECT_EQ(identity.emitterOrigin.kind, origin.kind);
        EXPECT_EQ(identity.emitterOrigin.registrationRevision, origin.registrationRevision);
        EXPECT_EQ(identity.controlCause.request.key, stop.request.key);
        EXPECT_EQ(identity.root, root.Handle());
    }
    ASSERT_TRUE(table.ReleaseRoot(root.Handle()));
    ASSERT_TRUE(table.PrepareRoot(harkentest::ObjectId(1), 8, 1, &root));
    ASSERT_TRUE(table.Commit(&root, 100));
    harken::ActionFiringSnapshot reused{};
    ASSERT_TRUE(table.Capture(root.Handle(), &reused));
    EXPECT_FALSE(reused.playbackRequest.key.Valid());
    EXPECT_FALSE(reused.emitterOrigin.host.Valid());
    EXPECT_EQ(harken::MonitorEmitterKind::Unknown, reused.emitterOrigin.kind);
    EXPECT_FALSE(reused.controlCause.request.key.Valid());
}
