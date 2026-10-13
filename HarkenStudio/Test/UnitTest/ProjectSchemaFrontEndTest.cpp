#include "UnitTest/StudioObjectIdTestSupport.h"
#include "harken_project_schema/FrontEnd.h"
#include "harken_project_schema/Model.h"
#include "harken_project_schema/ProjectInput.h"

#include <gtest/gtest.h>

#include <cstdint>

namespace harkenprojectschema {
namespace {

AuthoringGuid G(std::uint64_t n) { return harkentest::StudioObjectId(n); }

TEST(FrontEnd, PreservesFullObjectUuid) {
    ProjectInput input;
    InputNode first;
    first.guid = {0x37b84b8013bf4630ull, 0x8d645095b15a38c9ull};
    InputNode second = first;
    second.guid.high ^= 1;
    input.nodes = {first, second};
    input.roots = {first.guid, second.guid};

    const auto result = CompileFrontEnd(input);
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.project.has_value());
    ASSERT_EQ(result.project->objects.size(), 2u);
    EXPECT_EQ(result.project->objects[0].chassis().id, first.guid);
    EXPECT_EQ(result.project->objects[1].chassis().id, second.guid);
}

TEST(FrontEnd, NullObjectUuidProducesDiagnostic) {
    ProjectInput input;
    input.nodes.emplace_back();
    const auto result = CompileFrontEnd(input);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().code, DiagnosticCode::NullGuid);
    EXPECT_FALSE(result.project.has_value());
}

TEST(FrontEnd, DuplicateObjectUuidIsRejectedWithoutReassignment) {
    ProjectInput input;
    InputNode node;
    node.guid = G(7);
    input.nodes = {node, node, node};
    input.roots = {node.guid};

    const auto result = CompileFrontEnd(input);
    ASSERT_EQ(result.diagnostics.size(), 2u);
    for (const auto& diagnostic : result.diagnostics) {
        EXPECT_EQ(diagnostic.code, DiagnosticCode::DuplicateGuid);
        EXPECT_EQ(diagnostic.primary, node.guid);
    }
    EXPECT_FALSE(result.project.has_value());
}

TEST(FrontEnd, SharedIdentityIndexMustAgreeWithObjectKind) {
    ProjectInput input;
    InputNode node;
    node.guid = G(7);
    input.nodes = {node};
    input.roots = {node.guid};
    input.identityIndex = BuildRuntimeIdentityIndexV1({
        {1, {node.guid, RuntimeIdentityKindV1::Container, node.guid,
             {node.guid, RuntimeIdentityKindV1::Container, 0, node.guid}}},
    }).index;
    ASSERT_NE(input.identityIndex, nullptr);

    const auto result = CompileFrontEnd(input);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().code, DiagnosticCode::DuplicateStableId);
    EXPECT_EQ(result.diagnostics.front().primary, node.guid);
    EXPECT_FALSE(result.project.has_value());
}

TEST(FrontEnd, NestedUuidCannotCollideWithAnObject) {
    ProjectInput input;
    InputNode sound;
    sound.guid = G(1);
    InputNode track;
    track.kind = ContentKind::Track;
    track.guid = G(2);
    InputClip clip;
    clip.guid = sound.guid;
    clip.targetGuid = sound.guid;
    track.clips = {clip};
    input.nodes = {sound, track};
    input.roots = {sound.guid, track.guid};

    const auto result = CompileFrontEnd(input);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().code, DiagnosticCode::DuplicateGuid);
    EXPECT_EQ(result.diagnostics.front().primary, sound.guid);
    EXPECT_FALSE(result.project.has_value());
}

TEST(FrontEnd, ClipDurationRejectsNegativeAndOverflowingEnds) {
    const auto compileDuration = [](std::int64_t start,
                                    std::int64_t duration) {
        ProjectInput input;
        InputNode track;
        track.kind = ContentKind::Track;
        track.guid = G(1);
        track.ownedChildren = {G(2)};
        InputClip clip;
        clip.guid = G(3);
        clip.targetGuid = G(2);
        clip.time = start;
        clip.duration = duration;
        track.clips = {clip};
        InputNode sound;
        sound.kind = ContentKind::Sound;
        sound.guid = G(2);
        input.nodes = {track, sound};
        input.roots = {track.guid};
        return CompileFrontEnd(input);
    };

    const FrontEndResult negative = compileDuration(0, -1);
    ASSERT_EQ(1u, negative.diagnostics.size());
    EXPECT_EQ(DiagnosticCode::NegativeClipDuration,
              negative.diagnostics.front().code);
    EXPECT_FALSE(negative.project.has_value());

    const FrontEndResult overflow =
        compileDuration(INT64_MAX - 1, 2);
    ASSERT_EQ(1u, overflow.diagnostics.size());
    EXPECT_EQ(DiagnosticCode::ClipEndOverflow,
              overflow.diagnostics.front().code);
    EXPECT_FALSE(overflow.project.has_value());

    const FrontEndResult zero = compileDuration(0, 0);
    ASSERT_TRUE(zero.ok());
    ASSERT_TRUE(zero.project.has_value());
    EXPECT_EQ(0, zero.project->objects.front().track.clips.front().duration);
}

