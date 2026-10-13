#include "Engine.h"
#include "Core/Components/VSelection.h"
#include "Core/ContainerBehaviorSemantics.h"
#include "Widgets/Blueprint/SignalPathEditing.h"
#include "Widgets/Blueprint/SignalPathScope.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VTrack.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/UndoCommands.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "Support/FieldGroupTestUtils.h"

#include <QApplication>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <algorithm>
#include <stdexcept>

namespace {

using namespace VNX;
using T = ProjectItemType;

class SignalPathDeletionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        PlatformContextService::instance().setStorageForTesting(std::make_unique<MemorySettingsStorage>());
        ProjectConfig config;
        config.name = QStringLiteral("SignalPathDeletion");
        config.location = directory.path();
        config.platforms = HarkenTest::platforms({"Windows", "Android"});
        ASSERT_TRUE(manager.createProject(config));
        PlatformContextService::instance().setSelected(
            {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, true);
        auto* master = manager.getMasterBusItem();
        b1 = create("B1", T::Bus, master);
        b2 = create("B2", T::Bus, master);
        b3 = create("B3", T::Bus, master);
        layer = create("Layer", T::Layer);
        container = create("Container", T::Container, layer);
        s1 = create("S1", T::Sound, container);
        s2 = create("S2", T::Sound, container);
        layer->fieldGroups.mainOutput.shared = {FieldGroupMode::Local,
            MainOutputFieldGroup{toFieldGroupGuid(b1->guid), -7.0f}};
        layer->fieldGroups.send.shared = {FieldGroupMode::Local,
            SendFieldGroup{{{toFieldGroupGuid(b2->guid), -6.0f},
                             {toFieldGroupGuid(b3->guid), -3.0f}}}};
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        QApplication::processEvents();
        QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        PlatformContextService::instance().setStorageForTesting(SettingsStorageFactory::create(
            SettingsStorageFactory::StorageType::QSettings,
            QStringLiteral("HarkenSound"), QStringLiteral("HarkenStudio")));
    }

    ProjectItem* create(const char* name, T type, ProjectItem* parent = nullptr)
    {
        auto* item = ProjectManager::instance().createItem(parent, QString::fromLatin1(name), type);
        EXPECT_NE(item, nullptr);
        return item;
    }

    SignalPathGraph graph(FieldGroupScope scope = HarkenTest::FieldGroupPlatformIds::Windows)
    {
        return buildProjectSignalPathGraph(*ProjectManager::instance().currentProject(),
            scope);
    }

    size_t edge(const SignalPathGraph& value, SignalPathEdgeKind kind,
                const ProjectItem* from, const ProjectItem* to, const ProjectItem* provider = nullptr)
    {
        for (size_t index = 0; index < value.edges.size(); ++index) {
            const auto& candidate = value.edges[index];
            if (candidate.kind == kind
                && value.nodes[candidate.from.occurrenceIndex].key.objectGuid == from->guid
                && value.nodes[candidate.to.occurrenceIndex].key.objectGuid == to->guid
                && (!provider || (candidate.routingSource && candidate.routingSource->objectGuid == provider->guid)))
                return index;
        }
        throw std::runtime_error("Expected connection is missing");
    }

    SignalPathRouting route(const ProjectItem* source, FieldGroupScope scope = HarkenTest::FieldGroupPlatformIds::Windows)
    {
        const auto value = graph(scope);
        EXPECT_TRUE(value.ok());
        for (const auto& path : value.paths)
            if (path.identity.sourceGuid == source->guid) return path.routing;
        throw std::runtime_error("Expected signal path is missing");
    }

    bool erase(const SignalPathGraph& value, const std::vector<size_t>& indices)
    {
        auto plan = prepareSignalPathDisconnect(value, indices);
        EXPECT_TRUE(plan.ok()) << plan.error.toStdString();
        return applySignalPathConnect(std::move(plan));
    }

    QTemporaryDir directory;
    ProjectItem* b1 = nullptr;
    ProjectItem* b2 = nullptr;
    ProjectItem* b3 = nullptr;
    ProjectItem* layer = nullptr;
    ProjectItem* container = nullptr;
    ProjectItem* s1 = nullptr;
    ProjectItem* s2 = nullptr;
};

