#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/PathUtils.h"
#include "Components/VAction.h"
#include "ProjectPackExporter.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "RuntimeBridge/ProjectSyncBridge.h"

#include "harken/media/PackFormat.h"
#include "harken/media/PackFormatV2.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QTemporaryDir>
#include <QUuid>

#include <cstdint>
#include <memory>
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

QJsonObject readJsonObject(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly | QIODevice::Text)) << path.toStdString();
    return QJsonDocument::fromJson(file.readAll()).object();
}

harken::PackTocEntry readSingleMediaToc(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    const QByteArray bytes = file.readAll();
    const auto* data = reinterpret_cast<const uint8_t*>(bytes.constData());
    EXPECT_GE(bytes.size(), static_cast<qsizetype>(harken::kPackV2HeaderBytes));

    harken::PackV2Header header;
    EXPECT_TRUE(harken::ParsePackV2Header(data, &header));
    EXPECT_EQ(header.packKind, harken::kPackKindContent);

    harken::PackTocEntry media{};
    int mediaEntries = 0;
    for (uint32_t i = 0; i < header.entryCount; ++i)
    {
        const harken::PackV2DirectoryEntry entry = harken::ParsePackV2DirectoryEntry(
            data + header.directoryOffset + i * harken::kPackV2DirectoryEntryBytes);
        if (entry.kind != harken::kPackEntryKindMedia)
            continue;
        ++mediaEntries;
        EXPECT_EQ(entry.size, harken::kPackV2MediaPayloadBytes);
        media = harken::ParsePackTocEntry(data + entry.offset);
    }
    EXPECT_EQ(mediaEntries, 1);
    return media;
}

QByteArray readAllBytes(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    return file.readAll();
}

std::shared_ptr<VNX::ProjectItem> makeItem(const QString& name,
                                           VNX::ProjectItem::Type type,
                                           uint32_t objectID)
{
    auto item = std::make_shared<VNX::ProjectItem>();
    item->name = name;
    item->type = type;
    item->objectID = objectID;
    item->guid = QUuid::createUuid();
    item->guid = VNX::authoringUuid(harkentest::StudioObjectId(objectID + 100000));
    item->attachDefaultComponents();
    return item;
}

void addChild(const std::shared_ptr<VNX::ProjectItem>& parent,
              const std::shared_ptr<VNX::ProjectItem>& child)
{
    child->parent = parent.get();
    parent->children.push_back(child);
}

} // namespace

