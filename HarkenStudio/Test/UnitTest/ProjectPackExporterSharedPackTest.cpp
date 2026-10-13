#include "UnitTest/StudioWorldTestSupport.h"
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Support/FieldGroupTestUtils.h"
#include "Engine.h"
#include "UnitTest/CanonicalPackSetActivate.h"
#include "UnitTest/TestUtil.h"
#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/PathUtils.h"
#include "ActionClosureSemantics.h"
#include "BindingSemantics.h"
#include "CanonicalRuntimeProjectAdapterPrivate.h"
#include "Components/VAction.h"
#include "Components/VAttenuation.h"
#include "Components/VMixNode.h"
#include "Components/VSound.h"
#include "Components/VTrack.h"
#include "Components/VVariable.h"
#include "CurrentWorldProjectAdapter.h"
#include "MemorySettingsStorage.h"
#include "PackBuildPlan.h"
#include "PlatformContextService.h"
#include "ProjectManager.h"
#include "ProjectPackExporter.h"
#include "ProjectTypes.h"
#include "QSettingsStorage.h"
#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "FieldGroupProjectAdapter.h"

#include "harken/Harken.h"
#include "harken/core/CurrentWorldCodec.h"
#include "harken/core/PlayablePropertyCut.h"
#include "harken/media/PackFormat.h"
#include "harken/media/PackFormatV2.h"
#include "harken/media/PackLogicImage.h"
#pragma push_macro("slots")
#undef slots
#include "harken_project_schema/FrontEnd.h"
#include "harken_project_schema/PieceProgram.h"
#pragma pop_macro("slots")

#include <gtest/gtest.h>

#include <QByteArray>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QTemporaryDir>
#include <QUuid>

#include <algorithm>
#include <array>
#include <cstdint>
#include <map>
#include <memory>
#include <set>
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

std::vector<harken::PackTocEntry> readMediaTocs(const PackImage& image)
{
    std::vector<harken::PackTocEntry> tocs;
    for (const auto& entry : image.entries)
        if (entry.kind == harken::kPackEntryKindMedia)
            tocs.push_back(harken::ParsePackTocEntry(image.data() + entry.offset));
    return tocs;
}

