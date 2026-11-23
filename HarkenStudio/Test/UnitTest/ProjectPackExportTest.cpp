#include "UnitTest/StudioObjectIdTestSupport.h"

#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/AssetConverter.h"
#include "AssetPipeline/BuildSourceInputs.h"
#include "AssetPipeline/CacheArtifactWriter.h"
#include "AssetPipeline/SourceFileState.h"
#include "AssetPipeline/PathUtils.h"
#include "AssetPipeline/WaveformCache.h"
#include "Components/VAction.h"
#include "Components/VAudio.h"
#include "Components/VBus.h"
#include "Components/VConversion.h"
#include "Components/VEffect.h"
#include "Components/VMixNode.h"
#include "Components/VSegment.h"
#include "Components/VSelection.h"
#include "Components/VTrack.h"
#include "Components/VVariable.h"
#include "Tooling/Harkenizer/Core/PackBuildPlan.h"
#include "Tooling/Harkenizer/Core/CanonicalPackSetExport.h"
#include "PlayablePropertyAuthoringV1.h"
#include "Tooling/Harkenizer/Core/ProjectPackExporter.h"
#include "Tooling/Harkenizer/Core/ProjectPackBuildService.h"
#include "Tooling/Harkenizer/Core/ProjectBuildSnapshot.h"
#include "Tooling/Harkenizer/Core/VorbisEncoder.h"
#include "Tooling/Harkenizer/Core/SoundSourceDerivative.h"
#include "Tooling/Harkenizer/ResourceCompiler/ResidentVorbis.h"
#include "RuntimeBridge/PlaybackBridge.h"
#include "ProjectSerializer.h"
#include "ProjectSchemaGenerated.h"
#include "ItemTypeCatalog.h"
#include "NameValidator.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"
#include "Core/Build/PackagingBridge.h"
#include "RuntimeContentBundleBuilder.h"
#include "harken_buildinfo.h"

#include <harken/Harken.h>
#include <harken/HarkenDevelopment.h>
#include "harken_studio_build/WorldCodec.h"
#include "harken_studio_build/MediaContentRevision.h"
#include "harken_studio_build/WorldValidation.h"
#include "harken_studio_build/BusSnapshot.h"
#include "harken_studio_build/PackFormat.h"
#include "harken_studio_build/PackFormatV2.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QCryptographicHash>
#include <QDateTime>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QLockFile>
#include <QScopeGuard>
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <QVector>

#ifdef Q_OS_WIN
#include <QtCore/qt_windows.h>
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <vector>
#include "Support/ProjectPlatformTestUtils.h"

namespace {

harken::studio::ProjectPackExporter::ExportResult buildPersistedPackSet(
    const harken::studio::ProjectData& project, const QString& root,
    const harken::studio::ProjectPackExporter::ExportOptions& options,
    std::vector<harken::studio::ProjectPackExporter::PackBuildRecord>* records = nullptr)
{
    QString error;
    harken::studio::ProjectPackBuildService::Request request;
    request.outputRoot = root;
    request.platforms = {options.platform};
    request.options = options;
    request.options.snapshot = harken::studio::ProjectBuildSnapshot::capture(project, &error);
    const auto result = harken::studio::ProjectPackBuildService::run(request);
    if (result.platforms.empty()) {
        harken::studio::ProjectPackExporter::ExportResult failed;
        failed.error = result.preflightError.isEmpty() ? error : result.preflightError;
        return failed;
    }
    if (records) *records = result.platforms.front().records;
    auto platform = result.platforms.front().result;
    if (platform.success && !result.finalizationResult.success) {
        platform.success = false;
        platform.error = result.finalizationResult.error;
    }
    platform.removedPackCount += result.finalizationResult.removedPackCount;
    platform.canceled = platform.canceled || result.canceled;
    return platform;
}


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

std::shared_ptr<harken::studio::ProjectItem> makeItem(const QString& name,
                                           harken::studio::ProjectItem::Type type,
                                           uint32_t objectID)
{
    auto item = std::make_shared<harken::studio::ProjectItem>();
    item->name = name;
    item->type = type;
    item->objectID = objectID;
    item->guid = harken::studio::authoringUuid(harkentest::StudioObjectId(objectID + 100000));
    item->attachDefaultComponents();
    return item;
}

void addChild(const std::shared_ptr<harken::studio::ProjectItem>& parent,
              const std::shared_ptr<harken::studio::ProjectItem>& child)
{
    child->parent = parent.get();
    parent->children.push_back(child);
}

uint32_t registerPcmAsset(const QString& projectRoot,
                         const QString& relativePath,
                         const std::vector<float>& samples,
                         uint16_t channels)
{
    constexpr uint32_t kSampleRate = 48000;
    const auto frames = static_cast<uint32_t>(samples.size() / channels);
    const QString audioRoot = harken::studio::PathUtils::getAudioSourcesDir(projectRoot);
    const QString sourcePath = QDir(audioRoot).filePath(relativePath);
    writeWavF32(sourcePath, samples, channels, kSampleRate);

    harken::studio::AssetInfo info;
    info.relativePath = relativePath;
    info.absolutePath = sourcePath;
    info.sourceID = harken::studio::AssetRegistry::instance().allocateStableID();
    EXPECT_NE(info.sourceID, 0u);
    info.metadata.sampleRate = kSampleRate;
    info.metadata.channels = channels;
    info.metadata.bitDepth = 32;
    info.metadata.totalSamples = frames;
    info.metadata.fileSize = static_cast<uint64_t>(QFileInfo(sourcePath).size());
    info.lastModified = static_cast<uint64_t>(
        QFileInfo(sourcePath).lastModified().toSecsSinceEpoch());
    EXPECT_TRUE(harken::studio::AssetRegistry::instance().registerAsset(info));
    return info.sourceID;
}

uint32_t registerToneAsset(const QString& projectRoot,
                           const QString& relativePath,
                           float amplitude,
                           uint32_t frames = 96)
{
    std::vector<float> samples(frames, 0.0f);
    for (uint32_t i = 0; i < frames; ++i)
        samples[i] = amplitude * static_cast<float>(i % 13) / 16.0f;
    return registerPcmAsset(projectRoot, relativePath, samples, 1);
}

constexpr uint32_t kSharedSoundObjectId = 1;
constexpr uint32_t kSoloSoundObjectId = 2;
constexpr uint64_t kStableIdBias = 100000;

struct ActionSpec
{
    QString name;
    uint32_t objectID;
    uint32_t targetObjectID;
};

void buildProject(harken::studio::ProjectData& project,
                  const QString& projectRoot,
                  uint32_t sharedSourceId,
                  uint32_t soloSourceId,
                  const std::vector<ActionSpec>& actionSpecs)
{
    project.platforms = HarkenTest::platforms({"Windows", "Android"});
    project.name = QStringLiteral("export-pack");
    project.projectPath =
        QDir(projectRoot).filePath(QStringLiteral("export-pack.vproj"));

    for (uint32_t sourceID : {sharedSourceId, soloSourceId})
    {
        const harken::studio::AssetInfo* info = harken::studio::AssetRegistry::instance().getAsset(sourceID);
        ASSERT_NE(info, nullptr);
        harken::studio::AudioSourceData sourceData;
        sourceData.sourceID = sourceID;
        sourceData.relativePath = info->relativePath;
        project.audioSources.push_back(sourceData);
    }

    auto master = makeItem(QStringLiteral("MasterBus"), harken::studio::ProjectItem::Type::Bus, 201);
    master->guid = harken::studio::authoringUuid(harkentest::StudioObjectId(0x4d4153544552));
    harken::studio::ProjectFileBinding busBinding;
    busBinding.rootItem = master;
    project.busses.push_back(std::move(busBinding));

    auto layer = makeItem(QStringLiteral("Root"), harken::studio::ProjectItem::Type::Layer, 10);
    layer->fieldGroups = harken::studio::MakeRootFieldGroups(harken::studio::toFieldGroupGuid(master->guid));
    auto sharedSound = makeItem(QStringLiteral("SharedTone"),
                                harken::studio::ProjectItem::Type::Sound, kSharedSoundObjectId);
    HarkenTest::setSharedSoundSource(*sharedSound, sharedSourceId);
    addChild(layer, sharedSound);
    auto soloSound = makeItem(QStringLiteral("SoloTone"),
                              harken::studio::ProjectItem::Type::Sound, kSoloSoundObjectId);
    HarkenTest::setSharedSoundSource(*soloSound, soloSourceId);
    addChild(layer, soloSound);
    harken::studio::ProjectFileBinding layerBinding;
    layerBinding.rootItem = layer;
    project.layers.push_back(std::move(layerBinding));

    auto actionsRoot = makeItem(QStringLiteral("ActionsRoot"),
                                harken::studio::ProjectItem::Type::None, 20);
    for (const ActionSpec& spec : actionSpecs)
    {
        auto action = makeItem(spec.name, harken::studio::ProjectItem::Type::Action, spec.objectID);
        harken::studio::VActionStep play;
        play.type = harken::studio::VActionStep::Type::Play;
        play.targetObjectID = spec.targetObjectID;
        action->comp<harken::studio::VAction>()->steps.push_back(play);
        addChild(actionsRoot, action);
    }
    harken::studio::ProjectFileBinding actionBinding;
    actionBinding.rootItem = actionsRoot;
    project.actions.push_back(std::move(actionBinding));
}

void localizeSoloSound(harken::studio::ProjectData& project, uint32_t sourceId)
{
    project.projectLanguages = {QStringLiteral("English"), QStringLiteral("ja")};
    auto* sound = project.layers.front().rootItem->children.at(1)->comp<harken::studio::VSound>();
    ASSERT_NE(sound, nullptr);
    sound->distinguishLanguage = true;
    sound->sources.localized.clear();
    for (const QString& language : project.projectLanguages) {
        harken::studio::SoundSourceDef source;
        source.entryId = QUuid::createUuid();
        source.sourceId = sourceId;
        sound->sources.localized[language].entries.push_back(source);
        sound->sources.localized[language].platforms[HarkenTest::platformIdString("Windows")].active = source.entryId;
    }
}

QJsonArray readPublication(const QString& outputDir, const QString& language = {},
                           bool requireComplete = true,
                           const QVector<harken::studio::PackArtifactMetadata>& artifacts = {})
{
    QJsonArray packs;
    QStringList files;
    QString error;
    QStringList expectedPaths;
    for (const auto& artifact : artifacts) {
        const auto relative = QDir(outputDir).relativeFilePath(artifact.outputPath);
        if (!relative.startsWith(QStringLiteral("../"))) expectedPaths.append(relative);
    }
    EXPECT_TRUE(harken::studio::ReadPackPublication(outputDir, {}, {}, &packs, &files, &error,
                                       requireComplete, expectedPaths))
        << error.toStdString();
    if (language.isEmpty()) return packs;
    QJsonArray selected;
    for (const auto& value : packs) {
        const auto variant = value.toObject().value(QStringLiteral("language")).toString();
        if (variant.isEmpty() || variant == language) selected.append(value);
    }
    return selected;
}

QStringList publicationFiles(const QString& outputDir)
{
    QJsonArray packs;
    QStringList files;
    QString error;
    EXPECT_TRUE(harken::studio::ReadPackPublication(outputDir, {}, {}, &packs, &files, &error))
        << error.toStdString();
    QStringList pairs;
    for (const auto& value : packs) {
        const auto path = value.toObject().value(QStringLiteral("path")).toString();
        pairs << path << path.chopped(6) + QStringLiteral(".json");
    }
    pairs.sort();
    return pairs;
}

QJsonArray actionPackMetadata(const QJsonArray& packs)
{
    QJsonArray actions;
    for (const auto& value : packs) {
        const auto row = value.toObject();
        if (row.value(QStringLiteral("packKind")).toInt() != 2)
            actions.append(row.value(QStringLiteral("packMetadata")));
    }
    return actions;
}

QJsonObject findByName(const QJsonArray& array, const QString& name)
{
    for (const QJsonValue& value : array)
        if (value.toObject().value(QStringLiteral("name")).toString() == name)
            return value.toObject();
    return {};
}

bool packsContainPathEndingWith(const QJsonArray& packs, const QString& suffix)
{
    for (const QJsonValue& value : packs)
        if (value.toString().endsWith(suffix))
            return true;
    return false;
}

QString variantFileName(const harken::studio::ProjectData& project, harken::studio::types::ObjectId stableId, const QString& language = QString())
{
    for (const auto& unit : harken::studio::ProjectPackExporter::CollectActionPackUnits(project))
        if (unit.stableId == stableId)
            return harken::studio::PackBuildPlanning::packFileName(language, unit.name, QStringLiteral(".bytes"));
    ADD_FAILURE() << "Action not found in test project";
    return {};
}

QByteArray readFileBytes(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    return file.readAll();
}

void writeFileBytes(const QString& path, const QByteArray& bytes)
{
    ASSERT_TRUE(QDir().mkpath(QFileInfo(path).absolutePath()));
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::WriteOnly));
    ASSERT_EQ(file.write(bytes), bytes.size());
}



void expectMediaMatchesDefinition(const QByteArray& bytes,
                                  const harken::studio::build::PackTocEntry& media,
                                  const harken::studio::build::PlayablePropertyProgramViewV1& property)
{
    ASSERT_EQ(media.soundId, property.stableId);
    ASSERT_EQ(media.sampleFormat, HK_PACK_SAMPLE_FORMAT_F32_PLANAR);
    ASSERT_LE(media.blobOffset + media.blobBytes,
              static_cast<std::uint64_t>(bytes.size()));
    ASSERT_EQ(media.blobBytes, media.frameCount * media.channelCount * sizeof(float));
    std::vector<float> planar(media.frameCount * media.channelCount);
    std::memcpy(planar.data(), bytes.constData() + media.blobOffset,
                static_cast<std::size_t>(media.blobBytes));
    std::vector<const float*> channels(media.channelCount);
    for (std::uint16_t channel = 0; channel < media.channelCount; ++channel)
        channels[channel] = planar.data() + channel * media.frameCount;
    std::uint64_t revision = 0;
    const bool loopable = (media.flags & HK_PACK_MEDIA_FLAG_LOOPABLE) != 0;
    ASSERT_TRUE(harken::studio::build::ComputeMediaPcmContentRevisionV1(
        media.sampleRate, media.channelCount, media.frameCount, channels.data(),
        1, &revision, loopable ? media.loopStartFrame : 0,
        loopable ? media.loopEndFrame : 0));
    EXPECT_EQ(property.contentRevision, revision);
}


void expectCanonicalPackParsesWithPartition(const QString& path)
{
    const QByteArray bytes = readFileBytes(path);
    harken::studio::build::PackCanonicalImage image;
    ASSERT_EQ(VN_OK,
              harken::studio::build::ParsePackCanonicalImage(
                  reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                  static_cast<std::uint64_t>(bytes.size()), &image))
        << path.toStdString();
    EXPECT_TRUE(image.hasPartition) << path.toStdString();
    EXPECT_EQ(harken::studio::build::kCanonicalDefinitionPartitionBodyVintage,
              image.partitionHeader.bodyVintage)
        << path.toStdString();
}

void expectInitHasVolumeOnlyKind24(const QString& path)
{
    const QByteArray bytes = readFileBytes(path);
    harken::studio::build::PackCanonicalImage image;
    ASSERT_EQ(VN_OK,
              harken::studio::build::ParsePackCanonicalImage(
                  reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                  static_cast<std::uint64_t>(bytes.size()), &image))
        << path.toStdString();
    ASSERT_TRUE(image.hasBusDefinitionSnapshot) << path.toStdString();
    ASSERT_TRUE(image.hasPartition) << path.toStdString();
    EXPECT_EQ(image.requirement.schemaVersion, 2u) << path.toStdString();
    EXPECT_EQ(image.partitionHeader.headerBytes,
              harken::studio::build::kPackBytes<HkPackCanonicalPartitionHeader>)
        << path.toStdString();
    const auto& snapshot = image.busDefinitionSnapshotHeader;
    EXPECT_EQ(snapshot.schemaVersion, 2u) << path.toStdString();
    EXPECT_EQ(snapshot.storageModelVersion, 2u) << path.toStdString();
    EXPECT_EQ(snapshot.headerBytes, harken::studio::build::kPackBytes<HkPackBusDefinitionSnapshotHeader>)
        << path.toStdString();
    EXPECT_EQ(snapshot.publicationCapabilityVintage, 5u) << path.toStdString();
    const auto expected = harken::studio::build::SnapshotBusTriple(snapshot);
    EXPECT_TRUE(harken::studio::build::CanonicalBusTripleFullyNonzero(expected))
        << path.toStdString();
    EXPECT_EQ(snapshot.contentGeneration, snapshot.publishedBusFingerprint)
        << path.toStdString();
    EXPECT_EQ(snapshot.publishedBusFingerprint,
              harken::studio::build::CanonicalVolumeBusFingerprint(image))
        << path.toStdString();
    EXPECT_TRUE(harken::studio::build::CanonicalBusTripleEqual(
        harken::studio::build::RequirementBusTriple(image.requirement), expected))
        << path.toStdString();
    EXPECT_TRUE(harken::studio::build::CanonicalBusTripleEqual(
        harken::studio::build::PartitionBusTriple(image.partitionHeader), expected))
        << path.toStdString();
    for (const auto& edge : image.dependencyEdges) {
        const bool initRequired =
            edge.edgeKind == HK_PACK_DEPENDENCY_EDGE_KIND_INIT &&
            edge.required01 == 1;
        if (initRequired) {
            EXPECT_TRUE(harken::studio::build::CanonicalBusTripleEqual(
                harken::studio::build::DependencyBusTriple(edge), expected))
                << path.toStdString();
        } else {
            EXPECT_EQ(edge.expectedBusContentGeneration, 0u)
                << path.toStdString();
            EXPECT_EQ(edge.expectedPublishedBusFingerprint, 0u)
                << path.toStdString();
            EXPECT_EQ(edge.expectedCompileTargetProfileFingerprint, 0u)
                << path.toStdString();
        }
    }
}

std::uint64_t fnvUtf8(const QString& text)
{
    const QByteArray utf8 = text.toUtf8();
    return harken::studio::build::Fnv1a64(utf8.constData(), static_cast<std::size_t>(utf8.size()));
}

std::uint32_t packKindCount(const QString& path, std::uint32_t kind)
{
    const QByteArray bytes = readFileBytes(path);
    harken::studio::build::PackFileHeader header;
    if (!harken::studio::build::ReadPackFileHeader(
            reinterpret_cast<const std::uint8_t*>(bytes.constData()), &header))
        return 0;
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const harken::studio::build::PackDirectoryEntry record = harken::studio::build::ReadPackDirectoryEntry(
            reinterpret_cast<const std::uint8_t*>(bytes.constData()) +
            header.directoryOffset + i * harken::studio::build::kPackBytes<HkPackDirectoryEntry>);
        if (record.kind == kind)
            ++count;
    }
    return count;
}

void renamePublicationToLegacy(const QDir& dir, bool wrapped,
                               const QString& legacyActionName = {})
{
    const auto publication = readPublication(dir.path());
    QJsonArray rows;
    QJsonArray actions;
    QJsonArray variables;
    QString projectId;
    for (const QJsonValue& value : publication) {
        QJsonObject row = value.toObject();
        projectId = row.value(QStringLiteral("projectId")).toString();
        const auto packMetadata = row.take(QStringLiteral("packMetadata")).toObject();
        const QString current = row.value(QStringLiteral("path")).toString();
        ASSERT_TRUE(current.endsWith(QStringLiteral(".bytes")));
        QString logical = current.chopped(6) + QStringLiteral(".pack");
        if (row.value(QStringLiteral("packKind")).toInt() == 2) {
            for (const auto& variableValue : packMetadata.value(QStringLiteral("variables")).toArray()) {
                const auto variable = variableValue.toObject();
                QJsonArray labels;
                for (const auto& labelValue : variable.value(QStringLiteral("labels")).toArray()) {
                    const auto label = labelValue.toObject();
                    labels.append(QJsonObject{{QStringLiteral("name"), label.value(QStringLiteral("name"))},
                        {QStringLiteral("labelId"), label.value(QStringLiteral("guid"))}});
                }
                variables.append(QJsonObject{{QStringLiteral("codeName"), variable.value(QStringLiteral("name"))},
                    {QStringLiteral("variableId"), variable.value(QStringLiteral("guid"))},
                    {QStringLiteral("labels"), labels}});
            }
        } else {
            actions.append(QJsonObject{{QStringLiteral("name"), packMetadata.value(QStringLiteral("name"))},
                {QStringLiteral("actionStableId"), packMetadata.value(QStringLiteral("guid"))},
                {QStringLiteral("ownerPackStableId"), row.value(QStringLiteral("packStableId"))}});
            if (!legacyActionName.isEmpty()) logical = legacyActionName + QStringLiteral(".pack");
        }
        const QString physical = logical + (wrapped ? QStringLiteral(".bytes") : QString());
        ASSERT_TRUE(QFile::rename(dir.filePath(current), dir.filePath(physical)));
        ASSERT_TRUE(QFile::remove(dir.filePath(current.chopped(6) + QStringLiteral(".json"))));
        row.insert(QStringLiteral("path"), logical);
        rows.append(row);
    }
    const QJsonObject manifest{{QStringLiteral("schema_version"), 3},
        {QStringLiteral("platformId"), HarkenTest::platformIdString("Windows")},
        {QStringLiteral("packs"), rows},
        {QStringLiteral("gameplayCatalog"), QJsonObject{{QStringLiteral("projectId"), projectId},
            {QStringLiteral("actions"), actions}}},
        {QStringLiteral("variableCatalog"), QJsonObject{{QStringLiteral("variables"), variables}}}};
    writeFileBytes(dir.filePath(QStringLiteral("HarkenPackManifest.json")),
                   QJsonDocument(manifest).toJson(QJsonDocument::Compact));
}

} // namespace

TEST(ProjectPackExport, SharedPlanningPreservesVariantCapabilitiesAndDiagnostics)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    harken::studio::AssetRegistry::instance().clear();
    const auto clearAssets = qScopeGuard([] { harken::studio::AssetRegistry::instance().clear(); });
    harken::studio::AssetRegistry::instance().setProjectRoot(dir.path());
    const auto sharedSource = registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const auto soloSource = registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), sharedSource, soloSource,
        {{QStringLiteral("Common"), 21, kSharedSoundObjectId},
         {QStringLiteral("Localized"), 22, kSoloSoundObjectId},
         {QStringLiteral("CommonAgain"), 23, kSharedSoundObjectId}}));
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, soloSource));
    const auto build = harken::studio::ProjectPackExporter::createBuildContext(project);
    harken::studio::RuntimeCapabilityContext cached;
    cached.buildSession = build->capabilities;
    const auto platforms = project.platformIds();
    const auto directPlan = harken::studio::PackBuildPlanning::build(project, platforms, {}, true);
    ASSERT_TRUE(std::any_of(directPlan.variants.begin(), directPlan.variants.end(),
        [](const auto& variant) { return variant.capability.permitted(); }));
    for (int pass = 0; pass < 2; ++pass) {
        const auto sharedPlan = harken::studio::PackBuildPlanning::build(project, platforms, {}, true, {},
            cached, build->closures.get());
        EXPECT_EQ(directPlan.toJson(project), sharedPlan.toJson(project));
    }
    std::vector<const harken::studio::ProjectItem*> items;
    for (const auto* binding : project.allFileBindings()) {
        const auto collect = [&](const auto& self, const harken::studio::ProjectItem* item) -> void {
            items.push_back(item);
            for (const auto& child : item->children) self(self, child.get());
        };
        if (binding && binding->rootItem) collect(collect, binding->rootItem.get());
    }
    for (const auto& platform : platforms) {
        for (const auto& language : project.projectLanguages) {
            const harken::studio::BuildVariant variant{platform, language};
            for (const auto* item : items) {
                for (const auto& entry : harken::studio::RuntimeCapabilityMatrix::entries()) {
                    if (entry.objectType != item->type) continue;
                    const auto direct = harken::studio::RuntimeCapabilityMatrix::evaluateForContext(
                        *item, project, entry.operation, entry.feature, variant, {});
                    const auto shared = harken::studio::RuntimeCapabilityMatrix::evaluateForContext(
                        *item, project, entry.operation, entry.feature, variant, cached);
                    EXPECT_EQ(direct.toJson(), shared.toJson())
                        << item->name.toStdString() << " " << platform.toStdString()
                        << " " << language.toStdString()
                        << " " << harken::studio::RuntimeCapabilityMatrix::featureName(entry.feature).toStdString();
                }
            }
        }
    }
    const auto& localized = *project.layers.front().rootItem->children.at(1);
    for (const auto& platform : platforms) {
        for (const auto& language : project.projectLanguages) {
            harken::studio::RuntimeCapabilityContext direct{QUuid::fromString(platform), language};
            auto shared = direct;
            shared.buildSession = build->capabilities;
            for (const auto& variant : {std::optional<harken::studio::BuildVariant>{},
                    std::optional<harken::studio::BuildVariant>{harken::studio::BuildVariant{}}}) {
                const auto expected = harken::studio::RuntimeCapabilityMatrix::assessForContext(
                    localized, project, harken::studio::RuntimeCapabilityOperation::PackBuild, variant, direct);
                const auto actual = harken::studio::RuntimeCapabilityMatrix::assessForContext(
                    localized, project, harken::studio::RuntimeCapabilityOperation::PackBuild, variant, shared);
                EXPECT_EQ(expected.toJson(), actual.toJson());
            }
        }
    }
    project.actions.front().rootItem->children.back()->comp<harken::studio::VAction>()->steps.front().targetObjectID = 999;
    const auto invalidBuild = harken::studio::ProjectPackExporter::createBuildContext(project);
    harken::studio::RuntimeCapabilityContext invalidContext;
    invalidContext.buildSession = invalidBuild->capabilities;
    const auto invalidDirect = harken::studio::PackBuildPlanning::build(project, platforms, {}, true);
    ASSERT_FALSE(invalidDirect.ready());
    const auto invalidShared = harken::studio::PackBuildPlanning::build(project, platforms, {}, true, {},
        invalidContext, invalidBuild->closures.get());
    EXPECT_EQ(invalidDirect.toJson(project), invalidShared.toJson(project));
}

TEST(ProjectPackExport, PackMetadataCoversEachExportedAction)
{

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    harken::studio::AssetRegistry::instance().clear();
    harken::studio::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("GroupedOne"), 21, kSharedSoundObjectId},
         {QStringLiteral("LooseOne"), 22, kSharedSoundObjectId},
         {QStringLiteral("LooseTwo"), 23, kSoloSoundObjectId}}));

    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");

    auto result = exporter.ExportAllPacks(project, dir.path(), opts);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QJsonArray packs = readPublication(dir.path());
    EXPECT_FALSE(QFileInfo::exists(QDir(dir.path()).filePath(
        QStringLiteral("HarkenPackManifest.json"))));


    const QJsonObject groupedPack = findByName(packs, QStringLiteral("GroupedOne"));
    ASSERT_FALSE(groupedPack.isEmpty());
    const QJsonObject groupedAction = findByName(
        actionPackMetadata(packs), QStringLiteral("GroupedOne"));
    EXPECT_EQ(QUuid(groupedAction.value(QStringLiteral("guid")).toString()),
              project.actions.front().rootItem->children.front()->guid);

    for (const auto& [actionName, stableId] :
         std::vector<std::pair<QString, harken::studio::types::ObjectId>>{
             {QStringLiteral("GroupedOne"), harkentest::StudioObjectId(21 + kStableIdBias)},
             {QStringLiteral("LooseOne"), harkentest::StudioObjectId(22 + kStableIdBias)},
             {QStringLiteral("LooseTwo"), harkentest::StudioObjectId(23 + kStableIdBias)}})
    {
        const QJsonObject actionPack = findByName(packs, actionName);
        EXPECT_EQ(actionPack.value(QStringLiteral("packKind")).toInt(), 3)
            << actionName.toStdString();
        EXPECT_TRUE(actionPack.value(QStringLiteral("path")).toString().endsWith(
            variantFileName(project, stableId)))
            << actionName.toStdString();
    }

    const QJsonObject sharedPack = findByName(packs, QStringLiteral("Shared"));
    EXPECT_TRUE(sharedPack.isEmpty());

    const QJsonObject initPack = findByName(packs, QStringLiteral("Init"));
    EXPECT_EQ(initPack.value(QStringLiteral("packKind")).toInt(), 2);
    EXPECT_TRUE(initPack.value(QStringLiteral("path")).toString().endsWith(
        QStringLiteral("Init.bytes")));
    EXPECT_EQ(initPack.value(QStringLiteral("packMetadata")).toObject().keys(),
              (QStringList{QStringLiteral("packMetadataVersion"), QStringLiteral("variables")}));
    EXPECT_EQ(initPack.value(QStringLiteral("packMetadata")).toObject()
                  .value(QStringLiteral("packMetadataVersion")).toInt(), HARKEN_PACK_METADATA_VERSION);

    for (const QString& fileName :
         {variantFileName(project, harkentest::StudioObjectId(21 + kStableIdBias)),
          variantFileName(project, harkentest::StudioObjectId(22 + kStableIdBias)),
          variantFileName(project, harkentest::StudioObjectId(23 + kStableIdBias))})
    {
        EXPECT_TRUE(QFileInfo::exists(QDir(dir.path()).filePath(fileName)))
            << fileName.toStdString();
        const auto packMetadata = QJsonDocument::fromJson(readFileBytes(QDir(dir.path()).filePath(
            fileName.chopped(6) + QStringLiteral(".json")))).object();
        EXPECT_EQ(packMetadata.keys(), (QStringList{QStringLiteral("guid"), QStringLiteral("name"),
            QStringLiteral("packMetadataVersion")}));
        EXPECT_EQ(packMetadata.value(QStringLiteral("packMetadataVersion")).toInt(), HARKEN_PACK_METADATA_VERSION);
    }

    harken::studio::AssetRegistry::instance().clear();
}

