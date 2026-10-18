#include "../../Source/Tooling/PackC/GameplayPublication.h"
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Engine.h"
#include "UnitTest/TestUtil.h"
#include "UnitTest/CanonicalPackSetActivate.h"

#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/AssetConverter.h"
#include "AssetPipeline/PathUtils.h"
#include "Components/VAction.h"
#include "Components/VAudio.h"
#include "Components/VBus.h"
#include "Components/VMixNode.h"
#include "Components/VSelection.h"
#include "Components/VTrack.h"
#include "Components/VVariable.h"
#include "PackBuildPlan.h"
#include "CanonicalPackSetExport.h"
#include "PackSchemaGenerated.h"
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectPackExporter.h"
#include "NameValidator.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "RuntimeContentBundleBuilder.h"

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
#include <QJsonObject>
#include <QSet>
#include <QTemporaryDir>
#include <QUuid>
#include <QVector>

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <limits>
#include <map>
#include <memory>
#include <vector>
#include "Support/ProjectPlatformTestUtils.h"

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
    project.conversionFailurePolicy = VNX::ConversionFailurePolicy::AbortBuild;

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

QJsonObject readManifest(const QString& outputDir)
{
    const QString manifestPath = QDir(outputDir).filePath(
        HarkenEditor::Bridge::packManifestFileName());
    QFile file(manifestPath);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << manifestPath.toStdString();
    const QJsonDocument doc = QJsonDocument::fromJson(file.readAll());
    EXPECT_TRUE(doc.isObject());
    return doc.object();
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

void expectWaveformMatchesFinalPcm(const QString& selectedDirectory, const QString& packRoot)
{
    const QByteArray manifest = readFileBytes(QDir(selectedDirectory).filePath(HarkenEditor::Bridge::packManifestFileName()));
    const auto metadata = QJsonDocument::fromJson(readFileBytes(QDir(selectedDirectory).filePath(
        QStringLiteral("HarkenEditorWaveforms.json")))).object();
    ASSERT_FALSE(metadata.isEmpty());
    EXPECT_EQ(metadata.value(QStringLiteral("manifestByteLength")).toInteger(), manifest.size());
    EXPECT_EQ(metadata.value(QStringLiteral("manifestFnv1a64")).toString().toULongLong(),
        harken::Fnv1a64(manifest.constData(), manifest.size()));
    const auto packs = QJsonDocument::fromJson(manifest).object().value(QStringLiteral("packs")).toArray();
    int checked = 0;
    for (const auto& value : metadata.value(QStringLiteral("sources")).toArray()) {
        const auto source = value.toObject();
        QJsonObject owner;
        for (const auto& item : packs)
            if (item.toObject().value(QStringLiteral("packStableId")) == source.value(QStringLiteral("ownerPackStableId")))
                owner = item.toObject();
        ASSERT_FALSE(owner.isEmpty());
        const auto bytes = readFileBytes(QDir(packRoot).filePath(owner.value(QStringLiteral("path")).toString()));
        EXPECT_EQ(source.value(QStringLiteral("contentHash")).toString().toULongLong(), harken::Fnv1a64(bytes.constData(), bytes.size()));
        harken::PackCanonicalImage image;
        ASSERT_EQ(harken::ParsePackCanonicalImage(reinterpret_cast<const std::uint8_t*>(bytes.constData()), bytes.size(), &image), VN_OK);
        const auto sound = VNX::runtimeObjectId(QUuid(source.value(QStringLiteral("soundStableId")).toString()));
        const auto media = std::find_if(image.media.begin(), image.media.end(), [sound](const auto& item) { return item.soundId == sound; });
        ASSERT_NE(media, image.media.end());
        EXPECT_EQ(source.value(QStringLiteral("frameCount")).toString().toULongLong(), media->frameCount);
        EXPECT_EQ(source.value(QStringLiteral("sampleRate")).toInt(), media->sampleRate);
        const auto channels = source.value(QStringLiteral("channels")).toArray();
        ASSERT_EQ(channels.size(), media->channelCount);
        for (int c = 0; c < channels.size(); ++c) {
            const auto minima = channels[c].toObject().value(QStringLiteral("min")).toArray();
            const auto maxima = channels[c].toObject().value(QStringLiteral("max")).toArray();
            ASSERT_EQ(minima.size(), std::min<std::uint64_t>(media->frameCount, 2048));
            ASSERT_EQ(maxima.size(), minima.size());
            for (int b = 0; b < minima.size(); ++b) {
                float lo = std::numeric_limits<float>::infinity(), hi = -lo;
                for (auto f = b * media->frameCount / minima.size(); f < (b + 1) * media->frameCount / minima.size(); ++f) {
                    float sample;
                    std::memcpy(&sample, bytes.constData() + media->blobOffset + (c * media->frameCount + f) * sizeof(float), sizeof(float));
                    lo = std::min(lo, sample); hi = std::max(hi, sample);
                }
                EXPECT_FLOAT_EQ(static_cast<float>(minima[b].toDouble()), lo);
                EXPECT_FLOAT_EQ(static_cast<float>(maxima[b].toDouble()), hi);
            }
        }
        ++checked;
    }
    EXPECT_GT(checked, 0);
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

void bindPublishedCatalog(const QJsonObject& manifest, VnPackHandle init) {
    harkenstudiojson::JsonValue json;
    std::string error;
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(harkenstudiojson::ParseJson(QJsonDocument(manifest).toJson(QJsonDocument::Compact).toStdString(), &json, &error, true));
    ASSERT_TRUE(harkenstudiobuild::tooling::EncodeGameplayPublication(json, &bytes));
    VnPackByteSpan span{};
    span.structSize = sizeof(span);
    span.bytes = bytes.data();
    span.byteCount = bytes.size();
    ASSERT_EQ(VN_OK, Vn_SetGameplayCatalog(init, &span));
}

void activateExportedNames(const QString& outputDir, const QStringList& names,
                          const QString& language = {},
                          std::map<QString, VnPackHandle>* handles = nullptr)
{
    const auto manifestDir = language.isEmpty() ? outputDir : QDir(outputDir).filePath(language);
    const auto packs = readManifest(manifestDir).value(QStringLiteral("packs")).toArray();
    std::vector<QByteArray> paths;
    std::vector<VnCanonicalPackSetMemberV1> members;
    QStringList memberNames;
    const auto root = QFileInfo(outputDir).absoluteFilePath().toUtf8();
    for (const auto& value : packs) {
        const auto pack = value.toObject();
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
    ASSERT_NO_FATAL_FAILURE(bindPublishedCatalog(readManifest(manifestDir), report.initHandle));
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

void renamePublicationToLegacy(const QDir& dir, bool wrapped)
{
    const QString manifestPath = dir.filePath(HarkenEditor::Bridge::packManifestFileName());
    QJsonObject manifest = readManifest(dir.path());
    QJsonArray rows;
    for (const QJsonValue& value : manifest.value(QStringLiteral("packs")).toArray()) {
        QJsonObject row = value.toObject();
        const QString current = row.value(QStringLiteral("path")).toString();
        ASSERT_TRUE(current.endsWith(QStringLiteral(".bytes")));
        const QString logical = current.chopped(6) + QStringLiteral(".pack");
        const QString physical = logical + (wrapped ? QStringLiteral(".bytes") : QString());
        ASSERT_TRUE(QFile::rename(dir.filePath(current), dir.filePath(physical)));
        row.insert(QStringLiteral("path"), logical);
        rows.append(row);
    }
    manifest.insert(QStringLiteral("packs"), rows);
    manifest.remove(QStringLiteral("catalogChecksum"));
    const QByteArray unsealed = QJsonDocument(manifest).toJson(QJsonDocument::Compact);
    manifest.insert(QStringLiteral("catalogChecksum"), QString::number(
        harken::Fnv1a64(unsealed.constData(), static_cast<std::size_t>(unsealed.size()))));
    writeFileBytes(manifestPath, QJsonDocument(manifest).toJson(QJsonDocument::Compact));
}

} // namespace

TEST(EditorProjectPackExporterManifest, StopOnlyPublishesNoTargetMediaOrDefinitions)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
    const auto packs = readManifest(dir.path()).value(QStringLiteral("packs")).toArray();
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
            ASSERT_EQ(body.ProductionView()->actionCount, 1u);
            const auto& action = body.ProductionView()->actions[0];
            for (uint32_t i = 0; i < action.stepCount; ++i)
                if (action.steps[i].opcode == harken::ActionOpcodeV1::Stop)
                    EXPECT_EQ(action.steps[i].target.expectedProgramSemanticFingerprint, fingerprint);
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
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
    for (const auto type : {VNX::ProjectItem::Type::Track, VNX::ProjectItem::Type::Piece}) {
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
            if (type == VNX::ProjectItem::Type::Piece) {
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
            EXPECT_EQ(body.ProductionView()->pieceScheduleCount, 0u);
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
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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

TEST(EditorProjectPackExporterManifest, CurrentSchemaCoversAllPackTypes)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

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

    VNX::PackGroup group;
    group.name = QStringLiteral("Bundle");
    group.actionIds = {harkentest::StudioObjectId(21 + kStableIdBias)};
    project.packGroups.push_back(group);

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");

    auto result = exporter.ExportAllPacks(project, dir.path(), opts);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QJsonObject manifest = readManifest(dir.path());
    EXPECT_EQ(manifest.value(QStringLiteral("schema_version")).toInt(),
              VNX::PackSchema::kPackManifestSchemaVersion);

    const QJsonArray packs = manifest.value(QStringLiteral("packs")).toArray();

    const QJsonObject groupedPack = findByName(packs, QStringLiteral("GroupedOne"));
    ASSERT_FALSE(groupedPack.isEmpty());
    EXPECT_TRUE(findByName(packs, QStringLiteral("Bundle")).isEmpty());
    const QJsonObject groupedAction = findByName(
        manifest.value(QStringLiteral("gameplayCatalog")).toObject()
            .value(QStringLiteral("actions")).toArray(), QStringLiteral("GroupedOne"));
    EXPECT_EQ(groupedAction.value(QStringLiteral("ownerPackStableId")),
              groupedPack.value(QStringLiteral("packStableId")));

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
    EXPECT_FALSE(manifest.contains(QStringLiteral("events")));

    for (const QString& fileName :
         {variantFileName(project, harkentest::StudioObjectId(21 + kStableIdBias)),
          variantFileName(project, harkentest::StudioObjectId(22 + kStableIdBias)),
          variantFileName(project, harkentest::StudioObjectId(23 + kStableIdBias))})
    {
        EXPECT_TRUE(QFileInfo::exists(QDir(dir.path()).filePath(fileName)))
            << fileName.toStdString();
    }

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest,
     LoopMediaRevisionMatchesCanonicalRuntimeRegistration)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

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

    const QJsonObject manifest = readManifest(dir.path());
    EXPECT_TRUE(manifest.contains(QStringLiteral("catalogChecksum")));
    EXPECT_TRUE(manifest.contains(QStringLiteral("gameplayCatalog")));
    const QJsonObject gameplayCatalog =
        manifest.value(QStringLiteral("gameplayCatalog")).toObject();
    EXPECT_EQ(gameplayCatalog.value(QStringLiteral("schema")).toInt(), 2);
    EXPECT_EQ(gameplayCatalog.value(QStringLiteral("actions")).toArray().size(), 3);
    const QJsonArray packs = manifest.value(QStringLiteral("packs")).toArray();
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

TEST(EditorProjectPackExporterManifest, RenameKeepsManifestMappingSelfConsistent)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

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
    const QJsonObject beforeEntry = findByName(
        readManifest(output.path()).value(QStringLiteral("packs")).toArray(),
        QStringLiteral("BeforeName"));
    const QString legacyName = QStringLiteral("action_3da91a64-b947-4560-0000-0000000186b5.bytes");
    ASSERT_TRUE(QFile::copy(output.filePath(beforeFileName),
                           output.filePath(legacyName)));
    {
        const QJsonObject manifest = readManifest(output.path());
        const QJsonObject packEntry = findByName(
            manifest.value(QStringLiteral("packs")).toArray(),
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
    EXPECT_FALSE(QFileInfo::exists(output.filePath(legacyName)));

    const QJsonObject manifest = readManifest(output.path());
    const QJsonArray packs = manifest.value(QStringLiteral("packs")).toArray();
    EXPECT_TRUE(findByName(packs, QStringLiteral("BeforeName")).isEmpty());
    const QJsonObject renamedEntry = findByName(packs, QStringLiteral("AfterName"));
    EXPECT_EQ(renamedEntry.value(QStringLiteral("packStableId")),
              beforeEntry.value(QStringLiteral("packStableId")));
    EXPECT_EQ(renamedEntry.value(QStringLiteral("dependencies")),
              beforeEntry.value(QStringLiteral("dependencies")));
    EXPECT_TRUE(renamedEntry.value(QStringLiteral("path")).toString().endsWith(
        afterFileName));
    EXPECT_TRUE(QFileInfo::exists(output.filePath(afterFileName)));

    project.actions.front().rootItem->children.front()->name = QStringLiteral("afterName");
    const auto caseOnly = exporter.ExportAllPacks(project, output.path(), opts);
    ASSERT_TRUE(caseOnly.success) << caseOnly.error.toStdString();
    const auto caseEntry = findByName(readManifest(output.path()).value(QStringLiteral("packs")).toArray(),
                                      QStringLiteral("afterName"));
    EXPECT_EQ(caseEntry.value(QStringLiteral("packStableId")), beforeEntry.value(QStringLiteral("packStableId")));
    EXPECT_EQ(caseEntry.value(QStringLiteral("path")).toString(), QStringLiteral("afterName.bytes"));
    EXPECT_TRUE(QFileInfo::exists(output.filePath(QStringLiteral("afterName.bytes"))));

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, InvalidActionPackNamesPreservePublishedFiles)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
    EXPECT_EQ(readManifest(outputDir).value(QStringLiteral("packs")).toArray().size(), 3);
    EXPECT_TRUE(QJsonDocument::fromJson(readFileBytes(QDir(outputDir).filePath(
        QStringLiteral("HarkenEditorWaveforms.json")))).isObject());
    action->name = QString(VNX::NameValidator::MAX_NAME_LENGTH, QLatin1Char('A'));
    const auto longest = exporter.ExportAllPacks(project, outputDir, options);
    ASSERT_TRUE(longest.success) << longest.error.toStdString();
    expectCanonicalPackParsesWithPartition(QDir(outputDir).filePath(action->name + QStringLiteral(".bytes")));
    registry.clear();
}

TEST(EditorProjectPackExporterManifest, UntargetedUnitsStayOutOfPlatformManifest)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

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

    VNX::PackGroup group;
    group.name = QStringLiteral("AndroidBundle");
    group.actionIds = {harkentest::StudioObjectId(21 + kStableIdBias)};
    group.allPlatforms = false;
    group.platformIds = {HarkenTest::platformIdString("Android")};
    project.packGroups.push_back(group);

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.selection.actionTargeting.push_back(
        {harkentest::StudioObjectId(23 + kStableIdBias), false, {HarkenTest::platformIdString("Android")}});
    opts.platform = HarkenTest::platformIdString("Windows");

    auto result = exporter.ExportAllPacks(project, dir.path(), opts);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QJsonObject manifest = readManifest(dir.path());
    const QJsonArray packs = manifest.value(QStringLiteral("packs")).toArray();

    EXPECT_TRUE(findByName(packs, QStringLiteral("AndroidBundle")).isEmpty());
    EXPECT_TRUE(findByName(packs, QStringLiteral("LooseDropped")).isEmpty());
    const QJsonObject keptPack = findByName(packs, QStringLiteral("LooseKept"));
    EXPECT_EQ(keptPack.value(QStringLiteral("packKind")).toInt(), 3);
    EXPECT_TRUE(findByName(packs, QStringLiteral("Init")).value(
        QStringLiteral("packKind")).toInt() == 2);

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, ActionNameCollisionFailsBeforePublishing)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

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
        HarkenEditor::Bridge::packManifestFileName())));

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, ManifestCommitFailureFailsWholeExport)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

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
        HarkenEditor::Bridge::packManifestFileName());
    ASSERT_TRUE(QDir().mkpath(manifestPath));

    VNX::ProjectPackExporter exporter;
    const auto result = exporter.ExportAllPacks(project, outputDir, {.platform = HarkenTest::platformIdString("Windows")});

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("manifest"),
                                      Qt::CaseInsensitive))
        << result.error.toStdString();
    EXPECT_TRUE(QFileInfo(manifestPath).isDir());

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, UnsupportedBusPropertiesKeepPublishedPackSetUnchanged)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
    ASSERT_TRUE(published.contains(HarkenEditor::Bridge::packIndexFileName()));
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
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

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
    ASSERT_NO_FATAL_FAILURE(expectWaveformMatchesFinalPcm(firstDir.path(), firstDir.path()));
    ASSERT_TRUE(exporter.ExportAllPacks(project, firstDir.path(), opts).success);
    ASSERT_NO_FATAL_FAILURE(expectWaveformMatchesFinalPcm(firstDir.path(), firstDir.path()));
    ASSERT_TRUE(exporter.ExportAllPacks(project, secondDir.path(), opts).success);
    QDir a(firstDir.path());
    QDir b(secondDir.path());
    const QStringList names = a.entryList({"*.bytes", "HarkenPackManifest.json", "HarkenEditorWaveforms.json"}, QDir::Files);
    ASSERT_TRUE(names.contains(QStringLiteral("HarkenEditorWaveforms.json")));
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

