#include "UnitTest/StudioObjectIdTestSupport.h"

#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/PathUtils.h"
#include "Components/VAction.h"
#include "Components/VAttenuation.h"
#include "Components/VAudio.h"
#include "Components/VMixNode.h"
#include "Components/VSegment.h"
#include "Components/VSelection.h"
#include "Components/VSound.h"
#include "Components/VTrack.h"
#include "Components/VVariable.h"
#include "BindingSemantics.h"
#include "Tooling/Harkenizer/Core/CurrentWorldProjectAdapter.h"
#include "PlayablePropertyAuthoringV1.h"
#include "AudioEngine.h"
#include "ActionManager.h"
#include "PlatformContextService.h"
#include "ProjectManager.h"
#include "FolderSaveState.h"
#include "Tooling/Harkenizer/Core/ProjectPackExporter.h"
#include "Tooling/Harkenizer/Core/CanonicalPackSetExport.h"
#include "RuntimeSyncCoordinator.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"
#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "Timeline/TimelineTypes.h"

#include "harken/Harken.h"
#include "harken_studio_build/PackFormatV2.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <vector>

#include <QByteArray>
#include <QApplication>
#include <QCoreApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QDirIterator>
#include <QEventLoop>
#include <QElapsedTimer>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QProcess>
#include <QRegularExpression>
#include <QSaveFile>
#include <QScopeGuard>
#include <QTemporaryDir>
#include <QThread>
#include <QUuid>

#include <cmath>
#include <filesystem>
#include <limits>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <memory>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include <process.h>
#include "Support/ProjectPlatformTestUtils.h"

#ifndef HARKEN_STUDIO_CLI_PATH
#error "HARKEN_STUDIO_CLI_PATH must point at the Studio CLI executable"
#endif

#ifndef HARKEN_TEMP_DIR
#error "HARKEN_TEMP_DIR must point at the tree .temp directory"
#endif

namespace {

constexpr uint64_t kStableIdBias = 100000;
constexpr auto kExportedPackProjectId = "3a89dd91-7b44-4f4c-9c58-96994c0e6e25";

constexpr uint32_t kMasterBusObjectId = 201;
constexpr uint32_t kRootLayerObjectId = 10;
constexpr uint32_t kSharedSoundObjectId = 1;
constexpr uint32_t kContentSoundObjectId = 2;
constexpr uint32_t kRandomChildSoundObjectId = 3;
constexpr uint32_t kRandomContainerObjectId = 4;
constexpr uint32_t kSequenceChildSoundObjectId = 5;
constexpr uint32_t kSequenceChildBSoundObjectId = 11;
constexpr uint32_t kVerticalContainerObjectId = 6;
constexpr uint32_t kPackBSoundObjectId = 7;
constexpr uint32_t kStepVariableObjectId = 8;
constexpr uint32_t kSmoothVariableObjectId = 9;
constexpr uint32_t kImmediateSoundActionObjectId = 21;
constexpr uint32_t kVerticalContainerActionObjectId = 22;
constexpr uint32_t kPackBActionObjectId = 23;
constexpr uint32_t kDelayedSoundActionObjectId = 24;
constexpr uint32_t kRandomContainerActionObjectId = 25;
constexpr uint32_t kQueueSoundChildObjectId = 12;
constexpr uint32_t kQueueSoundContainerObjectId = 13;
constexpr uint32_t kQueueSoundActionObjectId = 26;
constexpr uint32_t kDirectTrackSoundObjectId = 14;
constexpr uint32_t kDirectTrackObjectId = 15;
constexpr uint32_t kDirectTrackActionObjectId = 27;
constexpr uint32_t kQueueTrackSoundObjectId = 16;
constexpr uint32_t kQueueTrackSoundBObjectId = 57;
constexpr uint32_t kQueueTrackObjectId = 17;
constexpr uint32_t kQueueTrackContainerObjectId = 18;
constexpr uint32_t kQueueTrackActionObjectId = 28;
constexpr uint32_t kDirectSegmentSoundObjectId = 19;
constexpr uint32_t kDirectSegmentTrackObjectId = 31;
constexpr uint32_t kDirectSegmentObjectId = 32;
constexpr uint32_t kDirectSegmentActionObjectId = 29;
constexpr uint32_t kQueueSegmentSoundObjectId = 33;
constexpr uint32_t kQueueSegmentSoundBObjectId = 58;
constexpr uint32_t kQueueSegmentTrackObjectId = 34;
constexpr uint32_t kQueueSegmentObjectId = 35;
constexpr uint32_t kQueueSegmentContainerObjectId = 36;
constexpr uint32_t kQueueSegmentActionObjectId = 37;
constexpr uint32_t kSharedLeafContainerObjectId = 38;
constexpr uint32_t kSharedLeafActionObjectId = 39;
constexpr uint32_t kTwoPlayActionObjectId = 40;
constexpr uint32_t kDiscreteSilentSoundObjectId = 41;
constexpr uint32_t kDiscreteAudibleSoundObjectId = 42;
constexpr uint32_t kDiscreteContainerObjectId = 43;
constexpr uint32_t kDiscreteActionObjectId = 44;
constexpr uint32_t kContinuousSilentSoundObjectId = 45;
constexpr uint32_t kContinuousAudibleSoundObjectId = 46;
constexpr uint32_t kContinuousContainerObjectId = 47;
constexpr uint32_t kContinuousActionObjectId = 48;
constexpr uint32_t kInitiallySilentSoundObjectId = 49;
constexpr uint32_t kInitiallyAudibleSoundObjectId = 50;
constexpr uint32_t kInitiallySilentContainerObjectId = 51;
constexpr uint32_t kInitiallySilentActionObjectId = 52;
constexpr uint32_t kNestedRandomChildObjectId = 53;
constexpr uint32_t kNestedRandomInnerObjectId = 54;
constexpr uint32_t kNestedRandomOuterObjectId = 55;
constexpr uint32_t kNestedRandomActionObjectId = 56;
constexpr uint32_t kDelayedSoundObjectId = 59;
constexpr uint32_t kTwoPlaySoundObjectId = 60;
constexpr uint32_t kQuantizedBeatActionObjectId = 61;
constexpr uint32_t kQuantizedBarActionObjectId = 62;
constexpr uint32_t kQuantizedMarkerActionObjectId = 63;
constexpr uint32_t kQuantizedStopActionObjectId = 64;
constexpr uint32_t kSpatialAttenuationObjectId = 68;
constexpr uint32_t kSpatialNearFarSoundObjectId = 69;
constexpr uint32_t kSpatialNearFarActionObjectId = 67;

constexpr uint32_t kSharedSourceId = 1001;
constexpr uint32_t kContentSourceId = 1002;
constexpr uint32_t kRandomChildSourceId = 1003;
constexpr uint32_t kSequenceChildSourceId = 1004;
constexpr uint32_t kPackBSourceId = 1005;
constexpr uint32_t kQueueSoundSourceId = 1006;
constexpr uint32_t kDirectTrackSourceId = 1007;
constexpr uint32_t kQueueTrackSourceId = 1008;
constexpr uint32_t kQueueTrackSourceBId = 1018;
constexpr uint32_t kDirectSegmentSourceId = 1009;
constexpr uint32_t kQueueSegmentSourceId = 1010;
constexpr uint32_t kQueueSegmentSourceBId = 1019;
constexpr uint32_t kDiscreteSilentSourceId = 1011;
constexpr uint32_t kDiscreteAudibleSourceId = 1012;
constexpr uint32_t kContinuousSilentSourceId = 1013;
constexpr uint32_t kContinuousAudibleSourceId = 1014;
constexpr uint32_t kInitiallySilentSourceId = 1015;
constexpr uint32_t kInitiallyAudibleSourceId = 1016;
constexpr uint32_t kNestedRandomSourceId = 1017;
constexpr uint32_t kDelayedSourceId = 1020;
constexpr uint32_t kTwoPlaySourceId = 1021;
constexpr uint32_t kSpatialNearFarSourceId = 1022;
constexpr float kAtt04MaxDistance = 40.0f;
constexpr float kAtt04NearDistance = 1.0f;
constexpr float kAtt04FarDistance = 20.0f;
constexpr float kAtt04FarVolumeDb = -18.0f;

constexpr uint32_t kToneFrames = 2048;
constexpr float kToneAmplitude = 0.25f;
constexpr uint32_t kSampleRate = 48000;
constexpr uint16_t kChannels = 1;
constexpr float kDelayedPlayMilliseconds = 20.0f;
constexpr float kDeferredRefusePlayMilliseconds = 1000.0f;
constexpr float kSuccessorToneAmplitude = 0.75f;

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

uint32_t registerToneAsset(const QString& projectRoot,
                           const QString& relativePath,
                           uint32_t sourceId,
                           float amplitude = kToneAmplitude)
{
    std::vector<float> samples(kToneFrames, amplitude);
    const QString audioRoot = VNX::PathUtils::getAudioSourcesDir(projectRoot);
    const QString sourcePath = QDir(audioRoot).filePath(relativePath);
    writeWavF32(sourcePath, samples, kChannels, kSampleRate);

    VNX::AssetInfo info;
    info.relativePath = relativePath;
    info.absolutePath = sourcePath;
    info.sourceID = sourceId;
    info.metadata.sampleRate = kSampleRate;
    info.metadata.channels = kChannels;
    info.metadata.bitDepth = 32;
    info.metadata.totalSamples = kToneFrames;
    info.metadata.fileSize = static_cast<uint64_t>(QFileInfo(sourcePath).size());
    info.lastModified = 1;
    EXPECT_TRUE(VNX::AssetRegistry::instance().registerAsset(info));
    return info.sourceID;
}

std::shared_ptr<VNX::ProjectItem> makeItem(const QString& name,
                                           VNX::ProjectItem::Type type,
                                           uint32_t objectId)
{
    auto item = std::make_shared<VNX::ProjectItem>();
    item->name = name;
    item->type = type;
    item->objectID = objectId;
    item->guid = VNX::authoringUuid(harkentest::StudioObjectId(objectId + kStableIdBias));
    item->attachDefaultComponents();
    return item;
}

void addChild(const std::shared_ptr<VNX::ProjectItem>& parent,
              const std::shared_ptr<VNX::ProjectItem>& child)
{
    child->parent = parent.get();
    parent->children.push_back(child);
}

void addBinding(std::vector<VNX::ProjectFileBinding>& bindings,
                const std::shared_ptr<VNX::ProjectItem>& root)
{
    VNX::ProjectFileBinding binding;
    binding.rootItem = root;
    bindings.push_back(std::move(binding));
}

void addAudioSource(VNX::ProjectData& project, uint32_t sourceId)
{
    const VNX::AssetInfo* info = VNX::AssetRegistry::instance().getAsset(sourceId);
    ASSERT_NE(info, nullptr);
    VNX::AudioSourceData source;
    source.sourceID = sourceId;
    source.relativePath = info->relativePath;
    source.language = QStringLiteral("SFX");
    project.audioSources.push_back(source);
}

void authorDefaultPlayableProperties(VNX::ProjectItem& item)
{
    const auto locator = VNX::PlayablePropertyAuthoringV1::definitionLocator(item);
    ASSERT_TRUE(locator.has_value());
    using Id = harkenplayableproperties::PlayablePropertyIdV1;
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        item, *locator, static_cast<uint8_t>(Id::Volume), QJsonValue(0.0), std::nullopt));
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        item, *locator, static_cast<uint8_t>(Id::Pitch), QJsonValue(0.0), std::nullopt));
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        item, *locator, static_cast<uint8_t>(Id::LoopCount), QJsonValue(1), std::nullopt));
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        item, *locator, static_cast<uint8_t>(Id::StreamingMode), QJsonValue(0), std::nullopt));
    ASSERT_EQ(item.fieldGroups, VNX::DefinitionFieldGroups{});
}

