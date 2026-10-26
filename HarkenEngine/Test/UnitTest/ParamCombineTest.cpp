// Parameter system behavior: the combine model's four domains reducing
// layered contributions onto authored bases, accumulation of several
// bindings into one category slot, changed-only commit flow with retry on
// a refused push, and the Variable table's public-surface contracts
// (non-finite refusal, fixed capacity).
#include "Common/ObjectIdTestSupport.h"
#include "Common/TestUtil.h"

#include "harken/core/EngineConfig.h"
#include "harken/param/BindingTable.h"
#include "harken/param/EmitterTable.h"
#include "harken/param/ParamSystem.h"

#include <cmath>
#include <limits>
#include <type_traits>
#include <utility>
#include <vector>

namespace {

using harken::Curve;
using harken::CurvePoint;
using harken::ParamCommit;
using harken::ParamSystem;
using harkentest::EngineSession;
using harkentest::MakeNullConfig;

// A voice handle as the pool would mint it: generation 1, slot 0. The
// parameter system resolves states by the packed slot index and verifies
// the full handle; no live engine is needed for combine-model tests.
constexpr VnVoiceHandle kVoice0 = (1ULL << 32) | 0u;
constexpr VnVoiceHandle kVoice1 = (1ULL << 32) | 1u;

harken::EngineConfig DefaultConfig() {
    return harken::EngineConfig{};
}

static_assert(noexcept(
    std::declval<ParamSystem&>().CommitPreparedVariable(
        static_cast<ParamSystem::PreparedVariableToken*>(nullptr))));

TEST(ParamCombine,
     PreparedVariableFailureIsAtomicAndCommitUsesTheExactSlot) {
    ParamSystem params(DefaultConfig());
    ParamSystem::PreparedVariableToken token;

    EXPECT_EQ(VN_INVALID_ARG,
              params.PrepareVariable(
                  harkentest::ObjectId(71), std::numeric_limits<float>::quiet_NaN(), 0, &token));
    EXPECT_FALSE(token.IsPrepared());
    EXPECT_EQ(0u, params.VariableCount());

    EXPECT_EQ(VN_OK, params.PrepareVariable(harkentest::ObjectId(71), -96.0f, 0, &token));
    ASSERT_TRUE(token.IsPrepared());
    EXPECT_EQ(0u, token.SlotIndex());
    params.CommitPreparedVariable(&token);

    EXPECT_FALSE(token.IsPrepared());
    EXPECT_EQ(1u, params.VariableCount());
    float value = 0.0f;
    ASSERT_TRUE(params.ScopedVariableValue(nullptr, 0, harkentest::ObjectId(71), &value));
    EXPECT_FLOAT_EQ(-96.0f, value);
}

Curve LinearCurve(float x0, float y0, float x1, float y1) {
    Curve curve;
    const CurvePoint pts[] = {{x0, y0}, {x1, y1}};
    const harken::CurveHandlePair handles[] = {
        {pts[0].x, pts[0].y, pts[1].x, pts[1].y}};
    EXPECT_TRUE(curve.SetPoints(pts, handles, 2));
    return curve;
}

std::vector<ParamCommit> FlushAll(ParamSystem& params) {
    std::vector<ParamCommit> commits;
    params.FlushCommits([&commits](const ParamCommit& commit) {
        commits.push_back(commit);
        return harken::FlushOutcome::Committed;
    });
    return commits;
}

void AttachIndex(ParamSystem& params, harken::BindingDependencyIndex& index) {
    ASSERT_TRUE(index.Reset(params.PreparationCapacity(), params.PreparationCapacity() + 2u));
    ASSERT_TRUE(params.AttachBindingDependencies(&index));
}

std::uint32_t DirtyDependencyCount(const harken::BindingDependencyIndex& index) {
    std::uint32_t count = 0;
    harken::BindingDirtyTarget target;
    while (index.DirtyTargetAt(count, &target)) ++count;
    return count;
}

TEST(ParamCombine, IndexedPreparationCancelsRefusalsAndConsumesWithoutGrowth) {
    auto config = DefaultConfig();
    config.voiceBindingInstanceSlots = 1;
    harken::BindingDependencyIndex index;
    ParamSystem params(config);
    AttachIndex(params, index);
    EXPECT_EQ(0u, params.BindingStorageBytes());
    params.OnVoiceStarted(kVoice0, 0, 1);
    const auto variable = harkentest::ObjectId(7);
    const Curve curve = LinearCurve(0, 0, 1, -6);
    ParamSystem::PreparedBindingToken prepared;
    params.SetBindingCommitPageLimitForTests(0);
    EXPECT_FALSE(params.PrepareBinding(&prepared));
    EXPECT_FALSE(prepared.IsPrepared());
    EXPECT_EQ(0u, params.VoiceBindingCount(kVoice0));
    params.SetBindingCommitPageLimitForTests(SIZE_MAX);
    {
        ASSERT_TRUE(params.PrepareBinding(&prepared));
        ParamSystem::PreparedBindingToken moved(std::move(prepared));
        EXPECT_FALSE(prepared.IsPrepared());
        EXPECT_TRUE(moved.IsPrepared());
    }
    ASSERT_TRUE(params.PrepareBinding(&prepared));
    auto bytes = index.StorageBytes();
    EXPECT_FALSE(params.BindVoiceTarget(variable, kVoice0, harken::kBuiltinTargetCount, curve, &prepared));
    EXPECT_FALSE(prepared.IsPrepared());
    EXPECT_EQ(bytes, index.StorageBytes());
    EXPECT_EQ(0u, params.VoiceBindingCount(kVoice0));
    ASSERT_TRUE(params.PrepareBinding(&prepared));
    bytes = index.StorageBytes();
    const auto bindingBytes = params.BindingStorageBytes();
    ASSERT_TRUE(params.BindVoiceTarget(variable, kVoice0, harken::kTargetVoiceVolume, curve, &prepared));
    EXPECT_FALSE(prepared.IsPrepared());
    EXPECT_EQ(bytes, index.StorageBytes());
    EXPECT_EQ(bindingBytes, params.BindingStorageBytes());
    ASSERT_TRUE(params.PrepareBinding(&prepared));
    bytes = index.StorageBytes();
    EXPECT_FALSE(params.BindVoiceTarget(variable, kVoice0, harken::kTargetVoicePitch, curve, &prepared));
    EXPECT_FALSE(prepared.IsPrepared());
    EXPECT_EQ(bytes, index.StorageBytes());
    ASSERT_TRUE(params.PrepareBinding(&prepared));
    bytes = index.StorageBytes();
    ASSERT_TRUE(params.BindVoiceTarget(variable, kVoice1, harken::kTargetVoiceVolume, curve, &prepared));
    EXPECT_EQ(bytes, index.StorageBytes());
}

TEST(ParamCombine, IndexedActiveAndLogicalPreparationUseSeparateBounds) {
    auto config = DefaultConfig();
    ASSERT_TRUE(harken::VoiceCapacityModel::TryCreate(1, 1, &config.voiceCapacity));
    config.voiceBindingInstanceSlots = 1;
    harken::BindingDependencyIndex index;
    ParamSystem params(config);
    EXPECT_EQ(1u, params.BindingCapacity());
    EXPECT_EQ(3u, params.PreparationCapacity());
    ASSERT_TRUE(index.Reset(params.BindingCapacity(), 3));
    EXPECT_FALSE(params.AttachBindingDependencies(&index));
    AttachIndex(params, index);
    params.OnVoiceStarted(kVoice0, 0, 1);
    const auto variable = harkentest::ObjectId(7);
    const Curve curve = LinearCurve(0, 0, 1, -6);
    ASSERT_TRUE(params.BindVoiceTarget(variable, kVoice0, harken::kTargetVoiceVolume, curve));
    ParamSystem::PreparedBindingToken pending, parked, excess;
    ASSERT_TRUE(params.PrepareBinding(&pending));
    ASSERT_TRUE(params.PrepareBinding(&parked));
    EXPECT_FALSE(params.PrepareBinding(&excess));
    const auto bytes = index.StorageBytes();
    EXPECT_FALSE(params.BindVoiceTarget(variable, kVoice1, harken::kTargetVoiceVolume, curve, &pending, true));
    EXPECT_TRUE(pending.IsPrepared());
    EXPECT_EQ(0u, params.VoiceBindingCount(kVoice1));
    EXPECT_EQ(1u, params.BindingCapacity());
    EXPECT_EQ(bytes, index.StorageBytes());
    params.CancelPreparedBinding(&pending);
    ASSERT_TRUE(params.PrepareBinding(&excess));
    EXPECT_EQ(bytes, index.StorageBytes());
}

TEST(ParamCombine, IndexedStagedRetryKeepsAcceptedBackingUntilFinalCommit) {
    static_assert(noexcept(std::declval<ParamSystem&>().CommitStagedVoiceBindings(kVoice0)));
    auto config = DefaultConfig();
    ASSERT_TRUE(harken::VoiceCapacityModel::TryCreate(1, 1, &config.voiceCapacity));
    config.voiceBindingInstanceSlots = 2;
    harken::BindingDependencyIndex index;
    ParamSystem params(config);
    AttachIndex(params, index);
    const auto variable = harkentest::ObjectId(7);
    const Curve raw = LinearCurve(0, 0, 1, -6);
    const Curve instance = LinearCurve(0, 0, 1, -3);
    const harken::BindingDefinitionView view{harkentest::ObjectId(100), variable,
        harken::kTargetVoiceVolume, &instance, nullptr, nullptr, true};
    ASSERT_EQ(VN_OK, params.SetInstalledValue(variable, 1));
    ParamSystem::PreparedBindingToken rawToken, instanceToken;
    ASSERT_TRUE(params.PrepareBinding(&rawToken));
    ASSERT_TRUE(params.PrepareBinding(&instanceToken));
    const auto bytes = index.StorageBytes();
    for (std::uint32_t generation = 1; generation <= 8; ++generation) {
        const VnVoiceHandle render = static_cast<VnVoiceHandle>(generation) << 32u;
        params.OnVoiceStarted(render, -3, 1);
        ASSERT_TRUE(params.BindVoiceTarget(variable, render, harken::kTargetVoiceVolume, raw, &rawToken, true));
        ASSERT_EQ(ParamSystem::InstanceBindResult::Ok,
            params.BindVoiceInstance(view, render, {}, true, &instanceToken, true));
        EXPECT_TRUE(rawToken.IsPrepared());
        EXPECT_TRUE(instanceToken.IsPrepared());
        float volume = 0;
        ASSERT_TRUE(params.TryPeekResolvedVolume(render, nullptr, &volume));
        EXPECT_FLOAT_EQ(-12, volume);
        params.EvaluateDirtyBindings(nullptr);
        EXPECT_EQ(0u, DirtyDependencyCount(index));
        params.DiscardPreparedVoice(render);
        EXPECT_TRUE(rawToken.IsPrepared());
        EXPECT_TRUE(instanceToken.IsPrepared());
        EXPECT_EQ(0u, params.VoiceBindingCount(render));
        EXPECT_EQ(0u, DirtyDependencyCount(index));
        EXPECT_EQ(bytes, index.StorageBytes());
    }
    constexpr VnVoiceHandle committed = 9ULL << 32u;
    params.OnVoiceStarted(committed, -3, 1);
    ASSERT_TRUE(params.BindVoiceTarget(variable, committed, harken::kTargetVoiceVolume, raw, &rawToken, true));
    ASSERT_EQ(ParamSystem::InstanceBindResult::Ok,
        params.BindVoiceInstance(view, committed, {}, true, &instanceToken, true));
    params.CommitStagedVoiceBindings(committed);
    EXPECT_FALSE(rawToken.IsPrepared());
    EXPECT_FALSE(instanceToken.IsPrepared());
    EXPECT_EQ(bytes, index.StorageBytes());
    EXPECT_EQ(1u, DirtyDependencyCount(index));
    params.EvaluateDirtyBindings(nullptr);
    EXPECT_FLOAT_EQ(-12, params.ResolvedValue(committed, harken::kTargetVoiceVolume));
    EXPECT_EQ(0u, DirtyDependencyCount(index));
}

TEST(ParamCombine, IndexedStagedAssociationsSurviveCompactionAndPrivateCopy) {
    auto config = DefaultConfig();
    ASSERT_TRUE(harken::VoiceCapacityModel::TryCreate(2, 0, &config.voiceCapacity));
    config.voiceBindingInstanceSlots = 2;
    harken::BindingDependencyIndex index;
    ParamSystem params(config);
    AttachIndex(params, index);
    const auto variable = harkentest::ObjectId(7);
    const Curve curve = LinearCurve(0, 0, 1, -3);
    harken::BindingDefinitionView view{harkentest::ObjectId(100), variable,
        harken::kTargetVoiceVolume, &curve, nullptr, nullptr, true};
    params.OnVoiceStarted(kVoice0, 0, 1);
    params.OnVoiceStarted(kVoice1, 0, 1);
    ASSERT_EQ(ParamSystem::InstanceBindResult::Ok, params.BindVoiceInstance(view, kVoice0, {}, true));
    ParamSystem::PreparedBindingToken rawToken, instanceToken;
    ASSERT_TRUE(params.PrepareBinding(&rawToken));
    ASSERT_TRUE(params.PrepareBinding(&instanceToken));
    ASSERT_TRUE(params.BindVoiceTarget(variable, kVoice1, harken::kTargetVoiceVolume, curve, &rawToken, true));
    view.definitionId = harkentest::ObjectId(101);
    ASSERT_EQ(ParamSystem::InstanceBindResult::Ok,
        params.BindVoiceInstance(view, kVoice1, {}, true, &instanceToken, true));
    params.PurgeInstancesOf(harkentest::ObjectId(100));
    ParamSystem copy(params);
    copy.DiscardPreparedVoice(kVoice1);
    EXPECT_TRUE(rawToken.IsPrepared());
    EXPECT_TRUE(instanceToken.IsPrepared());
    const auto bytes = index.StorageBytes();
    params.CommitStagedVoiceBindings(kVoice1);
    EXPECT_FALSE(rawToken.IsPrepared());
    EXPECT_FALSE(instanceToken.IsPrepared());
    EXPECT_EQ(bytes, index.StorageBytes());
    ASSERT_EQ(VN_OK, params.SetInstalledValue(variable, 1));
    params.EvaluateDirtyBindings(nullptr);
    EXPECT_FLOAT_EQ(-6, params.ResolvedValue(kVoice1, harken::kTargetVoiceVolume));
    EXPECT_EQ(0u, DirtyDependencyCount(index));
}

TEST(ParamCombine, IndexedOwnedStagingRetriesFromPhysicalBackingWithoutGrowth) {
    auto config = DefaultConfig();
    ASSERT_TRUE(harken::VoiceCapacityModel::TryCreate(1, 1, &config.voiceCapacity));
    config.voiceBindingInstanceSlots = 2;
    harken::BindingDependencyIndex index;
    ParamSystem params(config);
    AttachIndex(params, index);
    ParamSystem::BackingPreparation backing;
    ASSERT_TRUE(params.PrepareBindingBacking(2, &backing));
    const Curve curve = LinearCurve(0, 0, 1, -3);
    harken::BindingDefinitionView view{harkentest::ObjectId(100), harkentest::ObjectId(7),
        harken::kTargetVoiceVolume, &curve, nullptr, nullptr, true};
    ASSERT_EQ(VN_OK, params.SetInstalledValue(view.variableId, 1));
    const auto bytes = index.StorageBytes();
    const auto bindingBytes = params.BindingStorageBytes();
    for (std::uint32_t generation = 1; generation <= 8; ++generation) {
        ParamSystem::PreparedUse use(params, &backing);
        const VnVoiceHandle render = static_cast<VnVoiceHandle>(generation) << 32u;
        params.OnVoiceStarted(render, 0, 1);
        view.definitionId = harkentest::ObjectId(100);
        ASSERT_EQ(ParamSystem::InstanceBindResult::Ok, params.StageOwnedVoiceInstance(view, render, {}, true));
        EXPECT_EQ(ParamSystem::InstanceBindResult::Duplicate, params.StageOwnedVoiceInstance(view, render, {}, true));
        view.definitionId = harkentest::ObjectId(101);
        ASSERT_EQ(ParamSystem::InstanceBindResult::Ok, params.StageOwnedVoiceInstance(view, render, {}, true));
        EXPECT_EQ(0u, DirtyDependencyCount(index));
        float resolved = 0;
        ASSERT_TRUE(params.TryPeekResolvedVolume(render, nullptr, &resolved));
        EXPECT_FLOAT_EQ(-6, resolved);
        params.DiscardPreparedVoice(render);
        EXPECT_EQ(0u, DirtyDependencyCount(index));
        EXPECT_EQ(bytes, index.StorageBytes());
        EXPECT_EQ(bindingBytes, params.BindingStorageBytes());
    }
    constexpr VnVoiceHandle committed = 9ULL << 32u;
    {
        ParamSystem::PreparedUse use(params, &backing);
        params.OnVoiceStarted(committed, 0, 1);
        ASSERT_EQ(ParamSystem::InstanceBindResult::Ok, params.StageOwnedVoiceInstance(view, committed, {}, true));
    }
    EXPECT_EQ(0u, DirtyDependencyCount(index));
    params.CommitStagedVoiceBindings(committed);
    EXPECT_EQ(bytes, index.StorageBytes());
    params.EvaluateDirtyBindings(nullptr);
    EXPECT_FLOAT_EQ(-3, params.ResolvedValue(committed, harken::kTargetVoiceVolume));
}

TEST(ParamCombine, IndexedPhysicalHoldsProtectSupplyFromOrdinaryGrowthWithoutReservingIdentities) {
    auto config = DefaultConfig();
    ASSERT_TRUE(harken::VoiceCapacityModel::TryCreate(4, 0, &config.voiceCapacity));
    config.voiceBindingInstanceSlots = 1;
    harken::BindingDependencyIndex index;
    ParamSystem params(config);
    AttachIndex(params, index);
    ParamSystem::BackingPreparation first, second;
    ASSERT_TRUE(params.PrepareBindingBacking(1, &first));
    ASSERT_TRUE(params.PrepareBindingBacking(1, &second));
    ParamSystem::BackingPreparation moved(std::move(second));
    EXPECT_FALSE(second.Valid());
    const Curve curve = LinearCurve(0, 0, 1, -3);
    harken::BindingDefinitionView view{harkentest::ObjectId(100), harkentest::ObjectId(7),
        harken::kTargetVoiceVolume, &curve, nullptr, nullptr, true};
    for (std::uint32_t slot = 0; slot < 2; ++slot) {
        const auto render = (1ULL << 32u) | slot;
        params.OnVoiceStarted(render, 0, 1);
        ASSERT_EQ(ParamSystem::InstanceBindResult::Ok, params.BindVoiceInstance(view, render, {}, true));
    }
    const auto bytes = index.StorageBytes();
    for (std::uint32_t slot = 2; slot < 4; ++slot) {
        ParamSystem::PreparedUse use(params, slot == 2 ? &first : &moved);
        const auto render = (1ULL << 32u) | slot;
        params.OnVoiceStarted(render, 0, 1);
        ASSERT_EQ(ParamSystem::InstanceBindResult::Ok, params.StageOwnedVoiceInstance(view, render, {}, true));
        params.CommitStagedVoiceBindings(render);
        EXPECT_EQ(bytes, index.StorageBytes());
    }
    for (std::uint32_t slot = 0; slot < 4; ++slot)
        EXPECT_EQ(1u, params.VoiceBindingCount((1ULL << 32u) | slot));
    first.Release();
    moved.Release();
    ASSERT_TRUE(params.PrepareBindingBacking(UINT32_MAX, &first));
    ASSERT_TRUE(params.PrepareBindingBacking(UINT32_MAX, &moved));
    for (std::uint32_t slot = 0; slot < 4; ++slot)
        EXPECT_EQ(1u, params.VoiceBindingCount((1ULL << 32u) | slot));
}

TEST(ParamCombine, IndexedOwnedStagingCompactionAndPrivateCopyRetainOriginalReservation) {
    auto config = DefaultConfig();
    ASSERT_TRUE(harken::VoiceCapacityModel::TryCreate(2, 0, &config.voiceCapacity));
    config.voiceBindingInstanceSlots = 1;
    harken::BindingDependencyIndex index;
    ParamSystem params(config);
    AttachIndex(params, index);
    ParamSystem::BackingPreparation backing;
    ASSERT_TRUE(params.PrepareBindingBacking(2, &backing));
    const Curve curve = LinearCurve(0, 0, 1, -3);
    harken::BindingDefinitionView view{harkentest::ObjectId(100), harkentest::ObjectId(7),
        harken::kTargetVoiceVolume, &curve, nullptr, nullptr, true};
    {
        ParamSystem::PreparedUse use(params, &backing);
        params.OnVoiceStarted(kVoice0, 0, 1);
        params.OnVoiceStarted(kVoice1, 0, 1);
        ASSERT_EQ(ParamSystem::InstanceBindResult::Ok, params.StageOwnedVoiceInstance(view, kVoice0, {}, true));
        view.definitionId = harkentest::ObjectId(101);
        ASSERT_EQ(ParamSystem::InstanceBindResult::Ok, params.StageOwnedVoiceInstance(view, kVoice1, {}, true));
        params.PurgeInstancesOf(harkentest::ObjectId(100));
    }
    ParamSystem copy(params);
    copy.DiscardPreparedVoice(kVoice1);
    params.CommitStagedVoiceBindings(kVoice1);
    ASSERT_EQ(VN_OK, params.SetInstalledValue(view.variableId, 1));
    params.EvaluateDirtyBindings(nullptr);
    EXPECT_FLOAT_EQ(-3, params.ResolvedValue(kVoice1, harken::kTargetVoiceVolume));
    EXPECT_EQ(0u, DirtyDependencyCount(index));
}

TEST(ParamCombine, IndexedBackingPreservesLateAttachmentPartitionAndFailedUse) {
    auto config = DefaultConfig();
    ASSERT_TRUE(harken::VoiceCapacityModel::TryCreate(1, 1, &config.voiceCapacity));
    config.voiceBindingInstanceSlots = 1;
    harken::BindingTable undersized(2, 4, 2, 1);
    harken::BindingTable bindings(2, 4, 2, 3);
    ParamSystem params(config);
    EXPECT_FALSE(params.AttachBindingDependencies(&undersized.Dependencies()));
    ASSERT_TRUE(params.AttachBindingDependencies(&bindings.Dependencies()));
    ParamSystem::BackingPreparation backing;
    ASSERT_TRUE(params.PrepareBindingBacking(1, &backing));
    EXPECT_FALSE(params.AttachBindingDependencies(nullptr));
    auto& index = bindings.Dependencies();
    const auto bytes = index.StorageBytes();
    {
        ParamSystem::PreparedUse use(params, &backing);
        EXPECT_FALSE(index.PrepareBinding(2));
        ParamSystem::PreparedBindingToken token;
        ASSERT_TRUE(params.PrepareBinding(&token));
        EXPECT_EQ(bytes, index.StorageBytes());
        EXPECT_FALSE(index.Reset(20, 20));
    }
    backing.Release();
    ASSERT_TRUE(index.PrepareBinding(2));
    index.CancelBindingPreparation(2);
    ASSERT_TRUE(params.AttachBindingDependencies(nullptr));
}

TEST(ParamCombine, IndexedCompactionAndAppendRetainFloatingPointContributorOrder) {
    harken::BindingDependencyIndex index;
    ParamSystem params(DefaultConfig());
    AttachIndex(params, index);
    params.OnVoiceStarted(kVoice0, 0, 1);
    const auto variable = harkentest::ObjectId(7);
    const Curve curves[] = {LinearCurve(0, 0, 1, 0), LinearCurve(0, 16777216, 1, 16777216),
        LinearCurve(0, 1, 1, 1), LinearCurve(0, -16777216, 1, -16777216)};
    for (std::uint32_t i = 0; i < std::size(curves); ++i) {
        const harken::BindingDefinitionView view{harkentest::ObjectId(100 + i), variable,
            harken::kTargetVoicePitch, &curves[i], nullptr, nullptr, true};
        ASSERT_EQ(ParamSystem::InstanceBindResult::Ok,
            params.BindVoiceInstance(view, kVoice0, {}, true));
    }
    ASSERT_EQ(VN_OK, params.SetInstalledValue(variable, 1));
    FlushAll(params);
    EXPECT_FLOAT_EQ(0, params.ResolvedValue(kVoice0, harken::kTargetVoicePitch));
    params.PurgeInstancesOf(harkentest::ObjectId(100));
    ASSERT_TRUE(params.BindVoiceTarget(variable, kVoice0, harken::kTargetVoicePitch, LinearCurve(0, 1, 1, 1)));
    const auto commits = FlushAll(params);
    ASSERT_EQ(1u, commits.size());
    EXPECT_FLOAT_EQ(1, commits[0].resolved);
    EXPECT_EQ(0u, DirtyDependencyCount(index));
}

TEST(ParamCombine, IndexedLocalFallbackAndLiveDefinitionKeepExactSubscriptions) {
    harken::BindingDependencyIndex index;
    ParamSystem params(DefaultConfig());
    harken::EmitterTable emitters(DefaultConfig());
    AttachIndex(params, index);
    VnEmitterHandle emitter = 0;
    ASSERT_TRUE(emitters.Create(&emitter));
    params.OnVoiceStarted(kVoice0, 0, 1, emitter);
    params.OnVoiceStarted(kVoice1, 0, 1);
    const auto variable = harkentest::ObjectId(7);
    Curve curve = LinearCurve(0, 0, 1, -6);
    std::uint32_t revision = 1;
    harken::BindingDefinitionView view{harkentest::ObjectId(100), variable,
        harken::kTargetVoiceVolume, &curve, nullptr, &revision, false};
    ASSERT_EQ(ParamSystem::InstanceBindResult::Ok, params.BindVoiceInstance(view, kVoice0, {}, true));
    view.snapshotOwned = true;
    ASSERT_EQ(ParamSystem::InstanceBindResult::Ok, params.BindVoiceInstance(view, kVoice1, {}, true));
    ASSERT_EQ(VN_OK, emitters.SetLocalValue(emitter, variable, 0));
    ASSERT_EQ(VN_OK, params.SetInstalledValue(variable, 1));
    params.EvaluateDirtyBindings(&emitters);
    EXPECT_FLOAT_EQ(0, params.ResolvedValue(kVoice0, harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(-6, params.ResolvedValue(kVoice1, harken::kTargetVoiceVolume));
    ASSERT_EQ(VN_OK, params.SetInstalledValue(variable, 0));
    EXPECT_EQ(2u, DirtyDependencyCount(index));
    params.EvaluateDirtyBindings(&emitters);
    EXPECT_FLOAT_EQ(0, params.ResolvedValue(kVoice1, harken::kTargetVoiceVolume));
    ASSERT_EQ(VN_OK, emitters.SetLocalValue(emitter, variable, 1));
    params.NoteVariableWrite(variable, emitter);
    ASSERT_EQ(1u, DirtyDependencyCount(index));
    harken::BindingDirtyTarget dirty;
    harken::BindingDependencyIdentity identity;
    ASSERT_TRUE(index.DirtyTargetAt(0, &dirty));
    ASSERT_TRUE(index.TargetIdentity(dirty, &identity));
    EXPECT_EQ(kVoice0, identity.object);
    params.EvaluateDirtyBindings(&emitters);
    EXPECT_FLOAT_EQ(-6, params.ResolvedValue(kVoice0, harken::kTargetVoiceVolume));
    curve = LinearCurve(0, 0, 1, -12);
    ++revision;
    params.NoteDefinitionReplaced(view.definitionId);
    EXPECT_EQ(1u, DirtyDependencyCount(index));
    params.EvaluateDirtyBindings(&emitters);
    EXPECT_FLOAT_EQ(-12, params.ResolvedValue(kVoice0, harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(0, params.ResolvedValue(kVoice1, harken::kTargetVoiceVolume));
    ASSERT_EQ(VN_OK, params.SetInstalledValue(variable, 1));
    params.EvaluateDirtyBindings(&emitters);
    EXPECT_FLOAT_EQ(-6, params.ResolvedValue(kVoice1, harken::kTargetVoiceVolume));
    ASSERT_TRUE(emitters.Destroy(emitter));
    params.NoteEmitterDetached(emitter);
    params.EvaluateDirtyBindings(&emitters);
    params.NoteVariableWrite(variable, emitter);
    EXPECT_EQ(0u, DirtyDependencyCount(index));
    params.ClearInstalledValues();
    EXPECT_EQ(2u, DirtyDependencyCount(index));
    params.EvaluateDirtyBindings(&emitters);
    EXPECT_FLOAT_EQ(0, params.ResolvedValue(kVoice0, harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(0, params.ResolvedValue(kVoice1, harken::kTargetVoiceVolume));
}

TEST(ParamCombine, IndexedPrivateCopyAndCompactionOwnTheirPendingExits) {
    harken::BindingDependencyIndex index;
    ParamSystem params(DefaultConfig());
    AttachIndex(params, index);
    params.OnVoiceStarted(kVoice0, 0, 1);
    const auto variable = harkentest::ObjectId(7);
    const Curve curves[] = {LinearCurve(0, 0, 1, -6), LinearCurve(0, 0, 1, -3)};
    const std::uint32_t ramp = 17;
    for (std::uint32_t i = 0; i < std::size(curves); ++i) {
        const harken::BindingDefinitionView view{harkentest::ObjectId(100 + i), variable,
            harken::kTargetVoiceVolume, &curves[i], &ramp, nullptr, true};
        ASSERT_EQ(ParamSystem::InstanceBindResult::Ok, params.BindVoiceInstance(view, kVoice0, {}, true));
    }
    ASSERT_EQ(VN_OK, params.SetInstalledValue(variable, 1));
    FlushAll(params);
    ASSERT_TRUE(params.NeutralizeVoiceInstance(kVoice0, harkentest::ObjectId(101)));
    harken::BindingDirtyTarget dirty;
    ASSERT_TRUE(index.DirtyTargetAt(0, &dirty));
    const auto version = index.TargetVersion(dirty);
    ParamSystem copy(params);
    copy.PurgeInstancesOf(harkentest::ObjectId(100));
    FlushAll(copy);
    EXPECT_EQ(version, index.TargetVersion(dirty));
    EXPECT_FLOAT_EQ(-9, params.ResolvedValue(kVoice0, harken::kTargetVoiceVolume));
    params.PurgeInstancesOf(harkentest::ObjectId(100));
    const auto commits = FlushAll(params);
    ASSERT_EQ(1u, commits.size());
    EXPECT_FLOAT_EQ(0, commits[0].resolved);
    EXPECT_EQ(ramp, commits[0].rampMs);
    std::uint32_t captured = 0;
    params.ForEachInstance([&](const ParamSystem::InstanceCaptureRow& row) {
        ++captured;
        EXPECT_TRUE(row.neutralized);
        EXPECT_FLOAT_EQ(0, row.lastContribution);
    });
    EXPECT_EQ(1u, captured);
    params.NoteVariableWrite(variable);
    EXPECT_EQ(0u, DirtyDependencyCount(index));
}

TEST(ParamCombine, IndexedEvaluationLeavesOtherOwnersAndRetiredGenerationsIsolated) {
    ParamSystem params(DefaultConfig());
    harken::BindingTable bindings(2, 2, 2, params.PreparationCapacity(), params.PreparationCapacity() + 2u);
    auto& index = bindings.Dependencies();
    ASSERT_TRUE(params.AttachBindingDependencies(&index));
    harken::BusTargetStore buses(2);
    params.OnVoiceStarted(kVoice0, 0, 1);
    const auto variable = harkentest::ObjectId(7);
    ASSERT_TRUE(params.BindVoiceTarget(variable, kVoice0, harken::kTargetVoiceVolume, LinearCurve(0, 0, 1, -6)));
    const CurvePoint points[] = {{0, 0}, {1, -3}};
    const harken::CurveHandlePair handles[] = {{0, 0, 1, -3}};
    ASSERT_EQ(VN_OK, bindings.RegisterBinding(harkentest::ObjectId(100), variable,
        harken::kTargetScopeBus, harken::kBusTargetBusVolume, points, handles, 2, 0, nullptr));
    ASSERT_EQ(VN_OK, bindings.Attach(harkentest::ObjectId(100), harken::kTargetScopeBus,
        harkentest::ObjectId(200), 0, nullptr));
    ASSERT_EQ(VN_OK, params.SetInstalledValue(variable, 1));
    params.EvaluateDirtyBindings(nullptr);
    EXPECT_EQ(1u, DirtyDependencyCount(index));
    bindings.EvaluateDirtyBusAttachments(params, buses);
    EXPECT_EQ(0u, DirtyDependencyCount(index));
    EXPECT_FLOAT_EQ(-3, buses.Reduce(0, harken::kBusTargetBusVolume));
    params.RetireVoiceTargets(kVoice0);
    params.EvaluateDirtyBindings(nullptr);
    constexpr VnVoiceHandle replacement = (2ULL << 32) | 0u;
    params.OnVoiceStarted(replacement, -3, 1);
    ASSERT_TRUE(params.BindVoiceTarget(variable, replacement,
        harken::kTargetVoiceVolume, LinearCurve(0, 0, 1, -6)));
    params.EvaluateDirtyBindings(nullptr);
    EXPECT_FLOAT_EQ(-9, params.ResolvedValue(replacement, harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(0, params.ResolvedValue(kVoice0, harken::kTargetVoiceVolume));
    EXPECT_EQ(0u, DirtyDependencyCount(index));
}

TEST(ParamCombine, CapturedMissingVolumeClearsThePreviousVariableContribution) {
    ParamSystem params(DefaultConfig());
    params.OnVoiceStarted(kVoice0, -3, 1);
    ASSERT_TRUE(params.BindVoiceTarget(harkentest::ObjectId(7), kVoice0,
        harken::kTargetVoiceVolume, LinearCurve(0, 0, 1, -12)));
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(7), 1));
    FlushAll(params);
    ASSERT_FLOAT_EQ(-15, params.ResolvedValue(kVoice0, harken::kTargetVoiceVolume));
    const harken::VariableReadCutReader reader{nullptr, {},
        [](void*, harken::VariableReadCutRef, VnObjectId, float*) noexcept {
            return harken::VariableReadCutValueStatus::Missing;
        }};
    float peek = 0;
    ASSERT_TRUE(params.TryPeekResolvedVolume(kVoice0, nullptr, &peek, &reader));
    EXPECT_FLOAT_EQ(-3, peek);
    harken::ParamSystem::PrimedValue primed[harken::kBuiltinTargetCount]{};
    std::uint32_t written = 0;
    ASSERT_TRUE(params.TryPrimeVoiceBindings(kVoice0, reader, primed, std::size(primed), &written));
    EXPECT_FLOAT_EQ(peek, params.ResolvedValue(kVoice0, harken::kTargetVoiceVolume));
}

TEST(ParamCombine, CapturedPrimingReadsEachInputOnceBeforeChangingAnyTarget) {
    ParamSystem params(DefaultConfig());
    params.OnVoiceStarted(kVoice0, 0, 1);
    const auto variable = harkentest::ObjectId(7);
    ASSERT_TRUE(params.BindVoiceTarget(variable, kVoice0, harken::kTargetVoiceVolume,
        LinearCurve(0, 0, 1, -12)));
    ASSERT_TRUE(params.BindVoiceTarget(variable, kVoice0, harken::kTargetVoicePitch,
        LinearCurve(0, 0, 1, 1200)));
    struct Inputs { std::uint32_t reads = 0; } inputs;
    const harken::VariableReadCutReader reader{&inputs, {},
        [](void* context, harken::VariableReadCutRef, VnObjectId, float* out) noexcept {
            auto& inputs = *static_cast<Inputs*>(context);
            if (++inputs.reads > 2) return harken::VariableReadCutValueStatus::Failed;
            *out = 1;
            return harken::VariableReadCutValueStatus::Value;
        }};
    harken::ParamSystem::PrimedValue primed[harken::kBuiltinTargetCount]{};
    std::uint32_t written = 0;
    ASSERT_TRUE(params.TryPrimeVoiceBindings(kVoice0, reader, primed, std::size(primed), &written));
    EXPECT_EQ(2u, inputs.reads);
    EXPECT_EQ(2u, written);
    EXPECT_FLOAT_EQ(-12, params.LastCommitted(kVoice0, harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(1200, params.LastCommitted(kVoice0, harken::kTargetVoicePitch));
}

TEST(ParamCombine, VolumeAddsInDbAcrossBaseApiDirectAndBinding) {
    ParamSystem params(DefaultConfig());
    params.OnVoiceStarted(kVoice0, -6.0f, 1.0);

    // No contributions: the resolved value IS the authored base.
    EXPECT_FLOAT_EQ(-6.0f, params.ResolvedValue(
                               kVoice0, harken::kTargetVoiceVolume));

    ASSERT_TRUE(params.BindVoiceTarget(
        harkentest::ObjectId(7), kVoice0, harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(7), 0.5f));
    const std::vector<ParamCommit> commits = FlushAll(params);
    ASSERT_EQ(1u, commits.size());
    const float contribution = 20.0f * std::log10((1.0f + std::pow(10.0f, -12.0f / 20.0f)) / 2.0f);
    EXPECT_NEAR(-6.0f + contribution, commits[0].resolved, 0.0001f);

    const float resolved =
        params.SetApiDirect(kVoice0, harken::kTargetVoiceVolume, 2.0f);
    EXPECT_NEAR(-4.0f + contribution, resolved, 0.0001f);
    EXPECT_NEAR(-4.0f + contribution, params.ResolvedValue(
                                kVoice0, harken::kTargetVoiceVolume), 0.0001f);
}

TEST(ParamCombine, CrossZeroLargeDecibelsProduceFiniteBindingContributions) {
    for (float direction : {-1.0f, 1.0f}) {
        ParamSystem params(DefaultConfig());
        params.OnVoiceStarted(kVoice0, 0.0f, 1.0);
        const CurvePoint points[] = {{0, direction * 400}, {1, -direction * 400}};
        const harken::CurveHandlePair handles[] = {{0.25f, points[0].y, 0.75f, points[0].y}};
        Curve curve;
        ASSERT_TRUE(curve.SetPoints(points, handles, 2));
        ASSERT_TRUE(params.BindVoiceTarget(
            harkentest::ObjectId(7), kVoice0, harken::kTargetVoiceVolume, curve));
        ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(7), 1e-8f));
        const auto commits = FlushAll(params);
        ASSERT_EQ(commits.size(), 1u);
        EXPECT_TRUE(std::isfinite(commits[0].resolved));
        EXPECT_NEAR(commits[0].resolved, direction * 400, 0.01f);
        EXPECT_NEAR(params.ResolvedValue(kVoice0, harken::kTargetVoiceVolume),
                    direction * 400, 0.01f);
    }
}

TEST(ParamCombine,
     SliceCommitRearmsDivergentTargetWithoutDroppingPendingRamp) {
    ParamSystem params(DefaultConfig());
    params.OnVoiceStarted(kVoice0, 0.0f, 1.0);
    const harken::VariableSliceRef owner{0, 1};
    const std::uint32_t revision = params.TargetRevision(
        kVoice0, harken::kTargetVoiceVolume);
    ASSERT_TRUE(params.TryClaim(kVoice0, harken::kTargetVoiceVolume, owner,
                                revision));
    constexpr std::uint32_t kSliceRampMs = 17;
    params.SetAttenuationSlot(kVoice0, harken::kTargetVoiceVolume, -3.0f,
                              kSliceRampMs);
    float frozenResolved = 0.0f;
    std::uint32_t frozenRampMs = 0;
    std::uint32_t frozenRevision = 0;
    ASSERT_TRUE(params.CaptureSnapshotForClaim(
        kVoice0, harken::kTargetVoiceVolume, owner, &frozenResolved,
        &frozenRampMs, &frozenRevision));
    EXPECT_FLOAT_EQ(-3.0f, frozenResolved);
    EXPECT_EQ(kSliceRampMs, frozenRampMs);
    EXPECT_EQ(revision, frozenRevision);

    constexpr std::uint32_t kPendingRampMs = 73;
    params.SetAttenuationSlot(kVoice0, harken::kTargetVoiceVolume, 0.0f,
                              kPendingRampMs);
    EXPECT_FLOAT_EQ(0.0f, params.ResolvedValue(
                               kVoice0, harken::kTargetVoiceVolume));
    EXPECT_EQ(kPendingRampMs,
              params.PendingRampMs(kVoice0,
                                   harken::kTargetVoiceVolume));
    std::uint32_t ordinaryFlushCalls = 0;
    params.FlushCommits([&ordinaryFlushCalls](const ParamCommit&) {
        ++ordinaryFlushCalls;
        return harken::FlushOutcome::Retry;
    });
    EXPECT_EQ(0u, ordinaryFlushCalls);
    EXPECT_TRUE(params.ForeignClaim(kVoice0,
                                    harken::kTargetVoiceVolume));
    EXPECT_TRUE(params.PendingPublication(kVoice0,
                                          harken::kTargetVoiceVolume));
    EXPECT_EQ(kPendingRampMs,
              params.PendingRampMs(kVoice0,
                                   harken::kTargetVoiceVolume));

    params.MarkSliceCommitted(kVoice0, harken::kTargetVoiceVolume, owner,
                              frozenResolved, frozenRampMs);
    EXPECT_FALSE(params.ForeignClaim(kVoice0,
                                     harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(frozenResolved,
                    params.LastCommitted(kVoice0,
                                         harken::kTargetVoiceVolume));
    EXPECT_TRUE(params.PendingPublication(kVoice0,
                                          harken::kTargetVoiceVolume));
    EXPECT_EQ(kPendingRampMs,
              params.PendingRampMs(kVoice0,
                                   harken::kTargetVoiceVolume));

    const std::vector<ParamCommit> commits = FlushAll(params);
    ASSERT_EQ(1u, commits.size());
    EXPECT_FLOAT_EQ(0.0f, commits[0].resolved);
    EXPECT_EQ(kPendingRampMs, commits[0].rampMs);
}

TEST(ParamCombine,
     SliceSnapshotConsumesOnlyItsRampAndClaimReleaseRestoresIt) {
    ParamSystem params(DefaultConfig());
    params.OnVoiceStarted(kVoice0, 0.0f, 1.0);
    constexpr std::uint32_t kTarget = harken::kTargetVoiceVolume;
    const harken::VariableSliceRef appliedOwner{0, 1};
    const std::uint32_t revision =
        params.TargetRevision(kVoice0, kTarget);
    ASSERT_TRUE(params.TryClaim(kVoice0, kTarget, appliedOwner, revision));
    params.SetAttenuationSlot(kVoice0, kTarget, -3.0f, 73);
    float frozenResolved = 0.0f;
    std::uint32_t frozenRampMs = 0;
    std::uint32_t frozenRevision = 0;
    ASSERT_TRUE(params.CaptureSnapshotForClaim(
        kVoice0, kTarget, appliedOwner, &frozenResolved, &frozenRampMs,
        &frozenRevision));
    EXPECT_FLOAT_EQ(-3.0f, frozenResolved);
    EXPECT_EQ(73u, frozenRampMs);
    params.SetAttenuationSlot(kVoice0, kTarget, 0.0f, 17);
    params.MarkSliceCommitted(kVoice0, kTarget, appliedOwner,
                              frozenResolved, frozenRampMs);
    std::vector<ParamCommit> commits = FlushAll(params);
    ASSERT_EQ(1u, commits.size());
    EXPECT_FLOAT_EQ(0.0f, commits[0].resolved);
    EXPECT_EQ(17u, commits[0].rampMs);

    const harken::VariableSliceRef abandonedOwner{1, 1};
    ASSERT_TRUE(params.TryClaim(kVoice0, kTarget, abandonedOwner, revision));
    params.SetAttenuationSlot(kVoice0, kTarget, -6.0f, 73);
    ASSERT_TRUE(params.CaptureSnapshotForClaim(
        kVoice0, kTarget, abandonedOwner, &frozenResolved, &frozenRampMs,
        &frozenRevision));
    params.SetAttenuationSlot(kVoice0, kTarget, -3.0f, 17);
    params.ReleaseClaim(kVoice0, kTarget, abandonedOwner);
    commits = FlushAll(params);
    ASSERT_EQ(1u, commits.size());
    EXPECT_FLOAT_EQ(-3.0f, commits[0].resolved);
    EXPECT_EQ(73u, commits[0].rampMs);
}

TEST(ParamCombine,
     ClaimInternalBankSyncDoesNotReintroduceTheFrozenRamp) {
    ParamSystem params(DefaultConfig());
    params.OnVoiceStarted(kVoice0, 0.0f, 1.0);
    constexpr std::uint32_t kTarget = harken::kTargetVoiceVolume;
    const harken::VariableSliceRef owner{0, 1};
    const std::uint32_t revision =
        params.TargetRevision(kVoice0, kTarget);
    ASSERT_TRUE(params.TryClaim(kVoice0, kTarget, owner, revision));
    params.SetAttenuationSlot(kVoice0, kTarget, -3.0f, 73);
    float frozenResolved = 0.0f;
    std::uint32_t frozenRampMs = 0;
    std::uint32_t frozenRevision = 0;
    ASSERT_TRUE(params.CaptureSnapshotForClaim(
        kVoice0, kTarget, owner, &frozenResolved, &frozenRampMs,
        &frozenRevision));
    EXPECT_FLOAT_EQ(-3.0f, frozenResolved);
    EXPECT_EQ(73u, frozenRampMs);
    EXPECT_FALSE(params.CaptureSnapshotForClaim(
        kVoice0, kTarget, owner, &frozenResolved, &frozenRampMs,
        &frozenRevision));

    params.SetClaimRampSuppressed(kVoice0, kTarget, owner, true);
    params.SetAttenuationSlot(kVoice0, kTarget, 0.0f, 73);
    params.SetClaimRampSuppressed(kVoice0, kTarget, owner, false);
    params.SetMeterProbeContribution(kVoice0, harkentest::ObjectId(91), kTarget, -1.0f, 17);
    params.SetClaimRampSuppressed(kVoice0, kTarget, owner, true);
    params.SetAttenuationSlot(kVoice0, kTarget, -3.0f, 73);
    params.SetClaimRampSuppressed(kVoice0, kTarget, owner, false);

    EXPECT_FLOAT_EQ(-4.0f, params.ResolvedValue(kVoice0, kTarget));
    EXPECT_EQ(17u, params.PendingRampMs(kVoice0, kTarget));
    params.MarkSliceCommitted(kVoice0, kTarget, owner, frozenResolved,
                              frozenRampMs);
    const std::vector<ParamCommit> commits = FlushAll(params);
    ASSERT_EQ(1u, commits.size());
    EXPECT_FLOAT_EQ(-4.0f, commits[0].resolved);
    EXPECT_EQ(17u, commits[0].rampMs);
}

TEST(ParamCombine, PitchAddsInCentsAndCancelsToBase) {
    ParamSystem params(DefaultConfig());
    params.OnVoiceStarted(kVoice0, 0.0f, 1.0); // base ratio 1.0 = 0 cents

    // Binding contributes one octave up (+1200 cents).
    ASSERT_TRUE(params.BindVoiceTarget(
        harkentest::ObjectId(3), kVoice0, harken::kTargetVoicePitch,
        LinearCurve(0.0f, 0.0f, 1.0f, 1200.0f)));
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(3), 1.0f));
    std::vector<ParamCommit> commits = FlushAll(params);
    ASSERT_EQ(1u, commits.size());
    EXPECT_NEAR(1200.0f, commits[0].resolved, 1e-3f);

    // API-direct one octave down (ratio 0.5 = -1200 cents) cancels the
    // binding exactly: resolved returns to the 0-cent base.
    const float cents = static_cast<float>(harken::RatioToCents(0.5));
    const float resolved =
        params.SetApiDirect(kVoice0, harken::kTargetVoicePitch, cents);
    EXPECT_NEAR(0.0f, resolved, 1e-3f);
    EXPECT_NEAR(1.0, harken::CentsToRatio(resolved), 1e-6);
}

TEST(ParamCombine, LowpassTakesMinimumAndDefaultsOpen) {
    harken::BindingDependencyIndex index;
    ParamSystem params(DefaultConfig());
    AttachIndex(params, index);
    params.OnVoiceStarted(kVoice0, 0.0f, 1.0);

    // Untouched: fully open (the no-contribution sentinel).
    EXPECT_EQ(harken::kLowpassOpenHz,
              params.ResolvedValue(kVoice0,
                                   harken::kTargetVoiceLowpassCutoff));

    ASSERT_TRUE(params.BindVoiceTarget(
        harkentest::ObjectId(11), kVoice0, harken::kTargetVoiceLowpassCutoff,
        LinearCurve(0.0f, 500.0f, 1.0f, 20000.0f)));
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(11), 1.0f)); // binding says 20000 Hz
    (void)FlushAll(params);

    // The more-closed source wins: API-direct 8000 under binding 20000.
    const float resolved = params.SetApiDirect(
        kVoice0, harken::kTargetVoiceLowpassCutoff, 8000.0f);
    EXPECT_FLOAT_EQ(8000.0f, resolved);

    // And the binding wins when it is the smaller one.
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(11), 0.0f)); // binding now 500 Hz
    (void)FlushAll(params);
    EXPECT_FLOAT_EQ(500.0f,
                    params.ResolvedValue(
                        kVoice0, harken::kTargetVoiceLowpassCutoff));
}

TEST(ParamCombine, HighpassTakesMaximumOverClosedBase) {
    harken::BindingDependencyIndex index;
    ParamSystem params(DefaultConfig());
    AttachIndex(params, index);
    params.OnVoiceStarted(kVoice0, 0.0f, 1.0);

    EXPECT_EQ(harken::kHighpassClosedHz,
              params.ResolvedValue(kVoice0,
                                   harken::kTargetVoiceHighpassCutoff));
    harken::VoiceTonePreset authored;
    authored.highpassEnabled = true;
    authored.highpassFrequencyHz = 150;
    params.ApplyTonePreset(kVoice0, authored);

    ASSERT_TRUE(params.BindVoiceTarget(
        harkentest::ObjectId(13), kVoice0, harken::kTargetVoiceHighpassCutoff,
        LinearCurve(0.0f, 0.0f, 1.0f, 400.0f)));
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(13), 0.25f)); // binding says 100 Hz
    (void)FlushAll(params);

    const float resolved = params.SetApiDirect(
        kVoice0, harken::kTargetVoiceHighpassCutoff, 200.0f);
    EXPECT_FLOAT_EQ(200.0f, resolved); // max(150, 100, 200)
    params.SetAttenuationSlot(kVoice0, harken::kTargetVoiceHighpassCutoff, 300.0f);
    EXPECT_FLOAT_EQ(300.0f, params.ResolvedValue(kVoice0, harken::kTargetVoiceHighpassCutoff));
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(13), 1.0f));
    (void)FlushAll(params);
    EXPECT_FLOAT_EQ(400.0f, params.ResolvedValue(kVoice0, harken::kTargetVoiceHighpassCutoff));
    params.SetAttenuationSlot(kVoice0, harken::kTargetVoiceHighpassCutoff, 0.0f);
    EXPECT_FLOAT_EQ(400.0f, params.ResolvedValue(kVoice0, harken::kTargetVoiceHighpassCutoff));
    params.SetApiDirect(kVoice0, harken::kTargetVoiceHighpassCutoff, 0);
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(13), 0));
    (void)FlushAll(params);
    EXPECT_FLOAT_EQ(150, params.ResolvedValue(kVoice0, harken::kTargetVoiceHighpassCutoff));
}

