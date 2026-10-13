#include "Support/ProjectPlatformTestUtils.h"
#include "Support/FieldGroupTestUtils.h"
#include "Core/FieldGroupDerivedView.h"
#include "Core/FieldGroupEditing.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/ProjectManager.h"
#include "Core/UndoCommands.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VSelection.h"
#include "Core/Components/VSpatial.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <memory>

namespace VNX {
namespace {

using Mode = FieldGroupMode;

void ensureFieldGroupApp()
{
    if (qApp) return;
    static int argc = 1;
    static char name[] = "harken_editor_tests";
    static char* argv[] = {name, nullptr};
    static QApplication app(argc, argv);
}

template <typename Payload>
GroupState<Payload> localGroup(Payload value)
{
    return {Mode::Local, std::move(value)};
}

class FieldGroupLifecycle : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureFieldGroupApp();
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ASSERT_TRUE(directory_.isValid());
        ProjectConfig config;
        config.platforms = HarkenTest::platforms({"Windows", "Android", "macOS", "Linux", "iOS"});
        config.name = QStringLiteral("FieldGroupLifecycle");
        config.location = directory_.path();
        ASSERT_TRUE(manager.createProject(config));
        project_ = manager.currentProject();
        ASSERT_NE(project_, nullptr);
        ASSERT_FALSE(project_->busses.empty());
        master_ = project_->busses.front().rootItem.get();
        busA_ = manager.createItem(master_, QStringLiteral("GroupBusA"), ProjectItem::Type::Bus);
        busB_ = manager.createItem(master_, QStringLiteral("GroupBusB"), ProjectItem::Type::Bus);
        layer_ = manager.createItem(nullptr, QStringLiteral("GroupLayer"), ProjectItem::Type::Layer);
        first_ = manager.createItem(layer_, QStringLiteral("GroupFirst"), ProjectItem::Type::Container);
        second_ = manager.createItem(layer_, QStringLiteral("GroupSecond"), ProjectItem::Type::Container);
        ASSERT_NE(busA_, nullptr);
        ASSERT_NE(busB_, nullptr);
        ASSERT_NE(layer_, nullptr);
        ASSERT_NE(first_, nullptr);
        ASSERT_NE(second_, nullptr);
        clean();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
    }

    void clean()
    {
        UndoRedoManager::instance().clear();
        for (auto* binding : project_->allFileBindings())
            if (binding && binding->rootItem) binding->rootItem->clearDirty();
    }

    bool refresh()
    {
        return validateFieldGroupProject(*project_).ok()
            && refreshFieldGroupDerivedViews(*project_);
    }

    void output(ProjectItem* item, ProjectItem* bus, float volume)
    {
        item->fieldGroups.mainOutput.shared =
            localGroup(MainOutputFieldGroup{toFieldGroupGuid(bus->guid), volume});
    }

    void send(ProjectItem* item, ProjectItem* bus, float volume)
    {
        item->fieldGroups.send.shared =
            localGroup(SendFieldGroup{{{toFieldGroupGuid(bus->guid), volume}}});
    }

    void expectOutput(ProjectItem* item, ProjectItem* bus, float volume)
    {
        const auto* mix = item->comp<VMixNode>();
        ASSERT_NE(mix, nullptr);
        EXPECT_EQ(mix->mainOutputBusRef, bus->runtimeId());
        EXPECT_EQ(mix->outputBusId, bus->objectID);
        EXPECT_FLOAT_EQ(mix->outputBusVolume, volume);
    }

    QTemporaryDir directory_;
    ProjectData* project_ = nullptr;
    ProjectItem* master_ = nullptr;
    ProjectItem* busA_ = nullptr;
    ProjectItem* busB_ = nullptr;
    ProjectItem* layer_ = nullptr;
    ProjectItem* first_ = nullptr;
    ProjectItem* second_ = nullptr;
};

TEST_F(FieldGroupLifecycle, RootPromotionPreservesLocalFieldsThatPreviouslyUsedSharedOwnValues)
{
    using Property = harkenplayableproperties::PlayablePropertyIdV1;
    auto& manager = ProjectManager::instance();
    auto* moving = manager.createItem(first_, QStringLiteral("SparseRoot"), ProjectItem::Type::Sound);
    ASSERT_NE(moving, nullptr);
    first_->fieldGroups.spatialRating.shared = {Mode::Local, SpatialRatingFieldGroup{0.1f, false, {}, 4.2f}};
    auto& group = moving->fieldGroups.spatialRating;
    group.shared = {Mode::Inherit, SpatialRatingFieldGroup{0.5f, false, {}, 0.7f}};
    group.platforms[HarkenTest::FieldGroupPlatformIds::macOS] = {
        Mode::Local, SpatialRatingFieldGroup{0.2f}, FieldGroupFieldMask(Property::SpatialPanning)};
    ASSERT_TRUE(refresh());
    const auto authored = moving->fieldGroups;
    const auto identity = toFieldGroupGuid(moving->guid);
    const auto expectValues = [&] {
        const auto windows = buildFieldGroupEffectiveSnapshot(*project_, HarkenTest::FieldGroupPlatformIds::Windows);
        const auto mac = buildFieldGroupEffectiveSnapshot(*project_, HarkenTest::FieldGroupPlatformIds::macOS);
        ASSERT_TRUE(windows.ok());
        ASSERT_TRUE(mac.ok());
        EXPECT_FLOAT_EQ(windows.definitions.at(identity).spatialRating.spatialAttenuationDistanceScaling, 4.2f);
        EXPECT_FLOAT_EQ(mac.definitions.at(identity).spatialRating.spatialAttenuationDistanceScaling, 0.7f);
        EXPECT_FLOAT_EQ(mac.definitions.at(identity).spatialRating.spatialPanning, 0.2f);
    };
    expectValues();
    clean();
    ASSERT_TRUE(manager.moveItemToBucketRoot(moving, QStringLiteral("Layers")));
    expectValues();
    const auto captured = moving->fieldGroups;
    EXPECT_EQ(captured.spatialRating.platforms.at(HarkenTest::FieldGroupPlatformIds::macOS).localFields,
              FieldGroupFieldMask(Property::SpatialPanning) | FieldGroupFieldMask(Property::SpatialAttenuationDistanceScaling));
    EXPECT_TRUE(captured.spatialRating.platforms.contains(HarkenTest::FieldGroupPlatformIds::macOS));
    EXPECT_EQ(captured.spatialRating.platforms.size(), 1u);
    auto* undo = UndoRedoManager::instance().undoStack();
    ASSERT_EQ(undo->count(), 1);
    undo->undo();
    EXPECT_EQ(moving->fieldGroups, authored);
    expectValues();
    undo->redo();
    EXPECT_EQ(moving->fieldGroups, captured);
    expectValues();
}

