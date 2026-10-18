#include "UnitTest/StudioObjectIdTestSupport.h"
#include "ActionManager.h"
#include "Support/RuntimeIdentityTestUtils.h"
#include "BindingSemantics.h"
#include "Components/VAction.h"
#include "Components/VMixNode.h"
#include "Components/VPiece.h"
#include "Components/VSelection.h"
#include "Components/VSound.h"
#include "Components/VSpatial.h"
#include "Components/VTrack.h"
#include "Components/VVariable.h"
#include "ContainerBehaviorSemantics.h"
#include "GridSemantics.h"
#include "ItemTypeCatalog.h"
#include "PackBuildPlan.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectManager.h"
#include "ReferenceSlotResolution.h"
#include "RuntimeCapabilityMatrix.h"
#include "SoundSourceSemantics.h"
#include "TransitionSemantics.h"
#include "UndoCommands.h"
#include "WorkspaceWindowTypes.h"
#include "Widgets/Inspector/ActionInspectorSchema.h"
#include "Widgets/Inspector/AttenuationInspectorSchema.h"
#include "Widgets/Inspector/BusInspectorSchema.h"
#include "Widgets/Inspector/ContainerInspectorSchema.h"
#include "Widgets/Inspector/InspectorPanel_p.h"
#include "Widgets/Inspector/LayerInspectorSchema.h"
#include "Widgets/Inspector/PieceInspectorSchema.h"
#include "Widgets/Inspector/SoundInspectorSchema.h"
#include "Widgets/Inspector/TrackInspectorSchema.h"
#include "Widgets/Inspector/VariableInspectorSchema.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QTemporaryDir>

#include <algorithm>
#include <array>
#include <set>
#include "Support/ProjectPlatformTestUtils.h"

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

ActionResult setProperty(ProjectItem* item, const QString& property,
                         const QJsonValue& value)
{
    return ActionManager::instance().execute(
        QStringLiteral("set_item_property"),
        {{QStringLiteral("authoring_handle"), static_cast<qint64>(item->objectID)},
         {QStringLiteral("property"), property},
         {QStringLiteral("value"), value}}, ActionSource::GUI);
}

QList<uint32_t> childIds(const ProjectItem& parent)
{
    QList<uint32_t> result;
    for (const auto& row : parent.comp<VNX::VSelection>()->selections.shared.occurrenceRows())
        result.push_back(row.stableOccurrenceOrdinal);
    return result;
}

uint32_t occurrenceOrdinal(const ProjectItem& parent, const QUuid& childGuid)
{
    const auto locator = PlayablePropertyAuthoringV1::containerChildLocator(
        parent, childGuid);
    return locator ? locator->stableOccurrenceOrdinal : 0;
}

bool schemaHas(const PropertySchema& schema, const QString& field)
{
    return std::any_of(schema.begin(), schema.end(), [&](const auto& descriptor) {
        return descriptor.field == field;
    });
}

Clip clip(ProjectItem& target, int64_t start, int64_t duration)
{
    Clip value;
    value.clipGuid = QUuid::createUuid();
    value.targetGuid = target.guid;
    value.start = start;
    value.duration = duration;
    return value;
}

class EditorWorkflowScenarioTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(), &manager, nullptr, {});
        ASSERT_TRUE(temp_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("EditorWorkflowScenario");
        config.location = temp_.path();
        ASSERT_TRUE(manager.createProject(config));
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    ProjectItem* create(ProjectItem* parent, const QString& name,
                        ProjectItem::Type type, const QString& bucket = {})
    {
        ProjectItem* item = ProjectManager::instance().createItem(
            parent, name, type, {}, bucket);
        EXPECT_NE(item, nullptr) << name.toStdString();
        return item;
    }

    QTemporaryDir temp_;
};

}