VnResult loadPack(const QString& path, VnPackHandle* outHandle)
{
    VnPackDesc desc{};
    desc.structSize = sizeof(VnPackDesc);
    const QByteArray utf8 = QFileInfo(path).absoluteFilePath().toUtf8();
    desc.utf8Path = utf8.constData();
    return Vn_LoadPack(&desc, outHandle);
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
    for (const auto& group : project.packGroups)
        if (std::find(group.actionIds.begin(), group.actionIds.end(), stableId) != group.actionIds.end())
            return QDir(dir).filePath(VNX::PackBuildPlanning::packFileName(language, group.name, QStringLiteral(".bytes")));
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

constexpr uint32_t kSharedSoundObjectId = 1;
constexpr uint32_t kSoloSoundObjectId = 2;
constexpr uint32_t kActionOneObjectId = 21;
constexpr uint32_t kActionTwoObjectId = 22;
constexpr uint32_t kActionThreeObjectId = 23;
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
    project.name = QStringLiteral("shared-pack");
    project.projectPath =
        QDir(projectRoot).filePath(QStringLiteral("shared-pack.vproj"));
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

std::shared_ptr<VNX::ProjectItem> attachDriveVariable(VNX::ProjectData& project)
{
    auto variable =
        makeItem(QStringLiteral("Drive"), VNX::ProjectItem::Type::Variable, 50);
    auto* variableComponent = variable->comp<VNX::VVariable>();
    variableComponent->kind = VNX::VariableKind::Smooth;
    variableComponent->rangeMin = 0.0f;
    variableComponent->rangeMax = 1.0f;
    variableComponent->defaultValue = 0.0f;
    VNX::ProjectFileBinding variableRoot;
    variableRoot.rootItem = variable;
    project.variables.push_back(variableRoot);
    return variable;
}

void attachSharedVolumeBindings(VNX::ProjectData& project,
                                const VNX::ProjectItem& variable)
{
    VNX::ParameterBindingDef voiceBinding;
    voiceBinding.guid = VNX::authoringUuid(harkentest::StudioObjectId(500));
    voiceBinding.variable = variable.guid;
    voiceBinding.address = VNX::BindingSemantics::volumeAddress();
    voiceBinding.curve = {{0.0f, -6.0f}, {1.0f, 0.0f}};
    voiceBinding.outputMin = -12.0f;
    voiceBinding.outputMax = 0.0f;
    auto* layerMix = project.layers.front().rootItem->comp<VNX::VMixNode>();
    ASSERT_TRUE(layerMix != nullptr);
    ASSERT_TRUE(layerMix->bindings.upsert(voiceBinding));
    auto busBinding = voiceBinding;
    busBinding.guid = VNX::authoringUuid(harkentest::StudioObjectId(501));
    busBinding.address =
        VNX::BindingSemantics::volumeAddress(VNX::ProjectItemType::Bus);
    ASSERT_TRUE(project.busses.front().rootItem->comp<VNX::VMixNode>()->bindings.upsert(
        busBinding));
}

struct CanonicalPackActivation {
    std::vector<QByteArray> pathUtf8;
    std::vector<VnCanonicalPackSetMemberV1> members;
    std::vector<VnCanonicalPackSetMemberResultV1> rows;
    VnCanonicalPackSetActivationReportV1 report{};
};

void fillCanonicalMember(VnCanonicalPackSetMemberV1* member,
                         uint32_t packKind,
                         uint64_t packStableId,
                         const char* utf8Path)
{
    *member = {};
    member->structSize = sizeof(*member);
    member->packKind = packKind;
    member->packStableId = packStableId;
    member->utf8Path = utf8Path;
}

VnResult parseCanonicalPackStableId(const QString& path, uint64_t* packStableId)
{
    if (packStableId == nullptr)
        return VN_INVALID_ARG;
    QFile file(path);
    if (!file.open(QIODevice::ReadOnly))
        return VN_BAD_PACK;
    const QByteArray bytes = file.readAll();
    harken::PackCanonicalImage canonical;
    const VnResult parsed = harken::ParsePackCanonicalImage(
        reinterpret_cast<const uint8_t*>(bytes.constData()),
        static_cast<uint64_t>(bytes.size()),
        &canonical);
    if (parsed != VN_OK)
        return parsed;
    *packStableId = canonical.packStableId != 0
        ? canonical.packStableId
        : canonical.partitionHeader.packStableId;
    return *packStableId != 0 ? VN_OK : VN_BAD_PACK;
}

VnResult activateCanonicalPackFiles(const QStringList& paths,
                                    CanonicalPackActivation* loaded,
                                    VnPackHandle previousInit = 0,
                                    std::uint64_t previousRevision = 0)
{
    if (loaded == nullptr || paths.isEmpty())
        return VN_INVALID_ARG;
    loaded->pathUtf8.clear();
    loaded->members.clear();
    loaded->rows.clear();
    loaded->report = {};
    loaded->pathUtf8.reserve(static_cast<size_t>(paths.size()));
    loaded->members.resize(static_cast<size_t>(paths.size()));
    loaded->rows.resize(static_cast<size_t>(paths.size()));
    for (const QString& path : paths)
        loaded->pathUtf8.push_back(QFileInfo(path).absoluteFilePath().toUtf8());
    for (int i = 0; i < paths.size(); ++i) {
        uint64_t packStableId = 0;
        const VnResult parsed = parseCanonicalPackStableId(paths.at(i), &packStableId);
        if (parsed != VN_OK)
            return parsed;
        fillCanonicalMember(&loaded->members[static_cast<size_t>(i)],
                            i == 0 ? VN_CANONICAL_PACK_SET_MEMBER_INIT
                                   : VN_CANONICAL_PACK_SET_MEMBER_CONTENT,
                            packStableId,
                            loaded->pathUtf8[static_cast<size_t>(i)].constData());
        if (previousInit != 0 && i == 0)
            loaded->members[0].expectedHandle = previousInit;
        loaded->rows[static_cast<size_t>(i)].structSize =
            sizeof(loaded->rows[static_cast<size_t>(i)]);
    }
    if (loaded->members.size() > 2) {
        std::vector<size_t> order;
        order.reserve(loaded->members.size() - 1);
        for (size_t i = 1; i < loaded->members.size(); ++i)
            order.push_back(i);
        std::sort(order.begin(), order.end(), [&](size_t lhs, size_t rhs) {
            return loaded->members[lhs].packStableId < loaded->members[rhs].packStableId;
        });
        std::vector<QByteArray> sortedUtf8;
        std::vector<VnCanonicalPackSetMemberV1> sortedMembers;
        sortedUtf8.reserve(loaded->pathUtf8.size());
        sortedMembers.reserve(loaded->members.size());
        sortedUtf8.push_back(std::move(loaded->pathUtf8[0]));
        sortedMembers.push_back(loaded->members[0]);
        for (size_t index : order) {
            sortedUtf8.push_back(std::move(loaded->pathUtf8[index]));
            sortedMembers.push_back(loaded->members[index]);
        }
        loaded->pathUtf8 = std::move(sortedUtf8);
        loaded->members = std::move(sortedMembers);
        for (size_t i = 0; i < loaded->members.size(); ++i)
            loaded->members[i].utf8Path = loaded->pathUtf8[i].constData();
    }
    return harkentest::ActivateCanonicalMembersV1(
        loaded->members.data(),
        static_cast<uint32_t>(loaded->members.size()),
        loaded->rows.data(),
        loaded->report,
        previousInit,
        previousRevision);
}

VnResult replaceCanonicalPackFile(const QString& path, VnPackHandle handle)
{
    const QByteArray utf8 = QFileInfo(path).absoluteFilePath().toUtf8();
    VnPackLoadDesc desc{};
    desc.structSize = sizeof(desc);
    desc.utf8Path = utf8.constData();
    return Vn_ReplacePack(handle, reinterpret_cast<const VnPackDesc*>(&desc));
}

bool writePackWithoutKind(const PackImage& image, const QString& dest,
                          uint32_t kind, QString* error)
{
    std::vector<HarkenEditor::Bridge::PackBuildSession::CanonicalPackEntry> entries;
    for (const auto& record : image.entries) {
        if (record.kind == kind)
            continue;
        HarkenEditor::Bridge::PackBuildSession::CanonicalPackEntry entry;
        entry.kind = record.kind;
        entry.vintage = record.vintage;
        entry.id = harkentest::ToStudio(record.id);
        if (record.kind == harken::kPackEntryKindMedia) {
            const harken::PackTocEntry toc =
                harken::ParsePackTocEntry(image.data() + record.offset);
            entry.payload.resize(static_cast<size_t>(
                harken::kPackV2MediaPayloadBytes + toc.blobBytes));
            std::copy_n(image.data() + record.offset,
                        static_cast<size_t>(harken::kPackV2MediaPayloadBytes),
                        entry.payload.data());
            if (toc.blobBytes != 0) {
                std::copy_n(image.data() + toc.blobOffset,
                            static_cast<size_t>(toc.blobBytes),
                            entry.payload.data() +
                                harken::kPackV2MediaPayloadBytes);
            }
        } else {
            entry.payload.assign(image.data() + record.offset,
                                 image.data() + record.offset + record.size);
        }
        entries.push_back(std::move(entry));
    }
    return HarkenEditor::Bridge::PackBuildSession::writeCanonicalPackFile(
        dest,
        HarkenEditor::Bridge::PackBuildSession::CanonicalPackKind::CanonicalContent,
        entries, error);
}

struct CompilationGroups
{
    VNX::MainOutputFieldGroup mainOutput;
    VNX::SendFieldGroup send;
    VNX::SpatialRatingFieldGroup rating;
    harkenstudio::ObjectId outputBusId = harkentest::StudioObjectId(0);
    harkenstudio::ObjectId sendBusId{};
    harkenstudio::ObjectId attenuationId = harkentest::StudioObjectId(0);
};

class EditorFieldGroupCompilation : public ::testing::Test
{
protected:
    void SetUp() override
    {
        using namespace VNX;
        ASSERT_TRUE(directory_.isValid());
        qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
        auto& manager = ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
        PlatformContextService::instance().setStorageForTesting(
            std::make_unique<MemorySettingsStorage>());
        ProjectConfig config;
        config.name = QStringLiteral("FieldGroupCompilationScope");
        config.location = directory_.path();
        config.platforms = HarkenTest::platforms({QStringLiteral("Windows"), QStringLiteral("Android")});
        ASSERT_TRUE(manager.createProject(config));
        AssetRegistry::instance().clear();
        AssetRegistry::instance().setProjectRoot(directory_.path());
        const auto sourceId = registerToneAsset(
            directory_.path(), QStringLiteral("shared.wav"), 1.0f);
        const auto otherSourceId = registerToneAsset(
            directory_.path(), QStringLiteral("solo.wav"), 0.5f);
        ASSERT_NO_FATAL_FAILURE(buildProject(
            project_, directory_.path(), sourceId, otherSourceId,
            {{QStringLiteral("AllEntries"), kActionOneObjectId, kSharedSoundObjectId}}));
        project_.platforms = config.platforms;
        ownershipParent_ = project_.layers.front().rootItem;
        sound_ = ownershipParent_->children.front();
        for (const auto& sound : ownershipParent_->children) {
            auto* source = sound->comp<VSound>();
            const auto entryId = QUuid::createUuid();
            source->sources.plain.entries.push_back({entryId, sound->sourceID});
            source->sources.plain.active = entryId;
        }
        const auto master = project_.busses.front().rootItem;
        for (uint32_t index = 0; index < expected_.size(); ++index) {
            auto output = makeItem(QStringLiteral("Output%1").arg(index),
                                   ProjectItem::Type::Bus, 202 + index * 2);
            auto send = makeItem(QStringLiteral("Send%1").arg(index),
                                 ProjectItem::Type::Bus, 203 + index * 2);
            addChild(master, output);
            addChild(master, send);
            auto attenuation = makeItem(QStringLiteral("Attenuation%1").arg(index),
                                        ProjectItem::Type::Attenuation, 301 + index);
            auto& definition = *attenuation->comp<VAttenuation>();
            definition.maxDistance = 50.0f;
            definition.dimensions.emplaceBack(AttenuationDimensionDef{AttenuationDimension::Volume}).curve =
                {{0.0f, 0.0f}, {50.0f, -60.0f}};
            ProjectFileBinding binding;
            binding.rootItem = attenuation;
            project_.attenuationItems.push_back(std::move(binding));
            auto& expected = expected_[index];
            expected.mainOutput = {toFieldGroupGuid(output->guid), -3.0f - 4.0f * index};
            expected.send.sends = {{toFieldGroupGuid(send->guid), -4.0f - 6.0f * index}};
            expected.rating = {0.25f - 0.25f * index, true,
                               toFieldGroupGuid(attenuation->guid),
                               1.5f + index};
            expected.outputBusId = output->runtimeId();
            expected.sendBusId = send->runtimeId();
            expected.attenuationId = attenuation->runtimeId();
        }
        setParentGroups({}, expected_[0]);
        setParentGroups(HarkenTest::FieldGroupPlatformIds::Android, expected_[1]);
        sound_->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Windows].mode =
            FieldGroupMode::UseShared;
        sound_->fieldGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::Windows].mode =
            FieldGroupMode::UseShared;
        sound_->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Windows].mode =
            FieldGroupMode::UseShared;
        sound_->fieldGroups.mainOutput.platforms[HarkenTest::FieldGroupPlatformIds::Android].mode =
            FieldGroupMode::Inherit;
        sound_->fieldGroups.send.platforms[HarkenTest::FieldGroupPlatformIds::Android].mode =
            FieldGroupMode::Inherit;
        sound_->fieldGroups.spatialRating.platforms[HarkenTest::FieldGroupPlatformIds::Android].mode =
            FieldGroupMode::Inherit;

        auto container = makeItem(QStringLiteral("OwnershipContainer"), ProjectItem::Type::Container, 47);
        ownershipParent_->children.erase(ownershipParent_->children.begin());
        addChild(ownershipParent_, container);
        auto outerPiece = makeItem(QStringLiteral("OuterPiece"), ProjectItem::Type::Piece, 40);
        auto innerPiece = makeItem(QStringLiteral("InnerPiece"), ProjectItem::Type::Piece, 42);
        addChild(container, outerPiece);
        ASSERT_TRUE(PlayablePropertyAuthoringV1::addContainerChildOccurrence(*container, outerPiece->guid));
        auto addTrack = [&](const std::shared_ptr<ProjectItem>& owner, uint32_t id,
                            const std::shared_ptr<ProjectItem>& target) {
            auto track = makeItem(QStringLiteral("Track%1").arg(id), ProjectItem::Type::Track, id);
            addChild(owner, track);
            addChild(track, target);
            Clip clip;
            clip.clipGuid = QUuid::createUuid();
            clip.targetGuid = target->guid;
            clip.clipGuid = VNX::authoringUuid(harkentest::StudioObjectId(150000 + id));
            auto& clips = track->comp<VTrack>()->clips;
            EXPECT_TRUE(clips.mintOccurrenceOrdinal(&clip));
            clips.items.push_back(clip);
            return track;
        };
        addTrack(outerPiece, 41, innerPiece);
        auto leafTrack = addTrack(innerPiece, 43, sound_);
        for (uint32_t index = 1; index < 4; ++index) {
            auto& clips = leafTrack->comp<VTrack>()->clips;
            Clip repeated = clips.items.front();
            repeated.clipGuid = QUuid::createUuid();

            ASSERT_TRUE(clips.mintOccurrenceOrdinal(&repeated));
            clips.items.push_back(repeated);
        }
        auto* action = project_.actions.front().rootItem->children.front()->comp<VAction>();
        for (const auto& target : {leafTrack, container, outerPiece}) {
            VActionStep step;
            step.type = VActionStep::Type::Play;
            step.targetObjectID = target->objectID;
            action->steps.push_back(step);
        }
    }

    void TearDown() override
    {
        auto& manager = VNX::ProjectManager::instance();
        if (manager.isProjectOpen())
            manager.closeProject();
        VNX::AssetRegistry::instance().clear();
        VNX::PlatformContextService::instance().setStorageForTesting(
            std::make_unique<VNX::QSettingsStorage>(
                QStringLiteral("HarkenSound"), QStringLiteral("HarkenStudio")));
    }

    void setParentGroups(VNX::FieldGroupScope scope, const CompilationGroups& expected)
    {
        auto& groups = ownershipParent_->fieldGroups;
        (scope ? groups.mainOutput.platforms[*scope] : groups.mainOutput.shared) =
            {VNX::FieldGroupMode::Local, expected.mainOutput};
        (scope ? groups.send.platforms[*scope] : groups.send.shared) =
            {VNX::FieldGroupMode::Local, expected.send};
        (scope ? groups.spatialRating.platforms[*scope] : groups.spatialRating.shared) =
            {VNX::FieldGroupMode::Local, expected.rating};
    }

    void addDormantHistory()
    {
        using namespace VNX;
        const auto missingBus = toFieldGroupGuid(QUuid::createUuid());
        const auto missingAttenuation = toFieldGroupGuid(QUuid::createUuid());
        auto& groups = sound_->fieldGroups;
        for (const FieldGroupScope scope : {
                 FieldGroupScope{}, FieldGroupScope{HarkenTest::FieldGroupPlatformIds::Windows},
                 FieldGroupScope{HarkenTest::FieldGroupPlatformIds::Android}}) {
            (scope ? groups.mainOutput.platforms[*scope] : groups.mainOutput.shared).localHistory =
                MainOutputFieldGroup{missingBus, -37.0f};
            (scope ? groups.send.platforms[*scope] : groups.send.shared).localHistory =
                SendFieldGroup{{{missingBus, -31.0f}}};
            (scope ? groups.spatialRating.platforms[*scope] : groups.spatialRating.shared).localHistory =
                SpatialRatingFieldGroup{-0.9f, false, missingAttenuation, 7.0f};
        }
    }

    void expectWorld(const harken::ContainerWorldInstallViewV1& world,
                     const CompilationGroups& expected, uint64_t& soundFingerprint)
    {
        ASSERT_GT(world.playablePropertyCount, 0u);
        const auto* end = world.playableProperties + world.playablePropertyCount;
        const auto* sound = std::find_if(world.playableProperties, end, [&](const auto& row) {
            return row.stableId == sound_->runtimeId();
        });
        ASSERT_NE(sound, end);
        soundFingerprint = sound->programSemanticFingerprint;
        ASSERT_NE(soundFingerprint, 0u);
        EXPECT_EQ(sound->payload.presentMask & VNX::kFieldGroupPropertyMask,
                  VNX::kFieldGroupPropertyMask);
        uint32_t soundReferences = 0;
        for (uint32_t index = 0; index < world.playableOccurrenceCount; ++index) {
            const auto& occurrence = world.playableOccurrences[index];
            EXPECT_EQ(occurrence.payload.presentMask & VNX::kFieldGroupPropertyMask, 0u);
            if (occurrence.childPlayable.stableId == sound_->runtimeId()) {
                ++soundReferences;
                EXPECT_EQ(occurrence.childPlayable.expectedProgramSemanticFingerprint,
                          soundFingerprint);
            }
        }
        EXPECT_EQ(soundReferences, 4u);

        harken::PlayableWorldViewV1 playback;
        playback.generation = 1;
        playback.identity = world.identity;
        playback.properties = world.playableProperties;
        playback.propertyCount = world.playablePropertyCount;
        playback.occurrences = world.playableOccurrences;
        playback.occurrenceCount = world.playableOccurrenceCount;
        playback.trackSchedules = world.trackSchedules;
        playback.trackScheduleCount = world.trackScheduleCount;
        playback.pieceSchedules = world.pieceSchedules;
        playback.pieceScheduleCount = world.pieceScheduleCount;
        playback.containers = world.containers;
        playback.containerCount = world.containerCount;
        struct EntryPath {
            uint32_t objectId;
            harken::PlayableKindV1 kind;
            std::vector<uint32_t> ordinals;
            std::vector<VnObjectId> trackIds;
        };
        using Kind = harken::PlayableKindV1;
        for (const auto& path : std::vector<EntryPath>{
                 {kSharedSoundObjectId, Kind::Sound, {}, {}},
                 {43, Kind::Track, {0}, {harkentest::ObjectId(100043)}},
                 {43, Kind::Track, {1}, {harkentest::ObjectId(100043)}},
                 {47, Kind::Container, {0, 0, 0}, {harkentest::ObjectId(0), harkentest::ObjectId(100041), harkentest::ObjectId(100043)}},
                 {42, Kind::Piece, {2}, {harkentest::ObjectId(100043)}},
                 {42, Kind::Piece, {3}, {harkentest::ObjectId(100043)}},
                 {40, Kind::Piece, {0, 0}, {harkentest::ObjectId(100041), harkentest::ObjectId(100043)}},
                 {40, Kind::Piece, {0, 3}, {harkentest::ObjectId(100041), harkentest::ObjectId(100043)}}}) {
            SCOPED_TRACE(path.objectId);
            const auto* root = std::find_if(world.playableProperties, end, [&](const auto& row) {
                return row.stableId == harkentest::StudioObjectId(path.objectId + kStableIdBias);
            });
            ASSERT_NE(root, end);
            const harken::PlayableRefV1 reference{
                path.kind, 0, root->stableId, root->programSemanticFingerprint,
                root->contentRevision, root->routeStableId, root->compatibilityFingerprint};
            harken::ResolvedPlayablePropertyCutStorageV1 cut;
            ASSERT_TRUE(cut.Reset({}));
            ASSERT_EQ(cut.Resolve(playback, reference, path.ordinals.data(),
                                  static_cast<uint32_t>(path.ordinals.size()), path.trackIds.data()),
                      harken::ResolvedPlayablePropertyCutStatusV1::Ok);
            const auto& value = cut.View();
            EXPECT_EQ(value.leaf.stableId, sound_->runtimeId());
            EXPECT_EQ(value.mainOutputBusRef, expected.outputBusId);
            EXPECT_FLOAT_EQ(value.outputBusVolumeDb, expected.mainOutput.outputBusVolume);
            ASSERT_EQ(value.sendCount, expected.send.sends.size());
            ASSERT_EQ(value.sendCount, 1u);
            EXPECT_EQ(value.sends[0].targetBusId, expected.sendBusId);
            EXPECT_FLOAT_EQ(value.sends[0].sendLevel, expected.send.sends[0].sendLevel);
            EXPECT_FLOAT_EQ(value.spatial.panning, expected.rating.spatialPanning);
            EXPECT_FLOAT_EQ(value.spatial.spread, 0.0f);
            EXPECT_EQ(value.spatial.is3DEnabled, expected.rating.spatialIs3DEnabled);
            EXPECT_EQ(value.spatial.attenuationRef, expected.attenuationId);
            EXPECT_FLOAT_EQ(value.spatial.attenuationDistanceScaling,
                            expected.rating.spatialAttenuationDistanceScaling);
        }
    }

    QTemporaryDir directory_;
    VNX::ProjectData project_;
    std::shared_ptr<VNX::ProjectItem> ownershipParent_;
    std::shared_ptr<VNX::ProjectItem> sound_;
    std::array<CompilationGroups, 3> expected_;
};

} // namespace

