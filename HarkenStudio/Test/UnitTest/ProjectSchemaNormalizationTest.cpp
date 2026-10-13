// Stage-4 (time and marker normalization) tests: the frozen Musical-map and
// single-exit rules that were deferred out of parse, and the derived facts — a
// Piece's marker table in ascending MarkerId order and resolved start coordinate, and
// a Track's clips in audio order. The ordering fixture authors things out of
// order on purpose, so a test that merely checked "facts exist" would pass while
// the normalization did nothing; these assert the resulting order and value.

#include "UnitTest/ProjectSchemaFixtures.h"

#include "harken_project_schema/FrontEnd.h"
#include "harken_project_schema/Serialize.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <map>
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

const PieceNormalization* FindPiece(const NormalizedFacts& facts, StableId id) {
    for (const PieceNormalization& p : facts.pieces) {
        if (p.piece == id) {
            return &p;
        }
    }
    return nullptr;
}

const TrackNormalization* FindTrack(const NormalizedFacts& facts, StableId id) {
    for (const TrackNormalization& t : facts.tracks) {
        if (t.track == id) {
            return &t;
        }
    }
    return nullptr;
}

// A refused compile must yield no model AND no facts — the "no partial output"
// contract now covers the facts product too.
void ExpectSingleRefusal(const ProjectInput& input, DiagnosticCode expected) {
    const FrontEndResult result = CompileFrontEnd(input);
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.project.has_value());
    EXPECT_FALSE(result.facts.has_value())
        << "a refused compile must yield no partial facts";
    ASSERT_EQ(result.diagnostics.size(), 1u)
        << RenderDiagnostics(result.diagnostics);
    EXPECT_EQ(result.diagnostics.front().code, expected)
        << "got " << DiagnosticCodeName(result.diagnostics.front().code);
}

TEST(ProjectSchemaNormalization, DerivesOrderAndStartCoordinate) {
    const FrontEndResult result = CompileFrontEnd(fixtures::NormalizationOrdering());
    ASSERT_TRUE(result.ok()) << RenderDiagnostics(result.diagnostics);
    ASSERT_TRUE(result.project.has_value());
    ASSERT_TRUE(result.facts.has_value());
    const CanonicalProject& project = *result.project;
    const NormalizedFacts& facts = *result.facts;

    // Track clips: the fact must list them in ascending time, not authored
    // order. Map each clip id to its authored time to check the resulting order.
    const ContentObject* trackObj = FindObject(project, G(2));
    ASSERT_NE(trackObj, nullptr);
    std::map<StableId, std::int64_t> clipTime;
    for (const Clip& clip : trackObj->track.clips) {
        clipTime[clip.id] = clip.time;
    }
    const TrackNormalization* track = FindTrack(facts, trackObj->chassis().id);
    ASSERT_NE(track, nullptr);
    ASSERT_EQ(track->clipsByOrder.size(), 3u);
    std::vector<std::int64_t> orderedTimes;
    for (const StableId id : track->clipsByOrder) {
        orderedTimes.push_back(clipTime[id]);
    }
    EXPECT_EQ(orderedTimes, (std::vector<std::int64_t>{0, 480, 960}));

    // Piece markers: the fact must list them in ascending MarkerId, not authored
    // order, and the start coordinate must resolve to the named marker's position.
    const ContentObject* pieceObj = FindObject(project, G(6));
    ASSERT_NE(pieceObj, nullptr);
    std::map<StableId, MarkerId> markerOfId;
    for (const Marker& marker : pieceObj->piece.markers) {
        markerOfId[marker.id] = marker.markerId;
    }
    const PieceNormalization* piece = FindPiece(facts, pieceObj->chassis().id);
    ASSERT_NE(piece, nullptr);
    std::vector<MarkerId> orderedMarkerIds;
    for (const StableId id : piece->markersByMarkerId) {
        orderedMarkerIds.push_back(markerOfId[id]);
    }
    EXPECT_EQ(orderedMarkerIds, (std::vector<MarkerId>{5, 7, 9}));

    EXPECT_EQ(piece->startKind, StartPointKind::Marker);
    EXPECT_EQ(piece->startMarkerId, 7u);
    EXPECT_EQ(piece->startCoordinate, 100);
}

TEST(ProjectSchemaNormalization, EntryStartResolvesToZero) {
    // The valid fixture's Piece (guid 6) starts at entry; its coordinate must be
    // 0 and it must carry no start marker.
    const FrontEndResult result = CompileFrontEnd(fixtures::Valid());
    ASSERT_TRUE(result.ok()) << RenderDiagnostics(result.diagnostics);
    ASSERT_TRUE(result.facts.has_value());
    const ContentObject* pieceObj = FindObject(*result.project, G(6));
    ASSERT_NE(pieceObj, nullptr);
    const PieceNormalization* piece =
        FindPiece(*result.facts, pieceObj->chassis().id);
    ASSERT_NE(piece, nullptr);
    EXPECT_EQ(piece->startKind, StartPointKind::Entry);
    EXPECT_EQ(piece->startCoordinate, 0);
    EXPECT_EQ(piece->startMarkerId, 0u);
}

TEST(ProjectSchemaNormalization, MusicalMapNotSingletonRefused) {
    ExpectSingleRefusal(fixtures::InvalidMusicalMapNotSingleton(),
                        DiagnosticCode::MusicalMapNotSingletonAtZero);
}

TEST(ProjectSchemaNormalization, MultipleExitMarkersRefused) {
    ExpectSingleRefusal(fixtures::InvalidMultipleExitMarkers(),
                        DiagnosticCode::MultipleExitMarkers);
}

TEST(ProjectSchemaNormalization, FactsAreDeterministic) {
    const std::string a =
        RenderResult(CompileFrontEnd(fixtures::NormalizationOrdering()));
    const std::string b =
        RenderResult(CompileFrontEnd(fixtures::NormalizationOrdering()));
    EXPECT_EQ(a, b);
}

}  // namespace
}  // namespace harkenprojectschema