void attachPlayableSound(const std::shared_ptr<VNX::ProjectItem>& parent,
                         const std::shared_ptr<VNX::ProjectItem>& sound,
                         uint32_t sourceId)
{
    sound->sourceID = sourceId;
    auto* audio = sound->comp<VNX::VAudio>();
    audio->loopCount = 1;
    audio->streamingMode = 0;
    auto* authored = sound->comp<VNX::VSound>();
    VNX::SoundSourceDef source;
    source.entryId = sound->guid;
    source.sourceId = sourceId;
    authored->sources.plain.entries = {source};
    authored->sources.plain.platforms[HarkenTest::platformIdString("Windows")].active = source.entryId;
    ASSERT_NO_FATAL_FAILURE(authorDefaultPlayableProperties(*sound));
    addChild(parent, sound);
}

void attachContainerChild(const std::shared_ptr<VNX::ProjectItem>& container,
                          const std::shared_ptr<VNX::ProjectItem>& child)
{
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::addContainerChildOccurrence(
        *container, child->guid));
}

void prepareContainer(const std::shared_ptr<VNX::ProjectItem>& container,
                      VNX::SelectionMode mode,
                      VNX::SelectionPlayType playType = VNX::SelectionPlayType::Single)
{
    ASSERT_NO_FATAL_FAILURE(authorDefaultPlayableProperties(*container));
    auto* selection = container->comp<VNX::VSelection>();
    selection->mode = mode;
    selection->playType = playType;
    selection->matchResponse = VNX::VariableResponse::AtStart;
    selection->blendResponse = VNX::VariableResponse::Live;
    const bool queueOrVariable =
        selection->isQueue()
        || mode == VNX::SelectionMode::Match
        || mode == VNX::SelectionMode::Blend;
    selection->behavior.memoryScope = queueOrVariable
        ? VNX::SelectionMemoryScope::PerEmitter
        : VNX::SelectionMemoryScope::Global;
}

void attachTrackClip(const std::shared_ptr<VNX::ProjectItem>& track,
                     const std::shared_ptr<VNX::ProjectItem>& sound,
                     int64_t start = 0,
                     uint8_t clipOrdinal = 2)
{
    auto* timeline = track->comp<VNX::VTrack>();
    ASSERT_NE(timeline, nullptr);
    VNX::Clip clip;
    clip.targetGuid = sound->guid;
    clip.start = start;
    clip.duration = 1000000;
    clip.clipGuid = VNX::authoringUuid(harkentest::OffsetObjectId(track->runtimeId(), 500000 + clipOrdinal));
    ASSERT_TRUE(timeline->clips.mintOccurrenceOrdinal(&clip));
    timeline->clips.items.push_back(clip);
}

void attachSecondTrackLeaf(const std::shared_ptr<VNX::ProjectItem>& track,
                           const QString& soundName,
                           uint32_t soundObjectId,
                           uint32_t sourceId)
{
    auto sound = makeItem(soundName, VNX::ProjectItem::Type::Sound, soundObjectId);
    ASSERT_NO_FATAL_FAILURE(attachPlayableSound(track, sound, sourceId));
    ASSERT_NO_FATAL_FAILURE(attachTrackClip(track, sound, INT64_C(12000000), 3));
}

void makeTrackWithSound(const std::shared_ptr<VNX::ProjectItem>& parent,
                        const QString& trackName,
                        uint32_t trackObjectId,
                        const QString& soundName,
                        uint32_t soundObjectId,
                        uint32_t sourceId,
                        std::shared_ptr<VNX::ProjectItem>* outTrack)
{
    auto track = makeItem(trackName, VNX::ProjectItem::Type::Track, trackObjectId);
    ASSERT_NO_FATAL_FAILURE(authorDefaultPlayableProperties(*track));
    addChild(parent, track);
    auto sound = makeItem(soundName, VNX::ProjectItem::Type::Sound, soundObjectId);
    ASSERT_NO_FATAL_FAILURE(attachPlayableSound(track, sound, sourceId));
    ASSERT_NO_FATAL_FAILURE(attachTrackClip(track, sound));
    *outTrack = std::move(track);
}

void makeSegmentWithSound(const std::shared_ptr<VNX::ProjectItem>& parent,
                        const QString& segmentName,
                        uint32_t segmentObjectId,
                        const QString& trackName,
                        uint32_t trackObjectId,
                        const QString& soundName,
                        uint32_t soundObjectId,
                        uint32_t sourceId,
                        std::shared_ptr<VNX::ProjectItem>* outSegment)
{
    auto segment = makeItem(segmentName, VNX::ProjectItem::Type::Segment, segmentObjectId);
    ASSERT_NO_FATAL_FAILURE(authorDefaultPlayableProperties(*segment));
    addChild(parent, segment);
    std::shared_ptr<VNX::ProjectItem> track;
    ASSERT_NO_FATAL_FAILURE(makeTrackWithSound(segment, trackName, trackObjectId, soundName,
                                               soundObjectId, sourceId, &track));
    *outSegment = std::move(segment);
}

VNX::VActionStep playStep(const VNX::ProjectItem& target, float delayMs = 0.0f)
{
    VNX::VActionStep step;
    step.type = VNX::VActionStep::Type::Play;
    step.targetObjectID = target.objectID;
    step.targetName = target.name;
    step.delay = delayMs;
    return step;
}

std::shared_ptr<VNX::ProjectItem> makePlayAction(
    const std::shared_ptr<VNX::ProjectItem>& actionsRoot,
    const QString& name,
    uint32_t objectId,
    const VNX::ProjectItem& target,
    float delayMs = 0.0f)
{
    auto action = makeItem(name, VNX::ProjectItem::Type::Action, objectId);
    action->comp<VNX::VAction>()->steps = {playStep(target, delayMs)};
    addChild(actionsRoot, action);
    return action;
}

