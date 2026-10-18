#include "Core/PackSchemaGenerated.h"
#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Core/Components/VSelection.h"
#include "Core/ActionManager.h"
#include "Core/AssetPipeline/AssetConverter.h"
#include "Core/AssetPipeline/AssetGarbageCollector.h"
#include "Core/AssetPipeline/AssetImporter.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AssetPipeline/MediaPool.h"
#include "Core/AssetPipeline/PathUtils.h"
#include "Core/Components/VAction.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VSpatial.h"
#include "Core/Components/VTrack.h"
#include "Core/IPC/IPCServer.h"
#include "Core/MemorySettingsStorage.h"
#include "Core/PackBuildPlan.h"
#include "Core/PackGenerationRecord.h"
#include "Core/PlatformContextService.h"
#include "Core/ProjectBuildSnapshot.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectPlatformActivity.h"
#include "Core/SoundSourceDerivative.h"
#include "Core/ProjectPackExporter.h"
#include "Core/ProjectSerializer.h"
#include "Core/RuntimeContentBundleBuilder.h"
#include "Core/RuntimeContentBundleBuilderInternal.h"
#include "Core/UndoCommands.h"
#include "RuntimeBridge/PackagingBridge.h"

#include <harken/media/PackFormat.h>
#include <harken/media/PackFormatV2.h>
#include <harken/media/PackLogicImage.h>

#include <gtest/gtest.h>
#include <QApplication>
#include <QDirIterator>
#include <QFile>
#include <QElapsedTimer>
#include <QThread>
#include <QJsonDocument>
#include <QTemporaryDir>
#include <QScopeGuard>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

#include <cmath>
#include <thread>

namespace {
using namespace VNX;
namespace Platforms = ProjectPlatformSemantics;

QByteArray bytes(const QString& path)
{
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly)) return {};
    return file.readAll();
}

void preservePlatformUnityFixture(const QString& sourceRoot)
{
    const QByteArray targetUtf8 = qgetenv("HARKEN_UNITY_PLATFORM_FIXTURE_OUT");
    if (targetUtf8.isEmpty()) return;
    const QString targetRoot = QString::fromUtf8(targetUtf8);
    QDir target(targetRoot);
    if (target.exists()) ASSERT_TRUE(target.removeRecursively());
    ASSERT_TRUE(QDir().mkpath(targetRoot));
    QDirIterator files(sourceRoot, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString source = files.next();
        const QString relative = QDir(sourceRoot).relativeFilePath(source);
        const QString destination = target.filePath(relative);
        ASSERT_TRUE(QDir().mkpath(QFileInfo(destination).absolutePath()));
        ASSERT_TRUE(QFile::copy(source, destination)) << destination.toStdString();
    }
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
TEST(RuntimeContentBundleActivation, TemporarySharingLockRetriesWithoutLosingContent)
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
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(nativeSourceFile.utf16()), GENERIC_READ,
        0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    std::jthread releaseLock([handle] {
        QThread::msleep(100);
        CloseHandle(handle);
    });

    QString error;
    ASSERT_TRUE(activateRuntimeContentBundleDirectory(source, target, &error)) << error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(source));
    EXPECT_EQ(bytes(QDir(target).filePath(QStringLiteral("content.bytes"))), QByteArray("captured content"));
}

TEST(RuntimeContentBundleActivation, SharingLockReportsNativeFailureAndPreservesDirectories)
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
    const bool sharingViolation =
        error.contains(QStringLiteral("Windows error %1").arg(ERROR_SHARING_VIOLATION));
    const bool accessDenied =
        error.contains(QStringLiteral("Windows error %1").arg(ERROR_ACCESS_DENIED));
    EXPECT_TRUE(sharingViolation || accessDenied) << error.toStdString();
    EXPECT_TRUE(error.contains(source)) << error.toStdString();
    EXPECT_TRUE(error.contains(target)) << error.toStdString();
    EXPECT_TRUE(error.contains(QStringLiteral("sourceDir=1, targetExists=0, targetParentDir=1")))
        << error.toStdString();
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
        registerProjectActions(ActionManager::instance(), &manager(), nullptr, {});
        windows = project().platforms.front().id;
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
    void expectAudio(const ConversionResult& result, int rate, int channels)
    {
        ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
        std::vector<float> decoded;
        int actualRate = 0, actualChannels = 0;
        ASSERT_TRUE(HarkenEditor::Bridge::decodeAudioFileInterleaved(
            result.absolutePath, decoded, actualChannels, actualRate));
        EXPECT_EQ(actualRate, rate);
        EXPECT_EQ(actualChannels, channels);
        EXPECT_FALSE(decoded.empty());
    }
    ProjectPackExporter::ExportResult exportPlatform(ProjectPlatformId id, const QString& path,
        std::vector<ProjectPackExporter::PackBuildRecord>& records,
        const std::shared_ptr<const ProjectBuildSnapshot>& snapshot = {})
    {
        ProjectPackExporter exporter;
        ProjectPackExporter::ExportOptions options;
        options.platform = Platforms::idString(id);
        options.snapshot = snapshot;
        return exporter.ExportAllPacks(project(), path, options, &records);
    }
    QString contentPath(const std::vector<ProjectPackExporter::PackBuildRecord>& records)
    {
        for (const auto& record : records)
            if (record.objectID == action->objectID) return record.outputPath;
        return {};
    }

    QTemporaryDir directory;
    ProjectPlatformId windows, low, high;
    ProjectItem* layer = nullptr;
    ProjectItem* sound = nullptr;
    ProjectItem* action = nullptr;
    AssetInfo source;
};

TEST_F(ProjectPlatformBuild, ExportsGameplayIdentityFixturesThroughRename)
{
    ASSERT_TRUE(QDir().mkpath(QString::fromUtf8(HARKEN_TEMP_DIR)));
    QTemporaryDir fixtureDirectory(QDir(QString::fromUtf8(HARKEN_TEMP_DIR)).filePath(
        QStringLiteral("gameplay-XXXXXX")));
    ASSERT_TRUE(fixtureDirectory.isValid()) << fixtureDirectory.errorString().toStdString();
    fixtureDirectory.setAutoRemove(false);
    const QString root = fixtureDirectory.path();
    QString originalProject;
    QString originalAction;
    for (int projectIndex = 0; projectIndex < 2; ++projectIndex) {
        if (projectIndex != 0) {
            ASSERT_TRUE(manager().closeProject());
            ProjectConfig config;
            config.name = QStringLiteral("GameplayOther");
            config.location = directory.path();
            ASSERT_TRUE(manager().createProject(config));
            windows = project().platforms.front().id;
            layer = manager().createItem(nullptr, "MediaLayer", ProjectItem::Type::Layer);
            sound = manager().createItem(layer, "Tone", ProjectItem::Type::Sound);
            action = manager().createItem(nullptr, "PlayTone", ProjectItem::Type::Action);
            ASSERT_NE(sound, nullptr);
            ASSERT_NE(action, nullptr);
            source = addAsset(101, 0.3f);
            auto* component = sound->comp<VSound>();
            component->sources.plain.entries = {{QUuid::createUuid(), source.sourceID, {}}};
            component->sources.plain.active = component->sources.plain.entries.front().entryId;
            sound->sourceID = source.sourceID;
            VActionStep play;
            play.targetObjectID = sound->objectID;
            action->comp<VAction>()->steps.push_back(play);
        }
        ASSERT_TRUE(manager().renameItem(action, QStringLiteral("Play_U4_A")));
        auto* secondSound = manager().createItem(layer, "ToneB", ProjectItem::Type::Sound);
        auto* second = manager().createItem(nullptr, "Play_U4_B", ProjectItem::Type::Action);
        auto* caller = manager().createItem(nullptr, "Play_U4_Caller", ProjectItem::Type::Action);
        ASSERT_NE(secondSound, nullptr);
        ASSERT_NE(second, nullptr);
        ASSERT_NE(caller, nullptr);
        const auto secondSource = addAsset(102, 0.5f);
        auto* component = secondSound->comp<VSound>();
        component->sources.plain.entries = {{QUuid::createUuid(), secondSource.sourceID, {}}};
        component->sources.plain.active = component->sources.plain.entries.front().entryId;
        secondSound->sourceID = secondSource.sourceID;
        VActionStep play;
        play.targetObjectID = secondSound->objectID;
        second->comp<VAction>()->steps.push_back(play);
        VActionStep post;
        post.type = VActionStep::Type::PostAction;
        post.targetObjectID = second->objectID;
        caller->comp<VAction>()->steps.push_back(post);
        ASSERT_TRUE(manager().saveProject());
        const QString projectPath = project().projectPath;
        const QUuid actionGuid = action->guid;
        const QString firstDir = QDir(root).filePath(projectIndex == 0 ? "U4Original" : "U4OtherProject");
        std::vector<ProjectPackExporter::PackBuildRecord> records;
        const auto exported = exportPlatform(windows, firstDir, records);
        ASSERT_TRUE(exported.success) << exported.error.toStdString();
        const auto manifest = QJsonDocument::fromJson(bytes(QDir(firstDir).filePath("HarkenPackManifest.json"))).object();
        const auto catalog = manifest.value("gameplayCatalog").toObject();
        ASSERT_EQ(catalog.value("schema").toInt(), 2);
        const QString projectId = catalog.value("projectId").toString();
        const QString actionId = objectIdString(action->runtimeId());
        const auto checkActions = [](const QJsonObject& projection, ProjectItem* a,
                                     ProjectItem* b, ProjectItem* c) {
            const auto rows = projection.value("actions").toArray();
            ASSERT_EQ(rows.size(), 3);
            for (const auto* authored : {a, b, c}) {
                int matches = 0;
                for (const auto& value : rows) {
                    const auto row = value.toObject();
                    if (row.value("name").toString() != authored->name) continue;
                    ++matches;
                    const QString id = row.value("actionStableId").toString();
                    EXPECT_FALSE(QUuid(id).isNull());
                    EXPECT_EQ(id, objectIdString(authored->runtimeId()));
                    EXPECT_EQ(row.value("nameBindingRevision").toString(),
                              QString::number(authored->nameBindingRevision));
                }
                EXPECT_EQ(matches, 1) << authored->name.toStdString();
            }
        };
        checkActions(catalog, action, second, caller);
        const auto retiredBefore = catalog.value("retired").toArray();
        EXPECT_TRUE(retiredBefore.isEmpty());
        QString secondOwner;
        QJsonArray callerDependencies;
        for (const auto& value : manifest.value("packs").toArray()) {
            const auto pack = value.toObject();
            if (pack.value("name").toString() == second->name)
                secondOwner = pack.value("packStableId").toString();
            if (pack.value("name").toString() == caller->name)
                callerDependencies = pack.value("dependencies").toArray();
        }
        ASSERT_FALSE(secondOwner.isEmpty());
        EXPECT_TRUE(callerDependencies.contains(QJsonValue(secondOwner)));
        if (projectIndex != 0) {
            EXPECT_NE(projectId, originalProject);
            EXPECT_NE(actionId, originalAction);
            continue;
        }
        originalProject = projectId;
        originalAction = actionId;
        const QString repeatedDir = QDir(root).filePath("U4Repeated");
        records.clear();
        const auto repeated = exportPlatform(windows, repeatedDir, records);
        ASSERT_TRUE(repeated.success) << repeated.error.toStdString();
        for (const auto& file : QDir(firstDir).entryList({"*.bytes", "*.json"}, QDir::Files))
            EXPECT_EQ(bytes(QDir(firstDir).filePath(file)), bytes(QDir(repeatedDir).filePath(file))) << file.toStdString();
        const QUuid secondGuid = second->guid;
        const QUuid callerGuid = caller->guid;
        const uint64_t priorRevision = action->nameBindingRevision;
        ASSERT_TRUE(manager().renameItem(action, QStringLiteral("Play_U4_Renamed")));
        ASSERT_TRUE(manager().saveProject());
        ASSERT_TRUE(manager().closeProject());
        ASSERT_TRUE(manager().loadProject(projectPath));
        action = manager().findItemByGuid(actionGuid);
        ASSERT_NE(action, nullptr);
        EXPECT_EQ(objectIdString(action->runtimeId()), originalAction);
        EXPECT_EQ(action->nameBindingRevision, priorRevision + 1);
        second = manager().findItemByGuid(secondGuid);
        caller = manager().findItemByGuid(callerGuid);
        ASSERT_NE(second, nullptr);
        ASSERT_NE(caller, nullptr);
        const QString renamedDir = QDir(root).filePath("U4Renamed");
        records.clear();
        const auto renamed = exportPlatform(windows, renamedDir, records);
        ASSERT_TRUE(renamed.success) << renamed.error.toStdString();
        const auto renamedCatalog = QJsonDocument::fromJson(bytes(QDir(renamedDir).filePath("HarkenPackManifest.json")))
            .object().value("gameplayCatalog").toObject();
        EXPECT_EQ(renamedCatalog.value("schema").toInt(), 2);
        EXPECT_EQ(renamedCatalog.value("projectId").toString(), originalProject);
        checkActions(renamedCatalog, action, second, caller);
        const auto retiredRows = renamedCatalog.value("retired").toArray();
        EXPECT_TRUE(retiredRows.isEmpty());
    }
    qInfo().noquote() << "Gameplay fixture root:" << root;
}

