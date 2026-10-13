#include <harken/Harken.h>
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/PathUtils.h"
#include "Components/VAction.h"
#include "Components/VSound.h"
#include "PackSchemaGenerated.h"
#include "ProjectManager.h"
#include "ProjectPackExporter.h"
#include "ProjectTypes.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "FieldGroupProjectAdapter.h"
#include "Support/AITestSupport.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "UndoCommands.h"

#include <harken/media/CanonicalBusSnapshot.h>
#include <harken/media/PackFormatV2.h>
#include <harken/media/PackLogicImage.h>

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

QJsonObject readJson(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    return QJsonDocument::fromJson(file.readAll()).object();
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
        project->conversionFailurePolicy = ConversionFailurePolicy::AbortBuild;
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
        opts.platform = ProjectManager::instance().currentProject()->packExportCurrentPlatformId;
        return exporter.ExportAllPacks(
            *ProjectManager::instance().currentProject(), outputDir, opts);
    }

    QJsonObject catalogOf(const QString& outputDir)
    {
        return readJson(QDir(outputDir).filePath(
                            HarkenEditor::Bridge::packManifestFileName()))
            .value(QStringLiteral("gameplayCatalog"))
            .toObject();
    }

    harken::PackCanonicalImage parsePack(const QString& path)
    {
        QFile file(path);
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        harken::PackCanonicalImage image;
        EXPECT_EQ(VN_OK,
                  harken::ParsePackCanonicalImage(
                      reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                      static_cast<std::uint64_t>(bytes.size()), &image));
        return image;
    }

    harken::PackCanonicalImage parseInit(const QString& outputDir)
    {
        return parsePack(QDir(outputDir).filePath(QStringLiteral("Init.bytes")));
    }

    QTemporaryDir tempDir_;
    QString projectPath_;
    QString projectRoot_;
    ProjectItem* action_ = nullptr;
    uint32_t soundObjectId_ = 0;
};

TEST_F(ActionCatalogExportTest, CreateSaveExportTwiceKeepsDigestAndAuthoredName)
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

    const QJsonObject firstCatalog = catalogOf(firstDir);
    const QJsonObject secondCatalog = catalogOf(secondDir);
    for (const auto& directory : {firstDir, secondDir}) {
        EXPECT_EQ(readJson(QDir(directory).filePath(
                               HarkenEditor::Bridge::packManifestFileName()))
                      .value(QStringLiteral("schema_version"))
                      .toInt(),
                  PackSchema::kPackManifestSchemaVersion);
    }
    EXPECT_EQ(firstCatalog.value(QStringLiteral("digest")),
              secondCatalog.value(QStringLiteral("digest")));
    EXPECT_EQ(readJson(QDir(firstDir).filePath(
                           HarkenEditor::Bridge::packManifestFileName()))
                  .value(QStringLiteral("catalogChecksum")),
              readJson(QDir(secondDir).filePath(
                           HarkenEditor::Bridge::packManifestFileName()))
                  .value(QStringLiteral("catalogChecksum")));
    EXPECT_FALSE(firstCatalog.contains(QStringLiteral("publicName")));
    const QJsonArray actions = firstCatalog.value(QStringLiteral("actions")).toArray();
    ASSERT_EQ(actions.size(), 1);
    EXPECT_EQ(actions.at(0).toObject().value(QStringLiteral("name")).toString(),
              QStringLiteral("Play_Fire"));

    const auto image = parseInit(firstDir);
    ASSERT_TRUE(image.hasGameplayCatalog);
    EXPECT_FALSE(image.hasCatalog);
    EXPECT_TRUE(image.hasBusDefinitionSnapshot);
    ASSERT_TRUE(image.hasPartition);
    EXPECT_EQ(image.requirement.schemaVersion, 2u);
    EXPECT_EQ(image.partitionHeader.headerBytes,
              harken::kCanonicalDefinitionPartitionHeaderV2Bytes);
    const auto& snapshot = image.busDefinitionSnapshotHeader;
    EXPECT_EQ(snapshot.schemaVersion, 2u);
    EXPECT_EQ(snapshot.storageModelVersion, 2u);
    EXPECT_EQ(snapshot.headerBytes, harken::kPackBusDefinitionSnapshotV2HeaderBytes);
    EXPECT_EQ(snapshot.publicationCapabilityVintage, 4u);
    const auto expected = harken::SnapshotBusTriple(snapshot);
    EXPECT_TRUE(harken::CanonicalBusTripleFullyNonzero(expected));
    EXPECT_EQ(snapshot.contentGeneration, snapshot.publishedBusFingerprint);
    EXPECT_EQ(snapshot.publishedBusFingerprint,
              harken::CanonicalVolumeBusFingerprint(image));
    EXPECT_TRUE(harken::CanonicalBusTripleEqual(
        harken::RequirementBusTriple(image.requirement), expected));
    EXPECT_TRUE(harken::CanonicalBusTripleEqual(
        harken::PartitionBusTriple(image.partitionHeader), expected));
    for (const auto& edge : image.dependencyEdges) {
        const bool initRequired =
            edge.edgeKind == harken::kPackDependencyEdgeKindInit &&
            edge.required01 == 1;
        if (initRequired) {
            EXPECT_TRUE(harken::CanonicalBusTripleEqual(
                harken::DependencyBusTriple(edge), expected));
        } else {
            EXPECT_EQ(edge.expectedBusContentGeneration, 0u);
            EXPECT_EQ(edge.expectedPublishedBusFingerprint, 0u);
            EXPECT_EQ(edge.expectedCompileTargetProfileFingerprint, 0u);
        }
    }
    const QJsonArray packs = readJson(QDir(firstDir).filePath(
        HarkenEditor::Bridge::packManifestFileName()))
        .value(QStringLiteral("packs")).toArray();
    ASSERT_EQ(packs.size(), 2);
    const QJsonObject contentRow = packs.at(0).toObject()
        .value(QStringLiteral("name")).toString() == QStringLiteral("Init")
        ? packs.at(1).toObject() : packs.at(0).toObject();
    ASSERT_NE(contentRow.value(QStringLiteral("name")).toString(),
              QStringLiteral("Init"));
    const auto content = parsePack(QDir(firstDir).filePath(
        contentRow.value(QStringLiteral("path")).toString()));
    EXPECT_TRUE(harken::CanonicalBusTripleEqual(
        harken::RequirementBusTriple(content.requirement), expected));
    EXPECT_TRUE(harken::CanonicalBusTripleEqual(
        harken::PartitionBusTriple(content.partitionHeader), expected));
    const auto initEdge = std::find_if(content.dependencyEdges.begin(),
        content.dependencyEdges.end(), [](const auto& edge) {
            return edge.edgeKind == harken::kPackDependencyEdgeKindInit &&
                   edge.required01 == 1;
        });
    ASSERT_NE(initEdge, content.dependencyEdges.end());
    EXPECT_EQ(initEdge->prerequisitePackStableId, image.packStableId);
    EXPECT_TRUE(harken::CanonicalBusTripleEqual(
        harken::DependencyBusTriple(*initEdge), expected));
    const auto secondImage = parseInit(secondDir);
    EXPECT_EQ(image.requirement.packContentRevision,
              secondImage.requirement.packContentRevision);
    EXPECT_TRUE(harken::CanonicalBusTripleEqual(
        harken::SnapshotBusTriple(secondImage.busDefinitionSnapshotHeader), expected));
    EXPECT_EQ(QString::number(image.gameplayCatalog.header.digest),
              firstCatalog.value(QStringLiteral("digest")).toString());
}

