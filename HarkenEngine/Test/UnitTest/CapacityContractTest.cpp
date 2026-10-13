// Pins the capacity contract's declared bounds. Every capacity the engine
// publishes as a configurable quantity owes callers two facts that no other
// test states: the largest value it accepts, and that one past it is refused
// with a defined code rather than attempted. These tests are the reason those
// two numbers cannot drift silently -- a resolver that lost a bound, or a
// bound that moved, fails here and nowhere else.
#include "Common/ObjectIdTestSupport.h"
#include <harken/Harken.h>

#include "Common/TestUtil.h"
#include "Engine.h"

#include "harken/core/EngineConfig.h"
#include "harken/core/LogRing.h"
#include "harken/core/NotificationQueue.h"
#include "harken/core/ScheduledTransactionPool.h"
#include "harken/render/FrontierTimelineFinalizer.h"

#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <new>
#include <type_traits>
#include <vector>

namespace harken::detail {
struct EnginePlayablePartitionTestAccess {
    static EnginePlayableServiceToken ChangeOwner(EnginePlayablePartitionPool& pool,
        EnginePlayablePartitionToken token, EnginePlayableServiceToken replacement) {
        auto& slot = pool.slots_[token.slot];
        const auto original = slot.owner;
        slot.owner = replacement;
        return original;
    }
    static void SeedFreeStorageIssuer(EnginePlayablePartitionPool& pool, std::uint64_t marker) {
        EXPECT_EQ(0u, pool.OccupiedCount());
        pool.nextOriginalStorageMarker_ = marker;
    }
    static void SeedFreeGeneration(EnginePlayablePartitionPool& pool, unsigned index,
        std::uint64_t generation) {
        auto& slot = pool.slots_[index];
        EXPECT_FALSE(slot.occupied);
        slot.generation = generation;
    }
};
}