TEST(EditorProjectPackExporterManifest, ProductionWaveformGoldenIsRepeatable)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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

    const QString requested = QString::fromUtf8(qgetenv("HARKEN_WAVEFORM_GOLDEN_OUT"));
    const QString outputRoot = requested.isEmpty()
        ? QDir(source.path()).filePath(QStringLiteral("golden-output")) : requested;
    ASSERT_FALSE(QFileInfo::exists(outputRoot));
    const QString contentName = variantFileName(project, harkentest::StudioObjectId(4));
    QStringList expectedNames{QStringLiteral("Init.bytes"), contentName,
        QStringLiteral("HarkenPackManifest.json"), QStringLiteral("HarkenEditorWaveforms.json")};
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
        EXPECT_EQ(output.entryList({"*.bytes", "HarkenPackManifest.json", "HarkenEditorWaveforms.json"},
            QDir::Files, QDir::Name), expectedNames);
        ASSERT_NO_FATAL_FAILURE(expectInitHasVolumeOnlyKind24(output.filePath(QStringLiteral("Init.bytes"))));
        ASSERT_NO_FATAL_FAILURE(expectWaveformMatchesFinalPcm(directory, directory));
        const auto manifest = readManifest(directory);
        const auto packs = manifest.value(QStringLiteral("packs")).toArray();
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
            EXPECT_EQ(header.formatVersion, 4u);
            ASSERT_NO_FATAL_FAILURE(expectCanonicalPackParsesWithPartition(path));
        }
        const auto content = findByName(packs, QStringLiteral("Play_Golden"));
        ASSERT_FALSE(content.isEmpty());
        ASSERT_NO_FATAL_FAILURE(expectPackRendersInIndependentSession(directory,
            output.filePath(QStringLiteral("Init.bytes")), output.filePath(contentName),
            content.value(QStringLiteral("packStableId")).toString().toULongLong(), harkentest::StudioObjectId(4)));
        const auto metadata = QJsonDocument::fromJson(readFileBytes(output.filePath(
            QStringLiteral("HarkenEditorWaveforms.json")))).object();
        const auto actions = metadata.value(QStringLiteral("actions")).toArray();
        ASSERT_EQ(actions.size(), 1);
        const auto waveform = metadata.value(QStringLiteral("sources")).toArray().at(0).toObject();
        EXPECT_EQ(waveform.value(QStringLiteral("frameCount")).toString(), QStringLiteral("3"));
        EXPECT_EQ(waveform.value(QStringLiteral("sampleRate")).toInt(), 48000);
        const auto channels = waveform.value(QStringLiteral("channels")).toArray();
        ASSERT_EQ(channels.size(), 2);
        const QJsonArray left{0.0, 0.5, -1.0};
        const QJsonArray right{1.0, -0.25, 0.0};
        EXPECT_EQ(channels[0].toObject().value(QStringLiteral("min")).toArray(), left);
        EXPECT_EQ(channels[0].toObject().value(QStringLiteral("max")).toArray(), left);
        EXPECT_EQ(channels[1].toObject().value(QStringLiteral("min")).toArray(), right);
        EXPECT_EQ(channels[1].toObject().value(QStringLiteral("max")).toArray(), right);
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
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

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
        readManifest(dir.path()).value(QStringLiteral("packs")).toArray(),
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
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

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
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

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
        {VNX::ProjectItem::Type::Piece,
         harken::playableproperties::ContentPropertyKindV1::Piece},
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
            if (spec.content.type == VNX::ProjectItem::Type::Piece) {
                track = makeItem(
                    QStringLiteral("PieceTrack_%1").arg(spec.objectId),
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

TEST(EditorProjectPackExporterManifest, SchemaV1ContentPacksSortByPackStableId)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

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

    const QJsonArray packs = readManifest(dir.path()).value(QStringLiteral("packs")).toArray();
    ASSERT_EQ(packs.size(), 3);
    EXPECT_EQ(packs.at(0).toObject().value(QStringLiteral("name")).toString(),
              QStringLiteral("Init"));

    const std::uint64_t earlyId = fnvUtf8(HarkenTest::platformIdString("Windows") + QStringLiteral("/action_3da91a64-b947-4560-0000-0000000186b5"));
    const std::uint64_t lateId = fnvUtf8(HarkenTest::platformIdString("Windows") + QStringLiteral("/action_3da91a64-b947-4560-0000-0000000186b7"));
    ASSERT_GT(earlyId, lateId);

    QVector<std::uint64_t> contentIds;
    for (int i = 1; i < packs.size(); ++i) {
        const QJsonObject row = packs.at(i).toObject();
        if (row.value(QStringLiteral("packKind")).toInt() != 3)
            continue;
        contentIds.push_back(row.value(QStringLiteral("packStableId")).toString().toULongLong());
    }
    ASSERT_EQ(contentIds.size(), 2);
    EXPECT_EQ(contentIds[0], lateId);
    EXPECT_EQ(contentIds[1], earlyId);
    EXPECT_LE(contentIds[0], contentIds[1]);

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, LaterExportPreservesUnownedFiles)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

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

    const QJsonArray packs = readManifest(outputDir).value(QStringLiteral("packs")).toArray();
    QSet<QString> named;
    named.insert(HarkenEditor::Bridge::packManifestFileName());
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

TEST(EditorProjectPackExporterManifest, LegacyMigrationReplacesOutputsAndCleansUnusedFiles)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
        const QJsonObject native = readManifest(live.path());
        ASSERT_NO_FATAL_FAILURE(renamePublicationToLegacy(live, wrapped));
        const QString oldInit = wrapped ? QStringLiteral("Init.pack.bytes") : QStringLiteral("Init.pack");
        writeFileBytes(live.filePath("unrelated.bytes"), "other");
        writeFileBytes(live.filePath("arbitrary.pack"), "other-pack");
        writeFileBytes(live.filePath(oldInit + ".meta"), "guid");
        writeFileBytes(live.filePath("Init.bytes"), "conflict");
        const auto migrated = exporter.ExportAllPacks(project, live.path(), opts);
        ASSERT_TRUE(migrated.success) << migrated.error.toStdString();
        EXPECT_EQ(readManifest(live.path()), native);
        EXPECT_FALSE(QFile::exists(live.filePath(oldInit)));
        EXPECT_FALSE(QFile::exists(live.filePath("unrelated.bytes")));
        EXPECT_FALSE(QFile::exists(live.filePath("arbitrary.pack")));
        EXPECT_FALSE(QFile::exists(live.filePath(oldInit + ".meta")));
    }
    registry.clear();
}

