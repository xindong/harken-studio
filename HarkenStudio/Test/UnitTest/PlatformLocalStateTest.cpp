#include "Core/ActionManager.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/BusRouteGraph.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VSelection.h"
#include "Core/Components/VSound.h"
#include "Core/Components/VVariable.h"
#include "Core/FieldGroupDerivedView.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/PlatformContextService.h"
#include "Core/PlatformLocalState.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectSerializer.h"
#include "Core/UndoCommands.h"
#include "Support/ProjectPlatformTestUtils.h"

#include <gtest/gtest.h>

#include <QApplication>
#include <QScopeGuard>
#include <QTemporaryDir>

namespace {

using namespace harken::studio;
using Kind = PlatformLocalAddress::Kind;

TEST(PlatformLocalState, EveryStoreKeepsEqualLocalValuesAndRestoresHistory)
{
    static int argc = 1;
    static char arg0[] = "harken_studio_tests";
    static char* argv[] = {arg0, nullptr};
    std::unique_ptr<QApplication> app;
    if (!qApp) app = std::make_unique<QApplication>(argc, argv);
    auto& manager = ProjectManager::instance();
    auto& history = UndoRedoManager::instance();
    ActionManager::instance().clearAllForTesting();
    registerProjectActions(ActionManager::instance(), &manager, nullptr);
    if (manager.isProjectOpen()) manager.closeProject();
    PlatformContextService::instance().setStorageForTesting(std::make_unique<MemorySettingsStorage>());
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const auto close = qScopeGuard([&] {
        history.clear();
        if (manager.isProjectOpen()) manager.closeProject();
        ActionManager::instance().clearAllForTesting();
    });
    ProjectConfig config;
    config.name = QStringLiteral("PlatformLocalState");
    config.location = directory.path();
    config.platforms = HarkenTest::platforms({"Windows", "Android"});
    ASSERT_TRUE(manager.createProject(config));
    const auto android = HarkenTest::PlatformIds::Android;

    auto* layer = manager.createItem(nullptr, QStringLiteral("Layer"), ProjectItem::Type::Layer);
    auto* sound = manager.createItem(layer, QStringLiteral("Sound"), ProjectItem::Type::Sound);
    auto* container = manager.createItem(nullptr, QStringLiteral("Container"), ProjectItem::Type::Container);
    auto* bus = manager.createItem(manager.getMasterBusItem(), QStringLiteral("Bus"), ProjectItem::Type::Bus);
    auto* target = manager.createItem(manager.getMasterBusItem(), QStringLiteral("Target"), ProjectItem::Type::Bus);
    auto* member = manager.createItem(container, QStringLiteral("Member"), ProjectItem::Type::Sound);
    auto* variableItem = manager.createItem(nullptr, QStringLiteral("Variable"), ProjectItem::Type::Variable);
    ASSERT_TRUE(layer && sound && container && bus && target && member && variableItem);
    AssetInfo asset;
    asset.sourceID = 1;
    asset.relativePath = QStringLiteral("Take.wav");
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(asset));
    SoundSourceDef source;
    source.entryId = QUuid::createUuid();
    source.sourceId = 1;
    source.name = QStringLiteral("Take");
    sound->comp<VSound>()->sources.plain.entries = {source};
    sound->comp<VSound>()->sources.plain.active = source.entryId;
    const auto row = [] { return QUuid::createUuid(); };
    const auto busSend = row(), busSidechain = row(), layerSend = row(), layerSidechain = row();
    EffectEntry busEffect;
    busEffect.guid = row();
    busEffect.mode = EffectMode::Custom;
    busEffect.custom.parameters = {0.0f, 20.0f};
    auto layerEffect = busEffect;
    layerEffect.guid = row();
    ASSERT_TRUE(restoreBusSendState(*bus, {bus->guid, {{busSend, target->objectID, -6.0f, false, {}}},
                                           {{busSidechain, target->objectID, -3.0f, true, {}}}}));
    ASSERT_TRUE(bus->comp<VMixNode>()->effects.replace({busEffect}));
    const auto targetGuid = toFieldGroupGuid(target->guid);
    layer->fieldGroups.send.shared = {FieldGroupMode::Local,
        SendFieldGroup{{{toFieldGroupGuid(layerSend), targetGuid, -6.0f}}}};
    layer->fieldGroups.sidechain.shared = {FieldGroupMode::Local,
        SidechainFieldGroup{{{toFieldGroupGuid(layerSidechain), targetGuid, -3.0f, true}}}};
    EffectFieldGroup layerEffects;
    ASSERT_TRUE(layerEffects.effects.replace({layerEffect}));
    layer->fieldGroups.effects.shared = {FieldGroupMode::Local, layerEffects};
    ASSERT_TRUE(refreshFieldGroupDerivedViews(*manager.currentProject()));
    auto* variable = variableItem->comp<VVariable>();
    variable->labels = {{1, QStringLiteral("One")}};
    auto& selections = container->comp<VSelection>()->selections;
    auto grid = selections.matchGrid();
    ASSERT_TRUE(grid.addAxis(variableItem->guid) && grid.addRow(member->guid)
                && grid.setCell(0, 0, DiscreteGridCell(variable->labels.front().id)));
    ASSERT_TRUE(selections.setMatchGrid(grid, std::nullopt));
    ASSERT_TRUE(selections.setMixing({{{variableItem->guid,
        {{member->guid, {{0.0f, 0.0f}, {100.0f, 1.0f}}, {{25.0f, 0.25f, 75.0f, 0.75f}}}}}}}, std::nullopt));
    ASSERT_FALSE(selections.random.shared.occurrenceRows().empty());
    history.clear();