namespace {

using PartitionPool = harken::EnginePlayablePartitionPool;
static_assert(!std::is_aggregate_v<PartitionPool::ReservationInput>);
static_assert(!std::is_constructible_v<PartitionPool::ReservationInput,
    PartitionPool*, harken::EnginePlayablePartitionToken, harken::EnginePlayableServiceToken>);
static_assert(!std::is_copy_constructible_v<PartitionPool>);
static_assert(!std::is_move_constructible_v<PartitionPool>);
static_assert(!std::is_copy_constructible_v<PartitionPool::PreparedCommitProof>);

static_assert(!std::is_copy_constructible_v<PartitionPool::OriginalStoragePin>);
static_assert(!std::is_aggregate_v<PartitionPool::OriginalStoragePin>);

TEST(EnginePlayablePartitionStorage, RealStorageLoansLeaveNativeProgressAndFenceIndependent) {
    PartitionPool pool(1);
    const harken::EnginePlayableServiceToken owner{2, 7};
    PartitionPool::ReservationInput original;
    PartitionPool::OriginalStoragePin baseline, loan;
    const auto token = pool.Reserve(owner, &original, &baseline);
    ASSERT_TRUE(token.Valid());
    ASSERT_TRUE(baseline.Valid());
    ASSERT_TRUE(pool.BorrowOriginalStorage(baseline, &loan));
    EXPECT_FALSE(pool.OriginalStorageReclaimReady(baseline));
    EXPECT_FALSE(pool.OriginalStorageReclaimReady(loan));
    EXPECT_FALSE(pool.Cancel(token, owner));
    PartitionPool::PreparedCommitProof proof;
    ASSERT_TRUE(pool.ProvePreparedCommit(original, &proof));
    PartitionPool::ConsumePreparedCommit(std::move(proof));
    ASSERT_TRUE(pool.Publish(token, owner));
    ASSERT_TRUE(pool.BeginRetirement(token, owner, 90));
    EXPECT_FALSE(pool.CanRelease(token, owner, 90));
    EXPECT_TRUE(pool.CanRelease(token, owner, 91));
    EXPECT_FALSE(pool.Release(token, owner, 91));
    EXPECT_TRUE(pool.CanRelease(token, owner, 91));
    ASSERT_TRUE(loan.Valid());
    loan.Return();
    ASSERT_TRUE(pool.OriginalStorageReclaimReady(baseline));
    EXPECT_FALSE(pool.Release(token, owner, 91));
    baseline.Return();
    EXPECT_FALSE(baseline.Valid());
    EXPECT_FALSE(pool.Release(token, owner, 90));
    ASSERT_TRUE(pool.Release(token, owner, 91));
    const auto reused = pool.Reserve(owner);
    EXPECT_EQ(token.slot, reused.slot);
    EXPECT_GT(reused.generation, token.generation);
    EXPECT_FALSE(pool.OriginalStorageMatches(baseline, reused, owner));
    ASSERT_TRUE(pool.Cancel(reused, owner));
}

TEST(EnginePlayablePartitionStorage, ActualMoveReturnAndForeignReceiverDoNotMintOrLeak) {
    PartitionPool first(1), second(1);
    const harken::EnginePlayableServiceToken owner{0, 1};
    PartitionPool::OriginalStoragePin a, b, loan, otherLoan;
    const auto at = first.Reserve(owner, nullptr, &a);
    const auto bt = second.Reserve(owner, nullptr, &b);
    ASSERT_TRUE(a.Valid());
    ASSERT_TRUE(b.Valid());
    EXPECT_FALSE(first.BorrowOriginalStorage(b, &loan));
    ASSERT_TRUE(first.BorrowOriginalStorage(a, &loan));
    EXPECT_FALSE(second.BorrowOriginalStorage(b, &loan));
    ASSERT_TRUE(second.BorrowOriginalStorage(b, &otherLoan));
    loan = std::move(otherLoan);
    EXPECT_FALSE(otherLoan.Valid());
    EXPECT_TRUE(first.OriginalStorageReclaimReady(a));
    EXPECT_FALSE(second.OriginalStorageReclaimReady(b));
    EXPECT_FALSE(second.OriginalStorageMatches(loan, bt, {0, 2}));
    loan.Return();
    EXPECT_TRUE(second.OriginalStorageReclaimReady(b));
    a.Return();
    b.Return();
    ASSERT_TRUE(first.Cancel(at, owner));
    ASSERT_TRUE(second.Cancel(bt, owner));
    EXPECT_EQ(0u, first.OccupiedCount());
    EXPECT_EQ(0u, second.OccupiedCount());
}

TEST(EnginePlayablePartitionStorage, LastTrueFactoryMarkerEndsWithoutWrapOrRegrant) {
    PartitionPool pool(1);
    const harken::EnginePlayableServiceToken owner{0, 1};
    harken::detail::EnginePlayablePartitionTestAccess::SeedFreeStorageIssuer(pool, UINT64_MAX - 1u);
    PartitionPool::OriginalStoragePin original, foreign, refused;
    const auto token = pool.Reserve(owner, nullptr, &original);
    ASSERT_TRUE(token.Valid());
    ASSERT_TRUE(original.Valid());
    ASSERT_TRUE(pool.BorrowOriginalStorage(original, &foreign));
    EXPECT_TRUE(pool.OriginalStorageIssuerExhausted());
    foreign.Return();
    original.Return();
    ASSERT_TRUE(pool.Cancel(token, owner));
    EXPECT_FALSE(pool.Reserve(owner, nullptr, &refused).Valid());
    EXPECT_FALSE(refused.Valid());
    EXPECT_EQ(0u, pool.OccupiedCount());
    const auto legacy = pool.Reserve(owner);
    ASSERT_TRUE(legacy.Valid());
    ASSERT_TRUE(pool.Cancel(legacy, owner));
}

TEST(EnginePlayablePartitionProof, ActualHeldSlotBlocksMutatorsAndReceiptReplacement) {
    PartitionPool pool(2);
    const harken::EnginePlayableServiceToken firstOwner{0, 1}, secondOwner{1, 1};
    PartitionPool::ReservationInput first, second, duplicate;
    const auto firstToken = pool.Reserve(firstOwner, &first);
    ASSERT_TRUE(firstToken.Valid());
    ASSERT_EQ(0u, firstToken.slot);
    EXPECT_FALSE(pool.Reserve(firstOwner, &duplicate).Valid());
    EXPECT_FALSE(duplicate.Valid());
    PartitionPool::PreparedCommitProof proof, aliasProof;
    ASSERT_TRUE(pool.ProvePreparedCommit(first, &proof));
    EXPECT_FALSE(pool.Reserve(secondOwner, &first).Valid());
    EXPECT_EQ(1u, pool.OccupiedCount());
    const auto secondToken = pool.Reserve(secondOwner, &second);
    ASSERT_EQ(1u, secondToken.slot);
    const auto alias = first;
    auto movedInput = std::move(first);
    EXPECT_EQ(firstToken.slot, first.Token().slot);
    EXPECT_TRUE(proof.Associated(movedInput));
    EXPECT_FALSE(pool.ProvePreparedCommit(alias, &aliasProof));
    first = second;
    first = std::move(second);
    EXPECT_EQ(firstToken.slot, first.Token().slot);
    EXPECT_EQ(secondToken.slot, second.Token().slot);
    EXPECT_FALSE(first.Reset());
    EXPECT_TRUE(pool.Owns(firstToken, firstOwner));
    EXPECT_FALSE(pool.Commit(firstToken, firstOwner));
    EXPECT_FALSE(pool.Cancel(firstToken, firstOwner));
    EXPECT_FALSE(pool.RollbackCommit(firstToken, firstOwner));
    EXPECT_FALSE(pool.Publish(firstToken, firstOwner));
    EXPECT_FALSE(pool.BeginRetirement(firstToken, firstOwner, 10));
    EXPECT_FALSE(pool.DeferRetirement(firstToken, firstOwner, 10));
    EXPECT_FALSE(pool.CanRelease(firstToken, firstOwner, 11));
    EXPECT_FALSE(pool.Release(firstToken, firstOwner, 11));
    EXPECT_FALSE(pool.Published(firstToken, firstOwner));
    EXPECT_EQ(2u, pool.OccupiedCount());
    ASSERT_TRUE(pool.ProvePreparedCommit(second, &aliasProof));
    aliasProof = std::move(proof);
    EXPECT_FALSE(proof.Valid());
    EXPECT_TRUE(aliasProof.Associated(first));
    EXPECT_TRUE(pool.CommitProofHeld(firstToken, firstOwner));
    EXPECT_FALSE(pool.CommitProofHeld(secondToken, secondOwner));
    ASSERT_TRUE(pool.Cancel(secondToken, secondOwner));
    aliasProof.Release();
    ASSERT_TRUE(pool.ProvePreparedCommit(alias, &proof));
    PartitionPool::ConsumePreparedCommit(std::move(proof));
    EXPECT_FALSE(proof.Valid());
    EXPECT_FALSE(pool.CommitProofHeld(firstToken, firstOwner));
    ASSERT_TRUE(pool.RollbackCommit(firstToken, firstOwner));
    EXPECT_FALSE(pool.ProvePreparedCommit(first, &proof));
    ASSERT_TRUE(pool.Cancel(firstToken, firstOwner));
    EXPECT_EQ(0u, pool.OccupiedCount());
    ASSERT_TRUE(first.Reset());
}

TEST(EnginePlayablePartitionProof, GenuineForeignAndChangedOriginalOwnerRefuseAndReleasePrefix) {
    PartitionPool pool(2), foreign(1);
    const harken::EnginePlayableServiceToken firstOwner{0, 1}, secondOwner{1, 1}, replacement{2, 1};
    PartitionPool::ReservationInput first, second, foreignInput;
    const auto firstToken = pool.Reserve(firstOwner, &first);
    const auto secondToken = pool.Reserve(secondOwner, &second);
    const auto foreignToken = foreign.Reserve(firstOwner, &foreignInput);
    ASSERT_TRUE(firstToken.Valid());
    ASSERT_TRUE(secondToken.Valid());
    ASSERT_TRUE(foreignToken.Valid());
    PartitionPool::PreparedCommitProof prefix, tail;
    EXPECT_FALSE(pool.ProvePreparedCommit(foreignInput, &tail));
    ASSERT_TRUE(pool.ProvePreparedCommit(first, &prefix));
    const auto saved = harken::detail::EnginePlayablePartitionTestAccess::ChangeOwner(pool, secondToken, replacement);
    EXPECT_FALSE(pool.ProvePreparedCommit(second, &tail));
    EXPECT_TRUE(prefix.Associated(first));
    EXPECT_FALSE(tail.Valid());
    prefix.Release();
    EXPECT_FALSE(pool.CommitProofHeld(firstToken, firstOwner));
    harken::detail::EnginePlayablePartitionTestAccess::ChangeOwner(pool, secondToken, saved);
    ASSERT_TRUE(pool.ProvePreparedCommit(first, &prefix));
    ASSERT_TRUE(pool.ProvePreparedCommit(second, &tail));
    PartitionPool::ConsumePreparedCommit(std::move(prefix));
    PartitionPool::ConsumePreparedCommit(std::move(tail));
    ASSERT_TRUE(pool.RollbackCommit(firstToken, firstOwner));
    ASSERT_TRUE(pool.RollbackCommit(secondToken, secondOwner));
    ASSERT_TRUE(pool.Cancel(firstToken, firstOwner));
    ASSERT_TRUE(pool.Cancel(secondToken, secondOwner));
    ASSERT_TRUE(foreign.Cancel(foreignToken, firstOwner));
    EXPECT_EQ(0u, pool.OccupiedCount());
    EXPECT_EQ(0u, foreign.OccupiedCount());
}

TEST(EnginePlayablePartitionProof, ActualCancelReuseAndMaxQuarantineRejectOldFactoryIssuance) {
    PartitionPool pool(2);
    const harken::EnginePlayableServiceToken owner{0, 1};
    harken::detail::EnginePlayablePartitionTestAccess::SeedFreeGeneration(pool, 0, UINT64_MAX);
    PartitionPool::ReservationInput exhausted, prior, fresh;
    const auto exhaustedToken = pool.Reserve(owner, &exhausted);
    ASSERT_EQ(UINT64_MAX, exhaustedToken.generation);
    ASSERT_TRUE(pool.Cancel(exhaustedToken, owner));
    const auto priorToken = pool.Reserve(owner, &prior);
    ASSERT_EQ(1u, priorToken.slot);
    ASSERT_TRUE(pool.Cancel(priorToken, owner));
    const auto freshToken = pool.Reserve(owner, &fresh);
    ASSERT_EQ(priorToken.slot, freshToken.slot);
    ASSERT_EQ(priorToken.generation + 1u, freshToken.generation);
    PartitionPool::PreparedCommitProof proof;
    EXPECT_FALSE(pool.ProvePreparedCommit(exhausted, &proof));
    EXPECT_FALSE(pool.ProvePreparedCommit(prior, &proof));
    EXPECT_FALSE(pool.Cancel(priorToken, owner));
    ASSERT_TRUE(pool.ProvePreparedCommit(fresh, &proof));
    proof.Release();
    ASSERT_TRUE(pool.Cancel(freshToken, owner));
    EXPECT_EQ(0u, pool.OccupiedCount());
}

TEST(EnginePlayablePartitionProof, ConsumedIssuanceNeverRegrantsAndPublishedFenceRemainsStrict) {
    PartitionPool pool(1);
    const harken::EnginePlayableServiceToken owner{0, 1};
    PartitionPool::ReservationInput original;
    const auto token = pool.Reserve(owner, &original);
    ASSERT_TRUE(token.Valid());
    const auto alias = original;
    PartitionPool::PreparedCommitProof proof;
    ASSERT_TRUE(pool.ProvePreparedCommit(original, &proof));
    PartitionPool::ConsumePreparedCommit(std::move(proof));
    EXPECT_FALSE(pool.ProvePreparedCommit(alias, &proof));
    ASSERT_TRUE(pool.RollbackCommit(token, owner));
    EXPECT_FALSE(pool.ProvePreparedCommit(original, &proof));
    ASSERT_TRUE(pool.Commit(token, owner));
    ASSERT_TRUE(pool.Commit(token, owner));
    ASSERT_TRUE(pool.Publish(token, owner));
    EXPECT_FALSE(pool.RollbackCommit(token, owner));
    EXPECT_FALSE(pool.Cancel(token, owner));
    ASSERT_TRUE(pool.BeginRetirement(token, owner, 10));
    EXPECT_FALSE(pool.Release(token, owner, 10));
    ASSERT_TRUE(pool.DeferRetirement(token, owner, 12));
    EXPECT_FALSE(pool.Release(token, owner, 12));
    ASSERT_TRUE(pool.Release(token, owner, 13));
    EXPECT_FALSE(pool.Owns(token, owner));
    EXPECT_FALSE(pool.ProvePreparedCommit(alias, &proof));
    EXPECT_EQ(0u, pool.OccupiedCount());
}

VnConfig MakeNullConfig() {
    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.deviceMode = VN_DEVICE_NULL;
    return config;
}

TEST(CapacityContract, StartupDefaultsAre256AndExplicitCapacitiesRemainEffective) {
    VnConfig config = MakeNullConfig();
    harken::EngineConfig resolved;
    ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &resolved));
    EXPECT_EQ(256u, resolved.voiceCapacity.Render());
    EXPECT_EQ(256u, resolved.maxPacks);
    EXPECT_EQ(256u, resolved.maxActions);
    EXPECT_EQ(256u, resolved.maxActionFirings);

    config.maxVoices = 128;
    config.maxPacks = 8;
    config.maxActions = 32;
    config.maxActionFirings = 64;
    ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &resolved));
    EXPECT_EQ(128u, resolved.voiceCapacity.Render());
    EXPECT_EQ(8u, resolved.maxPacks);
    EXPECT_EQ(32u, resolved.maxActions);
    EXPECT_EQ(64u, resolved.maxActionFirings);
}

template <typename T>
std::uint64_t StorageBytes(const std::vector<T>& storage) {
    return static_cast<std::uint64_t>(storage.capacity()) * sizeof(T);
}