TEST_F(ProjectPlatformBuild, SiblingAndroidProfilesProduceIndependentPcmAndCaches)
{
    AssetConverter converter;
    const auto lowResult = converter.convert(source.sourceID, low);
    const auto highResult = converter.convert(source.sourceID, high);
    expectAudio(lowResult, 22050, 2);
    expectAudio(highResult, 44100, 2);
    EXPECT_NE(bytes(lowResult.absolutePath), bytes(highResult.absolutePath));
    EXPECT_TRUE(lowResult.cachePath.startsWith(Platforms::idString(low) + '/'));
    EXPECT_TRUE(highResult.cachePath.startsWith(Platforms::idString(high) + '/'));
    EXPECT_TRUE(converter.convert(source.sourceID, low).cacheHit);
    EXPECT_TRUE(converter.convert(source.sourceID, high).cacheHit);
    ASSERT_TRUE(run("rename_project_platform", {{"platform_id", Platforms::idString(low)}, {"name", "Mobile_Low"}}).success);
    EXPECT_EQ(converter.convert(source.sourceID, low).cachePath, lowResult.cachePath);
    setRate(low, 32000);
    EXPECT_FALSE(AssetRegistry::instance().isCacheValid(source.sourceID, low));
    EXPECT_TRUE(AssetRegistry::instance().isCacheValid(source.sourceID, high));
    expectAudio(converter.convert(source.sourceID, low), 32000, 2);
}

TEST_F(ProjectPlatformBuild, ResampleSwitchInvalidatesCachesAndKeepsCapturedBuildSettings)
{
    auto definition = *Platforms::find(project().platforms, low);
    definition.profile.sizePolicies.front().maxBytes = source.metadata.fileSize;
    definition.profile.sizePolicies.push_back({std::nullopt, {}});
    const auto rows = definition.profile.toJson().value("sizePolicies");
    ASSERT_TRUE(run("set_project_platform_profile", {{"platform_id", Platforms::idString(low)},
        {"profile", definition.profile.toJson()}}).success);
    AssetConverter converter;
    expectAudio(converter.convert(source.sourceID, low), 22050, 2);
    QString error;
    const auto snapshot = ProjectBuildSnapshot::capture(project(), &error);
    ASSERT_NE(snapshot, nullptr) << error.toStdString();
    definition.profile.performResample = false;
    ASSERT_TRUE(run("set_project_platform_profile", {{"platform_id", Platforms::idString(low)},
        {"profile", definition.profile.toJson()}}).success);
    EXPECT_EQ(Platforms::find(project().platforms, low)->profile.toJson().value("sizePolicies"), rows);
    EXPECT_FALSE(AssetRegistry::instance().isCacheValid(source.sourceID, low));
    expectAudio(converter.convert(source.sourceID, low), 48000, 2);
    std::vector<ProjectPackExporter::PackBuildRecord> records;
    ASSERT_TRUE(exportPlatform(low, directory.filePath("CapturedResample"), records, snapshot).success);
    EXPECT_EQ(mediaEntries(contentPath(records)).front().sampleRate, 22050u);
    records.clear();
    ASSERT_TRUE(exportPlatform(low, directory.filePath("Unprocessed"), records).success);
    EXPECT_EQ(mediaEntries(contentPath(records)).front().sampleRate, 48000u);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    expectAudio(converter.convert(source.sourceID, low), 22050, 2);
}

TEST_F(ProjectPlatformBuild, SameSettingsStillHaveSeparateInstanceSlots)
{
    setRate(low, 44100);
    AssetConverter converter;
    const auto first = converter.convert(source.sourceID, low);
    const auto second = converter.convert(source.sourceID, high);
    ASSERT_TRUE(first.success);
    ASSERT_TRUE(second.success);
    EXPECT_NE(first.cachePath, second.cachePath);
    EXPECT_EQ(bytes(first.absolutePath), bytes(second.absolutePath));
    EXPECT_EQ(AssetRegistry::instance().getAsset(source.sourceID)->platformCaches.size(), 2);
}

TEST_F(ProjectPlatformBuild, ChangedProfileDoesNotJoinOrPublishAnOlderJob)
{
    int complete = 0;
    AssetConverter converter;
    QObject::connect(&converter, &AssetConverter::conversionComplete, &converter,
        [&complete] { ++complete; });
    auto before = converter.convertAsync(source.sourceID, low);
    setRate(low, 32000);
    auto after = converter.convertAsync(source.sourceID, low);
    before.waitForFinished();
    after.waitForFinished();
    expectAudio(before.result(), 22050, 2);
    expectAudio(after.result(), 32000, 2);
    EXPECT_NE(before.result().cachePath, after.result().cachePath);
    QCoreApplication::processEvents();
    const auto* asset = AssetRegistry::instance().getAsset(source.sourceID);
    ASSERT_NE(asset, nullptr);
    EXPECT_EQ(asset->platformCaches.value(low).cachePath, after.result().cachePath);
    EXPECT_EQ(complete, 1);
}

TEST_F(ProjectPlatformBuild, BaseChangeSeparatesConversionsAndPreservesCapturedBuildIdentity)
{
    QString error;
    const auto snapshot = ProjectBuildSnapshot::capture(project(), &error);
    ASSERT_NE(snapshot, nullptr) << error.toStdString();
    const auto request = AssetRegistry::instance().conversionRequest(source.sourceID, low);
    ASSERT_TRUE(request);
    AssetConverter converter;
    auto before = converter.convertAsync(source.sourceID, low);
    ASSERT_TRUE(run("set_project_platform_base", {{"platform_id", Platforms::idString(low)},
        {"base_platform", "Windows"}}).success);
    EXPECT_FALSE(AssetRegistry::instance().accepts(*request));
    before.waitForFinished();
    expectAudio(before.result(), 22050, 2);
    const auto after = converter.convert(source.sourceID, low);
    expectAudio(after, 48000, 2);
    EXPECT_NE(before.result().cachePath, after.cachePath);
    QCoreApplication::processEvents();
    EXPECT_EQ(AssetRegistry::instance().getAsset(source.sourceID)->platformCaches.value(low).cachePath, after.cachePath);
    std::vector<ProjectPackExporter::PackBuildRecord> records;
    ASSERT_TRUE(exportPlatform(low, directory.filePath("CapturedBase"), records, snapshot).success);
    const auto entries = mediaEntries(contentPath(records));
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries.front().sampleRate, 22050u);
    auto& store = PackGenerationRecordStore::instance();
    store.clear();
    for (const auto& built : records) {
        EXPECT_EQ(built.basePlatform, BasePlatform::Android);
        EXPECT_EQ(built.platformName, QStringLiteral("Android_Low"));
        PackGenerationRecord record;
        record.platform = built.platform;
        record.platformName = built.platformName;
        record.basePlatform = built.basePlatform;
        record.platformDefinition = built.platformDefinition;
        record.outputPath = built.outputPath;
        record.status = "Built";
        store.add(record);
    }
    for (const auto& record : store.records()) {
        EXPECT_EQ(record.status, QStringLiteral("Built"));
        EXPECT_EQ(record.basePlatform, BasePlatform::Android);
    }
    EXPECT_EQ(AssetRegistry::instance().getAsset(source.sourceID)->platformCaches.value(low).cachePath, after.cachePath);
    store.clear();
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
    expectAudio(converter.convert(source.sourceID, low), 22050, 2);
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
    QCoreApplication::processEvents();
    EXPECT_FALSE(AssetRegistry::instance().getAsset(source.sourceID)->platformCaches.contains(low));
    EXPECT_TRUE(AssetRegistry::instance().getAsset(source.sourceID)->platformCaches.contains(high));
}

TEST_F(ProjectPlatformBuild, SameSizeSourceChangesInvalidateContentIdentity)
{
    AssetConverter converter;
    const auto previous = converter.convert(source.sourceID, low);
    ASSERT_TRUE(previous.success);
    const auto originalSize = QFileInfo(source.absolutePath).size();
    QFile file(source.absolutePath);
    ASSERT_TRUE(file.open(QIODevice::ReadWrite));
    ASSERT_TRUE(file.seek(100));
    ASSERT_EQ(file.write(QByteArray(4, '\0')), 4);
    file.close();
    EXPECT_EQ(QFileInfo(source.absolutePath).size(), originalSize);
    EXPECT_FALSE(AssetRegistry::instance().isCacheValid(source.sourceID, low));
    EXPECT_FALSE(converter.convert(source.sourceID, low).success);
    AssetRegistry::instance().updateSourceFile(source.sourceID, source.metadata, source.absolutePath,
        AssetImporter::computeContentHash(source.absolutePath), 0);
    const auto current = converter.convert(source.sourceID, low);
    ASSERT_TRUE(current.success);
    EXPECT_NE(previous.cachePath, current.cachePath);
}

