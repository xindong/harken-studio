#include "Core/Components/VSelection.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectSerializer.h"
#include "Core/UndoCommands.h"
#include "Support/ProjectPlatformTestUtils.h"

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
       DeleteUndoAndFreshInsertPreserveHighWaterWithoutReuse)
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
    EXPECT_EQ(firstRow->stableOccurrenceOrdinal, 0u);
    EXPECT_EQ(secondRow->stableOccurrenceOrdinal, 1u);
    EXPECT_EQ(firstContainer_->comp<VNX::VSelection>()->selections.shared.occurrenceOrdinalHighWater, 2u);

    UndoRedoManager::instance().clear();
    ASSERT_TRUE(manager.deleteItem(first));
    EXPECT_EQ(occurrenceFor(firstContainer_, first->guid), std::nullopt);
    EXPECT_EQ(firstContainer_->comp<VNX::VSelection>()->selections.shared.occurrenceOrdinalHighWater, 2u);
    UndoRedoManager::instance().undoStack()->undo();
    ASSERT_NE(occurrenceFor(firstContainer_, first->guid), std::nullopt);
    EXPECT_EQ(occurrenceFor(firstContainer_, first->guid)->stableOccurrenceOrdinal, 0u);
    UndoRedoManager::instance().undoStack()->redo();

    ProjectItem* fresh = manager.createItem(
        firstContainer_, QStringLiteral("FreshSound"), ProjectItem::Type::Sound);
    ASSERT_NE(fresh, nullptr);
    ASSERT_NE(occurrenceFor(firstContainer_, fresh->guid), std::nullopt);
    EXPECT_EQ(occurrenceFor(firstContainer_, fresh->guid)->stableOccurrenceOrdinal, 2u);
    EXPECT_EQ(firstContainer_->comp<VNX::VSelection>()->selections.shared.occurrenceOrdinalHighWater, 3u);
}

TEST_F(PlayablePropertyOccurrenceLifecycleTest,
       ReparentMintsDestinationOrdinalAndUndoIsExact)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* moving = manager.createItem(
        firstContainer_, QStringLiteral("Moving"), ProjectItem::Type::Sound);
    ProjectItem* seed = manager.createItem(
        secondContainer_, QStringLiteral("Seed"), ProjectItem::Type::Sound);
    ASSERT_NE(moving, nullptr);
    ASSERT_NE(seed, nullptr);
    auto sourceRow = occurrenceFor(firstContainer_, moving->guid);
    ASSERT_NE(sourceRow, std::nullopt);
    const auto sourceOccurrence = *sourceRow;

    UndoRedoManager::instance().clear();
    ASSERT_TRUE(manager.moveItem(moving, secondContainer_));
    EXPECT_EQ(occurrenceFor(firstContainer_, moving->guid), std::nullopt);
    auto destinationRow = occurrenceFor(secondContainer_, moving->guid);
    ASSERT_NE(destinationRow, std::nullopt);
    EXPECT_EQ(destinationRow->stableOccurrenceOrdinal, 1u);
    EXPECT_EQ(destinationRow->childGuid, moving->guid);
    EXPECT_EQ(destinationRow->weight, 50);

    UndoRedoManager::instance().undoStack()->undo();
    auto restoredSource = occurrenceFor(firstContainer_, moving->guid);
    ASSERT_NE(restoredSource, std::nullopt);
    EXPECT_EQ(*restoredSource, sourceOccurrence);
    EXPECT_EQ(occurrenceFor(secondContainer_, moving->guid), std::nullopt);

    UndoRedoManager::instance().undoStack()->redo();
    destinationRow = occurrenceFor(secondContainer_, moving->guid);
    ASSERT_NE(destinationRow, std::nullopt);
    EXPECT_EQ(destinationRow->stableOccurrenceOrdinal, 1u);
    EXPECT_EQ(destinationRow->childGuid, moving->guid);
    EXPECT_EQ(destinationRow->weight, 50);
}

