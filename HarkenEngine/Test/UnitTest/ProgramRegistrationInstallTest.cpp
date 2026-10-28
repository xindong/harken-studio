// Coverage for the voice-start install: the registration step that carries a
// compiled occurrence's folded gain/pitch/loop and merged routing onto a
// VoiceStartInstall, and the thin primitive that applies one to the live engine.
//
// Two layers are pinned. The registration layer (no engine) checks that the
// install carries the folded bases and loop, resolves the subject to the
// session's engine sound id and refuses a subject the session never registered,
// maps schema bus ids to engine bus ids, gates the merged send count against the
// session's resolved slot count, refuses an unmapped bus, and applies the caller
// default to an unresolved bus with a recorded (never silent) trace. The engine
// layer drives the real seam on a null backend: it proves each install field
// reaches and affects a started voice — gain relative to a louder install, pitch
// by a pitched voice ending earlier, loop by a looped voice outlasting a single
// pass, a send by adding signal on its target bus — and that an unresolved
// install is refused rather than routed to a guessed bus. Exact fold values are
// the schema tests' goldens; this test proves the wiring, not the arithmetic.

#include "UnitTest/StudioObjectIdTestSupport.h"
#include "UnitTest/ProjectSchemaFixtures.h"
#include "Common/TestUtil.h"

#include "harken_program_registration/EffectDescriptorWriter.h"
#include "harken_program_registration/ProgramRegistration.h"
#include "harken_program_registration/VoiceStartInstall.h"
#include "harken_project_schema/BuildSegmentIR.h"
#include "harken_project_schema/BuildSegmentProgram.h"
#include "harken_project_schema/FixedSlotLowerer.h"
#include "harken_project_schema/FrontEnd.h"
#include "harken_project_schema/ResourceInput.h"
#include "harken_project_schema/Serialize.h"

#include "Engine.h"
#include "harken/core/LogicCommitCoordinator.h"
#include "harken/core/PlayableInstanceTable.h"

#include <harken/Harken.h>

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <map>
#include <optional>
#include <string>
#include <vector>