TEST_F(EditorFieldGroupCompilation, AllEntriesMatchAcrossPlatformsAndIgnoreDormantHistory)
{
    using namespace VNX;
    using namespace HarkenEditor::Bridge;
    std::array<std::array<uint64_t, 2>, 3> currentFingerprints{};
    std::array<std::array<uint64_t, 2>, 3> pieceFingerprints{};
    std::array<std::array<uint64_t, 2>, 3> packFingerprints{};
    std::array<QByteArray, 2> baselineCanonical;
    const std::array platforms{HarkenTest::PlatformIds::Windows, HarkenTest::PlatformIds::Android};
    const std::array authoringPlatforms{HarkenTest::FieldGroupPlatformIds::Windows, HarkenTest::FieldGroupPlatformIds::Android};
    for (size_t phase = 0; phase < 3; ++phase) {
        SCOPED_TRACE(phase);
        if (phase == 1)
            addDormantHistory();
        if (phase == 2)
            setParentGroups(HarkenTest::FieldGroupPlatformIds::Windows, expected_[2]);
        for (size_t platformIndex = 0; platformIndex < platforms.size(); ++platformIndex) {
            const auto platform = platforms[platformIndex];
            const auto platformName = ProjectPlatformSemantics::idString(platform);
            SCOPED_TRACE(platformName.toStdString());
            PlatformContextService::instance().setSelected({platform}, platform, false);
            ASSERT_EQ(PlatformContextService::instance().context().primary, platform);
            const auto& expected = expected_[phase == 2 && platformIndex == 0 ? 2 : platformIndex];
            const auto groups = buildFieldGroupEffectiveSnapshot(project_, authoringPlatforms[platformIndex]);
            ASSERT_TRUE(groups.ok());
            const auto effective = groups.definitions.find(toFieldGroupGuid(sound_->guid));
            ASSERT_NE(effective, groups.definitions.end());
            EXPECT_EQ(effective->second.mainOutput, expected.mainOutput);
            EXPECT_EQ(effective->second.send, expected.send);
            EXPECT_EQ(effective->second.spatialRating, expected.rating);
            for (const auto& source : {effective->second.mainOutputSource, effective->second.sendSource,
                                       effective->second.spatialRatingSource}) {
                EXPECT_EQ(source.definition, toFieldGroupGuid(ownershipParent_->guid));
                EXPECT_FALSE(source.isDefault);
            }

            const auto compiled = compileStudioProjectInput(project_);
            ASSERT_TRUE(compiled.ok()) << compiled.diagnostics.front().message.toStdString();
            const auto* piece = compiledPieceProgram(*compiled.snapshot);
            ASSERT_NE(piece, nullptr);
            pieceFingerprints[phase][platformIndex] = piece->programSemanticFingerprint();
            if (phase == 0)
                baselineCanonical[platformIndex] = compiled.canonicalProject;
            else if (phase == 1 || platformIndex == 1)
                EXPECT_EQ(compiled.canonicalProject, baselineCanonical[platformIndex]);
            size_t occurrences = 0;
            for (const auto& track : piece->ir().tracks()) {
                for (const auto& occurrence : track.occurrences) {
                    if (occurrence.contentTargetGuid != toFieldGroupGuid(sound_->guid))
                        continue;
                    ++occurrences;
                    EXPECT_TRUE(occurrence.outputBusResolved);
                    EXPECT_EQ(occurrence.resolvedOutputBus, expected.outputBusId);
                    EXPECT_FLOAT_EQ(occurrence.outputBusVolumeDb, expected.mainOutput.outputBusVolume);
                    ASSERT_EQ(occurrence.sends.size(), 1u);
                    EXPECT_EQ(occurrence.sends[0].targetBusId, expected.sendBusId);
                    EXPECT_FLOAT_EQ(occurrence.sends[0].sendLevel, expected.send.sends[0].sendLevel);
                    EXPECT_FLOAT_EQ(occurrence.panning, expected.rating.spatialPanning);
                    EXPECT_FLOAT_EQ(occurrence.spread, 0.0f);
                    EXPECT_EQ(occurrence.is3DEnabled, expected.rating.spatialIs3DEnabled);
                    EXPECT_TRUE(occurrence.attenuationResolved);
                    EXPECT_EQ(occurrence.resolvedAttenuation, expected.attenuationId);
                    EXPECT_FLOAT_EQ(occurrence.attenuationDistanceScaling,
                                    expected.rating.spatialAttenuationDistanceScaling);
                }
            }
            EXPECT_EQ(occurrences, 4u);

            QString error;
            const auto request = buildCurrentWorldSyncRequest(project_, platform, &error);
            ASSERT_NE(request, nullptr) << error.toStdString();
            for (auto& materialization : request->materializations)
                if (materialization.kind == harkenplayableproperties::ContentPropertyKindV1::Sound)
                    materialization.contentRevision = std::hash<harkenstudio::ObjectId>{}(materialization.stableId) + 1000;
            const auto front = harkenprojectschema::CompileFrontEnd(request->project);
            ASSERT_TRUE(front.ok());
            const auto current = harkencurrentworldcompiler::CompileCurrentWorldV1(
                *front.project, request->variables, request->materializations, request->containers,
                {}, request->transitions, request->actions);
            ASSERT_TRUE(current.ok());
            harken::CurrentWorldBodyImage currentImage;
            ASSERT_TRUE(harkentest::DecodeStudioWorld(current.world->View(), currentImage));
            ASSERT_NO_FATAL_FAILURE(expectWorld(
                *currentImage.ProductionView(), expected, currentFingerprints[phase][platformIndex]));

            const auto closure = ActionClosureSemantics::build(
                project_, harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias), {platformName, QString()});
            ASSERT_TRUE(closure.ready());
            using Reason = ActionClosureEdgeReason;
            std::map<Reason, std::set<harkenstudio::ObjectId>> dependencies;
            for (const auto& edge : closure.edges)
                if (edge.fromStableId == sound_->runtimeId())
                    dependencies[edge.reason].insert(edge.toStableId);
            const std::map<Reason, std::set<harkenstudio::ObjectId>> expectedDependencies{
                {Reason::EffectiveOutputBus, {expected.outputBusId}},
                {Reason::EffectiveSend, {expected.sendBusId}},
                {Reason::EffectiveAttenuation, {expected.attenuationId}}};
            EXPECT_EQ(dependencies, expectedDependencies);

            ProjectPackExporter exporter;
            ProjectPackExporter::ExportOptions options;
            options.platform = platformName;
            options.language = QString();
            const QString outputDir = QDir(directory_.path()).filePath(
                QStringLiteral("groups-%1-%2").arg(platformName).arg(phase));
            const auto exported = exporter.ExportAllPacks(project_, outputDir, options);
            ASSERT_TRUE(exported.success) << exported.error.toStdString();
            const QString path = variantPackPath(project_,
                outputDir, harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias), platformName);
            PackImage image;
            ASSERT_NO_FATAL_FAILURE(readPack(path, image));
            harken::PackCanonicalImage canonical;
            ASSERT_EQ(harken::ParsePackCanonicalImage(
                          image.data(), static_cast<uint64_t>(image.bytes.size()), &canonical), VN_OK);
            ASSERT_TRUE(canonical.hasPartition);
            harken::CurrentWorldBodyImage decoded;
            ASSERT_TRUE(decoded.Decode(
                canonical.partitionBody.data(), canonical.partitionBody.size(),
                canonical.partitionHeader.bodyVintage, harken::CurrentWorldDecodeMode::StrictArtifact));
            PackImage initImage;
            ASSERT_NO_FATAL_FAILURE(readPack(
                QDir(outputDir).filePath(QStringLiteral("Init.bytes")), initImage));
            harken::PackCanonicalImage initCanonical;
            ASSERT_EQ(harken::ParsePackCanonicalImage(
                          initImage.data(), static_cast<uint64_t>(initImage.bytes.size()),
                          &initCanonical), VN_OK);
            ASSERT_TRUE(initCanonical.hasPartition);
            harken::CurrentWorldBodyImage initDecoded;
            ASSERT_TRUE(initDecoded.Decode(
                initCanonical.partitionBody.data(), initCanonical.partitionBody.size(),
                initCanonical.partitionHeader.bodyVintage, harken::CurrentWorldDecodeMode::StrictArtifact));
            ASSERT_NE(decoded.ProductionView(), nullptr);
            ASSERT_NE(initDecoded.ProductionView(), nullptr);
            auto packWorld = *decoded.ProductionView();
            const auto& initWorld = *initDecoded.ProductionView();
            ASSERT_GT(initWorld.playablePropertyCount, 0u);
            ASSERT_GT(packWorld.playablePropertyCount, 0u);
            std::vector<harken::PlayablePropertyProgramViewV1> properties(
                initWorld.playableProperties,
                initWorld.playableProperties + initWorld.playablePropertyCount);
            properties.insert(properties.end(), packWorld.playableProperties,
                              packWorld.playableProperties + packWorld.playablePropertyCount);
            packWorld.playableProperties = properties.data();
            packWorld.playablePropertyCount = static_cast<uint32_t>(properties.size());
            ASSERT_NO_FATAL_FAILURE(expectWorld(
                packWorld, expected, packFingerprints[phase][platformIndex]));
        }
    }
    for (const auto& fingerprints : {currentFingerprints, pieceFingerprints, packFingerprints}) {
        EXPECT_EQ(fingerprints[1], fingerprints[0]);
        EXPECT_NE(fingerprints[2][0], fingerprints[0][0]);
        EXPECT_EQ(fingerprints[2][1], fingerprints[0][1]);
    }
}

