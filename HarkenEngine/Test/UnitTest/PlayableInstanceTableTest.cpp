// Playable-instance table contract tests.
//
// The mechanisms these cases hold up are the ones a later caller cannot
// re-derive by reading a value: that a handle to an ended instance reads as
// stale rather than as the next occupant, that the lifecycle cannot be walked
// backwards, and that a cancelled instance invalidates work stamped against it
// without anyone having to find that work first.

#include "Common/ObjectIdTestSupport.h"
#include "harken/core/PlayableInstanceTable.h"
#include "harken/core/ContainerPlaybackTable.h"
#include "harken/core/CurrentWorld.h"
#include "PlayableInstanceTableTestAccess.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <type_traits>
#include <utility>
#include <vector>

namespace harken::detail {
struct ContainerPlaybackTableTestAccess {
    static bool RollbackOwned(ContainerPlaybackTable& table, const PreparedContainerPlaybackRow& cap) {
        if (cap.owner_ != &table || cap.preparedOwned_ || cap.slot_ >= table.rows_.size()) return false;
        const auto& row = table.rows_[cap.slot_];
        if (!row.live || row.instance != cap.instance_ || row.expectedScheduleEpoch != cap.expectedScheduleEpoch_)
            return false;
        return table.RollbackCommitted(cap.instance_);
    }
    static ContainerPlaybackRow CorruptPreparing(ContainerPlaybackTable& table,
        const PreparedContainerPlaybackRow& cap, unsigned field) {
        auto* row = table.FindPrepared(cap);
        if (row == nullptr) return {};
        const auto before = *row;
        if (field == 0) ++row->expectedScheduleEpoch;
        if (field == 1) ++row->containerStableId.low;
        if (field == 2) ++row->programSemanticFingerprint;
        if (field == 3) ++row->mode;
        if (field == 4) ++row->loopLimit;
        if (field == 5) ++row->completedSelections;
        if (field == 6) row->activeChild = 19;
        return before;
    }
    static void RestorePreparing(ContainerPlaybackTable& table,
        const PreparedContainerPlaybackRow& cap, const ContainerPlaybackRow& original) {
        table.rows_[cap.slot_] = original;
    }
};
struct PlayableInstancePreparationTestAccess {
    static auto CorruptPreparing(PlayableInstanceTable& table,
        const PreparedPlayableInstance& cap, unsigned field) {
        auto* entry = table.FindPrepared(cap);
        const auto original = *entry;
        switch (field) {
        case 0: entry->kind = PlayableKind::Segment; break;
        case 1: ++entry->parent; break;
        case 2: ++entry->scheduleEpoch; break;
        case 3: entry->lifecycle = entry->lifecycle == PlayableLifecycle::Free
            ? PlayableLifecycle::Retired : PlayableLifecycle::Free; break;
        case 4: ++entry->rootMetadata.projectId.low; break;
        case 5: entry->rootMetadata.definitionKind = PlayableKind::Segment; break;
        case 6: ++entry->rootMetadata.definitionStableId.low; break;
        case 7: ++entry->rootMetadata.programSemanticFingerprint; break;
        case 8: ++entry->rootMetadata.invocationEmitter; break;
        case 9: ++entry->rootMetadata.actionFiring; break;
        case 10: ++entry->rootMetadata.actionExecutionOrdinal; break;
        case 11: ++entry->rootMetadata.worldGeneration; break;
        case 12: entry->sourceRetirementHold = !entry->sourceRetirementHold; break;
        case 13: entry->firstChild = 19; break;
        case 14: entry->nextSibling = 19; break;
        case 15: entry->terminalOutcome.kind = PlayableTerminalKind::NaturalCompletion; break;
        case 16: entry->cancellationPrepared = true; break;
        case 17: entry->preparedObserver.actionFiring = 23; break;
        case 18: entry->retirementObserver.actionExecutionOrdinal = 23; break;
        case 19: entry->delayedCancellationEpoch = 23; break;
        }
        return original;
    }
    static void RestorePreparing(PlayableInstanceTable& table,
        const PreparedPlayableInstance& cap, const PlayableInstanceTable::Entry& original) {
        table.m_entries[PlayableInstanceTable::SlotOf(cap.Handle())] = original;
    }
};
}

using harken::PlayableInstanceHandle;
using harken::PlayableInstanceTable;
using harken::PlayableKind;
using harken::PlayableLifecycle;
using harken::PlayableTerminalKind;
using harken::PlayableTerminalOutcome;

namespace {
static_assert(!std::is_copy_constructible_v<harken::ContainerPlaybackTable>);
static_assert(!std::is_move_constructible_v<harken::ContainerPlaybackTable>);
static_assert(!std::is_copy_constructible_v<harken::ContainerPlaybackTable::PreparedCommitProof>);
static_assert(!std::is_copy_constructible_v<PlayableInstanceTable>);
static_assert(!std::is_move_constructible_v<PlayableInstanceTable>);
static_assert(!std::is_copy_constructible_v<PlayableInstanceTable::PreparedCommitProof>);
static_assert(!std::is_copy_constructible_v<PlayableInstanceTable::PreparedParentLinksProof>);
static_assert(!std::is_copy_assignable_v<PlayableInstanceTable::PreparedParentLinksProof>);
static_assert(std::is_nothrow_move_constructible_v<PlayableInstanceTable::PreparedParentLinksProof>);
static_assert(std::is_nothrow_move_assignable_v<PlayableInstanceTable::PreparedParentLinksProof>);
static_assert(std::is_copy_constructible_v<harken::PreparedPlayableInstance>);

static_assert(!std::is_copy_constructible_v<harken::DelayedPlayableCancellation>);
static_assert(!std::is_copy_assignable_v<harken::DelayedPlayableCancellation>);
static_assert(std::is_nothrow_move_constructible_v<harken::DelayedPlayableCancellation>);
static_assert(std::is_nothrow_move_assignable_v<harken::DelayedPlayableCancellation>);

PlayableInstanceHandle AcquireActionRoot(PlayableInstanceTable& table) {
    harken::PreparedPlayableInstance prepared;
    if (!table.PrepareAcquire(PlayableKind::Container, 0, &prepared)) return 0;
    harken::PlayableRootMetadata metadata{};
    metadata.definitionKind = PlayableKind::Container;
    metadata.actionFiring = 41;
    if (!table.SetPreparedRootMetadata(prepared, metadata)) return 0;
    const auto handle = prepared.Handle();
    return table.CommitPrepared(&prepared) ? handle : 0;
}

// Drives an instance through every stage up to and including Retired.
void RetireFully(PlayableInstanceTable& table, PlayableInstanceHandle handle)
{
    table.AdvanceLifecycle(handle, PlayableLifecycle::LogicalComplete);
    table.AdvanceLifecycle(handle, PlayableLifecycle::AudibleRetiring);
    table.AdvanceLifecycle(handle, PlayableLifecycle::Retired);
}

struct FailingOwnerProbe {
    bool committed = false;
    bool cancelled = false;
};

struct AuthorityOwnerProbe {
    bool valid = true;
};

bool ValidateAuthorityOwner(void* opaque) noexcept {
    return opaque != nullptr &&
           static_cast<AuthorityOwnerProbe*>(opaque)->valid;
}

bool CommitAuthorityOwner(void*, std::uint64_t) noexcept { return true; }
bool CancelAuthorityOwner(void*) noexcept { return true; }

struct BlockingOwnerProbe {
    PlayableInstanceTable* table = nullptr;
    PlayableInstanceHandle parent = 0;
    PlayableInstanceHandle child = 0;
    bool cancelled = false;
};

struct RetryingOwnerProbe {
    PlayableInstanceTable* table = nullptr;
    PlayableInstanceHandle parent = 0;
    PlayableInstanceHandle child = 0;
    bool cancelled = false;
};

struct RetainedOwnerProbe {
    std::uint32_t cancelFailuresRemaining = 0;
    std::uint32_t cancelAttempts = 0;
    bool failCommit = false;
};

struct RetentionContractOwner {
    std::uint32_t id = 0;
    std::vector<std::uint32_t>* cancellationOrder = nullptr;
    std::uint32_t commits = 0;
    std::uint32_t cancels = 0;
    bool failCommit = false;
    bool blockedCancel = false;
};

bool ValidateRetentionContractOwner(void* context) noexcept { return context != nullptr; }
bool CommitRetentionContractOwner(void* context, std::uint64_t) noexcept {
    auto& owner = *static_cast<RetentionContractOwner*>(context);
    ++owner.commits;
    return !owner.failCommit;
}
bool CancelRetentionContractOwner(void* context) noexcept {
    auto& owner = *static_cast<RetentionContractOwner*>(context);
    ++owner.cancels;
    if (owner.cancellationOrder != nullptr) owner.cancellationOrder->push_back(owner.id);
    return !owner.blockedCancel;
}

class RetentionExecutorTest : public testing::TestWithParam<bool> {
protected:
    void SetUp() override {
        if (GetParam()) {
            ASSERT_TRUE(instances.PrepareAcquire(PlayableKind::Container, 0, &external));
            root = external.Handle();
            ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &external));
        } else {
            ASSERT_TRUE(executor.PrepareInstance(instances, PlayableKind::Container));
            root = executor.PreparedInstanceHandle(0);
        }
        ASSERT_TRUE(executor.PrepareInstance(instances, PlayableKind::Sound, root));
        ASSERT_TRUE(executor.EnablePreparedOwnerRollbackRetention());
    }
    bool Commit() { return GetParam() ? executor.Commit(instances, 7) : executor.Commit(7); }
    bool Adopt(RetentionContractOwner& owner) {
        return executor.AdoptPreparedOwnerToken(&owner, ValidateRetentionContractOwner,
            CommitRetentionContractOwner, CancelRetentionContractOwner);
    }
    PlayableInstanceTable instances{4};
    harken::PlayableTreeExecutor executor{2};
    harken::PreparedPlayableInstance external{};
    PlayableInstanceHandle root = 0;
};

struct CommitWindowFailureProbe {
    PlayableInstanceTable* table = nullptr;
    harken::PreparedPlayableInstance* internal = nullptr;
    PlayableInstanceHandle root = 0;
    harken::PreparedPlayableInstance blocker{};
    bool mutated = false;
};

bool ValidateFailingOwner(void* opaque) noexcept {
    return opaque != nullptr;
}

bool CommitFailingOwner(void* opaque, std::uint64_t) noexcept {
    auto* probe = static_cast<FailingOwnerProbe*>(opaque);
    probe->committed = true;
    return false;
}

bool CancelFailingOwner(void* opaque) noexcept {
    auto* probe = static_cast<FailingOwnerProbe*>(opaque);
    probe->cancelled = true;
    return true;
}

bool ValidateBlockingOwner(void* opaque) noexcept {
    const auto* probe = static_cast<const BlockingOwnerProbe*>(opaque);
    return probe != nullptr && probe->table != nullptr && probe->parent != 0;
}

bool CommitBlockingOwner(void* opaque, std::uint64_t) noexcept {
    auto* probe = static_cast<BlockingOwnerProbe*>(opaque);
    probe->child = probe->table->Acquire(PlayableKind::Sound, probe->parent);
    return false;
}

bool CancelBlockingOwner(void* opaque) noexcept {
    auto* probe = static_cast<BlockingOwnerProbe*>(opaque);
    probe->cancelled = true;
    return true;
}

bool ValidateRetryingOwner(void* opaque) noexcept {
    const auto* probe = static_cast<const RetryingOwnerProbe*>(opaque);
    return probe != nullptr && probe->table != nullptr && probe->parent != 0;
}

bool CommitRetryingOwner(void* opaque, std::uint64_t) noexcept {
    auto* probe = static_cast<RetryingOwnerProbe*>(opaque);
    probe->child = probe->table->Acquire(PlayableKind::Sound, probe->parent);
    return false;
}

bool CancelRetryingOwner(void* opaque) noexcept {
    auto* probe = static_cast<RetryingOwnerProbe*>(opaque);
    if (probe->child != 0 && probe->table->Classify(probe->child).live) {
        return false;
    }
    probe->cancelled = true;
    return true;
}

bool ValidateRetainedOwner(void* opaque) noexcept {
    return opaque != nullptr;
}

bool CommitRetainedOwner(void* opaque, std::uint64_t) noexcept {
    const auto* probe = static_cast<const RetainedOwnerProbe*>(opaque);
    return !probe->failCommit;
}

bool CancelRetainedOwner(void* opaque) noexcept {
    auto* probe = static_cast<RetainedOwnerProbe*>(opaque);
    ++probe->cancelAttempts;
    if (probe->cancelFailuresRemaining == 0) return true;
    --probe->cancelFailuresRemaining;
    return false;
}

bool ValidateCommitWindowFailure(void* opaque) noexcept {
    auto* probe = static_cast<CommitWindowFailureProbe*>(opaque);
    if (probe == nullptr || probe->table == nullptr ||
        probe->internal == nullptr || probe->root == 0 || probe->mutated) {
        return false;
    }
    if (!probe->table->PrepareAcquire(
            PlayableKind::Sound, probe->root, &probe->blocker)) {
        return false;
    }
    if (!probe->table->CommitPrepared(&probe->blocker)) {
        (void)probe->table->CancelPrepared(&probe->blocker);
        return false;
    }
    if (!probe->table->CancelPrepared(probe->internal)) {
        return false;
    }
    probe->mutated = true;
    return true;
}

bool CommitCommitWindowFailure(void*, std::uint64_t) noexcept {
    return true;
}

bool CancelCommitWindowFailure(void*) noexcept {
    return true;
}

} // namespace

TEST(PlayableInstanceTable, WithdrawUnpublishedParentLinkPreservesIdentityAndSiblings) {
    PlayableInstanceTable table(6);
    const auto parent = table.Acquire(PlayableKind::Container, 0);
    const auto parentEpoch = table.Classify(parent).scheduleEpoch;
    const auto before = table.Acquire(PlayableKind::Sound, parent);
    harken::PreparedPlayableInstance privateChild{};
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, parent, &privateChild));
    const auto child = privateChild.Handle();
    ASSERT_TRUE(table.CommitPrepared(&privateChild));
    const auto after = table.Acquire(PlayableKind::Sound, parent);
    ASSERT_EQ(child, table.NextSibling(before));
    ASSERT_EQ(after, table.NextSibling(child));
    EXPECT_FALSE(table.WithdrawUnpublishedParentLink(privateChild, parent, parentEpoch + 1));
    PlayableInstanceTable foreign(6);
    EXPECT_FALSE(foreign.WithdrawUnpublishedParentLink(privateChild, parent, parentEpoch));
    EXPECT_EQ(child, table.NextSibling(before));
    EXPECT_EQ(after, table.NextSibling(child));
    ASSERT_TRUE(table.WithdrawUnpublishedParentLink(privateChild, parent, parentEpoch));
    EXPECT_EQ(before, table.Classify(parent).firstChild);
    EXPECT_EQ(after, table.NextSibling(before));
    EXPECT_EQ(0u, table.NextSibling(child));
    EXPECT_EQ(parent, table.Classify(child).parent);
    EXPECT_TRUE(table.Classify(child).live);
    EXPECT_TRUE(table.WithdrawUnpublishedParentLink(privateChild, parent, parentEpoch));
    const auto oldToken = privateChild;
    ASSERT_TRUE(table.RollbackCommitted(child));
    ASSERT_TRUE(table.CancelPrepared(&privateChild));
    EXPECT_FALSE(table.WithdrawUnpublishedParentLink(oldToken, parent, parentEpoch));
    EXPECT_EQ(after, table.NextSibling(before));
}

TEST(PlayableInstanceTable, StartedObservationIsOncePerCommittedSoundGeneration) {
    PlayableInstanceTable table(1);
    EXPECT_FALSE(table.HasPendingStartedObservations());
    harken::PreparedPlayableInstance prepared;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, 0, &prepared));
    const auto first = prepared.Handle();
    EXPECT_FALSE(table.NeedsStartedObservation(first));
    EXPECT_FALSE(table.HasPendingStartedObservations());
    table.ObserveStarted(first, 5, 256);
    ASSERT_TRUE(table.CommitPrepared(&prepared));
    EXPECT_TRUE(table.NeedsStartedObservation(first));
    EXPECT_TRUE(table.HasPendingStartedObservations());
    table.ObserveStarted(first, 0, 256);
    EXPECT_TRUE(table.NeedsStartedObservation(first));
    EXPECT_TRUE(table.HasPendingStartedObservations());
    table.ObserveStarted(first, 5, 256);
    EXPECT_FALSE(table.NeedsStartedObservation(first));
    EXPECT_FALSE(table.HasPendingStartedObservations());
    table.ObserveStarted(first, 6, 512);
    EXPECT_FALSE(table.NeedsStartedObservation(first));
    EXPECT_FALSE(table.HasPendingStartedObservations());
    RetireFully(table, first);
    EXPECT_FALSE(table.NeedsStartedObservation(first));
    EXPECT_FALSE(table.HasPendingStartedObservations());
    const auto reused = table.Acquire(PlayableKind::Sound, 0);
    ASSERT_NE(0u, reused);
    EXPECT_EQ(static_cast<std::uint32_t>(first), static_cast<std::uint32_t>(reused));
    EXPECT_NE(first, reused);
    EXPECT_FALSE(table.NeedsStartedObservation(first));
    EXPECT_TRUE(table.NeedsStartedObservation(reused));
    table.ObserveStarted(first, 7, 768);
    EXPECT_TRUE(table.HasPendingStartedObservations());
    RetireFully(table, reused);
    EXPECT_FALSE(table.HasPendingStartedObservations());
    const auto container = table.Acquire(PlayableKind::Container, 0);
    ASSERT_NE(0u, container);
    EXPECT_FALSE(table.NeedsStartedObservation(container));
    EXPECT_FALSE(table.HasPendingStartedObservations());
    RetireFully(table, container);
}

TEST(PlayableInstanceTable, PreparedProvenanceIsNotPublishedAndClearsOnRollbackReuse) {
    PlayableInstanceTable table(1);
    harken::PreparedPlayableInstance prepared;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, 0, &prepared));
    harken::MonitorPlayableProvenance provenance{};
    provenance.knownFlags = 1;
    provenance.rootPlayableId = harkentest::ObjectId(77);
    provenance.mediaChecksum = 123;
    ASSERT_TRUE(table.SetPreparedProvenance(prepared, provenance));
    harken::MonitorLifecycleFacts facts{};
    EXPECT_EQ(0u, table.CaptureMonitorIdentityAt(0, &facts));
    EXPECT_FALSE(table.NeedsStartedObservation(prepared.Handle()));
    const auto stale = prepared;
    ASSERT_TRUE(table.CancelPrepared(&prepared));
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, 0, &prepared));
    EXPECT_FALSE(table.SetPreparedProvenance(stale, provenance));
    ASSERT_TRUE(table.CommitPrepared(&prepared));
    ASSERT_TRUE(table.CaptureMonitorFacts(prepared.Handle(), &facts));
    EXPECT_EQ(0u, facts.playableProvenance.knownFlags);
    ASSERT_TRUE(table.RollbackCommitted(prepared.Handle()));
    ASSERT_TRUE(table.SetPreparedProvenance(prepared, provenance));
    ASSERT_TRUE(table.CommitPrepared(&prepared));
    ASSERT_TRUE(table.CaptureMonitorFacts(prepared.Handle(), &facts));
    EXPECT_EQ(123u, facts.playableProvenance.mediaChecksum);
    RetireFully(table, prepared.Handle());
    const auto next = table.Acquire(PlayableKind::Sound, 0);
    ASSERT_TRUE(table.CaptureMonitorFacts(next, &facts));
    EXPECT_EQ(0u, facts.playableProvenance.knownFlags);
    RetireFully(table, next);
}

TEST(PlayableInstanceTable, PendingStartedObservationsFollowActualRollbackAndRecommit) {
    PlayableInstanceTable table(3);
    harken::PreparedPlayableInstance prepared;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, 0, &prepared));
    const auto direct = table.Acquire(PlayableKind::Sound, 0);
    ASSERT_NE(0u, direct);
    ASSERT_TRUE(table.CommitPrepared(&prepared));
    const auto handle = prepared.Handle();
    table.ObserveStarted(direct, 1, 0);
    EXPECT_TRUE(table.HasPendingStartedObservations());
    ASSERT_TRUE(table.AcquireSourceRetirementHold(handle, prepared.ScheduleEpoch()));
    EXPECT_FALSE(table.RollbackCommitted(handle));
    EXPECT_FALSE(table.AdvanceLifecycle(handle, PlayableLifecycle::Retired));
    EXPECT_TRUE(table.HasPendingStartedObservations());
    ASSERT_TRUE(table.ReleaseSourceRetirementHold(handle));
    ASSERT_TRUE(table.RollbackCommitted(handle));
    EXPECT_FALSE(table.HasPendingStartedObservations());
    ASSERT_TRUE(table.CommitPrepared(&prepared));
    EXPECT_TRUE(table.HasPendingStartedObservations());
    table.ObserveStarted(handle, 2, 256);
    EXPECT_FALSE(table.HasPendingStartedObservations());
    ASSERT_TRUE(table.RollbackCommitted(handle));
    EXPECT_FALSE(table.HasPendingStartedObservations());
    ASSERT_TRUE(table.CommitPrepared(&prepared));
    EXPECT_FALSE(table.HasPendingStartedObservations());
    RetireFully(table, handle);

    harken::PreparedPlayableInstance cancelled;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, 0, &cancelled));
    auto stale = cancelled;
    ASSERT_TRUE(table.CancelPrepared(&cancelled));
    const auto next = table.Acquire(PlayableKind::Sound, 0);
    ASSERT_NE(0u, next);
    EXPECT_FALSE(table.CancelPrepared(&stale));
    table.ObserveStarted(handle, 3, 512);
    EXPECT_TRUE(table.HasPendingStartedObservations());
    RetireFully(table, next);
    EXPECT_FALSE(table.HasPendingStartedObservations());
    RetireFully(table, direct);
    EXPECT_FALSE(table.HasPendingStartedObservations());
}

TEST(PlayableInstanceTable, AcquiredInstanceReportsItsKindAndParent)
{
    PlayableInstanceTable table(4);
    const PlayableInstanceHandle segment =
        table.Acquire(PlayableKind::Segment, 0);
    ASSERT_NE(segment, 0u);
    const PlayableInstanceHandle track =
        table.Acquire(PlayableKind::Track, segment);
    ASSERT_NE(track, 0u);

    const auto view = table.Classify(track);
    EXPECT_TRUE(view.live);
    EXPECT_EQ(view.kind, PlayableKind::Track);
    EXPECT_EQ(view.parent, segment);
    EXPECT_EQ(view.lifecycle, PlayableLifecycle::Active);
    // A live instance always carries a stamp, so an unstamped record can never
    // be mistaken for a current one.
    EXPECT_NE(view.scheduleEpoch, 0u);
}

TEST(PlayableInstanceTable,
     CancelledTokenCopyCannotCommitOrCancelSameSlotReprepare)
{
    PlayableInstanceTable table(1);
    harken::PreparedPlayableInstance first{};
    ASSERT_TRUE(table.PrepareAcquire(
        harken::PlayableKind::Sound, 0, &first));
    harken::PreparedPlayableInstance stale = first;
    ASSERT_TRUE(table.CancelPrepared(&first));

    harken::PreparedPlayableInstance current{};
    ASSERT_TRUE(table.PrepareAcquire(
        harken::PlayableKind::Container, 0, &current));
    ASSERT_EQ(static_cast<std::uint32_t>(stale.Handle()),
              static_cast<std::uint32_t>(current.Handle()));
    ASSERT_EQ((stale.Handle() >> 32) + 1u, current.Handle() >> 32);
    ASSERT_NE(stale.Handle(), current.Handle());
    ASSERT_NE(stale.ScheduleEpoch(), current.ScheduleEpoch());

    EXPECT_FALSE(stale.Valid());
    EXPECT_FALSE(table.CommitPrepared(&stale, false));
    EXPECT_TRUE(current.Valid());
    EXPECT_FALSE(table.CancelPrepared(&stale));
    EXPECT_TRUE(current.Valid());

    EXPECT_TRUE(table.CommitPrepared(&current, false));
    EXPECT_TRUE(table.Classify(current.Handle()).live);
    EXPECT_EQ(harken::PlayableKind::Container,
              table.Classify(current.Handle()).kind);
    EXPECT_TRUE(table.RollbackCommitted(current.Handle()));
    EXPECT_TRUE(table.CancelPrepared(&current));
    EXPECT_EQ(table.Capacity(), table.FreeCount());
}

TEST(PlayableTreeExecutor,
     RejectsCancelledTokenCopyForSameSlotReprepare)
{
    PlayableInstanceTable table(1);
    harken::PreparedPlayableInstance first{};
    ASSERT_TRUE(table.PrepareAcquire(
        harken::PlayableKind::Sound, 0, &first));
    harken::PreparedPlayableInstance stale = first;
    ASSERT_TRUE(table.CancelPrepared(&first));

    harken::PreparedPlayableInstance current{};
    ASSERT_TRUE(table.PrepareAcquire(
        harken::PlayableKind::Container, 0, &current));
    ASSERT_EQ(static_cast<std::uint32_t>(stale.Handle()),
              static_cast<std::uint32_t>(current.Handle()));
    ASSERT_EQ((stale.Handle() >> 32) + 1u, current.Handle() >> 32);
    ASSERT_NE(stale.Handle(), current.Handle());
    ASSERT_NE(stale.ScheduleEpoch(), current.ScheduleEpoch());

    harken::PlayableTreeExecutor executor(1);
    EXPECT_FALSE(executor.AdoptPreparedInstance(table, &stale));
    EXPECT_TRUE(current.Valid());
    EXPECT_TRUE(executor.AdoptPreparedInstance(table, &current));
    EXPECT_TRUE(executor.Cancel(table));
    EXPECT_EQ(table.Capacity(), table.FreeCount());
}

TEST(ContainerPlaybackTable, PreparedCommitHoldProtectsLaterSlotAndStableOriginalCap)
{
    harken::PlayableInstanceTable instances(3);
    const auto first = instances.Acquire(harken::PlayableKind::Container, 0);
    const auto heldInstance = instances.Acquire(harken::PlayableKind::Container, 0);
    const auto extra = instances.Acquire(harken::PlayableKind::Container, 0);
    ASSERT_NE(0u, first);
    ASSERT_NE(0u, heldInstance);
    ASSERT_NE(0u, extra);
    harken::ContainerPlaybackTable table(2);
    harken::PreparedContainerPlaybackRow filler, original, other;
    ASSERT_TRUE(table.Prepare(instances, first, instances.Classify(first).scheduleEpoch,
        harkentest::ObjectId(77), 99, 2, 1, &filler));
    ASSERT_TRUE(table.Prepare(instances, heldInstance, instances.Classify(heldInstance).scheduleEpoch,
        harkentest::ObjectId(78), 100, 2, 2, &original));
    ASSERT_TRUE(table.Cancel(&filler));
    harken::ContainerPlaybackTable::PreparedCommitProof proof;
    ASSERT_TRUE(table.ProvePreparedCommit(original, &proof));
    EXPECT_TRUE(original.Valid());
    EXPECT_TRUE(table.PreparedCommitHeld(original));
    EXPECT_FALSE(table.Commit(&original));
    EXPECT_FALSE(table.Cancel(&original));
    EXPECT_FALSE(table.Prepare(instances, heldInstance, instances.Classify(heldInstance).scheduleEpoch,
        harkentest::ObjectId(78), 100, 2, 2, &other));
    EXPECT_FALSE(table.Prepare(instances, extra, instances.Classify(extra).scheduleEpoch,
        harkentest::ObjectId(79), 101, 2, 1, &original));
    ASSERT_TRUE(table.Prepare(instances, extra, instances.Classify(extra).scheduleEpoch,
        harkentest::ObjectId(79), 101, 2, 1, &other));
    EXPECT_EQ(0u, table.FreeCount());
    EXPECT_EQ(0u, table.LiveCount());
    harken::PreparedContainerPlaybackRow denied(std::move(original));
    EXPECT_FALSE(denied.Valid());
    other = std::move(original);
    original = std::move(other);
    EXPECT_EQ(heldInstance, original.Instance());
    EXPECT_EQ(extra, other.Instance());
    EXPECT_TRUE(proof.Associated(&table, &original));
    harken::ContainerPlaybackRow snapshot{};
    snapshot.live = true;
    snapshot.instance = heldInstance;
    EXPECT_FALSE(table.Restore(snapshot));
    EXPECT_FALSE(table.SetActiveChild(heldInstance, first, 1));
    EXPECT_FALSE(table.MarkLogicalComplete(heldInstance));
    EXPECT_FALSE(table.Discard(heldInstance));
    EXPECT_FALSE(table.Capture(heldInstance, &snapshot));
    proof.Release();
    ASSERT_TRUE(table.ProvePreparedCommit(original, &proof));
    proof.Release();
    ASSERT_TRUE(table.Cancel(&original));
    ASSERT_TRUE(table.Cancel(&other));
    EXPECT_EQ(table.Capacity(), table.FreeCount());
}