namespace {

using harkenprojectschema::BuildSegmentIR;
using harkenprojectschema::BuildSegmentProgram;
using harkenprojectschema::CompileFrontEnd;
using harkenprojectschema::ContentKind;
using harkenprojectschema::FrontEndResult;
using harkenprojectschema::SegmentIR;
using harkenprojectschema::SegmentProgram;
using harkenprojectschema::ProjectInput;
using harkenprojectschema::ResourceInput;
using harkenprojectschema::ResourceTable;

namespace fx = harkenprojectschema::fixtures;

// The Sound every fixture's clip targets, and the engine sound id the session
// registers for it. Registration resolves the first to the second, so the
// install a test plays already names its own sound.
constexpr VnObjectId kSubjectId = harkentest::ObjectId(3);
constexpr VnSoundId kSound = harkentest::ObjectId(1);

// Sound 3 under Segment 1 -> Track 2 (clip 100). Callers set the Sound's sends,
// bus ref, gain and pitch before compiling. Resource facts are provided for the
// Sound so the IR builds.
ProjectInput InstallWorld() {
    ProjectInput in;
    harkenprojectschema::InputNode segment = fx::Node(ContentKind::Segment, 1);
    segment.guid = harkentest::StudioObjectId(1);
    segment.ownedChildren = {fx::G(2)};
    segment.timeSpec = fx::AbsoluteTime();
    segment.startPoint = fx::EntryStart();

    harkenprojectschema::InputNode track = fx::Node(ContentKind::Track, 2);
    track.guid = harkentest::StudioObjectId(2);
    track.ownedChildren = {fx::G(3)};
    harkenprojectschema::InputClip clip = fx::Clip(100, 3);
    clip.guid = harkentest::StudioObjectId(100);
    track.clips = {clip};

    harkenprojectschema::InputNode sound = fx::Node(ContentKind::Sound, 3);
    sound.guid = harkentest::StudioObjectId(3);

    in.nodes = {segment, track, sound};
    in.roots = {fx::G(1)};
    return in;
}

ProjectInput AncestorSpatialWorld() {
    ProjectInput in;
    harkenprojectschema::InputNode segment = fx::Node(ContentKind::Segment, 1);
    segment.guid = harkentest::StudioObjectId(1);
    segment.ownedChildren = {fx::G(2)};
    segment.timeSpec = fx::AbsoluteTime();
    segment.startPoint = fx::EntryStart();

    harkenprojectschema::InputNode track = fx::Node(ContentKind::Track, 2);
    track.guid = harkentest::StudioObjectId(2);
    track.ownedChildren = {fx::G(3)};
    harkenprojectschema::InputClip clip = fx::Clip(100, 3);
    clip.guid = harkentest::StudioObjectId(100);
    track.clips = {clip};

    harkenprojectschema::InputNode container =
        fx::Node(ContentKind::Container, 4);
    container.guid = harkentest::StudioObjectId(4);
    container.ownedChildren = {fx::G(1)};

    harkenprojectschema::InputNode sound = fx::Node(ContentKind::Sound, 3);
    sound.guid = harkentest::StudioObjectId(3);

    in.nodes = {segment, track, container, sound};
    in.roots = {fx::G(4)};
    return in;
}

ResourceTable Resources(const harkenprojectschema::CanonicalProject& project) {
    ResourceTable table;
    for (const harkenprojectschema::ContentObject& obj : project.objects) {
        if (obj.kind == ContentKind::Sound) {
            ResourceInput input;
            input.facts.valid = true;
            input.facts.sampleRate = 48000;
            input.facts.channelCount = 1;
            input.facts.frameCount = 48000;
            input.seekable = true;
            table.emplace(obj.chassis().id, input);
        }
    }
    return table;
}

// Compiles with the routing facts threaded, so the occurrence carries its merged
// sends and resolved output bus.
std::optional<SegmentProgram> CompileWithRouting(const ProjectInput& in) {
    const FrontEndResult front = CompileFrontEnd(in);
    if (!front.ok() || !front.project.has_value() || !front.facts.has_value() ||
        !front.resolution.has_value() || !front.sends.has_value() ||
        !front.effectChains.has_value()) {
        return std::nullopt;
    }
    const std::optional<SegmentIR> ir =
        BuildSegmentIR(*front.project, *front.facts, Resources(*front.project),
                     *front.resolution, *front.sends, *front.effectChains);
    if (!ir.has_value()) {
        return std::nullopt;
    }
    return BuildSegmentProgram(*ir);
}

harkenprogramreg::RegistrationResult Register(
    const SegmentProgram& program, const harkenprogramreg::RegistrationParams& base,
    harken::LogicCommitCoordinator& commit,
    harken::PlayableInstanceTable& instances) {
    harkenprogramreg::RegistrationParams params = base;
    const harken::PlayableInstanceHandle owner =
        instances.Acquire(harken::PlayableKind::Segment, 0);
    // The session's sound registration, unless the caller already supplied one.
    params.soundIdMap.emplace(kSubjectId, kSound);
    params.deviceSampleRateHz = 48000;
    params.rootSegmentId = harkentest::ObjectId(1);
    params.ownerInstance = owner;
    params.ownerInstanceScheduleEpoch = instances.Classify(owner).scheduleEpoch;
    return harkenprogramreg::RegisterSegmentProgram(program, params, commit);
}

void ExpectSameLineage(const harkenprogramreg::OccurrenceLineage& actual,
                       const harkenprogramreg::OccurrenceLineage& expected) {
    EXPECT_EQ(actual.hasSegment, expected.hasSegment);
    EXPECT_EQ(actual.segmentId, expected.segmentId);
    EXPECT_EQ(actual.trackId, expected.trackId);
    EXPECT_EQ(actual.clipId, expected.clipId);
    EXPECT_EQ(actual.occurrenceOrdinal, expected.occurrenceOrdinal);
    EXPECT_EQ(actual.subjectId, expected.subjectId);
}

// ---- registration layer (no engine) --------------------------------------

TEST(ProgramRegistrationInstall, ProductionProjectionOwnsSemanticPayloadAndFades) {
    ProjectInput input = InstallWorld();
    input.nodes[1].clips[0].fades.inLength = 100000;
    input.nodes[1].clips[0].fades.outLength = 100000;
    const std::optional<SegmentProgram> program = CompileWithRouting(input);
    ASSERT_TRUE(program.has_value());

    harkenprogramreg::RegistrationParams params;
    params.resolvedVoiceSendSlots = 2;
    params.resolvedVoiceFxSlots = 1;
    params.callerDefaultOutputBus = harkentest::ObjectId(1);
    harken::LogicCommitCoordinator commit;
    harken::PlayableInstanceTable instances(2);
    harkenprogramreg::RegistrationResult result =
        Register(*program, params, commit, instances);
    ASSERT_TRUE(result.ok);
    ASSERT_NE(result.program, nullptr);
    EXPECT_EQ(result.program->OccurrenceCount(), 1u);
    ASSERT_EQ(result.program->TimelineOccurrenceCount(), 1u);
    EXPECT_EQ(result.program->TimelineResolvedOccurrenceIndex(0), 0u);
    const harken::ProgramView& timeline = result.program->TimelineView(0);
    EXPECT_EQ(timeline.ownerInstance, 0u);
    EXPECT_EQ(timeline.ownerInstanceScheduleEpoch, 0u);
    ASSERT_EQ(timeline.eventCount, 4u);
    const harken::TimelineProgramView& runtime = result.program->TimelineProgram();
    ASSERT_EQ(runtime.payloadCount, 4u);
    EXPECT_EQ(runtime.payloads[0].kind, harken::TimelineEventKind::Start);
    EXPECT_EQ(runtime.payloads[1].kind, harken::TimelineEventKind::Envelope);
    EXPECT_EQ(runtime.payloads[1].envelopeDirection,
              harken::TimelineEnvelopeDirection::FadeIn);
    EXPECT_EQ(runtime.payloads[2].envelopeDirection,
              harken::TimelineEnvelopeDirection::FadeOut);
    EXPECT_TRUE(runtime.payloads[3].authoredFadeOutCandidate);
    EXPECT_EQ(runtime.payloads[0].target.programSemanticFingerprint,
              program->programSemanticFingerprint());
    EXPECT_EQ(runtime.payloads[0].target.clipId, harkentest::ObjectId(100u));
    EXPECT_EQ(runtime.payloads[0].target.subjectId, kSubjectId);

    VnConfig config = harkentest::MakeNullConfig(2, 256, 48000);
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    const std::vector<float> pcm(48000, 0.25f);
    const VnPCMDesc pcmDesc = harkentest::MonoDesc(pcm, 48000);
    ASSERT_EQ(Vn_RegisterPCM(kSound, &pcmDesc), VN_OK);
    const harken::TimelineRuntimeInstallResult installed =
        result.program->InstallTimeline();
    EXPECT_EQ(installed.status,
              harken::TimelineRuntimeInstallStatus::Installed);
    EXPECT_EQ(installed.occurrenceCount, 1u);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    const std::size_t setSlot = installed.generation & 1u;
    ASSERT_NE(engine->timelineProgramSets[setSlot], nullptr);
    ASSERT_EQ(engine->timelineProgramSets[setSlot]->occurrences.size(), 1u);
    const harken::EngineTimelineOccurrence& engineOccurrence =
        engine->timelineProgramSets[setSlot]->occurrences[0];
    EXPECT_NE(engineOccurrence.schedule.ownerInstance, 0u);
    EXPECT_NE(engineOccurrence.schedule.ownerInstanceScheduleEpoch, 0u);
    EXPECT_TRUE(engineOccurrence.schedule.occurrence.present);
    EXPECT_NE(engineOccurrence.schedule.occurrence.token.generation, 0u);
    EXPECT_NE(engineOccurrence.schedule.occurrence.occurrenceScheduleEpoch, 0u);
}

TEST(ProgramRegistrationInstall, NonphysicalTimelineCandidatesAcquireNothing) {
    ProjectInput input = InstallWorld();
    const std::optional<SegmentProgram> program = CompileWithRouting(input);
    ASSERT_TRUE(program.has_value());
    harkenprogramreg::RegistrationParams params;
    params.resolvedVoiceSendSlots = 2;
    params.resolvedVoiceFxSlots = 1;
    params.callerDefaultOutputBus = harkentest::ObjectId(1);
    harken::LogicCommitCoordinator commit;
    harken::PlayableInstanceTable instances(2);
    harkenprogramreg::RegistrationResult result =
        Register(*program, params, commit, instances);
    ASSERT_TRUE(result.ok);
    ASSERT_NE(result.program, nullptr);

    const std::vector<float> pcm(48000, 0.25f);
    const VnPCMDesc pcmDesc = harkentest::MonoDesc(pcm, 48000);
    for (const harken::TimelinePhysicalAdmissionFault fault : {
             harken::TimelinePhysicalAdmissionFault::DirectVirtual,
             harken::TimelinePhysicalAdmissionFault::Parked,
             harken::TimelinePhysicalAdmissionFault::PendingSteal}) {
        VnConfig config = harkentest::MakeNullConfig(2, 256, 48000);
        harkentest::EngineSession session(config);
        ASSERT_EQ(session.InitResult(), VN_OK);
        ASSERT_EQ(Vn_RegisterPCM(kSound, &pcmDesc), VN_OK);
        harken::Engine* engine = session.Engine();
        ASSERT_NE(engine, nullptr);
        engine->timelinePhysicalAdmissionFault = fault;
        const harken::TimelineRuntimeInstallResult installed =
            result.program->InstallTimeline();
        ASSERT_EQ(installed.status,
                  harken::TimelineRuntimeInstallStatus::Installed);
        ASSERT_EQ(engine->timelineArms.LiveCount(), 0u);
        ASSERT_EQ(engine->timelinePool.LiveTransactionCount(), 0u);
        ASSERT_EQ(engine->timelineDeferred.Count(), 0u);
        ASSERT_EQ(engine->voices.ActiveCount(), 0u);
        ASSERT_EQ(Vn_Update(), VN_OK);
        EXPECT_EQ(engine->timelineArms.LiveCount(), 0u);
        EXPECT_EQ(engine->timelinePool.LiveTransactionCount(), 0u);
        EXPECT_EQ(engine->timelineDeferred.Count(), 0u);
        EXPECT_EQ(engine->voices.ActiveCount(), 0u);
    }
}

TEST(ProgramRegistrationInstall, InstallCarriesFoldedAndMappedRouting) {
    ProjectInput in = InstallWorld();
    in.nodes[2].volume = 6.0f;           // Sound gain folds into baseVolumeDb
    in.nodes[2].sends = {harkenprojectschema::Send{harkentest::StudioObjectId(7), 0.5f}};
    in.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(20);   // resolves as the subject's override

    const std::optional<SegmentProgram> program = CompileWithRouting(in);
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base;
    base.resolvedVoiceSendSlots = 4;
    base.busIdMap = {{harkentest::ObjectId(7), harkentest::ObjectId(700)}, {harkentest::ObjectId(20), harkentest::ObjectId(200)}};
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    ASSERT_TRUE(result.ok) << static_cast<int>(result.refusal);
    ASSERT_EQ(result.program->OccurrenceCount(), 1u);

    const harkenprogramreg::VoiceStartInstall& install = result.program->Install(0);
    EXPECT_EQ(install.baseVolumeDb, 6.0f);
    EXPECT_EQ(install.basePitchRatio, 1.0f);
    EXPECT_EQ(install.loopCount, 1u);
    EXPECT_TRUE(install.outputBusResolved);
    EXPECT_FALSE(install.outputBusDefaulted);
    EXPECT_EQ(install.outputBus, harkentest::ObjectId(200u));  // schema bus 20 mapped
    ASSERT_EQ(install.sends.size(), 1u);
    EXPECT_EQ(install.sends[0].targetBus, harkentest::ObjectId(700u));  // schema bus 7 mapped
    EXPECT_EQ(install.sends[0].level, 0.5f);
}

TEST(ProgramRegistrationInstall, WideSchemaBusIdsMapIndependently) {
    constexpr VnObjectId kLowSchemaBus = harkentest::ObjectId(1);
    constexpr VnObjectId kHighSchemaBus = harkentest::ObjectId(0x1'0000'0001ull);
    ProjectInput lowInput = InstallWorld();
    lowInput.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(kLowSchemaBus);
    ProjectInput highInput = InstallWorld();
    highInput.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(kHighSchemaBus);
    const std::optional<SegmentProgram> lowProgram = CompileWithRouting(lowInput);
    const std::optional<SegmentProgram> highProgram = CompileWithRouting(highInput);
    ASSERT_TRUE(lowProgram.has_value());
    ASSERT_TRUE(highProgram.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base;
    base.resolvedVoiceSendSlots = 4;
    base.busIdMap = {{kLowSchemaBus, harkentest::ObjectId(101)}, {kHighSchemaBus, harkentest::ObjectId(202)}};
    const harkenprogramreg::RegistrationResult low =
        Register(*lowProgram, base, commit, instances);
    const harkenprogramreg::RegistrationResult high =
        Register(*highProgram, base, commit, instances);
    ASSERT_TRUE(low.ok) << static_cast<int>(low.refusal);
    ASSERT_TRUE(high.ok) << static_cast<int>(high.refusal);
    EXPECT_EQ(low.program->Install(0).outputBus, harkentest::ObjectId(101u));
    EXPECT_EQ(high.program->Install(0).outputBus, harkentest::ObjectId(202u));
}

TEST(ProgramRegistrationInstall, SendCountAtResolvedSlotsAccepted) {
    ProjectInput in = InstallWorld();
    in.nodes[2].sends = {harkenprojectschema::Send{harkentest::StudioObjectId(7), 0.5f},
                         harkenprojectschema::Send{harkentest::StudioObjectId(8), 0.25f}};
    in.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(20);
    const std::optional<SegmentProgram> program = CompileWithRouting(in);
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base;
    base.resolvedVoiceSendSlots = 2;  // exactly the merged count
    base.busIdMap = {{harkentest::ObjectId(7), harkentest::ObjectId(700)}, {harkentest::ObjectId(8), harkentest::ObjectId(800)}, {harkentest::ObjectId(20), harkentest::ObjectId(200)}};
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    EXPECT_TRUE(result.ok) << static_cast<int>(result.refusal);
}

TEST(ProgramRegistrationInstall, SendCountOverResolvedSlotsRefused) {
    ProjectInput in = InstallWorld();
    in.nodes[2].sends = {harkenprojectschema::Send{harkentest::StudioObjectId(7), 0.5f},
                         harkenprojectschema::Send{harkentest::StudioObjectId(8), 0.25f}};
    in.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(20);
    const std::optional<SegmentProgram> program = CompileWithRouting(in);
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base;
    base.resolvedVoiceSendSlots = 1;  // one below the merged count
    base.busIdMap = {{harkentest::ObjectId(7), harkentest::ObjectId(700)}, {harkentest::ObjectId(8), harkentest::ObjectId(800)}, {harkentest::ObjectId(20), harkentest::ObjectId(200)}};
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.refusal,
              harkenprogramreg::RegistrationRefusal::SendCountExceedsResolvedSlots);
}

// The subject is resolved to an engine sound while the program is registered, so
// the install a later start consumes already names what plays. Two subjects
// mapped to two ids resolve independently: an implementation that reached for a
// constant, or for whichever id it saw first, would collapse them.
TEST(ProgramRegistrationInstall, SoundSubjectResolvesToItsMappedEngineId) {
    for (const VnSoundId engineSound : {harkentest::ObjectId(1), harkentest::ObjectId(4242)}) {
        ProjectInput in = InstallWorld();
        in.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(20);
        const std::optional<SegmentProgram> program = CompileWithRouting(in);
        ASSERT_TRUE(program.has_value());

        harken::PlayableInstanceTable instances(4);
        harken::LogicCommitCoordinator commit;
        harkenprogramreg::RegistrationParams base;
        base.resolvedVoiceSendSlots = 4;
        base.busIdMap = {{harkentest::ObjectId(20), harkentest::ObjectId(200)}};
        base.soundIdMap = {{kSubjectId, engineSound}};
        const harkenprogramreg::RegistrationResult result =
            Register(*program, base, commit, instances);
        ASSERT_TRUE(result.ok) << static_cast<int>(result.refusal);
        ASSERT_EQ(result.program->OccurrenceCount(), 1u);
        const harkenprogramreg::VoiceStartInstall& install =
            result.program->Install(0);
        EXPECT_TRUE(install.startsAudibly);
        EXPECT_EQ(install.soundId, engineSound);
        // The lineage carries the subject the id came from, so an install can be
        // traced back to the authored object rather than only to a track slot.
        EXPECT_EQ(result.program->Lineage(0).subjectId, kSubjectId);
    }
}

// A Sound subject the session did not register is a refusal here, with the same
// shape as an unmapped bus. Deciding it now is what keeps an unregistered sound
// from becoming a refused play with nothing to attribute it to.
TEST(ProgramRegistrationInstall, UnmappedSoundSubjectRefused) {
    ProjectInput in = InstallWorld();
    in.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(20);
    const std::optional<SegmentProgram> program = CompileWithRouting(in);
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    // Registered directly rather than through the helper, which supplies the
    // session's sound map for every other case in this file.
    harkenprogramreg::RegistrationParams params;
    params.deviceSampleRateHz = 48000;
    params.rootSegmentId = harkentest::ObjectId(1);
    params.ownerInstance = instances.Acquire(harken::PlayableKind::Segment, 0);
    params.ownerInstanceScheduleEpoch =
        instances.Classify(params.ownerInstance).scheduleEpoch;
    params.resolvedVoiceSendSlots = 4;
    params.busIdMap = {{harkentest::ObjectId(20), harkentest::ObjectId(200)}};
    // A map that names some other subject, so the lookup runs and misses rather
    // than being skipped because the map is empty.
    params.soundIdMap = {{harkentest::OffsetObjectId(kSubjectId, 1), harkentest::ObjectId(9)}};
    const harken::LogicCommitSequence currentBefore = commit.Current();
    const std::uint64_t issuedBefore = commit.IssuedCount();
    const harkenprogramreg::RegistrationResult refused =
        harkenprogramreg::RegisterSegmentProgram(*program, params, commit);
    EXPECT_FALSE(refused.ok);
    EXPECT_EQ(refused.refusal,
              harkenprogramreg::RegistrationRefusal::SoundTargetUnmapped);
    EXPECT_EQ(static_cast<std::uint32_t>(refused.refusal), 22u);
    EXPECT_EQ(refused.program, nullptr);
    EXPECT_TRUE(refused.notes.empty());
    EXPECT_EQ(commit.Current(), currentBefore);
    EXPECT_EQ(commit.IssuedCount(), issuedBefore);

    params.soundIdMap = {{kSubjectId, kSound}};
    const harkenprogramreg::RegistrationResult accepted =
        harkenprogramreg::RegisterSegmentProgram(*program, params, commit);
    ASSERT_TRUE(accepted.ok) << static_cast<int>(accepted.refusal);
    ASSERT_NE(accepted.program, nullptr);
    EXPECT_EQ(accepted.program->CommitSequence(), currentBefore + 1u);
    EXPECT_EQ(commit.Current(), currentBefore + 1u);
    EXPECT_EQ(commit.IssuedCount(), issuedBefore + 1u);
}

TEST(ProgramRegistrationInstall, UnmappedBusRefused) {
    ProjectInput in = InstallWorld();
    in.nodes[2].sends = {harkenprojectschema::Send{harkentest::StudioObjectId(7), 0.5f}};
    in.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(20);
    const std::optional<SegmentProgram> program = CompileWithRouting(in);
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base;
    base.resolvedVoiceSendSlots = 4;
    base.busIdMap = {{harkentest::ObjectId(20), harkentest::ObjectId(200)}};  // send bus 7 deliberately absent
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.refusal,
              harkenprogramreg::RegistrationRefusal::BusIdUnmapped);
}