TEST(EditorProjectPackExporter, UsesMediaCacheAndPropagatesLoopToPackc)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir(QDir::tempPath() + QStringLiteral("/pack export \u6d4b\u8bd5 \U0001f3b5-XXXXXX"));
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    constexpr uint16_t kChannels = 2;
    constexpr uint32_t kSampleRate = 48000;
    constexpr uint32_t kFrames = 96;
    const harkenstudio::ObjectId kExpectedSoundId = harkentest::StudioObjectId(100001);

    std::vector<float> samples(kFrames * kChannels, 0.0f);
    for (uint32_t i = 0; i < kFrames; ++i)
    {
        samples[i * kChannels] = static_cast<float>(i % 17) / 32.0f;
        samples[i * kChannels + 1] = static_cast<float>(i % 13) / 24.0f;
    }

    const QString audioRoot = VNX::PathUtils::getAudioSourcesDir(dir.path());
    const QString relativePath = QStringLiteral("tone.wav");
    const QString sourcePath = QDir(audioRoot).filePath(relativePath);
    writeWavF32(sourcePath, samples, kChannels, kSampleRate);

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
    info.metadata.loopInfo.loopStart = 12;
    info.metadata.loopInfo.loopEnd = 72;
    info.lastModified = static_cast<uint64_t>(
        QFileInfo(sourcePath).lastModified().toSecsSinceEpoch());
    ASSERT_TRUE(VNX::AssetRegistry::instance().registerAsset(info));

    auto project = HarkenTest::projectWithPlatforms();
    project.name = QStringLiteral("cache-e2e");
    project.projectPath = QDir(dir.path()).filePath(QStringLiteral("cache-e2e.vproj"));
    VNX::ProjectFileBinding busBinding;
    busBinding.rootItem = makeItem(
        QStringLiteral("MasterBus"), VNX::ProjectItem::Type::Bus, 201);
    project.busses.push_back(std::move(busBinding));
    VNX::AudioSourceData sourceData;
    sourceData.sourceID = info.sourceID;
    sourceData.relativePath = relativePath;
    sourceData.language = QStringLiteral("SFX");
    project.audioSources.push_back(sourceData);

    VNX::ProjectFileBinding binding;
    binding.rootItem = makeItem(QStringLiteral("Root"), VNX::ProjectItem::Type::Layer, 10);
    binding.rootItem->fieldGroups = VNX::MakeRootFieldGroups(
        VNX::toFieldGroupGuid(project.busses.front().rootItem->guid));
    auto sound = makeItem(QStringLiteral("Tone"), VNX::ProjectItem::Type::Sound, 1);
    VNX::SpatialRatingFieldGroup rating;
    rating.spatialIs3DEnabled = true;
    sound->fieldGroups.spatialRating.shared = {VNX::FieldGroupMode::Local, rating};
    HarkenTest::setSharedSoundSource(*sound, info.sourceID);
    auto& authored = sound->comp<VNX::VSound>()->sources.plain.entries.front();
    authored.loopProvenance = VNX::SoundLoopProvenance::Authored;
    authored.loopRegion = VNX::SoundLoopRegion{12, 73};
    addChild(binding.rootItem, sound);
    project.layers.push_back(std::move(binding));

    VNX::ProjectFileBinding actionBinding;
    actionBinding.rootItem = makeItem(
        QStringLiteral("CacheAction"), VNX::ProjectItem::Type::Action, 20);
    VNX::VActionStep playStep;
    playStep.targetObjectID = 1;
    actionBinding.rootItem->comp<VNX::VAction>()->steps.push_back(playStep);
    project.actions.push_back(std::move(actionBinding));

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");

    const QString directPackPath = QDir(dir.path()).filePath(QStringLiteral("direct.bytes"));
    HarkenEditor::Bridge::PackBuildSession directBuilder;
    HarkenEditor::Bridge::PackMediaDesc directMedia;
    directMedia.sourceId = harkentest::StudioObjectId(kExpectedSoundId);
    directMedia.authoringSourceId = info.sourceID;
    directMedia.sourcePath = sourcePath;
    directMedia.loopable = true;
    directMedia.loopStartFrame = 12;
    directMedia.loopEndFrame = 73;
    directBuilder.addMedia(directMedia);
    ASSERT_TRUE(directBuilder.buildToFile(directPackPath))
        << directBuilder.lastError().toStdString();

    const QString packPath = QDir(dir.path()).filePath(QStringLiteral("out.bytes"));
    auto first = exporter.ExportActionPack(
        project, QStringLiteral("CacheAction"), packPath, opts);
    ASSERT_TRUE(first.success) << first.error.toStdString();
    EXPECT_EQ(first.mediaConversions, 1);
    EXPECT_EQ(first.mediaCacheHits, 0);
    EXPECT_EQ(first.mediaFallbacks, 0);

    EXPECT_EQ(readAllBytes(packPath), readAllBytes(directPackPath));

    QJsonObject firstReport = readJsonObject(packPath + QStringLiteral(".report.json"));
    const QJsonObject firstPack = firstReport[QStringLiteral("packs")].toArray()[0].toObject();
    const QJsonObject firstEntry =
        firstPack[QStringLiteral("entries")].toArray()[0].toObject();
    const QString cachePath = firstEntry[QStringLiteral("source")].toString();
    ASSERT_TRUE(QFileInfo::exists(cachePath));
    EXPECT_TRUE(cachePath.contains(QStringLiteral("pack-derivatives/%1/").arg(opts.platform)));
    EXPECT_FALSE(firstEntry[QStringLiteral("skippedByCache")].toBool());

    auto toc = readSingleMediaToc(packPath);
    EXPECT_EQ(toc.soundId, kExpectedSoundId);
    EXPECT_EQ(toc.channelCount, 2u);
    EXPECT_EQ(toc.flags & harken::kPackFlagLoopable, harken::kPackFlagLoopable);
    EXPECT_EQ(toc.loopStartFrame, 12u);
    EXPECT_EQ(toc.loopEndFrame, 73u);

    auto second = exporter.ExportActionPack(
        project, QStringLiteral("CacheAction"), packPath, opts);
    ASSERT_TRUE(second.success) << second.error.toStdString();
    EXPECT_EQ(second.mediaConversions, 0);
    EXPECT_EQ(second.mediaCacheHits, 1);
    EXPECT_EQ(second.mediaFallbacks, 0);

    QJsonObject secondReport = readJsonObject(packPath + QStringLiteral(".report.json"));
    const QJsonObject secondPack =
        secondReport[QStringLiteral("packs")].toArray()[0].toObject();
    const QJsonObject secondEntry =
        secondPack[QStringLiteral("entries")].toArray()[0].toObject();
    EXPECT_EQ(secondEntry[QStringLiteral("source")].toString(),
              QFileInfo(cachePath).absoluteFilePath());
    EXPECT_TRUE(secondEntry[QStringLiteral("skippedByCache")].toBool());
    EXPECT_EQ(secondPack[QStringLiteral("totals")]
                  .toObject()[QStringLiteral("cacheHits")]
                  .toInt(),
              1);

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporter, PackcPreservesUnicodePathsAndCacheThroughCli)
{
    QTemporaryDir dir(QDir::tempPath() + QStringLiteral("/pack cli \u6d4b\u8bd5 \U0001f3b5-XXXXXX"));
    ASSERT_TRUE(dir.isValid());
    const QString sourceName = QStringLiteral("\u58f0\u97f3 source.wav");
    const QString outputName = QStringLiteral("\u5bfc\u51fa output.bytes");
    const QString manifestPath = dir.filePath(QStringLiteral("\u6e05\u5355 input.json"));
    const QString reportPath = dir.filePath(QStringLiteral("\u62a5\u544a report.json"));
    const QString cachePath = dir.filePath(QStringLiteral("\u7f13\u5b58 cache.json"));
    const QString emittedPath = dir.filePath(QStringLiteral("\u89c4\u8303 emitted.json"));
    const QString outputPath = dir.filePath(outputName);
    writeWavF32(dir.filePath(sourceName), std::vector<float>(96, 0.25f), 1, 48000);

    const QJsonObject manifest{
        {QStringLiteral("manifestVersion"), 3},
        {QStringLiteral("output"), outputName},
        {QStringLiteral("entries"), QJsonArray{QJsonObject{
            {QStringLiteral("path"), sourceName},
            {QStringLiteral("id"), VNX::objectIdString(harkentest::StudioObjectId(100001))}}}}};
    QFile manifestFile(manifestPath);
    ASSERT_TRUE(manifestFile.open(QIODevice::WriteOnly));
    const QByteArray manifestBytes = QJsonDocument(manifest).toJson();
    ASSERT_EQ(manifestFile.write(manifestBytes), manifestBytes.size());
    manifestFile.close();

    QByteArray firstPack;
    for (int run = 0; run < 2; ++run)
    {
        QProcess process;
        process.start(QString::fromUtf8(HARKEN_PACKC_PATH),
                      {manifestPath, QStringLiteral("--report"), reportPath,
                       QStringLiteral("--cache-path"), cachePath,
                       QStringLiteral("--emit-manifest"), emittedPath});
        ASSERT_TRUE(process.waitForFinished(30000));
        ASSERT_EQ(process.exitStatus(), QProcess::NormalExit);
        ASSERT_EQ(process.exitCode(), 0) << process.readAllStandardError().toStdString();
        const auto pack = readJsonObject(reportPath)[QStringLiteral("packs")].toArray()[0].toObject();
        EXPECT_EQ(pack[QStringLiteral("totals")].toObject()[QStringLiteral("cacheHits")].toInt(), run);
        const auto entry = pack[QStringLiteral("entries")].toArray()[0].toObject();
        EXPECT_EQ(entry[QStringLiteral("skippedByCache")].toBool(), run == 1);
        EXPECT_TRUE(QFileInfo::exists(cachePath));
        EXPECT_EQ(readJsonObject(emittedPath)[QStringLiteral("output")].toString(), outputName);
        const QByteArray bytes = readAllBytes(outputPath);
        ASSERT_FALSE(bytes.isEmpty());
        if (run == 0)
            firstPack = bytes;
        else
            EXPECT_EQ(bytes, firstPack);
    }
}

