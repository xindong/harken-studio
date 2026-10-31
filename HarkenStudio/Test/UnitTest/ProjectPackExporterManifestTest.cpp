#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Engine.h"
#include "UnitTest/TestUtil.h"
#include "UnitTest/CanonicalPackSetActivate.h"

#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/AssetConverter.h"
#include "AssetPipeline/PathUtils.h"
#include "AssetPipeline/WaveformCache.h"
#include "Components/VAction.h"
#include "Components/VAudio.h"
#include "Components/VBus.h"
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
#include "ProjectSerializer.h"
#include "ProjectSchemaGenerated.h"
#include "ItemTypeCatalog.h"
#include "NameValidator.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "RuntimeContentBundleBuilder.h"
#include "harken_buildinfo.h"

#include <harken/Harken.h>
#include <harken/core/CurrentWorldCodec.h>
#include <harken/core/MediaContentRevision.h>
#include <harken/core/detail/CurrentWorldValidation.h>
#include <harken/media/CanonicalBusSnapshot.h>
#include <harken/media/PackFormat.h>
#include <harken/media/PackFormatV2.h>
#include <harken/media/PackLogicImage.h>

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
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <vector>
#include "Support/ProjectPlatformTestUtils.h"

#ifndef HARKENIZER_PATH
#error "HARKENIZER_PATH must point at the Harkenizer executable"
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

std::shared_ptr<VNX::ProjectItem> makeItem(const QString& name,
                                           VNX::ProjectItem::Type type,
                                           uint32_t objectID)
{
    auto item = std::make_shared<VNX::ProjectItem>();
    item->name = name;
    item->type = type;
    item->objectID = objectID;
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

uint32_t registerPcmAsset(const QString& projectRoot,
                         const QString& relativePath,
                         const std::vector<float>& samples,
                         uint16_t channels)
{
    constexpr uint32_t kSampleRate = 48000;
    const auto frames = static_cast<uint32_t>(samples.size() / channels);
    const QString audioRoot = VNX::PathUtils::getAudioSourcesDir(projectRoot);
    const QString sourcePath = QDir(audioRoot).filePath(relativePath);
    writeWavF32(sourcePath, samples, channels, kSampleRate);

    VNX::AssetInfo info;
    info.relativePath = relativePath;
    info.absolutePath = sourcePath;
    info.sourceID = VNX::AssetRegistry::instance().allocateStableID();
    EXPECT_NE(info.sourceID, 0u);
    info.metadata.sampleRate = kSampleRate;
    info.metadata.channels = channels;
    info.metadata.bitDepth = 32;
    info.metadata.totalSamples = frames;
    info.metadata.fileSize = static_cast<uint64_t>(QFileInfo(sourcePath).size());
    info.lastModified = static_cast<uint64_t>(
        QFileInfo(sourcePath).lastModified().toSecsSinceEpoch());
    EXPECT_TRUE(VNX::AssetRegistry::instance().registerAsset(info));
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

void buildProject(VNX::ProjectData& project,
                  const QString& projectRoot,
                  uint32_t sharedSourceId,
                  uint32_t soloSourceId,
                  const std::vector<ActionSpec>& actionSpecs)
{
    project.platforms = HarkenTest::platforms({"Windows", "Android"});
    project.name = QStringLiteral("manifest-pack");
    project.projectPath =
        QDir(projectRoot).filePath(QStringLiteral("manifest-pack.vproj"));

    for (uint32_t sourceID : {sharedSourceId, soloSourceId})
    {
        const VNX::AssetInfo* info = VNX::AssetRegistry::instance().getAsset(sourceID);
        ASSERT_NE(info, nullptr);
        VNX::AudioSourceData sourceData;
        sourceData.sourceID = sourceID;
        sourceData.relativePath = info->relativePath;
        project.audioSources.push_back(sourceData);
    }

    auto master = makeItem(QStringLiteral("MasterBus"), VNX::ProjectItem::Type::Bus, 201);
    master->guid = VNX::authoringUuid(harkentest::StudioObjectId(0x4d4153544552));
    VNX::ProjectFileBinding busBinding;
    busBinding.rootItem = master;
    project.busses.push_back(std::move(busBinding));

    auto layer = makeItem(QStringLiteral("Root"), VNX::ProjectItem::Type::Layer, 10);
    layer->fieldGroups = VNX::MakeRootFieldGroups(VNX::toFieldGroupGuid(master->guid));
    auto sharedSound = makeItem(QStringLiteral("SharedTone"),
                                VNX::ProjectItem::Type::Sound, kSharedSoundObjectId);
    HarkenTest::setSharedSoundSource(*sharedSound, sharedSourceId);
    addChild(layer, sharedSound);
    auto soloSound = makeItem(QStringLiteral("SoloTone"),
                              VNX::ProjectItem::Type::Sound, kSoloSoundObjectId);
    HarkenTest::setSharedSoundSource(*soloSound, soloSourceId);
    addChild(layer, soloSound);
    VNX::ProjectFileBinding layerBinding;
    layerBinding.rootItem = layer;
    project.layers.push_back(std::move(layerBinding));

    auto actionsRoot = makeItem(QStringLiteral("ActionsRoot"),
                                VNX::ProjectItem::Type::None, 20);
    for (const ActionSpec& spec : actionSpecs)
    {
        auto action = makeItem(spec.name, VNX::ProjectItem::Type::Action, spec.objectID);
        VNX::VActionStep play;
        play.type = VNX::VActionStep::Type::Play;
        play.targetObjectID = spec.targetObjectID;
        action->comp<VNX::VAction>()->steps.push_back(play);
        addChild(actionsRoot, action);
    }
    VNX::ProjectFileBinding actionBinding;
    actionBinding.rootItem = actionsRoot;
    project.actions.push_back(std::move(actionBinding));
}

void localizeSoloSound(VNX::ProjectData& project, uint32_t sourceId)
{
    project.projectLanguages = {QStringLiteral("English"), QStringLiteral("ja")};
    auto* sound = project.layers.front().rootItem->children.at(1)->comp<VNX::VSound>();
    ASSERT_NE(sound, nullptr);
    sound->distinguishLanguage = true;
    sound->sources.localized.clear();
    for (const QString& language : project.projectLanguages) {
        VNX::SoundSourceDef source;
        source.entryId = QUuid::createUuid();
        source.sourceId = sourceId;
        sound->sources.localized[language].entries.push_back(source);
        sound->sources.localized[language].platforms[HarkenTest::platformIdString("Windows")].active = source.entryId;
    }
}

QJsonArray readPublication(const QString& outputDir, const QString& language = {},
                           bool requireComplete = true,
                           const QVector<VNX::PackArtifactMetadata>& artifacts = {})
{
    QJsonArray packs;
    QStringList files;
    QString error;
    QStringList expectedPaths;
    for (const auto& artifact : artifacts) {
        const auto relative = QDir(outputDir).relativeFilePath(artifact.outputPath);
        if (!relative.startsWith(QStringLiteral("../"))) expectedPaths.append(relative);
    }
    EXPECT_TRUE(VNX::ReadPackPublication(outputDir, {}, {}, &packs, &files, &error,
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
    EXPECT_TRUE(VNX::ReadPackPublication(outputDir, {}, {}, &packs, &files, &error))
        << error.toStdString();
    QStringList pairs;
    for (const auto& value : packs) {
        const auto path = value.toObject().value(QStringLiteral("path")).toString();
        pairs << path << path.chopped(6) + QStringLiteral(".json");
    }
    pairs.sort();
    return pairs;
}

QJsonArray actionCompanions(const QJsonArray& packs)
{
    QJsonArray actions;
    for (const auto& value : packs) {
        const auto row = value.toObject();
        if (row.value(QStringLiteral("packKind")).toInt() != 2)
            actions.append(row.value(QStringLiteral("companion")));
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

QString variantFileName(const VNX::ProjectData& project, harkenstudio::ObjectId stableId, const QString& language = QString())
{
    for (const auto& unit : VNX::ProjectPackExporter::CollectActionPackUnits(project))
        if (unit.stableId == stableId)
            return VNX::PackBuildPlanning::packFileName(language, unit.name, QStringLiteral(".bytes"));
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

void readCanonicalWorld(const QString& path, QByteArray& bytes,
                         harken::PackCanonicalImage& image,
                         harken::CurrentWorldBodyImage& world)
{
    bytes = readFileBytes(path);
    ASSERT_EQ(VN_OK, harken::ParsePackCanonicalImage(
        reinterpret_cast<const std::uint8_t*>(bytes.constData()),
        static_cast<std::uint64_t>(bytes.size()), &image));
    ASSERT_TRUE(image.hasPartition);
    ASSERT_TRUE(world.Decode(image.partitionBody.data(), image.partitionBody.size(),
                             image.partitionHeader.bodyVintage,
                             harken::CurrentWorldDecodeMode::StrictArtifact));
    ASSERT_NE(world.ProductionView(), nullptr);
}

void activateExportedNames(const QString& outputDir, const QStringList& names,
                          const QString& language = {},
                          std::map<QString, VnPackHandle>* handles = nullptr)
{
    std::vector<QJsonObject> packs;
    for (const auto& value : readPublication(outputDir, language)) packs.push_back(value.toObject());
    std::sort(packs.begin(), packs.end(), [](const auto& left, const auto& right) {
        const bool leftInit = left.value(QStringLiteral("packKind")).toInt() == 2;
        const bool rightInit = right.value(QStringLiteral("packKind")).toInt() == 2;
        if (leftInit != rightInit) return leftInit;
        return left.value(QStringLiteral("packStableId")).toString().toULongLong()
            < right.value(QStringLiteral("packStableId")).toString().toULongLong();
    });
    std::vector<QByteArray> paths;
    std::vector<VnCanonicalPackSetMemberV1> members;
    QStringList memberNames;
    const auto root = QFileInfo(outputDir).absoluteFilePath().toUtf8();
    for (const auto& pack : packs) {
        const auto name = pack.value(QStringLiteral("name")).toString();
        const auto variant = pack.value(QStringLiteral("language")).toString();
        if ((name != QStringLiteral("Init") && !names.contains(name))
            || (!variant.isEmpty() && variant != language)) continue;
        paths.push_back(QDir(outputDir).filePath(pack.value(QStringLiteral("path")).toString()).toUtf8());
        VnCanonicalPackSetMemberV1 member{};
        member.structSize = sizeof(member);
        member.flags = VN_CANONICAL_PACK_SET_MEMBER_ROOT;
        member.packKind = name == QStringLiteral("Init")
            ? VN_CANONICAL_PACK_SET_MEMBER_INIT : VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
        member.packStableId = pack.value(QStringLiteral("packStableId")).toString().toULongLong();
        member.utf8ContainmentRoot = root.constData();
        members.push_back(member);
        memberNames.push_back(name);
    }
    ASSERT_EQ(members.size(), names.size() + 1u);
    std::vector<VnCanonicalPackSetMemberResultV1> rows(members.size());
    for (size_t i = 0; i < members.size(); ++i) {
        members[i].utf8Path = paths[i].constData();
        rows[i].structSize = sizeof(rows[i]);
    }
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(
        members.data(), static_cast<uint32_t>(members.size()), rows.data(), report));
    if (handles)
        for (size_t i = 0; i < rows.size(); ++i)
            handles->emplace(memberNames[i], rows[i].handle);
}

void expectMediaMatchesDefinition(const QByteArray& bytes,
                                  const harken::PackTocEntry& media,
                                  const harken::PlayablePropertyProgramViewV1& property)
{
    ASSERT_EQ(media.soundId, property.stableId);
    ASSERT_EQ(media.sampleFormat, harken::kPackSampleFormatF32Planar);
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
    const bool loopable = (media.flags & harken::kPackFlagLoopable) != 0;
    ASSERT_TRUE(harken::ComputeMediaPcmContentRevisionV1(
        media.sampleRate, media.channelCount, media.frameCount, channels.data(),
        1, &revision, loopable ? media.loopStartFrame : 0,
        loopable ? media.loopEndFrame : 0));
    EXPECT_EQ(property.contentRevision, revision);
}

void expectPackRendersInIndependentSession(const QString& outputDir,
                                          const QString& initPath,
                                          const QString& contentPath,
                                          std::uint64_t packStableId,
                                          harkenstudio::ObjectId actionStableId)
{
    harkentest::EngineSession session(harkentest::MakeNullConfig(8, 256, 48000));
    ASSERT_EQ(session.InitResult(), VN_OK);
    ASSERT_NE(session.Engine(), nullptr);
    const QByteArray rootUtf8 = QFileInfo(outputDir).absoluteFilePath().toUtf8();
    const QByteArray initUtf8 = QFileInfo(initPath).absoluteFilePath().toUtf8();
    const QByteArray initBytes = readFileBytes(initPath);
    harken::PackCanonicalImage initImage;
    ASSERT_EQ(VN_OK, harken::ParsePackCanonicalImage(
        reinterpret_cast<const uint8_t*>(initBytes.constData()),
        static_cast<uint64_t>(initBytes.size()), &initImage));
    ASSERT_TRUE(initImage.hasPartition);
    const QByteArray contentUtf8 = QFileInfo(contentPath).absoluteFilePath().toUtf8();
    VnCanonicalPackSetMemberV1 members[2]{};
    VnCanonicalPackSetMemberResultV1 rows[2]{};
    for (size_t i = 0; i < 2; ++i) {
        members[i].structSize = sizeof(members[i]);
        members[i].flags = VN_CANONICAL_PACK_SET_MEMBER_ROOT;
        members[i].utf8ContainmentRoot = rootUtf8.constData();
        rows[i].structSize = sizeof(rows[i]);
    }
    members[0].packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
    members[0].packStableId = initImage.partitionHeader.packStableId;
    members[0].utf8Path = initUtf8.constData();
    members[1].packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
    members[1].packStableId = packStableId;
    members[1].utf8Path = contentUtf8.constData();
    VnCanonicalPackSetActivationReportV1 activation{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(members, 2, rows, activation));
    ASSERT_NE(0u, rows[0].handle);
    ASSERT_NE(0u, rows[1].handle);
    VnPostActionDesc post{};
    post.structSize = sizeof(post);
    post.actionId = harkentest::ToRuntime(actionStableId);
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&post.emitter));
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&post, &firing));
    ASSERT_NE(firing, 0u);
    VnActionStepResult step{};
    step.structSize = sizeof(step);
    ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 0, &step));
    EXPECT_EQ(step.state, VN_ACTION_STEP_ACTIVE);
    EXPECT_NE(step.producedPlayableInstance, 0u);
    std::vector<float> capture;
    for (int block = 0; block < 16; ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        session.Engine()->nullBackend.Pump(1);
        const float* samples = session.Engine()->nullBackend.LastBlock();
        capture.insert(capture.end(), samples,
                       samples + session.Engine()->nullBackend.BlockFrames());
    }
    HARKENTEST_ASSERT_NON_SILENT(capture);
    const VnResult released = Vn_ReleaseActionFiring(firing);
    EXPECT_TRUE(released == VN_OK || released == VN_IN_USE);
}

void preserveUnityFixture(const QString& sourceRoot)
{
    const QByteArray targetUtf8 = qgetenv("HARKEN_UNITY_FIXTURE_OUT");
    if (targetUtf8.isEmpty())
        return;
    const QString targetRoot = QString::fromUtf8(targetUtf8);
    QDir target(targetRoot);
    if (target.exists())
        ASSERT_TRUE(target.removeRecursively());
    ASSERT_TRUE(QDir().mkpath(targetRoot));
    QDirIterator files(sourceRoot, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString source = files.next();
        const QString relative = QDir(sourceRoot).relativeFilePath(source);
        const QString destination = target.filePath(relative);
        ASSERT_TRUE(QDir().mkpath(QFileInfo(destination).absolutePath()));
        ASSERT_TRUE(QFile::copy(source, destination))
            << destination.toStdString();
    }
}

void expectCanonicalPackParsesWithPartition(const QString& path)
{
    const QByteArray bytes = readFileBytes(path);
    harken::PackCanonicalImage image;
    ASSERT_EQ(VN_OK,
              harken::ParsePackCanonicalImage(
                  reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                  static_cast<std::uint64_t>(bytes.size()), &image))
        << path.toStdString();
    EXPECT_TRUE(image.hasPartition) << path.toStdString();
    EXPECT_EQ(harken::kCanonicalDefinitionPartitionBodyVintage,
              image.partitionHeader.bodyVintage)
        << path.toStdString();
}

void expectInitHasVolumeOnlyKind24(const QString& path)
{
    const QByteArray bytes = readFileBytes(path);
    harken::PackCanonicalImage image;
    ASSERT_EQ(VN_OK,
              harken::ParsePackCanonicalImage(
                  reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                  static_cast<std::uint64_t>(bytes.size()), &image))
        << path.toStdString();
    ASSERT_TRUE(image.hasBusDefinitionSnapshot) << path.toStdString();
    ASSERT_TRUE(image.hasPartition) << path.toStdString();
    EXPECT_EQ(image.requirement.schemaVersion, 2u) << path.toStdString();
    EXPECT_EQ(image.partitionHeader.headerBytes,
              harken::kCanonicalDefinitionPartitionHeaderV2Bytes)
        << path.toStdString();
    const auto& snapshot = image.busDefinitionSnapshotHeader;
    EXPECT_EQ(snapshot.schemaVersion, 2u) << path.toStdString();
    EXPECT_EQ(snapshot.storageModelVersion, 2u) << path.toStdString();
    EXPECT_EQ(snapshot.headerBytes, harken::kPackBusDefinitionSnapshotV2HeaderBytes)
        << path.toStdString();
    EXPECT_EQ(snapshot.publicationCapabilityVintage, 4u) << path.toStdString();
    const auto expected = harken::SnapshotBusTriple(snapshot);
    EXPECT_TRUE(harken::CanonicalBusTripleFullyNonzero(expected))
        << path.toStdString();
    EXPECT_EQ(snapshot.contentGeneration, snapshot.publishedBusFingerprint)
        << path.toStdString();
    EXPECT_EQ(snapshot.publishedBusFingerprint,
              harken::CanonicalVolumeBusFingerprint(image))
        << path.toStdString();
    EXPECT_TRUE(harken::CanonicalBusTripleEqual(
        harken::RequirementBusTriple(image.requirement), expected))
        << path.toStdString();
    EXPECT_TRUE(harken::CanonicalBusTripleEqual(
        harken::PartitionBusTriple(image.partitionHeader), expected))
        << path.toStdString();
    for (const auto& edge : image.dependencyEdges) {
        const bool initRequired =
            edge.edgeKind == harken::kPackDependencyEdgeKindInit &&
            edge.required01 == 1;
        if (initRequired) {
            EXPECT_TRUE(harken::CanonicalBusTripleEqual(
                harken::DependencyBusTriple(edge), expected))
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
    return harken::Fnv1a64(utf8.constData(), static_cast<std::size_t>(utf8.size()));
}

std::uint32_t packKindCount(const QString& path, std::uint32_t kind)
{
    const QByteArray bytes = readFileBytes(path);
    harken::PackV2Header header;
    if (!harken::ParsePackV2Header(
            reinterpret_cast<const std::uint8_t*>(bytes.constData()), &header))
        return 0;
    std::uint32_t count = 0;
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const harken::PackV2DirectoryEntry record = harken::ParsePackV2DirectoryEntry(
            reinterpret_cast<const std::uint8_t*>(bytes.constData()) +
            header.directoryOffset + i * harken::kPackV2DirectoryEntryBytes);
        if (record.kind == kind)
            ++count;
    }
    return count;
}

bool packHasKind(const QString& path, std::uint32_t kind)
{
    return packKindCount(path, kind) != 0;
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
        const auto companion = row.take(QStringLiteral("companion")).toObject();
        const QString current = row.value(QStringLiteral("path")).toString();
        ASSERT_TRUE(current.endsWith(QStringLiteral(".bytes")));
        QString logical = current.chopped(6) + QStringLiteral(".pack");
        if (row.value(QStringLiteral("packKind")).toInt() == 2) {
            for (const auto& variableValue : companion.value(QStringLiteral("variables")).toArray()) {
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
            actions.append(QJsonObject{{QStringLiteral("name"), companion.value(QStringLiteral("name"))},
                {QStringLiteral("actionStableId"), companion.value(QStringLiteral("guid"))},
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

TEST(EditorProjectPackExporterManifest, SharedPlanningPreservesVariantCapabilitiesAndDiagnostics)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    const auto clearAssets = qScopeGuard([] { VNX::AssetRegistry::instance().clear(); });
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    const auto sharedSource = registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const auto soloSource = registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), sharedSource, soloSource,
        {{QStringLiteral("Common"), 21, kSharedSoundObjectId},
         {QStringLiteral("Localized"), 22, kSoloSoundObjectId},
         {QStringLiteral("CommonAgain"), 23, kSharedSoundObjectId}}));
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, soloSource));
    const auto build = VNX::ProjectPackExporter::createBuildContext(project);
    VNX::RuntimeCapabilityContext cached;
    cached.buildSession = build->capabilities;
    const auto platforms = project.platformIds();
    const auto directPlan = VNX::PackBuildPlanning::build(project, platforms, {}, true);
    ASSERT_TRUE(std::any_of(directPlan.variants.begin(), directPlan.variants.end(),
        [](const auto& variant) { return variant.capability.permitted(); }));
    for (int pass = 0; pass < 2; ++pass) {
        const auto sharedPlan = VNX::PackBuildPlanning::build(project, platforms, {}, true, {},
            cached, build->closures.get());
        EXPECT_EQ(directPlan.toJson(project), sharedPlan.toJson(project));
    }
    std::vector<const VNX::ProjectItem*> items;
    for (const auto* binding : project.allFileBindings()) {
        const auto collect = [&](const auto& self, const VNX::ProjectItem* item) -> void {
            items.push_back(item);
            for (const auto& child : item->children) self(self, child.get());
        };
        if (binding && binding->rootItem) collect(collect, binding->rootItem.get());
    }
    for (const auto& platform : platforms) {
        for (const auto& language : project.projectLanguages) {
            const VNX::BuildVariant variant{platform, language};
            for (const auto* item : items) {
                for (const auto& entry : VNX::RuntimeCapabilityMatrix::entries()) {
                    if (entry.objectType != item->type) continue;
                    const auto direct = VNX::RuntimeCapabilityMatrix::evaluateForContext(
                        *item, project, entry.operation, entry.feature, variant, {});
                    const auto shared = VNX::RuntimeCapabilityMatrix::evaluateForContext(
                        *item, project, entry.operation, entry.feature, variant, cached);
                    EXPECT_EQ(direct.toJson(), shared.toJson())
                        << item->name.toStdString() << " " << platform.toStdString()
                        << " " << language.toStdString()
                        << " " << VNX::RuntimeCapabilityMatrix::featureName(entry.feature).toStdString();
                }
            }
        }
    }
    const auto& localized = *project.layers.front().rootItem->children.at(1);
    for (const auto& platform : platforms) {
        for (const auto& language : project.projectLanguages) {
            VNX::RuntimeCapabilityContext direct{QUuid::fromString(platform), language};
            auto shared = direct;
            shared.buildSession = build->capabilities;
            for (const auto& variant : {std::optional<VNX::BuildVariant>{},
                    std::optional<VNX::BuildVariant>{VNX::BuildVariant{}}}) {
                const auto expected = VNX::RuntimeCapabilityMatrix::assessForContext(
                    localized, project, VNX::RuntimeCapabilityOperation::PackBuild, variant, direct);
                const auto actual = VNX::RuntimeCapabilityMatrix::assessForContext(
                    localized, project, VNX::RuntimeCapabilityOperation::PackBuild, variant, shared);
                EXPECT_EQ(expected.toJson(), actual.toJson());
            }
        }
    }
    project.actions.front().rootItem->children.back()->comp<VNX::VAction>()->steps.front().targetObjectID = 999;
    const auto invalidBuild = VNX::ProjectPackExporter::createBuildContext(project);
    VNX::RuntimeCapabilityContext invalidContext;
    invalidContext.buildSession = invalidBuild->capabilities;
    const auto invalidDirect = VNX::PackBuildPlanning::build(project, platforms, {}, true);
    ASSERT_FALSE(invalidDirect.ready());
    const auto invalidShared = VNX::PackBuildPlanning::build(project, platforms, {}, true, {},
        invalidContext, invalidBuild->closures.get());
    EXPECT_EQ(invalidDirect.toJson(project), invalidShared.toJson(project));
}

