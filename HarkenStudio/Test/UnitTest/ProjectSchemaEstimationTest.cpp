#include "UnitTest/ProjectSchemaFixtures.h"

#include "harken_project_schema/BuildSegmentIR.h"
#include "harken_project_schema/CapacityEstimate.h"
#include "harken_project_schema/FrontEnd.h"
#include "harken_project_schema/ResourceInput.h"
#include "harken_project_schema/Serialize.h"

#include <gtest/gtest.h>

#include <optional>

namespace harkenprojectschema {
namespace {

ResourceInput OneSecondResident() {
    ResourceInput input;
    input.facts.valid = true;
    input.facts.sampleRate = 48000;
    input.facts.channelCount = 2;
    input.facts.frameCount = 48000;
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

TEST(ProjectSchemaEstimation, MusicalTickOverflowRefusesEstimate) {
    const FrontEndResult front = CompileFrontEnd(fixtures::MusicalOverflowTick());
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    const std::optional<SegmentIR> ir =
        BuildSegmentIRForStructureOnly(*front.project, *front.facts, ResidentResources(*front.project));
    ASSERT_TRUE(ir.has_value());
    EXPECT_FALSE(EstimateCapacity(*ir).has_value());
}

TEST(ProjectSchemaEstimation, UnknownDurationMusicalTickOverflowRefusesEstimate) {
    ProjectInput input = fixtures::MusicalOverflowTick();
    input.nodes[0].kind = ContentKind::Container;
    const FrontEndResult front = CompileFrontEnd(input);
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    const std::optional<SegmentIR> ir =
        BuildSegmentIRForStructureOnly(*front.project, *front.facts, ResourceTable{});
    ASSERT_TRUE(ir.has_value());
    ASSERT_EQ(ir->tracks().size(), 1u);
    ASSERT_EQ(ir->tracks()[0].occurrences.size(), 1u);
    ASSERT_FALSE(ir->tracks()[0].occurrences[0].hasDuration);

    EXPECT_FALSE(EstimateCapacity(*ir).has_value());
}

}
}