TEST(EditorProjectPackExporterManifest, DotPrefixedLegacyPackIsRetiredAfterActionExport)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
        const auto expected = readManifest(live.path());
        auto legacy = expected;
        auto packs = legacy.value(QStringLiteral("packs")).toArray();
        for (int i = 0; i < packs.size(); ++i) {
            auto pack = packs[i].toObject();
            if (pack.value(QStringLiteral("name")).toString() != QStringLiteral("One")) continue;
            ASSERT_TRUE(QFile::rename(live.filePath(pack.value(QStringLiteral("path")).toString()),
                                     live.filePath(QStringLiteral(".Ambient.bytes"))));
            pack.insert(QStringLiteral("name"), QStringLiteral(".Ambient"));
            pack.insert(QStringLiteral("path"), QStringLiteral(".Ambient.bytes"));
            packs[i] = pack;
        }
        legacy.insert(QStringLiteral("packs"), packs);
        ASSERT_NO_FATAL_FAILURE(writeFileBytes(live.filePath(HarkenEditor::Bridge::packManifestFileName()),
                                               QJsonDocument(legacy).toJson()));
        ASSERT_NO_FATAL_FAILURE(renamePublicationToLegacy(live, wrapped));
        const auto migrated = exporter.ExportAllPacks(project, live.path(), opts);
        ASSERT_TRUE(migrated.success) << migrated.error.toStdString();
        EXPECT_EQ(readManifest(live.path()), expected);
        EXPECT_TRUE(QFileInfo::exists(live.filePath(QStringLiteral("One.bytes"))));
        EXPECT_FALSE(QFileInfo::exists(live.filePath(QStringLiteral(".Ambient.bytes"))));
        EXPECT_FALSE(QFileInfo::exists(live.filePath(wrapped ? ".Ambient.pack.bytes" : ".Ambient.pack")));
    }
    registry.clear();
}