TEST(EditorProjectPackExporterManifest, StopOnlyPublishesNoTargetMediaOrDefinitions)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    const auto sharedSource = registerToneAsset(dir.path(), QStringLiteral("music.wav"), 1.0f, 48000);
    const auto soloSource = registerToneAsset(dir.path(), QStringLiteral("unused.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), sharedSource, soloSource,
        {{QStringLiteral("StopMusic"), 21, kSharedSoundObjectId}}));
    project.actions.front().rootItem->children.front()->comp<VNX::VAction>()->steps.front().type =
        VNX::VActionStep::Type::Stop;
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    const auto result = exporter.ExportAllPacks(project, dir.path(), options);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    QByteArray bytes;
    harken::PackCanonicalImage image;
    harken::CurrentWorldBodyImage body;
    ASSERT_NO_FATAL_FAILURE(readCanonicalWorld(QDir(dir.path()).filePath(
        variantFileName(project, harkentest::StudioObjectId(21 + kStableIdBias))), bytes, image, body));
    EXPECT_TRUE(image.media.empty());
    EXPECT_EQ(body.ProductionView()->playablePropertyCount, 0u);
    ASSERT_EQ(body.ProductionView()->actionCount, 1u);
    {
        harkentest::EngineSession session(harkentest::MakeNullConfig(8, 256, 48000));
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_NO_FATAL_FAILURE(activateExportedNames(dir.path(), {QStringLiteral("StopMusic")}));
        VnPostActionDesc post{};
        post.structSize = sizeof(post);
        post.actionId = harkentest::ToRuntime(harkentest::StudioObjectId(21 + kStableIdBias));
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&post.emitter));
        VnActionFiringHandle firing = 0;
        ASSERT_EQ(VN_OK, Vn_PostAction(&post, &firing));
        EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
    }
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, StopControlsKeepLocalizedFingerprintsAndMixedPlaybackMedia)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    const auto early = registerToneAsset(dir.path(), QStringLiteral("early.wav"), 0.25f);
    const auto target = registerToneAsset(dir.path(), QStringLiteral("target.wav"), 0.75f, 48000);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), early, target,
        {{QStringLiteral("PlayEarly"), 21, 1}, {QStringLiteral("PlayTarget"), 22, 2},
         {QStringLiteral("StopTarget"), 23, 2}, {QStringLiteral("PostStop"), 24, 23},
         {QStringLiteral("Mixed"), 25, 2}, {QStringLiteral("StopThenPlay"), 26, 2},
         {QStringLiteral("PlayThenStop"), 27, 2}}));
    auto& actions = project.actions.front().rootItem->children;
    actions[2]->comp<VNX::VAction>()->steps[0].type = VNX::VActionStep::Type::Stop;
    actions[3]->comp<VNX::VAction>()->steps[0].type = VNX::VActionStep::Type::PostAction;
    auto stop = actions[2]->comp<VNX::VAction>()->steps[0];
    auto play = actions[1]->comp<VNX::VAction>()->steps[0];
    actions[4]->comp<VNX::VAction>()->steps = {stop, actions[0]->comp<VNX::VAction>()->steps[0]};
    actions[5]->comp<VNX::VAction>()->steps = {stop, play};
    actions[6]->comp<VNX::VAction>()->steps = {play, stop};
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, target));
    const auto japanese = registerToneAsset(dir.path(), QStringLiteral("target_ja.wav"), 0.4f, 24000);
    VNX::AudioSourceData localizedSource;
    localizedSource.sourceID = japanese;
    localizedSource.relativePath = QStringLiteral("target_ja.wav");
    project.audioSources.push_back(localizedSource);
    project.layers.front().rootItem->children[1]->comp<VNX::VSound>()
        ->sources.localized[QStringLiteral("ja")].entries.front().sourceId = japanese;
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    const auto result = exporter.ExportAllPacks(project, dir.path(), options);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    const auto packs = readPublication(dir.path());
    for (const auto& language : project.projectLanguages) {
        SCOPED_TRACE(language.toStdString());
        QByteArray playBytes;
        harken::PackCanonicalImage playImage;
        harken::CurrentWorldBodyImage playBody;
        ASSERT_NO_FATAL_FAILURE(readCanonicalWorld(QDir(dir.path()).filePath(
            variantFileName(project, harkentest::StudioObjectId(22 + kStableIdBias), language)),
            playBytes, playImage, playBody));
        ASSERT_EQ(playImage.media.size(), 1u);
        ASSERT_EQ(playBody.ProductionView()->playablePropertyCount, 1u);
        const auto fingerprint = playBody.ProductionView()->playableProperties[0].programSemanticFingerprint;
        for (uint32_t id : {23u, 24u, 25u, 26u, 27u}) {
            SCOPED_TRACE(id);
            QByteArray bytes;
            harken::PackCanonicalImage image;
            harken::CurrentWorldBodyImage body;
            ASSERT_NO_FATAL_FAILURE(readCanonicalWorld(QDir(dir.path()).filePath(
                variantFileName(project, harkentest::StudioObjectId(id + kStableIdBias), language)),
                bytes, image, body));
            if (id == 23 || id == 24) {
                EXPECT_TRUE(image.media.empty());
                EXPECT_EQ(body.ProductionView()->playablePropertyCount, 0u);
            } else {
                ASSERT_EQ(image.media.size(), 1u);
                EXPECT_EQ(image.media[0].soundId, harkentest::StudioObjectId((id == 25 ? 1 : 2) + kStableIdBias));
            }
            ASSERT_EQ(body.ProductionView()->actionCount, id == 24 ? 2u : 1u);
            for (uint32_t j = 0; j < body.ProductionView()->actionCount; ++j) {
                const auto& action = body.ProductionView()->actions[j];
                for (uint32_t i = 0; i < action.stepCount; ++i)
                    if (action.steps[i].opcode == harken::ActionOpcodeV1::Stop)
                        EXPECT_EQ(action.steps[i].target.expectedProgramSemanticFingerprint, fingerprint);
            }
        }
        for (const auto& value : packs) {
            const auto pack = value.toObject();
            if (pack.value(QStringLiteral("name")).toString() == QStringLiteral("StopTarget"))
                EXPECT_EQ(pack.value(QStringLiteral("dependencies")).toArray().size(), 1);
        }
        harkentest::EngineSession session(harkentest::MakeNullConfig(8, 256, 48000));
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_NO_FATAL_FAILURE(activateExportedNames(dir.path(),
            {QStringLiteral("PlayEarly"), QStringLiteral("PlayTarget"), QStringLiteral("StopTarget"),
             QStringLiteral("PostStop"), QStringLiteral("Mixed"), QStringLiteral("StopThenPlay"),
             QStringLiteral("PlayThenStop")}, language));
    }
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, StopContainerAndQuantizeReferenceDoNotOwnDescendantMedia)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    for (bool queue : {false, true}) {
        SCOPED_TRACE(queue);
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        VNX::AssetRegistry::instance().clear();
        VNX::AssetRegistry::instance().setProjectRoot(dir.path());
        const auto source = registerToneAsset(dir.path(), QStringLiteral("one.wav"), 0.25f);
        const auto target = registerToneAsset(dir.path(), QStringLiteral("two.wav"), 0.75f);
        auto project = HarkenTest::projectWithPlatforms({"Windows"});
        ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), source, target,
            {{QStringLiteral("PlayContainer"), 21, 40}, {QStringLiteral("StopContainer"), 22, 40},
             {QStringLiteral("GroupedStop"), 23, 40}, {QStringLiteral("GroupedPlay"), 24, 1}}));
        ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, target));
        auto layer = project.layers.front().rootItem;
        auto sound = layer->children[1];
        layer->children.erase(layer->children.begin() + 1);
        auto container = makeItem(QStringLiteral("Container"), VNX::ProjectItem::Type::Container, 40);
        addChild(layer, container);
        addChild(container, sound);
        auto* selection = container->comp<VNX::VSelection>();
        selection->mode = queue ? VNX::SelectionMode::Sequence : VNX::SelectionMode::Random;
        selection->playType = queue ? VNX::SelectionPlayType::Queue : VNX::SelectionPlayType::Single;
        selection->behavior.memoryScope = queue ? VNX::SelectionMemoryScope::PerEmitter
                                                : VNX::SelectionMemoryScope::Global;
        ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::addContainerChildOccurrence(*container, sound->guid));
        auto& actions = project.actions.front().rootItem->children;
        for (size_t index : {1u, 2u}) {
            auto& stop = actions[index]->comp<VNX::VAction>()->steps[0];
            stop.type = VNX::VActionStep::Type::Stop;
            stop.quantize.timing = VNX::QuantizeTiming::NextBeat;
            stop.quantize.referenceObjectID = 40;
        }
        VNX::ProjectPackExporter exporter;
        VNX::ProjectPackExporter::ExportOptions options;
        options.platform = HarkenTest::platformIdString("Windows");
        const auto result = exporter.ExportAllPacks(project, dir.path(), options);
        ASSERT_TRUE(result.success) << result.error.toStdString();
        for (const auto& language : project.projectLanguages) {
            for (const auto& name : {QStringLiteral("StopContainer"), QStringLiteral("GroupedStop")}) {
                QByteArray bytes;
                harken::PackCanonicalImage image;
                harken::CurrentWorldBodyImage body;
                ASSERT_NO_FATAL_FAILURE(readCanonicalWorld(QDir(dir.path()).filePath(
                    language + QStringLiteral("/") + name + QStringLiteral(".bytes")), bytes, image, body));
                EXPECT_EQ(body.ProductionView()->containerCount, 0u);
                EXPECT_TRUE(image.media.empty());
                EXPECT_EQ(body.ProductionView()->playablePropertyCount, 0u);
            }
            harkentest::EngineSession session(harkentest::MakeNullConfig(8, 256, 48000));
            ASSERT_EQ(VN_OK, session.InitResult());
            ASSERT_NO_FATAL_FAILURE(activateExportedNames(dir.path(),
                {QStringLiteral("PlayContainer"), QStringLiteral("StopContainer"), QStringLiteral("GroupedStop"), QStringLiteral("GroupedPlay")}, language));
        }
    }
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, StopTimelineTargetsDoNotPublishSchedulesOrMedia)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    for (const auto type : {VNX::ProjectItem::Type::Track, VNX::ProjectItem::Type::Segment}) {
        for (bool withProvider : {false, true}) {
            SCOPED_TRACE(static_cast<int>(type));
            SCOPED_TRACE(withProvider);
            QTemporaryDir dir;
            ASSERT_TRUE(dir.isValid());
            VNX::AssetRegistry::instance().clear();
            VNX::AssetRegistry::instance().setProjectRoot(dir.path());
            const auto source = registerToneAsset(dir.path(), QStringLiteral("one.wav"), 0.25f);
            const auto other = registerToneAsset(dir.path(), QStringLiteral("two.wav"), 0.75f);
            auto project = HarkenTest::projectWithPlatforms({"Windows"});
            ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), source, other,
                {{QStringLiteral("StopTimeline"), 21, 40}, {QStringLiteral("PlayTimeline"), 22, 40}}));
            auto& actions = project.actions.front().rootItem->children;
            actions[0]->comp<VNX::VAction>()->steps[0].type = VNX::VActionStep::Type::Stop;
            if (!withProvider) actions.pop_back();
            auto target = makeItem(QStringLiteral("Timeline"), type, 40);
            addChild(project.layers.front().rootItem, target);
            auto track = target;
            if (type == VNX::ProjectItem::Type::Segment) {
                track = makeItem(QStringLiteral("Track"), VNX::ProjectItem::Type::Track, 41);
                addChild(target, track);
            }
            auto sound = makeItem(QStringLiteral("ClipSound"), VNX::ProjectItem::Type::Sound, 42);
            HarkenTest::setSharedSoundSource(*sound, source);
            addChild(track, sound);
            VNX::Clip clip;
            clip.clipGuid = QUuid::createUuid();
            clip.targetGuid = sound->guid;
            clip.duration = 1000000;
            auto* timeline = track->comp<VNX::VTrack>();
            ASSERT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
            timeline->clips.items.push_back(clip);
            VNX::ProjectPackExporter exporter;
            VNX::ProjectPackExporter::ExportOptions options;
            options.platform = HarkenTest::platformIdString("Windows");
            const auto result = exporter.ExportAllPacks(project, dir.path(), options);
            ASSERT_TRUE(result.success) << result.error.toStdString();
            QByteArray bytes;
            harken::PackCanonicalImage image;
            harken::CurrentWorldBodyImage body;
            ASSERT_NO_FATAL_FAILURE(readCanonicalWorld(QDir(dir.path()).filePath(
                QStringLiteral("StopTimeline.bytes")), bytes, image, body));
            EXPECT_TRUE(image.media.empty());
            EXPECT_EQ(body.ProductionView()->playablePropertyCount, 0u);
            EXPECT_EQ(body.ProductionView()->trackScheduleCount, 0u);
            EXPECT_EQ(body.ProductionView()->segmentScheduleCount, 0u);
            harkentest::EngineSession session(harkentest::MakeNullConfig(8, 256, 48000));
            ASSERT_EQ(VN_OK, session.InitResult());
            ASSERT_NO_FATAL_FAILURE(activateExportedNames(dir.path(), withProvider
                ? QStringList{QStringLiteral("StopTimeline"), QStringLiteral("PlayTimeline")}
                : QStringList{QStringLiteral("StopTimeline")}));
        }
    }
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, MediaFreeDelayedStopCapturesOnlyExistingInstances)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    const auto source = registerToneAsset(dir.path(), QStringLiteral("long.wav"), 0.75f, 96000);
    const auto unused = registerToneAsset(dir.path(), QStringLiteral("unused.wav"), 0.25f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), source, unused,
        {{QStringLiteral("Play"), 21, 1}, {QStringLiteral("Stop"), 22, 1}}));
    auto& stop = project.actions.front().rootItem->children[1]->comp<VNX::VAction>()->steps[0];
    stop.type = VNX::VActionStep::Type::Stop;
    stop.delay = 500.0f;
    stop.fadeTime = 10.0f;
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    const auto result = exporter.ExportAllPacks(project, dir.path(), options);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    for (int scenario = 0; scenario != 6; ++scenario) {
        SCOPED_TRACE(scenario);
        harkentest::EngineSession session(harkentest::MakeNullConfig(8, 256, 48000));
        ASSERT_EQ(VN_OK, session.InitResult());
        std::map<QString, VnPackHandle> handles;
        ASSERT_NO_FATAL_FAILURE(activateExportedNames(dir.path(), scenario == 0
            ? QStringList{QStringLiteral("Stop")}
            : QStringList{QStringLiteral("Play"), QStringLiteral("Stop")}, {}, &handles));
        VnPostActionDesc post{};
        post.structSize = sizeof(post);
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&post.emitter));
        VnActionFiringHandle playFiring = 0, stopFiring = 0;
        const auto postAction = [&](uint32_t id, VnActionFiringHandle* firing) {
            post.actionId = harkentest::ToRuntime(harkentest::StudioObjectId(id + kStableIdBias));
            return Vn_PostAction(&post, firing);
        };
        if (scenario >= 2) ASSERT_EQ(VN_OK, postAction(21, &playFiring));
        ASSERT_EQ(VN_OK, postAction(22, &stopFiring));
        EXPECT_EQ(VN_IN_USE, Vn_ReleaseActionFiring(stopFiring));
        ASSERT_EQ(1u, session.Engine()->actionDeferredRecords.LiveCount());
        harken::ActionDeferredRecordToken token{};
        harken::ActionDeferredRecord record{};
        harken::ActionDeferredRecordState state{};
        ASSERT_TRUE(session.Engine()->actionDeferredRecords.CaptureAt(0, &token, &record, &state));
        EXPECT_EQ(record.targetPackLease.leaseToken, 0u);
        EXPECT_EQ(record.rootPlayable != 0, scenario >= 2);
        if (scenario == 1) ASSERT_EQ(VN_OK, postAction(21, &playFiring));
        if (scenario == 3) ASSERT_EQ(VN_OK, Vn_StopActionFiring(stopFiring, 0));
        VnActionStepResult playStep{};
        playStep.structSize = sizeof(playStep);
        if (playFiring != 0) ASSERT_EQ(VN_OK, Vn_QueryActionStep(playFiring, 0, &playStep));
        harken::PackResourceRevisionToken oldRevision = 0;
        if (scenario == 4) {
            harken::PackRevisionKey oldKey{};
            const auto provider = handles.at(QStringLiteral("Play"));
            ASSERT_TRUE(session.Engine()->packs.ActiveRevisionIdentity(provider, &oldKey, &oldRevision));
            ASSERT_EQ(VN_OK, Vn_UnloadPack(provider));
            EXPECT_EQ(session.Engine()->packs.RetiringCount(), 1u);
            EXPECT_TRUE(session.Engine()->playableInstances.Classify(playStep.producedPlayableInstance).live);
        }
        if (scenario == 5) {
            const auto oldRoot = playStep.producedPlayableInstance;
            ASSERT_EQ(VN_OK, Vn_StopActionFiring(playFiring, 0));
            for (int block = 0; block != 8; ++block) {
                ASSERT_EQ(VN_OK, Vn_Update());
                session.Engine()->nullBackend.Pump(1);
            }
            ASSERT_EQ(VN_OK, Vn_Update());
            ASSERT_FALSE(session.Engine()->playableInstances.Classify(oldRoot).live);
            ASSERT_EQ(VN_OK, Vn_ReleaseActionFiring(playFiring));
            ASSERT_EQ(VN_OK, postAction(21, &playFiring));
            ASSERT_EQ(VN_OK, Vn_QueryActionStep(playFiring, 0, &playStep));
            EXPECT_EQ(static_cast<uint32_t>(oldRoot), static_cast<uint32_t>(playStep.producedPlayableInstance));
            EXPECT_NE(oldRoot, playStep.producedPlayableInstance);
        }
        for (int block = 0; block != 150; ++block) {
            ASSERT_EQ(VN_OK, Vn_Update());
            session.Engine()->nullBackend.Pump(1);
            if (scenario == 4 && block == 30) {
                const auto* pcm = session.Engine()->nullBackend.LastBlock();
                const auto samples = session.Engine()->nullBackend.BlockFrames()
                    * session.Engine()->nullBackend.Channels();
                ASSERT_TRUE(std::any_of(pcm, pcm + samples, [](float sample) {
                    return std::abs(sample) > 1e-4f;
                }));
                EXPECT_TRUE(session.Engine()->playableInstances.Classify(playStep.producedPlayableInstance).live);
            }
        }
        ASSERT_EQ(VN_OK, Vn_Update());
        EXPECT_EQ(session.Engine()->actionDeferredRecords.LiveCount(), 0u);
        EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(stopFiring));
        if (playFiring != 0)
            EXPECT_EQ(session.Engine()->playableInstances.Classify(playStep.producedPlayableInstance).live,
                      scenario != 2 && scenario != 4);
        if (scenario == 4) {
            ASSERT_EQ(VN_OK, Vn_ReleaseActionFiring(playFiring));
            for (int block = 0; block != 4; ++block) {
                session.Engine()->nullBackend.Pump(1);
                ASSERT_EQ(VN_OK, Vn_Update());
            }
            EXPECT_EQ(session.Engine()->packs.RetiringCount(), 0u);
            harken::PCMView exact{};
            EXPECT_EQ(VN_CONTENT_NOT_LOADED, session.Engine()->packs.LookupExact(oldRevision,
                harkentest::ToRuntime(harkentest::StudioObjectId(1 + kStableIdBias)),
                session.Engine()->registry, &exact));
        }
    }
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, CompanionsCoverEachExportedAction)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

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

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");

    auto result = exporter.ExportAllPacks(project, dir.path(), opts);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QJsonArray manifest = readPublication(dir.path());
    EXPECT_FALSE(QFileInfo::exists(QDir(dir.path()).filePath(
        QStringLiteral("HarkenPackManifest.json"))));

    const QJsonArray packs = manifest;

    const QJsonObject groupedPack = findByName(packs, QStringLiteral("GroupedOne"));
    ASSERT_FALSE(groupedPack.isEmpty());
    const QJsonObject groupedAction = findByName(
        actionCompanions(manifest), QStringLiteral("GroupedOne"));
    EXPECT_EQ(QUuid(groupedAction.value(QStringLiteral("guid")).toString()),
              project.actions.front().rootItem->children.front()->guid);

    for (const auto& [actionName, stableId] :
         std::vector<std::pair<QString, harkenstudio::ObjectId>>{
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
    EXPECT_EQ(initPack.value(QStringLiteral("companion")).toObject().keys(),
              (QStringList{QStringLiteral("schemaVersion"), QStringLiteral("variables")}));
    EXPECT_EQ(initPack.value(QStringLiteral("companion")).toObject()
                  .value(QStringLiteral("schemaVersion")).toInt(), HARKEN_PACK_SCHEMA_VERSION);

    for (const QString& fileName :
         {variantFileName(project, harkentest::StudioObjectId(21 + kStableIdBias)),
          variantFileName(project, harkentest::StudioObjectId(22 + kStableIdBias)),
          variantFileName(project, harkentest::StudioObjectId(23 + kStableIdBias))})
    {
        EXPECT_TRUE(QFileInfo::exists(QDir(dir.path()).filePath(fileName)))
            << fileName.toStdString();
        const auto companion = QJsonDocument::fromJson(readFileBytes(QDir(dir.path()).filePath(
            fileName.chopped(6) + QStringLiteral(".json")))).object();
        EXPECT_EQ(companion.keys(), (QStringList{QStringLiteral("guid"), QStringLiteral("name"),
            QStringLiteral("schemaVersion")}));
        EXPECT_EQ(companion.value(QStringLiteral("schemaVersion")).toInt(), HARKEN_PACK_SCHEMA_VERSION);
    }

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, OptionalCompanionDataFollowsPublicationLifecycle)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    const auto sourceId = registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 0.75f);
    const auto otherId = registerToneAsset(dir.path(), QStringLiteral("other.wav"), 0.25f);
    VNX::ProjectData project;
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), sourceId, otherId,
        {{QStringLiteral("Parent"), 21, kSharedSoundObjectId},
         {QStringLiteral("Child"), 22, kSoloSoundObjectId}}));
    const auto& trimmedSound = project.layers.front().rootItem->children.at(1);
    auto& source = trimmedSound->comp<VNX::VSound>()->sources.plain.entries.front();
    source.sourceId = sourceId;
    source.trimStartFrame = 8;
    source.trimEndFrame = 48;
    VNX::VActionStep post;
    post.type = VNX::VActionStep::Type::PostAction;
    post.targetObjectID = 22;
    project.actions.front().rootItem->children.front()->comp<VNX::VAction>()->steps.push_back(post);
    auto variable = makeItem(QStringLiteral("Speed"), VNX::ProjectItem::Type::Variable, 50);
    project.variables.push_back({variable, QStringLiteral("Variables")});
    VNX::VActionStep setVariable;
    setVariable.type = VNX::VActionStep::Type::SetVariable;
    setVariable.targetObjectID = variable->objectID;
    project.actions.front().rootItem->children.back()->comp<VNX::VAction>()->steps.push_back(setVariable);
    auto bindingVariable = makeItem(QStringLiteral("Volume"), VNX::ProjectItem::Type::Variable, 51);
    project.variables.push_back({bindingVariable, QStringLiteral("Variables")});
    VNX::ParameterBindingDef binding;
    binding.guid = VNX::authoringUuid(harkentest::StudioObjectId(500));
    binding.variable = bindingVariable->guid;
    binding.address = VNX::BindingSemantics::volumeAddress();
    binding.curve = {{0.0f, -6.0f}, {100.0f, 0.0f}};
    ASSERT_TRUE(project.layers.front().rootItem->comp<VNX::VMixNode>()->bindings.upsert(binding));
    auto unusedVariable = makeItem(QStringLiteral("Unused"), VNX::ProjectItem::Type::Variable, 52);
    project.variables.push_back({unusedVariable, QStringLiteral("Variables")});
    auto attenuation = makeItem(QStringLiteral("Distance"), VNX::ProjectItem::Type::Attenuation, 301);
    project.attenuationItems.push_back({attenuation, QStringLiteral("Attenuations")});
    VNX::SpatialRatingFieldGroup rating;
    rating.spatialAttenuationRef = VNX::toFieldGroupGuid(attenuation->guid);
    project.layers.front().rootItem->fieldGroups.spatialRating.shared = {VNX::FieldGroupMode::Local, rating};
    project.layers.front().rootItem->name = QStringLiteral("RenamedLayer");
    const QString root = QDir(dir.path()).filePath(QStringLiteral("BuildPacks"));
    const QString output = VNX::ProjectPackExporter::platformOutputDir(root, project.platforms.front());
    const QString waveformRoot = VNX::PathUtils::waveformBuildOutputRoot(project.projectPath, {});
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    options.selfCleaning = true;
    const auto original = exporter.ExportAllPacks(project, output, options);
    ASSERT_TRUE(original.success) << original.error.toStdString();
    QMap<QString, QByteArray> binaries;
    for (const QString& name : {QStringLiteral("Init"), QStringLiteral("Parent"), QStringLiteral("Child")})
        binaries.insert(name, readFileBytes(QDir(output).filePath(name + QStringLiteral(".bytes"))));
    EXPECT_FALSE(QDir(waveformRoot).exists());
    for (const auto& value : readPublication(output))
        EXPECT_FALSE(value.toObject().value(QStringLiteral("companion")).toObject()
            .contains(QStringLiteral("metadata")));
    project.buildExportPackMetadata = true;
    const auto metadataOnly = exporter.ExportAllPacks(project, output, options);
    ASSERT_TRUE(metadataOnly.success) << metadataOnly.error.toStdString();
    for (const auto& value : readPublication(output)) {
        const auto row = value.toObject();
        const auto companion = row.value(QStringLiteral("companion")).toObject();
        EXPECT_FALSE(companion.contains(QStringLiteral("waveforms")));
        EXPECT_FALSE(QDir(waveformRoot).exists());
        const auto metadata = companion.value(QStringLiteral("metadata")).toObject();
        EXPECT_EQ(companion.value(QStringLiteral("schemaVersion")).toInt(), HARKEN_PACK_SCHEMA_VERSION);
        EXPECT_FALSE(metadata.contains(QStringLiteral("schemaVersion")));
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
        EXPECT_TRUE(guids.contains(VNX::objectIdString(variable->runtimeId())));
        EXPECT_TRUE(guids.contains(VNX::objectIdString(bindingVariable->runtimeId())));
        if (row.value(QStringLiteral("name")) == QStringLiteral("Init")) {
            EXPECT_TRUE(guids.contains(VNX::objectIdString(project.busses.front().rootItem->runtimeId())));
            EXPECT_TRUE(guids.contains(VNX::objectIdString(unusedVariable->runtimeId())));
        } else {
            EXPECT_FALSE(guids.contains(VNX::objectIdString(unusedVariable->runtimeId())));
            EXPECT_TRUE(guids.contains(VNX::objectIdString(attenuation->runtimeId())));
            EXPECT_TRUE(guids.contains(VNX::objectIdString(trimmedSound->runtimeId())));
        }
    }
    project.buildExportWaveforms = true;
    const auto enabled = exporter.ExportAllPacks(project, output, options);
    ASSERT_TRUE(enabled.success) << enabled.error.toStdString();
    for (auto binary = binaries.cbegin(); binary != binaries.cend(); ++binary)
        EXPECT_EQ(readFileBytes(QDir(output).filePath(binary.key() + QStringLiteral(".bytes"))), binary.value());
    const auto publication = readPublication(output);
    const auto parent = findByName(publication, QStringLiteral("Parent")).value(QStringLiteral("companion")).toObject();
    const auto child = findByName(publication, QStringLiteral("Child")).value(QStringLiteral("companion")).toObject();
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
    EXPECT_EQ(readFileBytes(waveformPath), readFileBytes(VNX::WaveformCache::cachePath(
        VNX::AssetRegistry::instance().getAsset(sourceId)->absolutePath)));
    auto absoluteChild = child;
    auto absoluteWaveform = childWaves.first().toObject();
    absoluteWaveform.insert(QStringLiteral("file"), waveformPath);
    absoluteChild.insert(QStringLiteral("waveforms"), QJsonArray{absoluteWaveform});
    writeFileBytes(QDir(output).filePath(QStringLiteral("Child.json")),
        QJsonDocument(absoluteChild).toJson());
    const QString orphan = QDir(waveformRoot).filePath(QStringLiteral("unreferenced.vkd"));
    writeFileBytes(orphan, "unused");
    const auto finalized = VNX::ProjectPackExporter::FinalizePackExport(
        root, project, {options.platform}, true, enabled.artifacts);
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
    EXPECT_TRUE(warned.success) << warned.error.toStdString();
    EXPECT_FALSE(warned.warnings.isEmpty());
    for (auto binary = binaries.cbegin(); binary != binaries.cend(); ++binary)
        EXPECT_EQ(readFileBytes(QDir(output).filePath(binary.key() + QStringLiteral(".bytes"))), binary.value());
    const auto parentWithoutWaveforms = QJsonDocument::fromJson(readFileBytes(
        QDir(output).filePath(QStringLiteral("Parent.json")))).object();
    EXPECT_FALSE(parentWithoutWaveforms.contains(QStringLiteral("waveforms")));
    EXPECT_FALSE(parentWithoutWaveforms.contains(QStringLiteral("metadata")));
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
        const auto companion = value.toObject().value(QStringLiteral("companion")).toObject();
        EXPECT_EQ(companion.value(QStringLiteral("schemaVersion")).toInt(), HARKEN_PACK_SCHEMA_VERSION);
        EXPECT_FALSE(companion.contains(QStringLiteral("waveforms")));
        EXPECT_FALSE(companion.contains(QStringLiteral("metadata")));
    }
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest,
     LoopMediaRevisionMatchesCanonicalRuntimeRegistration)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("loop.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("unused.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("Loop"), 21, kSharedSoundObjectId}}));
    auto* sound = project.layers.front().rootItem->children.front()->comp<VNX::VSound>();
    ASSERT_NE(sound, nullptr);
    auto& source = sound->sources.plain.entries.front();
    source.loopProvenance = VNX::SoundLoopProvenance::Authored;
    source.loopRegion = VNX::SoundLoopRegion{12, 73};

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const auto result = exporter.ExportAllPacks(project, dir.path(), opts);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QString contentPath = QDir(dir.path()).filePath(
        variantFileName(project, harkentest::StudioObjectId(21 + kStableIdBias)));
    QByteArray bytes;
    harken::PackCanonicalImage image;
    harken::CurrentWorldBodyImage body;
    ASSERT_NO_FATAL_FAILURE(readCanonicalWorld(
        contentPath, bytes, image, body));
    ASSERT_EQ(image.media.size(), 1u);
    ASSERT_EQ(body.ProductionView()->playablePropertyCount, 1u);
    EXPECT_EQ(image.media[0].flags & harken::kPackFlagLoopable,
              harken::kPackFlagLoopable);
    EXPECT_EQ(image.media[0].loopStartFrame, 12u);
    EXPECT_EQ(image.media[0].loopEndFrame, 73u);
    ASSERT_NO_FATAL_FAILURE(expectMediaMatchesDefinition(
        bytes, image.media[0], body.ProductionView()->playableProperties[0]));
    ASSERT_NO_FATAL_FAILURE(expectPackRendersInIndependentSession(
        dir.path(), QDir(dir.path()).filePath(QStringLiteral("Init.bytes")),
        contentPath, image.packStableId,
        harkentest::StudioObjectId(21 + kStableIdBias)));

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, InitHasNoRoutingCatalogAndEveryPackHasKind21)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

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

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    auto result = exporter.ExportAllPacks(project, dir.path(), opts);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QString initPath = QDir(dir.path()).filePath(QStringLiteral("Init.bytes"));
    const QByteArray initBytes = readFileBytes(initPath);
    harken::PackCanonicalImage initImage;
    ASSERT_EQ(VN_OK,
              harken::ParsePackCanonicalImage(
                  reinterpret_cast<const std::uint8_t*>(initBytes.constData()),
                  static_cast<std::uint64_t>(initBytes.size()), &initImage));
    EXPECT_TRUE(initImage.hasPartition);
    EXPECT_FALSE(initImage.hasCatalog);
    EXPECT_FALSE(initImage.hasGameplayCatalog);
    EXPECT_TRUE(initImage.catalogRows.empty());
    bool sawCatalogEntry = false;
    harken::PackV2Header initHeader;
    ASSERT_TRUE(harken::ParsePackV2Header(
        reinterpret_cast<const std::uint8_t*>(initBytes.constData()), &initHeader));
    for (std::uint32_t i = 0; i < initHeader.entryCount; ++i) {
        const harken::PackV2DirectoryEntry record = harken::ParsePackV2DirectoryEntry(
            reinterpret_cast<const std::uint8_t*>(initBytes.constData()) +
            initHeader.directoryOffset + i * harken::kPackV2DirectoryEntryBytes);
        if (record.kind == harken::kPackEntryKindInitRoutingCatalog)
            sawCatalogEntry = true;
    }
    EXPECT_FALSE(sawCatalogEntry);

    const QJsonArray manifest = readPublication(dir.path());
    EXPECT_EQ(actionCompanions(manifest).size(), 3);
    const QJsonArray packs = manifest;
    EXPECT_FALSE(findByName(packs, QStringLiteral("GroupedOne")).isEmpty());
    EXPECT_FALSE(findByName(packs, QStringLiteral("LooseOne")).isEmpty());
    EXPECT_FALSE(findByName(packs, QStringLiteral("LooseTwo")).isEmpty());

    expectCanonicalPackParsesWithPartition(
        QDir(dir.path()).filePath(variantFileName(project, harkentest::StudioObjectId(21 + kStableIdBias))));
    expectCanonicalPackParsesWithPartition(
        QDir(dir.path()).filePath(variantFileName(project, harkentest::StudioObjectId(22 + kStableIdBias))));
    expectCanonicalPackParsesWithPartition(
        QDir(dir.path()).filePath(variantFileName(project, harkentest::StudioObjectId(23 + kStableIdBias))));

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, RenameKeepsCompanionIdentitySelfConsistent)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t firstSourceId =
        registerToneAsset(dir.path(), QStringLiteral("first.wav"), 1.0f);
    const uint32_t secondSourceId =
        registerToneAsset(dir.path(), QStringLiteral("second.wav"), 0.5f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), firstSourceId, secondSourceId,
        {{QStringLiteral("BeforeName"), 21, kSharedSoundObjectId}}));

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    opts.selfCleaning = true;

    const QDir output(dir.filePath("exports"));
    auto firstResult = exporter.ExportAllPacks(project, output.path(), opts);
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
        const QJsonArray manifest = readPublication(output.path());
        const QJsonObject packEntry = findByName(
            manifest,
            QStringLiteral("BeforeName"));
        EXPECT_TRUE(packEntry.value(QStringLiteral("path")).toString().endsWith(
            beforeFileName));
        EXPECT_TRUE(QFileInfo::exists(output.filePath(beforeFileName)));
    }

    project.actions.front().rootItem->children.front()->name =
        QStringLiteral("AfterName");

    auto secondResult = exporter.ExportAllPacks(project, output.path(), opts);
    ASSERT_TRUE(secondResult.success) << secondResult.error.toStdString();

    const QString afterFileName = variantFileName(project, harkentest::StudioObjectId(21 + kStableIdBias));
    EXPECT_EQ(afterFileName, QStringLiteral("AfterName.bytes"));
    EXPECT_FALSE(QFileInfo::exists(output.filePath(beforeFileName)));
    EXPECT_FALSE(QFileInfo::exists(output.filePath(QStringLiteral("BeforeName.json"))));
    EXPECT_FALSE(QFileInfo::exists(output.filePath(legacyName)));

    const QJsonArray manifest = readPublication(output.path());
    const QJsonArray packs = manifest;
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
    EXPECT_EQ(renamedEntry.value(QStringLiteral("companion")).toObject().value(QStringLiteral("guid")),
              beforeEntry.value(QStringLiteral("companion")).toObject().value(QStringLiteral("guid")));

    project.actions.front().rootItem->children.front()->name = QStringLiteral("afterName");
    const auto caseOnly = exporter.ExportAllPacks(project, output.path(), opts);
    ASSERT_TRUE(caseOnly.success) << caseOnly.error.toStdString();
    const auto caseEntry = findByName(readPublication(output.path()),
                                      QStringLiteral("afterName"));
    EXPECT_EQ(caseEntry.value(QStringLiteral("packStableId")), beforeEntry.value(QStringLiteral("packStableId")));
    EXPECT_EQ(caseEntry.value(QStringLiteral("path")).toString(), QStringLiteral("afterName.bytes"));
    EXPECT_TRUE(QFileInfo::exists(output.filePath(QStringLiteral("afterName.bytes"))));

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, InvalidActionPackNamesPreservePublishedFiles)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& registry = VNX::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(dir.path());
    const auto sourceId = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), sourceId, sourceId,
        {{QStringLiteral("Shoot"), 21, kSharedSoundObjectId},
         {QStringLiteral("Tail"), 22, kSharedSoundObjectId}}));
    const QString outputDir = QDir(dir.path()).filePath(QStringLiteral("published"));
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions options;
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
        std::vector<VNX::ProjectPackExporter::PackBuildRecord> records;
        const auto result = exporter.ExportAllPacks(project, outputDir, options, &records);
        EXPECT_FALSE(result.success);
        EXPECT_FALSE(result.error.isEmpty());
        EXPECT_EQ(publication(), original);
        EXPECT_TRUE(records.empty());
        if (name != QStringLiteral("shoot")) {
            const auto single = exporter.ExportActionPack(project, name,
                QDir(outputDir).filePath(QStringLiteral("Init.bytes")), options);
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
    action->name = QString(VNX::NameValidator::MAX_NAME_LENGTH, QLatin1Char('A'));
    const auto longest = exporter.ExportAllPacks(project, outputDir, options);
    ASSERT_TRUE(longest.success) << longest.error.toStdString();
    expectCanonicalPackParsesWithPartition(QDir(outputDir).filePath(action->name + QStringLiteral(".bytes")));
    registry.clear();
}

TEST(EditorProjectPackExporterManifest, UntargetedUnitsStayOutOfPublication)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

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

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.selection.actionTargeting.push_back(
        {harkentest::StudioObjectId(23 + kStableIdBias), false, {HarkenTest::platformIdString("Android")}});
    opts.platform = HarkenTest::platformIdString("Windows");

    auto result = exporter.ExportAllPacks(project, dir.path(), opts);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QJsonArray manifest = readPublication(dir.path());
    const QJsonArray packs = manifest;

    EXPECT_TRUE(findByName(packs, QStringLiteral("LooseDropped")).isEmpty());
    const QJsonObject keptPack = findByName(packs, QStringLiteral("LooseKept"));
    EXPECT_EQ(keptPack.value(QStringLiteral("packKind")).toInt(), 3);
    EXPECT_TRUE(findByName(packs, QStringLiteral("Init")).value(
        QStringLiteral("packKind")).toInt() == 2);

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, ActionNameCollisionFailsBeforePublishing)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

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

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");

    auto result = exporter.ExportAllPacks(project, dir.path(), opts);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("collision"), Qt::CaseInsensitive))
        << result.error.toStdString();
    EXPECT_TRUE(QDir(dir.path()).entryList({QStringLiteral("*.bytes")}, QDir::Files).isEmpty());
    EXPECT_FALSE(QFileInfo::exists(QDir(dir.path()).filePath(
        QStringLiteral("HarkenPackManifest.json"))));

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, CompanionCommitFailureFailsWholeExport)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

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
    const QString manifestPath = QDir(outputDir).filePath(
        QStringLiteral("Init.json"));
    ASSERT_TRUE(QDir().mkpath(manifestPath));

    VNX::ProjectPackExporter exporter;
    const auto result = exporter.ExportAllPacks(project, outputDir, {.platform = HarkenTest::platformIdString("Windows")});

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("Init.json")))
        << result.error.toStdString();
    EXPECT_TRUE(QFileInfo(manifestPath).isDir());

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, UnsupportedBusPropertiesKeepPublishedPackSetUnchanged)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir source;
    QTemporaryDir output;
    ASSERT_TRUE(source.isValid());
    ASSERT_TRUE(output.isValid());
    auto& registry = VNX::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(source.path());
    const auto sourceId = registerToneAsset(source.path(), QStringLiteral("tone.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, source.path(), sourceId, sourceId,
        {{QStringLiteral("PlayTone"), 21, kSharedSoundObjectId}}));
    VNX::ProjectPackExporter exporter;
    project.packBuildOutputDir = output.path();
    const VNX::ProjectPackExporter::ExportOptions options{
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
    master->comp<VNX::VBus>()->pan = 0.25f;
    rejected(QStringLiteral("pan"));
    master->comp<VNX::VBus>()->pan = 0.0f;
    master->comp<VNX::VMixNode>()->pitch = 300.0f;
    master->playablePropertyPresence.Set(harkenplayableproperties::PlayablePropertyIdV1::Pitch);
    rejected(QStringLiteral("pitch"));
    master->comp<VNX::VMixNode>()->pitch = 0.0f;
    master->comp<VNX::VBus>()->listenerRelativeRouting = true;
    rejected(QStringLiteral("listenerRelativeRouting"));
    master->comp<VNX::VBus>()->listenerRelativeRouting = false;
    const auto retry = exporter.ExportAllPacks(project, {}, options);
    EXPECT_TRUE(retry.success) << retry.error.toStdString();
    master->comp<VNX::VMixNode>()->pitch = 300.0f;
    const auto locator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*master);
    ASSERT_TRUE(locator.has_value());
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(*master, *locator,
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

TEST(EditorProjectPackExporterManifest, TwoExportsAreByteIdentical)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));

    QTemporaryDir firstDir;
    QTemporaryDir secondDir;
    ASSERT_TRUE(firstDir.isValid());
    ASSERT_TRUE(secondDir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(firstDir.path());
    const uint32_t sharedSourceId =
        registerToneAsset(firstDir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(firstDir.path(), QStringLiteral("solo.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, firstDir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("One"), 21, kSharedSoundObjectId},
         {QStringLiteral("Two"), 22, kSoloSoundObjectId}}));
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
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
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, ProductionPackPairGoldenIsRepeatable)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir source;
    ASSERT_TRUE(source.isValid());
    auto& registry = VNX::AssetRegistry::instance();
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
        VNX::authoringUuid(harkentest::StudioObjectId(4));

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
        VNX::ProjectPackExporter exporter;
        VNX::ProjectPackExporter::ExportOptions opts;
        opts.platform = HarkenTest::platformIdString("Windows");
        const auto result = exporter.ExportAllPacks(project, directory, opts);
        ASSERT_TRUE(result.success) << result.error.toStdString();
        const QDir output(directory);
        EXPECT_EQ(output.entryList({"*.bytes", "*.json"}, QDir::Files, QDir::Name), expectedNames);
        ASSERT_NO_FATAL_FAILURE(expectInitHasVolumeOnlyKind24(output.filePath(QStringLiteral("Init.bytes"))));
        const auto manifest = readPublication(directory);
        const auto packs = manifest;
        ASSERT_EQ(packs.size(), 2);
        for (const auto& value : packs) {
            const auto row = value.toObject();
            const QString path = output.filePath(row.value(QStringLiteral("path")).toString());
            const auto bytes = readFileBytes(path);
            EXPECT_EQ(row.value(QStringLiteral("contentHash")).toString().toULongLong(),
                harken::Fnv1a64(bytes.constData(), bytes.size()));
            harken::PackV2Header header;
            ASSERT_GE(bytes.size(), harken::kPackV2HeaderBytes);
            ASSERT_TRUE(harken::ParsePackV2Header(reinterpret_cast<const uint8_t*>(bytes.constData()), &header));
            EXPECT_EQ(header.formatVersion, HARKEN_PACK_SCHEMA_VERSION);
            ASSERT_NO_FATAL_FAILURE(expectCanonicalPackParsesWithPartition(path));
        }
        const auto content = findByName(packs, QStringLiteral("Play_Golden"));
        ASSERT_FALSE(content.isEmpty());
        ASSERT_NO_FATAL_FAILURE(expectPackRendersInIndependentSession(directory,
            output.filePath(QStringLiteral("Init.bytes")), output.filePath(contentName),
            content.value(QStringLiteral("packStableId")).toString().toULongLong(), harkentest::StudioObjectId(4)));
        EXPECT_EQ(QUuid(content.value(QStringLiteral("companion")).toObject()
            .value(QStringLiteral("guid")).toString()), VNX::authoringUuid(harkentest::StudioObjectId(4)));
    }
    for (const QString& name : expectedNames) {
        const auto a = readFileBytes(QDir(first).filePath(name));
        const auto b = readFileBytes(QDir(second).filePath(name));
        EXPECT_EQ(a, b) << name.toStdString();
        EXPECT_EQ(harken::Fnv1a64(a.constData(), a.size()), harken::Fnv1a64(b.constData(), b.size()));
    }
    registry.clear();
}

