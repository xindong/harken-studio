#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Support/FieldGroupTestUtils.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "Core/ActionClosureSemantics.h"
#include "Core/Components/VAction.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VSelection.h"
#include "Core/Components/VSound.h"
#include "Core/Components/VTrack.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "Core/RuntimeCapabilityMatrix.h"
#include "Core/FieldGroupProjectAdapter.h"

#include <QJsonDocument>
#include <gtest/gtest.h>

#include <set>

using namespace VNX;

namespace {

std::shared_ptr<ProjectItem> item(ProjectItem::Type type, const QString& name,
                                  uint32_t objectId, harkenstudio::ObjectId stableId)
{
    auto value = std::make_shared<ProjectItem>(name, type);
    value->objectID = objectId;
    value->guid = VNX::authoringUuid(harkentest::StudioObjectId(stableId));
    value->attachDefaultComponents();
    return value;
}

void attach(const std::shared_ptr<ProjectItem>& parent,
            const std::shared_ptr<ProjectItem>& child)
{
    child->parent = parent.get();
    child->parentRef = {ParentRef::Kind::Item, parent->guid};
    parent->children.push_back(child);
}

ProjectData projectWith(const std::vector<std::shared_ptr<ProjectItem>>& roots)
{
    auto project = HarkenTest::projectWithPlatforms();
    auto master = item(ProjectItem::Type::Bus, QStringLiteral("Master"), 10001, harkentest::StudioObjectId(10001));
    master->guid = QUuid(QStringLiteral("00000000-0000-0000-0000-000000010001"));
    project.busses.push_back({master, {}});
    const auto rootDefaults = MakeRootFieldGroups(toFieldGroupGuid(master->guid));
    for (const auto& root : roots) {
        if (!root->parent
            && (root->type == ProjectItem::Type::Sound
                || root->type == ProjectItem::Type::Container
                || root->type == ProjectItem::Type::Layer
                || root->type == ProjectItem::Type::Track
                || root->type == ProjectItem::Type::Piece)) {
            if (!root->fieldGroups.mainOutput.shared.localHistory)
                root->fieldGroups.mainOutput.shared.localHistory =
                    rootDefaults.mainOutput.shared.localHistory;
            if (!root->fieldGroups.send.shared.localHistory)
                root->fieldGroups.send.shared.localHistory =
                    rootDefaults.send.shared.localHistory;
            if (!root->fieldGroups.spatialRating.shared.localHistory)
                root->fieldGroups.spatialRating.shared.localHistory =
                    rootDefaults.spatialRating.shared.localHistory;
        }
        ProjectFileBinding binding;
        binding.rootItem = root;
        if (root->type == ProjectItem::Type::Action)
            project.actions.push_back(std::move(binding));
        else if (root->type == ProjectItem::Type::Bus) {
            root->parent = master.get();
            root->parentRef = {ParentRef::Kind::Bus, master->guid};
            master->children.push_back(root);
        } else if (root->type == ProjectItem::Type::Variable)
            project.variables.push_back(std::move(binding));
        else if (root->type == ProjectItem::Type::Attenuation)
            project.attenuationItems.push_back(std::move(binding));
        else
            project.layers.push_back(std::move(binding));
    }
    return project;
}

VActionStep step(VActionStep::Type type, const ProjectItem& target)
{
    VActionStep value;
    value.type = type;
    value.targetObjectID = target.objectID;
    value.targetName = target.name;
    return value;
}

bool hasEdge(const ActionClosurePlan& plan, ActionClosureEdgeReason reason,
             harkenstudio::ObjectId target)
{
    return std::any_of(plan.edges.begin(), plan.edges.end(), [&](const auto& edge) {
        return edge.reason == reason && edge.toStableId == target;
    });
}

}

TEST(ActionClosureSemanticsTest, PlayStopPostAndZeroMediaStepsFollowHardRules)
{
    auto sound = item(ProjectItem::Type::Sound, QStringLiteral("Sound"), 1, harkentest::StudioObjectId(101));
    auto* soundComponent = sound->comp<VSound>();
    ASSERT_NE(soundComponent, nullptr);
    SoundSourceDef source;
    source.entryId = QUuid::createUuid();
    source.sourceId = 77;
    soundComponent->sources.plain.entries.push_back(source);

    auto nested = item(ProjectItem::Type::Action, QStringLiteral("Nested"), 2, harkentest::StudioObjectId(102));
    nested->comp<VAction>()->steps.push_back(step(VActionStep::Type::Stop, *sound));

    auto root = item(ProjectItem::Type::Action, QStringLiteral("Root"), 3, harkentest::StudioObjectId(103));
    root->comp<VAction>()->steps = {
        step(VActionStep::Type::Play, *sound),
        step(VActionStep::Type::PostAction, *nested),
        step(VActionStep::Type::SetVariable, *sound),
        step(VActionStep::Type::Reset, *sound)
    };

    ProjectData project = projectWith({sound, nested, root});
    const ActionClosurePlan plan = ActionClosureSemantics::build(project, harkentest::StudioObjectId(103));
    ASSERT_EQ(plan.actions.size(), 2u);
    ASSERT_EQ(plan.objects.size(), 1u);
    EXPECT_EQ(plan.objects.front().stableId, harkentest::StudioObjectId(101u));
    EXPECT_TRUE(plan.media.empty());
    EXPECT_TRUE(hasEdge(plan, ActionClosureEdgeReason::PostAction, harkentest::StudioObjectId(102)));
    EXPECT_TRUE(hasEdge(plan, ActionClosureEdgeReason::ActionStep, harkentest::StudioObjectId(101)));
    EXPECT_TRUE(std::any_of(plan.capabilities.begin(), plan.capabilities.end(), [](const auto& cap) {
        return cap.feature == RuntimeCapabilityFeature::ActionReset;
    }));
}