TEST(ContainerPlaybackTable, PreparedCommitPreservesRollbackIdentityAndNeverReacquiresPreparedRights)
{
    for (const bool legacy : {false, true}) {
        harken::PlayableInstanceTable instances(1);
        const auto instance = instances.Acquire(harken::PlayableKind::Container, 0);
        ASSERT_NE(0u, instance);
        const auto epoch = instances.Classify(instance).scheduleEpoch;
        harken::ContainerPlaybackTable table(1);
        harken::PreparedContainerPlaybackRow original;
        ASSERT_TRUE(table.Prepare(instances, instance, epoch, harkentest::ObjectId(77), 99, 2, 2, &original));
        if (legacy) ASSERT_TRUE(table.Commit(&original));
        else {
            harken::ContainerPlaybackTable::PreparedCommitProof proof;
            ASSERT_TRUE(table.ProvePreparedCommit(original, &proof));
            harken::ContainerPlaybackTable::ConsumePreparedCommit(std::move(proof));
            EXPECT_FALSE(proof.Valid());
        }
        EXPECT_FALSE(original.Valid());
        EXPECT_EQ(&instances, original.InstanceOwner());
        EXPECT_EQ(instance, original.Instance());
        EXPECT_EQ(epoch, original.ExpectedScheduleEpoch());
        EXPECT_EQ(1u, table.LiveCount());
        ASSERT_TRUE(harken::detail::ContainerPlaybackTableTestAccess::RollbackOwned(table, original));
        EXPECT_EQ(0u, table.LiveCount());
        harken::PreparedContainerPlaybackRow fresh;
        ASSERT_TRUE(table.Prepare(instances, instance, epoch, harkentest::ObjectId(77), 99, 2, 2, &fresh));
        EXPECT_FALSE(original.Valid());
        EXPECT_FALSE(table.Cancel(&original));
        EXPECT_FALSE(table.Commit(&original));
        harken::ContainerPlaybackTable::PreparedCommitProof staleProof, freshProof;
        EXPECT_FALSE(table.ProvePreparedCommit(original, &staleProof));
        EXPECT_TRUE(fresh.Valid());
        ASSERT_TRUE(table.ProvePreparedCommit(fresh, &freshProof));
        harken::ContainerPlaybackTable::ConsumePreparedCommit(std::move(freshProof));
        harken::ContainerPlaybackRow snapshot{};
        ASSERT_TRUE(table.Capture(instance, &snapshot));
        EXPECT_EQ(harken::ContainerPlaybackPhase::Active, snapshot.phase);
        EXPECT_EQ(2u, snapshot.loopLimit);
        ASSERT_TRUE(table.Discard(instance));
        EXPECT_EQ(table.Capacity(), table.FreeCount());
    }
}

TEST(ContainerPlaybackTable, PreparedCommitRejectsChangedOriginalInputAndRestoresPrefix)
{
    harken::ContainerPlaybackTable table(2), foreign(1);
    harken::PreparedContainerPlaybackRow original, second;
    ASSERT_TRUE(table.Prepare(11, 13, harkentest::ObjectId(77), 99, 2, 2, &original));
    ASSERT_TRUE(table.Prepare(17, 19, harkentest::ObjectId(78), 100, 2, 1, &second));
    for (unsigned field = 0; field < 7; ++field) {
        const auto unchanged = harken::detail::ContainerPlaybackTableTestAccess::CorruptPreparing(table, original, field);
        harken::ContainerPlaybackTable::PreparedCommitProof proof;
        EXPECT_FALSE(table.ProvePreparedCommit(original, &proof));
        EXPECT_FALSE(proof.Valid());
        harken::detail::ContainerPlaybackTableTestAccess::RestorePreparing(table, original, unchanged);
    }
    harken::ContainerPlaybackTable::PreparedCommitProof firstProof, secondProof;
    ASSERT_TRUE(table.ProvePreparedCommit(original, &firstProof));
    EXPECT_FALSE(foreign.ProvePreparedCommit(second, &secondProof));
    EXPECT_FALSE(secondProof.Valid());
    ASSERT_TRUE(table.ProvePreparedCommit(second, &secondProof));
    firstProof = std::move(secondProof);
    ASSERT_TRUE(table.Cancel(&original));
    EXPECT_FALSE(table.Cancel(&second));
    harken::ContainerPlaybackTable::ConsumePreparedCommit(std::move(firstProof));
    ASSERT_TRUE(table.Discard(second.Instance()));
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(table.Capacity(), table.FreeCount());
}

TEST(ContainerPlaybackTable, ReservesExactCapacityAndRollsBackOneShort)
{
    harken::PlayableInstanceTable instances(2);
    harken::ContainerPlaybackTable containers(1);
    harken::PreparedPlayableTreeStep step(2);
    ASSERT_TRUE(step.PrepareInstance(
        instances, harken::PlayableKind::Container));
    ASSERT_TRUE(step.PrepareInstance(
        instances, harken::PlayableKind::Sound,
        step.PreparedInstanceHandle(0)));
    EXPECT_EQ(instances.FreeCount(), 0u);
    EXPECT_FALSE(step.PrepareInstance(instances, harken::PlayableKind::Sound));
    step.Cancel();
    EXPECT_EQ(instances.FreeCount(), instances.Capacity());
    const auto root = instances.Acquire(harken::PlayableKind::Container, 0);
    ASSERT_NE(root, 0u);
    harken::PreparedContainerPlaybackRow row;
    ASSERT_TRUE(containers.Prepare(instances, root, instances.Classify(root).scheduleEpoch,
                                   harkentest::ObjectId(77u), 99u, 2u, 0u, &row));
    ASSERT_TRUE(containers.Commit(&row));
    EXPECT_EQ(containers.FreeCount(), 0u);
    EXPECT_FALSE(containers.Prepare(instances, root, instances.Classify(root).scheduleEpoch,
                                    harkentest::ObjectId(77u), 99u, 2u, 0u, &row));
    EXPECT_TRUE(containers.Discard(root));
    RetireFully(instances, root);
    EXPECT_EQ(containers.FreeCount(), containers.Capacity());
}

TEST(ContainerPlaybackTable, ChildOutcomeRequiresExactHandleAndEpoch)
{
    harken::ContainerPlaybackTable table(1);
    const auto parent = static_cast<harken::PlayableInstanceHandle>(0x100000001ull);
    const auto child = static_cast<harken::PlayableInstanceHandle>(0x100000002ull);
    harken::PreparedContainerPlaybackRow prepared;
    ASSERT_TRUE(table.Prepare(parent, 11u, harkentest::ObjectId(77u), 99u, 2u, 2u, &prepared));
    ASSERT_TRUE(table.Commit(&prepared));
    ASSERT_TRUE(table.SetActiveChild(parent, child, 23u));
    EXPECT_EQ(table.ConsumeChildOutcome(parent, child, 24u, 1u),
              harken::ContainerPlaybackTable::ChildOutcomeStatus::Mismatch);
    EXPECT_EQ(table.ConsumeChildOutcome(parent, child, 23u, 1u),
              harken::ContainerPlaybackTable::ChildOutcomeStatus::Accepted);
    EXPECT_EQ(table.ConsumeChildOutcome(parent, child, 23u, 1u),
              harken::ContainerPlaybackTable::ChildOutcomeStatus::Duplicate);
}

TEST(ContainerPlaybackTable, RejectsDuplicatePreparedRowsForOneInstance)
{
    harken::ContainerPlaybackTable table(2);
    const auto instance = static_cast<harken::PlayableInstanceHandle>(
        0x100000001ull);
    harken::PreparedContainerPlaybackRow first;
    harken::PreparedContainerPlaybackRow second;
    ASSERT_TRUE(table.Prepare(instance, 11u, harkentest::ObjectId(77u), 99u, 2u, 1u, &first));
    EXPECT_FALSE(table.Prepare(instance, 12u, harkentest::ObjectId(78u), 100u, 2u, 1u, &second));
    EXPECT_TRUE(table.Cancel(&first));
    EXPECT_EQ(table.FreeCount(), table.Capacity());
}

TEST(ContainerPlaybackTable, AudibleRetiringRowsContinueToConsumeCapacity)
{
    harken::PlayableInstanceTable instances(2);
    harken::ContainerPlaybackTable containers(2);
    const auto parent = instances.Acquire(harken::PlayableKind::Container, 0);
    const auto child = instances.Acquire(harken::PlayableKind::Sound, parent);
    ASSERT_NE(parent, 0u);
    ASSERT_NE(child, 0u);
    harken::PreparedContainerPlaybackRow parentRow;
    ASSERT_TRUE(containers.Prepare(instances,
        parent, instances.Classify(parent).scheduleEpoch, harkentest::ObjectId(77u), 99u, 2u, 1u,
        &parentRow));
    ASSERT_TRUE(containers.Commit(&parentRow));
    ASSERT_TRUE(containers.MarkLogicalComplete(parent));
    ASSERT_TRUE(containers.MarkAudibleRetiring(parent));
    EXPECT_EQ(containers.FreeCount(), 1u);
    harken::PreparedContainerPlaybackRow second;
    const auto other = static_cast<harken::PlayableInstanceHandle>(0x100000003ull);
    ASSERT_TRUE(containers.Prepare(instances,
        other, 12u, harkentest::ObjectId(78u), 100u, 2u, 1u,
        &second));
    EXPECT_EQ(containers.FreeCount(), 0u);
    EXPECT_FALSE(containers.Prepare(instances,
        child, instances.Classify(child).scheduleEpoch, harkentest::ObjectId(79u), 101u, 2u, 1u,
        &second));
    EXPECT_TRUE(containers.Cancel(&second));
    EXPECT_TRUE(containers.Discard(parent));
    RetireFully(instances, child);
    RetireFully(instances, parent);
}

TEST(PlayableTreeExecutor, PreparedLookupIncludesAdoptedReservationAndExcludesUnownedOrReturnedHandles)
{
    PlayableInstanceTable instances(4);
    harken::PreparedPlayableInstance root, adopted, unowned;
    ASSERT_TRUE(instances.PrepareAcquire(PlayableKind::Container, 0, &root));
    harken::PlayableTreeExecutor executor(3);
    ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &root));
    ASSERT_TRUE(executor.PrepareInstance(instances, PlayableKind::Sound, root.Handle()));
    const auto privateHandle = executor.PreparedInstanceHandle(0);
    ASSERT_TRUE(instances.PrepareAcquire(PlayableKind::Sound, root.Handle(), &adopted));
    const auto adoptedHandle = adopted.Handle();
    ASSERT_TRUE(executor.AdoptPreparedInstanceReservation(&adopted));
    ASSERT_TRUE(instances.PrepareAcquire(PlayableKind::Sound, root.Handle(), &unowned));

    const auto* privateToken = executor.FindPreparedInstanceToken(privateHandle);
    ASSERT_NE(nullptr, privateToken);
    EXPECT_TRUE(privateToken->Valid());
    EXPECT_EQ(root.Handle(), privateToken->Parent());
    EXPECT_EQ(&adopted, executor.FindPreparedInstanceToken(adoptedHandle));
    EXPECT_EQ(nullptr, executor.FindPreparedInstanceToken(unowned.Handle()));
    EXPECT_EQ(nullptr, executor.FindPreparedInstanceToken(0));
    EXPECT_EQ(nullptr, executor.FindPreparedInstanceToken(root.Handle()));
    EXPECT_EQ(1u, executor.InstanceReservationCount());
    EXPECT_EQ(0u, instances.FreeCount());

    ASSERT_TRUE(instances.CancelPrepared(&unowned));
    ASSERT_TRUE(executor.Cancel(instances));
    EXPECT_EQ(nullptr, executor.FindPreparedInstanceToken(privateHandle));
    EXPECT_EQ(nullptr, executor.FindPreparedInstanceToken(adoptedHandle));
    EXPECT_EQ(instances.Capacity(), instances.FreeCount());
}

TEST(PreparedPlayableTreeStep, ContainerProofConsumesInternalRowAfterInstanceActivation)
{
    PlayableInstanceTable instances(2);
    harken::ContainerPlaybackTable containers(1);
    harken::PreparedPlayableTreeStep step(2);
    ASSERT_TRUE(step.PrepareInstance(instances, PlayableKind::Container));
    const auto root = step.PreparedInstanceHandle(0);
    ASSERT_TRUE(step.PrepareInstance(instances, PlayableKind::Sound, root));
    const auto leaf = step.PreparedInstanceHandle(1);
    ASSERT_TRUE(step.PrepareContainer(instances, containers, root, step.PreparedInstanceEpoch(0),
        harkentest::ObjectId(77), 99, 2, 1));
    ASSERT_TRUE(step.ProveContainerCommits());
    EXPECT_TRUE(step.ContainerCommitProofsHeld());
    EXPECT_TRUE(step.Validate());
    EXPECT_FALSE(step.PrepareInstance(instances, PlayableKind::Sound));
    harken::PreparedPlayableTreeStep denied(std::move(step));
    EXPECT_EQ(0u, denied.InstanceReservationCount());
    EXPECT_EQ(2u, step.InstanceReservationCount());
    ASSERT_TRUE(step.Commit());
    EXPECT_FALSE(step.ContainerCommitProofsHeld());
    EXPECT_EQ(2u, instances.LiveCount());
    EXPECT_EQ(1u, containers.LiveCount());
    EXPECT_EQ(leaf, instances.FirstChild(root));
    ASSERT_TRUE(step.RollbackCommitted());
    EXPECT_EQ(instances.Capacity(), instances.FreeCount());
    EXPECT_EQ(containers.Capacity(), containers.FreeCount());
}

TEST(PlayableTreeExecutor, ContainerProofFreezesAdoptedRootAndBothWholeMoveEndpoints)
{
    PlayableInstanceTable instances(3);
    harken::ContainerPlaybackTable containers(1);
    harken::PreparedPlayableInstance root;
    ASSERT_TRUE(instances.PrepareAcquire(PlayableKind::Container, 0, &root));
    harken::PreparedContainerPlaybackRow row;
    ASSERT_TRUE(containers.Prepare(instances, root.Handle(), root.ScheduleEpoch(),
        harkentest::ObjectId(77), 99, 2, 1, &row));
    harken::PlayableTreeExecutor executor(2), other(1);
    ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &root));
    ASSERT_TRUE(executor.PrepareInstance(instances, PlayableKind::Sound, root.Handle()));
    const auto leaf = executor.PreparedInstanceHandle(0);
    ASSERT_TRUE(executor.AdoptPreparedContainerReservation(&row));
    ASSERT_TRUE(other.PrepareInstance(instances, PlayableKind::Sound));
    const auto unrelated = other.PreparedInstanceHandle(0);
    RetainedOwnerProbe owner{};
    ASSERT_TRUE(executor.AdoptPreparedOwnerToken(&owner, ValidateRetainedOwner,
        CommitRetainedOwner, CancelRetainedOwner));
    ASSERT_TRUE(executor.ProveContainerCommits(&containers, &row));
    EXPECT_FALSE(executor.ResetForReuse());
    EXPECT_FALSE(executor.SetPreparedRootMetadata(instances, {}));
    EXPECT_FALSE(executor.AdoptPreparedOwnerToken(&owner, ValidateRetainedOwner,
        CommitRetainedOwner, CancelRetainedOwner));
    EXPECT_FALSE(executor.AdoptPreparedContainerReservation(&row));
    harken::PlayableTreeExecutor denied(std::move(executor));
    EXPECT_EQ(0u, denied.InstanceReservationCount());
    EXPECT_EQ(1u, executor.OwnerTokenCount());
    other = std::move(executor);
    executor = std::move(other);
    EXPECT_EQ(unrelated, other.PreparedInstanceHandle(0));
    EXPECT_EQ(leaf, executor.PreparedInstanceHandle(0));
    EXPECT_TRUE(containers.PreparedCommitHeld(row));
    ASSERT_TRUE(executor.Commit(instances, 17));
    EXPECT_EQ(leaf, instances.FirstChild(root.Handle()));
    EXPECT_EQ(1u, containers.LiveCount());
    ASSERT_TRUE(executor.RollbackCommitted(instances));
    other.Cancel();
    EXPECT_EQ(instances.Capacity(), instances.FreeCount());
    EXPECT_EQ(containers.Capacity(), containers.FreeCount());
}

TEST(PlayableTreeExecutor, ContainerProofPrefixReleaseRequiresGenuineReproveAndNeverFallsBack)
{
    PlayableInstanceTable instances(2);
    harken::ContainerPlaybackTable containers(2), foreign(1);
    harken::PreparedPlayableInstance root, child;
    ASSERT_TRUE(instances.PrepareAcquire(PlayableKind::Container, 0, &root));
    ASSERT_TRUE(instances.PrepareAcquire(PlayableKind::Container, root.Handle(), &child));
    harken::PreparedContainerPlaybackRow rootRow, childRow, absent;
    ASSERT_TRUE(containers.Prepare(instances, root.Handle(), root.ScheduleEpoch(),
        harkentest::ObjectId(77), 99, 2, 1, &rootRow));
    ASSERT_TRUE(containers.Prepare(instances, child.Handle(), child.ScheduleEpoch(),
        harkentest::ObjectId(78), 100, 2, 1, &childRow));
    harken::PlayableTreeExecutor executor(2);
    ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &root));
    ASSERT_TRUE(executor.AdoptPreparedInstanceReservation(&child));
    ASSERT_TRUE(executor.AdoptPreparedContainerReservation(&rootRow));
    ASSERT_TRUE(executor.AdoptPreparedContainerReservation(&childRow));
    EXPECT_FALSE(executor.ProveContainerCommits(&containers, &absent));
    EXPECT_FALSE(executor.ProveContainerCommits(&foreign, &rootRow));
    const auto saved = harken::detail::ContainerPlaybackTableTestAccess::CorruptPreparing(containers, childRow, 2);
    EXPECT_FALSE(executor.ProveContainerCommits(&containers, &rootRow));
    EXPECT_FALSE(executor.ContainerCommitProofsHeld());
    EXPECT_FALSE(containers.PreparedCommitHeld(rootRow));
    EXPECT_TRUE(rootRow.Valid());
    EXPECT_TRUE(childRow.Valid());
    EXPECT_FALSE(executor.Validate());
    harken::detail::ContainerPlaybackTableTestAccess::RestorePreparing(containers, childRow, saved);
    ASSERT_TRUE(executor.ProveContainerCommits(&containers, &rootRow));
    ASSERT_TRUE(executor.Validate());
    executor.ReleaseContainerCommitProofs();
    EXPECT_FALSE(executor.ContainerCommitProofsHeld());
    EXPECT_FALSE(executor.Validate());
    EXPECT_FALSE(executor.Commit(instances, 19));
    EXPECT_EQ(0u, instances.LiveCount());
    EXPECT_EQ(0u, containers.LiveCount());
    ASSERT_TRUE(executor.ProveContainerCommits(&containers, &rootRow));
    ASSERT_TRUE(executor.Commit(instances, 19));
    ASSERT_TRUE(executor.RollbackCommitted(instances));
    EXPECT_EQ(instances.Capacity(), instances.FreeCount());
    EXPECT_EQ(containers.Capacity(), containers.FreeCount());
}

TEST(PlayableTreeExecutor, ContainerProofExternalRootRevocationReleasesBeforeStepEntry)
{
    PlayableInstanceTable instances(2);
    harken::ContainerPlaybackTable containers(1);
    harken::PreparedPlayableInstance root;
    ASSERT_TRUE(instances.PrepareAcquire(PlayableKind::Container, 0, &root));
    harken::PreparedContainerPlaybackRow row;
    ASSERT_TRUE(containers.Prepare(instances, root.Handle(), root.ScheduleEpoch(),
        harkentest::ObjectId(77), 99, 2, 1, &row));
    harken::PlayableTreeExecutor executor(2);
    ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &root));
    ASSERT_TRUE(executor.PrepareInstance(instances, PlayableKind::Sound, root.Handle()));
    ASSERT_TRUE(executor.AdoptPreparedContainerReservation(&row));
    ASSERT_TRUE(executor.ProveContainerCommits(&containers, &row));
    ASSERT_TRUE(instances.CancelPrepared(&root));
    EXPECT_FALSE(executor.Commit(instances, 23));
    EXPECT_FALSE(executor.ContainerCommitProofsHeld());
    EXPECT_TRUE(row.Valid());
    EXPECT_FALSE(containers.PreparedCommitHeld(row));
    EXPECT_EQ(0u, containers.LiveCount());
    ASSERT_TRUE(executor.Cancel(instances));
    EXPECT_EQ(instances.Capacity(), instances.FreeCount());
    EXPECT_EQ(containers.Capacity(), containers.FreeCount());
}

TEST(PlayableTreeExecutor, ContainerProofConsumedRowsWaitForActualSourceHoldBeforeRollback)
{
    PlayableInstanceTable instances(2);
    harken::ContainerPlaybackTable containers(1);
    harken::PlayableTreeExecutor executor(2);
    ASSERT_TRUE(executor.PrepareInstance(instances, PlayableKind::Container));
    const auto root = executor.PreparedInstanceHandle(0);
    ASSERT_TRUE(executor.PrepareInstance(instances, PlayableKind::Sound, root));
    const auto leaf = executor.PreparedInstanceHandle(1);
    ASSERT_TRUE(executor.PrepareContainer(instances, containers, root, executor.PreparedInstanceEpoch(0),
        harkentest::ObjectId(77), 99, 2, 1));
    ASSERT_TRUE(instances.AcquireSourceRetirementHold(leaf, executor.PreparedInstanceEpoch(1)));
    FailingOwnerProbe owner;
    ASSERT_TRUE(executor.AdoptPreparedOwnerToken(&owner, ValidateFailingOwner, CommitFailingOwner, CancelFailingOwner));
    ASSERT_TRUE(executor.ProveContainerCommits());
    EXPECT_FALSE(executor.Commit(29));
    EXPECT_TRUE(owner.committed);
    EXPECT_TRUE(owner.cancelled);
    EXPECT_FALSE(executor.ContainerCommitProofsHeld());
    EXPECT_EQ(1u, containers.LiveCount());
    EXPECT_EQ(2u, instances.LiveCount());
    EXPECT_FALSE(executor.RollbackCommitted(instances));
    EXPECT_FALSE(executor.ResetForReuse());
    ASSERT_TRUE(instances.ReleaseSourceRetirementHold(leaf));
    ASSERT_TRUE(executor.RollbackCommitted(instances));
    ASSERT_TRUE(executor.ResetForReuse());
    EXPECT_EQ(instances.Capacity(), instances.FreeCount());
    EXPECT_EQ(containers.Capacity(), containers.FreeCount());
}

TEST(PreparedPlayableTreeStep, InstanceProofConsumesActualInternalRowsBeforeLegacyContainerAndLinks) {
    PlayableInstanceTable instances(2);
    harken::ContainerPlaybackTable containers(1);
    harken::PreparedPlayableTreeStep step(2);
    ASSERT_TRUE(step.PrepareInstance(instances, PlayableKind::Container));
    const auto root = step.PreparedInstanceHandle(0);
    ASSERT_TRUE(step.PrepareInstance(instances, PlayableKind::Sound, root));
    const auto child = step.PreparedInstanceHandle(1);
    ASSERT_TRUE(step.PrepareContainer(instances, containers, root, step.PreparedInstanceEpoch(0),
        harkentest::ObjectId(77), 99, 2, 1));
    ASSERT_TRUE(step.ProveInstanceCommits(&instances));
    EXPECT_TRUE(step.InstanceCommitProofsHeld());
    harken::PreparedPlayableTreeStep denied(std::move(step)), other(1);
    EXPECT_EQ(0u, denied.InstanceReservationCount());
    other = std::move(step);
    step = std::move(other);
    EXPECT_EQ(2u, step.InstanceReservationCount());
    EXPECT_EQ(0u, other.InstanceReservationCount());
    EXPECT_FALSE(step.PrepareInstance(instances, PlayableKind::Sound));
    ASSERT_TRUE(step.Commit());
    EXPECT_FALSE(step.InstanceCommitProofsHeld());
    EXPECT_EQ(child, instances.FirstChild(root));
    EXPECT_EQ(2u, instances.LiveCount());
    EXPECT_EQ(1u, containers.LiveCount());
    ASSERT_TRUE(step.RollbackCommitted());
    EXPECT_EQ(2u, instances.FreeCount());
    EXPECT_EQ(1u, containers.FreeCount());
}

TEST(PlayableTreeExecutor, InstanceProofExternalRootAndAdoptedChildFreezeBothWholeMoves) {
    PlayableInstanceTable instances(3);
    harken::PreparedPlayableInstance root, child;
    ASSERT_TRUE(instances.PrepareAcquire(PlayableKind::Container, 0, &root));
    ASSERT_TRUE(instances.PrepareAcquire(PlayableKind::Sound, root.Handle(), &child));
    ASSERT_TRUE(instances.AcquireSourceRetirementHold(child.Handle(), child.ScheduleEpoch()));
    harken::PlayableTreeExecutor executor(2), other(1);
    ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &root));
    ASSERT_TRUE(executor.AdoptPreparedInstanceReservation(&child));
    ASSERT_TRUE(other.PrepareInstance(instances, PlayableKind::Sound));
    const auto unrelated = other.PreparedInstanceHandle(0);
    ASSERT_TRUE(executor.ProveInstanceCommits(&instances, &root));
    EXPECT_TRUE(instances.PreparedCommitHeld(root));
    EXPECT_TRUE(instances.PreparedCommitHeld(child));
    EXPECT_FALSE(executor.SetPreparedRootMetadata(instances, {}));
    EXPECT_FALSE(executor.EnablePreparedOwnerRollbackRetention());
    FailingOwnerProbe owner;
    EXPECT_FALSE(executor.AdoptPreparedOwnerToken(&owner, ValidateFailingOwner,
        CommitFailingOwner, CancelFailingOwner));
    EXPECT_FALSE(executor.ResetForReuse());
    harken::PlayableTreeExecutor denied(std::move(executor));
    EXPECT_EQ(0u, denied.InstanceReservationCount());
    EXPECT_TRUE(executor.InstanceCommitProofsHeld());
    other = std::move(executor);
    executor = std::move(other);
    EXPECT_EQ(unrelated, other.PreparedInstanceHandle(0));
    EXPECT_TRUE(instances.PreparedCommitHeld(root));
    ASSERT_TRUE(executor.Commit(instances, 17));
    EXPECT_FALSE(executor.InstanceCommitProofsHeld());
    EXPECT_EQ(child.Handle(), instances.FirstChild(root.Handle()));
    EXPECT_FALSE(executor.RollbackCommitted(instances));
    ASSERT_TRUE(instances.ReleaseSourceRetirementHold(child.Handle()));
    ASSERT_TRUE(executor.RollbackCommitted(instances));
    other.Cancel();
    EXPECT_EQ(3u, instances.FreeCount());
}

TEST(PlayableTreeExecutor, OriginalInstanceLifetimeCensusIncludesExternalOwnedAndAdoptedPreparations) {
    harken::PlayableInstanceTable table(4), foreign(4);
    harken::PreparedPlayableInstance root, adopted;
    ASSERT_TRUE(table.PrepareAcquire(harken::PlayableKind::Container, 0, &root));
    ASSERT_TRUE(table.PrepareAcquire(harken::PlayableKind::Sound, root.Handle(), &adopted));
    harken::PlayableTreeExecutor executor(2);
    ASSERT_TRUE(executor.AdoptPreparedInstance(table, &root));
    ASSERT_TRUE(executor.AdoptPreparedInstanceReservation(&adopted));
    ASSERT_TRUE(executor.PrepareInstance(table, harken::PlayableKind::Track, root.Handle()));
    const auto* owned = executor.PreparedInstanceToken(0);
    ASSERT_NE(nullptr, owned);
    ASSERT_EQ(3u, executor.PreparedInstanceCensusCount());
    EXPECT_EQ(owned, executor.PreparedInstanceCensusToken(0));
    EXPECT_EQ(&adopted, executor.PreparedInstanceCensusToken(1));
    EXPECT_EQ(&root, executor.PreparedInstanceCensusToken(2));
    EXPECT_EQ(nullptr, executor.PreparedInstanceCensusToken(3));
    const std::array<const harken::PreparedPlayableInstance*, 3> census{&root, owned, &adopted};
    std::array<harken::PlayableInstanceTable::OriginalLifetimePin, 4> baselines;
    EXPECT_FALSE(executor.IssueOriginalPreparedInstanceLifetimes(table, census, baselines));
    ASSERT_TRUE(executor.ProveInstanceCommits(&table, &root));
    EXPECT_FALSE(executor.IssueOriginalPreparedInstanceLifetimes(foreign, census, baselines));
    EXPECT_FALSE(executor.IssueOriginalPreparedInstanceLifetimes(table, {census.data(), 2}, baselines));
    const std::array<const harken::PreparedPlayableInstance*, 3> duplicate{&root, owned, owned};
    EXPECT_FALSE(executor.IssueOriginalPreparedInstanceLifetimes(table, duplicate, baselines));
    harken::PreparedPlayableInstance copied = adopted;
    const std::array<const harken::PreparedPlayableInstance*, 3> substituted{&root, owned, &copied};
    EXPECT_FALSE(executor.IssueOriginalPreparedInstanceLifetimes(table, substituted, baselines));
    EXPECT_FALSE(std::any_of(baselines.begin(), baselines.end(), [](const auto& pin) { return pin.Valid(); }));
    ASSERT_TRUE(executor.IssueOriginalPreparedInstanceLifetimes(table, census, baselines));
    EXPECT_FALSE(executor.ParentLinkProofHeld());
    EXPECT_FALSE(executor.IssueOriginalPreparedInstanceLifetimes(table, census, baselines));
    for (const auto* original : census) {
        const auto& baseline = baselines[static_cast<std::uint32_t>(original->Handle())];
        EXPECT_TRUE(table.IsOriginalLifetimePinFor(baseline, original->Handle()));
        EXPECT_EQ(original->Parent(), baseline.OriginalParent());
        EXPECT_EQ(original->ScheduleEpoch(), baseline.OriginalScheduleEpoch());
    }
    executor.ReleaseCommitProofs();
    for (auto& baseline : baselines) baseline.Release();
    ASSERT_TRUE(executor.Cancel(table));
    EXPECT_EQ(4u, table.FreeCount());
}

TEST(PlayableTreeExecutor, OriginalFutureLifetimeDoesNotAcquireHeldArrangingParentLinks) {
    harken::PlayableInstanceTable table(3);
    harken::PreparedPlayableInstance arranging, successor;
    ASSERT_TRUE(table.PrepareAcquire(harken::PlayableKind::Container, 0, &arranging));
    harken::PlayableTreeExecutor parent(1), future(1);
    ASSERT_TRUE(parent.AdoptPreparedInstance(table, &arranging));
    ASSERT_TRUE(table.PrepareAcquire(harken::PlayableKind::Segment, arranging.Handle(), &successor));
    ASSERT_TRUE(future.AdoptPreparedInstance(table, &successor));
    ASSERT_TRUE(parent.ProveInstanceCommits(&table, &arranging));
    ASSERT_TRUE(parent.ProveParentLinks(table, &arranging));
    ASSERT_TRUE(table.ParentLinksHeld(arranging.Handle()));
    ASSERT_TRUE(future.ProveInstanceCommits(&table, &successor));
    const std::array<const harken::PreparedPlayableInstance*, 1> census{&successor};
    std::array<harken::PlayableInstanceTable::OriginalLifetimePin, 3> baselines;
    ASSERT_TRUE(future.IssueOriginalPreparedInstanceLifetimes(table, census, baselines));
    auto& baseline = baselines[static_cast<std::uint32_t>(successor.Handle())];
    harken::PlayableInstanceTable::OriginalLifetimePin loan;
    ASSERT_TRUE(baseline.Borrow(&loan));
    future.ReleaseCommitProofs();
    EXPECT_FALSE(future.Cancel(table));
    EXPECT_TRUE(successor.Valid());
    EXPECT_EQ(arranging.Handle(), loan.OriginalParent());
    EXPECT_FALSE(table.CancelPrepared(&successor, &baseline));
    EXPECT_TRUE(successor.Valid());
    EXPECT_TRUE(baseline.Valid());
    loan.Release();
    // This standalone fixture supplies its baseline explicitly; Engine registers a resolver.
    ASSERT_TRUE(table.CancelPrepared(&successor, &baseline));
    ASSERT_TRUE(future.Cancel(table));
    parent.ReleaseCommitProofs();
    ASSERT_TRUE(parent.Cancel(table));
    EXPECT_EQ(3u, table.FreeCount());
    EXPECT_FALSE(loan.Valid());
}

