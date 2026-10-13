#include "ActionManager.h"
#include "ActionStepEditing.h"
#include "AuthoringPolicy.h"
#include "Components/VAction.h"
#include "Components/VPiece.h"
#include "Components/VSelection.h"
#include "Components/VTrack.h"
#include "Components/VVariable.h"
#include "PlacementContext.h"
#include "ProjectManager.h"
#include "TransitionRehearsal.h"
#include "TransitionSemantics.h"
#include "UndoCommands.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QTemporaryDir>

#include <vector>

using namespace VNX;
namespace TS = VNX::TransitionSemantics;

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

void setNominal(ProjectItem& piece, int64_t entry, int64_t exit)
{
    piece.comp<VPiece>()->markers.items = {
        {QUuid::createUuid(), MarkerKind::Entry, {}, entry},
        {QUuid::createUuid(), MarkerKind::Exit, {}, exit}
    };
}

TimelineItemResolver resolverForManager(ProjectManager& manager)
{
    return [&manager](const QUuid& guid) { return manager.findItemByGuid(guid); };
}

ActionResult setProp(ProjectItem* item, const QString& property, const QJsonValue& value)
{
    return ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        {{QStringLiteral("authoring_handle"), static_cast<qint64>(item->objectID)},
         {QStringLiteral("property"), property},
         {QStringLiteral("value"), value}},
        ActionSource::GUI);
}

QJsonObject quantizeJson(qint64 referenceObjectID, const QString& referenceName,
                        const QString& timing, const QString& ifSilent)
{
    return QJsonObject{
        {QStringLiteral("referenceObjectID"), referenceObjectID},
        {QStringLiteral("referenceName"), referenceName},
        {QStringLiteral("timing"), timing},
        {QStringLiteral("ifSilent"), ifSilent}
    };
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

TransitionRuleDef ruleFor(std::optional<QUuid> source, std::optional<QUuid> target)
{
    TransitionRuleDef rule = AuthoringPolicy::defaultTransitionRule();
    rule.source = source;
    rule.target = target;
    return rule;
}

class ActionTransitionAuthoringScenarioTest : public ::testing::Test
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
        config.name = QStringLiteral("ActionTransitionAuthoringScenarioProject");
        config.location = tempDir_.path();
        ASSERT_TRUE(manager.createProject(config));
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    QTemporaryDir tempDir_;
};

}

TEST_F(ActionTransitionAuthoringScenarioTest,
       VariableControlMountStatesGateRealAuthoringAndIgnoreContinuousOnlyGrid)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* axisVar = manager.createItem(
        nullptr, QStringLiteral("Axis"), ProjectItem::Type::Variable);
    ASSERT_NE(axisVar, nullptr);
    auto* declaration = axisVar->comp<VVariable>();
    declaration->kind = VariableKind::Step;
    declaration->rangeMin = 0.0f;
    declaration->rangeMax = 10.0f;

    ProjectItem* vcContainer = manager.createItem(
        nullptr, QStringLiteral("VCContainer"), ProjectItem::Type::Container);
    ProjectItem* childLow = manager.createItem(
        vcContainer, QStringLiteral("Low"), ProjectItem::Type::Sound);
    ProjectItem* childHigh = manager.createItem(
        vcContainer, QStringLiteral("High"), ProjectItem::Type::Sound);
    ASSERT_NE(vcContainer, nullptr);
    ASSERT_NE(childLow, nullptr);
    ASSERT_NE(childHigh, nullptr);

    auto* selection = vcContainer->comp<VSelection>();
    selection->mode = SelectionMode::Match;
    EXPECT_EQ(TS::transitionMountState(*vcContainer), TransitionMountState::None);

    selection->matchResponse = VariableResponse::AtStart;
    {
        auto grid = selection->matchGrid();
        ASSERT_TRUE(grid.addAxis(axisVar->guid));
        ASSERT_TRUE(grid.addRow(childLow->guid));
        ASSERT_TRUE(grid.addRow(childHigh->guid));
        ASSERT_TRUE(grid.setCell(0, 0, 0));
        ASSERT_TRUE(grid.setCell(1, 0, 5));
        EXPECT_TRUE(selection->selections.shared.setMatchGrid(grid));
    }
    EXPECT_EQ(TS::transitionMountState(*vcContainer), TransitionMountState::Idle);

    selection->matchResponse = VariableResponse::Live;
    EXPECT_EQ(TS::transitionMountState(*vcContainer), TransitionMountState::Editable);

    TransitionTableDef table = AuthoringPolicy::defaultTransitionTable();
    ASSERT_TRUE(table.addRule(ruleFor(childLow->guid, childHigh->guid)));
    const ActionResult authored = setProp(vcContainer, QStringLiteral("transitions"),
                                          table.toJson());
    ASSERT_TRUE(authored.success) << authored.errorMessage.toStdString();
    EXPECT_EQ(vcContainer->comp<VSelection>()->transitions, table);

    ProjectItem* continuousOnlyContainer = manager.createItem(
        nullptr, QStringLiteral("ContinuousOnlyContainer"), ProjectItem::Type::Container);
    ASSERT_NE(continuousOnlyContainer, nullptr);
    auto* continuousOnlySelection = continuousOnlyContainer->comp<VSelection>();
    continuousOnlySelection->mode = SelectionMode::Blend;
    {
        auto mixing = continuousOnlySelection->mixing();
        mixing.rows.push_back({axisVar->guid, {}});
        mixing.rows.back().children.push_back(VNX::MixingChildDef::unity(childLow->guid, 0.0f, 100.0f));
        mixing.rows.front().children[0].normalize();
        EXPECT_TRUE(continuousOnlySelection->selections.shared.setMixing(mixing));
    }
    EXPECT_TRUE(continuousOnlySelection->matchGrid().axes.empty());
    EXPECT_EQ(TS::transitionMountState(*continuousOnlyContainer), TransitionMountState::None);
}

