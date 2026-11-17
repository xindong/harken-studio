#include "Core/AssetPipeline/AssetConverter.h"
#include "Core/AssetPipeline/AssetImporter.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/AudioEngine.h"
#include "Core/Components/VSound.h"
#include "Core/Components/VConversion.h"
#include "Core/PlatformContextService.h"
#include "Core/PreviewMediaModeService.h"
#include "Core/ProjectManager.h"
#include "RuntimeBridge/PlaybackBridge.h"
#include "Widgets/SourceEditor/SourceEditorWidget.h"
#include "harken/Harken.h"
#include "harken/HarkenDevelopment.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QCoreApplication>
#include <QEvent>
#include <QFileInfo>
#include <QScopeGuard>
#include <QTemporaryDir>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace HarkenEditor::Bridge;

namespace {

std::vector<float> takeMasterChannelPeaks()
{
    VnPreviewChannelPeaksV1 peaks{};
    peaks.structSize = sizeof(peaks);
    std::vector<float> values;
    if (VnPreview_TakeChannelPeaks(&peaks) != VN_OK)
        return values;
    for (uint32_t bus = 0; bus < peaks.busCount; ++bus) {
        if (peaks.busIds[bus] != VN_BUS_MASTER)
            continue;
        values.assign(peaks.channelPeaks + bus * VN_MAX_CHANNELS,
            peaks.channelPeaks + bus * VN_MAX_CHANNELS + peaks.mixChannelCount);
    }
    return values;
}

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
        ASSERT_EQ(VnPreview_EnableChannelPeakMetering(), VN_OK);
        takeMasterChannelPeaks();
        for (int block = 0; block < 8; ++block) {
            ASSERT_EQ(Vn_Update(), VN_OK);
            VnPreview_PumpNullMixer(1);
            const auto peaks = takeMasterChannelPeaks();
            output.insert(output.end(), peaks.begin(), peaks.end());
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

TEST_F(SourceEditorPcmSliceTest, ConvertedSourceUsesMediaCoordinatesAndKeepsLibraryAuditionIndependent)
{
    auto& projects = VNX::ProjectManager::instance();
    VNX::ProjectConfig config;
    config.name = QStringLiteral("ConvertedSourceCoordinates");
    config.location = directory_.path();
    ASSERT_TRUE(projects.createProject(config));
    const auto closeProject = qScopeGuard([&] { EXPECT_TRUE(projects.closeProject()); });
    auto* project = projects.currentProject();
    ASSERT_GE(project->platforms.size(), 2);
    const auto firstPlatform = project->platforms[0].id;
    const auto secondPlatform = project->platforms[1].id;
    VNX::PlatformContextService::instance().setSelected({firstPlatform}, firstPlatform);
    auto& modes = VNX::PreviewMediaModeService::instance();
    const auto previousMode = modes.mode();
    const auto restoreMode = qScopeGuard([&] { modes.setMode(previousMode); });
    modes.setMode(VNX::PreviewMediaMode::Converted);
    auto* preset = projects.findItemByGuid(project->defaultConversionRef);
    ASSERT_NE(preset, nullptr);
    auto* settings = preset->comp<VNX::VConversion>();
    ASSERT_NE(settings, nullptr);
    for (auto& platform : settings->platforms) {
        platform.format = VNX::ConversionFormat::PCM;
        platform.tiers = {{std::nullopt, 22050}};
    }
    const QString path = QFileInfo(project->projectPath).dir().filePath(QStringLiteral("source.wav"));
    std::vector<float> samples(32768);
    for (size_t frame = 0; frame < samples.size(); ++frame)
        samples[frame] = static_cast<float>(0.2 * std::sin(6.283185307179586 * 371.0 * frame / 48000.0));
    ASSERT_TRUE(VNX::AssetConverter::writeFloatWav(path, samples, 1, 48000));
    auto& assets = VNX::AssetRegistry::instance();
    VNX::AssetInfo asset;
    asset.sourceID = assets.allocateStableID();
    asset.absolutePath = path;
    asset.relativePath = QStringLiteral("source.wav");
    asset.contentHash = VNX::AssetImporter::computeContentHash(path);
    asset.metadata = VNX::AssetImporter().analyzeFile(path);
    asset.metadata.loopInfo.enabled = true;
    asset.metadata.loopInfo.loopStart = 0;
    asset.metadata.loopInfo.loopEnd = samples.size() - 1;
    ASSERT_TRUE(assets.registerAsset(asset));
    auto* item = projects.createItem(nullptr, QStringLiteral("Sound"), VNX::ProjectItem::Type::Sound);
    ASSERT_NE(item, nullptr);
    auto* sound = item->comp<VNX::VSound>();
    ASSERT_NE(sound, nullptr);
    VNX::SoundSourceDef source;
    source.entryId = QUuid::createUuid();
    source.sourceId = asset.sourceID;
    source.rangeContentHash = asset.contentHash;
    source.trimStartFrame = 1003;
    source.trimEndFrame = 28117;
    source.loopRegion = VNX::SoundLoopRegion{4105, 25121};
    source.fadeIn.durationFrames = 2048;
    source.fadeOut.durationFrames = 4096;
    sound->sources.plain.entries = {source};
    sound->sources.plain.active = source.entryId;
    VNX::SourceEditorWidget editor;
    VNX::SourceEditorWidget library;
    editor.bindSource(asset.sourceID, item->guid, source.entryId);
    library.bindSource(asset.sourceID);
    ASSERT_TRUE(library.startPreviewPlayhead());
    const auto libraryHandle = library.previewPlaybackHandle();
    const auto mapFrame = [&](uint64_t frame, uint32_t rate) {
        return static_cast<uint64_t>(std::llround(
            static_cast<long double>(frame - source.trimStartFrame) * rate / 48000));
    };
    for (const auto format : {VNX::ConversionFormat::PCM, VNX::ConversionFormat::Vorbis}) {
        SCOPED_TRACE(static_cast<int>(format));
        settings->platformConfig(firstPlatform)->format = format;
        settings->platformConfig(firstPlatform)->tiers = {{std::nullopt, 22050}};
        emit projects.itemPropertyChanged(preset, QStringLiteral("platforms"));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        for (const uint64_t start : {4105ull, 12007ull, 25120ull}) {
            SCOPED_TRACE(start);
            ASSERT_TRUE(editor.startPreviewPlayhead(start));
            const auto windowStart = mapFrame(source.loopRegion->startFrame, 22050);
            const auto windowEnd = mapFrame(source.loopRegion->endFrame, 22050);
            const auto mediaStart = std::clamp(mapFrame(start, 22050), windowStart, windowEnd - 1);
            const auto registered = engine_->relocateRawPcmSlice(editor.previewSoundId(), mediaStart);
            ASSERT_EQ(registered.result, kEngineOk);
            EXPECT_EQ(registered.sampleRate, 22050u);
            EXPECT_EQ(registered.windowStartFrame, windowStart);
            EXPECT_EQ(registered.windowFrameCount, windowEnd - windowStart);
            EXPECT_TRUE(registered.loops);
            const auto playhead = editor.previewPlayheadFrame();
            ASSERT_TRUE(playhead);
            EXPECT_GE(*playhead, source.trimStartFrame);
            EXPECT_LT(*playhead, *source.trimEndFrame);
            ASSERT_EQ(Vn_Update(), VN_OK);
            VnPreview_PumpNullMixer(1);
            ASSERT_EQ(Vn_Update(), VN_OK);
        }
        settings->platformConfig(secondPlatform)->tiers = {{std::nullopt, 16000}};
        emit projects.itemPropertyChanged(preset, QStringLiteral("platforms"));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        EXPECT_TRUE(editor.isPreviewPlayheadActive());
        settings->platformConfig(firstPlatform)->tiers = {{std::nullopt, 24000}};
        emit projects.itemPropertyChanged(preset, QStringLiteral("platforms"));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        EXPECT_FALSE(editor.isPreviewPlayheadActive());
        ASSERT_TRUE(editor.startPreviewPlayhead());
        sound->sources.plain.entries.front().fadeIn.durationFrames += 128;
        emit projects.itemPropertyChanged(item, QStringLiteral("sources"));
        QCoreApplication::sendPostedEvents(nullptr, QEvent::MetaCall);
        EXPECT_FALSE(editor.isPreviewPlayheadActive());
        ASSERT_TRUE(editor.startPreviewPlayhead());
        modes.setMode(VNX::PreviewMediaMode::Original);
        EXPECT_FALSE(editor.isPreviewPlayheadActive());
        ASSERT_TRUE(editor.startPreviewPlayhead());
        const auto original = engine_->relocateRawPcmSlice(editor.previewSoundId(), 12007);
        ASSERT_EQ(original.result, kEngineOk);
        EXPECT_EQ(original.sampleRate, asset.metadata.sampleRate);
        EXPECT_EQ(original.windowStartFrame, source.loopRegion->startFrame);
        modes.setMode(VNX::PreviewMediaMode::Converted);
        ASSERT_TRUE(editor.startPreviewPlayhead());
        VNX::PlatformContextService::instance().setSelected({secondPlatform}, secondPlatform);
        EXPECT_FALSE(editor.isPreviewPlayheadActive());
        ASSERT_TRUE(editor.startPreviewPlayhead());
        const auto second = engine_->relocateRawPcmSlice(editor.previewSoundId(),
            mapFrame(source.loopRegion->startFrame, 16000));
        ASSERT_EQ(second.result, kEngineOk);
        EXPECT_EQ(second.sampleRate, 16000u);
        VNX::PlatformContextService::instance().setSelected({firstPlatform}, firstPlatform);
        EXPECT_TRUE(library.isPreviewPlayheadActive());
        EXPECT_EQ(library.previewPlaybackHandle(), libraryHandle);
    }
}
