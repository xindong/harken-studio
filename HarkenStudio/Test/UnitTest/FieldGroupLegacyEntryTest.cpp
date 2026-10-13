#include "Core/Components/VSelection.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "Support/FieldGroupTestUtils.h"
#include "ActionManager.h"
#include "AttenuationStore.h"
#include "Components/VMixNode.h"
#include "Components/VSpatial.h"
#include "Components/VTrack.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectManager.h"
#include "ProjectTypes.h"
#include "FieldGroupEditing.h"
#include "UndoCommands.h"
#include "Widgets/Blueprint/BlueprintConnectCommand.h"
#include "Widgets/Blueprint/SignalChainSource.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QJsonArray>
#include <QTemporaryDir>
#include <QtEndian>

#include <array>
#include <utility>

namespace VNX {
namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

constexpr std::array kDefinitionTypes{
    ProjectItem::Type::Sound, ProjectItem::Type::Container,
    ProjectItem::Type::Layer, ProjectItem::Type::Track, ProjectItem::Type::Piece};

harkenprojectschema::AuthoringGuid authoringGuid(const QUuid& guid)
{
    const auto bytes = guid.toRfc4122();
    return {qFromBigEndian<quint64>(bytes.constData()),
            qFromBigEndian<quint64>(bytes.constData() + 8)};
}

QJsonObject componentValues(const ProjectItem& item)
{
    QJsonObject result;
    if (const auto* mix = item.comp<VMixNode>()) {
        result.insert(QStringLiteral("mix"), mix->toJson());
        result.insert(QStringLiteral("mainOutputBusRef"), VNX::objectIdString(mix->mainOutputBusRef));
    }
    if (const auto* spatial = item.comp<VSpatial>()) {
        result.insert(QStringLiteral("spatial"), spatial->toJson());
        result.insert(QStringLiteral("panning"), spatial->panning);
        result.insert(QStringLiteral("attenuationRef"), VNX::objectIdString(spatial->attenuationRef));
    }
    result.insert(QStringLiteral("outputBusOverride"), item.outputBusOverride);
    return result;
}

class FieldGroupLegacyEntryTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("FieldGroupLegacyEntry");
        config.location = tempDir_.path();
        config.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("Android")});
        ASSERT_TRUE(manager.createProject(config));
        auto& actions = ActionManager::instance();
        actions.clearAllForTesting();
        registerProjectActions(actions, &manager, nullptr, {});
        registerMixerActions(actions, nullptr, &manager, nullptr);
        registerSpatialActions(actions, nullptr, &manager);
        registerContainerActions(actions, &manager);
        auto* master = manager.findBusItem(kMasterBusID);
        ASSERT_NE(master, nullptr);
        bus_ = manager.createItem(master, QStringLiteral("LegacyEntryBus"), ProjectItem::Type::Bus);
        targetBus_ = manager.createItem(master, QStringLiteral("LegacyEntryTarget"), ProjectItem::Type::Bus);
        ASSERT_NE(bus_, nullptr);
        ASSERT_NE(targetBus_, nullptr);
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ActionManager::instance().clearAllForTesting();
        ProjectManager::instance().closeProject();
    }

    ProjectItem* makeDefinition(ProjectItem::Type type, ProjectItem* parent = nullptr)
    {
        return ProjectManager::instance().createItem(
            parent, QStringLiteral("LegacyDefinition%1").arg(static_cast<int>(type)), type);
    }

    QJsonObject groupEdits() const
    {
        return {
            {"mainOutputBusRef", QStringLiteral("0")}, {"outputBusVolume", -8.0},
            {"sends", QJsonArray{}},
            {"spatialPanning", 0.5}, {"spatial.panning", 0.5}, {"panning", 0.5},
            {"spatialIs3DEnabled", true}, {"spatial.is3DEnabled", true}, {"is3DEnabled", true},
            {"spatialAttenuationRef", QStringLiteral("0")},
            {"spatial.attenuationRef", QStringLiteral("0")}, {"attenuationRef", QStringLiteral("0")},
            {"spatialAttenuationDistanceScaling", 2.0},
            {"spatial.attenuationDistanceScaling", 2.0}, {"attenuationDistanceScaling", 2.0},
            {"outputBusId", static_cast<qint64>(targetBus_->objectID)},
            {"outputBus", static_cast<qint64>(targetBus_->objectID)},
            {"output_bus", static_cast<qint64>(targetBus_->objectID)}};
    }

    QJsonObject itemArgs(const ProjectItem& item, const QString& property,
                         const QJsonValue& value) const
    {
        return {{"authoring_handle", static_cast<qint64>(item.objectID)},
                {"property", property}, {"value", value}};
    }

    void expectRejected(ProjectItem& item, const QString& action, const QJsonObject& args)
    {
        SCOPED_TRACE(action.toStdString());
        SCOPED_TRACE(args.value(QStringLiteral("property")).toString().toStdString());
        item.clearDirty();
        const auto groups = item.fieldGroups;
        const auto presence = item.playablePropertyPresence.bits;
        const auto platforms = item.platformOverrides;
        const auto* selection = item.comp<VSelection>();
        const auto selections = selection ? selection->selections : SelectionData{};
        const auto components = componentValues(item);
        const auto* timeline = item.comp<VTrack>();
        const auto clips = timeline ? timeline->clips : ClipLedger{};
        auto* stack = UndoRedoManager::instance().undoStack();
        const int undoCount = stack->count();
        const int undoIndex = stack->index();
        int changes = 0;
        QObject observer;
        QObject::connect(&ProjectManager::instance(), &ProjectManager::itemPropertyChanged,
                         &observer, [&changes] { ++changes; });
        const auto result = ActionManager::instance().execute(action, args);
        EXPECT_FALSE(result.success);
        EXPECT_EQ(result.errorCode, QStringLiteral("FIELD_GROUP_COMMAND_REQUIRED"));
        EXPECT_TRUE(result.errorMessage.contains(QStringLiteral("field group commands")));
        EXPECT_FALSE(result.undoFn);
        EXPECT_FALSE(result.redoFn);
        EXPECT_EQ(item.fieldGroups, groups);
        EXPECT_EQ(item.playablePropertyPresence.bits, presence);
        EXPECT_EQ(item.platformOverrides, platforms);
        if (selection) EXPECT_EQ(selection->selections, selections);
        EXPECT_EQ(componentValues(item), components);
        if (timeline) EXPECT_EQ(timeline->clips, clips);
        EXPECT_FALSE(item.isDirtyObject());
        EXPECT_EQ(changes, 0);
        EXPECT_EQ(stack->count(), undoCount);
        EXPECT_EQ(stack->index(), undoIndex);
    }

    QTemporaryDir tempDir_;
    ProjectItem* bus_ = nullptr;
    ProjectItem* targetBus_ = nullptr;
};

