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

#include <cstdint>
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

    QTemporaryDir tempDir_;
    QString projectPath_;
    QString projectRoot_;
    QHash<QString, QStringList> publishedPaths_;
    ProjectItem* action_ = nullptr;
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
    const QString actionGuid = action_->guid.toString(QUuid::WithoutBraces);
    EXPECT_EQ(packBytes(firstDir, packRow(firstDir)), packBytes(secondDir, packRow(secondDir)));
    EXPECT_EQ(packBytes(firstDir, packRow(firstDir, actionGuid)),
              packBytes(secondDir, packRow(secondDir, actionGuid)));
}

} // namespace
