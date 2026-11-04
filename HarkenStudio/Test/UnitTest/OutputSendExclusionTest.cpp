#include "ProjectManager.h"
#include "ProjectSerializer.h"
#include "ProjectTypes.h"
#include "UndoCommands.h"
#include "Core/ActionManager.h"
#include "Core/BusRouteGraph.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/IPC/IPCServer.h"
#include "Core/PlatformContextService.h"
#include "Widgets/Blueprint/BlueprintConnectCommand.h"
#include "Widgets/Blueprint/SignalPathEditing.h"

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
        VNX::PlatformContextService::instance().setStorageForTesting(
            std::make_unique<VNX::MemorySettingsStorage>());
        ASSERT_TRUE(tempDir.isValid());
        VNX::ProjectConfig config;
        config.name = QStringLiteral("OutputSendExclusionProject");
        config.location = tempDir.path();
        config.platforms = {
            {QUuid::createUuid(), QStringLiteral("Windows"), VNX::BasePlatform::Windows},
            {QUuid::createUuid(), QStringLiteral("Android"), VNX::BasePlatform::Android}};
        ASSERT_TRUE(pm.createProject(config));
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

TEST_F(OutputSendExclusionTest, RouteCommandsRestoreCompleteAuthoredProperty)
{
    using namespace VNX;
    auto& manager = ProjectManager::instance();
    auto* source = manager.createItem(manager.getMasterBusItem(), QStringLiteral("Source"),
                                      ProjectItem::Type::Bus);
    auto* target = manager.createItem(manager.getMasterBusItem(), QStringLiteral("Target"),
                                      ProjectItem::Type::Bus);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(target, nullptr);
    const QJsonArray rows{
        QJsonObject{{"targetBusID", static_cast<qint64>(target->objectID)},
                    {"sendLevel", -9.0}, {"mode", static_cast<int>(SendMode::Mix)},
                    {"modeVolumes", QJsonObject{{"1", 6.0}, {"2", -18.0}}}},
        QJsonObject{{"targetBusID", static_cast<qint64>(target->objectID)},
                    {"sendLevel", -3.0}, {"mode", static_cast<int>(SendMode::Sidechain)}}};
    ASSERT_TRUE(restoreBusSendState(*source, {source->guid, {}, {true, rows}}));
    const auto before = captureBusSendState(*source);
    ASSERT_TRUE(before);
    auto& actions = ActionManager::instance();
    actions.clearAllForTesting();
    registerMixerActions(actions, nullptr, &manager, nullptr);
    for (const auto* action : {"set_send", "remove_send"}) {
        SCOPED_TRACE(action);
        const auto* definition = actions.findAction(QLatin1String(action));
        ASSERT_NE(definition, nullptr);
        const auto result = definition->handler({
            {"src_bus", source->guid.toString(QUuid::WithoutBraces)},
            {"target_bus_id", static_cast<qint64>(target->objectID)}, {"level", 0.5}});
        ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
        ASSERT_TRUE(result.undoFn);
        ASSERT_TRUE(result.redoFn);
        const auto after = captureBusSendState(*source);
        ASSERT_NE(after, before);
        result.undoFn();
        EXPECT_EQ(captureBusSendState(*source), before);
        result.redoFn();
        EXPECT_EQ(captureBusSendState(*source), after);
        result.undoFn();
    }
    actions.clearAllForTesting();

    BlueprintSendDisconnectCommand disconnect(source->objectID, target->objectID);
    disconnect.redo();
    const auto disconnected = captureBusSendState(*source);
    ASSERT_NE(disconnected, before);
    disconnect.undo();
    EXPECT_EQ(captureBusSendState(*source), before);
    disconnect.redo();
    EXPECT_EQ(captureBusSendState(*source), disconnected);
    disconnect.undo();
    EXPECT_EQ(captureBusSendState(*source), before);
}