TEST_F(EditorWorkflowScenarioTest,
       FamiliesFiveBucketsGuidIdentityAndWidgetStructureSurviveReopen)
{
    EXPECT_EQ(ItemTypeCatalog::allTypes().size(), 9u);
    EXPECT_EQ(ItemTypeCatalog::allBuckets().size(), 5u);
    EXPECT_FALSE(ItemTypeCatalog::parseTypeName(
        QStringLiteral("AudioLayer"), Qt::CaseInsensitive, true));

    const std::array<std::pair<ProjectItem::Type, InspectorPanelPrivate::Page>, 10> pages{{
        {ProjectItem::Type::None, InspectorPanelPrivate::PageEmpty},
        {ProjectItem::Type::Sound, InspectorPanelPrivate::PageSound},
        {ProjectItem::Type::Container, InspectorPanelPrivate::PageContainer},
        {ProjectItem::Type::Track, InspectorPanelPrivate::PageTrack},
        {ProjectItem::Type::Piece, InspectorPanelPrivate::PagePiece},
        {ProjectItem::Type::Layer, InspectorPanelPrivate::PageLayer},
        {ProjectItem::Type::Bus, InspectorPanelPrivate::PageBus},
        {ProjectItem::Type::Action, InspectorPanelPrivate::PageAction},
        {ProjectItem::Type::Variable, InspectorPanelPrivate::PageVariable},
        {ProjectItem::Type::Attenuation, InspectorPanelPrivate::PageAttenuation},
    }};
    for (const auto& [type, page] : pages)
        EXPECT_EQ(pageForItemType(type), page);

    const WorkspaceWindowCatalog windows = defaultWorkspaceWindowCatalog();
    for (const QString& id : {QStringLiteral("Explorer"),
                              QStringLiteral("BlueprintCanvas"),
                              QStringLiteral("ExportPacksManager")})
        ASSERT_NE(windows.find(id), nullptr) << id.toStdString();

    auto& manager = ProjectManager::instance();
    ProjectItem* layer = create(nullptr, QStringLiteral("World"),
                                ProjectItem::Type::Layer);
    ProjectItem* container = create(layer, QStringLiteral("Weather"),
                                    ProjectItem::Type::Container);
    ProjectItem* sound = create(container, QStringLiteral("Rain"),
                                ProjectItem::Type::Sound);
    ProjectItem* track = create(layer, QStringLiteral("AmbienceTrack"),
                                ProjectItem::Type::Track);
    ProjectItem* piece = create(layer, QStringLiteral("AmbiencePiece"),
                                ProjectItem::Type::Piece);
    ProjectItem* nestedLayer = create(layer, QStringLiteral("Interior"),
                                      ProjectItem::Type::Layer);
    ProjectItem* bus = create(manager.getMasterBusItem(), QStringLiteral("WeatherBus"),
                              ProjectItem::Type::Bus);
    auto* actionFolder = manager.createFolder(
        nullptr, QStringLiteral("WeatherActions"), QStringLiteral("Actions"));
    ASSERT_NE(actionFolder, nullptr);
    ProjectItem* action = ProjectManager::instance().createItem(
        nullptr, QStringLiteral("PlayWeather"), ProjectItem::Type::Action,
        QStringLiteral("WeatherActions"));
    EXPECT_NE(action, nullptr);
    ProjectItem* variable = create(nullptr, QStringLiteral("WeatherState"),
                                   ProjectItem::Type::Variable,
                                   QStringLiteral("Variables"));
    ProjectItem* attenuation = create(nullptr, QStringLiteral("WeatherDistance"),
                                      ProjectItem::Type::Attenuation,
                                      QStringLiteral("Attenuations"));
    ASSERT_NE(sound, nullptr);
    const std::array<ProjectItem*, 9> objects{
        sound, container, track, piece, nestedLayer,
        bus, action, variable, attenuation,
    };
    QHash<QUuid, ProjectItem::Type> identities;
    for (ProjectItem* item : objects) {
        ASSERT_NE(item, nullptr);
        identities.insert(item->guid, item->type);
    }
    EXPECT_EQ(identities.size(), 9);
    EXPECT_EQ(ItemTypeCatalog::bucketOf(sound->type), ItemTypeCatalog::Bucket::Layers);
    EXPECT_EQ(ItemTypeCatalog::bucketOf(bus->type), ItemTypeCatalog::Bucket::Busses);
    EXPECT_EQ(ItemTypeCatalog::bucketOf(action->type), ItemTypeCatalog::Bucket::Actions);
    EXPECT_EQ(ItemTypeCatalog::bucketOf(variable->type), ItemTypeCatalog::Bucket::Variables);
    EXPECT_EQ(ItemTypeCatalog::bucketOf(attenuation->type),
              ItemTypeCatalog::Bucket::Attenuations);
    ASSERT_TRUE(setProperty(sound, QStringLiteral("notes"),
                            QStringLiteral("Panorama note")).success);
    EXPECT_EQ(sound->notes, QStringLiteral("Panorama note"));
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_TRUE(sound->notes.isEmpty());
    UndoRedoManager::instance().undoStack()->redo();
    EXPECT_EQ(sound->notes, QStringLiteral("Panorama note"));
    const QUuid guid = sound->guid;
    const std::array<std::pair<QUuid, ProjectItem::Type>, 5> persistedIdentities{{
        {sound->guid, sound->type},
        {container->guid, container->type},
        {track->guid, track->type},
        {piece->guid, piece->type},
        {nestedLayer->guid, nestedLayer->type},
    }};
    const QString path = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.loadProject(path));
    for (const auto& [persistedGuid, persistedType] : persistedIdentities) {
        ProjectItem* item = manager.findItemByGuid(persistedGuid);
        ASSERT_NE(item, nullptr);
        EXPECT_EQ(item->type, persistedType);
    }
    ProjectItem* reopened = manager.findItemByGuid(guid);
    ASSERT_NE(reopened, nullptr);
    EXPECT_NE(reopened->runtimeId(), harkentest::StudioObjectId(0u));
    EXPECT_EQ(manager.findItemByRuntimeStableId(reopened->runtimeId()), reopened);
    EXPECT_EQ(reopened->parent->type, ProjectItem::Type::Container);
    EXPECT_EQ(reopened->notes, QStringLiteral("Panorama note"));
}