TEST(ProjectPackExport, OptionalPackMetadataDataFollowsPublicationLifecycle)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    harken::studio::AssetRegistry::instance().clear();
    harken::studio::AssetRegistry::instance().setProjectRoot(dir.path());
    const auto sourceId = registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 0.75f);
    const auto otherId = registerToneAsset(dir.path(), QStringLiteral("other.wav"), 0.25f);
    harken::studio::ProjectData project;
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), sourceId, otherId,
        {{QStringLiteral("Parent"), 21, kSharedSoundObjectId},
         {QStringLiteral("Child"), 22, kSoloSoundObjectId}}));
    const auto& trimmedSound = project.layers.front().rootItem->children.at(1);
    auto& source = trimmedSound->comp<harken::studio::VSound>()->sources.plain.entries.front();
    source.sourceId = sourceId;
    source.trimStartFrame = 8;
    source.trimEndFrame = 48;
    harken::studio::VActionStep post;
    post.type = harken::studio::VActionStep::Type::PostAction;
    post.targetObjectID = 22;
    project.actions.front().rootItem->children.front()->comp<harken::studio::VAction>()->steps.push_back(post);
    auto variable = makeItem(QStringLiteral("Speed"), harken::studio::ProjectItem::Type::Variable, 50);
    project.variables.push_back({variable, QStringLiteral("Variables")});
    harken::studio::VActionStep setVariable;
    setVariable.type = harken::studio::VActionStep::Type::SetVariable;
    setVariable.targetObjectID = variable->objectID;
    project.actions.front().rootItem->children.back()->comp<harken::studio::VAction>()->steps.push_back(setVariable);
    auto bindingVariable = makeItem(QStringLiteral("Volume"), harken::studio::ProjectItem::Type::Variable, 51);
    project.variables.push_back({bindingVariable, QStringLiteral("Variables")});
    harken::studio::ParameterBindingDef binding;
    binding.guid = harken::studio::authoringUuid(harkentest::StudioObjectId(500));
    binding.variable = bindingVariable->guid;
    binding.address = harken::studio::BindingSemantics::volumeAddress();
    binding.curve = {{0.0f, -6.0f}, {100.0f, 0.0f}};
    ASSERT_TRUE(project.layers.front().rootItem->comp<harken::studio::VMixNode>()->bindings.upsert(binding));
    auto unusedVariable = makeItem(QStringLiteral("Unused"), harken::studio::ProjectItem::Type::Variable, 52);
    project.variables.push_back({unusedVariable, QStringLiteral("Variables")});
    auto attenuation = makeItem(QStringLiteral("Distance"), harken::studio::ProjectItem::Type::Attenuation, 301);
    project.attenuationItems.push_back({attenuation, QStringLiteral("Attenuations")});
    harken::studio::SpatialRatingFieldGroup rating;
    rating.spatialAttenuationRef = harken::studio::toFieldGroupGuid(attenuation->guid);
    project.layers.front().rootItem->fieldGroups.spatialRating.shared = {harken::studio::FieldGroupMode::Local, rating};
    project.layers.front().rootItem->name = QStringLiteral("RenamedLayer");
    const QString root = QDir(dir.path()).filePath(QStringLiteral("BuildPacks"));
    const QString output = harken::studio::ProjectPackExporter::platformOutputDir(root, project.platforms.front());
    const QString waveformRoot = harken::studio::PathUtils::waveformBuildOutputRoot(project.projectPath, {});
    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    options.selfCleaning = true;
    const auto original = exporter.ExportAllPacks(project, output, options);
    ASSERT_TRUE(original.success) << original.error.toStdString();
    QMap<QString, QByteArray> binaries;
    for (const QString& name : {QStringLiteral("Init"), QStringLiteral("Parent"), QStringLiteral("Child")})
        binaries.insert(name, readFileBytes(QDir(output).filePath(name + QStringLiteral(".bytes"))));
    EXPECT_FALSE(QDir(waveformRoot).exists());
    for (const auto& value : readPublication(output))
        EXPECT_FALSE(value.toObject().value(QStringLiteral("packMetadata")).toObject()
            .contains(QStringLiteral("metadata")));
    project.buildExportPackMetadata = true;
    const auto metadataOnly = exporter.ExportAllPacks(project, output, options);
    ASSERT_TRUE(metadataOnly.success) << metadataOnly.error.toStdString();
    for (const auto& value : readPublication(output)) {
        const auto row = value.toObject();
        const auto packMetadata = row.value(QStringLiteral("packMetadata")).toObject();
        EXPECT_FALSE(packMetadata.contains(QStringLiteral("waveforms")));
        EXPECT_FALSE(QDir(waveformRoot).exists());
        const auto metadata = packMetadata.value(QStringLiteral("metadata")).toObject();
        EXPECT_EQ(packMetadata.value(QStringLiteral("packMetadataVersion")).toInt(), HARKEN_PACK_METADATA_VERSION);
        EXPECT_FALSE(metadata.contains(QStringLiteral("packMetadataVersion")));
        EXPECT_EQ(metadata.value(QStringLiteral("packStableId")), row.value(QStringLiteral("packStableId")));
        EXPECT_EQ(metadata.value(QStringLiteral("platform")).toString(), QStringLiteral("Windows"));
        EXPECT_EQ(metadata.value(QStringLiteral("language")).toString(), QString());
        QSet<QString> guids;
        for (const auto& objectValue : metadata.value(QStringLiteral("objects")).toArray()) {
            const auto object = objectValue.toObject();
            const auto guid = object.value(QStringLiteral("guid")).toString();
            EXPECT_FALSE(guids.contains(guid));
            guids.insert(guid);
            EXPECT_FALSE(object.value(QStringLiteral("path")).toString().isEmpty());
            if (QUuid(guid) == trimmedSound->guid)
                EXPECT_EQ(object.value(QStringLiteral("path")).toString(),
                          QStringLiteral("\\RenamedLayer\\") + trimmedSound->name);
            if (row.value(QStringLiteral("name")) != QStringLiteral("Init"))
                EXPECT_NE(object.value(QStringLiteral("type")).toString(), QStringLiteral("Bus"));
        }
        EXPECT_TRUE(guids.contains(harken::studio::objectIdString(variable->runtimeId())));
        EXPECT_TRUE(guids.contains(harken::studio::objectIdString(bindingVariable->runtimeId())));
        if (row.value(QStringLiteral("name")) == QStringLiteral("Init")) {
            EXPECT_TRUE(guids.contains(harken::studio::objectIdString(project.busses.front().rootItem->runtimeId())));
            EXPECT_TRUE(guids.contains(harken::studio::objectIdString(unusedVariable->runtimeId())));
        } else {
            EXPECT_FALSE(guids.contains(harken::studio::objectIdString(unusedVariable->runtimeId())));
            EXPECT_TRUE(guids.contains(harken::studio::objectIdString(attenuation->runtimeId())));
            EXPECT_TRUE(guids.contains(harken::studio::objectIdString(trimmedSound->runtimeId())));
        }
    }
    project.buildExportWaveforms = true;
    const auto enabled = exporter.ExportAllPacks(project, output, options);
    ASSERT_TRUE(enabled.success) << enabled.error.toStdString();
    for (auto binary = binaries.cbegin(); binary != binaries.cend(); ++binary)
        EXPECT_EQ(readFileBytes(QDir(output).filePath(binary.key() + QStringLiteral(".bytes"))), binary.value());
    const auto publication = readPublication(output);
    const auto parent = findByName(publication, QStringLiteral("Parent")).value(QStringLiteral("packMetadata")).toObject();
    const auto child = findByName(publication, QStringLiteral("Child")).value(QStringLiteral("packMetadata")).toObject();
    const auto parentWaves = parent.value(QStringLiteral("waveforms")).toArray();
    const auto childWaves = child.value(QStringLiteral("waveforms")).toArray();
    const auto parentObjects = parent.value(QStringLiteral("metadata")).toObject()
        .value(QStringLiteral("objects")).toArray();
    ASSERT_EQ(parentWaves.size(), 2);
    ASSERT_EQ(childWaves.size(), 1);
    QSet<QString> files;
    for (const auto& value : parentWaves) {
        const auto row = value.toObject();
        const auto soundObject = std::find_if(parentObjects.begin(), parentObjects.end(), [&](const auto& value) {
            return value.toObject().value(QStringLiteral("guid")) == row.value(QStringLiteral("soundGuid"));
        });
        ASSERT_NE(soundObject, parentObjects.end());
        EXPECT_EQ(soundObject->toObject().value(QStringLiteral("type")).toString(), QStringLiteral("Sound"));
        files.insert(row.value(QStringLiteral("file")).toString());
        if (QUuid(row.value(QStringLiteral("soundGuid")).toString()) == trimmedSound->guid) {
            EXPECT_EQ(row.value(QStringLiteral("trimStartFrame")).toInteger(), 8);
            EXPECT_EQ(row.value(QStringLiteral("trimEndFrame")).toInteger(), 48);
            EXPECT_EQ(row, childWaves.first().toObject());
        } else {
            EXPECT_EQ(row.value(QStringLiteral("trimStartFrame")).toInteger(), 0);
            EXPECT_EQ(row.value(QStringLiteral("trimEndFrame")).toInteger(), 96);
        }
    }
    ASSERT_EQ(files.size(), 1);
    const QString waveformPath = QDir::cleanPath(QDir(output).absoluteFilePath(*files.cbegin()));
    EXPECT_EQ(QFileInfo(waveformPath).absolutePath(), waveformRoot);
    EXPECT_EQ(readFileBytes(waveformPath), readFileBytes(harken::studio::WaveformCache::cachePath(
        harken::studio::AssetRegistry::instance().getAsset(sourceId)->absolutePath)));
    auto absoluteChild = child;
    auto absoluteWaveform = childWaves.first().toObject();
    absoluteWaveform.insert(QStringLiteral("file"), waveformPath);
    absoluteChild.insert(QStringLiteral("waveforms"), QJsonArray{absoluteWaveform});
    writeFileBytes(QDir(output).filePath(QStringLiteral("Child.json")),
        QJsonDocument(absoluteChild).toJson());
    const QString orphan = QDir(waveformRoot).filePath(QStringLiteral("unreferenced.vkd"));
    writeFileBytes(orphan, "unused");
    const auto finalized = harken::studio::ProjectPackExporter::FinalizePackExport(
        root, project, {options.platform}, true, {{options.platform, enabled.outputs}});
    ASSERT_TRUE(finalized.success) << finalized.error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(waveformPath));
    EXPECT_FALSE(QFileInfo::exists(orphan));
    const auto childJson = readFileBytes(QDir(output).filePath(QStringLiteral("Child.json")));
    project.buildExportWaveforms = false;
    options.updateSelectedOnly = true;
    options.onlyUnits = {QStringLiteral("Parent")};
    const auto disabled = exporter.ExportAllPacks(project, output, options);
    ASSERT_TRUE(disabled.success) << disabled.error.toStdString();
    EXPECT_FALSE(QJsonDocument::fromJson(readFileBytes(QDir(output).filePath(QStringLiteral("Parent.json"))))
        .object().contains(QStringLiteral("waveforms")));
    EXPECT_TRUE(QJsonDocument::fromJson(readFileBytes(QDir(output).filePath(QStringLiteral("Parent.json"))))
        .object().contains(QStringLiteral("metadata")));
    EXPECT_EQ(readFileBytes(QDir(output).filePath(QStringLiteral("Child.json"))), childJson);
    EXPECT_TRUE(QFileInfo::exists(waveformPath));
    project.buildExportWaveforms = true;
    const QString blocked = QDir(dir.path()).filePath(QStringLiteral("blocked"));
    project.buildExportPackMetadata = false;
    writeFileBytes(blocked, "file");
    options.waveformOutputDir = blocked;
    const auto warned = exporter.ExportAllPacks(project, output, options);
    EXPECT_FALSE(warned.success);
    EXPECT_FALSE(warned.error.isEmpty());
    for (auto binary = binaries.cbegin(); binary != binaries.cend(); ++binary)
        EXPECT_EQ(readFileBytes(QDir(output).filePath(binary.key() + QStringLiteral(".bytes"))), binary.value());
    const auto parentWithoutWaveforms = QJsonDocument::fromJson(readFileBytes(
        QDir(output).filePath(QStringLiteral("Parent.json")))).object();
    EXPECT_FALSE(parentWithoutWaveforms.contains(QStringLiteral("waveforms")));
    EXPECT_TRUE(parentWithoutWaveforms.contains(QStringLiteral("metadata")));
    EXPECT_EQ(readFileBytes(QDir(output).filePath(QStringLiteral("Child.json"))), childJson);
    ASSERT_TRUE(QFile::remove(waveformPath));
    EXPECT_FALSE(readPublication(output).isEmpty());
    project.buildExportWaveforms = false;
    options.updateSelectedOnly = false;
    options.onlyUnits.clear();
    options.waveformOutputDir.clear();
    const auto disabledAll = exporter.ExportAllPacks(project, output, options);
    ASSERT_TRUE(disabledAll.success) << disabledAll.error.toStdString();
    for (const auto& value : readPublication(output)) {
        const auto packMetadata = value.toObject().value(QStringLiteral("packMetadata")).toObject();
        EXPECT_EQ(packMetadata.value(QStringLiteral("packMetadataVersion")).toInt(), HARKEN_PACK_METADATA_VERSION);
        EXPECT_FALSE(packMetadata.contains(QStringLiteral("waveforms")));
        EXPECT_FALSE(packMetadata.contains(QStringLiteral("metadata")));
    }
    harken::studio::AssetRegistry::instance().clear();
}

TEST(ProjectPackExport, InitHasNoRoutingCatalogAndEveryPackHasKind21)
{

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    harken::studio::AssetRegistry::instance().clear();
    harken::studio::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("GroupedOne"), 21, kSharedSoundObjectId},
         {QStringLiteral("LooseOne"), 22, kSharedSoundObjectId},
         {QStringLiteral("LooseTwo"), 23, kSoloSoundObjectId}}));

    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    auto result = exporter.ExportAllPacks(project, dir.path(), opts);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QString initPath = QDir(dir.path()).filePath(QStringLiteral("Init.bytes"));
    const QByteArray initBytes = readFileBytes(initPath);
    harken::studio::build::PackCanonicalImage initImage;
    ASSERT_EQ(VN_OK,
              harken::studio::build::ParsePackCanonicalImage(
                  reinterpret_cast<const std::uint8_t*>(initBytes.constData()),
                  static_cast<std::uint64_t>(initBytes.size()), &initImage));
    EXPECT_TRUE(initImage.hasPartition);
    bool sawCatalogEntry = false;
    harken::studio::build::PackFileHeader initHeader;
    ASSERT_TRUE(harken::studio::build::ReadPackFileHeader(
        reinterpret_cast<const std::uint8_t*>(initBytes.constData()), &initHeader));
    for (std::uint32_t i = 0; i < initHeader.entryCount; ++i) {
        const harken::studio::build::PackDirectoryEntry record = harken::studio::build::ReadPackDirectoryEntry(
            reinterpret_cast<const std::uint8_t*>(initBytes.constData()) +
            initHeader.directoryOffset + i * harken::studio::build::kPackBytes<HkPackDirectoryEntry>);
        if (record.kind == HK_PACK_ENTRY_KIND_RETIRED_INIT_ROUTING_CATALOG)
            sawCatalogEntry = true;
    }
    EXPECT_FALSE(sawCatalogEntry);

    const QJsonArray packs = readPublication(dir.path());
    EXPECT_EQ(actionPackMetadata(packs).size(), 3);
    EXPECT_FALSE(findByName(packs, QStringLiteral("GroupedOne")).isEmpty());
    EXPECT_FALSE(findByName(packs, QStringLiteral("LooseOne")).isEmpty());
    EXPECT_FALSE(findByName(packs, QStringLiteral("LooseTwo")).isEmpty());

    expectCanonicalPackParsesWithPartition(
        QDir(dir.path()).filePath(variantFileName(project, harkentest::StudioObjectId(21 + kStableIdBias))));
    expectCanonicalPackParsesWithPartition(
        QDir(dir.path()).filePath(variantFileName(project, harkentest::StudioObjectId(22 + kStableIdBias))));
    expectCanonicalPackParsesWithPartition(
        QDir(dir.path()).filePath(variantFileName(project, harkentest::StudioObjectId(23 + kStableIdBias))));

    harken::studio::AssetRegistry::instance().clear();
}

TEST(ProjectPackExport, RenameKeepsPackMetadataIdentitySelfConsistent)
{

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    harken::studio::AssetRegistry::instance().clear();
    harken::studio::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t firstSourceId =
        registerToneAsset(dir.path(), QStringLiteral("first.wav"), 1.0f);
    const uint32_t secondSourceId =
        registerToneAsset(dir.path(), QStringLiteral("second.wav"), 0.5f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), firstSourceId, secondSourceId,
        {{QStringLiteral("BeforeName"), 21, kSharedSoundObjectId}}));

    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    opts.selfCleaning = true;

    const auto root = dir.filePath("exports");
    const QDir output(QDir(root).filePath("Windows"));
    auto firstResult = buildPersistedPackSet(project, root, opts);
    ASSERT_TRUE(firstResult.success) << firstResult.error.toStdString();

    const QString beforeFileName = variantFileName(project, harkentest::StudioObjectId(21 + kStableIdBias));
    EXPECT_EQ(beforeFileName, QStringLiteral("BeforeName.bytes"));
    const auto beforeBytes = readFileBytes(output.filePath(beforeFileName));
    const QJsonObject beforeEntry = findByName(
        readPublication(output.path()),
        QStringLiteral("BeforeName"));
    const QString legacyName = QStringLiteral("action_3da91a64-b947-4560-0000-0000000186b5.bytes");
    ASSERT_TRUE(QFile::copy(output.filePath(beforeFileName),
                           output.filePath(legacyName)));
    {
        const QJsonArray packs = readPublication(output.path());
        const QJsonObject packEntry = findByName(
            packs,
            QStringLiteral("BeforeName"));
        EXPECT_TRUE(packEntry.value(QStringLiteral("path")).toString().endsWith(
            beforeFileName));
        EXPECT_TRUE(QFileInfo::exists(output.filePath(beforeFileName)));
    }

    project.actions.front().rootItem->children.front()->name =
        QStringLiteral("AfterName");

    auto secondResult = buildPersistedPackSet(project, root, opts);
    ASSERT_TRUE(secondResult.success) << secondResult.error.toStdString();

    const QString afterFileName = variantFileName(project, harkentest::StudioObjectId(21 + kStableIdBias));
    EXPECT_EQ(afterFileName, QStringLiteral("AfterName.bytes"));
    EXPECT_FALSE(QFileInfo::exists(output.filePath(beforeFileName)));
    EXPECT_FALSE(QFileInfo::exists(output.filePath(QStringLiteral("BeforeName.json"))));
    EXPECT_FALSE(QFileInfo::exists(output.filePath(legacyName)));

    const QJsonArray packs = readPublication(output.path());
    EXPECT_TRUE(findByName(packs, QStringLiteral("BeforeName")).isEmpty());
    const QJsonObject renamedEntry = findByName(packs, QStringLiteral("AfterName"));
    EXPECT_EQ(renamedEntry.value(QStringLiteral("packStableId")),
              beforeEntry.value(QStringLiteral("packStableId")));
    EXPECT_EQ(renamedEntry.value(QStringLiteral("dependencies")),
              beforeEntry.value(QStringLiteral("dependencies")));
    EXPECT_TRUE(renamedEntry.value(QStringLiteral("path")).toString().endsWith(
        afterFileName));
    EXPECT_TRUE(QFileInfo::exists(output.filePath(afterFileName)));
    EXPECT_EQ(readFileBytes(output.filePath(afterFileName)), beforeBytes);
    EXPECT_EQ(renamedEntry.value(QStringLiteral("packMetadata")).toObject().value(QStringLiteral("guid")),
              beforeEntry.value(QStringLiteral("packMetadata")).toObject().value(QStringLiteral("guid")));

    project.actions.front().rootItem->children.front()->name = QStringLiteral("afterName");
    const auto caseOnly = buildPersistedPackSet(project, root, opts);
    ASSERT_TRUE(caseOnly.success) << caseOnly.error.toStdString();
    const auto caseEntry = findByName(readPublication(output.path()),
                                      QStringLiteral("afterName"));
    EXPECT_EQ(caseEntry.value(QStringLiteral("packStableId")), beforeEntry.value(QStringLiteral("packStableId")));
    EXPECT_EQ(caseEntry.value(QStringLiteral("path")).toString(), QStringLiteral("afterName.bytes"));
    EXPECT_TRUE(QFileInfo::exists(output.filePath(QStringLiteral("afterName.bytes"))));

    harken::studio::AssetRegistry::instance().clear();
}

TEST(ProjectPackExport, InvalidActionPackNamesPreservePublishedFiles)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& registry = harken::studio::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(dir.path());
    const auto sourceId = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), sourceId, sourceId,
        {{QStringLiteral("Shoot"), 21, kSharedSoundObjectId},
         {QStringLiteral("Tail"), 22, kSharedSoundObjectId}}));
    const QString outputDir = QDir(dir.path()).filePath(QStringLiteral("published"));
    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    const auto initial = exporter.ExportAllPacks(project, outputDir, options);
    ASSERT_TRUE(initial.success) << initial.error.toStdString();
    auto publication = [&]() {
        std::map<QString, QByteArray> files;
        QDirIterator it(outputDir, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            files.emplace(QDir(outputDir).relativeFilePath(path), readFileBytes(path));
        }
        return files;
    };
    const auto original = publication();
    auto* action = project.actions.front().rootItem->children.at(1).get();
    for (const QString& name : {QStringLiteral("Init"), QStringLiteral("init"),
                               QStringLiteral("INIT"), QStringLiteral("CON"),
                               QStringLiteral("con"), QStringLiteral("COM1"),
                               QStringLiteral("shoot")}) {
        SCOPED_TRACE(name.toStdString());
        action->name = name;
        std::vector<harken::studio::ProjectPackExporter::PackBuildRecord> records;
        const auto result = exporter.ExportAllPacks(project, outputDir, options, &records);
        EXPECT_FALSE(result.success);
        EXPECT_FALSE(result.error.isEmpty());
        for (const auto& [path, bytes] : original)
            if (!path.startsWith(QStringLiteral("Init.")))
                EXPECT_EQ(readFileBytes(QDir(outputDir).filePath(path)), bytes);
        EXPECT_TRUE(std::any_of(records.begin(), records.end(), [](const auto& record) {
            return !record.success && !record.error.isEmpty();
        }));
        if (name != QStringLiteral("shoot")) {
            std::vector<harken::studio::bridge::PackBuildSession::CanonicalPackEntry> entries;
            const auto single = exporter.ExportActionPack(project, name,
                QDir(outputDir).filePath(QStringLiteral("Init.bytes")), options, entries);
            EXPECT_FALSE(single.success);
            EXPECT_EQ(publication(), original);
        }
    }
    options.onlyUnits = {QStringLiteral("Shoot")};
    const QString selectedDir = QDir(dir.path()).filePath(QStringLiteral("selected"));
    const auto selected = exporter.ExportAllPacks(project, selectedDir, options);
    ASSERT_TRUE(selected.success) << selected.error.toStdString();
    EXPECT_EQ(QDir(selectedDir).entryList({QStringLiteral("*.bytes")}, QDir::Files),
              (QStringList{QStringLiteral("Init.bytes"), QStringLiteral("Shoot.bytes")}));
    options.onlyUnits.clear();
    action->name = QStringLiteral("HarkenPackManifest");
    project.actions.front().rootItem->children.front()->name = QStringLiteral("HarkenEditorWaveforms");
    const auto rebuilt = exporter.ExportAllPacks(project, outputDir, options);
    ASSERT_TRUE(rebuilt.success) << rebuilt.error.toStdString();
    for (const QString& name : {QStringLiteral("HarkenPackManifest"),
                               QStringLiteral("HarkenEditorWaveforms")})
        expectCanonicalPackParsesWithPartition(QDir(outputDir).filePath(name + QStringLiteral(".bytes")));
    EXPECT_EQ(readPublication(outputDir, {}, true, rebuilt.artifacts).size(), 3);
    EXPECT_TRUE(QJsonDocument::fromJson(readFileBytes(QDir(outputDir).filePath(
        QStringLiteral("HarkenEditorWaveforms.json")))).isObject());
    action->name = QString(harken::studio::NameValidator::MAX_NAME_LENGTH, QLatin1Char('A'));
    const auto longest = exporter.ExportAllPacks(project, outputDir, options);
    ASSERT_TRUE(longest.success) << longest.error.toStdString();
    expectCanonicalPackParsesWithPartition(QDir(outputDir).filePath(action->name + QStringLiteral(".bytes")));
    registry.clear();
}

TEST(ProjectPackExport, InvalidActionStepsInSelectedClosurePreservePublication)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& registry = harken::studio::AssetRegistry::instance();
    registry.clear();
    const auto clearRegistry = qScopeGuard([&] { registry.clear(); });
    registry.setProjectRoot(dir.path());
    const auto sourceId = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), sourceId, sourceId,
        {{QStringLiteral("Caller"), 21, kSharedSoundObjectId},
         {QStringLiteral("Child"), 22, kSharedSoundObjectId}}));
    const auto& actions = project.actions.front().rootItem->children;
    harken::studio::VActionStep post;
    post.type = harken::studio::VActionStep::Type::PostAction;
    post.targetObjectID = actions.back()->objectID;
    actions.front()->comp<harken::studio::VAction>()->steps.push_back(post);
    const QString outputDir = dir.filePath(QStringLiteral("published"));
    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    options.onlyUnits = {QStringLiteral("Caller")};
    const auto initial = exporter.ExportAllPacks(project, outputDir, options);
    ASSERT_TRUE(initial.success) << initial.error.toStdString();
    const auto publication = [&] {
        std::map<QString, QByteArray> files;
        QDirIterator it(outputDir, QDir::Files | QDir::Hidden, QDirIterator::Subdirectories);
        while (it.hasNext()) {
            const QString path = it.next();
            files.emplace(QDir(outputDir).relativeFilePath(path), readFileBytes(path));
        }
        return files;
    };
    const auto original = publication();
    ASSERT_FALSE(original.empty());
    EXPECT_FALSE(original.contains(QStringLiteral("Child.bytes")));
    for (const auto& action : actions) {
        for (const auto type : {harken::studio::VActionStep::Type::Play,
                               static_cast<harken::studio::VActionStep::Type>(255)}) {
            SCOPED_TRACE(QStringLiteral("%1/%2").arg(action->name)
                .arg(static_cast<int>(type)).toStdString());
            harken::studio::VActionStep invalid;
            invalid.type = type;
            invalid.targetObjectID = type == harken::studio::VActionStep::Type::Play
                ? 0 : kSharedSoundObjectId;
            auto& steps = action->comp<harken::studio::VAction>()->steps;
            steps.push_back(invalid);
            const auto rejected = exporter.ExportAllPacks(project, outputDir, options);
            EXPECT_FALSE(rejected.success);
            EXPECT_FALSE(rejected.error.isEmpty());
            for (const auto& [path, bytes] : original)
                if (!path.startsWith(QStringLiteral("Init.")))
                    EXPECT_EQ(readFileBytes(QDir(outputDir).filePath(path)), bytes);
            steps.pop_back();
        }
    }
    const auto restored = exporter.ExportAllPacks(project, outputDir, options);
    ASSERT_TRUE(restored.success) << restored.error.toStdString();
    EXPECT_EQ(publication(), original);
}

TEST(ProjectPackExport, UntargetedUnitsStayOutOfPublication)
{

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    harken::studio::AssetRegistry::instance().clear();
    harken::studio::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t firstSourceId =
        registerToneAsset(dir.path(), QStringLiteral("first.wav"), 1.0f);
    const uint32_t secondSourceId =
        registerToneAsset(dir.path(), QStringLiteral("second.wav"), 0.5f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), firstSourceId, secondSourceId,
        {{QStringLiteral("GroupedOne"), 21, kSharedSoundObjectId},
         {QStringLiteral("LooseKept"), 22, kSharedSoundObjectId},
         {QStringLiteral("LooseDropped"), 23, kSoloSoundObjectId}}));

    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions opts;
    opts.selection.actionTargeting.push_back(
        {harkentest::StudioObjectId(23 + kStableIdBias), false, {HarkenTest::platformIdString("Android")}});
    opts.platform = HarkenTest::platformIdString("Windows");

    auto result = exporter.ExportAllPacks(project, dir.path(), opts);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QJsonArray packs = readPublication(dir.path());

    EXPECT_TRUE(findByName(packs, QStringLiteral("LooseDropped")).isEmpty());
    const QJsonObject keptPack = findByName(packs, QStringLiteral("LooseKept"));
    EXPECT_EQ(keptPack.value(QStringLiteral("packKind")).toInt(), 3);
    EXPECT_TRUE(findByName(packs, QStringLiteral("Init")).value(
        QStringLiteral("packKind")).toInt() == 2);

    harken::studio::AssetRegistry::instance().clear();
}

