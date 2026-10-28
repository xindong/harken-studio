#include "Common/ObjectIdTestSupport.h"
#include "harken/core/EngineConfig.h"
#include "harken/core/PackPlaybackView.h"
#include "harken/core/PlayablePropertyCut.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <initializer_list>

namespace {

harken::PlayablePropertyPayloadViewV1 ResolvedPayload() {
    harken::PlayablePropertyPayloadViewV1 payload{};
    payload.presentMask = harken::kResolvedSectionGroupPropertyMaskV1;
    payload.values[3].bits = 1;
    payload.values[6].objectId = VN_BUS_MASTER;
    payload.values[11].bits = std::bit_cast<std::uint32_t>(1.0f);
    payload.values[16].bits = std::bit_cast<std::uint32_t>(1.0f);
    return payload;
}

harken::PlayableRefV1 Ref(harken::PlayableKindV1 kind, std::uint64_t id,
                         std::uint64_t fingerprint) {
    harken::PlayableRefV1 ref{};
    ref.kind = kind;
    ref.stableId = harkentest::ObjectId(id);
    ref.expectedProgramSemanticFingerprint = fingerprint;
    ref.contentRevision = id * 10;
    ref.routeStableId = harkentest::ObjectId(id * 100);
    ref.compatibilityFingerprint = id * 1000;
    return ref;
}

harken::PlayablePropertyProgramViewV1 Property(
    harken::playableproperties::ContentPropertyKindV1 kind,
    const harken::PlayableRefV1& ref) {
    harken::PlayablePropertyProgramViewV1 row{};
    row.payload = ResolvedPayload();
    row.contentPropertyKind = kind;
    row.stableId = ref.stableId;
    row.programSemanticFingerprint = ref.expectedProgramSemanticFingerprint;
    row.contentRevision = ref.contentRevision;
    row.routeStableId = ref.routeStableId;
    row.compatibilityFingerprint = ref.compatibilityFingerprint;
    return row;
}

harken::PackVoiceBinding HostBinding(VnObjectId objectId, VnBindingId bindingId) {
    harken::PackVoiceBinding row{};
    row.objectId = objectId;
    row.bindingId = bindingId;
    return row;
}

void ExpectBindingIds(const harken::PackVoiceBinding** rows, std::uint32_t count,
                      std::initializer_list<VnBindingId> ids) {
    ASSERT_EQ(count, static_cast<std::uint32_t>(ids.size()));
    std::uint32_t index = 0;
    for (VnBindingId id : ids) {
        ASSERT_NE(rows[index], nullptr);
        EXPECT_EQ(rows[index]->bindingId, id);
        ++index;
    }
}

} // namespace