TEST_F(PlayablePropertyOccurrenceLifecycleTest,
       ReparentRemovesEveryDuplicateOccurrenceAndUndoRestoresThem)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* moving = manager.createItem(
        firstContainer_, QStringLiteral("MovingDup"), ProjectItem::Type::Sound);
    ASSERT_NE(moving, nullptr);
    SelectionEntry duplicate;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *firstContainer_, moving->guid, std::nullopt, &duplicate));
    ASSERT_EQ(firstContainer_->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 2u);

    UndoRedoManager::instance().clear();
    ASSERT_TRUE(manager.moveItem(moving, secondContainer_));
    EXPECT_EQ(occurrenceFor(firstContainer_, moving->guid), std::nullopt);
    EXPECT_EQ(firstContainer_->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 0u);
    EXPECT_EQ(moving->parent, secondContainer_);
    EXPECT_EQ(secondContainer_->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 1u);
    EXPECT_EQ(occurrenceFor(secondContainer_, moving->guid)->childGuid, moving->guid);

    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(moving->parent, firstContainer_);
    EXPECT_EQ(secondContainer_->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 0u);
    ASSERT_EQ(firstContainer_->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 2u);
    EXPECT_EQ(firstContainer_->comp<VNX::VSelection>()->selections.shared.occurrenceRows()[0].childGuid, moving->guid);
    EXPECT_EQ(firstContainer_->comp<VNX::VSelection>()->selections.shared.occurrenceRows()[1].childGuid, moving->guid);
    EXPECT_EQ(firstContainer_->comp<VNX::VSelection>()->selections.shared.occurrenceRows()[1].stableOccurrenceOrdinal,
              duplicate.stableOccurrenceOrdinal);

    UndoRedoManager::instance().undoStack()->redo();
    EXPECT_EQ(moving->parent, secondContainer_);
    EXPECT_EQ(occurrenceFor(firstContainer_, moving->guid), std::nullopt);
    EXPECT_EQ(secondContainer_->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 1u);
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
    std::swap(firstContainer_->children[0], firstContainer_->children[1]);

    ProjectSerializer serializer;
    ASSERT_TRUE(serializer.prepareReferenceWrite(*manager.currentProject()));
    const QJsonObject json = serializer.projectItemToJson(*firstContainer_);
    ASSERT_FALSE(json.isEmpty()) << serializer.lastError().toStdString();
    ProjectItem restored;
    ASSERT_TRUE(serializer.projectItemFromJson(json, restored));
    ASSERT_EQ(restored.children.size(), 2u);
    EXPECT_EQ(restored.children[0]->guid, second->guid);
    EXPECT_EQ(restored.children[1]->guid, first->guid);
    auto restoredFirst = occurrenceFor(&restored, first->guid);
    auto restoredSecond = occurrenceFor(&restored, second->guid);
    ASSERT_NE(restoredFirst, std::nullopt);
    ASSERT_NE(restoredSecond, std::nullopt);
    EXPECT_EQ(restoredFirst->stableOccurrenceOrdinal, 0u);
    EXPECT_EQ(restoredFirst->weight, 50);
    EXPECT_EQ(restoredSecond->stableOccurrenceOrdinal, 1u);

    ProjectItem* clone = manager.duplicateItem(firstContainer_);
    ASSERT_NE(clone, nullptr);
    ASSERT_EQ(clone->children.size(), 2u);
    ASSERT_EQ(clone->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 2u);
    for (const auto& child : clone->children) {
        ASSERT_NE(occurrenceFor(clone, child->guid), std::nullopt);
    }
    EXPECT_EQ(clone->comp<VNX::VSelection>()->selections.shared.occurrenceOrdinalHighWater, 2u);

    ProjectItem* childClone = manager.duplicateItem(first);
    ASSERT_NE(childClone, nullptr);
    auto clonedOccurrence = occurrenceFor(firstContainer_, childClone->guid);
    ASSERT_NE(clonedOccurrence, std::nullopt);
    EXPECT_EQ(clonedOccurrence->stableOccurrenceOrdinal, 2u);
    EXPECT_EQ(clonedOccurrence->weight, 50);
}