TEST_F(SignalPathDeletionTest, ThroughMovesOriginalToRootAndUndoRestoresOccurrencesAndInheritance)
{
    const auto originalGroups = s1->fieldGroups;
    const auto originalRows = container->comp<VNX::VSelection>()->selections.shared;
    const auto originalRoute = route(s1);
    const auto value = graph();
    ASSERT_TRUE(erase(value, {edge(value, SignalPathEdgeKind::Through, s1, container)}));
    EXPECT_EQ(s1->parent, nullptr);
    EXPECT_NE(ProjectManager::findRootBinding(ProjectManager::instance().currentProject(), s1), nullptr);
    EXPECT_EQ(route(s1), originalRoute);
    EXPECT_EQ(s2->parent, container);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(s1->parent, container);
    EXPECT_EQ(s1->fieldGroups, originalGroups);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared, originalRows);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(s1->parent, nullptr);
    EXPECT_EQ(route(s1), originalRoute);
}

TEST_F(SignalPathDeletionTest, PlatformOnlySelectionDetachRestoresAllTablesAcrossUndoRedo)
{
    auto& selections = container->comp<VSelection>()->selections;
    ASSERT_TRUE(selections.shared.setOccurrenceRows({{s2->guid, 10, 50}}));
    ASSERT_TRUE(selections.editTable(HarkenTest::PlatformIds::Windows).setOccurrenceRows({{s1->guid, 10, 30}}));
    ASSERT_TRUE(selections.editTable(HarkenTest::PlatformIds::Android).setOccurrenceRows({
        {s1->guid, 11, 70}, {s2->guid, 12, 90}}));
    const auto original = selections;
    const auto value = graph();
    ASSERT_TRUE(value.ok());
    ASSERT_TRUE(erase(value, {edge(value, SignalPathEdgeKind::Through, s1, container)}));
    EXPECT_EQ(s1->parent, nullptr);
    EXPECT_EQ(selections.shared, original.shared);
    EXPECT_TRUE(selections.table(HarkenTest::PlatformIds::Windows).occurrenceRows().empty());
    ASSERT_EQ(selections.table(HarkenTest::PlatformIds::Android).occurrenceRows().size(), 1u);
    EXPECT_EQ(selections.table(HarkenTest::PlatformIds::Android).occurrenceRows().front().childGuid, s2->guid);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(s1->parent, container);
    EXPECT_EQ(selections, original);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(s1->parent, nullptr);
    EXPECT_TRUE(selections.table(HarkenTest::PlatformIds::Windows).occurrenceRows().empty());
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(selections, original);
}

TEST_F(SignalPathDeletionTest, SharedInheritedSendDeletionChangesProviderAndPreservesChildInheritance)
{
    const auto ancestorGroups = layer->fieldGroups;
    const auto originalFirst = s1->fieldGroups;
    const auto originalSecond = s2->fieldGroups;
    const auto value = graph();
    ASSERT_TRUE(erase(value, {edge(value, SignalPathEdgeKind::Send, layer, b2)}));
    for (const auto* source : {s1, s2}) {
        const auto routing = route(source);
        ASSERT_EQ(routing.sends.size(), 1u);
        EXPECT_EQ(routing.sends.front().targetBusID, b3->objectID);
        EXPECT_FLOAT_EQ(routing.sends.front().sendLevel, VMixNode::dbToLinear(-3.0f));
        EXPECT_EQ(source->fieldGroups.send.shared.mode, FieldGroupMode::Inherit);
        EXPECT_EQ(source->parent, container);
    }
    EXPECT_EQ(s1->fieldGroups, originalFirst);
    EXPECT_EQ(s2->fieldGroups, originalSecond);
    EXPECT_EQ(layer->fieldGroups.mainOutput, ancestorGroups.mainOutput);
    EXPECT_EQ(layer->fieldGroups.spatialRating, ancestorGroups.spatialRating);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(s1->fieldGroups, originalFirst);
    EXPECT_EQ(s2->fieldGroups, originalSecond);
    EXPECT_EQ(layer->fieldGroups, ancestorGroups);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(route(s1).sends.size(), 1u);
}