TEST_F(ProjectPlatformBuild, RealPacksContainTheSelectedInstancePcm)
{
    std::vector<ProjectPackExporter::PackBuildRecord> lowRecords, highRecords;
    const auto lowResult = exportPlatform(low, directory.filePath("Android_Low"), lowRecords);
    ASSERT_TRUE(lowResult.success) << lowResult.error.toStdString();
    const auto highResult = exportPlatform(high, directory.filePath("Android_High"), highRecords);
    ASSERT_TRUE(highResult.success) << highResult.error.toStdString();
    const auto lowMedia = mediaEntries(contentPath(lowRecords));
    const auto highMedia = mediaEntries(contentPath(highRecords));
    ASSERT_EQ(lowMedia.size(), 1);
    ASSERT_EQ(highMedia.size(), 1);
    EXPECT_EQ(lowMedia.front().sampleRate, 22050u);
    EXPECT_EQ(lowMedia.front().channelCount, 2u);
    EXPECT_EQ(highMedia.front().sampleRate, 44100u);
    EXPECT_EQ(highMedia.front().channelCount, 2u);
    EXPECT_EQ(harkentest::ToStudio(lowMedia.front().soundId), sound->runtimeId());
    EXPECT_EQ(harkentest::ToStudio(highMedia.front().soundId), sound->runtimeId());
    EXPECT_NE(bytes(contentPath(lowRecords)), bytes(contentPath(highRecords)));
    EXPECT_GT(lowResult.mediaConversions, 0);
    EXPECT_GT(highResult.mediaConversions, 0);
    std::vector<ProjectPackExporter::PackBuildRecord> repeat;
    const auto reused = exportPlatform(low, directory.filePath("Repeat"), repeat);
    ASSERT_TRUE(reused.success) << reused.error.toStdString();
    EXPECT_EQ(reused.mediaConversions, 0);
    EXPECT_GT(reused.mediaCacheHits, 0);
    EXPECT_EQ(bytes(contentPath(lowRecords)), bytes(contentPath(repeat)));
}

TEST_F(ProjectPlatformBuild, StrictInitPairingSurvivesRepeatExport)
{
    ASSERT_TRUE(manager().saveProject());
    std::vector<ProjectPackExporter::PackBuildRecord> first, second;
    const auto exported = exportPlatform(low, directory.filePath("First"), first);
    ASSERT_TRUE(exported.success) << exported.error.toStdString();
    const auto repeated = exportPlatform(low, directory.filePath("Second"), second);
    ASSERT_TRUE(repeated.success) << repeated.error.toStdString();
    ASSERT_EQ(first.size(), second.size());
    const auto initRecord = std::find_if(first.begin(), first.end(), [](const auto& record) {
        return record.packName == QStringLiteral("Init");
    });
    ASSERT_NE(initRecord, first.end());
    const auto initBytes = bytes(initRecord->outputPath);
    harken::PackCanonicalImage init;
    ASSERT_EQ(VN_OK, harken::ParsePackCanonicalImage(
        reinterpret_cast<const uint8_t*>(initBytes.constData()), initBytes.size(), &init));
    ASSERT_EQ(harken::kPackKindInit, init.packKind);
    std::size_t contents = 0;
    for (const auto& record : first) {
        const auto matching = std::find_if(second.begin(), second.end(), [&](const auto& other) {
            return other.packName == record.packName;
        });
        ASSERT_NE(matching, second.end());
        const auto data = bytes(record.outputPath);
        ASSERT_FALSE(data.isEmpty());
        EXPECT_EQ(data, bytes(matching->outputPath));
        harken::PackCanonicalImage decoded;
        ASSERT_EQ(VN_OK, harken::ParsePackCanonicalImage(
            reinterpret_cast<const uint8_t*>(data.constData()), data.size(), &decoded));
        if (decoded.packKind != harken::kPackKindCanonicalContent) continue;
        ++contents;
        EXPECT_EQ(VN_OK, harken::ValidateCanonicalInitDependency(decoded.dependencyEdges, init.packStableId));
        for (const auto& edge : decoded.dependencyEdges) {
            if (edge.edgeKind == harken::kPackDependencyEdgeKindInit)
                EXPECT_EQ(init.requirement.packContentRevision, edge.expectedPrerequisiteContentRevision);
        }
    }
    EXPECT_GT(contents, 0u);
}

TEST_F(ProjectPlatformBuild, CapturedBuildSurvivesRenameProfileAndSourceSelectionChanges)
{
    QString error;
    const auto snapshot = ProjectBuildSnapshot::capture(project(), &error);
    ASSERT_NE(snapshot, nullptr) << error.toStdString();
    const auto oldActionId = action->runtimeId();
    setRate(low, 32000);
    ASSERT_TRUE(run("rename_project_platform", {{"platform_id", Platforms::idString(low)}, {"name", "Mobile"}}).success);
    sound->comp<VSound>()->sources.plain.active = QUuid{};
    sound->comp<VSound>()->sources.plain.platforms.clear();
    action->comp<VAction>()->steps.clear();
    std::vector<ProjectPackExporter::PackBuildRecord> records;
    const auto result = exportPlatform(low, directory.filePath("Captured"), records, snapshot);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    const auto media = mediaEntries(contentPath(records));
    ASSERT_EQ(media.size(), 1);
    EXPECT_EQ(media.front().sampleRate, 22050u);
    const auto plan = PackBuildPlanning::build(snapshot->project, {Platforms::idString(low)});
    ASSERT_FALSE(plan.variants.empty());
    EXPECT_EQ(plan.variants.front().actionStableId, oldActionId);
    EXPECT_EQ(plan.variants.front().platformDefinition.name, QStringLiteral("Android_Low"));
}

TEST_F(ProjectPlatformBuild, SelectionAndDependencyFiltersDistinguishSiblingInstances)
{
    PackExportSelection selection;
    selection.actionTargeting = {{action->runtimeId(), false, {Platforms::idString(low)}}};
    const auto plan = PackBuildPlanning::build(project(), {}, {}, false, selection);
    ASSERT_TRUE(plan.ready());
    ASSERT_EQ(plan.variants.size(), 1u);
    EXPECT_EQ(plan.variants.front().variant.platform, Platforms::idString(low));
    auto* dependent = manager().createItem(nullptr, "PostTone", ProjectItem::Type::Action);
    VActionStep post;
    post.type = VActionStep::Type::PostAction;
    post.targetObjectID = action->objectID;
    dependent->comp<VAction>()->steps.push_back(post);
    const auto conflict = PackBuildPlanning::build(project(), {Platforms::idString(high)}, {}, false, selection);
    EXPECT_FALSE(conflict.ready());
    EXPECT_TRUE(std::any_of(conflict.diagnostics.begin(), conflict.diagnostics.end(), [](const auto& issue) {
        return issue.code == ActionClosureDiagnosticCode::ExcludedDependency;
    }));
}

TEST_F(ProjectPlatformBuild, BundleUsesPackedMediaRevisionAndStableInstanceIdentity)
{
    const auto first = RuntimeContentBundleBuilder::build(project(), directory.filePath("Bundle1"), Platforms::idString(low));
    ASSERT_TRUE(first.success) << first.error.toStdString();
    ASSERT_GT(first.worldSemanticFingerprint, 0u);
    const auto second = RuntimeContentBundleBuilder::build(project(), directory.filePath("Bundle2"), Platforms::idString(high));
    ASSERT_TRUE(second.success) << second.error.toStdString();
    EXPECT_NE(first.bundleId, second.bundleId);
    EXPECT_NE(first.worldSemanticFingerprint, second.worldSemanticFingerprint);
    ASSERT_EQ(first.actions.size(), 1u);
    EXPECT_EQ(first.actions.front().stableId, action->runtimeId());
    ASSERT_TRUE(run("rename_project_platform", {{"platform_id", Platforms::idString(low)}, {"name", "Renamed"}}).success);
    const auto renamed = RuntimeContentBundleBuilder::build(project(), directory.filePath("Bundle3"), Platforms::idString(low));
    ASSERT_TRUE(renamed.success) << renamed.error.toStdString();
    EXPECT_EQ(first.bundleId, renamed.bundleId);
    const auto rebuilt = RuntimeContentBundleBuilder::build(project(), directory.filePath("Bundle1"), Platforms::idString(low));
    ASSERT_TRUE(rebuilt.success) << rebuilt.error.toStdString();
    EXPECT_EQ(first.bundleId, rebuilt.bundleId);
    EXPECT_EQ(rebuilt.platformName, QStringLiteral("Renamed"));
    QString error;
    const QString deployed = directory.filePath("Deployed");
    ASSERT_TRUE(RuntimeContentBundleBuilder::deploy(first, deployed, &error)) << error.toStdString();
    ASSERT_TRUE(RuntimeContentBundleBuilder::deploy(renamed, deployed, &error)) << error.toStdString();
    const QString manifestPath = QDir(deployed).filePath(QStringLiteral("Bundles/%1/%2")
        .arg(first.bundleId, HarkenEditor::Bridge::packManifestFileName()));
    const auto assertPair = [](const QString& root) {
        const auto manifest = bytes(QDir(root).filePath(HarkenEditor::Bridge::packManifestFileName()));
        const auto waveform = QJsonDocument::fromJson(bytes(QDir(root).filePath(QStringLiteral("HarkenEditorWaveforms.json")))).object();
        ASSERT_FALSE(waveform.isEmpty());
        EXPECT_EQ(waveform.value(QStringLiteral("manifestByteLength")).toInteger(), manifest.size());
        EXPECT_EQ(waveform.value(QStringLiteral("manifestFnv1a64")).toString().toULongLong(),
            harken::Fnv1a64(manifest.constData(), manifest.size()));
    };
    for (const auto& root : {first.bundleDirectory, renamed.bundleDirectory, rebuilt.bundleDirectory,
                            QFileInfo(manifestPath).absolutePath()})
        ASSERT_NO_FATAL_FAILURE(assertPair(root));
    const QString companionPath = QDir(QFileInfo(manifestPath).absolutePath()).filePath(QStringLiteral("HarkenEditorWaveforms.json"));
    const QByteArray originalCompanion = bytes(companionPath);
    auto companion = QJsonDocument::fromJson(originalCompanion).object();
    companion.insert(QStringLiteral("manifestFnv1a64"), QStringLiteral("0"));
    {
        QFile file(companionPath);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const QByteArray broken = QJsonDocument(companion).toJson();
        ASSERT_EQ(file.write(broken), broken.size());
    }
    EXPECT_FALSE(RuntimeContentBundleBuilder::deploy(renamed, deployed, &error));
    {
        QFile file(companionPath);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(file.write(originalCompanion), originalCompanion.size());
    }
    ASSERT_TRUE(RuntimeContentBundleBuilder::deploy(renamed, deployed, &error)) << error.toStdString();
    auto manifest = QJsonDocument::fromJson(bytes(manifestPath)).object();
    ASSERT_FALSE(manifest.isEmpty());
    manifest.insert(QStringLiteral("language"), QStringLiteral("tampered"));
    QFile file(manifestPath);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    const QByteArray changed = QJsonDocument(manifest).toJson();
    ASSERT_EQ(file.write(changed), changed.size());
    file.close();
    EXPECT_FALSE(RuntimeContentBundleBuilder::deploy(renamed, deployed, &error));

    const QString sourceManifestPath = QDir(renamed.bundleDirectory)
        .filePath(HarkenEditor::Bridge::packManifestFileName());
    manifest = QJsonDocument::fromJson(bytes(sourceManifestPath)).object();
    auto variables = manifest.value(QStringLiteral("variableCatalog")).toObject();
    variables.insert(QStringLiteral("sdkCatalogFingerprint"), QStringLiteral("EXACT_U64"));
    manifest.insert(QStringLiteral("variableCatalog"), variables);
    const QByteArray templateBytes = QJsonDocument(manifest).toJson();
    for (const QString& path : {sourceManifestPath, manifestPath}) {
        QFile output(path);
        ASSERT_TRUE(output.open(QIODevice::WriteOnly | QIODevice::Truncate));
        QByteArray exact = templateBytes;
        exact.replace("\"EXACT_U64\"", path == sourceManifestPath
            ? "18446744073709551614" : "18446744073709551615");
        ASSERT_EQ(output.write(exact), exact.size());
        output.close();
        const QString companionPath = QDir(QFileInfo(path).absolutePath()).filePath(QStringLiteral("HarkenEditorWaveforms.json"));
        auto companion = QJsonDocument::fromJson(bytes(companionPath)).object();
        companion.insert(QStringLiteral("manifestByteLength"), exact.size());
        companion.insert(QStringLiteral("manifestFnv1a64"), QString::number(harken::Fnv1a64(exact.constData(), exact.size())));
        QFile companionFile(companionPath);
        ASSERT_TRUE(companionFile.open(QIODevice::WriteOnly | QIODevice::Truncate));
        const auto paired = QJsonDocument(companion).toJson();
        ASSERT_EQ(companionFile.write(paired), paired.size());
    }
    EXPECT_FALSE(RuntimeContentBundleBuilder::deploy(renamed, deployed, &error));
}