TEST_F(FieldGroupLifecycle, FreshRootsHaveCurrentValuesAndChildrenInheritWithoutLegacyPresence)
{
    auto& manager = ProjectManager::instance();
    const auto rootGroups = MakeRootFieldGroups(toFieldGroupGuid(master_->guid));
    for (const auto type : {ProjectItem::Type::Sound, ProjectItem::Type::Container,
                            ProjectItem::Type::Layer, ProjectItem::Type::Track, ProjectItem::Type::Piece}) {
        auto* root = manager.createItem(nullptr,
            QStringLiteral("GroupRoot%1").arg(static_cast<int>(type)), type);
        ASSERT_NE(root, nullptr);
        expectOutput(root, master_, 0.0f);
        EXPECT_EQ(root->fieldGroups, rootGroups);
        EXPECT_EQ(root->playablePropertyPresence.bits & kFieldGroupPropertyMask, 0u);
        EXPECT_EQ(root->parentRef.kind, ParentRef::Kind::Root);
    }
    auto* importedRoot = manager.createRootAudioItem(
        QStringLiteral("ImportedRoot"), QStringLiteral("Sources/Root.wav"), 0);
    auto* importedChild = manager.createAudioItem(
        first_, QStringLiteral("ImportedChild"), QStringLiteral("Sources/Child.wav"), 0);
    auto* child = manager.createItem(first_, QStringLiteral("GroupChild"), ProjectItem::Type::Sound);
    ASSERT_NE(importedRoot, nullptr);
    ASSERT_NE(importedChild, nullptr);
    ASSERT_NE(child, nullptr);
    expectOutput(importedRoot, master_, 0.0f);
    expectOutput(importedChild, master_, 0.0f);
    expectOutput(child, master_, 0.0f);
    EXPECT_EQ(importedRoot->fieldGroups, rootGroups);
    EXPECT_EQ(importedChild->fieldGroups, DefinitionFieldGroups{});
    EXPECT_EQ(child->fieldGroups, DefinitionFieldGroups{});
    EXPECT_EQ(child->playablePropertyPresence.bits & kFieldGroupPropertyMask, 0u);
    EXPECT_EQ(child->parentRef.kind, ParentRef::Kind::Item);
    EXPECT_EQ(child->parentRef.guid, first_->guid);
    EXPECT_EQ(child->parentRef.documentId, first_->guid);
}

TEST_F(FieldGroupLifecycle, MovingSubtreeRefreshesEveryInheritedGroupAndUndoRestoresOwnership)
{
    auto& manager = ProjectManager::instance();
    auto* moving = manager.createItem(first_, QStringLiteral("GroupMoving"), ProjectItem::Type::Container);
    auto* leaf = manager.createItem(moving, QStringLiteral("GroupLeaf"), ProjectItem::Type::Sound);
    ASSERT_NE(moving, nullptr);
    ASSERT_NE(leaf, nullptr);
    output(first_, busA_, -3.0f);
    send(first_, busB_, -6.0f);
    first_->fieldGroups.spatialRating.shared =
        localGroup(SpatialRatingFieldGroup{-0.4f, true, {}, 2.0f});
    output(second_, busB_, -9.0f);
    send(second_, busA_, -12.0f);
    second_->fieldGroups.spatialRating.shared =
        localGroup(SpatialRatingFieldGroup{0.7f, false, {}, 3.0f});
    leaf->fieldGroups.mainOutput.shared.localHistory =
        MainOutputFieldGroup{toFieldGroupGuid(busA_->guid), -17.0f};
    ASSERT_TRUE(refresh());
    const auto movingBefore = moving->fieldGroups;
    const auto leafBefore = leaf->fieldGroups;
    clean();
    ASSERT_TRUE(manager.moveItem(moving, second_));
    EXPECT_EQ(moving->parentRef.guid, second_->guid);
    EXPECT_EQ(moving->parentRef.documentId, second_->guid);
    EXPECT_EQ(leaf->parentRef.guid, moving->guid);
    for (auto* item : {moving, leaf}) {
        expectOutput(item, busB_, -9.0f);
        ASSERT_NE(item->comp<VMixNode>(), nullptr);
        ASSERT_EQ(item->comp<VMixNode>()->sends.size(), 1u);
        EXPECT_EQ(item->comp<VMixNode>()->sends.front().targetBusID, busA_->objectID);
        EXPECT_NEAR(item->comp<VMixNode>()->sends.front().sendLevel,
                    VMixNode::dbToLinear(-12.0f), 0.00001f);
        ASSERT_NE(item->comp<VSpatial>(), nullptr);
        EXPECT_FLOAT_EQ(item->comp<VSpatial>()->panning, 0.7f);
        EXPECT_FLOAT_EQ(item->comp<VSpatial>()->attenuationDistanceScaling, 3.0f);
    }
    EXPECT_EQ(moving->fieldGroups, movingBefore);
    EXPECT_EQ(leaf->fieldGroups, leafBefore);
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(moving->parent, first_);
    EXPECT_EQ(moving->parentRef.guid, first_->guid);
    expectOutput(leaf, busA_, -3.0f);
    EXPECT_FLOAT_EQ(leaf->comp<VSpatial>()->panning, -0.4f);
    UndoRedoManager::instance().undoStack()->redo();
    EXPECT_EQ(moving->parent, second_);
    EXPECT_EQ(moving->parentRef.guid, second_->guid);
    expectOutput(leaf, busB_, -9.0f);
    EXPECT_EQ(leaf->fieldGroups, leafBefore);
}

