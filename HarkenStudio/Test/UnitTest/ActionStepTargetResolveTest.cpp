#include "ActionStepUtils.h"
#include "Components/VAction.h"
#include "ItemTypeCatalog.h"
#include "ProjectTypes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <vector>

using VNX::ProjectItem;
using VNX::VActionStep;
namespace EU = VNX::EditorUtils;

namespace {

bool inSet(const std::vector<ProjectItem::Type>& set, ProjectItem::Type t)
{
    return std::find(set.begin(), set.end(), t) != set.end();
}

// A hand-built lookup exercising the single resolver that live-sync, pack
// export, and the provider all call -- proving they reach one conclusion.
struct Fixture
{
    std::vector<std::unique_ptr<ProjectItem>> items;

    ProjectItem* add(ProjectItem::Type type, const QString& name, uint32_t objectID)
    {
        auto item = std::make_unique<ProjectItem>();
        item->type = type;
        item->name = name;
        item->objectID = objectID;
        ProjectItem* raw = item.get();
        items.push_back(std::move(item));
        return raw;
    }

    EU::ActionTargetLookup lookup()
    {
        EU::ActionTargetLookup l;
        for (const auto& item : items)
            l.add(*item);
        return l;
    }
};

} // namespace

TEST(ActionStepTargetResolve, ResolvesByIdAndNormalizesDisplayName)
{
    Fixture f;
    f.add(ProjectItem::Type::Sound, QStringLiteral("Explosion"), 10);
    // targetName carries a stale label; the resolved display normalizes to
    // the object's current name (rename tolerance).
    const auto res = EU::resolveActionStepTarget(
        VActionStep::Type::Play, 10, QStringLiteral("OldExplosionName"), f.lookup());
    EXPECT_EQ(res.state, EU::ActionTargetState::Resolved);
    EXPECT_EQ(res.objectID, 10u);
    EXPECT_EQ(res.displayName, QStringLiteral("Explosion"));
    EXPECT_FALSE(res.ambiguous);
}

TEST(ActionStepTargetResolve, RebindsNameOnlyReference)
{
    Fixture f;
    f.add(ProjectItem::Type::Sound, QStringLiteral("Footstep"), 42);
    const auto res = EU::resolveActionStepTarget(
        VActionStep::Type::Play, 0, QStringLiteral("Footstep"), f.lookup());
    EXPECT_EQ(res.state, EU::ActionTargetState::Resolved);
    EXPECT_EQ(res.objectID, 42u);
    EXPECT_EQ(res.displayName, QStringLiteral("Footstep"));
}

TEST(ActionStepTargetResolve, LookupOwnsTargetMetadata)
{
    Fixture f;
    f.add(ProjectItem::Type::Sound, QStringLiteral("Footstep"), 42);
    const EU::ActionTargetLookup lookup = f.lookup();
    f.items.clear();

    const auto res = EU::resolveActionStepTarget(
        VActionStep::Type::Play, 42, QString(), lookup);
    EXPECT_EQ(res.state, EU::ActionTargetState::Resolved);
    EXPECT_EQ(res.displayName, QStringLiteral("Footstep"));
}

TEST(ActionStepTargetMatrix, PlayTargetsAreExactlyTheContentObjects)
{
    const auto& playable = EU::actionStepTargetTypes(VActionStep::Type::Play);
    for (ProjectItem::Type t : VNX::ItemTypeCatalog::allTypes())
        EXPECT_EQ(inSet(playable, t), VNX::ItemTypeCatalog::isContentObject(t))
            << static_cast<int>(t);

}

TEST(ActionStepTargetMatrix, EmptyAndUnknownDomainsRejectEverything)
{
    const auto unknown = static_cast<VActionStep::Type>(255);
    EXPECT_TRUE(EU::actionStepTargetTypes(unknown).empty());
    for (ProjectItem::Type type : VNX::ItemTypeCatalog::allTypes())
        EXPECT_FALSE(EU::canActionStepReferenceObject(unknown, type));
}

TEST(ActionStepTargetMatrix, PostActionRejectsSelf)
{
    using PT = ProjectItem::Type;
    EXPECT_TRUE(EU::canActionStepReferenceObject(
        VActionStep::Type::PostAction, PT::Action, 9, 10));
    EXPECT_FALSE(EU::canActionStepReferenceObject(
        VActionStep::Type::PostAction, PT::Action, 10, 10));
}