TEST_F(FieldGroupLegacyEntryTest, PropertyAliasesAndOverrideOperationsRejectAllFiveKinds)
{
    const auto edits = groupEdits();
    auto* parent = ProjectManager::instance().createItem(
        nullptr, QStringLiteral("LegacyOwner"), ProjectItem::Type::Layer);
    ASSERT_NE(parent, nullptr);
    for (const auto type : kDefinitionTypes) {
        auto* item = makeDefinition(type, parent);
        ASSERT_NE(item, nullptr);
        item->fieldGroups.mainOutput.shared.localHistory = MainOutputFieldGroup{{1, 2}, -11.0f};
        item->fieldGroups.send.shared.localHistory = SendFieldGroup{{{{3, 4}, -7.0f}}};
        item->fieldGroups.spatialRating.shared = {
            FieldGroupMode::Local, SpatialRatingFieldGroup{0.25f, false, {}, 1.5f}};
        item->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
            FieldGroupMode::UseShared, SpatialRatingFieldGroup{-0.25f, true, {}, 3.0f}};
        for (auto edit = edits.begin(); edit != edits.end(); ++edit) {
            for (const auto* operation : {"set_override", "clear_override", "clear_platform_override"}) {
                auto args = itemArgs(*item, edit.key(), edit.value());
                args.insert(QStringLiteral("override_operation"), QLatin1String(operation));
                args.insert(QStringLiteral("platform_ids"), QJsonArray{HarkenTest::platformIdString("Android")});
                if (QLatin1String(operation) != QLatin1String("set_override"))
                    args.remove(QStringLiteral("value"));
                expectRejected(*item, QStringLiteral("set_item_property"), args);
            }
        }
        auto args = itemArgs(*item, QStringLiteral("outputBusVolume"), -5.0);
        expectRejected(*item, QStringLiteral("set_item_property"), args);
        args.insert(QStringLiteral("platform_ids"),
                    QJsonArray{HarkenTest::platformIdString("Windows"), HarkenTest::platformIdString("Android")});
        expectRejected(*item, QStringLiteral("set_item_property"), args);
    }
}

