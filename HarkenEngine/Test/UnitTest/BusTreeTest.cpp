// Bus tree behavior: creation validation (invalid id, duplicates, unknown
// parent, capacity), volume argument validation, and per-voice routing to a
// created bus with unknown bus ids refused.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/dsp/GainRamp.h"
#include "harken/param/BusTargetStore.h"
#include "harken/param/ParamTargetTable.h"

#include <cmath>
#include <limits>
#include <tuple>
#include <utility>

namespace {

using harkentest::Capture;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MakePlayDesc;
using harkentest::MonoDesc;

constexpr std::uint32_t kBlockFrames = 256;

auto RetirementState(const harken::BusTree& buses, std::uint32_t slot) {
    return std::make_tuple(buses.IdAt(slot), buses.GenerationAt(slot), buses.AliveAt(slot),
        buses.ActiveBusCount(), buses.FreeBusSlotCount(), buses.QuarantinedBusSlotCount(),
        buses.RetirementRevisionAt(slot), buses.SlotReuseCount(), buses.BusCount(),
        buses.InstalledGraphRevision());
}

void ExpectRetirementCapacity(const harken::BusTree& buses, std::uint32_t capacity) {
    EXPECT_EQ(capacity, buses.ActiveBusCount() + buses.FreeBusSlotCount() +
                        buses.QuarantinedBusSlotCount());
}

static_assert(noexcept(std::declval<harken::BusTree&>().HoldSlotReuse({}, 1)));
static_assert(noexcept(std::declval<harken::BusTree&>().HoldRetiredSlot({}, 1)));
static_assert(noexcept(std::declval<harken::BusTree&>().ReleaseRetiredSlot({}, 1)));

TEST(BusTree, RetiredSlotNeedsExactFutureFenceBeforeReuse) {
    auto config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));
    auto& buses = session.Engine()->buses;
    const auto old = buses.FindRef(harkentest::ObjectId(10));
    ASSERT_TRUE(old.Valid());
    buses.AcknowledgeInstalledGraphRevision(5);
    ASSERT_TRUE(buses.HoldRetiredSlot(old, 10));
    EXPECT_FALSE(buses.AliveAt(old.index));
    EXPECT_EQ(10u, buses.RetirementRevisionAt(old.index));
    EXPECT_EQ(0u, buses.NextGenerationForSlot(old.index));
    EXPECT_EQ(0u, buses.FreeBusSlotCount());
    EXPECT_EQ(1u, buses.QuarantinedBusSlotCount());
    ExpectRetirementCapacity(buses, 2);
    const auto held = RetirementState(buses, old.index);
    EXPECT_EQ(VN_NO_BUS, buses.CreateBus(harkentest::ObjectId(11), VN_BUS_MASTER));
    EXPECT_EQ(held, RetirementState(buses, old.index));
    EXPECT_FALSE(buses.ReleaseRetiredSlot(old, 10));
    EXPECT_EQ(held, RetirementState(buses, old.index));
    buses.AcknowledgeInstalledGraphRevision(9);
    EXPECT_FALSE(buses.ReleaseRetiredSlot(old, 10));
    buses.AcknowledgeInstalledGraphRevision(10);
    ASSERT_TRUE(buses.ReleaseRetiredSlot(old, 10));
    EXPECT_EQ(1u, buses.FreeBusSlotCount());
    ExpectRetirementCapacity(buses, 2);
    ASSERT_EQ(VN_OK, buses.CreateBus(harkentest::ObjectId(11), VN_BUS_MASTER));
    const auto current = buses.FindRef(harkentest::ObjectId(11));
    EXPECT_EQ(old.index, current.index);
    EXPECT_EQ(old.generation + 1, current.generation);
    const auto reused = RetirementState(buses, current.index);
    EXPECT_FALSE(buses.HoldSlotReuse(old, 20));
    EXPECT_FALSE(buses.HoldRetiredSlot(old, 20));
    EXPECT_FALSE(buses.ReleaseRetiredSlot(old, 10));
    EXPECT_EQ(reused, RetirementState(buses, current.index));
    ExpectRetirementCapacity(buses, 2);
}

