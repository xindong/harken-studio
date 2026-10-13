
#include "ActionManager.h"
#include "BindingEditing.h"
#include "Components/VAction.h"
#include "Components/VMixNode.h"
#include "Components/VSelection.h"
#include "Components/VVariable.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "VariableConsumerSemantics.h"
#include "VariableSemantics.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

}

class VariableConsumerSemanticsTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
        ASSERT_TRUE(temp_.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("VariableConsumers");
        config.location = temp_.path();
        ASSERT_TRUE(manager.createProject(config));
        auto& actions = VNX::ActionManager::instance();
        if (!actions.hasAction(QStringLiteral("delete_item")))
            VNX::registerProjectActions(actions, &manager, nullptr, {});
        sound_ = manager.createItem(nullptr, QStringLiteral("HostSound"),
                                    VNX::ProjectItem::Type::Sound);
        container_ = manager.createItem(nullptr, QStringLiteral("HostContainer"),
                                        VNX::ProjectItem::Type::Container);
        child_ = manager.createItem(container_, QStringLiteral("Child"),
                                    VNX::ProjectItem::Type::Sound);
        blend_ = manager.createItem(container_, QStringLiteral("Blend"),
                                    VNX::ProjectItem::Type::Container);
        blendChild_ = manager.createItem(blend_, QStringLiteral("BlendChild"),
                                         VNX::ProjectItem::Type::Sound);
        drive_ = manager.createItem(nullptr, QStringLiteral("Drive"),
                                    VNX::ProjectItem::Type::Variable);
        unused_ = manager.createItem(nullptr, QStringLiteral("Unused"),
                                     VNX::ProjectItem::Type::Variable);
        ASSERT_NE(sound_, nullptr);
        ASSERT_NE(container_, nullptr);
        ASSERT_NE(child_, nullptr);
        ASSERT_NE(blend_, nullptr);
        ASSERT_NE(blendChild_, nullptr);
        ASSERT_NE(drive_, nullptr);
        ASSERT_NE(unused_, nullptr);
        VNX::UndoRedoManager::instance().clear();
        container_->comp<VNX::VSelection>()->mode = VNX::SelectionMode::Match;
        blend_->comp<VNX::VSelection>()->mode = VNX::SelectionMode::Blend;
    }

    bool attachCompleteDriveGraph()
    {
        auto* selection = container_->compOrCreate<VNX::VSelection>();
        if (!selection)
            return false;
        selection->mode = VNX::SelectionMode::Match;
        auto grid = selection->matchGrid();
        if (!grid.addAxis(drive_->guid))
            return false;
        if (!grid.addRow(blend_->guid))
            return false;
        if (!selection->selections.shared.setMatchGrid(grid))
            return false;
        auto* blendSelection = blend_->comp<VNX::VSelection>();
        if (!blendSelection->selections.shared.setMixing(VNX::MixingDef{
            {{drive_->guid, {VNX::MixingChildDef::unity(blendChild_->guid, 0.0f, 100.0f)}},
             {{}, {{blendChild_->guid, {}, {}}}},
             {{}, {VNX::MixingChildDef::unity(blendChild_->guid, 20.0f, 60.0f)}}}}))
            return false;
        auto binding = VNX::BindingSemantics::defaultBinding(
            VNX::BindingSemantics::volumeAddress(), drive_->guid);
        if (!VNX::BindingEditing::upsert(sound_, binding))
            return false;
        auto& manager = VNX::ProjectManager::instance();
        action_ = manager.createItem(nullptr, QStringLiteral("SetDrive"),
                                     VNX::ProjectItem::Type::Action);
        if (!action_)
            return false;
        auto* component = action_->comp<VNX::VAction>();
        if (!component)
            return false;
        VNX::VActionStep setValue;
        setValue.type = VNX::VActionStep::Type::SetVariable;
        setValue.targetObjectID = drive_->objectID;
        component->steps = {setValue};
        return true;
    }

    static void expectFourKinds(const std::vector<VNX::VariableConsumerRef>& consumers,
                                const QUuid& bindingHost,
                                const QUuid& matchHost,
                                const QUuid& blendHost,
                                const QUuid& actionHost)
    {
        ASSERT_EQ(consumers.size(), 4u);
        int bindings = 0;
        int discrete = 0;
        int continuous = 0;
        int setValue = 0;
        for (const auto& consumer : consumers) {
            if (consumer.kind == VNX::VariableConsumerKind::Binding) {
                EXPECT_EQ(consumer.hostGuid, bindingHost);
                ++bindings;
            } else if (consumer.kind == VNX::VariableConsumerKind::DiscreteGridAxis) {
                EXPECT_EQ(consumer.hostGuid, matchHost);
                ++discrete;
            } else if (consumer.kind == VNX::VariableConsumerKind::ContinuousGridAxis) {
                EXPECT_EQ(consumer.hostGuid, blendHost);
                ++continuous;
            } else if (consumer.kind == VNX::VariableConsumerKind::ActionSetVariable) {
                EXPECT_EQ(consumer.hostGuid, actionHost);
                ++setValue;
            }
        }
        EXPECT_EQ(bindings, 1);
        EXPECT_EQ(discrete, 1);
        EXPECT_EQ(continuous, 1);
        EXPECT_EQ(setValue, 1);
    }

    void TearDown() override
    {
        VNX::UndoRedoManager::instance().clear();
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
    }

    QTemporaryDir temp_;
    VNX::ProjectItem* sound_ = nullptr;
    VNX::ProjectItem* container_ = nullptr;
    VNX::ProjectItem* child_ = nullptr;
    VNX::ProjectItem* blend_ = nullptr;
    VNX::ProjectItem* blendChild_ = nullptr;
    VNX::ProjectItem* drive_ = nullptr;
    VNX::ProjectItem* unused_ = nullptr;
    VNX::ProjectItem* action_ = nullptr;
};

