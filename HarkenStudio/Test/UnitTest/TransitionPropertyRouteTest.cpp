#include "ActionManager.h"
#include "AuthoringPolicy.h"
#include "Components/VPiece.h"
#include "Components/VSelection.h"
#include "IPC/IPCServer.h"
#include "ProjectManager.h"
#include "TransitionSemantics.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QTemporaryDir>

namespace VNX {
uint32_t itemShortID(const ProjectItem* item);
}

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

class TransitionPropertyRouteTest : public ::testing::Test
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
        config.name = QStringLiteral("TransitionPropertyRouteProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(manager.createProject(config));
        server_.setProjectManager(&manager);

        layer_ = manager.createItem(nullptr, QStringLiteral("RouteLayer"),
                                    ProjectItem::Type::Layer);
        container_ = manager.createItem(layer_, QStringLiteral("RouteContainer"),
                                        ProjectItem::Type::Container);
        childA_ = manager.createItem(container_, QStringLiteral("ChildA"),
                                    ProjectItem::Type::Sound);
        childB_ = manager.createItem(container_, QStringLiteral("ChildB"),
                                    ProjectItem::Type::Piece);
        external_ = manager.createItem(nullptr, QStringLiteral("External"),
                                       ProjectItem::Type::Track);
        invalidBridge_ = manager.createItem(nullptr, QStringLiteral("InvalidBridge"),
                                            ProjectItem::Type::Variable);
        ASSERT_NE(layer_, nullptr);
        ASSERT_NE(container_, nullptr);
        ASSERT_NE(childA_, nullptr);
        ASSERT_NE(childB_, nullptr);
        ASSERT_NE(external_, nullptr);
        ASSERT_NE(invalidBridge_, nullptr);
        UndoRedoManager::instance().clear();
        container_->dirtyBit = false;
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    TransitionTableDef table(const std::optional<QUuid>& source,
                             const std::optional<QUuid>& target,
                             const std::optional<QUuid>& bridge = std::nullopt) const
    {
        TransitionTableDef result = AuthoringPolicy::defaultTransitionTable();
        TransitionRuleDef rule = AuthoringPolicy::defaultTransitionRule();
        rule.source = source;
        rule.target = target;
        rule.bridge = bridge;
        EXPECT_TRUE(result.addRule(rule));
        return result;
    }

    ActionResult guiSet(const QJsonValue& value,
                        const QString& property = QStringLiteral("transitions"))
    {
        return ActionManager::instance().execute(
            QStringLiteral("set_item_property"),
            {{QStringLiteral("authoring_handle"),
              static_cast<qint64>(container_->objectID)},
             {QStringLiteral("property"), property},
             {QStringLiteral("value"), value}},
            ActionSource::GUI);
    }

    QJsonObject ipcSet(const QJsonValue& value)
    {
        return server_.dispatchCommand(
            QStringLiteral("set_item_property"),
            {{QStringLiteral("object_id"),
              static_cast<qint64>(itemShortID(container_))},
             {QStringLiteral("property"), QStringLiteral("transitions")},
             {QStringLiteral("value"), value}});
    }

    QJsonObject ipcGet()
    {
        return server_.dispatchCommand(
            QStringLiteral("get_item_info"),
            {{QStringLiteral("object_id"),
              static_cast<qint64>(itemShortID(container_))}});
    }

    QTemporaryDir tempDir_;
    IPCServer server_{nullptr};
    ProjectItem* layer_ = nullptr;
    ProjectItem* container_ = nullptr;
    ProjectItem* childA_ = nullptr;
    ProjectItem* childB_ = nullptr;
    ProjectItem* external_ = nullptr;
    ProjectItem* invalidBridge_ = nullptr;
};

}

