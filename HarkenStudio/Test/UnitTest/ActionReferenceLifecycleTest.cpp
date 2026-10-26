#include "ActionManager.h"
#include "Components/VAction.h"
#include "ProjectManager.h"
#include "PhysicalFolderReferenceEdits.h"
#include "TransitionSemantics.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QTemporaryDir>

using namespace VNX;

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

TEST(ActionReferenceDiskLifecycleTest, RemovingBridgePreservesValidDefaultTransition)
{
    const QUuid target = QUuid::createUuid();
    auto transitions = canonicalTransitionTable();
    transitions.rules.front().bridge = target;
    const QJsonObject content{{QStringLiteral("components"), QJsonObject{
        {QStringLiteral("selection"), QJsonObject{
            {QStringLiteral("transitions"), transitions.toJson()}}}}}};
    const auto cleaned = PhysicalFolderReferenceEdits::removeReferences(content, {target});
    const auto parsed = TransitionTableDef::fromJson(cleaned.value(QStringLiteral("components"))
        .toObject().value(QStringLiteral("selection")).toObject()
        .value(QStringLiteral("transitions")).toObject());
    ASSERT_TRUE(parsed.has_value());
    ASSERT_EQ(parsed->rules.size(), 1u);
    EXPECT_FALSE(parsed->rules.front().bridge.has_value());
    auto restored = cleaned;
    EXPECT_TRUE(PhysicalFolderReferenceEdits::mergeReferences(restored, cleaned, content));
    EXPECT_EQ(restored, content);
}

class ActionReferenceLifecycleTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(), &manager, nullptr, {});
        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("ActionReferenceLifecycleProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(manager.createProject(config));

        target_ = manager.createItem(nullptr, QStringLiteral("ClockOld"),
                                     ProjectItem::Type::Piece);
        duplicate_ = manager.createItem(nullptr, QStringLiteral("OtherClock"),
                                        ProjectItem::Type::Sound);
        action_ = manager.createItem(nullptr, QStringLiteral("Action"),
                                     ProjectItem::Type::Action);
        ASSERT_NE(target_, nullptr);
        ASSERT_NE(duplicate_, nullptr);
        ASSERT_NE(action_, nullptr);
        VActionStep step;
        step.type = VActionStep::Type::Play;
        step.targetObjectID = target_->objectID;
        step.targetName = target_->name;
        step.quantize.referenceObjectID = target_->objectID;
        step.quantize.referenceName = target_->name;
        step.quantize.timing = QuantizeTiming::NextMarker;
        action_->comp<VAction>()->steps = {step};
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    QTemporaryDir tempDir_;
    ProjectItem* target_ = nullptr;
    ProjectItem* duplicate_ = nullptr;
    ProjectItem* action_ = nullptr;
};


}

TEST_F(ActionReferenceLifecycleTest, RenameUpdatesTargetAndQuantizeFallbackNames)
{
    auto& manager = ProjectManager::instance();
    duplicate_->name = QStringLiteral("ClockOld");
    ASSERT_TRUE(manager.renameItem(target_, QStringLiteral("ClockNew")));
    const VActionStep& step = action_->comp<VAction>()->steps.front();
    EXPECT_EQ(step.targetName, QStringLiteral("ClockNew"));
    EXPECT_EQ(step.quantize.referenceName, QStringLiteral("ClockNew"));

    duplicate_->name = QStringLiteral("OtherClock");
    QUndoStack* undo = UndoRedoManager::instance().undoStack();
    ASSERT_TRUE(undo->canUndo());
    undo->undo();
    EXPECT_EQ(action_->comp<VAction>()->steps.front().targetName,
              QStringLiteral("ClockOld"));
    EXPECT_EQ(action_->comp<VAction>()->steps.front().quantize.referenceName,
              QStringLiteral("ClockOld"));
    undo->redo();
    EXPECT_EQ(action_->comp<VAction>()->steps.front().targetName,
              QStringLiteral("ClockNew"));
    EXPECT_EQ(action_->comp<VAction>()->steps.front().quantize.referenceName,
              QStringLiteral("ClockNew"));
}

