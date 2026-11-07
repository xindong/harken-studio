#include "Core/ActionManager.h"
#include "Core/IPC/IPCServer.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectSerializer.h"
#include "Core/UndoCommands.h"

#include <gtest/gtest.h>
#include <QApplication>
#include <QEvent>
#include <QTemporaryDir>

namespace {
using namespace VNX;
namespace Platforms = ProjectPlatformSemantics;

class ProjectPlatformCommand : public testing::Test
{
protected:
    void SetUp() override
    {
        if (!qApp) {
            static int argc = 1;
            static char name[] = "harken_editor_tests";
            static char* argv[] = {name, nullptr};
            static QApplication app(argc, argv);
        }
        if (manager().isProjectOpen()) manager().closeProject();
        PlatformContextService::instance().setStorageForTesting(std::make_unique<MemorySettingsStorage>());
        ASSERT_TRUE(directory.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("PlatformCommands");
        config.location = directory.path();
        config.platforms = {{QUuid::createUuid(), QStringLiteral("Windows"), BasePlatform::Windows}};
        ASSERT_TRUE(manager().createProject(config));
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(), &manager(), nullptr);
        undo().clear();
    }

    void TearDown() override
    {
        undo().clear();
        ActionManager::instance().clearAllForTesting();
        if (manager().isProjectOpen()) manager().closeProject();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
    }

    ProjectManager& manager() { return ProjectManager::instance(); }
    ProjectData& project() { return *manager().currentProject(); }
    UndoRedoManager& undo() { return UndoRedoManager::instance(); }
    QJsonObject authoredState()
    {
        ProjectSerializer serializer;
        EXPECT_TRUE(serializer.prepareReferenceWrite(project()));
        QJsonObject items;
        for (const auto* item : Platforms::referenceOwners(project()))
            items.insert(item->guid.toString(), serializer.projectItemToJson(*item));
        QJsonArray targeting;
        for (const auto& row : manager().getActionPackTargeting()) targeting.append(row.toJson());
        return {{"platforms", Platforms::platformsToJson(project().platforms)}, {"items", items},
            {"targeting", targeting},
            {"current", manager().getPackExportCurrentPlatform()},
            {"selected", QJsonArray::fromStringList(manager().getPackExportSelectedPlatforms())}};
    }
    void reopen()
    {
        const auto path = project().projectPath;
        ASSERT_TRUE(manager().saveProject());
        manager().closeProject();
        auto prepared = ProjectManager::prepareProject(path);
        ASSERT_NE(prepared.project, nullptr) << prepared.error.toStdString();
        ASSERT_TRUE(manager().commitPreparedProject(std::move(prepared)));
    }