void authorDiscreteVariableControl(const std::shared_ptr<VNX::ProjectItem>& container,
                                   const std::shared_ptr<VNX::ProjectItem>& variable,
                                   const std::shared_ptr<VNX::ProjectItem>& silentChild,
                                   const std::shared_ptr<VNX::ProjectItem>& audibleChild,
                                   int silentValue,
                                   int audibleValue)
{
    ASSERT_NO_FATAL_FAILURE(prepareContainer(container, VNX::SelectionMode::Match));
    auto* selection = container->comp<VNX::VSelection>();
    {
        auto grid = selection->matchGrid();
        ASSERT_TRUE(grid.addAxis(variable->guid));
        ASSERT_TRUE(grid.addRow(silentChild->guid));
        ASSERT_TRUE(grid.addRow(audibleChild->guid));
        ASSERT_TRUE(grid.setCell(0, 0, silentValue));
        ASSERT_TRUE(grid.setCell(1, 0, audibleValue));
        EXPECT_TRUE(selection->selections.shared.setMatchGrid(grid));
    }
}

void authorContinuousVariableControl(const std::shared_ptr<VNX::ProjectItem>& container,
                                     const std::shared_ptr<VNX::ProjectItem>& variable,
                                     const std::shared_ptr<VNX::ProjectItem>& silentChild,
                                     const std::shared_ptr<VNX::ProjectItem>& audibleChild)
{
    ASSERT_NO_FATAL_FAILURE(prepareContainer(container, VNX::SelectionMode::Blend));
    auto* selection = container->comp<VNX::VSelection>();
    {
        auto mixing = selection->mixing();
        mixing.rows.push_back({variable->guid, {}});
        mixing.rows.back().children.push_back(VNX::MixingChildDef::unity(silentChild->guid, 0.0f, 100.0f));
        mixing.rows.back().children.push_back(VNX::MixingChildDef::unity(audibleChild->guid, 0.0f, 100.0f));
        mixing.rows.front().children[0].points = {{0.0f, 1.0f}, {0.25f, 1.0f}, {0.75f, 0.0f}, {1.0f, 0.0f}};
        mixing.rows.front().children[1].points = {{0.0f, 0.0f}, {0.25f, 0.0f}, {0.75f, 1.0f}, {1.0f, 1.0f}};
        for (auto& child : mixing.rows.front().children) child.normalize();
        EXPECT_TRUE(selection->selections.shared.setMixing(mixing));
    }
}