TEST_F(ActionTransitionAuthoringScenarioTest,
       QueueIllustrativeWinnerStaysStaticWhileRehearsalDrivesBridgedHandover)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* queueContainer = manager.createItem(
        nullptr, QStringLiteral("QueueContainer"), ProjectItem::Type::Container);
    ProjectItem* qFirst = manager.createItem(
        queueContainer, QStringLiteral("First"), ProjectItem::Type::Sound);
    ProjectItem* qSecond = manager.createItem(
        queueContainer, QStringLiteral("Second"), ProjectItem::Type::Sound);
    ProjectItem* bridgeSound = manager.createItem(
        nullptr, QStringLiteral("BridgeSound"), ProjectItem::Type::Sound);
    ASSERT_NE(queueContainer, nullptr);
    ASSERT_NE(qFirst, nullptr);
    ASSERT_NE(qSecond, nullptr);
    ASSERT_NE(bridgeSound, nullptr);

    queueContainer->comp<VSelection>()->mode = SelectionMode::Sequence;
    queueContainer->comp<VSelection>()->playType = SelectionPlayType::Queue;

    const auto illustrative = TransitionRehearsal::queueIllustrativeWinner(*queueContainer);
    ASSERT_TRUE(illustrative.has_value());
    EXPECT_EQ(*illustrative, qFirst->guid);

    TransitionRuleDef bridged = ruleFor(qFirst->guid, qSecond->guid);
    bridged.bridge = bridgeSound->guid;
    TransitionTableDef table = AuthoringPolicy::defaultTransitionTable();
    ASSERT_TRUE(table.addRule(bridged));
    const ActionResult authored = setProp(queueContainer, QStringLiteral("transitions"),
                                          table.toJson());
    ASSERT_TRUE(authored.success) << authored.errorMessage.toStdString();

    TransitionRehearsal& rehearsal = manager.transitionRehearsal();
    rehearsal.refreshWinner(queueContainer->guid, qFirst->guid);
    EXPECT_FALSE(rehearsal.previousWinner(queueContainer->guid).has_value());
    EXPECT_EQ(*TransitionRehearsal::queueIllustrativeWinner(*queueContainer), qFirst->guid);

    rehearsal.refreshWinner(queueContainer->guid, qSecond->guid);
    ASSERT_TRUE(rehearsal.previousWinner(queueContainer->guid).has_value());
    EXPECT_EQ(*rehearsal.previousWinner(queueContainer->guid), qFirst->guid);
    EXPECT_EQ(*TransitionRehearsal::queueIllustrativeWinner(*queueContainer), qFirst->guid);

    const auto& liveTable = queueContainer->comp<VSelection>()->transitions;
    const TS::HandoverResult handover = rehearsal.handoverFor(queueContainer->guid, liveTable);
    ASSERT_EQ(handover.kind, TS::HandoverKind::Transition);
    EXPECT_EQ(handover.referenceChain,
             (std::vector<QUuid>{qFirst->guid, bridgeSound->guid, qSecond->guid}));
    EXPECT_EQ(handover.ruleChain, (std::vector<QUuid>{qFirst->guid, qSecond->guid}));

    EXPECT_EQ(TS::deriveHandover(liveTable, qFirst->guid, qFirst->guid).kind,
             TS::HandoverKind::SameObject);

    const TS::HandoverResult fallback = TS::deriveHandover(
        liveTable, bridgeSound->guid, QUuid::createUuid());
    EXPECT_EQ(fallback.kind, TS::HandoverKind::Transition);
    EXPECT_EQ(fallback.ruleIndex, 0);
}

