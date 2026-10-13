#include "Core/ActionManager.h"
#include "Core/AssetPipeline/AssetConverter.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VSound.h"
#include "Core/LanguageContextService.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectManager.h"
#include "Core/FieldGroupProjectAdapter.h"
#include "Core/SoundSourceEditing.h"
#include "Core/UndoCommands.h"
#include "Support/ProjectPlatformTestUtils.h"

#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <functional>
#include <stdexcept>
#include <vector>

namespace {

using namespace VNX;
using T = ProjectItemType;

void drainAudioImportEvents()
{
    QApplication::processEvents();
    QApplication::processEvents();
    QApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

class BlueprintAudioImportTest : public ::testing::Test
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
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen()) manager.closeProject();
        ASSERT_TRUE(directory.isValid());
        PlatformContextService::instance().setStorageForTesting(std::make_unique<MemorySettingsStorage>());
        LanguageContextService::instance().setStorageForTesting(std::make_unique<MemorySettingsStorage>());
        ProjectConfig config;
        config.name = QStringLiteral("BlueprintAudioImport");
        config.location = directory.path();
        config.platforms = HarkenTest::platforms({"Windows", "Android"});
        ASSERT_TRUE(manager.createProject(config));
        PlatformContextService::instance().setSelected(
            {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, true);
        auto& actions = ActionManager::instance();
        actions.clearAllForTesting();
        registerProjectActions(actions, &manager, nullptr, {});
        registerAssetActions(actions, nullptr, &manager);
        ASSERT_TRUE(actions.hasAction(QStringLiteral("import_audio_to_canvas")));
        UndoRedoManager::instance().clear();
        drainAudioImportEvents();
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ActionManager::instance().clearAllForTesting();
        ProjectManager::instance().closeProject();
        drainAudioImportEvents();
        PlatformContextService::instance().setStorageForTesting(SettingsStorageFactory::create(
            SettingsStorageFactory::StorageType::QSettings,
            QStringLiteral("HarkenSound"), QStringLiteral("HarkenStudio")));
        LanguageContextService::instance().setStorageForTesting(SettingsStorageFactory::create(
            SettingsStorageFactory::StorageType::QSettings,
            QStringLiteral("HarkenSound"), QStringLiteral("HarkenStudio")));
    }

    ProjectItem* create(const char* name, T type, ProjectItem* parent = nullptr)
    {
        auto* item = ProjectManager::instance().createItem(parent, QString::fromLatin1(name), type);
        if (!item) throw std::runtime_error("Cannot create test object");
        return item;
    }

    QString wave(const char* name, float amplitude)
    {
        const auto path = QDir(directory.path()).filePath(QString::fromLatin1(name));
        if (!AssetConverter::writeFloatWav(path, std::vector<float>(480, amplitude), 1, 48000))
            throw std::runtime_error("Cannot create test WAV");
        return path;
    }

    ActionResult importFiles(const QStringList& files, uint32_t target = 0, uint32_t canvasRoot = 0)
    {
        const QJsonObject args{
            {QStringLiteral("files"), QJsonArray::fromStringList(files)},
            {QStringLiteral("drop_x"), 320.0},
            {QStringLiteral("drop_y"), 160.0},
            {QStringLiteral("target_container_id"), static_cast<qint64>(target)},
            {QStringLiteral("canvas_root_id"), static_cast<qint64>(canvasRoot)}};
        const auto result = ActionManager::instance().execute(
            QStringLiteral("import_audio_to_canvas"), args, ActionSource::GUI);
        drainAudioImportEvents();
        return result;
    }

    QList<ProjectItem*> sounds() const
    {
        QList<ProjectItem*> result;
        const std::function<void(ProjectItem*)> collect = [&](ProjectItem* item) {
            if (item->type == T::Sound) result.push_back(item);
            for (const auto& child : item->children) collect(child.get());
        };
        for (const auto* binding : ProjectManager::instance().currentProject()->allFileBindings())
            if (binding->rootItem) collect(binding->rootItem.get());
        return result;
    }

    QStringList importedFiles() const
    {
        QStringList files;
        QDirIterator iterator(QDir(AssetRegistry::instance().projectRoot())
            .filePath(QStringLiteral("AudioSources")), QDir::Files, QDirIterator::Subdirectories);
        while (iterator.hasNext()) files.push_back(iterator.next());
        files.sort();
        return files;
    }