TEST_F(TransitionPropertyRouteTest, GuiAndIpcShareCanonicalLedgerAndUndo)
{
    const QJsonObject value = table(childA_->guid, childB_->guid,
                                    external_->guid).toJson();
    const ActionResult gui = guiSet(value);
    ASSERT_TRUE(gui.success) << gui.errorMessage.toStdString();
    EXPECT_EQ(gui.data.value(QStringLiteral("value")).toObject(), value);
    EXPECT_EQ(container_->comp<VSelection>()->transitions.toJson(), value);
    EXPECT_TRUE(container_->isDirtyObject());
    ASSERT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);

    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(container_->comp<VSelection>()->transitions,
              AuthoringPolicy::defaultTransitionTable());
    UndoRedoManager::instance().undoStack()->redo();
    EXPECT_EQ(container_->comp<VSelection>()->transitions.toJson(), value);

    UndoRedoManager::instance().undoStack()->undo();
    const QJsonObject ipc = ipcSet(value);
    ASSERT_EQ(ipc.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    EXPECT_EQ(ipc.value(QStringLiteral("result")).toObject(), gui.data);

    const QJsonObject info = ipcGet();
    ASSERT_EQ(info.value(QStringLiteral("status")).toString(),
              QStringLiteral("ok"));
    EXPECT_EQ(info.value(QStringLiteral("result")).toObject()
                  .value(QStringLiteral("components")).toObject()
                  .value(QStringLiteral("selection")).toObject()
                  .value(QStringLiteral("transitions")).toObject(),
              value);
}

TEST_F(TransitionPropertyRouteTest, RejectsInvalidLedgerAtomically)
{
    const TransitionTableDef before = container_->comp<VSelection>()->transitions;
    const int undoCount = UndoRedoManager::instance().undoStack()->count();

    ActionResult gui = guiSet(QJsonArray{});
    EXPECT_FALSE(gui.success);
    EXPECT_EQ(gui.errorCode, QStringLiteral("INVALID_ARGS"));

    QJsonObject missingDefault = table(childA_->guid, childB_->guid).toJson();
    QJsonArray rules = missingDefault.value(QStringLiteral("rules")).toArray();
    rules.removeFirst();
    missingDefault[QStringLiteral("rules")] = rules;
    gui = guiSet(missingDefault);
    EXPECT_FALSE(gui.success);
    EXPECT_EQ(gui.errorCode, QStringLiteral("INVALID_ARGS"));

    const QJsonObject ipc = ipcSet(missingDefault);
    EXPECT_EQ(ipc.value(QStringLiteral("status")).toString(),
              QStringLiteral("error"));
    EXPECT_EQ(ipc.value(QStringLiteral("code")).toString(), gui.errorCode);
    EXPECT_EQ(ipc.value(QStringLiteral("message")).toString(), gui.errorMessage);

    EXPECT_EQ(container_->comp<VSelection>()->transitions, before);
    EXPECT_FALSE(container_->isDirtyObject());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoCount);
}

TEST_F(TransitionPropertyRouteTest, RejectsMissingEndpointsAndPreservesBridgeSemantics)
{
    const auto before = container_->comp<VSelection>()->transitions;
    ActionResult result = guiSet(table(external_->guid, childB_->guid).toJson());
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("INVALID_TARGET"));

    result = guiSet(table(childA_->guid, childB_->guid,
                          invalidBridge_->guid).toJson());
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("INVALID_TARGET"));

    const QUuid missing = QUuid::createUuid();
    const QJsonObject dangling = table(missing, childB_->guid, missing).toJson();
    result = guiSet(dangling);
    EXPECT_FALSE(result.success);
    EXPECT_EQ(result.errorCode, QStringLiteral("INVALID_TARGET"));
    EXPECT_EQ(container_->comp<VSelection>()->transitions, before);
    result = guiSet(table(childA_->guid, childB_->guid, missing).toJson());
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    const auto& rule = container_->comp<VSelection>()->transitions.rules[1];
    EXPECT_EQ(rule.source, childA_->guid);
    EXPECT_EQ(rule.bridge, missing);
}