TEST_F(SignalPathDeletionTest, MixedThroughAndSendDeletionIsOneReversibleTransaction)
{
    const auto firstGroups = s1->fieldGroups;
    const auto secondGroups = s2->fieldGroups;
    const auto originalRows = container->comp<VNX::VSelection>()->selections.shared;
    const auto value = graph();
    ASSERT_TRUE(erase(value, {edge(value, SignalPathEdgeKind::Through, s1, container),
                             edge(value, SignalPathEdgeKind::Send, layer, b2)}));
    EXPECT_EQ(s1->parent, nullptr);
    EXPECT_EQ(route(s1).sends.size(), 1u);
    EXPECT_EQ(route(s2).sends.size(), 1u);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(s1->parent, container);
    EXPECT_EQ(s1->fieldGroups, firstGroups);
    EXPECT_EQ(s2->fieldGroups, secondGroups);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared, originalRows);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(s1->parent, nullptr);
    EXPECT_EQ(route(s1).sends.size(), 1u);
}

TEST_F(SignalPathDeletionTest, NestedAndSiblingDetachmentsRestoreOriginalOrder)
{
    const auto containerGroups = container->fieldGroups;
    const auto firstGroups = s1->fieldGroups;
    const auto originalRows = container->comp<VNX::VSelection>()->selections.shared;
    const auto value = graph();
    ASSERT_TRUE(erase(value, {edge(value, SignalPathEdgeKind::Through, s1, container),
                             edge(value, SignalPathEdgeKind::Through, container, layer),
                             edge(value, SignalPathEdgeKind::Through, s2, container)}));
    EXPECT_EQ(s1->parent, nullptr);
    EXPECT_EQ(s2->parent, nullptr);
    EXPECT_EQ(container->parent, nullptr);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(container->parent, layer);
    EXPECT_EQ(s1->parent, container);
    EXPECT_EQ(s2->parent, container);
    ASSERT_EQ(container->children.size(), 2u);
    EXPECT_EQ(container->children[0].get(), s1);
    EXPECT_EQ(container->children[1].get(), s2);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared, originalRows);
    EXPECT_EQ(container->fieldGroups, containerGroups);
    EXPECT_EQ(s1->fieldGroups, firstGroups);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(s1->parent, nullptr);
    EXPECT_EQ(s2->parent, nullptr);
    EXPECT_EQ(container->parent, nullptr);
}

TEST_F(SignalPathDeletionTest, ThroughAndRoutingDeletionCaptureFinalInheritedValuesTogether)
{
    const auto firstGroups = s1->fieldGroups;
    const auto providerGroups = layer->fieldGroups;
    const auto originalRows = container->comp<VNX::VSelection>()->selections.shared;
    const auto value = graph();
    auto* master = ProjectManager::instance().getMasterBusItem();
    ASSERT_TRUE(erase(value, {edge(value, SignalPathEdgeKind::Through, s1, container),
                             edge(value, SignalPathEdgeKind::Output, layer, b1),
                             edge(value, SignalPathEdgeKind::Send, layer, b2)}));
    EXPECT_EQ(s1->parent, nullptr);
    EXPECT_EQ(s2->parent, container);
    for (const auto* source : {s1, s2}) {
        const auto routing = route(source);
        EXPECT_EQ(routing.outputBusID, master->objectID);
        ASSERT_EQ(routing.sends.size(), 1u);
        EXPECT_EQ(routing.sends.front().targetBusID, b3->objectID);
    }
    EXPECT_FLOAT_EQ(s1->fieldGroups.mainOutput.shared.localHistory->outputBusVolume, -7.0f);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(s1->parent, container);
    EXPECT_EQ(s1->fieldGroups, firstGroups);
    EXPECT_EQ(layer->fieldGroups, providerGroups);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared, originalRows);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(s1->parent, nullptr);
    EXPECT_EQ(route(s1).outputBusID, master->objectID);
    EXPECT_EQ(route(s1).sends.size(), 1u);
}

TEST_F(SignalPathDeletionTest, BusSendDeletionPreservesOtherSendsAndRoundTrips)
{
    b1->comp<VMixNode>()->sends = {{b2->objectID, 0.5f}, {b3->objectID, 0.25f}};
    const auto original = b1->comp<VMixNode>()->sends;
    const auto value = graph();
    ASSERT_TRUE(erase(value, {edge(value, SignalPathEdgeKind::Send, b1, b2)}));
    const auto& sends = b1->comp<VMixNode>()->sends;
    ASSERT_EQ(sends.size(), 1u);
    EXPECT_EQ(sends.front().targetBusID, b3->objectID);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(b1->comp<VMixNode>()->sends.size(), original.size());
    for (const auto& expected : original) {
        const auto& restored = b1->comp<VMixNode>()->sends;
        const auto found = std::find_if(restored.begin(), restored.end(),
            [&](const auto& send) { return send.targetBusID == expected.targetBusID; });
        ASSERT_NE(found, restored.end());
        EXPECT_FLOAT_EQ(found->sendLevel, expected.sendLevel);
    }
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(b1->comp<VMixNode>()->sends.size(), 1u);
}

