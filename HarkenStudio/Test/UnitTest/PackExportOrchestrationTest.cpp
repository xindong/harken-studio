#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Support/FieldGroupTestUtils.h"
#include "Support/ProjectPlatformTestUtils.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/PathUtils.h"
#include "Components/VAction.h"
#include "Components/VAudio.h"
#include "Components/VMixNode.h"
#include "Components/VVariable.h"
#include "ItemTypeCatalog.h"
#include "PackBuildPlan.h"
#include "BindingSemantics.h"
#include <QDirIterator>
#include "PlayablePropertyAuthoringV1.h"
#include "ProjectPackExporter.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"

#include "harken/Harken.h"
#include "harken/core/CurrentWorldCodec.h"
#include "harken/media/PackFormat.h"
#include "harken/media/PackFormatV2.h"
#include "harken/media/PackLogicImage.h"

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <cstdint>
#include <functional>
#include <memory>
#include <map>
#include <set>
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

struct PackImage
{
    QByteArray bytes;
    harken::PackV2Header header;
    std::vector<harken::PackV2DirectoryEntry> entries;

    const uint8_t* data() const
    {
        return reinterpret_cast<const uint8_t*>(bytes.constData());
    }

    int countKind(uint32_t kind) const
    {
        int count = 0;
        for (const auto& entry : entries)
            if (entry.kind == kind)
                ++count;
        return count;
    }
};

void readPack(const QString& path, PackImage& out)
{
    QFile file(path);
    ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    out.bytes = file.readAll();
    ASSERT_GE(out.bytes.size(), static_cast<qsizetype>(harken::kPackV2HeaderBytes));
    ASSERT_TRUE(harken::ParsePackV2Header(out.data(), &out.header));
    out.entries.clear();
    for (uint32_t i = 0; i < out.header.entryCount; ++i)
        out.entries.push_back(harken::ParsePackV2DirectoryEntry(
            out.data() + out.header.directoryOffset +
            i * harken::kPackV2DirectoryEntryBytes));
}

std::set<harkenstudio::ObjectId> readMediaSoundIds(const PackImage& image)
{
    std::set<harkenstudio::ObjectId> ids;
    for (const auto& entry : image.entries)
        if (entry.kind == harken::kPackEntryKindMedia)
            ids.insert(harkentest::ToStudio(harken::ParsePackTocEntry(image.data() + entry.offset).soundId));
    return ids;
}

VnResult loadPack(const QString& path, VnPackHandle* outHandle)
{
    VnPackDesc desc{};
    desc.structSize = sizeof(VnPackDesc);
    const QByteArray utf8 = QFileInfo(path).absoluteFilePath().toUtf8();
    desc.utf8Path = utf8.constData();
    return Vn_LoadPack(&desc, outHandle);
}

class RuntimeSession
{
public:
    explicit RuntimeSession(const VnConfig& config)
        : initResult_(Vn_Init(&config))
    {
    }

    ~RuntimeSession()
    {
        if (initResult_ == VN_OK)
            Vn_Term();
    }

    VnResult initResult() const { return initResult_; }

private:
    VnResult initResult_;
};

QJsonObject readManifest(const QString& outputDir)
{
    QFile file(QDir(outputDir).filePath(
        HarkenEditor::Bridge::packManifestFileName()));
    EXPECT_TRUE(file.open(QIODevice::ReadOnly));
    const QJsonDocument document = QJsonDocument::fromJson(file.readAll());
    EXPECT_TRUE(document.isObject());
    return document.object();
}

QJsonObject findNamedObject(const QJsonArray& array, const QString& name)
{
    for (const QJsonValue& value : array)
    {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("name")).toString() == name)
            return object;
    }
    return {};
}

bool pathsContainFileName(const QJsonArray& paths, const QString& fileName)
{
    for (const QJsonValue& value : paths)
        if (QFileInfo(value.toString()).fileName() == fileName)
            return true;
    return false;
}

QStringList manifestLoadPlan(const QString& outputDir, const QString& actionName)
{
    QStringList paths;
    const QJsonObject manifest = readManifest(outputDir);
    const QJsonArray packs = manifest.value(QStringLiteral("packs")).toArray();
    const QJsonObject initPack = findNamedObject(packs, QStringLiteral("Init"));
    EXPECT_FALSE(initPack.isEmpty());
    paths.append(QDir(outputDir).filePath(
        QFileInfo(initPack.value(QStringLiteral("path")).toString()).fileName()));
    const QJsonObject sharedPack = findNamedObject(packs, QStringLiteral("Shared"));
    if (!sharedPack.isEmpty())
        paths.append(QDir(outputDir).filePath(
            QFileInfo(sharedPack.value(QStringLiteral("path")).toString()).fileName()));
    const QJsonObject action = findNamedObject(
        manifest.value(QStringLiteral("gameplayCatalog")).toObject()
            .value(QStringLiteral("actions")).toArray(), actionName);
    QJsonObject actionPack;
    for (const auto& value : packs) {
        const auto pack = value.toObject();
        if (pack.value(QStringLiteral("packStableId")) == action.value(QStringLiteral("ownerPackStableId")))
            actionPack = pack;
    }
    EXPECT_FALSE(actionPack.isEmpty());
    for (const QJsonValue& value : actionPack.value(QStringLiteral("dependencies")).toArray()) {
        const QString token = value.toString();
        bool knownPack = false;
        for (const QJsonValue& packValue : packs) {
            const QJsonObject pack = packValue.toObject();
            if (pack.value(QStringLiteral("packStableId")).toString() == token ||
                pack.value(QStringLiteral("path")).toString() == token) {
                const QString fileName =
                    QFileInfo(pack.value(QStringLiteral("path")).toString()).fileName();
                if (!paths.contains(QDir(outputDir).filePath(fileName)))
                    paths.append(QDir(outputDir).filePath(fileName));
                knownPack = true;
            }
        }
        if (!knownPack && token.endsWith(QStringLiteral(".bytes")))
            paths.append(QDir(outputDir).filePath(QFileInfo(token).fileName()));
    }
    paths.append(QDir(outputDir).filePath(
        QFileInfo(actionPack.value(QStringLiteral("path")).toString()).fileName()));
    return paths;
}

void loadManifestPlan(const QStringList& paths, std::vector<VnPackHandle>& handles)
{
    for (const QString& path : paths)
    {
        VnPackHandle handle = 0;
        ASSERT_EQ(VN_PACK_INIT_LIFECYCLE, loadPack(path, &handle)) << path.toStdString();
        handles.push_back(handle);
    }
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

QString variantPackPath(const VNX::ProjectData& project, const QString& dir, harkenstudio::ObjectId stableId,
                        const QString& platform = QStringLiteral("Windows"),
                        const QString& language = QString())
{
    (void)platform;
    for (const auto& unit : VNX::ProjectPackExporter::CollectActionPackUnits(project))
        if (unit.stableId == stableId)
            return QDir(dir).filePath(VNX::PackBuildPlanning::packFileName(
                language, unit.name, QStringLiteral(".bytes")));
    ADD_FAILURE() << "Action not found in test project";
    return {};
}

uint32_t registerToneAsset(const QString& projectRoot,
                           const QString& relativePath,
                           float amplitude)
{
    constexpr uint16_t kChannels = 1;
    constexpr uint32_t kSampleRate = 48000;
    constexpr uint32_t kFrames = 96;

    std::vector<float> samples(kFrames, 0.0f);
    for (uint32_t i = 0; i < kFrames; ++i)
        samples[i] = amplitude * static_cast<float>(i % 13) / 16.0f;

    const QString audioRoot = VNX::PathUtils::getAudioSourcesDir(projectRoot);
    const QString sourcePath = QDir(audioRoot).filePath(relativePath);
    writeWavF32(sourcePath, samples, kChannels, kSampleRate);

    VNX::AssetInfo info;
    info.relativePath = relativePath;
    info.absolutePath = sourcePath;
    info.sourceID = VNX::AssetRegistry::instance().allocateStableID();
    EXPECT_NE(info.sourceID, 0u);
    info.metadata.sampleRate = kSampleRate;
    info.metadata.channels = kChannels;
    info.metadata.bitDepth = 32;
    info.metadata.totalSamples = kFrames;
    info.metadata.fileSize = static_cast<uint64_t>(QFileInfo(sourcePath).size());
    info.lastModified = static_cast<uint64_t>(
        QFileInfo(sourcePath).lastModified().toSecsSinceEpoch());
    EXPECT_TRUE(VNX::AssetRegistry::instance().registerAsset(info));
    return info.sourceID;
}

constexpr uint32_t kMasterBusObjectId = 201;
constexpr uint32_t kRootLayerObjectId = 10;
constexpr uint32_t kSoundAObjectId = 1;
constexpr uint32_t kSoundBObjectId = 2;
constexpr uint32_t kSoundCObjectId = 3;
constexpr uint32_t kActionOneObjectId = 21;
constexpr uint32_t kActionTwoObjectId = 22;
constexpr uint32_t kActionThreeObjectId = 23;
constexpr uint64_t kStableIdBias = 100000;

struct SoundSpec
{
    QString name;
    uint32_t objectID;
    uint32_t sourceID;
};

struct ActionSpec
{
    QString name;
    uint32_t objectID;
    std::vector<uint32_t> targetObjectIDs;
    // Non-zero appends a PostAction step whose target is the object id of
    // another Action (the "trigger another Action" step), authored the same
    // way the editor does: same targetObjectID field a Play step uses.
    uint32_t postActionTargetObjectID = 0;
};

void buildProject(VNX::ProjectData& project,
                  const QString& projectRoot,
                  const std::vector<SoundSpec>& soundSpecs,
                  const std::vector<ActionSpec>& actionSpecs)
{
    project.name = QStringLiteral("orchestration");
    project.projectPath =
        QDir(projectRoot).filePath(QStringLiteral("orchestration.vproj"));
    project.conversionFailurePolicy = VNX::ConversionFailurePolicy::AbortBuild;

    std::set<uint32_t> seenSources;
    for (const SoundSpec& spec : soundSpecs)
    {
        if (!seenSources.insert(spec.sourceID).second) continue;
        const VNX::AssetInfo* info =
            VNX::AssetRegistry::instance().getAsset(spec.sourceID);
        ASSERT_NE(info, nullptr);
        VNX::AudioSourceData sourceData;
        sourceData.sourceID = spec.sourceID;
        sourceData.relativePath = info->relativePath;
        sourceData.language = QStringLiteral("SFX");
        project.audioSources.push_back(sourceData);
    }

    auto master = makeItem(QStringLiteral("MasterBus"), VNX::ProjectItem::Type::Bus,
                           kMasterBusObjectId);
    master->guid = VNX::authoringUuid(harkentest::StudioObjectId(0x4d4153544552));
    VNX::ProjectFileBinding busBinding;
    busBinding.rootItem = master;
    project.busses.push_back(std::move(busBinding));

    auto layer = makeItem(QStringLiteral("Root"), VNX::ProjectItem::Type::Layer,
                          kRootLayerObjectId);
    layer->fieldGroups = VNX::MakeRootFieldGroups(VNX::toFieldGroupGuid(master->guid));
    for (const SoundSpec& spec : soundSpecs)
    {
        auto sound = makeItem(spec.name, VNX::ProjectItem::Type::Sound, spec.objectID);
        HarkenTest::setSharedSoundSource(*sound, spec.sourceID);
        sound->comp<VNX::VMixNode>()->outputBusId = kMasterBusObjectId;
        addChild(layer, sound);
    }
    VNX::ProjectFileBinding layerBinding;
    layerBinding.rootItem = layer;
    project.layers.push_back(std::move(layerBinding));

    auto actionsRoot = makeItem(QStringLiteral("ActionsRoot"),
                                VNX::ProjectItem::Type::None, 20);
    for (const ActionSpec& spec : actionSpecs)
    {
        auto action = makeItem(spec.name, VNX::ProjectItem::Type::Action, spec.objectID);
        for (uint32_t targetObjectID : spec.targetObjectIDs)
        {
            VNX::VActionStep play;
            play.type = VNX::VActionStep::Type::Play;
            play.targetObjectID = targetObjectID;
            action->comp<VNX::VAction>()->steps.push_back(play);
        }
        if (spec.postActionTargetObjectID != 0)
        {
            VNX::VActionStep post;
            post.type = VNX::VActionStep::Type::PostAction;
            post.targetObjectID = spec.postActionTargetObjectID;
            action->comp<VNX::VAction>()->steps.push_back(post);
        }
        addChild(actionsRoot, action);
    }
    VNX::ProjectFileBinding actionBinding;
    actionBinding.rootItem = actionsRoot;
    project.actions.push_back(std::move(actionBinding));
}

} // namespace

