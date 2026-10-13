#include "Core/ContainerBehaviorSemantics.h"
#include "ActionManager.h"
#include "Components/VSelection.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "Support/ProjectPlatformTestUtils.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonObject>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdint>
#include <vector>

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

QUndoStack* undoStack()
{
    return UndoRedoManager::instance().undoStack();
}

std::vector<SelectionEntry> occurrencesFor(
    const ProjectItem* container, const QUuid& childGuid)
{
    std::vector<SelectionEntry> rows;
    if (!container) return rows;
    for (const auto& row : container->comp<VNX::VSelection>()->selections.shared.occurrenceRows()) {
        if (row.childGuid == childGuid) rows.push_back(row);
    }
    return rows;
}

uint32_t occurrenceCount(const ProjectItem* container, const QUuid& childGuid)
{
    return PlayablePropertyAuthoringV1::containerChildOccurrenceCount(
        *container, childGuid);
}


} // namespace

class ContainerSelectionZeroOccurrenceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(), &manager, nullptr, {});
        registerContainerActions(ActionManager::instance(), &manager);

        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("ContainerSelectionZeroOccurrence");
        config.location = tempDir_.path();
        config.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("Android")});
        ASSERT_TRUE(manager.createProject(config));

        layerR_ = manager.createItem(nullptr, QStringLiteral("Layer_R"),
                                     ProjectItem::Type::Layer);
        ASSERT_NE(layerR_, nullptr);
        containerA_ = manager.createItem(layerR_, QStringLiteral("Container_A"),
                                         ProjectItem::Type::Container);
        containerB_ = manager.createItem(layerR_, QStringLiteral("Container_B"),
                                         ProjectItem::Type::Container);
        ASSERT_NE(containerA_, nullptr);
        ASSERT_NE(containerB_, nullptr);
        soundA_ = manager.createItem(containerA_, QStringLiteral("Sound_A"),
                                     ProjectItem::Type::Sound);
        soundB_ = manager.createItem(containerA_, QStringLiteral("Sound_B"),
                                     ProjectItem::Type::Sound);
        ASSERT_NE(soundA_, nullptr);
        ASSERT_NE(soundB_, nullptr);
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    ActionResult setOccurrences(ProjectItem* owner,
                                const SelectionTable& ledger)
    {
        auto candidate = owner->comp<VNX::VSelection>()->selections;
        candidate.shared = ledger;
        return ActionManager::instance().execute(
            QStringLiteral("set_item_property"),
            QJsonObject{
                {QStringLiteral("authoring_handle"), static_cast<qint64>(owner->objectID)},
                {QStringLiteral("property"), QStringLiteral("selections")},
                {QStringLiteral("value"), candidate.toJson()}},
            ActionSource::GUI);
    }

    QTemporaryDir tempDir_;
    ProjectItem* layerR_ = nullptr;
    ProjectItem* containerA_ = nullptr;
    ProjectItem* containerB_ = nullptr;
    ProjectItem* soundA_ = nullptr;
    ProjectItem* soundB_ = nullptr;
};

TEST_F(ContainerSelectionZeroOccurrenceTest,
       RemovingLastSelectionRowLeavesTreeChild)
{
    ASSERT_EQ(occurrenceCount(containerA_, soundA_->guid), 1u);
    auto ledger = containerA_->comp<VNX::VSelection>()->selections.shared;
    auto rows = ledger.occurrenceRows();
    std::erase_if(rows, [this](const auto& row) {
        return row.childGuid == soundA_->guid;
    });
    ASSERT_TRUE(ledger.setOccurrenceRows(rows));
    ASSERT_TRUE(setOccurrences(containerA_, ledger).success);

    EXPECT_EQ(soundA_->parent, containerA_);
    EXPECT_EQ(occurrenceCount(containerA_, soundA_->guid), 0u);

    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.saveProject());
    const QString projectPath = manager.currentProject()->projectPath;
    const QUuid soundGuid = soundA_->guid;
    const QUuid containerGuid = containerA_->guid;
    ASSERT_TRUE(manager.loadProject(projectPath));
    const auto* reloadedContainer = manager.findItemByGuid(containerGuid);
    const auto* reloadedSound = manager.findItemByGuid(soundGuid);
    ASSERT_NE(reloadedContainer, nullptr);
    ASSERT_NE(reloadedSound, nullptr);
    EXPECT_EQ(reloadedSound->parent, reloadedContainer);
    EXPECT_EQ(occurrenceCount(reloadedContainer, soundGuid), 0u);
}

