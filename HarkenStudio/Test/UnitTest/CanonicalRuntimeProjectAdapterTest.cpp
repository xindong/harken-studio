#include "UnitTest/StudioWorldTestSupport.h"
#include "UnitTest/StudioObjectIdTestSupport.h"
#include "Core/Components/VSelection.h"
#include <harken/core/PlayablePropertyCut.h>
#pragma push_macro("slots")
#undef slots
#include <harken_project_schema/FrontEnd.h>
#include <harken_project_schema/PieceProgram.h>
#pragma pop_macro("slots")

#include "CanonicalRuntimeProjectAdapter.h"
#include "CanonicalRuntimeProjectAdapterPrivate.h"
#include "CurrentWorldProjectAdapter.h"
#include "FieldGroupProjectAdapter.h"
#include "RuntimeBridge/CurrentWorldSyncBridgeInternal.h"

#include "Core/AssetPipeline/AssetConverter.h"
#include "Core/AssetPipeline/AssetRegistry.h"
#include "Core/Components/VAudio.h"
#include "Core/Components/VBus.h"
#include "Core/Components/VMixNode.h"
#include "Core/Components/VPiece.h"
#include "Core/Components/VSound.h"
#include "Core/Components/VSpatial.h"
#include "Core/Components/VTrack.h"
#include "Core/PlayablePropertyAuthoringV1.h"
#include "Core/ProjectTypes.h"

#include <QDir>
#include <QFileInfo>
#include <QTemporaryDir>

#include <gtest/gtest.h>

#include <bit>
#include <cmath>

using namespace VNX;
using namespace HarkenEditor::Bridge;

namespace {

std::shared_ptr<ProjectItem> makeItem(ProjectItem::Type type, uint32_t objectId,
                                      harkenstudio::ObjectId stableId)
{
    auto value = std::make_shared<ProjectItem>(QStringLiteral("Item"), type);
    value->objectID = objectId;
    value->guid = VNX::authoringUuid(harkentest::StudioObjectId(stableId));
    value->attachDefaultComponents();
    return value;
}

ProjectData projectWithRoot(const std::shared_ptr<ProjectItem>& root)
{
    ProjectData project;
    auto master = makeItem(ProjectItem::Type::Bus, 1000, harkentest::StudioObjectId(1000));
    root->fieldGroups = MakeRootFieldGroups(toFieldGroupGuid(master->guid));
    ProjectFileBinding busBinding;
    busBinding.rootItem = std::move(master);
    project.busses.push_back(std::move(busBinding));
    ProjectFileBinding binding;
    binding.rootItem = root;
    project.layers.push_back(std::move(binding));
    return project;
}

class PreviewAsset
{
public:
    PreviewAsset()
    {
        AssetRegistry::instance().clear();
        AssetRegistry::instance().setProjectRoot(directory_.path());
        const QString path = QDir(directory_.path()).filePath(QStringLiteral("preview.wav"));
        const std::vector<float> samples(480, 0.25f);
        EXPECT_TRUE(AssetConverter::writeFloatWav(path, samples, 1, 48000));
        AssetInfo asset;
        asset.sourceID = 1;
        asset.absolutePath = path;
        asset.relativePath = QStringLiteral("preview.wav");
        asset.metadata.sampleRate = 48000;
        asset.metadata.channels = 1;
        asset.metadata.totalSamples = samples.size();
        asset.metadata.durationSec = 0.01;
        asset.metadata.fileSize = QFileInfo(path).size();
        EXPECT_TRUE(AssetRegistry::instance().registerAsset(asset));
    }

    ~PreviewAsset()
    {
        AssetRegistry::instance().clear();
    }

    void attach(ProjectItem& sound) const
    {
        sound.sourceID = 1;
        auto* component = sound.comp<VSound>();
        ASSERT_NE(component, nullptr);
        SoundSourceDef source;
        source.entryId = QUuid::createUuid();
        source.sourceId = 1;
        component->sources.plain.entries.push_back(source);
        component->sources.plain.active = source.entryId;
    }

private:
    QTemporaryDir directory_;
};

}

