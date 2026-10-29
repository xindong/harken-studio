#include "Core/CanonicalPackSetExport.h"
#include "Core/Components/VSelection.h"
#include "Core/ActionManager.h"
#include "Core/AssetPipeline/AssetConverter.h"
#include "Core/AssetPipeline/AssetImporter.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AssetPipeline/PathUtils.h"
#include "Core/Components/VAction.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VTrack.h"
#include "Core/IPC/IPCServer.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectBuildSnapshot.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectPackExporter.h"
#include "Core/RuntimeContentBundleBuilderInternal.h"
#include "Core/UndoCommands.h"

#include <harken/media/PackFormat.h>
#include <harken/media/PackFormatV2.h>

#include <gtest/gtest.h>
#include <QApplication>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QMap>
#include <QProcess>
#include <QTemporaryDir>
#include <QScopeGuard>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

#include <algorithm>
#include <cmath>
#include <memory>
#include <vector>

namespace {
using namespace VNX;
namespace Platforms = ProjectPlatformSemantics;

QByteArray bytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

QMap<QString, QByteArray> directoryContents(const QString& root, const QStringList& filters = {})
{
    QMap<QString, QByteArray> result;
    QDirIterator files(root, filters, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString path = files.next();
        const QString relative = QDir(root).relativeFilePath(path);
        if (!relative.startsWith(QStringLiteral(".cache/")))
            result.insert(relative, bytes(path));
    }
    return result;
}

struct HarkenizerResult
{
    bool finished = false;
    int exitCode = -1;
    QByteArray output;
    QByteArray error;
};

HarkenizerResult runHarkenizer(const QStringList& arguments)
{
    QProcess process;
    process.start(QString::fromUtf8(HARKENIZER_PATH), arguments);
    const bool finished = process.waitForStarted(10000) && process.waitForFinished(60000);
    if (!finished) {
        process.kill();
        process.waitForFinished(1000);
    }
    return {finished && process.exitStatus() == QProcess::NormalExit, process.exitCode(),
        process.readAllStandardOutput(), process.readAllStandardError()};
}

QVector<harken::PackTocEntry> mediaEntries(const QString& path)
{
    QVector<harken::PackTocEntry> result;
    const auto content = bytes(path);
    if (content.size() < static_cast<qsizetype>(harken::kPackV2HeaderBytes)) return result;
    const auto* data = reinterpret_cast<const uint8_t*>(content.constData());
    harken::PackV2Header header;
    if (!harken::ParsePackV2Header(data, &header)) return result;
    for (uint32_t i = 0; i < header.entryCount; ++i) {
        const auto entry = harken::ParsePackV2DirectoryEntry(
            data + header.directoryOffset + i * harken::kPackV2DirectoryEntryBytes);
        if (entry.kind == harken::kPackEntryKindMedia)
            result.append(harken::ParsePackTocEntry(data + entry.offset));
    }
    return result;
}

#ifdef Q_OS_WIN
TEST(RuntimeContentBundleActivation, SharingLockPreservesDirectories)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString source = directory.filePath(QStringLiteral("staged"));
    const QString target = directory.filePath(QStringLiteral("Bundles/candidate"));
    ASSERT_TRUE(QDir().mkpath(source));
    ASSERT_TRUE(QDir().mkpath(QFileInfo(target).absolutePath()));
    const QString sourceFile = QDir(source).filePath(QStringLiteral("content.bytes"));
    {
        QFile file(sourceFile);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write("captured content"), 16);
    }
    const QString nativeSourceFile = QDir::toNativeSeparators(sourceFile);
    HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(nativeSourceFile.utf16()), GENERIC_READ,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    const auto closeHandle = qScopeGuard([&]() {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle);
    });
    QString error;
    EXPECT_FALSE(activateRuntimeContentBundleDirectory(source, target, &error));
    EXPECT_TRUE(QFileInfo(source).isDir());
    EXPECT_TRUE(QFileInfo::exists(sourceFile));
    EXPECT_FALSE(QFileInfo::exists(target));

    ASSERT_TRUE(CloseHandle(handle));
    handle = INVALID_HANDLE_VALUE;
    EXPECT_EQ(bytes(sourceFile), QByteArray("captured content"));
    ASSERT_TRUE(activateRuntimeContentBundleDirectory(source, target, &error)) << error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(source));
    EXPECT_EQ(bytes(QDir(target).filePath(QStringLiteral("content.bytes"))), QByteArray("captured content"));
}
#endif

