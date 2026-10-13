// Coverage for the compile-time gain/pitch/loop fold. The fold sums the gain
// (volume + makeUpGain) and the pitch (cents) of the
// subject and every contributor on its chain — its placement Track and owning
// Piece, and every one of its structural owners regardless of kind — then clamps
// the cent sum and converts it to a resample ratio. loop is the subject's
// own flag and is not inherited. These tests pin the dB addition across all chain
// levels, the cent clamp boundaries and the ratio mapping, that a Container
// contributes gain (unlike reference-slot resolution, which takes only a Layer),
// that loop does not inherit, and that the folded values reach the occurrence and
// its rendering.

#include "UnitTest/StudioObjectIdTestSupport.h"
#include "UnitTest/ProjectSchemaFixtures.h"

#include "harken_project_schema/BuildPieceIR.h"
#include "harken_project_schema/FrontEnd.h"
#include "harken_project_schema/Model.h"
#include "harken_project_schema/PieceIR.h"
#include "harken_project_schema/PropertyFold.h"
#include "harken_project_schema/ResourceInput.h"
#include "harken_project_schema/Serialize.h"

#include <gtest/gtest.h>

#include <bit>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string>

namespace harkenprojectschema {
namespace {

using fixtures::G;

CanonicalProject Compile(const ProjectInput& input) {
    const FrontEndResult front = CompileFrontEnd(input);
    EXPECT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    if (!front.project.has_value()) {
        return CanonicalProject{};
    }
    return *front.project;
}

ResourceInput StereoResource() {
    ResourceInput input;
    input.facts.valid = true;
    input.facts.sampleRate = 48000;
    input.facts.channelCount = 2;
    input.facts.frameCount = 48000;
    input.seekable = true;
    return input;
}

ProjectInput FiveKindWorld() {
    ProjectInput in;
    InputNode layer = fixtures::Node(ContentKind::Layer, 1);
    layer.guid = harkentest::StudioObjectId(1);
    layer.ownedChildren = {G(2)};
    layer.volume = 0.25f;

    InputNode container = fixtures::Node(ContentKind::Container, 2);
    container.guid = harkentest::StudioObjectId(2);
    container.ownedChildren = {G(6)};
    container.volume = 0.5f;

    InputNode sound = fixtures::Node(ContentKind::Sound, 3);
    sound.guid = harkentest::StudioObjectId(3);
    sound.volume = 1.0f;
    sound.makeUpGain = 0.5f;
    sound.outputBusVolume = 0.25f;
    sound.pitch = 600.0f;
    sound.loopCount = 0;

    InputNode piece = fixtures::Node(ContentKind::Piece, 6);
    piece.guid = harkentest::StudioObjectId(6);
    piece.ownedChildren = {G(4)};
    piece.timeSpec = fixtures::AbsoluteTime();
    piece.startPoint = fixtures::EntryStart();
    piece.volume = 4.0f;

    InputNode track = fixtures::Node(ContentKind::Track, 4);
    track.guid = harkentest::StudioObjectId(4);
    track.ownedChildren = {G(3)};
    track.volume = 2.0f;
    track.pitch = 600.0f;
    InputClip clip = fixtures::Clip(100, 3);
    clip.guid = harkentest::StudioObjectId(100);
    track.clips = {clip};

    in.nodes = {layer, container, sound, piece, track};
    in.roots = {G(1)};
    return in;
}

// Every level of the chain — the subject, its placement Track and Piece, and its
// Container and Layer owners — contributes gain, summed in dB.
TEST(ProjectSchemaPropertyFold, GainSumsAcrossAllChainLevels) {
    const CanonicalProject project = Compile(FiveKindWorld());
    const PropertyFoldContext fold(project);
    const FoldedProperties folded = fold.Fold(/*subject=*/harkentest::StudioObjectId(3), /*trackId=*/harkentest::StudioObjectId(4));
    EXPECT_EQ(folded.baseVolumeDb, 8.25f);
    EXPECT_EQ(folded.outputBusVolumeDb, 0.25f);
}

TEST(ProjectSchemaPropertyFold, AncestorVoiceGainAccumulatesAndExplicitZeroOutputOverrides) {
    ProjectInput in;
    InputNode layer = fixtures::Node(ContentKind::Layer, 1);
    layer.guid = harkentest::StudioObjectId(1);
    layer.ownedChildren = {G(3)};
    layer.volume = 1.0f;
    layer.makeUpGain = 2.0f;
    layer.outputBusVolume = 4.0f;

    InputNode sound = fixtures::Node(ContentKind::Sound, 2);
    sound.guid = harkentest::StudioObjectId(2);
    InputNode track = fixtures::Node(ContentKind::Track, 3);
    track.guid = harkentest::StudioObjectId(3);
    track.ownedChildren = {G(2)};
    InputClip clip = fixtures::Clip(100, 2);
    clip.guid = harkentest::StudioObjectId(100);
    track.clips = {clip};

    in.nodes = {layer, sound, track};
    in.roots = {G(1)};

    const CanonicalProject project = Compile(in);
    const PropertyFoldContext fold(project);
    EXPECT_EQ(fold.Fold(/*subject=*/harkentest::StudioObjectId(2), /*trackId=*/harkentest::StudioObjectId(3)).baseVolumeDb,
              3.0f);
    EXPECT_EQ(fold.Fold(harkentest::StudioObjectId(2), harkentest::StudioObjectId(3)).outputBusVolumeDb, 0.0f);
}

// The placement Track and Piece are also structural owners of the subject.
// Each object contributes once even though both fold chains can reach it.
TEST(ProjectSchemaPropertyFold, TrackOwnedSubjectCountsOwnersOnce) {
    ProjectInput in;
    InputNode piece = fixtures::Node(ContentKind::Piece, 1);
    piece.guid = harkentest::StudioObjectId(1);
    piece.ownedChildren = {G(2)};
    piece.timeSpec = fixtures::AbsoluteTime();
    piece.startPoint = fixtures::EntryStart();
    piece.volume = 4.0f;
    piece.pitch = 400.0f;

    InputNode track = fixtures::Node(ContentKind::Track, 2);
    track.guid = harkentest::StudioObjectId(2);
    track.ownedChildren = {G(3)};
    track.volume = 2.0f;
    track.pitch = 200.0f;
    InputClip clip = fixtures::Clip(100, 3);
    clip.guid = harkentest::StudioObjectId(100);
    track.clips = {clip};

    InputNode sound = fixtures::Node(ContentKind::Sound, 3);
    sound.guid = harkentest::StudioObjectId(3);
    sound.volume = 1.0f;
    sound.pitch = 100.0f;

    in.nodes = {piece, track, sound};
    in.roots = {G(1)};

    const CanonicalProject project = Compile(in);
    const PropertyFoldContext fold(project);
    const FoldedProperties folded = fold.Fold(/*subject=*/harkentest::StudioObjectId(3), /*trackId=*/harkentest::StudioObjectId(2));
    EXPECT_EQ(folded.baseVolumeDb, 7.0f);
    EXPECT_FLOAT_EQ(folded.basePitchRatio,
                    static_cast<float>(std::exp2(7.0 / 12.0)));
}

// The cent sum along the same chain becomes a resample ratio: 1200 cents
// is exactly one octave, ratio 2.0.
TEST(ProjectSchemaPropertyFold, PitchCentSumBecomesRatio) {
    const CanonicalProject project = Compile(FiveKindWorld());
    const PropertyFoldContext fold(project);
    const FoldedProperties folded = fold.Fold(harkentest::StudioObjectId(3), harkentest::StudioObjectId(4));
    EXPECT_EQ(folded.basePitchRatio, 2.0f);
}

TEST(ProjectSchemaPropertyFold, OrganizationalOwnersContributePitch) {
    ProjectInput in;
    InputNode layer = fixtures::Node(ContentKind::Layer, 1);
    layer.guid = harkentest::StudioObjectId(1);
    layer.ownedChildren = {G(2)};
    layer.pitch = 400.0f;
    InputNode container = fixtures::Node(ContentKind::Container, 2);
    container.guid = harkentest::StudioObjectId(2);
    container.ownedChildren = {G(4)};
    container.pitch = 800.0f;
    InputNode sound = fixtures::Node(ContentKind::Sound, 3);
    sound.guid = harkentest::StudioObjectId(3);
    InputNode track = fixtures::Node(ContentKind::Track, 4);
    track.guid = harkentest::StudioObjectId(4);
    track.ownedChildren = {G(3)};
    InputClip clip = fixtures::Clip(100, 3);
    clip.guid = harkentest::StudioObjectId(100);
    track.clips = {clip};

    in.nodes = {layer, container, sound, track};
    in.roots = {G(1)};

    const CanonicalProject project = Compile(in);
    const PropertyFoldContext fold(project);
    EXPECT_EQ(fold.Fold(/*subject=*/harkentest::StudioObjectId(3), /*trackId=*/harkentest::StudioObjectId(4)).basePitchRatio,
              2.0f);
}

// loop is the subject's own flag; the chain does not carry it.
TEST(ProjectSchemaPropertyFold, LoopIsSubjectOwnFlag) {
    const CanonicalProject project = Compile(FiveKindWorld());
    const PropertyFoldContext fold(project);
    EXPECT_EQ(fold.Fold(harkentest::StudioObjectId(3), harkentest::StudioObjectId(4)).loopCount, 0u);
}

// A Container owner contributes gain to the fold, unlike reference-slot
// resolution where only a Layer supplies a value. Here the Container's 0.5 dB is
// part of the sum.
TEST(ProjectSchemaPropertyFold, ContainerOwnerContributesGain) {
    ProjectInput in;
    InputNode container = fixtures::Node(ContentKind::Container, 2);
    container.guid = harkentest::StudioObjectId(2);
    container.ownedChildren = {G(4)};
    container.volume = 3.0f;  // the only gain anywhere
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

    const CanonicalProject project = Compile(in);
    const PropertyFoldContext fold(project);
    EXPECT_EQ(fold.Fold(harkentest::StudioObjectId(3), harkentest::StudioObjectId(4)).baseVolumeDb, 3.0f);  // the Container was counted
}

TEST(ProjectSchemaPropertyFold, NonPieceTrackOwnerContributesThroughOwnershipOnly) {
    ProjectInput in;
    InputNode layer = fixtures::Node(ContentKind::Layer, 1);
    layer.guid = harkentest::StudioObjectId(1);
    layer.ownedChildren = {G(3)};
    layer.volume = 8.0f;
    InputNode track = fixtures::Node(ContentKind::Track, 2);
    track.guid = harkentest::StudioObjectId(2);
    track.ownedChildren = {G(4)};
    track.volume = 2.0f;
    InputClip clip = fixtures::Clip(100, 4);
    clip.guid = harkentest::StudioObjectId(100);
    track.clips = {clip};
    InputNode container = fixtures::Node(ContentKind::Container, 3);
    container.guid = harkentest::StudioObjectId(3);
    container.ownedChildren = {G(2)};
    container.volume = 4.0f;
    InputNode sound = fixtures::Node(ContentKind::Sound, 4);
    sound.guid = harkentest::StudioObjectId(4);
    sound.volume = 1.0f;

    in.nodes = {layer, track, container, sound};
    in.roots = {G(1)};

    const CanonicalProject project = Compile(in);
    const PropertyFoldContext fold(project);
    const auto chain = fold.ChainComponents(harkentest::StudioObjectId(4), harkentest::StudioObjectId(2));
    EXPECT_EQ(chain.placementChain, (std::vector<StableId>{harkentest::StudioObjectId(2)}));
    EXPECT_EQ(chain.organizationalChain, (std::vector<StableId>{harkentest::StudioObjectId(2), harkentest::StudioObjectId(3), harkentest::StudioObjectId(1)}));
    EXPECT_EQ(fold.Fold(harkentest::StudioObjectId(4), harkentest::StudioObjectId(2)).baseVolumeDb, 15.0f);
}

TEST(ProjectSchemaPropertyFold, ContainerSubjectFoldsLikeSound) {
    ProjectInput in;
    InputNode layer = fixtures::Node(ContentKind::Layer, 1);
    layer.guid = harkentest::StudioObjectId(1);
    layer.ownedChildren = {G(3)};
    layer.volume = 8.0f;
    InputNode container = fixtures::Node(ContentKind::Container, 2);
    container.guid = harkentest::StudioObjectId(2);
    container.volume = 1.0f;
    container.pitch = 400.0f;
    container.loopCount = 0;
    InputNode piece = fixtures::Node(ContentKind::Piece, 3);
    piece.guid = harkentest::StudioObjectId(3);
    piece.ownedChildren = {G(4)};
    piece.timeSpec = fixtures::AbsoluteTime();
    piece.startPoint = fixtures::EntryStart();
    piece.volume = 4.0f;
    InputNode track = fixtures::Node(ContentKind::Track, 4);
    track.guid = harkentest::StudioObjectId(4);
    track.ownedChildren = {G(2)};
    track.volume = 2.0f;
    track.pitch = 800.0f;
    InputClip clip = fixtures::Clip(5, 2);
    clip.guid = harkentest::StudioObjectId(5);
    track.clips = {clip};

    in.nodes = {layer, container, piece, track};
    in.roots = {G(1)};

    const CanonicalProject project = Compile(in);
    const PropertyFoldContext fold(project);
    const FoldedProperties folded = fold.Fold(/*subject=*/harkentest::StudioObjectId(2), /*trackId=*/harkentest::StudioObjectId(4));
    EXPECT_EQ(folded.baseVolumeDb, 15.0f);
    EXPECT_EQ(folded.basePitchRatio, 2.0f);
    EXPECT_EQ(folded.loopCount, 0u);
}

// loop set on an owner does not reach the subject.
TEST(ProjectSchemaPropertyFold, LoopDoesNotInheritFromOwner) {
    ProjectInput in;
    InputNode container = fixtures::Node(ContentKind::Container, 2);
    container.guid = harkentest::StudioObjectId(2);
    container.ownedChildren = {G(4)};
    container.loopCount = 0;  // owner loops
    InputNode sound = fixtures::Node(ContentKind::Sound, 3);
    sound.guid = harkentest::StudioObjectId(3);
    sound.loopCount = 1;  // subject does not
    InputNode track = fixtures::Node(ContentKind::Track, 4);
    track.guid = harkentest::StudioObjectId(4);
    track.ownedChildren = {G(3)};
    InputClip clip = fixtures::Clip(100, 3);
    clip.guid = harkentest::StudioObjectId(100);
    track.clips = {clip};
    in.nodes = {container, sound, track};
    in.roots = {G(2)};

    const CanonicalProject project = Compile(in);
    const PropertyFoldContext fold(project);
    EXPECT_EQ(fold.Fold(harkentest::StudioObjectId(3), harkentest::StudioObjectId(4)).loopCount, 1u);
}

// A single Sound with a cent sum past the +7200 ceiling clamps to 7200, an
// exact six octaves up: ratio 64.0.
TEST(ProjectSchemaPropertyFold, PitchClampsAtPositiveBoundary) {
    ProjectInput in;
    InputNode sound = fixtures::Node(ContentKind::Sound, 3);
    sound.guid = harkentest::StudioObjectId(3);
    sound.pitch = 10000.0f;  // past +7200
    InputNode track = fixtures::Node(ContentKind::Track, 4);
    track.guid = harkentest::StudioObjectId(4);
    track.ownedChildren = {G(3)};
    InputClip clip = fixtures::Clip(100, 3);
    clip.guid = harkentest::StudioObjectId(100);
    track.clips = {clip};
    in.nodes = {sound, track};
    in.roots = {G(4)};

    const CanonicalProject project = Compile(in);
    const PropertyFoldContext fold(project);
    EXPECT_EQ(fold.Fold(harkentest::StudioObjectId(3), harkentest::StudioObjectId(4)).basePitchRatio, 64.0f);  // 2^(7200/1200)
}

// The mirror: past -7200 clamps to -7200, six octaves down, ratio 0.015625.
TEST(ProjectSchemaPropertyFold, PitchClampsAtNegativeBoundary) {
    ProjectInput in;
    InputNode sound = fixtures::Node(ContentKind::Sound, 3);
    sound.guid = harkentest::StudioObjectId(3);
    sound.pitch = -10000.0f;  // past -7200
    InputNode track = fixtures::Node(ContentKind::Track, 4);
    track.guid = harkentest::StudioObjectId(4);
    track.ownedChildren = {G(3)};
    InputClip clip = fixtures::Clip(100, 3);
    clip.guid = harkentest::StudioObjectId(100);
    track.clips = {clip};
    in.nodes = {sound, track};
    in.roots = {G(4)};

    const CanonicalProject project = Compile(in);
    const PropertyFoldContext fold(project);
    EXPECT_EQ(fold.Fold(harkentest::StudioObjectId(3), harkentest::StudioObjectId(4)).basePitchRatio, 0.015625f);  // 2^(-7200/1200)
}

TEST(ProjectSchemaPropertyFold, PitchClampsAfterSummingContributors) {
    ProjectInput in;
    InputNode sound = fixtures::Node(ContentKind::Sound, 1);
    sound.guid = harkentest::StudioObjectId(1);
    sound.pitch = 10000.0f;
    InputNode track = fixtures::Node(ContentKind::Track, 2);
    track.guid = harkentest::StudioObjectId(2);
    track.ownedChildren = {G(1)};
    track.pitch = -5000.0f;
    InputClip clip = fixtures::Clip(3, 1);
    clip.guid = harkentest::StudioObjectId(3);
    track.clips = {clip};

    in.nodes = {sound, track};
    in.roots = {G(2)};

    const CanonicalProject project = Compile(in);
    const PropertyFoldContext fold(project);
    EXPECT_FLOAT_EQ(fold.Fold(/*subject=*/harkentest::StudioObjectId(1), /*trackId=*/harkentest::StudioObjectId(2)).basePitchRatio,
                    static_cast<float>(std::exp2(50.0 / 12.0)));
}

// A non-boundary cent sum maps by 2^(cent/1200); pinning the formula, not just
// the octave-aligned values.
TEST(ProjectSchemaPropertyFold, PitchRatioFollowsCentConversion) {
    ProjectInput in;
    InputNode sound = fixtures::Node(ContentKind::Sound, 3);
    sound.guid = harkentest::StudioObjectId(3);
    sound.pitch = 700.0f;  // a perfect fifth, not an octave
    InputNode track = fixtures::Node(ContentKind::Track, 4);
    track.guid = harkentest::StudioObjectId(4);
    track.ownedChildren = {G(3)};
    InputClip clip = fixtures::Clip(100, 3);
    clip.guid = harkentest::StudioObjectId(100);
    track.clips = {clip};
    in.nodes = {sound, track};
    in.roots = {G(4)};

    const CanonicalProject project = Compile(in);
    const PropertyFoldContext fold(project);
    EXPECT_FLOAT_EQ(fold.Fold(harkentest::StudioObjectId(3), harkentest::StudioObjectId(4)).basePitchRatio,
                    static_cast<float>(std::exp2(7.0 / 12.0)));
}

TEST(ProjectSchemaPropertyFold, DirectTriggerFoldsWithoutPlacement) {
    auto input = FiveKindWorld();
    input.nodes[1].ownedChildren = {G(3)};
    input.nodes[4].ownedChildren.clear();
    input.nodes[4].clips.clear();
    input.roots = {G(1), G(6)};
    const CanonicalProject project = Compile(input);
    const PropertyFoldContext fold(project);
    const FoldedProperties direct = fold.Fold(harkentest::StudioObjectId(3), kUnmintedStableId);
    EXPECT_EQ(direct.baseVolumeDb, 2.25f);
    EXPECT_EQ(direct.outputBusVolumeDb, 0.25f);
    // Pitch loses the Track's 600 cents: 600 remain.
    EXPECT_FLOAT_EQ(direct.basePitchRatio,
                    static_cast<float>(std::exp2(6.0 / 12.0)));
}

TEST(ProjectSchemaPropertyFold, OutputVolumeUsesSubjectSnapshotWhileClipMixRemainsAdditive) {
    using Id = harkenplayableproperties::PlayablePropertyIdV1;
    auto input = FiveKindWorld();
    for (auto& node : input.nodes) {
        node.presence.Clear(Id::OutputBusVolume);
        node.outputBusVolume = 0.0f;
    }
    input.nodes[0].outputBusVolume = -30.0f;
    input.nodes[0].presence.Set(Id::OutputBusVolume);
    input.nodes[3].outputBusVolume = -6.0f;
    input.nodes[3].presence.Set(Id::OutputBusVolume);
    const auto project = Compile(input);
    PropertyFoldContext fold(project);
    EXPECT_FLOAT_EQ(0.0f, fold.Fold(harkentest::StudioObjectId(3), harkentest::StudioObjectId(0)).outputBusVolumeDb);
    EXPECT_FLOAT_EQ(0.0f, fold.Fold(harkentest::StudioObjectId(3), harkentest::StudioObjectId(4)).outputBusVolumeDb);
    PlayablePropertyPayloadV1 occurrence;
    occurrence.outputBusVolume = -3.0f;
    occurrence.presence.Set(Id::OutputBusVolume);
    EXPECT_FLOAT_EQ(0.0f, fold.Fold(harkentest::StudioObjectId(3), harkentest::StudioObjectId(4), &occurrence).outputBusVolumeDb);
    occurrence.volume = -3.0f;
    occurrence.makeUpGain = 1.0f;
    occurrence.pitch = 1200.0f;
    occurrence.presence.Set(Id::Volume);
    occurrence.presence.Set(Id::MakeUpGain);
    occurrence.presence.Set(Id::Pitch);
    EXPECT_FLOAT_EQ(fold.Fold(harkentest::StudioObjectId(3), harkentest::StudioObjectId(4)).baseVolumeDb - 2.0f,
                   fold.Fold(harkentest::StudioObjectId(3), harkentest::StudioObjectId(4), &occurrence).baseVolumeDb);
    EXPECT_FLOAT_EQ(fold.Fold(harkentest::StudioObjectId(3), harkentest::StudioObjectId(4)).basePitchRatio * 2.0f,
                   fold.Fold(harkentest::StudioObjectId(3), harkentest::StudioObjectId(4), &occurrence).basePitchRatio);
    occurrence.outputBusVolume = 0.0f;
    EXPECT_FLOAT_EQ(0.0f, fold.Fold(harkentest::StudioObjectId(3), harkentest::StudioObjectId(4), &occurrence).outputBusVolumeDb);
}

TEST(ProjectSchemaPropertyFold, ContributionOrderHasStableFloatBits) {
    ProjectInput in;
    InputNode sound = fixtures::Node(ContentKind::Sound, 30);
    sound.guid = harkentest::StudioObjectId(30);
    sound.volume = 0.1f;
    InputNode piece = fixtures::Node(ContentKind::Piece, 10);
    piece.guid = harkentest::StudioObjectId(10);
    piece.ownedChildren = {G(20)};
    piece.timeSpec = fixtures::AbsoluteTime();
    piece.startPoint = fixtures::EntryStart();
    piece.volume = -0.3f;
    InputNode track = fixtures::Node(ContentKind::Track, 20);
    track.guid = harkentest::StudioObjectId(20);
    track.ownedChildren = {G(30)};
    track.volume = 0.2f;
    InputClip clip = fixtures::Clip(100, 30);
    clip.guid = harkentest::StudioObjectId(100);
    track.clips = {clip};

    in.nodes = {sound, piece, track};
    in.roots = {G(10)};

    const CanonicalProject firstProject = Compile(in);
    const CanonicalProject secondProject = Compile(in);
    const PropertyFoldContext firstFold(firstProject);
    const PropertyFoldContext secondFold(secondProject);
    const std::uint32_t firstBits = std::bit_cast<std::uint32_t>(
        firstFold.Fold(/*subject=*/harkentest::StudioObjectId(30), /*trackId=*/harkentest::StudioObjectId(20)).baseVolumeDb);
    const std::uint32_t secondBits = std::bit_cast<std::uint32_t>(
        secondFold.Fold(/*subject=*/harkentest::StudioObjectId(30), /*trackId=*/harkentest::StudioObjectId(20)).baseVolumeDb);
    EXPECT_EQ(firstBits, secondBits);
    EXPECT_EQ(firstBits, 0u);
}

// The folded values reach the built occurrence, so a later stage installs them
// from the IR rather than re-deriving the chain.
TEST(ProjectSchemaPropertyFold, OccurrenceCarriesFoldedValues) {
    const FrontEndResult front = CompileFrontEnd(FiveKindWorld());
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    ResourceTable resources;
    resources.emplace(harkentest::StudioObjectId(3), StereoResource());  // Sound S
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
    EXPECT_EQ(occ->baseVolumeDb, 8.25f);
    EXPECT_EQ(occ->outputBusVolumeDb, 0.25f);
    EXPECT_EQ(occ->basePitchRatio, 2.0f);
    EXPECT_EQ(occ->loopCount, 0u);
}

TEST(ProjectSchemaPropertyFold, RenderingPinsFoldFieldBits) {
    ProjectInput in = FiveKindWorld();
    in.nodes[1].volume = 0.0f;
    const FrontEndResult front = CompileFrontEnd(in);
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    ASSERT_TRUE(front.project.has_value());
    ASSERT_TRUE(front.facts.has_value());
    ResourceTable resources;
    resources.emplace(harkentest::StudioObjectId(3), StereoResource());
    const std::optional<PieceIR> ir =
        BuildPieceIRForStructureOnly(*front.project, *front.facts, resources);
    ASSERT_TRUE(ir.has_value());
    // Voice gain is 7.75 dB; the 0.25 dB output volume remains independent.
    EXPECT_NE(RenderPieceIR(*ir).find(
                  "/fold1089994752,1073741824,0,1048576000"),
              std::string::npos);
}

// The rendered IR reflects the folded values and stays deterministic; a project
// differing only in a folded gain renders differently.
TEST(ProjectSchemaPropertyFold, RenderReflectsFoldAndStaysDeterministic) {
    const FrontEndResult front = CompileFrontEnd(FiveKindWorld());
    ASSERT_TRUE(front.ok());
    ResourceTable resources;
    resources.emplace(harkentest::StudioObjectId(3), StereoResource());
    const std::optional<PieceIR> a =
        BuildPieceIRForStructureOnly(*front.project, *front.facts, resources);
    const std::optional<PieceIR> b =
        BuildPieceIRForStructureOnly(*front.project, *front.facts, resources);
    ASSERT_TRUE(a.has_value());
    ASSERT_TRUE(b.has_value());
    EXPECT_EQ(RenderPieceIR(*a), RenderPieceIR(*b));

    ProjectInput louder = FiveKindWorld();
    louder.nodes[2].volume = 9.0f;  // change the Sound's gain only
    const FrontEndResult loudFront = CompileFrontEnd(louder);
    ASSERT_TRUE(loudFront.ok());
    const std::optional<PieceIR> loud =
        BuildPieceIRForStructureOnly(*loudFront.project, *loudFront.facts, resources);
    ASSERT_TRUE(loud.has_value());
    EXPECT_NE(RenderPieceIR(*a), RenderPieceIR(*loud));
}

}  // namespace
}  // namespace harkenprojectschema
