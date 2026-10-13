// FrameBatch templates and the active-capacity estimate.
//
// The template cases pin the event shape against the Voice protocol: a Sound
// occurrence emits Start then Stop, a Container or Piece occurrence emits only
// Start (its end is its own to decide), and neither emits SetStartCursor or
// Envelope. The estimate cases pin the interval-overlap count, the third state
// for unknown-duration occurrences (excluded from the peak, counted, and marking
// the bound a lower bound), the musical tick-to-nanosecond conversion, and its
// overflow refusal.

#include "UnitTest/ProjectSchemaFixtures.h"

#include "harken_project_schema/BuildPieceIR.h"
#include "harken_project_schema/CapacityEstimate.h"
#include "harken_project_schema/FrontEnd.h"
#include "harken_project_schema/ResourceInput.h"
#include "harken_project_schema/Serialize.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace harkenprojectschema {
namespace {

using fixtures::G;

ResourceInput OneSecondResident() {
    ResourceInput input;
    input.facts.valid = true;
    input.facts.sampleRate = 48000;
    input.facts.channelCount = 2;
    input.facts.frameCount = 48000;  // exactly one second
    input.seekable = true;
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

std::vector<std::uint32_t> Phases(const IrOccurrence& occ) {
    std::vector<std::uint32_t> phases;
    for (const TemplateEvent& ev : occ.frameBatch.events) {
        phases.push_back(ev.phasePriority);
    }
    return phases;
}

// A Sound occurrence's template is Start (20) then Stop (50), ordered by phase,
// with the occurrence's ordinal as its eventIndex — and nothing else.
TEST(ProjectSchemaEstimation, SoundOccurrenceEmitsStartThenStop) {
    const FrontEndResult front = CompileFrontEnd(fixtures::IrLineageMix());
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    const std::optional<PieceIR> ir =
        BuildPieceIRForStructureOnly(*front.project, *front.facts, ResidentResources(*front.project));
    ASSERT_TRUE(ir.has_value());

    for (const IrTrack& track : ir->tracks()) {
        for (const IrOccurrence& occ : track.occurrences) {
            ASSERT_EQ(occ.content.kind, ContentKind::Sound);
            EXPECT_EQ(Phases(occ),
                      (std::vector<std::uint32_t>{kPhasePriorityStart,
                                                  kPhasePriorityStop}));
            EXPECT_EQ(occ.frameBatch.eventIndex, occ.key.occurrenceOrdinal);
            EXPECT_EQ(occ.frameBatch.events[0].kind, TemplateEventKind::Start);
            EXPECT_EQ(occ.frameBatch.events[1].kind, TemplateEventKind::Stop);
        }
    }
}

// A Container or Piece occurrence emits only Start: its end is decided by its own
// program or exit marker, so there is no Stop to schedule.
TEST(ProjectSchemaEstimation, ContainerAndPieceOccurrencesEmitNoStop) {
    const FrontEndResult front = CompileFrontEnd(fixtures::Valid());
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    const std::optional<PieceIR> ir =
        BuildPieceIRForStructureOnly(*front.project, *front.facts, ResidentResources(*front.project));
    ASSERT_TRUE(ir.has_value());

    int startOnly = 0;
    int startStop = 0;
    for (const IrTrack& track : ir->tracks()) {
        for (const IrOccurrence& occ : track.occurrences) {
            if (occ.content.kind == ContentKind::Sound) {
                EXPECT_EQ(Phases(occ),
                          (std::vector<std::uint32_t>{kPhasePriorityStart,
                                                      kPhasePriorityStop}));
                ++startStop;
            } else {
                EXPECT_EQ(Phases(occ),
                          (std::vector<std::uint32_t>{kPhasePriorityStart}));
                ++startOnly;
            }
        }
    }
    EXPECT_GT(startOnly, 0);
    EXPECT_GT(startStop, 0);
}

// The active bound counts overlapping occurrences per scope, excludes the
// unknown-duration ones, counts them separately, and marks the bound a lower
// bound when any were excluded.
TEST(ProjectSchemaEstimation, CountsOverlapAndUnknownDurations) {
    const FrontEndResult front = CompileFrontEnd(fixtures::Valid());
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    const std::optional<PieceIR> ir =
        BuildPieceIRForStructureOnly(*front.project, *front.facts, ResidentResources(*front.project));
    ASSERT_TRUE(ir.has_value());
    const std::optional<CapacityEstimate> estimate = EstimateCapacity(*ir);
    ASSERT_TRUE(estimate.has_value());

    // Track 3 places two one-second Sound clips at ns 0 and 480 (they overlap)
    // plus a Container and a Piece clip (unknown duration); Track 2 places one
    // Sound clip. So the standalone-Track peaks are 2 and 1, summing to 3, and
    // the two unknown-duration occurrences are counted, not folded in.
    EXPECT_EQ(estimate->activeUpperBound, 3u);
    EXPECT_EQ(estimate->unknownDurationOccurrences, 2u);
    EXPECT_TRUE(estimate->activeIsLowerBound);
}

// A musical scope converts tick starts to nanoseconds with the constant tempo;
// two clips a quarter apart under a one-second Sound overlap, so the peak is 2
// and, with every occurrence bounded, the bound is exact.
TEST(ProjectSchemaEstimation, MusicalOverlapCountsTwo) {
    const FrontEndResult front = CompileFrontEnd(fixtures::MusicalOverlap());
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    const std::optional<PieceIR> ir =
        BuildPieceIRForStructureOnly(*front.project, *front.facts, ResidentResources(*front.project));
    ASSERT_TRUE(ir.has_value());
    const std::optional<CapacityEstimate> estimate = EstimateCapacity(*ir);
    ASSERT_TRUE(estimate.has_value());

    ASSERT_EQ(estimate->scopes.size(), 1u);
    EXPECT_TRUE(estimate->scopes[0].isPiece);
    EXPECT_EQ(estimate->scopes[0].peakActiveOccurrences, 2u);
    EXPECT_EQ(estimate->activeUpperBound, 2u);
    EXPECT_FALSE(estimate->activeIsLowerBound);
    EXPECT_EQ(estimate->unknownDurationOccurrences, 0u);
}

// A tick coordinate whose nanosecond value overflows int64 refuses the estimate
// rather than wrapping.
TEST(ProjectSchemaEstimation, MusicalTickOverflowRefusesEstimate) {
    const FrontEndResult front = CompileFrontEnd(fixtures::MusicalOverflowTick());
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    const std::optional<PieceIR> ir =
        BuildPieceIRForStructureOnly(*front.project, *front.facts, ResidentResources(*front.project));
    ASSERT_TRUE(ir.has_value());  // the IR itself does no tick-to-ns conversion
    EXPECT_FALSE(EstimateCapacity(*ir).has_value());
}

TEST(ProjectSchemaEstimation, UnknownDurationMusicalTickOverflowRefusesEstimate) {
    ProjectInput input = fixtures::MusicalOverflowTick();
    input.nodes[0].kind = ContentKind::Container;
    const FrontEndResult front = CompileFrontEnd(input);
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    const std::optional<PieceIR> ir =
        BuildPieceIRForStructureOnly(*front.project, *front.facts, ResourceTable{});
    ASSERT_TRUE(ir.has_value());
    ASSERT_EQ(ir->tracks().size(), 1u);
    ASSERT_EQ(ir->tracks()[0].occurrences.size(), 1u);
    ASSERT_FALSE(ir->tracks()[0].occurrences[0].hasDuration);

    EXPECT_FALSE(EstimateCapacity(*ir).has_value());
}

TEST(ProjectSchemaEstimation, AbuttingOccurrencesDoNotOverlap) {
    ProjectInput input = fixtures::MusicalOverlap();
    input.nodes[2].clips[1].time = 1920;
    const FrontEndResult front = CompileFrontEnd(input);
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    const std::optional<PieceIR> ir =
        BuildPieceIRForStructureOnly(*front.project, *front.facts,
                     ResidentResources(*front.project));
    ASSERT_TRUE(ir.has_value());
    const std::optional<CapacityEstimate> estimate = EstimateCapacity(*ir);
    ASSERT_TRUE(estimate.has_value());

    ASSERT_EQ(estimate->scopes.size(), 1u);
    EXPECT_EQ(estimate->scopes[0].peakActiveOccurrences, 1u);
    EXPECT_EQ(estimate->activeUpperBound, 1u);
    EXPECT_EQ(estimate->unknownDurationOccurrences, 0u);
}

TEST(ProjectSchemaEstimation, EstimateRenderIsDeterministic) {
    const FrontEndResult front = CompileFrontEnd(fixtures::Valid());
    ASSERT_TRUE(front.ok());
    const std::optional<PieceIR> ir =
        BuildPieceIRForStructureOnly(*front.project, *front.facts, ResidentResources(*front.project));
    ASSERT_TRUE(ir.has_value());
    const std::optional<CapacityEstimate> a = EstimateCapacity(*ir);
    const std::optional<CapacityEstimate> b = EstimateCapacity(*ir);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(RenderCapacityEstimate(*a), RenderCapacityEstimate(*b));
}

}  // namespace
}  // namespace harkenprojectschema
