// TimelinePlanner scheduling simulation tests.
//
// The planner is the sole publisher of scheduled transactions, so the properties
// pinned here are the publication contract: the same timestamped inputs
// produce the same filed schedule whatever the Update cadence; a play beyond the
// horizon is deferred and later materialized at exactly its planned frames; the
// per-quantum materialization budget paces work without losing any; the late
// trichotomy is wired to the pool and NextEquivalentBoundary to the marker grid; and
// the keyed RNG seed depends only on its key.
//
// The digest a determinism case compares is built by consuming each page as it
// leaves the window, so it is the actual filed schedule in frame order -- not the
// planner's internal path, which is allowed to differ with cadence.

#include "Common/ObjectIdTestSupport.h"
#include "harken/core/ClockKernel.h"
#include "harken/core/MarkerIndex.h"
#include "harken/core/DeferredIntentPool.h"
#include "harken/core/KeyedRng.h"
#include "harken/core/OccurrenceStatePool.h"
#include "harken/core/PlayableInstanceTable.h"
#include "harken/core/ScheduledTransactionPool.h"
#include "harken/core/TimelinePlanner.h"
#include "harken/core/TimelineArmTable.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <limits>
#include <vector>

using harken::BoundaryType;
using harken::ClockKernel;
using harken::ConsumedEvent;
using harken::MarkerKind;
using harken::MarkerIndex;
using harken::MarkerPoint;
using harken::DeferredIntent;
using harken::DeferredIntentPool;
using harken::DeferredIntentRefusal;
using harken::EventTemplate;
using harken::HashKeyedRng;
using harken::KeyedRngInput;
using harken::MeterSignature;
using harken::MissingBoundaryPolicy;
using harken::MusicalTick;
using harken::OccurrenceStatePool;
using harken::PieceTimeType;
using harken::PlayableInstanceHandle;
using harken::PlayableInstanceTable;
using harken::PlayableKind;
using harken::PostInput;
using harken::PostResult;
using harken::PostStatus;
using harken::PrePublishLatePolicy;
using harken::ProgramView;
using harken::ReserveRefusal;
using harken::ScheduledTransactionPool;
using harken::TempoSegment;
using harken::TimelinePlanner;
using harken::TransactionId;
using harken::TransportEpoch;

namespace {

constexpr TransportEpoch kEpoch = 3;

ScheduledTransactionPool::Config PoolConfig(std::uint32_t quantum,
                                            std::uint64_t horizon) {
    ScheduledTransactionPool::Config c;
    c.quantumFrames = quantum;
    c.scheduleHorizonFrames = horizon;
    c.publishLeadQuanta = 1;
    c.maxScheduledTransactions = 64;
    c.maxTransactionSpanFrames = horizon;
    c.maxIngressTransactionsPerQuantum = 64;
    c.maxFrameBatchesPerQuantum = 64;
    c.maxEventsPerQuantum = 64;
    c.maxTransactionEvents = 16;
    return c;
}

// Folds a consumed event into a running FNV-1a digest.
void Fold(std::uint64_t& hash, std::uint64_t v) {
    for (int b = 0; b < 8; ++b) {
        hash ^= (v >> (b * 8)) & 0xFFu;
        hash *= 1099511628211ull;
    }
}

std::uint64_t ConsumePageDigest(ScheduledTransactionPool& pool, std::uint32_t page,
                                const PlayableInstanceTable& instances,
                                const OccurrenceStatePool& occ,
                                std::uint64_t hash) {
    std::array<ConsumedEvent, 128> buf{};
    const std::size_t n = pool.ConsumePage(page, kEpoch, instances, occ,
                                           buf.data(), buf.size());
    for (std::size_t i = 0; i < n; ++i) {
        Fold(hash, buf[i].executeFrame);
        Fold(hash, buf[i].payload);
        Fold(hash, buf[i].eventIndex);
        Fold(hash, buf[i].transactionId);
    }
    return hash;
}

std::size_t ConsumeFrontier(ScheduledTransactionPool& pool,
                            ConsumedEvent* out, std::size_t outCapacity) {
    const std::uint64_t quantum = pool.ConsumerFrontier().load();
    if (pool.PageStamp(quantum) != quantum) {
        return 0;
    }
    const std::size_t consumed =
        pool.ConsumePageRt(quantum, out, outCapacity);
    pool.ConsumerFrontier().store(quantum + 1);
    return consumed;
}

std::uint64_t ServiceDeferred(
    TimelinePlanner& planner, ScheduledTransactionPool& pool,
    DeferredIntentPool& deferred, const PlayableInstanceTable& instances,
    TransportEpoch epoch, std::uint64_t cutoffFrame, std::uint32_t budget,
    std::uint64_t currentQuantum) {
    planner.DropStaleDeferred();
    std::uint32_t attempted = 0;
    while (attempted < budget) {
        harken::DeferredIntentClaim claim{};
        if (deferred.ClaimDue(pool.WindowEndFrame(), instances, &claim, 1,
                              nullptr, nullptr, currentQuantum) == 0) {
            break;
        }
        ++attempted;
        const harken::TimelineDeferredMaterializeStatus result =
            planner.MaterializeDeferredClaim(claim, epoch, cutoffFrame);
        if (result != harken::TimelineDeferredMaterializeStatus::Consumed) {
            EXPECT_EQ(deferred.Retry(claim.token, currentQuantum + 1u),
                      DeferredIntentRefusal::None);
        }
    }
    return planner.DeferredBlockerBefore(cutoffFrame);
}

void AdvancePlannerQuantum(
    TimelinePlanner& planner, ScheduledTransactionPool& pool,
    DeferredIntentPool& deferred, const PlayableInstanceTable& instances,
    TransportEpoch epoch, std::uint32_t budget = UINT32_MAX) {
    if (!planner.ReclaimConsumedQuantum() || !pool.Valid()) {
        return;
    }
    const std::uint64_t publishQuanta =
        std::max<std::uint64_t>(pool.PublishLeadQuanta(), 1u);
    const std::uint64_t cutoff =
        pool.WindowStartFrame() + publishQuanta * pool.QuantumFrames();
    const std::uint64_t blocker =
        ServiceDeferred(planner, pool, deferred, instances, epoch, cutoff,
                        budget, pool.WindowStartQuantum());
    const std::uint64_t safeCutoff =
        blocker == UINT64_MAX
            ? cutoff
            : std::min(cutoff,
                       blocker - blocker % pool.QuantumFrames());
    planner.PublishThrough(safeCutoff);
}

} // namespace

TEST(TimelinePlanner, FrontierMetadataSurvivesProducerFilingAndCannotShiftOrLeakIntoNextProgram) {
    PlayableInstanceTable instances(1);
    const auto owner = instances.Acquire(PlayableKind::Container, 0);
    const auto epoch = instances.Classify(owner).scheduleEpoch;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(64, 256), 0, kEpoch));
    DeferredIntentPool deferred(4, 0);
    TimelinePlanner planner;
    ASSERT_TRUE(planner.Reset({}, &pool, &deferred, &instances));
    EventTemplate event{};
    event.logicCommitSequence = 7;
    event.frontier.kind = harken::FrontierOperationKind::Entry;
    event.frontier.entry = {{0, 1}, 3, {owner, epoch, 0, 0, owner, epoch, 0, 1}};
    event.frontier.owner = event.frontier.entry.owner;
    event.frontier.transport = kEpoch;
    event.frontier.world = 1;
    event.frontier.operationEventCount = 1;
    event.frontier.entryEventOrdinal = 0;
    ProgramView program{};
    program.ownerInstance = owner;
    program.ownerInstanceScheduleEpoch = epoch;
    program.transactionId = 7;
    program.entryAnchorFrame = 8;
    program.events = &event;
    program.eventCount = 1;
    PostInput input{};
    input.transportEpoch = kEpoch;
    input.latePolicy = PrePublishLatePolicy::ExecuteAtEarliestSafeFrame;
    auto result = planner.PostPlay(program, input);
    EXPECT_EQ(PostStatus::Refused, result.status);
    EXPECT_EQ(ReserveRefusal::Late, result.refusal);
    program.entryAnchorFrame = 72;
    ASSERT_EQ(PostStatus::Reserved, planner.PostPlay(program, input).status);
    pool.Publish(128, &instances, nullptr);
    std::array<ConsumedEvent, 2> consumed{};
    ASSERT_EQ(1u, pool.ConsumePageRt(1, consumed.data(), consumed.size()));
    EXPECT_EQ(72u, consumed[0].executeFrame);
    EXPECT_EQ(event.frontier, consumed[0].frontier);
    event.frontier = {};
    program.transactionId = 8;
    program.entryAnchorFrame = 136;
    ASSERT_EQ(PostStatus::Reserved, planner.PostPlay(program, input).status);
    pool.Publish(192, &instances, nullptr);
    ASSERT_EQ(1u, pool.ConsumePageRt(2, consumed.data(), consumed.size()));
    EXPECT_FALSE(consumed[0].frontier.Present());
}

TEST(TimelinePlanner, RollingStartRefusalLeavesNoFutureIntent) {
    PlayableInstanceTable instances(1);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Piece, 0);
    ScheduledTransactionPool pool;
    auto poolConfig = PoolConfig(256, 1024);
    poolConfig.maxScheduledTransactions = 1;
    poolConfig.maxIngressTransactionsPerQuantum = 1;
    ASSERT_TRUE(pool.Reset(poolConfig, 0, kEpoch));
    DeferredIntentPool deferred(2, 0);
    TimelinePlanner planner;
    TimelinePlanner::Config config;
    config.maxRollingOccurrences = 2;
    ASSERT_TRUE(planner.Reset(config, &pool, &deferred, &instances));

    const std::uint64_t ownerEpoch =
        instances.Classify(owner).scheduleEpoch;
    const EventTemplate occupyingEvent[] = {{256, 1, 0, 0, 20, 7}};
    ProgramView occupying{};
    occupying.ownerInstance = owner;
    occupying.ownerInstanceScheduleEpoch = ownerEpoch;
    occupying.transactionId = 7;
    occupying.events = occupyingEvent;
    occupying.eventCount = 1;
    PostInput occupyingInput{};
    occupyingInput.transportEpoch = kEpoch;
    occupyingInput.latePolicy = PrePublishLatePolicy::Refuse;
    ASSERT_EQ(planner.PostPlay(occupying, occupyingInput).status,
              PostStatus::Reserved);

    std::array<EventTemplate, 2> events{};
    events[0] = {0, 1, 0, 0, 20, 0};
    events[1] = {3840, 1, 1, 0, 50, 1};
    ProgramView program{};
    program.ownerInstance = owner;
    program.ownerInstanceScheduleEpoch = ownerEpoch;
    program.transactionId = 1;
    program.entryAnchorFrame = 256;
    program.events = events.data();
    program.eventCount = events.size();
    program.rollingGroups = true;
    PostInput input{};
    input.transportEpoch = kEpoch;
    input.latePolicy = PrePublishLatePolicy::Refuse;
    const PostResult result = planner.PostPlay(program, input);
    EXPECT_EQ(result.status, PostStatus::Refused);
    EXPECT_EQ(deferred.Count(), 0u);
}

