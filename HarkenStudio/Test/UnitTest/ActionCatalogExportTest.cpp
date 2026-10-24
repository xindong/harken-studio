#include "UnitTest/StudioObjectIdTestSupport.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/PathUtils.h"
#include "Components/VAction.h"
#include "Components/VSound.h"
#include "CanonicalPackSetExport.h"
#include "ProjectManager.h"
#include "ProjectPackExporter.h"
#include "ProjectTypes.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "FieldGroupProjectAdapter.h"
#include "Support/AITestSupport.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "UndoCommands.h"

#include <harken_studio_build/PackImage.h>

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#ifndef HARKEN_PACKC_PATH
#error "HARKEN_PACKC_PATH must point at the packc executable"
#endif

using namespace VNX;

namespace {

using TestSupport::ensureApp;

void appendU16(QByteArray& out, uint16_t value)
{
    out.append(static_cast<char>(value & 0xFFu));
    out.append(static_cast<char>((value >> 8) & 0xFFu));
}

void appendU32(QByteArray& out, uint32_t value)
{
    out.append(static_cast<char>(value & 0xFFu));
    out.append(static_cast<char>((value >> 8) & 0xFFu));
    out.append(static_cast<char>((value >> 16) & 0xFFu));
    out.append(static_cast<char>((value >> 24) & 0xFFu));
}

void writeWavF32(const QString& path, const std::vector<float>& interleaved,
                 uint16_t channels, uint32_t sampleRate)
{
    const uint32_t frames = static_cast<uint32_t>(interleaved.size() / channels);
    const uint32_t dataBytes = static_cast<uint32_t>(interleaved.size() * sizeof(float));
    const uint16_t blockAlign = static_cast<uint16_t>(channels * sizeof(float));
    const uint32_t byteRate = sampleRate * blockAlign;
    constexpr uint32_t fmtSize = 18;
    constexpr uint32_t factSize = 4;
    const uint32_t riffSize = 4 + (8 + fmtSize) + (8 + factSize) + (8 + dataBytes);
    QByteArray bytes;
    bytes.append("RIFF", 4);
    appendU32(bytes, riffSize);
    bytes.append("WAVE", 4);
    bytes.append("fmt ", 4);
    appendU32(bytes, fmtSize);
    appendU16(bytes, 3);
    appendU16(bytes, channels);
    appendU32(bytes, sampleRate);
    appendU32(bytes, byteRate);
    appendU16(bytes, blockAlign);
    appendU16(bytes, 32);
    appendU16(bytes, 0);
    bytes.append("fact", 4);
    appendU32(bytes, factSize);
    appendU32(bytes, frames);
    bytes.append("data", 4);
    appendU32(bytes, dataBytes);
    bytes.append(reinterpret_cast<const char*>(interleaved.data()),
                 static_cast<qsizetype>(dataBytes));
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(bytes), bytes.size());
}

uint32_t registerToneAsset(const QString& projectRoot, const QString& relativePath)
{
    std::vector<float> samples(96, 0.125f);
    const QString sourcePath =
        QDir(PathUtils::getAudioSourcesDir(projectRoot)).filePath(relativePath);
    writeWavF32(sourcePath, samples, 1, 48000);
    AssetInfo info;
    info.relativePath = relativePath;
    info.absolutePath = sourcePath;
    info.sourceID = AssetRegistry::instance().allocateStableID();
    info.metadata.sampleRate = 48000;
    info.metadata.channels = 1;
    info.metadata.bitDepth = 32;
    info.metadata.totalSamples = 96;
    info.metadata.fileSize = static_cast<uint64_t>(QFileInfo(sourcePath).size());
    EXPECT_TRUE(AssetRegistry::instance().registerAsset(info));
    return info.sourceID;
}

class ActionCatalogExportTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ensureApp();
        qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
        ASSERT_TRUE(tempDir_.isValid());
        ProjectConfig config;
        config.name = QStringLiteral("ActionCatalogExport");
        config.location = tempDir_.path();
        ASSERT_TRUE(manager.createProject(config));
        projectPath_ = manager.currentProject()->projectPath;
        projectRoot_ = QFileInfo(projectPath_).absolutePath();
        AssetRegistry::instance().clear();
        AssetRegistry::instance().setProjectRoot(projectRoot_);
        UndoRedoManager::instance().clear();
        action_ = manager.createItem(
            nullptr, QStringLiteral("Play_Fire"), ProjectItem::Type::Action);
        ASSERT_NE(action_, nullptr);
    }

    void TearDown() override
    {
        UndoRedoManager::instance().clear();
        ProjectManager::instance().closeProject();
        AssetRegistry::instance().clear();
    }

    bool seedPackableAction()
    {
        auto& manager = ProjectManager::instance();
        ProjectData* project = manager.currentProject();
        if (!project || project->busses.empty() || !project->busses.front().rootItem)
            return false;
        const uint32_t sourceId = registerToneAsset(projectRoot_, QStringLiteral("tone.wav"));
        AudioSourceData sourceData;
        sourceData.sourceID = sourceId;
        sourceData.relativePath = QStringLiteral("tone.wav");
        project->audioSources.push_back(sourceData);

        auto layer = std::make_shared<ProjectItem>();
        layer->name = QStringLiteral("Root");
        layer->type = ProjectItem::Type::Layer;
        layer->objectID = 10010;
        layer->guid = VNX::authoringUuid(harkentest::StudioObjectId(200010));
        layer->attachDefaultComponents();
        layer->fieldGroups = MakeRootFieldGroups(
            toFieldGroupGuid(project->busses.front().rootItem->guid));
        auto sound = std::make_shared<ProjectItem>();
        sound->name = QStringLiteral("Tone");
        sound->type = ProjectItem::Type::Sound;
        sound->objectID = 10001;
        sound->guid = VNX::authoringUuid(harkentest::StudioObjectId(200001));
        sound->attachDefaultComponents();
        HarkenTest::setSharedSoundSource(*sound, sourceId);
        sound->parent = layer.get();
        layer->children.push_back(sound);
        ProjectFileBinding layerBinding;
        layerBinding.rootItem = layer;
        project->layers.push_back(std::move(layerBinding));

        if (!action_ || !action_->comp<VAction>())
            return false;
        VActionStep play;
        play.type = VActionStep::Type::Play;
        play.targetObjectID = sound->objectID;
        action_->comp<VAction>()->steps = {play};
        action_->markDirty();
        soundObjectId_ = sound->objectID;
        return action_->runtimeId() != harkentest::StudioObjectId(0);
    }

    ProjectPackExporter::ExportResult exportPacks(const QString& outputDir)
    {
        ProjectPackExporter exporter;
        ProjectPackExporter::ExportOptions opts;
        opts.platform = ProjectManager::instance().getPackExportCurrentPlatform();
        auto result = exporter.ExportAllPacks(
            *ProjectManager::instance().currentProject(), outputDir, opts);
        if (result.success) {
            QStringList paths;
            for (const auto& artifact : result.artifacts)
                paths.append(QDir(outputDir).relativeFilePath(artifact.outputPath));
            publishedPaths_.insert(outputDir, paths);
        }
        return result;
    }

    QJsonArray publication(const QString& outputDir)
    {
        QJsonArray packs;
        QStringList files;
        QString error;
        auto& manager = ProjectManager::instance();
        EXPECT_TRUE(ReadPackPublication(outputDir, manager.currentProject()->projectId,
            manager.getPackExportCurrentPlatform(), &packs, &files, &error, true,
            publishedPaths_.value(outputDir)))
            << error.toStdString();
        return packs;
    }

    QJsonArray actions(const QString& outputDir)
    {
        QJsonArray result;
        for (const auto& value : publication(outputDir)) {
            const auto row = value.toObject();
            if (row.value("packKind").toInt() == harkenstudiobuild::kPackKindCanonicalContent)
                result.append(row.value("companion"));
        }
        return result;
    }

    QJsonObject packRow(const QString& outputDir, const QString& guid = {})
    {
        for (const auto& value : publication(outputDir)) {
            const auto row = value.toObject();
            if (guid.isEmpty() ? row.value("packKind").toInt() == harkenstudiobuild::kPackKindInit
                : row.value("companion").toObject().value("guid").toString() == guid)
                return row;
        }
        ADD_FAILURE() << "Published pack missing for " << guid.toStdString();
        return {};
    }

    QByteArray packBytes(const QString& outputDir, const QJsonObject& row)
    {
        QFile file(QDir(outputDir).filePath(row.value("path").toString()));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        return file.readAll();
    }

    harkenstudiobuild::PackCanonicalImage parsePack(const QString& path)
    {
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        harkenstudiobuild::PackCanonicalImage image;
        EXPECT_EQ(harkenstudiobuild::kPackOk,
                  harkenstudiobuild::ParsePackCanonicalImage(
                      reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                      static_cast<std::uint64_t>(bytes.size()), &image));
        return image;
    }

    harkenstudiobuild::PackCanonicalImage parseInit(const QString& outputDir)
    {
        return parsePack(QDir(outputDir).filePath(packRow(outputDir).value("path").toString()));
    }

    QTemporaryDir tempDir_;
    QString projectPath_;
    QString projectRoot_;
    QHash<QString, QStringList> publishedPaths_;
    ProjectItem* action_ = nullptr;
    uint32_t soundObjectId_ = 0;
};