TEST(ActionClosureSemanticsTest, TrackClipsExcludeShelfItemsAndPieceUsesDirectTracks)
{
    auto placed = item(ProjectItem::Type::Sound, QStringLiteral("Placed"), 10, harkentest::StudioObjectId(110));
    auto shelf = item(ProjectItem::Type::Sound, QStringLiteral("Shelf"), 11, harkentest::StudioObjectId(111));
    auto track = item(ProjectItem::Type::Track, QStringLiteral("Track"), 12, harkentest::StudioObjectId(112));
    Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.targetGuid = placed->guid;
    clip.duration = 1;
    track->comp<VTrack>()->clips.items.push_back(clip);
    attach(track, shelf);
    auto piece = item(ProjectItem::Type::Piece, QStringLiteral("Piece"), 13, harkentest::StudioObjectId(113));
    attach(piece, track);
    auto action = item(ProjectItem::Type::Action, QStringLiteral("Action"), 14, harkentest::StudioObjectId(114));
    action->comp<VAction>()->steps.push_back(step(VActionStep::Type::Play, *piece));

    ProjectData project = projectWith({placed, piece, action});
    const ActionClosurePlan plan = ActionClosureSemantics::build(project, harkentest::StudioObjectId(114));
    EXPECT_TRUE(std::any_of(plan.objects.begin(), plan.objects.end(), [](const auto& object) {
        return object.stableId == harkentest::StudioObjectId(110);
    }));
    EXPECT_FALSE(std::any_of(plan.objects.begin(), plan.objects.end(), [](const auto& object) {
        return object.stableId == harkentest::StudioObjectId(111);
    }));
    EXPECT_TRUE(hasEdge(plan, ActionClosureEdgeReason::TrackClip, harkentest::StudioObjectId(110)));
    EXPECT_TRUE(std::any_of(plan.objects.begin(), plan.objects.end(), [](const auto& object) {
        return object.stableId == harkentest::StudioObjectId(112) && object.type == ProjectItem::Type::Track;
    }));
    EXPECT_TRUE(std::any_of(plan.objects.begin(), plan.objects.end(), [](const auto& object) {
        return object.stableId == harkentest::StudioObjectId(113) && object.type == ProjectItem::Type::Piece;
    }));
}

TEST(ActionClosureSemanticsTest, ContainerMediaFollowsPlatformSelections)
{
    auto container = item(ProjectItem::Type::Container, QStringLiteral("Container"), 1, harkentest::StudioObjectId(101));
    auto first = item(ProjectItem::Type::Sound, QStringLiteral("First"), 2, harkentest::StudioObjectId(102));
    auto second = item(ProjectItem::Type::Sound, QStringLiteral("Second"), 3, harkentest::StudioObjectId(103));
    auto action = item(ProjectItem::Type::Action, QStringLiteral("Action"), 4, harkentest::StudioObjectId(104));
    attach(container, first);
    attach(container, second);
    HarkenTest::setSharedSoundSource(*first, 11);
    HarkenTest::setSharedSoundSource(*second, 22);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(*container, first->guid));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(*container, second->guid));
    auto& selections = container->comp<VSelection>()->selections;
    selections.editTable(HarkenTest::PlatformIds::Windows).entries.pop_back();
    auto& android = selections.editTable(HarkenTest::PlatformIds::Android);
    android.entries.erase(android.entries.begin());
    action->comp<VAction>()->steps = {step(VActionStep::Type::Play, *container)};
    auto project = projectWith({container, action});
    project.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("Android")});
    for (const auto& platform : {QStringLiteral("Windows"), QStringLiteral("Android")}) {
        const auto plan = ActionClosureSemantics::build(project, action->runtimeId(),
            {HarkenTest::platformIdString(platform), {}});
        ASSERT_TRUE(plan.ready());
        ASSERT_EQ(plan.objects.size(), 2u);
        ASSERT_EQ(plan.media.size(), 1u);
        EXPECT_EQ(plan.media.front().authoringSourceId, platform == QStringLiteral("Windows") ? 11u : 22u);
    }
}