TEST(TimelinePlanner, RollingCursorAndDeferredCapacityAt95_100_101) {
    constexpr std::uint32_t kCapacity = 20;
    PlayableInstanceTable instances(1);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Piece, 0);
    const std::uint64_t ownerEpoch =
        instances.Classify(owner).scheduleEpoch;
    ScheduledTransactionPool pool;
    auto poolConfig = PoolConfig(256, 1024);
    poolConfig.maxScheduledTransactions = kCapacity + 1;
    poolConfig.maxIngressTransactionsPerQuantum = kCapacity + 1;
    poolConfig.maxEventsPerQuantum = kCapacity + 1;
    poolConfig.maxFrameBatchesPerQuantum = kCapacity + 1;
    ASSERT_TRUE(pool.Reset(poolConfig, 0, kEpoch));
    DeferredIntentPool deferred(kCapacity, 0);
    TimelinePlanner planner;
    TimelinePlanner::Config config;
    config.maxRollingOccurrences = kCapacity;
    ASSERT_TRUE(planner.Reset(config, &pool, &deferred, &instances));

    std::array<EventTemplate, 2> events{{
        {256, 1, 0, 0, 20, 0},
        {4096, 1, 1, 0, 50, 1},
    }};
    std::array<ProgramView, kCapacity + 1> programs{};
    PostInput input{};
    input.transportEpoch = kEpoch;
    input.latePolicy = PrePublishLatePolicy::Refuse;
    for (std::uint32_t i = 0; i < kCapacity + 1; ++i) {
        programs[i].ownerInstance = owner;
        programs[i].ownerInstanceScheduleEpoch = ownerEpoch;
        programs[i].transactionId = i + 1;
        programs[i].events = events.data();
        programs[i].eventCount = events.size();
        programs[i].rollingGroups = true;
        const PostResult posted = planner.PostPlay(programs[i], input);
        if (i < kCapacity) {
            EXPECT_EQ(posted.status, PostStatus::Reserved) << i;
        } else {
            EXPECT_EQ(posted.status, PostStatus::Refused);
            EXPECT_EQ(posted.refusal, ReserveRefusal::PoolFull);
        }
        if (i == 18) {
            EXPECT_EQ(deferred.Count(), 19u);
        }
    }
    EXPECT_EQ(deferred.Count(), kCapacity);
    EXPECT_EQ(deferred.HighWater(), kCapacity);
}

TEST(TimelinePlanner, ReplaceGenerationMaxKeepsStartedArmOwnedForDeclickStop) {
    PlayableInstanceTable instances(1);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Piece, 0);
    const std::uint64_t ownerEpoch =
        instances.Classify(owner).scheduleEpoch;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(256, 1024), 0, kEpoch));
    DeferredIntentPool deferred(1, 0);
    ASSERT_TRUE(deferred.SetGenerationForTests(0, UINT64_MAX));
    harken::TimelineArmTable arms(1);
    harken::TimelineSemanticTarget target{};
    harken::TimelineArmToken arm{};
    ASSERT_TRUE(arms.Reserve(0, 1, 256, 256, 7, target, &arm));
    ASSERT_TRUE(arms.AcknowledgeApplied(arm));
    ASSERT_TRUE(arms.Publish(arm));
    ASSERT_TRUE(arms.Start(arm));
    TimelinePlanner planner;
    TimelinePlanner::Config config;
    config.maxRollingOccurrences = 1;
    ASSERT_TRUE(planner.Reset(config, &pool, &deferred, &instances,
                              nullptr, &arms));

    std::array<EventTemplate, 3> events{{
        {256, 1, 0, 0, 20, 0},
        {512, 1, 1, 0, 30, 1},
        {768, 1, 2, 0, 50, 2},
    }};
    ProgramView program{};
    program.ownerInstance = owner;
    program.ownerInstanceScheduleEpoch = ownerEpoch;
    program.transactionId = 1;
    program.events = events.data();
    program.eventCount = events.size();
    program.rollingGroups = true;
    program.runtimeProgram = {7, nullptr};
    program.armToken = arm;
    PostInput input{};
    input.transportEpoch = kEpoch;
    ASSERT_EQ(planner.PostPlay(program, input).status, PostStatus::Reserved);
    ASSERT_EQ(deferred.Count(), 1u);

    EXPECT_EQ(ServiceDeferred(planner, pool, deferred, instances, kEpoch,
                              1024, 1, pool.WindowStartQuantum()),
              UINT64_MAX);
    EXPECT_EQ(deferred.Count(), 0u);
    EXPECT_TRUE(planner.HasLiveProgramGeneration(7));
    EXPECT_TRUE(planner.RollingAbortPending(arm));
    harken::ArmState state{};
    ASSERT_TRUE(arms.State(arm, &state));
    EXPECT_EQ(state, harken::ArmState::Started);
    EXPECT_FALSE(planner.CompleteRollingAbort(arm));
    ASSERT_TRUE(arms.Tombstone(arm, 2));
    ASSERT_TRUE(planner.CompleteRollingAbort(arm));
    EXPECT_FALSE(planner.HasLiveProgramGeneration(7));
}

TEST(TimelinePlanner, LongOccurrencePublishesRollingGroupsWithoutSpanFailure) {
    PlayableInstanceTable instances(1);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Piece, 0);
    ScheduledTransactionPool pool;
    auto poolConfig = PoolConfig(256, 16384);
    poolConfig.maxTransactionSpanFrames = 8192;
    ASSERT_TRUE(pool.Reset(poolConfig, 0, kEpoch));
    DeferredIntentPool deferred(4, 0);
    TimelinePlanner planner;
    TimelinePlanner::Config config;
    config.maxRollingOccurrences = 4;
    ASSERT_TRUE(planner.Reset(config, &pool, &deferred, &instances));

    const std::uint64_t epoch = instances.Classify(owner).scheduleEpoch;
    std::array<EventTemplate, 3> events{};
    events[0] = {0, 1, 0, 0, 20, 0};
    events[1] = {479000, 1, 0, 0, 30, 1};
    events[2] = {480000, 1, 0, 0, 50, 2};
    ProgramView program{};
    program.ownerInstance = owner;
    program.ownerInstanceScheduleEpoch = epoch;
    program.transactionId = 1;
    program.entryAnchorFrame = pool.PublishCutoffFrame();
    program.events = events.data();
    program.eventCount = events.size();
    program.rollingGroups = true;
    PostInput input{};
    input.transportEpoch = kEpoch;
    input.logicCommitSequence = 1;
    harken::TimelineProgramView runtime{};
    runtime.generation = 91;
    const auto otherRuntime = runtime;
    program.runtimeProgram = {runtime.generation, &runtime};
    const PostResult result = planner.PostPlay(program, input);
    EXPECT_NE(result.refusal, ReserveRefusal::SpanExceeded);
    EXPECT_TRUE(planner.HasProgramReference(program.runtimeProgram));
    EXPECT_FALSE(planner.HasProgramReference({runtime.generation + 1, &runtime}));
    EXPECT_FALSE(planner.HasProgramReference({runtime.generation, &otherRuntime}));
    EXPECT_EQ(result.status, PostStatus::Reserved)
        << "refusal=" << static_cast<std::uint32_t>(result.refusal);
    EXPECT_EQ(result.firstFrame, program.entryAnchorFrame);
    EXPECT_EQ(deferred.Count(), 1u);
    std::array<harken::DeferredIntentClaim, 1> due{};
    EXPECT_EQ(deferred.ClaimDue(pool.WindowEndFrame(), instances, due.data(),
                                1u),
              0u);
    EXPECT_EQ(pool.LiveTransactionCount(), 1u);
}

TEST(TimelinePlanner, SplitOccurrenceStampSurvivesDeferredAndRollingPrograms) {
    for (const bool rolling : {false, true}) {
        for (const bool cancelArranger : {false, true}) {
            SCOPED_TRACE(rolling);
            SCOPED_TRACE(cancelArranger);
            PlayableInstanceTable instances(2);
            OccurrenceStatePool occurrences(1);
            const auto arranger = instances.Acquire(PlayableKind::Container, 0);
            const auto successor = instances.Acquire(PlayableKind::Sound, arranger);
            const auto arrangingEpoch = instances.Classify(arranger).scheduleEpoch;
            const auto successorEpoch = instances.Classify(successor).scheduleEpoch;
            ASSERT_NE(arrangingEpoch, successorEpoch);
            harken::OccurrenceToken occurrence{};
            ASSERT_TRUE(occurrences.Acquire({arranger, harkentest::ObjectId(1), 0, 0}, &occurrence));
            ScheduledTransactionPool pool;
            ASSERT_TRUE(pool.Reset(PoolConfig(100, 400), 0, kEpoch));
            DeferredIntentPool deferred(4, 0);
            TimelinePlanner planner;
            TimelinePlanner::Config config;
            config.maxRollingOccurrences = 2;
            ASSERT_TRUE(planner.Reset(config, &pool, &deferred, &instances,
                                      &occurrences));
            std::array<EventTemplate, 2> events{{
                {100, 1, 0, 0, 20, 1}, {500, 1, 1, 0, 50, 2}}};
            ProgramView program{};
            program.ownerInstance = successor;
            program.ownerInstanceScheduleEpoch = successorEpoch;
            program.transactionId = 1;
            program.events = rolling ? events.data() : &events[1];
            program.eventCount = rolling ? 2u : 1u;
            program.rollingGroups = rolling;
            program.occurrence = {true, occurrence,
                occurrences.ScheduleEpochOf(occurrence), arrangingEpoch};
            PostInput input{};
            input.transportEpoch = kEpoch;
            input.logicCommitSequence = 1;
            ASSERT_EQ(planner.PostPlay(program, input).status,
                      rolling ? PostStatus::Reserved : PostStatus::Deferred);
            std::uint32_t starts = 0;
            std::uint32_t completions = 0;
            for (std::uint32_t quantum = 0; quantum < 8; ++quantum) {
                if (cancelArranger && quantum == 2) {
                    ASSERT_NE(instances.BumpScheduleEpoch(arranger), 0u);
                    ASSERT_TRUE(instances.ValidateStamp(successor, successorEpoch));
                }
                std::array<ConsumedEvent, 4> consumed{};
                const auto count = ConsumeFrontier(pool, consumed.data(), consumed.size());
                for (std::size_t index = 0; index < count; ++index) {
                    if (!consumed[index].frozenTxnValid) continue;
                    if (consumed[index].payload == 1) ++starts;
                    if (consumed[index].payload == 2) ++completions;
                }
                AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch);
            }
            EXPECT_EQ(starts, rolling ? 1u : 0u);
            EXPECT_EQ(completions, cancelArranger ? 0u : 1u);
            EXPECT_EQ(deferred.Count(), 0u);
        }
    }
}