TEST(ParamCombine, ToneFrequencyAndQComposeInLog2Domains) {
    harken::BindingDependencyIndex index;
    ParamSystem params(DefaultConfig());
    AttachIndex(params, index);
    params.OnVoiceStarted(kVoice0, 0.0f, 1.0);

    ASSERT_TRUE(params.BindVoiceTarget(
        harkentest::ObjectId(7), kVoice0, harken::kTargetVoiceLowShelfFreqHz,
        LinearCurve(0.0f, 0.0f, 1.0f, 1.0f)));
    ASSERT_TRUE(params.BindVoiceTarget(
        harkentest::ObjectId(8), kVoice0, harken::kTargetVoiceLowShelfQ,
        LinearCurve(0.0f, 0.0f, 1.0f, 1.0f)));
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(7), 1.0f));
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(8), 1.0f));
    (void)FlushAll(params);

    EXPECT_FLOAT_EQ(500.0f, params.ResolvedValue(
                                kVoice0,
                                harken::kTargetVoiceLowShelfFreqHz));
    EXPECT_FLOAT_EQ(harken::kToneDefaultQ * 2.0f,
                    params.ResolvedValue(
                        kVoice0, harken::kTargetVoiceLowShelfQ));
}

TEST(ParamCombine, ToneHighQCompositionClampsAfterLogReduction) {
    harken::BindingDependencyIndex index;
    ParamSystem params(DefaultConfig());
    AttachIndex(params, index);
    params.OnVoiceStarted(kVoice0, 0.0f, 1.0);
    ASSERT_TRUE(params.BindVoiceTarget(
        harkentest::ObjectId(7), kVoice0, harken::kTargetVoiceHighShelfQ,
        LinearCurve(0.0f, 0.0f, 1.0f, 32.0f)));
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(7), 1.0f));
    (void)FlushAll(params);
    EXPECT_FLOAT_EQ(10.0f, params.ResolvedValue(
                               kVoice0,
                               harken::kTargetVoiceHighShelfQ));
}