TEST(EditorProjectPackExporterManifest, LegacySnapshotRequestFieldsDoNotOverrideBusProjection)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir source;
    ASSERT_TRUE(source.isValid());
    auto& registry = VNX::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(source.path());
    const uint32_t sourceId = registerToneAsset(source.path(), QStringLiteral("snapshot.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, source.path(), sourceId, sourceId,
        {{QStringLiteral("Play_Snapshot"), 4, kSoloSoundObjectId}}));
    project.audioSources.resize(1);
    project.layers.front().rootItem->children.erase(project.layers.front().rootItem->children.begin());
    project.actions.front().rootItem->children.front()->guid =
        VNX::authoringUuid(harkentest::StudioObjectId(4));
    VNX::CanonicalPackSetExportRequest request;
    request.project = &project;
    request.options.platform = HarkenTest::platformIdString("Windows");
    const auto plan = VNX::PackBuildPlanning::build(project, {request.options.platform});
    ASSERT_TRUE(plan.ready());
    request.plan = &plan;
    request.includedActionIds = {harkentest::StudioObjectId(4)};
    request.outputDir = QDir(source.path()).filePath(QStringLiteral("default"));
    ASSERT_FALSE(QFileInfo::exists(request.outputDir));
    VNX::ProjectPackExporter exporter;
    const auto baseline = VNX::ExportCanonicalPackSet(exporter, request, nullptr);
    ASSERT_TRUE(baseline.success) << baseline.error.toStdString();
    ASSERT_NO_FATAL_FAILURE(expectInitHasVolumeOnlyKind24(
        QDir(request.outputDir).filePath(QStringLiteral("Init.bytes"))));
    ASSERT_TRUE(QFileInfo::exists(QDir(request.outputDir).filePath(
        variantFileName(project, harkentest::StudioObjectId(4)))));
    const QByteArray baselineInit = readFileBytes(QDir(request.outputDir).filePath(
        QStringLiteral("Init.bytes")));
    const QByteArray baselineContent = readFileBytes(QDir(request.outputDir).filePath(
        variantFileName(project, harkentest::StudioObjectId(4))));

    const std::array<harkenstudio::ObjectId VNX::CanonicalPackSetExportRequest::*, 6> fields = {
        &VNX::CanonicalPackSetExportRequest::snapshotBus,
        &VNX::CanonicalPackSetExportRequest::snapshotParent,
        &VNX::CanonicalPackSetExportRequest::snapshotSendSource,
        &VNX::CanonicalPackSetExportRequest::snapshotSendTarget,
        &VNX::CanonicalPackSetExportRequest::snapshotDownmix,
        &VNX::CanonicalPackSetExportRequest::snapshotMaster};
    for (std::size_t i = 0; i < fields.size(); ++i) {
        SCOPED_TRACE(i);
        auto changed = request;
        (changed.*fields[i]).high = 1;
        changed.outputDir = QDir(source.path()).filePath(
            QStringLiteral("legacy-field-%1").arg(static_cast<int>(i)));
        ASSERT_FALSE(QFileInfo::exists(changed.outputDir));
        const auto result = VNX::ExportCanonicalPackSet(exporter, changed, nullptr);
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

TEST(EditorProjectPackExporterManifest, CanonicalContentOmitsKindNineAndClaimsContainer)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("PlayContainer"), 24, 3}}));

    auto container = makeItem(QStringLiteral("PlayableContainer"),
                              VNX::ProjectItem::Type::Container, 3);
    auto childSound = makeItem(QStringLiteral("ContainedTone"),
                               VNX::ProjectItem::Type::Sound, 4);
    HarkenTest::setSharedSoundSource(*childSound, soloSourceId);
    addChild(container, childSound);
    container->comp<VNX::VSelection>()->selections.shared.occurrenceOrdinalHighWater = 1;
    EXPECT_TRUE(container->comp<VNX::VSelection>()->selections.shared.setOccurrenceRows({{childSound->guid, 0}}));
    addChild(project.layers.front().rootItem, container);

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const auto result = exporter.ExportAllPacks(project, dir.path(), opts);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QDir output(dir.path());
    const QStringList packFiles = output.entryList({"*.bytes"}, QDir::Files);
    ASSERT_FALSE(packFiles.isEmpty());
    for (const QString& name : packFiles) {
        const QString path = output.filePath(name);
        EXPECT_FALSE(packHasKind(path, harken::kPackEntryKindContainer))
            << name.toStdString();
        EXPECT_FALSE(packHasKind(path, harken::kPackEntryKindVoiceRecipe))
            << name.toStdString();
        if (name == QStringLiteral("Init.bytes"))
            continue;
        expectCanonicalPackParsesWithPartition(path);
    }

    const QByteArray initBytes = readFileBytes(output.filePath(QStringLiteral("Init.bytes")));
    harken::PackCanonicalImage initImage;
    ASSERT_EQ(VN_OK,
              harken::ParsePackCanonicalImage(
                  reinterpret_cast<const std::uint8_t*>(initBytes.constData()),
                  static_cast<std::uint64_t>(initBytes.size()), &initImage));
    EXPECT_FALSE(initImage.hasCatalog);
    EXPECT_TRUE(initImage.catalogRows.empty());

    const QJsonObject ownerPack = findByName(
        readPublication(dir.path()),
        QStringLiteral("PlayContainer"));
    EXPECT_EQ(ownerPack.value(QStringLiteral("packStableId")).toString(),
              QString::number(fnvUtf8(HarkenTest::platformIdString("Windows") + QStringLiteral("/action_3da91a64-b947-4560-0000-0000000186b8"))));

    const QString ownerPath = output.filePath(variantFileName(project, harkentest::StudioObjectId(24 + kStableIdBias)));
    const QByteArray ownerBytes = readFileBytes(ownerPath);
    harken::PackCanonicalImage ownerImage;
    ASSERT_EQ(VN_OK,
              harken::ParsePackCanonicalImage(
                  reinterpret_cast<const std::uint8_t*>(ownerBytes.constData()),
                  static_cast<std::uint64_t>(ownerBytes.size()), &ownerImage));
    EXPECT_TRUE(ownerImage.hasPartition);
    harken::CurrentWorldBodyImage ownerBody;
    ASSERT_EQ(harken::CurrentWorldBodyDecodeResult::Ok,
              ownerBody.DecodeWithResult(
                  ownerImage.partitionBody.data(),
                  ownerImage.partitionBody.size(),
                  ownerImage.partitionHeader.bodyVintage,
                  harken::CurrentWorldDecodeMode::StrictArtifact));
    std::vector<std::uint8_t> shippingBody;
    ASSERT_TRUE(harken::EncodeCurrentWorldShippingBody(*ownerBody.ProductionView(),
                                                      shippingBody));
    EXPECT_EQ(ownerImage.partitionBody, shippingBody);

    preserveUnityFixture(dir.path());

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest,
     DefaultBlendExportsTargetsWithoutVariableAxesOrCurves)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    const auto source = registerToneAsset(dir.path(), QStringLiteral("default-blend.wav"), 0.5f);
    const auto sharedSource = registerToneAsset(dir.path(), QStringLiteral("shared-blend.wav"), 1.0f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), sharedSource, source,
        {{QStringLiteral("PlayDefaultBlend"), 24, 3}}));
    auto container = makeItem(QStringLiteral("DefaultBlend"), VNX::ProjectItem::Type::Container, 3);
    auto child = makeItem(QStringLiteral("BlendTarget"), VNX::ProjectItem::Type::Sound, 4);
    HarkenTest::setSharedSoundSource(*child, source);
    addChild(container, child);
    auto* selection = container->comp<VNX::VSelection>();
    selection->mode = VNX::SelectionMode::Blend;
    ASSERT_TRUE(selection->selections.shared.setMixing(
        VNX::MixingDef{{{{}, {{child->guid, {}, {}}}}}}));
    addChild(project.layers.front().rootItem, container);
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    const auto result = exporter.ExportAllPacks(project, dir.path(), options);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    const auto bytes = readFileBytes(QDir(dir.path()).filePath(
        variantFileName(project, harkentest::StudioObjectId(24 + kStableIdBias))));
    harken::PackCanonicalImage image;
    ASSERT_EQ(VN_OK, harken::ParsePackCanonicalImage(
        reinterpret_cast<const std::uint8_t*>(bytes.constData()), bytes.size(), &image));
    ASSERT_TRUE(image.hasPartition);
    harken::CurrentWorldBodyImage body;
    ASSERT_EQ(harken::CurrentWorldBodyDecodeResult::Ok, body.DecodeWithResult(
        image.partitionBody.data(), image.partitionBody.size(), image.partitionHeader.bodyVintage,
        harken::CurrentWorldDecodeMode::StrictArtifact));
    const auto world = *body.ProductionView();
    ASSERT_EQ(world.containerCount, 1u);
    EXPECT_EQ(world.variableCount, 0u);
    EXPECT_EQ(world.containers[0].childCount, 1u);
    EXPECT_EQ(world.containers[0].continuousAxisCount, 0u);
    EXPECT_EQ(world.containers[0].continuousRowCount, 1u);
    EXPECT_EQ(world.containers[0].curveCount, 0u);
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest,
     CanonicalPartitionsNormalizeLocalTransitionIdentity)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);

    struct OwnerSpec
    {
        QString name;
        std::uint32_t actionObjectId;
        std::uint32_t containerObjectId;
        std::uint32_t childObjectId;
        bool customTransition;
    };
    const std::array<OwnerSpec, 3> specs{{
        {QStringLiteral("TransitionOwnerA"), 24, 3, 4, true},
        {QStringLiteral("TransitionOwnerB"), 25, 5, 6, true},
        {QStringLiteral("TransitionOwnerNone"), 26, 7, 8, false},
    }};
    std::vector<ActionSpec> actions;
    for (const OwnerSpec& spec : specs) {
        actions.push_back(
            {spec.name, spec.actionObjectId, spec.containerObjectId});
    }

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId, actions));
    const auto layer = project.layers.front().rootItem;
    ASSERT_NE(layer, nullptr);

    for (const OwnerSpec& spec : specs) {
        auto container = makeItem(
            QStringLiteral("Container_%1").arg(spec.containerObjectId),
            VNX::ProjectItem::Type::Container, spec.containerObjectId);
        auto child = makeItem(
            QStringLiteral("Child_%1").arg(spec.childObjectId),
            VNX::ProjectItem::Type::Sound, spec.childObjectId);
        HarkenTest::setSharedSoundSource(*child, soloSourceId);
        addChild(container, child);
        ASSERT_TRUE(
            VNX::PlayablePropertyAuthoringV1::addContainerChildOccurrence(
                *container, child->guid));
        auto* selection = container->comp<VNX::VSelection>();
        ASSERT_NE(selection, nullptr);
        selection->mode = VNX::SelectionMode::Sequence;
        selection->playType = spec.customTransition
            ? VNX::SelectionPlayType::Queue : VNX::SelectionPlayType::Single;
        if (spec.customTransition) {
            auto rule = selection->transitions.rules[0];
            rule.exitTiming = VNX::TransitionExitTiming::Immediate;
            rule.sourceFadeOut.durationSeconds = 0.01f;
            ASSERT_TRUE(selection->transitions.updateRule(0, rule));
        }
        addChild(layer, container);
    }

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const auto result = exporter.ExportAllPacks(project, dir.path(), opts);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QDir output(dir.path());
    const QStringList packFiles = output.entryList({"*.bytes"}, QDir::Files);
    ASSERT_FALSE(packFiles.isEmpty());
    harken::WorldCapacitySummaryV1 fullWorldCapacity{};
    bool haveFullWorldCapacity = false;
    static_assert(sizeof(harken::WorldCapacitySummaryV1) ==
                  37 * sizeof(std::uint32_t));
    for (const QString& name : packFiles) {
        const QString path = output.filePath(name);
        const QByteArray bytes = readFileBytes(path);
        harken::PackCanonicalImage image;
        ASSERT_EQ(VN_OK,
                  harken::ParsePackCanonicalImage(
                      reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                      static_cast<std::uint64_t>(bytes.size()), &image))
            << name.toStdString();
        ASSERT_TRUE(image.hasPartition) << name.toStdString();
        harken::CurrentWorldBodyImage body;
        ASSERT_EQ(harken::CurrentWorldBodyDecodeResult::Ok,
                  body.DecodeWithResult(
                      image.partitionBody.data(), image.partitionBody.size(),
                      image.partitionHeader.bodyVintage,
                      harken::CurrentWorldDecodeMode::StrictArtifact))
            << name.toStdString();
        const auto world = *body.ProductionView();
        EXPECT_EQ(world.identity.capabilityMask,
                  harken::detail::ComputeCurrentWorldLocalCapabilityMaskV1(
                      world))
            << name.toStdString();
        EXPECT_EQ(world.identity.runtimeSemanticFingerprint,
                  harken::ComputeCurrentWorldFingerprintV4(world))
            << name.toStdString();
        if (!haveFullWorldCapacity) {
            fullWorldCapacity = world.capacity;
            haveFullWorldCapacity = true;
        } else {
            EXPECT_EQ(0, std::memcmp(&fullWorldCapacity, &world.capacity,
                                     sizeof(fullWorldCapacity)))
                << name.toStdString();
        }
    }

    for (const OwnerSpec& spec : specs) {
        const QString path = output.filePath(
            variantFileName(project, harkentest::StudioObjectId(spec.actionObjectId + kStableIdBias)));
        const QByteArray bytes = readFileBytes(path);
        harken::PackCanonicalImage image;
        ASSERT_EQ(VN_OK,
                  harken::ParsePackCanonicalImage(
                      reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                      static_cast<std::uint64_t>(bytes.size()), &image));
        ASSERT_TRUE(image.hasPartition);
        harken::CurrentWorldBodyImage body;
        ASSERT_EQ(harken::CurrentWorldBodyDecodeResult::Ok,
                  body.DecodeWithResult(
                      image.partitionBody.data(), image.partitionBody.size(),
                      image.partitionHeader.bodyVintage,
                      harken::CurrentWorldDecodeMode::StrictArtifact));
        const auto world = *body.ProductionView();
        ASSERT_EQ(world.actionCount, 1u);
        ASSERT_EQ(world.containerCount, 1u);
        const auto& container = world.containers[0];
        EXPECT_EQ(container.containerStableId,
                  harkentest::StudioObjectId(spec.containerObjectId + kStableIdBias));
        if (spec.customTransition) {
            ASSERT_EQ(world.transitionCount, 1u);
            EXPECT_EQ(container.transitionProgramIndex, 0u);
            EXPECT_EQ(world.transitions[0].ownerContainerStableId,
                      container.containerStableId);
        } else {
            EXPECT_EQ(world.transitionCount, 0u);
            EXPECT_EQ(container.transitionProgramIndex, UINT32_MAX);
        }
    }

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest,
     CanonicalCurrentWorldPreservesNonSoundLoopCounts)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);

    struct KindSpec
    {
        VNX::ProjectItem::Type type;
        harken::playableproperties::ContentPropertyKindV1 kind;
    };
    struct LoopSpec
    {
        KindSpec content;
        std::uint32_t loopCount;
        std::uint32_t objectId;
        std::uint32_t actionObjectId;
    };
    constexpr std::array<std::uint32_t, 5> kLoopCounts{
        0u, 1u, 255u, 256u, std::numeric_limits<std::uint32_t>::max()};
    constexpr std::array<KindSpec, 3> kKinds{{
        {VNX::ProjectItem::Type::Container,
         harken::playableproperties::ContentPropertyKindV1::Container},
        {VNX::ProjectItem::Type::Track,
         harken::playableproperties::ContentPropertyKindV1::Track},
        {VNX::ProjectItem::Type::Segment,
         harken::playableproperties::ContentPropertyKindV1::Segment},
    }};

    std::vector<LoopSpec> specs;
    std::vector<ActionSpec> actions;
    std::uint32_t nextObjectId = 1000;
    std::uint32_t nextActionObjectId = 2000;
    for (const KindSpec content : kKinds) {
        for (const std::uint32_t loopCount : kLoopCounts) {
            const LoopSpec spec{
                content, loopCount, nextObjectId++, nextActionObjectId++};
            specs.push_back(spec);
            actions.push_back({
                QStringLiteral("Loop_%1_%2")
                    .arg(static_cast<int>(content.kind))
                    .arg(QString::number(loopCount)),
                spec.actionObjectId,
                spec.objectId});
        }
    }

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId, actions));
    const auto layer = project.layers.front().rootItem;
    ASSERT_NE(layer, nullptr);
    ASSERT_GE(layer->children.size(), 2u);

    std::uint32_t nextAuxObjectId = 3000;
    uint64_t nextClipStableId = 500000;
    for (const LoopSpec& spec : specs) {
        auto target = makeItem(
            QStringLiteral("LoopTarget_%1").arg(spec.objectId),
            spec.content.type, spec.objectId);
        auto* audio = target->comp<VNX::VAudio>();
        ASSERT_NE(audio, nullptr);
        audio->loopCount = spec.loopCount;

        if (spec.content.type == VNX::ProjectItem::Type::Container) {
            auto childSound = makeItem(
                QStringLiteral("ContainerSound_%1").arg(spec.objectId),
                VNX::ProjectItem::Type::Sound, nextAuxObjectId++);
            HarkenTest::setSharedSoundSource(*childSound, soloSourceId);
            addChild(target, childSound);
            ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::addContainerChildOccurrence(
                *target, childSound->guid));
        } else {
            std::shared_ptr<VNX::ProjectItem> track = target;
            if (spec.content.type == VNX::ProjectItem::Type::Segment) {
                track = makeItem(
                    QStringLiteral("SegmentTrack_%1").arg(spec.objectId),
                    VNX::ProjectItem::Type::Track, nextAuxObjectId++);
                addChild(target, track);
            }
            auto* timeline = track->comp<VNX::VTrack>();
            ASSERT_NE(timeline, nullptr);
            auto clipSound = makeItem(
                QStringLiteral("ClipSound_%1").arg(spec.objectId),
                VNX::ProjectItem::Type::Sound, nextAuxObjectId++);
            HarkenTest::setSharedSoundSource(*clipSound, soloSourceId);
            addChild(track, clipSound);
            VNX::Clip clip;
            clip.clipGuid = QUuid::createUuid();
            clip.targetGuid = clipSound->guid;
            clip.duration = 1000000;
            clip.clipGuid = VNX::authoringUuid(harkentest::StudioObjectId(nextClipStableId++));
            ASSERT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
            timeline->clips.items.push_back(clip);
        }
        addChild(layer, target);
    }
    project.nextObjectID = nextAuxObjectId;

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const auto result = exporter.ExportAllPacks(project, dir.path(), opts);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    constexpr std::size_t kLoopSlot =
        static_cast<std::size_t>(
            harken::playableproperties::PlayablePropertyIdV1::LoopCount) - 1u;
    constexpr std::uint64_t kLoopMask = UINT64_C(1) << kLoopSlot;
    const QDir output(dir.path());
    for (const LoopSpec& spec : specs) {
        const QString path = output.filePath(
            variantFileName(project, harkentest::StudioObjectId(spec.actionObjectId + kStableIdBias)));
        const QByteArray bytes = readFileBytes(path);
        harken::PackV2Header header;
        ASSERT_TRUE(harken::ParsePackV2Header(
            reinterpret_cast<const std::uint8_t*>(bytes.constData()), &header))
            << path.toStdString();
        EXPECT_EQ(header.formatVersion, harken::kPackFormatVersion2);
        EXPECT_EQ(packKindCount(
                      path, harken::kPackEntryKindCanonicalDefinitionPartition),
                  1u);
        EXPECT_FALSE(packHasKind(path, harken::kPackEntryKindContainer));
        EXPECT_FALSE(packHasKind(path, harken::kPackEntryKindVoiceRecipe));

        harken::PackCanonicalImage image;
        ASSERT_EQ(VN_OK,
                  harken::ParsePackCanonicalImage(
                      reinterpret_cast<const std::uint8_t*>(bytes.constData()),
                      static_cast<std::uint64_t>(bytes.size()), &image));
        ASSERT_TRUE(image.hasPartition);
        harken::CurrentWorldBodyImage body;
        ASSERT_EQ(harken::CurrentWorldBodyDecodeResult::Ok,
                  body.DecodeWithResult(
                      image.partitionBody.data(), image.partitionBody.size(),
                      image.partitionHeader.bodyVintage,
                      harken::CurrentWorldDecodeMode::StrictArtifact));
        const auto world = *body.ProductionView();
        const auto* row = std::find_if(
            world.playableProperties,
            world.playableProperties + world.playablePropertyCount,
            [&](const auto& candidate) {
                return candidate.stableId == harkentest::StudioObjectId(spec.objectId + kStableIdBias);
            });
        ASSERT_NE(row, world.playableProperties + world.playablePropertyCount)
            << spec.objectId;
        EXPECT_EQ(row->contentPropertyKind, spec.content.kind);
        EXPECT_NE(row->payload.presentMask & kLoopMask, 0u);
        EXPECT_EQ(row->payload.values[kLoopSlot].bits, spec.loopCount)
            << spec.objectId;
    }

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, ExportedPairsHaveDistinctPackAndActionIdentities)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("EarlyAction"), 21, kSharedSoundObjectId},
         {QStringLiteral("LateAction"), 23, kSharedSoundObjectId}}));

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
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
        const auto companion = row.value(QStringLiteral("companion")).toObject();
        const QUuid guid(companion.value(QStringLiteral("guid")).toString());
        EXPECT_FALSE(guid.isNull());
        EXPECT_FALSE(actionIds.contains(guid));
        actionIds.insert(guid);
    }
    EXPECT_EQ(actionIds.size(), 2);

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, LaterExportPreservesUnownedFiles)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

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
        writeFileBytes(QDir(nestedDir).filePath(QStringLiteral("HarkenEditorWaveforms.json")), "stale-companion");
    }
    {
        QFile notes(QDir(outputDir).filePath(QStringLiteral("notes.txt")));
        ASSERT_TRUE(notes.open(QIODevice::WriteOnly));
        notes.write("keep");
    }

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
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

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, FullRebuildReplacesRetiredOutputsWithoutReadingTheirMetadata)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    auto& registry = VNX::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(dir.path());
    const auto source = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), source, source,
        {{QStringLiteral("One"), 21, kSoloSoundObjectId}}));
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    opts.selfCleaning = true;
    for (bool wrapped : {false, true}) {
        const QDir live(QDir(dir.path()).filePath(wrapped ? "wrapped" : "plain"));
        ASSERT_TRUE(exporter.ExportAllPacks(project, live.path(), opts).success);
        const QJsonArray native = readPublication(live.path());
        ASSERT_NO_FATAL_FAILURE(renamePublicationToLegacy(live, wrapped));
        QJsonArray retiredPacks;
        QStringList retiredFiles;
        QString error;
        EXPECT_FALSE(VNX::ReadPackPublication(live.path(), {}, {}, &retiredPacks, &retiredFiles, &error));
        EXPECT_TRUE(retiredPacks.isEmpty());
        writeFileBytes(live.filePath("HarkenPackManifest.json"), "invalid-retired-metadata");
        const QString oldInit = wrapped ? QStringLiteral("Init.pack.bytes") : QStringLiteral("Init.pack");
        writeFileBytes(live.filePath("unrelated.bytes"), "other");
        writeFileBytes(live.filePath("arbitrary.pack"), "other-pack");
        writeFileBytes(live.filePath(oldInit + ".meta"), "guid");
        writeFileBytes(live.filePath("Init.bytes"), "conflict");
        const auto migrated = exporter.ExportAllPacks(project, live.path(), opts);
        ASSERT_TRUE(migrated.success) << migrated.error.toStdString();
        EXPECT_EQ(readPublication(live.path()), native);
        EXPECT_FALSE(QFile::exists(live.filePath(oldInit)));
        EXPECT_FALSE(QFile::exists(live.filePath("unrelated.bytes")));
        EXPECT_FALSE(QFile::exists(live.filePath("arbitrary.pack")));
        EXPECT_FALSE(QFile::exists(live.filePath(oldInit + ".meta")));
    }
    registry.clear();
}

TEST(EditorProjectPackExporterManifest, FullRebuildCleansDotPrefixedRetiredOutputs)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    auto& registry = VNX::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(dir.path());
    const auto source = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), source, source,
        {{QStringLiteral("One"), 21, kSoloSoundObjectId}}));
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    opts.selfCleaning = true;
    for (bool wrapped : {false, true}) {
        const QDir live(QDir(dir.path()).filePath(wrapped ? "wrapped" : "plain"));
        const auto initial = exporter.ExportAllPacks(project, live.path(), opts);
        ASSERT_TRUE(initial.success) << initial.error.toStdString();
        const auto expected = readPublication(live.path());
        ASSERT_NO_FATAL_FAILURE(renamePublicationToLegacy(live, wrapped, QStringLiteral(".Ambient")));
        const auto migrated = exporter.ExportAllPacks(project, live.path(), opts);
        ASSERT_TRUE(migrated.success) << migrated.error.toStdString();
        EXPECT_EQ(readPublication(live.path()), expected);
        EXPECT_TRUE(QFileInfo::exists(live.filePath(QStringLiteral("One.bytes"))));
        EXPECT_FALSE(QFileInfo::exists(live.filePath(QStringLiteral(".Ambient.bytes"))));
        EXPECT_FALSE(QFileInfo::exists(live.filePath(wrapped ? ".Ambient.pack.bytes" : ".Ambient.pack")));
    }
    registry.clear();
}

TEST(EditorProjectPackExporterManifest, ExportsPlayableDemoSampleFixtures)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir sources;
    QTemporaryDir first;
    QTemporaryDir second;
    ASSERT_TRUE(sources.isValid() && first.isValid() && second.isValid());
    auto& registry = VNX::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(sources.path());
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    project.name = QStringLiteral("PlayableDemo");
    project.projectId = QStringLiteral("2c2ed2ff-33b0-4940-af9e-b1d174612e2b");
    project.projectPath = QDir(sources.path()).filePath(QStringLiteral("PlayableDemo.vproj"));
    project.projectLanguages.clear();
    project.defaultLanguage.clear();
    auto master = makeItem(QStringLiteral("MasterBus"), VNX::ProjectItem::Type::Bus, 201);
    VNX::ProjectFileBinding busBinding;
    busBinding.rootItem = master;
    project.busses.push_back(busBinding);
    auto layer = makeItem(QStringLiteral("Demo"), VNX::ProjectItem::Type::Layer, 202);
    layer->fieldGroups = VNX::MakeRootFieldGroups(VNX::toFieldGroupGuid(master->guid));
    VNX::ProjectFileBinding layerBinding;
    layerBinding.rootItem = layer;
    project.layers.push_back(layerBinding);
    auto variable = makeItem(QStringLiteral("Demo_Choice"), VNX::ProjectItem::Type::Variable, 203);
    auto* declaration = variable->comp<VNX::VVariable>();
    ASSERT_NE(declaration, nullptr);
    declaration->kind = VNX::VariableKind::Step;
    declaration->rangeMin = 0.0f;
    declaration->rangeMax = 1.0f;
    declaration->defaultValue = 0.0f;
    declaration->normalizeDeclaration();
    auto variables = makeItem(QStringLiteral("Variables"), VNX::ProjectItem::Type::None, 204);
    addChild(variables, variable);
    VNX::ProjectFileBinding variableBinding;
    variableBinding.rootItem = variables;
    project.variables.push_back(variableBinding);
    auto actions = makeItem(QStringLiteral("Actions"), VNX::ProjectItem::Type::None, 205);
    VNX::ProjectFileBinding actionBinding;
    actionBinding.rootItem = actions;
    project.actions.push_back(actionBinding);
    const QStringList names = {QStringLiteral("Play_Random_Draw"), QStringLiteral("Play_Shuffle"),
        QStringLiteral("Play_Random_Queue"), QStringLiteral("Play_Sequence"),
        QStringLiteral("Play_Sequence_Queue"), QStringLiteral("Play_Variable_Control")};
    const VNX::SelectionMode modes[] = {VNX::SelectionMode::Random, VNX::SelectionMode::Random,
        VNX::SelectionMode::Random, VNX::SelectionMode::Sequence,
        VNX::SelectionMode::Sequence, VNX::SelectionMode::Match};
    for (int i = 0; i < names.size(); ++i) {
        const uint32_t id = 300 + static_cast<uint32_t>(i) * 10;
        auto container = makeItem(names[i] + QStringLiteral("_Container"), VNX::ProjectItem::Type::Container, id);
        addChild(layer, container);
        auto* selection = container->comp<VNX::VSelection>();
        selection->mode = modes[i];
        selection->playType = (i == 2 || i == 4)
            ? VNX::SelectionPlayType::Queue : VNX::SelectionPlayType::Single;
        selection->behavior.memoryScope = (i == 2 || i >= 4)
            ? VNX::SelectionMemoryScope::PerEmitter : VNX::SelectionMemoryScope::Global;
        if (i == 1) selection->behavior.discipline = VNX::RandomDiscipline::Shuffle;
        if (i == 5) {
            auto grid = selection->matchGrid();
            ASSERT_TRUE(grid.addAxis(variable->guid));
            EXPECT_TRUE(selection->selections.shared.setMatchGrid(grid));
        }
        for (int j = 0; j < 2; ++j) {
            const QString file = QStringLiteral("demo_%1_%2.wav").arg(i).arg(j);
            const uint32_t source = registerToneAsset(sources.path(), file, j == 0 ? 0.25f : 0.75f, 24000);
            VNX::AudioSourceData data;
            data.sourceID = source;
            data.relativePath = file;
            project.audioSources.push_back(data);
            auto sound = makeItem(names[i] + QStringLiteral("_%1").arg(j), VNX::ProjectItem::Type::Sound, id + 1 + j);
            HarkenTest::setSharedSoundSource(*sound, source);
            addChild(container, sound);
            ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::addContainerChildOccurrence(*container, sound->guid));
            if (i == 5) {
                {
                    auto grid = selection->matchGrid();
                    ASSERT_TRUE(grid.addRow(sound->guid));
                    ASSERT_TRUE(grid.setCell(j, 0, j));
                    EXPECT_TRUE(selection->selections.shared.setMatchGrid(grid));
                }
            }
        }
        auto action = makeItem(names[i], VNX::ProjectItem::Type::Action, id + 3);
        VNX::VActionStep play;
        play.type = VNX::VActionStep::Type::Play;
        play.targetObjectID = container->objectID;
        play.targetName = container->name;
        action->comp<VNX::VAction>()->steps.push_back(play);
        addChild(actions, action);
    }
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    for (const QString& root : {first.path(), second.path()}) {
        const QString output = VNX::ProjectPackExporter::platformOutputDir(root, project.platforms.front());
        const auto result = exporter.ExportAllPacks(project, output, options);
        ASSERT_TRUE(result.success) << result.error.toStdString();
        QString error;
        ASSERT_TRUE(VNX::ProjectPackExporter::ValidatePackOutputs(root, project.platforms, &error, result.artifacts)) << error.toStdString();
    }
    const QString output = VNX::ProjectPackExporter::platformOutputDir(first.path(), project.platforms.front());
    const QJsonArray manifest = readPublication(output);
    const QJsonArray rows = actionCompanions(manifest);
    ASSERT_EQ(rows.size(), names.size());
    QSet<QString> owners;
    for (const QString& name : names) {
        const QJsonObject row = findByName(rows, name);
        ASSERT_FALSE(row.isEmpty());
        EXPECT_FALSE(QUuid(row.value(QStringLiteral("guid")).toString()).isNull());
        owners.insert(findByName(manifest, name).value(QStringLiteral("packStableId")).toString());
    }
    EXPECT_EQ(owners.size(), 6);
    const QJsonArray variablesJson = findByName(manifest, QStringLiteral("Init"))
        .value(QStringLiteral("companion")).toObject().value(QStringLiteral("variables")).toArray();
    ASSERT_EQ(variablesJson.size(), 1);
    EXPECT_EQ(variablesJson[0].toObject().value(QStringLiteral("name")).toString(), QStringLiteral("Demo_Choice"));
    EXPECT_EQ(QUuid(variablesJson[0].toObject().value(QStringLiteral("guid")).toString()), variable->guid);
    QStringList relativeFiles;
    const QString platformRelative = QDir(first.path()).relativeFilePath(output);
    ASSERT_EQ(manifest.size(), 7);
    for (const QString& file : publicationFiles(output))
        relativeFiles << platformRelative + QLatin1Char('/') + file;
    for (const QString& relative : relativeFiles) {
        const QByteArray bytes = readFileBytes(QDir(first.path()).filePath(relative));
        ASSERT_FALSE(bytes.isEmpty()) << relative.toStdString();
        ASSERT_EQ(bytes, readFileBytes(QDir(second.path()).filePath(relative))) << relative.toStdString();
    }
    const QString preserved = QString::fromUtf8(qgetenv("HARKEN_UNITY_FIXTURE_OUT"));
    if (!preserved.isEmpty()) {
        ASSERT_FALSE(QDir(preserved).exists());
        for (const QString& relative : relativeFiles) {
            const QString target = QDir(preserved).filePath(relative);
            ASSERT_TRUE(QDir().mkpath(QFileInfo(target).absolutePath()));
            ASSERT_TRUE(QFile::copy(QDir(first.path()).filePath(relative), target));
        }
    }
}