TEST_F(ProjectPlatformBuild, BundleDeployRequiresTheCapturedPublicationSet)
{
    const auto candidate = RuntimeContentBundleBuilder::build(project(), directory.filePath("Candidate"), Platforms::idString(low));
    ASSERT_TRUE(candidate.success) << candidate.error.toStdString();
    const auto previous = RuntimeContentBundleBuilder::build(project(), directory.filePath("Previous"), Platforms::idString(high));
    ASSERT_TRUE(previous.success) << previous.error.toStdString();
    ASSERT_NE(candidate.bundleId, previous.bundleId);
    const QString activeRoot = directory.filePath("ActiveTarget");
    const QString emptyRoot = directory.filePath("EmptyTarget");
    QString error;
    ASSERT_TRUE(RuntimeContentBundleBuilder::deploy(previous, activeRoot, &error)) << error.toStdString();
    const QString activePath = QDir(activeRoot).filePath(QStringLiteral("HarkenContent.json"));
    const QByteArray previousActive = bytes(activePath);
    ASSERT_FALSE(previousActive.isEmpty());
    const QString sourceCompanion = QDir(candidate.bundleDirectory).filePath(QStringLiteral("HarkenEditorWaveforms.json"));
    const QByteArray companionBytes = bytes(sourceCompanion);
    ASSERT_FALSE(companionBytes.isEmpty());
    ASSERT_TRUE(QFile::remove(sourceCompanion));
    EXPECT_FALSE(RuntimeContentBundleBuilder::deploy(candidate, emptyRoot, &error));
    EXPECT_FALSE(QFileInfo::exists(QDir(emptyRoot).filePath(QStringLiteral("HarkenContent.json"))));
    EXPECT_FALSE(RuntimeContentBundleBuilder::deploy(candidate, activeRoot, &error));
    EXPECT_EQ(bytes(activePath), previousActive);
    {
        QFile file(sourceCompanion);
        ASSERT_TRUE(file.open(QIODevice::WriteOnly));
        ASSERT_EQ(file.write(companionBytes), companionBytes.size());
    }
    ASSERT_TRUE(QFile::remove(QDir(candidate.bundleDirectory).filePath(QStringLiteral("CurrentWorld.vnworld"))));
    ASSERT_TRUE(RuntimeContentBundleBuilder::deploy(candidate, emptyRoot, &error)) << error.toStdString();
    const QString newActivePath = QDir(emptyRoot).filePath(QStringLiteral("HarkenContent.json"));
    const QByteArray newActive = bytes(newActivePath);
    const QString targetCompanion = QDir(emptyRoot).filePath(QStringLiteral("Bundles/%1/HarkenEditorWaveforms.json").arg(candidate.bundleId));
    ASSERT_TRUE(QFile::remove(targetCompanion));
    EXPECT_FALSE(RuntimeContentBundleBuilder::deploy(candidate, emptyRoot, &error));
    EXPECT_EQ(bytes(newActivePath), newActive);
}

TEST_F(ProjectPlatformBuild, SoundCropProcessingAndScheduleOffsetReachThePack)
{
    std::vector<float> samples(4800 * 2, 0.0f);
    std::fill(samples.begin(), samples.begin() + 1200 * 2, 0.9f);
    for (size_t i = 1200; i < 2400; ++i) {
        samples[i * 2] = 0.2f;
        samples[i * 2 + 1] = 0.4f;
    }
    ASSERT_TRUE(AssetConverter::writeFloatWav(source.absolutePath, samples, 2, 48000));
    ASSERT_TRUE(AssetRegistry::instance().updateSourceFile(source.sourceID, source.metadata,
        source.absolutePath, AssetImporter::computeContentHash(source.absolutePath), 0));
    ProjectPlatformProfile profile;
    profile.performResample = true;
    profile.sizePolicies.front().config.targetSampleRate = 24000;
    ASSERT_TRUE(run("set_project_platform_profile", {{"platform_id", Platforms::idString(low)},
        {"profile", profile.toJson()}}).success);
    auto* component = sound->comp<VSound>();
    auto& selection = component->sources.plain.entries.front();
    selection.trimStartFrame = 1200;
    selection.trimEndFrame = 3600;
    selection.loopProvenance = SoundLoopProvenance::Authored;
    selection.loopRegion = SoundLoopRegion{1400, 2200};
    component->scheduleOffsetSec = 0.01;
    std::vector<ProjectPackExporter::PackBuildRecord> records;
    const auto packed = exportPlatform(low, directory.filePath("Processed"), records);
    ASSERT_TRUE(packed.success) << packed.error.toStdString();
    const auto entries = mediaEntries(contentPath(records));
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries.front().sampleRate, 24000u);
    EXPECT_EQ(entries.front().channelCount, 2u);
    EXPECT_EQ(entries.front().frameCount, 1440u);
    EXPECT_EQ(entries.front().loopStartFrame, 340u);
    EXPECT_EQ(entries.front().loopEndFrame, 740u);
    QString error;
    const auto artifact = SoundSourceDerivative::materialize(source.absolutePath, selection,
        component->scheduleOffsetSec, low, directory.filePath("Derivative"), ConversionSettings(profile.sizePolicies.front().config, profile.performResample), &error);
    ASSERT_TRUE(artifact) << error.toStdString();
    std::vector<float> decoded;
    int channels = 0, rate = 0;
    ASSERT_TRUE(HarkenEditor::Bridge::decodeAudioFileInterleaved(artifact->path, decoded, channels, rate));
    ASSERT_EQ(decoded.size(), 2880u);
    EXPECT_EQ(channels, 2);
    EXPECT_EQ(rate, 24000);
    EXPECT_TRUE(std::all_of(decoded.begin(), decoded.begin() + 480, [](float sample) { return sample == 0; }));
    EXPECT_NEAR(decoded[1000], 0.2f, 0.001f);
    EXPECT_NEAR(decoded[1001], 0.4f, 0.001f);
    EXPECT_FLOAT_EQ(decoded.back(), 0.0f);
}

TEST_F(ProjectPlatformBuild, SpatialPropertiesPreserveStereoAndReuseMediaCache)
{
    const auto settings = PlatformProfileManager::effectiveSettingsForAsset(
        source, *Platforms::find(project().platforms, low));
    const auto selection = sound->comp<VSound>()->sources.plain.entries.front();
    QString error;
    const auto stereo = SoundSourceDerivative::materialize(source.absolutePath, selection,
        0.0, low, directory.filePath("SpatialDerivative"), settings, &error);
    ASSERT_TRUE(stereo) << error.toStdString();
    EXPECT_EQ(stereo->channels, 2);
    std::vector<float> samples;
    int channels = 0, rate = 0;
    ASSERT_TRUE(HarkenEditor::Bridge::decodeAudioFileInterleaved(
        stereo->path, samples, channels, rate));
    EXPECT_EQ(channels, 2);
    EXPECT_EQ(rate, 22050);
    std::vector<ProjectPackExporter::PackBuildRecord> records;
    ASSERT_TRUE(exportPlatform(low, directory.filePath("StereoPack"), records).success);
    const auto twoDimensional = mediaEntries(contentPath(records));
    ASSERT_EQ(twoDimensional.size(), 1);
    EXPECT_EQ(twoDimensional.front().channelCount, 2u);

    auto* spatial = sound->comp<VSpatial>();
    ASSERT_NE(spatial, nullptr);
    sound->fieldGroups.spatialRating.shared = {
        FieldGroupMode::Local, SpatialRatingFieldGroup{0.0f, true}};
    for (float mix : {0.0f, 0.5f, 1.0f}) {
        SCOPED_TRACE(mix);
        spatial->spatializationMix = mix;
        spatial->spatializationMixPresent = true;
        records.clear();
        const auto result = exportPlatform(
            low, directory.filePath(QStringLiteral("SpatialPack-%1").arg(mix)), records);
        ASSERT_TRUE(result.success) << result.error.toStdString();
        EXPECT_EQ(result.mediaConversions, 0);
        EXPECT_GT(result.mediaCacheHits, 0);
        const auto threeDimensional = mediaEntries(contentPath(records));
        ASSERT_EQ(threeDimensional.size(), 1);
        EXPECT_EQ(threeDimensional.front().channelCount, 2u);
        EXPECT_EQ(threeDimensional.front().blobChecksum, twoDimensional.front().blobChecksum);
        EXPECT_EQ(threeDimensional.front().frameCount, twoDimensional.front().frameCount);
    }
    const auto cached = SoundSourceDerivative::materialize(source.absolutePath, selection,
        0.0, low, directory.filePath("SpatialDerivative"), settings, &error);
    ASSERT_TRUE(cached) << error.toStdString();
    EXPECT_TRUE(cached->cacheHit);
    EXPECT_EQ(cached->cacheKey, stereo->cacheKey);
    AssetConverter converter;
    expectAudio(converter.convert(source.sourceID, low), 22050, 2);
}

TEST_F(ProjectPlatformBuild, VeryShortCropsKeepTheirResampledFrames)
{
    SoundSourceDef selection = sound->comp<VSound>()->sources.plain.entries.front();
    selection.trimStartFrame = 10;
    selection.trimEndFrame = 90;
    ConversionSettings settings;
    settings.performResample = true;
    settings.targetSampleRate = 24000;
    QString error;
    const auto artifact = SoundSourceDerivative::materialize(source.absolutePath, selection,
        0.0, low, directory.filePath("Short"), settings, &error);
    ASSERT_TRUE(artifact) << error.toStdString();
    AssetImporter importer;
    EXPECT_EQ(importer.analyzeFile(artifact->path).totalSamples, 40u);
}

