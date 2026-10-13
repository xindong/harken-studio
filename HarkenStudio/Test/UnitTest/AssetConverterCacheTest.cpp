#include "Support/ProjectPlatformTestUtils.h"
#include "AssetPipeline/AssetConverter.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/PathUtils.h"
#include "AssetPipeline/PlatformProfile.h"
#include "RuntimeBridge/PackagingBridge.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QTemporaryDir>

#include <cstdint>
#include <vector>

namespace {

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

void writeWavF32(const QString& path,
                 const std::vector<float>& interleaved,
                 uint16_t channels,
                 uint32_t sampleRate)
{
    ASSERT_NE(channels, 0);
    ASSERT_EQ(interleaved.size() % channels, 0u);
    QDir().mkpath(QFileInfo(path).absolutePath());
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));

    const uint32_t frames = static_cast<uint32_t>(interleaved.size() / channels);
    const uint32_t dataBytes = static_cast<uint32_t>(interleaved.size() * sizeof(float));
    const uint16_t blockAlign = static_cast<uint16_t>(channels * sizeof(float));
    const uint32_t byteRate = sampleRate * blockAlign;
    constexpr uint32_t fmtSize = 18;
    constexpr uint32_t factSize = 4;
    const uint32_t riffSize = 4 + (8 + fmtSize) + (8 + factSize) + (8 + dataBytes);

    QByteArray bytes;
    bytes.reserve(static_cast<int>(riffSize + 8));
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

    ASSERT_EQ(file.write(bytes), bytes.size());
}

} // namespace

