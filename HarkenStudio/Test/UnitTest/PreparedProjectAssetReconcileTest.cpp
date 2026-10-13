#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AssetPipeline/ImportAssetsCommand.h"
#include "Core/AssetPipeline/ImportSoundSourcesCommand.h"
#include "Core/AssetPipeline/PathUtils.h"
#include "Core/AssetPipeline/SourceFileSystem.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VSound.h"
#include "Core/ProjectManager.h"
#include "Core/ProjectSerializer.h"

#include <gtest/gtest.h>

#include <future>

#include <QApplication>
#include <QDataStream>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QMap>
#include <QJsonDocument>
#include <QTemporaryDir>

namespace {

void ensureApp()
{
    if (qApp)
        return;
    static int argc = 1;
    static char arg0[] = "harken_editor_tests";
    static char* argv[] = {arg0, nullptr};
    static QApplication app(argc, argv);
}

QString createPreparedProject(QTemporaryDir& tempDir, const QString& name)
{
    VNX::ProjectConfig config;
    config.name = name;
    config.location = tempDir.path();
    VNX::PreparedProject prepared = VNX::ProjectManager::prepareProject(config);
    EXPECT_NE(prepared.project, nullptr);
    return prepared.project ? prepared.project->projectPath : QString();
}

void writeWavPcm16(const QString& path, qint16 sample = 0)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    QDataStream stream(&file);
    stream.setByteOrder(QDataStream::LittleEndian);
    stream.writeRawData("RIFF", 4);
    stream << quint32(38);
    stream.writeRawData("WAVE", 4);
    stream.writeRawData("fmt ", 4);
    stream << quint32(16) << quint16(1) << quint16(1) << quint32(48000)
           << quint32(96000) << quint16(2) << quint16(16);
    stream.writeRawData("data", 4);
    stream << quint32(2) << sample;
    ASSERT_EQ(stream.status(), QDataStream::Ok);
}

VNX::AssetInfo writeOrphanAsset(const QString& projectRoot,
                                const QString& relativePath)
{
    const QString audioRoot = VNX::PathUtils::getAudioSourcesDir(projectRoot);
    QDir().mkpath(QFileInfo(QDir(audioRoot).filePath(relativePath)).absolutePath());
    QFile source(QDir(audioRoot).filePath(relativePath));
    EXPECT_TRUE(source.open(QIODevice::WriteOnly));
    source.write("orphan");
    source.close();

    VNX::AssetInfo asset;
    asset.sourceID = 101;
    asset.relativePath = relativePath;
    asset.absolutePath = source.fileName();
    QDir().mkpath(VNX::PathUtils::getAssetRegistryCacheDir(projectRoot));
    EXPECT_TRUE(VNX::AssetRegistry::saveSnapshotToFile(projectRoot, {asset}));
    const QStringList projectFiles = QDir(projectRoot).entryList({QStringLiteral("*.hsp")}, QDir::Files);
    if (!projectFiles.isEmpty()) {
        const QString projectPath = QDir(projectRoot).filePath(projectFiles.front());
        auto prepared = VNX::ProjectManager::prepareProject(projectPath);
        EXPECT_NE(prepared.project, nullptr);
        if (!prepared.project) return asset;
        VNX::ProjectSerializer serializer;
        prepared.project->sourceAssets.insert(asset.sourceID, relativePath);
        EXPECT_TRUE(serializer.saveProject(projectPath, *prepared.project))
            << serializer.lastError().toStdString();
    }
    return asset;
}

}

