// Coverage for the common content-object chassis: the scalar mixing, playback
// and 3D fields plus the send list that all five content kinds share. The
// front-end stores these verbatim — the same "store, don't interpret" discipline
// the time values follow — and validates exactly one new rule: a send may not
// target the reserved bus id 0. These tests pin the contract defaults, per-kind
// passthrough (Layer included), deterministic serialization of the new fields,
// and that one refusal with its deterministic ordering.

#include "UnitTest/StudioObjectIdTestSupport.h"
#include "UnitTest/ProjectSchemaFixtures.h"

#include "harken_project_schema/BuildPieceIR.h"
#include "harken_project_schema/BuildPieceProgram.h"
#include "harken_project_schema/FrontEnd.h"
#include "harken_project_schema/Model.h"
#include "harken_project_schema/ResourceInput.h"
#include "harken_project_schema/Serialize.h"

#include <gtest/gtest.h>

#include <array>
#include <bit>
#include <cstdint>
#include <optional>
#include <string>

namespace harkenprojectschema {
namespace {

using fixtures::G;

const ContentObject* FindByGuid(const CanonicalProject& project,
                                const AuthoringGuid& guid) {
    for (const ContentObject& obj : project.objects) {
        if (obj.chassis().guid == guid) {
            return &obj;
        }
    }
    return nullptr;
}

ProjectInput SingleNodeProject(ContentKind kind) {
    ProjectInput input;
    InputNode node = fixtures::Node(kind, 1);
    if (kind == ContentKind::Piece) {
        node.timeSpec = fixtures::AbsoluteTime();
        node.startPoint = fixtures::EntryStart();
    }
    input.nodes = {node};
    input.roots = {G(1)};
    return input;
}

FrontEndResult CompileSingleSend(const Send& send) {
    ProjectInput input = SingleNodeProject(ContentKind::Sound);
    input.nodes.front().sends = {send};
    return CompileFrontEnd(input);
}

ResourceInput ChassisTestResource() {
    ResourceInput input;
    input.facts.valid = true;
    input.facts.sampleRate = 48000;
    input.facts.channelCount = 2;
    input.facts.frameCount = 48000;
    input.seekable = true;
    return input;
}

ResourceTable ChassisTestResources(const CanonicalProject& project) {
    ResourceTable table;
    for (const ContentObject& obj : project.objects) {
        if (obj.kind == ContentKind::Sound) {
            table.emplace(obj.chassis().id, ChassisTestResource());
        }
    }
    return table;
}

std::optional<PieceProgram> BuildChassisTestProgram(const ProjectInput& input) {
    const FrontEndResult front = CompileFrontEnd(input);
    if (!front.ok() || !front.project.has_value() || !front.facts.has_value() ||
        !front.resolution.has_value() || !front.sends.has_value() ||
        !front.effectChains.has_value()) {
        return std::nullopt;
    }
    // Thread the routing and effect facts so the resolved output bus, merged sends
    // and selected chain reach the occurrences and the semantic fingerprint, the
    // way a shipping program build does.
    const std::optional<PieceIR> ir = BuildPieceIR(
        *front.project, *front.facts, ChassisTestResources(*front.project),
        *front.resolution, *front.sends, *front.effectChains);
    if (!ir.has_value()) {
        return std::nullopt;
    }
    return BuildPieceProgram(*ir);
}

// A default-constructed chassis carries the contract defaults: gains at zero,
// loop off, streaming mode 0, zero output-bus level, an empty send list, and a
// default spatial group (panning centered, 3D off, unit distance scaling, curve
// type 0). The float defaults are exact, so an exact comparison is legitimate.
TEST(ProjectSchemaChassis, DefaultsMatchContract) {
    Chassis c;
    EXPECT_EQ(c.volume, 0.0f);
    EXPECT_EQ(c.pitch, 0.0f);
    EXPECT_EQ(c.makeUpGain, 0.0f);
    EXPECT_EQ(c.loopCount, 1u);
    EXPECT_EQ(c.streamingMode, 0u);
    EXPECT_EQ(c.outputBusVolume, 0.0f);
    EXPECT_TRUE(c.sends.empty());
    EXPECT_EQ(c.spatial.panning, 0.0f);
    EXPECT_EQ(c.spatial.spread, 0.0f);
    EXPECT_FALSE(c.spatial.is3DEnabled);
    EXPECT_EQ(c.spatial.attenuationDistanceScaling, 1.0f);
    EXPECT_EQ(c.mainOutputBusRef, harkentest::StudioObjectId(0u));
    EXPECT_EQ(c.spatial.attenuationRef, harkentest::StudioObjectId(0u));
}

// Two sends are equal exactly when their target bus and level agree.
TEST(ProjectSchemaChassis, SendEquality) {
    EXPECT_TRUE((Send{harkentest::StudioObjectId(7), 0.5f} == Send{harkentest::StudioObjectId(7), 0.5f}));
    EXPECT_FALSE((Send{harkentest::StudioObjectId(7), 0.5f} == Send{harkentest::StudioObjectId(8), 0.5f}));
    EXPECT_FALSE((Send{harkentest::StudioObjectId(7), 0.5f} == Send{harkentest::StudioObjectId(7), 0.25f}));
}

TEST(ProjectSchemaChassis, SendEqualityMatchesSerializationAtBitBoundaries) {
    const Send positiveZero{harkentest::StudioObjectId(7), +0.0f};
    const Send negativeZero{harkentest::StudioObjectId(7), -0.0f};
    const Send firstNan{harkentest::StudioObjectId(7), std::bit_cast<float>(0x7fc00001u)};
    const Send sameNan{harkentest::StudioObjectId(7), std::bit_cast<float>(0x7fc00001u)};
    const Send otherNan{harkentest::StudioObjectId(7), std::bit_cast<float>(0x7fc00002u)};

    EXPECT_FALSE(positiveZero == negativeZero);
    EXPECT_TRUE(firstNan == sameNan);
    EXPECT_FALSE(firstNan == otherNan);

    const FrontEndResult positiveResult = CompileSingleSend(positiveZero);
    const FrontEndResult negativeResult = CompileSingleSend(negativeZero);
    const FrontEndResult firstNanResult = CompileSingleSend(firstNan);
    const FrontEndResult sameNanResult = CompileSingleSend(sameNan);
    const FrontEndResult otherNanResult = CompileSingleSend(otherNan);
    ASSERT_TRUE(positiveResult.ok());
    ASSERT_TRUE(negativeResult.ok());
    ASSERT_TRUE(firstNanResult.ok());
    ASSERT_TRUE(sameNanResult.ok());
    ASSERT_TRUE(otherNanResult.ok());
    ASSERT_TRUE(positiveResult.project.has_value());
    ASSERT_TRUE(negativeResult.project.has_value());
    ASSERT_TRUE(firstNanResult.project.has_value());
    ASSERT_TRUE(sameNanResult.project.has_value());
    ASSERT_TRUE(otherNanResult.project.has_value());

    const std::string positiveRender = RenderProject(*positiveResult.project);
    const std::string negativeRender = RenderProject(*negativeResult.project);
    const std::string firstNanRender = RenderProject(*firstNanResult.project);
    const std::string sameNanRender = RenderProject(*sameNanResult.project);
    const std::string otherNanRender = RenderProject(*otherNanResult.project);

    EXPECT_NE(positiveRender, negativeRender);
    EXPECT_NE(positiveRender.find("  send target=3da91a64-b947-4560-0000-000000000007 level=0\n"),
              std::string::npos);
    EXPECT_NE(negativeRender.find("  send target=3da91a64-b947-4560-0000-000000000007 level=2147483648\n"),
              std::string::npos);
    EXPECT_EQ(firstNanRender, sameNanRender);
    EXPECT_NE(firstNanRender, otherNanRender);
    EXPECT_NE(firstNanRender.find("  send target=3da91a64-b947-4560-0000-000000000007 level=2143289345\n"),
              std::string::npos);
    EXPECT_NE(otherNanRender.find("  send target=3da91a64-b947-4560-0000-000000000007 level=2143289346\n"),
              std::string::npos);
}

// Every one of the five kinds — Layer included — carries the same chassis, and
// the front-end passes every scalar field and the send list through unchanged
// and in authored order.
TEST(ProjectSchemaChassis, ScalarFieldsPassThroughForEveryKind) {
    const FrontEndResult result =
        CompileFrontEnd(fixtures::SharedChassisAllKinds());
    ASSERT_TRUE(result.ok()) << RenderDiagnostics(result.diagnostics);
    ASSERT_TRUE(result.project.has_value());

    for (std::uint64_t guid = 1; guid <= 5; ++guid) {
        const ContentObject* obj = FindByGuid(*result.project, G(guid));
        ASSERT_NE(obj, nullptr) << "missing object guid " << guid;
        const Chassis& c = obj->chassis();
        EXPECT_EQ(c.volume, 1.5f);
        EXPECT_EQ(c.pitch, -2.25f);
        EXPECT_EQ(c.makeUpGain, 0.5f);
        EXPECT_EQ(c.loopCount, 0u);
        EXPECT_EQ(c.streamingMode, 2u);
        EXPECT_EQ(c.outputBusVolume, -6.0f);
        EXPECT_EQ(c.spatial.panning, -0.75f);
        EXPECT_EQ(c.spatial.spread, 0.375f);
        EXPECT_TRUE(c.spatial.is3DEnabled);
        EXPECT_EQ(c.spatial.attenuationDistanceScaling, 2.0f);
        EXPECT_EQ(c.mainOutputBusRef, harkentest::StudioObjectId(4001u));
        EXPECT_EQ(c.spatial.attenuationRef, harkentest::StudioObjectId(4002u));
        ASSERT_EQ(c.sends.size(), 2u);
        EXPECT_TRUE((c.sends[0] == Send{harkentest::StudioObjectId(7), 0.25f}));
        EXPECT_TRUE((c.sends[1] == Send{harkentest::StudioObjectId(9), -0.5f}));
    }
}

// The same populated input compiled twice renders byte-identically, so the new
// fields do not introduce any nondeterministic formatting.
TEST(ProjectSchemaChassis, SerializationWithFieldsIsDeterministic) {
    const std::string a =
        RenderResult(CompileFrontEnd(fixtures::SharedChassisAllKinds()));
    const std::string b =
        RenderResult(CompileFrontEnd(fixtures::SharedChassisAllKinds()));
    EXPECT_EQ(a, b);
}

// The serializer actually emits the send list: a compile with sends renders a
// send line, and one without renders none. Guards against a field silently
// dropped from the rendering.
TEST(ProjectSchemaChassis, SerializationEmitsSends) {
    const std::string withSends =
        RenderResult(CompileFrontEnd(fixtures::SharedChassisAllKinds()));
    EXPECT_NE(withSends.find("\n  send "), std::string::npos);

    const std::string withoutSends =
        RenderResult(CompileFrontEnd(fixtures::Valid()));
    EXPECT_EQ(withoutSends.find("\n  send "), std::string::npos);
}

// A scalar field's value reaches the rendering: two inputs differing only in one
// scalar render differently, so the field is not silently omitted.
TEST(ProjectSchemaChassis, SerializationReflectsScalarValues) {
    ProjectInput lo;
    InputNode loSound = fixtures::Node(ContentKind::Sound, 1);
    loSound.volume = 1.0f;
    lo.nodes = {loSound};
    lo.roots = {G(1)};

    ProjectInput hi;
    InputNode hiSound = fixtures::Node(ContentKind::Sound, 1);
    hiSound.volume = 2.0f;
    hi.nodes = {hiSound};
    hi.roots = {G(1)};

    EXPECT_NE(RenderResult(CompileFrontEnd(lo)),
              RenderResult(CompileFrontEnd(hi)));
}

TEST(ProjectSchemaChassis, SerializationPinsEveryScalarField) {
    ProjectInput input = SingleNodeProject(ContentKind::Sound);
    fixtures::ApplyChassisProbe(&input.nodes.front());
    input.nodes.front().sends.clear();

    const FrontEndResult result = CompileFrontEnd(input);
    ASSERT_TRUE(result.ok()) << RenderDiagnostics(result.diagnostics);
    ASSERT_TRUE(result.project.has_value());
    EXPECT_EQ(RenderProject(*result.project),
              "project\n"
              "object 3da91a64-b947-4560-0000-000000000001 kind=sound guid=4443111527285212512:1 editorOrder=0 volume=1069547520 "
              "pitch=3222274048 makeUpGain=1056964608 loopCount=0 "
              "streamingMode=2 outputBusVolume=3233808384\n"
              "  spatial panning=3208642560 spread=1052770304 is3D=1 "
              "attenScale=1073741824 attenRef=3da91a64-b947-4560-0000-000000000fa2\n"
              "  refs bus=3da91a64-b947-4560-0000-000000000fa1\n"
              "roots: 3da91a64-b947-4560-0000-000000000001\n");
}

TEST(ProjectSchemaChassis, InputDefaultsPassThroughToCanonicalChassis) {
    InputNode node;
    node.guid = G(1);
    ProjectInput input;
    input.nodes = {node};
    input.roots = {G(1)};
    const InputNode& authored = input.nodes.front();
    EXPECT_EQ(authored.volume, 0.0f);
    EXPECT_EQ(authored.pitch, 0.0f);
    EXPECT_EQ(authored.makeUpGain, 0.0f);
    EXPECT_EQ(authored.loopCount, 1u);
    EXPECT_EQ(authored.streamingMode, 0u);
    EXPECT_EQ(authored.outputBusVolume, 0.0f);
    EXPECT_TRUE(authored.sends.empty());
    EXPECT_EQ(authored.mainOutputBusRef, harkentest::StudioObjectId(0u));
    EXPECT_EQ(authored.spatial.panning, 0.0f);
    EXPECT_EQ(authored.spatial.spread, 0.0f);
    EXPECT_FALSE(authored.spatial.is3DEnabled);
    EXPECT_EQ(authored.spatial.attenuationDistanceScaling, 1.0f);
    EXPECT_EQ(authored.spatial.attenuationRef, harkentest::StudioObjectId(0u));

    const FrontEndResult result = CompileFrontEnd(input);
    ASSERT_TRUE(result.ok()) << RenderDiagnostics(result.diagnostics);
    ASSERT_TRUE(result.project.has_value());
    const ContentObject* object = FindByGuid(*result.project, G(1));
    ASSERT_NE(object, nullptr);
    const Chassis& canonical = object->chassis();
    EXPECT_EQ(canonical.volume, authored.volume);
    EXPECT_EQ(canonical.pitch, authored.pitch);
    EXPECT_EQ(canonical.makeUpGain, authored.makeUpGain);
    EXPECT_EQ(canonical.loopCount, authored.loopCount);
    EXPECT_EQ(canonical.streamingMode, authored.streamingMode);
    EXPECT_EQ(canonical.outputBusVolume, authored.outputBusVolume);
    EXPECT_EQ(canonical.sends, authored.sends);
    EXPECT_EQ(canonical.mainOutputBusRef, authored.mainOutputBusRef);
    EXPECT_EQ(canonical.spatial.panning, authored.spatial.panning);
    EXPECT_EQ(canonical.spatial.spread, authored.spatial.spread);
    EXPECT_EQ(canonical.spatial.is3DEnabled, authored.spatial.is3DEnabled);
    EXPECT_EQ(canonical.spatial.attenuationDistanceScaling,
              authored.spatial.attenuationDistanceScaling);
    EXPECT_TRUE(canonical.spatial == authored.spatial);
}

// A send at the reserved bus id 0 is refused with a single diagnostic and no
// partial output; a valid send (proven by the passthrough fixtures) is not.
TEST(ProjectSchemaChassis, SendUnsetTargetBusRefused) {
    const FrontEndResult result =
        CompileFrontEnd(fixtures::InvalidSendUnsetTargetBus());
    EXPECT_FALSE(result.ok());
    EXPECT_FALSE(result.project.has_value());
    EXPECT_FALSE(result.facts.has_value());
    ASSERT_EQ(result.diagnostics.size(), 1u)
        << RenderDiagnostics(result.diagnostics);
    EXPECT_EQ(result.diagnostics.front().code,
              DiagnosticCode::SendTargetBusUnset)
        << "got " << DiagnosticCodeName(result.diagnostics.front().code);
}

TEST(ProjectSchemaChassis, SendUnsetTargetBusRefusedForEveryKind) {
    constexpr std::array<ContentKind, 5> kinds = {
        ContentKind::Layer, ContentKind::Sound, ContentKind::Container,
        ContentKind::Track, ContentKind::Piece};
    for (const ContentKind kind : kinds) {
        SCOPED_TRACE(static_cast<std::uint32_t>(kind));
        ProjectInput input = SingleNodeProject(kind);
        input.nodes.front().sends = {Send{kReservedBusId, 0.5f}};

        const FrontEndResult result = CompileFrontEnd(input);
        EXPECT_FALSE(result.ok());
        EXPECT_FALSE(result.project.has_value());
        EXPECT_FALSE(result.facts.has_value());
        ASSERT_EQ(result.diagnostics.size(), 1u)
            << RenderDiagnostics(result.diagnostics);
        EXPECT_EQ(result.diagnostics.front().code,
                  DiagnosticCode::SendTargetBusUnset);
        EXPECT_EQ(static_cast<std::uint32_t>(result.diagnostics.front().code),
                  118u);
        EXPECT_EQ(result.diagnostics.front().primary, G(1));
        EXPECT_EQ(result.diagnostics.front().detail, 0u);
    }
}

// Two unset sends on one node produce two refusals, ordered by the send index
// carried in the diagnostic detail, and the rendering is deterministic.
TEST(ProjectSchemaChassis, MultipleUnsetSendsDeterministicByIndex) {
    const FrontEndResult result =
        CompileFrontEnd(fixtures::InvalidTwoUnsetSends());
    ASSERT_EQ(result.diagnostics.size(), 2u)
        << RenderDiagnostics(result.diagnostics);
    EXPECT_EQ(result.diagnostics[0].code, DiagnosticCode::SendTargetBusUnset);
    EXPECT_EQ(result.diagnostics[1].code, DiagnosticCode::SendTargetBusUnset);
    EXPECT_EQ(result.diagnostics[0].detail, 0u);
    EXPECT_EQ(result.diagnostics[1].detail, 1u);

    const std::string a =
        RenderResult(CompileFrontEnd(fixtures::InvalidTwoUnsetSends()));
    const std::string b =
        RenderResult(CompileFrontEnd(fixtures::InvalidTwoUnsetSends()));
    EXPECT_EQ(a, b);
}

// Chassis fields outside the folded values, routing facts and spatial group
// include editorOrder and streamingMode. They
// are carried into no part of the semantic fingerprint, so changing them leaves
// both fingerprints untouched. Folded gain, pitch and loop do move the hash.
TEST(ProjectSchemaChassis, NonFoldedChassisFieldsDoNotChangeProgramFingerprints) {
    const ProjectInput defaults = fixtures::IrLineageMix();
    ProjectInput populated = defaults;
    for (InputNode& node : populated.nodes) {
        node.editorOrder = 7;
        node.streamingMode = 2;
    }
    // sends, mainOutputBusRef and the spatial group are deliberately not touched
    // here: they are routing and spatial facts that now enter the semantic
    // fingerprint, so their effects are tested separately.

    const std::optional<PieceProgram> defaultProgram =
        BuildChassisTestProgram(defaults);
    const std::optional<PieceProgram> populatedProgram =
        BuildChassisTestProgram(populated);
    ASSERT_TRUE(defaultProgram.has_value());
    ASSERT_TRUE(populatedProgram.has_value());
    EXPECT_EQ(defaultProgram->programSemanticFingerprint(),
              populatedProgram->programSemanticFingerprint());
    EXPECT_EQ(defaultProgram->runtimeLineageFingerprint(),
              populatedProgram->runtimeLineageFingerprint());
}

// Gain, pitch and loop fold into the audible base values a voice starts with, so
// a change to any of them changes the rendered sound and moves the semantic
// fingerprint — while leaving the lineage fingerprint, which is not audible,
// untouched.
TEST(ProjectSchemaChassis, FoldedAudibleValuesChangeProgramFingerprint) {
    const ProjectInput defaults = fixtures::IrLineageMix();
    const std::optional<PieceProgram> base = BuildChassisTestProgram(defaults);
    ASSERT_TRUE(base.has_value());

    ProjectInput louder = defaults;
    for (InputNode& node : louder.nodes) {
        node.volume = 3.0f;
    }
    const std::optional<PieceProgram> louderProgram =
        BuildChassisTestProgram(louder);
    ASSERT_TRUE(louderProgram.has_value());
    EXPECT_NE(base->programSemanticFingerprint(),
              louderProgram->programSemanticFingerprint());
    EXPECT_EQ(base->runtimeLineageFingerprint(),
              louderProgram->runtimeLineageFingerprint());

    ProjectInput oneDb = defaults;
    oneDb.nodes.front().volume = 1.0f;
    ProjectInput twoDb = defaults;
    twoDb.nodes.front().volume = 2.0f;
    const std::optional<PieceProgram> oneDbProgram =
        BuildChassisTestProgram(oneDb);
    const std::optional<PieceProgram> twoDbProgram =
        BuildChassisTestProgram(twoDb);
    ASSERT_TRUE(oneDbProgram.has_value());
    ASSERT_TRUE(twoDbProgram.has_value());
    EXPECT_NE(oneDbProgram->programSemanticFingerprint(),
              twoDbProgram->programSemanticFingerprint());
    EXPECT_EQ(oneDbProgram->runtimeLineageFingerprint(),
              twoDbProgram->runtimeLineageFingerprint());

    ProjectInput pitched = defaults;
    for (InputNode& node : pitched.nodes) {
        node.pitch = 5.0f;
    }
    const std::optional<PieceProgram> pitchedProgram =
        BuildChassisTestProgram(pitched);
    ASSERT_TRUE(pitchedProgram.has_value());
    EXPECT_NE(base->programSemanticFingerprint(),
              pitchedProgram->programSemanticFingerprint());
    EXPECT_EQ(base->runtimeLineageFingerprint(),
              pitchedProgram->runtimeLineageFingerprint());

    ProjectInput looped = defaults;
    for (InputNode& node : looped.nodes) {
        node.loopCount = 0;
    }
    const std::optional<PieceProgram> loopedProgram =
        BuildChassisTestProgram(looped);
    ASSERT_TRUE(loopedProgram.has_value());
    EXPECT_NE(base->programSemanticFingerprint(),
              loopedProgram->programSemanticFingerprint());
    EXPECT_EQ(base->runtimeLineageFingerprint(),
              loopedProgram->runtimeLineageFingerprint());
}

// The routing facts — a merged send and a resolved output bus — are program
// semantics, so a change to either moves the semantic fingerprint while leaving
// the lineage fingerprint untouched. This is the counterpart to the non-folded
// test above, which no longer covers sends or the output bus.
TEST(ProjectSchemaChassis, RoutingFactsChangeProgramFingerprint) {
    const ProjectInput defaults = fixtures::IrLineageMix();
    const std::optional<PieceProgram> base = BuildChassisTestProgram(defaults);
    ASSERT_TRUE(base.has_value());

    ProjectInput withSend = defaults;
    for (InputNode& node : withSend.nodes) {
        node.sends = {Send{harkentest::StudioObjectId(5), 0.5f}};
    }
    const std::optional<PieceProgram> withSendProgram =
        BuildChassisTestProgram(withSend);
    ASSERT_TRUE(withSendProgram.has_value());
    EXPECT_NE(base->programSemanticFingerprint(),
              withSendProgram->programSemanticFingerprint());
    EXPECT_EQ(base->runtimeLineageFingerprint(),
              withSendProgram->runtimeLineageFingerprint());

    ProjectInput withBus = defaults;
    for (InputNode& node : withBus.nodes) {
        node.mainOutputBusRef = harkentest::StudioObjectId(9);
    }
    const std::optional<PieceProgram> withBusProgram =
        BuildChassisTestProgram(withBus);
    ASSERT_TRUE(withBusProgram.has_value());
    EXPECT_NE(base->programSemanticFingerprint(),
              withBusProgram->programSemanticFingerprint());
    EXPECT_EQ(base->runtimeLineageFingerprint(),
              withBusProgram->runtimeLineageFingerprint());

    // The send level, not merely the presence of a send, is in the hash.
    ProjectInput louderSend = defaults;
    for (InputNode& node : louderSend.nodes) {
        node.sends = {Send{harkentest::StudioObjectId(5), 0.75f}};
    }
    const std::optional<PieceProgram> louderSendProgram =
        BuildChassisTestProgram(louderSend);
    ASSERT_TRUE(louderSendProgram.has_value());
    EXPECT_NE(withSendProgram->programSemanticFingerprint(),
              louderSendProgram->programSemanticFingerprint());
}

TEST(ProjectSchemaChassis, SpatialFactsChangeProgramFingerprint) {
    const ProjectInput defaults = fixtures::IrLineageMix();
    const std::optional<PieceProgram> base = BuildChassisTestProgram(defaults);
    ASSERT_TRUE(base.has_value());

    struct Mutation {
        const char* label;
        void (*apply)(InputNode&);
    };
    const Mutation mutations[] = {
        {"panning", [](InputNode& n) { n.spatial.panning = 0.5f; }},
        {"spread", [](InputNode& n) { n.spatial.spread = 0.25f; }},
        {"is3D", [](InputNode& n) { n.spatial.is3DEnabled = true; }},
        {"distanceScaling",
         [](InputNode& n) { n.spatial.attenuationDistanceScaling = 2.0f; }},
        {"attenuationRef",
         [](InputNode& n) {
             n.spatial.attenuationRef = harkentest::StudioObjectId(77);
             n.presence.Set(harkenplayableproperties::PlayablePropertyIdV1::SpatialAttenuationRef);
         }},
    };
    for (const Mutation& mutation : mutations) {
        ProjectInput mutated = defaults;
        for (InputNode& node : mutated.nodes) {
            mutation.apply(node);
        }
        const std::optional<PieceProgram> mutatedProgram =
            BuildChassisTestProgram(mutated);
        ASSERT_TRUE(mutatedProgram.has_value()) << mutation.label;
        EXPECT_NE(base->programSemanticFingerprint(),
                  mutatedProgram->programSemanticFingerprint())
            << mutation.label;
        EXPECT_EQ(base->runtimeLineageFingerprint(),
                  mutatedProgram->runtimeLineageFingerprint())
            << mutation.label;
    }
}

// This golden freezes the semantic hash field layout. Updating it must be an
// intentional contract change.
TEST(ProjectSchemaChassis, SemanticFingerprintGoldenLayout) {
    ProjectInput input;
    InputNode sound = fixtures::Node(ContentKind::Sound, 1);
    sound.guid = harkentest::StudioObjectId(1);
    sound.volume = 2.0f;
    sound.pitch = 400.0f;
    sound.loopCount = 0;

    InputNode track = fixtures::Node(ContentKind::Track, 2);
    track.guid = harkentest::StudioObjectId(2);
    track.volume = 2.0f;
    track.pitch = 800.0f;
    track.ownedChildren = {G(1)};
    InputClip clip = fixtures::Clip(3, 1, 8);
    clip.guid = harkentest::StudioObjectId(3);
    clip.semanticOrderKey = 16;
    track.clips = {clip};

    input.nodes = {sound, track};
    input.roots = {G(2)};

    const std::optional<PieceProgram> program =
        BuildChassisTestProgram(input);
    ASSERT_TRUE(program.has_value());
    // This value freezes the occurrence hash layout, including the spatial
    // facts. Moving it requires an intentional fingerprint contract change.
    constexpr std::uint64_t kExpectedSemanticFingerprint =
        4462142723514770557ull;
    EXPECT_EQ(program->programSemanticFingerprint(),
              kExpectedSemanticFingerprint);
}

}  // namespace
}  // namespace harkenprojectschema