TEST_F(EditorWorkflowScenarioTest,
       RoutingAttenuationEffectsAndBlueprintUseOneAuthoredState)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* layer = create(nullptr, QStringLiteral("World"), ProjectItem::Type::Layer);
    ProjectItem* sound = create(layer, QStringLiteral("Footsteps"), ProjectItem::Type::Sound);
    ProjectItem* bus = manager.getMasterBusItem();
    ASSERT_NE(bus, nullptr);
    ProjectItem* reverb = create(bus, QStringLiteral("Reverb"),
                                 ProjectItem::Type::Bus);
    ProjectItem* attenuation = create(nullptr, QStringLiteral("NearField"),
                                      ProjectItem::Type::Attenuation,
                                      QStringLiteral("Attenuations"));
    ASSERT_NE(attenuation, nullptr);
    auto* mix = sound->comp<VMixNode>();
    auto* spatial = sound->comp<VSpatial>();
    ASSERT_NE(mix, nullptr);
    ASSERT_NE(spatial, nullptr);
    auto& actions = ActionManager::instance();
    QJsonObject edit{{"definition_guid", sound->guid.toString(QUuid::WithoutBraces)},
                     {"scope", "Shared"}, {"mode", "Local"}};
    for (const auto* group : {"mainOutput", "send", "spatialRating"}) {
        edit.insert("group", QLatin1String(group));
        ASSERT_TRUE(actions.execute("set_field_group_mode", edit).success);
    }
    edit.insert("group", "mainOutput");
    edit.insert("field", "outputBusVolume");
    edit.insert("value", -3.0);
    ASSERT_TRUE(actions.execute("set_field_group_field", edit).success);
    edit.insert("group", "send");
    edit.insert("sends", QJsonArray{QJsonObject{
        {"targetBusID", reverb->guid.toString(QUuid::WithoutBraces)}, {"sendLevel", -12.0}}});
    ASSERT_TRUE(actions.execute("replace_field_group_sends", edit).success);
    edit.insert("group", "spatialRating");
    edit.insert("field", "spatial.attenuationRef");
    edit.insert("value", attenuation->guid.toString(QUuid::WithoutBraces));
    ASSERT_TRUE(actions.execute("set_field_group_field", edit).success);
    edit.insert("field", "spatial.is3DEnabled");
    edit.insert("value", true);
    ASSERT_TRUE(actions.execute("set_field_group_field", edit).success);
    EffectEntry effect;
    effect.guid = QUuid::createUuid();
    effect.type = EffectType::Gain;
    effect.parameters = EffectChainSemantics::defaultParameters(effect.type);
    ASSERT_TRUE(mix->effects.replace({effect}));
    const auto localEffects = mix->effects;
    const auto localGroups = sound->fieldGroups;

    const SignalChainView view = manager.buildSignalChainView(sound);
    EXPECT_EQ(view.self.objectID, sound->objectID);
    EXPECT_EQ(view.busSlotOwnerObjectID, sound->objectID);
    ASSERT_EQ(view.slotSources.size(), 3u);
    for (const auto& source : view.slotSources) {
        EXPECT_EQ(source.ownerObjectID, sound->objectID);
        EXPECT_EQ(source.source, QStringLiteral("self"));
    }
    ASSERT_FALSE(view.busChain.empty());
    EXPECT_EQ(view.busChain.front().busID, bus->objectID);
    EXPECT_TRUE(schemaHas(soundInspectorSchema(), QStringLiteral("sends")));
    EXPECT_EQ(mix->outputBusId, bus->objectID);
    EXPECT_FLOAT_EQ(mix->outputBusVolume, -3.0f);
    ASSERT_EQ(mix->sends.size(), 1u);
    EXPECT_EQ(mix->sends.front().targetBusID, reverb->objectID);
    EXPECT_FLOAT_EQ(mix->sends.front().sendLevel, VMixNode::dbToLinear(-12.0f));
    EXPECT_EQ(mix->effects.entries().front().type, EffectType::Gain);
    EXPECT_EQ(spatial->attenuationRef, attenuation->runtimeId());
    EXPECT_TRUE(spatial->is3DEnabled);
    EXPECT_EQ(sound->playablePropertyPresence.bits & kFieldGroupPropertyMask, 0u);

    auto* undo = UndoRedoManager::instance().undoStack();
    undo->clear();
    edit.insert("mode", "Inherit");
    for (const auto* group : {"mainOutput", "send", "spatialRating"}) {
        edit.insert("group", QLatin1String(group));
        ASSERT_TRUE(actions.execute("set_field_group_mode", edit).success);
    }
    const SignalChainView inherited = manager.buildSignalChainView(sound);
    EXPECT_EQ(inherited.busSlotOwnerObjectID, layer->objectID);
    ASSERT_EQ(inherited.slotSources.size(), 3u);
    for (const auto& source : inherited.slotSources) {
        EXPECT_EQ(source.ownerObjectID, layer->objectID);
        EXPECT_EQ(source.source, QStringLiteral("organization"));
    }
    EXPECT_FLOAT_EQ(mix->outputBusVolume, 0.0f);
    EXPECT_TRUE(mix->sends.empty());
    EXPECT_EQ(spatial->attenuationRef, harkentest::StudioObjectId(0u));
    EXPECT_FALSE(spatial->is3DEnabled);
    EXPECT_EQ(mix->effects, localEffects);
    ASSERT_EQ(undo->count(), 3);
    while (undo->canUndo()) undo->undo();
    EXPECT_EQ(sound->fieldGroups, localGroups);
    EXPECT_EQ(manager.buildSignalChainView(sound).busSlotOwnerObjectID, sound->objectID);
    EXPECT_EQ(spatial->attenuationRef, attenuation->runtimeId());
    EXPECT_EQ(mix->effects, localEffects);
}

