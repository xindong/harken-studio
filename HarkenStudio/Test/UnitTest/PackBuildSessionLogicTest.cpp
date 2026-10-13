#include "UnitTest/StudioObjectIdTestSupport.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "harken/Harken.h"
#include "harken/media/PackFormatV2.h"
#include "harken/media/PackLogicImage.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFileInfo>
#include <QFile>
#include <QTemporaryDir>

#include <cmath>
#include <cstdint>
#include <vector>

#ifndef HARKEN_PACKC_PATH
#error "HARKEN_PACKC_PATH must point at the packc executable"
#endif

namespace {

using namespace HarkenEditor;

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

void writeWavF32(const QString& path, const std::vector<float>& samples)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));

    constexpr uint32_t sampleRate = 48000;
    constexpr uint16_t channels = 1;
    const uint32_t frames = static_cast<uint32_t>(samples.size());
    const uint32_t dataBytes = frames * static_cast<uint32_t>(sizeof(float));
    const uint16_t blockAlign = channels * sizeof(float);
    const uint32_t fmtSize = 18;
    const uint32_t factSize = 4;
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
    appendU32(bytes, sampleRate * blockAlign);
    appendU16(bytes, blockAlign);
    appendU16(bytes, 32);
    appendU16(bytes, 0);
    bytes.append("fact", 4);
    appendU32(bytes, factSize);
    appendU32(bytes, frames);
    bytes.append("data", 4);
    appendU32(bytes, dataBytes);
    bytes.append(reinterpret_cast<const char*>(samples.data()),
                 static_cast<qsizetype>(dataBytes));
    ASSERT_EQ(file.write(bytes), bytes.size());
}

VnResult loadPack(const QString& path, VnPackHandle* outHandle)
{
    VnPackDesc desc{};
    desc.structSize = sizeof(VnPackDesc);
    const QByteArray utf8 = QFileInfo(path).absoluteFilePath().toUtf8();
    desc.utf8Path = utf8.constData();
    return Vn_LoadPack(&desc, outHandle);
}

constexpr uint64_t kRootBusId = 501;
constexpr uint64_t kAuxBusId = 502;
constexpr uint64_t kAttenuationId = 601;
constexpr uint64_t kMediaId = 7101;

void buildResidentPack(const QString& path)
{
    Bridge::PackBuildSession builder;
    builder.setInitPack(true);

    Bridge::PackBus root;
    root.id = harkentest::StudioObjectId(kRootBusId);
    root.parentId = harkentest::StudioObjectId(kRootBusId);
    root.name = QStringLiteral("RootBus");
    builder.addBus(root);

    Bridge::PackBus aux;
    aux.id = harkentest::StudioObjectId(kAuxBusId);
    aux.parentId = harkentest::StudioObjectId(kRootBusId);
    aux.name = QStringLiteral("AuxBus");
    builder.addBus(aux);

    Bridge::PackBusSend send;
    send.fromBusId = harkentest::StudioObjectId(kAuxBusId);
    send.toBusId = harkentest::StudioObjectId(kRootBusId);
    send.levelDb = -6.0f;
    builder.addBusSend(send);

    Bridge::PackAttenuation att;
    att.id = harkentest::StudioObjectId(kAttenuationId);
    att.maxDistance = 50.0f;
    Bridge::PackAttenuation::Slot volumeSlot;
    volumeSlot.slot = 0;
    volumeSlot.points.push_back({0.0f, 0.0f});
    volumeSlot.points.push_back({50.0f, -60.0f});
    volumeSlot.handles.push_back({0.0f, 0.0f, 50.0f, -60.0f});
    att.curveSlots.push_back(volumeSlot);
    Bridge::PackAttenuation::Slot spreadSlot;
    spreadSlot.slot = 6;
    spreadSlot.points.push_back({0.0f, 100.0f});
    spreadSlot.points.push_back({50.0f, 0.0f});
    spreadSlot.handles.push_back({0.0f, 100.0f, 50.0f, 0.0f});
    att.curveSlots.push_back(spreadSlot);
    builder.addAttenuation(att);

    ASSERT_TRUE(builder.buildToFile(path)) << builder.lastError().toStdString();
    EXPECT_TRUE(builder.warnings().isEmpty()) << builder.warnings().join('\n').toStdString();
}

void buildContentPack(const QString& path, const QString& wavPath)
{
    Bridge::PackBuildSession builder;

    Bridge::PackMediaDesc media;
    media.sourceId = harkentest::StudioObjectId(kMediaId);
    media.authoringSourceId = 11;
    media.sourcePath = wavPath;
    builder.addMedia(media);

    ASSERT_TRUE(builder.buildToFile(path)) << builder.lastError().toStdString();

    EXPECT_TRUE(builder.warnings().isEmpty())
        << builder.warnings().join('\n').toStdString();
}