TEST_F(ContainerSelectionZeroOccurrenceTest,
       RemovingOneOfTwoOccurrencesKeepsTheOther)
{
    SelectionEntry duplicate;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *containerA_, soundA_->guid, std::nullopt, &duplicate));
    auto* first = containerA_->comp<VSelection>()->selections.shared.find(0u);
    auto* second = containerA_->comp<VSelection>()->selections.shared.find(duplicate.stableOccurrenceOrdinal);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    auto* selection = containerA_->comp<VNX::VSelection>();
    ASSERT_NE(selection, nullptr);
    selection->selections.shared.find(first->stableOccurrenceOrdinal)->weight = 20;
    selection->selections.shared.find(second->stableOccurrenceOrdinal)->weight = 80;
    selection->behavior.avoidRepeat = 2;
    const auto originalLedger = containerA_->comp<VNX::VSelection>()->selections.shared;
    const auto originalBehavior = selection->behavior;
    const auto keptWeight = second->weight;
    const auto keptOrdinal = second->stableOccurrenceOrdinal;

    auto ledger = containerA_->comp<VNX::VSelection>()->selections.shared;
    auto rows = ledger.occurrenceRows();
    std::erase_if(rows, [](const auto& row) {
        return row.stableOccurrenceOrdinal == 0u;
    });
    ASSERT_TRUE(ledger.setOccurrenceRows(rows));
    ASSERT_TRUE(setOccurrences(containerA_, ledger).success);

    EXPECT_EQ(soundA_->parent, containerA_);
    ASSERT_EQ(occurrenceCount(containerA_, soundA_->guid), 1u);
    const auto remaining = occurrencesFor(containerA_, soundA_->guid);
    ASSERT_EQ(remaining.size(), 1u);
    EXPECT_EQ(remaining[0].stableOccurrenceOrdinal, keptOrdinal);
    EXPECT_EQ(remaining[0].weight, keptWeight);
    EXPECT_FALSE(selection->selections.shared.find(0u));
    EXPECT_EQ(selection->selections.shared.find(keptOrdinal)->weight, 80);
    EXPECT_EQ(ContainerBehaviorSemantics::effectiveAvoidRepeat(*selection), 1);
    EXPECT_EQ(selection->behavior.avoidRepeat, 2);

    undoStack()->undo();
    EXPECT_EQ(containerA_->comp<VNX::VSelection>()->selections.shared, originalLedger);
    EXPECT_EQ(selection->behavior, originalBehavior);
    EXPECT_EQ(selection->behavior.avoidRepeat, originalBehavior.avoidRepeat);
    undoStack()->redo();
    EXPECT_EQ(containerA_->comp<VNX::VSelection>()->selections.shared, ledger);
    EXPECT_FALSE(selection->selections.shared.find(0u));
    EXPECT_EQ(selection->selections.shared.find(keptOrdinal)->weight, 80);
    EXPECT_EQ(ContainerBehaviorSemantics::effectiveAvoidRepeat(*selection), 1);
    EXPECT_EQ(selection->behavior.avoidRepeat, 2);
    undoStack()->undo();
    EXPECT_EQ(selection->behavior, originalBehavior);
    EXPECT_EQ(selection->behavior.avoidRepeat, originalBehavior.avoidRepeat);
}