TEST(BusTree, AliveRetirementHoldKeepsMaximumFutureRevisionAndCounts) {
    auto config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 3;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));
    auto& buses = session.Engine()->buses;
    const auto ref = buses.FindRef(harkentest::ObjectId(10));
    buses.AcknowledgeInstalledGraphRevision(5);
    ASSERT_TRUE(buses.HoldSlotReuse(ref, 10));
    ASSERT_TRUE(buses.HoldSlotReuse(ref, 20));
    ASSERT_TRUE(buses.HoldSlotReuse(ref, 8));
    EXPECT_EQ(20u, buses.RetirementRevisionAt(ref.index));
    EXPECT_TRUE(buses.AliveAt(ref.index));
    EXPECT_EQ(2u, buses.ActiveBusCount());
    EXPECT_EQ(0u, buses.QuarantinedBusSlotCount());
    ExpectRetirementCapacity(buses, 3);
    buses.AcknowledgeInstalledGraphRevision(10);
    const auto before = RetirementState(buses, ref.index);
    EXPECT_FALSE(buses.HoldSlotReuse(ref, 8));
    EXPECT_FALSE(buses.HoldSlotReuse(ref, 10));
    EXPECT_FALSE(buses.ReleaseRetiredSlot(ref, 10));
    EXPECT_FALSE(buses.ReleaseRetiredSlot({ref.index, ref.generation + 1}, 20));
    EXPECT_EQ(before, RetirementState(buses, ref.index));
    buses.AcknowledgeInstalledGraphRevision(19);
    EXPECT_FALSE(buses.ReleaseRetiredSlot(ref, 20));
    buses.AcknowledgeInstalledGraphRevision(20);
    ASSERT_TRUE(buses.ReleaseRetiredSlot(ref, 20));
    EXPECT_TRUE(buses.AliveAt(ref.index));
    ExpectRetirementCapacity(buses, 3);
    const auto master = buses.FindRef(VN_BUS_MASTER);
    ASSERT_TRUE(buses.HoldSlotReuse(master, 30));
    const auto masterBefore = RetirementState(buses, master.index);
    EXPECT_FALSE(buses.HoldRetiredSlot(master, 31));
    EXPECT_EQ(masterBefore, RetirementState(buses, master.index));
    ExpectRetirementCapacity(buses, 3);
    buses.AcknowledgeInstalledGraphRevision(30);
    EXPECT_TRUE(buses.ReleaseRetiredSlot(master, 30));
}

TEST(BusTree, InvalidOrAlreadyInstalledRetirementRequestsNeverMutateState) {
    auto config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 3;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));
    auto& buses = session.Engine()->buses;
    const auto ref = buses.FindRef(harkentest::ObjectId(10));
    buses.AcknowledgeInstalledGraphRevision(100);
    for (bool dead : {false, true}) {
        if (dead) buses.MarkLogicDead(ref.index);
        const auto before = RetirementState(buses, ref.index);
        for (std::uint64_t revision : {0u, 50u, 100u}) {
            EXPECT_FALSE(buses.HoldSlotReuse(ref, revision));
            EXPECT_EQ(before, RetirementState(buses, ref.index));
            EXPECT_FALSE(buses.HoldRetiredSlot(ref, revision));
            EXPECT_EQ(before, RetirementState(buses, ref.index));
            EXPECT_FALSE(buses.ReleaseRetiredSlot(ref, revision));
            EXPECT_EQ(before, RetirementState(buses, ref.index));
        }
        const harken::BusSlotRef invalid[]{{}, {3, ref.generation},
            {ref.index, 0}, {ref.index, ref.generation + 1}};
        for (auto bad : invalid) {
            EXPECT_FALSE(buses.HoldSlotReuse(bad, 101));
            EXPECT_FALSE(buses.HoldRetiredSlot(bad, 101));
            EXPECT_FALSE(buses.ReleaseRetiredSlot(bad, 101));
            EXPECT_EQ(before, RetirementState(buses, ref.index));
        }
        const auto master = buses.FindRef(VN_BUS_MASTER);
        const auto masterBefore = RetirementState(buses, master.index);
        EXPECT_FALSE(buses.HoldRetiredSlot(master, 101));
        EXPECT_EQ(masterBefore, RetirementState(buses, master.index));
        EXPECT_EQ(0u, buses.RetirementRevisionAt(UINT32_MAX));
        ExpectRetirementCapacity(buses, 3);
    }
}

