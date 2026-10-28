#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Engine.h"
#include "ProjectGenerationTransaction.h"
#include "UnitTest/TestUtil.h"
#include "UnitTest/CanonicalPackSetActivate.h"

#include "AssetPipeline/AssetRegistry.h"
#include "AssetPipeline/PathUtils.h"
#include "Components/VAction.h"
#include "Components/VAttenuation.h"
#include "Components/VAudio.h"
#include "Components/VMixNode.h"
#include "Components/VPiece.h"
#include "Components/VSelection.h"
#include "Components/VSound.h"
#include "Components/VTrack.h"
#include "Components/VVariable.h"
#include "BindingSemantics.h"
#include "CurrentWorldProjectAdapter.h"
#include "PlayablePropertyAuthoringV1.h"
#include "AudioEngine.h"
#include "PlatformContextService.h"
#include "ProjectManager.h"
#include "FolderSaveState.h"
#include "ProjectPackExporter.h"
#include "CanonicalPackSetExport.h"
#include "RuntimeSyncCoordinator.h"
#include "ProjectTypes.h"
#include "FieldGroupProjectAdapter.h"
#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"
#include "RuntimeBridge/PackagingBridge.h"
#include "Timeline/TimelineTypes.h"

#include "harken/Harken.h"
#include "harken/media/PackFormatV2.h"
#include "harken/media/PackLogicImage.h"
#include "TraceFormat.h"

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

#ifndef HARKEN_HOST_PATH
#error "HARKEN_HOST_PATH must point at the harken_host executable"
#endif

#ifndef HARKEN_PACKC_PATH
#error "HARKEN_PACKC_PATH must point at the packc executable"
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
constexpr uint32_t kDirectPieceSoundObjectId = 19;
constexpr uint32_t kDirectPieceTrackObjectId = 31;
constexpr uint32_t kDirectPieceObjectId = 32;
constexpr uint32_t kDirectPieceActionObjectId = 29;
constexpr uint32_t kQueuePieceSoundObjectId = 33;
constexpr uint32_t kQueuePieceSoundBObjectId = 58;
constexpr uint32_t kQueuePieceTrackObjectId = 34;
constexpr uint32_t kQueuePieceObjectId = 35;
constexpr uint32_t kQueuePieceContainerObjectId = 36;
constexpr uint32_t kQueuePieceActionObjectId = 37;
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
constexpr uint32_t kDirectPieceSourceId = 1009;
constexpr uint32_t kQueuePieceSourceId = 1010;
constexpr uint32_t kQueuePieceSourceBId = 1019;
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
        item, *locator, static_cast<uint8_t>(Id::MakeUpGain), QJsonValue(0.0), std::nullopt));
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

void makePieceWithSound(const std::shared_ptr<VNX::ProjectItem>& parent,
                        const QString& pieceName,
                        uint32_t pieceObjectId,
                        const QString& trackName,
                        uint32_t trackObjectId,
                        const QString& soundName,
                        uint32_t soundObjectId,
                        uint32_t sourceId,
                        std::shared_ptr<VNX::ProjectItem>* outPiece)
{
    auto piece = makeItem(pieceName, VNX::ProjectItem::Type::Piece, pieceObjectId);
    ASSERT_NO_FATAL_FAILURE(authorDefaultPlayableProperties(*piece));
    addChild(parent, piece);
    std::shared_ptr<VNX::ProjectItem> track;
    ASSERT_NO_FATAL_FAILURE(makeTrackWithSound(piece, trackName, trackObjectId, soundName,
                                               soundObjectId, sourceId, &track));
    *outPiece = std::move(piece);
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
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kDirectPieceSourceId));
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kQueuePieceSourceId));
    ASSERT_NO_FATAL_FAILURE(addAudioSource(project, kQueuePieceSourceBId));
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

    std::shared_ptr<VNX::ProjectItem> directPiece;
    ASSERT_NO_FATAL_FAILURE(makePieceWithSound(
        layer, QStringLiteral("ExportedPackDirectPiece"), kDirectPieceObjectId,
        QStringLiteral("ExportedPackDirectPieceTrack"), kDirectPieceTrackObjectId,
        QStringLiteral("ExportedPackDirectPieceSound"), kDirectPieceSoundObjectId,
        kDirectPieceSourceId, &directPiece));

    auto queuePieceContainer = makeItem(QStringLiteral("ExportedPackQueuePieceContainer"),
                                        VNX::ProjectItem::Type::Container,
                                        kQueuePieceContainerObjectId);
    ASSERT_NO_FATAL_FAILURE(prepareContainer(queuePieceContainer,
                                             VNX::SelectionMode::Sequence, VNX::SelectionPlayType::Queue));
    addChild(layer, queuePieceContainer);
    std::shared_ptr<VNX::ProjectItem> queuePiece;
    ASSERT_NO_FATAL_FAILURE(makePieceWithSound(
        queuePieceContainer, QStringLiteral("ExportedPackQueuePiece"), kQueuePieceObjectId,
        QStringLiteral("ExportedPackQueuePieceTrack"), kQueuePieceTrackObjectId,
        QStringLiteral("ExportedPackQueuePieceSound"), kQueuePieceSoundObjectId,
        kQueuePieceSourceId, &queuePiece));
    {
        std::shared_ptr<VNX::ProjectItem> queuePieceTrack;
        for (const auto& child : queuePiece->children) {
            if (child && child->type == VNX::ProjectItem::Type::Track) {
                queuePieceTrack = child;
                break;
            }
        }
        ASSERT_NE(queuePieceTrack, nullptr);
        ASSERT_NO_FATAL_FAILURE(attachSecondTrackLeaf(
            queuePieceTrack, QStringLiteral("ExportedPackQueuePieceSoundB"),
            kQueuePieceSoundBObjectId, kQueuePieceSourceBId));
    }
    ASSERT_NO_FATAL_FAILURE(attachContainerChild(queuePieceContainer, queuePiece));

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
    auto queuePieceAction = makePlayAction(
        actionsRoot, QStringLiteral("ExportedPackQueuePieceChild"),
        kQueuePieceActionObjectId, *queuePieceContainer);
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
    auto directPieceAction = makePlayAction(
        actionsRoot, QStringLiteral("ExportedPackDirectPiece"),
        kDirectPieceActionObjectId, *directPiece);
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

bool bufferIsAudible(const std::vector<float>& samples)
{
    for (float sample : samples) {
        if (std::fabs(sample) > harkentest::kSilenceFloor)
            return true;
    }
    return false;
}

struct LoadedExportedPack {
    harkentest::EngineSession session;
    VnPackHandle initHandle = 0;
    VnPackHandle contentHandle = 0;
    VnPackHandle packBHandle = 0;
    QByteArray rootUtf8;
    QByteArray initPathUtf8;
    QByteArray nextRootUtf8;
    QByteArray nextPathUtf8;
    std::vector<QByteArray> memberPaths;
    std::vector<VnCanonicalPackSetMemberV1> members;
    std::vector<VnCanonicalPackSetMemberResultV1> memberResults;
    VnCanonicalPackSetActivationReportV1 activationReport{};

    explicit LoadedExportedPack(const VnConfig& config) : session(config) {}
    LoadedExportedPack() : LoadedExportedPack(harkentest::MakeNullConfig(8, 256, 48000)) {}
};

struct FrozenPlayableOwnership {
    uint64_t instance = 0;
    harken::PackPartitionLease packLease{};
    std::uint64_t playbackGeneration = 0;
    std::uint64_t playbackRootPack = 0;
    bool queueUsesCurrent = false;
    bool variableControlUsesCurrent = false;
    std::uint32_t selectedChildOrdinal = 0;
    harken::PlayableRefV1 leaf{};
};

const harken::EnginePlayableServiceRow* findStartRow(harken::Engine* engine,
                                                  uint64_t instance)
{
    if (engine == nullptr || instance == 0)
        return nullptr;
    for (const auto& row : engine->playableServices) {
        if (row.rootInstance == instance)
            return &row;
    }
    return nullptr;
}

void captureOwnership(harken::Engine* engine,
                      uint64_t instance,
                      FrozenPlayableOwnership* out)
{
    ASSERT_NE(nullptr, engine);
    ASSERT_NE(nullptr, out);
    const auto* row = findStartRow(engine, instance);
    ASSERT_NE(nullptr, row);
    out->instance = instance;
    out->packLease = row->Invocation().packLease;
    out->playbackGeneration = row->Invocation().packPlaybackView.CompositeGeneration();
    out->playbackRootPack = row->Invocation().packPlaybackView.RootPackStableId();
    out->queueUsesCurrent = row->queueWorldUsesCurrentAuthority;
    out->variableControlUsesCurrent = row->variableControlWorldUsesCurrentAuthority;
    out->selectedChildOrdinal =
        row->selectionCount == 0 ? 0u : row->selectedChildOrdinals[0];
    out->leaf = row->leaf;
    EXPECT_NE(0u, out->packLease.leaseToken);
    EXPECT_NE(0u, out->packLease.compositeGeneration);
    EXPECT_FALSE(out->queueUsesCurrent);
    EXPECT_FALSE(out->variableControlUsesCurrent);
}

void expectFrozenOwnership(harken::Engine* engine,
                           const FrozenPlayableOwnership& frozen)
{
    ASSERT_NE(nullptr, engine);
    const auto* row = findStartRow(engine, frozen.instance);
    ASSERT_NE(nullptr, row);
    EXPECT_EQ(frozen.packLease.leaseToken, row->Invocation().packLease.leaseToken);
    EXPECT_EQ(frozen.packLease.compositeGeneration, row->Invocation().packLease.compositeGeneration);
    EXPECT_EQ(frozen.packLease.handle, row->Invocation().packLease.handle);
    EXPECT_EQ(frozen.packLease.packStableId, row->Invocation().packLease.packStableId);
    EXPECT_EQ(frozen.packLease.slotGeneration, row->Invocation().packLease.slotGeneration);
    EXPECT_EQ(frozen.playbackGeneration, row->Invocation().packPlaybackView.CompositeGeneration());
    EXPECT_EQ(frozen.playbackRootPack, row->Invocation().packPlaybackView.RootPackStableId());
    EXPECT_FALSE(row->queueWorldUsesCurrentAuthority);
    EXPECT_FALSE(row->variableControlWorldUsesCurrentAuthority);
    EXPECT_EQ(VN_OK, engine->packComposition.ValidateLease(frozen.packLease));
    EXPECT_EQ(VN_OK, engine->packComposition.ValidateLease(row->Invocation().packLease));
}

struct ExportedPackActivation {
    std::vector<QByteArray> paths;
    std::vector<VnCanonicalPackSetMemberV1> members;
    std::vector<VnCanonicalPackSetMemberResultV1> rows;
};

void prepareExportedPackActivation(const QString& exportedPackDir,
                                  const QString& actionName,
                                  const LoadedExportedPack& loaded,
                                  ExportedPackActivation* prepared)
{
    ASSERT_NE(nullptr, prepared);
    *prepared = {};
    const auto packs = readPublication(exportedPackDir);
    std::map<uint64_t, QJsonObject> byId;
    uint64_t initId = 0;
    for (const auto& value : packs) {
        const auto pack = value.toObject();
        const uint64_t id = publishedPackStableId(pack);
        ASSERT_NE(0u, id);
        ASSERT_TRUE(byId.emplace(id, pack).second);
        if (pack.value(QStringLiteral("packKind")).toInt() == 2) {
            ASSERT_EQ(0u, initId);
            initId = id;
        } else {
            ASSERT_EQ(3, pack.value(QStringLiteral("packKind")).toInt());
        }
    }
    ASSERT_NE(0u, initId);
    const auto selected = findNamedPack(packs, actionName);
    ASSERT_FALSE(selected.isEmpty());
    ASSERT_EQ(3, selected.value(QStringLiteral("packKind")).toInt());
    std::vector<uint64_t> pending{initId, publishedPackStableId(selected)};
    for (const auto& member : loaded.members)
        pending.push_back(member.packStableId);
    std::set<uint64_t> closure;
    while (!pending.empty()) {
        const uint64_t id = pending.back();
        pending.pop_back();
        if (!closure.insert(id).second)
            continue;
        const auto found = byId.find(id);
        ASSERT_NE(found, byId.end()) << "Missing publication dependency " << id;
        for (const auto& dependency : found->second.value(QStringLiteral("dependencies")).toArray()) {
            bool ok = false;
            const uint64_t dependencyId = dependency.toString().toULongLong(&ok);
            ASSERT_TRUE(ok);
            ASSERT_NE(0u, dependencyId);
            pending.push_back(dependencyId);
        }
    }
    std::vector<uint64_t> ordered{initId};
    for (const uint64_t id : closure) {
        if (id != initId)
            ordered.push_back(id);
    }
    prepared->paths.reserve(ordered.size());
    for (const uint64_t id : ordered)
        prepared->paths.push_back(packPathUtf8(exportedPackDir, byId.at(id)));
    prepared->members.resize(ordered.size());
    prepared->rows.resize(ordered.size());
    for (size_t i = 0; i < ordered.size(); ++i) {
        auto& member = prepared->members[i];
        member.structSize = sizeof(member);
        member.packStableId = ordered[i];
        member.packKind = i == 0 ? VN_CANONICAL_PACK_SET_MEMBER_INIT
                                 : VN_CANONICAL_PACK_SET_MEMBER_CONTENT;
        member.flags = VN_CANONICAL_PACK_SET_MEMBER_ROOT;
        member.utf8Path = prepared->paths[i].constData();
        member.utf8ContainmentRoot = loaded.rootUtf8.constData();
        for (size_t j = 0; j < loaded.members.size(); ++j) {
            if (loaded.members[j].packStableId == member.packStableId)
                member.expectedHandle = loaded.memberResults[j].handle;
        }
        prepared->rows[i].structSize = sizeof(prepared->rows[i]);
    }
}

void acceptExportedPackActivation(ExportedPackActivation* prepared,
                                 const VnCanonicalPackSetActivationReportV1& report,
                                 LoadedExportedPack* loaded)
{
    loaded->memberPaths = std::move(prepared->paths);
    loaded->members = std::move(prepared->members);
    loaded->memberResults = std::move(prepared->rows);
    loaded->activationReport = report;
    loaded->initHandle = report.initHandle;
    ASSERT_NE(0u, loaded->initHandle);
    for (size_t i = 0; i < loaded->members.size(); ++i) {
        loaded->members[i].utf8Path = loaded->memberPaths[i].constData();
        ASSERT_NE(0u, loaded->memberResults[i].handle);
    }
}

VnPackHandle exportedMemberHandle(const LoadedExportedPack& loaded, uint64_t stableId)
{
    for (size_t i = 0; i < loaded.members.size(); ++i) {
        if (loaded.members[i].packStableId == stableId)
            return loaded.memberResults[i].handle;
    }
    return 0;
}

void loadExportedPackSession(const QString& exportedPackDir,
                           const QString& actionName,
                           LoadedExportedPack* loaded)
{
    ASSERT_NE(nullptr, loaded);
    loaded->rootUtf8 = QFileInfo(exportedPackDir).absoluteFilePath().toUtf8();
    loaded->initPathUtf8 = QDir(exportedPackDir).filePath(QStringLiteral("Init.bytes")).toUtf8();
    const QJsonArray packs = readPublication(exportedPackDir);
    const QJsonObject contentPack = findNamedPack(packs, actionName);
    ASSERT_FALSE(contentPack.isEmpty()) << actionName.toStdString();

    VnPackLoadDesc inspectDesc{};
    inspectDesc.structSize = sizeof(inspectDesc);
    inspectDesc.utf8Path = loaded->initPathUtf8.constData();
    VnInitPackInspectReport report{};
    report.structSize = sizeof(report);
    ASSERT_EQ(VN_OK, Vn_InspectInitPack(&inspectDesc, &report));

    ASSERT_EQ(VN_OK, loaded->session.InitResult());
    ASSERT_NE(nullptr, loaded->session.Engine());

    ExportedPackActivation prepared;
    ASSERT_NO_FATAL_FAILURE(prepareExportedPackActivation(
        exportedPackDir, actionName, *loaded, &prepared));
    VnCanonicalPackSetActivationReportV1 activation{};
    const VnPackHandle previousInit = loaded->initHandle;
    const std::uint64_t previousRevision = loaded->activationReport.resultSetRevision;
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(
        prepared.members.data(), static_cast<uint32_t>(prepared.members.size()),
        prepared.rows.data(), activation, previousInit, previousRevision));
    ASSERT_NO_FATAL_FAILURE(acceptExportedPackActivation(&prepared, activation, loaded));
    EXPECT_EQ(previousRevision + 1u, loaded->activationReport.resultSetRevision);
    loaded->contentHandle = exportedMemberHandle(*loaded, publishedPackStableId(contentPack));
    ASSERT_NE(0u, loaded->contentHandle);
}

void appendCapture(harken::Engine* engine, harkentest::Capture* capture)
{
    capture->left.insert(capture->left.end(),
                         engine->nullBackend.LastBlock(),
                         engine->nullBackend.LastBlock() + engine->nullBackend.BlockFrames());
}

void pumpCapture(harken::Engine* engine, harkentest::Capture* capture, uint32_t blocks)
{
    for (uint32_t block = 0; block < blocks; ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
        appendCapture(engine, capture);
    }
}

void expectActivePlayable(VnActionFiringHandle firing,
                          harken::Engine* engine,
                          uint32_t minVoices)
{
    VnActionStepResult step{};
    step.structSize = sizeof(step);
    ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 0, &step));
    EXPECT_EQ(VN_ACTION_STEP_ACTIVE, step.state);
    EXPECT_NE(0u, step.producedPlayableInstance);
    EXPECT_GE(engine->voices.ActiveCount(), minVoices);
}

void postAndCaptureImmediate(LoadedExportedPack* loaded,
                             harkenstudio::ObjectId actionId,
                             harkentest::Capture* capture,
                             uint32_t minVoices)
{
    VnPostActionDesc post{};
    post.structSize = sizeof(post);
    post.actionId = harkentest::ToRuntime(harkentest::StudioObjectId(actionId));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&post.emitter));
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&post, &firing)) << "actionId=" << actionId;
    ASSERT_NE(0u, firing);
    ASSERT_NO_FATAL_FAILURE(expectActivePlayable(firing, loaded->session.Engine(), minVoices));
    ASSERT_NO_FATAL_FAILURE(pumpCapture(loaded->session.Engine(), capture, 16));
    HARKENTEST_ASSERT_NON_SILENT(capture->left);
    const VnResult released = Vn_ReleaseActionFiring(firing);
    EXPECT_TRUE(released == VN_OK || released == VN_IN_USE);
}

void loadExportedPack(const QString& exportedPackDir,
                    const QString& actionName,
                    harkenstudio::ObjectId actionId,
                    harkentest::Capture* capture)
{
    LoadedExportedPack loaded;
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(exportedPackDir, actionName, &loaded));
    ASSERT_NO_FATAL_FAILURE(postAndCaptureImmediate(&loaded, actionId, capture, 1u));
}

void loadAndPostFamily(const QString& exportedPackDir,
                       const QString& actionName,
                       uint32_t actionObjectId)
{
    harkentest::Capture capture;
    ASSERT_NO_FATAL_FAILURE(loadExportedPack(
        exportedPackDir, actionName, harkentest::StudioObjectId(actionObjectId + kStableIdBias), &capture));
}

void loadExportedPackB(const QString& exportedPackDir, LoadedExportedPack* loaded)
{
    ASSERT_NE(nullptr, loaded);
    const QJsonObject packB =
        findNamedPack(readPublication(exportedPackDir),
                      QStringLiteral("ExportedPackPackBAction"));
    ASSERT_FALSE(packB.isEmpty());
    ExportedPackActivation prepared;
    ASSERT_NO_FATAL_FAILURE(prepareExportedPackActivation(
        exportedPackDir, QStringLiteral("ExportedPackPackBAction"), *loaded, &prepared));
    VnCanonicalPackSetActivationReportV1 activation{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(
        prepared.members.data(), static_cast<uint32_t>(prepared.members.size()),
        prepared.rows.data(), activation, loaded->initHandle,
        loaded->activationReport.resultSetRevision));
    ASSERT_NO_FATAL_FAILURE(acceptExportedPackActivation(&prepared, activation, loaded));
    loaded->packBHandle = exportedMemberHandle(*loaded, publishedPackStableId(packB));
    ASSERT_NE(0u, loaded->packBHandle);
}

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

void makeRootReplaceDesc(LoadedExportedPack* loaded,
                         const QString& nextDir,
                         const QString& actionName,
                         VnPackLoadDesc* desc)
{
    ASSERT_NE(nullptr, loaded);
    ASSERT_NE(nullptr, desc);
    loaded->nextRootUtf8 = QFileInfo(nextDir).absoluteFilePath().toUtf8();
    const QJsonObject pack =
        findNamedPack(readPublication(nextDir),
                      actionName);
    ASSERT_FALSE(pack.isEmpty()) << actionName.toStdString();
    const QString fileName =
        QFileInfo(pack.value(QStringLiteral("path")).toString()).fileName();
    loaded->nextPathUtf8 = QDir(nextDir).filePath(fileName).toUtf8();
    *desc = {};
    desc->structSize = sizeof(*desc);
    desc->flags = VN_PACK_LOAD_FLAG_ROOT;
    desc->packStableId = publishedPackStableId(pack);
    desc->utf8Path = loaded->nextPathUtf8.constData();
    desc->utf8ContainmentRoot = loaded->nextRootUtf8.constData();
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

void pumpUntilAudible(harken::Engine* engine, harkentest::Capture* capture)
{
    bool heard = false;
    for (uint32_t block = 0; block < 32 && !heard; ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
        appendCapture(engine, capture);
        heard = bufferIsAudible(capture->left);
    }
    HARKENTEST_ASSERT_NON_SILENT(capture->left);
}

void drainPlayableAndFiring(harken::Engine* engine, VnActionFiringHandle firing)
{
    ASSERT_NE(nullptr, engine);
    for (uint32_t block = 0; block < 256; ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
        if (engine->playableInstances.LiveCount() == 0
            && engine->actionDeferredRecords.LiveCount() == 0
            && engine->voices.ActiveCount() == 0)
            break;
    }
    const VnResult released = Vn_ReleaseActionFiring(firing);
    EXPECT_TRUE(released == VN_OK || released == VN_STALE_HANDLE);
    for (uint32_t block = 0; block < 64; ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
        if (engine->packs.RetiringCount() == 0
            && engine->playableInstances.LiveCount() == 0)
            break;
    }
}

void expectInitVariablesReadable(harken::Engine* engine)
{
    ASSERT_NE(nullptr, engine);
    float step = 0.0f;
    float smooth = 0.0f;
    ASSERT_TRUE(engine->variables.CurrentValue(
        harkentest::ObjectId(kStepVariableObjectId + kStableIdBias), &step));
    ASSERT_TRUE(engine->variables.CurrentValue(
        harkentest::ObjectId(kSmoothVariableObjectId + kStableIdBias), &smooth));
    EXPECT_FLOAT_EQ(1.0f, step);
    EXPECT_FLOAT_EQ(0.9f, smooth);
}

void exportAndLoadContent(LoadedExportedPack* loaded,
                          const VNX::ProjectData& project,
                          const QString& exportedPackDir,
                          const QString& actionName)
{
    ASSERT_TRUE(QDir().mkpath(exportedPackDir));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project, exportedPackDir));
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(exportedPackDir, actionName, loaded));
}

struct GoldJournalRecord {
    uint16_t type = 0;
    std::vector<uint8_t> payload;
};

std::vector<GoldJournalRecord> readGoldJournal(const QString& path)
{
    QFile file(path);
    EXPECT_TRUE(file.open(QIODevice::ReadOnly)) << path.toStdString();
    const QByteArray bytes = file.readAll();
    std::vector<GoldJournalRecord> records;
    if (static_cast<size_t>(bytes.size()) < harken::kTraceFixedHeaderBytes)
        return records;
    uint32_t headerBytes = 0;
    std::memcpy(&headerBytes, bytes.constData() + 8, sizeof(headerBytes));
    size_t offset = headerBytes;
    while (offset + harken::kTraceRecordHeaderBytes <=
           static_cast<size_t>(bytes.size())) {
        uint32_t payloadBytes = 0;
        uint16_t type = 0;
        std::memcpy(&payloadBytes, bytes.constData() + static_cast<qsizetype>(offset),
                    sizeof(payloadBytes));
        std::memcpy(&type, bytes.constData() + static_cast<qsizetype>(offset + 4),
                    sizeof(type));
        offset += harken::kTraceRecordHeaderBytes;
        if (payloadBytes > static_cast<size_t>(bytes.size()) - offset) {
            records.clear();
            return records;
        }
        GoldJournalRecord record;
        record.type = type;
        record.payload.assign(
            bytes.constData() + static_cast<qsizetype>(offset),
            bytes.constData() + static_cast<qsizetype>(offset + payloadBytes));
        records.push_back(std::move(record));
        offset += payloadBytes;
    }
    return records;
}

uint16_t goldJournalCallId(const GoldJournalRecord& record)
{
    uint16_t callId = 0;
    if (record.type == harken::kTraceRecordApiCall &&
        record.payload.size() >= sizeof(callId)) {
        std::memcpy(&callId, record.payload.data(), sizeof(callId));
    }
    return callId;
}

struct GoldHostRun {
    int exitCode = -1;
    std::string output;
};

GoldHostRun runHarkenHost(const std::string& args)
{
    GoldHostRun run;
    const std::string cmd = std::string(HARKEN_HOST_PATH) + " " + args + " 2>&1";
    FILE* pipe = _popen(cmd.c_str(), "rt");
    if (pipe == nullptr)
        return run;
    char chunk[512];
    while (std::fgets(chunk, sizeof chunk, pipe) != nullptr)
        run.output += chunk;
    run.exitCode = _pclose(pipe);
    return run;
}

void runGoldLifecycleWithoutTrace(VNX::ProjectData& project,
                                  const QString& projectRoot,
                                  const QString& baselineDir,
                                  const QString& nextDir)
{
    ASSERT_NO_FATAL_FAILURE(exportProjectPackSuccessor(project, projectRoot, nextDir));

    LoadedExportedPack loaded;
    ASSERT_EQ(VN_OK, loaded.session.InitResult());
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        baselineDir, QStringLiteral("ExportedPackVerticalContainer"), &loaded));
    const VnActionFiringHandle firing =
        postGoldAction(harkentest::StudioObjectId(kVerticalContainerActionObjectId + kStableIdBias));
    ASSERT_NE(0u, firing);
    harkentest::Capture before;
    ASSERT_NO_FATAL_FAILURE(pumpUntilAudible(loaded.session.Engine(), &before));

    VnPackLoadDesc nextDesc{};
    ASSERT_NO_FATAL_FAILURE(makeRootReplaceDesc(
        &loaded, nextDir, QStringLiteral("ExportedPackVerticalContainer"), &nextDesc));
    ASSERT_EQ(VN_OK, Vn_ReplacePack(loaded.contentHandle,
                                    reinterpret_cast<const VnPackDesc*>(&nextDesc)));
    harkentest::Capture after;
    ASSERT_NO_FATAL_FAILURE(pumpUntilAudible(loaded.session.Engine(), &after));
    EXPECT_NEAR(kToneAmplitude, harkentest::Peak(after.left), 0.05f);
    EXPECT_GT(harkentest::Peak(after.left), harkentest::kSilenceFloor);
    EXPECT_LT(harkentest::Peak(after.left), 0.50f);
    ASSERT_NO_FATAL_FAILURE(drainPlayableAndFiring(loaded.session.Engine(), firing));
    ASSERT_EQ(VN_OK, Vn_UnloadPack(loaded.contentHandle));
}

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
    ASSERT_EQ(kDirectPieceSourceId,
              registerToneAsset(projectRoot, QStringLiteral("direct-piece.wav"),
                                kDirectPieceSourceId));
    ASSERT_EQ(kQueuePieceSourceId,
              registerToneAsset(projectRoot, QStringLiteral("queue-piece.wav"),
                                kQueuePieceSourceId));
    ASSERT_EQ(kQueuePieceSourceBId,
              registerToneAsset(projectRoot, QStringLiteral("queue-piece-b.wav"),
                                kQueuePieceSourceBId, -kToneAmplitude));
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
        qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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

TEST_F(AuthoredPackLifecycle, TargetPlatformZeroOutputOverrideReachesCanonicalAudio)
{
    ASSERT_FALSE(project_.busses.empty());
    auto master = project_.busses.front().rootItem;
    ASSERT_NE(master, nullptr);
    auto* mix = master->comp<VNX::VMixNode>();
    ASSERT_NE(mix, nullptr);
    mix->outputBusVolume = -6.0f;
    VNX::ProjectItem* routedSound = nullptr;
    for (auto* binding : project_.allFileBindings()) {
        if (!binding || !binding->rootItem)
            continue;
        const auto visit = [&](auto&& self, VNX::ProjectItem* item) -> void {
            if (item->objectID == kContentSoundObjectId)
                routedSound = item;
            for (const auto& child : item->children)
                self(self, child.get());
        };
        visit(visit, binding->rootItem.get());
    }
    ASSERT_NE(routedSound, nullptr);
    routedSound->compOrCreate<VNX::VMixNode>()->outputBusId = kMasterBusObjectId;
    const auto locator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*master);
    ASSERT_TRUE(locator.has_value());
    const auto property = static_cast<uint8_t>(
        harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume);
    const auto target = HarkenTest::PlatformIds::Windows;
    const VnBusId busId = harkentest::ToRuntime(master->runtimeId());
    ASSERT_NE(0u, busId.high);
    const auto outputZero = QDir(dir_.path()).filePath(QStringLiteral("output-zero"));
    const auto outputMinusSix = QDir(dir_.path()).filePath(QStringLiteral("output-minus-six"));

    const auto exportWithOverride = [&](double value, const QString& outputDir) {
        ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
            *master, *locator, property, QJsonValue(value), target));
        ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, outputDir));
        QFile file(QDir(outputDir).filePath(QStringLiteral("Init.bytes")));
        ASSERT_TRUE(file.open(QIODevice::ReadOnly));
        const QByteArray bytes = file.readAll();
        harken::PackCanonicalImage image;
        ASSERT_EQ(VN_OK, harken::ParsePackCanonicalImage(
            reinterpret_cast<const uint8_t*>(bytes.constData()),
            static_cast<uint64_t>(bytes.size()), &image));
        ASSERT_TRUE(image.hasBusDefinitionSnapshot);
        EXPECT_EQ(2u, image.busDefinitionSnapshotHeader.schemaVersion);
        EXPECT_EQ(4u, image.busDefinitionSnapshotHeader.publicationCapabilityVintage);
        const auto authored = std::find_if(image.busDefinitionBusesV2.begin(),
            image.busDefinitionBusesV2.end(), [&](const auto& row) {
                return row.busId == busId;
            });
        ASSERT_NE(authored, image.busDefinitionBusesV2.end());
        const auto outputRoot = harkentest::ToRuntime(harkenstudio::kMasterBusObjectId);
        EXPECT_NE(busId, outputRoot);
        EXPECT_EQ(outputRoot, authored->parentId);
        const auto root = std::find_if(image.busDefinitionBusesV2.begin(),
            image.busDefinitionBusesV2.end(), [&](const auto& row) {
                return row.busId == outputRoot;
            });
        ASSERT_NE(root, image.busDefinitionBusesV2.end());
        EXPECT_FLOAT_EQ(0.0f, root->outputBusVolumeDb);
        EXPECT_FLOAT_EQ(static_cast<float>(value), authored->outputBusVolumeDb);
        const auto legacy = std::find_if(image.buses.begin(), image.buses.end(),
            [&](const auto& row) { return row.busId == busId; });
        ASSERT_NE(legacy, image.buses.end());
        EXPECT_FLOAT_EQ(static_cast<float>(value), legacy->outputBusVolume);
    };
    ASSERT_NO_FATAL_FAILURE(exportWithOverride(0.0, outputZero));
    ASSERT_NO_FATAL_FAILURE(exportWithOverride(-6.0, outputMinusSix));

    struct FreshBlock {
        std::uint64_t before = 0;
        std::uint64_t rendered = 0;
        std::uint32_t ordinal = 0;
        std::vector<float> samples;
    };
    const auto playFreshBlock = [&](const QString& outputDir, float expectedDb,
                                    FreshBlock* result) {
        ASSERT_NE(nullptr, result);
        LoadedExportedPack loaded;
        ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(outputDir,
            QStringLiteral("ExportedPackImmediateSound"), &loaded));
        ASSERT_NE(nullptr, loaded.session.Engine());
        const auto index = loaded.session.Engine()->buses.FindIndex(busId);
        ASSERT_NE(harken::kInvalidBusIndex, index);
        EXPECT_FLOAT_EQ(expectedDb, loaded.session.Engine()->busTargets.SlotValue(
            index, harken::kBusTargetBusOutputVolume,
            harken::BusTargetStore::kSlotAuthoredIndex));
        auto* engine = loaded.session.Engine();
        VnPostActionDesc post{};
        post.structSize = sizeof(post);
        post.actionId = harkentest::ToRuntime(harkentest::StudioObjectId(
            kImmediateSoundActionObjectId + kStableIdBias));
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&post.emitter));
        VnActionFiringHandle firing = 0;
        ASSERT_EQ(VN_OK, Vn_PostAction(&post, &firing));
        ASSERT_NE(0u, firing);
        ASSERT_NO_FATAL_FAILURE(expectActivePlayable(firing, engine, 1));
        result->before = engine->mixer.BlocksRendered();
        const auto sampleCount = static_cast<std::size_t>(engine->nullBackend.BlockFrames())
            * engine->nullBackend.Channels();
        for (std::uint32_t ordinal = 1; ordinal <= 16; ++ordinal) {
            ASSERT_EQ(VN_OK, Vn_Update());
            engine->nullBackend.Pump(1);
            const auto rendered = engine->mixer.BlocksRendered();
            ASSERT_EQ(result->before + ordinal, rendered);
            const float* block = engine->nullBackend.LastBlock();
            if (std::any_of(block, block + sampleCount, [](float sample) {
                    return std::abs(sample) > harkentest::kSilenceFloor;
                })) {
                result->rendered = rendered;
                result->ordinal = ordinal;
                result->samples.assign(block, block + sampleCount);
                break;
            }
        }
        ASSERT_FALSE(result->samples.empty());
        const auto* bus = loaded.session.Engine()->buses.BusState(index);
        ASSERT_NE(nullptr, bus);
        EXPECT_NEAR(bus->outputGain.target, std::pow(10.0f, expectedDb / 20.0f), 1e-5f);
        const VnResult released = Vn_ReleaseActionFiring(firing);
        EXPECT_TRUE(released == VN_OK || released == VN_IN_USE);
    };
    FreshBlock zero;
    FreshBlock minusSix;
    ASSERT_NO_FATAL_FAILURE(playFreshBlock(outputZero, 0.0f, &zero));
    ASSERT_NO_FATAL_FAILURE(playFreshBlock(outputMinusSix, -6.0f, &minusSix));
    ASSERT_EQ(zero.ordinal, minusSix.ordinal);
    ASSERT_EQ(zero.samples.size(), minusSix.samples.size());
    const auto audible = std::find_if(zero.samples.begin(), zero.samples.end(), [](float sample) {
        return std::abs(sample) > 0.05f && std::abs(sample) < 0.8f;
    });
    ASSERT_NE(audible, zero.samples.end());
    const auto sampleIndex = static_cast<std::size_t>(audible - zero.samples.begin());
    ASSERT_GT(std::abs(minusSix.samples[sampleIndex]), harkentest::kSilenceFloor);
    EXPECT_NEAR(minusSix.samples[sampleIndex] / *audible,
        std::pow(10.0f, -6.0f / 20.0f), 1e-4f);
}