TEST_F(ActionCatalogExportTest, RenameExportsOnlyCurrentName)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(manager.renameItem(action_, QStringLiteral("Play_Storm")));
    ASSERT_TRUE(manager.saveProject());
    ASSERT_TRUE(seedPackableAction());
    const QString outputDir = QDir(tempDir_.path()).filePath(QStringLiteral("out-rename"));
    const auto result = exportPacks(outputDir);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    const QJsonObject catalog = catalogOf(outputDir);
    const QJsonArray actions = catalog.value(QStringLiteral("actions")).toArray();
    const QJsonArray retired = catalog.value(QStringLiteral("retired")).toArray();
    ASSERT_EQ(actions.size(), 1);
    EXPECT_EQ(actions.at(0).toObject().value(QStringLiteral("name")).toString(),
              QStringLiteral("Play_Storm"));
    EXPECT_TRUE(retired.isEmpty());
    EXPECT_TRUE(parseInit(outputDir).gameplayCatalog.retired.empty());
}

TEST_F(ActionCatalogExportTest, CurrentVariableNamesChangeInitContentRevision)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(seedPackableAction());
    auto* variable = manager.createItem(nullptr, QStringLiteral("Speed"), ProjectItem::Type::Variable);
    ASSERT_NE(variable, nullptr);
    const QString directory = QDir(tempDir_.path()).filePath("out-variable-names");
    auto result = exportPacks(directory);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    const auto before = parseInit(directory);
    ASSERT_TRUE(before.hasVariableNameCatalog);
    ASSERT_EQ(before.variableNameCatalog.rows.size(), 1u);
    EXPECT_EQ(before.variableNameCatalog.rows.front().name, "Speed");
    ASSERT_TRUE(manager.renameItem(variable, QStringLiteral("Velocity")));
    result = exportPacks(directory);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    const auto after = parseInit(directory);
    EXPECT_NE(before.requirement.packContentRevision, after.requirement.packContentRevision);
    EXPECT_EQ(after.variableNameCatalog.initContentRevision, after.requirement.packContentRevision);
    ASSERT_EQ(after.variableNameCatalog.rows.size(), 1u);
    EXPECT_EQ(after.variableNameCatalog.rows.front().name, "Velocity");
    EXPECT_EQ(after.variableNameCatalog.rows.front().variableId, before.variableNameCatalog.rows.front().variableId);
    ASSERT_TRUE(manager.deleteItem(variable));
    result = exportPacks(directory);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    const auto deleted = parseInit(directory);
    EXPECT_TRUE(deleted.variableNameCatalog.rows.empty());
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
    const QJsonObject catalog = catalogOf(outputDir);
    EXPECT_TRUE(catalog.value(QStringLiteral("actions")).toArray().isEmpty());
    const QJsonArray retired = catalog.value(QStringLiteral("retired")).toArray();
    EXPECT_TRUE(retired.isEmpty());
    EXPECT_TRUE(parseInit(outputDir).gameplayCatalog.retired.empty());
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
    EXPECT_EQ(catalogOf(outputDir).value(QStringLiteral("actions")).toArray().size(), 2);
}