TEST_F(ContainerSelectionZeroOccurrenceTest,
       ZeroOccurrenceChildCanBeDeletedFromTheTree)
{
    auto ledger = containerA_->comp<VNX::VSelection>()->selections.shared;
    auto rows = ledger.occurrenceRows();
    std::erase_if(rows, [this](const auto& row) {
        return row.childGuid == soundA_->guid;
    });
    ASSERT_TRUE(ledger.setOccurrenceRows(rows));
    ASSERT_TRUE(setOccurrences(containerA_, ledger).success);
    EXPECT_EQ(occurrenceCount(containerA_, soundA_->guid), 0u);

    auto& manager = ProjectManager::instance();
    const QUuid soundGuid = soundA_->guid;
    ASSERT_TRUE(manager.deleteItem(soundA_));
    EXPECT_EQ(manager.findItemByGuid(soundGuid), nullptr);
    EXPECT_EQ(occurrenceCount(containerA_, soundGuid), 0u);
}

TEST_F(ContainerSelectionZeroOccurrenceTest,
       MovingZeroOccurrenceChildToLayerAndUndoRestoresEmpty)
{
    auto ledger = containerA_->comp<VNX::VSelection>()->selections.shared;
    auto rows = ledger.occurrenceRows();
    std::erase_if(rows, [this](const auto& row) {
        return row.childGuid == soundA_->guid;
    });
    ASSERT_TRUE(ledger.setOccurrenceRows(rows));
    ASSERT_TRUE(setOccurrences(containerA_, ledger).success);
    const int undoCount = undoStack()->count();

    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.moveItem(soundA_, layerR_));
    EXPECT_EQ(soundA_->parent, layerR_);
    EXPECT_EQ(occurrenceCount(containerA_, soundA_->guid), 0u);

    undoStack()->undo();
    EXPECT_EQ(soundA_->parent, containerA_);
    EXPECT_EQ(occurrenceCount(containerA_, soundA_->guid), 0u);
    EXPECT_EQ(undoStack()->count(), undoCount + 1);
}

TEST_F(ContainerSelectionZeroOccurrenceTest,
       ZeroOccurrenceTreeEditsPreserveRemainingSelectionBehavior)
{
    for (int i = 0; i < 2; ++i)
        ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(*containerA_, soundB_->guid));
    auto ledger = containerA_->comp<VNX::VSelection>()->selections.shared;
    auto rows = ledger.occurrenceRows();
    std::erase_if(rows, [this](const auto& row) { return row.childGuid == soundA_->guid; });
    ASSERT_TRUE(ledger.setOccurrenceRows(rows));
    ASSERT_TRUE(setOccurrences(containerA_, ledger).success);
    auto* selection = containerA_->comp<VNX::VSelection>();
    selection->behavior.avoidRepeat = 2;
    const auto original = selection->toJson();
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.moveItem(soundA_, layerR_));
    EXPECT_EQ(selection->toJson(), original);
    undoStack()->undo();
    EXPECT_EQ(selection->toJson(), original);
    ASSERT_TRUE(manager.deleteItem(soundA_));
    EXPECT_EQ(selection->toJson(), original);
    undoStack()->undo();
    EXPECT_EQ(selection->toJson(), original);
}

TEST_F(ContainerSelectionZeroOccurrenceTest,
       MovingZeroOccurrenceChildIntoAnotherContainerMintsOne)
{
    auto ledger = containerA_->comp<VNX::VSelection>()->selections.shared;
    auto rows = ledger.occurrenceRows();
    std::erase_if(rows, [this](const auto& row) {
        return row.childGuid == soundA_->guid;
    });
    ASSERT_TRUE(ledger.setOccurrenceRows(rows));
    ASSERT_TRUE(setOccurrences(containerA_, ledger).success);

    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.moveItem(soundA_, containerB_));
    EXPECT_EQ(soundA_->parent, containerB_);
    EXPECT_EQ(occurrenceCount(containerA_, soundA_->guid), 0u);
    ASSERT_EQ(occurrenceCount(containerB_, soundA_->guid), 1u);

    undoStack()->undo();
    EXPECT_EQ(soundA_->parent, containerA_);
    EXPECT_EQ(occurrenceCount(containerA_, soundA_->guid), 0u);
    EXPECT_EQ(occurrenceCount(containerB_, soundA_->guid), 0u);
}

