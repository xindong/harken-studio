#include "UnitTest/StudioObjectIdTestSupport.h"
#include "harken_current_world_compiler/CurrentWorldCompiler.h"

#include "Common/TestUtil.h"
#include "UnitTest/StudioWorldTestSupport.h"

#include "Engine.h"
#include "harken/core/CurrentWorldArtifact.h"
#include "harken/core/CurrentWorldCodec.h"
#include "harken/media/PackFormat.h"
#include "harken/media/PackFormatV2.h"
#include "harken/param/ParamTargetTable.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <bit>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <vector>

namespace {

harkenprojectschema::Chassis ResolvedChassis(std::uint64_t id) {
    harkenprojectschema::Chassis chassis;
    chassis.id = harkentest::StudioObjectId(id);
    chassis.presence.bits = harken::kResolvedSectionGroupPropertyMaskV1;
    chassis.mainOutputBusRef = harkenstudio::kMasterBusObjectId;
    return chassis;
}

harkenprojectschema::ContentObject Layer(std::uint64_t id,
                                        std::vector<std::uint64_t> children) {
    harkenprojectschema::ContentObject object{};
    object.kind = harkenprojectschema::ContentKind::Layer;
    object.layer.chassis = ResolvedChassis(id);
    for (const auto child : children)
        object.layer.ownedChildren.push_back(harkentest::StudioObjectId(child));
    return object;
}

harkenprojectschema::ContentObject Sound(std::uint64_t id) {
    harkenprojectschema::ContentObject object{};
    object.kind = harkenprojectschema::ContentKind::Sound;
    object.sound.chassis = ResolvedChassis(id);
    return object;
}

harkenprojectschema::ContentObject Container(std::uint64_t id,
                                             std::uint64_t childId) {
    harkenprojectschema::ContentObject object{};
    object.kind = harkenprojectschema::ContentKind::Container;
    object.container.chassis = ResolvedChassis(id);
    object.container.ownedChildren.push_back(harkentest::StudioObjectId(childId));
    harkenprojectschema::ContainerChildOccurrence occurrence{};
    occurrence.stableOccurrenceOrdinal = 7;
    occurrence.childPlayableRef.kind = harkenprojectschema::ContentKind::Sound;
    occurrence.childPlayableRef.target = harkentest::StudioObjectId(childId);
    occurrence.presence.Set(
        harkenplayableproperties::PlayablePropertyIdV1::LoopCount);
    occurrence.loopCount = 1;
    object.container.children.push_back(occurrence);
    return object;
}

harkenprojectschema::CanonicalProject Project() {
    harkenprojectschema::CanonicalProject project{};
    project.objects.push_back(Layer(1, {2}));
    project.objects.push_back(Container(2, 3));
    project.objects.push_back(Sound(3));
    project.roots = {harkentest::StudioObjectId(1)};
    return project;
}

std::vector<harkencurrentworldcompiler::DefinitionMaterializationV1>
Materializations() {
    return {
        {harkenplayableproperties::ContentPropertyKindV1::Container, harkentest::StudioObjectId(2), 22, harkentest::StudioObjectId(20),
         200},
        {harkenplayableproperties::ContentPropertyKindV1::Sound, harkentest::StudioObjectId(3), 30, harkentest::StudioObjectId(21),
         201},
    };
}

harkencurrentworldcompiler::ContainerAuthoringV1 Authored(
    harkenstudiobuild::ContainerModeV1 mode, harkenstudiobuild::RandomDisciplineV1 discipline) {
    harkencurrentworldcompiler::ContainerAuthoringV1 authored{};
    authored.containerStableId = harkentest::StudioObjectId(2);
    authored.mode = mode;
    authored.randomDiscipline = discipline;
    harkencurrentworldcompiler::ContainerChildAuthoringV1 child{};
    child.stableOccurrenceOrdinal = 7;
    if (mode == harkenstudiobuild::ContainerModeV1::Random &&
        discipline == harkenstudiobuild::RandomDisciplineV1::Draw) {
        child.frequencyWeight = 4;
    } else if (mode == harkenstudiobuild::ContainerModeV1::Random) {
        child.orderWeight = 4;
    }
    authored.children.push_back(child);
    return authored;
}

harkenstudiobuild::CurveV1 FadeCurve(std::uint32_t localId) {
    harkenstudiobuild::CurveV1 curve{};
    curve.curveLocalId = localId;
    curve.curve.pointCount = 2;
    curve.curve.points[0] = {0.0f, 0.0f};
    curve.curve.points[1] = {1.0f, 1.0f};
    curve.curve.handles[0] = {1.0f / 3.0f, 1.0f / 3.0f,
                              2.0f / 3.0f, 2.0f / 3.0f};
    return curve;
}

void WriteU32(std::vector<std::uint8_t>& bytes, std::size_t offset,
              std::uint32_t value) {
    ASSERT_LE(offset + 4, bytes.size());
    bytes[offset] = static_cast<std::uint8_t>(value);
    bytes[offset + 1] = static_cast<std::uint8_t>(value >> 8);
    bytes[offset + 2] = static_cast<std::uint8_t>(value >> 16);
    bytes[offset + 3] = static_cast<std::uint8_t>(value >> 24);
}

void WriteU64(std::vector<std::uint8_t>& bytes, std::size_t offset,
              std::uint64_t value) {
    WriteU32(bytes, offset, static_cast<std::uint32_t>(value));
    WriteU32(bytes, offset + 4, static_cast<std::uint32_t>(value >> 32));
}

std::uint64_t ReadU64(const std::vector<std::uint8_t>& bytes,
                      std::size_t offset) {
    EXPECT_LE(offset + 8, bytes.size());
    std::uint64_t value = 0;
    for (std::uint32_t i = 0; i < 8; ++i) {
        value |= static_cast<std::uint64_t>(bytes[offset + i]) << (8u * i);
    }
    return value;
}

bool WriteBinary(const std::filesystem::path& path,
                 const std::vector<std::uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary | std::ios::trunc);
    if (!out) {
        return false;
    }
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

struct EntryTimelineFixture {
    harkenprojectschema::CanonicalProject project;
    std::vector<harkencurrentworldcompiler::DefinitionMaterializationV1> materializations;

    EntryTimelineFixture() {
        using namespace harkenprojectschema;
        ContentObject piece{};
        piece.kind = ContentKind::Piece;
        piece.piece.chassis = ResolvedChassis(1);
        piece.piece.tracks = {harkentest::StudioObjectId(2)};
        piece.piece.entryMarker = EntryMarker{harkentest::StudioObjectId(5), {5, 5}, 1000000000};
        piece.piece.markers.push_back({harkentest::StudioObjectId(6), {6, 6}, 1,
                                      MarkerKind::Exit, 2000000000});
        ContentObject track{};
        track.kind = ContentKind::Track;
        track.track.chassis = ResolvedChassis(2);
        Clip clip{};
        clip.id = harkentest::StudioObjectId(4);
        clip.guid = {4, 4};
        clip.contentRef = {ContentKind::Sound, harkentest::StudioObjectId(3)};
        clip.duration = 2500000000;
        track.track.clips.push_back(clip);
        project.objects = {piece, track, Sound(3)};
        project.roots = {harkentest::StudioObjectId(1), harkentest::StudioObjectId(3)};
        materializations = {
            {harkenplayableproperties::ContentPropertyKindV1::Piece, harkentest::StudioObjectId(1), 11, {}, 101},
            {harkenplayableproperties::ContentPropertyKindV1::Track, harkentest::StudioObjectId(2), 22, {}, 202},
            {harkenplayableproperties::ContentPropertyKindV1::Sound, harkentest::StudioObjectId(3), 33, {}, 303}};
    }

    auto Compile(std::uint32_t vintage = 18) const {
        return harkencurrentworldcompiler::CompileCurrentWorldV1(
            project, {}, materializations, {}, {}, {}, {}, nullptr, vintage);
    }
};

TEST(CurrentWorldCompiler, Entry19CrossCodecRoundtripAndShippingGate) {
    EntryTimelineFixture fixture;
    const auto legacy = fixture.Compile();
    EXPECT_EQ(harkencurrentworldcompiler::CompileStatusV1::EntryScheduleCapabilityUnavailable,
              legacy.status);
    EXPECT_EQ(nullptr, legacy.world);
    const auto compiled = fixture.Compile(19);
    ASSERT_TRUE(compiled.ok()) << static_cast<unsigned>(compiled.status);
    const auto& world = compiled.world->View();
    EXPECT_EQ(19u, world.sourceBodyVintage);
    EXPECT_EQ(harkenstudiobuild::WorldInstallStatus::CapabilityUnavailable,
              harkenstudiobuild::ValidateCurrentWorldV1(world));
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(harkenstudiobuild::EncodeCurrentWorldBodyV19(world, bytes));
    harken::CurrentWorldBodyImage runtime;
    ASSERT_TRUE(runtime.Decode(bytes.data(), bytes.size(), 19,
                              harken::CurrentWorldDecodeMode::StrictArtifact));
    const auto& decoded = *runtime.ProductionView();
    ASSERT_EQ(1u, decoded.pieceScheduleCount);
    const auto& piece = decoded.pieceSchedules[0];
    EXPECT_EQ(harkentest::ObjectId(5), piece.entryMarker.stableMarkerId);
    EXPECT_EQ(1000000000, piece.entryMarker.position);
    EXPECT_EQ(0, piece.bounds.contentStart);
    EXPECT_EQ(2500000000, piece.bounds.contentEnd);
    EXPECT_EQ(1000000000, piece.bounds.nominalStart);
    EXPECT_EQ(2000000000, piece.bounds.nominalEnd);
    EXPECT_EQ(harken::ComputePieceScheduleFingerprintV2(piece), piece.scheduleSemanticFingerprint);
    EXPECT_EQ(harken::ComputeCurrentWorldFingerprintV6(decoded),
              world.identity.runtimeSemanticFingerprint);
    EXPECT_EQ(harken::WorldInstallStatus::CapabilityUnavailable,
              harken::ValidateCurrentWorldV1(decoded));
    std::vector<std::uint8_t> runtimeBytes;
    ASSERT_TRUE(harken::EncodeCurrentWorldBodyV19(decoded, runtimeBytes));
    EXPECT_EQ(bytes, runtimeBytes);
    harkenstudiobuild::CurrentWorldBodyImage studio;
    ASSERT_TRUE(studio.Decode(runtimeBytes.data(), runtimeBytes.size(), 19));
    EXPECT_EQ(world.identity.runtimeSemanticFingerprint,
              studio.ProductionView()->identity.runtimeSemanticFingerprint);
    EXPECT_EQ(19u, studio.ProductionView()->sourceBodyVintage);
    EXPECT_FALSE(harken::EncodeCurrentWorldShippingBody(decoded, runtimeBytes));
    EXPECT_FALSE(harken::EncodeCurrentWorldArtifactV1(decoded, runtimeBytes));
    EXPECT_FALSE(harkenstudiobuild::EncodeCurrentWorldShippingBody(world, runtimeBytes));
    EXPECT_FALSE(harkenstudiobuild::EncodeCurrentWorldArtifactV1(world, runtimeBytes));
    harken::CurrentWorldBodyImage wrong;
    EXPECT_FALSE(wrong.Decode(bytes.data(), bytes.size(), 18,
                             harken::CurrentWorldDecodeMode::StrictArtifact));
    EXPECT_FALSE(wrong.Decode(bytes.data(), bytes.size(), 20,
                             harken::CurrentWorldDecodeMode::StrictArtifact));
    EXPECT_FALSE(wrong.Decode(bytes.data(), bytes.size() - 1, 19,
                             harken::CurrentWorldDecodeMode::StrictArtifact));
    EXPECT_FALSE(wrong.Decode(bytes.data(), bytes.size(), 19,
                             harken::CurrentWorldDecodeMode::ReplayCurrentAttempt));
    auto disguised = decoded;
    disguised.sourceBodyVintage = 18;
    EXPECT_FALSE(harken::EncodeCurrentWorldShippingBody(disguised, runtimeBytes));

    harken::CurrentWorldArtifactHeaderV1 header{};
    header.bodyVintage = 19;
    header.bodyBytes = bytes.size();
    header.bodyChecksum = harken::Fnv1a64(bytes.data(), bytes.size());
    header.runtimeSemanticFingerprint = decoded.identity.runtimeSemanticFingerprint;
    header.capabilityMask = decoded.identity.capabilityMask;
    harken::CurrentWorldBodyImage artifactImage;
    EXPECT_EQ(harken::CurrentWorldLoadStatusV1::Ok,
              harken::DecodeCurrentWorldArtifactBodyV1(header, bytes.data(), bytes.size(), artifactImage));
    EXPECT_EQ(19u, artifactImage.ProductionView()->sourceBodyVintage);
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    EXPECT_EQ(harken::WorldInstallStatus::CapabilityUnavailable,
              harken::InstallContainerWorldV1(*session.Engine(), decoded).status);
}

TEST(CurrentWorldCompiler, Entry19ValidatesDerivedContentEvenWithRecomputedHashes) {
    EntryTimelineFixture fixture;
    fixture.project.objects[1].track.clips[0].time = -1000000000;
    fixture.project.objects[1].track.clips[0].duration = 3500000000;
    const auto compiled = fixture.Compile(19);
    ASSERT_TRUE(compiled.ok());
    auto world = compiled.world->View();
    auto piece = world.pieceSchedules[0];
    EXPECT_EQ(-1000000000, piece.bounds.contentStart);
    world.pieceSchedules = &piece;
    --piece.bounds.contentStart;
    piece.scheduleSemanticFingerprint = harkenstudiobuild::ComputePieceScheduleFingerprintV2(piece);
    world.identity.runtimeSemanticFingerprint = harkenstudiobuild::ComputeCurrentWorldFingerprintV6(world);
    EXPECT_EQ(harkenstudiobuild::WorldInstallStatus::InvalidWorld,
              harkenstudiobuild::ValidateCurrentWorldV1(world));
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(harkenstudiobuild::EncodeCurrentWorldBodyV19(world, bytes));
    harken::CurrentWorldBodyImage decoded;
    ASSERT_TRUE(decoded.Decode(bytes.data(), bytes.size(), 19,
                               harken::CurrentWorldDecodeMode::StrictArtifact));
    EXPECT_EQ(harken::WorldInstallStatus::InvalidWorld,
              harken::ValidateCurrentWorldV1(*decoded.ProductionView()));
}

TEST(CurrentWorldCompiler, ActionTimingTablesAreExplicitAndVersioned) {
    constexpr bool legacy[5][10] = {
        {1,1,1,1,0,0,0,0,0,0}, {1,1,1,1,1,1,1,0,0,0},
        {1,1,1,1,0,0,0,0,0,0}, {1,0,0,0,0,0,0,0,0,0}, {1,0,0,0,0,0,0,0,0,0}};
    constexpr bool entry[5][10] = {
        {1,1,1,1,0,0,0,1,1,0}, {1,1,1,1,1,1,1,1,1,1},
        {1,0,0,0,0,0,0,0,0,0}, {1,0,0,0,0,0,0,0,0,0}, {1,0,0,0,0,0,0,0,0,0}};
    for (unsigned op = 0; op != 6; ++op) {
        for (unsigned timing = 0; timing != 11; ++timing) {
            const bool oldExpected = op < 5 && timing < 10 && legacy[op][timing];
            const bool newExpected = op < 5 && timing < 10 && entry[op][timing];
            const auto coreOp = static_cast<harken::ActionOpcodeV1>(op);
            const auto coreTiming = static_cast<harken::ActionQuantizeTimingV1>(timing);
            const auto studioOp = static_cast<harkenstudiobuild::ActionOpcodeV1>(op);
            const auto studioTiming = static_cast<harkenstudiobuild::ActionQuantizeTimingV1>(timing);
            EXPECT_EQ(oldExpected, harken::ActionQuantizeTimingReadableV1(coreOp, coreTiming, 18));
            EXPECT_EQ(newExpected, harken::ActionQuantizeTimingReadableV1(coreOp, coreTiming, 19));
            EXPECT_EQ(oldExpected, harkenstudiobuild::ActionQuantizeTimingReadableV1(studioOp, studioTiming, 18));
            EXPECT_EQ(newExpected, harkenstudiobuild::ActionQuantizeTimingReadableV1(studioOp, studioTiming, 19));
            EXPECT_EQ(oldExpected && (op != 2 || timing == 0),
                      harkenstudiobuild::ActionQuantizeTimingSupportedV1(studioOp, studioTiming, 18));
            EXPECT_FALSE(harken::ActionQuantizeTimingReadableV1(coreOp, coreTiming, 20));
            EXPECT_FALSE(harkenstudiobuild::ActionQuantizeTimingSupportedV1(studioOp, studioTiming, 20));
        }
    }
}

TEST(CurrentWorldCompiler, LegacyPieceAndActionKeepFixedMainWireAndFingerprintGolden) {
    using namespace harkenstudiobuild;
    // Captured by building the PackBuild sources at main f1754691 independently.
    ScheduleMarkerV1 marker{};
    marker.stableMarkerId = {4, 5};
    marker.markerId = 1;
    marker.markerKind = 1;
    marker.position = 300;
    PieceScheduleProgramViewV1 piece{};
    piece.rootDefinition.kind = PlayableKindV1::Piece;
    piece.rootDefinition.stableId = {0, 1};
    piece.rootDefinition.expectedProgramSemanticFingerprint = 11;
    piece.markerCount = 1;
    piece.markers = &marker;
    piece.scheduleSemanticFingerprint = ComputePieceScheduleFingerprintV1(piece);
    ActionStepProgramViewV1 step{};
    step.opcode = ActionOpcodeV1::Stop;
    step.target = {ActionTargetKindV1::Piece, 0, {0, 1}, 11};
    step.delay = step.fade = step.cooldown = {AuthoredTimeDomainV1::AbsoluteNanoseconds, 0, 1};
    ActionProgramViewV1 action{};
    action.actionStableId = {0, 6};
    action.steps = &step;
    action.stepCount = action.expandedStepOccurrenceCount = action.expandedFiringCount = 1;
    action.programSemanticFingerprint = ComputeActionProgramFingerprintV1(action);
    ContainerWorldInstallViewV1 world{};
    world.identity.projectId = {9, 17};
    world.identity.capabilityMask = kWorldCapabilityPlayableProperties | kWorldCapabilityActionDefinitions;
    world.pieceSchedules = &piece;
    world.pieceScheduleCount = 1;
    world.actions = &action;
    world.actionCount = 1;
    world.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV5(world);
    EXPECT_EQ(UINT64_C(0xd355194a1488a8c9), piece.scheduleSemanticFingerprint);
    EXPECT_EQ(UINT64_C(0x57850d8c034b843d), action.programSemanticFingerprint);
    EXPECT_EQ(UINT64_C(0x46a3c886751a3dee), world.identity.runtimeSemanticFingerprint);
    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(EncodeCurrentWorldShippingBody(world, bytes));
    EXPECT_EQ(724u, bytes.size());
    EXPECT_EQ(UINT64_C(0x3c1536121b9a3f9b), harken::Fnv1a64(bytes.data(), bytes.size()));
    harken::CurrentWorldBodyImage decoded;
    ASSERT_TRUE(decoded.Decode(bytes.data(), bytes.size(), 18,
                               harken::CurrentWorldDecodeMode::StrictArtifact));
    EXPECT_EQ(UINT64_C(0xd355194a1488a8c9),
              harken::ComputePieceScheduleFingerprintV1(decoded.ProductionView()->pieceSchedules[0]));
    EXPECT_EQ(UINT64_C(0x46a3c886751a3dee),
              harken::ComputeCurrentWorldFingerprintV5(*decoded.ProductionView()));
    std::vector<std::uint8_t> runtimeBytes;
    ASSERT_TRUE(harken::EncodeCurrentWorldShippingBody(*decoded.ProductionView(), runtimeBytes));
    EXPECT_EQ(bytes, runtimeBytes);
}

TEST(CurrentWorldCompiler, EntryTimingDiagnosticsIdentifyActionAndStep) {
    using namespace harkenstudiobuild;
    using namespace harkencurrentworldcompiler;
    ActionProgramAuthoringV1 action{};
    action.actionStableId = harkentest::StudioObjectId(41);
    ActionStepAuthoringV1 step{};
    step.target.kind = ActionTargetKindV1::Sound;
    step.target.stableId = harkentest::StudioObjectId(3);
    action.steps.push_back(step);
    step.authoredEventIndex = 1;
    action.steps.push_back(step);
    const auto compile = [&](std::uint32_t vintage) {
        return CompileCurrentWorldV1(Project(), {}, Materializations(),
            {Authored(ContainerModeV1::Random, RandomDisciplineV1::Draw)},
            {}, {}, {action}, nullptr, vintage);
    };
    for (const auto opcode : {ActionOpcodeV1::Play, ActionOpcodeV1::Stop}) {
        for (const auto timing : {ActionQuantizeTimingV1::EntryMarker,
                                 ActionQuantizeTimingV1::ReferenceExitMarker,
                                 ActionQuantizeTimingV1::TargetNextMarker}) {
            for (const bool reference : {false, true}) {
                auto& authored = action.steps[1];
                authored.opcode = opcode;
                authored.quantizeTiming = timing;
                authored.quantizeReference = {};
                if (reference) authored.quantizeReference = authored.target;
                const auto shipping = compile(18);
                EXPECT_EQ(CompileStatusV1::EntryScheduleCapabilityUnavailable, shipping.status);
                EXPECT_EQ(action.actionStableId, shipping.relatedStableId);
                EXPECT_EQ(1u, shipping.relatedEventIndex);
                const bool valid = opcode == ActionOpcodeV1::Play
                    ? reference && timing != ActionQuantizeTimingV1::TargetNextMarker
                    : timing == ActionQuantizeTimingV1::EntryMarker ||
                      (timing == ActionQuantizeTimingV1::ReferenceExitMarker && reference) ||
                      (timing == ActionQuantizeTimingV1::TargetNextMarker && !reference);
                const auto future = compile(19);
                ASSERT_EQ(valid, future.ok()) << static_cast<unsigned>(opcode) << ':'
                    << static_cast<unsigned>(timing) << ':' << reference << ':'
                    << static_cast<unsigned>(future.status);
                if (!valid) {
                    EXPECT_EQ(CompileStatusV1::InvalidActionAuthoring, future.status);
                    EXPECT_EQ(action.actionStableId, future.relatedStableId);
                    EXPECT_EQ(1u, future.relatedEventIndex);
                    EXPECT_EQ(nullptr, future.world);
                    continue;
                }
                const auto& world = future.world->View();
                std::vector<std::uint8_t> bytes;
                ASSERT_TRUE(EncodeCurrentWorldBodyV19(world, bytes));
                harken::CurrentWorldBodyImage decoded;
                ASSERT_TRUE(decoded.Decode(bytes.data(), bytes.size(), 19,
                    harken::CurrentWorldDecodeMode::StrictArtifact));
                EXPECT_EQ(harken::WorldInstallStatus::CapabilityUnavailable,
                          harken::ValidateCurrentWorldV1(*decoded.ProductionView()));
                EXPECT_EQ(static_cast<harken::ActionQuantizeTimingV1>(timing),
                          decoded.ProductionView()->actions[0].steps[1].quantizeTiming);
                EXPECT_EQ(reference ? harkentest::ObjectId(3) : VnObjectId{},
                          decoded.ProductionView()->actions[0].steps[1].quantizeReference.stableId);
            }
        }
    }
}

TEST(CurrentWorldCompiler, LegacyQuantizedPostRemainsReadableWithoutEnablingAuthoring) {
    using namespace harkenstudiobuild;
    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(),
        {Authored(ContainerModeV1::Random, RandomDisciplineV1::Draw)});
    ASSERT_TRUE(compiled.ok());
    auto world = compiled.world->View();
    ActionStepProgramViewV1 step{};
    step.opcode = ActionOpcodeV1::PostAction;
    step.target = {ActionTargetKindV1::Action, 0, harkentest::StudioObjectId(42), 0};
    const auto sound = std::find_if(world.playableProperties,
        world.playableProperties + world.playablePropertyCount, [](const auto& property) {
            return property.stableId == harkentest::StudioObjectId(3);
        });
    ASSERT_NE(world.playableProperties + world.playablePropertyCount, sound);
    step.quantizeReference = {ActionTargetKindV1::Sound, 0, sound->stableId,
                              sound->programSemanticFingerprint};
    step.delay = step.fade = step.cooldown = {AuthoredTimeDomainV1::AbsoluteNanoseconds, 0, 1};
    ActionProgramViewV1 actions[2]{};
    actions[0].actionStableId = harkentest::StudioObjectId(41);
    actions[0].steps = &step;
    actions[0].stepCount = 1;
    actions[0].expandedStepOccurrenceCount = 2;
    actions[0].expandedFiringCount = 2;
    auto childStep = step;
    childStep.opcode = ActionOpcodeV1::Stop;
    childStep.target = step.quantizeReference;
    childStep.quantizeReference = {};
    actions[1].actionStableId = harkentest::StudioObjectId(42);
    actions[1].steps = &childStep;
    actions[1].stepCount = actions[1].expandedStepOccurrenceCount = 1;
    actions[1].expandedFiringCount = 1;
    actions[1].programSemanticFingerprint = ComputeActionProgramFingerprintV1(actions[1]);
    step.target.expectedProgramSemanticFingerprint = actions[1].programSemanticFingerprint;
    world.actions = actions;
    world.actionCount = 2;
    world.identity.capabilityMask |= kWorldCapabilityActionDefinitions;
    for (const auto timing : {ActionQuantizeTimingV1::NextBeat,
                             ActionQuantizeTimingV1::NextBar,
                             ActionQuantizeTimingV1::NextMarker}) {
        step.quantizeTiming = timing;
        actions[0].directFeatureMask = actions[0].transitiveFeatureMask = 2;
        actions[0].programSemanticFingerprint = ComputeActionProgramFingerprintV1(actions[0]);
        world.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV5(world);
        ASSERT_EQ(WorldInstallStatus::Ok, ValidateCurrentWorldV1(world));
        EXPECT_FALSE(ActionQuantizeTimingSupportedV1(step.opcode, timing, 18));
        std::vector<std::uint8_t> bytes;
        ASSERT_TRUE(EncodeCurrentWorldShippingBody(world, bytes));
        harken::CurrentWorldBodyImage decoded;
        ASSERT_TRUE(decoded.Decode(bytes.data(), bytes.size(), 18,
                                   harken::CurrentWorldDecodeMode::StrictArtifact));
        EXPECT_EQ(harken::WorldInstallStatus::Ok,
                  harken::ValidateCurrentWorldV1(*decoded.ProductionView()));
        std::vector<std::uint8_t> again;
        ASSERT_TRUE(harken::EncodeCurrentWorldShippingBody(*decoded.ProductionView(), again));
        EXPECT_EQ(bytes, again);
    }
}