TEST_F(VariableConsumerSemanticsTest, AuditionVisitsOnlyActiveTransitionBridges)
{
    const auto binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), drive_->guid);
    ASSERT_TRUE(VNX::BindingEditing::upsert(sound_, binding));
    auto* selection = container_->comp<VNX::VSelection>();
    selection->transitions.rules.front().bridge = sound_->guid;
    const auto hasBridgeVariable = [&] {
        const auto dependencies = VNX::VariableConsumerSemantics::dependenciesForAudition(
            VNX::ProjectManager::instance().currentProject(), container_);
        return std::any_of(dependencies.variables.begin(), dependencies.variables.end(),
            [&](const auto* variable) { return variable->guid == drive_->guid; });
    };
    selection->mode = VNX::SelectionMode::Random;
    EXPECT_FALSE(hasBridgeVariable());
    selection->playType = VNX::SelectionPlayType::Queue;
    EXPECT_TRUE(hasBridgeVariable());
    selection->mode = VNX::SelectionMode::Blend;
    EXPECT_FALSE(hasBridgeVariable());
    selection->mode = VNX::SelectionMode::Match;
    selection->matchResponse = VNX::VariableResponse::AtStart;
    EXPECT_FALSE(hasBridgeVariable());
    selection->matchResponse = VNX::VariableResponse::Live;
    EXPECT_TRUE(hasBridgeVariable());
    selection->mode = VNX::SelectionMode::Sequence;
    selection->playType = VNX::SelectionPlayType::Single;
    EXPECT_FALSE(hasBridgeVariable());
}

TEST_F(VariableConsumerSemanticsTest, BindingAndGridAxisAndUnused)
{
    auto binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), drive_->guid);
    ASSERT_TRUE(VNX::BindingEditing::upsert(
        sound_, binding));
    auto second = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::pitchAddress(), drive_->guid);
    ASSERT_TRUE(VNX::BindingEditing::upsert(
        sound_, second));

    auto* selection = container_->compOrCreate<VNX::VSelection>();
    auto grid = selection->matchGrid();
    ASSERT_TRUE(grid.addAxis(drive_->guid));
    blend_->comp<VNX::VSelection>()->selections.shared.mixingRows.push_back({QUuid::createUuid(), drive_->guid});
    ASSERT_TRUE(grid.addRow(child_->guid));
    ASSERT_TRUE(selection->selections.shared.setMatchGrid(grid));

    const auto consumers = VNX::VariableConsumerSemantics::consumersOf(
        VNX::ProjectManager::instance().currentProject(), drive_->guid);
    ASSERT_EQ(consumers.size(), 4u);
    int bindings = 0;
    int discrete = 0;
    int continuous = 0;
    for (const auto& consumer : consumers) {
        EXPECT_EQ(consumer.hostGuid == sound_->guid
                      || consumer.hostGuid == container_->guid
                      || consumer.hostGuid == blend_->guid,
                  true);
        if (consumer.kind == VNX::VariableConsumerKind::Binding)
            ++bindings;
        else if (consumer.kind == VNX::VariableConsumerKind::DiscreteGridAxis)
            ++discrete;
        else if (consumer.kind == VNX::VariableConsumerKind::ContinuousGridAxis)
            ++continuous;
    }
    EXPECT_EQ(bindings, 2);
    EXPECT_EQ(discrete, 1);
    EXPECT_EQ(continuous, 1);

    EXPECT_TRUE(VNX::VariableConsumerSemantics::consumersOf(
                    VNX::ProjectManager::instance().currentProject(),
                    unused_->guid)
                    .empty());
    EXPECT_TRUE(VNX::VariableConsumerSemantics::consumersOf(
                    nullptr, drive_->guid)
                    .empty());
}