TEST_F(EditorFieldGroupCompilation, ContainerOnlyEntryIncludesOwnedSoundAndEffectiveDependencies)
{
    using namespace VNX;
    auto* action = project_.actions.front().rootItem->children.front()->comp<VAction>();
    VActionStep step;
    step.type = VActionStep::Type::Play;
    step.targetObjectID = 47;
    action->steps = {step};
    addDormantHistory();
    const auto closure = ActionClosureSemantics::build(
        project_, harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias),
        {HarkenTest::platformIdString("Android"), QString()});
    ASSERT_TRUE(closure.ready());
    EXPECT_EQ(std::count_if(closure.objects.begin(), closure.objects.end(), [&](const auto& item) {
        return item.stableId == sound_->runtimeId();
    }), 1);
    ASSERT_EQ(closure.media.size(), 1u);
    EXPECT_EQ(closure.media.front().soundStableId, sound_->runtimeId());
    using Reason = ActionClosureEdgeReason;
    std::map<Reason, std::set<harkenstudio::ObjectId>> dependencies;
    for (const auto& edge : closure.edges)
        if (edge.fromStableId == sound_->runtimeId())
            dependencies[edge.reason].insert(edge.toStableId);
    const auto& expected = expected_[1];
    const std::map<Reason, std::set<harkenstudio::ObjectId>> expectedDependencies{
        {Reason::EffectiveOutputBus, {expected.outputBusId}},
        {Reason::EffectiveSend, {expected.sendBusId}},
        {Reason::EffectiveAttenuation, {expected.attenuationId}}};
    EXPECT_EQ(dependencies, expectedDependencies);
    ProjectPackExporter exporter;
    ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Android");
    options.language = QString();
    const QString outputDir = QDir(directory_.path()).filePath(QStringLiteral("container-only"));
    const auto exported = exporter.ExportAllPacks(project_, outputDir, options);
    ASSERT_TRUE(exported.success) << exported.error.toStdString();
    const QString path = variantPackPath(project_,
        outputDir, harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias), QStringLiteral("Android"));
    PackImage image;
    ASSERT_NO_FATAL_FAILURE(readPack(path, image));
    harken::PackCanonicalImage canonical;
    ASSERT_EQ(harken::ParsePackCanonicalImage(
                  image.data(), static_cast<uint64_t>(image.bytes.size()), &canonical), VN_OK);
    ASSERT_TRUE(canonical.hasPartition);
    ASSERT_EQ(canonical.media.size(), 1u);
    EXPECT_EQ(canonical.media.front().soundId, sound_->runtimeId());
    harken::CurrentWorldBodyImage decoded;
    ASSERT_TRUE(decoded.Decode(
        canonical.partitionBody.data(), canonical.partitionBody.size(),
        canonical.partitionHeader.bodyVintage, harken::CurrentWorldDecodeMode::StrictArtifact));
    ASSERT_NE(decoded.ProductionView(), nullptr);
    const auto& world = *decoded.ProductionView();
    ASSERT_GT(world.playablePropertyCount, 0u);
    const auto* end = world.playableProperties + world.playablePropertyCount;
    const auto* sound = std::find_if(world.playableProperties, end, [&](const auto& row) {
        return row.stableId == sound_->runtimeId();
    });
    ASSERT_NE(sound, end);
    EXPECT_EQ(sound->payload.presentMask & kFieldGroupPropertyMask, kFieldGroupPropertyMask);
    ASSERT_EQ(sound->payload.sendCount, 1u);
    EXPECT_EQ(sound->payload.sends[0].targetBusId, expected.sendBusId);
    EXPECT_FLOAT_EQ(sound->payload.sends[0].sendLevel, expected.send.sends[0].sendLevel);
}