TEST_F(ActionCatalogExportTest, RepeatedExportKeepsCompanionsAndExecutableBytes)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(seedPackableAction());
    const QString firstDir = QDir(tempDir_.path()).filePath(QStringLiteral("out-a"));
    const QString secondDir = QDir(tempDir_.path()).filePath(QStringLiteral("out-b"));
    const auto first = exportPacks(firstDir);
    ASSERT_TRUE(first.success) << first.error.toStdString();
    const auto second = exportPacks(secondDir);
    ASSERT_TRUE(second.success) << second.error.toStdString();

    const auto firstActions = actions(firstDir);
    EXPECT_EQ(firstActions, actions(secondDir));
    ASSERT_EQ(firstActions.size(), 1);
    EXPECT_EQ(firstActions.at(0).toObject().value(QStringLiteral("name")).toString(),
              QStringLiteral("Play_Fire"));
    const QString actionGuid = action_->guid.toString(QUuid::WithoutBraces);
    EXPECT_EQ(firstActions.at(0).toObject().value("guid").toString(), actionGuid);
    EXPECT_EQ(packBytes(firstDir, packRow(firstDir)), packBytes(secondDir, packRow(secondDir)));
    EXPECT_EQ(packBytes(firstDir, packRow(firstDir, actionGuid)),
              packBytes(secondDir, packRow(secondDir, actionGuid)));
    EXPECT_FALSE(QFileInfo::exists(QDir(firstDir).filePath(
        QStringLiteral("HarkenPackManifest.json"))));

    const auto image = parseInit(firstDir);
    EXPECT_FALSE(image.hasGameplayCatalog);
    EXPECT_FALSE(image.hasCatalog);
    EXPECT_FALSE(image.hasVariableNameCatalog);
    EXPECT_TRUE(image.hasBusDefinitionSnapshot);
    ASSERT_TRUE(image.hasPartition);
    EXPECT_EQ(image.requirement.schemaVersion, 2u);
    EXPECT_EQ(image.partitionHeader.headerBytes,
              harkenstudiobuild::kCanonicalDefinitionPartitionHeaderV2Bytes);
    const auto& snapshot = image.busDefinitionSnapshotHeader;
    EXPECT_EQ(snapshot.schemaVersion, 2u);
    EXPECT_EQ(snapshot.storageModelVersion, 2u);
    EXPECT_EQ(snapshot.headerBytes, harkenstudiobuild::kPackBusDefinitionSnapshotV2HeaderBytes);
    EXPECT_EQ(snapshot.publicationCapabilityVintage, 4u);
    const auto expected = harkenstudiobuild::SnapshotBusTriple(snapshot);
    EXPECT_TRUE(harkenstudiobuild::CanonicalBusTripleFullyNonzero(expected));
    EXPECT_EQ(snapshot.contentGeneration, snapshot.publishedBusFingerprint);
    EXPECT_EQ(snapshot.publishedBusFingerprint,
              harkenstudiobuild::CanonicalVolumeBusFingerprint(image));
    EXPECT_TRUE(harkenstudiobuild::CanonicalBusTripleEqual(
        harkenstudiobuild::RequirementBusTriple(image.requirement), expected));
    EXPECT_TRUE(harkenstudiobuild::CanonicalBusTripleEqual(
        harkenstudiobuild::PartitionBusTriple(image.partitionHeader), expected));
    for (const auto& edge : image.dependencyEdges) {
        const bool initRequired =
            edge.edgeKind == harkenstudiobuild::kPackDependencyEdgeKindInit &&
            edge.required01 == 1;
        if (initRequired) {
            EXPECT_TRUE(harkenstudiobuild::CanonicalBusTripleEqual(
                harkenstudiobuild::DependencyBusTriple(edge), expected));
        } else {
            EXPECT_EQ(edge.expectedBusContentGeneration, 0u);
            EXPECT_EQ(edge.expectedPublishedBusFingerprint, 0u);
            EXPECT_EQ(edge.expectedCompileTargetProfileFingerprint, 0u);
        }
    }
    const QJsonArray packs = publication(firstDir);
    ASSERT_EQ(packs.size(), 2);
    const QJsonObject contentRow = packRow(firstDir, actionGuid);
    const auto content = parsePack(QDir(firstDir).filePath(
        contentRow.value(QStringLiteral("path")).toString()));
    EXPECT_TRUE(harkenstudiobuild::CanonicalBusTripleEqual(
        harkenstudiobuild::RequirementBusTriple(content.requirement), expected));
    EXPECT_TRUE(harkenstudiobuild::CanonicalBusTripleEqual(
        harkenstudiobuild::PartitionBusTriple(content.partitionHeader), expected));
    const auto initEdge = std::find_if(content.dependencyEdges.begin(),
        content.dependencyEdges.end(), [](const auto& edge) {
            return edge.edgeKind == harkenstudiobuild::kPackDependencyEdgeKindInit &&
                   edge.required01 == 1;
        });
    ASSERT_NE(initEdge, content.dependencyEdges.end());
    EXPECT_EQ(initEdge->prerequisitePackStableId, image.packStableId);
    EXPECT_TRUE(harkenstudiobuild::CanonicalBusTripleEqual(
        harkenstudiobuild::DependencyBusTriple(*initEdge), expected));
    const auto secondImage = parseInit(secondDir);
    EXPECT_EQ(image.requirement.packContentRevision,
              secondImage.requirement.packContentRevision);
    EXPECT_TRUE(harkenstudiobuild::CanonicalBusTripleEqual(
        harkenstudiobuild::SnapshotBusTriple(secondImage.busDefinitionSnapshotHeader), expected));
}

