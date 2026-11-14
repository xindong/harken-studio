// Bus tree behavior: creation validation (invalid id, duplicates, unknown
// parent, capacity), volume argument validation, and per-voice routing to a
// created bus with unknown bus ids refused.
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"
#include "HarkenApiInternal.h"
#include "SignalPublication.h"
#include "harken/render/BusActivityPlan.h"

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


TEST(BusWorkRows, CandidateKeepsDetachedEffectUntilAudioRetirement) {
    auto config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    auto& buses = engine.buses;
    const auto id = harkentest::ObjectId(10);
    // Isolate row leases from the activity owner's additional lease.
    ASSERT_EQ(VN_OK, buses.CreateBus(id, VN_BUS_MASTER));
    const auto topology = engine.topologyPlans.AcquireSparse(0);
    ASSERT_NE(harken::kInvalidTopologyPlanIndex, topology);
    harken::FillLiveSignalTopology(engine, topology);
    buses.ApplyTopologyPlan(topology, harken::kInvalidBusIndex, harken::kInvalidBusIndex, 0);
    VnGainDesc gain{};
    gain.structSize = sizeof(gain);
    gain.effectId = harkentest::ObjectId(100);
    gain.gainDb = -6;
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(id, 0, VN_EFFECT_GAIN,
        reinterpret_cast<const VnEffectDesc*>(&gain)));
    engine.nullBackend.Pump(1);
    const auto bus = buses.FindRef(id);
    const auto old = buses.AudioWorkRef(bus.index);
    auto* oldState = buses.BusState(bus.index);
    const auto oldEffect = oldState->fxSlotEffectRef[0];
    ASSERT_TRUE(buses.RetainWorkRow(old));
    const auto next = buses.PrepareWorkRow();
    ASSERT_TRUE(next.Valid());
    auto* prepared = buses.PreparedWorkRow(next);
    ASSERT_NE(nullptr, prepared);
    ASSERT_EQ(VN_OK, engine.PrepareBusWorkState(bus.index, *prepared));
    const auto nextEffect = prepared->fxSlotEffectRef[0];
    ASSERT_TRUE(buses.PublishWorkRow(next));
    EXPECT_EQ(nullptr, buses.PreparedWorkRow(next));
    {
        harken::memguard::AudioThreadScope audio;
        ASSERT_TRUE(buses.AudioAdoptWorkRow(bus, next));
    }
    buses.DrainWorkRows();
    EXPECT_NE(nullptr, engine.effectBank.LiveStateFor(oldEffect));
    EXPECT_EQ(2u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
    EXPECT_EQ(oldState->generation, bus.generation);
    buses.ReleaseWorkRow(old);
    EXPECT_FALSE(buses.RetainWorkRow(old));
    buses.DrainWorkRows();
    EXPECT_NE(nullptr, engine.effectBank.LiveStateFor(oldEffect));
    {
        harken::memguard::AudioThreadScope audio;
        buses.ServiceWorkRowRetirements();
    }
    buses.DrainWorkRows();
    EXPECT_EQ(nullptr, engine.effectBank.LiveStateFor(oldEffect));
    EXPECT_NE(nullptr, engine.effectBank.LiveStateFor(nextEffect));
    EXPECT_EQ(1u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
    EXPECT_EQ(config.maxBuses, buses.WorkRowsInUse());
    buses.ReleaseWorkRow(next);
    const auto reused = buses.PrepareWorkRow();
    EXPECT_EQ(old.slot, reused.slot);
    EXPECT_GT(reused.generation, old.generation);
    EXPECT_FALSE(buses.RetainWorkRow(old));
    buses.ReleaseWorkRow(reused);
    // The installed directory still owns its row after candidate destruction.
    engine.nullBackend.Pump(1);
    EXPECT_EQ(next, buses.AudioWorkRef(bus.index));
    EXPECT_EQ(1u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
}

TEST(BusWorkRows, CancelPrivateRowReturnsEffectsAndColdIdentitySurvivesDetach) {
    auto config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    auto& buses = engine.buses;
    const auto id = harkentest::ObjectId(10);
    ASSERT_EQ(VN_OK, Vn_CreateBus(id, VN_BUS_MASTER));
    VnGainDesc gain{};
    gain.structSize = sizeof(gain);
    gain.effectId = harkentest::ObjectId(100);
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(id, 0, VN_EFFECT_GAIN,
        reinterpret_cast<const VnEffectDesc*>(&gain)));
    engine.nullBackend.Pump(1);
    const auto bus = buses.FindRef(id);
    const auto privateRow = buses.PrepareWorkRow();
    ASSERT_TRUE(privateRow.Valid());
    ASSERT_EQ(VN_OK, engine.PrepareBusWorkState(bus.index, *buses.PreparedWorkRow(privateRow)));
    EXPECT_EQ(2u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
    buses.ReleaseWorkRow(privateRow);
    EXPECT_EQ(1u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
    EXPECT_EQ(config.maxBuses, buses.WorkRowsInUse());
    const auto retained = buses.AudioWorkRef(bus.index);
    ASSERT_TRUE(buses.RetainWorkRow(retained));
    auto* state = buses.BusState(bus.index);
    state->outputPeak = .8f;
    state->outputChannelPeaks[0] = .8f;
    {
        harken::memguard::AudioThreadScope audio;
        ASSERT_TRUE(buses.AudioDetachWorkRow(bus));
        EXPECT_TRUE(buses.AudioRefMatches(bus));
        EXPECT_EQ(bus.generation, buses.AudioGenerationAt(bus.index));
        EXPECT_EQ(nullptr, buses.BusState(bus.index));
        EXPECT_FLOAT_EQ(0, buses.OutputPeak(bus.index));
        EXPECT_FLOAT_EQ(0, buses.OutputChannelPeak(bus.index, 0));
        ASSERT_TRUE(buses.AudioAdoptWorkRow(bus, retained));
    }
    EXPECT_EQ(state, buses.BusState(bus.index));
    EXPECT_FLOAT_EQ(.8f, buses.OutputPeak(bus.index));
    buses.ReleaseWorkRow(retained);
    engine.nullBackend.Pump(1);
    EXPECT_EQ(1u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
}


TEST(BusActivity, DependencyClosureKeepsDetectorsAndDropsRootlessComponents) {
    using namespace harken;
    auto config = MakeNullConfig(2, 64);
    config.maxBuses = 7;
    config.topologyPlanPoolCapacity = 16;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    for (std::uint32_t i = 1; i < 7; ++i)
        ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(i + 100), VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_ReparentBus(harkentest::ObjectId(101), harkentest::ObjectId(102)));
    ASSERT_EQ(VN_OK, Vn_ConnectBusSend(harkentest::ObjectId(102), harkentest::ObjectId(103), 0));
    engine.nullBackend.Pump(1);
    const auto topology = engine.buses.InstalledTopology();
    std::array<SignalRenderNodeInput, 9> nodes{};
    for (std::uint32_t i = 0; i < 7; ++i) {
        nodes[i].busIndex = i;
        nodes[i].busId = nodes[i].stableId = topology.busIds[i];
        nodes[i].busGeneration = topology.generations[i];
    }
    for (std::uint32_t i = 7; i < 9; ++i) {
        nodes[i].kind = SignalEndpointKindV1::Sound;
        nodes[i].stableId = harkentest::ObjectId(200 + i);
        nodes[i].receiver = true;
    }
    nodes[5].receiver = nodes[6].receiver = true;
    const SignalConnectionInput relations[]{{4, 5, 0}, {0, 6, 1}, {7, 8, 1}, {8, 7, 1}};
    auto signals = SignalRenderPlan::Prepare({2, 64, 48000, VN_CHANNEL_LAYOUT_STEREO}, nodes, relations, {});
    ASSERT_NE(signals, nullptr);
    const BusActivityDetectorDependency detectors[]{{engine.buses.RefAt(3), engine.buses.RefAt(4)}};
    const BusActivityMemberRoute members[]{{7, engine.buses.RefAt(2)}};
    BusActivitySelection selected;
    ASSERT_TRUE(PrepareBusActivitySelection(topology, signals.get(), {}, {}, detectors, members, selected));
    EXPECT_EQ(selected.buses, std::vector<std::uint32_t>{0});
    EXPECT_TRUE(selected.signalNodes.empty());
    const BusSlotRef root[]{engine.buses.RefAt(1)};
    ASSERT_TRUE(PrepareBusActivitySelection(topology, signals.get(), root, {}, detectors, members, selected));
    EXPECT_EQ(selected.buses.size(), 7u);
    EXPECT_EQ(selected.signalNodes, (std::vector<std::uint32_t>{0, 1, 2, 3, 4, 5, 6}));
    const std::uint32_t contentRoot[]{8};
    ASSERT_TRUE(PrepareBusActivitySelection(topology, signals.get(), {}, contentRoot, detectors, members, selected));
    EXPECT_EQ(selected.buses.size(), 6u);
    EXPECT_EQ(std::find(selected.buses.begin(), selected.buses.end(), 1), selected.buses.end());
    EXPECT_EQ(selected.signalNodes, (std::vector<std::uint32_t>{0, 2, 3, 4, 5, 6, 7, 8}));
    // A source must wake its receiver even if no dry audio reaches the latter.
    const BusSlotRef detectorRoot[]{engine.buses.RefAt(4)};
    ASSERT_TRUE(PrepareBusActivitySelection(topology, signals.get(), detectorRoot, {}, detectors, members, selected));
    EXPECT_NE(std::find(selected.buses.begin(), selected.buses.end(), 3), selected.buses.end());
}

TEST(BusActivity, OwnerRootsKeepTimelineIdentityAndCurrentSendsWithoutRawFacts) {
    using namespace harken;
    EngineSession session(MakeNullConfig(2, 64, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    for (unsigned id = 301; id <= 303; ++id)
        ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(id), VN_BUS_MASTER));
    const auto samples = harkentest::MakeDC(4096, .25f);
    const auto pcm = MonoDesc(samples, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(304), &pcm));
    auto play = MakePlayDesc(harkentest::ObjectId(304));
    play.busId = harkentest::ObjectId(301);
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    ASSERT_EQ(VN_OK, Vn_SetVoiceSend(voice, 19, harkentest::ObjectId(303), 0));
    engine.nullBackend.Pump(1);
    const auto fact = std::find_if(engine.voicePlayFacts.begin(), engine.voicePlayFacts.end(),
        [](const auto& row) { return row.valid; });
    ASSERT_NE(fact, engine.voicePlayFacts.end());
    const auto render = fact->renderHandle;
    const auto slot = static_cast<std::uint32_t>(render);
    auto& identity = engine.timelineVoiceInstallIdentities[slot];
    identity.renderHandle = render;
    identity.busIndex = fact->busIndex;
    identity.busGeneration = fact->busGeneration;
    const auto oldSend = engine.buses.FindRef(harkentest::ObjectId(302));
    identity.voiceSends = identity.voiceSends.WithSet({true, oldSend.index, 0, oldSend.generation, 19, 1});
    fact->valid = false;
    BusActivityRoots roots;
    ASSERT_TRUE(CaptureBusActivityOwners(engine, engine.buses.AcceptedTopology(), {}, roots));
    EXPECT_NE(std::find(roots.buses.begin(), roots.buses.end(), engine.buses.FindRef(play.busId)), roots.buses.end());
    EXPECT_NE(std::find(roots.buses.begin(), roots.buses.end(), engine.buses.FindRef(harkentest::ObjectId(303))), roots.buses.end());
    EXPECT_EQ(std::find(roots.buses.begin(), roots.buses.end(), oldSend), roots.buses.end());
    identity = {};
    fact->valid = true;
}

TEST(BusActivity, WorkObservationIsFreshAndRetainsSilentPhysicalVoicesAndTransitions) {
    using namespace harken;
    auto config = MakeNullConfig(2, 64, 48000);
    config.maxBuses = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    auto& buses = engine.buses;
    const auto id = harkentest::ObjectId(301);
    ASSERT_EQ(VN_OK, Vn_CreateBus(id, VN_BUS_MASTER));
    const auto bus = buses.FindRef(id);
    const auto work = buses.AcceptedWorkRef(bus);
    BusTree::WorkObservation observed;
    EXPECT_FALSE(buses.CaptureWorkObservation(bus, work, observed));
    engine.nullBackend.Pump(1);
    ASSERT_TRUE(buses.CaptureWorkObservation(bus, work, observed));
    EXPECT_TRUE(observed.rendered);
    EXPECT_TRUE(observed.localRecoveryReady);
    EXPECT_FALSE(observed.physicalVoice);
    EXPECT_EQ(observed.quietFrames, 0u);
    const auto firstBlock = observed.blockSequence;
    const std::vector<float> silence(64 * 4);
    const auto pcm = MonoDesc(silence, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(302), &pcm));
    auto play = MakePlayDesc(harkentest::ObjectId(302));
    play.busId = id;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    engine.nullBackend.Pump(1);
    ASSERT_TRUE(buses.CaptureWorkObservation(bus, work, observed));
    EXPECT_GT(observed.blockSequence, firstBlock);
    EXPECT_EQ(observed.frameEnd, observed.blockSequence * 64);
    EXPECT_TRUE(observed.physicalVoice);
    EXPECT_FALSE(observed.inputActive);
    EXPECT_FALSE(observed.localRecoveryReady);
    EXPECT_EQ(observed.quietFrames, 0u);
    engine.nullBackend.Pump(8);
    ASSERT_TRUE(buses.CaptureWorkObservation(bus, work, observed));
    EXPECT_FALSE(observed.physicalVoice);
    EXPECT_TRUE(observed.localRecoveryReady);
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(id, 0, 0));
    engine.nullBackend.Pump(1);
    ASSERT_TRUE(buses.CaptureWorkObservation(bus, work, observed));
    const auto revision = observed.stateRevision;
    const auto beforeRamp = observed.blockSequence;
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(id, -6, 100));
    engine.nullBackend.Pump(1);
    ASSERT_TRUE(buses.CaptureWorkObservation(bus, work, observed));
    EXPECT_GT(observed.blockSequence, beforeRamp);
    EXPECT_GT(observed.stateRevision, revision);
    EXPECT_FALSE(observed.localRecoveryReady);
    EXPECT_EQ(observed.quietFrames, 0u);
    EXPECT_FALSE(buses.CaptureWorkObservation({bus.index, bus.generation + 1}, work, observed));
    EXPECT_FALSE(buses.CaptureWorkObservation(bus, {work.slot, work.generation + 1}, observed));
    engine.nullBackend.Pump(500);
    ASSERT_TRUE(buses.CaptureWorkObservation(bus, work, observed));
    EXPECT_TRUE(observed.localRecoveryReady);
    EXPECT_GE(observed.quietFrames, 24000u);
}