TEST(BusTree, HeldSlotCannotBypassAllocatorThroughDirectCommit) {
    auto config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));
    auto& buses = session.Engine()->buses;
    const auto ref = buses.FindRef(harkentest::ObjectId(10));
    ASSERT_TRUE(buses.HoldRetiredSlot(ref, 10));
    buses.AcknowledgeInstalledGraphRevision(10);
    const auto before = RetirementState(buses, ref.index);
#if defined(NDEBUG)
    buses.CommitLogicSlot(ref.index, harkentest::ObjectId(11), 0, ref.generation + 1);
#else
    EXPECT_DEATH(buses.CommitLogicSlot(ref.index, harkentest::ObjectId(11), 0, ref.generation + 1), "");
#endif
    EXPECT_EQ(before, RetirementState(buses, ref.index));
    ASSERT_TRUE(buses.ReleaseRetiredSlot(ref, 10));
    buses.CommitLogicSlot(ref.index, harkentest::ObjectId(11), 0, ref.generation + 1);
    EXPECT_EQ(harkentest::ObjectId(11u), buses.IdAt(ref.index));
    EXPECT_EQ(ref.generation + 1, buses.GenerationAt(ref.index));
    EXPECT_EQ(0u, buses.RetirementRevisionAt(ref.index));
    EXPECT_TRUE(buses.AliveAt(ref.index));
    ExpectRetirementCapacity(buses, 2);
}

TEST(BusTree, TemporaryHoldReleaseNeverClearsPermanentGenerationQuarantine) {
    auto config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));
    auto& buses = session.Engine()->buses;
    const auto original = buses.FindRef(harkentest::ObjectId(10));
    buses.MarkLogicDead(original.index);
    buses.CommitLogicSlot(original.index, harkentest::ObjectId(10), 0, UINT32_MAX);
    const auto last = buses.FindRef(harkentest::ObjectId(10));
    buses.MarkLogicDead(last.index);
    EXPECT_EQ(VN_NO_BUS, buses.CreateBus(harkentest::ObjectId(11), VN_BUS_MASTER));
    EXPECT_TRUE(buses.QuarantinedAt(last.index));
    ExpectRetirementCapacity(buses, 2);
    buses.AcknowledgeInstalledGraphRevision(5);
    ASSERT_TRUE(buses.HoldSlotReuse(last, 10));
    EXPECT_EQ(1u, buses.QuarantinedBusSlotCount());
    ExpectRetirementCapacity(buses, 2);
    buses.AcknowledgeInstalledGraphRevision(10);
    ASSERT_TRUE(buses.ReleaseRetiredSlot(last, 10));
    EXPECT_TRUE(buses.QuarantinedAt(last.index));
    EXPECT_EQ(0u, buses.RetirementRevisionAt(last.index));
    EXPECT_EQ(0u, buses.NextGenerationForSlot(last.index));
    const auto before = RetirementState(buses, last.index);
#if defined(NDEBUG)
    buses.CommitLogicSlot(last.index, harkentest::ObjectId(11), 0, 1);
#else
    EXPECT_DEATH(buses.CommitLogicSlot(last.index, harkentest::ObjectId(11), 0, 1), "");