// The MixQuantum frame count is the existing blockFrames field, not a second
// frame count alongside it, so this is where its bound is pinned. Both sides
// of the boundary are asserted from one config that differs in nothing else:
// accepting at the ceiling is what makes the refusal above it attributable to
// this field, rather than to a derived budget that happened to overflow at a
// large block size.
TEST(CapacityContract, EmitterVariableBankUsesCheckedByteBudget) {
    harken::EngineConfig resolved;
    VnConfig config = MakeNullConfig();
    ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &resolved));
    EXPECT_EQ(128u, resolved.maxEmitters);
    EXPECT_EQ(8u, resolved.emitterVariableSlots);

    config.maxEmitters = 129;
    config.emitterVariableSlots = 17;
    ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &resolved));
    EXPECT_EQ(129u, resolved.maxEmitters);
    EXPECT_EQ(17u, resolved.emitterVariableSlots);

    config.maxEmitters = 65536;
    config.emitterVariableSlots = 8;
    EXPECT_EQ(VN_OK, harken::ResolveConfig(&config, &resolved));

    config.emitterVariableSlots = 4096;
    EXPECT_EQ(VN_INVALID_CONFIG, harken::ResolveConfig(&config, &resolved));
}

TEST(CapacityContract, BlockFramesAcceptsItsCeilingAndRefusesOnePastIt) {
    VnConfig config = MakeNullConfig();
    harken::EngineConfig resolved;

    config.blockFrames = 1u << 16;
    ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &resolved))
        << "the documented ceiling itself must resolve";
    EXPECT_EQ(1u << 16, resolved.blockFrames);

    config.blockFrames = (1u << 16) + 1u;
    EXPECT_EQ(VN_INVALID_CONFIG, harken::ResolveConfig(&config, &resolved))
        << "one frame past the ceiling must be refused, not attempted";
}

// A zero field means "use the documented default" across this whole struct, so
// the frame count has no lower bound of its own to pin -- but the default it
// resolves to is part of the contract, and a caller reading the header's
// "0 = 256" has nothing else asserting it.
TEST(CapacityContract, BlockFramesZeroResolvesToTheDocumentedDefault) {
    VnConfig config = MakeNullConfig();
    harken::EngineConfig resolved;

    config.blockFrames = 0;
    ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &resolved));
    EXPECT_EQ(256u, resolved.blockFrames);
}

TEST(CapacityContract,
     ScheduleHorizonResolvesToWholeBlockQuantumsAndResetsPool) {
    struct Case {
        std::uint32_t blockFrames;
        std::uint64_t expectedHorizon;
    };
    constexpr std::array<Case, 3> cases = {{
        {960, 17'280},
        {127, 16'510},
        {64, 16'384},
    }};

    for (const Case& c : cases) {
        VnConfig config = MakeNullConfig();
        config.blockFrames = c.blockFrames;
        harken::EngineConfig resolved;
        ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &resolved))
            << "blockFrames=" << c.blockFrames;
        EXPECT_EQ(resolved.scheduleHorizonFrames, c.expectedHorizon)
            << "blockFrames=" << c.blockFrames;
        EXPECT_EQ(resolved.scheduleHorizonFrames % resolved.blockFrames, 0u)
            << "blockFrames=" << c.blockFrames;

        harken::ScheduledTransactionPool::Config poolConfig;
        poolConfig.quantumFrames = resolved.blockFrames;
        poolConfig.scheduleHorizonFrames = resolved.scheduleHorizonFrames;
        poolConfig.publishLeadQuanta = resolved.publishLeadQuanta;
        poolConfig.maxScheduledTransactions =
            resolved.maxScheduledTransactions;
        poolConfig.maxTransactionSpanFrames =
            resolved.maxTransactionSpanFrames;
        poolConfig.maxIngressTransactionsPerQuantum =
            resolved.maxIngressTransactionsPerQuantum;
        poolConfig.maxFrameBatchesPerQuantum =
            resolved.maxFrameBatchesPerQuantum;
        poolConfig.maxEventsPerQuantum = resolved.maxEventsPerQuantum;
        poolConfig.maxTransactionEvents = resolved.maxTransactionEvents;
        harken::ScheduledTransactionPool pool;
        EXPECT_TRUE(pool.Reset(poolConfig, 0, 1u))
            << "blockFrames=" << c.blockFrames;
    }
}

// The per-block control-command ceiling. Its bound is shared with the queue
// depths because it is counted in the same unit: no block can apply more
// commands than the queue can hold, so the largest admissible queue is also the
// largest allowance that can ever bind.
TEST(CapacityContract, ControlCommandCeilingAcceptsItsBoundAndRefusesOnePastIt) {
    VnConfig config = MakeNullConfig();
    harken::EngineConfig resolved;

    config.maxControlCommandsPerQuantum = 1u << 20;
    ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &resolved));
    EXPECT_EQ(1u << 20, resolved.maxControlCommandsPerQuantum);

    config.maxControlCommandsPerQuantum = (1u << 20) + 1u;
    EXPECT_EQ(VN_INVALID_CONFIG, harken::ResolveConfig(&config, &resolved));
}

// Without this, the field could be validated and then never read, and every
// other assertion here would still hold: a caller would set a ceiling, get
// VN_OK, and silently run on the default. A value that survives to the resolved
// config is the only proof the plumbing exists.
TEST(CapacityContract, ControlCommandCeilingIsTakenFromTheCallerAndZeroDefaults) {
    VnConfig config = MakeNullConfig();
    harken::EngineConfig resolved;

    config.maxControlCommandsPerQuantum = 64;
    ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &resolved));
    EXPECT_EQ(64u, resolved.maxControlCommandsPerQuantum)
        << "the caller's ceiling did not reach the resolved config";

    // Zero selects the engine default across this whole struct, so it is not a
    // refusal here either -- a ceiling below what a caller pushes costs latency,
    // not correctness, because over-ceiling commands defer rather than drop.
    config.maxControlCommandsPerQuantum = 0;
    ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &resolved));
    EXPECT_EQ(256u, resolved.maxControlCommandsPerQuantum);
}

// The field occupies a slot that older callers do not declare, so a shorter
// struct must read the default no matter what bytes happen to sit there. The
// value planted here is past the ceiling: if the vintage gate ever stopped
// guarding this field, the config would be refused instead of quietly
// defaulting, which is the failure this pins.
TEST(CapacityContract, ControlCommandCeilingIsIgnoredForAnOlderVintage) {
    VnConfig config = MakeNullConfig();
    config.maxControlCommandsPerQuantum = (1u << 20) + 1u;
    config.structSize =
        static_cast<std::uint32_t>(offsetof(VnConfig, maxBusTopologyOwners));

    harken::EngineConfig resolved;
    ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &resolved))
        << "an older vintage was judged on a field it never declared";
    EXPECT_EQ(256u, resolved.maxControlCommandsPerQuantum);
}

TEST(CapacityContract, ActionFiringCapacityDefaultsAndStopsAtItsHardCeiling) {
    VnConfig config = MakeNullConfig();
    harken::EngineConfig resolved;

    ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &resolved));
    EXPECT_EQ(256u, resolved.maxActionFirings);

    config.maxActionFirings = 4096;
    ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &resolved));
    EXPECT_EQ(4096u, resolved.maxActionFirings);

    config.maxActionFirings = 4097;
    EXPECT_EQ(VN_INVALID_CONFIG, harken::ResolveConfig(&config, &resolved));
}

TEST(CapacityContract,
     ActionCooldownCapacityIsVintageAwareAndStopsAtItsHardCeiling) {
    VnConfig config = MakeNullConfig();
    harken::EngineConfig resolved;

    ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &resolved));
    EXPECT_EQ(256u, resolved.maxActionCooldownRows);

    config.maxActionCooldownRows = 4096;
    ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &resolved));
    EXPECT_EQ(4096u, resolved.maxActionCooldownRows);

    config.maxActionCooldownRows = 4097;
    EXPECT_EQ(VN_INVALID_CONFIG, harken::ResolveConfig(&config, &resolved));

    config.structSize =
        static_cast<std::uint32_t>(offsetof(VnConfig, maxActionCooldownRows));
    ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &resolved));
    EXPECT_EQ(256u, resolved.maxActionCooldownRows);
}

TEST(CapacityContract, ActionCooldownCapacityPreallocatesAtInit) {
    VnConfig config = MakeNullConfig();
    config.maxActionCooldownRows = 3;

    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    EXPECT_EQ(3u, session.Engine()->actionCooldowns.Capacity());
}

TEST(CapacityContract, CompositeActionInternalDefaultsCoverBothLeafPlanes) {
    VnConfig config = MakeNullConfig();
    harken::EngineConfig resolved;

    ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &resolved));
    EXPECT_EQ(64u, resolved.maxKnownScheduleLeaves);
    EXPECT_EQ(128u, resolved.maxIngressTransactionsPerQuantum);
    EXPECT_EQ(128u, resolved.maxFrameBatchesPerQuantum);
    EXPECT_EQ(128u, resolved.maxArmsPerGroup);

    const std::uint64_t knownLeaves = resolved.maxKnownScheduleLeaves;
    EXPECT_GE(resolved.maxArmsPerGroup, 2u * knownLeaves);
    EXPECT_GE(resolved.maxIngressTransactionsPerQuantum,
              2u * knownLeaves);
    EXPECT_GE(resolved.maxFrameBatchesPerQuantum, knownLeaves + 2u);
}