TEST_F(ProjectPlatformBuild, MediaStatusAndCleanupUseTheRequestedInstance)
{
    AssetConverter converter;
    const auto converted = converter.convert(source.sourceID, low);
    ASSERT_TRUE(converted.success);
    auto& pool = MediaPool::instance();
    EXPECT_EQ(pool.getStats(low).convertedCount, 1u);
    EXPECT_EQ(pool.getStats(high).convertedCount, 0u);
    EXPECT_EQ(pool.computeStatus(source.sourceID, low), MediaStatus::Converted);
    std::vector<ProjectPackExporter::PackBuildRecord> records;
    ASSERT_TRUE(exportPlatform(low, directory.filePath("CachedPack"), records).success);
    AssetGarbageCollector gc;
    gc.setProjectRoot(AssetRegistry::instance().projectRoot());
    EXPECT_EQ(gc.getCacheStats().fileCount, 2);
    const auto legacyPath = QDir(PathUtils::getCacheDir(AssetRegistry::instance().projectRoot()))
        .filePath("Android/101_123.wav");
    ASSERT_TRUE(AssetConverter::writeFloatWav(legacyPath, {0.1f}, 1, 48000));
    EXPECT_TRUE(gc.scanForStaleCaches().contains(legacyPath));
    {
        const auto activity = ProjectPlatformActivity::acquire();
        EXPECT_FALSE(gc.cleanupLRU(90, 1).errors.isEmpty());
        EXPECT_TRUE(QFileInfo::exists(converted.absolutePath));
    }
    const auto cleaned = gc.cleanupLRU(90, 1);
    EXPECT_TRUE(cleaned.errors.isEmpty());
    EXPECT_EQ(cleaned.deletedCount, 3);
    EXPECT_FALSE(AssetRegistry::instance().getAsset(source.sourceID)->platformCaches.value(low).valid);
}

TEST_F(ProjectPlatformBuild, ExplicitExportsIgnorePrimaryAndKeepSnapshotRecordNames)
{
    auto& context = PlatformContextService::instance();
    context.setSelected({low}, low, false);
    std::vector<ProjectPackExporter::PackBuildRecord> first;
    ASSERT_TRUE(exportPlatform(high, directory.filePath("PrimaryLow"), first).success);
    const auto originalBytes = bytes(contentPath(first));
    context.setSelected({high}, high, false);
    std::vector<ProjectPackExporter::PackBuildRecord> second;
    ASSERT_TRUE(exportPlatform(high, directory.filePath("PrimaryHigh"), second).success);
    EXPECT_EQ(bytes(contentPath(second)), originalBytes);
    auto& store = PackGenerationRecordStore::instance();
    store.clear();
    for (const auto& built : first) {
        EXPECT_EQ(built.platformName, QStringLiteral("Android_High"));
        EXPECT_EQ(built.basePlatform, BasePlatform::Android);
        PackGenerationRecord record;
        record.platform = built.platform;
        record.platformName = built.platformName;
        record.platformDefinition = built.platformDefinition;
        record.outputPath = built.outputPath;
        record.status = "Built";
        store.add(record);
    }
    ASSERT_TRUE(run("rename_project_platform", {{"platform_id", Platforms::idString(high)}, {"name", "MobileHigh"}}).success);
    for (const auto& record : store.records()) {
        EXPECT_EQ(record.status, QStringLiteral("Built"));
        EXPECT_EQ(record.platformName, QStringLiteral("Android_High"));
        EXPECT_TRUE(record.outputPath.contains("PrimaryLow"));
    }
    store.clear();
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
    EXPECT_EQ(mediaEntries(record->outputPath).front().sampleRate, 22050u);
    QCoreApplication::processEvents();
    EXPECT_TRUE(AssetRegistry::instance().getAllSourceIDs().empty());
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
    ASSERT_EQ(copiedContainer->comp<VNX::VSelection>()->selections.shared.occurrenceRows().size(), 1u);
    EXPECT_EQ(copiedContainer->comp<VNX::VSelection>()->selections.toJson(), selectionsJson);
}

TEST_F(ProjectPlatformBuild, SiblingInstancesBuildLanguageSelectionsAndSharedFallback)
{
    const auto japanese = addAsset(102, 0.6f);
    const auto highJapanese = addAsset(103, 0.9f);
    auto* component = sound->comp<VSound>();
    component->distinguishLanguage = true;
    auto& english = component->sources.localized["English"];
    english.entries = {{QUuid::createUuid(), source.sourceID}};
    english.active = english.entries[0].entryId;
    auto& ja = component->sources.localized["ja"];
    ja.entries = {{QUuid::createUuid(), japanese.sourceID}, {QUuid::createUuid(), highJapanese.sourceID}};
    ja.active = ja.entries[0].entryId;
    ja.platforms[Platforms::idString(high)].active = ja.entries[1].entryId;
    project().projectLanguages = {"English", "ja"};
    project().defaultLanguage = "English";
    const auto plan = PackBuildPlanning::build(project(), {Platforms::idString(low), Platforms::idString(high)});
    ASSERT_TRUE(plan.ready());
    ASSERT_EQ(plan.variants.size(), 4u);
    for (const auto& variant : plan.variants) {
        ASSERT_EQ(variant.closure.media.size(), 1u);
        const auto expected = variant.variant.language == "English" ? source.sourceID
            : variant.variant.platform == Platforms::idString(high) ? highJapanese.sourceID : japanese.sourceID;
        EXPECT_EQ(variant.closure.media.front().authoringSourceId, expected);
    }
    for (const auto id : {low, high}) {
        std::vector<ProjectPackExporter::PackBuildRecord> records;
        const auto result = exportPlatform(id, directory.filePath(Platforms::idString(id)), records);
        ASSERT_TRUE(result.success) << result.error.toStdString();
        int variants = 0;
        QByteArray firstLanguage;
        for (const auto& record : records) {
            if (record.objectID != action->objectID) continue;
            ++variants;
            EXPECT_TRUE(record.outputPath.contains('/' + record.language + '/'));
            const auto entries = mediaEntries(record.outputPath);
            ASSERT_EQ(entries.size(), 1);
            EXPECT_EQ(entries.front().sampleRate, id == low ? 22050u : 44100u);
            EXPECT_EQ(entries.front().frameCount, id == low ? 2205u : 4410u);
            EXPECT_EQ(harkentest::ToStudio(entries.front().soundId), sound->runtimeId());
            const auto content = bytes(record.outputPath);
            if (firstLanguage.isEmpty()) firstLanguage = content;
            else EXPECT_NE(firstLanguage, content);
        }
        EXPECT_EQ(variants, 2);
    }
}

TEST_F(ProjectPlatformBuild, IpcBuildPublishesSnapshotRecordsAfterPlatformChanges)
{
    IPCServer server(nullptr);
    server.setProjectManager(&manager());
    server.setAppState(IPCServer::AppState::Ready);
    auto& store = PackGenerationRecordStore::instance();
    store.clear();
    const auto response = server.dispatchCommand("export_pack", {
        {"platform_id", Platforms::idString(low)}, {"output_path", directory.filePath("IpcBuild")}});
    ASSERT_EQ(response.value("status").toString(), QStringLiteral("ok"))
        << QJsonDocument(response).toJson().constData();
    ASSERT_TRUE(response.value("result").toObject().value("async").toBool());
    setRate(low, 32000);
    ASSERT_TRUE(run("rename_project_platform", {{"platform_id", Platforms::idString(low)}, {"name", "Changed"}}).success);
    QElapsedTimer timer;
    timer.start();
    while (server.dispatchCommand("get_pack_build_status", {}).value("result").toObject()
            .value("in_progress").toBool() && timer.elapsed() < 30000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    ASSERT_LT(timer.elapsed(), 30000);
    const auto completion = server.dispatchCommand("get_pack_build_status", {}).value("result").toObject();
    EXPECT_EQ(completion.value("result").toObject().value("status").toString(), QStringLiteral("error"));
    EXPECT_TRUE(completion.value("message").toString().contains(QStringLiteral("platform changed")));
    const auto index = QJsonDocument::fromJson(bytes(directory.filePath("IpcBuild/HarkenPackIndex.json"))).object();
    EXPECT_TRUE(index.value("platforms").toArray().isEmpty());
    const auto records = store.records();
    ASSERT_FALSE(records.empty());
    for (const auto& record : records) {
        EXPECT_TRUE(record.success) << record.errors.toStdString();
        EXPECT_EQ(record.platformName, QStringLiteral("Android_Low"));
        EXPECT_EQ(record.status, QStringLiteral("Built"));
        EXPECT_TRUE(record.outputPath.contains("Android_Low"));
        if (record.objectID == action->objectID)
            EXPECT_EQ(mediaEntries(record.outputPath).front().sampleRate, 22050u);
    }
    store.clear();
}


QJsonObject deliveryJson(const QString& path)
{
    return QJsonDocument::fromJson(bytes(path)).object();
}

void writeDeliveryJson(const QString& path, const QJsonObject& value)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly | QIODevice::Truncate));
    file.write(QJsonDocument(value).toJson());
}

TEST_F(ProjectPlatformBuild, DeliveryIndexAccumulatesOnlyPublishedInstances)
{
    const QString root = directory.filePath("Delivery");
    QString error;
    for (const auto id : {low, high}) {
        const auto& definition = *Platforms::find(project().platforms, id);
        std::vector<ProjectPackExporter::PackBuildRecord> records;
        const auto built = exportPlatform(id, QDir(root).filePath(definition.name), records);
        ASSERT_TRUE(built.success) << built.error.toStdString();
        const auto manifest = deliveryJson(QDir(built.outputPath).filePath("HarkenPackManifest.json"));
        EXPECT_EQ(manifest.value("platformId").toString(), Platforms::idString(id));
        EXPECT_EQ(manifest.value("platformName").toString(), definition.name);
        EXPECT_EQ(manifest.value("basePlatform").toString(), QStringLiteral("Android"));
        EXPECT_FALSE(manifest.contains("platform"));
        for (const auto& value : manifest.value("packs").toArray()) {
            EXPECT_EQ(value.toObject().value("platformId").toString(), Platforms::idString(id));
            EXPECT_FALSE(value.toObject().contains("platform"));
        }
        ASSERT_TRUE(ProjectPackExporter::PublishPackIndex(root, project().platforms,
            {Platforms::idString(id)}, {Platforms::idString(id)}, &error)) << error.toStdString();
    }
    const auto entries = deliveryJson(QDir(root).filePath("HarkenPackIndex.json")).value("platforms").toArray();
    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(entries[0].toObject().value("id").toString(), Platforms::idString(low));
    EXPECT_EQ(entries[1].toObject().value("id").toString(), Platforms::idString(high));
    EXPECT_EQ(entries[1].toObject().value("path").toString(), QStringLiteral("Android_High"));
    preservePlatformUnityFixture(root);
}

