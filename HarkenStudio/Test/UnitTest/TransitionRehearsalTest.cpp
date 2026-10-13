#include "Core/Components/VSelection.h"
#include "Core/Components/VVariable.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectTypes.h"
#include "Core/TransitionRehearsal.h"
#include "Core/TransitionSemantics.h"
#include "Core/VariableRehearsal.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QHash>
#include <QTemporaryDir>

#include <memory>

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

std::shared_ptr<ProjectItem> makeItem(ProjectItem::Type type, const QString& name)
{
    auto item = std::make_shared<ProjectItem>(name, type);
    item->guid = QUuid::createUuid();
    item->attachDefaultComponents();
    return item;
}

TimelineItemResolver resolverFor(const std::vector<std::shared_ptr<ProjectItem>>& items)
{
    auto index = std::make_shared<QHash<QUuid, const ProjectItem*>>();
    for (const auto& item : items) index->insert(item->guid, item.get());
    return [index](const QUuid& guid) { return index->value(guid, nullptr); };
}

TransitionRuleDef ruleFor(std::optional<QUuid> source, std::optional<QUuid> target)
{
    TransitionRuleDef rule = canonicalTransitionRule();
    rule.source = source;
    rule.target = target;
    return rule;
}

}

TEST(TransitionRehearsalUnit, RefreshWinnerAdvancesStateAndEmitsOnCurrentChange)
{
    ensureApp();
    TransitionRehearsal rehearsal;
    int emits = 0;
    QObject::connect(&rehearsal, &TransitionRehearsal::rehearsalChanged,
                     [&](const QUuid&) { ++emits; });

    const QUuid container = QUuid::createUuid();
    const QUuid a = QUuid::createUuid();
    const QUuid b = QUuid::createUuid();

    EXPECT_FALSE(rehearsal.previousWinner(container).has_value());
    EXPECT_FALSE(rehearsal.currentWinner(container).has_value());

    rehearsal.refreshWinner(container, a);
    EXPECT_FALSE(rehearsal.previousWinner(container).has_value());
    ASSERT_TRUE(rehearsal.currentWinner(container).has_value());
    EXPECT_EQ(*rehearsal.currentWinner(container), a);
    EXPECT_EQ(emits, 1);

    rehearsal.refreshWinner(container, a);
    EXPECT_FALSE(rehearsal.previousWinner(container).has_value());
    EXPECT_EQ(*rehearsal.currentWinner(container), a);
    EXPECT_EQ(emits, 1);

    rehearsal.refreshWinner(container, b);
    EXPECT_EQ(*rehearsal.previousWinner(container), a);
    EXPECT_EQ(*rehearsal.currentWinner(container), b);
    EXPECT_EQ(emits, 2);

    rehearsal.refreshWinner(container, std::nullopt);
    EXPECT_EQ(*rehearsal.previousWinner(container), b);
    EXPECT_FALSE(rehearsal.currentWinner(container).has_value());
    EXPECT_EQ(emits, 3);
}


TEST(TransitionRehearsalUnit, SameWinnerObservationDoesNotAdvanceOrEmit)
{
    ensureApp();
    TransitionRehearsal rehearsal;
    const QUuid container = QUuid::createUuid();
    const QUuid winner = QUuid::createUuid();
    int emits = 0;
    QObject::connect(&rehearsal, &TransitionRehearsal::rehearsalChanged,
                     [&](const QUuid&) { ++emits; });

    rehearsal.refreshWinner(container, winner);
    rehearsal.refreshWinner(container, winner);

    EXPECT_FALSE(rehearsal.previousWinner(container).has_value());
    ASSERT_TRUE(rehearsal.currentWinner(container).has_value());
    EXPECT_EQ(*rehearsal.currentWinner(container), winner);
    EXPECT_EQ(emits, 1);
}

TEST(TransitionRehearsalUnit, ClearAndClearAllResetState)
{
    ensureApp();
    TransitionRehearsal rehearsal;
    const QUuid containerA = QUuid::createUuid();
    const QUuid containerB = QUuid::createUuid();
    rehearsal.refreshWinner(containerA, QUuid::createUuid());
    rehearsal.refreshWinner(containerB, QUuid::createUuid());

    int emits = 0;
    QObject::connect(&rehearsal, &TransitionRehearsal::rehearsalChanged,
                     [&](const QUuid&) { ++emits; });

    rehearsal.clear(containerA);
    EXPECT_FALSE(rehearsal.currentWinner(containerA).has_value());
    EXPECT_TRUE(rehearsal.currentWinner(containerB).has_value());
    EXPECT_EQ(emits, 1);

    rehearsal.clearAll();
    EXPECT_FALSE(rehearsal.currentWinner(containerB).has_value());
    EXPECT_EQ(emits, 2);
}