    void expectRegisteredWave(uint32_t sourceID)
    {
        const auto* asset = AssetRegistry::instance().getAsset(sourceID);
        ASSERT_NE(asset, nullptr);
        EXPECT_EQ(asset->metadata.sampleRate, 48000u);
        EXPECT_EQ(asset->metadata.channels, 1);
        EXPECT_EQ(asset->metadata.totalSamples, 480u);
        EXPECT_TRUE(QFile::exists(asset->absolutePath));
        EXPECT_FALSE(asset->contentHash.isEmpty());
    }

    void expectCounts(const ActionResult& result, int success, int failed, int duplicates)
    {
        EXPECT_EQ(result.data.value(QStringLiteral("success_count")).toInt(-1), success);
        EXPECT_EQ(result.data.value(QStringLiteral("fail_count")).toInt(-1), failed);
        EXPECT_EQ(result.data.value(QStringLiteral("duplicate_count")).toInt(-1), duplicates);
        EXPECT_EQ(result.data.value(QStringLiteral("source_ids")).toArray().size(), success);
    }

    QTemporaryDir directory;
};

TEST_F(BlueprintAudioImportTest, EmptyCanvasImportsRootSoundAndRestoresMediaInOneUndoStep)
{
    const auto input = wave("root.wav", 0.1f);
    const auto result = importFiles({input});
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    expectCounts(result, 1, 0, 0);
    const auto imported = sounds();
    ASSERT_EQ(imported.size(), 1);
    auto* sound = imported.front();
    EXPECT_EQ(sound->parent, nullptr);
    EXPECT_EQ(sound->name, QStringLiteral("root"));
    const auto guid = sound->guid;
    const auto sourceID = sound->sourceID;
    ASSERT_NE(sourceID, 0u);
    EXPECT_EQ(result.data.value(QStringLiteral("created_authoring_handles")).toArray(),
        QJsonArray{static_cast<qint64>(sound->objectID)});
    EXPECT_EQ(result.data.value(QStringLiteral("target_parent_authoring_handle")).toInteger(-1), 0);
    expectRegisteredWave(sourceID);
    ASSERT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainAudioImportEvents();
    EXPECT_TRUE(sounds().empty());
    EXPECT_FALSE(AssetRegistry::instance().contains(sourceID));
    EXPECT_TRUE(importedFiles().empty());
    EXPECT_TRUE(QFile::exists(input));
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    drainAudioImportEvents();
    ASSERT_EQ(sounds().size(), 1);
    sound = sounds().front();
    EXPECT_EQ(sound->guid, guid);
    EXPECT_EQ(sound->sourceID, sourceID);
    EXPECT_EQ(sound->parent, nullptr);
    expectRegisteredWave(sourceID);
}

TEST_F(BlueprintAudioImportTest, ContainerImportKeepsHierarchyAndInheritedOutputAcrossUndo)
{
    auto& manager = ProjectManager::instance();
    auto* layer = create("Layer", T::Layer);
    auto* container = create("Container", T::Container, layer);
    auto* bus = create("TargetBus", T::Bus, manager.getMasterBusItem());
    container->fieldGroups.mainOutput.shared = {FieldGroupMode::Local,
        MainOutputFieldGroup{toFieldGroupGuid(bus->guid), -4.0f}};
    UndoRedoManager::instance().clear();
    const auto input = wave("child.wav", 0.2f);
    const auto result = importFiles({input}, container->objectID);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    expectCounts(result, 1, 0, 0);
    EXPECT_EQ(result.data.value(QStringLiteral("target_parent_authoring_handle")).toInteger(),
        container->objectID);
    ASSERT_EQ(container->children.size(), 1u);
    ASSERT_EQ(sounds().size(), 1);
    auto* sound = container->children.front().get();
    EXPECT_EQ(sound->type, T::Sound);
    EXPECT_EQ(sound->parent, container);
    EXPECT_EQ(sound->name, QStringLiteral("child"));
    EXPECT_EQ(sound->fieldGroups.mainOutput.shared.mode, FieldGroupMode::Inherit);
    const auto sourceID = sound->sourceID;
    expectRegisteredWave(sourceID);
    ASSERT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainAudioImportEvents();
    EXPECT_TRUE(container->children.empty());
    EXPECT_FALSE(AssetRegistry::instance().contains(sourceID));
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    drainAudioImportEvents();
    ASSERT_EQ(container->children.size(), 1u);
    sound = container->children.front().get();
    EXPECT_EQ(sound->parent, container);
    EXPECT_EQ(sound->name, QStringLiteral("child"));
    EXPECT_EQ(sound->sourceID, sourceID);
    EXPECT_EQ(sound->fieldGroups.mainOutput.shared.mode, FieldGroupMode::Inherit);
    ASSERT_TRUE(container->fieldGroups.mainOutput.shared.localHistory.has_value());
    EXPECT_EQ(container->fieldGroups.mainOutput.shared.localHistory->mainOutputBusRef,
        toFieldGroupGuid(bus->guid));
    expectRegisteredWave(sourceID);
}