TEST(EditorProjectPackExporterManifest, LanguageVariantsKeepDistinctPackIdentities)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("Spoken"), 21, kSoloSoundObjectId}}));
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, soloSourceId));
    project.buildExportPackMetadata = true;

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const auto result = exporter.ExportAllPacks(project, dir.path(), opts);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QString englishName = variantFileName(project, harkentest::StudioObjectId(21 + kStableIdBias), QStringLiteral("English"));
    const QString jaName = variantFileName(project, harkentest::StudioObjectId(21 + kStableIdBias), QStringLiteral("ja"));
    EXPECT_TRUE(QFileInfo::exists(QDir(dir.path()).filePath(englishName)));
    EXPECT_TRUE(QFileInfo::exists(QDir(dir.path()).filePath(jaName)));
    EXPECT_EQ(QFileInfo(englishName).fileName(), QFileInfo(jaName).fileName());

    const QByteArray initBytes =
        readFileBytes(QDir(dir.path()).filePath(QStringLiteral("Init.bytes")));
    harken::PackCanonicalImage initImage;
    ASSERT_EQ(VN_OK,
              harken::ParsePackCanonicalImage(
                  reinterpret_cast<const std::uint8_t*>(initBytes.constData()),
                  static_cast<std::uint64_t>(initBytes.size()), &initImage));
    EXPECT_FALSE(initImage.hasCatalog);
    EXPECT_TRUE(initImage.catalogRows.empty());
    expectInitHasVolumeOnlyKind24(QDir(dir.path()).filePath(QStringLiteral("Init.bytes")));

    const std::uint64_t englishId = fnvUtf8(HarkenTest::platformIdString("Windows") + QStringLiteral("/English/action_3da91a64-b947-4560-0000-0000000186b5"));
    const std::uint64_t jaId = fnvUtf8(HarkenTest::platformIdString("Windows") + QStringLiteral("/ja/action_3da91a64-b947-4560-0000-0000000186b5"));
    EXPECT_NE(englishId, jaId);
    EXPECT_FALSE(initImage.hasGameplayCatalog);
    const QJsonArray packs = readPublication(dir.path());
    const auto findByStableId = [&](std::uint64_t id) {
        const QString text = QString::number(id);
        for (const QJsonValue& value : packs) {
            const QJsonObject object = value.toObject();
            if (object.value(QStringLiteral("packStableId")).toString() == text)
                return object;
        }
        return QJsonObject();
    };
    const QJsonObject englishPack = findByStableId(englishId);
    const QJsonObject jaPack = findByStableId(jaId);
    ASSERT_FALSE(englishPack.isEmpty());
    ASSERT_FALSE(jaPack.isEmpty());
    EXPECT_EQ(englishPack.value(QStringLiteral("path")).toString(), englishName);
    EXPECT_EQ(jaPack.value(QStringLiteral("path")).toString(), jaName);
    EXPECT_NE(englishPack.value(QStringLiteral("packStableId")).toString(),
              jaPack.value(QStringLiteral("packStableId")).toString());
    EXPECT_EQ(englishPack.value(QStringLiteral("name")).toString(),
              QStringLiteral("Spoken"));
    EXPECT_EQ(jaPack.value(QStringLiteral("name")).toString(),
              QStringLiteral("Spoken"));
    EXPECT_EQ(englishPack.value(QStringLiteral("language")).toString(),
              QStringLiteral("English"));
    EXPECT_EQ(jaPack.value(QStringLiteral("language")).toString(),
              QStringLiteral("ja"));
    expectCanonicalPackParsesWithPartition(QDir(dir.path()).filePath(englishName));
    expectCanonicalPackParsesWithPartition(QDir(dir.path()).filePath(jaName));

    for (const QString& language : project.projectLanguages) {
        const QJsonArray selected = readPublication(dir.path(), language);
        const QJsonArray selectedPacks = selected;
        ASSERT_EQ(selectedPacks.size(), 2);
        const QJsonObject selectedInit = findByName(selectedPacks, QStringLiteral("Init"));
        EXPECT_EQ(selectedInit.value(QStringLiteral("path")).toString(),
                  QStringLiteral("Init.bytes"));
        expectInitHasVolumeOnlyKind24(
            QDir(dir.path()).filePath(selectedInit.value(QStringLiteral("path")).toString()));
        const auto selectedAction = findByName(selected, QStringLiteral("Spoken"));
        const auto owner = language == QStringLiteral("English") ? englishId : jaId;
        EXPECT_EQ(selectedAction.value(QStringLiteral("language")).toString(), language);
        const auto metadata = selectedAction.value(QStringLiteral("companion")).toObject()
            .value(QStringLiteral("metadata")).toObject();
        EXPECT_EQ(metadata.value(QStringLiteral("language")).toString(), language);
        EXPECT_EQ(metadata.value(QStringLiteral("packStableId")).toString(), QString::number(owner));
        EXPECT_EQ(selectedAction.value(QStringLiteral("packStableId")).toString(), QString::number(owner));
        EXPECT_EQ(QUuid(selectedAction.value(QStringLiteral("companion")).toObject()
            .value(QStringLiteral("guid")).toString()), project.actions.front().rootItem->children.front()->guid);
        const auto selectedBytes = readFileBytes(QDir(dir.path()).filePath(
            selectedAction.value(QStringLiteral("path")).toString()));
        harken::PackCanonicalImage selectedImage;
        ASSERT_EQ(VN_OK, harken::ParsePackCanonicalImage(
            reinterpret_cast<const std::uint8_t*>(selectedBytes.constData()), selectedBytes.size(), &selectedImage));
        EXPECT_EQ(selectedImage.packStableId, owner);
        ASSERT_FALSE(selectedImage.dependencyEdges.empty());
        EXPECT_EQ(selectedImage.dependencyEdges.front().prerequisitePackStableId,
            selectedInit.value(QStringLiteral("packStableId")).toString().toULongLong());

        harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
        ASSERT_EQ(session.InitResult(), VN_OK);
        const QByteArray initPath = QDir(dir.path()).filePath(
            selectedInit.value(QStringLiteral("path")).toString()).toUtf8();
        VnCanonicalPackSetMemberV1 members[2]{};
        members[0].structSize = sizeof(members[0]);
        members[0].packKind = VN_CANONICAL_PACK_SET_MEMBER_INIT;
        members[0].packStableId = selectedInit.value(QStringLiteral("packStableId")).toString().toULongLong();
        members[0].utf8Path = initPath.constData();
        VnCanonicalPackSetMemberResultV1 initRow{};
        initRow.structSize = sizeof(initRow);
        VnCanonicalPackSetActivationReportV1 initial{};
        ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(members, 1, &initRow, initial));
        ASSERT_NE(0u, initRow.handle);
        const auto* before = session.Engine()->packComposition.Published();
        ASSERT_NE(before, nullptr);
        members[0].expectedHandle = initRow.handle;
        members[1].structSize = sizeof(members[1]);
        members[1].packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
        const QByteArray contentPath = QDir(dir.path()).filePath(
            language == QStringLiteral("English") ? englishName : jaName).toUtf8();
        members[1].packStableId = owner;
        members[1].utf8Path = contentPath.constData();
        VnCanonicalPackSetMemberResultV1 accepted[2]{};
        for (auto& row : accepted)
            row.structSize = sizeof(row);
        VnCanonicalPackSetActivationReportV1 activation{};
        ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(
            members, 2, accepted, activation, initial.initHandle, initial.resultSetRevision));
        ASSERT_NE(0u, accepted[1].handle);
        const auto* active = session.Engine()->packComposition.Published();
        ASSERT_NE(active, nullptr);
        EXPECT_EQ(active->owners.size(), 2u);
    }
    preserveUnityFixture(dir.path());

    std::map<QString, QByteArray> publication;
    for (const QString& relative : publicationFiles(dir.path()))
        publication.emplace(relative, readFileBytes(QDir(dir.path()).filePath(relative)));
    QTemporaryDir repeatedDir;
    ASSERT_TRUE(repeatedDir.isValid());
    const auto repeated = exporter.ExportAllPacks(project, repeatedDir.path(), opts);
    ASSERT_TRUE(repeated.success) << repeated.error.toStdString();
    for (const auto& [relative, bytes] : publication)
        EXPECT_EQ(readFileBytes(QDir(repeatedDir.path()).filePath(relative)), bytes) << relative.toStdString();

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, LanguageTargetingPublishesIndependentPlayablePairs)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    const auto shared = registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const auto voice = registerToneAsset(dir.path(), QStringLiteral("voice.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "macOS"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), shared, voice,
        {{QStringLiteral("Common"), 21, kSharedSoundObjectId},
         {QStringLiteral("FirstVoice"), 22, kSoloSoundObjectId},
         {QStringLiteral("SecondVoice"), 23, kSoloSoundObjectId}}));
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, voice));
    project.defaultLanguage = QStringLiteral("English");
    project.platforms = HarkenTest::platforms({"Windows", "macOS"});
    for (auto& sources : project.layers.front().rootItem->children.at(1)->comp<VNX::VSound>()->sources.localized)
        sources.active = sources.entries.front().entryId;
    auto separateLayer = makeItem(QStringLiteral("OtherVoices"), VNX::ProjectItem::Type::Layer, 11);
    separateLayer->fieldGroups = project.layers.front().rootItem->fieldGroups;
    auto separateSound = makeItem(QStringLiteral("OtherVoice"), VNX::ProjectItem::Type::Sound, 3);
    separateSound->comp<VNX::VSound>()->distinguishLanguage = true;
    separateSound->comp<VNX::VSound>()->sources =
        project.layers.front().rootItem->children.at(1)->comp<VNX::VSound>()->sources;
    addChild(separateLayer, separateSound);
    project.layers.push_back({separateLayer, QStringLiteral("Layers")});
    project.actions.front().rootItem->children.at(2)->comp<VNX::VAction>()->steps.front().targetObjectID = 3;
    const auto first = harkentest::StudioObjectId(22 + kStableIdBias);
    const auto second = harkentest::StudioObjectId(23 + kStableIdBias);
    const QString windows = HarkenTest::platformIdString("Windows");
    const QString mac = HarkenTest::platformIdString("macOS");
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.selection.actionTargeting = {{first, false, {mac}, QStringLiteral("ja")},
                                     {second, false, {mac}, QStringLiteral("English")}};
    opts.platform = windows;
    const QString output = dir.filePath(QStringLiteral("Output"));
    const auto result = exporter.ExportAllPacks(project, output, opts);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    for (const QString& language : project.projectLanguages) {
        SCOPED_TRACE(language.toStdString());
        const QString selected = language == "English" ? "FirstVoice" : "SecondVoice";
        const QString excluded = language == "English" ? "SecondVoice" : "FirstVoice";
        const auto manifest = readPublication(output, language);
        const auto catalog = actionCompanions(manifest);
        ASSERT_EQ(catalog.size(), 2);
        EXPECT_FALSE(findByName(catalog, selected).isEmpty());
        EXPECT_FALSE(findByName(catalog, "Common").isEmpty());
        EXPECT_TRUE(findByName(catalog, excluded).isEmpty());
        EXPECT_FALSE(QFile::exists(QDir(output).filePath(language + '/' + excluded + ".bytes")));
        harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
        ASSERT_EQ(session.InitResult(), VN_OK);
        ASSERT_NO_FATAL_FAILURE(activateExportedNames(output, {"Common", selected}, language));
        EXPECT_EQ(VN_OK, Vn_Update());
    }
    const QString macOutput = dir.filePath(QStringLiteral("MacOutput"));
    auto macOpts = opts;
    macOpts.platform = mac;
    const auto macResult = exporter.ExportAllPacks(project, macOutput, macOpts);
    ASSERT_TRUE(macResult.success) << macResult.error.toStdString();
    for (const auto& language : project.projectLanguages)
        EXPECT_EQ(actionCompanions(readPublication(macOutput, language)).size(), 3);
    opts.selection.actionTargeting.push_back({first, false, {mac}, QStringLiteral("English")});
    opts.currentWorldOutputPath = dir.filePath(QStringLiteral("Selected.world"));
    opts.selfCleaning = true;
    const auto fallback = exporter.ExportAllPacks(project, output, opts);
    ASSERT_TRUE(fallback.success) << fallback.error.toStdString();
    EXPECT_EQ(project.defaultLanguage, QStringLiteral("English"));
    EXPECT_TRUE(findByName(readPublication(output, QStringLiteral("English")), QStringLiteral("FirstVoice")).isEmpty());
    EXPECT_EQ(findByName(readPublication(output), QStringLiteral("SecondVoice"))
        .value(QStringLiteral("language")).toString(), QStringLiteral("ja"));
    EXPECT_FALSE(QFile::exists(QDir(output).filePath("English/FirstVoice.bytes")));
    EXPECT_TRUE(QFile::exists(opts.currentWorldOutputPath));
    EXPECT_NE(fallback.currentWorldSemanticFingerprint, 0u);

    opts.selection.actionTargeting.clear();
    opts.onlyPackPaths = {QStringLiteral("ja/SecondVoice")};
    const auto selected = exporter.ExportAllPacks(project, output, opts);
    ASSERT_TRUE(selected.success) << selected.error.toStdString();
    const auto packs = readPublication(output);
    ASSERT_EQ(packs.size(), 2);
    EXPECT_EQ(findByName(packs, "SecondVoice").value("language").toString(), QStringLiteral("ja"));
    EXPECT_TRUE(findByName(packs, "FirstVoice").isEmpty());
    EXPECT_TRUE(findByName(packs, "Common").isEmpty());

    VNX::VActionStep post;
    post.type = VNX::VActionStep::Type::PostAction;
    post.targetObjectID = 23;
    project.actions.front().rootItem->children.at(1)->comp<VNX::VAction>()->steps = {post};
    opts.onlyPackPaths = {QStringLiteral("ja/FirstVoice")};
    const auto dependencyExport = exporter.ExportAllPacks(project, output, opts);
    ASSERT_TRUE(dependencyExport.success) << dependencyExport.error.toStdString();
    const auto dependencyManifest = readPublication(output);
    const auto dependencyPacks = dependencyManifest;
    ASSERT_EQ(dependencyPacks.size(), 2);
    EXPECT_EQ(findByName(dependencyPacks, "FirstVoice").value("language").toString(), QStringLiteral("ja"));
    EXPECT_TRUE(findByName(dependencyPacks, "SecondVoice").isEmpty());
    EXPECT_FALSE(QFileInfo::exists(QDir(output).filePath("ja/SecondVoice.bytes")));
    QByteArray closureBytes;
    harken::PackCanonicalImage closureImage;
    harken::CurrentWorldBodyImage closureWorld;
    ASSERT_NO_FATAL_FAILURE(readCanonicalWorld(QDir(output).filePath("ja/FirstVoice.bytes"),
        closureBytes, closureImage, closureWorld));
    ASSERT_EQ(closureWorld.ProductionView()->actionCount, 2u);
    EXPECT_EQ(closureImage.media.size(), 1u);
    EXPECT_EQ(findByName(dependencyPacks, "FirstVoice").value("dependencies").toArray(),
        QJsonArray{findByName(dependencyPacks, "Init").value("packStableId")});
    {
        harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
        ASSERT_EQ(session.InitResult(), VN_OK);
        ASSERT_NO_FATAL_FAILURE(activateExportedNames(output, {"FirstVoice"}, "ja"));
        EXPECT_EQ(VN_OK, Vn_Update());
    }
    opts.selection.actionTargeting.push_back({second, false, {mac}, QStringLiteral("ja")});
    const auto excludedDependency = exporter.ExportAllPacks(project, output, opts);
    ASSERT_TRUE(excludedDependency.success) << excludedDependency.error.toStdString();
    EXPECT_EQ(readFileBytes(QDir(output).filePath("ja/FirstVoice.bytes")), closureBytes);
    EXPECT_EQ(readPublication(output), dependencyManifest);
    separateSound->comp<VNX::VSound>()->sources.localized[QStringLiteral("ja")].entries.clear();
    const auto invalidClosure = exporter.ExportAllPacks(project, output, opts);
    EXPECT_FALSE(invalidClosure.success);
    EXPECT_EQ(readPublication(output), dependencyManifest);
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, SelectedNonLocalizedActionsAreSelfContained)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
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
    auto* unselectedSound = project.layers.front().rootItem->children.at(1)->comp<VNX::VSound>();
    unselectedSound->sources.plain.active = QUuid{};
    unselectedSound->sources.plain.platforms.clear();

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    opts.onlyUnits = {QStringLiteral("First"), QStringLiteral("Second")};
    opts.language = QStringLiteral("French");
    const QString outputDir = VNX::ProjectPackExporter::platformOutputDir(
        dir.path(), *VNX::ProjectPlatformSemantics::find(project.platforms, QUuid(opts.platform)));
    std::vector<VNX::ProjectPackExporter::PackBuildRecord> records;
    const auto result = exporter.ExportAllPacks(project, outputDir, opts, &records);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QJsonArray manifest = readPublication(outputDir);
    const QJsonArray packs = manifest;
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
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, LocalizedActionEmbedsNonLocalizedExecutionClosure)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("Spoken"), 21, kSoloSoundObjectId},
         {QStringLiteral("Common"), 22, kSharedSoundObjectId}}));
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, soloSourceId));
    VNX::VActionStep post;
    post.type = VNX::VActionStep::Type::PostAction;
    post.targetName = QStringLiteral("Common");
    project.actions.front().rootItem->children.front()->comp<VNX::VAction>()->steps.push_back(post);

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    opts.onlyUnits = {QStringLiteral("Spoken")};
    const QString outputDir = VNX::ProjectPackExporter::platformOutputDir(
        dir.path(), *VNX::ProjectPlatformSemantics::find(project.platforms, QUuid(opts.platform)));
    const auto result = exporter.ExportAllPacks(project, outputDir, opts);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    QString error;
    ASSERT_TRUE(VNX::ProjectPackExporter::ValidatePackOutputs(dir.path(), {*VNX::ProjectPlatformSemantics::find(project.platforms, QUuid(opts.platform))}, &error, result.artifacts))
        << error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(QDir(dir.path()).filePath(QStringLiteral("HarkenPackIndex.json"))));

    const QJsonArray packs = readPublication(outputDir);
    ASSERT_EQ(packs.size(), 3);
    EXPECT_TRUE(findByName(packs, QStringLiteral("Shared")).isEmpty());
    EXPECT_TRUE(findByName(packs, QStringLiteral("Common")).isEmpty());
    EXPECT_FALSE(QFileInfo::exists(QDir(outputDir).filePath(QStringLiteral("Common.bytes"))));
    const auto initId = findByName(packs, QStringLiteral("Init")).value(QStringLiteral("packStableId"));
    int spokenCount = 0;
    for (const QJsonValue& value : packs) {
        const QJsonObject pack = value.toObject();
        const QJsonArray dependencies = pack.value(QStringLiteral("dependencies")).toArray();
        const QString relative = pack.value(QStringLiteral("path")).toString();
        EXPECT_TRUE(QFileInfo::exists(QDir(outputDir).filePath(relative)));
        if (pack.value(QStringLiteral("name")).toString() != QStringLiteral("Spoken"))
            continue;
        ++spokenCount;
        EXPECT_EQ(dependencies, QJsonArray{initId});
        const QString language = pack.value(QStringLiteral("language")).toString();
        EXPECT_EQ(relative, language + QStringLiteral("/Spoken.bytes"));
        QByteArray bytes;
        harken::PackCanonicalImage image;
        harken::CurrentWorldBodyImage world;
        ASSERT_NO_FATAL_FAILURE(readCanonicalWorld(
            QDir(outputDir).filePath(relative), bytes, image, world));
        const auto& view = *world.ProductionView();
        ASSERT_EQ(view.actionCount, 2u);
        const auto* spoken = std::find_if(view.actions, view.actions + view.actionCount,
            [](const auto& action) { return action.actionStableId == harkentest::StudioObjectId(21 + kStableIdBias); });
        const auto* common = std::find_if(view.actions, view.actions + view.actionCount,
            [](const auto& action) { return action.actionStableId == harkentest::StudioObjectId(22 + kStableIdBias); });
        ASSERT_NE(spoken, view.actions + view.actionCount);
        ASSERT_NE(common, view.actions + view.actionCount);
        ASSERT_EQ(spoken->stepCount, 2u);
        EXPECT_EQ(spoken->steps[1].target.stableId, common->actionStableId);
        EXPECT_EQ(spoken->steps[1].target.expectedProgramSemanticFingerprint,
                  common->programSemanticFingerprint);
        ASSERT_EQ(image.media.size(), 2u);
        for (const auto soundId : {kSharedSoundObjectId, kSoloSoundObjectId})
            EXPECT_NE(std::find_if(image.media.begin(), image.media.end(), [&](const auto& media) {
                return media.soundId == harkentest::StudioObjectId(soundId + kStableIdBias);
            }), image.media.end());
    }
    EXPECT_EQ(spokenCount, 2);
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, MixedLanguageVariantsOwnMatchingDefinitionsAndMedia)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f, 4096);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f, 4096);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("Mixed"), 21, kSoloSoundObjectId}}));
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, soloSourceId));
    VNX::VActionStep playCommon;
    playCommon.type = VNX::VActionStep::Type::Play;
    playCommon.targetObjectID = kSharedSoundObjectId;
    auto* action = project.actions.front().rootItem->children.front()->comp<VNX::VAction>();
    action->steps.front().targetObjectID = 0;
    action->steps.front().targetName = QStringLiteral("SoloTone");
    action->steps.push_back(playCommon);
    auto* sound = project.layers.front().rootItem->children.at(1)->comp<VNX::VSound>();
    sound->sources.localized.clear();
    const QStringList platforms = {QStringLiteral("Windows"), QStringLiteral("Android")};
    std::map<QString, std::uint64_t> expectedRevisions;
    float amplitude = 0.125f;
    for (const QString& platform : platforms) {
        for (const QString& language : project.projectLanguages) {
            const QString key = platform + QLatin1Char('/') + language;
            const uint32_t sourceId = registerToneAsset(
                dir.path(), key + QStringLiteral(".wav"), amplitude);
            amplitude += 0.125f;
            const auto* asset = VNX::AssetRegistry::instance().getAsset(sourceId);
            ASSERT_NE(asset, nullptr);
            VNX::AudioSourceData audioSource;
            audioSource.sourceID = sourceId;
            audioSource.relativePath = asset->relativePath;
            project.audioSources.push_back(audioSource);
            VNX::SoundSourceDef source;
            source.entryId = QUuid::createUuid();
            source.sourceId = sourceId;
            source.trimStartFrame = 8;
            source.trimEndFrame = 88;
            sound->sources.localized[language].entries.push_back(source);
            sound->sources.localized[language].platforms[HarkenTest::platformIdString(platform)].active = source.entryId;
            std::vector<float> samples;
            int channels = 0;
            int sampleRate = 0;
            ASSERT_TRUE(HarkenEditor::Bridge::decodeAudioFileInterleaved(
                asset->absolutePath, samples, channels, sampleRate));
            ASSERT_EQ(channels, 1);
            std::vector<float> cropped(samples.begin() + source.trimStartFrame,
                                       samples.begin() + *source.trimEndFrame);
            const float* pcm = cropped.data();
            ASSERT_TRUE(harken::ComputeMediaPcmContentRevisionV1(
                sampleRate, channels, cropped.size(), &pcm, 1, &expectedRevisions[key]));
        }
    }

    VNX::ProjectPackExporter exporter;
    std::map<QString, std::uint64_t> commonRevisions;
    QSet<QString> packIds;
    QVector<VNX::PackArtifactMetadata> artifacts;
    for (const QString& platform : platforms) {
        VNX::ProjectPackExporter::ExportOptions opts;
        opts.platform = HarkenTest::platformIdString(platform);
        const QString outputDir = VNX::ProjectPackExporter::platformOutputDir(dir.path(), *VNX::ProjectPlatformSemantics::find(project.platforms, HarkenTest::platformId(platform)));
        const auto result = exporter.ExportAllPacks(project, outputDir, opts);
        ASSERT_TRUE(result.success) << result.error.toStdString();
        artifacts += result.artifacts;
        const QJsonArray packs = readPublication(outputDir);
        ASSERT_EQ(packs.size(), 3);
        EXPECT_TRUE(findByName(packs, QStringLiteral("Shared")).isEmpty());
        for (const QString& language : project.projectLanguages) {
            SCOPED_TRACE((platform + QLatin1Char('/') + language).toStdString());
            const QString relative = variantFileName(project, harkentest::StudioObjectId(21 + kStableIdBias), language);
            QByteArray bytes;
            harken::PackCanonicalImage image;
            harken::CurrentWorldBodyImage body;
            ASSERT_NO_FATAL_FAILURE(readCanonicalWorld(
                QDir(outputDir).filePath(relative), bytes, image, body));
            EXPECT_EQ(image.packStableId, fnvUtf8(HarkenTest::platformIdString(platform) + QLatin1Char('/')
                + language + QStringLiteral("/action_3da91a64-b947-4560-0000-0000000186b5")));
            packIds.insert(QString::number(image.packStableId));
            const auto& world = *body.ProductionView();
            ASSERT_EQ(world.actionCount, 1u);
            EXPECT_EQ(world.actions[0].actionStableId, harkentest::StudioObjectId(21 + kStableIdBias));
            ASSERT_EQ(world.actions[0].stepCount, 2u);
            ASSERT_EQ(image.media.size(), 2u);
            ASSERT_EQ(world.playablePropertyCount, 2u);
            for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i) {
                const auto& property = world.playableProperties[i];
                const auto media = std::find_if(image.media.begin(), image.media.end(),
                                               [&](const auto& row) {
                    return row.soundId == property.stableId;
                });
                ASSERT_NE(media, image.media.end());
                ASSERT_NO_FATAL_FAILURE(expectMediaMatchesDefinition(bytes, *media, property));
                if (property.stableId == harkentest::StudioObjectId(kSoloSoundObjectId + kStableIdBias)) {
                    EXPECT_EQ(media->frameCount, 80u);
                    EXPECT_EQ(property.contentRevision,
                              expectedRevisions.at(platform + QLatin1Char('/') + language));
                } else {
                    EXPECT_EQ(property.stableId, harkentest::StudioObjectId(kSharedSoundObjectId + kStableIdBias));
                    const auto expected =
                        commonRevisions.emplace(platform, property.contentRevision).first;
                    EXPECT_EQ(property.contentRevision, expected->second);
                    EXPECT_EQ(media->sampleRate, 48000u);
                }
                const auto step = std::find_if(
                    world.actions[0].steps, world.actions[0].steps + world.actions[0].stepCount,
                    [&](const auto& value) { return value.target.stableId == property.stableId; });
                ASSERT_NE(step, world.actions[0].steps + world.actions[0].stepCount);
                EXPECT_EQ(step->target.expectedProgramSemanticFingerprint,
                          property.programSemanticFingerprint);
            }
            const QJsonArray selected = readPublication(outputDir, language);
            const QJsonObject selectedInit = findByName(selected, QStringLiteral("Init"));
            ASSERT_FALSE(selectedInit.isEmpty());
            ASSERT_NO_FATAL_FAILURE(expectPackRendersInIndependentSession(
                outputDir, QDir(outputDir).filePath(selectedInit.value(QStringLiteral("path")).toString()),
                QDir(outputDir).filePath(relative), image.packStableId,
                harkentest::StudioObjectId(21 + kStableIdBias)));
        }
    }
    EXPECT_EQ(packIds.size(), 4);
    ASSERT_EQ(commonRevisions.size(), 2u);
    EXPECT_EQ(commonRevisions.at(QStringLiteral("Windows")),
              commonRevisions.at(QStringLiteral("Android")));
    QString error;
    ASSERT_TRUE(VNX::ProjectPackExporter::ValidatePackOutputs(dir.path(), project.platforms, &error, artifacts))
        << error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(QDir(dir.path()).filePath(QStringLiteral("HarkenPackIndex.json"))));
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, RuntimeBundlePreservesLanguagePathsThroughDeploy)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
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
    VNX::AudioSourceData japaneseSource;
    japaneseSource.sourceID = japaneseSourceId;
    japaneseSource.relativePath = QStringLiteral("japanese.wav");
    project.audioSources.push_back(japaneseSource);
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, englishSourceId));
    project.layers.front().rootItem->children.at(1)->comp<VNX::VSound>()
        ->sources.localized["ja"].entries.front().sourceId = japaneseSourceId;
    project.buildExportWaveforms = true;

    const QString publishRoot = QDir(dir.path()).filePath(QStringLiteral("published"));
    const auto bundle = VNX::RuntimeContentBundleBuilder::build(
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
    ASSERT_TRUE(VNX::RuntimeContentBundleBuilder::deploy(bundle, deployRoot, &error))
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
            const auto waves = spoken.value(QStringLiteral("companion")).toObject()
                .value(QStringLiteral("waveforms")).toArray();
            ASSERT_EQ(waves.size(), 1);
            const auto wave = waves.first().toObject();
            const QDir companionDir(QFileInfo(QDir(platformRoot).filePath(
                spoken.value(QStringLiteral("path")).toString())).absolutePath());
            const QString waveformPath = QDir::cleanPath(companionDir.absoluteFilePath(
                wave.value(QStringLiteral("file")).toString()));
            EXPECT_EQ(QFileInfo(waveformPath).absolutePath(),
                QDir(platformRoot).absoluteFilePath(QStringLiteral("Waveforms")));
            const auto sourceId = language == QStringLiteral("English") ? englishSourceId : japaneseSourceId;
            EXPECT_EQ(readFileBytes(waveformPath), readFileBytes(VNX::WaveformCache::cachePath(
                VNX::AssetRegistry::instance().getAsset(sourceId)->absolutePath)));
            const auto selectedInit = findByName(selected, QStringLiteral("Init"));
            expectCanonicalPackParsesWithPartition(QDir(platformRoot).filePath(
                selectedInit.value(QStringLiteral("path")).toString()));
            const QByteArray contents = readFileBytes(QDir(root).filePath(
                prefix + language + QStringLiteral("/Spoken.bytes")));
            harken::PackCanonicalImage image;
            ASSERT_EQ(VN_OK, harken::ParsePackCanonicalImage(
                reinterpret_cast<const std::uint8_t*>(contents.constData()),
                static_cast<std::uint64_t>(contents.size()), &image));
            ASSERT_EQ(image.media.size(), 1u);
            const auto& media = image.media.front();
            ASSERT_EQ(media.channelCount, 1u);
            ASSERT_EQ(media.sampleFormat, harken::kPackSampleFormatF32Planar);
            ASSERT_LE(media.blobOffset + 2 * sizeof(float),
                      static_cast<std::uint64_t>(contents.size()));
            float sample = 0.0f;
            std::memcpy(&sample, contents.constData() + media.blobOffset + sizeof(float),
                        sizeof(sample));
            EXPECT_FLOAT_EQ(sample, (language == QStringLiteral("English") ? 0.5f : 0.875f) / 16.0f);
        }
    }
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, RuntimeBundleKeepsAllActionsAvailable)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t commonSourceId =
        registerToneAsset(dir.path(), QStringLiteral("common.wav"), 1.0f);
    const uint32_t sourceId = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), commonSourceId, sourceId,
        {{QStringLiteral("One"), 21, kSoloSoundObjectId},
         {QStringLiteral("Two"), 22, kSharedSoundObjectId}}));
    project.projectLanguages = {QStringLiteral("English"), QStringLiteral("ja")};
    const auto bundle = VNX::RuntimeContentBundleBuilder::build(
        project, QDir(dir.path()).filePath(QStringLiteral("published")),
        HarkenTest::platformIdString("Windows"));
    ASSERT_TRUE(bundle.success) << bundle.error.toStdString();
    ASSERT_EQ(bundle.orderedPacks.size(), 3u);
    ASSERT_EQ(bundle.actions.size(), 2u);
    std::map<harkenstudio::ObjectId, QString> actions;
    for (const auto& action : bundle.actions)
        actions.emplace(action.stableId, action.name);
    EXPECT_EQ(actions, (std::map<harkenstudio::ObjectId, QString>{
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
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, ExternalLocalizedOccurrenceKeepsInvalidOwnershipRejected)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("External"), 21, 3}}));
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, soloSourceId));
    auto container = makeItem(QStringLiteral("ExternalVoice"), VNX::ProjectItem::Type::Container, 3);
    container->comp<VNX::VSelection>()->selections.shared.occurrenceOrdinalHighWater = 1;
    EXPECT_TRUE(container->comp<VNX::VSelection>()->selections.shared.setOccurrenceRows({{project.layers.front().rootItem->children.at(1)->guid, 0, {}}}));
    addChild(project.layers.front().rootItem, container);
    const auto plan = VNX::PackBuildPlanning::buildForAction(project, harkentest::StudioObjectId(21 + kStableIdBias),
        {HarkenTest::platformIdString("Windows")});
    ASSERT_EQ(plan.variants.size(), 2u);
    for (const auto& variant : plan.variants) {
        EXPECT_TRUE(project.projectLanguages.contains(variant.variant.language));
        ASSERT_EQ(variant.closure.media.size(), 1u);
        EXPECT_EQ(variant.closure.media.front().soundStableId, harkentest::StudioObjectId(kSoloSoundObjectId + kStableIdBias));
    }
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const QString outputDir = QDir(dir.path()).filePath(QStringLiteral("out"));
    const auto result = exporter.ExportAllPacks(project, outputDir, opts);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("direct child")))
        << result.error.toStdString();
    EXPECT_TRUE(container->children.empty());
    QDirIterator packs(outputDir, {"*.bytes"}, QDir::Files, QDirIterator::Subdirectories);
    EXPECT_FALSE(packs.hasNext());
    EXPECT_FALSE(QFileInfo::exists(
        QDir(outputDir).filePath(QStringLiteral("HarkenPackManifest.json"))));
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, LocalizedSinglePackRequiresExplicitLanguage)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("Spoken"), 21, kSoloSoundObjectId}}));
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, soloSourceId));
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const QString path = QDir(dir.path()).filePath(QStringLiteral("single.bytes"));
    auto result = exporter.ExportActionPack(project, QStringLiteral("Spoken"), path, opts);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("requires an explicit project language")))
        << result.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(path));
    opts.language = QStringLiteral("english");
    result = exporter.ExportActionPack(project, QStringLiteral("Spoken"), path, opts);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("not a project language")))
        << result.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(path));
    opts.language = QStringLiteral("ja");
    result = exporter.ExportActionPack(project, QStringLiteral("Spoken"), path, opts);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    EXPECT_EQ(result.mediaCount, 1);
    EXPECT_TRUE(packHasKind(path, harken::kPackEntryKindMedia));
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, MissingLocalizedActiveWritesNoPacks)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("Spoken"), 21, kSoloSoundObjectId}}));
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, soloSourceId));
    auto* sound = project.layers.front().rootItem->children.at(1)->comp<VNX::VSound>();
    sound->sources.localized["ja"].platforms.clear();
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const QString outputDir = QDir(dir.path()).filePath(QStringLiteral("out"));
    auto result = exporter.ExportAllPacks(project, outputDir, opts);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("source active is not configured for %1/ja").arg(opts.platform)))
        << result.error.toStdString();
    QDirIterator packs(outputDir, {"*.bytes"}, QDir::Files, QDirIterator::Subdirectories);
    EXPECT_FALSE(packs.hasNext());
    EXPECT_FALSE(QFileInfo::exists(
        QDir(outputDir).filePath(QStringLiteral("HarkenPackManifest.json"))));
    opts.language = QStringLiteral("ja");
    const QString path = QDir(dir.path()).filePath(QStringLiteral("single.bytes"));
    result = exporter.ExportActionPack(project, QStringLiteral("Spoken"), path, opts);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("source active is not configured for %1/ja").arg(opts.platform)))
        << result.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(path));
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, CommonAndLocalizedDefinitionsStayInIndependentPacks)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("Mixed"), 21, kSoloSoundObjectId},
         {QStringLiteral("Common"), 22, kSharedSoundObjectId}}));
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, soloSourceId));
    VNX::VActionStep play;
    play.type = VNX::VActionStep::Type::Play;
    play.targetObjectID = kSharedSoundObjectId;
    project.actions.front().rootItem->children.front()->comp<VNX::VAction>()->steps.push_back(play);
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const QString outputDir = QDir(dir.path()).filePath(QStringLiteral("out"));
    const auto result = exporter.ExportAllPacks(project, outputDir, opts);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    QDirIterator packs(outputDir, {"*.bytes"}, QDir::Files, QDirIterator::Subdirectories);
    EXPECT_TRUE(packs.hasNext());
    EXPECT_TRUE(findByName(readPublication(outputDir),
                           QStringLiteral("Shared")).isEmpty());
    EXPECT_TRUE(QFileInfo::exists(QDir(outputDir).filePath(QStringLiteral("Common.bytes"))));
    for (const QString& language : project.projectLanguages) {
        const QString path = QDir(outputDir).filePath(language + QStringLiteral("/Mixed.bytes"));
        QByteArray bytes;
        harken::PackCanonicalImage image;
        harken::CurrentWorldBodyImage body;
        ASSERT_NO_FATAL_FAILURE(readCanonicalWorld(path, bytes, image, body));
        EXPECT_EQ(body.ProductionView()->actionCount, 1u);
        EXPECT_EQ(image.media.size(), 2u);
        const auto selected = readPublication(outputDir, language);
        const auto owner = findByName(selected, QStringLiteral("Mixed"));
        ASSERT_FALSE(owner.isEmpty());
        const auto catalog = actionCompanions(selected);
        ASSERT_EQ(catalog.size(), 2);
        EXPECT_EQ(QUuid(findByName(catalog, QStringLiteral("Mixed")).value(QStringLiteral("guid")).toString()),
                  VNX::authoringUuid(harkentest::ToStudio(body.ProductionView()->actions[0].actionStableId)));
        EXPECT_NE(findByName(selected, QStringLiteral("Common")).value(QStringLiteral("packStableId")),
                  owner.value(QStringLiteral("packStableId")));
    }
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, InvalidRequestedLanguageWritesNoPacks)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("Spoken"), 21, kSoloSoundObjectId}}));
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, soloSourceId));

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
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
        QDirIterator packs(outputDir, {"*.bytes"}, QDir::Files, QDirIterator::Subdirectories);
        EXPECT_FALSE(packs.hasNext());
        EXPECT_FALSE(QFileInfo::exists(
            QDir(outputDir).filePath(QStringLiteral("HarkenPackManifest.json"))));
    }
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, SelectedPostActionChainWithoutLanguagesWritesNoPacks)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
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
    VNX::VActionStep post;
    post.type = VNX::VActionStep::Type::PostAction;
    post.targetObjectID = 22;
    project.actions.front().rootItem->children.front()->comp<VNX::VAction>()->steps = {post};

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    opts.onlyUnits = {QStringLiteral("Root")};
    const QString outputDir = VNX::ProjectPackExporter::platformOutputDir(dir.path(), *VNX::ProjectPlatformSemantics::find(project.platforms, QUuid(opts.platform)));
    const auto result = exporter.ExportAllPacks(project, outputDir, opts);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("project has no languages")))
        << result.error.toStdString();
    EXPECT_TRUE(result.error.contains(VNX::objectIdString(harkentest::StudioObjectId(21 + kStableIdBias))))
        << result.error.toStdString();
    QDirIterator packs(outputDir, {"*.bytes"}, QDir::Files, QDirIterator::Subdirectories);
    EXPECT_FALSE(packs.hasNext());
    EXPECT_FALSE(QFileInfo::exists(
        QDir(outputDir).filePath(QStringLiteral("HarkenPackManifest.json"))));
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, InterruptedPublishRestoresOwnedFilesAndPreservesNeighbors)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& registry = VNX::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(dir.path());
    const auto source = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), source, source,
        {{QStringLiteral("One"), 21, kSoloSoundObjectId}}));
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    for (int mode : {0, 1, 2, 3, 4}) {
        const bool legacy = mode != 0;
        const bool missingNext = mode >= 3;
        const bool wrapped = mode == 2 || mode == 4;
        const QDir live(QDir(dir.path()).filePath(QString::number(mode)));
        const QString previousName = QStringLiteral(".harken-pack-previous-interrupted");
        const QString nextName = QStringLiteral(".harken-pack-next-interrupted");
        const QDir previous(live.filePath(previousName));
        const QDir next(live.filePath(nextName));
        opts.cancelCheck = {};
        ASSERT_TRUE(exporter.ExportAllPacks(project, previous.path(), opts).success);
        if (!missingNext)
            ASSERT_TRUE(exporter.ExportAllPacks(project, next.path(), opts).success);
        if (legacy) {
            ASSERT_NO_FATAL_FAILURE(renamePublicationToLegacy(previous, wrapped));
            if (!missingNext)
                ASSERT_NO_FATAL_FAILURE(renamePublicationToLegacy(next, wrapped));
        }
        QStringList names = previous.entryList(QDir::Files);
        std::map<QString, QByteArray> snapshot;
        for (const QString& name : names) {
            snapshot[name] = readFileBytes(previous.filePath(name));
            writeFileBytes(live.filePath(name), "partial");
        }
        const QList<QDir> locations = missingNext ? QList<QDir>{live, previous} : QList<QDir>{live, previous, next};
        for (const QDir& location : locations) {
            writeFileBytes(location.filePath("unrelated.bytes"), "unrelated");
            writeFileBytes(location.filePath("Init.bytes.meta"), "guid");
        }
        const QString marker = live.filePath(QStringLiteral(".harken-pack-publishing"));
        QJsonObject transaction{{"version", 1}, {"previous", previousName}, {"next", nextName},
            {"previousFiles", QJsonArray::fromStringList(names)}, {"nextFiles", QJsonArray::fromStringList(names)}};
        writeFileBytes(marker, QJsonDocument(transaction).toJson());
        opts.cancelCheck = [] { return true; };
        const auto result = exporter.ExportAllPacks(project, live.path(), opts);
        EXPECT_FALSE(result.success);
        EXPECT_EQ(result.error, QStringLiteral("Export canceled"));
        EXPECT_FALSE(QFileInfo::exists(marker));
        for (const auto& [name, bytes] : snapshot)
            EXPECT_EQ(readFileBytes(live.filePath(name)), bytes) << name.toStdString();
        for (const QDir& location : locations) {
            EXPECT_EQ(readFileBytes(location.filePath("unrelated.bytes")), "unrelated");
            EXPECT_EQ(readFileBytes(location.filePath("Init.bytes.meta")), "guid");
        }
        if (missingNext) {
            EXPECT_FALSE(next.exists());
            opts.cancelCheck = {};
            const auto retry = exporter.ExportAllPacks(project, live.path(), opts);
            ASSERT_TRUE(retry.success) << retry.error.toStdString();
        }
    }
    const QDir root(dir.filePath("root-recovery"));
    const QString previousName = QStringLiteral(".harken-pack-previous-interrupted");
    const QString nextName = QStringLiteral(".harken-pack-next-interrupted");
    const QDir previous(root.filePath(previousName));
    const QDir next(root.filePath(nextName));
    const QStringList previousNames{QStringLiteral("HarkenPackIndex.json"),
        QStringLiteral("OldPlatform/pack.archive"), QStringLiteral("OldPlatform/pack.archive.meta")};
    for (const auto& name : previousNames)
        writeFileBytes(previous.filePath(name), "previous");
    writeFileBytes(root.filePath("SelectedPlatform/current.bytes"), "unchanged");
    const QStringList nextNames{QStringLiteral("HarkenPackIndex.json")};
    for (const auto& name : nextNames) {
        writeFileBytes(root.filePath(name), "partial");
        writeFileBytes(next.filePath(name), "next");
    }
    const auto marker = root.filePath(".harken-pack-publishing");
    writeFileBytes(marker, QJsonDocument(QJsonObject{{"version", 1}, {"previous", previousName},
        {"next", nextName}, {"previousFiles", QJsonArray::fromStringList(previousNames)},
        {"nextFiles", QJsonArray::fromStringList(nextNames)}}).toJson());
    QString recoveryError;
    const auto outputLock = VNX::ProjectPackExporter::LockPackOutputRoot(root.path(), &recoveryError);
    ASSERT_NE(outputLock, nullptr) << recoveryError.toStdString();
    for (const auto& name : previousNames) EXPECT_EQ(readFileBytes(root.filePath(name)), "previous");
    EXPECT_EQ(readFileBytes(root.filePath("SelectedPlatform/current.bytes")), "unchanged");
    EXPECT_FALSE(QFileInfo::exists(marker));
    EXPECT_FALSE(previous.exists());
    EXPECT_FALSE(next.exists());
    registry.clear();
}