TEST_F(ActionCatalogExportTest, RenameExportsOnlyCurrentName)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(seedPackableAction());
    const QString outputDir = QDir(tempDir_.path()).filePath(QStringLiteral("out-rename"));
    auto result = exportPacks(outputDir);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    const QString guid = action_->guid.toString(QUuid::WithoutBraces);
    const auto initBytes = packBytes(outputDir, packRow(outputDir));
    const auto actionBytes = packBytes(outputDir, packRow(outputDir, guid));
    ASSERT_TRUE(manager.renameItem(action_, QStringLiteral("Play_Storm")));
    ASSERT_TRUE(manager.saveProject());
    result = exportPacks(outputDir);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    const auto current = actions(outputDir);
    ASSERT_EQ(current.size(), 1);
    EXPECT_EQ(current.at(0).toObject().value(QStringLiteral("name")).toString(),
              QStringLiteral("Play_Storm"));
    EXPECT_EQ(current.at(0).toObject().value("guid").toString(), guid);
    EXPECT_EQ(initBytes, packBytes(outputDir, packRow(outputDir)));
    EXPECT_EQ(actionBytes, packBytes(outputDir, packRow(outputDir, guid)));
}

TEST_F(ActionCatalogExportTest, VariableRenamePreservesExecutableIdentity)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(seedPackableAction());
    auto* variable = manager.createItem(nullptr, QStringLiteral("Speed"), ProjectItem::Type::Variable);
    ASSERT_NE(variable, nullptr);
    const QString directory = QDir(tempDir_.path()).filePath("out-variable-names");
    auto result = exportPacks(directory);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    const auto before = parseInit(directory);
    const auto beforeBytes = packBytes(directory, packRow(directory));
    const auto beforeNames = packRow(directory).value("companion").toObject().value("variables").toArray();
    ASSERT_EQ(beforeNames.size(), 1);
    EXPECT_EQ(beforeNames.at(0).toObject().value("name").toString(), "Speed");
    EXPECT_FALSE(before.hasVariableNameCatalog);
    ASSERT_TRUE(manager.renameItem(variable, QStringLiteral("Velocity")));
    result = exportPacks(directory);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    const auto after = parseInit(directory);
    EXPECT_EQ(before.requirement.packContentRevision, after.requirement.packContentRevision);
    EXPECT_EQ(beforeBytes, packBytes(directory, packRow(directory)));
    const auto afterNames = packRow(directory).value("companion").toObject().value("variables").toArray();
    ASSERT_EQ(afterNames.size(), 1);
    EXPECT_EQ(afterNames.at(0).toObject().value("name").toString(), "Velocity");
    EXPECT_EQ(afterNames.at(0).toObject().value("guid"), beforeNames.at(0).toObject().value("guid"));
    ASSERT_TRUE(manager.deleteItem(variable));
    result = exportPacks(directory);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    EXPECT_TRUE(packRow(directory).value("companion").toObject().value("variables").toArray().isEmpty());
    EXPECT_NE(beforeBytes, packBytes(directory, packRow(directory)));
}