TEST(ProjectPackExport, ActionNameCollisionRejectsBothConflictingPacks)
{

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    harken::studio::AssetRegistry::instance().clear();
    harken::studio::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t firstSourceId =
        registerToneAsset(dir.path(), QStringLiteral("first.wav"), 1.0f);
    const uint32_t secondSourceId =
        registerToneAsset(dir.path(), QStringLiteral("second.wav"), 0.5f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), firstSourceId, secondSourceId,
        {{QStringLiteral("Member"), 21, kSharedSoundObjectId},
         {QStringLiteral("Clash"), 22, kSoloSoundObjectId}}));

    project.actions.front().rootItem->children.front()->name = QStringLiteral("Clash");

    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");

    auto result = exporter.ExportAllPacks(project, dir.path(), opts);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("collid"), Qt::CaseInsensitive))
        << result.error.toStdString();
    EXPECT_EQ(QDir(dir.path()).entryList({QStringLiteral("*.bytes")}, QDir::Files),
              QStringList{QStringLiteral("Init.bytes")});
    ASSERT_NO_FATAL_FAILURE(expectCanonicalPackParsesWithPartition(dir.filePath(QStringLiteral("Init.bytes"))));
    EXPECT_FALSE(QFileInfo::exists(QDir(dir.path()).filePath(
        QStringLiteral("HarkenPackManifest.json"))));

    harken::studio::AssetRegistry::instance().clear();
}

TEST(ProjectPackExport, PackMetadataCommitFailureFailsWholeExport)
{

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    harken::studio::AssetRegistry::instance().clear();
    harken::studio::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t firstSourceId =
        registerToneAsset(dir.path(), QStringLiteral("first.wav"), 1.0f);
    const uint32_t secondSourceId =
        registerToneAsset(dir.path(), QStringLiteral("second.wav"), 0.5f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), firstSourceId, secondSourceId,
        {{QStringLiteral("First"), 21, kSharedSoundObjectId},
         {QStringLiteral("Second"), 22, kSoloSoundObjectId}}));

    const QString outputDir = QDir(dir.path()).filePath(QStringLiteral("output"));
    ASSERT_TRUE(QDir().mkpath(outputDir));
    const QString packMetadataPath = QDir(outputDir).filePath(
        QStringLiteral("Init.json"));
    ASSERT_TRUE(QDir().mkpath(packMetadataPath));

    harken::studio::ProjectPackExporter exporter;
    const auto result = exporter.ExportAllPacks(project, outputDir, {.platform = HarkenTest::platformIdString("Windows")});

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("Init.json")))
        << result.error.toStdString();
    EXPECT_TRUE(QFileInfo(packMetadataPath).isDir());

    harken::studio::AssetRegistry::instance().clear();
}

TEST(ProjectPackExport, UnsupportedBusPropertiesKeepPublishedPackSetUnchanged)
{
    QTemporaryDir source;
    QTemporaryDir output;
    ASSERT_TRUE(source.isValid());
    ASSERT_TRUE(output.isValid());
    auto& registry = harken::studio::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(source.path());
    const auto sourceId = registerToneAsset(source.path(), QStringLiteral("tone.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, source.path(), sourceId, sourceId,
        {{QStringLiteral("PlayTone"), 21, kSharedSoundObjectId}}));
    harken::studio::ProjectPackExporter exporter;
    project.packBuildOutputDir = output.path();
    const harken::studio::ProjectPackExporter::ExportOptions options{
        .platform = HarkenTest::platformIdString("Windows"),
        .currentWorldOutputPath = QDir(output.path()).filePath(QStringLiteral("current-world.bin"))};
    const auto first = exporter.ExportAllPacks(project, {}, options);
    ASSERT_TRUE(first.success) << first.error.toStdString();
    const auto outputBytes = [&]() {
        std::map<QString, QByteArray> bytes;
        QDirIterator files(output.path(), QDir::Files, QDirIterator::Subdirectories);
        while (files.hasNext()) {
            const auto path = files.next();
            bytes.emplace(QDir(output.path()).relativeFilePath(path), readFileBytes(path));
        }
        return bytes;
    };
    const auto published = outputBytes();
    ASSERT_FALSE(published.empty());
    ASSERT_TRUE(published.contains(QStringLiteral("current-world.bin")));
    ASSERT_FALSE(published.contains(QStringLiteral("HarkenPackIndex.json")));
    RecordProperty("publishedFileCount", static_cast<int>(published.size()));
    QCryptographicHash fileSetHash(QCryptographicHash::Sha256);
    for (const auto& [name, bytes] : published) {
        fileSetHash.addData(name.toUtf8());
        fileSetHash.addData(QByteArray::number(bytes.size()));
        fileSetHash.addData(bytes);
    }
    RecordProperty("publishedFileSetSha256", fileSetHash.result().toHex().toStdString());
    auto* master = project.busses.front().rootItem.get();
    const auto rejected = [&](const QString& field) {
        const auto result = exporter.ExportAllPacks(project, {}, options);
        EXPECT_FALSE(result.success);
        EXPECT_TRUE(result.error.contains(field)) << result.error.toStdString();
        EXPECT_TRUE(result.error.contains(master->guid.toString(QUuid::WithoutBraces)));
        EXPECT_EQ(outputBytes(), published);
        auto freshProject = project;
        const QString freshRoot = QDir(output.path()).filePath(QStringLiteral("fresh-") + field);
        freshProject.packBuildOutputDir = freshRoot;
        const auto fresh = exporter.ExportAllPacks(freshProject, {},
            {.platform = options.platform,
             .currentWorldOutputPath = QDir(freshRoot).filePath(QStringLiteral("current-world.bin"))});
        EXPECT_FALSE(fresh.success);
        EXPECT_TRUE(fresh.error.contains(field)) << fresh.error.toStdString();
        EXPECT_FALSE(QFileInfo::exists(freshRoot));
        const QString directRoot = QDir(output.path()).filePath(QStringLiteral("direct-") + field);
        const auto direct = exporter.ExportAllPacks(project, directRoot,
            {.platform = options.platform});
        EXPECT_FALSE(direct.success);
        EXPECT_TRUE(direct.error.contains(field)) << direct.error.toStdString();
        EXPECT_FALSE(QFileInfo::exists(directRoot));
        EXPECT_EQ(outputBytes(), published);
    };
    master->comp<harken::studio::VBus>()->pan = 0.25f;
    rejected(QStringLiteral("pan"));
    master->comp<harken::studio::VBus>()->pan = 0.0f;
    master->comp<harken::studio::VMixNode>()->pitch = 300.0f;
    master->playablePropertyPresence.Set(harkenplayableproperties::PlayablePropertyIdV1::Pitch);
    rejected(QStringLiteral("pitch"));
    master->comp<harken::studio::VMixNode>()->pitch = 0.0f;
    master->comp<harken::studio::VBus>()->listenerRelativeRouting = true;
    rejected(QStringLiteral("listenerRelativeRouting"));
    master->comp<harken::studio::VBus>()->listenerRelativeRouting = false;
    const auto retry = exporter.ExportAllPacks(project, {}, options);
    EXPECT_TRUE(retry.success) << retry.error.toStdString();
    master->comp<harken::studio::VMixNode>()->pitch = 300.0f;
    const auto locator = harken::studio::PlayablePropertyAuthoringV1::definitionLocator(*master);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(harken::studio::PlayablePropertyAuthoringV1::setOverride(*master, *locator,
        static_cast<uint8_t>(harkenplayableproperties::PlayablePropertyIdV1::Pitch),
        QJsonValue(0.0), HarkenTest::PlatformIds::Windows));
    const auto neutralOverride = exporter.ExportAllPacks(project, {}, options);
    EXPECT_TRUE(neutralOverride.success) << neutralOverride.error.toStdString();
    const auto windowsPublished = outputBytes();
    const auto android = exporter.ExportAllPacks(project, {},
        {.platform = HarkenTest::platformIdString("Android")});
    EXPECT_FALSE(android.success);
    EXPECT_TRUE(android.error.contains(QStringLiteral("pitch")));
    EXPECT_EQ(outputBytes(), windowsPublished);
    registry.clear();
}

TEST(ProjectPackExport, TwoExportsAreByteIdentical)
{

    QTemporaryDir firstDir;
    QTemporaryDir secondDir;
    ASSERT_TRUE(firstDir.isValid());
    ASSERT_TRUE(secondDir.isValid());
    harken::studio::AssetRegistry::instance().clear();
    harken::studio::AssetRegistry::instance().setProjectRoot(firstDir.path());
    const uint32_t sharedSourceId =
        registerToneAsset(firstDir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(firstDir.path(), QStringLiteral("solo.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, firstDir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("One"), 21, kSharedSoundObjectId},
         {QStringLiteral("Two"), 22, kSoloSoundObjectId}}));
    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    ASSERT_TRUE(exporter.ExportAllPacks(project, firstDir.path(), opts).success);
    ASSERT_TRUE(exporter.ExportAllPacks(project, firstDir.path(), opts).success);
    ASSERT_TRUE(exporter.ExportAllPacks(project, secondDir.path(), opts).success);
    QDir a(firstDir.path());
    QDir b(secondDir.path());
    const QStringList names = publicationFiles(a.path());
    EXPECT_EQ(names, publicationFiles(b.path()));
    ASSERT_FALSE(names.isEmpty());
    for (const QString& name : names) {
        QFile left(a.filePath(name));
        QFile right(b.filePath(name));
        ASSERT_TRUE(left.open(QIODevice::ReadOnly));
        ASSERT_TRUE(right.open(QIODevice::ReadOnly));
        EXPECT_EQ(left.readAll(), right.readAll()) << name.toStdString();
    }
    harken::studio::AssetRegistry::instance().clear();
}

TEST(ProjectPackExport, LegacySnapshotRequestFieldsDoNotOverrideBusProjection)
{
    QTemporaryDir source;
    ASSERT_TRUE(source.isValid());
    auto& registry = harken::studio::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(source.path());
    const uint32_t sourceId = registerToneAsset(source.path(), QStringLiteral("snapshot.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, source.path(), sourceId, sourceId,
        {{QStringLiteral("Play_Snapshot"), 4, kSoloSoundObjectId}}));
    project.audioSources.resize(1);
    project.layers.front().rootItem->children.erase(project.layers.front().rootItem->children.begin());
    project.actions.front().rootItem->children.front()->guid =
        harken::studio::authoringUuid(harkentest::StudioObjectId(4));
    harken::studio::CanonicalPackSetExportRequest request;
    request.project = &project;
    request.options.platform = HarkenTest::platformIdString("Windows");
    const auto plan = harken::studio::PackBuildPlanning::build(project, {request.options.platform});
    ASSERT_TRUE(plan.ready());
    request.plan = &plan;
    request.includedActionIds = {harkentest::StudioObjectId(4)};
    request.outputDir = QDir(source.path()).filePath(QStringLiteral("default"));
    ASSERT_FALSE(QFileInfo::exists(request.outputDir));
    harken::studio::ProjectPackExporter exporter;
    const auto baseline = harken::studio::ExportCanonicalPackSet(exporter, request, nullptr);
    ASSERT_TRUE(baseline.success) << baseline.error.toStdString();
    ASSERT_NO_FATAL_FAILURE(expectInitHasVolumeOnlyKind24(
        QDir(request.outputDir).filePath(QStringLiteral("Init.bytes"))));
    ASSERT_TRUE(QFileInfo::exists(QDir(request.outputDir).filePath(
        variantFileName(project, harkentest::StudioObjectId(4)))));
    const QByteArray baselineInit = readFileBytes(QDir(request.outputDir).filePath(
        QStringLiteral("Init.bytes")));
    const QByteArray baselineContent = readFileBytes(QDir(request.outputDir).filePath(
        variantFileName(project, harkentest::StudioObjectId(4))));

    const std::array<harken::studio::types::ObjectId harken::studio::CanonicalPackSetExportRequest::*, 6> fields = {
        &harken::studio::CanonicalPackSetExportRequest::snapshotBus,
        &harken::studio::CanonicalPackSetExportRequest::snapshotParent,
        &harken::studio::CanonicalPackSetExportRequest::snapshotSendSource,
        &harken::studio::CanonicalPackSetExportRequest::snapshotSendTarget,
        &harken::studio::CanonicalPackSetExportRequest::snapshotDownmix,
        &harken::studio::CanonicalPackSetExportRequest::snapshotMaster};
    for (std::size_t i = 0; i < fields.size(); ++i) {
        SCOPED_TRACE(i);
        auto changed = request;
        (changed.*fields[i]).high = 1;
        changed.outputDir = QDir(source.path()).filePath(
            QStringLiteral("legacy-field-%1").arg(static_cast<int>(i)));
        ASSERT_FALSE(QFileInfo::exists(changed.outputDir));
        const auto result = harken::studio::ExportCanonicalPackSet(exporter, changed, nullptr);
        ASSERT_TRUE(result.success) << result.error.toStdString();
        const QDir output(changed.outputDir);
        ASSERT_NO_FATAL_FAILURE(expectInitHasVolumeOnlyKind24(
            output.filePath(QStringLiteral("Init.bytes"))));
        EXPECT_EQ(readFileBytes(output.filePath(QStringLiteral("Init.bytes"))),
                  baselineInit);
        EXPECT_EQ(readFileBytes(output.filePath(
                      variantFileName(project, harkentest::StudioObjectId(4)))),
                  baselineContent);
    }
    registry.clear();
}

TEST(ProjectPackExport, ExportedPairsHaveDistinctPackAndActionIdentities)
{

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    harken::studio::AssetRegistry::instance().clear();
    harken::studio::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("EarlyAction"), 21, kSharedSoundObjectId},
         {QStringLiteral("LateAction"), 23, kSharedSoundObjectId}}));

    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const auto result = exporter.ExportAllPacks(project, dir.path(), opts);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QJsonArray packs = readPublication(dir.path());
    ASSERT_EQ(packs.size(), 3);
    QSet<QString> packIds;
    QSet<QUuid> actionIds;
    for (const auto& value : packs) {
        const auto row = value.toObject();
        EXPECT_FALSE(packIds.contains(row.value(QStringLiteral("packStableId")).toString()));
        packIds.insert(row.value(QStringLiteral("packStableId")).toString());
        if (row.value(QStringLiteral("packKind")).toInt() == 2) continue;
        const auto packMetadata = row.value(QStringLiteral("packMetadata")).toObject();
        const QUuid guid(packMetadata.value(QStringLiteral("guid")).toString());
        EXPECT_FALSE(guid.isNull());
        EXPECT_FALSE(actionIds.contains(guid));
        actionIds.insert(guid);
    }
    EXPECT_EQ(actionIds.size(), 2);

    harken::studio::AssetRegistry::instance().clear();
}

TEST(ProjectPackExport, LaterExportPreservesUnownedFiles)
{

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    harken::studio::AssetRegistry::instance().clear();
    harken::studio::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("KeepOne"), 21, kSharedSoundObjectId}}));

    const QString outputDir = QDir(dir.path()).filePath(QStringLiteral("out"));
    ASSERT_TRUE(QDir().mkpath(outputDir));
    {
        QFile stale(QDir(outputDir).filePath(QStringLiteral("Obsolete.bytes")));
        ASSERT_TRUE(stale.open(QIODevice::WriteOnly));
        stale.write("stale");
    }
    {
        const QString nestedDir = QDir(outputDir).filePath(QStringLiteral("OldLanguage"));
        ASSERT_TRUE(QDir().mkpath(nestedDir));
        QFile stale(QDir(nestedDir).filePath(QStringLiteral("Obsolete.bytes")));
        ASSERT_TRUE(stale.open(QIODevice::WriteOnly));
        stale.write("stale");
        writeFileBytes(QDir(nestedDir).filePath(QStringLiteral("HarkenEditorWaveforms.json")), "stale-metadata");
    }
    {
        QFile notes(QDir(outputDir).filePath(QStringLiteral("notes.txt")));
        ASSERT_TRUE(notes.open(QIODevice::WriteOnly));
        notes.write("keep");
    }

    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const auto result = exporter.ExportAllPacks(project, outputDir, opts);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QJsonArray packs = readPublication(outputDir);
    QSet<QString> named;
    for (const QJsonValue& value : packs)
        named.insert(value.toObject().value(QStringLiteral("path")).toString());

    const QDir live(outputDir);
    EXPECT_TRUE(QFileInfo::exists(live.filePath(QStringLiteral("Obsolete.bytes"))));
    EXPECT_TRUE(QFileInfo::exists(live.filePath(QStringLiteral("OldLanguage"))));
    EXPECT_TRUE(QFileInfo::exists(live.filePath(QStringLiteral("notes.txt"))));
    QDirIterator livePacks(outputDir, {"*.bytes"}, QDir::Files,
                          QDirIterator::Subdirectories);
    while (livePacks.hasNext()) {
        livePacks.next();
        const QString relative = live.relativeFilePath(livePacks.filePath());
        EXPECT_TRUE(named.contains(relative) || relative.endsWith(QStringLiteral("Obsolete.bytes"))) << relative.toStdString();
    }
    for (const QJsonValue& value : packs) {
        const QString path = value.toObject().value(QStringLiteral("path")).toString();
        EXPECT_TRUE(QFileInfo::exists(live.filePath(path))) << path.toStdString();
    }

    harken::studio::AssetRegistry::instance().clear();
}

TEST(ProjectPackExport, FullRebuildReplacesRetiredOutputsWithoutReadingTheirMetadata)
{
    QTemporaryDir dir;
    auto& registry = harken::studio::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(dir.path());
    const auto source = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), source, source,
        {{QStringLiteral("One"), 21, kSoloSoundObjectId}}));
    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    opts.selfCleaning = true;
    for (bool wrapped : {false, true}) {
        const auto root = QDir(dir.path()).filePath(wrapped ? "wrapped" : "plain");
        const QDir live(QDir(root).filePath("Windows"));
        ASSERT_TRUE(buildPersistedPackSet(project, root, opts).success);
        const QJsonArray native = readPublication(live.path());
        ASSERT_NO_FATAL_FAILURE(renamePublicationToLegacy(live, wrapped));
        QJsonArray retiredPacks;
        QStringList retiredFiles;
        QString error;
        EXPECT_FALSE(harken::studio::ReadPackPublication(live.path(), {}, {}, &retiredPacks, &retiredFiles, &error));
        EXPECT_TRUE(retiredPacks.isEmpty());
        writeFileBytes(live.filePath("HarkenPackManifest.json"), "invalid-retired-metadata");
        const QString oldInit = wrapped ? QStringLiteral("Init.pack.bytes") : QStringLiteral("Init.pack");
        writeFileBytes(live.filePath("unrelated.bytes"), "other");
        writeFileBytes(live.filePath("arbitrary.pack"), "other-pack");
        writeFileBytes(live.filePath(oldInit + ".meta"), "guid");
        writeFileBytes(live.filePath("Init.bytes"), "conflict");
        const auto migrated = buildPersistedPackSet(project, root, opts);
        ASSERT_TRUE(migrated.success) << migrated.error.toStdString();
        EXPECT_EQ(readPublication(live.path()), native);
        EXPECT_FALSE(QFile::exists(live.filePath(oldInit)));
        EXPECT_FALSE(QFile::exists(live.filePath("unrelated.bytes")));
        EXPECT_FALSE(QFile::exists(live.filePath("arbitrary.pack")));
        EXPECT_FALSE(QFile::exists(live.filePath(oldInit + ".meta")));
    }
    registry.clear();
}

TEST(ProjectPackExport, FullRebuildCleansDotPrefixedRetiredOutputs)
{
    QTemporaryDir dir;
    auto& registry = harken::studio::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(dir.path());
    const auto source = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), source, source,
        {{QStringLiteral("One"), 21, kSoloSoundObjectId}}));
    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    opts.selfCleaning = true;
    for (bool wrapped : {false, true}) {
        const auto root = QDir(dir.path()).filePath(wrapped ? "wrapped" : "plain");
        const QDir live(QDir(root).filePath("Windows"));
        const auto initial = buildPersistedPackSet(project, root, opts);
        ASSERT_TRUE(initial.success) << initial.error.toStdString();
        const auto expected = readPublication(live.path());
        ASSERT_NO_FATAL_FAILURE(renamePublicationToLegacy(live, wrapped, QStringLiteral(".Ambient")));
        const auto migrated = buildPersistedPackSet(project, root, opts);
        ASSERT_TRUE(migrated.success) << migrated.error.toStdString();
        EXPECT_EQ(readPublication(live.path()), expected);
        EXPECT_TRUE(QFileInfo::exists(live.filePath(QStringLiteral("One.bytes"))));
        EXPECT_FALSE(QFileInfo::exists(live.filePath(QStringLiteral(".Ambient.bytes"))));
        EXPECT_FALSE(QFileInfo::exists(live.filePath(wrapped ? ".Ambient.pack.bytes" : ".Ambient.pack")));
    }
    registry.clear();
}

TEST(ProjectPackExport, ProductionPackPairGoldenIsRepeatable)
{
    QTemporaryDir source;
    ASSERT_TRUE(source.isValid());
    auto& registry = harken::studio::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(source.path());
    const uint32_t sourceId = registerPcmAsset(source.path(), QStringLiteral("golden.wav"),
        {0.0f, 1.0f, 0.5f, -0.25f, -1.0f, 0.0f}, 2);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, source.path(), sourceId, sourceId,
        {{QStringLiteral("Play_Golden"), 4, kSoloSoundObjectId}}));
    project.projectId = QStringLiteral("11111111-1111-1111-1111-111111111111");
    project.platforms = HarkenTest::platforms({"Windows"});
    project.audioSources.resize(1);
    project.layers.front().rootItem->children.erase(project.layers.front().rootItem->children.begin());
    project.actions.front().rootItem->children.front()->guid =
        harken::studio::authoringUuid(harkentest::StudioObjectId(4));

    const QString requested = QString::fromUtf8(qgetenv("HARKEN_PACK_PAIR_GOLDEN_OUT"));
    const QString outputRoot = requested.isEmpty()
        ? QDir(source.path()).filePath(QStringLiteral("golden-output")) : requested;
    ASSERT_FALSE(QFileInfo::exists(outputRoot));
    const QString contentName = variantFileName(project, harkentest::StudioObjectId(4));
    QStringList expectedNames{QStringLiteral("Init.bytes"), QStringLiteral("Init.json"),
        contentName, contentName.chopped(6) + QStringLiteral(".json")};
    expectedNames.sort();
    const QString first = QDir(outputRoot).filePath(QStringLiteral("first"));
    const QString second = QDir(outputRoot).filePath(QStringLiteral("second"));
    for (const QString& directory : {first, second}) {
        ASSERT_FALSE(QFileInfo::exists(directory));
        harken::studio::ProjectPackExporter exporter;
        harken::studio::ProjectPackExporter::ExportOptions opts;
        opts.platform = HarkenTest::platformIdString("Windows");
        const auto result = exporter.ExportAllPacks(project, directory, opts);
        ASSERT_TRUE(result.success) << result.error.toStdString();
        const QDir output(directory);
        EXPECT_EQ(output.entryList({"*.bytes", "*.json"}, QDir::Files, QDir::Name), expectedNames);
        ASSERT_NO_FATAL_FAILURE(expectInitHasVolumeOnlyKind24(output.filePath(QStringLiteral("Init.bytes"))));
        const auto packs = readPublication(directory);
        ASSERT_EQ(packs.size(), 2);
        for (const auto& value : packs) {
            const auto row = value.toObject();
            const QString path = output.filePath(row.value(QStringLiteral("path")).toString());
            const auto bytes = readFileBytes(path);
            EXPECT_EQ(row.value(QStringLiteral("contentHash")).toString().toULongLong(),
                harken::studio::build::Fnv1a64(bytes.constData(), bytes.size()));
            harken::studio::build::PackFileHeader header;
            ASSERT_GE(bytes.size(), harken::studio::build::kPackBytes<HkPackFileHeader>);
            ASSERT_TRUE(harken::studio::build::ReadPackFileHeader(reinterpret_cast<const uint8_t*>(bytes.constData()), &header));
            EXPECT_EQ(header.formatVersion, HK_PACK_FORMAT_VERSION);
            ASSERT_NO_FATAL_FAILURE(expectCanonicalPackParsesWithPartition(path));
        }
        const auto content = findByName(packs, QStringLiteral("Play_Golden"));
        ASSERT_FALSE(content.isEmpty());
        EXPECT_EQ(QUuid(content.value(QStringLiteral("packMetadata")).toObject()
            .value(QStringLiteral("guid")).toString()), harken::studio::authoringUuid(harkentest::StudioObjectId(4)));
    }
    for (const QString& name : expectedNames) {
        const auto a = readFileBytes(QDir(first).filePath(name));
        const auto b = readFileBytes(QDir(second).filePath(name));
        EXPECT_EQ(a, b) << name.toStdString();
        EXPECT_EQ(harken::studio::build::Fnv1a64(a.constData(), a.size()), harken::studio::build::Fnv1a64(b.constData(), b.size()));
    }
    registry.clear();
}