TEST(BusActivity, PendingVoiceDetectorSurvivesProducerWithdrawalUntilNextBank) {
    using namespace harken;
    EngineSession session(MakeNullConfig(2, 64, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(301), VN_BUS_MASTER));
    engine.nullBackend.Pump(1);
    const auto bus = engine.buses.FindRef(harkentest::ObjectId(301));
    const auto work = engine.buses.AcceptedWorkRef(bus);
    auto& routes = engine.dynamicRoutes;
    DynamicRouteDefinition route{};
    route.active = true;
    route.targetBusIndex = bus.index;
    route.sourceKind = VN_DYNAMIC_SOURCE_VOICE;
    route.sourceIndex = 7;
    route.sourceGeneration = 1;
    route.tap = VN_DYNAMIC_TAP_VOICE_POST_GAIN_PRE_SPATIAL;
    ASSERT_TRUE(routes.AcquireRouteTap(route));
    const auto plan = routes.AcquirePlan();
    ASSERT_TRUE(routes.BuildPlan(plan, &route, false));
    routes.InstallPlan(plan);
    routes.BeginBlock();
    float pcm[64]{};
    pcm[0] = 1;
    float* channels[]{pcm};
    routes.PublishVoiceTap((std::uint64_t{1} << 32) | 7, channels, 1, 0, 64);
    routes.EndBlock(2);
    // No live Voice owner remains, but next block still owns detector PCM.
    engine.buses.BeginWorkObservations();
    engine.buses.PublishWorkObservations(3, 192, 64, true);
    BusTree::WorkObservation observed;
    ASSERT_TRUE(engine.buses.CaptureWorkObservation(bus, work, observed));
    EXPECT_FALSE(observed.physicalVoice);
    EXPECT_TRUE(observed.detectorActive);
    EXPECT_FALSE(observed.localRecoveryReady);
    routes.BeginBlock();
    routes.EndBlock(3);
    engine.buses.BeginWorkObservations();
    engine.buses.PublishWorkObservations(4, 256, 64, true);
    ASSERT_TRUE(engine.buses.CaptureWorkObservation(bus, work, observed));
    EXPECT_FALSE(observed.detectorActive);
    EXPECT_TRUE(observed.localRecoveryReady);
    routes.InstallPlan(0);
    routes.DrainAudioMessages();
    routes.ReleaseRouteTap(route);
}