TEST(PlayableTreeExecutor, OriginalLifetimeNativeLaterRefusalReturnsOnlyIssuedPrefixAndKeepsProofs) {
    harken::PlayableInstanceTable table(2);
    harken::PreparedPlayableInstance root;
    ASSERT_TRUE(table.PrepareAcquire(harken::PlayableKind::Container, 0, &root));
    harken::PlayableInstanceTable::PreparedCommitProof earlierProof;
    harken::PlayableInstanceTable::OriginalLifetimePin existingBaseline;
    ASSERT_TRUE(table.ProvePreparedCommit(root, &earlierProof));
    ASSERT_TRUE(table.IssueOriginalPreparedLifetime(earlierProof, &existingBaseline));
    earlierProof.Release();
    harken::PlayableTreeExecutor executor(1);
    ASSERT_TRUE(executor.AdoptPreparedInstance(table, &root));
    ASSERT_TRUE(executor.PrepareInstance(table, harken::PlayableKind::Sound, root.Handle()));
    const auto* child = executor.PreparedInstanceToken(0);
    ASSERT_NE(nullptr, child);
    ASSERT_TRUE(executor.ProveInstanceCommits(&table, &root));
    const std::array<const harken::PreparedPlayableInstance*, 2> census{&root, child};
    std::array<harken::PlayableInstanceTable::OriginalLifetimePin, 2> baselines;
    EXPECT_FALSE(executor.IssueOriginalPreparedInstanceLifetimes(table, census, baselines));
    EXPECT_FALSE(std::any_of(baselines.begin(), baselines.end(), [](const auto& pin) { return pin.Valid(); }));
    EXPECT_TRUE(table.IsOriginalLifetimePinFor(existingBaseline, root.Handle()));
    EXPECT_TRUE(executor.InstanceCommitProofsHeld());
    EXPECT_TRUE(executor.Validate());
    EXPECT_TRUE(table.PreparedCommitHeld(root));
    EXPECT_TRUE(table.PreparedCommitHeld(*child));
    existingBaseline.Release();
    ASSERT_TRUE(executor.IssueOriginalPreparedInstanceLifetimes(table, census, baselines));
    EXPECT_TRUE(executor.Validate());
    for (const auto* original : census)
        EXPECT_TRUE(table.IsOriginalLifetimePinFor(
            baselines[static_cast<std::uint32_t>(original->Handle())], original->Handle()));
    executor.ReleaseCommitProofs();
    for (auto& baseline : baselines) baseline.Release();
    ASSERT_TRUE(executor.Cancel(table));
    EXPECT_EQ(2u, table.FreeCount());
}

TEST(PlayableTreeExecutor, InstanceProofPrefixRefusalReleasesRootChildrenAndContainerButKeepsRequired) {
    PlayableInstanceTable instances(3);
    harken::ContainerPlaybackTable containers(1);
    harken::PreparedPlayableInstance root, first, second;
    ASSERT_TRUE(instances.PrepareAcquire(PlayableKind::Container, 0, &root));
    ASSERT_TRUE(instances.PrepareAcquire(PlayableKind::Sound, root.Handle(), &first));
    ASSERT_TRUE(instances.PrepareAcquire(PlayableKind::Sound, root.Handle(), &second));
    harken::PreparedContainerPlaybackRow row;
    ASSERT_TRUE(containers.Prepare(instances, root.Handle(), root.ScheduleEpoch(),
        harkentest::ObjectId(77), 99, 2, 1, &row));
    harken::PlayableTreeExecutor executor(3);
    ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &root));
    ASSERT_TRUE(executor.AdoptPreparedInstanceReservation(&first));
    ASSERT_TRUE(executor.AdoptPreparedInstanceReservation(&second));
    ASSERT_TRUE(executor.AdoptPreparedContainerReservation(&row));
    ASSERT_TRUE(executor.ProveContainerCommits(&containers, &row));
    const auto saved = harken::detail::PlayableInstancePreparationTestAccess::CorruptPreparing(instances, second, 0);
    EXPECT_FALSE(executor.ProveInstanceCommits(&instances, &root));
    EXPECT_FALSE(executor.InstanceCommitProofsHeld());
    EXPECT_FALSE(executor.ContainerCommitProofsHeld());
    EXPECT_FALSE(instances.PreparedCommitHeld(root));
    EXPECT_FALSE(instances.PreparedCommitHeld(first));
    EXPECT_TRUE(root.Valid());
    EXPECT_TRUE(first.Valid());
    EXPECT_TRUE(second.Valid());
    EXPECT_TRUE(row.Valid());
    EXPECT_FALSE(executor.Validate());
    EXPECT_FALSE(executor.Commit(instances, 19));
    EXPECT_EQ(0u, instances.LiveCount());
    harken::detail::PlayableInstancePreparationTestAccess::RestorePreparing(instances, second, saved);
    ASSERT_TRUE(executor.ProveContainerCommits(&containers, &row));
    ASSERT_TRUE(executor.ProveInstanceCommits(&instances, &root));
    executor.ReleaseCommitProofs();
    EXPECT_FALSE(executor.Commit(instances, 19));
    EXPECT_TRUE(root.Valid());
    ASSERT_TRUE(executor.ProveContainerCommits(&containers, &row));
    ASSERT_TRUE(executor.ProveInstanceCommits(&instances, &root));
    ASSERT_TRUE(executor.Commit(instances, 19));
    ASSERT_TRUE(executor.RollbackCommitted(instances));
    EXPECT_EQ(3u, instances.FreeCount());
    EXPECT_EQ(1u, containers.FreeCount());
}

TEST(PlayableTreeExecutor, InstanceProofLaterContainerRefusalReleasesExternalRootAndChildren) {
    for (unsigned refusal = 0; refusal < 5; ++refusal) {
        SCOPED_TRACE(refusal);
        PlayableInstanceTable instances(2);
        harken::ContainerPlaybackTable containers(2), foreign(1);
        harken::PreparedPlayableInstance root, child;
        ASSERT_TRUE(instances.PrepareAcquire(PlayableKind::Container, 0, &root));
        ASSERT_TRUE(instances.PrepareAcquire(PlayableKind::Container, root.Handle(), &child));
        harken::PreparedContainerPlaybackRow first, second, unrelated;
        ASSERT_TRUE(containers.Prepare(instances, root.Handle(), root.ScheduleEpoch(),
            harkentest::ObjectId(77), 99, 2, 1, &first));
        ASSERT_TRUE(containers.Prepare(instances, child.Handle(), child.ScheduleEpoch(),
            harkentest::ObjectId(78), 100, 2, 1, &second));
        ASSERT_TRUE(foreign.Prepare(instances, root.Handle(), root.ScheduleEpoch(),
            harkentest::ObjectId(79), 101, 2, 1, &unrelated));
        harken::PlayableTreeExecutor executor(2);
        ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &root));
        ASSERT_TRUE(executor.AdoptPreparedInstanceReservation(&child));
        ASSERT_TRUE(executor.AdoptPreparedContainerReservation(&first));
        ASSERT_TRUE(executor.AdoptPreparedContainerReservation(&second));
        harken::ContainerPlaybackRow saved{};
        if (refusal == 3) {
            saved = harken::detail::ContainerPlaybackTableTestAccess::CorruptPreparing(
                containers, second, 2);
        }
        ASSERT_TRUE(executor.ProveInstanceCommits(&instances, &root));
        ASSERT_TRUE(instances.PreparedCommitHeld(root));
        ASSERT_TRUE(instances.PreparedCommitHeld(child));
        if (refusal == 4) {
            ASSERT_TRUE(executor.ProveContainerCommits(&containers, &first));
            ASSERT_TRUE(containers.PreparedCommitHeld(first));
            ASSERT_TRUE(containers.PreparedCommitHeld(second));
        }
        if (refusal == 0) EXPECT_FALSE(executor.ProveContainerCommits(&foreign, &unrelated));
        if (refusal == 1) EXPECT_FALSE(executor.ProveContainerCommits(&containers, nullptr));
        if (refusal == 2) EXPECT_FALSE(executor.ProveContainerCommits(&containers, &unrelated));
        if (refusal >= 3) EXPECT_FALSE(executor.ProveContainerCommits(&containers, &first));
        EXPECT_FALSE(executor.InstanceCommitProofsHeld());
        EXPECT_FALSE(executor.ContainerCommitProofsHeld());
        EXPECT_FALSE(instances.PreparedCommitHeld(root));
        EXPECT_FALSE(instances.PreparedCommitHeld(child));
        EXPECT_FALSE(containers.PreparedCommitHeld(first));
        EXPECT_FALSE(containers.PreparedCommitHeld(second));
        EXPECT_TRUE(root.Valid());
        EXPECT_TRUE(child.Valid());
        EXPECT_TRUE(first.Valid());
        EXPECT_TRUE(second.Valid());
        EXPECT_FALSE(executor.Validate());
        EXPECT_FALSE(executor.Commit(instances, 19));
        EXPECT_EQ(0u, instances.LiveCount());
        EXPECT_EQ(0u, containers.LiveCount());
        if (refusal == 3) {
            harken::detail::ContainerPlaybackTableTestAccess::RestorePreparing(
                containers, second, saved);
        }
        ASSERT_TRUE(executor.ProveInstanceCommits(&instances, &root));
        ASSERT_TRUE(executor.ProveContainerCommits(&containers, &first));
        ASSERT_TRUE(executor.Commit(instances, 19));
        EXPECT_EQ(child.Handle(), instances.FirstChild(root.Handle()));
        ASSERT_TRUE(executor.RollbackCommitted(instances));
        ASSERT_TRUE(foreign.Cancel(&unrelated));
        EXPECT_EQ(2u, instances.FreeCount());
        EXPECT_EQ(2u, containers.FreeCount());
        EXPECT_EQ(1u, foreign.FreeCount());
    }
}

TEST(PlayableTreeExecutor, InstanceProofWrongRootTableAndOverloadReleaseAllBeforeAnyActivation) {
    PlayableInstanceTable instances(2), foreign(1);
    harken::ContainerPlaybackTable containers(1);
    harken::PreparedPlayableInstance root, absent;
    ASSERT_TRUE(instances.PrepareAcquire(PlayableKind::Container, 0, &root));
    harken::PreparedContainerPlaybackRow row;
    ASSERT_TRUE(containers.Prepare(instances, root.Handle(), root.ScheduleEpoch(),
        harkentest::ObjectId(77), 99, 2, 1, &row));
    harken::PlayableTreeExecutor executor(2);
    ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &root));
    ASSERT_TRUE(executor.PrepareInstance(instances, PlayableKind::Sound, root.Handle()));
    ASSERT_TRUE(executor.AdoptPreparedContainerReservation(&row));
    EXPECT_FALSE(executor.ProveInstanceCommits());
    EXPECT_FALSE(executor.ProveInstanceCommits(&instances, &absent));
    EXPECT_FALSE(executor.ProveInstanceCommits(&foreign, &root));
    const auto prove = [&] {
        ASSERT_TRUE(executor.ProveContainerCommits(&containers, &row));
        ASSERT_TRUE(executor.ProveInstanceCommits(&instances, &root));
    };
    ASSERT_NO_FATAL_FAILURE(prove());
    EXPECT_FALSE(executor.Commit(23));
    EXPECT_FALSE(executor.InstanceCommitProofsHeld());
    EXPECT_FALSE(executor.ContainerCommitProofsHeld());
    EXPECT_TRUE(root.Valid());
    EXPECT_EQ(0u, instances.LiveCount());
    ASSERT_NO_FATAL_FAILURE(prove());
    EXPECT_FALSE(executor.Commit(foreign, 23));
    EXPECT_FALSE(executor.InstanceCommitProofsHeld());
    EXPECT_FALSE(executor.ContainerCommitProofsHeld());
    EXPECT_EQ(1u, foreign.FreeCount());
    ASSERT_NO_FATAL_FAILURE(prove());
    ASSERT_TRUE(executor.Commit(instances, 23));
    ASSERT_TRUE(executor.RollbackCommitted(instances));
    EXPECT_EQ(2u, instances.FreeCount());
    EXPECT_EQ(1u, containers.FreeCount());
}

TEST(PlayableTreeExecutor, AdoptionRefusesActualDuplicateExternalRootAliasAndReclaims) {
    PlayableInstanceTable instances(2);
    harken::PreparedPlayableInstance root;
    ASSERT_TRUE(instances.PrepareAcquire(PlayableKind::Container, 0, &root));
    auto alias = root;
    harken::PlayableTreeExecutor executor(2);
    ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &root));
    EXPECT_FALSE(executor.AdoptPreparedInstanceReservation(&alias));
    EXPECT_FALSE(executor.InstanceCommitProofsHeld());
    EXPECT_FALSE(instances.PreparedCommitHeld(root));
    EXPECT_TRUE(root.Valid());
    ASSERT_TRUE(executor.ProveInstanceCommits(&instances, &root));
    EXPECT_TRUE(executor.InstanceCommitProofsHeld());
    EXPECT_EQ(0u, instances.LiveCount());
    ASSERT_TRUE(executor.Cancel(instances));
    EXPECT_FALSE(root.Valid());
    EXPECT_FALSE(alias.Valid());
    EXPECT_EQ(2u, instances.FreeCount());
}

TEST(PlayableTreeExecutor, InstanceProofBorrowsActualActiveParentWithoutConsumingIt) {
    PlayableInstanceTable instances(2);
    const auto parent = instances.Acquire(PlayableKind::Container, 0);
    ASSERT_NE(0u, parent);
    harken::PlayableTreeExecutor executor(1);
    ASSERT_TRUE(executor.PrepareInstance(instances, PlayableKind::Sound, parent));
    const auto child = executor.PreparedInstanceHandle(0);
    ASSERT_TRUE(executor.ProveInstanceCommits());
    ASSERT_TRUE(executor.Commit(29));
    EXPECT_EQ(child, instances.FirstChild(parent));
    ASSERT_TRUE(executor.RollbackCommitted(instances));
    EXPECT_TRUE(instances.Classify(parent).live);
    EXPECT_EQ(PlayableLifecycle::Active, instances.Classify(parent).lifecycle);
    EXPECT_EQ(1u, instances.LiveCount());
    RetireFully(instances, parent);
    EXPECT_EQ(2u, instances.FreeCount());
}

TEST(PlayableTreeExecutor, InstanceProofConsumedOwnerFailureRetainsSourceUntilActualRelease) {
    PlayableInstanceTable instances(2);
    harken::ContainerPlaybackTable containers(1);
    harken::PlayableTreeExecutor executor(2);
    ASSERT_TRUE(executor.PrepareInstance(instances, PlayableKind::Container));
    const auto root = executor.PreparedInstanceHandle(0);
    ASSERT_TRUE(executor.PrepareInstance(instances, PlayableKind::Sound, root));
    const auto child = executor.PreparedInstanceHandle(1);
    ASSERT_TRUE(executor.PrepareContainer(instances, containers, root, executor.PreparedInstanceEpoch(0),
        harkentest::ObjectId(77), 99, 2, 1));
    ASSERT_TRUE(instances.AcquireSourceRetirementHold(child, executor.PreparedInstanceEpoch(1)));
    FailingOwnerProbe owner;
    ASSERT_TRUE(executor.AdoptPreparedOwnerToken(&owner, ValidateFailingOwner, CommitFailingOwner, CancelFailingOwner));
    ASSERT_TRUE(executor.ProveContainerCommits());
    ASSERT_TRUE(executor.ProveInstanceCommits());
    EXPECT_FALSE(executor.Commit(31));
    EXPECT_TRUE(owner.committed);
    EXPECT_TRUE(owner.cancelled);
    EXPECT_FALSE(executor.InstanceCommitProofsHeld());
    EXPECT_EQ(2u, instances.LiveCount());
    EXPECT_EQ(1u, containers.LiveCount());
    EXPECT_FALSE(executor.RollbackCommitted(instances));
    EXPECT_FALSE(executor.ResetForReuse());
    ASSERT_TRUE(instances.ReleaseSourceRetirementHold(child));
    ASSERT_TRUE(executor.RollbackCommitted(instances));
    ASSERT_TRUE(executor.ResetForReuse());
    EXPECT_EQ(2u, instances.FreeCount());
    EXPECT_EQ(1u, containers.FreeCount());
}

TEST(PlayableTreeExecutor, InstanceProofDestructionReleasesOriginalExternalRootBeforeCapsDie) {
    PlayableInstanceTable instances(2);
    harken::ContainerPlaybackTable containers(1);
    harken::PreparedPlayableInstance root, child;
    ASSERT_TRUE(instances.PrepareAcquire(PlayableKind::Container, 0, &root));
    ASSERT_TRUE(instances.PrepareAcquire(PlayableKind::Sound, root.Handle(), &child));
    harken::PreparedContainerPlaybackRow row;
    ASSERT_TRUE(containers.Prepare(instances, root.Handle(), root.ScheduleEpoch(),
        harkentest::ObjectId(77), 99, 2, 1, &row));
    {
        harken::PlayableTreeExecutor executor(2);
        ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &root));
        ASSERT_TRUE(executor.AdoptPreparedInstanceReservation(&child));
        ASSERT_TRUE(executor.AdoptPreparedContainerReservation(&row));
        ASSERT_TRUE(executor.ProveContainerCommits(&containers, &row));
        ASSERT_TRUE(executor.ProveInstanceCommits(&instances, &root));
    }
    EXPECT_TRUE(root.Valid());
    EXPECT_FALSE(instances.PreparedCommitHeld(root));
    EXPECT_FALSE(child.Valid());
    EXPECT_EQ(1u, containers.FreeCount());
    ASSERT_TRUE(instances.CancelPrepared(&root));
    EXPECT_EQ(2u, instances.FreeCount());
}

TEST(PreparedPlayableTreeStep, CommitsParentBeforePreparedChildAndContainerRow)
{
    harken::PlayableInstanceTable instances(2);
    harken::ContainerPlaybackTable containers(1);
    harken::PreparedPlayableTreeStep step(2);
    ASSERT_TRUE(step.PrepareInstance(
        instances, harken::PlayableKind::Container));
    ASSERT_TRUE(step.PrepareInstance(
        instances, harken::PlayableKind::Sound,
        step.PreparedInstanceHandle(0)));
    ASSERT_TRUE(step.PrepareContainer(instances, containers, step.PreparedInstanceHandle(0),
        step.PreparedInstanceEpoch(0), harkentest::ObjectId(77u), 99u, 2u, 2u));
    ASSERT_TRUE(step.Validate());
    ASSERT_TRUE(step.Commit());
    EXPECT_EQ(instances.LiveCount(), 2u);
    const auto root = instances.Acquire(harken::PlayableKind::Sound, 0);
    EXPECT_EQ(root, 0u);
    EXPECT_EQ(containers.FreeCount(), 0u);
}

TEST(PreparedPlayableTreeStep, RollsBackEarlierCommitsWhenALaterParentIsStale)
{
    harken::PlayableInstanceTable instances(6);
    harken::PreparedPlayableInstance preparedParent;
    harken::PreparedPlayableInstance staleParent;
    ASSERT_TRUE(instances.PrepareAcquire(
        harken::PlayableKind::Container, 0, &preparedParent));
    ASSERT_TRUE(instances.PrepareAcquire(
        harken::PlayableKind::Container, 0, &staleParent));

    harken::PreparedPlayableTreeStep step(4);
    ASSERT_TRUE(step.PrepareInstance(
        instances, harken::PlayableKind::Container));
    const auto root = step.PreparedInstanceHandle(0);
    ASSERT_TRUE(step.PrepareInstance(
        instances, harken::PlayableKind::Sound, root));
    ASSERT_TRUE(step.PrepareInstance(
        instances, harken::PlayableKind::Sound, preparedParent.Handle()));
    ASSERT_TRUE(step.PrepareInstance(
        instances, harken::PlayableKind::Sound, staleParent.Handle()));
    ASSERT_TRUE(instances.CancelPrepared(&staleParent));

    EXPECT_FALSE(step.Commit());
    EXPECT_EQ(instances.LiveCount(), 0u);
    ASSERT_TRUE(instances.CancelPrepared(&preparedParent));
    EXPECT_EQ(instances.FreeCount(), instances.Capacity());
}

TEST(PlayableTreeExecutor, SharesTreeTransactionAndChildFactService)
{
    harken::PlayableInstanceTable instances(2);
    harken::ContainerPlaybackTable containers(1);
    harken::PlayableTreeExecutor executor(2);
    ASSERT_TRUE(executor.PrepareInstance(
        instances, harken::PlayableKind::Container));
    const auto parent = executor.PreparedInstanceHandle(0);
    ASSERT_TRUE(executor.PrepareInstance(
        instances, harken::PlayableKind::Sound, parent));
    const auto child = executor.PreparedInstanceHandle(1);
    ASSERT_TRUE(executor.PrepareContainer(instances, containers, parent, executor.PreparedInstanceEpoch(0), harkentest::ObjectId(77u), 99u,
        2u, 1u));

    // Nothing is visible until the shared transaction commits all owners.
    EXPECT_EQ(instances.LiveCount(), 0u);
    EXPECT_EQ(containers.LiveCount(), 0u);
    ASSERT_TRUE(executor.Validate());
    ASSERT_TRUE(executor.Commit());
    ASSERT_TRUE(containers.SetActiveChild(
        parent, child, instances.Classify(child).scheduleEpoch));
    ASSERT_TRUE(instances.SetTerminalOutcome(
        child, {harken::PlayableTerminalKind::NaturalCompletion, 0, 1u}));
    ASSERT_TRUE(instances.AdvanceLifecycle(
        child, harken::PlayableLifecycle::LogicalComplete));

    EXPECT_EQ(
        harken::PlayableTreeExecutor::ServiceChildOutcome(
            instances, containers, parent, child,
            instances.Classify(child).scheduleEpoch, 1u),
        harken::ContainerPlaybackTable::ChildOutcomeStatus::Accepted);
    // A delayed duplicate is consumed by the same service seam and cannot
    // advance the container a second time.
    EXPECT_EQ(
        harken::PlayableTreeExecutor::ServiceChildOutcome(
            instances, containers, parent, child,
            instances.Classify(child).scheduleEpoch, 1u),
        harken::ContainerPlaybackTable::ChildOutcomeStatus::Duplicate);
}

class NaturalSelectionOutcome : public ::testing::Test {
protected:
    harken::PlayableInstanceTable instances{4};
    harken::ContainerPlaybackTable containers{1};
    harken::PlayableInstanceHandle parent = 0;
    harken::PlayableInstanceHandle child = 0;
    static constexpr std::uint64_t ordinal = (UINT64_C(1) << 40) + 17u;

    void SetUp() override {
        parent = instances.Acquire(harken::PlayableKind::Container, 0);
        child = instances.Acquire(harken::PlayableKind::Sound, parent);
        ASSERT_NE(0u, child);
        harken::PreparedContainerPlaybackRow prepared;
        ASSERT_TRUE(containers.Prepare(instances, parent,
            instances.Classify(parent).scheduleEpoch, harkentest::ObjectId(77u), 99u,
            static_cast<std::uint8_t>(harken::ContainerModeV1::Sequence), 2u,
            &prepared));
        ASSERT_TRUE(containers.Commit(&prepared));
        ASSERT_TRUE(harken::PlayableTreeExecutor::ServiceActiveChild(
            instances, containers, parent, child));
    }

    void Publish(harken::PlayableTerminalKind kind, std::uint64_t event) {
        ASSERT_TRUE(instances.SetTerminalOutcome(child, {kind, 0, 17u, event}));
        ASSERT_TRUE(instances.AdvanceLifecycle(child, harken::PlayableLifecycle::LogicalComplete));
    }

    auto Consume(std::uint64_t event) {
        return harken::PlayableTreeExecutor::ServiceNaturalSelectionOutcome(
            instances, containers, parent, child,
            instances.Classify(child).scheduleEpoch, event);
    }

    harken::ContainerPlaybackRow Snapshot() {
        harken::ContainerPlaybackRow snapshot{};
        EXPECT_TRUE(containers.Capture(parent, &snapshot));
        return snapshot;
    }
};

TEST_F(NaturalSelectionOutcome, ExactFullWidthEventIsImmutableAndConsumedOnce) {
    using Status = harken::ContainerPlaybackTable::ChildOutcomeStatus;
    Publish(harken::PlayableTerminalKind::NaturalCompletion, ordinal);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_FALSE(instances.SetTerminalOutcome(child,
        {harken::PlayableTerminalKind::NaturalCompletion, 0, 17u, ordinal + 1u}));
    EXPECT_EQ(ordinal, instances.Classify(child).terminalOutcome.naturalCompletionOrdinal);
    EXPECT_EQ(Status::Mismatch, Consume(0));
    EXPECT_EQ(Status::Mismatch, Consume(static_cast<std::uint32_t>(ordinal)));
    EXPECT_EQ(Status::Mismatch,
        harken::PlayableTreeExecutor::ServiceNaturalSelectionOutcome(
            instances, containers, parent, child,
            instances.Classify(child).scheduleEpoch + 1u, ordinal));
    EXPECT_EQ(0u, Snapshot().completedSelections);
    EXPECT_EQ(child, Snapshot().activeChild);
    EXPECT_EQ(Status::Accepted, Consume(ordinal));
    const auto consumed = Snapshot();
    EXPECT_EQ(1u, consumed.completedSelections);
    EXPECT_EQ(0u, consumed.activeChild);
    EXPECT_EQ(ordinal, consumed.lastConsumedOutcomeOrdinal);
    EXPECT_EQ(Status::Duplicate, Consume(ordinal));
    ASSERT_TRUE(containers.MarkLogicalComplete(parent));
    EXPECT_EQ(Status::Duplicate, Consume(ordinal));
    EXPECT_EQ(1u, Snapshot().completedSelections);
    RetireFully(instances, child);
    EXPECT_EQ(Status::Invalid, Consume(ordinal));
    EXPECT_EQ(1u, Snapshot().completedSelections);
}

TEST_F(NaturalSelectionOutcome, MissingImmutableOrdinalDoesNotConsume) {
    Publish(harken::PlayableTerminalKind::NaturalCompletion, 0);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(harken::ContainerPlaybackTable::ChildOutcomeStatus::Mismatch, Consume(ordinal));
    EXPECT_EQ(0u, Snapshot().completedSelections);
    EXPECT_EQ(child, Snapshot().activeChild);
}

class NonNaturalSelectionOutcome : public NaturalSelectionOutcome,
    public ::testing::WithParamInterface<harken::PlayableTerminalKind> {};

TEST_P(NonNaturalSelectionOutcome, TerminalKindCannotConsumeASelection) {
    Publish(GetParam(), ordinal);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(harken::ContainerPlaybackTable::ChildOutcomeStatus::Mismatch, Consume(ordinal));
    EXPECT_EQ(0u, Snapshot().completedSelections);
    EXPECT_EQ(child, Snapshot().activeChild);
    EXPECT_EQ(harken::ContainerPlaybackPhase::WaitingForChild, Snapshot().phase);
}

INSTANTIATE_TEST_SUITE_P(OrdinaryCompletion, NonNaturalSelectionOutcome,
    ::testing::Values(harken::PlayableTerminalKind::Failure,
                     harken::PlayableTerminalKind::RequestedStop,
                     harken::PlayableTerminalKind::ParentCancellation,
                     harken::PlayableTerminalKind::ReplacementReset));

TEST_F(NaturalSelectionOutcome, FiniteOverConsumptionPreservesTheWholeRow) {
    using Status = harken::ContainerPlaybackTable::ChildOutcomeStatus;
    auto snapshot = Snapshot();
    snapshot.loopLimit = 1;
    ASSERT_TRUE(containers.Restore(snapshot));
    Publish(harken::PlayableTerminalKind::NaturalCompletion, ordinal);
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_EQ(Status::Accepted, Consume(ordinal));
    const auto consumedChild = child;
    child = instances.Acquire(harken::PlayableKind::Sound, parent);
    ASSERT_NE(0u, child);
    ASSERT_TRUE(harken::PlayableTreeExecutor::ServiceActiveChild(
        instances, containers, parent, child));
    Publish(harken::PlayableTerminalKind::NaturalCompletion, ordinal + 1u);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(Status::Invalid, Consume(ordinal + 1u));
    snapshot = Snapshot();
    EXPECT_EQ(1u, snapshot.completedSelections);
    EXPECT_EQ(child, snapshot.activeChild);
    EXPECT_EQ(consumedChild, snapshot.lastConsumedChild);
    EXPECT_EQ(ordinal, snapshot.lastConsumedOutcomeOrdinal);
    EXPECT_EQ(harken::ContainerPlaybackPhase::WaitingForChild, snapshot.phase);
}