TEST(CapacityContract, PlayableStorageUsesSevenCheckedFamilies) {
    VnConfig config = MakeNullConfig();
    harken::EngineConfig resolved;
    ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &resolved));

    const harken::PlayableStorageLayout layout(resolved);
    EXPECT_EQ(192u, layout.memberStride);
    EXPECT_EQ(128u, layout.pairedStride);
    EXPECT_EQ(128u, layout.currentEventStride);
    EXPECT_EQ(96u, layout.initialProgramStride);
    EXPECT_EQ(64u, layout.endpointStride);
    EXPECT_EQ(128u, layout.weightStride);
    EXPECT_EQ(32u, layout.continuousStride);

    EXPECT_EQ(49'152u, layout.memberCount);
    EXPECT_EQ(32'768u, layout.pairedCount);
    EXPECT_EQ(32'768u, layout.currentEventCount);
    EXPECT_EQ(24'576u, layout.initialProgramCount);
    EXPECT_EQ(16'384u, layout.endpointCount);
    EXPECT_EQ(32'768u, layout.weightCount);
    EXPECT_EQ(8'192u, layout.continuousCount);

    EXPECT_EQ(64u, layout.PackedDiscreteTransitionOffset(128u));
    EXPECT_EQ(127u, layout.PackedDiscreteTransitionOffset(191u));

    harken::EngineConfig overflow = resolved;
    overflow.maxKnownScheduleLeaves =
        std::numeric_limits<std::uint32_t>::max();
    EXPECT_THROW(
        {
            const harken::PlayableStorageLayout rejected(overflow);
            (void)rejected;
        },
        std::bad_alloc);
}

TEST(CapacityContract, PlayableStoragePartitionsAllMemberAndScheduleVectors) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(2, 256, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_NE(nullptr, engine);
    const harken::PlayableStorageLayout& layout = engine->playableStorage;
    EXPECT_EQ(engine->config.maxPlayableInstances, engine->playableStarts.size());
    EXPECT_EQ(engine->config.maxPlayableInstances, engine->playableServices.size());
    EXPECT_EQ(engine->config.maxPlayableInstances, engine->playablePartitions.Capacity());
    EXPECT_EQ(engine->config.maxContainerInstances, engine->queueInstances.Capacity());
    EXPECT_EQ(engine->config.maxContainerInstances, engine->variableControlInstances.Capacity());

    EXPECT_EQ(layout.memberCount, engine->playableScheduleVoices.size());
    EXPECT_EQ(layout.memberCount, engine->playableScheduleLeaves.size());
    EXPECT_EQ(layout.memberCount, engine->playableScheduleArms.size());
    EXPECT_EQ(layout.memberCount,
              engine->playableScheduleRenderHandles.size());
    EXPECT_EQ(layout.memberCount, engine->playableScheduleReleaseObservers.size());
    EXPECT_EQ(layout.memberCount, engine->variableControlMemberCompletions.size());
    EXPECT_EQ(layout.memberCount, engine->playableMemberReferences.size());
    EXPECT_EQ(engine->config.maxPlayableInstances, engine->sourceTerminals.Capacity());
    EXPECT_EQ(layout.memberCount, engine->playableScheduleStartFrames.size());
    EXPECT_EQ(layout.memberCount,
              engine->variableControlDynamicMembers.size());

    EXPECT_EQ(layout.pairedCount,
              engine->playableScheduleCompletions.size());
    EXPECT_EQ(layout.pairedCount,
              engine->playableSchedulePostSourceTailFrames.size());
    EXPECT_EQ(layout.pairedCount, engine->playableSuccessorPayloads.size());
    EXPECT_EQ(layout.pairedCount, engine->playableSuccessorEvents.size());

    EXPECT_EQ(layout.currentEventCount,
              engine->playableSchedulePayloads.size());
    EXPECT_EQ(layout.currentEventCount, engine->playableScheduleEvents.size());

    EXPECT_EQ(layout.initialProgramCount,
              engine->variableControlRuntimePrograms.size());
    EXPECT_EQ(layout.initialProgramCount,
              engine->variableControlSchedules.size());

    EXPECT_EQ(layout.endpointCount,
              engine->playableScheduleRuntimePrograms.size());
    EXPECT_EQ(layout.endpointCount, engine->playableSchedules.size());
    EXPECT_EQ(layout.endpointCount,
              engine->playableScheduleOccurrences.size());
    EXPECT_EQ(layout.endpointCount, engine->playableSuccessorVoices.size());
    EXPECT_EQ(layout.endpointCount, engine->playableSuccessorLeaves.size());
    EXPECT_EQ(layout.endpointCount,
              engine->playableSuccessorRuntimePrograms.size());
    EXPECT_EQ(layout.endpointCount, engine->playableSuccessorSchedules.size());
    EXPECT_EQ(layout.endpointCount, engine->playableSuccessorArms.size());
    EXPECT_EQ(layout.endpointCount,
              engine->playableSuccessorRenderHandles.size());
    EXPECT_EQ(layout.endpointCount,
              engine->playableSuccessorOccurrences.size());
    EXPECT_EQ(layout.endpointCount,
              engine->playableSuccessorCompletions.size());
    EXPECT_EQ(layout.endpointCount,
              engine->playableSuccessorStartFrames.size());
    EXPECT_EQ(layout.endpointCount,
              engine->playableSuccessorPostSourceTailFrames.size());

    EXPECT_EQ(layout.weightCount,
              engine->variableControlWeightSegments.size());

    EXPECT_EQ(layout.continuousCount,
              engine->variableControlPendingWeightSegments.size());
    EXPECT_EQ(layout.continuousCount,
              engine->variableControlInitialContinuousArms.size());
    EXPECT_EQ(layout.continuousCount,
              engine->variableControlRetargetPayloads.size());
    EXPECT_EQ(layout.continuousCount,
              engine->variableControlRetargetEvents.size());
    EXPECT_EQ(layout.continuousCount,
              engine->variableControlRetargetRuntimePrograms.size());
    EXPECT_EQ(layout.continuousCount,
              engine->variableControlRetargetSchedules.size());
    EXPECT_EQ(layout.continuousCount,
              engine->variableControlRetargetArms.size());
    EXPECT_EQ(layout.continuousCount,
              engine->variableControlRetargetMemberKinds.size());
    EXPECT_EQ(layout.continuousCount,
              engine->variableControlRetargetDesiredOrdinals.size());

    const std::uint64_t memberBytesPerSlot =
        sizeof(engine->playableScheduleVoices[0]) +
        sizeof(engine->playableScheduleLeaves[0]) +
        sizeof(engine->playableScheduleArms[0]) +
        sizeof(engine->playableScheduleRenderHandles[0]) +
        sizeof(engine->playableScheduleReleaseObservers[0]) +
        sizeof(engine->variableControlMemberCompletions[0]) +
        sizeof(engine->playableMemberReferences[0]) +
        sizeof(engine->playableScheduleStartFrames[0]) +
        sizeof(engine->variableControlDynamicMembers[0]);
    const std::uint64_t pairedBytesPerSlot =
        sizeof(engine->playableScheduleCompletions[0]) +
        sizeof(engine->playableSchedulePostSourceTailFrames[0]) +
        sizeof(engine->playableSuccessorPayloads[0]) +
        sizeof(engine->playableSuccessorEvents[0]);
    const std::uint64_t currentEventBytesPerSlot =
        sizeof(engine->playableSchedulePayloads[0]) +
        sizeof(engine->playableScheduleEvents[0]);
    const std::uint64_t initialProgramBytesPerSlot =
        sizeof(engine->variableControlRuntimePrograms[0]) +
        sizeof(engine->variableControlSchedules[0]);
    const std::uint64_t endpointBytesPerSlot =
        sizeof(engine->playableScheduleRuntimePrograms[0]) +
        sizeof(engine->playableSchedules[0]) +
        sizeof(engine->playableScheduleOccurrences[0]) +
        sizeof(engine->playableSuccessorVoices[0]) +
        sizeof(engine->playableSuccessorLeaves[0]) +
        sizeof(engine->playableSuccessorRuntimePrograms[0]) +
        sizeof(engine->playableSuccessorSchedules[0]) +
        sizeof(engine->playableSuccessorArms[0]) +
        sizeof(engine->playableSuccessorRenderHandles[0]) +
        sizeof(engine->playableSuccessorOccurrences[0]) +
        sizeof(engine->playableSuccessorCompletions[0]) +
        sizeof(engine->playableSuccessorStartFrames[0]) +
        sizeof(engine->playableSuccessorPostSourceTailFrames[0]);
    const std::uint64_t weightBytesPerSlot =
        sizeof(engine->variableControlWeightSegments[0]);
    const std::uint64_t continuousBytesPerSlot =
        sizeof(engine->variableControlPendingWeightSegments[0]) +
        sizeof(engine->variableControlInitialContinuousArms[0]) +
        sizeof(engine->variableControlRetargetPayloads[0]) +
        sizeof(engine->variableControlRetargetEvents[0]) +
        sizeof(engine->variableControlRetargetRuntimePrograms[0]) +
        sizeof(engine->variableControlRetargetSchedules[0]) +
        sizeof(engine->variableControlRetargetArms[0]) +
        sizeof(engine->variableControlRetargetMemberKinds[0]) +
        sizeof(engine->variableControlRetargetDesiredOrdinals[0]);

    const auto frontierEventBytes = sizeof(harken::FrontierEventBinding) +
        sizeof(harken::VariableWeightControlIdentity) + sizeof(harken::AuthoredStopOperationIdentity);
    const auto stopFrameBytes = sizeof(harken::AuthoredStopOperationIdentity::stopFrame);
    const auto immediateEntryBytes = sizeof(harken::ProgramView::immediateEntry);
    EXPECT_EQ(8u, stopFrameBytes);
    EXPECT_EQ(16u, immediateEntryBytes);
    // The frozen stop frame extends each event identity; each schedule owns
    // a full immediate-entry gate reference independently of its runtime view.
    EXPECT_EQ(320u + stopFrameBytes, frontierEventBytes);
    EXPECT_EQ(428u + frontierEventBytes, pairedBytesPerSlot);
    EXPECT_EQ(264u + frontierEventBytes, currentEventBytesPerSlot);
    EXPECT_EQ(192u + immediateEntryBytes, initialProgramBytesPerSlot);
    const auto playbackPathBytes = sizeof(harken::PlaybackPath);
    EXPECT_EQ(1'544u, playbackPathBytes);
    const auto voiceBindingStorageBytes =
        sizeof(harken::TimelineVoiceBindingInstall) *
        harken::kMaxVoiceBindingInstanceSlots;
    EXPECT_EQ(sizeof(harken::TimelineVoiceInstallDesc),
              sizeof(engine->playableScheduleVoices[0]));
    EXPECT_EQ(sizeof(harken::TimelineVoiceInstallDesc),
              sizeof(engine->playableSuccessorVoices[0]));
    EXPECT_EQ(voiceBindingStorageBytes,
              sizeof(engine->playableScheduleVoices[0].bindingStorage));
    EXPECT_EQ(voiceBindingStorageBytes,
              sizeof(engine->playableSuccessorVoices[0].bindingStorage));
    EXPECT_EQ(2'636u + 2u * sizeof(float) + sizeof(harken::PcmLoopRegion) +
                  playbackPathBytes + voiceBindingStorageBytes -
                  sizeof(void*) * harken::kMaxVoiceBindingInstanceSlots +
                  2u * immediateEntryBytes,
              endpointBytesPerSlot);
    EXPECT_EQ(48u, weightBytesPerSlot);
    EXPECT_EQ(541u + frontierEventBytes + immediateEntryBytes, continuousBytesPerSlot);

    const std::uint64_t totalBytes =
        layout.memberCount * memberBytesPerSlot +
        layout.pairedCount * pairedBytesPerSlot +
        layout.currentEventCount * currentEventBytesPerSlot +
        layout.initialProgramCount * initialProgramBytesPerSlot +
        layout.endpointCount * endpointBytesPerSlot +
        layout.weightCount * weightBytesPerSlot +
        layout.continuousCount * continuousBytesPerSlot;
    EXPECT_EQ(static_cast<std::uint64_t>(engine->config.maxPlayableInstances) * sizeof(harken::SourceTerminalRecord),
              static_cast<std::uint64_t>(engine->sourceTerminals.Capacity()) * sizeof(harken::SourceTerminalRecord));

    EXPECT_EQ(layout.memberCount * memberBytesPerSlot,
              StorageBytes(engine->playableScheduleVoices) +
                  StorageBytes(engine->playableScheduleLeaves) +
                  StorageBytes(engine->playableScheduleArms) +
                  StorageBytes(engine->playableScheduleRenderHandles) +
                  StorageBytes(engine->playableScheduleReleaseObservers) +
                  StorageBytes(engine->variableControlMemberCompletions) +
                  StorageBytes(engine->playableMemberReferences) +
                  StorageBytes(engine->playableScheduleStartFrames) +
                  StorageBytes(engine->variableControlDynamicMembers));
    EXPECT_EQ(layout.pairedCount * pairedBytesPerSlot,
              StorageBytes(engine->playableScheduleCompletions) +
                  StorageBytes(engine->playableSchedulePostSourceTailFrames) +
                  StorageBytes(engine->playableSuccessorPayloads) +
                  StorageBytes(engine->playableSuccessorEvents));
    EXPECT_EQ(layout.currentEventCount * currentEventBytesPerSlot,
              StorageBytes(engine->playableSchedulePayloads) +
                  StorageBytes(engine->playableScheduleEvents));
    EXPECT_EQ(layout.initialProgramCount * initialProgramBytesPerSlot,
              StorageBytes(engine->variableControlRuntimePrograms) +
                  StorageBytes(engine->variableControlSchedules));
    EXPECT_EQ(layout.endpointCount * endpointBytesPerSlot,
              StorageBytes(engine->playableScheduleRuntimePrograms) +
                  StorageBytes(engine->playableSchedules) +
                  StorageBytes(engine->playableScheduleOccurrences) +
                  StorageBytes(engine->playableSuccessorVoices) +
                  StorageBytes(engine->playableSuccessorLeaves) +
                  StorageBytes(engine->playableSuccessorRuntimePrograms) +
                  StorageBytes(engine->playableSuccessorSchedules) +
                  StorageBytes(engine->playableSuccessorArms) +
                  StorageBytes(engine->playableSuccessorRenderHandles) +
                  StorageBytes(engine->playableSuccessorOccurrences) +
                  StorageBytes(engine->playableSuccessorCompletions) +
                  StorageBytes(engine->playableSuccessorStartFrames) +
                  StorageBytes(
                      engine->playableSuccessorPostSourceTailFrames));
    EXPECT_EQ(layout.weightCount * weightBytesPerSlot,
              StorageBytes(engine->variableControlWeightSegments));
    EXPECT_EQ(layout.continuousCount * continuousBytesPerSlot,
              StorageBytes(engine->variableControlPendingWeightSegments) +
                  StorageBytes(
                      engine->variableControlInitialContinuousArms) +
                  StorageBytes(engine->variableControlRetargetPayloads) +
                  StorageBytes(engine->variableControlRetargetEvents) +
                  StorageBytes(
                      engine->variableControlRetargetRuntimePrograms) +
                  StorageBytes(engine->variableControlRetargetSchedules) +
                  StorageBytes(engine->variableControlRetargetArms) +
                  StorageBytes(engine->variableControlRetargetMemberKinds) +
                  StorageBytes(
                      engine->variableControlRetargetDesiredOrdinals));
    testing::Test::RecordProperty("seven_family_capacity_bytes", std::to_string(totalBytes));
    testing::Test::RecordProperty("member_reference_capacity_bytes",
        std::to_string(StorageBytes(engine->playableMemberReferences)));
    testing::Test::RecordProperty("facade_capacity_bytes", std::to_string(StorageBytes(engine->playableStarts)));
    testing::Test::RecordProperty("service_capacity_bytes", std::to_string(StorageBytes(engine->playableServices)));
    testing::Test::RecordProperty("partition_capacity_bytes", std::to_string(engine->playablePartitions.StorageBytes()));
    testing::Test::RecordProperty("published_empty_page_stamp_bytes",
        std::to_string(engine->timelinePool.PublishedEmptyStampStorageBytes()));
    testing::Test::RecordProperty("engine_inline_bytes", std::to_string(sizeof(harken::Engine)));
    testing::Test::RecordProperty("source_terminal_row_bytes", std::to_string(sizeof(harken::SourceTerminalRecord)));
    testing::Test::RecordProperty("timeline_arm_row_bytes", std::to_string(sizeof(harken::TimelineArmTable::Row)));
    testing::Test::RecordProperty("authored_control_metadata_row_bytes",
        std::to_string(sizeof(harken::TimelineArmTable::AuthoredControlMetadata)));
    testing::Test::RecordProperty("authored_control_metadata_capacity_bytes",
        std::to_string(static_cast<std::uint64_t>(engine->timelineArms.Capacity()) *
            sizeof(harken::TimelineArmTable::AuthoredControlMetadata)));
    testing::Test::RecordProperty("timeline_arm_capacity_bytes",
        std::to_string(static_cast<std::uint64_t>(engine->timelineArms.Capacity()) *
            sizeof(harken::TimelineArmTable::Row)));

    testing::Test::RecordProperty("member_completion_row_bytes", std::to_string(sizeof(harken::VariableControlMemberCompletion)));
    testing::Test::RecordProperty("member_completion_onset_offset",
        std::to_string(offsetof(harken::VariableControlMemberCompletion, sourceStartedBlockSequence)));
    testing::Test::RecordProperty("member_completion_capacity_bytes",
        std::to_string(StorageBytes(engine->variableControlMemberCompletions)));
    testing::Test::RecordProperty("member_completion_onset_capacity_bytes",
        std::to_string(engine->variableControlMemberCompletions.size() * sizeof(std::uint64_t)));
    testing::Test::RecordProperty("invocation_row_bytes", std::to_string(sizeof(harken::EnginePlayableStartRow)));
    testing::Test::RecordProperty("invocation_pending_stop_bytes", std::to_string(sizeof(harken::PlayableRetirementObserver)));
    testing::Test::RecordProperty("invocation_pending_stop_capacity_bytes",
        std::to_string(engine->playableStarts.size() * sizeof(harken::PlayableRetirementObserver)));
    testing::Test::RecordProperty("timeline_start_scratch_bytes", std::to_string(sizeof(harken::EngineTimelineStartScratch)));
    testing::Test::RecordProperty("timeline_start_transaction_count", std::to_string(engine->timelineStartScratch.transactions.size()));
    testing::Test::RecordProperty("timeline_start_two_coordinators_bytes", std::to_string(2u * sizeof(harken::PreparedTransactionBatch)));
    testing::Test::RecordProperty("frontier_completion_proof_row_bytes", std::to_string(sizeof(harken::TimelineArmTable::BoundAudioProof)));
    testing::Test::RecordProperty("frontier_completion_proof_capacity_bytes",
        std::to_string(engine->config.maxArmsPerGroup * sizeof(harken::TimelineArmTable::BoundAudioProof)));
    harken::FrontierTimelineFinalizer finalizer(engine->voices, engine->timelineArms);
    ASSERT_TRUE(finalizer.Attach(engine->decisions, engine->timelinePool));
    testing::Test::RecordProperty("frontier_finalizer_storage_bytes", std::to_string(finalizer.StorageBytes()));
    testing::Test::RecordProperty("frontier_event_binding_bytes", std::to_string(sizeof(harken::FrontierEventBinding)));
    constexpr auto frozenProofBytesPerVoice = harken::kTimelineMaxEventsPerVoiceQuantum *
        sizeof(harken::FrozenFrontierEventProof);
    testing::Test::RecordProperty("frozen_frontier_event_proof_bytes",
        std::to_string(sizeof(harken::FrozenFrontierEventProof)));
    testing::Test::RecordProperty("frozen_frontier_event_proof_bytes_per_voice",
        std::to_string(frozenProofBytesPerVoice));
    testing::Test::RecordProperty("frozen_frontier_event_proof_capacity_bytes",
        std::to_string(engine->voices.Capacity() * frozenProofBytesPerVoice));
    constexpr auto queueFutureRollbackBytes =
        sizeof(harken::EngineActionPlayScratch::queueFutureMemberSlots) +
        sizeof(harken::EngineActionPlayScratch::queueFuturePriorMemberReferences);
    testing::Test::RecordProperty("queue_future_rollback_bytes_per_scratch",
        std::to_string(queueFutureRollbackBytes));
    testing::Test::RecordProperty("queue_future_rollback_capacity_bytes",
        std::to_string(engine->actionPlayScratch.size() * queueFutureRollbackBytes));
    testing::Test::RecordProperty("variable_weight_owner_bytes", std::to_string(sizeof(harken::VariableWeightControlIdentity)));
    testing::Test::RecordProperty("authored_stop_identity_bytes", std::to_string(sizeof(harken::AuthoredStopOperationIdentity)));
    testing::Test::RecordProperty("frontier_event_family_capacity_increase_bytes",
        std::to_string((layout.pairedCount + layout.currentEventCount + layout.continuousCount) * frontierEventBytes));
}

TEST(CapacityContract, CompositeActionStorageIncreaseMatchesBothBlockProfiles) {
    VnConfig config = MakeNullConfig();
    harken::EngineConfig resolved;
    ASSERT_EQ(VN_OK, harken::ResolveConfig(&config, &resolved));
    const harken::PlayableStorageLayout layout(resolved);

    constexpr std::uint64_t kPreviousAllFamilyBytesPerSlot =
        1'577u + 396u + 240u + 176u + 2'100u + 48u + 501u;
    constexpr std::uint64_t kPreviousSharedStride =
        2u * 16u + 2u * 32u;
    const std::uint64_t previousPlayableBytes =
        resolved.maxPlayableInstances * kPreviousSharedStride *
        kPreviousAllFamilyBytesPerSlot;
    const std::uint64_t currentPlayableBytes =
        layout.memberCount * 1'753u + layout.pairedCount * 396u +
        layout.currentEventCount * 240u +
        layout.initialProgramCount * 176u +
        layout.endpointCount * 2'276u + layout.weightCount * 48u +
        layout.continuousCount * 501u;
    ASSERT_EQ(123'813'888u, previousPlayableBytes);
    ASSERT_EQ(154'296'320u, currentPlayableBytes);
    const std::uint64_t playableIncrease =
        currentPlayableBytes - previousPlayableBytes;
    ASSERT_EQ(30'482'432u, playableIncrease);

    constexpr std::uint64_t kPreviousKnownLeaves = 16u;
    constexpr std::uint64_t kPreviousArmsPerGroup = 64u;
    constexpr std::uint64_t kPreviousFrameBatches = 64u;
    constexpr std::uint64_t kArmGroupMemberRowBytes = 20u;
    ASSERT_EQ(216u, sizeof(harken::KnownScheduleLeafV1));
    ASSERT_EQ(16u, sizeof(harken::TimelineArmToken));

    const std::uint64_t completionLeafIncrease =
        resolved.maxCompositeCompletionOwners *
        (resolved.maxKnownScheduleLeaves - kPreviousKnownLeaves) *
        sizeof(harken::KnownScheduleLeafV1);
    const std::uint64_t armGroupIncrease =
        resolved.maxArmGroups *
        (resolved.maxArmsPerGroup - kPreviousArmsPerGroup) *
        kArmGroupMemberRowBytes;
    ASSERT_EQ(2'654'208u, completionLeafIncrease);
    ASSERT_EQ(327'680u, armGroupIncrease);

    struct Profile {
        std::uint32_t blockFrames;
        std::uint64_t expectedPageMembers;
        std::uint64_t expectedFrameBatches;
        std::uint64_t expectedTotal;
        double expectedMiB;
    };
    constexpr std::array<Profile, 2> profiles = {{
        {256u, 4'194'304u, 32'768u, 37'691'392u, 35.9453125},
        {64u, 16'777'216u, 131'072u, 50'372'608u, 48.0390625},
    }};

    for (const Profile& profile : profiles) {
        VnConfig profileConfig = MakeNullConfig();
        profileConfig.blockFrames = profile.blockFrames;
        harken::EngineConfig profileResolved;
        ASSERT_EQ(VN_OK,
                  harken::ResolveConfig(&profileConfig, &profileResolved));
        const std::uint64_t pageCount =
            profileResolved.scheduleHorizonFrames /
            profileResolved.blockFrames;
        const std::uint64_t pageMemberIncrease =
            pageCount * profileResolved.maxPlan03GroupHeadersPerPage *
            (profileResolved.maxArmsPerGroup - kPreviousArmsPerGroup) *
            sizeof(harken::TimelineArmToken);
        const std::uint64_t frameBatchIncrease =
            pageCount *
            (profileResolved.maxFrameBatchesPerQuantum -
             kPreviousFrameBatches) *
            sizeof(std::uint64_t);
        EXPECT_EQ(profile.expectedPageMembers, pageMemberIncrease)
            << "blockFrames=" << profile.blockFrames;
        EXPECT_EQ(profile.expectedFrameBatches, frameBatchIncrease)
            << "blockFrames=" << profile.blockFrames;
        const std::uint64_t totalIncrease =
            playableIncrease + completionLeafIncrease + armGroupIncrease +
            pageMemberIncrease + frameBatchIncrease;
        EXPECT_EQ(profile.expectedTotal, totalIncrease)
            << "blockFrames=" << profile.blockFrames;
        EXPECT_DOUBLE_EQ(
            profile.expectedMiB,
            static_cast<double>(totalIncrease) / (1024.0 * 1024.0))
            << "blockFrames=" << profile.blockFrames;
    }
}

TEST(CapacityContract, CurrentActionCapacityTombstonesMustBeZero) {
    VnConfig config = MakeNullConfig();
    harken::EngineConfig resolved;

    config.pendingActionCapacity = 1;
    EXPECT_EQ(VN_INVALID_ARG, harken::ResolveConfig(&config, &resolved));

    config.pendingActionCapacity = 0;
    config.reserved10 = 1;
    EXPECT_EQ(VN_INVALID_ARG, harken::ResolveConfig(&config, &resolved));

    config.reserved10 = 0;
    config.reserved11 = 1;
    EXPECT_EQ(VN_INVALID_ARG, harken::ResolveConfig(&config, &resolved));

    config.reserved11 = 0;
    config.reserved12 = 1;
    EXPECT_EQ(VN_INVALID_ARG, harken::ResolveConfig(&config, &resolved));
}

// The high-water half of the diagnostic transports' contract. A drop counter
// says loss already happened, which is the one thing a caller cannot size a
// depth from; the high-water says how much headroom remained while nothing was
// lost. Both are consumer-side, so neither costs the audio thread anything.
//
// Measured through the rings directly rather than through an engine, because
// what is under test is the accounting rule -- saturate at capacity, never
// exceed it, zero until something is actually queued -- and a live engine would
// make the observed occupancy depend on how much it happened to log.
TEST(CapacityContract, LogRingHighWaterTracksOccupancyAndSaturatesAtCapacity) {
    harken::LogRing ring;
    EXPECT_EQ(0u, ring.OccupancyHighWater())
        << "a ring nothing was drained from must report no occupancy";

    harken::LogRecord record{};
    record.level = VN_LOG_INFO;
    record.code = 1;
    for (std::uint32_t i = 0; i < 3; ++i) {
        ring.Push(record);
    }
    harken::LogRecord out{};
    while (ring.TryPop(out)) {
    }
    EXPECT_EQ(3u, ring.OccupancyHighWater());

    // Overflow: the raw producer/consumer lag passes capacity, but occupancy is
    // what the ring can hold, so it saturates instead of reporting a number no
    // ring could ever contain.
    for (std::uint32_t i = 0; i < harken::LogRing::kCapacity * 2u; ++i) {
        ring.Push(record);
    }
    while (ring.TryPop(out)) {
    }
    EXPECT_EQ(harken::LogRing::kCapacity, ring.OccupancyHighWater());
    EXPECT_GT(ring.DroppedCount(), 0u)
        << "the overflow that saturated the high-water must also have dropped";
}

// The queue's capacity is configurable, so its high-water has to saturate at the
// resolved capacity rather than at any fixed number -- and the resolved capacity
// is the requested depth rounded up to a power of two, not the request.
TEST(CapacityContract, NotificationQueueHighWaterSaturatesAtResolvedCapacity) {
    harken::NotificationQueue queue(4);
    ASSERT_EQ(4u, queue.Capacity());
    EXPECT_EQ(0u, queue.OccupancyHighWater());

    VnNotification notification{};
    notification.type = VN_NOTIFY_VOICE_ENDED;
    queue.Push(notification);
    queue.Push(notification);
    VnNotification out{};
    while (queue.TryPop(out)) {
    }
    EXPECT_EQ(2u, queue.OccupancyHighWater());

    for (std::uint32_t i = 0; i < 16; ++i) {
        queue.Push(notification);
    }
    while (queue.TryPop(out)) {
    }
    EXPECT_EQ(queue.Capacity(), queue.OccupancyHighWater());
}

// The published surface, which is what a caller can actually act on. A session
// that logs nothing must publish zero: without this, an implementation that
// reported the ring's capacity from the first block would satisfy every
// monotonicity assertion above and still be useless for sizing.
TEST(CapacityContract, DiagnosticHighWatersArePublishedAndStartAtZero) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(2, 256, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());

    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    EXPECT_EQ(0u, snapshot.logRingHighWater);
    EXPECT_EQ(0u, snapshot.notificationQueueHighWater);
}

// A short real voice drives an audio-to-logic notification through the engine,
// so the high-water survives the drain and proves the published fields are
// sourced from live ring state. The prior snapshot vintage must still leave
// the appended diagnostic pair untouched in its poisoned tail.
TEST(CapacityContract,
     DiagnosticHighWaterPropagatesAndOlderSnapshotTailStaysUntouched) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(2, 256, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());

    const std::vector<float> shortSound = harkentest::MakeDC(64, 0.5f);
    const VnPCMDesc desc = harkentest::MonoDesc(shortSound, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    const VnPlayDesc play = harkentest::MakePlayDesc(harkentest::ObjectId(1));
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));
    session.Engine()->nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());

    VnSnapshot current{};
    current.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&current));
    EXPECT_GT(current.notificationQueueHighWater, 0u);

    constexpr std::uint32_t kPreviousSnapshotSize = 4288;
    constexpr std::uint8_t kPoison = 0xA5;
    VnSnapshot older;
    std::memset(&older, kPoison, sizeof(older));
    older.structSize = kPreviousSnapshotSize;
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&older));
    EXPECT_EQ(0xA5A5A5A5u, older.logRingHighWater);
    EXPECT_EQ(0xA5A5A5A5u, older.notificationQueueHighWater);
}

// --- per-quantum producer budget ---
//
// The budget bounds what one block may produce, which is a different quantity
// from what the ring can hold at a drain. These tests are the only place the two
// are kept apart: an implementation that reported occupancy under the budget's
// name would satisfy every other assertion in this file.

TEST(CapacityContract, LogRingBudgetTakesCapacityAndRefusesTheNextInSameQuantum) {
    harken::LogRing ring;
    ring.EnableQuantumBudget();
    ring.BeginQuantum();

    harken::LogRecord record{};
    record.level = VN_LOG_INFO;
    record.code = 1;
    for (std::uint32_t i = 0; i < harken::LogRing::kCapacity; ++i) {
        ASSERT_TRUE(ring.Push(record)) << "record " << i << " of the budget";
    }
    EXPECT_EQ(0u, ring.QuantumBudgetDropCount())
        << "the budget is capacity, so capacity records must all be taken";

    EXPECT_FALSE(ring.Push(record)) << "the record past capacity must be refused";
    EXPECT_EQ(1u, ring.QuantumBudgetDropCount());
    EXPECT_EQ(harken::LogRing::kCapacity, ring.QuantumProducedHighWater());

    // A new quantum restores the allowance; the drop total does not reset,
    // because it is read after the fact to answer "did this ever happen".
    ring.BeginQuantum();
    EXPECT_TRUE(ring.Push(record)) << "the next quantum must start with a full "
                                      "allowance, not a spent one";
    EXPECT_EQ(1u, ring.QuantumBudgetDropCount())
        << "budget drops accumulate across quanta";
    EXPECT_EQ(harken::LogRing::kCapacity, ring.QuantumProducedHighWater())
        << "the produced high-water is monotone";
}

// The logic-side ring is pushed from API calls that belong to no block. If the
// budget were on by default, those pushes would be refused for being outside a
// quantum that does not exist -- so the default has to be off, and this is what
// says so.
TEST(CapacityContract, ARingWithoutTheBudgetKeepsItsOldSemantics) {
    harken::LogRing ring;
    harken::LogRecord record{};
    record.level = VN_LOG_INFO;
    record.code = 7;

    for (std::uint32_t i = 0; i < harken::LogRing::kCapacity * 2u; ++i) {
        EXPECT_TRUE(ring.Push(record)) << "push " << i
                                       << " on an unbudgeted ring must be taken";
    }
    EXPECT_EQ(0u, ring.QuantumBudgetDropCount());
    EXPECT_EQ(0u, ring.QuantumProducedHighWater());

    // Overwrite still happens: capacity is unchanged, only the budget is absent.
    harken::LogRecord out{};
    while (ring.TryPop(out)) {
    }
    EXPECT_GT(ring.DroppedCount(), 0u)
        << "an unbudgeted ring must still overwrite when it laps the reader";
}

// The queue's budget is its resolved capacity -- the requested depth rounded up
// to a power of two -- not the request. Asking for 3 and getting a budget of 3
// would refuse a record the queue could hold.
TEST(CapacityContract, NotificationBudgetUsesResolvedCapacityNotTheRequest) {
    harken::NotificationQueue queue(3);
    ASSERT_EQ(4u, queue.Capacity()) << "3 must resolve up to 4";
    queue.EnableQuantumBudget();
    queue.BeginQuantum();

    VnNotification notification{};
    notification.type = VN_NOTIFY_VOICE_ENDED;
    for (std::uint32_t i = 0; i < queue.Capacity(); ++i) {
        ASSERT_TRUE(queue.Push(notification)) << "notification " << i;
    }
    EXPECT_FALSE(queue.Push(notification));
    EXPECT_EQ(1u, queue.QuantumBudgetDropCount());
    EXPECT_EQ(queue.Capacity(), queue.QuantumProducedHighWater());

    queue.BeginQuantum();
    EXPECT_TRUE(queue.Push(notification));
    EXPECT_EQ(1u, queue.QuantumBudgetDropCount());
}

// The two drop counters answer different questions and must not move together.
// Overwrite drops come from draining too late; budget drops come from producing
// too much in one block. A single counter could not tell them apart.
TEST(CapacityContract, OverwriteDropsAndBudgetDropsAreCountedApart) {
    harken::NotificationQueue queue(4);
    queue.EnableQuantumBudget();

    VnNotification notification{};
    notification.type = VN_NOTIFY_VOICE_ENDED;

    // Four quanta of one record each: never over budget, but nothing is drained
    // between them, so the ring laps and the consumer sees overwrite drops.
    for (std::uint32_t block = 0; block < 8; ++block) {
        queue.BeginQuantum();
        ASSERT_TRUE(queue.Push(notification)) << "block " << block;
    }
    EXPECT_EQ(0u, queue.QuantumBudgetDropCount())
        << "one record per quantum can never exceed a budget of four";
    VnNotification out{};
    while (queue.TryPop(out)) {
    }
    EXPECT_GT(queue.DroppedCount(), 0u)
        << "eight records through a four-slot ring must have overwritten";
}

// Through a real engine: two consecutive blocks prove the allowance is per block
// and not a session total, and prove the counters reach the public surface.
TEST(CapacityContract, QuantumBudgetCountersArePublishedAndStartAtZero) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(2, 256, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    engine->nullBackend.Pump(1);
    VnSnapshot first{};
    first.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&first));
    EXPECT_EQ(0u, first.logQuantumBudgetDropCount)
        << "an ordinary block must not exceed either budget";
    EXPECT_EQ(0u, first.notificationQuantumBudgetDropCount);

    // A second block: if the reset were missing, production would accumulate
    // across blocks and the high-water would keep climbing on identical work.
    engine->nullBackend.Pump(1);
    VnSnapshot second{};
    second.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&second));
    EXPECT_EQ(0u, second.logQuantumBudgetDropCount);
    EXPECT_EQ(0u, second.notificationQuantumBudgetDropCount);
    EXPECT_EQ(first.logQuantumProducedHighWater,
              second.logQuantumProducedHighWater)
        << "two identical blocks must produce the same per-quantum peak; a "
           "climbing peak means the quantum is never reset";
}