TEST(CurrentWorldCompiler, StopControlReferencesRejectKnownNonPlayableIdentities) {
    using namespace harkenstudiobuild;
    ActionStepProgramViewV1 step{};
    step.opcode = ActionOpcodeV1::Stop;
    step.target = {ActionTargetKindV1::Sound, 0, harkentest::StudioObjectId(10), 123};
    step.delay = {AuthoredTimeDomainV1::AbsoluteNanoseconds, 0, 1};
    step.fade = step.cooldown = step.delay;
    ActionProgramViewV1 action{};
    action.actionStableId = harkentest::StudioObjectId(99);
    action.steps = &step;
    action.stepCount = action.expandedStepOccurrenceCount = action.expandedFiringCount = 1;
    VariableDefinitionV1 variable{};
    variable.variableId = harkentest::StudioObjectId(98);
    variable.kind = VariableKindV1::Smooth;
    variable.rangeMax = 1.0f;
    ContainerWorldInstallViewV1 world{};
    world.actions = &action;
    world.actionCount = 1;
    world.variables = &variable;
    world.variableCount = 1;
    world.identity.capabilityMask = kWorldCapabilityActionDefinitions | kWorldCapabilityVariableDefinitions;
    const auto validate = [&] {
        action.programSemanticFingerprint = ComputeActionProgramFingerprintV1(action);
        world.identity.runtimeSemanticFingerprint = ComputeCurrentWorldFingerprintV5(world);
        return ValidateCurrentWorldV1(world);
    };
    ASSERT_EQ(WorldInstallStatus::Ok, validate());
    const auto valid = step;
    for (const auto id : {action.actionStableId, variable.variableId}) {
        step = valid;
        step.target.stableId = id;
        EXPECT_EQ(WorldInstallStatus::InvalidWorld, validate());
        step = valid;
        step.quantizeTiming = ActionQuantizeTimingV1::NextBeat;
        step.quantizeReference = step.target;
        step.quantizeReference.stableId = id;
        action.directFeatureMask = action.transitiveFeatureMask = 2;
        EXPECT_EQ(WorldInstallStatus::InvalidWorld, validate());
        action.directFeatureMask = action.transitiveFeatureMask = 0;
    }
}

TEST(CurrentWorldCompiler, DefinitionsOccurrencesAndLayerPathAreCanonical) {
    const auto result = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)});
    ASSERT_TRUE(result.ok());
    ASSERT_NE(nullptr, result.world);
    const auto& world = result.world->View();
    ASSERT_EQ(3u, world.playablePropertyCount);
    ASSERT_EQ(1u, world.playableOccurrenceCount);
    ASSERT_EQ(1u, world.containerCount);
    EXPECT_EQ(harken::kCurrentWorldShippingBodyVintage, world.sourceBodyVintage);
    for (std::uint32_t i = 0; i < world.playablePropertyCount; ++i) {
        EXPECT_EQ(UINT64_C(0x12fe0),
                  world.playableProperties[i].payload.presentMask);
        EXPECT_EQ(harkenstudio::kMasterBusObjectId, world.playableProperties[i].payload.values[6].objectId);
    }
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::ValidateCompilerWorld(world));

    const auto& sound = world.playableProperties[1];
    const auto& container = world.playableProperties[2];
    ASSERT_EQ(1u, sound.organizationLayerAncestorCount);
    ASSERT_EQ(1u, container.organizationLayerAncestorCount);
    EXPECT_EQ(harkentest::StudioObjectId(1u), sound.organizationLayerAncestors[0].layerStableId);
    EXPECT_EQ(harkentest::StudioObjectId(1u), container.organizationLayerAncestors[0].layerStableId);
    EXPECT_NE(0u, sound.organizationLayerAncestors[0]
                      .layerProgramSemanticFingerprint);
    ASSERT_EQ(2u, sound.structuralAncestorCount);
    EXPECT_EQ(harkenstudiobuild::playableproperties::ContentPropertyKindV1::Layer,
              sound.structuralAncestors[0].contentPropertyKind);
    EXPECT_EQ(harkenstudiobuild::playableproperties::ContentPropertyKindV1::Container,
              sound.structuralAncestors[1].contentPropertyKind);
    EXPECT_EQ(harkentest::StudioObjectId(1u), sound.structuralAncestors[0].stableId);
    EXPECT_EQ(harkentest::StudioObjectId(2u), sound.structuralAncestors[1].stableId);
    ASSERT_EQ(1u, container.structuralAncestorCount);
    EXPECT_EQ(harkenstudiobuild::playableproperties::ContentPropertyKindV1::Layer,
              container.structuralAncestors[0].contentPropertyKind);
    EXPECT_EQ(UINT64_C(1) << 3,
              world.playableOccurrences[0].payload.presentMask);
    EXPECT_EQ(1u, world.playableOccurrences[0].payload.values[3].bits);

    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto installed =
        harkentest::InstallCompilerWorld(*session.Engine(), world);
    EXPECT_EQ(harken::WorldInstallStatus::Ok, installed.status);
    EXPECT_NE(0u, installed.resultGeneration);
    const auto installedView = session.Engine()->variables.CurrentPlayableWorld();
    EXPECT_EQ(18u, installedView.ProgramSource().sourceBodyVintage);
    EXPECT_EQ(installed.resultGeneration, installedView.ProgramSource().worldGeneration);
    EXPECT_EQ(0u, installedView.ProgramSource().ownerStableId);
}

TEST(CurrentWorldCompiler, CompiledBusIdSetMustBeCanonicalAndOwnEveryRoute) {
    auto project = Project();
    constexpr harkenstudio::ObjectId routedBus = harkentest::StudioObjectId(UINT64_C(0x100000005));
    project.objects[0].layer.chassis.mainOutputBusRef = routedBus;
    project.objects[1].container.chassis.mainOutputBusRef = routedBus;
    project.objects[2].sound.chassis.mainOutputBusRef = harkentest::StudioObjectId(routedBus);

    harkenbuscompiler::CompiledBusIdSetV1 unsorted{{routedBus, harkenstudio::kMasterBusObjectId}};
    auto compile = [&](const harkenbuscompiler::CompiledBusIdSetV1& busIds) {
        return harkencurrentworldcompiler::CompileCurrentWorldV1(
            project, {}, Materializations(),
            {Authored(harkenstudiobuild::ContainerModeV1::Random,
                      harkenstudiobuild::RandomDisciplineV1::Draw)},
            {}, {}, {}, &busIds);
    };

    auto result = compile(unsorted);
    EXPECT_EQ(harkencurrentworldcompiler::CompileStatusV1::InvalidProject,
              result.status);
    EXPECT_EQ(nullptr, result.world);

    harkenbuscompiler::CompiledBusIdSetV1 duplicate{{harkenstudio::kMasterBusObjectId,
                                                    harkenstudio::kMasterBusObjectId}};
    result = compile(duplicate);
    EXPECT_EQ(harkencurrentworldcompiler::CompileStatusV1::InvalidProject,
              result.status);
    EXPECT_EQ(nullptr, result.world);

    harkenbuscompiler::CompiledBusIdSetV1 missingRoute{{harkenstudio::kMasterBusObjectId}};
    result = compile(missingRoute);
    EXPECT_EQ(harkencurrentworldcompiler::CompileStatusV1::InvalidProject,
              result.status);
    EXPECT_EQ(routedBus, result.relatedStableId);
    EXPECT_EQ(nullptr, result.world);

    harkenbuscompiler::CompiledBusIdSetV1 canonical{{routedBus}};
    result = compile(canonical);
    ASSERT_TRUE(result.ok());
    ASSERT_NE(nullptr, result.world);
}

TEST(CurrentWorldCompiler,
     RepeatedContainerChildCompilesAsDistinctOccurrenceAndProgramRows) {
    auto project = Project();
    auto& occurrences = project.objects[1].container.children;
    occurrences[0].presence.bits = 0;
    occurrences[0].presence.Set(
        harkenplayableproperties::PlayablePropertyIdV1::Volume);
    occurrences[0].volume = -6.0f;
    auto second = occurrences[0];
    second.stableOccurrenceOrdinal = 9;
    second.presence.bits = 0;
    second.presence.Set(
        harkenplayableproperties::PlayablePropertyIdV1::Pitch);
    second.pitch = 3.0f;
    occurrences.push_back(second);

    auto authored = Authored(harkenstudiobuild::ContainerModeV1::Random,
                             harkenstudiobuild::RandomDisciplineV1::Draw);
    auto secondAuthored = authored.children[0];
    secondAuthored.stableOccurrenceOrdinal = 9;
    secondAuthored.authoredStableOrder = 1;
    secondAuthored.frequencyWeight = 2;
    authored.children.push_back(secondAuthored);

    const auto result = harkencurrentworldcompiler::CompileCurrentWorldV1(
        project, {}, Materializations(), {authored});
    ASSERT_TRUE(result.ok())
        << static_cast<std::uint32_t>(result.status) << " "
        << result.relatedStableId;
    ASSERT_NE(nullptr, result.world);
    const auto& world = result.world->View();
    ASSERT_EQ(world.playableOccurrenceCount, 2u);
    ASSERT_EQ(world.containerCount, 1u);
    ASSERT_EQ(world.containers[0].childCount, 2u);

    EXPECT_EQ(world.containers[0].children[0].playable.stableId, harkentest::StudioObjectId(3u));
    EXPECT_EQ(world.containers[0].children[1].playable.stableId, harkentest::StudioObjectId(3u));
    EXPECT_EQ(world.containers[0].children[0].stableChildOrdinal, 7u);
    EXPECT_EQ(world.containers[0].children[1].stableChildOrdinal, 9u);
    EXPECT_EQ(world.playableOccurrences[0].childPlayable.stableId, harkentest::StudioObjectId(3u));
    EXPECT_EQ(world.playableOccurrences[1].childPlayable.stableId, harkentest::StudioObjectId(3u));
    EXPECT_EQ(world.playableOccurrences[0].stableOccurrenceOrdinal, 7u);
    EXPECT_EQ(world.playableOccurrences[1].stableOccurrenceOrdinal, 9u);
    EXPECT_EQ(world.playableOccurrences[0].payload.presentMask, UINT64_C(1));
    EXPECT_EQ(world.playableOccurrences[1].payload.presentMask, UINT64_C(2));
    EXPECT_EQ(world.playableOccurrences[0].payload.values[0].bits,
              std::bit_cast<std::uint32_t>(-6.0f));
    EXPECT_EQ(world.playableOccurrences[1].payload.values[1].bits,
              std::bit_cast<std::uint32_t>(3.0f));
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::ValidateCompilerWorld(world));
}

TEST(CurrentWorldCompiler, MovingSoundToAnotherParentChangesStructuralFingerprint) {
    const auto nested = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)});
    ASSERT_TRUE(nested.ok());
    const auto& nestedSound = nested.world->View().playableProperties[1];
    harkenstudiobuild::PlayablePropertyProgramViewV1 movedSound = nestedSound;
    harkenstudiobuild::StructuralAncestorRefV1 movedParent{
        harkenstudiobuild::playableproperties::ContentPropertyKindV1::Layer, 0, harkentest::StudioObjectId(1),
        nestedSound.structuralAncestors[0].programSemanticFingerprint};
    movedSound.structuralAncestorCount = 1;
    movedSound.structuralAncestors = &movedParent;
    movedSound.programSemanticFingerprint =
        harkenstudiobuild::ComputePlayablePropertyFingerprintV1(movedSound);
    EXPECT_NE(nestedSound.programSemanticFingerprint,
              movedSound.programSemanticFingerprint);
}

TEST(CurrentWorldCompiler, ZeroActionFingerprintIncludesVariableDefault) {
    harkenstudiobuild::VariableDefinitionV1 globalVariable{};
    globalVariable.variableId = harkentest::StudioObjectId(90);
    globalVariable.kind = harkenstudiobuild::VariableKindV1::Smooth;
    globalVariable.rangeMin = -1.0f;
    globalVariable.rangeMax = 1.0f;
    auto emitterVariable = globalVariable;
    emitterVariable.defaultValue = 0.25f;

    const auto global = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {globalVariable}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)});
    const auto emitter = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {emitterVariable}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)});

    ASSERT_TRUE(global.ok());
    ASSERT_TRUE(emitter.ok());
    const auto& globalWorld = global.world->View();
    const auto& emitterWorld = emitter.world->View();
    ASSERT_EQ(0u, globalWorld.actionCount);
    ASSERT_EQ(0u, emitterWorld.actionCount);
    EXPECT_EQ(harkenstudiobuild::ComputeCurrentWorldFingerprintV4(globalWorld),
              globalWorld.identity.runtimeSemanticFingerprint);
    EXPECT_EQ(harkenstudiobuild::ComputeCurrentWorldFingerprintV4(emitterWorld),
              emitterWorld.identity.runtimeSemanticFingerprint);
    EXPECT_NE(globalWorld.identity.runtimeSemanticFingerprint,
              emitterWorld.identity.runtimeSemanticFingerprint);
}