    struct Case { ProjectItem* item; PlatformLocalAddress address; };
    std::vector<Case> cases{
        {sound, {Kind::Property, QStringLiteral("volume")}},
        {sound, {Kind::Property, QStringLiteral("pitch")}},
        {sound, {Kind::Property, QStringLiteral("loopCount")}},
        {sound, {Kind::Filter, QStringLiteral("filter")}},
        {sound, {Kind::SourceActive, {}}},
        {bus, {Kind::Property, QStringLiteral("volume")}},
        {bus, {Kind::Property, QStringLiteral("outputBusVolume")}},
        {layer, {Kind::GroupField, QStringLiteral("outputBusVolume"), FieldGroupId::Output}},
        {layer, {Kind::GroupField, QStringLiteral("spatialPanning"), FieldGroupId::SpatialRating}},
        {layer, {Kind::GroupField, QStringLiteral("spatialIs3DEnabled"), FieldGroupId::SpatialRating}},
        {layer, {Kind::GroupField, QStringLiteral("spatialAttenuationRef"), FieldGroupId::SpatialRating}},
        {layer, {Kind::GroupField, QStringLiteral("spatialAttenuationDistanceScaling"), FieldGroupId::SpatialRating}},
        {layer, {Kind::GroupField, QStringLiteral("settingsRef"), FieldGroupId::Conversion}},
        {bus, {Kind::RowCell, QStringLiteral("sends"), FieldGroupId::Send, false, busSend, QStringLiteral("sendLevel")}},
        {bus, {Kind::RowCell, QStringLiteral("sidechains"), FieldGroupId::Sidechain, false, busSidechain,
               QStringLiteral("duckTarget")}},
        {bus, {Kind::EffectChain, QStringLiteral("effects"), FieldGroupId::Effect}},
        {layer, {Kind::RowCell, QStringLiteral("sends"), FieldGroupId::Send, false, layerSend,
                 QStringLiteral("sendLevel")}},
        {layer, {Kind::RowCell, QStringLiteral("sidechains"), FieldGroupId::Sidechain, false, layerSidechain,
                 QStringLiteral("duckTarget")}},
        {layer, {Kind::EffectChain, QStringLiteral("effects"), FieldGroupId::Effect}},
        {container, {Kind::Selections, QStringLiteral("selections")}},
    };

    ProjectSerializer serializer;
    for (const auto& [item, address] : cases) {
        SCOPED_TRACE(QStringLiteral("%1 %2 %3").arg(item->name, address.field)
            .arg(static_cast<int>(address.kind) * 10 + static_cast<int>(address.group)).toStdString());
        const auto& project = *manager.currentProject();
        for (const auto& platform : PlatformLocalState::localPlatforms(project, *item, address))
            ASSERT_TRUE(PlatformLocalState::clearLocal(*item, address, platform).success);
        ASSERT_TRUE(serializer.prepareReferenceWrite(*manager.currentProject()));
        const auto before = serializer.projectItemToJson(*item);
        const auto value = PlatformLocalState::effectiveValue(project, *item, address, android);
        EXPECT_TRUE(PlatformLocalState::localPlatforms(project, *item, address).isEmpty());

        const auto set = PlatformLocalState::setLocal(project, *item, address, android);
        EXPECT_TRUE(set.success) << set.errorMessage.toStdString();
        EXPECT_EQ(PlatformLocalState::localPlatforms(project, *item, address), QSet<ProjectPlatformId>{android});
        EXPECT_EQ(PlatformLocalState::effectiveValue(project, *item, address, android), value);
        history.undo();
        EXPECT_EQ(serializer.projectItemToJson(*item), before);
        history.redo();
        EXPECT_EQ(PlatformLocalState::localPlatforms(project, *item, address), QSet<ProjectPlatformId>{android});

        const auto cleared = PlatformLocalState::clearLocal(*item, address, android);
        EXPECT_TRUE(cleared.success) << cleared.errorMessage.toStdString();
        EXPECT_TRUE(PlatformLocalState::localPlatforms(project, *item, address).isEmpty());
        EXPECT_EQ(PlatformLocalState::effectiveValue(project, *item, address, android), value);
        history.undo();
        EXPECT_EQ(PlatformLocalState::localPlatforms(project, *item, address), QSet<ProjectPlatformId>{android});
        history.redo();
    }
}

}
