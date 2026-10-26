#include "UnitTest/StudioObjectIdTestSupport.h"
#include "UnitTest/ProjectSchemaFixtures.h"

#include "harken_project_schema/BuildPieceIR.h"
#include "harken_project_schema/FrontEnd.h"
#include "harken_project_schema/ResourceInput.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <utility>
#include <vector>

namespace harkenprojectschema {
namespace {

ResourceInput OneSecondResident(bool seekable = true) {
    ResourceInput input;
    input.facts.valid = true;
    input.facts.sampleRate = 48000;
    input.facts.channelCount = 2;
    input.facts.frameCount = 48000;
    input.seekable = seekable;
    return input;
}

ResourceTable ResidentResources(const CanonicalProject& project) {
    ResourceTable table;
    for (const ContentObject& obj : project.objects) {
        if (obj.kind == ContentKind::Sound) {
            table.emplace(obj.chassis().id, OneSecondResident());
        }
    }
    return table;
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

}
}