TEST_F(EditorWorkflowScenarioTest,
       RandomDrawUsesFrequencyWeightsAvoidRepeatAndPerEmitterMemory)
{
    ProjectItem* container = create(nullptr, QStringLiteral("Asphalt"),
                                    ProjectItem::Type::Container);
    ProjectItem* first = create(container, QStringLiteral("A"), ProjectItem::Type::Sound);
    ProjectItem* second = create(container, QStringLiteral("B"), ProjectItem::Type::Sound);
    ASSERT_NE(second, nullptr);
    auto* selection = container->comp<VNX::VSelection>();
    selection->mode = SelectionMode::Random;
    selection->behavior.discipline = RandomDiscipline::Draw;
    selection->behavior.avoidRepeat = 1;
    selection->behavior.memoryScope = SelectionMemoryScope::PerEmitter;
    selection->selections.shared.find(occurrenceOrdinal(*container, first->guid))->weight = 1;
    selection->selections.shared.find(occurrenceOrdinal(*container, second->guid))->weight = 100;
    EXPECT_EQ(ContainerBehaviorSemantics::childWeightLabel(*selection),
              QStringLiteral("Frequency"));
    EXPECT_EQ(ContainerBehaviorSemantics::effectiveChildWeight(
                  *selection, occurrenceOrdinal(*container, first->guid)),
              1);
    EXPECT_EQ(ContainerBehaviorSemantics::effectiveChildWeight(
                  *selection, occurrenceOrdinal(*container, second->guid)),
              100);
    const QList<uint32_t> ids = childIds(*container);
    ContainerBehaviorSemantics::Memory memoryA;
    ContainerBehaviorSemantics::Memory memoryB;
    const auto a1 = ContainerBehaviorSemantics::advance(*selection, ids, 17, memoryA);
    const auto a2 = ContainerBehaviorSemantics::advance(*selection, ids, 17, memoryA);
    const auto b1 = ContainerBehaviorSemantics::advance(*selection, ids, 17, memoryB);
    ASSERT_TRUE(a1.child);
    ASSERT_TRUE(a2.child);
    ASSERT_TRUE(b1.child);
    EXPECT_EQ(*b1.child, *a1.child);
    EXPECT_NE(*a2.child, *a1.child);
    EXPECT_EQ(memoryA.history.back(), *a2.child);
    EXPECT_EQ(memoryB.history.back(), *b1.child);
}

TEST_F(EditorWorkflowScenarioTest,
       RandomShuffleUsesOrderWeightsWithoutReplacementAcrossRounds)
{
    ProjectItem* container = create(nullptr, QStringLiteral("EnemyBarks"),
                                    ProjectItem::Type::Container);
    for (int index = 0; index < 4; ++index)
        create(container, QStringLiteral("Bark%1").arg(index), ProjectItem::Type::Sound);
    auto* selection = container->comp<VNX::VSelection>();
    selection->mode = SelectionMode::Random;
    selection->behavior.discipline = RandomDiscipline::Shuffle;
    selection->behavior.avoidRepeat = 1;
    ContainerBehaviorSemantics::Memory memory;
    const QList<uint32_t> ids = childIds(*container);
    for (int index = 0; index < ids.size(); ++index)
        selection->selections.shared.find(ids[index])->weight = 10 + index * 20;
    EXPECT_EQ(ContainerBehaviorSemantics::childWeightLabel(*selection),
              QStringLiteral("Order"));
    EXPECT_EQ(ContainerBehaviorSemantics::effectiveChildWeight(*selection, ids.back()), 70);
    std::vector<uint32_t> round;
    for (int index = 0; index < 4; ++index) {
        auto step = ContainerBehaviorSemantics::advance(*selection, ids, 31 + index, memory);
        ASSERT_TRUE(step.child);
        round.push_back(*step.child);
    }
    EXPECT_EQ(std::set<uint32_t>(round.begin(), round.end()).size(), 4u);
    auto next = ContainerBehaviorSemantics::advance(*selection, ids, 47, memory);
    ASSERT_TRUE(next.child);
    EXPECT_NE(round.back(), *next.child);
}