TEST_F(ActionCatalogExportTest, CloneAvoidsAnotherActionsFormerNameAndPublishes)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(seedPackableAction());
    auto* former = manager.createItem(nullptr, QStringLiteral("Play_Fire_01"),
                                      ProjectItem::Type::Action);
    ASSERT_NE(former, nullptr);
    ASSERT_TRUE(manager.deleteItem(former));
    auto* clone = manager.duplicateItem(action_);
    ASSERT_NE(clone, nullptr);
    ASSERT_EQ(clone->name, QStringLiteral("Play_Fire_02"));
    const auto result = exportPacks(QDir(tempDir_.path()).filePath("out-takeover"));
    EXPECT_TRUE(result.success) << result.error.toStdString();
}

TEST_F(ActionCatalogExportTest, CloneRenameExportsCurrentIdentity)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(seedPackableAction());
    auto* clone = manager.duplicateItem(action_);
    ASSERT_NE(clone, nullptr);
    const auto id = clone->runtimeId();
    ASSERT_TRUE(manager.renameItem(clone, QStringLiteral("Play_Clone")));
    const QString outputDir = QDir(tempDir_.path()).filePath("out-clone-rename");
    const auto result = exportPacks(outputDir);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    const auto image = parseInit(outputDir);
    EXPECT_TRUE(image.gameplayCatalog.retired.empty());
    const auto row = std::find_if(image.gameplayCatalog.active.begin(), image.gameplayCatalog.active.end(),
        [&](const auto& current) { return current.actionStableId == id; });
    ASSERT_NE(row, image.gameplayCatalog.active.end());
    EXPECT_EQ(row->nameBindingRevision, 2u);
    EXPECT_TRUE(catalogOf(outputDir).value("retired").toArray().isEmpty());
}

TEST_F(ActionCatalogExportTest, CloneDeleteOmitsFormerIdentity)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(seedPackableAction());
    auto* clone = manager.duplicateItem(action_);
    ASSERT_NE(clone, nullptr);
    const auto id = clone->runtimeId();
    ASSERT_TRUE(manager.deleteItem(clone));
    const QString outputDir = QDir(tempDir_.path()).filePath("out-clone-delete");
    const auto result = exportPacks(outputDir);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    const auto image = parseInit(outputDir);
    EXPECT_TRUE(image.gameplayCatalog.retired.empty());
    EXPECT_TRUE(std::none_of(image.gameplayCatalog.active.begin(), image.gameplayCatalog.active.end(),
        [&](const auto& current) { return current.actionStableId == id; }));
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

TEST_F(ActionCatalogExportTest, ExplicitReuseAndUndoRedoExportOnlyCurrentName)
{
    auto& manager = ProjectManager::instance();
    ASSERT_TRUE(seedPackableAction());
    auto* former = manager.createItem(nullptr, QStringLiteral("Play_Old"),
                                      ProjectItem::Type::Action);
    ASSERT_NE(former, nullptr);
    ASSERT_TRUE(manager.deleteItem(former));
    QString error;
    ASSERT_TRUE(manager.reusePublishedActionName(action_, QStringLiteral("Play_Old"), &error));
    const QString outputDir = QDir(tempDir_.path()).filePath("out-reuse");
    auto check = [&](const QString& currentName, uint64_t revision) {
        const auto result = exportPacks(outputDir);
        ASSERT_TRUE(result.success) << result.error.toStdString();
        const auto image = parseInit(outputDir);
        EXPECT_TRUE(image.gameplayCatalog.retired.empty());
        ASSERT_EQ(image.gameplayCatalog.active.size(), 1u);
        EXPECT_EQ(image.gameplayCatalog.active.front().nameBindingRevision, revision);
        EXPECT_EQ(catalogOf(outputDir).value("actions").toArray().at(0).toObject()
                      .value("name").toString(), currentName);
    };
    check(QStringLiteral("Play_Old"), 2);
    ASSERT_TRUE(UndoRedoManager::instance().undo());
    check(QStringLiteral("Play_Fire"), 3);
    ASSERT_TRUE(UndoRedoManager::instance().redo());
    check(QStringLiteral("Play_Old"), 4);
}

} // namespace