TEST(ActionClosureSemanticsTest, ClosureObjectsExcludeBusAndLayer)
{
    auto sound = item(ProjectItem::Type::Sound, QStringLiteral("Sound"), 40, harkentest::StudioObjectId(140));
    auto bus = item(ProjectItem::Type::Bus, QStringLiteral("Bus"), 41, harkentest::StudioObjectId(141));
    auto layer = item(ProjectItem::Type::Layer, QStringLiteral("Layer"), 42, harkentest::StudioObjectId(142));
    auto action = item(ProjectItem::Type::Action, QStringLiteral("Action"), 43, harkentest::StudioObjectId(143));
    action->comp<VAction>()->steps.push_back(step(VActionStep::Type::Play, *sound));
    sound->fieldGroups.mainOutput.shared = {FieldGroupMode::Local,
        MainOutputFieldGroup{toFieldGroupGuid(bus->guid), 0.0f}};

    ProjectData project = projectWith({sound, bus, layer, action});
    const ActionClosurePlan plan = ActionClosureSemantics::build(project, harkentest::StudioObjectId(143));
    EXPECT_TRUE(std::any_of(plan.objects.begin(), plan.objects.end(), [](const auto& object) {
        return object.stableId == harkentest::StudioObjectId(140) && object.type == ProjectItem::Type::Sound;
    }));
    EXPECT_FALSE(std::any_of(plan.objects.begin(), plan.objects.end(), [](const auto& object) {
        return object.type == ProjectItem::Type::Bus || object.type == ProjectItem::Type::Layer;
    }));
    EXPECT_TRUE(hasEdge(plan, ActionClosureEdgeReason::EffectiveOutputBus, harkentest::StudioObjectId(141)));
}