TEST_F(OutputSendExclusionTest, StructuralEditsPreserveIndependentRoutingScopes)
{
    using namespace VNX;
    auto& manager = ProjectManager::instance();
    auto& undo = UndoRedoManager::instance();
    auto* source = manager.createItem(manager.getMasterBusItem(), QStringLiteral("Source"),
                                      ProjectItem::Type::Bus);
    auto* platformSource = manager.createItem(manager.getMasterBusItem(), QStringLiteral("PlatformSource"),
                                              ProjectItem::Type::Bus);
    auto* target = manager.createItem(manager.getMasterBusItem(), QStringLiteral("Target"),
                                      ProjectItem::Type::Bus);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(platformSource, nullptr);
    ASSERT_NE(target, nullptr);
    const auto targetId = target->objectID;
    const auto targetGuid = target->guid;
    const auto windows = manager.currentProject()->platforms[0].id;
    const auto android = manager.currentProject()->platforms[1].id;
    QJsonArray rows;
    for (const auto mode : {SendMode::Mix, SendMode::Ducking, SendMode::Sidechain}) {
        const int other = (static_cast<int>(mode) + 1) % 3;
        rows.append(QJsonObject{{"targetBusID", static_cast<qint64>(targetId)},
            {"mode", static_cast<int>(mode)}, {"sendLevel", -6.0},
            {"modeVolumes", QJsonObject{{QString::number(other), 3.0}}}});
    }
    ASSERT_TRUE(restoreBusSendState(*source, {source->guid, {}, {true, rows}}));
    rows.removeAt(0);
    ASSERT_TRUE(restoreBusSendState(*source, {source->guid, windows, {true, rows}}));
    ASSERT_TRUE(restoreBusSendState(*platformSource, {platformSource->guid, windows, {true, rows}}));
    const auto snapshot = [&] {
        std::vector<BusSendState> states;
        for (const auto* item : {source, platformSource})
            for (const auto& scope : busRoutingScopes(*manager.currentProject()))
                states.push_back(*captureBusSendState(*item, scope));
        return states;
    };
    const auto before = snapshot();
    auto& context = PlatformContextService::instance();
    context.setSelected({windows}, windows);
    const auto platformReceives = manager.collectIncomingSends(targetId);
    ASSERT_EQ(platformReceives.size(), 4u);
    for (const auto& receive : platformReceives) EXPECT_NE(receive.mode, SendMode::Mix);
    context.setSelected({android}, android);
    const auto sharedReceives = manager.collectIncomingSends(targetId);
    ASSERT_EQ(sharedReceives.size(), 3u);
    for (const auto& receive : sharedReceives) EXPECT_EQ(receive.fromBusID, source->objectID);
    undo.clear();

    ASSERT_TRUE(manager.deleteItem(target));
    for (const auto* item : {source, platformSource})
        for (const auto& scope : busRoutingScopes(*manager.currentProject())) {
            const auto sends = effectiveBusSends(*item, scope);
            ASSERT_TRUE(sends);
            EXPECT_TRUE(sends->empty());
        }
    const auto deleted = snapshot();
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(snapshot(), before);
    ASSERT_TRUE(undo.redo());
    EXPECT_EQ(snapshot(), deleted);
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(snapshot(), before);
    target = manager.findItemByGuid(targetGuid);
    ASSERT_NE(target, nullptr);

    ASSERT_TRUE(manager.moveItem(source, target));
    for (const auto& scope : busRoutingScopes(*manager.currentProject())) {
        const auto sends = effectiveBusSends(*source, scope);
        ASSERT_TRUE(sends);
        ASSERT_EQ(sends->size(), 1u);
        EXPECT_EQ(sends->front().mode, SendMode::Sidechain);
        EXPECT_FALSE(sends->front().modeVolumesDb.empty());
    }
    EXPECT_FALSE(captureBusSendState(*source, android)->value.present);
    const auto moved = snapshot();
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(source->parent, manager.getMasterBusItem());
    EXPECT_EQ(snapshot(), before);
    ASSERT_TRUE(undo.redo());
    EXPECT_EQ(source->parent, target);
    EXPECT_EQ(snapshot(), moved);
}