#endif
    EXPECT_EQ(before, RetirementState(buses, last.index));
    EXPECT_EQ(VN_NO_BUS, buses.CreateBus(harkentest::ObjectId(11), VN_BUS_MASTER));
    EXPECT_EQ(before, RetirementState(buses, last.index));
    ExpectRetirementCapacity(buses, 2);
}

TEST(BusTree, CreateBusRequiresInit) {
    EXPECT_EQ(VN_NOT_INITIALIZED, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));
}

TEST(BusTree, CreateBusValidationAndCapacity) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 3; // master plus two
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    // Id zero stays out of the id space.
    EXPECT_EQ(VN_INVALID_ARG, Vn_CreateBus(harkentest::ObjectId(0), VN_BUS_MASTER));

    // The master id is already taken, like any duplicate.
    EXPECT_EQ(VN_IN_USE, Vn_CreateBus(VN_BUS_MASTER, VN_BUS_MASTER));

    // The parent must exist before its children.
    EXPECT_EQ(VN_UNKNOWN_ID, Vn_CreateBus(harkentest::ObjectId(10), harkentest::ObjectId(999)));

    EXPECT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));
    EXPECT_EQ(VN_IN_USE, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));

    // Nesting under a created bus works; the table is now full.
    EXPECT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(11), harkentest::ObjectId(10)));
    EXPECT_EQ(VN_NO_BUS, Vn_CreateBus(harkentest::ObjectId(12), VN_BUS_MASTER));

    // Refusals left the tree usable: the created buses still resolve.
    EXPECT_EQ(3u, session.Engine()->buses.BusCount());
}

TEST(BusTree, PreparedBusStaysHiddenUntilVisibilityCommit) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 3;
    config.fxSlotsPerBus = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    harken::BusTree& buses = session.Engine()->buses;
    const harken::BusTree::PreparedBusSlot prepared{harkentest::ObjectId(10), 0};
    ASSERT_TRUE(buses.CanMaterializePreparedBusSlots(1, 1));

    buses.MaterializePreparedBusSlots(1, &prepared, 1);

    EXPECT_EQ(1u, buses.BusCount());
    EXPECT_EQ(harken::kInvalidBusIndex, buses.FindIndex(harkentest::ObjectId(10)));
    EXPECT_EQ(nullptr, buses.BusState(1));

    buses.CommitPreparedBusVisibility(1, 1);

    EXPECT_EQ(2u, buses.BusCount());
    EXPECT_EQ(1u, buses.FindIndex(harkentest::ObjectId(10)));
    EXPECT_TRUE(buses.AliveAt(1));
    EXPECT_EQ(0u, buses.ParentOf(1));
    ASSERT_NE(nullptr, buses.BusState(1));
    EXPECT_EQ(harkentest::ObjectId(10u), buses.BusState(1)->id);
    EXPECT_EQ(0u, buses.BusState(1)->parentIndex);
}

