#include "ActionManager.h"
#include "ActionStepEditing.h"
#include "Components/VAction.h"
#include "Components/VVariable.h"
#include "IPC/IPCServer.h"
#include "ProjectManager.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QTemporaryDir>

using namespace VNX;

namespace VNX {
uint32_t itemShortID(const ProjectItem* item);
}

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

QJsonObject stepJson(const QString& type, const ProjectItem* target)
{
    return QJsonObject{
        {QStringLiteral("type"), type},
        {QStringLiteral("targetObjectID"), static_cast<qint64>(target->objectID)},
        {QStringLiteral("targetName"), target->name},
        {QStringLiteral("delay"), 0.0}
    };
}

class ActionStepPropertyRouteTest : public ::testing::Test
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
        config.name = QStringLiteral("ActionStepPropertyRouteProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(manager.createProject(config));
        server_.setProjectManager(&manager);

        action_ = manager.createItem(nullptr, QStringLiteral("Action"),
                                     ProjectItem::Type::Action);
        sound_ = manager.createItem(nullptr, QStringLiteral("Sound"),
                                    ProjectItem::Type::Sound);
        variable_ = manager.createItem(nullptr, QStringLiteral("Variable"),
                                       ProjectItem::Type::Variable);
        container_ = manager.createItem(nullptr, QStringLiteral("Container"),
                                        ProjectItem::Type::Container);
        ASSERT_NE(action_, nullptr);
        ASSERT_NE(sound_, nullptr);
        ASSERT_NE(variable_, nullptr);
        ASSERT_NE(container_, nullptr);
        auto* declaration = variable_->comp<VVariable>();
        ASSERT_NE(declaration, nullptr);
        declaration->kind = VariableKind::Step;
        declaration->rangeMin = 0.0f;
        declaration->rangeMax = 5.0f;
        ASSERT_TRUE(manager.saveProject());
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    ActionResult setSteps(const QJsonArray& value)
    {
        return ActionManager::instance().execute(
            QStringLiteral("set_item_property"),
            {{QStringLiteral("authoring_handle"),
              static_cast<qint64>(action_->objectID)},
             {QStringLiteral("property"), QStringLiteral("steps")},
             {QStringLiteral("value"), value}},
            ActionSource::GUI);
    }

    QJsonObject ipcSetSteps(const QJsonArray& value)
    {
        return server_.dispatchCommand(
            QStringLiteral("set_item_property"),
            {{QStringLiteral("object_id"),
              static_cast<qint64>(itemShortID(action_))},
             {QStringLiteral("property"), QStringLiteral("steps")},
             {QStringLiteral("value"), value}});
    }

    QTemporaryDir tempDir_;
    IPCServer server_{nullptr};
    ProjectItem* action_ = nullptr;
    ProjectItem* sound_ = nullptr;
    ProjectItem* variable_ = nullptr;
    ProjectItem* container_ = nullptr;
};

}

TEST_F(ActionStepPropertyRouteTest, GuiAndIpcShareCanonicalLedgerErrorsAndUndo)
{
    QJsonObject setValue = stepJson(QStringLiteral("SetVariable"), variable_);
    setValue[QStringLiteral("targetName")] = QStringLiteral("Stale");
    setValue[QStringLiteral("value")] = 2;
    const QJsonArray value{setValue};

    const ActionResult gui = setSteps(value);
    ASSERT_TRUE(gui.success) << gui.errorMessage.toStdString();
    const QJsonArray canonical = gui.data.value(QStringLiteral("value")).toArray();
    EXPECT_EQ(canonical, ActionStepEditing::stepsToJson(
                             action_->comp<VAction>()->steps));
    ASSERT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);

    QUndoStack* undo = UndoRedoManager::instance().undoStack();
    undo->undo();
    EXPECT_TRUE(action_->comp<VAction>()->steps.empty());
    const QJsonObject ipc = ipcSetSteps(value);
    ASSERT_EQ(ipc.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    EXPECT_EQ(ipc.value(QStringLiteral("result")).toObject(), gui.data);
    EXPECT_EQ(ActionStepEditing::stepsToJson(action_->comp<VAction>()->steps),
              canonical);

    undo->undo();
    EXPECT_TRUE(action_->comp<VAction>()->steps.empty());
    undo->redo();
    EXPECT_EQ(ActionStepEditing::stepsToJson(action_->comp<VAction>()->steps),
              canonical);

    const int undoCount = undo->count();
    const QJsonArray invalid{
        stepJson(QStringLiteral("SetVariable"), sound_)
    };
    const ActionResult invalidGui = setSteps(invalid);
    const QJsonObject invalidIpc = ipcSetSteps(invalid);
    ASSERT_FALSE(invalidGui.success);
    EXPECT_EQ(invalidIpc.value(QStringLiteral("status")).toString(),
              QStringLiteral("error"));
    EXPECT_EQ(invalidIpc.value(QStringLiteral("code")).toString(),
              invalidGui.errorCode);
    EXPECT_EQ(invalidIpc.value(QStringLiteral("message")).toString(),
              invalidGui.errorMessage);
    EXPECT_EQ(ActionStepEditing::stepsToJson(action_->comp<VAction>()->steps),
              canonical);
    EXPECT_EQ(undo->count(), undoCount);
}

