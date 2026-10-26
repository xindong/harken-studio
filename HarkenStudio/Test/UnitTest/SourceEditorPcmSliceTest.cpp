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