TEST_F(SignalPathDeletionTest, ProtectedMasterOutputAndInvalidBatchesDoNotMutateOrCreateUndo)
{
    const auto groups = s1->fieldGroups;
    const auto value = graph();
    auto plan = prepareSignalPathDisconnect(value,
        {edge(value, SignalPathEdgeKind::Through, s1, container),
         edge(value, SignalPathEdgeKind::Output, b1, ProjectManager::instance().getMasterBusItem())});
    EXPECT_FALSE(plan.ok());
    EXPECT_FALSE(applySignalPathConnect(std::move(plan)));
    plan = prepareSignalPathDisconnect(value,
        {edge(value, SignalPathEdgeKind::Send, layer, b2), value.edges.size()});
    EXPECT_FALSE(plan.ok());
    EXPECT_FALSE(applySignalPathConnect(std::move(plan)));
    EXPECT_EQ(s1->parent, container);
    EXPECT_EQ(s1->fieldGroups, groups);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SignalPathDeletionTest, PreparedDeletionRejectsChangedInheritedConfiguration)
{
    const auto original = s1->fieldGroups;
    const auto value = graph();
    auto plan = prepareSignalPathDisconnect(value,
        {edge(value, SignalPathEdgeKind::Send, layer, b2)});
    ASSERT_TRUE(plan.ok()) << plan.error.toStdString();
    layer->fieldGroups.send.shared.localHistory->sends.front().sendLevel = -12.0f;
    EXPECT_FALSE(applySignalPathConnect(std::move(plan)));
    EXPECT_EQ(s1->fieldGroups, original);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SignalPathDeletionTest, ThroughRemovesAllRepeatedSelectionEntriesAndRestoresBehavior)
{
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(*container, s1->guid, {}));
    const auto originalGroups = s1->fieldGroups;
    const auto originalRoute = route(s1);
    auto* selection = container->comp<VNX::VSelection>();
    for (auto& row : selection->selections.shared.member(s1->guid)->occurrences)
        row.weight = 5;
    const auto originalRows = selection->selections.shared;
    selection->behavior.avoidRepeat = 2;
    const auto behavior = selection->behavior;
    const auto value = graph();
    const auto selected = edge(value, SignalPathEdgeKind::Through, s1, container);
    ASSERT_TRUE(erase(value, {selected}));
    EXPECT_EQ(s1->parent, nullptr);
    EXPECT_EQ(route(s1), originalRoute);
    EXPECT_EQ(selection->selections.shared.member(s1->guid), nullptr);
    ASSERT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 1u);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().front().childGuid, s2->guid);
    EXPECT_EQ(selection->selections.shared.occurrenceRows().front().weight, 50);
    EXPECT_EQ(selection->behavior.avoidRepeat, 2);
    EXPECT_EQ(ContainerBehaviorSemantics::effectiveAvoidRepeat(*selection), 0);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared, originalRows);
    EXPECT_EQ(selection->behavior, behavior);
    EXPECT_EQ(s1->parent, container);
    EXPECT_EQ(s1->fieldGroups, originalGroups);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(s1->parent, nullptr);
    ASSERT_EQ(selection->selections.shared.occurrenceRows().size(), 1u);
    EXPECT_EQ(selection->selections.shared.occurrenceRows().front().childGuid, s2->guid);
    EXPECT_EQ(selection->selections.shared.occurrenceRows().front().weight, 50);
}