TEST_F(VariableConsumerSemanticsTest, FindsDiscreteCellsOutsideACandidateRange)
{
    auto* selection = container_->compOrCreate<VNX::VSelection>();
    auto grid = selection->matchGrid();
    ASSERT_TRUE(grid.addAxis(drive_->guid));
    ASSERT_TRUE(grid.addRow(child_->guid));
    ASSERT_TRUE(grid.setCell(0, 0, 2));
    ASSERT_TRUE(selection->selections.shared.setMatchGrid(grid));

    const auto invalid = VNX::VariableConsumerSemantics::invalidDiscreteGridCells(
        VNX::ProjectManager::instance().currentProject(), drive_->guid, 0.0f, 1.0f);
    ASSERT_EQ(invalid.size(), 1u);
    EXPECT_EQ(invalid.front().hostGuid, container_->guid);
    EXPECT_EQ(invalid.front().rowIndex, 0);
    EXPECT_EQ(invalid.front().axisIndex, 0);
    EXPECT_EQ(invalid.front().value, 2);

    EXPECT_TRUE(VNX::VariableConsumerSemantics::invalidDiscreteGridCells(
                    VNX::ProjectManager::instance().currentProject(),
                    drive_->guid, 0.0f, 2.0f)
                    .empty());
}

TEST_F(VariableConsumerSemanticsTest, ActionSetVariableInClosure)
{
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* action = manager.createItem(
        nullptr, QStringLiteral("SetValue_Action"),
        VNX::ProjectItem::Type::Action);
    ASSERT_NE(action, nullptr);
    auto* component = action->comp<VNX::VAction>();
    ASSERT_NE(component, nullptr);
    VNX::VActionStep setValue;
    setValue.type = VNX::VActionStep::Type::SetVariable;
    setValue.targetObjectID = drive_->objectID;
    component->steps = {setValue};

    const auto consumers = VNX::VariableConsumerSemantics::consumersOf(
        manager.currentProject(), drive_->guid);
    ASSERT_EQ(consumers.size(), 1u);
    EXPECT_EQ(consumers.front().kind, VNX::VariableConsumerKind::ActionSetVariable);
    EXPECT_EQ(consumers.front().hostGuid, action->guid);

    EXPECT_TRUE(VNX::VariableConsumerSemantics::consumersOf(
                    manager.currentProject(), unused_->guid)
                    .empty());
}

TEST_F(VariableConsumerSemanticsTest, CompleteGraphBindingMatchBlendAndSetValue)
{
    ASSERT_TRUE(attachCompleteDriveGraph());
    auto* project = VNX::ProjectManager::instance().currentProject();
    expectFourKinds(
        VNX::VariableConsumerSemantics::consumersOf(project, drive_->guid),
        sound_->guid, container_->guid, blend_->guid, action_->guid);
    EXPECT_TRUE(VNX::VariableConsumerSemantics::consumersOf(project, unused_->guid)
                    .empty());
}