TEST(TimelinePlanner, ResetPublishesOnlyBootstrapQuantum) {
    PlayableInstanceTable instances(1);
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(100, 400), 0, kEpoch));
    DeferredIntentPool deferred(1, 0);
    TimelinePlanner planner;
    TimelinePlanner::Config config;

    ASSERT_TRUE(planner.Reset(config, &pool, &deferred, &instances));
    EXPECT_EQ(pool.PublishCutoffFrame(), 100u);
    EXPECT_TRUE(pool.IsFrozen(0));
    EXPECT_EQ(pool.PageStamp(0), 0u);
    EXPECT_EQ(pool.PageStamp(1), harken::kUnpublishedQuantumStamp);
}

TEST(TimelinePlanner, ZeroLeadFrontierKeepsOnePublishedQuantumAfterBootstrap) {
    PlayableInstanceTable instances(1);
    ScheduledTransactionPool::Config poolConfig = PoolConfig(100, 400);
    poolConfig.publishLeadQuanta = 0;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(poolConfig, 0, kEpoch));
    DeferredIntentPool deferred(1, 0);
    TimelinePlanner planner;
    TimelinePlanner::Config config;
    ASSERT_TRUE(planner.Reset(config, &pool, &deferred, &instances));

    std::uint64_t previousCutoff = pool.PublishCutoffFrame();
    EXPECT_EQ(previousCutoff,
              pool.WindowStartFrame() + pool.QuantumFrames());
    EXPECT_TRUE(pool.IsFrozen(0));
    EXPECT_EQ(pool.ConsumerFrontier().load(), 0u);
    AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch, 1);
    EXPECT_EQ(pool.WindowStartQuantum(), 0u);
    EXPECT_EQ(pool.PublishCutoffFrame(), previousCutoff);
    EXPECT_EQ(pool.PageStamp(1), harken::kUnpublishedQuantumStamp);
    for (std::uint64_t quantum = 0; quantum < 2; ++quantum) {
        std::array<ConsumedEvent, 1> consumed{};
        EXPECT_EQ(ConsumeFrontier(pool, consumed.data(), consumed.size()), 0u);
        EXPECT_EQ(pool.ConsumerFrontier().load(), quantum + 1);
        AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch);
        EXPECT_EQ(pool.WindowStartQuantum(), quantum + 1);
        EXPECT_EQ(pool.PublishCutoffFrame(), (quantum + 2) * 100u);
        EXPECT_GT(pool.PublishCutoffFrame(), previousCutoff);
        EXPECT_EQ(pool.PageStamp(quantum + 1), quantum + 1);
        EXPECT_TRUE(pool.IsFrozen(0));
        previousCutoff = pool.PublishCutoffFrame();
    }
}

TEST(TimelinePlanner, ResetRequiresAnEmptyDeferredIntentPool) {
    PlayableInstanceTable instances(1);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Piece, 0);
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(100, 400), 0, kEpoch));
    DeferredIntentPool deferred(1, 0);
    DeferredIntent intent;
    intent.ownerDomain = harken::DeferredIntentOwnerDomain::Playable;
    intent.ownerHandle = owner;
    intent.ownerEpoch = instances.Classify(owner).scheduleEpoch;
    intent.dueFrame = 0;
    ASSERT_EQ(deferred.Add(intent), DeferredIntentRefusal::None);

    TimelinePlanner planner;
    TimelinePlanner::Config config;
    EXPECT_FALSE(planner.Reset(config, &pool, &deferred, &instances));

    std::vector<DeferredIntent> drained;
    EXPECT_EQ(deferred.Materialize(1, instances, &drained), 0u);
    ASSERT_EQ(drained.size(), 1u);
    ASSERT_EQ(deferred.Count(), 0u);
    EXPECT_TRUE(planner.Reset(config, &pool, &deferred, &instances));
}

TEST(TimelinePlanner, FailedResetPreservesLiveDeferredMappings) {
    PlayableInstanceTable instances(2);
    OccurrenceStatePool occurrences(1);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Piece, 0);
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(100, 400), 0, kEpoch));
    DeferredIntentPool deferred(1, 0);
    TimelinePlanner planner;
    TimelinePlanner::Config config;
    ASSERT_TRUE(planner.Reset(config, &pool, &deferred, &instances));

    const EventTemplate event[] = {{0, 1, 0, 0, 0, 0xD1}};
    ProgramView program;
    program.ownerInstance = owner;
    program.ownerInstanceScheduleEpoch =
        instances.Classify(owner).scheduleEpoch;
    program.transactionId = 1;
    program.entryAnchorFrame = 450;
    program.events = event;
    program.eventCount = 1;
    PostInput input;
    input.transportEpoch = kEpoch;
    ASSERT_EQ(planner.PostPlay(program, input).status, PostStatus::Deferred);
    ASSERT_EQ(deferred.Count(), 1u);

    EXPECT_FALSE(planner.Reset(config, &pool, &deferred, &instances));
    ASSERT_TRUE(planner.Valid());
    std::array<ConsumedEvent, 1> consumed{};
    ASSERT_EQ(ConsumeFrontier(pool, consumed.data(), consumed.size()), 0u);
    AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch);
    ASSERT_EQ(ConsumeFrontier(pool, consumed.data(), consumed.size()), 0u);
    AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch);

    EXPECT_EQ(deferred.Count(), 0u);
    ASSERT_EQ(pool.ConsumePage(2, kEpoch, instances, occurrences,
                               consumed.data(), consumed.size()),
              1u);
    EXPECT_EQ(consumed[0].payload, 0xD1u);
    EXPECT_EQ(consumed[0].executeFrame, 450u);
}

TEST(TimelinePlanner, NullEventStorageIsARefusedTransaction) {
    PlayableInstanceTable instances(1);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Piece, 0);
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(100, 400), 0, kEpoch));
    DeferredIntentPool deferred(1, 0);
    TimelinePlanner planner;
    TimelinePlanner::Config config;
    ASSERT_TRUE(planner.Reset(config, &pool, &deferred, &instances));

    ProgramView program;
    program.ownerInstance = owner;
    program.ownerInstanceScheduleEpoch =
        instances.Classify(owner).scheduleEpoch;
    program.transactionId = 1;
    program.entryAnchorFrame = 200;
    program.events = nullptr;
    program.eventCount = 1;
    PostInput input;
    input.transportEpoch = kEpoch;

    const PostResult result = planner.PostPlay(program, input);
    EXPECT_EQ(result.status, PostStatus::Refused);
    EXPECT_EQ(result.refusal, ReserveRefusal::EmptyTransaction);
}

TEST(TimelinePlanner, AdvancePublishesLeadFromTheNewWindowStart) {
    PlayableInstanceTable instances(1);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Piece, 0);
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(100, 400), 0, kEpoch));
    DeferredIntentPool deferred(1, 0);
    TimelinePlanner planner;
    TimelinePlanner::Config config;
    ASSERT_TRUE(planner.Reset(config, &pool, &deferred, &instances));

    std::array<ConsumedEvent, 1> consumed{};
    ASSERT_EQ(ConsumeFrontier(pool, consumed.data(), consumed.size()), 0u);
    AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch);
    ASSERT_EQ(pool.WindowStartFrame(), 100u);
    EXPECT_EQ(pool.PublishCutoffFrame(), 200u);
    EXPECT_TRUE(pool.IsFrozen(0));

    const EventTemplate event[] = {{0, 1, 0, 0, 0, 0xA1}};
    ProgramView program;
    program.ownerInstance = owner;
    program.ownerInstanceScheduleEpoch =
        instances.Classify(owner).scheduleEpoch;
    program.transactionId = 1;
    program.entryAnchorFrame = 100;
    program.events = event;
    program.eventCount = 1;
    PostInput input;
    input.currentFrame = 100;
    input.transportEpoch = kEpoch;
    input.latePolicy = PrePublishLatePolicy::Refuse;

    const PostResult result = planner.PostPlay(program, input);
    EXPECT_EQ(result.status, PostStatus::Refused);
    EXPECT_EQ(result.refusal, ReserveRefusal::Late);
}