TEST_F(FieldGroupLifecycle, PlatformRoutingConflictRejectsMoveWithoutStructuralSideEffects)
{
    auto& manager = ProjectManager::instance();
    auto* moving = manager.createItem(first_, QStringLiteral("GroupMoving"), ProjectItem::Type::Sound);
    ASSERT_NE(moving, nullptr);
    moving->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        localGroup(MainOutputFieldGroup{toFieldGroupGuid(busB_->guid), -4.0f});
    second_->fieldGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        localGroup(SendFieldGroup{{{toFieldGroupGuid(busB_->guid), -8.0f}}});
    auto* selection = first_->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    ASSERT_FALSE(first_->comp<VNX::VSelection>()->selections.shared.occurrenceRows().empty());
    selection->selections.shared.find(first_->comp<VNX::VSelection>()->selections.shared.occurrenceRows().front().stableOccurrenceOrdinal)->weight = 71;
    ASSERT_TRUE(refresh());
    const auto firstChildren = first_->children;
    const auto secondChildren = second_->children;
    const auto firstOccurrences = first_->comp<VNX::VSelection>()->selections.shared;
    const auto secondOccurrences = second_->comp<VNX::VSelection>()->selections.shared;
    const auto oldParentRef = moving->parentRef;
    const auto oldSelections = selection->selections;
    const auto oldGroup = moving->fieldGroups;
    clean();
    QObject receiver;
    int moved = 0;
    QObject::connect(&manager, &ProjectManager::itemMoved, &receiver, [&] { ++moved; });
    EXPECT_FALSE(manager.moveItem(moving, second_));
    EXPECT_EQ(first_->children, firstChildren);
    EXPECT_EQ(second_->children, secondChildren);
    EXPECT_EQ(first_->comp<VNX::VSelection>()->selections.shared, firstOccurrences);
    EXPECT_EQ(second_->comp<VNX::VSelection>()->selections.shared, secondOccurrences);
    EXPECT_EQ(moving->parent, first_);
    EXPECT_EQ(moving->parentRef.kind, oldParentRef.kind);
    EXPECT_EQ(moving->parentRef.guid, oldParentRef.guid);
    EXPECT_EQ(moving->parentRef.documentId, oldParentRef.documentId);
    EXPECT_EQ(moving->fieldGroups, oldGroup);
    EXPECT_EQ(selection->selections, oldSelections);
    EXPECT_FALSE(first_->dirtyBit);
    EXPECT_FALSE(second_->dirtyBit);
    EXPECT_FALSE(moving->dirtyBit);
    EXPECT_EQ(moved, 0);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    expectOutput(moving, master_, 0.0f);
}

TEST_F(FieldGroupLifecycle, RootToChildMovePreservesLocalModesAndUndoRestoresBinding)
{
    auto& manager = ProjectManager::instance();
    auto* moving = manager.createItem(nullptr, QStringLiteral("GroupRoot"), ProjectItem::Type::Sound);
    ASSERT_NE(moving, nullptr);
    moving->fieldGroups.mainOutput.shared.mode = Mode::Local;
    moving->fieldGroups.send.shared.mode = Mode::Local;
    moving->fieldGroups.spatialRating.shared.mode = Mode::Local;
    output(second_, busA_, -5.0f);
    ASSERT_TRUE(refresh());
    const auto rootCount = project_->layers.size();
    const auto authored = moving->fieldGroups;
    clean();
    ASSERT_TRUE(manager.moveItem(moving, second_));
    EXPECT_EQ(project_->layers.size(), rootCount - 1);
    EXPECT_EQ(ProjectManager::findRootBinding(project_, moving), nullptr);
    EXPECT_EQ(moving->parentRef.kind, ParentRef::Kind::Item);
    EXPECT_EQ(moving->parentRef.guid, second_->guid);
    expectOutput(moving, master_, 0.0f);
    EXPECT_EQ(moving->fieldGroups, authored);
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(moving->parent, nullptr);
    EXPECT_EQ(moving->parentRef.kind, ParentRef::Kind::Root);
    EXPECT_EQ(project_->layers.size(), rootCount);
    EXPECT_NE(ProjectManager::findRootBinding(project_, moving), nullptr);
    expectOutput(moving, master_, 0.0f);
    EXPECT_EQ(moving->fieldGroups, authored);
    UndoRedoManager::instance().undoStack()->redo();
    EXPECT_EQ(moving->parent, second_);
    EXPECT_EQ(project_->layers.size(), rootCount - 1);
    expectOutput(moving, master_, 0.0f);
    EXPECT_EQ(moving->fieldGroups, authored);
}

TEST_F(FieldGroupLifecycle, RejectedRootToChildMoveRetainsItsBindingAndDestinationLedger)
{
    auto& manager = ProjectManager::instance();
    auto* moving = manager.createItem(nullptr, QStringLiteral("GroupRoot"), ProjectItem::Type::Sound);
    ASSERT_NE(moving, nullptr);
    output(moving, busB_, -2.0f);
    send(second_, busB_, -6.0f);
    ASSERT_TRUE(refresh());
    const auto rootCount = project_->layers.size();
    const auto occurrences = second_->comp<VNX::VSelection>()->selections.shared;
    const auto children = second_->children;
    clean();
    EXPECT_FALSE(manager.moveItem(moving, second_));
    EXPECT_EQ(moving->parent, nullptr);
    EXPECT_EQ(moving->parentRef.kind, ParentRef::Kind::Root);
    EXPECT_EQ(project_->layers.size(), rootCount);
    EXPECT_NE(ProjectManager::findRootBinding(project_, moving), nullptr);
    EXPECT_EQ(second_->children, children);
    EXPECT_EQ(second_->comp<VNX::VSelection>()->selections.shared, occurrences);
    EXPECT_FALSE(moving->dirtyBit);
    EXPECT_FALSE(second_->dirtyBit);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    expectOutput(moving, busB_, -2.0f);
}