    QTemporaryDir directory;
};

TEST_F(ProjectPlatformCommand, OldCommandCannotReplayIntoReopenedProjectWithSameIds)
{
    const auto lease = manager().currentProjectLease();
    const auto* command = ActionManager::instance().findAction("create_project_platform");
    ASSERT_NE(command, nullptr);
    const auto result = command->handler({{"name", "Android"}, {"base_platform", "Android"}});
    ASSERT_TRUE(result.success);
    ASSERT_TRUE(result.canReplay());
    reopen();
    EXPECT_NE(manager().currentProject(), lease.get());
    const auto before = authoredState();
    EXPECT_FALSE(result.canReplay());
    result.undoFn();
    EXPECT_EQ(authoredState(), before);
    result.redoFn();
    EXPECT_EQ(authoredState(), before);
}

TEST_F(ProjectPlatformCommand, TransactionRollbackPreservesAuthoredPropertiesAcrossScopes)
{
    auto& actions = ActionManager::instance();
    registerMixerActions(actions, nullptr, &manager(), nullptr);
    registerContainerActions(actions, &manager());
    IPCServer server(nullptr, nullptr, CommandHost::Cli);
    server.setProjectManager(&manager());
    server.setAppState(IPCServer::AppState::Ready);
    const auto created = server.dispatchCommand("create_bus", {{"name", "TransactionBus"}});
    ASSERT_EQ(created.value("status"), "ok") << QJsonDocument(created).toJson().toStdString();
    auto* bus = manager().findBusItem(static_cast<uint32_t>(
        created.value("result").toObject().value("bus_id").toInteger()));
    auto* container = manager().createItem(nullptr, "TransactionContainer", ProjectItem::Type::Container);
    ASSERT_NE(bus, nullptr);
    ASSERT_NE(container, nullptr);
    const auto busLocator = PlayablePropertyAuthoringV1::definitionLocator(*bus);
    const auto containerLocator = PlayablePropertyAuthoringV1::definitionLocator(*container);
    ASSERT_TRUE(busLocator);
    ASSERT_TRUE(containerLocator);
    const auto platformId = project().platforms.front().id;
    const QJsonObject busPatch{{"volume", -9.0}, {"pitch", 240.0}, {"outputBusVolume", -12.0},
        {"pan", 0.5}, {"mute", true}, {"solo", true}};
    const QJsonObject containerPatch{{"volume", -15.0}, {"pitch", 360.0}};
    auto snapshot = [&] {
        return QJsonObject{{"authored", authoredState()},
            {"mute", manager().auditionOverlay().isMuted(bus->runtimeId())},
            {"solo", manager().auditionOverlay().isSoloed(bus->runtimeId())}};
    };
    for (int initial = 0; initial < 3; ++initial) {
        SCOPED_TRACE(initial);
        for (auto* item : {bus, container}) {
            const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*item);
            for (const auto* name : {"volume", "pitch", "outputBusVolume"}) {
                if (item == container && QString::fromLatin1(name) == "outputBusVolume") continue;
                const auto* field = PlayablePropertyAuthoringV1::fieldByName(QString::fromLatin1(name));
                ASSERT_NE(field, nullptr);
                ASSERT_TRUE(PlayablePropertyAuthoringV1::clearOverride(*item, *locator, field->id, std::nullopt));
                ASSERT_TRUE(PlayablePropertyAuthoringV1::clearOverride(*item, *locator, field->id, platformId));
                if (initial > 0)
                    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(*item, *locator, field->id, -3.0, std::nullopt));
                if (initial > 1)
                    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(*item, *locator, field->id, -6.0, platformId));
            }
        }
        const auto initialized = server.dispatchCommand("set_bus_properties", {
            {"bus_id", static_cast<qint64>(bus->objectID)}, {"pan", initial ? -0.25 : 0.0},
            {"mute", initial != 0}, {"solo", initial != 0}});
        ASSERT_EQ(initialized.value("status"), "ok") << QJsonDocument(initialized).toJson().toStdString();
        QVector<QJsonArray> cases;
        QJsonArray combined;
        QJsonObject allBusFields{{"bus_id", static_cast<qint64>(bus->objectID)}};
        for (auto it = busPatch.begin(); it != busPatch.end(); ++it) {
            const QJsonValue value = it.value().isBool() ? QJsonValue(initial == 0) : it.value();
            const QJsonObject request{{"cmd", "set_bus_properties"},
                {"args", QJsonObject{{"bus_id", static_cast<qint64>(bus->objectID)}, {it.key(), value}}}};
            cases.append(QJsonArray{request});
            allBusFields.insert(it.key(), value);
        }
        combined.append(QJsonObject{{"cmd", "set_bus_properties"}, {"args", allBusFields}});
        for (auto it = containerPatch.begin(); it != containerPatch.end(); ++it) {
            const QJsonObject request{{"cmd", "set_container_property"},
                {"args", QJsonObject{{"container_path", container->guid.toString(QUuid::WithoutBraces)},
                    {"property", it.key()}, {"value", it.value()}}}};
            cases.append(QJsonArray{request});
            combined.append(request);
        }
        cases.append(combined);
        for (const auto& requests : cases) {
            SCOPED_TRACE(QJsonDocument(requests).toJson(QJsonDocument::Compact).toStdString());
            const auto before = snapshot();
            const auto preview = server.dispatchCommand("preview_actions", {{"actions", requests}});
            ASSERT_EQ(preview.value("status"), "ok") << QJsonDocument(preview).toJson().toStdString();
            EXPECT_EQ(snapshot(), before);
            const QJsonObject transaction{{"tx_id", preview.value("result").toObject().value("tx_id")}};
            const auto confirmed = server.dispatchCommand("confirm_actions", transaction);
            ASSERT_EQ(confirmed.value("status"), "ok") << QJsonDocument(confirmed).toJson().toStdString();
            const auto applied = server.dispatchCommand("apply_actions", transaction);
            ASSERT_EQ(applied.value("status"), "ok") << QJsonDocument(applied).toJson().toStdString();
            EXPECT_NE(snapshot(), before);
            const auto rolledBack = server.dispatchCommand("rollback_actions", transaction);
            ASSERT_EQ(rolledBack.value("status"), "ok") << QJsonDocument(rolledBack).toJson().toStdString();
            EXPECT_EQ(snapshot(), before);
        }
    }
    const QJsonObject unavailable{{"spatializationMix", 0.5}, {"attenuationDistanceScaling", 2.0}, {"is3DEnabled", true}};
    for (auto it = unavailable.begin(); it != unavailable.end(); ++it) {
        const QJsonObject args{{"container_path", container->guid.toString(QUuid::WithoutBraces)},
            {"property", it.key()}, {"value", it.value()}};
        const auto before = snapshot();
        EXPECT_EQ(server.dispatchCommand("set_container_property", args).value("status"), "error");
        EXPECT_EQ(server.dispatchCommand("preview_actions", {{"actions", QJsonArray{
            QJsonObject{{"cmd", "set_container_property"}, {"args", args}}}}}).value("status"), "error");
        EXPECT_EQ(snapshot(), before);
    }
}

}