// A tight arena makes the real mixer emit one log and one degradation
// notification on every block. Each block must open an independent producer
// quantum before those records are generated; otherwise the second block
// would report a produced high-water of two.
TEST(CapacityContract, QuantumBudgetResetsAcrossIndependentRenderBlocks) {
    VnConfig config = harkentest::MakeNullConfig(2, 256, 48000);
    config.arenaBytes = 64;
    harkentest::EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    for (std::uint32_t block = 0; block < 2; ++block) {
        engine->nullBackend.Pump(1);

        VnSnapshot snapshot{};
        snapshot.structSize = sizeof(VnSnapshot);
        ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
        EXPECT_EQ(0u, snapshot.logQuantumBudgetDropCount)
            << "log budget drop in block " << block;
        EXPECT_EQ(0u, snapshot.notificationQuantumBudgetDropCount)
            << "notification budget drop in block " << block;
        EXPECT_EQ(1u, snapshot.logQuantumProducedHighWater)
            << "log production did not reset before block " << block;
        EXPECT_EQ(1u, snapshot.notificationQuantumProducedHighWater)
            << "notification production did not reset before block " << block;
        EXPECT_EQ(static_cast<std::uint64_t>(block + 1u),
                  snapshot.arenaOverflowCount)
            << "tight-arena degradation did not occur exactly once in block "
            << block;
    }
}