TEST(EditorProjectPackExporterSharedPack, SharedMediaIncludedInEveryActionPack)
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
        {{QStringLiteral("ActionOne"), kActionOneObjectId, kSharedSoundObjectId},
         {QStringLiteral("ActionTwo"), kActionTwoObjectId, kSharedSoundObjectId},
         {QStringLiteral("ActionThree"), kActionThreeObjectId, kSoloSoundObjectId}}));

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");

    std::vector<VNX::ProjectPackExporter::PackBuildRecord> records;
    auto result = exporter.ExportAllPacks(project, dir.path(), opts, &records);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    qint64 recordedBytes = 0;
    for (const auto& record : records)
    {
        recordedBytes += record.fileSize;
        EXPECT_NE(record.packName, QStringLiteral("Shared"));
    }
    EXPECT_EQ(result.mediaCount, 3);
    EXPECT_EQ(result.fileSize, recordedBytes);

    const QString sharedPath = QDir(dir.path()).filePath(QStringLiteral("Shared.bytes"));
    EXPECT_FALSE(QFileInfo::exists(sharedPath));

    for (const auto& [packName, stableId] :
         std::vector<std::pair<QString, harkenstudio::ObjectId>>{
             {QStringLiteral("ActionOne"), harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias)},
             {QStringLiteral("ActionTwo"), harkentest::StudioObjectId(kActionTwoObjectId + kStableIdBias)}})
    {
        const QString packPath = variantPackPath(project, dir.path(), stableId);
        ASSERT_TRUE(QFileInfo::exists(packPath)) << packName.toStdString();
        PackImage image;
        ASSERT_NO_FATAL_FAILURE(readPack(packPath, image));
        EXPECT_EQ(image.countKind(harken::kPackEntryKindMedia), 1)
            << packName.toStdString();
        const auto tocs = readMediaTocs(image);
        ASSERT_EQ(tocs.size(), 1u);
        EXPECT_EQ(tocs[0].soundId, harkentest::StudioObjectId(kSharedSoundObjectId + kStableIdBias));
        EXPECT_EQ(image.countKind(harken::kPackEntryKindAction), 0);
        EXPECT_EQ(image.header.packKind, harken::kPackKindCanonicalContent);
    }

    const QString soloPackPath =
        variantPackPath(project, dir.path(), harkentest::StudioObjectId(kActionThreeObjectId + kStableIdBias));
    ASSERT_TRUE(QFileInfo::exists(soloPackPath));
    PackImage soloImage;
    ASSERT_NO_FATAL_FAILURE(readPack(soloPackPath, soloImage));
    const auto soloTocs = readMediaTocs(soloImage);
    ASSERT_EQ(soloTocs.size(), 1u);
    EXPECT_EQ(soloTocs[0].soundId, harkentest::StudioObjectId(kSoloSoundObjectId + kStableIdBias));

    VnConfig config{};
    config.structSize = sizeof(VnConfig);
    config.deviceMode = VN_DEVICE_NULL;
    ASSERT_EQ(VN_OK, Vn_Init(&config));
    PackImage initImage;
    ASSERT_NO_FATAL_FAILURE(readPack(
        QDir(dir.path()).filePath(QStringLiteral("Init.bytes")), initImage));
    EXPECT_EQ(initImage.header.packKind, harken::kPackKindInit);
    VnPackHandle initHandle = 0;
    EXPECT_EQ(VN_PACK_INIT_LIFECYCLE, loadPack(QDir(dir.path()).filePath(QStringLiteral("Init.bytes")),
                              &initHandle));
    EXPECT_EQ(VN_OK, Vn_Term());

    EXPECT_EQ(initImage.countKind(harken::kPackEntryKindInitRoutingCatalog), 0);
    QFile manifestFile(QDir(dir.path()).filePath(HarkenEditor::Bridge::packManifestFileName()));
    ASSERT_TRUE(manifestFile.open(QIODevice::ReadOnly));
    const QJsonObject manifest = QJsonDocument::fromJson(manifestFile.readAll()).object();
    EXPECT_TRUE(manifest.value(QStringLiteral("language")).toString().isEmpty());
    const QJsonArray packs = manifest.value(QStringLiteral("packs")).toArray();
    ASSERT_EQ(packs.size(), 4);
    for (const QJsonValue& value : packs) {
        const QJsonObject pack = value.toObject();
        EXPECT_TRUE(pack.value(QStringLiteral("language")).toString().isEmpty());
        const QString path = pack.value(QStringLiteral("path")).toString();
        EXPECT_EQ(QFileInfo(path).fileName(), path);
    }

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterSharedPack,
     ProjectLanguagesWithoutVoiceSoundsKeepIndependentActions)
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
        {{QStringLiteral("ActionOne"), kActionOneObjectId, kSharedSoundObjectId},
         {QStringLiteral("ActionTwo"), kActionTwoObjectId, kSharedSoundObjectId},
         {QStringLiteral("ActionThree"), kActionThreeObjectId, kSoloSoundObjectId}}));
    project.projectLanguages = {QStringLiteral("English"), QStringLiteral("Japanese")};

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    const auto result = exporter.ExportAllPacks(project, dir.path(), opts);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    EXPECT_FALSE(QFileInfo::exists(
        QDir(dir.path()).filePath(QStringLiteral("Shared.bytes"))));
    EXPECT_TRUE(QFileInfo::exists(
        variantPackPath(project, dir.path(), harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias))));
    EXPECT_FALSE(QFileInfo::exists(QDir(dir.path()).filePath(
        QStringLiteral("English/ActionOne.bytes"))));

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterSharedPack, SharedBindingsKeepPlaybackAndRejectConflictingRevisions)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& assets = VNX::AssetRegistry::instance();
    assets.clear();
    assets.setProjectRoot(dir.path());
    const auto source = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 1.0f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), source, source,
        {{QStringLiteral("A"), kActionOneObjectId, kSharedSoundObjectId},
         {QStringLiteral("B"), kActionTwoObjectId, kSharedSoundObjectId}}));
    auto variable = makeItem(QStringLiteral("Drive"), VNX::ProjectItem::Type::Variable, 50);
    auto* variableComponent = variable->comp<VNX::VVariable>();
    variableComponent->kind = VNX::VariableKind::Smooth;
    variableComponent->rangeMin = 0.0f;
    variableComponent->rangeMax = 1.0f;
    variableComponent->defaultValue = 0.0f;
    VNX::ProjectFileBinding variableRoot;
    variableRoot.rootItem = variable;
    project.variables.push_back(variableRoot);
    VNX::ParameterBindingDef voiceBinding;
    voiceBinding.guid = VNX::authoringUuid(harkentest::StudioObjectId(500));
    voiceBinding.variable = variable->guid;
    voiceBinding.address = VNX::BindingSemantics::volumeAddress();
    voiceBinding.curve = {{0.0f, -6.0f}, {1.0f, 0.0f}};
    voiceBinding.outputMin = -12.0f;
    voiceBinding.outputMax = 0.0f;
    auto* layerMix = project.layers.front().rootItem->comp<VNX::VMixNode>();
    ASSERT_TRUE(layerMix->bindings.upsert(voiceBinding));
    auto busBinding = voiceBinding;
    busBinding.guid = VNX::authoringUuid(harkentest::StudioObjectId(501));
    busBinding.address = VNX::BindingSemantics::volumeAddress(VNX::ProjectItemType::Bus);
    ASSERT_TRUE(project.busses.front().rootItem->comp<VNX::VMixNode>()->bindings.upsert(busBinding));
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    const QString baseDir = QDir(dir.path()).filePath(QStringLiteral("base"));
    const QString nextDir = QDir(dir.path()).filePath(QStringLiteral("next"));
    const QString globalDir = QDir(dir.path()).filePath(QStringLiteral("globals"));
    auto result = exporter.ExportAllPacks(project, baseDir, options);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    voiceBinding.curve[0].value = -12.0f;
    ASSERT_TRUE(layerMix->bindings.upsert(voiceBinding));
    result = exporter.ExportAllPacks(project, nextDir, options);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    voiceBinding.curve[0].value = -6.0f;
    ASSERT_TRUE(layerMix->bindings.upsert(voiceBinding));
    variableComponent->defaultValue = 0.5f;
    result = exporter.ExportAllPacks(project, globalDir, options);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    const auto aId = harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias);
    const auto bId = harkentest::StudioObjectId(kActionTwoObjectId + kStableIdBias);
    CanonicalPackActivation loaded;
    const QString aPath = variantPackPath(project, baseDir, aId);
    const QString bPath = variantPackPath(project, baseDir, bId);
    ASSERT_EQ(VN_OK, activateCanonicalPackFiles(
        {QDir(baseDir).filePath(QStringLiteral("Init.bytes")), aPath, bPath},
        &loaded));
    ASSERT_EQ(3u, loaded.rows.size());
    const auto handleFor = [&](const QString& path) {
        const QByteArray utf8 = QFileInfo(path).absoluteFilePath().toUtf8();
        for (size_t i = 0; i < loaded.pathUtf8.size(); ++i) {
            if (loaded.pathUtf8[i] == utf8)
                return loaded.rows[i].handle;
        }
        return VnPackHandle{0};
    };
    const VnPackHandle init = loaded.report.initHandle;
    const VnPackHandle a = handleFor(aPath);
    const VnPackHandle b = handleFor(bPath);
    ASSERT_NE(0u, init);
    ASSERT_NE(0u, a);
    ASSERT_NE(0u, b);
    EXPECT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(501)));
    const auto originalMedia = engine->registry.Lookup(harkentest::ObjectId(kSharedSoundObjectId + kStableIdBias));
    ASSERT_TRUE(originalMedia.Valid());
    const auto generation = engine->packComposition.CompositeGeneration();
    EXPECT_EQ(VN_BAD_PACK, replaceCanonicalPackFile(variantPackPath(project, nextDir, aId), a));
    EXPECT_EQ(generation, engine->packComposition.CompositeGeneration());
    EXPECT_NE(harkentest::StudioObjectId(0u), engine->packComposition.Facts().conflictingStableId);
    EXPECT_EQ(VN_PACK_STALE_REVISION, replaceCanonicalPackFile(variantPackPath(project, globalDir, aId), a));
    EXPECT_EQ(generation, engine->packComposition.CompositeGeneration());

    VnPostActionDesc post{};
    post.structSize = sizeof(post);
    post.actionId = harkentest::ToRuntime(aId);
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&post.emitter));
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&post, &firing));
    std::uint32_t bindings = 0;
    engine->params.ForEachInstance([&](const auto& row) {
        if (row.definitionId == harkentest::StudioObjectId(500)) {
            ++bindings;
            EXPECT_FLOAT_EQ(-6.0f, row.lastContribution);
            EXPECT_EQ(post.emitter, row.emitterHandle);
        }
    });
    ASSERT_EQ(1u, bindings);
    ASSERT_EQ(VN_OK, Vn_UnloadPack(b));
    ASSERT_EQ(VN_OK, replaceCanonicalPackFile(variantPackPath(project, nextDir, aId), a));
    EXPECT_EQ(originalMedia.channels[0],
              engine->registry.Lookup(harkentest::ObjectId(kSharedSoundObjectId + kStableIdBias)).channels[0]);
    engine->params.ForEachInstance([&](const auto& row) {
        if (row.definitionId == harkentest::StudioObjectId(500)) EXPECT_FLOAT_EQ(-6.0f, row.lastContribution);
    });
    ASSERT_EQ(VN_OK, Vn_UnloadPack(a));
    EXPECT_FALSE(engine->registry.Lookup(harkentest::ObjectId(kSharedSoundObjectId + kStableIdBias)).Valid());
    for (int i = 0; i < 128; ++i) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
    }
    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, engine->packs.RetiringCount());
    bindings = 0;
    engine->params.ForEachInstance([&](const auto&) { ++bindings; });
    EXPECT_EQ(0u, bindings);
    assets.clear();
}