TEST_F(OutputSendExclusionTest, UnsupportedCommandFieldsRejectBeforeAuthoringOrAuditionMutation)
{
    using namespace VNX;
    auto& manager = ProjectManager::instance();
    auto& actions = ActionManager::instance();
    auto& undo = UndoRedoManager::instance();
    auto* bus = manager.getMasterBusItem();
    ASSERT_NE(bus, nullptr);
    actions.clearAllForTesting();
    registerMixerActions(actions, nullptr, &manager, nullptr);
    IPCServer server(nullptr);
    server.setProjectManager(&manager);
    server.setAppState(IPCServer::AppState::Ready);
    ProjectSerializer serializer;
    ASSERT_TRUE(serializer.prepareReferenceWrite(*manager.currentProject()));
    const auto before = serializer.projectItemToJson(*bus);
    ASSERT_FALSE(before.isEmpty());
    const bool muted = manager.auditionOverlay().isMuted(bus->runtimeId());
    const bool dirty = bus->isDirtyObject();
    undo.clear();
    for (const auto* unsupported : {"unknownProperty", "makeUpGain", "makeup_gain"}) {
        SCOPED_TRACE(unsupported);
        QJsonObject args{{"bus_id", static_cast<qint64>(bus->objectID)},
            {"volume", -6.0}, {"pan", 0.5}, {"mute", !muted},
            {QLatin1String(unsupported), 3.0}};
        const auto rejected = actions.execute(QStringLiteral("set_bus_properties"), args);
        EXPECT_FALSE(rejected.success);
        EXPECT_EQ(rejected.errorCode, QStringLiteral("PROPERTY_NOT_SUPPORTED"));
        for (const bool preview : {false, true}) {
            args.insert(QStringLiteral("preview"), preview);
            const auto response = server.dispatchCommand(QStringLiteral("set_bus_properties"), args);
            EXPECT_EQ(response.value(QStringLiteral("status")).toString(), QStringLiteral("error"));
            EXPECT_EQ(serializer.projectItemToJson(*bus), before);
            EXPECT_EQ(manager.auditionOverlay().isMuted(bus->runtimeId()), muted);
            EXPECT_EQ(bus->isDirtyObject(), dirty);
            EXPECT_EQ(undo.undoStack()->count(), 0);
            EXPECT_EQ(undo.undoStack()->index(), 0);
        }
    }
    ASSERT_TRUE(actions.execute(QStringLiteral("set_bus_properties"), {
        {"bus_id", static_cast<qint64>(bus->objectID)}, {"volume", -6.0}}).success);
    EXPECT_NE(serializer.projectItemToJson(*bus), before);
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(serializer.projectItemToJson(*bus), before);
}