TEST(ActionClosureSemanticsTest, GroupDependenciesUseTheTargetPlatformWithoutRetainingProviders)
{
    auto layer = item(ProjectItem::Type::Layer, QStringLiteral("Layer"), 200, harkentest::StudioObjectId(1200));
    auto sound = item(ProjectItem::Type::Sound, QStringLiteral("Sound"), 201, harkentest::StudioObjectId(1201));
    auto action = item(ProjectItem::Type::Action, QStringLiteral("Action"), 202, harkentest::StudioObjectId(1202));
    auto outputBus = item(ProjectItem::Type::Bus, QStringLiteral("AndroidOutput"), 203, harkentest::StudioObjectId(1203));
    auto sharedSend = item(ProjectItem::Type::Bus, QStringLiteral("SharedSend"), 204, harkentest::StudioObjectId(1204));
    auto androidSend = item(ProjectItem::Type::Bus, QStringLiteral("AndroidSend"), 205, harkentest::StudioObjectId(1205));
    auto sharedAttenuation = item(ProjectItem::Type::Attenuation,
                                  QStringLiteral("SharedAttenuation"), 206, harkentest::StudioObjectId(1206));
    auto androidAttenuation = item(ProjectItem::Type::Attenuation,
                                   QStringLiteral("AndroidAttenuation"), 207, harkentest::StudioObjectId(1207));
    attach(layer, sound);
    action->comp<VAction>()->steps.push_back(step(VActionStep::Type::Play, *sound));
    SoundSourceDef source;
    source.entryId = QUuid::createUuid();
    source.sourceId = 1;
    sound->comp<VSound>()->sources.plain.entries.push_back(source);
    sound->comp<VSound>()->sources.plain.platforms[HarkenTest::platformIdString("Windows")].active = source.entryId;
    sound->comp<VSound>()->sources.plain.platforms[HarkenTest::platformIdString("Android")].active = source.entryId;
    layer->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::Local,
        MainOutputFieldGroup{toFieldGroupGuid(outputBus->guid), -3.0f}};
    layer->fieldGroups.send.shared = {FieldGroupMode::Local,
        SendFieldGroup{{{toFieldGroupGuid(sharedSend->guid), -3.0f}}}};
    layer->fieldGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::Local,
        SendFieldGroup{{{toFieldGroupGuid(androidSend->guid), -6.0f}}}};
    SpatialRatingFieldGroup sharedRating;
    sharedRating.spatialAttenuationRef = toFieldGroupGuid(sharedAttenuation->guid);
    layer->fieldGroups.spatialRating.shared = {FieldGroupMode::Local, sharedRating};
    auto androidRating = sharedRating;
    androidRating.spatialAttenuationRef = toFieldGroupGuid(androidAttenuation->guid);
    layer->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::Local, androidRating};
    const auto missing = toFieldGroupGuid(QUuid::createUuid());
    sound->fieldGroups.mainOutput.shared.localHistory = MainOutputFieldGroup{missing, 0.0f};
    sound->fieldGroups.send.shared.localHistory = SendFieldGroup{{{missing, 0.0f}}};
    SpatialRatingFieldGroup dormantRating;
    dormantRating.spatialAttenuationRef = missing;
    sound->fieldGroups.spatialRating.shared.localHistory = dormantRating;
    sound->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::UseShared, {}};
    sound->fieldGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::UseShared, {}};
    sound->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::UseShared, {}};
    auto project = projectWith({layer, action, outputBus, sharedSend,
                                      androidSend, sharedAttenuation, androidAttenuation});
    project.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("Android")});
    const auto android = ActionClosureSemantics::build(
        project, action->runtimeId(),
        {HarkenTest::platformIdString("Android"), QStringLiteral("SFX")});
    ASSERT_TRUE(android.ready());
    ASSERT_EQ(android.objects.size(), 1u);
    EXPECT_EQ(android.objects.front().stableId, sound->runtimeId());
    EXPECT_TRUE(hasEdge(android, ActionClosureEdgeReason::EffectiveOutputBus, harkentest::StudioObjectId(1203)));
    EXPECT_TRUE(hasEdge(android, ActionClosureEdgeReason::EffectiveSend, harkentest::StudioObjectId(1205)));
    EXPECT_TRUE(hasEdge(android, ActionClosureEdgeReason::EffectiveAttenuation, harkentest::StudioObjectId(1207)));
    EXPECT_FALSE(hasEdge(android, ActionClosureEdgeReason::EffectiveSend, harkentest::StudioObjectId(1204)));
    EXPECT_FALSE(hasEdge(android, ActionClosureEdgeReason::EffectiveAttenuation, harkentest::StudioObjectId(1206)));
    const auto windows = ActionClosureSemantics::build(
        project, action->runtimeId(),
        {HarkenTest::platformIdString("Windows"), QStringLiteral("SFX")});
    ASSERT_TRUE(windows.ready());
    EXPECT_TRUE(hasEdge(windows, ActionClosureEdgeReason::EffectiveOutputBus,
                        project.busses.front().rootItem->runtimeId()));
    EXPECT_TRUE(hasEdge(windows, ActionClosureEdgeReason::EffectiveSend, harkentest::StudioObjectId(1204)));
    EXPECT_TRUE(hasEdge(windows, ActionClosureEdgeReason::EffectiveAttenuation, harkentest::StudioObjectId(1206)));
    EXPECT_FALSE(hasEdge(windows, ActionClosureEdgeReason::EffectiveOutputBus, harkentest::StudioObjectId(1203)));
    sound->fieldGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::Local, SendFieldGroup{}};
    const auto empty = ActionClosureSemantics::build(
        project, action->runtimeId(),
        {HarkenTest::platformIdString("Android"), QStringLiteral("SFX")});
    ASSERT_TRUE(empty.ready());
    EXPECT_FALSE(std::any_of(empty.edges.begin(), empty.edges.end(), [](const auto& edge) {
        return edge.reason == ActionClosureEdgeReason::EffectiveSend;
    }));
}