TEST_F(ContainerSelectionZeroOccurrenceTest,
       DuplicatingAndCopyingZeroOccurrenceChildMintsOneOnTheCopy)
{
    auto ledger = containerA_->comp<VNX::VSelection>()->selections.shared;
    auto rows = ledger.occurrenceRows();
    std::erase_if(rows, [this](const auto& row) {
        return row.childGuid == soundA_->guid;
    });
    ASSERT_TRUE(ledger.setOccurrenceRows(rows));
    ASSERT_TRUE(setOccurrences(containerA_, ledger).success);

    auto& manager = ProjectManager::instance();
    auto* duplicate = manager.duplicateItem(soundA_);
    ASSERT_NE(duplicate, nullptr);
    EXPECT_EQ(duplicate->parent, containerA_);
    EXPECT_EQ(occurrenceCount(containerA_, soundA_->guid), 0u);
    EXPECT_EQ(occurrenceCount(containerA_, duplicate->guid), 1u);

    ASSERT_EQ(manager.setClipboard({soundA_}, ProjectManager::ClipboardOp::Copy), 1);
    const auto pasted = manager.pasteClipboard(containerB_);
    ASSERT_EQ(pasted.size(), 1u);
    EXPECT_EQ(pasted.front()->parent, containerB_);
    EXPECT_EQ(occurrenceCount(containerA_, soundA_->guid), 0u);
    EXPECT_EQ(occurrenceCount(containerB_, pasted.front()->guid), 1u);
}

TEST_F(ContainerSelectionZeroOccurrenceTest,
       DuplicatingContainerPreservesZeroAndDuplicateRows)
{
    SelectionEntry duplicate;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *containerA_, soundB_->guid, std::nullopt, &duplicate));
    auto ledger = containerA_->comp<VNX::VSelection>()->selections.shared;
    auto rows = ledger.occurrenceRows();
    std::erase_if(rows, [this](const auto& row) {
        return row.childGuid == soundA_->guid;
    });
    ASSERT_TRUE(ledger.setOccurrenceRows(rows));
    ASSERT_TRUE(setOccurrences(containerA_, ledger).success);
    ASSERT_EQ(occurrenceCount(containerA_, soundA_->guid), 0u);
    ASSERT_EQ(occurrenceCount(containerA_, soundB_->guid), 2u);

    auto* clone = ProjectManager::instance().duplicateItem(containerA_);
    ASSERT_NE(clone, nullptr);
    ASSERT_EQ(clone->children.size(), containerA_->children.size());
    auto* soundA2 = clone->children[0].get();
    auto* soundB2 = clone->children[1].get();
    ASSERT_NE(soundA2, nullptr);
    ASSERT_NE(soundB2, nullptr);
    EXPECT_EQ(occurrenceCount(clone, soundA2->guid), 0u);
    EXPECT_EQ(occurrenceCount(clone, soundB2->guid), 2u);
    EXPECT_EQ(occurrenceCount(containerA_, soundA_->guid), 0u);
    EXPECT_EQ(occurrenceCount(containerA_, soundB_->guid), 2u);
}