TEST_F(FieldGroupLifecycle, ChildToRootPreservesEveryPlatformCurrentValueAcrossUndoReparentAndReload)
{
    auto& manager = ProjectManager::instance();
    auto* moving = manager.createItem(first_, QStringLiteral("GroupMoving"), ProjectItem::Type::Container);
    ASSERT_NE(moving, nullptr);
    auto* leaf = manager.createItem(moving, QStringLiteral("GroupLeaf"), ProjectItem::Type::Sound);
    ASSERT_NE(leaf, nullptr);
    output(first_, busA_, -3.0f);
    send(first_, busB_, -6.0f);
    first_->fieldGroups.spatialRating.shared =
        localGroup(SpatialRatingFieldGroup{-0.4f, true, {}, 2.0f});
    for (const auto platform : HarkenTest::fieldGroupPlatforms) {
        const float offset = platform == HarkenTest::FieldGroupPlatformIds::Windows
            || platform == HarkenTest::FieldGroupPlatformIds::iOS ? 0.0f : static_cast<float>(std::find(HarkenTest::fieldGroupPlatforms.begin(), HarkenTest::fieldGroupPlatforms.end(), platform) - HarkenTest::fieldGroupPlatforms.begin());
        first_->fieldGroups.mainOutput.platforms[platform] = localGroup(
            MainOutputFieldGroup{toFieldGroupGuid(busA_->guid), -3.0f - offset});
        first_->fieldGroups.send.platforms[platform] = localGroup(
            SendFieldGroup{{{toFieldGroupGuid(busB_->guid), -6.0f - offset}}});
        first_->fieldGroups.spatialRating.platforms[platform] = localGroup(
            SpatialRatingFieldGroup{-0.4f + offset * 0.1f, true, {}, 2.0f + offset});
    }
    const auto missing = toFieldGroupGuid(QUuid::createUuid());
    moving->fieldGroups.mainOutput.shared.localHistory = MainOutputFieldGroup{missing, -21.0f};
    moving->fieldGroups.send.shared.localHistory = SendFieldGroup{{{missing, -22.0f}}};
    moving->fieldGroups.spatialRating.shared.localHistory =
        SpatialRatingFieldGroup{0.9f, true, missing, 4.0f};
    for (const auto platform : {HarkenTest::FieldGroupPlatformIds::Windows, HarkenTest::FieldGroupPlatformIds::Android}) {
        const auto mode = platform == HarkenTest::FieldGroupPlatformIds::Windows ? Mode::Inherit : Mode::UseShared;
        moving->fieldGroups.mainOutput.platforms[platform] =
            {mode, moving->fieldGroups.mainOutput.shared.localHistory};
        moving->fieldGroups.send.platforms[platform] =
            {mode, moving->fieldGroups.send.shared.localHistory};
        moving->fieldGroups.spatialRating.platforms[platform] =
            {mode, moving->fieldGroups.spatialRating.shared.localHistory};
    }
    moving->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::macOS] = localGroup(
        MainOutputFieldGroup{toFieldGroupGuid(busB_->guid), -41.0f});
    moving->fieldGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::macOS] = localGroup(
        SendFieldGroup{{{toFieldGroupGuid(busA_->guid), -42.0f}}});
    moving->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::macOS] = localGroup(
        SpatialRatingFieldGroup{0.7f, false, {}, 3.0f});
    output(second_, busB_, -9.0f);
    send(second_, busA_, -12.0f);
    second_->fieldGroups.spatialRating.shared = localGroup(SpatialRatingFieldGroup{});
    ASSERT_TRUE(refresh());
    const auto authored = moving->fieldGroups;
    const auto leafAuthored = leaf->fieldGroups;
    const auto movingGuid = moving->guid;
    const auto movingKey = toFieldGroupGuid(movingGuid);
    const auto leafKey = toFieldGroupGuid(leaf->guid);
    std::array<FieldGroupScope, 1 + HarkenTest::fieldGroupPlatforms.size()> scopes{};
    std::array<EffectiveDefinitionFieldGroups, 1 + HarkenTest::fieldGroupPlatforms.size()> current;
    for (std::size_t i = 0; i < HarkenTest::fieldGroupPlatforms.size(); ++i)
        scopes[i + 1] = HarkenTest::fieldGroupPlatforms[i];
    for (std::size_t i = 0; i < scopes.size(); ++i) {
        const auto snapshot = buildFieldGroupEffectiveSnapshot(*project_, scopes[i]);
        ASSERT_TRUE(snapshot.ok());
        current[i] = snapshot.definitions.at(movingKey);
    }
    const auto expectCurrent = [&] {
        for (std::size_t i = 0; i < scopes.size(); ++i) {
            const auto snapshot = buildFieldGroupEffectiveSnapshot(*project_, scopes[i]);
            ASSERT_TRUE(snapshot.ok());
            for (const auto key : {movingKey, leafKey}) {
                const auto& actual = snapshot.definitions.at(key);
                EXPECT_EQ(actual.mainOutput, current[i].mainOutput);
                EXPECT_EQ(actual.send, current[i].send);
                EXPECT_EQ(actual.spatialRating, current[i].spatialRating);
            }
        }
    };
    const auto originalRows = first_->comp<VNX::VSelection>()->selections.shared.occurrenceRows();
    const auto rootCount = project_->layers.size();
    clean();
    ASSERT_TRUE(manager.moveItemToBucketRoot(moving, QStringLiteral("Layers")));
    const auto captured = moving->fieldGroups;
    EXPECT_EQ(moving->parent, nullptr);
    EXPECT_EQ(moving->parentRef.kind, ParentRef::Kind::Root);
    EXPECT_EQ(project_->layers.size(), rootCount + 1);
    EXPECT_EQ(captured.mainOutput.shared.mode, Mode::Inherit);
    EXPECT_EQ(captured.send.shared.mode, Mode::Inherit);
    EXPECT_EQ(captured.spatialRating.shared.mode, Mode::Inherit);
    for (const auto platform : {HarkenTest::FieldGroupPlatformIds::Windows, HarkenTest::FieldGroupPlatformIds::Linux,
                                HarkenTest::FieldGroupPlatformIds::Android, HarkenTest::FieldGroupPlatformIds::macOS}) {
        const auto expectedMode = platform == HarkenTest::FieldGroupPlatformIds::macOS ? Mode::Local : Mode::Inherit;
        EXPECT_EQ(ResolveFieldGroupLocal(captured.mainOutput, platform).mode, expectedMode);
        EXPECT_EQ(ResolveFieldGroupLocal(captured.send, platform).mode, expectedMode);
        EXPECT_EQ(ResolveFieldGroupLocal(captured.spatialRating, platform).mode, expectedMode);
    }
    EXPECT_NE(captured.spatialRating.platforms.at(HarkenTest::FieldGroupPlatformIds::macOS).localFields
        & FieldGroupFieldMask(harkenplayableproperties::PlayablePropertyIdV1::SpatialAttenuationDistanceScaling), 0u);
    EXPECT_FALSE(captured.mainOutput.platforms.contains(HarkenTest::FieldGroupPlatformIds::iOS));
    EXPECT_FALSE(captured.send.platforms.contains(HarkenTest::FieldGroupPlatformIds::iOS));
    EXPECT_FALSE(captured.spatialRating.platforms.contains(HarkenTest::FieldGroupPlatformIds::iOS));
    EXPECT_EQ(leaf->fieldGroups, leafAuthored);
    expectCurrent();
    auto* stack = UndoRedoManager::instance().undoStack();
    ASSERT_EQ(stack->count(), 1);
    stack->undo();
    EXPECT_EQ(moving->parent, first_);
    EXPECT_EQ(moving->parentRef.guid, first_->guid);
    EXPECT_EQ(first_->comp<VNX::VSelection>()->selections.shared.occurrenceRows(), originalRows);
    EXPECT_EQ(project_->layers.size(), rootCount);
    EXPECT_EQ(moving->fieldGroups, authored);
    EXPECT_EQ(leaf->fieldGroups, leafAuthored);
    expectCurrent();
    stack->redo();
    EXPECT_EQ(moving->parent, nullptr);
    EXPECT_EQ(moving->fieldGroups, captured);
    expectCurrent();
    ASSERT_TRUE(manager.moveItem(moving, second_));
    EXPECT_EQ(moving->fieldGroups, captured);
    for (std::size_t i = 0; i < scopes.size(); ++i) {
        const auto snapshot = buildFieldGroupEffectiveSnapshot(*project_, scopes[i]);
        ASSERT_TRUE(snapshot.ok());
        const auto& expected = scopes[i] == HarkenTest::FieldGroupPlatformIds::macOS
            ? current[i] : snapshot.definitions.at(toFieldGroupGuid(second_->guid));
        for (const auto key : {movingKey, leafKey}) {
            const auto& actual = snapshot.definitions.at(key);
            EXPECT_EQ(actual.mainOutput, expected.mainOutput);
            EXPECT_EQ(actual.send, expected.send);
            EXPECT_EQ(actual.spatialRating, expected.spatialRating);
        }
    }
    stack->undo();
    EXPECT_EQ(moving->parent, nullptr);
    EXPECT_EQ(moving->fieldGroups, captured);
    expectCurrent();
    const QString path = project_->projectPath;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(path));
    project_ = manager.currentProject();
    moving = manager.findItemByGuid(movingGuid);
    ASSERT_NE(moving, nullptr);
    EXPECT_EQ(moving->parent, nullptr);
    EXPECT_EQ(moving->fieldGroups, captured);
    expectCurrent();
}