TEST_F(NaturalSelectionOutcome, InfiniteCountSaturatesWithoutRejectingNewEvents) {
    using Status = harken::ContainerPlaybackTable::ChildOutcomeStatus;
    auto snapshot = Snapshot();
    snapshot.loopLimit = 0;
    snapshot.completedSelections = UINT32_MAX - 1u;
    ASSERT_TRUE(containers.Restore(snapshot));
    Publish(harken::PlayableTerminalKind::NaturalCompletion, ordinal);
    ASSERT_FALSE(HasFatalFailure());
    ASSERT_EQ(Status::Accepted, Consume(ordinal));
    EXPECT_EQ(UINT32_MAX, Snapshot().completedSelections);
    child = instances.Acquire(harken::PlayableKind::Sound, parent);
    ASSERT_NE(0u, child);
    ASSERT_TRUE(harken::PlayableTreeExecutor::ServiceActiveChild(
        instances, containers, parent, child));
    Publish(harken::PlayableTerminalKind::NaturalCompletion, ordinal + 1u);
    ASSERT_FALSE(HasFatalFailure());
    EXPECT_EQ(Status::Accepted, Consume(ordinal + 1u));
    EXPECT_EQ(Status::Duplicate, Consume(ordinal + 1u));
    EXPECT_EQ(UINT32_MAX, Snapshot().completedSelections);
    EXPECT_EQ(0u, Snapshot().activeChild);
}

TEST_F(NaturalSelectionOutcome, QueueAndVariableControlKeepGenericConsumption) {
    using Status = harken::ContainerPlaybackTable::ChildOutcomeStatus;
    const auto initial = Snapshot();
    Publish(harken::PlayableTerminalKind::NaturalCompletion, ordinal);
    ASSERT_FALSE(HasFatalFailure());
    for (auto mode : {harken::ContainerModeV1::RandomQueue,
                      harken::ContainerModeV1::SequenceQueue,
                      harken::ContainerModeV1::VariableControl}) {
        auto snapshot = initial;
        snapshot.mode = static_cast<std::uint8_t>(mode);
        ASSERT_TRUE(containers.Restore(snapshot));
        EXPECT_EQ(Status::Invalid, Consume(ordinal));
        EXPECT_EQ(child, Snapshot().activeChild);
        EXPECT_EQ(Status::Accepted,
            harken::PlayableTreeExecutor::ServiceChildOutcome(instances, containers,
                parent, child, instances.Classify(child).scheduleEpoch, ordinal));
        EXPECT_EQ(0u, Snapshot().completedSelections);
    }
}

TEST(PlayableTreeExecutor, OwnerFailureDoesNotPublishPreparedLinks)
{
    harken::PlayableInstanceTable instances(2);
    harken::ContainerPlaybackTable containers(1);
    harken::PlayableTreeExecutor executor(2);
    ASSERT_TRUE(executor.PrepareInstance(
        instances, harken::PlayableKind::Container));
    const auto parent = executor.PreparedInstanceHandle(0);
    ASSERT_TRUE(executor.PrepareInstance(
        instances, harken::PlayableKind::Sound, parent));
    const auto child = executor.PreparedInstanceHandle(1);
    ASSERT_TRUE(executor.PrepareContainer(instances, containers, parent, executor.PreparedInstanceEpoch(0), harkentest::ObjectId(77u), 99u,
        2u, 1u));

    FailingOwnerProbe owner;
    ASSERT_TRUE(executor.AdoptPreparedOwnerToken(
        &owner, ValidateFailingOwner, CommitFailingOwner,
        CancelFailingOwner));

    EXPECT_FALSE(executor.Commit());
    EXPECT_TRUE(owner.committed);
    EXPECT_TRUE(owner.cancelled);
    EXPECT_EQ(0u, instances.LiveCount());
    EXPECT_EQ(0u, instances.FirstChild(parent));
    EXPECT_EQ(0u, instances.Classify(child).live);
    EXPECT_EQ(0u, containers.LiveCount());
    EXPECT_EQ(instances.Capacity(), instances.FreeCount());
    EXPECT_EQ(containers.Capacity(), containers.FreeCount());
}

TEST(PlayableTreeExecutor, AuthorityInvalidationRejectsCommitBeforePublication)
{
    harken::PlayableInstanceTable instances(1);
    harken::ContainerPlaybackTable containers(1);
    harken::PlayableTreeExecutor executor(1);
    ASSERT_TRUE(executor.PrepareInstance(
        instances, harken::PlayableKind::Container));
    const auto parent = executor.PreparedInstanceHandle(0);
    ASSERT_TRUE(executor.PrepareContainer(
        instances, containers, parent, executor.PreparedInstanceEpoch(0),
        harkentest::ObjectId(77u), 99u, 2u, 1u));
    AuthorityOwnerProbe authority;
    ASSERT_TRUE(executor.AdoptPreparedOwnerToken(
        &authority, ValidateAuthorityOwner, CommitAuthorityOwner,
        CancelAuthorityOwner));
    authority.valid = false;
    EXPECT_FALSE(executor.Validate());
    EXPECT_FALSE(executor.Commit());
    EXPECT_EQ(0u, instances.LiveCount());
    EXPECT_EQ(0u, containers.LiveCount());
}

TEST(PlayableTreeExecutor, RejectsStaleOrForeignChildOutcome)
{
    harken::PlayableInstanceTable instances(3);
    harken::ContainerPlaybackTable containers(1);
    const auto parent = instances.Acquire(harken::PlayableKind::Container, 0);
    const auto child = instances.Acquire(harken::PlayableKind::Sound, parent);
    const auto foreignParent = instances.Acquire(harken::PlayableKind::Container, 0);
    ASSERT_NE(0u, parent);
    ASSERT_NE(0u, child);
    ASSERT_NE(0u, foreignParent);
    harken::PreparedContainerPlaybackRow row;
    ASSERT_TRUE(containers.Prepare(instances,
        parent, instances.Classify(parent).scheduleEpoch, harkentest::ObjectId(77u), 99u, 2u, 1u,
        &row));
    ASSERT_TRUE(containers.Commit(&row));
    ASSERT_TRUE(containers.SetActiveChild(
        parent, child, instances.Classify(child).scheduleEpoch));
    EXPECT_EQ(
        harken::ContainerPlaybackTable::ChildOutcomeStatus::Invalid,
        harken::PlayableTreeExecutor::ServiceChildOutcome(
            instances, containers, foreignParent, child,
            instances.Classify(child).scheduleEpoch, 1u));
    EXPECT_EQ(
        harken::ContainerPlaybackTable::ChildOutcomeStatus::Mismatch,
        harken::PlayableTreeExecutor::ServiceChildOutcome(
            instances, containers, parent, child, 999u, 1u));
}

TEST(PlayableTreeExecutor, RejectsBumpedParentEpochAndUnlinkedChild)
{
    harken::PlayableInstanceTable instances(3);
    harken::ContainerPlaybackTable containers(1);
    const auto parent = instances.Acquire(harken::PlayableKind::Container, 0);
    const auto child = instances.Acquire(harken::PlayableKind::Sound, parent);
    const auto unrelated = instances.Acquire(harken::PlayableKind::Sound, 0);
    ASSERT_NE(0u, parent);
    ASSERT_NE(0u, child);
    ASSERT_NE(0u, unrelated);
    harken::PreparedContainerPlaybackRow row;
    ASSERT_TRUE(containers.Prepare(instances,
        parent, instances.Classify(parent).scheduleEpoch, harkentest::ObjectId(77u), 99u, 2u, 1u,
        &row));
    ASSERT_TRUE(containers.Commit(&row));
    ASSERT_TRUE(containers.SetActiveChild(
        parent, child, instances.Classify(child).scheduleEpoch));
    harken::ContainerPlaybackRow snapshot{};
    ASSERT_TRUE(containers.Capture(parent, &snapshot));
    ASSERT_NE(0u, instances.BumpScheduleEpoch(parent));
    EXPECT_EQ(
        harken::ContainerPlaybackTable::ChildOutcomeStatus::Mismatch,
        harken::PlayableTreeExecutor::ServiceChildOutcome(
            instances, containers, parent, child,
            instances.Classify(child).scheduleEpoch, 1u));

    ASSERT_TRUE(containers.Restore(snapshot));
    EXPECT_EQ(
        harken::ContainerPlaybackTable::ChildOutcomeStatus::Invalid,
        harken::PlayableTreeExecutor::ServiceChildOutcome(
            instances, containers, parent, unrelated,
            instances.Classify(unrelated).scheduleEpoch, 1u));
}

TEST(PlayableTreeExecutor, ServicesCommittedActiveChildWithCurrentEpoch)
{
    harken::PlayableInstanceTable instances(3);
    harken::ContainerPlaybackTable containers(1);
    const auto parent = instances.Acquire(harken::PlayableKind::Container, 0);
    const auto child = instances.Acquire(harken::PlayableKind::Sound, parent);
    const auto foreign = instances.Acquire(harken::PlayableKind::Sound, 0);
    ASSERT_NE(0u, parent);
    ASSERT_NE(0u, child);
    ASSERT_NE(0u, foreign);
    harken::PreparedContainerPlaybackRow row;
    ASSERT_TRUE(containers.Prepare(instances,
        parent, instances.Classify(parent).scheduleEpoch, harkentest::ObjectId(77u), 99u, 2u, 1u,
        &row));
    ASSERT_TRUE(containers.Commit(&row));

    EXPECT_TRUE(harken::PlayableTreeExecutor::ServiceActiveChild(
        instances, containers, parent, child));
    harken::ContainerPlaybackRow snapshot{};
    ASSERT_TRUE(containers.Capture(parent, &snapshot));
    EXPECT_EQ(child, snapshot.activeChild);
    EXPECT_EQ(instances.Classify(child).scheduleEpoch,
              snapshot.activeChildEpoch);
    EXPECT_FALSE(harken::PlayableTreeExecutor::ServiceActiveChild(
        instances, containers, parent, foreign));
}

TEST(PlayableTreeExecutor, RetiresPlayableTreeBottomUpAndReclaimsRows)
{
    harken::PlayableInstanceTable instances(3);
    harken::ContainerPlaybackTable containers(2);
    harken::PlayableTreeExecutor executor(3);
    ASSERT_TRUE(executor.PrepareInstance(
        instances, harken::PlayableKind::Container));
    const auto root = executor.PreparedInstanceHandle(0);
    ASSERT_TRUE(executor.PrepareInstance(
        instances, harken::PlayableKind::Container, root));
    const auto nested = executor.PreparedInstanceHandle(1);
    ASSERT_TRUE(executor.PrepareInstance(
        instances, harken::PlayableKind::Sound, nested));
    const auto leaf = executor.PreparedInstanceHandle(2);
    ASSERT_TRUE(executor.PrepareContainer(
        instances, containers, root, executor.PreparedInstanceEpoch(0),
        harkentest::ObjectId(700u), 701u, static_cast<std::uint8_t>(harken::ContainerModeV1::Random),
        1u));
    ASSERT_TRUE(executor.PrepareContainer(
        instances, containers, nested, executor.PreparedInstanceEpoch(1),
        harkentest::ObjectId(701u), 702u,
        static_cast<std::uint8_t>(harken::ContainerModeV1::SequenceQueue), 1u));
    ASSERT_TRUE(executor.Commit());
    ASSERT_TRUE(harken::PlayableTreeExecutor::ServiceActiveChild(
        instances, containers, root, nested));
    ASSERT_TRUE(harken::PlayableTreeExecutor::ServiceActiveChild(
        instances, containers, nested, leaf));

    EXPECT_TRUE(harken::PlayableTreeExecutor::RetirePlayableTreeBottomUp(
        instances, containers, root,
        harken::PlayableTerminalKind::RequestedStop));
    EXPECT_FALSE(instances.Classify(root).live);
    EXPECT_FALSE(instances.Classify(nested).live);
    EXPECT_FALSE(instances.Classify(leaf).live);
    EXPECT_EQ(0u, instances.LiveCount());
    EXPECT_EQ(0u, containers.LiveCount());
}

TEST(PlayableTreeExecutor, CancelsPreparedTreeWithoutPublishingRows)
{
    harken::PlayableInstanceTable instances(2);
    harken::ContainerPlaybackTable containers(1);
    harken::PlayableTreeExecutor executor(2);
    ASSERT_TRUE(executor.PrepareInstance(
        instances, harken::PlayableKind::Container));
    ASSERT_TRUE(executor.PrepareContainer(instances, containers, executor.PreparedInstanceHandle(0),
        executor.PreparedInstanceEpoch(0), harkentest::ObjectId(123u), 456u, 3u, 1u));

    executor.Cancel();
    EXPECT_EQ(instances.LiveCount(), 0u);
    EXPECT_EQ(instances.FreeCount(), instances.Capacity());
    EXPECT_EQ(containers.LiveCount(), 0u);
    EXPECT_EQ(containers.FreeCount(), containers.Capacity());
}

TEST(PlayableTreeExecutor, OwnsAdoptedActionTokenLifecycle)
{
    harken::PlayableInstanceTable instances(2);
    harken::PlayableTreeExecutor executor(1);
    harken::PreparedPlayableInstance prepared{};
    ASSERT_TRUE(instances.PrepareAcquire(
        harken::PlayableKind::Sound, 0, &prepared));
    ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &prepared));
    harken::PlayableRootMetadata metadata{};
    metadata.definitionKind = harken::PlayableKind::Sound;
    ASSERT_TRUE(executor.SetPreparedRootMetadata(instances, metadata));
    ASSERT_TRUE(executor.Commit(instances));
    EXPECT_EQ(instances.LiveCount(), 1u);

    harken::PreparedPlayableInstance second{};
    ASSERT_TRUE(instances.PrepareAcquire(
        harken::PlayableKind::Sound, 0, &second));
    EXPECT_FALSE(executor.AdoptPreparedInstance(instances, &second));
    ASSERT_TRUE(instances.CancelPrepared(&second));
    EXPECT_FALSE(executor.Cancel(instances));
    EXPECT_EQ(instances.LiveCount(), 1u);
    EXPECT_EQ(instances.FreeCount(), 1u);
}

TEST(PlayableTreeExecutor, AdoptedCommitRejectsInvalidatedToken)
{
    harken::PlayableInstanceTable instances(1);
    harken::PlayableTreeExecutor executor(1);
    harken::PreparedPlayableInstance prepared{};
    ASSERT_TRUE(instances.PrepareAcquire(
        harken::PlayableKind::Sound, 0, &prepared));
    ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &prepared));
    ASSERT_TRUE(instances.CancelPrepared(&prepared));
    EXPECT_FALSE(executor.Validate());
    EXPECT_FALSE(executor.Commit(instances));
    EXPECT_EQ(instances.LiveCount(), 0u);
}

TEST(PreparedPlayableTreeStep, RejectsChildWhosePreparedParentWasCancelled)
{
    harken::PlayableInstanceTable instances(2);
    harken::PreparedPlayableInstance parent{};
    ASSERT_TRUE(instances.PrepareAcquire(
        harken::PlayableKind::Container, 0, &parent));
    harken::PreparedPlayableTreeStep step(1);
    ASSERT_TRUE(step.PrepareInstance(
        instances, harken::PlayableKind::Sound, parent.Handle()));
    ASSERT_TRUE(instances.CancelPrepared(&parent));
    EXPECT_FALSE(step.Validate());
    step.Cancel();
    EXPECT_EQ(instances.FreeCount(), instances.Capacity());
}

TEST(PlayableTreeExecutor, CancelReclaimsPrecommittedAdoptedRoot)
{
    harken::PlayableInstanceTable instances(1);
    harken::PlayableTreeExecutor executor(1);
    harken::PreparedPlayableInstance prepared{};
    ASSERT_TRUE(instances.PrepareAcquire(
        harken::PlayableKind::Sound, 0, &prepared));
    ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &prepared));
    ASSERT_TRUE(instances.CommitPrepared(&prepared));
    EXPECT_TRUE(executor.Cancel(instances));
    harken::PreparedPlayableInstance replacement{};
    ASSERT_TRUE(instances.PrepareAcquire(
        harken::PlayableKind::Sound, 0, &replacement));
    ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &replacement));
    EXPECT_TRUE(executor.Cancel(instances));
    EXPECT_EQ(instances.Capacity(), instances.FreeCount());
}

TEST(PlayableTreeExecutor, ExternalRootRollbackFailureRetainsOwnership)
{
    PlayableInstanceTable instances(2);
    harken::PlayableTreeExecutor executor(1);
    harken::PreparedPlayableInstance root{};
    ASSERT_TRUE(instances.PrepareAcquire(
        harken::PlayableKind::Container, 0, &root));
    ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &root));
    ASSERT_TRUE(executor.Commit(instances));

    // An independently committed child keeps the external root from being
    // rolled back.  The failed attempt must retain the executor token so a
    // later retry can reclaim the root after the child retires.
    const auto child = instances.Acquire(harken::PlayableKind::Sound,
                                         root.Handle());
    ASSERT_NE(0u, child);
    EXPECT_FALSE(executor.RollbackCommitted(instances));
    EXPECT_TRUE(executor.Committed());
    EXPECT_EQ(2u, instances.LiveCount());

    RetireFully(instances, child);
    EXPECT_TRUE(executor.RollbackCommitted(instances));
    EXPECT_FALSE(executor.Committed());
    EXPECT_EQ(0u, instances.LiveCount());
    EXPECT_EQ(instances.Capacity(), instances.FreeCount());
}

TEST(PlayableTreeExecutor,
     PrecommittedExternalRootRollbackRetainsOwnershipUntilChildRetires)
{
    PlayableInstanceTable instances(3);
    harken::PlayableTreeExecutor executor(1);
    harken::PreparedPlayableInstance root{};
    ASSERT_TRUE(instances.PrepareAcquire(
        harken::PlayableKind::Container, 0, &root));
    ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &root));
    ASSERT_TRUE(instances.CommitPrepared(&root, false));
    const auto child = instances.Acquire(
        harken::PlayableKind::Sound, root.Handle());
    ASSERT_NE(0u, child);

    EXPECT_FALSE(executor.RollbackCommitted(instances));
    EXPECT_TRUE(executor.Committed());
    harken::PreparedPlayableInstance replacement{};
    ASSERT_TRUE(instances.PrepareAcquire(
        harken::PlayableKind::Sound, 0, &replacement));
    EXPECT_FALSE(executor.AdoptPreparedInstance(instances, &replacement));
    ASSERT_TRUE(instances.CancelPrepared(&replacement));
    EXPECT_EQ(2u, instances.LiveCount());

    RetireFully(instances, child);
    EXPECT_TRUE(executor.RollbackCommitted(instances));
    EXPECT_FALSE(executor.Committed());
    EXPECT_EQ(0u, instances.LiveCount());
    EXPECT_EQ(instances.Capacity(), instances.FreeCount());
}

TEST(PlayableTreeExecutor,
     CancelRecognizesPrecommittedExternalRootUntilChildRetires)
{
    PlayableInstanceTable instances(2);
    harken::PlayableTreeExecutor executor(1);
    harken::PreparedPlayableInstance root{};
    ASSERT_TRUE(instances.PrepareAcquire(
        harken::PlayableKind::Container, 0, &root));
    ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &root));
    ASSERT_TRUE(instances.CommitPrepared(&root, false));
    const auto child = instances.Acquire(
        harken::PlayableKind::Sound, root.Handle());
    ASSERT_NE(0u, child);

    EXPECT_FALSE(executor.Cancel(instances));
    EXPECT_TRUE(executor.Committed());
    EXPECT_EQ(2u, instances.LiveCount());

    RetireFully(instances, child);
    EXPECT_TRUE(executor.Cancel(instances));
    EXPECT_FALSE(executor.Committed());
    EXPECT_EQ(0u, instances.LiveCount());
    EXPECT_EQ(instances.Capacity(), instances.FreeCount());
}

TEST(PlayableTreeExecutor,
     RejectsForeignExternalTokenWithoutMutatingSameHandleTable)
{
    PlayableInstanceTable local(1);
    PlayableInstanceTable foreign(1);
    harken::PreparedPlayableInstance localRoot{};
    harken::PreparedPlayableInstance foreignRoot{};
    ASSERT_TRUE(local.PrepareAcquire(
        harken::PlayableKind::Container, 0, &localRoot));
    ASSERT_TRUE(foreign.PrepareAcquire(
        harken::PlayableKind::Sound, 0, &foreignRoot));
    ASSERT_EQ(localRoot.Handle(), foreignRoot.Handle());
    ASSERT_TRUE(local.CommitPrepared(&localRoot, false));

    harken::PlayableTreeExecutor executor(1);
    EXPECT_FALSE(executor.AdoptPreparedInstance(local, &foreignRoot));
    ASSERT_TRUE(foreign.CommitPrepared(&foreignRoot, false));
    const auto localBefore = local.Classify(localRoot.Handle());
    const auto foreignBefore = foreign.Classify(foreignRoot.Handle());
    const auto localFreeBefore = local.FreeCount();
    const auto foreignFreeBefore = foreign.FreeCount();

    EXPECT_TRUE(executor.Cancel(local));
    const auto localAfter = local.Classify(localRoot.Handle());
    const auto foreignAfter = foreign.Classify(foreignRoot.Handle());
    EXPECT_EQ(localBefore.live, localAfter.live);
    EXPECT_EQ(localBefore.kind, localAfter.kind);
    EXPECT_EQ(localBefore.lifecycle, localAfter.lifecycle);
    EXPECT_EQ(localBefore.scheduleEpoch, localAfter.scheduleEpoch);
    EXPECT_EQ(foreignBefore.live, foreignAfter.live);
    EXPECT_EQ(foreignBefore.kind, foreignAfter.kind);
    EXPECT_EQ(foreignBefore.lifecycle, foreignAfter.lifecycle);
    EXPECT_EQ(foreignBefore.scheduleEpoch, foreignAfter.scheduleEpoch);
    EXPECT_EQ(1u, local.LiveCount());
    EXPECT_EQ(1u, foreign.LiveCount());
    EXPECT_EQ(localFreeBefore, local.FreeCount());
    EXPECT_EQ(foreignFreeBefore, foreign.FreeCount());

    ASSERT_TRUE(local.RollbackCommitted(localRoot.Handle()));
    ASSERT_TRUE(local.CancelPrepared(&localRoot));
    ASSERT_TRUE(foreign.RollbackCommitted(foreignRoot.Handle()));
    ASSERT_TRUE(foreign.CancelPrepared(&foreignRoot));
}

TEST(PlayableTreeExecutor,
     CommitFailureRetainsBlockedExternalRootForRollbackRetry)
{
    PlayableInstanceTable instances(3);
    harken::PlayableTreeExecutor executor(1);
    harken::PreparedPlayableInstance root{};
    ASSERT_TRUE(instances.PrepareAcquire(
        harken::PlayableKind::Container, 0, &root));
    ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &root));
    ASSERT_TRUE(executor.PrepareInstance(
        instances, harken::PlayableKind::Sound, root.Handle()));
    CommitWindowFailureProbe probe{
        &instances, executor.PreparedInstanceToken(0), root.Handle()};
    ASSERT_TRUE(executor.AdoptPreparedOwnerToken(
        &probe, ValidateCommitWindowFailure, CommitCommitWindowFailure,
        CancelCommitWindowFailure));

    EXPECT_FALSE(executor.Commit(instances));
    ASSERT_TRUE(probe.mutated);
    ASSERT_NE(0u, probe.blocker.Handle());
    EXPECT_TRUE(executor.Committed());
    EXPECT_EQ(2u, instances.LiveCount());

    RetireFully(instances, probe.blocker.Handle());
    EXPECT_TRUE(executor.RollbackCommitted(instances));
    EXPECT_FALSE(executor.Committed());
    EXPECT_EQ(0u, instances.LiveCount());
    EXPECT_EQ(instances.Capacity(), instances.FreeCount());
}

TEST(PlayableTreeExecutor,
     CommitFailureRetainsBlockedExternalRootForCancelRetry)
{
    PlayableInstanceTable instances(3);
    harken::PlayableTreeExecutor executor(1);
    harken::PreparedPlayableInstance root{};
    ASSERT_TRUE(instances.PrepareAcquire(
        harken::PlayableKind::Container, 0, &root));
    ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &root));
    ASSERT_TRUE(executor.PrepareInstance(
        instances, harken::PlayableKind::Sound, root.Handle()));
    CommitWindowFailureProbe probe{
        &instances, executor.PreparedInstanceToken(0), root.Handle()};
    ASSERT_TRUE(executor.AdoptPreparedOwnerToken(
        &probe, ValidateCommitWindowFailure, CommitCommitWindowFailure,
        CancelCommitWindowFailure));

    EXPECT_FALSE(executor.Commit(instances));
    ASSERT_TRUE(probe.mutated);
    ASSERT_NE(0u, probe.blocker.Handle());
    EXPECT_TRUE(executor.Committed());
    EXPECT_EQ(2u, instances.LiveCount());

    RetireFully(instances, probe.blocker.Handle());
    EXPECT_TRUE(executor.Cancel(instances));
    EXPECT_FALSE(executor.Committed());
    EXPECT_EQ(0u, instances.LiveCount());
    EXPECT_EQ(instances.Capacity(), instances.FreeCount());
}

TEST(PlayableTreeExecutor, FailedOwnerCommitRetainsExternalRootOwnership)
{
    PlayableInstanceTable instances(2);
    harken::PlayableTreeExecutor executor(1);
    harken::PreparedPlayableInstance root{};
    ASSERT_TRUE(instances.PrepareAcquire(
        harken::PlayableKind::Container, 0, &root));
    ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &root));

    BlockingOwnerProbe owner{&instances, root.Handle()};
    ASSERT_TRUE(executor.AdoptPreparedOwnerToken(
        &owner, ValidateBlockingOwner, CommitBlockingOwner,
        CancelBlockingOwner));

    EXPECT_FALSE(executor.Commit(instances));
    ASSERT_NE(0u, owner.child);
    EXPECT_TRUE(owner.cancelled);
    EXPECT_TRUE(executor.Committed());
    EXPECT_EQ(2u, instances.LiveCount());

    RetireFully(instances, owner.child);
    EXPECT_TRUE(executor.RollbackCommitted(instances));
    EXPECT_FALSE(executor.Committed());
    EXPECT_EQ(0u, instances.LiveCount());
    EXPECT_EQ(instances.Capacity(), instances.FreeCount());
}

TEST(PlayableTreeExecutor, FailedOwnerCancelRetainsTokenUntilDependencyRetires)
{
    PlayableInstanceTable instances(2);
    harken::PlayableTreeExecutor executor(1);
    harken::PreparedPlayableInstance root{};
    ASSERT_TRUE(instances.PrepareAcquire(
        harken::PlayableKind::Container, 0, &root));
    ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &root));

    RetryingOwnerProbe owner{&instances, root.Handle(), 0, false};
    ASSERT_TRUE(executor.AdoptPreparedOwnerToken(
        &owner, ValidateRetryingOwner, CommitRetryingOwner,
        CancelRetryingOwner));

    EXPECT_FALSE(executor.Commit(instances));
    ASSERT_NE(0u, owner.child);
    EXPECT_FALSE(owner.cancelled);
    EXPECT_EQ(1u, executor.OwnerTokenCount());
    EXPECT_TRUE(executor.Committed());
    EXPECT_EQ(2u, instances.LiveCount());
    EXPECT_FALSE(executor.ResetForReuse());
    EXPECT_EQ(1u, executor.OwnerTokenCount());

    RetireFully(instances, owner.child);
    EXPECT_TRUE(executor.RollbackCommitted(instances));
    EXPECT_TRUE(owner.cancelled);
    EXPECT_EQ(0u, executor.OwnerTokenCount());
    EXPECT_FALSE(executor.Committed());
    EXPECT_EQ(0u, instances.LiveCount());
    EXPECT_EQ(instances.Capacity(), instances.FreeCount());
    EXPECT_TRUE(executor.ResetForReuse());
}

TEST(PlayableTreeExecutor, ReusesReservationStorageWithoutRetiringPublishedTree) {
    PlayableInstanceTable instances(4);
    harken::PlayableTreeExecutor executor(2);
    ASSERT_TRUE(executor.PrepareInstance(instances, PlayableKind::Sound));
    auto* const firstSlot = executor.PreparedInstanceToken(0);
    const auto first = executor.PreparedInstanceHandle(0);
    EXPECT_FALSE(executor.ResetForReuse());
    ASSERT_TRUE(executor.Commit());
    EXPECT_FALSE(executor.ResetForReuse());
    ASSERT_TRUE(executor.ReleasePublishedOwnership());
    ASSERT_TRUE(executor.ResetForReuse());
    EXPECT_TRUE(instances.Classify(first).live);
    ASSERT_TRUE(executor.PrepareInstance(instances, PlayableKind::Sound));
    EXPECT_EQ(firstSlot, executor.PreparedInstanceToken(0));
    const auto second = executor.PreparedInstanceHandle(0);
    ASSERT_TRUE(executor.Commit());
    ASSERT_TRUE(executor.ReleasePublishedOwnership());
    ASSERT_TRUE(executor.ResetForReuse());
    EXPECT_EQ(2u, instances.LiveCount());
    RetireFully(instances, second);
    RetireFully(instances, first);
}

TEST(PlayableTreeExecutor, CannotReuseAfterSourceHeldRollbackUntilOriginalRelease) {
    PlayableInstanceTable instances(2);
    harken::PlayableTreeExecutor executor(2);
    ASSERT_TRUE(executor.PrepareInstance(instances, PlayableKind::Container));
    const auto root = executor.PreparedInstanceHandle(0);
    ASSERT_TRUE(executor.PrepareInstance(instances, PlayableKind::Sound, root));
    const auto leaf = executor.PreparedInstanceHandle(1);
    ASSERT_TRUE(executor.Commit());
    ASSERT_TRUE(instances.AcquireSourceRetirementHold(leaf, instances.Classify(leaf).scheduleEpoch));
    EXPECT_FALSE(executor.RollbackCommitted(instances));
    EXPECT_FALSE(executor.ResetForReuse());
    EXPECT_EQ(2u, executor.InstanceReservationCount());
    EXPECT_EQ(2u, instances.LiveCount());
    ASSERT_TRUE(instances.ReleaseSourceRetirementHold(leaf));
    ASSERT_TRUE(executor.RollbackCommitted(instances));
    EXPECT_TRUE(executor.ResetForReuse());
    EXPECT_EQ(0u, instances.LiveCount());
}