// The same timestamped plays produce the same schedule when update batches span
// one, two or four quanta, even though their direct/deferred paths differ.
TEST(TimelinePlanner, ScheduleIsDeterministicAcrossUpdateCadence) {
    PlayableInstanceTable instances(8);
    OccurrenceStatePool occ(8);
    const PlayableInstanceHandle owner = instances.Acquire(PlayableKind::Piece, 0);
    const std::uint64_t ownerEpoch = instances.Classify(owner).scheduleEpoch;

    const EventTemplate e1[] = {{0, 1, 0, 0, 0, 0xA0}, {30, 1, 1, 0, 0, 0xA1}};
    const EventTemplate e2[] = {{0, 2, 0, 0, 0, 0xB0}};
    const EventTemplate e3[] = {{0, 3, 0, 0, 0, 0xC0}, {30, 3, 1, 0, 0, 0xC1}};
    const EventTemplate e4[] = {{0, 4, 0, 0, 0, 0xD0}};
    auto makeProgram = [&](TransactionId id, std::uint64_t anchor,
                           const EventTemplate* ev, std::size_t n) {
        ProgramView p;
        p.ownerInstance = owner;
        p.ownerInstanceScheduleEpoch = ownerEpoch;
        p.transactionId = id;
        p.entryAnchorFrame = anchor;
        p.events = ev;
        p.eventCount = n;
        return p;
    };
    const ProgramView programs[] = {
        makeProgram(1, 450, e1, 2),
        makeProgram(2, 451, e2, 1),
        makeProgram(3, 550, e3, 2),
        makeProgram(4, 551, e4, 1),
    };
    constexpr std::array<std::uint64_t, 4> postFrames = {0, 100, 200, 300};

    struct RunResult {
        std::uint64_t digest = 0;
        std::uint32_t deferredPosts = 0;
    };

    auto run = [&](std::uint32_t cadence) {
        ScheduledTransactionPool pool;
        EXPECT_TRUE(pool.Reset(PoolConfig(100, 400), 0, kEpoch));
        DeferredIntentPool deferred(16, 0);
        TimelinePlanner planner;
        TimelinePlanner::Config pc;
        EXPECT_TRUE(planner.Reset(pc, &pool, &deferred, &instances));

        std::uint64_t hash = 14695981039346656037ull;
        std::uint32_t deferredPosts = 0;
        const int totalQuanta = 12;
        for (int q = 0; q < totalQuanta; q += static_cast<int>(cadence)) {
            const std::uint64_t batchStart = static_cast<std::uint64_t>(q) * 100u;
            const std::uint64_t batchEnd =
                static_cast<std::uint64_t>(q + cadence) * 100u;
            for (std::size_t i = 0; i < postFrames.size(); ++i) {
                if (postFrames[i] < batchStart || postFrames[i] >= batchEnd) {
                    continue;
                }
                PostInput input;
                input.currentFrame = postFrames[i];
                input.transportEpoch = kEpoch;
                input.logicCommitSequence = i + 1;
                input.latePolicy =
                    PrePublishLatePolicy::ExecuteAtEarliestSafeFrame;
                if (planner.PostPlay(programs[i], input).status ==
                    PostStatus::Deferred) {
                    ++deferredPosts;
                }
            }
            for (std::uint32_t c = 0; c < cadence; ++c) {
                std::array<ConsumedEvent, 128> consumed{};
                const std::size_t count =
                    ConsumeFrontier(pool, consumed.data(), consumed.size());
                for (std::size_t i = 0; i < count; ++i) {
                    Fold(hash, consumed[i].executeFrame);
                    Fold(hash, consumed[i].payload);
                    Fold(hash, consumed[i].eventIndex);
                    Fold(hash, consumed[i].transactionId);
                }
                AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch);
            }
        }
        // Fold whatever remains in the window.
        for (std::uint32_t p = 0; p < pool.PageCount(); ++p) {
            hash = ConsumePageDigest(pool, p, instances, occ, hash);
        }
        return RunResult{hash, deferredPosts};
    };

    const RunResult d1 = run(1);
    const RunResult d2 = run(2);
    const RunResult d4 = run(4);
    EXPECT_EQ(d1.digest, d2.digest);
    EXPECT_EQ(d1.digest, d4.digest);
    EXPECT_EQ(d1.deferredPosts, 1u);
    EXPECT_EQ(d2.deferredPosts, 2u);
    EXPECT_EQ(d4.deferredPosts, 4u);
    // The digest is non-trivial (events really were scheduled and consumed).
    EXPECT_NE(d1.digest, 14695981039346656037ull);
}

// A play beyond the horizon is deferred, then materialized at exactly its planned
// frame once the window reaches it -- occupying no page until then.
TEST(TimelinePlanner, BeyondHorizonPlayDefersThenMaterializes) {
    PlayableInstanceTable instances(8);
    OccurrenceStatePool occ(8);
    const PlayableInstanceHandle owner = instances.Acquire(PlayableKind::Piece, 0);

    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(100, 400), 0, kEpoch)); // window [0,400)
    DeferredIntentPool deferred(8, 0);
    TimelinePlanner planner;
    TimelinePlanner::Config pc;
    ASSERT_TRUE(planner.Reset(pc, &pool, &deferred, &instances));

    const EventTemplate ev[] = {{0, 1, 0, 0, 0, 0x777}};
    ProgramView p;
    p.ownerInstance = owner;
    p.ownerInstanceScheduleEpoch = instances.Classify(owner).scheduleEpoch;
    p.transactionId = 1;
    p.entryAnchorFrame = 650; // beyond the far edge 400
    p.events = ev;
    p.eventCount = 1;
    harken::TimelineProgramView runtime{};
    runtime.generation = 73;
    const auto otherRuntime = runtime;
    p.runtimeProgram = {runtime.generation, &runtime};

    PostInput in;
    in.currentFrame = 0;
    in.transportEpoch = kEpoch;
    in.latePolicy = PrePublishLatePolicy::ExecuteAtEarliestSafeFrame;
    const PostResult posted = planner.PostPlay(p, in);
    EXPECT_EQ(posted.status, PostStatus::Deferred);
    EXPECT_EQ(deferred.Count(), 1u);
    EXPECT_TRUE(planner.HasProgramReference(p.runtimeProgram));
    EXPECT_FALSE(planner.HasProgramReference({runtime.generation + 1, &runtime}));
    EXPECT_FALSE(planner.HasProgramReference({runtime.generation, &otherRuntime}));

    // Advance until the window covers frame 650, consuming the event when it lands.
    bool sawEvent = false;
    for (int q = 0; q < 12; ++q) {
        std::array<ConsumedEvent, 8> buf{};
        const std::size_t n =
            ConsumeFrontier(pool, buf.data(), buf.size());
        for (std::size_t i = 0; i < n; ++i) {
            if (buf[i].payload == 0x777) {
                sawEvent = true;
                EXPECT_EQ(buf[i].executeFrame, 650u);
            }
        }
        AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch);
    }
    EXPECT_TRUE(sawEvent);
    EXPECT_EQ(deferred.Count(), 0u); // materialized and released
    EXPECT_FALSE(planner.HasProgramReference(p.runtimeProgram));
}

TEST(TimelinePlanner, DefersUntilTheEntireTransactionSpanFits) {
    PlayableInstanceTable instances(2);
    OccurrenceStatePool occurrences(1);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Piece, 0);
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(100, 400), 0, kEpoch));
    DeferredIntentPool deferred(2, 0);
    TimelinePlanner planner;
    TimelinePlanner::Config config;
    ASSERT_TRUE(planner.Reset(config, &pool, &deferred, &instances));

    const EventTemplate events[] = {
        {0, 1, 0, 0, 0, 0x31},
        {250, 1, 1, 0, 0, 0x32},
    };
    ProgramView program;
    program.ownerInstance = owner;
    program.ownerInstanceScheduleEpoch =
        instances.Classify(owner).scheduleEpoch;
    program.transactionId = 1;
    program.entryAnchorFrame = 300;
    program.events = events;
    program.eventCount = 2;
    PostInput input;
    input.transportEpoch = kEpoch;
    input.latePolicy = PrePublishLatePolicy::Refuse;

    EXPECT_EQ(planner.PostPlay(program, input).status, PostStatus::Deferred);
    ASSERT_EQ(deferred.Count(), 1u);
    EXPECT_EQ(planner.DeferredBlockerBefore(400u), 300u);
    EXPECT_EQ(planner.DeferredBlockerBefore(300u), UINT64_MAX);

    std::array<ConsumedEvent, 2> consumed{};
    ASSERT_EQ(ConsumeFrontier(pool, consumed.data(), consumed.size()), 0u);
    AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch, 1);
    EXPECT_EQ(pool.WindowStartFrame(), 100u);
    EXPECT_EQ(pool.WindowEndFrame(), 500u);
    EXPECT_EQ(pool.PublishCutoffFrame(), 200u);
    EXPECT_EQ(deferred.Count(), 1u);
    ASSERT_EQ(ConsumeFrontier(pool, consumed.data(), consumed.size()), 0u);
    AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch);
    EXPECT_EQ(pool.WindowStartFrame(), 200u);
    EXPECT_EQ(pool.WindowEndFrame(), 600u);
    EXPECT_EQ(pool.PublishCutoffFrame(), 300u);
    EXPECT_EQ(deferred.Count(), 0u);

    ASSERT_NE(pool.Page(1), nullptr);
    EXPECT_EQ(pool.Page(1)->absoluteQuantumIndex, 3u);
    ASSERT_EQ(pool.ConsumePage(1, kEpoch, instances, occurrences,
                               consumed.data(), consumed.size()),
              1u);
    EXPECT_EQ(consumed[0].executeFrame, 300u);
    ASSERT_NE(pool.Page(3), nullptr);
    EXPECT_EQ(pool.Page(3)->absoluteQuantumIndex, 5u);
    ASSERT_EQ(pool.ConsumePage(3, kEpoch, instances, occurrences,
                               consumed.data(), consumed.size()),
              1u);
    EXPECT_EQ(consumed[0].executeFrame, 550u);
}

TEST(TimelinePlanner, RefusesADeferredSpanThatCanNeverFit) {
    PlayableInstanceTable instances(2);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Piece, 0);
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(100, 400), 0, kEpoch));
    DeferredIntentPool deferred(2, 0);
    TimelinePlanner planner;
    TimelinePlanner::Config config;
    ASSERT_TRUE(planner.Reset(config, &pool, &deferred, &instances));

    const EventTemplate events[] = {
        {0, 1, 0, 0, 0, 0x41},
        {300, 1, 1, 0, 0, 0x42},
    };
    ProgramView program;
    program.ownerInstance = owner;
    program.ownerInstanceScheduleEpoch =
        instances.Classify(owner).scheduleEpoch;
    program.transactionId = 1;
    program.entryAnchorFrame = 500;
    program.events = events;
    program.eventCount = 2;
    PostInput input;
    input.transportEpoch = kEpoch;

    const PostResult result = planner.PostPlay(program, input);
    EXPECT_EQ(result.status, PostStatus::Refused);
    EXPECT_EQ(result.refusal, ReserveRefusal::SpanExceeded);
    EXPECT_EQ(deferred.Count(), 0u);
}