TEST(CanonicalRuntimeProjectAdapterTest, CompilesFiveKindsAndOwnership)
{
    auto layer = makeItem(ProjectItem::Type::Layer, 1, harkentest::StudioObjectId(11));
    auto sound = makeItem(ProjectItem::Type::Sound, 2, harkentest::StudioObjectId(12));
    auto container = makeItem(ProjectItem::Type::Container, 3, harkentest::StudioObjectId(13));
    auto piece = makeItem(ProjectItem::Type::Piece, 4, harkentest::StudioObjectId(14));
    auto track = makeItem(ProjectItem::Type::Track, 5, harkentest::StudioObjectId(15));
    piece->children.push_back(track);
    track->parent = piece.get();
    layer->children = {sound, container, piece};
    for (auto& child : layer->children) child->parent = layer.get();

    const auto result = compileStudioProjectInput(projectWithRoot(layer));
    ASSERT_TRUE(result.ok()) << result.diagnostics.front().message.toStdString();
    EXPECT_FALSE(result.canonicalProject.isEmpty());

}

TEST(CanonicalRuntimeProjectAdapterTest, MapsTrackClipAndPieceMarkers)
{
    PreviewAsset asset;
    auto piece = makeItem(ProjectItem::Type::Piece, 1, harkentest::StudioObjectId(11));
    auto track = makeItem(ProjectItem::Type::Track, 2, harkentest::StudioObjectId(12));
    auto sound = makeItem(ProjectItem::Type::Sound, 3, harkentest::StudioObjectId(13));
    asset.attach(*sound);
    piece->children.push_back(track);
    track->parent = piece.get();
    track->children.push_back(sound);
    sound->parent = track.get();

    Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.clipGuid = VNX::authoringUuid(harkentest::StudioObjectId(21));
    clip.targetGuid = sound->guid;
    clip.start = 42;
    track->comp<VTrack>()->clips.items.push_back(clip);
    Marker exit;
    exit.markerGuid = QUuid::createUuid();
    exit.markerGuid = VNX::authoringUuid(harkentest::StudioObjectId(22));
    exit.kind = MarkerKind::Exit;
    exit.position = 240;
    piece->comp<VPiece>()->markers.items.push_back(exit);

    const auto result = compileStudioProjectInput(projectWithRoot(piece));
    ASSERT_TRUE(result.ok()) << result.diagnostics.front().message.toStdString();
    EXPECT_FALSE(result.canonicalProject.isEmpty());
    ASSERT_EQ(result.resources.size(), 1u);
    EXPECT_EQ(result.resources.front().soundStableId, harkentest::StudioObjectId(13u));
}

TEST(CanonicalRuntimeProjectAdapterTest, AcceptsFiniteLoopCount)
{
    PreviewAsset asset;
    auto sound = makeItem(ProjectItem::Type::Sound, 1, harkentest::StudioObjectId(11));
    asset.attach(*sound);
    sound->comp<VAudio>()->loopCount = 3;
    sound->playablePropertyPresence.Set(
        harkenplayableproperties::PlayablePropertyIdV1::LoopCount);
    const auto result = compileStudioProjectInput(projectWithRoot(sound));
    ASSERT_TRUE(result.ok()) << result.diagnostics.front().message.toStdString();
}