TEST_F(SignalPathDeletionTest, ThroughRemovesAllRepeatedClipsAndPreservesOtherTimingAndIdentities)
{
    auto* track = create("Track", T::Track, layer);
    auto* sound = create("TrackSound", T::Sound, track);
    auto* sibling = create("OtherTrackSound", T::Sound, track);
    auto& clips = track->comp<VTrack>()->clips;
    clips.items.clear();
    for (int index = 0; index < 3; ++index) {
        Clip clip{QUuid::createUuid(), index < 2 ? sound->guid : sibling->guid, index * 10, 1};
        clip.clipGuid = QUuid::createUuid();
        ASSERT_TRUE(clips.mintOccurrenceOrdinal(&clip));
        clips.items.push_back(clip);
    }
    const auto originalClips = clips;
    const auto originalRoute = route(sound);
    UndoRedoManager::instance().clear();
    const auto value = graph();
    ASSERT_TRUE(erase(value, {edge(value, SignalPathEdgeKind::Through, sound, track)}));
    EXPECT_EQ(sound->parent, nullptr);
    EXPECT_EQ(route(sound), originalRoute);
    ASSERT_EQ(clips.items.size(), 1u);
    EXPECT_EQ(clips.items.front(), originalClips.items.back());
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(sound->parent, track);
    EXPECT_EQ(clips, originalClips);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(sound->parent, nullptr);
    ASSERT_EQ(clips.items.size(), 1u);
    EXPECT_EQ(clips.items.front(), originalClips.items.back());
}

TEST_F(SignalPathDeletionTest, PreparedDeletionRejectsRemovedOccurrence)
{
    const auto value = graph();
    auto plan = prepareSignalPathDisconnect(value,
        {edge(value, SignalPathEdgeKind::Through, s1, container)});
    ASSERT_TRUE(plan.ok()) << plan.error.toStdString();
    PlayablePropertyAuthoringV1::removeContainerChildOccurrences(*container, s1->guid);
    EXPECT_FALSE(applySignalPathConnect(std::move(plan)));
    EXPECT_EQ(s1->parent, container);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SignalPathDeletionTest, ThroughAlsoMovesUnreferencedChildrenToRoot)
{
    PlayablePropertyAuthoringV1::removeContainerChildOccurrences(*container, s1->guid);
    const auto originalRows = container->comp<VNX::VSelection>()->selections.shared;
    const auto originalGroups = s1->fieldGroups;
    const auto value = graph();
    const auto selected = edge(value, SignalPathEdgeKind::Through, s1, container);
    ASSERT_TRUE(erase(value, {selected}));
    EXPECT_EQ(s1->parent, nullptr);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared, originalRows);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(s1->parent, container);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared, originalRows);
    EXPECT_EQ(s1->fieldGroups, originalGroups);
}

TEST_F(SignalPathDeletionTest, SameTargetSendFromAnotherProviderRemainsIndependent)
{
    s2->fieldGroups.send = layer->fieldGroups.send;
    const auto secondGroups = s2->fieldGroups;
    const auto firstGroups = s1->fieldGroups;
    const auto value = graph();
    ASSERT_TRUE(erase(value, {edge(value, SignalPathEdgeKind::Send, layer, b2, layer)}));
    EXPECT_EQ(s1->fieldGroups, firstGroups);
    EXPECT_EQ(s2->fieldGroups, secondGroups);
    EXPECT_EQ(route(s1).sends.size(), 1u);
    EXPECT_EQ(route(s2).sends.size(), 2u);
    const auto after = graph();
    EXPECT_TRUE(after.edges[edge(after, SignalPathEdgeKind::Send, layer, b2, s2)].routingSource);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(route(s1).sends.size(), 2u);
    EXPECT_EQ(s2->fieldGroups, secondGroups);
}

TEST_F(SignalPathDeletionTest, OutputResetChangesOnlyItsProviderAndPreservesVolumeAndInheritance)
{
    auto* master = ProjectManager::instance().getMasterBusItem();
    s2->fieldGroups.mainOutput.shared = {FieldGroupMode::Local,
        MainOutputFieldGroup{toFieldGroupGuid(b1->guid), -3.0f}};
    const auto firstGroups = s1->fieldGroups;
    const auto secondGroups = s2->fieldGroups;
    const auto providerGroups = layer->fieldGroups;
    const auto value = graph();
    ASSERT_TRUE(erase(value, {edge(value, SignalPathEdgeKind::Output, layer, b1, layer)}));
    EXPECT_EQ(route(s1).outputBusID, master->objectID);
    EXPECT_EQ(route(s2).outputBusID, b1->objectID);
    EXPECT_EQ(s1->fieldGroups, firstGroups);
    EXPECT_EQ(s2->fieldGroups, secondGroups);
    EXPECT_FLOAT_EQ(layer->fieldGroups.mainOutput.shared.localHistory->outputBusVolume, -7.0f);
    EXPECT_EQ(layer->fieldGroups.send, providerGroups.send);
    EXPECT_EQ(layer->fieldGroups.spatialRating, providerGroups.spatialRating);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(layer->fieldGroups, providerGroups);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(route(s1).outputBusID, master->objectID);
}