TEST(EditorProjectPackExporterManifest, ExportsPlayableDemoSampleFixtures)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
        ASSERT_TRUE(VNX::ProjectPackExporter::WritePackIndex(root, project.platforms, &error)) << error.toStdString();
    }
    const QString output = VNX::ProjectPackExporter::platformOutputDir(first.path(), project.platforms.front());
    const QJsonObject manifest = readManifest(output);
    const QJsonObject gameplay = manifest.value(QStringLiteral("gameplayCatalog")).toObject();
    ASSERT_EQ(gameplay.value(QStringLiteral("schema")).toInt(), 2);
    const QJsonArray rows = gameplay.value(QStringLiteral("actions")).toArray();
    ASSERT_EQ(rows.size(), names.size());
    QSet<QString> owners;
    for (const QString& name : names) {
        const QJsonObject row = findByName(rows, name);
        ASSERT_FALSE(row.isEmpty());
        EXPECT_FALSE(QUuid(row.value(QStringLiteral("actionStableId")).toString()).isNull());
        EXPECT_FALSE(row.contains(QStringLiteral("objectGuid")));
        owners.insert(row.value(QStringLiteral("ownerPackStableId")).toString());
    }
    EXPECT_EQ(owners.size(), 6);
    const QJsonArray variablesJson = manifest.value(QStringLiteral("variableCatalog")).toObject()
        .value(QStringLiteral("variables")).toArray();
    ASSERT_EQ(variablesJson.size(), 1);
    EXPECT_EQ(variablesJson[0].toObject().value(QStringLiteral("codeName")).toString(), QStringLiteral("Demo_Choice"));
    EXPECT_EQ(QUuid(variablesJson[0].toObject().value(QStringLiteral("variableId")).toString()), variable->guid);
    QStringList relativeFiles{QStringLiteral("HarkenPackIndex.json")};
    const QString platformRelative = QDir(first.path()).relativeFilePath(output);
    relativeFiles << platformRelative + QStringLiteral("/HarkenPackManifest.json")
                  << platformRelative + QStringLiteral("/HarkenEditorWaveforms.json");
    const QJsonArray packs = manifest.value(QStringLiteral("packs")).toArray();
    ASSERT_EQ(packs.size(), 7);
    for (const QJsonValue& pack : packs)
        relativeFiles << platformRelative + QLatin1Char('/') + pack.toObject().value(QStringLiteral("path")).toString();
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
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

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
    EXPECT_EQ(readManifest(dir.path()).value(QStringLiteral("gameplayCatalog")).toObject()
        .value(QStringLiteral("actions")).toArray().first().toObject()
        .value(QStringLiteral("ownerPackStableId")).toString(), QString::number(englishId));
    EXPECT_EQ(readManifest(dir.path()).value(QStringLiteral("language")).toString(), project.defaultLanguage);

    const QJsonArray packs = readManifest(dir.path()).value(QStringLiteral("packs")).toArray();
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

    std::uint64_t previousDigest = 0;
    for (const QString& language : project.projectLanguages) {
        const QJsonObject selected = readManifest(QDir(dir.path()).filePath(language));
        EXPECT_EQ(selected.value(QStringLiteral("language")).toString(), language);
        const QJsonArray selectedPacks = selected.value(QStringLiteral("packs")).toArray();
        ASSERT_EQ(selectedPacks.size(), 2);
        const QJsonObject selectedInit = findByName(selectedPacks, QStringLiteral("Init"));
        EXPECT_EQ(selectedInit.value(QStringLiteral("path")).toString(),
                  QStringLiteral("Init.bytes"));
        expectInitHasVolumeOnlyKind24(
            QDir(dir.path()).filePath(selectedInit.value(QStringLiteral("path")).toString()));
        const QJsonObject gameplay = selected.value(QStringLiteral("gameplayCatalog")).toObject();
        EXPECT_EQ(gameplay.value(QStringLiteral("schema")).toInt(), 2);
        const QJsonArray catalog = gameplay.value(QStringLiteral("actions")).toArray();
        ASSERT_EQ(catalog.size(), 1);
        const QJsonObject action = catalog[0].toObject();
        EXPECT_FALSE(QUuid(action.value(QStringLiteral("actionStableId")).toString()).isNull());
        EXPECT_FALSE(action.contains(QStringLiteral("objectGuid")));
        const auto owner = language == QStringLiteral("English") ? englishId : jaId;
        EXPECT_EQ(catalog[0].toObject().value(QStringLiteral("ownerPackStableId")).toString(),
                  QString::number(owner));
        const auto digest = selected.value(QStringLiteral("gameplayCatalog")).toObject()
            .value(QStringLiteral("digest")).toString().toULongLong();
        EXPECT_NE(digest, previousDigest);
        previousDigest = digest;

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
        ASSERT_NO_FATAL_FAILURE(bindPublishedCatalog(selected, initRow.handle));
        const auto* before = session.Engine()->packComposition.Published();
        ASSERT_NE(before, nullptr);
        EXPECT_EQ(before->gameplayCatalog.header.digest, digest);
        members[0].expectedHandle = initRow.handle;
        const QByteArray wrongPath = QDir(dir.path()).filePath(
            language == QStringLiteral("English") ? jaName : englishName).toUtf8();
        members[1].structSize = sizeof(members[1]);
        members[1].packKind = VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
        members[1].packStableId = language == QStringLiteral("English") ? jaId : englishId;
        members[1].utf8Path = wrongPath.constData();
        VnCanonicalPackSetMemberResultV1 refused[2]{};
        for (auto& row : refused)
            row.structSize = sizeof(row);
        VnCanonicalPackSetActivationReportV1 rejection{};
        EXPECT_EQ(VN_PACK_STALE_REVISION, harkentest::ActivateCanonicalMembersV1(
            members, 2, refused, rejection, initial.initHandle, initial.resultSetRevision));
        EXPECT_EQ(refused[0].handle, 0u);
        EXPECT_EQ(refused[1].handle, 0u);
        EXPECT_EQ(before, session.Engine()->packComposition.Published());
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
        EXPECT_EQ(active->gameplayCatalog.header.digest, digest);
    }
    preserveUnityFixture(dir.path());

    std::map<QString, QByteArray> publication;
    for (const QString& language : project.projectLanguages) {
        for (const QString& name : {HarkenEditor::Bridge::packManifestFileName(),
                                   QFileInfo(englishName).fileName(), QStringLiteral("HarkenEditorWaveforms.json")}) {
            const QString relative = language + QLatin1Char('/') + name;
            publication.emplace(relative, readFileBytes(QDir(dir.path()).filePath(relative)));
        }
    }
    QTemporaryDir repeatedDir;
    ASSERT_TRUE(repeatedDir.isValid());
    const auto repeated = exporter.ExportAllPacks(project, repeatedDir.path(), opts);
    ASSERT_TRUE(repeated.success) << repeated.error.toStdString();
    for (const auto& [relative, bytes] : publication)
        EXPECT_EQ(readFileBytes(QDir(repeatedDir.path()).filePath(relative)), bytes) << relative.toStdString();

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, LanguageTargetingPublishesIndependentPlayableCatalogs)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
        const auto manifest = readManifest(QDir(output).filePath(language));
        const auto catalog = manifest.value("gameplayCatalog").toObject().value("actions").toArray();
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
        EXPECT_EQ(readManifest(QDir(macOutput).filePath(language))
            .value("gameplayCatalog").toObject().value("actions").toArray().size(), 3);
    opts.selection.actionTargeting.push_back({first, false, {mac}, QStringLiteral("English")});
    opts.currentWorldOutputPath = dir.filePath(QStringLiteral("Selected.world"));
    opts.selfCleaning = true;
    const auto fallback = exporter.ExportAllPacks(project, output, opts);
    ASSERT_TRUE(fallback.success) << fallback.error.toStdString();
    EXPECT_EQ(project.defaultLanguage, QStringLiteral("English"));
    EXPECT_EQ(readManifest(output).value("language").toString(), QStringLiteral("ja"));
    EXPECT_FALSE(QFile::exists(QDir(output).filePath("English/FirstVoice.bytes")));
    EXPECT_TRUE(QFile::exists(opts.currentWorldOutputPath));
    EXPECT_NE(fallback.currentWorldSemanticFingerprint, 0u);

    opts.selection.actionTargeting.clear();
    opts.onlyPackPaths = {QStringLiteral("ja/SecondVoice")};
    const auto selected = exporter.ExportAllPacks(project, output, opts);
    ASSERT_TRUE(selected.success) << selected.error.toStdString();
    const auto packs = readManifest(output).value("packs").toArray();
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
    const auto dependencyManifest = readManifest(output);
    const auto dependencyPacks = dependencyManifest.value("packs").toArray();
    ASSERT_EQ(dependencyPacks.size(), 3);
    EXPECT_EQ(findByName(dependencyPacks, "FirstVoice").value("language").toString(), QStringLiteral("ja"));
    EXPECT_EQ(findByName(dependencyPacks, "SecondVoice").value("language").toString(), QStringLiteral("ja"));
    {
        harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
        ASSERT_EQ(session.InitResult(), VN_OK);
        ASSERT_NO_FATAL_FAILURE(activateExportedNames(output, {"FirstVoice", "SecondVoice"}, "ja"));
        EXPECT_EQ(VN_OK, Vn_Update());
    }
    opts.selection.actionTargeting.push_back({second, false, {mac}, QStringLiteral("ja")});
    const auto excludedDependency = exporter.ExportAllPacks(project, output, opts);
    EXPECT_FALSE(excludedDependency.success);
    EXPECT_TRUE(excludedDependency.error.contains(QStringLiteral("excluded")))
        << excludedDependency.error.toStdString();
    EXPECT_EQ(readManifest(output), dependencyManifest);
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, SelectedNonLocalizedActionsAreSelfContained)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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

    const QJsonObject manifest = readManifest(outputDir);
    EXPECT_TRUE(manifest.value(QStringLiteral("language")).toString().isEmpty());
    const QJsonArray packs = manifest.value(QStringLiteral("packs")).toArray();
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