TEST_F(AuthoredPackLifecycle, SavedWindowsProjectPreviewAndPackRenderSameBusVolume)
{
    if (!qApp) {
        static int argc = 1;
        static char arg0[] = "harken_editor_tests";
        static char* argv[] = {arg0, nullptr};
        static QApplication app(argc, argv);
    }
    auto& editor = VNX::AudioEngine::instance();
    auto& projects = VNX::ProjectManager::instance();
    auto& platforms = VNX::PlatformContextService::instance();
    const auto previousPlatform = platforms.context();
    const auto cleanup = qScopeGuard([&]() {
        editor.shutdown();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        if (projects.currentProject() != nullptr)
            projects.closeProject();
        platforms.setSelected(previousPlatform.selected, previousPlatform.primary,
                              previousPlatform.all);
    });
    editor.shutdown();
    if (projects.currentProject() != nullptr)
        ASSERT_TRUE(projects.closeProject());
    const auto target = HarkenTest::PlatformIds::Windows;
    platforms.setSelected({target}, target, false);
    const QString projectPath = QDir(dir_.path()).filePath(QStringLiteral("ExportedPack.hsp"));
    project_.projectPath = projectPath;
    auto* master = project_.busses.front().rootItem.get();
    ASSERT_NE(nullptr, master);
    auto* mix = master->comp<VNX::VMixNode>();
    ASSERT_NE(nullptr, mix);
    mix->volume = -3.0f;
    mix->makeUpGain = 2.0f;
    mix->outputBusVolume = -5.0f;
    const auto locator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*master);
    ASSERT_TRUE(locator.has_value());
    using Property = harkenplayableproperties::PlayablePropertyIdV1;
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *master, *locator, static_cast<uint8_t>(Property::Volume),
        QJsonValue(-3.0), std::nullopt));
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *master, *locator, static_cast<uint8_t>(Property::MakeUpGain),
        QJsonValue(2.0), std::nullopt));
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *master, *locator, static_cast<uint8_t>(Property::OutputBusVolume),
        QJsonValue(-5.0), std::nullopt));
    const auto property = static_cast<uint8_t>(
        Property::OutputBusVolume);
    VNX::ProjectItem* authoredSound = nullptr;
    for (auto* binding : project_.allFileBindings()) {
        if (!binding || !binding->rootItem)
            continue;
        const auto visit = [&](auto&& self, VNX::ProjectItem* item) -> void {
            if (item->objectID == kContentSoundObjectId)
                authoredSound = item;
            for (const auto& child : item->children)
                self(self, child.get());
        };
        visit(visit, binding->rootItem.get());
    }
    ASSERT_NE(nullptr, authoredSound);
    const QUuid soundGuid = authoredSound->guid;
    authoredSound->compOrCreate<VNX::VMixNode>()->outputBusId = kMasterBusObjectId;
    auto& layer = project_.layers.front().rootItem;
    const auto soundChild = std::find_if(layer->children.begin(), layer->children.end(),
        [](const auto& child) { return child->objectID == kContentSoundObjectId; });
    ASSERT_NE(soundChild, layer->children.end());
    const auto onlySound = *soundChild;
    layer->children = {onlySound};
    onlySound->parent = layer.get();
    const auto& actionChildren = project_.actions.front().rootItem->children;
    const auto actionChild = std::find_if(actionChildren.begin(), actionChildren.end(),
        [](const auto& child) { return child->objectID == kImmediateSoundActionObjectId; });
    ASSERT_NE(actionChild, actionChildren.end());
    VNX::ProjectFileBinding actionBinding;
    actionBinding.parentFolder = QStringLiteral("Actions");
    actionBinding.rootItem = *actionChild;
    actionBinding.rootItem->parent = nullptr;
    project_.actions = {actionBinding};
    project_.variables.clear();
    project_.attenuationItems.clear();
    project_.nextObjectID = 1000;
    QList<VNX::AssetInfo> sourceAssets;
    for (const auto& source : project_.audioSources) {
        const auto* asset = VNX::AssetRegistry::instance().getAsset(source.sourceID);
        ASSERT_NE(nullptr, asset);
        sourceAssets.append(*asset);
    }

    const auto capture = [&](int zeroOverride, const QString& outputDir,
                             float* previewPeak, float* packPeak) {
        ASSERT_NE(nullptr, previewPeak);
        ASSERT_NE(nullptr, packPeak);
        if (zeroOverride != 0) {
            ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
                *master, *locator, zeroOverride == 1 ? property
                    : static_cast<uint8_t>(Property::Volume), QJsonValue(0.0), target));
        }
        VNX::PreparedProject prepared;
        prepared.project = std::make_unique<VNX::ProjectData>(project_);
        prepared.folderSaveState = VNX::FolderSaveState::capture(*prepared.project, &prepared.error);
        ASSERT_NE(prepared.folderSaveState, nullptr) << prepared.error.toStdString();
        prepared.assets = sourceAssets;
        ASSERT_TRUE(projects.commitPreparedProject(std::move(prepared)));
        ASSERT_TRUE(projects.saveProject());
        ASSERT_TRUE(projects.closeProject());
        HarkenEditor::Bridge::EngineSessionConfig config;
        config.sampleRate = 48000;
        config.bufferSize = 256;
        config.maxVoices = 8;
        config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
        ASSERT_TRUE(editor.initialize(config));
        ASSERT_TRUE(editor.startOutput());
        QString loadError;
        ASSERT_TRUE(projects.loadProject(projectPath, false, &loadError))
            << loadError.toStdString();
        VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
        auto& coordinator = editor.runtimeSyncCoordinator();
        bool ready = false;
        for (int attempt = 0; attempt < 128 && !ready; ++attempt) {
            ASSERT_TRUE(QMetaObject::invokeMethod(
                &editor, "onRuntimeServiceTimer", Qt::DirectConnection));
            ASSERT_EQ(VN_OK, Vn_Update());
            auto* runtime = harken::detail::ActiveEngine();
            ASSERT_NE(nullptr, runtime);
            runtime->nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_Update());
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            const auto installed = coordinator.installedWorld();
            ready = installed && installed->worldGeneration != 0
                && coordinator.barrier(coordinator.currentAuthoringRevision()).state
                    == VNX::RuntimeSyncCoordinator::RevisionBarrierState::Ready;
        }
        ASSERT_TRUE(ready);
        VNX::ProjectItem* sound = nullptr;
        for (auto* binding : projects.currentProject()->allFileBindings()) {
            if (!binding || !binding->rootItem)
                continue;
            const auto visit = [&](auto&& self, VNX::ProjectItem* item) -> void {
                if (item->guid == soundGuid)
                    sound = item;
                for (const auto& child : item->children)
                    self(self, child.get());
            };
            visit(visit, binding->rootItem.get());
        }
        ASSERT_NE(nullptr, sound);
        auto* loadedMaster = projects.findItemByGuid(master->guid);
        ASSERT_NE(nullptr, loadedMaster);
        EXPECT_EQ(loadedMaster->objectID,
                  VNX::ProjectManager::resolveEffectiveOutputBusId(sound));
        const auto busIndex = harken::detail::ActiveEngine()->buses.FindIndex(
            harkentest::ToRuntime(loadedMaster->runtimeId()));
        ASSERT_NE(harken::kInvalidBusIndex, busIndex);
        std::printf("[SavedVolume] override=%d project=%s profile=Windows bus=%u mix=%f output=%f\n",
            zeroOverride, projectPath.toUtf8().constData(), loadedMaster->objectID,
            harken::detail::ActiveEngine()->busTargets.SlotValue(
                busIndex, harken::kBusTargetBusVolume,
                harken::BusTargetStore::kSlotAuthoredIndex),
            harken::detail::ActiveEngine()->busTargets.SlotValue(
                busIndex, harken::kBusTargetBusOutputVolume,
                harken::BusTargetStore::kSlotAuthoredIndex));
        const auto played = editor.playPreviewPlayable(0, sound->runtimeId());
        ASSERT_EQ(0, played.status);
        auto* previewRuntime = harken::detail::ActiveEngine();
        ASSERT_NE(nullptr, previewRuntime);
        const auto beforePreview = previewRuntime->mixer.BlocksRendered();
        for (std::uint32_t ordinal = 1; ordinal <= 8; ++ordinal) {
            ASSERT_EQ(VN_OK, Vn_Update());
            previewRuntime->nullBackend.Pump(1);
            ASSERT_EQ(beforePreview + ordinal, previewRuntime->mixer.BlocksRendered());
            *previewPeak = std::max(*previewPeak, harkentest::Peak(
                previewRuntime->nullBackend.LastBlock(),
                previewRuntime->nullBackend.BlockFrames()
                    * previewRuntime->nullBackend.Channels()));
        }
        ASSERT_GT(*previewPeak, harkentest::kSilenceFloor);
        ASSERT_NO_FATAL_FAILURE(exportProjectPack(*projects.currentProject(), outputDir));
        editor.shutdown();
        ASSERT_TRUE(projects.closeProject());

        LoadedExportedPack loaded;
        ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
            outputDir, QStringLiteral("ExportedPackImmediateSound"), &loaded));
        auto* packRuntime = loaded.session.Engine();
        ASSERT_NE(nullptr, packRuntime);
        VnPostActionDesc post{};
        post.structSize = sizeof(post);
        post.actionId = harkentest::ToRuntime(harkentest::StudioObjectId(
            kImmediateSoundActionObjectId + kStableIdBias));
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&post.emitter));
        VnActionFiringHandle firing = 0;
        ASSERT_EQ(VN_OK, Vn_PostAction(&post, &firing));
        ASSERT_NO_FATAL_FAILURE(expectActivePlayable(firing, packRuntime, 1));
        const auto beforePack = packRuntime->mixer.BlocksRendered();
        for (std::uint32_t ordinal = 1; ordinal <= 8; ++ordinal) {
            ASSERT_EQ(VN_OK, Vn_Update());
            packRuntime->nullBackend.Pump(1);
            ASSERT_EQ(beforePack + ordinal, packRuntime->mixer.BlocksRendered());
            *packPeak = std::max(*packPeak, harkentest::Peak(
                packRuntime->nullBackend.LastBlock(),
                packRuntime->nullBackend.BlockFrames()
                    * packRuntime->nullBackend.Channels()));
        }
        ASSERT_GT(*packPeak, harkentest::kSilenceFloor);
        std::printf("[SavedVolume] override=%d previewPeak=%f packPeak=%f\n",
            zeroOverride, *previewPeak, *packPeak);
        const auto released = Vn_ReleaseActionFiring(firing);
        EXPECT_TRUE(released == VN_OK || released == VN_IN_USE);
    };
    float basePreview = 0.0f, basePack = 0.0f;
    ASSERT_NO_FATAL_FAILURE(capture(0,
        QDir(dir_.path()).filePath(QStringLiteral("saved-volume-base")),
        &basePreview, &basePack));
    float zeroPreview = 0.0f, zeroPack = 0.0f;
    ASSERT_NO_FATAL_FAILURE(capture(1,
        QDir(dir_.path()).filePath(QStringLiteral("saved-volume-zero")),
        &zeroPreview, &zeroPack));
    float volumeZeroPreview = 0.0f, volumeZeroPack = 0.0f;
    ASSERT_NO_FATAL_FAILURE(capture(2,
        QDir(dir_.path()).filePath(QStringLiteral("saved-volume-property-zero")),
        &volumeZeroPreview, &volumeZeroPack));
    EXPECT_NEAR(volumeZeroPreview, kToneAmplitude * std::pow(10.0f, 2.0f / 20.0f), 0.003f);
    EXPECT_NEAR(volumeZeroPack, volumeZeroPreview, 0.003f);
    EXPECT_NEAR(basePreview, kToneAmplitude * std::pow(10.0f, -6.0f / 20.0f), 0.003f);
    EXPECT_NEAR(basePack, basePreview, 0.003f);
    EXPECT_NEAR(zeroPreview, kToneAmplitude * std::pow(10.0f, -1.0f / 20.0f), 0.003f);
    EXPECT_NEAR(zeroPack, zeroPreview, 0.003f);
}