TEST(ActionClosureSemanticsTest, BindingDependenciesIncludeEveryContributingOwnerAndParameter)
{
    auto layer = item(ProjectItem::Type::Layer, QStringLiteral("Layer"), 80, harkentest::StudioObjectId(180));
    auto sound = item(ProjectItem::Type::Sound, QStringLiteral("Sound"), 81, harkentest::StudioObjectId(181));
    auto parentVolume = item(ProjectItem::Type::Variable, QStringLiteral("ParentVolume"), 82, harkentest::StudioObjectId(182));
    auto parentPitch = item(ProjectItem::Type::Variable, QStringLiteral("ParentPitch"), 83, harkentest::StudioObjectId(183));
    auto localVolume = item(ProjectItem::Type::Variable, QStringLiteral("LocalVolume"), 84, harkentest::StudioObjectId(184));
    auto action = item(ProjectItem::Type::Action, QStringLiteral("Play"), 85, harkentest::StudioObjectId(185));
    attach(layer, sound);
    action->comp<VAction>()->steps.push_back(step(VActionStep::Type::Play, *sound));

    auto volumeBinding = BindingSemantics::defaultBinding(
        BindingSemantics::volumeAddress(), parentVolume->guid);
    volumeBinding.guid = VNX::authoringUuid(harkentest::StudioObjectId(280));
    ASSERT_TRUE(layer->comp<VMixNode>()->bindings.upsert(volumeBinding));
    auto pitchBinding = BindingSemantics::defaultBinding(
        BindingSemantics::pitchAddress(), parentPitch->guid);
    pitchBinding.guid = VNX::authoringUuid(harkentest::StudioObjectId(281));
    ASSERT_TRUE(layer->comp<VMixNode>()->bindings.upsert(pitchBinding));
    auto ownBinding = BindingSemantics::defaultBinding(
        BindingSemantics::volumeAddress(), localVolume->guid);
    ownBinding.guid = VNX::authoringUuid(harkentest::StudioObjectId(282));
    ASSERT_TRUE(sound->comp<VMixNode>()->bindings.upsert(ownBinding));

    ProjectData project = projectWith({layer, action});
    for (const auto& variable : {parentVolume, parentPitch, localVolume}) {
        ProjectFileBinding file;
        file.rootItem = variable;
        project.variables.push_back(std::move(file));
    }
    const auto plan = ActionClosureSemantics::build(project, action->runtimeId());
    EXPECT_TRUE(hasEdge(plan, ActionClosureEdgeReason::BindingDependency, harkentest::StudioObjectId(182)));
    EXPECT_TRUE(hasEdge(plan, ActionClosureEdgeReason::BindingDependency, harkentest::StudioObjectId(183)));
    EXPECT_TRUE(hasEdge(plan, ActionClosureEdgeReason::BindingDependency, harkentest::StudioObjectId(184)));

    ASSERT_TRUE(sound->comp<VMixNode>()->bindings.remove(ownBinding.runtimeId()));
    const auto cleared = ActionClosureSemantics::build(project, action->runtimeId());
    EXPECT_TRUE(hasEdge(cleared, ActionClosureEdgeReason::BindingDependency, harkentest::StudioObjectId(182)));
    EXPECT_TRUE(hasEdge(cleared, ActionClosureEdgeReason::BindingDependency, harkentest::StudioObjectId(183)));
    EXPECT_FALSE(hasEdge(cleared, ActionClosureEdgeReason::BindingDependency, harkentest::StudioObjectId(184)));
}

TEST(ActionClosureSemanticsTest, GridRowsAndTransitionBridgeAreReachable)
{
    auto child = item(ProjectItem::Type::Sound, QStringLiteral("Child"), 20, harkentest::StudioObjectId(120));
    auto bridge = item(ProjectItem::Type::Piece, QStringLiteral("Bridge"), 21, harkentest::StudioObjectId(121));
    auto container = item(ProjectItem::Type::Container, QStringLiteral("Container"), 22, harkentest::StudioObjectId(122));
    attach(container, child);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(*container, child->guid));
    auto* selection = container->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->mode = SelectionMode::Match;
    selection->matchResponse = VariableResponse::Live;
    {
        auto grid = selection->matchGrid();
        ASSERT_TRUE(grid.addAxis(QUuid::createUuid()));
        ASSERT_TRUE(grid.addRow(child->guid));
        ASSERT_TRUE(selection->selections.shared.setMatchGrid(grid));
    }
    selection->transitions.rules.front().bridge = bridge->guid;
    auto action = item(ProjectItem::Type::Action, QStringLiteral("Action"), 23, harkentest::StudioObjectId(123));
    action->comp<VAction>()->steps.push_back(step(VActionStep::Type::Play, *container));

    ProjectData project = projectWith({bridge, container, action});
    const ActionClosurePlan plan = ActionClosureSemantics::build(project, harkentest::StudioObjectId(123));
    EXPECT_TRUE(hasEdge(plan, ActionClosureEdgeReason::GridRow, harkentest::StudioObjectId(120)));
    EXPECT_TRUE(hasEdge(plan, ActionClosureEdgeReason::TransitionBridge, harkentest::StudioObjectId(121)));
    EXPECT_TRUE(std::any_of(plan.capabilities.begin(), plan.capabilities.end(), [](const auto& cap) {
        return cap.feature == RuntimeCapabilityFeature::Transition;
    }));
    ASSERT_TRUE(selection->setField(QStringLiteral("mode"), static_cast<int>(SelectionMode::Random)));
    const auto dormant = ActionClosureSemantics::build(project, harkentest::StudioObjectId(123));
    EXPECT_FALSE(hasEdge(dormant, ActionClosureEdgeReason::GridRow, harkentest::StudioObjectId(120)));
    EXPECT_FALSE(hasEdge(dormant, ActionClosureEdgeReason::TransitionBridge, harkentest::StudioObjectId(121)));
    EXPECT_FALSE(std::any_of(dormant.capabilities.begin(), dormant.capabilities.end(), [](const auto& cap) {
        return cap.feature == RuntimeCapabilityFeature::Transition;
    }));
    EXPECT_FALSE(selection->matchGrid().rows.empty());
}