TEST(BusTree, PreparedBusBatchMatchesOrdinaryCreationAndSupportsPlanApply) {
    VnConfig config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 5;
    config.fxSlotsPerBus = 3;
    config.busSendSlotsPerBus = 2;
    config.topologyPlanPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());

    harken::Engine* engine = session.Engine();
    harken::BusTree& buses = engine->buses;
    EXPECT_FALSE(buses.CanMaterializePreparedBusSlots(2, 2));
    EXPECT_FALSE(buses.CanMaterializePreparedBusSlots(1, 0));
    EXPECT_FALSE(buses.CanMaterializePreparedBusSlots(1, 5));

    const harken::BusTree::PreparedBusSlot prepared[] = {
        {harkentest::ObjectId(10), 0},
        {harkentest::ObjectId(11), 1},
    };
    ASSERT_TRUE(buses.CanMaterializePreparedBusSlots(1, 2));
    buses.MaterializePreparedBusSlots(1, prepared, 2);

    EXPECT_EQ(1u, buses.BusCount());
    EXPECT_EQ(1u, buses.AudioBusCount());
    EXPECT_EQ(harken::kInvalidBusIndex, buses.FindIndex(harkentest::ObjectId(10)));
    EXPECT_EQ(harken::kInvalidBusIndex, buses.FindIndex(harkentest::ObjectId(11)));
    EXPECT_FALSE(buses.CanMaterializePreparedBusSlots(1, 1));

    // Effect commands may precede the final plan swap in one published batch.
    // The hidden slot must therefore already carry a valid id and full chain
    // state even though logic lookup still cannot reach it.
    buses.ApplySetBusEffectBypass(2, 0, 1);

    const std::uint32_t plan = engine->topologyPlans.Acquire();
    ASSERT_NE(harken::kInvalidTopologyPlanIndex, plan);
    std::uint32_t* order = engine->topologyPlans.OrderData(plan);
    order[0] = 2;
    order[1] = 1;
    order[2] = 0;
    engine->topologyPlans.SetOrderCount(plan, 3);
    engine->topologyPlans.SetSlotCount(plan, 3);
    std::uint32_t* parents = engine->topologyPlans.ParentsData(plan);
    parents[0] = 0;
    parents[1] = 0;
    parents[2] = 1;
    std::uint32_t* outputLayouts =
        engine->topologyPlans.OutputLayoutData(plan);
    float* downmix = engine->topologyPlans.DownmixCoeffsData(plan);
    const std::uint32_t defaultLayout = buses.BusOutputLayout(0);
    const float* defaultDownmix = buses.BusDownmixCoeffs(0);
    ASSERT_NE(nullptr, defaultDownmix);
    for (std::uint32_t bus = 0; bus < 3; ++bus) {
        outputLayouts[bus] = defaultLayout;
        for (std::uint32_t coefficient = 0; coefficient < 64;
             ++coefficient) {
            downmix[static_cast<std::size_t>(bus) * 64 + coefficient] =
                defaultDownmix[coefficient];
        }
    }

    buses.ApplyTopologyPlan(plan, harken::kInvalidBusIndex,
                            harken::kInvalidBusIndex, 0.0f);

    EXPECT_EQ(3u, buses.AudioBusCount());
    EXPECT_EQ(1u, buses.BusCount());
    EXPECT_EQ(config.fxSlotsPerBus, buses.BusState(0)->faderAfterSlot);
    EXPECT_EQ(buses.BusState(0)->outputLayout, buses.BusOutputLayout(1));
    ASSERT_NE(nullptr, buses.BusDownmixCoeffs(1));

    buses.CommitPreparedBusVisibility(1, 2);

    EXPECT_EQ(3u, buses.BusCount());
    EXPECT_EQ(1u, buses.FindIndex(harkentest::ObjectId(10)));
    EXPECT_EQ(2u, buses.FindIndex(harkentest::ObjectId(11)));
    EXPECT_EQ(0u, buses.ParentOf(1));
    EXPECT_EQ(1u, buses.ParentOf(2));
    EXPECT_TRUE(buses.AliveAt(1));
    EXPECT_TRUE(buses.AliveAt(2));
    ASSERT_NE(nullptr, buses.BusState(2));
    EXPECT_EQ(1.0f, buses.BusState(2)->fxBypassMix[0]);
    EXPECT_EQ(1.0f, buses.BusState(2)->fxBypassTarget[0]);
    EXPECT_FALSE(buses.BusState(2)->fxBypassRamping[0]);

    ASSERT_EQ(VN_OK, buses.CreateBus(harkentest::ObjectId(12), VN_BUS_MASTER));
    const harken::BusTree::Bus* materialized = buses.BusState(1);
    const harken::BusTree::Bus* ordinary = buses.BusState(3);
    ASSERT_NE(nullptr, materialized);
    ASSERT_NE(nullptr, ordinary);
    EXPECT_EQ(ordinary->gain.current, materialized->gain.current);
    EXPECT_EQ(ordinary->gain.target, materialized->gain.target);
    EXPECT_EQ(ordinary->gain.remainingFrames, materialized->gain.remainingFrames);
    EXPECT_EQ(ordinary->gainStarted, materialized->gainStarted);
    EXPECT_EQ(ordinary->preFaderPeak, materialized->preFaderPeak);
    EXPECT_EQ(ordinary->postFaderPeak, materialized->postFaderPeak);
    EXPECT_EQ(ordinary->outputPeak, materialized->outputPeak);
    EXPECT_EQ(ordinary->faderAfterSlot, materialized->faderAfterSlot);
    EXPECT_EQ(ordinary->outputLayout, materialized->outputLayout);
    for (std::uint32_t slot = 0; slot < harken::kMaxFxSlotsPerBus; ++slot) {
        EXPECT_EQ(nullptr, materialized->fxSlots[slot].process);
        EXPECT_EQ(nullptr, materialized->fxSlots[slot].processDynamic);
        EXPECT_EQ(nullptr, materialized->fxSlots[slot].tailFrames);
        EXPECT_EQ(nullptr, materialized->fxSlots[slot].setParam);
        EXPECT_EQ(nullptr, materialized->fxSlots[slot].reset);
        EXPECT_EQ(nullptr, materialized->fxSlots[slot].state);
        EXPECT_EQ(0.0f, materialized->fxBypassMix[slot]);
        EXPECT_EQ(0.0f, materialized->fxBypassTarget[slot]);
        EXPECT_FALSE(materialized->fxBypassRamping[slot]);
        EXPECT_EQ(harken::kEffectTypeNone,
                  materialized->fxSlotEffectRef[slot].type);
        EXPECT_EQ(0u, materialized->fxSlotEffectRef[slot].poolIndex);
        EXPECT_EQ(0u, materialized->fxSlotEffectRef[slot].generation);
    }
    for (std::uint32_t coefficient = 0; coefficient < 64; ++coefficient) {
        EXPECT_EQ(ordinary->downmixCoeffs[coefficient],
                  materialized->downmixCoeffs[coefficient]);
    }
}

