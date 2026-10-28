#include "UnitTest/StudioObjectIdTestSupport.h"
#include "UnitTest/ProjectSchemaFixtures.h"

#include "harken_project_schema/BuildSegmentIR.h"
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
        BuildSegmentIRForStructureOnly(*front.project, missingTrack, resources).has_value());

    NormalizedFacts extraTrack = *front.facts;
    TrackNormalization unknownTrack;
    unknownTrack.track = harkentest::StudioObjectId(99'999);
    extraTrack.tracks.push_back(unknownTrack);
    EXPECT_FALSE(
        BuildSegmentIRForStructureOnly(*front.project, extraTrack, resources).has_value());

    NormalizedFacts duplicateTrack = *front.facts;
    duplicateTrack.tracks.push_back(duplicateTrack.tracks.front());
    EXPECT_FALSE(
        BuildSegmentIRForStructureOnly(*front.project, duplicateTrack, resources).has_value());

    NormalizedFacts missingSegment = *front.facts;
    missingSegment.segments.clear();
    EXPECT_FALSE(
        BuildSegmentIRForStructureOnly(*front.project, missingSegment, resources).has_value());

    NormalizedFacts extraSegment = *front.facts;
    SegmentNormalization unknownSegment;
    unknownSegment.segment = harkentest::StudioObjectId(88'888);
    extraSegment.segments.push_back(unknownSegment);
    EXPECT_FALSE(
        BuildSegmentIRForStructureOnly(*front.project, extraSegment, resources).has_value());

    NormalizedFacts duplicateSegment = *front.facts;
    duplicateSegment.segments.push_back(duplicateSegment.segments.front());
    EXPECT_FALSE(
        BuildSegmentIRForStructureOnly(*front.project, duplicateSegment, resources).has_value());
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
    EXPECT_FALSE(BuildSegmentIRForStructureOnly(*front.project, missing, resources).has_value());

    NormalizedFacts extra = *front.facts;
    extra.tracks[factIndex].clipsByOrder.push_back(harkentest::StudioObjectId(99'999));
    EXPECT_FALSE(BuildSegmentIRForStructureOnly(*front.project, extra, resources).has_value());

    NormalizedFacts duplicate = *front.facts;
    duplicate.tracks[factIndex].clipsByOrder.push_back(
        duplicate.tracks[factIndex].clipsByOrder.front());
    EXPECT_FALSE(BuildSegmentIRForStructureOnly(*front.project, duplicate, resources).has_value());
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

    EXPECT_FALSE(BuildSegmentIRForStructureOnly(*front.project, stale, resources).has_value());
}

TEST(ProjectSchemaIr, RejectsSegmentMarkerFactSetMismatch) {
    const FrontEndResult front =
        CompileFrontEnd(fixtures::NormalizationOrdering());
    ASSERT_TRUE(front.ok());
    const ResourceTable resources = ResidentResources(*front.project);
    ASSERT_EQ(front.facts->segments.size(), 1u);
    ASSERT_FALSE(front.facts->segments[0].markersByMarkerId.empty());

    NormalizedFacts missing = *front.facts;
    missing.segments[0].markersByMarkerId.pop_back();
    EXPECT_FALSE(BuildSegmentIRForStructureOnly(*front.project, missing, resources).has_value());

    NormalizedFacts extra = *front.facts;
    extra.segments[0].markersByMarkerId.push_back(harkentest::StudioObjectId(99'999));
    EXPECT_FALSE(BuildSegmentIRForStructureOnly(*front.project, extra, resources).has_value());

    NormalizedFacts duplicate = *front.facts;
    duplicate.segments[0].markersByMarkerId.push_back(
        duplicate.segments[0].markersByMarkerId.front());
    EXPECT_FALSE(BuildSegmentIRForStructureOnly(*front.project, duplicate, resources).has_value());
}

TEST(ProjectSchemaIr, RejectsStaleSegmentStartCoordinate) {
    const FrontEndResult front =
        CompileFrontEnd(fixtures::NormalizationOrdering());
    ASSERT_TRUE(front.ok());
    const ResourceTable resources = ResidentResources(*front.project);
    NormalizedFacts stale = *front.facts;
    ASSERT_EQ(stale.segments.size(), 1u);
    stale.segments[0].startCoordinate += 1;

    EXPECT_FALSE(BuildSegmentIRForStructureOnly(*front.project, stale, resources).has_value());
}

TEST(ProjectSchemaIr, RejectsStaleMarkerOrderWithSameIds) {
    const FrontEndResult front =
        CompileFrontEnd(fixtures::NormalizationOrdering());
    ASSERT_TRUE(front.ok());
    const ResourceTable resources = ResidentResources(*front.project);
    NormalizedFacts stale = *front.facts;
    ASSERT_EQ(stale.segments.size(), 1u);
    ASSERT_GT(stale.segments[0].markersByMarkerId.size(), 1u);
    std::swap(stale.segments[0].markersByMarkerId[0],
              stale.segments[0].markersByMarkerId[1]);

    EXPECT_FALSE(BuildSegmentIRForStructureOnly(*front.project, stale, resources).has_value());
}

TEST(ProjectSchemaIr, RejectsTrackWithMultipleSegmentOwners) {
    const FrontEndResult front = CompileFrontEnd(fixtures::IrLineageMix());
    ASSERT_TRUE(front.ok());
    CanonicalProject project = *front.project;
    NormalizedFacts facts = *front.facts;
    const ResourceTable resources = ResidentResources(project);

    StableId ownedTrack = kUnmintedStableId;
    for (const ContentObject& obj : project.objects) {
        if (obj.kind == ContentKind::Segment && !obj.segment.tracks.empty()) {
            ownedTrack = obj.segment.tracks.front();
            break;
        }
    }
    ASSERT_NE(ownedTrack, kUnmintedStableId);

    ContentObject secondOwner;
    secondOwner.kind = ContentKind::Segment;
    secondOwner.segment.chassis.id = harkentest::StudioObjectId(99'999);
    secondOwner.segment.tracks.push_back(ownedTrack);
    project.objects.push_back(secondOwner);
    SegmentNormalization secondOwnerFacts;
    secondOwnerFacts.segment = secondOwner.segment.chassis.id;
    facts.segments.push_back(secondOwnerFacts);

    EXPECT_FALSE(BuildSegmentIRForStructureOnly(project, facts, resources).has_value());
}

}
}