TEST_F(FieldGroupLegacyEntryTest, ItemSelectorsCannotBypassThePropertyGuard)
{
    auto* item = makeDefinition(ProjectItem::Type::Sound);
    ASSERT_NE(item, nullptr);
    const std::array selectors{
        QJsonObject{{"path", item->name}},
        QJsonObject{{"object_id", static_cast<qint64>(item->objectID)}},
        QJsonObject{{"authoring_handle", static_cast<qint64>(item->objectID)}}};
    for (auto args : selectors) {
        args.insert(QStringLiteral("property"), QStringLiteral(" outputBusVolume "));
        args.insert(QStringLiteral("value"), -9.0);
        expectRejected(*item, QStringLiteral("set_item_property"), args);
    }
}

TEST_F(FieldGroupLegacyEntryTest, ClipAndContainerOccurrencesRejectGroupsAndPlaybackEdits)
{
    auto& manager = ProjectManager::instance();
    auto* container = makeDefinition(ProjectItem::Type::Container);
    auto* track = makeDefinition(ProjectItem::Type::Track);
    ASSERT_NE(container, nullptr);
    ASSERT_NE(track, nullptr);
    auto* sound = manager.createItem(container, QStringLiteral("OccurrenceSound"), ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    ASSERT_EQ(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 1u);
    Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.targetGuid = sound->guid;
    clip.duration = 960;
    auto* timeline = track->compOrCreate<VTrack>();
    ASSERT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
    timeline->clips.items.push_back(clip);
    const auto childLocator = PlayablePropertyAuthoringV1::containerChildLocator(*container, sound->guid);
    const auto clipLocator = PlayablePropertyAuthoringV1::trackClipLocator(*track, clip.clipGuid);
    ASSERT_TRUE(childLocator);
    ASSERT_TRUE(clipLocator);
    const auto soundGroups = sound->fieldGroups;
    const auto soundValues = componentValues(*sound);
    for (const auto& [owner, locator] : std::array{
             std::pair{container, *childLocator}, std::pair{track, *clipLocator}}) {
        auto args = itemArgs(*owner, QStringLiteral("sends"), QJsonArray{});
        args.insert(QStringLiteral("occurrence_kind"), owner == track
            ? QStringLiteral("track_clip") : QStringLiteral("container_child"));
        args.insert(QStringLiteral("occurrence_guid"), locator.occurrenceGuid.toString(QUuid::WithoutBraces));
        args.insert(QStringLiteral("occurrence_target_guid"), sound->guid.toString(QUuid::WithoutBraces));
        args.insert(QStringLiteral("occurrence_ordinal"), static_cast<qint64>(locator.stableOccurrenceOrdinal));
        args.insert(QStringLiteral("platform_ids"), QJsonArray{HarkenTest::platformIdString("Android")});
        for (const auto* field : {"mainOutputBusRef", "sends", "spatialIs3DEnabled"}) {
            args.insert(QStringLiteral("property"), QLatin1String(field));
            expectRejected(*owner, QStringLiteral("set_item_property"), args);
        }
        args.insert(QStringLiteral("property"), QStringLiteral("volume"));
        args.insert(QStringLiteral("value"), -7.0);
        const auto childRows = container->comp<VNX::VSelection>()->selections.shared;
        const auto clips = timeline->clips;
        const auto values = componentValues(*owner);
        const auto groups = owner->fieldGroups;
        const auto platforms = owner->platformOverrides;
        const auto presence = owner->playablePropertyPresence.bits;
        const int undoCount = UndoRedoManager::instance().undoStack()->count();
        int changes = 0;
        QObject observer;
        QObject::connect(&manager, &ProjectManager::itemPropertyChanged,
                         &observer, [&changes] { ++changes; });
        owner->clearDirty();
        for (const auto* operation : {"set_override", "clear_override", "clear_platform_override"}) {
            args.insert(QStringLiteral("override_operation"), QLatin1String(operation));
            const auto result = ActionManager::instance().execute(QStringLiteral("set_item_property"), args);
            EXPECT_FALSE(result.success);
            EXPECT_FALSE(result.undoFn);
            EXPECT_FALSE(result.redoFn);
        }
        EXPECT_EQ(container->comp<VNX::VSelection>()->selections.shared, childRows);
        EXPECT_EQ(timeline->clips, clips);
        EXPECT_EQ(componentValues(*owner), values);
        EXPECT_EQ(owner->fieldGroups, groups);
        EXPECT_EQ(owner->platformOverrides, platforms);
        EXPECT_EQ(owner->playablePropertyPresence.bits, presence);
        EXPECT_FALSE(owner->isDirtyObject());
        EXPECT_EQ(changes, 0);
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoCount);
        const auto volume = PlayablePropertyAuthoringV1::readProperty(
            *owner, locator, 1, HarkenTest::PlatformIds::Android);
        ASSERT_TRUE(volume);
        EXPECT_FALSE(volume->present);
        EXPECT_DOUBLE_EQ(volume->value.toDouble(), 0.0);
    }
    EXPECT_EQ(sound->fieldGroups, soundGroups);
    EXPECT_EQ(componentValues(*sound), soundValues);
}