TEST_F(ActionStepPropertyRouteTest, RejectsUnknownAndWrongTargetWithoutMutation)
{
    const int undoCount = UndoRedoManager::instance().undoStack()->count();
    ActionResult result = setSteps(QJsonArray{
        QJsonObject{{QStringLiteral("type"), QStringLiteral("Unknown")}}
    });
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("INVALID_ARGS"));

    result = setSteps(QJsonArray{stepJson(QStringLiteral("SetVariable"), sound_)});
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("INVALID_TARGET"));
    EXPECT_TRUE(action_->comp<VAction>()->steps.empty());
    EXPECT_FALSE(action_->isDirtyObject());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoCount);
}

TEST_F(ActionStepPropertyRouteTest, GuiAndIpcRejectPostActionCyclesWithoutMutation)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* nested = manager.createItem(nullptr, QStringLiteral("NestedAction"),
        ProjectItem::Type::Action);
    ASSERT_NE(nested, nullptr);
    nested->comp<VAction>()->steps = ActionStepEditing::stepsFromJson(
        QJsonArray{stepJson(QStringLiteral("PostAction"), action_)});
    nested->markDirty();
    ASSERT_TRUE(manager.saveProject());
    UndoRedoManager::instance().clear();
    const QJsonArray replacement{stepJson(QStringLiteral("PostAction"), nested)};

    const auto gui = setSteps(replacement);
    EXPECT_FALSE(gui.success);
    EXPECT_EQ(gui.errorCode, QStringLiteral("POST_ACTION_CYCLE"));
    EXPECT_TRUE(gui.errorMessage.contains(QStringLiteral("Action -> NestedAction -> Action")));
    const auto ipc = ipcSetSteps(replacement);
    EXPECT_EQ(ipc.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
    EXPECT_EQ(ipc.value(QStringLiteral("code")).toString(), QStringLiteral("POST_ACTION_CYCLE"));
    EXPECT_TRUE(action_->comp<VAction>()->steps.empty());
    EXPECT_FALSE(action_->isDirtyObject());
    EXPECT_FALSE(nested->isDirtyObject());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(ActionStepPropertyRouteTest, ReplacementCanRepairAnExistingPostActionCycle)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* nested = manager.createItem(nullptr, QStringLiteral("NestedAction"),
        ProjectItem::Type::Action);
    ASSERT_NE(nested, nullptr);
    action_->comp<VAction>()->steps = ActionStepEditing::stepsFromJson(
        QJsonArray{stepJson(QStringLiteral("PostAction"), nested)});
    nested->comp<VAction>()->steps = ActionStepEditing::stepsFromJson(
        QJsonArray{stepJson(QStringLiteral("PostAction"), action_)});
    action_->markDirty();
    nested->markDirty();
    ASSERT_TRUE(manager.saveProject());
    UndoRedoManager::instance().clear();

    const auto oldSteps = ActionStepEditing::stepsToJson(action_->comp<VAction>()->steps);
    const auto repaired = setSteps(QJsonArray{});
    ASSERT_TRUE(repaired.success) << repaired.errorMessage.toStdString();
    EXPECT_TRUE(action_->comp<VAction>()->steps.empty());
    EXPECT_EQ(nested->comp<VAction>()->steps.size(), 1u);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(ActionStepEditing::stepsToJson(action_->comp<VAction>()->steps), oldSteps);
    UndoRedoManager::instance().undoStack()->redo();
    EXPECT_TRUE(action_->comp<VAction>()->steps.empty());
}