TEST(CurrentWorldCompiler, ZeroActionDefaultReplacementAdvancesGeneration) {
    harkenstudiobuild::VariableDefinitionV1 globalVariable{};
    globalVariable.variableId = harkentest::StudioObjectId(91);
    globalVariable.kind = harkenstudiobuild::VariableKindV1::Smooth;
    globalVariable.rangeMin = -1.0f;
    globalVariable.rangeMax = 1.0f;
    auto emitterVariable = globalVariable;
    emitterVariable.defaultValue = 0.25f;

    const auto global = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {globalVariable}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)});
    const auto emitter = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {emitterVariable}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)});
    ASSERT_TRUE(global.ok());
    ASSERT_TRUE(emitter.ok());

    std::vector<std::uint8_t> globalArtifact;
    std::vector<std::uint8_t> emitterArtifact;
    ASSERT_TRUE(harkenstudiobuild::EncodeCurrentWorldArtifactV1(global.world->View(),
                                                    globalArtifact));
    ASSERT_TRUE(harkenstudiobuild::EncodeCurrentWorldArtifactV1(emitter.world->View(),
                                                    emitterArtifact));
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    const std::filesystem::path globalPath =
        std::filesystem::path(HARKEN_TEMP_DIR) /
        "current-world-zero-action-global.vnworld";
    const std::filesystem::path emitterPath =
        std::filesystem::path(HARKEN_TEMP_DIR) /
        "current-world-zero-action-emitter.vnworld";
    ASSERT_TRUE(WriteBinary(globalPath, globalArtifact));
    ASSERT_TRUE(WriteBinary(emitterPath, emitterArtifact));

    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const auto load = [](const std::filesystem::path& path,
                         VnCurrentWorldLoadReport& report) {
        const std::string utf8Path = path.string();
        VnCurrentWorldLoadDesc desc{};
        desc.structSize = sizeof(desc);
        desc.utf8Path = utf8Path.c_str();
        report = {};
        report.structSize = sizeof(report);
        return Vn_LoadCurrentWorld(&desc, &report);
    };

    VnCurrentWorldLoadReport globalReport{};
    ASSERT_EQ(VN_OK, load(globalPath, globalReport));
    ASSERT_NE(0u, globalReport.resultGeneration);
    const auto* installedGlobal =
        session.Engine()->variables.Definition(harkentest::ToRuntime(globalVariable.variableId));
    ASSERT_NE(nullptr, installedGlobal);
    EXPECT_EQ(globalVariable.variableId, harkentest::ToStudio(installedGlobal->variableId));

    VnCurrentWorldLoadReport emitterReport{};
    ASSERT_EQ(VN_OK, load(emitterPath, emitterReport));
    EXPECT_EQ(globalReport.resultGeneration, emitterReport.priorGeneration);
    EXPECT_GT(emitterReport.resultGeneration, globalReport.resultGeneration);
    const auto* installedEmitter =
        session.Engine()->variables.Definition(harkentest::ToRuntime(emitterVariable.variableId));
    ASSERT_NE(nullptr, installedEmitter);
    EXPECT_FLOAT_EQ(0.25f, installedEmitter->defaultValue);
    VnEmitterHandle handle = 0;
    ASSERT_EQ(VN_OK, Vn_CreateEmitter(&handle));
    EXPECT_EQ(VN_OK, Vn_SetEmitterValue(handle, installedEmitter->variableId, 0.5f));

    std::error_code ignored;
    std::filesystem::remove(globalPath, ignored);
    std::filesystem::remove(emitterPath, ignored);
}

TEST(CurrentWorldCompiler, ActionRowsAreCanonicalInstalledDataBeforeExecution) {
    harkencurrentworldcompiler::ActionProgramAuthoringV1 action{};
    action.actionStableId = harkentest::StudioObjectId(40);
    harkencurrentworldcompiler::ActionStepAuthoringV1 step{};
    step.opcode = harkenstudiobuild::ActionOpcodeV1::Play;
    step.authoredEventIndex = 0;
    step.target.kind = harkenstudiobuild::ActionTargetKindV1::Sound;
    step.target.stableId = harkentest::StudioObjectId(3);
    action.steps.push_back(step);

    const auto result = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)},
        {}, {}, {action});

    ASSERT_TRUE(result.ok());
    ASSERT_NE(nullptr, result.world);
    const auto& world = result.world->View();
    ASSERT_EQ(1u, world.actionCount);
    ASSERT_NE(nullptr, world.actions);
    EXPECT_EQ(harkentest::StudioObjectId(40u), world.actions[0].actionStableId);
    ASSERT_EQ(1u, world.actions[0].stepCount);
    EXPECT_EQ(harkenstudiobuild::ActionOpcodeV1::Play,
              world.actions[0].steps[0].opcode);
    EXPECT_EQ(harkenstudiobuild::ActionTargetKindV1::Sound,
              world.actions[0].steps[0].target.kind);
    EXPECT_EQ(harkentest::StudioObjectId(3u), world.actions[0].steps[0].target.stableId);
    EXPECT_NE(0u,
              world.actions[0].steps[0].target
                  .expectedProgramSemanticFingerprint);
    EXPECT_NE(0u, world.actions[0].programSemanticFingerprint);
    EXPECT_NE(0u,
              world.identity.capabilityMask &
                  harken::kWorldCapabilityActionDefinitions);
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::ValidateCompilerWorld(world));

    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::InstallCompilerWorld(*session.Engine(), world).status);
}

TEST(CurrentWorldCompiler, V9BodyRoundTripsActionTransitionWideLoopCountAndStructure) {
    auto project = Project();
    project.objects[1].container.children[0].loopCount = 1000;

    auto container = Authored(harkenstudiobuild::ContainerModeV1::SequenceQueue,
                              harkenstudiobuild::RandomDisciplineV1::Draw);
    container.transitionProgramIndex = 0;
    container.curves = {FadeCurve(1)};

    harkencurrentworldcompiler::TransitionProgramAuthoringV1 transition{};
    transition.ownerContainerStableId = container.containerStableId;
    harkenstudiobuild::TransitionRuleViewV1 rule{};
    rule.sourceAny = 1;
    rule.targetAny = 1;
    rule.targetFade.active = 1;
    rule.targetFade.durationFrames = 96;
    rule.targetFade.curveLocalId = 1;
    transition.rules.push_back(rule);

    harkencurrentworldcompiler::ActionProgramAuthoringV1 action{};
    action.actionStableId = harkentest::StudioObjectId(40);
    harkencurrentworldcompiler::ActionStepAuthoringV1 step{};
    step.target.kind = harkenstudiobuild::ActionTargetKindV1::Sound;
    step.target.stableId = harkentest::StudioObjectId(3);
    action.steps.push_back(step);

    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        project, {}, Materializations(), {container}, {}, {transition},
        {action});
    ASSERT_TRUE(compiled.ok());

    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(harkenstudiobuild::EncodeCurrentWorldShippingBody(compiled.world->View(), bytes));
    ASSERT_FALSE(bytes.empty());

    harken::CurrentWorldBodyImage decoded;
    ASSERT_TRUE(decoded.Decode(bytes.data(), bytes.size(),
                               harken::kCurrentWorldShippingBodyVintage,
                               harken::CurrentWorldDecodeMode::StrictArtifact));
    const auto& world = *decoded.ProductionView();
    ASSERT_EQ(1u, world.actionCount);
    ASSERT_EQ(1u, world.transitionCount);
    ASSERT_EQ(1u, world.playableOccurrenceCount);
    EXPECT_EQ(1000u, world.playableOccurrences[0].payload.values[3].bits);
    EXPECT_EQ(harkentest::StudioObjectId(40u), world.actions[0].actionStableId);
    EXPECT_EQ(96u, world.transitions[0].rules[0].targetFade.durationFrames);
    EXPECT_EQ(compiled.world->View().identity.runtimeSemanticFingerprint,
              world.identity.runtimeSemanticFingerprint);
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::ValidateCompilerWorld(world));
}

TEST(CurrentWorldCompiler, V9BodyRejectsHostileActionCounts) {
    harkencurrentworldcompiler::ActionProgramAuthoringV1 action{};
    action.actionStableId = harkentest::StudioObjectId(40);
    harkencurrentworldcompiler::ActionStepAuthoringV1 step{};
    step.target.kind = harkenstudiobuild::ActionTargetKindV1::Sound;
    step.target.stableId = harkentest::StudioObjectId(3);
    action.steps.push_back(step);
    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)},
        {}, {}, {action});
    ASSERT_TRUE(compiled.ok());

    std::vector<std::uint8_t> canonical;
    ASSERT_TRUE(harkenstudiobuild::EncodeCurrentWorldShippingBody(compiled.world->View(),
                                                canonical));
    harken::CurrentWorldBodyImage image;
    ASSERT_TRUE(image.Decode(canonical.data(), canonical.size(),
                             harken::kCurrentWorldShippingBodyVintage,
                             harken::CurrentWorldDecodeMode::StrictArtifact));
    const auto installedFingerprint =
        image.ProductionView()->identity.runtimeSemanticFingerprint;

    auto hostile = canonical;
    WriteU32(hostile, harken::kCurrentWorldV3PrefixWireBytes,
             harken::kHardActionDefinitionCapacity + 1u);
    EXPECT_FALSE(image.Decode(hostile.data(), hostile.size(),
                              harken::kCurrentWorldShippingBodyVintage,
                              harken::CurrentWorldDecodeMode::StrictArtifact));
    EXPECT_EQ(installedFingerprint,
              image.ProductionView()->identity.runtimeSemanticFingerprint);

    const std::size_t actionHeader =
        canonical.size() - harken::kCurrentWorldActionHeaderWireBytes -
        harken::kCurrentWorldActionStepWireBytes;
    hostile = canonical;
    WriteU32(hostile, actionHeader + 24, harken::kMaxActionSteps + 1u);
    EXPECT_FALSE(image.Decode(hostile.data(), hostile.size(),
                              harken::kCurrentWorldShippingBodyVintage,
                              harken::CurrentWorldDecodeMode::StrictArtifact));
    EXPECT_EQ(installedFingerprint,
              image.ProductionView()->identity.runtimeSemanticFingerprint);

    hostile = canonical;
    WriteU32(hostile, actionHeader + 24, harken::kMaxActionSteps);
    EXPECT_FALSE(image.Decode(hostile.data(), hostile.size(),
                              harken::kCurrentWorldShippingBodyVintage,
                              harken::CurrentWorldDecodeMode::StrictArtifact));
    EXPECT_EQ(installedFingerprint,
              image.ProductionView()->identity.runtimeSemanticFingerprint);
}

TEST(CurrentWorldCompiler, RetiredBindingWireSlotsMustRemainZero) {
    harkenprojectschema::CanonicalProject project{};
    project.objects.push_back(Sound(40));
    project.roots.push_back(harkentest::StudioObjectId(40));
    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        project, {},
        {{harkenplayableproperties::ContentPropertyKindV1::Sound,
          harkentest::StudioObjectId(40), 400, harkenstudio::kMasterBusObjectId, 1}}, {});
    ASSERT_TRUE(compiled.ok());
    std::vector<std::uint8_t> canonical;
    ASSERT_TRUE(harkenstudiobuild::EncodeCurrentWorldShippingBody(compiled.world->View(), canonical));
    harken::CurrentWorldBodyImage image;
    ASSERT_TRUE(image.Decode(canonical.data(), canonical.size(),
                             harken::kCurrentWorldShippingBodyVintage,
                             harken::CurrentWorldDecodeMode::StrictArtifact));
    const auto fingerprint = image.ProductionView()->identity.runtimeSemanticFingerprint;
    constexpr std::size_t payloadHeaderBytes = 95;
    ASSERT_GE(canonical.size(), payloadHeaderBytes);
    const auto payloadOffset = canonical.size() - payloadHeaderBytes;
    const std::size_t retiredOffsets[] = {
        56, 60, payloadOffset + 67, payloadOffset + 79, payloadOffset + 83};
    for (const auto offset : retiredOffsets) {
        auto retired = canonical;
        WriteU32(retired, offset, 1);
        for (const auto mode : {harken::CurrentWorldDecodeMode::StrictArtifact,
                                harken::CurrentWorldDecodeMode::ReplayRawAttempt}) {
            EXPECT_FALSE(image.Decode(retired.data(), retired.size(),
                                      harken::kCurrentWorldShippingBodyVintage, mode))
                << offset;
            EXPECT_EQ(fingerprint, image.ProductionView()->identity.runtimeSemanticFingerprint);
        }
    }
}

TEST(CurrentWorldCompiler, V6BodyRejectsStructuralAncestorData) {
    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)});
    ASSERT_TRUE(compiled.ok());
    bool hasStructuralAncestors = false;
    for (std::uint32_t i = 0;
         i < compiled.world->View().playablePropertyCount; ++i) {
        hasStructuralAncestors = hasStructuralAncestors ||
            compiled.world->View().playableProperties[i].structuralAncestorCount != 0;
    }
    ASSERT_TRUE(hasStructuralAncestors);

    std::vector<std::uint8_t> bytes;
    harken::CurrentWorldBodyImage image;
    ASSERT_TRUE(harkentest::DecodeStudioWorld(compiled.world->View(), image));
    EXPECT_FALSE(harken::EncodeCurrentWorldBodyV6(harkentest::AsHistoricalPrefix(*image.ProductionView()), bytes));
    EXPECT_TRUE(bytes.empty());
}

TEST(CurrentWorldCompiler, ArtifactEnvelopePinsCanonicalBodyIdentity) {
    harkencurrentworldcompiler::ActionProgramAuthoringV1 action{};
    action.actionStableId = harkentest::StudioObjectId(40);
    harkencurrentworldcompiler::ActionStepAuthoringV1 step{};
    step.target.kind = harkenstudiobuild::ActionTargetKindV1::Sound;
    step.target.stableId = harkentest::StudioObjectId(3);
    action.steps.push_back(step);
    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)},
        {}, {}, {action});
    ASSERT_TRUE(compiled.ok());

    std::vector<std::uint8_t> artifact;
    ASSERT_TRUE(harkenstudiobuild::EncodeCurrentWorldArtifactV1(compiled.world->View(),
                                                    artifact));
    ASSERT_GE(artifact.size(), harken::kCurrentWorldArtifactHeaderBytes);
    EXPECT_EQ(std::vector<std::uint8_t>({'V', 'N', 'C', 'W', 'O', 'R', 'L',
                                         'D'}),
              std::vector<std::uint8_t>(artifact.begin(),
                                        artifact.begin() + 8));
    EXPECT_EQ(1u, artifact[8]);
    EXPECT_EQ(0u, artifact[9]);
    EXPECT_EQ(0u, artifact[10]);
    EXPECT_EQ(0u, artifact[11]);
    EXPECT_EQ(harken::kCurrentWorldArtifactHeaderBytes, ReadU64(artifact, 12) &
                                                           UINT64_C(0xffffffff));
    EXPECT_EQ(1u, ReadU64(artifact, 16) & UINT64_C(0xffffffff));
    EXPECT_EQ(harken::kCurrentWorldShippingBodyVintage,
              harken::kCanonicalDefinitionPartitionBodyVintage);
    EXPECT_EQ(harken::kCurrentWorldShippingBodyVintage,
              ReadU64(artifact, 20) & UINT64_C(0xffffffff));
    const std::uint64_t bodyBytes = ReadU64(artifact, 24);
    EXPECT_EQ(artifact.size() - harken::kCurrentWorldArtifactHeaderBytes,
              bodyBytes);
    std::vector<std::uint8_t> shippingBody;
    ASSERT_TRUE(harkenstudiobuild::EncodeCurrentWorldShippingBody(
        compiled.world->View(), shippingBody));
    EXPECT_EQ(shippingBody,
              std::vector<std::uint8_t>(
                  artifact.begin() + harken::kCurrentWorldArtifactHeaderBytes,
                  artifact.end()));
    EXPECT_EQ(harken::Fnv1a64(
                  artifact.data() + harken::kCurrentWorldArtifactHeaderBytes,
                  static_cast<std::size_t>(bodyBytes)),
              ReadU64(artifact, 32));
    EXPECT_EQ(compiled.world->View().identity.runtimeSemanticFingerprint,
              ReadU64(artifact, 40));
    EXPECT_EQ(compiled.world->View().identity.capabilityMask,
              ReadU64(artifact, 48));
    EXPECT_EQ(0u, ReadU64(artifact, 56));
}

TEST(CurrentWorldCompiler, StrictAndReplayDecodeRejectHistoricalBody) {
    harken::ContainerWorldInstallViewV1 world{};
    std::vector<std::uint8_t> shippingBody;
    ASSERT_TRUE(harken::EncodeCurrentWorldShippingBody(world, shippingBody));

    harken::CurrentWorldBodyImage strictShipping;
    EXPECT_EQ(harken::CurrentWorldBodyDecodeResult::Ok,
              strictShipping.DecodeWithResult(
                  shippingBody.data(), shippingBody.size(),
                  harken::kCurrentWorldShippingBodyVintage,
                  harken::CurrentWorldDecodeMode::StrictArtifact));

    std::vector<std::uint8_t> historicalBody;
    ASSERT_TRUE(harken::EncodeCurrentWorldBodyV6(harkentest::AsHistoricalPrefix(world), historicalBody));
    harken::CurrentWorldBodyImage strictHistorical;
    EXPECT_EQ(harken::CurrentWorldBodyDecodeResult::Invalid,
              strictHistorical.DecodeWithResult(
                  historicalBody.data(), historicalBody.size(),
                  harken::kCurrentWorldBodyVintageV6,
                  harken::CurrentWorldDecodeMode::StrictArtifact));

    harken::CurrentWorldBodyImage replayHistorical;
    EXPECT_EQ(harken::CurrentWorldBodyDecodeResult::Invalid,
              replayHistorical.DecodeWithResult(
                  historicalBody.data(), historicalBody.size(),
                  harken::kCurrentWorldBodyVintageV6,
                  harken::CurrentWorldDecodeMode::ReplayRawAttempt));
}

TEST(CurrentWorldCompiler, ArtifactEnvelopeRefusesRetiredBodyVintage) {
    harkencurrentworldcompiler::ActionProgramAuthoringV1 action{};
    action.actionStableId = harkentest::StudioObjectId(40);
    harkencurrentworldcompiler::ActionStepAuthoringV1 step{};
    step.target.kind = harkenstudiobuild::ActionTargetKindV1::Sound;
    step.target.stableId = harkentest::StudioObjectId(3);
    action.steps.push_back(step);
    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)},
        {}, {}, {action});
    ASSERT_TRUE(compiled.ok());

    std::vector<std::uint8_t> artifact;
    ASSERT_TRUE(harkenstudiobuild::EncodeCurrentWorldArtifactV1(compiled.world->View(),
                                                    artifact));
    const auto observedBytes = static_cast<std::uint64_t>(artifact.size());
    harken::CurrentWorldArtifactHeaderV1 header{};

    for (const auto vintage : {harken::kCurrentWorldBodyVintageV6,
                                harken::kCurrentWorldBodyVintageV7,
                                harken::kCurrentWorldBodyVintageV8,
                                harken::kCurrentWorldBodyVintageV19 + 1}) {
        auto hostile = artifact;
        WriteU32(hostile, 20, vintage);
        EXPECT_EQ(harken::CurrentWorldLoadStatusV1::UnsupportedBody,
                  harken::InspectCurrentWorldArtifactHeaderV1(
                      hostile.data(), hostile.size(), observedBytes, header))
            << vintage;
        EXPECT_EQ(0u, header.bodyBytes);
    }
}