TEST_F(EditorWorkflowScenarioTest,
       SequenceUsesGlobalCursorAndActionResetIsReady)
{
    ProjectItem* container = create(nullptr, QStringLiteral("ComboHits"),
                                    ProjectItem::Type::Container);
    for (int index = 0; index < 3; ++index)
        create(container, QStringLiteral("Hit%1").arg(index), ProjectItem::Type::Sound);
    auto* selection = container->comp<VNX::VSelection>();
    selection->mode = SelectionMode::Sequence;
    selection->behavior.memoryScope = SelectionMemoryScope::Global;
    const QList<uint32_t> ids = childIds(*container);
    ContainerBehaviorSemantics::Memory memory;
    EXPECT_EQ(*ContainerBehaviorSemantics::advance(*selection, ids, 0, memory).child, ids[0]);
    EXPECT_EQ(*ContainerBehaviorSemantics::advance(*selection, ids, 0, memory).child, ids[1]);
    const auto gate = RuntimeCapabilityMatrix::query(
        ProjectItem::Type::Action, RuntimeCapabilityOperation::ActionRegistration,
        RuntimeCapabilityFeature::ActionReset);
    ASSERT_TRUE(gate);
    EXPECT_EQ(gate->state, RuntimeCapabilityState::Ready);
}

TEST_F(EditorWorkflowScenarioTest,
       RandomQueueWrapsWithOrderWeightsAndResumeBoundaryVisible)
{
    ProjectItem* container = create(nullptr, QStringLiteral("DebrisScatter"),
                                    ProjectItem::Type::Container);
    for (int index = 0; index < 3; ++index)
        create(container, QStringLiteral("Debris%1").arg(index), ProjectItem::Type::Sound);
    auto* selection = container->comp<VNX::VSelection>();
    selection->mode = SelectionMode::Random;
    selection->playType = SelectionPlayType::Queue;
    selection->behavior.resumeFromLast = true;
    const auto visibility = ContainerBehaviorSemantics::visibilityFor(*selection);
    EXPECT_TRUE(visibility.childWeights);
    EXPECT_FALSE(visibility.memoryScope);
    EXPECT_TRUE(visibility.resumeFromLast);
    ContainerBehaviorSemantics::Memory memory;
    const QList<uint32_t> ids = childIds(*container);
    for (int index = 0; index < ids.size(); ++index)
        selection->selections.shared.find(ids[index])->weight = 20 + index * 20;
    EXPECT_EQ(ContainerBehaviorSemantics::childWeightLabel(*selection),
              QStringLiteral("Order"));
    EXPECT_EQ(ContainerBehaviorSemantics::effectiveChildWeight(*selection, ids.back()), 60);
    for (int index = 0; index < 4; ++index) {
        const auto step = ContainerBehaviorSemantics::advance(
            *selection, ids, 100 + index, memory);
        ASSERT_TRUE(step.child);
    }
    EXPECT_EQ(memory.cursor, 1);
}

TEST_F(EditorWorkflowScenarioTest,
       SequenceQueuePersistsResumeSpliceMarkersAndNestedPieceStructure)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* container = create(nullptr, QStringLiteral("MusicSet"),
                                    ProjectItem::Type::Container);
    ProjectItem* piece = create(container, QStringLiteral("MusicPiece"),
                                ProjectItem::Type::Piece);
    ProjectItem* track = create(piece, QStringLiteral("MusicTrack"),
                                ProjectItem::Type::Track);
    ASSERT_NE(track, nullptr);
    auto* selection = container->comp<VNX::VSelection>();
    selection->mode = SelectionMode::Sequence;
    selection->playType = SelectionPlayType::Queue;
    selection->behavior.resumeFromLast = true;
    selection->spliceByMarker = true;
    auto* timeline = piece->comp<VPiece>();
    timeline->pieceTime = PieceTimeSpec::musical();
    timeline->markers.items = {
        {QUuid::createUuid(), MarkerKind::Entry, QStringLiteral("Intro"), 0},
        {QUuid::createUuid(), MarkerKind::Custom, QStringLiteral("Loop"), 960},
        {QUuid::createUuid(), MarkerKind::Exit, QStringLiteral("Outro"), 1920},
    };
    for (Marker& marker : timeline->markers.items) {
        ASSERT_NE(marker.runtimeId(), harkentest::StudioObjectId(0u));
    }
    EXPECT_TRUE(timeline->markers.validate());
    const QUuid guid = container->guid;
    const QString path = manager.currentProject()->projectPath;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.loadProject(path));
    auto* reopened = manager.findItemByGuid(guid)->comp<VNX::VSelection>();
    EXPECT_TRUE(reopened->behavior.resumeFromLast);
    EXPECT_TRUE(reopened->spliceByMarker);
}

TEST_F(EditorWorkflowScenarioTest,
       DiscreteOnTriggerGridHasUniqueWinnerAndExplicitMiss)
{
    DiscreteGridDef grid;
    const QUuid axis = QUuid::createUuid();
    ASSERT_TRUE(grid.addAxis(axis));
    ASSERT_TRUE(grid.addRow(QUuid::createUuid()));
    ASSERT_TRUE(grid.addRow(QUuid::createUuid()));
    ASSERT_TRUE(grid.setCell(0, 0, 1));
    ASSERT_TRUE(grid.setCell(1, 0, 2));
    EXPECT_EQ(GridSemantics::evalDiscreteGrid(grid, {2.0f}).winner, 1);
    EXPECT_EQ(GridSemantics::evalDiscreteGrid(grid, {3.0f}).winner, -1);
}