TEST(ParamCombine, TwoBindingsAccumulateIntoTheVariableSlot) {
    ParamSystem params(DefaultConfig());
    params.OnVoiceStarted(kVoice0, 0.0f, 1.0);

    // Two Variables both bound to the same voice's volume: their
    // contributions accumulate inside the Variable-bindings category.
    ASSERT_TRUE(params.BindVoiceTarget(
        harkentest::ObjectId(21), kVoice0, harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, -6.0f)));
    ASSERT_TRUE(params.BindVoiceTarget(
        harkentest::ObjectId(22), kVoice0, harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, -9.0f)));
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(21), 1.0f)); // -6 dB
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(22), 1.0f)); // -9 dB
    (void)FlushAll(params);
    EXPECT_FLOAT_EQ(-15.0f, params.ResolvedValue(
                                kVoice0, harken::kTargetVoiceVolume));

    // Changing only one Variable rebuilds the slot from both current
    // values — the untouched Variable's contribution is not lost.
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(21), 0.5f));
    (void)FlushAll(params);
    const float midpoint = 20.0f * std::log10((1.0f + std::pow(10.0f, -6.0f / 20.0f)) / 2.0f);
    EXPECT_NEAR(midpoint - 9.0f, params.ResolvedValue(
                                kVoice0, harken::kTargetVoiceVolume), 0.0001f);
}

