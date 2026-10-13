// PieceProgram assembly, the program semantic fingerprint, and capability
// prerequisites.
//
// The fingerprint cases pin the lineage/semantic split, audible edits and
// identity retargeting. The capability cases pin that prerequisites mirror the
// content, no more and no less.

#include "UnitTest/StudioObjectIdTestSupport.h"
#include "UnitTest/ProjectSchemaFixtures.h"

#include "harken_project_schema/BuildPieceIR.h"
#include "harken_project_schema/BuildPieceProgram.h"
#include "harken_project_schema/FrontEnd.h"
#include "harken_project_schema/ResourceInput.h"
#include "harken_project_schema/Serialize.h"

#include <gtest/gtest.h>

#include <optional>
#include <string>

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

std::optional<PieceProgram> BuildProgramFrom(const ProjectInput& input) {
    const FrontEndResult front = CompileFrontEnd(input);
    if (!front.ok() || !front.project.has_value() || !front.facts.has_value()) {
        return std::nullopt;
    }
    const std::optional<PieceIR> ir =
        BuildPieceIRForStructureOnly(*front.project, *front.facts, ResidentResources(*front.project));
    if (!ir.has_value()) {
        return std::nullopt;
    }
    return BuildPieceProgram(*ir);
}

TEST(ProjectSchemaProgram, AssemblesFromIr) {
    const std::optional<PieceProgram> program =
        BuildProgramFrom(fixtures::IrLineageMix());
    ASSERT_TRUE(program.has_value());
    // The program embeds the IR (with its lineage) and an estimate, and carries
    // both fingerprints.
    EXPECT_FALSE(program->ir().tracks().empty());
    EXPECT_NE(program->programSemanticFingerprint(), 0u);
    EXPECT_EQ(program->runtimeLineageFingerprint(),
              program->ir().runtimeLineageFingerprint());
    // The estimate is the one the IR produces.
    EXPECT_FALSE(program->capacityEstimate().scopes.empty());
}

TEST(ProjectSchemaProgram, UuidReassignmentChangesProgramAndLineageIdentity) {
    ProjectInput base = fixtures::IrLineageMix();
    ProjectInput renumbered = fixtures::IrLineageMix();
    const auto reassign = [](AuthoringGuid& id) {
        if (!IsNull(id)) id.high ^= UINT64_C(0x2b71d6eeda144363);
    };
    for (auto& node : renumbered.nodes) {
        reassign(node.guid);
        for (auto& edge : node.ownedChildren) reassign(edge.childGuid);
        for (auto& occurrence : node.containerChildOccurrences) reassign(occurrence.childGuid);
        for (auto& clip : node.clips) {
            reassign(clip.guid);
            reassign(clip.targetGuid);
        }
        for (auto& marker : node.markers) reassign(marker.guid);
    }
    for (auto& root : renumbered.roots) reassign(root);

    const std::optional<PieceProgram> a = BuildProgramFrom(base);
    const std::optional<PieceProgram> b = BuildProgramFrom(renumbered);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());

    EXPECT_NE(a->programSemanticFingerprint(), b->programSemanticFingerprint());
    EXPECT_NE(a->runtimeLineageFingerprint(), b->runtimeLineageFingerprint());
}

// The semantic fingerprint covers audible content: a tempo change and a fade
// change each move it.
TEST(ProjectSchemaProgram, SemanticFingerprintChangesOnAudibleEdit) {
    const FrontEndResult front = CompileFrontEnd(fixtures::MusicalOverlap());
    ASSERT_TRUE(front.ok());
    const ResourceTable resources = ResidentResources(*front.project);
    const std::optional<PieceIR> baseIr =
        BuildPieceIRForStructureOnly(*front.project, *front.facts, resources);
    ASSERT_TRUE(baseIr.has_value());
    const std::optional<PieceProgram> base = BuildPieceProgram(*baseIr);
    ASSERT_TRUE(base.has_value());

    // A different tempo.
    CanonicalProject slower = *front.project;
    for (ContentObject& obj : slower.objects) {
        if (obj.kind == ContentKind::Piece) {
            obj.piece.timeSpec.tempoMap[0].microsPerQuarter = 1'000'000;
        }
    }
    const std::optional<PieceIR> slowerIr =
        BuildPieceIRForStructureOnly(slower, *front.facts, resources);
    ASSERT_TRUE(slowerIr.has_value());
    const std::optional<PieceProgram> slowerProgram = BuildPieceProgram(*slowerIr);
    ASSERT_TRUE(slowerProgram.has_value());
    EXPECT_NE(base->programSemanticFingerprint(),
              slowerProgram->programSemanticFingerprint());

    // A different fade on a clip.
    CanonicalProject faded = *front.project;
    for (ContentObject& obj : faded.objects) {
        if (obj.kind == ContentKind::Track) {
            for (Clip& clip : obj.track.clips) {
                clip.fades.inLength += 128;
            }
        }
    }
    const std::optional<PieceIR> fadedIr =
        BuildPieceIRForStructureOnly(faded, *front.facts, resources);
    ASSERT_TRUE(fadedIr.has_value());
    const std::optional<PieceProgram> fadedProgram = BuildPieceProgram(*fadedIr);
    ASSERT_TRUE(fadedProgram.has_value());
    EXPECT_NE(base->programSemanticFingerprint(),
              fadedProgram->programSemanticFingerprint());
}