TEST(OccurrencePath, NonFirstTrackBindingsAttachAndSiblingDoesNot) {
    using Kind = harken::playableproperties::ContentPropertyKindV1;
    const auto sound = Ref(harken::PlayableKindV1::Sound, 10, 110);
    const auto track0 = Ref(harken::PlayableKindV1::Track, 30, 130);
    const auto track1 = Ref(harken::PlayableKindV1::Track, 40, 140);
    const auto segment = Ref(harken::PlayableKindV1::Segment, 50, 150);
    std::array properties{
        Property(Kind::Sound, sound), Property(Kind::Track, track0),
        Property(Kind::Track, track1), Property(Kind::Segment, segment)};
    std::array<harken::ScheduleClipPlacementV1, 2> placements{};
    placements[0].content = sound;
    placements[0].stableOccurrenceOrdinal = 0;
    placements[1].content = sound;
    placements[1].stableOccurrenceOrdinal = 7;
    std::array<harken::TrackScheduleProgramViewV1, 2> tracks{};
    tracks[0].rootDefinition = track0;
    tracks[0].placementCount = 1;
    tracks[0].placements = &placements[0];
    tracks[0].scheduleSemanticFingerprint = 500;
    tracks[1].rootDefinition = track1;
    tracks[1].placementCount = 1;
    tracks[1].placements = &placements[1];
    tracks[1].scheduleSemanticFingerprint = 501;
    std::array<harken::PlayableOccurrencePropertyViewV1, 2> occurrences{};
    occurrences[0].parentKind = Kind::Track;
    occurrences[0].parentStableId = track0.stableId;
    occurrences[0].parentProgramSemanticFingerprint =
        track0.expectedProgramSemanticFingerprint;
    occurrences[0].stableOccurrenceOrdinal = 0;
    occurrences[0].childPlayable = sound;
    occurrences[0].occurrenceSemanticFingerprint = 600;
    occurrences[1].parentKind = Kind::Track;
    occurrences[1].parentStableId = track1.stableId;
    occurrences[1].parentProgramSemanticFingerprint =
        track1.expectedProgramSemanticFingerprint;
    occurrences[1].stableOccurrenceOrdinal = 7;
    occurrences[1].childPlayable = sound;
    occurrences[1].occurrenceSemanticFingerprint = 601;
    std::array<harken::ScheduleTrackRefV1, 2> trackRefs{};
    trackRefs[0].trackDefinition = track0;
    trackRefs[0].trackScheduleSemanticFingerprint = 500;
    trackRefs[0].stableTrackOrdinal = 0;
    trackRefs[1].trackDefinition = track1;
    trackRefs[1].trackScheduleSemanticFingerprint = 501;
    trackRefs[1].stableTrackOrdinal = 1;
    harken::SegmentScheduleProgramViewV1 segmentSchedule{};
    segmentSchedule.rootDefinition = segment;
    segmentSchedule.trackRefCount = 2;
    segmentSchedule.trackRefs = trackRefs.data();
    harken::PlayableWorldViewV1 world{};
    world.generation = 1;
    world.identity.runtimeSemanticFingerprint = 1;
    world.properties = properties.data();
    world.propertyCount = properties.size();
    world.occurrences = occurrences.data();
    world.occurrenceCount = occurrences.size();
    world.trackSchedules = tracks.data();
    world.trackScheduleCount = tracks.size();
    world.segmentSchedules = &segmentSchedule;
    world.segmentScheduleCount = 1;

    harken::ResolvedPlayablePropertyCutStorageV1 storage;
    ASSERT_TRUE(storage.Reset({}));
    const std::uint32_t ordinal = 7;
    auto trackId = track1.stableId;
    ASSERT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::Ok,
              storage.Resolve(world, segment, &ordinal, 1, &trackId));
    const auto& cut = storage.View();
    EXPECT_EQ(sound.stableId, cut.leaf.stableId);
    bool sawTrack1Occurrence = false;
    bool sawTrack0 = false;
    for (std::uint32_t i = 0; i < cut.pathCount; ++i) {
        const auto& row = cut.path[i];
        if (row.stableId == track0.stableId)
            sawTrack0 = true;
        if (row.rowKind == harken::ResolvedPlayablePathRowKindV1::Occurrence) {
            EXPECT_EQ(row.stableId, track1.stableId);
            EXPECT_EQ(row.stableOccurrenceOrdinal, 7u);
            sawTrack1Occurrence = true;
        }
    }
    EXPECT_TRUE(sawTrack1Occurrence);
    EXPECT_FALSE(sawTrack0);

    const harken::PackVoiceBinding catalog[] = {
        HostBinding(track0.stableId, harkentest::ObjectId(11)), HostBinding(track1.stableId, harkentest::ObjectId(12))};
    const harken::PackVoiceBinding* resolved[harken::kMaxVoiceBindingInstanceSlots]{};
    const std::uint32_t count = harken::CollectCutVoiceBindings(
        cut.path, cut.pathCount, catalog, 2, resolved,
        harken::kMaxVoiceBindingInstanceSlots);
    ExpectBindingIds(resolved, count, {harkentest::ObjectId(12)});
}

TEST(OccurrencePath, NearestExplicitReplacesInheritedList) {
    using Kind = harken::playableproperties::ContentPropertyKindV1;
    const auto sound = Ref(harken::PlayableKindV1::Sound, 30, 130);
    harken::PlayablePropertyProgramViewV1 layer = Property(Kind::Layer, Ref(
        harken::PlayableKindV1::Sound, 1, 101));
    layer.contentPropertyKind = Kind::Layer;
    layer.stableId = harkentest::ObjectId(1);
    layer.programSemanticFingerprint = 101;
    auto soundRow = Property(Kind::Sound, sound);
    harken::LayerAncestorRefV1 ancestor{harkentest::ObjectId(1), 101};
    soundRow.organizationLayerAncestorCount = 1;
    soundRow.organizationLayerAncestors = &ancestor;
    harken::PlayablePropertyProgramViewV1 properties[] = {layer, soundRow};
    harken::PlayableWorldViewV1 world{};
    world.generation = 1;
    world.identity.runtimeSemanticFingerprint = 1;
    world.properties = properties;
    world.propertyCount = 2;

    harken::ResolvedPlayablePropertyCutStorageV1 storage;
    ASSERT_TRUE(storage.Reset({}));
    ASSERT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::Ok,
              storage.Resolve(world, sound, nullptr, 0));
    const harken::PackVoiceBinding catalog[] = {
        HostBinding(harkentest::ObjectId(1), harkentest::ObjectId(21)), HostBinding(sound.stableId, harkentest::ObjectId(22))};
    const harken::PackVoiceBinding* resolved[harken::kMaxVoiceBindingInstanceSlots]{};
    const std::uint32_t count = harken::CollectCutVoiceBindings(
        storage.View().path, storage.View().pathCount, catalog, 2, resolved,
        harken::kMaxVoiceBindingInstanceSlots);
    ExpectBindingIds(resolved, count, {harkentest::ObjectId(22)});
}