TEST_F(ContainerSelectionZeroOccurrenceTest,
       DeleteUndoRedoUndoKeepsZeroOccurrences)
{
    auto ledger = containerA_->comp<VNX::VSelection>()->selections.shared;
    auto rows = ledger.occurrenceRows();
    std::erase_if(rows, [this](const auto& row) {
        return row.childGuid == soundA_->guid;
    });
    ASSERT_TRUE(ledger.setOccurrenceRows(rows));
    ASSERT_TRUE(setOccurrences(containerA_, ledger).success);
    const QUuid soundGuid = soundA_->guid;
    const int childrenBefore = static_cast<int>(containerA_->children.size());

    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.deleteItem(soundA_));
    EXPECT_EQ(manager.findItemByGuid(soundGuid), nullptr);

    undoStack()->undo();
    auto* restored = manager.findItemByGuid(soundGuid);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->parent, containerA_);
    EXPECT_EQ(occurrenceCount(containerA_, soundGuid), 0u);
    EXPECT_EQ(static_cast<int>(containerA_->children.size()), childrenBefore);

    undoStack()->redo();
    EXPECT_EQ(manager.findItemByGuid(soundGuid), nullptr);
    EXPECT_EQ(static_cast<int>(containerA_->children.size()), childrenBefore - 1);

    undoStack()->undo();
    restored = manager.findItemByGuid(soundGuid);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->parent, containerA_);
    EXPECT_EQ(occurrenceCount(containerA_, soundGuid), 0u);
    EXPECT_EQ(static_cast<int>(containerA_->children.size()), childrenBefore);
}

TEST_F(ContainerSelectionZeroOccurrenceTest,
       EmptySelectionSurvivesReloadAndOnlyNewChildGetsADefaultRow)
{
    SelectionTable empty;
    empty.occurrenceOrdinalHighWater =
        containerA_->comp<VNX::VSelection>()->selections.shared.occurrenceOrdinalHighWater;
    ASSERT_TRUE(setOccurrences(containerA_, empty).success);
    EXPECT_TRUE(containerA_->comp<VNX::VSelection>()->selections.shared.occurrenceRows().empty());
    EXPECT_EQ(containerA_->children.size(), 2u);

    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.saveProject());
    const QString projectPath = manager.currentProject()->projectPath;
    const QUuid containerGuid = containerA_->guid;
    const QUuid soundAGuid = soundA_->guid;
    const QUuid soundBGuid = soundB_->guid;
    ASSERT_TRUE(manager.loadProject(projectPath));
    auto* reloaded = manager.findItemByGuid(containerGuid);
    ASSERT_NE(reloaded, nullptr);
    EXPECT_TRUE(reloaded->comp<VNX::VSelection>()->selections.shared.occurrenceRows().empty());
    EXPECT_EQ(occurrenceCount(reloaded, soundAGuid), 0u);
    EXPECT_EQ(occurrenceCount(reloaded, soundBGuid), 0u);

    auto* created = manager.createItem(reloaded, QStringLiteral("Sound_C"),
                                       ProjectItem::Type::Sound);
    ASSERT_NE(created, nullptr);
    EXPECT_EQ(occurrenceCount(reloaded, soundAGuid), 0u);
    EXPECT_EQ(occurrenceCount(reloaded, soundBGuid), 0u);
    EXPECT_EQ(occurrenceCount(reloaded, created->guid), 1u);
}

TEST_F(ContainerSelectionZeroOccurrenceTest, PlatformTablesEditUndoAndReloadIndependently)
{
    auto* selection = containerA_->comp<VSelection>();
    const auto before = selection->selections;
    auto edited = before;
    auto& android = edited.editTable(HarkenTest::PlatformIds::Android);
    auto androidRows = android.occurrenceRows();
    std::swap(androidRows[0], androidRows[1]);
    androidRows[0].weight = 23;
    ASSERT_TRUE(android.setOccurrenceRows(androidRows));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *containerA_, android, soundB_->guid));
    android.find(android.occurrenceRows().back().stableOccurrenceOrdinal)->weight = 77;
    edited.editTable(HarkenTest::PlatformIds::Windows).setOccurrenceRows({});
    const auto result = ActionManager::instance().execute(QStringLiteral("set_item_property"),
        {{"authoring_handle", static_cast<qint64>(containerA_->objectID)},
         {"property", "selections"}, {"value", edited.toJson()}}, ActionSource::IPC);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_EQ(selection->selections.shared, before.shared);
    EXPECT_TRUE(selection->selections.table(HarkenTest::PlatformIds::Windows).occurrenceRows().empty());
    EXPECT_EQ(selection->selections.table(HarkenTest::PlatformIds::Android), android);
    EXPECT_EQ(containerA_->children.size(), 2u);
    undoStack()->undo();
    EXPECT_EQ(selection->selections, before);
    undoStack()->redo();
    EXPECT_EQ(selection->selections, edited);

    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.saveProject());
    const auto path = manager.currentProject()->projectPath;
    const auto guid = containerA_->guid;
    ASSERT_TRUE(manager.loadProject(path));
    const auto* restored = manager.findItemByGuid(guid);
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->comp<VSelection>()->selections, edited);
}

