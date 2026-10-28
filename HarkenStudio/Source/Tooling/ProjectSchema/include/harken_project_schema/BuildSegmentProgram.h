// Assembling the immutable SegmentProgram from a built SegmentIR.
//
// This is the last stage in this library's scope: it takes a validated IR and
// bundles it with its active-capacity estimate, the capabilities it requires,
// and its audible semantic fingerprint. Everything it needs is already in the
// IR (folded Tracks with occurrence templates, Segments, the required lineage
// table); the estimate is derived here and the fingerprint computed here.
//
// It is all-or-nothing, the same optional philosophy the earlier stages follow:
// the one inconsistency that yields no program is an IR whose capacity cannot be
// estimated — a musical coordinate that overflows nanoseconds — which returns no
// value rather than a program with a missing estimate.
//
// Adapting the program to a runtime ProgramView and writing it into a Pack are a
// later, separate concern and are not done here.
#ifndef HARKEN_PROJECT_SCHEMA_BUILD_SEGMENT_PROGRAM_H
#define HARKEN_PROJECT_SCHEMA_BUILD_SEGMENT_PROGRAM_H

#include "harken_project_schema/SegmentIR.h"
#include "harken_project_schema/SegmentProgram.h"

#include <optional>

namespace harkenprojectschema {

std::optional<SegmentProgram> BuildSegmentProgram(const SegmentIR& ir);

}  // namespace harkenprojectschema

#endif  // HARKEN_PROJECT_SCHEMA_BUILD_SEGMENT_PROGRAM_H