class ProjectPlatformBuild : public testing::Test
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
        config.name = "PlatformBuild";
        config.location = directory.path();
        ASSERT_TRUE(manager().createProject(config));
        ActionManager::instance().clearAllForTesting();
        registerProjectActions(ActionManager::instance(), &manager(), nullptr);
        low = createPlatform("Android_Low");
        high = createPlatform("Android_High");
        ASSERT_FALSE(low.isNull());
        ASSERT_FALSE(high.isNull());
        setRate(low, 22050);
        ProjectPlatformProfile highProfile;
        highProfile.performResample = true;
        highProfile.sizePolicies.front().config.targetSampleRate = 44100;
        ASSERT_TRUE(run("set_project_platform_profile", {{"platform_id", Platforms::idString(high)},
            {"profile", highProfile.toJson()}}).success);
        layer = manager().createItem(nullptr, "MediaLayer", ProjectItem::Type::Layer);
        sound = manager().createItem(layer, "Tone", ProjectItem::Type::Sound);
        action = manager().createItem(nullptr, "PlayTone", ProjectItem::Type::Action);
        ASSERT_NE(sound, nullptr);
        ASSERT_NE(action, nullptr);
        VActionStep step;
        step.targetObjectID = sound->objectID;
        action->comp<VAction>()->steps.push_back(step);
        source = addAsset(101, 0.3f);
        auto* component = sound->comp<VSound>();
        component->sources.plain.entries = {{QUuid::createUuid(), source.sourceID, {}}};
        component->sources.plain.active = component->sources.plain.entries.front().entryId;
        sound->sourceID = source.sourceID;
        UndoRedoManager::instance().clear();
    }

    void TearDown() override
    {
        QCoreApplication::processEvents();
        UndoRedoManager::instance().clear();
        if (manager().isProjectOpen()) manager().closeProject();
        ActionManager::instance().clearAllForTesting();
    }

    ProjectManager& manager() { return ProjectManager::instance(); }
    ProjectData& project() { return *manager().currentProject(); }
    ActionResult run(const QString& name, const QJsonObject& args)
    {
        return ActionManager::instance().execute(name, args, ActionSource::IPC);
    }
    ProjectPlatformId createPlatform(const QString& name)
    {
        const auto result = run("create_project_platform", {{"name", name}, {"base_platform", "Android"}});
        EXPECT_TRUE(result.success) << result.errorMessage.toStdString();
        return QUuid(result.data.value("platform_id").toString());
    }
    AssetInfo addAsset(uint32_t id, float amplitude)
    {
        AssetInfo asset;
        asset.sourceID = id;
        asset.relativePath = QString::number(id) + ".wav";
        asset.absolutePath = QDir(PathUtils::getAudioSourcesDir(AssetRegistry::instance().projectRoot()))
            .filePath(asset.relativePath);
        std::vector<float> samples(4800 * 2);
        for (size_t i = 0; i < 4800; ++i) {
            samples[i * 2] = amplitude * std::sin(static_cast<float>(i) * 0.07f);
            samples[i * 2 + 1] = amplitude * std::cos(static_cast<float>(i) * 0.13f);
        }
        EXPECT_TRUE(AssetConverter::writeFloatWav(asset.absolutePath, samples, 2, 48000));
        AssetImporter importer;
        asset.metadata = importer.analyzeFile(asset.absolutePath);
        asset.contentHash = AssetImporter::computeContentHash(asset.absolutePath);
        EXPECT_TRUE(AssetRegistry::instance().registerAsset(asset));
        AudioSourceData entry;
        entry.sourceID = id;
        entry.relativePath = asset.relativePath;
        project().audioSources.push_back(entry);
        return asset;
    }
    void setRate(ProjectPlatformId id, uint32_t rate)
    {
        ProjectPlatformProfile profile;
        profile.performResample = true;
        profile.sizePolicies.front().config.targetSampleRate = rate;
        ASSERT_TRUE(run("set_project_platform_profile", {{"platform_id", Platforms::idString(id)},
            {"profile", profile.toJson()}}).success);
    }
    ProjectPackExporter::ExportResult exportPlatform(ProjectPlatformId id, const QString& path,
        std::vector<ProjectPackExporter::PackBuildRecord>& records)
    {
        ProjectPackExporter exporter;
        ProjectPackExporter::ExportOptions options;
        options.platform = Platforms::idString(id);
        return exporter.ExportAllPacks(project(), path, options, &records);
    }
    QString contentPath(const std::vector<ProjectPackExporter::PackBuildRecord>& records)
    {
        for (const auto& record : records)
            if (record.objectID == action->objectID) return record.outputPath;
        return {};
    }

    QMap<QString, QByteArray> publicationBytes(const QString& path, ProjectPlatformId id)
    {
        QJsonArray rows;
        QStringList files;
        QString error;
        EXPECT_TRUE(ReadPackPublication(path, project().projectId, Platforms::idString(id),
            &rows, &files, &error)) << error.toStdString();
        QMap<QString, QByteArray> result;
        for (const auto& file : files) result.insert(file, bytes(QDir(path).filePath(file)));
        return result;
    }

    QTemporaryDir directory;
    ProjectPlatformId low, high;
    ProjectItem* layer = nullptr;
    ProjectItem* sound = nullptr;
    ProjectItem* action = nullptr;
    AssetInfo source;
};

