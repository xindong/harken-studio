// Focused coverage: three diagnostics the earlier
// fixtures exercised only incidentally — an unreachable object, an ownership
// edge to a missing GUID, and a Piece with no time spec. Each isolates its one
// rule and asserts a single refusal with no partial output, so a regression that
// stopped raising one of them would fail here rather than hide behind a
// multi-fault fixture.

#include "UnitTest/ProjectSchemaFixtures.h"

#include "harken_project_schema/FrontEnd.h"
#include "harken_project_schema/Serialize.h"

#include <gtest/gtest.h>

namespace harkenprojectschema {
namespace {

void ExpectSingleRefusal(const ProjectInput& input, DiagnosticCode expected) {
    const FrontEndResult result = CompileFrontEnd(input);
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.project.has_value());
    EXPECT_FALSE(result.facts.has_value());
    ASSERT_EQ(result.diagnostics.size(), 1u)
        << RenderDiagnostics(result.diagnostics);
    EXPECT_EQ(result.diagnostics.front().code, expected)
        << "got " << DiagnosticCodeName(result.diagnostics.front().code);
}

TEST(ProjectSchemaCoverage, OrphanNodeRefused) {
    ExpectSingleRefusal(fixtures::InvalidOrphanNode(),
                        DiagnosticCode::OwnedNotRootNorOwned);
}

TEST(ProjectSchemaCoverage, DanglingOwnershipRefused) {
    ExpectSingleRefusal(fixtures::InvalidDanglingOwnership(),
                        DiagnosticCode::DanglingOwnership);
}

TEST(ProjectSchemaCoverage, PieceMissingTimeSpecRefused) {
    ExpectSingleRefusal(fixtures::InvalidPieceMissingTimeSpec(),
                        DiagnosticCode::PieceMissingTimeSpec);
}

}  // namespace
}  // namespace harkenprojectschema