TEST(CanonicalRuntimeProjectAdapterTest, AcceptsAuthoredSpatializationMix)
{
    PreviewAsset asset;
    auto sound = makeItem(ProjectItem::Type::Sound, 1,
                          harkentest::StudioObjectId(11));
    asset.attach(*sound);
    auto project = projectWithRoot(sound);
    ASSERT_TRUE(sound->fieldGroups.spatialRating.shared.localHistory.has_value());
    constexpr auto property =
        harkenplayableproperties::PlayablePropertyIdV1::SpatializationMix;
    constexpr std::size_t slot = static_cast<std::size_t>(property) - 1;
    for (const float authored : {0.0f, 0.5f, 1.0f}) {
        sound->comp<VSpatial>()->spatializationMix = authored;
        const auto result = compileStudioProjectInput(project);
        ASSERT_TRUE(result.ok());

        QString error;
        auto request = buildCurrentWorldSyncRequest(project, {}, &error);
        ASSERT_NE(request, nullptr) << error.toStdString();
        for (auto& materialization : request->materializations)
            materialization.contentRevision = 1;
        const auto front = harkenprojectschema::CompileFrontEnd(request->project);
        ASSERT_TRUE(front.ok());
        auto world = harkencurrentworldcompiler::CompileCurrentWorldV1(
            *front.project, request->variables, request->materializations,
            request->containers);
        ASSERT_TRUE(world.ok());
        harken::CurrentWorldBodyImage image;
        ASSERT_TRUE(harkentest::DecodeStudioWorld(world.world->View(), image));
        const auto& runtime = *image.ProductionView();
        ASSERT_EQ(1u, runtime.playablePropertyCount);
        const auto& payload = runtime.playableProperties[0].payload;
        EXPECT_NE(0u, payload.presentMask & (UINT64_C(1) << slot));
        EXPECT_FLOAT_EQ(
            authored,
            std::bit_cast<float>(
                static_cast<std::uint32_t>(payload.values[slot].bits)));
    }
}

TEST(CanonicalRuntimeProjectAdapterTest, ComponentMixUsesOwnershipWithoutFreezingSpatialGroup)
{
    PreviewAsset asset;
    auto layer = makeItem(ProjectItem::Type::Layer, 1, harkentest::StudioObjectId(11));
    auto sound = makeItem(ProjectItem::Type::Sound, 2, harkentest::StudioObjectId(12));
    asset.attach(*sound);
    layer->children.push_back(sound);
    auto project = projectWithRoot(layer);
    sound->comp<VSpatial>()->spatializationMixPresent = false;
    const auto childGroups = sound->fieldGroups;
    const auto compileMix = [&]() -> float {
        QString error;
        auto request = buildCurrentWorldSyncRequest(project, {}, &error);
        if (!request) { ADD_FAILURE() << error.toStdString(); return -1; }
        for (auto& materialization : request->materializations) materialization.contentRevision = 1;
        auto front = harkenprojectschema::CompileFrontEnd(request->project);
        if (!front.ok()) { ADD_FAILURE(); return -1; }
        auto world = harkencurrentworldcompiler::CompileCurrentWorldV1(
            *front.project, request->variables, request->materializations, request->containers);
        if (!world.ok()) { ADD_FAILURE(); return -1; }
        harken::CurrentWorldBodyImage image;
        if (!harkentest::DecodeStudioWorld(world.world->View(), image)) { ADD_FAILURE(); return -1; }
        const auto& runtime = *image.ProductionView();
        for (uint32_t i = 0; i < runtime.playablePropertyCount; ++i) {
            const auto& row = runtime.playableProperties[i];
            if (row.stableId == sound->runtimeId())
                return std::bit_cast<float>(static_cast<uint32_t>(row.payload.values[16].bits));
        }
        ADD_FAILURE() << "Sound payload missing";
        return -1;
    };
    // The canonical index accepts absent back-pointers and uses children ownership.
    ASSERT_EQ(sound->parent, nullptr);
    for (float mix : {0.0f, 0.5f, 1.0f}) {
        layer->comp<VSpatial>()->spatializationMix = mix;
        const auto index = canonicalRuntimeProjectSourceIndex(project);
        ASSERT_TRUE(index && index->valid);
        EXPECT_FLOAT_EQ(resolvedStudioSpatializationMix(*sound, *index), mix);
        // CurrentWorld admission additionally requires installed parent back-pointers.
        sound->parent = layer.get();
        EXPECT_FLOAT_EQ(compileMix(), mix);
        sound->parent = nullptr;
    }
    sound->parent = layer.get();
    const auto locator = *PlayablePropertyAuthoringV1::definitionLocator(*sound);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::setOverride(*sound, locator, 17, 1.0, std::nullopt));
    auto& spatial = *layer->fieldGroups.spatialRating.shared.localHistory;
    spatial.spatialPanning = 0.25f;
    spatial.spatialAttenuationDistanceScaling = 2.0f;
    layer->comp<VSpatial>()->spatializationMix = 0.5f;
    EXPECT_FLOAT_EQ(compileMix(), 1.0f);
    auto resolved = buildFieldGroupEffectiveSnapshot(project, std::nullopt);
    ASSERT_TRUE(resolved.ok());
    EXPECT_EQ(resolved.definitions.at(toFieldGroupGuid(sound->guid)).spatialRating, spatial);
    EXPECT_EQ(sound->fieldGroups, childGroups);
    ASSERT_TRUE(PlayablePropertyAuthoringV1::clearOverride(*sound, locator, 17, std::nullopt));
    EXPECT_FLOAT_EQ(compileMix(), 0.5f);
    layer->comp<VSpatial>()->spatializationMixPresent = false;
    EXPECT_FLOAT_EQ(compileMix(), 1.0f);
}