TEST_F(AuthoredPackLifecycle, SavedWindowsProjectPreviewAndPackRenderSameBusSend)
{
    if (!qApp) {
        static int argc = 1;
        static char arg0[] = "harken_editor_tests";
        static char* argv[] = {arg0, nullptr};
        static QApplication app(argc, argv);
    }
    auto& editor = VNX::AudioEngine::instance();
    auto& projects = VNX::ProjectManager::instance();
    auto& platforms = VNX::PlatformContextService::instance();
    const auto previousPlatform = platforms.context();
    const auto cleanup = qScopeGuard([&]() {
        editor.shutdown();
        QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
        if (projects.currentProject() != nullptr) projects.closeProject();
        platforms.setSelected(previousPlatform.selected, previousPlatform.primary,
                              previousPlatform.all);
    });
    editor.shutdown();
    if (projects.currentProject() != nullptr) ASSERT_TRUE(projects.closeProject());
    const auto target = HarkenTest::PlatformIds::Windows;
    platforms.setSelected({target}, target, false);
    const QString projectPath = QDir(dir_.path()).filePath(QStringLiteral("Send.hsp"));
    project_.projectPath = projectPath;
    auto* master = project_.busses.front().rootItem.get();
    ASSERT_NE(nullptr, master);
    auto source = makeItem(QStringLiteral("SendSource"), VNX::ProjectItem::Type::Bus, 801);
    auto destination = makeItem(QStringLiteral("SendDestination"), VNX::ProjectItem::Type::Bus, 802);
    const QUuid sourceGuid = source->guid;
    const QUuid destinationGuid = destination->guid;
    addChild(project_.busses.front().rootItem, source);
    addChild(project_.busses.front().rootItem, destination);
    const auto locator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*source);
    ASSERT_TRUE(locator.has_value());
    const auto sendsProperty = static_cast<uint8_t>(
        harkenplayableproperties::PlayablePropertyIdV1::Sends);
    const auto setSend = [&](double db, std::optional<QUuid> platform) {
        return VNX::PlayablePropertyAuthoringV1::setOverride(
            *source, *locator, sendsProperty,
            QJsonArray{QJsonObject{{QStringLiteral("targetBusID"), 802},
                                   {QStringLiteral("sendLevel"), db}}}, platform);
    };
    ASSERT_TRUE(setSend(-6.0, std::nullopt));
    VNX::ProjectItem* sound = nullptr;
    for (auto* binding : project_.allFileBindings()) {
        if (!binding || !binding->rootItem) continue;
        const auto visit = [&](auto&& self, VNX::ProjectItem* item) -> void {
            if (item->objectID == kContentSoundObjectId) sound = item;
            for (const auto& child : item->children) self(self, child.get());
        };
        visit(visit, binding->rootItem.get());
    }
    ASSERT_NE(nullptr, sound);
    const auto soundLocator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*sound);
    ASSERT_TRUE(soundLocator.has_value());
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(*sound, *soundLocator,
        static_cast<uint8_t>(harkenplayableproperties::PlayablePropertyIdV1::LoopCount),
        QJsonValue(0), std::nullopt));
    const QUuid soundGuid = sound->guid;
    sound->compOrCreate<VNX::VMixNode>()->outputBusId = 801;
    sound->fieldGroups.mainOutput.shared = {VNX::FieldGroupMode::Local,
        VNX::MainOutputFieldGroup{VNX::toFieldGroupGuid(sourceGuid), 0.0f}};
    auto& layer = project_.layers.front().rootItem;
    const auto soundChild = std::find_if(layer->children.begin(), layer->children.end(),
        [](const auto& child) { return child->objectID == kContentSoundObjectId; });
    ASSERT_NE(soundChild, layer->children.end());
    const auto onlySound = *soundChild;
    layer->children = {onlySound};
    onlySound->parent = layer.get();
    const auto& actionChildren = project_.actions.front().rootItem->children;
    const auto actionChild = std::find_if(actionChildren.begin(), actionChildren.end(),
        [](const auto& child) { return child->objectID == kImmediateSoundActionObjectId; });
    ASSERT_NE(actionChild, actionChildren.end());
    VNX::ProjectFileBinding actionBinding;
    actionBinding.parentFolder = QStringLiteral("Actions");
    actionBinding.rootItem = *actionChild;
    actionBinding.rootItem->parent = nullptr;
    project_.actions = {actionBinding};
    project_.variables.clear();
    project_.attenuationItems.clear();
    project_.nextObjectID = 1000;
    QList<VNX::AssetInfo> sourceAssets;
    for (const auto& audio : project_.audioSources) {
        const auto* asset = VNX::AssetRegistry::instance().getAsset(audio.sourceID);
        ASSERT_NE(nullptr, asset);
        sourceAssets.append(*asset);
    }
    const auto capture = [&](bool unity, const QString& outputDir,
                             float* previewPeak, float* packPeak) {
        if (unity) ASSERT_TRUE(setSend(0.0, target));
        VNX::PreparedProject prepared;
        prepared.project = std::make_unique<VNX::ProjectData>(project_);
        prepared.folderSaveState = VNX::FolderSaveState::capture(*prepared.project, &prepared.error);
        ASSERT_NE(prepared.folderSaveState, nullptr) << prepared.error.toStdString();
        prepared.assets = sourceAssets;
        ASSERT_TRUE(projects.commitPreparedProject(std::move(prepared)));
        ASSERT_TRUE(projects.saveProject());
        ASSERT_TRUE(projects.closeProject());
        HarkenEditor::Bridge::EngineSessionConfig config;
        config.sampleRate = 48000;
        config.bufferSize = 256;
        config.maxVoices = 8;
        config.deviceMode = HarkenEditor::Bridge::EngineDeviceMode::Null;
        ASSERT_TRUE(editor.initialize(config));
        ASSERT_TRUE(editor.startOutput());
        QString loadError;
        ASSERT_TRUE(projects.loadProject(projectPath, false, &loadError))
            << loadError.toStdString();
        VNX::ProjectManager::syncProjectToRuntime(projects.currentProject());
        auto& coordinator = editor.runtimeSyncCoordinator();
        bool ready = false;
        for (int attempt = 0; attempt < 128 && !ready; ++attempt) {
            ASSERT_TRUE(QMetaObject::invokeMethod(
                &editor, "onRuntimeServiceTimer", Qt::DirectConnection));
            ASSERT_EQ(VN_OK, Vn_Update());
            auto* runtime = harken::detail::ActiveEngine();
            ASSERT_NE(nullptr, runtime);
            runtime->nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_Update());
            QCoreApplication::processEvents(QEventLoop::AllEvents, 100);
            const auto installed = coordinator.installedWorld();
            ready = installed && installed->worldGeneration != 0 &&
                coordinator.barrier(coordinator.currentAuthoringRevision()).state ==
                    VNX::RuntimeSyncCoordinator::RevisionBarrierState::Ready;
        }
        ASSERT_TRUE(ready);
        auto* loadedSound = projects.findItemByGuid(soundGuid);
        auto* loadedSource = projects.findItemByGuid(sourceGuid);
        auto* loadedDestination = projects.findItemByGuid(destinationGuid);
        ASSERT_NE(nullptr, loadedSound);
        ASSERT_NE(nullptr, loadedSource);
        ASSERT_NE(nullptr, loadedDestination);
        EXPECT_EQ(loadedSource->objectID,
                  VNX::ProjectManager::resolveEffectiveOutputBusId(loadedSound));
        const auto sourceId = harkentest::ToRuntime(loadedSource->runtimeId());
        const auto destinationId = harkentest::ToRuntime(loadedDestination->runtimeId());
        auto* previewRuntime = harken::detail::ActiveEngine();
        ASSERT_NE(nullptr, previewRuntime);
        const auto sourceSlot = previewRuntime->buses.FindIndex(sourceId);
        const auto targetSlot = previewRuntime->buses.FindIndex(destinationId);
        ASSERT_NE(harken::kInvalidBusIndex, sourceSlot);
        ASSERT_NE(harken::kInvalidBusIndex, targetSlot);
        const auto sendSlots = previewRuntime->config.busSendSlotsPerBus;
        bool foundPreview = false;
        std::size_t previewSendFlat = 0;
        for (std::uint32_t slot = 0; slot < sendSlots; ++slot) {
            const auto flat = static_cast<std::size_t>(sourceSlot) * sendSlots + slot;
            if (previewRuntime->busSendAdj[flat] == targetSlot) {
                EXPECT_FLOAT_EQ(unity ? 0.0f : -6.0f,
                                previewRuntime->busSendLevelDb[flat]);
                previewSendFlat = flat;
                foundPreview = true;
            }
        }
        ASSERT_TRUE(foundPreview);
        ASSERT_EQ(0, editor.playPreviewPlayable(0, loadedSound->runtimeId()).status);
        const auto beforePreview = previewRuntime->mixer.BlocksRendered();
        for (std::uint32_t ordinal = 1; ordinal <= 8; ++ordinal) {
            ASSERT_EQ(VN_OK, Vn_Update());
            previewRuntime->nullBackend.Pump(1);
            ASSERT_EQ(beforePreview + ordinal, previewRuntime->mixer.BlocksRendered());
            const auto freshSamples = previewRuntime->nullBackend.BlockFrames() *
                previewRuntime->nullBackend.Channels();
            if (harkentest::Peak(previewRuntime->nullBackend.LastBlock(), freshSamples) > harkentest::kSilenceFloor) {
                const float expected = kToneAmplitude *
                    (1.0f + std::pow(10.0f, (unity ? 0.0f : -6.0f) / 20.0f));
                for (std::uint32_t sample = 0; sample < freshSamples; ++sample)
                    EXPECT_NEAR(expected, previewRuntime->nullBackend.LastBlock()[sample], 1e-4f)
                        << "new send first audible/steady ordinal=" << ordinal << " sample=" << sample;
            }
            *previewPeak = std::max(*previewPeak, harkentest::Peak(
                previewRuntime->nullBackend.LastBlock(),
                previewRuntime->nullBackend.BlockFrames() *
                    previewRuntime->nullBackend.Channels()));
        }
        ASSERT_GT(*previewPeak, harkentest::kSilenceFloor);
        ASSERT_NO_FATAL_FAILURE(exportProjectPack(*projects.currentProject(), outputDir));
        const QString changedDir = outputDir + QStringLiteral("-retained-minus-twelve");
        std::vector<float> previewRamp;
        const auto checkRamp = [&](harken::Engine* runtime, std::vector<float>* pcm,
                                   const auto& unchangedRetry, bool firstRendered = false) {
            const auto frames = runtime->nullBackend.BlockFrames();
            const auto channels = runtime->nullBackend.Channels();
            ASSERT_EQ(256u, frames);
            const auto firstBlock = runtime->mixer.BlocksRendered() - (firstRendered ? 1u : 0u);
            const float from = std::pow(10.0f, -6.0f / 20.0f);
            const float to = std::pow(10.0f, -12.0f / 20.0f);
            for (std::uint32_t block = 0; block < 5; ++block) {
                const auto beforeRetry = runtime->mixer.BlocksRendered();
                if (block == 1) ASSERT_NO_FATAL_FAILURE(unchangedRetry());
                ASSERT_LE(runtime->mixer.BlocksRendered() - beforeRetry, 1u);
                if (!(block == 0 && firstRendered)
                    && runtime->mixer.BlocksRendered() == beforeRetry) {
                    ASSERT_EQ(VN_OK, Vn_Update());
                    runtime->nullBackend.Pump(1);
                }
                ASSERT_EQ(firstBlock + block + 1, runtime->mixer.BlocksRendered());
                const float* output = runtime->nullBackend.LastBlock();
                for (std::uint32_t frame = 0; frame < frames; ++frame) {
                    const auto ordinal = block * frames + frame + 1;
                    const float gain = ordinal <= 1024
                        ? from + (to - from) * static_cast<float>(ordinal) / 1024.0f : to;
                    for (std::uint32_t channel = 0; channel < channels; ++channel) {
                        const auto offset = frame * channels + channel;
                        EXPECT_NEAR(kToneAmplitude * (1.0f + gain), output[offset], 1e-4f)
                            << "send ramp frame=" << ordinal << " channel=" << channel;
                    }
                }
                pcm->insert(pcm->end(), output, output + frames * channels);
            }
        };
        if (!unity) {
            const auto oldGeneration = previewRuntime->buses.GenerationAt(sourceSlot);
            const auto loadedLocator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*loadedSource);
            ASSERT_TRUE(loadedLocator.has_value());
            ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
                *loadedSource, *loadedLocator, sendsProperty,
                QJsonArray{QJsonObject{{QStringLiteral("targetBusID"), static_cast<int>(loadedDestination->objectID)},
                                       {QStringLiteral("sendLevel"), -12.0}}}, target));
            ASSERT_TRUE(projects.saveProject());
            const auto editedSend = VNX::PlayablePropertyAuthoringV1::readProperty(
                *loadedSource, *loadedLocator, sendsProperty, target);
            ASSERT_TRUE(editedSend.has_value());
            ASSERT_EQ(-12.0, editedSend->value.toArray().at(0).toObject().value(QStringLiteral("sendLevel")).toDouble());
            ASSERT_NO_FATAL_FAILURE(exportProjectPack(*projects.currentProject(), changedDir));
            ASSERT_EQ(VNX::RuntimeSyncCoordinator::AllocationState::Allocated,
                coordinator.markCurrentWorldMutation().state);
            coordinator.flushPendingMutationNow();
            bool applied = false;
            bool firstRendered = false;
            for (int attempt = 0; attempt < 128 && !applied; ++attempt) {
                const auto beforeService = previewRuntime->mixer.BlocksRendered();
                ASSERT_TRUE(QMetaObject::invokeMethod(&editor, "onRuntimeServiceTimer", Qt::DirectConnection));
                QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
                coordinator.flushPendingMutationNow();
                applied = previewRuntime->busSendLevelDb[previewSendFlat] == -12.0f;
                if (applied) {
                    const auto consumed = previewRuntime->mixer.BlocksRendered() - beforeService;
                    ASSERT_LE(consumed, 1u);
                    firstRendered = consumed == 1;
                } else {
                    ASSERT_FALSE(previewRuntime->suspended);
                    const auto beforeOldPump = previewRuntime->mixer.BlocksRendered();
                    previewRuntime->nullBackend.Pump(1);
                    ASSERT_EQ(beforeOldPump + 1, previewRuntime->mixer.BlocksRendered());
                    ASSERT_EQ(VN_OK, Vn_Update());
                }
            }
            RecordProperty("publication_consumers", harken::HasCurrentWorldPublicationConsumers(*previewRuntime) ? 1 : 0);
            RecordProperty("draining_transport", previewRuntime->variables.DrainingTransport() ? 1 : 0);
            RecordProperty("draining_world", previewRuntime->variables.DrainingWorld() ? 1 : 0);
            RecordProperty("unreclaimed_world_rows", previewRuntime->variables.HasUnreclaimedLiveWorldRows() ? 1 : 0);
            RecordProperty("predecessor_direct_publication", harken::HasPredecessorDirectWorldPublication(*previewRuntime) ? 1 : 0);
            HarkenEditor::Bridge::CurrentWorldSoundPreparation probeSounds;
            QString probeError;
            ASSERT_TRUE(VNX::ProjectManager::prepareRuntimeSounds(*projects.currentProject(), probeSounds, &probeError));
            VNX::CurrentWorldBuildOptions probeOptions;
            probeOptions.isolateUnavailable = true;
            probeOptions.soundContentRevisions = &probeSounds.contentRevisions;
            auto probeRequest = VNX::buildCurrentWorldSyncRequest(*projects.currentProject(), target,
                &probeError, nullptr, &probeOptions, coordinator.currentAuthoringRevision());
            ASSERT_NE(nullptr, probeRequest) << probeError.toStdString();
            auto probeCompiled = HarkenEditor::Bridge::compileCurrentWorldRequest(*probeRequest);
            ASSERT_TRUE(probeCompiled.ok());
            RecordProperty("requested_world_fingerprint", std::to_string(probeCompiled.world->View().identity.runtimeSemanticFingerprint));
            RecordProperty("live_world_fingerprint", std::to_string(previewRuntime->variables.CurrentWorldInstallView().identity.runtimeSemanticFingerprint));
            ASSERT_TRUE(applied) << "flat=" << previewSendFlat
                << " value=" << previewRuntime->busSendLevelDb[previewSendFlat]
                << " state=" << static_cast<int>(editor.busTopologyState())
                << " barrier=" << static_cast<int>(coordinator.barrier(coordinator.currentAuthoringRevision()).state)
                << " failure=" << static_cast<int>(coordinator.barrier(coordinator.currentAuthoringRevision()).failure)
                << " tokenRevision=" << previewRuntime->lastPairedProjectGenerationToken.authoringRevision
                << " wantedRevision=" << coordinator.currentAuthoringRevision()
                << " outstanding=" << coordinator.hasOutstandingSubmission()
                << " preparedPlans=" << previewRuntime->preparedTopologyPlans.InUse()
                << " livePlans=" << previewRuntime->topologyPlans.InUse()
                << " admissionOpen=" << harken::CurrentWorldBusAdmissionIsOpenV1(
                    *previewRuntime, previewRuntime->variables.CurrentWorldGeneration());
            EXPECT_EQ(sourceSlot, previewRuntime->buses.FindIndex(sourceId));
            EXPECT_EQ(oldGeneration, previewRuntime->buses.GenerationAt(sourceSlot));
            ASSERT_NO_FATAL_FAILURE(checkRamp(previewRuntime, &previewRamp, [&]() {
                ASSERT_EQ(VNX::RuntimeSyncCoordinator::AllocationState::Allocated,
                    coordinator.markCurrentWorldMutation().state);
                coordinator.flushPendingMutationNow();
                ASSERT_TRUE(QMetaObject::invokeMethod(&editor, "onRuntimeServiceTimer", Qt::DirectConnection));
            }, firstRendered));
            QElapsedTimer publicationTimeout;
            publicationTimeout.start();
            bool publicationReady = false;
            while (publicationTimeout.elapsed() < 5000) {
                ASSERT_TRUE(QMetaObject::invokeMethod(&editor, "onRuntimeServiceTimer", Qt::DirectConnection));
                ASSERT_EQ(VN_OK, Vn_Update());
                previewRuntime->nullBackend.Pump(1);
                ASSERT_EQ(VN_OK, Vn_Update());
                QCoreApplication::processEvents(QEventLoop::AllEvents, 1);
                const auto barrier = coordinator.barrier(coordinator.currentAuthoringRevision());
                ASSERT_TRUE(barrier.state == VNX::RuntimeSyncCoordinator::RevisionBarrierState::Pending
                    || barrier.state == VNX::RuntimeSyncCoordinator::RevisionBarrierState::Ready)
                    << "state=" << static_cast<int>(barrier.state)
                    << " failure=" << static_cast<int>(barrier.failure);
                publicationReady = barrier.state == VNX::RuntimeSyncCoordinator::RevisionBarrierState::Ready
                    && !coordinator.hasOutstandingSubmission();
                if (publicationReady) break;
                QThread::msleep(1);
            }
            ASSERT_TRUE(publicationReady)
                << "revision=" << coordinator.currentAuthoringRevision()
                << " outstanding=" << coordinator.hasOutstandingSubmission();
            EXPECT_EQ(HarkenEditor::Bridge::BusTopologyState::Ready, editor.busTopologyState());
            EXPECT_TRUE(editor.getRuntimeBusIds().contains(loadedSource->runtimeId()));
            EXPECT_FLOAT_EQ(-12.0f, previewRuntime->busSendLevelDb[previewSendFlat]);
            const auto ownerSlot = std::find_if(previewRuntime->busTopologyOwners.begin(),
                previewRuntime->busTopologyOwners.end(), [&](const auto& owner) {
                    return owner.live && owner.ownerId == previewRuntime->lastPairedProjectGenerationToken.projectSessionId;
                });
            ASSERT_NE(previewRuntime->busTopologyOwners.end(), ownerSlot);
            const auto ownerBefore = *ownerSlot;
            const auto tokenBefore = previewRuntime->lastPairedProjectGenerationToken;
            const auto busFingerprintBefore = previewRuntime->lastPairedPublishedBusFingerprint;
            const auto compileFingerprintBefore = previewRuntime->lastPairedCompileTargetProfileFingerprint;
            const auto acksBefore = previewRuntime->projectGenerationInstallAckSideTable;
            const auto confirmedIdsBefore = editor.getRuntimeBusIds();
            const auto confirmedVolumeBefore = editor.getBusVolumeDbById(loadedSource->runtimeId());
            const auto worldBefore = previewRuntime->variables.CurrentWorldGeneration();
            const auto count = previewRuntime->buses.BusCount();
            const auto activeCountBeforeFaults = previewRuntime->buses.ActiveBusCount();
            std::vector<VnBusId> ids(count);
            std::vector<std::uint32_t> generations(count), parents(count);
            std::vector<std::uint8_t> alive(count, 1), apply(count * sendSlots, 0);
            std::vector<float> volumes(count), outputVolumes(count), neutral(count, 0.0f);
            std::vector<float> levels(count * sendSlots);
            std::vector<float> slotsBefore;
            for (std::uint32_t bus = 0; bus < count; ++bus) {
                ids[bus] = previewRuntime->buses.IdAt(bus);
                alive[bus] = previewRuntime->buses.AliveAt(bus);
                generations[bus] = previewRuntime->buses.GenerationAt(bus);
                parents[bus] = previewRuntime->buses.ParentOf(bus);
                volumes[bus] = previewRuntime->busTargets.SlotValue(bus, harken::kBusTargetBusVolume,
                    harken::BusTargetStore::kSlotAuthoredIndex);
                outputVolumes[bus] = previewRuntime->busTargets.SlotValue(bus, harken::kBusTargetBusOutputVolume,
                    harken::BusTargetStore::kSlotAuthoredIndex);
                for (auto target : {harken::kBusTargetBusVolume, harken::kBusTargetBusOutputVolume}) {
                    for (std::uint32_t slot = 0; slot < harken::BusTargetStore::kSlotCountPerTarget; ++slot)
                        slotsBefore.push_back(previewRuntime->busTargets.SlotValue(bus, target, slot));
                    slotsBefore.push_back(previewRuntime->busTargets.Reduce(bus, target));
                }
            }
            const auto sendsBefore = previewRuntime->busSendAdj;
            const auto sendLevelsBefore = previewRuntime->busSendLevelDb;
            for (std::size_t edge = 0; edge < levels.size(); ++edge)
                levels[edge] = std::pow(10.0f, sendLevelsBefore[edge] / 20.0f);
            harken::ProspectiveTopologyView prospective{};
            prospective.busCount = count; prospective.busIds = ids.data();
            prospective.generations = generations.data(); prospective.parents = parents.data();
            prospective.alive = alive.data(); prospective.sendTargets = sendsBefore.data();
            prospective.sendLevels = levels.data(); prospective.sendLevelApply = apply.data();
            prospective.sendLevelsDb = sendLevelsBefore.data();
            harken::ProjectGenerationInstallViewV1 candidate{};
            candidate.token = tokenBefore;
            candidate.token.authoringRevision += 100;
            candidate.token.submissionSerial += 100;
            candidate.publicationVintage = 4;
            candidate.projectContentGeneration = candidate.token.authoringRevision;
            candidate.publishedBusFingerprint = busFingerprintBefore + 1;
            candidate.compileTargetProfileFingerprint = compileFingerprintBefore;
            candidate.ownerId = ownerBefore.ownerId;
            candidate.ownerBaseRevision = ownerBefore.currentRevision;
            candidate.ownerRevision = ownerBefore.currentRevision + 1;
            candidate.compiledWorld = previewRuntime->variables.CurrentWorldInstallView();
            candidate.prospective = &prospective; candidate.authoredBusCount = count;
            candidate.authoredBusIds = ids.data(); candidate.authoredBusVolumeDb = volumes.data();
            candidate.authoredBusMakeUpGainDb = neutral.data(); candidate.authoredBusOutputVolumeDb = outputVolumes.data();
            const auto commandsBeforeFaults = previewRuntime->commands.PublishedHead();
            const auto assertUnchanged = [&]() {
                EXPECT_EQ(commandsBeforeFaults, previewRuntime->commands.PublishedHead());
                EXPECT_EQ(count, previewRuntime->buses.BusCount());
                EXPECT_EQ(activeCountBeforeFaults, previewRuntime->buses.ActiveBusCount());
                EXPECT_EQ(ownerBefore.live, ownerSlot->live);
                EXPECT_EQ(ownerBefore.ownerId, ownerSlot->ownerId);
                EXPECT_EQ(ownerBefore.predecessorRevision, ownerSlot->predecessorRevision);
                EXPECT_EQ(0, std::memcmp(&tokenBefore, &previewRuntime->lastPairedProjectGenerationToken, sizeof(tokenBefore)));
                EXPECT_EQ(busFingerprintBefore, previewRuntime->lastPairedPublishedBusFingerprint);
                EXPECT_EQ(compileFingerprintBefore, previewRuntime->lastPairedCompileTargetProfileFingerprint);
                EXPECT_EQ(ownerBefore.currentRevision, ownerSlot->currentRevision);
                EXPECT_EQ(ownerBefore.digestLow, ownerSlot->digestLow);
                EXPECT_EQ(ownerBefore.digestHigh, ownerSlot->digestHigh);
                EXPECT_EQ(worldBefore, previewRuntime->variables.CurrentWorldGeneration());
                ASSERT_EQ(acksBefore.size(), previewRuntime->projectGenerationInstallAckSideTable.size());
                EXPECT_EQ(0, std::memcmp(acksBefore.data(), previewRuntime->projectGenerationInstallAckSideTable.data(),
                    acksBefore.size() * sizeof(acksBefore.front())));
                EXPECT_EQ(confirmedIdsBefore, editor.getRuntimeBusIds());
                EXPECT_FLOAT_EQ(confirmedVolumeBefore, editor.getBusVolumeDbById(loadedSource->runtimeId()));
                EXPECT_EQ(sendsBefore, previewRuntime->busSendAdj);
                EXPECT_EQ(sendLevelsBefore, previewRuntime->busSendLevelDb);
                std::vector<float> now;
                for (std::uint32_t bus = 0; bus < count; ++bus) {
                    EXPECT_EQ(alive[bus], previewRuntime->buses.AliveAt(bus));
                    EXPECT_EQ(ids[bus], previewRuntime->buses.IdAt(bus));
                    EXPECT_EQ(generations[bus], previewRuntime->buses.GenerationAt(bus));
                    EXPECT_EQ(parents[bus], previewRuntime->buses.ParentOf(bus));
                    for (auto target : {harken::kBusTargetBusVolume, harken::kBusTargetBusOutputVolume}) {
                        for (std::uint32_t slot = 0; slot < harken::BusTargetStore::kSlotCountPerTarget; ++slot)
                            now.push_back(previewRuntime->busTargets.SlotValue(bus, target, slot));
                        now.push_back(previewRuntime->busTargets.Reduce(bus, target));
                    }
                }
                EXPECT_EQ(slotsBefore, now);
            };
            volumes[sourceSlot] = std::numeric_limits<float>::quiet_NaN();
            harken::PreparedProjectGenerationV1 invalid;
            EXPECT_EQ(harken::ProjectGenerationPrepareStatusV1::InvalidView,
                harken::PrepareProjectGenerationV1(*previewRuntime, candidate, invalid).status);
            ASSERT_NO_FATAL_FAILURE(assertUnchanged());
            volumes[sourceSlot] = -12.0f;
            harken::PreparedProjectGenerationV1 canceled;
            ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
                harken::PrepareProjectGenerationV1(*previewRuntime, candidate, canceled).status);
            harken::CancelPreparedProjectGenerationV1(*previewRuntime, std::move(canceled));
            ASSERT_NO_FATAL_FAILURE(assertUnchanged());
            harken::PreparedProjectGenerationV1 blocked;
            ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
                harken::PrepareProjectGenerationV1(*previewRuntime, candidate, blocked).status);
            previewRuntime->topologyPlans.DrainRetire();
            std::vector<std::uint32_t> leases;
            while (previewRuntime->topologyPlans.InUse() < previewRuntime->topologyPlans.Capacity())
                leases.push_back(previewRuntime->topologyPlans.Acquire());
            const auto headBefore = previewRuntime->commands.PublishedHead();
            EXPECT_EQ(harken::ProjectGenerationCommitStatusV1::Backpressure,
                harken::CommitPreparedProjectGenerationV1(*previewRuntime, blocked).status);
            EXPECT_EQ(headBefore, previewRuntime->commands.PublishedHead());
            ASSERT_NO_FATAL_FAILURE(assertUnchanged());
            for (auto lease : leases) previewRuntime->topologyPlans.Recycle(lease);
            const auto beforeRejectedAudio = previewRuntime->mixer.BlocksRendered();
            previewRuntime->nullBackend.Pump(1);
            ASSERT_EQ(beforeRejectedAudio + 1, previewRuntime->mixer.BlocksRendered());
            const auto samples = previewRuntime->nullBackend.BlockFrames() * previewRuntime->nullBackend.Channels();
            const float oldPcm = kToneAmplitude * (1.0f + std::pow(10.0f, -12.0f / 20.0f));
            for (std::uint32_t sample = 0; sample < samples; ++sample)
                EXPECT_NEAR(oldPcm, previewRuntime->nullBackend.LastBlock()[sample], 1e-4f);
            ASSERT_EQ(harken::ProjectGenerationCommitStatusV1::Ok,
                harken::CommitPreparedProjectGenerationV1(*previewRuntime, blocked).status);
            EXPECT_EQ(ownerBefore.currentRevision + 1, ownerSlot->currentRevision);
            const auto newWorld = previewRuntime->variables.CurrentWorldGeneration();
            EXPECT_FALSE(harken::CurrentWorldBusAdmissionIsOpenV1(*previewRuntime, newWorld));
            const auto pendingAck = std::find_if(previewRuntime->projectGenerationInstallAckSideTable.begin(),
                previewRuntime->projectGenerationInstallAckSideTable.end(), [&](const auto& row) {
                    return row.valid == 1 && row.planFingerprint == candidate.token.authoringRevision
                        && row.claimSetFingerprint == candidate.token.submissionSerial;
                });
            ASSERT_NE(previewRuntime->projectGenerationInstallAckSideTable.end(), pendingAck);
            harken::ProjectGenerationInstallAckV1 staleAck{};
            staleAck.planIndex = static_cast<std::uint32_t>(std::distance(
                previewRuntime->projectGenerationInstallAckSideTable.begin(), pendingAck));
            staleAck.slotGeneration = pendingAck->slotGeneration;
            staleAck.planSequence = pendingAck->planSequence;
            staleAck.planFingerprint = pendingAck->planFingerprint;
            staleAck.claimSetFingerprint = pendingAck->claimSetFingerprint;
            const auto acceptedToken = previewRuntime->lastPairedProjectGenerationToken;
            const auto pendingAcks = previewRuntime->projectGenerationInstallAckSideTable;
            for (int field = 0; field < 4; ++field) {
                auto invalidAck = staleAck;
                if (field == 0) ++invalidAck.slotGeneration;
                if (field == 1) ++invalidAck.planSequence;
                if (field == 2) ++invalidAck.planFingerprint;
                if (field == 3) ++invalidAck.claimSetFingerprint;
                previewRuntime->projectGenerationInstallAckRing.Push(invalidAck);
                EXPECT_EQ(1u, harken::DrainProjectGenerationInstallAcksV1(*previewRuntime));
                EXPECT_FALSE(harken::CurrentWorldBusAdmissionIsOpenV1(*previewRuntime, newWorld));
                EXPECT_EQ(0, std::memcmp(&acceptedToken, &previewRuntime->lastPairedProjectGenerationToken,
                    sizeof(acceptedToken)));
                EXPECT_EQ(0, std::memcmp(pendingAcks.data(),
                    previewRuntime->projectGenerationInstallAckSideTable.data(),
                    pendingAcks.size() * sizeof(pendingAcks.front())));
                EXPECT_EQ(confirmedIdsBefore, editor.getRuntimeBusIds());
                EXPECT_FLOAT_EQ(confirmedVolumeBefore, editor.getBusVolumeDbById(loadedSource->runtimeId()));
            }
            previewRuntime->nullBackend.Pump(1);
            EXPECT_FALSE(harken::CurrentWorldBusAdmissionIsOpenV1(*previewRuntime, newWorld));
            EXPECT_EQ(1u, harken::DrainProjectGenerationInstallAcksV1(*previewRuntime));
            EXPECT_TRUE(harken::CurrentWorldBusAdmissionIsOpenV1(*previewRuntime, newWorld));
            RecordProperty("stale_actual_ring_records_rejected", 4);
            for (std::uint32_t sample = 0; sample < samples; ++sample)
                EXPECT_NEAR(oldPcm * std::pow(10.0f, -12.0f / 20.0f),
                    previewRuntime->nullBackend.LastBlock()[sample], 1e-4f);
            ASSERT_EQ(VN_OK, Vn_Update());
            harken::PreparedProjectGenerationV1 exact;
            ASSERT_EQ(harken::ProjectGenerationPrepareStatusV1::Ok,
                harken::PrepareProjectGenerationV1(*previewRuntime, candidate, exact).status);
            ASSERT_EQ(harken::ProjectGenerationCommitStatusV1::Ok,
                harken::CommitPreparedProjectGenerationV1(*previewRuntime, exact).status);
            EXPECT_EQ(ownerBefore.currentRevision + 1, ownerSlot->currentRevision);
            EXPECT_EQ(newWorld, previewRuntime->variables.CurrentWorldGeneration());
            RecordProperty("fault_tuple_ack_rows", static_cast<int>(acksBefore.size()));
            RecordProperty("fault_tuple_old_world", std::to_string(worldBefore));
            RecordProperty("fault_tuple_retry_world", std::to_string(newWorld));
        }
        editor.shutdown();
        ASSERT_TRUE(projects.closeProject());
        LoadedExportedPack loaded;
        ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
            outputDir, QStringLiteral("ExportedPackImmediateSound"), &loaded));
        auto* packRuntime = loaded.session.Engine();
        ASSERT_NE(nullptr, packRuntime);
        const auto packSource = packRuntime->buses.FindIndex(sourceId);
        const auto packTarget = packRuntime->buses.FindIndex(destinationId);
        ASSERT_NE(harken::kInvalidBusIndex, packSource);
        ASSERT_NE(harken::kInvalidBusIndex, packTarget);
        bool foundPack = false;
        std::size_t packSendFlat = 0;
        for (std::uint32_t slot = 0; slot < sendSlots; ++slot) {
            const auto flat = static_cast<std::size_t>(packSource) * sendSlots + slot;
            if (packRuntime->busSendAdj[flat] == packTarget) {
                EXPECT_FLOAT_EQ(unity ? 0.0f : -6.0f,
                                packRuntime->busSendLevelDb[flat]);
                packSendFlat = flat;
                foundPack = true;
            }
        }
        ASSERT_TRUE(foundPack);
        VnPostActionDesc post{};
        post.structSize = sizeof(post);
        post.actionId = harkentest::ToRuntime(harkentest::StudioObjectId(
            kImmediateSoundActionObjectId + kStableIdBias));
        ASSERT_EQ(VN_OK, Vn_CreateEmitter(&post.emitter));
        VnActionFiringHandle firing = 0;
        ASSERT_EQ(VN_OK, Vn_PostAction(&post, &firing));
        ASSERT_NO_FATAL_FAILURE(expectActivePlayable(firing, packRuntime, 1));
        const auto beforePack = packRuntime->mixer.BlocksRendered();
        for (std::uint32_t ordinal = 1; ordinal <= 8; ++ordinal) {
            ASSERT_EQ(VN_OK, Vn_Update());
            packRuntime->nullBackend.Pump(1);
            ASSERT_EQ(beforePack + ordinal, packRuntime->mixer.BlocksRendered());
            const auto freshSamples = packRuntime->nullBackend.BlockFrames() *
                packRuntime->nullBackend.Channels();
            if (harkentest::Peak(packRuntime->nullBackend.LastBlock(), freshSamples) > harkentest::kSilenceFloor) {
                const float expected = kToneAmplitude *
                    (1.0f + std::pow(10.0f, (unity ? 0.0f : -6.0f) / 20.0f));
                for (std::uint32_t sample = 0; sample < freshSamples; ++sample)
                    EXPECT_NEAR(expected, packRuntime->nullBackend.LastBlock()[sample], 1e-4f)
                        << "new send first audible/steady ordinal=" << ordinal << " sample=" << sample;
            }
            *packPeak = std::max(*packPeak, harkentest::Peak(
                packRuntime->nullBackend.LastBlock(),
                packRuntime->nullBackend.BlockFrames() *
                    packRuntime->nullBackend.Channels()));
        }
        ASSERT_GT(*packPeak, harkentest::kSilenceFloor);
        if (!unity) {
            const auto oldRevision = loaded.activationReport.resultSetRevision;
            const auto oldGeneration = packRuntime->buses.GenerationAt(packSource);
            const auto oldHandle = loaded.initHandle;
            loaded.rootUtf8 = QFileInfo(changedDir).absoluteFilePath().toUtf8();
            ExportedPackActivation prepared;
            ASSERT_NO_FATAL_FAILURE(prepareExportedPackActivation(
                changedDir, QStringLiteral("ExportedPackImmediateSound"), loaded, &prepared));
            VnCanonicalPackSetActivationReportV1 changed{};
            ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(prepared.members.data(),
                static_cast<std::uint32_t>(prepared.members.size()), prepared.rows.data(),
                changed, oldHandle, oldRevision));
            ASSERT_NO_FATAL_FAILURE(acceptExportedPackActivation(&prepared, changed, &loaded));
            EXPECT_EQ(oldRevision + 1, changed.resultSetRevision);
            EXPECT_EQ(packSource, packRuntime->buses.FindIndex(sourceId));
            EXPECT_EQ(oldGeneration, packRuntime->buses.GenerationAt(packSource));
            std::vector<float> packRamp;
            ASSERT_NO_FATAL_FAILURE(checkRamp(packRuntime, &packRamp, [&]() {
                VnCanonicalPackSetActivationReportV1 retry{};
                ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(loaded.members.data(),
                    static_cast<std::uint32_t>(loaded.members.size()), loaded.memberResults.data(),
                    retry, oldHandle, oldRevision));
                EXPECT_EQ(changed.resultSetRevision, retry.resultSetRevision);
                EXPECT_EQ(changed.activationGeneration, retry.activationGeneration);
            }));
            ASSERT_EQ(previewRamp.size(), packRamp.size());
            for (std::size_t sample = 0; sample < packRamp.size(); ++sample)
                EXPECT_NEAR(previewRamp[sample], packRamp[sample], 1e-4f) << "paired ramp sample " << sample;
            EXPECT_FLOAT_EQ(-12.0f, packRuntime->busSendLevelDb[packSendFlat]);
            RecordProperty("retained_send_ramp_frames", 1024);
            RecordProperty("retained_send_compared_samples", static_cast<int>(packRamp.size()));
        }
        ASSERT_EQ(VN_OK, Vn_StopActionFiring(firing, 0));
        const auto released = Vn_ReleaseActionFiring(firing);
        EXPECT_TRUE(released == VN_OK || released == VN_IN_USE);
    };
    float minusSixPreview = 0.0f, minusSixPack = 0.0f;
    ASSERT_NO_FATAL_FAILURE(capture(false,
        QDir(dir_.path()).filePath(QStringLiteral("send-minus-six")),
        &minusSixPreview, &minusSixPack));
    float zeroPreview = 0.0f, zeroPack = 0.0f;
    ASSERT_NO_FATAL_FAILURE(capture(true,
        QDir(dir_.path()).filePath(QStringLiteral("send-zero")),
        &zeroPreview, &zeroPack));
    EXPECT_NEAR(minusSixPreview, minusSixPack, 0.003f);
    EXPECT_NEAR(zeroPreview, zeroPack, 0.003f);
    EXPECT_GT(zeroPreview, minusSixPreview * 1.25f);
}

TEST_F(AuthoredPackLifecycle, CanonicalV2SnapshotRejectsTamperAndUnknownVintage)
{
    const QString outputDir = QDir(dir_.path()).filePath(QStringLiteral("volume-v2-wire"));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, outputDir));
    QFile file(QDir(outputDir).filePath(QStringLiteral("Init.bytes")));
    ASSERT_TRUE(file.open(QIODevice::ReadOnly));
    const QByteArray raw = file.readAll();
    const std::vector<std::uint8_t> original(raw.begin(), raw.end());
    harken::PackCanonicalImage valid;
    ASSERT_EQ(VN_OK, harken::ParsePackCanonicalImage(
        original.data(), original.size(), &valid));
    ASSERT_TRUE(valid.hasBusDefinitionSnapshot);
    ASSERT_EQ(2u, valid.busDefinitionSnapshotHeader.schemaVersion);
    harken::PackV2Header header{};
    ASSERT_TRUE(harken::ParsePackV2Header(original.data(), &header));
    std::size_t directoryOffset = 0;
    harken::PackV2DirectoryEntry snapshot{};
    for (std::uint32_t i = 0; i < header.entryCount; ++i) {
        const auto offset = static_cast<std::size_t>(header.directoryOffset)
            + static_cast<std::size_t>(i) * harken::kPackV2DirectoryEntryBytes;
        const auto entry = harken::ParsePackV2DirectoryEntry(original.data() + offset);
        if (entry.kind == harken::kPackEntryKindBusDefinitionSnapshot) {
            snapshot = entry;
            directoryOffset = offset;
            break;
        }
    }
    ASSERT_NE(0u, directoryOffset);
    ASSERT_EQ(harken::kPackBusDefinitionSnapshotVintage2, snapshot.vintage);
    ASSERT_GT(snapshot.size, 16u);
    auto damaged = original;
    damaged[static_cast<std::size_t>(snapshot.offset + 16)] ^= 0x5au;
    harken::PackCanonicalImage rejected;
    EXPECT_EQ(VN_BAD_PACK, harken::ParsePackCanonicalImage(
        damaged.data(), damaged.size(), &rejected));

    ASSERT_GE(valid.busDefinitionBusesV2.size(), 2u);
    auto duplicate = original;
    auto* snapshotPayload = duplicate.data() + snapshot.offset;
    harken::PackBusDefinitionSnapshotHeaderV1 snapshotHeader{};
    ASSERT_TRUE(harken::ParsePackBusDefinitionSnapshotHeaderV2(
        snapshotPayload, snapshot.size, &snapshotHeader));
    auto* secondBus = snapshotPayload + snapshotHeader.busOffset
        + snapshotHeader.busRowBytes;
    harken::PackBusDefinitionBusV2 secondRow{};
    ASSERT_TRUE(harken::ParsePackBusDefinitionBusV2(secondBus, &secondRow));
    secondRow.busId = valid.busDefinitionBusesV2.front().busId;
    harken::SerializePackBusDefinitionBusV2(secondRow, secondBus);
    harken::StoreLE64(snapshotPayload + 112, 0);
    const auto snapshotChecksum = harken::Fnv1a64(snapshotPayload,
        static_cast<std::size_t>(snapshot.size));
    harken::StoreLE64(snapshotPayload + 112, snapshotChecksum);
    ASSERT_TRUE(harken::ParsePackBusDefinitionSnapshotHeaderV2(
        snapshotPayload, snapshot.size, &snapshotHeader));
    auto duplicateEntry = snapshot;
    duplicateEntry.payloadChecksum = harken::Fnv1a64(snapshotPayload,
        static_cast<std::size_t>(snapshot.size));
    harken::SerializePackV2DirectoryEntry(duplicateEntry, duplicate.data() + directoryOffset);
    auto duplicateHeader = header;
    duplicateHeader.directoryChecksum = harken::Fnv1a64(
        duplicate.data() + duplicateHeader.directoryOffset,
        static_cast<std::size_t>(duplicateHeader.entryCount)
            * harken::kPackV2DirectoryEntryBytes);
    harken::SerializePackV2Header(duplicateHeader, duplicate.data());
    EXPECT_EQ(VN_BAD_PACK, harken::ParsePackCanonicalImage(
        duplicate.data(), duplicate.size(), &rejected));

    auto unknown = original;
    snapshot.vintage = 999;
    harken::SerializePackV2DirectoryEntry(snapshot, unknown.data() + directoryOffset);
    header.directoryChecksum = harken::Fnv1a64(unknown.data() + header.directoryOffset,
        static_cast<std::size_t>(header.entryCount) * harken::kPackV2DirectoryEntryBytes);
    harken::SerializePackV2Header(header, unknown.data());
    EXPECT_EQ(VN_PACK_INCOMPATIBLE, harken::ParsePackCanonicalImage(
        unknown.data(), unknown.size(), &rejected));
}

TEST_F(AuthoredPackLifecycle, CanonicalV2ReplaceUpdatesRetainedMasterAndLiveSound)
{
    const QString firstDir = QDir(dir_.path()).filePath(QStringLiteral("volume-replace-first"));
    const QString nextDir = QDir(dir_.path()).filePath(QStringLiteral("volume-replace-next"));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, firstDir));
    LoadedExportedPack loaded;
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        firstDir, QStringLiteral("ExportedPackImmediateSound"), &loaded));
    auto* engine = loaded.session.Engine();
    ASSERT_NE(nullptr, engine);
    const auto busId = harkentest::ToRuntime(project_.busses.front().rootItem->runtimeId());
    const auto index = engine->buses.FindIndex(busId);
    ASSERT_NE(harken::kInvalidBusIndex, index);
    const auto oldGeneration = engine->buses.GenerationAt(index);
    const auto oldRevision = loaded.activationReport.resultSetRevision;
    const auto oldHandle = loaded.initHandle;
    VnPostActionDesc post{};
    post.structSize = sizeof(post);
    post.actionId = harkentest::ToRuntime(harkentest::StudioObjectId(
        kImmediateSoundActionObjectId + kStableIdBias));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&post.emitter));
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&post, &firing));
    ASSERT_NO_FATAL_FAILURE(expectActivePlayable(firing, engine, 1));
    const auto sampleCount = static_cast<std::size_t>(engine->nullBackend.BlockFrames())
        * engine->nullBackend.Channels();
    const auto initialBlock = engine->mixer.BlocksRendered();
    std::vector<float> before;
    std::uint64_t audibleBlock = 0;
    for (std::uint32_t ordinal = 1; ordinal <= 16; ++ordinal) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
        ASSERT_EQ(initialBlock + ordinal, engine->mixer.BlocksRendered());
        const float* block = engine->nullBackend.LastBlock();
        if (std::any_of(block, block + sampleCount, [](float sample) {
                return std::abs(sample) > 0.05f && std::abs(sample) < 0.8f;
            })) {
            before.assign(block, block + sampleCount);
            audibleBlock = engine->mixer.BlocksRendered();
            break;
        }
    }
    ASSERT_FALSE(before.empty());
    const auto sample = std::find_if(before.begin(), before.end(), [](float value) {
        return std::abs(value) > 0.05f && std::abs(value) < 0.8f;
    });
    ASSERT_NE(sample, before.end());
    const auto sampleIndex = static_cast<std::size_t>(sample - before.begin());

    auto* mix = project_.busses.front().rootItem->comp<VNX::VMixNode>();
    ASSERT_NE(nullptr, mix);
    mix->outputBusVolume = -6.0f;
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, nextDir));
    loaded.rootUtf8 = QFileInfo(nextDir).absoluteFilePath().toUtf8();
    ExportedPackActivation prepared;
    ASSERT_NO_FATAL_FAILURE(prepareExportedPackActivation(
        nextDir, QStringLiteral("ExportedPackImmediateSound"), loaded, &prepared));
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(
        prepared.members.data(), static_cast<std::uint32_t>(prepared.members.size()),
        prepared.rows.data(), report, oldHandle, oldRevision));
    ASSERT_NO_FATAL_FAILURE(acceptExportedPackActivation(&prepared, report, &loaded));
    EXPECT_EQ(oldRevision + 1, loaded.activationReport.resultSetRevision);
    EXPECT_EQ(index, engine->buses.FindIndex(busId));
    EXPECT_EQ(oldGeneration, engine->buses.GenerationAt(index));
    EXPECT_EQ(loaded.initHandle, engine->busOwnerCookies[index]);
    EXPECT_FLOAT_EQ(-6.0f, engine->busTargets.SlotValue(
        index, harken::kBusTargetBusOutputVolume,
        harken::BusTargetStore::kSlotAuthoredIndex));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(audibleBlock + 1, engine->mixer.BlocksRendered());
    const float expectedGain = std::pow(10.0f, -6.0f / 20.0f);
    for (std::size_t i = 0; i < sampleCount; ++i)
        EXPECT_NEAR(before[i] * expectedGain, engine->nullBackend.LastBlock()[i], 1e-4f)
            << "retained first application sample " << i;
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    ASSERT_EQ(audibleBlock + 2, engine->mixer.BlocksRendered());
    for (std::size_t i = 0; i < sampleCount; ++i)
        EXPECT_NEAR(before[i] * expectedGain, engine->nullBackend.LastBlock()[i], 1e-4f)
            << "retained steady sample " << i;
    ASSERT_EQ(VN_OK, Vn_StopActionFiring(firing, 0));
    const VnResult released = Vn_ReleaseActionFiring(firing);
    EXPECT_TRUE(released == VN_OK || released == VN_IN_USE);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);

    VnActionFiringHandle successor = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&post, &successor));
    ASSERT_NO_FATAL_FAILURE(expectActivePlayable(successor, engine, 1));
    const auto nextBefore = engine->mixer.BlocksRendered();
    std::vector<float> next;
    for (std::uint32_t ordinal = 1; ordinal <= 16; ++ordinal) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
        ASSERT_EQ(nextBefore + ordinal, engine->mixer.BlocksRendered());
        const float* block = engine->nullBackend.LastBlock();
        if (std::any_of(block, block + sampleCount, [](float value) {
                return std::abs(value) > harkentest::kSilenceFloor;
            })) {
            next.assign(block, block + sampleCount);
            break;
        }
    }
    ASSERT_EQ(before.size(), next.size());
    EXPECT_NEAR(*sample * std::pow(10.0f, -6.0f / 20.0f),
        next[sampleIndex], 1e-4f);
    const VnResult successorReleased = Vn_ReleaseActionFiring(successor);
    EXPECT_TRUE(successorReleased == VN_OK || successorReleased == VN_IN_USE);
}