TEST(ProjectSchemaProgram, SemanticFingerprintChangesOnAuthoredClipDuration) {
    ProjectInput base = fixtures::IrLineageMix();
    ProjectInput shortened = base;
    bool changed = false;
    for (InputNode& node : shortened.nodes) {
        for (InputClip& clip : node.clips) {
            clip.duration = 123;
            changed = true;
        }
    }
    ASSERT_TRUE(changed);

    const std::optional<PieceProgram> a = BuildProgramFrom(base);
    const std::optional<PieceProgram> b = BuildProgramFrom(shortened);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_NE(a->programSemanticFingerprint(),
              b->programSemanticFingerprint());
}

TEST(ProjectSchemaProgram, SemanticFingerprintChangesWhenContentIsRetargeted) {
    ProjectInput first;
    InputNode track = fixtures::Node(ContentKind::Track, 1);
    track.ownedChildren = {fixtures::G(10), fixtures::G(20)};
    track.clips = {fixtures::Clip(100, 10)};
    const InputNode sound10 = fixtures::Node(ContentKind::Sound, 10);
    const InputNode sound20 = fixtures::Node(ContentKind::Sound, 20);
    first.nodes = {track, sound10, sound20};
    first.roots = {fixtures::G(1)};

    ProjectInput second = first;
    second.nodes[0].clips[0].targetGuid = fixtures::G(20);

    const std::optional<PieceProgram> a = BuildProgramFrom(first);
    const std::optional<PieceProgram> b = BuildProgramFrom(second);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_NE(a->programSemanticFingerprint(), b->programSemanticFingerprint());
    EXPECT_EQ(a->runtimeLineageFingerprint(), b->runtimeLineageFingerprint());
}

TEST(ProjectSchemaProgram, SemanticFingerprintTracksClipIdTieBreakOrder) {
    ProjectInput first;
    InputNode track = fixtures::Node(ContentKind::Track, 1);
    track.ownedChildren = {fixtures::G(10)};
    InputClip clip100 = fixtures::Clip(100, 10);
    clip100.guid = harkentest::StudioObjectId(40);
    InputClip clip200 = fixtures::Clip(200, 10);
    clip200.fades.inLength = 10;
    clip200.stableOccurrenceOrdinal = 1;
    clip200.guid = harkentest::StudioObjectId(50);
    track.clips = {clip100, clip200};
    const InputNode sound = fixtures::Node(ContentKind::Sound, 10);
    first.nodes = {track, sound};
    first.roots = {fixtures::G(1)};

    ProjectInput swapped = first;
    swapped.nodes[0].clips[0].guid = harkentest::StudioObjectId(50);
    swapped.nodes[0].clips[1].guid = harkentest::StudioObjectId(40);

    const std::optional<PieceProgram> a = BuildProgramFrom(first);
    const std::optional<PieceProgram> b = BuildProgramFrom(swapped);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_NE(a->programSemanticFingerprint(), b->programSemanticFingerprint());
}

TEST(ProjectSchemaProgram, CapabilitiesMirrorContent) {
    // IrLineageMix: an Absolute Piece and a standalone Track, Sound clips only.
    const std::optional<PieceProgram> mix =
        BuildProgramFrom(fixtures::IrLineageMix());
    ASSERT_TRUE(mix.has_value());
    const CapabilityPrerequisites& mixCaps = mix->capabilities();
    EXPECT_TRUE(mixCaps.Requires(ProgramCapability::AbsoluteTiming));
    EXPECT_TRUE(mixCaps.Requires(ProgramCapability::NaturalLengthPlayback));
    EXPECT_FALSE(mixCaps.Requires(ProgramCapability::MusicalTiming));
    EXPECT_FALSE(mixCaps.Requires(ProgramCapability::ContainerPlacement));
    EXPECT_FALSE(mixCaps.Requires(ProgramCapability::NestedPiecePlacement));

    // Valid: a Musical Piece, standalone Tracks, and Sound, Container and Piece
    // clip targets — every capability this vintage models.
    const std::optional<PieceProgram> valid =
        BuildProgramFrom(fixtures::Valid());
    ASSERT_TRUE(valid.has_value());
    const CapabilityPrerequisites& validCaps = valid->capabilities();
    EXPECT_TRUE(validCaps.Requires(ProgramCapability::MusicalTiming));
    EXPECT_TRUE(validCaps.Requires(ProgramCapability::AbsoluteTiming));
    EXPECT_TRUE(validCaps.Requires(ProgramCapability::NaturalLengthPlayback));
    EXPECT_TRUE(validCaps.Requires(ProgramCapability::ContainerPlacement));
    EXPECT_TRUE(validCaps.Requires(ProgramCapability::NestedPiecePlacement));
}

// All-or-nothing: an IR whose capacity cannot be estimated forms no program.
TEST(ProjectSchemaProgram, RefusesWhenCapacityUnestimable) {
    const FrontEndResult front = CompileFrontEnd(fixtures::MusicalOverflowTick());
    ASSERT_TRUE(front.ok());
    const std::optional<PieceIR> ir =
        BuildPieceIRForStructureOnly(*front.project, *front.facts, ResidentResources(*front.project));
    ASSERT_TRUE(ir.has_value());
    EXPECT_FALSE(BuildPieceProgram(*ir).has_value());
}

TEST(ProjectSchemaProgram, RenderIsDeterministic) {
    const std::optional<PieceProgram> a =
        BuildProgramFrom(fixtures::Valid());
    const std::optional<PieceProgram> b =
        BuildProgramFrom(fixtures::Valid());
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(RenderPieceProgram(*a), RenderPieceProgram(*b));
}

}  // namespace
}  // namespace harkenprojectschema