TEST_F(VariableConsumerSemanticsTest, RetargetMovesConsumersToReplacement)
{
    ASSERT_TRUE(attachCompleteDriveGraph());
    auto expectedMixing = blend_->comp<VNX::VSelection>()->mixing();
    auto& manager = VNX::ProjectManager::instance();
    VNX::ProjectItem* other = manager.createItem(
        nullptr, QStringLiteral("Other"), VNX::ProjectItem::Type::Variable);
    ASSERT_NE(other, nullptr);
    expectedMixing.rows.front().variable = other->guid;
    auto* project = manager.currentProject();
    ASSERT_TRUE(VNX::VariableConsumerSemantics::retarget(
        project, drive_->guid, other->guid));
    EXPECT_TRUE(VNX::VariableConsumerSemantics::consumersOf(project, drive_->guid)
                    .empty());
    expectFourKinds(
        VNX::VariableConsumerSemantics::consumersOf(project, other->guid),
        sound_->guid, container_->guid, blend_->guid, action_->guid);
    const auto* mix = sound_->comp<VNX::VMixNode>();
    ASSERT_NE(mix, nullptr);
    ASSERT_FALSE(mix->bindings.entries().empty());
    EXPECT_EQ(mix->bindings.entries().front().variable, other->guid);
    const auto* selection = container_->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    ASSERT_FALSE(selection->matchGrid().axes.empty());
    const auto* blendSelection = blend_->comp<VNX::VSelection>();
    ASSERT_FALSE(blendSelection->mixing().rows.empty());
    EXPECT_EQ(selection->matchGrid().axes.front(), other->guid);
    EXPECT_EQ(blendSelection->mixing().rows.front().variable, other->guid);
    EXPECT_EQ(blendSelection->mixing().toJson(), expectedMixing.toJson());
    const auto* action = action_->comp<VNX::VAction>();
    ASSERT_NE(action, nullptr);
    ASSERT_FALSE(action->steps.empty());
    EXPECT_EQ(action->steps.front().targetObjectID, other->objectID);
}

TEST_F(VariableConsumerSemanticsTest, ClearRemovesConsumers)
{
    ASSERT_TRUE(attachCompleteDriveGraph());
    auto expectedMixing = blend_->comp<VNX::VSelection>()->mixing();
    expectedMixing.rows.erase(expectedMixing.rows.begin());
    auto& manager = VNX::ProjectManager::instance();
    auto* project = manager.currentProject();
    ASSERT_TRUE(VNX::VariableConsumerSemantics::clear(project, drive_->guid));
    EXPECT_TRUE(VNX::VariableConsumerSemantics::consumersOf(project, drive_->guid)
                    .empty());
    EXPECT_NE(manager.findItemByGuid(drive_->guid), nullptr);
    EXPECT_NE(manager.findItemByGuid(sound_->guid), nullptr);
    EXPECT_NE(manager.findItemByGuid(container_->guid), nullptr);
    EXPECT_NE(manager.findItemByGuid(action_->guid), nullptr);
    const auto* mix = sound_->comp<VNX::VMixNode>();
    ASSERT_NE(mix, nullptr);
    EXPECT_TRUE(mix->bindings.isEmpty());
    const auto* selection = container_->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    EXPECT_EQ(selection->matchGrid().axisIndex(drive_->guid), -1);
    EXPECT_EQ(blend_->comp<VNX::VSelection>()->mixing().rowIndexForVariable(drive_->guid), -1);
    EXPECT_EQ(blend_->comp<VNX::VSelection>()->mixing().toJson(), expectedMixing.toJson());
    const auto* action = action_->comp<VNX::VAction>();
    ASSERT_NE(action, nullptr);
    EXPECT_TRUE(action->steps.empty());
}

TEST_F(VariableConsumerSemanticsTest, DeleteItemCommandClearsReferencedVariableAndUndoRestoresBinding)
{
    auto binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), drive_->guid);
    ASSERT_TRUE(VNX::BindingEditing::upsert(sound_, binding));
    auto& manager = VNX::ProjectManager::instance();
    const auto result = VNX::ActionManager::instance().execute(
        QStringLiteral("delete_item"),
        QJsonObject{{QStringLiteral("object_id"),
                     static_cast<qint64>(drive_->objectID)}});
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_EQ(manager.findItemByGuid(drive_->guid), nullptr);
    EXPECT_TRUE(sound_->comp<VNX::VMixNode>()->bindings.isEmpty());
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    EXPECT_EQ(manager.findItemByGuid(drive_->guid), drive_);
    ASSERT_EQ(sound_->comp<VNX::VMixNode>()->bindings.size(), 1u);
    EXPECT_EQ(sound_->comp<VNX::VMixNode>()->bindings.entries().front().variable, drive_->guid);
}

