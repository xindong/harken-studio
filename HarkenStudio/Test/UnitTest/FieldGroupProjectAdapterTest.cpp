#include "Core/Components/VSelection.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "Support/FieldGroupTestUtils.h"
#include "FieldGroupProjectAdapter.h"

#include "CanonicalRuntimeProjectAdapterPrivate.h"
#include "Components/VTrack.h"
#include "ProjectTypes.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>

namespace VNX {
namespace {

using namespace harkenprojectschema;
using States = std::map<QUuid, DefinitionFieldGroups>;

std::shared_ptr<ProjectItem> item(std::uint64_t id, ProjectItem::Type type)
{
    auto result = std::make_shared<ProjectItem>(QString::number(id), type);
    result->guid = QUuid(QStringLiteral("00000000-0000-0000-0000-%1")
                            .arg(id, 12, 16, QLatin1Char('0')));
    return result;
}

void own(const std::shared_ptr<ProjectItem>& parent,
         const std::shared_ptr<ProjectItem>& child)
{
    parent->children.push_back(child);
    child->parent = parent.get();
}

FieldGroupProjectView projectView(ProjectData& project, const States& states)
{
    return buildFieldGroupProjectView(project, [&](const ProjectItem& definition) {
        const auto found = states.find(definition.guid);
        return found == states.end() ? nullptr : &found->second;
    });
}

TEST(FieldGroupProjectAdapter, PreservesAllFiveDefinitionKindsBeforeRuntimePruning)
{
    ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "macOS", "Linux", "iOS"});
    auto layer = item(1, ProjectItem::Type::Layer);
    auto container = item(2, ProjectItem::Type::Container);
    auto piece = item(3, ProjectItem::Type::Piece);
    auto track = item(4, ProjectItem::Type::Track);
    auto sound = item(5, ProjectItem::Type::Sound);
    auto master = item(100, ProjectItem::Type::Bus);
    own(layer, container);
    own(container, piece);
    own(piece, track);
    own(track, sound);
    project.layers.push_back({layer, {}});
    project.busses.push_back({master, {}});
    States states;
    for (const auto& node : {layer, container, piece, track, sound})
        states.emplace(node->guid, DefinitionFieldGroups{});
    states.at(layer->guid) = MakeRootFieldGroups({0, 100});
    states.at(container->guid).mainOutput.shared = {
        FieldGroupMode::Local, MainOutputFieldGroup{{0, 100}, -7.0f}};

    const auto view = projectView(project, states);
    ASSERT_TRUE(view.ok());
    ASSERT_EQ(view.definitions.size(), 5u);
    const FieldGroupResolver resolver(view.definitions, {0, 100});
    ASSERT_TRUE(resolver.ok());
    const auto result = resolver.Resolve({0, 5}, {}, FieldGroupId::MainOutput);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value->source.definition, (AuthoringGuid{0, 2}));
    EXPECT_EQ(std::get<MainOutputFieldGroup>(result.value->payload).outputBusVolume, -7.0f);
    EXPECT_TRUE(ValidateActiveFieldGroup(
        FieldGroupId::MainOutput, result.value->payload, view.referenceTargets).empty());
}

TEST(FieldGroupProjectAdapter, ClipAndContainerReferencesDoNotBecomeParents)
{
    ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "macOS", "Linux", "iOS"});
    auto layerA = item(1, ProjectItem::Type::Layer);
    auto sound = item(2, ProjectItem::Type::Sound);
    auto layerB = item(3, ProjectItem::Type::Layer);
    auto track = item(4, ProjectItem::Type::Track);
    auto container = item(5, ProjectItem::Type::Container);
    own(layerA, sound);
    own(layerB, track);
    own(layerB, container);
    Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.targetGuid = sound->guid;
    track->compOrCreate<VTrack>()->clips.items.push_back(clip);
    container->compOrCreate<VSelection>()->selections.shared.setOccurrenceRows({{sound->guid, 1}});
    project.layers = {{layerA, {}}, {layerB, {}}};
    States states;
    for (const auto& node : {layerA, sound, layerB, track, container})
        states.emplace(node->guid, DefinitionFieldGroups{});
    states.at(layerA->guid) = MakeRootFieldGroups({0, 100});
    states.at(layerB->guid) = MakeRootFieldGroups({0, 100});
    states.at(layerA->guid).mainOutput.shared = {
        FieldGroupMode::Local, MainOutputFieldGroup{{0, 100}, -3.0f}};
    for (const auto& node : {layerB, track, container})
        states.at(node->guid).mainOutput.shared = {
            FieldGroupMode::Local, MainOutputFieldGroup{{0, 101}, -19.0f}};

    const auto view = projectView(project, states);
    ASSERT_TRUE(view.ok());
    const FieldGroupResolver resolver(view.definitions, {0, 100});
    const auto result = resolver.Resolve({0, 2}, HarkenTest::FieldGroupPlatformIds::Android,
                                          FieldGroupId::MainOutput);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(result.value->source.definition, (AuthoringGuid{0, 1}));
    EXPECT_EQ(std::get<MainOutputFieldGroup>(result.value->payload).mainOutputBusRef,
              (AuthoringGuid{0, 100}));
}

