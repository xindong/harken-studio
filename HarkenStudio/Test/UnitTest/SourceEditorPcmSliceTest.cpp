#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Core/AssetPipeline/AssetConverter.h"
#include "Core/AudioEngine.h"
#include "RuntimeBridge/PlaybackBridge.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QTemporaryDir>

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

TEST_F(SourceEditorPcmSliceTest, RegistersAFrameAlignedDecodedSlice)
{
    const QString path = QDir(directory_.path()).filePath(QStringLiteral("slice.wav"));
    std::vector<float> samples(16);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<float>(i + 1);
    ASSERT_TRUE(VNX::AssetConverter::writeFloatWav(path, samples, 2, 48000));

    const harkenstudio::ObjectId soundId = engine_->mintRawPcmSoundId();
    ASSERT_FALSE(harkenstudio::IsNull(soundId));
    const auto registered = engine_->registerRawPcmSlice(soundId, path, 3, 4);
    EXPECT_EQ(registered.result, kEngineOk);
    EXPECT_EQ(registered.soundId, soundId);
    EXPECT_EQ(registered.channels, 2u);
    EXPECT_EQ(registered.sampleRate, 48000u);
    EXPECT_EQ(registered.startFrame, 3u);
    EXPECT_EQ(registered.frameCount, 4u);
    EXPECT_EQ(registered.windowStartFrame, 3u);
    EXPECT_EQ(registered.windowFrameCount, 4u);
    EXPECT_FALSE(registered.loops);

    const auto replaced = engine_->registerRawPcmSlice(soundId, path, 1, 2);
    EXPECT_EQ(replaced.result, kEngineOk);
    EXPECT_EQ(replaced.startFrame, 1u);
    EXPECT_EQ(replaced.frameCount, 2u);
    EXPECT_FALSE(replaced.loops);
    EXPECT_EQ(engine_->unregisterRawPcm(soundId), kEngineOk);
    EXPECT_EQ(engine_->unregisterRawPcm(soundId), kEngineUnknownId);
}

TEST_F(SourceEditorPcmSliceTest, RejectsEmptyAndOutOfRangeSlices)
{
    const QString path = QDir(directory_.path()).filePath(QStringLiteral("reject.wav"));
    ASSERT_TRUE(VNX::AssetConverter::writeFloatWav(path, std::vector<float>(8, 0.25f), 1, 48000));

    const harkenstudio::ObjectId soundId = engine_->mintRawPcmSoundId();
    EXPECT_EQ(engine_->registerRawPcmSlice(harkentest::StudioObjectId(0), path, 0, 4).result, kEngineInvalidArg);
    EXPECT_EQ(engine_->registerRawPcmSlice(soundId, path, 0, 0).result, kEngineInvalidArg);
    EXPECT_EQ(engine_->registerRawPcmSlice(soundId, path, 8, 1).result, kEngineInvalidArg);
    EXPECT_EQ(engine_->registerRawPcmSlice(soundId, path, 6, 4).result, kEngineInvalidArg);
    EXPECT_EQ(engine_->unregisterRawPcm(soundId), kEngineUnknownId);
}

TEST_F(SourceEditorPcmSliceTest, RegistersExplicitUuidAndRequiresRegistrationForRemoval)
{
    const QString path = QDir(directory_.path()).filePath(QStringLiteral("authoring.wav"));
    ASSERT_TRUE(VNX::AssetConverter::writeFloatWav(path, std::vector<float>(8, 0.25f), 1, 48000));

    constexpr harkenstudio::ObjectId kAuthoringId = harkentest::StudioObjectId(41);
    EXPECT_EQ(engine_->unregisterRawPcm(kAuthoringId), kEngineUnknownId);
    EXPECT_EQ(engine_->registerRawPcmSlice(kAuthoringId, path, 0, 4).result, kEngineOk);
    EXPECT_EQ(engine_->unregisterRawPcm(kAuthoringId), kEngineOk);
    EXPECT_EQ(engine_->unregisterRawPcm(harkentest::StudioObjectId(0)), kEngineInvalidArg);
}