// ---- ownership kind rule -------------------------------------------------

TEST(OwnershipRule, MatchesTheFrozenContract) {
    // Layer files any of the five.
    EXPECT_TRUE(MayOwn(ContentKind::Layer, ContentKind::Layer));
    EXPECT_TRUE(MayOwn(ContentKind::Layer, ContentKind::Sound));
    EXPECT_TRUE(MayOwn(ContentKind::Layer, ContentKind::Container));
    EXPECT_TRUE(MayOwn(ContentKind::Layer, ContentKind::Track));
    EXPECT_TRUE(MayOwn(ContentKind::Layer, ContentKind::Piece));

    // Container owns everything but a Layer.
    EXPECT_FALSE(MayOwn(ContentKind::Container, ContentKind::Layer));
    EXPECT_TRUE(MayOwn(ContentKind::Container, ContentKind::Sound));
    EXPECT_TRUE(MayOwn(ContentKind::Container, ContentKind::Container));
    EXPECT_TRUE(MayOwn(ContentKind::Container, ContentKind::Track));
    EXPECT_TRUE(MayOwn(ContentKind::Container, ContentKind::Piece));

    // Track owns Sound, Container, Piece — never a Track, never a Layer.
    EXPECT_FALSE(MayOwn(ContentKind::Track, ContentKind::Layer));
    EXPECT_TRUE(MayOwn(ContentKind::Track, ContentKind::Sound));
    EXPECT_TRUE(MayOwn(ContentKind::Track, ContentKind::Container));
    EXPECT_FALSE(MayOwn(ContentKind::Track, ContentKind::Track));
    EXPECT_TRUE(MayOwn(ContentKind::Track, ContentKind::Piece));

    // Piece owns Tracks only.
    EXPECT_FALSE(MayOwn(ContentKind::Piece, ContentKind::Layer));
    EXPECT_FALSE(MayOwn(ContentKind::Piece, ContentKind::Sound));
    EXPECT_FALSE(MayOwn(ContentKind::Piece, ContentKind::Container));
    EXPECT_TRUE(MayOwn(ContentKind::Piece, ContentKind::Track));
    EXPECT_FALSE(MayOwn(ContentKind::Piece, ContentKind::Piece));

    // Sound owns nothing.
    EXPECT_FALSE(MayOwn(ContentKind::Sound, ContentKind::Sound));
}

TEST(PlayableRule, OnlySoundContainerPiece) {
    EXPECT_TRUE(IsPlayableRefKind(ContentKind::Sound));
    EXPECT_TRUE(IsPlayableRefKind(ContentKind::Container));
    EXPECT_TRUE(IsPlayableRefKind(ContentKind::Piece));
    EXPECT_FALSE(IsPlayableRefKind(ContentKind::Track));
    EXPECT_FALSE(IsPlayableRefKind(ContentKind::Layer));
}

// ---- stage reporting -----------------------------------------------------

TEST(FrontEnd, InvalidContentKindRefusedAndReportsOrdinal) {
    ProjectInput in;
    InputNode node;
    node.kind = static_cast<ContentKind>(5);
    node.guid = G(1);
    in.nodes = {node};
    in.roots = {G(1)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().code,
              DiagnosticCode::InvalidContentKind);
    EXPECT_EQ(result.diagnostics.front().detail, 5u);
    EXPECT_FALSE(result.project.has_value());
}

TEST(FrontEnd, InvalidPieceTimeKindRefusedAndReportsOrdinal) {
    ProjectInput in;
    InputNode piece;
    piece.kind = ContentKind::Piece;
    piece.guid = G(1);
    InputPieceTimeSpec spec;
    spec.kind = static_cast<PieceTimeKind>(2);
    piece.timeSpec = spec;
    InputStartPoint start;
    start.kind = StartPointKind::Entry;
    piece.startPoint = start;
    in.nodes = {piece};
    in.roots = {G(1)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().code,
              DiagnosticCode::InvalidPieceTimeKind);
    EXPECT_EQ(result.diagnostics.front().detail, 2u);
    EXPECT_FALSE(result.project.has_value());
}