TEST_F(ActionTransitionAuthoringScenarioTest,
       WatchResolutionPrefersRealPlacementOverRealOrganizationalParentage)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* orgPiece = manager.createItem(
        nullptr, QStringLiteral("OrgPiece"), ProjectItem::Type::Piece);
    ProjectItem* orgTrack = manager.createItem(
        orgPiece, QStringLiteral("OrgTrack"), ProjectItem::Type::Track);
    ProjectItem* watchContainer = manager.createItem(
        orgTrack, QStringLiteral("WatchContainer"), ProjectItem::Type::Container);
    ProjectItem* placementPiece = manager.createItem(
        nullptr, QStringLiteral("PlacementPiece"), ProjectItem::Type::Piece);
    ProjectItem* placementTrack = manager.createItem(
        placementPiece, QStringLiteral("PlacementTrack"), ProjectItem::Type::Track);
    ASSERT_NE(orgPiece, nullptr);
    ASSERT_NE(orgTrack, nullptr);
    ASSERT_NE(watchContainer, nullptr);
    ASSERT_NE(placementPiece, nullptr);
    ASSERT_NE(placementTrack, nullptr);

    const Clip placementClip{QUuid::createUuid(), watchContainer->guid, 0, 1};
    placementTrack->comp<VTrack>()->clips.items = {placementClip};

    const TimelineItemResolver resolve = resolverForManager(manager);

    const auto organized = TS::resolveWatchReference(
        watchContainer, *watchContainer, nullptr, resolve);
    ASSERT_TRUE(organized) << organized.failureReason.toStdString();
    EXPECT_EQ(organized.direction, TS::TransitionWatchDirection::UpOrganization);
    EXPECT_EQ(organized.referencePiece, orgPiece);

    const PlacementContext placement = PlacementContext::fromOccurrence(
        {placementTrack->guid, placementClip.clipGuid, watchContainer->guid});
    const auto placed = TS::resolveWatchReference(
        watchContainer, *watchContainer, &placement, resolve);
    ASSERT_TRUE(placed) << placed.failureReason.toStdString();
    EXPECT_EQ(placed.direction, TS::TransitionWatchDirection::UpPlacement);
    EXPECT_EQ(placed.referencePiece, placementPiece);

    ProjectItem* isolatedContainer = manager.createItem(
        nullptr, QStringLiteral("IsolatedContainer"), ProjectItem::Type::Container);
    ASSERT_NE(isolatedContainer, nullptr);
    const auto none = TS::resolveWatchReference(
        isolatedContainer, *isolatedContainer, nullptr, resolve);
    EXPECT_FALSE(none);
    EXPECT_EQ(none.direction, TS::TransitionWatchDirection::None);
    EXPECT_FALSE(none.failureReason.isEmpty());
}

