#include "UnitTest/StudioObjectIdTestSupport.h"
#include "harken_project_schema/FrontEnd.h"
#include "harken_playable_properties.g.h"
#include "harken_playable_property_studio.g.h"

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <iterator>
#include <utility>

namespace harkenprojectschema {
namespace {

using harkenplayableproperties::ChassisPresenceV1;
using harkenplayableproperties::ContentPropertyKindV1;
using harkenplayableproperties::PlayablePropertyDefaultV1;
using harkenplayableproperties::PlayablePropertyIdV1;

AuthoringGuid G(std::uint64_t low) { return harkentest::StudioObjectId(low); }

InputNode Node(ContentKind kind, std::uint64_t guid) {
    InputNode node;
    node.kind = kind;
    node.guid = G(guid);
    return node;
}

ProjectInput SingleSound(InputNode sound) {
    ProjectInput input;
    input.nodes.push_back(std::move(sound));
    input.roots = {G(1)};
    return input;
}

TEST(PlayablePropertyRegistryV1, RetiredSlotsLeaveOtherIdsUnchanged) {
    constexpr std::array<std::uint8_t, 15> expectedIds{
        1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 14, 16, 17};
    static_assert(static_cast<std::uint8_t>(ContentPropertyKindV1::Layer) == 0);
    static_assert(static_cast<std::uint8_t>(ContentPropertyKindV1::Sound) == 1);
    static_assert(static_cast<std::uint8_t>(ContentPropertyKindV1::Container) == 2);
    static_assert(static_cast<std::uint8_t>(ContentPropertyKindV1::Track) == 3);
    static_assert(static_cast<std::uint8_t>(ContentPropertyKindV1::Piece) == 4);
    static_assert(static_cast<std::uint8_t>(PlayablePropertyDefaultV1::PositiveZero) == 0);
    static_assert(static_cast<std::uint8_t>(PlayablePropertyDefaultV1::One) == 1);
    static_assert(static_cast<std::uint8_t>(PlayablePropertyDefaultV1::Zero) == 2);
    static_assert(static_cast<std::uint8_t>(PlayablePropertyDefaultV1::Empty) == 3);
    static_assert(static_cast<std::uint8_t>(PlayablePropertyDefaultV1::IntegerOne) == 4);
    ASSERT_EQ(std::size(harkenplayableproperties::kPlayablePropertyRegistryV1),
              expectedIds.size());
    ASSERT_EQ(std::size(harkenplayableproperties::studio::kJsonFieldsV1),
              expectedIds.size());
    EXPECT_EQ(harkenplayableproperties::kPlayablePropertyKnownMaskV1,
              UINT64_C(0x1afff));
    for (std::size_t index = 0; index < expectedIds.size(); ++index) {
        const auto id = expectedIds[index];
        EXPECT_EQ(static_cast<std::uint8_t>(
                      harkenplayableproperties::kPlayablePropertyRegistryV1[index].id),
                  id);
        EXPECT_EQ(harkenplayableproperties::studio::kJsonFieldsV1[index].id,
                  id);
        EXPECT_EQ(
            harkenplayableproperties::kPlayablePropertyRegistryV1[index].presentBit,
            UINT64_C(1) << (id - 1));
        EXPECT_EQ(
            harkenplayableproperties::studio::kJsonFieldsV1[index].presentBit,
            UINT64_C(1) << (id - 1));
    }
    EXPECT_EQ(harkenplayableproperties::kPlayablePropertyRegistryV1[0].canonicalDefault,
              PlayablePropertyDefaultV1::PositiveZero);
    EXPECT_EQ(harkenplayableproperties::kPlayablePropertyRegistryV1[3].canonicalDefault,
              PlayablePropertyDefaultV1::IntegerOne);
    EXPECT_EQ(harkenplayableproperties::kPlayablePropertyRegistryV1[11].canonicalDefault,
              PlayablePropertyDefaultV1::One);
    EXPECT_EQ(harkenplayableproperties::kPlayablePropertyRegistryV1[13].canonicalDefault,
              PlayablePropertyDefaultV1::Empty);
    EXPECT_EQ(harkenplayableproperties::kPlayablePropertyRegistryV1[0].combineRole,
              harkenplayableproperties::PlayablePropertyCombineRoleV1::DbSum);
    EXPECT_EQ(
        harkenplayableproperties::kPlayablePropertyRegistryV1[6].referenceRole,
        harkenplayableproperties::PlayablePropertyReferenceRoleV1::MainOutputBus);
    EXPECT_EQ(harkenplayableproperties::kPlayablePropertyRegistryV1[13].phaseRole,
              harkenplayableproperties::PlayablePropertyPhaseRoleV1::SameCutInstall);

    for (std::uint8_t id : {13, 15}) {
        ChassisPresenceV1 presence;
        presence.Set(static_cast<PlayablePropertyIdV1>(id));
        EXPECT_EQ(presence.bits, 0u);
        EXPECT_FALSE(presence.Has(static_cast<PlayablePropertyIdV1>(id)));
        presence.bits = UINT64_C(1) << (id - 1);
        EXPECT_FALSE(presence.IsValid());
    }
}

TEST(PlayablePropertyRegistryV1, DefinitionExplicitDefaultDiffersFromAbsent) {
    InputNode absent = Node(ContentKind::Sound, 1);
    FrontEndResult absentResult = CompileFrontEnd(SingleSound(absent));
    ASSERT_TRUE(absentResult.ok());
    const Chassis& absentChassis = absentResult.project->objects[0].chassis();
    EXPECT_FALSE(absentChassis.presence.Has(PlayablePropertyIdV1::Volume));

    InputNode explicitDefault = Node(ContentKind::Sound, 1);
    explicitDefault.volume = +0.0f;
    explicitDefault.presence.Set(PlayablePropertyIdV1::Volume);
    FrontEndResult explicitResult =
        CompileFrontEnd(SingleSound(explicitDefault));
    ASSERT_TRUE(explicitResult.ok());
    EXPECT_TRUE(explicitResult.project->objects[0].chassis().presence.Has(
        PlayablePropertyIdV1::Volume));
}

TEST(PlayablePropertyRegistryV1, PresentEmptySignalGraphDiffersFromAbsentEmpty) {
    FrontEndResult absent = CompileFrontEnd(
        SingleSound(Node(ContentKind::Sound, 1)));
    ASSERT_TRUE(absent.ok());
    EXPECT_FALSE(absent.project->objects[0].chassis().presence.Has(
        PlayablePropertyIdV1::SignalGraph));

    InputNode present = Node(ContentKind::Sound, 1);
    present.presence.Set(PlayablePropertyIdV1::SignalGraph);
    FrontEndResult declared = CompileFrontEnd(SingleSound(present));
    ASSERT_TRUE(declared.ok());
    EXPECT_TRUE(declared.project->objects[0].chassis().presence.Has(
        PlayablePropertyIdV1::SignalGraph));
    EXPECT_TRUE(declared.project->objects[0].chassis().signalGraph.nodes.empty());
}

TEST(PlayablePropertyRegistryV1, ClipOccurrenceExplicitDefaultSurvivesBuild) {
    InputNode track = Node(ContentKind::Track, 1);
    track.ownedChildren = {G(2)};
    InputClip clip;
    clip.guid = G(3);
    clip.targetGuid = G(2);
    clip.stableOccurrenceOrdinal = 23;
    clip.presence.Set(PlayablePropertyIdV1::LoopCount);
    clip.loopCount = 1;
    track.clips.push_back(clip);
    InputNode sound = Node(ContentKind::Sound, 2);
    ProjectInput input;
    input.nodes = {track, sound};
    input.roots = {G(1)};
    FrontEndResult result = CompileFrontEnd(input);
    ASSERT_TRUE(result.ok());
    const ContentObject* builtTrack = nullptr;
    for (const ContentObject& object : result.project->objects) {
        if (object.kind == ContentKind::Track) builtTrack = &object;
    }
    ASSERT_NE(builtTrack, nullptr);
    ASSERT_EQ(builtTrack->track.clips.size(), 1u);
    EXPECT_TRUE(builtTrack->track.clips[0].presence.Has(
        PlayablePropertyIdV1::LoopCount));
    EXPECT_EQ(builtTrack->track.clips[0].loopCount, 1u);
    EXPECT_EQ(builtTrack->track.clips[0].stableOccurrenceOrdinal, 23u);
}

TEST(PlayablePropertyRegistryV1, TrackClipOccurrenceOrdinalsAreUniquePerTrack)
{
    InputNode track = Node(ContentKind::Track, 1);
    track.ownedChildren = {G(2)};
    InputClip first;
    first.guid = G(3);
    first.targetGuid = G(2);
    first.stableOccurrenceOrdinal = 7;
    InputClip second = first;
    second.guid = G(4);
    track.clips = {first, second};
    ProjectInput input;
    input.nodes = {track, Node(ContentKind::Sound, 2)};
    input.roots = {G(1)};

    FrontEndResult result = CompileFrontEnd(input);
    ASSERT_FALSE(result.ok());
    ASSERT_FALSE(result.diagnostics.empty());
    EXPECT_EQ(result.diagnostics[0].code,
              DiagnosticCode::DuplicateOccurrenceOrdinal);
    EXPECT_FALSE(result.project.has_value());
}

TEST(PlayablePropertyRegistryV1, TrackClipVectorReorderPreservesAuthoredOrdinals)
{
    InputNode track = Node(ContentKind::Track, 1);
    track.ownedChildren = {G(2)};
    InputClip first;
    first.guid = G(3);
    first.targetGuid = G(2);
    first.stableOccurrenceOrdinal = 19;
    InputClip second;
    second.guid = G(4);
    second.targetGuid = G(2);
    second.stableOccurrenceOrdinal = 5;
    track.clips = {second, first};
    ProjectInput input;
    input.nodes = {track, Node(ContentKind::Sound, 2)};
    input.roots = {G(1)};

    FrontEndResult result = CompileFrontEnd(input);
    ASSERT_TRUE(result.ok());
    const TrackDef* built = nullptr;
    for (const ContentObject& object : result.project->objects)
        if (object.kind == ContentKind::Track) built = &object.track;
    ASSERT_NE(built, nullptr);
    ASSERT_EQ(built->clips.size(), 2u);
    EXPECT_EQ(built->clips[0].guid, G(4));
    EXPECT_EQ(built->clips[0].stableOccurrenceOrdinal, 5u);
    EXPECT_EQ(built->clips[1].guid, G(3));
    EXPECT_EQ(built->clips[1].stableOccurrenceOrdinal, 19u);
}

TEST(PlayablePropertyRegistryV1, ContainerChildOccurrenceSurvivesBuild) {
    InputNode container = Node(ContentKind::Container, 1);
    InputContainerChildOccurrence child;
    child.childGuid = G(2);
    child.stableOccurrenceOrdinal = 17;
    child.presence.Set(PlayablePropertyIdV1::Volume);
    child.volume = +0.0f;
    container.ownedChildren = {G(2)};
    container.containerChildOccurrences.push_back(child);
    InputNode sound = Node(ContentKind::Sound, 2);
    ProjectInput input;
    input.nodes = {container, sound};
    input.roots = {G(1)};
    FrontEndResult result = CompileFrontEnd(input);
    ASSERT_TRUE(result.ok());
    const ContainerDef* built = nullptr;
    for (const ContentObject& object : result.project->objects) {
        if (object.kind == ContentKind::Container) built = &object.container;
    }
    ASSERT_NE(built, nullptr);
    ASSERT_EQ(built->children.size(), 1u);
    EXPECT_EQ(built->children[0].stableOccurrenceOrdinal, 17u);
    EXPECT_EQ(built->children[0].childPlayableRef.target, harkentest::StudioObjectId(2u));
    EXPECT_TRUE(built->children[0].presence.Has(PlayablePropertyIdV1::Volume));
    EXPECT_EQ(built->children[0].volume, +0.0f);
}

TEST(PlayablePropertyRegistryV1,
     ContainerGuidCompatibilityListAssignsStableAuthoredOrdinals) {
    InputNode container = Node(ContentKind::Container, 1);
    container.ownedChildren = {G(2), G(3)};
    container.containerChildOccurrences = {G(2), G(3)};
    InputNode firstSound = Node(ContentKind::Sound, 2);
    InputNode secondSound = Node(ContentKind::Sound, 3);
    ProjectInput input;
    input.nodes = {container, firstSound, secondSound};
    input.roots = {G(1)};

    FrontEndResult result = CompileFrontEnd(input);
    ASSERT_TRUE(result.ok());
    const ContainerDef* built = nullptr;
    for (const ContentObject& object : result.project->objects) {
        if (object.kind == ContentKind::Container) built = &object.container;
    }
    ASSERT_NE(built, nullptr);
    ASSERT_EQ(built->children.size(), 2u);
    EXPECT_EQ(built->children[0].stableOccurrenceOrdinal, 0u);
    EXPECT_EQ(built->children[0].childPlayableRef.target, harkentest::StudioObjectId(2u));
    EXPECT_EQ(built->children[1].stableOccurrenceOrdinal, 1u);
    EXPECT_EQ(built->children[1].childPlayableRef.target, harkentest::StudioObjectId(3u));
}

TEST(PlayablePropertyRegistryV1, UnknownMaskAndAbsentNonDefaultRefuseAllOutput) {
    for (std::uint8_t bit : {12, 14, 63}) {
        InputNode unknown = Node(ContentKind::Sound, 1);
        unknown.presence.bits = UINT64_C(1) << bit;
        FrontEndResult unknownResult = CompileFrontEnd(SingleSound(unknown));
        ASSERT_FALSE(unknownResult.ok());
        EXPECT_EQ(unknownResult.diagnostics[0].code,
                  DiagnosticCode::UnknownPlayablePropertyBits);
        EXPECT_FALSE(unknownResult.project.has_value());
    }

    InputNode nonCanonical = Node(ContentKind::Sound, 1);
    nonCanonical.volume = 1.0f;
    FrontEndResult nonCanonicalResult =
        CompileFrontEnd(SingleSound(nonCanonical));
    ASSERT_FALSE(nonCanonicalResult.ok());
    EXPECT_EQ(nonCanonicalResult.diagnostics[0].code,
              DiagnosticCode::NonCanonicalAbsentProperty);
    EXPECT_FALSE(nonCanonicalResult.project.has_value());
}

}  // namespace
}  // namespace harkenprojectschema