TEST(FieldGroupProjectAdapter, FileBindingEnumerationDoesNotCreateRootsOrReorderChildren)
{
    ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "macOS", "Linux", "iOS"});
    auto piece = item(1, ProjectItem::Type::Piece);
    auto first = item(2, ProjectItem::Type::Track);
    auto second = item(3, ProjectItem::Type::Track);
    own(piece, first);
    own(piece, second);
    project.layers = {{second, {}}, {first, {}}, {piece, {}}};
    States states{{piece->guid, MakeRootFieldGroups({0, 100})},
                  {first->guid, {}}, {second->guid, {}}};
    const auto index = HarkenEditor::Bridge::canonicalRuntimeProjectSourceIndex(project);
    ASSERT_TRUE(index->valid);
    EXPECT_EQ(index->parentDefinition.at(first.get()), piece.get());
    EXPECT_EQ(index->parentDefinition.at(second.get()), piece.get());
    ASSERT_EQ(index->children.at(piece.get()).size(), 2u);
    EXPECT_EQ(index->children.at(piece.get())[0], first.get());
    EXPECT_EQ(index->children.at(piece.get())[1], second.get());
    const auto view = projectView(project, states);
    ASSERT_TRUE(view.ok());
    EXPECT_EQ(view.definitions.size(), 3u);
    EXPECT_TRUE(FieldGroupResolver(view.definitions, {0, 100}).ok());
}

TEST(FieldGroupProjectAdapter, StateLookupIsReadOnlyAndNeverSynthesizesLegacyState)
{
    ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "macOS", "Linux", "iOS"});
    auto sound = item(1, ProjectItem::Type::Sound);
    project.layers.push_back({sound, {}});
    const auto missing = projectView(project, {});
    EXPECT_FALSE(missing.ok());
    EXPECT_TRUE(missing.definitions.empty());
    States states{{sound->guid, MakeRootFieldGroups({0, 100})}};
    const auto before = states;
    const auto view = projectView(project, states);
    ASSERT_TRUE(view.ok());
    EXPECT_EQ(view.definitions.front().groups, &states.at(sound->guid));
    const FieldGroupResolver resolver(view.definitions, {0, 100});
    EXPECT_TRUE(resolver.Resolve({0, 1}, {}, FieldGroupId::Send).ok());
    EXPECT_EQ(states, before);
    EXPECT_EQ(sound->playablePropertyPresence.bits, 0u);
}

TEST(FieldGroupProjectAdapter, RefusesDetachedParentReferences)
{
    ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "macOS", "Linux", "iOS"});
    auto sound = item(1, ProjectItem::Type::Sound);
    sound->parentRef.kind = ParentRef::Kind::Item;
    sound->parentRef.guid = item(2, ProjectItem::Type::Layer)->guid;
    project.layers.push_back({sound, {}});
    const auto view = projectView(project, {{sound->guid, {}}});
    EXPECT_FALSE(view.ok());
    EXPECT_TRUE(view.definitions.empty());
    ASSERT_FALSE(view.diagnostics.empty());
    EXPECT_EQ(view.diagnostics.front().field, QStringLiteral("parentRef"));
}

TEST(FieldGroupProjectAdapter, RefusesMultipleOwnersAndDuplicateEdges)
{
    for (bool duplicateEdge : {false, true}) {
        ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "macOS", "Linux", "iOS"});
        auto first = item(1, ProjectItem::Type::Layer);
        auto second = item(2, ProjectItem::Type::Layer);
        auto sound = item(3, ProjectItem::Type::Sound);
        own(first, sound);
        (duplicateEdge ? first : second)->children.push_back(sound);
        project.layers = {{first, {}}, {second, {}}};
        const auto view = projectView(project,
            {{first->guid, {}}, {second->guid, {}}, {sound->guid, {}}});
        EXPECT_FALSE(view.ok());
        EXPECT_TRUE(view.definitions.empty());
        EXPECT_TRUE(view.referenceTargets.empty());
    }
}

