#include "UnitTest/StudioObjectIdTestSupport.h"
#include "UnitTest/ProjectSchemaFixtures.h"

#include "harken_project_schema/BuildPieceIR.h"
#include "harken_project_schema/BuildPieceProgram.h"
#include "harken_project_schema/FrontEnd.h"
#include "harken_project_schema/Model.h"
#include "harken_project_schema/ReferenceResolution.h"
#include "harken_project_schema/ResourceInput.h"
#include "harken_project_schema/Serialize.h"

#include "harken_reference_slots/ReferenceSlotResolver.h"

#include <gtest/gtest.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace harkenprojectschema {
namespace {

using fixtures::G;
using harkenreferenceslots::ReferenceSlot;
using harkenreferenceslots::ResolutionLevel;
using harkenreferenceslots::ResolutionOutcome;

constexpr std::size_t kOutputBus =
    static_cast<std::size_t>(ReferenceSlot::OutputBus);
constexpr std::size_t kAttenuation =
    static_cast<std::size_t>(ReferenceSlot::Attenuation);

// Compiles the input (asserting success) and resolves its reference slots.
ReferenceResolutionResult Resolve(const ProjectInput& input) {
    const FrontEndResult front = CompileFrontEnd(input);
    EXPECT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    EXPECT_TRUE(front.project.has_value());
    EXPECT_TRUE(front.facts.has_value());
    if (!front.project.has_value() || !front.facts.has_value()) {
        return {};
    }
    return BuildReferenceResolution(*front.project, *front.facts);
}

const OccurrenceReferenceResolution* FindOccurrence(
    const ReferenceResolutionFacts& facts, StableId clipId) {
    for (const OccurrenceReferenceResolution& occ : facts.occurrences) {
        if (occ.clipId == clipId) {
            return &occ;
        }
    }
    return nullptr;
}

const ContentObject* FindObject(const CanonicalProject& project, StableId id) {
    for (const ContentObject& object : project.objects) {
        if (object.chassis().id == id) {
            return &object;
        }
    }
    return nullptr;
}

ProjectInput LayeredWorld() {
    ProjectInput in;
    InputNode layer = fixtures::Node(ContentKind::Layer, 1);
    layer.guid = harkentest::StudioObjectId(1);
    layer.ownedChildren = {G(2)};
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
    in.nodes = {layer, container, sound, track};
    in.roots = {G(1)};
    return in;
}

TEST(ProjectSchemaReferenceResolution,
     CompileFrontEndPublishesResolutionMatchingStandaloneBuilder) {
    ProjectInput in = LayeredWorld();
    in.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(100);
    in.nodes[3].spatial.attenuationRef = harkentest::StudioObjectId(210);
    in.nodes[3].presence.Set(harkenplayableproperties::PlayablePropertyIdV1::SpatialAttenuationRef);

    const FrontEndResult front = CompileFrontEnd(in);
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    ASSERT_TRUE(front.project.has_value());
    ASSERT_TRUE(front.facts.has_value());
    ASSERT_TRUE(front.resolution.has_value());
    const ReferenceResolutionResult standalone =
        BuildReferenceResolution(*front.project, *front.facts);
    ASSERT_TRUE(standalone.ok()) << RenderDiagnostics(standalone.diagnostics);
    ASSERT_TRUE(standalone.facts.has_value());
    EXPECT_EQ(RenderReferenceResolution(*front.resolution),
              RenderReferenceResolution(*standalone.facts));

    const OccurrenceReferenceResolution* occurrence =
        FindOccurrence(*front.resolution, harkentest::StudioObjectId(100));
    ASSERT_NE(occurrence, nullptr);
    EXPECT_EQ(occurrence->resolvedSlots[kOutputBus].value, harkentest::StudioObjectId(100u));
    EXPECT_EQ(occurrence->resolvedSlots[kAttenuation].outcome, ResolutionOutcome::Unresolved);
}

// Nothing authored anywhere: every slot resolves to nothing, with an empty
// candidate list. The compiler supplies no slot default, so this is Unresolved
// rather than a fabricated value.
TEST(ProjectSchemaReferenceResolution, AbsentEverywhereIsUnresolved) {
    const ReferenceResolutionResult result = Resolve(LayeredWorld());
    ASSERT_TRUE(result.ok()) << RenderDiagnostics(result.diagnostics);
    ASSERT_TRUE(result.facts.has_value());
    const OccurrenceReferenceResolution* occ =
        FindOccurrence(*result.facts, harkentest::StudioObjectId(100));
    ASSERT_NE(occ, nullptr);
    EXPECT_EQ(occ->subjectId, harkentest::StudioObjectId(3u));
    EXPECT_EQ(occ->subjectKind, ContentKind::Sound);
    for (const auto& slot : occ->resolvedSlots) {
        EXPECT_EQ(slot.outcome, ResolutionOutcome::Unresolved);
        EXPECT_TRUE(slot.candidates.empty());
    }
}

TEST(ProjectSchemaReferenceResolution, SubjectAttenuationSnapshotControlsVoiceBinding) {
    using Property = harkenplayableproperties::PlayablePropertyIdV1;
    for (const uint64_t attenuation : {UINT64_C(0), UINT64_C(0x100000002)}) {
        ProjectInput input = LayeredWorld();
        input.nodes[0].spatial.attenuationRef = harkentest::StudioObjectId(777);
        input.nodes[0].presence.Set(Property::SpatialAttenuationRef);
        input.nodes[2].spatial.attenuationRef = harkentest::StudioObjectId(attenuation);
        input.nodes[2].presence.Set(Property::SpatialAttenuationRef);
        const auto front = CompileFrontEnd(input);
        ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
        const auto* occurrence = FindOccurrence(*front.resolution, harkentest::StudioObjectId(100));
        ASSERT_NE(occurrence, nullptr);
        const auto& slot = occurrence->resolvedSlots[kAttenuation];
        EXPECT_EQ(slot.outcome, ResolutionOutcome::Resolved);
        EXPECT_EQ(slot.value, harkentest::StudioObjectId(attenuation));
        EXPECT_EQ(slot.sourceObjectId, harkentest::StudioObjectId(3u));
        ResourceInput resource;
        resource.facts.valid = true;
        resource.facts.sampleRate = 48000;
        resource.facts.channelCount = 1;
        resource.facts.frameCount = 48000;
        const auto ir = BuildPieceIR(*front.project, *front.facts, ResourceTable{{harkentest::StudioObjectId(3), resource}},
            *front.resolution, *front.sends, *front.effectChains);
        ASSERT_TRUE(ir.has_value());
        ASSERT_EQ(ir->tracks()[0].occurrences.size(), 1u);
        EXPECT_EQ(ir->tracks()[0].occurrences[0].attenuationResolved, attenuation != 0);
        EXPECT_EQ(ir->tracks()[0].occurrences[0].resolvedAttenuation, harkentest::StudioObjectId(attenuation));
    }
}

TEST(ProjectSchemaReferenceResolution,
     UnresolvedSerializationDoesNotWalkAncestors) {
    const FrontEndResult front = CompileFrontEnd(LayeredWorld());
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    ASSERT_TRUE(front.project.has_value());
    ReferenceResolutionContext context(*front.project);

    const auto direct = context.Resolve(harkentest::StudioObjectId(3), {});
    const auto placed = context.Resolve(harkentest::StudioObjectId(3), {harkentest::StudioObjectId(4)});
    for (const auto& slot : direct) {
        EXPECT_EQ(slot.outcome, ResolutionOutcome::Unresolved);
        EXPECT_TRUE(slot.candidates.empty());
        EXPECT_EQ(slot.placementDepthWalked, 0u);
        EXPECT_EQ(slot.organizationalDepthWalked, 0u);
    }
    for (const auto& slot : placed) {
        EXPECT_EQ(slot.outcome, ResolutionOutcome::Unresolved);
        EXPECT_TRUE(slot.candidates.empty());
        EXPECT_EQ(slot.placementDepthWalked, 0u);
        EXPECT_EQ(slot.organizationalDepthWalked, 0u);
    }

    ReferenceResolutionFacts directFacts;
    OccurrenceReferenceResolution directOccurrence;
    directOccurrence.trackId = harkentest::StudioObjectId(4);
    directOccurrence.clipId = harkentest::StudioObjectId(100);
    directOccurrence.subjectId = harkentest::StudioObjectId(3);
    directOccurrence.subjectKind = ContentKind::Sound;
    directOccurrence.resolvedSlots = direct;
    directFacts.occurrences.push_back(directOccurrence);

    ReferenceResolutionFacts placedFacts = directFacts;
    placedFacts.occurrences.front().resolvedSlots = placed;

    const std::string directRender = RenderReferenceResolution(directFacts);
    const std::string placedRender = RenderReferenceResolution(placedFacts);
    EXPECT_EQ(directRender, placedRender);
    EXPECT_EQ(directRender,
              "resolution 1\n"
              "occ track=3da91a64-b947-4560-0000-000000000004 clip=3da91a64-b947-4560-0000-000000000064 ord=0 subject=sound:3da91a64-b947-4560-0000-000000000003\n"
              "  slot=outputBus outcome=unresolved pw=0 ow=0\n"
              "  slot=attenuation outcome=unresolved pw=0 ow=0\n");
    EXPECT_EQ(placedRender,
              "resolution 1\n"
              "occ track=3da91a64-b947-4560-0000-000000000004 clip=3da91a64-b947-4560-0000-000000000064 ord=0 subject=sound:3da91a64-b947-4560-0000-000000000003\n"
              "  slot=outputBus outcome=unresolved pw=0 ow=0\n"
              "  slot=attenuation outcome=unresolved pw=0 ow=0\n");
}

TEST(ProjectSchemaReferenceResolution, ObjectOverrideWins) {
    ProjectInput in = LayeredWorld();
    in.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(100);  // Sound S
    in.nodes[3].mainOutputBusRef = harkentest::StudioObjectId(110);  // Track T (placement)
    in.nodes[0].mainOutputBusRef = harkentest::StudioObjectId(120);  // Layer L (organizational)

    const ReferenceResolutionResult result = Resolve(in);
    ASSERT_TRUE(result.ok()) << RenderDiagnostics(result.diagnostics);
    const OccurrenceReferenceResolution* occ =
        FindOccurrence(*result.facts, harkentest::StudioObjectId(100));
    ASSERT_NE(occ, nullptr);
    const auto& bus = occ->resolvedSlots[kOutputBus];
    EXPECT_EQ(bus.outcome, ResolutionOutcome::Resolved);
    EXPECT_EQ(bus.level, ResolutionLevel::ObjectOverride);
    EXPECT_EQ(bus.value, harkentest::StudioObjectId(100u));
    EXPECT_EQ(bus.sourceObjectId, harkentest::StudioObjectId(3u));
    ASSERT_EQ(bus.candidates.size(), 1u);
    EXPECT_TRUE(bus.candidates.front().winner);
}

TEST(ProjectSchemaReferenceResolution, PlacementDoesNotSupplyDefinitionReferences) {
    ProjectInput in = LayeredWorld();
    in.nodes[3].mainOutputBusRef = harkentest::StudioObjectId(110);
    in.nodes[0].mainOutputBusRef = harkentest::StudioObjectId(120);
    const auto result = Resolve(in);
    ASSERT_TRUE(result.ok());
    const auto* occurrence = FindOccurrence(*result.facts, harkentest::StudioObjectId(100));
    ASSERT_NE(occurrence, nullptr);
    EXPECT_EQ(occurrence->resolvedSlots[kOutputBus].outcome, ResolutionOutcome::Unresolved);
    EXPECT_TRUE(occurrence->resolvedSlots[kOutputBus].candidates.empty());
}

TEST(ProjectSchemaReferenceResolution, OwnershipDoesNotRepeatDefinitionResolution) {
    ProjectInput in = LayeredWorld();
    in.nodes[0].mainOutputBusRef = harkentest::StudioObjectId(120);
    const auto result = Resolve(in);
    ASSERT_TRUE(result.ok());
    const auto* occurrence = FindOccurrence(*result.facts, harkentest::StudioObjectId(100));
    ASSERT_NE(occurrence, nullptr);
    EXPECT_EQ(occurrence->resolvedSlots[kOutputBus].outcome, ResolutionOutcome::Unresolved);
    EXPECT_TRUE(occurrence->resolvedSlots[kOutputBus].candidates.empty());
}

TEST(ProjectSchemaReferenceResolution, NestedOwnershipLayersCannotReplaceTheSubjectSnapshot) {
    ProjectInput in = LayeredWorld();
    InputNode inner = fixtures::Node(ContentKind::Layer, 5);
    inner.guid = harkentest::StudioObjectId(5);
    inner.ownedChildren = {G(2)};
    inner.mainOutputBusRef = harkentest::StudioObjectId(125);
    in.nodes[0].ownedChildren = {G(5)};
    in.nodes[0].mainOutputBusRef = harkentest::StudioObjectId(120);
    in.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(140);
    in.nodes.push_back(inner);
    const auto result = Resolve(in);
    ASSERT_TRUE(result.ok());
    const auto* occurrence = FindOccurrence(*result.facts, harkentest::StudioObjectId(100));
    ASSERT_NE(occurrence, nullptr);
    EXPECT_EQ(occurrence->resolvedSlots[kOutputBus].value, harkentest::StudioObjectId(140u));
    EXPECT_EQ(occurrence->resolvedSlots[kOutputBus].sourceObjectId, harkentest::StudioObjectId(3u));
    ASSERT_EQ(occurrence->resolvedSlots[kOutputBus].candidates.size(), 1u);
}

TEST(ProjectSchemaReferenceResolution, ContainerAndLayerDoNotRepeatDefinitionResolution) {
    ProjectInput in = LayeredWorld();
    in.nodes[1].mainOutputBusRef = harkentest::StudioObjectId(130);
    in.nodes[0].mainOutputBusRef = harkentest::StudioObjectId(120);
    const auto result = Resolve(in);
    ASSERT_TRUE(result.ok());
    const auto* occurrence = FindOccurrence(*result.facts, harkentest::StudioObjectId(100));
    ASSERT_NE(occurrence, nullptr);
    EXPECT_EQ(occurrence->resolvedSlots[kOutputBus].outcome, ResolutionOutcome::Unresolved);
    EXPECT_TRUE(occurrence->resolvedSlots[kOutputBus].candidates.empty());
}

TEST(ProjectSchemaReferenceResolution, SlotsResolveIndependently) {
    ProjectInput in = LayeredWorld();
    in.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(100);        // Sound: output bus by override
    in.nodes[3].spatial.attenuationRef = harkentest::StudioObjectId(210);  // Track: attenuation by placement
    in.nodes[3].presence.Set(harkenplayableproperties::PlayablePropertyIdV1::SpatialAttenuationRef);

    const ReferenceResolutionResult result = Resolve(in);
    ASSERT_TRUE(result.ok()) << RenderDiagnostics(result.diagnostics);
    const OccurrenceReferenceResolution* occ =
        FindOccurrence(*result.facts, harkentest::StudioObjectId(100));
    ASSERT_NE(occ, nullptr);
    EXPECT_EQ(occ->resolvedSlots[kOutputBus].level, ResolutionLevel::ObjectOverride);
    EXPECT_EQ(occ->resolvedSlots[kOutputBus].value, harkentest::StudioObjectId(100u));
    EXPECT_EQ(occ->resolvedSlots[kAttenuation].outcome, ResolutionOutcome::Unresolved);
    EXPECT_EQ(occ->resolvedSlots[kAttenuation].value, harkentest::StudioObjectId(0u));
}

TEST(ProjectSchemaReferenceResolution,
     WideReferenceValuesPassThroughResolveAndRender) {
    constexpr harkenstudio::ObjectId outputBus{0x1122334455664788ull, 0x100000001ull};
    constexpr harkenstudio::ObjectId attenuation{0x8877665544334211ull, 0x100000002ull};
    ProjectInput in = LayeredWorld();
    in.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(outputBus);
    in.nodes[2].spatial.attenuationRef = harkentest::StudioObjectId(attenuation);
    in.nodes[2].presence.Set(harkenplayableproperties::PlayablePropertyIdV1::SpatialAttenuationRef);

    const FrontEndResult front = CompileFrontEnd(in);
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    ASSERT_TRUE(front.project.has_value());
    ASSERT_TRUE(front.facts.has_value());
    const ContentObject* sound = FindObject(*front.project, harkentest::StudioObjectId(3));
    ASSERT_NE(sound, nullptr);
    EXPECT_EQ(sound->chassis().mainOutputBusRef, outputBus);
    EXPECT_EQ(sound->chassis().spatial.attenuationRef, attenuation);

    const ReferenceResolutionResult resolution =
        BuildReferenceResolution(*front.project, *front.facts);
    ASSERT_TRUE(resolution.ok()) << RenderDiagnostics(resolution.diagnostics);
    ASSERT_TRUE(resolution.facts.has_value());
    const OccurrenceReferenceResolution* occurrence =
        FindOccurrence(*resolution.facts, harkentest::StudioObjectId(100));
    ASSERT_NE(occurrence, nullptr);
    EXPECT_EQ(occurrence->resolvedSlots[kOutputBus].value, outputBus);
    EXPECT_EQ(occurrence->resolvedSlots[kAttenuation].value, attenuation);
    EXPECT_EQ(RenderReferenceResolution(*resolution.facts),
              "resolution 1\n"
              "occ track=3da91a64-b947-4560-0000-000000000004 clip=3da91a64-b947-4560-0000-000000000064 ord=0 subject=sound:3da91a64-b947-4560-0000-000000000003\n"
              "  slot=outputBus outcome=resolved level=objectOverride "
              "value=11223344-5566-4788-0000-000100000001 src=3da91a64-b947-4560-0000-000000000003 depth=0 pw=0 ow=0\n"
              "    cand level=objectOverride src=3da91a64-b947-4560-0000-000000000003 depth=0 value=11223344-5566-4788-0000-000100000001 "
              "win=1\n"
              "  slot=attenuation outcome=resolved level=objectOverride "
              "value=88776655-4433-4211-0000-000100000002 src=3da91a64-b947-4560-0000-000000000003 depth=0 pw=0 ow=0\n"
              "    cand level=objectOverride src=3da91a64-b947-4560-0000-000000000003 depth=0 value=88776655-4433-4211-0000-000100000002 "
              "win=1\n");
}

TEST(ProjectSchemaReferenceResolution,
     OccurrenceOrdinalUsesNormalizedClipOrder) {
    ProjectInput in = LayeredWorld();
    InputClip authoredFirst = fixtures::Clip(101, 3, 0, 20);
    authoredFirst.guid = harkentest::StudioObjectId(101);
    InputClip authoredSecond = fixtures::Clip(100, 3, 0, 10);
    authoredSecond.stableOccurrenceOrdinal = 1;
    authoredSecond.guid = harkentest::StudioObjectId(100);
    in.nodes[3].clips = {authoredFirst, authoredSecond};

    const ReferenceResolutionResult result = Resolve(in);
    ASSERT_TRUE(result.ok()) << RenderDiagnostics(result.diagnostics);
    ASSERT_TRUE(result.facts.has_value());
    ASSERT_EQ(result.facts->occurrences.size(), 2u);
    EXPECT_EQ(result.facts->occurrences[0].clipId, harkentest::StudioObjectId(100u));
    EXPECT_EQ(result.facts->occurrences[0].occurrenceOrdinal, 0u);
    EXPECT_EQ(result.facts->occurrences[1].clipId, harkentest::StudioObjectId(101u));
    EXPECT_EQ(result.facts->occurrences[1].occurrenceOrdinal, 1u);
}

// A clip on a Track owned by a Piece sees the Piece as the second placement
// level: [Track, Piece]. With the value on the Piece, resolution walks past the
// Track (depth 1, no value) to the Piece (depth 2).
//
//   4 Sound S    referenced by clip 700 on Track 7
//   6 Piece P    root, Absolute; owns Track 7; output bus 111
//   7 Track T    under P; clip 700 -> S
ProjectInput PieceNestedWorld() {
    ProjectInput in;
    InputNode sound = fixtures::Node(ContentKind::Sound, 4);
    sound.guid = harkentest::StudioObjectId(4);
    InputNode piece = fixtures::Node(ContentKind::Piece, 6);
    piece.guid = harkentest::StudioObjectId(6);
    piece.ownedChildren = {G(7)};
    piece.timeSpec = fixtures::AbsoluteTime();
    piece.startPoint = fixtures::EntryStart();
    InputNode track = fixtures::Node(ContentKind::Track, 7);
    track.guid = harkentest::StudioObjectId(7);
    track.ownedChildren = {G(4)};
    InputClip clip = fixtures::Clip(700, 4);
    clip.guid = harkentest::StudioObjectId(700);
    track.clips = {clip};
    in.nodes = {sound, piece, track};
    in.roots = {G(6)};
    return in;
}

TEST(ProjectSchemaReferenceResolution, OwningPieceDoesNotSupplyThePlacedDefinitionReference) {
    ProjectInput in = PieceNestedWorld();
    in.nodes[1].mainOutputBusRef = harkentest::StudioObjectId(111);
    const auto result = Resolve(in);
    ASSERT_TRUE(result.ok());
    const auto* occurrence = FindOccurrence(*result.facts, harkentest::StudioObjectId(700));
    ASSERT_NE(occurrence, nullptr);
    EXPECT_EQ(occurrence->resolvedSlots[kOutputBus].outcome, ResolutionOutcome::Unresolved);
    EXPECT_EQ(occurrence->resolvedSlots[kOutputBus].placementDepthWalked, 0u);
}

// The referenced object's own override wins whether the object is triggered
// directly (empty placement chain) or placed on a timeline whose ancestors hold
// conflicting values. Both paths resolve to the same bus.
TEST(ProjectSchemaReferenceResolution, OverrideResolvesAlikeDirectOrPlaced) {
    ProjectInput in = PieceNestedWorld();
    in.nodes[0].mainOutputBusRef = harkentest::StudioObjectId(100);  // Sound overrides its own bus
    in.nodes[1].mainOutputBusRef = harkentest::StudioObjectId(111);  // Piece holds a conflicting value

    const FrontEndResult front = CompileFrontEnd(in);
    ASSERT_TRUE(front.ok()) << RenderDiagnostics(front.diagnostics);
    ReferenceResolutionContext context(*front.project);

    const auto placed = context.Resolve(harkentest::StudioObjectId(4), /*placementChain=*/{harkentest::StudioObjectId(7), harkentest::StudioObjectId(6)});
    const auto direct = context.Resolve(harkentest::StudioObjectId(4), /*placementChain=*/{});

    EXPECT_EQ(placed[kOutputBus].outcome, ResolutionOutcome::Resolved);
    EXPECT_EQ(placed[kOutputBus].level, ResolutionLevel::ObjectOverride);
    EXPECT_EQ(placed[kOutputBus].value, harkentest::StudioObjectId(100u));
    EXPECT_EQ(direct[kOutputBus].level, ResolutionLevel::ObjectOverride);
    EXPECT_EQ(direct[kOutputBus].value, harkentest::StudioObjectId(100u));
    EXPECT_EQ(placed[kOutputBus].value, direct[kOutputBus].value);
}

TEST(ProjectSchemaReferenceResolution, DirectResolutionUsesOnlyTheDefinitionSnapshot) {
    ProjectInput in = LayeredWorld();
    in.nodes[0].mainOutputBusRef = harkentest::StudioObjectId(120);
    const auto front = CompileFrontEnd(in);
    ASSERT_TRUE(front.ok());
    const ReferenceResolutionContext context(*front.project);
    const auto direct = context.Resolve(harkentest::StudioObjectId(3), {});
    EXPECT_EQ(direct[kOutputBus].outcome, ResolutionOutcome::Unresolved);
    EXPECT_TRUE(direct[kOutputBus].candidates.empty());
    EXPECT_EQ(direct[kOutputBus].organizationalDepthWalked, 0u);
}

// A clip pointing at a Container resolves the Container itself as the subject;
// this stage does not drill into the Container's descendants.
//
TEST(ProjectSchemaReferenceResolution, ContainerSubjectResolvesItselfNoDrillDown) {
    ProjectInput in;
    InputNode sound = fixtures::Node(ContentKind::Sound, 1);
    sound.guid = harkentest::StudioObjectId(1);
    InputNode container = fixtures::Node(ContentKind::Container, 2);
    container.guid = harkentest::StudioObjectId(2);
    container.ownedChildren = {G(1)};
    container.mainOutputBusRef = harkentest::StudioObjectId(200);
    InputNode track = fixtures::Node(ContentKind::Track, 3);
    track.guid = harkentest::StudioObjectId(3);
    track.ownedChildren = {G(2)};
    InputClip clip = fixtures::Clip(300, 2);  // -> Container
    clip.guid = harkentest::StudioObjectId(300);
    track.clips = {clip};
    in.nodes = {sound, container, track};
    in.roots = {G(3)};

    const ReferenceResolutionResult result = Resolve(in);
    ASSERT_TRUE(result.ok()) << RenderDiagnostics(result.diagnostics);
    ASSERT_TRUE(result.facts.has_value());
    // Exactly one occurrence: the clip -> Container. No row drills into Sound 1.
    ASSERT_EQ(result.facts->occurrences.size(), 1u);
    const OccurrenceReferenceResolution& occ = result.facts->occurrences.front();
    EXPECT_EQ(occ.clipId, harkentest::StudioObjectId(300u));
    EXPECT_EQ(occ.subjectId, harkentest::StudioObjectId(2u));
    EXPECT_EQ(occ.subjectKind, ContentKind::Container);
    EXPECT_EQ(occ.resolvedSlots[kOutputBus].level, ResolutionLevel::ObjectOverride);
    EXPECT_EQ(occ.resolvedSlots[kOutputBus].value, harkentest::StudioObjectId(200u));
}

TEST(ProjectSchemaReferenceResolution, DefinitionSnapshotsDoNotWalkPastAnyDepthCeiling) {
    ProjectInput in = LayeredWorld();
    in.nodes[0].mainOutputBusRef = harkentest::StudioObjectId(120);
    in.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(130);
    const auto front = CompileFrontEnd(in);
    ASSERT_TRUE(front.ok());
    const auto result = BuildReferenceResolution(*front.project, *front.facts, 0);
    ASSERT_TRUE(result.ok());
    ASSERT_TRUE(result.facts.has_value());
    EXPECT_EQ(FindOccurrence(*result.facts, harkentest::StudioObjectId(100))->resolvedSlots[kOutputBus].value, harkentest::StudioObjectId(130u));
}

ProjectInput TwoOccurrenceCeilingWorld() {
    ProjectInput in;
    InputNode firstSound = fixtures::Node(ContentKind::Sound, 1);
    firstSound.guid = harkentest::StudioObjectId(1);
    firstSound.mainOutputBusRef = harkentest::StudioObjectId(501);
    InputNode secondSound = fixtures::Node(ContentKind::Sound, 2);
    secondSound.guid = harkentest::StudioObjectId(2);
    InputNode standaloneTrack = fixtures::Node(ContentKind::Track, 3);
    standaloneTrack.guid = harkentest::StudioObjectId(3);
    standaloneTrack.ownedChildren = {G(1)};
    InputClip firstClip = fixtures::Clip(100, 1);
    firstClip.guid = harkentest::StudioObjectId(100);
    standaloneTrack.clips = {firstClip};
    InputNode piece = fixtures::Node(ContentKind::Piece, 4);
    piece.guid = harkentest::StudioObjectId(4);
    piece.ownedChildren = {G(5)};
    piece.timeSpec = fixtures::AbsoluteTime();
    piece.startPoint = fixtures::EntryStart();
    InputNode pieceTrack = fixtures::Node(ContentKind::Track, 5);
    pieceTrack.guid = harkentest::StudioObjectId(5);
    pieceTrack.ownedChildren = {G(2)};
    InputClip secondClip = fixtures::Clip(101, 2);
    secondClip.guid = harkentest::StudioObjectId(101);
    pieceTrack.clips = {secondClip};
    in.nodes = {firstSound, secondSound, standaloneTrack, piece, pieceTrack};
    in.roots = {G(3), G(4)};
    return in;
}

TEST(ProjectSchemaReferenceResolution, PlacementDepthCannotRefuseOtherDefinitionSnapshots) {
    const auto front = CompileFrontEnd(TwoOccurrenceCeilingWorld());
    ASSERT_TRUE(front.ok());
    const auto result = BuildReferenceResolution(*front.project, *front.facts, 0);
    ASSERT_TRUE(result.ok());
    ASSERT_EQ(result.facts->occurrences.size(), 2u);
    EXPECT_EQ(FindOccurrence(*result.facts, harkentest::StudioObjectId(100))->resolvedSlots[kOutputBus].value, harkentest::StudioObjectId(501u));
    EXPECT_EQ(FindOccurrence(*result.facts, harkentest::StudioObjectId(101))->resolvedSlots[kOutputBus].outcome,
              ResolutionOutcome::Unresolved);
}

TEST(ProjectSchemaReferenceResolution, FrontEndPublishesDefinitionSnapshotsWithoutAncestorWalking) {
    const auto result = CompileFrontEnd(TwoOccurrenceCeilingWorld(), 0);
    ASSERT_TRUE(result.ok()) << RenderDiagnostics(result.diagnostics);
    ASSERT_TRUE(result.project.has_value());
    ASSERT_TRUE(result.facts.has_value());
    ASSERT_TRUE(result.resolution.has_value());
    ASSERT_EQ(result.resolution->occurrences.size(), 2u);
}

// The rendered resolution is byte-identical on repeat.
TEST(ProjectSchemaReferenceResolution, SerializationIsDeterministic) {
    ProjectInput in = LayeredWorld();
    in.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(100);
    in.nodes[3].mainOutputBusRef = harkentest::StudioObjectId(110);
    in.nodes[0].mainOutputBusRef = harkentest::StudioObjectId(120);

    const ReferenceResolutionResult a = Resolve(in);
    const ReferenceResolutionResult b = Resolve(in);
    ASSERT_TRUE(a.facts.has_value());
    ASSERT_TRUE(b.facts.has_value());
    EXPECT_EQ(RenderReferenceResolution(*a.facts),
              RenderReferenceResolution(*b.facts));
}

// The rendered resolution pins the occurrence line, the per-slot winners and the
// full candidate explain for a small three-level project.
TEST(ProjectSchemaReferenceResolution, SerializationPinsResolution) {
    ProjectInput in = LayeredWorld();
    in.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(100);  // Sound override
    in.nodes[3].mainOutputBusRef = harkentest::StudioObjectId(110);  // Track placement
    in.nodes[0].mainOutputBusRef = harkentest::StudioObjectId(120);  // Layer organizational

    const ReferenceResolutionResult result = Resolve(in);
    ASSERT_TRUE(result.ok()) << RenderDiagnostics(result.diagnostics);
    EXPECT_EQ(RenderReferenceResolution(*result.facts),
              "resolution 1\n"
              "occ track=3da91a64-b947-4560-0000-000000000004 clip=3da91a64-b947-4560-0000-000000000064 ord=0 subject=sound:3da91a64-b947-4560-0000-000000000003\n"
              "  slot=outputBus outcome=resolved level=objectOverride value=3da91a64-b947-4560-0000-000000000064 "
              "src=3da91a64-b947-4560-0000-000000000003 depth=0 pw=0 ow=0\n"
              "    cand level=objectOverride src=3da91a64-b947-4560-0000-000000000003 depth=0 value=3da91a64-b947-4560-0000-000000000064 win=1\n"
              "  slot=attenuation outcome=unresolved pw=0 ow=0\n");
}

TEST(ProjectSchemaReferenceResolution, SerializationPinsDefinitionAttenuation) {
    ProjectInput in = LayeredWorld();
    in.nodes[2].spatial.attenuationRef = harkentest::StudioObjectId(210);
    in.nodes[2].presence.Set(harkenplayableproperties::PlayablePropertyIdV1::SpatialAttenuationRef);
    in.nodes[0].spatial.attenuationRef = harkentest::StudioObjectId(220);
    in.nodes[0].presence.Set(harkenplayableproperties::PlayablePropertyIdV1::SpatialAttenuationRef);
    const auto result = Resolve(in);
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(RenderReferenceResolution(*result.facts),
        "resolution 1\n"
        "occ track=3da91a64-b947-4560-0000-000000000004 clip=3da91a64-b947-4560-0000-000000000064 ord=0 subject=sound:3da91a64-b947-4560-0000-000000000003\n"
        "  slot=outputBus outcome=unresolved pw=0 ow=0\n"
        "  slot=attenuation outcome=resolved level=objectOverride value=3da91a64-b947-4560-0000-0000000000d2 src=3da91a64-b947-4560-0000-000000000003 depth=0 pw=0 ow=0\n"
        "    cand level=objectOverride src=3da91a64-b947-4560-0000-000000000003 depth=0 value=3da91a64-b947-4560-0000-0000000000d2 win=1\n");
}



}  // namespace
}  // namespace harkenprojectschema