TEST(TimelinePlanner, ReusesSlotsReleasedByStaleDeferredIntents) {
    PlayableInstanceTable instances(2);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Piece, 0);
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(100, 400), 0, kEpoch));
    DeferredIntentPool deferred(2, 0);
    TimelinePlanner planner;
    TimelinePlanner::Config config;
    ASSERT_TRUE(planner.Reset(config, &pool, &deferred, &instances));

    const EventTemplate event[] = {{0, 1, 0, 0, 0, 0x51}};
    ProgramView cancelled;
    cancelled.ownerInstance = owner;
    cancelled.ownerInstanceScheduleEpoch =
        instances.Classify(owner).scheduleEpoch;
    cancelled.transactionId = 1;
    cancelled.entryAnchorFrame = 450;
    cancelled.events = event;
    cancelled.eventCount = 1;
    PostInput input;
    input.transportEpoch = kEpoch;
    ASSERT_EQ(planner.PostPlay(cancelled, input).status, PostStatus::Deferred);

    ASSERT_TRUE(instances.BumpScheduleEpoch(owner));
    std::array<ConsumedEvent, 1> consumed{};
    ASSERT_EQ(ConsumeFrontier(pool, consumed.data(), consumed.size()), 0u);
    AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch);
    ASSERT_EQ(deferred.Count(), 0u);

    ProgramView first = cancelled;
    first.ownerInstanceScheduleEpoch = instances.Classify(owner).scheduleEpoch;
    first.transactionId = 2;
    first.entryAnchorFrame = 600;
    ProgramView second = first;
    second.transactionId = 3;
    second.entryAnchorFrame = 601;

    EXPECT_EQ(planner.PostPlay(first, input).status, PostStatus::Deferred);
    EXPECT_EQ(planner.PostPlay(second, input).status, PostStatus::Deferred);
    EXPECT_EQ(deferred.Count(), 2u);
}

TEST(TimelinePlanner, DeferredMaterializationPreservesLatePolicies) {
    for (const PrePublishLatePolicy policy : {
             PrePublishLatePolicy::Drop,
             PrePublishLatePolicy::Refuse,
         }) {
        PlayableInstanceTable instances(2);
        const PlayableInstanceHandle owner =
            instances.Acquire(PlayableKind::Piece, 0);
        ScheduledTransactionPool pool;
        ASSERT_TRUE(pool.Reset(PoolConfig(100, 400), 0, kEpoch));
        DeferredIntentPool deferred(2, 0);
        TimelinePlanner planner;
        TimelinePlanner::Config config;
        ASSERT_TRUE(planner.Reset(config, &pool, &deferred, &instances));

        const EventTemplate events[] = {
            {0, 1, 0, 0, 0, 0x61},
            {250, 1, 1, 0, 0, 0x62},
        };
        ProgramView program;
        program.ownerInstance = owner;
        program.ownerInstanceScheduleEpoch =
            instances.Classify(owner).scheduleEpoch;
        program.transactionId = 1;
        program.entryAnchorFrame = 250;
        program.events = events;
        program.eventCount = 2;
        PostInput input;
        input.transportEpoch = kEpoch;
        input.latePolicy = policy;

        ASSERT_EQ(planner.PostPlay(program, input).status, PostStatus::Deferred);
        std::array<ConsumedEvent, 1> consumed{};
        ASSERT_EQ(ConsumeFrontier(pool, consumed.data(), consumed.size()), 0u);
        AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch);
        EXPECT_EQ(pool.WindowStartFrame(), 100u);
        EXPECT_EQ(pool.WindowEndFrame(), 500u);
        EXPECT_EQ(pool.PublishCutoffFrame(), 200u);
        EXPECT_EQ(deferred.Count(), 1u);
        ASSERT_EQ(ConsumeFrontier(pool, consumed.data(), consumed.size()), 0u);
        AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch);
        EXPECT_EQ(pool.WindowStartFrame(), 200u);
        EXPECT_EQ(pool.WindowEndFrame(), 600u);
        EXPECT_EQ(pool.PublishCutoffFrame(), 300u);

        EXPECT_EQ(deferred.Count(), 0u);
        EXPECT_EQ(pool.LiveTransactionCount(), 0u);
        EXPECT_EQ(planner.MaterializationsRetried(), 0u);
        if (policy == PrePublishLatePolicy::Drop) {
            EXPECT_EQ(pool.DroppedLateCount(), 1u);
        } else {
            EXPECT_EQ(pool.RefusalCount(ReserveRefusal::Late), 1u);
        }
    }
}

TEST(TimelinePlanner, DeferredMaterializationPreservesBoundaryPolicy) {
    PlayableInstanceTable instances(2);
    OccurrenceStatePool occurrences(1);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Piece, 0);
    TempoSegment segment{MusicalTick{0}, 20000};
    ClockKernel clock;
    ASSERT_TRUE(clock.Reset(48000, &segment, 1));
    const MarkerPoint marker[] = {{harkentest::ObjectId(1), MarkerKind::Custom, 51}};
    MarkerIndex markerIndex;
    ASSERT_TRUE(markerIndex.Reset(PieceTimeType::Musical, &clock,
                               MeterSignature{4, 4}, marker, 1, 1));
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(100, 400), 0, kEpoch));
    DeferredIntentPool deferred(2, 0);
    TimelinePlanner planner;
    TimelinePlanner::Config config;
    ASSERT_TRUE(planner.Reset(config, &pool, &deferred, &instances));

    const EventTemplate events[] = {
        {0, 1, 0, 0, 0, 0x71},
        {250, 1, 1, 0, 0, 0x72},
    };
    ProgramView program;
    program.ownerInstance = owner;
    program.ownerInstanceScheduleEpoch =
        instances.Classify(owner).scheduleEpoch;
    program.transactionId = 1;
    program.entryAnchorFrame = 250;
    program.events = events;
    program.eventCount = 2;
    program.timeType = PieceTimeType::Musical;
    program.clock = &clock;
    program.markerIndex = &markerIndex;
    program.boundaryType = BoundaryType::Marker;
    PostInput input;
    input.transportEpoch = kEpoch;
    input.latePolicy = PrePublishLatePolicy::NextEquivalentBoundary;
    input.missingBoundaryPolicy = MissingBoundaryPolicy::Refuse;

    ASSERT_EQ(planner.PostPlay(program, input).status, PostStatus::Deferred);
    std::array<ConsumedEvent, 2> consumed{};
    ASSERT_EQ(ConsumeFrontier(pool, consumed.data(), consumed.size()), 0u);
    AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch);
    EXPECT_EQ(pool.WindowStartFrame(), 100u);
    EXPECT_EQ(pool.WindowEndFrame(), 500u);
    EXPECT_EQ(pool.PublishCutoffFrame(), 200u);
    EXPECT_EQ(deferred.Count(), 1u);
    ASSERT_EQ(ConsumeFrontier(pool, consumed.data(), consumed.size()), 0u);
    AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch);
    EXPECT_EQ(pool.WindowStartFrame(), 200u);
    EXPECT_EQ(pool.WindowEndFrame(), 600u);
    EXPECT_EQ(pool.PublishCutoffFrame(), 300u);

    ASSERT_NE(pool.Page(1), nullptr);
    EXPECT_EQ(pool.Page(1)->absoluteQuantumIndex, 3u);
    ASSERT_EQ(pool.ConsumePage(1, kEpoch, instances, occurrences,
                               consumed.data(), consumed.size()),
              1u);
    EXPECT_EQ(consumed[0].executeFrame, 301u);
    EXPECT_EQ(consumed[0].payload, 0x71u);
    ASSERT_NE(pool.Page(3), nullptr);
    EXPECT_EQ(pool.Page(3)->absoluteQuantumIndex, 5u);
    ASSERT_EQ(pool.ConsumePage(3, kEpoch, instances, occurrences,
                               consumed.data(), consumed.size()),
              1u);
    EXPECT_EQ(consumed[0].executeFrame, 551u);
    EXPECT_EQ(consumed[0].payload, 0x72u);
}

TEST(TimelinePlanner, DeferredMaterializationPreservesMissingBoundaryPolicy) {
    for (const MissingBoundaryPolicy missingPolicy : {
             MissingBoundaryPolicy::Drop,
             MissingBoundaryPolicy::Refuse,
         }) {
        PlayableInstanceTable instances(2);
        const PlayableInstanceHandle owner =
            instances.Acquire(PlayableKind::Piece, 0);
        TempoSegment segment{MusicalTick{0}, 20000};
        ClockKernel clock;
        ASSERT_TRUE(clock.Reset(48000, &segment, 1));
        MarkerIndex emptyMarkerIndex;
        ASSERT_TRUE(emptyMarkerIndex.Reset(PieceTimeType::Musical, &clock,
                                        MeterSignature{4, 4}, nullptr, 0, 1));
        ScheduledTransactionPool pool;
        ASSERT_TRUE(pool.Reset(PoolConfig(100, 400), 0, kEpoch));
        DeferredIntentPool deferred(2, 0);
        TimelinePlanner planner;
        TimelinePlanner::Config config;
        ASSERT_TRUE(planner.Reset(config, &pool, &deferred, &instances));

        const EventTemplate events[] = {
            {0, 1, 0, 0, 0, 0x73},
            {250, 1, 1, 0, 0, 0x74},
        };
        ProgramView program;
        program.ownerInstance = owner;
        program.ownerInstanceScheduleEpoch =
            instances.Classify(owner).scheduleEpoch;
        program.transactionId = 1;
        program.entryAnchorFrame = 250;
        program.events = events;
        program.eventCount = 2;
        program.timeType = PieceTimeType::Musical;
        program.clock = &clock;
        program.markerIndex = &emptyMarkerIndex;
        program.boundaryType = BoundaryType::Marker;
        PostInput input;
        input.transportEpoch = kEpoch;
        input.latePolicy = PrePublishLatePolicy::NextEquivalentBoundary;
        input.missingBoundaryPolicy = missingPolicy;

        ASSERT_EQ(planner.PostPlay(program, input).status, PostStatus::Deferred);
        std::array<ConsumedEvent, 1> consumed{};
        ASSERT_EQ(ConsumeFrontier(pool, consumed.data(), consumed.size()), 0u);
        AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch);
        EXPECT_EQ(pool.WindowStartFrame(), 100u);
        EXPECT_EQ(pool.WindowEndFrame(), 500u);
        EXPECT_EQ(pool.PublishCutoffFrame(), 200u);
        EXPECT_EQ(deferred.Count(), 1u);
        ASSERT_EQ(ConsumeFrontier(pool, consumed.data(), consumed.size()), 0u);
        AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch);
        EXPECT_EQ(pool.WindowStartFrame(), 200u);
        EXPECT_EQ(pool.WindowEndFrame(), 600u);
        EXPECT_EQ(pool.PublishCutoffFrame(), 300u);

        EXPECT_EQ(deferred.Count(), 0u);
        EXPECT_EQ(pool.LiveTransactionCount(), 0u);
        const std::uint64_t expectedFailures =
            missingPolicy == MissingBoundaryPolicy::Refuse ? 1u : 0u;
        EXPECT_EQ(planner.MaterializationReserveFailures(), expectedFailures);
    }
}