TEST_F(AuthoredPackLifecycle, CanonicalV2BindingRemovalChangesRetainedSoundInFirstNewBlock)
{
    auto* master = project_.busses.front().rootItem.get();
    ASSERT_NE(nullptr, master);
    VNX::ProjectItem* variable = nullptr;
    for (const auto& binding : project_.variables) {
        if (!binding.rootItem) continue;
        for (const auto& child : binding.rootItem->children) {
            if (child->objectID == kSmoothVariableObjectId) variable = child.get();
        }
    }
    ASSERT_NE(nullptr, variable);
    auto* mix = master->comp<VNX::VMixNode>();
    ASSERT_NE(nullptr, mix);
    VNX::ParameterBindingDef authored;
    authored.guid = VNX::authoringUuid(harkentest::StudioObjectId(9001));
    authored.variable = variable->guid;
    authored.address = VNX::BindingSemantics::volumeAddress(VNX::ProjectItemType::Bus);
    authored.curve = {{0.0f, -6.0f}, {1.0f, -6.0f}};
    authored.outputMin = -12.0f;
    authored.outputMax = 0.0f;
    ASSERT_TRUE(mix->bindings.upsert(authored));

    const QString firstDir = QDir(dir_.path()).filePath(QStringLiteral("binding-before"));
    const QString nextDir = QDir(dir_.path()).filePath(QStringLiteral("binding-after"));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, firstDir));
    LoadedExportedPack loaded;
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        firstDir, QStringLiteral("ExportedPackImmediateSound"), &loaded));
    auto* engine = loaded.session.Engine();
    ASSERT_NE(nullptr, engine);
    const auto index = engine->buses.FindIndex(harkentest::ToRuntime(master->runtimeId()));
    ASSERT_NE(harken::kInvalidBusIndex, index);
    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ToRuntime(variable->runtimeId()), 0.0f));
    harkentest::AdvanceVariableWrite(engine);
    ASSERT_FLOAT_EQ(-6.0f, engine->busTargets.SlotValue(
        index, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotVariableIndex));

    VnPostActionDesc post{};
    post.structSize = sizeof(post);
    post.actionId = harkentest::ToRuntime(harkentest::StudioObjectId(
        kImmediateSoundActionObjectId + kStableIdBias));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&post.emitter));
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&post, &firing));
    ASSERT_NO_FATAL_FAILURE(expectActivePlayable(firing, engine, 1));
    const auto sampleCount = static_cast<std::size_t>(engine->nullBackend.BlockFrames())
        * engine->nullBackend.Channels();
    const auto beforeBlock = engine->mixer.BlocksRendered();
    std::vector<float> before;
    std::uint64_t audibleBlock = 0;
    for (std::uint32_t ordinal = 1; ordinal <= 16; ++ordinal) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
        ASSERT_EQ(beforeBlock + ordinal, engine->mixer.BlocksRendered());
        const float* block = engine->nullBackend.LastBlock();
        if (std::any_of(block, block + sampleCount, [](float value) {
                return std::abs(value) > 0.05f && std::abs(value) < 0.8f;
            })) {
            before.assign(block, block + sampleCount);
            audibleBlock = engine->mixer.BlocksRendered();
            break;
        }
    }
    ASSERT_FALSE(before.empty());
    const auto sample = std::find_if(before.begin(), before.end(), [](float value) {
        return std::abs(value) > 0.05f && std::abs(value) < 0.8f;
    });
    ASSERT_NE(sample, before.end());
    const auto sampleIndex = static_cast<std::size_t>(sample - before.begin());

    ASSERT_TRUE(mix->bindings.remove(authored.runtimeId()));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, nextDir));
    loaded.rootUtf8 = QFileInfo(nextDir).absoluteFilePath().toUtf8();
    ExportedPackActivation prepared;
    ASSERT_NO_FATAL_FAILURE(prepareExportedPackActivation(
        nextDir, QStringLiteral("ExportedPackImmediateSound"), loaded, &prepared));
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(
        prepared.members.data(), static_cast<std::uint32_t>(prepared.members.size()),
        prepared.rows.data(), report, loaded.initHandle,
        loaded.activationReport.resultSetRevision));
    ASSERT_NO_FATAL_FAILURE(acceptExportedPackActivation(&prepared, report, &loaded));
    engine->nullBackend.Pump(1);
    ASSERT_EQ(audibleBlock + 1, engine->mixer.BlocksRendered());
    EXPECT_NEAR(*sample * std::pow(10.0f, 6.0f / 20.0f),
        engine->nullBackend.LastBlock()[sampleIndex], 1e-4f);
    const VnResult released = Vn_ReleaseActionFiring(firing);
    EXPECT_TRUE(released == VN_OK || released == VN_IN_USE);
}

TEST_F(AuthoredPackLifecycle, CanonicalV2NewBindingAndDefaultReachFirstRetainedBlock)
{
    auto* master = project_.busses.front().rootItem.get();
    ASSERT_NE(nullptr, master);
    VNX::ProjectItem* variable = nullptr;
    for (const auto& group : project_.variables) {
        if (!group.rootItem) continue;
        for (const auto& child : group.rootItem->children)
            if (child->objectID == kSmoothVariableObjectId) variable = child.get();
    }
    ASSERT_NE(nullptr, variable);
    auto* mix = master->comp<VNX::VMixNode>();
    auto* smooth = variable->comp<VNX::VVariable>();
    ASSERT_NE(nullptr, mix);
    ASSERT_NE(nullptr, smooth);
    const QString firstDir = QDir(dir_.path()).filePath(QStringLiteral("binding-add-first"));
    const QString nextDir = QDir(dir_.path()).filePath(QStringLiteral("binding-add-next"));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, firstDir));
    LoadedExportedPack loaded;
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        firstDir, QStringLiteral("ExportedPackImmediateSound"), &loaded));
    auto* engine = loaded.session.Engine();
    ASSERT_NE(nullptr, engine);
    const auto index = engine->buses.FindIndex(harkentest::ToRuntime(master->runtimeId()));
    ASSERT_NE(harken::kInvalidBusIndex, index);

    VnPostActionDesc post{};
    post.structSize = sizeof(post);
    post.actionId = harkentest::ToRuntime(harkentest::StudioObjectId(
        kImmediateSoundActionObjectId + kStableIdBias));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&post.emitter));
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&post, &firing));
    ASSERT_NO_FATAL_FAILURE(expectActivePlayable(firing, engine, 1));
    const auto sampleCount = static_cast<std::size_t>(engine->nullBackend.BlockFrames())
        * engine->nullBackend.Channels();
    std::vector<float> before;
    for (std::uint32_t ordinal = 0; ordinal < 16 && before.empty(); ++ordinal) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
        const float* block = engine->nullBackend.LastBlock();
        if (std::any_of(block, block + sampleCount, [](float value) {
                return std::abs(value) > 0.05f && std::abs(value) < 0.8f;
            })) before.assign(block, block + sampleCount);
    }
    ASSERT_FALSE(before.empty());
    const auto sample = std::find_if(before.begin(), before.end(), [](float value) {
        return std::abs(value) > 0.05f && std::abs(value) < 0.8f;
    });
    ASSERT_NE(sample, before.end());
    const auto sampleIndex = static_cast<std::size_t>(sample - before.begin());

    VNX::ParameterBindingDef authored;
    authored.guid = VNX::authoringUuid(harkentest::StudioObjectId(9002));
    authored.variable = variable->guid;
    authored.address = VNX::BindingSemantics::volumeAddress(VNX::ProjectItemType::Bus);
    authored.curve = {{0.0f, 0.0f}, {1.0f, -6.0f}};
    authored.outputMin = -12.0f;
    authored.outputMax = 0.0f;
    ASSERT_TRUE(mix->bindings.upsert(authored));
    smooth->defaultValue = 0.5f;
    smooth->normalizeDeclaration();
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, nextDir));
    loaded.rootUtf8 = QFileInfo(nextDir).absoluteFilePath().toUtf8();
    ExportedPackActivation prepared;
    ASSERT_NO_FATAL_FAILURE(prepareExportedPackActivation(
        nextDir, QStringLiteral("ExportedPackImmediateSound"), loaded, &prepared));
    VnCanonicalPackSetActivationReportV1 report{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(
        prepared.members.data(), static_cast<std::uint32_t>(prepared.members.size()),
        prepared.rows.data(), report, loaded.initHandle,
        loaded.activationReport.resultSetRevision));
    ASSERT_NO_FATAL_FAILURE(acceptExportedPackActivation(&prepared, report, &loaded));
    const auto* binding = engine->bindings.Find(harkentest::ToRuntime(authored.runtimeId()));
    ASSERT_NE(nullptr, binding);
    const float contribution = binding->Evaluate(0.5f);
    ASSERT_LT(contribution, 0.0f);
    EXPECT_FLOAT_EQ(contribution, engine->busTargets.SlotValue(
        index, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotVariableIndex));
    const float expected = *sample * std::pow(10.0f, contribution / 20.0f);
    const auto beforeBlock = engine->mixer.BlocksRendered();
    engine->nullBackend.Pump(1);
    ASSERT_EQ(beforeBlock + 1, engine->mixer.BlocksRendered());
    EXPECT_NEAR(expected, engine->nullBackend.LastBlock()[sampleIndex], 1e-4f);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    EXPECT_NEAR(expected, engine->nullBackend.LastBlock()[sampleIndex], 1e-4f);

    authored.curve = {{0.0f, -8.0f}, {1.0f, -8.0f}};
    authored.outputMin = -12.0f;
    ASSERT_TRUE(mix->bindings.upsert(authored));
    const QString revisedDir = QDir(dir_.path()).filePath(QStringLiteral("binding-add-revised"));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, revisedDir));
    loaded.rootUtf8 = QFileInfo(revisedDir).absoluteFilePath().toUtf8();
    ExportedPackActivation revised;
    ASSERT_NO_FATAL_FAILURE(prepareExportedPackActivation(
        revisedDir, QStringLiteral("ExportedPackImmediateSound"), loaded, &revised));
    VnCanonicalPackSetActivationReportV1 revisedReport{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(
        revised.members.data(), static_cast<std::uint32_t>(revised.members.size()),
        revised.rows.data(), revisedReport, loaded.initHandle,
        loaded.activationReport.resultSetRevision));
    ASSERT_NO_FATAL_FAILURE(acceptExportedPackActivation(&revised, revisedReport, &loaded));
    EXPECT_FLOAT_EQ(-8.0f, engine->busTargets.SlotValue(
        index, harken::kBusTargetBusVolume,
        harken::BusTargetStore::kSlotVariableIndex));
    const float revisedExpected = *sample * std::pow(10.0f, -8.0f / 20.0f);
    engine->nullBackend.Pump(1);
    EXPECT_NEAR(revisedExpected, engine->nullBackend.LastBlock()[sampleIndex], 1e-4f);
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    EXPECT_NEAR(revisedExpected, engine->nullBackend.LastBlock()[sampleIndex], 1e-4f);
    const VnResult released = Vn_ReleaseActionFiring(firing);
    EXPECT_TRUE(released == VN_OK || released == VN_IN_USE);
}

class ExportedPackLifecycle : public AuthoredPackLifecycle
{
protected:
    static void SetUpTestSuite()
    {
        qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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
        qputenv("HARKEN_PACKC_PATH", QByteArray(HARKEN_PACKC_PATH));
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

GoldHostRun runHarkenHostBinary(const QStringList& args, const QString& program = QString())
{
    GoldHostRun run;
    QProcess host;
    host.setProcessChannelMode(QProcess::MergedChannels);
    host.setProgram(program.isEmpty() ? QString::fromUtf8(HARKEN_HOST_PATH) : program);
    host.setArguments(args);
    host.start();
    if (!host.waitForStarted(30000)) {
        const QByteArray bytes = host.readAll();
        run.output.assign(bytes.constData(), static_cast<size_t>(bytes.size()));
        return run;
    }
    QElapsedTimer timer;
    timer.start();
    QByteArray bytes;
    while (!host.waitForFinished(100)) {
        bytes += host.readAll();
        if (timer.elapsed() >= 30000) {
            host.kill();
            host.waitForFinished(5000);
            bytes += host.readAll();
            run.output.assign(bytes.constData(), static_cast<size_t>(bytes.size()));
            return run;
        }
    }
    bytes += host.readAll();
    run.output.assign(bytes.constData(), static_cast<size_t>(bytes.size()));
    if (host.exitStatus() != QProcess::NormalExit)
        return run;
    run.exitCode = host.exitCode();
    return run;
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

void att06StageSpacedHost(QString* exeOut)
{
    ASSERT_NE(nullptr, exeOut);
    const QString sourceExe = QString::fromUtf8(HARKEN_HOST_PATH);
    ASSERT_TRUE(QFileInfo::exists(sourceExe)) << sourceExe.toStdString();
    const QString destDir = QDir(QString::fromUtf8(HARKEN_TEMP_DIR)).filePath(
        QStringLiteral("att06-space-host/space dir"));
    const auto destPath = att06FsPath(destDir);
    std::error_code ec;
    std::filesystem::remove_all(destPath, ec);
    ASSERT_TRUE(QDir().mkpath(destDir)) << destDir.toStdString();
    const QDir srcDir(QFileInfo(sourceExe).absolutePath());
    const auto destExePath = destPath / att06FsPath(QFileInfo(sourceExe).fileName()).filename();
    ASSERT_TRUE(att06StageFile(att06FsPath(sourceExe), destExePath)) << destDir.toStdString();
    const auto dlls = srcDir.entryList(QStringList{QStringLiteral("*.dll")}, QDir::Files);
    for (const QString& name : dlls) {
        ASSERT_TRUE(att06StageFile(att06FsPath(srcDir.filePath(name)),
                                   destPath / att06FsPath(name).filename()))
            << name.toStdString();
    }
    const QString qtConf = srcDir.filePath(QStringLiteral("qt.conf"));
    if (QFileInfo::exists(qtConf)) {
        ASSERT_TRUE(att06StageFile(att06FsPath(qtConf), destPath / std::filesystem::path("qt.conf")));
    }
    const auto dirs = srcDir.entryList(QDir::Dirs | QDir::NoDotAndDotDot);
    for (const QString& name : dirs) {
        ASSERT_TRUE(att06StageTree(att06FsPath(srcDir.filePath(name)),
                                   destPath / att06FsPath(name).filename()))
            << name.toStdString();
    }
    *exeOut = QFileInfo(att06QString(att06Utf8(destExePath))).absoluteFilePath();
    ASSERT_TRUE(QFileInfo::exists(*exeOut)) << exeOut->toStdString();
    ASSERT_TRUE(exeOut->contains(QLatin1Char(' '))) << exeOut->toStdString();
}

void att06ExpectNoSuccessLines(const GoldHostRun& run)
{
    std::vector<Att06Opened> opened;
    att06CollectOpened(run.output, &opened);
    EXPECT_TRUE(opened.empty()) << run.output;
    Att06Identity identity;
    EXPECT_FALSE(att06ParseIdentity(run.output, &identity)) << run.output;
    EXPECT_FALSE(att06HasDistanceSuccess(run.output)) << run.output;
}

bool att06ChoosePayload(const std::vector<uint8_t>& bytes, uint64_t* offset, uint64_t* size,
                        uint64_t* payloadChecksum, uint64_t* directoryChecksum)
{
    if (bytes.size() < harken::kPackV2HeaderBytes)
        return false;
    harken::PackV2Header header;
    if (!harken::ParsePackV2Header(bytes.data(), &header))
        return false;
    if (header.fileBytes != bytes.size() ||
        header.headerBytes != harken::kPackV2HeaderBytes ||
        header.directoryOffset != harken::kPackV2HeaderBytes)
        return false;
    if (header.entryCount == 0 ||
        header.entryCount > (std::numeric_limits<uint64_t>::max() /
                             harken::kPackV2DirectoryEntryBytes))
        return false;
    const uint64_t dirBytes =
        uint64_t(header.entryCount) * harken::kPackV2DirectoryEntryBytes;
    if (header.directoryOffset > bytes.size() ||
        dirBytes > bytes.size() - header.directoryOffset)
        return false;
    const uint64_t dirEnd = header.directoryOffset + dirBytes;
    *directoryChecksum = harken::Fnv1a64(bytes.data() + header.directoryOffset,
                                        static_cast<size_t>(dirBytes));
    if (*directoryChecksum != header.directoryChecksum)
        return false;
    for (uint32_t i = 0; i < header.entryCount; ++i) {
        const auto entry = harken::ParsePackV2DirectoryEntry(
            bytes.data() + header.directoryOffset +
            size_t(i) * harken::kPackV2DirectoryEntryBytes);
        if (entry.kind == harken::kPackEntryKindInitRoutingCatalog)
            continue;
        if (entry.size == 0 || entry.offset < dirEnd ||
            entry.offset % harken::kPackV2PayloadAlign != 0 ||
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

TEST_F(AuthoredPackLifecycle, LabelPackRoundTripFiltersHiddenAndRejectsStaleDiscreteMapping)
{
    const auto find = [&](uint32_t objectId) -> VNX::ProjectItem* {
        const auto visit = [&](auto&& self, VNX::ProjectItem* item) -> VNX::ProjectItem* {
            if (item->objectID == objectId) return item;
            for (const auto& child : item->children)
                if (auto* found = self(self, child.get())) return found;
            return nullptr;
        };
        for (auto* binding : project_.allFileBindings())
            if (binding && binding->rootItem)
                if (auto* found = visit(visit, binding->rootItem.get())) return found;
        return nullptr;
    };
    auto* stepItem = find(kStepVariableObjectId);
    auto* smoothItem = find(kSmoothVariableObjectId);
    ASSERT_NE(stepItem, nullptr);
    ASSERT_NE(smoothItem, nullptr);
    auto* step = stepItem->comp<VNX::VVariable>();
    auto* smooth = smoothItem->comp<VNX::VVariable>();
    step->labels = {{0, QStringLiteral("Silent")}, {1, QStringLiteral("Audible")},
                    {10, QStringLiteral("Hidden")}};
    smooth->labels = {{0, QStringLiteral("None")}, {1, QStringLiteral("Full")},
                      {2, QStringLiteral("HiddenSmooth")}};
    auto* selection = find(kDiscreteContainerObjectId)->comp<VNX::VSelection>();
    {
        auto grid = selection->matchGrid();
        ASSERT_TRUE(grid.setCell(0, 0, step->labels[0].id));
        ASSERT_TRUE(grid.setCell(1, 0, step->labels[1].id));
        EXPECT_TRUE(selection->selections.shared.setMatchGrid(grid));
    }
    const QString firstDir = dir_.filePath(QStringLiteral("labels-first"));
    const QString secondDir = dir_.filePath(QStringLiteral("labels-next"));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, firstDir));
    const auto companion = findNamedPack(readPublication(firstDir), QStringLiteral("Init"))
        .value(QStringLiteral("companion")).toObject();
    for (const auto& value : companion["variables"].toArray()) {
        const auto labels = value.toObject()["labels"].toArray();
        ASSERT_EQ(labels.size(), 2);
        for (const auto& label : labels) {
            EXPECT_TRUE(label.toObject().contains(QStringLiteral("guid")));
            EXPECT_FALSE(label.toObject()["name"].toString().startsWith(QStringLiteral("Hidden")));
        }
    }
    std::swap(step->labels[0].anchor, step->labels[1].anchor);
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, secondDir));
    {
        LoadedExportedPack loaded;
        ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(firstDir, QStringLiteral("ExportedPackVariableControlDiscrete"), &loaded));
        const auto variableId = harkentest::ToRuntime(stepItem->runtimeId());
        ASSERT_EQ(VN_OK, Vn_SetValueByLabelId(variableId, harkentest::ToRuntime(VNX::runtimeObjectId(step->labels[1].id)), 0));
        EXPECT_EQ(VN_NO_VARIABLE_LABEL, Vn_SetValueByLabelId(variableId, harkentest::ToRuntime(VNX::runtimeObjectId(step->labels[2].id)), 0));
        EXPECT_EQ(VN_NO_VARIABLE_LABEL, Vn_SetValueByLabelId(harkentest::ToRuntime(smoothItem->runtimeId()), harkentest::ToRuntime(VNX::runtimeObjectId(smooth->labels[2].id)), 0));
        ASSERT_EQ(VN_OK, Vn_SetValueByLabelId(harkentest::ToRuntime(smoothItem->runtimeId()), harkentest::ToRuntime(VNX::runtimeObjectId(smooth->labels[1].id)), 0));
        ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(loaded.session.Engine()));
        VnPackLoadDesc next{};
        ASSERT_NO_FATAL_FAILURE(makeRootReplaceDesc(&loaded, secondDir, QStringLiteral("ExportedPackVariableControlDiscrete"), &next));
        EXPECT_EQ(VN_BAD_PACK, Vn_ReplacePack(loaded.contentHandle, reinterpret_cast<const VnPackDesc*>(&next)));
    }
    {
        LoadedExportedPack loaded;
        ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(secondDir, QStringLiteral("ExportedPackVariableControlDiscrete"), &loaded));
        const auto variableId = harkentest::ToRuntime(stepItem->runtimeId());
        ASSERT_EQ(VN_OK, Vn_SetValueByLabelId(variableId, harkentest::ToRuntime(VNX::runtimeObjectId(step->labels[1].id)), 0));
        ASSERT_NO_FATAL_FAILURE(harkentest::AdvanceVariableWrite(loaded.session.Engine()));
        VnVariableQueryDescV1 query{};
        query.structSize = sizeof(query);
        query.variableId = variableId;
        VnVariableQueryResultV1 value{};
        value.structSize = sizeof(value);
        ASSERT_EQ(VN_OK, Vn_QueryVariable(&query, &value));
        EXPECT_FLOAT_EQ(value.currentValue, 0.0f);
        harkentest::Capture audio;
        ASSERT_NO_FATAL_FAILURE(postAndCaptureImmediate(&loaded,
            harkentest::StudioObjectId(kDiscreteActionObjectId + kStableIdBias), &audio, 1));
        EXPECT_TRUE(bufferIsAudible(audio.left));
    }
}