TEST(CurrentWorldCompiler, VintageEightRetainsV7WireAndCannotBecomeProduction) {
    harkenprojectschema::CanonicalProject project{};
    project.objects.push_back(Sound(40));
    project.roots.push_back(harkentest::StudioObjectId(40));
    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        project, {},
        {{harkenplayableproperties::ContentPropertyKindV1::Sound,
          harkentest::StudioObjectId(40), 400, harkenstudio::kMasterBusObjectId, 1}}, {});
    ASSERT_TRUE(compiled.ok());
    harken::CurrentWorldBodyImage currentImage;
    ASSERT_TRUE(harkentest::DecodeStudioWorld(compiled.world->View(), currentImage));
    auto historical = harkentest::AsHistoricalPrefix(*currentImage.ProductionView());
    const auto& sound = historical.playableProperties[0];
    harken::ContainerChildProgramViewV1 child{};
    child.playable.kind = harken::PlayableKindV1::Sound;
    child.playable.stableId = sound.stableId;
    child.playable.expectedProgramSemanticFingerprint =
        sound.programSemanticFingerprint;
    child.playable.contentRevision = sound.contentRevision;
    child.playable.routeStableId = sound.routeStableId;
    child.playable.compatibilityFingerprint = sound.compatibilityFingerprint;
    child.stableChildOrdinal = 3;
    harken::AuthoredCompletionLeafV1 leaf{};
    leaf.content = child.playable;
    leaf.startOffset = {harken::AuthoredTimeDomainV1::SourceFrames, 0, 1};
    leaf.duration = {harken::AuthoredTimeDomainV1::SourceFrames, 48000, 1};
    leaf.loopCount = 1;
    child.completion.payload.inlineTemplate.completionClass =
        harken::AuthoredCompletionClassV1::FiniteMediaCandidate;
    child.completion.payload.inlineTemplate.leafCount = 1;
    child.completion.payload.inlineTemplate.leaves = &leaf;
    harken::ContainerProgramViewV2 queue{};
    queue.containerStableId = harkentest::ObjectId(90);
    queue.programSemanticFingerprint = 9001;
    queue.mode = harken::ContainerModeV1::SequenceQueue;
    queue.queueCycle = harken::QueueCycleV1::Loop;
    queue.reservedBool[0] = 255;
    queue.signalOnlyPolicy = harken::SignalOnlyPolicyV1::StopQueue;
    queue.childCount = 1;
    queue.children = &child;
    historical.containers = &queue;
    historical.containerCount = 1;
    historical.identity.runtimeSemanticFingerprint =
        harken::ComputeCurrentWorldFingerprintV4(historical);
    std::vector<std::uint8_t> vintageSeven;
    std::vector<std::uint8_t> vintageEight;
    ASSERT_TRUE(harken::EncodeCurrentWorldBodyV7(historical, vintageSeven));
    ASSERT_TRUE(harken::EncodeCurrentWorldBodyV8(historical, vintageEight));
    EXPECT_EQ(vintageSeven, vintageEight);
    ASSERT_EQ(18u, harken::kCurrentWorldShippingBodyVintage);

    harken::CurrentWorldBodyImage strict;
    EXPECT_EQ(nullptr, strict.ProductionView());
    EXPECT_EQ(nullptr, strict.ReplayView());
    EXPECT_FALSE(strict.Decode(vintageEight.data(), vintageEight.size(),
                               harken::kCurrentWorldBodyVintageV8,
                               harken::CurrentWorldDecodeMode::StrictArtifact));
    EXPECT_FALSE(strict.Decode(vintageEight.data(), vintageEight.size(),
                               harken::kCurrentWorldShippingBodyVintage,
                               harken::CurrentWorldDecodeMode::StrictArtifact));

    harken::CurrentWorldBodyImage replay;
    EXPECT_FALSE(replay.Decode(vintageEight.data(), vintageEight.size(),
                              harken::kCurrentWorldBodyVintageV8,
                              harken::CurrentWorldDecodeMode::ReplayRawAttempt));
    EXPECT_EQ(nullptr, replay.ReplayView());
    EXPECT_EQ(nullptr, replay.ProductionView());

    auto malformed = vintageEight;
    WriteU32(malformed, 0, 0);
    harken::CurrentWorldBodyImage diagnostic;
    EXPECT_FALSE(diagnostic.Decode(malformed.data(), malformed.size(),
                                  harken::kCurrentWorldBodyVintageV8,
                                  harken::CurrentWorldDecodeMode::ReplayRawAttempt));
    EXPECT_EQ(nullptr, diagnostic.ProductionView());
    EXPECT_EQ(nullptr, diagnostic.ReplayView());

    std::vector<std::uint8_t> shipping;
    ASSERT_TRUE(harkenstudiobuild::EncodeCurrentWorldShippingBody(compiled.world->View(),
                                                     shipping));
    EXPECT_NE(vintageEight, shipping);
    ASSERT_TRUE(strict.Decode(shipping.data(), shipping.size(),
                               harken::kCurrentWorldShippingBodyVintage,
                               harken::CurrentWorldDecodeMode::StrictArtifact));
    ASSERT_NE(nullptr, strict.ProductionView());
    EXPECT_EQ(nullptr, strict.ReplayView());
    const auto productionFingerprint =
        strict.ProductionView()->identity.runtimeSemanticFingerprint;
    EXPECT_FALSE(strict.Decode(vintageEight.data(), vintageEight.size(),
                                harken::kCurrentWorldBodyVintageV8,
                                harken::CurrentWorldDecodeMode::StrictArtifact));
    ASSERT_NE(nullptr, strict.ProductionView());
    EXPECT_EQ(productionFingerprint,
              strict.ProductionView()->identity.runtimeSemanticFingerprint);
    EXPECT_FALSE(replay.Decode(shipping.data(), shipping.size(),
                               harken::kCurrentWorldBodyVintageV8,
                               harken::CurrentWorldDecodeMode::ReplayRawAttempt));
    EXPECT_FALSE(replay.Decode(shipping.data(), shipping.size(),
                               harken::kCurrentWorldShippingBodyVintage,
                               harken::CurrentWorldDecodeMode::ReplayRawAttempt));
    EXPECT_EQ(nullptr, replay.ReplayView());
}

TEST(CurrentWorldCompiler, ArtifactHeaderUsesFirstFailurePrecedence) {
    harkencurrentworldcompiler::ActionProgramAuthoringV1 action{};
    action.actionStableId = harkentest::StudioObjectId(40);
    harkencurrentworldcompiler::ActionStepAuthoringV1 step{};
    step.target.kind = harkenstudiobuild::ActionTargetKindV1::Sound;
    step.target.stableId = harkentest::StudioObjectId(3);
    action.steps.push_back(step);
    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)},
        {}, {}, {action});
    ASSERT_TRUE(compiled.ok());

    std::vector<std::uint8_t> artifact;
    ASSERT_TRUE(harkenstudiobuild::EncodeCurrentWorldArtifactV1(compiled.world->View(),
                                                    artifact));
    const auto observedBytes = static_cast<std::uint64_t>(artifact.size());
    harken::CurrentWorldArtifactHeaderV1 header{};

    auto hostile = artifact;
    hostile[0] = 'X';
    WriteU64(hostile, 24, harken::kCurrentWorldArtifactMaxBodyBytes + 1);
    EXPECT_EQ(harken::CurrentWorldLoadStatusV1::BadMagic,
              harken::InspectCurrentWorldArtifactHeaderV1(
                  hostile.data(), hostile.size(), observedBytes, header));
    EXPECT_EQ(0u, header.bodyBytes);

    hostile = artifact;
    WriteU32(hostile, 8, 2);
    WriteU64(hostile, 24, harken::kCurrentWorldArtifactMaxBodyBytes + 1);
    EXPECT_EQ(harken::CurrentWorldLoadStatusV1::UnsupportedEnvelope,
              harken::InspectCurrentWorldArtifactHeaderV1(
                  hostile.data(), hostile.size(), observedBytes, header));
    EXPECT_EQ(0u, header.bodyBytes);

    hostile = artifact;
    WriteU32(hostile, 16, 2);
    WriteU64(hostile, 24, harken::kCurrentWorldArtifactMaxBodyBytes + 1);
    EXPECT_EQ(harken::CurrentWorldLoadStatusV1::UnsupportedBody,
              harken::InspectCurrentWorldArtifactHeaderV1(
                  hostile.data(), hostile.size(), observedBytes, header));
    EXPECT_EQ(0u, header.bodyBytes);

    hostile = artifact;
    WriteU64(hostile, 24, harken::kCurrentWorldArtifactMaxBodyBytes + 1);
    EXPECT_EQ(harken::CurrentWorldLoadStatusV1::TooLarge,
              harken::InspectCurrentWorldArtifactHeaderV1(
                  hostile.data(), hostile.size(), observedBytes, header));
    EXPECT_EQ(harken::kCurrentWorldArtifactMaxBodyBytes + 1,
              header.bodyBytes);

    EXPECT_EQ(harken::CurrentWorldLoadStatusV1::Truncated,
              harken::InspectCurrentWorldArtifactHeaderV1(
                  artifact.data(), 63, 63, header));
    EXPECT_EQ(0u, header.bodyBytes);

    EXPECT_EQ(harken::CurrentWorldLoadStatusV1::Truncated,
              harken::InspectCurrentWorldArtifactHeaderV1(
                  artifact.data(), artifact.size(), observedBytes - 1,
                  header));
    EXPECT_EQ(artifact.size() - harken::kCurrentWorldArtifactHeaderBytes,
              header.bodyBytes);

    EXPECT_EQ(harken::CurrentWorldLoadStatusV1::SizeMismatch,
              harken::InspectCurrentWorldArtifactHeaderV1(
                  artifact.data(), artifact.size(), observedBytes + 1,
                  header));
    EXPECT_EQ(artifact.size() - harken::kCurrentWorldArtifactHeaderBytes,
              header.bodyBytes);
}

TEST(CurrentWorldCompiler, ArtifactBodySeparatesChecksumDecodeAndIdentity) {
    harkencurrentworldcompiler::ActionProgramAuthoringV1 action{};
    action.actionStableId = harkentest::StudioObjectId(40);
    harkencurrentworldcompiler::ActionStepAuthoringV1 step{};
    step.target.kind = harkenstudiobuild::ActionTargetKindV1::Sound;
    step.target.stableId = harkentest::StudioObjectId(3);
    action.steps.push_back(step);
    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)},
        {}, {}, {action});
    ASSERT_TRUE(compiled.ok());

    std::vector<std::uint8_t> artifact;
    ASSERT_TRUE(harkenstudiobuild::EncodeCurrentWorldArtifactV1(compiled.world->View(),
                                                    artifact));
    harken::CurrentWorldArtifactHeaderV1 header{};
    ASSERT_EQ(harken::CurrentWorldLoadStatusV1::Ok,
              harken::InspectCurrentWorldArtifactHeaderV1(
                  artifact.data(), artifact.size(), artifact.size(), header));
    const auto bodyBegin = artifact.begin() +
                           harken::kCurrentWorldArtifactHeaderBytes;
    std::vector<std::uint8_t> body(bodyBegin, artifact.end());
    harken::CurrentWorldBodyImage image;

    body.back() ^= 1;
    EXPECT_EQ(harken::CurrentWorldLoadStatusV1::ChecksumMismatch,
              harken::DecodeCurrentWorldArtifactBodyV1(
                  header, body.data(), body.size(), image));

    body.assign(bodyBegin, artifact.end());
    WriteU32(body, harken::kCurrentWorldV3PrefixWireBytes,
             harken::kHardActionDefinitionCapacity + 1u);
    auto hostileHeader = header;
    hostileHeader.bodyChecksum = harken::Fnv1a64(body.data(), body.size());
    EXPECT_EQ(harken::CurrentWorldLoadStatusV1::DecodeFailed,
              harken::DecodeCurrentWorldArtifactBodyV1(
                  hostileHeader, body.data(), body.size(), image));

    body.assign(bodyBegin, artifact.end());
    hostileHeader = header;
    hostileHeader.runtimeSemanticFingerprint ^= 1;
    EXPECT_EQ(harken::CurrentWorldLoadStatusV1::SemanticInvalid,
              harken::DecodeCurrentWorldArtifactBodyV1(
                  hostileHeader, body.data(), body.size(), image));
    EXPECT_EQ(compiled.world->View().identity.runtimeSemanticFingerprint,
              image.ProductionView()->identity.runtimeSemanticFingerprint);

    EXPECT_EQ(harken::CurrentWorldLoadStatusV1::Ok,
              harken::DecodeCurrentWorldArtifactBodyV1(
                  header, body.data(), body.size(), image));
}

TEST(CurrentWorldCompiler, ArtifactPathLoadsAndIdempotentlyReusesGeneration) {
    harkencurrentworldcompiler::ActionProgramAuthoringV1 action{};
    action.actionStableId = harkentest::StudioObjectId(40);
    harkencurrentworldcompiler::ActionStepAuthoringV1 step{};
    step.target.kind = harkenstudiobuild::ActionTargetKindV1::Sound;
    step.target.stableId = harkentest::StudioObjectId(3);
    action.steps.push_back(step);
    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)},
        {}, {}, {action});
    ASSERT_TRUE(compiled.ok());

    std::vector<std::uint8_t> artifact;
    ASSERT_TRUE(harkenstudiobuild::EncodeCurrentWorldArtifactV1(compiled.world->View(),
                                                    artifact));
    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    const std::filesystem::path path =
        std::filesystem::path(HARKEN_TEMP_DIR) /
        "current-world-load-valid.vnworld";
    {
        std::ofstream out(path, std::ios::binary | std::ios::trunc);
        ASSERT_TRUE(out);
        out.write(reinterpret_cast<const char*>(artifact.data()),
                  static_cast<std::streamsize>(artifact.size()));
        ASSERT_TRUE(out);
    }

    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    const std::string utf8Path = path.string();
    VnCurrentWorldLoadDesc desc{};
    desc.structSize = sizeof(desc);
    desc.utf8Path = utf8Path.c_str();
    VnCurrentWorldLoadReport report{};
    report.structSize = sizeof(report);
    ASSERT_EQ(VN_OK, Vn_LoadCurrentWorld(&desc, &report));
    EXPECT_EQ(VN_CURRENT_WORLD_LOAD_OK, report.status);
    EXPECT_EQ(0u, report.priorGeneration);
    EXPECT_NE(0u, report.resultGeneration);
    EXPECT_EQ(compiled.world->View().identity.runtimeSemanticFingerprint,
              report.runtimeSemanticFingerprint);
    EXPECT_EQ(compiled.world->View().identity.capabilityMask,
              report.capabilityMask);
    EXPECT_EQ(artifact.size() - harken::kCurrentWorldArtifactHeaderBytes,
              report.bodyBytes);

    const std::uint64_t firstGeneration = report.resultGeneration;
    report.structSize = sizeof(report);
    ASSERT_EQ(VN_OK, Vn_LoadCurrentWorld(&desc, &report));
    EXPECT_EQ(firstGeneration, report.priorGeneration);
    EXPECT_EQ(firstGeneration, report.resultGeneration);

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
}

TEST(CurrentWorldCompiler, ArtifactPathReportsFirstMalformedFileFailure) {
    harkencurrentworldcompiler::ActionProgramAuthoringV1 action{};
    action.actionStableId = harkentest::StudioObjectId(40);
    harkencurrentworldcompiler::ActionStepAuthoringV1 step{};
    step.target.kind = harkenstudiobuild::ActionTargetKindV1::Sound;
    step.target.stableId = harkentest::StudioObjectId(3);
    action.steps.push_back(step);
    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)},
        {}, {}, {action});
    ASSERT_TRUE(compiled.ok());
    std::vector<std::uint8_t> canonical;
    ASSERT_TRUE(harkenstudiobuild::EncodeCurrentWorldArtifactV1(compiled.world->View(),
                                                    canonical));

    std::filesystem::create_directories(HARKEN_TEMP_DIR);
    const std::filesystem::path path =
        std::filesystem::path(HARKEN_TEMP_DIR) /
        "current-world-load-malformed.vnworld";
    const std::string utf8Path = path.string();
    VnCurrentWorldLoadDesc desc{};
    desc.structSize = sizeof(desc);
    desc.utf8Path = utf8Path.c_str();
    harkentest::EngineSession session(harkentest::MakeNullConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    VnCurrentWorldLoadReport report{};
    const auto load = [&](const std::vector<std::uint8_t>& bytes) {
        EXPECT_TRUE(WriteBinary(path, bytes));
        report = {};
        report.structSize = sizeof(report);
        return Vn_LoadCurrentWorld(&desc, &report);
    };

    std::error_code ignored;
    std::filesystem::remove(path, ignored);
    report.structSize = sizeof(report);
    EXPECT_EQ(VN_CURRENT_WORLD_IO_ERROR,
              Vn_LoadCurrentWorld(&desc, &report));
    EXPECT_EQ(VN_CURRENT_WORLD_LOAD_IO_ERROR, report.status);

    std::vector<std::uint8_t> hostile(canonical.begin(),
                                      canonical.begin() + 63);
    EXPECT_EQ(VN_BAD_CURRENT_WORLD, load(hostile));
    EXPECT_EQ(VN_CURRENT_WORLD_LOAD_TRUNCATED, report.status);
    EXPECT_EQ(0u, report.bodyBytes);

    hostile = canonical;
    hostile[0] = 'X';
    WriteU64(hostile, 24, harken::kCurrentWorldArtifactMaxBodyBytes + 1);
    EXPECT_EQ(VN_BAD_CURRENT_WORLD, load(hostile));
    EXPECT_EQ(VN_CURRENT_WORLD_LOAD_BAD_MAGIC, report.status);
    EXPECT_EQ(0u, report.bodyBytes);

    hostile = canonical;
    WriteU64(hostile, 24, harken::kCurrentWorldArtifactMaxBodyBytes + 1);
    EXPECT_EQ(VN_BAD_CURRENT_WORLD, load(hostile));
    EXPECT_EQ(VN_CURRENT_WORLD_LOAD_TOO_LARGE, report.status);
    EXPECT_EQ(harken::kCurrentWorldArtifactMaxBodyBytes + 1,
              report.bodyBytes);

    hostile = canonical;
    WriteU64(hostile, 24,
             ReadU64(canonical, 24) + static_cast<std::uint64_t>(1));
    EXPECT_EQ(VN_BAD_CURRENT_WORLD, load(hostile));
    EXPECT_EQ(VN_CURRENT_WORLD_LOAD_TRUNCATED, report.status);

    hostile = canonical;
    hostile.push_back(0);
    EXPECT_EQ(VN_BAD_CURRENT_WORLD, load(hostile));
    EXPECT_EQ(VN_CURRENT_WORLD_LOAD_SIZE_MISMATCH, report.status);

    hostile = canonical;
    hostile.back() ^= 1;
    EXPECT_EQ(VN_BAD_CURRENT_WORLD, load(hostile));
    EXPECT_EQ(VN_CURRENT_WORLD_LOAD_CHECKSUM_MISMATCH, report.status);
    EXPECT_EQ(0u, report.runtimeSemanticFingerprint);

    hostile = canonical;
    WriteU32(hostile,
             harken::kCurrentWorldArtifactHeaderBytes +
                 harken::kCurrentWorldV3PrefixWireBytes,
             harken::kHardActionDefinitionCapacity + 1u);
    WriteU64(hostile, 32,
             harken::Fnv1a64(
                 hostile.data() + harken::kCurrentWorldArtifactHeaderBytes,
                 hostile.size() -
                     harken::kCurrentWorldArtifactHeaderBytes));
    EXPECT_EQ(VN_BAD_CURRENT_WORLD, load(hostile));
    EXPECT_EQ(VN_CURRENT_WORLD_LOAD_DECODE_FAILED, report.status);
    EXPECT_EQ(0u, report.runtimeSemanticFingerprint);

    hostile = canonical;
    WriteU64(hostile, 40, ReadU64(hostile, 40) ^ 1);
    EXPECT_EQ(VN_BAD_CURRENT_WORLD, load(hostile));
    EXPECT_EQ(VN_CURRENT_WORLD_LOAD_SEMANTIC_INVALID, report.status);
    EXPECT_EQ(compiled.world->View().identity.runtimeSemanticFingerprint,
              report.runtimeSemanticFingerprint);
    EXPECT_EQ(0u, session.Engine()->variables.CurrentWorldGeneration());

    std::filesystem::remove(path, ignored);
}

namespace {

harkencurrentworldcompiler::CompileResultV1 CompileSetValue(
    harkenstudiobuild::VariableKindV1 kind, harkenstudiobuild::VariableScopeV1 scope, float rangeMin,
    float rangeMax, float value, harkenstudio::ObjectId variableId = harkentest::StudioObjectId(90),
    std::uint64_t targetId = 90) {
    harkenstudiobuild::VariableDefinitionV1 variable{};
    variable.variableId = variableId;
    variable.kind = kind;
    variable.rangeMin = rangeMin;
    variable.rangeMax = rangeMax;

    harkencurrentworldcompiler::ActionProgramAuthoringV1 action{};
    action.actionStableId = harkentest::StudioObjectId(41);
    harkencurrentworldcompiler::ActionStepAuthoringV1 step{};
    step.opcode = harkenstudiobuild::ActionOpcodeV1::SetVariable;
    step.target.kind = harkenstudiobuild::ActionTargetKindV1::Variable;
    step.target.stableId = harkentest::StudioObjectId(targetId);
    step.value = value;
    step.scope = static_cast<harkenstudiobuild::ActionScopeV1>(scope);
    action.steps.push_back(step);

    return harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {variable}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)},
        {}, {}, {action});
}

}  // namespace