TEST_F(BlueprintAudioImportTest, EmptySoundImportActivatesFirstSuccessfulSourceAcrossUndoAndPlatforms)
{
    auto* sound = create("EmptySound", T::Sound);
    ASSERT_NE(ProjectManager::instance().createFolder(nullptr, QStringLiteral("Organization"), QStringLiteral("Layers")), nullptr);
    ASSERT_TRUE(ProjectManager::instance().moveEntriesToFolder(
        {sound}, {}, QStringLiteral("Layers"), QStringLiteral("Organization")));
    auto* component = sound->comp<VSound>();
    ASSERT_NE(component, nullptr);
    ASSERT_TRUE(component->sources.plain.entries.empty());
    const auto before = component->toJson();
    const auto first = wave("first.wav", 0.1f);
    const auto second = wave("second.wav", 0.2f);
    const auto missing = QDir(directory.path()).filePath(QStringLiteral("missing.wav"));
    UndoRedoManager::instance().clear();
    const auto result = importFiles({missing, first, second}, sound->objectID);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    expectCounts(result, 2, 1, 0);
    ASSERT_EQ(sounds().size(), 1);
    ASSERT_EQ(component->sources.plain.entries.size(), 2u);
    const auto active = component->sources.plain.entries.front();
    const auto selection = component->sources.plain.active;
    ASSERT_FALSE(selection.isNull());
    EXPECT_EQ(selection, active.entryId);
    const auto expectActive = [&]() {
        EXPECT_EQ(component->sources.plain.active, selection);
        EXPECT_EQ(sound->sourceID, active.sourceId);
        EXPECT_FALSE(sound->sourcePath.isEmpty());
        EXPECT_EQ(ProjectManager::instance().currentProject()->itemFolderPath(sound), QStringLiteral("Organization"));
        for (const auto& platform : {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}) {
            EXPECT_EQ(SoundSourceEditing::activeSourceId(*sound, platform, {}), active.sourceId);
            EXPECT_FALSE(SoundSourceEditing::activeRelativePath(*sound, platform, {}).isEmpty());
            const auto duration = SoundSourceEditing::activeTrimmedDuration(*sound, platform, {});
            ASSERT_TRUE(duration);
            EXPECT_DOUBLE_EQ(*duration, 0.01);
        }
    };
    expectActive();
    VSound restored;
    ASSERT_TRUE(restored.fromJson(component->toJson()));
    EXPECT_EQ(restored.sources.plain.active, selection);
    ASSERT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainAudioImportEvents();
    EXPECT_EQ(component->toJson(), before);
    EXPECT_EQ(sound->sourceID, 0u);
    EXPECT_TRUE(sound->sourcePath.isEmpty());
    EXPECT_EQ(ProjectManager::instance().currentProject()->itemFolderPath(sound), QStringLiteral("Organization"));
    PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Android}, HarkenTest::PlatformIds::Android, false);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    drainAudioImportEvents();
    ASSERT_EQ(component->sources.plain.entries.size(), 2u);
    EXPECT_EQ(component->sources.plain.entries.front(), active);
    expectActive();
}