TEST(CanonicalRuntimeProjectAdapterTest,
     RejectsNonNeutralBusPropertiesBeforeRuntimePublication)
{
    auto layer = makeItem(ProjectItem::Type::Layer, 1, harkentest::StudioObjectId(11));
    ProjectData project = projectWithRoot(layer);
    ProjectItem* master = project.busses.front().rootItem.get();
    ASSERT_NE(master, nullptr);
    auto* mix = master->comp<VMixNode>();
    auto* bus = master->comp<VBus>();
    ASSERT_NE(mix, nullptr);
    ASSERT_NE(bus, nullptr);
    mix->outputBusVolume = -1.0f;
    master->playablePropertyPresence.Set(
        harkenplayableproperties::PlayablePropertyIdV1::OutputBusVolume);

    const auto neutral = compileStudioProjectInput(project);
    ASSERT_TRUE(neutral.ok()) << neutral.diagnostics.front().message.toStdString();

    const auto rejectedField = [&]() {
        const auto result = compileStudioProjectInput(project);
        EXPECT_FALSE(result.ok());
        EXPECT_EQ(result.busResult, nullptr);
        EXPECT_EQ(result.diagnostics.size(), 1u);
        if (result.diagnostics.empty()) return QString{};
        EXPECT_EQ(result.diagnostics.front().code,
                  ProjectInputDiagnosticCode::UnsupportedField);
        EXPECT_EQ(result.diagnostics.front().itemGuid, master->guid);
        return result.diagnostics.front().field;
    };
    mix->pitch = 300.0f;
    master->playablePropertyPresence.Set(
        harkenplayableproperties::PlayablePropertyIdV1::Pitch);
    EXPECT_EQ(rejectedField(), QStringLiteral("pitch"));
    mix->pitch = 0.0f;
    bus->pan = -0.25f;
    EXPECT_EQ(rejectedField(), QStringLiteral("pan"));
    bus->pan = 0.0f;
    bus->listenerRelativeRouting = true;
    EXPECT_EQ(rejectedField(), QStringLiteral("listenerRelativeRouting"));
}