TEST(CurrentWorldCompiler, SetVariableUsesStepScopeAndRejectsClamp) {
    const auto compiled = CompileSetValue(
        harkenstudiobuild::VariableKindV1::Smooth, harkenstudiobuild::VariableScopeV1::Emitter, -1.0f,
        1.0f, 0.25f);
    ASSERT_TRUE(compiled.ok()) << static_cast<std::uint32_t>(compiled.status);
    const auto& authoredWorld = compiled.world->View();
    const auto& compiledStep = authoredWorld.actions[0].steps[0];
    EXPECT_EQ(harkenstudiobuild::ActionScopeV1::Emitter, compiledStep.scope);
    EXPECT_EQ(std::bit_cast<std::uint32_t>(0.25f), compiledStep.valueBits);
    EXPECT_EQ(0u, compiledStep.target.expectedProgramSemanticFingerprint);

    std::vector<std::uint8_t> bytes;
    ASSERT_TRUE(harkenstudiobuild::EncodeCurrentWorldShippingBody(authoredWorld, bytes));
    harken::CurrentWorldBodyImage decoded;
    ASSERT_TRUE(decoded.Decode(bytes.data(), bytes.size(),
                               harken::kCurrentWorldShippingBodyVintage,
                               harken::CurrentWorldDecodeMode::StrictArtifact));
    ASSERT_EQ(1u, decoded.ProductionView()->variableCount);
    EXPECT_EQ(harken::ActionScopeV1::Emitter,
              decoded.ProductionView()->actions[0].steps[0].scope);
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::ValidateCompilerWorld(*decoded.ProductionView()));
}

TEST(CurrentWorldCompiler, SetValueOutOfRangeIsInvalidAuthoring) {
    const auto compiled = CompileSetValue(
        harkenstudiobuild::VariableKindV1::Smooth, harkenstudiobuild::VariableScopeV1::Emitter, -1.0f,
        1.0f, 4.0f);
    EXPECT_EQ(harkencurrentworldcompiler::CompileStatusV1::InvalidActionAuthoring,
              compiled.status);
    EXPECT_EQ(nullptr, compiled.world);
}

TEST(CurrentWorldCompiler, SetValueDiscreteNonIntegerIsInvalidAuthoring) {
    const auto compiled = CompileSetValue(
        harkenstudiobuild::VariableKindV1::Step, harkenstudiobuild::VariableScopeV1::Global, 0.0f, 3.0f,
        1.5f);
    EXPECT_EQ(harkencurrentworldcompiler::CompileStatusV1::InvalidActionAuthoring,
              compiled.status);
    EXPECT_EQ(nullptr, compiled.world);
}

TEST(CurrentWorldCompiler, SetValueMissingVariableIsInvalidAuthoring) {
    const auto compiled = CompileSetValue(
        harkenstudiobuild::VariableKindV1::Smooth, harkenstudiobuild::VariableScopeV1::Global, -1.0f,
        1.0f, 0.25f, harkentest::StudioObjectId(90), 91);
    EXPECT_EQ(harkencurrentworldcompiler::CompileStatusV1::InvalidActionAuthoring,
              compiled.status);
    EXPECT_EQ(nullptr, compiled.world);
}

TEST(CurrentWorldCompiler, StopCanonicalizesTimingPolicyAndSelfReference) {
    harkencurrentworldcompiler::ActionProgramAuthoringV1 action{};
    action.actionStableId = harkentest::StudioObjectId(42);
    harkencurrentworldcompiler::ActionStepAuthoringV1 step{};
    step.opcode = harkenstudiobuild::ActionOpcodeV1::Stop;
    step.target.kind = harkenstudiobuild::ActionTargetKindV1::Container;
    step.target.stableId = harkentest::StudioObjectId(2);
    step.delayMilliseconds = 1.25f;
    step.fadeMilliseconds = 2.5f;
    step.quantizeTiming = harkenstudiobuild::ActionQuantizeTimingV1::NaturalEnd;
    step.cooldownScope = harkenstudiobuild::ActionCooldownScopeV1::PerEmitter;
    step.ifSilent = harkenstudiobuild::ActionIfSilentV1::Drop;
    step.cooldownSeconds = 0.25f;
    action.steps.push_back(step);

    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)},
        {}, {}, {action});
    ASSERT_TRUE(compiled.ok()) << static_cast<std::uint32_t>(compiled.status);
    const auto& world = compiled.world->View();
    const auto& result = world.actions[0];
    ASSERT_EQ(1u, result.stepCount);
    const auto& canonical = result.steps[0];
    EXPECT_EQ(INT64_C(1250000), canonical.delay.numerator);
    EXPECT_EQ(INT64_C(2500000), canonical.fade.numerator);
    EXPECT_EQ(INT64_C(250000000), canonical.cooldown.numerator);
    EXPECT_EQ(1u, canonical.delay.denominator);
    EXPECT_EQ(1u, canonical.fade.denominator);
    EXPECT_EQ(1u, canonical.cooldown.denominator);
    EXPECT_EQ(harkenstudiobuild::ActionQuantizeTimingV1::NaturalEnd,
              canonical.quantizeTiming);
    EXPECT_EQ(harkentest::StudioObjectId(0u), canonical.quantizeReference.stableId);
    EXPECT_EQ(harkenstudiobuild::ActionCooldownScopeV1::PerEmitter,
              canonical.cooldownScope);
    EXPECT_EQ(harkenstudiobuild::ActionIfSilentV1::Drop, canonical.ifSilent);
    EXPECT_EQ(15u, result.directFeatureMask);
    EXPECT_EQ(15u, result.transitiveFeatureMask);
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::ValidateCompilerWorld(world));
}

TEST(CurrentWorldCompiler, ResetIgnoresInapplicableResidue) {
    harkencurrentworldcompiler::ActionProgramAuthoringV1 action{};
    action.actionStableId = harkentest::StudioObjectId(43);
    harkencurrentworldcompiler::ActionStepAuthoringV1 step{};
    step.opcode = harkenstudiobuild::ActionOpcodeV1::Reset;
    step.target.kind = harkenstudiobuild::ActionTargetKindV1::Container;
    step.target.stableId = harkentest::StudioObjectId(2);
    step.delayMilliseconds = 0.5f;
    step.fadeMilliseconds = std::numeric_limits<float>::quiet_NaN();
    step.quantizeReference.kind = harkenstudiobuild::ActionTargetKindV1::Bus;
    step.quantizeReference.stableId = harkentest::StudioObjectId(UINT64_MAX);
    step.quantizeTiming = harkenstudiobuild::ActionQuantizeTimingV1::ExitMarker;
    step.cooldownScope = harkenstudiobuild::ActionCooldownScopeV1::PerEmitter;
    step.ifSilent = harkenstudiobuild::ActionIfSilentV1::Drop;
    step.resetScope = harkenstudiobuild::ActionScopeV1::Emitter;
    step.cooldownSeconds = std::numeric_limits<float>::quiet_NaN();
    step.value = std::numeric_limits<float>::quiet_NaN();
    action.steps.push_back(step);

    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)},
        {}, {}, {action});
    ASSERT_TRUE(compiled.ok()) << static_cast<std::uint32_t>(compiled.status);
    const auto& world = compiled.world->View();
    const auto& result = world.actions[0];
    const auto& canonical = result.steps[0];
    EXPECT_EQ(INT64_C(500000), canonical.delay.numerator);
    EXPECT_EQ(INT64_C(0), canonical.fade.numerator);
    EXPECT_EQ(INT64_C(0), canonical.cooldown.numerator);
    EXPECT_EQ(harkenstudiobuild::ActionQuantizeTimingV1::Immediate,
              canonical.quantizeTiming);
    EXPECT_EQ(harkentest::StudioObjectId(0u), canonical.quantizeReference.stableId);
    EXPECT_EQ(harkenstudiobuild::ActionCooldownScopeV1::Global,
              canonical.cooldownScope);
    EXPECT_EQ(harkenstudiobuild::ActionIfSilentV1::Immediate, canonical.ifSilent);
    EXPECT_EQ(harkenstudiobuild::ActionScopeV1::Emitter, canonical.scope);
    EXPECT_EQ(0u, canonical.valueBits);
    EXPECT_EQ(1u, result.directFeatureMask);
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::ValidateCompilerWorld(world));
}

TEST(CurrentWorldCompiler, PostActionDerivesChildFirstFingerprintAndWork) {
    harkencurrentworldcompiler::ActionProgramAuthoringV1 parent{};
    parent.actionStableId = harkentest::StudioObjectId(50);
    harkencurrentworldcompiler::ActionStepAuthoringV1 post{};
    post.opcode = harkenstudiobuild::ActionOpcodeV1::PostAction;
    post.target.kind = harkenstudiobuild::ActionTargetKindV1::Action;
    post.target.stableId = harkentest::StudioObjectId(40);
    parent.steps.push_back(post);

    harkencurrentworldcompiler::ActionProgramAuthoringV1 child{};
    child.actionStableId = harkentest::StudioObjectId(40);
    harkencurrentworldcompiler::ActionStepAuthoringV1 play{};
    play.opcode = harkenstudiobuild::ActionOpcodeV1::Play;
    play.target.kind = harkenstudiobuild::ActionTargetKindV1::Sound;
    play.target.stableId = harkentest::StudioObjectId(3);
    child.steps.push_back(play);

    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)},
        {}, {}, {parent, child});
    ASSERT_TRUE(compiled.ok()) << static_cast<std::uint32_t>(compiled.status);
    const auto& world = compiled.world->View();
    ASSERT_EQ(2u, world.actionCount);
    const auto& canonicalChild = world.actions[0];
    const auto& canonicalParent = world.actions[1];
    ASSERT_EQ(harkentest::StudioObjectId(40u), canonicalChild.actionStableId);
    ASSERT_EQ(harkentest::StudioObjectId(50u), canonicalParent.actionStableId);
    EXPECT_EQ(canonicalChild.programSemanticFingerprint,
              canonicalParent.steps[0].target
                  .expectedProgramSemanticFingerprint);
    EXPECT_EQ(2u, canonicalParent.expandedStepOccurrenceCount);
    EXPECT_EQ(2u, canonicalParent.expandedFiringCount);
    EXPECT_EQ(canonicalChild.transitiveFeatureMask,
              canonicalParent.transitiveFeatureMask);
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::ValidateCompilerWorld(world));
}

TEST(CurrentWorldCompiler, ImmediatePlayClearsInapplicableResidue) {
    harkencurrentworldcompiler::ActionProgramAuthoringV1 action{};
    action.actionStableId = harkentest::StudioObjectId(44);
    harkencurrentworldcompiler::ActionStepAuthoringV1 step{};
    step.opcode = harkenstudiobuild::ActionOpcodeV1::Play;
    step.target.kind = harkenstudiobuild::ActionTargetKindV1::Sound;
    step.target.stableId = harkentest::StudioObjectId(3);
    step.delayMilliseconds = 0.125f;
    step.fadeMilliseconds = std::numeric_limits<float>::quiet_NaN();
    step.quantizeReference.kind = harkenstudiobuild::ActionTargetKindV1::Bus;
    step.quantizeReference.stableId = harkentest::StudioObjectId(UINT64_MAX);
    step.cooldownScope = harkenstudiobuild::ActionCooldownScopeV1::PerEmitter;
    step.ifSilent = harkenstudiobuild::ActionIfSilentV1::Drop;
    step.resetScope = harkenstudiobuild::ActionScopeV1::Emitter;
    step.value = std::numeric_limits<float>::quiet_NaN();
    action.steps.push_back(step);

    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)},
        {}, {}, {action});
    ASSERT_TRUE(compiled.ok()) << static_cast<std::uint32_t>(compiled.status);
    const auto& world = compiled.world->View();
    const auto& result = world.actions[0];
    const auto& canonical = result.steps[0];
    EXPECT_EQ(INT64_C(125000), canonical.delay.numerator);
    EXPECT_EQ(INT64_C(0), canonical.fade.numerator);
    EXPECT_EQ(harkentest::StudioObjectId(0u), canonical.quantizeReference.stableId);
    EXPECT_EQ(harkenstudiobuild::ActionCooldownScopeV1::Global,
              canonical.cooldownScope);
    EXPECT_EQ(harkenstudiobuild::ActionIfSilentV1::Immediate, canonical.ifSilent);
    EXPECT_EQ(harkenstudiobuild::ActionScopeV1::Global, canonical.scope);
    EXPECT_EQ(0u, canonical.valueBits);
    EXPECT_EQ(1u, result.directFeatureMask);
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::ValidateCompilerWorld(world));
}

TEST(CurrentWorldCompiler, ImmediatePostActionCanonicalizesPolicyAndPropagatesFeatures) {
    harkencurrentworldcompiler::ActionProgramAuthoringV1 parent{};
    parent.actionStableId = harkentest::StudioObjectId(51);
    harkencurrentworldcompiler::ActionStepAuthoringV1 post{};
    post.opcode = harkenstudiobuild::ActionOpcodeV1::PostAction;
    post.target.kind = harkenstudiobuild::ActionTargetKindV1::Action;
    post.target.stableId = harkentest::StudioObjectId(40);
    post.delayMilliseconds = 1.0f;
    post.fadeMilliseconds = std::numeric_limits<float>::quiet_NaN();
    post.cooldownScope = harkenstudiobuild::ActionCooldownScopeV1::PerEmitter;
    post.cooldownSeconds = 0.5f;
    parent.steps.push_back(post);

    harkencurrentworldcompiler::ActionProgramAuthoringV1 child{};
    child.actionStableId = harkentest::StudioObjectId(40);
    harkencurrentworldcompiler::ActionStepAuthoringV1 play{};
    play.target.kind = harkenstudiobuild::ActionTargetKindV1::Sound;
    play.target.stableId = harkentest::StudioObjectId(3);
    child.steps.push_back(play);

    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)},
        {}, {}, {parent, child});
    ASSERT_TRUE(compiled.ok()) << static_cast<std::uint32_t>(compiled.status);
    const auto& world = compiled.world->View();
    const auto& canonicalParent = world.actions[1];
    const auto& canonical = canonicalParent.steps[0];
    EXPECT_EQ(INT64_C(1000000), canonical.delay.numerator);
    EXPECT_EQ(INT64_C(0), canonical.fade.numerator);
    EXPECT_EQ(INT64_C(500000000), canonical.cooldown.numerator);
    EXPECT_EQ(harkenstudiobuild::ActionQuantizeTimingV1::Immediate,
              canonical.quantizeTiming);
    EXPECT_EQ(harkentest::StudioObjectId(0u), canonical.quantizeReference.stableId);
    EXPECT_EQ(13u, canonicalParent.directFeatureMask);
    EXPECT_EQ(13u, canonicalParent.transitiveFeatureMask);
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::ValidateCompilerWorld(world));
}

TEST(CurrentWorldCompiler, RejectsQuantizedPostAction) {
    harkencurrentworldcompiler::ActionProgramAuthoringV1 parent{};
    parent.actionStableId = harkentest::StudioObjectId(51);
    harkencurrentworldcompiler::ActionStepAuthoringV1 post{};
    post.opcode = harkenstudiobuild::ActionOpcodeV1::PostAction;
    post.target.kind = harkenstudiobuild::ActionTargetKindV1::Action;
    post.target.stableId = harkentest::StudioObjectId(40);
    post.quantizeReference.kind = harkenstudiobuild::ActionTargetKindV1::Sound;
    post.quantizeReference.stableId = harkentest::StudioObjectId(3);
    post.quantizeTiming = harkenstudiobuild::ActionQuantizeTimingV1::NextBeat;
    parent.steps.push_back(post);

    harkencurrentworldcompiler::ActionProgramAuthoringV1 child{};
    child.actionStableId = harkentest::StudioObjectId(40);
    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)},
        {}, {}, {parent, child});
    EXPECT_EQ(harkencurrentworldcompiler::CompileStatusV1::InvalidActionAuthoring,
              compiled.status);
    EXPECT_FALSE(compiled.ok());
}

TEST(CurrentWorldCompiler, PostActionDiamondAndRepeatedEdgesCountMultiplicity) {
    const auto post = [](std::uint64_t target) {
        harkencurrentworldcompiler::ActionStepAuthoringV1 step{};
        step.opcode = harkenstudiobuild::ActionOpcodeV1::PostAction;
        step.target.kind = harkenstudiobuild::ActionTargetKindV1::Action;
        step.target.stableId = harkentest::StudioObjectId(target);
        return step;
    };
    harkencurrentworldcompiler::ActionProgramAuthoringV1 leaf{};
    leaf.actionStableId = harkentest::StudioObjectId(10);
    harkencurrentworldcompiler::ActionStepAuthoringV1 play{};
    play.target.kind = harkenstudiobuild::ActionTargetKindV1::Sound;
    play.target.stableId = harkentest::StudioObjectId(3);
    leaf.steps.push_back(play);

    harkencurrentworldcompiler::ActionProgramAuthoringV1 left{};
    left.actionStableId = harkentest::StudioObjectId(20);
    left.steps.push_back(post(10));
    harkencurrentworldcompiler::ActionProgramAuthoringV1 right{};
    right.actionStableId = harkentest::StudioObjectId(30);
    right.steps.push_back(post(10));
    harkencurrentworldcompiler::ActionProgramAuthoringV1 root{};
    root.actionStableId = harkentest::StudioObjectId(40);
    root.steps = {post(20), post(30), post(10)};
    for (std::uint32_t i = 0; i < root.steps.size(); ++i) {
        root.steps[i].authoredEventIndex = i;
    }

    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)},
        {}, {}, {root, right, leaf, left});
    ASSERT_TRUE(compiled.ok()) << static_cast<std::uint32_t>(compiled.status);
    const auto& world = compiled.world->View();
    ASSERT_EQ(4u, world.actionCount);
    EXPECT_EQ(8u, world.actions[3].expandedStepOccurrenceCount);
    EXPECT_EQ(6u, world.actions[3].expandedFiringCount);
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::ValidateCompilerWorld(world));
}

TEST(CurrentWorldCompiler, PostActionCycleIsInvalidAuthoring) {
    const auto action = [](std::uint64_t id, std::uint64_t target) {
        harkencurrentworldcompiler::ActionProgramAuthoringV1 row{};
        row.actionStableId = harkentest::StudioObjectId(id);
        harkencurrentworldcompiler::ActionStepAuthoringV1 step{};
        step.opcode = harkenstudiobuild::ActionOpcodeV1::PostAction;
        step.target.kind = harkenstudiobuild::ActionTargetKindV1::Action;
        step.target.stableId = harkentest::StudioObjectId(target);
        row.steps.push_back(step);
        return row;
    };
    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)},
        {}, {}, {action(10, 20), action(20, 10)});
    EXPECT_EQ(harkencurrentworldcompiler::CompileStatusV1::InvalidActionAuthoring,
              compiled.status);
    EXPECT_EQ(harkentest::StudioObjectId(10u), compiled.relatedStableId);
    EXPECT_EQ(nullptr, compiled.world);
}

TEST(CurrentWorldCompiler, PostActionDepthIsRejectedBeforeChildTraversal) {
    const auto action = [](std::uint64_t id, std::uint64_t target) {
        harkencurrentworldcompiler::ActionProgramAuthoringV1 row{};
        row.actionStableId = harkentest::StudioObjectId(id);
        harkencurrentworldcompiler::ActionStepAuthoringV1 step{};
        step.opcode = harkenstudiobuild::ActionOpcodeV1::PostAction;
        step.target.kind = harkenstudiobuild::ActionTargetKindV1::Action;
        step.target.stableId = harkentest::StudioObjectId(target);
        row.steps.push_back(step);
        return row;
    };
    harkenstudiobuild::WorldCapacitySummaryV1 capacity{};
    capacity.maxActionTriggerDepth = 1;
    capacity.maxActionExpandedWork = harken::kMaxActionSteps;
    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)},
        capacity, {}, {action(10, 20), action(20, 30)});

    EXPECT_EQ(harkencurrentworldcompiler::CompileStatusV1::Capacity,
              compiled.status);
    EXPECT_EQ(harkentest::StudioObjectId(10u), compiled.relatedStableId);
    EXPECT_EQ(nullptr, compiled.world);
}