TEST_F(BlueprintAudioImportTest, SoundImportAppendsSourcesPreservingActiveTrimAndPlaybackWithoutSibling)
{
    auto& manager = ProjectManager::instance();
    auto* layer = create("Layer", T::Layer);
    auto* container = create("Container", T::Container, layer);
    const auto first = wave("first.wav", 0.3f);
    auto result = importFiles({first}, container->objectID);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    ASSERT_EQ(container->children.size(), 1u);
    auto* sound = container->children.front().get();
    auto* component = sound->comp<VSound>();
    ASSERT_NE(component, nullptr);
    ASSERT_EQ(component->sources.plain.entries.size(), 1u);
    component->sources.plain.entries.front().trimStartFrame = 20;
    component->sources.plain.entries.front().trimEndFrame = 200;
    component->sources.plain.active = component->sources.plain.entries.front().entryId;
    component->scheduleOffsetSec = 0.125;
    auto* mix = sound->comp<VMixNode>();
    ASSERT_NE(mix, nullptr);
    mix->volume = -5.0f;
    mix->pitch = 200.0f;
    const auto original = component->sources.plain.entries.front();
    const auto selection = component->sources.plain.active;
    const auto guid = sound->guid;
    ASSERT_TRUE(manager.saveProject());
    UndoRedoManager::instance().clear();
    const auto second = wave("second.wav", 0.4f);
    result = importFiles({second}, sound->objectID);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    expectCounts(result, 1, 0, 0);
    EXPECT_TRUE(result.data.value(QStringLiteral("created_authoring_handles")).toArray().isEmpty());
    EXPECT_EQ(result.data.value(QStringLiteral("target_sound_authoring_handle")).toInteger(), sound->objectID);
    ASSERT_EQ(container->children.size(), 1u);
    ASSERT_EQ(sounds().size(), 1);
    ASSERT_EQ(component->sources.plain.entries.size(), 2u);
    EXPECT_EQ(component->sources.plain.entries.front(), original);
    EXPECT_EQ(component->sources.plain.active, selection);
    EXPECT_EQ(SoundSourceEditing::activeSourceId(*sound, HarkenTest::PlatformIds::Windows, {}), original.sourceId);
    const auto appended = component->sources.plain.entries.back();
    EXPECT_NE(appended.sourceId, original.sourceId);
    expectRegisteredWave(appended.sourceId);
    const auto expectUnchanged = [&]() {
        EXPECT_EQ(sound->guid, guid);
        EXPECT_EQ(sound->parent, container);
        EXPECT_EQ(component->sources.plain.entries.front(), original);
        EXPECT_EQ(component->sources.plain.active, selection);
        EXPECT_DOUBLE_EQ(component->scheduleOffsetSec, 0.125);
        EXPECT_FLOAT_EQ(mix->volume, -5.0f);
        EXPECT_FLOAT_EQ(mix->pitch, 200.0f);
        EXPECT_EQ(container->children.size(), 1u);
        expectRegisteredWave(original.sourceId);
    };
    expectUnchanged();
    ASSERT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainAudioImportEvents();
    ASSERT_EQ(component->sources.plain.entries.size(), 1u);
    EXPECT_FALSE(AssetRegistry::instance().contains(appended.sourceId));
    expectUnchanged();
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    drainAudioImportEvents();
    ASSERT_EQ(component->sources.plain.entries.size(), 2u);
    EXPECT_EQ(component->sources.plain.entries.back(), appended);
    expectRegisteredWave(appended.sourceId);
    expectUnchanged();
}

TEST_F(BlueprintAudioImportTest, InvalidTargetAndEmptyFilesLeaveNoImportSideEffects)
{
    auto* invalid = create("Variable", T::Variable);
    UndoRedoManager::instance().clear();
    const auto input = wave("rejected.wav", 0.5f);
    const int assetCount = AssetRegistry::instance().count();
    const auto files = importedFiles();
    for (const auto target : {invalid->objectID, uint32_t{0xffffffff}}) {
        SCOPED_TRACE(target);
        const auto result = importFiles({input}, target);
        EXPECT_FALSE(result.success);
        EXPECT_FALSE(result.errorCode.isEmpty());
        EXPECT_EQ(AssetRegistry::instance().count(), assetCount);
        EXPECT_EQ(importedFiles(), files);
        EXPECT_TRUE(sounds().empty());
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    }
    EXPECT_FALSE(importFiles({}).success);
    EXPECT_EQ(AssetRegistry::instance().count(), assetCount);
    EXPECT_EQ(importedFiles(), files);
    EXPECT_TRUE(sounds().empty());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
    EXPECT_TRUE(QFile::exists(input));
}