TEST(ProjectPackExport, ExportsContainerAndVariableProjectDeterministically)
{
    QTemporaryDir sources;
    QTemporaryDir first;
    QTemporaryDir second;
    ASSERT_TRUE(sources.isValid() && first.isValid() && second.isValid());
    auto& registry = harken::studio::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(sources.path());
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    project.name = QStringLiteral("PlayableDemo");
    project.projectId = QStringLiteral("2c2ed2ff-33b0-4940-af9e-b1d174612e2b");
    project.projectPath = QDir(sources.path()).filePath(QStringLiteral("PlayableDemo.vproj"));
    project.projectLanguages.clear();
    project.defaultLanguage.clear();
    auto master = makeItem(QStringLiteral("MasterBus"), harken::studio::ProjectItem::Type::Bus, 201);
    harken::studio::ProjectFileBinding busBinding;
    busBinding.rootItem = master;
    project.busses.push_back(busBinding);
    auto layer = makeItem(QStringLiteral("Demo"), harken::studio::ProjectItem::Type::Layer, 202);
    layer->fieldGroups = harken::studio::MakeRootFieldGroups(harken::studio::toFieldGroupGuid(master->guid));
    harken::studio::ProjectFileBinding layerBinding;
    layerBinding.rootItem = layer;
    project.layers.push_back(layerBinding);
    auto variable = makeItem(QStringLiteral("Demo_Choice"), harken::studio::ProjectItem::Type::Variable, 203);
    auto* declaration = variable->comp<harken::studio::VVariable>();
    ASSERT_NE(declaration, nullptr);
    declaration->kind = harken::studio::VariableKind::Step;
    declaration->rangeMin = 0.0f;
    declaration->rangeMax = 1.0f;
    declaration->defaultValue = 0.0f;
    declaration->normalizeDeclaration();
    auto variables = makeItem(QStringLiteral("Variables"), harken::studio::ProjectItem::Type::None, 204);
    addChild(variables, variable);
    harken::studio::ProjectFileBinding variableBinding;
    variableBinding.rootItem = variables;
    project.variables.push_back(variableBinding);
    auto actions = makeItem(QStringLiteral("Actions"), harken::studio::ProjectItem::Type::None, 205);
    harken::studio::ProjectFileBinding actionBinding;
    actionBinding.rootItem = actions;
    project.actions.push_back(actionBinding);
    const QStringList names = {QStringLiteral("Play_Random_Draw"), QStringLiteral("Play_Shuffle"),
        QStringLiteral("Play_Random_Queue"), QStringLiteral("Play_Sequence"),
        QStringLiteral("Play_Sequence_Queue"), QStringLiteral("Play_Variable_Control")};
    const harken::studio::SelectionMode modes[] = {harken::studio::SelectionMode::Random, harken::studio::SelectionMode::Random,
        harken::studio::SelectionMode::Random, harken::studio::SelectionMode::Sequence,
        harken::studio::SelectionMode::Sequence, harken::studio::SelectionMode::Match};
    for (int i = 0; i < names.size(); ++i) {
        const uint32_t id = 300 + static_cast<uint32_t>(i) * 10;
        auto container = makeItem(names[i] + QStringLiteral("_Container"), harken::studio::ProjectItem::Type::Container, id);
        addChild(layer, container);
        auto* selection = container->comp<harken::studio::VSelection>();
        selection->mode = modes[i];
        selection->playType = (i == 2 || i == 4)
            ? harken::studio::SelectionPlayType::Queue : harken::studio::SelectionPlayType::Single;
        selection->transitions.rules.front().exitTiming = harken::studio::TransitionExitTiming::Immediate;
        selection->behavior.memoryScope = (i == 2 || i >= 4)
            ? harken::studio::SelectionMemoryScope::PerEmitter : harken::studio::SelectionMemoryScope::Global;
        if (i == 1) selection->behavior.discipline = harken::studio::RandomDiscipline::Shuffle;
        if (i == 5) {
            auto grid = selection->matchGrid();
            ASSERT_TRUE(grid.addAxis(variable->guid));
            EXPECT_TRUE(selection->selections.shared.setMatchGrid(grid));
        }
        for (int j = 0; j < 2; ++j) {
            const QString file = QStringLiteral("demo_%1_%2.wav").arg(i).arg(j);
            const uint32_t source = registerToneAsset(sources.path(), file, j == 0 ? 0.25f : 0.75f, 24000);
            harken::studio::AudioSourceData data;
            data.sourceID = source;
            data.relativePath = file;
            project.audioSources.push_back(data);
            auto sound = makeItem(names[i] + QStringLiteral("_%1").arg(j), harken::studio::ProjectItem::Type::Sound, id + 1 + j);
            HarkenTest::setSharedSoundSource(*sound, source);
            addChild(container, sound);
            ASSERT_TRUE(harken::studio::PlayablePropertyAuthoringV1::addContainerChildOccurrence(*container, sound->guid));
            if (i == 5) {
                {
                    auto grid = selection->matchGrid();
                    ASSERT_TRUE(grid.addRow(sound->guid));
                    ASSERT_TRUE(grid.setCell(j, 0, j));
                    EXPECT_TRUE(selection->selections.shared.setMatchGrid(grid));
                }
            }
        }
        auto action = makeItem(names[i], harken::studio::ProjectItem::Type::Action, id + 3);
        harken::studio::VActionStep play;
        play.type = harken::studio::VActionStep::Type::Play;
        play.targetObjectID = container->objectID;
        play.targetName = container->name;
        action->comp<harken::studio::VAction>()->steps.push_back(play);
        addChild(actions, action);
    }
    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    for (const QString& root : {first.path(), second.path()}) {
        const QString output = harken::studio::ProjectPackExporter::platformOutputDir(root, project.platforms.front());
        const auto result = exporter.ExportAllPacks(project, output, options);
        ASSERT_TRUE(result.success) << result.error.toStdString();
        QString error;
    for (const auto& artifact : result.artifacts)
        ASSERT_NO_FATAL_FAILURE(expectCanonicalPackParsesWithPartition(artifact.outputPath));
    }
    const QString output = harken::studio::ProjectPackExporter::platformOutputDir(first.path(), project.platforms.front());
    const QJsonArray packs = readPublication(output);
    const QJsonArray rows = actionPackMetadata(packs);
    ASSERT_EQ(rows.size(), names.size());
    QSet<QString> owners;
    for (const QString& name : names) {
        const QJsonObject row = findByName(rows, name);
        ASSERT_FALSE(row.isEmpty());
        EXPECT_FALSE(QUuid(row.value(QStringLiteral("guid")).toString()).isNull());
        owners.insert(findByName(packs, name).value(QStringLiteral("packStableId")).toString());
    }
    EXPECT_EQ(owners.size(), 6);
    const QJsonArray variablesJson = findByName(packs, QStringLiteral("Init"))
        .value(QStringLiteral("packMetadata")).toObject().value(QStringLiteral("variables")).toArray();
    ASSERT_EQ(variablesJson.size(), 1);
    EXPECT_EQ(variablesJson[0].toObject().value(QStringLiteral("name")).toString(), QStringLiteral("Demo_Choice"));
    EXPECT_EQ(QUuid(variablesJson[0].toObject().value(QStringLiteral("guid")).toString()), variable->guid);
    QStringList relativeFiles;
    const QString platformRelative = QDir(first.path()).relativeFilePath(output);
    ASSERT_EQ(packs.size(), 7);
    for (const QString& file : publicationFiles(output))
        relativeFiles << platformRelative + QLatin1Char('/') + file;
    for (const QString& relative : relativeFiles) {
        const QByteArray bytes = readFileBytes(QDir(first.path()).filePath(relative));
        ASSERT_FALSE(bytes.isEmpty()) << relative.toStdString();
        ASSERT_EQ(bytes, readFileBytes(QDir(second.path()).filePath(relative))) << relative.toStdString();
    }
}

TEST(ProjectPackExport, SelectedNonLocalizedActionsAreSelfContained)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    harken::studio::AssetRegistry::instance().clear();
    harken::studio::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("First"), 21, kSharedSoundObjectId},
         {QStringLiteral("Second"), 22, kSharedSoundObjectId},
         {QStringLiteral("Spoken"), 23, kSoloSoundObjectId}}));
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, soloSourceId));
    project.projectLanguages.clear();
    auto* unselectedSound = project.layers.front().rootItem->children.at(1)->comp<harken::studio::VSound>();
    unselectedSound->sources.plain.active = QUuid{};
    unselectedSound->sources.plain.platforms.clear();

    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    opts.onlyUnits = {QStringLiteral("First"), QStringLiteral("Second")};
    opts.language = QStringLiteral("French");
    const QString outputDir = harken::studio::ProjectPackExporter::platformOutputDir(
        dir.path(), *harken::studio::ProjectPlatformSemantics::find(project.platforms, QUuid(opts.platform)));
    std::vector<harken::studio::ProjectPackExporter::PackBuildRecord> records;
    const auto result = exporter.ExportAllPacks(project, outputDir, opts, &records);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QJsonArray packs = readPublication(outputDir);
    ASSERT_EQ(packs.size(), 3);
    EXPECT_TRUE(findByName(packs, QStringLiteral("Shared")).isEmpty());
    EXPECT_TRUE(findByName(packs, QStringLiteral("Spoken")).isEmpty());
    for (const QJsonValue& value : packs) {
        const QJsonObject pack = value.toObject();
        EXPECT_TRUE(pack.value(QStringLiteral("language")).toString().isEmpty());
        const QString relative = pack.value(QStringLiteral("path")).toString();
        EXPECT_FALSE(relative.contains(QLatin1Char('/')));
        EXPECT_TRUE(QFileInfo::exists(QDir(outputDir).filePath(relative)));
    }
    for (const auto& record : records)
        EXPECT_TRUE(record.language.isEmpty());
    harken::studio::AssetRegistry::instance().clear();
}

TEST(ProjectPackExport, RuntimeBundlePreservesLanguagePathsThroughDeploy)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    harken::studio::AssetRegistry::instance().clear();
    harken::studio::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t commonSourceId =
        registerToneAsset(dir.path(), QStringLiteral("common.wav"), 1.0f);
    const uint32_t englishSourceId =
        registerToneAsset(dir.path(), QStringLiteral("english.wav"), 0.5f);
    const uint32_t japaneseSourceId =
        registerToneAsset(dir.path(), QStringLiteral("japanese.wav"), 0.875f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), commonSourceId, englishSourceId,
        {{QStringLiteral("Spoken"), 21, kSoloSoundObjectId},
         {QStringLiteral("Common"), 22, kSharedSoundObjectId}}));
    harken::studio::AudioSourceData japaneseSource;
    japaneseSource.sourceID = japaneseSourceId;
    japaneseSource.relativePath = QStringLiteral("japanese.wav");
    project.audioSources.push_back(japaneseSource);
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, englishSourceId));
    project.layers.front().rootItem->children.at(1)->comp<harken::studio::VSound>()
        ->sources.localized["ja"].entries.front().sourceId = japaneseSourceId;
    project.buildExportWaveforms = true;

    const QString publishRoot = QDir(dir.path()).filePath(QStringLiteral("published"));
    const auto bundle = harken::studio::RuntimeContentBundleBuilder::build(
        project, publishRoot, HarkenTest::platformIdString("Windows"));
    ASSERT_TRUE(bundle.success) << bundle.error.toStdString();
    ASSERT_EQ(bundle.orderedPacks.size(), 4u);
    const QString prefix = QStringLiteral("Bundles/%1/").arg(bundle.bundleId);
    const QSet<QString> expectedPaths = {
        prefix + QStringLiteral("Init.bytes"),
        prefix + QStringLiteral("Common.bytes"),
        prefix + QStringLiteral("English/Spoken.bytes"),
        prefix + QStringLiteral("ja/Spoken.bytes")};
    QSet<QString> resultPaths;
    QCryptographicHash legacyFlatIdentity(QCryptographicHash::Sha256);
    legacyFlatIdentity.addData(bundle.platform.toUtf8());
    legacyFlatIdentity.addData(QByteArray::number(
        static_cast<qulonglong>(bundle.worldSemanticFingerprint)));
    legacyFlatIdentity.addData(QCryptographicHash::hash(
        readFileBytes(QDir(publishRoot).filePath(bundle.currentWorldPath)),
        QCryptographicHash::Sha256).toHex());
    for (const auto& pack : bundle.orderedPacks) {
        resultPaths.insert(pack.path);
        legacyFlatIdentity.addData(pack.sha256.toLatin1());
    }
    EXPECT_EQ(resultPaths, expectedPaths);
    EXPECT_NE(bundle.bundleId, QString::fromLatin1(legacyFlatIdentity.result().toHex()));

    const QString deployRoot = QDir(dir.path()).filePath(QStringLiteral("deployed"));
    QString error;
    ASSERT_TRUE(harken::studio::RuntimeContentBundleBuilder::deploy(bundle, deployRoot, &error))
        << error.toStdString();
    const QByteArray manifestBytes = readFileBytes(bundle.manifestPath);
    for (const QString& root : {publishRoot, deployRoot}) {
        SCOPED_TRACE(root.toStdString());
        const QByteArray bytes = readFileBytes(
            QDir(root).filePath(QStringLiteral("HarkenContent.json")));
        EXPECT_EQ(bytes, manifestBytes);
        const auto manifest = QJsonDocument::fromJson(bytes);
        ASSERT_TRUE(manifest.isObject());
        const auto packs = manifest.object().value(QStringLiteral("orderedPacks")).toArray();
        ASSERT_EQ(packs.size(), 4);
        QSet<QString> paths;
        for (int index = 0; index < packs.size(); ++index) {
            const auto pack = packs.at(index).toObject();
            const QString path = pack.value(QStringLiteral("path")).toString();
            EXPECT_FALSE(paths.contains(path));
            paths.insert(path);
            const auto& record = bundle.orderedPacks.at(static_cast<std::size_t>(index));
            EXPECT_EQ(path, record.path);
            const QByteArray contents = readFileBytes(QDir(root).filePath(path));
            EXPECT_EQ(contents.size(), record.size);
            EXPECT_EQ(pack.value(QStringLiteral("size")).toInteger(), record.size);
            const QString sha256 = QString::fromLatin1(
                QCryptographicHash::hash(contents, QCryptographicHash::Sha256).toHex());
            EXPECT_EQ(sha256, record.sha256);
            EXPECT_EQ(pack.value(QStringLiteral("sha256")).toString(), sha256);
        }
        EXPECT_EQ(paths, expectedPaths);
        for (const QString& language : project.projectLanguages) {
            const QString platformRoot = QDir(root).filePath(prefix);
            EXPECT_FALSE(QFileInfo::exists(QDir(platformRoot).filePath(QStringLiteral("HarkenEditorWaveforms.json"))));
            const QJsonArray selected = readPublication(platformRoot, language);
            const auto spoken = findByName(selected, QStringLiteral("Spoken"));
            const auto waves = spoken.value(QStringLiteral("packMetadata")).toObject()
                .value(QStringLiteral("waveforms")).toArray();
            ASSERT_EQ(waves.size(), 1);
            const auto wave = waves.first().toObject();
            const QDir packMetadataDir(QFileInfo(QDir(platformRoot).filePath(
                spoken.value(QStringLiteral("path")).toString())).absolutePath());
            const QString waveformPath = QDir::cleanPath(packMetadataDir.absoluteFilePath(
                wave.value(QStringLiteral("file")).toString()));
            EXPECT_EQ(QFileInfo(waveformPath).absolutePath(),
                QDir(platformRoot).absoluteFilePath(QStringLiteral("Waveforms")));
            const auto sourceId = language == QStringLiteral("English") ? englishSourceId : japaneseSourceId;
            EXPECT_EQ(readFileBytes(waveformPath), readFileBytes(harken::studio::WaveformCache::cachePath(
                harken::studio::AssetRegistry::instance().getAsset(sourceId)->absolutePath)));
            const auto selectedInit = findByName(selected, QStringLiteral("Init"));
            expectCanonicalPackParsesWithPartition(QDir(platformRoot).filePath(
                selectedInit.value(QStringLiteral("path")).toString()));
            const QByteArray contents = readFileBytes(QDir(root).filePath(
                prefix + language + QStringLiteral("/Spoken.bytes")));
            harken::studio::build::PackCanonicalImage image;
            ASSERT_EQ(VN_OK, harken::studio::build::ParsePackCanonicalImage(
                reinterpret_cast<const std::uint8_t*>(contents.constData()),
                static_cast<std::uint64_t>(contents.size()), &image));
            ASSERT_EQ(image.media.size(), 1u);
            const auto& media = image.media.front();
            ASSERT_EQ(media.channelCount, 1u);
            ASSERT_EQ(media.sampleFormat, HK_PACK_SAMPLE_FORMAT_F32_PLANAR);
            ASSERT_LE(media.blobOffset + 2 * sizeof(float),
                      static_cast<std::uint64_t>(contents.size()));
            float sample = 0.0f;
            std::memcpy(&sample, contents.constData() + media.blobOffset + sizeof(float),
                        sizeof(sample));
            EXPECT_FLOAT_EQ(sample, (language == QStringLiteral("English") ? 0.5f : 0.875f) / 16.0f);
        }
    }
    harken::studio::AssetRegistry::instance().clear();
}

TEST(ProjectPackExport, RuntimeBundleKeepsAllActionsAvailable)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    harken::studio::AssetRegistry::instance().clear();
    harken::studio::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t commonSourceId =
        registerToneAsset(dir.path(), QStringLiteral("common.wav"), 1.0f);
    const uint32_t sourceId = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), commonSourceId, sourceId,
        {{QStringLiteral("One"), 21, kSoloSoundObjectId},
         {QStringLiteral("Two"), 22, kSharedSoundObjectId}}));
    project.projectLanguages = {QStringLiteral("English"), QStringLiteral("ja")};
    const auto bundle = harken::studio::RuntimeContentBundleBuilder::build(
        project, QDir(dir.path()).filePath(QStringLiteral("published")),
        HarkenTest::platformIdString("Windows"));
    ASSERT_TRUE(bundle.success) << bundle.error.toStdString();
    ASSERT_EQ(bundle.orderedPacks.size(), 3u);
    ASSERT_EQ(bundle.actions.size(), 2u);
    std::map<harken::studio::types::ObjectId, QString> actions;
    for (const auto& action : bundle.actions)
        actions.emplace(action.stableId, action.name);
    EXPECT_EQ(actions, (std::map<harken::studio::types::ObjectId, QString>{
        {harkentest::StudioObjectId(21 + kStableIdBias), QStringLiteral("One")},
        {harkentest::StudioObjectId(22 + kStableIdBias), QStringLiteral("Two")}}));
    QSet<QString> paths;
    for (const auto& pack : bundle.orderedPacks) {
        const QString relative = pack.path.mid(QStringLiteral("Bundles/%1/").arg(bundle.bundleId).size());
        paths.insert(relative);
        EXPECT_TRUE(QFileInfo::exists(QDir(bundle.bundleDirectory).filePath(relative)));
    }
    EXPECT_EQ(paths, (QSet<QString>{QStringLiteral("Init.bytes"),
                                  QStringLiteral("One.bytes"), QStringLiteral("Two.bytes")}));
    harken::studio::AssetRegistry::instance().clear();
}

TEST(ProjectPackExport, ExternalLocalizedOccurrenceKeepsInvalidOwnershipRejected)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    harken::studio::AssetRegistry::instance().clear();
    harken::studio::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("External"), 21, 3}}));
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, soloSourceId));
    auto container = makeItem(QStringLiteral("ExternalVoice"), harken::studio::ProjectItem::Type::Container, 3);
    container->comp<harken::studio::VSelection>()->selections.shared.occurrenceOrdinalHighWater = 1;
    EXPECT_TRUE(container->comp<harken::studio::VSelection>()->selections.shared.setOccurrenceRows({{project.layers.front().rootItem->children.at(1)->guid, 0, {}}}));
    addChild(project.layers.front().rootItem, container);
    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const QString outputDir = QDir(dir.path()).filePath(QStringLiteral("out"));
    const auto result = exporter.ExportAllPacks(project, outputDir, opts);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("direct child")))
        << result.error.toStdString();
    EXPECT_TRUE(container->children.empty());
    QDirIterator packs(outputDir, {"*.bytes"}, QDir::Files, QDirIterator::Subdirectories);
    ASSERT_TRUE(packs.hasNext());
    EXPECT_EQ(packs.next(), QDir(outputDir).filePath(QStringLiteral("Init.bytes")));
    EXPECT_FALSE(packs.hasNext());
    ASSERT_NO_FATAL_FAILURE(expectCanonicalPackParsesWithPartition(
        QDir(outputDir).filePath(QStringLiteral("Init.bytes"))));
    EXPECT_FALSE(QFileInfo::exists(
        QDir(outputDir).filePath(QStringLiteral("HarkenPackManifest.json"))));
    harken::studio::AssetRegistry::instance().clear();
}

TEST(ProjectPackExport, LocalizedSinglePackRequiresExplicitLanguage)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    harken::studio::AssetRegistry::instance().clear();
    harken::studio::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("Spoken"), 21, kSoloSoundObjectId}}));
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, soloSourceId));
    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    std::vector<harken::studio::bridge::PackBuildSession::CanonicalPackEntry> entries;
    const QString path = QDir(dir.path()).filePath(QStringLiteral("single.bytes"));
    auto result = exporter.ExportActionPack(project, QStringLiteral("Spoken"), path, opts, entries);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("requires an explicit project language")))
        << result.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(path));
    opts.language = QStringLiteral("english");
    result = exporter.ExportActionPack(project, QStringLiteral("Spoken"), path, opts, entries);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("not a project language")))
        << result.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(path));
    opts.language = QStringLiteral("ja");
    result = exporter.ExportActionPack(project, QStringLiteral("Spoken"), path, opts, entries);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    EXPECT_EQ(result.mediaCount, 1);
    EXPECT_TRUE(std::any_of(entries.begin(), entries.end(), [](const auto& entry) {
        return entry.kind == HK_PACK_ENTRY_KIND_MEDIA;
    }));
    harken::studio::AssetRegistry::instance().clear();
}

TEST(ProjectPackExport, MissingLocalizedActiveRejectsUnavailableContent)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    harken::studio::AssetRegistry::instance().clear();
    harken::studio::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("Spoken"), 21, kSoloSoundObjectId}}));
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, soloSourceId));
    auto* sound = project.layers.front().rootItem->children.at(1)->comp<harken::studio::VSound>();
    sound->sources.localized["ja"].platforms.clear();
    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const QString outputDir = QDir(dir.path()).filePath(QStringLiteral("out"));
    auto result = exporter.ExportAllPacks(project, outputDir, opts);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("source active is not configured for %1/ja").arg(opts.platform)))
        << result.error.toStdString();
    ASSERT_NO_FATAL_FAILURE(expectCanonicalPackParsesWithPartition(QDir(outputDir).filePath(QStringLiteral("Init.bytes"))));
    EXPECT_FALSE(QFileInfo::exists(QDir(outputDir).filePath(QStringLiteral("ja/Spoken.bytes"))));
    ASSERT_NO_FATAL_FAILURE(expectCanonicalPackParsesWithPartition(QDir(outputDir).filePath(QStringLiteral("English/Spoken.bytes"))));
    EXPECT_FALSE(QFileInfo::exists(
        QDir(outputDir).filePath(QStringLiteral("HarkenPackManifest.json"))));
    opts.language = QStringLiteral("ja");
    std::vector<harken::studio::bridge::PackBuildSession::CanonicalPackEntry> entries;
    const QString path = QDir(dir.path()).filePath(QStringLiteral("single.bytes"));
    result = exporter.ExportActionPack(project, QStringLiteral("Spoken"), path, opts, entries);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("source active is not configured for %1/ja").arg(opts.platform)))
        << result.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(path));
    harken::studio::AssetRegistry::instance().clear();
}


TEST(ProjectPackExport, InvalidRequestedLanguageRejectsUnavailableContent)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    harken::studio::AssetRegistry::instance().clear();
    harken::studio::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("Spoken"), 21, kSoloSoundObjectId}}));
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, soloSourceId));

    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    for (const QString& language : {QStringLiteral("French"), QStringLiteral("english"),
                                    QStringLiteral("English")}) {
        SCOPED_TRACE(language.toStdString());
        if (language == QStringLiteral("English"))
            project.projectLanguages.clear();
        opts.language = language;
        const QString outputDir = QDir(dir.path()).filePath(language);
        const auto result = exporter.ExportAllPacks(project, outputDir, opts);
        EXPECT_FALSE(result.success);
        EXPECT_TRUE(result.error.contains(QStringLiteral("not a project language")))
            << result.error.toStdString();
        EXPECT_EQ(QDir(outputDir).entryList({QStringLiteral("*.bytes")}, QDir::Files),
                  QStringList{QStringLiteral("Init.bytes")});
        EXPECT_FALSE(QFileInfo::exists(
            QDir(outputDir).filePath(QStringLiteral("HarkenPackManifest.json"))));
    }
    harken::studio::AssetRegistry::instance().clear();
}

TEST(ProjectPackExport, SelectedPostActionChainWithoutLanguagesRejectsUnavailableContent)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    harken::studio::AssetRegistry::instance().clear();
    harken::studio::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("Root"), 21, kSharedSoundObjectId},
         {QStringLiteral("Spoken"), 22, kSoloSoundObjectId}}));
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, soloSourceId));
    project.projectLanguages.clear();
    harken::studio::VActionStep post;
    post.type = harken::studio::VActionStep::Type::PostAction;
    post.targetObjectID = 22;
    project.actions.front().rootItem->children.front()->comp<harken::studio::VAction>()->steps = {post};

    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    opts.onlyUnits = {QStringLiteral("Root")};
    const QString outputDir = harken::studio::ProjectPackExporter::platformOutputDir(dir.path(), *harken::studio::ProjectPlatformSemantics::find(project.platforms, QUuid(opts.platform)));
    const auto result = exporter.ExportAllPacks(project, outputDir, opts);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("project has no languages")))
        << result.error.toStdString();
    EXPECT_TRUE(result.error.contains(harken::studio::objectIdString(harkentest::StudioObjectId(21 + kStableIdBias))))
        << result.error.toStdString();
    ASSERT_NO_FATAL_FAILURE(expectCanonicalPackParsesWithPartition(QDir(outputDir).filePath(QStringLiteral("Init.bytes"))));
    EXPECT_FALSE(QFileInfo::exists(QDir(outputDir).filePath(QStringLiteral("ja/Spoken.bytes"))));
    EXPECT_FALSE(QFileInfo::exists(QDir(outputDir).filePath(QStringLiteral("Root.bytes"))));
    EXPECT_FALSE(QFileInfo::exists(
        QDir(outputDir).filePath(QStringLiteral("HarkenPackManifest.json"))));
    harken::studio::AssetRegistry::instance().clear();
}


namespace {


void verifyIncrementalPublication(const QString& directory)
{
    const QDir dir(directory);
    auto& registry = harken::studio::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(dir.path());
    const auto tone = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.2f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), tone, tone,
        {{QStringLiteral("One"), 21, kSharedSoundObjectId},
         {QStringLiteral("Two"), 22, kSoloSoundObjectId}}));
    harken::studio::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const auto outputRoot = dir.filePath("exports");
    const QDir live(QDir(outputRoot).filePath("Windows"));
    const auto mediaReport = [](const harken::studio::ProjectPackExporter::ExportResult& result) {
        QMap<QByteArray, QJsonObject> rows;
        for (const auto& value : result.media) {
            const auto row = value.toObject();
            const auto identity = QJsonDocument(QJsonArray{
                row.value(QStringLiteral("pack")),
                row.value(QStringLiteral("soundId")),
                row.value(QStringLiteral("sourceEntryId")),
                row.value(QStringLiteral("language"))}).toJson(QJsonDocument::Compact);
            EXPECT_FALSE(rows.contains(identity));
            rows.insert(identity, row);
        }
        return rows;
    };
    const auto initial = buildPersistedPackSet(project, outputRoot, opts);
    ASSERT_TRUE(initial.success) << initial.error.toStdString();
    EXPECT_EQ(initial.builtPackCount, 3);
    const auto initialMedia = mediaReport(initial);
    QMap<QString, qsizetype> reportedMediaCounts;
    for (const auto& row : initialMedia)
        ++reportedMediaCounts[row.value(QStringLiteral("pack")).toString()];
    for (const auto& value : initial.outputs) {
        const auto path = value.toObject().value(QStringLiteral("path")).toString();
        EXPECT_EQ(reportedMediaCounts.value(path),
            packKindCount(live.filePath(path), HK_PACK_ENTRY_KIND_MEDIA));
    }
    QMap<QString, QDateTime> timestamps;
    for (const auto& name : publicationFiles(live.path()))
        timestamps.insert(name, QFileInfo(live.filePath(name)).lastModified());
    std::vector<harken::studio::ProjectPackExporter::PackBuildRecord> records;
    const auto unchanged = buildPersistedPackSet(project, outputRoot, opts, &records);
    ASSERT_TRUE(unchanged.success) << unchanged.error.toStdString();
    EXPECT_EQ(unchanged.builtPackCount, 0);
    EXPECT_EQ(unchanged.skippedPackCount, 3);
    EXPECT_EQ(mediaReport(unchanged), initialMedia);
    ASSERT_EQ(records.size(), 3u);
    for (const auto& record : records) {
        EXPECT_TRUE(record.skipped);
        EXPECT_EQ(QFileInfo(record.outputPath).lastModified(), timestamps.value(live.relativeFilePath(record.outputPath)));
    }
    for (const auto& name : publicationFiles(live.path()))
        EXPECT_EQ(QFileInfo(live.filePath(name)).lastModified(), timestamps.value(name));
    opts.selection.actionTargeting = {{harkentest::StudioObjectId(22 + kStableIdBias), false, {}}};
    const auto deselected = buildPersistedPackSet(project, outputRoot, opts);
    ASSERT_TRUE(deselected.success) << deselected.error.toStdString();
    EXPECT_TRUE(findByName(readPublication(live.path(), {}, true, deselected.artifacts), "Two").isEmpty());
    EXPECT_EQ(QFileInfo(live.filePath("Two.bytes")).lastModified(), timestamps.value(QStringLiteral("Two.bytes")));
    EXPECT_EQ(QFileInfo(live.filePath("Two.json")).lastModified(), timestamps.value(QStringLiteral("Two.json")));
    opts.selection.actionTargeting.clear();
    const auto reselected = buildPersistedPackSet(project, outputRoot, opts);
    ASSERT_TRUE(reselected.success) << reselected.error.toStdString();
    EXPECT_EQ(reselected.builtPackCount, 0);
    EXPECT_EQ(reselected.skippedPackCount, 3);
    EXPECT_EQ(mediaReport(reselected), initialMedia);
    EXPECT_EQ(QFileInfo(live.filePath("Two.bytes")).lastModified(), timestamps.value(QStringLiteral("Two.bytes")));
    const auto oldOne = readFileBytes(live.filePath("One.bytes"));
    project.actions.front().rootItem->children.front()->comp<harken::studio::VAction>()->steps.front().delay = 0.1f;
    const auto changed = buildPersistedPackSet(project, outputRoot, opts);
    ASSERT_TRUE(changed.success) << changed.error.toStdString();
    EXPECT_EQ(changed.builtPackCount, 1);
    EXPECT_EQ(changed.skippedPackCount, 2);
    EXPECT_EQ(mediaReport(changed), initialMedia);
    EXPECT_NE(readFileBytes(live.filePath("One.bytes")), oldOne);
    EXPECT_EQ(QFileInfo(live.filePath("Two.bytes")).lastModified(), timestamps.value(QStringLiteral("Two.bytes")));
    const auto validTwo = readFileBytes(live.filePath("Two.bytes"));
    for (const auto& damaged : {QByteArray("invalid"), validTwo.chopped(1)}) {
        writeFileBytes(live.filePath("Two.bytes"), damaged);
        ASSERT_TRUE(QFile::remove(live.filePath("One.bytes")));
        const auto repaired = buildPersistedPackSet(project, outputRoot, opts);
        ASSERT_TRUE(repaired.success) << repaired.error.toStdString();
        EXPECT_EQ(repaired.builtPackCount, 2);
        EXPECT_EQ(readFileBytes(live.filePath("Two.bytes")), validTwo);
    }
    const auto replacement = registerToneAsset(dir.path(), QStringLiteral("replacement.wav"), 0.7f);
    harken::studio::AudioSourceData source;
    source.sourceID = replacement;
    source.relativePath = QStringLiteral("replacement.wav");
    project.audioSources.push_back(source);
    HarkenTest::setSharedSoundSource(*project.layers.front().rootItem->children.front(), replacement);
    const auto beforeMediaChange = readFileBytes(live.filePath("One.bytes"));
    const auto mediaChanged = buildPersistedPackSet(project, outputRoot, opts);
    ASSERT_TRUE(mediaChanged.success) << mediaChanged.error.toStdString();
    EXPECT_EQ(mediaChanged.builtPackCount, 1);
    EXPECT_EQ(mediaChanged.skippedPackCount, 2);
    EXPECT_NE(readFileBytes(live.filePath("One.bytes")), beforeMediaChange);
    EXPECT_EQ(readFileBytes(live.filePath("Two.bytes")), validTwo);
    for (const auto& name : publicationFiles(live.path())) {
        if (!name.endsWith(QStringLiteral(".bytes"))) continue;
        auto outdated = readFileBytes(live.filePath(name));
        ASSERT_GE(outdated.size(), harken::studio::build::kPackBytes<HkPackFileHeader>);
        harken::studio::build::StoreLE32(reinterpret_cast<std::uint8_t*>(outdated.data()) + 4,
                          HK_PACK_FORMAT_VERSION + 1);
        writeFileBytes(live.filePath(name), outdated);
    }
    const auto rebuilt = buildPersistedPackSet(project, outputRoot, opts);
    ASSERT_TRUE(rebuilt.success) << rebuilt.error.toStdString();
    EXPECT_EQ(rebuilt.builtPackCount, 3);
    EXPECT_EQ(rebuilt.skippedPackCount, 0);
    EXPECT_EQ(readFileBytes(live.filePath("Two.bytes")), validTwo);
    opts.forceReexport = true;
    const auto forced = buildPersistedPackSet(project, outputRoot, opts);
    ASSERT_TRUE(forced.success) << forced.error.toStdString();
    EXPECT_EQ(forced.builtPackCount, 3);
    EXPECT_EQ(forced.skippedPackCount, 0);
    EXPECT_NE(QFileInfo(live.filePath("Init.bytes")).lastModified(), timestamps.value(QStringLiteral("Init.bytes")));
    registry.clear();
}

}