TEST_F(FieldGroupLifecycle, RejectedChildToRootMoveRestoresSourceWithoutCreatingBinding)
{
    auto& manager = ProjectManager::instance();
    auto* moving = manager.createItem(first_, QStringLiteral("GroupMoving"), ProjectItem::Type::Sound);
    ASSERT_NE(moving, nullptr);
    output(first_, busA_, -3.0f);
    send(moving, master_, -6.0f);
    ASSERT_TRUE(refresh());
    second_->fieldGroups.mainOutput.shared = localGroup(MainOutputFieldGroup{
        toFieldGroupGuid(QUuid::createUuid()), -4.0f});
    const auto children = first_->children;
    const auto occurrences = first_->comp<VNX::VSelection>()->selections.shared;
    const auto parentRef = moving->parentRef;
    const auto rootCount = project_->layers.size();
    clean();
    EXPECT_FALSE(manager.moveItemToBucketRoot(moving, QStringLiteral("Layers")));
    EXPECT_EQ(moving->parent, first_);
    EXPECT_EQ(moving->parentRef.kind, parentRef.kind);
    EXPECT_EQ(moving->parentRef.guid, parentRef.guid);
    EXPECT_EQ(moving->parentRef.documentId, parentRef.documentId);
    EXPECT_EQ(first_->children, children);
    EXPECT_EQ(first_->comp<VNX::VSelection>()->selections.shared, occurrences);
    EXPECT_EQ(project_->layers.size(), rootCount);
    EXPECT_EQ(ProjectManager::findRootBinding(project_, moving), nullptr);
    EXPECT_FALSE(first_->dirtyBit);
    EXPECT_FALSE(moving->dirtyBit);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    expectOutput(moving, busA_, -3.0f);
}