TEST_F(BlueprintAudioImportTest, MissingOnlyBatchDoesNotCreateObjectsAssetsOrUndoEntry)
{
    const auto missing = QDir(directory.path()).filePath(QStringLiteral("missing.wav"));
    ASSERT_FALSE(QFile::exists(missing));
    const auto result = importFiles({missing});
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.errorCode.isEmpty());
    EXPECT_TRUE(sounds().empty());
    EXPECT_EQ(AssetRegistry::instance().count(), 0);
    EXPECT_TRUE(importedFiles().empty());
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 0);
}

TEST_F(BlueprintAudioImportTest, BusImportCreatesRootSoundWithSharedOutputAndOneUndoStep)
{
    auto& manager = ProjectManager::instance();
    auto* bus = create("TargetBus", T::Bus, manager.getMasterBusItem());
    auto* inheritedBus = create("InheritedBus", T::Bus, manager.getMasterBusItem());
    auto* inheritedSendBus = create("InheritedSendBus", T::Bus, manager.getMasterBusItem());
    auto* layer = create("DestinationLayer", T::Layer);
    auto* container = create("DestinationContainer", T::Container, layer);
    container->fieldGroups.mainOutput.shared = {FieldGroupMode::Local,
        MainOutputFieldGroup{toFieldGroupGuid(inheritedBus->guid), -3.0f}};
    const SendFieldGroup inheritedSends{{{toFieldGroupGuid(inheritedSendBus->guid), -6.0f}}};
    container->fieldGroups.send.shared = {FieldGroupMode::Local, inheritedSends};
    const auto busGuid = bus->guid;
    const auto busOutput = bus->fieldGroups.mainOutput;
    UndoRedoManager::instance().clear();
    PlatformContextService::instance().setSelected(
        {HarkenTest::PlatformIds::Windows}, HarkenTest::PlatformIds::Windows, false);
    const auto result = importFiles({wave("routed.wav", 0.6f)}, bus->objectID);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    expectCounts(result, 1, 0, 0);
    EXPECT_EQ(result.data.value(QStringLiteral("target_parent_authoring_handle")).toInteger(-1), 0);
    EXPECT_EQ(result.data.value(QStringLiteral("target_bus_authoring_handle")).toInteger(), bus->objectID);
    ASSERT_EQ(sounds().size(), 1);
    auto* sound = sounds().front();
    const auto guid = sound->guid;
    const auto sourceID = sound->sourceID;
    const auto expectEffectiveRoutes = [&](const ProjectItem* target, const ProjectItem* provider,
                                            float volume) {
        for (const auto platform : {HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android}) {
            const auto snapshot = buildFieldGroupEffectiveSnapshot(
                *manager.currentProject(), toFieldGroupScope(PlatformScope{platform}));
            ASSERT_TRUE(snapshot.ok());
            const auto found = snapshot.definitions.find(toFieldGroupGuid(sound->guid));
            ASSERT_NE(found, snapshot.definitions.end());
            EXPECT_EQ(found->second.mainOutput.mainOutputBusRef, toFieldGroupGuid(target->guid));
            EXPECT_FLOAT_EQ(found->second.mainOutput.outputBusVolume, volume);
            EXPECT_EQ(found->second.mainOutputSource.definition, toFieldGroupGuid(provider->guid));
            EXPECT_FALSE(found->second.mainOutputSource.isDefault);
            EXPECT_FALSE(found->second.mainOutputSource.scope.has_value());
            EXPECT_EQ(found->second.send, provider == container ? inheritedSends : SendFieldGroup{});
            EXPECT_EQ(found->second.sendSource.definition, toFieldGroupGuid(provider->guid));
            EXPECT_FALSE(found->second.sendSource.isDefault);
            EXPECT_FALSE(found->second.sendSource.scope.has_value());
        }
    };
    const auto expectOutput = [&]() {
        EXPECT_EQ(sound->parent, nullptr);
        EXPECT_TRUE(bus->children.empty());
        EXPECT_EQ(bus->fieldGroups.mainOutput, busOutput);
        EXPECT_EQ(sound->fieldGroups.mainOutput.shared.mode, FieldGroupMode::Inherit);
        EXPECT_EQ(sound->fieldGroups.send.shared.mode, FieldGroupMode::Inherit);
        ASSERT_TRUE(sound->fieldGroups.mainOutput.shared.localHistory.has_value());
        EXPECT_EQ(sound->fieldGroups.mainOutput.shared.localHistory->mainOutputBusRef,
            toFieldGroupGuid(busGuid));
        EXPECT_FLOAT_EQ(sound->fieldGroups.mainOutput.shared.localHistory->outputBusVolume, 0.0f);
        EXPECT_TRUE(sound->fieldGroups.mainOutput.platforms.empty());
        expectRegisteredWave(sourceID);
        expectEffectiveRoutes(bus, sound, 0.0f);
    };
    expectOutput();
    ASSERT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainAudioImportEvents();
    EXPECT_TRUE(sounds().empty());
    EXPECT_FALSE(AssetRegistry::instance().contains(sourceID));
    EXPECT_EQ(bus->fieldGroups.mainOutput, busOutput);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    drainAudioImportEvents();
    ASSERT_EQ(sounds().size(), 1);
    sound = sounds().front();
    EXPECT_EQ(sound->guid, guid);
    EXPECT_EQ(sound->sourceID, sourceID);
    expectOutput();
    UndoRedoManager::instance().push(new MoveItemCommand(sound, container));
    drainAudioImportEvents();
    EXPECT_EQ(sound->parent, container);
    EXPECT_EQ(sound->fieldGroups.mainOutput.shared.mode, FieldGroupMode::Inherit);
    EXPECT_EQ(sound->fieldGroups.send.shared.mode, FieldGroupMode::Inherit);
    EXPECT_EQ(sound->sourceID, sourceID);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 2);
    expectEffectiveRoutes(inheritedBus, container, -3.0f);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainAudioImportEvents();
    EXPECT_TRUE(container->children.empty());
    EXPECT_EQ(sound->guid, guid);
    expectOutput();
    auto local = sound->fieldGroups;
    local.mainOutput.shared.mode = FieldGroupMode::Local;
    local.send.shared.mode = FieldGroupMode::Local;
    ASSERT_TRUE(manager.restoreFieldGroupsInternal(sound, local));
    UndoRedoManager::instance().push(new MoveItemCommand(sound, container));
    drainAudioImportEvents();
    EXPECT_EQ(sound->parent, container);
    EXPECT_EQ(sound->fieldGroups, local);
    expectEffectiveRoutes(bus, sound, 0.0f);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainAudioImportEvents();
    EXPECT_EQ(sound->parent, nullptr);
    EXPECT_EQ(sound->fieldGroups, local);
    expectEffectiveRoutes(bus, sound, 0.0f);
}