TEST(BusActivity, ExactDelayRecoveryProbeBacksOffAfterFiniteTail) {
    using namespace harken;
    auto config = MakeNullConfig(2, 64, 48000);
    config.maxBuses = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    auto& buses = engine.buses;
    const auto id = harkentest::ObjectId(301);
    ASSERT_EQ(VN_OK, Vn_CreateBus(id, VN_BUS_MASTER));
    VnFeedbackDelayDesc delay{};
    delay.structSize = sizeof(delay);
    delay.delayMs = 1;
    delay.wet = delay.dry = 1;
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(id, 0, VN_EFFECT_FEEDBACK_DELAY,
        reinterpret_cast<const VnEffectDesc*>(&delay)));
    engine.nullBackend.Pump(1);
    const auto bus = buses.FindRef(id);
    const auto work = buses.AcceptedWorkRef(bus);
    auto* state = static_cast<DelayEffectState*>(buses.BusState(bus.index)->fxSlots[0].state);
    ASSERT_NE(state, nullptr);
    ASSERT_GT(state->lineFrames, 1024u);
    state->lineBase[state->lineFrames - 1] = .001f;
    // Retuning invalidates the earlier zero-state proof without changing the
    // distant ring sample; a finite tail estimate cannot certify that history.
    ASSERT_EQ(VN_OK, Vn_SetBusVolume(id, -1, 0));
    BusTree::WorkObservation observed;
    engine.nullBackend.Pump(4);
    ASSERT_TRUE(buses.CaptureWorkObservation(bus, work, observed));
    EXPECT_EQ(observed.outputTailFrames, 0u);
    EXPECT_FALSE(observed.localRecoveryReady);
    ASSERT_GT(observed.recoveryProbeFrame, 0u);
    const auto probeFrame = observed.recoveryProbeFrame;
    const auto block = observed.blockSequence;
    engine.nullBackend.Pump(4);
    ASSERT_TRUE(buses.CaptureWorkObservation(bus, work, observed));
    EXPECT_GT(observed.blockSequence, block);
    EXPECT_EQ(observed.recoveryProbeFrame, probeFrame);
    EXPECT_FALSE(observed.localRecoveryReady);
}