TEST_F(ActionReferenceLifecycleTest, DeleteClearsActionTargetAndTimingAndUndoRestoresBoth)
{
    auto& manager = ProjectManager::instance();
    const auto targetGuid = target_->guid;
    action_->clearDirty();
    action_->comp<VAction>()->steps.front().delay = 27.0f;
    ASSERT_TRUE(manager.deleteItem(target_));
    EXPECT_EQ(manager.findItemByGuid(targetGuid), nullptr);
    ASSERT_EQ(action_->comp<VAction>()->steps.size(), 1u);
    const auto& cleared = action_->comp<VAction>()->steps.front();
    EXPECT_EQ(cleared.targetObjectID, 0u);
    EXPECT_TRUE(cleared.targetName.isEmpty());
    EXPECT_EQ(cleared.quantize.referenceObjectID, 0u);
    EXPECT_TRUE(cleared.quantize.referenceName.isEmpty());
    EXPECT_EQ(cleared.delay, 27.0f);
    EXPECT_TRUE(action_->dirtyBit);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(manager.findItemByGuid(targetGuid), target_);
    const auto& restored = action_->comp<VAction>()->steps.front();
    EXPECT_EQ(restored.targetObjectID, target_->objectID);
    EXPECT_EQ(restored.targetName, target_->name);
    EXPECT_EQ(restored.quantize.referenceObjectID, target_->objectID);
    EXPECT_EQ(restored.quantize.referenceName, target_->name);
    EXPECT_EQ(restored.delay, 27.0f);
}

TEST_F(ActionReferenceLifecycleTest, RenameCannotActivateLatentPostActionCycle)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* nested = manager.createItem(nullptr, QStringLiteral("Nested"),
                                            ProjectItem::Type::Action);
    ASSERT_NE(nested, nullptr);
    VActionStep pending;
    pending.type = VActionStep::Type::PostAction;
    pending.targetName = QStringLiteral("Pending");
    pending.delay = 500.0f;
    action_->comp<VAction>()->steps = {pending};
    VActionStep back = pending;
    back.targetObjectID = action_->objectID;
    back.targetName = action_->name;
    nested->comp<VAction>()->steps = {back};
    UndoRedoManager::instance().clear();
    nested->clearDirty();
    action_->clearDirty();

    QString error;
    EXPECT_FALSE(manager.validateItemRename(nested, QStringLiteral("Pending"), &error));
    EXPECT_TRUE(error.contains(QStringLiteral("cycle"), Qt::CaseInsensitive));
    EXPECT_FALSE(manager.renameItem(nested, QStringLiteral("Pending"), &error));
    EXPECT_EQ(nested->name, QStringLiteral("Nested"));
    EXPECT_FALSE(nested->dirtyBit);
    EXPECT_FALSE(action_->dirtyBit);
    EXPECT_FALSE(UndoRedoManager::instance().undoStack()->canUndo());
    EXPECT_EQ(action_->comp<VAction>()->steps.front().targetObjectID, 0u);
    EXPECT_EQ(action_->comp<VAction>()->steps.front().targetName, QStringLiteral("Pending"));

    nested->comp<VAction>()->steps.clear();
    EXPECT_TRUE(manager.renameItem(nested, QStringLiteral("Pending"), &error))
        << error.toStdString();
}

TEST_F(ActionReferenceLifecycleTest, ExistingPostActionCycleDoesNotBlockUnchangedRename)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* nested = manager.createItem(nullptr, QStringLiteral("Nested"),
                                            ProjectItem::Type::Action);
    ASSERT_NE(nested, nullptr);
    VActionStep forward;
    forward.type = VActionStep::Type::PostAction;
    forward.targetObjectID = nested->objectID;
    forward.targetName = nested->name;
    action_->comp<VAction>()->steps = {forward};
    VActionStep back = forward;
    back.targetObjectID = action_->objectID;
    back.targetName = action_->name;
    nested->comp<VAction>()->steps = {back};

    QString error;
    EXPECT_TRUE(manager.renameItem(nested, QStringLiteral("Renamed"), &error))
        << error.toStdString();
    EXPECT_EQ(action_->comp<VAction>()->steps.front().targetName, QStringLiteral("Renamed"));
}