TEST(PreparedProjectAssetReconcile, WorkerIntegrityScanUsesOnlyTheSuppliedProjectAndAssets)
{
    using namespace VNX;
    const auto item = [](ProjectItem::Type type, uint32_t id) {
        auto result = std::make_shared<ProjectItem>(QString::number(id), type);
        result->objectID = id;
        result->guid = QUuid::createUuid();
        result->attachDefaultComponents();
        return result;
    };
    auto master = item(ProjectItem::Type::Bus, 700);
    auto output = item(ProjectItem::Type::Bus, 701);
    auto send = item(ProjectItem::Type::Bus, 702);
    for (const auto& bus : {output, send}) {
        bus->parent = master.get();
        master->children.push_back(bus);
    }
    output->comp<VMixNode>()->sends = {{send->objectID, 1.0f}};
    auto sound = item(ProjectItem::Type::Sound, 800);
    sound->comp<VMixNode>()->outputBusId = output->objectID;
    sound->comp<VSound>()->sources.plain.entries = {{QUuid::createUuid(), 1701, {}}};
    ProjectData project;
    project.busses.push_back({master, {}});
    project.layers.push_back({sound, {}});
    AssetInfo asset;
    asset.sourceID = 1701;
    const QList<AssetInfo> assets{asset};
    const auto report = std::async(std::launch::async, [&] {
        return IntegrityScanner().scan(project, assets);
    }).get();
    EXPECT_TRUE(report.issues.empty());
    const auto missing = IntegrityScanner().scan(project, {});
    ASSERT_EQ(missing.issues.size(), 1u);
    EXPECT_EQ(missing.issues.front().kind, IntegrityScanner::RefKind::ItemSourceID);
    EXPECT_EQ(missing.issues.front().missingTarget, QStringLiteral("1701"));
}

TEST(PreparedProjectAssetReconcile, RejectsProjectWhenBindingLoadFails)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    const QString projectPath = createPreparedProject(
        tempDir, QStringLiteral("FailedBindingReconcile"));
    ASSERT_FALSE(projectPath.isEmpty());
    const QString projectRoot = QFileInfo(projectPath).absolutePath();
    const VNX::AssetInfo asset = writeOrphanAsset(
        projectRoot, QStringLiteral("missing-binding.wav"));

    QFile broken(QDir(projectRoot).filePath(QStringLiteral("Layers/Broken.vp")));
    ASSERT_TRUE(broken.open(QIODevice::WriteOnly));
    broken.write("{}");
    broken.close();

    VNX::PreparedProject prepared =
        VNX::ProjectManager::prepareProject(projectPath);
    ASSERT_EQ(prepared.project, nullptr);
    EXPECT_EQ(prepared.audioLoadFailures, 0);
    EXPECT_TRUE(QFileInfo::exists(asset.absolutePath));
}

TEST(PreparedProjectAssetReconcile, DoesNotCleanupAssetsWhenProjectOpens)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    const QString projectPath = createPreparedProject(
        tempDir, QStringLiteral("SuccessfulBindingReconcile"));
    ASSERT_FALSE(projectPath.isEmpty());
    const QString projectRoot = QFileInfo(projectPath).absolutePath();
    const VNX::AssetInfo asset = writeOrphanAsset(
        projectRoot, QStringLiteral("unreferenced.wav"));

    VNX::PreparedProject prepared =
        VNX::ProjectManager::prepareProject(projectPath);
    ASSERT_NE(prepared.project, nullptr);
    ASSERT_EQ(prepared.audioLoadFailures, 0);
    ASSERT_TRUE(VNX::ProjectManager::instance().commitPreparedProject(
        std::move(prepared)));

    EXPECT_TRUE(VNX::AssetRegistry::instance().contains(asset.sourceID));
    EXPECT_TRUE(QFileInfo::exists(asset.absolutePath));
    VNX::ProjectManager::instance().closeProject();
}