TEST_F(EditorWorkflowScenarioTest,
       ContinuousGridOverlapsFadeAndOwnsBoundaryDeterministically)
{
    MixingChildDef left{QUuid::createUuid(), {{0.0f, 1.0f}, {5.0f, 1.0f}, {10.0f, 0.0f}}, {}};
    MixingChildDef right{QUuid::createUuid(), {{0.0f, 0.0f}, {5.0f, 0.0f}, {10.0f, 1.0f}}, {}};
    left.normalize();
    right.normalize();
    const MixingDef mixing{{{QUuid::createUuid(), {left, right}}}};
    const auto overlap = MixingSemantics::evaluate(mixing,
        [](const QUuid&) -> std::optional<float> { return 7.5f; });
    ASSERT_TRUE(overlap);
    ASSERT_EQ(overlap->size(), 2);
    EXPECT_GT(overlap->value(left.target), 0.0f);
    EXPECT_GT(overlap->value(right.target), 0.0f);
    EXPECT_FLOAT_EQ(left.gainAt(0.0f), 1.0f);

}

TEST_F(EditorWorkflowScenarioTest,
       MultiAxisGridUsesAxisOrderSpecificityAndMountEligibility)
{
    DiscreteGridDef grid;
    ASSERT_TRUE(grid.addAxis(QUuid::createUuid()));
    ASSERT_TRUE(grid.addAxis(QUuid::createUuid()));
    ASSERT_TRUE(grid.addRow(QUuid::createUuid()));
    ASSERT_TRUE(grid.addRow(QUuid::createUuid()));
    ASSERT_TRUE(grid.setCell(0, 1, 2));
    ASSERT_TRUE(grid.setCell(1, 0, 1));
    const auto winner = GridSemantics::evalDiscreteGrid(grid, {1.0f, 2.0f});
    EXPECT_EQ(winner.winner, 1);
    EXPECT_EQ(winner.hits.size(), 2u);
    EXPECT_EQ(GridSemantics::gridMountState(grid, VariableResponse::AtStart),
              GridSemantics::GridMountState::Idle);
    ASSERT_TRUE(grid.moveAxis(0, 1));
    EXPECT_EQ(GridSemantics::evalDiscreteGrid(grid, {2.0f, 1.0f}).winner, 0);
    EXPECT_EQ(GridSemantics::gridMountState(grid, VariableResponse::Live),
              GridSemantics::GridMountState::Editable);
}

TEST_F(EditorWorkflowScenarioTest,
       TrackAndPieceKeepAbsoluteMusicalLedgersMarkersOverlapAndOffset)
{
    ProjectItem* piece = create(nullptr, QStringLiteral("StormScene"),
                                ProjectItem::Type::Piece);
    ProjectItem* firstTrack = create(piece, QStringLiteral("Stealth"),
                                     ProjectItem::Type::Track);
    ProjectItem* secondTrack = create(piece, QStringLiteral("HighIntensity"),
                                      ProjectItem::Type::Track);
    ProjectItem* layer = create(nullptr, QStringLiteral("StormContent"),
                                ProjectItem::Type::Layer);
    ProjectItem* sound = create(layer, QStringLiteral("Wind"),
                                ProjectItem::Type::Sound);
    auto* pieceTimeline = piece->comp<VPiece>();
    auto* firstTimeline = firstTrack->comp<VTrack>();
    auto* secondTimeline = secondTrack->comp<VTrack>();
    pieceTimeline->pieceTime = PieceTimeSpec::musical();
    pieceTimeline->markers.items = {
        {QUuid::createUuid(), MarkerKind::Entry, QString(), -480},
        {QUuid::createUuid(), MarkerKind::Exit, QString(), 1920},
    };
    Clip firstClip = clip(*sound, -480, 960);
    Clip secondClip = clip(*sound, -480, 1920);
    ASSERT_TRUE(firstTimeline->clips.mintOccurrenceOrdinal(&firstClip));
    ASSERT_TRUE(secondTimeline->clips.mintOccurrenceOrdinal(&secondClip));
    firstTimeline->clips.items = {firstClip};
    secondTimeline->clips.items = {secondClip};
    EXPECT_EQ(pieceTimeline->pieceTime.domain, TimelineDomain::Musical);
    EXPECT_TRUE(pieceTimeline->markers.validate());
    EXPECT_TRUE(firstTimeline->clips.validateStructure());
    EXPECT_EQ(firstTimeline->clips.items.front().start,
              secondTimeline->clips.items.front().start);
}