TEST_F(OutputSendExclusionTest, RoutingEditsRejectCyclesAtomicallyAcrossLandingScopes)
{
    using namespace VNX;
    auto& manager = ProjectManager::instance();
    auto& actions = ActionManager::instance();
    auto& undo = UndoRedoManager::instance();
    auto* source = manager.createItem(manager.getMasterBusItem(), QStringLiteral("Source"),
                                      ProjectItem::Type::Bus);
    auto* target = manager.createItem(manager.getMasterBusItem(), QStringLiteral("Target"),
                                      ProjectItem::Type::Bus);
    auto* other = manager.createItem(manager.getMasterBusItem(), QStringLiteral("Other"),
                                     ProjectItem::Type::Bus);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(target, nullptr);
    ASSERT_NE(other, nullptr);
    const auto windows = manager.currentProject()->platforms[0].id;
    const auto android = manager.currentProject()->platforms[1].id;
    const QJsonArray shared{
        QJsonObject{{"targetBusID", static_cast<qint64>(other->objectID)},
            {"mode", static_cast<int>(SendMode::Mix)}, {"sendLevel", -6.0},
            {"modeVolumes", QJsonObject{{"1", 3.0}}}}};
    const QJsonArray overlay{
        QJsonObject{{"targetBusID", static_cast<qint64>(other->objectID)},
            {"mode", static_cast<int>(SendMode::Sidechain)}, {"sendLevel", -12.0},
            {"modeVolumes", QJsonObject{{"0", -18.0}}}}};
    ASSERT_TRUE(restoreBusSendState(*source, {source->guid, {}, {true, shared}}));
    ASSERT_TRUE(restoreBusSendState(*source, {source->guid, android, {true, overlay}}));
    ASSERT_TRUE(restoreBusSendState(*target, {target->guid, windows, {true, QJsonArray{
        QJsonObject{{"targetBusID", static_cast<qint64>(source->objectID)},
            {"mode", static_cast<int>(SendMode::Mix)}, {"sendLevel", -9.0}}}}}));
    ASSERT_TRUE(validateBusRouting(*manager.currentProject()).isEmpty());
    const auto snapshot = [&] {
        std::vector<BusSendState> states;
        for (const auto* item : {source, target, other})
            for (const auto& scope : busRoutingScopes(*manager.currentProject()))
                states.push_back(*captureBusSendState(*item, scope));
        return states;
    };
    const auto before = snapshot();
    ASSERT_FALSE(captureBusSendState(*source, windows)->value.present);
    ASSERT_TRUE(captureBusSendState(*source, android)->value.present);
    actions.clearAllForTesting();
    registerProjectActions(actions, &manager, nullptr);
    undo.clear();
    PlatformContextService::instance().setSelected({android}, android);
    const QJsonArray rows{
        QJsonObject{{"targetBusID", static_cast<qint64>(target->objectID)},
            {"mode", static_cast<int>(SendMode::Mix)}, {"sendLevel", -3.0},
            {"modeVolumes", QJsonObject{{"2", -24.0}}}}};
    QJsonObject request{{"authoring_handle", static_cast<qint64>(source->objectID)},
        {"property", "sends"}, {"value", rows}};
    for (const bool sharedEdit : {false, true}) {
        SCOPED_TRACE(sharedEdit);
        auto candidate = request;
        if (!sharedEdit)
            candidate.insert(QStringLiteral("platform_ids"), QJsonArray{
                android.toString(QUuid::WithoutBraces), windows.toString(QUuid::WithoutBraces)});
        const int count = undo.undoStack()->count();
        const int index = undo.undoStack()->index();
        const auto result = actions.execute(QStringLiteral("set_item_property"), candidate);
        EXPECT_FALSE(result.success);
        EXPECT_EQ(snapshot(), before);
        EXPECT_EQ(undo.undoStack()->count(), count);
        EXPECT_EQ(undo.undoStack()->index(), index);
        EXPECT_TRUE(validateBusRouting(*manager.currentProject()).isEmpty());
    }
    request.insert(QStringLiteral("platform_ids"), QJsonArray{
        android.toString(QUuid::WithoutBraces)});
    const auto accepted = actions.execute(QStringLiteral("set_item_property"), request);
    ASSERT_TRUE(accepted.success) << accepted.errorMessage.toStdString();
    EXPECT_EQ(captureBusSendState(*source, android)->value,
              (PlayablePropertyStateV1{true, rows}));
    EXPECT_FALSE(captureBusSendState(*source, windows)->value.present);
    EXPECT_EQ(captureBusSendState(*source)->value, (PlayablePropertyStateV1{true, shared}));
    EXPECT_TRUE(validateBusRouting(*manager.currentProject()).isEmpty());
    const auto after = snapshot();
    ASSERT_EQ(undo.undoStack()->count(), 1);
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(snapshot(), before);
    ASSERT_TRUE(undo.redo());
    EXPECT_EQ(snapshot(), after);
}