TEST_F(ProjectPlatformBuild, FailedDeliveryRetainsFilesWithoutResurrectingIndexEntries)
{
    const QString root = directory.filePath("Delivery");
    QString error;
    for (const auto id : {low, high}) {
        std::vector<ProjectPackExporter::PackBuildRecord> records;
        ASSERT_TRUE(exportPlatform(id, QDir(root).filePath(Platforms::find(project().platforms, id)->name), records).success);
    }
    const auto ids = QStringList{Platforms::idString(low), Platforms::idString(high)};
    ASSERT_TRUE(ProjectPackExporter::PublishPackIndex(root, project().platforms, ids, ids, &error));
    const auto before = bytes(QDir(root).filePath("Android_Low/Init.bytes"));
    ASSERT_TRUE(ProjectPackExporter::PublishPackIndex(root, project().platforms, {ids.front()}, {}, &error));
    EXPECT_EQ(bytes(QDir(root).filePath("Android_Low/Init.bytes")), before);
    ASSERT_TRUE(ProjectPackExporter::PublishPackIndex(root, project().platforms, {ids.back()}, {ids.back()}, &error));
    auto entries = deliveryJson(QDir(root).filePath("HarkenPackIndex.json")).value("platforms").toArray();
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries.first().toObject().value("id").toString(), ids.back());
    ASSERT_TRUE(ProjectPackExporter::PublishPackIndex(root, project().platforms, ids, {}, &error));
    EXPECT_TRUE(deliveryJson(QDir(root).filePath("HarkenPackIndex.json")).value("platforms").toArray().isEmpty());
    EXPECT_EQ(bytes(QDir(root).filePath("Android_Low/Init.bytes")), before);
}

TEST_F(ProjectPlatformBuild, IncrementalPublicationPreservesPlatformsAndRejectsInvalidIndex)
{
    const QString root = directory.filePath("LegacyDelivery");
    const auto ids = QStringList{Platforms::idString(low), Platforms::idString(high)};
    for (const auto id : {low, high}) {
        std::vector<ProjectPackExporter::PackBuildRecord> records;
        ASSERT_TRUE(exportPlatform(id,
            QDir(root).filePath(Platforms::find(project().platforms, id)->name), records).success);
    }
    QString error;
    ASSERT_TRUE(ProjectPackExporter::PublishPackIndex(root, project().platforms, ids, ids, &error))
        << error.toStdString();
    const QDir output(root);
    const QString current = output.filePath("HarkenPackIndex.json");
    ASSERT_TRUE(ProjectPackExporter::PublishPackIndex(root, project().platforms,
        {ids.front()}, {ids.front()}, &error)) << error.toStdString();
    const auto entries = deliveryJson(current).value("platforms").toArray();
    ASSERT_EQ(entries.size(), 2);
    EXPECT_EQ(entries[1].toObject().value("id").toString(), ids.back());
    writeDeliveryJson(current, {});
    EXPECT_FALSE(ProjectPackExporter::PublishPackIndex(root, project().platforms,
        {ids.front()}, {ids.front()}, &error));
    EXPECT_FALSE(error.isEmpty());
}

TEST_F(ProjectPlatformBuild, HistoricalIndexIsRejectedWithoutChangingFiles)
{
    const QString root = directory.filePath("HistoricalDelivery");
    const QString id = Platforms::idString(low);
    std::vector<ProjectPackExporter::PackBuildRecord> records;
    ASSERT_TRUE(exportPlatform(low, QDir(root).filePath("Android_Low"), records).success);
    QString error;
    ASSERT_TRUE(ProjectPackExporter::PublishPackIndex(root, project().platforms, {id}, {id}, &error))
        << error.toStdString();
    const QString indexPath = QDir(root).filePath("HarkenPackIndex.json");
    auto index = deliveryJson(indexPath);
    for (int version = 1; version < VNX::PackSchema::kPackManifestSchemaVersion; ++version) {
        index.insert("schema_version", version);
        writeDeliveryJson(indexPath, index);
        const auto before = bytes(indexPath);
        EXPECT_FALSE(ProjectPackExporter::PublishPackIndex(root, project().platforms, {id}, {id}, &error));
        EXPECT_FALSE(error.isEmpty());
        EXPECT_EQ(bytes(indexPath), before);
    }
}

TEST_F(ProjectPlatformBuild, UnreadableIndexIsPreservedWhenPublicationFails)
{
    const QString root = directory.filePath("UnreadableLegacyDelivery");
    ASSERT_TRUE(QDir().mkpath(root));
    ASSERT_TRUE(QDir(root).mkdir("HarkenPackIndex.json"));

    QString error;
    EXPECT_FALSE(ProjectPackExporter::PublishPackIndex(root, project().platforms, {}, {}, &error));
    EXPECT_FALSE(error.isEmpty());
    EXPECT_TRUE(QFileInfo(QDir(root).filePath("HarkenPackIndex.json")).isDir());
}

TEST_F(ProjectPlatformBuild, RenamedAndRecreatedPlatformsCannotReuseOldDirectoryIdentity)
{
    const QString root = directory.filePath("Delivery");
    const QString oldDir = QDir(root).filePath("Android_Low");
    std::vector<ProjectPackExporter::PackBuildRecord> records;
    ASSERT_TRUE(exportPlatform(low, oldDir, records).success);
    const auto originalInit = bytes(QDir(oldDir).filePath("Init.bytes"));
    const auto id = Platforms::idString(low);
    QString error;
    ASSERT_TRUE(ProjectPackExporter::PublishPackIndex(root, project().platforms, {id}, {id}, &error));
    ASSERT_TRUE(run("rename_project_platform", {{"platform_id", id}, {"name", "Mobile"}}).success);
    ASSERT_TRUE(ProjectPackExporter::PublishPackIndex(root, project().platforms, {}, {}, &error));
    EXPECT_TRUE(deliveryJson(QDir(root).filePath("HarkenPackIndex.json")).value("platforms").toArray().isEmpty());
    EXPECT_EQ(bytes(QDir(oldDir).filePath("Init.bytes")), originalInit);
    records.clear();
    ASSERT_TRUE(exportPlatform(low, QDir(root).filePath("Mobile"), records).success);
    EXPECT_EQ(bytes(QDir(root).filePath("Mobile/Init.bytes")), originalInit);
    ASSERT_TRUE(ProjectPackExporter::PublishPackIndex(root, project().platforms, {id}, {id}, &error));
    ASSERT_TRUE(run("delete_project_platform", {{"platform_id", id}}).success);
    const auto replacement = createPlatform("Android_Low");
    const auto replacementId = Platforms::idString(replacement);
    EXPECT_NE(replacement, low);
    EXPECT_FALSE(ProjectPackExporter::PublishPackIndex(root, project().platforms, {replacementId}, {replacementId}, &error));
    EXPECT_TRUE(deliveryJson(QDir(root).filePath("HarkenPackIndex.json")).value("platforms").toArray().isEmpty());
    EXPECT_EQ(bytes(QDir(oldDir).filePath("Init.bytes")), originalInit);
    records.clear();
    ASSERT_TRUE(exportPlatform(replacement, oldDir, records).success);
    EXPECT_NE(bytes(QDir(oldDir).filePath("Init.bytes")), originalInit);
    ASSERT_TRUE(ProjectPackExporter::PublishPackIndex(root, project().platforms, {replacementId}, {replacementId}, &error));
    const auto entry = deliveryJson(QDir(root).filePath("HarkenPackIndex.json")).value("platforms").toArray().first().toObject();
    EXPECT_EQ(entry.value("id").toString(), replacementId);
    EXPECT_EQ(entry.value("path").toString(), QStringLiteral("Android_Low"));
}

TEST_F(ProjectPlatformBuild, ChangedPlatformsCannotReportSuccessfulPublication)
{
    const QString root = directory.filePath("ChangedDelivery");
    const auto ids = QStringList{Platforms::idString(low), Platforms::idString(high)};
    for (const auto id : {low, high}) {
        std::vector<ProjectPackExporter::PackBuildRecord> records;
        ASSERT_TRUE(exportPlatform(id, QDir(root).filePath(Platforms::find(project().platforms, id)->name), records).success);
    }
    const auto original = bytes(QDir(root).filePath("Android_Low/Init.bytes"));
    for (const auto operation : {QStringLiteral("rename"), QStringLiteral("base"), QStringLiteral("delete"), QStringLiteral("recreate")}) {
        SCOPED_TRACE(operation.toStdString());
        QString error;
        ASSERT_TRUE(ProjectPackExporter::PublishPackIndex(root, project().platforms, ids, ids, &error));
        if (operation == QStringLiteral("rename")) {
            ASSERT_TRUE(run("rename_project_platform", {{"platform_id", ids.front()}, {"name", "Mobile"}}).success);
        } else if (operation == QStringLiteral("base")) {
            ASSERT_TRUE(run("set_project_platform_base", {{"platform_id", ids.front()}, {"base_platform", "iOS"}}).success);
        } else {
            ASSERT_TRUE(run("delete_project_platform", {{"platform_id", ids.front()}}).success);
            if (operation == QStringLiteral("recreate")) EXPECT_NE(createPlatform("Android_Low"), low);
        }
        EXPECT_FALSE(ProjectPackExporter::PublishPackIndex(root, project().platforms, ids, ids, &error));
        EXPECT_FALSE(error.isEmpty());
        const auto entries = deliveryJson(QDir(root).filePath("HarkenPackIndex.json")).value("platforms").toArray();
        ASSERT_EQ(entries.size(), 1);
        EXPECT_EQ(entries.first().toObject().value("id").toString(), ids.back());
        EXPECT_EQ(bytes(QDir(root).filePath("Android_Low/Init.bytes")), original);
        ASSERT_TRUE(UndoRedoManager::instance().undo());
        if (operation == QStringLiteral("recreate")) ASSERT_TRUE(UndoRedoManager::instance().undo());
    }
}

#ifdef Q_OS_WIN
TEST_F(ProjectPlatformBuild, TemporarySharingLockRetriesWithoutLosingPublishedIndex)
{
    const QString root = directory.filePath("RetryDelivery");
    const auto ids = QStringList{Platforms::idString(low), Platforms::idString(high)};
    for (const auto id : {low, high}) {
        std::vector<ProjectPackExporter::PackBuildRecord> records;
        ASSERT_TRUE(exportPlatform(id, QDir(root).filePath(Platforms::find(project().platforms, id)->name), records).success);
    }
    QString error;
    ASSERT_TRUE(ProjectPackExporter::PublishPackIndex(root, project().platforms, ids, ids, &error)) << error.toStdString();
    const QString path = QDir(root).filePath(HarkenEditor::Bridge::packIndexFileName());
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(QDir::toNativeSeparators(path).utf16()),
        GENERIC_READ, 0, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    std::jthread releaseLock([handle] {
        QThread::msleep(80);
        CloseHandle(handle);
    });
    ASSERT_TRUE(run("delete_project_platform", {{"platform_id", ids.front()}}).success);
    ASSERT_TRUE(ProjectPackExporter::PublishPackIndex(root, project().platforms, ids, {ids.back()}, &error))
        << error.toStdString();
    const auto entries = deliveryJson(path).value("platforms").toArray();
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries.first().toObject().value("id").toString(), ids.back());
}
#endif