TEST_F(SignalPathDeletionTest, ProviderEditsRespectSelectedPlatformAndLeaveSharedAndOtherPlatformsIntact)
{
    PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    const auto original = layer->fieldGroups;
    const auto sourceGroups = s1->fieldGroups;
    const auto value = graph();
    ASSERT_TRUE(erase(value, {edge(value, SignalPathEdgeKind::Send, layer, b2),
                             edge(value, SignalPathEdgeKind::Output, layer, b1)}));
    EXPECT_EQ(layer->fieldGroups.send.shared, original.send.shared);
    EXPECT_EQ(layer->fieldGroups.mainOutput.shared, original.mainOutput.shared);
    EXPECT_EQ(s1->fieldGroups, sourceGroups);
    EXPECT_EQ(route(s1).sends.size(), 1u);
    EXPECT_EQ(route(s1).outputBusID, ProjectManager::instance().getMasterBusItem()->objectID);
    const auto android = route(s1, HarkenTest::FieldGroupPlatformIds::Android);
    EXPECT_EQ(android.sends.size(), 2u);
    EXPECT_EQ(android.outputBusID, b1->objectID);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(layer->fieldGroups, original);
}

TEST_F(SignalPathDeletionTest, OutputResetRejectsMasterSendConflictUnlessTheSameBatchRemovesIt)
{
    auto* master = ProjectManager::instance().getMasterBusItem();
    layer->fieldGroups.send.shared.localHistory->sends.push_back({toFieldGroupGuid(master->guid), -12.0f});
    const auto original = layer->fieldGroups;
    const auto value = graph();
    const auto output = edge(value, SignalPathEdgeKind::Output, layer, b1);
    auto plan = prepareSignalPathDisconnect(value, {output});
    EXPECT_FALSE(plan.ok());
    EXPECT_FALSE(applySignalPathConnect(std::move(plan)));
    EXPECT_EQ(layer->fieldGroups, original);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    ASSERT_TRUE(erase(value, {output, edge(value, SignalPathEdgeKind::Send, layer, master)}));
    EXPECT_EQ(route(s1).outputBusID, master->objectID);
    EXPECT_EQ(route(s1).sends.size(), 2u);
    EXPECT_FLOAT_EQ(layer->fieldGroups.mainOutput.shared.localHistory->outputBusVolume, -7.0f);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(layer->fieldGroups, original);
}

TEST_F(SignalPathDeletionTest, BusOutputResetMovesSubtreeAndChecksMasterSendAgainstFinalBatch)
{
    auto* master = ProjectManager::instance().getMasterBusItem();
    auto* bus = create("NestedBus", T::Bus, b1);
    auto* child = create("NestedChildBus", T::Bus, bus);
    bus->comp<VMixNode>()->sends = {{master->objectID, 0.25f}, {b2->objectID, 0.5f}};
    UndoRedoManager::instance().clear();
    const auto value = graph();
    const auto output = edge(value, SignalPathEdgeKind::Output, bus, b1);
    auto plan = prepareSignalPathDisconnect(value, {output});
    EXPECT_FALSE(plan.ok());
    EXPECT_FALSE(applySignalPathConnect(std::move(plan)));
    EXPECT_EQ(bus->parent, b1);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    ASSERT_TRUE(erase(value, {output, edge(value, SignalPathEdgeKind::Send, bus, master)}));
    EXPECT_EQ(bus->parent, master);
    EXPECT_EQ(child->parent, bus);
    ASSERT_EQ(bus->comp<VMixNode>()->sends.size(), 1u);
    EXPECT_EQ(bus->comp<VMixNode>()->sends.front().targetBusID, b2->objectID);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(bus->parent, b1);
    EXPECT_EQ(child->parent, bus);
    ASSERT_EQ(bus->comp<VMixNode>()->sends.size(), 2u);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(bus->parent, master);
    EXPECT_EQ(child->parent, bus);
    EXPECT_EQ(bus->comp<VMixNode>()->sends.size(), 1u);
}

