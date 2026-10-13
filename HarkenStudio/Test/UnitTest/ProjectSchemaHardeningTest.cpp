#include "UnitTest/StudioObjectIdTestSupport.h"
// Focused contract coverage for sorting, fact ordering, diagnostics and
// serialization. The cases pin every level of the clip sort key, stable-id
// ordering of fact lists, and whether the Musical-map diagnostic distinguishes
// tempo from meter and comes out ordered, and that the serializer's facts
// section is present when it should be and absent when it should not.

#include "UnitTest/ProjectSchemaFixtures.h"

#include "harken_project_schema/FrontEnd.h"
#include "harken_project_schema/Serialize.h"

#include <gtest/gtest.h>

#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace harkenprojectschema {
namespace {

using fixtures::G;

const ContentObject* FindObject(const CanonicalProject& project,
                                const AuthoringGuid& guid) {
    for (const ContentObject& obj : project.objects) {
        if (obj.chassis().guid == guid) {
            return &obj;
        }
    }
    return nullptr;
}

const TrackNormalization* FindTrackFacts(const NormalizedFacts& facts,
                                         StableId trackId) {
    for (const TrackNormalization& t : facts.tracks) {
        if (t.track == trackId) {
            return &t;
        }
    }
    return nullptr;
}

// Item 1: the clip sort key is (time, semanticOrderKey, clipId), and every level
// is load-bearing. TrackKey shares a time so the key breaks the tie; TrackId
// shares time and key so the clip id breaks it. The ids are chosen so that
// dropping either level would produce a different order than asserted here.
TEST(ProjectSchemaHardening, ClipSortUsesEveryKeyLevel) {
    const FrontEndResult result = CompileFrontEnd(fixtures::ClipSortMultiLevel());
    ASSERT_TRUE(result.ok()) << RenderDiagnostics(result.diagnostics);

    const ContentObject* keyTrack = FindObject(*result.project, G(2));
    const ContentObject* idTrack = FindObject(*result.project, G(3));
    ASSERT_NE(keyTrack, nullptr);
    ASSERT_NE(idTrack, nullptr);

    std::map<StableId, SemanticOrderKey> keyOf;
    for (const Clip& clip : keyTrack->track.clips) {
        keyOf[clip.id] = clip.semanticOrderKey;
    }

    const TrackNormalization* keyFacts =
        FindTrackFacts(*result.facts, keyTrack->chassis().id);
    ASSERT_NE(keyFacts, nullptr);
    ASSERT_EQ(keyFacts->clipsByOrder.size(), 2u);
    // Ordered by ascending semanticOrderKey (3 then 9), which is the reverse of
    // the ascending-clip-id order — so the key level is doing the work.
    std::vector<SemanticOrderKey> keySeq;
    for (const StableId id : keyFacts->clipsByOrder) {
        keySeq.push_back(keyOf[id]);
    }
    EXPECT_EQ(keySeq, (std::vector<SemanticOrderKey>{3, 9}));

    const TrackNormalization* idFacts =
        FindTrackFacts(*result.facts, idTrack->chassis().id);
    ASSERT_NE(idFacts, nullptr);
    ASSERT_EQ(idFacts->clipsByOrder.size(), 2u);
    // Same time and key, so ascending clip id decides: 15 before 30.
    EXPECT_LT(idFacts->clipsByOrder[0], idFacts->clipsByOrder[1]);
}

// Item 2: the top-level fact lists are ordered by ascending stable id. Asserted
// by walking the lists directly, without any find-by-id helper, so the ordering
// itself is what is under test rather than a lookup that would paper over it.
TEST(ProjectSchemaHardening, FactListsAreStableIdOrdered) {
    const FrontEndResult result = CompileFrontEnd(fixtures::MultiPieceMultiTrack());
    ASSERT_TRUE(result.ok()) << RenderDiagnostics(result.diagnostics);
    const NormalizedFacts& facts = *result.facts;

    ASSERT_GE(facts.pieces.size(), 2u);
    std::vector<StableId> pieceIds;
    for (const PieceNormalization& piece : facts.pieces) {
        pieceIds.push_back(piece.piece);
    }
    EXPECT_EQ(pieceIds, (std::vector<StableId>{harkentest::StudioObjectId(40), harkentest::StudioObjectId(50)}));
    for (std::size_t i = 1; i < facts.pieces.size(); ++i) {
        EXPECT_LT(facts.pieces[i - 1].piece, facts.pieces[i].piece);
    }
    ASSERT_GE(facts.tracks.size(), 3u);
    std::vector<StableId> trackIds;
    for (const TrackNormalization& track : facts.tracks) {
        trackIds.push_back(track.track);
    }
    EXPECT_EQ(trackIds, (std::vector<StableId>{harkentest::StudioObjectId(10), harkentest::StudioObjectId(20), harkentest::StudioObjectId(30)}));
    for (std::size_t i = 1; i < facts.tracks.size(); ++i) {
        EXPECT_LT(facts.tracks[i - 1].track, facts.tracks[i].track);
    }
}

// Item 3: the Musical-map diagnostic carries detail 0 for tempo and 1 for meter,
// both-violate raises both in detail order, and a two-row map whose first row is
// at tick 0 is still refused (the rule is "exactly one row", not "first row at
// tick 0").
TEST(ProjectSchemaHardening, MusicalMapDetailDistinguishesTempoFromMeter) {
    const FrontEndResult tempo =
        CompileFrontEnd(fixtures::InvalidMusicalMapNotSingleton());
    ASSERT_EQ(tempo.diagnostics.size(), 1u);
    EXPECT_EQ(tempo.diagnostics[0].code,
              DiagnosticCode::MusicalMapNotSingletonAtZero);
    EXPECT_EQ(tempo.diagnostics[0].detail, 0u);

    const FrontEndResult meter =
        CompileFrontEnd(fixtures::InvalidMeterMapNotSingleton());
    ASSERT_EQ(meter.diagnostics.size(), 1u);
    EXPECT_EQ(meter.diagnostics[0].code,
              DiagnosticCode::MusicalMapNotSingletonAtZero);
    EXPECT_EQ(meter.diagnostics[0].detail, 1u);
}

TEST(ProjectSchemaHardening, MusicalBothMapsBadAreOrderedByDetail) {
    const FrontEndResult both =
        CompileFrontEnd(fixtures::InvalidMusicalBothMapsBad());
    ASSERT_EQ(both.diagnostics.size(), 2u);
    EXPECT_EQ(both.diagnostics[0].code,
              DiagnosticCode::MusicalMapNotSingletonAtZero);
    EXPECT_EQ(both.diagnostics[1].code,
              DiagnosticCode::MusicalMapNotSingletonAtZero);
    EXPECT_EQ(both.diagnostics[0].detail, 0u);  // tempo before meter
    EXPECT_EQ(both.diagnostics[1].detail, 1u);
    EXPECT_FALSE(both.project.has_value());
}

TEST(ProjectSchemaHardening, MusicalMultiRowFirstAtZeroStillRefused) {
    const FrontEndResult result =
        CompileFrontEnd(fixtures::InvalidMusicalMultiRowFirstAtZero());
    ASSERT_EQ(result.diagnostics.size(), 1u);
    EXPECT_EQ(result.diagnostics[0].code,
              DiagnosticCode::MusicalMapNotSingletonAtZero);
    EXPECT_EQ(result.diagnostics[0].detail, 0u);
    EXPECT_FALSE(result.project.has_value());
}

// Item 4: the serializer's facts section is present on success and absent on a
// refusal, so a regression that stopped rendering the facts would be caught.
TEST(ProjectSchemaHardening, RenderedFactsSectionIsDetectable) {
    const std::string ok = RenderResult(CompileFrontEnd(fixtures::Valid()));
    EXPECT_NE(ok.find("\nfacts\n"), std::string::npos)
        << "a successful compile must render a facts section";

    const std::string refused =
        RenderResult(CompileFrontEnd(fixtures::InvalidStructuralCycle()));
    EXPECT_EQ(refused.find("facts"), std::string::npos)
        << "a refused compile must render no facts";
}

// Item 4: an empty Piece round-trips through facts with an empty marker list.
TEST(ProjectSchemaHardening, EmptyPieceProducesEmptyMarkerFacts) {
    const FrontEndResult result = CompileFrontEnd(fixtures::EmptyPiece());
    ASSERT_TRUE(result.ok()) << RenderDiagnostics(result.diagnostics);
    ASSERT_TRUE(result.facts.has_value());
    ASSERT_EQ(result.facts->pieces.size(), 1u);
    EXPECT_TRUE(result.facts->pieces[0].markersByMarkerId.empty());
    EXPECT_EQ(result.facts->pieces[0].startKind, StartPointKind::Entry);
    EXPECT_TRUE(result.facts->tracks.empty());
}

// Item 4: a Start=Marker Piece renders its resolved start in the facts section.
TEST(ProjectSchemaHardening, StartMarkerFactsRender) {
    const std::string s =
        RenderResult(CompileFrontEnd(fixtures::NormalizationOrdering()));
    EXPECT_NE(s.find("start=marker"), std::string::npos);
    EXPECT_NE(s.find("coord=100"), std::string::npos);
}

}  // namespace
}  // namespace harkenprojectschema