TEST(ParamCombine, FlushCommitsOnlyChangedResolvedValues) {
    ParamSystem params(DefaultConfig());
    params.OnVoiceStarted(kVoice0, 0.0f, 1.0);
    ASSERT_TRUE(params.BindVoiceTarget(
        harkentest::ObjectId(5), kVoice0, harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));

    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(5), 0.5f));
    EXPECT_EQ(1u, FlushAll(params).size());
    EXPECT_EQ(1u, params.CommitCount());

    // Nothing changed: nothing commits.
    EXPECT_EQ(0u, FlushAll(params).size());
    // Setting the identical value again does not mark anything dirty.
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(5), 0.5f));
    EXPECT_EQ(0u, FlushAll(params).size());
    // A different Variable value that resolves to a NEW target value
    // commits exactly once more.
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(5), 0.75f));
    EXPECT_EQ(1u, FlushAll(params).size());
    EXPECT_EQ(2u, params.CommitCount());

    // A value change whose curve output is identical (clamped region)
    // resolves to the same value and commits nothing.
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(5), 2.0f)); // clamps to y(-12)
    EXPECT_EQ(1u, FlushAll(params).size());     // first arrival at -12
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(5), 3.0f)); // still clamped at -12
    EXPECT_EQ(0u, FlushAll(params).size());
}