TEST_F(OutputSendExclusionTest, GraphRouteEditsPreserveUnselectedScopesAndRestoreOverlayAbsence)
{
    using namespace VNX;
    auto& manager = ProjectManager::instance();
    auto& undo = UndoRedoManager::instance();
    auto* source = manager.createItem(manager.getMasterBusItem(), QStringLiteral("Source"), ProjectItem::Type::Bus);
    auto* target = manager.createItem(manager.getMasterBusItem(), QStringLiteral("Target"), ProjectItem::Type::Bus);
    auto* other = manager.createItem(manager.getMasterBusItem(), QStringLiteral("Other"), ProjectItem::Type::Bus);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(target, nullptr);
    ASSERT_NE(other, nullptr);
    const auto windows = manager.currentProject()->platforms[0].id;
    const auto android = manager.currentProject()->platforms[1].id;
    const QJsonArray rows{
        QJsonObject{{"targetBusID", static_cast<qint64>(target->objectID)}, {"mode", 0}, {"sendLevel", -3.0}},
        QJsonObject{{"targetBusID", static_cast<qint64>(target->objectID)}, {"mode", 2}, {"sendLevel", -12.0},
                    {"modeVolumes", QJsonObject{{"0", 3.0}, {"1", -9.0}}}}};
    ASSERT_TRUE(restoreBusSendState(*source, {source->guid, {}, {true, rows}}));
    ASSERT_TRUE(restoreBusSendState(*source, {source->guid, android, {true, QJsonArray{rows[1]}}}));
    PlatformContextService::instance().setSelected({windows}, windows, false);
    const auto snapshot = [&] {
        std::vector<BusSendState> states;
        for (const auto& scope : busRoutingScopes(*manager.currentProject()))
            states.push_back(*captureBusSendState(*source, scope));
        return states;
    };
    const auto before = snapshot();
    const auto graph = buildSignalPathGraph(*manager.currentProject(), {}, toFieldGroupGuid(windows),
        {source->objectID, target->objectID, other->objectID});
    ASSERT_TRUE(graph.ok());
    const auto edge = std::find_if(graph.edges.begin(), graph.edges.end(), [&](const auto& entry) {
        return entry.kind == SignalPathEdgeKind::Send && entry.mode == SendMode::Sidechain
            && graph.nodes[entry.from.occurrenceIndex].objectID == source->objectID;
    });
    ASSERT_NE(edge, graph.edges.end());
    auto disconnect = prepareSignalPathDisconnect(graph, {static_cast<size_t>(std::distance(graph.edges.begin(), edge))});
    ASSERT_TRUE(disconnect.ok()) << disconnect.error.toStdString();
    undo.clear();
    ASSERT_TRUE(applySignalPathConnect(std::move(disconnect)));
    ASSERT_TRUE(captureBusSendState(*source, windows)->value.present);
    EXPECT_EQ(busSendRows(*source, windows), (std::optional<QJsonArray>{QJsonArray{rows[0]}}));
    EXPECT_EQ(captureBusSendState(*source), std::optional{before[0]});
    EXPECT_EQ(captureBusSendState(*source, android), std::optional{before[2]});
    const auto disconnected = snapshot();
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(snapshot(), before);
    EXPECT_FALSE(captureBusSendState(*source, windows)->value.present);
    ASSERT_TRUE(undo.redo());
    EXPECT_EQ(snapshot(), disconnected);
    ASSERT_TRUE(undo.undo());

    const auto endpoint = [&](const ProjectItem* item) {
        const auto node = std::find_if(graph.nodes.begin(), graph.nodes.end(),
            [&](const auto& entry) { return entry.objectID == item->objectID; });
        return SignalPathEndpoint{static_cast<size_t>(std::distance(graph.nodes.begin(), node)), 0};
    };
    auto connect = prepareSignalPathConnect(graph, endpoint(source), endpoint(other), true);
    ASSERT_TRUE(connect.ok()) << connect.error.toStdString();
    ASSERT_TRUE(applySignalPathConnect(std::move(connect)));
    ASSERT_TRUE(busSendRows(*source, windows));
    EXPECT_EQ(busSendRows(*source, windows)->size(), 3);
    EXPECT_EQ(captureBusSendState(*source), std::optional{before[0]});
    EXPECT_EQ(captureBusSendState(*source, android), std::optional{before[2]});
    const auto connected = snapshot();
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(snapshot(), before);
    ASSERT_TRUE(undo.redo());
    EXPECT_EQ(snapshot(), connected);
    ASSERT_TRUE(undo.undo());
    auto stale = prepareSignalPathDisconnect(graph, {static_cast<size_t>(std::distance(graph.edges.begin(), edge))});
    ASSERT_TRUE(stale.ok()) << stale.error.toStdString();
    auto changedShared = rows;
    changedShared.append(QJsonObject{{"targetBusID", static_cast<qint64>(other->objectID)},
        {"mode", 2}, {"sendLevel", -6.0}});
    ASSERT_TRUE(restoreBusSendState(*source, {source->guid, {}, {true, changedShared}}));
    const auto changed = snapshot();
    EXPECT_FALSE(applySignalPathConnect(std::move(stale)));
    EXPECT_EQ(snapshot(), changed);
}