TEST(ProgramRegistrationInstall, UnmappedResolvedOutputBusRefused) {
    ProjectInput in = InstallWorld();
    in.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(20);
    const std::optional<SegmentProgram> program = CompileWithRouting(in);
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base;
    base.resolvedVoiceSendSlots = 4;
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.refusal,
              harkenprogramreg::RegistrationRefusal::BusIdUnmapped);
    EXPECT_EQ(static_cast<std::uint32_t>(result.refusal), 8u);
}

TEST(ProgramRegistrationInstall, UnresolvedBusTakesCallerDefaultWithTrace) {
    ProjectInput in = InstallWorld();  // no mainOutputBusRef: bus is unresolved
    const std::optional<SegmentProgram> program = CompileWithRouting(in);
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base;
    base.resolvedVoiceSendSlots = 4;
    base.callerDefaultOutputBus = harkentest::ObjectId(99);  // a preview loader's master
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    ASSERT_TRUE(result.ok) << static_cast<int>(result.refusal);

    const harkenprogramreg::VoiceStartInstall& install = result.program->Install(0);
    EXPECT_TRUE(install.outputBusResolved);
    EXPECT_TRUE(install.outputBusDefaulted);  // the trace: never silent
    EXPECT_EQ(install.outputBus, harkentest::ObjectId(99u));
}

TEST(ProgramRegistrationInstall, UnresolvedBusNoDefaultStaysUnresolved) {
    ProjectInput in = InstallWorld();  // unresolved bus, no default supplied
    const std::optional<SegmentProgram> program = CompileWithRouting(in);
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base;
    base.resolvedVoiceSendSlots = 4;
    base.callerDefaultOutputBus = harkentest::ObjectId(0);  // none
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    ASSERT_TRUE(result.ok) << static_cast<int>(result.refusal);

    const harkenprogramreg::VoiceStartInstall& install = result.program->Install(0);
    EXPECT_FALSE(install.outputBusResolved);
    EXPECT_FALSE(install.outputBusDefaulted);
}

// ---- engine layer (null backend, real seam) ------------------------------

using harkentest::EngineSession;
using harkentest::MakeNullConfig;
using harkentest::MonoDesc;
using harkentest::MakeDC;
using harkentest::Capture;
using harkentest::PumpAndCapture;
using harkentest::Peak;

constexpr std::uint32_t kBlock = 128;
constexpr std::uint32_t kRate = 48000;

VnConfig InstallEngineConfig() {
    VnConfig config = MakeNullConfig(/*maxVoices=*/8, kBlock, kRate);
    config.voiceSendSlots = 4;
    return config;
}

// An install of the session's sound routed to master at the given
// gain/pitch/loop, no sends.
harkenprogramreg::VoiceStartInstall MasterInstall(float volumeDb,
                                                 float pitchRatio, std::uint32_t loopCount) {
    harkenprogramreg::VoiceStartInstall install;
    install.startsAudibly = true;
    install.soundId = kSound;
    install.baseVolumeDb = volumeDb;
    install.basePitchRatio = pitchRatio;
    install.loopCount = loopCount;
    install.outputBus = VN_BUS_MASTER;
    install.outputBusResolved = true;
    return install;
}

VnResult PlayInstall(const harkenprogramreg::VoiceStartInstall& install,
                     VnVoiceHandle* outHandle) {
    bool stopDeferred = true;
    const VnResult result = harkenprogramreg::PlayVoiceStartInstall(
        install, outHandle, &stopDeferred);
    EXPECT_FALSE(stopDeferred);
    return result;
}

VnSnapshot Snapshot() {
    VnSnapshot snapshot{};
    snapshot.structSize = sizeof(VnSnapshot);
    EXPECT_EQ(Vn_CaptureSnapshot(&snapshot), VN_OK);
    return snapshot;
}

TEST(ProgramRegistrationInstall, FullCompileRegistrationAndPlayPathProducesAudio) {
    EngineSession session(InstallEngineConfig());
    ASSERT_EQ(session.InitResult(), VN_OK);
    constexpr VnBusId kSendBus = harkentest::ObjectId(2);
    ASSERT_EQ(Vn_CreateBus(kSendBus, VN_BUS_MASTER), VN_OK);
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(Vn_RegisterPCM(kSound, &desc), VN_OK);

    ProjectInput input = InstallWorld();
    input.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(20);
    input.nodes[2].sends = {harkenprojectschema::Send{harkentest::StudioObjectId(7), -6.0f}};
    const FrontEndResult front = CompileFrontEnd(input);
    ASSERT_TRUE(front.ok());
    ASSERT_TRUE(front.project.has_value());
    ASSERT_TRUE(front.facts.has_value());
    ASSERT_TRUE(front.resolution.has_value());
    ASSERT_TRUE(front.sends.has_value());
    ASSERT_TRUE(front.effectChains.has_value());
    const std::optional<SegmentIR> ir =
        BuildSegmentIR(*front.project, *front.facts, Resources(*front.project),
                     *front.resolution, *front.sends, *front.effectChains);
    ASSERT_TRUE(ir.has_value());
    const std::optional<SegmentProgram> program = BuildSegmentProgram(*ir);
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base;
    base.resolvedVoiceSendSlots = 4;
    base.busIdMap = {{harkentest::ObjectId(7), kSendBus}, {harkentest::ObjectId(20), VN_BUS_MASTER}};
    const harkenprogramreg::RegistrationResult registration =
        Register(*program, base, commit, instances);
    ASSERT_TRUE(registration.ok)
        << static_cast<int>(registration.refusal);
    ASSERT_EQ(registration.program->OccurrenceCount(), 1u);

    VnVoiceHandle handle = 0;
    ASSERT_EQ(PlayInstall(
                  registration.program->Install(0), &handle),
              VN_OK);
    Capture capture;
    PumpAndCapture(session.Engine(), 2, capture);
    EXPECT_GT(Peak(capture.left), harkentest::kSilenceFloor);
}

TEST(ProgramRegistrationInstall, PlayDrivesEngineSeamAndAppliesGain) {
    EngineSession session(InstallEngineConfig());
    ASSERT_EQ(session.InitResult(), VN_OK);
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(Vn_RegisterPCM(kSound, &desc), VN_OK);

    VnVoiceHandle loud = 0;
    ASSERT_EQ(PlayInstall(
                  MasterInstall(0.0f, 1.0f, false), &loud),
              VN_OK);
    Capture loudCapture;
    PumpAndCapture(session.Engine(), 2, loudCapture);
    const float loudPeak = Peak(loudCapture.left);
    ASSERT_GT(loudPeak, harkentest::kSilenceFloor);  // gain reached the voice
    Vn_Stop(loud, 0);
    PumpAndCapture(session.Engine(), 2, loudCapture);

    VnVoiceHandle quiet = 0;
    ASSERT_EQ(PlayInstall(
                  MasterInstall(-20.0f, 1.0f, false), &quiet),
              VN_OK);
    Capture quietCapture;
    PumpAndCapture(session.Engine(), 2, quietCapture);
    const float quietPeak = Peak(quietCapture.left);

    // A 20 dB quieter install renders quieter: the base gain reached the seam.
    EXPECT_LT(quietPeak, loudPeak);
}

TEST(ProgramRegistrationInstall, StagedOutputVolumeLeavesSendGainUnchanged) {
    EngineSession session(InstallEngineConfig());
    ASSERT_EQ(VN_OK, session.InitResult());
    ASSERT_EQ(VN_OK, Vn_CreateBus(harkentest::ObjectId(2), VN_BUS_MASTER));
    const auto dc = MakeDC(kRate, 0.5f);
    const auto desc = MonoDesc(dc, kRate);
    ASSERT_EQ(VN_OK, Vn_RegisterPCM(kSound, &desc));
    auto install = MasterInstall(0.0f, 1.0f, 0);
    install.outputBusVolumeDb = -120.0f;
    install.sends.push_back({harkentest::ObjectId(2), 0.0f});
    VnVoiceHandle handle = 0;
    ASSERT_EQ(VN_OK, PlayInstall(install, &handle));
    Capture out;
    PumpAndCapture(session.Engine(), 1, out);
    for (float sample : out.left) EXPECT_NEAR(0.5f, sample, 1e-5f);
}

TEST(ProgramRegistrationInstall, PlayRefusesUnresolvedBus) {
    EngineSession session(InstallEngineConfig());
    ASSERT_EQ(session.InitResult(), VN_OK);
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(Vn_RegisterPCM(kSound, &desc), VN_OK);

    harkenprogramreg::VoiceStartInstall unresolved;
    // A startable install in every respect but its routing, so the refusal below
    // is the bus gate's and not the audibility gate's.
    unresolved.startsAudibly = true;
    unresolved.soundId = kSound;
    unresolved.baseVolumeDb = 0.0f;
    unresolved.outputBusResolved = false;  // no bus, no default
    const VnSnapshot before = Snapshot();
    constexpr VnVoiceHandle kInitialHandle = 0x1234;
    VnVoiceHandle handle = kInitialHandle;
    // The primitive refuses rather than guessing a bus; no voice starts.
    EXPECT_EQ(PlayInstall(unresolved, &handle),
              VN_INVALID_ARG);
    EXPECT_EQ(handle, 0u);
    const VnSnapshot after = Snapshot();
    EXPECT_EQ(after.activeVoiceCount, before.activeVoiceCount);
    EXPECT_EQ(after.pendingStartCount, before.pendingStartCount);
    EXPECT_EQ(after.noVoiceCount, before.noVoiceCount);
}

TEST(ProgramRegistrationInstall, LoopedVoiceOutlastsSinglePass) {
    EngineSession session(InstallEngineConfig());
    ASSERT_EQ(session.InitResult(), VN_OK);
    // A source shorter than the blocks pumped, so a single pass falls silent
    // while a loop keeps producing.
    const std::vector<float> dc = MakeDC(kBlock, 0.5f);  // one block long
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(Vn_RegisterPCM(kSound, &desc), VN_OK);

    VnVoiceHandle single = 0;
    ASSERT_EQ(PlayInstall(
                  MasterInstall(0.0f, 1.0f, /*loopCount=*/1), &single),
              VN_OK);
    Capture singleCapture;
    PumpAndCapture(session.Engine(), 6, singleCapture);
    // Late blocks of a single pass are silent (the one-block source ended).
    const std::size_t tailStart = singleCapture.left.size() - kBlock;
    EXPECT_LT(Peak(singleCapture.left.data() + tailStart, kBlock),
              harkentest::kSilenceFloor);
    Vn_Stop(single, 0);
    PumpAndCapture(session.Engine(), 6, singleCapture);

    VnVoiceHandle looped = 0;
    ASSERT_EQ(PlayInstall(
                  MasterInstall(0.0f, 1.0f, /*loopCount=*/0), &looped),
              VN_OK);
    Capture loopCapture;
    PumpAndCapture(session.Engine(), 6, loopCapture);
    const std::size_t loopTail = loopCapture.left.size() - kBlock;
    // A looped voice is still producing in the same late block.
    EXPECT_GT(Peak(loopCapture.left.data() + loopTail, kBlock),
              harkentest::kSilenceFloor);
}