TEST(ProjectPackExport, IncrementalPublicationPreservesUnchangedFilesAndRepairsInvalidPacks)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_NO_FATAL_FAILURE(verifyIncrementalPublication(dir.path()));
}

namespace {

void verifyPartialPublication(const QString& directory, void (*verifyActivation)(const QString&))
{
    const QDir dir(directory);
    auto& registry = harken::studio::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(dir.path());
    const auto tone = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.2f);
    const auto privateTone = registerToneAsset(dir.path(), QStringLiteral("private.wav"), 0.4f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), tone, privateTone,
        {{QStringLiteral("One"), 21, kSharedSoundObjectId}, {QStringLiteral("Two"), 22, kSharedSoundObjectId}}));
    auto privateLayer = makeItem(QStringLiteral("PrivateLayer"), harken::studio::ProjectItem::Type::Layer, 60);
    privateLayer->fieldGroups = project.layers.front().rootItem->fieldGroups;
    auto privateSound = project.layers.front().rootItem->children.back();
    project.layers.front().rootItem->children.pop_back();
    addChild(privateLayer, privateSound);
    project.layers.push_back({privateLayer, QStringLiteral("Layers")});
    auto& actions = project.actions.front().rootItem->children;
    auto privatePlay = actions.back()->comp<harken::studio::VAction>()->steps.front();
    privatePlay.targetObjectID = kSoloSoundObjectId;
    actions.back()->comp<harken::studio::VAction>()->steps.push_back(privatePlay);
    auto variable = makeItem(QStringLiteral("Drive"), harken::studio::ProjectItem::Type::Variable, 50);
    auto* definition = variable->comp<harken::studio::VVariable>();
    definition->kind = harken::studio::VariableKind::Smooth;
    definition->rangeMin = 0.0f;
    definition->rangeMax = 1.0f;
    definition->defaultValue = 0.0f;
    project.variables.push_back({variable, QStringLiteral("Variables")});
    harken::studio::ParameterBindingDef binding;
    binding.guid = harken::studio::authoringUuid(harkentest::StudioObjectId(500));
    binding.variable = variable->guid;
    binding.address = harken::studio::BindingSemantics::volumeAddress();
    binding.curve = {{0.0f, -6.0f}, {1.0f, 0.0f}};
    binding.outputMin = -12.0f;
    binding.outputMax = 0.0f;
    auto* mix = project.layers.front().rootItem->comp<harken::studio::VMixNode>();
    ASSERT_TRUE(mix->bindings.upsert(binding));
    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const auto root = dir.filePath("exports");
    const QDir live(QDir(root).filePath("Windows"));
    std::vector<harken::studio::ProjectPackExporter::PackBuildRecord> initialRecords;
    const auto initial = buildPersistedPackSet(project, root, opts, &initialRecords);
    ASSERT_TRUE(initial.success) << initial.error.toStdString();
    ASSERT_NO_FATAL_FAILURE(verifyActivation(live.path()));
    const auto statePath = QDir(harken::studio::PathUtils::getDotCacheDir(dir.path()))
        .filePath(QStringLiteral("pack/builds.json"));
    auto state = QJsonDocument::fromJson(readFileBytes(statePath)).object();
    auto targets = state.value(QStringLiteral("targets")).toObject();
    auto target = targets.value(live.absolutePath()).toObject();
    auto savedPacks = target.value(QStringLiteral("packs")).toObject();
    auto legacyPack = savedPacks.value(QStringLiteral("Two.bytes")).toObject();
    ASSERT_TRUE(legacyPack.value(QStringLiteral("media")).isArray());
    legacyPack.remove(QStringLiteral("media"));
    savedPacks.insert(QStringLiteral("Two.bytes"), legacyPack);
    target.insert(QStringLiteral("packs"), savedPacks);
    targets.insert(live.absolutePath(), target);
    state.insert(QStringLiteral("targets"), targets);
    writeFileBytes(statePath, QJsonDocument(state).toJson());
    auto partialOptions = opts;
    partialOptions.updateSelectedOnly = true;
    partialOptions.onlyPackPaths = {QStringLiteral("One")};
    const auto legacyPartial = buildPersistedPackSet(project, root, partialOptions);
    ASSERT_TRUE(legacyPartial.success) << legacyPartial.error.toStdString();
    bool retainedSeen = false;
    for (const auto& value : legacyPartial.outputs) {
        const auto output = value.toObject();
        if (output.value(QStringLiteral("path")).toString() != QStringLiteral("Two.bytes")) continue;
        retainedSeen = true;
        EXPECT_FALSE(output.contains(QStringLiteral("media")));
    }
    EXPECT_TRUE(retainedSeen);
    ASSERT_FALSE(legacyPartial.media.isEmpty());
    for (const auto& value : legacyPartial.media)
        EXPECT_EQ(value.toObject().value(QStringLiteral("pack")).toString(), QStringLiteral("One.bytes"));
    const auto recovered = buildPersistedPackSet(project, root, opts);
    ASSERT_TRUE(recovered.success) << recovered.error.toStdString();
    ASSERT_EQ(recovered.media.size(), initial.media.size());
    for (const auto& value : initial.media)
        EXPECT_TRUE(recovered.media.contains(value));
    const auto old = readPublication(live.path());
    QSet<QString> initialKeys;
    QSet<QString> selectedKeys;
    for (const auto& artifact : initial.artifacts) {
        initialKeys.insert(artifact.unitKey);
        if (artifact.packName == QStringLiteral("Init") || artifact.packName == QStringLiteral("One"))
            selectedKeys.insert(artifact.unitKey);
    }
    EXPECT_EQ(initialKeys.size(), old.size());
    ASSERT_FALSE(initialRecords.empty());
    for (const auto& record : initialRecords) EXPECT_GE(record.elapsedMs, 0);
    const auto oldTwo = readFileBytes(live.filePath("Two.bytes"));
    const auto oldTwoPackMetadata = readFileBytes(live.filePath("Two.json"));
    const auto oldTwoPackMetadataTime = QFileInfo(live.filePath("Two.json")).lastModified();
    const auto oldInitPackMetadata = readFileBytes(live.filePath("Init.json"));
    const auto oldInitPackMetadataTime = QFileInfo(live.filePath("Init.json")).lastModified();
    const auto oldTime = QFileInfo(live.filePath("Two.bytes")).lastModified();
    const auto oldInit = readFileBytes(live.filePath("Init.bytes"));
    const auto oldInitTime = QFileInfo(live.filePath("Init.bytes")).lastModified();
    actions.front()->comp<harken::studio::VAction>()->steps.front().delay = 0.1f;
    actions.back()->comp<harken::studio::VAction>()->steps.front().delay = 0.3f;
    actions.back()->name = QStringLiteral("EditedTwo");
    opts.selection.actionTargeting = {{actions.back()->runtimeId(), false, {}}};
    opts.updateSelectedOnly = true;
    opts.onlyPackPaths = {QStringLiteral("One")};
    opts.selfCleaning = true;
    for (const bool force : {false, true}) {
        opts.forceReexport = force;
        std::vector<harken::studio::ProjectPackExporter::PackBuildRecord> records;
        const auto partial = exporter.ExportAllPacks(project, live.path(), opts, &records);
        ASSERT_TRUE(partial.success) << partial.error.toStdString();
        for (const auto& warning : partial.warnings)
            EXPECT_TRUE(initial.warnings.contains(warning)) << warning.toStdString();
        EXPECT_EQ(partial.builtPackCount, 1);
        EXPECT_EQ(partial.removedPackCount, 0);
        EXPECT_EQ(readFileBytes(live.filePath("Two.bytes")), oldTwo);
        EXPECT_EQ(readFileBytes(live.filePath("Two.json")), oldTwoPackMetadata);
        EXPECT_EQ(QFileInfo(live.filePath("Two.json")).lastModified(), oldTwoPackMetadataTime);
        EXPECT_EQ(readFileBytes(live.filePath("Init.json")), oldInitPackMetadata);
        EXPECT_EQ(QFileInfo(live.filePath("Init.json")).lastModified(), oldInitPackMetadataTime);
        EXPECT_EQ(QFileInfo(live.filePath("Two.bytes")).lastModified(), oldTime);
        EXPECT_EQ(readFileBytes(live.filePath("Init.bytes")), oldInit);
        const auto packs = readPublication(live.path(), {}, true, partial.artifacts);
        QSet<QString> publishedPaths;
        for (const auto& value : packs)
            publishedPaths.insert(live.filePath(value.toObject().value("path").toString()));
        QSet<QString> artifactPaths;
        QSet<QString> artifactKeys;
        for (const auto& artifact : partial.artifacts) {
            artifactPaths.insert(artifact.outputPath);
            artifactKeys.insert(artifact.unitKey);
            EXPECT_EQ(artifact.modified, QFileInfo(artifact.outputPath).lastModified());
            EXPECT_EQ(artifact.bytes, QFileInfo(artifact.outputPath).size());
        }
        EXPECT_EQ(artifactPaths, publishedPaths);
        EXPECT_EQ(artifactKeys, selectedKeys);
        ASSERT_FALSE(records.empty());
        EXPECT_LT(records.size(), static_cast<std::size_t>(partial.artifacts.size()));
        for (const auto& record : records) EXPECT_GE(record.elapsedMs, 0);
        EXPECT_TRUE(findByName(packs, "Two").isEmpty());
        const auto inventory = readPublication(live.path(), {}, false);
        EXPECT_EQ(findByName(inventory, "Two"), findByName(old, "Two"));
        EXPECT_EQ(findByName(actionPackMetadata(inventory), "Two"),
            findByName(actionPackMetadata(old), "Two"));
        ASSERT_NO_FATAL_FAILURE(verifyActivation(live.path()));
    }
    const auto one = readFileBytes(live.filePath("One.bytes"));
    binding.curve.front().value = -12.0f;
    ASSERT_TRUE(mix->bindings.upsert(binding));
    auto* master = project.busses.front().rootItem.get();
    master->comp<harken::studio::VMixNode>()->volume = -6.0f;
    master->playablePropertyPresence.Set(harkenplayableproperties::PlayablePropertyIdV1::Volume);
    const auto sharedChange = buildPersistedPackSet(project, root, opts);
    ASSERT_TRUE(sharedChange.success) << sharedChange.error.toStdString();
    for (const auto& warning : sharedChange.warnings)
        EXPECT_TRUE(initial.warnings.contains(warning)) << warning.toStdString();
    EXPECT_EQ(sharedChange.builtPackCount, 1);
    EXPECT_EQ(sharedChange.removedPackCount, 0);
    EXPECT_NE(readFileBytes(live.filePath("One.bytes")), one);
    EXPECT_EQ(readFileBytes(live.filePath("Two.bytes")), oldTwo);
    EXPECT_EQ(readFileBytes(live.filePath("Two.json")), oldTwoPackMetadata);
    EXPECT_EQ(QFileInfo(live.filePath("Two.json")).lastModified(), oldTwoPackMetadataTime);
    EXPECT_EQ(readFileBytes(live.filePath("Init.json")), oldInitPackMetadata);
    EXPECT_EQ(QFileInfo(live.filePath("Init.json")).lastModified(), oldInitPackMetadataTime);
    EXPECT_EQ(QFileInfo(live.filePath("Two.bytes")).lastModified(), oldTime);
    EXPECT_EQ(readFileBytes(live.filePath("Init.bytes")), oldInit);
    EXPECT_EQ(QFileInfo(live.filePath("Init.bytes")).lastModified(), oldInitTime);
    const auto packs = readPublication(live.path(), {}, true, sharedChange.artifacts);
    EXPECT_EQ(findByName(packs, "Init"), findByName(old, "Init"));
    EXPECT_TRUE(findByName(packs, "Two").isEmpty());
    const auto inventory = readPublication(live.path(), {}, false);
    EXPECT_EQ(findByName(inventory, "Two"), findByName(old, "Two"));
    EXPECT_EQ(findByName(actionPackMetadata(inventory), "Two"),
        findByName(actionPackMetadata(old), "Two"));
    actions.back()->comp<harken::studio::VAction>()->steps.front().targetObjectID = kSoloSoundObjectId;
    ASSERT_TRUE(QFile::remove(registry.getAsset(privateTone)->absolutePath));
    const auto unfinishedDraft = buildPersistedPackSet(project, root, opts);
    ASSERT_TRUE(unfinishedDraft.success) << unfinishedDraft.error.toStdString();
    for (const auto& warning : unfinishedDraft.warnings)
        EXPECT_TRUE(initial.warnings.contains(warning)) << warning.toStdString();
    EXPECT_EQ(unfinishedDraft.builtPackCount, 1);
    EXPECT_EQ(readFileBytes(live.filePath("Two.bytes")), oldTwo);
    EXPECT_EQ(readFileBytes(live.filePath("Two.json")), oldTwoPackMetadata);
    EXPECT_EQ(QFileInfo(live.filePath("Two.json")).lastModified(), oldTwoPackMetadataTime);
    EXPECT_EQ(readFileBytes(live.filePath("Init.json")), oldInitPackMetadata);
    EXPECT_EQ(QFileInfo(live.filePath("Init.json")).lastModified(), oldInitPackMetadataTime);
    EXPECT_EQ(QFileInfo(live.filePath("Two.bytes")).lastModified(), oldTime);
    EXPECT_EQ(readFileBytes(live.filePath("Init.bytes")), oldInit);
    registry.clear();
}

}

TEST(ProjectPackExport, PartialPublicationRetainsOtherPackPairs)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_NO_FATAL_FAILURE(verifyPartialPublication(dir.path(), [](const QString&) {}));
}

TEST(ProjectPackExport, PartialPublicationRefreshesEmbeddedClosureAndKeepsOtherPairs)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& registry = harken::studio::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(dir.path());
    const auto tone = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.2f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), tone, tone,
        {{QStringLiteral("Provider"), 21, kSharedSoundObjectId}, {QStringLiteral("Consumer"), 22, kSharedSoundObjectId}}));
    harken::studio::VActionStep post;
    post.type = harken::studio::VActionStep::Type::PostAction;
    post.targetObjectID = 21;
    auto& actions = project.actions.front().rootItem->children;
    actions.back()->comp<harken::studio::VAction>()->steps = {post};
    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const QDir live(dir.filePath("exports"));
    const auto initial = exporter.ExportAllPacks(project, live.path(), opts);
    ASSERT_TRUE(initial.success) << initial.error.toStdString();
    const auto packs = readPublication(live.path());
    const auto provider = readFileBytes(live.filePath("Provider.bytes"));
    const auto providerPackMetadata = readFileBytes(live.filePath("Provider.json"));
    const auto providerTime = QFileInfo(live.filePath("Provider.bytes")).lastModified();
    const auto consumer = readFileBytes(live.filePath("Consumer.bytes"));
    const auto init = readFileBytes(live.filePath("Init.bytes"));
    actions.front()->comp<harken::studio::VAction>()->steps.front().delay = 0.1f;
    opts.onlyPackPaths = {QStringLiteral("Consumer")};
    opts.updateSelectedOnly = true;
    const auto partial = exporter.ExportAllPacks(project, live.path(), opts);
    ASSERT_TRUE(partial.success) << partial.error.toStdString();
    for (const auto& warning : partial.warnings)
        EXPECT_TRUE(initial.warnings.contains(warning)) << warning.toStdString();
    EXPECT_EQ(partial.builtPackCount, 1);
    EXPECT_EQ(readFileBytes(live.filePath("Provider.bytes")), provider);
    EXPECT_EQ(readFileBytes(live.filePath("Provider.json")), providerPackMetadata);
    EXPECT_EQ(QFileInfo(live.filePath("Provider.bytes")).lastModified(), providerTime);
    EXPECT_NE(readFileBytes(live.filePath("Consumer.bytes")), consumer);
    EXPECT_EQ(readFileBytes(live.filePath("Init.bytes")), init);
    EXPECT_EQ(findByName(readPublication(live.path()), "Provider"),
        findByName(packs, "Provider"));
    actions.front()->comp<harken::studio::VAction>()->steps.front().delay = 0.0f;
    actions.back()->comp<harken::studio::VAction>()->steps = actions.front()->comp<harken::studio::VAction>()->steps;
    opts.selection.actionTargeting = {{actions.front()->runtimeId(), false, {}}};
    opts.updateSelectedOnly = false;
    opts.onlyPackPaths.clear();
    const QDir selectedLive(dir.filePath("selected-exports"));
    const auto selected = exporter.ExportAllPacks(project, selectedLive.path(), opts);
    ASSERT_TRUE(selected.success) << selected.error.toStdString();
    const auto selectedConsumer = readFileBytes(selectedLive.filePath("Consumer.bytes"));
    const auto selectedInit = readFileBytes(selectedLive.filePath("Init.bytes"));
    ASSERT_FALSE(QFileInfo::exists(selectedLive.filePath("Provider.bytes")));
    actions.back()->comp<harken::studio::VAction>()->steps = {post};
    opts.updateSelectedOnly = true;
    opts.onlyPackPaths = {QStringLiteral("Consumer")};
    const auto unpublished = exporter.ExportAllPacks(project, selectedLive.path(), opts);
    ASSERT_TRUE(unpublished.success) << unpublished.error.toStdString();
    for (const auto& warning : unpublished.warnings)
        EXPECT_TRUE(selected.warnings.contains(warning)) << warning.toStdString();
    EXPECT_EQ(unpublished.builtPackCount, 1);
    EXPECT_NE(readFileBytes(selectedLive.filePath("Consumer.bytes")), selectedConsumer);
    EXPECT_EQ(readFileBytes(selectedLive.filePath("Init.bytes")), selectedInit);
    EXPECT_FALSE(QFileInfo::exists(selectedLive.filePath("Provider.bytes")));
    EXPECT_FALSE(QFileInfo::exists(selectedLive.filePath("Provider.json")));
    const auto selectedPublication = readPublication(selectedLive.path());
    EXPECT_TRUE(findByName(selectedPublication, "Provider").isEmpty());
    EXPECT_EQ(findByName(selectedPublication, "Consumer").value("dependencies").toArray(),
        QJsonArray{findByName(selectedPublication, "Init").value("packStableId")});
    registry.clear();
}


namespace {
struct PackWriteAccessProject
{
    QTemporaryDir directory;
    harken::studio::ProjectData project;
    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions options;
    QString root;
    QString platform;
    QDir live;

    void setup(bool waveforms = false)
    {
        ASSERT_TRUE(directory.isValid());
        auto& registry = harken::studio::AssetRegistry::instance();
        registry.clear();
        registry.setProjectRoot(directory.path());
        const auto tone = registerToneAsset(directory.path(), "tone.wav", 0.2f);
        ASSERT_NO_FATAL_FAILURE(buildProject(project, directory.path(), tone, tone,
            {{QStringLiteral("One"), 21, kSharedSoundObjectId}}));
        root = directory.filePath("exports");
        platform = HarkenTest::platformIdString("Windows");
        live = QDir(QDir(root).filePath("Windows"));
        project.packBuildOutputDir = root;
        project.buildExportWaveforms = waveforms;
        project.buildWaveformOutputDir = QDir(root).filePath("Waveforms");
        options.platform = platform;
    }
    ~PackWriteAccessProject() { harken::studio::AssetRegistry::instance().clear(); }
};

QFileDevice::Permissions withoutWritePermissions(QFileDevice::Permissions permissions)
{
    return permissions & ~(QFileDevice::WriteOwner | QFileDevice::WriteUser
                           | QFileDevice::WriteGroup | QFileDevice::WriteOther);
}
}

TEST(ProjectPackExport, ReadOnlyUnchangedPackDoesNotNeedReplacement)
{
    for (const bool metadataChange : {false, true}) {
        SCOPED_TRACE(metadataChange);
        PackWriteAccessProject context;
        ASSERT_NO_FATAL_FAILURE(context.setup());
        const auto first = buildPersistedPackSet(context.project, context.root, context.options);
        ASSERT_TRUE(first.success) << first.error.toStdString();
        const auto unchanged = buildPersistedPackSet(context.project, context.root, context.options);
        ASSERT_TRUE(unchanged.success) << unchanged.error.toStdString();
        ASSERT_EQ(unchanged.builtPackCount, 0);
        std::map<QString, QFileDevice::Permissions> permissions;
        std::map<QString, QByteArray> before;
        const auto restore = qScopeGuard([&] {
            for (const auto& [path, value] : permissions) QFile::setPermissions(path, value);
        });
        for (const auto& name : context.live.entryList(QDir::Files)) {
            const auto path = context.live.filePath(name);
            permissions.emplace(path, QFile::permissions(path));
            before.emplace(path, readFileBytes(path));
            ASSERT_TRUE(QFile::setPermissions(path, withoutWritePermissions(permissions.at(path))));
        }
        for (const bool clean : {false, true}) {
            context.options.selfCleaning = clean;
            const auto readonly = buildPersistedPackSet(context.project, context.root, context.options);
            EXPECT_TRUE(readonly.success) << readonly.error.toStdString();
            EXPECT_EQ(readonly.builtPackCount, 0);
        }
        if (metadataChange) context.project.buildExportPackMetadata = true;
        else context.project.actions.front().rootItem->children.front()->comp<harken::studio::VAction>()->steps.front().delay = 0.1f;
        const auto changed = buildPersistedPackSet(context.project, context.root, context.options);
        EXPECT_FALSE(changed.success);
        EXPECT_TRUE(changed.error.contains(metadataChange ? QStringLiteral(".json") : QStringLiteral("One.bytes")))
            << changed.error.toStdString();
        for (const auto& [path, bytes] : before) {
            EXPECT_EQ(readFileBytes(path), bytes);
            EXPECT_EQ(QFile::permissions(path), withoutWritePermissions(permissions.at(path)));
        }
        EXPECT_FALSE(QFileInfo::exists(context.live.filePath(".harken-pack-publishing")));
    }
}

TEST(ProjectPackExport, CleanupMustAllowRetainedReadOnlyWaveform)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup(true));
    const auto first = buildPersistedPackSet(context.project, context.root, context.options);
    ASSERT_TRUE(first.success) << first.error.toStdString();
    QDir waveforms(context.project.buildWaveformOutputDir);
    const auto names = waveforms.entryList({"*.vkd"}, QDir::Files);
    ASSERT_FALSE(names.isEmpty());
    const auto path = waveforms.filePath(names.front());
    const auto permissions = QFile::permissions(path);
    const auto restore = qScopeGuard([&] { QFile::setPermissions(path, permissions); });
    ASSERT_TRUE(QFile::setPermissions(path, withoutWritePermissions(permissions)));
    context.options.selfCleaning = true;
    QString error;
    const auto publication = harken::studio::ProjectPackExporter::FinalizePackExport(
        context.root, context.project, {context.platform}, true, {{context.platform, first.outputs}});
    ASSERT_TRUE(publication.success) << publication.error.toStdString();
    ASSERT_TRUE(QFileInfo::exists(path));
    const auto exported = buildPersistedPackSet(context.project, context.root, context.options);
    ASSERT_TRUE(exported.success) << exported.error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(path));
    const QString obsolete = waveforms.filePath(QString(64, QLatin1Char('f')) + QStringLiteral(".vkd"));
    writeFileBytes(obsolete, "obsolete");
    const auto obsoletePermissions = QFile::permissions(obsolete);
    const auto restoreObsolete = qScopeGuard([&] { QFile::setPermissions(obsolete, obsoletePermissions); });
    ASSERT_TRUE(QFile::setPermissions(obsolete, withoutWritePermissions(obsoletePermissions)));
    const auto blockedCleanup = harken::studio::ProjectPackExporter::FinalizePackExport(
        context.root, context.project, {context.platform}, true, {{context.platform, exported.outputs}});
    EXPECT_FALSE(blockedCleanup.success);
    EXPECT_TRUE(QDir::fromNativeSeparators(blockedCleanup.error).contains(obsolete))
        << blockedCleanup.error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(path));
    EXPECT_EQ(readFileBytes(obsolete), QByteArray("obsolete"));
    EXPECT_FALSE(QFileInfo::exists(QDir(context.root).filePath(".harken-pack-publishing")));
}

TEST(ProjectPackExport, WaveformFailuresAreReportedAndRecoverOnTheNextRequest)
{
    for (const bool conflictingFile : {false, true}) {
        SCOPED_TRACE(conflictingFile);
        PackWriteAccessProject context;
        ASSERT_NO_FATAL_FAILURE(context.setup(true));
        const auto first = buildPersistedPackSet(context.project, context.root, context.options);
        ASSERT_TRUE(first.success) << first.error.toStdString();
        const QDir waveforms(context.project.buildWaveformOutputDir);
        const auto names = waveforms.entryList({"*.vkd"}, QDir::Files);
        ASSERT_FALSE(names.isEmpty());
        const QString blocked = conflictingFile ? waveforms.filePath(names.front())
            : context.directory.filePath(QStringLiteral("blocked-waveforms"));
        writeFileBytes(blocked, "occupied");
        const auto permissions = QFile::permissions(blocked);
        const auto restore = qScopeGuard([&] { QFile::setPermissions(blocked, permissions); });
        if (conflictingFile)
            ASSERT_TRUE(QFile::setPermissions(blocked, withoutWritePermissions(permissions)));
        if (!conflictingFile) context.options.waveformOutputDir = blocked;
        std::vector<harken::studio::ProjectPackExporter::PackBuildRecord> records;
        context.project.actions.front().rootItem->children.front()->comp<harken::studio::VAction>()->steps.front().delay = 0.2f;
        const auto result = buildPersistedPackSet(context.project, context.root, context.options, &records);
        EXPECT_FALSE(result.success);
        EXPECT_FALSE(result.error.isEmpty());
        EXPECT_FALSE(result.canceled);
        EXPECT_EQ(readFileBytes(blocked), QByteArray("occupied"));
        EXPECT_TRUE(std::any_of(records.begin(), records.end(), [](const auto& record) {
            return !record.success && !record.error.isEmpty();
        }));
        ASSERT_TRUE(QFile::setPermissions(blocked, permissions));
        ASSERT_TRUE(QFile::remove(blocked));
        context.options.waveformOutputDir.clear();
        const auto next = buildPersistedPackSet(context.project, context.root, context.options);
        ASSERT_TRUE(next.success) << next.error.toStdString();
        ASSERT_NO_FATAL_FAILURE(expectCanonicalPackParsesWithPartition(context.live.filePath(QStringLiteral("One.bytes"))));
        const auto publication = readPublication(context.live.path());
        const auto packMetadata = findByName(publication, QStringLiteral("One"))
            .value(QStringLiteral("packMetadata")).toObject();
        const auto references = packMetadata.value(QStringLiteral("waveforms")).toArray();
        ASSERT_FALSE(references.isEmpty());
        for (const auto& reference : references)
            EXPECT_TRUE(QFileInfo::exists(context.live.filePath(reference.toObject().value(QStringLiteral("file")).toString())));
    }
}

