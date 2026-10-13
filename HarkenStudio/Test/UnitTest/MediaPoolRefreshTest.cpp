#include "AssetPipeline/AssetImporter.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/AssetTypes.h"
#include "AssetPipeline/MediaPool.h"
#include "AssetPipeline/PathUtils.h"

#include <gtest/gtest.h>

#include <QDir>
#include <QFile>
#include <QTemporaryDir>

#include <vector>

namespace {

void appendU16(QByteArray& out, uint16_t value)
{
    out.append(static_cast<char>(value & 0xffu));
    out.append(static_cast<char>((value >> 8) & 0xffu));
}

void appendU32(QByteArray& out, uint32_t value)
{
    out.append(static_cast<char>(value & 0xffu));
    out.append(static_cast<char>((value >> 8) & 0xffu));
    out.append(static_cast<char>((value >> 16) & 0xffu));
    out.append(static_cast<char>((value >> 24) & 0xffu));
}

void writeWav(const QString& path, uint32_t frames, float value)
{
    const std::vector<float> samples(frames, value);
    constexpr uint16_t channels = 1;
    constexpr uint32_t sampleRate = 48000;
    constexpr uint16_t blockAlign = channels * sizeof(float);
    constexpr uint32_t fmtSize = 18;
    constexpr uint32_t factSize = 4;
    const uint32_t dataBytes = static_cast<uint32_t>(samples.size() * sizeof(float));
    const uint32_t riffSize = 4 + (8 + fmtSize) + (8 + factSize) + (8 + dataBytes);
    QByteArray bytes;
    bytes.append("RIFF", 4);
    appendU32(bytes, riffSize);
    bytes.append("WAVEfmt ", 8);
    appendU32(bytes, fmtSize);
    appendU16(bytes, 3);
    appendU16(bytes, channels);
    appendU32(bytes, sampleRate);
    appendU32(bytes, sampleRate * blockAlign);
    appendU16(bytes, blockAlign);
    appendU16(bytes, 32);
    appendU16(bytes, 0);
    bytes.append("fact", 4);
    appendU32(bytes, factSize);
    appendU32(bytes, frames);
    bytes.append("data", 4);
    appendU32(bytes, dataBytes);
    bytes.append(reinterpret_cast<const char*>(samples.data()), dataBytes);
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(bytes), bytes.size());
}

}

TEST(MediaPoolRefreshTest, RefreshesMetadataAndHashForAnExternallyChangedSource)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const QString audioRoot = VNX::PathUtils::getAudioSourcesDir(dir.path());
    ASSERT_TRUE(QDir().mkpath(audioRoot));
    const QString path = QDir(audioRoot).filePath(QStringLiteral("external.wav"));
    writeWav(path, 480, 0.1f);

    VNX::AssetImporter importer;
    importer.setProjectRoot(dir.path());
    VNX::AssetInfo info;
    info.sourceID = 8601;
    info.relativePath = QStringLiteral("external.wav");
    info.absolutePath = path;
    info.contentHash = VNX::AssetImporter::computeContentHash(path);
    info.metadata = importer.analyzeFile(path);
    info.lastModified = QFileInfo(path).lastModified().toSecsSinceEpoch();
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(info));
    const QString oldHash = info.contentHash;

    int modified = 0;
    const QMetaObject::Connection connection = QObject::connect(
        &VNX::AssetRegistry::instance(), &VNX::AssetRegistry::assetModified,
        [&modified](uint32_t sourceID) {
            if (sourceID == 8601) ++modified;
        });

    writeWav(path, 240, 0.2f);
    ASSERT_TRUE(VNX::MediaPool::instance().refreshSourceFile(info.sourceID, path));
    const VNX::AssetInfo* refreshed =
        VNX::AssetRegistry::instance().getAsset(info.sourceID);
    ASSERT_NE(refreshed, nullptr);
    EXPECT_EQ(refreshed->metadata.totalSamples, 240u);
    EXPECT_NE(refreshed->contentHash, oldHash);
    EXPECT_EQ(modified, 1);

    ASSERT_TRUE(VNX::MediaPool::instance().refreshSourceFile(info.sourceID, path));
    EXPECT_EQ(modified, 1);

    QObject::disconnect(connection);
    VNX::AssetRegistry::instance().clear();
}