TEST(PreparedProjectAssetReconcile, PreparedProjectCanImportAudioImmediately)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());

    VNX::ProjectConfig config;
    config.name = QStringLiteral("ImmediateImport");
    config.location = tempDir.path();
    VNX::PreparedProject prepared = VNX::ProjectManager::prepareProject(config);
    ASSERT_NE(prepared.project, nullptr);

    const QString projectRoot =
        QFileInfo(prepared.project->projectPath).absolutePath();
    const QString audioRoot = VNX::PathUtils::getAudioSourcesDir(projectRoot);
    ASSERT_FALSE(QDir(audioRoot).exists());
    ASSERT_TRUE(VNX::ProjectManager::instance().commitPreparedProject(
        std::move(prepared)));
    ASSERT_TRUE(QDir(audioRoot).exists());

    const QString sourcePath = tempDir.filePath(QStringLiteral("Skill_PC.wav"));
    writeWavPcm16(sourcePath);
    VNX::ImportAssetsCommand* command =
        VNX::ImportAssetsCommand::pushImportAtRoot({sourcePath});

    EXPECT_EQ(command->successCount(), 1);
    EXPECT_EQ(command->failCount(), 0);
    EXPECT_TRUE(QFileInfo::exists(
        QDir(audioRoot).filePath(QStringLiteral("Skill_PC.wav"))));
    VNX::ProjectManager::instance().closeProject();
}

TEST(PreparedProjectAssetReconcile, InvalidRegistryReadPreservesLoadedAssets)
{
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    const VNX::AssetInfo asset = writeOrphanAsset(
        tempDir.path(), QStringLiteral("Keep.wav"));
    VNX::AssetRegistry& registry = VNX::AssetRegistry::instance();
    registry.replaceSnapshot(tempDir.path(), {asset});

    QFile file(QDir(VNX::PathUtils::getAssetRegistryCacheDir(tempDir.path()))
                   .filePath(QStringLiteral("assets.json")));
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    file.write("{\"version\":1,\"assets\":[]}");
    file.close();
    EXPECT_FALSE(registry.loadFromFile());
    ASSERT_TRUE(registry.contains(asset.sourceID));
    EXPECT_EQ(registry.getAsset(asset.sourceID)->relativePath, asset.relativePath);
    EXPECT_TRUE(QFileInfo::exists(asset.absolutePath));
    registry.clear();
}

TEST(PreparedProjectAssetReconcile, SourceAssetReferencesRejectAmbiguousOrUnsafePaths)
{
    QMap<uint32_t, QString> references{{9, QStringLiteral("Keep.wav")}};
    for (const QJsonObject& value : {
             QJsonObject{{"0", "A.wav"}}, QJsonObject{{"01", "A.wav"}},
             QJsonObject{{"4294967296", "A.wav"}}, QJsonObject{{"1", "../A.wav"}},
             QJsonObject{{"1", 10}}, QJsonObject{{"1", "A.wav"}, {"2", "./A.wav"}}}) {
        EXPECT_FALSE(VNX::AssetRegistry::sourceAssetsFromJson(value, references));
        EXPECT_EQ(references.value(9), QStringLiteral("Keep.wav"));
    }
    ASSERT_TRUE(VNX::AssetRegistry::sourceAssetsFromJson(
        QJsonObject{{"1", "Voice/A.wav"}, {"2", "Voice/B.wav"}}, references));
    EXPECT_EQ(references.size(), 2);
    EXPECT_EQ(references.value(2), QStringLiteral("Voice/B.wav"));
}

TEST(PreparedProjectAssetReconcile, MissingAudioRetainsItsPersistentIdentityAndPath)
{
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    QList<VNX::AssetInfo> assets;
    const QMap<uint32_t, QString> references{{201, QStringLiteral("Missing.wav")}};
    EXPECT_TRUE(VNX::AssetRegistry::reconcileSnapshot(tempDir.path(), references, assets));
    ASSERT_EQ(assets.size(), 1);
    EXPECT_EQ(assets.front().sourceID, 201u);
    EXPECT_EQ(assets.front().relativePath, QStringLiteral("Missing.wav"));
    EXPECT_EQ(assets.front().metadata.totalSamples, 0u);
    EXPECT_FALSE(QFileInfo::exists(assets.front().absolutePath));
}