TEST(EditorPackExportOrchestration, ProjectBuildDirectoryControlsArtifacts)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t sourceId = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 0.5f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), {{QStringLiteral("Tone"), kSoundAObjectId, sourceId}},
        {{QStringLiteral("PlayTone"), kActionOneObjectId, {kSoundAObjectId}}}));

    for (const QString& relative : {QString(), QStringLiteral("Exports/AudioPacks")}) {
        project.packBuildOutputDir = relative;
        VNX::ProjectPackExporter exporter;
        VNX::ProjectPackExporter::ExportOptions opts;
        opts.platform = HarkenTest::platformIdString("Windows");
        std::vector<VNX::ProjectPackExporter::PackBuildRecord> records;
        const auto result = exporter.ExportAllPacks(project, QString(), opts, &records);
        ASSERT_TRUE(result.success) << result.error.toStdString();
        ASSERT_FALSE(records.empty());

        const QDir output(dir.filePath(relative.isEmpty() ? QStringLiteral("BuildPacks") : relative));
        EXPECT_TRUE(QFileInfo::exists(output.filePath(QStringLiteral("HarkenPackIndex.json"))));
        for (const auto& record : records) {
            EXPECT_EQ(QFileInfo(record.outputPath).absolutePath(),
                      output.filePath(QStringLiteral("Windows")));
            EXPECT_TRUE(QFileInfo::exists(record.outputPath));
        }
    }
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorPackExportOrchestration, ExportedPacksHonorLoadOrderContract)
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
        project, dir.path(),
        {{QStringLiteral("SoundA"), kSoundAObjectId, sharedSourceId},
         {QStringLiteral("SoundB"), kSoundBObjectId, sharedSourceId},
         {QStringLiteral("SoundC"), kSoundCObjectId, soloSourceId}},
        {{QStringLiteral("ActionOne"), kActionOneObjectId, {kSoundAObjectId}},
         {QStringLiteral("ActionTwo"), kActionTwoObjectId, {kSoundBObjectId}},
         {QStringLiteral("ActionThree"), kActionThreeObjectId, {kSoundCObjectId}}}));

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");

    std::vector<VNX::ProjectPackExporter::PackBuildRecord> records;
    auto result = exporter.ExportAllPacks(project, dir.path(), opts, &records);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    bool initSucceeded = false;
    bool actionOneSucceeded = false;
    const VNX::ProjectPackExporter::PackBuildRecord* actionTwoRecord = nullptr;
    const VNX::ProjectPackExporter::PackBuildRecord* actionThreeRecord = nullptr;
    for (const auto& record : records)
    {
        if (record.packName == QStringLiteral("Init"))
            initSucceeded = record.success;
        EXPECT_NE(record.packName, QStringLiteral("Shared"));
        if (record.packName == QStringLiteral("ActionOne") &&
            record.source == VNX::ProjectPackExporter::PackBuildRecord::Source::Action)
            actionOneSucceeded = record.success;
        if (record.packName == QStringLiteral("ActionTwo") &&
            record.source == VNX::ProjectPackExporter::PackBuildRecord::Source::Action)
            actionTwoRecord = &record;
        if (record.packName == QStringLiteral("ActionThree") &&
            record.source == VNX::ProjectPackExporter::PackBuildRecord::Source::Action)
            actionThreeRecord = &record;
    }
    EXPECT_TRUE(initSucceeded);
    EXPECT_TRUE(actionOneSucceeded);
    ASSERT_NE(actionTwoRecord, nullptr);
    ASSERT_NE(actionThreeRecord, nullptr);
    EXPECT_EQ(actionTwoRecord->objectID, kActionTwoObjectId);
    EXPECT_EQ(actionThreeRecord->objectID, kActionThreeObjectId);

    const QString initPath = QDir(dir.path()).filePath(QStringLiteral("Init.bytes"));
    const QString sharedPath = QDir(dir.path()).filePath(QStringLiteral("Shared.bytes"));
    const QString bundlePath = QDir(dir.path()).filePath(QStringLiteral("ActionOne.bytes"));
    const QString actionTwoPath = QDir(dir.path()).filePath(QStringLiteral("ActionTwo.bytes"));
    const QString actionThreePath =
        QDir(dir.path()).filePath(QStringLiteral("ActionThree.bytes"));
    EXPECT_EQ(actionTwoRecord->outputPath, actionTwoPath);
    EXPECT_EQ(actionThreeRecord->outputPath, actionThreePath);
    EXPECT_EQ(actionTwoRecord->fileSize, QFileInfo(actionTwoPath).size());
    EXPECT_EQ(actionThreeRecord->fileSize, QFileInfo(actionThreePath).size());
    for (const QString& path :
         {initPath, bundlePath, actionTwoPath, actionThreePath})
        ASSERT_TRUE(QFileInfo::exists(path)) << path.toStdString();

    EXPECT_FALSE(QFileInfo::exists(sharedPath));

    PackImage bundleImage;
    ASSERT_NO_FATAL_FAILURE(readPack(bundlePath, bundleImage));
    EXPECT_EQ(readMediaSoundIds(bundleImage),
              (std::set<harkenstudio::ObjectId>{harkentest::StudioObjectId(kSoundAObjectId + kStableIdBias)}));
    EXPECT_EQ(bundleImage.countKind(10u), 0);

    PackImage actionTwoImage;
    ASSERT_NO_FATAL_FAILURE(readPack(actionTwoPath, actionTwoImage));
    EXPECT_EQ(readMediaSoundIds(actionTwoImage),
              (std::set<harkenstudio::ObjectId>{harkentest::StudioObjectId(kSoundBObjectId + kStableIdBias)}));
    EXPECT_EQ(actionTwoImage.countKind(10u), 0);

    PackImage actionThreeImage;
    ASSERT_NO_FATAL_FAILURE(readPack(actionThreePath, actionThreeImage));
    EXPECT_EQ(readMediaSoundIds(actionThreeImage),
              (std::set<harkenstudio::ObjectId>{harkentest::StudioObjectId(kSoundCObjectId + kStableIdBias)}));

    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.deviceMode = VN_DEVICE_NULL;
    RuntimeSession session(config);
    ASSERT_EQ(VN_OK, session.initResult());

    VnPackHandle initHandle = 0;
    ASSERT_EQ(VN_PACK_INIT_LIFECYCLE, loadPack(initPath, &initHandle));

    VnPackHandle bundleHandle = 0;
    EXPECT_EQ(VN_PACK_INIT_LIFECYCLE, loadPack(bundlePath, &bundleHandle));
    VnPackHandle actionTwoHandle = 0;
    EXPECT_EQ(VN_PACK_INIT_LIFECYCLE, loadPack(actionTwoPath, &actionTwoHandle));
    VnPackHandle actionThreeHandle = 0;
    EXPECT_EQ(VN_PACK_INIT_LIFECYCLE, loadPack(actionThreePath, &actionThreeHandle));

    EXPECT_EQ(VN_OK, Vn_Update());

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorPackExportOrchestration, InspectorStepsPersistWithoutLegacyPackCargo)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t sourceId =
        registerToneAsset(dir.path(), QStringLiteral("inspector-steps.wav"), 1.0f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(),
        {{QStringLiteral("Tone"), kSoundAObjectId, sourceId}},
        {{QStringLiteral("Head"), kActionOneObjectId, {kSoundAObjectId}},
         {QStringLiteral("Tail"), kActionTwoObjectId, {kSoundAObjectId}}}));

    const QJsonArray inspectorSteps{
        QJsonObject{{QStringLiteral("type"), QStringLiteral("Play")},
                    {QStringLiteral("targetObjectID"),
                     static_cast<qint64>(kSoundAObjectId)},
                    {QStringLiteral("targetName"), QStringLiteral("Tone")},
                    {QStringLiteral("delay"), 125.0}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("Play")},
                    {QStringLiteral("targetObjectID"), 0},
                    {QStringLiteral("targetName"), QString()},
                    {QStringLiteral("delay"), 500.0}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("Stop")},
                    {QStringLiteral("targetObjectID"),
                     static_cast<qint64>(kSoundAObjectId)},
                    {QStringLiteral("targetName"), QStringLiteral("Tone")},
                    {QStringLiteral("delay"), 250.0},
                    {QStringLiteral("fadeTime"), 75.0}},
        QJsonObject{{QStringLiteral("type"), QStringLiteral("Post Action")},
                    {QStringLiteral("targetObjectID"),
                     static_cast<qint64>(kActionTwoObjectId)},
                    {QStringLiteral("targetName"), QStringLiteral("Tail")},
                    {QStringLiteral("delay"), 375.0}}};

    VNX::VAction authored;
    authored.fromJsonExtra(
        QJsonObject{{QStringLiteral("steps"), inspectorSteps}});
    QJsonObject persisted;
    authored.toJsonExtra(persisted);
    ASSERT_EQ(persisted.value(QStringLiteral("steps")).toArray(), inspectorSteps);

    VNX::VAction restored;
    restored.fromJsonExtra(persisted);
    QJsonObject roundTripped;
    restored.toJsonExtra(roundTripped);
    EXPECT_EQ(roundTripped, persisted);
    for (const QJsonValue& value : roundTripped.value(QStringLiteral("steps")).toArray())
    {
        const QJsonObject step = value.toObject();
        EXPECT_FALSE(step.contains(QStringLiteral("delayJitterMs")));
        EXPECT_FALSE(step.contains(QStringLiteral("probabilityPermille")));
    }

    auto* head = project.actions.front().rootItem->children.front().get();
    ASSERT_NE(head, nullptr);
    head->comp<VNX::VAction>()->steps = restored.steps;

    VNX::ProjectPackExporter exporter;
    const auto refused = exporter.ExportAllPacks(
        project, dir.path(), {.platform = HarkenTest::platformIdString("Windows")});
    EXPECT_FALSE(refused.success);
    EXPECT_TRUE(refused.error.contains(QStringLiteral("Head")))
        << refused.error.toStdString();
    EXPECT_TRUE(refused.error.contains(QStringLiteral("step 1")))
        << refused.error.toStdString();
    EXPECT_TRUE(refused.error.contains(QStringLiteral("target")))
        << refused.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(
        QDir(dir.path()).filePath(QStringLiteral("Head.bytes"))));

    head->comp<VNX::VAction>()->steps.erase(
        head->comp<VNX::VAction>()->steps.begin() + 1);
    const auto result = exporter.ExportAllPacks(
        project, dir.path(), {.platform = HarkenTest::platformIdString("Windows")});
    ASSERT_TRUE(result.success) << result.error.toStdString();
    EXPECT_FALSE(result.warnings.join(QStringLiteral("\n"))
                     .contains(QStringLiteral("no resolvable target")));

    PackImage image;
    ASSERT_NO_FATAL_FAILURE(readPack(
        QDir(dir.path()).filePath(QStringLiteral("Head.bytes")), image));

    EXPECT_EQ(image.countKind(10u), 0);

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorPackExportOrchestration, ActionFailurePreservesPublishedPackSet)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const QString relativePath = QStringLiteral("missing.wav");
    const uint32_t sourceId = registerToneAsset(dir.path(), relativePath, 1.0f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(),
        {{QStringLiteral("SoundA"), kSoundAObjectId, sourceId}},
        {{QStringLiteral("ActionOne"), kActionOneObjectId, {kSoundAObjectId}}}));

    VNX::ProjectPackExporter exporter;
    const auto initial = exporter.ExportAllPacks(
        project, dir.path(), {.platform = HarkenTest::platformIdString("Windows")});
    ASSERT_TRUE(initial.success) << initial.error.toStdString();
    const auto manifest = readManifest(dir.path());
    const QString contentPath = QDir(dir.path()).filePath(QStringLiteral("ActionOne.bytes"));
    PackImage initialImage;
    ASSERT_NO_FATAL_FAILURE(readPack(contentPath, initialImage));
    const QString sourcePath =
        QDir(VNX::PathUtils::getAudioSourcesDir(dir.path())).filePath(relativePath);
    ASSERT_TRUE(QFile::remove(sourcePath));

    const auto result = exporter.ExportAllPacks(
        project, dir.path(), {.platform = HarkenTest::platformIdString("Windows")});
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.isEmpty());
    EXPECT_EQ(readManifest(dir.path()), manifest);
    PackImage preservedImage;
    ASSERT_NO_FATAL_FAILURE(readPack(contentPath, preservedImage));
    EXPECT_EQ(preservedImage.bytes, initialImage.bytes);

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorPackExportOrchestration, EmptyActionRecordsFailureRowAndBatchContinues)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t sourceId =
        registerToneAsset(dir.path(), QStringLiteral("first.wav"), 1.0f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(),
        {{QStringLiteral("SoundA"), kSoundAObjectId, sourceId}},
        {{QStringLiteral("Hollow"), kActionOneObjectId, {}},
         {QStringLiteral("Built"), kActionTwoObjectId, {kSoundAObjectId}}}));

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");

    std::vector<VNX::ProjectPackExporter::PackBuildRecord> records;
    auto result = exporter.ExportAllPacks(project, dir.path(), opts, &records);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const VNX::ProjectPackExporter::PackBuildRecord* hollowRecord = nullptr;
    const VNX::ProjectPackExporter::PackBuildRecord* builtRecord = nullptr;
    for (const auto& record : records)
    {
        if (record.packName == QStringLiteral("Hollow"))
            hollowRecord = &record;
        if (record.packName == QStringLiteral("Built"))
            builtRecord = &record;
    }
    ASSERT_NE(hollowRecord, nullptr);
    EXPECT_EQ(hollowRecord->source,
              VNX::ProjectPackExporter::PackBuildRecord::Source::Action);
    EXPECT_FALSE(hollowRecord->success);
    EXPECT_TRUE(hollowRecord->error.contains(QStringLiteral("no steps")))
        << hollowRecord->error.toStdString();
    EXPECT_EQ(hollowRecord->objectID, kActionOneObjectId);
    EXPECT_EQ(hollowRecord->fileSize, 0);
    ASSERT_NE(builtRecord, nullptr);
    EXPECT_TRUE(builtRecord->success);

    EXPECT_FALSE(QFileInfo::exists(
        variantPackPath(project, dir.path(), harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias))));
    EXPECT_TRUE(QFileInfo::exists(
        variantPackPath(project, dir.path(), harkentest::StudioObjectId(kActionTwoObjectId + kStableIdBias))));

    EXPECT_TRUE(result.warnings.join(QStringLiteral("\n")).contains(
        QStringLiteral("1 action(s) have no steps and were not packed")));

    const QJsonObject manifest = readManifest(dir.path());
    EXPECT_TRUE(findNamedObject(
        manifest.value(QStringLiteral("packs")).toArray(),
        QStringLiteral("Hollow")).isEmpty());
    EXPECT_FALSE(findNamedObject(
        manifest.value(QStringLiteral("packs")).toArray(),
        QStringLiteral("Built")).isEmpty());

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorPackExportOrchestration, ExportActionPackRejectsActionWithoutSteps)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), {},
        {{QStringLiteral("Hollow"), kActionOneObjectId, {}}}));

    const QString packPath = QDir(dir.path()).filePath(QStringLiteral("hollow.pack"));
    VNX::ProjectPackExporter exporter;
    const auto result = exporter.ExportActionPack(
        project, QStringLiteral("Hollow"), packPath, {.platform = HarkenTest::platformIdString("Windows")});
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("has no steps")))
        << result.error.toStdString();
    EXPECT_FALSE(result.error.contains(QStringLiteral("was not found")));
    EXPECT_FALSE(QFileInfo::exists(packPath));

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorPackExportOrchestration, PostActionToEmptyActionIsRejectedBeforePublishing)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t sourceId =
        registerToneAsset(dir.path(), QStringLiteral("head.wav"), 1.0f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(),
        {{QStringLiteral("HeadTone"), kSoundAObjectId, sourceId}},
        {{QStringLiteral("Head"), kActionOneObjectId, {kSoundAObjectId},
          kActionTwoObjectId},
         {QStringLiteral("Hollow"), kActionTwoObjectId, {}}}));

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");

    const auto result = exporter.ExportAllPacks(project, dir.path(), opts);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("PostAction dependency 'Hollow'")))
        << result.error.toStdString();
    EXPECT_TRUE(result.error.contains(QStringLiteral("has no steps")))
        << result.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(
        QDir(dir.path()).filePath(QStringLiteral("Init.bytes"))));

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorPackExportOrchestration, EffectiveObjectLevelSendsAreIncludedWithoutObsoleteWarning)
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
        project, dir.path(),
        {{QStringLiteral("SoundA"), kSoundAObjectId, firstSourceId},
         {QStringLiteral("SoundB"), kSoundBObjectId, secondSourceId}},
        {{QStringLiteral("ActionOne"), kActionOneObjectId, {kSoundAObjectId}},
         {QStringLiteral("ActionTwo"), kActionTwoObjectId, {kSoundBObjectId}}}));

    auto sendBus = makeItem(QStringLiteral("SendBus"), VNX::ProjectItem::Type::Bus, 202);
    addChild(project.busses.front().rootItem, sendBus);
    auto& parentSend = project.layers.front().rootItem->fieldGroups.send;
    parentSend.shared = {VNX::FieldGroupMode::Local,
        VNX::SendFieldGroup{{{VNX::toFieldGroupGuid(sendBus->guid), -3.0f}}}};
    parentSend.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = {VNX::FieldGroupMode::Local,
        VNX::SendFieldGroup{{{VNX::toFieldGroupGuid(sendBus->guid), -6.0f}}}};
    for (auto& child : project.layers.front().rootItem->children) {
        if (child->name == QStringLiteral("SoundA")) {
            child->fieldGroups.send.shared.localHistory = VNX::SendFieldGroup{
                {{VNX::toFieldGroupGuid(QUuid::createUuid()), 0.0f}}};
            child->fieldGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::Windows] = {
                VNX::FieldGroupMode::UseShared, {}};
        } else
            child->fieldGroups.send.shared = {
                VNX::FieldGroupMode::Local, VNX::SendFieldGroup{}};
    }

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");

    std::vector<VNX::ProjectPackExporter::PackBuildRecord> records;
    auto result = exporter.ExportAllPacks(project, dir.path(), opts, &records);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QString joinedWarnings = result.warnings.join(QStringLiteral("\n"));
    EXPECT_FALSE(joinedWarnings.contains(
        QStringLiteral("Object-level sends on 'SoundA'")));
    EXPECT_FALSE(joinedWarnings.contains(QStringLiteral("SoundB")));

    for (const auto& [actionId, soundId] : {
             std::pair{kActionOneObjectId, kSoundAObjectId},
             std::pair{kActionTwoObjectId, kSoundBObjectId}}) {
        PackImage pack;
        ASSERT_NO_FATAL_FAILURE(readPack(
            variantPackPath(project, dir.path(), harkentest::StudioObjectId(actionId + kStableIdBias)), pack));
        harken::PackCanonicalImage canonical;
        ASSERT_EQ(VN_OK, harken::ParsePackCanonicalImage(
            pack.data(), static_cast<uint64_t>(pack.bytes.size()), &canonical));
        ASSERT_TRUE(canonical.hasPartition);
        harken::CurrentWorldBodyImage decoded;
        ASSERT_TRUE(decoded.Decode(canonical.partitionBody.data(), canonical.partitionBody.size(),
            canonical.partitionHeader.bodyVintage, harken::CurrentWorldDecodeMode::StrictArtifact));
        ASSERT_NE(decoded.ProductionView(), nullptr);
        const auto& world = *decoded.ProductionView();
        ASSERT_GT(world.playablePropertyCount, 0u);
        const auto* end = world.playableProperties + world.playablePropertyCount;
        const auto* sound = std::find_if(world.playableProperties, end, [&](const auto& value) {
            return value.stableId == harkentest::StudioObjectId(soundId + kStableIdBias);
        });
        ASSERT_NE(sound, end);
        if (soundId == kSoundAObjectId) {
            ASSERT_EQ(sound->payload.sendCount, 1u);
            EXPECT_EQ(sound->payload.sends[0].targetBusId, sendBus->runtimeId());
            EXPECT_FLOAT_EQ(sound->payload.sends[0].sendLevel, -6.0f);
        } else
            EXPECT_EQ(sound->payload.sendCount, 0u);
    }

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorPackExportOrchestration, BusOnlySendsProduceNoObjectWarning)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t firstSourceId =
        registerToneAsset(dir.path(), QStringLiteral("first.wav"), 1.0f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(),
        {{QStringLiteral("SoundA"), kSoundAObjectId, firstSourceId}},
        {{QStringLiteral("ActionOne"), kActionOneObjectId, {kSoundAObjectId}}}));

    {
        auto* master = project.busses.front().rootItem.get();
        ASSERT_NE(master, nullptr);
        auto aux = makeItem(QStringLiteral("AuxBus"),
                            VNX::ProjectItem::Type::Bus, 202);
        addChild(project.busses.front().rootItem, aux);
        VNX::Send send;
        send.targetBusID = kMasterBusObjectId;
        send.sendLevel = 1.0f;
        aux->comp<VNX::VMixNode>()->sends.push_back(send);
    }

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");

    std::vector<VNX::ProjectPackExporter::PackBuildRecord> records;
    auto result = exporter.ExportAllPacks(project, dir.path(), opts, &records);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QString joinedWarnings = result.warnings.join(QStringLiteral("\n"));
    EXPECT_FALSE(joinedWarnings.contains(QStringLiteral("Object-level sends")));

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorPackExportOrchestration, SharedPlayTargetHasCompleteActionProviders)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t sourceId =
        registerToneAsset(dir.path(), QStringLiteral("shared-container.wav"), 1.0f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(),
        {{QStringLiteral("Tone"), kSoundAObjectId, sourceId}},
        {{QStringLiteral("ActionOne"), kActionOneObjectId, {kSoundAObjectId}},
         {QStringLiteral("ActionTwo"), kActionTwoObjectId, {kSoundAObjectId}}}));

    auto* soundMix = project.layers.front()
                         .rootItem->children.front()
                         ->comp<VNX::VMixNode>();
    soundMix->volume = -6.0f;
    soundMix->pitch = 12.0f;

    VNX::ProjectPackExporter exporter;
    auto result = exporter.ExportAllPacks(project, dir.path(), {.platform = HarkenTest::platformIdString("Windows")});
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QString sharedPath = QDir(dir.path()).filePath(QStringLiteral("Shared.bytes"));
    const QString firstPath = QDir(dir.path()).filePath(QStringLiteral("ActionOne.bytes"));
    const QString secondPath = QDir(dir.path()).filePath(QStringLiteral("ActionTwo.bytes"));

    PackImage firstImage;
    PackImage secondImage;
    EXPECT_FALSE(QFileInfo::exists(sharedPath));
    ASSERT_NO_FATAL_FAILURE(readPack(firstPath, firstImage));
    ASSERT_NO_FATAL_FAILURE(readPack(secondPath, secondImage));
    EXPECT_EQ(firstImage.countKind(harken::kPackEntryKindContainer), 0);
    EXPECT_EQ(firstImage.countKind(harken::kPackEntryKindMedia), 1);
    EXPECT_EQ(secondImage.countKind(harken::kPackEntryKindContainer), 0);
    EXPECT_EQ(secondImage.countKind(harken::kPackEntryKindMedia), 1);
    EXPECT_EQ(firstImage.countKind(10u), 0);
    EXPECT_EQ(secondImage.countKind(10u), 0);

    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.deviceMode = VN_DEVICE_NULL;
    RuntimeSession session(config);
    ASSERT_EQ(VN_OK, session.initResult());

    VnPackHandle initHandle = 0;
    VnPackHandle firstHandle = 0;
    VnPackHandle secondHandle = 0;
    ASSERT_EQ(VN_PACK_INIT_LIFECYCLE, loadPack(QDir(dir.path()).filePath(QStringLiteral("Init.bytes")),
                              &initHandle));
    ASSERT_EQ(VN_PACK_INIT_LIFECYCLE, loadPack(firstPath, &firstHandle));
    ASSERT_EQ(VN_PACK_INIT_LIFECYCLE, loadPack(secondPath, &secondHandle));

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorPackExportOrchestration, UnassignedPostActionLoadsFromManifestTopology)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t firstSourceId =
        registerToneAsset(dir.path(), QStringLiteral("head.wav"), 1.0f);
    const uint32_t secondSourceId =
        registerToneAsset(dir.path(), QStringLiteral("tail.wav"), 0.5f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(),
        {{QStringLiteral("HeadTone"), kSoundAObjectId, firstSourceId},
         {QStringLiteral("TailTone"), kSoundBObjectId, secondSourceId}},
        {{QStringLiteral("Head"), kActionOneObjectId, {kSoundAObjectId},
          kActionTwoObjectId},
         {QStringLiteral("Tail"), kActionTwoObjectId, {kSoundBObjectId}}}));

    VNX::ProjectPackExporter exporter;
    auto result = exporter.ExportAllPacks(project, dir.path(), {.platform = HarkenTest::platformIdString("Windows")});
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QStringList plan = manifestLoadPlan(dir.path(), QStringLiteral("Head"));
    ASSERT_GE(plan.size(), 2);
    EXPECT_EQ(QFileInfo(plan.first()).fileName(), QStringLiteral("Init.bytes"));
    EXPECT_EQ(QFileInfo(plan.last()).fileName(),
              QFileInfo(QDir(dir.path()).filePath(QStringLiteral("Head.bytes"))).fileName());
    const QJsonObject manifest = readManifest(dir.path());
    const QJsonObject headPack = findNamedObject(
        manifest.value(QStringLiteral("packs")).toArray(), QStringLiteral("Head"));
    EXPECT_EQ(headPack.value(QStringLiteral("packKind")).toInt(), 3);
    EXPECT_FALSE(headPack.value(QStringLiteral("dependencies")).toArray().isEmpty());

    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.deviceMode = VN_DEVICE_NULL;
    RuntimeSession session(config);
    ASSERT_EQ(VN_OK, session.initResult());
    std::vector<VnPackHandle> handles;
    ASSERT_NO_FATAL_FAILURE(loadManifestPlan(plan, handles));
    ASSERT_EQ(VN_OK, Vn_Update());

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorPackExportOrchestration, OnlyUnitsExpandsOnlyTransitivePostActionDependencies)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t sourceId =
        registerToneAsset(dir.path(), QStringLiteral("subset.wav"), 1.0f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(),
        {{QStringLiteral("Tone"), kSoundAObjectId, sourceId}},
        {{QStringLiteral("Head"), kActionOneObjectId, {}, kActionTwoObjectId},
         {QStringLiteral("Tail"), kActionTwoObjectId, {kSoundAObjectId}, 24},
         {QStringLiteral("Peer"), kActionThreeObjectId, {kSoundAObjectId}},
         {QStringLiteral("Nested"), 24, {kSoundAObjectId}},
         {QStringLiteral("Unrelated"), 25, {kSoundAObjectId}}}));

    VNX::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    options.onlyUnits.insert(QStringLiteral("Head"));
    VNX::ProjectPackExporter exporter;
    const auto result = exporter.ExportAllPacks(project, dir.path(), options);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    EXPECT_TRUE(QFileInfo::exists(
        variantPackPath(project, dir.path(), harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias))));
    EXPECT_TRUE(QFileInfo::exists(
        variantPackPath(project, dir.path(), harkentest::StudioObjectId(kActionTwoObjectId + kStableIdBias))));
    EXPECT_TRUE(QFileInfo::exists(
        variantPackPath(project, dir.path(), harkentest::StudioObjectId(24 + kStableIdBias))));
    EXPECT_FALSE(QFileInfo::exists(
        variantPackPath(project, dir.path(), harkentest::StudioObjectId(25 + kStableIdBias))));
    EXPECT_FALSE(QFileInfo::exists(
        QDir(dir.path()).filePath(QStringLiteral("Shared.bytes"))));
    const QJsonObject manifest = readManifest(dir.path());
    EXPECT_TRUE(findNamedObject(
        manifest.value(QStringLiteral("packs")).toArray(),
        QStringLiteral("Unrelated")).isEmpty());

    const auto actions = manifest.value(QStringLiteral("gameplayCatalog")).toObject()
                             .value(QStringLiteral("actions")).toArray();
    ASSERT_EQ(actions.size(), 3);
    EXPECT_TRUE(findNamedObject(actions, QStringLiteral("Peer")).isEmpty());
    EXPECT_FALSE(findNamedObject(actions, QStringLiteral("Nested")).isEmpty());
    options.onlyUnits = {QStringLiteral("Tail")};
    const QString output = QDir(dir.path()).filePath(QStringLiteral("Selected"));
    const auto selected = exporter.ExportAllPacks(project, output, options);
    ASSERT_TRUE(selected.success) << selected.error.toStdString();
    EXPECT_EQ(QDir(output).entryList({QStringLiteral("*.bytes")}, QDir::Files),
              (QStringList{QStringLiteral("Init.bytes"), QStringLiteral("Nested.bytes"), QStringLiteral("Tail.bytes")}));
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorPackExportOrchestration, OnlyUnitsRejectsUntargetedDependencyBeforeInit)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t sourceId =
        registerToneAsset(dir.path(), QStringLiteral("targeting.wav"), 1.0f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(),
        {{QStringLiteral("Tone"), kSoundAObjectId, sourceId}},
        {{QStringLiteral("Head"), kActionOneObjectId, {}, kActionTwoObjectId},
         {QStringLiteral("Tail"), kActionTwoObjectId, {kSoundAObjectId}}}));
    project.actionPackTargeting.push_back(
        {harkentest::StudioObjectId(kActionTwoObjectId + kStableIdBias), false, {HarkenTest::platformIdString("Linux")}});

    VNX::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    options.onlyUnits.insert(QStringLiteral("Head"));
    VNX::ProjectPackExporter exporter;
    const auto result = exporter.ExportAllPacks(project, dir.path(), options);
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("excluded from platform")))
        << result.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(
        QDir(dir.path()).filePath(QStringLiteral("Init.bytes"))));

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorPackExportOrchestration, LegacyGroupsExportPostActionDependenciesAsSeparatePacks)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t firstSourceId =
        registerToneAsset(dir.path(), QStringLiteral("group-head.wav"), 1.0f);
    const uint32_t secondSourceId =
        registerToneAsset(dir.path(), QStringLiteral("group-tail.wav"), 0.5f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(),
        {{QStringLiteral("HeadTone"), kSoundAObjectId, firstSourceId},
         {QStringLiteral("TailTone"), kSoundBObjectId, secondSourceId}},
        {{QStringLiteral("Head"), kActionOneObjectId, {kSoundAObjectId},
          kActionTwoObjectId},
         {QStringLiteral("Tail"), kActionTwoObjectId, {kSoundBObjectId}}}));

    VNX::PackGroup sourceGroup;
    sourceGroup.name = QStringLiteral("SourceGroup");
    sourceGroup.actionIds = {harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias)};
    project.packGroups.push_back(sourceGroup);
    VNX::PackGroup targetGroup;
    targetGroup.name = QStringLiteral("TargetGroup");
    targetGroup.actionIds = {harkentest::StudioObjectId(kActionTwoObjectId + kStableIdBias)};
    project.packGroups.push_back(targetGroup);

    VNX::ProjectPackExporter exporter;
    auto result = exporter.ExportAllPacks(project, dir.path(), {.platform = HarkenTest::platformIdString("Windows")});
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QStringList plan = manifestLoadPlan(dir.path(), QStringLiteral("Head"));
    ASSERT_EQ(plan.size(), 3);
    EXPECT_EQ(QFileInfo(plan.first()).fileName(), QStringLiteral("Init.bytes"));
    EXPECT_EQ(QFileInfo(plan.last()).fileName(),
              QFileInfo(QDir(dir.path()).filePath(QStringLiteral("Head.bytes"))).fileName());
    const QJsonObject manifest = readManifest(dir.path());
    const QJsonObject sourcePack = findNamedObject(
        manifest.value(QStringLiteral("packs")).toArray(),
        QStringLiteral("Head"));
    EXPECT_EQ(sourcePack.value(QStringLiteral("packKind")).toInt(), 3);
    EXPECT_FALSE(sourcePack.value(QStringLiteral("dependencies")).toArray().isEmpty());
    EXPECT_EQ(QFileInfo(plan.at(1)).fileName(), QStringLiteral("Tail.bytes"));

    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.deviceMode = VN_DEVICE_NULL;
    RuntimeSession session(config);
    ASSERT_EQ(VN_OK, session.initResult());
    std::vector<VnPackHandle> handles;
    ASSERT_NO_FATAL_FAILURE(loadManifestPlan(plan, handles));
    ASSERT_EQ(VN_OK, Vn_Update());

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorPackExportOrchestration, CrossPackPostActionCycleFailsExport)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t sourceId =
        registerToneAsset(dir.path(), QStringLiteral("cycle.wav"), 1.0f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(),
        {{QStringLiteral("Tone"), kSoundAObjectId, sourceId}},
        {{QStringLiteral("First"), kActionOneObjectId, {kSoundAObjectId},
          kActionTwoObjectId},
         {QStringLiteral("Second"), kActionTwoObjectId, {},
          kActionOneObjectId}}));

    VNX::ProjectPackExporter exporter;
    const auto result = exporter.ExportAllPacks(project, dir.path(), {.platform = HarkenTest::platformIdString("Windows")});
    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("dependency cycle")));
    EXPECT_FALSE(QFileInfo::exists(
        QDir(dir.path()).filePath(QStringLiteral("Init.bytes"))));

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorPackExportOrchestration, InvalidProjectIdentityStopsExportBeforeWritingPacks)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t sourceId =
        registerToneAsset(dir.path(), QStringLiteral("readiness.wav"), 1.0f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(),
        {{QStringLiteral("Tone"), kSoundAObjectId, sourceId}},
        {{QStringLiteral("ActionOne"), kActionOneObjectId, {kSoundAObjectId}}}));

    project.actions.front().rootItem->children.front()->guid = QUuid();
    const QString outputDir = QDir(dir.path()).filePath(QStringLiteral("output"));
    VNX::ProjectPackExporter exporter;
    const auto result = exporter.ExportAllPacks(project, outputDir, {.platform = HarkenTest::platformIdString("Windows")});

    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.isEmpty());
    EXPECT_FALSE(QDir(outputDir).exists());

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorPackExportOrchestration, DependencyTreePrunesByCatalogTruthSet)
{
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});

    auto layerRoot =
        makeItem(QStringLiteral("Root"), VNX::ProjectItem::Type::Layer, 10);
    auto container = makeItem(QStringLiteral("ContainerNode"),
                              VNX::ProjectItem::Type::Container, 11);
    auto track =
        makeItem(QStringLiteral("TrackNode"), VNX::ProjectItem::Type::Track, 12);
    auto piece =
        makeItem(QStringLiteral("PieceNode"), VNX::ProjectItem::Type::Piece, 13);
    auto sound =
        makeItem(QStringLiteral("SoundLeaf"), VNX::ProjectItem::Type::Sound, 14);
    addChild(layerRoot, container);
    addChild(container, track);
    addChild(track, piece);
    addChild(piece, sound);
    VNX::ProjectFileBinding layerBinding;
    layerBinding.rootItem = layerRoot;
    project.layers.push_back(std::move(layerBinding));

    auto busRoot =
        makeItem(QStringLiteral("MasterBus"), VNX::ProjectItem::Type::Bus, 20);
    addChild(busRoot,
             makeItem(QStringLiteral("ChildBus"), VNX::ProjectItem::Type::Bus, 21));
    VNX::ProjectFileBinding busBinding;
    busBinding.rootItem = busRoot;
    project.busses.push_back(std::move(busBinding));

    auto actionsRoot = makeItem(QStringLiteral("ActionsRoot"),
                                VNX::ProjectItem::Type::Action, 30);
    addChild(actionsRoot, makeItem(QStringLiteral("PlayAction"),
                                   VNX::ProjectItem::Type::Action, 31));
    addChild(actionsRoot, makeItem(QStringLiteral("ActionFolder"),
                                   VNX::ProjectItem::Type::None, 32));
    VNX::ProjectFileBinding actionBinding;
    actionBinding.rootItem = actionsRoot;
    project.actions.push_back(std::move(actionBinding));

    auto variablesRoot = makeItem(QStringLiteral("VariablesRoot"),
                                  VNX::ProjectItem::Type::Variable, 40);
    addChild(variablesRoot, makeItem(QStringLiteral("ChildVariable"),
                                     VNX::ProjectItem::Type::Variable, 41));
    VNX::ProjectFileBinding variableBinding;
    variableBinding.rootItem = variablesRoot;
    project.variables.push_back(std::move(variableBinding));

    auto attenuationsRoot = makeItem(QStringLiteral("AttenuationsRoot"),
                                     VNX::ProjectItem::Type::Attenuation, 50);
    addChild(attenuationsRoot, makeItem(QStringLiteral("ChildAttenuation"),
                                        VNX::ProjectItem::Type::Attenuation, 51));
    VNX::ProjectFileBinding attenuationBinding;
    attenuationBinding.rootItem = attenuationsRoot;
    project.attenuationItems.push_back(std::move(attenuationBinding));

    VNX::ProjectPackExporter exporter;
    const auto roots = exporter.GetDependencyTree(project);

    std::vector<QString> allNames;
    std::function<void(const VNX::ProjectPackExporter::DependencyNode&)> collect =
        [&](const VNX::ProjectPackExporter::DependencyNode& node) {
            allNames.push_back(node.name);
            for (const auto& child : node.children)
                collect(child);
        };
    for (const auto& root : roots)
        collect(root);

    for (const QString& pruned :
         {QStringLiteral("ChildBus"), QStringLiteral("ActionFolder"),
          QStringLiteral("ChildVariable"), QStringLiteral("ChildAttenuation")})
        EXPECT_EQ(std::count(allNames.begin(), allNames.end(), pruned), 0)
            << pruned.toStdString();

    ASSERT_EQ(roots.size(), 2u);

    const VNX::ProjectPackExporter::DependencyNode* containerNode = nullptr;
    const VNX::ProjectPackExporter::DependencyNode* actionNode = nullptr;
    for (const auto& root : roots)
    {
        if (root.name == QStringLiteral("ContainerNode"))
            containerNode = &root;
        if (root.name == QStringLiteral("PlayAction"))
            actionNode = &root;
    }

    ASSERT_NE(actionNode, nullptr);
    EXPECT_EQ(actionNode->type,
              VNX::ItemTypeCatalog::canonicalName(VNX::ProjectItem::Type::Action));
    EXPECT_TRUE(actionNode->children.empty());

    ASSERT_NE(containerNode, nullptr);
    EXPECT_EQ(containerNode->type, VNX::ItemTypeCatalog::canonicalName(
                                       VNX::ProjectItem::Type::Container));
    ASSERT_EQ(containerNode->children.size(), 1u);

    const auto& trackNode = containerNode->children.front();
    EXPECT_EQ(trackNode.name, QStringLiteral("TrackNode"));
    EXPECT_EQ(trackNode.type,
              VNX::ItemTypeCatalog::canonicalName(VNX::ProjectItem::Type::Track));
    ASSERT_EQ(trackNode.children.size(), 1u);

    const auto& pieceNode = trackNode.children.front();
    EXPECT_EQ(pieceNode.name, QStringLiteral("PieceNode"));
    EXPECT_EQ(pieceNode.type,
              VNX::ItemTypeCatalog::canonicalName(VNX::ProjectItem::Type::Piece));
    ASSERT_EQ(pieceNode.children.size(), 1u);

    const auto& soundNode = pieceNode.children.front();
    EXPECT_EQ(soundNode.name, QStringLiteral("SoundLeaf"));
    EXPECT_EQ(soundNode.type,
              VNX::ItemTypeCatalog::canonicalName(VNX::ProjectItem::Type::Sound));
    EXPECT_TRUE(soundNode.children.empty());
}