TEST_F(ActionStepPropertyRouteTest, CanonicalizesNamesAndSetValueRange)
{
    QJsonObject setValue = stepJson(QStringLiteral("SetVariable"), variable_);
    setValue[QStringLiteral("targetName")] = QStringLiteral("Stale");
    setValue[QStringLiteral("value")] = 2;
    const ActionResult result = setSteps(QJsonArray{setValue});
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();

    const auto& steps = action_->comp<VAction>()->steps;
    ASSERT_EQ(steps.size(), 1u);
    EXPECT_EQ(steps[0].targetName, variable_->name);
    EXPECT_FLOAT_EQ(steps[0].value, 2.0f);
    EXPECT_EQ(result.data.value(QStringLiteral("value")).toArray(),
              ActionStepEditing::stepsToJson(steps));

    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_TRUE(action_->comp<VAction>()->steps.empty());
    UndoRedoManager::instance().undoStack()->redo();
    ASSERT_EQ(action_->comp<VAction>()->steps.size(), 1u);
    EXPECT_FLOAT_EQ(action_->comp<VAction>()->steps[0].value, 2.0f);
}

TEST_F(ActionStepPropertyRouteTest, SetValueDiscreteNonIntegerIsInvalidValue)
{
    QJsonObject illegal = stepJson(QStringLiteral("SetVariable"), variable_);
    illegal[QStringLiteral("value")] = 1.5;
    const ActionResult rejected = setSteps(QJsonArray{illegal});
    EXPECT_FALSE(rejected.success);
    EXPECT_EQ(rejected.errorCode, QStringLiteral("INVALID_VALUE"));
    EXPECT_TRUE(action_->comp<VAction>()->steps.empty());
    EXPECT_FALSE(action_->isDirtyObject());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);

    QJsonObject legal = stepJson(QStringLiteral("SetVariable"), variable_);
    legal[QStringLiteral("value")] = 2;
    const ActionResult applied = setSteps(QJsonArray{legal});
    ASSERT_TRUE(applied.success) << applied.errorMessage.toStdString();
    ASSERT_EQ(action_->comp<VAction>()->steps.size(), 1u);
    EXPECT_FLOAT_EQ(action_->comp<VAction>()->steps[0].value, 2.0f);
}

TEST_F(ActionStepPropertyRouteTest, CanonicalizesResolvableNameOnlyReferences)
{
    QJsonObject setValue{
        {QStringLiteral("type"), QStringLiteral("SetVariable")},
        {QStringLiteral("targetObjectID"), 0},
        {QStringLiteral("targetName"), variable_->name},
        {QStringLiteral("delay"), 0.0},
        {QStringLiteral("value"), 2}
    };
    ActionResult result = setSteps(QJsonArray{setValue});
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    ASSERT_EQ(action_->comp<VAction>()->steps.size(), 1u);
    EXPECT_EQ(action_->comp<VAction>()->steps[0].targetObjectID,
              variable_->objectID);
    EXPECT_FLOAT_EQ(action_->comp<VAction>()->steps[0].value, 2.0f);

    QJsonObject play{
        {QStringLiteral("type"), QStringLiteral("Play")},
        {QStringLiteral("targetObjectID"), 0},
        {QStringLiteral("targetName"), sound_->name},
        {QStringLiteral("delay"), 0.0},
        {QStringLiteral("quantize"), QJsonObject{
            {QStringLiteral("referenceObjectID"), 0},
            {QStringLiteral("referenceName"), container_->name},
            {QStringLiteral("timing"), QStringLiteral("NextMarker")},
            {QStringLiteral("ifSilent"), QStringLiteral("Immediate")}
        }}
    };
    result = setSteps(QJsonArray{play});
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    ASSERT_EQ(action_->comp<VAction>()->steps.size(), 1u);
    EXPECT_EQ(action_->comp<VAction>()->steps[0].targetObjectID,
              sound_->objectID);
    EXPECT_EQ(action_->comp<VAction>()->steps[0].quantize.referenceObjectID,
              container_->objectID);
}

TEST_F(ActionStepPropertyRouteTest, ValidatesResetTargetAndCanonicalizesName)
{
    QJsonObject reset = stepJson(QStringLiteral("Reset"), sound_);
    reset[QStringLiteral("resetScope")] = QStringLiteral("Global");
    ActionResult result = setSteps(QJsonArray{reset});
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("INVALID_TARGET"));
    EXPECT_TRUE(action_->comp<VAction>()->steps.empty());

    reset = stepJson(QStringLiteral("Reset"), container_);
    reset[QStringLiteral("targetName")] = QStringLiteral("Stale");
    reset[QStringLiteral("resetScope")] = QStringLiteral("Global");
    result = setSteps(QJsonArray{reset});
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    ASSERT_EQ(action_->comp<VAction>()->steps.size(), 1u);
    EXPECT_EQ(action_->comp<VAction>()->steps[0].targetObjectID,
              container_->objectID);
    EXPECT_EQ(action_->comp<VAction>()->steps[0].targetName, container_->name);
    EXPECT_EQ(action_->comp<VAction>()->steps[0].resetScope, ResetScope::Global);
}