TEST_F(BlueprintAudioImportTest, BlankPartialBatchIgnoresLegacyCanvasRootAndCreatesOnlySuccessfulSounds)
{
    auto* layer = create("Layer", T::Layer);
    auto* container = create("Container", T::Container, layer);
    UndoRedoManager::instance().clear();
    const auto first = wave("first.wav", 0.1f);
    const auto second = wave("second.wav", 0.2f);
    const auto missing = QDir(directory.path()).filePath(QStringLiteral("missing.wav"));
    const auto result = importFiles({first, missing, second}, 0, container->objectID);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    expectCounts(result, 2, 1, 0);
    ASSERT_EQ(sounds().size(), 2);
    EXPECT_TRUE(container->children.empty());
    EXPECT_EQ(result.data.value(QStringLiteral("created_authoring_handles")).toArray().size(), 2);
    QList<uint32_t> sourceIDs;
    for (auto* sound : sounds()) {
        EXPECT_EQ(sound->parent, nullptr);
        sourceIDs.push_back(sound->sourceID);
        expectRegisteredWave(sound->sourceID);
    }
    EXPECT_EQ(AssetRegistry::instance().count(), 2);
    ASSERT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainAudioImportEvents();
    EXPECT_TRUE(sounds().empty());
    EXPECT_TRUE(container->children.empty());
    EXPECT_EQ(AssetRegistry::instance().count(), 0);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    drainAudioImportEvents();
    ASSERT_EQ(sounds().size(), 2);
    for (auto* sound : sounds()) {
        EXPECT_EQ(sound->parent, nullptr);
        EXPECT_TRUE(sourceIDs.contains(sound->sourceID));
        expectRegisteredWave(sound->sourceID);
    }
}