TEST(CurrentWorldCompiler, VariableControlGridAuthoringLowersExactly) {
    harkenstudiobuild::VariableDefinitionV1 variable{};
    variable.variableId = harkentest::StudioObjectId(91);
    variable.kind = harkenstudiobuild::VariableKindV1::Step;
    variable.rangeMin = 0.0f;
    variable.rangeMax = 10.0f;
    variable.defaultValue = 0.0f;
    auto authored = Authored(harkenstudiobuild::ContainerModeV1::VariableControl,
                             harkenstudiobuild::RandomDisciplineV1::Draw);
    authored.discreteAxes.push_back({0, 0, variable.variableId});
    harkencurrentworldcompiler::DiscreteGridRowAuthoringV1 row{};
    row.stableRowOrdinal = 4;
    row.authoredStableOrder = 5;
    row.childStableOrdinal = 7;
    row.cells.push_back(
        {harkenstudiobuild::DiscreteCellKindV1::Exact, 3});
    authored.discreteRows.push_back(row);

    const auto result = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {variable}, Materializations(), {authored});
    ASSERT_TRUE(result.ok());
    const auto& world = result.world->View();
    ASSERT_EQ(1u, world.containerCount);
    const auto& container = world.containers[0];
    ASSERT_EQ(1u, container.discreteAxisCount);
    ASSERT_EQ(1u, container.discreteRowCount);
    EXPECT_EQ(variable.variableId, container.discreteAxes[0].variableId);
    EXPECT_EQ(3, container.discreteRows[0].cells[0].exactValue);
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::ValidateCompilerWorld(world));
    std::vector<std::uint8_t> encoded;
    ASSERT_TRUE(harkenstudiobuild::EncodeCurrentWorldShippingBody(world, encoded));
    harken::CurrentWorldBodyImage decoded;
    ASSERT_TRUE(decoded.Decode(encoded.data(), encoded.size(), harken::kCurrentWorldShippingBodyVintage,
        harken::CurrentWorldDecodeMode::StrictArtifact));
    ASSERT_NE(nullptr, decoded.ProductionView());
    std::vector<std::uint8_t> runtimeEncoded;
    ASSERT_TRUE(harken::EncodeCurrentWorldShippingBody(*decoded.ProductionView(), runtimeEncoded));
    EXPECT_EQ(encoded, runtimeEncoded);
    const auto* before = decoded.ProductionView();
    EXPECT_FALSE(decoded.Decode(encoded.data(), encoded.size(), harken::kCurrentWorldBodyVintageV15,
        harken::CurrentWorldDecodeMode::StrictArtifact));
    EXPECT_EQ(before, decoded.ProductionView());
}

TEST(CurrentWorldCompiler,
     VariableControlCellsFollowAxesIntoCanonicalStableOrder) {
    harkenstudiobuild::VariableDefinitionV1 variables[2]{};
    for (std::uint32_t i = 0; i < 2; ++i) {
        variables[i].variableId = harkentest::StudioObjectId(91 + i);
        variables[i].kind = harkenstudiobuild::VariableKindV1::Step;
        variables[i].rangeMin = 0.0f;
        variables[i].rangeMax = 10.0f;
        variables[i].defaultValue = 0.0f;
    }
    auto authored = Authored(harkenstudiobuild::ContainerModeV1::VariableControl,
                             harkenstudiobuild::RandomDisciplineV1::Draw);
    authored.discreteAxes.push_back({20, 0, variables[0].variableId});
    authored.discreteAxes.push_back({10, 1, variables[1].variableId});
    harkencurrentworldcompiler::DiscreteGridRowAuthoringV1 row{};
    row.childStableOrdinal = 7;
    row.cells.push_back({harkenstudiobuild::DiscreteCellKindV1::Exact, 3});
    row.cells.push_back({harkenstudiobuild::DiscreteCellKindV1::Exact, 4});
    authored.discreteRows.push_back(row);

    const auto result = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {variables[0], variables[1]}, Materializations(),
        {authored});
    ASSERT_TRUE(result.ok());
    const auto& container = result.world->View().containers[0];
    ASSERT_EQ(2u, container.discreteAxisCount);
    ASSERT_EQ(2u, container.discreteRows[0].cellCount);
    EXPECT_EQ(10u, container.discreteAxes[0].stableAxisOrdinal);
    EXPECT_EQ(1u, container.discreteAxes[0].authoredColumnOrder);
    EXPECT_EQ(4, container.discreteRows[0].cells[0].exactValue);
    EXPECT_EQ(20u, container.discreteAxes[1].stableAxisOrdinal);
    EXPECT_EQ(0u, container.discreteAxes[1].authoredColumnOrder);
    EXPECT_EQ(3, container.discreteRows[0].cells[1].exactValue);
}

TEST(CurrentWorldCompiler,
     VariableControlCurveCannotResolveFromAnotherContainer) {
    harkenprojectschema::CanonicalProject project{};
    project.objects.push_back(Layer(1, {2, 4}));
    project.objects.push_back(Container(2, 3));
    project.objects.push_back(Sound(3));
    project.objects.push_back(Container(4, 5));
    project.objects.push_back(Sound(5));
    project.roots = {harkentest::StudioObjectId(1)};
    const std::vector<harkencurrentworldcompiler::DefinitionMaterializationV1>
        materializations = {
            {harkenplayableproperties::ContentPropertyKindV1::Container, harkentest::StudioObjectId(2), 22,
             harkentest::StudioObjectId(20), 200},
            {harkenplayableproperties::ContentPropertyKindV1::Sound, harkentest::StudioObjectId(3), 30, harkentest::StudioObjectId(21),
             201},
            {harkenplayableproperties::ContentPropertyKindV1::Container, harkentest::StudioObjectId(4), 44,
             harkentest::StudioObjectId(40), 400},
            {harkenplayableproperties::ContentPropertyKindV1::Sound, harkentest::StudioObjectId(5), 50, harkentest::StudioObjectId(41),
             401},
        };
    harkenstudiobuild::VariableDefinitionV1 variable{};
    variable.variableId = harkentest::StudioObjectId(91);
    variable.kind = harkenstudiobuild::VariableKindV1::Smooth;
    variable.rangeMin = 0.0f;
    variable.rangeMax = 10.0f;
    variable.defaultValue = 0.0f;
    const auto makeAuthoring = [&](std::uint64_t id, bool ownsCurve) {
        harkencurrentworldcompiler::ContainerAuthoringV1 authored{};
        authored.containerStableId = harkentest::StudioObjectId(id);
        authored.mode = harkenstudiobuild::ContainerModeV1::VariableControl;
        authored.evaluationPeriodFrames = 64;
        authored.maxActiveRows = 1;
        authored.maxLiveRows = 1;
        authored.continuousAxes.push_back({0, 0, variable.variableId});
        harkencurrentworldcompiler::ContinuousGridRowAuthoringV1 row{};
        row.childStableOrdinal = 7;
        harkenstudiobuild::ContinuousGridCellV1 cell{};
        cell.kind = harkenstudiobuild::ContinuousCellKindV1::Interval;
        cell.lower = 0.0f;
        cell.upper = 10.0f;
        cell.lowerInclusive = 1;
        cell.upperInclusive = 1;
        cell.leftCurveLocalId = 42;
        row.cells.push_back(cell);
        authored.continuousRows.push_back(row);
        harkencurrentworldcompiler::ContainerChildAuthoringV1 child{};
        child.stableOccurrenceOrdinal = 7;
        authored.children.push_back(child);
        if (ownsCurve) {
            harkenstudiobuild::CurveV1 curve{};
            curve.curveLocalId = 42;
            curve.curve.pointCount = 2;
            curve.curve.points[0] = {0.0f, 0.0f};
            curve.curve.points[1] = {10.0f, 1.0f};
            authored.curves.push_back(curve);
        }
        return authored;
    };

    const auto result = harkencurrentworldcompiler::CompileCurrentWorldV1(
        project, {variable}, materializations,
        {makeAuthoring(2, false), makeAuthoring(4, true)});
    EXPECT_EQ(harkencurrentworldcompiler::CompileStatusV1::InvalidCanonicalWorld,
              result.status);
    EXPECT_EQ(nullptr, result.world);
}

TEST(CurrentWorldCompiler, FiveDefinitionKindsAndTrackClipCompileTogether) {
    harkenprojectschema::CanonicalProject project{};
    project.objects.push_back(Layer(1, {2, 3}));
    project.objects.push_back(Container(2, 6));

    harkenprojectschema::ContentObject piece{};
    piece.kind = harkenprojectschema::ContentKind::Piece;
    piece.piece.chassis = ResolvedChassis(3);
    piece.piece.tracks = {harkentest::StudioObjectId(4)};
    project.objects.push_back(piece);

    harkenprojectschema::ContentObject track{};
    track.kind = harkenprojectschema::ContentKind::Track;
    track.track.chassis = ResolvedChassis(4);
    harkenprojectschema::Clip clip{};
    clip.id = harkentest::StudioObjectId(5);
    clip.guid = {50, 51};
    clip.stableOccurrenceOrdinal = 9;
    clip.contentRef.kind = harkenprojectschema::ContentKind::Sound;
    clip.contentRef.target = harkentest::StudioObjectId(6);
    clip.time = 120;
    clip.fades = {10, 20};
    clip.presence.Set(harkenplayableproperties::PlayablePropertyIdV1::Volume);
    clip.volume = 0.0f;
    track.track.clips.push_back(clip);
    project.objects.push_back(track);
    project.objects.push_back(Sound(6));
    project.roots = {harkentest::StudioObjectId(1)};

    const std::vector<harkencurrentworldcompiler::DefinitionMaterializationV1>
        materializations = {
            {harkenplayableproperties::ContentPropertyKindV1::Container, harkentest::StudioObjectId(2), 22,
             harkentest::StudioObjectId(20), 200},
            {harkenplayableproperties::ContentPropertyKindV1::Piece, harkentest::StudioObjectId(3), 33, harkentest::StudioObjectId(0),
             300},
            {harkenplayableproperties::ContentPropertyKindV1::Track, harkentest::StudioObjectId(4), 44, harkentest::StudioObjectId(0),
             400},
            {harkenplayableproperties::ContentPropertyKindV1::Sound, harkentest::StudioObjectId(6), 60, harkentest::StudioObjectId(21),
             201},
        };
    const auto result = harkencurrentworldcompiler::CompileCurrentWorldV1(
        project, {}, materializations,
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)});
    ASSERT_TRUE(result.ok());
    const auto& world = result.world->View();
    ASSERT_EQ(5u, world.playablePropertyCount);
    ASSERT_EQ(2u, world.playableOccurrenceCount);
    const auto trackOccurrence = std::find_if(
        world.playableOccurrences,
        world.playableOccurrences + world.playableOccurrenceCount,
        [](const harkenstudiobuild::PlayableOccurrencePropertyViewV1& row) {
            return row.parentKind ==
                       harkenstudiobuild::playableproperties::ContentPropertyKindV1::Track &&
                   row.parentStableId == harkentest::StudioObjectId(4);
        });
    ASSERT_NE(world.playableOccurrences + world.playableOccurrenceCount,
              trackOccurrence);
    EXPECT_EQ(9u, trackOccurrence->stableOccurrenceOrdinal);
    EXPECT_EQ(harkenstudiobuild::PlayableKindV1::Sound,
              trackOccurrence->childPlayable.kind);
    EXPECT_EQ(harkentest::StudioObjectId(6u), trackOccurrence->childPlayable.stableId);
    EXPECT_EQ(UINT64_C(1), trackOccurrence->payload.presentMask);

    ASSERT_EQ(1u, world.trackScheduleCount);
    ASSERT_EQ(1u, world.pieceScheduleCount);
    const auto& trackSchedule = world.trackSchedules[0];
    EXPECT_EQ(harkentest::StudioObjectId(4u), trackSchedule.rootDefinition.stableId);
    ASSERT_EQ(1u, trackSchedule.placementCount);
    EXPECT_EQ(0u, trackSchedule.placements[0].stablePlacementOrdinal);
    EXPECT_EQ(9u, trackSchedule.placements[0].stableOccurrenceOrdinal);
    EXPECT_EQ(120, trackSchedule.placements[0].authoredStart);
    EXPECT_EQ(10, trackSchedule.placements[0].authoredFadeIn);
    EXPECT_EQ(20, trackSchedule.placements[0].authoredFadeOut);
    EXPECT_EQ(harkentest::StudioObjectId(5u), trackSchedule.placements[0].clipId);
    EXPECT_EQ(50u, trackSchedule.placements[0].clipGuidHigh);
    EXPECT_EQ(51u, trackSchedule.placements[0].clipGuidLow);
    const auto& pieceSchedule = world.pieceSchedules[0];
    EXPECT_EQ(harkentest::StudioObjectId(3u), pieceSchedule.rootDefinition.stableId);
    EXPECT_EQ(harkenstudiobuild::ScheduleTimeKindV1::AbsoluteNanoseconds,
              pieceSchedule.timeKind);
    ASSERT_EQ(1u, pieceSchedule.trackRefCount);
    EXPECT_EQ(harkentest::StudioObjectId(4u), pieceSchedule.trackRefs[0].trackDefinition.stableId);
    EXPECT_EQ(trackSchedule.scheduleSemanticFingerprint,
              pieceSchedule.trackRefs[0].trackScheduleSemanticFingerprint);
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::ValidateCompilerWorld(world));
}

TEST(CurrentWorldCompiler, ThreeUnit2ModesProduceExactCapability) {
    struct Case {
        harkenstudiobuild::ContainerModeV1 mode;
        harkenstudiobuild::RandomDisciplineV1 discipline;
        std::uint64_t capability;
    };
    const Case cases[] = {
        {harkenstudiobuild::ContainerModeV1::Random,
         harkenstudiobuild::RandomDisciplineV1::Draw,
         harken::kWorldCapabilityRandomDraw},
        {harkenstudiobuild::ContainerModeV1::Random,
         harkenstudiobuild::RandomDisciplineV1::Rotation,
         harken::kWorldCapabilityRandomRotation},
        {harkenstudiobuild::ContainerModeV1::Sequence,
         harkenstudiobuild::RandomDisciplineV1::Draw,
         harken::kWorldCapabilitySequence},
    };
    for (const Case& test : cases) {
        const auto result = harkencurrentworldcompiler::CompileCurrentWorldV1(
            Project(), {}, Materializations(),
            {Authored(test.mode, test.discipline)});
        ASSERT_TRUE(result.ok());
        EXPECT_EQ(harken::kWorldCapabilityPlayableProperties | test.capability,
                  result.world->View().identity.capabilityMask);
    }
}

TEST(CurrentWorldCompiler, StableChildIdentityIsNeverInferredFromPosition) {
    auto authored = Authored(harkenstudiobuild::ContainerModeV1::Random,
                             harkenstudiobuild::RandomDisciplineV1::Draw);
    authored.children[0].stableOccurrenceOrdinal = 0;
    const auto result = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(), {authored});
    EXPECT_EQ(
        harkencurrentworldcompiler::CompileStatusV1::InvalidContainerAuthoring,
        result.status) << " related " << result.relatedStableId;
    EXPECT_EQ(harkentest::StudioObjectId(2u), result.relatedStableId);
    EXPECT_EQ(nullptr, result.world);
}

TEST(CurrentWorldCompiler, InactiveCompletionAuthoringIsNeverSilentlyDropped) {
    auto nonQueue = Authored(harkenstudiobuild::ContainerModeV1::Sequence,
                             harkenstudiobuild::RandomDisciplineV1::Draw);
    nonQueue.children[0].completion.completionClass =
        harkenstudiobuild::AuthoredCompletionClassV1::FiniteMediaCandidate;
    auto result = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(), {nonQueue});
    EXPECT_EQ(
        harkencurrentworldcompiler::CompileStatusV1::InvalidContainerAuthoring,
        result.status);
    EXPECT_EQ(nullptr, result.world);

    harkenprojectschema::CanonicalProject project{};
    auto parent = Container(2, 4);
    parent.container.children[0].childPlayableRef.kind =
        harkenprojectschema::ContentKind::Container;
    project.objects.push_back(parent);
    project.objects.push_back(Sound(3));
    project.objects.push_back(Container(4, 3));
    project.roots = {harkentest::StudioObjectId(2)};
    auto parentAuthoring = Authored(harkenstudiobuild::ContainerModeV1::RandomQueue,
                                    harkenstudiobuild::RandomDisciplineV1::Draw);
    parentAuthoring.children[0].orderWeight = 1;
    parentAuthoring.children[0].completion.completionClass =
        harkenstudiobuild::AuthoredCompletionClassV1::FiniteMediaCandidate;
    auto childAuthoring = Authored(harkenstudiobuild::ContainerModeV1::Random,
                                   harkenstudiobuild::RandomDisciplineV1::Draw);
    childAuthoring.containerStableId = harkentest::StudioObjectId(4);
    result = harkencurrentworldcompiler::CompileCurrentWorldV1(
        project, {},
        {{harkenplayableproperties::ContentPropertyKindV1::Container, harkentest::StudioObjectId(2), 22, harkentest::StudioObjectId(0),
          200},
         {harkenplayableproperties::ContentPropertyKindV1::Container, harkentest::StudioObjectId(4), 44, harkentest::StudioObjectId(0),
          400},
         {harkenplayableproperties::ContentPropertyKindV1::Sound, harkentest::StudioObjectId(3), 33, harkentest::StudioObjectId(0),
          300}},
        {parentAuthoring, childAuthoring});
    EXPECT_EQ(
        harkencurrentworldcompiler::CompileStatusV1::InvalidContainerAuthoring,
        result.status) << " related " << result.relatedStableId;
    EXPECT_EQ(nullptr, result.world);
}

TEST(CurrentWorldCompiler, CanonicalQueueModeIsInstallable) {
    auto authored = Authored(harkenstudiobuild::ContainerModeV1::RandomQueue,
                             harkenstudiobuild::RandomDisciplineV1::Draw);
    authored.children[0].orderWeight = 1;
    const auto result = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(), {authored});
    ASSERT_TRUE(result.ok());
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::ValidateCompilerWorld(result.world->View()));
}

TEST(CurrentWorldCompiler, BasicTransitionIsDeepOwnedAndAdvancedIsRefused) {
    auto authored = Authored(harkenstudiobuild::ContainerModeV1::SequenceQueue,
                             harkenstudiobuild::RandomDisciplineV1::Draw);
    authored.transitionProgramIndex = 0;
    authored.curves = {FadeCurve(1), FadeCurve(2)};

    harkencurrentworldcompiler::TransitionProgramAuthoringV1 transition{};
    transition.ownerContainerStableId = authored.containerStableId;
    harkenstudiobuild::TransitionRuleViewV1 rule{};
    rule.sourceAny = 1;
    rule.targetAny = 1;
    rule.sourceFade.active = 1;
    rule.sourceFade.durationFrames = 48;
    rule.sourceFade.curveLocalId = 1;
    rule.bridgeFadeIn.curveLocalId = UINT32_MAX;
    rule.bridgeFadeOut.curveLocalId = UINT32_MAX;
    rule.targetFade.active = 1;
    rule.targetFade.durationFrames = 96;
    rule.targetFade.curveLocalId = 2;
    transition.rules.push_back(rule);

    auto result = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(), {authored}, {}, {transition});
    ASSERT_TRUE(result.ok()) << static_cast<std::uint32_t>(result.status);
    ASSERT_NE(nullptr, result.world);
    const auto& world = result.world->View();
    ASSERT_EQ(1u, world.transitionCount);
    ASSERT_NE(nullptr, world.transitions);
    ASSERT_EQ(1u, world.transitions[0].ruleCount);
    EXPECT_EQ(48u, world.transitions[0].rules[0].sourceFade.durationFrames);
    EXPECT_EQ(harkenstudiobuild::TransitionProgramClassV1::Basic,
              harkenstudiobuild::ClassifyTransitionProgramV1(world.transitions[0]));
    EXPECT_NE(0u,
              world.identity.capabilityMask &
                  harken::kWorldCapabilityTransition);

    transition.rules[0].sourceFade.durationFrames = 999;
    EXPECT_EQ(48u, world.transitions[0].rules[0].sourceFade.durationFrames);

    transition.rules[0].sourceExit = harkenstudiobuild::TransitionSourceExitV1::NextBeat;
    transition.rules[0].requiredClockFingerprint = 123;
    result = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(), {authored}, {}, {transition});
    EXPECT_EQ(harkencurrentworldcompiler::CompileStatusV1::
                  TransitionCapabilityUnavailable,
              result.status);
    EXPECT_EQ(nullptr, result.world);
}