TEST(ProgramRegistrationInstall, PitchedVoiceConsumesSourceFaster) {
    EngineSession session(InstallEngineConfig());
    ASSERT_EQ(session.InitResult(), VN_OK);
    // Eight blocks of source are observed through the same six-block window.
    // Double pitch ends inside it while unity pitch remains audible.
    const std::vector<float> dc = MakeDC(kBlock * 8, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(Vn_RegisterPCM(kSound, &desc), VN_OK);

    VnVoiceHandle pitched = 0;
    ASSERT_EQ(PlayInstall(
                  MasterInstall(0.0f, 2.0f, 1), &pitched),
              VN_OK);
    Capture pitchedCapture;
    PumpAndCapture(session.Engine(), 6, pitchedCapture);
    const std::size_t tail = pitchedCapture.left.size() - kBlock;
    // The pitched-up voice has already consumed its source by the last block.
    EXPECT_LT(Peak(pitchedCapture.left.data() + tail, kBlock),
              harkentest::kSilenceFloor);
    Vn_Stop(pitched, 0);
    Capture drain;
    PumpAndCapture(session.Engine(), 6, drain);

    VnVoiceHandle unity = 0;
    ASSERT_EQ(PlayInstall(
                  MasterInstall(0.0f, 1.0f, 1), &unity),
              VN_OK);
    Capture unityCapture;
    PumpAndCapture(session.Engine(), 6, unityCapture);
    const std::size_t unityTail = unityCapture.left.size() - kBlock;
    EXPECT_GT(Peak(unityCapture.left.data() + unityTail, kBlock),
              harkentest::kSilenceFloor);
}

TEST(ProgramRegistrationInstall, SendsInstallIntoAscendingSlots) {
    EngineSession session(InstallEngineConfig());
    ASSERT_EQ(session.InitResult(), VN_OK);
    constexpr VnBusId kFirstBus = harkentest::ObjectId(2);
    constexpr VnBusId kSecondBus = harkentest::ObjectId(3);
    ASSERT_EQ(Vn_CreateBus(kFirstBus, VN_BUS_MASTER), VN_OK);
    ASSERT_EQ(Vn_CreateBus(kSecondBus, VN_BUS_MASTER), VN_OK);
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(Vn_RegisterPCM(kSound, &desc), VN_OK);

    harkenprogramreg::VoiceStartInstall install = MasterInstall(0.0f, 1.0f, false);
    install.sends = {{kFirstBus, -6.0f}, {kSecondBus, -12.0f}};
    VnVoiceHandle handle = 0;
    ASSERT_EQ(PlayInstall(install, &handle),
              VN_OK);
    const VnVoiceHandle render =
        harkentest::RenderHandle(session.Engine(), handle);
    ASSERT_NE(render, 0u);
    const auto& slots =
        session.Engine()->voiceSendConfig[static_cast<std::uint32_t>(render)];
    const std::uint32_t firstBusIndex =
        session.Engine()->buses.FindIndex(kFirstBus);
    const std::uint32_t secondBusIndex =
        session.Engine()->buses.FindIndex(kSecondBus);
    ASSERT_NE(firstBusIndex, harken::kInvalidBusIndex);
    ASSERT_NE(secondBusIndex, harken::kInvalidBusIndex);
    ASSERT_TRUE(slots[0].active);
    EXPECT_EQ(slots[0].targetBusIndex, firstBusIndex);
    EXPECT_EQ(slots[0].levelDb, -6.0f);
    ASSERT_TRUE(slots[1].active);
    EXPECT_EQ(slots[1].targetBusIndex, secondBusIndex);
    EXPECT_EQ(slots[1].levelDb, -12.0f);
}

TEST(ProgramRegistrationInstall, SendAddsSignalOnTargetBus) {
    EngineSession session(InstallEngineConfig());
    ASSERT_EQ(session.InitResult(), VN_OK);
    // A send bus under master: a voice with a send contributes an extra copy to
    // master through it, so the total is louder than the dry path alone.
    constexpr VnBusId kSendBus = harkentest::ObjectId(2);
    ASSERT_EQ(Vn_CreateBus(kSendBus, VN_BUS_MASTER), VN_OK);
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(Vn_RegisterPCM(kSound, &desc), VN_OK);

    VnVoiceHandle dry = 0;
    ASSERT_EQ(PlayInstall(
                  MasterInstall(0.0f, 1.0f, false), &dry),
              VN_OK);
    Capture dryCapture;
    PumpAndCapture(session.Engine(), 2, dryCapture);
    const float dryPeak = Peak(dryCapture.left);
    ASSERT_GT(dryPeak, harkentest::kSilenceFloor);
    Vn_Stop(dry, 0);
    PumpAndCapture(session.Engine(), 2, dryCapture);

    harkenprogramreg::VoiceStartInstall withSend = MasterInstall(0.0f, 1.0f, false);
    withSend.sends.push_back(harkenprogramreg::VoiceSend{kSendBus, 0.0f});
    VnVoiceHandle sent = 0;
    ASSERT_EQ(PlayInstall(withSend, &sent),
              VN_OK);
    Capture sendCapture;
    PumpAndCapture(session.Engine(), 2, sendCapture);
    // The send routed a second copy through the send bus into master.
    EXPECT_GT(Peak(sendCapture.left), dryPeak);
}

// ---- spatial: registration layer (no engine) -----------------------------

TEST(ProgramRegistrationInstall,
     AncestorSpatialScalarsDoNotPolluteOccurrence) {
    const std::optional<SegmentProgram> baseline =
        CompileWithRouting(AncestorSpatialWorld());
    ASSERT_TRUE(baseline.has_value());

    ProjectInput poisoned = AncestorSpatialWorld();
    for (std::size_t i = 0; i < 3; ++i) {
        harkenprojectschema::SpatialSpec& spatial = poisoned.nodes[i].spatial;
        spatial.panning = i == 0 ? -0.75f : (i == 1 ? 0.5f : 0.75f);
        spatial.spread = i == 0 ? 0.25f : (i == 1 ? 0.5f : 0.75f);
        spatial.is3DEnabled = true;
        spatial.attenuationDistanceScaling = static_cast<float>(i + 2);
    }
    const std::optional<SegmentProgram> mutated = CompileWithRouting(poisoned);
    ASSERT_TRUE(mutated.has_value());

    ASSERT_EQ(mutated->ir().tracks().size(), 1u);
    ASSERT_EQ(mutated->ir().tracks()[0].occurrences.size(), 1u);
    const harkenprojectschema::IrOccurrence& occurrence =
        mutated->ir().tracks()[0].occurrences[0];
    EXPECT_EQ(occurrence.panning, 0.0f);
    EXPECT_EQ(occurrence.spread, 0.0f);
    EXPECT_FALSE(occurrence.is3DEnabled);
    EXPECT_EQ(occurrence.attenuationDistanceScaling, 1.0f);
    EXPECT_EQ(baseline->programSemanticFingerprint(),
              mutated->programSemanticFingerprint());
    EXPECT_EQ(baseline->runtimeLineageFingerprint(),
              mutated->runtimeLineageFingerprint());
}

TEST(ProgramRegistrationInstall, SpatialAndAttenuationRenderBitsAreFrozen) {
    ProjectInput in = InstallWorld();
    in.nodes[2].spatial.panning = -0.5f;
    in.nodes[2].spatial.spread = 0.25f;
    in.nodes[2].spatial.is3DEnabled = true;
    in.nodes[2].spatial.attenuationDistanceScaling = 2.0f;
    in.nodes[2].spatial.attenuationRef = harkentest::StudioObjectId(50);
    in.nodes[2].presence.Set(harkenplayableproperties::PlayablePropertyIdV1::SpatialAttenuationRef);
    const std::optional<SegmentProgram> program = CompileWithRouting(in);
    ASSERT_TRUE(program.has_value());

    EXPECT_NE(harkenprojectschema::RenderSegmentIR(program->ir()).find(
                  "/spatial3204448256,1048576000,1,1073741824/atten" +
                  harken::ObjectIdString(harkentest::ObjectId(50))),
              std::string::npos);
}

TEST(ProgramRegistrationInstall, InstallCarriesSpatialAndResolvedAttenuation) {
    ProjectInput in = InstallWorld();
    in.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(20);
    in.nodes[2].spatial.panning = -0.5f;
    in.nodes[2].spatial.spread = 0.25f;
    in.nodes[2].spatial.is3DEnabled = true;
    in.nodes[2].spatial.attenuationDistanceScaling = 3.0f;
    in.nodes[2].spatial.attenuationRef = harkentest::StudioObjectId(50);   // resolves as the subject override
    in.nodes[2].presence.Set(harkenplayableproperties::PlayablePropertyIdV1::SpatialAttenuationRef);
    const std::optional<SegmentProgram> program = CompileWithRouting(in);
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base;
    base.resolvedVoiceSendSlots = 4;
    base.busIdMap = {{harkentest::ObjectId(20), VN_BUS_MASTER}};
    base.attenuationIdMap = {{harkentest::ObjectId(50), harkentest::ObjectId(900)}};
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    ASSERT_TRUE(result.ok) << static_cast<int>(result.refusal);

    const harkenprogramreg::VoiceStartInstall& install = result.program->Install(0);
    EXPECT_EQ(install.panning, -0.5f);
    EXPECT_EQ(install.spread, 0.25f);
    EXPECT_TRUE(install.is3DEnabled);
    EXPECT_EQ(install.attenuationDistanceScaling, 3.0f);
    EXPECT_TRUE(install.attenuationResolved);
    EXPECT_EQ(install.resolvedAttenuation, harkentest::ObjectId(900u));
    EXPECT_TRUE(result.notes.empty());
}

TEST(ProgramRegistrationInstall, UnresolvedAttenuationEmitsNote) {
    ProjectInput in = InstallWorld();
    in.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(20);
    const std::optional<SegmentProgram> program = CompileWithRouting(in);
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base;
    base.resolvedVoiceSendSlots = 4;
    base.busIdMap = {{harkentest::ObjectId(20), VN_BUS_MASTER}};
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    ASSERT_TRUE(result.ok) << static_cast<int>(result.refusal);
    ASSERT_EQ(result.notes.size(), 1u);
    EXPECT_EQ(result.notes[0].code,
              harkenprogramreg::RegistrationNoteCode::
                  AttenuationUnresolvedNoBinding);
    EXPECT_TRUE(result.notes[0].lineage.hasSegment);
    EXPECT_EQ(result.notes[0].lineage.segmentId, harkentest::ObjectId(1u));
    EXPECT_EQ(result.notes[0].lineage.trackId, harkentest::ObjectId(2u));
    EXPECT_EQ(result.notes[0].lineage.clipId, harkentest::ObjectId(100u));
    EXPECT_EQ(result.notes[0].lineage.occurrenceOrdinal, 0u);
    EXPECT_EQ(result.notes[0].relatedId, harkentest::ObjectId(0u));
}

TEST(ProgramRegistrationInstall, AttenuationTargetUnmappedRefused) {
    ProjectInput in = InstallWorld();
    in.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(20);
    in.nodes[2].spatial.attenuationRef = harkentest::StudioObjectId(50);
    in.nodes[2].presence.Set(harkenplayableproperties::PlayablePropertyIdV1::SpatialAttenuationRef);
    const std::optional<SegmentProgram> program = CompileWithRouting(in);
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base;
    base.resolvedVoiceSendSlots = 4;
    base.busIdMap = {{harkentest::ObjectId(20), VN_BUS_MASTER}};
    // attenuationIdMap deliberately empty: the target is not a registered profile.
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.refusal,
              harkenprogramreg::RegistrationRefusal::AttenuationTargetUnmapped);
}