TEST(EditorProjectPackExporterSharedPack, BindingOnlyChangesReviseCanonicalPackIdentity)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& assets = VNX::AssetRegistry::instance();
    assets.clear();
    assets.setProjectRoot(dir.path());
    const auto source = registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 1.0f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(project, dir.path(), source, source,
        {{QStringLiteral("A"), kActionOneObjectId, kSharedSoundObjectId}}));
    const auto variable = attachDriveVariable(project);
    ASSERT_NO_FATAL_FAILURE(attachSharedVolumeBindings(project, *variable));
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    const auto exportRevisions = [&](const QString& name,
                                      std::map<std::uint64_t, std::uint64_t>& revisions) {
        const QString output = QDir(dir.path()).filePath(name);
        const auto result = exporter.ExportAllPacks(project, output, options);
        ASSERT_TRUE(result.success) << result.error.toStdString();
        QFile manifest(QDir(output).filePath(QStringLiteral("HarkenPackManifest.json")));
        ASSERT_TRUE(manifest.open(QIODevice::ReadOnly));
        const auto json = QJsonDocument::fromJson(manifest.readAll()).object();
        const auto rows = json.value(QStringLiteral("packs")).toArray();
        const auto initId = json.value(QStringLiteral("initPackStableId")).toString().toULongLong();
        for (const auto& value : rows) {
            const auto row = value.toObject();
            const auto id = row.value(QStringLiteral("packStableId")).toString().toULongLong();
            const auto revision = static_cast<std::uint64_t>(
                row.value(QStringLiteral("contentRevision")).toInteger());
            revisions[id] = revision;
            PackImage image;
            ASSERT_NO_FATAL_FAILURE(readPack(QDir(output).filePath(
                row.value(QStringLiteral("path")).toString()), image));
            bool found = false;
            for (const auto& entry : image.entries) {
                if (entry.kind != harken::kPackEntryKindPackRequirementBlock) continue;
                harken::PackRequirementBlock requirement{};
                ASSERT_TRUE(harken::ParsePackRequirementBlockV2(
                    image.data() + entry.offset, entry.size, &requirement));
                EXPECT_EQ(revision, requirement.packContentRevision);
                found = true;
            }
            EXPECT_TRUE(found);
        }
        for (const auto& value : rows) {
            const auto row = value.toObject();
            PackImage image;
            ASSERT_NO_FATAL_FAILURE(readPack(QDir(output).filePath(
                row.value(QStringLiteral("path")).toString()), image));
            for (const auto& entry : image.entries) {
                if (entry.kind != harken::kPackEntryKindPackDependencyEdge) continue;
                harken::PackDependencyEdge edge{};
                ASSERT_TRUE(harken::ParsePackDependencyEdgeV2(
                    image.data() + entry.offset, entry.size, &edge));
                if (edge.prerequisitePackStableId == initId)
                    EXPECT_EQ(revisions.at(initId), edge.expectedPrerequisiteContentRevision);
            }
        }
    };
    std::map<std::uint64_t, std::uint64_t> base, repeated, voiceChanged, busChanged;
    ASSERT_NO_FATAL_FAILURE(exportRevisions(QStringLiteral("base"), base));
    ASSERT_NO_FATAL_FAILURE(exportRevisions(QStringLiteral("repeat"), repeated));
    EXPECT_EQ(base, repeated);
    auto* voiceMix = project.layers.front().rootItem->comp<VNX::VMixNode>();
    auto voice = *voiceMix->bindings.find(harkentest::StudioObjectId(500));
    voice.curve[0].value = -12.0f;
    ASSERT_TRUE(voiceMix->bindings.upsert(voice));
    ASSERT_NO_FATAL_FAILURE(exportRevisions(QStringLiteral("voice"), voiceChanged));
    EXPECT_NE(base, voiceChanged);
    auto* busMix = project.busses.front().rootItem->comp<VNX::VMixNode>();
    auto bus = *busMix->bindings.find(harkentest::StudioObjectId(501));
    bus.curve[0].value = -9.0f;
    ASSERT_TRUE(busMix->bindings.upsert(bus));
    ASSERT_NO_FATAL_FAILURE(exportRevisions(QStringLiteral("bus"), busChanged));
    EXPECT_NE(voiceChanged, busChanged);
    assets.clear();
}

TEST(EditorProjectPackExporterSharedPack, FormalExportLoadPublishesBindingIdentities)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& assets = VNX::AssetRegistry::instance();
    assets.clear();
    assets.setProjectRoot(dir.path());
    const auto source =
        registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 1.0f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), source, source,
        {{QStringLiteral("A"), kActionOneObjectId, kSharedSoundObjectId},
         {QStringLiteral("B"), kActionTwoObjectId, kSharedSoundObjectId}}));
    auto variable = attachDriveVariable(project);
    ASSERT_NO_FATAL_FAILURE(attachSharedVolumeBindings(project, *variable));

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    const auto result = exporter.ExportAllPacks(project, dir.path(), options);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QString initPath =
        QDir(dir.path()).filePath(QStringLiteral("Init.bytes"));
    const QString contentPath =
        variantPackPath(project, dir.path(), harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias));
    PackImage initImage;
    ASSERT_NO_FATAL_FAILURE(readPack(initPath, initImage));
    EXPECT_GE(initImage.countKind(harken::kPackEntryKindBindingDefinition), 1);
    EXPECT_GE(initImage.countKind(harken::kPackEntryKindBindingAttachment), 1);

    harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    CanonicalPackActivation loaded;
    ASSERT_EQ(VN_OK, activateCanonicalPackFiles({initPath, contentPath}, &loaded));
    ASSERT_EQ(2u, loaded.rows.size());
    const VnPackHandle init = loaded.report.initHandle;
    const VnPackHandle content = loaded.rows[1].handle;
    ASSERT_NE(0u, init);
    ASSERT_NE(0u, content);
    EXPECT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(501)));

    const auto* published = engine->packComposition.Published();
    ASSERT_NE(published, nullptr);
    std::uint32_t voiceRows = 0;
    std::uint64_t contentRevision = 0;
    for (const auto& owner : published->owners) {
        if (!owner.voiceBindings)
            continue;
        for (const auto& row : *owner.voiceBindings) {
            if (row.bindingId != harkentest::StudioObjectId(500))
                continue;
            ++voiceRows;
            EXPECT_NE(harkentest::StudioObjectId(0u), row.objectId);
            EXPECT_EQ(static_cast<std::uint32_t>(VN_TARGET_SCOPE_VOICE),
                      row.descriptor.targetScopeKind);
            EXPECT_EQ(static_cast<std::uint32_t>(VN_TARGET_VOICE_VOLUME),
                      row.descriptor.targetId);
            contentRevision = owner.contentRevision;
        }
    }
    ASSERT_GE(voiceRows, 1u);
    EXPECT_NE(0u, contentRevision);
    assets.clear();
}

TEST(EditorProjectPackExporterSharedPack, ReplaceMissingBindingDefinitionRollsBack)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& assets = VNX::AssetRegistry::instance();
    assets.clear();
    assets.setProjectRoot(dir.path());
    const auto source =
        registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 1.0f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), source, source,
        {{QStringLiteral("A"), kActionOneObjectId, kSharedSoundObjectId},
         {QStringLiteral("B"), kActionTwoObjectId, kSharedSoundObjectId}}));
    auto variable = attachDriveVariable(project);
    ASSERT_NO_FATAL_FAILURE(attachSharedVolumeBindings(project, *variable));
    auto* sharedSound = project.layers.front().rootItem->children.front().get();
    ASSERT_NE(sharedSound, nullptr);
    VNX::ParameterBindingDef soundBinding;
    soundBinding.guid = VNX::authoringUuid(harkentest::StudioObjectId(502));
    soundBinding.variable = variable->guid;
    soundBinding.address = VNX::BindingSemantics::volumeAddress();
    soundBinding.curve = {{0.0f, -3.0f}, {1.0f, 0.0f}};
    soundBinding.outputMin = -12.0f;
    soundBinding.outputMax = 0.0f;
    ASSERT_TRUE(sharedSound->comp<VNX::VMixNode>()->bindings.upsert(soundBinding));

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    const auto result = exporter.ExportAllPacks(project, dir.path(), options);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    const QString initPath =
        QDir(dir.path()).filePath(QStringLiteral("Init.bytes"));
    const QString contentPath =
        variantPackPath(project, dir.path(), harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias));
    harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    auto* engine = session.Engine();
    CanonicalPackActivation loaded;
    ASSERT_EQ(VN_OK, activateCanonicalPackFiles({initPath, contentPath}, &loaded));
    ASSERT_EQ(2u, loaded.rows.size());
    const VnPackHandle init = loaded.report.initHandle;
    const VnPackHandle content = loaded.rows[1].handle;
    ASSERT_NE(0u, init);
    ASSERT_NE(0u, content);
    ASSERT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(501)));
    const auto* published = engine->packComposition.Published();
    ASSERT_NE(published, nullptr);
    std::shared_ptr<const std::vector<harken::PackVoiceBinding>> liveBindings;
    for (const auto& owner : published->owners) {
        if (!owner.voiceBindings)
            continue;
        for (const auto& row : *owner.voiceBindings) {
            if (row.bindingId == harkentest::StudioObjectId(500)) {
                liveBindings = owner.voiceBindings;
                break;
            }
        }
        if (liveBindings)
            break;
    }
    ASSERT_TRUE(liveBindings);
    const auto generation = engine->packComposition.CompositeGeneration();

    PackImage contentImage;
    ASSERT_NO_FATAL_FAILURE(readPack(contentPath, contentImage));
    ASSERT_GE(contentImage.countKind(harken::kPackEntryKindBindingDefinition), 1);
    ASSERT_GE(contentImage.countKind(harken::kPackEntryKindBindingAttachment), 1);
    const QString mutatedPath =
        QDir(dir.path()).filePath(QStringLiteral("mutated-missing-definition.bytes"));
    QString writeError;
    ASSERT_TRUE(writePackWithoutKind(contentImage, mutatedPath,
                                     harken::kPackEntryKindBindingDefinition,
                                     &writeError))
        << writeError.toStdString();
    PackImage mutatedImage;
    ASSERT_NO_FATAL_FAILURE(readPack(mutatedPath, mutatedImage));
    EXPECT_EQ(mutatedImage.countKind(harken::kPackEntryKindBindingDefinition), 0);
    EXPECT_GE(mutatedImage.countKind(harken::kPackEntryKindBindingAttachment), 1);

    EXPECT_EQ(VN_BAD_PACK, replaceCanonicalPackFile(mutatedPath, content));
    EXPECT_EQ(generation, engine->packComposition.CompositeGeneration());
    EXPECT_NE(nullptr, engine->bindings.Find(harkentest::ObjectId(501)));
    const auto* after = engine->packComposition.Published();
    ASSERT_NE(after, nullptr);
    bool kept = false;
    for (const auto& owner : after->owners) {
        if (owner.voiceBindings == liveBindings)
            kept = true;
        if (!owner.voiceBindings)
            continue;
        for (const auto& row : *owner.voiceBindings) {
            if (row.bindingId == harkentest::StudioObjectId(500))
                kept = true;
        }
    }
    EXPECT_TRUE(kept);
    assets.clear();
}