TEST_F(ActionStepPropertyRouteTest, RejectsAmbiguousQuantizeNameWithoutMutation)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* duplicate = manager.createItem(
        nullptr, QStringLiteral("DuplicateClock"), ProjectItem::Type::Track);
    ASSERT_NE(duplicate, nullptr);
    duplicate->name = container_->name;
    UndoRedoManager::instance().clear();

    QJsonObject play = stepJson(QStringLiteral("Play"), sound_);
    play[QStringLiteral("quantize")] = QJsonObject{
        {QStringLiteral("referenceObjectID"), 0},
        {QStringLiteral("referenceName"), container_->name},
        {QStringLiteral("timing"), QStringLiteral("NextMarker")},
        {QStringLiteral("ifSilent"), QStringLiteral("Immediate")}
    };
    const ActionResult result = setSteps(QJsonArray{play});
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("AUTHORING_RESTRICTED"));
    EXPECT_TRUE(action_->comp<VAction>()->steps.empty());
    EXPECT_FALSE(action_->isDirtyObject());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(ActionStepPropertyRouteTest, RejectsDanglingPostActionQuantizeReferenceWithoutMutation)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* posted = manager.createItem(
        nullptr, QStringLiteral("Posted"), ProjectItem::Type::Action);
    ASSERT_NE(posted, nullptr);
    UndoRedoManager::instance().clear();

    QJsonObject post = stepJson(QStringLiteral("PostAction"), posted);
    post[QStringLiteral("quantize")] = QJsonObject{
        {QStringLiteral("referenceObjectID"),
         static_cast<qint64>(UINT32_MAX)},
        {QStringLiteral("referenceName"), QStringLiteral("MissingClock")},
        {QStringLiteral("timing"), QStringLiteral("NextBeat")},
        {QStringLiteral("ifSilent"), QStringLiteral("Immediate")}
    };
    const ActionResult result = setSteps(QJsonArray{post});
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("AUTHORING_RESTRICTED"));
    EXPECT_TRUE(action_->comp<VAction>()->steps.empty());
    EXPECT_FALSE(action_->isDirtyObject());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(ActionStepPropertyRouteTest, ValidatesReferenceAndAllowsTimingBeforeReference)
{
    QJsonObject play = stepJson(QStringLiteral("Play"), sound_);
    play[QStringLiteral("quantize")] = QJsonObject{
        {QStringLiteral("referenceObjectID"),
         static_cast<qint64>(variable_->objectID)},
        {QStringLiteral("referenceName"), variable_->name},
        {QStringLiteral("timing"), QStringLiteral("NextMarker")},
        {QStringLiteral("ifSilent"), QStringLiteral("Immediate")}
    };
    ActionResult result = setSteps(QJsonArray{play});
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("INVALID_TARGET"));

    QJsonObject quantize = play.value(QStringLiteral("quantize")).toObject();
    quantize[QStringLiteral("referenceObjectID")] = 0;
    quantize[QStringLiteral("referenceName")] = QString();
    play[QStringLiteral("quantize")] = quantize;
    result = setSteps(QJsonArray{play});
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_EQ(action_->comp<VAction>()->steps[0].quantize.timing, QuantizeTiming::NextMarker);
    EXPECT_EQ(action_->comp<VAction>()->steps[0].quantize.referenceObjectID, 0u);

    quantize[QStringLiteral("referenceObjectID")] =
        static_cast<qint64>(container_->objectID);
    quantize[QStringLiteral("referenceName")] = QStringLiteral("Stale");
    play[QStringLiteral("quantize")] = quantize;
    result = setSteps(QJsonArray{play});
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_EQ(action_->comp<VAction>()->steps[0].quantize.referenceName,
              container_->name);
}

TEST_F(ActionStepPropertyRouteTest, StopMayUseTargetItselfClock)
{
    QJsonObject stop = stepJson(QStringLiteral("Stop"), sound_);
    stop[QStringLiteral("quantize")] = QJsonObject{
        {QStringLiteral("referenceObjectID"), 0},
        {QStringLiteral("referenceName"), QString()},
        {QStringLiteral("timing"), QStringLiteral("ExitMarker")},
        {QStringLiteral("ifSilent"), QStringLiteral("Immediate")}
    };
    const ActionResult result = setSteps(QJsonArray{stop});
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    ASSERT_EQ(action_->comp<VAction>()->steps.size(), 1u);
    EXPECT_EQ(action_->comp<VAction>()->steps[0].quantize.timing,
              QuantizeTiming::ExitMarker);
}