TEST(ProjectPackExport, ProtectedLegacyWaveformLocationsCannotWriteOrCleanProjectData)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    const QString projectRoot = directory.filePath(QStringLiteral("Project"));
    ASSERT_TRUE(QDir().mkpath(projectRoot));
    auto& registry = harken::studio::AssetRegistry::instance();
    registry.clear();
    const auto clearRegistry = qScopeGuard([&] { registry.clear(); });
    registry.setProjectRoot(projectRoot);
    const auto tone = registerToneAsset(projectRoot, QStringLiteral("tone.wav"), 0.2f);
    harken::studio::ProjectData project;
    ASSERT_NO_FATAL_FAILURE(buildProject(project, projectRoot, tone, tone,
        {{QStringLiteral("One"), 21, kSharedSoundObjectId}}));
    const QString sourcePath = QDir(projectRoot).filePath(QStringLiteral("AudioSources/tone.wav"));
    const QByteArray sourceBytes = readFileBytes(sourcePath);
    const QString outputRoot = directory.filePath(QStringLiteral("Published"));
    const QString platform = HarkenTest::platformIdString("Windows");
    const QDir live(QDir(outputRoot).filePath(QStringLiteral("Windows")));
    project.packBuildOutputDir = outputRoot;
    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions options;
    options.platform = platform;
    options.forceReexport = true;
    options.selfCleaning = true;
    for (const QString& destination : {QStringLiteral("AudioSources/Waveforms"), QStringLiteral("..")}) {
        project.buildWaveformOutputDir = destination;
        const QDir protectedDirectory(harken::studio::PathUtils::waveformBuildOutputRoot(project.projectPath, destination));
        ASSERT_TRUE(QDir().mkpath(protectedDirectory.path()));
        const QString sentinel = protectedDirectory.filePath(QString(64, QLatin1Char('f')) + QStringLiteral(".vkd"));
        writeFileBytes(sentinel, "preserved project data");
        const auto before = protectedDirectory.entryList({QStringLiteral("*.vkd")}, QDir::Files);
        for (const bool enabled : {false, true}) {
            project.buildExportWaveforms = enabled;
            const auto result = exporter.ExportAllPacks(project, {}, options);
            EXPECT_EQ(result.success, !enabled) << result.error.toStdString();
            if (enabled) EXPECT_FALSE(result.error.isEmpty());
            const auto publication = readPublication(live.path());
            ASSERT_EQ(publication.size(), 2);
            for (const auto& value : publication)
                EXPECT_FALSE(value.toObject().value(QStringLiteral("packMetadata")).toObject()
                    .contains(QStringLiteral("waveforms")));
            const auto finalized = harken::studio::ProjectPackExporter::FinalizePackExport(
                outputRoot, project, {platform}, !enabled, {{platform, result.outputs}});
            EXPECT_TRUE(finalized.success) << finalized.error.toStdString();
            EXPECT_EQ(readFileBytes(sourcePath), sourceBytes);
            EXPECT_EQ(readFileBytes(sentinel), QByteArray("preserved project data"));
            EXPECT_EQ(protectedDirectory.entryList({QStringLiteral("*.vkd")}, QDir::Files), before);
            EXPECT_FALSE(QFileInfo::exists(protectedDirectory.filePath(QStringLiteral(".harken-pack-waveforms.lock"))));
        }
    }
}

TEST(ProjectPackExport, BusyWaveformOutputFailsWithoutRetry)
{
    for (const int location : {0, 1, 2}) {
        PackWriteAccessProject context;
        ASSERT_NO_FATAL_FAILURE(context.setup(true));
        context.project.buildWaveformOutputDir = location == 0
            ? context.directory.filePath(QStringLiteral("external-waveforms"))
            : location == 1 ? context.live.filePath(QStringLiteral("Waveforms"))
                            : QDir(context.root).filePath(QStringLiteral("Waveforms"));
        const QDir waveforms(context.project.buildWaveformOutputDir);
        ASSERT_TRUE(QDir().mkpath(waveforms.path()));
        const QString otherWaveform = waveforms.filePath(QString(64, QLatin1Char('f')) + QStringLiteral(".vkd"));
        writeFileBytes(otherWaveform, "other build waveform");
        const QString lockPath = waveforms.filePath(QStringLiteral(".harken-pack-waveforms.lock"));
        QLockFile competingBuild(lockPath);
        competingBuild.setStaleLockTime(0);
        ASSERT_TRUE(competingBuild.tryLock());
        context.options.selfCleaning = true;
        QString error;
        const auto result = context.exporter.ExportAllPacks(context.project, {}, context.options);
        EXPECT_FALSE(result.success);
        EXPECT_FALSE(result.error.isEmpty());
        EXPECT_EQ(waveforms.entryList({QStringLiteral("*.vkd")}, QDir::Files).size(), 1);
        EXPECT_EQ(readFileBytes(otherWaveform), QByteArray("other build waveform"));
        EXPECT_TRUE(QFileInfo::exists(lockPath));
        context.project.buildExportWaveforms = false;
        const auto disabled = context.exporter.ExportAllPacks(context.project, {}, context.options);
        ASSERT_TRUE(disabled.success) << disabled.error.toStdString();
        const auto cleaned = harken::studio::ProjectPackExporter::FinalizePackExport(
            context.root, context.project, {context.platform}, true, {{context.platform, disabled.outputs}});
        EXPECT_TRUE(cleaned.success) << cleaned.error.toStdString();
        if (location != 0) EXPECT_FALSE(cleaned.warnings.isEmpty());
        EXPECT_EQ(cleaned.globalWarnings, cleaned.warnings);
        EXPECT_EQ(readFileBytes(otherWaveform), QByteArray("other build waveform"));
        competingBuild.unlock();
        context.project.buildExportWaveforms = true;
        const auto retry = context.exporter.ExportAllPacks(context.project, {}, context.options);
        ASSERT_TRUE(retry.success) << retry.error.toStdString();
        const auto packMetadata = findByName(readPublication(context.live.path()), QStringLiteral("One"))
            .value(QStringLiteral("packMetadata")).toObject();
        ASSERT_FALSE(packMetadata.value(QStringLiteral("waveforms")).toArray().isEmpty());
        for (const auto& value : packMetadata.value(QStringLiteral("waveforms")).toArray())
            EXPECT_TRUE(QFileInfo::exists(context.live.absoluteFilePath(
                value.toObject().value(QStringLiteral("file")).toString())));
        EXPECT_FALSE(QFileInfo::exists(lockPath));
        const auto cleanup = harken::studio::ProjectPackExporter::FinalizePackExport(
            context.root, context.project, {context.platform}, true, {{context.platform, retry.outputs}});
        EXPECT_TRUE(cleanup.success) << cleanup.error.toStdString();
        EXPECT_EQ(QFileInfo::exists(otherWaveform), location == 0);
    }
}


TEST(ProjectPackExport, CancellationStopsAtBuildStageBoundaries)
{
    for (const auto stage : {harken::studio::PackGenerationStage::Compiling,
                            harken::studio::PackGenerationStage::Writing}) {
        PackWriteAccessProject context;
        ASSERT_NO_FATAL_FAILURE(context.setup());
        const auto first = context.exporter.ExportAllPacks(context.project, {}, context.options);
        ASSERT_TRUE(first.success) << first.error.toStdString();
        QMap<QString, QByteArray> before;
        for (const auto& name : publicationFiles(context.live.path()))
            before.insert(name, readFileBytes(context.live.filePath(name)));
        bool canceled = false;
        int writeCount = 0;
        context.options.progressCallback = [&](const harken::studio::PackGenerationProgress& progress) {
            if (progress.stage == harken::studio::PackGenerationStage::Writing) ++writeCount;
            if (progress.stage == stage) canceled = true;
        };
        context.options.cancelCheck = [&] { return canceled; };
        const auto result = context.exporter.ExportAllPacks(context.project, {}, context.options);
        EXPECT_FALSE(result.success);
        EXPECT_TRUE(result.canceled);
        EXPECT_EQ(result.error, QStringLiteral("Export canceled"));
        if (stage == harken::studio::PackGenerationStage::Compiling) EXPECT_EQ(writeCount, 0);
        if (stage == harken::studio::PackGenerationStage::Writing) EXPECT_EQ(writeCount, 1);
        for (auto file = before.cbegin(); file != before.cend(); ++file)
            EXPECT_EQ(readFileBytes(context.live.filePath(file.key())), file.value());
    }
}

TEST(ProjectPackExport, CancellationBeforeCleanupLeavesExistingOutputs)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup());
    context.project.buildExportProjectData = true;
    const auto first = context.exporter.ExportAllPacks(context.project, {}, context.options);
    ASSERT_TRUE(first.success) << first.error.toStdString();
    const auto orphan = QDir(context.root).filePath(QStringLiteral("obsolete.bin"));
    ASSERT_NO_FATAL_FAILURE(writeFileBytes(orphan, QByteArray("preserved")));
    const auto metadata = readFileBytes(QDir(context.root).filePath(QStringLiteral("Project.json")));
    context.project.name += QStringLiteral(" changed");
    const auto canceled = harken::studio::ProjectPackExporter::FinalizePackExport(
        context.root, context.project, {context.platform}, true, {{context.platform, first.outputs}}, [] { return true; });
    EXPECT_FALSE(canceled.success);
    EXPECT_TRUE(canceled.canceled);
    EXPECT_EQ(readFileBytes(orphan), QByteArray("preserved"));
    EXPECT_EQ(readFileBytes(QDir(context.root).filePath(QStringLiteral("Project.json"))), metadata);
}

TEST(ProjectPackExport, ProjectBuildCancellationPreservesCommittedPlatformsAndRootPublication)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup());
    context.project.buildExportProjectData = true;
    const auto initial = context.exporter.ExportAllPacks(context.project, {}, context.options);
    ASSERT_TRUE(initial.success) << initial.error.toStdString();
    const QDir root(context.root);
    const QByteArray previousProject = readFileBytes(root.filePath(QStringLiteral("Project.json")));
    ASSERT_FALSE(previousProject.isEmpty());
    const QByteArray previousContent = readFileBytes(context.live.filePath(QStringLiteral("One.bytes")));
    ASSERT_FALSE(previousContent.isEmpty());
    const QString sentinel = root.filePath(QStringLiteral("obsolete.bin"));
    ASSERT_NO_FATAL_FAILURE(writeFileBytes(sentinel, QByteArray("keep until all platforms finish")));
    context.project.name += QStringLiteral(" changed");
    context.project.actions.front().rootItem->children.front()->comp<harken::studio::VAction>()->steps.front().delay = 0.1f;
    QString error;
    const auto snapshot = harken::studio::ProjectBuildSnapshot::capture(context.project, &error);
    ASSERT_TRUE(snapshot) << error.toStdString();
    bool canceled = false;
    QMap<QString, QByteArray> committed;
    harken::studio::ProjectPackBuildService::Request request;
    request.outputRoot = context.root;
    request.platforms = {context.platform, HarkenTest::platformIdString("Android")};
    request.options.snapshot = snapshot;
    request.options.selfCleaning = true;
    request.options.cancelCheck = [&] { return canceled; };
    request.platformFinished = [&](const harken::studio::ProjectPackBuildService::PlatformResult& completed) {
        EXPECT_EQ(completed.platform, context.platform);
        EXPECT_TRUE(completed.result.success) << completed.result.error.toStdString();
        for (const auto& name : publicationFiles(completed.outputDir))
            committed.insert(name, readFileBytes(QDir(completed.outputDir).filePath(name)));
        QString lockError;
        EXPECT_FALSE(harken::studio::ProjectPackExporter::LockPackOutputRoot(context.root, &lockError));
        canceled = true;
    };
    const auto result = harken::studio::ProjectPackBuildService::run(request);
    ASSERT_TRUE(result.preflightSucceeded) << result.preflightError.toStdString();
    EXPECT_TRUE(result.canceled);
    ASSERT_FALSE(committed.isEmpty());
    EXPECT_NE(readFileBytes(context.live.filePath(QStringLiteral("One.bytes"))), previousContent);
    for (auto file = committed.cbegin(); file != committed.cend(); ++file)
        EXPECT_EQ(readFileBytes(context.live.filePath(file.key())), file.value());
    EXPECT_FALSE(QFileInfo::exists(root.filePath(QStringLiteral("Android/Init.bytes"))));
    EXPECT_FALSE(QFileInfo::exists(root.filePath(QStringLiteral("Android/One.bytes"))));
    EXPECT_EQ(readFileBytes(root.filePath(QStringLiteral("Project.json"))), previousProject);
    EXPECT_EQ(readFileBytes(sentinel), QByteArray("keep until all platforms finish"));
    const auto released = harken::studio::ProjectPackExporter::LockPackOutputRoot(context.root, &error);
    EXPECT_TRUE(released) << error.toStdString();
}

TEST(ProjectPackExport, ProjectBuildRejectsConcurrentWritersAndUnsafeOutputRoots)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup());
    const auto sentinel = QDir(context.root).filePath(QStringLiteral("personal.txt"));
    ASSERT_NO_FATAL_FAILURE(writeFileBytes(sentinel, QByteArray("preserved")));
    QString error;
    harken::studio::ProjectPackBuildService::Request request;
    request.outputRoot = context.root;
    request.platforms = {context.platform};
    request.options.snapshot = harken::studio::ProjectBuildSnapshot::capture(context.project, &error);
    ASSERT_TRUE(request.options.snapshot) << error.toStdString();
    auto lock = harken::studio::ProjectPackExporter::LockPackOutputRoot(context.root, &error);
    ASSERT_TRUE(lock) << error.toStdString();
    const auto competing = harken::studio::ProjectPackBuildService::run(request);
    EXPECT_FALSE(competing.outputLocked);
    EXPECT_TRUE(competing.platforms.empty());
    EXPECT_EQ(readFileBytes(sentinel), QByteArray("preserved"));
    const auto protectedFile = context.directory.filePath(QStringLiteral("project-data.txt"));
    ASSERT_NO_FATAL_FAILURE(writeFileBytes(protectedFile, QByteArray("project data")));
    request.outputRoot = context.directory.path();
    const auto rejected = harken::studio::ProjectPackBuildService::run(request);
    EXPECT_FALSE(rejected.allPlatformsSucceeded);
    EXPECT_TRUE(rejected.platforms.empty());
    EXPECT_EQ(readFileBytes(protectedFile), QByteArray("project data"));
}

TEST(ProjectPackExport, ProjectBuildOwnsOutputUntilAllPlatformsFinish)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup());
    const auto obsolete = QDir(context.root).filePath(QStringLiteral("obsolete.bin"));
    ASSERT_NO_FATAL_FAILURE(writeFileBytes(obsolete, QByteArray("obsolete")));
    QString error;
    harken::studio::ProjectPackBuildService::Request request;
    request.outputRoot = context.root;
    request.platforms = HarkenTest::platformIdStrings({"Windows", "Android"});
    request.options.selfCleaning = true;
    request.options.snapshot = harken::studio::ProjectBuildSnapshot::capture(context.project, &error);
    ASSERT_TRUE(request.options.snapshot) << error.toStdString();
    int completed = 0;
    request.platformFinished = [&](const auto& built) {
        EXPECT_TRUE(built.result.success) << built.result.error.toStdString();
        EXPECT_FALSE(harken::studio::ProjectPackExporter::LockPackOutputRoot(context.root, &error));
        ++completed;
    };
    const auto built = harken::studio::ProjectPackBuildService::run(request);
    ASSERT_TRUE(built.allPlatformsSucceeded) << built.preflightError.toStdString();
    ASSERT_TRUE(built.finalizationResult.success) << built.finalizationResult.error.toStdString();
    EXPECT_EQ(completed, request.platforms.size());
    EXPECT_FALSE(QFileInfo::exists(obsolete));
    EXPECT_TRUE(harken::studio::ProjectPackExporter::LockPackOutputRoot(context.root, &error));
}

TEST(ProjectPackExport, PersistentSourceDigestsTrackContentAcrossReloadAndReplacement)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    harken::studio::AssetInfo asset;
    asset.sourceID = 1;
    asset.relativePath = QStringLiteral("source.wav");
    asset.absolutePath = QDir(harken::studio::PathUtils::getAudioSourcesDir(directory.path())).filePath(asset.relativePath);
    ASSERT_NO_FATAL_FAILURE(writeWavF32(asset.absolutePath, std::vector<float>(32, 0.25f), 1, 48000));
    const auto digest = [](const QString& path) {
        return QString::fromLatin1(QCryptographicHash::hash(readFileBytes(path), QCryptographicHash::Sha256).toHex());
    };
    const auto original = digest(asset.absolutePath);
    const auto modified = QFileInfo(asset.absolutePath).lastModified();
    {
        harken::studio::BuildSourceInputs inputs(directory.path(), {{asset.sourceID, asset}});
        const auto input = inputs.inspect(asset);
        ASSERT_TRUE(input);
        EXPECT_EQ(input->contentHash, original);
        ASSERT_TRUE(inputs.save());
    }
    QList<harken::studio::AssetInfo> stored;
    ASSERT_TRUE(harken::studio::AssetRegistry::loadSnapshotFromFile(directory.path(), stored));
    ASSERT_EQ(stored.size(), 1);
    asset = stored.front();
    EXPECT_EQ(asset.contentHash, original);
    EXPECT_FALSE(asset.sourceState.isEmpty());
    {
        harken::studio::BuildSourceInputs inputs(directory.path(), {{asset.sourceID, asset}});
        const auto input = inputs.inspect(asset);
        ASSERT_TRUE(input);
        EXPECT_EQ(input->contentHash, original);
        EXPECT_EQ(input->inputPath, asset.absolutePath);
    }
    ASSERT_NO_FATAL_FAILURE(writeWavF32(asset.absolutePath, std::vector<float>(32, -0.25f), 1, 48000));
    {
        QFile source(asset.absolutePath);
        ASSERT_TRUE(source.open(QIODevice::ReadWrite));
        ASSERT_TRUE(source.setFileTime(modified, QFileDevice::FileModificationTime));
    }
    const auto changed = digest(asset.absolutePath);
    EXPECT_NE(changed, original);
    {
        harken::studio::BuildSourceInputs inputs(directory.path(), {{asset.sourceID, asset}});
        const auto input = inputs.inspect(asset);
        ASSERT_TRUE(input);
        EXPECT_EQ(input->contentHash, changed);
        ASSERT_TRUE(inputs.save());
    }
    ASSERT_TRUE(harken::studio::AssetRegistry::loadSnapshotFromFile(directory.path(), stored));
    asset = stored.front();
    const auto replacement = directory.filePath(QStringLiteral("replacement.wav"));
    ASSERT_NO_FATAL_FAILURE(writeWavF32(replacement, std::vector<float>(32, 0.5f), 1, 48000));
    {
        QFile source(replacement);
        ASSERT_TRUE(source.open(QIODevice::ReadWrite));
        ASSERT_TRUE(source.setFileTime(modified, QFileDevice::FileModificationTime));
    }
    ASSERT_TRUE(QFile::remove(asset.absolutePath));
    ASSERT_TRUE(QFile::rename(replacement, asset.absolutePath));
    harken::studio::BuildSourceInputs replaced(directory.path(), {{asset.sourceID, asset}});
    const auto replacementInput = replaced.inspect(asset);
    ASSERT_TRUE(replacementInput);
    EXPECT_EQ(replacementInput->contentHash, digest(asset.absolutePath));
    EXPECT_NE(replacementInput->contentHash, changed);
    ASSERT_TRUE(replaced.save());
    EXPECT_FALSE(QFileInfo::exists(directory.filePath(QStringLiteral(".cache/pack/sources.json"))));
}

TEST(ProjectPackExport, CapturedSourceVersionSurvivesLaterExternalEdits)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    harken::studio::AssetInfo asset;
    asset.sourceID = 1;
    asset.relativePath = QStringLiteral("source.wav");
    asset.absolutePath = QDir(harken::studio::PathUtils::getAudioSourcesDir(directory.path())).filePath(asset.relativePath);
    ASSERT_NO_FATAL_FAILURE(writeWavF32(asset.absolutePath, std::vector<float>(64, 0.25f), 1, 48000));
    const auto original = readFileBytes(asset.absolutePath);
    harken::studio::BuildSourceInputs inputs(directory.path(), {{asset.sourceID, asset}});
    const auto captured = inputs.capture(asset);
    ASSERT_TRUE(captured);
    ASSERT_NE(captured->inputPath, asset.absolutePath);
    EXPECT_EQ(readFileBytes(captured->inputPath), original);
    ASSERT_NO_FATAL_FAILURE(writeWavF32(asset.absolutePath, std::vector<float>(64, -0.25f), 1, 48000));
    EXPECT_EQ(inputs.capture(asset), captured);
    EXPECT_EQ(readFileBytes(captured->inputPath), original);
    ASSERT_TRUE(inputs.save());
    QList<harken::studio::AssetInfo> stored;
    ASSERT_TRUE(harken::studio::AssetRegistry::loadSnapshotFromFile(directory.path(), stored));
    asset = stored.front();
    harken::studio::BuildSourceInputs next(directory.path(), {{asset.sourceID, asset}});
    const auto nextInput = next.inspect(asset);
    ASSERT_TRUE(nextInput);
    EXPECT_NE(nextInput->contentHash, captured->contentHash);
}

#ifdef Q_OS_WIN
TEST(ProjectPackExport, PersistentSourceDigestsRejectUnfinishedWrites)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    harken::studio::AssetInfo asset;
    asset.sourceID = 1;
    asset.relativePath = QStringLiteral("source.wav");
    asset.absolutePath = QDir(harken::studio::PathUtils::getAudioSourcesDir(directory.path())).filePath(asset.relativePath);
    ASSERT_NO_FATAL_FAILURE(writeWavF32(asset.absolutePath, std::vector<float>(32, 0.25f), 1, 48000));
    {
        harken::studio::BuildSourceInputs inputs(directory.path(), {{asset.sourceID, asset}});
        ASSERT_TRUE(inputs.inspect(asset));
        ASSERT_TRUE(inputs.save());
    }
    QList<harken::studio::AssetInfo> stored;
    ASSERT_TRUE(harken::studio::AssetRegistry::loadSnapshotFromFile(directory.path(), stored));
    asset = stored.front();
    const auto modified = QFileInfo(asset.absolutePath).lastModified();
    QFile writer(asset.absolutePath);
    ASSERT_TRUE(writer.open(QIODevice::ReadWrite));
    ASSERT_TRUE(writer.seek(writer.size() - 4));
    ASSERT_EQ(writer.write(QByteArray(4, '\0')), 4);
    ASSERT_TRUE(writer.flush());
    ASSERT_TRUE(writer.setFileTime(modified, QFileDevice::FileModificationTime));
    harken::studio::BuildSourceInputs inputs(directory.path(), {{asset.sourceID, asset}});
    EXPECT_FALSE(inputs.inspect(asset));
    writer.close();
    EXPECT_FALSE(inputs.inspect(asset));
    inputs.beginRequest();
    const auto completed = inputs.inspect(asset);
    ASSERT_TRUE(completed);
    EXPECT_NE(completed->contentHash, asset.contentHash);
}
#endif

TEST(ProjectPackExport, ProjectBuildIncrementalResultsMatchForcedBuildAfterInputAndOutputChanges)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup());
    const auto build = [&](bool force) {
        QString error;
        harken::studio::ProjectPackBuildService::Request request;
        request.outputRoot = context.root;
        request.platforms = {context.platform};
        request.options.snapshot = harken::studio::ProjectBuildSnapshot::capture(context.project, &error);
        EXPECT_TRUE(request.options.snapshot) << error.toStdString();
        request.options.forceReexport = force;
        return harken::studio::ProjectPackBuildService::run(request);
    };
    const auto initial = build(false);
    ASSERT_TRUE(initial.allPlatformsSucceeded) << initial.preflightError.toStdString();
    ASSERT_TRUE(initial.finalizationResult.success) << initial.finalizationResult.error.toStdString();
    QByteArray previousContent = readFileBytes(context.live.filePath(QStringLiteral("One.bytes")));
    const auto readMedia = [&](QByteArray& payload) {
        const QByteArray bytes = readFileBytes(context.live.filePath(QStringLiteral("One.bytes")));
        harken::studio::build::PackCanonicalImage image;
        ASSERT_EQ(VN_OK, harken::studio::build::ParsePackCanonicalImage(
            reinterpret_cast<const std::uint8_t*>(bytes.constData()),
            static_cast<std::uint64_t>(bytes.size()), &image));
        ASSERT_EQ(image.media.size(), 1u);
        const auto& media = image.media.front();
        payload = bytes.mid(static_cast<qsizetype>(media.blobOffset),
                            static_cast<qsizetype>(media.blobBytes));
    };
    QByteArray previousMedia;
    ASSERT_NO_FATAL_FAILURE(readMedia(previousMedia));
    auto& source = context.project.layers.front().rootItem->children.front()
        ->comp<harken::studio::VSound>()->sources.plain.entries.front();
    const QString sourcePath = QDir(harken::studio::PathUtils::getAudioSourcesDir(context.directory.path()))
        .filePath(QStringLiteral("tone.wav"));
    const std::vector<std::function<void()>> changes{
        [&] {
            context.project.actions.front().rootItem->children.front()
                ->comp<harken::studio::VAction>()->steps.front().delay = 0.125f;
        },
        [&] {
            const auto modified = QFileInfo(sourcePath).lastModified();
            auto bytes = readFileBytes(sourcePath);
            ASSERT_GE(bytes.size(), static_cast<qsizetype>(sizeof(float)));
            const float sample = 0.75f;
            std::memcpy(bytes.data() + bytes.size() - sizeof(sample), &sample, sizeof(sample));
            ASSERT_NO_FATAL_FAILURE(writeFileBytes(sourcePath, bytes));
            QFile file(sourcePath);
            ASSERT_TRUE(file.open(QIODevice::ReadWrite));
            ASSERT_TRUE(file.setFileTime(modified, QFileDevice::FileModificationTime));
            EXPECT_EQ(QFileInfo(sourcePath).size(), bytes.size());
            EXPECT_EQ(QFileInfo(sourcePath).lastModified(), modified);
        },
        [&] {
            source.fadeIn.durationFrames = 32;
            source.fadeOut.durationFrames = 16;
        },
        [&] {
            source.fadeIn.durationFrames = 48;
        },
        [&] {
            source.fadeIn.handles = {{0.1f, 0.8f, 0.6f, 1.0f}};
        },
        [&] {
            const QString path = context.live.filePath(QStringLiteral("One.bytes"));
            auto bytes = readFileBytes(path);
            ASSERT_FALSE(bytes.isEmpty());
            bytes[bytes.size() - 1] = static_cast<char>(bytes.back() ^ 0x40);
            ASSERT_NO_FATAL_FAILURE(writeFileBytes(path, bytes));
        },
        [&] {
            ASSERT_TRUE(QFile::remove(context.live.filePath(QStringLiteral("One.json"))));
        },
        [&] {
            context.project.buildExportPackMetadata = true;
        }};
    for (size_t index = 0; index < changes.size(); ++index) {
        SCOPED_TRACE(index);
        ASSERT_NO_FATAL_FAILURE(changes[index]());
        const auto incremental = build(false);
        ASSERT_TRUE(incremental.allPlatformsSucceeded) << incremental.preflightError.toStdString();
        ASSERT_TRUE(incremental.finalizationResult.success) << incremental.finalizationResult.error.toStdString();
        ASSERT_EQ(incremental.platforms.size(), 1u);
        const auto content = readFileBytes(context.live.filePath(QStringLiteral("One.bytes")));
        if (index < 5) EXPECT_NE(content, previousContent);
        previousContent = content;
        QByteArray media;
        ASSERT_NO_FATAL_FAILURE(readMedia(media));
        if (index >= 1 && index <= 4) EXPECT_NE(media, previousMedia);
        else EXPECT_EQ(media, previousMedia);
        previousMedia = media;
        QMap<QString, QByteArray> expected;
        for (const auto& path : publicationFiles(context.live.path()))
            expected.insert(path, readFileBytes(context.live.filePath(path)));
        const auto forced = build(true);
        ASSERT_TRUE(forced.allPlatformsSucceeded) << forced.preflightError.toStdString();
        ASSERT_TRUE(forced.finalizationResult.success) << forced.finalizationResult.error.toStdString();
        ASSERT_EQ(forced.platforms.size(), 1u);
        EXPECT_EQ(incremental.platforms.front().result.warnings, forced.platforms.front().result.warnings);
        EXPECT_EQ(publicationFiles(context.live.path()), expected.keys());
        for (auto it = expected.cbegin(); it != expected.cend(); ++it)
            EXPECT_EQ(readFileBytes(context.live.filePath(it.key())), it.value()) << it.key().toStdString();
    }
}