TEST_F(ActionTransitionAuthoringScenarioTest,
       SamePositionEntryModeRuleValidatesThroughPropertyRouteOnRealChildren)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* spContainer = manager.createItem(
        nullptr, QStringLiteral("SamePositionContainer"), ProjectItem::Type::Container);
    ProjectItem* sourcePiece = manager.createItem(
        spContainer, QStringLiteral("SourcePiece"), ProjectItem::Type::Piece);
    ProjectItem* targetPiece = manager.createItem(
        spContainer, QStringLiteral("TargetPiece"), ProjectItem::Type::Piece);
    ASSERT_NE(spContainer, nullptr);
    ASSERT_NE(sourcePiece, nullptr);
    ASSERT_NE(targetPiece, nullptr);

    spContainer->comp<VSelection>()->mode = SelectionMode::Sequence;
    spContainer->comp<VSelection>()->playType = SelectionPlayType::Queue;
    setNominal(*sourcePiece, -960, 7680);
    setNominal(*targetPiece, 480, 4320);

    TransitionRuleDef sameEntry = ruleFor(sourcePiece->guid, targetPiece->guid);
    sameEntry.entryMode = TransitionEntryMode::SamePosition;
    TransitionTableDef table = AuthoringPolicy::defaultTransitionTable();
    ASSERT_TRUE(table.addRule(sameEntry));
    const ActionResult authored = setProp(spContainer, QStringLiteral("transitions"),
                                          table.toJson());
    ASSERT_TRUE(authored.success) << authored.errorMessage.toStdString();
    EXPECT_EQ(spContainer->comp<VSelection>()->transitions.rules[1].entryMode,
             TransitionEntryMode::SamePosition);

    const TimelineItemResolver resolve = resolverForManager(manager);
    const TS::SamePositionResult mapped = TS::samePositionMap(
        7200, *sourcePiece, *targetPiece, resolve);
    ASSERT_TRUE(mapped) << mapped.validation.message.toStdString();
    EXPECT_EQ(mapped.targetNominalLength, 3840);
    EXPECT_EQ(mapped.targetOffset, 480);
    EXPECT_EQ(mapped.targetPosition, 960);
}

TEST_F(ActionTransitionAuthoringScenarioTest,
       FiveVerbActionTableCoversMarkerTimingAbsentBehaviorAndSetValueNormalization)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* action1 = manager.createItem(
        nullptr, QStringLiteral("MainAction"), ProjectItem::Type::Action);
    ProjectItem* action2 = manager.createItem(
        nullptr, QStringLiteral("FollowupAction"), ProjectItem::Type::Action);
    ProjectItem* playTarget = manager.createItem(
        nullptr, QStringLiteral("PlayTarget"), ProjectItem::Type::Sound);
    ProjectItem* stopTarget = manager.createItem(
        nullptr, QStringLiteral("StopTarget"), ProjectItem::Type::Sound);
    ProjectItem* clockRef = manager.createItem(
        nullptr, QStringLiteral("ClockRef"), ProjectItem::Type::Container);
    ProjectItem* resetTarget = manager.createItem(
        nullptr, QStringLiteral("ResetTarget"), ProjectItem::Type::Container);
    ProjectItem* speedVar = manager.createItem(
        nullptr, QStringLiteral("SpeedVar"), ProjectItem::Type::Variable);
    ASSERT_NE(action1, nullptr);
    ASSERT_NE(action2, nullptr);
    ASSERT_NE(playTarget, nullptr);
    ASSERT_NE(stopTarget, nullptr);
    ASSERT_NE(clockRef, nullptr);
    ASSERT_NE(resetTarget, nullptr);
    ASSERT_NE(speedVar, nullptr);

    auto* declaration = speedVar->comp<VVariable>();
    declaration->kind = VariableKind::Step;
    declaration->rangeMin = 0.0f;
    declaration->rangeMax = 5.0f;

    QJsonObject play = stepJson(QStringLiteral("Play"), playTarget);
    play[QStringLiteral("quantize")] = quantizeJson(
        static_cast<qint64>(clockRef->objectID), clockRef->name,
        QStringLiteral("NextMarker"), QStringLiteral("Drop"));

    QJsonObject stop = stepJson(QStringLiteral("Stop"), stopTarget);
    stop[QStringLiteral("fadeTime")] = 250.0;
    stop[QStringLiteral("quantize")] = quantizeJson(
        0, QString(), QStringLiteral("ExitMarker"), QStringLiteral("Immediate"));

    QJsonObject postAction = stepJson(QStringLiteral("PostAction"), action2);
    postAction[QStringLiteral("quantize")] = quantizeJson(
        0, QString(), QStringLiteral("Immediate"), QStringLiteral("Immediate"));

    QJsonObject setValue = stepJson(QStringLiteral("SetVariable"), speedVar);
    setValue[QStringLiteral("value")] = 3.0;

    QJsonObject reset = stepJson(QStringLiteral("Reset"), resetTarget);
    reset[QStringLiteral("resetScope")] = QStringLiteral("Global");

    const QJsonArray ledger{play, stop, postAction, setValue, reset};
    const ActionResult result = setProp(action1, QStringLiteral("steps"), ledger);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();

    const auto& steps = action1->comp<VAction>()->steps;
    ASSERT_EQ(steps.size(), 5u);

    EXPECT_EQ(steps[0].quantize.referenceObjectID, clockRef->objectID);
    EXPECT_EQ(steps[0].quantize.ifSilent, QuantizeIfSilent::Drop);

    EXPECT_EQ(steps[1].quantize.timing, QuantizeTiming::ExitMarker);
    EXPECT_EQ(steps[1].quantize.referenceObjectID, 0u);
    EXPECT_EQ(steps[1].quantize.ifSilent, QuantizeIfSilent::Immediate);
    EXPECT_FLOAT_EQ(steps[1].fadeTime, 250.0f);

    EXPECT_EQ(steps[2].targetObjectID, action2->objectID);

    EXPECT_FLOAT_EQ(steps[3].value, 3.0f);

    EXPECT_EQ(steps[4].resetScope, ResetScope::Global);

    EXPECT_EQ(result.data.value(QStringLiteral("value")).toArray(),
             ActionStepEditing::stepsToJson(steps));
}