TEST(PlayableTreeExecutor, MultipleFailedOwnerCancelsRetainOneTokenEach)
{
    PlayableInstanceTable instances(1);
    harken::PlayableTreeExecutor executor(1);
    ASSERT_TRUE(executor.PrepareInstance(
        instances, harken::PlayableKind::Sound));

    RetainedOwnerProbe first{1, 0, false};
    RetainedOwnerProbe second{1, 0, false};
    RetainedOwnerProbe final{0, 0, true};
    ASSERT_TRUE(executor.AdoptPreparedOwnerToken(
        &first, ValidateRetainedOwner, CommitRetainedOwner,
        CancelRetainedOwner));
    ASSERT_TRUE(executor.AdoptPreparedOwnerToken(
        &second, ValidateRetainedOwner, CommitRetainedOwner,
        CancelRetainedOwner));
    ASSERT_TRUE(executor.AdoptPreparedOwnerToken(
        &final, ValidateRetainedOwner, CommitRetainedOwner,
        CancelRetainedOwner));

    EXPECT_FALSE(executor.Commit());
    EXPECT_EQ(2u, executor.OwnerTokenCount());
    EXPECT_EQ(1u, first.cancelAttempts);
    EXPECT_EQ(1u, second.cancelAttempts);
    EXPECT_EQ(1u, final.cancelAttempts);

    EXPECT_TRUE(executor.RollbackCommitted(instances));
    EXPECT_EQ(0u, executor.OwnerTokenCount());
    EXPECT_EQ(2u, first.cancelAttempts);
    EXPECT_EQ(2u, second.cancelAttempts);
    EXPECT_EQ(1u, final.cancelAttempts);
    EXPECT_EQ(0u, instances.LiveCount());
    EXPECT_EQ(instances.Capacity(), instances.FreeCount());
}

TEST(PlayableTreeExecutor,
     OwnerTokenCapacityAcceptsExactLimitAndRejectsOneMoreWithoutLeak)
{
    PlayableInstanceTable instances(1);
    harken::PlayableTreeExecutor executor(1);
    ASSERT_TRUE(executor.PrepareInstance(
        instances, harken::PlayableKind::Sound));
    std::array<RetainedOwnerProbe,
               harken::PlayableTreeExecutor::kOwnerTokenCapacity> owners{};

    for (auto& owner : owners) {
        ASSERT_TRUE(executor.AdoptPreparedOwnerToken(
            &owner, ValidateRetainedOwner, CommitRetainedOwner,
            CancelRetainedOwner));
    }
    EXPECT_EQ(harken::PlayableTreeExecutor::kOwnerTokenCapacity,
              executor.OwnerTokenCount());

    RetainedOwnerProbe overflow{};
    EXPECT_FALSE(executor.AdoptPreparedOwnerToken(
        &overflow, ValidateRetainedOwner, CommitRetainedOwner,
        CancelRetainedOwner));
    EXPECT_EQ(0u, overflow.cancelAttempts);

    executor.Cancel();
    EXPECT_EQ(0u, executor.OwnerTokenCount());
    EXPECT_EQ(instances.Capacity(), instances.FreeCount());
    for (const auto& owner : owners) {
        EXPECT_EQ(1u, owner.cancelAttempts);
    }
}

TEST(PlayableTreeExecutor, CommitsAdoptedRootWithPreparedChildAndRow)
{
    harken::PlayableInstanceTable instances(2);
    harken::ContainerPlaybackTable containers(1);
    harken::PlayableTreeExecutor executor(2);
    harken::PreparedPlayableInstance root{};
    ASSERT_TRUE(instances.PrepareAcquire(
        harken::PlayableKind::Container, 0, &root));
    ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &root));
    ASSERT_TRUE(executor.PrepareInstance(
        instances, harken::PlayableKind::Sound, root.Handle()));
    const auto child = executor.PreparedInstanceHandle(0);
    ASSERT_TRUE(executor.PrepareContainer(instances, containers, root.Handle(), root.ScheduleEpoch(), harkentest::ObjectId(77u), 99u, 2u, 1u));
    ASSERT_TRUE(executor.Commit(instances));
    EXPECT_NE(0u, child);
    EXPECT_EQ(2u, instances.LiveCount());
    EXPECT_EQ(child, instances.FirstChild(root.Handle()));
    EXPECT_EQ(1u, containers.LiveCount());
}

TEST_P(RetentionExecutorTest, RetainedOwnersRollbackInReverseOrderAndRecoverPartialFailure) {
    std::vector<std::uint32_t> order;
    RetentionContractOwner first{1, &order}, second{2, &order}, appended{3, &order};
    ASSERT_TRUE(Adopt(first));
    ASSERT_TRUE(Adopt(second));
    EXPECT_TRUE(executor.EnablePreparedOwnerRollbackRetention());
    ASSERT_TRUE(Commit());
    ASSERT_TRUE(executor.AdoptCommittedOwnerToken(&appended, CancelRetentionContractOwner));
    EXPECT_FALSE(Commit());
    EXPECT_FALSE(executor.EnablePreparedOwnerRollbackRetention());
    EXPECT_EQ(1u, first.commits);
    EXPECT_EQ(1u, second.commits);
    EXPECT_TRUE(order.empty());
    first.blockedCancel = true;
    EXPECT_FALSE(executor.RollbackCommitted(instances));
    EXPECT_EQ((std::vector<std::uint32_t>{3, 2, 1}), order);
    EXPECT_TRUE(instances.Classify(root).live);
    EXPECT_FALSE(executor.ResetForReuse());
    first.blockedCancel = false;
    ASSERT_TRUE(executor.RollbackCommitted(instances));
    EXPECT_EQ((std::vector<std::uint32_t>{3, 2, 1, 1}), order);
    EXPECT_EQ(1u, second.cancels);
    EXPECT_EQ(1u, appended.cancels);
    EXPECT_EQ(0u, instances.LiveCount());
    EXPECT_EQ(instances.Capacity(), instances.FreeCount());
    EXPECT_TRUE(executor.RollbackCommitted(instances));
    EXPECT_TRUE(executor.ResetForReuse());
}

TEST_P(RetentionExecutorTest, MidCommitFailureRetainsFailedCallbacksAndOriginalTree) {
    std::vector<std::uint32_t> order;
    RetentionContractOwner first{1, &order}, blocked{2, &order}, failure{3, &order}, uncommitted{4, &order};
    blocked.blockedCancel = true;
    failure.failCommit = true;
    ASSERT_TRUE(Adopt(first));
    ASSERT_TRUE(Adopt(blocked));
    ASSERT_TRUE(Adopt(failure));
    ASSERT_TRUE(Adopt(uncommitted));
    EXPECT_FALSE(Commit());
    EXPECT_EQ((std::vector<std::uint32_t>{4, 3, 2, 1}), order);
    EXPECT_EQ(1u, executor.OwnerTokenCount());
    EXPECT_EQ(1u, first.commits);
    EXPECT_EQ(1u, blocked.commits);
    EXPECT_EQ(1u, failure.commits);
    EXPECT_EQ(0u, uncommitted.commits);
    EXPECT_TRUE(instances.Classify(root).live);
    EXPECT_FALSE(executor.ResetForReuse());
    EXPECT_FALSE(Commit());
    EXPECT_EQ(4u, order.size());
    blocked.blockedCancel = false;
    ASSERT_TRUE(executor.RollbackCommitted(instances));
    EXPECT_EQ((std::vector<std::uint32_t>{4, 3, 2, 1, 2}), order);
    EXPECT_EQ(0u, instances.LiveCount());
    EXPECT_EQ(instances.Capacity(), instances.FreeCount());
    EXPECT_TRUE(executor.ResetForReuse());
}

TEST_P(RetentionExecutorTest, TransferDetachesCallbacksAndResetRestoresDefaultMode) {
    RetentionContractOwner prepared{}, appended{}, refused{};
    ASSERT_TRUE(Adopt(prepared));
    ASSERT_TRUE(Commit());
    EXPECT_FALSE(executor.ResetForReuse());
    ASSERT_TRUE(executor.AdoptCommittedOwnerToken(&appended, CancelRetentionContractOwner));
    ASSERT_TRUE(executor.ReleasePublishedOwnership());
    EXPECT_TRUE(executor.ReleasePublishedOwnership());
    EXPECT_FALSE(Commit());
    EXPECT_FALSE(Adopt(refused));
    EXPECT_FALSE(executor.AdoptCommittedOwnerToken(&refused, CancelRetentionContractOwner));
    executor.Cancel();
    EXPECT_FALSE(executor.Cancel(instances));
    EXPECT_FALSE(executor.RollbackCommitted(instances));
    EXPECT_TRUE(instances.Classify(root).live);
    EXPECT_EQ(0u, prepared.cancels);
    EXPECT_EQ(0u, appended.cancels);
    ASSERT_TRUE(executor.ResetForReuse());
    EXPECT_TRUE(instances.Classify(root).live);
    RetentionContractOwner defaultOwner{};
    ASSERT_TRUE(executor.PrepareInstance(instances, PlayableKind::Sound));
    ASSERT_TRUE(Adopt(defaultOwner));
    ASSERT_TRUE(executor.Commit());
    ASSERT_TRUE(executor.RollbackCommitted(instances));
    EXPECT_EQ(1u, defaultOwner.commits);
    EXPECT_EQ(0u, defaultOwner.cancels);
    EXPECT_EQ(0u, prepared.cancels);
    EXPECT_EQ(0u, appended.cancels);
    const auto child = instances.FirstChild(root);
    ASSERT_NE(0u, child);
    RetireFully(instances, child);
    RetireFully(instances, root);
    EXPECT_EQ(0u, instances.LiveCount());
}

TEST_P(RetentionExecutorTest, CombinedCapacityAccepts640AndRejectsPreparedAndCommittedOverflow) {
    constexpr auto capacity = harken::PlayableTreeExecutor::kOwnerTokenCapacity;
    std::array<RetentionContractOwner, capacity> owners{};
    for (auto& owner : owners) ASSERT_TRUE(Adopt(owner));
    RetentionContractOwner overflow{};
    EXPECT_FALSE(Adopt(overflow));
    EXPECT_EQ(0u, overflow.commits);
    ASSERT_TRUE(Commit());
    EXPECT_FALSE(executor.AdoptCommittedOwnerToken(&overflow, CancelRetentionContractOwner));
    ASSERT_TRUE(executor.RollbackCommitted(instances));
    for (const auto& owner : owners) {
        EXPECT_EQ(1u, owner.commits);
        EXPECT_EQ(1u, owner.cancels);
    }
    EXPECT_EQ(0u, overflow.cancels);
    EXPECT_EQ(0u, instances.LiveCount());
}

TEST_P(RetentionExecutorTest, CombinedCapacityRetains639ThenOneExplicitCommittedOwner) {
    constexpr auto capacity = harken::PlayableTreeExecutor::kOwnerTokenCapacity;
    std::array<RetentionContractOwner, capacity - 1u> prepared{};
    for (auto& owner : prepared) ASSERT_TRUE(Adopt(owner));
    ASSERT_TRUE(Commit());
    RetentionContractOwner appended{}, overflow{};
    ASSERT_TRUE(executor.AdoptCommittedOwnerToken(&appended, CancelRetentionContractOwner));
    EXPECT_FALSE(executor.AdoptCommittedOwnerToken(&overflow, CancelRetentionContractOwner));
    ASSERT_TRUE(executor.RollbackCommitted(instances));
    for (const auto& owner : prepared) EXPECT_EQ(1u, owner.cancels);
    EXPECT_EQ(1u, appended.cancels);
    EXPECT_EQ(0u, overflow.cancels);
    EXPECT_EQ(0u, instances.LiveCount());
}

INSTANTIATE_TEST_SUITE_P(OrdinaryAndExternalRoot, RetentionExecutorTest, testing::Bool());

TEST(PlayableTreeExecutor, CommitsAdoptedChildAndContainerRowWithLiveParent)
{
    harken::PlayableInstanceTable instances(3);
    harken::ContainerPlaybackTable containers(1);
    const auto parent = instances.Acquire(harken::PlayableKind::Container, 0);
    ASSERT_NE(0u, parent);

    harken::PreparedPlayableInstance child{};
    ASSERT_TRUE(instances.PrepareAcquire(
        harken::PlayableKind::Container, parent, &child));
    harken::PreparedContainerPlaybackRow row{};
    ASSERT_TRUE(containers.Prepare(instances,
        child.Handle(), child.ScheduleEpoch(), harkentest::ObjectId(77u), 99u, 2u, 1u, &row));

    harken::PlayableTreeExecutor executor(2);
    ASSERT_TRUE(executor.AdoptPreparedInstanceReservation(&child));
    ASSERT_TRUE(executor.AdoptPreparedContainerReservation(&row));
    ASSERT_TRUE(executor.Validate());
    ASSERT_TRUE(executor.Commit());

    EXPECT_EQ(2u, instances.LiveCount());
    EXPECT_EQ(1u, containers.LiveCount());
    EXPECT_EQ(child.Handle(), instances.FirstChild(parent));
    EXPECT_EQ(child.Handle(), row.Instance());
}

TEST(PlayableTreeExecutor, RollsBackMixedInternalAndAdoptedTopology)
{
    harken::PlayableInstanceTable instances(2);
    harken::PlayableTreeExecutor executor(2);
    ASSERT_TRUE(executor.PrepareInstance(
        instances, harken::PlayableKind::Container));
    const auto parent = executor.PreparedInstanceHandle(0);

    harken::PreparedPlayableInstance adoptedChild{};
    ASSERT_TRUE(instances.PrepareAcquire(
        harken::PlayableKind::Sound, parent, &adoptedChild));
    ASSERT_TRUE(executor.AdoptPreparedInstanceReservation(&adoptedChild));
    ASSERT_TRUE(executor.Commit());
    EXPECT_EQ(adoptedChild.Handle(), instances.FirstChild(parent));

    EXPECT_TRUE(executor.RollbackCommitted(instances));
    EXPECT_FALSE(executor.Committed());
    EXPECT_EQ(0u, instances.LiveCount());
    EXPECT_EQ(instances.Capacity(), instances.FreeCount());
}

TEST(PreparedPlayableTreeStep, RollsBackAfterPartialLinkPublication)
{
    harken::PlayableInstanceTable firstTable(2);
    harken::PlayableInstanceTable secondTable(2);
    const auto firstParent =
        firstTable.Acquire(harken::PlayableKind::Container, 0);
    const auto secondParent =
        secondTable.Acquire(harken::PlayableKind::Container, 0);
    ASSERT_NE(0u, firstParent);
    ASSERT_NE(0u, secondParent);

    harken::PreparedPlayableTreeStep step(2);
    ASSERT_TRUE(step.PrepareInstance(
        firstTable, harken::PlayableKind::Sound, firstParent));
    const auto firstChild = step.PreparedInstanceHandle(0);
    ASSERT_TRUE(step.PrepareInstance(
        secondTable, harken::PlayableKind::Sound, secondParent));
    ASSERT_TRUE(step.CommitWithoutPublishingLinks());

    // Invalidate only the second external parent after commit.  The first
    // child link is published before the second child fails, exercising
    // rollback of a partially published tree.
    RetireFully(secondTable, secondParent);
    EXPECT_FALSE(step.PublishPreparedLinks());
    EXPECT_EQ(firstChild, firstTable.FirstChild(firstParent));
    EXPECT_TRUE(step.RollbackCommitted());
    EXPECT_EQ(0u, firstTable.FirstChild(firstParent));
    EXPECT_EQ(1u, firstTable.LiveCount());
    EXPECT_EQ(0u, secondTable.LiveCount());

    RetireFully(firstTable, firstParent);
    EXPECT_EQ(firstTable.Capacity(), firstTable.FreeCount());
    EXPECT_EQ(secondTable.Capacity(), secondTable.FreeCount());
}

TEST(PlayableTreeExecutor, CancelsAdoptedChildAndContainerRowWithoutPublishing)
{
    harken::PlayableInstanceTable instances(2);
    harken::ContainerPlaybackTable containers(1);
    const auto parent = instances.Acquire(harken::PlayableKind::Container, 0);
    ASSERT_NE(0u, parent);

    harken::PreparedPlayableInstance child{};
    ASSERT_TRUE(instances.PrepareAcquire(
        harken::PlayableKind::Container, parent, &child));
    harken::PreparedContainerPlaybackRow row{};
    ASSERT_TRUE(containers.Prepare(instances,
        child.Handle(), child.ScheduleEpoch(), harkentest::ObjectId(77u), 99u, 2u, 1u, &row));

    harken::PlayableTreeExecutor executor(2);
    ASSERT_TRUE(executor.AdoptPreparedInstanceReservation(&child));
    ASSERT_TRUE(executor.AdoptPreparedContainerReservation(&row));
    executor.Cancel();

    EXPECT_EQ(1u, instances.LiveCount());
    EXPECT_EQ(0u, containers.LiveCount());
    EXPECT_EQ(0u, instances.FirstChild(parent));
    EXPECT_EQ(instances.Capacity() - 1u, instances.FreeCount());
}

TEST(PlayableTreeExecutor, FailedCommitCanCancelAdoptedRootAndReuse)
{
    harken::PlayableInstanceTable instances(2);
    harken::PlayableTreeExecutor executor(2);
    harken::PreparedPlayableInstance root{};
    ASSERT_TRUE(instances.PrepareAcquire(
        harken::PlayableKind::Container, 0, &root));
    ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &root));
    ASSERT_TRUE(executor.PrepareInstance(
        instances, harken::PlayableKind::Sound, root.Handle()));
    auto* child = executor.PreparedInstanceToken(0);
    ASSERT_NE(nullptr, child);
    ASSERT_TRUE(instances.CancelPrepared(child));
    EXPECT_FALSE(executor.Commit(instances));
    EXPECT_TRUE(executor.Cancel(instances));
    EXPECT_EQ(0u, instances.LiveCount());
    EXPECT_EQ(instances.Capacity(), instances.FreeCount());

    harken::PreparedPlayableInstance retry{};
    ASSERT_TRUE(instances.PrepareAcquire(
        harken::PlayableKind::Container, 0, &retry));
    ASSERT_TRUE(executor.AdoptPreparedInstance(instances, &retry));
    EXPECT_TRUE(executor.Cancel(instances));
}

TEST(PlayableInstanceTable, IntrusiveChildLinksEnumerateAndUnlinkInOrder)
{
    PlayableInstanceTable table(4);
    const PlayableInstanceHandle parent =
        table.Acquire(PlayableKind::Container, 0);
    const PlayableInstanceHandle first =
        table.Acquire(PlayableKind::Sound, parent);
    const PlayableInstanceHandle second =
        table.Acquire(PlayableKind::Container, parent);
    ASSERT_NE(parent, 0u);
    ASSERT_NE(first, 0u);
    ASSERT_NE(second, 0u);

    EXPECT_EQ(table.FirstChild(parent), first);
    EXPECT_EQ(table.NextSibling(first), second);
    EXPECT_EQ(table.NextSibling(second), 0u);

    RetireFully(table, first);
    EXPECT_EQ(table.FirstChild(parent), second);
    EXPECT_EQ(table.NextSibling(second), 0u);

    RetireFully(table, second);
    EXPECT_EQ(table.FirstChild(parent), 0u);
    RetireFully(table, parent);
}

TEST(PlayableInstanceTable, PreparedChildLinksOnlyOnCommit)
{
    PlayableInstanceTable table(3);
    const PlayableInstanceHandle parent =
        table.Acquire(PlayableKind::Container, 0);
    harken::PreparedPlayableInstance prepared;
    ASSERT_TRUE(
        table.PrepareAcquire(PlayableKind::Container, parent, &prepared));
    const PlayableInstanceHandle child = prepared.Handle();
    ASSERT_NE(child, 0u);
    EXPECT_EQ(table.FirstChild(parent), 0u);
    ASSERT_TRUE(table.CommitPrepared(&prepared));
    EXPECT_EQ(table.FirstChild(parent), child);

    harken::PreparedPlayableInstance cancelled;
    ASSERT_TRUE(
        table.PrepareAcquire(PlayableKind::Sound, parent, &cancelled));
    EXPECT_TRUE(table.CancelPrepared(&cancelled));
    EXPECT_EQ(table.NextSibling(child), 0u);

    RetireFully(table, child);
    RetireFully(table, parent);
}

TEST(PlayableInstanceTable, PreparedCommitHeldFreezesActualInputsAliasesAndSourceOwnership) {
    PlayableInstanceTable table(3);
    harken::PreparedPlayableInstance root, child;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Container, 0, &root));
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, root.Handle(), &child));
    harken::PlayableRootMetadata metadata{};
    metadata.actionFiring = 41;
    metadata.worldGeneration = 17;
    metadata.programSemanticFingerprint = 29;
    ASSERT_TRUE(table.SetPreparedRootMetadata(root, metadata));
    ASSERT_TRUE(table.SetPreparedRootMetadata(child, metadata));
    harken::MonitorContentIdentity content{};
    content.stableId = harkentest::ObjectId(19);
    content.contentRevision = 23;
    ASSERT_TRUE(table.SetPreparedContentIdentity(child, content));
    ASSERT_TRUE(table.AcquireSourceRetirementHold(child.Handle(), child.ScheduleEpoch()));
    const auto rootHandle = root.Handle();
    const auto childHandle = child.Handle();
    PlayableInstanceTable::PreparedCommitProof rootProof, childProof, duplicate;
    ASSERT_TRUE(table.ProvePreparedCommit(root, &rootProof));
    ASSERT_TRUE(table.ProvePreparedCommit(child, &childProof));
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(0u, table.HighWater());
    EXPECT_EQ(1u, table.FreeCount());
    EXPECT_TRUE(rootProof.AssociatedWith(root));
    harken::MonitorControlCause cause{};
    cause.request.key = {17, 51};
    cause.actionFiring = 71;
    cause.executionOrdinal = 3;
    table.RecordControlCause(childHandle, cause);
    table.RecordControlCause(childHandle, {});
    harken::MonitorControlCause capturedCause{};
    ASSERT_TRUE(table.CaptureControlCause(childHandle, &capturedCause));
    EXPECT_EQ(cause.request.key, capturedCause.request.key);
    EXPECT_EQ(cause.actionFiring, capturedCause.actionFiring);
    EXPECT_EQ(cause.executionOrdinal, capturedCause.executionOrdinal);
    EXPECT_TRUE(childProof.AssociatedWith(child));
    auto alias = child;
    auto movedAlias = std::move(root);
    EXPECT_TRUE(alias.Valid());
    EXPECT_TRUE(movedAlias.Valid());
    EXPECT_TRUE(root.Valid());
    EXPECT_FALSE(childProof.AssociatedWith(alias));
    EXPECT_FALSE(table.ProvePreparedCommit(alias, &duplicate));
    child = {};
    root = alias;
    alias = std::move(root);
    EXPECT_EQ(childHandle, child.Handle());
    EXPECT_EQ(rootHandle, root.Handle());
    EXPECT_EQ(childHandle, alias.Handle());
    EXPECT_FALSE(table.PrepareAcquire(PlayableKind::Sound, 0, &child));
    harken::PreparedPlayableInstance third;
    EXPECT_FALSE(table.PrepareAcquire(PlayableKind::Sound, rootHandle, &third));
    EXPECT_FALSE(third.Valid());
    EXPECT_EQ(0u, table.Acquire(PlayableKind::Sound, rootHandle));
    EXPECT_EQ(1u, table.FreeCount());
    EXPECT_FALSE(table.SetPreparedRootMetadata(child, {}));
    EXPECT_FALSE(table.SetPreparedContentIdentity(child, {}));
    EXPECT_FALSE(table.SetPreparedContentIdentity(alias, {}));
    harken::MonitorContentIdentity capturedContent{};
    ASSERT_TRUE(table.CaptureContentIdentity(childHandle, &capturedContent));
    EXPECT_EQ(content.stableId, capturedContent.stableId);
    EXPECT_EQ(content.contentRevision, capturedContent.contentRevision);
    EXPECT_FALSE(table.CommitPrepared(&root));
    EXPECT_FALSE(table.CancelPrepared(&root));
    EXPECT_FALSE(table.CancelPrepared(&alias));
    EXPECT_FALSE(table.ReleaseSourceRetirementHold(childHandle));
    EXPECT_FALSE(table.AcquireSourceRetirementHold(childHandle, child.ScheduleEpoch()));
    harken::PreparedPlayableCancellation cancellation;
    harken::DelayedPlayableCancellation delayed;
    EXPECT_FALSE(table.PrepareCancellation(rootHandle, 51, 1, &cancellation));
    EXPECT_FALSE(table.PrepareDelayedCancellation(rootHandle, 51, 1, &delayed));
    EXPECT_FALSE(table.SetTerminalOutcome(childHandle,
        {PlayableTerminalKind::RequestedStop, 0, 0}));
    EXPECT_EQ(0u, table.BumpScheduleEpoch(rootHandle));
    EXPECT_FALSE(table.AdvanceLifecycle(rootHandle, PlayableLifecycle::Retired));
    harken::PlayableRootMetadata actual{};
    ASSERT_TRUE(table.CaptureRootMetadata(rootHandle, &actual));
    EXPECT_EQ(metadata.programSemanticFingerprint, actual.programSemanticFingerprint);
    EXPECT_TRUE(table.HasSourceRetirementHold(childHandle));
    childProof.Release();
    EXPECT_FALSE(table.CancelPrepared(&child));
    ASSERT_TRUE(table.ProvePreparedCommit(child, &childProof));
    PlayableInstanceTable::ConsumePreparedCommit(std::move(rootProof));
    EXPECT_FALSE(rootProof.Valid());
    EXPECT_FALSE(root.Valid());
    EXPECT_EQ(1u, table.LiveCount());
    EXPECT_FALSE(table.HasPendingStartedObservations());
    EXPECT_FALSE(table.PublishPreparedLink(childHandle));
    EXPECT_EQ(0u, table.FirstChild(rootHandle));
    PlayableInstanceTable::ConsumePreparedCommit(std::move(childProof));
    EXPECT_TRUE(table.HasPendingStartedObservations());
    ASSERT_TRUE(table.PublishPreparedLink(childHandle));
    EXPECT_EQ(childHandle, table.FirstChild(rootHandle));
    EXPECT_EQ(2u, table.LiveCount());
    EXPECT_EQ(2u, table.HighWater());
    ASSERT_TRUE(table.CaptureControlCause(childHandle, &capturedCause));
    EXPECT_EQ(cause.request.key, capturedCause.request.key);
    EXPECT_EQ(cause.actionFiring, capturedCause.actionFiring);
    EXPECT_EQ(cause.executionOrdinal, capturedCause.executionOrdinal);
    EXPECT_FALSE(table.RollbackCommitted(childHandle));
    EXPECT_FALSE(table.AdvanceLifecycle(childHandle, PlayableLifecycle::Retired));
    ASSERT_TRUE(table.ReleaseSourceRetirementHold(childHandle));
    RetireFully(table, childHandle);
    RetireFully(table, rootHandle);
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(3u, table.FreeCount());
}

TEST(PlayableInstanceTable, PreparedCommitAcceptsRealRetiredReuseButConsumedGrantDoesNotReawaken) {
    PlayableInstanceTable table(1);
    const auto first = table.Acquire(PlayableKind::Sound, 0);
    ASSERT_NE(0u, first);
    RetireFully(table, first);
    harken::PreparedPlayableInstance prepared;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Container, 0, &prepared));
    const auto handle = prepared.Handle();
    EXPECT_NE(first, handle);
    PlayableInstanceTable::PreparedCommitProof proof;
    ASSERT_TRUE(table.ProvePreparedCommit(prepared, &proof));
    proof.Release();
    ASSERT_TRUE(table.ProvePreparedCommit(prepared, &proof));
    PlayableInstanceTable::ConsumePreparedCommit(std::move(proof));
    EXPECT_EQ(PlayableLifecycle::Active, table.Classify(handle).lifecycle);
    ASSERT_TRUE(table.RollbackCommitted(handle));
    EXPECT_TRUE(prepared.Valid());
    EXPECT_FALSE(table.ProvePreparedCommit(prepared, &proof));
    ASSERT_TRUE(table.CommitPrepared(&prepared, false));
    ASSERT_TRUE(table.RollbackCommitted(handle));
    ASSERT_TRUE(table.CancelPrepared(&prepared));
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, 0, &prepared));
    EXPECT_NE(handle, prepared.Handle());
    ASSERT_TRUE(table.ProvePreparedCommit(prepared, &proof));
    proof.Release();
    ASSERT_TRUE(table.CancelPrepared(&prepared));
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(1u, table.FreeCount());
}

TEST(PlayableInstanceTable, PreparedCommitRejectsChangedOriginalInputsAndReleasesGenuinePrefix) {
    PlayableInstanceTable table(2), foreign(1);
    harken::PreparedPlayableInstance first, second;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, 0, &first));
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, 0, &second));
    harken::PlayableRootMetadata metadata{};
    metadata.actionFiring = 41;
    metadata.programSemanticFingerprint = 17;
    ASSERT_TRUE(table.SetPreparedRootMetadata(second, metadata));
    ASSERT_TRUE(table.AcquireSourceRetirementHold(second.Handle(), second.ScheduleEpoch()));
    for (unsigned field = 0; field < 20; ++field) {
        SCOPED_TRACE(field);
        const auto saved = harken::detail::PlayableInstancePreparationTestAccess::CorruptPreparing(
            table, second, field);
        PlayableInstanceTable::PreparedCommitProof proof;
        EXPECT_FALSE(table.ProvePreparedCommit(second, &proof));
        EXPECT_FALSE(proof.Valid());
        harken::detail::PlayableInstancePreparationTestAccess::RestorePreparing(table, second, saved);
    }
    PlayableInstanceTable::PreparedCommitProof prefix, tail;
    EXPECT_FALSE(foreign.ProvePreparedCommit(second, &tail));
    ASSERT_TRUE(table.ProvePreparedCommit(first, &prefix));
    const auto saved = harken::detail::PlayableInstancePreparationTestAccess::CorruptPreparing(
        table, second, 7);
    EXPECT_FALSE(table.ProvePreparedCommit(second, &tail));
    prefix.Release();
    EXPECT_FALSE(table.PreparedCommitHeld(first));
    EXPECT_TRUE(first.Valid());
    harken::detail::PlayableInstancePreparationTestAccess::RestorePreparing(table, second, saved);
    ASSERT_TRUE(table.ProvePreparedCommit(second, &tail));
    PlayableInstanceTable::PreparedCommitProof transferred(std::move(tail));
    EXPECT_FALSE(tail.Valid());
    EXPECT_TRUE(transferred.AssociatedWith(second));
    transferred.Release();
    ASSERT_TRUE(table.ReleaseSourceRetirementHold(second.Handle()));
    ASSERT_TRUE(table.CancelPrepared(&second));
    ASSERT_TRUE(table.CancelPrepared(&first));
    EXPECT_EQ(2u, table.FreeCount());
    EXPECT_EQ(0u, table.LiveCount());
}