// ---- spatial: engine layer (null backend, real seam) ---------------------

// An install routed to master with the given spatial placement, no sends.
harkenprogramreg::VoiceStartInstall SpatialMasterInstall(float panning,
                                                        float spread,
                                                        bool is3D) {
    harkenprogramreg::VoiceStartInstall install = MasterInstall(0.0f, 1.0f, false);
    install.panning = panning;
    install.spread = spread;
    install.is3DEnabled = is3D;
    return install;
}

TEST(ProgramRegistrationInstall, PlayInstallsPanAsChannelAsymmetry) {
    EngineSession session(InstallEngineConfig());
    ASSERT_EQ(session.InitResult(), VN_OK);
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(Vn_RegisterPCM(kSound, &desc), VN_OK);

    // Hard right: the installed pan reaches the voice, so the right channel
    // carries more energy than the left from the first rendered block.
    VnVoiceHandle handle = 0;
    ASSERT_EQ(PlayInstall(
                  SpatialMasterInstall(/*panning=*/1.0f, /*spread=*/0.0f,
                                       /*is3D=*/false),
                  &handle),
              VN_OK);
    Capture capture;
    PumpAndCapture(session.Engine(), 2, capture);
    EXPECT_GT(Peak(capture.right), harkentest::kSilenceFloor);
    EXPECT_GT(Peak(capture.right), Peak(capture.left));
}

TEST(ProgramRegistrationInstall, PlayInstalls3DDistanceScaleAndAttenuationBind) {
    EngineSession session(InstallEngineConfig());
    ASSERT_EQ(session.InitResult(), VN_OK);
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(Vn_RegisterPCM(kSound, &desc), VN_OK);
    const VnAttenuationDesc atten =
        harkentest::MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(Vn_RegisterAttenuation(harkentest::ObjectId(77), &atten), VN_OK);

    harkenprogramreg::VoiceStartInstall install =
        SpatialMasterInstall(/*panning=*/0.0f, /*spread=*/50.0f, /*is3D=*/true);
    install.attenuationDistanceScaling = 4.0f;
    install.attenuationResolved = true;
    install.resolvedAttenuation = harkentest::ObjectId(77);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(PlayInstall(install, &handle),
              VN_OK);
    session.Engine()->nullBackend.Pump(1);

    harken::Spatializer::SpatialVoiceState spatial;
    ASSERT_TRUE(session.Engine()->spatial.CaptureVoice(
        harkentest::RenderHandle(session.Engine(), handle), &spatial));
    const harken::VoiceRenderContext* rendered = session.Engine()->voices.Resolve(
        harkentest::RenderHandle(session.Engine(), handle));
    ASSERT_NE(nullptr, rendered);
    EXPECT_FLOAT_EQ(0.5f, rendered->width);
    EXPECT_TRUE(spatial.spatialized);            // is3D reached the play descriptor
    EXPECT_EQ(spatial.distanceScale, 4.0f);      // distance scale installed
    EXPECT_EQ(spatial.attenuationId, harkentest::ObjectId(77u));       // the profile bound
}

TEST(ProgramRegistrationInstall, CompiledDefinitionAttenuationReachesLiveVoice) {
    ProjectInput in = InstallWorld();
    harkenprojectschema::InputNode layer = fx::Node(ContentKind::Layer, 4);
    layer.guid = harkentest::StudioObjectId(4);
    layer.ownedChildren = {fx::G(1)};
    layer.spatial.attenuationRef = harkentest::StudioObjectId(50);
    layer.spatial.is3DEnabled = true;
    layer.presence.Set(harkenplayableproperties::PlayablePropertyIdV1::SpatialAttenuationRef);
    in.nodes[2].mainOutputBusRef = harkentest::StudioObjectId(20);
    in.nodes[2].spatial = layer.spatial;
    in.nodes[2].presence.Set(harkenplayableproperties::PlayablePropertyIdV1::SpatialAttenuationRef);
    in.nodes.push_back(layer);
    in.roots = {fx::G(4)};
    const std::optional<SegmentProgram> program = CompileWithRouting(in);
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base;
    base.resolvedVoiceSendSlots = 4;
    base.busIdMap = {{harkentest::ObjectId(20), VN_BUS_MASTER}};
    base.attenuationIdMap = {{harkentest::ObjectId(50), harkentest::ObjectId(77)}};
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    ASSERT_TRUE(result.ok) << static_cast<int>(result.refusal);
    ASSERT_EQ(result.program->OccurrenceCount(), 1u);
    const harkenprogramreg::VoiceStartInstall& install =
        result.program->Install(0);
    ASSERT_TRUE(install.attenuationResolved);
    ASSERT_EQ(install.resolvedAttenuation, harkentest::ObjectId(77u));

    EngineSession session(InstallEngineConfig());
    ASSERT_EQ(session.InitResult(), VN_OK);
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc pcm = MonoDesc(dc, kRate);
    ASSERT_EQ(Vn_RegisterPCM(kSound, &pcm), VN_OK);
    const VnAttenuationDesc attenuation =
        harkentest::MakeAttenuationDesc(1.0f, 100.0f);
    ASSERT_EQ(Vn_RegisterAttenuation(harkentest::ObjectId(77), &attenuation), VN_OK);
    VnVoiceHandle handle = 0;
    ASSERT_EQ(PlayInstall(install, &handle),
              VN_OK);
    session.Engine()->nullBackend.Pump(1);
    harken::Spatializer::SpatialVoiceState spatial;
    ASSERT_TRUE(session.Engine()->spatial.CaptureVoice(
        harkentest::RenderHandle(session.Engine(), handle), &spatial));
    EXPECT_TRUE(spatial.spatialized);
    EXPECT_EQ(spatial.attenuationId, harkentest::ObjectId(77u));
}

TEST(ProgramRegistrationInstall, FailedSpatialInstallAbandonsTheInstall) {
    EngineSession session(InstallEngineConfig());
    ASSERT_EQ(session.InitResult(), VN_OK);
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(Vn_RegisterPCM(kSound, &desc), VN_OK);
    const VnSnapshot before = Snapshot();

    harkenprogramreg::VoiceStartInstall install =
        SpatialMasterInstall(/*panning=*/0.0f, /*spread=*/0.0f,
                             /*is3D=*/true);
    install.attenuationResolved = true;
    install.resolvedAttenuation = harkentest::ObjectId(999);
    constexpr VnVoiceHandle kInitialHandle = 0x1234;
    VnVoiceHandle handle = kInitialHandle;
    EXPECT_EQ(PlayInstall(install, &handle),
              VN_UNKNOWN_ID);
    EXPECT_EQ(handle, 0u);

    session.Engine()->nullBackend.Pump(1);
    ASSERT_EQ(Vn_Update(), VN_OK);
    const VnSnapshot after = Snapshot();
    EXPECT_EQ(after.activeVoiceCount, before.activeVoiceCount);
    EXPECT_EQ(after.pendingStartCount, before.pendingStartCount);
}

TEST(ProgramRegistrationInstall, PendingReplayInstallsPanFromFirstBlock) {
    // A voice that starts through the deferred (pending) path must come up with
    // its installed pan already applied — its first audible block is asymmetric,
    // never centered-then-jumped. maxVoices 1 with steal-oldest forces the
    // incoming voice down the PendingStartRecord -> replay path.
    VnConfig config = MakeNullConfig(/*maxVoices=*/1, kBlock, kRate);
    config.voiceStealPolicy = 2;  // steal-oldest, deferred
    EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* engine = session.Engine();
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(Vn_RegisterPCM(kSound, &desc), VN_OK);

    VnVoiceHandle incumbent = 0;
    VnPlayDesc play = harkentest::MakePlayDesc(kSound);
    ASSERT_EQ(harkentest::PlayRaw(&play, &incumbent), VN_OK);
    engine->nullBackend.Pump(1);

    // The incoming voice steals the single slot and becomes pending.
    VnVoiceHandle incoming = 0;
    ASSERT_EQ(harkentest::PlayRaw(&play, &incoming), VN_OK);
    ASSERT_EQ(engine->ledger.Classify(incoming).kind,
              harken::VoiceLedger::HandleKind::Pending);
    const std::uint32_t slot = engine->ledger.Classify(incoming).ledgerSlot;

    // Stage pan (hard right) and width on the pending voice: both land in the
    // staging record for replay at the deferred start.
    ASSERT_EQ(Vn_SetVoicePan(incoming, 1.0f), VN_OK);
    ASSERT_EQ(Vn_SetVoiceWidth(incoming, 0.5f), VN_OK);
    EXPECT_TRUE(engine->pendingStarts[slot].hasPan);
    EXPECT_EQ(engine->pendingStarts[slot].pan, 1.0f);
    EXPECT_TRUE(engine->pendingStarts[slot].hasWidth);
    EXPECT_EQ(engine->pendingStarts[slot].width, 0.5f);

    // Drive the deferred start: the victim declicks, the start issues and replays
    // the staged pan, then the incoming voice renders its first block.
    engine->nullBackend.Pump(1);
    ASSERT_EQ(Vn_Update(), VN_OK);
    Capture capture;
    PumpAndCapture(engine, 1, capture);
    // The very first audible block of the deferred voice is already panned right.
    EXPECT_GT(Peak(capture.right), harkentest::kSilenceFloor);
    EXPECT_GT(Peak(capture.right), Peak(capture.left));
}

// ---- effect chain: the load-time admission gate --------------------------
//
// The compiler already refused a chain past the fixed slot layout; these pin the
// separate load-time gate, which admits the same chain against what one session
// resolved: its slot budget, its registered effect types and its per-type pools.
// Each condition refuses on its own so a caller learns which one failed.

// A declared chain of `count` effects in one line, all of one effect type, each
// carrying parameter id 0.
harkenprojectschema::SignalGraph ChainOfType(std::uint32_t count,
                                            std::uint32_t effectType) {
    harkenprojectschema::SignalGraph graph;
    for (std::uint32_t i = 0; i < count; ++i) {
        harkenprojectschema::EffectNode node;
        node.nodeId = i + 1;
        node.effectType = effectType;
        node.params = {harkenprojectschema::EffectParam{0, 0x3f800000u}};
        graph.nodes.push_back(node);
        if (i > 0) {
            graph.edges.push_back(harkenprojectschema::GraphEdge{i, i + 1});
        }
    }
    return graph;
}

// InstallWorld with the given chain declared on the subject Sound.
ProjectInput ChainInstallWorld(const harkenprojectschema::SignalGraph& graph) {
    ProjectInput in = InstallWorld();
    in.nodes[2].hasSignalGraph = true;
    in.nodes[2].signalGraph = graph;
    return in;
}

// A synthetic availability row for a type with `parameterCount` parameters, each
// accepting [0, 1], laid out as the header plus one float per parameter. Synthetic
// on purpose: these tests pin the admission rules, not any built-in effect, and the
// writer's agreement with the real registry is pinned separately.
harkenprogramreg::EffectTypeAvailability SyntheticType(
    std::uint32_t parameterCount, std::uint32_t poolCapacity) {
    harkenprogramreg::EffectTypeAvailability available;
    available.poolCapacity = poolCapacity;
    available.descriptorSize =
        (harkenprogramreg::kEffectDescriptorHeaderBytes + 4u * parameterCount + 7u) & ~7u;
    available.descriptorVintage =
        harkenprogramreg::kSupportedEffectDescriptorVintage;
    available.hostsVoiceInserts = true;
    available.parameters.assign(parameterCount,
                               harkenprogramreg::EffectParameterRange{0.0f, 1.0f});
    return available;
}

