// End-to-end front-end fixtures: one valid project that must compile, a set of
// invalid projects that must each be refused for exactly one reason with no
// partial output, and a determinism check that the same input compiles to
// byte-identical output twice.
//
// Each invalid fixture isolates a single violation, so the assertion is sharp:
// not merely "it failed" but "it failed for this rule and produced nothing
// else". A fixture that raised a second, incidental diagnostic would be a
// weaker test, and size()==1 is what keeps it honest.

#include "UnitTest/ProjectSchemaFixtures.h"

#include "harken_project_schema/FrontEnd.h"
#include "harken_project_schema/Serialize.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <vector>

namespace harkenprojectschema {
namespace {

using fixtures::G;

const ContentObject* FindByGuid(const CanonicalProject& project,
                                const AuthoringGuid& guid) {
    for (const ContentObject& obj : project.objects) {
        if (obj.chassis().guid == guid) {
            return &obj;
        }
    }
    return nullptr;
}

// Every invalid fixture must produce exactly one diagnostic of the expected
// code and no canonical project — the "no partial output" contract in one place.
void ExpectSingleRefusal(const ProjectInput& input, DiagnosticCode expected) {
    const FrontEndResult result = CompileFrontEnd(input);
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.project.has_value())
        << "a refused compile must yield no partial project";
    ASSERT_EQ(result.diagnostics.size(), 1u)
        << RenderDiagnostics(result.diagnostics);
    EXPECT_EQ(result.diagnostics.front().code, expected)
        << "got " << DiagnosticCodeName(result.diagnostics.front().code);
}

TEST(ProjectSchemaFixture, ValidProjectCompiles) {
    const FrontEndResult result = CompileFrontEnd(fixtures::Valid());
    ASSERT_TRUE(result.ok()) << RenderDiagnostics(result.diagnostics);
    ASSERT_TRUE(result.project.has_value());

    const CanonicalProject& project = *result.project;
    EXPECT_EQ(project.objects.size(), 8u);
    EXPECT_EQ(project.roots.size(), 2u);

    // The nested Piece owns its own Track and is itself referenced as clip
    // content, both at once.
    const ContentObject* piece = FindByGuid(project, G(6));
    ASSERT_NE(piece, nullptr);
    ASSERT_EQ(piece->kind, ContentKind::Piece);
    ASSERT_EQ(piece->piece.tracks.size(), 1u);
    const ContentObject* pieceTrack = FindByGuid(project, G(7));
    ASSERT_NE(pieceTrack, nullptr);
    EXPECT_EQ(piece->piece.tracks.front(), pieceTrack->chassis().id);

    const ContentObject* sound = FindByGuid(project, G(4));
    ASSERT_NE(sound, nullptr);
    const StableId soundId = sound->chassis().id;
    int refs = 0;
    int tracksReferencing = 0;
    for (const ContentObject& obj : project.objects) {
        if (obj.kind != ContentKind::Track) {
            continue;
        }
        bool here = false;
        for (const Clip& clip : obj.track.clips) {
            if (clip.contentRef.kind == ContentKind::Sound &&
                clip.contentRef.target == soundId) {
                ++refs;
                here = true;
            }
        }
        if (here) {
            ++tracksReferencing;
        }
    }
    EXPECT_EQ(refs, 2);
    EXPECT_EQ(tracksReferencing, 1);

    // A Track filed under a Layer and a Track standing alone both exist.
    const ContentObject* underLayer = FindByGuid(project, G(3));
    const ContentObject* standalone = FindByGuid(project, G(2));
    ASSERT_NE(underLayer, nullptr);
    ASSERT_NE(standalone, nullptr);
    EXPECT_EQ(underLayer->kind, ContentKind::Track);
    EXPECT_EQ(standalone->kind, ContentKind::Track);
}

TEST(ProjectSchemaFixture, ParentTrackRefused) {
    ExpectSingleRefusal(fixtures::InvalidParentTrack(),
                        DiagnosticCode::IllegalOwnership);
}

TEST(ProjectSchemaFixture, TrackTimeFieldRefused) {
    ExpectSingleRefusal(fixtures::InvalidTrackTimeField(),
                        DiagnosticCode::TimeSpecOnNonPiece);
}

TEST(ProjectSchemaFixture, PieceOwningNonTrackRefused) {
    ExpectSingleRefusal(fixtures::InvalidPieceOwnsNonTrack(),
                        DiagnosticCode::IllegalOwnership);
}

TEST(ProjectSchemaFixture, StructuralCycleRefused) {
    ExpectSingleRefusal(fixtures::InvalidStructuralCycle(),
                        DiagnosticCode::StructuralCycle);
}

TEST(ProjectSchemaFixture, DuplicateStableIdRefused) {
    ExpectSingleRefusal(fixtures::InvalidDuplicateStableId(),
                        DiagnosticCode::DuplicateGuid);
}

TEST(ProjectSchemaFixture, DanglingContentRefRefused) {
    ExpectSingleRefusal(fixtures::InvalidDanglingContentRef(),
                        DiagnosticCode::DanglingContentRef);
}

TEST(ProjectSchemaFixture, ContentRefWrongKindRefused) {
    ExpectSingleRefusal(fixtures::InvalidContentRefType(),
                        DiagnosticCode::IllegalContentRefType);
}

TEST(ProjectSchemaFixture, ReservedMarkerIdRefused) {
    ExpectSingleRefusal(fixtures::InvalidReservedMarkerId(),
                        DiagnosticCode::ReservedMarkerId);
}

TEST(ProjectSchemaFixture, StartPointReservedMarkerRefused) {
    ExpectSingleRefusal(fixtures::InvalidStartPointReserved(),
                        DiagnosticCode::StartPointMarkerReserved);
}

// Determinism: the same input compiled twice yields byte-identical rendered
// output — including the diagnostic order — for both an accepted and a refused
// project.
TEST(ProjectSchemaFixture, ValidCompileIsDeterministic) {
    const std::string a = RenderResult(CompileFrontEnd(fixtures::Valid()));
    const std::string b = RenderResult(CompileFrontEnd(fixtures::Valid()));
    EXPECT_EQ(a, b);
}

TEST(ProjectSchemaFixture, RefusalIsDeterministic) {
    const std::string a =
        RenderResult(CompileFrontEnd(fixtures::InvalidStructuralCycle()));
    const std::string b =
        RenderResult(CompileFrontEnd(fixtures::InvalidStructuralCycle()));
    EXPECT_EQ(a, b);
}

}  // namespace
}  // namespace harkenprojectschema