TEST_F(ProjectPlatformBuild, IpcBuildRejectsPublicationAfterItsPlatformIsDeleted)
{
    IPCServer server(nullptr);
    server.setProjectManager(&manager());
    server.setAppState(IPCServer::AppState::Ready);
    const QString root = directory.filePath("DeletedIpcDelivery");
    const auto response = server.dispatchCommand("export_pack", {
        {"platform_id", Platforms::idString(low)}, {"output_path", root}});
    ASSERT_EQ(response.value("status").toString(), QStringLiteral("ok"));
    ASSERT_TRUE(run("delete_project_platform", {{"platform_id", Platforms::idString(low)}}).success);
    QElapsedTimer timer;
    timer.start();
    while (server.dispatchCommand("get_pack_build_status", {}).value("result").toObject()
            .value("in_progress").toBool() && timer.elapsed() < 30000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    ASSERT_LT(timer.elapsed(), 30000);
    const auto completion = server.dispatchCommand("get_pack_build_status", {}).value("result").toObject();
    EXPECT_EQ(completion.value("result").toObject().value("status").toString(), QStringLiteral("error"));
    EXPECT_TRUE(completion.value("message").toString().contains(QStringLiteral("deleted during the build")));
    EXPECT_TRUE(deliveryJson(QDir(root).filePath("HarkenPackIndex.json")).value("platforms").toArray().isEmpty());
    EXPECT_TRUE(QFileInfo::exists(QDir(root).filePath("Android_Low/Init.bytes")));
    const auto& records = PackGenerationRecordStore::instance().records();
    ASSERT_FALSE(records.empty());
    for (const auto& record : records) EXPECT_EQ(record.status, QStringLiteral("Built"));
    PackGenerationRecordStore::instance().clear();
}

TEST_F(ProjectPlatformBuild, DeliveryRejectsMismatchedIdentityCorruptionAndIncompleteSets)
{
    const QString root = directory.filePath("Delivery");
    const QString platformDir = QDir(root).filePath("Android_Low");
    const QString manifestPath = QDir(platformDir).filePath("HarkenPackManifest.json");
    const QString initPath = QDir(platformDir).filePath("Init.bytes");
    const QString id = Platforms::idString(low);
    std::vector<ProjectPackExporter::PackBuildRecord> records;
    ASSERT_TRUE(exportPlatform(low, platformDir, records).success);
    const auto manifest = deliveryJson(manifestPath);
    QString error;
    for (const auto& key : {"platformId", "platformName", "basePlatform"}) {
        auto wrong = manifest;
        wrong.insert(QLatin1String(key), key == QStringLiteral("platformId") ? Platforms::idString(high) : QStringLiteral("Windows"));
        writeDeliveryJson(manifestPath, wrong);
        EXPECT_FALSE(ProjectPackExporter::WritePackIndex(root, {*Platforms::find(project().platforms, low)}, &error));
    }
    auto wrong = manifest;
    auto rows = wrong.value("packs").toArray();
    auto row = rows.first().toObject();
    row.insert("platformId", Platforms::idString(high));
    rows[0] = row;
    wrong.insert("packs", rows);
    writeDeliveryJson(manifestPath, wrong);
    EXPECT_FALSE(ProjectPackExporter::PublishPackIndex(root, project().platforms, {id}, {id}, &error));
    writeDeliveryJson(manifestPath, manifest);
    ASSERT_TRUE(ProjectPackExporter::PublishPackIndex(root, project().platforms, {id}, {id}, &error));
    QFile file(initPath);
    ASSERT_TRUE(file.open(QIODevice::Append));
    file.write("changed");
    file.close();
    ASSERT_TRUE(ProjectPackExporter::PublishPackIndex(root, project().platforms, {}, {}, &error));
    EXPECT_TRUE(deliveryJson(QDir(root).filePath("HarkenPackIndex.json")).value("platforms").toArray().isEmpty());
}

TEST_F(ProjectPlatformBuild, ProjectRootOutputIsRejectedBeforeMutation)
{
    const QString root = QFileInfo(project().projectPath).absolutePath();
    const auto original = bytes(project().projectPath);
    const int undoCount = UndoRedoManager::instance().undoStack()->count();
    const auto current = project().packBuildOutputDir;
    IPCServer server(nullptr);
    server.setProjectManager(&manager());
    server.setAppState(IPCServer::AppState::Ready);
    for (const auto& path : {QStringLiteral("."), QStringLiteral("../") + QDir(root).dirName()}) {
        EXPECT_FALSE(run("set_pack_build_output_dir", {{"path", path}}).success);
        EXPECT_EQ(project().packBuildOutputDir, current);
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoCount);
        const auto response = server.dispatchCommand("export_pack", {{"output_path", path},
            {"platform_id", Platforms::idString(low)}});
        EXPECT_EQ(response.value("code").toString(), QStringLiteral("INVALID_ARGS"));
        project().packBuildOutputDir = path;
        ProjectPackExporter exporter;
        ProjectPackExporter::ExportOptions options;
        options.platform = Platforms::idString(low);
        const auto result = exporter.ExportAllPacks(project(), {}, options);
        EXPECT_FALSE(result.success);
        EXPECT_TRUE(result.error.contains(QStringLiteral("project root")));
        ProjectSerializer serializer;
        EXPECT_FALSE(serializer.saveProject(project().projectPath, project()));
        EXPECT_TRUE(serializer.lastError().contains(QStringLiteral("project root")));
        EXPECT_EQ(bytes(project().projectPath), original);
        project().packBuildOutputDir = current;
    }
    EXPECT_FALSE(QFile::exists(QDir(root).filePath("HarkenPackIndex.json")));
    EXPECT_FALSE(QFile::exists(QDir(root).filePath(".harken-pack-build.lock")));
    EXPECT_FALSE(QDir(QDir(root).filePath("Android_Low")).exists());
    auto document = QJsonDocument::fromJson(original).object();
    auto content = document.value("content").toObject();
    content.insert("packBuildOutputDir", ".");
    document.insert("content", content);
    writeDeliveryJson(project().projectPath, document);
    ProjectSerializer serializer;
    ProjectData loaded;
    EXPECT_FALSE(serializer.loadProject(project().projectPath, loaded, false));
    EXPECT_TRUE(serializer.lastError().contains(QStringLiteral("project root")));
    EXPECT_EQ(deliveryJson(project().projectPath), document);
}

TEST_F(ProjectPlatformBuild, ObjectBucketOutputIsRejectedBeforeMutation)
{
    ASSERT_TRUE(manager().saveProject());
    const QString root = QFileInfo(project().projectPath).absolutePath();
    const auto original = bytes(project().projectPath);
    const int undoCount = UndoRedoManager::instance().undoStack()->count();
    const auto current = project().packBuildOutputDir;
    IPCServer server(nullptr);
    server.setProjectManager(&manager());
    server.setAppState(IPCServer::AppState::Ready);
    for (const auto& path : {QStringLiteral("Actions"), QStringLiteral("Layers/BuildPacks"),
                            QStringLiteral("Busses/Packs"), QStringLiteral("Variables/Packs"),
                            QStringLiteral("Attenuations/Packs")}) {
        const auto change = run("set_pack_build_output_dir", {{"path", path}});
        EXPECT_FALSE(change.success);
        EXPECT_TRUE(change.errorMessage.contains(QStringLiteral("BuildPacks")));
        EXPECT_EQ(project().packBuildOutputDir, current);
        EXPECT_EQ(UndoRedoManager::instance().undoStack()->count(), undoCount);
        const QString output = QDir(root).filePath(path);
        const auto response = server.dispatchCommand("export_pack", {{"output_path", output},
            {"platform_id", Platforms::idString(low)}});
        EXPECT_EQ(response.value("code").toString(), QStringLiteral("INVALID_ARGS"));
        project().packBuildOutputDir = path;
        ProjectPackExporter exporter;
        ProjectPackExporter::ExportOptions options;
        options.platform = Platforms::idString(low);
        for (const auto& explicitOutput : {QString(), output}) {
            const auto result = exporter.ExportAllPacks(project(), explicitOutput, options);
            EXPECT_FALSE(result.success);
            EXPECT_TRUE(result.error.contains(QStringLiteral("object bucket"))) << result.error.toStdString();
        }
        EXPECT_FALSE(exporter.ExportInitPack(project(), QDir(output).filePath("Init.bytes"), options).success);
        EXPECT_FALSE(exporter.ExportActionPack(project(), action->name, QDir(output).filePath("Action.bytes"), options).success);
        EXPECT_FALSE(exporter.ExportSharedPack(project(), QDir(output).filePath("Shared.bytes"), options, {}, {}).success);
        EXPECT_FALSE(ProjectPackExporter::ValidatePackBuildReadiness(project(), output).valid);
        ProjectSerializer serializer;
        EXPECT_FALSE(serializer.saveProject(project().projectPath, project()));
        EXPECT_TRUE(serializer.lastError().contains(QStringLiteral("object bucket")));
        EXPECT_FALSE(manager().saveProject());
        EXPECT_EQ(bytes(project().projectPath), original);
        EXPECT_FALSE(QFile::exists(QDir(output).filePath("HarkenPackIndex.json")));
        EXPECT_FALSE(QFile::exists(QDir(output).filePath(".harken-pack-build.lock")));
        EXPECT_FALSE(QFile::exists(QDir(output).filePath("Init.bytes")));
        project().packBuildOutputDir = current;
    }
}