TEST(BusActivity, DependencyClosureDoesNotBindGhostToReusedBusAndRefusesStaleRoot) {
    using namespace harken;
    auto config = MakeNullConfig(2, 64);
    config.maxBuses = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(101), VN_BUS_MASTER));
    engine.nullBackend.Pump(1);
    const auto topology = engine.buses.InstalledTopology();
    SignalRenderNodeInput nodes[2];
    nodes[0].kind = SignalEndpointKindV1::Sound;
    nodes[0].stableId = harkentest::ObjectId(400);
    nodes[1].busIndex = 1;
    nodes[1].busId = nodes[1].stableId = harkentest::ObjectId(401);
    nodes[1].busGeneration = topology.generations[1] + 1;
    nodes[1].receiver = true;
    const SignalConnectionInput relation{0, 1, 1};
    auto signals = SignalRenderPlan::Prepare({2, 64, 48000, VN_CHANNEL_LAYOUT_STEREO}, nodes, {&relation, 1}, {});
    ASSERT_NE(signals, nullptr);
    const std::uint32_t root[]{0};
    BusActivitySelection selected;
    ASSERT_TRUE(PrepareBusActivitySelection(topology, signals.get(), {}, root, {}, {}, selected));
    EXPECT_EQ(selected.buses, std::vector<std::uint32_t>{0});
    EXPECT_EQ(selected.signalNodes, (std::vector<std::uint32_t>{0, 1}));
    const BusSlotRef stale[]{BusSlotRef{1, nodes[1].busGeneration}};
    EXPECT_FALSE(PrepareBusActivitySelection(topology, signals.get(), stale, {}, {}, {}, selected));
    EXPECT_EQ(selected.buses, std::vector<std::uint32_t>{0});
    EXPECT_EQ(selected.signalNodes, (std::vector<std::uint32_t>{0, 1}));
}

TEST(BusActivity, RealMixerUsesCompactPlanesAndReconstructedWorkRows) {
    auto config = MakeNullConfig(4, kBlockFrames);
    config.maxBuses = 4;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    auto& buses = engine.buses;
    const auto activeId = harkentest::ObjectId(10), idleId = harkentest::ObjectId(11);
    ASSERT_EQ(VN_OK, Vn_CreateBus(activeId, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_CreateBus(idleId, VN_BUS_MASTER));
    VnGainDesc gain{};
    gain.structSize = sizeof(gain);
    gain.effectId = harkentest::ObjectId(100);
    gain.gainDb = -6.0205999f;
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(idleId, 0, VN_EFFECT_GAIN,
        reinterpret_cast<const VnEffectDesc*>(&gain)));
    engine.nullBackend.Pump(1);
    const auto master = buses.FindRef(VN_BUS_MASTER), active = buses.FindRef(activeId), idle = buses.FindRef(idleId);
    const auto idleWork = buses.AudioWorkRef(idle.index);
    const std::array compact{
        harken::BusTree::WorkBinding{active, buses.AudioWorkRef(active.index)},
        harken::BusTree::WorkBinding{master, buses.AudioWorkRef(master.index)}};
    const auto publish = [&](harken::BusActivityPlan* plan) {
        ASSERT_NE(nullptr, plan);
        harken::CommandRecord carrier{};
        carrier.opcode = harken::kOpcodeSetBusVolume;
        carrier.payload.setBusVolume.busIndex = 0;
        carrier.payload.setBusVolume.volumeDb = 0;
        harken::CommandQueue::BatchReservation reservation;
        harken::CommandQueue::PreparedBatchProof proof;
        ASSERT_TRUE(engine.commands.ReserveBatch(1, &reservation));
        ASSERT_TRUE(engine.commands.ProvePreparedBatch(&reservation, &carrier, 1, &proof));
        ASSERT_TRUE(engine.commands.BindPreparedBusActivity(proof, *buses.ActivityAttachment(plan)));
        EXPECT_EQ(1u, engine.commands.ConsumePreparedBatch(std::move(proof)).Count());
    };
    EXPECT_EQ(nullptr, buses.PrepareActivity(buses.InstalledTopology(), {compact.data(), 1}));
    publish(buses.PrepareActivity(buses.InstalledTopology(), compact));
    const auto pcm = harkentest::MakeDC(kBlockFrames, .5f);
    const auto desc = MonoDesc(pcm, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(1), &desc));
    auto play = MakePlayDesc(harkentest::ObjectId(1));
    play.busId = activeId;
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    Capture first;
    harkentest::PumpAndCapture(&engine, 1, first);
    EXPECT_EQ(2u, buses.WorkingPlaneCount());
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(snapshot);
    ASSERT_EQ(VN_OK, Vn_CaptureSnapshot(&snapshot));
    EXPECT_EQ(3u, snapshot.busCount);
    EXPECT_EQ(nullptr, buses.BusState(idle.index));
    EXPECT_TRUE(buses.AudioRefMatches(idle));
    EXPECT_FLOAT_EQ(0, buses.OutputPeak(idle.index));
    for (auto sample : first.left) EXPECT_FLOAT_EQ(.5f, sample);
    buses.DrainActivities();
    EXPECT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
    // A cancelled real candidate must return its private effects without
    // changing the installed projection or manufacturing a queued predecessor.
    const auto cancelled = buses.PrepareWorkRow();
    ASSERT_TRUE(cancelled.Valid());
    ASSERT_EQ(VN_OK, engine.PrepareBusWorkState(idle.index, *buses.PreparedWorkRow(cancelled)));
    const std::array cancelledRows{compact[0], compact[1], harken::BusTree::WorkBinding{idle, cancelled}};
    auto* abandoned = buses.PrepareActivity(buses.InstalledTopology(), cancelledRows);
    ASSERT_NE(nullptr, abandoned);
    buses.ReleaseWorkRow(cancelled);
    const auto before = engine.commands.PublishedHead();
    {
        harken::CommandRecord carrier{};
        carrier.opcode = harken::kOpcodeSetBusVolume;
        harken::CommandQueue::BatchReservation reservation;
        harken::CommandQueue::PreparedBatchProof proof;
        ASSERT_TRUE(engine.commands.ReserveBatch(1, &reservation));
        ASSERT_TRUE(engine.commands.ProvePreparedBatch(&reservation, &carrier, 1, &proof));
        ASSERT_TRUE(engine.commands.BindPreparedBusActivity(proof, *buses.ActivityAttachment(abandoned)));
        proof.Release();
        engine.commands.CancelReservedBatch(&reservation);
    }
    buses.DrainActivities();
    EXPECT_EQ(before, engine.commands.PublishedHead());
    EXPECT_EQ(2u, buses.WorkingPlaneCount());
    EXPECT_EQ(0u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
    const auto restored = buses.PrepareWorkRow();
    ASSERT_TRUE(restored.Valid());
    EXPECT_FALSE(buses.RetainWorkRow(idleWork));
    EXPECT_NE(idleWork, restored);
    ASSERT_EQ(VN_OK, engine.PrepareBusWorkState(idle.index, *buses.PreparedWorkRow(restored)));
    const std::array expanded{compact[0], compact[1], harken::BusTree::WorkBinding{idle, restored}};
    publish(buses.PrepareActivity(buses.InstalledTopology(), expanded));
    buses.ReleaseWorkRow(restored);
    play.busId = idleId;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    Capture second;
    harkentest::PumpAndCapture(&engine, 1, second);
    EXPECT_EQ(3u, buses.WorkingPlaneCount());
    for (auto sample : second.left) EXPECT_NEAR(.25f, sample, 1e-6f);
    EXPECT_EQ(0u, buses.ActivityFallbackCount());
    buses.DrainActivities();
    EXPECT_EQ(1u, engine.effectBank.InUseOf(VN_EFFECT_GAIN));
}