void buildExportedPack(VNX::ProjectData& project, const QString& projectRoot)
{
    project.name = QStringLiteral("ExportedPack");
    project.projectId = QString::fromLatin1(kExportedPackProjectId);
    project.projectPath = QDir(projectRoot).filePath(QStringLiteral("ExportedPack.vproj"));
    project.platforms = HarkenTest::platforms({QStringLiteral("Windows")});
    project.projectLanguages = {};

    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kSharedSourceId));
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kContentSourceId));
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kRandomChildSourceId));
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kSequenceChildSourceId));
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kPackBSourceId));
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kQueueSoundSourceId));
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kDirectTrackSourceId));
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kQueueTrackSourceId));
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kQueueTrackSourceBId));
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kDirectSegmentSourceId));
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kQueueSegmentSourceId));
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kQueueSegmentSourceBId));
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kDiscreteSilentSourceId));
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kDiscreteAudibleSourceId));
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kContinuousSilentSourceId));
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kContinuousAudibleSourceId));
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kInitiallySilentSourceId));
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kInitiallyAudibleSourceId));
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kNestedRandomSourceId));
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kDelayedSourceId));
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kTwoPlaySourceId));
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kSpatialNearFarSourceId));

    auto master = makeItem(QStringLiteral("MasterBus"),
                           VNX::ProjectItem::Type::Bus,
                           kMasterBusObjectId);
    master->guid = VNX::authoringUuid(harkentest::StudioObjectId(0x4d4153544552));
    addBinding(project.busses, master);

    auto layer = makeItem(QStringLiteral("Root"),
                          VNX::ProjectItem::Type::Layer,
                          kRootLayerObjectId);
    layer->fieldGroups = VNX::MakeRootFieldGroups(VNX::toFieldGroupGuid(master->guid));

    auto sharedSound = makeItem(QStringLiteral("ExportedPackSharedSound"),
                                VNX::ProjectItem::Type::Sound,
                                kSharedSoundObjectId);
    auto contentSound = makeItem(QStringLiteral("ExportedPackImmediateSoundTarget"),
                                 VNX::ProjectItem::Type::Sound,
                                 kContentSoundObjectId);
    auto delayedSound = makeItem(QStringLiteral("ExportedPackDelayedSoundTarget"),
                                 VNX::ProjectItem::Type::Sound,
                                 kDelayedSoundObjectId);
    auto twoPlaySound = makeItem(QStringLiteral("ExportedPackTwoPlaySoundTarget"),
                                 VNX::ProjectItem::Type::Sound,
                                 kTwoPlaySoundObjectId);
    auto randomChild = makeItem(QStringLiteral("ExportedPackRandomChild"),
                                VNX::ProjectItem::Type::Sound,
                                kRandomChildSoundObjectId);
    auto sequenceChild = makeItem(QStringLiteral("ExportedPackSequenceChild"),
                                  VNX::ProjectItem::Type::Sound,
                                  kSequenceChildSoundObjectId);
    auto sequenceChildB = makeItem(QStringLiteral("ExportedPackSequenceChildB"),
                                   VNX::ProjectItem::Type::Sound,
                                   kSequenceChildBSoundObjectId);
    auto packBSound = makeItem(QStringLiteral("ExportedPackPackBSound"),
                               VNX::ProjectItem::Type::Sound,
                               kPackBSoundObjectId);
    ASSERT_NO_FATAL_FAILURE(attachPlayableSound(layer, contentSound, kContentSourceId));
    ASSERT_NO_FATAL_FAILURE(attachPlayableSound(layer, delayedSound, kDelayedSourceId));
    ASSERT_NO_FATAL_FAILURE(attachPlayableSound(layer, twoPlaySound, kTwoPlaySourceId));
    ASSERT_NO_FATAL_FAILURE(attachPlayableSound(layer, packBSound, kPackBSourceId));

    auto randomContainer = makeItem(QStringLiteral("ExportedPackRandomContainer"),
                                    VNX::ProjectItem::Type::Container,
                                    kRandomContainerObjectId);
    ASSERT_NO_FATAL_FAILURE(prepareContainer(randomContainer, VNX::SelectionMode::Random));
    addChild(layer, randomContainer);
    ASSERT_NO_FATAL_FAILURE(attachPlayableSound(randomContainer, randomChild,
                                                kRandomChildSourceId));
    ASSERT_NO_FATAL_FAILURE(attachContainerChild(randomContainer, randomChild));

    auto nestedInnerChild = makeItem(QStringLiteral("ExportedPackNestedRandomChild"),
                                     VNX::ProjectItem::Type::Sound,
                                     kNestedRandomChildObjectId);
    auto nestedInner = makeItem(QStringLiteral("ExportedPackNestedRandomInner"),
                                VNX::ProjectItem::Type::Container,
                                kNestedRandomInnerObjectId);
    auto nestedOuter = makeItem(QStringLiteral("ExportedPackNestedRandomOuter"),
                                VNX::ProjectItem::Type::Container,
                                kNestedRandomOuterObjectId);
    ASSERT_NO_FATAL_FAILURE(prepareContainer(nestedInner, VNX::SelectionMode::Random));
    ASSERT_NO_FATAL_FAILURE(prepareContainer(nestedOuter, VNX::SelectionMode::Random));
    addChild(layer, nestedOuter);
    addChild(nestedOuter, nestedInner);
    ASSERT_NO_FATAL_FAILURE(attachPlayableSound(nestedInner, nestedInnerChild,
                                                kNestedRandomSourceId));
    ASSERT_NO_FATAL_FAILURE(attachContainerChild(nestedInner, nestedInnerChild));
    ASSERT_NO_FATAL_FAILURE(attachContainerChild(nestedOuter, nestedInner));

    auto verticalContainer = makeItem(QStringLiteral("ExportedPackVerticalContainer"),
                                      VNX::ProjectItem::Type::Container,
                                      kVerticalContainerObjectId);
    ASSERT_NO_FATAL_FAILURE(prepareContainer(verticalContainer, VNX::SelectionMode::Sequence));
    addChild(layer, verticalContainer);
    ASSERT_NO_FATAL_FAILURE(attachPlayableSound(verticalContainer, sequenceChild,
                                                kSequenceChildSourceId));
    ASSERT_NO_FATAL_FAILURE(attachPlayableSound(verticalContainer, sequenceChildB,
                                                kSequenceChildSourceId));
    ASSERT_NO_FATAL_FAILURE(attachContainerChild(verticalContainer, sequenceChild));
    ASSERT_NO_FATAL_FAILURE(attachContainerChild(verticalContainer, sequenceChildB));

    auto queueSoundChild = makeItem(QStringLiteral("ExportedPackQueueSoundChild"),
                                    VNX::ProjectItem::Type::Sound,
                                    kQueueSoundChildObjectId);
    auto queueSoundContainer = makeItem(QStringLiteral("ExportedPackQueueSoundContainer"),
                                        VNX::ProjectItem::Type::Container,
                                        kQueueSoundContainerObjectId);
    ASSERT_NO_FATAL_FAILURE(prepareContainer(queueSoundContainer,
                                             VNX::SelectionMode::Sequence, VNX::SelectionPlayType::Queue));
    queueSoundContainer->comp<VNX::VSelection>()->transitions.rules.front().exitTiming =
        VNX::TransitionExitTiming::Immediate;
    addChild(layer, queueSoundContainer);
    ASSERT_NO_FATAL_FAILURE(attachPlayableSound(queueSoundContainer, queueSoundChild,
                                                kQueueSoundSourceId));
    ASSERT_NO_FATAL_FAILURE(attachContainerChild(queueSoundContainer, queueSoundChild));

    std::shared_ptr<VNX::ProjectItem> directTrack;
    ASSERT_NO_FATAL_FAILURE(makeTrackWithSound(
        layer, QStringLiteral("ExportedPackDirectTrack"), kDirectTrackObjectId,
        QStringLiteral("ExportedPackDirectTrackSound"), kDirectTrackSoundObjectId,
        kDirectTrackSourceId, &directTrack));

    auto queueTrackContainer = makeItem(QStringLiteral("ExportedPackQueueTrackContainer"),
                                        VNX::ProjectItem::Type::Container,
                                        kQueueTrackContainerObjectId);
    ASSERT_NO_FATAL_FAILURE(prepareContainer(queueTrackContainer,
                                             VNX::SelectionMode::Sequence, VNX::SelectionPlayType::Queue));
    queueTrackContainer->comp<VNX::VSelection>()->transitions.rules.front().exitTiming =
        VNX::TransitionExitTiming::Immediate;
    addChild(layer, queueTrackContainer);
    std::shared_ptr<VNX::ProjectItem> queueTrack;
    ASSERT_NO_FATAL_FAILURE(makeTrackWithSound(
        queueTrackContainer, QStringLiteral("ExportedPackQueueTrack"), kQueueTrackObjectId,
        QStringLiteral("ExportedPackQueueTrackSound"), kQueueTrackSoundObjectId,
        kQueueTrackSourceId, &queueTrack));
    ASSERT_NO_FATAL_FAILURE(attachSecondTrackLeaf(
        queueTrack, QStringLiteral("ExportedPackQueueTrackSoundB"),
        kQueueTrackSoundBObjectId, kQueueTrackSourceBId));
    ASSERT_NO_FATAL_FAILURE(attachContainerChild(queueTrackContainer, queueTrack));

    std::shared_ptr<VNX::ProjectItem> directSegment;
    ASSERT_NO_FATAL_FAILURE(makeSegmentWithSound(
        layer, QStringLiteral("ExportedPackDirectSegment"), kDirectSegmentObjectId,
        QStringLiteral("ExportedPackDirectSegmentTrack"), kDirectSegmentTrackObjectId,
        QStringLiteral("ExportedPackDirectSegmentSound"), kDirectSegmentSoundObjectId,
        kDirectSegmentSourceId, &directSegment));

    auto queueSegmentContainer = makeItem(QStringLiteral("ExportedPackQueueSegmentContainer"),
                                        VNX::ProjectItem::Type::Container,
                                        kQueueSegmentContainerObjectId);
    ASSERT_NO_FATAL_FAILURE(prepareContainer(queueSegmentContainer,
                                             VNX::SelectionMode::Sequence, VNX::SelectionPlayType::Queue));
    queueSegmentContainer->comp<VNX::VSelection>()->transitions.rules.front().exitTiming =
        VNX::TransitionExitTiming::Immediate;
    addChild(layer, queueSegmentContainer);
    std::shared_ptr<VNX::ProjectItem> queueSegment;
    ASSERT_NO_FATAL_FAILURE(makeSegmentWithSound(
        queueSegmentContainer, QStringLiteral("ExportedPackQueueSegment"), kQueueSegmentObjectId,
        QStringLiteral("ExportedPackQueueSegmentTrack"), kQueueSegmentTrackObjectId,
        QStringLiteral("ExportedPackQueueSegmentSound"), kQueueSegmentSoundObjectId,
        kQueueSegmentSourceId, &queueSegment));
    {
        std::shared_ptr<VNX::ProjectItem> queueSegmentTrack;
        for (const auto& child : queueSegment->children) {
            if (child && child->type == VNX::ProjectItem::Type::Track) {
                queueSegmentTrack = child;
                break;
            }
        }
        ASSERT_NE(queueSegmentTrack, nullptr);
        ASSERT_NO_FATAL_FAILURE(attachSecondTrackLeaf(
            queueSegmentTrack, QStringLiteral("ExportedPackQueueSegmentSoundB"),
            kQueueSegmentSoundBObjectId, kQueueSegmentSourceBId));
    }
    ASSERT_NO_FATAL_FAILURE(attachContainerChild(queueSegmentContainer, queueSegment));

    auto sharedLeafContainer = makeItem(QStringLiteral("ExportedPackSharedLeafContainer"),
                                        VNX::ProjectItem::Type::Container,
                                        kSharedLeafContainerObjectId);
    ASSERT_NO_FATAL_FAILURE(prepareContainer(sharedLeafContainer, VNX::SelectionMode::Sequence));
    addChild(layer, sharedLeafContainer);
    ASSERT_NO_FATAL_FAILURE(attachPlayableSound(sharedLeafContainer, sharedSound,
                                                kSharedSourceId));
    ASSERT_NO_FATAL_FAILURE(attachContainerChild(sharedLeafContainer, sharedSound));

    auto discreteSilent = makeItem(QStringLiteral("ExportedPackDiscreteSilent"),
                                   VNX::ProjectItem::Type::Sound,
                                   kDiscreteSilentSoundObjectId);
    auto discreteAudible = makeItem(QStringLiteral("ExportedPackDiscreteAudible"),
                                    VNX::ProjectItem::Type::Sound,
                                    kDiscreteAudibleSoundObjectId);
    auto discreteContainer = makeItem(QStringLiteral("ExportedPackDiscreteContainer"),
                                      VNX::ProjectItem::Type::Container,
                                      kDiscreteContainerObjectId);
    addChild(layer, discreteContainer);
    ASSERT_NO_FATAL_FAILURE(attachPlayableSound(discreteContainer, discreteSilent,
                                                kDiscreteSilentSourceId));
    ASSERT_NO_FATAL_FAILURE(attachPlayableSound(discreteContainer, discreteAudible,
                                                kDiscreteAudibleSourceId));
    ASSERT_NO_FATAL_FAILURE(attachContainerChild(discreteContainer, discreteSilent));
    ASSERT_NO_FATAL_FAILURE(attachContainerChild(discreteContainer, discreteAudible));

    auto continuousSilent = makeItem(QStringLiteral("ExportedPackContinuousSilent"),
                                     VNX::ProjectItem::Type::Sound,
                                     kContinuousSilentSoundObjectId);
    auto continuousAudible = makeItem(QStringLiteral("ExportedPackContinuousAudible"),
                                      VNX::ProjectItem::Type::Sound,
                                      kContinuousAudibleSoundObjectId);
    auto continuousContainer = makeItem(QStringLiteral("ExportedPackContinuousContainer"),
                                        VNX::ProjectItem::Type::Container,
                                        kContinuousContainerObjectId);
    addChild(layer, continuousContainer);
    ASSERT_NO_FATAL_FAILURE(attachPlayableSound(continuousContainer, continuousSilent,
                                                kContinuousSilentSourceId));
    ASSERT_NO_FATAL_FAILURE(attachPlayableSound(continuousContainer, continuousAudible,
                                                kContinuousAudibleSourceId));
    ASSERT_NO_FATAL_FAILURE(attachContainerChild(continuousContainer, continuousSilent));
    ASSERT_NO_FATAL_FAILURE(attachContainerChild(continuousContainer, continuousAudible));

    auto initiallySilent = makeItem(QStringLiteral("ExportedPackInitiallySilent"),
                                    VNX::ProjectItem::Type::Sound,
                                    kInitiallySilentSoundObjectId);
    auto initiallyAudible = makeItem(QStringLiteral("ExportedPackInitiallyAudible"),
                                     VNX::ProjectItem::Type::Sound,
                                     kInitiallyAudibleSoundObjectId);
    auto initiallySilentContainer = makeItem(
        QStringLiteral("ExportedPackInitiallySilentContainer"),
        VNX::ProjectItem::Type::Container,
        kInitiallySilentContainerObjectId);
    addChild(layer, initiallySilentContainer);
    ASSERT_NO_FATAL_FAILURE(attachPlayableSound(initiallySilentContainer, initiallySilent,
                                                kInitiallySilentSourceId));
    initiallySilent->comp<VNX::VAudio>()->loopCount = 16;
    ASSERT_NO_FATAL_FAILURE(attachPlayableSound(initiallySilentContainer, initiallyAudible,
                                                kInitiallyAudibleSourceId));
    ASSERT_NO_FATAL_FAILURE(attachContainerChild(initiallySilentContainer, initiallySilent));
    ASSERT_NO_FATAL_FAILURE(attachContainerChild(initiallySilentContainer, initiallyAudible));
    addBinding(project.layers, layer);

    auto attenuation = makeItem(QStringLiteral("ExportedPackSpatialAttenuation"),
                                VNX::ProjectItem::Type::Attenuation,
                                kSpatialAttenuationObjectId);
    auto* attenuationComp = attenuation->comp<VNX::VAttenuation>();
    attenuationComp->maxDistance = kAtt04MaxDistance;
    VNX::AttenuationDimensionDef volume;
    volume.type = VNX::AttenuationDimension::Volume;
    // Default linear dB 0→-96 over maxDistance=40 only loses ~2.4 dB from
    // 1 m to 20 m (~1.95×), which fails ATT04's peak_near > peak_far * 2.
    // Keep both distances inside [min,max]; drop 18 dB by the far probe.
    volume.curve = {
        {0.0f, 0.0f},
        {kAtt04FarDistance, kAtt04FarVolumeDb},
        {kAtt04MaxDistance, -96.0f},
    };
    VNX::attenuationNormalizeCurve(volume.type, volume.curve, volume.handles);
    attenuationComp->dimensions = {volume};
    addBinding(project.attenuationItems, attenuation);

    auto spatialSound = makeItem(QStringLiteral("ExportedPackSpatialNearFarSound"),
                                 VNX::ProjectItem::Type::Sound,
                                 kSpatialNearFarSoundObjectId);
    ASSERT_NO_FATAL_FAILURE(attachPlayableSound(layer, spatialSound,
                                                kSpatialNearFarSourceId));
    VNX::SpatialRatingFieldGroup spatialRating;
    spatialRating.spatialIs3DEnabled = true;
    spatialRating.spatialAttenuationRef = VNX::toFieldGroupGuid(attenuation->guid);
    spatialSound->fieldGroups.spatialRating.shared = {
        VNX::FieldGroupMode::Local, spatialRating};

    auto stepVariable = makeItem(QStringLiteral("ExportedPackStepGlobal"),
                                 VNX::ProjectItem::Type::Variable,
                                 kStepVariableObjectId);
    auto* stepComp = stepVariable->comp<VNX::VVariable>();
    stepComp->kind = VNX::VariableKind::Step;
    stepComp->rangeMin = 0.0f;
    stepComp->rangeMax = 3.0f;
    stepComp->defaultValue = 1.0f;
    stepComp->normalizeDeclaration();

    auto smoothVariable = makeItem(QStringLiteral("ExportedPackSmoothGlobal"),
                                   VNX::ProjectItem::Type::Variable,
                                   kSmoothVariableObjectId);
    auto* smoothComp = smoothVariable->comp<VNX::VVariable>();
    smoothComp->kind = VNX::VariableKind::Smooth;
    smoothComp->rangeMin = 0.0f;
    smoothComp->rangeMax = 1.0f;
    smoothComp->defaultValue = 0.9f;
    smoothComp->normalizeDeclaration();

    ASSERT_NO_FATAL_FAILURE(authorDiscreteVariableControl(
        discreteContainer, stepVariable, discreteSilent, discreteAudible, 0, 1));
    ASSERT_NO_FATAL_FAILURE(authorContinuousVariableControl(
        continuousContainer, smoothVariable, continuousSilent, continuousAudible));
    ASSERT_NO_FATAL_FAILURE(authorDiscreteVariableControl(
        initiallySilentContainer, stepVariable, initiallySilent, initiallyAudible, 1, 2));
    initiallySilentContainer->comp<VNX::VSelection>()->matchResponse =
        VNX::VariableResponse::Live;
    auto& liveMatchTransition = initiallySilentContainer->comp<VNX::VSelection>()->transitions.rules.front();
    liveMatchTransition.exitTiming = VNX::TransitionExitTiming::Immediate;
    liveMatchTransition.sourceFadeOut.durationSeconds = 0.0f;
    liveMatchTransition.targetFadeIn.durationSeconds = 0.0f;

    auto variablesRoot = makeItem(QStringLiteral("Variables"),
                                  VNX::ProjectItem::Type::None,
                                  30);
    addChild(variablesRoot, stepVariable);
    addChild(variablesRoot, smoothVariable);
    addBinding(project.variables, variablesRoot);

    auto actionsRoot = makeItem(QStringLiteral("Actions"),
                                VNX::ProjectItem::Type::None,
                                20);
    auto immediateAction = makePlayAction(
        actionsRoot, QStringLiteral("ExportedPackImmediateSound"),
        kImmediateSoundActionObjectId, *contentSound);
    auto delayedAction = makePlayAction(
        actionsRoot, QStringLiteral("ExportedPackDelayedSound"),
        kDelayedSoundActionObjectId, *delayedSound, kDelayedPlayMilliseconds);
    auto verticalAction = makePlayAction(
        actionsRoot, QStringLiteral("ExportedPackVerticalContainer"),
        kVerticalContainerActionObjectId, *verticalContainer);
    auto randomAction = makePlayAction(
        actionsRoot, QStringLiteral("ExportedPackRandomContainer"),
        kRandomContainerActionObjectId, *randomContainer);
    auto nestedRandomAction = makePlayAction(
        actionsRoot, QStringLiteral("ExportedPackRandomNestedContainer"),
        kNestedRandomActionObjectId, *nestedOuter);
    auto queueSoundAction = makePlayAction(
        actionsRoot, QStringLiteral("ExportedPackQueueSoundChild"),
        kQueueSoundActionObjectId, *queueSoundContainer);
    auto queueTrackAction = makePlayAction(
        actionsRoot, QStringLiteral("ExportedPackQueueTrackChild"),
        kQueueTrackActionObjectId, *queueTrackContainer);
    auto queueSegmentAction = makePlayAction(
        actionsRoot, QStringLiteral("ExportedPackQueueSegmentChild"),
        kQueueSegmentActionObjectId, *queueSegmentContainer);
    auto discreteAction = makePlayAction(
        actionsRoot, QStringLiteral("ExportedPackVariableControlDiscrete"),
        kDiscreteActionObjectId, *discreteContainer);
    auto continuousAction = makePlayAction(
        actionsRoot, QStringLiteral("ExportedPackVariableControlContinuous"),
        kContinuousActionObjectId, *continuousContainer);
    auto initiallySilentAction = makePlayAction(
        actionsRoot, QStringLiteral("ExportedPackVariableControlInitiallySilent"),
        kInitiallySilentActionObjectId, *initiallySilentContainer);
    auto directTrackAction = makePlayAction(
        actionsRoot, QStringLiteral("ExportedPackDirectTrack"),
        kDirectTrackActionObjectId, *directTrack);
    auto directSegmentAction = makePlayAction(
        actionsRoot, QStringLiteral("ExportedPackDirectSegment"),
        kDirectSegmentActionObjectId, *directSegment);
    auto sharedLeafAction = makePlayAction(
        actionsRoot, QStringLiteral("ExportedPackSharedLeafFromContentRoot"),
        kSharedLeafActionObjectId, *sharedLeafContainer);
    auto twoPlayAction = makeItem(QStringLiteral("ExportedPackTwoPlayStepsDifferentPacks"),
                                  VNX::ProjectItem::Type::Action,
                                  kTwoPlayActionObjectId);
    twoPlayAction->comp<VNX::VAction>()->steps = {
        playStep(*twoPlaySound),
        playStep(*sharedSound),
    };
    addChild(actionsRoot, twoPlayAction);
    auto packBAction = makePlayAction(
        actionsRoot, QStringLiteral("ExportedPackPackBAction"),
        kPackBActionObjectId, *packBSound);
    auto spatialAction = makePlayAction(
        actionsRoot, QStringLiteral("ExportedPackSpatialNearFar"),
        kSpatialNearFarActionObjectId, *spatialSound);
    addBinding(project.actions, actionsRoot);

}