TEST(FrontEnd, InvalidStartPointKindRefusedAndReportsOrdinal) {
    ProjectInput in;
    InputNode piece;
    piece.kind = ContentKind::Piece;
    piece.guid = G(1);
    InputPieceTimeSpec spec;
    spec.kind = PieceTimeKind::Absolute;
    piece.timeSpec = spec;
    InputStartPoint start;
    start.kind = static_cast<StartPointKind>(2);
    piece.startPoint = start;
    in.nodes = {piece};
    in.roots = {G(1)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().code,
              DiagnosticCode::InvalidStartPointKind);
    EXPECT_EQ(result.diagnostics.front().detail, 2u);
    EXPECT_FALSE(result.project.has_value());
}

TEST(FrontEnd, InvalidPieceTimeKindOnNonPieceReportsBothFailures) {
    ProjectInput in;
    InputNode node;
    node.kind = ContentKind::Sound;
    node.guid = G(1);
    InputPieceTimeSpec spec;
    spec.kind = static_cast<PieceTimeKind>(2);
    node.timeSpec = spec;
    in.nodes = {node};
    in.roots = {G(1)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_EQ(result.diagnostics.size(), 2u);
    EXPECT_EQ(result.diagnostics[0].code, DiagnosticCode::TimeSpecOnNonPiece);
    EXPECT_EQ(result.diagnostics[1].code,
              DiagnosticCode::InvalidPieceTimeKind);
    EXPECT_EQ(result.diagnostics[1].detail, 2u);
    EXPECT_FALSE(result.project.has_value());
}

TEST(FrontEnd, InvalidStartPointKindOnNonPieceReportsBothFailures) {
    ProjectInput in;
    InputNode node;
    node.kind = ContentKind::Sound;
    node.guid = G(1);
    InputStartPoint start;
    start.kind = static_cast<StartPointKind>(2);
    node.startPoint = start;
    in.nodes = {node};
    in.roots = {G(1)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_EQ(result.diagnostics.size(), 2u);
    EXPECT_EQ(result.diagnostics[0].code,
              DiagnosticCode::StartPointOnNonPiece);
    EXPECT_EQ(result.diagnostics[1].code,
              DiagnosticCode::InvalidStartPointKind);
    EXPECT_EQ(result.diagnostics[1].detail, 2u);
    EXPECT_FALSE(result.project.has_value());
}

TEST(FrontEnd, EntryMarkerIdOnNonPieceReportsBothFailures) {
    ProjectInput in;
    InputNode node;
    node.kind = ContentKind::Sound;
    node.guid = G(1);
    InputStartPoint start;
    start.kind = StartPointKind::Entry;
    start.markerId = 7;
    node.startPoint = start;
    in.nodes = {node};
    in.roots = {G(1)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_EQ(result.diagnostics.size(), 2u);
    EXPECT_EQ(result.diagnostics[0].code,
              DiagnosticCode::StartPointOnNonPiece);
    EXPECT_EQ(result.diagnostics[1].code,
              DiagnosticCode::EntryStartPointMarkerPresent);
    EXPECT_EQ(result.diagnostics[1].detail, 7u);
    EXPECT_FALSE(result.project.has_value());
}

TEST(FrontEnd, InvalidMarkerKindRefusedAndReportsOrdinal) {
    ProjectInput in;
    InputNode piece;
    piece.kind = ContentKind::Piece;
    piece.guid = G(1);
    InputPieceTimeSpec spec;
    spec.kind = PieceTimeKind::Absolute;
    piece.timeSpec = spec;
    InputStartPoint start;
    start.kind = StartPointKind::Entry;
    piece.startPoint = start;
    InputMarker marker;
    marker.guid = G(2);
    marker.markerId = 1;
    marker.kind = static_cast<MarkerKind>(2);
    piece.markers = {marker};
    in.nodes = {piece};
    in.roots = {G(1)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().code,
              DiagnosticCode::InvalidMarkerKind);
    EXPECT_EQ(result.diagnostics.front().primary, G(1));
    EXPECT_EQ(result.diagnostics.front().secondary, G(2));
    EXPECT_EQ(result.diagnostics.front().detail, 2u);
    EXPECT_FALSE(result.project.has_value());
}

TEST(FrontEnd, DuplicateMarkerIdRefusedAndReportsId) {
    ProjectInput in;
    InputNode piece;
    piece.kind = ContentKind::Piece;
    piece.guid = G(1);
    InputPieceTimeSpec spec;
    spec.kind = PieceTimeKind::Absolute;
    piece.timeSpec = spec;
    InputStartPoint start;
    start.kind = StartPointKind::Entry;
    piece.startPoint = start;
    InputMarker first;
    first.guid = G(2);
    first.markerId = 7;
    InputMarker second;
    second.guid = G(3);
    second.markerId = 7;
    piece.markers = {first, second};
    in.nodes = {piece};
    in.roots = {G(1)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().code, DiagnosticCode::DuplicateMarkerId);
    EXPECT_EQ(result.diagnostics.front().primary, G(1));
    EXPECT_EQ(result.diagnostics.front().secondary, G(3));
    EXPECT_EQ(result.diagnostics.front().detail, 7u);
    EXPECT_FALSE(result.project.has_value());
}

TEST(FrontEnd, EntryStartPointWithMarkerIdRefused) {
    ProjectInput in;
    InputNode piece;
    piece.kind = ContentKind::Piece;
    piece.guid = G(1);
    InputPieceTimeSpec spec;
    spec.kind = PieceTimeKind::Absolute;
    piece.timeSpec = spec;
    InputStartPoint start;
    start.kind = StartPointKind::Entry;
    start.markerId = 7;
    piece.startPoint = start;
    in.nodes = {piece};
    in.roots = {G(1)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().code,
              DiagnosticCode::EntryStartPointMarkerPresent);
    EXPECT_EQ(result.diagnostics.front().detail, 7u);
    EXPECT_FALSE(result.project.has_value());
}

TEST(FrontEnd, DuplicateRootRefused) {
    ProjectInput in;
    InputNode node;
    node.kind = ContentKind::Sound;
    node.guid = G(1);
    in.nodes = {node};
    in.roots = {G(1), G(1)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().code, DiagnosticCode::DuplicateRoot);
    EXPECT_EQ(result.diagnostics.front().primary, G(1));
    EXPECT_FALSE(result.project.has_value());
}

TEST(FrontEnd, DuplicateLayerChildEdgeRefusedWithoutMultipleOwners) {
    ProjectInput in;
    InputNode parent;
    parent.kind = ContentKind::Layer;
    parent.guid = G(1);
    parent.ownedChildren = {G(2), G(2)};
    InputNode child;
    child.kind = ContentKind::Sound;
    child.guid = G(2);
    in.nodes = {parent, child};
    in.roots = {G(1)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().code,
              DiagnosticCode::DuplicateOwnershipEdge);
    EXPECT_EQ(result.diagnostics.front().primary, G(1));
    EXPECT_EQ(result.diagnostics.front().secondary, G(2));
    EXPECT_FALSE(result.project.has_value());
}

TEST(FrontEnd, DuplicateTrackChildEdgeRefusedWithoutMultipleOwners) {
    ProjectInput in;
    InputNode parent;
    parent.kind = ContentKind::Track;
    parent.guid = G(1);
    parent.ownedChildren = {G(2), G(2)};
    InputNode child;
    child.kind = ContentKind::Sound;
    child.guid = G(2);
    in.nodes = {parent, child};
    in.roots = {G(1)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().code,
              DiagnosticCode::DuplicateOwnershipEdge);
    EXPECT_EQ(result.diagnostics.front().primary, G(1));
    EXPECT_EQ(result.diagnostics.front().secondary, G(2));
    EXPECT_FALSE(result.project.has_value());
}

TEST(FrontEnd, ContainerAllowsRepeatedChildWithDistinctOccurrenceOrdinals) {
    ProjectInput in;
    InputNode parent;
    parent.kind = ContentKind::Container;
    parent.guid = G(1);
    InputContainerChildOccurrence first(G(2));
    first.stableOccurrenceOrdinal = 7;
    first.presence.Set(
        harkenplayableproperties::PlayablePropertyIdV1::Volume);
    first.volume = -6.0f;
    InputContainerChildOccurrence second(G(2));
    second.stableOccurrenceOrdinal = 11;
    second.presence.Set(
        harkenplayableproperties::PlayablePropertyIdV1::Pitch);
    second.pitch = 3.0f;
    parent.ownedChildren = {G(2)};
    parent.containerChildOccurrences = {first, second};
    InputNode child;
    child.kind = ContentKind::Sound;
    child.guid = G(2);
    in.nodes = {parent, child};
    in.roots = {G(1)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.project.has_value());
    ASSERT_EQ(result.project->objects.size(), 2u);
    const ContentObject& container = result.project->objects[0];
    ASSERT_EQ(container.kind, ContentKind::Container);
    ASSERT_EQ(container.container.ownedChildren.size(), 1u);
    ASSERT_EQ(container.container.children.size(), 2u);
    EXPECT_EQ(container.container.children[0].childPlayableRef.target,
              container.container.children[1].childPlayableRef.target);
    EXPECT_EQ(container.container.children[0].stableOccurrenceOrdinal, 7u);
    EXPECT_EQ(container.container.children[1].stableOccurrenceOrdinal, 11u);
    EXPECT_FLOAT_EQ(container.container.children[0].volume, -6.0f);
    EXPECT_FLOAT_EQ(container.container.children[1].pitch, 3.0f);
}

TEST(FrontEnd, ContainerOccurrenceTargetMustResolve) {
    ProjectInput in;
    InputNode container;
    container.kind = ContentKind::Container;
    container.guid = G(1);
    container.containerChildOccurrences = {G(2)};
    in.nodes = {container};
    in.roots = {G(1)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().code,
              DiagnosticCode::DanglingContentRef);
    EXPECT_EQ(result.diagnostics.front().primary, G(1));
    EXPECT_EQ(result.diagnostics.front().secondary, G(2));
}

TEST(FrontEnd, ContainerOccurrenceTargetMustBePlayable) {
    ProjectInput in;
    InputNode container;
    container.kind = ContentKind::Container;
    container.guid = G(1);
    container.containerChildOccurrences = {G(2)};
    InputNode layer;
    layer.kind = ContentKind::Layer;
    layer.guid = G(2);
    in.nodes = {container, layer};
    in.roots = {G(1), G(2)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().code,
              DiagnosticCode::IllegalContentRefType);
    EXPECT_EQ(result.diagnostics.front().primary, G(1));
    EXPECT_EQ(result.diagnostics.front().secondary, G(2));
}

TEST(FrontEnd, ContainerOccurrenceTargetAllowsOwnedTrack) {
    ProjectInput in;
    InputNode container;
    container.kind = ContentKind::Container;
    container.guid = G(1);
    container.ownedChildren = {G(2)};
    container.containerChildOccurrences = {G(2)};
    InputNode track;
    track.kind = ContentKind::Track;
    track.guid = G(2);
    in.nodes = {container, track};
    in.roots = {G(1)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.project.has_value());
    ASSERT_EQ(result.project->objects[0].container.children.size(), 1u);
    EXPECT_EQ(result.project->objects[0].container.children[0]
                  .childPlayableRef.kind,
              ContentKind::Track);
}

TEST(FrontEnd, ContainerOccurrenceTargetMustBeOwnedChild) {
    ProjectInput in;
    InputNode container;
    container.kind = ContentKind::Container;
    container.guid = G(1);
    container.containerChildOccurrences = {G(2)};
    InputNode sound;
    sound.kind = ContentKind::Sound;
    sound.guid = G(2);
    in.nodes = {container, sound};
    in.roots = {G(1), G(2)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().code, DiagnosticCode::IllegalOwnership);
    EXPECT_EQ(result.diagnostics.front().primary, G(1));
    EXPECT_EQ(result.diagnostics.front().secondary, G(2));
    EXPECT_FALSE(result.project.has_value());
}

TEST(FrontEnd, ClipTargetsMustBeDirectChildrenAcrossHosts) {
    for (const auto hostKind : {ContentKind::Layer, ContentKind::Container, ContentKind::Piece}) {
        ProjectInput input;
        InputNode layer;
        layer.guid = G(1);
        layer.kind = ContentKind::Layer;
        layer.ownedChildren = {G(2), G(7), G(13)};
        InputNode host;
        host.guid = G(2);
        host.kind = hostKind;
        host.ownedChildren = {G(3), G(11)};
        if (hostKind == ContentKind::Piece) {
            host.timeSpec = InputPieceTimeSpec{};
            host.timeSpec->kind = PieceTimeKind::Absolute;
            host.startPoint = InputStartPoint{};
            host.startPoint->kind = StartPointKind::Entry;
        }
        InputNode track;
        track.guid = G(3);
        track.kind = ContentKind::Track;
        track.ownedChildren = {G(4), G(5), G(8)};
        InputNode sound;
        sound.guid = G(4);
        sound.kind = ContentKind::Sound;
        InputNode nested;
        nested.guid = G(5);
        nested.kind = ContentKind::Container;
        nested.ownedChildren = {G(6)};
        InputNode grandchild;
        grandchild.guid = G(6);
        grandchild.kind = ContentKind::Sound;
        InputNode external;
        external.guid = G(7);
        external.kind = ContentKind::Sound;
        InputNode piece;
        piece.guid = G(8);
        piece.kind = ContentKind::Piece;
        piece.ownedChildren = {G(9)};
        piece.timeSpec = InputPieceTimeSpec{};
        piece.timeSpec->kind = PieceTimeKind::Absolute;
        piece.startPoint = InputStartPoint{};
        piece.startPoint->kind = StartPointKind::Entry;
        InputNode innerTrack;
        innerTrack.guid = G(9);
        innerTrack.kind = ContentKind::Track;
        innerTrack.ownedChildren = {G(10)};
        InputNode innerSound;
        innerSound.guid = G(10);
        innerSound.kind = ContentKind::Sound;
        InputNode peerTrack;
        peerTrack.guid = G(11);
        peerTrack.kind = ContentKind::Track;
        peerTrack.ownedChildren = {G(12)};
        InputNode peerSound;
        peerSound.guid = G(12);
        peerSound.kind = ContentKind::Sound;
        InputNode otherLayer;
        otherLayer.guid = G(13);
        otherLayer.kind = ContentKind::Layer;
        otherLayer.ownedChildren = {G(14)};
        InputNode otherSound;
        otherSound.guid = G(14);
        otherSound.kind = ContentKind::Sound;
        InputClip first;
        first.guid = G(100);
        first.targetGuid = sound.guid;
        first.stableOccurrenceOrdinal = 9;
        InputClip second = first;
        second.guid = G(101);
        second.stableOccurrenceOrdinal = 14;
        second.time = 200;
        InputClip containerClip = first;
        containerClip.guid = G(102);
        containerClip.targetGuid = nested.guid;
        containerClip.stableOccurrenceOrdinal = 17;
        InputClip pieceClip = first;
        pieceClip.guid = G(103);
        pieceClip.targetGuid = piece.guid;
        pieceClip.stableOccurrenceOrdinal = 21;
        track.clips = {first, second, containerClip, pieceClip};
        input.nodes = {layer, host, track, sound, nested, grandchild, external, piece,
                       innerTrack, innerSound, peerTrack, peerSound, otherLayer, otherSound};
        input.roots = {layer.guid};
        auto valid = CompileFrontEnd(input);
        ASSERT_TRUE(valid.ok()) << static_cast<int>(hostKind);
        ASSERT_TRUE(valid.project.has_value());
        const auto& clips = valid.project->objects[2].track.clips;
        ASSERT_EQ(clips.size(), 4u);
        EXPECT_EQ(clips[0].stableOccurrenceOrdinal, 9u);
        EXPECT_EQ(clips[1].stableOccurrenceOrdinal, 14u);
        EXPECT_EQ(clips[2].contentRef.kind, ContentKind::Container);
        EXPECT_EQ(clips[3].contentRef.kind, ContentKind::Piece);

        for (const auto target : {G(6), G(7), G(2), G(1), G(3), G(9), G(10),
                                  G(11), G(12), G(13), G(14), G(99)}) {
            SCOPED_TRACE(static_cast<int>(hostKind));
            input.nodes[2].clips.front().targetGuid = target;
            const auto invalid = CompileFrontEnd(input);
            ASSERT_FALSE(invalid.ok());
            ASSERT_FALSE(invalid.project.has_value());
            ASSERT_EQ(invalid.diagnostics.size(), 1u);
            const auto expected = target == G(99) ? DiagnosticCode::DanglingContentRef
                : target == G(1) || target == G(3) || target == G(9)
                    || target == G(11) || target == G(13)
                    || (target == G(2) && hostKind == ContentKind::Layer)
                    ? DiagnosticCode::IllegalContentRefType : DiagnosticCode::IllegalOwnership;
            EXPECT_EQ(invalid.diagnostics.front().code, expected);
            EXPECT_EQ(invalid.diagnostics.front().secondary, target);
            if (expected == DiagnosticCode::IllegalOwnership) {
                EXPECT_EQ(invalid.diagnostics.front().primary, track.guid);
                EXPECT_EQ(invalid.diagnostics.front().detail, first.stableOccurrenceOrdinal);
            }
        }
    }
}

TEST(FrontEnd, ContainerRejectsGrandchildWithoutChangingOwnedChildren) {
    ProjectInput input;
    InputNode owner;
    owner.kind = ContentKind::Container;
    owner.guid = G(1);
    owner.ownedChildren = {G(2)};
    owner.containerChildOccurrences = {G(3)};
    owner.containerChildOccurrences.front().stableOccurrenceOrdinal = 9;
    InputNode child;
    child.kind = ContentKind::Container;
    child.guid = G(2);
    child.ownedChildren = {G(3)};
    InputNode grandchild;
    grandchild.kind = ContentKind::Sound;
    grandchild.guid = G(3);
    input.nodes = {owner, child, grandchild};
    input.roots = {owner.guid};
    const auto invalid = CompileFrontEnd(input);
    ASSERT_FALSE(invalid.ok());
    ASSERT_EQ(invalid.diagnostics.size(), 1u);
    EXPECT_EQ(invalid.diagnostics.front().code, DiagnosticCode::IllegalOwnership);
    EXPECT_EQ(invalid.diagnostics.front().detail, 9u);
    EXPECT_EQ(invalid.diagnostics.front().primary, owner.guid);
    EXPECT_EQ(invalid.diagnostics.front().secondary, grandchild.guid);
    input.nodes.front().containerChildOccurrences.clear();
    EXPECT_TRUE(CompileFrontEnd(input).ok());
    input.nodes.front().containerChildOccurrences = {child.guid};
    EXPECT_TRUE(CompileFrontEnd(input).ok());
}

TEST(FrontEnd, ContainerOccurrencesAreRefusedOnOtherKinds) {
    ProjectInput in;
    InputNode sound;
    sound.kind = ContentKind::Sound;
    sound.guid = G(1);
    sound.containerChildOccurrences = {G(1)};
    in.nodes = {sound};
    in.roots = {G(1)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().code,
              DiagnosticCode::ContainerChildOccurrencesOnNonContainer);
}

TEST(FrontEnd, RepeatedContainerChildStillRequiresDistinctOccurrenceOrdinals) {
    ProjectInput in;
    InputNode parent;
    parent.kind = ContentKind::Container;
    parent.guid = G(1);
    parent.ownedChildren = {G(2)};
    InputContainerChildOccurrence first(G(2));
    first.stableOccurrenceOrdinal = 7;
    InputContainerChildOccurrence second(G(2));
    second.stableOccurrenceOrdinal = 7;
    parent.containerChildOccurrences = {first, second};
    InputNode child;
    child.kind = ContentKind::Sound;
    child.guid = G(2);
    in.nodes = {parent, child};
    in.roots = {G(1)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().code,
              DiagnosticCode::DuplicateOccurrenceOrdinal);
    EXPECT_EQ(result.diagnostics.front().primary, G(1));
    EXPECT_EQ(result.diagnostics.front().secondary, G(2));
    EXPECT_EQ(result.diagnostics.front().detail, 7u);
    EXPECT_FALSE(result.project.has_value());
}

TEST(FrontEnd, ContainerOccurrenceCycleIsIndependentOfOwnershipGraph) {
    ProjectInput in;
    InputNode first;
    first.kind = ContentKind::Container;
    first.guid = G(1);
    first.containerChildOccurrences = {G(2)};
    InputNode second;
    second.kind = ContentKind::Container;
    second.guid = G(2);
    second.containerChildOccurrences = {G(1)};
    in.nodes = {first, second};
    in.roots = {G(1), G(2)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_EQ(result.diagnostics.size(), 3u);
    EXPECT_EQ(result.diagnostics[0].code, DiagnosticCode::IllegalOwnership);
    EXPECT_EQ(result.diagnostics[0].primary, G(1));
    EXPECT_EQ(result.diagnostics[0].secondary, G(2));
    EXPECT_EQ(result.diagnostics[1].code, DiagnosticCode::IllegalOwnership);
    EXPECT_EQ(result.diagnostics[1].primary, G(2));
    EXPECT_EQ(result.diagnostics[1].secondary, G(1));
    EXPECT_EQ(result.diagnostics[2].code,
              DiagnosticCode::ContainerOccurrenceCycle);
    EXPECT_FALSE(result.project.has_value());
}

TEST(FrontEnd, DistinctParentsStillReportMultipleOwners) {
    ProjectInput in;
    InputNode firstParent;
    firstParent.kind = ContentKind::Container;
    firstParent.guid = G(1);
    firstParent.ownedChildren = {G(3)};
    InputNode secondParent;
    secondParent.kind = ContentKind::Container;
    secondParent.guid = G(2);
    secondParent.ownedChildren = {G(3)};
    InputNode child;
    child.kind = ContentKind::Sound;
    child.guid = G(3);
    in.nodes = {firstParent, secondParent, child};
    in.roots = {G(1), G(2)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().code, DiagnosticCode::MultipleOwners);
    EXPECT_EQ(result.diagnostics.front().primary, G(3));
    EXPECT_EQ(result.diagnostics.front().secondary, G(1));
    EXPECT_EQ(result.diagnostics.front().detail, 2u);
    EXPECT_FALSE(result.project.has_value());
}

TEST(FrontEnd, DeepOwnershipChainCompilesWithoutUsingCallStack) {
    constexpr std::size_t kNodeCount = 20'000;
    ProjectInput in;
    in.nodes.reserve(kNodeCount);
    for (std::size_t i = 0; i < kNodeCount; ++i) {
        InputNode node;
        node.kind = ContentKind::Layer;
        node.guid = G(i + 1);
        if (i + 1 < kNodeCount) {
            node.ownedChildren = {G(i + 2)};
        }
        in.nodes.push_back(node);
    }
    in.roots = {G(1)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.project.has_value());
    EXPECT_EQ(result.project->objects.size(), kNodeCount);
}

TEST(FrontEnd, StructuralCycleRingPathRemainsDeterministic) {
    ProjectInput in;
    InputNode first;
    first.kind = ContentKind::Container;
    first.guid = G(1);
    first.ownedChildren = {G(2)};
    InputNode second;
    second.kind = ContentKind::Container;
    second.guid = G(2);
    second.ownedChildren = {G(3)};
    InputNode third;
    third.kind = ContentKind::Container;
    third.guid = G(3);
    third.ownedChildren = {G(1)};
    in.nodes = {first, second, third};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_EQ(result.diagnostics.size(), 1u);
    const Diagnostic& diagnostic = result.diagnostics.front();
    EXPECT_EQ(diagnostic.code, DiagnosticCode::StructuralCycle);
    EXPECT_EQ(diagnostic.primary, G(1));
    const std::vector<AuthoringGuid> expectedPath = {G(1), G(2), G(3), G(1)};
    EXPECT_EQ(diagnostic.path, expectedPath);
    EXPECT_FALSE(result.project.has_value());
}

// A single node breaking two parse rules must report both, ordered by code, so
// a whole stage's failures come out at once and in a fixed order.
TEST(FrontEnd, ReportsAllFailuresOfAStageDeterministically) {
    ProjectInput in;
    InputNode bad;
    bad.kind = ContentKind::Sound;
    bad.guid = G(1);
    InputClip clip;
    clip.guid = G(10);
    clip.targetGuid = G(1);
    bad.clips = {clip};                    // clips on a non-Track
    InputPieceTimeSpec spec;
    spec.kind = PieceTimeKind::Absolute;
    bad.timeSpec = spec;                   // a time spec on a non-Piece
    in.nodes = {bad};
    in.roots = {G(1)};

    const FrontEndResult result = CompileFrontEnd(in);
    ASSERT_EQ(result.diagnostics.size(), 2u);
    EXPECT_EQ(result.diagnostics[0].code, DiagnosticCode::ClipsOnNonTrack);
    EXPECT_EQ(result.diagnostics[1].code, DiagnosticCode::TimeSpecOnNonPiece);
    EXPECT_FALSE(result.project.has_value());
}

// A parse failure stops the pipeline before identity, so a duplicate id in the
// same input is not also reported: the earliest failing stage owns the verdict.
TEST(FrontEnd, EarliestStageStopsThePipeline) {
    ProjectInput in;
    InputNode a;
    a.kind = ContentKind::Sound;
    a.guid = G(1);
    a.guid = harkentest::StudioObjectId(7);
    InputPieceTimeSpec spec;                // parse failure: time spec on a Sound
    spec.kind = PieceTimeKind::Absolute;
    a.timeSpec = spec;
    InputNode b;
    b.kind = ContentKind::Sound;
    b.guid = G(2);
    b.guid = harkentest::StudioObjectId(7);                // would be a stage-2 duplicate id
    in.nodes = {a, b};
    in.roots = {G(1), G(2)};

    const FrontEndResult result = CompileFrontEnd(in);
    // Only the stage-1 failure surfaces; stage 2 never ran.
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics.front().code,
              DiagnosticCode::TimeSpecOnNonPiece);
}

}  // namespace
}  // namespace harkenprojectschema