TEST(BusActivity, ConditionalSleepRejectsStaleProofAndWakeOwnsRemovedRowPastAck) {
    using namespace harken;
    for (const bool reject : {false, true}) {
        SCOPED_TRACE(reject);
        EngineSession session(MakeNullConfig(2, 64, 48000));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        auto& buses = engine.buses;
        const auto id = harkentest::ObjectId(601);
        ASSERT_EQ(VN_OK, Vn_CreateBus(id, VN_BUS_MASTER));
        VnGainDesc gain{};
        gain.structSize = sizeof(gain);
        gain.gainDb = -6;
        ASSERT_EQ(VN_OK, Vn_SetBusEffect(id, 0, VN_EFFECT_GAIN, reinterpret_cast<const VnEffectDesc*>(&gain)));
        engine.nullBackend.Pump(400);
        const auto bus = buses.FindRef(id), master = buses.RefAt(0);
        const auto work = buses.AcceptedWorkRef(bus);
        BusTree::WorkObservation fact;
        ASSERT_TRUE(buses.CaptureWorkObservation(bus, work, fact));
        ASSERT_GE(fact.quietFrames, 24000u);
        const BusTree::WorkBinding compact[]{ {master, buses.AcceptedWorkRef(master)} };
        auto* sleep = buses.PrepareActivity(buses.AcceptedTopology(), compact);
        ASSERT_NE(sleep, nullptr);
        ASSERT_TRUE(buses.PrepareConditionalSleep(*sleep, {&fact, 1}, engine.voices));
        const auto publish = [&](BusActivityPlan* candidate) {
            CommandRecord command{};
            command.opcode = kOpcodeSetBusActivity;
            CommandQueue::BatchReservation reservation;
            CommandQueue::PreparedBatchProof proof;
            ASSERT_TRUE(engine.commands.ReserveBatch(1, &reservation));
            ASSERT_TRUE(engine.commands.ProvePreparedBatch(&reservation, &command, 1, &proof));
            ASSERT_TRUE(engine.commands.BindPreparedBusActivity(proof, *buses.ActivityAttachment(candidate)));
            ASSERT_EQ(engine.commands.ConsumePreparedBatch(std::move(proof)).Count(), 1u);
        };
        if (reject) ASSERT_EQ(VN_OK, Vn_SetBusVolume(id, -3, 0));
        publish(sleep);
        ASSERT_EQ(buses.AcceptedWorkRef(bus), work);
        const BusTree::WorkBinding expanded[]{compact[0], {bus, buses.AcceptedWorkRef(bus)}};
        auto* wake = buses.PrepareActivity(buses.AcceptedTopology(), expanded);
        ASSERT_NE(wake, nullptr);
        publish(wake);
        engine.nullBackend.Pump(1);
        EXPECT_EQ(sleep->sleepDecision.load(), reject ? BusActivityPlan::SleepDecision::Rejected
                                                     : BusActivityPlan::SleepDecision::Accepted);
        EXPECT_EQ(buses.AudioWorkRef(bus.index), work);
        EXPECT_EQ(buses.WorkingPlaneCount(), 2u);
        buses.AcknowledgeSleep();
        buses.DrainActivities();
        engine.nullBackend.Pump(1);
        buses.DrainActivities();
        EXPECT_EQ(buses.AudioWorkRef(bus.index), work);
        EXPECT_EQ(engine.effectBank.InUseOf(VN_EFFECT_GAIN), 1u);
        EXPECT_EQ(buses.ActivityFallbackCount(), 0u);
        if (reject) EXPECT_NEAR(buses.BusState(bus.index)->gain.current, std::pow(10.f, -3.f / 20), 1e-6f);
    }
}