TEST_F(FieldGroupLegacyEntryTest, LegacyRoutingAndSendNamesRejectEveryDefinitionKind)
{
    for (const auto type : kDefinitionTypes) {
        auto* item = makeDefinition(type);
        ASSERT_NE(item, nullptr);
        for (const auto* action : {"set_send", "remove_send", "set_send_level"}) {
            expectRejected(*item, QLatin1String(action),
                {{"src_bus", item->name}, {"target_bus_id", static_cast<qint64>(targetBus_->objectID)},
                 {"level", 0.5}});
            expectRejected(*item, QLatin1String(action),
                {{"src_bus", QStringLiteral("Pack://") + item->name},
                 {"target_bus_id", static_cast<qint64>(targetBus_->objectID)}, {"level", 0.5}});
        }
        expectRejected(*item, QStringLiteral("set_output_bus_routing"),
                       {{"path", item->name}, {"volume_db", -8.0}});
    }
}

TEST_F(FieldGroupLegacyEntryTest, AttenuationTargetAliasesRejectWithoutChangingTheResource)
{
    auto& actions = ActionManager::instance();
    const auto created = actions.execute(QStringLiteral("create_attenuation"), {{"name", "LegacyAttenuation"}});
    ASSERT_TRUE(created.success);
    const QUuid guid(created.data.value(QStringLiteral("guid")).toString());
    const auto* attenuation = AttenuationStore::findByGuid(ProjectManager::instance().currentProject(), guid);
    ASSERT_NE(attenuation, nullptr);
    const auto resource = attenuation->toJson();
    for (const auto type : kDefinitionTypes) {
        auto* item = makeDefinition(type);
        ASSERT_NE(item, nullptr);
        const std::array selectors{
            QJsonObject{{"path", item->name}}, QJsonObject{{"object_path", item->name}},
            QJsonObject{{"target_object", item->name}},
            QJsonObject{{"object_id", static_cast<qint64>(item->objectID)}}};
        for (const auto& selector : selectors) {
            auto args = selector;
            args.insert(QStringLiteral("attenuation"), QStringLiteral("LegacyAttenuation"));
            expectRejected(*item, QStringLiteral("bind_attenuation_to_object"), args);
            args = selector;
            args.insert(QStringLiteral("attenuation_guid"), guid.toString(QUuid::WithoutBraces));
            expectRejected(*item, QStringLiteral("bind_attenuation_to_object"), args);
            expectRejected(*item, QStringLiteral("unbind_attenuation_from_object"), selector);
            args = selector;
            args.insert(QStringLiteral("scaling"), 3.0);
            expectRejected(*item, QStringLiteral("set_attenuation_distance_scaling"), args);
        }
    }
    EXPECT_EQ(attenuation->toJson(), resource);
}