TEST(ActionStepTargetResolve, PostActionGraphRejectsCyclesAndAllowsSharedDependencies)
{
    Fixture f;
    auto* a = f.add(ProjectItem::Type::Action, QStringLiteral("A"), 10);
    auto* b = f.add(ProjectItem::Type::Action, QStringLiteral("B"), 20);
    auto* c = f.add(ProjectItem::Type::Action, QStringLiteral("C"), 30);
    auto* d = f.add(ProjectItem::Type::Action, QStringLiteral("D"), 40);
    const auto post = [](const ProjectItem* target) {
        VActionStep step;
        step.type = VActionStep::Type::PostAction;
        step.targetObjectID = target->objectID;
        step.targetName = target->name;
        return step;
    };
    const std::vector<VActionStep> replacement{post(b), post(c)};
    b->compOrCreate<VNX::VAction>()->steps = {post(d)};
    c->compOrCreate<VNX::VAction>()->steps = {post(d)};
    EXPECT_TRUE(EU::postActionCycleError(a->objectID, replacement, f.lookup()).isEmpty());

    d->compOrCreate<VNX::VAction>()->steps = {post(a)};
    EXPECT_EQ(EU::postActionCycleError(a->objectID, replacement, f.lookup()),
        QStringLiteral("PostAction dependency cycle: A -> B -> D -> A"));
    b->comp<VNX::VAction>()->steps = {post(a)};
    EXPECT_EQ(EU::postActionCycleError(a->objectID, replacement, f.lookup()),
        QStringLiteral("PostAction dependency cycle: A -> B -> A"));
    EXPECT_EQ(EU::postActionCycleError(a->objectID, {post(a)}, f.lookup()),
        QStringLiteral("PostAction dependency cycle: A -> A"));
    EXPECT_TRUE(EU::postActionCycleError(a->objectID, {}, f.lookup()).isEmpty());
}

TEST(ActionStepTargetResolve, PostActionGraphOwnsDependenciesAndUsesCanonicalTargetResolution)
{
    Fixture f;
    auto* a = f.add(ProjectItem::Type::Action, QStringLiteral("A"), 10);
    auto* b = f.add(ProjectItem::Type::Action, QStringLiteral("B"), 20);
    VActionStep toA;
    toA.type = VActionStep::Type::PostAction;
    toA.targetName = QStringLiteral("Actions/A");
    toA.delay = 1000;
    VActionStep toB;
    toB.type = VActionStep::Type::PostAction;
    toB.targetObjectID = b->objectID;
    toB.targetName = QStringLiteral("StaleName");
    a->compOrCreate<VNX::VAction>()->steps = {toB};
    b->compOrCreate<VNX::VAction>()->steps = {toA};
    const auto lookup = f.lookup();
    EXPECT_FALSE(EU::postActionCycleError(a->objectID, {toB}, lookup).isEmpty());
    EXPECT_TRUE(EU::postActionCycleError(a->objectID, {}, lookup).isEmpty());

    b->comp<VNX::VAction>()->steps.front().targetObjectID = 999;
    EXPECT_TRUE(EU::postActionCycleError(a->objectID, {toB}, f.lookup()).isEmpty());
    f.items.clear();
    EXPECT_EQ(EU::postActionCycleError(10, {toB}, lookup),
        QStringLiteral("PostAction dependency cycle: A -> B -> A"));
}

TEST(ActionStepTargetResolve, TypeChangeClearsResolvedNameOnlyIncompatibleTarget)
{
    Fixture f;
    f.add(ProjectItem::Type::Sound, QStringLiteral("Footstep"), 42);
    VActionStep step;
    step.type = VActionStep::Type::Play;
    step.targetName = QStringLiteral("Footstep");

    EU::applyActionStepTypeChange(step, VActionStep::Type::PostAction, f.lookup());

    EXPECT_EQ(step.type, VActionStep::Type::PostAction);
    EXPECT_EQ(step.targetObjectID, 0u);
    EXPECT_TRUE(step.targetName.isEmpty());
}

TEST(ActionStepTargetResolve, TypeChangeKeepsAndCanonicalizesCompatibleNameOnlyTarget)
{
    Fixture f;
    f.add(ProjectItem::Type::Sound, QStringLiteral("Footstep"), 42);
    VActionStep step;
    step.type = VActionStep::Type::Play;
    step.targetName = QStringLiteral("Footstep");

    EU::applyActionStepTypeChange(step, VActionStep::Type::Stop, f.lookup());

    EXPECT_EQ(step.type, VActionStep::Type::Stop);
    EXPECT_EQ(step.targetObjectID, 42u);
    EXPECT_EQ(step.targetName, QStringLiteral("Footstep"));
}