TEST(PlayableInstanceTable, PreparedCancellationDisposalClosesRawHandleReuseAndTaggedMaximum) {
    PlayableInstanceTable table(1);
    harken::PreparedPlayableInstance prepared;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, 0, &prepared));
    const auto stale = prepared;
    const auto staleHandle = prepared.Handle();
    ASSERT_TRUE(table.CancelPrepared(&prepared));
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, 0, &prepared));
    EXPECT_NE(staleHandle, prepared.Handle());
    ASSERT_TRUE(table.AcquireSourceRetirementHold(prepared.Handle(), prepared.ScheduleEpoch()));
    EXPECT_FALSE(table.ReleaseSourceRetirementHold(staleHandle));
    EXPECT_FALSE(table.AcquireSourceRetirementHold(staleHandle, prepared.ScheduleEpoch()));
    auto staleCap = stale;
    EXPECT_FALSE(table.CancelPrepared(&staleCap));
    EXPECT_TRUE(table.HasSourceRetirementHold(prepared.Handle()));
    ASSERT_TRUE(table.ReleaseSourceRetirementHold(prepared.Handle()));
    ASSERT_TRUE(table.CancelPrepared(&prepared));
    harken::detail::PlayableInstanceTableTestAccess::SeedGeneration(table, 0, 0x7fffffff);
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, 0, &prepared));
    PlayableInstanceTable::PreparedCommitProof proof;
    ASSERT_TRUE(table.ProvePreparedCommit(prepared, &proof));
    EXPECT_FALSE(table.CancelPrepared(&prepared));
    EXPECT_EQ(0u, table.QuarantinedCount());
    proof.Release();
    ASSERT_TRUE(table.CancelPrepared(&prepared));
    EXPECT_FALSE(table.CancelPrepared(&prepared));
    EXPECT_EQ(1u, table.QuarantinedCount());
    EXPECT_EQ(0u, table.FreeCount());
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_FALSE(table.PrepareAcquire(PlayableKind::Sound, 0, &prepared));
}

TEST(PlayableInstanceTable, PreparedCancellationDisposalReleasesOnlyActualUnissuedClaim) {
    PlayableInstanceTable table(2);
    harken::PreparedPlayableInstance immediateRoot, delayedRoot;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Container, 0, &immediateRoot));
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Container, 0, &delayedRoot));
    harken::PlayableRootMetadata metadata{};
    metadata.actionFiring = 41;
    ASSERT_TRUE(table.SetPreparedRootMetadata(immediateRoot, metadata));
    ASSERT_TRUE(table.SetPreparedRootMetadata(delayedRoot, metadata));
    harken::PreparedPlayableCancellation immediate;
    ASSERT_TRUE(table.PrepareCancellation(immediateRoot.Handle(), 51, 1, &immediate));
    harken::DelayedPlayableCancellation delayed;
    ASSERT_TRUE(table.PrepareDelayedCancellation(delayedRoot.Handle(), 51, 1, &delayed));
    EXPECT_EQ(1u, harken::detail::PlayableInstanceTableTestAccess::PreparedCancellationCount(table));
    ASSERT_TRUE(table.CancelPrepared(&delayedRoot));
    EXPECT_FALSE(delayed.Valid());
    EXPECT_EQ(1u, harken::detail::PlayableInstanceTableTestAccess::PreparedCancellationCount(table));
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Container, 0, &delayedRoot));
    ASSERT_TRUE(table.SetPreparedRootMetadata(delayedRoot, metadata));
    EXPECT_FALSE(table.CancelDelayedCancellation(&delayed));
    harken::DelayedPlayableCancellation accepted;
    ASSERT_TRUE(table.PrepareDelayedCancellation(delayedRoot.Handle(), 51, 1, &accepted));
    ASSERT_TRUE(table.CommitPreparedDelayedCancellation(&accepted));
    ASSERT_TRUE(table.CancelPrepared(&delayedRoot));
    EXPECT_FALSE(accepted.Valid());
    EXPECT_EQ(1u, harken::detail::PlayableInstanceTableTestAccess::PreparedCancellationCount(table));
    ASSERT_TRUE(table.CancelPrepared(&immediateRoot));
    EXPECT_FALSE(immediate.Valid());
    EXPECT_EQ(0u, harken::detail::PlayableInstanceTableTestAccess::PreparedCancellationCount(table));
    harken::detail::PlayableInstanceTableTestAccess::SeedNextScheduleEpoch(table, UINT64_MAX - 2);
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Container, 0, &immediateRoot));
    ASSERT_TRUE(table.SetPreparedRootMetadata(immediateRoot, metadata));
    ASSERT_TRUE(table.PrepareCancellation(immediateRoot.Handle(), 51, 1, &immediate));
    ASSERT_TRUE(table.CancelPrepared(&immediateRoot));
    EXPECT_EQ(0u, harken::detail::PlayableInstanceTableTestAccess::PreparedCancellationCount(table));
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Container, 0, &immediateRoot));
    ASSERT_TRUE(table.SetPreparedRootMetadata(immediateRoot, metadata));
    EXPECT_FALSE(table.CancelPreparedCancellation(&immediate));
    ASSERT_TRUE(table.PrepareCancellation(immediateRoot.Handle(), 51, 1, &immediate));
    ASSERT_TRUE(table.CancelPreparedCancellation(&immediate));
    EXPECT_EQ(0u, harken::detail::PlayableInstanceTableTestAccess::PreparedCancellationCount(table));
    ASSERT_TRUE(table.CancelPrepared(&immediateRoot));
    const auto final = table.Acquire(PlayableKind::Container, 0);
    ASSERT_NE(0u, final);
    EXPECT_TRUE(table.ScheduleEpochExhausted());
    RetireFully(table, final);
    EXPECT_EQ(2u, table.FreeCount());
}

TEST(PlayableInstanceTable, TerminalOutcomeIsPublishedOnceAndRemainsExact)
{
    PlayableInstanceTable table(1);
    const PlayableInstanceHandle handle =
        table.Acquire(PlayableKind::Container, 0);
    ASSERT_NE(handle, 0u);
    const PlayableTerminalOutcome natural{
        PlayableTerminalKind::NaturalCompletion, -301, 17};
    EXPECT_TRUE(table.SetTerminalOutcome(handle, natural));
    EXPECT_FALSE(table.SetTerminalOutcome(
        handle, {PlayableTerminalKind::RequestedStop, -302, 18}));
    EXPECT_FALSE(table.SetTerminalOutcome(
        handle, {PlayableTerminalKind::None, 0, 0}));

    const auto view = table.Classify(handle);
    EXPECT_EQ(view.terminalOutcome.kind,
              PlayableTerminalKind::NaturalCompletion);
    EXPECT_EQ(view.terminalOutcome.resultCode, -301);
    EXPECT_EQ(view.terminalOutcome.reasonCode, 17u);
    EXPECT_EQ(view.terminalOutcome.naturalCompletionOrdinal, 0u);
}

TEST(PlayableInstanceTable, RefusesRetirementWhileAChildIsStillLive)
{
    PlayableInstanceTable table(2);
    const PlayableInstanceHandle parent =
        table.Acquire(PlayableKind::Container, 0);
    const PlayableInstanceHandle child =
        table.Acquire(PlayableKind::Sound, parent);
    ASSERT_NE(child, 0u);
    table.AdvanceLifecycle(parent, PlayableLifecycle::LogicalComplete);
    table.AdvanceLifecycle(parent, PlayableLifecycle::AudibleRetiring);
    EXPECT_FALSE(table.AdvanceLifecycle(parent, PlayableLifecycle::Retired));
    EXPECT_TRUE(table.Classify(parent).live);

    RetireFully(table, child);
    EXPECT_TRUE(table.AdvanceLifecycle(parent, PlayableLifecycle::Retired));
}

// The load-bearing property of a generational handle: after the instance ends,
// the handle names nobody. Without the generation move at retirement, this same
// handle would address whichever instance took the slot next.
TEST(PlayableInstanceTable, HandleGoesStaleTheMomentTheInstanceRetires)
{
    PlayableInstanceTable table(1);
    const PlayableInstanceHandle first = table.Acquire(PlayableKind::Sound, 0);
    ASSERT_NE(first, 0u);
    RetireFully(table, first);

    EXPECT_FALSE(table.Classify(first).live);

    const PlayableInstanceHandle second = table.Acquire(PlayableKind::Sound, 0);
    ASSERT_NE(second, 0u);
    EXPECT_NE(second, first);
    EXPECT_TRUE(table.Classify(second).live);
    // The old handle must not have been revived by the reuse.
    EXPECT_FALSE(table.Classify(first).live);
}

// Retirement is the only exit, and it is one step rather than two. If a second
// release call were required, forgetting it would leak the slot; if the slot
// were freed without moving the generation, the stale handle above would resolve.
TEST(PlayableInstanceTable, RetirementBothEndsIdentityAndFreesTheSlot)
{
    PlayableInstanceTable table(1);
    const PlayableInstanceHandle handle = table.Acquire(PlayableKind::Sound, 0);
    ASSERT_NE(handle, 0u);
    EXPECT_EQ(table.LiveCount(), 1u);
    // A full pool refuses rather than overwriting a live record.
    EXPECT_EQ(table.Acquire(PlayableKind::Sound, 0), 0u);

    RetireFully(table, handle);
    EXPECT_EQ(table.LiveCount(), 0u);
    EXPECT_NE(table.Acquire(PlayableKind::Sound, 0), 0u);
}

// "No longer audible" must not be a reversible claim, so the stage order is
// enforced by the table rather than by convention at each call site.
TEST(PlayableInstanceTable, RefusesLifecycleMovesThatAreNotStrictlyForward)
{
    PlayableInstanceTable table(2);
    const PlayableInstanceHandle handle = table.Acquire(PlayableKind::Segment, 0);
    ASSERT_NE(handle, 0u);

    EXPECT_TRUE(
        table.AdvanceLifecycle(handle, PlayableLifecycle::AudibleRetiring));
    // Backwards to a stage it already passed.
    EXPECT_FALSE(
        table.AdvanceLifecycle(handle, PlayableLifecycle::LogicalComplete));
    // Sideways to the stage it is in.
    EXPECT_FALSE(
        table.AdvanceLifecycle(handle, PlayableLifecycle::AudibleRetiring));
    // Free is not a destination; the slot leaves the live set through Retired.
    EXPECT_FALSE(table.AdvanceLifecycle(handle, PlayableLifecycle::Free));
    EXPECT_EQ(table.Classify(handle).lifecycle,
              PlayableLifecycle::AudibleRetiring);
}

// An instance parented to a handle that has already gone would outlive its
// tree. Refusing at acquire is what keeps the parent edge meaningful, because
// nothing later re-checks it.
TEST(PlayableInstanceTable, RefusesToParentOntoAStaleInstance)
{
    PlayableInstanceTable table(4);
    const PlayableInstanceHandle parent = table.Acquire(PlayableKind::Segment, 0);
    ASSERT_NE(parent, 0u);
    RetireFully(table, parent);

    EXPECT_EQ(table.Acquire(PlayableKind::Track, parent), 0u);
}

// Cancellation has to take effect on records the canceller never sees. Bumping
// the epoch is what makes an already-issued stamp fail on arrival.
TEST(PlayableInstanceTable, CancellationInvalidatesStampsIssuedBeforeIt)
{
    PlayableInstanceTable table(2);
    const PlayableInstanceHandle handle = table.Acquire(PlayableKind::Track, 0);
    ASSERT_NE(handle, 0u);
    const std::uint64_t issued = table.Classify(handle).scheduleEpoch;
    EXPECT_TRUE(table.ValidateStamp(handle, issued));

    const std::uint64_t bumped = table.BumpScheduleEpoch(handle);
    EXPECT_NE(bumped, 0u);
    EXPECT_NE(bumped, issued);
    EXPECT_FALSE(table.ValidateStamp(handle, issued));
    EXPECT_TRUE(table.ValidateStamp(handle, bumped));
}

TEST(PlayableInstanceTable, DelayedCancellationPreissuesLastEpochWithoutCancellingPlayback) {
    PlayableInstanceTable table(1);
    const auto root = AcquireActionRoot(table);
    ASSERT_NE(0u, root);
    const auto before = table.Classify(root);
    harken::detail::PlayableInstanceTableTestAccess::SeedNextScheduleEpoch(table, UINT64_MAX);
    harken::DelayedPlayableCancellation prepared;
    ASSERT_TRUE(table.PrepareDelayedCancellation(root, 51, 2, &prepared));
    EXPECT_EQ(before.scheduleEpoch, table.Classify(root).scheduleEpoch);
    EXPECT_EQ(before.lifecycle, table.Classify(root).lifecycle);
    EXPECT_TRUE(table.ScheduleEpochExhausted());
    EXPECT_TRUE(table.HasCancellationClaim(root));
    harken::PlayableRetirementObserver observer{};
    EXPECT_FALSE(table.CaptureRetirementObserver(root, &observer));
    harken::DelayedPlayableCancellation accepted(std::move(prepared));
    EXPECT_FALSE(prepared.Valid());
    ASSERT_TRUE(table.CommitPreparedDelayedCancellation(&accepted));
    EXPECT_FALSE(table.CommitPreparedDelayedCancellation(&accepted));
    EXPECT_EQ(before.scheduleEpoch, table.Classify(root).scheduleEpoch);
    ASSERT_TRUE(table.CaptureRetirementObserver(root, &observer));
    EXPECT_EQ(51u, observer.actionFiring);
    EXPECT_EQ(2u, observer.actionExecutionOrdinal);
    ASSERT_TRUE(table.ActivateDelayedCancellation(&accepted));
    EXPECT_EQ(UINT64_MAX, table.Classify(root).scheduleEpoch);
    EXPECT_FALSE(accepted.Valid());
    EXPECT_FALSE(table.ActivateDelayedCancellation(&accepted));
    EXPECT_TRUE(table.TakeRetirementObserver(root, &observer));
    EXPECT_FALSE(table.TakeRetirementObserver(root, &observer));
}

TEST(PlayableInstanceTable, DelayedCancellationPreservesExistingLastEpochReservation) {
    PlayableInstanceTable table(3);
    const auto first = AcquireActionRoot(table);
    const auto second = AcquireActionRoot(table);
    const auto third = AcquireActionRoot(table);
    ASSERT_NE(0u, third);
    harken::detail::PlayableInstanceTableTestAccess::SeedNextScheduleEpoch(table, UINT64_MAX - 1);
    harken::PreparedPlayableCancellation immediate;
    ASSERT_TRUE(table.PrepareCancellation(first, 51, 1, &immediate));
    harken::DelayedPlayableCancellation delayed;
    ASSERT_TRUE(table.PrepareDelayedCancellation(second, 52, 2, &delayed));
    EXPECT_EQ(1u, harken::detail::PlayableInstanceTableTestAccess::PreparedCancellationCount(table));
    const auto before = table.Classify(third);
    harken::DelayedPlayableCancellation refused;
    EXPECT_FALSE(table.PrepareDelayedCancellation(third, 53, 3, &refused));
    EXPECT_FALSE(refused.Valid());
    EXPECT_FALSE(table.HasCancellationClaim(third));
    EXPECT_EQ(before.scheduleEpoch, table.Classify(third).scheduleEpoch);
    EXPECT_EQ(before.lifecycle, table.Classify(third).lifecycle);
    EXPECT_FALSE(table.ScheduleEpochExhausted());
    ASSERT_TRUE(table.CommitPreparedCancellation(&immediate));
    EXPECT_EQ(UINT64_MAX, table.Classify(first).scheduleEpoch);
    EXPECT_EQ(0u, harken::detail::PlayableInstanceTableTestAccess::PreparedCancellationCount(table));
    ASSERT_TRUE(table.CommitPreparedDelayedCancellation(&delayed));
    ASSERT_TRUE(table.ActivateDelayedCancellation(&delayed));
    EXPECT_EQ(UINT64_MAX - 1, table.Classify(second).scheduleEpoch);
    EXPECT_FALSE(table.PrepareDelayedCancellation(third, 53, 3, &refused));
    EXPECT_FALSE(table.HasCancellationClaim(third));
}

TEST(PlayableInstanceTable, DelayedCancellationMoveAndCancelNeverReissueNumbers) {
    PlayableInstanceTable table(2);
    const auto first = AcquireActionRoot(table);
    const auto second = AcquireActionRoot(table);
    ASSERT_NE(0u, second);
    harken::detail::PlayableInstanceTableTestAccess::SeedNextScheduleEpoch(table, 100);
    harken::DelayedPlayableCancellation cancelled;
    ASSERT_TRUE(table.PrepareDelayedCancellation(first, 51, 1, &cancelled));
    ASSERT_TRUE(table.CancelDelayedCancellation(&cancelled));
    EXPECT_FALSE(table.CancelDelayedCancellation(&cancelled));
    harken::DelayedPlayableCancellation next;
    ASSERT_TRUE(table.PrepareDelayedCancellation(first, 51, 1, &next));
    harken::DelayedPlayableCancellation replacement;
    ASSERT_TRUE(table.PrepareDelayedCancellation(second, 52, 2, &replacement));
    replacement = std::move(next);
    EXPECT_FALSE(next.Valid());
    EXPECT_FALSE(table.HasCancellationClaim(second));
    ASSERT_TRUE(table.CommitPreparedDelayedCancellation(&replacement));
    ASSERT_TRUE(table.ActivateDelayedCancellation(&replacement));
    EXPECT_EQ(101u, table.Classify(first).scheduleEpoch);
    EXPECT_EQ(103u, table.BumpScheduleEpoch(second));
    {
        harken::DelayedPlayableCancellation scoped;
        ASSERT_TRUE(table.PrepareDelayedCancellation(second, 52, 2, &scoped));
        ASSERT_TRUE(table.CommitPreparedDelayedCancellation(&scoped));
    }
    EXPECT_FALSE(table.HasCancellationClaim(second));
    EXPECT_EQ(0u, harken::detail::PlayableInstanceTableTestAccess::PreparedCancellationCount(table));
}

TEST(PlayableInstanceTable, DelayedCancellationRejectsStaleSourceObserverAndRootWithoutClearingNewClaim) {
    PlayableInstanceTable table(1);
    auto root = AcquireActionRoot(table);
    ASSERT_NE(0u, root);
    harken::DelayedPlayableCancellation staleSource;
    ASSERT_TRUE(table.PrepareDelayedCancellation(root, 51, 1, &staleSource));
    ASSERT_TRUE(table.CommitPreparedDelayedCancellation(&staleSource));
    const auto externalEpoch = table.BumpScheduleEpoch(root);
    ASSERT_NE(0u, externalEpoch);
    EXPECT_FALSE(table.ActivateDelayedCancellation(&staleSource));
    EXPECT_EQ(externalEpoch, table.Classify(root).scheduleEpoch);
    ASSERT_TRUE(table.CancelDelayedCancellation(&staleSource));

    for (const bool take : {false, true}) {
        harken::DelayedPlayableCancellation staleObserver;
        ASSERT_TRUE(table.PrepareDelayedCancellation(root, 51, 1, &staleObserver));
        ASSERT_TRUE(table.CommitPreparedDelayedCancellation(&staleObserver));
        harken::PlayableRetirementObserver observer{};
        ASSERT_TRUE(table.CaptureRetirementObserver(root, &observer));
        ASSERT_TRUE(take ? table.TakeRetirementObserver(root, &observer)
                         : table.DetachRetirementObserver(root, observer));
        harken::DelayedPlayableCancellation current;
        ASSERT_TRUE(table.PrepareDelayedCancellation(root, 51, 1, &current));
        ASSERT_TRUE(table.CommitPreparedDelayedCancellation(&current));
        EXPECT_FALSE(table.ActivateDelayedCancellation(&staleObserver));
        EXPECT_FALSE(table.CancelDelayedCancellation(&staleObserver));
        EXPECT_TRUE(current.Valid());
        EXPECT_TRUE(table.HasCancellationClaim(root));
        ASSERT_TRUE(table.CancelDelayedCancellation(&current));
    }

    harken::DelayedPlayableCancellation staleRoot;
    ASSERT_TRUE(table.PrepareDelayedCancellation(root, 51, 1, &staleRoot));
    ASSERT_TRUE(table.CommitPreparedDelayedCancellation(&staleRoot));
    RetireFully(table, root);
    root = AcquireActionRoot(table);
    ASSERT_NE(0u, root);
    harken::DelayedPlayableCancellation current;
    ASSERT_TRUE(table.PrepareDelayedCancellation(root, 51, 1, &current));
    EXPECT_FALSE(table.ActivateDelayedCancellation(&staleRoot));
    EXPECT_FALSE(table.CancelDelayedCancellation(&staleRoot));
    EXPECT_TRUE(current.Valid());
}

TEST(PlayableInstanceTable, DelayedCancellationCannotSurvivePreparedRootRollback) {
    PlayableInstanceTable table(1);
    harken::PreparedPlayableInstance root;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Container, 0, &root));
    harken::PlayableRootMetadata metadata{};
    metadata.actionFiring = 41;
    ASSERT_TRUE(table.SetPreparedRootMetadata(root, metadata));
    harken::DelayedPlayableCancellation stale;
    ASSERT_TRUE(table.PrepareDelayedCancellation(root.Handle(), 51, 1, &stale));
    ASSERT_TRUE(table.CommitPreparedDelayedCancellation(&stale));
    EXPECT_FALSE(table.ActivateDelayedCancellation(&stale));
    ASSERT_TRUE(table.CancelPrepared(&root));
    const auto replacement = AcquireActionRoot(table);
    ASSERT_NE(0u, replacement);
    harken::DelayedPlayableCancellation current;
    ASSERT_TRUE(table.PrepareDelayedCancellation(replacement, 51, 1, &current));
    EXPECT_FALSE(table.ActivateDelayedCancellation(&stale));
    EXPECT_FALSE(table.CancelDelayedCancellation(&stale));
    EXPECT_TRUE(current.Valid());
}

TEST(PlayableInstanceTable, DelayedCancellationCannotSurviveCommittedRootRollback) {
    PlayableInstanceTable table(1);
    harken::PreparedPlayableInstance root;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Container, 0, &root));
    harken::PlayableRootMetadata metadata{};
    metadata.actionFiring = 41;
    ASSERT_TRUE(table.SetPreparedRootMetadata(root, metadata));
    ASSERT_TRUE(table.CommitPrepared(&root));
    harken::DelayedPlayableCancellation stale;
    ASSERT_TRUE(table.PrepareDelayedCancellation(root.Handle(), 51, 1, &stale));
    ASSERT_TRUE(table.CommitPreparedDelayedCancellation(&stale));
    const auto sourceEpoch = table.Classify(root.Handle()).scheduleEpoch;
    ASSERT_TRUE(table.RollbackCommitted(root.Handle()));
    EXPECT_FALSE(stale.Valid());
    ASSERT_TRUE(table.CommitPrepared(&root));
    EXPECT_EQ(sourceEpoch, table.Classify(root.Handle()).scheduleEpoch);
    harken::DelayedPlayableCancellation current;
    ASSERT_TRUE(table.PrepareDelayedCancellation(root.Handle(), 51, 1, &current));
    ASSERT_TRUE(table.CommitPreparedDelayedCancellation(&current));
    EXPECT_FALSE(table.ActivateDelayedCancellation(&stale));
    EXPECT_FALSE(table.CancelDelayedCancellation(&stale));
    EXPECT_TRUE(current.Valid());
}

TEST(PlayableInstanceTable, SourceHoldProtectsPreparedAndCancelledGenerations) {
    PlayableInstanceTable table(1);
    harken::PreparedPlayableInstance prepared;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, 0, &prepared));
    const auto leaf = prepared.Handle();
    ASSERT_TRUE(table.AcquireSourceRetirementHold(leaf, prepared.ScheduleEpoch()));
    EXPECT_FALSE(table.AcquireSourceRetirementHold(leaf, prepared.ScheduleEpoch()));
    EXPECT_FALSE(table.CancelPrepared(&prepared));
    EXPECT_EQ(0u, table.FreeCount());
    ASSERT_TRUE(table.CommitPrepared(&prepared));
    EXPECT_FALSE(table.RollbackCommitted(leaf));
    EXPECT_NE(0u, table.BumpScheduleEpoch(leaf));
    EXPECT_FALSE(table.AdvanceLifecycle(leaf, PlayableLifecycle::Retired));
    ASSERT_TRUE(table.ReleaseSourceRetirementHold(leaf));
    ASSERT_TRUE(table.AdvanceLifecycle(leaf, PlayableLifecycle::Retired));
    const auto next = table.Acquire(PlayableKind::Sound, 0);
    ASSERT_NE(0u, next);
    EXPECT_NE(leaf, next);
    EXPECT_FALSE(table.ReleaseSourceRetirementHold(leaf));
}

TEST(PlayableInstanceTable, SourceHoldPreservesAncestorContainerRowsUntilRelease) {
    PlayableInstanceTable instances(3);
    harken::ContainerPlaybackTable containers(2);
    const auto root = instances.Acquire(PlayableKind::Container, 0);
    const auto nested = instances.Acquire(PlayableKind::Container, root);
    const auto leaf = instances.Acquire(PlayableKind::Sound, nested);
    for (const auto container : {root, nested}) {
        harken::PreparedContainerPlaybackRow row;
        ASSERT_TRUE(containers.Prepare(instances, container,
            instances.Classify(container).scheduleEpoch, harkentest::ObjectId(container), 1,
            static_cast<std::uint8_t>(harken::ContainerModeV1::Sequence), 1, &row));
        ASSERT_TRUE(containers.Commit(&row));
    }
    ASSERT_TRUE(instances.AcquireSourceRetirementHold(leaf, instances.Classify(leaf).scheduleEpoch));
    for (unsigned repeat = 0; repeat < 3; ++repeat) {
        EXPECT_FALSE(harken::PlayableTreeExecutor::RetirePlayableTreeBottomUp(
            instances, containers, root, PlayableTerminalKind::RequestedStop));
        for (const auto container : {root, nested}) {
            harken::ContainerPlaybackRow row{};
            EXPECT_TRUE(containers.Capture(container, &row));
            EXPECT_TRUE(instances.Classify(container).live);
        }
        EXPECT_EQ(0u, instances.FreeCount());
    }
    ASSERT_TRUE(instances.ReleaseSourceRetirementHold(leaf));
    EXPECT_TRUE(harken::PlayableTreeExecutor::RetirePlayableTreeBottomUp(
        instances, containers, root, PlayableTerminalKind::RequestedStop));
    EXPECT_EQ(0u, instances.LiveCount());
    EXPECT_EQ(0u, containers.LiveCount());
}

// One check has to cover both failure modes, because a caller that only tested
// liveness would apply a cancelled transaction and one that only tested the
// epoch would apply work to a slot's next occupant.
TEST(PlayableInstanceTable, StampValidationCoversBothStalenessAndCancellation)
{
    PlayableInstanceTable table(1);
    const PlayableInstanceHandle handle = table.Acquire(PlayableKind::Sound, 0);
    ASSERT_NE(handle, 0u);
    const std::uint64_t issued = table.Classify(handle).scheduleEpoch;

    RetireFully(table, handle);
    EXPECT_FALSE(table.ValidateStamp(handle, issued));

    // The next occupant reuses the slot but not the identity, so the earlier
    // stamp stays invalid.
    const PlayableInstanceHandle next = table.Acquire(PlayableKind::Sound, 0);
    ASSERT_NE(next, 0u);
    EXPECT_FALSE(table.ValidateStamp(handle, issued));
}

TEST(PlayableInstanceTable, TracksTheDeepestConcurrentOccupancy)
{
    PlayableInstanceTable table(3);
    const PlayableInstanceHandle a = table.Acquire(PlayableKind::Sound, 0);
    const PlayableInstanceHandle b = table.Acquire(PlayableKind::Sound, 0);
    ASSERT_NE(a, 0u);
    ASSERT_NE(b, 0u);
    EXPECT_EQ(table.HighWater(), 2u);

    RetireFully(table, a);
    RetireFully(table, b);
    // The high-water mark records the peak, not the present.
    EXPECT_EQ(table.LiveCount(), 0u);
    EXPECT_EQ(table.HighWater(), 2u);
}

TEST(PlayableInstanceTable,
     QuarantinesTheSlotWhenGenerationExhaustsInsteadOfWrapping)
{
    constexpr std::uint32_t maximum = 0x7FFFFFFFu;
    PlayableInstanceTable table(1);
    harken::detail::PlayableInstanceTableTestAccess::SeedGeneration(
        table, 0, maximum - 1);

    const PlayableInstanceHandle penultimate =
        table.Acquire(PlayableKind::Sound, 0);
    ASSERT_NE(penultimate, 0u);
    EXPECT_EQ(
        penultimate,
        harken::detail::PlayableInstanceTableTestAccess::Handle(0,
                                                                 maximum - 1));
    RetireFully(table, penultimate);

    // The maximum generation is still available for the slot's final reuse.
    EXPECT_EQ(table.FreeCount(), 1u);
    EXPECT_EQ(table.QuarantinedCount(), 0u);
    const PlayableInstanceHandle maximumHandle =
        table.Acquire(PlayableKind::Sound, 0);
    ASSERT_NE(maximumHandle, 0u);
    EXPECT_EQ(
        maximumHandle,
        harken::detail::PlayableInstanceTableTestAccess::Handle(0, maximum));
    EXPECT_FALSE(table.Classify(penultimate).live);

    RetireFully(table, maximumHandle);

    // Exhaustion quarantines the slot rather than reviving generation one.
    EXPECT_EQ(table.LiveCount(), 0u);
    EXPECT_EQ(table.FreeCount(), 0u);
    EXPECT_EQ(table.QuarantinedCount(), 1u);
    EXPECT_EQ(table.Acquire(PlayableKind::Sound, 0), 0u);
    EXPECT_FALSE(table.Classify(maximumHandle).live);
    EXPECT_FALSE(table.Classify(
                       harken::detail::PlayableInstanceTableTestAccess::Handle(
                           0, 1))
                     .live);
}