// Params that admit a chain of effect type 5, whose entries each author one
// parameter.
harkenprogramreg::RegistrationParams ChainBase() {
    harkenprogramreg::RegistrationParams base;
    base.resolvedVoiceSendSlots = 4;
    base.callerDefaultOutputBus = harkentest::ObjectId(900);
    base.resolvedVoiceFxSlots = 4;
    base.effectTypes = {{5, SyntheticType(/*parameterCount=*/1,
                                          /*poolCapacity=*/4)}};
    return base;
}

// One parameter value out of a lowered step's descriptor. Read straight from the
// bytes because these types are synthetic and the engine's reverse mapping only
// knows registered ones; that the layout read here is the layout the engine reads
// is pinned against the real registry by the descriptor writer's own round trip.
float StepParam(const harkenprogramreg::VoiceEffectSlot& step,
                std::uint32_t index) {
    float value = 0.0f;
    std::memcpy(&value,
                step.descriptor.data +
                    harkenprogramreg::kEffectDescriptorHeaderBytes +
                    4u * index,
                sizeof(value));
    return value;
}

// A chain naming a real built-in effect survives the whole pipeline, with the
// engine's own enumerator as the authored type. The value matters: the engine
// numbers its effect types from zero, so the first one carries the type id a
// schema sentinel is most tempted to claim. Authoring that type and following it
// to a lowered slot is what proves the two id spaces have not collided — a
// schema-side sentinel sharing the value would refuse this chain in the
// front-end, and the whole effect would be unreachable from an authored graph.
TEST(ProgramRegistrationInstall, ChainOfTheEngineFirstEffectTypeSurvives) {
    constexpr std::uint32_t kFeedbackDelay = VN_EFFECT_FEEDBACK_DELAY;
    static_assert(kFeedbackDelay == 0u,
                  "the engine's first effect type is the zero id space this "
                  "checks a schema sentinel does not claim");

    const std::optional<SegmentProgram> program =
        CompileWithRouting(ChainInstallWorld(ChainOfType(1, kFeedbackDelay)));
    ASSERT_TRUE(program.has_value())
        << "an authored chain of the engine's first effect type was refused "
           "before registration";

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base = ChainBase();
    base.effectTypes = {{kFeedbackDelay, SyntheticType(/*parameterCount=*/1,
                                                       /*poolCapacity=*/4)}};
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    ASSERT_TRUE(result.ok) << static_cast<int>(result.refusal);

    const harkenprogramreg::VoiceStartInstall& install =
        result.program->Install(0);
    ASSERT_EQ(install.effectSlots.size(), 1u);
    EXPECT_EQ(static_cast<std::uint32_t>(install.effectSlots[0].descriptor.type),
              kFeedbackDelay);
}

TEST(ProgramRegistrationInstall, AdmittedChainLowersOntoAscendingSlots) {
    const std::optional<SegmentProgram> program =
        CompileWithRouting(ChainInstallWorld(ChainOfType(2, 5)));
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    const harkenprogramreg::RegistrationResult result =
        Register(*program, ChainBase(), commit, instances);
    ASSERT_TRUE(result.ok) << static_cast<int>(result.refusal);

    const harkenprogramreg::VoiceStartInstall& install =
        result.program->Install(0);
    ASSERT_EQ(install.effectSlots.size(), 2u);
    for (std::uint32_t i = 0; i < 2u; ++i) {
        EXPECT_EQ(install.effectSlots[i].slot, i);
        EXPECT_EQ(static_cast<std::uint32_t>(install.effectSlots[i].descriptor.type),
                  5u);
        EXPECT_EQ(install.effectSlots[i].descriptor.size,
                  harkenprogramreg::kEffectDescriptorHeaderBytes + 8u);
        EXPECT_EQ(StepParam(install.effectSlots[i], 0), 1.0f);
    }
}

TEST(ProgramRegistrationInstall, ZeroParameterTypeInstallsHeaderOnly) {
    harkenprojectschema::SignalGraph graph = ChainOfType(1, 5);
    graph.nodes[0].params.clear();
    const std::optional<SegmentProgram> program =
        CompileWithRouting(ChainInstallWorld(graph));
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base = ChainBase();
    base.effectTypes[5] =
        SyntheticType(/*parameterCount=*/0, /*poolCapacity=*/4);
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    ASSERT_TRUE(result.ok) << static_cast<int>(result.refusal);
    const harkenprogramreg::VoiceStartInstall& install =
        result.program->Install(0);
    ASSERT_EQ(install.effectSlots.size(), 1u);
    const harkenprogramreg::EffectDescriptorBytes& descriptor =
        install.effectSlots[0].descriptor;
    EXPECT_EQ(static_cast<std::uint32_t>(descriptor.type), 5u);
    EXPECT_EQ(descriptor.size,
              harkenprogramreg::kEffectDescriptorHeaderBytes);
    EXPECT_EQ(descriptor.AsDesc()->structSize,
              harkenprogramreg::kEffectDescriptorHeaderBytes);
    EXPECT_EQ(descriptor.AsDesc()->reserved0, 0u);
}

TEST(ProgramRegistrationInstall, ParameterZeroRequiresOneRegisteredParameter) {
    const std::optional<SegmentProgram> program =
        CompileWithRouting(ChainInstallWorld(ChainOfType(1, 5)));
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base = ChainBase();
    base.effectTypes[5] =
        SyntheticType(/*parameterCount=*/0, /*poolCapacity=*/4);
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.refusal,
              harkenprogramreg::RegistrationRefusal::
                  EffectTypeParameterUnknown);
    EXPECT_EQ(result.refusalContext.effectType, 5u);
    EXPECT_EQ(result.refusalContext.expected, 0u);
    EXPECT_EQ(result.refusalContext.actual, 0u);
}

TEST(ProgramRegistrationInstall, FixedSlotGateRejectsNineNodeCompile) {
    const FrontEndResult front =
        CompileFrontEnd(ChainInstallWorld(ChainOfType(9, 5)));
    EXPECT_FALSE(front.ok());
    EXPECT_FALSE(front.project.has_value());
    EXPECT_FALSE(front.facts.has_value());
    EXPECT_FALSE(front.resolution.has_value());
    EXPECT_FALSE(front.sends.has_value());
    EXPECT_FALSE(front.effectChains.has_value());
    ASSERT_EQ(front.diagnostics.size(), 1u)
        << harkenprojectschema::RenderDiagnostics(front.diagnostics);
    EXPECT_EQ(front.diagnostics[0].code,
              harkenprojectschema::DiagnosticCode::
                  EffectChainExceedsSlotLimit);
    EXPECT_EQ(front.diagnostics[0].primary, fx::G(3));
    EXPECT_EQ(front.diagnostics[0].detail, 9u);
}

TEST(ProgramRegistrationInstall, EightNodeChainCompilesAndRegisters) {
    const std::optional<SegmentProgram> program =
        CompileWithRouting(ChainInstallWorld(ChainOfType(8, 5)));
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base = ChainBase();
    base.resolvedVoiceFxSlots = harkenprojectschema::kFixedVoiceFxSlotCount;
    base.effectTypes[5].poolCapacity = harkenprojectschema::kFixedVoiceFxSlotCount;
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    ASSERT_TRUE(result.ok) << static_cast<int>(result.refusal);
    const harkenprogramreg::VoiceStartInstall& install =
        result.program->Install(0);
    ASSERT_EQ(install.effectSlots.size(),
              harkenprojectschema::kFixedVoiceFxSlotCount);
    for (std::uint32_t i = 0;
         i < harkenprojectschema::kFixedVoiceFxSlotCount; ++i) {
        EXPECT_EQ(install.effectSlots[i].slot, i);
    }
}

TEST(ProgramRegistrationInstall, ChainLongerThanResolvedSlotsIsRefused) {
    // Three entries fit the layout ceiling the compiler enforces but not this
    // session's resolved budget of two.
    const std::optional<SegmentProgram> program =
        CompileWithRouting(ChainInstallWorld(ChainOfType(3, 5)));
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base = ChainBase();
    base.resolvedVoiceFxSlots = 2;
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.refusal,
              harkenprogramreg::RegistrationRefusal::
                  EffectChainExceedsResolvedSlots);
}

TEST(ProgramRegistrationInstall, UnregisteredEffectTypeIsRefused) {
    const std::optional<SegmentProgram> program =
        CompileWithRouting(ChainInstallWorld(ChainOfType(1, 77)));
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    const harkenprogramreg::RegistrationResult result =
        Register(*program, ChainBase(), commit, instances);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.refusal,
              harkenprogramreg::RegistrationRefusal::EffectTypeUnavailable);
}

TEST(ProgramRegistrationInstall, BusOnlyEffectTypeCannotHostAVoiceChain) {
    const std::optional<SegmentProgram> program =
        CompileWithRouting(ChainInstallWorld(ChainOfType(1, 5)));
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base = ChainBase();
    base.effectTypes[5].hostsVoiceInserts = false;
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.refusal,
              harkenprogramreg::RegistrationRefusal::EffectTypeCannotHostVoice);
}

TEST(ProgramRegistrationInstall, ParameterIdPastTheTypesCountIsRefused) {
    // Parameter id 2 addresses the third index of a type that exposes one.
    harkenprojectschema::SignalGraph graph = ChainOfType(1, 5);
    graph.nodes[0].params = {harkenprojectschema::EffectParam{2, 0u}};
    const std::optional<SegmentProgram> program =
        CompileWithRouting(ChainInstallWorld(graph));
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    const harkenprogramreg::RegistrationResult result =
        Register(*program, ChainBase(), commit, instances);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.refusal,
              harkenprogramreg::RegistrationRefusal::EffectTypeParameterUnknown);
    EXPECT_EQ(result.refusalContext.effectType, 5u);
    EXPECT_EQ(result.refusalContext.expected, 1u);  // the parameter count
    EXPECT_EQ(result.refusalContext.actual, 2u);    // the id that overran it
}

// A parameter index nobody authored has no legal value to fill it with, so a
// sparse payload is refused rather than completed by guesswork.
TEST(ProgramRegistrationInstall, SparseParameterSetIsRefused) {
    // The type exposes two parameters; the entry authors only index 0.
    const std::optional<SegmentProgram> program =
        CompileWithRouting(ChainInstallWorld(ChainOfType(1, 5)));
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base = ChainBase();
    base.effectTypes = {{5, SyntheticType(/*parameterCount=*/2,
                                          /*poolCapacity=*/4)}};
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.refusal, harkenprogramreg::RegistrationRefusal::
                                  EffectTypeParameterSetIncomplete);
    EXPECT_EQ(result.refusalContext.expected, 2u);  // indices the type exposes
    EXPECT_EQ(result.refusalContext.actual, 1u);    // indices the entry authored
}

// A malformed compiled artifact must still be rejected at registration even
// though the front end normally prevents duplicate authored parameter ids.
TEST(ProgramRegistrationInstall,
     DuplicateParameterInCompiledArtifactIsRefused) {
    const FrontEndResult front =
        CompileFrontEnd(ChainInstallWorld(ChainOfType(1, 5)));
    ASSERT_TRUE(front.ok())
        << harkenprojectschema::RenderDiagnostics(front.diagnostics);
    ASSERT_TRUE(front.project.has_value());
    ASSERT_TRUE(front.facts.has_value());
    ASSERT_TRUE(front.resolution.has_value());
    ASSERT_TRUE(front.sends.has_value());
    ASSERT_TRUE(front.effectChains.has_value());

    harkenprojectschema::EffectChainSelectionFacts effectChains =
        *front.effectChains;
    ASSERT_EQ(effectChains.chains.size(), 1u);
    ASSERT_EQ(effectChains.chains[0].paramBlocks.size(), 1u);
    ASSERT_EQ(effectChains.chains[0].paramBlocks[0].params.size(), 1u);
    effectChains.chains[0].paramBlocks[0].params.push_back(
        effectChains.chains[0].paramBlocks[0].params[0]);

    const std::optional<SegmentIR> ir = BuildSegmentIR(
        *front.project, *front.facts, Resources(*front.project),
        *front.resolution, *front.sends, effectChains);
    ASSERT_TRUE(ir.has_value());
    const std::optional<SegmentProgram> program = BuildSegmentProgram(*ir);
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base = ChainBase();
    base.effectTypes[5] = SyntheticType(/*parameterCount=*/2,
                                        /*poolCapacity=*/4);
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.refusal, harkenprogramreg::RegistrationRefusal::
                                  EffectTypeParameterSetIncomplete);
    EXPECT_EQ(result.refusalContext.effectType, 5u);
    EXPECT_EQ(result.refusalContext.expected, 2u);
    EXPECT_EQ(result.refusalContext.actual, 0u);
}