TEST_F(FieldGroupLegacyEntryTest, ContainerPropertyEntryCannotBypassGroupsOnAnyDefinitionKind)
{
    const auto edits = groupEdits();
    for (const auto type : kDefinitionTypes) {
        auto* item = makeDefinition(type);
        ASSERT_NE(item, nullptr);
        for (auto edit = edits.begin(); edit != edits.end(); ++edit)
            expectRejected(*item, QStringLiteral("set_container_property"),
                {{"container_path", item->name}, {"property", edit.key()}, {"value", edit.value()}});
    }
}

TEST_F(FieldGroupLegacyEntryTest, BusLocalOutputAndSendsRemainEditable)
{
    auto& actions = ActionManager::instance();
    ASSERT_TRUE(actions.execute(QStringLiteral("set_item_property"),
        itemArgs(*bus_, QStringLiteral("outputBusVolume"), -6.0)).success);
    EXPECT_FLOAT_EQ(bus_->comp<VMixNode>()->outputBusVolume, -6.0f);
    auto clear = itemArgs(*bus_, QStringLiteral("outputBusVolume"), QJsonValue{});
    clear.insert(QStringLiteral("override_operation"), QStringLiteral("clear_override"));
    ASSERT_TRUE(actions.execute(QStringLiteral("set_item_property"), clear).success);
    EXPECT_FLOAT_EQ(bus_->comp<VMixNode>()->outputBusVolume, 0.0f);
    ASSERT_TRUE(actions.execute(QStringLiteral("set_output_bus_routing"),
        {{"path", bus_->name}, {"volume_db", -4.0}}).success);
    EXPECT_FLOAT_EQ(bus_->comp<VMixNode>()->outputBusVolume, -4.0f);
    auto sendArgs = QJsonObject{{"src_bus", bus_->name},
        {"target_bus_id", static_cast<qint64>(targetBus_->objectID)}, {"level", 0.25}};
    ASSERT_TRUE(actions.execute(QStringLiteral("set_send"), sendArgs).success);
    ASSERT_EQ(bus_->comp<VMixNode>()->sends.size(), 1u);
    sendArgs.insert(QStringLiteral("level"), 0.5);
    ASSERT_TRUE(actions.execute(QStringLiteral("set_send_level"), sendArgs).success);
    EXPECT_FLOAT_EQ(bus_->comp<VMixNode>()->sends.front().sendLevel, 0.5f);
    ASSERT_TRUE(actions.execute(QStringLiteral("remove_send"), sendArgs).success);
    EXPECT_TRUE(bus_->comp<VMixNode>()->sends.empty());
    UndoRedoManager::instance().undoStack()->undo();
    ASSERT_EQ(bus_->comp<VMixNode>()->sends.size(), 1u);
    EXPECT_FLOAT_EQ(bus_->comp<VMixNode>()->sends.front().sendLevel, 0.5f);
    EXPECT_EQ(bus_->fieldGroups, DefinitionFieldGroups{});
}