TEST(EditorProjectPackExporter, PreservesStereoForSpatialAndNonSpatialSoundsSharingASource)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    constexpr uint16_t kChannels = 2;
    constexpr uint32_t kSampleRate = 48000;
    constexpr uint32_t kFrames = 32;
    std::vector<float> samples(kFrames * kChannels, 0.0f);
    for (uint32_t i = 0; i < kFrames; ++i)
    {
        samples[i * kChannels] = static_cast<float>(i + 1) / 64.0f;
        samples[i * kChannels + 1] = static_cast<float>(kFrames - i) / 64.0f;
    }

    const QString audioRoot = VNX::PathUtils::getAudioSourcesDir(dir.path());
    const QString relativePath = QStringLiteral("shared.wav");
    const QString sourcePath = QDir(audioRoot).filePath(relativePath);
    writeWavF32(sourcePath, samples, kChannels, kSampleRate);

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

    auto project = HarkenTest::projectWithPlatforms();
    project.name = QStringLiteral("shared-source-stereo");
    project.projectPath =
        QDir(dir.path()).filePath(QStringLiteral("shared-source-stereo.vproj"));
    VNX::ProjectFileBinding busBinding;
    busBinding.rootItem = makeItem(
        QStringLiteral("MasterBus"), VNX::ProjectItem::Type::Bus, 201);
    project.busses.push_back(std::move(busBinding));
    VNX::AudioSourceData sourceData;
    sourceData.sourceID = info.sourceID;
    sourceData.relativePath = relativePath;
    sourceData.language = QStringLiteral("SFX");
    project.audioSources.push_back(sourceData);

    VNX::ProjectFileBinding spatialBinding;
    spatialBinding.rootItem =
        makeItem(QStringLiteral("SpatialRoot"), VNX::ProjectItem::Type::Layer, 10);
    spatialBinding.rootItem->fieldGroups = VNX::MakeRootFieldGroups(
        VNX::toFieldGroupGuid(project.busses.front().rootItem->guid));
    auto spatialSound =
        makeItem(QStringLiteral("SpatialSound"), VNX::ProjectItem::Type::Sound, 1);
    VNX::SpatialRatingFieldGroup rating;
    rating.spatialIs3DEnabled = true;
    spatialSound->fieldGroups.spatialRating.shared = {VNX::FieldGroupMode::Local, rating};
    HarkenTest::setSharedSoundSource(*spatialSound, info.sourceID);
    addChild(spatialBinding.rootItem, spatialSound);
    project.layers.push_back(std::move(spatialBinding));

    VNX::ProjectFileBinding twoDimensionalBinding;
    twoDimensionalBinding.rootItem =
        makeItem(QStringLiteral("TwoDimensionalRoot"),
                 VNX::ProjectItem::Type::Layer, 11);
    twoDimensionalBinding.rootItem->fieldGroups = VNX::MakeRootFieldGroups(
        VNX::toFieldGroupGuid(project.busses.front().rootItem->guid));
    auto twoDimensionalSound =
        makeItem(QStringLiteral("TwoDimensionalSound"),
                 VNX::ProjectItem::Type::Sound, 2);
    HarkenTest::setSharedSoundSource(*twoDimensionalSound, info.sourceID);
    addChild(twoDimensionalBinding.rootItem, twoDimensionalSound);
    project.layers.push_back(std::move(twoDimensionalBinding));

    VNX::ProjectFileBinding unrelatedBinding;
    unrelatedBinding.rootItem =
        makeItem(QStringLiteral("UnrelatedRoot"),
                 VNX::ProjectItem::Type::Layer, 12);
    unrelatedBinding.rootItem->fieldGroups = VNX::MakeRootFieldGroups(
        VNX::toFieldGroupGuid(project.busses.front().rootItem->guid));
    auto unrelatedSound =
        makeItem(QStringLiteral("UnrelatedSound"),
                 VNX::ProjectItem::Type::Sound, 3);
    HarkenTest::setSharedSoundSource(*unrelatedSound, info.sourceID);
    addChild(unrelatedBinding.rootItem, unrelatedSound);
    project.layers.push_back(std::move(unrelatedBinding));

    VNX::ProjectFileBinding actionBinding;
    actionBinding.rootItem =
        makeItem(QStringLiteral("SharedSourceAction"),
                 VNX::ProjectItem::Type::Action, 20);
    VNX::VActionStep spatialStep;
    spatialStep.targetObjectID = 1;
    actionBinding.rootItem->comp<VNX::VAction>()->steps.push_back(spatialStep);
    VNX::VActionStep twoDimensionalStep;
    twoDimensionalStep.targetObjectID = 2;
    actionBinding.rootItem->comp<VNX::VAction>()->steps.push_back(twoDimensionalStep);
    project.actions.push_back(std::move(actionBinding));

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const QString packPath = QDir(dir.path()).filePath(QStringLiteral("shared.bytes"));
    const auto result = exporter.ExportActionPack(
        project, QStringLiteral("SharedSourceAction"), packPath, opts);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QJsonObject report = readJsonObject(
        packPath + QStringLiteral(".report.json"));
    const QJsonArray entries = report[QStringLiteral("packs")]
                                   .toArray()[0]
                                   .toObject()[QStringLiteral("entries")]
                                   .toArray();
    ASSERT_EQ(entries.size(), 2);

    QHash<QString, int> channelsById;
    for (const QJsonValue& value : entries)
    {
        const QJsonObject entry = value.toObject();
        channelsById.insert(entry[QStringLiteral("id")].toString(),
                            entry[QStringLiteral("channels")].toInt());
    }
    EXPECT_EQ(channelsById.value(VNX::objectIdString(spatialSound->runtimeId())), 2);
    EXPECT_EQ(channelsById.value(VNX::objectIdString(twoDimensionalSound->runtimeId())), 2);
    EXPECT_FALSE(channelsById.contains(VNX::objectIdString(unrelatedSound->runtimeId())));

    VNX::AssetRegistry::instance().clear();
}
