#include "Engine.h"
#include "Core/AssetPipeline/AssetConverter.h"
#include "Core/AssetPipeline/AssetImporter.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AudioEngine.h"
#include "Core/Components/VSound.h"
#include "Core/ProjectManager.h"
#include "RuntimeBridge/PlaybackBridge.h"
#include "Widgets/SourceEditor/SourceEditorWidget.h"
#include "harken/Harken.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace HarkenEditor::Bridge;

namespace {

bool ensureFreshNullEngine()
{
    auto& engine = VNX::AudioEngine::instance();
    if (engine.isInitialized())
        engine.shutdown();
    EngineSessionConfig config;
    config.sampleRate = 48000;
    config.bufferSize = 256;
    config.deviceMode = EngineDeviceMode::Null;
    return engine.initialize(config) && !engine.isSimulationMode();
}

class SourceEditorPcmSliceTest : public ::testing::Test
{
protected:
    void SetUp() override
    {
        ASSERT_TRUE(directory_.isValid());
        ASSERT_TRUE(ensureFreshNullEngine());
        engine_ = &VNX::AudioEngine::instance();
    }

    void TearDown() override
    {
        engine_ = nullptr;
        VNX::AudioEngine::instance().shutdown();
    }

    QTemporaryDir directory_;
    VNX::AudioEngine* engine_ = nullptr;
};

}

TEST_F(SourceEditorPcmSliceTest, MintsIndependentSoundUuids)
{
    const harkenstudio::ObjectId first = engine_->mintRawPcmSoundId();
    const harkenstudio::ObjectId second = engine_->mintRawPcmSoundId();
    EXPECT_FALSE(harkenstudio::IsNull(first));
    EXPECT_FALSE(harkenstudio::IsNull(second));
    EXPECT_NE(first, second);
}

TEST_F(SourceEditorPcmSliceTest, RejectsSlicesOutsideDecodedBuffer)
{
    const QString path = QDir(directory_.path()).filePath(QStringLiteral("reject.wav"));
    ASSERT_TRUE(VNX::AssetConverter::writeFloatWav(path, std::vector<float>(8, 0.25f), 1, 48000));

    const harkenstudio::ObjectId soundId = engine_->mintRawPcmSoundId();
    ASSERT_EQ(engine_->registerRawPcmSlice(soundId, path, 0, 8).result, kEngineOk);
    ASSERT_EQ(engine_->unregisterRawPcm(soundId), kEngineOk);
    EXPECT_NE(engine_->registerRawPcmSlice(soundId, path, 8, 1).result, kEngineOk);
    EXPECT_NE(engine_->registerRawPcmSlice(soundId, path, 6, 4).result, kEngineOk);
    EXPECT_NE(engine_->unregisterRawPcm(soundId), kEngineOk);
}

TEST_F(SourceEditorPcmSliceTest, MemoryAndFilePcmRenderIdenticallyAfterRelocation)
{
    std::vector<float> samples;
    for (int frame = 0; frame < 2048; ++frame) {
        samples.push_back(static_cast<float>(frame) / 8192.0f);
        samples.push_back(-static_cast<float>(frame) / 16384.0f);
    }
    const QString path = directory_.filePath(QStringLiteral("prepared.wav"));
    ASSERT_TRUE(VNX::AssetConverter::writeFloatWav(path, samples, 2, 48000));

    const auto capture = [&](bool memory, bool loops, std::vector<float>& output) {
        ASSERT_TRUE(ensureFreshNullEngine());
        const auto soundId = engine_->mintRawPcmSoundId();
        const auto registered = memory
            ? engine_->registerRawPcmSlice(soundId, samples, 2, 48000, 512, 1024, loops)
            : engine_->registerRawPcmSlice(soundId, path, 512, 1024, loops);
        ASSERT_EQ(registered.result, kEngineOk);
        ASSERT_EQ(engine_->relocateRawPcmSlice(soundId, 1024).result, kEngineOk);
        ASSERT_EQ(engine_->relocateRawPcmSlice(soundId, 512).result, kEngineOk);
        ASSERT_EQ(engine_->relocateRawPcmSlice(soundId, 1024).result, kEngineOk);
        ASSERT_NE(engine_->playRawPcm(soundId, loops), 0u);
        auto* runtime = harken::detail::ActiveEngine();
        ASSERT_NE(runtime, nullptr);
        for (int block = 0; block < 8; ++block) {
            ASSERT_EQ(Vn_Update(), VN_OK);
            runtime->nullBackend.Pump(1);
            const auto* begin = runtime->nullBackend.LastBlock();
            output.insert(output.end(), begin,
                begin + runtime->nullBackend.BlockFrames() * runtime->nullBackend.Channels());
        }
    };

    for (const bool loops : {false, true}) {
        std::vector<float> fileOutput;
        std::vector<float> memoryOutput;
        ASSERT_NO_FATAL_FAILURE(capture(false, loops, fileOutput));
        ASSERT_NO_FATAL_FAILURE(capture(true, loops, memoryOutput));
        EXPECT_TRUE(std::any_of(fileOutput.begin(), fileOutput.end(),
            [](float value) { return std::abs(value) > 0.00001f; }));
        EXPECT_EQ(memoryOutput, fileOutput);
    }
}

TEST_F(SourceEditorPcmSliceTest, PreparedSourceRestartsImmediatelyAfterRuntimeReset)
{
    auto& projects = VNX::ProjectManager::instance();
    VNX::ProjectConfig config;
    config.name = QStringLiteral("SourcePreviewLifecycle");
    config.location = directory_.path();
    ASSERT_TRUE(projects.createProject(config));
    const auto closeProject = qScopeGuard([&] { EXPECT_TRUE(projects.closeProject()); });
    const QString path = QFileInfo(projects.currentProject()->projectPath).dir()
        .filePath(QStringLiteral("prepared.wav"));
    ASSERT_TRUE(VNX::AssetConverter::writeFloatWav(
        path, std::vector<float>(4096, 0.25f), 1, 48000));

    auto& assets = VNX::AssetRegistry::instance();
    VNX::AssetInfo asset;
    asset.sourceID = assets.allocateStableID();
    asset.absolutePath = path;
    asset.relativePath = QStringLiteral("prepared.wav");
    asset.contentHash = VNX::AssetImporter::computeContentHash(path);
    asset.metadata = VNX::AssetImporter().analyzeFile(path);
    ASSERT_TRUE(assets.registerAsset(asset));
    auto* layer = projects.createItem(nullptr, QStringLiteral("Layer"), VNX::ProjectItem::Type::Layer);
    ASSERT_NE(layer, nullptr);
    auto* item = projects.createItem(layer, QStringLiteral("Sound"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(item, nullptr);
    auto* sound = item->comp<VNX::VSound>();
    ASSERT_NE(sound, nullptr);
    VNX::SoundSourceDef source;
    source.entryId = QUuid::createUuid();
    source.sourceId = asset.sourceID;
    source.rangeContentHash = asset.contentHash;
    source.fadeIn.durationFrames = 1024;
    sound->sources.plain.entries = {source};
    sound->sources.plain.active = source.entryId;

    VNX::SourceEditorWidget editor;
    editor.bindSource(asset.sourceID, item->guid, source.entryId);
    ASSERT_TRUE(editor.startPreviewPlayhead(512));
    engine_->clearRuntimeObjects();
    EXPECT_TRUE(editor.startPreviewPlayhead(768));
    engine_->stopOutput();
    EXPECT_TRUE(editor.startPreviewPlayhead(1024));
}