TEST_F(ActionCatalogExportTest, DeleteSaveReopenOmitsFormerName)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.deleteItem(action_));
    action_ = nullptr;
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(manager.closeProject());
    ASSERT_TRUE(manager.loadProject(projectPath_));
    const QString outputDir = QDir(tempDir_.path()).filePath(QStringLiteral("out-delete"));
    const auto result = exportPacks(outputDir);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    EXPECT_TRUE(actions(outputDir).isEmpty());
}

TEST_F(ActionCatalogExportTest, DuplicateUnicodeTargetsExportWithDistinctIdentities)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(seedPackableAction());
    auto* sound = manager.findItemByObjectID(soundObjectId_);
    ASSERT_NE(sound, nullptr);
    ASSERT_TRUE(manager.renameItem(sound, QStringLiteral("\u811a\u6b65")));
    auto* copy = manager.duplicateItem(sound);
    ASSERT_NE(copy, nullptr);
    ASSERT_TRUE(manager.renameItem(copy, sound->name));
    ASSERT_NE(manager.createActionTargeting(copy, VActionStep::Type::Play, QStringLiteral("Play_Copy")), nullptr);
    EXPECT_NE(sound->guid, copy->guid);
    const QString outputDir = QDir(tempDir_.path()).filePath(QStringLiteral("out-duplicate-unicode"));
    const auto result = exportPacks(outputDir);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    EXPECT_EQ(actions(outputDir).size(), 2);
}

TEST_F(ActionCatalogExportTest, CloneReusesReleasedNameAndPublishes)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(seedPackableAction());
    auto* former = manager.createItem(nullptr, QStringLiteral("Play_Fire_01"),
                                      ProjectItem::Type::Action);
    ASSERT_NE(former, nullptr);
    ASSERT_TRUE(manager.deleteItem(former));
    auto* clone = manager.duplicateItem(action_);
    ASSERT_NE(clone, nullptr);
    ASSERT_EQ(clone->name, QStringLiteral("Play_Fire_01"));
    const auto result = exportPacks(QDir(tempDir_.path()).filePath("out-takeover"));
    EXPECT_TRUE(result.success) << result.error.toStdString();
}