TEST_F(ContainerSelectionZeroOccurrenceTest, TreeMoveDeleteAndClonePreserveEveryPlatformTable)
{
    auto& manager = ProjectManager::instance();
    auto& selections = containerA_->comp<VSelection>()->selections;
    auto& android = selections.editTable(HarkenTest::PlatformIds::Android);
    auto androidRows = android.occurrenceRows();
    androidRows.erase(androidRows.begin());
    androidRows.front().weight = 19;
    ASSERT_TRUE(android.setOccurrenceRows(androidRows));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *containerA_, android, soundB_->guid));
    android.find(android.occurrenceRows().back().stableOccurrenceOrdinal)->weight = 81;
    selections.editTable(HarkenTest::PlatformIds::Windows).setOccurrenceRows({});
    containerB_->comp<VSelection>()->selections.editTable(HarkenTest::PlatformIds::Android);
    const auto before = selections;
    const auto destination = containerB_->comp<VSelection>()->selections;
    UndoRedoManager::instance().clear();

    ASSERT_TRUE(manager.moveItem(soundB_, containerB_));
    EXPECT_TRUE(selections.table(HarkenTest::PlatformIds::Android).occurrenceRows().empty());
    ASSERT_EQ(containerB_->comp<VSelection>()->selections.table(HarkenTest::PlatformIds::Android).occurrenceRows().size(), 1u);
    const auto moved = containerB_->comp<VSelection>()->selections;
    undoStack()->undo();
    EXPECT_EQ(selections, before);
    EXPECT_EQ(containerB_->comp<VSelection>()->selections, destination);
    undoStack()->redo();
    EXPECT_EQ(containerB_->comp<VSelection>()->selections, moved);
    undoStack()->undo();

    ASSERT_TRUE(manager.deleteItem(soundB_));
    EXPECT_TRUE(selections.table(HarkenTest::PlatformIds::Android).occurrenceRows().empty());
    undoStack()->undo();
    EXPECT_EQ(selections, before);

    auto* cloned = manager.duplicateItem(containerA_);
    ASSERT_NE(cloned, nullptr);
    const auto& clonedSelections = cloned->comp<VSelection>()->selections;
    EXPECT_TRUE(clonedSelections.table(HarkenTest::PlatformIds::Windows).occurrenceRows().empty());
    const auto& clonedAndroid = clonedSelections.table(HarkenTest::PlatformIds::Android);
    ASSERT_EQ(clonedAndroid.occurrenceRows().size(), 2u);
    EXPECT_EQ(clonedAndroid.occurrenceRows()[0].weight, 19);
    EXPECT_EQ(clonedAndroid.occurrenceRows()[1].weight, 81);
    EXPECT_EQ(clonedAndroid.occurrenceRows()[0].childGuid, clonedAndroid.occurrenceRows()[1].childGuid);
    EXPECT_NE(clonedAndroid.occurrenceRows()[0].childGuid, soundB_->guid);
    EXPECT_TRUE(PlayablePropertyAuthoringV1::validateContainerOccurrences(*cloned));
}