QJsonArray readPublication(const QString& outputDir)
{
    QJsonArray packs;
    QStringList files;
    QString error;
    EXPECT_TRUE(VNX::ReadPackPublication(outputDir, {}, {}, &packs, &files, &error))
        << error.toStdString();
    return packs;
}

QJsonObject findNamedPack(const QJsonArray& packs, const QString& name)
{
    for (const QJsonValue& value : packs) {
        const QJsonObject object = value.toObject();
        if (object.value(QStringLiteral("name")).toString() == name)
            return object;
    }
    return {};
}

QByteArray fileSha256(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    return QCryptographicHash::hash(file.readAll(), QCryptographicHash::Sha256);
}

QStringList contentPackPaths(const QString& outputDir)
{
    QStringList paths;
    const QJsonArray packs = readPublication(outputDir);
    for (const QJsonValue& value : packs) {
        const QJsonObject pack = value.toObject();
        const QString name = pack.value(QStringLiteral("name")).toString();
        if (name == QStringLiteral("Init"))
            continue;
        const QString fileName =
            QFileInfo(pack.value(QStringLiteral("path")).toString()).fileName();
        paths.append(QDir(outputDir).filePath(fileName));
    }
    paths.sort();
    return paths;
}

uint64_t publishedPackStableId(const QJsonObject& pack)
{
    bool ok = false;
    const uint64_t id =
        pack.value(QStringLiteral("packStableId")).toString().toULongLong(&ok);
    EXPECT_TRUE(ok) << pack.value(QStringLiteral("name")).toString().toStdString();
    return id;
}