TEST_F(PlayablePropertyOccurrenceLifecycleTest,
       DuplicateChildOccurrencesHaveIndependentStableIdentity)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* first = manager.createItem(
        firstContainer_, QStringLiteral("FirstSound"), ProjectItem::Type::Sound);
    ProjectItem* second = manager.createItem(
        firstContainer_, QStringLiteral("SecondSound"), ProjectItem::Type::Sound);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);

    const auto firstLocator = PlayablePropertyAuthoringV1::containerChildLocator(
        *firstContainer_, first->guid);
    ASSERT_TRUE(firstLocator.has_value());
    SelectionEntry duplicate;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *firstContainer_, first->guid, std::nullopt, &duplicate));
    EXPECT_EQ(duplicate.childGuid, first->guid);
    EXPECT_NE(duplicate.stableOccurrenceOrdinal,
              firstLocator->stableOccurrenceOrdinal);
    EXPECT_TRUE(PlayablePropertyAuthoringV1::validateContainerOccurrences(
        *firstContainer_));

    const auto legacyLocator = PlayablePropertyAuthoringV1::containerChildLocator(
        *firstContainer_, first->guid);
    const auto duplicateLocator = PlayablePropertyAuthoringV1::containerChildLocator(
        *firstContainer_, duplicate.stableOccurrenceOrdinal);
    ASSERT_TRUE(legacyLocator.has_value());
    ASSERT_TRUE(duplicateLocator.has_value());
    EXPECT_EQ(legacyLocator->stableOccurrenceOrdinal,
              firstLocator->stableOccurrenceOrdinal);
    EXPECT_EQ(duplicateLocator->occurrenceGuid, first->guid);

    SelectionEntry previous;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::replaceContainerChildOccurrence(
        *firstContainer_, duplicate.stableOccurrenceOrdinal, second->guid,
        &previous));
    EXPECT_EQ(previous, duplicate);
    const auto replacedLocator = PlayablePropertyAuthoringV1::containerChildLocator(
        *firstContainer_, duplicate.stableOccurrenceOrdinal);
    ASSERT_TRUE(replacedLocator.has_value());
    EXPECT_EQ(replacedLocator->occurrenceGuid, second->guid);
    const auto replacedRows = firstContainer_->comp<VNX::VSelection>()->selections.shared.occurrenceRows();
    const auto replacedRow = std::find_if(
        replacedRows.cbegin(),
        replacedRows.cend(),
        [&](const auto& row) {
            return row.stableOccurrenceOrdinal == duplicate.stableOccurrenceOrdinal;
        });
    ASSERT_NE(replacedRow,
              replacedRows.cend());
    EXPECT_EQ(replacedRow->weight, 50);
    const SelectionEntry expectedRemoved = *replacedRow;
    EXPECT_TRUE(PlayablePropertyAuthoringV1::validateContainerOccurrences(
        *firstContainer_));

    const auto secondLocator = PlayablePropertyAuthoringV1::containerChildLocator(
        *firstContainer_, second->guid);
    ASSERT_TRUE(secondLocator.has_value());
    const auto legacyRemoved =
        PlayablePropertyAuthoringV1::removeContainerChildOccurrence(
            *firstContainer_, second->guid);
    ASSERT_TRUE(legacyRemoved.has_value());
    EXPECT_EQ(legacyRemoved->stableOccurrenceOrdinal,
              secondLocator->stableOccurrenceOrdinal);

    const auto removed = PlayablePropertyAuthoringV1::removeContainerChildOccurrence(
        *firstContainer_, duplicate.stableOccurrenceOrdinal);
    ASSERT_TRUE(removed.has_value());
    EXPECT_EQ(*removed, expectedRemoved);
    EXPECT_FALSE(PlayablePropertyAuthoringV1::containerChildLocator(
        *firstContainer_, duplicate.stableOccurrenceOrdinal).has_value());
    EXPECT_TRUE(PlayablePropertyAuthoringV1::validateContainerOccurrences(
        *firstContainer_));
}

