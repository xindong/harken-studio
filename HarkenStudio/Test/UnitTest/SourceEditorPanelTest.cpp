#include "Core/AssetPipeline/WaveformCache.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QFile>
#include <QTemporaryDir>

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

void writeWavF32(const QString& path, const std::vector<float>& interleaved,
                 uint16_t channels, uint32_t sampleRate)
{
    ASSERT_NE(channels, 0);
    ASSERT_EQ(interleaved.size() % channels, 0u);
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

}

TEST(SourceEditorPanelTest, WaveformDetailReadsOnlyRequestedFramesAndPreservesChannels)
{
    QTemporaryDir temp;
    ASSERT_TRUE(temp.isValid());
    const QString path = temp.filePath(QStringLiteral("detail.wav"));
    std::vector<float> samples;
    for (int frame = 0; frame < 20; ++frame) {
        samples.push_back(float(frame) / 20);
        samples.push_back(-float(frame) / 20);
        samples.push_back(0.125f);
    }
    writeWavF32(path, samples, 3, 48000);
    auto detail = VNX::WaveformCache::readRange(path, 7, 9, 3);
    ASSERT_TRUE(detail.isValid());
    EXPECT_EQ(detail.numChannels, 3u);
    EXPECT_EQ(detail.numPeaks, 3u);
    EXPECT_EQ(detail.samplesPerPeak, 3u);
    EXPECT_FLOAT_EQ(detail.peaks[0], 7.0f / 20);
    EXPECT_FLOAT_EQ(detail.peaks[1], 9.0f / 20);
    EXPECT_FLOAT_EQ(detail.peaks[6], -9.0f / 20);
    EXPECT_FLOAT_EQ(detail.peaks[7], -7.0f / 20);
    EXPECT_FLOAT_EQ(detail.peaks[12], 0.125f);
    detail = VNX::WaveformCache::readRange(path, 17, 20, 10);
    ASSERT_TRUE(detail.isValid());
    EXPECT_EQ(detail.numPeaks, 3u);
    EXPECT_EQ(detail.samplesPerPeak, 1u);
    EXPECT_FLOAT_EQ(detail.peaks[4], 19.0f / 20);
    EXPECT_FLOAT_EQ(detail.peaks[5], 19.0f / 20);
    detail = VNX::WaveformCache::readRange(path, 0, 20, 6);
    ASSERT_TRUE(detail.isValid());
    EXPECT_LE(detail.numPeaks, 6u);
    EXPECT_FLOAT_EQ(detail.peaks[(detail.numPeaks - 1) * 2 + 1], 19.0f / 20);
    EXPECT_FALSE(VNX::WaveformCache::readRange(path, 20, 1, 1).isValid());
    EXPECT_FALSE(VNX::WaveformCache::readRange(path, 0, 0, 1).isValid());
    EXPECT_FALSE(VNX::WaveformCache::readRange(path, 0, 1, 0).isValid());
}