QByteArray packPathUtf8(const QString& exportedPackDir, const QJsonObject& pack)
{
    const QString fileName =
        QFileInfo(pack.value(QStringLiteral("path")).toString()).fileName();
    return QFileInfo(QDir(exportedPackDir).filePath(fileName)).absoluteFilePath().toUtf8();
}

void exportProjectPack(const VNX::ProjectData& project, const QString& outputDir)
{
    VNX::ProjectPackExporter exporter;
    VNX::ProjectPackExporter::ExportOptions opts;
    opts.platform = HarkenTest::platformIdString("Windows");
    opts.debugStrings = false;
    const auto result = exporter.ExportAllPacks(project, outputDir, opts);
    ASSERT_TRUE(result.success) << result.error.toStdString();
    ASSERT_TRUE(QFileInfo::exists(QDir(outputDir).filePath(QStringLiteral("Init.bytes"))));
    EXPECT_FALSE(QFileInfo::exists(QDir(outputDir).filePath(QStringLiteral("Shared.bytes"))));
}







struct ExportedPackActivation {
    std::vector<QByteArray> paths;
    std::vector<VnCanonicalPackSetMemberV1> members;
    std::vector<VnCanonicalPackSetMemberResultV1> rows;
};












void rewriteContentTone(const QString& projectRoot, float amplitude)
{
    ASSERT_EQ(kContentSourceId,
              registerToneAsset(projectRoot, QStringLiteral("content.wav"),
                                kContentSourceId, amplitude));
    VNX::AssetInfo* info = VNX::AssetRegistry::instance().getAsset(kContentSourceId);
    ASSERT_NE(nullptr, info);
    ++info->lastModified;
    HarkenEditor::Bridge::mediaRegistryUnregister(kContentSourceId);
    HarkenEditor::Bridge::mediaRegistryRegister(kContentSourceId, info->absolutePath,
                                               info->metadata.fileSize);
}

void exportProjectPackSuccessor(VNX::ProjectData& project,
                         const QString& projectRoot,
                         const QString& outputDir)
{
    ASSERT_NO_FATAL_FAILURE(rewriteContentTone(projectRoot, kSuccessorToneAmplitude));
    ASSERT_TRUE(QDir().mkpath(outputDir));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project, outputDir));
    ASSERT_NO_FATAL_FAILURE(rewriteContentTone(projectRoot, kToneAmplitude));
}


VnActionFiringHandle postGoldAction(harkenstudio::ObjectId actionId)
{
    VnPostActionDesc post{};
    post.structSize = sizeof(post);
    post.actionId = harkentest::ToRuntime(harkentest::StudioObjectId(actionId));
    EXPECT_EQ(VN_OK, Vn_CreateEmitter(&post.emitter));
    VnActionFiringHandle firing = 0;
    EXPECT_EQ(VN_OK, Vn_PostAction(&post, &firing)) << "actionId=" << actionId;
    EXPECT_NE(0u, firing);
    return firing;
}

uint64_t queryProducedInstance(VnActionFiringHandle firing)
{
    VnActionStepResult step{};
    step.structSize = sizeof(step);
    EXPECT_EQ(VN_OK, Vn_QueryActionStep(firing, 0, &step));
    EXPECT_NE(0u, step.producedPlayableInstance);
    return step.producedPlayableInstance;
}





struct GoldJournalRecord {
    uint16_t type = 0;
    std::vector<uint8_t> payload;
};




void prepareExportedPackProject(VNX::ProjectData& project, const QString& projectRoot)
{
    VNX::AssetRegistry::instance().clear();
    VNX::AssetRegistry::instance().setProjectRoot(projectRoot);
    ASSERT_EQ(kSharedSourceId,
              registerToneAsset(projectRoot, QStringLiteral("shared.wav"),
                                kSharedSourceId));
    ASSERT_EQ(kContentSourceId,
              registerToneAsset(projectRoot, QStringLiteral("content.wav"),
                                kContentSourceId));
    ASSERT_EQ(kRandomChildSourceId,
              registerToneAsset(projectRoot, QStringLiteral("random-child.wav"),
                                kRandomChildSourceId));
    ASSERT_EQ(kSequenceChildSourceId,
              registerToneAsset(projectRoot, QStringLiteral("sequence-child.wav"),
                                kSequenceChildSourceId));
    ASSERT_EQ(kPackBSourceId,
              registerToneAsset(projectRoot, QStringLiteral("pack-b.wav"),
                                kPackBSourceId));
    ASSERT_EQ(kQueueSoundSourceId,
              registerToneAsset(projectRoot, QStringLiteral("queue-sound.wav"),
                                kQueueSoundSourceId));
    ASSERT_EQ(kDirectTrackSourceId,
              registerToneAsset(projectRoot, QStringLiteral("direct-track.wav"),
                                kDirectTrackSourceId));
    ASSERT_EQ(kQueueTrackSourceId,
              registerToneAsset(projectRoot, QStringLiteral("queue-track.wav"),
                                kQueueTrackSourceId));
    ASSERT_EQ(kQueueTrackSourceBId,
              registerToneAsset(projectRoot, QStringLiteral("queue-track-b.wav"),
                                kQueueTrackSourceBId, -kToneAmplitude));
    ASSERT_EQ(kDirectSegmentSourceId,
              registerToneAsset(projectRoot, QStringLiteral("direct-segment.wav"),
                                kDirectSegmentSourceId));
    ASSERT_EQ(kQueueSegmentSourceId,
              registerToneAsset(projectRoot, QStringLiteral("queue-segment.wav"),
                                kQueueSegmentSourceId));
    ASSERT_EQ(kQueueSegmentSourceBId,
              registerToneAsset(projectRoot, QStringLiteral("queue-segment-b.wav"),
                                kQueueSegmentSourceBId, -kToneAmplitude));
    ASSERT_EQ(kDiscreteSilentSourceId,
              registerToneAsset(projectRoot, QStringLiteral("discrete-silent.wav"),
                                kDiscreteSilentSourceId, 0.0f));
    ASSERT_EQ(kDiscreteAudibleSourceId,
              registerToneAsset(projectRoot, QStringLiteral("discrete-audible.wav"),
                                kDiscreteAudibleSourceId));
    ASSERT_EQ(kContinuousSilentSourceId,
              registerToneAsset(projectRoot, QStringLiteral("continuous-silent.wav"),
                                kContinuousSilentSourceId, 0.0f));
    ASSERT_EQ(kContinuousAudibleSourceId,
              registerToneAsset(projectRoot, QStringLiteral("continuous-audible.wav"),
                                kContinuousAudibleSourceId));
    ASSERT_EQ(kInitiallySilentSourceId,
              registerToneAsset(projectRoot, QStringLiteral("initially-silent.wav"),
                                kInitiallySilentSourceId, 0.0f));
    ASSERT_EQ(kInitiallyAudibleSourceId,
              registerToneAsset(projectRoot, QStringLiteral("initially-audible.wav"),
                                kInitiallyAudibleSourceId));
    ASSERT_EQ(kNestedRandomSourceId,
              registerToneAsset(projectRoot, QStringLiteral("nested-random.wav"),
                                kNestedRandomSourceId));
    ASSERT_EQ(kDelayedSourceId,
              registerToneAsset(projectRoot, QStringLiteral("delayed.wav"),
                                kDelayedSourceId));
    ASSERT_EQ(kTwoPlaySourceId,
              registerToneAsset(projectRoot, QStringLiteral("two-play.wav"),
                                kTwoPlaySourceId));
    ASSERT_EQ(kSpatialNearFarSourceId,
              registerToneAsset(projectRoot, QStringLiteral("spatial-nearfar.wav"),
                                kSpatialNearFarSourceId));
    ASSERT_NO_FATAL_FAILURE(buildExportedPack(project, projectRoot));
}