TEST(ActionClosureSemanticsTest, QuantizeReferenceIsIncludedInPackClosure)
{
    auto reference = item(ProjectItem::Type::Piece, QStringLiteral("Clock"), 30, harkentest::StudioObjectId(130));
    auto sound = item(ProjectItem::Type::Sound, QStringLiteral("Sound"), 31, harkentest::StudioObjectId(131));
    auto action = item(ProjectItem::Type::Action, QStringLiteral("Action"), 32, harkentest::StudioObjectId(132));
    VActionStep play = step(VActionStep::Type::Play, *sound);
    play.quantize.timing = QuantizeTiming::NextMarker;
    play.quantize.referenceObjectID = reference->objectID;
    action->comp<VAction>()->steps.push_back(play);

    ProjectData project = projectWith({reference, sound, action});
    const ActionClosurePlan plan = ActionClosureSemantics::build(project, harkentest::StudioObjectId(132));
    EXPECT_TRUE(hasEdge(plan, ActionClosureEdgeReason::QuantizeReference, harkentest::StudioObjectId(130)));
    EXPECT_TRUE(std::any_of(plan.objects.begin(), plan.objects.end(), [](const auto& object) {
        return object.stableId == harkentest::StudioObjectId(130);
    }));
}

TEST(ActionClosureSemanticsTest, PostActionCycleAndDiamondAreFiniteAndExplained)
{
    auto a = item(ProjectItem::Type::Action, QStringLiteral("A"), 40, harkentest::StudioObjectId(140));
    auto b = item(ProjectItem::Type::Action, QStringLiteral("B"), 41, harkentest::StudioObjectId(141));
    auto c = item(ProjectItem::Type::Action, QStringLiteral("C"), 42, harkentest::StudioObjectId(142));
    auto d = item(ProjectItem::Type::Action, QStringLiteral("D"), 43, harkentest::StudioObjectId(143));
    a->comp<VAction>()->steps = {step(VActionStep::Type::PostAction, *b),
                                 step(VActionStep::Type::PostAction, *c)};
    b->comp<VAction>()->steps = {step(VActionStep::Type::PostAction, *d)};
    c->comp<VAction>()->steps = {step(VActionStep::Type::PostAction, *d)};
    d->comp<VAction>()->steps = {step(VActionStep::Type::PostAction, *a)};

    ProjectData project = projectWith({a, b, c, d});
    const ActionClosurePlan plan = ActionClosureSemantics::build(project, harkentest::StudioObjectId(140));
    EXPECT_EQ(plan.actions.size(), 4u);
    EXPECT_TRUE(std::any_of(plan.diagnostics.begin(), plan.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == ActionClosureDiagnosticCode::PostActionCycle;
    }));
}

TEST(ActionClosureSemanticsTest, CanonicalJsonIsStableAcrossBindingOrder)
{
    auto sound = item(ProjectItem::Type::Sound, QStringLiteral("Sound"), 50, harkentest::StudioObjectId(150));
    auto action = item(ProjectItem::Type::Action, QStringLiteral("Action"), 51, harkentest::StudioObjectId(151));
    action->comp<VAction>()->steps.push_back(step(VActionStep::Type::Play, *sound));
    ProjectData first = projectWith({sound, action});
    ProjectData second = projectWith({action, sound});

    EXPECT_EQ(QJsonDocument(ActionClosureSemantics::build(first, harkentest::StudioObjectId(151)).toJson()).toJson(),
              QJsonDocument(ActionClosureSemantics::build(second, harkentest::StudioObjectId(151)).toJson()).toJson());
}

TEST(ActionClosureSemanticsTest, InvalidReferencesRemainVisibleInDiagnostics)
{
    auto action = item(ProjectItem::Type::Action, QStringLiteral("Action"), 60, harkentest::StudioObjectId(160));
    VActionStep stepValue;
    stepValue.type = VActionStep::Type::Play;
    stepValue.targetObjectID = 999;
    stepValue.quantize.timing = QuantizeTiming::NextMarker;
    stepValue.quantize.referenceObjectID = 998;
    action->comp<VAction>()->steps.push_back(stepValue);

    ProjectData project = projectWith({action});
    const ActionClosurePlan plan = ActionClosureSemantics::build(project, harkentest::StudioObjectId(160));
    EXPECT_GE(std::count_if(plan.diagnostics.begin(), plan.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == ActionClosureDiagnosticCode::DanglingReference;
    }), 2);
    EXPECT_FALSE(plan.ready());
}