TEST(CurrentWorldCompiler,
     CompositeTrackTransitionIsAcceptedAsBoundedEndpoint) {
    harkenprojectschema::CanonicalProject project{};
    project.objects.push_back(Layer(1, {2}));

    harkenprojectschema::ContentObject container{};
    container.kind = harkenprojectschema::ContentKind::Container;
    container.container.chassis = ResolvedChassis(2);
    harkenprojectschema::ContainerChildOccurrence child{};
    child.stableOccurrenceOrdinal = 7;
    child.childPlayableRef.kind = harkenprojectschema::ContentKind::Track;
    child.childPlayableRef.target = harkentest::StudioObjectId(4);
    container.container.children.push_back(child);
    project.objects.push_back(container);

    harkenprojectschema::ContentObject track{};
    track.kind = harkenprojectschema::ContentKind::Track;
    track.track.chassis = ResolvedChassis(4);
    for (std::uint32_t index = 0; index < 2; ++index) {
        harkenprojectschema::Clip clip{};
        clip.id = harkentest::StudioObjectId(10 + index);
        clip.guid = {20 + index, 30 + index};
        clip.stableOccurrenceOrdinal = index + 1;
        clip.contentRef.kind = harkenprojectschema::ContentKind::Sound;
        clip.contentRef.target = harkentest::StudioObjectId(3);
        clip.time = index * 1000000;
        track.track.clips.push_back(clip);
    }
    project.objects.push_back(Sound(3));
    project.objects.push_back(track);
    project.roots = {harkentest::StudioObjectId(1)};

    const std::vector<harkencurrentworldcompiler::DefinitionMaterializationV1>
        materializations = {
            {harkenplayableproperties::ContentPropertyKindV1::Container, harkentest::StudioObjectId(2), 22,
             harkentest::StudioObjectId(20), 200},
            {harkenplayableproperties::ContentPropertyKindV1::Sound, harkentest::StudioObjectId(3), 30, harkentest::StudioObjectId(21),
             201},
            {harkenplayableproperties::ContentPropertyKindV1::Track, harkentest::StudioObjectId(4), 44, harkentest::StudioObjectId(0),
             400},
        };
    auto authored = Authored(harkenstudiobuild::ContainerModeV1::SequenceQueue,
                             harkenstudiobuild::RandomDisciplineV1::Draw);
    authored.transitionProgramIndex = 0;

    harkencurrentworldcompiler::TransitionProgramAuthoringV1 transition{};
    transition.ownerContainerStableId = authored.containerStableId;
    harkenstudiobuild::TransitionRuleViewV1 rule{};
    rule.sourceAny = 1;
    rule.targetAny = 1;
    rule.sourceFade.curveLocalId = UINT32_MAX;
    rule.bridgeFadeIn.curveLocalId = UINT32_MAX;
    rule.bridgeFadeOut.curveLocalId = UINT32_MAX;
    rule.targetFade.curveLocalId = UINT32_MAX;
    transition.rules.push_back(rule);

    const auto result = harkencurrentworldcompiler::CompileCurrentWorldV1(
        project, {}, materializations, {authored}, {}, {transition});
    ASSERT_TRUE(result.ok()) << static_cast<std::uint32_t>(result.status);
    ASSERT_NE(nullptr, result.world);
    const auto& world = result.world->View();
    ASSERT_EQ(1u, world.transitionCount);
    EXPECT_EQ(harkenstudiobuild::PlayableKindV1::Track,
              world.containers[0].children[0].playable.kind);
    EXPECT_NE(0u, world.identity.capabilityMask &
                      harken::kWorldCapabilityTransition);
}

TEST(CurrentWorldCompiler,
     MalformedAdvancedTransitionIsInvalidBeforeCapabilityRefusal) {
    auto authored = Authored(harkenstudiobuild::ContainerModeV1::SequenceQueue,
                             harkenstudiobuild::RandomDisciplineV1::Draw);
    authored.transitionProgramIndex = 0;
    authored.curves = {FadeCurve(1), FadeCurve(2)};

    harkencurrentworldcompiler::TransitionProgramAuthoringV1 transition{};
    transition.ownerContainerStableId = authored.containerStableId;
    harkenstudiobuild::TransitionRuleViewV1 rule{};
    rule.sourceAny = 1;
    rule.targetAny = 1;
    rule.sourceExit = harkenstudiobuild::TransitionSourceExitV1::NextBeat;
    rule.requiredClockFingerprint = 123;
    rule.sourceFade.active = 1;
    rule.sourceFade.durationFrames = 48;
    rule.sourceFade.curveLocalId = 999;
    rule.bridgeFadeIn.curveLocalId = UINT32_MAX;
    rule.bridgeFadeOut.curveLocalId = UINT32_MAX;
    rule.targetFade.active = 1;
    rule.targetFade.durationFrames = 96;
    rule.targetFade.curveLocalId = 2;
    transition.rules.push_back(rule);

    const auto result = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(), {authored}, {}, {transition});
    EXPECT_EQ(harkencurrentworldcompiler::CompileStatusV1::
                  InvalidCanonicalWorld,
              result.status);
    EXPECT_EQ(nullptr, result.world);
}

TEST(CurrentWorldCompiler,
     TransitionIndicesFollowOwnerAfterCanonicalSorting) {
    harkenprojectschema::CanonicalProject project{};
    project.objects.push_back(Layer(1, {4, 2}));
    project.objects.push_back(Container(2, 3));
    project.objects.push_back(Sound(3));
    project.objects.push_back(Container(4, 5));
    project.objects.push_back(Sound(5));
    project.roots = {harkentest::StudioObjectId(1)};

    auto high = Authored(harkenstudiobuild::ContainerModeV1::SequenceQueue,
                         harkenstudiobuild::RandomDisciplineV1::Draw);
    high.containerStableId = harkentest::StudioObjectId(4);
    high.transitionProgramIndex = 0;
    high.authoredStableOrder = 1;
    high.curves = {FadeCurve(1), FadeCurve(2)};
    auto low = Authored(harkenstudiobuild::ContainerModeV1::SequenceQueue,
                        harkenstudiobuild::RandomDisciplineV1::Draw);
    low.transitionProgramIndex = 1;
    low.curves = {FadeCurve(1), FadeCurve(2)};

    const auto transition = [](std::uint64_t owner) {
        harkencurrentworldcompiler::TransitionProgramAuthoringV1 value{};
        value.ownerContainerStableId = harkentest::StudioObjectId(owner);
        harkenstudiobuild::TransitionRuleViewV1 rule{};
        rule.sourceAny = 1;
        rule.targetAny = 1;
        rule.sourceFade.active = 1;
        rule.sourceFade.curveLocalId = 1;
        rule.bridgeFadeIn.curveLocalId = UINT32_MAX;
        rule.bridgeFadeOut.curveLocalId = UINT32_MAX;
        rule.targetFade.active = 1;
        rule.targetFade.curveLocalId = 2;
        value.rules.push_back(rule);
        return value;
    };

    const auto result = harkencurrentworldcompiler::CompileCurrentWorldV1(
        project, {},
        {{harkenplayableproperties::ContentPropertyKindV1::Container, harkentest::StudioObjectId(4), 44, harkentest::StudioObjectId(0),
          400},
         {harkenplayableproperties::ContentPropertyKindV1::Sound, harkentest::StudioObjectId(5), 55, harkentest::StudioObjectId(0),
          500},
         {harkenplayableproperties::ContentPropertyKindV1::Container, harkentest::StudioObjectId(2), 22, harkentest::StudioObjectId(0),
          200},
         {harkenplayableproperties::ContentPropertyKindV1::Sound, harkentest::StudioObjectId(3), 33, harkentest::StudioObjectId(0),
          300}},
        {high, low}, {}, {transition(4), transition(2)});
    ASSERT_TRUE(result.ok()) << static_cast<std::uint32_t>(result.status)
                             << " related=" << result.relatedStableId;
    const auto& world = result.world->View();
    ASSERT_EQ(2u, world.containerCount);
    ASSERT_EQ(2u, world.transitionCount);
    EXPECT_EQ(harkentest::StudioObjectId(2u), world.containers[0].containerStableId);
    EXPECT_EQ(0u, world.containers[0].transitionProgramIndex);
    EXPECT_EQ(harkentest::StudioObjectId(2u), world.transitions[0].ownerContainerStableId);
    EXPECT_EQ(harkentest::StudioObjectId(4u), world.containers[1].containerStableId);
    EXPECT_EQ(1u, world.containers[1].transitionProgramIndex);
    EXPECT_EQ(harkentest::StudioObjectId(4u), world.transitions[1].ownerContainerStableId);
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::ValidateCompilerWorld(world));
}

TEST(CurrentWorldCompiler,
     TransitionRulesAndDefaultIndexFollowCanonicalStableOrder) {
    auto authored = Authored(harkenstudiobuild::ContainerModeV1::SequenceQueue,
                             harkenstudiobuild::RandomDisciplineV1::Draw);
    authored.transitionProgramIndex = 0;

    harkencurrentworldcompiler::TransitionProgramAuthoringV1 transition{};
    transition.ownerContainerStableId = authored.containerStableId;

    harkenstudiobuild::TransitionRuleViewV1 fallback{};
    fallback.stableRuleOrdinal = 20;
    fallback.authoredStableOrder = 20;
    fallback.sourceAny = 1;
    fallback.targetAny = 1;

    harkenstudiobuild::TransitionRuleViewV1 exactTarget{};
    exactTarget.stableRuleOrdinal = 10;
    exactTarget.authoredStableOrder = 10;
    exactTarget.sourceAny = 1;
    exactTarget.target.kind = harkenstudiobuild::PlayableKindV1::Sound;
    exactTarget.target.stableId = harkentest::StudioObjectId(3);

    transition.rules = {fallback, exactTarget};
    transition.defaultRuleIndex = 0;

    const auto result = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(), {authored}, {}, {transition});
    ASSERT_TRUE(result.ok()) << static_cast<std::uint32_t>(result.status);
    const auto& compiled = result.world->View().transitions[0];
    ASSERT_EQ(2u, compiled.ruleCount);
    EXPECT_EQ(10u, compiled.rules[0].stableRuleOrdinal);
    EXPECT_EQ(20u, compiled.rules[1].stableRuleOrdinal);
    EXPECT_EQ(1u, compiled.defaultRuleIndex);
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::ValidateCompilerWorld(result.world->View()));
}

TEST(CurrentWorldCompiler,
     TransitionRuleEndpointsCannotReferenceAnotherContainersChild) {
    harkenprojectschema::CanonicalProject project{};
    project.objects.push_back(Layer(1, {2, 4}));
    project.objects.push_back(Container(2, 3));
    project.objects.push_back(Sound(3));
    project.objects.push_back(Container(4, 5));
    project.objects.push_back(Sound(5));
    project.roots = {harkentest::StudioObjectId(1)};
    const std::vector<harkencurrentworldcompiler::DefinitionMaterializationV1>
        materializations = {
            {harkenplayableproperties::ContentPropertyKindV1::Container, harkentest::StudioObjectId(2), 22,
             harkentest::StudioObjectId(20), 200},
            {harkenplayableproperties::ContentPropertyKindV1::Sound, harkentest::StudioObjectId(3), 30, harkentest::StudioObjectId(21),
             201},
            {harkenplayableproperties::ContentPropertyKindV1::Container, harkentest::StudioObjectId(4), 44,
             harkentest::StudioObjectId(40), 400},
            {harkenplayableproperties::ContentPropertyKindV1::Sound, harkentest::StudioObjectId(5), 50, harkentest::StudioObjectId(41),
             401},
        };

    auto first = Authored(harkenstudiobuild::ContainerModeV1::SequenceQueue,
                          harkenstudiobuild::RandomDisciplineV1::Draw);
    first.transitionProgramIndex = 0;
    auto second = Authored(harkenstudiobuild::ContainerModeV1::SequenceQueue,
                           harkenstudiobuild::RandomDisciplineV1::Draw);
    second.containerStableId = harkentest::StudioObjectId(4);

    harkencurrentworldcompiler::TransitionProgramAuthoringV1 transition{};
    transition.ownerContainerStableId = first.containerStableId;
    harkenstudiobuild::TransitionRuleViewV1 fallback{};
    fallback.sourceAny = 1;
    fallback.targetAny = 1;
    harkenstudiobuild::TransitionRuleViewV1 foreign{};
    foreign.stableRuleOrdinal = 1;
    foreign.source.kind = harkenstudiobuild::PlayableKindV1::Sound;
    foreign.source.stableId = harkentest::StudioObjectId(5);
    foreign.targetAny = 1;
    foreign.authoredStableOrder = 1;
    transition.rules = {fallback, foreign};

    const auto result = harkencurrentworldcompiler::CompileCurrentWorldV1(
        project, {}, materializations, {first, second}, {}, {transition});
    EXPECT_EQ(harkencurrentworldcompiler::CompileStatusV1::InvalidCanonicalWorld,
              result.status);
    EXPECT_EQ(nullptr, result.world);
}

TEST(CurrentWorldCompiler, TransitionProgramsCannotBeOrphaned) {
    const auto authored = Authored(harkenstudiobuild::ContainerModeV1::SequenceQueue,
                                   harkenstudiobuild::RandomDisciplineV1::Draw);
    harkencurrentworldcompiler::TransitionProgramAuthoringV1 transition{};
    transition.ownerContainerStableId = authored.containerStableId;
    harkenstudiobuild::TransitionRuleViewV1 rule{};
    rule.sourceAny = 1;
    rule.targetAny = 1;
    transition.rules.push_back(rule);

    const auto result = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(), {authored}, {}, {transition});
    EXPECT_EQ(harkencurrentworldcompiler::CompileStatusV1::InvalidCanonicalWorld,
              result.status);
    EXPECT_EQ(nullptr, result.world);
}

TEST(CurrentWorldCompiler, QueuePoliciesAndFiniteSoundTimingHintCompileExactly) {
    auto authored = Authored(harkenstudiobuild::ContainerModeV1::SequenceQueue,
                             harkenstudiobuild::RandomDisciplineV1::Draw);
    authored.queueCycle = harkenstudiobuild::QueueCycleV1::Loop;
    authored.queueSplice = harkenstudiobuild::QueueSpliceV1::MarkerAligned;
    authored.resumePosition = true;
    auto& child = authored.children[0];
    child.queueStopPolicy = harkenstudiobuild::QueueStopPolicyV1::AdvanceQueue;
    child.completion.completionClass =
        harkenstudiobuild::AuthoredCompletionClassV1::FiniteMediaCandidate;
    harkencurrentworldcompiler::CompletionLeafAuthoringV1 leaf{};
    leaf.value.stableLeafOrdinal = 1;
    leaf.value.content.kind = harkenstudiobuild::PlayableKindV1::Sound;
    leaf.value.content.stableId = harkentest::StudioObjectId(3);
    leaf.value.startOffset = {
        harkenstudiobuild::AuthoredTimeDomainV1::SourceFrames, 0, 1};
    leaf.value.duration = {
        harkenstudiobuild::AuthoredTimeDomainV1::NaturalMediaEnd, 0, 1};
    child.completion.leaves.push_back(leaf);

    const auto result = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(), {authored});
    ASSERT_TRUE(result.ok()) << static_cast<std::uint32_t>(result.status);
    ASSERT_EQ(1u, result.world->View().containerCount);
    const auto& queue = result.world->View().containers[0];
    EXPECT_EQ(harkenstudiobuild::QueueCycleV1::Loop, queue.queueCycle);
    EXPECT_EQ(0u, queue.queueCycleCount);
    EXPECT_EQ(harkenstudiobuild::QueueSpliceV1::MarkerAligned, queue.queueSplice);
    EXPECT_EQ(1u, queue.resumePosition);
    ASSERT_EQ(1u, queue.childCount);
    EXPECT_EQ(harkenstudiobuild::QueueStopPolicyV1::AdvanceQueue,
              queue.children[0].queueStopPolicy);
    const auto& timingHint = queue.children[0].timingHint;
    EXPECT_EQ(harkenstudiobuild::ChildTimingHintKindV2::InlineSound, timingHint.kind);
    EXPECT_EQ(0u, timingHint.reserved);
    EXPECT_EQ(harkenstudiobuild::AuthoredTimeDomainV1::SourceFrames,
              timingHint.payload.inlineSound.startOffset.domain);
    EXPECT_EQ(harkenstudiobuild::AuthoredTimeDomainV1::NaturalMediaEnd,
              timingHint.payload.inlineSound.duration.domain);

}

TEST(CurrentWorldCompiler, QueuePieceChildUsesItsCanonicalScheduleReference) {
    harkenprojectschema::CanonicalProject project{};
    harkenprojectschema::ContentObject container{};
    container.kind = harkenprojectschema::ContentKind::Container;
    container.container.chassis = ResolvedChassis(1);
    harkenprojectschema::ContainerChildOccurrence child{};
    child.stableOccurrenceOrdinal = 7;
    child.childPlayableRef.kind = harkenprojectschema::ContentKind::Piece;
    child.childPlayableRef.target = harkentest::StudioObjectId(2);
    container.container.children.push_back(child);
    project.objects.push_back(container);

    harkenprojectschema::ContentObject piece{};
    piece.kind = harkenprojectschema::ContentKind::Piece;
    piece.piece.chassis = ResolvedChassis(2);
    piece.piece.tracks = {harkentest::StudioObjectId(3)};
    project.objects.push_back(piece);

    harkenprojectschema::ContentObject track{};
    track.kind = harkenprojectschema::ContentKind::Track;
    track.track.chassis = ResolvedChassis(3);
    project.objects.push_back(track);
    project.roots = {harkentest::StudioObjectId(1)};

    const std::vector<harkencurrentworldcompiler::DefinitionMaterializationV1>
        materializations = {
            {harkenplayableproperties::ContentPropertyKindV1::Container, harkentest::StudioObjectId(1), 11,
             harkentest::StudioObjectId(0), 101},
            {harkenplayableproperties::ContentPropertyKindV1::Piece, harkentest::StudioObjectId(2), 22, harkentest::StudioObjectId(0),
             202},
            {harkenplayableproperties::ContentPropertyKindV1::Track, harkentest::StudioObjectId(3), 33, harkentest::StudioObjectId(0),
             303},
        };
    harkencurrentworldcompiler::ContainerAuthoringV1 authored{};
    authored.containerStableId = harkentest::StudioObjectId(1);
    authored.mode = harkenstudiobuild::ContainerModeV1::RandomQueue;
    harkencurrentworldcompiler::ContainerChildAuthoringV1 authoredChild{};
    authoredChild.stableOccurrenceOrdinal = 7;
    authoredChild.orderWeight = 1;
    authored.children.push_back(authoredChild);

    const auto result = harkencurrentworldcompiler::CompileCurrentWorldV1(
        project, {}, materializations, {authored});
    ASSERT_TRUE(result.ok()) << static_cast<std::uint32_t>(result.status)
                             << " related " << result.relatedStableId;
    const auto& world = result.world->View();
    ASSERT_EQ(1u, world.containerCount);
    ASSERT_EQ(1u, world.pieceScheduleCount);
    const auto& timingHint = world.containers[0].children[0].timingHint;
    EXPECT_EQ(harkenstudiobuild::ChildTimingHintKindV2::ScheduleReference,
              timingHint.kind);
    EXPECT_EQ(harkenstudiobuild::PlayableKindV1::Piece,
              timingHint.payload.schedule.kind);
    EXPECT_EQ(harkentest::StudioObjectId(2u), timingHint.payload.schedule.stableId);
    EXPECT_EQ(world.pieceSchedules[0].scheduleSemanticFingerprint,
              timingHint.payload.schedule.scheduleSemanticFingerprint);
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::ValidateCompilerWorld(world));

    authored.children[0].completion.completionClass =
        harkenstudiobuild::AuthoredCompletionClassV1::FiniteMediaCandidate;
    const auto duplicateAuthority =
        harkencurrentworldcompiler::CompileCurrentWorldV1(
            project, {}, materializations, {authored});
    EXPECT_EQ(
        harkencurrentworldcompiler::CompileStatusV1::InvalidContainerAuthoring,
        duplicateAuthority.status);
    EXPECT_EQ(nullptr, duplicateAuthority.world);
}