TEST(ParamCombine, RefusedPushKeepsTargetDirtyForRetry) {
    ParamSystem params(DefaultConfig());
    params.OnVoiceStarted(kVoice0, 0.0f, 1.0);
    ASSERT_TRUE(params.BindVoiceTarget(
        harkentest::ObjectId(9), kVoice0, harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(9), 1.0f));

    // Simulated full queue: the flush hands the commit over, gets refused,
    // and must present the same commit on the next flush.
    std::uint32_t refused = 0;
    params.FlushCommits([&refused](const ParamCommit&) {
        ++refused;
        return harken::FlushOutcome::Retry;
    });
    EXPECT_EQ(1u, refused);
    EXPECT_EQ(0u, params.CommitCount());

    const std::vector<ParamCommit> retried = FlushAll(params);
    ASSERT_EQ(1u, retried.size());
    EXPECT_FLOAT_EQ(-12.0f, retried[0].resolved);
}

TEST(ParamCombine, BindingToUnknownVoiceIsInert) {
    ParamSystem params(DefaultConfig());
    // kVoice1's slot never started a voice: the binding must evaluate to
    // nothing rather than touch foreign state.
    ASSERT_TRUE(params.BindVoiceTarget(
        harkentest::ObjectId(4), kVoice1, harken::kTargetVoiceVolume,
        LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    ASSERT_EQ(VN_OK, params.SetValue(harkentest::ObjectId(4), 1.0f));
    EXPECT_EQ(0u, FlushAll(params).size());
}

TEST(ParamCombine, SetValueRefusesNonFiniteThroughPublicApi) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallSmoothDefinitions(
                  session.Engine(), {harkentest::ObjectId(1)}, 0.0f, 1.0f).status);

    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetValue(harkentest::ObjectId(1), std::numeric_limits<float>::quiet_NaN()));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetValue(harkentest::ObjectId(1), std::numeric_limits<float>::infinity()));
    EXPECT_EQ(VN_INVALID_ARG,
              Vn_SetValue(harkentest::ObjectId(1), -std::numeric_limits<float>::infinity()));
    // Refused writes leave the installed default untouched.
    float value = -1.0f;
    ASSERT_TRUE(session.Engine()->variables.CurrentValue(harkentest::ObjectId(1), &value));
    EXPECT_FLOAT_EQ(0.0f, value);
    EXPECT_EQ(VN_OK, Vn_SetValue(harkentest::ObjectId(1), 0.5f));
}