TEST(FieldGroupProjectAdapter, RefusesCyclesWithoutRecursingThroughDocumentPaths)
{
    ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "macOS", "Linux", "iOS"});
    auto first = item(1, ProjectItem::Type::Layer);
    auto second = item(2, ProjectItem::Type::Layer);
    own(first, second);
    own(second, first);
    project.layers.push_back({first, {}});
    const auto view = projectView(project, {{first->guid, {}}, {second->guid, {}}});
    EXPECT_FALSE(view.ok());
    EXPECT_TRUE(view.definitions.empty());
    first->children.clear();
    second->children.clear();
    first->parent = nullptr;
    second->parent = nullptr;
}

TEST(FieldGroupProjectAdapter, RefusesMismatchedBackPointersAndDuplicateGuids)
{
    ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "macOS", "Linux", "iOS"});
    auto layer = item(1, ProjectItem::Type::Layer);
    auto sound = item(2, ProjectItem::Type::Sound);
    auto wrong = item(3, ProjectItem::Type::Layer);
    own(layer, sound);
    sound->parent = wrong.get();
    project.layers = {{layer, {}}, {wrong, {}}};
    States states{{layer->guid, {}}, {sound->guid, {}}, {wrong->guid, {}}};
    EXPECT_FALSE(projectView(project, states).ok());
    sound->parent = layer.get();
    wrong->guid = sound->guid;
    EXPECT_FALSE(projectView(project, states).ok());
}

TEST(FieldGroupProjectAdapter, LeavesIllegalContentOwnershipForTheSharedCoreToReject)
{
    ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "macOS", "Linux", "iOS"});
    auto piece = item(1, ProjectItem::Type::Piece);
    auto sound = item(2, ProjectItem::Type::Sound);
    own(piece, sound);
    project.layers = {{piece, {}}};
    States states{{piece->guid, MakeRootFieldGroups({0, 100})}, {sound->guid, {}}};
    const auto view = projectView(project, states);
    ASSERT_TRUE(view.ok());
    const FieldGroupResolver resolver(view.definitions, {0, 100});
    EXPECT_FALSE(resolver.ok());
    ASSERT_EQ(resolver.diagnostics().size(), 1u);
    EXPECT_EQ(resolver.diagnostics().front().code, FieldGroupDiagnosticCode::IllegalParentKind);
    EXPECT_FALSE(resolver.Resolve({0, 2}, {}, FieldGroupId::MainOutput).ok());
}

TEST(FieldGroupProjectAdapter, RefusesBusinessObjectsAsContentParents)
{
    for (const auto kind : {ProjectItem::Type::Bus, ProjectItem::Type::Action,
                            ProjectItem::Type::Attenuation}) {
        ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "macOS", "Linux", "iOS"});
        auto layer = item(1, ProjectItem::Type::Layer);
        auto invalidParent = item(2, kind);
        auto sound = item(3, ProjectItem::Type::Sound);
        own(layer, invalidParent);
        own(invalidParent, sound);
        project.layers.push_back({layer, {}});
        const auto view = projectView(project,
            {{layer->guid, {}}, {sound->guid, {}}});
        EXPECT_FALSE(view.ok());
        EXPECT_TRUE(view.definitions.empty());
    }
}

TEST(FieldGroupProjectAdapter, UsesTheSamePlatformIdentityAsTheCore)
{
    for (const auto platform : HarkenTest::fieldGroupPlatforms) {
        EXPECT_EQ(toFieldGroupGuid(fromFieldGroupGuid(platform)), platform);
        EXPECT_EQ(fromFieldGroupGuid(platform).toString(QUuid::WithoutBraces).toStdString(),
                  HarkenTest::fieldGroupPlatformIdString(platform));
    }
    EXPECT_FALSE(ParseAuthoringPlatform("unknown").has_value());
    EXPECT_FALSE(IsFieldGroupScopeValid(HarkenTest::FieldGroupPlatformIds::Universal));
}