TEST_F(FieldGroupLegacyEntryTest, NonGroupMixingStillSupportsPlatformEditingAndUndo)
{
    auto* sound = makeDefinition(ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    const auto groups = sound->fieldGroups;
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*sound);
    ASSERT_TRUE(locator);
    for (const auto* name : {"volume", "pitch", "makeUpGain"}) {
        const auto* field = PlayablePropertyAuthoringV1::fieldByName(QLatin1String(name));
        ASSERT_NE(field, nullptr);
        const auto before = PlayablePropertyAuthoringV1::readProperty(*sound, *locator, field->id, HarkenTest::PlatformIds::Android);
        auto args = itemArgs(*sound, QLatin1String(name), -3.0);
        args.insert(QStringLiteral("platform_ids"), QJsonArray{HarkenTest::platformIdString("Android")});
        ASSERT_TRUE(ActionManager::instance().execute(QStringLiteral("set_item_property"), args).success);
        const auto after = PlayablePropertyAuthoringV1::readProperty(*sound, *locator, field->id, HarkenTest::PlatformIds::Android);
        ASSERT_TRUE(after);
        EXPECT_TRUE(after->present);
        EXPECT_DOUBLE_EQ(after->value.toDouble(), -3.0);
        UndoRedoManager::instance().undoStack()->undo();
        EXPECT_EQ(PlayablePropertyAuthoringV1::readProperty(*sound, *locator, field->id, HarkenTest::PlatformIds::Android), before);
        EXPECT_EQ(sound->fieldGroups, groups);
    }
    auto* container = makeDefinition(ProjectItem::Type::Container);
    ASSERT_NE(container, nullptr);
    ASSERT_TRUE(ActionManager::instance().execute(QStringLiteral("set_container_property"),
        {{"container_path", container->name}, {"property", "volume"}, {"value", -5.0}}).success);
    EXPECT_FLOAT_EQ(container->comp<VMixNode>()->volume, -5.0f);
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_FLOAT_EQ(container->comp<VMixNode>()->volume, 0.0f);
}

TEST_F(FieldGroupLegacyEntryTest, BlueprintRequiresSharedLocalAndCannotToggleAnOverride)
{
    auto* parent = makeDefinition(ProjectItem::Type::Layer);
    ASSERT_NE(parent, nullptr);
    auto* item = makeDefinition(ProjectItem::Type::Sound, parent);
    ASSERT_NE(item, nullptr);
    item->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android] = {
        FieldGroupMode::Local, MainOutputFieldGroup{authoringGuid(bus_->guid), -3.0f}};
    const auto before = item->fieldGroups;
    const auto legacy = componentValues(*item);
    auto* stack = UndoRedoManager::instance().undoStack();
    const int count = stack->count();
    SignalChainSource source;
    for (const auto* edge : {"bus_route", "override"}) {
        EXPECT_FALSE(source.onConnect(item->objectID, targetBus_->objectID,
                                     QLatin1String(edge), QStringLiteral("item"), QStringLiteral("bus")));
        EXPECT_EQ(source.lastError(), QStringLiteral("field_group_read_only"));
        EXPECT_FALSE(source.onDisconnect(item->objectID, bus_->objectID,
                                        QLatin1String(edge), QStringLiteral("item"), QStringLiteral("bus")));
        EXPECT_EQ(stack->count(), count);
        EXPECT_EQ(item->fieldGroups, before);
    }
    stack->push(new BlueprintOverrideToggleCommand(item->objectID, true, targetBus_->objectID));
    EXPECT_EQ(stack->count(), count);
    EXPECT_EQ(item->fieldGroups, before);
    EXPECT_EQ(componentValues(*item), legacy);
}