TEST_F(EditorWorkflowScenarioTest,
       TransitionSpecificityBridgeSamePositionWatchAndRuntimeGateStayExplicit)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* container = create(nullptr, QStringLiteral("CombatMusic"),
                                    ProjectItem::Type::Container);
    ProjectItem* source = create(container, QStringLiteral("Source"),
                                 ProjectItem::Type::Piece);
    ProjectItem* target = create(container, QStringLiteral("Target"),
                                 ProjectItem::Type::Piece);
    ProjectItem* bridge = create(container, QStringLiteral("DrumFill"),
                                 ProjectItem::Type::Piece);
    auto* selection = container->comp<VNX::VSelection>();
    selection->mode = SelectionMode::Sequence;
    selection->playType = SelectionPlayType::Queue;
    source->comp<VPiece>()->pieceTime = PieceTimeSpec::musical();
    target->comp<VPiece>()->pieceTime = PieceTimeSpec::musical();
    source->comp<VPiece>()->markers.items = {
        {QUuid::createUuid(), MarkerKind::Entry, {}, -960},
        {QUuid::createUuid(), MarkerKind::Exit, {}, 7680},
    };
    target->comp<VPiece>()->markers.items = {
        {QUuid::createUuid(), MarkerKind::Entry, {}, 480},
        {QUuid::createUuid(), MarkerKind::Exit, {}, 4320},
    };
    for (ProjectItem* piece : {source, target}) {
        for (Marker& marker : piece->comp<VPiece>()->markers.items) {
            ASSERT_NE(marker.runtimeId(), harkentest::StudioObjectId(0u));
        }
    }
    TransitionRuleDef rule;
    rule.source = source->guid;
    rule.target = target->guid;
    rule.bridge = bridge->guid;
    rule.entryMode = TransitionEntryMode::EntryMarker;
    ASSERT_TRUE(selection->transitions.addRule(rule));
    EXPECT_EQ(TransitionSemantics::matchRule(
                  selection->transitions, source->guid, target->guid),
              &selection->transitions.rules.back());
    EXPECT_GT(TransitionSemantics::ruleSpecificity(rule), 0);
    EXPECT_EQ(TransitionSemantics::transitionMountState(*container),
              TransitionMountState::Editable);
    const TimelineItemResolver resolve = [&manager](const QUuid& guid) {
        return manager.findItemByGuid(guid);
    };
    const auto mapped = TransitionSemantics::samePositionMap(
        7200, *source, *target, resolve);
    ASSERT_TRUE(mapped) << mapped.validation.message.toStdString();
    EXPECT_EQ(mapped.targetPosition, 960);
    const auto watch = TransitionSemantics::resolveWatchReference(
        source, *container, nullptr, resolve);
    ASSERT_TRUE(watch) << watch.failureReason.toStdString();
    EXPECT_EQ(watch.referencePiece, source);
    EXPECT_EQ(watch.direction, TransitionSemantics::TransitionWatchDirection::Down);
    const auto gate = RuntimeCapabilityMatrix::evaluate(
        *container, *manager.currentProject(),
        RuntimeCapabilityOperation::LiveSync,
        RuntimeCapabilityFeature::Transition);
    EXPECT_EQ(gate.state, RuntimeCapabilityState::UnavailableEngineContract);
}

TEST_F(EditorWorkflowScenarioTest,
       BindingContributionsStepSmoothCurveAndVariableIdentityShareOneLedger)
{
    auto& manager = ProjectManager::instance();
    ProjectItem* layer = create(nullptr, QStringLiteral("Music"), ProjectItem::Type::Layer);
    ProjectItem* sound = create(layer, QStringLiteral("WindLayers"), ProjectItem::Type::Sound);
    ProjectItem* variable = create(nullptr, QStringLiteral("Intensity"),
                                   ProjectItem::Type::Variable,
                                   QStringLiteral("Variables"));
    auto* declaration = variable->comp<VVariable>();
    declaration->kind = VariableKind::Smooth;
    declaration->rangeMin = 0.0f;
    declaration->rangeMax = 1.0f;
    ParameterBindingDef layerBinding = BindingSemantics::defaultBinding(
        BindingSemantics::volumeAddress(), variable->guid);
    layerBinding.guid = QUuid::createUuid();
    layerBinding.outputMin = -24.0f;
    layerBinding.outputMax = 0.0f;
    ASSERT_TRUE(layer->comp<VMixNode>()->bindings.upsert(layerBinding));
    ParameterBindingDef own = BindingSemantics::defaultBinding(
        BindingSemantics::volumeAddress(), variable->guid);
    own.guid = QUuid::createUuid();
    own.outputMin = -24.0f;
    own.outputMax = 0.0f;
    own.curve.front().value = -24.0f;
    own.curve.insert(1, {0.5f, -18.0f});
    own.handles.clear();
    ASSERT_TRUE(sound->comp<VMixNode>()->bindings.upsert(own));
    const harkenstudio::ObjectId stableId = variable->runtimeId();
    const auto resolved = ReferenceSlotResolution::resolveParameterBindings(
        sound, BindingSemantics::volumeAddress());
    ASSERT_EQ(resolved.size(), 2u);
    EXPECT_EQ(resolved[0].owner, sound);
    EXPECT_EQ(resolved[1].owner, layer);
    const float curved = BindingSemantics::contribution(
        *resolved[0].binding, VariableKind::Smooth, 0.5f, 0.0f, 1.0f);
    const float step = BindingSemantics::contribution(
        *resolved[1].binding, VariableKind::Step, 0.5f, 0.0f, 1.0f);
    EXPECT_FLOAT_EQ(curved, -18.0f);
    EXPECT_FLOAT_EQ(step, -24.0f);
    EXPECT_EQ(layerBinding.runtimeId(),
              layer->comp<VMixNode>()->bindings.entries().front().runtimeId());
    EXPECT_EQ(layer->comp<VMixNode>()->bindings.size(), 1u);
    EXPECT_EQ(sound->comp<VMixNode>()->bindings.size(), 1u);
    variable->name = QStringLiteral("IntensityRenamed");
    EXPECT_EQ(variable->runtimeId(), stableId);
}