TEST(OccurrencePath, AbsenceInheritsAncestor) {
    using Kind = harken::playableproperties::ContentPropertyKindV1;
    const auto sound = Ref(harken::PlayableKindV1::Sound, 30, 130);
    harken::PlayablePropertyProgramViewV1 layer = Property(Kind::Layer, Ref(
        harken::PlayableKindV1::Sound, 1, 101));
    layer.contentPropertyKind = Kind::Layer;
    layer.stableId = harkentest::ObjectId(1);
    layer.programSemanticFingerprint = 101;
    auto soundRow = Property(Kind::Sound, sound);
    harken::LayerAncestorRefV1 ancestor{harkentest::ObjectId(1), 101};
    soundRow.organizationLayerAncestorCount = 1;
    soundRow.organizationLayerAncestors = &ancestor;
    harken::PlayablePropertyProgramViewV1 properties[] = {layer, soundRow};
    harken::PlayableWorldViewV1 world{};
    world.generation = 1;
    world.identity.runtimeSemanticFingerprint = 1;
    world.properties = properties;
    world.propertyCount = 2;

    harken::ResolvedPlayablePropertyCutStorageV1 storage;
    ASSERT_TRUE(storage.Reset({}));
    ASSERT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::Ok,
              storage.Resolve(world, sound, nullptr, 0));
    const harken::PackVoiceBinding catalog[] = {HostBinding(harkentest::ObjectId(1), harkentest::ObjectId(21))};
    const harken::PackVoiceBinding* resolved[harken::kMaxVoiceBindingInstanceSlots]{};
    const std::uint32_t count = harken::CollectCutVoiceBindings(
        storage.View().path, storage.View().pathCount, catalog, 1, resolved,
        harken::kMaxVoiceBindingInstanceSlots);
    ExpectBindingIds(resolved, count, {harkentest::ObjectId(21)});
}

TEST(OccurrencePath, ExplicitEmptySuppressesInherited) {
    using Kind = harken::playableproperties::ContentPropertyKindV1;
    const auto sound = Ref(harken::PlayableKindV1::Sound, 30, 130);
    harken::PlayablePropertyProgramViewV1 layer = Property(Kind::Layer, Ref(
        harken::PlayableKindV1::Sound, 1, 101));
    layer.contentPropertyKind = Kind::Layer;
    layer.stableId = harkentest::ObjectId(1);
    layer.programSemanticFingerprint = 101;
    auto soundRow = Property(Kind::Sound, sound);
    harken::LayerAncestorRefV1 ancestor{harkentest::ObjectId(1), 101};
    soundRow.organizationLayerAncestorCount = 1;
    soundRow.organizationLayerAncestors = &ancestor;
    harken::PlayablePropertyProgramViewV1 properties[] = {layer, soundRow};
    harken::PlayableWorldViewV1 world{};
    world.generation = 1;
    world.identity.runtimeSemanticFingerprint = 1;
    world.properties = properties;
    world.propertyCount = 2;

    harken::ResolvedPlayablePropertyCutStorageV1 storage;
    ASSERT_TRUE(storage.Reset({}));
    ASSERT_EQ(harken::ResolvedPlayablePropertyCutStatusV1::Ok,
              storage.Resolve(world, sound, nullptr, 0));
    const harken::PackVoiceBinding catalog[] = {
        HostBinding(harkentest::ObjectId(1), harkentest::ObjectId(21)), HostBinding(sound.stableId, harkentest::ObjectId(0))};
    const harken::PackVoiceBinding* resolved[harken::kMaxVoiceBindingInstanceSlots]{};
    const std::uint32_t count = harken::CollectCutVoiceBindings(
        storage.View().path, storage.View().pathCount, catalog, 2, resolved,
        harken::kMaxVoiceBindingInstanceSlots);
    EXPECT_EQ(0u, count);
}