TEST_F(FieldGroupLifecycle, DuplicateAndPasteKeepDormantHistoryAndRemintOwnedParentLinks)
{
    auto& manager = ProjectManager::instance();
    auto* source = manager.createItem(first_, QStringLiteral("GroupSource"), ProjectItem::Type::Container);
    auto* leaf = manager.createItem(source, QStringLiteral("GroupLeaf"), ProjectItem::Type::Sound);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(leaf, nullptr);
    output(first_, busA_, -2.0f);
    output(second_, busB_, -7.0f);
    const auto missing = toFieldGroupGuid(QUuid::createUuid());
    source->fieldGroups.mainOutput.shared.localHistory = MainOutputFieldGroup{missing, -13.0f};
    leaf->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        {Mode::UseShared, SpatialRatingFieldGroup{0.3f, true, missing, 4.0f}};
    ASSERT_TRUE(refresh());
    const auto sourceGroups = source->fieldGroups;
    const auto leafGroups = leaf->fieldGroups;
    clean();
    auto* duplicate = manager.duplicateItem(source);
    ASSERT_NE(duplicate, nullptr);
    ASSERT_EQ(duplicate->children.size(), 1u);
    EXPECT_NE(duplicate->guid, source->guid);
    EXPECT_EQ(duplicate->parentRef.guid, first_->guid);
    EXPECT_EQ(duplicate->children.front()->parentRef.guid, duplicate->guid);
    EXPECT_EQ(duplicate->children.front()->parentRef.documentId, duplicate->guid);
    EXPECT_EQ(duplicate->fieldGroups, sourceGroups);
    EXPECT_EQ(duplicate->children.front()->fieldGroups, leafGroups);
    expectOutput(duplicate->children.front().get(), busA_, -2.0f);
    ASSERT_EQ(manager.setClipboard({source}, ProjectManager::ClipboardOp::Copy), 1);
    const auto pasted = manager.pasteClipboard(second_);
    ASSERT_EQ(pasted.size(), 1u);
    auto* copy = pasted.front();
    ASSERT_EQ(copy->children.size(), 1u);
    EXPECT_EQ(copy->parentRef.guid, second_->guid);
    EXPECT_EQ(copy->children.front()->parentRef.guid, copy->guid);
    EXPECT_EQ(copy->fieldGroups, sourceGroups);
    EXPECT_EQ(copy->children.front()->fieldGroups, leafGroups);
    expectOutput(copy->children.front().get(), busB_, -7.0f);
    EXPECT_EQ(source->fieldGroups, sourceGroups);
    EXPECT_EQ(leaf->fieldGroups, leafGroups);
}

TEST_F(FieldGroupLifecycle, RejectedPasteKeepsDestinationAndUndoHistoryUntouched)
{
    auto& manager = ProjectManager::instance();
    auto* source = manager.createItem(first_, QStringLiteral("GroupSource"), ProjectItem::Type::Sound);
    ASSERT_NE(source, nullptr);
    output(source, busB_, -1.0f);
    send(second_, busB_, -8.0f);
    ASSERT_TRUE(refresh());
    ASSERT_EQ(manager.setClipboard({source}, ProjectManager::ClipboardOp::Copy), 1);
    const auto children = second_->children;
    const auto occurrences = second_->comp<VNX::VSelection>()->selections.shared;
    const auto sourceGroups = source->fieldGroups;
    const auto nextObject = project_->nextObjectID;
    clean();
    QObject receiver;
    int created = 0;
    QObject::connect(&manager, &ProjectManager::itemCreated, &receiver, [&] { ++created; });
    EXPECT_FALSE(manager.canPasteClipboard(second_));
    EXPECT_TRUE(manager.pasteClipboard(second_).empty());
    EXPECT_EQ(second_->children, children);
    EXPECT_EQ(second_->comp<VNX::VSelection>()->selections.shared, occurrences);
    EXPECT_EQ(source->fieldGroups, sourceGroups);
    EXPECT_FALSE(source->dirtyBit);
    EXPECT_FALSE(second_->dirtyBit);
    EXPECT_EQ(created, 0);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    EXPECT_EQ(project_->nextObjectID, nextObject);
}

TEST_F(FieldGroupLifecycle, BucketPasteCapturesRootCurrentValuesAndPreservesChildrenAndLocalPlatforms)
{
    auto& manager = ProjectManager::instance();
    auto* source = manager.createItem(first_, QStringLiteral("GroupSource"), ProjectItem::Type::Container);
    ASSERT_NE(source, nullptr);
    auto* leaf = manager.createItem(source, QStringLiteral("GroupLeaf"), ProjectItem::Type::Sound);
    ASSERT_NE(leaf, nullptr);
    output(first_, busA_, -2.0f);
    send(first_, busB_, -4.0f);
    first_->fieldGroups.spatialRating.shared =
        localGroup(SpatialRatingFieldGroup{-0.2f, true, {}, 2.0f});
    first_->fieldGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        localGroup(SendFieldGroup{{{toFieldGroupGuid(busB_->guid), -9.0f}}});
    first_->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        localGroup(SpatialRatingFieldGroup{0.3f, false, {}, 3.0f});
    source->fieldGroups.mainOutput.shared.localHistory =
        MainOutputFieldGroup{toFieldGroupGuid(busB_->guid), -15.0f};
    source->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        localGroup(MainOutputFieldGroup{toFieldGroupGuid(busA_->guid), -17.0f});
    const auto missing = toFieldGroupGuid(QUuid::createUuid());
    source->fieldGroups.send.shared.localHistory = SendFieldGroup{{{missing, -19.0f}}};
    source->fieldGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::Windows] =
        {Mode::UseShared, SendFieldGroup{{{missing, -21.0f}}}};
    leaf->fieldGroups.spatialRating.shared.localHistory =
        SpatialRatingFieldGroup{0.8f, true, missing, 8.0f};
    ASSERT_TRUE(refresh());
    const auto authored = source->fieldGroups;
    const auto leafAuthored = leaf->fieldGroups;
    clean();
    ASSERT_EQ(manager.setClipboard({source}, ProjectManager::ClipboardOp::Copy), 1);
    const auto pasted = manager.pasteClipboardAtBucketRoot(QStringLiteral("Layers"));
    ASSERT_EQ(pasted.size(), 1u);
    auto* copy = pasted.front();
    ASSERT_EQ(copy->children.size(), 1u);
    EXPECT_EQ(copy->parent, nullptr);
    EXPECT_EQ(copy->parentRef.kind, ParentRef::Kind::Root);
    EXPECT_EQ(copy->children.front()->fieldGroups, leafAuthored);
    EXPECT_EQ(copy->children.front()->parentRef.guid, copy->guid);
    EXPECT_EQ(copy->fieldGroups.mainOutput.shared.mode, Mode::Inherit);
    EXPECT_EQ(copy->fieldGroups.send.shared.mode, Mode::Inherit);
    EXPECT_EQ(ResolveFieldGroupLocal(copy->fieldGroups.mainOutput, HarkenTest::FieldGroupPlatformIds::Windows),
              ResolveFieldGroupLocal(authored.mainOutput, HarkenTest::FieldGroupPlatformIds::Windows));
    EXPECT_EQ(copy->fieldGroups.mainOutput.platforms.at(HarkenTest::FieldGroupPlatformIds::Windows).localFields,
              FieldGroupFieldMask(harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume));
    EXPECT_EQ(ResolveFieldGroupLocal(copy->fieldGroups.send, HarkenTest::FieldGroupPlatformIds::Windows).mode, Mode::Inherit);
    for (const auto platform : HarkenTest::fieldGroupPlatforms) {
        const auto snapshot = buildFieldGroupEffectiveSnapshot(*project_, platform);
        ASSERT_TRUE(snapshot.ok());
        const auto& expected = snapshot.definitions.at(toFieldGroupGuid(source->guid));
        for (const auto* definition : {copy, copy->children.front().get()}) {
            const auto& actual = snapshot.definitions.at(toFieldGroupGuid(definition->guid));
            EXPECT_EQ(actual.mainOutput, expected.mainOutput);
            EXPECT_EQ(actual.send, expected.send);
            EXPECT_EQ(actual.spatialRating, expected.spatialRating);
        }
    }
    EXPECT_EQ(source->fieldGroups, authored);
    EXPECT_EQ(leaf->fieldGroups, leafAuthored);
    const auto captured = copy->fieldGroups;
    const auto copyGuid = copy->guid;
    auto* stack = UndoRedoManager::instance().undoStack();
    ASSERT_EQ(stack->count(), 1);
    stack->undo();
    EXPECT_EQ(manager.findItemByGuid(copyGuid), nullptr);
    stack->redo();
    copy = manager.findItemByGuid(copyGuid);
    ASSERT_NE(copy, nullptr);
    EXPECT_EQ(copy->fieldGroups, captured);
    EXPECT_EQ(copy->children.front()->fieldGroups, leafAuthored);
}