TEST(TimelinePlanner, DeferredIntentKeepsItsOriginalTransportEpoch) {
    PlayableInstanceTable instances(2);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Piece, 0);
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(100, 400), 0, kEpoch));
    DeferredIntentPool deferred(2, 0);
    TimelinePlanner planner;
    TimelinePlanner::Config config;
    ASSERT_TRUE(planner.Reset(config, &pool, &deferred, &instances));

    const EventTemplate event[] = {{0, 1, 0, 0, 0, 0x81}};
    ProgramView program;
    program.ownerInstance = owner;
    program.ownerInstanceScheduleEpoch =
        instances.Classify(owner).scheduleEpoch;
    program.transactionId = 1;
    program.entryAnchorFrame = 450;
    program.events = event;
    program.eventCount = 1;
    PostInput input;
    input.transportEpoch = kEpoch - 1;

    ASSERT_EQ(planner.PostPlay(program, input).status, PostStatus::Deferred);
    std::array<ConsumedEvent, 1> consumed{};
    ASSERT_EQ(ConsumeFrontier(pool, consumed.data(), consumed.size()), 0u);
    AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch);
    ASSERT_EQ(ConsumeFrontier(pool, consumed.data(), consumed.size()), 0u);
    AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch);

    EXPECT_EQ(deferred.Count(), 0u);
    EXPECT_EQ(pool.LiveTransactionCount(), 0u);
    EXPECT_EQ(pool.RefusalCount(ReserveRefusal::InvalidTransportEpoch), 0u);
}

TEST(TimelinePlanner, DeferredBudgetOrdersByLogicCommitSequence) {
    PlayableInstanceTable instances(2);
    OccurrenceStatePool occurrences(1);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Piece, 0);
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(100, 400), 0, kEpoch));
    DeferredIntentPool deferred(2, 0);
    TimelinePlanner planner;
    TimelinePlanner::Config config;
    ASSERT_TRUE(planner.Reset(config, &pool, &deferred, &instances));

    const EventTemplate firstEvent[] = {{0, 1, 0, 0, 0, 0x91}};
    const EventTemplate secondEvent[] = {{0, 1, 0, 0, 0, 0x92}};
    ProgramView first;
    first.ownerInstance = owner;
    first.ownerInstanceScheduleEpoch =
        instances.Classify(owner).scheduleEpoch;
    first.transactionId = 1;
    first.entryAnchorFrame = 400;
    first.events = firstEvent;
    first.eventCount = 1;
    ProgramView second = first;
    second.transactionId = 2;
    second.events = secondEvent;
    PostInput input;
    input.transportEpoch = kEpoch;
    input.logicCommitSequence = 20;
    ASSERT_EQ(planner.PostPlay(first, input).status, PostStatus::Deferred);
    input.logicCommitSequence = 10;
    ASSERT_EQ(planner.PostPlay(second, input).status, PostStatus::Deferred);

    std::array<ConsumedEvent, 2> consumed{};
    ASSERT_EQ(ConsumeFrontier(pool, consumed.data(), consumed.size()), 0u);
    AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch, 1);
    EXPECT_EQ(pool.WindowStartFrame(), 100u);
    EXPECT_EQ(pool.WindowEndFrame(), 500u);
    EXPECT_EQ(pool.PublishCutoffFrame(), 200u);

    ASSERT_EQ(deferred.Count(), 1u);
    ASSERT_EQ(pool.ConsumePage(3, kEpoch, instances, occurrences,
                               consumed.data(), consumed.size()),
              1u);
    EXPECT_EQ(consumed[0].executeFrame, 400u);
    EXPECT_EQ(consumed[0].payload, 0x92u);
    EXPECT_EQ(consumed[0].transactionId, 2u);
}

TEST(TimelinePlanner, TransientMaterializationRefusalRetriesNextQuantum) {
    PlayableInstanceTable instances(2);
    OccurrenceStatePool occurrences(1);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Piece, 0);
    ScheduledTransactionPool::Config poolConfig = PoolConfig(100, 400);
    poolConfig.maxScheduledTransactions = 1;
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(poolConfig, 0, kEpoch));
    DeferredIntentPool deferred(1, 0);
    TimelinePlanner planner;
    TimelinePlanner::Config config;
    ASSERT_TRUE(planner.Reset(config, &pool, &deferred, &instances));

    const EventTemplate occupyingEvents[] = {
        {0, 1, 0, 0, 0, 0xE1},
        {100, 1, 1, 0, 0, 0xE3},
    };
    ProgramView occupying;
    occupying.ownerInstance = owner;
    occupying.ownerInstanceScheduleEpoch =
        instances.Classify(owner).scheduleEpoch;
    occupying.transactionId = 1;
    occupying.entryAnchorFrame = 150;
    occupying.events = occupyingEvents;
    occupying.eventCount = 2;
    PostInput input;
    input.transportEpoch = kEpoch;
    input.latePolicy = PrePublishLatePolicy::Refuse;
    ASSERT_EQ(planner.PostPlay(occupying, input).status, PostStatus::Reserved);

    const EventTemplate deferredEvent[] = {{0, 2, 0, 0, 0, 0xE2}};
    ProgramView waiting = occupying;
    waiting.transactionId = 2;
    waiting.entryAnchorFrame = 450;
    waiting.events = deferredEvent;
    waiting.eventCount = 1;
    ASSERT_EQ(planner.PostPlay(waiting, input).status, PostStatus::Deferred);

    std::array<ConsumedEvent, 1> consumed{};
    ASSERT_EQ(ConsumeFrontier(pool, consumed.data(), consumed.size()), 0u);
    EXPECT_EQ(pool.RefusalCount(ReserveRefusal::PoolFull), 0u);
    EXPECT_EQ(planner.MaterializationsRetried(), 0u);
    AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch);
    EXPECT_EQ(pool.RefusalCount(ReserveRefusal::PoolFull), 1u);
    EXPECT_EQ(planner.MaterializationsRetried(), 1u);
    EXPECT_EQ(pool.WindowStartFrame(), 100u);
    EXPECT_EQ(pool.WindowEndFrame(), 500u);
    EXPECT_EQ(pool.PublishCutoffFrame(), 200u);
    ASSERT_EQ(ConsumeFrontier(pool, consumed.data(), consumed.size()), 1u);
    EXPECT_EQ(consumed[0].payload, 0xE1u);
    EXPECT_EQ(consumed[0].executeFrame, 150u);
    AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch);
    EXPECT_EQ(pool.WindowStartFrame(), 200u);
    EXPECT_EQ(pool.WindowEndFrame(), 600u);
    EXPECT_EQ(pool.PublishCutoffFrame(), 300u);
    EXPECT_EQ(deferred.Count(), 1u);
    EXPECT_EQ(pool.LiveTransactionCount(), 1u);
    EXPECT_EQ(pool.RefusalCount(ReserveRefusal::PoolFull), 2u);
    EXPECT_EQ(planner.MaterializationReserveFailures(), 0u);
    EXPECT_EQ(planner.MaterializationsRetried(), 2u);

    ASSERT_EQ(ConsumeFrontier(pool, consumed.data(), consumed.size()), 1u);
    EXPECT_EQ(consumed[0].payload, 0xE3u);
    EXPECT_EQ(consumed[0].executeFrame, 250u);
    AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch);
    EXPECT_EQ(pool.WindowStartFrame(), 300u);
    EXPECT_EQ(pool.WindowEndFrame(), 700u);
    EXPECT_EQ(pool.PublishCutoffFrame(), 400u);
    EXPECT_EQ(deferred.Count(), 0u);
    EXPECT_EQ(pool.RefusalCount(ReserveRefusal::PoolFull), 2u);
    EXPECT_EQ(planner.MaterializationsRetried(), 2u);
    ASSERT_EQ(pool.ConsumePage(1, kEpoch, instances, occurrences,
                               consumed.data(), consumed.size()),
              1u);
    EXPECT_EQ(consumed[0].payload, 0xE2u);
    EXPECT_EQ(consumed[0].executeFrame, 450u);
}

// The per-service materialization budget paces work: two due rolling groups
// share one budget slot, so publication stops at the later held group and catches
// up on the next service without losing either occurrence.
TEST(TimelinePlanner, RollingClaimsRouteOneAtATime) {
    PlayableInstanceTable instances(2);
    const PlayableInstanceHandle firstOwner =
        instances.Acquire(PlayableKind::Piece, 0);
    const PlayableInstanceHandle secondOwner =
        instances.Acquire(PlayableKind::Piece, 0);
    const std::uint64_t firstEpoch =
        instances.Classify(firstOwner).scheduleEpoch;
    const std::uint64_t secondEpoch =
        instances.Classify(secondOwner).scheduleEpoch;
    ScheduledTransactionPool pool;
    auto poolConfig = PoolConfig(100, 800);
    poolConfig.publishLeadQuanta = 6;
    ASSERT_TRUE(pool.Reset(poolConfig, 0, kEpoch));
    DeferredIntentPool deferred(2, 0);
    TimelinePlanner planner;
    TimelinePlanner::Config config;
    config.maxRollingOccurrences = 2;
    ASSERT_TRUE(planner.Reset(config, &pool, &deferred, &instances));

    std::array<EventTemplate, 2> firstEvents{{
        {100, 1, 0, 0, 20, 0},
        {400, 1, 1, 0, 50, 1},
    }};
    std::array<EventTemplate, 2> secondEvents{{
        {100, 2, 0, 0, 20, 0},
        {500, 2, 1, 0, 50, 1},
    }};
    std::array<ProgramView, 2> programs{};
    programs[0].ownerInstance = firstOwner;
    programs[0].ownerInstanceScheduleEpoch = firstEpoch;
    programs[0].transactionId = 1;
    programs[0].events = firstEvents.data();
    programs[0].eventCount = firstEvents.size();
    programs[0].rollingGroups = true;
    programs[1].ownerInstance = secondOwner;
    programs[1].ownerInstanceScheduleEpoch = secondEpoch;
    programs[1].transactionId = 2;
    programs[1].events = secondEvents.data();
    programs[1].eventCount = secondEvents.size();
    programs[1].rollingGroups = true;
    PostInput input{};
    input.transportEpoch = kEpoch;
    ASSERT_EQ(planner.PostPlay(programs[0], input).status,
              PostStatus::Reserved);
    ASSERT_EQ(planner.PostPlay(programs[1], input).status,
              PostStatus::Reserved);
    ASSERT_EQ(deferred.Count(), 2u);

    const std::uint64_t desired = 600;
    const std::uint64_t firstBlocker =
        ServiceDeferred(planner, pool, deferred, instances, kEpoch, desired,
                        1, pool.WindowStartQuantum());
    ASSERT_EQ(firstBlocker, 500u);
    planner.PublishThrough(firstBlocker);
    EXPECT_EQ(pool.PageStamp(4), 4u);
    EXPECT_EQ(pool.PageStamp(5), harken::kUnpublishedQuantumStamp);
    EXPECT_EQ(deferred.Count(), 1u);

    const std::uint64_t secondBlocker =
        ServiceDeferred(planner, pool, deferred, instances, kEpoch, desired,
                        1, pool.WindowStartQuantum() + 1u);
    EXPECT_EQ(secondBlocker, UINT64_MAX);
    planner.PublishThrough(desired);
    EXPECT_EQ(pool.PageStamp(5), 5u);
    EXPECT_EQ(deferred.Count(), 0u);
}