TEST_F(TransitionPropertyRouteTest, DeleteRemovesIllegalEndpointAndRestoresOnUndo)
{
    auto& manager = ProjectManager::instance();
    const QUuid sourceGuid = childA_->guid;
    const QJsonObject value = table(sourceGuid, childB_->guid).toJson();
    ASSERT_TRUE(guiSet(value).success);
    UndoRedoManager::instance().clear();

    ASSERT_TRUE(manager.deleteItem(childA_));
    EXPECT_EQ(manager.findItemByGuid(sourceGuid), nullptr);
    ASSERT_EQ(container_->comp<VSelection>()->transitions.rules.size(), 1u);
    EXPECT_TRUE(container_->comp<VSelection>()->transitions.rules[0].isDefault());

    QUndoStack* undo = UndoRedoManager::instance().undoStack();
    ASSERT_TRUE(undo->canUndo());
    undo->undo();
    EXPECT_NE(manager.findItemByGuid(sourceGuid), nullptr);
    EXPECT_EQ(container_->comp<VSelection>()->transitions.rules[1].source,
              sourceGuid);
}

TEST_F(TransitionPropertyRouteTest, MoveOutRemovesIllegalEndpointAndRestoresOnUndo)
{
    auto& manager = ProjectManager::instance();
    const QUuid sourceGuid = childA_->guid;
    ASSERT_TRUE(guiSet(table(sourceGuid, childB_->guid).toJson()).success);
    UndoRedoManager::instance().clear();

    ASSERT_TRUE(manager.moveItem(childA_, layer_));
    EXPECT_EQ(childA_->parent, layer_);
    EXPECT_FALSE(TransitionSemantics::isEndpointCandidate(*container_, *childA_));
    ASSERT_EQ(container_->comp<VSelection>()->transitions.rules.size(), 1u);
    EXPECT_TRUE(container_->comp<VSelection>()->transitions.rules[0].isDefault());

    QUndoStack* undo = UndoRedoManager::instance().undoStack();
    ASSERT_TRUE(undo->canUndo());
    undo->undo();
    EXPECT_EQ(childA_->parent, container_);
    EXPECT_TRUE(TransitionSemantics::isEndpointCandidate(*container_, *childA_));
    EXPECT_EQ(container_->comp<VSelection>()->transitions.rules[1].source,
              sourceGuid);
}

TEST_F(TransitionPropertyRouteTest, IdenticalLedgerDoesNotAddUndo)
{
    const QJsonObject value = table(childA_->guid, childB_->guid).toJson();
    ASSERT_TRUE(guiSet(value).success);
    const int undoCount = UndoRedoManager::instance().undoStack()->count();
    ASSERT_TRUE(guiSet(value).success);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoCount);
}

TEST_F(TransitionPropertyRouteTest, MusicalRulesRejectBeforeMutationAndShareIpcReason)
{
    const auto before = container_->comp<VSelection>()->transitions;
    for (auto timing : {TransitionExitTiming::NextBeat, TransitionExitTiming::NextBar}) {
        auto replacement = table(childA_->guid, childB_->guid);
        replacement.rules[1].exitTiming = timing;
        const auto gui = guiSet(replacement.toJson());
        EXPECT_FALSE(gui.success);
        EXPECT_EQ(gui.errorCode, QStringLiteral("AUTHORING_RESTRICTED"));
        const auto ipc = ipcSet(replacement.toJson());
        EXPECT_EQ(ipc.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
        EXPECT_EQ(ipc.value(QStringLiteral("message")).toString(), gui.errorMessage);
        EXPECT_EQ(container_->comp<VSelection>()->transitions, before);
        EXPECT_FALSE(container_->isDirtyObject());
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    }
}

TEST_F(TransitionPropertyRouteTest, LegacyRulesCanBeRemovedAndRepairedIndividually)
{
    auto legacy = canonicalTransitionTable();
    auto unsupported = canonicalTransitionRule();
    unsupported.source = childA_->guid;
    unsupported.target = childB_->guid;
    unsupported.exitTiming = TransitionExitTiming::NextBeat;
    ASSERT_TRUE(legacy.addRule(unsupported));
    container_->comp<VSelection>()->transitions = legacy;
    childB_->comp<VPiece>()->pieceTime = PieceTimeSpec::musical();

    auto duplicated = legacy;
    duplicated.rules.push_back(legacy.rules[1]);
    const auto duplicateWrite = guiSet(duplicated.toJson());
    EXPECT_FALSE(duplicateWrite.success);
    EXPECT_EQ(duplicateWrite.errorCode, QStringLiteral("INVALID_ARGS"));
    EXPECT_EQ(container_->comp<VSelection>()->transitions, legacy);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);

    auto repaired = legacy;
    repaired.rules[1].exitTiming = TransitionExitTiming::Immediate;
    ASSERT_TRUE(guiSet(repaired.toJson()).success);
    EXPECT_EQ(container_->comp<VSelection>()->transitions.rules[0], legacy.rules[0]);
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(container_->comp<VSelection>()->transitions, legacy);

    auto removed = legacy;
    ASSERT_TRUE(removed.removeRule(1));
    ASSERT_TRUE(guiSet(removed.toJson()).success);
    EXPECT_EQ(container_->comp<VSelection>()->transitions, canonicalTransitionTable());
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(container_->comp<VSelection>()->transitions, legacy);

    auto edited = legacy;
    edited.rules[1].sourceFadeOut.durationSeconds = 0.8f;
    EXPECT_FALSE(guiSet(edited.toJson()).success);
    auto newRule = AuthoringPolicy::defaultTransitionRule();
    newRule.source = childB_->guid;
    newRule.target = childA_->guid;
    ASSERT_TRUE(edited.addRule(newRule));
    edited.rules[1] = legacy.rules[1];
    EXPECT_FALSE(guiSet(edited.toJson()).success);
    EXPECT_EQ(container_->comp<VSelection>()->transitions, legacy);
}

