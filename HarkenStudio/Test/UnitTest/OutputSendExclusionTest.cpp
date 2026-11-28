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

#include <cmath>

namespace {

void ensureApp()
{
    if (qApp) return;
    static int argc = 1;
    static char arg0[] = "harken_studio_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

class OutputSendExclusionTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        auto& pm = harken::studio::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        harken::studio::UndoRedoManager::instance().clear();
        harken::studio::PlatformContextService::instance().setStorageForTesting(
            std::make_unique<harken::studio::MemorySettingsStorage>());
        ASSERT_TRUE(tempDir.isValid());
        harken::studio::ProjectConfig config;
        config.name = QStringLiteral("OutputSendExclusionProject");
        config.location = tempDir.path();
        config.platforms = {
            {QUuid::createUuid(), QStringLiteral("Windows"), harken::studio::BasePlatform::Windows},
            {QUuid::createUuid(), QStringLiteral("Android"), harken::studio::BasePlatform::Android}};
        ASSERT_TRUE(pm.createProject(config));
    }

    void TearDown() override
    {
        auto& pm = harken::studio::ProjectManager::instance();
        if (pm.isProjectOpen())
            pm.closeProject();
        harken::studio::UndoRedoManager::instance().clear();
        harken::studio::ActionManager::instance().clearAllForTesting();
    }

    QTemporaryDir tempDir;
};

harken::studio::BusRoute route(uint32_t target, float sendLevelDb,
                               std::map<QUuid, harken::studio::BusRouteCells> platforms = {},
                               bool duckTarget = false)
{
    return {QUuid::createUuid(), target, sendLevelDb, duckTarget, std::move(platforms)};
}

}

TEST_F(OutputSendExclusionTest, MasterKeepsItsOwnUuidAcrossSaveAndOpen)
{
    auto& pm = harken::studio::ProjectManager::instance();
    harken::studio::ProjectItem* master = pm.findBusItem(harken::studio::kMasterBusID);
    ASSERT_NE(master, nullptr);

    const auto masterId = master->runtimeId();
    ASSERT_TRUE(pm.saveProject());
    const auto projectPath = pm.currentProject()->projectPath;
    pm.closeProject();
    ASSERT_TRUE(pm.loadProject(projectPath));
    EXPECT_EQ(pm.getMasterBusItem()->runtimeId(), masterId);
    pm.closeProject();
    harken::studio::ProjectConfig config;
    config.name = QStringLiteral("IndependentMasterProject");
    config.location = tempDir.path();
    ASSERT_TRUE(pm.createProject(config));
    EXPECT_NE(pm.getMasterBusItem()->runtimeId(), masterId);
}

TEST_F(OutputSendExclusionTest, RouteCommandsRestoreCompleteAuthoredProperty)
{
    using namespace harken::studio;
    auto& manager = ProjectManager::instance();
    auto* source = manager.createItem(manager.getMasterBusItem(), QStringLiteral("Source"),
                                      ProjectItem::Type::Bus);
    auto* target = manager.createItem(manager.getMasterBusItem(), QStringLiteral("Target"),
                                      ProjectItem::Type::Bus);
    ASSERT_NE(source, nullptr);
    ASSERT_NE(target, nullptr);
    ASSERT_TRUE(restoreBusSendState(*source, {source->guid, {route(target->objectID, -9.0f)},
                                              {route(target->objectID, -3.0f)}}));
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

TEST_F(OutputSendExclusionTest, UnsupportedCommandFieldsRejectBeforeAuthoringOrAuditionMutation)
{
    using namespace harken::studio;
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
    using namespace harken::studio;
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
    ASSERT_TRUE(restoreBusSendState(*source, {source->guid, {route(other->objectID, -6.0f)}, {}}));
    ASSERT_TRUE(restoreBusSendState(*target, {target->guid,
        {route(source->objectID, -9.0f)}, {}}));
    ASSERT_TRUE(validateBusRouting(*manager.currentProject()).isEmpty());
    const auto snapshot = [&] {
        std::vector<BusSendState> states;
        for (const auto* item : {source, target, other}) states.push_back(*captureBusSendState(*item));
        return states;
    };
    const auto before = snapshot();
    actions.clearAllForTesting();
    registerProjectActions(actions, &manager, nullptr);
    undo.clear();
    const auto rowId = before[0].sends.front().id.toString(QUuid::WithoutBraces);
    QJsonObject request{{"authoring_handle", static_cast<qint64>(source->objectID)},
        {"property", "sends"}, {"value", QJsonArray{QJsonObject{{"id", rowId},
            {"targetBusID", static_cast<qint64>(target->objectID)}, {"sendLevel", -3.0}}}}};
    for (const bool platformLanding : {true, false}) {
        SCOPED_TRACE(platformLanding);
        auto candidate = request;
        if (platformLanding)
            candidate.insert(QStringLiteral("platform_ids"), QJsonArray{android.toString(QUuid::WithoutBraces)});
        const int count = undo.undoStack()->count();
        const int index = undo.undoStack()->index();
        const auto result = actions.execute(QStringLiteral("set_item_property"), candidate);
        EXPECT_FALSE(result.success);
        EXPECT_EQ(snapshot(), before);
        EXPECT_EQ(undo.undoStack()->count(), count);
        EXPECT_EQ(undo.undoStack()->index(), index);
        EXPECT_TRUE(validateBusRouting(*manager.currentProject()).isEmpty());
    }
    const QJsonArray accepted{QJsonObject{{"id", rowId},
        {"targetBusID", static_cast<qint64>(other->objectID)}, {"sendLevel", -6.0},
        {"platforms", QJsonObject{{android.toString(QUuid::WithoutBraces),
            QJsonObject{{"sendLevel", -12.0}}}}}}};
    request.insert(QStringLiteral("value"), accepted);
    const auto result = actions.execute(QStringLiteral("set_item_property"), request);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_EQ(effectiveBusSends(*source, android)->front().targetBusID, other->objectID);
    EXPECT_FLOAT_EQ(effectiveBusSends(*source, android)->front().sendLevel, std::pow(10.0f, -12.0f / 20.0f));
    EXPECT_FLOAT_EQ(effectiveBusSends(*source, windows)->front().sendLevel, std::pow(10.0f, -6.0f / 20.0f));
    EXPECT_TRUE(validateBusRouting(*manager.currentProject()).isEmpty());
    const auto after = snapshot();
    ASSERT_EQ(undo.undoStack()->count(), 1);
    ASSERT_TRUE(undo.undo());
    EXPECT_EQ(snapshot(), before);
    ASSERT_TRUE(undo.redo());
    EXPECT_EQ(snapshot(), after);
}

TEST_F(OutputSendExclusionTest, CombinedGraphRouteAndParentEditsReplayCompleteScopedState)
{
    using namespace harken::studio;
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
    ASSERT_TRUE(restoreBusSendState(*source, {source->guid,
        {route(master->objectID, -6.0f), route(other->objectID, -9.0f)}, {}}));
    PlatformContextService::instance().setSelected({windows}, windows, false);
    const auto snapshot = [&] { return *captureBusSendState(*source); };
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