TEST_F(AuthoredPackLifecycle, QuantizedActionsMatchPreviewAndExecuteFromReloadedPacks)
{
    auto layer = project_.layers.front().rootItem;
    ASSERT_NE(nullptr, layer);
    const auto findLayerChild = [&](uint32_t objectId) {
        const auto it = std::find_if(
            layer->children.begin(), layer->children.end(),
            [&](const auto& child) { return child && child->objectID == objectId; });
        return it == layer->children.end() ? std::shared_ptr<VNX::ProjectItem>{} : *it;
    };
    const auto contentSound = findLayerChild(kContentSoundObjectId);
    const auto directTrack = findLayerChild(kDirectTrackObjectId);
    const auto directPiece = findLayerChild(kDirectPieceObjectId);
    ASSERT_NE(nullptr, contentSound);
    ASSERT_NE(nullptr, directTrack);
    ASSERT_NE(nullptr, directPiece);

    contentSound->comp<VNX::VAudio>()->loopCount = 64;
    const auto contentLocator =
        VNX::PlayablePropertyAuthoringV1::definitionLocator(*contentSound);
    ASSERT_TRUE(contentLocator.has_value());
    ASSERT_TRUE(VNX::PlayablePropertyAuthoringV1::setOverride(
        *contentSound, *contentLocator,
        static_cast<uint8_t>(
            harkenplayableproperties::PlayablePropertyIdV1::LoopCount),
        QJsonValue(64), std::nullopt));

    auto* piece = directPiece->comp<VNX::VPiece>();
    ASSERT_NE(nullptr, piece);
    piece->pieceTime = VNX::PieceTimeSpec::musical(500000, 4, 4);
    piece->markers.items = {
        {VNX::authoringUuid(harkentest::StudioObjectId(
             kQuantizedMarkerActionObjectId + kStableIdBias + 900000)),
         VNX::MarkerKind::Custom, QStringLiteral("QuantizeMarker"), 1920},
        {VNX::authoringUuid(harkentest::StudioObjectId(
             kQuantizedMarkerActionObjectId + kStableIdBias + 900001)),
         VNX::MarkerKind::Exit, QStringLiteral("Exit"), 2880},
    };
    ASSERT_TRUE(piece->markers.validate());

    struct QuantizedCase {
        QString name;
        uint32_t objectId;
        VNX::QuantizeTiming authoredTiming;
        harkenstudiobuild::ActionQuantizeTimingV1 previewTiming;
        harken::ActionQuantizeTimingV1 runtimeTiming;
        uint64_t boundaryTicks;
    };
    const std::vector<QuantizedCase> cases = {
        {QStringLiteral("ExportedPackQuantizedBeat"), kQuantizedBeatActionObjectId,
         VNX::QuantizeTiming::NextBeat,
         harkenstudiobuild::ActionQuantizeTimingV1::NextBeat,
         harken::ActionQuantizeTimingV1::NextBeat, 960},
        {QStringLiteral("ExportedPackQuantizedBar"), kQuantizedBarActionObjectId,
         VNX::QuantizeTiming::NextBar,
         harkenstudiobuild::ActionQuantizeTimingV1::NextBar,
         harken::ActionQuantizeTimingV1::NextBar, 3840},
        {QStringLiteral("ExportedPackQuantizedMarker"), kQuantizedMarkerActionObjectId,
         VNX::QuantizeTiming::NextMarker,
         harkenstudiobuild::ActionQuantizeTimingV1::NextMarker,
         harken::ActionQuantizeTimingV1::NextMarker, 1920},
    };

    auto actionsRoot = project_.actions.front().rootItem;
    ASSERT_NE(nullptr, actionsRoot);
    for (const auto& test : cases) {
        auto action = makeItem(test.name, VNX::ProjectItem::Type::Action, test.objectId);
        VNX::VActionStep step = playStep(*contentSound, 5.0f);
        step.quantize.timing = test.authoredTiming;
        step.quantize.referenceObjectID = directPiece->objectID;
        step.quantize.referenceName = directPiece->name;
        action->comp<VNX::VAction>()->steps = {step};
        addChild(actionsRoot, action);
    }

    auto stopAction = makeItem(
        QStringLiteral("ExportedPackQuantizedStop"),
        VNX::ProjectItem::Type::Action, kQuantizedStopActionObjectId);
    VNX::VActionStep immediatePlay = playStep(*contentSound);
    VNX::VActionStep quantizedStop;
    quantizedStop.type = VNX::VActionStep::Type::Stop;
    quantizedStop.targetObjectID = contentSound->objectID;
    quantizedStop.targetName = contentSound->name;
    quantizedStop.quantize.timing = VNX::QuantizeTiming::ExitMarker;
    quantizedStop.quantize.referenceObjectID = directPiece->objectID;
    quantizedStop.quantize.referenceName = directPiece->name;
    quantizedStop.fadeTime = 10.0f;
    stopAction->comp<VNX::VAction>()->steps = {immediatePlay, quantizedStop};
    addChild(actionsRoot, stopAction);

    struct DelayedCompositeCase {
        QString name;
        uint32_t objectId;
        std::shared_ptr<VNX::ProjectItem> target;
        harkenstudiobuild::ActionTargetKindV1 previewKind;
        harken::ActionTargetKindV1 runtimeKind;
    };
    const std::vector<DelayedCompositeCase> delayedCompositeCases = {
        {QStringLiteral("ExportedPackDelayedTrackStop"), 65, directTrack,
         harkenstudiobuild::ActionTargetKindV1::Track,
         harken::ActionTargetKindV1::Track},
        {QStringLiteral("ExportedPackDelayedPieceStop"), 66, directPiece,
         harkenstudiobuild::ActionTargetKindV1::Piece,
         harken::ActionTargetKindV1::Piece},
    };
    for (const auto& test : delayedCompositeCases) {
        auto action = makeItem(
            test.name, VNX::ProjectItem::Type::Action, test.objectId);
        VNX::VActionStep stop;
        stop.type = VNX::VActionStep::Type::Stop;
        stop.targetObjectID = test.target->objectID;
        stop.targetName = test.target->name;
        stop.delay = 5.0f;
        stop.fadeTime = 2.0f;
        action->comp<VNX::VAction>()->steps = {
            playStep(*test.target), stop};
        addChild(actionsRoot, action);
    }

    QString previewError;
    const auto preview = VNX::buildCurrentWorldSyncRequest(
        project_, std::nullopt, &previewError);
    ASSERT_NE(nullptr, preview) << previewError.toStdString();

    const QString outputDir = QDir(dir_.path()).filePath(QStringLiteral("quantized-export"));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, outputDir));
    const QJsonObject anchorPack =
        findNamedPack(readPublication(outputDir), QStringLiteral("ExportedPackDirectPiece"));
    ASSERT_FALSE(anchorPack.isEmpty());

    for (const auto& test : delayedCompositeCases) {
        SCOPED_TRACE(test.name.toStdString());
        const auto actionId = harkentest::StudioObjectId(
            test.objectId + kStableIdBias);
        const auto previewAction = std::find_if(
            preview->actions.begin(), preview->actions.end(),
            [&](const auto& action) {
                return action.actionStableId == actionId;
            });
        ASSERT_NE(previewAction, preview->actions.end());
        ASSERT_EQ(previewAction->steps.size(), 2u);
        EXPECT_EQ(previewAction->steps[1].target.kind, test.previewKind);
        EXPECT_FLOAT_EQ(previewAction->steps[1].delayMilliseconds, 5.0f);
        EXPECT_FLOAT_EQ(previewAction->steps[1].fadeMilliseconds, 2.0f);

        LoadedExportedPack loaded;
        ASSERT_NO_FATAL_FAILURE(
            loadExportedPackSession(outputDir, test.name, &loaded));
        const harken::ActionProgramViewV1* packed =
            loaded.session.Engine()->packComposition.FindAction(
                harkentest::ToRuntime(actionId));
        ASSERT_NE(nullptr, packed);
        ASSERT_EQ(packed->stepCount, 2u);
        EXPECT_EQ(packed->steps[1].target.kind, test.runtimeKind);
        EXPECT_EQ(packed->steps[1].delay.domain,
                  harken::AuthoredTimeDomainV1::AbsoluteNanoseconds);
        EXPECT_EQ(packed->steps[1].delay.numerator, INT64_C(5000000));
        EXPECT_EQ(packed->steps[1].delay.denominator, 1u);
        EXPECT_EQ(packed->steps[1].fade.numerator, INT64_C(2000000));
        EXPECT_EQ(packed->steps[1].fade.denominator, 1u);

        const VnActionFiringHandle firing = postGoldAction(actionId);
        VnActionStepResult play{};
        play.structSize = sizeof(play);
        ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 0, &play));
        ASSERT_NE(0u, play.producedPlayableInstance);
        const auto target = loaded.session.Engine()->playableInstances.Classify(
            play.producedPlayableInstance);
        ASSERT_TRUE(target.live);
        const auto* targetRow = findStartRow(
            loaded.session.Engine(), play.producedPlayableInstance);
        ASSERT_NE(nullptr, targetRow);

        VnActionStepResult stop{};
        stop.structSize = sizeof(stop);
        ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 1, &stop));
        EXPECT_EQ(VN_ACTION_STEP_ACTIVE, stop.state);
        EXPECT_EQ(1u, stop.capturedPlayableCount);
        EXPECT_EQ(1u, stop.remainingPlayableCount);
        const harken::EngineAuthoredStopRoot* operation = nullptr;
        for (const auto& candidate :
             loaded.session.Engine()->authoredStopRoots) {
            if (!candidate.committed || !candidate.operation) {
                continue;
            }
            const auto& identity = candidate.operation->Identity();
            if (identity.actionFiring == firing &&
                identity.root == play.producedPlayableInstance) {
                operation = &candidate;
                break;
            }
        }
        ASSERT_NE(nullptr, operation);
        constexpr uint64_t kDelayFrames = 240u;
        constexpr uint64_t kFadeFrames = 96u;
        const auto& identity = operation->operation->Identity();
        EXPECT_EQ(play.producedPlayableInstance, identity.root);
        EXPECT_EQ(target.scheduleEpoch, identity.sourceEpoch);
        EXPECT_EQ(targetRow->startFrame + kDelayFrames,
                  operation->fadeStartFrame);
        EXPECT_EQ(targetRow->startFrame + kDelayFrames + kFadeFrames,
                  identity.stopFrame);

        bool terminal = false;
        VnActionFiringInfo info{};
        info.structSize = sizeof(info);
        for (uint32_t block = 0; block < 128u && !terminal; ++block) {
            ASSERT_EQ(VN_OK, Vn_Update());
            loaded.session.Engine()->nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 1, &stop));
            ASSERT_EQ(VN_OK, Vn_QueryActionFiring(firing, &info));
            terminal = info.state == VN_ACTION_FIRING_TERMINAL_RETAINED;
        }
        EXPECT_TRUE(terminal);
        EXPECT_EQ(VN_ACTION_OUTCOME_SUCCEEDED, info.outcome);
        EXPECT_EQ(0u, stop.remainingPlayableCount);
        EXPECT_FALSE(loaded.session.Engine()->playableInstances.Classify(
            play.producedPlayableInstance).live);
        EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
    }

    for (const auto& test : cases) {
        SCOPED_TRACE(test.name.toStdString());
        const auto previewAction = std::find_if(
            preview->actions.begin(), preview->actions.end(),
            [&](const auto& action) {
                return action.actionStableId ==
                    harkentest::StudioObjectId(test.objectId + kStableIdBias);
            });
        ASSERT_NE(previewAction, preview->actions.end());
        ASSERT_EQ(previewAction->steps.size(), 1u);
        EXPECT_EQ(previewAction->steps[0].quantizeTiming, test.previewTiming);
        EXPECT_EQ(previewAction->steps[0].quantizeReference.stableId,
                  directPiece->runtimeId());

        LoadedExportedPack loaded;
        ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
            outputDir, QStringLiteral("ExportedPackDirectPiece"), &loaded));
        loaded.packBHandle = loaded.contentHandle;
        ASSERT_NE(0u, loaded.packBHandle);
        ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(outputDir, test.name, &loaded));

        const auto actionId =
            harkentest::StudioObjectId(test.objectId + kStableIdBias);
        const harken::ActionProgramViewV1* packed =
            loaded.session.Engine()->packComposition.FindAction(
                harkentest::ToRuntime(actionId));
        ASSERT_NE(nullptr, packed);
        ASSERT_EQ(packed->stepCount, 1u);
        EXPECT_EQ(packed->steps[0].quantizeTiming, test.runtimeTiming);
        EXPECT_EQ(packed->steps[0].quantizeReference.stableId,
                  harkentest::ToRuntime(directPiece->runtimeId()));
        EXPECT_EQ(static_cast<uint32_t>(packed->steps[0].opcode),
                  static_cast<uint32_t>(previewAction->steps[0].opcode));
        EXPECT_EQ(packed->steps[0].target.stableId,
                  harkentest::ToRuntime(previewAction->steps[0].target.stableId));
        EXPECT_FLOAT_EQ(previewAction->steps[0].delayMilliseconds, 5.0f);
        EXPECT_EQ(packed->steps[0].delay.domain,
                  harken::AuthoredTimeDomainV1::AbsoluteNanoseconds);
        EXPECT_EQ(packed->steps[0].delay.numerator, INT64_C(5000000));
        EXPECT_EQ(packed->steps[0].delay.denominator, 1u);

        const VnActionFiringHandle anchorFiring = postGoldAction(
            harkentest::StudioObjectId(kDirectPieceActionObjectId + kStableIdBias));
        const uint64_t anchorInstance = queryProducedInstance(anchorFiring);
        const auto* anchorRow = findStartRow(loaded.session.Engine(), anchorInstance);
        ASSERT_NE(nullptr, anchorRow);
        const uint64_t anchorStartFrame = anchorRow->startFrame;

        const VnActionFiringHandle firing = postGoldAction(actionId);
        VnActionStepResult step{};
        step.structSize = sizeof(step);
        ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 0, &step));
        EXPECT_EQ(VN_ACTION_STEP_AWAITING_MATERIALIZATION, step.state);
        EXPECT_EQ(0u, step.producedPlayableInstance);
        ASSERT_EQ(1u, loaded.session.Engine()->actionDeferredRecords.LiveCount());
        harken::ActionDeferredRecordToken token{};
        harken::ActionDeferredRecord record{};
        harken::ActionDeferredRecordState state =
            harken::ActionDeferredRecordState::Free;
        ASSERT_TRUE(loaded.session.Engine()->actionDeferredRecords.CaptureAt(
            0, &token, &record, &state));
        constexpr uint64_t kFramesPerTick = 25;
        EXPECT_EQ(anchorStartFrame + test.boundaryTicks * kFramesPerTick,
                  record.intendedBoundaryFrame);

        bool materialized = false;
        for (uint32_t block = 0; block < 512 && !materialized; ++block) {
            ASSERT_EQ(VN_OK, Vn_Update());
            loaded.session.Engine()->nullBackend.Pump(1);
            ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 0, &step));
            materialized = step.state == VN_ACTION_STEP_ACTIVE
                && step.producedPlayableInstance != 0;
        }
        EXPECT_TRUE(materialized);
        EXPECT_NE(0u, step.producedPlayableInstance);
        const auto* producedRow = findStartRow(
            loaded.session.Engine(), step.producedPlayableInstance);
        ASSERT_NE(nullptr, producedRow);
        EXPECT_EQ(anchorStartFrame + test.boundaryTicks * kFramesPerTick,
                  producedRow->startFrame);
        const VnResult release = Vn_ReleaseActionFiring(firing);
        EXPECT_TRUE(release == VN_OK || release == VN_IN_USE);
        const VnResult anchorRelease = Vn_ReleaseActionFiring(anchorFiring);
        EXPECT_TRUE(anchorRelease == VN_OK || anchorRelease == VN_IN_USE);
    }

    const auto previewStop = std::find_if(
        preview->actions.begin(), preview->actions.end(),
        [](const auto& action) {
            return action.actionStableId == harkentest::StudioObjectId(
                kQuantizedStopActionObjectId + kStableIdBias);
        });
    ASSERT_NE(previewStop, preview->actions.end());
    ASSERT_EQ(previewStop->steps.size(), 2u);
    EXPECT_EQ(previewStop->steps[1].quantizeTiming,
              harkenstudiobuild::ActionQuantizeTimingV1::ExitMarker);
    EXPECT_EQ(previewStop->steps[1].quantizeReference.stableId,
              directPiece->runtimeId());
    EXPECT_FLOAT_EQ(previewStop->steps[1].fadeMilliseconds, 10.0f);

    LoadedExportedPack loaded;
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        outputDir, QStringLiteral("ExportedPackDirectPiece"), &loaded));
    loaded.packBHandle = loaded.contentHandle;
    ASSERT_NE(0u, loaded.packBHandle);
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        outputDir, QStringLiteral("ExportedPackQuantizedStop"), &loaded));

    const auto stopActionId = harkentest::StudioObjectId(
        kQuantizedStopActionObjectId + kStableIdBias);
    const harken::ActionProgramViewV1* packedStop =
        loaded.session.Engine()->packComposition.FindAction(
            harkentest::ToRuntime(stopActionId));
    ASSERT_NE(nullptr, packedStop);
    ASSERT_EQ(packedStop->stepCount, 2u);
    EXPECT_EQ(static_cast<uint32_t>(packedStop->steps[1].opcode),
              static_cast<uint32_t>(previewStop->steps[1].opcode));
    EXPECT_EQ(packedStop->steps[1].quantizeTiming,
              harken::ActionQuantizeTimingV1::ExitMarker);
    EXPECT_EQ(packedStop->steps[1].fade.numerator, INT64_C(10000000));
    EXPECT_EQ(packedStop->steps[1].fade.denominator, 1u);

    const VnActionFiringHandle anchorFiring = postGoldAction(
        harkentest::StudioObjectId(kDirectPieceActionObjectId + kStableIdBias));
    const uint64_t anchorInstance = queryProducedInstance(anchorFiring);
    const auto* anchorRow = findStartRow(loaded.session.Engine(), anchorInstance);
    ASSERT_NE(nullptr, anchorRow);
    const uint64_t anchorStartFrame = anchorRow->startFrame;

    const VnActionFiringHandle stopFiring = postGoldAction(stopActionId);
    VnActionStepResult immediate{};
    immediate.structSize = sizeof(immediate);
    ASSERT_EQ(VN_OK, Vn_QueryActionStep(stopFiring, 0, &immediate));
    ASSERT_NE(0u, immediate.producedPlayableInstance);
    VnActionStepResult stopped{};
    stopped.structSize = sizeof(stopped);
    ASSERT_EQ(VN_OK, Vn_QueryActionStep(stopFiring, 1, &stopped));
    EXPECT_EQ(VN_ACTION_STEP_AWAITING_MATERIALIZATION, stopped.state);

    constexpr uint64_t kFramesPerTick = 25;
    const uint64_t expectedExit = anchorStartFrame + 2880u * kFramesPerTick;
    ASSERT_EQ(1u, loaded.session.Engine()->actionDeferredRecords.LiveCount());
    harken::ActionDeferredRecordToken token{};
    harken::ActionDeferredRecord record{};
    harken::ActionDeferredRecordState state =
        harken::ActionDeferredRecordState::Free;
    ASSERT_TRUE(loaded.session.Engine()->actionDeferredRecords.CaptureAt(
        0, &token, &record, &state));
    EXPECT_EQ(expectedExit, record.intendedBoundaryFrame);
    constexpr uint64_t kExitFadeFrames = 480u;
    EXPECT_EQ(expectedExit - kExitFadeFrames, record.firstRowFrame);
    EXPECT_EQ(expectedExit, record.lastRowFrame);
    bool stopExecuted = false;
    for (uint32_t block = 0; block < 512 && !stopExecuted; ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        loaded.session.Engine()->nullBackend.Pump(1);
        ASSERT_EQ(VN_OK, Vn_QueryActionStep(stopFiring, 1, &stopped));
        stopExecuted = stopped.state != VN_ACTION_STEP_AWAITING_MATERIALIZATION
            && stopped.capturedPlayableCount >= 1u
            && stopped.remainingPlayableCount == 0u;
    }
    EXPECT_TRUE(stopExecuted);
    EXPECT_EQ(stopped.targetStableId,
              harkentest::ToRuntime(contentSound->runtimeId()));
    EXPECT_GE(stopped.capturedPlayableCount, 1u);
    EXPECT_EQ(0u, stopped.remainingPlayableCount);

    const VnResult stopRelease = Vn_ReleaseActionFiring(stopFiring);
    EXPECT_TRUE(stopRelease == VN_OK || stopRelease == VN_IN_USE);
    const VnResult anchorRelease = Vn_ReleaseActionFiring(anchorFiring);
    EXPECT_TRUE(anchorRelease == VN_OK || anchorRelease == VN_IN_USE);
}

TEST_F(ExportedPackReadOnlyLifecycle, ImmediateSoundRenders)
{
    ASSERT_NO_FATAL_FAILURE(loadAndPostFamily(
        preparedExportedPackDir(),
        QStringLiteral("ExportedPackImmediateSound"), kImmediateSoundActionObjectId));
}

TEST_F(ExportedPackReadOnlyLifecycle, DelayedSoundRenders)
{
    LoadedExportedPack loaded;
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        preparedExportedPackDir(), QStringLiteral("ExportedPackDelayedSound"), &loaded));

    VnPostActionDesc post{};
    post.structSize = sizeof(post);
    post.actionId = harkentest::ToRuntime(harkentest::StudioObjectId(kDelayedSoundActionObjectId + kStableIdBias));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&post.emitter));
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&post, &firing));
    ASSERT_NE(0u, firing);

    harkentest::Capture capture;
    for (uint32_t block = 0; block < 2; ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        loaded.session.Engine()->nullBackend.Pump(1);
        appendCapture(loaded.session.Engine(), &capture);
    }
    EXPECT_FALSE(bufferIsAudible(capture.left));

    bool heard = false;
    for (uint32_t block = 0; block < 32 && !heard; ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        loaded.session.Engine()->nullBackend.Pump(1);
        appendCapture(loaded.session.Engine(), &capture);
        heard = bufferIsAudible(capture.left);
    }
    VnActionStepResult step{};
    step.structSize = sizeof(step);
    ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 0, &step));
    EXPECT_EQ(VN_ACTION_STEP_ACTIVE, step.state);
    EXPECT_NE(0u, step.producedPlayableInstance);
    EXPECT_GE(loaded.session.Engine()->voices.ActiveCount(), 1u);
    HARKENTEST_ASSERT_NON_SILENT(capture.left);
    const VnResult released = Vn_ReleaseActionFiring(firing);
    EXPECT_TRUE(released == VN_OK || released == VN_IN_USE);
}

TEST_F(ExportedPackReadOnlyLifecycle, RandomNestedContainerRenders)
{
    ASSERT_NO_FATAL_FAILURE(loadAndPostFamily(
        preparedExportedPackDir(),
        QStringLiteral("ExportedPackRandomNestedContainer"), kNestedRandomActionObjectId));
}

TEST_F(ExportedPackReadOnlyLifecycle, QueueSoundChildRenders)
{
    ASSERT_NO_FATAL_FAILURE(loadAndPostFamily(
        preparedExportedPackDir(),
        QStringLiteral("ExportedPackQueueSoundChild"), kQueueSoundActionObjectId));
}

TEST_F(ExportedPackReadOnlyLifecycle, QueueTrackChildRenders)
{
    ASSERT_NO_FATAL_FAILURE(loadAndPostFamily(
        preparedExportedPackDir(),
        QStringLiteral("ExportedPackQueueTrackChild"), kQueueTrackActionObjectId));
}

TEST_F(ExportedPackReadOnlyLifecycle, QueuePieceChildRenders)
{
    ASSERT_NO_FATAL_FAILURE(loadAndPostFamily(
        preparedExportedPackDir(),
        QStringLiteral("ExportedPackQueuePieceChild"), kQueuePieceActionObjectId));
}

TEST_F(ExportedPackReadOnlyLifecycle, VariableControlDiscreteRenders)
{
    ASSERT_NO_FATAL_FAILURE(loadAndPostFamily(
        preparedExportedPackDir(),
        QStringLiteral("ExportedPackVariableControlDiscrete"), kDiscreteActionObjectId));
}

TEST_F(ExportedPackReadOnlyLifecycle, VariableControlContinuousRenders)
{
    ASSERT_NO_FATAL_FAILURE(loadAndPostFamily(
        preparedExportedPackDir(),
        QStringLiteral("ExportedPackVariableControlContinuous"), kContinuousActionObjectId));
}

TEST_F(ExportedPackReadOnlyLifecycle, LiveMatchRetargetsFromPlayingSilentContent)
{
    LoadedExportedPack loaded;
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        preparedExportedPackDir(), QStringLiteral("ExportedPackVariableControlInitiallySilent"), &loaded));

    VnPostActionDesc post{};
    post.structSize = sizeof(post);
    post.actionId = harkentest::ToRuntime(harkentest::StudioObjectId(kInitiallySilentActionObjectId + kStableIdBias));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&post.emitter));
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&post, &firing));
    ASSERT_NE(0u, firing);

    VnActionStepResult step{};
    step.structSize = sizeof(step);
    ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 0, &step));
    EXPECT_EQ(VN_ACTION_STEP_ACTIVE, step.state);
    EXPECT_NE(0u, step.producedPlayableInstance);
    EXPECT_EQ(1u, loaded.session.Engine()->voices.ActiveCount());

    harkentest::Capture silentCapture;
    ASSERT_NO_FATAL_FAILURE(pumpCapture(loaded.session.Engine(), &silentCapture, 4));
    EXPECT_FALSE(bufferIsAudible(silentCapture.left));

    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ToRuntime(harkentest::StudioObjectId(kStepVariableObjectId + kStableIdBias)), 2.0f));
    harkentest::AdvanceVariableWrite(loaded.session.Engine());

    harkentest::Capture capture;
    bool heard = false;
    for (uint32_t block = 0; block < 64 && !heard; ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        loaded.session.Engine()->nullBackend.Pump(1);
        appendCapture(loaded.session.Engine(), &capture);
        heard = loaded.session.Engine()->voices.ActiveCount() >= 1u
            && bufferIsAudible(capture.left);
    }
    EXPECT_GE(loaded.session.Engine()->voices.ActiveCount(), 1u);
    HARKENTEST_ASSERT_NON_SILENT(capture.left);
    const VnResult released = Vn_ReleaseActionFiring(firing);
    EXPECT_TRUE(released == VN_OK || released == VN_IN_USE);
}

TEST_F(ExportedPackReadOnlyLifecycle, UnmatchedLiveMatchCompletesWithoutReviving)
{
    LoadedExportedPack loaded;
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        preparedExportedPackDir(), QStringLiteral("ExportedPackVariableControlInitiallySilent"), &loaded));
    auto* engine = loaded.session.Engine();
    const auto variableId = harkentest::ToRuntime(
        harkentest::StudioObjectId(kStepVariableObjectId + kStableIdBias));
    ASSERT_EQ(VN_OK, Vn_SetValue(variableId, 3.0f));
    harkentest::AdvanceVariableWrite(engine);
    const auto firing = postGoldAction(
        harkentest::StudioObjectId(kInitiallySilentActionObjectId + kStableIdBias));
    ASSERT_NE(firing, 0u);
    ASSERT_NO_FATAL_FAILURE(drainPlayableAndFiring(engine, firing));
    EXPECT_EQ(engine->playableInstances.LiveCount(), 0u);
    EXPECT_EQ(engine->voices.ActiveCount(), 0u);

    ASSERT_EQ(VN_OK, Vn_SetValue(variableId, 2.0f));
    harkentest::AdvanceVariableWrite(engine);
    harkentest::Capture capture;
    ASSERT_NO_FATAL_FAILURE(pumpCapture(engine, &capture, 16));
    EXPECT_FALSE(bufferIsAudible(capture.left));
    EXPECT_EQ(engine->playableInstances.LiveCount(), 0u);
    EXPECT_EQ(engine->voices.ActiveCount(), 0u);
}

TEST_F(ExportedPackReadOnlyLifecycle, DirectTrackRenders)
{
    ASSERT_NO_FATAL_FAILURE(loadAndPostFamily(
        preparedExportedPackDir(),
        QStringLiteral("ExportedPackDirectTrack"), kDirectTrackActionObjectId));
}

TEST_F(ExportedPackReadOnlyLifecycle, DirectPieceRenders)
{
    ASSERT_NO_FATAL_FAILURE(loadAndPostFamily(
        preparedExportedPackDir(),
        QStringLiteral("ExportedPackDirectPiece"), kDirectPieceActionObjectId));
}

TEST_F(ExportedPackReadOnlyLifecycle, SharedLeafFromContentRootRenders)
{
    ASSERT_NO_FATAL_FAILURE(loadAndPostFamily(
        preparedExportedPackDir(),
        QStringLiteral("ExportedPackSharedLeafFromContentRoot"), kSharedLeafActionObjectId));
}

TEST_F(ExportedPackReadOnlyLifecycle, TwoPlayStepsDifferentPacksRenders)
{
    LoadedExportedPack loaded;
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        preparedExportedPackDir(), QStringLiteral("ExportedPackTwoPlayStepsDifferentPacks"), &loaded));

    VnPostActionDesc post{};
    post.structSize = sizeof(post);
    post.actionId = harkentest::ToRuntime(harkentest::StudioObjectId(kTwoPlayActionObjectId + kStableIdBias));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&post.emitter));
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&post, &firing));
    ASSERT_NE(0u, firing);

    VnActionStepResult first{};
    first.structSize = sizeof(first);
    ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 0, &first));
    VnActionStepResult second{};
    second.structSize = sizeof(second);
    ASSERT_EQ(VN_OK, Vn_QueryActionStep(firing, 1, &second));
    EXPECT_EQ(VN_ACTION_STEP_ACTIVE, first.state);
    EXPECT_EQ(VN_ACTION_STEP_ACTIVE, second.state);
    EXPECT_NE(0u, first.producedPlayableInstance);
    EXPECT_NE(0u, second.producedPlayableInstance);
    EXPECT_NE(first.producedPlayableInstance, second.producedPlayableInstance);
    EXPECT_GE(loaded.session.Engine()->voices.ActiveCount(), 2u);

    harkentest::Capture capture;
    ASSERT_NO_FATAL_FAILURE(pumpCapture(loaded.session.Engine(), &capture, 16));
    HARKENTEST_ASSERT_NON_SILENT(capture.left);
    ASSERT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
}

TEST_F(ExportedPackReadOnlyLifecycle, DefaultUnloadKeepsLivePlayableOnOldRevision)
{
    LoadedExportedPack loaded;
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        preparedExportedPackDir(), QStringLiteral("ExportedPackImmediateSound"), &loaded));
    const VnActionFiringHandle firing =
        postGoldAction(harkentest::StudioObjectId(kImmediateSoundActionObjectId + kStableIdBias));
    const auto instance = queryProducedInstance(firing);
    FrozenPlayableOwnership frozen{};
    ASSERT_NO_FATAL_FAILURE(captureOwnership(loaded.session.Engine(), instance, &frozen));
    harkentest::Capture before;
    ASSERT_NO_FATAL_FAILURE(pumpUntilAudible(loaded.session.Engine(), &before));

    ASSERT_EQ(VN_OK, Vn_UnloadPack(loaded.contentHandle));
    ASSERT_NO_FATAL_FAILURE(expectFrozenOwnership(loaded.session.Engine(), frozen));
    harkentest::Capture after;
    ASSERT_NO_FATAL_FAILURE(pumpUntilAudible(loaded.session.Engine(), &after));
    const VnResult released = Vn_ReleaseActionFiring(firing);
    EXPECT_TRUE(released == VN_OK || released == VN_IN_USE);
}

TEST_F(ExportedPackLifecycle, ReplaceKeepsLivePlayableOnOldRevision)
{
    const QString nextDir = QDir(dir_.path()).filePath(QStringLiteral("exported-pack-replace-keep-next"));
    LoadedExportedPack loaded;
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        preparedExportedPackDir(), QStringLiteral("ExportedPackImmediateSound"), &loaded));
    ASSERT_NO_FATAL_FAILURE(exportProjectPackSuccessor(project_, dir_.path(), nextDir));
    const VnActionFiringHandle firing =
        postGoldAction(harkentest::StudioObjectId(kImmediateSoundActionObjectId + kStableIdBias));
    const auto instance = queryProducedInstance(firing);
    FrozenPlayableOwnership frozen{};
    ASSERT_NO_FATAL_FAILURE(captureOwnership(loaded.session.Engine(), instance, &frozen));
    harkentest::Capture before;
    ASSERT_NO_FATAL_FAILURE(pumpUntilAudible(loaded.session.Engine(), &before));
    EXPECT_NEAR(kToneAmplitude, harkentest::Peak(before.left), 0.05f);

    VnPackLoadDesc nextDesc{};
    ASSERT_NO_FATAL_FAILURE(makeRootReplaceDesc(
        &loaded, nextDir, QStringLiteral("ExportedPackImmediateSound"), &nextDesc));
    ASSERT_EQ(VN_OK, Vn_ReplacePack(loaded.contentHandle,
                                    reinterpret_cast<const VnPackDesc*>(&nextDesc)));
    ASSERT_NO_FATAL_FAILURE(expectFrozenOwnership(loaded.session.Engine(), frozen));
    harkentest::Capture after;
    ASSERT_NO_FATAL_FAILURE(pumpUntilAudible(loaded.session.Engine(), &after));
    EXPECT_NEAR(kToneAmplitude, harkentest::Peak(after.left), 0.05f);
    const VnResult released = Vn_ReleaseActionFiring(firing);
    EXPECT_TRUE(released == VN_OK || released == VN_IN_USE);
}

TEST_F(ExportedPackReadOnlyLifecycle, RefuseIfUsedCoversActionFiringOwner)
{
    LoadedExportedPack loaded;
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        preparedExportedPackDir(), QStringLiteral("ExportedPackImmediateSound"), &loaded));
    const VnActionFiringHandle firing =
        postGoldAction(harkentest::StudioObjectId(kImmediateSoundActionObjectId + kStableIdBias));
    EXPECT_EQ(VN_IN_USE,
              Vn_UnloadPackEx(loaded.contentHandle, VN_PACK_UNLOAD_REFUSE_IF_USED));
    EXPECT_GE(loaded.session.Engine()->voices.ActiveCount(), 1u);
    EXPECT_EQ(VN_IN_USE, Vn_ReleaseActionFiring(firing));
    ASSERT_NO_FATAL_FAILURE(drainPlayableAndFiring(loaded.session.Engine(), firing));
    EXPECT_EQ(VN_OK,
              Vn_UnloadPackEx(loaded.contentHandle, VN_PACK_UNLOAD_REFUSE_IF_USED));
}

TEST_F(AuthoredPackLifecycle, RefuseIfUsedCoversDeferredTarget)
{
    for (auto& binding : project_.actions) {
        if (!binding.rootItem)
            continue;
        for (const auto& child : binding.rootItem->children) {
            if (!child || child->objectID != kDelayedSoundActionObjectId)
                continue;
            auto* action = child->comp<VNX::VAction>();
            ASSERT_NE(nullptr, action);
            ASSERT_FALSE(action->steps.empty());
            action->steps.front().delay = kDeferredRefusePlayMilliseconds;
        }
    }
    const QString exportedPackDir = QDir(dir_.path()).filePath(QStringLiteral("exported-pack-refuse-deferred"));
    LoadedExportedPack loaded;
    ASSERT_NO_FATAL_FAILURE(exportAndLoadContent(
        &loaded, project_, exportedPackDir, QStringLiteral("ExportedPackDelayedSound")));
    const VnActionFiringHandle firing =
        postGoldAction(harkentest::StudioObjectId(kDelayedSoundActionObjectId + kStableIdBias));
    EXPECT_EQ(1u, loaded.session.Engine()->actionDeferredRecords.LiveCount());
    EXPECT_EQ(VN_IN_USE,
              Vn_UnloadPackEx(loaded.contentHandle, VN_PACK_UNLOAD_REFUSE_IF_USED));
    ASSERT_NO_FATAL_FAILURE(drainPlayableAndFiring(loaded.session.Engine(), firing));
    EXPECT_EQ(VN_OK,
              Vn_UnloadPackEx(loaded.contentHandle, VN_PACK_UNLOAD_REFUSE_IF_USED));
}

TEST_F(ExportedPackReadOnlyLifecycle, RefuseIfUsedCoversLivePlayable)
{
    LoadedExportedPack loaded;
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        preparedExportedPackDir(), QStringLiteral("ExportedPackImmediateSound"), &loaded));
    const VnActionFiringHandle firing =
        postGoldAction(harkentest::StudioObjectId(kImmediateSoundActionObjectId + kStableIdBias));
    EXPECT_NE(0u, queryProducedInstance(firing));
    EXPECT_EQ(VN_IN_USE,
              Vn_UnloadPackEx(loaded.contentHandle, VN_PACK_UNLOAD_REFUSE_IF_USED));
    EXPECT_GE(loaded.session.Engine()->playableInstances.LiveCount(), 1u);
    EXPECT_EQ(VN_IN_USE, Vn_ReleaseActionFiring(firing));
    ASSERT_NO_FATAL_FAILURE(drainPlayableAndFiring(loaded.session.Engine(), firing));
    EXPECT_EQ(VN_OK,
              Vn_UnloadPackEx(loaded.contentHandle, VN_PACK_UNLOAD_REFUSE_IF_USED));
}

TEST_F(ExportedPackLifecycle, QueueContinuationKeepsOldRevisionUntilRetirement)
{
    const QString nextDir = QDir(dir_.path()).filePath(QStringLiteral("exported-pack-queue-keep-next"));
    LoadedExportedPack loaded;
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        preparedExportedPackDir(), QStringLiteral("ExportedPackQueueSoundChild"), &loaded));
    ASSERT_NO_FATAL_FAILURE(exportProjectPackSuccessor(project_, dir_.path(), nextDir));
    const VnActionFiringHandle firing =
        postGoldAction(harkentest::StudioObjectId(kQueueSoundActionObjectId + kStableIdBias));
    const auto instance = queryProducedInstance(firing);
    FrozenPlayableOwnership frozen{};
    ASSERT_NO_FATAL_FAILURE(captureOwnership(loaded.session.Engine(), instance, &frozen));
    harkentest::Capture first;
    ASSERT_NO_FATAL_FAILURE(pumpUntilAudible(loaded.session.Engine(), &first));

    VnPackLoadDesc nextDesc{};
    ASSERT_NO_FATAL_FAILURE(makeRootReplaceDesc(
        &loaded, nextDir, QStringLiteral("ExportedPackQueueSoundChild"), &nextDesc));
    ASSERT_EQ(VN_OK, Vn_ReplacePack(loaded.contentHandle,
                                    reinterpret_cast<const VnPackDesc*>(&nextDesc)));
    ASSERT_NO_FATAL_FAILURE(expectFrozenOwnership(loaded.session.Engine(), frozen));
    harkentest::Capture after;
    ASSERT_NO_FATAL_FAILURE(pumpUntilAudible(loaded.session.Engine(), &after));
    const VnResult released = Vn_ReleaseActionFiring(firing);
    EXPECT_TRUE(released == VN_OK || released == VN_IN_USE);
}

TEST_F(ExportedPackLifecycle, LiveMatchRetargetingKeepsOldRevisionUntilRetirement)
{
    const QString nextDir = QDir(dir_.path()).filePath(QStringLiteral("exported-pack-vc-keep-next"));
    LoadedExportedPack loaded;
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        preparedExportedPackDir(), QStringLiteral("ExportedPackVariableControlInitiallySilent"), &loaded));
    ASSERT_NO_FATAL_FAILURE(exportProjectPackSuccessor(project_, dir_.path(), nextDir));
    const VnActionFiringHandle firing =
        postGoldAction(harkentest::StudioObjectId(kInitiallySilentActionObjectId + kStableIdBias));
    const auto instance = queryProducedInstance(firing);
    FrozenPlayableOwnership frozen{};
    ASSERT_NO_FATAL_FAILURE(captureOwnership(loaded.session.Engine(), instance, &frozen));
    EXPECT_EQ(1u, loaded.session.Engine()->voices.ActiveCount());

    VnPackLoadDesc nextDesc{};
    ASSERT_NO_FATAL_FAILURE(makeRootReplaceDesc(
        &loaded, nextDir, QStringLiteral("ExportedPackVariableControlInitiallySilent"),
        &nextDesc));
    ASSERT_EQ(VN_OK, Vn_ReplacePack(loaded.contentHandle,
                                    reinterpret_cast<const VnPackDesc*>(&nextDesc)));
    ASSERT_NO_FATAL_FAILURE(expectFrozenOwnership(loaded.session.Engine(), frozen));

    ASSERT_EQ(VN_OK, Vn_SetValue(harkentest::ToRuntime(harkentest::StudioObjectId(kStepVariableObjectId + kStableIdBias)), 2.0f));
    harkentest::AdvanceVariableWrite(loaded.session.Engine());
    ASSERT_NO_FATAL_FAILURE(expectFrozenOwnership(loaded.session.Engine(), frozen));
    harkentest::Capture capture;
    ASSERT_NO_FATAL_FAILURE(pumpUntilAudible(loaded.session.Engine(), &capture));
    EXPECT_NEAR(kToneAmplitude, harkentest::Peak(capture.left), 0.05f);
    const VnResult released = Vn_ReleaseActionFiring(firing);
    EXPECT_TRUE(released == VN_OK || released == VN_IN_USE);
}