TEST(PreparedProjectAssetReconcile, FailedSaveDoesNotPublishNewSourceAssetReferences)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    const QString projectPath = createPreparedProject(tempDir, QStringLiteral("ReferenceRollback"));
    auto& manager = VNX::ProjectManager::instance();
    ASSERT_TRUE(manager.commitPreparedProject(VNX::ProjectManager::prepareProject(projectPath)));
    const QString savedInput = tempDir.filePath(QStringLiteral("Saved.wav"));
    const QString pendingInput = tempDir.filePath(QStringLiteral("Pending.wav"));
    writeWavPcm16(savedInput, 1);
    writeWavPcm16(pendingInput, 2);
    ASSERT_EQ(VNX::ImportAssetsCommand::pushImportAtRoot({savedInput})->successCount(), 1);
    ASSERT_TRUE(manager.saveProject());
    auto* pending = VNX::ImportAssetsCommand::pushImportAtRoot({pendingInput});
    ASSERT_EQ(pending->successCount(), 1);
    const uint32_t pendingId = pending->importedIDs().front();
    const auto channelConfig = manager.currentProject()->channelConfig;
    manager.currentProject()->channelConfig = static_cast<VNX::AudioChannelConfig>(999);
    EXPECT_FALSE(manager.saveProject());
    manager.currentProject()->channelConfig = channelConfig;
    const auto recovered = VNX::ProjectManager::prepareProject(projectPath);
    ASSERT_NE(recovered.project, nullptr) << recovered.error.toStdString();
    EXPECT_FALSE(recovered.project->sourceAssets.contains(pendingId));
    ASSERT_EQ(recovered.assets.size(), 1);
    EXPECT_EQ(recovered.assets.front().relativePath, QStringLiteral("Saved.wav"));
    EXPECT_TRUE(VNX::AssetRegistry::instance().isUncommitted(pendingId));
    manager.closeProject();
}

