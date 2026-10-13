#include "Core/Components/VSelection.h"
#include "Core/ContainerBehaviorSemantics.h"

#include <gtest/gtest.h>

#include <QJsonArray>

#include <algorithm>

using namespace VNX;

namespace {

QList<uint32_t> occurrenceOrdinals(int count)
{
    QList<uint32_t> result;
    while (result.size() < count)
        result.push_back(static_cast<uint32_t>(result.size()));
    return result;
}

SelectionTable table(int count)
{
    SelectionTable result;
    result.occurrenceOrdinalHighWater = static_cast<uint64_t>(count);
    std::vector<SelectionEntry> rows;
    for (int index = 0; index < count; ++index)
        rows.push_back({QUuid::createUuid(), static_cast<uint32_t>(index), 50});
    result.setOccurrenceRows(rows);
    return result;
}

}

TEST(ContainerBehaviorSemanticsTest, DisciplineChangesLabelWithoutChangingWeights)
{
    VSelection selection;
    constexpr uint32_t child = 7;
    selection.selections.shared.occurrenceOrdinalHighWater = 8;
    selection.selections.shared.setOccurrenceRows({{QUuid::createUuid(), child, 73}});
    EXPECT_EQ(ContainerBehaviorSemantics::childWeightLabel(selection), QStringLiteral("Frequency"));
    selection.behavior.discipline = RandomDiscipline::Shuffle;
    EXPECT_EQ(ContainerBehaviorSemantics::childWeightLabel(selection), QStringLiteral("Order"));
    EXPECT_EQ(ContainerBehaviorSemantics::effectiveChildWeight(selection, child), 73);
}

TEST(ContainerBehaviorSemanticsTest, DormantWeightsSurviveModeChanges)
{
    VSelection selection;
    selection.selections.shared = table(2);
    selection.selections.shared.find(0)->weight = 0;
    selection.mode = SelectionMode::Sequence;
    EXPECT_EQ(selection.selections.shared.occurrenceRows()[0].weight, 0);
    selection.mode = SelectionMode::Random;
    selection.playType = SelectionPlayType::Queue;
    EXPECT_EQ(ContainerBehaviorSemantics::effectiveChildWeight(selection, 0), 1);
    selection.mode = SelectionMode::Random;
    selection.playType = SelectionPlayType::Single;
    EXPECT_EQ(ContainerBehaviorSemantics::effectiveChildWeight(selection, 0), 0);
}

TEST(ContainerBehaviorSemanticsTest, SelectionJsonRejectsDuplicateOrdinalsAndInvalidWeights)
{
    auto original = table(2);
    EXPECT_TRUE(SelectionTable::fromJson(original.toJson()));
    auto invalid = original;
    invalid.entries[1].occurrences[0].stableOccurrenceOrdinal = 0;
    EXPECT_FALSE(SelectionTable::fromJson(invalid.toJson()));
    invalid = original;
    invalid.entries[0].occurrences[0].stableOccurrenceOrdinal = 9;
    EXPECT_FALSE(SelectionTable::fromJson(invalid.toJson()));
    invalid = original;
    invalid.entries[0].occurrences[0].weight = 101;
    EXPECT_FALSE(SelectionTable::fromJson(invalid.toJson()));
    invalid.entries[0].occurrences[0].weight = -1;
    EXPECT_FALSE(SelectionTable::fromJson(invalid.toJson()));
    EXPECT_FALSE(ContainerBehaviorSemantics::fromJson(
        {{QStringLiteral("childWeights"), QJsonArray{}}}));
}