TEST_F(FieldGroupLegacyEntryTest, BlueprintRouteUndoKeepsFullGroupsAndDisconnectSelectsMaster)
{
    auto& manager = ProjectManager::instance();
    auto* item = makeDefinition(ProjectItem::Type::Sound);
    auto* master = manager.findBusItem(kMasterBusID);
    ASSERT_NE(item, nullptr);
    ASSERT_NE(master, nullptr);
    auto initial = item->fieldGroups;
    initial.mainOutput.shared = {
        FieldGroupMode::Local, MainOutputFieldGroup{authoringGuid(master->guid), -13.0f}};
    initial.spatialRating.shared.localHistory = SpatialRatingFieldGroup{0.3f, false, {}, 2.0f};
    ASSERT_TRUE(manager.restoreFieldGroupsInternal(item, initial));
    SignalChainSource source;
    auto* stack = UndoRedoManager::instance().undoStack();
    const int base = stack->count();
    ASSERT_TRUE(source.onConnect(item->objectID, targetBus_->objectID,
                                 QStringLiteral("override"), QStringLiteral("item"), QStringLiteral("bus")));
    auto routed = initial;
    routed.mainOutput.shared.localHistory->mainOutputBusRef = authoringGuid(targetBus_->guid);
    EXPECT_EQ(item->fieldGroups, routed);
    EXPECT_EQ(stack->count(), base + 1);
    stack->undo();
    EXPECT_EQ(item->fieldGroups, initial);
    stack->redo();
    EXPECT_EQ(item->fieldGroups, routed);
    EXPECT_FALSE(source.onDisconnect(item->objectID, bus_->objectID,
                                     QStringLiteral("bus_route"), QStringLiteral("item"), QStringLiteral("bus")));
    EXPECT_EQ(item->fieldGroups, routed);
    EXPECT_EQ(stack->count(), base + 1);
    ASSERT_TRUE(source.onDisconnect(item->objectID, targetBus_->objectID,
                                    QStringLiteral("bus_route"), QStringLiteral("item"), QStringLiteral("bus")));
    EXPECT_EQ(item->fieldGroups, initial);
    EXPECT_EQ(item->fieldGroups.mainOutput.shared.mode, FieldGroupMode::Local);
    stack->undo();
    EXPECT_EQ(item->fieldGroups, routed);
}

TEST_F(FieldGroupLegacyEntryTest, BlueprintRootRoutingPreservesInheritAndUndoRestoresCurrentValues)
{
    auto& manager = ProjectManager::instance();
    auto* item = makeDefinition(ProjectItem::Type::Sound);
    ASSERT_NE(item, nullptr);
    const auto before = item->fieldGroups;
    ASSERT_EQ(before.mainOutput.shared.mode, FieldGroupMode::Inherit);
    auto* stack = UndoRedoManager::instance().undoStack();
    const int base = stack->count();
    SignalChainSource source;
    ASSERT_TRUE(source.onConnect(item->objectID, targetBus_->objectID,
        QStringLiteral("bus_route"), QStringLiteral("item"), QStringLiteral("bus")));
    auto routed = before;
    routed.mainOutput.shared.localHistory->mainOutputBusRef = authoringGuid(targetBus_->guid);
    EXPECT_EQ(item->fieldGroups, routed);
    EXPECT_EQ(stack->count(), base + 1);
    stack->undo();
    EXPECT_EQ(item->fieldGroups, before);
    stack->redo();
    EXPECT_EQ(item->fieldGroups, routed);
    ASSERT_TRUE(source.onDisconnect(item->objectID, targetBus_->objectID,
        QStringLiteral("bus_route"), QStringLiteral("item"), QStringLiteral("bus")));
    EXPECT_EQ(item->fieldGroups, before);
}