// The four producer-budget fields are audio-owned facts. Publish a small
// first-block value, then change the live transport counters without rendering
// another block: capture must retain the first payload rather than rereading
// the mutable rings from the logic thread.
TEST(CapacityContract,
     QuantumBudgetSnapshotUsesThePublishedPayloadNotLiveTransportGetters) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(2, 256, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    harken::LogRecord log{};
    log.level = VN_LOG_INFO;
    log.code = 1;
    engine->logRing.BeginQuantum();
    ASSERT_TRUE(engine->logRing.Push(log));

    VnNotification notification{};
    notification.type = VN_NOTIFY_VOICE_ENDED;
    engine->notifications.BeginQuantum();
    ASSERT_TRUE(engine->notifications.Push(notification));

    engine->nullBackend.Pump(1);

    VnSnapshot published{};
    published.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&published));
    EXPECT_EQ(0u, published.logQuantumBudgetDropCount);
    EXPECT_EQ(0u, published.notificationQuantumBudgetDropCount);
    EXPECT_EQ(1u, published.logQuantumProducedHighWater);
    EXPECT_EQ(1u, published.notificationQuantumProducedHighWater);

    // The null backend is caller-thread pumped, so this intentional transport
    // mutation has no callback racing it. Do not publish a block that could
    // replace the captured payload.
    engine->logRing.BeginQuantum();
    for (std::uint32_t i = 0; i < harken::LogRing::kCapacity; ++i) {
        ASSERT_TRUE(engine->logRing.Push(log));
    }
    ASSERT_FALSE(engine->logRing.Push(log));

    engine->notifications.BeginQuantum();
    for (std::uint32_t i = 0; i < engine->notifications.Capacity(); ++i) {
        ASSERT_TRUE(engine->notifications.Push(notification));
    }
    ASSERT_FALSE(engine->notifications.Push(notification));

    EXPECT_GT(engine->logRing.QuantumBudgetDropCount(),
              published.logQuantumBudgetDropCount);
    EXPECT_GT(engine->notifications.QuantumBudgetDropCount(),
              published.notificationQuantumBudgetDropCount);
    EXPECT_GT(engine->logRing.QuantumProducedHighWater(),
              published.logQuantumProducedHighWater);
    EXPECT_GT(engine->notifications.QuantumProducedHighWater(),
              published.notificationQuantumProducedHighWater);

    VnSnapshot recaptured{};
    recaptured.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&recaptured));
    EXPECT_EQ(published.logQuantumBudgetDropCount,
              recaptured.logQuantumBudgetDropCount);
    EXPECT_EQ(published.notificationQuantumBudgetDropCount,
              recaptured.notificationQuantumBudgetDropCount);
    EXPECT_EQ(published.logQuantumProducedHighWater,
              recaptured.logQuantumProducedHighWater);
    EXPECT_EQ(published.notificationQuantumProducedHighWater,
              recaptured.notificationQuantumProducedHighWater);
}