TEST(CurrentWorldCompiler,
     AuthoredClipDurationChangesFingerprintAndRoundTripsInVintageFourteen) {
    const auto compileDuration = [](std::int64_t duration) {
        harkenprojectschema::CanonicalProject project{};
        harkenprojectschema::ContentObject track{};
        track.kind = harkenprojectschema::ContentKind::Track;
        track.track.chassis = ResolvedChassis(1);
        harkenprojectschema::Clip clip{};
        clip.id = harkentest::StudioObjectId(2);
        clip.guid = {20, 21};
        clip.stableOccurrenceOrdinal = 0;
        clip.contentRef.kind = harkenprojectschema::ContentKind::Sound;
        clip.contentRef.target = harkentest::StudioObjectId(3);
        clip.time = 17;
        clip.duration = duration;
        track.track.clips.push_back(clip);
        project.objects.push_back(track);
        project.objects.push_back(Sound(3));
        project.roots = {harkentest::StudioObjectId(1)};
        const std::vector<
            harkencurrentworldcompiler::DefinitionMaterializationV1>
            materializations = {
                {harkenplayableproperties::ContentPropertyKindV1::Track,
                 harkentest::StudioObjectId(1), 11,
                 harkentest::StudioObjectId(0), 101},
                {harkenplayableproperties::ContentPropertyKindV1::Sound,
                 harkentest::StudioObjectId(3), 33,
                 harkentest::StudioObjectId(0), 303},
            };
        return harkencurrentworldcompiler::CompileCurrentWorldV1(
            project, {}, materializations, {});
    };

    const auto shortClip = compileDuration(123);
    const auto longClip = compileDuration(456);
    ASSERT_TRUE(shortClip.ok());
    ASSERT_TRUE(longClip.ok());
    ASSERT_EQ(1u, shortClip.world->View().trackScheduleCount);
    ASSERT_EQ(1u,
              shortClip.world->View().trackSchedules[0].placementCount);
    EXPECT_EQ(123, shortClip.world->View().trackSchedules[0]
                       .placements[0].authoredDuration);
    EXPECT_NE(shortClip.world->View().trackSchedules[0]
                  .scheduleSemanticFingerprint,
              longClip.world->View().trackSchedules[0]
                  .scheduleSemanticFingerprint);
    EXPECT_NE(shortClip.world->View().identity.runtimeSemanticFingerprint,
              longClip.world->View().identity.runtimeSemanticFingerprint);
    ASSERT_EQ(18u, harken::kCurrentWorldShippingBodyVintage);

    std::vector<std::uint8_t> body;
    ASSERT_TRUE(harkenstudiobuild::EncodeCurrentWorldShippingBody(
        shortClip.world->View(), body));
    harken::CurrentWorldBodyImage decoded;
    ASSERT_TRUE(decoded.Decode(
        body.data(), body.size(), harken::kCurrentWorldShippingBodyVintage,
        harken::CurrentWorldDecodeMode::StrictArtifact));
    ASSERT_NE(nullptr, decoded.ProductionView());
    ASSERT_EQ(1u, decoded.ProductionView()->trackScheduleCount);
    ASSERT_EQ(1u, decoded.ProductionView()->trackSchedules[0].placementCount);
    EXPECT_EQ(123, decoded.ProductionView()->trackSchedules[0]
                       .placements[0].authoredDuration);
    EXPECT_EQ(shortClip.world->View().identity.runtimeSemanticFingerprint,
              decoded.ProductionView()->identity.runtimeSemanticFingerprint);

    harken::CurrentWorldBodyImage retired;
    EXPECT_FALSE(retired.Decode(
        body.data(), body.size(), harken::kCurrentWorldBodyVintageV13,
        harken::CurrentWorldDecodeMode::StrictArtifact));
    EXPECT_EQ(nullptr, retired.ProductionView());
}

TEST(CurrentWorldCompiler, MusicalPieceAndSemanticClipOrderCompileExactly) {
    harkenprojectschema::CanonicalProject project{};
    harkenprojectschema::ContentObject piece{};
    piece.kind = harkenprojectschema::ContentKind::Piece;
    piece.piece.chassis = ResolvedChassis(1);
    piece.piece.tracks = {harkentest::StudioObjectId(2)};
    piece.piece.timeSpec.kind = harkenprojectschema::PieceTimeKind::Musical;
    piece.piece.timeSpec.tempoMap.push_back({0, 600000});
    piece.piece.timeSpec.meterMap.push_back({0, 3, 4});
    piece.piece.startPoint = {harkenprojectschema::StartPointKind::Marker, 7};
    piece.piece.markers.push_back({harkentest::StudioObjectId(6), {60, 61}, 7,
                                harkenprojectschema::MarkerKind::Custom, -120});
    project.objects.push_back(piece);
    harkenprojectschema::ContentObject track{};
    track.kind = harkenprojectschema::ContentKind::Track;
    track.track.chassis = ResolvedChassis(2);
    for (const auto [id, ordinal, semanticKey] : {
             std::tuple<std::uint64_t, std::uint32_t, std::uint64_t>{4, 8, 20},
             std::tuple<std::uint64_t, std::uint32_t, std::uint64_t>{5, 9, 10}}) {
        harkenprojectschema::Clip clip{};
        clip.id = harkentest::StudioObjectId(id);
        clip.guid = {id + 100, id + 200};
        clip.stableOccurrenceOrdinal = ordinal;
        clip.contentRef.kind = harkenprojectschema::ContentKind::Sound;
        clip.contentRef.target = harkentest::StudioObjectId(3);
        clip.time = 100;
        clip.semanticOrderKey = semanticKey;
        track.track.clips.push_back(clip);
    }
    project.objects.push_back(track);
    project.objects.push_back(Sound(3));
    project.roots = {harkentest::StudioObjectId(1), harkentest::StudioObjectId(3)};
    const std::vector<harkencurrentworldcompiler::DefinitionMaterializationV1>
        materializations = {
            {harkenplayableproperties::ContentPropertyKindV1::Piece, harkentest::StudioObjectId(1), 11, harkentest::StudioObjectId(0),
             101},
            {harkenplayableproperties::ContentPropertyKindV1::Track, harkentest::StudioObjectId(2), 22, harkentest::StudioObjectId(0),
             202},
            {harkenplayableproperties::ContentPropertyKindV1::Sound, harkentest::StudioObjectId(3), 33, harkentest::StudioObjectId(0),
             303},
        };
    const auto result = harkencurrentworldcompiler::CompileCurrentWorldV1(
        project, {}, materializations, {});
    ASSERT_TRUE(result.ok());
    const auto& world = result.world->View();
    ASSERT_EQ(1u, world.trackScheduleCount);
    ASSERT_EQ(2u, world.trackSchedules[0].placementCount);
    EXPECT_EQ(harkentest::StudioObjectId(5u), world.trackSchedules[0].placements[0].clipId);
    EXPECT_EQ(harkentest::StudioObjectId(4u), world.trackSchedules[0].placements[1].clipId);
    ASSERT_EQ(1u, world.pieceScheduleCount);
    const auto& schedule = world.pieceSchedules[0];
    EXPECT_EQ(harkenstudiobuild::ScheduleTimeKindV1::MusicalTicks, schedule.timeKind);
    EXPECT_EQ(600000, schedule.microsPerQuarter);
    EXPECT_EQ(3u, schedule.meterNumerator);
    EXPECT_EQ(4u, schedule.meterDenominator);
    EXPECT_EQ(1u, schedule.startPointKind);
    EXPECT_EQ(7u, schedule.startMarkerId);
    ASSERT_EQ(1u, schedule.markerCount);
    EXPECT_EQ(-120, schedule.markers[0].position);
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::ValidateCompilerWorld(world));
}

TEST(CurrentWorldCompiler, CapacityPlusOneRefusesWithoutOutput) {
    harkenstudiobuild::WorldCapacitySummaryV1 capacity{};
    capacity.maxPlayablePropertyPrograms = 2;
    const auto result = harkencurrentworldcompiler::CompileCurrentWorldV1(
        Project(), {}, Materializations(),
        {Authored(harkenstudiobuild::ContainerModeV1::Random,
                  harkenstudiobuild::RandomDisciplineV1::Draw)},
        capacity);
    EXPECT_EQ(harkencurrentworldcompiler::CompileStatusV1::Capacity,
              result.status);
    EXPECT_EQ(nullptr, result.world);
}

TEST(CurrentWorldCompiler, PropertyCapacityZeroHundredAndPlusOne) {
    harkenstudiobuild::WorldCapacitySummaryV1 zeroCapacity{};
    zeroCapacity.maxPlayablePropertyPrograms = 0;
    const harkenprojectschema::CanonicalProject empty{};
    const auto zero = harkencurrentworldcompiler::CompileCurrentWorldV1(
        empty, {}, {}, {}, zeroCapacity);
    ASSERT_TRUE(zero.ok());
    EXPECT_EQ(0u, zero.world->View().playablePropertyCount);

    const auto compileCount = [](std::uint32_t count) {
        harkenprojectschema::CanonicalProject project{};
        std::vector<harkencurrentworldcompiler::DefinitionMaterializationV1>
            materializations;
        for (std::uint32_t i = 0; i < count; ++i) {
            const std::uint64_t id = static_cast<std::uint64_t>(i) + 1;
            project.objects.push_back(Sound(id));
            project.roots.push_back(harkentest::StudioObjectId(id));
            materializations.push_back(
                {harkenplayableproperties::ContentPropertyKindV1::Sound, harkentest::StudioObjectId(id),
                 id + 1000, {}, 0});
        }
        harkenstudiobuild::WorldCapacitySummaryV1 capacity{};
        capacity.maxPlayablePropertyPrograms = 100;
        return harkencurrentworldcompiler::CompileCurrentWorldV1(
            project, {}, materializations, {}, capacity);
    };

    const auto full = compileCount(100);
    ASSERT_TRUE(full.ok());
    EXPECT_EQ(100u, full.world->View().playablePropertyCount);
    const auto plusOne = compileCount(101);
    EXPECT_EQ(harkencurrentworldcompiler::CompileStatusV1::Capacity,
              plusOne.status);
    EXPECT_EQ(nullptr, plusOne.world);
}

TEST(CurrentWorldCompiler, RandomDrawUsesOnlyPositiveWeightsForAvoid) {
    const auto compile = [](const std::vector<std::uint32_t>& weights,
                            std::uint32_t avoidLastN) {
        harkenprojectschema::CanonicalProject project{};
        harkenprojectschema::ContentObject container{};
        container.kind = harkenprojectschema::ContentKind::Container;
        container.container.chassis = ResolvedChassis(1);
        std::vector<harkencurrentworldcompiler::DefinitionMaterializationV1>
            materializations = {{
                harkenplayableproperties::ContentPropertyKindV1::Container,
                harkentest::StudioObjectId(1), 101, harkentest::StudioObjectId(0), 0}};
        harkencurrentworldcompiler::ContainerAuthoringV1 authored{};
        authored.containerStableId = harkentest::StudioObjectId(1);
        authored.mode = harkenstudiobuild::ContainerModeV1::Random;
        authored.randomDiscipline = harkenstudiobuild::RandomDisciplineV1::Draw;
        authored.avoidLastN = avoidLastN;
        for (std::uint32_t i = 0; i < weights.size(); ++i) {
            const harkenstudio::ObjectId soundId = harkentest::StudioObjectId(static_cast<std::uint64_t>(i) + 2);
            harkenprojectschema::ContainerChildOccurrence occurrence{};
            occurrence.stableOccurrenceOrdinal = i;
            occurrence.childPlayableRef.kind =
                harkenprojectschema::ContentKind::Sound;
            occurrence.childPlayableRef.target = soundId;
            container.container.children.push_back(occurrence);
            project.objects.push_back(Sound(soundId.low));
            materializations.push_back({
                harkenplayableproperties::ContentPropertyKindV1::Sound,
                soundId, 200 + soundId.low, {}, 0});
            harkencurrentworldcompiler::ContainerChildAuthoringV1 child{};
            child.stableOccurrenceOrdinal = i;
            child.authoredStableOrder = i;
            child.frequencyWeight = weights[i];
            authored.children.push_back(child);
        }
        project.objects.insert(project.objects.begin(), std::move(container));
        project.roots.push_back(harkentest::StudioObjectId(1));
        return harkencurrentworldcompiler::CompileCurrentWorldV1(
            project, {}, materializations, {authored});
    };

    const auto mixed = compile({2, 0, 3}, 1);
    ASSERT_TRUE(mixed.ok());
    ASSERT_EQ(1u, mixed.world->View().containerCount);
    ASSERT_EQ(3u, mixed.world->View().containers[0].childCount);
    EXPECT_EQ(0u, mixed.world->View().containers[0].children[1]
                      .frequencyWeight);
    EXPECT_EQ(harken::WorldInstallStatus::Ok,
              harkentest::ValidateCompilerWorld(mixed.world->View()));

    EXPECT_EQ(harkencurrentworldcompiler::CompileStatusV1::InvalidCanonicalWorld,
              compile({2, 0, 3}, 2).status);
    EXPECT_EQ(harkencurrentworldcompiler::CompileStatusV1::InvalidCanonicalWorld,
              compile({0, 0}, 0).status);
}

TEST(CurrentWorldCompiler, CanonicalizesNegativeZeroAtCompilerBoundaries) {
    const float negativeZero =
        std::bit_cast<float>(UINT32_C(0x80000000));
    harkenprojectschema::CanonicalProject project{};
    auto sound = Sound(40);
    sound.sound.chassis.presence.Set(
        harkenplayableproperties::PlayablePropertyIdV1::Volume);
    sound.sound.chassis.presence.Set(
        harkenplayableproperties::PlayablePropertyIdV1::Sends);
    sound.sound.chassis.volume = negativeZero;
    sound.sound.chassis.sends.push_back({harkentest::StudioObjectId(2), negativeZero});
    project.objects.push_back(sound);
    project.roots.push_back(harkentest::StudioObjectId(40));

    std::vector<harkenstudiobuild::VariableDefinitionV1> variables(3);
    variables[0] = {harkentest::StudioObjectId(1), harkenstudiobuild::VariableKindV1::Smooth,
                    negativeZero, 1.0f, 0.0f};
    variables[1] = {harkentest::StudioObjectId(2), harkenstudiobuild::VariableKindV1::Smooth,
                    -1.0f, negativeZero, 0.0f};
    variables[2] = {harkentest::StudioObjectId(3), harkenstudiobuild::VariableKindV1::Smooth,
                    -1.0f, 1.0f, negativeZero};
    const auto compiled = harkencurrentworldcompiler::CompileCurrentWorldV1(
        project, variables,
        {{harkenplayableproperties::ContentPropertyKindV1::Sound,
          harkentest::StudioObjectId(40), 400, harkentest::StudioObjectId(0), 0}}, {});
    ASSERT_TRUE(compiled.ok());
    const auto& world = compiled.world->View();
    ASSERT_EQ(3u, world.variableCount);
    EXPECT_EQ(0u, std::bit_cast<std::uint32_t>(world.variables[0].rangeMin));
    EXPECT_EQ(0u, std::bit_cast<std::uint32_t>(world.variables[1].rangeMax));
    EXPECT_EQ(0u, std::bit_cast<std::uint32_t>(world.variables[2].defaultValue));
    ASSERT_EQ(1u, world.playablePropertyCount);
    EXPECT_EQ(0u, world.playableProperties[0].payload.values[0].bits);
    ASSERT_EQ(1u, world.playableProperties[0].payload.sendCount);
    EXPECT_EQ(0u, std::bit_cast<std::uint32_t>(
                      world.playableProperties[0].payload.sends[0].sendLevel));

    project.objects[0].sound.chassis.volume = 0.0f;
    project.objects[0].sound.chassis.sends[0].sendLevel = 0.0f;
    variables[0].rangeMin = 0.0f;
    variables[1].rangeMax = 0.0f;
    variables[2].defaultValue = 0.0f;
    const auto positive = harkencurrentworldcompiler::CompileCurrentWorldV1(
        project, variables,
        {{harkenplayableproperties::ContentPropertyKindV1::Sound,
          harkentest::StudioObjectId(40), 400, harkentest::StudioObjectId(0), 0}}, {});
    ASSERT_TRUE(positive.ok());
    EXPECT_EQ(world.identity.runtimeSemanticFingerprint,
              positive.world->View().identity.runtimeSemanticFingerprint);
}



} // namespace

TEST(CurrentWorldCompiler, BlendResponseAndAxislessMembersRoundTripThroughPackBody) {
    for (const bool axes : {false, true}) {
        std::uint64_t priorFingerprint = 0;
        for (const bool live : {false, true}) {
            SCOPED_TRACE(axes);
            SCOPED_TRACE(live);
            auto authored = Authored(harkenstudiobuild::ContainerModeV1::VariableControl,
                harkenstudiobuild::RandomDisciplineV1::Draw);
            authored.continuousTiming = live ? 1u : 0u;
            authored.evaluationPeriodFrames = axes && live ? 64u : 0u;
            authored.maxActiveRows = 1;
            authored.maxLiveRows = 1;
            harkencurrentworldcompiler::ContinuousGridRowAuthoringV1 row{};
            row.childStableOrdinal = 7;
            std::vector<harkenstudiobuild::VariableDefinitionV1> variables;
            if (axes) {
                variables.push_back({harkentest::StudioObjectId(91),
                    harkenstudiobuild::VariableKindV1::Smooth, 0.0f, 10.0f, 3.0f});
                authored.continuousAxes.push_back({0, 0, variables[0].variableId});
                row.cells.push_back({});
            }
            authored.continuousRows.push_back(row);
            const auto result = harkencurrentworldcompiler::CompileCurrentWorldV1(
                Project(), variables, Materializations(), {authored});
            ASSERT_TRUE(result.ok());
            const auto& world = result.world->View();
            EXPECT_EQ(live ? 1u : 0u, world.containers[0].continuousTiming);
            EXPECT_EQ(harken::WorldInstallStatus::Ok, harkentest::ValidateCompilerWorld(world));
            EXPECT_NE(priorFingerprint, world.identity.runtimeSemanticFingerprint);
            priorFingerprint = world.identity.runtimeSemanticFingerprint;
            std::vector<std::uint8_t> encoded;
            ASSERT_TRUE(harkenstudiobuild::EncodeCurrentWorldShippingBody(world, encoded));
            harken::CurrentWorldBodyImage decoded;
            ASSERT_TRUE(decoded.Decode(encoded.data(), encoded.size(), harken::kCurrentWorldShippingBodyVintage,
                harken::CurrentWorldDecodeMode::StrictArtifact));
            ASSERT_NE(nullptr, decoded.ProductionView());
            EXPECT_EQ(live ? 1u : 0u, decoded.ProductionView()->containers[0].continuousTiming);
            std::vector<std::uint8_t> runtimeEncoded;
            ASSERT_TRUE(harken::EncodeCurrentWorldShippingBody(*decoded.ProductionView(), runtimeEncoded));
            EXPECT_EQ(encoded, runtimeEncoded);
        }
    }
}