TEST_F(ActionTransitionAuthoringScenarioTest,
       StopAcceptsFourMarkerTimingsAndExplicitReference)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* action = manager.createItem(
        nullptr, QStringLiteral("StopTimingAction"), ProjectItem::Type::Action);
    ProjectItem* stopTarget = manager.createItem(
        nullptr, QStringLiteral("StopTimingTarget"), ProjectItem::Type::Sound);
    ProjectItem* clockRef = manager.createItem(
        nullptr, QStringLiteral("StopTimingClock"), ProjectItem::Type::Piece);
    ASSERT_NE(action, nullptr);
    ASSERT_NE(stopTarget, nullptr);
    ASSERT_NE(clockRef, nullptr);

    const std::vector<QString> timings = {
        QStringLiteral("Immediate"), QStringLiteral("EntryMarker"),
        QStringLiteral("ExitMarker"), QStringLiteral("NextMarker")
    };
    for (const QString& timing : timings) {
        QJsonObject stop = stepJson(QStringLiteral("Stop"), stopTarget);
        stop[QStringLiteral("quantize")] = quantizeJson(
            0, QString(), timing, QStringLiteral("Immediate"));
        const ActionResult result = setProp(action, QStringLiteral("steps"),
                                            QJsonArray{stop});
        ASSERT_TRUE(result.success)
            << timing.toStdString() << ": " << result.errorMessage.toStdString();
        EXPECT_EQ(action->comp<VAction>()->steps[0].quantize.referenceObjectID, 0u);
    }

    QJsonObject explicitStop = stepJson(QStringLiteral("Stop"), stopTarget);
    explicitStop[QStringLiteral("quantize")] = quantizeJson(
        static_cast<qint64>(clockRef->objectID), clockRef->name,
        QStringLiteral("NextMarker"), QStringLiteral("Drop"));
    const ActionResult explicitResult = setProp(action, QStringLiteral("steps"),
                                                QJsonArray{explicitStop});
    ASSERT_TRUE(explicitResult.success) << explicitResult.errorMessage.toStdString();
    EXPECT_EQ(action->comp<VAction>()->steps[0].quantize.referenceObjectID,
             clockRef->objectID);
    EXPECT_EQ(action->comp<VAction>()->steps[0].quantize.timing,
             QuantizeTiming::NextMarker);
}