TEST_F(EditorWorkflowScenarioTest,
       ActionSourcesPackVariantsAndRuntimeVerdictsFormOneBoundary)
{
    auto& manager = ProjectManager::instance();
    ProjectData* project = manager.currentProject();
    project->platforms = HarkenTest::platforms({QStringLiteral("Windows")});
    manager.setPackExportCurrentPlatformInternal(project->platformIds().front());
    manager.setPackExportSelectedPlatformsInternal(project->platformIds());
    project->projectLanguages = {QStringLiteral("English"), QStringLiteral("Japanese")};
    ProjectItem* layer = create(nullptr, QStringLiteral("Callouts"), ProjectItem::Type::Layer);
    ProjectItem* sound = create(layer, QStringLiteral("Reload"), ProjectItem::Type::Sound);
    ProjectItem* variable = create(nullptr, QStringLiteral("Urgency"),
                                   ProjectItem::Type::Variable,
                                   QStringLiteral("Variables"));
    ProjectItem* followup = create(nullptr, QStringLiteral("Followup"),
                                   ProjectItem::Type::Action,
                                   QStringLiteral("Actions"));
    ProjectItem* action = create(nullptr, QStringLiteral("CalloutReload"),
                                 ProjectItem::Type::Action,
                                 QStringLiteral("Actions"));
    auto* soundData = sound->comp<VSound>();
    soundData->distinguishLanguage = true;
    for (const QString& language : project->projectLanguages) {
        SoundSourceDef source;
        source.entryId = QUuid::createUuid();
        source.sourceId = static_cast<uint32_t>(100 + soundData->sources.localized.size());
        soundData->sources.localized[language].entries.push_back(source);
        soundData->sources.localized[language].platforms[HarkenTest::platformIdString(QStringLiteral("Windows"))].active = source.entryId;
    }
    for (const QString& language : project->projectLanguages) {
        const auto resolved = SoundSourceSemantics::resolveSourceSelection(
            *soundData, HarkenTest::platformIdString(QStringLiteral("Windows")), language);
        ASSERT_TRUE(resolved) << language.toStdString();
        EXPECT_EQ(resolved.source->entryId, soundData->sources.localized[language].entries[0].entryId);
    }

    auto step = [](VActionStep::Type type, ProjectItem* target) {
        VActionStep value;
        value.type = type;
        value.targetObjectID = target->objectID;
        value.targetName = target->name;
        return value;
    };
    action->comp<VAction>()->steps = {
        step(VActionStep::Type::Play, sound),
        step(VActionStep::Type::Stop, sound),
        step(VActionStep::Type::PostAction, followup),
        step(VActionStep::Type::SetVariable, variable),
        step(VActionStep::Type::Reset, layer),
    };
    action->comp<VAction>()->steps.front().quantize.timing = QuantizeTiming::NextMarker;
    const PackBuildPlan plan = PackBuildPlanning::buildForAction(
        *project, action->runtimeId());
    EXPECT_EQ(plan.variants.size(), 2u);
    for (const auto& variant : plan.variants) {
        EXPECT_EQ(variant.actionStableId, action->runtimeId());
        EXPECT_FALSE(variant.artifactName.isEmpty());
        EXPECT_EQ(variant.capability.state, RuntimeCapabilityState::InvalidAuthoring);
    }
    const auto assessment = RuntimeCapabilityMatrix::assess(
        *action, *project, RuntimeCapabilityOperation::ActionRegistration);
    EXPECT_EQ(assessment.rootStableId, action->runtimeId());
    EXPECT_FALSE(assessment.permitted());
    EXPECT_FALSE(std::any_of(assessment.issues.begin(), assessment.issues.end(),
        [](const auto& issue) {
            return issue.feature == RuntimeCapabilityFeature::ActionReset;
        }));

    EXPECT_TRUE(schemaHas(containerInspectorSchema(), QStringLiteral("mode")));
    EXPECT_FALSE(soundInspectorSchema().empty());
    EXPECT_FALSE(trackInspectorSchema().empty());
    EXPECT_FALSE(pieceInspectorSchema().empty());
    EXPECT_FALSE(layerInspectorSchema().empty());
    EXPECT_FALSE(busInspectorSchema().empty());
    EXPECT_TRUE(schemaHas(actionInspectorSchema(), QStringLiteral("steps")));
    EXPECT_TRUE(schemaHas(actionInspectorSchema(), QStringLiteral("notes")));
    EXPECT_FALSE(variableInspectorSchema().empty());
    EXPECT_TRUE(schemaHas(attenuationInspectorSchema(), QStringLiteral("dimensions")));
    EXPECT_TRUE(schemaHas(attenuationInspectorSchema(), QStringLiteral("notes")));
}