TEST(BusTree, SetBusVolumeValidatesArguments) {
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));

    EXPECT_EQ(VN_UNKNOWN_ID, Vn_SetBusVolume(harkentest::ObjectId(999), -3.0f, 0));

    // Non-finite dB would poison the linear conversion at apply time.
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetBusVolume(harkentest::ObjectId(10), std::numeric_limits<float>::quiet_NaN(),
                              0));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetBusVolume(harkentest::ObjectId(10), std::numeric_limits<float>::infinity(),
                              0));

    // The master accepts volume changes like any other bus.
    EXPECT_EQ(VN_OK, Vn_SetBusVolume(VN_BUS_MASTER, -3.0f, 0));
    EXPECT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(10), -3.0f, 10));

    harken::Engine* engine = session.Engine();
    const std::uint32_t busIndex = engine->buses.FindIndex(harkentest::ObjectId(10));
    ASSERT_NE(harken::kInvalidBusIndex, busIndex);
    const float lastCommitted =
        engine->busTargets.LastCommitted(busIndex, harken::kBusTargetBusVolume);
    const float resolvedBefore =
        engine->busTargets.Reduce(busIndex, harken::kBusTargetBusVolume);
    const std::uint64_t commandHead = engine->commands.PublishedHead();
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetBusVolume(harkentest::ObjectId(10), 800.0f, 0));
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetBusOutputVolume(harkentest::ObjectId(10), 800.0f, 0));
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_FLOAT_EQ(resolvedBefore, engine->busTargets.Reduce(
                                        busIndex, harken::kBusTargetBusVolume));
    EXPECT_FLOAT_EQ(lastCommitted, engine->busTargets.LastCommitted(
                                       busIndex, harken::kBusTargetBusVolume));
    EXPECT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(10), 600.0f, 0));
    EXPECT_FLOAT_EQ(600.0f, engine->busTargets.Reduce(
                                busIndex, harken::kBusTargetBusVolume));
    EXPECT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(10), -800.0f, 0));
    EXPECT_FLOAT_EQ(-800.0f, engine->busTargets.Reduce(
                                 busIndex, harken::kBusTargetBusVolume));
}