TEST(BusActivity, PendingSleepAllowsExplicitContentReplacementOnEitherBranch) {
    using namespace harken;
    for (const bool reject : {false, true}) {
        SCOPED_TRACE(reject);
        EngineSession session(MakeNullConfig(2, 64, 48000));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        auto& buses = engine.buses;
        const auto id = harkentest::ObjectId(621);
        ASSERT_EQ(VN_OK, Vn_CreateBus(id, VN_BUS_MASTER));
        engine.nullBackend.Pump(1);
        const std::array working{BusTree::WorkBinding{buses.RefAt(0), buses.AcceptedWorkRef(buses.RefAt(0))},
            BusTree::WorkBinding{buses.RefAt(1), buses.AcceptedWorkRef(buses.RefAt(1))}};
        std::array<SignalRenderNodeInput, 2> nodes{};
        for (unsigned i = 0; i < 2; ++i) {
            nodes[i].busIndex = i;
            nodes[i].stableId = nodes[i].busId = buses.IdAt(i);
            nodes[i].busGeneration = buses.GenerationAt(i);
        }
        const auto publish = [&](BusActivityPlan* candidate) {
            ASSERT_NE(candidate, nullptr);
            CommandRecord command{};
            command.opcode = kOpcodeSetBusActivity;
            CommandQueue::BatchReservation reservation;
            CommandQueue::PreparedBatchProof proof;
            ASSERT_TRUE(engine.commands.ReserveBatch(1, &reservation));
            ASSERT_TRUE(engine.commands.ProvePreparedBatch(&reservation, &command, 1, &proof));
            ASSERT_TRUE(engine.commands.BindPreparedBusActivity(proof, *buses.ActivityAttachment(candidate)));
            ASSERT_EQ(engine.commands.ConsumePreparedBatch(std::move(proof)).Count(), 1u);
        };
        auto original = SignalRenderPlan::Prepare({2, 64, 48000, VN_CHANNEL_LAYOUT_STEREO}, nodes, {}, {});
        ASSERT_NE(original, nullptr);
        original->EnableActualRouting(2);
        ASSERT_TRUE(original->ConfigurePublication(501, 51, nullptr, 64));
        publish(buses.PrepareActivity(buses.AcceptedTopology(), working, std::move(original)));
        engine.nullBackend.Pump(400);
        const auto* previous = buses.AcceptedActivitySignals();
        const std::uint32_t masterNode[]{0};
        auto projected = previous->PrepareProjection(masterNode, 2);
        ASSERT_NE(projected, nullptr);
        ASSERT_TRUE(projected->ConfigurePublication(502, 51, previous, 64));
        auto* sleep = buses.PrepareActivity(buses.AcceptedTopology(), {working.data(), 1}, std::move(projected));
        ASSERT_NE(sleep, nullptr);
        BusTree::WorkObservation fact;
        ASSERT_TRUE(buses.CaptureWorkObservation(working[1].bus, working[1].work, fact));
        ASSERT_TRUE(buses.PrepareConditionalSleep(*sleep, {&fact, 1}, engine.voices));
        if (reject) ASSERT_EQ(VN_OK, Vn_SetBusVolume(id, -3, 0));
        publish(sleep);
        // Explicitly removing the old content must accept the actual A or S
        // predecessor, without treating an unrelated publication as valid.
        auto* replacement = buses.PrepareActivity(buses.AcceptedTopology(), working, {},
            BusTree::SignalTransition::ReplaceContent, buses.AcceptedActivitySignals());
        publish(replacement);
        engine.nullBackend.Pump(1);
        EXPECT_EQ(sleep->sleepDecision.load(), reject ? BusActivityPlan::SleepDecision::Rejected
                                                     : BusActivityPlan::SleepDecision::Accepted);
        EXPECT_EQ(buses.InstalledSignals(), nullptr);
        EXPECT_EQ(buses.WorkingPlaneCount(), 2u);
        buses.AcknowledgeSleep();
        buses.DrainActivities();
    }
}

TEST(BusActivity, AcceptedSleepKeepsColdHistoryUntilAckThenReturnsEffects) {
    using namespace harken;
    EngineSession session(MakeNullConfig(2, 64, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    auto& buses = engine.buses;
    const auto id = harkentest::ObjectId(611);
    ASSERT_EQ(VN_OK, Vn_CreateBus(id, VN_BUS_MASTER));
    VnGainDesc gain{};
    gain.structSize = sizeof(gain);
    gain.gainDb = -6;
    ASSERT_EQ(VN_OK, Vn_SetBusEffect(id, 0, VN_EFFECT_GAIN, reinterpret_cast<const VnEffectDesc*>(&gain)));
    engine.nullBackend.Pump(400);
    const auto bus = buses.FindRef(id);
    BusTree::WorkObservation fact;
    ASSERT_TRUE(buses.CaptureWorkObservation(bus, buses.AcceptedWorkRef(bus), fact));
    const BusTree::WorkBinding compact[]{ {buses.RefAt(0), buses.AcceptedWorkRef(buses.RefAt(0))} };
    auto* sleep = buses.PrepareActivity(buses.AcceptedTopology(), compact);
    ASSERT_NE(sleep, nullptr);
    ASSERT_TRUE(buses.PrepareConditionalSleep(*sleep, {&fact, 1}, engine.voices));
    CommandRecord command{};
    command.opcode = kOpcodeSetBusActivity;
    CommandQueue::BatchReservation reservation;
    CommandQueue::PreparedBatchProof proof;
    ASSERT_TRUE(engine.commands.ReserveBatch(1, &reservation));
    ASSERT_TRUE(engine.commands.ProvePreparedBatch(&reservation, &command, 1, &proof));
    ASSERT_TRUE(engine.commands.BindPreparedBusActivity(proof, *buses.ActivityAttachment(sleep)));
    ASSERT_EQ(engine.commands.ConsumePreparedBatch(std::move(proof)).Count(), 1u);
    engine.nullBackend.Pump(1);
    ASSERT_EQ(sleep->sleepDecision.load(), BusActivityPlan::SleepDecision::Accepted);
    ASSERT_EQ(buses.BusState(bus.index), nullptr);
    EXPECT_EQ(engine.effectBank.InUseOf(VN_EFFECT_GAIN), 1u);
    const auto saved = sleep->sleeping[0].saved;
    ASSERT_EQ(saved->effects[0].type, VN_EFFECT_GAIN);
    EXPECT_TRUE(saved->effects[0].hasRendered);
    buses.AcknowledgeSleep();
    buses.DrainActivities();
    engine.nullBackend.Pump(1);
    buses.DrainActivities();
    EXPECT_EQ(engine.effectBank.InUseOf(VN_EFFECT_GAIN), 0u);
    EXPECT_FALSE(buses.AcceptedWorkRef(bus).Valid());
    EXPECT_TRUE(saved->effects[0].hasRendered);
    // Preparing and cancelling a production projection must not consume cold
    // state. A subsequent real Send wake still restores the original history.
    const auto fullTopology = buses.AcceptedTopology();
    ASSERT_TRUE(engine.commands.ReserveBatch(1, &reservation));
    ASSERT_TRUE(PrepareTopologyActivity(engine, fullTopology, reservation, {&command, 1}, proof,
        nullptr, false));
    proof.Release();
    engine.commands.CancelReservedBatch(&reservation);
    buses.DrainActivities();
    EXPECT_FALSE(buses.AcceptedWorkRef(bus).Valid());
    EXPECT_EQ(engine.effectBank.InUseOf(VN_EFFECT_GAIN), 0u);
    const auto fresh = buses.PrepareWorkRow();
    ASSERT_TRUE(fresh.Valid());
    auto& row = *buses.PreparedWorkRow(fresh);
    ASSERT_EQ(engine.PrepareBusWorkState(bus.index, row), VN_OK);
    ASSERT_TRUE(buses.RestoreDormantWork(row));
    EXPECT_TRUE(static_cast<GainState*>(row.fxSlots[0].state)->hasRendered);
    EXPECT_EQ(static_cast<GainState*>(row.fxSlots[0].state)->gainLinear, saved->effects[0].values.gain);
    buses.ReleaseWorkRow(fresh);
    const auto pcm = harkentest::MakeDC(4096, .25f);
    const auto desc = MonoDesc(pcm, 48000);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(612), &desc));
    auto play = MakePlayDesc(harkentest::ObjectId(612));
    VnVoiceHandle voice = 0;
    ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    engine.nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_SetVoiceSend(voice, 12, id, 0));
    EXPECT_TRUE(buses.AcceptedWorkRef(bus).Valid());
    Capture captured;
    harkentest::PumpAndCapture(&engine, 1, captured);
    const auto* restored = buses.BusState(bus.index);
    ASSERT_NE(restored, nullptr);
    EXPECT_TRUE(static_cast<const GainState*>(restored->fxSlots[0].state)->hasRendered);
    EXPECT_EQ(engine.busEffectInstanceRefs[bus.index][0].generation, restored->fxSlotEffectRef[0].generation);
    for (auto sample : captured.left) EXPECT_NEAR(sample, .25f * (1 + std::pow(10.f, -6.f / 20)), 1e-6f);
    EXPECT_EQ(buses.ActivityFallbackCount(), 0u);
}