TEST_F(FieldGroupLifecycle, CopiedGroupsKeepTheirAuthoredStateAndCapturedRootValues)
{
    auto& manager = ProjectManager::instance();
    auto* source = manager.createItem(first_, QStringLiteral("CopiedGroups"), ProjectItem::Type::Sound);
    ASSERT_NE(source, nullptr);
    output(first_, busA_, -2.0f);
    send(first_, busB_, -4.0f);
    output(second_, busB_, -7.0f);
    ASSERT_TRUE(refresh());
    const auto authored = source->fieldGroups;
    ASSERT_EQ(manager.setClipboard({source}, ProjectManager::ClipboardOp::Copy), 1);
    output(first_, busB_, -12.0f);
    send(first_, master_, -9.0f);
    output(source, busA_, -15.0f);
    ASSERT_TRUE(refresh());

    const auto children = manager.pasteClipboard(second_);
    ASSERT_EQ(children.size(), 1u);
    EXPECT_EQ(children.front()->fieldGroups, authored);
    expectOutput(children.front(), busB_, -7.0f);

    ASSERT_TRUE(manager.deleteItem(first_));
    const auto roots = manager.pasteClipboardAtBucketRoot(QStringLiteral("Layers"));
    ASSERT_EQ(roots.size(), 1u);
    expectOutput(roots.front(), busA_, -2.0f);
    const auto snapshot = buildFieldGroupEffectiveSnapshot(*project_, std::nullopt);
    ASSERT_TRUE(snapshot.ok());
    EXPECT_EQ(snapshot.definitions.at(toFieldGroupGuid(roots.front()->guid)).send,
              (SendFieldGroup{{{toFieldGroupGuid(busB_->guid), -4.0f}}}));
}

TEST_F(FieldGroupLifecycle, RejectedChildCreationRestoresOccurrenceRowsAndHighWater)
{
    auto& manager = ProjectManager::instance();
    second_->fieldGroups.mainOutput.shared = localGroup(MainOutputFieldGroup{
        toFieldGroupGuid(QUuid::createUuid()), -3.0f});
    const auto occurrences = second_->comp<VNX::VSelection>()->selections.shared;
    const auto children = second_->children;
    const auto nextObject = project_->nextObjectID;
    clean();
    EXPECT_EQ(manager.createItem(second_, QStringLiteral("RejectedChild"), ProjectItem::Type::Sound), nullptr);
    EXPECT_EQ(second_->comp<VNX::VSelection>()->selections.shared, occurrences);
    EXPECT_EQ(second_->children, children);
    EXPECT_EQ(project_->nextObjectID, nextObject);
    EXPECT_FALSE(second_->dirtyBit);
}