TEST(CanonicalRuntimeProjectAdapterTest, PreservesAuthoredClipDuration)
{
    PreviewAsset asset;
    auto track = makeItem(ProjectItem::Type::Track, 1, harkentest::StudioObjectId(11));
    auto sound = makeItem(ProjectItem::Type::Sound, 2, harkentest::StudioObjectId(12));
    asset.attach(*sound);
    track->children.push_back(sound);
    sound->parent = track.get();
    Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.clipGuid = VNX::authoringUuid(harkentest::StudioObjectId(21));
    clip.targetGuid = sound->guid;
    clip.duration = 100;
    track->comp<VTrack>()->clips.items.push_back(clip);
    const auto result = compileStudioProjectInput(projectWithRoot(track));
    ASSERT_TRUE(result.ok()) << result.diagnostics.front().message.toStdString();
    EXPECT_TRUE(result.canonicalProject.contains("duration=100"));
}

TEST(CanonicalRuntimeProjectAdapterTest, RejectsTrackClipTarget)
{
    auto layer = makeItem(ProjectItem::Type::Layer, 1, harkentest::StudioObjectId(11));
    auto sourceTrack = makeItem(ProjectItem::Type::Track, 2, harkentest::StudioObjectId(12));
    auto targetTrack = makeItem(ProjectItem::Type::Track, 3, harkentest::StudioObjectId(13));
    layer->children = {sourceTrack, targetTrack};
    sourceTrack->parent = layer.get();
    targetTrack->parent = layer.get();
    Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.clipGuid = VNX::authoringUuid(harkentest::StudioObjectId(21));
    clip.targetGuid = targetTrack->guid;
    sourceTrack->comp<VTrack>()->clips.items.push_back(clip);
    const auto result = compileStudioProjectInput(projectWithRoot(layer));
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.diagnostics.front().code,
              ProjectInputDiagnosticCode::InvalidReferenceType);
}

TEST(CanonicalRuntimeProjectAdapterTest, RejectsDuplicateStableIdentity)
{
    auto layer = makeItem(ProjectItem::Type::Layer, 1, harkentest::StudioObjectId(11));
    auto first = makeItem(ProjectItem::Type::Sound, 2, harkentest::StudioObjectId(12));
    auto second = makeItem(ProjectItem::Type::Sound, 3, harkentest::StudioObjectId(12));
    layer->children = {first, second};
    first->parent = layer.get();
    second->parent = layer.get();
    const auto result = compileStudioProjectInput(projectWithRoot(layer));
    ASSERT_FALSE(result.ok());
    EXPECT_EQ(result.diagnostics.front().code,
              ProjectInputDiagnosticCode::DuplicateIdentity);
}