TEST(ParamCombine, DisabledLowShelfPresetLeavesProximityCornerOwnership) {
    ParamSystem params(DefaultConfig());
    params.OnVoiceStarted(kVoice0, 0.0f, 1.0);

    // A preset whose low-shelf section is DISABLED renders nothing there:
    // it must not claim the shared corner, so an authored proximity
    // frequency still lands.
    harken::VoiceTonePreset disabled;
    disabled.lowShelfEnabled = false;
    disabled.lowShelfFrequencyHz = 200.0f;
    disabled.lowShelfGainDb = 6.0f;
    params.ApplyTonePreset(kVoice0, disabled);
    EXPECT_FLOAT_EQ(0.0f, params.ResolvedValue(
                              kVoice0, harken::kTargetVoiceLowShelfGainDb));
    params.ApplyProximityFrequency(kVoice0, 400.0f);
    EXPECT_FLOAT_EQ(400.0f, params.ResolvedValue(
                                kVoice0, harken::kTargetVoiceLowShelfFreqHz));

    // An ENABLED low shelf owns the corner: proximity writes are inert.
    params.OnVoiceStarted(kVoice1, 0.0f, 1.0);
    harken::VoiceTonePreset enabled;
    enabled.lowShelfEnabled = true;
    enabled.lowShelfFrequencyHz = 200.0f;
    enabled.lowShelfGainDb = 6.0f;
    params.ApplyTonePreset(kVoice1, enabled);
    params.ApplyProximityFrequency(kVoice1, 400.0f);
    EXPECT_FLOAT_EQ(200.0f, params.ResolvedValue(
                                kVoice1, harken::kTargetVoiceLowShelfFreqHz));
}

