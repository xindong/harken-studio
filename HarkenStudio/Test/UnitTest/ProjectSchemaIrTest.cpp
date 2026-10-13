// PieceIR skeleton, required RuntimeLineageTable, and occurrence resource facts.
//
// The IR is derived from a compiled model, its facts, and a resource table, so
// these check what the derivation must get right: that a Track's occurrences
// follow the normalized clip order, that lineage records the two-level path for
// a directly triggered Track and the three-level path for a Track inside a
// Piece, that each Sound occurrence is enriched with its resource's duration and
// seekability, and that a missing resource fact refuses the whole build. The
// fingerprint is checked for stability and for actually depending on the
// lineage — a constant would pass a naive "it's deterministic" test.

#include "UnitTest/StudioObjectIdTestSupport.h"
#include "UnitTest/ProjectSchemaFixtures.h"

#include "harken_project_schema/BuildPieceIR.h"
#include "harken_project_schema/FrontEnd.h"
#include "harken_project_schema/ResourceInput.h"
#include "harken_project_schema/Serialize.h"
#include "harken_project_schema/detail/Fnv1a.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace harkenprojectschema {
namespace {

using fixtures::G;

static_assert(!std::is_default_constructible_v<PieceIR>);
static_assert(std::is_move_constructible_v<PieceIR>);
static_assert(std::is_same_v<decltype(std::declval<const PieceIR&>().tracks()),
                             const std::vector<IrTrack>&>);
static_assert(std::is_same_v<decltype(std::declval<const PieceIR&>().pieces()),
                             const std::vector<IrPiece>&>);
static_assert(std::is_same_v<decltype(std::declval<const PieceIR&>().lineage()),
                             const RuntimeLineageTable&>);

StableId IdOfGuid(const CanonicalProject& project, const AuthoringGuid& guid) {
    for (const ContentObject& obj : project.objects) {
        if (obj.chassis().guid == guid) {
            return obj.chassis().id;
        }
    }
    return kUnmintedStableId;
}

const IrTrack* FindIrTrack(const PieceIR& ir, StableId id) {
    for (const IrTrack& t : ir.tracks()) {
        if (t.trackId == id) {
            return &t;
        }
    }
    return nullptr;
}

// A one-second stereo resident fact for one resource.
ResourceInput OneSecondResident(bool seekable = true) {
    ResourceInput input;
    input.facts.valid = true;
    input.facts.sampleRate = 48000;
    input.facts.channelCount = 2;
    input.facts.frameCount = 48000;
    input.seekable = seekable;
    return input;
}

// Gives every Sound in the project a valid resident fact, so an IR build that
// only cares about structure is not refused for a missing one.
ResourceTable ResidentResources(const CanonicalProject& project) {
    ResourceTable table;
    for (const ContentObject& obj : project.objects) {
        if (obj.kind == ContentKind::Sound) {
            table.emplace(obj.chassis().id, OneSecondResident());
        }
    }
    return table;
}

TEST(ProjectSchemaIr, FoldsTracksAndPieces) {
    const FrontEndResult front = CompileFrontEnd(fixtures::IrLineageMix());
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    const std::optional<PieceIR> built =
        BuildPieceIRForStructureOnly(*front.project, *front.facts, ResidentResources(*front.project));
    ASSERT_TRUE(built.has_value());
    const PieceIR& ir = *built;

    // Two Tracks (T2, T3) are folded; the Sound and Piece are not Tracks. One
    // Piece is present.
    EXPECT_EQ(ir.tracks().size(), 2u);
    EXPECT_EQ(ir.pieces().size(), 1u);

    // The standalone Track's occurrences follow normalized time order (50, 100),
    // not authored order (100, 50).
    const StableId t2 = IdOfGuid(*front.project, G(2));
    const IrTrack* track2 = FindIrTrack(ir, t2);
    ASSERT_NE(track2, nullptr);
    ASSERT_EQ(track2->occurrences.size(), 2u);
    EXPECT_EQ(track2->occurrences[0].time, 50);
    EXPECT_EQ(track2->occurrences[1].time, 100);
    EXPECT_EQ(track2->occurrences[0].key.occurrenceOrdinal, 0u);
    EXPECT_EQ(track2->occurrences[1].key.occurrenceOrdinal, 1u);
}

TEST(ProjectSchemaIr, IdentityReferencesCarryAuthoringGuids) {
    const FrontEndResult front =
        CompileFrontEnd(fixtures::NormalizationOrdering());
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    const std::optional<PieceIR> built = BuildPieceIRForStructureOnly(
        *front.project, *front.facts, ResidentResources(*front.project));
    ASSERT_TRUE(built.has_value());

    const IrTrack* track = FindIrTrack(*built, IdOfGuid(*front.project, G(2)));
    ASSERT_NE(track, nullptr);
    EXPECT_EQ(track->guid, G(2));
    ASSERT_EQ(track->occurrences.size(), 3u);
    EXPECT_EQ(track->occurrences[0].clipGuid, G(201));
    EXPECT_EQ(track->occurrences[0].contentTargetGuid, G(4));

    ASSERT_EQ(built->pieces().size(), 1u);
    const IrPiece& piece = built->pieces()[0];
    EXPECT_EQ(piece.guid, G(6));
    ASSERT_EQ(piece.markers.size(), 3u);
    EXPECT_EQ(piece.markers[0].guid, G(93));
    EXPECT_EQ(piece.markers[1].guid, G(92));
    EXPECT_EQ(piece.markers[2].guid, G(91));
}

TEST(ProjectSchemaIr, LineageRecordsBothDepths) {
    const FrontEndResult front = CompileFrontEnd(fixtures::IrLineageMix());
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    const std::optional<PieceIR> built =
        BuildPieceIRForStructureOnly(*front.project, *front.facts, ResidentResources(*front.project));
    ASSERT_TRUE(built.has_value());
    const PieceIR& ir = *built;

    const StableId sound = IdOfGuid(*front.project, G(4));
    const StableId t2 = IdOfGuid(*front.project, G(2));
    const StableId t3 = IdOfGuid(*front.project, G(7));
    const StableId piece = IdOfGuid(*front.project, G(6));

    // Two occurrences on T2, one on T3 -> three lineage rows.
    ASSERT_EQ(ir.lineage().entries.size(), 3u);

    int twoLevel = 0;
    int threeLevel = 0;
    for (const LineageEntry& e : ir.lineage().entries) {
        // Every row names a real Track and a Playable clip target.
        EXPECT_NE(e.trackId, kUnmintedStableId);
        EXPECT_NE(e.clipId, kUnmintedStableId);
        if (e.trackId == t2) {
            EXPECT_FALSE(e.hasPiece) << "a standalone Track is a two-level path";
            ++twoLevel;
        } else if (e.trackId == t3) {
            EXPECT_TRUE(e.hasPiece) << "a Track inside a Piece is three-level";
            EXPECT_EQ(e.pieceId, piece);
            ++threeLevel;
        }
    }
    EXPECT_EQ(twoLevel, 2);
    EXPECT_EQ(threeLevel, 1);
    // The referenced Sound is not a lineage row: lineage is per Track occurrence,
    // not per referenced object.
    (void)sound;

    // Entries are ordered by (trackId, occurrenceOrdinal).
    for (std::size_t i = 1; i < ir.lineage().entries.size(); ++i) {
        const LineageEntry& prev = ir.lineage().entries[i - 1];
        const LineageEntry& cur = ir.lineage().entries[i];
        const bool ordered =
            prev.trackId < cur.trackId ||
            (prev.trackId == cur.trackId &&
             prev.occurrenceOrdinal < cur.occurrenceOrdinal);
        EXPECT_TRUE(ordered);
    }
}

// Each Sound occurrence carries its resource's authoritative frame length and
// rate, the derived nanosecond floor, and its seekability.
TEST(ProjectSchemaIr, OccurrenceCarriesResourceFacts) {
    const FrontEndResult front = CompileFrontEnd(fixtures::IrLineageMix());
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    const std::optional<PieceIR> built =
        BuildPieceIRForStructureOnly(*front.project, *front.facts, ResidentResources(*front.project));
    ASSERT_TRUE(built.has_value());

    for (const IrTrack& track : built->tracks()) {
        for (const IrOccurrence& occ : track.occurrences) {
            // Every occurrence in this fixture targets the Sound.
            ASSERT_EQ(occ.content.kind, ContentKind::Sound);
            EXPECT_TRUE(occ.hasDuration);
            EXPECT_EQ(occ.durationFrames, 48000u);
            EXPECT_EQ(occ.resourceSampleRate, 48000u);
            EXPECT_EQ(occ.durationNs, 1000000000);  // 48000 frames / 48 kHz = 1 s
            EXPECT_TRUE(occ.seekable);
        }
    }
}

TEST(ProjectSchemaIr, OccurrencePreservesAuthoredDurationSeparatelyFromMedia) {
    ProjectInput input = fixtures::IrLineageMix();
    for (InputNode& node : input.nodes) {
        for (InputClip& clip : node.clips) {
            clip.duration = 123;
        }
    }
    const FrontEndResult front = CompileFrontEnd(input);
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    const std::optional<PieceIR> built = BuildPieceIRForStructureOnly(
        *front.project, *front.facts, ResidentResources(*front.project));
    ASSERT_TRUE(built.has_value());

    for (const IrTrack& track : built->tracks()) {
        for (const IrOccurrence& occurrence : track.occurrences) {
            EXPECT_EQ(123, occurrence.authoredDuration);
            if (occurrence.content.kind == ContentKind::Sound) {
                EXPECT_EQ(48000u, occurrence.durationFrames);
            }
        }
    }
    EXPECT_NE(std::string::npos,
              RenderPieceIR(*built).find("/clipdur123"));
}

// The derived nanosecond duration is a floor of frames * 1e9 / rate, not a
// rounded value, and the authoritative frame pair is carried unrounded.
TEST(ProjectSchemaIr, OccurrenceDurationNsIsFloor) {
    const FrontEndResult front = CompileFrontEnd(fixtures::IrLineageMix());
    ASSERT_TRUE(front.ok());
    const StableId sound = IdOfGuid(*front.project, G(4));

    ResourceTable resources;
    ResourceInput input;
    input.facts.valid = true;
    input.facts.sampleRate = 48000;
    input.facts.channelCount = 2;
    input.facts.frameCount = 48001;  // one frame past a whole second
    input.seekable = false;
    resources.emplace(sound, input);
    resources.emplace(IdOfGuid(*front.project, G(8)), input);

    const std::optional<PieceIR> built =
        BuildPieceIRForStructureOnly(*front.project, *front.facts, resources);
    ASSERT_TRUE(built.has_value());
    bool sawOccurrence = false;
    for (const IrTrack& track : built->tracks()) {
        for (const IrOccurrence& occ : track.occurrences) {
            sawOccurrence = true;
            EXPECT_EQ(occ.durationFrames, 48001u);
            // floor(48001 * 1e9 / 48000) = 1000020833 (the exact value is
            // 1000020833.33...), and the frame pair stays exact.
            EXPECT_EQ(occ.durationNs, 1000020833);
            EXPECT_FALSE(occ.seekable);
        }
    }
    EXPECT_TRUE(sawOccurrence);
}

// A clip that references a Sound with no resource fact refuses the whole build:
// zero partial output, the same rule the front-end follows.
TEST(ProjectSchemaIr, RejectsMissingResourceFacts) {
    const FrontEndResult front = CompileFrontEnd(fixtures::IrLineageMix());
    ASSERT_TRUE(front.ok());
    // An empty resource table leaves the referenced Sound without facts.
    EXPECT_FALSE(BuildPieceIRForStructureOnly(*front.project, *front.facts, ResourceTable{})
                     .has_value());

    // An invalid fact is refused just as a missing one is.
    const StableId sound = IdOfGuid(*front.project, G(4));
    ResourceTable invalid;
    ResourceInput bad;
    bad.facts.valid = false;
    invalid.emplace(sound, bad);
    EXPECT_FALSE(
        BuildPieceIRForStructureOnly(*front.project, *front.facts, invalid).has_value());
}

TEST(ProjectSchemaIr, RejectsZeroChannelResourceFacts) {
    const FrontEndResult front = CompileFrontEnd(fixtures::IrLineageMix());
    ASSERT_TRUE(front.ok());
    ResourceTable resources = ResidentResources(*front.project);
    const StableId sound = IdOfGuid(*front.project, G(4));
    resources.at(sound).facts.channelCount = 0;

    EXPECT_FALSE(
        BuildPieceIRForStructureOnly(*front.project, *front.facts, resources).has_value());
}

TEST(ProjectSchemaIr, RejectsZeroFrameResourceFacts) {
    const FrontEndResult front = CompileFrontEnd(fixtures::IrLineageMix());
    ASSERT_TRUE(front.ok());
    ResourceTable resources = ResidentResources(*front.project);
    const StableId sound = IdOfGuid(*front.project, G(4));
    resources.at(sound).facts.frameCount = 0;

    EXPECT_FALSE(
        BuildPieceIRForStructureOnly(*front.project, *front.facts, resources).has_value());
}

// A Container or Piece clip target carries no duration and needs no resource
// fact: its end is decided by its own program or exit marker.
TEST(ProjectSchemaIr, ContainerTargetHasNoDuration) {
    // The valid fixture's Track 3 references a Sound, a Container and a Piece.
    const FrontEndResult front = CompileFrontEnd(fixtures::Valid());
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    const std::optional<PieceIR> built =
        BuildPieceIRForStructureOnly(*front.project, *front.facts, ResidentResources(*front.project));
    ASSERT_TRUE(built.has_value());

    bool sawContainerOrPiece = false;
    for (const IrTrack& track : built->tracks()) {
        for (const IrOccurrence& occ : track.occurrences) {
            if (occ.content.kind == ContentKind::Container ||
                occ.content.kind == ContentKind::Piece) {
                sawContainerOrPiece = true;
                EXPECT_FALSE(occ.hasDuration);
                EXPECT_EQ(occ.durationFrames, 0u);
                EXPECT_EQ(occ.durationNs, 0);
            } else {
                EXPECT_TRUE(occ.hasDuration);
            }
        }
    }
    EXPECT_TRUE(sawContainerOrPiece);
}

TEST(ProjectSchemaIr, FingerprintIsStableAndLineageDependent) {
    const FrontEndResult mixFront = CompileFrontEnd(fixtures::IrLineageMix());
    ASSERT_TRUE(mixFront.ok());
    const std::optional<PieceIR> first = BuildPieceIRForStructureOnly(
        *mixFront.project, *mixFront.facts, ResidentResources(*mixFront.project));
    const std::optional<PieceIR> second = BuildPieceIRForStructureOnly(
        *mixFront.project, *mixFront.facts, ResidentResources(*mixFront.project));
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    const std::uint64_t a = first->runtimeLineageFingerprint();
    const std::uint64_t b = second->runtimeLineageFingerprint();
    EXPECT_EQ(a, b);
    EXPECT_NE(a, 0u);

    // A structurally different project must hash differently, or the fingerprint
    // is not a function of the lineage.
    const FrontEndResult otherFront = CompileFrontEnd(fixtures::Valid());
    ASSERT_TRUE(otherFront.ok());
    const std::optional<PieceIR> other = BuildPieceIRForStructureOnly(
        *otherFront.project, *otherFront.facts, ResidentResources(*otherFront.project));
    ASSERT_TRUE(other.has_value());
    const std::uint64_t c = other->runtimeLineageFingerprint();
    EXPECT_NE(a, c);
}

TEST(ProjectSchemaIr, LineageFingerprintUsesStandardFnv1a64Vectors) {
    const CanonicalProject emptyProject;
    const NormalizedFacts emptyFacts;
    const std::optional<PieceIR> empty =
        BuildPieceIRForStructureOnly(emptyProject, emptyFacts, ResourceTable{});
    ASSERT_TRUE(empty.has_value());
    EXPECT_EQ(empty->runtimeLineageFingerprint(), 0xcbf29ce484222325ull);

    std::uint64_t singleByte = detail::kFnv1a64OffsetBasis;
    detail::Fnv1a64HashByte(&singleByte, static_cast<std::uint8_t>('a'));
    EXPECT_EQ(singleByte, 0xaf63dc4c8601ec8cull);

    CanonicalProject project;
    ContentObject track;
    track.kind = ContentKind::Track;
    track.track.chassis.id = harkentest::StudioObjectId(1);
    track.track.chassis.guid = fixtures::G(1);
    Clip clip;
    clip.id = harkentest::StudioObjectId(2);
    clip.guid = fixtures::G(2);
    clip.contentRef.target = harkentest::StudioObjectId(3);
    track.track.clips.push_back(clip);
    project.objects.push_back(track);
    ContentObject sound;
    sound.kind = ContentKind::Sound;
    sound.sound.chassis.id = harkentest::StudioObjectId(3);
    sound.sound.chassis.guid = fixtures::G(3);
    project.objects.push_back(sound);

    NormalizedFacts facts;
    TrackNormalization trackFacts;
    trackFacts.track = harkentest::StudioObjectId(1);
    trackFacts.clipsByOrder.push_back(harkentest::StudioObjectId(2));
    facts.tracks.push_back(trackFacts);

    // The clip's Sound target needs a fact; its value does not enter the
    // lineage, so the fingerprint is over the five little-endian u64 fields
    // {0, 0, 1, 2, 0}.
    ResourceTable resources;
    resources.emplace(harkentest::StudioObjectId(3), OneSecondResident());
    const std::optional<PieceIR> oneLineageRow =
        BuildPieceIRForStructureOnly(project, facts, resources);
    ASSERT_TRUE(oneLineageRow.has_value());
    EXPECT_EQ(oneLineageRow->runtimeLineageFingerprint(),
              16819108842722967952ull);
}

TEST(ProjectSchemaIr, RejectsMissingExtraAndDuplicateFactRows) {
    const FrontEndResult front =
        CompileFrontEnd(fixtures::NormalizationOrdering());
    ASSERT_TRUE(front.ok());
    const ResourceTable resources = ResidentResources(*front.project);

    NormalizedFacts missingTrack = *front.facts;
    missingTrack.tracks.pop_back();
    EXPECT_FALSE(
        BuildPieceIRForStructureOnly(*front.project, missingTrack, resources).has_value());

    NormalizedFacts extraTrack = *front.facts;
    TrackNormalization unknownTrack;
    unknownTrack.track = harkentest::StudioObjectId(99'999);
    extraTrack.tracks.push_back(unknownTrack);
    EXPECT_FALSE(
        BuildPieceIRForStructureOnly(*front.project, extraTrack, resources).has_value());

    NormalizedFacts duplicateTrack = *front.facts;
    duplicateTrack.tracks.push_back(duplicateTrack.tracks.front());
    EXPECT_FALSE(
        BuildPieceIRForStructureOnly(*front.project, duplicateTrack, resources).has_value());

    NormalizedFacts missingPiece = *front.facts;
    missingPiece.pieces.clear();
    EXPECT_FALSE(
        BuildPieceIRForStructureOnly(*front.project, missingPiece, resources).has_value());

    NormalizedFacts extraPiece = *front.facts;
    PieceNormalization unknownPiece;
    unknownPiece.piece = harkentest::StudioObjectId(88'888);
    extraPiece.pieces.push_back(unknownPiece);
    EXPECT_FALSE(
        BuildPieceIRForStructureOnly(*front.project, extraPiece, resources).has_value());

    NormalizedFacts duplicatePiece = *front.facts;
    duplicatePiece.pieces.push_back(duplicatePiece.pieces.front());
    EXPECT_FALSE(
        BuildPieceIRForStructureOnly(*front.project, duplicatePiece, resources).has_value());
}

TEST(ProjectSchemaIr, RejectsTrackClipFactSetMismatch) {
    const FrontEndResult front = CompileFrontEnd(fixtures::IrLineageMix());
    ASSERT_TRUE(front.ok());
    const ResourceTable resources = ResidentResources(*front.project);
    std::size_t factIndex = 0;
    while (factIndex < front.facts->tracks.size() &&
           front.facts->tracks[factIndex].clipsByOrder.empty()) {
        ++factIndex;
    }
    ASSERT_LT(factIndex, front.facts->tracks.size());

    NormalizedFacts missing = *front.facts;
    missing.tracks[factIndex].clipsByOrder.pop_back();
    EXPECT_FALSE(BuildPieceIRForStructureOnly(*front.project, missing, resources).has_value());

    NormalizedFacts extra = *front.facts;
    extra.tracks[factIndex].clipsByOrder.push_back(harkentest::StudioObjectId(99'999));
    EXPECT_FALSE(BuildPieceIRForStructureOnly(*front.project, extra, resources).has_value());

    NormalizedFacts duplicate = *front.facts;
    duplicate.tracks[factIndex].clipsByOrder.push_back(
        duplicate.tracks[factIndex].clipsByOrder.front());
    EXPECT_FALSE(BuildPieceIRForStructureOnly(*front.project, duplicate, resources).has_value());
}

TEST(ProjectSchemaIr, RejectsStaleClipOrderWithSameIds) {
    const FrontEndResult front =
        CompileFrontEnd(fixtures::NormalizationOrdering());
    ASSERT_TRUE(front.ok());
    const ResourceTable resources = ResidentResources(*front.project);
    NormalizedFacts stale = *front.facts;

    std::vector<StableId>* order = nullptr;
    for (TrackNormalization& track : stale.tracks) {
        if (track.clipsByOrder.size() > 1) {
            order = &track.clipsByOrder;
            break;
        }
    }
    ASSERT_NE(order, nullptr);
    std::swap((*order)[0], (*order)[1]);

    EXPECT_FALSE(BuildPieceIRForStructureOnly(*front.project, stale, resources).has_value());
}

TEST(ProjectSchemaIr, RejectsPieceMarkerFactSetMismatch) {
    const FrontEndResult front =
        CompileFrontEnd(fixtures::NormalizationOrdering());
    ASSERT_TRUE(front.ok());
    const ResourceTable resources = ResidentResources(*front.project);
    ASSERT_EQ(front.facts->pieces.size(), 1u);
    ASSERT_FALSE(front.facts->pieces[0].markersByMarkerId.empty());

    NormalizedFacts missing = *front.facts;
    missing.pieces[0].markersByMarkerId.pop_back();
    EXPECT_FALSE(BuildPieceIRForStructureOnly(*front.project, missing, resources).has_value());

    NormalizedFacts extra = *front.facts;
    extra.pieces[0].markersByMarkerId.push_back(harkentest::StudioObjectId(99'999));
    EXPECT_FALSE(BuildPieceIRForStructureOnly(*front.project, extra, resources).has_value());

    NormalizedFacts duplicate = *front.facts;
    duplicate.pieces[0].markersByMarkerId.push_back(
        duplicate.pieces[0].markersByMarkerId.front());
    EXPECT_FALSE(BuildPieceIRForStructureOnly(*front.project, duplicate, resources).has_value());
}

TEST(ProjectSchemaIr, RejectsStalePieceStartCoordinate) {
    const FrontEndResult front =
        CompileFrontEnd(fixtures::NormalizationOrdering());
    ASSERT_TRUE(front.ok());
    const ResourceTable resources = ResidentResources(*front.project);
    NormalizedFacts stale = *front.facts;
    ASSERT_EQ(stale.pieces.size(), 1u);
    stale.pieces[0].startCoordinate += 1;

    EXPECT_FALSE(BuildPieceIRForStructureOnly(*front.project, stale, resources).has_value());
}

TEST(ProjectSchemaIr, RejectsStaleMarkerOrderWithSameIds) {
    const FrontEndResult front =
        CompileFrontEnd(fixtures::NormalizationOrdering());
    ASSERT_TRUE(front.ok());
    const ResourceTable resources = ResidentResources(*front.project);
    NormalizedFacts stale = *front.facts;
    ASSERT_EQ(stale.pieces.size(), 1u);
    ASSERT_GT(stale.pieces[0].markersByMarkerId.size(), 1u);
    std::swap(stale.pieces[0].markersByMarkerId[0],
              stale.pieces[0].markersByMarkerId[1]);

    EXPECT_FALSE(BuildPieceIRForStructureOnly(*front.project, stale, resources).has_value());
}

TEST(ProjectSchemaIr, RejectsTrackWithMultiplePieceOwners) {
    const FrontEndResult front = CompileFrontEnd(fixtures::IrLineageMix());
    ASSERT_TRUE(front.ok());
    CanonicalProject project = *front.project;
    NormalizedFacts facts = *front.facts;
    const ResourceTable resources = ResidentResources(project);

    StableId ownedTrack = kUnmintedStableId;
    for (const ContentObject& obj : project.objects) {
        if (obj.kind == ContentKind::Piece && !obj.piece.tracks.empty()) {
            ownedTrack = obj.piece.tracks.front();
            break;
        }
    }
    ASSERT_NE(ownedTrack, kUnmintedStableId);

    ContentObject secondOwner;
    secondOwner.kind = ContentKind::Piece;
    secondOwner.piece.chassis.id = harkentest::StudioObjectId(99'999);
    secondOwner.piece.tracks.push_back(ownedTrack);
    project.objects.push_back(secondOwner);
    PieceNormalization secondOwnerFacts;
    secondOwnerFacts.piece = secondOwner.piece.chassis.id;
    facts.pieces.push_back(secondOwnerFacts);

    EXPECT_FALSE(BuildPieceIRForStructureOnly(project, facts, resources).has_value());
}

TEST(ProjectSchemaIr, RenderIsDeterministic) {
    const FrontEndResult front = CompileFrontEnd(fixtures::IrLineageMix());
    ASSERT_TRUE(front.ok());
    const ResourceTable resources = ResidentResources(*front.project);
    const std::optional<PieceIR> first =
        BuildPieceIRForStructureOnly(*front.project, *front.facts, resources);
    const std::optional<PieceIR> second =
        BuildPieceIRForStructureOnly(*front.project, *front.facts, resources);
    ASSERT_TRUE(first.has_value());
    ASSERT_TRUE(second.has_value());
    const std::string a = RenderPieceIR(*first);
    const std::string b = RenderPieceIR(*second);
    EXPECT_EQ(a, b);
}

TEST(ProjectSchemaIr, RenderDistinguishesTempoMaps) {
    const FrontEndResult front =
        CompileFrontEnd(fixtures::NormalizationOrdering());
    ASSERT_TRUE(front.ok());
    const ResourceTable resources = ResidentResources(*front.project);
    CanonicalProject slower = *front.project;
    bool changed = false;
    for (ContentObject& obj : slower.objects) {
        if (obj.kind == ContentKind::Piece) {
            ASSERT_EQ(obj.piece.timeSpec.tempoMap.size(), 1u);
            obj.piece.timeSpec.tempoMap[0].microsPerQuarter = 1'000'000;
            changed = true;
        }
    }
    ASSERT_TRUE(changed);

    const std::optional<PieceIR> original =
        BuildPieceIRForStructureOnly(*front.project, *front.facts, resources);
    const std::optional<PieceIR> modified =
        BuildPieceIRForStructureOnly(slower, *front.facts, resources);
    ASSERT_TRUE(original.has_value());
    ASSERT_TRUE(modified.has_value());
    EXPECT_NE(RenderPieceIR(*original), RenderPieceIR(*modified));
}

TEST(ProjectSchemaIr, RenderDistinguishesMeterMaps) {
    const FrontEndResult front =
        CompileFrontEnd(fixtures::NormalizationOrdering());
    ASSERT_TRUE(front.ok());
    const ResourceTable resources = ResidentResources(*front.project);
    CanonicalProject tripleMeter = *front.project;
    bool changed = false;
    for (ContentObject& obj : tripleMeter.objects) {
        if (obj.kind == ContentKind::Piece) {
            ASSERT_EQ(obj.piece.timeSpec.meterMap.size(), 1u);
            obj.piece.timeSpec.meterMap[0].numerator = 3;
            changed = true;
        }
    }
    ASSERT_TRUE(changed);

    const std::optional<PieceIR> original =
        BuildPieceIRForStructureOnly(*front.project, *front.facts, resources);
    const std::optional<PieceIR> modified =
        BuildPieceIRForStructureOnly(tripleMeter, *front.facts, resources);
    ASSERT_TRUE(original.has_value());
    ASSERT_TRUE(modified.has_value());
    EXPECT_NE(RenderPieceIR(*original), RenderPieceIR(*modified));
}

}  // namespace
}  // namespace harkenprojectschema