TEST_F(ExportedPackLifecycle, InitVariablesRemainReadableAcrossContentReplace)
{
    const QString nextDir = QDir(dir_.path()).filePath(QStringLiteral("exported-pack-init-replace-next"));
    LoadedExportedPack loaded;
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        preparedExportedPackDir(), QStringLiteral("ExportedPackImmediateSound"), &loaded));
    ASSERT_NO_FATAL_FAILURE(exportProjectPackSuccessor(project_, dir_.path(), nextDir));
    ASSERT_NO_FATAL_FAILURE(expectInitVariablesReadable(loaded.session.Engine()));
    VnPackLoadDesc nextDesc{};
    ASSERT_NO_FATAL_FAILURE(makeRootReplaceDesc(
        &loaded, nextDir, QStringLiteral("ExportedPackImmediateSound"), &nextDesc));
    ASSERT_EQ(VN_OK, Vn_ReplacePack(loaded.contentHandle,
                                    reinterpret_cast<const VnPackDesc*>(&nextDesc)));
    ASSERT_NO_FATAL_FAILURE(expectInitVariablesReadable(loaded.session.Engine()));
}

TEST_F(ExportedPackReadOnlyLifecycle, InitVariablesRemainReadableAcrossContentUnload)
{
    LoadedExportedPack loaded;
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        preparedExportedPackDir(), QStringLiteral("ExportedPackImmediateSound"), &loaded));
    ASSERT_NO_FATAL_FAILURE(expectInitVariablesReadable(loaded.session.Engine()));
    ASSERT_EQ(VN_OK, Vn_UnloadPack(loaded.contentHandle));
    ASSERT_NO_FATAL_FAILURE(expectInitVariablesReadable(loaded.session.Engine()));
}

TEST_F(ExportedPackLifecycle, NewPostAfterReplaceSelectsSuccessor)
{
    const QString nextDir = QDir(dir_.path()).filePath(QStringLiteral("exported-pack-successor-next"));
    LoadedExportedPack loaded;
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        preparedExportedPackDir(), QStringLiteral("ExportedPackImmediateSound"), &loaded));
    ASSERT_NO_FATAL_FAILURE(exportProjectPackSuccessor(project_, dir_.path(), nextDir));
    const VnActionFiringHandle oldFiring =
        postGoldAction(harkentest::StudioObjectId(kImmediateSoundActionObjectId + kStableIdBias));
    const auto oldInstance = queryProducedInstance(oldFiring);
    FrozenPlayableOwnership frozen{};
    ASSERT_NO_FATAL_FAILURE(captureOwnership(loaded.session.Engine(), oldInstance, &frozen));
    harkentest::Capture oldCapture;
    ASSERT_NO_FATAL_FAILURE(pumpUntilAudible(loaded.session.Engine(), &oldCapture));
    EXPECT_NEAR(kToneAmplitude, harkentest::Peak(oldCapture.left), 0.05f);

    VnPackLoadDesc nextDesc{};
    ASSERT_NO_FATAL_FAILURE(makeRootReplaceDesc(
        &loaded, nextDir, QStringLiteral("ExportedPackImmediateSound"), &nextDesc));
    ASSERT_EQ(VN_OK, Vn_ReplacePack(loaded.contentHandle,
                                    reinterpret_cast<const VnPackDesc*>(&nextDesc)));
    ASSERT_NO_FATAL_FAILURE(expectFrozenOwnership(loaded.session.Engine(), frozen));
    ASSERT_NO_FATAL_FAILURE(drainPlayableAndFiring(loaded.session.Engine(), oldFiring));

    const VnActionFiringHandle nextFiring =
        postGoldAction(harkentest::StudioObjectId(kImmediateSoundActionObjectId + kStableIdBias));
    const auto nextInstance = queryProducedInstance(nextFiring);
    EXPECT_NE(oldInstance, nextInstance);
    harkentest::Capture nextCapture;
    ASSERT_NO_FATAL_FAILURE(pumpUntilAudible(loaded.session.Engine(), &nextCapture));
    EXPECT_NEAR(kSuccessorToneAmplitude, harkentest::Peak(nextCapture.left), 0.08f);
    const VnResult releasedNext = Vn_ReleaseActionFiring(nextFiring);
    EXPECT_TRUE(releasedNext == VN_OK || releasedNext == VN_IN_USE);
}

TEST_F(ExportedPackLifecycle, PackBMutationLeavesPackAStateUnchanged)
{
    const QString nextDir = QDir(dir_.path()).filePath(QStringLiteral("exported-pack-packb-next"));
    LoadedExportedPack loaded;
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        preparedExportedPackDir(), QStringLiteral("ExportedPackImmediateSound"), &loaded));
    ASSERT_NO_FATAL_FAILURE(loadExportedPackB(preparedExportedPackDir(), &loaded));
    ASSERT_NO_FATAL_FAILURE(exportProjectPackSuccessor(project_, dir_.path(), nextDir));
    const VnActionFiringHandle firing =
        postGoldAction(harkentest::StudioObjectId(kImmediateSoundActionObjectId + kStableIdBias));
    const auto instance = queryProducedInstance(firing);
    FrozenPlayableOwnership frozen{};
    ASSERT_NO_FATAL_FAILURE(captureOwnership(loaded.session.Engine(), instance, &frozen));
    harkentest::Capture before;
    ASSERT_NO_FATAL_FAILURE(pumpUntilAudible(loaded.session.Engine(), &before));

    VnPackLoadDesc nextDesc{};
    ASSERT_NO_FATAL_FAILURE(makeRootReplaceDesc(
        &loaded, nextDir, QStringLiteral("ExportedPackPackBAction"), &nextDesc));
    ASSERT_EQ(VN_OK, Vn_ReplacePack(loaded.packBHandle,
                                    reinterpret_cast<const VnPackDesc*>(&nextDesc)));
    ASSERT_NO_FATAL_FAILURE(expectFrozenOwnership(loaded.session.Engine(), frozen));
    const auto* row = findStartRow(loaded.session.Engine(), instance);
    ASSERT_NE(nullptr, row);
    EXPECT_EQ(frozen.selectedChildOrdinal,
              row->selectionCount == 0 ? 0u : row->selectedChildOrdinals[0]);
    EXPECT_EQ(frozen.leaf.stableId, row->leaf.stableId);
    EXPECT_GE(loaded.session.Engine()->voices.ActiveCount(), 1u);
    harkentest::Capture after;
    ASSERT_NO_FATAL_FAILURE(pumpUntilAudible(loaded.session.Engine(), &after));
    const VnResult released = Vn_ReleaseActionFiring(firing);
    EXPECT_TRUE(released == VN_OK || released == VN_IN_USE);
}

TEST_F(ExportedPackReadOnlyLifecycle, RetirementReleasesClosureAndReusesSlot)
{
    LoadedExportedPack loaded;
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        preparedExportedPackDir(), QStringLiteral("ExportedPackImmediateSound"), &loaded));
    const VnPackHandle firstHandle = loaded.contentHandle;
    const std::uint32_t firstGeneration =
        static_cast<std::uint32_t>(firstHandle >> 32);
    const VnActionFiringHandle firing =
        postGoldAction(harkentest::StudioObjectId(kImmediateSoundActionObjectId + kStableIdBias));
    FrozenPlayableOwnership frozen{};
    ASSERT_NO_FATAL_FAILURE(
        captureOwnership(loaded.session.Engine(), queryProducedInstance(firing), &frozen));
    ASSERT_NO_FATAL_FAILURE(drainPlayableAndFiring(loaded.session.Engine(), firing));
    ASSERT_EQ(VN_OK, Vn_UnloadPack(loaded.contentHandle));
    for (uint32_t block = 0; block < 64; ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        loaded.session.Engine()->nullBackend.Pump(1);
        if (loaded.session.Engine()->packs.RetiringCount() == 0)
            break;
    }
    EXPECT_EQ(0u, loaded.session.Engine()->packs.RetiringCount());
    EXPECT_EQ(VN_STALE_HANDLE,
              loaded.session.Engine()->packComposition.ValidateLease(frozen.packLease));

    const QJsonObject contentPack =
        findNamedPack(readPublication(preparedExportedPackDir()),
                      QStringLiteral("ExportedPackImmediateSound"));
    ASSERT_FALSE(contentPack.isEmpty());
    ExportedPackActivation prepared;
    ASSERT_NO_FATAL_FAILURE(prepareExportedPackActivation(
        preparedExportedPackDir(), QStringLiteral("ExportedPackImmediateSound"),
        loaded, &prepared));
    for (auto& member : prepared.members) {
        if (member.packStableId == frozen.packLease.packStableId)
            member.expectedHandle = 0;
    }
    // 原生 Content 卸载清空集合 ledger，但保留 Init 和其他存活 owner。
    VnCanonicalPackSetActivationReportV1 activation{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(
        prepared.members.data(), static_cast<uint32_t>(prepared.members.size()),
        prepared.rows.data(), activation, loaded.initHandle, 0));
    ASSERT_NO_FATAL_FAILURE(acceptExportedPackActivation(&prepared, activation, &loaded));
    const VnPackHandle reused = exportedMemberHandle(loaded, frozen.packLease.packStableId);
    loaded.contentHandle = reused;
    ASSERT_NE(0u, reused);
    EXPECT_NE(firstHandle, reused);
    EXPECT_NE(firstGeneration, static_cast<std::uint32_t>(reused >> 32));
    EXPECT_NE(frozen.packLease.leaseToken,
              loaded.session.Engine()->packComposition.Lookup(
                  harkentest::ObjectId(kImmediateSoundActionObjectId + kStableIdBias)).lease.leaseToken);
}

TEST_F(ExportedPackLifecycle, TraceActiveActivationPublishesAndJournals)
{
    const QString journalPath = QDir(dir_.path()).filePath(QStringLiteral("exported-pack-trace.vtrc"));
    LoadedExportedPack loaded;
    ASSERT_EQ(VN_OK, loaded.session.InitResult());
    loaded.rootUtf8 = QFileInfo(preparedExportedPackDir()).absoluteFilePath().toUtf8();
    ExportedPackActivation prepared;
    ASSERT_NO_FATAL_FAILURE(prepareExportedPackActivation(
        preparedExportedPackDir(), QStringLiteral("ExportedPackVerticalContainer"),
        loaded, &prepared));
    auto* engine = loaded.session.Engine();
    ASSERT_NE(nullptr, engine);
    const auto before = engine->packComposition.PublishedHandle();
    const QByteArray journalUtf8 = journalPath.toUtf8();
    ASSERT_EQ(VN_OK, Vn_TraceStart(journalUtf8.constData()));
    VnCanonicalPackSetActivationReportV1 activation{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(
        prepared.members.data(), static_cast<uint32_t>(prepared.members.size()),
        prepared.rows.data(), activation));
    EXPECT_NE(before.get(), engine->packComposition.PublishedHandle().get());
    EXPECT_NE(0u, activation.initHandle);
    for (const auto& row : prepared.rows)
        EXPECT_NE(0u, row.handle);
    ASSERT_EQ(VN_OK, Vn_TraceStop());
    const auto records = readGoldJournal(journalPath);
    size_t activationCallCount = 0;
    for (const auto& record : records) {
        const auto callId = goldJournalCallId(record);
        if (callId == harken::kTraceCallActivateCanonicalPackSet)
            ++activationCallCount;
        EXPECT_NE(harken::kTraceCallLoadCurrentWorld, callId);
        EXPECT_NE(harken::kTraceCallLoadPack, callId);
        EXPECT_NE(harken::kTraceCallReplacePack, callId);
        EXPECT_NE(harken::kTraceCallUnloadPack, callId);
        EXPECT_NE(harken::kTraceCallPostAction, callId);
    }
    EXPECT_EQ(1u, activationCallCount);
}

TEST_F(ExportedPackLifecycle, ActivationKeepsOldRevisionAcrossReplaceWithoutTrace)
{
    const QString nextDir =
        QDir(dir_.path()).filePath(QStringLiteral("exported-pack-activation-keep-next"));
    ASSERT_NO_FATAL_FAILURE(runGoldLifecycleWithoutTrace(
        project_, dir_.path(), preparedExportedPackDir(), nextDir));
}

TEST_F(ExportedPackReadOnlyLifecycle, PackBootstrapPostsVerticalContainer)
{
    LoadedExportedPack loaded;
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        preparedExportedPackDir(), QStringLiteral("ExportedPackVerticalContainer"), &loaded));
    harkentest::Capture capture;
    ASSERT_NO_FATAL_FAILURE(postAndCaptureImmediate(
        &loaded, harkentest::StudioObjectId(kVerticalContainerActionObjectId + kStableIdBias), &capture, 1u));
}

TEST_F(ExportedPackLifecycle, HostAtt04NearFarPostsOfficialExportInTwoProcesses)
{
    const std::string root =
        QFileInfo(preparedExportedPackDir()).absoluteFilePath().toStdString();
    const auto runDistance = [&](float distance) {
        return runHarkenHost(
            std::string("att04-nearfar --null --seconds 0.4 --root ") + root +
            " --action-name ExportedPackSpatialNearFar --distance " +
            std::to_string(distance));
    };
    const GoldHostRun nearRun = runDistance(kAtt04NearDistance);
    const GoldHostRun farRun = runDistance(kAtt04FarDistance);
    ASSERT_EQ(0, nearRun.exitCode) << nearRun.output;
    ASSERT_EQ(0, farRun.exitCode) << farRun.output;
    const QRegularExpression line(
        QStringLiteral(
            R"(^att04-nearfar distance=([0-9.]+) peak=([0-9.]+) firing=([0-9]+) VnResult=([0-9-]+)\r?$)"),
        QRegularExpression::MultilineOption);
    const auto parsePeak = [&](const GoldHostRun& run, float expectedDistance) {
        const QString output = QString::fromStdString(run.output);
        const auto match = line.match(output);
        EXPECT_TRUE(match.hasMatch()) << run.output;
        bool validDistance = false;
        const float distance = match.captured(1).toFloat(&validDistance);
        EXPECT_TRUE(validDistance) << run.output;
        EXPECT_NEAR(expectedDistance, distance, 0.001f) << run.output;
        bool validPeak = false;
        const float peak = match.captured(2).toFloat(&validPeak);
        EXPECT_TRUE(validPeak) << run.output;
        bool validFiring = false;
        const auto firing = match.captured(3).toULongLong(&validFiring);
        EXPECT_TRUE(validFiring) << run.output;
        EXPECT_NE(0ull, firing) << run.output;
        EXPECT_EQ(QStringLiteral("0"), match.captured(4)) << run.output;
        EXPECT_GT(peak, harkentest::kSilenceFloor) << run.output;
        return peak;
    };
    const float nearPeak = parsePeak(nearRun, kAtt04NearDistance);
    const float farPeak = parsePeak(farRun, kAtt04FarDistance);
    EXPECT_GT(nearPeak, farPeak * 2.0f)
        << "near=" << nearPeak << " far=" << farPeak << "\n"
        << nearRun.output << "\n"
        << farRun.output;
}

TEST_F(ExportedPackLifecycle, HostPackPlayNullPostsVerticalContainer)
{
    const GoldHostRun run = runHarkenHost(
        std::string("pack-play --null --seconds 0.2 --root ") +
        QFileInfo(preparedExportedPackDir()).absoluteFilePath().toStdString() + " --action-name ExportedPackVerticalContainer");
    ASSERT_EQ(0, run.exitCode) << run.output;
    EXPECT_NE(std::string::npos, run.output.find("non-silent")) << run.output;
    EXPECT_EQ(std::string::npos, run.output.find("LoadCurrentWorld"))
        << run.output;
}

TEST_F(ExportedPackLifecycle, HostPackPlayNullRepeatsInOneSession)
{
    const GoldHostRun run = runHarkenHost(
        std::string("pack-play --null --no-recovery --repeats 2 ") +
        "--caller-seed 5 --seconds 0.2 --root " +
        QFileInfo(preparedExportedPackDir()).absoluteFilePath().toStdString() + " --action-name ExportedPackVerticalContainer");
    ASSERT_EQ(0, run.exitCode) << run.output;
    const QString output = QString::fromStdString(run.output);
    ASSERT_EQ(2, output.count(QStringLiteral("pack-play trigger "))) << run.output;
    const QRegularExpression triggerLine(
        QStringLiteral(R"(^pack-play trigger ([0-9]+)/([0-9]+) peak=([^ ]+) firing=([0-9]+)\r?$)"),
        QRegularExpression::MultilineOption);
    auto triggers = triggerLine.globalMatch(output);
    VnActionFiringHandle previousFiring = 0;
    for (int expectedTrigger = 1; expectedTrigger <= 2; ++expectedTrigger) {
        ASSERT_TRUE(triggers.hasNext()) << run.output;
        const auto trigger = triggers.next();
        EXPECT_EQ(expectedTrigger, trigger.captured(1).toInt()) << run.output;
        EXPECT_EQ(2, trigger.captured(2).toInt()) << run.output;
        bool validPeak = false;
        const float peak = trigger.captured(3).toFloat(&validPeak);
        ASSERT_TRUE(validPeak) << run.output;
        EXPECT_GT(peak, harkentest::kSilenceFloor) << run.output;
        bool validFiring = false;
        const VnActionFiringHandle firing = trigger.captured(4).toULongLong(&validFiring);
        ASSERT_TRUE(validFiring) << run.output;
        EXPECT_NE(0u, firing) << run.output;
        EXPECT_NE(previousFiring, firing) << run.output;
        previousFiring = firing;
    }
    EXPECT_FALSE(triggers.hasNext()) << run.output;
}

TEST_F(AuthoredPackLifecycle, DISABLED_HostCaptureReadyGateOrdersFirstTrigger)
{
    const QString exportedPackDir =
        QDir(dir_.path()).filePath(QStringLiteral("exported-pack-capture-ready"));
    ASSERT_TRUE(QDir().mkpath(exportedPackDir));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, exportedPackDir));

    const QString staleGate =
        QDir(dir_.path()).filePath(QStringLiteral("stale.capture-ready"));
    QFile stale(staleGate);
    ASSERT_TRUE(stale.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(13, stale.write("process_id=1\n"));
    stale.close();
    const GoldHostRun staleRun = runHarkenHost(
        std::string("pack-play --null --no-recovery --seconds 0.2 --root \"") +
        QFileInfo(exportedPackDir).absoluteFilePath().toStdString() + "\" --action-name ExportedPackImmediateSound --capture-ready-gate \"" +
        staleGate.toStdString() + "\" --capture-ready-timeout-ms 1000");
    EXPECT_NE(0, staleRun.exitCode) << staleRun.output;
    EXPECT_NE(std::string::npos,
              staleRun.output.find("capture-ready gate must not preexist"))
        << staleRun.output;
    EXPECT_EQ(std::string::npos, staleRun.output.find("pack-play trigger "))
        << staleRun.output;

    const QString gate =
        QDir(dir_.path()).filePath(QStringLiteral("fresh.capture-ready"));
    QProcess host;
    host.setProcessChannelMode(QProcess::MergedChannels);
    host.setProgram(QString::fromUtf8(HARKEN_HOST_PATH));
    host.setArguments({
        QStringLiteral("pack-play"), QStringLiteral("--null"),
        QStringLiteral("--no-recovery"), QStringLiteral("--seconds"),
        QStringLiteral("0.2"), QStringLiteral("--root"), exportedPackDir,
        QStringLiteral("--action-name"), QStringLiteral("ExportedPackImmediateSound"),
        QStringLiteral("--capture-ready-gate"), gate,
        QStringLiteral("--capture-ready-timeout-ms"), QStringLiteral("3000")});
    host.start();
    ASSERT_TRUE(host.waitForStarted(3000));

    QByteArray output;
    QElapsedTimer armedTimer;
    armedTimer.start();
    while (!output.contains("pack-play capture-ready waiting pid=") &&
           armedTimer.elapsed() < 3000) {
        host.waitForReadyRead(50);
        output += host.readAll();
    }
    ASSERT_TRUE(output.contains("pack-play capture-ready waiting pid="))
        << output.constData();
    host.waitForReadyRead(250);
    output += host.readAll();
    EXPECT_FALSE(output.contains("pack-play trigger ")) << output.constData();

    QSaveFile fresh(gate);
    ASSERT_TRUE(fresh.open(QIODevice::WriteOnly));
    const QByteArray marker =
        "process_id=" + QByteArray::number(host.processId()) + "\n";
    ASSERT_EQ(marker.size(), fresh.write(marker));
    ASSERT_TRUE(fresh.flush());
    EXPECT_FALSE(QFileInfo::exists(gate));
    ASSERT_TRUE(fresh.commit());
    ASSERT_TRUE(QFileInfo::exists(gate));

    ASSERT_TRUE(host.waitForFinished(10000));
    output += host.readAll();
    EXPECT_EQ(0, host.exitCode()) << output.constData();
    EXPECT_TRUE(output.contains("pack-play capture-ready pid="))
        << output.constData();
    EXPECT_TRUE(output.contains("pack-play trigger 1/1")) << output.constData();
}

TEST_F(ExportedPackLifecycle, WasapiImmediateSoundHarness)
{
    const GoldHostRun run = runHarkenHost(
        std::string("pack-play --seconds 0.4 --root ") +
        QFileInfo(preparedExportedPackDir()).absoluteFilePath().toStdString() + " --action-name ExportedPackImmediateSound");
    if (run.output.find("SKIPPED") != std::string::npos)
        GTEST_SKIP() << "No real WASAPI device\n" << run.output;
    ASSERT_EQ(0, run.exitCode) << run.output;
    EXPECT_NE(std::string::npos, run.output.find("first-callback non-silent"))
        << run.output;
    EXPECT_NE(std::string::npos, run.output.find("non-silent")) << run.output;
    const bool stopRestart =
        run.output.find("stop-restart restored playback") != std::string::npos;
    const bool termInit =
        run.output.find("term-init restored playback") != std::string::npos;
    EXPECT_TRUE(stopRestart || termInit) << run.output;
    EXPECT_FALSE(stopRestart && termInit) << run.output;
}

TEST_F(ExportedPackLifecycle, RelWithDebInfoQualificationReport)
{
#ifndef HARKEN_EXPORTED_PACK_QUAL_PATH
    GTEST_SKIP() << "harken_exported_pack_qualification is not built";
#else
    const QString nextDir =
        QDir(dir_.path()).filePath(QStringLiteral("exported-pack-qual-next"));
    ASSERT_NO_FATAL_FAILURE(exportProjectPackSuccessor(project_, dir_.path(), nextDir));

    const QString reportDir = QDir(QString::fromUtf8(HARKEN_STUDIO_SOURCE_DIR))
                                  .absoluteFilePath(QStringLiteral(".."));
    const QString reportPath =
        QDir(QFileInfo(reportDir).absoluteFilePath())
            .filePath(QStringLiteral(".temp/exported-pack-qualification.json"));
    ASSERT_TRUE(QDir().mkpath(QFileInfo(reportPath).absolutePath()));
    const std::string cmd =
        std::string(HARKEN_EXPORTED_PACK_QUAL_PATH) + " --exported-pack-root " +
        QFileInfo(preparedExportedPackDir()).absoluteFilePath().toStdString() + " --next-root " +
        QFileInfo(nextDir).absoluteFilePath().toStdString() +
        " --action-name ExportedPackVerticalContainer 2>&1";
    FILE* pipe = _popen(cmd.c_str(), "rt");
    ASSERT_NE(nullptr, pipe);
    std::string output;
    char chunk[512];
    while (std::fgets(chunk, sizeof chunk, pipe) != nullptr)
        output += chunk;
    const int exitCode = _pclose(pipe);
    ASSERT_EQ(0, exitCode) << output;
    EXPECT_NE(std::string::npos, output.find("\"status\":\"complete\"")) << output;
    EXPECT_NE(std::string::npos, output.find("\"minus_one_ok\":true")) << output;
    EXPECT_NE(std::string::npos, output.find("\"exact_ok\":true")) << output;
    EXPECT_NE(std::string::npos, output.find("\"plus_one_ok\":true")) << output;
    EXPECT_NE(std::string::npos, output.find("\"retiring_count\":0")) << output;
    EXPECT_NE(std::string::npos, output.find("\"source_sha\"")) << output;
    EXPECT_NE(std::string::npos, output.find("\"binary_sha\"")) << output;
    EXPECT_NE(std::string::npos, output.find("\"runner_sha\"")) << output;
    EXPECT_NE(std::string::npos, output.find("\"exported_pack_content_hash\"")) << output;
    QFile report(reportPath);
    ASSERT_TRUE(report.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(report.write(QByteArray::fromStdString(output)),
              static_cast<qint64>(output.size()));
#endif
}

TEST_F(ExportedPackLifecycle, TraceActiveActivatedPackRenders)
{
    LoadedExportedPack loaded;
    ASSERT_EQ(VN_OK, loaded.session.InitResult());
    const QByteArray journalPath = QDir(dir_.path())
        .filePath(QStringLiteral("exported-pack-trace-playable.vtrc")).toUtf8();
    ASSERT_EQ(VN_OK, Vn_TraceStart(journalPath.constData()));
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        preparedExportedPackDir(), QStringLiteral("ExportedPackVerticalContainer"), &loaded));
    harkentest::Capture capture;
    ASSERT_NO_FATAL_FAILURE(postAndCaptureImmediate(
        &loaded, harkentest::StudioObjectId(kVerticalContainerActionObjectId + kStableIdBias),
        &capture, 1u));
    ASSERT_EQ(VN_OK, Vn_TraceStop());
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

struct Att05Voice {
    VnVoiceHandle render = 0;
    harken::Spatializer::SpatialVoiceState state{};
    float maxDistance = 0.0f;
};

Att05Publication att05Publication(const harken::Engine* engine)
{
    Att05Publication out;
    const auto* published = engine->packComposition.Published();
    if (published == nullptr)
        return out;
    out.initHandle = published->initHandle;
    out.setRevision = published->ledger.setRevision;
    out.activationGeneration = published->ledger.activationGeneration;
    out.publishedBusFingerprint = published->ledger.publishedBusFingerprint;
    return out;
}

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

bool att05CaptureVoice(harken::Engine* engine, VnObjectId attenuationId, Att05Voice* out)
{
    bool found = false;
    auto consider = [&](VnVoiceHandle render) {
        harken::Spatializer::SpatialVoiceState state{};
        if (!engine->spatial.CaptureVoice(render, &state))
            return;
        if (out->render == 0)
            out->state = state;
        if (state.attenuationId != attenuationId)
            return;
        const auto* profile = engine->spatial.FindProfile(state.attenuationId, state.attenuationGeneration);
        if (profile == nullptr)
            return;
        out->render = render;
        out->state = state;
        out->maxDistance = profile->maxDistance;
        found = true;
    };
    engine->ledger.ForEachLive([&](VnVoiceHandle render, std::uint32_t, std::uint64_t, bool) {
        consider(render);
    });
    engine->ledger.ForEachPlayableOwnedLive([&](VnVoiceHandle render, const harken::VoiceLedger::PlayableParentRef&) {
        consider(render);
    });
    return found;
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

void att05Activate(const QString& exportedDir, const QString& actionName, LoadedExportedPack* loaded)
{
    loaded->rootUtf8 = QFileInfo(exportedDir).absoluteFilePath().toUtf8();
    ExportedPackActivation prepared;
    ASSERT_NO_FATAL_FAILURE(prepareExportedPackActivation(exportedDir, actionName, *loaded, &prepared));
    VnCanonicalPackSetActivationReportV1 activation{};
    const VnPackHandle previousInit = loaded->initHandle;
    const uint64_t previousRevision = loaded->activationReport.resultSetRevision;
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(
        prepared.members.data(), static_cast<uint32_t>(prepared.members.size()),
        prepared.rows.data(), activation, previousInit, previousRevision));
    ASSERT_NO_FATAL_FAILURE(acceptExportedPackActivation(&prepared, activation, loaded));
}

bool att05ReachTerminal(harken::Engine* engine, const std::vector<VnActionFiringHandle>& firings)
{
    for (uint32_t block = 0; block < kAtt05TerminalBlockCap; ++block) {
        if (Vn_Update() != VN_OK)
            return false;
        engine->nullBackend.Pump(1);
        if (engine->playableInstances.LiveCount() != 0 || engine->voices.ActiveCount() != 0)
            continue;
        bool allTerminal = true;
        for (const auto firing : firings) {
            VnActionFiringInfo info{};
            info.structSize = sizeof(info);
            if (Vn_QueryActionFiring(firing, &info) != VN_OK
                || info.state != VN_ACTION_FIRING_TERMINAL_RETAINED) {
                allTerminal = false;
                break;
            }
        }
        if (allTerminal)
            return true;
    }
    return false;
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
    if (bytes->size() < harken::kPackV2HeaderBytes)
        return false;
    harken::PackV2Header header;
    if (!harken::ParsePackV2Header(bytes->data(), &header))
        return false;
    const uint64_t directoryBytes =
        static_cast<uint64_t>(header.entryCount) * harken::kPackV2DirectoryEntryBytes;
    if (header.directoryOffset != header.headerBytes || header.entryCount == 0
        || header.directoryOffset + directoryBytes > bytes->size())
        return false;
    int found = -1;
    int attenuationCount = 0;
    for (uint32_t index = 0; index < header.entryCount; ++index) {
        const auto entry = harken::ParsePackV2DirectoryEntry(
            bytes->data() + header.directoryOffset
            + static_cast<size_t>(index) * harken::kPackV2DirectoryEntryBytes);
        if (entry.kind != harken::kPackEntryKindAttenuation)
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
    std::memmove(directory + static_cast<size_t>(found) * harken::kPackV2DirectoryEntryBytes,
                 directory + (static_cast<size_t>(found) + 1u) * harken::kPackV2DirectoryEntryBytes,
                 static_cast<size_t>(tail) * harken::kPackV2DirectoryEntryBytes);
    header.entryCount -= 1u;
    header.directoryChecksum = harken::Fnv1a64(
        directory, static_cast<size_t>(header.entryCount) * harken::kPackV2DirectoryEntryBytes);
    harken::SerializePackV2Header(header, bytes->data());
    *removedCount = attenuationCount;
    return true;
}

} // namespace

