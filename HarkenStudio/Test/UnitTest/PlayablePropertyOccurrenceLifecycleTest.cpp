#include "Core/Components/VSelection.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectSerializer.h"
#include "Core/UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QTemporaryDir>

#include <algorithm>
#include <cstdint>
#include <utility>

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

std::optional<SelectionEntry> occurrenceFor(const ProjectItem* container,
                                                 const QUuid& childGuid)
{
    if (!container) return std::nullopt;
    const auto& rows = container->comp<VNX::VSelection>()->selections.shared.occurrenceRows();
    const auto it = std::find_if(rows.begin(), rows.end(), [&](const auto& row) {
        return row.childGuid == childGuid;
    });
    return it == rows.end() ? std::nullopt : std::optional(*it);
}

class PlayablePropertyOccurrenceLifecycleTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("OccurrenceLifecycle");
        config.location = tempDir_.path();
        ASSERT_TRUE(manager.createProject(config));
        layer_ = manager.createItem(
            nullptr, QStringLiteral("Layer"), ProjectItem::Type::Layer);
        firstContainer_ = manager.createItem(
            layer_, QStringLiteral("First"), ProjectItem::Type::Container);
        secondContainer_ = manager.createItem(
            layer_, QStringLiteral("Second"), ProjectItem::Type::Container);
        ASSERT_NE(layer_, nullptr);
        ASSERT_NE(firstContainer_, nullptr);
        ASSERT_NE(secondContainer_, nullptr);
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
    }

    QTemporaryDir tempDir_;
    ProjectItem* layer_ = nullptr;
    ProjectItem* firstContainer_ = nullptr;
    ProjectItem* secondContainer_ = nullptr;
};

TEST_F(PlayablePropertyOccurrenceLifecycleTest,
       DeleteUndoAndFreshInsertPreserveOccurrenceIdentity)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* first = manager.createItem(
        firstContainer_, QStringLiteral("FirstSound"), ProjectItem::Type::Sound);
    ProjectItem* second = manager.createItem(
        firstContainer_, QStringLiteral("SecondSound"), ProjectItem::Type::Sound);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_EQ(firstContainer_->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 2u);
    auto firstRow = occurrenceFor(firstContainer_, first->guid);
    auto secondRow = occurrenceFor(firstContainer_, second->guid);
    ASSERT_NE(firstRow, std::nullopt);
    ASSERT_NE(secondRow, std::nullopt);
    EXPECT_NE(firstRow->stableOccurrenceOrdinal, secondRow->stableOccurrenceOrdinal);

    UndoRedoManager::instance().clear();
    ASSERT_TRUE(manager.deleteItem(first));
    EXPECT_EQ(occurrenceFor(firstContainer_, first->guid), std::nullopt);
    UndoRedoManager::instance().undoStack()->undo();
    ASSERT_NE(occurrenceFor(firstContainer_, first->guid), std::nullopt);
    EXPECT_EQ(occurrenceFor(firstContainer_, first->guid)->stableOccurrenceOrdinal,
              firstRow->stableOccurrenceOrdinal);
    UndoRedoManager::instance().undoStack()->redo();

    ProjectItem* fresh = manager.createItem(
        firstContainer_, QStringLiteral("FreshSound"), ProjectItem::Type::Sound);
    ASSERT_NE(fresh, nullptr);
    ASSERT_NE(occurrenceFor(firstContainer_, fresh->guid), std::nullopt);
    EXPECT_NE(occurrenceFor(firstContainer_, fresh->guid)->stableOccurrenceOrdinal,
              firstRow->stableOccurrenceOrdinal);
    EXPECT_NE(occurrenceFor(firstContainer_, fresh->guid)->stableOccurrenceOrdinal,
              secondRow->stableOccurrenceOrdinal);
}