TEST_F(ProjectPlatformBuild, ChangedProfileDoesNotJoinOrPublishAnOlderJob)
{
    AssetConverter converter;
    auto before = converter.convertAsync(source.sourceID, low);
    setRate(low, 32000);
    auto after = converter.convertAsync(source.sourceID, low);
    before.waitForFinished();
    after.waitForFinished();
    ASSERT_TRUE(before.result().success);
    ASSERT_TRUE(after.result().success);
    EXPECT_NE(before.result().cachePath, after.result().cachePath);
    QCoreApplication::processEvents();
    const auto* asset = AssetRegistry::instance().getAsset(source.sourceID);
    ASSERT_NE(asset, nullptr);
    EXPECT_EQ(asset->platformCaches.value(low).cachePath, after.result().cachePath);
}

TEST_F(ProjectPlatformBuild, DeleteAndUndoRejectsThePreviousPlatformLifetime)
{
    AssetConverter converter;
    auto future = converter.convertAsync(source.sourceID, low);
    ASSERT_TRUE(run("delete_project_platform", {{"platform_id", Platforms::idString(low)}}).success);
    UndoRedoManager::instance().undo();
    ASSERT_NE(Platforms::find(project().platforms, low), nullptr);
    future.waitForFinished();
    ASSERT_TRUE(future.result().success);
    QCoreApplication::processEvents();
    EXPECT_FALSE(AssetRegistry::instance().getAsset(source.sourceID)->platformCaches.contains(low));
    ASSERT_TRUE(converter.convert(source.sourceID, low).success);
}

TEST_F(ProjectPlatformBuild, ReopenedRegistryWithSamePathAndIdsRejectsLateCompletion)
{
    AssetConverter converter;
    auto future = converter.convertAsync(source.sourceID, low);
    const auto root = AssetRegistry::instance().projectRoot();
    AssetRegistry::instance().clear();
    AssetRegistry::instance().setProjectRoot(root);
    AssetRegistry::instance().setPlatforms(project().platforms);
    ASSERT_TRUE(AssetRegistry::instance().registerAsset(source));
    future.waitForFinished();
    ASSERT_TRUE(future.result().success);
    QCoreApplication::processEvents();
    EXPECT_FALSE(AssetRegistry::instance().getAsset(source.sourceID)->platformCaches.contains(low));
}