// A value outside the parameter's resolved range fails while the program is being
// registered, not when a voice starts.
TEST(ProgramRegistrationInstall, ParameterValueOutsideItsRangeIsRefused) {
    harkenprojectschema::SignalGraph graph = ChainOfType(1, 5);
    // 2.0f, outside the synthetic type's [0, 1].
    graph.nodes[0].params = {harkenprojectschema::EffectParam{0, 0x40000000u}};
    const std::optional<SegmentProgram> program =
        CompileWithRouting(ChainInstallWorld(graph));
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    const harkenprogramreg::RegistrationResult result =
        Register(*program, ChainBase(), commit, instances);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.refusal, harkenprogramreg::RegistrationRefusal::
                                  EffectTypeParameterValueOutOfRange);
    EXPECT_EQ(result.refusalContext.expected, 0u);  // the parameter index
    EXPECT_EQ(result.refusalContext.actual, 0x40000000u);  // the offending bits
}

TEST(ProgramRegistrationInstall, NaNParameterValueIsRefused) {
    harkenprojectschema::SignalGraph graph = ChainOfType(1, 5);
    graph.nodes[0].params = {
        harkenprojectschema::EffectParam{0, 0x7fc00000u}};
    const std::optional<SegmentProgram> program =
        CompileWithRouting(ChainInstallWorld(graph));
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    const harkenprogramreg::RegistrationResult result =
        Register(*program, ChainBase(), commit, instances);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.refusal, harkenprogramreg::RegistrationRefusal::
                                  EffectTypeParameterValueOutOfRange);
    EXPECT_EQ(result.refusalContext.effectType, 5u);
    EXPECT_EQ(result.refusalContext.expected, 0u);
    EXPECT_EQ(result.refusalContext.actual, 0x7fc00000u);
}

// The descriptor gates: a size that is not the header plus one float per parameter,
// or a layout vintage this build does not know, refuses the type instead of writing
// bytes the engine would read as some other struct.
TEST(ProgramRegistrationInstall, DescriptorSizeMismatchRefusesTheType) {
    const std::optional<SegmentProgram> program =
        CompileWithRouting(ChainInstallWorld(ChainOfType(1, 5)));
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base = ChainBase();
    base.effectTypes[5].descriptorSize += 4;  // one float too wide
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.refusal, harkenprogramreg::RegistrationRefusal::
                                  EffectTypeDescriptorLayoutUnsupported);
    EXPECT_EQ(result.refusalContext.effectType, 5u);
    EXPECT_EQ(result.refusalContext.expected,
              harkenprogramreg::kEffectDescriptorHeaderBytes + 8u);
    EXPECT_EQ(result.refusalContext.actual,
              harkenprogramreg::kEffectDescriptorHeaderBytes + 12u);
}

TEST(ProgramRegistrationInstall, UnknownDescriptorVintageRefusesTheType) {
    const std::optional<SegmentProgram> program =
        CompileWithRouting(ChainInstallWorld(ChainOfType(1, 5)));
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base = ChainBase();
    base.effectTypes[5].descriptorVintage =
        harkenprogramreg::kSupportedEffectDescriptorVintage + 1;
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.refusal, harkenprogramreg::RegistrationRefusal::
                                  EffectTypeDescriptorVintageUnsupported);
    EXPECT_EQ(result.refusalContext.effectType, 5u);
    EXPECT_EQ(result.refusalContext.expected,
              harkenprogramreg::kSupportedEffectDescriptorVintage);
    EXPECT_EQ(result.refusalContext.actual,
              harkenprogramreg::kSupportedEffectDescriptorVintage + 1u);
}

TEST(ProgramRegistrationInstall,
     DescriptorVintageRefusalPrecedesParameterCapacity) {
    harkenprojectschema::SignalGraph graph = ChainOfType(1, 5);
    graph.nodes[0].params.clear();
    for (std::uint32_t paramId = 0; paramId < 33; ++paramId) {
        graph.nodes[0].params.push_back(
            harkenprojectschema::EffectParam{paramId, 0u});
    }
    const std::optional<SegmentProgram> program =
        CompileWithRouting(ChainInstallWorld(graph));
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base = ChainBase();
    base.effectTypes[5] = SyntheticType(/*parameterCount=*/33,
                                        /*poolCapacity=*/4);
    base.effectTypes[5].descriptorVintage =
        harkenprogramreg::kSupportedEffectDescriptorVintage + 1u;
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.refusal, harkenprogramreg::RegistrationRefusal::
                                  EffectTypeDescriptorVintageUnsupported);
    EXPECT_EQ(result.refusalContext.effectType, 5u);
    EXPECT_EQ(result.refusalContext.expected,
              harkenprogramreg::kSupportedEffectDescriptorVintage);
    EXPECT_EQ(result.refusalContext.actual,
              harkenprogramreg::kSupportedEffectDescriptorVintage + 1u);
}

TEST(ProgramRegistrationInstall, ChainNeedingMoreInstancesThanThePoolIsRefused) {
    const std::optional<SegmentProgram> program =
        CompileWithRouting(ChainInstallWorld(ChainOfType(3, 5)));
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base = ChainBase();
    base.effectTypes[5].poolCapacity = 2;  // three instances demanded
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    EXPECT_FALSE(result.ok);
    EXPECT_EQ(result.refusal,
              harkenprogramreg::RegistrationRefusal::EffectTypePoolInsufficient);
}

TEST(ProgramRegistrationInstall, SelectedEmptyChainInstallsNothingAndNotes) {
    const std::optional<SegmentProgram> program = CompileWithRouting(
        ChainInstallWorld(harkenprojectschema::SignalGraph{}));
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    // A session that hosts no effects at all still admits an empty chain: an
    // override to no effects asks for no slot, type or instance.
    harkenprogramreg::RegistrationParams base = ChainBase();
    base.resolvedVoiceFxSlots = 0;
    base.effectTypes.clear();
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    ASSERT_TRUE(result.ok) << static_cast<int>(result.refusal);
    EXPECT_TRUE(result.program->Install(0).effectSlots.empty());

    bool sawEmptyNote = false;
    for (const harkenprogramreg::RegistrationNote& note : result.notes) {
        if (note.code ==
            harkenprogramreg::RegistrationNoteCode::EffectChainSelectedEmpty) {
            sawEmptyNote = true;
            EXPECT_EQ(note.relatedId, harkentest::ObjectId(3u));  // the declaring Sound
        }
    }
    EXPECT_TRUE(sawEmptyNote)
        << "an authored empty chain must be distinguishable from no chain";
}

TEST(ProgramRegistrationInstall, EveryNoteCarriesItsOccurrenceLineage) {
    ProjectInput in = InstallWorld();
    in.nodes[1].clips[0].semanticOrderKey = 1;
    harkenprojectschema::InputClip secondClip = fx::Clip(101, 4, 0, 2, 1);
    secondClip.guid = harkentest::StudioObjectId(101);
    in.nodes[1].clips.push_back(secondClip);

    in.nodes[2].spatial.attenuationRef = harkentest::StudioObjectId(50);

    in.nodes[2].presence.Set(harkenplayableproperties::PlayablePropertyIdV1::SpatialAttenuationRef);
    in.nodes[2].hasSignalGraph = true;
    in.nodes[2].signalGraph = harkenprojectschema::SignalGraph{};

    harkenprojectschema::InputNode secondSound = fx::Node(ContentKind::Sound, 4);
    secondSound.guid = harkentest::StudioObjectId(4);
    secondSound.hasSignalGraph = true;
    secondSound.signalGraph = harkenprojectschema::SignalGraph{};
    in.nodes.push_back(secondSound);
    in.nodes[1].ownedChildren.push_back(fx::G(4));

    const std::optional<SegmentProgram> program = CompileWithRouting(in);
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base = ChainBase();
    base.resolvedVoiceFxSlots = 0;
    base.effectTypes.clear();
    base.soundIdMap = {{harkentest::ObjectId(4), harkentest::ObjectId(2)}};
    base.attenuationIdMap = {{harkentest::ObjectId(50), harkentest::ObjectId(900)}};
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    ASSERT_TRUE(result.ok) << static_cast<int>(result.refusal);
    ASSERT_EQ(result.program->OccurrenceCount(), 2u);
    ASSERT_EQ(result.notes.size(), 3u);

    const harkenprogramreg::RegistrationNoteCode expectedCodes[] = {
        harkenprogramreg::RegistrationNoteCode::EffectChainSelectedEmpty,
        harkenprogramreg::RegistrationNoteCode::AttenuationUnresolvedNoBinding,
        harkenprogramreg::RegistrationNoteCode::EffectChainSelectedEmpty};
    const std::size_t occurrenceForNote[] = {0u, 1u, 1u};
    for (std::size_t i = 0; i < result.notes.size(); ++i) {
        EXPECT_EQ(result.notes[i].code, expectedCodes[i]);
        ExpectSameLineage(result.notes[i].lineage,
                          result.program->Lineage(occurrenceForNote[i]));
    }
}

TEST(ProgramRegistrationInstall, NoSelectedChainInstallsNothingAndDoesNotNote) {
    const std::optional<SegmentProgram> program =
        CompileWithRouting(InstallWorld());
    ASSERT_TRUE(program.has_value());

    harken::PlayableInstanceTable instances(4);
    harken::LogicCommitCoordinator commit;
    harkenprogramreg::RegistrationParams base = ChainBase();
    base.resolvedVoiceFxSlots = 0;
    base.effectTypes.clear();
    const harkenprogramreg::RegistrationResult result =
        Register(*program, base, commit, instances);
    ASSERT_TRUE(result.ok) << static_cast<int>(result.refusal);
    EXPECT_TRUE(result.program->Install(0).effectSlots.empty());
    for (const harkenprogramreg::RegistrationNote& note : result.notes) {
        EXPECT_NE(note.code,
                  harkenprogramreg::RegistrationNoteCode::
                      EffectChainSelectedEmpty);
    }
}

// ---- effect chain: installation against the live engine ------------------

// A Gain step at the given dB, built the way registration builds one.
harkenprogramreg::VoiceEffectSlot GainStep(std::uint32_t slot, float gainDb) {
    harkenprogramreg::VoiceEffectSlot step;
    step.slot = slot;
    const float params[1] = {gainDb};
    EXPECT_EQ(harkenprogramreg::WriteEffectDescriptor(
                  VN_EFFECT_GAIN, sizeof(VnGainDesc),
                  harkenprogramreg::kSupportedEffectDescriptorVintage, params, 1,
                  &step.descriptor),
              harkenprogramreg::EffectDescriptorWriteStatus::Ok);
    return step;
}