TEST_F(SourceEditorPcmSliceTest, RelocatesARegisteredSliceFromALaterFrame)
{
    const QString path = QDir(directory_.path()).filePath(QStringLiteral("locate.wav"));
    std::vector<float> samples(16);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<float>(i + 1);
    ASSERT_TRUE(VNX::AssetConverter::writeFloatWav(path, samples, 2, 48000));

    const harkenstudio::ObjectId soundId = engine_->mintRawPcmSoundId();
    const auto registered = engine_->registerRawPcmSlice(soundId, path, 2, 5);
    ASSERT_EQ(registered.result, kEngineOk);
    EXPECT_EQ(registered.startFrame, 2u);
    EXPECT_EQ(registered.frameCount, 5u);

    const auto relocated = engine_->relocateRawPcmSlice(soundId, 4);
    EXPECT_EQ(relocated.result, kEngineOk);
    EXPECT_EQ(relocated.soundId, soundId);
    EXPECT_EQ(relocated.startFrame, 4u);
    EXPECT_EQ(relocated.frameCount, 3u);
    EXPECT_EQ(relocated.windowStartFrame, 2u);
    EXPECT_EQ(relocated.windowFrameCount, 5u);
    EXPECT_FALSE(relocated.loops);
    EXPECT_EQ(relocated.channels, 2u);
    EXPECT_EQ(relocated.sampleRate, 48000u);

    const auto same = engine_->relocateRawPcmSlice(soundId, 4);
    EXPECT_EQ(same.result, kEngineOk);
    EXPECT_EQ(same.startFrame, 4u);
    EXPECT_EQ(same.frameCount, 3u);

    EXPECT_EQ(engine_->relocateRawPcmSlice(soundId, 1).result, kEngineInvalidArg);
    EXPECT_EQ(engine_->relocateRawPcmSlice(soundId, 7).result, kEngineInvalidArg);
    EXPECT_EQ(engine_->unregisterRawPcm(soundId), kEngineOk);
}

TEST_F(SourceEditorPcmSliceTest, RelocateRequiresARegisteredOwnedSlice)
{
    const QString path = QDir(directory_.path()).filePath(QStringLiteral("missing.wav"));
    ASSERT_TRUE(VNX::AssetConverter::writeFloatWav(path, std::vector<float>(8, 0.25f), 1, 48000));

    EXPECT_EQ(engine_->relocateRawPcmSlice(harkentest::StudioObjectId(0), 0).result, kEngineInvalidArg);
    EXPECT_EQ(engine_->relocateRawPcmSlice(harkentest::StudioObjectId(41), 0).result, kEngineUnknownId);
    const harkenstudio::ObjectId unused = engine_->mintRawPcmSoundId();
    EXPECT_FALSE(harkenstudio::IsNull(unused));
    EXPECT_EQ(engine_->relocateRawPcmSlice(unused, 0).result, kEngineUnknownId);
}

TEST_F(SourceEditorPcmSliceTest, RegistersALoopWindowAndRelocatesByRotation)
{
    const QString path = QDir(directory_.path()).filePath(QStringLiteral("loop.wav"));
    std::vector<float> samples(16);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = static_cast<float>(i + 1);
    ASSERT_TRUE(VNX::AssetConverter::writeFloatWav(path, samples, 2, 48000));

    const harkenstudio::ObjectId soundId = engine_->mintRawPcmSoundId();
    const auto registered = engine_->registerRawPcmSlice(soundId, path, 2, 4, true);
    ASSERT_EQ(registered.result, kEngineOk);
    EXPECT_EQ(registered.startFrame, 2u);
    EXPECT_EQ(registered.frameCount, 4u);
    EXPECT_EQ(registered.windowStartFrame, 2u);
    EXPECT_EQ(registered.windowFrameCount, 4u);
    EXPECT_TRUE(registered.loops);

    const auto relocated = engine_->relocateRawPcmSlice(soundId, 4);
    EXPECT_EQ(relocated.result, kEngineOk);
    EXPECT_EQ(relocated.startFrame, 4u);
    EXPECT_EQ(relocated.frameCount, 4u);
    EXPECT_EQ(relocated.windowStartFrame, 2u);
    EXPECT_EQ(relocated.windowFrameCount, 4u);
    EXPECT_TRUE(relocated.loops);

    const auto same = engine_->relocateRawPcmSlice(soundId, 4);
    EXPECT_EQ(same.result, kEngineOk);
    EXPECT_EQ(same.startFrame, 4u);
    EXPECT_EQ(same.frameCount, 4u);
    EXPECT_TRUE(same.loops);

    EXPECT_EQ(engine_->relocateRawPcmSlice(soundId, 1).result, kEngineInvalidArg);
    EXPECT_EQ(engine_->relocateRawPcmSlice(soundId, 6).result, kEngineInvalidArg);

    const uint64_t playback = engine_->playRawPcm(soundId, true);
    EXPECT_NE(playback, 0u);
    EXPECT_TRUE(engine_->stopRawPcm(playback, 0));
    EXPECT_FALSE(engine_->stopRawPcm(playback, 0));
}