TEST_F(ProjectPlatformBuild, CancelIsScopedToTheRequestedInstance)
{
    AssetConverter converter;
    auto first = converter.convertAsync(source.sourceID, low);
    auto second = converter.convertAsync(source.sourceID, high);
    converter.cancelJob(source.sourceID, low);
    first.waitForFinished();
    second.waitForFinished();
    ASSERT_TRUE(second.result().success);
    QCoreApplication::processEvents();
    EXPECT_FALSE(AssetRegistry::instance().getAsset(source.sourceID)->platformCaches.contains(low));
    EXPECT_TRUE(AssetRegistry::instance().getAsset(source.sourceID)->platformCaches.contains(high));
}

TEST_F(ProjectPlatformBuild, CapturedProjectBuildsAfterTheActiveProjectCloses)
{
    ASSERT_TRUE(manager().saveProject());
    const auto snapshot = ProjectBuildSnapshot::capture(project());
    ASSERT_TRUE(snapshot);
    const auto oldActionId = action->objectID;
    ASSERT_TRUE(manager().closeProject());
    sound = nullptr;
    action = nullptr;
    ProjectPackExporter exporter;
    ProjectPackExporter::ExportOptions options;
    options.platform = Platforms::idString(low);
    options.snapshot = snapshot;
    std::vector<ProjectPackExporter::PackBuildRecord> records;
    const auto result = exporter.ExportAllPacks(snapshot->project, directory.filePath("Closed"), options, &records);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    const auto record = std::find_if(records.begin(), records.end(), [oldActionId](const auto& value) {
        return value.objectID == oldActionId;
    });
    ASSERT_NE(record, records.end());
    EXPECT_FALSE(mediaEntries(record->outputPath).empty());
    QCoreApplication::processEvents();
}

TEST_F(ProjectPlatformBuild, IndependentBuildMatchesSavedProjectPublication)
{
    project().buildExportProjectData = true;
    project().buildExportPackMetadata = true;
    ASSERT_TRUE(manager().saveProject());
    const auto snapshot = ProjectBuildSnapshot::capture(project());
    ASSERT_TRUE(snapshot);
    const QString projectPath = project().projectPath;
    const QString projectRoot = QFileInfo(projectPath).absolutePath();
    const QString referenceRoot = directory.filePath("Reference");
    const QString consoleRoot = directory.filePath("Console");
    const QStringList platforms{Platforms::idString(low), Platforms::idString(high)};
    QVector<PackArtifactMetadata> artifacts;
    ProjectPackExporter exporter;
    for (const auto& id : platforms) {
        ProjectPackExporter::ExportOptions options;
        options.platform = id;
        options.snapshot = snapshot;
        const auto* definition = Platforms::find(snapshot->project.platforms, QUuid(id));
        ASSERT_NE(definition, nullptr);
        const auto result = exporter.ExportAllPacks(snapshot->project,
            ProjectPackExporter::platformOutputDir(referenceRoot, *definition), options);
        ASSERT_TRUE(result.success) << result.error.toStdString();
        artifacts.append(result.artifacts);
    }
    const auto finalization = ProjectPackExporter::FinalizePackExport(
        referenceRoot, snapshot->project, platforms, false, artifacts);
    ASSERT_TRUE(finalization.success) << finalization.error.toStdString();
    const auto reference = directoryContents(referenceRoot, {"*.bytes", "*.json"});
    ASSERT_GE(reference.size(), 9);
    ASSERT_TRUE(reference.contains(QStringLiteral("Project.json")));
    ASSERT_TRUE(manager().closeProject());
    const auto saved = directoryContents(projectRoot);

    const auto targets = runHarkenizer({"list-targets", projectPath, "--json"});
    ASSERT_TRUE(targets.finished) << targets.error.toStdString();
    ASSERT_EQ(targets.exitCode, 0) << targets.error.toStdString();
    QJsonParseError parseError;
    const auto targetDocument = QJsonDocument::fromJson(targets.output, &parseError);
    ASSERT_EQ(parseError.error, QJsonParseError::NoError) << targets.output.toStdString();
    ASSERT_TRUE(targetDocument.isObject());
    EXPECT_TRUE(targets.output.contains(platforms.front().toUtf8()));
    EXPECT_TRUE(targets.output.contains(platforms.back().toUtf8()));
    EXPECT_EQ(directoryContents(projectRoot), saved);

    QStringList arguments{"generate-packs", projectPath, "--all", "--self-clean", "off",
        "--platform-id", platforms.front(), "--platform-id", platforms.back(), "--output", consoleRoot};
    for (const bool force : {false, false, true}) {
        auto current = arguments;
        if (force) current.append("--force");
        const auto result = runHarkenizer(current);
        ASSERT_TRUE(result.finished) << result.error.toStdString();
        ASSERT_EQ(result.exitCode, 0) << result.error.toStdString() << result.output.toStdString();
        EXPECT_EQ(directoryContents(consoleRoot, {"*.bytes", "*.json"}), reference);
        EXPECT_EQ(directoryContents(projectRoot), saved);
    }
}