TEST(ActionStepTargetResolve, TypeChangePreservesDanglingNameOnlyTarget)
{
    Fixture f;
    VActionStep step;
    step.type = VActionStep::Type::Play;
    step.targetName = QStringLiteral("Missing");

    EU::applyActionStepTypeChange(step, VActionStep::Type::PostAction, f.lookup());

    EXPECT_EQ(step.type, VActionStep::Type::PostAction);
    EXPECT_EQ(step.targetObjectID, 0u);
    EXPECT_EQ(step.targetName, QStringLiteral("Missing"));
}

TEST(ActionStepTargetResolve, EmptyTargetIsEmptyState)
{
    Fixture f;
    const auto res = EU::resolveActionStepTarget(
        VActionStep::Type::Play, 0, QString(), f.lookup());
    EXPECT_EQ(res.state, EU::ActionTargetState::Empty);
    EXPECT_EQ(res.objectID, 0u);
}

TEST(ActionStepTargetResolve, MissingReferenceIsDangling)
{
    Fixture f;
    f.add(ProjectItem::Type::Sound, QStringLiteral("Present"), 10);
    const auto res = EU::resolveActionStepTarget(
        VActionStep::Type::Play, 99, QStringLiteral("Gone"), f.lookup());
    EXPECT_EQ(res.state, EU::ActionTargetState::Dangling);
    EXPECT_EQ(res.objectID, 0u);
    EXPECT_FALSE(res.ambiguous);
}

TEST(ActionStepTargetResolve, AmbiguousNameIsDanglingNotSilentPick)
{
    Fixture f;
    f.add(ProjectItem::Type::Sound, QStringLiteral("Hit"), 10);
    f.add(ProjectItem::Type::Sound, QStringLiteral("Hit"), 11);
    const auto res = EU::resolveActionStepTarget(
        VActionStep::Type::Play, 0, QStringLiteral("Hit"), f.lookup());
    EXPECT_EQ(res.state, EU::ActionTargetState::Dangling);
    EXPECT_TRUE(res.ambiguous);
    EXPECT_EQ(res.objectID, 0u);
}

TEST(ActionStepTargetResolve, IncompatibleIdIsDangling)
{
    Fixture f;
    // Play cannot target a Bus; with no compatible name match this is dangling,
    // never a silent mispoint at the bus.
    f.add(ProjectItem::Type::Bus, QStringLiteral("MasterBus"), 5);
    const auto res = EU::resolveActionStepTarget(
        VActionStep::Type::Play, 5, QString(), f.lookup());
    EXPECT_EQ(res.state, EU::ActionTargetState::Dangling);
    EXPECT_EQ(res.objectID, 0u);
}

TEST(ActionStepTargetResolve, IncompatibleIdDoesNotRebindToAnotherObjectWithTheSameName)
{
    Fixture f;
    f.add(ProjectItem::Type::Bus, QStringLiteral("Boom"), 5);
    f.add(ProjectItem::Type::Sound, QStringLiteral("Boom"), 6);
    const auto res = EU::resolveActionStepTarget(
        VActionStep::Type::Play, 5, QStringLiteral("Boom"), f.lookup());
    EXPECT_EQ(res.state, EU::ActionTargetState::Dangling);
    EXPECT_EQ(res.objectID, 0u);
}

TEST(ActionStepTargetResolve, MissingIdDoesNotRebindToAnotherObjectWithTheSameName)
{
    Fixture f;
    f.add(ProjectItem::Type::Sound, QStringLiteral("Boom"), 6);
    const auto res = EU::resolveActionStepTarget(
        VActionStep::Type::Play, 5, QStringLiteral("Boom"), f.lookup());
    EXPECT_EQ(res.state, EU::ActionTargetState::Dangling);
    EXPECT_EQ(res.objectID, 0u);
}

TEST(ActionStepTargetResolve, StopTargetsTrackButNotBus)
{
    Fixture f;
    f.add(ProjectItem::Type::Bus, QStringLiteral("SFXBus"), 5);
    f.add(ProjectItem::Type::Track, QStringLiteral("MusicTrack"), 6);
    const auto busRes = EU::resolveActionStepTarget(
        VActionStep::Type::Stop, 5, QString(), f.lookup());
    EXPECT_EQ(busRes.state, EU::ActionTargetState::Dangling);

    const auto trackRes = EU::resolveActionStepTarget(
        VActionStep::Type::Stop, 6, QString(), f.lookup());
    EXPECT_EQ(trackRes.state, EU::ActionTargetState::Resolved);
    EXPECT_EQ(trackRes.objectID, 6u);
}