TEST(CanonicalRuntimeProjectAdapterTest, LegacyClipMixIsIgnoredByBothAdaptersAndKeepsGroupOutputVolume)
{
    using Property = harkenplayableproperties::PlayablePropertyIdV1;
    PreviewAsset asset;
    auto track = makeItem(ProjectItem::Type::Track, 1, harkentest::StudioObjectId(11));
    auto sound = makeItem(ProjectItem::Type::Sound, 2, harkentest::StudioObjectId(12));
    track->children.push_back(sound);
    sound->parent = track.get();
    asset.attach(*sound);
    auto project = projectWithRoot(track);
    track->fieldGroups.mainOutput.shared = {
        FieldGroupMode::Local,
        MainOutputFieldGroup{toFieldGroupGuid(project.busses.front().rootItem->guid), -11.0f}};
    track->comp<VMixNode>()->volume = -4.0f;
    track->comp<VMixNode>()->pitch = 300.0f;
    track->comp<VMixNode>()->makeUpGain = 1.0f;
    sound->comp<VMixNode>()->volume = -2.0f;
    sound->comp<VMixNode>()->pitch = 200.0f;
    for (const auto& item : {track, sound})
        for (const auto property : {Property::Volume, Property::Pitch, Property::MakeUpGain})
            item->playablePropertyPresence.Set(property);
    Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.clipGuid = VNX::authoringUuid(harkentest::StudioObjectId(21));
    clip.targetGuid = sound->guid;
    auto* timeline = track->comp<VTrack>();
    timeline->clips.items.push_back(clip);
    for (const auto& [property, value] : {
             std::pair{Property::Volume, -3.0f}, std::pair{Property::Pitch, 700.0f},
             std::pair{Property::MakeUpGain, 2.0f}}) {
        auto& properties = timeline->clips.items.front().playableProperties;
        ASSERT_TRUE(properties.setValue(property, value));
        properties.presence.Set(property);
    }
    const auto compiled = compileStudioProjectInput(project);
    ASSERT_TRUE(compiled.ok()) << compiled.diagnostics.front().message.toStdString();
    const auto* program = compiledPieceProgram(*compiled.snapshot);
    ASSERT_NE(program, nullptr);
    ASSERT_EQ(program->ir().tracks().size(), 1u);
    ASSERT_EQ(program->ir().tracks()[0].occurrences.size(), 1u);
    const auto& occurrence = program->ir().tracks()[0].occurrences[0];
    EXPECT_FLOAT_EQ(occurrence.baseVolumeDb, -5.0f);
    EXPECT_FLOAT_EQ(occurrence.basePitchRatio, std::exp2(500.0f / 1200.0f));
    EXPECT_FLOAT_EQ(occurrence.outputBusVolumeDb, -11.0f);

    QString error;
    const auto request = buildCurrentWorldSyncRequest(project, {}, &error);
    ASSERT_NE(request, nullptr) << error.toStdString();
    for (auto& materialization : request->materializations)
        if (materialization.kind == harkenplayableproperties::ContentPropertyKindV1::Sound)
            materialization.contentRevision = 1;
    const auto front = harkenprojectschema::CompileFrontEnd(request->project);
    ASSERT_TRUE(front.ok());
    auto world = harkencurrentworldcompiler::CompileCurrentWorldV1(
        *front.project, request->variables, request->materializations, request->containers);
    ASSERT_TRUE(world.ok());
    harken::CurrentWorldBodyImage image;
    ASSERT_TRUE(harkentest::DecodeStudioWorld(world.world->View(), image));
    const auto& view = *image.ProductionView();
    harken::PlayableWorldViewV1 playback;
    playback.generation = 1;
    playback.identity = view.identity;
    playback.properties = view.playableProperties;
    playback.propertyCount = view.playablePropertyCount;
    playback.occurrences = view.playableOccurrences;
    playback.occurrenceCount = view.playableOccurrenceCount;
    playback.trackSchedules = view.trackSchedules;
    playback.trackScheduleCount = view.trackScheduleCount;
    ASSERT_EQ(playback.trackScheduleCount, 1u);
    const auto root = playback.trackSchedules[0].rootDefinition;
    const uint32_t ordinal = 0;
    harken::ResolvedPlayablePropertyCutStorageV1 cut;
    ASSERT_TRUE(cut.Reset({}));
    ASSERT_EQ(cut.Resolve(playback, root, &ordinal, 1),
              harken::ResolvedPlayablePropertyCutStatusV1::Ok);
    EXPECT_FLOAT_EQ(cut.View().volumeDb, -6.0f);
    EXPECT_FLOAT_EQ(cut.View().makeUpGainDb, 1.0f);
    EXPECT_FLOAT_EQ(cut.View().pitchCents, 500.0f);
    EXPECT_FLOAT_EQ(occurrence.baseVolumeDb, cut.View().volumeDb + cut.View().makeUpGainDb);
    EXPECT_FLOAT_EQ(occurrence.basePitchRatio, std::exp2(cut.View().pitchCents / 1200.0f));
    EXPECT_FLOAT_EQ(occurrence.outputBusVolumeDb, cut.View().outputBusVolumeDb);
}