TEST(PreparedProjectAssetReconcile, DamagedCachesRebuildWithoutChangingSourceIdentity)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    const QString projectPath = createPreparedProject(
        tempDir, QStringLiteral("DamagedCacheRecovery"));
    ASSERT_FALSE(projectPath.isEmpty());
    VNX::ProjectManager& manager = VNX::ProjectManager::instance();
    ASSERT_TRUE(manager.commitPreparedProject(
        VNX::ProjectManager::prepareProject(projectPath)));

    const QString input = tempDir.filePath(QStringLiteral("Keep.wav"));
    writeWavPcm16(input);
    VNX::ImportAssetsCommand* command =
        VNX::ImportAssetsCommand::pushImportAtRoot({input});
    ASSERT_EQ(command->successCount(), 1);
    const VNX::AssetInfo* imported =
        VNX::AssetRegistry::instance().getAssetByPath(QStringLiteral("Keep.wav"));
    ASSERT_NE(imported, nullptr);
    const uint32_t sourceID = imported->sourceID;
    const QString audioPath = imported->absolutePath;
    ASSERT_TRUE(manager.saveProject());
    manager.closeProject();

    const QDir projectRoot(QFileInfo(projectPath).absolutePath());
    const auto writeBytes = [](const QString& path, const QByteArray& bytes) {
        EXPECT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::WriteOnly));
        EXPECT_EQ(file.write(bytes), bytes.size());
    };
    const auto readBytes = [](const QString& path) {
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        return file.readAll();
    };
    const QByteArray audioBytes = readBytes(audioPath);
    const QString trashPath = projectRoot.filePath(QStringLiteral(".trash/Retained.wav"));
    const QByteArray trashBytes = QByteArrayLiteral("recoverable audio");
    writeBytes(trashPath, trashBytes);

    const QByteArray staleRegistry = QJsonDocument(QJsonObject{
        {"version", 1}, {"assets", QJsonObject{{QString::number(sourceID), QJsonObject{
            {"sourceID", static_cast<qint64>(sourceID)}, {"relativePath", "Wrong.wav"}}}}}}).toJson();
    for (const QByteArray& registryBytes : {
             QByteArray(), staleRegistry,
             QByteArrayLiteral("{broken"),
             QByteArrayLiteral("{\"version\":1,\"assets\":{}}"),
             QByteArrayLiteral("{\"version\":1,\"assets\":[]}"),
             QByteArrayLiteral("{\"version\":1,\"assets\":{\"501\":{\"sourceID\":502,\"relativePath\":\"Keep.wav\"}}}")}) {
        writeBytes(projectRoot.filePath(QStringLiteral(".cache/asset_registry/assets.json")),
                   registryBytes);
        if (registryBytes.isEmpty())
            ASSERT_TRUE(QFile::remove(projectRoot.filePath(QStringLiteral(".cache/asset_registry/assets.json"))));
        for (const QString& relative : {
                 QStringLiteral(".cache/project_index/index.json"),
                 QStringLiteral(".cache/project_index/integrity.json"),
                 QStringLiteral(".cache/editor_layout.json")})
            writeBytes(projectRoot.filePath(relative), QByteArrayLiteral("{broken"));

        VNX::PreparedProject recovered = VNX::ProjectManager::prepareProject(projectPath);
        ASSERT_NE(recovered.project, nullptr) << recovered.error.toStdString();
        EXPECT_TRUE(recovered.rebuildAssetRegistry);
        ASSERT_EQ(recovered.assets.size(), 1);
        EXPECT_EQ(recovered.assets.first().sourceID, sourceID);
        EXPECT_EQ(recovered.assets.first().relativePath, QStringLiteral("Keep.wav"));
        EXPECT_EQ(recovered.assets.first().metadata.sampleRate, 48000u);
        EXPECT_FALSE(recovered.assets.first().contentHash.isEmpty());
        ASSERT_TRUE(manager.commitPreparedProject(std::move(recovered)));
        EXPECT_TRUE(VNX::AssetRegistry::instance().contains(sourceID));
        EXPECT_EQ(readBytes(audioPath), audioBytes);
        EXPECT_EQ(readBytes(trashPath), trashBytes);
        ASSERT_TRUE(manager.saveProject());
        manager.closeProject();
    }

    VNX::PreparedProject reopened = VNX::ProjectManager::prepareProject(projectPath);
    ASSERT_NE(reopened.project, nullptr) << reopened.error.toStdString();
    EXPECT_FALSE(reopened.rebuildAssetRegistry);
    ASSERT_EQ(reopened.assets.size(), 1);
    EXPECT_EQ(reopened.assets.first().sourceID, sourceID);
    EXPECT_EQ(readBytes(audioPath), audioBytes);
    EXPECT_EQ(readBytes(trashPath), trashBytes);
}