void expectCurrentDirectoryVintages(const QString& path)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    const QByteArray bytes = file.readAll();
    ASSERT_GE(bytes.size(), static_cast<qsizetype>(harken::kPackV2HeaderBytes));
    const auto* data = reinterpret_cast<const std::uint8_t*>(bytes.constData());
    harken::PackV2Header header;
    ASSERT_TRUE(harken::ParsePackV2Header(data, &header));
    EXPECT_EQ(harken::kPackFormatVersion2, header.formatVersion);
    int voiceRecipes = 0;
    int attenuations = 0;
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const harken::PackV2DirectoryEntry entry = harken::ParsePackV2DirectoryEntry(
            data + header.directoryOffset + i * harken::kPackV2DirectoryEntryBytes);
        if (entry.kind == harken::kPackEntryKindVoiceRecipe) {
            ++voiceRecipes;
        } else if (entry.kind == harken::kPackEntryKindAttenuation) {
            ++attenuations;
            EXPECT_EQ(harken::kPackV2AuthoredToneSchemaVintage, entry.vintage)
                << path.toStdString();
            EXPECT_NE(harken::kPackV2SchemaVintage, entry.vintage);
        }
    }
    EXPECT_EQ(0, voiceRecipes) << path.toStdString();
    EXPECT_GT(attenuations, 0) << path.toStdString();
}

} // namespace

TEST(EditorPackBuildSessionLogic, ResidentThenContentRoundtrip)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    std::vector<float> samples(512);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = std::sin(static_cast<float>(i) * 0.05f) * 0.25f;
    const QString wavPath = QDir(dir.path()).filePath(QStringLiteral("tone.wav"));
    writeWavF32(wavPath, samples);

    const QString residentPath = QDir(dir.path()).filePath(QStringLiteral("init.pack"));
    const QString contentPath = QDir(dir.path()).filePath(QStringLiteral("content.pack"));
    ASSERT_NO_FATAL_FAILURE(buildResidentPack(residentPath));
    ASSERT_NO_FATAL_FAILURE(buildContentPack(contentPath, wavPath));
    ASSERT_NO_FATAL_FAILURE(expectCurrentDirectoryVintages(residentPath));

    QFile contentFile(contentPath);
    ASSERT_TRUE(contentFile.open(QIODevice::ReadOnly));
    const QByteArray contentBytes = contentFile.readAll();
    harken::PackLogicImage contentImage;
    ASSERT_EQ(VN_OK,
              harken::ParsePackV2Image(
                  reinterpret_cast<const std::uint8_t*>(
                      contentBytes.constData()),
                  static_cast<std::uint64_t>(contentBytes.size()),
                  &contentImage));
    EXPECT_TRUE(contentImage.voiceRecipes.empty());

    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(VN_OK, Vn_Init(&config));

    VnPackHandle contentFirst = 0;
    ASSERT_EQ(VN_OK, loadPack(contentPath, &contentFirst))
        << "media pack loads independently of playback definitions";
    EXPECT_EQ(VN_OK, Vn_UnloadPack(contentFirst));

    VnPackHandle resident = 0;
    ASSERT_EQ(VN_OK, loadPack(residentPath, &resident)) << "resident pack";
    VnPackHandle content = 0;
    ASSERT_EQ(VN_OK, loadPack(contentPath, &content)) << "content pack";

    EXPECT_EQ(VN_OK, Vn_Update());

    EXPECT_EQ(VN_OK, Vn_Term());
}

TEST(EditorPackBuildSessionLogic, MediaPackPreservesLoopRegionWithoutPlaybackDefinitions)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    std::vector<float> samples(256);
    for (size_t i = 0; i < samples.size(); ++i)
        samples[i] = std::sin(static_cast<float>(i) * 0.05f) * 0.25f;
    const QString wavPath = QDir(dir.path()).filePath(QStringLiteral("loop.wav"));
    writeWavF32(wavPath, samples);

    const QString contentPath =
        QDir(dir.path()).filePath(QStringLiteral("passes.pack"));

    Bridge::PackBuildSession builder;

    Bridge::PackMediaDesc media;
    media.sourceId = harkentest::StudioObjectId(kMediaId);
    media.authoringSourceId = 11;
    media.sourcePath = wavPath;
    media.loopable = true;
    media.loopStartFrame = 32;
    media.loopEndFrame = 224;
    builder.addMedia(media);

    ASSERT_TRUE(builder.buildToFile(contentPath))
        << builder.lastError().toStdString();

    QFile contentFile(contentPath);
    ASSERT_TRUE(contentFile.open(QIODevice::ReadOnly));
    const QByteArray contentBytes = contentFile.readAll();
    const auto* data =
        reinterpret_cast<const std::uint8_t*>(contentBytes.constData());
    harken::PackV2Header header;
    ASSERT_TRUE(harken::ParsePackV2Header(data, &header));
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const harken::PackV2DirectoryEntry entry = harken::ParsePackV2DirectoryEntry(
            data + header.directoryOffset + i * harken::kPackV2DirectoryEntryBytes);
        EXPECT_NE(harken::kPackEntryKindContainer, entry.kind);
        EXPECT_NE(harken::kPackEntryKindVoiceRecipe, entry.kind);
    }

    harken::PackLogicImage contentImage;
    ASSERT_EQ(VN_OK,
              harken::ParsePackV2Image(
                  data, static_cast<std::uint64_t>(contentBytes.size()),
                  &contentImage));
    ASSERT_EQ(contentImage.media.size(), 1u);
    EXPECT_EQ(contentImage.media.front().soundId, harkentest::ObjectId(kMediaId));
    EXPECT_EQ(contentImage.media.front().frameCount, samples.size());
    EXPECT_NE(contentImage.media.front().flags & harken::kPackFlagLoopable, 0u);
    EXPECT_EQ(contentImage.media.front().loopStartFrame, 32u);
    EXPECT_EQ(contentImage.media.front().loopEndFrame, 224u);
    EXPECT_TRUE(contentImage.containers.empty());
    EXPECT_TRUE(contentImage.voiceRecipes.empty());
}