TEST_F(AuthoredPackLifecycle, CanonicalV2RejectsLegacyBusMismatchAndKeepsPublishedAudio)
{
    auto source = makeItem(QStringLiteral("MismatchSource"), VNX::ProjectItem::Type::Bus, 811);
    auto target = makeItem(QStringLiteral("MismatchTarget"), VNX::ProjectItem::Type::Bus, 812);
    addChild(project_.busses.front().rootItem, source);
    addChild(project_.busses.front().rootItem, target);
    const auto locator = VNX::PlayablePropertyAuthoringV1::definitionLocator(*source);
    ASSERT_TRUE(locator.has_value());
    const auto sendsProperty = static_cast<uint8_t>(
        harkenplayableproperties::PlayablePropertyIdV1::Sends);
    const auto setSend = [&](double db) {
        return VNX::PlayablePropertyAuthoringV1::setOverride(
            *source, *locator, sendsProperty,
            QJsonArray{QJsonObject{{QStringLiteral("targetBusID"), 812},
                                   {QStringLiteral("sendLevel"), db}}}, std::nullopt);
    };
    ASSERT_TRUE(setSend(-6.0));
    const auto& layerChildren = project_.layers.front().rootItem->children;
    const auto soundRow = std::find_if(layerChildren.begin(), layerChildren.end(),
        [](const auto& item) { return item->objectID == kContentSoundObjectId; });
    ASSERT_NE(layerChildren.end(), soundRow);
    (*soundRow)->fieldGroups.mainOutput.shared = {VNX::FieldGroupMode::Local,
        VNX::MainOutputFieldGroup{VNX::toFieldGroupGuid(source->guid), 0.0f}};
    (*soundRow)->compOrCreate<VNX::VMixNode>()->outputBusId = 811;
    const QString firstDir = QDir(dir_.path()).filePath(QStringLiteral("volume-before"));
    const QString nextDir = QDir(dir_.path()).filePath(QStringLiteral("volume-mismatched"));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, firstDir));
    LoadedExportedPack loaded;
    ASSERT_NO_FATAL_FAILURE(loadExportedPackSession(
        firstDir, QStringLiteral("ExportedPackImmediateSound"), &loaded));
    auto* engine = loaded.session.Engine();
    ASSERT_NE(nullptr, engine);
    const auto masterId = project_.busses.front().rootItem->runtimeId();
    const auto busId = harkentest::ToRuntime(masterId);
    const auto index = engine->buses.FindIndex(busId);
    ASSERT_NE(harken::kInvalidBusIndex, index);
    const auto previousHandle = loaded.initHandle;
    const auto previousRevision = loaded.activationReport.resultSetRevision;
    const auto previousPublished = engine->packComposition.PublishedHandle();
    const auto previousOwner = engine->busOwnerCookies[index];
    const auto sourceSlot = engine->buses.FindIndex(harkentest::ToRuntime(source->runtimeId()));
    const auto targetSlot = engine->buses.FindIndex(harkentest::ToRuntime(target->runtimeId()));
    ASSERT_NE(harken::kInvalidBusIndex, sourceSlot);
    ASSERT_NE(harken::kInvalidBusIndex, targetSlot);
    const auto sendSlots = engine->config.busSendSlotsPerBus;
    std::uint32_t sendSlot = sendSlots;
    for (std::uint32_t s = 0; s < sendSlots; ++s)
        if (engine->busSendAdj[static_cast<std::size_t>(sourceSlot) * sendSlots + s] == targetSlot)
            sendSlot = s;
    ASSERT_LT(sendSlot, sendSlots);
    const auto sendFlat = static_cast<std::size_t>(sourceSlot) * sendSlots + sendSlot;
    const auto previousSendOwner = engine->busSendOwnerCookies[sendFlat];
    ASSERT_FLOAT_EQ(-6.0f, engine->busSendLevelDb[sendFlat]);
    const float previousOutput = engine->busTargets.SlotValue(
        index, harken::kBusTargetBusOutputVolume,
        harken::BusTargetStore::kSlotAuthoredIndex);
    VnPostActionDesc post{};
    post.structSize = sizeof(post);
    post.actionId = harkentest::ToRuntime(harkentest::StudioObjectId(
        kImmediateSoundActionObjectId + kStableIdBias));
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&post.emitter));
    VnActionFiringHandle firing = 0;
    ASSERT_EQ(VN_OK, Vn_PostAction(&post, &firing));
    ASSERT_NO_FATAL_FAILURE(expectActivePlayable(firing, engine, 1));
    const auto sampleCount = static_cast<std::size_t>(engine->nullBackend.BlockFrames())
        * engine->nullBackend.Channels();
    const auto beforeBlock = engine->mixer.BlocksRendered();
    std::vector<float> beforeSamples;
    std::uint64_t audibleBlock = 0;
    for (std::uint32_t ordinal = 1; ordinal <= 16; ++ordinal) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
        ASSERT_EQ(beforeBlock + ordinal, engine->mixer.BlocksRendered());
        const float* block = engine->nullBackend.LastBlock();
        if (std::any_of(block, block + sampleCount, [](float sample) {
                return std::abs(sample) > 0.05f && std::abs(sample) < 0.8f;
            })) {
            audibleBlock = engine->mixer.BlocksRendered();
            beforeSamples.assign(block, block + sampleCount);
            break;
        }
    }
    ASSERT_FALSE(beforeSamples.empty());
    const auto audible = std::find_if(beforeSamples.begin(), beforeSamples.end(),
        [](float sample) { return std::abs(sample) > 0.05f && std::abs(sample) < 0.8f; });
    ASSERT_NE(audible, beforeSamples.end());
    const auto sampleIndex = static_cast<std::size_t>(audible - beforeSamples.begin());

    auto* mix = project_.busses.front().rootItem->comp<VNX::VMixNode>();
    ASSERT_NE(nullptr, mix);
    mix->outputBusVolume = -6.0f;
    ASSERT_TRUE(setSend(0.0));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, nextDir));
    const QString initPath = QDir(nextDir).filePath(QStringLiteral("Init.bytes"));
    QFile input(initPath);
    ASSERT_TRUE(input.open(QIODevice::ReadOnly));
    const QByteArray original = input.readAll();
    input.close();
    enum class Mismatch { Volume, MakeUpGain, OutputVolume, BusId, SendLevel };
    for (const auto mismatch : {Mismatch::Volume, Mismatch::MakeUpGain,
                                  Mismatch::OutputVolume, Mismatch::BusId,
                                  Mismatch::SendLevel}) {
        SCOPED_TRACE(static_cast<int>(mismatch));
        std::vector<std::uint8_t> bytes(original.begin(), original.end());
        harken::PackV2Header header{};
        ASSERT_TRUE(harken::ParsePackV2Header(bytes.data(), &header));
        bool changed = false;
        for (std::uint32_t i = 0; i < header.entryCount; ++i) {
            auto* directory = bytes.data() + header.directoryOffset
                + static_cast<std::size_t>(i) * harken::kPackV2DirectoryEntryBytes;
            auto entry = harken::ParsePackV2DirectoryEntry(directory);
            if (mismatch == Mismatch::SendLevel &&
                entry.kind != harken::kPackEntryKindBusSend) continue;
            if (mismatch != Mismatch::SendLevel &&
                (entry.kind != harken::kPackEntryKindBus || entry.id != busId))
                continue;
            if (mismatch == Mismatch::SendLevel) {
                harken::PackBusSendEntry send{};
                ASSERT_TRUE(harken::ParsePackV2BusSend(
                    bytes.data() + entry.offset, entry.size, &send));
                if (send.fromBusId != harkentest::ToRuntime(source->runtimeId()) ||
                    send.toBusId != harkentest::ToRuntime(target->runtimeId())) continue;
                ASSERT_FLOAT_EQ(0.0f, send.levelDb);
                send.levelDb = -12.0f;
                harken::SerializePackV2BusSend(send, bytes.data() + entry.offset);
            } else {
                ASSERT_EQ(harken::kPackV6BusPayloadBytes, entry.size);
                harken::PackBusEntry bus{};
                ASSERT_TRUE(harken::ParsePackV6Bus(bytes.data() + entry.offset, entry.size, &bus));
                ASSERT_FLOAT_EQ(-6.0f, bus.outputBusVolume);
                switch (mismatch) {
                case Mismatch::Volume: bus.volume = -3.0f; break;
                case Mismatch::MakeUpGain: bus.makeUpGain = 2.0f; break;
                case Mismatch::OutputVolume: bus.outputBusVolume = -9.0f; break;
                case Mismatch::BusId:
                    bus.busId = harkentest::ToRuntime(harkentest::StudioObjectId(999999));
                    entry.id = bus.busId;
                    break;
                case Mismatch::SendLevel: break;
                }
                harken::SerializePackV6Bus(bus, bytes.data() + entry.offset);
            }
            entry.payloadChecksum = harken::Fnv1a64(bytes.data() + entry.offset,
                                                    static_cast<std::size_t>(entry.size));
            harken::SerializePackV2DirectoryEntry(entry, directory);
            changed = true;
            break;
        }
        ASSERT_TRUE(changed);
        header.directoryChecksum = harken::Fnv1a64(bytes.data() + header.directoryOffset,
            static_cast<std::size_t>(header.entryCount) * harken::kPackV2DirectoryEntryBytes);
        harken::SerializePackV2Header(header, bytes.data());
        QFile output(initPath);
        ASSERT_TRUE(output.open(QIODevice::WriteOnly | QIODevice::Truncate));
        ASSERT_EQ(static_cast<qint64>(bytes.size()), output.write(
            reinterpret_cast<const char*>(bytes.data()), static_cast<qint64>(bytes.size())));
        output.close();
        harken::PackCanonicalImage decoded;
        ASSERT_EQ(VN_OK, harken::ParsePackCanonicalImage(bytes.data(), bytes.size(), &decoded));
        ASSERT_TRUE(decoded.hasBusDefinitionSnapshot);

        loaded.rootUtf8 = QFileInfo(nextDir).absoluteFilePath().toUtf8();
        ExportedPackActivation prepared;
        ASSERT_NO_FATAL_FAILURE(prepareExportedPackActivation(
            nextDir, QStringLiteral("ExportedPackImmediateSound"), loaded, &prepared));
        VnCanonicalPackSetActivationReportV1 report{};
        EXPECT_EQ(VN_BAD_PACK, harkentest::ActivateCanonicalMembersV1(
            prepared.members.data(), static_cast<std::uint32_t>(prepared.members.size()),
            prepared.rows.data(), report, previousHandle, previousRevision));
        EXPECT_EQ(previousPublished, engine->packComposition.PublishedHandle());
        EXPECT_EQ(previousOwner, engine->busOwnerCookies[index]);
        EXPECT_EQ(previousSendOwner, engine->busSendOwnerCookies[sendFlat]);
        EXPECT_FLOAT_EQ(-6.0f, engine->busSendLevelDb[sendFlat]);
        EXPECT_FLOAT_EQ(previousOutput, engine->busTargets.SlotValue(
            index, harken::kBusTargetBusOutputVolume,
            harken::BusTargetStore::kSlotAuthoredIndex));
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
        ++audibleBlock;
        ASSERT_EQ(audibleBlock, engine->mixer.BlocksRendered());
        const float afterSample = engine->nullBackend.LastBlock()[sampleIndex];
        EXPECT_NEAR(afterSample, beforeSamples[sampleIndex], 1e-5f);
    }
    QFile restored(initPath);
    ASSERT_TRUE(restored.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(original.size(), restored.write(original));
    restored.close();
    ExportedPackActivation retry;
    ASSERT_NO_FATAL_FAILURE(prepareExportedPackActivation(
        nextDir, QStringLiteral("ExportedPackImmediateSound"), loaded, &retry));
    VnCanonicalPackSetActivationReportV1 accepted{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(
        retry.members.data(), static_cast<std::uint32_t>(retry.members.size()),
        retry.rows.data(), accepted, previousHandle, previousRevision));
    ASSERT_NO_FATAL_FAILURE(acceptExportedPackActivation(&retry, accepted, &loaded));
    ASSERT_EQ(previousRevision + 1, loaded.activationReport.resultSetRevision);
    ASSERT_EQ(targetSlot, engine->busSendAdj[sendFlat]);
    EXPECT_EQ(loaded.initHandle, engine->busSendOwnerCookies[sendFlat]);
    EXPECT_FLOAT_EQ(0.0f, engine->busSendLevelDb[sendFlat]);
    bool foundRetainedRamp = false;
    for (std::uint32_t planIndex = 0; planIndex < engine->topologyPlans.Capacity();
         ++planIndex) {
        if (!engine->topologyPlans.IsAcquired(planIndex)) continue;
        const auto plan = engine->topologyPlans.View(planIndex);
        if (!plan.valid || plan.sendTargets == nullptr ||
            plan.preparedSendLevels == nullptr ||
            plan.sendLevelApply == nullptr || plan.sendLevelRampMs == nullptr ||
            plan.sendTargets[sendFlat] != targetSlot ||
            plan.preparedSendLevels[sendFlat] != 1.0f ||
            plan.sendLevelApply[sendFlat] == 0) continue;
        EXPECT_EQ(20u, plan.sendLevelRampMs[sendFlat]);
        foundRetainedRamp = true;
    }
    EXPECT_TRUE(foundRetainedRamp);
    const VnResult released = Vn_ReleaseActionFiring(firing);
    EXPECT_TRUE(released == VN_OK || released == VN_IN_USE);
}

TEST_F(ExportedPackLifecycle, Att05HoldKeepsGenerationWhileFiring)
{
    const QString exported = QDir(dir_.path()).filePath(QStringLiteral("att05-hold"));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, exported));
    LoadedExportedPack loaded;
    ASSERT_EQ(VN_OK, loaded.session.InitResult());
    ASSERT_NO_FATAL_FAILURE(att05Activate(exported, QStringLiteral("ExportedPackSpatialNearFar"), &loaded));
    const Att05Publication published = att05Publication(loaded.session.Engine());
    ASSERT_NE(0u, published.initHandle);

    VnActionFiringHandle firing = 0;
    ASSERT_NO_FATAL_FAILURE(att05PostSpatial(&firing));
    ASSERT_NE(0u, firing);
    harkentest::Capture capture;
    ASSERT_NO_FATAL_FAILURE(pumpUntilAudible(loaded.session.Engine(), &capture));
    ASSERT_NO_FATAL_FAILURE(expectActivePlayable(firing, loaded.session.Engine(), 1u));

    Att05Voice voice;
    ASSERT_TRUE(att05CaptureVoice(loaded.session.Engine(), att05AttenuationId(), &voice));
    const auto generation = voice.state.attenuationGeneration;
    const auto maxDistance = voice.maxDistance;
    ASSERT_NE(0u, generation);

    ASSERT_EQ(VN_OK, Vn_Update());
    loaded.session.Engine()->nullBackend.Pump(1);
    ASSERT_NO_FATAL_FAILURE(expectActivePlayable(firing, loaded.session.Engine(), 1u));
    Att05Voice again;
    ASSERT_TRUE(att05CaptureVoice(loaded.session.Engine(), att05AttenuationId(), &again));
    EXPECT_EQ(generation, again.state.attenuationGeneration);
    EXPECT_FLOAT_EQ(maxDistance, again.maxDistance);
    EXPECT_EQ(voice.render, again.render);
    expectSamePublication(published, att05Publication(loaded.session.Engine()));
}

TEST_F(ExportedPackLifecycle, Att05ReplaceKeepsOldGenerationUntilFiringRelease)
{
    const QString firstDir = QDir(dir_.path()).filePath(QStringLiteral("att05-replace-a"));
    const QString secondDir = QDir(dir_.path()).filePath(QStringLiteral("att05-replace-b"));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, firstDir));
    auto* attenuation = att05Attenuation(project_);
    ASSERT_NE(nullptr, attenuation);
    attenuation->maxDistance = kAtt05ReplacementMaxDistance;
    for (auto& dimension : attenuation->dimensions) {
        if (dimension.type != VNX::AttenuationDimension::Volume)
            continue;
        for (auto& point : dimension.curve) {
            if (point.x == kAtt04MaxDistance)
                point.x = kAtt05ReplacementMaxDistance;
        }
        VNX::attenuationNormalizeCurve(dimension.type, dimension.curve, dimension.handles);
    }
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, secondDir));

    LoadedExportedPack loaded;
    ASSERT_EQ(VN_OK, loaded.session.InitResult());
    ASSERT_NO_FATAL_FAILURE(att05Activate(firstDir, QStringLiteral("ExportedPackSpatialNearFar"), &loaded));
    VnActionFiringHandle oldFiring = 0;
    ASSERT_NO_FATAL_FAILURE(att05PostSpatial(&oldFiring));
    ASSERT_NE(0u, oldFiring);
    harkentest::Capture first;
    ASSERT_NO_FATAL_FAILURE(pumpUntilAudible(loaded.session.Engine(), &first));
    ASSERT_NO_FATAL_FAILURE(expectActivePlayable(oldFiring, loaded.session.Engine(), 1u));
    Att05Voice oldVoice;
    ASSERT_TRUE(att05CaptureVoice(loaded.session.Engine(), att05AttenuationId(), &oldVoice));

    ASSERT_NO_FATAL_FAILURE(att05Activate(secondDir, QStringLiteral("ExportedPackSpatialNearFar"), &loaded));
    Att05Voice stillOld;
    ASSERT_TRUE(att05CaptureVoice(loaded.session.Engine(), att05AttenuationId(), &stillOld));
    EXPECT_EQ(oldVoice.state.attenuationGeneration, stillOld.state.attenuationGeneration);
    EXPECT_FLOAT_EQ(oldVoice.maxDistance, stillOld.maxDistance);

    VnActionFiringHandle newFiring = 0;
    ASSERT_NO_FATAL_FAILURE(att05PostSpatial(&newFiring));
    ASSERT_NE(0u, newFiring);
    harkentest::Capture second;
    ASSERT_NO_FATAL_FAILURE(pumpUntilAudible(loaded.session.Engine(), &second));
    ASSERT_NO_FATAL_FAILURE(expectActivePlayable(newFiring, loaded.session.Engine(), 1u));
    const auto currentGeneration =
        loaded.session.Engine()->spatial.CurrentProfileGeneration(att05AttenuationId());
    const auto* currentProfile =
        loaded.session.Engine()->spatial.FindProfile(att05AttenuationId(), currentGeneration);
    ASSERT_NE(nullptr, currentProfile);
    EXPECT_NE(oldVoice.state.attenuationGeneration, currentGeneration);
    EXPECT_FLOAT_EQ(kAtt05ReplacementMaxDistance, currentProfile->maxDistance);

    ASSERT_TRUE(att05ReachTerminal(loaded.session.Engine(), {oldFiring, newFiring}))
        << "playableLive=" << loaded.session.Engine()->playableInstances.LiveCount()
        << " voices=" << loaded.session.Engine()->voices.ActiveCount();
    EXPECT_NE(nullptr, loaded.session.Engine()->spatial.FindProfile(
        att05AttenuationId(), oldVoice.state.attenuationGeneration));

    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(oldFiring));
    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(newFiring));
    EXPECT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, loaded.session.Engine()->spatial.FindProfile(
        att05AttenuationId(), oldVoice.state.attenuationGeneration));
    EXPECT_EQ(currentGeneration,
        loaded.session.Engine()->spatial.CurrentProfileGeneration(att05AttenuationId()));
    const auto* stillCurrent =
        loaded.session.Engine()->spatial.FindProfile(att05AttenuationId(), currentGeneration);
    ASSERT_NE(nullptr, stillCurrent);
    EXPECT_FLOAT_EQ(kAtt05ReplacementMaxDistance, stillCurrent->maxDistance);
}

TEST_F(ExportedPackLifecycle, Att05UnloadThenReclaimThenReload)
{
    const QString exported = QDir(dir_.path()).filePath(QStringLiteral("att05-unload"));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, exported));
    LoadedExportedPack loaded;
    ASSERT_EQ(VN_OK, loaded.session.InitResult());
    ASSERT_NO_FATAL_FAILURE(att05Activate(exported, QStringLiteral("ExportedPackSpatialNearFar"), &loaded));
    VnActionFiringHandle firing = 0;
    ASSERT_NO_FATAL_FAILURE(att05PostSpatial(&firing));
    ASSERT_NE(0u, firing);
    harkentest::Capture capture;
    ASSERT_NO_FATAL_FAILURE(pumpUntilAudible(loaded.session.Engine(), &capture));
    ASSERT_NO_FATAL_FAILURE(expectActivePlayable(firing, loaded.session.Engine(), 1u));
    Att05Voice voice;
    ASSERT_TRUE(att05CaptureVoice(loaded.session.Engine(), att05AttenuationId(), &voice));
    const Att05Publication beforeUnload = att05Publication(loaded.session.Engine());
    const uint64_t priorActivation = beforeUnload.activationGeneration;

    VnCanonicalPackSetUnloadDescV1 unload{};
    unload.structSize = sizeof(unload);
    unload.expectedInitHandle = beforeUnload.initHandle;
    unload.baseSetRevision = beforeUnload.setRevision;
    unload.unloadRevision = beforeUnload.setRevision + 1u;
    ASSERT_EQ(VN_OK, Vn_UnloadCanonicalPackSetV1(&unload));
    EXPECT_EQ(VN_OK, Vn_UnloadCanonicalPackSetV1(&unload));
    Att05Voice duringUnload;
    EXPECT_TRUE(att05CaptureVoice(loaded.session.Engine(), att05AttenuationId(), &duringUnload));
    EXPECT_EQ(voice.state.attenuationGeneration, duringUnload.state.attenuationGeneration);

    EXPECT_TRUE(att05ReachTerminal(loaded.session.Engine(), {firing}))
        << "terminal stage playableLive=" << loaded.session.Engine()->playableInstances.LiveCount()
        << " voices=" << loaded.session.Engine()->voices.ActiveCount();
    EXPECT_NE(nullptr, loaded.session.Engine()->spatial.FindProfile(
        att05AttenuationId(), voice.state.attenuationGeneration));
    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
    EXPECT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, loaded.session.Engine()->spatial.FindProfile(
        att05AttenuationId(), voice.state.attenuationGeneration));

    auto* engine = loaded.session.Engine();
    const auto tombstoneBefore = engine->packSetTombstone;
    const auto currentGenerationBefore =
        engine->spatial.CurrentProfileGeneration(att05AttenuationId());
    const auto* profileBeforeReject = engine->spatial.FindProfile(
        att05AttenuationId(), voice.state.attenuationGeneration);
    const auto* publishedBefore = engine->packComposition.Published();
    const VnPackHandle publishedInitBefore =
        publishedBefore == nullptr ? 0 : publishedBefore->initHandle;

    ExportedPackActivation prepared;
    ASSERT_NO_FATAL_FAILURE(prepareExportedPackActivation(
        exported, QStringLiteral("ExportedPackSpatialNearFar"), loaded, &prepared));
    for (auto& member : prepared.members)
        member.expectedHandle = 0;
    VnCanonicalPackSetActivationReportV1 rejected{};
    const VnResult higherResult = harkentest::ActivateCanonicalMembersV1(
        prepared.members.data(), static_cast<uint32_t>(prepared.members.size()),
        prepared.rows.data(), rejected, beforeUnload.initHandle, beforeUnload.setRevision);
    ASSERT_EQ(VN_REVISION_CONFLICT, higherResult);

    const auto* publishedAfterReject = engine->packComposition.Published();
    const VnPackHandle publishedInitAfter =
        publishedAfterReject == nullptr ? 0 : publishedAfterReject->initHandle;
    EXPECT_EQ(0u, publishedInitBefore);
    EXPECT_EQ(publishedInitBefore, publishedInitAfter);
    const auto& tombstoneAfter = engine->packSetTombstone;
    EXPECT_EQ(tombstoneBefore.hasTombstone, tombstoneAfter.hasTombstone);
    EXPECT_EQ(tombstoneBefore.setRevision, tombstoneAfter.setRevision);
    EXPECT_EQ(tombstoneBefore.unloadRevision, tombstoneAfter.unloadRevision);
    EXPECT_EQ(tombstoneBefore.semanticKey, tombstoneAfter.semanticKey);
    EXPECT_EQ(tombstoneBefore.activationGeneration, tombstoneAfter.activationGeneration);
    EXPECT_EQ(tombstoneBefore.initHandle, tombstoneAfter.initHandle);
    EXPECT_EQ(tombstoneBefore.busContentGeneration, tombstoneAfter.busContentGeneration);
    EXPECT_EQ(tombstoneBefore.publishedBusFingerprint, tombstoneAfter.publishedBusFingerprint);
    EXPECT_EQ(tombstoneBefore.compileTargetProfileFingerprint,
        tombstoneAfter.compileTargetProfileFingerprint);
    const auto* profileAfterReject = engine->spatial.FindProfile(
        att05AttenuationId(), voice.state.attenuationGeneration);
    EXPECT_EQ(profileBeforeReject != nullptr, profileAfterReject != nullptr);
    if (profileBeforeReject != nullptr && profileAfterReject != nullptr)
        EXPECT_FLOAT_EQ(profileBeforeReject->maxDistance, profileAfterReject->maxDistance);
    EXPECT_EQ(currentGenerationBefore,
        engine->spatial.CurrentProfileGeneration(att05AttenuationId()));

    VnCanonicalPackSetActivationReportV1 reloaded{};
    ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(
        prepared.members.data(), static_cast<uint32_t>(prepared.members.size()),
        prepared.rows.data(), reloaded, 0, 0));
    EXPECT_EQ(priorActivation + 1u, reloaded.activationGeneration);
    const auto* reloadedPublished = engine->packComposition.Published();
    ASSERT_NE(nullptr, reloadedPublished);
    const auto attenuationToken =
        reloadedPublished->attenuationBindings.GenerationFor(att05AttenuationId());
    const auto reloadedGeneration =
        engine->spatial.CurrentProfileGeneration(att05AttenuationId());
    EXPECT_NE(0u, attenuationToken);
    EXPECT_EQ(attenuationToken, reloadedGeneration);
    EXPECT_NE(voice.state.attenuationGeneration, attenuationToken);
    const auto* reloadedProfile = engine->spatial.FindProfile(
        att05AttenuationId(), attenuationToken);
    ASSERT_NE(nullptr, reloadedProfile);
    EXPECT_FLOAT_EQ(kAtt04MaxDistance, reloadedProfile->maxDistance);
    VnActionFiringHandle again = 0;
    ASSERT_NO_FATAL_FAILURE(att05PostSpatial(&again));
    ASSERT_NE(0u, again);
    harkentest::Capture reloadedCapture;
    ASSERT_NO_FATAL_FAILURE(pumpUntilAudible(loaded.session.Engine(), &reloadedCapture));
    ASSERT_NO_FATAL_FAILURE(expectActivePlayable(again, loaded.session.Engine(), 1u));
}

TEST_F(ExportedPackLifecycle, Att05MissingAttenuationEntryRejectsInUse)
{
    const QString exported = QDir(dir_.path()).filePath(QStringLiteral("att05-missing"));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, exported));
    LoadedExportedPack loaded;
    ASSERT_EQ(VN_OK, loaded.session.InitResult());
    ASSERT_NO_FATAL_FAILURE(att05Activate(exported, QStringLiteral("ExportedPackSpatialNearFar"), &loaded));
    VnActionFiringHandle firing = 0;
    ASSERT_NO_FATAL_FAILURE(att05PostSpatial(&firing));
    ASSERT_NE(0u, firing);
    harkentest::Capture capture;
    ASSERT_NO_FATAL_FAILURE(pumpUntilAudible(loaded.session.Engine(), &capture));
    ASSERT_NO_FATAL_FAILURE(expectActivePlayable(firing, loaded.session.Engine(), 1u));
    Att05Voice voice;
    ASSERT_TRUE(att05CaptureVoice(loaded.session.Engine(), att05AttenuationId(), &voice));
    const Att05Publication published = att05Publication(loaded.session.Engine());

    QString initPath;
    QString contentPath;
    for (size_t index = 0; index < loaded.members.size(); ++index) {
        const QString path = QString::fromUtf8(loaded.memberPaths[index]);
        if (loaded.members[index].packKind == VN_CANONICAL_PACK_SET_MEMBER_INIT)
            initPath = path;
        else
            contentPath = path;
    }
    ASSERT_FALSE(initPath.isEmpty());
    ASSERT_FALSE(contentPath.isEmpty());
    const auto contentBefore = fileSha256(contentPath);
    auto initBytes = att05ReadFile(initPath);
    harken::PackCanonicalImage initImage;
    ASSERT_EQ(VN_OK, harken::ParsePackCanonicalImage(initBytes.data(), initBytes.size(), &initImage));
    const VnObjectId target = att05AttenuationId();
    int parsedMatches = 0;
    for (const auto& attenuation : initImage.attenuations) {
        if (attenuation.id == target)
            ++parsedMatches;
    }
    ASSERT_EQ(1, parsedMatches);
    for (const auto& row : initImage.catalogRows) {
        EXPECT_NE(target, row.stableId)
            << "Init catalog names the attenuation; directory-only removal is not sufficient";
    }
    auto contentBytes = att05ReadFile(contentPath);
    std::array<std::uint8_t, 16> idBytes{};
    harken::StoreObjectId(idBytes.data(), target);
    EXPECT_NE(contentBytes.end(),
        std::search(contentBytes.begin(), contentBytes.end(), idBytes.begin(), idBytes.end()));
    harken::PackCanonicalImage contentImage;
    ASSERT_EQ(VN_OK, harken::ParsePackCanonicalImage(
        contentBytes.data(), contentBytes.size(), &contentImage));

    int attenuationEntries = 0;
    ASSERT_TRUE(att05DropAttenuationEntry(&initBytes, target, &attenuationEntries));
    ASSERT_GT(attenuationEntries, 0);
    harken::PackCanonicalImage stripped;
    ASSERT_EQ(VN_OK, harken::ParsePackCanonicalImage(initBytes.data(), initBytes.size(), &stripped));
    EXPECT_EQ(static_cast<size_t>(parsedMatches - 1), stripped.attenuations.size());
    for (const auto& attenuation : stripped.attenuations)
        EXPECT_NE(target, attenuation.id);
    const QString derivedPath = QDir(exported).filePath(QStringLiteral("att05-missing-init.bytes"));
    {
        QFile derived(derivedPath);
        ASSERT_TRUE(derived.open(QIODevice::WriteOnly));
        ASSERT_EQ(static_cast<qint64>(initBytes.size()), derived.write(
            reinterpret_cast<const char*>(initBytes.data()), static_cast<qint64>(initBytes.size())));
    }
    EXPECT_EQ(contentBefore, fileSha256(contentPath));

    ExportedPackActivation prepared;
    ASSERT_NO_FATAL_FAILURE(prepareExportedPackActivation(
        exported, QStringLiteral("ExportedPackSpatialNearFar"), loaded, &prepared));
    QByteArray derivedUtf8 = QFileInfo(derivedPath).absoluteFilePath().toUtf8();
    ASSERT_FALSE(prepared.members.empty());
    prepared.members.front().utf8Path = derivedUtf8.constData();
    VnCanonicalPackSetActivationReportV1 rejected{};
    const VnResult result = harkentest::ActivateCanonicalMembersV1(
        prepared.members.data(), static_cast<uint32_t>(prepared.members.size()),
        prepared.rows.data(), rejected, published.initHandle, published.setRevision);
    EXPECT_EQ(VN_IN_USE, result);
    expectSamePublication(published, att05Publication(loaded.session.Engine()));
    Att05Voice after;
    EXPECT_TRUE(att05CaptureVoice(loaded.session.Engine(), target, &after));
    EXPECT_EQ(voice.state.attenuationGeneration, after.state.attenuationGeneration);
}