TEST(TransitionRehearsalUnit, HandoverForDerivesSilentEntryTransitionSourceExitAndSameObject)
{
    ensureApp();
    const QUuid a = QUuid::createUuid();
    const QUuid b = QUuid::createUuid();
    const QUuid bridge = QUuid::createUuid();

    TransitionTableDef table = canonicalTransitionTable();
    TransitionRuleDef transitionRule = ruleFor(a, b);
    transitionRule.bridge = bridge;
    ASSERT_TRUE(table.addRule(transitionRule));
    ASSERT_TRUE(table.addRule(ruleFor(b, std::nullopt)));

    const QUuid container = QUuid::createUuid();
    TransitionRehearsal rehearsal;

    rehearsal.refreshWinner(container, a);
    TransitionSemantics::HandoverResult handover = rehearsal.handoverFor(container, table);
    EXPECT_EQ(handover.kind, TransitionSemantics::HandoverKind::SilentEntry);

    rehearsal.refreshWinner(container, a);
    handover = rehearsal.handoverFor(container, table);
    EXPECT_EQ(handover.kind, TransitionSemantics::HandoverKind::SilentEntry);

    rehearsal.refreshWinner(container, b);
    handover = rehearsal.handoverFor(container, table);
    ASSERT_EQ(handover.kind, TransitionSemantics::HandoverKind::Transition);
    ASSERT_EQ(handover.referenceChain.size(), 3u);
    EXPECT_EQ(handover.referenceChain[0], a);
    EXPECT_EQ(handover.referenceChain[1], bridge);
    EXPECT_EQ(handover.referenceChain[2], b);
    ASSERT_EQ(handover.ruleChain.size(), 2u);
    EXPECT_EQ(handover.ruleChain[0], a);
    EXPECT_EQ(handover.ruleChain[1], b);

    rehearsal.refreshWinner(container, std::nullopt);
    handover = rehearsal.handoverFor(container, table);
    EXPECT_EQ(handover.kind, TransitionSemantics::HandoverKind::SourceExit);
}

TEST(TransitionRehearsalUnit, QueueIllustrativeWinnerUsesSelectionOrderAndNoneWhenEmpty)
{
    ensureApp();
    auto container = makeItem(ProjectItem::Type::Container, QStringLiteral("Queue"));
    EXPECT_FALSE(TransitionRehearsal::queueIllustrativeWinner(*container).has_value());

    auto first = makeItem(ProjectItem::Type::Piece, QStringLiteral("First"));
    auto second = makeItem(ProjectItem::Type::Piece, QStringLiteral("Second"));
    first->parent = container.get();
    second->parent = container.get();
    container->children.push_back(first);
    container->children.push_back(second);
    EXPECT_FALSE(TransitionRehearsal::queueIllustrativeWinner(*container).has_value());
    auto& table = container->comp<VSelection>()->selections.shared;
    ASSERT_TRUE(table.setOccurrenceRows({{first->guid, 0, 50}, {second->guid, 1, 50}}));

    const auto winner = TransitionRehearsal::queueIllustrativeWinner(*container);
    ASSERT_TRUE(winner.has_value());
    EXPECT_EQ(*winner, first->guid);
    ASSERT_TRUE(table.setOccurrenceRows({{second->guid, 1, 50}, {first->guid, 0, 50}}));
    EXPECT_EQ(TransitionRehearsal::queueIllustrativeWinner(*container),
              std::optional<QUuid>(second->guid));
    ASSERT_TRUE(table.setOccurrenceRows({}));
    EXPECT_FALSE(TransitionRehearsal::queueIllustrativeWinner(*container).has_value());
}

TEST(TransitionRehearsalUnit, VariableControlWinnerReadsDiscreteAxesThroughRehearsalOverride)
{
    ensureApp();
    auto variable = makeItem(ProjectItem::Type::Variable, QStringLiteral("Axis"));
    VVariable* v = variable->comp<VVariable>();
    ASSERT_NE(v, nullptr);
    v->kind = VariableKind::Step;
    v->rangeMin = 0.0f;
    v->rangeMax = 10.0f;
    v->defaultValue = 0.0f;

    auto childLow = makeItem(ProjectItem::Type::Piece, QStringLiteral("Low"));
    auto childHigh = makeItem(ProjectItem::Type::Piece, QStringLiteral("High"));

    DiscreteGridDef grid;
    ASSERT_TRUE(grid.addAxis(variable->guid));
    ASSERT_TRUE(grid.addRow(childLow->guid));
    ASSERT_TRUE(grid.addRow(childHigh->guid));
    ASSERT_TRUE(grid.setCell(0, 0, 0));
    ASSERT_TRUE(grid.setCell(1, 0, 5));

    const TimelineItemResolver resolve = resolverFor({variable});
    VariableRehearsal rehearsal;

    auto winner = TransitionRehearsal::variableControlWinner(grid, rehearsal, resolve);
    ASSERT_TRUE(winner.has_value());
    EXPECT_EQ(*winner, childLow->guid);

    rehearsal.setOverride(variable->guid, 5.0f);
    winner = TransitionRehearsal::variableControlWinner(grid, rehearsal, resolve);
    ASSERT_TRUE(winner.has_value());
    EXPECT_EQ(*winner, childHigh->guid);

    rehearsal.setOverride(variable->guid, 9.0f);
    winner = TransitionRehearsal::variableControlWinner(grid, rehearsal, resolve);
    EXPECT_FALSE(winner.has_value());
}