class VariableDeletionLifecycleTest : public VariableConsumerSemanticsTest,
                                      public ::testing::WithParamInterface<int>
{
};

TEST_P(VariableDeletionLifecycleTest, AllEntryPointsClearReferencesAndSaveUndoPreservesUnrelatedConfiguration)
{
    ASSERT_TRUE(attachCompleteDriveGraph());
    auto& manager = VNX::ProjectManager::instance();
    auto* project = manager.currentProject();
    auto* folder = manager.createFolder(nullptr, QStringLiteral("VariablesFolder"), QStringLiteral("Variables"));
    ASSERT_NE(folder, nullptr);
    if (GetParam() == 2)
        ASSERT_TRUE(manager.moveEntriesToFolder({drive_}, {}, QStringLiteral("Variables"), project->folderRelPath(folder)));
    auto otherBinding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::pitchAddress(), unused_->guid);
    ASSERT_TRUE(VNX::BindingEditing::upsert(sound_, otherBinding));
    auto* match = container_->comp<VNX::VSelection>();
    auto grid = match->matchGrid();
    ASSERT_TRUE(grid.addAxis(unused_->guid));
    const auto driveLabel = QUuid::createUuid();
    const auto otherLabel = QUuid::createUuid();
    drive_->comp<VNX::VVariable>()->labels.push_back({3, QStringLiteral("Three"), driveLabel});
    unused_->comp<VNX::VVariable>()->labels.push_back({7, QStringLiteral("Seven"), otherLabel});
    ASSERT_TRUE(grid.setCell(0, 0, VNX::DiscreteGridCell(driveLabel)));
    ASSERT_TRUE(grid.setCell(0, 1, VNX::DiscreteGridCell(otherLabel)));
    ASSERT_TRUE(match->selections.shared.setMatchGrid(grid));
    const QString platform = VNX::ProjectPlatformSemantics::idString(project->platforms.front().id);
    match->selections.platforms.insert(platform, match->selections.shared);
    auto* blend = blend_->comp<VNX::VSelection>();
    blend->selections.platforms.insert(platform, blend->selections.shared);
    const auto bindingsBefore = sound_->comp<VNX::VMixNode>()->bindings.toJson();
    const auto matchBefore = match->selections.toJson();
    const auto blendBefore = blend->selections.toJson();
    auto expectedBlend = blend->selections;
    for (auto* table : expectedBlend.tables())
        table->mixingRows.front().variable = QUuid{};
    const auto driveGuid = drive_->guid;
    const auto soundGuid = sound_->guid;
    const auto containerGuid = container_->guid;
    const auto blendGuid = blend_->guid;
    const auto actionGuid = action_->guid;
    ASSERT_TRUE(manager.saveProject());
    VNX::UndoRedoManager::instance().clear();

    if (GetParam() == 0) {
        ASSERT_TRUE(manager.deleteItem(drive_));
    } else if (GetParam() == 1) {
        const auto result = VNX::ActionManager::instance().execute(
            QStringLiteral("delete_item"), QJsonObject{
                {QStringLiteral("object_id"), static_cast<qint64>(drive_->objectID)}});
        ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    } else if (GetParam() == 2) {
        ASSERT_TRUE(manager.deleteEntries({}, {folder}));
    } else {
        ASSERT_TRUE(manager.deleteEntries({drive_}, {folder}));
    }
    EXPECT_EQ(VNX::UndoRedoManager::instance().undoStack()->count(), 1);
    EXPECT_EQ(manager.findItemByGuid(driveGuid), nullptr);
    EXPECT_TRUE(VNX::VariableConsumerSemantics::consumersOf(project, driveGuid).empty());
    ASSERT_EQ(sound_->comp<VNX::VMixNode>()->bindings.size(), 1u);
    EXPECT_EQ(sound_->comp<VNX::VMixNode>()->bindings.entries().front().variable, unused_->guid);
    for (const auto* table : match->selections.tables()) {
        EXPECT_EQ(table->matchAxes, std::vector<QUuid>{unused_->guid});
        ASSERT_EQ(table->matchGrid().rows.size(), 1u);
        ASSERT_EQ(table->matchGrid().rows.front().cells.size(), 1u);
        EXPECT_EQ(table->matchGrid().rows.front().cells.front()->labelId, otherLabel);
    }
    EXPECT_EQ(blend->selections.toJson(), expectedBlend.toJson());
    ASSERT_EQ(action_->comp<VNX::VAction>()->steps.size(), 1u);
    EXPECT_EQ(action_->comp<VNX::VAction>()->steps.front().targetObjectID, 0u);
    for (const auto* owner : {sound_, container_, blend_, action_})
        EXPECT_TRUE(owner->dirtyBit) << owner->name.toStdString();
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    EXPECT_EQ(manager.findItemByGuid(driveGuid), drive_);
    EXPECT_EQ(sound_->comp<VNX::VMixNode>()->bindings.toJson(), bindingsBefore);
    EXPECT_EQ(match->selections.toJson(), matchBefore);
    EXPECT_EQ(blend->selections.toJson(), blendBefore);
    EXPECT_EQ(action_->comp<VNX::VAction>()->steps.front().targetObjectID, drive_->objectID);
    for (const auto* owner : {sound_, container_, blend_, action_})
        EXPECT_TRUE(owner->dirtyBit) << owner->name.toStdString();
    ASSERT_TRUE(VNX::UndoRedoManager::instance().redo());
    for (const auto* owner : {sound_, container_, blend_, action_})
        EXPECT_FALSE(owner->dirtyBit) << owner->name.toStdString();
    sound_->notes = QStringLiteral("Keep edits outside the undo stack");
    sound_->markDirty();
    emit manager.itemPropertyChanged(sound_, QStringLiteral("notes"));
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    ASSERT_TRUE(VNX::UndoRedoManager::instance().redo());
    EXPECT_TRUE(sound_->dirtyBit);
    for (const auto* owner : {container_, blend_, action_})
        EXPECT_FALSE(owner->dirtyBit) << owner->name.toStdString();
    EXPECT_EQ(sound_->notes, QStringLiteral("Keep edits outside the undo stack"));
    ASSERT_TRUE(VNX::UndoRedoManager::instance().undo());
    ASSERT_TRUE(manager.saveProject());
    const auto projectFile = project->projectPath;
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectFile));
    ASSERT_NE(manager.findItemByGuid(driveGuid), nullptr);
    EXPECT_EQ(manager.findItemByGuid(soundGuid)->comp<VNX::VMixNode>()->bindings.toJson(), bindingsBefore);
    EXPECT_EQ(manager.findItemByGuid(containerGuid)->comp<VNX::VSelection>()->selections.toJson(), matchBefore);
    EXPECT_EQ(manager.findItemByGuid(blendGuid)->comp<VNX::VSelection>()->selections.toJson(), blendBefore);
    EXPECT_EQ(manager.findItemByGuid(actionGuid)->comp<VNX::VAction>()->steps.front().targetObjectID,
              manager.findItemByGuid(driveGuid)->objectID);
}