TEST(EditorProjectPackExporterManifest, CompletedPublicationCopiesAreRemovedWithoutInventories)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    auto& registry = VNX::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(dir.path());
    const auto source = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), source, source,
        {{QStringLiteral("One"), 21, kSoloSoundObjectId}}));
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const QDir live(QDir(dir.path()).filePath("live"));
    ASSERT_TRUE(exporter.ExportAllPacks(project, live.path(), opts).success);
    const auto companion = readFileBytes(live.filePath(QStringLiteral("Init.json")));
    for (const QString& prefix : {QStringLiteral(".harken-pack-previous-"), QStringLiteral(".harken-pack-next-")}) {
        for (int mode : {0, 1, 2}) {
            const QDir copy(live.filePath(prefix + QString::number(mode)));
            ASSERT_TRUE(exporter.ExportAllPacks(project, copy.path(), opts).success);
            if (mode != 0) ASSERT_NO_FATAL_FAILURE(renamePublicationToLegacy(copy, mode == 2));
            writeFileBytes(copy.filePath("unrelated.bytes"), "neighbor");
            writeFileBytes(copy.filePath("Init.bytes.meta"), "guid");
        }
    }
    const QDir unknown(live.filePath(".harken-pack-next-unknown"));
    writeFileBytes(unknown.filePath("unowned.bytes"), "unknown");
    const QDir delta(live.filePath(".harken-pack-previous-delta"));
    writeFileBytes(delta.filePath("One.bytes"), readFileBytes(live.filePath("One.bytes")));
    writeFileBytes(delta.filePath("unrelated.bytes"), "neighbor");
    writeFileBytes(delta.filePath("Init.bytes.meta"), "guid");
    opts.cancelCheck = [] { return true; };
    const auto canceled = exporter.ExportAllPacks(project, live.path(), opts);
    EXPECT_EQ(canceled.error, QStringLiteral("Export canceled"));
    EXPECT_EQ(readFileBytes(live.filePath(QStringLiteral("Init.json"))), companion);
    EXPECT_TRUE(live.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
    registry.clear();
}

TEST(EditorProjectPackExporterManifest, FailedInterruptedRecoveryKeepsSnapshotAndStopsPublish)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("One"), 21, kSoloSoundObjectId}}));
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    for (bool hasSnapshot : {false, true}) {
        SCOPED_TRACE(hasSnapshot);
        const QDir live(QDir(dir.path()).filePath(
            hasSnapshot ? QStringLiteral("blocked") : QStringLiteral("missing")));
        const QString previousDir = live.filePath(QStringLiteral(".harken-pack-previous-interrupted"));
        const QString previousPack = QDir(previousDir).filePath(QStringLiteral("English/action_3da91a64-b947-4560-0000-0000000186b5.bytes"));
        if (hasSnapshot) {
            ASSERT_NO_FATAL_FAILURE(writeFileBytes(previousPack, QByteArray("previous")));
            ASSERT_TRUE(QDir().mkpath(live.filePath(QStringLiteral("English/action_3da91a64-b947-4560-0000-0000000186b5.bytes"))));
        }
        const QString marker = live.filePath(QStringLiteral(".harken-pack-publishing"));
        ASSERT_NO_FATAL_FAILURE(writeFileBytes(marker, {}));
        const auto result = exporter.ExportAllPacks(project, live.absolutePath(), opts);
        EXPECT_FALSE(result.success);
        EXPECT_TRUE(result.error.startsWith(QStringLiteral("Cannot recover interrupted pack publish:")))
            << result.error.toStdString();
        EXPECT_TRUE(QFileInfo::exists(marker));
        EXPECT_FALSE(QFileInfo::exists(live.filePath(QStringLiteral("Init.bytes"))));
        if (hasSnapshot)
            EXPECT_EQ(readFileBytes(previousPack), QByteArray("previous"));
        const QStringList children = live.entryList(
            QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
        for (const QString& name : children) {
            EXPECT_FALSE(name.startsWith(QStringLiteral(".harken-pack-staging-")));
            EXPECT_FALSE(name.startsWith(QStringLiteral(".harken-pack-next-")));
            if (name.startsWith(QStringLiteral(".harken-pack-previous-")))
                EXPECT_EQ(name, QStringLiteral(".harken-pack-previous-interrupted"));
        }
    }
    VNX::AssetRegistry::instance().clear();
}

namespace {

void verifyIncrementalPublication(const QString& directory)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    const QDir dir(directory);
    auto& registry = VNX::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(dir.path());
    const auto tone = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.2f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), tone, tone,
        {{QStringLiteral("One"), 21, kSharedSoundObjectId},
         {QStringLiteral("Two"), 22, kSoloSoundObjectId}}));
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const QDir live(dir.filePath("exports"));
    const auto initial = exporter.ExportAllPacks(project, live.path(), opts);
    ASSERT_TRUE(initial.success) << initial.error.toStdString();
    EXPECT_EQ(initial.builtPackCount, 3);
    const auto timestamp = QDateTime::fromSecsSinceEpoch(1500000000);
    for (const auto& name : publicationFiles(live.path())) {
        QFile file(live.filePath(name));
        ASSERT_TRUE(file.open(QIODevice::ReadWrite));
        ASSERT_TRUE(file.setFileTime(timestamp, QFileDevice::FileModificationTime));
    }
    std::vector<VNX::ProjectPackExporter::PackBuildRecord> records;
    const auto unchanged = exporter.ExportAllPacks(project, live.path(), opts, &records);
    ASSERT_TRUE(unchanged.success) << unchanged.error.toStdString();
    EXPECT_EQ(unchanged.builtPackCount, 0);
    EXPECT_EQ(unchanged.skippedPackCount, 3);
    ASSERT_EQ(records.size(), 3u);
    for (const auto& record : records) {
        EXPECT_TRUE(record.skipped);
        EXPECT_EQ(QFileInfo(record.outputPath).lastModified(), timestamp);
    }
    for (const auto& name : publicationFiles(live.path()))
        EXPECT_EQ(QFileInfo(live.filePath(name)).lastModified(), timestamp);
    opts.selection.actionTargeting = {{harkentest::StudioObjectId(22 + kStableIdBias), false, {}}};
    const auto deselected = exporter.ExportAllPacks(project, live.path(), opts);
    ASSERT_TRUE(deselected.success) << deselected.error.toStdString();
    EXPECT_TRUE(findByName(readPublication(live.path(), {}, true, deselected.artifacts), "Two").isEmpty());
    EXPECT_EQ(QFileInfo(live.filePath("Two.bytes")).lastModified(), timestamp);
    EXPECT_EQ(QFileInfo(live.filePath("Two.json")).lastModified(), timestamp);
    ASSERT_TRUE(QDir(dir.filePath(".cache/pack-inputs")).removeRecursively());
    opts.selection.actionTargeting.clear();
    const auto reselected = exporter.ExportAllPacks(project, live.path(), opts);
    ASSERT_TRUE(reselected.success) << reselected.error.toStdString();
    EXPECT_EQ(reselected.builtPackCount, 0);
    EXPECT_EQ(reselected.skippedPackCount, 3);
    EXPECT_EQ(QFileInfo(live.filePath("Two.bytes")).lastModified(), timestamp);
    const auto oldOne = readFileBytes(live.filePath("One.bytes"));
    project.actions.front().rootItem->children.front()->comp<VNX::VAction>()->steps.front().delay = 0.1f;
    const auto changed = exporter.ExportAllPacks(project, live.path(), opts);
    ASSERT_TRUE(changed.success) << changed.error.toStdString();
    EXPECT_EQ(changed.builtPackCount, 1);
    EXPECT_EQ(changed.skippedPackCount, 2);
    EXPECT_NE(readFileBytes(live.filePath("One.bytes")), oldOne);
    EXPECT_EQ(QFileInfo(live.filePath("Two.bytes")).lastModified(), timestamp);
    const auto validTwo = readFileBytes(live.filePath("Two.bytes"));
    for (const auto& damaged : {QByteArray("invalid"), validTwo.chopped(1)}) {
        writeFileBytes(live.filePath("Two.bytes"), damaged);
        ASSERT_TRUE(QFile::remove(live.filePath("One.bytes")));
        const auto repaired = exporter.ExportAllPacks(project, live.path(), opts);
        ASSERT_TRUE(repaired.success) << repaired.error.toStdString();
        EXPECT_EQ(repaired.builtPackCount, 2);
        EXPECT_EQ(readFileBytes(live.filePath("Two.bytes")), validTwo);
    }
    const auto replacement = registerToneAsset(dir.path(), QStringLiteral("replacement.wav"), 0.7f);
    VNX::AudioSourceData source;
    source.sourceID = replacement;
    source.relativePath = QStringLiteral("replacement.wav");
    project.audioSources.push_back(source);
    HarkenTest::setSharedSoundSource(*project.layers.front().rootItem->children.front(), replacement);
    const auto beforeMediaChange = readFileBytes(live.filePath("One.bytes"));
    const auto mediaChanged = exporter.ExportAllPacks(project, live.path(), opts);
    ASSERT_TRUE(mediaChanged.success) << mediaChanged.error.toStdString();
    EXPECT_EQ(mediaChanged.builtPackCount, 1);
    EXPECT_EQ(mediaChanged.skippedPackCount, 2);
    EXPECT_NE(readFileBytes(live.filePath("One.bytes")), beforeMediaChange);
    EXPECT_EQ(readFileBytes(live.filePath("Two.bytes")), validTwo);
    for (const auto& name : publicationFiles(live.path())) {
        if (!name.endsWith(QStringLiteral(".bytes"))) continue;
        auto outdated = readFileBytes(live.filePath(name));
        ASSERT_GE(outdated.size(), harken::kPackV2HeaderBytes);
        harken::StoreLE32(reinterpret_cast<std::uint8_t*>(outdated.data()) + 4,
                          HARKEN_PACK_SCHEMA_VERSION + 1);
        writeFileBytes(live.filePath(name), outdated);
    }
    const auto rebuilt = exporter.ExportAllPacks(project, live.path(), opts);
    ASSERT_TRUE(rebuilt.success) << rebuilt.error.toStdString();
    EXPECT_EQ(rebuilt.builtPackCount, 3);
    EXPECT_EQ(rebuilt.skippedPackCount, 0);
    EXPECT_EQ(readFileBytes(live.filePath("Two.bytes")), validTwo);
    opts.forceReexport = true;
    const auto forced = exporter.ExportAllPacks(project, live.path(), opts);
    ASSERT_TRUE(forced.success) << forced.error.toStdString();
    EXPECT_EQ(forced.builtPackCount, 3);
    EXPECT_EQ(forced.skippedPackCount, 0);
    EXPECT_NE(QFileInfo(live.filePath("Init.bytes")).lastModified(), timestamp);
    registry.clear();
}

}

TEST(EditorProjectPackExporterManifest, IncrementalPublicationPreservesUnchangedFilesAndRepairsInvalidPacks)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_NO_FATAL_FAILURE(verifyIncrementalPublication(dir.path()));
}

TEST(EditorProjectPackExporterManifest, IncrementalPublicationActivatesRepairedPacks)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_NO_FATAL_FAILURE(verifyIncrementalPublication(dir.path()));
    harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
    ASSERT_EQ(session.InitResult(), VN_OK);
    ASSERT_NO_FATAL_FAILURE(activateExportedNames(dir.filePath("exports"), {QStringLiteral("One"), QStringLiteral("Two")}));
}

TEST(EditorProjectPackExporterManifest, PublicationSelectionReconcilesTheWholeOutputDirectory)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& registry = VNX::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(dir.path());
    const auto tone = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.2f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), tone, tone,
        {{QStringLiteral("Remove"), 21, kSharedSoundObjectId},
         {QStringLiteral("Project"), 22, kSharedSoundObjectId}}));
    project.packBuildOutputDir = dir.filePath("exports");
    project.projectLanguages = {QStringLiteral("English"), QStringLiteral("Japanese")};
    VNX::ProjectPackExporter exporter;
    const auto platform = HarkenTest::platformIdString("Windows");
    QVector<VNX::PackArtifactMetadata> artifacts;
    for (const auto& id : {platform, HarkenTest::platformIdString("Android")}) {
        const auto result = exporter.ExportAllPacks(project, {}, {.platform = id});
        ASSERT_TRUE(result.success) << result.error.toStdString();
        artifacts += result.artifacts;
    }
    ASSERT_TRUE(VNX::ProjectPackExporter::FinalizePackExport(project.packBuildOutputDir,
        project, {platform, HarkenTest::platformIdString("Android")}, false, artifacts).success);
    const QDir live(QDir(project.packBuildOutputDir).filePath("Windows"));
    const QDir other(QDir(project.packBuildOutputDir).filePath("Android"));
    const auto projectDataPath = QDir(project.packBuildOutputDir).filePath("Project.json");
    EXPECT_FALSE(QFileInfo::exists(projectDataPath));
    QMap<QString, QByteArray> originalPublication;
    for (const auto& file : publicationFiles(live.path()))
        originalPublication.insert(file, readFileBytes(live.filePath(file)));
    project.buildExportProjectData = true;
    const auto enabled = exporter.ExportAllPacks(project, {}, {.platform = platform});
    ASSERT_TRUE(enabled.success) << enabled.error.toStdString();
    const auto projectData = QJsonDocument::fromJson(readFileBytes(projectDataPath)).object();
    EXPECT_EQ(projectData.value("schemaVersion").toInt(), HARKEN_PACK_SCHEMA_VERSION);
    EXPECT_EQ(projectData.value("studioSchemaVersion").toInt(), VNX::ProjectSchema::kProjectSchemaVersion);
    EXPECT_EQ(projectData.value("studioVersion").toString(), QString::fromUtf8(
        Harken::BuildInfo::Version.data(), static_cast<int>(Harken::BuildInfo::Version.size())));
    EXPECT_EQ(projectData.value("projectGuid").toString(), project.projectId);
    const auto configuredPlatforms = projectData.value("platforms").toArray();
    ASSERT_EQ(configuredPlatforms.size(), 2);
    EXPECT_EQ(configuredPlatforms.at(1).toObject().value("guid").toString(),
        HarkenTest::platformIdString("Android"));
    EXPECT_EQ(projectData.value("languages").toArray(), QJsonArray::fromStringList(project.projectLanguages));
    for (auto file = originalPublication.cbegin(); file != originalPublication.cend(); ++file)
        EXPECT_EQ(readFileBytes(live.filePath(file.key())), file.value());
    const auto previousProjectDataPath = QDir(project.packBuildOutputDir).filePath("ProjectData.json");
    ASSERT_TRUE(QFile::rename(projectDataPath, previousProjectDataPath));
    VNX::ProjectPackExporter::ExportOptions partialOptions;
    partialOptions.platform = platform;
    partialOptions.updateSelectedOnly = true;
    partialOptions.onlyUnits = {QStringLiteral("Project")};
    partialOptions.language = QStringLiteral("English");
    project.name = QStringLiteral("updated-project");
    const auto partial = exporter.ExportAllPacks(project, {}, partialOptions);
    ASSERT_TRUE(partial.success) << partial.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(previousProjectDataPath));
    const auto partialData = QJsonDocument::fromJson(readFileBytes(projectDataPath)).object();
    EXPECT_EQ(partialData.value("projectName").toString(), project.name);
    EXPECT_EQ(partialData.value("platforms"), projectData.value("platforms"));
    EXPECT_EQ(partialData.value("languages"), projectData.value("languages"));
    const auto otherCompanion = readFileBytes(other.filePath(QStringLiteral("Init.json")));
    const auto keptBytes = readFileBytes(live.filePath("Project.bytes"));
    writeFileBytes(live.filePath("Remove.bytes.meta"), "guid");
    writeFileBytes(live.filePath("unrelated.bytes"), "neighbor");
    writeFileBytes(QDir(project.packBuildOutputDir).filePath("DeletedPlatform/old.no-extension"), "old");
    writeFileBytes(QDir(project.packBuildOutputDir).filePath("DeletedPlatform/.harken-pack-staging-stale/report.txt"), "old");
    writeFileBytes(QDir(project.packBuildOutputDir).filePath("notes.txt"), "unused");
    ASSERT_TRUE(QDir().mkpath(QDir(project.packBuildOutputDir).filePath("empty/nested")));
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.selection.actionTargeting = {{harkentest::StudioObjectId(21 + kStableIdBias), false, {}}};
    opts.platform = platform;
    const auto selected = exporter.ExportAllPacks(project, {}, opts);
    ASSERT_TRUE(selected.success) << selected.error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(live.filePath("Remove.bytes")));
    EXPECT_TRUE(QFileInfo::exists(live.filePath("Remove.json")));
    EXPECT_TRUE(findByName(readPublication(live.path(), {}, true, selected.artifacts), "Remove").isEmpty());
    const auto retained = VNX::ProjectPackExporter::FinalizePackExport(project.packBuildOutputDir,
        project, {platform}, false, selected.artifacts);
    ASSERT_TRUE(retained.success) << retained.error.toStdString();
    const auto indexPath = QDir(project.packBuildOutputDir).filePath(QStringLiteral("HarkenPackIndex.json"));
    EXPECT_FALSE(QFileInfo::exists(indexPath));
    EXPECT_EQ(readFileBytes(other.filePath(QStringLiteral("Init.json"))), otherCompanion);
    EXPECT_TRUE(QFileInfo::exists(QDir(project.packBuildOutputDir).filePath("notes.txt")));
    const auto companion = readFileBytes(live.filePath(QStringLiteral("Init.json")));
    const auto retainedProjectData = readFileBytes(projectDataPath);
    project.name = QStringLiteral("next-project-name");
    project.buildExportProjectData = false;
    opts.cancelCheck = [] { return true; };
    const auto canceled = exporter.ExportAllPacks(project, {}, opts);
    EXPECT_TRUE(canceled.canceled);
    EXPECT_TRUE(QFileInfo::exists(live.filePath("Remove.bytes")));
    EXPECT_EQ(readFileBytes(live.filePath(QStringLiteral("Init.json"))), companion);
    EXPECT_EQ(readFileBytes(projectDataPath), retainedProjectData);
    opts.cancelCheck = {};
    const auto keepPath = live.filePath("Project.bytes");
    ASSERT_TRUE(QFile::remove(keepPath));
    const auto invalid = VNX::ProjectPackExporter::FinalizePackExport(project.packBuildOutputDir,
        project, {platform}, true, selected.artifacts);
    EXPECT_FALSE(invalid.success);
    EXPECT_FALSE(QFileInfo::exists(indexPath));
    EXPECT_TRUE(QFileInfo::exists(live.filePath("Remove.bytes")));
    EXPECT_EQ(readFileBytes(other.filePath(QStringLiteral("Init.json"))), otherCompanion);
    EXPECT_EQ(readFileBytes(projectDataPath), retainedProjectData);
    writeFileBytes(keepPath, keptBytes);
    project.buildExportProjectData = true;
    const auto cleaned = VNX::ProjectPackExporter::FinalizePackExport(project.packBuildOutputDir,
        project, {platform}, true, selected.artifacts);
    ASSERT_TRUE(cleaned.success) << cleaned.error.toStdString();
    EXPECT_EQ(cleaned.removedPackCount, 4);
    EXPECT_FALSE(QFileInfo::exists(live.filePath("Remove.bytes")));
    EXPECT_FALSE(QFileInfo::exists(live.filePath("Remove.json")));
    EXPECT_EQ(readFileBytes(live.filePath("Project.bytes")), keptBytes);
    const auto cleanedProjectData = QJsonDocument::fromJson(readFileBytes(projectDataPath)).object();
    EXPECT_EQ(cleanedProjectData.value("projectName").toString(), project.name);
    EXPECT_EQ(cleanedProjectData.value("platforms"), projectData.value("platforms"));
    EXPECT_FALSE(QFileInfo::exists(live.filePath("Remove.bytes.meta")));
    EXPECT_FALSE(QFileInfo::exists(live.filePath("unrelated.bytes")));
    EXPECT_FALSE(other.exists());
    for (const auto& name : {"DeletedPlatform", "notes.txt", "empty"})
        EXPECT_FALSE(QFileInfo::exists(QDir(project.packBuildOutputDir).filePath(name))) << name;
    {
        harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
        ASSERT_EQ(session.InitResult(), VN_OK);
        ASSERT_NO_FATAL_FAILURE(activateExportedNames(live.path(), {QStringLiteral("Project")}));
    }
    const auto platformDefinition = std::find_if(project.platforms.begin(), project.platforms.end(),
        [&](const auto& value) { return value.id == QUuid(platform); });
    ASSERT_NE(platformDefinition, project.platforms.end());
    platformDefinition->name = QStringLiteral("Desktop");
    const auto renamedExport = exporter.ExportAllPacks(project, {}, opts);
    ASSERT_TRUE(renamedExport.success) << renamedExport.error.toStdString();
    const auto renamed = VNX::ProjectPackExporter::FinalizePackExport(project.packBuildOutputDir,
        project, {platform}, true, renamedExport.artifacts);
    ASSERT_TRUE(renamed.success) << renamed.error.toStdString();
    EXPECT_FALSE(live.exists());
    const QDir renamedLive(QDir(project.packBuildOutputDir).filePath("Desktop"));
    EXPECT_TRUE(QFileInfo::exists(renamedLive.filePath("Project.bytes")));
    EXPECT_EQ(QJsonDocument::fromJson(readFileBytes(projectDataPath)).object()
        .value("platforms").toArray().first().toObject().value("name").toString(), QStringLiteral("Desktop"));
    const auto renamedCompanion = readFileBytes(renamedLive.filePath("Project.json"));
    const auto metadataAlias = QDir(project.packBuildOutputDir).filePath("project.JSON");
    ASSERT_TRUE(QFile::rename(projectDataPath, projectDataPath + ".tmp"));
    ASSERT_TRUE(QFile::rename(projectDataPath + ".tmp", metadataAlias));
    project.buildExportProjectData = false;
    const auto disabled = exporter.ExportAllPacks(project, {}, opts);
    ASSERT_TRUE(disabled.success) << disabled.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(projectDataPath));
    EXPECT_FALSE(QFileInfo::exists(metadataAlias));
    EXPECT_EQ(readFileBytes(renamedLive.filePath("Project.json")), renamedCompanion);
    EXPECT_EQ(readFileBytes(renamedLive.filePath("Project.bytes")), keptBytes);
    opts.selection.actionTargeting.push_back({harkentest::StudioObjectId(22 + kStableIdBias), false, {}});
    project.buildExportProjectData = true;
    project.projectLanguages.clear();
    const auto emptyExport = exporter.ExportAllPacks(project, {}, opts);
    ASSERT_TRUE(emptyExport.success) << emptyExport.error.toStdString();
    const auto empty = VNX::ProjectPackExporter::FinalizePackExport(project.packBuildOutputDir,
        project, {platform}, true, emptyExport.artifacts);
    ASSERT_TRUE(empty.success) << empty.error.toStdString();
    EXPECT_EQ(empty.removedPackCount, 1);
    EXPECT_EQ(readPublication(renamedLive.path()).size(), 1);
    EXPECT_TRUE(QFileInfo::exists(renamedLive.filePath("Init.bytes")));
    EXPECT_TRUE(actionCompanions(readPublication(renamedLive.path())).isEmpty());
    EXPECT_TRUE(QJsonDocument::fromJson(readFileBytes(projectDataPath)).object()
        .value("languages").toArray().isEmpty());
    registry.clear();
}