TEST_F(SignalPathDeletionTest, SameValueProviderChangeRejectsOldSendAndOutputEdges)
{
    const auto oldSendGraph = graph();
    s1->fieldGroups.send = layer->fieldGroups.send;
    const auto sendGroups = s1->fieldGroups;
    auto plan = prepareSignalPathDisconnect(oldSendGraph,
        {edge(oldSendGraph, SignalPathEdgeKind::Send, layer, b2)});
    EXPECT_FALSE(plan.ok());
    EXPECT_FALSE(applySignalPathConnect(std::move(plan)));
    EXPECT_EQ(s1->fieldGroups, sendGroups);
    const auto oldOutputGraph = graph();
    s1->fieldGroups.mainOutput = layer->fieldGroups.mainOutput;
    const auto outputGroups = s1->fieldGroups;
    plan = prepareSignalPathDisconnect(oldOutputGraph,
        {edge(oldOutputGraph, SignalPathEdgeKind::Output, layer, b1)});
    EXPECT_FALSE(plan.ok());
    EXPECT_FALSE(applySignalPathConnect(std::move(plan)));
    EXPECT_EQ(s1->fieldGroups, outputGroups);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SignalPathDeletionTest, PreparedSelectionDeletionRejectsChangedWeightWithoutPartialRoutingChanges)
{
    const auto originalRows = container->comp<VNX::VSelection>()->selections.shared;
    const auto originalGroups = layer->fieldGroups;
    const auto value = graph();
    auto plan = prepareSignalPathDisconnect(value,
        {edge(value, SignalPathEdgeKind::Through, s1, container),
         edge(value, SignalPathEdgeKind::Send, layer, b2)});
    ASSERT_TRUE(plan.ok()) << plan.error.toStdString();
    container->comp<VNX::VSelection>()->selections.shared.find(originalRows.occurrenceRows().front().stableOccurrenceOrdinal)->weight = 9;
    const auto editedRows = container->comp<VNX::VSelection>()->selections.shared;
    EXPECT_FALSE(applySignalPathConnect(std::move(plan)));
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared, editedRows);
    EXPECT_EQ(layer->fieldGroups, originalGroups);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SignalPathDeletionTest, PreparedThroughCannotRemoveANewPlaybackReference)
{
    PlayablePropertyAuthoringV1::removeContainerChildOccurrences(*container, s1->guid);
    const auto value = graph();
    auto plan = prepareSignalPathDisconnect(value, {edge(value, SignalPathEdgeKind::Through, s1, container)});
    ASSERT_TRUE(plan.ok()) << plan.error.toStdString();
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(*container, s1->guid, {}));
    const auto rows = container->comp<VNX::VSelection>()->selections.shared;
    const auto groups = s1->fieldGroups;
    EXPECT_FALSE(applySignalPathConnect(std::move(plan)));
    EXPECT_EQ(s1->parent, container);
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared, rows);
    EXPECT_EQ(s1->fieldGroups, groups);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(SignalPathDeletionTest, PreparedThroughRejectsChangedClipTimingAndNewUses)
{
    auto* track = create("Track", T::Track, layer);
    auto* sound = create("TrackSound", T::Sound, track);
    auto& clips = track->comp<VTrack>()->clips;
    clips.items.clear();
    Clip clip{QUuid::createUuid(), sound->guid, 10, 1};
    clip.clipGuid = QUuid::createUuid();
    ASSERT_TRUE(clips.mintOccurrenceOrdinal(&clip));
    clips.items.push_back(clip);
    UndoRedoManager::instance().clear();
    const auto value = graph();
    const auto selected = edge(value, SignalPathEdgeKind::Through, sound, track);
    auto plan = prepareSignalPathDisconnect(value, {selected});
    ASSERT_TRUE(plan.ok()) << plan.error.toStdString();
    clips.items.front().start = 20;
    EXPECT_FALSE(applySignalPathConnect(std::move(plan)));
    EXPECT_EQ(sound->parent, track);
    EXPECT_EQ(clips.items.front().start, 20);
    clips.items.front().start = 10;
    plan = prepareSignalPathDisconnect(value, {selected});
    ASSERT_TRUE(plan.ok()) << plan.error.toStdString();
    clip.clipGuid = QUuid::createUuid();
    clip.clipGuid = QUuid::createUuid();
    ASSERT_TRUE(clips.mintOccurrenceOrdinal(&clip));
    clips.items.push_back(clip);
    EXPECT_FALSE(applySignalPathConnect(std::move(plan)));
    EXPECT_EQ(sound->parent, track);
    EXPECT_EQ(clips.items.size(), 2u);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

}