TEST(EditorProjectPackExporterManifest, LocalizedActionKeepsNonLocalizedDependencyPath)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
    ASSERT_TRUE(VNX::ProjectPackExporter::WritePackIndex(dir.path(), {*VNX::ProjectPlatformSemantics::find(project.platforms, QUuid(opts.platform))}, &error))
        << error.toStdString();
    const QJsonObject index = QJsonDocument::fromJson(readFileBytes(
        QDir(dir.path()).filePath(HarkenEditor::Bridge::packIndexFileName()))).object();
    const QJsonArray platforms = index.value(QStringLiteral("platforms")).toArray();
    ASSERT_EQ(platforms.size(), 1);
    EXPECT_EQ(platforms.at(0).toObject().value(QStringLiteral("path")).toString(),
              QStringLiteral("Windows"));

    const QJsonArray packs = readManifest(outputDir).value(QStringLiteral("packs")).toArray();
    ASSERT_EQ(packs.size(), 4);
    EXPECT_TRUE(findByName(packs, QStringLiteral("Shared")).isEmpty());
    const QJsonObject common = findByName(packs, QStringLiteral("Common"));
    ASSERT_FALSE(common.isEmpty());
    EXPECT_EQ(common.value(QStringLiteral("path")).toString(),
              QStringLiteral("Common.bytes"));
    EXPECT_TRUE(common.value(QStringLiteral("language")).toString().isEmpty());
    const QString commonId = QString::number(fnvUtf8(HarkenTest::platformIdString("Windows") + QStringLiteral("/action_3da91a64-b947-4560-0000-0000000186b6")));
    EXPECT_EQ(common.value(QStringLiteral("packStableId")).toString(), commonId);
    QByteArray commonBytes;
    harken::PackCanonicalImage commonImage;
    harken::CurrentWorldBodyImage commonWorld;
    ASSERT_NO_FATAL_FAILURE(readCanonicalWorld(
        QDir(outputDir).filePath(common.value(QStringLiteral("path")).toString()),
        commonBytes, commonImage, commonWorld));
    ASSERT_EQ(commonWorld.ProductionView()->actionCount, 1u);
    EXPECT_EQ(commonWorld.ProductionView()->actions[0].actionStableId, harkentest::StudioObjectId(22 + kStableIdBias));
    ASSERT_EQ(commonImage.media.size(), 1u);
    EXPECT_EQ(commonImage.media.front().soundId, harkentest::StudioObjectId(kSharedSoundObjectId + kStableIdBias));
    QSet<QString> packIds;
    for (const QJsonValue& value : packs)
        packIds.insert(value.toObject().value(QStringLiteral("packStableId")).toString());
    int spokenCount = 0;
    for (const QJsonValue& value : packs) {
        const QJsonObject pack = value.toObject();
        const QJsonArray dependencies = pack.value(QStringLiteral("dependencies")).toArray();
        for (const QJsonValue& dependency : dependencies)
            EXPECT_TRUE(packIds.contains(dependency.toString()));
        const QString relative = pack.value(QStringLiteral("path")).toString();
        EXPECT_TRUE(QFileInfo::exists(QDir(outputDir).filePath(relative)));
        if (pack.value(QStringLiteral("name")).toString() != QStringLiteral("Spoken"))
            continue;
        ++spokenCount;
        EXPECT_TRUE(dependencies.contains(commonId));
        const QString language = pack.value(QStringLiteral("language")).toString();
        EXPECT_EQ(relative, language + QStringLiteral("/Spoken.bytes"));
        QByteArray bytes;
        harken::PackCanonicalImage image;
        harken::CurrentWorldBodyImage world;
        ASSERT_NO_FATAL_FAILURE(readCanonicalWorld(
            QDir(outputDir).filePath(relative), bytes, image, world));
        ASSERT_EQ(world.ProductionView()->actionCount, 1u);
        EXPECT_EQ(world.ProductionView()->actions[0].actionStableId, harkentest::StudioObjectId(21 + kStableIdBias));
        ASSERT_EQ(world.ProductionView()->actions[0].stepCount, 2u);
        EXPECT_EQ(world.ProductionView()->actions[0].steps[1].target.stableId,
                  commonWorld.ProductionView()->actions[0].actionStableId);
        EXPECT_EQ(world.ProductionView()->actions[0].steps[1].target.expectedProgramSemanticFingerprint,
                  commonWorld.ProductionView()->actions[0].programSemanticFingerprint);
        ASSERT_EQ(image.media.size(), 1u);
        EXPECT_EQ(image.media.front().soundId, harkentest::StudioObjectId(kSoloSoundObjectId + kStableIdBias));
    }
    EXPECT_EQ(spokenCount, 2);
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, MixedLanguageVariantsOwnMatchingDefinitionsAndMedia)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
    for (const QString& platform : platforms) {
        VNX::ProjectPackExporter::ExportOptions opts;
        opts.platform = HarkenTest::platformIdString(platform);
        const QString outputDir = VNX::ProjectPackExporter::platformOutputDir(dir.path(), *VNX::ProjectPlatformSemantics::find(project.platforms, HarkenTest::platformId(platform)));
        const auto result = exporter.ExportAllPacks(project, outputDir, opts);
        ASSERT_TRUE(result.success) << result.error.toStdString();
        const QJsonArray packs = readManifest(outputDir).value(QStringLiteral("packs")).toArray();
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
            const QJsonObject selected = readManifest(QDir(outputDir).filePath(language));
            const QJsonObject selectedInit = findByName(selected.value(QStringLiteral("packs")).toArray(), QStringLiteral("Init"));
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
    ASSERT_TRUE(VNX::ProjectPackExporter::WritePackIndex(dir.path(), project.platforms, &error))
        << error.toStdString();
    const QJsonObject index = QJsonDocument::fromJson(readFileBytes(
        QDir(dir.path()).filePath(HarkenEditor::Bridge::packIndexFileName()))).object();
    EXPECT_EQ(index.value(QStringLiteral("platforms")).toArray().size(), 2);
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, RuntimeBundlePreservesLanguagePathsThroughDeploy)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
            ASSERT_NO_FATAL_FAILURE(expectWaveformMatchesFinalPcm(QDir(platformRoot).filePath(language), platformRoot));
            const QJsonObject selected = readManifest(QDir(platformRoot).filePath(language));
            EXPECT_EQ(selected.value(QStringLiteral("language")).toString(), language);
            const auto selectedInit = findByName(selected.value(QStringLiteral("packs")).toArray(), QStringLiteral("Init"));
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