// Budget drops are cumulative audio-owned facts, so publish a nonzero value
// before the first block and then advance the live counters without another
// publication. The public snapshot must retain the published nonzero values.
TEST(CapacityContract,
     QuantumBudgetDropCountersUsePublishedPayloadWhenNonzero) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(2, 256, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();

    harken::LogRecord log{};
    log.level = VN_LOG_INFO;
    log.code = 1;
    engine->logRing.BeginQuantum();
    for (std::uint32_t i = 0; i < harken::LogRing::kCapacity; ++i) {
        ASSERT_TRUE(engine->logRing.Push(log));
    }
    ASSERT_FALSE(engine->logRing.Push(log));

    VnNotification notification{};
    notification.type = VN_NOTIFY_VOICE_ENDED;
    engine->notifications.BeginQuantum();
    for (std::uint32_t i = 0; i < engine->notifications.Capacity(); ++i) {
        ASSERT_TRUE(engine->notifications.Push(notification));
    }
    ASSERT_FALSE(engine->notifications.Push(notification));

    engine->nullBackend.Pump(1);

    VnSnapshot published{};
    published.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&published));
    EXPECT_EQ(1u, published.logQuantumBudgetDropCount);
    EXPECT_EQ(1u, published.notificationQuantumBudgetDropCount);
    EXPECT_EQ(harken::LogRing::kCapacity,
              published.logQuantumProducedHighWater);
    EXPECT_EQ(engine->notifications.Capacity(),
              published.notificationQuantumProducedHighWater);

    // The null backend is caller-thread pumped, so this intentional transport
    // mutation has no callback racing it. Do not publish another block.
    engine->logRing.BeginQuantum();
    for (std::uint32_t i = 0; i < harken::LogRing::kCapacity; ++i) {
        ASSERT_TRUE(engine->logRing.Push(log));
    }
    ASSERT_FALSE(engine->logRing.Push(log));
    engine->notifications.BeginQuantum();
    for (std::uint32_t i = 0; i < engine->notifications.Capacity(); ++i) {
        ASSERT_TRUE(engine->notifications.Push(notification));
    }
    ASSERT_FALSE(engine->notifications.Push(notification));

    EXPECT_GT(engine->logRing.QuantumBudgetDropCount(),
              published.logQuantumBudgetDropCount);
    EXPECT_GT(engine->notifications.QuantumBudgetDropCount(),
              published.notificationQuantumBudgetDropCount);

    VnSnapshot recaptured{};
    recaptured.structSize = sizeof(VnSnapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&recaptured));
    EXPECT_EQ(published.logQuantumBudgetDropCount,
              recaptured.logQuantumBudgetDropCount);
    EXPECT_EQ(published.notificationQuantumBudgetDropCount,
              recaptured.notificationQuantumBudgetDropCount);
    EXPECT_EQ(published.logQuantumProducedHighWater,
              recaptured.logQuantumProducedHighWater);
    EXPECT_EQ(published.notificationQuantumProducedHighWater,
              recaptured.notificationQuantumProducedHighWater);
}

// An older caller's struct stops before these fields, so the engine must not
// write them. The tail is poisoned first: anything the engine touches past the
// declared size shows up as a changed byte.
TEST(CapacityContract, QuantumBudgetFieldsAreNotWrittenForAnOlderVintage) {
    harkentest::EngineSession session(harkentest::MakeNullConfig(2, 256, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    session.Engine()->nullBackend.Pump(1);

    constexpr std::uint8_t kPoison = 0xA5;
    const std::uint32_t olderSize = static_cast<std::uint32_t>(
        offsetof(VnSnapshot, logQuantumBudgetDropCount));
    std::vector<std::uint8_t> storage(sizeof(VnSnapshot), kPoison);
    auto* snapshot = reinterpret_cast<VnSnapshot*>(storage.data());
    snapshot->structSize = olderSize;
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(snapshot));

    for (std::size_t i = olderSize; i < sizeof(VnSnapshot); ++i) {
        ASSERT_EQ(kPoison, storage[i])
            << "byte " << i << " past the caller's declared size was written";
    }
}

} // namespace
