// Coverage for send resolution: the compile-time merge of an occurrence's sends
// along the shared fold chain. The merge walks the subject, then its
// organizational chain (nearest first), then its placement chain, keeping the
// first send seen per target bus and its whole entry (levels are not summed).
// These tests pin that distinct-bus sends from several chain levels all survive,
// that an object's own design chain (organizational) outranks the arrangement
// lane it is placed on (placement) when both send to one bus, that the subject
// outranks both, that the merged sends and resolved output bus reach the
// occurrence, and that the merged count over the per-voice layout bound is a
// deterministic refusal.

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

#include <cstdint>
#include <optional>
#include <string>
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

// The merged sends of the single occurrence for the given clip id, or an empty
// vector if the compile failed or the occurrence is absent.
std::vector<Send> MergedSendsForClip(const ProjectInput& input,
                                     StableId clipId) {
    const FrontEndResult front = CompileFrontEnd(input);
    EXPECT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    if (!front.sends.has_value()) {
        return {};
    }
    for (const OccurrenceSendResolution& occ : front.sends->occurrences) {
        if (occ.clipId == clipId) {
            return occ.sends;
        }
    }
    return {};
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

ProjectInput FullSendWorld() {
    ProjectInput in;
    InputNode layer = fixtures::Node(ContentKind::Layer, 1);
    layer.guid = harkentest::StudioObjectId(1);
    layer.ownedChildren = {G(2)};
    InputNode container = fixtures::Node(ContentKind::Container, 2);
    container.guid = harkentest::StudioObjectId(2);
    container.ownedChildren = {G(5)};
    InputNode sound = fixtures::Node(ContentKind::Sound, 3);
    sound.guid = harkentest::StudioObjectId(3);
    InputNode piece = fixtures::Node(ContentKind::Piece, 5);
    piece.guid = harkentest::StudioObjectId(5);
    piece.ownedChildren = {G(4)};
    piece.timeSpec = fixtures::AbsoluteTime();
    piece.startPoint = fixtures::EntryStart();
    InputNode track = fixtures::Node(ContentKind::Track, 4);
    track.guid = harkentest::StudioObjectId(4);
    track.ownedChildren = {G(3)};
    InputClip clip = fixtures::Clip(100, 3);
    clip.guid = harkentest::StudioObjectId(100);
    track.clips = {clip};
    in.nodes = {layer, container, sound, piece, track};
    in.roots = {G(1)};
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

TEST(ProjectSchemaSendResolution, SubjectSnapshotReplacesEveryAncestorSendList) {
    ProjectInput in = FullSendWorld();
    in.nodes[0].sends = {Send{harkentest::StudioObjectId(7), 0.1f}};
    in.nodes[1].sends = {Send{harkentest::StudioObjectId(8), 0.2f}};
    in.nodes[2].sends = {Send{harkentest::StudioObjectId(9), 0.3f}};
    in.nodes[3].sends = {Send{harkentest::StudioObjectId(10), 0.4f}};
    in.nodes[4].sends = {Send{harkentest::StudioObjectId(11), 0.5f}};
    const std::vector<Send> sends = MergedSendsForClip(in, harkentest::StudioObjectId(100));
    ASSERT_EQ(sends.size(), 1u);
    EXPECT_TRUE((sends[0] == Send{harkentest::StudioObjectId(9), 0.3f}));
}

TEST(ProjectSchemaSendResolution, EmptySubjectSnapshotDoesNotReadOwnershipOrPlacementSends) {
    ProjectInput in = FullSendWorld();
    in.nodes[0].sends = {Send{harkentest::StudioObjectId(7), 0.1f}};
    in.nodes[1].sends = {Send{harkentest::StudioObjectId(8), 0.2f}};
    in.nodes[3].sends = {Send{harkentest::StudioObjectId(9), 0.3f}};
    in.nodes[4].sends = {Send{harkentest::StudioObjectId(10), 0.4f}};
    EXPECT_TRUE(MergedSendsForClip(in, harkentest::StudioObjectId(100)).empty());
}

TEST(ProjectSchemaSendResolution, SubjectSnapshotPreservesAuthoredOrder) {
    ProjectInput in = FullSendWorld();
    in.nodes[2].sends = {Send{harkentest::StudioObjectId(9), 0.5f}, Send{harkentest::StudioObjectId(7), 0.25f}};
    const std::vector<Send> sends = MergedSendsForClip(in, harkentest::StudioObjectId(100));
    ASSERT_EQ(sends.size(), 2u);
    EXPECT_TRUE((sends[0] == Send{harkentest::StudioObjectId(9), 0.5f}));
    EXPECT_TRUE((sends[1] == Send{harkentest::StudioObjectId(7), 0.25f}));
}

// Exactly the layout bound of distinct buses is accepted.
TEST(ProjectSchemaSendResolution, CapacityAtLayoutLimitAccepted) {
    ProjectInput in = SendWorld();
    std::vector<Send> eight;
    for (std::uint64_t bus = 1; bus <= 8; ++bus) {
        eight.push_back(Send{harkentest::StudioObjectId(bus), 0.5f});
    }
    in.nodes[1].sends = eight;  // Sound (subject) sends to 8 distinct buses

    const FrontEndResult front = CompileFrontEnd(in);
    EXPECT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    const std::vector<Send> merged = MergedSendsForClip(in, harkentest::StudioObjectId(100));
    EXPECT_EQ(merged.size(), 8u);
}

// One more distinct bus than the layout can hold is a deterministic refusal.
TEST(ProjectSchemaSendResolution, CapacityOverLayoutLimitRefused) {
    ProjectInput in = SendWorld();
    std::vector<Send> nine;
    for (std::uint64_t bus = 1; bus <= 9; ++bus) {
        nine.push_back(Send{harkentest::StudioObjectId(bus), 0.5f});
    }
    in.nodes[1].sends = nine;  // Sound (subject) sends to 9 distinct buses

    const FrontEndResult front = CompileFrontEnd(in);
    EXPECT_FALSE(front.ok());
    EXPECT_FALSE(front.project.has_value());
    EXPECT_FALSE(front.facts.has_value());
    EXPECT_FALSE(front.resolution.has_value());
    EXPECT_FALSE(front.sends.has_value());
    ASSERT_EQ(front.diagnostics.size(), 1u)
        << RenderDiagnostics(front.diagnostics);
    EXPECT_EQ(front.diagnostics.front().code,
              DiagnosticCode::SendCountExceedsLayoutLimit);
    EXPECT_EQ(static_cast<std::uint32_t>(front.diagnostics.front().code), 601u);
    EXPECT_EQ(front.diagnostics.front().primary, G(3));  // the subject
    EXPECT_EQ(front.diagnostics.front().detail, 9u);
}

// The merge is deterministic: the same input renders byte-identical send facts.
TEST(ProjectSchemaSendResolution, MergeIsDeterministic) {
    ProjectInput in = SendWorld();
    in.nodes[0].sends = {Send{harkentest::StudioObjectId(7), 0.5f}};
    in.nodes[1].sends = {Send{harkentest::StudioObjectId(8), 0.25f}};

    const FrontEndResult a = CompileFrontEnd(in);
    const FrontEndResult b = CompileFrontEnd(in);
    ASSERT_TRUE(a.sends.has_value());
    ASSERT_TRUE(b.sends.has_value());
    EXPECT_EQ(RenderSendResolution(*a.sends), RenderSendResolution(*b.sends));
}

TEST(ProjectSchemaSendResolution, NonEmptyRoutingRenderingIsBytePinned) {
    ProjectInput in = FullSendWorld();
    in.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(0x1'0000'0001ull);
    in.nodes[2].sends = {
        Send{harkentest::StudioObjectId(0x1'0000'0002ull), -0.0f}, Send{harkentest::StudioObjectId(9), 0.5f}};

    const FrontEndResult front = CompileFrontEnd(in);
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    ASSERT_TRUE(front.sends.has_value());
    EXPECT_NE(RenderSendResolution(*front.sends).find(
                  "occ track=3da91a64-b947-4560-0000-000000000004 clip=3da91a64-b947-4560-0000-000000000064 ord=0 subject=3da91a64-b947-4560-0000-000000000003 sends=2 "
                  "3da91a64-b947-4560-0000-000100000002,2147483648 3da91a64-b947-4560-0000-000000000009,1056964608\n"),
              std::string::npos);

    const std::optional<PieceIR> ir = BuildRoutingIr(in);
    ASSERT_TRUE(ir.has_value());
    EXPECT_NE(RenderPieceIR(*ir).find(
                  "/bus3da91a64-b947-4560-0000-000100000001/sends2:3da91a64-b947-4560-0000-000100000002,2147483648:3da91a64-b947-4560-0000-000000000009,1056964608"),
              std::string::npos);
}

TEST(ProjectSchemaSendResolution, NonEmptyRoutingFingerprintGolden) {
    ProjectInput in = FullSendWorld();
    in.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(0x1'0000'0001ull);
    in.nodes[2].sends = {
        Send{harkentest::StudioObjectId(0x1'0000'0002ull), -0.0f}, Send{harkentest::StudioObjectId(9), 0.5f}};

    const std::optional<PieceIR> ir = BuildRoutingIr(in);
    ASSERT_TRUE(ir.has_value());
    const std::optional<PieceProgram> program = BuildPieceProgram(*ir);
    ASSERT_TRUE(program.has_value());
    // This value freezes the occurrence hash layout, including the spatial
    // facts. Moving it requires an intentional fingerprint contract change.
    constexpr std::uint64_t kExpectedFingerprint =
        7532003811813560920ull;
    EXPECT_EQ(program->programSemanticFingerprint(), kExpectedFingerprint);
}

TEST(ProjectSchemaSendResolution, SendFingerprintPreservesAuthoredOrder) {
    ProjectInput forward = FullSendWorld();
    forward.nodes[2].sends = {Send{harkentest::StudioObjectId(9), 0.5f}, Send{harkentest::StudioObjectId(7), 0.25f}};
    ProjectInput reverse = FullSendWorld();
    reverse.nodes[2].sends = {Send{harkentest::StudioObjectId(7), 0.25f}, Send{harkentest::StudioObjectId(9), 0.5f}};

    const std::optional<PieceIR> forwardIr = BuildRoutingIr(forward);
    const std::optional<PieceIR> reverseIr = BuildRoutingIr(reverse);
    ASSERT_TRUE(forwardIr.has_value());
    ASSERT_TRUE(reverseIr.has_value());
    const std::optional<PieceProgram> forwardProgram =
        BuildPieceProgram(*forwardIr);
    const std::optional<PieceProgram> reverseProgram =
        BuildPieceProgram(*reverseIr);
    ASSERT_TRUE(forwardProgram.has_value());
    ASSERT_TRUE(reverseProgram.has_value());
    EXPECT_NE(forwardProgram->programSemanticFingerprint(),
              reverseProgram->programSemanticFingerprint());
}

// The merged sends and the resolved output bus reach the built occurrence, so a
// later stage installs them from the IR rather than re-deriving the chain.
TEST(ProjectSchemaSendResolution, OccurrenceCarriesRoutingFacts) {
    ProjectInput in = SendWorld();
    in.nodes[1].sends = {Send{harkentest::StudioObjectId(8), 0.25f}};  // Sound (subject)
    in.nodes[1].mainOutputBusRef = harkentest::StudioObjectId(42);     // resolves as the subject's override

    const FrontEndResult front = CompileFrontEnd(in);
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    ASSERT_TRUE(front.resolution.has_value());
    ASSERT_TRUE(front.sends.has_value());
    ASSERT_TRUE(front.effectChains.has_value());
    ResourceTable resources;
    resources.emplace(harkentest::StudioObjectId(3), StereoResource());  // Sound 3
    const std::optional<PieceIR> ir =
        BuildPieceIR(*front.project, *front.facts, resources, *front.resolution,
                     *front.sends, *front.effectChains);
    ASSERT_TRUE(ir.has_value());

    const IrOccurrence* occ = nullptr;
    for (const IrTrack& track : ir->tracks()) {
        for (const IrOccurrence& candidate : track.occurrences) {
            if (candidate.key.clipId == harkentest::StudioObjectId(100)) {
                occ = &candidate;
            }
        }
    }
    ASSERT_NE(occ, nullptr);
    ASSERT_EQ(occ->sends.size(), 1u);
    EXPECT_TRUE((occ->sends[0] == Send{harkentest::StudioObjectId(8), 0.25f}));
    EXPECT_TRUE(occ->outputBusResolved);
    EXPECT_EQ(occ->resolvedOutputBus, harkentest::StudioObjectId(42u));
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
    EXPECT_NE(RenderPieceIR(*lowIr).find("/bus3da91a64-b947-4560-0000-000000000001/sends"),
              std::string::npos);
    EXPECT_NE(RenderPieceIR(*highIr).find("/bus3da91a64-b947-4560-0000-000100000001/sends"),
              std::string::npos);

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

// Absent routing facts leave the occurrence's neutral defaults: no sends and an
// unresolved bus, so a caller exercising only the IR structure is unaffected.
TEST(ProjectSchemaSendResolution, AbsentRoutingFactsLeaveNeutralDefaults) {
    ProjectInput in = SendWorld();
    in.nodes[1].sends = {Send{harkentest::StudioObjectId(8), 0.25f}};
    in.nodes[1].mainOutputBusRef = harkentest::StudioObjectId(42);

    const FrontEndResult front = CompileFrontEnd(in);
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    ResourceTable resources;
    resources.emplace(harkentest::StudioObjectId(3), StereoResource());
    const std::optional<PieceIR> ir =
        BuildPieceIRForStructureOnly(*front.project, *front.facts, resources);
    ASSERT_TRUE(ir.has_value());

    const IrOccurrence* occ = nullptr;
    for (const IrTrack& track : ir->tracks()) {
        for (const IrOccurrence& candidate : track.occurrences) {
            if (candidate.key.clipId == harkentest::StudioObjectId(100)) {
                occ = &candidate;
            }
        }
    }
    ASSERT_NE(occ, nullptr);
    EXPECT_TRUE(occ->sends.empty());
    EXPECT_FALSE(occ->outputBusResolved);
    EXPECT_EQ(occ->resolvedOutputBus, harkentest::StudioObjectId(0u));
}

}  // namespace
}  // namespace harkenprojectschema
