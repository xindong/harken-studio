#include "UnitTest/StudioObjectIdTestSupport.h"
#include "UnitTest/ProjectSchemaFixtures.h"

#include "harken_project_schema/BuildPieceIR.h"
#include "harken_project_schema/BuildPieceProgram.h"
#include "harken_project_schema/FrontEnd.h"
#include "harken_project_schema/Model.h"
#include "harken_project_schema/PieceIR.h"
#include "harken_project_schema/ResourceInput.h"
#include "harken_project_schema/SendResolution.h"
#include "harken_project_schema/Serialize.h"

#include <gtest/gtest.h>

#include <optional>
#include <vector>

namespace harkenprojectschema {
namespace {

using fixtures::G;

ResourceInput StereoResource() {
    ResourceInput input;
    input.facts.valid = true;
    input.facts.sampleRate = 48000;
    input.facts.channelCount = 2;
    input.facts.frameCount = 48000;
    input.seekable = true;
    return input;
}

ProjectInput SendWorld() {
    ProjectInput in;
    InputNode container = fixtures::Node(ContentKind::Container, 2);
    container.guid = harkentest::StudioObjectId(2);
    container.ownedChildren = {G(4)};
    InputNode sound = fixtures::Node(ContentKind::Sound, 3);
    sound.guid = harkentest::StudioObjectId(3);
    InputNode track = fixtures::Node(ContentKind::Track, 4);
    track.guid = harkentest::StudioObjectId(4);
    track.ownedChildren = {G(3)};
    InputClip clip = fixtures::Clip(100, 3);
    clip.guid = harkentest::StudioObjectId(100);
    track.clips = {clip};
    in.nodes = {container, sound, track};
    in.roots = {G(2)};
    return in;
}

ProjectInput TwoOccurrenceSendWorld() {
    ProjectInput in = SendWorld();
    InputClip clip = fixtures::Clip(101, 3);
    clip.guid = harkentest::StudioObjectId(101);
    clip.stableOccurrenceOrdinal = 1;
    in.nodes[2].clips.push_back(clip);
    return in;
}

std::optional<PieceIR> BuildRoutingIr(const ProjectInput& input) {
    const FrontEndResult front = CompileFrontEnd(input);
    EXPECT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    if (!front.ok() || !front.project.has_value() ||
        !front.facts.has_value() || !front.resolution.has_value() ||
        !front.sends.has_value() || !front.effectChains.has_value()) {
        return std::nullopt;
    }
    ResourceTable resources;
    resources.emplace(harkentest::StudioObjectId(3), StereoResource());
    return BuildPieceIR(*front.project, *front.facts, resources,
                        *front.resolution, *front.sends, *front.effectChains);
}

TEST(ProjectSchemaSendResolution, WideOutputBusIdentityReachesProgram) {
    constexpr harkenstudio::ObjectId kLowBus = harkentest::StudioObjectId(1);
    constexpr harkenstudio::ObjectId kHighBus = harkentest::StudioObjectId(0x1'0000'0001ull);
    ProjectInput lowInput = SendWorld();
    lowInput.nodes[1].mainOutputBusRef = harkentest::StudioObjectId(kLowBus);
    ProjectInput highInput = SendWorld();
    highInput.nodes[1].mainOutputBusRef = harkentest::StudioObjectId(kHighBus);

    const std::optional<PieceIR> lowIr = BuildRoutingIr(lowInput);
    const std::optional<PieceIR> highIr = BuildRoutingIr(highInput);
    ASSERT_TRUE(lowIr.has_value());
    ASSERT_TRUE(highIr.has_value());
    ASSERT_EQ(lowIr->tracks().size(), 1u);
    ASSERT_EQ(highIr->tracks().size(), 1u);
    ASSERT_EQ(lowIr->tracks().front().occurrences.size(), 1u);
    ASSERT_EQ(highIr->tracks().front().occurrences.size(), 1u);
    EXPECT_EQ(lowIr->tracks().front().occurrences.front().resolvedOutputBus,
              kLowBus);
    EXPECT_EQ(highIr->tracks().front().occurrences.front().resolvedOutputBus,
              kHighBus);
    const std::optional<PieceProgram> lowProgram = BuildPieceProgram(*lowIr);
    const std::optional<PieceProgram> highProgram = BuildPieceProgram(*highIr);
    ASSERT_TRUE(lowProgram.has_value());
    ASSERT_TRUE(highProgram.has_value());
    EXPECT_NE(lowProgram->programSemanticFingerprint(),
              highProgram->programSemanticFingerprint());
}

TEST(ProjectSchemaSendResolution, ReferenceFactsMustMatchOccurrencesExactly) {
    const FrontEndResult front = CompileFrontEnd(TwoOccurrenceSendWorld());
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    ASSERT_TRUE(front.resolution.has_value());
    ASSERT_TRUE(front.sends.has_value());
    ASSERT_EQ(front.resolution->occurrences.size(), 2u);
    ResourceTable resources;
    resources.emplace(harkentest::StudioObjectId(3), StereoResource());

    std::vector<ReferenceResolutionFacts> invalid;
    ReferenceResolutionFacts missing = *front.resolution;
    missing.occurrences.pop_back();
    invalid.push_back(missing);
    ReferenceResolutionFacts duplicate = *front.resolution;
    duplicate.occurrences.push_back(duplicate.occurrences.front());
    invalid.push_back(duplicate);
    ReferenceResolutionFacts extra = *front.resolution;
    OccurrenceReferenceResolution extraOccurrence = extra.occurrences.front();
    extraOccurrence.trackId = harkentest::StudioObjectId(999);
    extraOccurrence.clipId = harkentest::StudioObjectId(998);
    extra.occurrences.push_back(extraOccurrence);
    invalid.push_back(extra);
    ReferenceResolutionFacts wrongSubject = *front.resolution;
    wrongSubject.occurrences.front().subjectId = harkentest::OffsetObjectId(wrongSubject.occurrences.front().subjectId, 1);
    invalid.push_back(wrongSubject);
    ReferenceResolutionFacts wrongOrdinal = *front.resolution;
    ++wrongOrdinal.occurrences.front().occurrenceOrdinal;
    invalid.push_back(wrongOrdinal);

    for (const ReferenceResolutionFacts& facts : invalid) {
        EXPECT_FALSE(BuildPieceIR(*front.project, *front.facts, resources,
                                  facts, *front.sends, *front.effectChains)
                         .has_value());
    }
}

TEST(ProjectSchemaSendResolution, SendFactsMustMatchOccurrencesExactly) {
    const FrontEndResult front = CompileFrontEnd(TwoOccurrenceSendWorld());
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    ASSERT_TRUE(front.resolution.has_value());
    ASSERT_TRUE(front.sends.has_value());
    ASSERT_EQ(front.sends->occurrences.size(), 2u);
    ResourceTable resources;
    resources.emplace(harkentest::StudioObjectId(3), StereoResource());

    std::vector<SendResolutionFacts> invalid;
    SendResolutionFacts missing = *front.sends;
    missing.occurrences.pop_back();
    invalid.push_back(missing);
    SendResolutionFacts duplicate = *front.sends;
    duplicate.occurrences.push_back(duplicate.occurrences.front());
    invalid.push_back(duplicate);
    SendResolutionFacts extra = *front.sends;
    OccurrenceSendResolution extraOccurrence = extra.occurrences.front();
    extraOccurrence.trackId = harkentest::StudioObjectId(999);
    extraOccurrence.clipId = harkentest::StudioObjectId(998);
    extra.occurrences.push_back(extraOccurrence);
    invalid.push_back(extra);
    SendResolutionFacts wrongSubject = *front.sends;
    wrongSubject.occurrences.front().subjectId = harkentest::OffsetObjectId(wrongSubject.occurrences.front().subjectId, 1);
    invalid.push_back(wrongSubject);
    SendResolutionFacts wrongOrdinal = *front.sends;
    ++wrongOrdinal.occurrences.front().occurrenceOrdinal;
    invalid.push_back(wrongOrdinal);

    for (const SendResolutionFacts& facts : invalid) {
        EXPECT_FALSE(BuildPieceIR(*front.project, *front.facts, resources,
                                  *front.resolution, facts,
                                  *front.effectChains)
                         .has_value());
    }
}

}
}