TEST(EditorPackExportOrchestration, QuantizedActionExportAdmitsSupportedAndRejectsUnsupportedAtomically)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t sourceId =
        registerToneAsset(dir.path(), QStringLiteral("unsupported.wav"), 1.0f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(),
        {{QStringLiteral("Tone"), kSoundAObjectId, sourceId}},
        {{QStringLiteral("Quantized"), kActionOneObjectId, {kSoundAObjectId}},
         {QStringLiteral("Posted"), kActionTwoObjectId, {kSoundAObjectId}}}));

    auto* actionItem = project.actions.front().rootItem->children.front().get();
    ASSERT_NE(actionItem, nullptr);
    auto* action = actionItem->comp<VNX::VAction>();
    ASSERT_NE(action, nullptr);

    auto play = [&](VNX::QuantizeTiming timing, uint32_t reference = 0) {
        VNX::VActionStep step;
        step.type = VNX::VActionStep::Type::Play;
        step.targetObjectID = kSoundAObjectId;
        step.targetName = QStringLiteral("Tone");
        step.delay = 17.0f;
        step.quantize.timing = timing;
        step.quantize.referenceObjectID = reference;
        step.quantize.ifSilent = VNX::QuantizeIfSilent::Drop;
        return step;
    };
    auto stop = [&](VNX::QuantizeTiming timing, uint32_t reference = 0) {
        VNX::VActionStep step;
        step.type = VNX::VActionStep::Type::Stop;
        step.targetObjectID = kSoundAObjectId;
        step.targetName = QStringLiteral("Tone");
        step.fadeTime = 23.0f;
        step.quantize.timing = timing;
        step.quantize.referenceObjectID = reference;
        return step;
    };
    auto post = [&](VNX::QuantizeTiming timing, uint32_t reference = 0) {
        VNX::VActionStep step;
        step.type = VNX::VActionStep::Type::PostAction;
        step.targetObjectID = kActionTwoObjectId;
        step.targetName = QStringLiteral("Posted");
        step.delay = 19.0f;
        step.quantize.timing = timing;
        step.quantize.referenceObjectID = reference;
        return step;
    };

    struct SupportedCase
    {
        QString label;
        VNX::VActionStep step;
    };
    const std::vector<SupportedCase> supportedCases = {
        {QStringLiteral("immediate"), play(VNX::QuantizeTiming::Immediate)},
        {QStringLiteral("next-beat"), play(VNX::QuantizeTiming::NextBeat, kSoundAObjectId)},
        {QStringLiteral("next-bar"), play(VNX::QuantizeTiming::NextBar, kSoundAObjectId)},
        {QStringLiteral("next-marker"), play(VNX::QuantizeTiming::NextMarker, kSoundAObjectId)},
        {QStringLiteral("post-immediate"), post(VNX::QuantizeTiming::Immediate)},
        {QStringLiteral("stop-next-bar"), stop(VNX::QuantizeTiming::NextBar, kSoundAObjectId)},
        {QStringLiteral("stop-natural-end"), stop(VNX::QuantizeTiming::NaturalEnd)},
        {QStringLiteral("stop-loop-end"), stop(VNX::QuantizeTiming::CurrentLoopEnd)},
        {QStringLiteral("stop-exit-marker"), stop(VNX::QuantizeTiming::ExitMarker)},
        {QStringLiteral("stop-explicit-exit-marker"),
         stop(VNX::QuantizeTiming::ExitMarker, kSoundAObjectId)},
    };

    VNX::ProjectPackExporter exporter;
    for (const SupportedCase& supported : supportedCases)
    {
        SCOPED_TRACE(supported.label.toStdString());
        action->steps = {supported.step};
        const QString packPath = QDir(dir.path()).filePath(
            supported.label + QStringLiteral(".pack"));
        const auto result = exporter.ExportActionPack(
            project, actionItem->name, packPath,
            {.platform = HarkenTest::platformIdString("Windows")});
        ASSERT_TRUE(result.success) << result.error.toStdString();
        EXPECT_EQ(result.actionCount, 1);
        EXPECT_TRUE(QFileInfo::exists(packPath));
        PackImage image;
        ASSERT_NO_FATAL_FAILURE(readPack(packPath, image));
        EXPECT_EQ(image.header.formatVersion, harken::kPackFormatVersion2);
    }

    action->steps = {
        play(VNX::QuantizeTiming::Immediate),
        play(VNX::QuantizeTiming::NextBeat, kSoundAObjectId),
        play(VNX::QuantizeTiming::NextBar, kSoundAObjectId),
        play(VNX::QuantizeTiming::NextMarker, kSoundAObjectId),
        post(VNX::QuantizeTiming::Immediate),
        stop(VNX::QuantizeTiming::NaturalEnd),
        stop(VNX::QuantizeTiming::CurrentLoopEnd),
        stop(VNX::QuantizeTiming::ExitMarker),
        stop(VNX::QuantizeTiming::ExitMarker, kSoundAObjectId),
    };
    const QString exportAllDir = QDir(dir.path()).filePath(QStringLiteral("export-all"));
    const auto allResult = exporter.ExportAllPacks(
        project, exportAllDir, {.platform = HarkenTest::platformIdString("Windows")});
    ASSERT_TRUE(allResult.success) << allResult.error.toStdString();
    std::map<QString, QByteArray> published;
    QDirIterator publishedFiles(exportAllDir, QDir::Files, QDirIterator::Subdirectories);
    while (publishedFiles.hasNext()) {
        const QString path = publishedFiles.next();
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        published[path] = file.readAll();
    }
    ASSERT_FALSE(published.empty());

    project.packBuildOutputDir = QStringLiteral("production-quantized");
    const auto productionResult = exporter.ExportAllPacks(
        project, QString(), {.platform = HarkenTest::platformIdString("Windows")});
    ASSERT_TRUE(productionResult.success) << productionResult.error.toStdString();
    const QString productionRoot =
        QDir(dir.path()).filePath(project.packBuildOutputDir);
    std::map<QString, QByteArray> productionPublished;
    QDirIterator productionFiles(
        productionRoot, QDir::Files, QDirIterator::Subdirectories);
    while (productionFiles.hasNext()) {
        const QString path = productionFiles.next();
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        productionPublished[path] = file.readAll();
    }
    ASSERT_FALSE(productionPublished.empty());
    EXPECT_TRUE(productionPublished.contains(
        QDir(productionRoot).filePath(QStringLiteral("HarkenPackIndex.json"))));

    struct UnsupportedCase
    {
        QString label;
        VNX::VActionStep step;
        QString diagnostic;
    };
    std::vector<UnsupportedCase> cases;

    cases.push_back({QStringLiteral("entry-marker"),
                     play(VNX::QuantizeTiming::EntryMarker, kSoundAObjectId),
                     QStringLiteral("Entry Marker")});
    cases.push_back({QStringLiteral("play-exit-marker"),
                     play(VNX::QuantizeTiming::ExitMarker, kSoundAObjectId),
                     QStringLiteral("Exit Marker")});
    cases.push_back({QStringLiteral("play-natural-end"),
                     play(VNX::QuantizeTiming::NaturalEnd, kSoundAObjectId),
                     QStringLiteral("timing is not supported")});
    cases.push_back({QStringLiteral("post-next-beat"),
                     post(VNX::QuantizeTiming::NextBeat, kSoundAObjectId),
                     QStringLiteral("timing is not supported")});
    VNX::VActionStep missingReference = play(VNX::QuantizeTiming::NextMarker);
    cases.push_back({QStringLiteral("missing-reference"), missingReference,
                     QStringLiteral("quantize reference is unresolved")});
    VNX::VActionStep wrongReference = play(VNX::QuantizeTiming::NextMarker, UINT32_MAX);
    wrongReference.quantize.referenceName = QStringLiteral("Tone");
    cases.push_back({QStringLiteral("wrong-reference"), wrongReference,
                     QStringLiteral("quantize reference")});
    VNX::VActionStep wrongTarget = play(VNX::QuantizeTiming::Immediate);
    wrongTarget.targetObjectID = UINT32_MAX;
    wrongTarget.targetName = QStringLiteral("MissingTone");
    cases.push_back({QStringLiteral("wrong-target"), wrongTarget,
                     QStringLiteral("target")});

    for (const UnsupportedCase& unsupported : cases)
    {
        action->steps = {
            play(VNX::QuantizeTiming::NextBeat, kSoundAObjectId),
            unsupported.step
        };
        const QString packPath = QDir(dir.path()).filePath(
            unsupported.label + QStringLiteral(".pack"));
        QFile stale(packPath);
        ASSERT_TRUE(stale.open(QIODevice::WriteOnly));
        ASSERT_EQ(stale.write("stale"), 5);
        stale.close();
        const QByteArray before = [&]() {
            QFile file(packPath);
            EXPECT_TRUE(file.open(QIODevice::ReadOnly));
            return file.readAll();
        }();

        const auto result = exporter.ExportActionPack(
            project, actionItem->name, packPath, {.platform = HarkenTest::platformIdString("Windows")});
        EXPECT_FALSE(result.success) << unsupported.label.toStdString();
        EXPECT_TRUE(result.error.contains(QStringLiteral("Quantized")))
            << result.error.toStdString();
        EXPECT_TRUE(result.error.contains(QStringLiteral("step 1")))
            << result.error.toStdString();
        EXPECT_TRUE(result.error.contains(unsupported.diagnostic))
            << result.error.toStdString();
        QFile after(packPath);
        ASSERT_TRUE(after.open(QIODevice::ReadOnly));
        EXPECT_EQ(after.readAll(), before);
    }

    action->steps = {
        play(VNX::QuantizeTiming::NextBeat, kSoundAObjectId),
        play(VNX::QuantizeTiming::EntryMarker, kSoundAObjectId),
    };
    const auto refusedAll = exporter.ExportAllPacks(
        project, exportAllDir, {.platform = HarkenTest::platformIdString("Windows")});
    EXPECT_FALSE(refusedAll.success);
    EXPECT_TRUE(refusedAll.error.contains(QStringLiteral("Quantized")))
        << refusedAll.error.toStdString();
    EXPECT_TRUE(refusedAll.error.contains(QStringLiteral("step 1")))
        << refusedAll.error.toStdString();
    for (const auto& [path, bytes] : published) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
        EXPECT_EQ(file.readAll(), bytes) << path.toStdString();
    }

    const auto refusedProduction = exporter.ExportAllPacks(
        project, QString(), {.platform = HarkenTest::platformIdString("Windows")});
    EXPECT_FALSE(refusedProduction.success);
    std::map<QString, QByteArray> productionAfterFailure;
    QDirIterator productionAfterFiles(
        productionRoot, QDir::Files, QDirIterator::Subdirectories);
    while (productionAfterFiles.hasNext()) {
        const QString path = productionAfterFiles.next();
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        productionAfterFailure[path] = file.readAll();
    }
    EXPECT_EQ(productionAfterFailure, productionPublished);

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorPackExportOrchestration, ExportActionPackWritesOnlyNamedActionClosure)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t firstSourceId =
        registerToneAsset(dir.path(), QStringLiteral("single-first.wav"), 1.0f);
    const uint32_t secondSourceId =
        registerToneAsset(dir.path(), QStringLiteral("single-second.wav"), 0.5f);

    auto project = HarkenTest::projectWithPlatforms({"Windows", "Android", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(),
        {{QStringLiteral("FirstTone"), kSoundAObjectId, firstSourceId},
         {QStringLiteral("SecondTone"), kSoundBObjectId, secondSourceId}},
        {{QStringLiteral("ActionOne"), kActionOneObjectId, {kSoundAObjectId}},
         {QStringLiteral("ActionTwo"), kActionTwoObjectId, {kSoundBObjectId}}}));

    const QString packPath = QDir(dir.path()).filePath(QStringLiteral("single.pack"));
    VNX::ProjectPackExporter exporter;
    auto* firstTone = project.layers.front().rootItem->children.front().get();
    ASSERT_NE(firstTone, nullptr);
    firstTone->comp<VNX::VAudio>()->loopCount = 3;

    const auto result = exporter.ExportActionPack(
        project, QStringLiteral("ActionOne"), packPath, {.platform = HarkenTest::platformIdString("Windows")});
    ASSERT_TRUE(result.success) << result.error.toStdString();
    EXPECT_EQ(result.actionCount, 1);
    EXPECT_EQ(result.objectCount, 1);
    EXPECT_EQ(result.mediaCount, 1);

    PackImage image;
    ASSERT_NO_FATAL_FAILURE(readPack(packPath, image));
    EXPECT_EQ(image.header.formatVersion, harken::kPackFormatVersion2);
    EXPECT_EQ(image.countKind(harken::kPackEntryKindContainer), 0);
    EXPECT_EQ(image.countKind(harken::kPackEntryKindVoiceRecipe), 0);
    EXPECT_EQ(image.countKind(harken::kPackEntryKindMedia), 1);
    EXPECT_EQ(image.countKind(10u), 0);
    EXPECT_EQ(readMediaSoundIds(image),
              std::set<harkenstudio::ObjectId>{harkentest::StudioObjectId(kSoundAObjectId + kStableIdBias)});
    for (const auto& entry : image.entries) {
        if (entry.kind == harken::kPackEntryKindAttenuation) {
            EXPECT_EQ(entry.vintage, harken::kPackV2AuthoredToneSchemaVintage);
            EXPECT_NE(entry.vintage, harken::kPackV2SchemaVintage);
        }
    }

    const QString missingPath =
        QDir(dir.path()).filePath(QStringLiteral("missing.pack"));
    const auto missing = exporter.ExportActionPack(
        project, QStringLiteral("MissingAction"), missingPath, {.platform = HarkenTest::platformIdString("Windows")});
    EXPECT_FALSE(missing.success);
    EXPECT_TRUE(missing.error.contains(QStringLiteral("was not found")));
    EXPECT_FALSE(QFileInfo::exists(missingPath));

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorPackExportOrchestration, ExportActionPackRejectsPlatformExcludedPostAction)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));

    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());

    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());

    const uint32_t sourceId =
        registerToneAsset(dir.path(), QStringLiteral("excluded-tail.wav"), 1.0f);
    auto project = HarkenTest::projectWithPlatforms({"Windows", "Linux"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(),
        {{QStringLiteral("Tone"), kSoundAObjectId, sourceId}},
        {{QStringLiteral("Head"), kActionOneObjectId, {}, kActionTwoObjectId},
         {QStringLiteral("Tail"), kActionTwoObjectId, {kSoundAObjectId}}}));
    project.actionPackTargeting.push_back(
        {harkentest::StudioObjectId(kActionTwoObjectId + kStableIdBias), false,
         {HarkenTest::platformIdString("Linux")}});

    const QString packPath = QDir(dir.path()).filePath(QStringLiteral("head.pack"));
    VNX::ProjectPackExporter exporter;
    const auto result = exporter.ExportActionPack(
        project, QStringLiteral("Head"), packPath,
        {.platform = HarkenTest::platformIdString("Windows")});

    EXPECT_FALSE(result.success);
    EXPECT_TRUE(result.error.contains(QStringLiteral("depends on Action")))
        << result.error.toStdString();
    EXPECT_TRUE(result.error.contains(QStringLiteral("excluded from platform 'Windows'")))
        << result.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(packPath));

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorPackExportOrchestration, VoiceBindingResponsesSurviveCanonicalActionAndSharedExport)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t sourceId = registerToneAsset(dir.path(), QStringLiteral("response.wav"), 1.0f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(),
        {{QStringLiteral("Tone"), kSoundAObjectId, sourceId}},
        {{QStringLiteral("PlayTone"), kActionOneObjectId, {kSoundAObjectId}}}));
    auto variable = makeItem(QStringLiteral("Drive"), VNX::ProjectItem::Type::Variable, 31);
    auto* variableComp = variable->comp<VNX::VVariable>();
    ASSERT_NE(variableComp, nullptr);
    variableComp->kind = VNX::VariableKind::Smooth;
    variableComp->rangeMin = 0.0f;
    variableComp->rangeMax = 1.0f;
    VNX::ProjectFileBinding variableFile;
    variableFile.rootItem = variable;
    project.variables.push_back(std::move(variableFile));
    auto sound = project.layers.front().rootItem->children.front();
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    for (const uint32_t duration : {20u, 0u, 137u}) {
        SCOPED_TRACE(duration);
        auto volume = VNX::BindingSemantics::defaultBinding(
            VNX::BindingSemantics::volumeAddress(), variable->guid);
        auto pitch = VNX::BindingSemantics::defaultBinding(
            VNX::BindingSemantics::pitchAddress(), variable->guid);
        volume.guid = VNX::authoringUuid(harkentest::StudioObjectId(800001));
        pitch.guid = VNX::authoringUuid(harkentest::StudioObjectId(800002));
        for (auto* binding : {&volume, &pitch}) {
            binding->transitionMs = duration;
            auto json = binding->toJson().toObject();
            if (duration == 20u) json.remove(QStringLiteral("transitionMs"));
            *binding = VNX::ParameterBindingDef::fromJson(json);
            ASSERT_TRUE(binding->typed);
            ASSERT_EQ(duration, binding->transitionMs);
        }
        ASSERT_TRUE(sound->comp<VNX::VMixNode>()->bindings.replace({volume, pitch}));
        const QString outputDir = QDir(dir.path()).filePath(QString::number(duration));
        const auto actionResult = exporter.ExportAllPacks(project, outputDir, options);
        ASSERT_TRUE(actionResult.success) << actionResult.error.toStdString();
        const QString sharedPath = QDir(outputDir).filePath(QStringLiteral("explicit-shared.pack"));
        const auto sharedResult = exporter.ExportSharedPack(
            project, sharedPath, options, {sourceId}, {kSoundAObjectId});
        ASSERT_TRUE(sharedResult.success) << sharedResult.error.toStdString();
        for (const QString& path : {
                 variantPackPath(project, outputDir, harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias)), sharedPath}) {
            SCOPED_TRACE(path.toStdString());
            PackImage pack;
            ASSERT_NO_FATAL_FAILURE(readPack(path, pack));
            ASSERT_EQ(2, pack.countKind(harken::kPackEntryKindBindingDefinition));
            ASSERT_EQ(2, pack.countKind(harken::kPackEntryKindBindingAttachment));
            std::set<harkenstudio::ObjectId> definitions;
            std::set<harkenstudio::ObjectId> attachments;
            for (const auto& entry : pack.entries) {
                if (entry.kind == harken::kPackEntryKindBindingDefinition) {
                    VnBindingId id{};
                    VnBindingDesc desc{};
                    ASSERT_TRUE(harken::ParsePackV2BindingDefinition(
                        pack.data() + entry.offset, entry.size, &id, &desc));
                    EXPECT_TRUE(id == volume.runtimeId() || id == pitch.runtimeId());
                    EXPECT_EQ(static_cast<uint32_t>(VN_TARGET_SCOPE_VOICE), desc.targetScopeKind);
                    EXPECT_EQ(static_cast<uint32_t>(id == volume.runtimeId()
                        ? VN_TARGET_VOICE_VOLUME : VN_TARGET_VOICE_PITCH), desc.targetId);
                    EXPECT_EQ(duration, desc.transitionMs);
                    definitions.insert(harkentest::ToStudio(id));
                } else if (entry.kind == harken::kPackEntryKindBindingAttachment) {
                    harken::PackBindingAttachmentEntry attachment{};
                    ASSERT_TRUE(harken::ParsePackV2BindingAttachment(
                        pack.data() + entry.offset, entry.size, &attachment));
                    EXPECT_EQ(static_cast<uint32_t>(VN_TARGET_SCOPE_VOICE), attachment.scopeKind);
                    EXPECT_EQ(sound->runtimeId(), attachment.objectId);
                    attachments.insert(harkentest::ToStudio(attachment.bindingId));
                }
            }
            const std::set<harkenstudio::ObjectId> expected{volume.runtimeId(), pitch.runtimeId()};
            EXPECT_EQ(expected, definitions);
            EXPECT_EQ(expected, attachments);
        }
    }
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorPackExportOrchestration, IllegalBindingPreservesCanonicalWorldAndPublishedFiles)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(dir.path());
    const uint32_t sourceId = registerToneAsset(dir.path(), QStringLiteral("response.wav"), 1.0f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(),
        {{QStringLiteral("Tone"), kSoundAObjectId, sourceId}},
        {{QStringLiteral("PlayTone"), kActionOneObjectId, {kSoundAObjectId}}}));
    const QString outputDir = QDir(dir.path()).filePath(QStringLiteral("published"));
    VNX::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    options.currentWorldOutputPath = QDir(dir.path()).filePath(QStringLiteral("world.bin"));
    VNX::ProjectPackExporter exporter;
    const auto baseline = exporter.ExportAllPacks(project, outputDir, options);
    ASSERT_TRUE(baseline.success) << baseline.error.toStdString();
    ASSERT_TRUE(QFileInfo::exists(options.currentWorldOutputPath));
    std::map<QString, QByteArray> published;
    QDirIterator files(outputDir, QDir::Files, QDirIterator::Subdirectories);
    while (files.hasNext()) {
        const QString path = files.next();
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        published[path] = file.readAll();
    }
    ASSERT_FALSE(published.empty());
    QFile world(options.currentWorldOutputPath);
    ASSERT_TRUE(world.open(QIODevice::ReadOnly));
    published[options.currentWorldOutputPath] = world.readAll();
    world.close();
    auto sound = project.layers.front().rootItem->children.front();
    auto binding = VNX::BindingSemantics::defaultBinding(
        VNX::BindingSemantics::volumeAddress(), QUuid::createUuid());
    binding.guid = VNX::authoringUuid(harkentest::StudioObjectId(800001));
    binding.transitionMs = 60001;
    ASSERT_TRUE(sound->comp<VNX::VMixNode>()->bindings.upsert(binding));
    const QString sharedPath = QDir(outputDir).filePath(QStringLiteral("Shared.bytes"));
    QFile shared(sharedPath);
    ASSERT_TRUE(shared.open(QIODevice::WriteOnly));
    ASSERT_EQ(shared.write("previous-shared"), 15);
    shared.close();
    published[sharedPath] = QByteArray("previous-shared");
    const auto initRefused = exporter.ExportInitPack(project,
        QDir(outputDir).filePath(QStringLiteral("Init.bytes")), options);
    EXPECT_FALSE(initRefused.success);
    EXPECT_TRUE(initRefused.error.contains(QStringLiteral("transitionMs")));
    const auto actionRefused = exporter.ExportActionPack(project, QStringLiteral("PlayTone"),
        variantPackPath(project, outputDir, harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias)), options);
    EXPECT_FALSE(actionRefused.success);
    EXPECT_TRUE(actionRefused.error.contains(QStringLiteral("transitionMs")));
    const auto sharedRefused = exporter.ExportSharedPack(project, sharedPath, options, {}, {});
    EXPECT_FALSE(sharedRefused.success);
    EXPECT_TRUE(sharedRefused.error.contains(QStringLiteral("transitionMs")));
    const auto refused = exporter.ExportAllPacks(project, outputDir, options);
    EXPECT_FALSE(refused.success);
    EXPECT_TRUE(refused.error.contains(QStringLiteral("transitionMs")))
        << refused.error.toStdString();
    for (const auto& [path, bytes] : published) {
        QFile file(path);
        ASSERT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
        EXPECT_EQ(file.readAll(), bytes) << path.toStdString();
    }
    VNX::AssetRegistry::instance().clear();
}