TEST(EditorProjectPackExporterManifest, RuntimeBundleDissolvesLegacyGroupsAndKeepsAllActionsAvailable)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
    VNX::PackGroup group;
    group.name = QStringLiteral("Bundle");
    group.actionIds = {harkentest::StudioObjectId(21 + kStableIdBias),
                       harkentest::StudioObjectId(22 + kStableIdBias)};
    project.packGroups.push_back(group);
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
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
        QDir(outputDir).filePath(HarkenEditor::Bridge::packManifestFileName())));
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, LocalizedSinglePackRequiresExplicitLanguage)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
        QDir(outputDir).filePath(HarkenEditor::Bridge::packManifestFileName())));
    opts.language = QStringLiteral("ja");
    const QString path = QDir(dir.path()).filePath(QStringLiteral("single.bytes"));
    result = exporter.ExportActionPack(project, QStringLiteral("Spoken"), path, opts);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("source active is not configured for %1/ja").arg(opts.platform)))
        << result.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(path));
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, LegacyMixedGroupKeepsCommonAndLocalizedDefinitionsInIndependentPacks)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
    VNX::PackGroup group;
    group.name = QStringLiteral("Dialogue");
    group.actionIds = {harkentest::StudioObjectId(21 + kStableIdBias),
                       harkentest::StudioObjectId(22 + kStableIdBias)};
    project.packGroups.push_back(group);
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const QString outputDir = QDir(dir.path()).filePath(QStringLiteral("out"));
    const auto result = exporter.ExportAllPacks(project, outputDir, opts);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    QDirIterator packs(outputDir, {"*.bytes"}, QDir::Files, QDirIterator::Subdirectories);
    EXPECT_TRUE(packs.hasNext());
    EXPECT_TRUE(findByName(readManifest(outputDir).value(QStringLiteral("packs")).toArray(),
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
        const auto selected = readManifest(QDir(outputDir).filePath(language));
        const auto owner = findByName(selected.value(QStringLiteral("packs")).toArray(), QStringLiteral("Mixed"));
        ASSERT_FALSE(owner.isEmpty());
        const auto catalog = selected.value(QStringLiteral("gameplayCatalog")).toObject()
                                 .value(QStringLiteral("actions")).toArray();
        ASSERT_EQ(catalog.size(), 2);
        EXPECT_EQ(findByName(catalog, QStringLiteral("Mixed")).value(QStringLiteral("ownerPackStableId")),
                  owner.value(QStringLiteral("packStableId")));
        EXPECT_NE(findByName(catalog, QStringLiteral("Common")).value(QStringLiteral("ownerPackStableId")),
                  owner.value(QStringLiteral("packStableId")));
    }
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, InvalidRequestedLanguageWritesNoPacks)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
            QDir(outputDir).filePath(HarkenEditor::Bridge::packManifestFileName())));
    }
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, SelectedPostActionChainWithoutLanguagesWritesNoPacks)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
        QDir(outputDir).filePath(HarkenEditor::Bridge::packManifestFileName())));
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterManifest, InterruptedPublishRestoresOwnedFilesAndPreservesNeighbors)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
        writeFileBytes(marker, legacy ? QByteArray() : QJsonDocument(transaction).toJson());
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
    const QStringList previousNames{HarkenEditor::Bridge::packIndexFileName(),
        QStringLiteral("OldPlatform/pack.archive"), QStringLiteral("OldPlatform/pack.archive.meta")};
    for (const auto& name : previousNames)
        writeFileBytes(previous.filePath(name), "previous");
    writeFileBytes(root.filePath("SelectedPlatform/current.bytes"), "unchanged");
    const QStringList nextNames{HarkenEditor::Bridge::packIndexFileName()};
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
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
    const auto manifest = readFileBytes(live.filePath(HarkenEditor::Bridge::packManifestFileName()));
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
    EXPECT_EQ(readFileBytes(live.filePath(HarkenEditor::Bridge::packManifestFileName())), manifest);
    EXPECT_TRUE(live.entryList(QDir::Dirs | QDir::NoDotAndDotDot | QDir::Hidden).isEmpty());
    registry.clear();
}