class AuthoredPackLifecycle : public ::testing::Test
{
protected:
    void SetUp() override
    {
        qputenv("HARKEN_STUDIO_CLI_PATH", QByteArray(HARKEN_STUDIO_CLI_PATH));
        ASSERT_TRUE(dir_.isValid());
        ASSERT_NO_FATAL_FAILURE(prepareExportedPackProject(project_, dir_.path()));
    }

    void TearDown() override
    {
        VNX::AssetRegistry::instance().clear();
    }

    QTemporaryDir dir_;
    VNX::ProjectData project_;
};


class ExportedPackLifecycle : public AuthoredPackLifecycle
{
protected:
    static void SetUpTestSuite()
    {
        qputenv("HARKEN_STUDIO_CLI_PATH", QByteArray(HARKEN_STUDIO_CLI_PATH));
        preparedDir_ = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(preparedDir_->isValid());
        {
            QTemporaryDir sourceDir;
            ASSERT_TRUE(sourceDir.isValid());
            const auto clearAssets = qScopeGuard([]() {
                VNX::AssetRegistry::instance().clear();
            });
            VNX::ProjectData project;
            ASSERT_NO_FATAL_FAILURE(prepareExportedPackProject(project, sourceDir.path()));
            ASSERT_NO_FATAL_FAILURE(exportProjectPack(project, preparedDir_->path()));
        }
    }

    static void TearDownTestSuite()
    {
        VNX::AssetRegistry::instance().clear();
        preparedDir_.reset();
    }

    static QString preparedExportedPackDir()
    {
        return preparedDir_->path();
    }

private:
    static std::unique_ptr<QTemporaryDir> preparedDir_;
};

std::unique_ptr<QTemporaryDir> ExportedPackLifecycle::preparedDir_;

class ExportedPackReadOnlyLifecycle : public ::testing::Test
{
protected:
    static void SetUpTestSuite()
    {
        qputenv("HARKEN_STUDIO_CLI_PATH", QByteArray(HARKEN_STUDIO_CLI_PATH));
        preparedDir_ = std::make_unique<QTemporaryDir>();
        ASSERT_TRUE(preparedDir_->isValid());
        {
            QTemporaryDir sourceDir;
            ASSERT_TRUE(sourceDir.isValid());
            const auto clearAssets = qScopeGuard([]() {
                VNX::AssetRegistry::instance().clear();
            });
            VNX::ProjectData project;
            ASSERT_NO_FATAL_FAILURE(prepareExportedPackProject(project, sourceDir.path()));
            ASSERT_NO_FATAL_FAILURE(exportProjectPack(project, preparedDir_->path()));
        }
    }

    static void TearDownTestSuite()
    {
        VNX::AssetRegistry::instance().clear();
        preparedDir_.reset();
    }

    void SetUp() override
    {
        ASSERT_NE(nullptr, preparedDir_);
        ASSERT_TRUE(preparedDir_->isValid());
    }

    static QString preparedExportedPackDir()
    {
        return preparedDir_->path();
    }

private:
    static std::unique_ptr<QTemporaryDir> preparedDir_;
};


std::filesystem::path att06FsPath(const std::string& utf8)
{
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(utf8.data()), utf8.size()));
}

std::filesystem::path att06FsPath(const QString& path)
{
    const QByteArray utf8 = path.toUtf8();
    return std::filesystem::path(std::u8string(
        reinterpret_cast<const char8_t*>(utf8.constData()),
        static_cast<size_t>(utf8.size())));
}

std::string att06Utf8(const std::filesystem::path& path)
{
    const auto text = path.generic_u8string();
    return {reinterpret_cast<const char*>(text.data()), text.size()};
}

std::string att06CanonicalUtf8(const std::filesystem::path& path)
{
    std::error_code ec;
    const auto canonical = std::filesystem::weakly_canonical(path, ec);
    if (ec)
        return {};
    return att06Utf8(canonical);
}

struct Att06Opened {
    size_t index = 0;
    uint64_t packStableId = 0;
    std::string path;
};

struct Att06Identity {
    uint64_t activationGeneration = 0;
    uint64_t busContentGeneration = 0;
    uint64_t publishedBusFingerprint = 0;
    uint64_t compileTargetProfileFingerprint = 0;
    uint64_t initStableId = 0;
};

bool att06ParseOpened(const std::string& line, Att06Opened* out)
{
    static const char kPrefix[] = "att04-nearfar opened ";
    if (line.size() < sizeof(kPrefix) - 1 ||
        line.compare(0, sizeof(kPrefix) - 1, kPrefix) != 0)
        return false;
    const char* cursor = line.c_str() + sizeof(kPrefix) - 1;
    char* end = nullptr;
    const unsigned long index = std::strtoul(cursor, &end, 10);
    if (end == cursor || *end != ' ')
        return false;
    cursor = end + 1;
    const unsigned long long id = std::strtoull(cursor, &end, 10);
    if (end == cursor || *end != ' ')
        return false;
    std::string path = end + 1;
    if (!path.empty() && path.back() == '\r')
        path.pop_back();
    if (path.empty() || path.find('\n') != std::string::npos ||
        path.find('\r') != std::string::npos)
        return false;
    out->index = static_cast<size_t>(index);
    out->packStableId = static_cast<uint64_t>(id);
    out->path = std::move(path);
    return true;
}

bool att06ParseIdentity(const std::string& output, Att06Identity* out)
{
    const QRegularExpression line(
        QStringLiteral(
            "^att04-nearfar identity activationGeneration=([0-9]+) "
            "busContentGeneration=([0-9]+) publishedBusFingerprint=([0-9]+) "
            "compileTargetProfileFingerprint=([0-9]+) initStableId=([0-9]+)\\r?$"),
        QRegularExpression::MultilineOption);
    const auto match = line.match(QString::fromUtf8(output.data(), int(output.size())));
    if (!match.hasMatch())
        return false;
    bool ok = false;
    out->activationGeneration = match.captured(1).toULongLong(&ok);
    if (!ok) return false;
    out->busContentGeneration = match.captured(2).toULongLong(&ok);
    if (!ok) return false;
    out->publishedBusFingerprint = match.captured(3).toULongLong(&ok);
    if (!ok) return false;
    out->compileTargetProfileFingerprint = match.captured(4).toULongLong(&ok);
    if (!ok) return false;
    out->initStableId = match.captured(5).toULongLong(&ok);
    return ok;
}

bool att06HasDistanceSuccess(const std::string& output)
{
    const QRegularExpression line(
        QStringLiteral(
            R"(^att04-nearfar distance=([0-9.]+) peak=([0-9.]+) firing=([0-9]+) VnResult=([0-9-]+)\r?$)"),
        QRegularExpression::MultilineOption);
    return line.match(QString::fromUtf8(output.data(), int(output.size()))).hasMatch();
}


std::map<std::string, QByteArray> att06SnapshotTree(const QString& root)
{
    std::map<std::string, QByteArray> files;
    QDirIterator it(root, QDir::Files | QDir::Hidden | QDir::NoSymLinks,
                    QDirIterator::Subdirectories);
    while (it.hasNext()) {
        const QString path = it.next();
        const std::string key = att06CanonicalUtf8(att06FsPath(path));
        if (key.empty())
            continue;
        files.emplace(key, fileSha256(path));
    }
    return files;
}

QStringList att06Att04Args(const QString& root, float distance)
{
    return QStringList{
        QStringLiteral("att04-nearfar"),
        QStringLiteral("--null"),
        QStringLiteral("--seconds"),
        QStringLiteral("0.4"),
        QStringLiteral("--root"),
        root,
        QStringLiteral("--action-name"),
        QStringLiteral("ExportedPackSpatialNearFar"),
        QStringLiteral("--distance"),
        QString::fromStdString(std::to_string(distance)),
    };
}

void att06CollectOpened(const std::string& output, std::vector<Att06Opened>* opened)
{
    opened->clear();
    std::string line;
    std::istringstream stream(output);
    while (std::getline(stream, line)) {
        if (!line.empty() && line.back() == '\r')
            line.pop_back();
        Att06Opened row;
        if (att06ParseOpened(line, &row))
            opened->push_back(std::move(row));
    }
}

QString att06QString(const std::string& utf8)
{
    return QString::fromUtf8(utf8.data(), int(utf8.size()));
}

bool att06CopyTree(const std::filesystem::path& src, const std::filesystem::path& dst)
{
    std::error_code ec;
    std::filesystem::copy(src, dst,
                          std::filesystem::copy_options::recursive |
                              std::filesystem::copy_options::overwrite_existing,
                          ec);
    return !ec;
}