TEST(BusTree, CombinedFiniteBusVolumeContributionsThatOverflowAreRefused) {
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));
    harken::Engine* engine = session.Engine();
    const std::uint32_t busIndex = engine->buses.FindIndex(harkentest::ObjectId(10));
    ASSERT_NE(harken::kInvalidBusIndex, busIndex);
    engine->busTargets.SetSlot(busIndex, harken::kBusTargetBusVolume,
                               harken::BusTargetStore::kSlotAuthoredIndex,
                               400.0f);
    const std::uint64_t commandHead = engine->commands.PublishedHead();
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetBusVolume(harkentest::ObjectId(10), 400.0f, 0));
    EXPECT_EQ(commandHead, engine->commands.PublishedHead());
    EXPECT_FALSE(engine->busTargets.SlotActive(
        busIndex, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotApiDirectIndex));
    EXPECT_FLOAT_EQ(400.0f, engine->busTargets.Reduce(
                                busIndex, harken::kBusTargetBusVolume));
}

TEST(BusTree, MutedBusStillRejectsUnconvertibleResolvedVolume) {
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusMute(harkentest::ObjectId(10), 1, 0));
    harken::Engine* engine = session.Engine();
    const std::uint32_t busIndex = engine->buses.FindIndex(harkentest::ObjectId(10));
    ASSERT_NE(harken::kInvalidBusIndex, busIndex);
    EXPECT_EQ(1u, engine->busMuted[busIndex]);
    EXPECT_EQ(VN_INVALID_ARG, Vn_SetBusVolume(harkentest::ObjectId(10), 800.0f, 0));
    EXPECT_EQ(1u, engine->busMuted[busIndex]);
    EXPECT_FALSE(engine->busTargets.SlotActive(
        busIndex, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotApiDirectIndex));
    const std::uint64_t invalidBefore =
        engine->buses.InvalidVolumeApplyCount();
    engine->buses.ApplyVolume(0, 800.0f, 0);
    EXPECT_EQ(invalidBefore + 1u, engine->buses.InvalidVolumeApplyCount());
}

