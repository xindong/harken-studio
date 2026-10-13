#include "ActionManager.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"
#include "UndoCommands.h"
#include "Components/VMixNode.h"
#include "RuntimeBridge/PlaybackBridge.h"
#include "Widgets/Blueprint/SignalChainSource.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QTemporaryDir>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

class OutputSendExclusionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        VNX::UndoRedoManager::instance().clear();
        ASSERT_TRUE(tempDir.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("OutputSendExclusionProject");
        config.location = tempDir.path();
        ASSERT_TRUE(pm.createProject(config));
        VNX::ActionManager::instance().clearAllForTesting();
        VNX::registerFieldGroupActions(VNX::ActionManager::instance(), &pm);
    }

    void TearDown() override
    {
        auto& pm = VNX::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        VNX::UndoRedoManager::instance().clear();
        VNX::ActionManager::instance().clearAllForTesting();
    }

    QTemporaryDir tempDir;
};

}

TEST_F(OutputSendExclusionTest, MasterKeepsItsOwnUuidAcrossSaveAndOpen)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* master = pm.findBusItem(VNX::kMasterBusID);
    ASSERT_NE(master, nullptr);

    const auto masterId = master->runtimeId();
    EXPECT_NE(masterId, HarkenEditor::Bridge::kEngineMasterBusId);
    EXPECT_EQ(pm.busRuntimeHandle(master), masterId);
    EXPECT_EQ(pm.busRuntimeHandleForId(VNX::kMasterBusID), masterId);
    ASSERT_TRUE(pm.saveProject());
    const auto projectPath = pm.currentProject()->projectPath;
    pm.closeProject();
    ASSERT_TRUE(pm.loadProject(projectPath));
    EXPECT_EQ(pm.getMasterBusItem()->runtimeId(), masterId);
    pm.closeProject();
    VNX::ProjectConfig config;
    config.name = QStringLiteral("IndependentMasterProject");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    EXPECT_NE(pm.getMasterBusItem()->runtimeId(), masterId);
}

