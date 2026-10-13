#include "UnitTest/StudioObjectIdTestSupport.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "UnitTest/RawVoiceTestSupport.h"

#include "harken/Harken.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>

#include <cmath>
#include <cstdint>
#include <vector>

#ifndef HARKEN_PACKC_PATH
#error "HARKEN_PACKC_PATH must point at the packc executable"
#endif

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
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));

    const uint32_t frames = static_cast<uint32_t>(interleaved.size() / channels);
    const uint32_t dataBytes = static_cast<uint32_t>(interleaved.size() * sizeof(float));
    const uint16_t blockAlign = static_cast<uint16_t>(channels * sizeof(float));
    const uint32_t byteRate = sampleRate * blockAlign;
    const uint32_t fmtSize = 18;
    const uint32_t factSize = 4;
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

TEST(EditorPackBuildSession, BuildsPackcPackAndRuntimeLoadsIt)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    constexpr uint64_t kSoundId = 7001001;
    constexpr uint32_t kSampleRate = 48000;
    std::vector<float> samples(512);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = std::sin(static_cast<float>(i) * 0.05f) * 0.25f;

    const QString wavPath = QDir(dir.path()).filePath(QStringLiteral("tone.wav"));
    writeWavF32(wavPath, samples, 1, kSampleRate);

    const QString packPath = QDir(dir.path()).filePath(QStringLiteral("editor-built.pack"));
    HarkenEditor::Bridge::PackBuildSession builder;
    HarkenEditor::Bridge::PackMediaDesc media;
    media.sourceId = harkentest::StudioObjectId(kSoundId);
    media.authoringSourceId = 11;
    media.sourcePath = wavPath;
    builder.addMedia(media);

    ASSERT_TRUE(builder.buildToFile(packPath)) << builder.lastError().toStdString();
    ASSERT_TRUE(QFileInfo::exists(packPath));
    ASSERT_TRUE(QFileInfo::exists(packPath + QStringLiteral(".report.json")));

    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(VN_OK, Vn_Init(&config));

    VnPackDesc desc{};
    desc.structSize = sizeof(VnPackDesc);
    const QByteArray utf8 = QFileInfo(packPath).absoluteFilePath().toUtf8();
    desc.utf8Path = utf8.constData();
    VnPackHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_LoadPack(&desc, &handle));
    ASSERT_NE(handle, 0u);

    VnPlayDesc play{};
    play.structSize = sizeof(VnPlayDesc);
    play.soundId = harkentest::ObjectId(kSoundId);
    play.busId = VN_BUS_MASTER;
    play.loopCount = 1;
    VnVoiceHandle voice = 0;
    EXPECT_EQ(VN_OK, harkentest::PlayRaw(&play, &voice));
    EXPECT_NE(voice, 0u);

    EXPECT_EQ(VN_OK, Vn_Stop(voice, 0));
    EXPECT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(VN_OK, Vn_Term());
}

TEST(EditorPackBuildSession, EmitsMonoDownmixPolicy)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    const QString wavPath = QDir(dir.path()).filePath(QStringLiteral("stereo.wav"));
    const std::vector<float> samples = {
        0.5f, 0.25f,
        -0.25f, 0.125f,
        0.75f, -0.25f
    };
    writeWavF32(wavPath, samples, 2, 48000);

    const QString packPath = QDir(dir.path()).filePath(QStringLiteral("mono.pack"));
    HarkenEditor::Bridge::PackBuildSession builder;
    HarkenEditor::Bridge::PackMediaDesc media;
    media.sourceId = harkentest::StudioObjectId(7001002);
    media.authoringSourceId = 12;
    media.sourcePath = wavPath;
    media.downmixToMono = true;
    builder.addMedia(media);

    ASSERT_TRUE(builder.buildToFile(packPath)) << builder.lastError().toStdString();

    QFile reportFile(packPath + QStringLiteral(".report.json"));
    ASSERT_TRUE(reportFile.open(QIODevice::ReadOnly | QIODevice::Text));
    const QJsonObject report = QJsonDocument::fromJson(reportFile.readAll()).object();
    const QJsonObject pack = report[QStringLiteral("packs")].toArray()[0].toObject();
    const QJsonObject entry = pack[QStringLiteral("entries")].toArray()[0].toObject();
    EXPECT_EQ(entry[QStringLiteral("channels")].toInt(), 1);

    QFile cacheFile(packPath + QStringLiteral(".cache.json"));
    ASSERT_TRUE(cacheFile.open(QIODevice::ReadOnly | QIODevice::Text));
    const QJsonObject cache = QJsonDocument::fromJson(cacheFile.readAll()).object();
    const QJsonObject source = cache[QStringLiteral("sources")].toArray()[0].toObject();
    EXPECT_EQ(source[QStringLiteral("downmix")].toString(), QStringLiteral("mono"));
    EXPECT_EQ(source[QStringLiteral("sourceChannelCount")].toInt(), 2);
    EXPECT_EQ(source[QStringLiteral("channelCount")].toInt(), 1);
}