TEST(ActionClosureSemanticsTest, ImmediateIgnoresInactiveQuantizeReferencesAndAbsentBehavior)
{
    auto sound = item(ProjectItem::Type::Sound, QStringLiteral("Sound"), 31, harkentest::StudioObjectId(131));
    SoundSourceDef source;
    source.entryId = QUuid::createUuid();
    source.sourceId = 77;
    sound->comp<VSound>()->sources.plain.entries = {source};
    sound->comp<VSound>()->sources.plain.platforms[HarkenTest::platformIdString("Windows")].active = source.entryId;
    auto action = item(ProjectItem::Type::Action, QStringLiteral("Action"), 32, harkentest::StudioObjectId(132));
    auto play = step(VActionStep::Type::Play, *sound);
    play.quantize.referenceObjectID = UINT32_MAX;
    play.quantize.ifSilent = QuantizeIfSilent::Drop;
    action->comp<VAction>()->steps = {play};
    ProjectData project = projectWith({sound, action});
    const auto plan = ActionClosureSemantics::build(
        project, harkentest::StudioObjectId(132), {HarkenTest::platformIdString("Windows"), QString()});
    EXPECT_TRUE(plan.ready());
    EXPECT_TRUE(plan.diagnostics.empty());
    EXPECT_TRUE(std::none_of(plan.capabilities.begin(), plan.capabilities.end(), [](const auto& cap) {
        return cap.feature == RuntimeCapabilityFeature::Quantize
            || cap.feature == RuntimeCapabilityFeature::IfSilent;
    }));
}

TEST(ActionClosureSemanticsTest, DuplicateObjectReferencesAreRejected)
{
    auto first = item(ProjectItem::Type::Sound, QStringLiteral("First"), 61, harkentest::StudioObjectId(161));
    auto second = item(ProjectItem::Type::Sound, QStringLiteral("Second"), 61, harkentest::StudioObjectId(162));
    auto action = item(ProjectItem::Type::Action, QStringLiteral("Action"), 62, harkentest::StudioObjectId(163));
    action->comp<VAction>()->steps.push_back(step(VActionStep::Type::Play, *first));

    ProjectData project = projectWith({first, second, action});
    const ActionClosurePlan plan = ActionClosureSemantics::build(project, harkentest::StudioObjectId(163));
    EXPECT_TRUE(std::any_of(plan.diagnostics.begin(), plan.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == ActionClosureDiagnosticCode::SchemaMismatch;
    }));
    EXPECT_TRUE(std::any_of(plan.diagnostics.begin(), plan.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.message.contains(QStringLiteral("ambiguous"));
    }));
}

TEST(ActionClosureSemanticsTest, MissingStableIdentityIsNotReady)
{
    auto sound = item(ProjectItem::Type::Sound, QStringLiteral("Sound"), 64, harkentest::StudioObjectId(0));
    auto action = item(ProjectItem::Type::Action, QStringLiteral("Action"), 65, harkentest::StudioObjectId(165));
    action->comp<VAction>()->steps.push_back(step(VActionStep::Type::Play, *sound));

    ProjectData project = projectWith({sound, action});
    const ActionClosurePlan plan = ActionClosureSemantics::build(project, harkentest::StudioObjectId(165));
    EXPECT_TRUE(std::any_of(plan.diagnostics.begin(), plan.diagnostics.end(), [](const auto& diagnostic) {
        return diagnostic.code == ActionClosureDiagnosticCode::MissingStableIdentity;
    }));
    EXPECT_FALSE(plan.ready());
}

TEST(ActionClosureSemanticsTest, OffSwitchIgnoresVariantLanguage)
{
    auto sound = item(ProjectItem::Type::Sound, QStringLiteral("Sound"), 70, harkentest::StudioObjectId(170));
    auto* soundComponent = sound->comp<VSound>();
    ASSERT_NE(soundComponent, nullptr);
    SoundSourceDef shared;
    shared.entryId = QUuid::createUuid();
    shared.sourceId = 11;
    SoundSourceDef english;
    english.entryId = QUuid::createUuid();
    english.sourceId = 22;
    soundComponent->sources.plain.entries = {shared};
    soundComponent->sources.localized["English"].entries = {english};
    soundComponent->sources.plain.platforms[HarkenTest::platformIdString("Windows")].active = shared.entryId;
    soundComponent->sources.localized[QStringLiteral("English")].platforms[HarkenTest::platformIdString("Windows")].active = english.entryId;
    auto action = item(ProjectItem::Type::Action, QStringLiteral("Action"), 71, harkentest::StudioObjectId(171));
    action->comp<VAction>()->steps.push_back(step(VActionStep::Type::Play, *sound));
    ProjectData project = projectWith({sound, action});

    const ActionClosurePlan off = ActionClosureSemantics::build(
        project, harkentest::StudioObjectId(171), {HarkenTest::platformIdString("Windows"), QStringLiteral("English")});
    ASSERT_EQ(off.media.size(), 1u);
    EXPECT_TRUE(off.media.front().language.isEmpty());
    EXPECT_EQ(off.media.front().authoringSourceId, 11u);

    soundComponent->distinguishLanguage = true;
    const ActionClosurePlan on = ActionClosureSemantics::build(
        project, harkentest::StudioObjectId(171), {HarkenTest::platformIdString("Windows"), QStringLiteral("English")});
    ASSERT_EQ(on.media.size(), 1u);
    EXPECT_EQ(on.media.front().language, QStringLiteral("English"));
    EXPECT_EQ(on.media.front().authoringSourceId, 22u);
}