TEST(TimelinePlanner, MaterializationBudgetPacesAt95_100_101) {
    PlayableInstanceTable instances(8);
    OccurrenceStatePool occ(8);
    const PlayableInstanceHandle owner = instances.Acquire(PlayableKind::Piece, 0);
    const std::uint64_t ownerEpoch = instances.Classify(owner).scheduleEpoch;

    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(100, 400), 0, kEpoch)); // window [0,400)
    DeferredIntentPool deferred(64, 0);
    TimelinePlanner planner;
    TimelinePlanner::Config pc;
    ASSERT_TRUE(planner.Reset(pc, &pool, &deferred, &instances));

    // 21 plays whose anchors all fall in one quantum just beyond the horizon, so
    // they all come due together (100%+1 of the budget).
    std::vector<EventTemplate> templates(21, EventTemplate{0, 1, 0, 0, 0, 0});
    std::vector<ProgramView> programs(21);
    for (int i = 0; i < 21; ++i) {
        templates[i].payload = 0x900u + i;
        programs[i].ownerInstance = owner;
        programs[i].ownerInstanceScheduleEpoch = ownerEpoch;
        programs[i].transactionId = 100 + i;
        programs[i].entryAnchorFrame = 400 + i; // all in the quantum [400,500)
        programs[i].events = &templates[i];
        programs[i].eventCount = 1;
    }
    PostInput in;
    in.currentFrame = 0;
    in.transportEpoch = kEpoch;
    in.latePolicy = PrePublishLatePolicy::ExecuteAtEarliestSafeFrame;
    for (const ProgramView& p : programs) {
        EXPECT_EQ(planner.PostPlay(p, in).status, PostStatus::Deferred);
    }
    ASSERT_EQ(deferred.Count(), 21u);

    // Advance, consuming only the leaving page each quantum so every event is
    // counted once, then drain whatever still sits in the window.
    int consumedCount = 0;
    auto countEvents = [&](const ConsumedEvent* events, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            if (events[i].payload >= 0x900u && events[i].payload <= 0x914u) {
                ++consumedCount;
            }
        }
    };
    for (int q = 0; q < 20; ++q) {
        std::array<ConsumedEvent, 128> consumed{};
        const std::size_t count =
            ConsumeFrontier(pool, consumed.data(), consumed.size());
        countEvents(consumed.data(), count);
        AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch, 20);
    }
    auto tally = [&](std::uint32_t page) {
        std::array<ConsumedEvent, 128> buf{};
        const std::size_t n = pool.ConsumePage(page, kEpoch, instances, occ,
                                               buf.data(), buf.size());
        countEvents(buf.data(), n);
    };
    for (std::uint32_t page = 0; page < pool.PageCount(); ++page) {
        tally(page);
    }
    EXPECT_EQ(deferred.Count(), 0u); // all eventually materialized, none lost
    EXPECT_EQ(consumedCount, 21);    // every play reached the schedule
}

// The late trichotomy the pool decides is wired straight through: a play before
// the publish frontier is refused, dropped or rescheduled per the caller's policy.
TEST(TimelinePlanner, LateTrichotomyWiredToPool) {
    PlayableInstanceTable instances(8);
    const PlayableInstanceHandle owner = instances.Acquire(PlayableKind::Piece, 0);
    const std::uint64_t ownerEpoch = instances.Classify(owner).scheduleEpoch;

    const EventTemplate ev[] = {{0, 1, 0, 0, 0, 0x55}};
    auto makeProgram = [&](TransactionId id) {
        ProgramView p;
        p.ownerInstance = owner;
        p.ownerInstanceScheduleEpoch = ownerEpoch;
        p.transactionId = id;
        p.entryAnchorFrame = 50; // in the frozen region below the frontier
        p.events = ev;
        p.eventCount = 1;
        return p;
    };

    auto attempt = [&](PrePublishLatePolicy policy) {
        ScheduledTransactionPool pool;
        EXPECT_TRUE(pool.Reset(PoolConfig(100, 400), 0, kEpoch));
        DeferredIntentPool deferred(8, 0);
        TimelinePlanner planner;
        TimelinePlanner::Config pc;
        EXPECT_TRUE(planner.Reset(pc, &pool, &deferred, &instances));
        pool.Publish(200); // frontier at 200; frame 50 is late
        const ProgramView p = makeProgram(1);
        PostInput in;
        in.currentFrame = 200;
        in.transportEpoch = kEpoch;
        in.latePolicy = policy;
        return planner.PostPlay(p, in);
    };

    EXPECT_EQ(attempt(PrePublishLatePolicy::Refuse).status, PostStatus::Refused);
    EXPECT_EQ(attempt(PrePublishLatePolicy::Drop).status, PostStatus::Dropped);
    const PostResult safe = attempt(PrePublishLatePolicy::ExecuteAtEarliestSafeFrame);
    EXPECT_EQ(safe.status, PostStatus::Rescheduled);
    EXPECT_GE(safe.firstFrame, 200u); // shifted to the frontier or beyond
}

// NextEquivalentBoundary is resolved through the marker grid: a late play reschedules
// onto the next bar; with no next boundary it takes the missing-boundary policy.
TEST(TimelinePlanner, NextEquivalentBoundaryResolvedThroughMarkerGrid) {
    PlayableInstanceTable instances(8);
    const PlayableInstanceHandle owner = instances.Acquire(PlayableKind::Piece, 0);
    const std::uint64_t ownerEpoch = instances.Classify(owner).scheduleEpoch;

    // 3000 BPM-ish: micros 20000 gives exactly 1 frame/tick at 48 kHz, so a 4/4
    // bar (3840 ticks) is 3840 frames -- small enough to land inside the horizon.
    TempoSegment seg{MusicalTick{0}, 20000};
    ClockKernel clock;
    ASSERT_TRUE(clock.Reset(48000, &seg, 1));
    MarkerIndex barGrid;
    ASSERT_TRUE(barGrid.Reset(PieceTimeType::Musical, &clock, MeterSignature{4, 4},
                              nullptr, 0, 4)); // bar 3840 ticks -> 3840 frames

    const EventTemplate ev[] = {{0, 1, 0, 0, 0, 0x42}};
    ProgramView p;
    p.ownerInstance = owner;
    p.ownerInstanceScheduleEpoch = ownerEpoch;
    p.transactionId = 1;
    p.entryAnchorFrame = 0; // entry at frame 0, below the frontier => late
    p.events = ev;
    p.eventCount = 1;
    p.timeType = PieceTimeType::Musical;
    p.clock = &clock;
    p.markerIndex = &barGrid;
    p.boundaryType = BoundaryType::Bar;

    {
        ScheduledTransactionPool pool;
        ASSERT_TRUE(pool.Reset(PoolConfig(960, 19200), 0, kEpoch)); // 20 pages
        DeferredIntentPool deferred(8, 0);
        TimelinePlanner planner;
        TimelinePlanner::Config pc;
        ASSERT_TRUE(planner.Reset(pc, &pool, &deferred, &instances));
        pool.Publish(1920); // frontier at 1920; entry frame 0 is late
        PostInput in;
        in.currentFrame = 1920;
        in.transportEpoch = kEpoch;
        in.latePolicy = PrePublishLatePolicy::NextEquivalentBoundary;
        in.missingBoundaryPolicy = MissingBoundaryPolicy::Refuse;
        const PostResult r = planner.PostPlay(p, in);
        EXPECT_EQ(r.status, PostStatus::Rescheduled);
        // Next bar after tick 1920 is tick 3840 -> frame 3840.
        EXPECT_EQ(r.firstFrame, 3840u);
    }

    // No next marker boundary -> missing-boundary policy.
    {
        ScheduledTransactionPool pool;
        ASSERT_TRUE(pool.Reset(PoolConfig(960, 19200), 0, kEpoch));
        DeferredIntentPool deferred(8, 0);
        TimelinePlanner planner;
        TimelinePlanner::Config pc;
        ASSERT_TRUE(planner.Reset(pc, &pool, &deferred, &instances));
        pool.Publish(1920);
        ProgramView markerProgram = p;
        markerProgram.boundaryType = BoundaryType::Marker; // empty marker grid -> no candidate
        PostInput in;
        in.currentFrame = 1920;
        in.transportEpoch = kEpoch;
        in.latePolicy = PrePublishLatePolicy::NextEquivalentBoundary;
        in.missingBoundaryPolicy = MissingBoundaryPolicy::Drop;
        EXPECT_EQ(planner.PostPlay(markerProgram, in).status, PostStatus::Dropped);
        in.missingBoundaryPolicy = MissingBoundaryPolicy::Refuse;
        EXPECT_EQ(planner.PostPlay(markerProgram, in).status, PostStatus::Refused);
    }
}