INSTANTIATE_TEST_SUITE_P(ObjectAutomationFolderAndMixedSelection, VariableDeletionLifecycleTest,
                        ::testing::Values(0, 1, 2, 3));

TEST_F(VariableConsumerSemanticsTest, DeleteItemCommandSucceedsAfterClear)
{
    ASSERT_TRUE(attachCompleteDriveGraph());
    auto& manager = VNX::ProjectManager::instance();
    auto* project = manager.currentProject();
    ASSERT_TRUE(VNX::VariableConsumerSemantics::clear(project, drive_->guid));
    const QUuid driveGuid = drive_->guid;
    const auto result = VNX::ActionManager::instance().execute(
        QStringLiteral("delete_item"),
        QJsonObject{{QStringLiteral("object_id"),
                     static_cast<qint64>(drive_->objectID)}});
    EXPECT_TRUE(result.success);
    EXPECT_EQ(manager.findItemByGuid(driveGuid), nullptr);
}

TEST_F(VariableConsumerSemanticsTest, FileBoundVariablesScansAllFileBindingsSorted)
{
    const auto variables = VNX::VariableSemantics::fileBoundVariables(
        VNX::ProjectManager::instance().currentProject());
    ASSERT_EQ(variables.size(), 2u);
    EXPECT_EQ(variables[0]->name, QStringLiteral("Drive"));
    EXPECT_EQ(variables[1]->name, QStringLiteral("Unused"));
    EXPECT_TRUE(VNX::VariableSemantics::fileBoundVariables(nullptr).empty());
}