TEST(EditorProjectPackExporterManifest, FailedInterruptedRecoveryKeepsSnapshotAndStopsPublish)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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

TEST(EditorProjectPackExporterManifest, IncrementalPublicationPreservesUnchangedFilesAndRepairsInvalidPacks)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
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
    for (const auto& name : {"Init.bytes", "One.bytes", "Two.bytes"}) {
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
    opts.selection.actionTargeting = {{harkentest::StudioObjectId(22 + kStableIdBias), false, {}}};
    const auto deselected = exporter.ExportAllPacks(project, live.path(), opts);
    ASSERT_TRUE(deselected.success) << deselected.error.toStdString();
    EXPECT_TRUE(findByName(readManifest(live.path()).value("packs").toArray(), "Two").isEmpty());
    EXPECT_EQ(QFileInfo(live.filePath("Two.bytes")).lastModified(), timestamp);
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
    writeFileBytes(live.filePath("Two.bytes"), "invalid");
    ASSERT_TRUE(QFile::remove(live.filePath("One.bytes")));
    const auto repaired = exporter.ExportAllPacks(project, live.path(), opts);
    ASSERT_TRUE(repaired.success) << repaired.error.toStdString();
    EXPECT_EQ(repaired.builtPackCount, 2);
    EXPECT_EQ(readFileBytes(live.filePath("Two.bytes")), validTwo);
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
    opts.forceReexport = true;
    const auto forced = exporter.ExportAllPacks(project, live.path(), opts);
    ASSERT_TRUE(forced.success) << forced.error.toStdString();
    EXPECT_EQ(forced.builtPackCount, 3);
    EXPECT_EQ(forced.skippedPackCount, 0);
    EXPECT_NE(QFileInfo(live.filePath("Init.bytes")).lastModified(), timestamp);
    harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
    ASSERT_EQ(session.InitResult(), VN_OK);
    ASSERT_NO_FATAL_FAILURE(activateExportedNames(live.path(), {QStringLiteral("One"), QStringLiteral("Two")}));
    registry.clear();
}

TEST(EditorProjectPackExporterManifest, PublicationSelectionReconcilesTheWholeOutputDirectory)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& registry = VNX::AssetRegistry::instance();
    registry.clear();
    registry.setProjectRoot(dir.path());
    const auto tone = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.2f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), tone, tone,
        {{QStringLiteral("Remove"), 21, kSharedSoundObjectId},
         {QStringLiteral("Keep"), 22, kSharedSoundObjectId}}));
    project.packBuildOutputDir = dir.filePath("exports");
    VNX::ProjectPackExporter exporter;
    const auto platform = HarkenTest::platformIdString("Windows");
    for (const auto& id : {platform, HarkenTest::platformIdString("Android")}) {
        const auto result = exporter.ExportAllPacks(project, {}, {.platform = id});
        ASSERT_TRUE(result.success) << result.error.toStdString();
    }
    ASSERT_TRUE(VNX::ProjectPackExporter::FinalizePackExport(project.packBuildOutputDir,
        project.platforms, {platform, HarkenTest::platformIdString("Android")}, false).success);
    const QDir live(QDir(project.packBuildOutputDir).filePath("Windows"));
    const QDir other(QDir(project.packBuildOutputDir).filePath("Android"));
    const auto otherManifest = readFileBytes(other.filePath(HarkenEditor::Bridge::packManifestFileName()));
    const auto keptBytes = readFileBytes(live.filePath("Keep.bytes"));
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
    EXPECT_TRUE(findByName(readManifest(live.path()).value("packs").toArray(), "Remove").isEmpty());
    const auto retained = VNX::ProjectPackExporter::FinalizePackExport(project.packBuildOutputDir,
        project.platforms, {platform}, false);
    ASSERT_TRUE(retained.success) << retained.error.toStdString();
    const auto indexPath = QDir(project.packBuildOutputDir).filePath(HarkenEditor::Bridge::packIndexFileName());
    const auto index = readFileBytes(indexPath);
    const auto selectedPlatforms = QJsonDocument::fromJson(index).object().value("platforms").toArray();
    ASSERT_EQ(selectedPlatforms.size(), 1);
    EXPECT_EQ(selectedPlatforms.first().toObject().value("id").toString(), platform);
    EXPECT_EQ(readFileBytes(other.filePath(HarkenEditor::Bridge::packManifestFileName())), otherManifest);
    EXPECT_TRUE(QFileInfo::exists(QDir(project.packBuildOutputDir).filePath("notes.txt")));
    const auto manifest = readFileBytes(live.filePath(HarkenEditor::Bridge::packManifestFileName()));
    opts.cancelCheck = [] { return true; };
    const auto canceled = exporter.ExportAllPacks(project, {}, opts);
    EXPECT_TRUE(canceled.canceled);
    EXPECT_TRUE(QFileInfo::exists(live.filePath("Remove.bytes")));
    EXPECT_EQ(readFileBytes(live.filePath(HarkenEditor::Bridge::packManifestFileName())), manifest);
    opts.cancelCheck = {};
    const auto keepPath = live.filePath("Keep.bytes");
    ASSERT_TRUE(QFile::remove(keepPath));
    const auto invalid = VNX::ProjectPackExporter::FinalizePackExport(project.packBuildOutputDir,
        project.platforms, {platform}, true);
    EXPECT_FALSE(invalid.success);
    EXPECT_EQ(readFileBytes(indexPath), index);
    EXPECT_TRUE(QFileInfo::exists(live.filePath("Remove.bytes")));
    EXPECT_EQ(readFileBytes(other.filePath(HarkenEditor::Bridge::packManifestFileName())), otherManifest);
    writeFileBytes(keepPath, keptBytes);
    const auto cleaned = VNX::ProjectPackExporter::FinalizePackExport(project.packBuildOutputDir,
        project.platforms, {platform}, true);
    ASSERT_TRUE(cleaned.success) << cleaned.error.toStdString();
    EXPECT_EQ(cleaned.removedPackCount, 4);
    EXPECT_FALSE(QFileInfo::exists(live.filePath("Remove.bytes")));
    EXPECT_EQ(readFileBytes(live.filePath("Keep.bytes")), keptBytes);
    EXPECT_FALSE(QFileInfo::exists(live.filePath("Remove.bytes.meta")));
    EXPECT_FALSE(QFileInfo::exists(live.filePath("unrelated.bytes")));
    EXPECT_FALSE(other.exists());
    for (const auto& name : {"DeletedPlatform", "notes.txt", "empty"})
        EXPECT_FALSE(QFileInfo::exists(QDir(project.packBuildOutputDir).filePath(name)));
    {
        harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
        ASSERT_EQ(session.InitResult(), VN_OK);
        ASSERT_NO_FATAL_FAILURE(activateExportedNames(live.path(), {QStringLiteral("Keep")}));
        EXPECT_EQ(session.Engine()->packComposition.Published()->gameplayCatalog.active.size(), 1u);
    }
    const auto platformDefinition = std::find_if(project.platforms.begin(), project.platforms.end(),
        [&](const auto& value) { return value.id == QUuid(platform); });
    ASSERT_NE(platformDefinition, project.platforms.end());
    platformDefinition->name = QStringLiteral("Desktop");
    ASSERT_TRUE(exporter.ExportAllPacks(project, {}, opts).success);
    const auto renamed = VNX::ProjectPackExporter::FinalizePackExport(project.packBuildOutputDir,
        project.platforms, {platform}, true);
    ASSERT_TRUE(renamed.success) << renamed.error.toStdString();
    EXPECT_FALSE(live.exists());
    const QDir renamedLive(QDir(project.packBuildOutputDir).filePath("Desktop"));
    EXPECT_TRUE(QFileInfo::exists(renamedLive.filePath("Keep.bytes")));
    opts.selection.actionTargeting.push_back({harkentest::StudioObjectId(22 + kStableIdBias), false, {}});
    ASSERT_TRUE(exporter.ExportAllPacks(project, {}, opts).success);
    const auto empty = VNX::ProjectPackExporter::FinalizePackExport(project.packBuildOutputDir,
        project.platforms, {platform}, true);
    ASSERT_TRUE(empty.success) << empty.error.toStdString();
    EXPECT_EQ(empty.removedPackCount, 1);
    EXPECT_EQ(readManifest(renamedLive.path()).value("packs").toArray().size(), 1);
    EXPECT_TRUE(QFileInfo::exists(renamedLive.filePath("Init.bytes")));
    EXPECT_TRUE(readManifest(renamedLive.path()).value("gameplayCatalog").toObject().value("actions").toArray().isEmpty());
    registry.clear();
}