TEST_F(OutputSendExclusionTest, CombinedGraphRouteAndParentEditsReplayCompleteScopedState)
{
    using namespace VNX;
    auto& manager = ProjectManager::instance();
    auto& undo = UndoRedoManager::instance();
    auto* master = manager.getMasterBusItem();
    auto* parent = manager.createItem(master, QStringLiteral("Parent"), ProjectItem::Type::Bus);
    ASSERT_NE(parent, nullptr);
    auto* source = manager.createItem(parent, QStringLiteral("Source"), ProjectItem::Type::Bus);
    auto* other = manager.createItem(master, QStringLiteral("Other"), ProjectItem::Type::Bus);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(other, nullptr);
    const auto windows = manager.currentProject()->platforms[0].id;
    const QJsonArray rows{
        QJsonObject{{"targetBusID", static_cast<qint64>(master->objectID)}, {"mode", 0}, {"sendLevel", -6.0},
                    {"modeVolumes", QJsonObject{{"2", -12.0}}}},
        QJsonObject{{"targetBusID", static_cast<qint64>(other->objectID)}, {"mode", 0}, {"sendLevel", -9.0}}};
    ASSERT_TRUE(restoreBusSendState(*source, {source->guid, {}, {true, rows}}));
    PlatformContextService::instance().setSelected({windows}, windows, false);
    const auto snapshot = [&] {
        std::vector<BusSendState> states;
        for (const auto& scope : busRoutingScopes(*manager.currentProject()))
            states.push_back(*captureBusSendState(*source, scope));
        return states;
    };
    const auto before = snapshot();
    for (const auto* removedTarget : {master, other}) {
        SCOPED_TRACE(removedTarget->name.toStdString());
        const auto graph = buildSignalPathGraph(*manager.currentProject(), {}, toFieldGroupGuid(windows),
            {source->objectID, other->objectID});
        ASSERT_TRUE(graph.ok());
        std::vector<size_t> selected;
        for (size_t index = 0; index < graph.edges.size(); ++index) {
            const auto& edge = graph.edges[index];
            if (graph.nodes[edge.from.occurrenceIndex].objectID != source->objectID) continue;
            if (edge.kind == SignalPathEdgeKind::Output
                || (edge.kind == SignalPathEdgeKind::Send
                    && graph.nodes[edge.to.occurrenceIndex].objectID == removedTarget->objectID))
                selected.push_back(index);
        }
        ASSERT_EQ(selected.size(), 2u);
        auto plan = prepareSignalPathDisconnect(graph, selected);
        ASSERT_TRUE(plan.ok()) << plan.error.toStdString();
        undo.clear();
        ASSERT_TRUE(applySignalPathConnect(std::move(plan)));
        EXPECT_EQ(source->parent, master);
        EXPECT_TRUE(validateBusRouting(*manager.currentProject()).isEmpty());
        const auto after = snapshot();
        ASSERT_TRUE(undo.undo());
        EXPECT_EQ(source->parent, parent);
        EXPECT_EQ(snapshot(), before);
        ASSERT_TRUE(undo.redo());
        EXPECT_EQ(source->parent, master);
        EXPECT_EQ(snapshot(), after);
        ASSERT_TRUE(undo.undo());
        EXPECT_EQ(source->parent, parent);
        EXPECT_EQ(snapshot(), before);
    }
}