TEST_F(OutputSendExclusionTest, BlueprintRouteConnectionToSendTargetIsRejected)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* master = pm.findBusItem(VNX::kMasterBusID);
    ASSERT_NE(master, nullptr);
    VNX::ProjectItem* target = pm.createItem(
        master, QStringLiteral("SendTargetBus"), VNX::ProjectItem::Type::Bus);
    VNX::ProjectItem* output = pm.createItem(
        master, QStringLiteral("MainOutputBus"), VNX::ProjectItem::Type::Bus);
    ASSERT_NE(target, nullptr);
    ASSERT_NE(output, nullptr);
    VNX::ProjectItem* parent = pm.createItem(
        nullptr, QStringLiteral("RoutingOwner"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(parent, nullptr);
    VNX::ProjectItem* item = pm.createItem(
        parent, QStringLiteral("RoutedLayer"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(item, nullptr);
    auto& actions = VNX::ActionManager::instance();
    QJsonObject edit{{"definition_guid", item->guid.toString(QUuid::WithoutBraces)},
                     {"scope", "Shared"}, {"group", "mainOutput"}, {"mode", "Local"}};
    ASSERT_TRUE(actions.execute("set_field_group_mode", edit).success);
    edit.insert("group", "send");
    ASSERT_TRUE(actions.execute("set_field_group_mode", edit).success);
    edit.insert("sends", QJsonArray{QJsonObject{
        {"targetBusID", target->guid.toString(QUuid::WithoutBraces)}, {"sendLevel", -6.0}}});
    ASSERT_TRUE(actions.execute("replace_field_group_sends", edit).success);
    const auto before = item->fieldGroups;
    auto* undo = VNX::UndoRedoManager::instance().undoStack();
    undo->clear();

    VNX::SignalChainSource source;

    EXPECT_FALSE(source.onConnect(item->objectID, target->objectID,
                                  QStringLiteral("bus_route"),
                                  QStringLiteral("item"), QStringLiteral("bus")));
    EXPECT_EQ(source.lastError(), QStringLiteral("output_rejected"));
    EXPECT_EQ(item->fieldGroups, before);
    EXPECT_EQ(item->comp<VNX::VMixNode>()->outputBusId, master->objectID);

    EXPECT_FALSE(source.onConnect(item->objectID, target->objectID,
                                  QStringLiteral("override"),
                                  QStringLiteral("item"), QStringLiteral("bus")));
    EXPECT_EQ(source.lastError(), QStringLiteral("output_rejected"));
    EXPECT_EQ(item->fieldGroups, before);
    EXPECT_EQ(undo->count(), 0);

    EXPECT_TRUE(source.onConnect(item->objectID, output->objectID,
                                 QStringLiteral("bus_route"),
                                 QStringLiteral("item"), QStringLiteral("bus")));
    EXPECT_EQ(item->comp<VNX::VMixNode>()->outputBusId, output->objectID);
    EXPECT_EQ(item->fieldGroups.mainOutput.shared.localHistory->mainOutputBusRef,
              VNX::toFieldGroupGuid(output->guid));
    EXPECT_EQ(item->fieldGroups.send, before.send);
    ASSERT_EQ(undo->count(), 1);
    undo->undo();
    EXPECT_EQ(item->fieldGroups, before);
    EXPECT_EQ(item->comp<VNX::VMixNode>()->outputBusId, master->objectID);
    edit.insert("group", "mainOutput");
    edit.insert("mode", "Inherit");
    ASSERT_TRUE(actions.execute("set_field_group_mode", edit).success);
    const auto inherited = item->fieldGroups;
    const int undoCount = undo->count();
    EXPECT_FALSE(source.onConnect(item->objectID, output->objectID,
                                  QStringLiteral("bus_route"),
                                  QStringLiteral("item"), QStringLiteral("bus")));
    EXPECT_EQ(source.lastError(), QStringLiteral("field_group_read_only"));
    EXPECT_EQ(item->fieldGroups, inherited);
    EXPECT_EQ(undo->count(), undoCount);
}

TEST_F(OutputSendExclusionTest, BusMoveRemovesConflictingSendAndUndoRestoresIt)
{
    auto& pm = VNX::ProjectManager::instance();
    auto& undo = VNX::UndoRedoManager::instance();
    VNX::ProjectItem* master = pm.findBusItem(VNX::kMasterBusID);
    ASSERT_NE(master, nullptr);
    VNX::ProjectItem* moved = pm.createItem(
        master, QStringLiteral("MovedBus"), VNX::ProjectItem::Type::Bus);
    VNX::ProjectItem* newParent = pm.createItem(
        master, QStringLiteral("NewParentBus"), VNX::ProjectItem::Type::Bus);
    ASSERT_NE(moved, nullptr);
    ASSERT_NE(newParent, nullptr);
    moved->compOrCreate<VNX::VMixNode>()->sends.push_back(
        VNX::Send{newParent->objectID, 0.25f});

    ASSERT_TRUE(pm.moveItem(moved, newParent));
    EXPECT_EQ(moved->parent, newParent);
    EXPECT_TRUE(moved->comp<VNX::VMixNode>()->sends.empty());

    undo.undoStack()->undo();
    EXPECT_EQ(moved->parent, master);
    ASSERT_EQ(moved->comp<VNX::VMixNode>()->sends.size(), 1u);
    EXPECT_EQ(moved->comp<VNX::VMixNode>()->sends[0].targetBusID, newParent->objectID);
    EXPECT_NEAR(moved->comp<VNX::VMixNode>()->sends[0].sendLevel, 0.25f, 1e-6f);

    undo.undoStack()->redo();
    EXPECT_EQ(moved->parent, newParent);
    EXPECT_TRUE(moved->comp<VNX::VMixNode>()->sends.empty());
}

TEST_F(OutputSendExclusionTest, BusMoveWithoutConflictLeavesSendsUntouched)
{
    auto& pm = VNX::ProjectManager::instance();
    VNX::ProjectItem* master = pm.findBusItem(VNX::kMasterBusID);
    ASSERT_NE(master, nullptr);
    VNX::ProjectItem* moved = pm.createItem(
        master, QStringLiteral("PlainMovedBus"), VNX::ProjectItem::Type::Bus);
    VNX::ProjectItem* newParent = pm.createItem(
        master, QStringLiteral("PlainParentBus"), VNX::ProjectItem::Type::Bus);
    VNX::ProjectItem* other = pm.createItem(
        master, QStringLiteral("OtherBus"), VNX::ProjectItem::Type::Bus);
    ASSERT_NE(moved, nullptr);
    ASSERT_NE(newParent, nullptr);
    ASSERT_NE(other, nullptr);
    moved->compOrCreate<VNX::VMixNode>()->sends.push_back(
        VNX::Send{other->objectID, 0.75f});

    ASSERT_TRUE(pm.moveItem(moved, newParent));
    EXPECT_EQ(moved->parent, newParent);
    ASSERT_EQ(moved->comp<VNX::VMixNode>()->sends.size(), 1u);
    EXPECT_EQ(moved->comp<VNX::VMixNode>()->sends[0].targetBusID, other->objectID);
}