TEST(PlayableInstanceTable, ParentLinksProofKeepsOriginalRootAndOrderedChildrenUntilPublication) {
    PlayableInstanceTable table(4);
    std::array<harken::PreparedPlayableInstance, 3> originals;
    std::array<PlayableInstanceTable::PreparedCommitProof, 3> activations;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Container, 0, &originals[0]));
    const auto root = originals[0].Handle();
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, root, &originals[1]));
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, root, &originals[2]));
    const auto first = originals[1].Handle();
    const auto second = originals[2].Handle();
    const auto rootEpoch = originals[0].ScheduleEpoch();
    for (std::size_t index = 0; index < originals.size(); ++index)
        ASSERT_TRUE(table.ProvePreparedCommit(originals[index], &activations[index]));
    const std::array<PlayableInstanceTable::ParentLinkRequest, 2> requests{{
        {&originals[1], &activations[1]}, {&originals[2], &activations[2]}}};
    const std::array<PlayableInstanceTable::ParentLinkRequest, 1> anchors{{
        {&originals[0], &activations[0]}}};
    PlayableInstanceTable::PreparedParentLinksProof links(2);
    ASSERT_TRUE(table.ProvePreparedParentLinks(requests, anchors, &links));
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(0u, table.FirstChild(root));
    EXPECT_TRUE(links.MatchesOriginalScope(&table, requests, anchors));
    std::array<PlayableInstanceTable::PreparedNodeCensusEntry, 3> census{{
        {&originals[2], PlayableKind::Sound}, {&originals[0], PlayableKind::Container},
        {&originals[1], PlayableKind::Sound}}};
    EXPECT_TRUE(table.MatchesPreparedOriginalNodeCensus(links, census));
    EXPECT_FALSE(table.MatchesPreparedOriginalNodeCensus(links, {census.data(), 2}));
    census[0].expectedKind = PlayableKind::Track;
    EXPECT_FALSE(table.MatchesPreparedOriginalNodeCensus(links, census));
    census[0].expectedKind = PlayableKind::Sound;
    const auto savedOriginal = census[0].original;
    census[0].original = census[2].original;
    EXPECT_FALSE(table.MatchesPreparedOriginalNodeCensus(links, census));
    auto copiedOriginal = originals[2];
    census[0].original = &copiedOriginal;
    EXPECT_FALSE(table.MatchesPreparedOriginalNodeCensus(links, census));
    census[0].original = savedOriginal;
    PlayableInstanceTable foreign(4);
    EXPECT_FALSE(foreign.MatchesPreparedOriginalNodeCensus(links, census));
    for (auto& activation : activations)
        PlayableInstanceTable::ConsumePreparedCommit(std::move(activation));
    ASSERT_TRUE(links.Valid());
    EXPECT_TRUE(links.MatchesOriginalScope(&table, requests, anchors));
    EXPECT_FALSE(activations[0].Valid());
    EXPECT_EQ(3u, table.LiveCount());
    EXPECT_EQ(0u, table.FirstChild(root));
    for (const auto handle : {root, first, second}) {
        EXPECT_TRUE(table.ParentLinksHeld(handle));
        EXPECT_FALSE(table.PublishPreparedLink(handle));
        EXPECT_FALSE(table.RollbackCommitted(handle));
        EXPECT_FALSE(table.AdvanceLifecycle(handle, PlayableLifecycle::Retired));
        EXPECT_FALSE(table.CanBumpScheduleEpoch(handle));
        EXPECT_EQ(0u, table.BumpScheduleEpoch(handle));
        EXPECT_FALSE(table.SetTerminalOutcome(handle, {PlayableTerminalKind::RequestedStop}));
    }
    EXPECT_FALSE(table.WithdrawUnpublishedParentLink(originals[1], root, rootEpoch));
    harken::PreparedPlayableInstance rejected;
    EXPECT_FALSE(table.PrepareAcquire(PlayableKind::Sound, root, &rejected));
    EXPECT_EQ(0u, table.Acquire(PlayableKind::Sound, root));
    originals[1] = {};
    EXPECT_EQ(first, originals[1].Handle());
    EXPECT_FALSE(table.PrepareAcquire(PlayableKind::Sound, 0, &originals[1]));
    EXPECT_TRUE(links.MatchesOriginalScope(&table, requests, anchors));
    EXPECT_EQ(1u, table.FreeCount());
    PlayableInstanceTable::ConsumePreparedParentLinks(std::move(links));
    EXPECT_FALSE(links.Valid());
    EXPECT_FALSE(table.MatchesPreparedOriginalNodeCensus(links, census));
    EXPECT_EQ(first, table.FirstChild(root));
    EXPECT_EQ(second, table.NextSibling(first));
    EXPECT_EQ(0u, table.NextSibling(second));
    EXPECT_FALSE(table.PublishPreparedLink(second));
    EXPECT_EQ(0u, table.NextSibling(second));
    for (const auto handle : {second, first, root}) ASSERT_TRUE(table.RollbackCommitted(handle));
    for (auto& original : originals) ASSERT_TRUE(table.CancelPrepared(&original));
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(4u, table.FreeCount());
}

TEST(PlayableInstanceTable, ParentLinksProofSharesNestedParentAndProtectsTheExistingTail) {
    PlayableInstanceTable table(6);
    const auto root = table.Acquire(PlayableKind::Container, 0);
    const auto oldFirst = table.Acquire(PlayableKind::Sound, root);
    const auto oldTail = table.Acquire(PlayableKind::Sound, root);
    ASSERT_NE(0u, oldTail);
    std::array<harken::PreparedPlayableInstance, 3> originals;
    std::array<PlayableInstanceTable::PreparedCommitProof, 3> activations;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Container, root, &originals[0]));
    const auto nested = originals[0].Handle();
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, nested, &originals[1]));
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, root, &originals[2]));
    const auto grandchild = originals[1].Handle();
    const auto sibling = originals[2].Handle();
    const auto sourceEpoch = originals[1].ScheduleEpoch();
    for (std::size_t index = 0; index < originals.size(); ++index)
        ASSERT_TRUE(table.ProvePreparedCommit(originals[index], &activations[index]));
    const std::array<PlayableInstanceTable::ParentLinkRequest, 3> requests{{
        {&originals[0], &activations[0]}, {&originals[1], &activations[1]},
        {&originals[2], &activations[2]}}};
    PlayableInstanceTable::PreparedParentLinksProof links(3);
    ASSERT_TRUE(table.ProvePreparedParentLinks(requests, {}, &links));
    EXPECT_TRUE(table.ParentLinksHeld(root));
    EXPECT_TRUE(table.ParentLinksHeld(oldTail));
    EXPECT_FALSE(table.ParentLinksHeld(oldFirst));
    EXPECT_FALSE(table.AdvanceLifecycle(oldFirst, PlayableLifecycle::Retired));
    EXPECT_FALSE(table.RollbackCommitted(oldFirst));
    EXPECT_FALSE(table.PublishPreparedLink(oldTail));
    EXPECT_FALSE(table.RollbackCommitted(oldTail));
    EXPECT_EQ(oldFirst, table.FirstChild(root));
    EXPECT_EQ(oldTail, table.NextSibling(oldFirst));
    EXPECT_EQ(0u, table.NextSibling(oldTail));
    for (auto& activation : activations)
        PlayableInstanceTable::ConsumePreparedCommit(std::move(activation));
    EXPECT_TRUE(table.AcquireSourceRetirementHold(grandchild, sourceEpoch));
    EXPECT_FALSE(table.AcquireSourceRetirementHold(grandchild, sourceEpoch + 1u));
    EXPECT_TRUE(table.HasSourceRetirementHold(grandchild));
    EXPECT_TRUE(links.Valid());
    EXPECT_TRUE(links.MatchesOriginalScope(&table, requests));
    PlayableInstanceTable::ConsumePreparedParentLinks(std::move(links));
    EXPECT_EQ(nested, table.NextSibling(oldTail));
    EXPECT_EQ(sibling, table.NextSibling(nested));
    EXPECT_EQ(grandchild, table.FirstChild(nested));
    EXPECT_EQ(0u, table.NextSibling(sibling));
    EXPECT_FALSE(table.RollbackCommitted(grandchild));
    ASSERT_TRUE(table.ReleaseSourceRetirementHold(grandchild));
    for (const auto handle : {grandchild, sibling, nested}) ASSERT_TRUE(table.RollbackCommitted(handle));
    for (auto& original : originals) ASSERT_TRUE(table.CancelPrepared(&original));
    EXPECT_EQ(oldFirst, table.FirstChild(root));
    EXPECT_EQ(oldTail, table.NextSibling(oldFirst));
    EXPECT_EQ(0u, table.NextSibling(oldTail));
    for (const auto handle : {oldTail, oldFirst, root})
        ASSERT_TRUE(table.AdvanceLifecycle(handle, PlayableLifecycle::Retired));
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(6u, table.FreeCount());
}

TEST(PlayableInstanceTable, ParentLinksProofRefusesTheParentEpochChangedAfterTheRealPreparation) {
    PlayableInstanceTable table(2);
    const auto root = table.Acquire(PlayableKind::Container, 0);
    harken::PreparedPlayableInstance original;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, root, &original));
    const auto staleChild = original.Handle();
    const auto oldParentEpoch = table.Classify(root).scheduleEpoch;
    ASSERT_NE(0u, table.BumpScheduleEpoch(root));
    ASSERT_NE(oldParentEpoch, table.Classify(root).scheduleEpoch);
    PlayableInstanceTable::PreparedCommitProof activation;
    ASSERT_TRUE(table.ProvePreparedCommit(original, &activation));
    const std::array<PlayableInstanceTable::ParentLinkRequest, 1> requests{{{&original, &activation}}};
    PlayableInstanceTable::PreparedParentLinksProof links(1);
    EXPECT_FALSE(table.ProvePreparedParentLinks(requests, {}, &links));
    EXPECT_FALSE(links.Valid());
    EXPECT_FALSE(table.ParentLinksHeld(root));
    EXPECT_FALSE(table.ParentLinksHeld(staleChild));
    EXPECT_TRUE(activation.Valid());
    activation.Release();
    ASSERT_TRUE(table.CancelPrepared(&original));
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, root, &original));
    EXPECT_NE(staleChild, original.Handle());
    ASSERT_TRUE(table.ProvePreparedCommit(original, &activation));
    ASSERT_TRUE(table.ProvePreparedParentLinks(requests, {}, &links));
    PlayableInstanceTable::ConsumePreparedCommit(std::move(activation));
    PlayableInstanceTable::ConsumePreparedParentLinks(std::move(links));
    EXPECT_EQ(original.Handle(), table.FirstChild(root));
    ASSERT_TRUE(table.RollbackCommitted(original.Handle()));
    ASSERT_TRUE(table.CancelPrepared(&original));
    ASSERT_TRUE(table.AdvanceLifecycle(root, PlayableLifecycle::Retired));
    EXPECT_EQ(2u, table.FreeCount());
}

TEST(PlayableInstanceTable, ParentLinksProofRejectsAliasesForeignTablesAndPartialBatchFailures) {
    PlayableInstanceTable table(3);
    PlayableInstanceTable foreign(1);
    std::array<harken::PreparedPlayableInstance, 3> originals;
    std::array<PlayableInstanceTable::PreparedCommitProof, 3> activations;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Container, 0, &originals[0]));
    const auto root = originals[0].Handle();
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, root, &originals[1]));
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, root, &originals[2]));
    for (std::size_t index = 0; index < originals.size(); ++index)
        ASSERT_TRUE(table.ProvePreparedCommit(originals[index], &activations[index]));
    std::array<PlayableInstanceTable::ParentLinkRequest, 2> requests{{
        {&originals[1], &activations[1]}, {&originals[2], &activations[2]}}};
    const std::array<PlayableInstanceTable::ParentLinkRequest, 1> anchors{{
        {&originals[0], &activations[0]}}};
    PlayableInstanceTable::PreparedParentLinksProof tooSmall(1);
    PlayableInstanceTable::PreparedParentLinksProof links(2);
    EXPECT_FALSE(table.ProvePreparedParentLinks(requests, anchors, &tooSmall));
    EXPECT_FALSE(foreign.ProvePreparedParentLinks(requests, anchors, &links));
    auto alias = originals[2];
    requests[1].original = &alias;
    EXPECT_FALSE(table.ProvePreparedParentLinks(requests, anchors, &links));
    for (const auto& original : originals) EXPECT_FALSE(table.ParentLinksHeld(original.Handle()));
    requests[1] = requests[0];
    EXPECT_FALSE(table.ProvePreparedParentLinks(requests, anchors, &links));
    requests[1] = {&originals[2], &activations[2]};
    EXPECT_FALSE(table.ProvePreparedParentLinks(requests, {}, &links));
    for (const auto& activation : activations) EXPECT_TRUE(activation.Valid());
    ASSERT_TRUE(table.ProvePreparedParentLinks(requests, anchors, &links));
    EXPECT_FALSE(table.ProvePreparedParentLinks(requests, anchors, &links));
    EXPECT_TRUE(links.Valid());
    EXPECT_FALSE(links.MatchesOriginalScope(&foreign, requests, anchors));
    auto reversed = requests;
    std::swap(reversed[0], reversed[1]);
    EXPECT_FALSE(links.MatchesOriginalScope(&table, reversed, anchors));
    links.Release();
    for (const auto& original : originals) EXPECT_FALSE(table.ParentLinksHeld(original.Handle()));
    ASSERT_TRUE(table.ProvePreparedParentLinks(requests, anchors, &links));
    links.Release();
    for (auto& activation : activations) activation.Release();
    for (auto& original : originals) ASSERT_TRUE(table.CancelPrepared(&original));
    EXPECT_EQ(3u, table.FreeCount());
}

TEST(PlayableInstanceTable, ParentLinksProofReleasePreservesRealSourceAndCannotRegrantActivation) {
    PlayableInstanceTable table(1);
    harken::PreparedPlayableInstance original;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, 0, &original));
    const auto handle = original.Handle();
    const auto epoch = original.ScheduleEpoch();
    PlayableInstanceTable::PreparedCommitProof activation;
    ASSERT_TRUE(table.ProvePreparedCommit(original, &activation));
    const std::array<PlayableInstanceTable::ParentLinkRequest, 1> requests{{{&original, &activation}}};
    PlayableInstanceTable::PreparedParentLinksProof links(1);
    ASSERT_TRUE(table.ProvePreparedParentLinks(requests, {}, &links));
    PlayableInstanceTable::ConsumePreparedCommit(std::move(activation));
    ASSERT_TRUE(table.AcquireSourceRetirementHold(handle, epoch));
    links.Release();
    EXPECT_FALSE(table.ParentLinksHeld(handle));
    EXPECT_TRUE(table.HasSourceRetirementHold(handle));
    EXPECT_FALSE(table.RollbackCommitted(handle));
    EXPECT_EQ(1u, table.LiveCount());
    ASSERT_TRUE(table.ReleaseSourceRetirementHold(handle));
    ASSERT_TRUE(table.RollbackCommitted(handle));
    EXPECT_TRUE(original.Valid());
    EXPECT_FALSE(table.ProvePreparedCommit(original, &activation));
    EXPECT_FALSE(table.ProvePreparedParentLinks(requests, {}, &links));
    ASSERT_TRUE(table.CancelPrepared(&original));
    EXPECT_EQ(1u, table.FreeCount());
}

TEST(PlayableInstanceTable, ParentLinksProofMoveTransfersTheActualTableHoldAndReleasesItsDestination) {
    PlayableInstanceTable first(1);
    PlayableInstanceTable second(1);
    harken::PreparedPlayableInstance firstOriginal, secondOriginal;
    ASSERT_TRUE(first.PrepareAcquire(PlayableKind::Container, 0, &firstOriginal));
    ASSERT_TRUE(second.PrepareAcquire(PlayableKind::Container, 0, &secondOriginal));
    PlayableInstanceTable::PreparedCommitProof firstActivation, secondActivation;
    ASSERT_TRUE(first.ProvePreparedCommit(firstOriginal, &firstActivation));
    ASSERT_TRUE(second.ProvePreparedCommit(secondOriginal, &secondActivation));
    const std::array<PlayableInstanceTable::ParentLinkRequest, 1> firstAnchor{{{&firstOriginal, &firstActivation}}};
    const std::array<PlayableInstanceTable::ParentLinkRequest, 1> secondAnchor{{{&secondOriginal, &secondActivation}}};
    PlayableInstanceTable::PreparedParentLinksProof destination(0);
    PlayableInstanceTable::PreparedParentLinksProof source(0);
    ASSERT_TRUE(first.ProvePreparedParentLinks({}, firstAnchor, &destination));
    ASSERT_TRUE(second.ProvePreparedParentLinks({}, secondAnchor, &source));
    destination = std::move(source);
    EXPECT_FALSE(source.Valid());
    EXPECT_FALSE(first.ParentLinksHeld(firstOriginal.Handle()));
    EXPECT_TRUE(second.ParentLinksHeld(secondOriginal.Handle()));
    EXPECT_TRUE(destination.MatchesOriginalScope(&second, {}, secondAnchor));
    {
        PlayableInstanceTable::PreparedParentLinksProof finalOwner(std::move(destination));
        EXPECT_FALSE(destination.Valid());
        EXPECT_TRUE(finalOwner.Valid());
    }
    EXPECT_FALSE(second.ParentLinksHeld(secondOriginal.Handle()));
    firstActivation.Release();
    secondActivation.Release();
    ASSERT_TRUE(first.CancelPrepared(&firstOriginal));
    ASSERT_TRUE(second.CancelPrepared(&secondOriginal));
    EXPECT_EQ(1u, first.FreeCount());
    EXPECT_EQ(1u, second.FreeCount());
}

namespace {
struct ParentLinkSourceOwner {
    PlayableInstanceTable* table = nullptr;
    harken::PlayableTreeExecutor* executor = nullptr;
    PlayableInstanceHandle root = 0;
    PlayableInstanceHandle leaf = 0;
    std::uint32_t blockedCancels = 0;
    std::uint32_t commits = 0;
    std::uint32_t cancels = 0;
    bool failCommit = false;
    bool observedPrivateCommit = false;
    bool observedReleaseBeforeCancel = false;
};
bool ValidateParentLinkSourceOwner(void* opaque) noexcept {
    const auto& owner = *static_cast<ParentLinkSourceOwner*>(opaque);
    return owner.table != nullptr && owner.executor != nullptr &&
        owner.table->HasSourceRetirementHold(owner.leaf);
}
bool CommitParentLinkSourceOwner(void* opaque, std::uint64_t) noexcept {
    auto& owner = *static_cast<ParentLinkSourceOwner*>(opaque);
    ++owner.commits;
    owner.observedPrivateCommit = owner.executor->ParentLinkProofHeld() &&
        !owner.executor->InstanceCommitProofsHeld() && owner.table->ParentLinksHeld(owner.root) &&
        owner.table->ParentLinksHeld(owner.leaf) && owner.table->Classify(owner.root).live &&
        owner.table->Classify(owner.leaf).live && owner.table->FirstChild(owner.root) == 0 &&
        owner.table->NextSibling(owner.leaf) == 0;
    return !owner.failCommit;
}
bool CancelParentLinkSourceOwner(void* opaque) noexcept {
    auto& owner = *static_cast<ParentLinkSourceOwner*>(opaque);
    ++owner.cancels;
    owner.observedReleaseBeforeCancel = !owner.executor->ParentLinkProofHeld() &&
        !owner.table->ParentLinksHeld(owner.root) && !owner.table->ParentLinksHeld(owner.leaf);
    if (owner.blockedCancels != 0) {
        --owner.blockedCancels;
        return false;
    }
    return owner.table->ReleaseSourceRetirementHold(owner.leaf);
}
}

TEST(PreparedPlayableTreeStep, ParentLinksProofPreservesAdoptBeforePrepareCapacityAndPublicationOrder) {
    PlayableInstanceTable table(3);
    const auto root = table.Acquire(PlayableKind::Container, 0);
    harken::PreparedPlayableInstance adopted;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, root, &adopted));
    const auto adoptedHandle = adopted.Handle();
    harken::PreparedPlayableTreeStep step(1);
    ASSERT_TRUE(step.AdoptPreparedInstanceReservation(&adopted));
    ASSERT_TRUE(step.PrepareInstance(table, PlayableKind::Sound, root));
    const auto internal = step.PreparedInstanceHandle(0);
    ASSERT_TRUE(step.ProveInstanceCommits(&table));
    ASSERT_TRUE(step.ProveParentLinks(table));
    EXPECT_TRUE(step.Validate());
    EXPECT_TRUE(table.ParentLinksHeld(root));
    ASSERT_TRUE(step.Commit());
    EXPECT_FALSE(step.ParentLinkProofHeld());
    EXPECT_EQ(internal, table.FirstChild(root));
    EXPECT_EQ(adoptedHandle, table.NextSibling(internal));
    EXPECT_EQ(0u, table.NextSibling(adoptedHandle));
    ASSERT_TRUE(step.RollbackCommitted());
    EXPECT_FALSE(adopted.Valid());
    ASSERT_TRUE(table.AdvanceLifecycle(root, PlayableLifecycle::Retired));
    EXPECT_EQ(3u, table.FreeCount());
}

TEST(PreparedPlayableTreeStep, ParentLinksProofKeepsNestedRolesAndRealBorrowedTailUntilPublication) {
    PlayableInstanceTable table(5);
    const auto root = table.Acquire(PlayableKind::Container, 0);
    const auto oldTail = table.Acquire(PlayableKind::Sound, root);
    harken::PreparedPlayableTreeStep step(2);
    ASSERT_TRUE(step.PrepareInstance(table, PlayableKind::Container, root));
    const auto nested = step.PreparedInstanceHandle(0);
    harken::PreparedPlayableInstance adopted;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Sound, nested, &adopted));
    const auto grandchild = adopted.Handle();
    ASSERT_TRUE(step.AdoptPreparedInstanceReservation(&adopted));
    ASSERT_TRUE(step.PrepareInstance(table, PlayableKind::Sound, root));
    const auto sibling = step.PreparedInstanceHandle(1);
    ASSERT_TRUE(step.ProveInstanceCommits(&table));
    ASSERT_TRUE(step.ProveParentLinks(table));
    ASSERT_TRUE(step.CommitWithoutPublishingLinks());
    EXPECT_FALSE(step.InstanceCommitProofsHeld());
    EXPECT_TRUE(step.ParentLinkProofHeld());
    EXPECT_TRUE(table.ParentLinksHeld(nested));
    EXPECT_TRUE(table.ParentLinksHeld(oldTail));
    EXPECT_EQ(oldTail, table.FirstChild(root));
    EXPECT_EQ(0u, table.NextSibling(oldTail));
    EXPECT_EQ(0u, table.FirstChild(nested));
    EXPECT_FALSE(table.AdvanceLifecycle(oldTail, PlayableLifecycle::Retired));
    EXPECT_FALSE(table.PublishPreparedLink(sibling));
    ASSERT_TRUE(step.PublishPreparedLinks());
    EXPECT_FALSE(step.ParentLinkProofHeld());
    EXPECT_EQ(nested, table.NextSibling(oldTail));
    EXPECT_EQ(sibling, table.NextSibling(nested));
    EXPECT_EQ(grandchild, table.FirstChild(nested));
    EXPECT_FALSE(step.PublishPreparedLinks());
    ASSERT_TRUE(step.RollbackCommitted());
    EXPECT_EQ(0u, table.NextSibling(oldTail));
    ASSERT_TRUE(table.AdvanceLifecycle(oldTail, PlayableLifecycle::Retired));
    ASSERT_TRUE(table.AdvanceLifecycle(root, PlayableLifecycle::Retired));
    EXPECT_EQ(5u, table.FreeCount());
}

TEST(PreparedPlayableTreeStep, ParentLinksProofReleaseRejectsLegacyFallbackAndOriginalStillPreparingCanReprove) {
    PlayableInstanceTable table(2);
    harken::PreparedPlayableTreeStep step(2);
    ASSERT_TRUE(step.PrepareInstance(table, PlayableKind::Container));
    const auto root = step.PreparedInstanceHandle(0);
    ASSERT_TRUE(step.PrepareInstance(table, PlayableKind::Sound, root));
    ASSERT_TRUE(step.ProveInstanceCommits(&table));
    ASSERT_TRUE(step.ProveParentLinks(table));
    step.ReleaseParentLinkProof();
    EXPECT_FALSE(step.Validate());
    EXPECT_TRUE(step.InstanceCommitProofsHeld());
    ASSERT_TRUE(step.ProveParentLinks(table));
    ASSERT_TRUE(step.CommitWithoutPublishingLinks());
    step.ReleaseParentLinkProof();
    EXPECT_FALSE(step.PublishPreparedLinks());
    EXPECT_EQ(0u, table.FirstChild(root));
    EXPECT_FALSE(step.ProveParentLinks(table));
    EXPECT_EQ(2u, table.LiveCount());
    ASSERT_TRUE(step.RollbackCommitted());
    EXPECT_EQ(2u, table.FreeCount());
}

TEST(PlayableTreeExecutor, ParentLinksProofKeepsConsumedExternalAnchorAndSourcePrivateAcrossOwnerCommit) {
    PlayableInstanceTable table(2);
    harken::PreparedPlayableInstance root;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Container, 0, &root));
    harken::PlayableRootMetadata metadata{};
    metadata.actionFiring = 41;
    ASSERT_TRUE(table.SetPreparedRootMetadata(root, metadata));
    const auto rootHandle = root.Handle();
    harken::PlayableTreeExecutor executor(1);
    ASSERT_TRUE(executor.AdoptPreparedInstance(table, &root));
    ASSERT_TRUE(executor.PrepareInstance(table, PlayableKind::Sound, rootHandle));
    const auto leaf = executor.PreparedInstanceHandle(0);
    ASSERT_TRUE(table.AcquireSourceRetirementHold(leaf, executor.PreparedInstanceEpoch(0)));
    ParentLinkSourceOwner owner{&table, &executor, rootHandle, leaf};
    ASSERT_TRUE(executor.EnablePreparedOwnerRollbackRetention());
    ASSERT_TRUE(executor.AdoptPreparedOwnerToken(&owner, ValidateParentLinkSourceOwner,
        CommitParentLinkSourceOwner, CancelParentLinkSourceOwner));
    ASSERT_TRUE(executor.ProveInstanceCommits(&table, &root));
    ASSERT_TRUE(executor.ProveParentLinks(table, &root));
    harken::PreparedPlayableCancellation cancellation;
    EXPECT_FALSE(table.PrepareCancellation(rootHandle, 51, 1, &cancellation));
    EXPECT_FALSE(cancellation.Valid());
    EXPECT_TRUE(executor.Validate());
    ASSERT_TRUE(executor.Commit(table, 17));
    EXPECT_TRUE(owner.observedPrivateCommit);
    EXPECT_EQ(1u, owner.commits);
    EXPECT_FALSE(root.Valid());
    EXPECT_FALSE(executor.ParentLinkProofHeld());
    EXPECT_EQ(leaf, table.FirstChild(rootHandle));
    ASSERT_TRUE(executor.RollbackCommitted(table));
    EXPECT_TRUE(owner.observedReleaseBeforeCancel);
    EXPECT_EQ(1u, owner.cancels);
    EXPECT_FALSE(table.HasSourceRetirementHold(leaf));
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(2u, table.FreeCount());
    EXPECT_TRUE(executor.ResetForReuse());
}

TEST(PlayableTreeExecutor, ParentLinksProofOwnerFailureReleasesBeforeRetainedSourceCancelAndRecovers) {
    for (const bool external : {false, true}) {
        PlayableInstanceTable table(2);
        harken::PreparedPlayableInstance root;
        harken::PlayableTreeExecutor executor(2);
        PlayableInstanceHandle rootHandle = 0;
        if (external) {
            ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Container, 0, &root));
            rootHandle = root.Handle();
            ASSERT_TRUE(executor.AdoptPreparedInstance(table, &root));
        } else {
            ASSERT_TRUE(executor.PrepareInstance(table, PlayableKind::Container));
            rootHandle = executor.PreparedInstanceHandle(0);
        }
        ASSERT_TRUE(executor.PrepareInstance(table, PlayableKind::Sound, rootHandle));
        const auto leafIndex = external ? 0u : 1u;
        const auto leaf = executor.PreparedInstanceHandle(leafIndex);
        ASSERT_TRUE(table.AcquireSourceRetirementHold(leaf, executor.PreparedInstanceEpoch(leafIndex)));
        ParentLinkSourceOwner owner{&table, &executor, rootHandle, leaf, 1u};
        owner.failCommit = true;
        ASSERT_TRUE(executor.EnablePreparedOwnerRollbackRetention());
        ASSERT_TRUE(executor.AdoptPreparedOwnerToken(&owner, ValidateParentLinkSourceOwner,
            CommitParentLinkSourceOwner, CancelParentLinkSourceOwner));
        ASSERT_TRUE(external ? executor.ProveInstanceCommits(&table, &root) : executor.ProveInstanceCommits());
        ASSERT_TRUE(executor.ProveParentLinks(table, external ? &root : nullptr));
        EXPECT_FALSE(external ? executor.Commit(table, 17) : executor.Commit(17));
        EXPECT_TRUE(owner.observedPrivateCommit);
        EXPECT_TRUE(owner.observedReleaseBeforeCancel);
        EXPECT_EQ(1u, owner.cancels);
        EXPECT_EQ(1u, executor.OwnerTokenCount());
        EXPECT_FALSE(executor.ParentLinkProofHeld());
        EXPECT_FALSE(executor.InstanceCommitProofsHeld());
        EXPECT_FALSE(table.ParentLinksHeld(rootHandle));
        EXPECT_FALSE(table.ParentLinksHeld(leaf));
        EXPECT_TRUE(table.HasSourceRetirementHold(leaf));
        EXPECT_EQ(2u, table.LiveCount());
        EXPECT_EQ(0u, table.FirstChild(rootHandle));
        EXPECT_FALSE(table.RollbackCommitted(leaf));
        EXPECT_FALSE(executor.ResetForReuse());
        ASSERT_TRUE(executor.RollbackCommitted(table));
        EXPECT_EQ(2u, owner.cancels);
        EXPECT_EQ(0u, executor.OwnerTokenCount());
        EXPECT_FALSE(table.HasSourceRetirementHold(leaf));
        EXPECT_EQ(0u, table.LiveCount());
        EXPECT_EQ(2u, table.FreeCount());
        EXPECT_TRUE(executor.ResetForReuse());
    }
}