// The generically built descriptor reaches the engine and takes effect: a voice
// carrying a Gain step at -20 dB is audibly quieter than the same voice without it.
TEST(ProgramRegistrationInstall, InstalledChainStepReachesTheEngine) {
    const std::vector<float> dc = MakeDC(kRate, 0.5f);

    Capture plain;
    {
        EngineSession session(InstallEngineConfig());
        ASSERT_EQ(session.InitResult(), VN_OK);
        const VnPCMDesc desc = MonoDesc(dc, kRate);
        ASSERT_EQ(Vn_RegisterPCM(kSound, &desc), VN_OK);
        VnVoiceHandle handle = 0;
        ASSERT_EQ(PlayInstall(
                      MasterInstall(0.0f, 1.0f, false), &handle),
                  VN_OK);
        PumpAndCapture(session.Engine(), 3, plain);
    }

    Capture attenuated;
    {
        EngineSession session(InstallEngineConfig());
        ASSERT_EQ(session.InitResult(), VN_OK);
        const VnPCMDesc desc = MonoDesc(dc, kRate);
        ASSERT_EQ(Vn_RegisterPCM(kSound, &desc), VN_OK);
        harkenprogramreg::VoiceStartInstall install =
            MasterInstall(0.0f, 1.0f, false);
        install.effectSlots = {GainStep(0, -20.0f)};
        VnVoiceHandle handle = 0;
        ASSERT_EQ(
            PlayInstall(install, &handle),
            VN_OK);
        ASSERT_NE(handle, 0u);
        PumpAndCapture(session.Engine(), 3, attenuated);
    }

    HARKENTEST_ASSERT_NON_SILENT(plain.left);
    HARKENTEST_ASSERT_NON_SILENT(attenuated.left);
    EXPECT_LT(Peak(attenuated.left), Peak(plain.left))
        << "the installed gain step did not reach the voice";
}

// A step naming a slot the session does not have is refused by the seam, and the
// primitive cancels the install rather than committing a voice that would run an
// incomplete chain. This covers the ordinary rollback; the saturated-queue
// refusal is covered separately below.
TEST(ProgramRegistrationInstall, ARefusedChainStepAbandonsTheInstall) {
    EngineSession session(InstallEngineConfig());
    ASSERT_EQ(session.InitResult(), VN_OK);
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(Vn_RegisterPCM(kSound, &desc), VN_OK);

    harkenprogramreg::VoiceStartInstall install =
        MasterInstall(0.0f, 1.0f, false);
    // InstallEngineConfig resolves one effect slot per voice, so slot 1 is past it.
    install.effectSlots = {GainStep(1, -6.0f)};
    VnVoiceHandle handle = 0;
    EXPECT_EQ(PlayInstall(install, &handle),
              VN_INVALID_ARG);
    EXPECT_EQ(handle, 0u);

    Capture capture;
    PumpAndCapture(session.Engine(), 2, capture);
    EXPECT_LE(Peak(capture.left), harkentest::kSilenceFloor)
        << "a voice whose chain was refused must never sound";
}

// A saturated command queue can no longer strand a voice on the caller. The
// install stages everything without publishing, so the shortfall surfaces at one
// point — the commit — where the whole install either goes out or does not.
// The primitive then cancels a voice that never sounded, which needs no queue
// room and cannot be refused, so no ownership transfers: outHandle stays cleared
// and outStopDeferred stays false. Nothing is ever audible.
TEST(ProgramRegistrationInstall, TimelineInstallBatchCeilingRefusesBeforeOwnership) {
    ProjectInput input = InstallWorld();
    const std::optional<SegmentProgram> program = CompileWithRouting(input);
    ASSERT_TRUE(program.has_value());
    harkenprogramreg::RegistrationParams params;
    params.resolvedVoiceSendSlots = 2;
    params.resolvedVoiceFxSlots = 1;
    params.callerDefaultOutputBus = harkentest::ObjectId(1);
    harken::LogicCommitCoordinator commit;
    harken::PlayableInstanceTable instances(2);
    harkenprogramreg::RegistrationResult result =
        Register(*program, params, commit, instances);
    ASSERT_TRUE(result.ok);
    ASSERT_NE(result.program, nullptr);

    VnConfig config = harkentest::MakeNullConfig(2, 256, 48000);
    config.maxControlCommandsPerQuantum = 2;
    harkentest::EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    const std::vector<float> pcm(48000, 0.25f);
    const VnPCMDesc pcmDesc = harkentest::MonoDesc(pcm, 48000);
    ASSERT_EQ(Vn_RegisterPCM(kSound, &pcmDesc), VN_OK);
    harken::Engine* engine = session.Engine();
    ASSERT_NE(engine, nullptr);
    ASSERT_EQ(result.program->InstallTimeline().status,
              harken::TimelineRuntimeInstallStatus::Installed);
    ASSERT_EQ(Vn_Update(), VN_OK);
    EXPECT_EQ(engine->timelineArms.LiveCount(), 0u);
    EXPECT_EQ(engine->timelinePool.LiveTransactionCount(), 0u);
    EXPECT_EQ(engine->timelineDeferred.Count(), 0u);
    EXPECT_EQ(engine->voices.ActiveCount(), 0u);
}

TEST(ProgramRegistrationInstall,
     ASaturatedQueueRefusesTheInstallAndStrandsNoVoice) {
    VnConfig config = InstallEngineConfig();
    config.commandQueueDepth = 8;
    config.fxSlotsPerVoice = 2;
    config.gainEffectPoolCapacity = 2;
    EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* const engine = session.Engine();
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(Vn_RegisterPCM(kSound, &desc), VN_OK);

    // The install's batch is four records — the play plus pan, width and the two
    // effects. Leave it one short. Distance scaling and the attenuation binding
    // publish nothing, so they are not in the count.
    while (engine->commands.FreeSlots() > 3u) {
        ASSERT_EQ(Vn_SetBusVolume(VN_BUS_MASTER, -1.0f, 0), VN_OK);
    }
    ASSERT_EQ(engine->commands.FreeSlots(), 3u);

    harkenprogramreg::VoiceStartInstall install =
        MasterInstall(0.0f, 1.0f, true);
    install.effectSlots = {GainStep(0, -6.0f), GainStep(1, -12.0f)};
    VnVoiceHandle handle = 0x1234;
    bool stopDeferred = true;
    const VnResult refused = harkenprogramreg::PlayVoiceStartInstall(
        install, &handle, &stopDeferred);
    EXPECT_NE(refused, VN_BACKPRESSURE)
        << "a mid-install caller must not be handed a retry contract";
    EXPECT_EQ(refused, VN_NO_VOICE);
    EXPECT_FALSE(stopDeferred);
    EXPECT_EQ(handle, 0u);
    EXPECT_EQ(engine->commands.FreeSlots(), 3u)
        << "a refused install published nothing";

    engine->nullBackend.Pump(1);
    EXPECT_EQ(Snapshot().activeVoiceCount, 0u);
    EXPECT_EQ(Snapshot().pendingStartCount, 0u);
    Capture capture;
    PumpAndCapture(engine, 2, capture);
    EXPECT_LE(Peak(capture.left), harkentest::kSilenceFloor)
        << "a refused install must leave nothing sounding";

    // The refusal returned everything it took: the same install succeeds once
    // the queue has drained, on a Gain pool with room for exactly these two.
    ASSERT_GE(engine->commands.FreeSlots(), 4u);
    VnVoiceHandle retried = 0;
    ASSERT_EQ(PlayInstall(install, &retried), VN_OK);
    ASSERT_NE(retried, 0u);
    Capture retriedCapture;
    PumpAndCapture(engine, 1, retriedCapture);
    HARKENTEST_ASSERT_NON_SILENT(retriedCapture.left);
}

// The rollback of a refused install is provably, exactly silent in the quantum
// it happens in — every sample bit-zero, not merely under a peak floor. That is
// what makes the rollback's zero-length stop correct where a committed voice's
// stop needs the timeline declick: a staged voice published nothing, so there is
// no rendered level for an immediate cut to step down from, and no ramp to run.
//
// The block boundary is what makes this a same-quantum statement. The drain
// budget is fixed at the top of the quantum, so the rollback issued here is
// resolved before the block below renders; a rollback that left a partly
// installed voice behind, or that cancelled a voice the audio side had already
// started, would put nonzero samples in exactly this block.
TEST(ProgramRegistrationInstall, RefusedInstallRollbackRendersExactZeroSamples) {
    VnConfig config = InstallEngineConfig();
    config.commandQueueDepth = 8;
    EngineSession session(config);
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* const engine = session.Engine();
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(Vn_RegisterPCM(kSound, &desc), VN_OK);

    // The run is the play plus the staged pan and width. Leave it one short, so
    // the commit refuses and the primitive rolls the staged voice back.
    while (engine->commands.FreeSlots() > 2u) {
        ASSERT_EQ(Vn_SetBusVolume(VN_BUS_MASTER, 0.0f, 0), VN_OK);
    }
    ASSERT_EQ(engine->commands.FreeSlots(), 2u);

    VnVoiceHandle handle = 0;
    bool stopDeferred = true;
    ASSERT_EQ(harkenprogramreg::PlayVoiceStartInstall(
                  MasterInstall(0.0f, 1.0f, false), &handle, &stopDeferred),
              VN_NO_VOICE);
    ASSERT_EQ(handle, 0u);
    ASSERT_FALSE(stopDeferred);

    Capture capture;
    PumpAndCapture(engine, 1, capture);
    ASSERT_FALSE(capture.left.empty());
    for (std::size_t i = 0; i < capture.left.size(); ++i) {
        EXPECT_EQ(capture.left[i], 0.0f) << "left sample " << i;
        EXPECT_EQ(capture.right[i], 0.0f) << "right sample " << i;
    }
    EXPECT_EQ(Snapshot().activeVoiceCount, 0u);
    EXPECT_EQ(Snapshot().pendingStartCount, 0u);
}

TEST(ProgramRegistrationInstall, DrainBudgetNeverExposesAnInstallPrefix) {
    EngineSession session(InstallEngineConfig());
    ASSERT_EQ(session.InitResult(), VN_OK);
    harken::Engine* const engine = session.Engine();
    const std::vector<float> dc = MakeDC(kRate, 0.5f);
    const VnPCMDesc desc = MonoDesc(dc, kRate);
    ASSERT_EQ(Vn_RegisterPCM(kSound, &desc), VN_OK);

    harkenprogramreg::VoiceStartInstall install =
        MasterInstall(0.0f, 1.0f, false);
    install.panning = 1.0f;
    engine->nullBackend.Pump(2);

    constexpr std::uint32_t kBatchSize = 3;
    for (std::uint32_t i = 0; i < kBatchSize - 1u; ++i) {
        ASSERT_EQ(Vn_SetBusVolume(VN_BUS_MASTER, -1.0f, 0), VN_OK);
    }
    VnVoiceHandle handle = 0;
    ASSERT_EQ(PlayInstall(install, &handle), VN_OK);
    const std::uint64_t batchSize = engine->commands.PublishedHead() -
                                    engine->commands.ConsumedCount() -
                                    (kBatchSize - 1u);
    ASSERT_EQ(kBatchSize, batchSize);
    engine->mixer.SetMaxCommandsPerQuantumForTest(2u * kBatchSize - 2u);

    Capture waiting;
    PumpAndCapture(engine, 1, waiting);
    EXPECT_LE(Peak(waiting.left), harkentest::kSilenceFloor);
    EXPECT_EQ(batchSize, engine->commands.PublishedHead() -
                             engine->commands.ConsumedCount());

    engine->mixer.SetMaxCommandsPerQuantumForTest(
        static_cast<std::uint32_t>(batchSize));
    Capture started;
    PumpAndCapture(engine, 1, started);
    HARKENTEST_ASSERT_NON_SILENT(started.right);
    EXPECT_GT(Peak(started.right), Peak(started.left));
}

}  // namespace