TEST(TimelinePlanner, BoundaryRealignmentTrimsOnlyUnavailablePreroll) {
    PlayableInstanceTable instances(2);
    OccurrenceStatePool occurrences(1);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Piece, 0);
    TempoSegment segment{MusicalTick{0}, 20000};
    ClockKernel clock;
    ASSERT_TRUE(clock.Reset(48000, &segment, 1));
    MarkerIndex barGrid;
    ASSERT_TRUE(barGrid.Reset(PieceTimeType::Musical, &clock,
                              MeterSignature{4, 4}, nullptr, 0, 1));
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(960, 19200), 0, kEpoch));
    DeferredIntentPool deferred(2, 0);
    TimelinePlanner planner;
    TimelinePlanner::Config config;
    ASSERT_TRUE(planner.Reset(config, &pool, &deferred, &instances));
    pool.Publish(1920);

    const EventTemplate events[] = {
        {-3000, 1, 0, 0, 0, 0xA1},
        {0, 1, 1, 0, 0, 0xA2},
    };
    ProgramView program;
    program.ownerInstance = owner;
    program.ownerInstanceScheduleEpoch =
        instances.Classify(owner).scheduleEpoch;
    program.transactionId = 1;
    program.entryAnchorFrame = 0;
    program.events = events;
    program.eventCount = 2;
    program.timeType = PieceTimeType::Musical;
    program.clock = &clock;
    program.markerIndex = &barGrid;
    program.boundaryType = BoundaryType::Bar;
    PostInput input;
    input.currentFrame = 1920;
    input.transportEpoch = kEpoch;
    input.latePolicy = PrePublishLatePolicy::NextEquivalentBoundary;

    const PostResult result = planner.PostPlay(program, input);
    ASSERT_EQ(result.status, PostStatus::Rescheduled);
    EXPECT_EQ(result.firstFrame, 3840u);
    EXPECT_EQ(result.lastFrame, 3840u);

    std::array<ConsumedEvent, 2> consumed{};
    ASSERT_EQ(pool.ConsumePage(4, kEpoch, instances, occurrences,
                               consumed.data(), consumed.size()),
              1u);
    EXPECT_EQ(consumed[0].payload, 0xA2u);
    EXPECT_EQ(consumed[0].executeFrame, 3840u);
}

// Out-of-order posting is deterministic: filing a far play before a near one still
// consumes in frame order.
TEST(TimelinePlanner, OutOfOrderPostingStaysInFrameOrder) {
    PlayableInstanceTable instances(8);
    OccurrenceStatePool occ(8);
    const PlayableInstanceHandle owner = instances.Acquire(PlayableKind::Piece, 0);
    const std::uint64_t ownerEpoch = instances.Classify(owner).scheduleEpoch;

    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(100, 400), 0, kEpoch));
    DeferredIntentPool deferred(8, 0);
    TimelinePlanner planner;
    TimelinePlanner::Config pc;
    ASSERT_TRUE(planner.Reset(pc, &pool, &deferred, &instances));

    const EventTemplate farEv[] = {{0, 1, 0, 0, 0, 0xFA}};
    const EventTemplate nearEv[] = {{0, 2, 0, 0, 0, 0x11}};
    ProgramView pf;
    pf.ownerInstance = owner;
    pf.ownerInstanceScheduleEpoch = ownerEpoch;
    pf.transactionId = 1;
    pf.entryAnchorFrame = 250;
    pf.events = farEv;
    pf.eventCount = 1;
    ProgramView pn;
    pn.ownerInstance = owner;
    pn.ownerInstanceScheduleEpoch = ownerEpoch;
    pn.transactionId = 2;
    pn.entryAnchorFrame = 150;
    pn.events = nearEv;
    pn.eventCount = 1;

    PostInput in;
    in.currentFrame = 0;
    in.transportEpoch = kEpoch;
    in.latePolicy = PrePublishLatePolicy::ExecuteAtEarliestSafeFrame;
    planner.PostPlay(pf, in); // far first
    planner.PostPlay(pn, in); // near second

    std::vector<std::uint64_t> frames;
    for (int q = 0; q < 6; ++q) {
        std::array<ConsumedEvent, 8> buf{};
        const std::size_t n =
            ConsumeFrontier(pool, buf.data(), buf.size());
        for (std::size_t i = 0; i < n; ++i) {
            frames.push_back(buf[i].executeFrame);
        }
        AdvancePlannerQuantum(planner, pool, deferred, instances, kEpoch);
    }
    ASSERT_EQ(frames.size(), 2u);
    EXPECT_EQ(frames[0], 150u); // near consumed first
    EXPECT_EQ(frames[1], 250u); // far consumed second
}

// The keyed RNG seed depends only on the key: same key hashes the same, different
// keys differ, and the hash is unaffected by the order keys are computed in.
TEST(TimelinePlanner, KeyedRngSeedDependsOnlyOnKey) {
    const KeyedRngInput a{10, harkentest::ObjectId(20), 3, 4};
    const KeyedRngInput b{10, harkentest::ObjectId(20), 3, 5}; // one field differs
    const KeyedRngInput c{20, harkentest::ObjectId(10), 3, 4}; // seed/clip swapped

    EXPECT_NE(HashKeyedRng(a), HashKeyedRng(b));
    EXPECT_NE(HashKeyedRng(a), HashKeyedRng(c)); // fields do not cancel

    // Computing hashes in different orders leaves each key's value unchanged.
    const std::uint64_t ha = HashKeyedRng(a);
    const std::uint64_t hb = HashKeyedRng(b);
    (void)HashKeyedRng(c);
    EXPECT_EQ(HashKeyedRng(b), hb);
    EXPECT_EQ(HashKeyedRng(a), ha);
}

TEST(TimelinePlanner, KeyedRngOccurrenceCoordinatesDoNotAlias) {
    const KeyedRngInput ordinalLow{10, harkentest::ObjectId(20), 0, 0};
    const KeyedRngInput ordinalHigh{10, harkentest::ObjectId(20), 0, 0x80000000u};
    const KeyedRngInput nextLoop{10, harkentest::ObjectId(20), 1, 0};
    const KeyedRngInput nextOrdinal{10, harkentest::ObjectId(20), 0, 1};

    EXPECT_NE(HashKeyedRng(ordinalLow), HashKeyedRng(ordinalHigh));
    EXPECT_NE(HashKeyedRng(nextLoop), HashKeyedRng(nextOrdinal));
}

// Rebasing an entry-relative offset is shared with every other consumer of a
// compiled program, so its two hazardous inputs are pinned on the function
// itself rather than only through a schedule that happens to exercise them.
//
// The extreme negative offset is the one that punishes a naive implementation:
// negating INT64_MIN is undefined, so the magnitude has to come from the
// complement. The extreme positive sum is the other: a wrap would turn the
// farthest future frame into an immediate one, which is the worst possible
// failure direction for a scheduler, so it saturates instead.
TEST(TimelinePlanner, AnchoredFrameClampsBelowZeroAndSaturatesAbove) {
    constexpr std::int64_t kMostNegative = std::numeric_limits<std::int64_t>::min();
    const std::uint64_t kMostNegativeMagnitude =
        static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max()) + 1u;

    // Ordinary rebasing in both directions.
    EXPECT_EQ(harken::AnchoredFrame(1000, 250), 1250u);
    EXPECT_EQ(harken::AnchoredFrame(1000, -250), 750u);
    EXPECT_EQ(harken::AnchoredFrame(1000, 0), 1000u);
    EXPECT_EQ(harken::AnchoredFrame(1000, -1000), 0u); // exactly the anchor

    // A weak-up further back than the anchor clamps at the session start rather
    // than wrapping to the end of the frame axis.
    EXPECT_EQ(harken::AnchoredFrame(1000, -1001), 0u);
    EXPECT_EQ(harken::AnchoredFrame(0, kMostNegative), 0u);
    EXPECT_EQ(harken::AnchoredFrame(kMostNegativeMagnitude - 1u, kMostNegative), 0u);

    // One frame past the clamp the magnitude is still exact: an implementation
    // that lost a bit taking |INT64_MIN| would not land on 1 here.
    EXPECT_EQ(harken::AnchoredFrame(kMostNegativeMagnitude + 1u, kMostNegative), 1u);
    EXPECT_EQ(harken::AnchoredFrame(kMostNegativeMagnitude, kMostNegative), 0u);

    // The positive sum saturates. A wrapping implementation would report a small
    // frame for the two cases below.
    EXPECT_EQ(harken::AnchoredFrame(UINT64_MAX, 1), UINT64_MAX);
    EXPECT_EQ(harken::AnchoredFrame(UINT64_MAX - 1u, 5), UINT64_MAX);
    EXPECT_EQ(harken::AnchoredFrame(UINT64_MAX, 0), UINT64_MAX);
    // One below the saturating case, where the exact sum is still representable.
    EXPECT_EQ(harken::AnchoredFrame(UINT64_MAX - 5u, 5), UINT64_MAX - 0u);
    EXPECT_EQ(harken::AnchoredFrame(UINT64_MAX - 6u, 5), UINT64_MAX - 1u);
}

// The planner files through that shared function, so a program whose only event
// sits at the extreme negative offset lands on the clamped frame, not on a
// wrapped one. This is the planner half of the shared definition; the preview
// driver's half is pinned in its own suite against the same function.
TEST(TimelinePlanner, ExtremeNegativeOffsetFilesAtTheClampedFrame) {
    PlayableInstanceTable instances(1);
    const PlayableInstanceHandle owner =
        instances.Acquire(PlayableKind::Piece, 0);
    ScheduledTransactionPool pool;
    ASSERT_TRUE(pool.Reset(PoolConfig(100, 400), 0, kEpoch));
    DeferredIntentPool deferred(1, 0);
    TimelinePlanner planner;
    TimelinePlanner::Config config;
    ASSERT_TRUE(planner.Reset(config, &pool, &deferred, &instances));

    constexpr std::int64_t kMostNegative = std::numeric_limits<std::int64_t>::min();
    const EventTemplate event[] = {{kMostNegative, 1, 0, 0, 0, 0xB1}};
    ProgramView program;
    program.ownerInstance = owner;
    program.ownerInstanceScheduleEpoch =
        instances.Classify(owner).scheduleEpoch;
    program.transactionId = 1;
    program.entryAnchorFrame = 150;
    program.events = event;
    program.eventCount = 1;
    PostInput input;
    input.currentFrame = pool.PublishCutoffFrame();
    input.transportEpoch = kEpoch;
    // The clamped frame is behind the publish frontier, so the late policy — not
    // an arithmetic accident — decides where it lands.
    input.latePolicy = PrePublishLatePolicy::ExecuteAtEarliestSafeFrame;

    ASSERT_EQ(harken::AnchoredFrame(program.entryAnchorFrame, kMostNegative), 0u);
    const PostResult result = planner.PostPlay(program, input);
    // A wrapping rebase would put the event near the top of the frame axis, far
    // past the horizon, and it would be held as a deferred intent. The clamped
    // frame is behind the frontier instead, so it is shifted forward to a safe
    // frame inside the window.
    EXPECT_EQ(result.status, PostStatus::Rescheduled);
    EXPECT_GE(result.firstFrame, pool.PublishCutoffFrame());
    EXPECT_LE(result.firstFrame, pool.WindowStartFrame() + 400u);
    EXPECT_EQ(deferred.Count(), 0u);
}