TEST(PreparedProjectAssetReconcile, BothModesRecoverAllCandidatesAndActiveStatesAfterSourceRename)
{
    ensureApp();
    QTemporaryDir tempDir;
    ASSERT_TRUE(tempDir.isValid());
    const QString projectPath = createPreparedProject(
        tempDir, QStringLiteral("MultiSourceRecovery"));
    ASSERT_FALSE(projectPath.isEmpty());
    VNX::ProjectManager& manager = VNX::ProjectManager::instance();
    ASSERT_TRUE(manager.commitPreparedProject(
        VNX::ProjectManager::prepareProject(projectPath)));

    const QString primaryInput = tempDir.filePath(QStringLiteral("Primary.wav"));
    const QString unresolvedInput = tempDir.filePath(QStringLiteral("Unresolved.wav"));
    const QString sharedInput = tempDir.filePath(QStringLiteral("Shared.wav"));
    writeWavPcm16(primaryInput, 1);
    writeWavPcm16(unresolvedInput, 2);
    writeWavPcm16(sharedInput, 3);
    ASSERT_EQ(VNX::ImportAssetsCommand::pushImportAtRoot({primaryInput, sharedInput})
                  ->successCount(), 2);
    auto* multiSound = manager.currentProject()->layers.front().rootItem.get();
    ASSERT_NE(multiSound, nullptr);
    const QUuid multiGuid = multiSound->guid;
    auto* binding = VNX::findFileBindingForItem(*manager.currentProject(), multiSound);
    ASSERT_NE(binding, nullptr);
    ASSERT_EQ(VNX::ImportSoundSourcesCommand::pushImport(
                  {unresolvedInput, sharedInput}, binding, multiSound)->successCount(), 2);
    const auto originalSources = multiSound->comp<VNX::VSound>()->sources.plain.entries;
    ASSERT_EQ(originalSources.size(), 3u);
    const uint32_t primaryId = originalSources[0].sourceId;
    const uint32_t unresolvedId = originalSources[1].sourceId;
    const uint32_t sharedId = originalSources[2].sourceId;
    const QString localizedInput = tempDir.filePath(QStringLiteral("Localized.wav"));
    writeWavPcm16(localizedInput, 5);
    manager.currentProject()->projectLanguages = {QStringLiteral("English"), QStringLiteral("Japanese")};
    auto* sound = multiSound->comp<VNX::VSound>();
    sound->distinguishLanguage = true;
    ASSERT_EQ(VNX::ImportSoundSourcesCommand::pushImport(
                  {localizedInput, sharedInput}, binding, multiSound, QStringLiteral("English"))->successCount(), 2);
    ASSERT_EQ(VNX::ImportSoundSourcesCommand::pushImport(
                  {primaryInput}, binding, multiSound, QStringLiteral("Japanese"))->successCount(), 1);
    const QString platform = manager.currentProject()->platformIds().front();
    sound->sources.plain.platforms[platform].active = originalSources[1].entryId;
    auto& english = sound->sources.localized[QStringLiteral("English")];
    english.platforms[platform].active = english.entries[1].entryId;
    const uint32_t localizedId = english.entries[0].sourceId;
    const auto originalBanks = sound->sources;
    sound->distinguishLanguage = false;
    ASSERT_TRUE(VNX::SourceFileSystem::rename(QStringLiteral("Unresolved.wav"), QStringLiteral("Renamed.wav")));
    ASSERT_TRUE(manager.saveProject());
    manager.closeProject();

    const QDir projectRoot(QFileInfo(projectPath).absolutePath());
    ASSERT_TRUE(QDir().mkpath(projectRoot.filePath(QStringLiteral(".trash"))));
    writeWavPcm16(projectRoot.filePath(QStringLiteral(".trash/Retained.wav")), 4);
    const auto audioFiles = [&]() {
        QMap<QString, QByteArray> files;
        for (const QString& folder : {QStringLiteral("AudioSources"), QStringLiteral(".trash")}) {
            QDirIterator iterator(projectRoot.filePath(folder), QDir::Files,
                                  QDirIterator::Subdirectories);
            while (iterator.hasNext()) {
                QFile file(iterator.next());
                EXPECT_TRUE(file.open(QIODevice::ReadOnly));
                files.insert(projectRoot.relativeFilePath(file.fileName()), file.readAll());
            }
        }
        return files;
    };
    const auto originalAudio = audioFiles();
    ASSERT_EQ(originalAudio.size(), 5);
    QFile registryFile(projectRoot.filePath(QStringLiteral(".cache/asset_registry/assets.json")));
    ASSERT_TRUE(registryFile.open(QIODevice::WriteOnly));
    registryFile.write("{broken");
    registryFile.close();

    {
        const auto prepared = VNX::ProjectManager::prepareProject(projectPath);
        ASSERT_NE(prepared.project, nullptr) << prepared.error.toStdString();
        VNX::IntegrityScanner scanner;
        auto staleReport = scanner.scan(*prepared.project, {});
        ASSERT_FALSE(staleReport.issues.empty());
        staleReport.indexHash = prepared.index->contentHash();
        ASSERT_TRUE(scanner.saveToCache(projectRoot.absolutePath(), staleReport));
    }

    VNX::AssetInfo foreignAsset;
    foreignAsset.sourceID = primaryId;
    foreignAsset.relativePath = QStringLiteral("DifferentProject.wav");
    VNX::AssetRegistry::instance().replaceSnapshot(
        tempDir.filePath(QStringLiteral("OtherProject")), {foreignAsset});
    VNX::PreparedProject recovered = VNX::ProjectManager::prepareProject(projectPath);
    ASSERT_NE(recovered.project, nullptr) << recovered.error.toStdString();
    EXPECT_TRUE(recovered.rebuildAssetRegistry);
    ASSERT_EQ(recovered.assets.size(), 4);
    QMap<uint32_t, QString> paths;
    for (const auto& asset : recovered.assets)
        paths.insert(asset.sourceID, asset.relativePath);
    EXPECT_EQ(paths.value(primaryId), QStringLiteral("Primary.wav"));
    EXPECT_EQ(paths.value(sharedId), QStringLiteral("Shared.wav"));
    EXPECT_EQ(paths.value(unresolvedId), QStringLiteral("Renamed.wav"));
    EXPECT_EQ(paths.value(localizedId), QStringLiteral("Localized.wav"));
    EXPECT_TRUE(recovered.integrityReport.issues.empty());
    EXPECT_EQ(VNX::AssetRegistry::instance().getAsset(primaryId)->relativePath,
              foreignAsset.relativePath);

    const auto snapshot = VNX::AssetRegistry::instance().buildSnapshot(
        projectRoot.absolutePath(), recovered.project->platforms);
    ASSERT_EQ(snapshot.assets.size(), 4);
    EXPECT_EQ(snapshot.assets.value(unresolvedId).relativePath, QStringLiteral("Renamed.wav"));
    EXPECT_EQ(snapshot.assets.value(unresolvedId).metadata.sampleRate, 48000u);

    ASSERT_TRUE(manager.commitPreparedProject(std::move(recovered)));
    EXPECT_EQ(audioFiles(), originalAudio);
    ASSERT_TRUE(manager.saveProject());
    manager.closeProject();
    VNX::PreparedProject reopened = VNX::ProjectManager::prepareProject(projectPath);
    ASSERT_NE(reopened.project, nullptr) << reopened.error.toStdString();
    EXPECT_FALSE(reopened.rebuildAssetRegistry);
    ASSERT_EQ(reopened.assets.size(), 4);
    const auto* reopenedBinding = VNX::findFileBindingByRootGuid(
        *reopened.project, multiGuid.toString(QUuid::WithoutBraces));
    ASSERT_NE(reopenedBinding, nullptr);
    const auto* reopenedSound = reopenedBinding->rootItem->comp<VNX::VSound>();
    ASSERT_NE(reopenedSound, nullptr);
    EXPECT_EQ(reopenedSound->sources, originalBanks);
    EXPECT_FALSE(reopenedSound->distinguishLanguage);
    ASSERT_EQ(reopenedSound->sources.plain.entries.size(), originalSources.size());
    for (size_t i = 0; i < originalSources.size(); ++i) {
        EXPECT_EQ(reopenedSound->sources.plain.entries[i].sourceId, originalSources[i].sourceId);
        EXPECT_EQ(reopenedSound->sources.plain.entries[i].entryId, originalSources[i].entryId);
    }
    ASSERT_TRUE(manager.commitPreparedProject(std::move(reopened)));
    ASSERT_TRUE(manager.saveProject());
    manager.closeProject();
    EXPECT_EQ(audioFiles(), originalAudio);
}