TEST_F(ActionReferenceLifecycleTest, NewActionTargetingRejectsExistingPostActionCycleWithoutMutation)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* nested = manager.createItem(nullptr, QStringLiteral("Nested"),
                                            ProjectItem::Type::Action);
    ProjectItem* valid = manager.createItem(nullptr, QStringLiteral("Valid"),
                                           ProjectItem::Type::Action);
    ASSERT_NE(nested, nullptr);
    ASSERT_NE(valid, nullptr);
    VActionStep forward;
    forward.type = VActionStep::Type::PostAction;
    forward.targetObjectID = nested->objectID;
    forward.targetName = nested->name;
    forward.delay = 500.0f;
    action_->comp<VAction>()->steps = {forward};
    VActionStep back = forward;
    back.targetObjectID = action_->objectID;
    back.targetName = action_->name;
    nested->comp<VAction>()->steps = {back};
    UndoRedoManager::instance().clear();
    const auto* project = manager.currentProject();
    const auto actionCount = project->actions.size();
    const auto nextObjectId = project->nextObjectID;

    EXPECT_FALSE(manager.canCreateActionsTargeting({action_}, VActionStep::Type::PostAction));
    EXPECT_FALSE(manager.canCreateActionsTargeting({valid, action_}, VActionStep::Type::PostAction));
    EXPECT_EQ(manager.createActionTargeting(action_, VActionStep::Type::PostAction,
                                            QStringLiteral("Trigger")), nullptr);
    EXPECT_TRUE(manager.createActionsTargeting({valid, action_},
                                               VActionStep::Type::PostAction).empty());
    EXPECT_EQ(project->actions.size(), actionCount);
    EXPECT_EQ(project->nextObjectID, nextObjectId);
    EXPECT_FALSE(UndoRedoManager::instance().undoStack()->canUndo());

    nested->comp<VAction>()->steps.clear();
    EXPECT_TRUE(manager.canCreateActionsTargeting({action_}, VActionStep::Type::PostAction));
    EXPECT_NE(manager.createActionTargeting(action_, VActionStep::Type::PostAction,
                                            QStringLiteral("Trigger")), nullptr);
}



TEST_F(ActionReferenceLifecycleTest, FolderReferenceRollbackPreservesUnrelatedMemoryChanges)
{
    auto* project = ProjectManager::instance().currentProject();
    PackGroup group;
    group.name = QStringLiteral("Before");
    group.actionIds = {target_->runtimeId(), duplicate_->runtimeId()};
    project->packGroups.push_back(group);
    const auto log = ProjectManager::cleanupReferencesToDeletedSubtree(target_, project);
    EXPECT_EQ(action_->comp<VAction>()->steps.front().targetObjectID, 0u);
    EXPECT_EQ(action_->comp<VAction>()->steps.front().quantize.referenceObjectID, 0u);
    ASSERT_EQ(project->packGroups.front().actionIds.size(), 1u);
    action_->comp<VAction>()->steps.front().delay = 27.0f;
    project->description = QStringLiteral("After");
    ProjectManager::restoreReferencesFromLog(log, project);
    EXPECT_EQ(action_->comp<VAction>()->steps.front().targetObjectID, target_->objectID);
    EXPECT_EQ(action_->comp<VAction>()->steps.front().quantize.referenceObjectID, target_->objectID);
    EXPECT_EQ(action_->comp<VAction>()->steps.front().delay, 27.0f);
    EXPECT_EQ(project->description, QStringLiteral("After"));
    EXPECT_EQ(project->packGroups.front().actionIds, group.actionIds);
}

TEST(ActionReferenceDiskLifecycleTest, UnidentifiedRowsRequireUniqueUnchangedContext)
{
    const QJsonObject reference{{QStringLiteral("id"), QUuid::createUuid().toString(QUuid::WithoutBraces)}};
    const QJsonObject first{{QStringLiteral("type"), QStringLiteral("Play")},
        {QStringLiteral("delay"), 1}, {QStringLiteral("targetRef"), reference}};
    const QJsonObject second{{QStringLiteral("type"), QStringLiteral("Stop")}, {QStringLiteral("delay"), 2}};
    auto removed = first;
    removed.remove(QStringLiteral("targetRef"));
    const QJsonObject before{{QStringLiteral("actions"), QJsonArray{first, second}}};
    const QJsonObject after{{QStringLiteral("actions"), QJsonArray{removed, second}}};
    QJsonObject reordered{{QStringLiteral("actions"), QJsonArray{second, removed,
        QJsonObject{{QStringLiteral("type"), QStringLiteral("Reset")}, {QStringLiteral("delay"), 3}}}}};
    ASSERT_TRUE(PhysicalFolderReferenceEdits::mergeReferences(reordered, after, before));
    EXPECT_EQ(reordered.value(QStringLiteral("actions")).toArray().at(1).toObject(), first);
    EXPECT_EQ(reordered.value(QStringLiteral("actions")).toArray().size(), 3);
    QJsonObject ambiguous{{QStringLiteral("actions"), QJsonArray{removed, removed}}};
    const auto original = ambiguous;
    EXPECT_FALSE(PhysicalFolderReferenceEdits::mergeReferences(ambiguous, after, before));
    EXPECT_EQ(ambiguous, original);
}