TEST(ProjectPackExport, ProjectBuildStateFollowsOutputsAcrossForceAndSelection)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup());
    QString error;
    harken::studio::ProjectPackBuildService::Request request;
    request.outputRoot = context.root;
    request.platforms = {context.platform};
    request.options.snapshot = harken::studio::ProjectBuildSnapshot::capture(context.project, &error);
    ASSERT_TRUE(request.options.snapshot) << error.toStdString();
    bool compiled = false;
    request.options.observationCallback = [&](const harken::studio::PackBuildObservation& observation) {
        if (observation.stage == harken::studio::PackBuildObservationStage::CompilingProject) compiled = true;
    };
    request.options.forceReexport = true;
    const auto forced = harken::studio::ProjectPackBuildService::run(request);
    ASSERT_TRUE(forced.finalizationResult.success) << forced.finalizationResult.error.toStdString();
    const auto original = readFileBytes(context.live.filePath(QStringLiteral("One.bytes")));
    auto* action = context.project.actions.front().rootItem->children.front().get();
    action->notes = QStringLiteral("Editor-only notes");
    context.project.documentHeader.insert(QStringLiteral("notes"), QStringLiteral("Editor-only header"));
    context.project.name += QStringLiteral(" renamed");
    const auto android = std::find_if(context.project.platforms.begin(), context.project.platforms.end(),
        [&](const auto& platform) { return platform.id == QUuid(HarkenTest::platformIdString("Android")); });
    ASSERT_NE(android, context.project.platforms.end());
    android->name += QStringLiteral(" renamed");
    const auto unrelated = context.live.filePath(QStringLiteral("personal.txt"));
    ASSERT_NO_FATAL_FAILURE(writeFileBytes(unrelated, QByteArray("unmanaged file")));
    request.options.forceReexport = false;
    compiled = false;
    const auto reused = harken::studio::ProjectPackBuildService::run(request);
    ASSERT_TRUE(reused.finalizationResult.success) << reused.finalizationResult.error.toStdString();
    ASSERT_EQ(forced.platforms.size(), 1u);
    ASSERT_EQ(reused.platforms.size(), 1u);
    EXPECT_EQ(reused.platforms.front().result.objectCount, forced.platforms.front().result.objectCount);
    EXPECT_EQ(reused.platforms.front().result.mediaCount, forced.platforms.front().result.mediaCount);
    EXPECT_EQ(reused.platforms.front().result.actionCount, forced.platforms.front().result.actionCount);
    EXPECT_FALSE(compiled);
    EXPECT_EQ(readFileBytes(unrelated), QByteArray("unmanaged file"));
    EXPECT_EQ(readFileBytes(context.live.filePath(QStringLiteral("One.bytes"))), original);
    action->comp<harken::studio::VAction>()->steps.front().delay = 0.125f;
    compiled = false;
    const auto changed = harken::studio::ProjectPackBuildService::run(request);
    ASSERT_TRUE(changed.finalizationResult.success) << changed.finalizationResult.error.toStdString();
    EXPECT_TRUE(compiled);
    EXPECT_NE(readFileBytes(context.live.filePath(QStringLiteral("One.bytes"))), original);
    request.options.updateSelectedOnly = true;
    request.options.onlyPackPaths.insert(QStringLiteral("One"));
    const auto partial = harken::studio::ProjectPackBuildService::run(request);
    ASSERT_TRUE(partial.finalizationResult.success) << partial.finalizationResult.error.toStdString();
    compiled = false;
    const auto partialReused = harken::studio::ProjectPackBuildService::run(request);
    ASSERT_TRUE(partialReused.finalizationResult.success) << partialReused.finalizationResult.error.toStdString();
    EXPECT_FALSE(compiled);
    ASSERT_EQ(partialReused.platforms.size(), 1u);
    EXPECT_EQ(partialReused.platforms.front().records.size(), partial.platforms.front().records.size());
    EXPECT_EQ(partialReused.artifacts.size(), partial.artifacts.size());
}

TEST(ProjectPackExport, ProjectBuildMediaCacheTracksCurrentSourceVersions)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup(true));
    const auto platforms = HarkenTest::platformIdStrings({"Windows", "Android"});
    const auto build = [&](const QStringList& selected, bool partial) {
        QString error;
        harken::studio::ProjectPackBuildService::Request request;
        request.outputRoot = context.root;
        request.platforms = selected;
        request.options.snapshot = harken::studio::ProjectBuildSnapshot::capture(context.project, &error);
        EXPECT_TRUE(request.options.snapshot) << error.toStdString();
        request.options.updateSelectedOnly = partial;
        if (partial) request.options.onlyPackPaths.insert(QStringLiteral("One"));
        return harken::studio::ProjectPackBuildService::run(request);
    };
    const auto first = build(platforms, false);
    ASSERT_TRUE(first.allPlatformsSucceeded) << first.preflightError.toStdString();
    ASSERT_TRUE(first.finalizationResult.success) << first.finalizationResult.error.toStdString();
    const auto unchanged = build({platforms.back(), platforms.front()}, false);
    ASSERT_TRUE(unchanged.allPlatformsSucceeded) << unchanged.preflightError.toStdString();
    ASSERT_TRUE(unchanged.finalizationResult.success) << unchanged.finalizationResult.error.toStdString();
    ASSERT_EQ(unchanged.platforms.size(), 2u);
    for (const auto& platform : unchanged.platforms) {
        EXPECT_EQ(platform.result.builtPackCount, 0);
        EXPECT_GT(platform.result.skippedPackCount, 0);
    }
    const QDir media(context.directory.filePath(QStringLiteral(".cache/pack/media")));
    ASSERT_EQ(media.entryList({QStringLiteral("*.pcm")}, QDir::Files).size(), 1);
    const QString sourcePath = QDir(harken::studio::PathUtils::getAudioSourcesDir(context.directory.path()))
        .filePath(QStringLiteral("tone.wav"));
    auto bytes = readFileBytes(sourcePath);
    ASSERT_GE(bytes.size(), static_cast<qsizetype>(sizeof(float)));
    const float sample = 0.75f;
    std::memcpy(bytes.data() + bytes.size() - sizeof(sample), &sample, sizeof(sample));
    ASSERT_NO_FATAL_FAILURE(writeFileBytes(sourcePath, bytes));
    const auto windows = build({platforms.front()}, true);
    ASSERT_TRUE(windows.allPlatformsSucceeded) << windows.preflightError.toStdString();
    ASSERT_TRUE(windows.finalizationResult.success) << windows.finalizationResult.error.toStdString();
    EXPECT_EQ(media.entryList({QStringLiteral("*.pcm")}, QDir::Files).size(), 1);
    const auto android = build({platforms.back()}, true);
    ASSERT_TRUE(android.allPlatformsSucceeded) << android.preflightError.toStdString();
    ASSERT_TRUE(android.finalizationResult.success) << android.finalizationResult.error.toStdString();
    EXPECT_EQ(media.entryList({QStringLiteral("*.pcm")}, QDir::Files).size(), 1);
    const auto reused = build(platforms, true);
    ASSERT_TRUE(reused.allPlatformsSucceeded) << reused.preflightError.toStdString();
    ASSERT_TRUE(reused.finalizationResult.success) << reused.finalizationResult.error.toStdString();
    for (const auto& platform : reused.platforms) EXPECT_EQ(platform.result.builtPackCount, 0);
}

TEST(ProjectPackExport, RegeneratedOutputsMatchAfterDeletingDisposableBuildState)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup(true));
    context.project.buildExportProjectData = true;
    const auto build = [&] {
        QString error;
        harken::studio::ProjectPackBuildService::Request request;
        request.outputRoot = context.root;
        request.platforms = {context.platform};
        request.options.snapshot = harken::studio::ProjectBuildSnapshot::capture(context.project, &error);
        EXPECT_TRUE(request.options.snapshot) << error.toStdString();
        return harken::studio::ProjectPackBuildService::run(request);
    };
    const auto initial = build();
    ASSERT_TRUE(initial.allPlatformsSucceeded) << initial.preflightError.toStdString();
    QMap<QString, QByteArray> expected;
    for (const auto& path : publicationFiles(context.live.path()))
        expected.insert(context.live.filePath(path), readFileBytes(context.live.filePath(path)));
    const auto cacheRoot = QDir(harken::studio::PathUtils::getDotCacheDir(context.directory.path()));
    ASSERT_TRUE(QDir(cacheRoot.filePath(QStringLiteral("pack"))).removeRecursively());
    const auto rebuilt = build();
    ASSERT_TRUE(rebuilt.allPlatformsSucceeded) << rebuilt.preflightError.toStdString();
    for (auto it = expected.cbegin(); it != expected.cend(); ++it)
        EXPECT_EQ(readFileBytes(it.key()), it.value()) << it.key().toStdString();
    const QDir waveforms(context.project.buildWaveformOutputDir);
    for (const auto& name : waveforms.entryList({QStringLiteral("*.vkd")}, QDir::Files))
        ASSERT_TRUE(QFile::remove(waveforms.filePath(name)));
    const auto regenerated = build();
    ASSERT_TRUE(regenerated.allPlatformsSucceeded) << regenerated.preflightError.toStdString();
    EXPECT_FALSE(waveforms.entryList({QStringLiteral("*.vkd")}, QDir::Files).isEmpty());
    for (auto it = expected.cbegin(); it != expected.cend(); ++it)
        EXPECT_EQ(readFileBytes(it.key()), it.value()) << it.key().toStdString();
}

TEST(ProjectPackExport, ProjectBuildReuseRestoresRowsAfterReload)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup(true));
    auto actions = std::move(context.project.actions.front().rootItem->children);
    actions.push_back(makeItem(QStringLiteral("Empty"), harken::studio::ProjectItem::Type::Action, 55));
    actions.push_back(makeItem(QStringLiteral("OtherEmpty"), harken::studio::ProjectItem::Type::Action, 56));
    context.project.actions.clear();
    for (auto& action : actions) {
        action->parent = nullptr;
        harken::studio::ProjectFileBinding binding;
        binding.rootItem = std::move(action);
        binding.parentFolder = QLatin1String(harken::studio::ItemTypeCatalog::kActionsFolderName);
        context.project.actions.push_back(std::move(binding));
    }
    const auto build = [&](const std::shared_ptr<const harken::studio::ProjectBuildSnapshot>& snapshot,
                           const QStringList& platforms, bool* compiled) {
        harken::studio::ProjectPackBuildService::Request request;
        request.outputRoot = context.root;
        request.platforms = platforms;
        request.options.snapshot = snapshot;
        request.options.observationCallback = [compiled](const harken::studio::PackBuildObservation& observation) {
            if (observation.stage == harken::studio::PackBuildObservationStage::CompilingProject) *compiled = true;
        };
        return harken::studio::ProjectPackBuildService::run(request);
    };
    QString error;
    const auto firstSnapshot = harken::studio::ProjectBuildSnapshot::capture(context.project, &error);
    ASSERT_TRUE(firstSnapshot) << error.toStdString();
    bool compiled = false;
    const auto initial = build(firstSnapshot, {context.platform}, &compiled);
    ASSERT_TRUE(initial.allPlatformsSucceeded) << initial.preflightError.toStdString();
    ASSERT_TRUE(initial.finalizationResult.success) << initial.finalizationResult.error.toStdString();
    ASSERT_EQ(initial.platforms.size(), 1u);
    ASSERT_TRUE(compiled);
    ASSERT_FALSE(initial.platforms.front().result.warnings.isEmpty());
    EXPECT_EQ(initial.platforms.front().result.warnings.size(), 2);
    EXPECT_TRUE(initial.platforms.front().result.globalWarnings.isEmpty());
    QMap<QString, QByteArray> expected;
    for (const auto& path : publicationFiles(context.live.path()))
        expected.insert(path, readFileBytes(context.live.filePath(path)));
    compiled = false;
    const auto added = build(firstSnapshot, {HarkenTest::platformIdString("Android")}, &compiled);
    ASSERT_TRUE(added.allPlatformsSucceeded) << added.preflightError.toStdString();
    ASSERT_TRUE(added.finalizationResult.success) << added.finalizationResult.error.toStdString();
    ASSERT_EQ(added.platforms.size(), 1u);
    ASSERT_TRUE(compiled);
    ASSERT_EQ(QDir(context.project.buildWaveformOutputDir)
        .entryList({QStringLiteral("*.vkd")}, QDir::Files).size(), 1);
    harken::studio::ProjectSerializer writer;
    ASSERT_TRUE(writer.prepareReferenceWrite(context.project)) << writer.lastError().toStdString();
    QJsonArray roots;
    for (const auto* binding : context.project.allFileBindings()) {
        const auto serialized = writer.projectItemToJson(*binding->rootItem);
        ASSERT_FALSE(serialized.isEmpty()) << writer.lastError().toStdString();
        roots.append(serialized);
    }
    harken::studio::ProjectSerializer reader;
    ASSERT_TRUE(reader.prepareStandaloneReferenceRead(roots)) << reader.lastError().toStdString();
    auto reloaded = context.project;
    size_t index = 0;
    for (auto* binding : reloaded.allFileBindings()) {
        binding->rootItem = std::make_shared<harken::studio::ProjectItem>();
        ASSERT_TRUE(reader.projectItemFromJson(roots.at(static_cast<qsizetype>(index++)).toObject(),
            *binding->rootItem)) << reader.lastError().toStdString();
    }
    ASSERT_NE(reloaded.actions.front().rootItem->objectID,
        context.project.actions.front().rootItem->objectID);
    const auto captured = harken::studio::ProjectBuildSnapshot::capture(reloaded, &error);
    ASSERT_TRUE(captured) << error.toStdString();
    auto nextSnapshot = std::make_shared<harken::studio::ProjectBuildSnapshot>(*captured);
    reloaded.audioSources.clear();
    reloaded.sourceAssets.clear();
    for (auto& asset : nextSnapshot->media.assets) {
        asset.metadata = {};
        asset.contentHash.clear();
        asset.ownerObjectGuid.clear();
        asset.isOrphaned = !asset.isOrphaned;
    }
    compiled = false;
    const auto reused = build(nextSnapshot,
        HarkenTest::platformIdStrings({"Windows", "Android"}), &compiled);
    ASSERT_TRUE(reused.allPlatformsSucceeded) << reused.preflightError.toStdString();
    ASSERT_TRUE(reused.finalizationResult.success) << reused.finalizationResult.error.toStdString();
    ASSERT_EQ(reused.platforms.size(), 2u);
    EXPECT_FALSE(compiled);
    for (const auto& platform : reused.platforms) {
        EXPECT_EQ(platform.result.builtPackCount, 0);
        EXPECT_GT(platform.result.skippedPackCount, 0);
        const auto& previous = platform.platform == context.platform
            ? initial.platforms.front() : added.platforms.front();
        EXPECT_EQ(platform.result.objectCount, previous.result.objectCount);
        EXPECT_EQ(platform.result.mediaCount, previous.result.mediaCount);
        EXPECT_EQ(platform.result.actionCount, previous.result.actionCount);
        EXPECT_EQ(platform.result.warnings, previous.result.warnings);
        EXPECT_EQ(platform.result.globalWarnings, previous.result.globalWarnings);
    }
    ASSERT_EQ(reused.platforms.front().records.size(), initial.platforms.front().records.size());
    for (const auto& record : reused.platforms.front().records) {
        const auto prior = std::find_if(initial.platforms.front().records.begin(),
            initial.platforms.front().records.end(), [&](const auto& candidate) {
                return candidate.unitKey == record.unitKey && candidate.language == record.language;
            });
        ASSERT_NE(prior, initial.platforms.front().records.end());
        EXPECT_EQ(record.omitted, prior->omitted);
        EXPECT_EQ(record.warnings, prior->warnings);
        EXPECT_TRUE(record.success);
        if (record.source == harken::studio::ProjectPackExporter::PackBuildRecord::Source::Action) {
            const auto action = std::find_if(reloaded.actions.begin(),
                reloaded.actions.end(), [&](const auto& binding) {
                    return binding.rootItem->name == record.packName;
                });
            ASSERT_NE(action, reloaded.actions.end());
            EXPECT_EQ(record.objectID, action->rootItem->objectID);
        }
    }
    for (auto it = expected.cbegin(); it != expected.cend(); ++it)
        EXPECT_EQ(readFileBytes(context.live.filePath(it.key())), it.value()) << it.key().toStdString();
}

TEST(ProjectPackExport, ProjectBuildReuseOnlyDependsOnSelectedPlatformSources)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup());
    const auto androidSource = registerToneAsset(context.directory.path(), "android.wav", 0.4f);
    auto& sources = context.project.layers.front().rootItem->children.front()
        ->comp<harken::studio::VSound>()->sources.plain;
    harken::studio::SoundSourceDef android;
    android.entryId = QUuid::createUuid();
    android.sourceId = androidSource;
    sources.entries.push_back(android);
    sources.platforms[HarkenTest::platformIdString("Android")].active = android.entryId;
    const auto build = [&](const QStringList& platforms, bool* compiled) {
        QString error;
        harken::studio::ProjectPackBuildService::Request request;
        request.outputRoot = context.root;
        request.platforms = platforms;
        request.options.snapshot = harken::studio::ProjectBuildSnapshot::capture(context.project, &error);
        EXPECT_TRUE(request.options.snapshot) << error.toStdString();
        request.options.observationCallback = [compiled](const harken::studio::PackBuildObservation& observation) {
            if (observation.stage == harken::studio::PackBuildObservationStage::CompilingProject) *compiled = true;
        };
        return harken::studio::ProjectPackBuildService::run(request);
    };
    bool compiled = false;
    const auto initial = build(HarkenTest::platformIdStrings({"Windows", "Android"}), &compiled);
    ASSERT_TRUE(initial.allPlatformsSucceeded) << initial.preflightError.toStdString();
    ASSERT_TRUE(initial.finalizationResult.success) << initial.finalizationResult.error.toStdString();
    const auto windowsBytes = readFileBytes(context.live.filePath(QStringLiteral("One.bytes")));
    ASSERT_TRUE(QFile::remove(QDir(harken::studio::PathUtils::getAudioSourcesDir(context.directory.path()))
        .filePath(QStringLiteral("android.wav"))));
    compiled = false;
    const auto reused = build({context.platform}, &compiled);
    ASSERT_TRUE(reused.allPlatformsSucceeded) << reused.preflightError.toStdString();
    ASSERT_TRUE(reused.finalizationResult.success) << reused.finalizationResult.error.toStdString();
    EXPECT_FALSE(compiled);
    EXPECT_EQ(readFileBytes(context.live.filePath(QStringLiteral("One.bytes"))), windowsBytes);
}

TEST(ResidentVorbisPackPlayback, ConvertedSampleRatesPreserveSupportedSourceLayouts)
{
    using namespace harken::studio;
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    SoundSourceDef source;
    source.entryId = QUuid::createUuid();
    source.loopRegion = SoundLoopRegion{240, 2400};
    const auto platform = HarkenTest::platformId("Windows");
    for (uint16_t channels : {3, 4, 5, 6, 7, 8}) {
        const auto path = directory.filePath(QStringLiteral("signal-%1.wav").arg(channels));
        const std::vector<float> samples(4800 * channels, 0.25f);
        ASSERT_NO_FATAL_FAILURE(writeWavF32(path, samples, channels, 48000));
        for (uint32_t rate : {48000u, 32000u}) {
            SCOPED_TRACE(::testing::Message() << channels << " channels at " << rate << " Hz");
            const ResolvedConversion conversion{.format = ConversionFormat::Vorbis, .sampleRate = rate};
            QString error;
            const auto artifact = SoundSourceDerivative::materialize(path, source, 0.0, platform, {}, conversion, &error);
            if (channels % 2) {
                EXPECT_FALSE(artifact);
                EXPECT_TRUE(error.contains(QStringLiteral("1, 2, 4, 6 or 8"))) << error.toStdString();
                continue;
            }
            ASSERT_TRUE(artifact && artifact->encodedMedia) << error.toStdString();
            EXPECT_EQ(artifact->channels, channels);
            EXPECT_EQ(artifact->sampleRate, rate);
            EXPECT_EQ(artifact->encodedMedia->frameCount, rate / 10);
            EXPECT_EQ(artifact->loopStartFrame, rate / 200);
            EXPECT_EQ(artifact->loopEndFrame, rate / 20);
            const auto preview = bridge::preparePreviewMedia(*artifact, &error);
            ASSERT_TRUE(preview && preview->vorbis) << error.toStdString();
            const auto& bytes = preview->vorbis->bytes;
            const VnPackByteSpan span{sizeof(span), 0, bytes.data(), bytes.size()};
            EXPECT_EQ(VnPreview_ValidateResidentVorbis(&span), VN_OK);
        }
    }
}

TEST(ResidentVorbisPackPlayback, EnginePlaysSeeksAndLoopsStudioExport)
{
    using namespace harken::studio;
    constexpr uint32_t rate = 48000, block = 128;
    constexpr uint32_t loopStart = 4321, loopEnd = 12513, seekFrame = 10721;
    std::vector<float> samples(16000);
    for (size_t i = 0; i < samples.size(); ++i) {
        float level = 0.02f;
        if (i < 2048) level = 0.12f;
        if (i >= loopStart && i < loopStart + 1024) level = 0.72f;
        if (i >= seekFrame && i < loopEnd) level = 0.36f;
        samples[i] = level * static_cast<float>(std::sin(i * (2.0 * 3.141592653589793 / 48.0)));
    }

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& registry = AssetRegistry::instance();
    registry.clear();
    const auto clearRegistry = qScopeGuard([&] { registry.clear(); });
    registry.setProjectRoot(dir.path());
    const auto source = registerPcmAsset(dir.path(), "markers.wav", samples, 1);
    ProjectData project;
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), source, source,
        {{QStringLiteral("Play"), 21, kSharedSoundObjectId}}));
    auto& sound = *project.layers.front().rootItem->children.front();
    sound.comp<VAudio>()->loopCount = 0;
    sound.comp<VSound>()->sources.plain.entries.front().loopRegion = SoundLoopRegion{loopStart, loopEnd};
    auto preset = makeItem("Vorbis", ProjectItem::Type::Conversion, 601);
    auto& rules = preset->comp<VConversion>()->config.rules;
    rules = {{std::nullopt, rate}};
    rules.front().setFormat(ConversionFormat::Vorbis);
    project.conversionItems.push_back({preset});
    sound.fieldGroups.conversion.shared = {FieldGroupMode::Local, ConversionFieldGroup{toFieldGroupGuid(preset->guid)}};

    const QDir output(dir.filePath("published"));
    ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    const auto result = ProjectPackExporter().ExportAllPacks(project, output.path(), options);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    const auto content = readFileBytes(output.filePath("Play.bytes"));
    build::PackCanonicalImage image;
    ASSERT_EQ(VN_OK, build::ParsePackCanonicalImage(reinterpret_cast<const uint8_t*>(content.constData()),
        static_cast<uint64_t>(content.size()), &image));
    ASSERT_EQ(1u, image.media.size());
    ASSERT_EQ(HK_PACK_SAMPLE_FORMAT_VORBIS_RESIDENT, image.media.front().sampleFormat);
    const auto* asset = registry.getAsset(source);
    ASSERT_NE(nullptr, asset);
    const auto conversion = ResolvedConversion{.format = ConversionFormat::Vorbis,
        .vorbisQuality = rules.front().vorbisQuality, .sampleRate = rate};
    QString error;
    const auto derivative = SoundSourceDerivative::materialize(asset->absolutePath,
        sound.comp<VSound>()->sources.plain.entries.front(), 0.0, QUuid(options.platform), {}, conversion, &error);
    ASSERT_TRUE(derivative) << error.toStdString();
    const auto preview = bridge::preparePreviewMedia(*derivative, &error);
    ASSERT_TRUE(preview && preview->vorbis) << error.toStdString();
    const auto& packedMedia = image.media.front();
    ASSERT_EQ(preview->vorbis->bytes.size(), packedMedia.blobBytes);
    EXPECT_EQ(0, std::memcmp(preview->vorbis->bytes.data(),
        content.constData() + packedMedia.blobOffset, preview->vorbis->bytes.size()));
    EXPECT_EQ(preview->contentRevision, packedMedia.blobChecksum ? packedMedia.blobChecksum : 1);
    const auto packs = readPublication(output.path());
    const QByteArray paths[] = {output.filePath("Init.bytes").toUtf8(), output.filePath("Play.bytes").toUtf8()};
    VnCanonicalPackSetMemberV1 members[2]{};
    for (size_t i = 0; i < 2; ++i) {
        auto& member = members[i];
        member.structSize = sizeof(member);
        member.packKind = i == 0 ? VN_CANONICAL_PACK_SET_MEMBER_INIT : VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
        member.packStableId = findByName(packs, i == 0 ? "Init" : "Play").value("packStableId").toString().toULongLong();
        ASSERT_NE(member.packStableId, 0u);
        member.utf8Path = paths[i].constData();
    }
    VnConfig config{};
    config.structSize = sizeof(config);
    config.deviceMode = VN_DEVICE_NULL;
    config.sampleRate = rate;
    config.blockFrames = block;
    ASSERT_EQ(VN_OK, Vn_Init(&config));
    const auto terminate = qScopeGuard([] { Vn_Term(); });
    VnCanonicalPackSetMemberResultV1 rows[2]{};
    for (auto& row : rows) row.structSize = sizeof(row);
    VnCanonicalPackSetActivationDescV1 load{};
    load.structSize = sizeof(load);
    load.setRevision = 1;
    load.memberCount = load.outputMemberCapacity = 2;
    load.memberStructSize = sizeof(members[0]);
    load.outputMemberStructSize = sizeof(rows[0]);
    load.members = members;
    load.outputMembers = rows;
    VnCanonicalPackSetActivationReportV1 report{};
    report.structSize = sizeof(report);
    ASSERT_EQ(VN_OK, Vn_ActivateCanonicalPackSetV1(&load, &report));
    ASSERT_EQ(VN_OK, VnPreview_EnableChannelPeakMetering());
    VnPostActionDesc post{};
    post.structSize = sizeof(post);
    post.actionId = harkentest::ObjectId(kStableIdBias + 21);
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&post.emitter));
    VnActionFiringHandle firing{};
    ASSERT_EQ(VN_OK, Vn_PostAction(&post, &firing));

    const auto renderPeak = [&]() -> float {
        EXPECT_EQ(VN_OK, Vn_Update());
        VnPreview_PumpNullMixer(1);
        VnPreviewChannelPeaksV1 levels{};
        levels.structSize = sizeof(levels);
        EXPECT_EQ(VN_OK, VnPreview_TakeChannelPeaks(&levels));
        for (uint32_t bus = 0; bus < levels.busCount; ++bus)
            if (levels.busIds[bus] == VN_BUS_MASTER)
                return levels.channelPeaks[bus * VN_MAX_CHANNELS];
        ADD_FAILURE() << "Missing master bus";
        return 0.0f;
    };
    float startPeak = 0.0f;
    for (int i = 0; i < 8; ++i) startPeak = renderPeak();
    ASSERT_GT(startPeak, 0.03f);

    VnActionSeekDescV1 seek{};
    seek.structSize = sizeof(seek);
    seek.firing = firing;
    seek.positionMs = 1000.0 * seekFrame / rate;
    seek.mode = VN_ACTION_SEEK_ABSOLUTE;
    VnActionSeekTicket ticket{};
    ASSERT_EQ(VN_OK, Vn_SeekActionFiring(&seek, &ticket));
    renderPeak();
    VnActionSeekInfoV1 info{};
    info.structSize = sizeof(info);
    ASSERT_EQ(VN_OK, Vn_QueryActionSeek(ticket, &info));
    ASSERT_EQ(VN_ACTION_SEEK_APPLIED, info.state);
    ASSERT_EQ(seekFrame, info.minimumAppliedSourceFrame);
    ASSERT_EQ(VN_OK, Vn_ReleaseActionSeek(ticket));

    constexpr unsigned firstWrap = (loopEnd - seekFrame) / block;
    constexpr unsigned secondWrap = firstWrap + (loopEnd - loopStart) / block;
    for (unsigned i = 1; i <= secondWrap + 2; ++i) {
        const float peak = renderPeak();
        if (i == 4 || i == firstWrap - 2 || i == secondWrap - 2)
            EXPECT_NEAR(peak / startPeak, 3.0f, 0.35f) << "seek marker, block " << i;
        if (i == firstWrap + 2 || i == secondWrap + 2)
            EXPECT_NEAR(peak / startPeak, 6.0f, 0.6f) << "loop marker, block " << i;
    }
}