TEST_F(ActionCatalogExportTest, CloneRenameExportsCurrentIdentity)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(seedPackableAction());
    auto* clone = manager.duplicateItem(action_);
    ASSERT_NE(clone, nullptr);
    const auto guid = clone->guid.toString(QUuid::WithoutBraces);
    ASSERT_TRUE(manager.renameItem(clone, QStringLiteral("Play_Clone")));
    const QString outputDir = QDir(tempDir_.path()).filePath("out-clone-rename");
    const auto result = exportPacks(outputDir);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    const auto names = packRow(outputDir, guid).value("companion").toObject();
    EXPECT_EQ(names.value("guid").toString(), guid);
    EXPECT_EQ(names.value("name").toString(), "Play_Clone");
}

TEST_F(ActionCatalogExportTest, CloneDeleteOmitsFormerIdentity)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(seedPackableAction());
    auto* clone = manager.duplicateItem(action_);
    ASSERT_NE(clone, nullptr);
    const auto guid = clone->guid.toString(QUuid::WithoutBraces);
    ASSERT_TRUE(manager.deleteItem(clone));
    const QString outputDir = QDir(tempDir_.path()).filePath("out-clone-delete");
    const auto result = exportPacks(outputDir);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    const auto names = actions(outputDir);
    EXPECT_TRUE(std::none_of(names.begin(), names.end(),
        [&](const auto& current) { return current.toObject().value("guid").toString() == guid; }));
}

TEST_F(ActionCatalogExportTest, InvalidAuthoredNameCannotPublish)
{
    ASSERT_TRUE(seedPackableAction());
    for (const auto* name : {"Invalid/Name", "Dash-Fire", "1Fire", "Play Fire", "Play.Fire"}) {
        action_->name = QString::fromLatin1(name);
        const auto result = exportPacks(QDir(tempDir_.path()).filePath("out-invalid"));
        EXPECT_FALSE(result.success) << name;
    }
}

TEST_F(ActionCatalogExportTest, KeywordAndPrefixedNamesCanPublish)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(seedPackableAction());
    for (const auto& names : {std::pair{"class", "A_class"}}) {
        ASSERT_TRUE(manager.renameItem(action_, QString::fromLatin1(names.first)));
        auto* other = manager.createItem(nullptr, QString::fromLatin1(names.second),
                                         ProjectItem::Type::Action);
        ASSERT_NE(other, nullptr);
        const auto result = exportPacks(QDir(tempDir_.path()).filePath("out-symbol"));
        EXPECT_TRUE(result.success) << result.error.toStdString();
        ASSERT_TRUE(manager.deleteItem(other));
    }
}

TEST_F(ActionCatalogExportTest, NameReuseAndUndoRedoExportCurrentIdentity)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(seedPackableAction());
    auto* former = manager.createItem(nullptr, QStringLiteral("Play_Old"),
                                      ProjectItem::Type::Action);
    ASSERT_NE(former, nullptr);
    const auto actionGuid = action_->guid.toString(QUuid::WithoutBraces);
    const QString outputDir = QDir(tempDir_.path()).filePath("out-reuse");
    const auto published = exportPacks(outputDir);
    ASSERT_TRUE(published.success) << published.error.toStdString();
    ASSERT_TRUE(manager.deleteItem(former));
    QString error;
    ASSERT_TRUE(manager.renameItem(action_, QStringLiteral("Play_Old"), &error));
    const auto actionBytes = packBytes(outputDir, packRow(outputDir, actionGuid));
    auto check = [&](const QString& currentName) {
        const auto result = exportPacks(outputDir);
        ASSERT_TRUE(result.success) << result.error.toStdString();
        const auto names = actions(outputDir);
        ASSERT_EQ(names.size(), 1);
        EXPECT_EQ(names.at(0).toObject().value("guid").toString(), actionGuid);
        EXPECT_EQ(names.at(0).toObject().value("name").toString(), currentName);
        EXPECT_EQ(actionBytes, packBytes(outputDir, packRow(outputDir, actionGuid)));
    };
    check(QStringLiteral("Play_Old"));
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    check(QStringLiteral("Play_Fire"));
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    check(QStringLiteral("Play_Old"));
}

} // namespace