TEST(EditorProjectPackExporterSharedPack, SingleOwnerMediaProducesNoSharedPack)
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
        {{QStringLiteral("ActionOne"), kActionOneObjectId, kSharedSoundObjectId},
         {QStringLiteral("ActionTwo"), kActionTwoObjectId, kSoloSoundObjectId}}));

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");

    std::vector<VNX::ProjectPackExporter::PackBuildRecord> records;
    auto result = exporter.ExportAllPacks(project, dir.path(), opts, &records);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    EXPECT_FALSE(QFileInfo::exists(
        QDir(dir.path()).filePath(QStringLiteral("Shared.bytes"))));
    for (const auto& record : records)
        EXPECT_NE(record.packName, QStringLiteral("Shared"));

    for (const auto& [packName, expectedSoundId] :
         std::vector<std::pair<QString, harkenstudio::ObjectId>>{
             {QStringLiteral("ActionOne"), harkentest::StudioObjectId(kSharedSoundObjectId + kStableIdBias)},
             {QStringLiteral("ActionTwo"), harkentest::StudioObjectId(kSoloSoundObjectId + kStableIdBias)}})
    {
        const harkenstudio::ObjectId stableId = packName == QLatin1String("ActionOne")
            ? harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias)
            : harkentest::StudioObjectId(kActionTwoObjectId + kStableIdBias);
        const QString packPath = variantPackPath(project, dir.path(), stableId);
        ASSERT_TRUE(QFileInfo::exists(packPath)) << packName.toStdString();
        PackImage image;
        ASSERT_NO_FATAL_FAILURE(readPack(packPath, image));
        const auto tocs = readMediaTocs(image);
        ASSERT_EQ(tocs.size(), 1u) << packName.toStdString();
        EXPECT_EQ(tocs[0].soundId, expectedSoundId);
    }

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterSharedPack, GroupedActionsShareOnePackAndMediaOwner)
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
        {{QStringLiteral("ActionOne"), kActionOneObjectId, kSharedSoundObjectId},
         {QStringLiteral("ActionTwo"), kActionTwoObjectId, kSharedSoundObjectId},
         {QStringLiteral("ActionThree"), kActionThreeObjectId, kSoloSoundObjectId}}));

    VNX::PackGroup group;
    group.name = QStringLiteral("Grouped");
    group.actionIds = {harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias),
                       harkentest::StudioObjectId(kActionTwoObjectId + kStableIdBias)};
    project.packGroups.push_back(group);

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");

    std::vector<VNX::ProjectPackExporter::PackBuildRecord> records;
    auto result = exporter.ExportAllPacks(project, dir.path(), opts, &records);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    EXPECT_FALSE(QFileInfo::exists(
        QDir(dir.path()).filePath(QStringLiteral("Shared.bytes"))));
    const auto groupRecord = std::find_if(records.begin(), records.end(), [](const auto& record) {
        return record.source == VNX::ProjectPackExporter::PackBuildRecord::Source::Group;
    });
    ASSERT_NE(groupRecord, records.end());
    EXPECT_EQ(groupRecord->packName, QStringLiteral("Grouped"));
    EXPECT_TRUE(groupRecord->success);
    EXPECT_EQ(QDir(dir.path()).entryList({QStringLiteral("*.bytes")}, QDir::Files).size(), 3);
    const QString groupPath = QDir(dir.path()).filePath(QStringLiteral("Grouped.bytes"));
    PackImage groupImage;
    ASSERT_NO_FATAL_FAILURE(readPack(groupPath, groupImage));
    EXPECT_EQ(groupImage.countKind(harken::kPackEntryKindAction), 0);
    EXPECT_EQ(groupImage.countKind(harken::kPackEntryKindMedia), 1);
    EXPECT_EQ(groupImage.header.packKind, harken::kPackKindCanonicalContent);
    harken::PackCanonicalImage canonical;
    ASSERT_EQ(VN_OK, harken::ParsePackCanonicalImage(groupImage.data(), groupImage.bytes.size(), &canonical));
    harken::CurrentWorldBodyImage body;
    ASSERT_TRUE(body.Decode(canonical.partitionBody.data(), canonical.partitionBody.size(),
                           canonical.partitionHeader.bodyVintage, harken::CurrentWorldDecodeMode::StrictArtifact));
    ASSERT_NE(body.ProductionView(), nullptr);
    ASSERT_EQ(body.ProductionView()->actionCount, 2u);
    std::set<harkenstudio::ObjectId> actionIds;
    for (uint32_t index = 0; index < body.ProductionView()->actionCount; ++index)
        actionIds.insert(harkentest::ToStudio(body.ProductionView()->actions[index].actionStableId));
    EXPECT_EQ(actionIds, (std::set<harkenstudio::ObjectId>{
        harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias),
        harkentest::StudioObjectId(kActionTwoObjectId + kStableIdBias)}));
    for (harkenstudio::ObjectId stableId : {harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias),
                              harkentest::StudioObjectId(kActionTwoObjectId + kStableIdBias)}) {
        const QString actionPath = variantPackPath(project, dir.path(), stableId);
        EXPECT_EQ(actionPath, groupPath);
    }
    EXPECT_FALSE(QFileInfo::exists(QDir(dir.path()).filePath(QStringLiteral("ActionOne.bytes"))));
    EXPECT_FALSE(QFileInfo::exists(QDir(dir.path()).filePath(QStringLiteral("ActionTwo.bytes"))));

    const QString soloPath =
        variantPackPath(project, dir.path(), harkentest::StudioObjectId(kActionThreeObjectId + kStableIdBias));
    ASSERT_TRUE(QFileInfo::exists(soloPath));
    PackImage soloImage;
    ASSERT_NO_FATAL_FAILURE(readPack(soloPath, soloImage));
    EXPECT_EQ(soloImage.countKind(harken::kPackEntryKindAction), 0);
    const auto soloTocs = readMediaTocs(soloImage);
    ASSERT_EQ(soloTocs.size(), 1u);
    EXPECT_EQ(soloTocs[0].soundId, harkentest::StudioObjectId(kSoloSoundObjectId + kStableIdBias));

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterSharedPack, GroupPlatformSelectionSkipsWithoutFallback)
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
        {{QStringLiteral("ActionOne"), kActionOneObjectId, kSharedSoundObjectId},
         {QStringLiteral("ActionTwo"), kActionTwoObjectId, kSoloSoundObjectId}}));

    VNX::PackGroup group;
    group.name = QStringLiteral("AndroidOnly");
    group.actionIds = {harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias)};
    group.allPlatforms = false;
    group.platformIds = {HarkenTest::platformIdString("Android")};
    project.packGroups.push_back(group);

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");

    std::vector<VNX::ProjectPackExporter::PackBuildRecord> records;
    auto result = exporter.ExportAllPacks(project, dir.path(), opts, &records);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    EXPECT_FALSE(QFileInfo::exists(
        QDir(dir.path()).filePath(QStringLiteral("AndroidOnly.bytes"))));
    EXPECT_FALSE(QFileInfo::exists(
        variantPackPath(project, dir.path(), harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias))));
    for (const auto& record : records)
        EXPECT_NE(record.packName, QStringLiteral("AndroidOnly"));

    EXPECT_TRUE(QFileInfo::exists(
        variantPackPath(project, dir.path(), harkentest::StudioObjectId(kActionTwoObjectId + kStableIdBias))));

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterSharedPack, ActionTargetingSkipsUntargetedPlatform)
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
        {{QStringLiteral("ActionOne"), kActionOneObjectId, kSharedSoundObjectId},
         {QStringLiteral("ActionTwo"), kActionTwoObjectId, kSoloSoundObjectId}}));

    project.actionPackTargeting.push_back(
        {harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias), false, {QStringLiteral("Android")}});

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");

    std::vector<VNX::ProjectPackExporter::PackBuildRecord> records;
    auto result = exporter.ExportAllPacks(project, dir.path(), opts, &records);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    EXPECT_FALSE(QFileInfo::exists(
        variantPackPath(project, dir.path(), harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias))));
    for (const auto& record : records)
        EXPECT_NE(record.packName, QStringLiteral("ActionOne"));

    EXPECT_TRUE(QFileInfo::exists(
        variantPackPath(project, dir.path(), harkentest::StudioObjectId(kActionTwoObjectId + kStableIdBias))));

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterSharedPack, SharedOwnershipIgnoresUnitsNotBuiltForPlatform)
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
        {{QStringLiteral("ActionOne"), kActionOneObjectId, kSharedSoundObjectId},
         {QStringLiteral("ActionTwo"), kActionTwoObjectId, kSharedSoundObjectId}}));

    project.actionPackTargeting.push_back(
        {harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias), false, {QStringLiteral("Android")}});

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");

    std::vector<VNX::ProjectPackExporter::PackBuildRecord> records;
    auto result = exporter.ExportAllPacks(project, dir.path(), opts, &records);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    EXPECT_FALSE(QFileInfo::exists(
        QDir(dir.path()).filePath(QStringLiteral("Shared.bytes"))));
    for (const auto& record : records)
        EXPECT_NE(record.packName, QStringLiteral("Shared"));

    const QString builtPath =
        variantPackPath(project, dir.path(), harkentest::StudioObjectId(kActionTwoObjectId + kStableIdBias));
    ASSERT_TRUE(QFileInfo::exists(builtPath));
    PackImage image;
    ASSERT_NO_FATAL_FAILURE(readPack(builtPath, image));
    const auto tocs = readMediaTocs(image);
    ASSERT_EQ(tocs.size(), 1u);
    EXPECT_EQ(tocs[0].soundId, harkentest::StudioObjectId(kSharedSoundObjectId + kStableIdBias));

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterSharedPack, GroupTargetedNowhereBuildsOnNoPlatform)
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
        {{QStringLiteral("ActionOne"), kActionOneObjectId, kSharedSoundObjectId},
         {QStringLiteral("ActionTwo"), kActionTwoObjectId, kSoloSoundObjectId}}));

    VNX::PackGroup group;
    group.name = QStringLiteral("Offline");
    group.actionIds = {harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias)};
    group.allPlatforms = false;
    project.packGroups.push_back(group);

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");

    std::vector<VNX::ProjectPackExporter::PackBuildRecord> records;
    auto result = exporter.ExportAllPacks(project, dir.path(), opts, &records);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    EXPECT_FALSE(QFileInfo::exists(
        QDir(dir.path()).filePath(QStringLiteral("Offline.bytes"))));
    EXPECT_FALSE(QFileInfo::exists(
        variantPackPath(project, dir.path(), harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias))));
    for (const auto& record : records)
        EXPECT_NE(record.packName, QStringLiteral("Offline"));

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterSharedPack, InitEmitsKind26AndContentEmitsKind27)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& assets = VNX::AssetRegistry::instance();
    assets.clear();
    assets.setProjectRoot(dir.path());
    const auto source =
        registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 1.0f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), source, source,
        {{QStringLiteral("PlayDrive"), kActionOneObjectId, kSharedSoundObjectId}}));
    auto variable = makeItem(QStringLiteral("Drive"), VNX::ProjectItem::Type::Variable, 50);
    auto* variableComponent = variable->comp<VNX::VVariable>();
    variableComponent->kind = VNX::VariableKind::Smooth;
    variableComponent->rangeMin = 0.0f;
    variableComponent->rangeMax = 1.0f;
    variableComponent->defaultValue = 0.0f;
    VNX::ProjectFileBinding variableRoot;
    variableRoot.rootItem = variable;
    project.variables.push_back(variableRoot);
    VNX::ParameterBindingDef binding;
    binding.guid = VNX::authoringUuid(harkentest::StudioObjectId(500));
    binding.variable = variable->guid;
    binding.address = VNX::BindingSemantics::volumeAddress();
    binding.curve = {{0.0f, -6.0f}, {1.0f, 0.0f}};
    binding.outputMin = -12.0f;
    binding.outputMax = 0.0f;
    auto* layerMix = project.layers.front().rootItem->comp<VNX::VMixNode>();
    ASSERT_NE(layerMix, nullptr);
    ASSERT_TRUE(layerMix->bindings.upsert(binding));
    VNX::ProjectItem* actionItem = nullptr;
    for (const auto& child : project.actions.front().rootItem->children) {
        if (child && child->name == QStringLiteral("PlayDrive"))
            actionItem = child.get();
    }
    ASSERT_NE(actionItem, nullptr);
    auto* actionComponent = actionItem->comp<VNX::VAction>();
    ASSERT_NE(actionComponent, nullptr);
    VNX::VActionStep setValue;
    setValue.type = VNX::VActionStep::Type::SetVariable;
    setValue.targetObjectID = variable->objectID;
    actionComponent->steps.push_back(setValue);

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    const auto result = exporter.ExportAllPacks(project, dir.path(), options);
    ASSERT_TRUE(result.success) << result.error.toStdString();

    PackImage initImage;
    ASSERT_NO_FATAL_FAILURE(
        readPack(QDir(dir.path()).filePath(QStringLiteral("Init.bytes")), initImage));
    harken::PackCanonicalImage initCanonical;
    ASSERT_EQ(VN_OK, harken::ParsePackCanonicalImage(
                         initImage.data(),
                         static_cast<uint64_t>(initImage.bytes.size()),
                         &initCanonical));
    EXPECT_TRUE(initCanonical.hasVariablePolicyOverlay);
    ASSERT_TRUE(initCanonical.hasGameplayCatalog);
    ASSERT_EQ(1u, initCanonical.gameplayCatalog.active.size());
    EXPECT_EQ(actionItem->runtimeId(),
              initCanonical.gameplayCatalog.active.front().actionStableId);
    ASSERT_EQ(1u, initCanonical.variablePolicyOverlays.size());
    EXPECT_EQ(variable->runtimeId(),
              initCanonical.variablePolicyOverlays[0].variableId);
    EXPECT_EQ(1u, initCanonical.variablePolicyOverlays[0].writers);
    EXPECT_NE(0u, initCanonical.variablePolicyOverlays[0].executionFingerprint);
    EXPECT_TRUE(initCanonical.variablePolicyOverlays[0].discreteAnchors.empty());

    PackImage contentImage;
    ASSERT_NO_FATAL_FAILURE(readPack(
        variantPackPath(project, dir.path(), harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias)),
        contentImage));
    harken::PackCanonicalImage contentCanonical;
    ASSERT_EQ(VN_OK, harken::ParsePackCanonicalImage(
                         contentImage.data(),
                         static_cast<uint64_t>(contentImage.bytes.size()),
                         &contentCanonical));
    EXPECT_TRUE(contentCanonical.hasVariableRequirements);
    EXPECT_EQ(contentCanonical.packStableId,
              initCanonical.gameplayCatalog.active.front().ownerPackStableId);
    ASSERT_EQ(1u, contentCanonical.variableRequirements.size());
    EXPECT_EQ(1u, contentCanonical.variableRequirements[0].identityClass);
    EXPECT_EQ(variable->runtimeId(),
              contentCanonical.variableRequirements[0].stableId);
    EXPECT_EQ(initCanonical.variablePolicyOverlays[0].executionFingerprint,
              contentCanonical.variableRequirements[0].expectedExecutionFingerprint);

    QFile manifestFile(QDir(dir.path()).filePath(
        HarkenEditor::Bridge::packManifestFileName()));
    ASSERT_TRUE(manifestFile.open(QIODevice::ReadOnly));
    const QByteArray manifestBytes = manifestFile.readAll();
    const QJsonObject manifest =
        QJsonDocument::fromJson(manifestBytes).object();
    const QJsonObject catalog =
        manifest.value(QStringLiteral("variableCatalog")).toObject();
    const QJsonArray catalogVariables =
        catalog.value(QStringLiteral("variables")).toArray();
    ASSERT_EQ(1, catalogVariables.size());
    const QJsonObject catalogRow = catalogVariables.at(0).toObject();
    EXPECT_EQ(variable->runtimeId(),
              VNX::runtimeObjectId(QUuid(catalogRow.value(QStringLiteral("variableId")).toString())));
    EXPECT_EQ(1, catalogRow.value(QStringLiteral("writers")).toInt());
    const QString overlayFingerprint = QString::number(
        initCanonical.variablePolicyOverlays[0].executionFingerprint);
    EXPECT_TRUE(QString::fromUtf8(manifestBytes).contains(
        QStringLiteral("\"executionFingerprint\":%1").arg(overlayFingerprint)));

    harkentest::EngineSession session(harkentest::MakeNullConfig(8, 64, 48000));
    ASSERT_EQ(VN_OK, session.InitResult());
    CanonicalPackActivation loaded;
    ASSERT_EQ(VN_OK, activateCanonicalPackFiles(
        {QDir(dir.path()).filePath(QStringLiteral("Init.bytes")),
         variantPackPath(project, dir.path(), harkentest::StudioObjectId(kActionOneObjectId + kStableIdBias))},
        &loaded));
    ASSERT_EQ(2u, loaded.rows.size());
    const VnPackHandle initHandle = loaded.report.initHandle;
    const VnPackHandle contentHandle = loaded.rows[1].handle;
    ASSERT_NE(0u, initHandle);
    ASSERT_NE(0u, contentHandle);
    const auto* published = session.Engine()->packComposition.Published();
    ASSERT_NE(published, nullptr);
    EXPECT_TRUE(published->hasGameplayCatalog);
    ASSERT_EQ(2u, published->owners.size());
    const auto keep = harken::PackComposition::Keep(published->owners.front());
    EXPECT_TRUE(keep.hasGameplayCatalog);
    EXPECT_TRUE(keep.hasVariablePolicyOverlay);
    EXPECT_EQ(1u, keep.variablePolicyOverlays.size());

    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterSharedPack, DiscreteNonIntegerDefaultIsExportRejected)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& assets = VNX::AssetRegistry::instance();
    assets.clear();
    assets.setProjectRoot(dir.path());
    const auto source =
        registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 1.0f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), source, source,
        {{QStringLiteral("PlayDrive"), kActionOneObjectId, kSharedSoundObjectId}}));
    auto variable = attachDriveVariable(project);
    auto* variableComponent = variable->comp<VNX::VVariable>();
    variableComponent->kind = VNX::VariableKind::Step;
    variableComponent->rangeMin = 0.0f;
    variableComponent->rangeMax = 3.0f;
    variableComponent->defaultValue = 1.5f;

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    const auto result = exporter.ExportAllPacks(project, dir.path(), options);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.isEmpty());
    EXPECT_FLOAT_EQ(variableComponent->defaultValue, 1.5f);
    EXPECT_EQ(variableComponent->kind, VNX::VariableKind::Step);
    VNX::AssetRegistry::instance().clear();
}