TEST_F(FieldGroupLifecycle, RejectedRootPasteAndCreationDoNotConsumeObjectIdentities)
{
    auto& manager = ProjectManager::instance();
    auto* source = manager.createItem(first_, QStringLiteral("GroupSource"), ProjectItem::Type::Sound);
    ASSERT_NE(source, nullptr);
    output(first_, busA_, -3.0f);
    send(source, master_, -6.0f);
    ASSERT_TRUE(refresh());
    source->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] =
        localGroup(MainOutputFieldGroup{toFieldGroupGuid(QUuid::createUuid()), -7.0f});
    ASSERT_EQ(manager.setClipboard({source}, ProjectManager::ClipboardOp::Copy), 1);
    const auto nextObject = project_->nextObjectID;
    const auto rootCount = project_->layers.size();
    clean();
    EXPECT_TRUE(manager.pasteClipboardAtBucketRoot(QStringLiteral("Layers")).empty());
    EXPECT_EQ(project_->nextObjectID, nextObject);
    EXPECT_EQ(project_->layers.size(), rootCount);
    second_->fieldGroups.mainOutput.shared = localGroup(MainOutputFieldGroup{
        toFieldGroupGuid(QUuid::createUuid()), -4.0f});
    EXPECT_EQ(manager.createItem(nullptr, QStringLiteral("RejectedRoot"), ProjectItem::Type::Sound), nullptr);
    EXPECT_EQ(manager.createItem(second_, QStringLiteral("RejectedChild"), ProjectItem::Type::Sound), nullptr);
    EXPECT_EQ(manager.duplicateItem(source), nullptr);
    EXPECT_EQ(project_->nextObjectID, nextObject);
    EXPECT_EQ(project_->layers.size(), rootCount);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(FieldGroupLifecycle, RejectedCutPastePreservesAnExistingRedoBranch)
{
    auto& manager = ProjectManager::instance();
    auto* source = manager.createItem(first_, QStringLiteral("GroupSource"), ProjectItem::Type::Sound);
    ASSERT_NE(source, nullptr);
    output(source, busB_, -4.0f);
    send(second_, busB_, -8.0f);
    ASSERT_TRUE(refresh());
    clean();
    auto* extra = manager.createItem(nullptr, QStringLiteral("GroupExtra"), ProjectItem::Type::Sound);
    ASSERT_NE(extra, nullptr);
    auto* stack = UndoRedoManager::instance().undoStack();
    stack->undo();
    ASSERT_TRUE(stack->canRedo());
    const auto occurrences = second_->comp<VNX::VSelection>()->selections.shared;
    const auto count = stack->count();
    const auto index = stack->index();
    ASSERT_EQ(manager.setClipboard({source}, ProjectManager::ClipboardOp::Cut), 1);
    EXPECT_FALSE(manager.canPasteClipboard(second_));
    EXPECT_TRUE(manager.pasteClipboard(second_).empty());
    EXPECT_EQ(source->parent, first_);
    EXPECT_EQ(source->parentRef.guid, first_->guid);
    EXPECT_EQ(second_->comp<VNX::VSelection>()->selections.shared, occurrences);
    EXPECT_EQ(stack->count(), count);
    EXPECT_EQ(stack->index(), index);
    EXPECT_TRUE(stack->canRedo());
    EXPECT_FALSE(manager.clipboardEmpty());
    EXPECT_EQ(manager.clipboardOp(), ProjectManager::ClipboardOp::Cut);
    stack->redo();
    EXPECT_NE(ProjectManager::findRootBinding(project_, extra), nullptr);
}

TEST_F(FieldGroupLifecycle, SuccessfulCutPasteRemainsOneUndoableOperation)
{
    auto& manager = ProjectManager::instance();
    auto* a = manager.createItem(first_, QStringLiteral("GroupCutA"), ProjectItem::Type::Sound);
    auto* b = manager.createItem(first_, QStringLiteral("GroupCutB"), ProjectItem::Type::Sound);
    ASSERT_NE(a, nullptr);
    ASSERT_NE(b, nullptr);
    output(first_, busA_, -2.0f);
    output(second_, busB_, -7.0f);
    ASSERT_TRUE(refresh());
    clean();
    ASSERT_EQ(manager.setClipboard({a, b}, ProjectManager::ClipboardOp::Cut), 2);
    ASSERT_EQ(manager.pasteClipboard(second_).size(), 2u);
    auto* stack = UndoRedoManager::instance().undoStack();
    ASSERT_EQ(stack->count(), 1);
    for (auto* item : {a, b}) {
        EXPECT_EQ(item->parent, second_);
        expectOutput(item, busB_, -7.0f);
    }
    stack->undo();
    for (auto* item : {a, b}) {
        EXPECT_EQ(item->parent, first_);
        EXPECT_EQ(item->parentRef.guid, first_->guid);
        expectOutput(item, busA_, -2.0f);
    }
    stack->redo();
    for (auto* item : {a, b}) {
        EXPECT_EQ(item->parent, second_);
        EXPECT_EQ(item->parentRef.guid, second_->guid);
        expectOutput(item, busB_, -7.0f);
    }
}

TEST_F(FieldGroupLifecycle, MixedPasteConflictRejectsEverySourceBeforeMutation)
{
    auto& manager = ProjectManager::instance();
    auto* valid = manager.createItem(first_, QStringLiteral("ValidPaste"), ProjectItem::Type::Sound);
    auto* invalid = manager.createItem(first_, QStringLiteral("InvalidPaste"), ProjectItem::Type::Sound);
    ASSERT_NE(valid, nullptr);
    ASSERT_NE(invalid, nullptr);
    output(invalid, busB_, -4.0f);
    send(second_, busB_, -8.0f);
    ASSERT_TRUE(refresh());
    clean();
    const auto before = second_->children;
    const auto nextObject = project_->nextObjectID;
    for (auto operation : {ProjectManager::ClipboardOp::Copy, ProjectManager::ClipboardOp::Cut}) {
        ASSERT_EQ(manager.setClipboard({valid, invalid}, operation), 2);
        EXPECT_FALSE(manager.canPasteClipboard(second_));
        EXPECT_TRUE(manager.pasteClipboard(second_).empty());
        EXPECT_EQ(second_->children, before);
        EXPECT_EQ(valid->parent, first_);
        EXPECT_EQ(invalid->parent, first_);
        EXPECT_EQ(project_->nextObjectID, nextObject);
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
        EXPECT_EQ(manager.clipboardCount(), 2);
    }
}

TEST_F(FieldGroupLifecycle, CopyIntoOwnDescendantValidatesTheClonedParentChain)
{
    auto& manager = ProjectManager::instance();
    auto* nested = manager.createItem(first_, QStringLiteral("NestedPaste"), ProjectItem::Type::Container);
    ASSERT_NE(nested, nullptr);
    ASSERT_TRUE(refresh());
    clean();
    ASSERT_EQ(manager.setClipboard({first_}, ProjectManager::ClipboardOp::Copy), 1);
    EXPECT_TRUE(manager.canPasteClipboard(nested));
    const auto result = manager.pasteClipboard(nested);
    ASSERT_EQ(result.size(), 1u);
    EXPECT_EQ(result.front()->parent, nested);
    EXPECT_EQ(nested->parent, first_);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
}

}
}