TEST_F(ProjectPlatformBuild, IndependentBuildRejectsUnavailableInputsWithoutChangingPublication)
{
    ASSERT_TRUE(manager().saveProject());
    const QString projectPath = project().projectPath;
    const QString projectRoot = QFileInfo(projectPath).absolutePath();
    const QString outputRoot = directory.filePath("ConsoleRejected");
    const QStringList arguments{"generate-packs", projectPath, "--all", "--self-clean", "off",
        "--platform-id", Platforms::idString(low), "--output", outputRoot};
    const auto saved = directoryContents(projectRoot);
    const auto occupied = runHarkenizer(arguments);
    ASSERT_TRUE(occupied.finished) << occupied.error.toStdString();
    EXPECT_EQ(occupied.exitCode, 1) << occupied.output.toStdString();
    EXPECT_FALSE(QFileInfo::exists(outputRoot));
    EXPECT_EQ(directoryContents(projectRoot), saved);
    ASSERT_TRUE(manager().closeProject());

    const auto incomplete = runHarkenizer({"generate-packs", projectPath});
    ASSERT_TRUE(incomplete.finished) << incomplete.error.toStdString();
    EXPECT_EQ(incomplete.exitCode, 2);
    EXPECT_FALSE(QFileInfo::exists(outputRoot));

    QString error;
    const auto lock = ProjectPackExporter::LockPackOutputRoot(outputRoot, &error);
    ASSERT_TRUE(lock) << error.toStdString();
    const auto original = directoryContents(outputRoot);
    const auto competing = runHarkenizer(arguments);
    ASSERT_TRUE(competing.finished) << competing.error.toStdString();
    EXPECT_EQ(competing.exitCode, 1) << competing.output.toStdString();
    EXPECT_EQ(directoryContents(outputRoot), original);
    EXPECT_EQ(directoryContents(projectRoot), saved);
}