TEST(FieldGroupProjectAdapter, EffectiveSnapshotUsesOnePlatformAcrossTheFullTree)
{
    ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "macOS", "Linux", "iOS"});
    auto layer = item(1, ProjectItem::Type::Layer);
    auto container = item(2, ProjectItem::Type::Container);
    auto sound = item(3, ProjectItem::Type::Sound);
    auto master = item(100, ProjectItem::Type::Bus);
    auto bus = item(101, ProjectItem::Type::Bus);
    own(layer, container);
    own(container, sound);
    own(master, bus);
    project.layers = {{layer, {}}};
    project.busses = {{master, {}}};
    layer->fieldGroups = MakeRootFieldGroups({0, 100});
    layer->fieldGroups.mainOutput.shared = {
        FieldGroupMode::Local, MainOutputFieldGroup{{0, 100}, -3.0f}};
    layer->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::Local, MainOutputFieldGroup{{0, 101}, -9.0f}};
    layer->fieldGroups.send.shared = {
        FieldGroupMode::Local, SendFieldGroup{{{{0, 101}, -2.0f}}}};
    layer->fieldGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::Local, SendFieldGroup{}};
    sound->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = {
        FieldGroupMode::Local, MainOutputFieldGroup{{0, 100}, -3.0f}};

    const auto android = buildFieldGroupEffectiveSnapshot(project, HarkenTest::FieldGroupPlatformIds::Android);
    ASSERT_TRUE(android.ok());
    ASSERT_EQ(android.definitions.size(), 3u);
    const auto& inherited = android.definitions.at({0, 3});
    EXPECT_EQ(inherited.mainOutput.mainOutputBusRef, (AuthoringGuid{0, 101}));
    EXPECT_FLOAT_EQ(inherited.mainOutput.outputBusVolume, -9.0f);
    EXPECT_TRUE(inherited.send.sends.empty());
    EXPECT_EQ(inherited.mainOutputSource.definition, (AuthoringGuid{0, 1}));
    EXPECT_EQ(inherited.mainOutputSource.scope, HarkenTest::FieldGroupPlatformIds::Android);
    EXPECT_EQ(android.objects.at({0, 101}), bus.get());

    layer->fieldGroups.mainOutput.shared.localHistory->outputBusVolume = -5.0f;
    const auto windows = buildFieldGroupEffectiveSnapshot(project, HarkenTest::FieldGroupPlatformIds::Windows);
    ASSERT_TRUE(windows.ok());
    EXPECT_FLOAT_EQ(windows.definitions.at({0, 3}).mainOutput.outputBusVolume, -3.0f);
    EXPECT_EQ(windows.definitions.at({0, 3}).mainOutputSource.definition, (AuthoringGuid{0, 3}));
    const auto shared = buildFieldGroupEffectiveSnapshot(project, {});
    ASSERT_TRUE(shared.ok());
    EXPECT_FLOAT_EQ(shared.definitions.at({0, 3}).mainOutput.outputBusVolume, -5.0f);
}

TEST(FieldGroupProjectAdapter, InactiveDanglingHistoryDoesNotEnterEffectiveSnapshot)
{
    ProjectData project = HarkenTest::projectWithPlatforms({"Windows", "Android", "macOS", "Linux", "iOS"});
    auto sound = item(1, ProjectItem::Type::Sound);
    auto layer = item(2, ProjectItem::Type::Layer);
    auto master = item(100, ProjectItem::Type::Bus);
    own(layer, sound);
    layer->fieldGroups = MakeRootFieldGroups({0, 100});
    project.layers = {{layer, {}}};
    project.busses = {{master, {}}};
    sound->fieldGroups.mainOutput.shared.localHistory = MainOutputFieldGroup{{9, 999}, -7.0f};
    sound->fieldGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::UseShared, SendFieldGroup{{{{9, 998}, -4.0f}}}};
    const auto snapshot = buildFieldGroupEffectiveSnapshot(project, HarkenTest::FieldGroupPlatformIds::Android);
    ASSERT_TRUE(snapshot.ok());
    EXPECT_EQ(snapshot.definitions.at({0, 1}).mainOutput.mainOutputBusRef, (AuthoringGuid{0, 100}));
    EXPECT_EQ(snapshot.definitions.at({0, 1}).mainOutputSource.definition, (AuthoringGuid{0, 2}));
    EXPECT_FALSE(snapshot.definitions.at({0, 1}).mainOutputSource.isDefault);
    EXPECT_TRUE(snapshot.definitions.at({0, 1}).send.sends.empty());
    sound->fieldGroups.mainOutput.shared.mode = FieldGroupMode::Local;
    const auto rejected = buildFieldGroupEffectiveSnapshot(project, HarkenTest::FieldGroupPlatformIds::Android);
    EXPECT_FALSE(rejected.ok());
    EXPECT_TRUE(rejected.definitions.empty());
    EXPECT_TRUE(rejected.objects.empty());
}

}
}