TEST(RuntimeCapabilityMatrixTest, InitialMatrixUsesMachineReadableVerdicts)
{
    const auto transition = RuntimeCapabilityMatrix::query(
        ProjectItem::Type::Container, QStringLiteral("execute"),
        RuntimeCapabilityFeature::Transition);
    ASSERT_TRUE(transition);
    EXPECT_EQ(transition->state, RuntimeCapabilityState::Ready);
    EXPECT_FALSE(RuntimeCapabilityMatrix::toJson()
                     .value(QStringLiteral("entries")).toArray().isEmpty());
}

TEST(RuntimeCapabilityMatrixTest, TransitionUsesCurrentWorldForPackAndPreview)
{
    const auto pack = RuntimeCapabilityMatrix::query(
        ProjectItem::Type::Container, RuntimeCapabilityOperation::PackBuild,
        RuntimeCapabilityFeature::Transition);
    ASSERT_TRUE(pack);
    EXPECT_EQ(pack->state, RuntimeCapabilityState::Ready);

    for (const auto operation : {
             RuntimeCapabilityOperation::LiveSync,
             RuntimeCapabilityOperation::ProjectInputCompile,
             RuntimeCapabilityOperation::CompiledPreview,
             RuntimeCapabilityOperation::AuditionPlay}) {
        const auto entry = RuntimeCapabilityMatrix::query(
            ProjectItem::Type::Container, operation,
            RuntimeCapabilityFeature::Transition);
        ASSERT_TRUE(entry);
        EXPECT_EQ(entry->state, RuntimeCapabilityState::Ready);
    }
}

TEST(RuntimeCapabilityMatrixTest, ActionAssessmentValidatesMarkerReferencesForEveryOperation)
{
    auto sound = item(ProjectItem::Type::Sound, QStringLiteral("Sound"), 70, harkentest::StudioObjectId(170));
    sound->comp<VSound>()->sources.plain.entries.push_back({QUuid::createUuid(), 77, {}});
    auto action = item(ProjectItem::Type::Action, QStringLiteral("Action"), 71, harkentest::StudioObjectId(171));
    action->comp<VAction>()->steps.push_back(step(VActionStep::Type::Play, *sound));
    ProjectData project = projectWith({sound, action});

    const auto ready = RuntimeCapabilityMatrix::assess(
        *action, project, RuntimeCapabilityOperation::ActionRegistration);
    EXPECT_TRUE(ready.permitted())
        << QJsonDocument(ready.toJson()).toJson().constData();
    EXPECT_EQ(ready.backend, RuntimeCapabilityBackend::CurrentWorld);

    action->comp<VAction>()->steps.front().quantize.timing = QuantizeTiming::NextMarker;
    for (const auto operation : {RuntimeCapabilityOperation::PackBuild,
                                 RuntimeCapabilityOperation::ActionRegistration}) {
        const auto refused = RuntimeCapabilityMatrix::assess(
            *action, project, operation,
            BuildVariant{HarkenTest::platformIdString("Windows"), QString()});
        EXPECT_FALSE(refused.permitted());
        EXPECT_TRUE(std::any_of(refused.issues.begin(), refused.issues.end(),
            [](const auto& issue) {
                return issue.state == RuntimeCapabilityState::InvalidAuthoring
                    && issue.diagnostic.contains(QStringLiteral("reference is unresolved"));
            }));
    }
}

TEST(RuntimeCapabilityMatrixTest, ContainerAssessmentKeepsDormantQueueSettingsValid)
{
    auto sound = item(ProjectItem::Type::Sound, QStringLiteral("Sound"), 80, harkentest::StudioObjectId(180));
    sound->comp<VSound>()->sources.plain.entries.push_back({QUuid::createUuid(), 77, {}});
    auto container = item(ProjectItem::Type::Container, QStringLiteral("Container"), 81, harkentest::StudioObjectId(181));
    attach(container, sound);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *container, sound->guid));
    ProjectData project = projectWith({container});

    const auto ready = RuntimeCapabilityMatrix::assess(
        *container, project, RuntimeCapabilityOperation::LiveSync);
    EXPECT_TRUE(ready.permitted())
        << QJsonDocument(ready.toJson()).toJson().constData();

    container->comp<VNX::VSelection>()->behavior.resumeFromLast = true;
    const auto dormant = RuntimeCapabilityMatrix::assess(
        *container, project, RuntimeCapabilityOperation::LiveSync);
    EXPECT_TRUE(dormant.permitted())
        << QJsonDocument(dormant.toJson()).toJson().constData();
    EXPECT_TRUE(container->comp<VNX::VSelection>()->behavior.resumeFromLast);
}