TEST(BusActivity, WakeCannotCountOrdinaryBusBindingsAsMissingContentNodes) {
    using namespace harken;
    for (bool extraDormantBus : {false, true}) {
        SCOPED_TRACE(extraDormantBus);
        EngineSession session(MakeNullConfig(2, 64, 48000));
        ASSERT_EQ(VN_OK, session.InitResult());
        auto& engine = *session.Engine();
        auto& buses = engine.buses;
        for (unsigned i = 0; i < (extraDormantBus ? 3u : 2u); ++i)
            ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(650 + i), VN_BUS_MASTER));
        const auto pcm = harkentest::MakeDC(4096, .25f);
        const auto desc = MonoDesc(pcm, 48000);
        ASSERT_EQ(VN_OK, Vn_RegisterPCM(harkentest::ObjectId(660), &desc));
        auto play = MakePlayDesc(harkentest::ObjectId(660));
        VnVoiceHandle voice = 0;
        ASSERT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
        engine.nullBackend.Pump(1);
        std::array<SignalRenderNodeInput, 3> nodes{};
        nodes[0].busIndex = 0;
        nodes[0].busId = nodes[0].stableId = VN_BUS_MASTER;
        nodes[0].busGeneration = buses.GenerationAt(0);
        for (unsigned i = 1; i != 3; ++i) {
            nodes[i].kind = SignalEndpointKindV1::Sound;
            nodes[i].stableId = harkentest::ObjectId(659 + i);
        }
        nodes[2].receiver = true;
        const std::array relations{SignalConnectionInput{1, 2, 1, harkentest::ObjectId(662)}};
        SignalMemberProof member;
        member.node = 1; member.stableId = nodes[1].stableId; member.propertyFingerprint = 11;
        auto full = SignalRenderPlan::Prepare({2, 64, 48000, VN_CHANNEL_LAYOUT_STEREO},
            nodes, relations, {}, {&member, 1});
        ASSERT_NE(full, nullptr);
        full->EnableActualRouting(buses.BusCount());
        ASSERT_TRUE(full->ConfigurePublication(701, 71, nullptr, 64));
        engine.acceptedSignalPlan = std::move(full);
        auto topology = buses.AcceptedTopology();
        topology.signals = engine.acceptedSignalPlan.get();
        const BusActivitySelection compact{{2, 1, 0}, {0}};
        CommandRecord command{};
        command.opcode = kOpcodeSetBusActivity;
        CommandQueue::BatchReservation reservation;
        CommandQueue::PreparedBatchProof proof;
        ASSERT_TRUE(engine.commands.ReserveBatch(1, &reservation));
        ASSERT_TRUE(PrepareTopologyActivity(engine, topology, reservation, {&command, 1}, proof, &compact));
        ASSERT_EQ(engine.commands.ConsumePreparedBatch(std::move(proof)).Count(), 1u);
        engine.nullBackend.Pump(1);
        ASSERT_EQ(buses.AcceptedActivitySignals()->NodeCount(), 3u);
        EXPECT_FALSE(buses.AcceptedActivitySignals()->ContainsNodeDefinition(*topology.signals, 1));
        const auto fact = std::find_if(engine.voicePlayFacts.begin(), engine.voicePlayFacts.end(),
            [](const auto& value) { return value.valid; });
        ASSERT_NE(fact, engine.voicePlayFacts.end());
        auto& path = engine.playbackPaths[static_cast<std::uint32_t>(fact->renderHandle)];
        PlaybackPathRow row;
        row.stableId = member.stableId;
        row.rowKind = ResolvedPlayablePathRowKindV1::Definition;
        row.propertyFingerprint = member.propertyFingerprint;
        row.source = member.source;
        path.voiceInstance = fact->renderHandle;
        path.AssignPath({&row, 1});
        ASSERT_TRUE(engine.commands.ReserveBatch(1, &reservation));
        ASSERT_TRUE(engine.commands.ProvePreparedBatch(&reservation, &command, 1, &proof));
        ASSERT_TRUE(PrepareBusActivityWake(engine, reservation, {&command, 1}, proof));
        ASSERT_EQ(engine.commands.ConsumePreparedBatch(std::move(proof)).Count(), 1u);
        EXPECT_TRUE(buses.AcceptedActivitySignals()->ContainsNodeDefinition(*topology.signals, 1));
        EXPECT_TRUE(buses.AcceptedActivitySignals()->ContainsNodeDefinition(*topology.signals, 2));
        EXPECT_EQ(buses.AcceptedActivitySignals()->NodeCount(), 5u);
        engine.nullBackend.Pump(1);
        EXPECT_EQ(buses.ActivityFallbackCount(), 0u);
    }
}

