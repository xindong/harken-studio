#include "Core/ActionManager.h"
#include "Core/Components/VConversion.h"
#include "Core/ConversionEditing.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/IPC/IPCServer.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/PlatformContextService.h"
#include "Core/PreviewMediaModeService.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectSerializer.h"
#include "Core/UndoCommands.h"

#include <gtest/gtest.h>
#include <QApplication>
#include <QEvent>
#include <QTemporaryDir>

namespace {
using namespace harken::studio;
namespace Platforms = ProjectPlatformSemantics;

class ProjectPlatformCommand : public testing::Test
{
protected:
    void SetUp() override
    {
        if (!qApp) {
            static int argc = 1;
            static char name[] = "harken_studio_tests";
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
        QJsonObject defaults;
        for (auto it = project().defaultConversionRefs.cbegin(); it != project().defaultConversionRefs.cend(); ++it)
            defaults.insert(Platforms::idString(it.key()), it.value().toString(QUuid::WithoutBraces));
        return {{"platforms", Platforms::platformsToJson(project().platforms)}, {"items", items},
            {"defaultConversions", defaults},
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

TEST_F(ProjectPlatformCommand, ConversionCommandsValidateAtomicallyAndReplayWithAuthoredState)
{
    auto& actions = ActionManager::instance();
    const auto created = actions.execute("create_conversion", {{"name", "CommandPreset"}}, ActionSource::IPC);
    ASSERT_TRUE(created.success) << created.errorMessage.toStdString();
    const auto guid = QUuid(created.data.value("guid").toString());
    auto* preset = manager().findItemByGuid(guid);
    ASSERT_NE(preset, nullptr);
    const auto initial = preset->comp<VConversion>()->config;
    const auto platform = Platforms::idString(project().platforms.front().id);
    const QJsonObject edit{{"conversion", guid.toString(QUuid::WithoutBraces)},
        {"rules", QJsonArray{QJsonObject{{"max_bytes", 1024}, {"sample_rate", 0}, {"format", "PCM"}},
                             QJsonObject{{"max_bytes", QJsonValue::Null}, {"sample_rate", 24000}, {"format", "Vorbis"}, {"quality", 6}}}}};
    undo().clear();
    const auto changed = actions.execute("set_conversion_fields", edit, ActionSource::IPC);
    ASSERT_TRUE(changed.success) << changed.errorMessage.toStdString();
    const auto updated = preset->comp<VConversion>()->config;
    EXPECT_NE(updated, initial);
    EXPECT_EQ(undo().undoStack()->count(), 1);
    auto invalid = edit;
    invalid.insert("platform_ids", QJsonArray{platform});
    EXPECT_FALSE(actions.execute("set_conversion_fields", invalid, ActionSource::IPC).success);
    invalid = edit;
    invalid.insert("rules", QJsonArray{QJsonObject{{"max_bytes", 1024}, {"sample_rate", 24000}, {"format", "PCM"}}});
    EXPECT_FALSE(actions.execute("set_conversion_fields", invalid, ActionSource::IPC).success);
    EXPECT_EQ(preset->comp<VConversion>()->config, updated);
    EXPECT_EQ(undo().undoStack()->count(), 1);
    ASSERT_TRUE(undo().undo());
    EXPECT_EQ(preset->comp<VConversion>()->config, initial);
    ASSERT_TRUE(undo().redo());
    EXPECT_EQ(preset->comp<VConversion>()->config, updated);
    preset->comp<VConversion>()->config.rules.back().vorbisQuality = 9;
    const auto externallyChanged = preset->comp<VConversion>()->config;
    EXPECT_FALSE(undo().undo());
    EXPECT_EQ(preset->comp<VConversion>()->config, externallyChanged);
    preset->comp<VConversion>()->config = updated;
    ASSERT_TRUE(undo().undo());
    EXPECT_EQ(preset->comp<VConversion>()->config, initial);
    ASSERT_TRUE(undo().redo());
    ASSERT_TRUE(manager().saveProject());
    reopen();
    ASSERT_NE(manager().findItemByGuid(guid), nullptr);
    EXPECT_EQ(manager().findItemByGuid(guid)->comp<VConversion>()->config, updated);
}

TEST_F(ProjectPlatformCommand, ConversionBatchEditsRejectStaleMembersWithoutPartialWrites)
{
    auto* layer = manager().createItem(nullptr, "ConversionLayer", ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    auto* first = manager().createItem(layer, "First", ProjectItem::Type::Sound);
    auto* second = manager().createItem(nullptr, "Second", ProjectItem::Type::Sound);
    auto* preset = manager().createItem(nullptr, "BatchPreset", ProjectItem::Type::Conversion);
    ASSERT_NE(first, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(preset, nullptr);
    const QVector<QUuid> definitions{first->guid, second->guid};
    const QVector<FieldGroupDefinitionSnapshot> before{{first->guid, first->fieldGroups}, {second->guid, second->fieldGroups}};
    undo().clear();
    QString error;
    ASSERT_TRUE(ConversionEditing::setObjects(manager(), definitions, false, preset->guid, {}, before, nullptr, &error))
        << error.toStdString();
    const auto changed = authoredState();
    EXPECT_EQ(undo().undoStack()->count(), 1);
    EXPECT_FALSE(ConversionEditing::setObjects(manager(), definitions, true, {}, {}, before, nullptr, &error));
    EXPECT_EQ(authoredState(), changed);
    ASSERT_TRUE(undo().undo());
    EXPECT_EQ(first->fieldGroups, before.front().groups);
    EXPECT_EQ(second->fieldGroups, before.back().groups);
    ASSERT_TRUE(undo().redo());
    EXPECT_EQ(authoredState(), changed);
    EXPECT_FALSE(ConversionEditing::setObjects(manager(), {first->guid, preset->guid}, false,
        QUuid{}, {}, {}, nullptr, &error));
    EXPECT_EQ(authoredState(), changed);
    auto& actions = ActionManager::instance();
    const auto effective = actions.execute("get_effective_conversion", {{"object", first->guid.toString(QUuid::WithoutBraces)}});
    ASSERT_TRUE(effective.success) << effective.errorMessage.toStdString();
    EXPECT_EQ(effective.data.value("conversion").toString(), preset->guid.toString(QUuid::WithoutBraces));
    const auto platform = project().platforms.front().id;
    const auto previousDefault = project().defaultConversionFor(platform);
    ASSERT_TRUE(actions.execute("set_default_conversion", {{"platform_id", Platforms::idString(platform)},
        {"conversion", preset->guid.toString(QUuid::WithoutBraces)}}).success);
    ASSERT_TRUE(undo().undo());
    EXPECT_EQ(project().defaultConversionFor(platform), previousDefault);
    ASSERT_TRUE(undo().redo());
    EXPECT_EQ(project().defaultConversionFor(platform), preset->guid);
}

TEST_F(ProjectPlatformCommand, LocalPreviewModeFollowsProjectIdentityBeforeOpenedNotification)
{
    auto storage = std::make_unique<MemorySettingsStorage>();
    auto* persisted = storage.get();
    PreviewMediaModeService preview(std::move(storage));
    const auto firstPath = project().projectPath;
    const auto firstId = project().projectId;
    const auto initial = authoredState();
    preview.setMode(PreviewMediaMode::Converted);
    EXPECT_EQ(preview.mode(), PreviewMediaMode::Converted);
    EXPECT_EQ(authoredState(), initial);
    ASSERT_TRUE(manager().saveProject());

    ProjectConfig config;
    config.name = QStringLiteral("SecondPreviewProject");
    config.location = directory.path();
    config.platforms = {{QUuid::createUuid(), QStringLiteral("Windows"), BasePlatform::Windows}};
    auto second = ProjectManager::prepareProject(config);
    ASSERT_NE(second.project, nullptr) << second.error.toStdString();
    const auto secondPath = second.project->projectPath;
    const auto secondId = second.project->projectId;
    ASSERT_TRUE(manager().commitPreparedProject(std::move(second)));
    EXPECT_NE(secondId, firstId);
    EXPECT_EQ(preview.mode(), PreviewMediaMode::Original);
    preview.setMode(PreviewMediaMode::Converted);
    preview.setMode(PreviewMediaMode::Original);
    ASSERT_TRUE(manager().closeProject());
    EXPECT_EQ(preview.mode(), PreviewMediaMode::Original);
    const auto saved = *persisted;
    preview.setMode(PreviewMediaMode::Converted);
    EXPECT_EQ(preview.mode(), PreviewMediaMode::Original);
    EXPECT_EQ(persisted->allKeys(), saved.allKeys());
    for (const auto& key : saved.allKeys()) EXPECT_EQ(persisted->value(key), saved.value(key));

    auto first = ProjectManager::prepareProject(firstPath);
    ASSERT_NE(first.project, nullptr) << first.error.toStdString();
    ASSERT_TRUE(manager().commitPreparedProject(std::move(first)));
    EXPECT_EQ(preview.mode(), PreviewMediaMode::Converted);
    manager().publishProjectOpened();
    EXPECT_EQ(preview.mode(), PreviewMediaMode::Converted);
    PreviewMediaModeService restored(std::make_unique<MemorySettingsStorage>(*persisted));
    EXPECT_EQ(restored.mode(), PreviewMediaMode::Converted);

    ASSERT_TRUE(manager().closeProject());
    second = ProjectManager::prepareProject(secondPath);
    ASSERT_NE(second.project, nullptr) << second.error.toStdString();
    ASSERT_TRUE(manager().commitPreparedProject(std::move(second)));
    preview.setMode(PreviewMediaMode::Converted);
    EXPECT_EQ(preview.mode(), PreviewMediaMode::Converted);
    ASSERT_TRUE(manager().closeProject());
    first = ProjectManager::prepareProject(firstPath);
    ASSERT_NE(first.project, nullptr) << first.error.toStdString();
    ASSERT_TRUE(manager().commitPreparedProject(std::move(first)));
    preview.setMode(PreviewMediaMode::Original);
    EXPECT_EQ(preview.mode(), PreviewMediaMode::Original);
    persisted->beginGroup(QStringLiteral("PreviewMedia/%1").arg(secondId));
    EXPECT_EQ(persisted->value(QStringLiteral("mode")).toString(), QStringLiteral("Converted"));
    persisted->endGroup();
}

TEST_F(ProjectPlatformCommand, PlatformMembershipEditsRoundTripAllAuthoredOwners)
{
    auto* preset = manager().createItem(nullptr, "Custom", ProjectItem::Type::Conversion);
    auto* layer = manager().createItem(nullptr, "Layer", ProjectItem::Type::Layer);
    ASSERT_NE(preset, nullptr);
    ASSERT_NE(layer, nullptr);
    auto* conversion = preset->comp<VConversion>();
    ASSERT_NE(conversion, nullptr);
    conversion->config.rules = {{1024, 32000}, {std::nullopt, 22050}};
    layer->fieldGroups.conversion.shared.localHistory = ConversionFieldGroup{toFieldGroupGuid(preset->guid)};
    layer->fieldGroups.output.platforms[toFieldGroupGuid(project().platforms.front().id)] = {
        FieldGroupMode::UseShared, OutputFieldGroup{toFieldGroupGuid(manager().getMasterBusItem()->guid), -6.0f},
        FieldGroupPlatformMask(FieldGroupId::Output)};
    undo().clear();
    const auto initial = authoredState();
    const auto create = ActionManager::instance().execute("create_project_platform",
        {{"name", "Mobile"}, {"base_platform", "Android"}}, ActionSource::IPC);
    ASSERT_TRUE(create.success) << create.errorMessage.toStdString();
    QString error;
    ASSERT_TRUE(Platforms::validateProject(project(), &error)) << error.toStdString();
    const auto created = authoredState();
    EXPECT_NE(created, initial);
    ASSERT_TRUE(undo().undo());
    EXPECT_EQ(authoredState(), initial);
    ASSERT_TRUE(undo().redo());
    EXPECT_EQ(authoredState(), created);

    const auto platform = create.data.value("platform_id");
    const auto changeBase = ActionManager::instance().execute("set_project_platform_base",
        {{"platform_id", platform}, {"base_platform", "Linux"}}, ActionSource::IPC);
    ASSERT_TRUE(changeBase.success) << changeBase.errorMessage.toStdString();
    const auto changed = authoredState();
    auto expected = created;
    expected.insert("platforms", changed.value("platforms"));
    EXPECT_EQ(changed, expected);
    ASSERT_TRUE(undo().undo());
    EXPECT_EQ(authoredState(), created);
    ASSERT_TRUE(undo().redo());
    EXPECT_EQ(authoredState(), changed);

    const auto remove = ActionManager::instance().execute("delete_project_platform",
        {{"platform_id", platform}}, ActionSource::IPC);
    ASSERT_TRUE(remove.success) << remove.errorMessage.toStdString();
    ASSERT_TRUE(Platforms::validateProject(project(), &error)) << error.toStdString();
    const auto removed = authoredState();
    ASSERT_TRUE(undo().undo());
    EXPECT_EQ(authoredState(), changed);
    ASSERT_TRUE(undo().redo());
    EXPECT_EQ(authoredState(), removed);
    ASSERT_TRUE(undo().undo());
    reopen();
    EXPECT_EQ(authoredState(), changed);
}

TEST_F(ProjectPlatformCommand, DeletingReferencedAssetsRestoresDefaultsAndInheritedHistory)
{
    auto* preset = manager().createItem(nullptr, "Doomed", ProjectItem::Type::Conversion);
    ASSERT_NE(preset, nullptr);
    const auto presetGuid = preset->guid;
    const auto platform = project().platforms.front().id;
    ASSERT_TRUE(manager().setDefaultConversion(platform, presetGuid));
    auto* layer = manager().createItem(nullptr, "Layer", ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    ASSERT_TRUE(layer->fieldGroups.conversion.platforms.contains(toFieldGroupGuid(platform)));
    auto* inherited = manager().createItem(layer, "Inherited", ProjectItem::Type::Sound);
    auto* local = manager().createItem(layer, "Local", ProjectItem::Type::Sound);
    ASSERT_NE(inherited, nullptr);
    ASSERT_NE(local, nullptr);
    const ConversionFieldGroup reference{toFieldGroupGuid(presetGuid)};
    inherited->fieldGroups.conversion.shared.localHistory = reference;
    local->fieldGroups.conversion.shared = {FieldGroupMode::Local, reference};
    undo().clear();
    const auto before = authoredState();
    ASSERT_TRUE(manager().deleteItem(preset));
    EXPECT_EQ(manager().findItemByGuid(presetGuid), nullptr);
    EXPECT_TRUE(project().defaultConversionFor(platform).isNull());
    const auto effective = buildFieldGroupEffectiveSnapshot(project(), toFieldGroupGuid(platform));
    ASSERT_TRUE(effective.ok());
    for (const auto* item : {layer, inherited, local})
        EXPECT_TRUE(harken::studio::project_schema::IsNull(
            effective.definitions.at(toFieldGroupGuid(item->guid)).conversion.settingsRef));
    const auto deleted = authoredState();
    ASSERT_TRUE(manager().saveProject());
    ASSERT_TRUE(undo().undo());
    EXPECT_EQ(authoredState(), before);
    ASSERT_TRUE(undo().redo());
    EXPECT_EQ(authoredState(), deleted);
    ASSERT_TRUE(undo().undo());
    reopen();
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