TEST(BusTree, FiniteOverflowingBindingVolumeIsDroppedNotRetried) {
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    harken::Engine* engine = session.Engine();
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(engine, {harkentest::ObjectId(7)}, 0.0f, 1.0f)
                  .status);
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(harkentest::ObjectId(10), -3.0f, 0));
    engine->nullBackend.Pump(1);
    const std::uint32_t busIndex = engine->buses.FindIndex(harkentest::ObjectId(10));
    ASSERT_NE(harken::kInvalidBusIndex, busIndex);
    harken::BusTree::Bus* bus = engine->buses.BusState(busIndex);
    ASSERT_NE(nullptr, bus);
    const float oldGain = bus->gain.target;
    EXPECT_NEAR(std::pow(10.0f, -3.0f / 20.0f), oldGain, 1e-5f);
    VnBindingDesc desc{};
    desc.structSize = sizeof(VnBindingDesc);
    desc.variableId = harkentest::ObjectId(7);
    desc.targetScopeKind = VN_TARGET_SCOPE_BUS;
    desc.targetId = harken::kBusTargetBusVolume;
    desc.curve.structSize = sizeof(VnCurveDesc);
    desc.curve.pointCount = 2;
    desc.curve.points[0] = {0.0f, 0.0f};
    desc.curve.points[1] = {1.0f, 0.0f};
    desc.curve.handles[0] = harkentest::EndpointHandles(
        desc.curve.points[0], desc.curve.points[1]);
    ASSERT_EQ(VN_OK, Vn_RegisterBinding(harkentest::ObjectId(1), &desc));
    VnBindingAttachDesc attach{};
    attach.structSize = sizeof(VnBindingAttachDesc);
    attach.scopeKind = VN_TARGET_SCOPE_BUS;
    attach.objectId = harkentest::ObjectId(10);
    ASSERT_EQ(VN_OK, Vn_AttachBinding(harkentest::ObjectId(1), &attach));
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(7), 1.0f));
    harkentest::AdvanceVariableWrite(engine);
    engine->nullBackend.Pump(1);
    EXPECT_NEAR(oldGain, bus->gain.target, 1e-5f);

    // Ordinary dirty: the Variable is already Applied. Replacing the curve
    // re-evaluates at Vn_Update and must drop through flushBuses, not the
    // Variable WriteRow command builder.
    desc.curve.points[0] = {0.0f, 800.0f};
    desc.curve.points[1] = {1.0f, 800.0f};
    desc.curve.handles[0] = harkentest::EndpointHandles(
        desc.curve.points[0], desc.curve.points[1]);
    ASSERT_EQ(VN_OK, Vn_ReplaceBinding(harkentest::ObjectId(1), &desc));
    const float lastCommitted =
        engine->busTargets.LastCommitted(busIndex, harken::kBusTargetBusVolume);
    const std::uint64_t dropsBefore = engine->paramDropCount;
    const std::uint64_t invalidBefore =
        engine->buses.InvalidVolumeApplyCount();

    ASSERT_EQ(VN_OK, Vn_Update());
    const float resolved =
        engine->busTargets.Reduce(busIndex, harken::kBusTargetBusVolume);
    EXPECT_TRUE(std::isfinite(resolved));
    EXPECT_FALSE(harken::TryVolumeGainFromDb(resolved));
    EXPECT_EQ(dropsBefore + 1u, engine->paramDropCount);
    EXPECT_FLOAT_EQ(lastCommitted, engine->busTargets.LastCommitted(
                                       busIndex, harken::kBusTargetBusVolume));
    EXPECT_EQ(oldGain, bus->gain.target);
    EXPECT_EQ(invalidBefore, engine->buses.InvalidVolumeApplyCount())
        << "the drop must not Stage and rely on the audio apply defense";

    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    EXPECT_EQ(dropsBefore + 1u, engine->paramDropCount);
    EXPECT_EQ(oldGain, bus->gain.target);
    EXPECT_EQ(invalidBefore, engine->buses.InvalidVolumeApplyCount());
}

TEST(BusTree, VoiceRoutesToCreatedBusAndUnknownBusRefused) {
    EngineSession session(MakeNullConfig(2, kBlockFrames));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));

    const std::vector<float> dc = harkentest::MakeDC(kBlockFrames, 0.5f);
    HARKENTEST_ASSERT_NON_SILENT(dc);
    const VnPCMDesc desc = MonoDesc(dc, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));

    // A bus id that was never created is refused up front.
    VnPlayDesc play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = harkentest::ObjectId(42);
    VnVoiceHandle handle = 0;
    EXPECT_EQ(VN_UNKNOWN_ID, harkentest::PlayRaw(&play, &handle));

    // Routing through the created bus reaches the master at unity: the
    // routed voice is audible and numerically identical to the source.
    play.busId = harkentest::ObjectId(10);
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &handle));

    Capture out;
    harkentest::PumpAndCapture(session.Engine(), 1, out);
    HARKENTEST_ASSERT_NON_SILENT(out.left);
    for (std::size_t i = 0; i < kBlockFrames; ++i) {
        ASSERT_EQ(0.5f, out.left[i]) << "sample " << i;
    }
}

} // namespace