TEST(ParamCombine, WorldDefinitionCapacityRefusesWholeInstall) {
    EngineSession session(MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    std::vector<harken::VariableDefinitionV1> definitions;
    definitions.reserve(harken::kVariableDefinitionCapacity + 1u);
    for (std::uint64_t i = 0; i <= harken::kVariableDefinitionCapacity; ++i) {
        definitions.push_back(harkentest::MakeVariableDefinition(
            harkentest::ObjectId(1000 + i), harken::VariableKindV1::Smooth, 0.0f, 2.0f, 0.0f));
    }
    EXPECT_EQ(harken::WorldInstallStatus::Capacity,
              harkentest::InstallDefinitionsWorld(
                  session.Engine(), definitions.data(),
                  static_cast<std::uint32_t>(definitions.size())).status);
    EXPECT_EQ(0u, session.Engine()->variables.CurrentWorldGeneration());
    EXPECT_EQ(VN_NO_VARIABLE, Vn_SetValue(harkentest::ObjectId(1000), 1.0f));
}

TEST(ParamCombine, ReduceWithSlotSubstitutesApiDirectWithoutWriting) {
    ParamSystem params(DefaultConfig());
    params.OnVoiceStarted(kVoice0, 400.0f, 1.0);
    EXPECT_FLOAT_EQ(400.0f, params.ResolvedValue(
                                kVoice0, harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(800.0f,
                    params.ReduceWithSlot(kVoice0, harken::kTargetVoiceVolume,
                                          harken::kSlotApiDirect, 400.0f));
    EXPECT_FLOAT_EQ(400.0f, params.ResolvedValue(
                                kVoice0, harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(400.0f, params.LastCommitted(
                                kVoice0, harken::kTargetVoiceVolume));
}

TEST(ParamCombine, RejectCandidateRestoresBaselineInsteadOfMergingPreparedRamp) {
    ParamSystem params(DefaultConfig());
    params.OnVoiceStarted(kVoice0, 0.0f, 1.0);
    constexpr std::uint32_t kTarget = harken::kTargetVoiceVolume;
    const harken::VariableSliceRef owner{0, 1};
    const std::uint32_t revision = params.TargetRevision(kVoice0, kTarget);
    ASSERT_TRUE(params.TryClaim(kVoice0, kTarget, owner, revision));
    params.SetAttenuationSlot(kVoice0, kTarget, -1.0f, 20);
    ASSERT_TRUE(params.CaptureCandidateBaseline(kVoice0, kTarget, owner));
    params.SetAttenuationSlot(kVoice0, kTarget, 0.0f, 500);
    float resolved = 0.0f;
    std::uint32_t rampMs = 0;
    std::uint32_t capturedRevision = 0;
    ASSERT_TRUE(params.CaptureSnapshotForClaim(
        kVoice0, kTarget, owner, &resolved, &rampMs, &capturedRevision));
    EXPECT_EQ(500u, rampMs);
    params.ReleaseClaim(kVoice0, kTarget, owner,
                        harken::ClaimReleaseReason::RejectCandidate);
    EXPECT_FALSE(params.ForeignClaim(kVoice0, kTarget));
    EXPECT_EQ(20u, params.PendingRampMs(kVoice0, kTarget));
}

TEST(ParamCombine, SharedPublicationSettlesOneSnapshotAndKeepsLaterDemandAndClaims) {
    ParamSystem params(DefaultConfig());
    params.OnVoiceStarted(kVoice0, 0.0f, 1.0);
    constexpr auto target = harken::kTargetVoiceVolume;
    const harken::TargetClaimOwner owner{0, 1, harken::TargetClaimKind::VariablePublication};
    const harken::VariableSliceRef probe{0, 1};
    const auto revision = params.TargetRevision(kVoice0, target);
    ASSERT_TRUE(params.TryClaim(kVoice0, target, owner, revision));
    ASSERT_TRUE(params.TryClaim(kVoice0, target, owner, revision));
    EXPECT_FALSE(params.TryClaim(kVoice0, target, probe, revision));
    params.SetAttenuationSlot(kVoice0, target, -1.0f, 20);
    float resolved = 0.0f;
    std::uint32_t ramp = 0, capturedRevision = 0;
    ASSERT_TRUE(params.CaptureSnapshotForClaim(kVoice0, target, owner, &resolved, &ramp, &capturedRevision));
    ASSERT_TRUE(params.CaptureSnapshotForClaim(kVoice0, target, owner, &resolved, &ramp, &capturedRevision));
    EXPECT_EQ(20u, ramp);
    params.MarkSliceCommitted(kVoice0, target, owner, -1.0f, 20);
    EXPECT_TRUE(params.ForeignClaim(kVoice0, target));
    EXPECT_EQ(20u, params.PendingRampMs(kVoice0, target));
    EXPECT_TRUE(FlushAll(params).empty());
    params.ReleaseClaim(kVoice0, target, probe);
    EXPECT_TRUE(params.ForeignClaim(kVoice0, target));
    params.ReleaseClaim(kVoice0, target, owner);
    EXPECT_FALSE(params.ForeignClaim(kVoice0, target));
    EXPECT_EQ(20u, params.PendingRampMs(kVoice0, target));
    ASSERT_TRUE(params.TryClaim(kVoice0, target, probe, revision));
    params.ReleaseClaim(kVoice0, target, probe);
}

TEST(ParamCombine, VariableProjectionRestoresInsertedValuesCachesAndDemand) {
    harken::BindingDependencyIndex index;
    ParamSystem params(DefaultConfig(), 1);
    AttachIndex(params, index);
    params.OnVoiceStarted(kVoice0, 0.0f, 1.0);
    ASSERT_TRUE(params.PrepareVoiceStorage(3));
    const auto variable = harkentest::ObjectId(7);
    const auto inserted = harkentest::ObjectId(8);
    ASSERT_TRUE(params.BindVoiceTarget(variable, kVoice0,
        harken::kTargetVoiceVolume, LinearCurve(0.0f, 0.0f, 1.0f, -12.0f)));
    ASSERT_EQ(VN_OK, params.SetInstalledValue(variable, 0.0f));
    params.NoteVariableWrite(variable);
    params.EvaluateDirtyBindings(nullptr);
    FlushAll(params);
    const auto count = params.VariableCount();
    params.NoteVariableWrite(variable);
    harken::BindingDirtyTarget pending;
    ASSERT_TRUE(index.DirtyTargetAt(0, &pending));
    const auto pendingVersion = index.TargetVersion(pending);
    const auto demand = params.DirtyTargetCount();
    ASSERT_TRUE(params.BeginVariableProjection());
    EXPECT_FALSE(params.BeginVariableProjection());
    EXPECT_FALSE(params.PrepareVoiceStorage(4));
    ASSERT_EQ(VN_OK, params.SetInstalledValue(variable, 1.0f));
    ASSERT_EQ(VN_OK, params.SetInstalledValue(inserted, 0.5f));
    params.NoteVariableWrite(variable);
    params.EvaluateDirtyBindings(nullptr);
    EXPECT_EQ(1u, DirtyDependencyCount(index));
    EXPECT_EQ(pendingVersion, index.TargetVersion(pending));
    EXPECT_FLOAT_EQ(-12.0f, params.ResolvedValue(kVoice0, harken::kTargetVoiceVolume));
    {
        ParamSystem copy(params);
        copy.RestoreVariableProjection();
        EXPECT_EQ(count, copy.VariableCount());
        EXPECT_FLOAT_EQ(0.0f, copy.ResolvedValue(kVoice0, harken::kTargetVoiceVolume));
        ASSERT_TRUE(copy.BeginVariableProjection());
        copy.ClearInstalledValues();
        ASSERT_EQ(VN_OK, copy.SetInstalledValue(inserted, 0.5f));
        copy.RestoreVariableProjection();
        float restored = -1.0f;
        ASSERT_TRUE(copy.ScopedVariableValue(nullptr, 0, variable, &restored));
        EXPECT_FLOAT_EQ(0.0f, restored);
        EXPECT_FALSE(copy.ScopedVariableValue(nullptr, 0, inserted, &restored));
        EXPECT_FLOAT_EQ(-12.0f, params.ResolvedValue(kVoice0, harken::kTargetVoiceVolume));
    }
    params.RestoreVariableProjection();
    EXPECT_EQ(count, params.VariableCount());
    EXPECT_EQ(demand, params.DirtyTargetCount());
    float value = -1.0f;
    EXPECT_FALSE(params.ScopedVariableValue(nullptr, 0, inserted, &value));
    ASSERT_TRUE(params.ScopedVariableValue(nullptr, 0, variable, &value));
    EXPECT_FLOAT_EQ(0.0f, value);
    EXPECT_FLOAT_EQ(0.0f, params.ResolvedValue(kVoice0, harken::kTargetVoiceVolume));
    EXPECT_TRUE(FlushAll(params).empty());
    ASSERT_EQ(VN_OK, params.SetValue(variable, 1.0f));
    const auto commits = FlushAll(params);
    ASSERT_EQ(1u, commits.size());
    EXPECT_FLOAT_EQ(-12.0f, commits[0].resolved);
    EXPECT_GT(params.VariableProjectionStorageBytes(), 0u);
}

TEST(ParamCombine, PrivateInitialPrimingDoesNotPublishValuesOrSuppressTheNextLiveUpdate) {
    harken::BindingDependencyIndex index;
    ParamSystem params(DefaultConfig());
    AttachIndex(params, index);
    params.OnVoiceStarted(kVoice0, 0, 1);
    const auto variable = harkentest::ObjectId(7);
    ASSERT_TRUE(params.BindVoiceTarget(variable, kVoice0, harken::kTargetVoiceVolume,
        LinearCurve(0, 0, 1, -12)));
    ASSERT_TRUE(params.BindVoiceTarget(variable, kVoice0, harken::kTargetVoicePan,
        LinearCurve(0, 0, 1, 0.5f)));
    ASSERT_EQ(VN_OK, params.SetValue(variable, 0));
    FlushAll(params);
    const auto demand = params.DirtyTargetCount();
    const harken::VariableReadCutReader reader{nullptr, {},
        [](void*, harken::VariableReadCutRef, VnObjectId, float* out) noexcept {
            *out = 1;
            return harken::VariableReadCutValueStatus::Value;
        }};
    harken::ParamSystem::PrimedValue primed[harken::kBuiltinTargetCount]{};
    std::uint32_t written = 0;
    ASSERT_TRUE(params.BeginVariableProjection());
    ASSERT_TRUE(params.TryPrimeVoiceBindings(kVoice0, reader, primed, std::size(primed), &written, true));
    ASSERT_EQ(2u, written);
    params.RestoreVariableProjection();
    EXPECT_EQ(demand, params.DirtyTargetCount());
    EXPECT_FLOAT_EQ(0, params.LastCommitted(kVoice0, harken::kTargetVoiceVolume));
    EXPECT_FLOAT_EQ(0, params.LastCommitted(kVoice0, harken::kTargetVoicePan));
    EXPECT_TRUE(FlushAll(params).empty());
    ASSERT_EQ(VN_OK, params.SetValue(variable, 1));
    const auto commits = FlushAll(params);
    ASSERT_EQ(2u, commits.size());
    const auto pan = std::find_if(commits.begin(), commits.end(), [](const auto& commit) {
        return commit.targetId == harken::kTargetVoicePan;
    });
    ASSERT_NE(commits.end(), pan);
    EXPECT_FLOAT_EQ(0.5f, pan->resolved);
    EXPECT_FLOAT_EQ(0.5f, pan->pan2d);
}

TEST(ParamCombine, RetiredSharedPublicationRetainsEverySnapshotReference) {
    ParamSystem params(DefaultConfig());
    params.OnVoiceStarted(kVoice0, 0.0f, 1.0);
    constexpr auto target = harken::kTargetVoiceVolume;
    const harken::TargetClaimOwner owner{0, 1, harken::TargetClaimKind::VariablePublication};
    const auto revision = params.TargetRevision(kVoice0, target);
    ASSERT_TRUE(params.TryClaim(kVoice0, target, owner, revision));
    ASSERT_TRUE(params.TryClaim(kVoice0, target, owner, revision));
    params.RetireVoiceTargets(kVoice0);
    EXPECT_EQ(1u, params.RetiringTargetCountForTests());
    params.MarkSliceCommitted(kVoice0, target, owner, -6.0f, 0);
    EXPECT_EQ(1u, params.RetiringTargetCountForTests());
    params.ReleaseClaim(kVoice0, target, harken::VariableSliceRef{0, 1});
    EXPECT_EQ(1u, params.RetiringTargetCountForTests());
    params.ReleaseClaim(kVoice0, target, owner);
    EXPECT_EQ(0u, params.RetiringTargetCountForTests());
}

TEST(ParamCombine, RetryPreserveStillMergesPreparedRamp) {
    ParamSystem params(DefaultConfig());
    params.OnVoiceStarted(kVoice0, 0.0f, 1.0);
    constexpr std::uint32_t kTarget = harken::kTargetVoiceVolume;
    const harken::VariableSliceRef owner{0, 1};
    const std::uint32_t revision = params.TargetRevision(kVoice0, kTarget);
    ASSERT_TRUE(params.TryClaim(kVoice0, kTarget, owner, revision));
    params.SetAttenuationSlot(kVoice0, kTarget, -1.0f, 20);
    ASSERT_TRUE(params.CaptureCandidateBaseline(kVoice0, kTarget, owner));
    params.SetAttenuationSlot(kVoice0, kTarget, 0.0f, 500);
    float resolved = 0.0f;
    std::uint32_t rampMs = 0;
    std::uint32_t capturedRevision = 0;
    ASSERT_TRUE(params.CaptureSnapshotForClaim(
        kVoice0, kTarget, owner, &resolved, &rampMs, &capturedRevision));
    params.ReleaseClaim(kVoice0, kTarget, owner,
                        harken::ClaimReleaseReason::RetryPreserve);
    EXPECT_EQ(500u, params.PendingRampMs(kVoice0, kTarget));
}

TEST(ParamCombine, RejectCandidateRestoresApiDirectTickPair) {
    ParamSystem params(DefaultConfig());
    params.OnVoiceStarted(kVoice0, 0.0f, 1.0);
    constexpr std::uint32_t kTarget = harken::kTargetVoiceVolume;
    (void)params.SetApiDirect(kVoice0, kTarget, -6.0f, 20);
    const harken::VariableSliceRef owner{0, 1};
    const std::uint32_t revision = params.TargetRevision(kVoice0, kTarget);
    ASSERT_TRUE(params.TryClaim(kVoice0, kTarget, owner, revision));
    ASSERT_TRUE(params.CaptureCandidateBaseline(kVoice0, kTarget, owner));
    params.SetAttenuationSlot(kVoice0, kTarget, -1.0f, 500);
    float resolved = 0.0f;
    std::uint32_t rampMs = 0;
    std::uint32_t capturedRevision = 0;
    ASSERT_TRUE(params.CaptureSnapshotForClaim(
        kVoice0, kTarget, owner, &resolved, &rampMs, &capturedRevision));
    params.ReleaseClaim(kVoice0, kTarget, owner,
                        harken::ClaimReleaseReason::RejectCandidate);
    EXPECT_EQ(20u, params.PendingRampMs(kVoice0, kTarget));
}

} // namespace