TEST(PlayableTreeExecutor, ParentLinksProofWrongRoutesAndDuplicateProofReleaseAllOriginalHolds) {
    for (unsigned mode = 0; mode != 6; ++mode) {
        PlayableInstanceTable table(2), foreign(1);
        harken::PreparedPlayableInstance root;
        ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Container, 0, &root));
        const auto rootHandle = root.Handle();
        harken::PlayableTreeExecutor executor(1);
        ASSERT_TRUE(executor.AdoptPreparedInstance(table, &root));
        ASSERT_TRUE(executor.PrepareInstance(table, PlayableKind::Sound, rootHandle));
        const auto leaf = executor.PreparedInstanceHandle(0);
        ASSERT_TRUE(executor.ProveInstanceCommits(&table, &root));
        ASSERT_TRUE(executor.ProveParentLinks(table, &root));
        if (mode == 0) EXPECT_FALSE(executor.Commit(17));
        if (mode == 1) EXPECT_FALSE(executor.Commit(foreign, 17));
        if (mode == 2) EXPECT_FALSE(executor.ProveParentLinks(table, &root));
        if (mode == 3) EXPECT_FALSE(executor.ProveParentLinks(foreign, &root));
        if (mode == 4) EXPECT_FALSE(executor.ProveParentLinks(table));
        if (mode == 5) {
            executor.ReleaseParentLinkProof();
            EXPECT_FALSE(executor.Validate());
            EXPECT_FALSE(executor.Commit(table, 17));
        }
        EXPECT_FALSE(executor.ParentLinkProofHeld());
        EXPECT_FALSE(executor.InstanceCommitProofsHeld());
        EXPECT_FALSE(table.PreparedCommitHeld(root));
        EXPECT_FALSE(table.ParentLinksHeld(rootHandle));
        EXPECT_FALSE(table.ParentLinksHeld(leaf));
        EXPECT_TRUE(root.Valid());
        EXPECT_EQ(0u, table.LiveCount());
        EXPECT_EQ(0u, table.FirstChild(rootHandle));
        ASSERT_TRUE(executor.Cancel(table));
        EXPECT_EQ(2u, table.FreeCount());
    }
}

TEST(PlayableTreeExecutor, ParentLinksProofRootOnlyScopeFreezesBothMoveEndpointsAndDoesNotPublishRootEdge) {
    PlayableInstanceTable table(3);
    const auto borrowed = table.Acquire(PlayableKind::Container, 0);
    harken::PreparedPlayableInstance root;
    ASSERT_TRUE(table.PrepareAcquire(PlayableKind::Container, borrowed, &root));
    const auto rootHandle = root.Handle();
    harken::PlayableTreeExecutor executor(1), other(1);
    ASSERT_TRUE(executor.AdoptPreparedInstance(table, &root));
    ASSERT_TRUE(other.PrepareInstance(table, PlayableKind::Sound));
    const auto unrelated = other.PreparedInstanceHandle(0);
    ASSERT_TRUE(executor.ProveInstanceCommits(&table, &root));
    ASSERT_TRUE(executor.ProveParentLinks(table, &root));
    EXPECT_TRUE(table.ParentLinksHeld(rootHandle));
    EXPECT_FALSE(table.ParentLinksHeld(borrowed));
    EXPECT_FALSE(executor.ResetForReuse());
    harken::PlayableTreeExecutor denied(std::move(executor));
    EXPECT_FALSE(denied.ParentLinkProofHeld());
    other = std::move(executor);
    executor = std::move(other);
    EXPECT_EQ(unrelated, other.PreparedInstanceHandle(0));
    EXPECT_TRUE(executor.ParentLinkProofHeld());
    EXPECT_TRUE(root.Valid());
    ASSERT_TRUE(executor.Commit(table, 17));
    EXPECT_FALSE(executor.ParentLinkProofHeld());
    EXPECT_EQ(0u, table.FirstChild(borrowed));
    EXPECT_TRUE(table.Classify(rootHandle).live);
    ASSERT_TRUE(executor.RollbackCommitted(table));
    other.Cancel();
    ASSERT_TRUE(table.AdvanceLifecycle(borrowed, PlayableLifecycle::Retired));
    EXPECT_EQ(3u, table.FreeCount());
}


namespace {
using NativePlayablePin = harken::PlayableInstanceTable::OriginalLifetimePin;
using NativePlayableAccess = harken::detail::PlayableInstanceTableTestAccess;
static_assert(!std::is_copy_constructible_v<NativePlayablePin>);
static_assert(std::is_nothrow_move_constructible_v<NativePlayablePin>);

struct GenuinePlayableLifetimeTree {
    harken::PlayableInstanceTable& table;
    harken::PreparedPlayableInstance root;
    harken::PreparedPlayableInstance child;
    harken::PlayableInstanceTable::PreparedCommitProof rootActivation;
    harken::PlayableInstanceTable::PreparedCommitProof childActivation;
    harken::PlayableInstanceTable::PreparedParentLinksProof links{2};
    NativePlayablePin rootOrigin;
    NativePlayablePin childOrigin;
    explicit GenuinePlayableLifetimeTree(harken::PlayableInstanceTable& receiver) : table(receiver) {}
    bool Prepare(bool withChild = true) {
        if (!table.PrepareAcquire(harken::PlayableKind::Container, 0, &root)) return false;
        if (withChild && !table.PrepareAcquire(harken::PlayableKind::Sound, root.Handle(), &child)) return false;
        if (!table.ProvePreparedCommit(root, &rootActivation) ||
            (withChild && !table.ProvePreparedCommit(child, &childActivation))) return false;
        const std::array<harken::PlayableInstanceTable::ParentLinkRequest, 1> nodes{{{&child, &childActivation}}};
        const std::array<harken::PlayableInstanceTable::ParentLinkRequest, 1> anchors{{{&root, &rootActivation}}};
        return table.ProvePreparedParentLinks(withChild ? std::span<const harken::PlayableInstanceTable::ParentLinkRequest>(nodes)
            : std::span<const harken::PlayableInstanceTable::ParentLinkRequest>{}, anchors, &links);
    }
    bool Issue() {
        return table.IssueOriginalPreparedLifetime(links, root, &rootOrigin) &&
            (!child.Valid() || table.IssueOriginalPreparedLifetime(links, child, &childOrigin));
    }
    void ReleaseProofs() {
        links.Release(); childActivation.Release(); rootActivation.Release();
    }
    void Commit() {
        const auto withChild = child.Valid();
        harken::PlayableInstanceTable::ConsumePreparedCommit(std::move(rootActivation));
        if (withChild) harken::PlayableInstanceTable::ConsumePreparedCommit(std::move(childActivation));
        harken::PlayableInstanceTable::ConsumePreparedParentLinks(std::move(links));
    }
};

TEST(PlayableInstanceTable, OriginalLifetimeRequiresTheActualHeldFactoryAndExactParentLinksNode) {
    harken::PlayableInstanceTable table(2);
    GenuinePlayableLifetimeTree tree(table);
    harken::PlayableInstanceTable::PreparedCommitProof absent;
    NativePlayablePin output;
    EXPECT_FALSE(table.IssueOriginalPreparedLifetime(absent, &output));
    ASSERT_TRUE(tree.Prepare());
    const auto root = tree.root.Handle();
    const auto child = tree.child.Handle();
    const auto childEpoch = tree.child.ScheduleEpoch();
    const auto copy = tree.child;
    EXPECT_FALSE(table.IssueOriginalPreparedLifetime(tree.links, copy, &output));
    ASSERT_TRUE(tree.Issue());
    EXPECT_EQ(root, tree.rootOrigin.OriginalHandle());
    EXPECT_EQ(child, tree.childOrigin.OriginalHandle());
    EXPECT_EQ(root, tree.childOrigin.OriginalParent());
    EXPECT_EQ(childEpoch, tree.childOrigin.OriginalScheduleEpoch());
    EXPECT_NE(0u, NativePlayableAccess::OriginalLifetimeMarker(table, tree.rootOrigin));
    EXPECT_NE(NativePlayableAccess::OriginalLifetimeMarker(table, tree.rootOrigin),
        NativePlayableAccess::OriginalLifetimeMarker(table, tree.childOrigin));
    EXPECT_FALSE(table.IssueOriginalPreparedLifetime(tree.rootActivation, &output));
    EXPECT_FALSE(table.IssueOriginalPreparedLifetime(tree.childActivation, &tree.rootOrigin));
    EXPECT_FALSE(tree.rootOrigin.Borrow(&tree.rootOrigin));
    EXPECT_FALSE(output.Valid());
    tree.ReleaseProofs();
    EXPECT_FALSE(table.IssueOriginalPreparedLifetime(tree.rootActivation, &output));
    ASSERT_TRUE(table.CancelPrepared(&tree.child, &tree.childOrigin));
    ASSERT_TRUE(table.CancelPrepared(&tree.root, &tree.rootOrigin));
    EXPECT_EQ(2u, table.FreeCount());
    EXPECT_EQ(0u, table.LiveCount());
}

TEST(PlayableInstanceTable, OriginalLifetimeForeignLoanKeepsTheRealPreparedOwnerUntilExactCancellation) {
    harken::PlayableInstanceTable table(1);
    GenuinePlayableLifetimeTree tree(table);
    ASSERT_TRUE(tree.Prepare(false));
    ASSERT_TRUE(tree.Issue());
    const auto handle = tree.root.Handle();
    const auto epoch = tree.rootOrigin.OriginalScheduleEpoch();
    const auto marker = NativePlayableAccess::OriginalLifetimeMarker(table, tree.rootOrigin);
    NativePlayablePin foreign;
    ASSERT_TRUE(tree.rootOrigin.Borrow(&foreign));
    EXPECT_EQ(2u, NativePlayableAccess::OriginalLifetimeCount(table, tree.rootOrigin));
    tree.ReleaseProofs();
    EXPECT_FALSE(table.CancelPrepared(&tree.root, &tree.rootOrigin));
    EXPECT_TRUE(tree.root.Valid());
    EXPECT_TRUE(tree.rootOrigin.Valid());
    EXPECT_EQ(epoch, foreign.OriginalScheduleEpoch());
    EXPECT_EQ(marker, NativePlayableAccess::OriginalLifetimeMarker(table, tree.rootOrigin));
    harken::PreparedPlayableInstance other;
    EXPECT_FALSE(table.PrepareAcquire(harken::PlayableKind::Sound, 0, &other));
    EXPECT_EQ(0u, table.Acquire(harken::PlayableKind::Sound, 0));
    foreign.Release();
    ASSERT_TRUE(table.CancelPrepared(&tree.root, &tree.rootOrigin));
    EXPECT_FALSE(tree.rootOrigin.Valid());
    ASSERT_TRUE(table.PrepareAcquire(harken::PlayableKind::Sound, 0, &other));
    EXPECT_NE(handle, other.Handle());
    EXPECT_EQ(static_cast<std::uint32_t>(handle), static_cast<std::uint32_t>(other.Handle()));
    EXPECT_FALSE(table.IsOriginalLifetimePinFor(foreign, other.Handle()));
    ASSERT_TRUE(table.CancelPrepared(&other));
    EXPECT_EQ(1u, table.FreeCount());
}

TEST(PlayableInstanceTable, OriginalLifetimeDoesNotFreezeNativeCommitEpochTerminalOrSourceProgress) {
    harken::PlayableInstanceTable table(2);
    GenuinePlayableLifetimeTree tree(table);
    ASSERT_TRUE(tree.Prepare());
    ASSERT_TRUE(tree.Issue());
    const auto root = tree.root.Handle();
    const auto child = tree.child.Handle();
    const auto epoch = tree.child.ScheduleEpoch();
    NativePlayablePin rootForeign, childForeign;
    ASSERT_TRUE(tree.rootOrigin.Borrow(&rootForeign));
    ASSERT_TRUE(tree.childOrigin.Borrow(&childForeign));
    tree.Commit();
    ASSERT_TRUE(table.AcquireSourceRetirementHold(child, epoch));
    ASSERT_EQ(2u, table.LiveCount());
    ASSERT_EQ(child, table.FirstChild(root));
    const auto nextEpoch = table.BumpScheduleEpochChecked(child);
    ASSERT_EQ(harken::IssueStatus::Ok, nextEpoch.status);
    ASSERT_NE(epoch, nextEpoch.value);
    EXPECT_TRUE(childForeign.Valid());
    EXPECT_EQ(epoch, childForeign.OriginalScheduleEpoch());
    EXPECT_FALSE(table.IsOriginalLifetimeCurrent(childForeign));
    ASSERT_TRUE(table.SetTerminalOutcome(child, {harken::PlayableTerminalKind::NaturalCompletion, 0, 1}));
    ASSERT_TRUE(table.AdvanceLifecycle(child, harken::PlayableLifecycle::LogicalComplete));
    ASSERT_TRUE(table.AdvanceLifecycle(child, harken::PlayableLifecycle::AudibleRetiring));
    EXPECT_FALSE(table.AdvanceLifecycle(child, harken::PlayableLifecycle::Retired, &tree.childOrigin));
    childForeign.Release();
    EXPECT_FALSE(table.AdvanceLifecycle(child, harken::PlayableLifecycle::Retired, &tree.childOrigin));
    EXPECT_TRUE(tree.childOrigin.Valid());
    ASSERT_TRUE(table.ReleaseSourceRetirementHold(child));
    ASSERT_TRUE(table.AdvanceLifecycle(child, harken::PlayableLifecycle::Retired, &tree.childOrigin));
    EXPECT_FALSE(tree.childOrigin.Valid());
    EXPECT_EQ(0u, table.FirstChild(root));
    EXPECT_FALSE(table.AdvanceLifecycle(root, harken::PlayableLifecycle::Retired, &tree.rootOrigin));
    rootForeign.Release();
    ASSERT_TRUE(table.AdvanceLifecycle(root, harken::PlayableLifecycle::Retired, &tree.rootOrigin));
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(2u, table.FreeCount());
}

TEST(PlayableInstanceTable, OriginalLifetimeAllowsRealRollbackButRetainsPreparedStorageUntilForeignReturn) {
    harken::PlayableInstanceTable table(2);
    GenuinePlayableLifetimeTree tree(table);
    ASSERT_TRUE(tree.Prepare());
    ASSERT_TRUE(tree.Issue());
    const auto root = tree.root.Handle();
    const auto child = tree.child.Handle();
    NativePlayablePin foreign;
    ASSERT_TRUE(tree.childOrigin.Borrow(&foreign));
    tree.Commit();
    ASSERT_TRUE(table.RollbackCommitted(child));
    EXPECT_TRUE(tree.child.Valid());
    EXPECT_TRUE(foreign.Valid());
    EXPECT_EQ(root, foreign.OriginalParent());
    EXPECT_EQ(child, foreign.OriginalHandle());
    EXPECT_EQ(0u, table.FirstChild(root));
    EXPECT_FALSE(table.CancelPrepared(&tree.child, &tree.childOrigin));
    EXPECT_TRUE(tree.child.Valid());
    foreign.Release();
    ASSERT_TRUE(table.CancelPrepared(&tree.child, &tree.childOrigin));
    ASSERT_TRUE(table.RollbackCommitted(root));
    ASSERT_TRUE(table.CancelPrepared(&tree.root, &tree.rootOrigin));
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(2u, table.FreeCount());
}

TEST(PlayableInstanceTable, OriginalLifetimeReceiverAndIndependentRootChildBaselineAreExact) {
    harken::PlayableInstanceTable table(2), foreignTable(2);
    GenuinePlayableLifetimeTree tree(table), other(foreignTable);
    ASSERT_TRUE(tree.Prepare());
    ASSERT_TRUE(other.Prepare());
    ASSERT_TRUE(tree.Issue());
    ASSERT_TRUE(other.Issue());
    ASSERT_EQ(tree.rootOrigin.OriginalHandle(), other.rootOrigin.OriginalHandle());
    EXPECT_FALSE(foreignTable.IsOriginalLifetimePinFor(tree.rootOrigin, tree.rootOrigin.OriginalHandle()));
    tree.ReleaseProofs(); other.ReleaseProofs();
    EXPECT_FALSE(table.CancelPrepared(&tree.root, &tree.childOrigin));
    EXPECT_FALSE(table.CancelPrepared(&tree.root, &other.rootOrigin));
    EXPECT_TRUE(tree.root.Valid());
    EXPECT_TRUE(tree.childOrigin.Valid());
    EXPECT_TRUE(other.rootOrigin.Valid());
    ASSERT_TRUE(table.CancelPrepared(&tree.child, &tree.childOrigin));
    ASSERT_TRUE(table.CancelPrepared(&tree.root, &tree.rootOrigin));
    ASSERT_TRUE(foreignTable.CancelPrepared(&other.child, &other.childOrigin));
    ASSERT_TRUE(foreignTable.CancelPrepared(&other.root, &other.rootOrigin));
    EXPECT_EQ(2u, table.FreeCount());
    EXPECT_EQ(2u, foreignTable.FreeCount());
}

TEST(PlayableInstanceTable, OriginalLifetimeMoveOverwriteAndScopeDestructionReturnOnlyTheirActualLoans) {
    harken::PlayableInstanceTable table(1), otherTable(1);
    GenuinePlayableLifetimeTree tree(table), other(otherTable);
    ASSERT_TRUE(tree.Prepare(false));
    ASSERT_TRUE(other.Prepare(false));
    ASSERT_TRUE(tree.Issue());
    ASSERT_TRUE(other.Issue());
    {
        NativePlayablePin loan;
        ASSERT_TRUE(tree.rootOrigin.Borrow(&loan));
        NativePlayablePin moved(std::move(loan));
        EXPECT_FALSE(loan.Valid());
        EXPECT_TRUE(moved.Valid());
        moved = std::move(other.rootOrigin);
        EXPECT_FALSE(other.rootOrigin.Valid());
        EXPECT_EQ(1u, NativePlayableAccess::OriginalLifetimeCount(table, tree.rootOrigin));
        EXPECT_TRUE(otherTable.IsOriginalLifetimePinFor(moved, other.root.Handle()));
        other.ReleaseProofs();
        ASSERT_TRUE(otherTable.CancelPrepared(&other.root, &moved));
        EXPECT_FALSE(moved.Valid());
    }
    {
        NativePlayablePin loan;
        ASSERT_TRUE(tree.rootOrigin.Borrow(&loan));
        EXPECT_FALSE(tree.rootOrigin.Borrow(&loan));
        EXPECT_EQ(2u, NativePlayableAccess::OriginalLifetimeCount(table, tree.rootOrigin));
    }
    EXPECT_EQ(1u, NativePlayableAccess::OriginalLifetimeCount(table, tree.rootOrigin));
    tree.ReleaseProofs();
    ASSERT_TRUE(table.CancelPrepared(&tree.root, &tree.rootOrigin));
    EXPECT_EQ(1u, table.FreeCount());
    EXPECT_EQ(1u, otherTable.FreeCount());
}

TEST(PlayableInstanceTable, OriginalLifetimeNativeCountLimitRefusesBorrowWithoutChangingTheOwnerOrFactory) {
    harken::PlayableInstanceTable table(1);
    GenuinePlayableLifetimeTree tree(table);
    ASSERT_TRUE(tree.Prepare(false));
    ASSERT_TRUE(tree.Issue());
    const auto marker = NativePlayableAccess::OriginalLifetimeMarker(table, tree.rootOrigin);
    struct ScopedNegativeLimit {
        harken::PlayableInstanceTable& table; const NativePlayablePin& original; bool active;
        ~ScopedNegativeLimit() { if (active) NativePlayableAccess::RestoreOriginalBorrow(table, original); }
    } scope{table, tree.rootOrigin, NativePlayableAccess::RefuseOriginalBorrow(table, tree.rootOrigin)};
    ASSERT_TRUE(scope.active);
    NativePlayablePin output;
    EXPECT_FALSE(tree.rootOrigin.Borrow(&output));
    EXPECT_FALSE(output.Valid());
    EXPECT_TRUE(tree.rootOrigin.Valid());
    EXPECT_EQ(marker, NativePlayableAccess::OriginalLifetimeMarker(table, tree.rootOrigin));
    EXPECT_TRUE(tree.links.Valid());
    NativePlayableAccess::RestoreOriginalBorrow(table, tree.rootOrigin); scope.active = false;
    tree.ReleaseProofs();
    ASSERT_TRUE(table.CancelPrepared(&tree.root, &tree.rootOrigin));
    EXPECT_EQ(1u, table.FreeCount());
}

TEST(PlayableInstanceTable, OriginalLifetimeMarkerRefusalLeavesExistingOriginsAndOldNativeProgressUsable) {
    harken::PlayableInstanceTable table(2);
    GenuinePlayableLifetimeTree original(table), noOutput(table);
    ASSERT_TRUE(original.Prepare(false));
    ASSERT_TRUE(original.Issue());
    ASSERT_TRUE(noOutput.Prepare(false));
    struct ScopedNegativeMarker {
        harken::PlayableInstanceTable& table; bool before;
        ~ScopedNegativeMarker() { NativePlayableAccess::RestoreOriginalMarker(table, before); }
    } scope{table, NativePlayableAccess::RefuseOriginalMarker(table)};
    NativePlayablePin refused, loan;
    EXPECT_FALSE(table.IssueOriginalPreparedLifetime(noOutput.rootActivation, &refused));
    EXPECT_FALSE(refused.Valid());
    ASSERT_TRUE(original.rootOrigin.Borrow(&loan));
    const auto first = original.root.Handle();
    const auto second = noOutput.root.Handle();
    original.Commit(); noOutput.Commit();
    ASSERT_TRUE(table.AdvanceLifecycle(first, harken::PlayableLifecycle::LogicalComplete));
    EXPECT_FALSE(table.AdvanceLifecycle(first, harken::PlayableLifecycle::Retired, &original.rootOrigin));
    ASSERT_TRUE(table.AdvanceLifecycle(second, harken::PlayableLifecycle::Retired));
    loan.Release();
    ASSERT_TRUE(table.AdvanceLifecycle(first, harken::PlayableLifecycle::Retired, &original.rootOrigin));
    EXPECT_EQ(0u, table.LiveCount());
    EXPECT_EQ(2u, table.FreeCount());
}
} // namespace


namespace {
TEST(PlayableInstanceTable, OriginalRetirementHoldUsesActualEndPreflightAndRetainsTheExactCoreRow) {
    harken::PlayableInstanceTable table(2), other(2);
    GenuinePlayableLifetimeTree original(table);
    ASSERT_TRUE(original.Prepare());
    ASSERT_TRUE(original.Issue());
    const auto root = original.root.Handle();
    const auto child = original.child.Handle();
    const auto marker = NativePlayableAccess::OriginalLifetimeMarker(table, original.childOrigin);
    harken::PlayableInstanceTable::OriginalLifetimeRetirementHold held;
    EXPECT_FALSE(table.BeginOriginalLifetimeRetirement(original.childOrigin, &held));
    EXPECT_FALSE(held.Valid());
    EXPECT_TRUE(table.IsOriginalLifetimeCurrent(original.childOrigin));
    original.Commit();
    NativePlayablePin foreign;
    ASSERT_TRUE(original.childOrigin.Borrow(&foreign));
    ASSERT_TRUE(table.AcquireSourceRetirementHold(child, table.Classify(child).scheduleEpoch));
    ASSERT_TRUE(table.AdvanceLifecycle(child, harken::PlayableLifecycle::LogicalComplete));
    EXPECT_FALSE(other.BeginOriginalLifetimeRetirement(original.childOrigin, &held));
    EXPECT_FALSE(table.BeginOriginalLifetimeRetirement(original.rootOrigin, &held));
    EXPECT_FALSE(table.BeginOriginalLifetimeRetirement(original.childOrigin, &held));
    EXPECT_TRUE(table.IsOriginalLifetimeCurrent(original.childOrigin));
    EXPECT_EQ(2u, NativePlayableAccess::OriginalLifetimeCount(table, original.childOrigin));
    ASSERT_TRUE(table.ReleaseSourceRetirementHold(child));
    ASSERT_TRUE(table.BeginOriginalLifetimeRetirement(original.childOrigin, &held));
    EXPECT_TRUE(held.Valid());
    EXPECT_EQ(child, held.OriginalHandle());
    EXPECT_TRUE(table.IsOriginalLifetimeRetirementHoldFor(held, child));
    EXPECT_FALSE(table.IsOriginalLifetimeRetirementHoldFor(held, root));
    EXPECT_FALSE(other.IsOriginalLifetimeRetirementHoldFor(held, child));
    EXPECT_EQ(marker, NativePlayableAccess::OriginalLifetimeMarker(table, original.childOrigin));
    EXPECT_EQ(3u, NativePlayableAccess::OriginalLifetimeCount(table, original.childOrigin));
    EXPECT_FALSE(table.IsOriginalLifetimeCurrent(foreign));
    EXPECT_TRUE(foreign.Valid());
    EXPECT_TRUE(table.HasRequestedOriginalLifetimeDisposition(original.childOrigin));
    EXPECT_FALSE(table.TryFinishOriginalLifetimeDisposition(original.childOrigin));
    EXPECT_FALSE(table.AdvanceLifecycle(child, harken::PlayableLifecycle::Retired, &original.childOrigin));
    EXPECT_EQ(child, table.FirstChild(root));
    EXPECT_TRUE(table.Classify(child).live);
    // The hold preserves storage, not epochs or ordinary terminal progress.
    const auto epoch = table.BumpScheduleEpochChecked(child);
    ASSERT_EQ(harken::IssueStatus::Ok, epoch.status);
    ASSERT_TRUE(table.SetTerminalOutcome(child, {harken::PlayableTerminalKind::RequestedStop, 0, 1}));
    ASSERT_TRUE(table.AdvanceLifecycle(child, harken::PlayableLifecycle::AudibleRetiring));
    ASSERT_TRUE(table.AcquireSourceRetirementHold(child, epoch.value));
    foreign.Release();
    EXPECT_FALSE(table.TryFinishOriginalLifetimeDisposition(original.childOrigin));
    held.Release();
    EXPECT_FALSE(held.Valid());
    EXPECT_FALSE(table.TryFinishOriginalLifetimeDisposition(original.childOrigin));
    EXPECT_TRUE(original.childOrigin.Valid());
    EXPECT_TRUE(table.HasSourceRetirementHold(child));
    ASSERT_TRUE(table.ReleaseSourceRetirementHold(child));
    ASSERT_TRUE(table.TryFinishOriginalLifetimeDisposition(original.childOrigin));
    EXPECT_FALSE(table.Classify(child).live);
    EXPECT_FALSE(original.childOrigin.Valid());
    EXPECT_EQ(0u, table.FirstChild(root));
    ASSERT_TRUE(table.AdvanceLifecycle(root, harken::PlayableLifecycle::Retired, &original.rootOrigin));
    EXPECT_FALSE(original.rootOrigin.Valid());
    EXPECT_EQ(2u, table.FreeCount());
    EXPECT_FALSE(table.IsOriginalLifetimeRetirementHoldFor(held, child));
}

TEST(PlayableInstanceTable, OriginalRetirementHoldMoveOverwriteAndDestructorReturnTheRealNativeLoans) {
    harken::PlayableInstanceTable first(1), second(1);
    GenuinePlayableLifetimeTree a(first), b(second);
    ASSERT_TRUE(a.Prepare(false));
    ASSERT_TRUE(b.Prepare(false));
    ASSERT_TRUE(a.Issue());
    ASSERT_TRUE(b.Issue());
    const auto aHandle = a.root.Handle();
    const auto bHandle = b.root.Handle();
    a.Commit();
    b.Commit();
    ASSERT_TRUE(first.AdvanceLifecycle(aHandle, harken::PlayableLifecycle::LogicalComplete));
    ASSERT_TRUE(second.AdvanceLifecycle(bHandle, harken::PlayableLifecycle::LogicalComplete));
    {
        harken::PlayableInstanceTable::OriginalLifetimeRetirementHold aHold, bHold;
        ASSERT_TRUE(first.BeginOriginalLifetimeRetirement(a.rootOrigin, &aHold));
        ASSERT_TRUE(second.BeginOriginalLifetimeRetirement(b.rootOrigin, &bHold));
        EXPECT_FALSE(first.BeginOriginalLifetimeRetirement(a.rootOrigin, &bHold));
        EXPECT_EQ(2u, NativePlayableAccess::OriginalLifetimeCount(first, a.rootOrigin));
        EXPECT_EQ(2u, NativePlayableAccess::OriginalLifetimeCount(second, b.rootOrigin));
        aHold = std::move(bHold);
        EXPECT_FALSE(bHold.Valid());
        EXPECT_TRUE(aHold.Valid());
        EXPECT_TRUE(second.IsOriginalLifetimeRetirementHoldFor(aHold, bHandle));
        EXPECT_FALSE(first.IsOriginalLifetimeRetirementHoldFor(aHold, aHandle));
        EXPECT_EQ(1u, NativePlayableAccess::OriginalLifetimeCount(first, a.rootOrigin));
        ASSERT_TRUE(first.TryFinishOriginalLifetimeDisposition(a.rootOrigin));
        EXPECT_FALSE(first.Classify(aHandle).live);
        EXPECT_FALSE(second.TryFinishOriginalLifetimeDisposition(b.rootOrigin));
    }
    EXPECT_EQ(1u, NativePlayableAccess::OriginalLifetimeCount(second, b.rootOrigin));
    ASSERT_TRUE(second.TryFinishOriginalLifetimeDisposition(b.rootOrigin));
    EXPECT_FALSE(second.Classify(bHandle).live);
    EXPECT_EQ(1u, first.FreeCount());
    EXPECT_EQ(1u, second.FreeCount());
    harken::PreparedPlayableInstance reused;
    ASSERT_TRUE(first.PrepareAcquire(harken::PlayableKind::Sound, 0, &reused));
    EXPECT_EQ(static_cast<std::uint32_t>(aHandle), static_cast<std::uint32_t>(reused.Handle()));
    EXPECT_NE(aHandle, reused.Handle());
    EXPECT_FALSE(first.IsOriginalLifetimePinFor(a.rootOrigin, reused.Handle()));
    ASSERT_TRUE(first.CancelPrepared(&reused));
}
} // namespace