TEST(CanonicalRuntimeProjectAdapterTest, NonPlayableOwnershipContainerKeepsPieceIrMixAndGroups)
{
    using Property = harkenplayableproperties::PlayablePropertyIdV1;
    PreviewAsset asset;
    auto layer = makeItem(ProjectItem::Type::Layer, 1, harkentest::StudioObjectId(11));
    auto container = makeItem(ProjectItem::Type::Container, 2, harkentest::StudioObjectId(12));
    auto sound = makeItem(ProjectItem::Type::Sound, 3, harkentest::StudioObjectId(13));
    auto track = makeItem(ProjectItem::Type::Track, 4, harkentest::StudioObjectId(14));
    layer->children = {container};
    container->parent = layer.get();
    container->children = {track};
    track->parent = container.get();
    track->children = {sound};
    sound->parent = track.get();
    asset.attach(*sound);
    auto project = projectWithRoot(layer);
    const auto master = project.busses.front().rootItem;
    auto sendBus = makeItem(ProjectItem::Type::Bus, 5, harkentest::StudioObjectId(15));
    master->children.push_back(sendBus);
    sendBus->parent = master.get();
    auto* mix = container->comp<VMixNode>();
    mix->volume = -6.0f;
    mix->pitch = 1200.0f;
    mix->makeUpGain = 2.0f;
    for (const auto property : {Property::Volume, Property::Pitch, Property::MakeUpGain})
        container->playablePropertyPresence.Set(property);
    container->fieldGroups.mainOutput.shared = {
        FieldGroupMode::Local, MainOutputFieldGroup{toFieldGroupGuid(master->guid), -9.0f}};
    container->fieldGroups.send.shared = {
        FieldGroupMode::Local, SendFieldGroup{{{toFieldGroupGuid(sendBus->guid), -4.0f}}}};
    SpatialRatingFieldGroup rating;
    rating.spatialPanning = 0.25f;
    rating.spatialIs3DEnabled = true;
    rating.spatialAttenuationDistanceScaling = 2.0f;
    container->fieldGroups.spatialRating.shared = {FieldGroupMode::Local, rating};
    Clip clip;
    clip.clipGuid = QUuid::createUuid();
    clip.clipGuid = VNX::authoringUuid(harkentest::StudioObjectId(21));
    clip.targetGuid = sound->guid;
    track->comp<VTrack>()->clips.items.push_back(clip);
    ASSERT_TRUE(container->comp<VNX::VSelection>()->selections.shared.occurrenceRows().empty());
    EXPECT_FALSE(isCurrentWorldPlayable(project, *container));

    const auto compiled = compileStudioProjectInput(project);
    ASSERT_TRUE(compiled.ok()) << compiled.diagnostics.front().message.toStdString();
    const auto* program = compiledPieceProgram(*compiled.snapshot);
    ASSERT_NE(program, nullptr);
    ASSERT_EQ(program->ir().tracks().size(), 1u);
    ASSERT_EQ(program->ir().tracks()[0].occurrences.size(), 1u);
    const auto& occurrence = program->ir().tracks()[0].occurrences[0];
    EXPECT_FLOAT_EQ(occurrence.baseVolumeDb, -4.0f);
    EXPECT_FLOAT_EQ(occurrence.basePitchRatio, 2.0f);
    EXPECT_FLOAT_EQ(occurrence.outputBusVolumeDb, -9.0f);
    EXPECT_EQ(occurrence.resolvedOutputBus, master->runtimeId());
    ASSERT_EQ(occurrence.sends.size(), 1u);
    EXPECT_EQ(occurrence.sends[0].targetBusId, sendBus->runtimeId());
    EXPECT_FLOAT_EQ(occurrence.sends[0].sendLevel, -4.0f);
    EXPECT_FLOAT_EQ(occurrence.panning, 0.25f);
    EXPECT_FLOAT_EQ(occurrence.spread, 0.0f);
    EXPECT_TRUE(occurrence.is3DEnabled);
    EXPECT_FLOAT_EQ(occurrence.attenuationDistanceScaling, 2.0f);
}