TEST(EditorProjectPackExporterManifest, LanguageCleanupKeepsSharedInitAndValidSelectedManifests)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
    EXPECT_EQ(readManifest(live.path()).value("language").toString(), "ja");
    EXPECT_FALSE(QFileInfo::exists(live.filePath(QStringLiteral("English/") + HarkenEditor::Bridge::packManifestFileName())));
    {
        harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
        ASSERT_EQ(session.InitResult(), VN_OK);
        ASSERT_NO_FATAL_FAILURE(activateExportedNames(live.path(), {QStringLiteral("Spoken"), QStringLiteral("Common")}, QStringLiteral("ja")));
    }
    opts.selection.actionTargeting.push_back({spoken, false, {}, QStringLiteral("ja")});
    const auto finalLanguage = exporter.ExportAllPacks(project, live.path(), opts);
    ASSERT_TRUE(finalLanguage.success) << finalLanguage.error.toStdString();
    EXPECT_EQ(finalLanguage.removedPackCount, 1);
    EXPECT_TRUE(readManifest(live.path()).value("language").toString().isEmpty());
    EXPECT_EQ(readFileBytes(live.filePath("Init.bytes")), init);
    harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
    ASSERT_EQ(session.InitResult(), VN_OK);
    ASSERT_NO_FATAL_FAILURE(activateExportedNames(live.path(), {QStringLiteral("Common")}));
    registry.clear();
}

TEST(EditorProjectPackExporterManifest, PartialPublicationRetainsOtherPacksAndCatalogRecords)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
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
    const auto old = readManifest(live.path());
    QSet<QString> initialKeys;
    for (const auto& artifact : initial.artifacts) initialKeys.insert(artifact.unitKey);
    EXPECT_EQ(initialKeys.size(), old.value("packs").toArray().size());
    ASSERT_FALSE(initialRecords.empty());
    for (const auto& record : initialRecords) EXPECT_GE(record.elapsedMs, 0);
    const auto oldTwo = readFileBytes(live.filePath("Two.bytes"));
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
        EXPECT_EQ(QFileInfo(live.filePath("Two.bytes")).lastModified(), oldTime);
        EXPECT_EQ(readFileBytes(live.filePath("Init.bytes")), oldInit);
        const auto manifest = readManifest(live.path());
        QSet<QString> publishedPaths;
        for (const auto& value : manifest.value("packs").toArray())
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
        EXPECT_EQ(artifactKeys, initialKeys);
        ASSERT_FALSE(records.empty());
        EXPECT_LT(records.size(), static_cast<std::size_t>(partial.artifacts.size()));
        for (const auto& record : records) EXPECT_GE(record.elapsedMs, 0);
        EXPECT_EQ(findByName(manifest.value("packs").toArray(), "Two"), findByName(old.value("packs").toArray(), "Two"));
        EXPECT_EQ(findByName(manifest.value("gameplayCatalog").toObject().value("actions").toArray(), "Two"),
            findByName(old.value("gameplayCatalog").toObject().value("actions").toArray(), "Two"));
        harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
        ASSERT_EQ(session.InitResult(), VN_OK);
        ASSERT_NO_FATAL_FAILURE(activateExportedNames(live.path(), {QStringLiteral("One"), QStringLiteral("Two")}));
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
    EXPECT_EQ(QFileInfo(live.filePath("Two.bytes")).lastModified(), oldTime);
    EXPECT_EQ(readFileBytes(live.filePath("Init.bytes")), oldInit);
    EXPECT_EQ(QFileInfo(live.filePath("Init.bytes")).lastModified(), oldInitTime);
    const auto manifest = readManifest(live.path());
    for (const auto& name : {"Two", "Init"})
        EXPECT_EQ(findByName(manifest.value("packs").toArray(), name),
            findByName(old.value("packs").toArray(), name));
    EXPECT_EQ(findByName(manifest.value("gameplayCatalog").toObject().value("actions").toArray(), "Two"),
        findByName(old.value("gameplayCatalog").toObject().value("actions").toArray(), "Two"));
    actions.back()->comp<VNX::VAction>()->steps.front().targetObjectID = kSoloSoundObjectId;
    ASSERT_TRUE(QFile::remove(registry.getAsset(privateTone)->absolutePath));
    const auto unfinishedDraft = exporter.ExportAllPacks(project, live.path(), opts);
    ASSERT_TRUE(unfinishedDraft.success) << unfinishedDraft.error.toStdString();
    for (const auto& warning : unfinishedDraft.warnings)
        EXPECT_TRUE(initial.warnings.contains(warning)) << warning.toStdString();
    EXPECT_EQ(unfinishedDraft.builtPackCount, 1);
    EXPECT_EQ(readFileBytes(live.filePath("Two.bytes")), oldTwo);
    EXPECT_EQ(QFileInfo(live.filePath("Two.bytes")).lastModified(), oldTime);
    EXPECT_EQ(readFileBytes(live.filePath("Init.bytes")), oldInit);
    registry.clear();
}

TEST(EditorProjectPackExporterManifest, PartialPublicationKeepsUnselectedDependencies)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
    const auto manifest = readManifest(live.path());
    const auto provider = readFileBytes(live.filePath("Provider.bytes"));
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
    EXPECT_EQ(QFileInfo(live.filePath("Provider.bytes")).lastModified(), providerTime);
    EXPECT_NE(readFileBytes(live.filePath("Consumer.bytes")), consumer);
    EXPECT_EQ(readFileBytes(live.filePath("Init.bytes")), init);
    EXPECT_EQ(findByName(readManifest(live.path()).value("packs").toArray(), "Provider"),
        findByName(manifest.value("packs").toArray(), "Provider"));
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
    EXPECT_TRUE(findByName(readManifest(selectedLive.path()).value("packs").toArray(), "Provider").isEmpty());
    registry.clear();
}

TEST(EditorProjectPackExporterManifest, CancelAfterMediaPreparationKeepsPreviousSet)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
    const QStringList names = {
        QStringLiteral("Init.bytes"),
        QStringLiteral("English/") + HarkenEditor::Bridge::packManifestFileName(),
        QStringLiteral("ja/") + HarkenEditor::Bridge::packManifestFileName(),
        variantFileName(project, harkentest::StudioObjectId(21 + kStableIdBias), QStringLiteral("English")),
        variantFileName(project, harkentest::StudioObjectId(21 + kStableIdBias), QStringLiteral("ja")),
        HarkenEditor::Bridge::packManifestFileName()};
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
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
    for (const QString& language : project.projectLanguages) {
        for (const QString& name : {HarkenEditor::Bridge::packManifestFileName(),
                                   QStringLiteral("HarkenEditorWaveforms.json")}) {
            const QString path = QDir(outputDir).filePath(language + QLatin1Char('/') + name);
            previousPublications.emplace(path, readFileBytes(path));
        }
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
    const QString blocker = QDir(outputDir).filePath(
        HarkenEditor::Bridge::packManifestFileName());
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