TEST_F(ExportedPackLifecycle, Att03MissingInitProfileRejectsDespiteAmbientRegistration)
{
    const QString exported = QDir(dir_.path()).filePath(QStringLiteral("att03-missing-init"));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, exported));
    const QJsonArray packs = readPublication(exported);
    const QJsonObject initEntry = findNamedPack(packs, QStringLiteral("Init"));
    const QJsonObject contentEntry = findNamedPack(packs, QStringLiteral("ExportedPackSpatialNearFar"));
    ASSERT_FALSE(initEntry.isEmpty());
    ASSERT_FALSE(contentEntry.isEmpty());
    const QString initPath = QString::fromUtf8(packPathUtf8(exported, initEntry));
    const QString contentPath = QString::fromUtf8(packPathUtf8(exported, contentEntry));
    const auto contentHash = fileSha256(contentPath);
    auto initBytes = att05ReadFile(initPath);
    const VnObjectId referencedId = att05AttenuationId();
    int attenuationEntries = 0;
    ASSERT_TRUE(att05DropAttenuationEntry(&initBytes, referencedId, &attenuationEntries));
    ASSERT_EQ(1, attenuationEntries);
    harken::PackCanonicalImage stripped;
    ASSERT_EQ(VN_OK, harken::ParsePackCanonicalImage(initBytes.data(), initBytes.size(), &stripped));
    EXPECT_TRUE(stripped.attenuations.empty());
    const QString derivedPath = QDir(exported).filePath(QStringLiteral("att03-missing-profile.bytes"));
    {
        QFile derived(derivedPath);
        ASSERT_TRUE(derived.open(QIODevice::WriteOnly));
        ASSERT_EQ(static_cast<qint64>(initBytes.size()), derived.write(
            reinterpret_cast<const char*>(initBytes.data()), static_cast<qint64>(initBytes.size())));
    }
    ASSERT_EQ(contentHash, fileSha256(contentPath));
    const QByteArray derivedUtf8 = QFileInfo(derivedPath).absoluteFilePath().toUtf8();
    const VnObjectId unrelatedId = harkentest::ObjectId(999999);
    const VnAttenuationDesc directProfile = harkentest::MakeAttenuationDesc(1.0f, 40.0f);

    for (const bool matchingAmbient : {false, true}) {
        SCOPED_TRACE(matchingAmbient ? "same-ID ambient profile" : "unrelated direct owner");
        LoadedExportedPack loaded;
        ASSERT_EQ(VN_OK, loaded.session.InitResult());
        auto* engine = loaded.session.Engine();
        ASSERT_NE(nullptr, engine);
        const VnObjectId directId = matchingAmbient ? referencedId : unrelatedId;
        ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(directId, &directProfile));
        const auto directGeneration = engine->spatial.CurrentProfileGeneration(directId);
        ASSERT_NE(nullptr, engine->spatial.FindProfile(directId));
        const auto beforePublication = att05Publication(engine);
        const auto beforeLoaded = engine->packs.LoadedCount();
        const auto beforeProfiles = engine->spatial.ProfileCount();
        const auto beforeRetained = engine->spatial.RetainedProfileCount();
        ASSERT_EQ(0u, beforePublication.initHandle);
        ASSERT_EQ(0u, beforeLoaded);
        ASSERT_EQ(1u, beforeProfiles);
        ASSERT_EQ(0u, beforeRetained);

        loaded.rootUtf8 = QFileInfo(exported).absoluteFilePath().toUtf8();
        ExportedPackActivation prepared;
        ASSERT_NO_FATAL_FAILURE(prepareExportedPackActivation(
            exported, QStringLiteral("ExportedPackSpatialNearFar"), loaded, &prepared));
        ASSERT_EQ(2u, prepared.members.size());
        prepared.members.front().utf8Path = derivedUtf8.constData();
        VnCanonicalPackSetActivationReportV1 rejected{};
        const VnResult result = harkentest::ActivateCanonicalMembersV1(
            prepared.members.data(), static_cast<uint32_t>(prepared.members.size()),
            prepared.rows.data(), rejected, 0, 0);
        EXPECT_EQ(VN_UNKNOWN_ID, result);
        if (matchingAmbient) {
            RecordProperty("ambient_refusal_result", static_cast<int>(result));
            RecordProperty("ambient_profile_count_before", beforeProfiles);
            RecordProperty("ambient_profile_count_after", engine->spatial.ProfileCount());
            RecordProperty("ambient_loaded_count_after", engine->packs.LoadedCount());
        } else {
            RecordProperty("clean_refusal_result", static_cast<int>(result));
            RecordProperty("clean_profile_count_before", beforeProfiles);
            RecordProperty("clean_profile_count_after", engine->spatial.ProfileCount());
            RecordProperty("clean_loaded_count_after", engine->packs.LoadedCount());
        }
        expectSamePublication(beforePublication, att05Publication(engine));
        EXPECT_EQ(beforeLoaded, engine->packs.LoadedCount());
        EXPECT_EQ(beforeProfiles, engine->spatial.ProfileCount());
        EXPECT_EQ(beforeRetained, engine->spatial.RetainedProfileCount());
        EXPECT_EQ(directGeneration, engine->spatial.CurrentProfileGeneration(directId));
        const auto* directAfter = engine->spatial.FindProfile(directId);
        ASSERT_NE(nullptr, directAfter);
        EXPECT_EQ(directId, directAfter->id);
        EXPECT_FLOAT_EQ(directProfile.minDistance, directAfter->minDistance);
        EXPECT_FLOAT_EQ(directProfile.maxDistance, directAfter->maxDistance);
        EXPECT_EQ(contentHash, fileSha256(contentPath));

        if (matchingAmbient)
            continue;
        ExportedPackActivation retry;
        ASSERT_NO_FATAL_FAILURE(prepareExportedPackActivation(
            exported, QStringLiteral("ExportedPackSpatialNearFar"), loaded, &retry));
        VnCanonicalPackSetActivationReportV1 accepted{};
        ASSERT_EQ(VN_OK, harkentest::ActivateCanonicalMembersV1(
            retry.members.data(), static_cast<uint32_t>(retry.members.size()),
            retry.rows.data(), accepted, 0, 0));
        ASSERT_NO_FATAL_FAILURE(acceptExportedPackActivation(&retry, accepted, &loaded));
        EXPECT_EQ(directGeneration, engine->spatial.CurrentProfileGeneration(unrelatedId));
        const auto* directAfterRetry = engine->spatial.FindProfile(unrelatedId);
        ASSERT_NE(nullptr, directAfterRetry);
        EXPECT_EQ(unrelatedId, directAfterRetry->id);
        EXPECT_FLOAT_EQ(directProfile.minDistance, directAfterRetry->minDistance);
        EXPECT_FLOAT_EQ(directProfile.maxDistance, directAfterRetry->maxDistance);
        VnActionFiringHandle firing = 0;
        ASSERT_NO_FATAL_FAILURE(att05PostSpatial(&firing));
        harkentest::Capture capture;
        ASSERT_NO_FATAL_FAILURE(pumpUntilAudible(engine, &capture));
        Att05Voice voice;
        EXPECT_TRUE(att05CaptureVoice(engine, referencedId, &voice));
        EXPECT_NE(0u, firing);
        float peak = 0.0f;
        for (const float sample : capture.left)
            peak = std::max(peak, std::fabs(sample));
        RecordProperty("clean_retry_peak", std::to_string(peak));
        RecordProperty("clean_retry_firing", std::to_string(firing));
        RecordProperty("direct_owner_generation_after_retry", std::to_string(
            engine->spatial.CurrentProfileGeneration(unrelatedId)));
    }
}

TEST_F(ExportedPackLifecycle, Att05UnloadRetiresOwnedResourcesAndPreservesUnrelatedDirectOwner)
{
    const QString exported = QDir(dir_.path()).filePath(QStringLiteral("att05-unrelated-owner"));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, exported));
    LoadedExportedPack loaded;
    ASSERT_EQ(VN_OK, loaded.session.InitResult());
    ASSERT_NO_FATAL_FAILURE(att05Activate(
        exported, QStringLiteral("ExportedPackSpatialNearFar"), &loaded));
    auto* engine = loaded.session.Engine();
    const VnBusId unrelatedBus = harkentest::ObjectId(999998);
    const VnObjectId unrelatedProfile = harkentest::ObjectId(999999);
    const auto directProfile = harkentest::MakeAttenuationDesc(1.0f, 40.0f);
    ASSERT_EQ(VN_OK, Vn_CreateBus(unrelatedBus, VN_BUS_MASTER));
    ASSERT_EQ(VN_OK, Vn_RegisterAttenuation(unrelatedProfile, &directProfile));
    ASSERT_EQ(VN_OK, Vn_Update());
    engine->nullBackend.Pump(1);
    const auto directBusSlot = engine->buses.FindIndex(unrelatedBus);
    ASSERT_NE(harken::kInvalidBusIndex, directBusSlot);
    const auto directBusGeneration = engine->buses.GenerationAt(directBusSlot);
    const auto directProfileGeneration = engine->spatial.CurrentProfileGeneration(unrelatedProfile);
    ASSERT_NE(nullptr, engine->spatial.FindProfile(unrelatedProfile));
    const auto before = att05Publication(engine);
    const auto beforeBusCount = engine->buses.ActiveBusCount();
    const auto beforeProfileCount = engine->spatial.ProfileCount();
    const auto beforeLoaded = engine->packs.LoadedCount();
    ASSERT_GT(beforeBusCount, 2u);
    ASSERT_EQ(2u, beforeProfileCount);
    ASSERT_EQ(2u, beforeLoaded);
    VnActionFiringHandle firing = 0;
    ASSERT_NO_FATAL_FAILURE(att05PostSpatial(&firing));
    harkentest::Capture capture;
    ASSERT_NO_FATAL_FAILURE(pumpUntilAudible(engine, &capture));
    Att05Voice oldVoice;
    ASSERT_TRUE(att05CaptureVoice(engine, att05AttenuationId(), &oldVoice));
    VnCanonicalPackSetUnloadDescV1 unload{};
    unload.structSize = sizeof(unload);
    unload.expectedInitHandle = before.initHandle;
    unload.baseSetRevision = before.setRevision;
    unload.unloadRevision = before.setRevision + 1u;
    ASSERT_EQ(VN_OK, Vn_UnloadCanonicalPackSetV1(&unload));
    const auto* emptyPublication = engine->packComposition.Published();
    ASSERT_NE(nullptr, emptyPublication);
    EXPECT_TRUE(emptyPublication->owners.empty());
    EXPECT_EQ(0u, emptyPublication->ledger.memberCount);
    EXPECT_EQ(0u, emptyPublication->ledger.initHandle);
    EXPECT_NE(nullptr, engine->spatial.FindProfile(
        att05AttenuationId(), oldVoice.state.attenuationGeneration));
    const auto duringRetiring = engine->packs.RetiringCount();
    EXPECT_GT(duringRetiring, 0u);
    ASSERT_TRUE(att05ReachTerminal(engine, {firing}));
    EXPECT_NE(nullptr, engine->spatial.FindProfile(
        att05AttenuationId(), oldVoice.state.attenuationGeneration));
    ASSERT_EQ(VN_OK, Vn_ReleaseActionFiring(firing));
    for (int block = 0; block < 8; ++block) {
        ASSERT_EQ(VN_OK, Vn_Update());
        engine->nullBackend.Pump(1);
    }
    ASSERT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(0u, engine->packs.LoadedCount());
    EXPECT_EQ(0u, engine->packs.RetiringCount());
    EXPECT_EQ(2u, engine->buses.ActiveBusCount());
    EXPECT_EQ(1u, engine->spatial.ProfileCount());
    EXPECT_EQ(0u, engine->spatial.RetainedProfileCount());
    EXPECT_EQ(nullptr, engine->spatial.FindProfile(
        att05AttenuationId(), oldVoice.state.attenuationGeneration));
    EXPECT_EQ(directBusSlot, engine->buses.FindIndex(unrelatedBus));
    EXPECT_EQ(directBusGeneration, engine->buses.GenerationAt(directBusSlot));
    EXPECT_EQ(0u, engine->busOwnerCookies[directBusSlot]);
    EXPECT_EQ(directProfileGeneration,
              engine->spatial.CurrentProfileGeneration(unrelatedProfile));
    const auto* directProfileAfter = engine->spatial.FindProfile(unrelatedProfile);
    ASSERT_NE(nullptr, directProfileAfter);
    EXPECT_EQ(unrelatedProfile, directProfileAfter->id);
    EXPECT_FLOAT_EQ(directProfile.minDistance, directProfileAfter->minDistance);
    EXPECT_FLOAT_EQ(directProfile.maxDistance, directProfileAfter->maxDistance);
    RecordProperty("loaded_before", beforeLoaded);
    RecordProperty("loaded_after", engine->packs.LoadedCount());
    RecordProperty("retiring_during_old_firing", std::to_string(duringRetiring));
    RecordProperty("retiring_after_release", std::to_string(engine->packs.RetiringCount()));
    RecordProperty("active_buses_before", beforeBusCount);
    RecordProperty("active_buses_after", engine->buses.ActiveBusCount());
    RecordProperty("profiles_before", beforeProfileCount);
    RecordProperty("profiles_after", engine->spatial.ProfileCount());
    RecordProperty("retained_profiles_after", engine->spatial.RetainedProfileCount());
    RecordProperty("direct_bus_slot", directBusSlot);
    RecordProperty("direct_bus_generation", directBusGeneration);
    RecordProperty("direct_profile_generation", std::to_string(directProfileGeneration));
    RecordProperty("old_profile_generation", std::to_string(oldVoice.state.attenuationGeneration));
}

TEST_F(ExportedPackLifecycle, Att05FatLeasesDoNotBlockUnnamedReclaim)
{
    for (uint32_t index = 0; index < 63u; ++index) {
        const uint32_t objectId = 800u + index;
        auto item = makeItem(QStringLiteral("Att05FatAttenuation%1").arg(index),
                             VNX::ProjectItem::Type::Attenuation, objectId);
        auto* component = item->comp<VNX::VAttenuation>();
        component->maxDistance = 12.0f;
        VNX::AttenuationDimensionDef volume;
        volume.type = VNX::AttenuationDimension::Volume;
        volume.curve = {{0.0f, 0.0f}, {12.0f, -96.0f}};
        VNX::attenuationNormalizeCurve(volume.type, volume.curve, volume.handles);
        component->dimensions = {volume};
        addBinding(project_.attenuationItems, item);
    }

    VnConfig config = harkentest::MakeNullConfig(4, 256, 48000);
    config.maxParkedVoices = 4;
    LoadedExportedPack loaded(config);
    ASSERT_EQ(VN_OK, loaded.session.InitResult());
    auto* engine = loaded.session.Engine();

    const QString smallDir = QDir(dir_.path()).filePath(QStringLiteral("att05-fat-small"));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, smallDir));
    ASSERT_NO_FATAL_FAILURE(att05Activate(smallDir, QStringLiteral("ExportedPackSpatialNearFar"), &loaded));
    VnActionFiringHandle spatial = 0;
    ASSERT_NO_FATAL_FAILURE(att05PostSpatial(&spatial));
    ASSERT_NE(0u, spatial);
    harkentest::Capture capture;
    ASSERT_NO_FATAL_FAILURE(pumpUntilAudible(engine, &capture));
    const auto unnamedGeneration =
        engine->spatial.CurrentProfileGeneration(att05AttenuationId());
    ASSERT_NE(0u, unnamedGeneration);

    auto* spatialAttenuation = att05Attenuation(project_);
    ASSERT_NE(nullptr, spatialAttenuation);
    const float previousMax = spatialAttenuation->maxDistance;
    spatialAttenuation->maxDistance = previousMax + 5.0f;
    for (auto& dimension : spatialAttenuation->dimensions) {
        if (dimension.type != VNX::AttenuationDimension::Volume)
            continue;
        for (auto& point : dimension.curve) {
            if (point.x == previousMax)
                point.x = spatialAttenuation->maxDistance;
        }
        VNX::attenuationNormalizeCurve(dimension.type, dimension.curve, dimension.handles);
    }
    const QString fatDir = QDir(dir_.path()).filePath(QStringLiteral("att05-fat-map"));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, fatDir));
    ASSERT_NO_FATAL_FAILURE(att05Activate(fatDir, QStringLiteral("ExportedPackSpatialNearFar"), &loaded));
    EXPECT_NE(nullptr, engine->spatial.FindProfile(att05AttenuationId(), unnamedGeneration));
    const auto namedGeneration =
        engine->spatial.CurrentProfileGeneration(att05AttenuationId());
    ASSERT_NE(unnamedGeneration, namedGeneration);
    const auto* published = engine->packComposition.Published();
    ASSERT_NE(nullptr, published);
    ASSERT_EQ(64u, published->attenuationBindings.count);

    VnActionFiringHandle first = 0;
    VnActionFiringHandle second = 0;
    ASSERT_NO_FATAL_FAILURE(att05PostSpatial(&first));
    ASSERT_NO_FATAL_FAILURE(att05PostSpatial(&second));
    harken::ActionFiringSnapshot firstSnap{};
    harken::ActionFiringSnapshot secondSnap{};
    ASSERT_TRUE(engine->actionFirings.Capture(first, &firstSnap));
    ASSERT_TRUE(engine->actionFirings.Capture(second, &secondSnap));
    ASSERT_EQ(harken::ActionFiringOwnerKind::Pack, firstSnap.owner.kind);
    ASSERT_EQ(harken::ActionFiringOwnerKind::Pack, secondSnap.owner.kind);
    ASSERT_EQ(VN_OK, engine->packComposition.ValidateLease(firstSnap.owner.pack));
    ASSERT_EQ(VN_OK, engine->packComposition.ValidateLease(secondSnap.owner.pack));
    EXPECT_NE(firstSnap.owner.pack.leaseToken, secondSnap.owner.pack.leaseToken);
    EXPECT_TRUE(engine->packComposition.HasLiveAttenuationGeneration(
        att05AttenuationId(), namedGeneration));

    ASSERT_TRUE(att05ReachTerminal(engine, {spatial}))
        << "playableLive=" << engine->playableInstances.LiveCount()
        << " voices=" << engine->voices.ActiveCount();
    EXPECT_NE(nullptr, engine->spatial.FindProfile(att05AttenuationId(), unnamedGeneration));
    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(spatial));
    EXPECT_FALSE(engine->packComposition.HasLiveAttenuationGeneration(
        att05AttenuationId(), unnamedGeneration));
    EXPECT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->spatial.FindProfile(att05AttenuationId(), unnamedGeneration))
        << "ended generation with no live lease must be reclaimed on the next Update";
    EXPECT_NE(nullptr, engine->spatial.FindProfile(att05AttenuationId(), namedGeneration));

    VnActionFiringHandle namedVoice = 0;
    ASSERT_NO_FATAL_FAILURE(att05PostSpatial(&namedVoice));
    ASSERT_NE(0u, namedVoice);
    harkentest::Capture namedCapture;
    ASSERT_NO_FATAL_FAILURE(pumpUntilAudible(engine, &namedCapture));
    EXPECT_EQ(namedGeneration,
        engine->spatial.CurrentProfileGeneration(att05AttenuationId()));
    const float namedMax = spatialAttenuation->maxDistance;
    spatialAttenuation->maxDistance = namedMax + 5.0f;
    for (auto& dimension : spatialAttenuation->dimensions) {
        if (dimension.type != VNX::AttenuationDimension::Volume)
            continue;
        for (auto& point : dimension.curve) {
            if (point.x == namedMax)
                point.x = spatialAttenuation->maxDistance;
        }
        VNX::attenuationNormalizeCurve(dimension.type, dimension.curve, dimension.handles);
    }
    const QString replacedDir = QDir(dir_.path()).filePath(QStringLiteral("att05-fat-replaced"));
    ASSERT_NO_FATAL_FAILURE(exportProjectPack(project_, replacedDir));
    ASSERT_NO_FATAL_FAILURE(att05Activate(replacedDir, QStringLiteral("ExportedPackSpatialNearFar"), &loaded));
    EXPECT_NE(namedGeneration, engine->spatial.CurrentProfileGeneration(att05AttenuationId()));
    EXPECT_NE(nullptr, engine->spatial.FindProfile(att05AttenuationId(), namedGeneration));
    ASSERT_TRUE(att05ReachTerminal(engine, {namedVoice}));
    EXPECT_EQ(VN_OK, Vn_Update());
    EXPECT_NE(nullptr, engine->spatial.FindProfile(att05AttenuationId(), namedGeneration))
        << "generation still named by a live lease must survive Update";
    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(first));
    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(second));
    EXPECT_EQ(VN_OK, Vn_ReleaseActionFiring(namedVoice));
    EXPECT_EQ(VN_OK, Vn_Update());
    EXPECT_EQ(nullptr, engine->spatial.FindProfile(att05AttenuationId(), namedGeneration));
}

TEST_F(ExportedPackLifecycle, Att06SameOfficialExportMatchesIdentityAcrossProcesses)
{
    const QString root = QFileInfo(preparedExportedPackDir()).absoluteFilePath();
    const auto snapshot = att06SnapshotTree(root);
    ASSERT_FALSE(snapshot.empty());
    const auto first = runHarkenHostBinary(att06Att04Args(root, kAtt04NearDistance));
    const auto second = runHarkenHostBinary(att06Att04Args(root, kAtt04NearDistance));
    ASSERT_EQ(0, first.exitCode) << first.output;
    ASSERT_EQ(0, second.exitCode) << second.output;
    ASSERT_TRUE(att06HasDistanceSuccess(first.output)) << first.output;
    ASSERT_TRUE(att06HasDistanceSuccess(second.output)) << second.output;

    std::vector<Att06Opened> firstOpened;
    std::vector<Att06Opened> secondOpened;
    att06CollectOpened(first.output, &firstOpened);
    att06CollectOpened(second.output, &secondOpened);
    ASSERT_GE(firstOpened.size(), 2u) << first.output;
    ASSERT_EQ(firstOpened.size(), secondOpened.size()) << second.output;
    size_t openedLines = 0;
    std::string line;
    std::istringstream countStream(first.output);
    while (std::getline(countStream, line)) {
        if (line.rfind("att04-nearfar opened ", 0) == 0)
            ++openedLines;
    }
    ASSERT_EQ(openedLines, firstOpened.size()) << first.output;

    std::vector<std::string> firstKeys;
    for (size_t i = 0; i < firstOpened.size(); ++i) {
        EXPECT_EQ(i, firstOpened[i].index);
        EXPECT_EQ(i, secondOpened[i].index);
        EXPECT_EQ(firstOpened[i].packStableId, secondOpened[i].packStableId);
        EXPECT_NE(0u, firstOpened[i].packStableId);
        const std::string key = att06CanonicalUtf8(att06FsPath(firstOpened[i].path));
        const std::string other = att06CanonicalUtf8(att06FsPath(secondOpened[i].path));
        ASSERT_FALSE(key.empty()) << firstOpened[i].path;
        ASSERT_EQ(key, other);
        const auto found = snapshot.find(key);
        ASSERT_NE(found, snapshot.end()) << key;
        EXPECT_EQ(found->second, fileSha256(att06QString(firstOpened[i].path)));
        EXPECT_EQ(found->second, fileSha256(att06QString(secondOpened[i].path)));
        firstKeys.push_back(key);
    }

    Att06Identity firstId;
    Att06Identity secondId;
    ASSERT_TRUE(att06ParseIdentity(first.output, &firstId)) << first.output;
    ASSERT_TRUE(att06ParseIdentity(second.output, &secondId)) << second.output;
    EXPECT_EQ(1u, firstId.activationGeneration);
    EXPECT_EQ(1u, secondId.activationGeneration);
    EXPECT_NE(0u, firstId.busContentGeneration);
    EXPECT_NE(0u, firstId.publishedBusFingerprint);
    EXPECT_NE(0u, firstId.compileTargetProfileFingerprint);
    EXPECT_NE(0u, firstId.initStableId);
    EXPECT_EQ(firstId.busContentGeneration, secondId.busContentGeneration);
    EXPECT_EQ(firstId.publishedBusFingerprint, secondId.publishedBusFingerprint);
    EXPECT_EQ(firstId.compileTargetProfileFingerprint, secondId.compileTargetProfileFingerprint);
    EXPECT_EQ(firstId.initStableId, secondId.initStableId);
}

TEST_F(ExportedPackLifecycle, Att06TamperedOfficialPayloadIsRejectedInNewProcess)
{
    const QString root = QFileInfo(preparedExportedPackDir()).absoluteFilePath();
    const auto snapshot = att06SnapshotTree(root);
    ASSERT_FALSE(snapshot.empty());
    const auto first = runHarkenHostBinary(att06Att04Args(root, kAtt04NearDistance));
    const auto second = runHarkenHostBinary(att06Att04Args(root, kAtt04NearDistance));
    ASSERT_EQ(0, first.exitCode) << first.output;
    ASSERT_EQ(0, second.exitCode) << second.output;
    std::vector<Att06Opened> opened;
    att06CollectOpened(first.output, &opened);
    ASSERT_GE(opened.size(), 2u) << first.output;
    Att06Identity firstId;
    ASSERT_TRUE(att06ParseIdentity(first.output, &firstId)) << first.output;

    const auto contentPath = att06FsPath(opened[1].path);
    const auto initPath = att06FsPath(opened[0].path);
    const auto initHash = fileSha256(att06QString(opened[0].path));
    const auto sourceRoot = att06FsPath(root);
    const QString copyDir = QDir(dir_.path()).filePath(QStringLiteral("att06-tamper"));
    std::error_code removeError;
    std::filesystem::remove_all(att06FsPath(copyDir), removeError);
    ASSERT_TRUE(att06CopyTree(sourceRoot, att06FsPath(copyDir)));

    std::error_code relError;
    const auto relContent = std::filesystem::relative(contentPath, sourceRoot, relError);
    ASSERT_FALSE(relError) << relError.message();
    const auto relInit = std::filesystem::relative(initPath, sourceRoot, relError);
    ASSERT_FALSE(relError) << relError.message();
    ASSERT_FALSE(relContent.empty());
    ASSERT_FALSE(relInit.empty());
    const auto copyContent = att06FsPath(copyDir) / relContent;
    const auto copyInit = att06FsPath(copyDir) / relInit;
    EXPECT_EQ(initHash, fileSha256(att06QString(att06Utf8(copyInit))));

    QFile packFile(att06QString(att06Utf8(copyContent)));
    ASSERT_TRUE(packFile.open(QIODevice::ReadOnly)) << att06Utf8(copyContent);
    auto original = packFile.readAll();
    packFile.close();
    std::vector<uint8_t> bytes(original.begin(), original.end());
    const auto before = bytes;
    uint64_t payloadOffset = 0;
    uint64_t payloadSize = 0;
    uint64_t payloadChecksum = 0;
    uint64_t directoryChecksum = 0;
    ASSERT_TRUE(att06ChoosePayload(bytes, &payloadOffset, &payloadSize, &payloadChecksum,
                                   &directoryChecksum))
        << "no legal non-catalog payload";
    bytes[static_cast<size_t>(payloadOffset)] =
        static_cast<uint8_t>(bytes[static_cast<size_t>(payloadOffset)] ^ 0x5au);
    size_t changed = 0;
    for (size_t i = 0; i < bytes.size(); ++i) {
        if (bytes[i] != before[i])
            ++changed;
    }
    ASSERT_EQ(1u, changed);
    EXPECT_NE(payloadChecksum,
              harken::Fnv1a64(bytes.data() + payloadOffset, static_cast<size_t>(payloadSize)));
    harken::PackV2Header header;
    ASSERT_TRUE(harken::ParsePackV2Header(bytes.data(), &header));
    const uint64_t dirBytes =
        uint64_t(header.entryCount) * harken::kPackV2DirectoryEntryBytes;
    EXPECT_EQ(directoryChecksum,
              harken::Fnv1a64(bytes.data() + header.directoryOffset, static_cast<size_t>(dirBytes)));
    for (uint32_t i = 0; i < header.entryCount; ++i) {
        const auto entry = harken::ParsePackV2DirectoryEntry(
            bytes.data() + header.directoryOffset +
            size_t(i) * harken::kPackV2DirectoryEntryBytes);
        if (entry.offset == payloadOffset && entry.size == payloadSize)
            continue;
        if (entry.size == 0)
            continue;
        EXPECT_EQ(0, std::memcmp(bytes.data() + entry.offset, before.data() + entry.offset,
                                 static_cast<size_t>(entry.size)));
    }
    ASSERT_TRUE(QFile::remove(att06QString(att06Utf8(copyContent))));
    QFile rewritten(att06QString(att06Utf8(copyContent)));
    ASSERT_TRUE(rewritten.open(QIODevice::WriteOnly | QIODevice::Truncate));
    ASSERT_EQ(rewritten.write(reinterpret_cast<const char*>(bytes.data()), qint64(bytes.size())),
              qint64(bytes.size()));
    rewritten.close();
    EXPECT_EQ(initHash, fileSha256(att06QString(att06Utf8(copyInit))));

    const auto tampered = runHarkenHostBinary(
        att06Att04Args(copyDir, kAtt04NearDistance));
    EXPECT_NE(0, tampered.exitCode) << tampered.output;
    EXPECT_NE(std::string::npos, tampered.output.find("native inspection failed (-401)")) << tampered.output;
    std::vector<Att06Opened> tamperedOpened;
    att06CollectOpened(tampered.output, &tamperedOpened);
    EXPECT_TRUE(tamperedOpened.empty()) << tampered.output;
    Att06Identity tamperedId;
    EXPECT_FALSE(att06ParseIdentity(tampered.output, &tamperedId)) << tampered.output;
    EXPECT_FALSE(att06HasDistanceSuccess(tampered.output)) << tampered.output;

    const auto after = att06SnapshotTree(root);
    EXPECT_EQ(snapshot, after);
    const auto third = runHarkenHostBinary(att06Att04Args(root, kAtt04NearDistance));
    ASSERT_EQ(0, third.exitCode) << third.output;
    Att06Identity thirdId;
    ASSERT_TRUE(att06ParseIdentity(third.output, &thirdId)) << third.output;
    EXPECT_EQ(firstId.busContentGeneration, thirdId.busContentGeneration);
    EXPECT_EQ(firstId.publishedBusFingerprint, thirdId.publishedBusFingerprint);
    EXPECT_EQ(firstId.compileTargetProfileFingerprint, thirdId.compileTargetProfileFingerprint);
    EXPECT_EQ(firstId.initStableId, thirdId.initStableId);
    EXPECT_EQ(1u, thirdId.activationGeneration);
    std::vector<Att06Opened> thirdOpened;
    att06CollectOpened(third.output, &thirdOpened);
    ASSERT_EQ(opened.size(), thirdOpened.size());
    for (size_t i = 0; i < opened.size(); ++i) {
        EXPECT_EQ(opened[i].index, thirdOpened[i].index);
        EXPECT_EQ(opened[i].packStableId, thirdOpened[i].packStableId);
        EXPECT_EQ(att06CanonicalUtf8(att06FsPath(opened[i].path)),
                  att06CanonicalUtf8(att06FsPath(thirdOpened[i].path)));
        EXPECT_EQ(fileSha256(att06QString(opened[i].path)),
                  fileSha256(att06QString(thirdOpened[i].path)));
    }
}

TEST_F(ExportedPackLifecycle, Att06SpacedHostBinaryLaunchesOfficialExport)
{
    QString spacedExe;
    ASSERT_NO_FATAL_FAILURE(att06StageSpacedHost(&spacedExe));
    const QString root = QFileInfo(preparedExportedPackDir()).absoluteFilePath();
    const auto run = runHarkenHostBinary(att06Att04Args(root, kAtt04NearDistance), spacedExe);
    ASSERT_EQ(0, run.exitCode) << run.output;
    std::vector<Att06Opened> opened;
    att06CollectOpened(run.output, &opened);
    ASSERT_GE(opened.size(), 2u) << run.output;
    Att06Identity identity;
    ASSERT_TRUE(att06ParseIdentity(run.output, &identity)) << run.output;
    EXPECT_NE(0u, identity.initStableId);
    EXPECT_NE(0u, identity.busContentGeneration);
    EXPECT_TRUE(att06HasDistanceSuccess(run.output)) << run.output;
}

TEST_F(ExportedPackLifecycle, Att06SpacedRootLaunchKeepsUnquotedRoot)
{
    const QString source = QFileInfo(preparedExportedPackDir()).absoluteFilePath();
    const QString copyDir = QDir(QString::fromUtf8(HARKEN_TEMP_DIR)).filePath(
        QStringLiteral("att06-space-host/space root"));
    std::error_code removeError;
    std::filesystem::remove_all(att06FsPath(copyDir), removeError);
    ASSERT_TRUE(att06CopyTree(att06FsPath(source), att06FsPath(copyDir)));
    const auto args = att06Att04Args(copyDir, kAtt04NearDistance);
    const int rootIndex = args.indexOf(QStringLiteral("--root"));
    ASSERT_GE(rootIndex, 0);
    ASSERT_LT(rootIndex + 1, args.size());
    EXPECT_FALSE(args.at(rootIndex + 1).startsWith(QLatin1Char('"')));
    EXPECT_FALSE(args.at(rootIndex + 1).endsWith(QLatin1Char('"')));
    EXPECT_TRUE(args.at(rootIndex + 1).contains(QLatin1Char(' '))) << args.at(rootIndex + 1).toStdString();
    const auto run = runHarkenHostBinary(args);
    ASSERT_EQ(0, run.exitCode) << run.output;
    std::vector<Att06Opened> opened;
    att06CollectOpened(run.output, &opened);
    ASSERT_GE(opened.size(), 2u) << run.output;
    Att06Identity identity;
    ASSERT_TRUE(att06ParseIdentity(run.output, &identity)) << run.output;
    EXPECT_NE(0u, identity.initStableId);
    EXPECT_TRUE(att06HasDistanceSuccess(run.output)) << run.output;
    const std::string rootCanon = att06CanonicalUtf8(att06FsPath(copyDir));
    ASSERT_FALSE(rootCanon.empty());
    for (const auto& row : opened) {
        const std::string key = att06CanonicalUtf8(att06FsPath(row.path));
        ASSERT_FALSE(key.empty()) << row.path;
        EXPECT_EQ(0, key.compare(0, rootCanon.size(), rootCanon)) << row.path;
        EXPECT_NE(key, rootCanon) << row.path;
    }
}

TEST_F(ExportedPackLifecycle, Att06MissingRootKeepsHostErrorAndNoSuccessLines)
{
    const QString missing = QDir(dir_.path()).filePath(QStringLiteral("missing root dir"));
    ASSERT_FALSE(QFileInfo::exists(missing));
    const auto run = runHarkenHostBinary(att06Att04Args(missing, kAtt04NearDistance));
    EXPECT_NE(0, run.exitCode) << run.output;
    EXPECT_NE(std::string::npos, run.output.find("error:")) << run.output;
    ASSERT_NO_FATAL_FAILURE(att06ExpectNoSuccessLines(run));
}

TEST_F(ExportedPackLifecycle, Att06MissingHostBinaryDoesNotReportSuccess)
{
    const QString missingExe = QDir(dir_.path()).filePath(
        QStringLiteral("missing host/harken_host.exe"));
    ASSERT_FALSE(QFileInfo::exists(missingExe));
    const QString root = QFileInfo(preparedExportedPackDir()).absoluteFilePath();
    const auto run = runHarkenHostBinary(att06Att04Args(root, kAtt04NearDistance), missingExe);
    EXPECT_EQ(-1, run.exitCode) << run.output;
    ASSERT_NO_FATAL_FAILURE(att06ExpectNoSuccessLines(run));
}