TEST(EditorProjectPackExporterManifest, LanguageCleanupKeepsSharedInitAndSelectedPairs)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& registry = VNX::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(dir.path());
    const auto tone = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.2f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), tone, tone,
        {{QStringLiteral("Spoken"), 21, kSoloSoundObjectId}, {QStringLiteral("Common"), 22, kSharedSoundObjectId}}));
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, tone));
    const QDir live(dir.filePath("exports"));
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const auto exported = exporter.ExportAllPacks(project, live.path(), opts);
    ASSERT_TRUE(exported.success) << exported.error.toStdString();
    const auto init = readFileBytes(live.filePath("Init.bytes"));
    const auto spoken = harkentest::StudioObjectId(21 + kStableIdBias);
    opts.selection.actionTargeting = {{spoken, false, {}, QStringLiteral("English")}};
    opts.selfCleaning = true;
    const auto cleaned = exporter.ExportAllPacks(project, live.path(), opts);
    ASSERT_TRUE(cleaned.success) << cleaned.error.toStdString();
    EXPECT_EQ(cleaned.removedPackCount, 1);
    EXPECT_EQ(readFileBytes(live.filePath("Init.bytes")), init);
    EXPECT_EQ(findByName(readPublication(live.path()), QStringLiteral("Spoken")).value(QStringLiteral("language")).toString(), QStringLiteral("ja"));
    EXPECT_TRUE(findByName(readPublication(live.path(), QStringLiteral("English")), QStringLiteral("Spoken")).isEmpty());
    {
        harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
        ASSERT_EQ(session.InitResult(), VN_OK);
        ASSERT_NO_FATAL_FAILURE(activateExportedNames(live.path(), {QStringLiteral("Spoken"), QStringLiteral("Common")}, QStringLiteral("ja")));
    }
    opts.selection.actionTargeting.push_back({spoken, false, {}, QStringLiteral("ja")});
    const auto finalLanguage = exporter.ExportAllPacks(project, live.path(), opts);
    ASSERT_TRUE(finalLanguage.success) << finalLanguage.error.toStdString();
    EXPECT_EQ(finalLanguage.removedPackCount, 1);
    EXPECT_TRUE(findByName(readPublication(live.path()), QStringLiteral("Spoken")).isEmpty());
    EXPECT_EQ(readFileBytes(live.filePath("Init.bytes")), init);
    harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
    ASSERT_EQ(session.InitResult(), VN_OK);
    ASSERT_NO_FATAL_FAILURE(activateExportedNames(live.path(), {QStringLiteral("Common")}));
    registry.clear();
}

namespace {

void verifyPartialPublication(const QString& directory, void (*verifyActivation)(const QString&))
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    const QDir dir(directory);
    auto& registry = VNX::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(dir.path());
    const auto tone = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.2f);
    const auto privateTone = registerToneAsset(dir.path(), QStringLiteral("private.wav"), 0.4f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), tone, privateTone,
        {{QStringLiteral("One"), 21, kSharedSoundObjectId}, {QStringLiteral("Two"), 22, kSharedSoundObjectId}}));
    auto privateLayer = makeItem(QStringLiteral("PrivateLayer"), VNX::ProjectItem::Type::Layer, 60);
    privateLayer->fieldGroups = project.layers.front().rootItem->fieldGroups;
    auto privateSound = project.layers.front().rootItem->children.back();
    project.layers.front().rootItem->children.pop_back();
    addChild(privateLayer, privateSound);
    project.layers.push_back({privateLayer, QStringLiteral("Layers")});
    auto& actions = project.actions.front().rootItem->children;
    auto privatePlay = actions.back()->comp<VNX::VAction>()->steps.front();
    privatePlay.targetObjectID = kSoloSoundObjectId;
    actions.back()->comp<VNX::VAction>()->steps.push_back(privatePlay);
    auto variable = makeItem(QStringLiteral("Drive"), VNX::ProjectItem::Type::Variable, 50);
    auto* definition = variable->comp<VNX::VVariable>();
    definition->kind = VNX::VariableKind::Smooth;
    definition->rangeMin = 0.0f;
    definition->rangeMax = 1.0f;
    definition->defaultValue = 0.0f;
    project.variables.push_back({variable, QStringLiteral("Variables")});
    VNX::ParameterBindingDef binding;
    binding.guid = VNX::authoringUuid(harkentest::StudioObjectId(500));
    binding.variable = variable->guid;
    binding.address = VNX::BindingSemantics::volumeAddress();
    binding.curve = {{0.0f, -6.0f}, {1.0f, 0.0f}};
    binding.outputMin = -12.0f;
    binding.outputMax = 0.0f;
    auto* mix = project.layers.front().rootItem->comp<VNX::VMixNode>();
    ASSERT_TRUE(mix->bindings.upsert(binding));
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const QDir live(dir.filePath("exports"));
    std::vector<VNX::ProjectPackExporter::PackBuildRecord> initialRecords;
    const auto initial = exporter.ExportAllPacks(project, live.path(), opts, &initialRecords);
    ASSERT_TRUE(initial.success) << initial.error.toStdString();
    ASSERT_NO_FATAL_FAILURE(verifyActivation(live.path()));
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
    const auto oldTwoCompanion = readFileBytes(live.filePath("Two.json"));
    const auto oldTwoCompanionTime = QFileInfo(live.filePath("Two.json")).lastModified();
    const auto oldInitCompanion = readFileBytes(live.filePath("Init.json"));
    const auto oldInitCompanionTime = QFileInfo(live.filePath("Init.json")).lastModified();
    const auto oldTime = QFileInfo(live.filePath("Two.bytes")).lastModified();
    const auto oldInit = readFileBytes(live.filePath("Init.bytes"));
    const auto oldInitTime = QFileInfo(live.filePath("Init.bytes")).lastModified();
    actions.front()->comp<VNX::VAction>()->steps.front().delay = 0.1f;
    actions.back()->comp<VNX::VAction>()->steps.front().delay = 0.3f;
    actions.back()->name = QStringLiteral("EditedTwo");
    opts.selection.actionTargeting = {{actions.back()->runtimeId(), false, {}}};
    opts.updateSelectedOnly = true;
    opts.onlyPackPaths = {QStringLiteral("One")};
    opts.selfCleaning = true;
    for (const bool force : {false, true}) {
        opts.forceReexport = force;
        std::vector<VNX::ProjectPackExporter::PackBuildRecord> records;
        const auto partial = exporter.ExportAllPacks(project, live.path(), opts, &records);
        ASSERT_TRUE(partial.success) << partial.error.toStdString();
        for (const auto& warning : partial.warnings)
            EXPECT_TRUE(initial.warnings.contains(warning)) << warning.toStdString();
        EXPECT_EQ(partial.builtPackCount, 1);
        EXPECT_EQ(partial.removedPackCount, 0);
        EXPECT_EQ(readFileBytes(live.filePath("Two.bytes")), oldTwo);
        EXPECT_EQ(readFileBytes(live.filePath("Two.json")), oldTwoCompanion);
        EXPECT_EQ(QFileInfo(live.filePath("Two.json")).lastModified(), oldTwoCompanionTime);
        EXPECT_EQ(readFileBytes(live.filePath("Init.json")), oldInitCompanion);
        EXPECT_EQ(QFileInfo(live.filePath("Init.json")).lastModified(), oldInitCompanionTime);
        EXPECT_EQ(QFileInfo(live.filePath("Two.bytes")).lastModified(), oldTime);
        EXPECT_EQ(readFileBytes(live.filePath("Init.bytes")), oldInit);
        const auto manifest = readPublication(live.path(), {}, true, partial.artifacts);
        QSet<QString> publishedPaths;
        for (const auto& value : manifest)
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
        EXPECT_TRUE(findByName(manifest, "Two").isEmpty());
        const auto inventory = readPublication(live.path(), {}, false);
        EXPECT_EQ(findByName(inventory, "Two"), findByName(old, "Two"));
        EXPECT_EQ(findByName(actionCompanions(inventory), "Two"),
            findByName(actionCompanions(old), "Two"));
        ASSERT_NO_FATAL_FAILURE(verifyActivation(live.path()));
    }
    const auto one = readFileBytes(live.filePath("One.bytes"));
    binding.curve.front().value = -12.0f;
    ASSERT_TRUE(mix->bindings.upsert(binding));
    auto* master = project.busses.front().rootItem.get();
    master->comp<VNX::VMixNode>()->volume = -6.0f;
    master->playablePropertyPresence.Set(harkenplayableproperties::PlayablePropertyIdV1::Volume);
    const auto sharedChange = exporter.ExportAllPacks(project, live.path(), opts);
    ASSERT_TRUE(sharedChange.success) << sharedChange.error.toStdString();
    for (const auto& warning : sharedChange.warnings)
        EXPECT_TRUE(initial.warnings.contains(warning)) << warning.toStdString();
    EXPECT_EQ(sharedChange.builtPackCount, 1);
    EXPECT_EQ(sharedChange.removedPackCount, 0);
    EXPECT_NE(readFileBytes(live.filePath("One.bytes")), one);
    EXPECT_EQ(readFileBytes(live.filePath("Two.bytes")), oldTwo);
    EXPECT_EQ(readFileBytes(live.filePath("Two.json")), oldTwoCompanion);
    EXPECT_EQ(QFileInfo(live.filePath("Two.json")).lastModified(), oldTwoCompanionTime);
    EXPECT_EQ(readFileBytes(live.filePath("Init.json")), oldInitCompanion);
    EXPECT_EQ(QFileInfo(live.filePath("Init.json")).lastModified(), oldInitCompanionTime);
    EXPECT_EQ(QFileInfo(live.filePath("Two.bytes")).lastModified(), oldTime);
    EXPECT_EQ(readFileBytes(live.filePath("Init.bytes")), oldInit);
    EXPECT_EQ(QFileInfo(live.filePath("Init.bytes")).lastModified(), oldInitTime);
    const auto manifest = readPublication(live.path(), {}, true, sharedChange.artifacts);
    EXPECT_EQ(findByName(manifest, "Init"), findByName(old, "Init"));
    EXPECT_TRUE(findByName(manifest, "Two").isEmpty());
    const auto inventory = readPublication(live.path(), {}, false);
    EXPECT_EQ(findByName(inventory, "Two"), findByName(old, "Two"));
    EXPECT_EQ(findByName(actionCompanions(inventory), "Two"),
        findByName(actionCompanions(old), "Two"));
    actions.back()->comp<VNX::VAction>()->steps.front().targetObjectID = kSoloSoundObjectId;
    ASSERT_TRUE(QFile::remove(registry.getAsset(privateTone)->absolutePath));
    const auto unfinishedDraft = exporter.ExportAllPacks(project, live.path(), opts);
    ASSERT_TRUE(unfinishedDraft.success) << unfinishedDraft.error.toStdString();
    for (const auto& warning : unfinishedDraft.warnings)
        EXPECT_TRUE(initial.warnings.contains(warning)) << warning.toStdString();
    EXPECT_EQ(unfinishedDraft.builtPackCount, 1);
    EXPECT_EQ(readFileBytes(live.filePath("Two.bytes")), oldTwo);
    EXPECT_EQ(readFileBytes(live.filePath("Two.json")), oldTwoCompanion);
    EXPECT_EQ(QFileInfo(live.filePath("Two.json")).lastModified(), oldTwoCompanionTime);
    EXPECT_EQ(readFileBytes(live.filePath("Init.json")), oldInitCompanion);
    EXPECT_EQ(QFileInfo(live.filePath("Init.json")).lastModified(), oldInitCompanionTime);
    EXPECT_EQ(QFileInfo(live.filePath("Two.bytes")).lastModified(), oldTime);
    EXPECT_EQ(readFileBytes(live.filePath("Init.bytes")), oldInit);
    registry.clear();
}

}

TEST(EditorProjectPackExporterManifest, PartialPublicationRetainsOtherPackPairs)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_NO_FATAL_FAILURE(verifyPartialPublication(dir.path(), [](const QString&) {}));
}

TEST(EditorProjectPackExporterManifest, PartialPublicationActivatesSelectedUpdates)
{
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    ASSERT_NO_FATAL_FAILURE(verifyPartialPublication(dir.path(), [](const QString& outputDir) {
        harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
        ASSERT_EQ(session.InitResult(), VN_OK);
        ASSERT_NO_FATAL_FAILURE(activateExportedNames(outputDir, {QStringLiteral("One"), QStringLiteral("Two")}));
    }));
}

TEST(EditorProjectPackExporterManifest, CapacityGrowthPreservesUnselectedPublishedContent)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& registry = VNX::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(dir.path());
    const auto shared = registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 0.2f);
    const auto privateSource = registerToneAsset(dir.path(), QStringLiteral("private.wav"), 0.4f);
    std::vector<ActionSpec> specs;
    for (std::uint32_t i = 0; i < 64; ++i)
        specs.push_back({QStringLiteral("Action%1").arg(i), 21 + i,
                        i == 0 ? kSoloSoundObjectId : kSharedSoundObjectId});
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), shared, privateSource, specs));
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    const QDir output(QDir(dir.path()).filePath(QStringLiteral("exports")));
    auto initial = exporter.ExportAllPacks(project, output.path(), options);
    ASSERT_TRUE(initial.success) << initial.error.toStdString();
    QByteArray oldBytes;
    harken::PackCanonicalImage oldImage;
    harken::CurrentWorldBodyImage oldBody;
    const auto retainedPath = output.filePath(QStringLiteral("Action0.bytes"));
    ASSERT_NO_FATAL_FAILURE(readCanonicalWorld(retainedPath, oldBytes, oldImage, oldBody));
    ASSERT_EQ(1u, oldImage.media.size());
    const auto oldCompanion = readFileBytes(output.filePath(QStringLiteral("Action0.json")));
    auto& actions = project.actions.front().rootItem->children;
    actions.front()->comp<VNX::VAction>()->steps.front().type = VNX::VActionStep::Type::Stop;
    const auto* privateAsset = registry.getAsset(privateSource);
    ASSERT_NE(nullptr, privateAsset);
    ASSERT_TRUE(QFile::remove(privateAsset->absolutePath));
    auto added = makeItem(QStringLiteral("Action64"), VNX::ProjectItem::Type::Action, 85);
    auto play = actions.back()->comp<VNX::VAction>()->steps.front();
    added->comp<VNX::VAction>()->steps.push_back(play);
    addChild(project.actions.front().rootItem, added);
    options.updateSelectedOnly = true;
    options.onlyActionStableIds = {added->runtimeId()};
    std::map<QString, QByteArray> before;
    for (const auto& path : publicationFiles(output.path()))
        before.emplace(path, readFileBytes(output.filePath(path)));
    const auto permissions = QFile::permissions(retainedPath);
    const auto restore = qScopeGuard([&] { QFile::setPermissions(retainedPath, permissions); });
    ASSERT_TRUE(QFile::setPermissions(retainedPath, permissions & ~(
        QFileDevice::WriteOwner | QFileDevice::WriteUser |
        QFileDevice::WriteGroup | QFileDevice::WriteOther)));
    const auto blocked = exporter.ExportAllPacks(project, output.path(), options);
    EXPECT_FALSE(blocked.success);
    for (const auto& [path, bytes] : before)
        EXPECT_EQ(bytes, readFileBytes(output.filePath(path))) << path.toStdString();
    EXPECT_FALSE(QFileInfo::exists(output.filePath(QStringLiteral("Action64.bytes"))));
    ASSERT_TRUE(QFile::setPermissions(retainedPath, permissions));
    const auto grown = exporter.ExportAllPacks(project, output.path(), options);
    ASSERT_TRUE(grown.success) << grown.error.toStdString();
    EXPECT_EQ(oldCompanion, readFileBytes(output.filePath(QStringLiteral("Action0.json"))));
    QByteArray newBytes;
    harken::PackCanonicalImage newImage;
    harken::CurrentWorldBodyImage newBody;
    ASSERT_NO_FATAL_FAILURE(readCanonicalWorld(retainedPath, newBytes, newImage, newBody));
    ASSERT_EQ(1u, newImage.media.size());
    EXPECT_EQ(oldImage.requirement.ownedResourceCount, newImage.requirement.ownedResourceCount);
    EXPECT_EQ(oldImage.requirement.ownedDefinitionCount, newImage.requirement.ownedDefinitionCount);
    EXPECT_EQ(oldBody.ProductionView()->actions[0].programSemanticFingerprint,
              newBody.ProductionView()->actions[0].programSemanticFingerprint);
    const auto& oldMedia = oldImage.media[0];
    const auto& newMedia = newImage.media[0];
    EXPECT_EQ(oldBytes.mid(static_cast<qsizetype>(oldMedia.blobOffset), static_cast<qsizetype>(oldMedia.blobBytes)),
              newBytes.mid(static_cast<qsizetype>(newMedia.blobOffset), static_cast<qsizetype>(newMedia.blobBytes)));
    harken::WorldCapacitySummaryV1 common = newBody.ProductionView()->capacity;
    EXPECT_GE(common.maxActionDefinitions, 65u);
    QStringList names;
    std::map<QString, QByteArray> stable;
    for (const auto& value : readPublication(output.path())) {
        const auto row = value.toObject();
        const auto path = row.value(QStringLiteral("path")).toString();
        QByteArray bytes;
        harken::PackCanonicalImage image;
        harken::CurrentWorldBodyImage body;
        ASSERT_NO_FATAL_FAILURE(readCanonicalWorld(output.filePath(path), bytes, image, body));
        EXPECT_EQ(0, std::memcmp(&common, &body.ProductionView()->capacity, sizeof(common)));
        stable.emplace(path, bytes);
        const auto name = row.value(QStringLiteral("name")).toString();
        if (name != QStringLiteral("Init")) names.append(name);
    }
    {
        harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_NO_FATAL_FAILURE(activateExportedNames(output.path(), names));
    }
    const auto unchanged = exporter.ExportAllPacks(project, output.path(), options);
    ASSERT_TRUE(unchanged.success) << unchanged.error.toStdString();
    for (const auto& [path, bytes] : stable)
        EXPECT_EQ(bytes, readFileBytes(output.filePath(path))) << path.toStdString();
    registry.clear();
}

TEST(EditorProjectPackExporterManifest, PartialPublicationCombinesScheduleAndEffectCapacity)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& registry = VNX::AssetRegistry::instance();
    registry.clear();
    const auto cleanup = qScopeGuard([&] { registry.clear(); });
    registry.setProjectRoot(dir.path());
    const auto shared = registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 0.2f);
    const auto privateSource = registerToneAsset(dir.path(), QStringLiteral("private.wav"), 0.4f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), shared, privateSource,
        {{QStringLiteral("Retained"), 21, 1000}}));
    auto retained = makeItem(QStringLiteral("Alternatives"), VNX::ProjectItem::Type::Container, 1000);
    addChild(project.layers.front().rootItem, retained);
    std::uint32_t nextObjectId = 2000;
    const auto appendSegment = [&](const std::shared_ptr<VNX::ProjectItem>& parent, std::uint32_t source) {
        auto segment = makeItem(QStringLiteral("Segment%1").arg(nextObjectId),
            VNX::ProjectItem::Type::Segment, nextObjectId++);
        addChild(parent, segment);
        for (unsigned i = 0; i < 4; ++i) {
            segment->comp<VNX::VSegment>()->markers.items.push_back({QUuid::createUuid(),
                VNX::MarkerKind::Custom, QStringLiteral("Marker%1").arg(i), 100 + i * 100});
            auto track = makeItem(QStringLiteral("Track%1").arg(nextObjectId),
                VNX::ProjectItem::Type::Track, nextObjectId++);
            addChild(segment, track);
            auto sound = makeItem(QStringLiteral("Sound%1").arg(nextObjectId),
                VNX::ProjectItem::Type::Sound, nextObjectId++);
            HarkenTest::setSharedSoundSource(*sound, source);
            addChild(track, sound);
            VNX::Clip clip;
            clip.clipGuid = QUuid::createUuid();
            clip.targetGuid = sound->guid;
            clip.duration = 1000000;
            auto* timeline = track->comp<VNX::VTrack>();
            EXPECT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
            timeline->clips.items.push_back(clip);
            std::vector<VNX::EffectEntry> effects(3);
            for (auto& effect : effects) {
                effect.guid = QUuid::createUuid();
                effect.type = VNX::EffectType::Compressor;
                effect.parameters = VNX::EffectChainSemantics::defaultParameters(effect.type);
            }
            EXPECT_TRUE(track->comp<VNX::VMixNode>()->effects.replace(std::move(effects)));
        }
        return segment;
    };
    for (std::uint32_t i = 0; i < harken::kSegmentScheduleProgramCapacity; ++i) {
        const auto segment = appendSegment(retained, privateSource);
        ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::addContainerChildOccurrence(*retained, segment->guid));
    }
    project.nextObjectID = nextObjectId;
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    const QDir output(QDir(dir.path()).filePath(QStringLiteral("exports")));
    const auto initial = exporter.ExportAllPacks(project, output.path(), options);
    ASSERT_TRUE(initial.success) << initial.error.toStdString();
    QByteArray oldBytes;
    harken::PackCanonicalImage oldImage;
    harken::CurrentWorldBodyImage oldBody;
    ASSERT_NO_FATAL_FAILURE(readCanonicalWorld(output.filePath(QStringLiteral("Retained.bytes")),
        oldBytes, oldImage, oldBody));
    const auto oldCompanion = readFileBytes(output.filePath(QStringLiteral("Retained.json")));
    const auto& oldWorld = *oldBody.ProductionView();
    QByteArray oldInitBytes;
    harken::PackCanonicalImage oldInitImage;
    harken::CurrentWorldBodyImage oldInitBody;
    ASSERT_NO_FATAL_FAILURE(readCanonicalWorld(output.filePath(QStringLiteral("Init.bytes")),
        oldInitBytes, oldInitImage, oldInitBody));
    ASSERT_EQ(harken::kSegmentScheduleProgramCapacity, oldWorld.segmentScheduleCount);
    const auto counts = [](const harken::ContainerWorldInstallViewV1& world) {
        std::array<std::uint64_t, 8> totals{world.trackScheduleCount, world.segmentScheduleCount};
        for (std::uint32_t i = 0; i < world.trackScheduleCount; ++i)
            totals[3] += world.trackSchedules[i].placementCount;
        for (std::uint32_t i = 0; i < world.segmentScheduleCount; ++i) {
            totals[2] += world.segmentSchedules[i].trackRefCount;
            totals[4] += world.segmentSchedules[i].markerCount;
        }
        const auto effects = [&](const harken::PlayablePropertyPayloadViewV1& payload) {
            totals[5] += payload.effectNodeCount;
            totals[7] += payload.effectEdgeCount;
            for (std::uint32_t i = 0; i < payload.effectNodeCount; ++i)
                totals[6] += payload.effectNodes[i].paramCount;
        };
        for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i) {
            const auto& property = world.playableProperties[i];
            effects(property.payload);
            for (std::uint32_t j = 0; j < property.structuralAncestorCount; ++j)
                effects(property.structuralAncestors[j].payload);
        }
        for (std::uint32_t i = 0; i < world.playableOccurrenceCount; ++i)
            effects(world.playableOccurrences[i].payload);
        return totals;
    };
    const auto capacities = [](const harken::WorldCapacitySummaryV1& capacity) {
        return std::array<std::uint64_t, 8>{capacity.maxTrackSchedulePrograms,
            capacity.maxSegmentSchedulePrograms, capacity.maxScheduleTrackRefsTotal,
            capacity.maxSchedulePlacementsTotal, capacity.maxScheduleMarkersTotal,
            capacity.maxPlayablePropertyEffectNodesTotal,
            capacity.maxPlayablePropertyEffectParamsTotal,
            capacity.maxPlayablePropertyEffectEdgesTotal};
    };
    const auto oldCounts = counts(oldWorld);
    auto& actions = project.actions.front().rootItem->children;
    actions.front()->comp<VNX::VAction>()->steps.front().type = VNX::VActionStep::Type::Stop;
    retained->children.front()->comp<VNX::VSegment>()->markers.items.front().position = 50;
    auto* draftTrack = retained->children.front()->children.front().get();
    draftTrack->comp<VNX::VTrack>()->clips.items.front().duration = 2000000;
    ASSERT_TRUE(draftTrack->comp<VNX::VMixNode>()->effects.replace({}));
    const auto* privateAsset = registry.getAsset(privateSource);
    ASSERT_NE(nullptr, privateAsset);
    ASSERT_TRUE(QFile::remove(privateAsset->absolutePath));
    auto addedContainer = makeItem(QStringLiteral("AddedAlternatives"),
        VNX::ProjectItem::Type::Container, nextObjectId++);
    addChild(project.layers.front().rootItem, addedContainer);
    const auto addedSegment = appendSegment(addedContainer, shared);
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::addContainerChildOccurrence(*addedContainer, addedSegment->guid));
    auto added = makeItem(QStringLiteral("Added"), VNX::ProjectItem::Type::Action, nextObjectId++);
    VNX::VActionStep play;
    play.type = VNX::VActionStep::Type::Play;
    play.targetObjectID = addedContainer->objectID;
    added->comp<VNX::VAction>()->steps.push_back(play);
    addChild(project.actions.front().rootItem, added);
    project.nextObjectID = nextObjectId;
    options.updateSelectedOnly = true;
    options.onlyActionStableIds = {added->runtimeId()};
    const auto partial = exporter.ExportAllPacks(project, output.path(), options);
    ASSERT_TRUE(partial.success) << partial.error.toStdString();
    EXPECT_EQ(oldCompanion, readFileBytes(output.filePath(QStringLiteral("Retained.json"))));
    QByteArray retainedBytes, addedBytes;
    harken::PackCanonicalImage retainedImage, addedImage;
    harken::CurrentWorldBodyImage retainedBody, addedBody;
    ASSERT_NO_FATAL_FAILURE(readCanonicalWorld(output.filePath(QStringLiteral("Retained.bytes")),
        retainedBytes, retainedImage, retainedBody));
    ASSERT_NO_FATAL_FAILURE(readCanonicalWorld(output.filePath(QStringLiteral("Added.bytes")),
        addedBytes, addedImage, addedBody));
    const auto& retainedWorld = *retainedBody.ProductionView();
    EXPECT_EQ(oldCounts, counts(retainedWorld));
    ASSERT_EQ(oldWorld.playablePropertyCount, retainedWorld.playablePropertyCount);
    ASSERT_EQ(oldWorld.trackScheduleCount, retainedWorld.trackScheduleCount);
    ASSERT_EQ(oldWorld.segmentScheduleCount, retainedWorld.segmentScheduleCount);
    ASSERT_EQ(oldWorld.containerCount, retainedWorld.containerCount);
    ASSERT_EQ(1u, addedBody.ProductionView()->containerCount);
    for (std::uint32_t i = 0; i < oldWorld.playablePropertyCount; ++i) {
        EXPECT_EQ(oldWorld.playableProperties[i].authoredStableOrder,
                  retainedWorld.playableProperties[i].authoredStableOrder);
        EXPECT_EQ(oldWorld.playableProperties[i].programSemanticFingerprint,
                  retainedWorld.playableProperties[i].programSemanticFingerprint);
    }
    for (std::uint32_t i = 0; i < oldWorld.containerCount; ++i) {
        EXPECT_EQ(oldWorld.containers[i].authoredStableOrder,
                  retainedWorld.containers[i].authoredStableOrder);
        EXPECT_EQ(oldWorld.containers[i].programSemanticFingerprint,
                  retainedWorld.containers[i].programSemanticFingerprint);
    }
    for (std::uint32_t i = 0; i < oldWorld.trackScheduleCount; ++i)
        EXPECT_EQ(oldWorld.trackSchedules[i].scheduleSemanticFingerprint,
                  retainedWorld.trackSchedules[i].scheduleSemanticFingerprint);
    for (std::uint32_t i = 0; i < oldWorld.segmentScheduleCount; ++i)
        EXPECT_EQ(oldWorld.segmentSchedules[i].scheduleSemanticFingerprint,
                  retainedWorld.segmentSchedules[i].scheduleSemanticFingerprint);
    EXPECT_EQ(oldWorld.actions[0].programSemanticFingerprint,
              retainedWorld.actions[0].programSemanticFingerprint);
    const auto addedCounts = counts(*addedBody.ProductionView());
    const auto defaults = capacities(harken::WorldCapacitySummaryV1{});
    const auto grown = capacities(retainedWorld.capacity);
    for (std::size_t i = 0; i < grown.size(); ++i) {
        EXPECT_GT(oldCounts[i] + addedCounts[i], defaults[i]) << i;
        EXPECT_GE(grown[i], oldCounts[i] + addedCounts[i]) << i;
    }
    auto expected = oldWorld.capacity;
    expected.maxTrackSchedulePrograms = retainedWorld.capacity.maxTrackSchedulePrograms;
    expected.maxSegmentSchedulePrograms = retainedWorld.capacity.maxSegmentSchedulePrograms;
    expected.maxScheduleTrackRefsTotal = retainedWorld.capacity.maxScheduleTrackRefsTotal;
    expected.maxSchedulePlacementsTotal = retainedWorld.capacity.maxSchedulePlacementsTotal;
    expected.maxScheduleMarkersTotal = retainedWorld.capacity.maxScheduleMarkersTotal;
    expected.maxPlayablePropertyEffectNodesTotal = retainedWorld.capacity.maxPlayablePropertyEffectNodesTotal;
    expected.maxPlayablePropertyEffectParamsTotal = retainedWorld.capacity.maxPlayablePropertyEffectParamsTotal;
    expected.maxPlayablePropertyEffectEdgesTotal = retainedWorld.capacity.maxPlayablePropertyEffectEdgesTotal;
    expected.maxPlayablePropertyPrograms = retainedWorld.capacity.maxPlayablePropertyPrograms;
    expected.maxPlayableOccurrencePropertyRows = retainedWorld.capacity.maxPlayableOccurrencePropertyRows;
    expected.maxPlayablePropertyLayerAncestorRefsTotal = retainedWorld.capacity.maxPlayablePropertyLayerAncestorRefsTotal;
    EXPECT_EQ(0, std::memcmp(&expected, &retainedWorld.capacity, sizeof(expected)));
    for (const auto& name : {QStringLiteral("Init"), QStringLiteral("Added")}) {
        QByteArray bytes;
        harken::PackCanonicalImage image;
        harken::CurrentWorldBodyImage body;
        ASSERT_NO_FATAL_FAILURE(readCanonicalWorld(output.filePath(name + QStringLiteral(".bytes")),
            bytes, image, body));
        EXPECT_EQ(0, std::memcmp(&retainedWorld.capacity, &body.ProductionView()->capacity,
            sizeof(retainedWorld.capacity)));
        if (name == QStringLiteral("Init")) {
            const auto& oldInit = *oldInitBody.ProductionView();
            const auto& newInit = *body.ProductionView();
            ASSERT_NE(0u, oldInit.playablePropertyCount);
            ASSERT_EQ(oldInit.playablePropertyCount, newInit.playablePropertyCount);
            for (std::uint32_t i = 0; i < oldInit.playablePropertyCount; ++i) {
                EXPECT_EQ(oldInit.playableProperties[i].stableId, newInit.playableProperties[i].stableId);
                EXPECT_EQ(oldInit.playableProperties[i].authoredStableOrder,
                          newInit.playableProperties[i].authoredStableOrder);
                EXPECT_EQ(oldInit.playableProperties[i].programSemanticFingerprint,
                          newInit.playableProperties[i].programSemanticFingerprint);
            }
        }
    }
    harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_NO_FATAL_FAILURE(activateExportedNames(output.path(),
        {QStringLiteral("Retained"), QStringLiteral("Added")}));
}

TEST(EditorProjectPackExporterManifest, LanguageVariantsShareCapacityWithoutAddingAlternativeDefinitions)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& registry = VNX::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(dir.path());
    const auto tone = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.2f);
    std::vector<ActionSpec> specs;
    for (std::uint32_t i = 0; i < 65; ++i)
        specs.push_back({QStringLiteral("Localized%1").arg(i), 21 + i, kSoloSoundObjectId});
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), tone, tone, specs));
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, tone));
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    options.selection.actionTargeting = {{project.actions.front().rootItem->children.back()->runtimeId(),
                                         false, {}, QStringLiteral("ja")}};
    const QDir output(QDir(dir.path()).filePath(QStringLiteral("exports")));
    const auto exported = exporter.ExportAllPacks(project, output.path(), options);
    ASSERT_TRUE(exported.success) << exported.error.toStdString();
    harken::WorldCapacitySummaryV1 common;
    bool initialized = false;
    for (const auto& value : readPublication(output.path())) {
        const auto row = value.toObject();
        QByteArray bytes;
        harken::PackCanonicalImage image;
        harken::CurrentWorldBodyImage body;
        ASSERT_NO_FATAL_FAILURE(readCanonicalWorld(output.filePath(
            row.value(QStringLiteral("path")).toString()), bytes, image, body));
        const auto& capacity = body.ProductionView()->capacity;
        if (!initialized) {
            common = capacity;
            initialized = true;
        }
        EXPECT_EQ(0, std::memcmp(&common, &capacity, sizeof(common)));
        EXPECT_GE(capacity.maxActionDefinitions, 65u);
        EXPECT_LT(capacity.maxActionDefinitions, 129u);
    }
    for (const auto& language : project.projectLanguages) {
        QStringList names;
        for (const auto& value : readPublication(output.path(), language)) {
            const auto name = value.toObject().value(QStringLiteral("name")).toString();
            if (name != QStringLiteral("Init")) names.append(name);
        }
        EXPECT_EQ(language == QStringLiteral("English") ? 65 : 64, names.size());
        harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
        ASSERT_EQ(VN_OK, session.InitResult());
        ASSERT_NO_FATAL_FAILURE(activateExportedNames(output.path(), names, language));
    }
    registry.clear();
}