TEST_F(ActionTransitionAuthoringScenarioTest, ActionStepsAndTransitionsSurviveSaveAndReloadCycle)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* action = manager.createItem(
        nullptr, QStringLiteral("PersistedAction"), ProjectItem::Type::Action);
    ProjectItem* playTarget = manager.createItem(
        nullptr, QStringLiteral("PersistedPlayTarget"), ProjectItem::Type::Sound);
    ProjectItem* speedVar = manager.createItem(
        nullptr, QStringLiteral("PersistedSpeedVar"), ProjectItem::Type::Variable);
    ProjectItem* transitionContainer = manager.createItem(
        nullptr, QStringLiteral("PersistedTransitionContainer"),
        ProjectItem::Type::Container);
    ProjectItem* transitionChildA = manager.createItem(
        transitionContainer, QStringLiteral("PersistedChildA"),
        ProjectItem::Type::Sound);
    ProjectItem* transitionChildB = manager.createItem(
        transitionContainer, QStringLiteral("PersistedChildB"),
        ProjectItem::Type::Sound);
    ASSERT_NE(action, nullptr);
    ASSERT_NE(playTarget, nullptr);
    ASSERT_NE(speedVar, nullptr);
    ASSERT_NE(transitionContainer, nullptr);
    ASSERT_NE(transitionChildA, nullptr);
    ASSERT_NE(transitionChildB, nullptr);

    auto* declaration = speedVar->comp<VVariable>();
    declaration->kind = VariableKind::Step;
    declaration->rangeMin = 0.0f;
    declaration->rangeMax = 5.0f;

    QJsonObject play = stepJson(QStringLiteral("Play"), playTarget);
    play[QStringLiteral("quantize")] = quantizeJson(
        0, QString(), QStringLiteral("EntryMarker"), QStringLiteral("Drop"));
    QJsonObject setValue = stepJson(QStringLiteral("SetVariable"), speedVar);
    setValue[QStringLiteral("value")] = 3.0;
    ASSERT_TRUE(setProp(action, QStringLiteral("steps"),
                        QJsonArray{play, setValue}).success);

    TransitionTableDef table = AuthoringPolicy::defaultTransitionTable();
    ASSERT_TRUE(table.addRule(ruleFor(transitionChildA->guid, transitionChildB->guid)));
    ASSERT_TRUE(setProp(transitionContainer, QStringLiteral("transitions"),
                        table.toJson()).success);

    const QUuid actionGuid = action->guid;
    const QUuid playTargetGuid = playTarget->guid;
    const QUuid speedVarGuid = speedVar->guid;
    const QUuid transitionContainerGuid = transitionContainer->guid;
    const QJsonArray expectedSteps = ActionStepEditing::stepsToJson(
        action->comp<VAction>()->steps);
    const TransitionTableDef expectedTable =
        transitionContainer->comp<VSelection>()->transitions;

    const QString projectPath = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.loadProject(projectPath));

    ProjectItem* reloadedAction = manager.findItemByGuid(actionGuid);
    ProjectItem* reloadedPlayTarget = manager.findItemByGuid(playTargetGuid);
    ProjectItem* reloadedSpeedVar = manager.findItemByGuid(speedVarGuid);
    ProjectItem* reloadedContainer = manager.findItemByGuid(transitionContainerGuid);
    ASSERT_NE(reloadedAction, nullptr);
    ASSERT_NE(reloadedPlayTarget, nullptr);
    ASSERT_NE(reloadedSpeedVar, nullptr);
    ASSERT_NE(reloadedContainer, nullptr);
    QJsonArray expectedReloadedSteps = expectedSteps;
    QJsonObject reloadedPlay = expectedReloadedSteps[0].toObject();
    reloadedPlay[QStringLiteral("targetObjectID")] =
        static_cast<qint64>(reloadedPlayTarget->objectID);
    expectedReloadedSteps[0] = reloadedPlay;
    QJsonObject reloadedSetValue = expectedReloadedSteps[1].toObject();
    reloadedSetValue[QStringLiteral("targetObjectID")] =
        static_cast<qint64>(reloadedSpeedVar->objectID);
    expectedReloadedSteps[1] = reloadedSetValue;
    EXPECT_EQ(ActionStepEditing::stepsToJson(reloadedAction->comp<VAction>()->steps),
             expectedReloadedSteps);
    EXPECT_EQ(reloadedContainer->comp<VSelection>()->transitions, expectedTable);
}