TEST_F(PlayablePropertyOccurrenceLifecycleTest,
       ReparentRemovesEveryDuplicateOccurrenceAndUndoRestoresThem)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* moving = manager.createItem(
        firstContainer_, QStringLiteral("MovingDup"), ProjectItem::Type::Sound);
    ASSERT_NE(moving, nullptr);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *firstContainer_, moving->guid));
    ASSERT_EQ(firstContainer_->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 2u);

    UndoRedoManager::instance().clear();
    ASSERT_TRUE(manager.moveItem(moving, secondContainer_));
    EXPECT_EQ(occurrenceFor(firstContainer_, moving->guid), std::nullopt);
    EXPECT_EQ(firstContainer_->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 0u);
    EXPECT_EQ(moving->parent, secondContainer_);
    ASSERT_NE(occurrenceFor(secondContainer_, moving->guid), std::nullopt);
    EXPECT_EQ(occurrenceFor(secondContainer_, moving->guid)->childGuid, moving->guid);

    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(moving->parent, firstContainer_);
    EXPECT_EQ(secondContainer_->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 0u);
    ASSERT_EQ(firstContainer_->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 2u);
    EXPECT_EQ(firstContainer_->comp<VNX::VSelection>()->selections.shared.occurrenceRows()[0].childGuid, moving->guid);
    EXPECT_EQ(firstContainer_->comp<VNX::VSelection>()->selections.shared.occurrenceRows()[1].childGuid, moving->guid);

    UndoRedoManager::instance().undoStack()->redo();
    EXPECT_EQ(moving->parent, secondContainer_);
    EXPECT_EQ(occurrenceFor(firstContainer_, moving->guid), std::nullopt);
    ASSERT_NE(occurrenceFor(secondContainer_, moving->guid), std::nullopt);
}

TEST_F(PlayablePropertyOccurrenceLifecycleTest,
       ReorderSerializationAndCopyKeepRowsBoundToExactChildren)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* first = manager.createItem(
        firstContainer_, QStringLiteral("FirstSound"), ProjectItem::Type::Sound);
    ProjectItem* second = manager.createItem(
        firstContainer_, QStringLiteral("SecondSound"), ProjectItem::Type::Sound);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    const auto firstRow = occurrenceFor(firstContainer_, first->guid);
    const auto secondRow = occurrenceFor(firstContainer_, second->guid);
    ASSERT_NE(firstRow, std::nullopt);
    ASSERT_NE(secondRow, std::nullopt);
    std::swap(firstContainer_->children[0], firstContainer_->children[1]);

    ProjectSerializer serializer;
    ASSERT_TRUE(serializer.prepareReferenceWrite(*manager.currentProject()));
    const QJsonObject json = serializer.projectItemToJson(*firstContainer_);
    ASSERT_FALSE(json.isEmpty()) << serializer.lastError().toStdString();
    ProjectItem restored;
    ASSERT_TRUE(serializer.projectItemFromJson(json, restored));
    ASSERT_EQ(restored.children.size(), 2u);
    auto restoredFirst = occurrenceFor(&restored, first->guid);
    auto restoredSecond = occurrenceFor(&restored, second->guid);
    ASSERT_NE(restoredFirst, std::nullopt);
    ASSERT_NE(restoredSecond, std::nullopt);
    EXPECT_EQ(restoredFirst->stableOccurrenceOrdinal, firstRow->stableOccurrenceOrdinal);
    EXPECT_EQ(restoredSecond->stableOccurrenceOrdinal, secondRow->stableOccurrenceOrdinal);

    ProjectItem* clone = manager.duplicateItem(firstContainer_);
    ASSERT_NE(clone, nullptr);
    ASSERT_EQ(clone->children.size(), 2u);
    ASSERT_EQ(clone->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 2u);
    for (const auto& child : clone->children) {
        ASSERT_NE(occurrenceFor(clone, child->guid), std::nullopt);
    }
}

TEST_F(PlayablePropertyOccurrenceLifecycleTest,
       ContainerLedgerLoadRejectsMalformedShapeAtomically)
{
    auto& manager = ProjectManager::instance();
    ASSERT_NE(manager.createItem(firstContainer_, QStringLiteral("Sound"), ProjectItem::Type::Sound), nullptr);
    const auto before = firstContainer_->comp<VNX::VSelection>()->selections;
    const auto reject = [&](const SelectionData& malformed) {
        auto component = firstContainer_->comp<VNX::VSelection>()->toJson();
        component["selections"] = malformed.toJson();
        EXPECT_FALSE(firstContainer_->comp<VNX::VSelection>()->fromJson(component));
        EXPECT_EQ(firstContainer_->comp<VNX::VSelection>()->selections, before);
    };
    auto malformed = before;
    malformed.shared.entries.push_back(malformed.shared.entries.front());
    reject(malformed);
    malformed = before;
    malformed.shared.entries.front().occurrences.front().stableOccurrenceOrdinal =
        malformed.shared.occurrenceOrdinalHighWater;
    reject(malformed);
    malformed = before;
    malformed.shared.entries.front().childGuid = QUuid{};
    reject(malformed);
}

}