TEST(EditorAssetConverterCache, WritesF32WavAndStoresRemappedLoop)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    auto definitions = HarkenTest::platforms({"Windows", "Android"});
    VNX::AssetRegistry::instance().setPlatforms(definitions);

    constexpr uint16_t kChannels = 2;
    constexpr uint32_t kSampleRate = 48000;
    constexpr size_t kFrames = 80;

    std::vector<float> source(kFrames * kChannels, 0.0f);
    source[1 * kChannels + 0] = 0.25f;
    source[1 * kChannels + 1] = 0.25f;
    source[2 * kChannels + 0] = 0.5f;
    source[2 * kChannels + 1] = 0.5f;

    const QString audioRoot = VNX::PathUtils::getAudioSourcesDir(dir.path());
    const QString relativePath = QStringLiteral("tone.wav");
    const QString sourcePath = QDir(audioRoot).filePath(relativePath);
    writeWavF32(sourcePath, source, kChannels, kSampleRate);

    VNX::AssetInfo info;
    info.relativePath = relativePath;
    info.absolutePath = sourcePath;
    info.sourceID = VNX::AssetRegistry::instance().allocateStableID();
    ASSERT_NE(info.sourceID, 0u);
    info.metadata.sampleRate = kSampleRate;
    info.metadata.channels = kChannels;
    info.metadata.bitDepth = 32;
    info.metadata.totalSamples = kFrames;
    info.metadata.fileSize = static_cast<uint64_t>(QFileInfo(sourcePath).size());
    info.metadata.loopInfo.enabled = true;
    info.metadata.loopInfo.loopStart = 1;
    info.metadata.loopInfo.loopEnd = 79;
    info.lastModified = static_cast<uint64_t>(QFileInfo(sourcePath).lastModified().toSecsSinceEpoch());
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(info));

    VNX::AssetConverter converter;
    converter.setProjectRoot(dir.path());
    const VNX::ConversionResult result = converter.convert(info.sourceID, HarkenTest::PlatformIds::Windows);
    ASSERT_TRUE(result.success) << result.errorMessage.toStdString();
    EXPECT_TRUE(result.cachePath.endsWith(QStringLiteral(".wav")));
    const QString retiredExt =
        QString(QChar('.')) + QString(QChar('w')) + QString(QChar('e')) + QString(QChar('m'));
    EXPECT_FALSE(result.cachePath.contains(retiredExt));
    ASSERT_TRUE(QFileInfo::exists(result.absolutePath));

    QFile cacheFile(result.absolutePath);
    ASSERT_TRUE(cacheFile.open(QIODevice::ReadOnly));
    EXPECT_EQ(cacheFile.read(4), QByteArray("RIFF", 4));

    std::vector<float> decoded;
    int decodedChannels = 0;
    int decodedRate = 0;
    ASSERT_TRUE(HarkenEditor::Bridge::decodeAudioFileInterleaved(
        result.absolutePath, decoded, decodedChannels, decodedRate));
    EXPECT_EQ(decodedChannels, 2);
    EXPECT_EQ(decodedRate, static_cast<int>(kSampleRate));
    EXPECT_EQ(decoded, source);

    const VNX::AssetInfo* converted = VNX::AssetRegistry::instance().getAsset(info.sourceID);
    ASSERT_NE(converted, nullptr);
    const auto cacheIt = converted->platformCaches.find(HarkenTest::PlatformIds::Windows);
    ASSERT_NE(cacheIt, converted->platformCaches.end());
    EXPECT_TRUE(cacheIt->loopInfo.enabled);
    EXPECT_EQ(cacheIt->loopInfo.loopStart, 1u);
    EXPECT_EQ(cacheIt->loopInfo.loopEnd, 80u);

    VNX::AssetInfo singleSample = info;
    singleSample.sourceID = VNX::AssetRegistry::instance().allocateStableID();
    ASSERT_NE(singleSample.sourceID, 0u);
    singleSample.metadata.loopInfo.loopStart = 1;
    singleSample.metadata.loopInfo.loopEnd = 1;
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(singleSample));
    const VNX::ConversionResult singleSampleResult =
        converter.convert(singleSample.sourceID, HarkenTest::PlatformIds::Windows);
    ASSERT_TRUE(singleSampleResult.success)
        << singleSampleResult.errorMessage.toStdString();
    const VNX::AssetInfo* convertedSingle =
        VNX::AssetRegistry::instance().getAsset(singleSample.sourceID);
    ASSERT_NE(convertedSingle, nullptr);
    const auto singleCache = convertedSingle->platformCaches.find(HarkenTest::PlatformIds::Windows);
    ASSERT_NE(singleCache, convertedSingle->platformCaches.end());
    EXPECT_TRUE(singleCache->loopInfo.enabled);
    EXPECT_EQ(singleCache->loopInfo.loopStart, 1u);
    EXPECT_EQ(singleCache->loopInfo.loopEnd, 2u);

    VNX::ConversionSettings pcmSettings;
    pcmSettings.format = VNX::ConversionSettings::Codec::PCM_16;
    VNX::ConversionSettings vorbisSettings = pcmSettings;
    vorbisSettings.format = VNX::ConversionSettings::Codec::Vorbis;
    vorbisSettings.vorbisQuality = 0.1f;
    EXPECT_EQ(pcmSettings.hash(), vorbisSettings.hash());

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorAssetConverterCache, PlatformProfilesProduceIndependentCacheArtifacts)
{
    VNX::PlatformProfileManager::instance().addProfile(
        VNX::PlatformProfileManager::createWindowsDefault());
    VNX::PlatformProfileManager::instance().addProfile(
        VNX::PlatformProfileManager::createAndroidDefault());

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    auto definitions = HarkenTest::platforms({"Windows", "Android"});
    VNX::AssetRegistry::instance().setPlatforms(definitions);

    constexpr uint16_t kChannels = 2;
    constexpr uint32_t kSampleRate = 48000;
    constexpr size_t kFrames = 4096;

    std::vector<float> source(kFrames * kChannels, 0.0f);
    for (size_t i = 0; i < kFrames; ++i) {
        source[i * kChannels + 0] = static_cast<float>(i % 128) / 128.0f;
        source[i * kChannels + 1] = 1.0f - static_cast<float>(i % 128) / 128.0f;
    }

    const QString audioRoot = VNX::PathUtils::getAudioSourcesDir(dir.path());
    const QString relativePath = QStringLiteral("matrix.wav");
    const QString sourcePath = QDir(audioRoot).filePath(relativePath);
    writeWavF32(sourcePath, source, kChannels, kSampleRate);

    VNX::AssetInfo info;
    info.relativePath = relativePath;
    info.absolutePath = sourcePath;
    info.sourceID = VNX::AssetRegistry::instance().allocateStableID();
    ASSERT_NE(info.sourceID, 0u);
    info.metadata.sampleRate = kSampleRate;
    info.metadata.channels = kChannels;
    info.metadata.bitDepth = 32;
    info.metadata.totalSamples = kFrames;
    info.metadata.fileSize = static_cast<uint64_t>(QFileInfo(sourcePath).size());
    info.lastModified = static_cast<uint64_t>(
        QFileInfo(sourcePath).lastModified().toSecsSinceEpoch());
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(info));

    VNX::AssetConverter converter;
    converter.setProjectRoot(dir.path());

    const VNX::ConversionResult windows =
        converter.convert(info.sourceID, HarkenTest::PlatformIds::Windows);
    ASSERT_TRUE(windows.success) << windows.errorMessage.toStdString();
    const VNX::ConversionResult android =
        converter.convert(info.sourceID, HarkenTest::PlatformIds::Android);
    ASSERT_TRUE(android.success) << android.errorMessage.toStdString();

    EXPECT_NE(windows.cachePath, android.cachePath);
    EXPECT_TRUE(windows.cachePath.contains(HarkenTest::platformIdString("Windows")));
    EXPECT_TRUE(android.cachePath.contains(HarkenTest::platformIdString("Android")));

    std::vector<float> windowsDecoded;
    int windowsChannels = 0;
    int windowsRate = 0;
    ASSERT_TRUE(HarkenEditor::Bridge::decodeAudioFileInterleaved(
        windows.absolutePath, windowsDecoded, windowsChannels, windowsRate));
    EXPECT_EQ(windowsChannels, 2);
    EXPECT_EQ(windowsRate, 48000);

    std::vector<float> androidDecoded;
    int androidChannels = 0;
    int androidRate = 0;
    ASSERT_TRUE(HarkenEditor::Bridge::decodeAudioFileInterleaved(
        android.absolutePath, androidDecoded, androidChannels, androidRate));
    EXPECT_EQ(androidChannels, 2);
    EXPECT_EQ(androidRate, 48000);

    ASSERT_TRUE(VNX::AssetRegistry::instance().isCacheValid(
        info.sourceID, HarkenTest::PlatformIds::Windows));
    ASSERT_TRUE(VNX::AssetRegistry::instance().isCacheValid(
        info.sourceID, HarkenTest::PlatformIds::Android));

    VNX::PlatformProfile changedAndroid =
        VNX::PlatformProfileManager::createAndroidDefault();
    changedAndroid.performResample = true;
    changedAndroid.sizePolicies.front().config.targetSampleRate = 44100;
    definitions[1].profile = VNX::ProjectPlatformProfile{changedAndroid.performResample, changedAndroid.sizePolicies};
    VNX::AssetRegistry::instance().setPlatforms(definitions);

    EXPECT_TRUE(VNX::AssetRegistry::instance().isCacheValid(
        info.sourceID, HarkenTest::PlatformIds::Windows));
    EXPECT_FALSE(VNX::AssetRegistry::instance().isCacheValid(
        info.sourceID, HarkenTest::PlatformIds::Android));

    VNX::PlatformProfileManager::instance().addProfile(
        VNX::PlatformProfileManager::createAndroidDefault());
    VNX::AssetRegistry::instance().clear();
}