TEST_F(PlayablePropertyOccurrenceLifecycleTest,
       DuplicateRowsAndPlatformTablesRoundTripWithIndependentWeights)
{
    auto& manager = ProjectManager::instance();
    auto* child = manager.createItem(firstContainer_, QStringLiteral("Sound"), ProjectItem::Type::Sound);
    ASSERT_NE(child, nullptr);
    SelectionEntry duplicate;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *firstContainer_, child->guid, std::nullopt, &duplicate));
    auto& selections = firstContainer_->comp<VNX::VSelection>()->selections;
    selections.shared.find(0)->weight = 20;
    selections.shared.find(1)->weight = 80;
    auto& android = selections.editTable(HarkenTest::PlatformIds::Android);
    auto androidRows = android.occurrenceRows();
    std::swap(androidRows[0], androidRows[1]);
    androidRows[0].weight = 70;
    ASSERT_TRUE(android.setOccurrenceRows(androidRows));
    const auto before = selections;
    const auto json = selections.toJson();
    const auto parsed = SelectionData::fromJson(json);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(*parsed, before);
    auto malformed = json;
    auto shared = malformed.value(QStringLiteral("shared")).toObject();
    auto rows = shared.value(QStringLiteral("entries")).toArray();
    auto row = rows[0].toObject();
    row.insert(QStringLiteral("playableProperties"), QJsonObject{});
    rows[0] = row;
    shared.insert(QStringLiteral("entries"), rows);
    malformed.insert(QStringLiteral("shared"), shared);
    EXPECT_FALSE(SelectionData::fromJson(malformed));
}

TEST_F(PlayablePropertyOccurrenceLifecycleTest,
       LocatorsFollowActiveOccurrenceAndMemberIdentityAcrossModes)
{
    auto* child = ProjectManager::instance().createItem(
        firstContainer_, QStringLiteral("Sound"), ProjectItem::Type::Sound);
    ASSERT_NE(child, nullptr);
    auto* selection = firstContainer_->comp<VSelection>();
    const auto memberOrdinal = selection->selections.shared.member(child->guid)->stableMemberOrdinal;
    SelectionEntry duplicate;
    ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *firstContainer_, child->guid, std::nullopt, &duplicate));
    ASSERT_TRUE(PlayablePropertyAuthoringV1::removeContainerChildOccurrence(
        *firstContainer_, memberOrdinal));
    const auto occurrenceLocator = PlayablePropertyAuthoringV1::containerChildLocator(
        *firstContainer_, child->guid);
    ASSERT_TRUE(occurrenceLocator);
    EXPECT_EQ(occurrenceLocator->stableOccurrenceOrdinal, duplicate.stableOccurrenceOrdinal);
    EXPECT_FALSE(PlayablePropertyAuthoringV1::containerChildLocator(*firstContainer_, memberOrdinal));

    DiscreteGridDef grid;
    ASSERT_TRUE(grid.addRow(child->guid));
    ASSERT_TRUE(selection->selections.shared.setMatchGrid(grid));
    selection->mode = SelectionMode::Match;
    const auto memberLocator = PlayablePropertyAuthoringV1::containerChildLocator(
        *firstContainer_, child->guid);
    ASSERT_TRUE(memberLocator);
    EXPECT_EQ(memberLocator->stableOccurrenceOrdinal, memberOrdinal);
    EXPECT_TRUE(PlayablePropertyAuthoringV1::containerChildLocator(*firstContainer_, memberOrdinal));
    EXPECT_FALSE(PlayablePropertyAuthoringV1::containerChildLocator(
        *firstContainer_, duplicate.stableOccurrenceOrdinal));
    ASSERT_TRUE(selection->selections.shared.setMatchGrid({}));
    EXPECT_FALSE(PlayablePropertyAuthoringV1::containerChildLocator(*firstContainer_, child->guid));
    selection->mode = SelectionMode::Sequence;
    EXPECT_EQ(PlayablePropertyAuthoringV1::containerChildLocator(*firstContainer_, child->guid),
              occurrenceLocator);
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
    malformed.shared.entries.front().occurrences.front().stableOccurrenceOrdinal = 9;
    reject(malformed);
    malformed = before;
    malformed.shared.entries.front().childGuid = QUuid{};
    reject(malformed);
    malformed = before;
    malformed.shared.entries.front().occurrences.front().weight = 101;
    reject(malformed);
    malformed = before;
    malformed.platforms.insert(QStringLiteral("invalid-platform"), malformed.shared);
    reject(malformed);
}

}