TEST(TransitionRehearsalUnit, WinnerForContainerHonorsMountConsumptionState)
{
    ensureApp();
    auto variable = makeItem(ProjectItem::Type::Variable, QStringLiteral("Axis"));
    auto childLow = makeItem(ProjectItem::Type::Piece, QStringLiteral("Low"));
    auto childHigh = makeItem(ProjectItem::Type::Piece, QStringLiteral("High"));
    auto container = makeItem(ProjectItem::Type::Container, QStringLiteral("Container"));
    container->children.push_back(childLow);
    container->children.push_back(childHigh);

    auto* declaration = variable->comp<VVariable>();
    declaration->kind = VariableKind::Step;
    declaration->rangeMin = 0.0f;
    declaration->rangeMax = 10.0f;

    auto* selection = container->comp<VSelection>();
    childLow->parent = container.get();
    childHigh->parent = container.get();
    ASSERT_TRUE(selection->selections.shared.setOccurrenceRows(
        {{childLow->guid, 0, 50}, {childHigh->guid, 1, 50}}));
    const TimelineItemResolver resolve = resolverFor({variable});
    VariableRehearsal rehearsal;

    selection->mode = SelectionMode::Sequence;
    selection->playType = SelectionPlayType::Queue;
    EXPECT_EQ(TransitionRehearsal::winnerForContainer(
                  *container, rehearsal, resolve),
              std::optional<QUuid>(childLow->guid));

    selection->mode = SelectionMode::Sequence;
    selection->playType = SelectionPlayType::Single;
    EXPECT_FALSE(TransitionRehearsal::winnerForContainer(
                     *container, rehearsal, resolve).has_value());

    selection->mode = SelectionMode::Match;
    auto grid = selection->matchGrid();
    ASSERT_TRUE(grid.addAxis(variable->guid));
    ASSERT_TRUE(grid.addRow(childLow->guid));
    ASSERT_TRUE(grid.addRow(childHigh->guid));
    ASSERT_TRUE(grid.setCell(0, 0, 0));
    ASSERT_TRUE(grid.setCell(1, 0, 5));
    ASSERT_TRUE(selection->selections.shared.setMatchGrid(grid));
    selection->matchResponse = VariableResponse::AtStart;
    EXPECT_FALSE(TransitionRehearsal::winnerForContainer(
                     *container, rehearsal, resolve).has_value());

    selection->matchResponse = VariableResponse::Live;
    ASSERT_TRUE(rehearsal.setOverride(variable->guid, 5.0f));
    EXPECT_EQ(TransitionRehearsal::winnerForContainer(
                  *container, rehearsal, resolve),
              std::optional<QUuid>(childHigh->guid));
}

TEST(TransitionRehearsalUnit, VariableControlWinnerHandlesUnresolvedAxisAndEmptyGrid)
{
    ensureApp();
    const DiscreteGridDef emptyGrid;
    VariableRehearsal rehearsal;
    const TimelineItemResolver resolve = resolverFor({});
    EXPECT_FALSE(TransitionRehearsal::variableControlWinner(emptyGrid, rehearsal, resolve)
                     .has_value());

    auto childLow = makeItem(ProjectItem::Type::Piece, QStringLiteral("Low"));
    DiscreteGridDef grid;
    const QUuid missingAxis = QUuid::createUuid();
    ASSERT_TRUE(grid.addAxis(missingAxis));
    ASSERT_TRUE(grid.addRow(childLow->guid));
    ASSERT_TRUE(grid.setCell(0, 0, 0));

    const auto winner = TransitionRehearsal::variableControlWinner(grid, rehearsal, resolve);
    ASSERT_TRUE(winner.has_value());
    EXPECT_EQ(*winner, childLow->guid);
}

class TransitionRehearsalProjectManagerTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("TransitionRehearsalProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(ProjectManager::instance().createProject(config));
    }

    void TearDown() override
    {
        ProjectManager::instance().closeProject();
    }

    QTemporaryDir tempDir_;
};

TEST_F(TransitionRehearsalProjectManagerTest, CloseProjectClearsTransitionRehearsal)
{
    auto& pm = ProjectManager::instance();
    const QUuid container = QUuid::createUuid();
    const QUuid winner = QUuid::createUuid();
    pm.transitionRehearsal().refreshWinner(container, winner);
    ASSERT_TRUE(pm.transitionRehearsal().currentWinner(container).has_value());

    pm.closeProject();

    EXPECT_FALSE(pm.transitionRehearsal().currentWinner(container).has_value());
    EXPECT_FALSE(pm.transitionRehearsal().previousWinner(container).has_value());
}