TEST_F(ProjectPlatformBuild, ExistingObjectBucketOutputCanBeLoadedAndCorrectedWithUndo)
{
    ASSERT_TRUE(manager().saveProject());
    const QString projectPath = project().projectPath;
    auto document = QJsonDocument::fromJson(bytes(projectPath)).object();
    auto content = document.value("content").toObject();
    content.insert("packBuildOutputDir", "Layers/BuildPacks");
    document.insert("content", content);
    writeDeliveryJson(projectPath, document);
    auto prepared = ProjectManager::prepareProject(projectPath);
    ASSERT_NE(prepared.project, nullptr) << prepared.error.toStdString();
    EXPECT_EQ(prepared.project->packBuildOutputDir, QStringLiteral("Layers/BuildPacks"));
    EXPECT_TRUE(prepared.diagnostics.join(QLatin1Char('\n')).contains(QStringLiteral("BuildPacks")));
    ASSERT_TRUE(manager().commitPreparedProject(std::move(prepared)));
    QElapsedTimer timer;
    timer.start();
    while (!manager().currentProject() && timer.elapsed() < 5000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    ASSERT_NE(manager().currentProject(), nullptr);
    const auto original = bytes(projectPath);
    EXPECT_FALSE(manager().saveProject());
    EXPECT_EQ(bytes(projectPath), original);
    ASSERT_TRUE(run("set_pack_build_output_dir", {{"path", "BuildPacks"}}).success);
    EXPECT_EQ(project().packBuildOutputDir, QStringLiteral("BuildPacks"));
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    EXPECT_EQ(project().packBuildOutputDir, QStringLiteral("Layers/BuildPacks"));
    EXPECT_FALSE(manager().saveProject());
    EXPECT_EQ(bytes(projectPath), original);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    EXPECT_EQ(project().packBuildOutputDir, QStringLiteral("BuildPacks"));
    ASSERT_TRUE(manager().saveProject());
    EXPECT_EQ(deliveryJson(projectPath).value("content").toObject().value("packBuildOutputDir").toString(),
              QStringLiteral("BuildPacks"));
}

TEST_F(ProjectPlatformBuild, DefaultOutputPublishesIndexAfterBuildAndCancellation)
{
    project().packBuildOutputDir = directory.filePath("DefaultDelivery");
    const QString root = project().packBuildOutputDir;
    ProjectPackExporter exporter;
    ProjectPackExporter::ExportOptions options;
    options.platform = Platforms::idString(low);
    bool polled = false;
    options.cancelCheck = [&] {
        polled = true;
        EXPECT_FALSE(QFile::exists(QDir(root).filePath("HarkenPackIndex.json")));
        return true;
    };
    const auto canceled = exporter.ExportAllPacks(project(), {}, options);
    EXPECT_TRUE(polled);
    EXPECT_FALSE(canceled.success);
    EXPECT_TRUE(canceled.canceled);
    EXPECT_TRUE(deliveryJson(QDir(root).filePath("HarkenPackIndex.json")).value("platforms").toArray().isEmpty());
    options.cancelCheck = {};
    ASSERT_TRUE(exporter.ExportAllPacks(project(), {}, options).success);
    EXPECT_EQ(deliveryJson(QDir(root).filePath("HarkenPackIndex.json")).value("platforms").toArray().size(), 1);
}

TEST_F(ProjectPlatformBuild, DefaultOutputExcludesPlatformsRenamedDuringBuild)
{
    project().packBuildOutputDir = directory.filePath("DefaultDelivery");
    const QString root = project().packBuildOutputDir;
    ProjectPackExporter exporter;
    ProjectPackExporter::ExportOptions options;
    options.platform = Platforms::idString(low);
    bool renamed = false;
    options.cancelCheck = [&] {
        if (!renamed) {
            renamed = true;
            EXPECT_TRUE(run("rename_project_platform", {{"platform_id", options.platform}, {"name", "Mobile"}}).success);
        }
        return false;
    };
    const auto result = exporter.ExportAllPacks(project(), {}, options);
    EXPECT_TRUE(renamed);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(QFile::exists(QDir(root).filePath("Android_Low/Init.bytes")));
    EXPECT_FALSE(QFile::exists(QDir(root).filePath("Mobile/Init.bytes")));
    EXPECT_TRUE(deliveryJson(QDir(root).filePath("HarkenPackIndex.json")).value("platforms").toArray().isEmpty());
}

TEST_F(ProjectPlatformBuild, BundleDeploymentKeepsCapturedInstanceAfterAnotherBuild)
{
    const QString root = directory.filePath("Bundles");
    const auto first = RuntimeContentBundleBuilder::build(project(), root, Platforms::idString(low));
    ASSERT_TRUE(first.success);
    const auto second = RuntimeContentBundleBuilder::build(project(), root, Platforms::idString(high));
    ASSERT_TRUE(second.success);
    QString error;
    const QString target = directory.filePath("Deployed");
    ASSERT_TRUE(RuntimeContentBundleBuilder::deploy(first, target, &error)) << error.toStdString();
    const auto manifest = deliveryJson(QDir(target).filePath("HarkenContent.json"));
    EXPECT_EQ(manifest.value("platformId").toString(), Platforms::idString(low));
    EXPECT_EQ(manifest.value("platformName").toString(), QStringLiteral("Android_Low"));
    EXPECT_EQ(manifest.value("basePlatform").toString(), QStringLiteral("Android"));
    EXPECT_EQ(manifest.value("bundleId").toString(), first.bundleId);
    EXPECT_FALSE(manifest.contains("platform"));
    for (const auto& value : manifest.value("orderedPacks").toArray()) {
        const auto row = value.toObject();
        EXPECT_EQ(row.value("platformId").toString(), Platforms::idString(low));
        EXPECT_TRUE(QFile::exists(QDir(target).filePath(row.value("path").toString())));
    }
}


TEST_F(ProjectPlatformBuild, CaseOnlyRenamePublishesTheExactDirectoryName)
{
    const QString root = directory.filePath("Delivery");
    std::vector<ProjectPackExporter::PackBuildRecord> records;
    ASSERT_TRUE(exportPlatform(low, QDir(root).filePath("Android_Low"), records).success);
    const auto id = Platforms::idString(low);
    ASSERT_TRUE(run("rename_project_platform", {{"platform_id", id}, {"name", "android_low"}}).success);
    records.clear();
    ASSERT_TRUE(exportPlatform(low, QDir(root).filePath("android_low"), records).success);
    QString error;
    ASSERT_TRUE(ProjectPackExporter::PublishPackIndex(root, project().platforms, {id}, {id}, &error)) << error.toStdString();
    const auto names = QDir(root).entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    EXPECT_TRUE(names.contains(QStringLiteral("android_low"), Qt::CaseSensitive));
}

TEST_F(ProjectPlatformBuild, IpcPartialFailurePublishesTheSuccessfulSiblingOnly)
{
    const QString root = directory.filePath("Delivery");
    auto* authored = sound->comp<VSound>();
    authored->sources.plain.active = QUuid{};
    authored->sources.plain.platforms.clear();
    authored->sources.plain.platforms[Platforms::idString(low)].active = authored->sources.plain.entries.front().entryId;
    ProjectManager::instance().setPackExportSelectedPlatformsInternal({Platforms::idString(high), Platforms::idString(low)});
    IPCServer server(nullptr);
    server.setProjectManager(&manager());
    server.setAppState(IPCServer::AppState::Ready);
    const auto response = server.dispatchCommand("export_pack", {{"output_path", root}});
    ASSERT_EQ(response.value("status").toString(), QStringLiteral("ok"));
    QElapsedTimer timer;
    timer.start();
    QJsonObject status;
    do {
        QCoreApplication::processEvents();
        QThread::msleep(2);
        status = server.dispatchCommand("get_pack_build_status", {}).value("result").toObject();
    } while (status.value("in_progress").toBool() && timer.elapsed() < 30000);
    ASSERT_LT(timer.elapsed(), 30000);
    const auto report = status.value("result").toObject().value("result").toObject();
    EXPECT_FALSE(report.value("success").toBool());
    EXPECT_EQ(report.value("platforms").toArray().size(), 2);
    const auto entries = deliveryJson(QDir(root).filePath("HarkenPackIndex.json")).value("platforms").toArray();
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries.first().toObject().value("id").toString(), Platforms::idString(low));
}

TEST_F(ProjectPlatformBuild, IpcCancellationRemovesRequestedPlatformsFromIndex)
{
    const QString root = directory.filePath("Delivery");
    std::vector<ProjectPackExporter::PackBuildRecord> records;
    ASSERT_TRUE(exportPlatform(low, QDir(root).filePath("Android_Low"), records).success);
    const auto id = Platforms::idString(low);
    QString error;
    ASSERT_TRUE(ProjectPackExporter::PublishPackIndex(root, project().platforms, {id}, {id}, &error));
    const auto original = bytes(QDir(root).filePath("Android_Low/Init.bytes"));
    IPCServer server(nullptr);
    server.setProjectManager(&manager());
    server.setAppState(IPCServer::AppState::Ready);
    ASSERT_EQ(server.dispatchCommand("export_pack", {{"output_path", root}, {"platform_id", id}}).value("status").toString(), QStringLiteral("ok"));
    server.dispatchCommand("cancel_pack_build", {});
    QElapsedTimer timer;
    timer.start();
    while (server.dispatchCommand("get_pack_build_status", {}).value("result").toObject().value("in_progress").toBool()
           && timer.elapsed() < 30000) {
        QCoreApplication::processEvents();
        QThread::msleep(2);
    }
    ASSERT_LT(timer.elapsed(), 30000);
    EXPECT_TRUE(deliveryJson(QDir(root).filePath("HarkenPackIndex.json")).value("platforms").toArray().isEmpty());
    EXPECT_EQ(bytes(QDir(root).filePath("Android_Low/Init.bytes")), original);
}


TEST_F(ProjectPlatformBuild, CompetingOutputRequestsDoNotChangePublishedIndex)
{
    const QString root = directory.filePath("Delivery");
    std::vector<ProjectPackExporter::PackBuildRecord> records;
    ASSERT_TRUE(exportPlatform(low, QDir(root).filePath("Android_Low"), records).success);
    const auto id = Platforms::idString(low);
    QString error;
    ASSERT_TRUE(ProjectPackExporter::PublishPackIndex(root, project().platforms, {id}, {id}, &error));
    const auto original = bytes(QDir(root).filePath("HarkenPackIndex.json"));
    const auto lock = ProjectPackExporter::LockPackOutputRoot(root, &error);
    ASSERT_NE(lock, nullptr);
    IPCServer server(nullptr);
    server.setProjectManager(&manager());
    server.setAppState(IPCServer::AppState::Ready);
    const auto rejected = server.dispatchCommand("export_pack", {{"output_path", root}, {"platform_id", id}});
    EXPECT_EQ(rejected.value("code").toString(), QStringLiteral("OUTPUT_BUSY"));
    EXPECT_EQ(bytes(QDir(root).filePath("HarkenPackIndex.json")), original);
}


TEST_F(ProjectPlatformBuild, WritesTwoAndroidInstancesForNativeSdkReader)
{
    const QString root = QDir(QString::fromUtf8(HARKEN_TEMP_DIR)).filePath("p6-sdk-delivery");
    QString error;
    QStringList ids;
    for (const auto id : {low, high}) {
        std::vector<ProjectPackExporter::PackBuildRecord> records;
        ASSERT_TRUE(exportPlatform(id, QDir(root).filePath(Platforms::find(project().platforms, id)->name), records).success);
        ids.append(Platforms::idString(id));
    }
    ASSERT_TRUE(ProjectPackExporter::PublishPackIndex(root, project().platforms, ids, ids, &error)) << error.toStdString();
}


TEST_F(ProjectPlatformBuild, DestroyingIpcServerCompletesOutputPublication)
{
    const QString root = directory.filePath("Delivery");
    const auto id = Platforms::idString(low);
    {
        IPCServer server(nullptr);
        server.setProjectManager(&manager());
        server.setAppState(IPCServer::AppState::Ready);
        const auto response = server.dispatchCommand("export_pack", {{"output_path", root}, {"platform_id", id}});
        ASSERT_EQ(response.value("status").toString(), QStringLiteral("ok"));
    }
    const auto entries = deliveryJson(QDir(root).filePath("HarkenPackIndex.json")).value("platforms").toArray();
    ASSERT_EQ(entries.size(), 1);
    EXPECT_EQ(entries.first().toObject().value("id").toString(), id);
    QString error;
    EXPECT_NE(ProjectPackExporter::LockPackOutputRoot(root, &error), nullptr) << error.toStdString();
}

}