TEST(ProjectPackExport, EffectiveMediaChangesRebuildOnlyTheirConsumers)
{
    using namespace harken::studio;
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup());
    auto* layer = context.project.layers.front().rootItem.get();
    auto* firstSound = layer->children.front().get();
    auto* secondSound = layer->children.back().get();
    auto secondAction = makeItem(QStringLiteral("Two"), ProjectItem::Type::Action, 22);
    VActionStep play;
    play.type = VActionStep::Type::Play;
    play.targetObjectID = secondSound->objectID;
    secondAction->comp<VAction>()->steps.push_back(play);
    addChild(context.project.actions.front().rootItem, secondAction);
    const auto addPreset = [&](const QString& name, uint32_t id, uint32_t rate) {
        auto item = makeItem(name, ProjectItem::Type::Conversion, id);
        item->comp<VConversion>()->config.rules = {{std::nullopt, rate}};
        context.project.conversionItems.push_back({item});
        return item;
    };
    const auto firstPreset = addPreset(QStringLiteral("First"), 601, 24000);
    const auto secondPreset = addPreset(QStringLiteral("Second"), 602, 16000);
    const auto unusedPreset = addPreset(QStringLiteral("Unused"), 603, 0);
    const auto androidPreset = addPreset(QStringLiteral("Android"), 604, 0);
    const auto bind = [](ProjectItem& item, const ProjectItem& preset) {
        auto& group = item.fieldGroups.conversion.shared;
        group.mode = FieldGroupMode::Local;
        group.localHistory = ConversionFieldGroup{toFieldGroupGuid(preset.guid)};
    };
    bind(*layer, *firstPreset);
    bind(*secondSound, *secondPreset);
    layer->fieldGroups.conversion.platforms[toFieldGroupGuid(HarkenTest::platformId("Android"))] =
        {FieldGroupMode::Local, ConversionFieldGroup{toFieldGroupGuid(androidPreset->guid)}};
    bool compiled = false;
    const auto build = [&](bool force = false) {
        QString error;
        ProjectPackBuildService::Request request;
        request.outputRoot = context.root;
        request.platforms = {context.platform};
        request.options.forceReexport = force;
        request.options.snapshot = ProjectBuildSnapshot::capture(context.project, &error);
        EXPECT_TRUE(request.options.snapshot) << error.toStdString();
        compiled = false;
        request.options.observationCallback = [&](const PackBuildObservation& observation) {
            if (observation.stage == PackBuildObservationStage::CompilingProject) compiled = true;
        };
        return ProjectPackBuildService::run(request);
    };
    const auto mediaRate = [&](const QString& name) -> uint32_t {
        const auto bytes = readFileBytes(context.live.filePath(name + QStringLiteral(".bytes")));
        harken::studio::build::PackCanonicalImage image;
        EXPECT_EQ(VN_OK, harken::studio::build::ParsePackCanonicalImage(
            reinterpret_cast<const std::uint8_t*>(bytes.constData()),
            static_cast<std::uint64_t>(bytes.size()), &image));
        EXPECT_EQ(image.media.size(), 1u);
        return image.media.size() == 1 ? image.media.front().sampleRate : 0;
    };
    const auto mediaReport = [](const auto& buildResult) {
        QMap<QString, QJsonObject> rows;
        EXPECT_EQ(buildResult.platforms.size(), 1u);
        for (const auto& platform : buildResult.platforms) {
            for (const auto& value : platform.result.media) {
                const auto row = value.toObject();
                const auto path = row.value(QStringLiteral("pack")).toString();
                EXPECT_FALSE(rows.contains(path));
                rows.insert(path, row);
            }
        }
        return rows;
    };
    const auto expectPcmReport = [&](const QMap<QString, QJsonObject>& rows) {
        ASSERT_EQ(rows.size(), 2);
        for (auto it = rows.cbegin(); it != rows.cend(); ++it) {
            const auto& row = it.value();
            const auto* asset = AssetRegistry::instance().getAsset(
                static_cast<uint32_t>(row.value(QStringLiteral("sourceId")).toInteger()));
            ASSERT_NE(asset, nullptr);
            const auto bytes = readFileBytes(context.live.filePath(it.key()));
            harken::studio::build::PackCanonicalImage image;
            ASSERT_EQ(VN_OK, harken::studio::build::ParsePackCanonicalImage(
                reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                static_cast<std::uint64_t>(bytes.size()), &image));
            ASSERT_EQ(image.media.size(), 1u);
            EXPECT_EQ(row.value(QStringLiteral("format")).toString(), QStringLiteral("PCM"));
            EXPECT_EQ(row.value(QStringLiteral("originalBytes")).toInteger(),
                QFileInfo(asset->absolutePath).size());
            EXPECT_EQ(row.value(QStringLiteral("outputBytes")).toInteger(),
                static_cast<qint64>(image.media.front().blobBytes));
        }
    };
    const auto initial = build();
    ASSERT_TRUE(initial.allPlatformsSucceeded) << initial.preflightError.toStdString();
    ASSERT_TRUE(initial.finalizationResult.success) << initial.finalizationResult.error.toStdString();
    EXPECT_EQ(mediaRate(QStringLiteral("One")), 24000u);
    EXPECT_EQ(mediaRate(QStringLiteral("Two")), 16000u);
    const auto initialReport = mediaReport(initial);
    ASSERT_NO_FATAL_FAILURE(expectPcmReport(initialReport));
    const auto secondBytes = readFileBytes(context.live.filePath(QStringLiteral("Two.bytes")));
    auto* firstConfig = &firstPreset->comp<VConversion>()->config;
    androidPreset->comp<VConversion>()->config.rules.front().setFormat(ConversionFormat::Vorbis);
    unusedPreset->comp<VConversion>()->config.rules.front().setFormat(ConversionFormat::Vorbis);
    const auto unrelated = build();
    ASSERT_TRUE(unrelated.allPlatformsSucceeded) << unrelated.preflightError.toStdString();
    ASSERT_TRUE(unrelated.finalizationResult.success) << unrelated.finalizationResult.error.toStdString();
    EXPECT_FALSE(compiled);
    EXPECT_EQ(mediaReport(unrelated), initialReport);
    QString error;
    const auto captured = ProjectBuildSnapshot::capture(context.project, &error);
    ASSERT_TRUE(captured) << error.toStdString();
    firstConfig->rules.front().sampleRate = 22050;
    const auto& source = firstSound->comp<VSound>()->sources.plain.entries.front();
    const auto retained = captured->media.inspectRequest(firstSound->guid, source.sourceId,
        QUuid(context.platform), &error);
    ASSERT_TRUE(retained) << error.toStdString();
    ASSERT_TRUE(retained->conversion);
    EXPECT_EQ(retained->conversion->sampleRate, 24000u);
    const auto changed = build();
    ASSERT_TRUE(changed.allPlatformsSucceeded) << changed.preflightError.toStdString();
    ASSERT_TRUE(changed.finalizationResult.success) << changed.finalizationResult.error.toStdString();
    EXPECT_TRUE(compiled);
    EXPECT_EQ(mediaRate(QStringLiteral("One")), 22050u);
    EXPECT_EQ(readFileBytes(context.live.filePath(QStringLiteral("Two.bytes"))), secondBytes);
    const auto changedReport = mediaReport(changed);
    ASSERT_NO_FATAL_FAILURE(expectPcmReport(changedReport));
    EXPECT_EQ(changedReport.value(QStringLiteral("Two.bytes")),
        initialReport.value(QStringLiteral("Two.bytes")));
    ASSERT_EQ(changed.platforms.size(), 1u);
    const auto secondRecord = std::find_if(changed.platforms.front().records.begin(),
        changed.platforms.front().records.end(), [](const auto& record) { return record.packName == QStringLiteral("Two"); });
    ASSERT_NE(secondRecord, changed.platforms.front().records.end());
    EXPECT_TRUE(secondRecord->skipped);
    const auto firstBytes = readFileBytes(context.live.filePath(QStringLiteral("One.bytes")));
    const auto forced = build(true);
    ASSERT_TRUE(forced.allPlatformsSucceeded) << forced.preflightError.toStdString();
    ASSERT_TRUE(forced.finalizationResult.success) << forced.finalizationResult.error.toStdString();
    EXPECT_EQ(readFileBytes(context.live.filePath(QStringLiteral("One.bytes"))), firstBytes);
    EXPECT_EQ(readFileBytes(context.live.filePath(QStringLiteral("Two.bytes"))), secondBytes);
    bind(*layer, *secondPreset);
    const auto ancestorChanged = build();
    ASSERT_TRUE(ancestorChanged.allPlatformsSucceeded) << ancestorChanged.preflightError.toStdString();
    ASSERT_TRUE(ancestorChanged.finalizationResult.success) << ancestorChanged.finalizationResult.error.toStdString();
    EXPECT_EQ(mediaRate(QStringLiteral("One")), 16000u);
    EXPECT_EQ(readFileBytes(context.live.filePath(QStringLiteral("Two.bytes"))), secondBytes);
    ASSERT_EQ(ancestorChanged.platforms.size(), 1u);
    const auto overridden = std::find_if(ancestorChanged.platforms.front().records.begin(),
        ancestorChanged.platforms.front().records.end(), [](const auto& record) { return record.packName == QStringLiteral("Two"); });
    ASSERT_NE(overridden, ancestorChanged.platforms.front().records.end());
    EXPECT_TRUE(overridden->skipped);
    bind(*layer, *firstPreset);
    bind(*firstSound, *secondPreset);
    const auto redirected = build();
    ASSERT_TRUE(redirected.allPlatformsSucceeded) << redirected.preflightError.toStdString();
    ASSERT_TRUE(redirected.finalizationResult.success) << redirected.finalizationResult.error.toStdString();
    EXPECT_FALSE(compiled);
    EXPECT_EQ(mediaRate(QStringLiteral("One")), 16000u);
    firstSound->fieldGroups.conversion.shared.mode = FieldGroupMode::Inherit;
    firstConfig->rules.front().sampleRate = 32000;
    const auto inherited = build();
    ASSERT_TRUE(inherited.allPlatformsSucceeded) << inherited.preflightError.toStdString();
    ASSERT_TRUE(inherited.finalizationResult.success) << inherited.finalizationResult.error.toStdString();
    EXPECT_EQ(mediaRate(QStringLiteral("One")), 32000u);
    EXPECT_EQ(readFileBytes(context.live.filePath(QStringLiteral("Two.bytes"))), secondBytes);
    const auto publishedReport = mediaReport(inherited);
    ASSERT_NO_FATAL_FAILURE(expectPcmReport(publishedReport));
    const auto published = readFileBytes(context.live.filePath(QStringLiteral("One.bytes")));
    firstConfig->rules.front().setFormat(ConversionFormat::Vorbis);
    const auto compressed = build();
    QString diagnostic = compressed.preflightError;
    for (const auto& platform : compressed.platforms) diagnostic += platform.result.error;
    ASSERT_TRUE(compressed.allPlatformsSucceeded) << diagnostic.toStdString();
    ASSERT_TRUE(compressed.finalizationResult.success) << compressed.finalizationResult.error.toStdString();
    const auto packed = readFileBytes(context.live.filePath(QStringLiteral("One.bytes")));
    EXPECT_NE(packed, published);
    harken::studio::build::PackCanonicalImage compressedImage;
    ASSERT_EQ(VN_OK, harken::studio::build::ParsePackCanonicalImage(
        reinterpret_cast<const uint8_t*>(packed.constData()), static_cast<uint64_t>(packed.size()), &compressedImage));
    ASSERT_EQ(compressedImage.media.size(), 1u);
    const auto& compressedMedia = compressedImage.media.front();
    ASSERT_EQ(compressedMedia.sampleFormat, HK_PACK_SAMPLE_FORMAT_VORBIS_RESIDENT);
    EXPECT_EQ(compressedMedia.sampleRate, 32000u);
    const auto compressedReport = mediaReport(compressed);
    ASSERT_EQ(compressedReport.size(), 2);
    EXPECT_EQ(compressedReport.value(QStringLiteral("Two.bytes")), publishedReport.value(QStringLiteral("Two.bytes")));
    const auto encodedReport = compressedReport.value(QStringLiteral("One.bytes"));
    EXPECT_EQ(encodedReport.value(QStringLiteral("format")).toString(), QStringLiteral("Vorbis"));
    const auto* asset = AssetRegistry::instance().getAsset(source.sourceId);
    ASSERT_NE(asset, nullptr);
    EXPECT_EQ(encodedReport.value(QStringLiteral("originalBytes")).toInteger(),
        QFileInfo(asset->absolutePath).size());
    const QDir cache(QDir(PathUtils::getDotCacheDir(context.directory.path()))
        .filePath(QStringLiteral("pack/media")));
    const auto encodedFiles = cache.entryInfoList({QStringLiteral("*.vcm")}, QDir::Files);
    ASSERT_EQ(encodedFiles.size(), 1);
    VorbisEncoder::EncodedMedia encoded;
    uint64_t revision = 0;
    uint64_t payloadBytes = 0;
    ASSERT_TRUE(readVorbisCacheArtifact(encodedFiles.front().absoluteFilePath(),
        encoded, revision, nullptr, &error, &payloadBytes)) << error.toStdString();
    EXPECT_GT(payloadBytes, 0u);
    EXPECT_LT(payloadBytes, static_cast<uint64_t>(encodedFiles.front().size()));
    EXPECT_EQ(encodedReport.value(QStringLiteral("outputBytes")).toInteger(),
        static_cast<qint64>(payloadBytes));
    for (bool force : {false, true}) {
        const auto rebuilt = build(force);
        ASSERT_TRUE(rebuilt.allPlatformsSucceeded) << rebuilt.preflightError.toStdString();
        ASSERT_TRUE(rebuilt.finalizationResult.success) << rebuilt.finalizationResult.error.toStdString();
        EXPECT_EQ(packed, readFileBytes(context.live.filePath(QStringLiteral("One.bytes"))));
    }
    const auto remainingBytes = readFileBytes(context.live.filePath(QStringLiteral("Two.bytes")));
    harken::studio::build::PackCanonicalImage previousImage;
    harken::studio::build::PackCanonicalImage remainingImage;
    ASSERT_EQ(VN_OK, harken::studio::build::ParsePackCanonicalImage(
        reinterpret_cast<const std::uint8_t*>(secondBytes.constData()),
        static_cast<std::uint64_t>(secondBytes.size()), &previousImage));
    ASSERT_EQ(VN_OK, harken::studio::build::ParsePackCanonicalImage(
        reinterpret_cast<const std::uint8_t*>(remainingBytes.constData()),
        static_cast<std::uint64_t>(remainingBytes.size()), &remainingImage));
    ASSERT_EQ(previousImage.media.size(), 1u);
    ASSERT_EQ(remainingImage.media.size(), 1u);
    const auto& previousMedia = previousImage.media.front();
    const auto& remainingMedia = remainingImage.media.front();
    EXPECT_EQ(remainingMedia.soundId, previousMedia.soundId);
    EXPECT_EQ(remainingMedia.sampleRate, previousMedia.sampleRate);
    EXPECT_EQ(remainingMedia.channelCount, previousMedia.channelCount);
    EXPECT_EQ(remainingMedia.sampleFormat, previousMedia.sampleFormat);
    EXPECT_EQ(remainingMedia.frameCount, previousMedia.frameCount);
    EXPECT_EQ(remainingMedia.flags, previousMedia.flags);
    EXPECT_EQ(remainingMedia.loopStartFrame, previousMedia.loopStartFrame);
    EXPECT_EQ(remainingMedia.loopEndFrame, previousMedia.loopEndFrame);
    EXPECT_EQ(remainingBytes.mid(static_cast<qsizetype>(remainingMedia.blobOffset),
                                static_cast<qsizetype>(remainingMedia.blobBytes)),
              secondBytes.mid(static_cast<qsizetype>(previousMedia.blobOffset),
                              static_cast<qsizetype>(previousMedia.blobBytes)));
}

TEST(ProjectPackExport, BuildCapturesSourceChangesOnTheFollowingRequest)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup());
    QString error;
    const auto snapshot = harken::studio::ProjectBuildSnapshot::capture(context.project, &error);
    ASSERT_TRUE(snapshot) << error.toStdString();
    const auto build = [&](const std::function<void()>& afterInputs) {
        harken::studio::ProjectPackBuildService::Request request;
        request.outputRoot = context.root;
        request.platforms = {context.platform};
        request.options.snapshot = snapshot;
        request.platformFinished = [&](const auto&) { if (afterInputs) afterInputs(); };
        return harken::studio::ProjectPackBuildService::run(request);
    };
    const auto initial = build({});
    ASSERT_TRUE(initial.allPlatformsSucceeded) << initial.preflightError.toStdString();
    const auto path = context.live.filePath(QStringLiteral("One.bytes"));
    const auto original = readFileBytes(path);
    const auto unchanged = build([&] {
        const auto source = QDir(harken::studio::PathUtils::getAudioSourcesDir(context.directory.path()))
            .filePath(QStringLiteral("tone.wav"));
        auto bytes = readFileBytes(source);
        const float sample = 0.75f;
        ASSERT_GE(bytes.size(), static_cast<qsizetype>(sizeof(sample)));
        std::memcpy(bytes.data() + bytes.size() - sizeof(sample), &sample, sizeof(sample));
        ASSERT_NO_FATAL_FAILURE(writeFileBytes(source, bytes));
    });
    EXPECT_TRUE(unchanged.allPlatformsSucceeded);
    EXPECT_EQ(readFileBytes(path), original);
    const auto changed = build({});
    ASSERT_TRUE(changed.allPlatformsSucceeded) << changed.preflightError.toStdString();
    EXPECT_NE(readFileBytes(path), original);
}


#ifdef Q_OS_WIN
TEST(ProjectPackExport, PublishingDoesNotReadExistingWaveformPayload)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup(true));
    context.project.buildWaveformOutputDir = context.live.filePath(QStringLiteral("Waveforms"));
    const auto first = buildPersistedPackSet(context.project, context.root, context.options);
    ASSERT_TRUE(first.success) << first.error.toStdString();
    const QDir waveforms(context.project.buildWaveformOutputDir);
    const auto names = waveforms.entryList({QStringLiteral("*.vkd")}, QDir::Files);
    ASSERT_FALSE(names.isEmpty());
    const QString path = waveforms.filePath(names.front());
    const HANDLE handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
        FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    const auto close = qScopeGuard([&] { CloseHandle(handle); });
    QString error;
    const auto result = buildPersistedPackSet(context.project, context.root, context.options);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    EXPECT_EQ(result.builtPackCount, 0);
    EXPECT_EQ(result.skippedPackCount, first.builtPackCount);
    EXPECT_TRUE(findByName(readPublication(context.live.path()), QStringLiteral("One"))
        .value(QStringLiteral("packMetadata")).toObject().contains(QStringLiteral("waveforms")));
    const auto finalized = harken::studio::ProjectPackExporter::FinalizePackExport(
        context.root, context.project, {context.platform}, false, {{context.platform, result.outputs}});
    EXPECT_TRUE(finalized.success) << finalized.error.toStdString();
}

TEST(ProjectPackExport, OccupiedOutputReportsFailureWithoutRetry)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup());
    const auto first = context.exporter.ExportAllPacks(context.project, {}, context.options);
    ASSERT_TRUE(first.success) << first.error.toStdString();
    const auto path = context.live.filePath(QStringLiteral("One.bytes"));
    const auto original = readFileBytes(path);
    const auto handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
        FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    ASSERT_NE(handle, INVALID_HANDLE_VALUE);
    const auto close = qScopeGuard([&] { CloseHandle(handle); });
    context.options.forceReexport = true;
    const auto result = context.exporter.ExportAllPacks(context.project, {}, context.options);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.isEmpty());
    EXPECT_EQ(readFileBytes(path), original);
}

#endif

TEST(ProjectPackExport, PartialExportReportsUnwritableRequiredInit)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup());
    const auto first = context.exporter.ExportAllPacks(context.project, {}, context.options);
    ASSERT_TRUE(first.success) << first.error.toStdString();
    const auto path = context.live.filePath(QStringLiteral("Init.bytes"));
    ASSERT_NO_FATAL_FAILURE(writeFileBytes(path, QByteArray("invalid pack")));
    const auto permissions = QFile::permissions(path);
    const auto restore = qScopeGuard([&] { QFile::setPermissions(path, permissions); });
    ASSERT_TRUE(QFile::setPermissions(path, withoutWritePermissions(permissions)));
    context.options.updateSelectedOnly = true;
    context.options.onlyPackPaths = {QStringLiteral("One")};
    const auto result = context.exporter.ExportAllPacks(context.project, {}, context.options);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.isEmpty());
    EXPECT_EQ(readFileBytes(path), QByteArray("invalid pack"));
}

TEST(ProjectPackExport, CancelAfterMediaPreparationKeepsPreviousSet)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    harken::studio::AssetRegistry::instance().clear();
    harken::studio::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("One"), 21, kSoloSoundObjectId}}));
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, soloSourceId));
    harken::studio::ProjectPackExporter exporter;
    harken::studio::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const QDir output(QDir(dir.path()).filePath(QStringLiteral("out")));
    const auto initial = exporter.ExportAllPacks(project, output.path(), opts);
    ASSERT_TRUE(initial.success) << initial.error.toStdString();
    std::map<QString, QByteArray> previous;
    const QStringList names = publicationFiles(output.path());
    for (const QString& name : names) {
        const QByteArray bytes = readFileBytes(output.filePath(name));
        ASSERT_FALSE(bytes.isEmpty()) << name.toStdString();
        previous.emplace(name, bytes);
    }
    auto* sound = project.layers.front().rootItem->children.at(1)->comp<harken::studio::VSound>();
    sound->sources.plain.entries.front().trimStartFrame = 8;
    int preparedVariants = 0;
    bool canceled = false;
    QObject::connect(&exporter, &harken::studio::ProjectPackExporter::progressChanged,
                     &exporter, [&](int progress, const QString& message) {
        if (progress == 100 && message == QStringLiteral("Action pack exported.")) {
            ++preparedVariants;
            if (preparedVariants == project.projectLanguages.size())
                canceled = true;
        }
    });
    opts.cancelCheck = [&] { return canceled; };
    const auto result = exporter.ExportAllPacks(project, output.path(), opts);
    EXPECT_EQ(preparedVariants, project.projectLanguages.size());
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.canceled);
    EXPECT_EQ(result.error, QStringLiteral("Export canceled"));
    for (const auto& [name, bytes] : previous)
        EXPECT_EQ(readFileBytes(output.filePath(name)), bytes) << name.toStdString();
    const QStringList children = output.entryList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QString& name : children)
        EXPECT_FALSE(name.startsWith(QStringLiteral(".harken-pack-")));
    EXPECT_FALSE(QFileInfo::exists(output.filePath(QStringLiteral(".harken-pack-publishing"))));
    harken::studio::AssetRegistry::instance().clear();
}

TEST(ProjectPackExport, FailedPackDoesNotPreventIndependentPacks)
{
    enum class Failure { Output, ActionTiming, ContainerBehavior, InheritedContainerBehavior,
                         InheritedTrackClips, Binding, TrackReference, ContainerReference,
                         UnusedBinding, UnusedPlayback };
    for (const auto failure : {Failure::Output, Failure::ActionTiming,
                               Failure::ContainerBehavior, Failure::InheritedContainerBehavior,
                               Failure::InheritedTrackClips, Failure::Binding,
                               Failure::TrackReference, Failure::ContainerReference,
                               Failure::UnusedBinding, Failure::UnusedPlayback}) {
        SCOPED_TRACE(static_cast<int>(failure));
        PackWriteAccessProject context;
        ASSERT_NO_FATAL_FAILURE(context.setup());
        auto& actions = context.project.actions.front().rootItem;
        auto independent = makeItem(QStringLiteral("Two"), harken::studio::ProjectItem::Type::Action, 22);
        harken::studio::VActionStep play;
        play.type = harken::studio::VActionStep::Type::Play;
        play.targetObjectID = kSoloSoundObjectId;
        independent->comp<harken::studio::VAction>()->steps.push_back(play);
        addChild(actions, independent);
        auto& invalid = actions->children.front()->comp<harken::studio::VAction>()->steps.front();
        if (failure == Failure::Output) {
            ASSERT_TRUE(QDir().mkpath(context.live.filePath(QStringLiteral("One.bytes"))));
        } else if (failure == Failure::ActionTiming) {
            invalid.quantize.timing = harken::studio::QuantizeTiming::EntryMarker;
        } else if (failure == Failure::Binding || failure == Failure::UnusedBinding) {
            auto layer = context.project.layers.front().rootItem;
            auto host = layer->children.front();
            if (failure == Failure::UnusedBinding) {
                host = makeItem(QStringLiteral("Unused"), harken::studio::ProjectItem::Type::Sound, 40);
                addChild(layer, host);
            }
            auto variable = makeItem(QStringLiteral("Drive"), harken::studio::ProjectItem::Type::Variable, 50);
            context.project.variables.push_back({variable, QStringLiteral("Variables")});
            harken::studio::ParameterBindingDef binding;
            binding.guid = harken::studio::authoringUuid(harkentest::StudioObjectId(500));
            binding.variable = variable->guid;
            binding.address = harken::studio::BindingSemantics::volumeAddress();
            binding.curve = {{0.0f, -6.0f}, {1.0f, 0.0f}};
            auto& bindings = host->comp<harken::studio::VMixNode>()->bindings;
            ASSERT_TRUE(bindings.upsert(binding));
            bindings.find(binding.runtimeId())->transitionMs = std::numeric_limits<uint32_t>::max();
        } else if (failure == Failure::TrackReference || failure == Failure::UnusedPlayback) {
            auto layer = context.project.layers.front().rootItem;
            auto track = makeItem(QStringLiteral("Track"), harken::studio::ProjectItem::Type::Track, 40);
            addChild(layer, track);
            harken::studio::Clip clip;
            clip.clipGuid = QUuid::createUuid();
            clip.targetGuid = layer->children.front()->guid;
            clip.duration = 1000000;
            auto* timeline = track->comp<harken::studio::VTrack>();
            ASSERT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
            timeline->clips.items.push_back(clip);
            if (failure == Failure::TrackReference)
                invalid.targetObjectID = 40;
        } else if (failure == Failure::InheritedTrackClips) {
            auto layer = context.project.layers.front().rootItem;
            auto sound = layer->children.front();
            auto privateSound = layer->children.at(1);
            layer->children.erase(layer->children.begin(), layer->children.begin() + 2);
            auto track = makeItem(QStringLiteral("Track"), harken::studio::ProjectItem::Type::Track, 40);
            addChild(layer, track);
            addChild(track, sound);
            addChild(track, privateSound);
            auto* timeline = track->comp<harken::studio::VTrack>();
            for (const auto& target : {sound, privateSound}) {
                harken::studio::Clip clip;
                clip.clipGuid = QUuid::createUuid();
                clip.targetGuid = target->guid;
                clip.duration = 1000000;
                ASSERT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
                timeline->clips.items.push_back(clip);
            }
            invalid.targetObjectID = 40;
            invalid.quantize.timing = harken::studio::QuantizeTiming::EntryMarker;
            independent->comp<harken::studio::VAction>()->steps.front().targetObjectID = kSharedSoundObjectId;
        } else {
            auto layer = context.project.layers.front().rootItem;
            auto sound = layer->children.front();
            layer->children.erase(layer->children.begin());
            auto container = makeItem(QStringLiteral("Container"), harken::studio::ProjectItem::Type::Container, 40);
            addChild(layer, container);
            addChild(container, sound);
            ASSERT_TRUE(harken::studio::PlayablePropertyAuthoringV1::addContainerChildOccurrence(*container, sound->guid));
            if (failure == Failure::ContainerReference) {
                container->children.clear();
                addChild(layer, sound);
            } else {
                container->comp<harken::studio::VSelection>()->behavior.avoidRepeat = -1;
            }
            invalid.targetObjectID = 40;
            if (failure == Failure::InheritedContainerBehavior)
                independent->comp<harken::studio::VAction>()->steps.front().targetObjectID = kSharedSoundObjectId;
        }
        std::vector<harken::studio::ProjectPackExporter::PackBuildRecord> records;
        const auto result = context.exporter.ExportAllPacks(context.project, {}, context.options, &records);
        SCOPED_TRACE(result.error.toStdString());
        const bool unused = failure == Failure::UnusedBinding || failure == Failure::UnusedPlayback;
        EXPECT_EQ(result.success, unused);
        EXPECT_EQ(result.error.isEmpty(), unused);
        EXPECT_EQ(QFileInfo(context.live.filePath(QStringLiteral("One.bytes"))).isDir(),
                  failure == Failure::Output);
        EXPECT_EQ(QFileInfo(context.live.filePath(QStringLiteral("One.bytes"))).isFile(), unused);
        ASSERT_NO_FATAL_FAILURE(expectCanonicalPackParsesWithPartition(context.live.filePath(QStringLiteral("Init.bytes"))));
        ASSERT_NO_FATAL_FAILURE(expectCanonicalPackParsesWithPartition(context.live.filePath(QStringLiteral("Two.bytes"))));
        EXPECT_TRUE(QFileInfo::exists(context.live.filePath(QStringLiteral("Two.json"))));
        const auto failed = std::find_if(records.begin(), records.end(), [](const auto& record) {
            return record.packName == QStringLiteral("One");
        });
        const auto completed = std::find_if(records.begin(), records.end(), [](const auto& record) {
            return record.packName == QStringLiteral("Two");
        });
        ASSERT_NE(failed, records.end());
        ASSERT_NE(completed, records.end());
        EXPECT_EQ(failed->success, unused);
        EXPECT_EQ(failed->error.isEmpty(), unused);
        EXPECT_TRUE(completed->success);
    }
}

TEST(ProjectPackExport, FailedPlatformDoesNotPreventRemainingPlatforms)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup());
    ASSERT_TRUE(QDir().mkpath(context.live.filePath(QStringLiteral("Init.bytes"))));
    QString error;
    harken::studio::ProjectPackBuildService::Request request;
    request.outputRoot = context.root;
    request.platforms = HarkenTest::platformIdStrings({"Windows", "Android"});
    request.options.snapshot = harken::studio::ProjectBuildSnapshot::capture(context.project, &error);
    ASSERT_TRUE(request.options.snapshot) << error.toStdString();
    const auto result = harken::studio::ProjectPackBuildService::run(request);
    EXPECT_FALSE(result.allPlatformsSucceeded);
    ASSERT_EQ(result.platforms.size(), 2u);
    EXPECT_FALSE(result.platforms.front().result.success);
    EXPECT_TRUE(result.platforms.back().result.success) << result.platforms.back().result.error.toStdString();
    ASSERT_NO_FATAL_FAILURE(expectCanonicalPackParsesWithPartition(
        QDir(result.platforms.back().outputDir).filePath(QStringLiteral("One.bytes"))));
}