TEST_F(ProjectPlatformBuild, SnapshotClonesComponentsLedgersAndParentAliases)
{
    auto* track = manager().createItem(layer, "Track", ProjectItem::Type::Track);
    auto* container = manager().createItem(layer, "Container", ProjectItem::Type::Container);
    ASSERT_NE(track, nullptr);
    ASSERT_NE(container, nullptr);
    PlatformOverrideLedgerV1 ledger;
    ASSERT_TRUE(ledger.ensure(low)->properties.setValue(
        harkenplayableproperties::PlayablePropertyIdV1::Volume, -6.0f));
    sound->platformOverrides = ledger;
    EXPECT_TRUE(container->comp<VNX::VSelection>()->selections.shared.setOccurrenceRows({{sound->guid, 0, 50}}));
    container->comp<VNX::VSelection>()->selections.shared.occurrenceOrdinalHighWater = 1;
    container->comp<VNX::VSelection>()->selections.editTable(low).entries.front().occurrences.front().weight = 25;
    const auto selectionsJson = container->comp<VNX::VSelection>()->selections.toJson();
    Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.targetGuid = sound->guid;
    clip.duration = 4800;
    clip.platformOverrides = ledger;
    track->comp<VTrack>()->clips.items.push_back(clip);
    const auto snapshot = ProjectBuildSnapshot::capture(project());
    ASSERT_TRUE(snapshot);
    const ProjectItem* copiedLayer = nullptr;
    for (const auto* binding : snapshot->project.allFileBindings())
        if (binding->rootItem && binding->rootItem->guid == layer->guid) copiedLayer = binding->rootItem.get();
    ASSERT_NE(copiedLayer, nullptr);
    EXPECT_NE(copiedLayer, layer);
    const auto findCopy = [&](const ProjectItem* original) -> const ProjectItem* {
        for (const auto& child : copiedLayer->children)
            if (child->guid == original->guid) return child.get();
        return nullptr;
    };
    const auto* copiedSound = findCopy(sound);
    const auto* copiedTrack = findCopy(track);
    const auto* copiedContainer = findCopy(container);
    ASSERT_TRUE(copiedSound && copiedTrack && copiedContainer);
    EXPECT_EQ(copiedSound->parent, copiedLayer);
    EXPECT_EQ(copiedSound->comp<VSound>()->hostItem(), copiedSound);
    EXPECT_EQ(copiedSound->comp<VMixNode>()->hostItem(), copiedSound);
    const auto trackJson = copiedTrack->comp<VTrack>()->toJson();
    const auto soundJson = copiedSound->comp<VSound>()->toJson();
    sound->platformOverrides.rows.clear();
    sound->comp<VSound>()->sources.plain.entries.clear();
    container->comp<VNX::VSelection>()->selections.shared.entries.clear();
    track->comp<VTrack>()->clips.items.clear();
    EXPECT_EQ(copiedSound->platformOverrides, ledger);
    EXPECT_EQ(copiedSound->comp<VSound>()->toJson(), soundJson);
    EXPECT_EQ(copiedTrack->comp<VTrack>()->toJson(), trackJson);
    EXPECT_EQ(copiedContainer->comp<VNX::VSelection>()->selections.toJson(), selectionsJson);
}

TEST_F(ProjectPlatformBuild, DeliveryRejectsCorruptPackHeaders)
{
    const QString root = directory.filePath("Delivery");
    const QString platformDir = QDir(root).filePath("Android_Low");
    const QString id = Platforms::idString(low);
    std::vector<ProjectPackExporter::PackBuildRecord> records;
    ASSERT_TRUE(exportPlatform(low, platformDir, records).success);
    const auto original = publicationBytes(platformDir, low);
    ASSERT_FALSE(original.isEmpty());
    QString error;
    ASSERT_TRUE(ProjectPackExporter::FinalizePackOutputs(root, project().platforms, {id}, {id}, &error));
    const QString packPath = contentPath(records);
    QFile corrupted(packPath);
    ASSERT_TRUE(corrupted.open(QIODevice::ReadWrite));
    ASSERT_TRUE(corrupted.seek(0));
    ASSERT_EQ(corrupted.write("BAD!"), 4);
    corrupted.close();
    EXPECT_FALSE(ProjectPackExporter::FinalizePackOutputs(root, project().platforms, {id}, {id}, &error));
}

TEST_F(ProjectPlatformBuild, CompetingOutputRequestsDoNotChangePublishedPairs)
{
    const QString root = directory.filePath("Delivery");
    std::vector<ProjectPackExporter::PackBuildRecord> records;
    ASSERT_TRUE(exportPlatform(low, QDir(root).filePath("Android_Low"), records).success);
    const auto id = Platforms::idString(low);
    QString error;
    ASSERT_TRUE(ProjectPackExporter::FinalizePackOutputs(root, project().platforms, {id}, {id}, &error));
    const auto original = publicationBytes(QDir(root).filePath("Android_Low"), low);
    const auto lock = ProjectPackExporter::LockPackOutputRoot(root, &error);
    ASSERT_NE(lock, nullptr);
    IPCServer server(nullptr);
    server.setProjectManager(&manager());
    server.setAppState(IPCServer::AppState::Ready);
    const auto rejected = server.dispatchCommand("export_pack", {{"output_path", root}, {"platform_id", id}});
    EXPECT_EQ(rejected.value("status").toString(), QStringLiteral("error"));
    EXPECT_EQ(publicationBytes(QDir(root).filePath("Android_Low"), low), original);
}

}