TEST(ContainerBehaviorSemanticsTest, PlatformAvoidRepeatDoesNotRewriteAuthoredBehavior)
{
    VSelection selection;
    selection.selections.shared = table(3);
    selection.behavior.avoidRepeat = 2;
    const QUuid platform = QUuid::createUuid();
    auto& override = selection.selections.editTable(platform);
    override.entries.resize(1);
    EXPECT_EQ(ContainerBehaviorSemantics::effectiveAvoidRepeat(selection), 2);
    EXPECT_EQ(ContainerBehaviorSemantics::effectiveAvoidRepeat(selection, platform), 0);
    EXPECT_EQ(selection.behavior.avoidRepeat, 2);
    override.entries[0].occurrences[0].weight = 0;
    EXPECT_FALSE(ContainerBehaviorSemantics::validateSelections(selection));
    override.entries.clear();
    EXPECT_TRUE(ContainerBehaviorSemantics::validateSelections(selection));
}

TEST(ContainerBehaviorSemanticsTest, DrawNeverSelectsZeroWeightChildren)
{
    VSelection selection;
    selection.mode = SelectionMode::Random;
    const QList<uint32_t> ids = occurrenceOrdinals(3);
    selection.selections.shared = table(3);
    selection.selections.shared.find(0)->weight = 0;
    selection.selections.shared.find(1)->weight = 2;
    selection.selections.shared.find(2)->weight = 3;
    selection.behavior.avoidRepeat = 1;
    ContainerBehaviorSemantics::Memory memory;
    for (uint64_t entropy = 1; entropy <= 32; ++entropy) {
        const auto step = ContainerBehaviorSemantics::advance(
            selection, ids, entropy, memory);
        ASSERT_TRUE(step.child.has_value());
        EXPECT_NE(*step.child, ids[0]);
    }
}

TEST(ContainerBehaviorSemanticsTest, DrawAvoidRepeatUsesRecentWindow)
{
    VSelection selection;
    selection.mode = SelectionMode::Random;
    selection.behavior.avoidRepeat = 2;
    const QList<uint32_t> ids = occurrenceOrdinals(3);
    ContainerBehaviorSemantics::Memory memory;
    std::vector<uint32_t> results;
    for (uint64_t entropy = 1; entropy <= 18; ++entropy) {
        const auto step = ContainerBehaviorSemantics::advance(selection, ids, entropy, memory);
        ASSERT_TRUE(step.child);
        results.push_back(*step.child);
    }
    for (size_t index = 1; index < results.size(); ++index)
        EXPECT_NE(results[index], results[index - 1]);
    for (size_t index = 2; index < results.size(); ++index)
        EXPECT_NE(results[index], results[index - 2]);
}

TEST(ContainerBehaviorSemanticsTest, ShuffleVisitsEveryChildAndAvoidsBoundaryRepeat)
{
    VSelection selection;
    selection.mode = SelectionMode::Random;
    selection.behavior.discipline = RandomDiscipline::Shuffle;
    selection.behavior.avoidRepeat = 1;
    const QList<uint32_t> ids = occurrenceOrdinals(4);
    ContainerBehaviorSemantics::Memory memory;
    std::vector<uint32_t> first;
    std::vector<uint32_t> second;
    for (int index = 0; index < 8; ++index) {
        auto step = ContainerBehaviorSemantics::advance(selection, ids, 17 + index, memory);
        ASSERT_TRUE(step.child);
        (index < 4 ? first : second).push_back(*step.child);
    }
    for (uint32_t id : ids) {
        EXPECT_EQ(std::count(first.begin(), first.end(), id), 1);
        EXPECT_EQ(std::count(second.begin(), second.end(), id), 1);
    }
    EXPECT_NE(first.back(), second.front());
}

TEST(ContainerBehaviorSemanticsTest, SequenceCursorWrapsDeterministically)
{
    VSelection selection;
    selection.mode = SelectionMode::Sequence;
    const QList<uint32_t> ids = occurrenceOrdinals(3);
    ContainerBehaviorSemantics::Memory memory;
    for (int index = 0; index < 6; ++index) {
        const auto step = ContainerBehaviorSemantics::advance(selection, ids, 0, memory);
        ASSERT_TRUE(step.child);
        EXPECT_EQ(*step.child, ids[index % 3]);
        EXPECT_EQ(step.roundCompleted, index % 3 == 2);
    }
}