TEST(EditorProjectPackExporterSharedPack, SmoothDefaultBelowRangeIsExportRejected)
{
    qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
    QTemporaryDir dir;
    ASSERT_TRUE(dir.isValid());
    auto& assets = VNX::AssetRegistry::instance();
    assets.clear();
    assets.setProjectRoot(dir.path());
    const auto source =
        registerToneAsset(dir.path(), QStringLiteral("tone.wav"), 1.0f);
    auto project = HarkenTest::projectWithPlatforms({"Windows"});
    ASSERT_NO_FATAL_FAILURE(buildProject(
        project, dir.path(), source, source,
        {{QStringLiteral("PlayDrive"), kActionOneObjectId, kSharedSoundObjectId}}));
    auto variable = attachDriveVariable(project);
    auto* variableComponent = variable->comp<VNX::VVariable>();
    variableComponent->kind = VNX::VariableKind::Smooth;
    variableComponent->rangeMin = 0.0f;
    variableComponent->rangeMax = 1.0f;
    variableComponent->defaultValue = -0.25f;

    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions options;
    options.platform = HarkenTest::platformIdString("Windows");
    const auto result = exporter.ExportAllPacks(project, dir.path(), options);
    EXPECT_FALSE(result.success);
    EXPECT_FALSE(result.error.isEmpty());
    EXPECT_FLOAT_EQ(variableComponent->defaultValue, -0.25f);
    EXPECT_FLOAT_EQ(variableComponent->rangeMin, 0.0f);
    EXPECT_FLOAT_EQ(variableComponent->rangeMax, 1.0f);
    VNX::AssetRegistry::instance().clear();
}