TEST_F(BlueprintAudioImportTest, SameContentFilesCreateDistinctChildrenWhileSharingOneAsset)
{
    auto* layer = create("Layer", T::Layer);
    auto* container = create("Container", T::Container, layer);
    UndoRedoManager::instance().clear();
    const auto first = wave("first.wav", 0.25f);
    const auto second = wave("second.wav", 0.25f);
    const auto result = importFiles({first, second}, container->objectID);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    expectCounts(result, 2, 0, 1);
    ASSERT_EQ(container->children.size(), 2u);
    const auto* firstSound = container->children[0].get();
    const auto* secondSound = container->children[1].get();
    EXPECT_NE(firstSound->guid, secondSound->guid);
    EXPECT_NE(firstSound->objectID, secondSound->objectID);
    EXPECT_EQ(firstSound->sourceID, secondSound->sourceID);
    const auto sourceID = firstSound->sourceID;
    EXPECT_EQ(AssetRegistry::instance().count(), 1);
    EXPECT_EQ(importedFiles().size(), 1);
    EXPECT_EQ(result.data.value(QStringLiteral("created_authoring_handles")).toArray().size(), 2);
    expectRegisteredWave(sourceID);
    ASSERT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainAudioImportEvents();
    EXPECT_TRUE(container->children.empty());
    EXPECT_FALSE(AssetRegistry::instance().contains(sourceID));
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    drainAudioImportEvents();
    ASSERT_EQ(container->children.size(), 2u);
    EXPECT_EQ(container->children[0]->sourceID, sourceID);
    EXPECT_EQ(container->children[1]->sourceID, sourceID);
    EXPECT_EQ(AssetRegistry::instance().count(), 1);
    expectRegisteredWave(sourceID);
}

TEST_F(BlueprintAudioImportTest, SoundCanAppendSharedContentAgainWithoutDuplicatingTheAsset)
{
    auto* layer = create("Layer", T::Layer);
    auto* sound = create("Sound", T::Sound, layer);
    const auto input = wave("shared.wav", 0.35f);
    auto result = importFiles({input}, sound->objectID);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    auto* component = sound->comp<VSound>();
    ASSERT_EQ(component->sources.plain.entries.size(), 1u);
    const auto original = component->sources.plain.entries.front();
    UndoRedoManager::instance().clear();
    result = importFiles({input}, sound->objectID);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    expectCounts(result, 1, 0, 1);
    ASSERT_EQ(component->sources.plain.entries.size(), 2u);
    const auto appended = component->sources.plain.entries.back();
    EXPECT_NE(appended.entryId, original.entryId);
    EXPECT_EQ(appended.sourceId, original.sourceId);
    EXPECT_EQ(AssetRegistry::instance().count(), 1);
    EXPECT_EQ(layer->children.size(), 1u);
    EXPECT_TRUE(result.data.value(QStringLiteral("created_authoring_handles")).toArray().isEmpty());
    ASSERT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    drainAudioImportEvents();
    ASSERT_EQ(component->sources.plain.entries.size(), 1u);
    EXPECT_EQ(component->sources.plain.entries.front(), original);
    expectRegisteredWave(original.sourceId);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    drainAudioImportEvents();
    ASSERT_EQ(component->sources.plain.entries.size(), 2u);
    EXPECT_EQ(component->sources.plain.entries.back(), appended);
    EXPECT_EQ(AssetRegistry::instance().count(), 1);
    expectRegisteredWave(original.sourceId);
}

TEST_F(BlueprintAudioImportTest, RepeatedPathInOneDropImportsOnlyOnce)
{
    const auto input = wave("repeated.wav", 0.45f);
    const auto result = importFiles({input, input});
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    expectCounts(result, 1, 0, 0);
    ASSERT_EQ(sounds().size(), 1);
    EXPECT_EQ(AssetRegistry::instance().count(), 1);
    EXPECT_EQ(result.data.value(QStringLiteral("created_authoring_handles")).toArray().size(), 1);
    EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), 1);
    expectRegisteredWave(sounds().front()->sourceID);
}

}