TEST_F(FieldGroupLegacyEntryTest, BlueprintContainmentDoesNotRewriteLocalGroupState)
{
    auto& manager = ProjectManager::instance();
    auto* item = makeDefinition(ProjectItem::Type::Sound);
    auto* parent = makeDefinition(ProjectItem::Type::Layer);
    ASSERT_NE(item, nullptr);
    ASSERT_NE(parent, nullptr);
    auto local = item->fieldGroups;
    local.mainOutput.shared = {
        FieldGroupMode::Local, MainOutputFieldGroup{authoringGuid(bus_->guid), -6.0f}};
    ASSERT_TRUE(manager.restoreFieldGroupsInternal(item, local));
    SignalChainSource source;
    ASSERT_TRUE(source.onConnect(item->objectID, parent->objectID,
                                 QStringLiteral("normal"), QStringLiteral("item"), QStringLiteral("item")));
    EXPECT_EQ(item->parent, parent);
    EXPECT_EQ(item->fieldGroups, local);
    UndoRedoManager::instance().undoStack()->undo();
    EXPECT_EQ(item->fieldGroups, local);
    auto* invalidParent = manager.createItem(parent, QStringLiteral("InvalidSoundParent"), ProjectItem::Type::Sound);
    ASSERT_NE(invalidParent, nullptr);
    auto* stack = UndoRedoManager::instance().undoStack();
    const int count = stack->count();
    const int index = stack->index();
    EXPECT_FALSE(source.onConnect(item->objectID, invalidParent->objectID,
                                  QStringLiteral("normal"), QStringLiteral("item"), QStringLiteral("item")));
    EXPECT_EQ(item->fieldGroups, local);
    EXPECT_EQ(stack->count(), count);
    EXPECT_EQ(stack->index(), index);
}

TEST_F(FieldGroupLegacyEntryTest, BlueprintSendCommandsKeepWholeLocalTablesAndDoNotAddUiCapability)
{
    auto& manager = ProjectManager::instance();
    auto* parent = makeDefinition(ProjectItem::Type::Layer);
    ASSERT_NE(parent, nullptr);
    auto* item = makeDefinition(ProjectItem::Type::Sound, parent);
    ASSERT_NE(item, nullptr);
    auto* stack = UndoRedoManager::instance().undoStack();
    const int base = stack->count();
    stack->push(new BlueprintSendConnectCommand(item->objectID, targetBus_->objectID));
    EXPECT_EQ(stack->count(), base);
    auto local = item->fieldGroups;
    local.send.shared = {FieldGroupMode::Local,
                        SendFieldGroup{{{authoringGuid(bus_->guid), -9.0f}}}};
    ASSERT_TRUE(manager.restoreFieldGroupsInternal(item, local));
    stack->push(new BlueprintSendConnectCommand(item->objectID, targetBus_->objectID));
    auto connected = local;
    connected.send.shared.localHistory->sends.push_back({authoringGuid(targetBus_->guid), 0.0f});
    EXPECT_EQ(item->fieldGroups, connected);
    stack->undo();
    EXPECT_EQ(item->fieldGroups, local);
    stack->redo();
    EXPECT_EQ(item->fieldGroups, connected);
    stack->push(new BlueprintSendDisconnectCommand(item->objectID, targetBus_->objectID));
    EXPECT_EQ(item->fieldGroups, local);
    stack->undo();
    EXPECT_EQ(item->fieldGroups, connected);
    SignalChainSource source;
    EXPECT_FALSE(source.onConnect(item->objectID, targetBus_->objectID,
                                  QStringLiteral("send"), QStringLiteral("item"), QStringLiteral("bus")));
    EXPECT_EQ(source.lastError(), QStringLiteral("send_from_not_bus"));
    EXPECT_EQ(item->fieldGroups, connected);
}

}
}