bool att06StageFile(const std::filesystem::path& src, const std::filesystem::path& dst)
{
    std::error_code ec;
    std::filesystem::create_directories(dst.parent_path(), ec);
    if (ec)
        return false;
    std::filesystem::remove(dst, ec);
    ec.clear();
    std::filesystem::create_hard_link(src, dst, ec);
    if (!ec)
        return true;
    return QFile::copy(att06QString(att06Utf8(src)), att06QString(att06Utf8(dst)));
}

bool att06StageTree(const std::filesystem::path& src, const std::filesystem::path& dst)
{
    std::error_code ec;
    if (std::filesystem::is_directory(src, ec)) {
        std::filesystem::create_directories(dst, ec);
        if (ec)
            return false;
        for (const auto& entry : std::filesystem::recursive_directory_iterator(src, ec)) {
            if (ec)
                return false;
            const auto rel = std::filesystem::relative(entry.path(), src, ec);
            if (ec)
                return false;
            const auto target = dst / rel;
            if (entry.is_directory(ec)) {
                std::filesystem::create_directories(target, ec);
                if (ec)
                    return false;
                continue;
            }
            if (!att06StageFile(entry.path(), target))
                return false;
        }
        return !ec;
    }
    return att06StageFile(src, dst);
}

bool att06ChoosePayload(const std::vector<uint8_t>& bytes, uint64_t* offset, uint64_t* size,
                        uint64_t* payloadChecksum, uint64_t* directoryChecksum)
{
    if (bytes.size() < harkenstudiobuild::kPackFileHeaderBytes)
        return false;
    harkenstudiobuild::PackFileHeader header;
    if (!harkenstudiobuild::ReadPackFileHeader(bytes.data(), &header))
        return false;
    if (header.fileBytes != bytes.size() ||
        header.headerBytes != harkenstudiobuild::kPackFileHeaderBytes ||
        header.directoryOffset != harkenstudiobuild::kPackFileHeaderBytes)
        return false;
    if (header.entryCount == 0 ||
        header.entryCount > (std::numeric_limits<uint64_t>::max() /
                             harkenstudiobuild::kPackDirectoryEntryBytes))
        return false;
    const uint64_t dirBytes =
        uint64_t(header.entryCount) * harkenstudiobuild::kPackDirectoryEntryBytes;
    if (header.directoryOffset > bytes.size() ||
        dirBytes > bytes.size() - header.directoryOffset)
        return false;
    const uint64_t dirEnd = header.directoryOffset + dirBytes;
    *directoryChecksum = harkenstudiobuild::Fnv1a64(bytes.data() + header.directoryOffset,
                                        static_cast<size_t>(dirBytes));
    if (*directoryChecksum != header.directoryChecksum)
        return false;
    for (uint32_t i = 0; i < header.entryCount; ++i) {
        const auto entry = harkenstudiobuild::ReadPackDirectoryEntry(
            bytes.data() + header.directoryOffset +
            size_t(i) * harkenstudiobuild::kPackDirectoryEntryBytes);
        if (entry.kind == harkenstudiobuild::kPackEntryKindInitRoutingCatalog)
            continue;
        if (entry.size == 0 || entry.offset < dirEnd ||
            entry.offset % harkenstudiobuild::kPackPayloadAlign != 0 ||
            entry.offset > bytes.size() ||
            entry.size > bytes.size() - entry.offset)
            continue;
        *offset = entry.offset;
        *size = entry.size;
        *payloadChecksum = entry.payloadChecksum;
        return true;
    }
    return false;
}


std::unique_ptr<QTemporaryDir> ExportedPackReadOnlyLifecycle::preparedDir_;

} // namespace

TEST_F(AuthoredPackLifecycle, ExportIsDeterministic)
{
    const QString firstDir = QDir(dir_.path()).filePath(QStringLiteral("export-a"));
    const QString secondDir = QDir(dir_.path()).filePath(QStringLiteral("export-b"));
    ASSERT_TRUE(QDir().mkpath(firstDir));
    ASSERT_TRUE(QDir().mkpath(secondDir));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, firstDir));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, secondDir));

    EXPECT_EQ(fileSha256(QDir(firstDir).filePath(QStringLiteral("Init.bytes"))),
              fileSha256(QDir(secondDir).filePath(QStringLiteral("Init.bytes"))));

    const QStringList firstContent = contentPackPaths(firstDir);
    const QStringList secondContent = contentPackPaths(secondDir);
    ASSERT_EQ(firstContent.size(), secondContent.size());
    ASSERT_FALSE(firstContent.isEmpty());
    for (int i = 0; i < firstContent.size(); ++i) {
        EXPECT_EQ(QFileInfo(firstContent[i]).fileName(),
                  QFileInfo(secondContent[i]).fileName());
        EXPECT_EQ(fileSha256(firstContent[i]), fileSha256(secondContent[i]))
            << firstContent[i].toStdString();
    }
}

namespace {

constexpr float kAtt05ReplacementMaxDistance = 80.0f;
constexpr uint32_t kAtt05TerminalBlockCap = 32;

struct Att05Publication {
    VnPackHandle initHandle = 0;
    uint64_t setRevision = 0;
    uint64_t activationGeneration = 0;
    uint64_t publishedBusFingerprint = 0;
};



void expectSamePublication(const Att05Publication& before, const Att05Publication& after)
{
    EXPECT_EQ(before.initHandle, after.initHandle);
    EXPECT_EQ(before.setRevision, after.setRevision);
    EXPECT_EQ(before.activationGeneration, after.activationGeneration);
    EXPECT_EQ(before.publishedBusFingerprint, after.publishedBusFingerprint);
}

VnObjectId att05AttenuationId()
{
    return harkentest::ToRuntime(harkentest::StudioObjectId(
        kSpatialAttenuationObjectId + kStableIdBias));
}


void att05PostSpatial(VnActionFiringHandle* firing)
{
    ASSERT_NE(nullptr, firing);
    *firing = 0;
    VnPostActionDesc post{};
    post.structSize = sizeof(post);
    post.actionId = harkentest::ToRuntime(harkentest::StudioObjectId(
        kSpatialNearFarActionObjectId + kStableIdBias));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&post.emitter));
    ASSERT_EQ(VN_OK, Vn_PostAction(&post, firing));
    ASSERT_NE(0u, *firing);
}



VNX::VAttenuation* att05Attenuation(VNX::ProjectData& project)
{
    for (auto& binding : project.attenuationItems) {
        if (binding.rootItem && binding.rootItem->objectID == kSpatialAttenuationObjectId)
            return binding.rootItem->comp<VNX::VAttenuation>();
    }
    return nullptr;
}

std::vector<std::uint8_t> att05ReadFile(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    const QByteArray bytes = file.readAll();
    return {bytes.begin(), bytes.end()};
}

bool att05DropAttenuationEntry(std::vector<std::uint8_t>* bytes, VnObjectId target, int* removedCount)
{
    if (bytes->size() < harkenstudiobuild::kPackFileHeaderBytes)
        return false;
    harkenstudiobuild::PackFileHeader header;
    if (!harkenstudiobuild::ReadPackFileHeader(bytes->data(), &header))
        return false;
    const uint64_t directoryBytes =
        static_cast<uint64_t>(header.entryCount) * harkenstudiobuild::kPackDirectoryEntryBytes;
    if (header.directoryOffset != header.headerBytes || header.entryCount == 0
        || header.directoryOffset + directoryBytes > bytes->size())
        return false;
    int found = -1;
    int attenuationCount = 0;
    for (uint32_t index = 0; index < header.entryCount; ++index) {
        const auto entry = harkenstudiobuild::ReadPackDirectoryEntry(
            bytes->data() + header.directoryOffset
            + static_cast<size_t>(index) * harkenstudiobuild::kPackDirectoryEntryBytes);
        if (entry.kind != harkenstudiobuild::kPackEntryKindAttenuation)
            continue;
        ++attenuationCount;
        if (entry.id == target) {
            if (found >= 0)
                return false;
            found = static_cast<int>(index);
        }
    }
    if (found < 0)
        return false;
    auto* directory = bytes->data() + header.directoryOffset;
    const uint32_t tail = header.entryCount - static_cast<uint32_t>(found) - 1u;
    std::memmove(directory + static_cast<size_t>(found) * harkenstudiobuild::kPackDirectoryEntryBytes,
                 directory + (static_cast<size_t>(found) + 1u) * harkenstudiobuild::kPackDirectoryEntryBytes,
                 static_cast<size_t>(tail) * harkenstudiobuild::kPackDirectoryEntryBytes);
    header.entryCount -= 1u;
    header.directoryChecksum = harkenstudiobuild::Fnv1a64(
        directory, static_cast<size_t>(header.entryCount) * harkenstudiobuild::kPackDirectoryEntryBytes);
    harkenstudiobuild::SerializePackFileHeader(header, bytes->data());
    *removedCount = attenuationCount;
    return true;
}

} // namespace