TEST_F(TransitionPropertyRouteTest, LegacyMusicalRulesCannotBeNewlyActivatedByPlayType)
{
    auto* selection = container_->comp<VSelection>();
    selection->transitions = canonicalTransitionTable();
    const auto rejected = guiSet(static_cast<int>(SelectionPlayType::Queue), QStringLiteral("playType"));
    EXPECT_FALSE(rejected.success);
    EXPECT_EQ(rejected.errorCode, QStringLiteral("AUTHORING_RESTRICTED"));
    EXPECT_EQ(selection->mode, SelectionMode::Random);
    EXPECT_EQ(selection->playType, SelectionPlayType::Single);
    EXPECT_EQ(selection->transitions, canonicalTransitionTable());
    EXPECT_FALSE(container_->isDirtyObject());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);

    ASSERT_TRUE(guiSet(static_cast<int>(SelectionMode::Sequence), QStringLiteral("mode")).success);
    ASSERT_TRUE(guiSet(AuthoringPolicy::defaultTransitionTable().toJson()).success);
    ASSERT_TRUE(guiSet(static_cast<int>(SelectionPlayType::Queue), QStringLiteral("playType")).success);
    EXPECT_EQ(selection->mode, SelectionMode::Sequence);
    EXPECT_EQ(selection->playType, SelectionPlayType::Queue);
}

TEST_F(TransitionPropertyRouteTest, LegacyMusicalRulesCannotBeNewlyActivatedByGridEvaluation)
{
    auto* selection = container_->comp<VSelection>();
    selection->mode = SelectionMode::Match;
    selection->selections.shared.matchAxes = {invalidBridge_->guid};
    selection->transitions = canonicalTransitionTable();
    const auto rejected = guiSet(static_cast<int>(VariableResponse::Live),
                                 QStringLiteral("variableResponse"));
    EXPECT_FALSE(rejected.success);
    EXPECT_EQ(rejected.errorCode, QStringLiteral("AUTHORING_RESTRICTED"));
    EXPECT_EQ(selection->variableResponse(), VariableResponse::AtStart);
    EXPECT_EQ(selection->transitions, canonicalTransitionTable());
    EXPECT_FALSE(container_->isDirtyObject());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);

    auto cleared = selection->selections;
    cleared.shared.matchAxes.clear();
    ASSERT_TRUE(guiSet(cleared.toJson(), QStringLiteral("selections")).success);
    EXPECT_TRUE(selection->matchGrid().axes.empty());
    EXPECT_EQ(selection->transitions, canonicalTransitionTable());
}