TEST(BusActivity, ReusedLogicalSlotRequiresANewWorkIncarnation) {
    auto config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    auto& buses = engine.buses;
    const auto firstId = harkentest::ObjectId(10), nextId = harkentest::ObjectId(11);
    ASSERT_EQ(VN_OK, Vn_CreateBus(firstId, VN_BUS_MASTER));
    engine.nullBackend.Pump(1);
    const auto first = buses.FindRef(firstId);
    const auto oldWork = buses.AudioWorkRef(first.index);
    ASSERT_TRUE(buses.RetainWorkRow(oldWork));
    ASSERT_EQ(VN_OK, Vn_DestroyBus(firstId));
    engine.nullBackend.Pump(1);
    ASSERT_EQ(VN_OK, Vn_Update());
    ASSERT_EQ(VN_OK, Vn_CreateBus(nextId, VN_BUS_MASTER));
    engine.nullBackend.Pump(1);
    const auto next = buses.FindRef(nextId), master = buses.FindRef(VN_BUS_MASTER);
    ASSERT_EQ(first.index, next.index);
    ASSERT_NE(first.generation, next.generation);
    const std::array stale{
        harken::BusTree::WorkBinding{master, buses.AudioWorkRef(0)},
        harken::BusTree::WorkBinding{next, oldWork}};
    EXPECT_EQ(nullptr, buses.PrepareActivity(buses.InstalledTopology(), stale));
    const auto replacement = buses.PrepareWorkRow();
    ASSERT_TRUE(replacement.Valid());
    ASSERT_EQ(VN_OK, engine.PrepareBusWorkState(next.index, *buses.PreparedWorkRow(replacement)));
    const std::array valid{stale[0], harken::BusTree::WorkBinding{next, replacement}};
    auto* candidate = buses.PrepareActivity(buses.InstalledTopology(), valid);
    ASSERT_NE(nullptr, candidate);
    buses.CancelActivity(candidate);
    buses.ReleaseWorkRow(replacement);
    buses.DrainActivities();
    buses.ReleaseWorkRow(oldWork);
}


TEST(BusActivity, SignalReplacementIsExplicitAndFencedToItsPreparedPredecessor) {
    auto config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    auto& buses = engine.buses;
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(10), VN_BUS_MASTER));
    engine.nullBackend.Pump(1);
    const std::array working{
        harken::BusTree::WorkBinding{buses.RefAt(0), buses.AudioWorkRef(0)},
        harken::BusTree::WorkBinding{buses.RefAt(1), buses.AudioWorkRef(1)}};
    std::array<harken::SignalRenderNodeInput, 2> nodes{};
    for (unsigned i = 0; i < nodes.size(); ++i) {
        nodes[i].busIndex = i;
        nodes[i].busId = buses.IdAt(i);
        nodes[i].stableId = buses.IdAt(i);
        nodes[i].busGeneration = buses.GenerationAt(i);
    }
    const auto signals = [&](std::uint64_t publication, std::uint64_t content,
                             const harken::SignalRenderPlan* predecessor = nullptr) {
        auto result = harken::SignalRenderPlan::Prepare(
            {engine.config.mixChannelCount, kBlockFrames, 48000, engine.config.mixChannelLayout}, nodes, {}, {});
        if (result != nullptr) {
            result->EnableActualRouting(2);
            if (!result->ConfigurePublication(publication, content, predecessor, kBlockFrames)) result.reset();
        }
        return result;
    };
    const auto adopt = [&](harken::BusActivityPlan* plan) {
        auto* attachment = buses.ActivityAttachment(plan);
        if (attachment == nullptr) return false;
        attachment->queued(attachment->context);
        harken::memguard::AudioThreadScope audio;
        const bool result = attachment->adopt(attachment->context);
        if (!result) attachment->discard(attachment->context);
        return result;
    };
    auto* first = buses.PrepareActivity(buses.InstalledTopology(), working, signals(10, 100));
    ASSERT_TRUE(adopt(first));
    auto* original = buses.InstalledSignals();
    using Transition = harken::BusTree::SignalTransition;
    EXPECT_EQ(nullptr, buses.PrepareActivity(buses.InstalledTopology(), working,
        signals(11, 100), Transition::ReplaceContent, original));
    auto* staleReplacement = buses.PrepareActivity(buses.InstalledTopology(), working,
        signals(12, 200), Transition::ReplaceContent, original);
    ASSERT_NE(nullptr, staleReplacement);
    auto* continuation = buses.PrepareActivity(buses.InstalledTopology(), working, signals(13, 100, original));
    ASSERT_TRUE(adopt(continuation));
    auto* continued = buses.InstalledSignals();
    EXPECT_FALSE(adopt(staleReplacement));
    EXPECT_EQ(continued, buses.InstalledSignals());
    auto* unprepared = buses.PrepareActivity(buses.InstalledTopology(), working, signals(14, 100));
    EXPECT_FALSE(adopt(unprepared));
    EXPECT_EQ(continued, buses.InstalledSignals());
    auto* replacement = buses.PrepareActivity(buses.InstalledTopology(), working,
        signals(15, 200), Transition::ReplaceContent, continued);
    ASSERT_TRUE(adopt(replacement));
    EXPECT_EQ(200u, buses.InstalledSignals()->ContentIdentity());
    auto* implicitRemoval = buses.PrepareActivity(buses.InstalledTopology(), working);
    EXPECT_FALSE(adopt(implicitRemoval));
    auto* removal = buses.PrepareActivity(buses.InstalledTopology(), working, {},
        Transition::ReplaceContent, buses.InstalledSignals());
    ASSERT_TRUE(adopt(removal));
    EXPECT_EQ(nullptr, buses.InstalledSignals());
    buses.DrainActivities();
    EXPECT_EQ(0u, buses.ActivityFallbackCount());
}

TEST(BusActivity, InitialTransactionalSlotBindsItsWorkIdentity) {
    auto config = MakeNullConfig(2, kBlockFrames);
    config.maxBuses = 2;
    EngineSession session(config);
    ASSERT_EQ(VN_OK, session.InitResult());
    auto& engine = *session.Engine();
    auto& buses = engine.buses;
    buses.CommitLogicSlot(1, harkentest::ObjectId(10), 0, 1);
    const auto index = engine.topologyPlans.AcquireSparse(0);
    ASSERT_NE(harken::kInvalidTopologyPlanIndex, index);
    harken::FillLiveSignalTopology(engine, index);
    buses.ApplyTopologyPlan(index, harken::kInvalidBusIndex, harken::kInvalidBusIndex, 0);
    const std::array working{
        harken::BusTree::WorkBinding{buses.RefAt(0), buses.AudioWorkRef(0)},
        harken::BusTree::WorkBinding{buses.RefAt(1), buses.AudioWorkRef(1)}};
    auto* plan = buses.PrepareActivity(buses.InstalledTopology(), working);
    ASSERT_NE(nullptr, plan);
    buses.CancelActivity(plan);
    buses.DrainActivities();
}

} // namespace