TEST(EditorProjectPackExporterManifest, PartialPublicationPreservesOrdersAcrossLanguageAlternatives)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    for (bool conflictingEmbeddedOrder : {false, true}) {
        SCOPED_TRACE(conflictingEmbeddedOrder);
        QTemporaryDir dir;
        ASSERT_TRUE(dir.isValid());
        auto& registry = VNX::AssetRegistry::instance();
        registry.clear();
        const auto cleanup = qScopeGuard([&] { registry.clear(); });
        registry.setProjectRoot(dir.path());
        const auto tone = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.2f);
        auto project = HarkenTest::projectWithPlatforms({"Windows"});
        ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), tone, tone,
            {{QStringLiteral("EnglishVoice"), 21, kSharedSoundObjectId},
             {QStringLiteral("JapaneseVoice"), 22, kSoloSoundObjectId}}));
        ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, tone));
        const auto layer = project.layers.front().rootItem;
        const auto localizedSources = layer->children.at(1)->comp<VNX::VSound>()->sources;
        layer->children.front()->comp<VNX::VSound>()->distinguishLanguage = true;
        layer->children.front()->comp<VNX::VSound>()->sources = localizedSources;
        auto embedded = makeItem(QStringLiteral("Embedded"), VNX::ProjectItem::Type::Sound, 5);
        HarkenTest::setSharedSoundSource(*embedded, tone);
        addChild(layer, embedded);
        VNX::VActionStep embeddedPlay;
        embeddedPlay.type = VNX::VActionStep::Type::Play;
        embeddedPlay.targetObjectID = embedded->objectID;
        for (auto& action : project.actions.front().rootItem->children)
            action->comp<VNX::VAction>()->steps.push_back(embeddedPlay);
        if (conflictingEmbeddedOrder) {
            auto extra = makeItem(QStringLiteral("JapaneseExtra"), VNX::ProjectItem::Type::Sound, 3);
            HarkenTest::setSharedSoundSource(*extra, tone);
            addChild(layer, extra);
            auto play = embeddedPlay;
            play.targetObjectID = extra->objectID;
            project.actions.front().rootItem->children[1]->comp<VNX::VAction>()->steps.push_back(play);
        }
        VNX::ProjectPackExporter exporter;
        VNX::ProjectPackExporter::ExportOptions options;
        options.platform = HarkenTest::platformIdString("Windows");
        options.selection.actionTargeting = {
            {project.actions.front().rootItem->children[0]->runtimeId(), false, {}, QStringLiteral("ja")},
            {project.actions.front().rootItem->children[1]->runtimeId(), false, {}, QStringLiteral("English")}};
        const QDir output(QDir(dir.path()).filePath(QStringLiteral("exports")));
        const auto initial = exporter.ExportAllPacks(project, output.path(), options);
        ASSERT_TRUE(initial.success) << initial.error.toStdString();
        std::map<QString, QByteArray> companions;
        std::map<QString, std::map<VnObjectId, std::pair<std::uint32_t, std::uint64_t>>> retainedProperties;
        std::vector<std::uint32_t> embeddedOrders;
        for (const auto& value : readPublication(output.path())) {
            const auto pack = value.toObject();
            if (pack.value(QStringLiteral("name")).toString() == QStringLiteral("Init")) continue;
            const auto path = pack.value(QStringLiteral("path")).toString();
            companions.emplace(path.chopped(6) + QStringLiteral(".json"),
                readFileBytes(output.filePath(path.chopped(6) + QStringLiteral(".json"))));
            QByteArray bytes;
            harken::PackCanonicalImage image;
            harken::CurrentWorldBodyImage body;
            ASSERT_NO_FATAL_FAILURE(readCanonicalWorld(output.filePath(path), bytes, image, body));
            const auto& world = *body.ProductionView();
            auto& properties = retainedProperties[path];
            for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i) {
                const auto& property = world.playableProperties[i];
                properties.emplace(property.stableId,
                    std::pair(property.authoredStableOrder, property.programSemanticFingerprint));
                if (property.stableId == embedded->runtimeId()) embeddedOrders.push_back(property.authoredStableOrder);
            }
        }
        ASSERT_EQ(2u, companions.size());
        ASSERT_EQ(2u, embeddedOrders.size());
        EXPECT_EQ(conflictingEmbeddedOrder, embeddedOrders[0] != embeddedOrders[1]);
        for (bool localized : {false, true}) {
            const auto name = localized ? QStringLiteral("AddedVoice") : QStringLiteral("Neutral");
            const std::uint32_t objectId = localized ? 60 : 40;
            auto container = makeItem(name + QStringLiteral("Container"), VNX::ProjectItem::Type::Container, objectId);
            auto sound = makeItem(name + QStringLiteral("Sound"), VNX::ProjectItem::Type::Sound, objectId + 1);
            HarkenTest::setSharedSoundSource(*sound, tone);
            if (localized) {
                sound->comp<VNX::VSound>()->distinguishLanguage = true;
                sound->comp<VNX::VSound>()->sources = localizedSources;
            }
            addChild(container, sound);
            addChild(layer, container);
            ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::addContainerChildOccurrence(*container, sound->guid));
            auto action = makeItem(name, VNX::ProjectItem::Type::Action, objectId + 2);
            VNX::VActionStep play;
            play.type = VNX::VActionStep::Type::Play;
            play.targetObjectID = objectId;
            action->comp<VNX::VAction>()->steps.push_back(play);
            if (!localized) action->comp<VNX::VAction>()->steps.push_back(embeddedPlay);
            addChild(project.actions.front().rootItem, action);
            options.updateSelectedOnly = true;
            options.onlyActionStableIds = {action->runtimeId()};
            std::map<QString, QByteArray> before;
            if (conflictingEmbeddedOrder)
                for (const auto& path : publicationFiles(output.path()))
                    before.emplace(path, readFileBytes(output.filePath(path)));
            const auto partial = exporter.ExportAllPacks(project, output.path(), options);
            if (conflictingEmbeddedOrder) {
                EXPECT_FALSE(partial.success);
                EXPECT_EQ(before.size(), publicationFiles(output.path()).size());
                for (const auto& [path, bytes] : before)
                    EXPECT_EQ(bytes, readFileBytes(output.filePath(path))) << path.toStdString();
                EXPECT_FALSE(QFileInfo::exists(output.filePath(QStringLiteral("Neutral.bytes"))));
                break;
            }
            ASSERT_TRUE(partial.success) << partial.error.toStdString();
            for (const auto& [path, bytes] : companions)
                EXPECT_EQ(bytes, readFileBytes(output.filePath(path))) << path.toStdString();
            for (const auto& [path, properties] : retainedProperties) {
                QByteArray bytes;
                harken::PackCanonicalImage image;
                harken::CurrentWorldBodyImage body;
                ASSERT_NO_FATAL_FAILURE(readCanonicalWorld(output.filePath(path), bytes, image, body));
                const auto& world = *body.ProductionView();
                ASSERT_EQ(properties.size(), world.playablePropertyCount);
                for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i) {
                    const auto& property = world.playableProperties[i];
                    const auto found = properties.find(property.stableId);
                    ASSERT_NE(properties.end(), found);
                    EXPECT_EQ(found->second.first, property.authoredStableOrder);
                    EXPECT_EQ(found->second.second, property.programSemanticFingerprint);
                }
            }
            for (const auto& language : project.projectLanguages) {
                QStringList names{language == QStringLiteral("English")
                    ? QStringLiteral("EnglishVoice") : QStringLiteral("JapaneseVoice"), QStringLiteral("Neutral")};
                if (localized) names.append(QStringLiteral("AddedVoice"));
                harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
                ASSERT_EQ(VN_OK, session.InitResult());
                ASSERT_NO_FATAL_FAILURE(activateExportedNames(output.path(), names, language));
            }
        }
    }
}

TEST(EditorProjectPackExporterManifest, PartialPublicationRefreshesEmbeddedClosureAndKeepsOtherPairs)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& registry = VNX::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(dir.path());
    const auto tone = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.2f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), tone, tone,
        {{QStringLiteral("Provider"), 21, kSharedSoundObjectId}, {QStringLiteral("Consumer"), 22, kSharedSoundObjectId}}));
    VNX::VActionStep post;
    post.type = VNX::VActionStep::Type::PostAction;
    post.targetObjectID = 21;
    auto& actions = project.actions.front().rootItem->children;
    actions.back()->comp<VNX::VAction>()->steps = {post};
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const QDir live(dir.filePath("exports"));
    const auto initial = exporter.ExportAllPacks(project, live.path(), opts);
    ASSERT_TRUE(initial.success) << initial.error.toStdString();
    const auto manifest = readPublication(live.path());
    const auto provider = readFileBytes(live.filePath("Provider.bytes"));
    const auto providerCompanion = readFileBytes(live.filePath("Provider.json"));
    const auto providerTime = QFileInfo(live.filePath("Provider.bytes")).lastModified();
    const auto consumer = readFileBytes(live.filePath("Consumer.bytes"));
    const auto init = readFileBytes(live.filePath("Init.bytes"));
    actions.front()->comp<VNX::VAction>()->steps.front().delay = 0.1f;
    opts.onlyPackPaths = {QStringLiteral("Consumer")};
    opts.updateSelectedOnly = true;
    const auto partial = exporter.ExportAllPacks(project, live.path(), opts);
    ASSERT_TRUE(partial.success) << partial.error.toStdString();
    for (const auto& warning : partial.warnings)
        EXPECT_TRUE(initial.warnings.contains(warning)) << warning.toStdString();
    EXPECT_EQ(partial.builtPackCount, 1);
    EXPECT_EQ(readFileBytes(live.filePath("Provider.bytes")), provider);
    EXPECT_EQ(readFileBytes(live.filePath("Provider.json")), providerCompanion);
    EXPECT_EQ(QFileInfo(live.filePath("Provider.bytes")).lastModified(), providerTime);
    EXPECT_NE(readFileBytes(live.filePath("Consumer.bytes")), consumer);
    EXPECT_EQ(readFileBytes(live.filePath("Init.bytes")), init);
    EXPECT_EQ(findByName(readPublication(live.path()), "Provider"),
        findByName(manifest, "Provider"));
    actions.front()->comp<VNX::VAction>()->steps.front().delay = 0.0f;
    actions.back()->comp<VNX::VAction>()->steps = actions.front()->comp<VNX::VAction>()->steps;
    opts.selection.actionTargeting = {{actions.front()->runtimeId(), false, {}}};
    opts.updateSelectedOnly = false;
    opts.onlyPackPaths.clear();
    const QDir selectedLive(dir.filePath("selected-exports"));
    const auto selected = exporter.ExportAllPacks(project, selectedLive.path(), opts);
    ASSERT_TRUE(selected.success) << selected.error.toStdString();
    const auto selectedConsumer = readFileBytes(selectedLive.filePath("Consumer.bytes"));
    const auto selectedInit = readFileBytes(selectedLive.filePath("Init.bytes"));
    ASSERT_FALSE(QFileInfo::exists(selectedLive.filePath("Provider.bytes")));
    actions.back()->comp<VNX::VAction>()->steps = {post};
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

TEST(EditorProjectPackExporterManifest, WriteProtectionPreflightRespectsExportScopeAndPreservesOutputs)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    QTemporaryDir output;
    ASSERT_TRUE(dir.isValid());
    ASSERT_TRUE(output.isValid());
    auto& registry = VNX::AssetRegistry::instance();
    registry.clear();
    const auto clearRegistry = qScopeGuard([&] { registry.clear(); });
    registry.setProjectRoot(dir.path());
    const auto tone = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.2f);
    VNX::ProjectData project;
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), tone, tone,
        {{QStringLiteral("One"), 21, kSharedSoundObjectId},
         {QStringLiteral("Two"), 22, kSoloSoundObjectId}}));
    project.packBuildOutputDir = output.path();
    project.buildExportProjectData = true;
    const QString windows = HarkenTest::platformIdString("Windows");
    const QString android = HarkenTest::platformIdString("Android");
    VNX::ProjectPackExporter exporter;
    for (const auto& platform : {windows, android}) {
        const auto initial = exporter.ExportAllPacks(project, {}, {.platform = platform});
        ASSERT_TRUE(initial.success) << initial.error.toStdString();
    }
    writeFileBytes(output.filePath("ProjectData.json"), "{}");
    writeFileBytes(output.filePath("notes.txt"), "keep");
    std::map<QString, QByteArray> before;
    QDirIterator files(output.path(), QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const auto path = files.next();
        before.emplace(path, readFileBytes(path));
    }
    const auto readOnly = [](QFileDevice::Permissions permissions) {
        return permissions & ~(QFileDevice::WriteOwner | QFileDevice::WriteUser
                               | QFileDevice::WriteGroup | QFileDevice::WriteOther);
    };
    for (const QString& relative : {QStringLiteral("Windows/Init.bytes"),
                                    QStringLiteral("Windows/One.bytes"),
                                    QStringLiteral("Android/Two.bytes"),
                                    QStringLiteral("Project.json"),
                                    QStringLiteral("ProjectData.json"),
                                    QStringLiteral("notes.txt")}) {
        SCOPED_TRACE(relative.toStdString());
        const QString path = output.filePath(relative);
        const auto permissions = QFile::permissions(path);
        const auto restore = qScopeGuard([&] { QFile::setPermissions(path, permissions); });
        ASSERT_TRUE(QFile::setPermissions(path, readOnly(permissions)));
        VNX::ProjectPackExporter::ExportOptions options;
        options.forceReexport = true;
        options.selfCleaning = relative == QStringLiteral("notes.txt");
        QString error;
        EXPECT_FALSE(VNX::ProjectPackExporter::ValidatePackBuildWriteAccess(
            project, output.path(), {windows, android}, options, &error));
        EXPECT_TRUE(error.contains(QDir::toNativeSeparators(path))) << error.toStdString();
        if (!relative.startsWith("Android/") && !options.selfCleaning) {
            options.platform = windows;
            options.currentWorldOutputPath = dir.filePath("blocked.world");
            const auto rejected = exporter.ExportAllPacks(project, {}, options);
            EXPECT_FALSE(rejected.success);
            EXPECT_TRUE(rejected.error.contains(QDir::toNativeSeparators(path)));
            EXPECT_FALSE(QFileInfo::exists(options.currentWorldOutputPath));
        }
        if (relative.startsWith("Android/") || relative == QStringLiteral("notes.txt")) {
            options.selfCleaning = false;
            EXPECT_TRUE(VNX::ProjectPackExporter::ValidatePackBuildWriteAccess(
                project, output.path(), {windows}, options, &error)) << error.toStdString();
        }
        for (const auto& [file, bytes] : before)
            EXPECT_EQ(readFileBytes(file), bytes) << file.toStdString();
        EXPECT_EQ(QFile::permissions(path), readOnly(permissions));
    }
    std::map<QString, QFileDevice::Permissions> protectedPaths;
    const auto restore = qScopeGuard([&] {
        for (const auto& [path, permissions] : protectedPaths) QFile::setPermissions(path, permissions);
    });
    for (const QString& relative : {QStringLiteral("Windows/Init.bytes"),
                                    QStringLiteral("Windows/Init.json"),
                                    QStringLiteral("Windows/Two.bytes"),
                                    QStringLiteral("Windows/Two.json"),
                                    QStringLiteral("Windows/One.json"),
                                    QStringLiteral("Android/Two.bytes"), QStringLiteral("notes.txt")}) {
        const QString path = output.filePath(relative);
        protectedPaths.emplace(path, QFile::permissions(path));
        ASSERT_TRUE(QFile::setPermissions(path, readOnly(protectedPaths.at(path))));
    }
    VNX::ProjectPackExporter::ExportOptions selected;
    selected.platform = windows;
    selected.updateSelectedOnly = true;
    selected.onlyPackPaths = {QStringLiteral("One")};
    selected.forceReexport = true;
    const auto result = exporter.ExportAllPacks(project, {}, selected);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    selected.updateSelectedOnly = false;
    selected.selfCleaning = true;
    selected.forceReexport = false;
    selected.onlyPackPaths.clear();
    const auto platformCleanup = exporter.ExportAllPacks(project, {}, selected);
    ASSERT_TRUE(platformCleanup.success) << platformCleanup.error.toStdString();
    for (const auto& [path, permissions] : protectedPaths) {
        EXPECT_EQ(readFileBytes(path), before.at(path));
        EXPECT_EQ(QFile::permissions(path), readOnly(permissions));
    }
}

TEST(EditorProjectPackExporterManifest, WriteProtectionChangedDuringBuildPreservesPublication)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& registry = VNX::AssetRegistry::instance();
    registry.clear();
    const auto clearRegistry = qScopeGuard([&] { registry.clear(); });
    registry.setProjectRoot(dir.path());
    const auto tone = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.2f);
    VNX::ProjectData project;
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), tone, tone,
        {{QStringLiteral("One"), 21, kSharedSoundObjectId}}));
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    const QDir output(dir.filePath("out"));
    const auto initial = exporter.ExportAllPacks(project, output.path(), options);
    ASSERT_TRUE(initial.success) << initial.error.toStdString();
    std::map<QString, QByteArray> before;
    for (const auto& name : publicationFiles(output.path()))
        before.emplace(name, readFileBytes(output.filePath(name)));
    const QString path = output.filePath("One.bytes");
    const auto permissions = QFile::permissions(path);
    const auto restore = qScopeGuard([&] { QFile::setPermissions(path, permissions); });
    bool protectedDuringBuild = false;
    options.forceReexport = true;
    options.progressCallback = [&](const VNX::PackGenerationProgress& progress) {
        if (progress.stage != VNX::PackGenerationStage::Publishing || protectedDuringBuild) return;
        protectedDuringBuild = true;
        EXPECT_TRUE(QFile::setPermissions(path, permissions
            & ~(QFileDevice::WriteOwner | QFileDevice::WriteUser
                | QFileDevice::WriteGroup | QFileDevice::WriteOther)));
    };
    const auto result = exporter.ExportAllPacks(project, output.path(), options);
    EXPECT_TRUE(protectedDuringBuild);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QDir::toNativeSeparators(path))) << result.error.toStdString();
    for (const auto& [name, bytes] : before)
        EXPECT_EQ(readFileBytes(output.filePath(name)), bytes) << name.toStdString();
    EXPECT_FALSE(QFileInfo::exists(output.filePath(".harken-pack-publishing")));
}

namespace {
struct PackWriteAccessProject
{
    QTemporaryDir directory;
    VNX::ProjectData project;
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions options;
    QString root;
    QString platform;
    QDir live;

    void setup(bool waveforms = false)
    {
        ASSERT_TRUE(directory.isValid());
        qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
        auto& registry = VNX::AssetRegistry::instance();
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
    ~PackWriteAccessProject() { VNX::AssetRegistry::instance().clear(); }
};

QFileDevice::Permissions withoutWritePermissions(QFileDevice::Permissions permissions)
{
    return permissions & ~(QFileDevice::WriteOwner | QFileDevice::WriteUser
                           | QFileDevice::WriteGroup | QFileDevice::WriteOther);
}
}

TEST(EditorProjectPackExporterManifest, ReadOnlyUnchangedPackDoesNotNeedReplacement)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup());
    const auto first = context.exporter.ExportAllPacks(context.project, {}, context.options);
    ASSERT_TRUE(first.success) << first.error.toStdString();
    const auto unchanged = context.exporter.ExportAllPacks(context.project, {}, context.options);
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
        const auto readonly = context.exporter.ExportAllPacks(context.project, {}, context.options);
        EXPECT_TRUE(readonly.success) << readonly.error.toStdString();
        EXPECT_EQ(readonly.builtPackCount, 0);
    }
    context.project.buildExportPackMetadata = true;
    const auto changed = context.exporter.ExportAllPacks(context.project, {}, context.options);
    EXPECT_FALSE(changed.success);
    EXPECT_TRUE(changed.error.contains(QStringLiteral(".json"))) << changed.error.toStdString();
    context.project.buildExportPackMetadata = false;
    context.project.actions.front().rootItem->children.front()->comp<VNX::VAction>()->steps.front().delay = 0.1f;
    const auto changedPack = context.exporter.ExportAllPacks(context.project, {}, context.options);
    EXPECT_FALSE(changedPack.success);
    EXPECT_TRUE(changedPack.error.contains(QStringLiteral("One.bytes"))) << changedPack.error.toStdString();
    for (const auto& [path, bytes] : before) {
        EXPECT_EQ(readFileBytes(path), bytes);
        EXPECT_EQ(QFile::permissions(path), withoutWritePermissions(permissions.at(path)));
    }
    EXPECT_FALSE(QFileInfo::exists(context.live.filePath(".harken-pack-publishing")));
}

TEST(EditorProjectPackExporterManifest, CleanupMustAllowRetainedReadOnlyWaveform)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup(true));
    const auto first = context.exporter.ExportAllPacks(context.project, {}, context.options);
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
    const auto preflight = VNX::ProjectPackExporter::ValidatePackBuildWriteAccess(
        context.project, context.root, {context.platform}, context.options, &error);
    const auto publication = VNX::ProjectPackExporter::FinalizePackExport(
        context.root, context.project, {context.platform}, true, first.artifacts);
    ASSERT_TRUE(publication.success) << publication.error.toStdString();
    ASSERT_TRUE(QFileInfo::exists(path));
    EXPECT_TRUE(preflight) << error.toStdString();
    const auto exported = context.exporter.ExportAllPacks(context.project, {}, context.options);
    ASSERT_TRUE(exported.success) << exported.error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(path));
    const QString obsolete = waveforms.filePath(QString(64, QLatin1Char('f')) + QStringLiteral(".vkd"));
    writeFileBytes(obsolete, "obsolete");
    const auto obsoletePermissions = QFile::permissions(obsolete);
    const auto restoreObsolete = qScopeGuard([&] { QFile::setPermissions(obsolete, obsoletePermissions); });
    ASSERT_TRUE(QFile::setPermissions(obsolete, withoutWritePermissions(obsoletePermissions)));
    const auto blockedCleanup = VNX::ProjectPackExporter::FinalizePackExport(
        context.root, context.project, {context.platform}, true, exported.artifacts);
    EXPECT_FALSE(blockedCleanup.success);
    EXPECT_TRUE(blockedCleanup.error.contains(QDir::toNativeSeparators(obsolete)))
        << blockedCleanup.error.toStdString();
    EXPECT_TRUE(QFileInfo::exists(path));
    EXPECT_EQ(readFileBytes(obsolete), QByteArray("obsolete"));
    EXPECT_FALSE(QFileInfo::exists(QDir(context.root).filePath(".harken-pack-publishing")));
}

TEST(EditorProjectPackExporterManifest, OptionalWaveformFailuresKeepPackPublicationValid)
{
    for (const bool conflictingFile : {false, true}) {
        PackWriteAccessProject context;
        ASSERT_NO_FATAL_FAILURE(context.setup(true));
        const auto first = context.exporter.ExportAllPacks(context.project, {}, context.options);
        ASSERT_TRUE(first.success) << first.error.toStdString();
        const QDir waveforms(context.project.buildWaveformOutputDir);
        const auto names = waveforms.entryList({"*.vkd"}, QDir::Files);
        ASSERT_FALSE(names.isEmpty());
        const QString blocked = conflictingFile ? waveforms.filePath(names.front())
            : context.directory.filePath(QStringLiteral("blocked-waveforms"));
        writeFileBytes(blocked, "occupied");
        if (!conflictingFile) context.options.waveformOutputDir = blocked;
        QString error;
        EXPECT_TRUE(VNX::ProjectPackExporter::ValidatePackBuildWriteAccess(
            context.project, context.root, {context.platform}, context.options, &error))
            << error.toStdString();
        std::vector<VNX::ProjectPackExporter::PackBuildRecord> records;
        context.project.actions.front().rootItem->children.front()->comp<VNX::VAction>()->steps.front().delay = 0.2f;
        const auto result = context.exporter.ExportAllPacks(context.project, {}, context.options, &records);
        ASSERT_TRUE(result.success) << result.error.toStdString();
        EXPECT_FALSE(result.canceled);
        EXPECT_FALSE(result.warnings.isEmpty());
        EXPECT_EQ(readFileBytes(blocked), QByteArray("occupied"));
        ASSERT_FALSE(records.empty());
        EXPECT_TRUE(std::all_of(records.begin(), records.end(), [](const auto& record) { return record.success; }));
        EXPECT_TRUE(std::any_of(records.begin(), records.end(), [](const auto& record) { return !record.warnings.isEmpty(); }));
        const auto publication = readPublication(context.live.path());
        ASSERT_EQ(publication.size(), 2);
        const auto action = findByName(publication, QStringLiteral("One"));
        EXPECT_FALSE(action.value(QStringLiteral("companion")).toObject().contains(QStringLiteral("waveforms")));
        const auto finalized = VNX::ProjectPackExporter::FinalizePackExport(
            context.root, context.project, {context.platform}, false, result.artifacts);
        EXPECT_TRUE(finalized.success) << finalized.error.toStdString();
        std::map<QString, QFileDevice::Permissions> permissions;
        const auto restore = qScopeGuard([&] {
            for (const auto& [path, value] : permissions) QFile::setPermissions(path, value);
        });
        for (const auto& name : publicationFiles(context.live.path())) {
            if (!name.endsWith(QStringLiteral(".json"))) continue;
            const QString path = context.live.filePath(name);
            permissions.emplace(path, QFile::permissions(path));
            ASSERT_TRUE(QFile::setPermissions(path, withoutWritePermissions(permissions.at(path))));
        }
        EXPECT_TRUE(VNX::ProjectPackExporter::ValidatePackBuildWriteAccess(
            context.project, context.root, {context.platform}, context.options, &error)) << error.toStdString();
        const auto unchanged = context.exporter.ExportAllPacks(context.project, {}, context.options);
        EXPECT_TRUE(unchanged.success) << unchanged.error.toStdString();
        EXPECT_FALSE(unchanged.warnings.isEmpty());
    }
}

TEST(EditorProjectPackExporterManifest, ProtectedLegacyWaveformLocationsCannotWriteOrCleanProjectData)
{
    QTemporaryDir directory;
    ASSERT_TRUE(directory.isValid());
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    const QString projectRoot = directory.filePath(QStringLiteral("Project"));
    ASSERT_TRUE(QDir().mkpath(projectRoot));
    auto& registry = VNX::AssetRegistry::instance();
    registry.clear();
    const auto clearRegistry = qScopeGuard([&] { registry.clear(); });
    registry.setProjectRoot(projectRoot);
    const auto tone = registerToneAsset(projectRoot, QStringLiteral("tone.wav"), 0.2f);
    VNX::ProjectData project;
    ASSERT_NO_FATAL_FAILURE(buildProject(project, projectRoot, tone, tone,
        {{QStringLiteral("One"), 21, kSharedSoundObjectId}}));
    const QString sourcePath = QDir(projectRoot).filePath(QStringLiteral("AudioSources/tone.wav"));
    const QByteArray sourceBytes = readFileBytes(sourcePath);
    const QString outputRoot = directory.filePath(QStringLiteral("Published"));
    const QString platform = HarkenTest::platformIdString("Windows");
    const QDir live(QDir(outputRoot).filePath(QStringLiteral("Windows")));
    project.packBuildOutputDir = outputRoot;
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions options;
    options.platform = platform;
    options.forceReexport = true;
    options.selfCleaning = true;
    for (const QString& destination : {QStringLiteral("AudioSources/Waveforms"), QStringLiteral("..")}) {
        project.buildWaveformOutputDir = destination;
        const QDir protectedDirectory(VNX::PathUtils::waveformBuildOutputRoot(project.projectPath, destination));
        ASSERT_TRUE(QDir().mkpath(protectedDirectory.path()));
        const QString sentinel = protectedDirectory.filePath(QString(64, QLatin1Char('f')) + QStringLiteral(".vkd"));
        writeFileBytes(sentinel, "preserved project data");
        const auto before = protectedDirectory.entryList({QStringLiteral("*.vkd")}, QDir::Files);
        for (const bool enabled : {false, true}) {
            project.buildExportWaveforms = enabled;
            const auto result = exporter.ExportAllPacks(project, {}, options);
            ASSERT_TRUE(result.success) << result.error.toStdString();
            if (enabled) EXPECT_FALSE(result.warnings.isEmpty());
            const auto publication = readPublication(live.path());
            ASSERT_EQ(publication.size(), 2);
            for (const auto& value : publication)
                EXPECT_FALSE(value.toObject().value(QStringLiteral("companion")).toObject()
                    .contains(QStringLiteral("waveforms")));
            const auto finalized = VNX::ProjectPackExporter::FinalizePackExport(
                outputRoot, project, {platform}, true, result.artifacts);
            EXPECT_TRUE(finalized.success) << finalized.error.toStdString();
            EXPECT_EQ(readFileBytes(sourcePath), sourceBytes);
            EXPECT_EQ(readFileBytes(sentinel), QByteArray("preserved project data"));
            EXPECT_EQ(protectedDirectory.entryList({QStringLiteral("*.vkd")}, QDir::Files), before);
            EXPECT_FALSE(QFileInfo::exists(protectedDirectory.filePath(QStringLiteral(".harken-pack-waveforms.lock"))));
        }
    }
}

TEST(EditorProjectPackExporterManifest, ConcurrentWaveformPublicationRemainsOptional)
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
        EXPECT_TRUE(VNX::ProjectPackExporter::ValidatePackBuildWriteAccess(
            context.project, context.root, {context.platform}, context.options, &error)) << error.toStdString();
        const auto result = context.exporter.ExportAllPacks(context.project, {}, context.options);
        ASSERT_TRUE(result.success) << result.error.toStdString();
        EXPECT_FALSE(result.warnings.isEmpty());
        EXPECT_EQ(waveforms.entryList({QStringLiteral("*.vkd")}, QDir::Files).size(), 1);
        EXPECT_EQ(readFileBytes(otherWaveform), QByteArray("other build waveform"));
        for (const auto& value : readPublication(context.live.path()))
            EXPECT_FALSE(value.toObject().value(QStringLiteral("companion")).toObject()
                .contains(QStringLiteral("waveforms")));
        const auto finalized = VNX::ProjectPackExporter::FinalizePackExport(
            context.root, context.project, {context.platform}, true, result.artifacts);
        EXPECT_TRUE(finalized.success) << finalized.error.toStdString();
        if (location != 0) EXPECT_FALSE(finalized.warnings.isEmpty());
        EXPECT_EQ(readFileBytes(otherWaveform), QByteArray("other build waveform"));
        EXPECT_TRUE(QFileInfo::exists(lockPath));
        context.project.buildExportWaveforms = false;
        const auto disabled = context.exporter.ExportAllPacks(context.project, {}, context.options);
        ASSERT_TRUE(disabled.success) << disabled.error.toStdString();
        const auto cleaned = VNX::ProjectPackExporter::FinalizePackExport(
            context.root, context.project, {context.platform}, true, disabled.artifacts);
        EXPECT_TRUE(cleaned.success) << cleaned.error.toStdString();
        if (location != 0) EXPECT_FALSE(cleaned.warnings.isEmpty());
        EXPECT_EQ(readFileBytes(otherWaveform), QByteArray("other build waveform"));
        competingBuild.unlock();
        context.project.buildExportWaveforms = true;
        const auto retry = context.exporter.ExportAllPacks(context.project, {}, context.options);
        ASSERT_TRUE(retry.success) << retry.error.toStdString();
        const auto companion = findByName(readPublication(context.live.path()), QStringLiteral("One"))
            .value(QStringLiteral("companion")).toObject();
        ASSERT_FALSE(companion.value(QStringLiteral("waveforms")).toArray().isEmpty());
        for (const auto& value : companion.value(QStringLiteral("waveforms")).toArray())
            EXPECT_TRUE(QFileInfo::exists(context.live.absoluteFilePath(
                value.toObject().value(QStringLiteral("file")).toString())));
        EXPECT_FALSE(QFileInfo::exists(lockPath));
        const auto cleanup = VNX::ProjectPackExporter::FinalizePackExport(
            context.root, context.project, {context.platform}, true, retry.artifacts);
        EXPECT_TRUE(cleanup.success) << cleanup.error.toStdString();
        EXPECT_EQ(QFileInfo::exists(otherWaveform), location == 0);
    }
}

TEST(EditorProjectPackExporterManifest, InterruptedPublicationRemovesOnlyNewWaveforms)
{
    for (const bool external : {false, true}) {
        for (const bool cancel : {false, true}) {
            PackWriteAccessProject context;
            ASSERT_NO_FATAL_FAILURE(context.setup(true));
            context.project.buildWaveformOutputDir = external
                ? context.directory.filePath(QStringLiteral("external-waveforms"))
                : context.live.filePath(QStringLiteral("Waveforms"));
            const auto first = context.exporter.ExportAllPacks(context.project, {}, context.options);
            ASSERT_TRUE(first.success) << first.error.toStdString();
            const QDir waveforms(context.project.buildWaveformOutputDir);
            const auto names = waveforms.entryList({"*.vkd"}, QDir::Files);
            ASSERT_FALSE(names.isEmpty());
            QMap<QString, QByteArray> before;
            for (const auto& name : publicationFiles(context.live.path()))
                before.insert(context.live.filePath(name), readFileBytes(context.live.filePath(name)));
            for (const auto& name : names)
                before.insert(waveforms.filePath(name), readFileBytes(waveforms.filePath(name)));
            const auto replacement = registerToneAsset(context.directory.path(), QStringLiteral("replacement.wav"), 0.7f);
            VNX::AudioSourceData source;
            source.sourceID = replacement;
            source.relativePath = QStringLiteral("replacement.wav");
            context.project.audioSources.push_back(source);
            HarkenTest::setSharedSoundSource(*context.project.layers.front().rootItem->children.front(), replacement);
            const QString companion = context.live.filePath(QStringLiteral("One.json"));
            const auto permissions = QFile::permissions(companion);
            const auto restore = qScopeGuard([&] { QFile::setPermissions(companion, permissions); });
            bool interrupted = false;
            context.options.cancelCheck = [&] {
                if (waveforms.entryList({"*.vkd"}, QDir::Files).size() > names.size()) {
                    interrupted = true;
                    if (!cancel) QFile::setPermissions(companion, withoutWritePermissions(permissions));
                }
                return cancel && interrupted;
            };
            const auto result = context.exporter.ExportAllPacks(context.project, {}, context.options);
            EXPECT_TRUE(interrupted);
            EXPECT_FALSE(result.success);
            EXPECT_EQ(result.canceled, cancel) << result.error.toStdString();
            EXPECT_EQ(waveforms.entryList({"*.vkd"}, QDir::Files), names);
            for (auto file = before.cbegin(); file != before.cend(); ++file)
                EXPECT_EQ(readFileBytes(file.key()), file.value()) << file.key().toStdString();
            EXPECT_FALSE(QFileInfo::exists(context.live.filePath(QStringLiteral(".harken-pack-publishing"))));
        }
    }
}

