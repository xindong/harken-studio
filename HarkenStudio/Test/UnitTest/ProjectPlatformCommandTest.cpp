#include "Core/Components/VSelection.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/ActionManager.h"
#include "Core/AssetPipeline/AssetConverter.h"
#include "Core/Components/VTrack.h"
#include "Core/IPC/IPCServer.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/PackGenerationRecord.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectPackExporter.h"
#include "Core/ProjectPlatformActivity.h"
#include "Core/ProjectSerializer.h"
#include "Core/PropertyEditSession.h"
#include "Core/SoundSourceEditing.h"
#include "Core/UndoCommands.h"

#include <gtest/gtest.h>
#include <QAction>
#include <QApplication>
#include <QEvent>
#include <QFile>
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
        ASSERT_TRUE(manager().createProject(config));
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(), &manager(), nullptr, {});
        undo().clear();
        PackGenerationRecordStore::instance().clear();
    }

    void TearDown() override
    {
        PropertyEditSession::finishAllActive(false);
        undo().clear();
        ActionManager::instance().clearAllForTesting();
        if (manager().isProjectOpen()) manager().closeProject();
        PackGenerationRecordStore::instance().clear();
        QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
        EXPECT_FALSE(ProjectPlatformActivity::instance().isBusy());
    }

    ProjectManager& manager() { return ProjectManager::instance(); }
    ProjectData& project() { return *manager().currentProject(); }
    UndoRedoManager& undo() { return UndoRedoManager::instance(); }
    ActionResult run(const QString& command, const QJsonObject& args = {})
    {
        return ActionManager::instance().execute(command, args, ActionSource::IPC);
    }
    QString create(const QString& name, const QString& base = QStringLiteral("Android"))
    {
        const auto result = run("create_project_platform", {{"name", name}, {"base_platform", base}});
        EXPECT_TRUE(result.success) << result.errorMessage.toStdString();
        return result.data.value("platform_id").toString();
    }
    void clearDirty()
    {
        for (auto* item : Platforms::referenceOwners(project())) item->clearDirty();
    }
    QJsonObject authoredState()
    {
        ProjectSerializer serializer;
        EXPECT_TRUE(serializer.prepareReferenceWrite(project()));
        QJsonObject items;
        for (const auto* item : Platforms::referenceOwners(project()))
            items.insert(item->guid.toString(), serializer.projectItemToJson(*item));
        QJsonArray groups;
        for (const auto& group : project().packGroups) groups.append(group.toJson());
        QJsonArray targeting;
        for (const auto& row : project().actionPackTargeting) targeting.append(row.toJson());
        QJsonArray deployments;
        for (const auto& row : project().deploymentProfiles)
            deployments.append(QJsonObject{{"id", row.profileId}, {"name", row.name},
                {"destination", row.destination}, {"platformIds", QJsonArray::fromStringList(row.platformIds)},
                {"languages", QJsonArray::fromStringList(row.languages)}});
        return {{"platforms", Platforms::platformsToJson(project().platforms)}, {"items", items},
            {"groups", groups}, {"targeting", targeting}, {"deployments", deployments},
            {"current", project().packExportCurrentPlatformId},
            {"selected", QJsonArray::fromStringList(project().packExportSelectedPlatformIds)}};
    }
    void setOverlay(PlatformOverrideLedgerV1& ledger, const QString& id, float value)
    {
        const auto property = harkenplayableproperties::PlayablePropertyIdV1::Volume;
        auto* row = ledger.ensure(QUuid(id));
        ASSERT_TRUE(row->properties.setValue(property, value));
        row->properties.presence.Set(property);
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

TEST_F(ProjectPlatformCommand, CreatesEveryBaseAndIndependentSiblingsWithoutCopyingAuthoredData)
{
    const auto windows = project().platforms.first().id;
    const auto first = create("Android_Low");
    auto* sound = manager().createItem(nullptr, "Sound", ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    setOverlay(sound->platformOverrides, first, 0.25f);
    const auto entry = QUuid::createUuid();
    auto* sources = sound->comp<VSound>();
    sources->sources.plain.entries = {{entry, 11}};
    sources->sources.plain.platforms[first].active = entry;
    sources->sources.plain.active = entry;
    project().packGroups = {{"Explicit", {}, false, {first}}, {"All", {}, true, {}}};
    PlatformContextService::instance().setSelected({QUuid(first)}, QUuid(first), false);
    const auto second = create("Android_High");
    EXPECT_NE(first, second);
    EXPECT_EQ(project().platforms.last().basePlatform, BasePlatform::Android);
    EXPECT_EQ(project().platforms.last().profile.toJson(), ProjectPlatformProfile::fromBasePlatform(BasePlatform::Android).toJson());
    EXPECT_EQ(sound->platformOverrides.find(QUuid(second)), nullptr);
    EXPECT_EQ(sources->sources.plain.platforms.size(), 1);
    EXPECT_FALSE(sources->sources.plain.active.isNull());
    EXPECT_TRUE(project().packExportSelectedPlatformIds.contains(second));
    EXPECT_EQ(project().packExportCurrentPlatformId, Platforms::idString(windows));
    EXPECT_FALSE(project().packGroups[0].includesPlatform(second));
    EXPECT_TRUE(project().packGroups[1].includesPlatform(second));
    EXPECT_EQ(PlatformContextService::instance().context().primary, QUuid(first));
    EXPECT_FALSE(PlatformContextService::instance().context().all);
    for (const auto& base : {"Windows", "macOS", "Linux", "Android", "iOS"})
        EXPECT_FALSE(create(QStringLiteral("Custom_") + base, base).isEmpty());
    EXPECT_EQ(project().platforms.size(), 8);
    const auto list = run("list_project_platforms");
    ASSERT_TRUE(list.success);
    EXPECT_EQ(list.data.value("platforms").toArray().size(), 8);
    EXPECT_EQ(list.data.value("base_platforms").toArray().size(), 5);
}

TEST_F(ProjectPlatformCommand, CreateUndoRedoPreservesIdentityAndAllSelection)
{
    const auto before = authoredState();
    const auto context = PlatformContextService::instance().context();
    const auto id = create("Android_Low");
    const auto after = authoredState();
    const auto all = PlatformContextService::instance().context();
    EXPECT_TRUE(all.all);
    EXPECT_TRUE(all.selected.contains(QUuid(id)));
    ASSERT_TRUE(undo().undo());
    EXPECT_EQ(authoredState(), before);
    EXPECT_EQ(PlatformContextService::instance().context(), context);
    ASSERT_TRUE(undo().redo());
    EXPECT_EQ(authoredState(), after);
    EXPECT_EQ(PlatformContextService::instance().context(), all);
    EXPECT_EQ(Platforms::idString(project().platforms.last().id), id);
}

TEST_F(ProjectPlatformCommand, RejectsInvalidEditsAndNoOpsWithoutDirtyOrHistory)
{
    const auto id = create("Android_Low");
    undo().clear();
    clearDirty();
    const auto before = authoredState();
    const QVector<QPair<QString, QJsonObject>> invalid{
        {"create_project_platform", {{"name", "android_low"}, {"base_platform", "Android"}}},
        {"create_project_platform", {{"name", "Bad/Name"}, {"base_platform", "Android"}}},
        {"create_project_platform", {{"name", "New"}, {"base_platform", "Universal"}}},
        {"create_project_platform", {{"name", "New"}, {"base_platform", "Android"}, {"platform_id", id}}},
        {"rename_project_platform", {{"platform_id", id}, {"name", "Windows"}}},
        {"rename_project_platform", {{"platform_id", id}, {"name", "New"}, {"base_platform", "iOS"}}},
        {"delete_project_platform", {{"platform_id", Platforms::idString(QUuid::createUuid())}}},
        {"set_project_platform_profile", {{"platform_id", id}}},
        {"set_project_platform_profile", {{"platform_id", id}, {"profile", QJsonObject{}}}},
        {"set_project_platform_profile", {{"platform_id", id}, {"profile", false}}}
    };
    for (const auto& [command, args] : invalid) {
        EXPECT_FALSE(run(command, args).success) << command.toStdString();
        EXPECT_EQ(authoredState(), before);
        EXPECT_EQ(undo().undoStack()->count(), 0);
        EXPECT_TRUE(undo().isClean());
    }
    EXPECT_TRUE(run("rename_project_platform", {{"platform_id", id}, {"name", "Android_Low"}}).success);
    EXPECT_TRUE(run("set_project_platform_profile", {{"platform_id", id}, {"profile", QJsonValue::Null}}).success);
    EXPECT_EQ(undo().undoStack()->count(), 0);
    EXPECT_TRUE(undo().isClean());
    manager().closeProject();
    EXPECT_EQ(run("create_project_platform", {{"name", "A"}, {"base_platform", "Android"}}).errorCode,
              QStringLiteral("NO_PROJECT_OPEN"));
}

TEST_F(ProjectPlatformCommand, RenameAndProfilePersistPerInstanceWithoutMutatingGlobalProfiles)
{
    const auto global = PlatformProfileManager::instance().toJson();
    const auto low = create("Android_Low");
    const auto high = create("Android_High");
    ProjectPlatformProfile profile;
    profile.performResample = true;
    profile.sizePolicies.front().config.targetSampleRate = 22050;
    profile.sizePolicies = {{1024, profile.sizePolicies.front().config}, {std::nullopt, ConversionParameters{}}};
    ASSERT_TRUE(run("set_project_platform_profile", {{"platform_id", low}, {"profile", profile.toJson()}}).success);
    EXPECT_EQ(Platforms::find(project().platforms, QUuid(high))->profile.toJson(), ProjectPlatformProfile::fromBasePlatform(BasePlatform::Android).toJson());
    const auto beforeRename = authoredState();
    ASSERT_TRUE(run("rename_project_platform", {{"platform_id", low}, {"name", "Android_Mobile"}}).success);
    const auto afterRename = authoredState();
    EXPECT_EQ(Platforms::find(project().platforms, QUuid(low))->basePlatform, BasePlatform::Android);
    ASSERT_TRUE(undo().undo());
    EXPECT_EQ(authoredState(), beforeRename);
    ASSERT_TRUE(undo().redo());
    EXPECT_EQ(authoredState(), afterRename);
    ASSERT_TRUE(run("set_project_platform_profile", {{"platform_id", low}, {"profile", QJsonValue::Null}}).success);
    EXPECT_EQ(Platforms::find(project().platforms, QUuid(low))->profile.toJson(), ProjectPlatformProfile::fromBasePlatform(BasePlatform::Android).toJson());
    ASSERT_TRUE(undo().undo());
    EXPECT_EQ(authoredState(), afterRename);
    reopen();
    const auto* restored = Platforms::find(project().platforms, QUuid(low));
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->name, QStringLiteral("Android_Mobile"));
    EXPECT_EQ(restored->profile.toJson(), profile.toJson());
    EXPECT_EQ(Platforms::find(project().platforms, QUuid(high))->profile.toJson(), ProjectPlatformProfile::fromBasePlatform(BasePlatform::Android).toJson());
    EXPECT_EQ(PlatformProfileManager::instance().toJson(), global);
    manager().closeProject();
    ProjectConfig other;
    other.name = QStringLiteral("Other");
    other.location = directory.path();
    ASSERT_TRUE(manager().createProject(other));
    const auto newId = create("Android_Mobile");
    EXPECT_NE(newId, low);
    EXPECT_EQ(Platforms::find(project().platforms, QUuid(newId))->profile.toJson(), ProjectPlatformProfile::fromBasePlatform(BasePlatform::Android).toJson());
}

TEST_F(ProjectPlatformCommand, BaseChangeResetsStoredProfileAndPreservesReferencesAcrossUndoAndReopen)
{
    const auto id = create("Mobile");
    const auto sibling = create("OtherMobile");
    ProjectPlatformProfile custom;
    custom.performResample = true;
    custom.sizePolicies.front().config.targetSampleRate = 32000;
    ASSERT_TRUE(run("set_project_platform_profile", {{"platform_id", id}, {"profile", custom.toJson()}}).success);
    auto* sound = manager().createItem(nullptr, "Sound", ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    setOverlay(sound->platformOverrides, id, 0.25f);
    const auto entry = QUuid::createUuid();
    auto* sources = sound->comp<VSound>();
    sources->sources.plain.entries = {{entry, 11}};
    sources->sources.plain.active = entry;
    sources->sources.plain.platforms[id].active = entry;
    auto* action = manager().createItem(nullptr, "Action", ProjectItem::Type::Action);
    ASSERT_NE(action, nullptr);
    project().actionPackTargeting = {{action->runtimeId(), false, {id}, QStringLiteral("English")}};
    project().packExportSelectedPlatformIds = {id};
    PlatformContextService::instance().setSelected({QUuid(id)}, QUuid(id), false);
    undo().clear();
    const auto before = authoredState();
    const auto context = PlatformContextService::instance().context();
    ASSERT_TRUE(run("set_project_platform_base", {{"platform_id", id}, {"base_platform", "Windows"}}).success);
    const auto after = authoredState();
    auto expected = before;
    expected.insert("platforms", after.value("platforms"));
    EXPECT_EQ(after, expected);
    const auto* changed = Platforms::find(project().platforms, QUuid(id));
    ASSERT_NE(changed, nullptr);
    EXPECT_EQ(changed->name, QStringLiteral("Mobile"));
    EXPECT_EQ(changed->basePlatform, BasePlatform::Windows);
    EXPECT_EQ(changed->profile.toJson(), ProjectPlatformProfile::fromBasePlatform(BasePlatform::Windows).toJson());
    EXPECT_EQ(Platforms::find(project().platforms, QUuid(sibling))->basePlatform, BasePlatform::Android);
    EXPECT_EQ(PlatformContextService::instance().context(), context);
    EXPECT_EQ(undo().undoStack()->count(), 1);
    ASSERT_TRUE(run("set_project_platform_base", {{"platform_id", id}, {"base_platform", "Windows"}}).success);
    EXPECT_EQ(undo().undoStack()->count(), 1);
    EXPECT_FALSE(run("set_project_platform_base", {{"platform_id", id}, {"base_platform", "Universal"}}).success);
    ASSERT_TRUE(undo().undo());
    EXPECT_EQ(authoredState(), before);
    ASSERT_TRUE(undo().redo());
    EXPECT_EQ(authoredState(), after);
    reopen();
    EXPECT_EQ(authoredState(), after);
}

TEST_F(ProjectPlatformCommand, DeleteCleansEveryReferenceAtomicallyAndUndoRestoresExactState)
{
    const auto low = create("Android_Low");
    const auto high = create("Android_High");
    auto* track = manager().createItem(nullptr, "Track", ProjectItem::Type::Track);
    auto* sound = manager().createItem(track, "Sound", ProjectItem::Type::Sound);
    auto* container = manager().createItem(nullptr, "Container", ProjectItem::Type::Container);
    auto* child = manager().createItem(container, "Child", ProjectItem::Type::Sound);
    auto* action = manager().createItem(nullptr, "Action", ProjectItem::Type::Action);
    ASSERT_NE(sound, nullptr);
    ASSERT_NE(child, nullptr);
    ASSERT_NE(track, nullptr);
    ASSERT_NE(action, nullptr);
    auto* timeline = track->comp<VTrack>();
    Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.targetGuid = sound->guid;
    clip.duration = 4;
    ASSERT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
    timeline->clips.items.push_back(clip);
    for (auto* ledger : {&track->platformOverrides, &sound->platformOverrides,
                        &timeline->clips.items.front().platformOverrides}) {
        setOverlay(*ledger, low, 0.25f);
        setOverlay(*ledger, high, 0.75f);
    }
    container->comp<VNX::VSelection>()->selections.editTable(QUuid(low)).entries.front().occurrences.front().weight = 25;
    container->comp<VNX::VSelection>()->selections.editTable(QUuid(high)).entries.front().occurrences.front().weight = 75;
    const auto lowGuid = toFieldGroupGuid(QUuid(low));
    const auto highGuid = toFieldGroupGuid(QUuid(high));
    sound->fieldGroups.mainOutput.platforms[lowGuid] = {FieldGroupMode::UseShared,
        MainOutputFieldGroup{{}, -3.0f}, FieldGroupFieldMask(harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume)};
    sound->fieldGroups.send.platforms[lowGuid] = {FieldGroupMode::Local, SendFieldGroup{}};
    sound->fieldGroups.spatialRating.platforms[lowGuid] = {FieldGroupMode::Local,
        SpatialRatingFieldGroup{0.25f}};
    sound->fieldGroups.spatialRating.platforms[highGuid] = {FieldGroupMode::UseShared,
        SpatialRatingFieldGroup{0.75f}, FieldGroupFieldMask(harkenplayableproperties::PlayablePropertyIdV1::SpatialPanning)};
    const auto groupFields = sound->fieldGroups;
    const auto firstEntry = QUuid::createUuid();
    const auto secondEntry = QUuid::createUuid();
    auto* sources = sound->comp<VSound>();
    sources->sources.plain.entries = {{firstEntry, 11}, {secondEntry, 12}};
    sources->sources.plain.platforms[low].active = firstEntry;
    sources->sources.plain.platforms[high].active = secondEntry;
    sources->sources.plain.active = firstEntry;
    project().actionPackTargeting = {{action->runtimeId(), false, {low}, QStringLiteral("English")},
                                     {action->runtimeId(), false, {high, low}, QStringLiteral("Japanese")}};
    ProjectData::DeploymentProfile deployment;
    deployment.profileId = "mobile";
    deployment.name = "Mobile";
    deployment.destination = "Device";
    deployment.platformIds = {low};
    project().deploymentProfiles = {deployment};
    project().packExportCurrentPlatformId = low;
    project().packExportSelectedPlatformIds = {low};
    PlatformContextService::instance().setSelected({QUuid(low)}, QUuid(low), false);
    SoundSourceEditing::syncActiveProjection(*sound, QUuid(low), {});
    ASSERT_TRUE(manager().saveProject());
    undo().clear();
    clearDirty();
    const auto before = authoredState();
    const auto beforeContext = PlatformContextService::instance().context();
    const auto soundGuid = sound->guid;
    const auto containerGuid = container->guid;
    const auto trackGuid = track->guid;
    QObject observer;
    int observations = 0;
    const auto check = [&]() {
        ++observations;
        QString error;
        EXPECT_TRUE(Platforms::validateProject(project(), &error)) << error.toStdString();
    };
    QObject::connect(&manager(), &ProjectManager::targetPlatformsChanged, &observer, check);
    QObject::connect(&manager(), &ProjectManager::itemPropertyChanged, &observer, check);
    QObject::connect(&PlatformContextService::instance(), &PlatformContextService::contextChanged, &observer, check);
    const auto removed = run("delete_project_platform", {{"platform_id", low}});
    ASSERT_TRUE(removed.success) << removed.errorMessage.toStdString();
    EXPECT_EQ(removed.data.value("affected_item_count").toInteger(), 3);
    EXPECT_GT(observations, 0);
    EXPECT_FALSE(undo().isClean());
    EXPECT_TRUE(sound->isDirtyObject());
    EXPECT_TRUE(container->isDirtyObject());
    EXPECT_TRUE(track->isDirtyObject());
    EXPECT_FALSE(child->isDirtyObject());
    EXPECT_FALSE(action->isDirtyObject());
    for (const auto* ledger : {&track->platformOverrides, &sound->platformOverrides,
                              &timeline->clips.items.front().platformOverrides}) {
        EXPECT_EQ(ledger->find(QUuid(low)), nullptr);
        EXPECT_NE(ledger->find(QUuid(high)), nullptr);
    }
    EXPECT_FALSE(container->comp<VNX::VSelection>()->selections.platforms.contains(low));
    EXPECT_EQ(container->comp<VNX::VSelection>()->selections.table(QUuid(high)).occurrenceRows().front().weight, 75);
    EXPECT_FALSE(sound->fieldGroups.mainOutput.platforms.contains(lowGuid));
    EXPECT_FALSE(sound->fieldGroups.send.platforms.contains(lowGuid));
    EXPECT_FALSE(sound->fieldGroups.spatialRating.platforms.contains(lowGuid));
    EXPECT_EQ(sound->fieldGroups.spatialRating.platforms.at(highGuid), groupFields.spatialRating.platforms.at(highGuid));
    EXPECT_EQ(sources->sources.plain.entries.size(), 2);
    ASSERT_EQ(sources->sources.plain.platforms.size(), 1);
    EXPECT_TRUE(sources->sources.plain.platforms.contains(high));
    EXPECT_FALSE(sources->sources.plain.active.isNull());
    EXPECT_EQ(project().actionPackTargeting[1].platformIds, QStringList{high});
    EXPECT_FALSE(project().actionPackTargeting[0].allPlatforms);
    EXPECT_TRUE(project().actionPackTargeting[0].platformIds.isEmpty());
    EXPECT_TRUE(project().deploymentProfiles[0].platformIds.isEmpty());
    EXPECT_TRUE(project().packExportSelectedPlatformIds.isEmpty());
    EXPECT_EQ(project().packExportCurrentPlatformId, Platforms::idString(project().platforms.first().id));
    EXPECT_FALSE(PlatformContextService::instance().context().all);
    EXPECT_EQ(PlatformContextService::instance().context().primary, project().platforms.first().id);
    const auto after = authoredState();
    ASSERT_TRUE(undo().undo());
    EXPECT_EQ(authoredState(), before);
    EXPECT_EQ(sound->fieldGroups, groupFields);
    EXPECT_EQ(PlatformContextService::instance().context(), beforeContext);
    ASSERT_TRUE(undo().redo());
    EXPECT_EQ(authoredState(), after);
    ASSERT_TRUE(manager().saveProject());
    ASSERT_TRUE(undo().undo());
    EXPECT_EQ(authoredState(), before);
    EXPECT_EQ(sound->fieldGroups, groupFields);
    QObject::disconnect(&manager(), nullptr, &observer, nullptr);
    QObject::disconnect(&PlatformContextService::instance(), nullptr, &observer, nullptr);
    reopen();
    EXPECT_NE(Platforms::find(project().platforms, QUuid(low)), nullptr);
    EXPECT_NE(manager().findItemByGuid(soundGuid)->platformOverrides.find(QUuid(low)), nullptr);
    EXPECT_EQ(manager().findItemByGuid(containerGuid)->comp<VNX::VSelection>()->selections.table(QUuid(low)).occurrenceRows().front().weight, 25);
    EXPECT_TRUE(manager().findItemByGuid(trackGuid)->comp<VTrack>()->clips.items.front().platformOverrides.rows.empty());
    ASSERT_TRUE(run("delete_project_platform", {{"platform_id", low}}).success);
    reopen();
    EXPECT_EQ(Platforms::find(project().platforms, QUuid(low)), nullptr);
    EXPECT_TRUE(Platforms::validateProject(project()));
    EXPECT_TRUE(project().packExportSelectedPlatformIds.isEmpty());
    EXPECT_FALSE(project().actionPackTargeting[0].allPlatforms);
    EXPECT_TRUE(project().actionPackTargeting[0].platformIds.isEmpty());
}

TEST_F(ProjectPlatformCommand, LastPlatformIsProtectedAndRecreatedNameGetsNewIdentity)
{
    const auto windows = Platforms::idString(project().platforms.first().id);
    EXPECT_EQ(run("delete_project_platform", {{"platform_id", windows}}).errorCode, QStringLiteral("LAST_PLATFORM"));
    EXPECT_EQ(undo().undoStack()->count(), 0);
    const auto oldId = create("Android");
    auto* sound = manager().createItem(nullptr, "Sound", ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    setOverlay(sound->platformOverrides, oldId, 0.25f);
    ASSERT_TRUE(run("delete_project_platform", {{"platform_id", oldId}}).success);
    const auto newId = create("Android");
    EXPECT_NE(oldId, newId);
    EXPECT_EQ(sound->platformOverrides.find(QUuid(oldId)), nullptr);
    EXPECT_EQ(sound->platformOverrides.find(QUuid(newId)), nullptr);
    ASSERT_TRUE(run("delete_project_platform", {{"platform_id", windows}}).success);
    EXPECT_EQ(project().platforms.size(), 1);
    EXPECT_EQ(run("delete_project_platform", {{"platform_id", newId}}).errorCode, QStringLiteral("LAST_PLATFORM"));
}

TEST_F(ProjectPlatformCommand, RenameAndProfileReplayPreserveSubsequentContextAndRootSettings)
{
    const auto id = create("Android");
    ASSERT_TRUE(run("rename_project_platform", {{"platform_id", id}, {"name", "Mobile"}}).success);
    PlatformContextService::instance().setSelected({QUuid(id)}, QUuid(id), false);
    project().packGroups = {{"New", {}, false, {id}}};
    project().packExportSelectedPlatformIds = {id};
    const auto context = PlatformContextService::instance().context();
    ASSERT_TRUE(undo().undo());
    EXPECT_EQ(Platforms::find(project().platforms, QUuid(id))->name, QStringLiteral("Android"));
    EXPECT_EQ(PlatformContextService::instance().context(), context);
    EXPECT_EQ(project().packExportSelectedPlatformIds, QStringList{id});
    ASSERT_EQ(project().packGroups.size(), 1);
    EXPECT_EQ(project().packGroups[0].platformIds, QStringList{id});
    ASSERT_TRUE(undo().redo());
    EXPECT_EQ(PlatformContextService::instance().context(), context);
    ProjectPlatformProfile profile;
    ASSERT_TRUE(run("set_project_platform_profile", {{"platform_id", id}, {"profile", profile.toJson()}}).success);
    const auto windows = project().platforms.first().id;
    PlatformContextService::instance().setSelected({windows}, windows, false);
    ASSERT_TRUE(undo().undo());
    EXPECT_EQ(Platforms::find(project().platforms, QUuid(id))->profile.toJson(), ProjectPlatformProfile::fromBasePlatform(BasePlatform::Android).toJson());
    EXPECT_EQ(PlatformContextService::instance().context().primary, windows);
}

TEST_F(ProjectPlatformCommand, ValidEditsFinishPreviewsBeforeSnapshotAndReplayCancelsNewPreviews)
{
    const auto low = create("Android_Low");
    auto* sound = manager().createItem(nullptr, "Sound", ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    const auto locator = PlayablePropertyAuthoringV1::definitionLocator(*sound);
    ASSERT_TRUE(locator);
    undo().clear();
    PropertyEditSession preview(sound, *locator, 1, nullptr, QVector<ProjectPlatformId>{QUuid(low)});
    preview.preview(-6.0);
    EXPECT_FALSE(run("rename_project_platform", {{"platform_id", low}, {"name", "Windows"}}).success);
    EXPECT_TRUE(preview.isActive());
    EXPECT_EQ(undo().undoStack()->count(), 0);
    ASSERT_TRUE(run("delete_project_platform", {{"platform_id", low}}).success);
    EXPECT_FALSE(preview.isActive());
    EXPECT_EQ(undo().undoStack()->count(), 2);
    EXPECT_EQ(sound->platformOverrides.find(QUuid(low)), nullptr);
    ASSERT_TRUE(undo().undo());
    const auto restored = PlayablePropertyAuthoringV1::readProperty(*sound, *locator, 1, QUuid(low));
    ASSERT_TRUE(restored);
    EXPECT_TRUE(restored->present);
    EXPECT_NEAR(restored->value.toDouble(), -6.0, 0.0001);
    PropertyEditSession next(sound, *locator, 1, nullptr, QVector<ProjectPlatformId>{QUuid(low)});
    next.preview(-12.0);
    ASSERT_TRUE(undo().redo());
    EXPECT_FALSE(next.isActive());
    EXPECT_EQ(sound->platformOverrides.find(QUuid(low)), nullptr);
    next.commit();
    EXPECT_EQ(undo().undoStack()->count(), 2);
    ASSERT_TRUE(undo().undo());
    ASSERT_TRUE(undo().undo());
    EXPECT_EQ(sound->platformOverrides.find(QUuid(low)), nullptr);
}

TEST_F(ProjectPlatformCommand, BusyCommandsAndGuiIpcUndoRedoRemainAvailable)
{
    const auto id = create("Android");
    IPCServer server(nullptr);
    server.setProjectManager(&manager());
    server.setAppState(IPCServer::AppState::Ready);
    std::unique_ptr<QAction> undoAction(undo().createUndoAction(nullptr));
    std::unique_ptr<QAction> redoAction(undo().createRedoAction(nullptr));
    const auto before = authoredState();
    auto activity = ProjectPlatformActivity::acquire();
    ProjectPlatformProfile profile;
    profile.performResample = true;
    profile.sizePolicies.front().config.targetSampleRate = 32000;
    const QVector<QPair<QString, QJsonObject>> commands{
        {"create_project_platform", {{"name", "New"}, {"base_platform", "Android"}}},
        {"rename_project_platform", {{"platform_id", id}, {"name", "New"}}},
        {"delete_project_platform", {{"platform_id", id}}},
        {"set_project_platform_profile", {{"platform_id", id}, {"profile", profile.toJson()}}}
    };
    for (const auto& [command, args] : commands) {
        ASSERT_TRUE(run(command, args).success);
        EXPECT_TRUE(undoAction->isEnabled());
        undoAction->trigger();
        EXPECT_EQ(authoredState(), before);
        EXPECT_TRUE(redoAction->isEnabled());
        redoAction->trigger();
        ASSERT_EQ(server.dispatchCommand("undo", {}).value("status").toString(), QStringLiteral("ok"));
        EXPECT_EQ(authoredState(), before);
        ASSERT_EQ(server.dispatchCommand(command, args).value("status").toString(), QStringLiteral("ok"));
        ASSERT_TRUE(undo().undo());
        EXPECT_EQ(authoredState(), before);
    }
}

TEST_F(ProjectPlatformCommand, BusyMacrosAndUnrelatedHistoryRemainReplayable)
{
    undo().beginMacro("Two platforms");
    const auto first = create("First");
    const auto second = create("Second");
    undo().endMacro();
    ASSERT_EQ(undo().undoStack()->count(), 1);
    QObject object;
    object.setProperty("value", 2);
    undo().push(new PropertyChangeCommand(&object, "value", 1, 2, "Value"));
    auto activity = ProjectPlatformActivity::acquire();
    ASSERT_TRUE(undo().undo());
    EXPECT_EQ(object.property("value").toInt(), 1);
    ASSERT_TRUE(undo().undo());
    EXPECT_EQ(project().platforms.size(), 1);
    ASSERT_TRUE(undo().redo());
    EXPECT_NE(Platforms::find(project().platforms, QUuid(first)), nullptr);
    EXPECT_NE(Platforms::find(project().platforms, QUuid(second)), nullptr);
}

TEST_F(ProjectPlatformCommand, IpcEditsUseProjectProfilesAndRejectGlobalProfileWrites)
{
    registerAssetActions(ActionManager::instance(), nullptr, &manager());
    IPCServer server(nullptr);
    server.setProjectManager(&manager());
    server.setAppState(IPCServer::AppState::Ready);
    const auto global = PlatformProfileManager::instance().toJson();
    const auto created = server.dispatchCommand("create_project_platform",
        {{"name", "Mobile"}, {"base_platform", "Android"}});
    ASSERT_EQ(created.value("status").toString(), QStringLiteral("ok"));
    const auto id = created.value("result").toObject().value("platform_id").toString();
    ProjectPlatformProfile profile;
    profile.performResample = true;
    profile.sizePolicies.front().config.targetSampleRate = 32000;
    const auto updated = server.dispatchCommand("set_project_platform_profile",
        {{"platform_id", id}, {"profile", profile.toJson()}});
    ASSERT_EQ(updated.value("status").toString(), QStringLiteral("ok"));
    const auto queried = server.dispatchCommand("list_project_platforms", {});
    EXPECT_EQ(queried.value("result").toObject().value("platforms").toArray().last()
                  .toObject().value("profile").toObject(), profile.toJson());
    EXPECT_EQ(server.dispatchCommand("set_platform_profile",
        {{"platform", "Android"}, {"default_config", profile.sizePolicies.front().config.toJson()}})
                  .value("code").toString(), QStringLiteral("PLATFORM_INSTANCES_REQUIRED"));
    EXPECT_EQ(PlatformProfileManager::instance().toJson(), global);
    EXPECT_EQ(server.dispatchCommand("rename_project_platform", {{"platform_id", id}, {"name", "Device"}})
                  .value("status").toString(), QStringLiteral("ok"));
    EXPECT_EQ(server.dispatchCommand("delete_project_platform", {{"platform_id", id}})
                  .value("status").toString(), QStringLiteral("ok"));
    ASSERT_TRUE(undo().undo());
    const auto* restored = Platforms::find(project().platforms, QUuid(id));
    ASSERT_NE(restored, nullptr);
    EXPECT_EQ(restored->profile.toJson(), profile.toJson());
    EXPECT_EQ(restored->name, QStringLiteral("Device"));
}

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

TEST_F(ProjectPlatformCommand, DeleteMacroRestoresOwnerBeforeRestoringPlatformReferences)
{
    const auto id = create("Android");
    auto* sound = manager().createItem(nullptr, "Sound", ProjectItem::Type::Sound);
    ASSERT_NE(sound, nullptr);
    setOverlay(sound->platformOverrides, id, 0.25f);
    const auto guid = sound->guid;
    undo().clear();
    undo().beginMacro("Delete platform and sound");
    const auto removed = run("delete_project_platform", {{"platform_id", id}});
    const auto deleted = manager().deleteItem(sound);
    undo().endMacro();
    ASSERT_TRUE(removed.success);
    ASSERT_TRUE(deleted);
    EXPECT_EQ(manager().findItemByGuid(guid), nullptr);
    ASSERT_TRUE(undo().undo());
    sound = manager().findItemByGuid(guid);
    ASSERT_NE(sound, nullptr);
    EXPECT_NE(sound->platformOverrides.find(QUuid(id)), nullptr);
    EXPECT_TRUE(Platforms::validateProject(project()));
    ASSERT_TRUE(undo().redo());
    EXPECT_EQ(manager().findItemByGuid(guid), nullptr);
    EXPECT_EQ(Platforms::find(project().platforms, QUuid(id)), nullptr);
}

TEST_F(ProjectPlatformCommand, BackgroundActivityNoLongerBlocksPlatformEdits)
{
    const auto id = create("Android");
    struct ExportActivity {
        ExportActivity() { ProjectPackExporter::beginBackgroundExport(); }
        ~ExportActivity() { ProjectPackExporter::endBackgroundExport(); }
    };
    {
        ExportActivity background;
        EXPECT_TRUE(ProjectPlatformActivity::instance().isBusy());
        EXPECT_TRUE(run("delete_project_platform", {{"platform_id", id}}).success);
        EXPECT_TRUE(undo().undo());
    }
    EXPECT_FALSE(ProjectPlatformActivity::instance().isBusy());
    AssetConverter converter;
    auto future = converter.convertAsync(0, QUuid(id));
    future.waitForFinished();
    EXPECT_FALSE(future.result().success);
    EXPECT_FALSE(ProjectPlatformActivity::instance().isBusy());
}

TEST_F(ProjectPlatformCommand, PlatformEditsInvalidateOnlyRelatedRecordsAndPreserveArtifacts)
{
    const auto id = create("Android");
    const auto windows = Platforms::idString(project().platforms.first().id);
    QFile artifact(directory.filePath("existing.pack"));
    ASSERT_TRUE(artifact.open(QIODevice::WriteOnly));
    ASSERT_EQ(artifact.write("existing"), 8);
    artifact.close();
    auto& records = PackGenerationRecordStore::instance();
    records.add({"Action", 1, "Action", id, {}, true, "Built", artifact.fileName(), {}, {}, 8, 10});
    records.add({"Action", 1, "Action", windows, {}, true, "Built", "Windows/Action.pack", {}, {}, 8, 10});
    ASSERT_TRUE(run("rename_project_platform", {{"platform_id", id}, {"name", "Android_Renamed"}}).success);
    EXPECT_EQ(records.records()[0].status, QStringLiteral("Needs rebuild"));
    EXPECT_EQ(records.records()[0].outputPath, artifact.fileName());
    EXPECT_TRUE(records.records()[0].success);
    EXPECT_EQ(records.records()[1].status, QStringLiteral("Built"));
    ASSERT_TRUE(undo().undo());
    EXPECT_EQ(records.records()[0].status, QStringLiteral("Needs rebuild"));
    ASSERT_TRUE(run("delete_project_platform", {{"platform_id", id}}).success);
    EXPECT_TRUE(artifact.exists());
    EXPECT_EQ(records.records().size(), 2);
}

TEST_F(ProjectPlatformCommand, SettingsBatchAtomicallyReplacesLastPlatformAndRestoresReferences)
{
    const auto id = Platforms::idString(project().platforms.first().id);
    auto* sound = manager().createItem(nullptr, "Sound", ProjectItem::Type::Sound);
    setOverlay(sound->platformOverrides, id, 0.25f);
    const auto entry = QUuid::createUuid();
    sound->comp<VSound>()->sources.plain.entries = {{entry, 11}};
    sound->comp<VSound>()->sources.plain.platforms[id].active = entry;
    sound->comp<VSound>()->sources.plain.active = entry;
    project().projectLanguages = {"English"};
    auto& dormant = sound->comp<VSound>()->sources.localized["English"];
    dormant.entries = {{QUuid::createUuid(), 11}};
    dormant.active = dormant.entries[0].entryId;
    dormant.platforms[id].active = dormant.active;
    const auto dormantBefore = dormant;
    project().packGroups = {{"Explicit", {}, false, {id}}};
    auto* action = manager().createItem(nullptr, "Action", ProjectItem::Type::Action);
    project().actionPackTargeting = {{action->runtimeId(), false, {id}}};
    PlatformContextService::instance().setSelected({QUuid(id)}, QUuid(id), false);
    SoundSourceEditing::syncActiveProjection(*sound, QUuid(id), {});
    undo().clear();
    const auto before = authoredState();
    const auto baseline = Platforms::platformsToJson(project().platforms);
    const auto result = run("apply_project_platforms", {{"expected_platforms", baseline},
        {"platforms", QJsonArray{QJsonObject{{"name", "Android_Low"}, {"basePlatform", "Android"},
                                     {"profile", ProjectPlatformProfile::fromBasePlatform(BasePlatform::Android).toJson()}},
                                 QJsonObject{{"name", "Android_High"}, {"basePlatform", "Android"},
                                     {"profile", ProjectPlatformProfile::fromBasePlatform(BasePlatform::Android).toJson()}}}}});
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_EQ(undo().undoStack()->count(), 1);
    EXPECT_EQ(project().platforms.size(), 2);
    EXPECT_NE(project().platforms[0].id, project().platforms[1].id);
    EXPECT_TRUE(project().packGroups.front().platformIds.isEmpty());
    EXPECT_FALSE(project().packGroups.front().allPlatforms);
    EXPECT_TRUE(sound->comp<VSound>()->sources.plain.platforms.isEmpty());
    EXPECT_FALSE(sound->comp<VSound>()->sources.plain.active.isNull());
    EXPECT_TRUE(sound->comp<VSound>()->sources.localized["English"].platforms.isEmpty());
    EXPECT_EQ(sound->comp<VSound>()->sources.localized["English"].entries, dormantBefore.entries);
    EXPECT_EQ(sound->platformOverrides.find(QUuid(id)), nullptr);
    EXPECT_EQ(project().packExportSelectedPlatformIds.size(), 2);
    const auto after = authoredState();
    undo().undo();
    EXPECT_EQ(authoredState(), before);
    EXPECT_EQ(sound->comp<VSound>()->sources.localized["English"], dormantBefore);
    EXPECT_EQ(PlatformContextService::instance().context().primary, QUuid(id));
    undo().redo();
    EXPECT_EQ(authoredState(), after);
    reopen();
    ASSERT_TRUE(manager().isProjectOpen());
    EXPECT_EQ(Platforms::platformsToJson(project().platforms), result.data.value("platforms").toArray());
}

TEST_F(ProjectPlatformCommand, SettingsBatchRejectsConflictsUnknownIdsAndMissingProfilesBeforeMutation)
{
    const auto baseline = Platforms::platformsToJson(project().platforms);
    auto changed = baseline.first().toObject();
    for (const auto& value : {QJsonValue(QJsonArray{}),
             QJsonValue(QJsonArray{QJsonObject{{"id", QUuid::createUuid().toString(QUuid::WithoutBraces)},
                 {"name", "Unknown"}, {"basePlatform", "Android"}}}),
             QJsonValue(QJsonArray{QJsonObject{{"id", changed.value("id")}, {"name", "Windows"}, {"basePlatform", "Android"}}})}) {
        const auto result = run("apply_project_platforms", {{"expected_platforms", baseline}, {"platforms", value}});
        EXPECT_FALSE(result.success);
        EXPECT_EQ(Platforms::platformsToJson(project().platforms), baseline);
        EXPECT_EQ(undo().undoStack()->count(), 0);
    }
    changed.insert("name", "Renamed");
    ASSERT_TRUE(run("rename_project_platform", {{"platform_id", changed.value("id")}, {"name", "External"}}).success);
    const auto result = run("apply_project_platforms", {{"expected_platforms", baseline}, {"platforms", QJsonArray{changed}}});
    EXPECT_EQ(result.errorCode, QStringLiteral("PLATFORM_EDIT_CONFLICT"));
    EXPECT_EQ(project().platforms.first().name, QStringLiteral("External"));
    EXPECT_EQ(undo().undoStack()->count(), 1);
}

TEST_F(ProjectPlatformCommand, SettingsBatchCanSwapNamesWithoutTemporaryInstancesAndNoopHasNoHistory)
{
    create("Second");
    undo().clear();
    const auto baseline = Platforms::platformsToJson(project().platforms);
    EXPECT_TRUE(run("apply_project_platforms", {{"expected_platforms", baseline}, {"platforms", baseline}}).success);
    EXPECT_EQ(undo().undoStack()->count(), 0);
    auto first = baseline.at(0).toObject();
    auto second = baseline.at(1).toObject();
    first.insert("name", "Second");
    second.insert("name", "Windows");
    const auto result = run("apply_project_platforms", {{"expected_platforms", baseline}, {"platforms", QJsonArray{first, second}}});
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_EQ(Platforms::platformsToJson(project().platforms), QJsonArray({first, second}));
    EXPECT_EQ(undo().undoStack()->count(), 1);
    undo().undo();
    EXPECT_EQ(Platforms::platformsToJson(project().platforms), baseline);
}

}