TEST(ActionStepTargetResolve, TriggerActionResolvesActionTarget)
{
    Fixture f;
    f.add(ProjectItem::Type::Action, QStringLiteral("ChainedAction"), 30);
    const auto res = EU::resolveActionStepTarget(
        VActionStep::Type::PostAction, 30, QString(), f.lookup());
    EXPECT_EQ(res.state, EU::ActionTargetState::Resolved);
    EXPECT_EQ(res.objectID, 30u);
}

TEST(ActionStepTargetResolve, TriggerActionDoesNotResolveSelf)
{
    Fixture f;
    f.add(ProjectItem::Type::Action, QStringLiteral("Self"), 30);
    const auto res = EU::resolveActionStepTarget(
        VActionStep::Type::PostAction, 30, QStringLiteral("Self"), f.lookup(), 30);
    EXPECT_EQ(res.state, EU::ActionTargetState::Dangling);
}

TEST(ActionStepContract, EditorTypeValuesRemainStable)
{
    EXPECT_EQ(static_cast<uint8_t>(VActionStep::Type::Play), 0u);
    EXPECT_EQ(static_cast<uint8_t>(VActionStep::Type::Stop), 1u);
    EXPECT_EQ(static_cast<uint8_t>(VActionStep::Type::PostAction), 2u);
}

TEST(ActionStepContract, ReadinessLeavesQuantizeAdmissionToCurrentWorld)
{
    VActionStep step;
    EXPECT_TRUE(EU::actionStepRuntimeReadiness(step).ready());

    step.quantize.timing = VNX::QuantizeTiming::NextMarker;
    const auto quantized = EU::actionStepRuntimeReadiness(step);
    EXPECT_TRUE(quantized.ready());
    EXPECT_TRUE(quantized.diagnostic.isEmpty());

    step = {};
    step.type = VActionStep::Type::SetVariable;
    EXPECT_TRUE(EU::actionStepRuntimeReadiness(step).ready());

    step.type = VActionStep::Type::Reset;
    EXPECT_TRUE(EU::actionStepRuntimeReadiness(step).ready());

    step.type = static_cast<VActionStep::Type>(255);
    EXPECT_EQ(EU::actionStepRuntimeReadiness(step).state,
              EU::ActionRuntimeReadinessState::UnknownType);
}

TEST(ActionStepTypeChange, PreservesMarkerTimingBetweenQuantizedVerbs)
{
    Fixture f;
    VActionStep step;
    step.type = VActionStep::Type::Stop;
    step.quantize.referenceObjectID = 77;
    step.quantize.timing = VNX::QuantizeTiming::ExitMarker;

    EU::applyActionStepTypeChange(step, VActionStep::Type::Play, f.lookup());
    EXPECT_EQ(step.quantize.referenceObjectID, 77u);
    EXPECT_EQ(step.quantize.timing, VNX::QuantizeTiming::ExitMarker);

    step.quantize.timing = VNX::QuantizeTiming::NextMarker;
    EU::applyActionStepTypeChange(step, VActionStep::Type::PostAction, f.lookup());
    EXPECT_EQ(step.quantize.referenceObjectID, 77u);
    EXPECT_EQ(step.quantize.timing, VNX::QuantizeTiming::NextMarker);

    EU::applyActionStepTypeChange(step, VActionStep::Type::SetVariable, f.lookup());
    EXPECT_TRUE(EU::isCanonicalActionQuantize(step.quantize));
}

TEST(ActionStepTypeChange, ClearsExclusiveFields)
{
    Fixture f;
    VActionStep step;
    step.type = VActionStep::Type::Stop;
    step.fadeTime = 250.0f;
    step.quantize.timing = VNX::QuantizeTiming::NextMarker;

    EU::applyActionStepTypeChange(step, VActionStep::Type::SetVariable, f.lookup());
    EXPECT_FLOAT_EQ(step.fadeTime, 0.0f);
    EXPECT_TRUE(EU::isCanonicalActionQuantize(step.quantize));

    step.value = 4.0f;
    EU::applyActionStepTypeChange(step, VActionStep::Type::Reset, f.lookup());
    EXPECT_FLOAT_EQ(step.value, 0.0f);
    step.resetScope = VNX::ResetScope::Global;

    EU::applyActionStepTypeChange(step, VActionStep::Type::Play, f.lookup());
    EXPECT_EQ(step.resetScope, VNX::ResetScope::Emitter);
}