TEST(EditorProjectPackExporterManifest, CancellationStopsAtBuildStageBoundaries)
{
    for (const auto stage : {VNX::PackGenerationStage::Compiling,
                            VNX::PackGenerationStage::Writing,
                            VNX::PackGenerationStage::Publishing}) {
        PackWriteAccessProject context;
        ASSERT_NO_FATAL_FAILURE(context.setup());
        const auto first = context.exporter.ExportAllPacks(context.project, {}, context.options);
        ASSERT_TRUE(first.success) << first.error.toStdString();
        QMap<QString, QByteArray> before;
        for (const auto& name : publicationFiles(context.live.path()))
            before.insert(name, readFileBytes(context.live.filePath(name)));
        bool canceled = false;
        int writeCount = 0;
        context.options.progressCallback = [&](const VNX::PackGenerationProgress& progress) {
            if (progress.stage == VNX::PackGenerationStage::Writing) ++writeCount;
            if (progress.stage == stage) canceled = true;
        };
        context.options.cancelCheck = [&] { return canceled; };
        const auto result = context.exporter.ExportAllPacks(context.project, {}, context.options);
        EXPECT_FALSE(result.success);
        EXPECT_TRUE(result.canceled);
        EXPECT_EQ(result.error, QStringLiteral("Export canceled"));
        if (stage == VNX::PackGenerationStage::Compiling) EXPECT_EQ(writeCount, 0);
        if (stage == VNX::PackGenerationStage::Writing) EXPECT_EQ(writeCount, 1);
        for (auto file = before.cbegin(); file != before.cend(); ++file)
            EXPECT_EQ(readFileBytes(context.live.filePath(file.key())), file.value());
    }
}

TEST(EditorProjectPackExporterManifest, CanceledFinalizationKeepsPublishedFiles)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup());
    context.project.buildExportProjectData = true;
    const auto first = context.exporter.ExportAllPacks(context.project, {}, context.options);
    ASSERT_TRUE(first.success) << first.error.toStdString();
    const auto finalized = VNX::ProjectPackExporter::FinalizePackExport(
        context.root, context.project, {context.platform}, false, first.artifacts);
    ASSERT_TRUE(finalized.success) << finalized.error.toStdString();
    const QDir root(context.root);
    const QString orphan = root.filePath(QStringLiteral("obsolete.bin"));
    writeFileBytes(orphan, "keep until committed");
    const auto metadata = readFileBytes(root.filePath(QStringLiteral("Project.json")));
    context.project.name += QStringLiteral(" changed");
    const auto cancelCheck = [&] {
        return !root.entryList({QStringLiteral(".harken-pack-next-*")},
            QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty();
    };
    const auto canceled = VNX::ProjectPackExporter::FinalizePackExport(
        context.root, context.project, {context.platform}, true, first.artifacts, cancelCheck);
    EXPECT_FALSE(canceled.success);
    EXPECT_TRUE(canceled.canceled);
    EXPECT_EQ(canceled.error, QStringLiteral("Export canceled"));
    EXPECT_EQ(readFileBytes(orphan), QByteArray("keep until committed"));
    EXPECT_EQ(readFileBytes(root.filePath(QStringLiteral("Project.json"))), metadata);
    EXPECT_FALSE(QFileInfo::exists(root.filePath(QStringLiteral(".harken-pack-publishing"))));
    EXPECT_TRUE(root.entryList({QStringLiteral(".harken-pack-next-*")},
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
}

TEST(EditorProjectPackExporterManifest, ProjectBuildCancellationPreservesCommittedPlatformsAndRootPublication)
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
    context.project.actions.front().rootItem->children.front()->comp<VNX::VAction>()->steps.front().delay = 0.1f;
    QString error;
    const auto snapshot = VNX::ProjectBuildSnapshot::capture(context.project, &error);
    ASSERT_TRUE(snapshot) << error.toStdString();
    bool canceled = false;
    QMap<QString, QByteArray> committed;
    VNX::ProjectPackBuildService::Request request;
    request.outputRoot = context.root;
    request.platforms = {context.platform, HarkenTest::platformIdString("Android")};
    request.options.snapshot = snapshot;
    request.options.selfCleaning = true;
    request.options.cancelCheck = [&] { return canceled; };
    request.platformFinished = [&](const VNX::ProjectPackBuildService::PlatformResult& completed) {
        EXPECT_EQ(completed.platform, context.platform);
        EXPECT_TRUE(completed.result.success) << completed.result.error.toStdString();
        for (const auto& name : publicationFiles(completed.outputDir))
            committed.insert(name, readFileBytes(QDir(completed.outputDir).filePath(name)));
        QString lockError;
        EXPECT_FALSE(VNX::ProjectPackExporter::LockPackOutputRoot(context.root, &lockError));
        canceled = true;
    };
    const auto result = VNX::ProjectPackBuildService::run(request);
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
    const auto released = VNX::ProjectPackExporter::LockPackOutputRoot(context.root, &error);
    EXPECT_TRUE(released) << error.toStdString();
}

TEST(EditorProjectPackExporterManifest, ProjectBuildLockAndPreflightFailuresPreserveOutputs)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup());
    const QDir root(context.root);
    const QString sentinel = root.filePath(QStringLiteral("Project.json"));
    const QString recoveryMarker = context.live.filePath(QStringLiteral(".harken-pack-publishing"));
    ASSERT_NO_FATAL_FAILURE(writeFileBytes(sentinel, QByteArray("previous project publication")));
    ASSERT_NO_FATAL_FAILURE(writeFileBytes(recoveryMarker, QByteArray("incomplete transaction")));
    QString error;
    const auto snapshot = VNX::ProjectBuildSnapshot::capture(context.project, &error);
    ASSERT_TRUE(snapshot) << error.toStdString();
    auto lock = VNX::ProjectPackExporter::LockPackOutputRoot(context.root, &error);
    ASSERT_TRUE(lock) << error.toStdString();
    bool platformStarted = false;
    VNX::ProjectPackBuildService::Request request;
    request.outputRoot = context.root;
    request.platforms = {context.platform};
    request.options.snapshot = snapshot;
    request.options.progressCallback = [&](const VNX::PackGenerationProgress&) { platformStarted = true; };
    const auto competing = VNX::ProjectPackBuildService::run(request);
    EXPECT_FALSE(competing.outputLocked);
    EXPECT_FALSE(competing.preflightSucceeded);
    EXPECT_FALSE(platformStarted);
    EXPECT_EQ(readFileBytes(sentinel), QByteArray("previous project publication"));
    EXPECT_EQ(readFileBytes(recoveryMarker), QByteArray("incomplete transaction"));
    request.outputLock = lock;
    const auto rejected = VNX::ProjectPackBuildService::run(request);
    EXPECT_TRUE(rejected.outputLocked);
    EXPECT_FALSE(rejected.preflightSucceeded);
    EXPECT_FALSE(platformStarted);
    EXPECT_TRUE(rejected.preflightError.startsWith(QStringLiteral("Cannot recover interrupted pack publish:")))
        << rejected.preflightError.toStdString();
    EXPECT_TRUE(lock->isLocked());
    EXPECT_FALSE(VNX::ProjectPackExporter::LockPackOutputRoot(context.root, &error));
    EXPECT_FALSE(QFileInfo::exists(context.live.filePath(QStringLiteral("Init.bytes"))));
    EXPECT_EQ(readFileBytes(sentinel), QByteArray("previous project publication"));
    EXPECT_EQ(readFileBytes(recoveryMarker), QByteArray("incomplete transaction"));
    const QDir protectedRoot(context.directory.path());
    const QString protectedFile = protectedRoot.filePath(QStringLiteral("project-data.txt"));
    const QString protectedMarker = protectedRoot.filePath(QStringLiteral(".harken-pack-publishing"));
    const QString previous = QStringLiteral(".harken-pack-previous-protected");
    ASSERT_NO_FATAL_FAILURE(writeFileBytes(protectedFile, QByteArray("project data")));
    ASSERT_NO_FATAL_FAILURE(writeFileBytes(protectedRoot.filePath(previous + QStringLiteral("/project-data.txt")),
        QByteArray("unrelated previous data")));
    const QJsonObject transaction{{"version", 1}, {"previous", previous},
        {"next", QStringLiteral(".harken-pack-next-protected")},
        {"previousFiles", QJsonArray{QStringLiteral("project-data.txt")}},
        {"nextFiles", QJsonArray{QStringLiteral("project-data.txt")}}};
    const auto protectedTransaction = QJsonDocument(transaction).toJson();
    ASSERT_NO_FATAL_FAILURE(writeFileBytes(protectedMarker, protectedTransaction));
    request.outputRoot = protectedRoot.path();
    request.outputLock.reset();
    const auto protectedResult = VNX::ProjectPackBuildService::run(request);
    EXPECT_FALSE(protectedResult.outputLocked);
    EXPECT_EQ(protectedResult.preflightError, VNX::PathUtils::packBuildOutputLocationError());
    EXPECT_EQ(readFileBytes(protectedFile), QByteArray("project data"));
    EXPECT_EQ(readFileBytes(protectedMarker), protectedTransaction);
}

TEST(EditorProjectPackExporterManifest, ProjectBuildCleanupPreservesActiveLocksAcrossPlatforms)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup());
    for (int attempt = 0; attempt < 3; ++attempt) {
        SCOPED_TRACE(attempt);
        if (attempt == 1) context.project.platforms.front().name = QStringLiteral("windows");
        const QString obsolete = QDir(context.root).filePath(QStringLiteral("obsolete.bin"));
        ASSERT_NO_FATAL_FAILURE(writeFileBytes(obsolete, QByteArray("remove during cleanup")));
        QString error;
        VNX::ProjectPackBuildService::Request request;
        request.outputRoot = context.root;
        request.platforms = HarkenTest::platformIdStrings({"Windows", "Android"});
        request.options.selfCleaning = true;
        request.options.snapshot = VNX::ProjectBuildSnapshot::capture(context.project, &error);
        ASSERT_TRUE(request.options.snapshot) << error.toStdString();
        std::vector<std::shared_ptr<QLockFile>> heldLocks;
        request.platformFinished = [&](const VNX::ProjectPackBuildService::PlatformResult& built) {
            ASSERT_TRUE(built.result.success) << built.result.error.toStdString();
            ASSERT_TRUE(built.result.publication);
            auto lock = built.result.publication->outputLock.lock();
            ASSERT_TRUE(lock);
            heldLocks.push_back(std::move(lock));
        };
        const auto built = VNX::ProjectPackBuildService::run(request);
        ASSERT_TRUE(built.allPlatformsSucceeded) << built.preflightError.toStdString();
        ASSERT_TRUE(built.finalizationResult.success) << built.finalizationResult.error.toStdString();
        ASSERT_EQ(heldLocks.size(), request.platforms.size());
        EXPECT_FALSE(QFileInfo::exists(obsolete));
        for (const auto& platform : built.platforms) {
            const QDir output(platform.outputDir);
            EXPECT_TRUE(QDir(context.root).entryList(QDir::Dirs | QDir::NoDotAndDotDot)
                .contains(platform.definition.name));
            const QString lockPath = output.filePath(QStringLiteral(".harken-pack-platform.lock"));
            EXPECT_TRUE(QFileInfo::exists(output.filePath(QStringLiteral("Init.bytes"))));
            EXPECT_TRUE(QFileInfo::exists(output.filePath(QStringLiteral("One.bytes"))));
            EXPECT_TRUE(QFileInfo::exists(lockPath));
            QLockFile competing(lockPath);
            competing.setStaleLockTime(0);
            EXPECT_FALSE(competing.tryLock());
        }
    }
}

TEST(EditorProjectPackExporterManifest, ProjectBuildIncrementalResultsMatchForcedBuildAfterInputAndOutputChanges)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup());
    const auto build = [&](bool force) {
        QString error;
        VNX::ProjectPackBuildService::Request request;
        request.outputRoot = context.root;
        request.platforms = {context.platform};
        request.options.snapshot = VNX::ProjectBuildSnapshot::capture(context.project, &error);
        EXPECT_TRUE(request.options.snapshot) << error.toStdString();
        request.options.forceReexport = force;
        return VNX::ProjectPackBuildService::run(request);
    };
    const auto initial = build(false);
    ASSERT_TRUE(initial.allPlatformsSucceeded) << initial.preflightError.toStdString();
    ASSERT_TRUE(initial.finalizationResult.success) << initial.finalizationResult.error.toStdString();
    QByteArray previousContent = readFileBytes(context.live.filePath(QStringLiteral("One.bytes")));
    const QString sourcePath = QDir(VNX::PathUtils::getAudioSourcesDir(context.directory.path()))
        .filePath(QStringLiteral("tone.wav"));
    const std::vector<std::function<void()>> changes{
        [&] {
            context.project.actions.front().rootItem->children.front()
                ->comp<VNX::VAction>()->steps.front().delay = 0.125f;
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
            const QString path = context.live.filePath(QStringLiteral("One.bytes"));
            const auto modified = QFileInfo(path).lastModified();
            auto bytes = readFileBytes(path);
            ASSERT_FALSE(bytes.isEmpty());
            bytes[bytes.size() - 1] = static_cast<char>(bytes.back() ^ 0x40);
            ASSERT_NO_FATAL_FAILURE(writeFileBytes(path, bytes));
            QFile file(path);
            ASSERT_TRUE(file.open(QIODevice::ReadWrite));
            ASSERT_TRUE(file.setFileTime(modified, QFileDevice::FileModificationTime));
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
        if (index < 2) EXPECT_NE(content, previousContent);
        previousContent = content;
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

TEST(EditorProjectPackExporterManifest, ProjectBuildReusePreservesDiagnosticsAfterReload)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup());
    auto actions = std::move(context.project.actions.front().rootItem->children);
    actions.push_back(makeItem(QStringLiteral("Empty"), VNX::ProjectItem::Type::Action, 55));
    context.project.actions.clear();
    for (auto& action : actions) {
        action->parent = nullptr;
        VNX::ProjectFileBinding binding;
        binding.rootItem = std::move(action);
        binding.parentFolder = QLatin1String(VNX::ItemTypeCatalog::kActionsFolderName);
        context.project.actions.push_back(std::move(binding));
    }
    const auto build = [&](const std::shared_ptr<const VNX::ProjectBuildSnapshot>& snapshot,
                           bool* compiled) {
        VNX::ProjectPackBuildService::Request request;
        request.outputRoot = context.root;
        request.platforms = {context.platform};
        request.options.snapshot = snapshot;
        request.options.observationCallback = [compiled](const VNX::PackBuildObservation& observation) {
            if (observation.stage == VNX::PackBuildObservationStage::CompilingProject) *compiled = true;
        };
        return VNX::ProjectPackBuildService::run(request);
    };
    QString error;
    const auto firstSnapshot = VNX::ProjectBuildSnapshot::capture(context.project, &error);
    ASSERT_TRUE(firstSnapshot) << error.toStdString();
    bool compiled = false;
    const auto initial = build(firstSnapshot, &compiled);
    ASSERT_TRUE(initial.allPlatformsSucceeded) << initial.preflightError.toStdString();
    ASSERT_TRUE(initial.finalizationResult.success) << initial.finalizationResult.error.toStdString();
    ASSERT_EQ(initial.platforms.size(), 1u);
    ASSERT_TRUE(compiled);
    ASSERT_FALSE(initial.platforms.front().result.warnings.isEmpty());
    QMap<QString, QByteArray> expected;
    for (const auto& path : publicationFiles(context.live.path()))
        expected.insert(path, readFileBytes(context.live.filePath(path)));
    VNX::ProjectSerializer writer;
    ASSERT_TRUE(writer.prepareReferenceWrite(context.project)) << writer.lastError().toStdString();
    QJsonArray roots;
    for (const auto* binding : context.project.allFileBindings()) {
        const auto serialized = writer.projectItemToJson(*binding->rootItem);
        ASSERT_FALSE(serialized.isEmpty()) << writer.lastError().toStdString();
        roots.append(serialized);
    }
    VNX::ProjectSerializer reader;
    ASSERT_TRUE(reader.prepareStandaloneReferenceRead(roots)) << reader.lastError().toStdString();
    auto reloaded = context.project;
    size_t index = 0;
    for (auto* binding : reloaded.allFileBindings()) {
        binding->rootItem = std::make_shared<VNX::ProjectItem>();
        ASSERT_TRUE(reader.projectItemFromJson(roots.at(static_cast<qsizetype>(index++)).toObject(),
            *binding->rootItem)) << reader.lastError().toStdString();
    }
    ASSERT_NE(reloaded.actions.front().rootItem->objectID,
        context.project.actions.front().rootItem->objectID);
    const auto captured = VNX::ProjectBuildSnapshot::capture(reloaded, &error);
    ASSERT_TRUE(captured) << error.toStdString();
    auto nextSnapshot = std::make_shared<VNX::ProjectBuildSnapshot>(*captured);
    reloaded.audioSources.clear();
    reloaded.sourceAssets.clear();
    for (auto& asset : nextSnapshot->media.assets) {
        asset.metadata = {};
        asset.contentHash.clear();
        asset.ownerObjectGuid.clear();
        asset.isOrphaned = !asset.isOrphaned;
    }
    compiled = false;
    const auto reused = build(nextSnapshot, &compiled);
    ASSERT_TRUE(reused.allPlatformsSucceeded) << reused.preflightError.toStdString();
    ASSERT_TRUE(reused.finalizationResult.success) << reused.finalizationResult.error.toStdString();
    ASSERT_EQ(reused.platforms.size(), 1u);
    EXPECT_FALSE(compiled);
    EXPECT_EQ(reused.platforms.front().result.warnings, initial.platforms.front().result.warnings);
    ASSERT_EQ(reused.platforms.front().records.size(), initial.platforms.front().records.size());
    for (const auto& record : reused.platforms.front().records) {
        const auto prior = std::find_if(initial.platforms.front().records.begin(),
            initial.platforms.front().records.end(), [&](const auto& candidate) {
                return candidate.unitKey == record.unitKey && candidate.language == record.language;
            });
        ASSERT_NE(prior, initial.platforms.front().records.end());
        EXPECT_EQ(record.warnings, prior->warnings);
        EXPECT_EQ(record.omitted, prior->omitted);
        EXPECT_TRUE(record.success);
        if (record.source == VNX::ProjectPackExporter::PackBuildRecord::Source::Action) {
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

TEST(EditorProjectPackExporterManifest, ProjectBuildReuseOnlyDependsOnSelectedPlatformSources)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup());
    const auto androidSource = registerToneAsset(context.directory.path(), "android.wav", 0.4f);
    auto& sources = context.project.layers.front().rootItem->children.front()
        ->comp<VNX::VSound>()->sources.plain;
    VNX::SoundSourceDef android;
    android.entryId = QUuid::createUuid();
    android.sourceId = androidSource;
    sources.entries.push_back(android);
    sources.platforms[HarkenTest::platformIdString("Android")].active = android.entryId;
    const auto build = [&](const QStringList& platforms, bool* compiled) {
        QString error;
        VNX::ProjectPackBuildService::Request request;
        request.outputRoot = context.root;
        request.platforms = platforms;
        request.options.snapshot = VNX::ProjectBuildSnapshot::capture(context.project, &error);
        EXPECT_TRUE(request.options.snapshot) << error.toStdString();
        request.options.observationCallback = [compiled](const VNX::PackBuildObservation& observation) {
            if (observation.stage == VNX::PackBuildObservationStage::CompilingProject) *compiled = true;
        };
        return VNX::ProjectPackBuildService::run(request);
    };
    bool compiled = false;
    const auto initial = build(HarkenTest::platformIdStrings({"Windows", "Android"}), &compiled);
    ASSERT_TRUE(initial.allPlatformsSucceeded) << initial.preflightError.toStdString();
    ASSERT_TRUE(initial.finalizationResult.success) << initial.finalizationResult.error.toStdString();
    const auto windowsBytes = readFileBytes(context.live.filePath(QStringLiteral("One.bytes")));
    ASSERT_TRUE(QFile::remove(QDir(VNX::PathUtils::getAudioSourcesDir(context.directory.path()))
        .filePath(QStringLiteral("android.wav"))));
    compiled = false;
    const auto reused = build({context.platform}, &compiled);
    ASSERT_TRUE(reused.allPlatformsSucceeded) << reused.preflightError.toStdString();
    ASSERT_TRUE(reused.finalizationResult.success) << reused.finalizationResult.error.toStdString();
    EXPECT_FALSE(compiled);
    EXPECT_EQ(readFileBytes(context.live.filePath(QStringLiteral("One.bytes"))), windowsBytes);
}

TEST(EditorProjectPackExporterManifest, PublicationValidationKeepsDependencyAndLockLifetimeBoundaries)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup());
    QString error;
    VNX::ProjectPackBuildService::Request request;
    request.outputRoot = context.root;
    request.platforms = {context.platform};
    request.options.snapshot = VNX::ProjectBuildSnapshot::capture(context.project, &error);
    ASSERT_TRUE(request.options.snapshot) << error.toStdString();
    request.platformFinished = [&](const VNX::ProjectPackBuildService::PlatformResult& built) {
        ASSERT_TRUE(built.result.success) << built.result.error.toStdString();
        ASSERT_TRUE(built.result.publication);
        const QMap<QString, VNX::PackPublicationProof> publications{
            {context.platform, *built.result.publication}};
        EXPECT_TRUE(VNX::ProjectPackExporter::ValidatePackOutputs(context.root,
            {built.definition}, &error, built.result.artifacts, {}, publications)) << error.toStdString();
        auto incomplete = built.result.artifacts;
        incomplete.removeIf([](const auto& artifact) { return artifact.unitKey == QStringLiteral("Init"); });
        ASSERT_FALSE(incomplete.isEmpty());
        EXPECT_FALSE(VNX::ProjectPackExporter::ValidatePackOutputs(context.root,
            {built.definition}, &error, incomplete, {}, publications));
    };
    const auto built = VNX::ProjectPackBuildService::run(request);
    ASSERT_TRUE(built.allPlatformsSucceeded) << built.preflightError.toStdString();
    ASSERT_TRUE(built.finalizationResult.success) << built.finalizationResult.error.toStdString();
    ASSERT_EQ(built.platforms.size(), 1u);
    ASSERT_TRUE(built.platforms.front().result.publication);
    const QMap<QString, VNX::PackPublicationProof> publications{
        {context.platform, *built.platforms.front().result.publication}};
    const QString path = context.live.filePath(QStringLiteral("One.bytes"));
    const auto valid = readFileBytes(path);
    ASSERT_NO_FATAL_FAILURE(writeFileBytes(path, valid.chopped(1)));
    EXPECT_FALSE(VNX::ProjectPackExporter::ValidatePackOutputs(context.root,
        {built.platforms.front().definition}, &error, built.artifacts, {}, publications));
}

TEST(EditorProjectPackExporterManifest, RecoveryPrecedesPreflightForRootAndPartialExports)
{
    for (const bool directPartial : {false, true}) {
        PackWriteAccessProject context;
        ASSERT_NO_FATAL_FAILURE(context.setup());
        const QString previousName = ".harken-pack-previous-interrupted";
        const QString nextName = ".harken-pack-next-interrupted";
        const QDir previous(context.live.filePath(previousName));
        const auto first = context.exporter.ExportAllPacks(context.project, previous.path(), context.options);
        ASSERT_TRUE(first.success) << first.error.toStdString();
        ASSERT_NO_FATAL_FAILURE(renamePublicationToLegacy(previous, false));
        const auto names = previous.entryList(QDir::Files);
        std::map<QString, QByteArray> before;
        for (const auto& name : names) {
            before.emplace(name, readFileBytes(previous.filePath(name)));
            writeFileBytes(context.live.filePath(name), "partial");
        }
        const QString marker = context.live.filePath(".harken-pack-publishing");
        const QJsonObject transaction{{"version", 1}, {"previous", previousName}, {"next", nextName},
            {"previousFiles", QJsonArray::fromStringList(names)}, {"nextFiles", QJsonArray::fromStringList(names)}};
        writeFileBytes(marker, QJsonDocument(transaction).toJson());
        context.options.cancelCheck = [] { return true; };
        context.options.updateSelectedOnly = directPartial;
        if (directPartial) context.options.onlyPackPaths = {QStringLiteral("One")};
        const auto result = context.exporter.ExportAllPacks(context.project,
            directPartial ? context.live.path() : QString(), context.options);
        EXPECT_TRUE(result.canceled);
        EXPECT_EQ(result.error, QStringLiteral("Export canceled"));
        EXPECT_FALSE(QFileInfo::exists(marker));
        for (const auto& [name, bytes] : before)
            EXPECT_EQ(readFileBytes(context.live.filePath(name)), bytes);
    }
}

#ifdef Q_OS_WIN
TEST(EditorProjectPackExporterManifest, UnreadableOptionalWaveformDoesNotBlockPackExport)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup(true));
    context.project.buildWaveformOutputDir = context.live.filePath(QStringLiteral("Waveforms"));
    const auto first = context.exporter.ExportAllPacks(context.project, {}, context.options);
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
    EXPECT_TRUE(VNX::ProjectPackExporter::ValidatePackBuildWriteAccess(
        context.project, context.root, {context.platform}, context.options, &error)) << error.toStdString();
    const auto result = context.exporter.ExportAllPacks(context.project, {}, context.options);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    EXPECT_FALSE(result.warnings.isEmpty());
    EXPECT_FALSE(findByName(readPublication(context.live.path()), QStringLiteral("One"))
        .value(QStringLiteral("companion")).toObject().contains(QStringLiteral("waveforms")));
    const auto finalized = VNX::ProjectPackExporter::FinalizePackExport(
        context.root, context.project, {context.platform}, false, result.artifacts);
    EXPECT_TRUE(finalized.success) << finalized.error.toStdString();
}

TEST(EditorProjectPackExporterManifest, FileDenyingReplacementMustFailBeforeBuilding)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup());
    const auto first = context.exporter.ExportAllPacks(context.project, {}, context.options);
    ASSERT_TRUE(first.success) << first.error.toStdString();
    const QString path = context.live.filePath("One.bytes");
    std::map<QString, QByteArray> before;
    for (const auto& name : context.live.entryList(QDir::Files))
        before.emplace(name, readFileBytes(context.live.filePath(name)));
    HANDLE handle = INVALID_HANDLE_VALUE;
    const auto close = qScopeGuard([&] { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); });
    const auto occupy = [&] {
        handle = CreateFileW(reinterpret_cast<LPCWSTR>(path.utf16()), GENERIC_READ,
            FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        EXPECT_NE(handle, INVALID_HANDLE_VALUE);
    };
    occupy();
    context.options.forceReexport = true;
    QString error;
    EXPECT_FALSE(VNX::ProjectPackExporter::ValidatePackBuildWriteAccess(
        context.project, context.root, {context.platform}, context.options, &error));
    bool started = false;
    context.options.progressCallback = [&](const VNX::PackGenerationProgress& progress) {
        started |= progress.stage == VNX::PackGenerationStage::Compiling
            || progress.stage == VNX::PackGenerationStage::Writing;
    };
    const auto result = context.exporter.ExportAllPacks(context.project, {}, context.options);
    EXPECT_FALSE(started) << result.error.toStdString();
    EXPECT_FALSE(result.success);
    CloseHandle(handle);
    handle = INVALID_HANDLE_VALUE;
    context.options.progressCallback = [&](const VNX::PackGenerationProgress& progress) {
        if (progress.stage == VNX::PackGenerationStage::Publishing && handle == INVALID_HANDLE_VALUE)
            occupy();
    };
    const auto lateLock = context.exporter.ExportAllPacks(context.project, {}, context.options);
    EXPECT_NE(handle, INVALID_HANDLE_VALUE);
    EXPECT_FALSE(lateLock.success);
    EXPECT_TRUE(lateLock.error.contains(QDir::toNativeSeparators(path))) << lateLock.error.toStdString();
    for (const auto& [name, bytes] : before)
        EXPECT_EQ(readFileBytes(context.live.filePath(name)), bytes);
    EXPECT_FALSE(QFileInfo::exists(context.live.filePath(".harken-pack-publishing")));
}

#endif

TEST(EditorProjectPackExporterManifest, PartialExportMustPreflightInvalidInitThatWillBeReplaced)
{
    PackWriteAccessProject context;
    ASSERT_NO_FATAL_FAILURE(context.setup());
    const auto first = context.exporter.ExportAllPacks(context.project, {}, context.options);
    ASSERT_TRUE(first.success) << first.error.toStdString();
    const QString path = context.live.filePath("Init.bytes");
    writeFileBytes(path, "corrupt");
    const auto permissions = QFile::permissions(path);
    const auto restore = qScopeGuard([&] { QFile::setPermissions(path, permissions); });
    ASSERT_TRUE(QFile::setPermissions(path, withoutWritePermissions(permissions)));
    context.options.updateSelectedOnly = true;
    context.options.onlyPackPaths = {QStringLiteral("One")};
    QString error;
    EXPECT_FALSE(VNX::ProjectPackExporter::ValidatePackBuildWriteAccess(
        context.project, context.root, {context.platform}, context.options, &error));
    bool started = false;
    context.options.progressCallback = [&](const VNX::PackGenerationProgress& progress) {
        started |= progress.stage == VNX::PackGenerationStage::Compiling
            || progress.stage == VNX::PackGenerationStage::Writing;
    };
    const auto result = context.exporter.ExportAllPacks(context.project, {}, context.options);
    EXPECT_FALSE(started) << result.error.toStdString();
    EXPECT_FALSE(result.success);
}

TEST(EditorProjectPackExporterManifest, CancelAfterMediaPreparationKeepsPreviousSet)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("One"), 21, kSoloSoundObjectId}}));
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, soloSourceId));
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
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
    auto* sound = project.layers.front().rootItem->children.at(1)->comp<VNX::VSound>();
    sound->sources.plain.entries.front().trimStartFrame = 8;
    int preparedVariants = 0;
    bool canceled = false;
    QObject::connect(&exporter, &VNX::ProjectPackExporter::progressChanged,
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
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, FailedPublishLeavesPreviousSet)
{
    qputenv("HARKENIZER_PATH", QByteArray(HARKENIZER_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t sharedSourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared.wav"), 1.0f);
    const uint32_t soloSourceId =
        registerToneAsset(dir.path(), QStringLiteral("solo.wav"), 0.5f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), sharedSourceId, soloSourceId,
        {{QStringLiteral("One"), 21, kSoloSoundObjectId}}));
    ASSERT_NO_FATAL_FAILURE(localizeSoloSound(project, soloSourceId));
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const QString outputDir = QDir(dir.path()).filePath(QStringLiteral("out"));
    ASSERT_TRUE(exporter.ExportAllPacks(project, outputDir, opts).success);
    std::map<QString, QByteArray> previousPublications;
    for (const QString& relative : publicationFiles(outputDir)) {
        if (relative == QStringLiteral("Init.json")) continue;
        const QString path = QDir(outputDir).filePath(relative);
        previousPublications.emplace(path, readFileBytes(path));
    }
    const QStringList previousPacks =
        QDir(outputDir).entryList({"*.bytes"}, QDir::Files);
    ASSERT_FALSE(previousPacks.isEmpty());
    const QString englishPath = QDir(outputDir).filePath(
        variantFileName(project, harkentest::StudioObjectId(21 + kStableIdBias), QStringLiteral("English")));
    const QString japanesePath = QDir(outputDir).filePath(
        variantFileName(project, harkentest::StudioObjectId(21 + kStableIdBias), QStringLiteral("ja")));
    const QByteArray previousEnglish = readFileBytes(englishPath);
    const QByteArray previousJapanese = readFileBytes(japanesePath);
    const QByteArray previousInit = [&] {
        QFile file(QDir(outputDir).filePath(QStringLiteral("Init.bytes")));
        EXPECT_TRUE(file.open(QIODevice::ReadOnly));
        return file.readAll();
    }();
    const QString blocker = QDir(outputDir).filePath(QStringLiteral("Init.json"));
    ASSERT_TRUE(QFile::remove(blocker));
    ASSERT_TRUE(QDir().mkpath(blocker));
    const auto failed = exporter.ExportAllPacks(project, outputDir, opts);
    EXPECT_FALSE(failed.success);
    EXPECT_TRUE(failed.artifacts.isEmpty());
    for (const auto& [path, bytes] : previousPublications)
        EXPECT_EQ(readFileBytes(path), bytes) << path.toStdString();
    QFile current(QDir(outputDir).filePath(QStringLiteral("Init.bytes")));
    ASSERT_TRUE(current.open(QIODevice::ReadOnly));
    EXPECT_EQ(current.readAll(), previousInit);
    EXPECT_EQ(readFileBytes(englishPath), previousEnglish);
    EXPECT_EQ(readFileBytes(japanesePath), previousJapanese);
    EXPECT_EQ(QDir(outputDir).entryList({"*.bytes"}, QDir::Files), previousPacks);
    const QStringList leftover = QDir(outputDir).entryList(
        QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden);
    for (const QString& name : leftover)
        EXPECT_FALSE(name.startsWith(QStringLiteral(".harken-pack-staging-")))
            << name.toStdString();
    VNX::AssetRegistry::instance().clear();
}
